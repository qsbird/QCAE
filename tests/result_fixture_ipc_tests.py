#!/usr/bin/env python3
"""BP-19 / F22: publish M input, read R fixture and retain physical provenance."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

from analysis_check_ipc_tests import Client


def wait_task(client, task):
    deadline = time.monotonic() + 15
    while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
        assert time.monotonic() < deadline, task
        time.sleep(.01)
        task = client.call("task.status", {"task_id": task["task_id"]}, client.current())
    assert task["state"] == "succeeded", task
    return task


def create_m(client, profile=None):
    document = client.call("project.create", {"name": "M result fixture"}, key="create")
    line = client.call("geometry.create_line", {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]},
                       document, "line")
    wait_task(client, client.call("mesh.generate_line", {"geometry_id": line["entity_id"], "segments": 10},
                                  client.current(), "mesh"))
    nodes = sorted((item["entity_id"] for item in client.entities("node")),
                   key=lambda identity: client.fields(identity)["position"][0])
    beams = [item["entity_id"] for item in client.entities("beam")]
    assert len(nodes) == 11 and len(beams) == 10
    material = client.call("material.create", {"name": "Steel", "young_modulus": {"value": 210, "unit": "GPa"},
                                                "poisson_ratio": .3}, client.current(), "material")["entity_id"]
    section = client.call("section.create", {"name": "M section", "material_id": material, "area_mm2": 100,
                                               "i1_mm4": 833.333, "i2_mm4": 833.333, "torsion_mm4": 1400},
                          client.current(), "section")["entity_id"]
    client.call("beam.assign_section", {"beam_ids": beams, "section_id": section}, client.current(), "assign")
    part = client.edit("part.upsert", {"name": "Beam", "members": beams}, "part")["entity_id"]
    client.edit("assembly.upsert", {"name": "Assembly", "children": [part]}, "assembly")
    client.edit("set.upsert", {"name": "Fixed", "members": [nodes[0]]}, "fixed-set")
    client.edit("set.upsert", {"name": "Loaded", "members": [nodes[10]]}, "loaded-set")
    force = client.call("force.create", {"node_id": nodes[10], "x": {"value": 0, "unit": "N"},
                                           "y": {"value": -1, "unit": "N"}, "z": {"value": 0, "unit": "N"}},
                        client.current(), "force")["entity_id"]
    constraint = client.call("constraint.create", {"node_ids": [nodes[0]], "dofs": "123456"},
                             client.current(), "constraint")["entity_id"]
    case = client.call("load_case.create", {"name": "LC1", "force_ids": [force], "constraint_ids": [constraint]},
                       client.current(), "case")["entity_id"]
    if profile is None:
        return nodes, beams, material, part, None
    analysis = client.call("analysis.create", {"name": "M static", "load_case_ids": [case]},
                           client.current(), "analysis", profile=profile)["entity_id"]
    assert client.call("analysis.check", {"analysis_id": analysis}, client.current(), "check")["issues"] == []
    return nodes, beams, material, part, analysis


def bind_r(artifact, manifest, nodes):
    # Runtime IDs and export numbers are frozen by the actual publisher. The R
    # template retains only the specified synthetic values and required metadata.
    fixture = json.loads((Path(__file__).parent / "fixtures/results/R-displacement-v1.json").read_text())
    fixture["input_fingerprint"] = artifact["physical_signature_hex"]
    fixture["identities"] = manifest["export_id_map"]
    numbers = {item["entity_id"]: item["number"] for item in fixture["identities"] if item["namespace"] == "GRID"}
    fixture["values"][0]["solver_number"] = numbers[nodes[0]]
    fixture["values"][1]["solver_number"] = numbers[nodes[10]]
    signature = bytes.fromhex(fixture["input_fingerprint"])
    assert hashlib.sha256(signature).hexdigest() == manifest["input_sha256"] == artifact["input_sha256"]
    return fixture


def run_once(engine, cli, mode, evidence):
    with tempfile.TemporaryDirectory(prefix="qcae-result-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint, workspace = str(root / "engine.sock"), str(root / "work.sqlite")
        client = Client(engine, cli, endpoint, mode)
        process = None
        fixture, manifest = None, None
        with (root / "engine.log").open("w+") as log:
            def start():
                nonlocal process
                process = subprocess.Popen([engine, "--socket", endpoint, "--workspace", workspace],
                                           stdout=log, stderr=log)
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        log.seek(0)
                        raise AssertionError(log.read())
                    try:
                        return client.call("capabilities.list")
                    except (OSError, AssertionError, json.JSONDecodeError):
                        time.sleep(.05)
                raise AssertionError("Engine startup timed out")

            try:
                capabilities = start()
                assert capabilities["durable"] and capabilities["storage_mode"] == "sqlite"
                operations = {item["name"]: item for item in capabilities["operations"]}
                assert operations["results.read_fixture"]["available"]
                assert operations["results.read_fixture"]["effect"] == "auxiliary_write"
                assert operations["results.read_fixture"]["requires_revision"]
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                nodes, beams, material, part, analysis = create_m(client, profile)
                original = client.current()
                task = wait_task(client, client.call("model.export", {"analysis_id": analysis,
                                                                        "output_directory": str(root / "artifact")},
                                                     original, "publish", profile=profile))
                assert "receipt" not in task and task["artifact_receipt"]["input_revision"] == original["revision"]
                assert client.current()["revision"] == original["revision"]
                artifact_id = task["artifact_receipt"]["artifact_id"]
                artifact = client.call("artifact.get", {"artifact_id": artifact_id}, original)
                assert artifact["state"] == "published" and artifact["verified"]
                manifest_bytes = Path(artifact["manifest_path"]).read_bytes()
                assert hashlib.sha256(manifest_bytes).hexdigest() == task["artifact_receipt"]["manifest_sha256"]
                manifest = json.loads(manifest_bytes)
                assert manifest["complete"] and manifest["analysis_id"] == analysis
                assert manifest["revision"] == original["revision"]
                fixture = bind_r(artifact, manifest, nodes)
                parameters = {"artifact_id": artifact_id, "fixture_json": json.dumps(fixture)}
                history_count = len(client.call("history.list", context=original)["items"])
                result = client.call("results.read_fixture", parameters, original, "read-R")
                result_id = result["result_id"]
                assert result["source_kind"] == "fixture" and result["state"] == "current"
                assert result["input_fingerprint"] == fixture["input_fingerprint"]
                assert result["input_version"] == {key: original[key] for key in ("document_id", "document_epoch", "revision")}
                field = result["field"]
                assert {key: field[key] for key in ("quantity", "unit", "components", "location", "coordinate_basis", "case")} == {
                    key: fixture[key] for key in ("quantity", "unit", "components", "location", "coordinate_basis", "case")}
                assert field["frame"] == "0" and field["values"] == [
                    {"entity_id": nodes[0], "value": [0, 0, 0]}, {"entity_id": nodes[10], "value": [0, -1, 0]}]
                assert client.current()["revision"] == original["revision"]
                assert len(client.call("history.list", context=original)["items"]) == history_count
                assert client.call("results.read_fixture", parameters, original, "read-R") == result
                changed = copy.deepcopy(fixture)
                changed["values"][1]["value"][1] = -2
                conflict = client.call("results.read_fixture", {"artifact_id": artifact_id, "fixture_json": json.dumps(changed)},
                                       original, "read-R", expected="conflict")
                assert conflict["error"]["code"] == "IDEMPOTENCY_KEY_CONFLICT"
                client.call("results.read_fixture", {"artifact_id": "missing-input", "fixture_json": json.dumps(fixture)},
                            original, "missing-artifact", expected="failed")
                negatives = []
                bad = copy.deepcopy(fixture)
                bad["input_fingerprint"] = "00" + bad["input_fingerprint"][2:]
                negatives.append(bad)
                bad = copy.deepcopy(fixture)
                bad["location"] = "integration_point"
                negatives.append(bad)
                bad = copy.deepcopy(fixture)
                bad["identities"][0]["number"] = str(int(bad["identities"][0]["number"]) + 1000)
                negatives.append(bad)
                for repeat in range(10):
                    for index, negative in enumerate(negatives):
                        client.call("results.read_fixture", {"artifact_id": artifact_id, "fixture_json": json.dumps(negative)},
                                    original, f"negative-{repeat}-{index}", expected="failed")
                for key in fixture:
                    missing = copy.deepcopy(fixture)
                    del missing[key]
                    client.call("results.read_fixture", {"artifact_id": artifact_id, "fixture_json": json.dumps(missing)},
                                original, "missing-" + key, expected="failed")
                mislabeled = copy.deepcopy(fixture)
                mislabeled["source_kind"] = "external_solver"
                client.call("results.read_fixture", {"artifact_id": artifact_id, "fixture_json": json.dumps(mislabeled)},
                            original, "not-a-solver", expected="failed")
                assert client.current()["revision"] == original["revision"]
                assert client.call("results.get", {"result_id": result_id}, original) == result
                client.edit("part.upsert", {"entity_id": part, "name": "Renamed organization", "members": beams}, "rename-part")
                view = client.call("view.create", {"hidden_ids": [], "camera_fingerprint": "fixture"}, client.current())
                client.call("view.update", {"view_session_id": view["view_session_id"],
                                           "expected_view_revision": view["view_revision"],
                                           "hidden_ids": [nodes[0]], "camera_fingerprint": "fixture"}, client.current())
                assert client.call("results.get", {"result_id": result_id}, client.current())["state"] == "current"
                client.call("material.set_young_modulus", {"entity_id": material, "young_modulus": {"value": 200, "unit": "GPa"}},
                            client.current(), "change-physics")
                stale = client.call("results.get", {"result_id": result_id}, client.current())
                assert stale["state"] == "stale" and stale["input_version"] == result["input_version"]
                assert stale["field"] == field and stale["source_kind"] == "fixture"
                retry = client.call("results.read_fixture", parameters, original, "read-R")
                assert retry["state"] == "stale" and retry["field"] == field
                late = client.call("results.read_fixture", parameters, original, "new-stale-key", expected="conflict")
                assert late["error"]["code"] == "REVISION_CONFLICT"
                client.call("history.undo", context=client.current(), key="undo-physics")
                assert client.call("results.get", {"result_id": result_id}, client.current())["state"] == "current"
                client.call("history.redo", context=client.current(), key="redo-physics")
                saved = client.call("project.save", {"path": str(root / "M.qcae")}, client.current(), "save")
                process.kill()
                process.wait(timeout=10)
                assert start()["recovery_available"]
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                assert recovered["document_epoch"] != saved["document_epoch"]
                retained = client.call("results.get", {"result_id": result_id}, recovered)
                assert retained["state"] == "stale" and retained["field"] == field
                assert retained["input_version"] == result["input_version"] and retained["source_kind"] == "fixture"
                client.call("history.undo", context=recovered, key="recovered-undo")
                assert client.call("results.get", {"result_id": result_id}, client.current())["state"] == "current"
                client.call("history.redo", context=client.current(), key="recovered-redo")
                saved_as = client.call("project.save_as", {"path": str(root / "M-copy.qcae")}, client.current(), "save-as")
                client.call("project.close", {"policy": "discard"}, saved_as, "close")
                opened = client.call("project.open", {"mode": "normal", "path": str(root / "M.qcae")}, key="normal-open")
                retained = client.call("results.get", {"result_id": result_id}, opened)
                assert retained["state"] == "stale" and retained["field"] == field
                assert retained["input_version"] == result["input_version"]
                report = {"mode": mode, "passed": True, "source_kind": "fixture", "node_values": 2,
                          "negative_rejections": 30, "required_metadata_rejections": len(fixture),
                          "recovery_verified": True, "normal_open_verified": True,
                          "physical_stale_verified": True, "organization_display_current_verified": True,
                          "requests": len(client.transcript)}
                print(json.dumps(report), flush=True)
                return report
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                if evidence:
                    evidence.mkdir(parents=True, exist_ok=True)
                    (evidence / f"{mode}-result-transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")
                    if fixture:
                        (evidence / f"{mode}-R-bound.json").write_text(json.dumps(fixture, indent=2) + "\n")
                    if manifest:
                        (evidence / f"{mode}-input-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
                    log.flush()
                    log.seek(0)
                    (evidence / f"{mode}-result-engine.log").write_text(log.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    reports = [run_once(str(Path(args.engine).resolve()), str(Path(args.cli).resolve()), mode, args.evidence_dir)
               for mode in ("cli", "script")]
    if args.evidence_dir:
        (args.evidence_dir / "result-fixture-report.json").write_text(json.dumps({"passed": 2, "total": 2, "runs": reports}, indent=2) + "\n")
    print("PASS: BP-19 R fixture publication/provenance, F22 negatives and durable CLI/IPC 2/2")


if __name__ == "__main__":
    main()
