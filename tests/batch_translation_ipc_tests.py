#!/usr/bin/env python3
"""EXT-02: production typed batch translation on the real geometry-derived M."""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import time

from analysis_check_ipc_tests import Client
from result_fixture_ipc_tests import create_m


def run(engine, cli, mode, evidence, without_nastran=False):
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="qcae-ext02-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        client = Client(engine, cli, str(root / "engine.sock"), mode)
        with (evidence / f"{mode}-engine.log").open("w+") as log:
            process = subprocess.Popen([engine, "--socket", client.endpoint,
                                        "--workspace", str(root / "work.sqlite")], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    assert process.poll() is None, "engine terminated during startup"
                    try:
                        capabilities = client.call("capabilities.list")
                        break
                    except (OSError, AssertionError, json.JSONDecodeError):
                        assert time.monotonic() < deadline, "engine startup timed out"
                        time.sleep(.05)
                operations = {item["name"]: item for item in capabilities["operations"]}
                for name in ("node.translate_batch", "node.translate_batch_preview", "node.move"):
                    assert operations[name]["available"], operations[name]
                assert operations["node.translate_batch"]["effect"] == "document_write"
                assert operations["node.translate_batch_preview"]["effect"] == "preview"
                profiles = capabilities["declared_solver_profiles"]
                if without_nastran:
                    assert profiles == [], profiles
                    profile = None
                else:
                    profile = profiles[0]["profile_ref"]
                nodes, beams, material, _, _ = create_m(client, profile)
                assert len(nodes) == 11 and len(beams) == 10
                assert len(client.entities("geometry")) == len(client.entities("mesh")) == 1
                for index, identity in enumerate(nodes):
                    assert client.fields(identity)["position"] == [index * 100, 0, 0]

                def rows():
                    return sorted(client.call("entity.query", context=client.current())["entities"],
                                  key=lambda row: row["entity_id"])

                def state():
                    current = client.current()
                    return current, client.call("history.list", context=current), rows()

                before = state()
                original = before[2]
                selected = {nodes[4], nodes[5]}
                parameters = {"node_ids": [nodes[4], nodes[5]],
                              "x": {"value": 0, "unit": "mm"},
                              "y": {"value": 1, "unit": "mm"},
                              "z": {"value": 0, "unit": "mm"}}
                cancelled = client.call("node.translate_batch_preview", parameters, before[0])
                assert cancelled["revision"] == before[0]["revision"]
                # Cancel disposes the intent: no changes.commit is sent for this token.
                assert state() == before, "preview/cancel changed model, revision or history"
                (evidence / f"{mode}-cancel.json").write_text(json.dumps({
                    "preview_id": cancelled["preview_id"], "intent_disposed": True,
                    "commit_sent": False, "model_history_revision_unchanged": True}, indent=2) + "\n")

                negatives = []
                missing_unit = copy.deepcopy(parameters)
                del missing_unit["y"]["unit"]
                negatives.append(("missing-unit", missing_unit, "needs_input", "MISSING_INPUT"))
                unknown = copy.deepcopy(parameters)
                unknown["node_ids"] = [nodes[4], "zzzz-missing-node"]
                negatives.append(("unknown-id", unknown, "failed", "ENTITY_NOT_FOUND"))
                wrong_entity = copy.deepcopy(parameters)
                wrong_entity["node_ids"] = [nodes[4], material]
                negatives.append(("wrong-entity-type", wrong_entity, "failed", "ENTITY_NOT_FOUND"))
                wrong_value = copy.deepcopy(parameters)
                wrong_value["y"]["value"] = True
                negatives.append(("wrong-value-type", wrong_value, "failed", "INVALID_INPUT"))
                wrong_ids = copy.deepcopy(parameters)
                wrong_ids["node_ids"] = nodes[4]
                negatives.append(("wrong-array-type", wrong_ids, "failed", "INVALID_INPUT"))
                duplicate = copy.deepcopy(parameters)
                duplicate["node_ids"] = [nodes[4], nodes[4]]
                negatives.append(("duplicate-id", duplicate, "failed", "INVALID_INPUT"))
                wrong_unit = copy.deepcopy(parameters)
                wrong_unit["y"]["unit"] = "N"
                negatives.append(("wrong-dimension", wrong_unit, "failed", "INVALID_UNIT"))
                for key, negative, status, code in negatives:
                    response = client.call("node.translate_batch", negative, before[0], key, expected=status)
                    assert response["error"]["code"] == code, response
                    assert state() == before, key
                # A finite JSON value whose conversion to mm overflows is also atomic.
                huge = copy.deepcopy(parameters)
                huge["y"] = {"value": 1.7976931348623157e308, "unit": "m"}
                client.call("node.translate_batch", huge, before[0], "overflow", expected="failed")
                assert state() == before

                changed_context = dict(before[0], revision=str(int(before[0]["revision"]) - 1))
                stale = client.call("node.translate_batch", parameters, changed_context, "stale", expected="conflict")
                assert stale["error"]["code"] == "REVISION_CONFLICT"
                expired = dict(before[0], document_epoch="expired-epoch")
                stale_epoch = client.call("node.translate_batch", parameters, expired, "expired", expected="conflict")
                assert stale_epoch["error"]["code"] == "DOCUMENT_EPOCH_EXPIRED"
                assert state() == before

                receipt = client.call("node.translate_batch", parameters, before[0], "batch")
                after = state()
                assert int(after[0]["revision"]) == int(before[0]["revision"]) + 1
                assert len(after[1]["items"]) == len(before[1]["items"]) + 1
                expected_rows = copy.deepcopy(original)
                for row in expected_rows:
                    if row["entity_id"] in selected:
                        assert row["kind"] == "node"
                        row["position_mm"][1] += 1
                assert after[2] == expected_rows, "batch changed records outside selected Nodes"
                translated = after[2]
                equivalent = copy.deepcopy(parameters)
                equivalent["node_ids"].reverse()
                equivalent["y"] = {"value": .001, "unit": "m"}
                retry = client.call("node.translate_batch", equivalent, before[0], "batch")
                assert retry["replayed"] and retry["transaction_id"] == receipt["transaction_id"]
                assert state() == after
                conflict = copy.deepcopy(parameters)
                conflict["y"]["value"] = 2
                rejection = client.call("node.translate_batch", conflict, before[0], "batch", expected="conflict")
                assert rejection["error"]["code"] == "IDEMPOTENCY_KEY_CONFLICT"
                assert state() == after
                client.call("history.undo", context=after[0], key="undo-batch")
                undone = state()
                assert undone[2] == original, "one undo did not restore every M record"
                retry = client.call("node.translate_batch", parameters, before[0], "batch")
                assert retry["replayed"] and retry["transaction_id"] == receipt["transaction_id"]
                assert state() == undone, "retry reapplied an undone batch"
                client.call("history.redo", context=undone[0], key="redo-batch")
                assert rows() == translated
                preview_context = client.current()
                applicable = client.call("node.translate_batch_preview", parameters, preview_context)
                client.call("changes.commit", {"preview_id": applicable["preview_id"]},
                            preview_context, "apply-batch-preview")
                twice = copy.deepcopy(translated)
                for row in twice:
                    if row["entity_id"] in selected:
                        row["position_mm"][1] += 1
                assert rows() == twice, "typed preview did not commit through existing changes.commit"
                client.call("history.undo", context=client.current(), key="undo-batch-preview")
                assert rows() == translated
                client.call("node.move", {"entity_id": nodes[4], "position_mm": [400, 2, 0]},
                            client.current(), "old-node-move")
                assert client.fields(nodes[4])["position"] == [400, 2, 0]
                client.call("history.undo", context=client.current(), key="undo-old-move")
                assert rows() == translated
                report = {"mode": mode, "passed": True, "nodes": len(nodes), "line2": len(beams),
                          "changed_ids": sorted(selected), "transactions_for_batch": 1,
                          "negative_rejections": len(negatives) + 3,
                          "preview_cancel_unchanged": True, "single_undo_redo_verified": True,
                          "preview_apply_verified": True,
                          "retry_after_undo_verified": True, "legacy_node_move_verified": True}
                (evidence / f"{mode}-M-original.json").write_text(json.dumps(original, indent=2) + "\n")
                (evidence / f"{mode}-M-translated.json").write_text(json.dumps(translated, indent=2) + "\n")
                (evidence / f"{mode}-report.json").write_text(json.dumps(report, indent=2) + "\n")
                print(json.dumps(report), flush=True)
                return report
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                (evidence / f"{mode}-transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--without-nastran", action="store_true")
    args = parser.parse_args()
    reports = [run(str(Path(args.engine).resolve()), str(Path(args.cli).resolve()), mode, args.evidence_dir,
                   args.without_nastran)
               for mode in ("cli", "script")]
    (args.evidence_dir / "report.json").write_text(json.dumps({"passed": 2, "total": 2, "runs": reports}, indent=2) + "\n")
    print("PASS: EXT-02 production registry/CLI/IPC batch translation 2/2")


if __name__ == "__main__":
    main()
