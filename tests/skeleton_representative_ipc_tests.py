#!/usr/bin/env python3
"""Specific BP assertions on the complete frozen M, through one durable engine."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

from analysis_check_ipc_tests import Client
from result_fixture_ipc_tests import create_m, wait_task


def run(args):
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    records = []
    invocation = uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix="qcae-sk-bp-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint = str(root / "engine.sock")
        client = Client(args.engine, args.cli, endpoint, "script")
        process = None
        with (root / "engine.log").open("w+") as log:
            def start():
                nonlocal process
                process = subprocess.Popen([args.engine, "--socket", endpoint, "--workspace", str(root / "work.sqlite")],
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

            def snapshot():
                rows = client.call("entity.query", context=client.current())["entities"]
                return sorted(rows, key=lambda row: row["entity_id"])

            def probe(case, inputs, action):
                if args.only_case and args.only_case != case:
                    return True
                begin = len(client.transcript)
                record = {"case_id": case, "run_id": "representative-" + invocation + '-' + case,
                          "source_tree_sha256": args.source_tree_sha256, "input": inputs,
                          "assertions": [], "actual": {}, "passed": False}

                def require(condition, expected, actual):
                    record["assertions"].append({"expected": expected, "actual": actual, "passed": bool(condition)})
                    assert condition, expected

                try:
                    action(require, record["actual"])
                    record["passed"] = True
                except Exception as error:
                    record["error"] = str(error)
                record["transcript"] = client.transcript[begin:]
                records.append(record)
                with (args.evidence_dir / "M-representatives.jsonl").open("a") as file:
                    file.write(json.dumps(record, ensure_ascii=False) + "\n")
                print(json.dumps({key: record[key] for key in ("case_id", "run_id", "passed")}), flush=True)
                return record["passed"]

            try:
                capabilities = start()
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                nodes, beams, material, part, analysis = create_m(client, profile)
                full_m = snapshot()
                (args.evidence_dir / "M-golden.json").write_text(json.dumps(full_m, indent=2) + "\n")
                (args.evidence_dir / "M-creation-transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")
                sets = client.entities("set")
                assembly = client.entities("assembly")[0]["entity_id"]

                def mesh(require, actual):
                    fields = {node: client.fields(node) for node in nodes}
                    beam_fields = {beam: client.fields(beam) for beam in beams}
                    ordered = sorted(beams, key=lambda beam: nodes.index(beam_fields[beam]["nodes"][0]))
                    require(len(nodes) == 11 and len(beams) == 10, "Real background discretizer generates exactly 11 nodes and 10 beams", [len(nodes), len(beams)])
                    for index, node in enumerate(nodes):
                        require(fields[node]["position"] == [index * 100, 0, 0], "Node coordinate matches the M golden", {"node": node, "position": fields[node]["position"]})
                    for index, beam in enumerate(ordered):
                        require(beam_fields[beam]["nodes"] == nodes[index:index + 2], "Beam connection matches both exact original node IDs", {"beam": beam, "nodes": beam_fields[beam]["nodes"]})
                    require(client.fields(material)["young_modulus_mpa"] == 210000 and client.fields(material)["poisson_ratio"] == .3,
                            "Complete M has the frozen steel properties", client.fields(material))
                    require(len(client.entities("load_case")) == 1 and len(client.entities("analysis")) == 1,
                            "Complete M includes one LoadCase and one Analysis", {"analysis": analysis})
                    actual["M"] = full_m
                    actual["background_task"] = [item for item in client.transcript if item["request"]["operation"] in {"mesh.generate_line", "task.status"}]

                probe("BP-05", "Empty document → actual 10-segment line meshing → complete M", mesh)

                def regenerate(require, actual):
                    before = snapshot()
                    require(before == full_m and len(before) == 33,
                            "Geometry replacement starts from complete unchanged M", before)
                    geometry = client.entities("geometry")[0]["entity_id"]
                    mesh_id = client.entities("mesh")[0]["entity_id"]
                    original_beams = {beam: client.fields(beam) for beam in beams}
                    moved = client.call("geometry.move_endpoint", {
                        "geometry_id": geometry, "end_mm": [1200, 0, 0]},
                        client.current(), "BP04-endpoint")
                    stale = snapshot()
                    require(client.fields(mesh_id)["stale"] is True,
                            "Moving the endpoint makes the old generated binding stale",
                            client.fields(mesh_id))
                    base = client.current()
                    completed = wait_task(client, client.call("mesh.regenerate_line", {
                        "mesh_id": mesh_id, "segments": 10,
                        "replacement_policy": "reject_unmapped"}, base, "BP04-regenerate"))
                    regenerated = snapshot()
                    binding = client.fields(mesh_id)
                    require(binding["stale"] is False and
                            binding["geometry_revision"] == client.fields(geometry)["geometry_revision"],
                            "Successful regeneration installs the actual new geometry revision", binding)
                    require(int(client.current()["revision"]) == int(base["revision"]) + 1,
                            "Regeneration commits exactly one model transaction", completed)
                    require({row["entity_id"] for row in client.entities("node")} == set(nodes) and
                            {row["entity_id"] for row in client.entities("beam")} == set(beams),
                            "Same-count regeneration preserves all M node and beam identities", regenerated)
                    for index, node in enumerate(nodes):
                        require(client.fields(node)["position"] == [index * 120, 0, 0],
                                "Regenerated M has exactly 120 mm spacing", client.fields(node))
                    require({beam: client.fields(beam) for beam in beams} == original_beams,
                            "Regeneration preserves complete beam connections, sections and ownership", original_beams)
                    unchanged_kinds = {"material", "section", "part", "assembly", "set", "force", "constraint", "load_case", "analysis"}
                    require([row for row in regenerated if row["kind"] in unchanged_kinds] ==
                            [row for row in before if row["kind"] in unchanged_kinds],
                            "Existing physics and organization references retain their exact M rows", True)
                    before_reject = client.current()
                    history = client.call("history.list", context=before_reject)
                    rejected = client.call("mesh.regenerate_line", {
                        "mesh_id": mesh_id, "segments": 11,
                        "replacement_policy": "reject_unmapped"}, before_reject, "BP04-unmapped")
                    deadline = time.monotonic() + 15
                    while rejected["state"] in {"queued", "running", "committing"}:
                        require(time.monotonic() < deadline, "Unmapped replacement reaches a terminal state", rejected)
                        time.sleep(.01)
                        rejected = client.call("task.status", {"task_id": rejected["task_id"]}, client.current())
                    require(rejected["state"] == "failed" and "receipt" not in rejected,
                            "Unmapped references reject the whole replacement without a receipt", rejected)
                    require(snapshot() == regenerated and client.current() == before_reject and
                            client.call("history.list", context=before_reject) == history,
                            "Failed replacement changes no model row, reference, revision or history", True)
                    client.call("history.undo", context=client.current(), key="BP04-undo-regeneration")
                    require(snapshot() == stale, "Undo restores the complete stale pre-regeneration image", snapshot())
                    client.call("history.undo", context=client.current(), key="BP04-undo-endpoint")
                    require(snapshot() == before, "Second undo restores the complete original M", snapshot())
                    actual.update(M_before=before, endpoint_receipt=moved,
                                  regenerated_M=regenerated, regeneration_task=completed,
                                  rejected_task=rejected, M_after_undo=snapshot())

                probe("BP-04", "Complete M endpoint 1000→1200 mm; same-count regeneration; reject unmapped 11-segment replacement atomically", regenerate)

                def duplicate_import(require, actual):
                    before_document = client.current()
                    before_model = snapshot()
                    before_history = client.call("history.list", context=before_document)
                    require(len(before_model) == 33 and before_model == full_m,
                            "Duplicate-number rejection is tested on the complete unmodified M", before_model)
                    exported = wait_task(client, client.call("model.export", {
                        "analysis_id": analysis, "output_directory": str(root / "BP15-M-artifact")},
                        before_document, "BP15-publish-M", profile=profile))
                    artifact = client.call("artifact.get", {"artifact_id": exported["artifact_receipt"]["artifact_id"]},
                                           before_document)
                    manifest_path = Path(artifact["manifest_path"])
                    manifest_bytes = manifest_path.read_bytes()
                    manifest = json.loads(manifest_bytes)
                    require(manifest["complete"] and artifact["verified"] and
                            hashlib.sha256(manifest_bytes).hexdigest() == exported["artifact_receipt"]["manifest_sha256"],
                            "M resources originate from an actual independently verified published artifact", exported)
                    resources = []
                    for item in manifest["files"]:
                        data = (manifest_path.parent / item["path"]).read_bytes()
                        require(hashlib.sha256(data).hexdigest() == item["sha256"] and len(data) == int(item["byte_length"]),
                                "Original M import resource matches its published digest and length", item)
                        resources.append({"path": item["path"], "text": data.decode("utf-8")})
                    parameters = {"command": "model.import", "root_resource": manifest["root_resource"],
                                  "resources": resources, "source_profile_ref": profile, "unit_system": "mm-N-MPa"}
                    valid = client.call("changes.preview", parameters, before_document, expected="conflict")
                    require(valid["error"]["code"] == "INVALID_INPUT" and "empty model" in valid["error"]["message"],
                            "Valid model.import explicitly requires an empty model; this current M cannot be overwritten", valid)
                    original_number = next(item["number"] for item in manifest["export_id_map"]
                                           if item["namespace"] == "GRID" and item["entity_id"] == nodes[0])
                    mutated = copy.deepcopy(resources)
                    root_resource = next(item for item in mutated if item["path"] == manifest["root_resource"])
                    require("ENDDATA" in root_resource["text"], "Actual exported root deck has a concrete injection position", root_resource)
                    root_resource["text"] = root_resource["text"].replace(
                        "ENDDATA", "INCLUDE 'BP15-duplicate-grid.bdf'\nENDDATA", 1)
                    duplicate = {"path": "BP15-duplicate-grid.bdf", "text": f"GRID,{original_number},,9,9,9\n"}
                    mutated.append(duplicate)
                    rejected = client.call("changes.preview", {**parameters, "resources": mutated},
                                           before_document, expected="failed")
                    issues = rejected.get("report", {}).get("issues", [])
                    require(rejected["error"]["code"] == "IMPORT_REJECTED" and
                            any(issue["code"] == "duplicate_number" for issue in issues),
                            "Actual public IPC import rejects the injected repeated GRID solver number before the empty-model gate", rejected)
                    after_document = client.current()
                    after_model = snapshot()
                    after_history = client.call("history.list", context=after_document)
                    require(after_model == before_model, "Duplicate-number rejection preserves every original M field, ID, connection and reference", after_model)
                    require(after_history == before_history, "Duplicate-number rejection preserves complete transaction IDs, cursor, labels and applied states", after_history)
                    require(after_document == before_document, "Duplicate-number rejection preserves document authority, epoch, revision and dirty state", after_document)
                    actual.update(M_before=before_model, M_after=after_model, history_before=before_history,
                                  history_after=after_history, document_before=before_document, document_after=after_document,
                                  duplicate_number=original_number, valid_import_boundary=valid, rejected=rejected,
                                  original_resources=resources, mutated_resources=mutated, manifest=manifest,
                                  actual_export_task=exported, model_transactions_added=0)
                    (args.evidence_dir / "BP15-original-resources.json").write_text(json.dumps(resources, indent=2) + "\n")
                    (args.evidence_dir / "BP15-duplicate-resources.json").write_text(json.dumps(mutated, indent=2) + "\n")

                probe("BP-15", "Complete M remains active; actual published M deck gains a duplicate GRID in a new INCLUDE; public model.import rejects", duplicate_import)

                def organization(require, actual):
                    original_nodes = set(nodes)
                    before = snapshot()
                    for view, owner in (("part", part), ("assembly", assembly)):
                        rows = client.call("entity.query", {"view": view, "owner_id": owner, "kind": "node"}, client.current())["entities"]
                        identities = {row["entity_id"] for row in rows}
                        require(identities == original_nodes, view + " query returns the original 11 node IDs without copies", sorted(identities))
                    fixed = next(row for row in sets if row["name"] == "Fixed")
                    loaded = next(row for row in sets if row["name"] == "Loaded")
                    for owner, golden in ((fixed["entity_id"], nodes[0]), (loaded["entity_id"], nodes[10])):
                        rows = client.call("entity.query", {"view": "set", "owner_id": owner}, client.current())["entities"]
                        require({row["entity_id"] for row in rows} == {golden}, "Frozen endpoint set resolves the exact original node", rows)
                    client.edit("set.upsert", {"entity_id": loaded["entity_id"], "name": "Loaded", "members": [nodes[9], nodes[10]]}, "BP08-edit-set")
                    require(client.fields(loaded["entity_id"])["members"] == [nodes[9], nodes[10]], "Set modification is committed through the coordinator", client.fields(loaded["entity_id"]))
                    client.call("history.undo", context=client.current(), key="BP08-undo-set")
                    require(snapshot() == before and set(row["entity_id"] for row in client.entities("node")) == original_nodes,
                            "Undo restores complete M and no organization operation creates copied nodes", snapshot())
                    actual["M_before"] = before
                    actual["M_after_undo"] = snapshot()

                probe("BP-08", "M part/assembly/Fixed and Loaded sets; modify Loaded then undo", organization)

                def hundred_rounds(require, actual):
                    before = snapshot()
                    revision = int(client.current()["revision"])
                    movement = client.call("node.move", {"entity_id": nodes[5], "position_mm": [500, 1, 0]}, client.current(), "BP12-move-node5")
                    moved = snapshot()
                    changes = [(left, right) for left, right in zip(before, moved) if left != right]
                    require(len(changes) == 1 and changes[0][1]["entity_id"] == nodes[5], "The initial edit changes only node 5", changes)
                    for round_number in range(1, 101):
                        client.call("history.undo", context=client.current(), key=f"BP12-undo-{round_number}")
                        require(snapshot() == before, f"Undo round {round_number} equals complete pre-move M including connectivity, properties, organization and analysis", True)
                        client.call("history.redo", context=client.current(), key=f"BP12-redo-{round_number}")
                        require(snapshot() == moved, f"Redo round {round_number} equals complete first-move M", True)
                    delta = int(client.current()["revision"]) - revision
                    require(delta == 201, "One edit and 100 undo/redo rounds advance revision by exactly 201", delta)
                    actual.update(M_before=before, M_after=moved, initial_receipt=movement, rounds=100, revision_delta=delta)

                probe("BP-12", "Complete M; node5+=(0,1,0)mm; 100 undo/redo rounds", hundred_rounds)

                def lifecycle(require, actual):
                    golden = snapshot()
                    original = client.current()
                    saved = client.call("project.save", {"path": str(root / "M.qcae")}, original, "BP01-save")
                    client.call("project.close", {"policy": "discard"}, saved, "BP01-close")
                    opened = client.call("project.open", {"mode": "normal", "path": str(root / "M.qcae")}, key="BP01-normal-open")
                    require(opened["document_id"] != original["document_id"] and opened["revision"] == "0",
                            "Normal open uses a new DocumentId and a fresh history revision", {"original": original, "opened": opened})
                    require(snapshot() == golden, "Normal open retains every M identity, coordinate, connection and reference", snapshot())
                    process.kill()
                    process.wait(timeout=10)
                    require(start()["recovery_available"], "Real process termination exposes explicit recovery", True)
                    recovered = client.call("project.open", {"mode": "recover"}, key="BP01-recover")
                    require(recovered["document_id"] == opened["document_id"] and recovered["document_epoch"] != opened["document_epoch"],
                            "Recovery retains DocumentId and renews Epoch", {"opened": opened, "recovered": recovered})
                    require(snapshot() == golden, "Recovery restores complete M without identity or reference loss", snapshot())
                    actual.update(original=original, normal_open=opened, recovery=recovered, M=golden)

                probe("BP-01", "Create complete M → save/close → normal open → kill process → explicit recover", lifecycle)
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                (args.evidence_dir / "M-transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")
                log.flush()
                log.seek(0)
                (args.evidence_dir / "M-engine.log").write_text(log.read())
    report = {"source_tree_sha256": args.source_tree_sha256, "cases": len(records),
              "passed": sum(record["passed"] for record in records), "records": records}
    (args.evidence_dir / "M-representatives-report.json").write_text(json.dumps(report, indent=2) + "\n")
    expected_cases = 1 if args.only_case else 6
    return 0 if len(records) == expected_cases and all(record["passed"] for record in records) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--source-tree-sha256", default="mutable-diagnostic")
    parser.add_argument("--only-case", choices=("BP-01", "BP-04", "BP-05", "BP-08", "BP-12", "BP-15"),
                        help="Run a single actual representative probe on a fresh complete M")
    args = parser.parse_args()
    args.engine, args.cli = str(Path(args.engine).resolve()), str(Path(args.cli).resolve())
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    (args.evidence_dir / "M-representatives.jsonl").write_text("")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
