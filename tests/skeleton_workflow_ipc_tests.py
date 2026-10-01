#!/usr/bin/env python3
"""Three complete pure-CLI workflows from empty documents, with explicit ID bijections."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

from analysis_check_ipc_tests import Client
from result_fixture_ipc_tests import bind_r, create_m, wait_task


def write(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def identities_for_m(rows):
    mapping = {}
    grouped = {}
    for row in rows:
        grouped.setdefault(row["kind"], []).append(row)
    nodes = sorted(grouped["node"], key=lambda row: row["position_mm"])
    for index, row in enumerate(nodes):
        mapping[row["entity_id"]] = f"node:{index:02}"
    beams = sorted(grouped["beam"], key=lambda row: (mapping[row["nodes"][0]], mapping[row["nodes"][1]]))
    for index, row in enumerate(beams):
        mapping[row["entity_id"]] = f"beam:{index:02}"
    for kind, members in grouped.items():
        if kind in {"node", "beam"}:
            continue
        # Names are preserved as model semantics. They only choose the identity
        # bijection for organization records; no field/reference is discarded.
        ordered = sorted(members, key=lambda row: (row.get("name", ""), row["entity_id"]))
        for index, row in enumerate(ordered):
            mapping[row["entity_id"]] = f"{kind}:{index:02}"
    assert len(mapping) == len(rows) and len(set(mapping.values())) == len(rows), "ID map is not bijective"
    return mapping


def canonical(value, mapping):
    if isinstance(value, str):
        return mapping.get(value, value)
    if isinstance(value, list):
        return [canonical(item, mapping) for item in value]
    if isinstance(value, dict):
        return {key: canonical(item, mapping) for key, item in value.items()}
    return value


def run_once(args, number):
    evidence = args.evidence_dir / f"cli-{number}"
    evidence.mkdir(parents=True, exist_ok=True)
    record = {"mode": "cli", "run_id": f"cli-workflow-{uuid.uuid4().hex}-{number}",
              "source_tree_sha256": args.source_tree_sha256, "passed": False,
              "assertions": [], "semantic_checkpoints": {}, "raw_semantic_checkpoints": {}, "real_solver": False,
              "real_ai_client": False, "result_source_kind": "fixture"}

    def require(condition, expected, actual):
        record["assertions"].append({"expected": expected, "actual": actual, "passed": bool(condition)})
        assert condition, expected

    with tempfile.TemporaryDirectory(prefix="qcae-full-cli-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        client = Client(args.engine, args.cli, str(root / "engine.sock"), "cli")
        process = None
        with (root / "engine.log").open("w+") as log:
            def start():
                nonlocal process
                process = subprocess.Popen([args.engine, "--socket", client.endpoint,
                                            "--workspace", str(root / "work.sqlite")], stdout=log, stderr=log)
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

            def rows():
                return sorted(client.call("entity.query", context=client.current())["entities"], key=lambda item: item["entity_id"])

            def normalized():
                return sorted((canonical(row, mapping) for row in rows()), key=lambda item: item["entity_id"])

            def checkpoint(name):
                actual = rows()
                record["raw_semantic_checkpoints"][name] = actual
                record["semantic_checkpoints"][name] = sorted(
                    (canonical(row, mapping) for row in actual), key=lambda item: item["entity_id"])
                return record["semantic_checkpoints"][name]

            def current_view():
                return client.call("view.create", {"hidden_ids": [], "camera_fingerprint": "workflow-through-query"}, client.current())

            try:
                capabilities = start()
                require(capabilities["durable"] and not capabilities["recovery_available"],
                        "Workflow starts in a fresh explicit SQLite workspace with no retained document", capabilities)
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                nodes, beams, material, part, analysis = create_m(client, profile)
                # create_m itself creates the empty document and records every actual
                # geometry, worker, physics and organization transaction in the transcript.
                raw_m = rows()
                mapping = identities_for_m(raw_m)
                record["identity_bijection"] = mapping
                write(evidence / "M-identities.json", mapping)
                write(evidence / "M-original.json", raw_m)
                original_m = checkpoint("M_baseline")
                require(len(nodes) == 11 and len(beams) == 10 and len(raw_m) == 33,
                        "Complete M has the fixed 11-node/10-beam physics, organization, LoadCase and Analysis", {"nodes": len(nodes), "beams": len(beams), "records": len(raw_m)})
                geometry = client.entities("geometry")[0]["entity_id"]
                midpoint = client.call("geometry.evaluate_line", {"geometry_id": geometry, "u": .5}, client.current())
                require(midpoint["position_mm"] == [500, 0, 0], "Production geometry evaluates the M line midpoint", midpoint)
                view = current_view()
                selection = client.call("selection.evaluate", {"view_session_id": view["view_session_id"],
                    "expected_view_revision": view["view_revision"], "predicate": {"op": "all"},
                    "scope": {"visibility": "through", "candidate_ids": [nodes[5]], "include_hidden": False}}, client.current())
                selected = client.call("selection.get", {"selection_handle": selection["selection_handle"]}, client.current())
                require(selected["entity_ids"] == [nodes[5]], "CLI selection resolves exactly original node5 in the single authority", selected)
                before_preview = client.current()
                before_history = client.call("history.list", context=before_preview)
                parameters = {"command": "node.move", "entity_id": nodes[5], "position_mm": [500, 1, 0]}
                cancelled = client.call("changes.preview", parameters, before_preview)
                # CLI cancellation is the explicit disposal of the preview intent. It
                # follows the same contract as GUI cancel: no changes.commit is sent.
                record["cancelled_preview"] = {"preview_id": cancelled["preview_id"], "intent_disposed": True, "commit_sent": False}
                require(client.current() == before_preview and client.call("history.list", context=before_preview) == before_history and normalized() == original_m,
                        "Cancelled preview has zero model/history/revision changes", cancelled)
                applicable = client.call("changes.preview", parameters, before_preview)
                applied = client.call("changes.commit", {"preview_id": applicable["preview_id"]}, before_preview, "apply-selected-preview")
                require(int(client.current()["revision"]) == int(before_preview["revision"]) + 1 and client.fields(nodes[5])["position"] == [500, 1, 0],
                        "Applying the selected preview creates exactly one transaction", applied)
                checkpoint("selection_applied")
                client.call("history.undo", context=client.current(), key="undo-selection")
                require(checkpoint("selection_undone") == original_m, "Undo restores complete M after preview apply", True)
                client.call("history.redo", context=client.current(), key="redo-selection")
                require(client.fields(nodes[5])["position"] == [500, 1, 0], "Redo restores the selected node edit", client.fields(nodes[5]))
                client.call("history.undo", context=client.current(), key="restore-frozen-M")
                require(normalized() == original_m, "Workflow restores the frozen M before engineering checks", True)

                load_case = client.entities("load_case")[0]["entity_id"]
                case_fields = client.fields(load_case)
                client.call("load_case.set_references", {"load_case_id": load_case, "force_ids": [],
                    "constraint_ids": case_fields["constraints"]}, client.current(), "remove-load-for-check")
                checked = client.call("analysis.check", {"analysis_id": analysis}, client.current(), "check-missing-load", expected="needs_input")["data"]
                require(len(checked["issues"]) == 1 and checked["issues"][0]["rule_id"] == "missing-load" and checked["issues"][0]["entity_id"] == analysis,
                        "Check produces exactly the specified missing-load Issue on M Analysis", checked)
                located = client.call("entity.query", {"ids": [checked["issues"][0]["entity_id"]]}, client.current())
                require(len(located["entities"]) == 1 and located["entities"][0]["entity_id"] == analysis,
                        "Issue identity locates the authoritative Analysis entity", located)
                client.call("load_case.set_references", {"load_case_id": load_case, "force_ids": case_fields["forces"],
                    "constraint_ids": case_fields["constraints"]}, client.current(), "repair-load-reference")
                repaired = client.call("analysis.check", {"analysis_id": analysis}, client.current(), "recheck-repaired")
                old_issue = client.call("analysis.get_issues", {"check_id": checked["check_id"]}, client.current())
                require(repaired["issues"] == [] and old_issue["state"] == "stale", "Repair recheck removes the rule Issue and marks the old report stale", {"repaired": repaired, "old": old_issue})
                client.call("history.undo", context=client.current(), key="undo-repair")
                require(client.fields(load_case)["forces"] == [], "Undo of the repair returns the missing reference", client.fields(load_case))
                client.call("history.redo", context=client.current(), key="redo-repair")
                require(checkpoint("repaired_M") == original_m, "Redo of the repair restores all M semantics and exact references", True)

                prior = client.current()
                saved = client.call("project.save", {"path": str(root / "M.qcae")}, prior, "save-M")
                saved_as = client.call("project.save_as", {"path": str(root / "M-copy.qcae")}, saved, "save-as-M")
                require(not saved["dirty"] and not saved_as["dirty"] and saved_as["document_id"] == prior["document_id"] and saved_as["project_id"] != saved["project_id"],
                        "Save and save-as establish exact save markers while retaining document authority", {"saved": saved, "saved_as": saved_as})
                client.call("project.close", {"policy": "discard"}, saved_as, "close-M")
                opened = client.call("project.open", {"mode": "normal", "path": str(root / "M.qcae")}, key="normal-open-M")
                require(opened["document_id"] != prior["document_id"] and opened["revision"] == "0" and checkpoint("normal_open_M") == original_m,
                        "Normal open uses a new DocumentId and retains every M identity/connection/reference", opened)
                process.kill()
                process.wait(timeout=10)
                require(start()["recovery_available"], "Actual engine process termination exposes explicit recovery", True)
                recovered = client.call("project.open", {"mode": "recover"}, key="recover-M")
                require(recovered["document_id"] == opened["document_id"] and recovered["document_epoch"] != opened["document_epoch"] and checkpoint("recovered_M") == original_m,
                        "Recovery retains the document and complete M under a new Epoch", recovered)

                export_context = client.current()
                exported = wait_task(client, client.call("model.export", {"analysis_id": analysis,
                    "output_directory": str(root / "artifact")}, export_context, "publish-Nastran-M", profile=profile))
                artifact_id = exported["artifact_receipt"]["artifact_id"]
                artifact = client.call("artifact.get", {"artifact_id": artifact_id}, export_context)
                manifest_bytes = Path(artifact["manifest_path"]).read_bytes()
                manifest = json.loads(manifest_bytes)
                require(artifact["state"] == "published" and artifact["verified"] and manifest["complete"] and client.current()["revision"] == export_context["revision"],
                        "Actual complete Nastran artifact is published without creating a model transaction", exported)
                require(hashlib.sha256(manifest_bytes).hexdigest() == exported["artifact_receipt"]["manifest_sha256"],
                        "Published manifest bytes match the actual task receipt SHA256", artifact)
                fixture = bind_r(artifact, manifest, nodes)
                result = client.call("results.read_fixture", {"artifact_id": artifact_id, "fixture_json": json.dumps(fixture)}, export_context, "read-R")
                require(result["state"] == "current" and result["source_kind"] == "fixture" and result["field"]["values"] == [
                    {"entity_id": nodes[0], "value": [0, 0, 0]}, {"entity_id": nodes[10], "value": [0, -1, 0]}],
                    "R fixture is read with exact node values, identity mapping and fixture provenance", result)
                require(checkpoint("published_M") == original_m,
                        "Artifact publication and reading R preserve every M field and reference", True)
                client.call("material.set_young_modulus", {"entity_id": material, "young_modulus": {"value": 200, "unit": "GPa"}}, client.current(), "change-result-input")
                stale = client.call("results.get", {"result_id": result["result_id"]}, client.current())
                require(stale["state"] == "stale" and stale["source_kind"] == "fixture" and stale["field"] == result["field"] and stale["input_version"] == result["input_version"],
                        "Changed corresponding input marks R stale while retaining exact fixture source and frozen fields", stale)
                checkpoint("stale_result_input_M")
                write(evidence / "R-bound.json", fixture)
                write(evidence / "artifact-manifest.json", manifest)
                write(evidence / "R-current.json", result)
                write(evidence / "R-stale.json", stale)
                record["artifact"] = artifact
                record["result_current"] = result
                record["result_stale"] = stale
                record["passed"] = True
            except Exception as error:
                record["error"] = str(error)
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                write(evidence / "transcript.json", client.transcript)
                log.flush()
                log.seek(0)
                (evidence / "engine.log").write_text(log.read())
    write(evidence / "workflow.json", record)
    print(json.dumps({"mode": "cli", "run_id": record["run_id"], "passed": record["passed"],
                      "error": record.get("error"), "checkpoints": len(record["semantic_checkpoints"])}), flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--source-tree-sha256", default="mutable-diagnostic")
    args = parser.parse_args()
    args.engine, args.cli = str(Path(args.engine).resolve()), str(Path(args.cli).resolve())
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    reports = [run_once(args, number) for number in range(1, 4)]
    semantic_differences = 0
    if all(report["passed"] for report in reports):
        for report in reports[1:]:
            semantic_differences += report["semantic_checkpoints"] != reports[0]["semantic_checkpoints"]
    report = {"mode": "cli", "runs": 3, "passed": sum(item["passed"] for item in reports),
              "semantic_differences": semantic_differences, "source_tree_sha256": args.source_tree_sha256,
              "source_kind": "fixture", "real_solver": False, "runs_detail": reports}
    write(args.evidence_dir / "workflow-report.json", report)
    return 0 if report["passed"] == 3 and semantic_differences == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
