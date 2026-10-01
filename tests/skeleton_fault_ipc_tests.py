#!/usr/bin/env python3
"""Ten actual F05 wire-loss retries and ten F22 probes with all R negative mutations."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import socket
import tempfile
import time
import uuid

from analysis_check_ipc_tests import Client
from result_fixture_ipc_tests import bind_r, create_m, wait_task


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def run(args):
    evidence = args.evidence_dir
    evidence.mkdir(parents=True, exist_ok=True)
    records = []
    invocation = uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix="qcae-sk-f22-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint = str(root / "engine.sock")
        client = Client(args.engine, args.cli, endpoint, "script")
        process = None
        with (root / "engine.log").open("w+") as log:
            try:
                process = subprocess.Popen([args.engine, "--socket", endpoint, "--workspace", str(root / "work.sqlite")],
                                           stdout=log, stderr=log)
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        log.seek(0)
                        raise AssertionError(log.read())
                    try:
                        capabilities = client.call("capabilities.list")
                        break
                    except (OSError, AssertionError, json.JSONDecodeError):
                        time.sleep(.05)
                else:
                    raise AssertionError("Engine startup timed out")
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                # F05 injects real wire loss: the writing connection never reads its
                # success response. An independent observer sees the one committed
                # fact before that connection is closed and the original intent retried.
                client.call("project.create", {"name": "Real response loss"}, key="F05-create")
                material = client.call("material.create", {"name": "Steel", "young_modulus": {"value": 210, "unit": "GPa"},
                    "poisson_ratio": .3}, client.current(), "F05-seed-material")["entity_id"]
                for repeat in range(1, 11):
                    start = len(client.transcript)
                    record = {"case_id": "F05", "run_id": f"real-ipc-{invocation}-F05-{repeat}",
                              "source_tree_sha256": args.source_tree_sha256, "input": {},
                              "assertions": [], "actual": {}, "passed": False}

                    def require_loss(condition, expectation, actual):
                        record["assertions"].append({"expected": expectation, "actual": actual, "passed": bool(condition)})
                        assert condition, expectation

                    try:
                        before = client.current()
                        before_history = client.call("history.list", context=before)
                        parameters = {"entity_id": material, "young_modulus": {"value": 70000 + repeat, "unit": "MPa"}}
                        key = f"F05-lost-response-{repeat}"
                        request = {"api_version": "1.1", "request_id": f"F05-wire-{repeat}",
                                   "operation": "material.set_young_modulus", "parameters": parameters,
                                   "idempotency_key": key, "document_id": before["document_id"],
                                   "document_epoch": before["document_epoch"], "expected_revision": before["revision"]}
                        record["input"] = request
                        with socket.socket(socket.AF_UNIX) as lost_connection:
                            lost_connection.settimeout(15)
                            lost_connection.connect(endpoint)
                            with lost_connection.makefile("rwb") as stream:
                                handshake = {"api_version": "1.1", "request_id": "F05-handshake", "operation": "runtime.handshake"}
                                stream.write(json.dumps(handshake).encode() + b"\n")
                                stream.flush()
                                assert json.loads(stream.readline())["status"] == "success"
                                stream.write(json.dumps(request).encode() + b"\n")
                                stream.flush()
                                deadline = time.monotonic() + 15
                                while True:
                                    committed = client.current()
                                    if int(committed["revision"]) == int(before["revision"]) + 1:
                                        break
                                    assert time.monotonic() < deadline, "Original wire request did not commit"
                                    time.sleep(.01)
                                # Deliberately do not call read/recv/readline for the
                                # successful request. Closing this connection loses its reply.
                        original_history = client.call("history.list", context=committed)
                        original_model = client.call("entity.query", context=committed)
                        original_transaction = original_history["items"][-1]["transaction_id"]
                        require_loss(len(original_history["items"]) == len(before_history["items"]) + 1,
                                     "Lost-response request is observed as exactly one committed transaction", original_history)
                        retry = client.call("material.set_young_modulus", parameters, before, key)
                        after = client.current()
                        after_history = client.call("history.list", context=after)
                        after_model = client.call("entity.query", context=after)
                        require_loss(retry["replayed"] and retry["transaction_id"] == original_transaction,
                                     "Same original key/context/parameters returns the observed original transaction fact", retry)
                        require_loss(after == committed and after_history == original_history and after_model == original_model,
                                     "Actual response-loss retry produces zero extra revision, history or model changes",
                                     {"before_retry": committed, "after_retry": after, "history": after_history})
                        record["actual"] = {"successful_write_response_read": False, "writing_connection_closed": True,
                            "original_transaction": original_transaction, "retry": retry,
                            "document_before_request": before, "document_after_commit": committed, "document_after_retry": after,
                            "model_after_commit": original_model, "model_after_retry": after_model,
                            "history_after_commit": original_history, "history_after_retry": after_history}
                        record["passed"] = True
                    except Exception as error:
                        record["error"] = str(error)
                    record["transcript"] = client.transcript[start:]
                    records.append(record)
                    with (evidence / "result-faults.jsonl").open("a") as file:
                        file.write(json.dumps(record, ensure_ascii=False) + "\n")
                    print(json.dumps({key: record[key] for key in ("case_id", "run_id", "passed")}), flush=True)
                client.call("project.close", {"policy": "discard"}, client.current(), "F05-close")
                nodes, _, _, _, analysis = create_m(client, profile)
                context = client.current()
                task = wait_task(client, client.call("model.export", {"analysis_id": analysis,
                    "output_directory": str(root / "artifact")}, context, "input-export", profile=profile))
                artifact_id = task["artifact_receipt"]["artifact_id"]
                artifact = client.call("artifact.get", {"artifact_id": artifact_id}, context)
                manifest = json.loads(Path(artifact["manifest_path"]).read_text())
                fixture = bind_r(artifact, manifest, nodes)
                params = {"artifact_id": artifact_id, "fixture_json": json.dumps(fixture)}
                valid = client.call("results.read_fixture", params, context, "valid-R")
                dump(evidence / "R-bound.json", fixture)
                dump(evidence / "input-manifest.json", manifest)
                for repeat in range(1, 11):
                    start = len(client.transcript)
                    record = {"case_id": "F22", "run_id": f"real-ipc-{invocation}-F22-{repeat}",
                              "source_tree_sha256": args.source_tree_sha256, "input": [],
                              "assertions": [], "actual": {}, "passed": False}

                    def require(condition, expectation, actual):
                        record["assertions"].append({"expected": expectation, "actual": actual,
                                                     "passed": bool(condition)})
                        assert condition, expectation

                    try:
                        before = client.current()
                        history = client.call("history.list", context=before)
                        result = client.call("results.get", {"result_id": valid["result_id"]}, before)
                        negatives = []
                        bad = copy.deepcopy(fixture)
                        bad["input_fingerprint"] = ("01" if bad["input_fingerprint"][:2] == "00" else "00") + bad["input_fingerprint"][2:]
                        negatives.append(("wrong_input_fingerprint", bad))
                        bad = copy.deepcopy(fixture)
                        bad["location"] = "integration_point"
                        negatives.append(("wrong_location", bad))
                        bad = copy.deepcopy(fixture)
                        bad["identities"][0]["number"] = str(int(bad["identities"][0]["number"]) + 1000)
                        negatives.append(("wrong_number_mapping", bad))
                        bad = copy.deepcopy(fixture)
                        bad["source_kind"] = "external_solver"
                        negatives.append(("fixture_mislabeled_solver", bad))
                        replies = []
                        for label, negative in negatives:
                            record["input"].append({"mutation": label, "fixture": negative})
                            response = client.call("results.read_fixture", {"artifact_id": artifact_id,
                                "fixture_json": json.dumps(negative)}, before, f"F22-{repeat}-{label}", expected="failed")
                            replies.append(response)
                            require(response["status"] == "failed" and "result_id" not in response.get("data", {}),
                                    label + " cannot be returned as the current displacement result", response)
                        after = client.current()
                        after_history = client.call("history.list", context=after)
                        retained = client.call("results.get", {"result_id": valid["result_id"]}, after)
                        require(after == before and after_history == history,
                                "All negative reads preserve document revision and full transaction history",
                                {"before": before, "after": after, "history_before": history, "history_after": after_history})
                        require(retained == result and retained["source_kind"] == "fixture" and retained["state"] == "current",
                                "Only the valid R field remains current, with fixture provenance retained", retained)
                        record["actual"] = {"responses": replies, "retained_result": retained,
                            "document_before": before, "document_after": after,
                            "history_before": history, "history_after": after_history}
                        record["passed"] = True
                    except Exception as error:
                        record["error"] = str(error)
                    record["transcript"] = client.transcript[start:]
                    records.append(record)
                    with (evidence / "result-faults.jsonl").open("a") as file:
                        file.write(json.dumps(record, ensure_ascii=False) + "\n")
                    print(json.dumps({key: record[key] for key in ("case_id", "run_id", "passed")}), flush=True)
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                dump(evidence / "result-transcript.json", client.transcript)
                log.flush()
                log.seek(0)
                (evidence / "result-engine.log").write_text(log.read())
    report = {"source_tree_sha256": args.source_tree_sha256, "runs": len(records),
              "passed": sum(item["passed"] for item in records), "expected_runs": 20,
              "input_sha256": hashlib.sha256((evidence / "R-bound.json").read_bytes()).hexdigest()}
    dump(evidence / "result-faults-report.json", report)
    return 0 if report["passed"] == report["expected_runs"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--source-tree-sha256", default="mutable-diagnostic")
    args = parser.parse_args()
    args.engine, args.cli = str(Path(args.engine).resolve()), str(Path(args.cli).resolve())
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    (args.evidence_dir / "result-faults.jsonl").write_text("")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
