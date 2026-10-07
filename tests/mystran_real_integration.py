#!/usr/bin/env python3
"""Opt-in real MYSTRAN 19.0.0 execution through the durable production engine.

An explicit executable is required. This test never synthesizes solver fields or
rewrites the engine's frozen export. Evidence and run artifacts remain local.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from c3_sync_ipc_tests import IpcClient, check
from nastran_artifact_ipc_tests import owned_facts, wait_task
from solver_run_ipc_tests import SolverHost


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(host, capabilities, root):
    client = host.client
    profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
    context = client.call("project.create", {"name": "Real MYSTRAN cantilever"}, key="create")
    fixtures = Path(__file__).parent / "fixtures/nastran-real-benchmark-v3"
    names = ("cantilever.bdf", "nodes.bdf", "beams.bdf", "properties.bdf")
    resources = [{"path": name, "text": (fixtures / name).read_text()} for name in names]
    preview = client.call("changes.preview", {"command": "model.import", "root_resource": names[0],
                          "resources": resources, "source_profile_ref": profile,
                          "unit_system": "mm-N-MPa"}, context)
    client.call("changes.commit", {"preview_id": preview["preview_id"]}, context, "import")
    context = client.current()
    analysis = client.call("entity.query", {"kind": "analysis"}, context)["entities"][0]["entity_id"]
    exported = client.call("model.export", {"analysis_id": analysis,
                           "output_directory": str(root / "artifact")}, context, "export",
                           extra={"expected_profile": profile})
    check(wait_task(client, exported, context)["state"] == "succeeded", "Export failed")
    artifact = "artifact-" + exported["task_id"]
    check(client.call("artifact.get", {"artifact_id": artifact}, context)["verified"],
          "Frozen exported input was not verified")
    return context, profile, {"analysis_id": analysis, "artifact_id": artifact,
                              "run_config_id": "mystran-19-local"}


def run(engine, cli, solver, root):
    root.mkdir()
    (root / "runs").mkdir()
    configuration = {
        "schema_version": "qcae.local-solver-config.v2", "run_config_id": "mystran-19-local",
        "executable": str(solver), "argv": ["{input_root}"], "solver_family": "MYSTRAN",
        "dialect": "MYSTRAN", "solver_version": "19.0.0",
        "version_evidence": "Declaration only; engine must probe the installed binary",
        "expected_outputs": ["work/cantilever.F06", "work/cantilever.ERR"],
        "run_root": str(root / "runs"), "test_only": False, "max_wall_time_ms": "30000",
        "cancel_grace_ms": "200", "max_output_bytes": "16777216",
        "result_reader": {"reader_version": "qcae.mystran.static-f06.v1",
                          "resource": "work/cantilever.F06", "subcase": "1",
                          "unit_system": "mm-N-MPa", "coordinate_basis": "basic"}}
    config = root / "local-solver.json"
    config.write_text(json.dumps(configuration, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="qcae-mystran-ipc-") as socket_root, \
            (root / "engine.log").open("w+") as log:
        host = SolverHost(str(engine), root, log, config)
        host.endpoint = str(Path(socket_root) / "engine.sock")
        host.client = IpcClient(host.endpoint)
        try:
            capabilities = host.start()
            client = host.client
            status = client.call("solver.configuration")
            check(status["validated"] and not status["test_only"] and
                  status["result_reader_available"] and status["numerical_validation_available"], status)
            probe = status["version_probe"]
            check(probe["protocol"] == "mystran.version.v1" and
                  probe["reported_version"] == "19.0.0" and not probe["synthetic"], probe)
            context, profile, parameters = prepare(host, capabilities, root)
            original_revision = context["revision"]
            original_history = client.call("history.list", context=context)
            published_input = {path.name: path.read_bytes() for path in (root / "artifact").glob("*.bdf")}
            check(len(published_input) == 4, "Incomplete exported input")
            runs = []
            for repetition in range(1, 4):
                key = "real-start-" + str(repetition)
                task = client.call("analysis.start", parameters, context, key,
                                   extra={"expected_profile": profile})
                run_id = "run-" + task["task_id"]
                finished = wait_task(client, task, context)
                check(finished["state"] == "succeeded", finished)
                observed = client.call("analysis.get_run", {"run_id": run_id}, context)
                check(observed["execution"] == "exited" and observed["exit_code"] == "0" and
                      observed["parsing"] == "parsed" and observed["results_available"] and
                      observed["source_kind"] == "external_process_observation" and
                      observed["numerical_validation"] == "not_run", observed)
                result = client.call("analysis.get_result", {"run_id": run_id}, context)
                check(result["source_kind"] == "external_solver" and not result["test_only"] and
                      result["input_current"] and result["raw_resources_verified"] and
                      result["reader_version"] == "qcae.mystran.static-f06.v1", result)
                fields = {row["quantity_id"]: row for row in result["fields"]}
                check(len(fields["displacement"]["values"]) == 21 and
                      len(fields["spc_reaction"]["values"]) == 1, "Incomplete actual nodal results")
                directory = root / "runs" / run_id
                for name, data in published_input.items():
                    check((directory / "input" / name).read_bytes() == data and
                          (directory / "work" / name).read_bytes() == data,
                          "Solver changed or adapted frozen input bytes")
                check((directory / "tmp").is_dir(), "No private solver temporary directory")
                manifest = json.loads((directory / ".qcae-result" / "manifest.json").read_text())
                check(manifest["solver_family"] == "MYSTRAN" and manifest["dialect"] == "MYSTRAN" and
                      manifest["solver_version"] == "19.0.0" and
                      manifest["source_kind"] == "external_solver" and len(manifest["files"]) == 5,
                      "Publication lost backend or raw output provenance")
                for file in manifest["files"]:
                    path = directory / ".qcae-result" / file["path"]
                    check(path.stat().st_size == int(file["byte_length"]) and sha(path) == file["sha256"],
                          "Published result hash mismatch")
                checked = client.call("analysis.validate_result", {"run_id": run_id}, context,
                                      "real-validation-" + str(repetition),
                                      extra={"expected_profile": profile})
                summary = checked["summary"]
                check(summary["source_kind"] == "external_solver" and
                      summary["check_kind"] == "numerical_validation" and
                      summary["numerical_stage"] == "passed" and summary["comparison"] == "matched" and
                      summary["component_count"] == "132" and len(checked["components"]) == 132 and
                      all(row["matched"] for row in checked["components"]), checked)
                baseline = owned_facts(host.workspace)
                replay = client.call("analysis.start", parameters, context, key,
                                     extra={"expected_profile": profile})
                numeric_replay = client.call("analysis.validate_result", {"run_id": run_id}, context,
                                             "real-validation-" + str(repetition),
                                             extra={"expected_profile": profile})
                check(replay["task_id"] == task["task_id"] and numeric_replay["replayed"] and
                      numeric_replay["summary"] == summary and owned_facts(host.workspace) == baseline,
                      "Retry repeated execution or changed immutable facts")
                runs.append({"run_id": run_id, "task_id": task["task_id"], "summary": summary,
                             "components": checked["components"], "manifest": manifest,
                             "tip": fields["displacement"]["values"][-1]["components"],
                             "reaction": fields["spc_reaction"]["values"][0]["components"]})
            check(client.current()["revision"] == original_revision and
                  client.call("history.list", context=context) == original_history,
                  "Execution or numerical checks changed model revision/history")
            first = runs[0]["run_id"]
            baseline = owned_facts(host.workspace)
            wrong = dict(profile, definition_digest="wrong-definition")
            client.call("analysis.validate_result", {"run_id": first}, context, "bad-profile",
                        extra={"expected_profile": wrong}, expected="failed")
            stale_revision = dict(context, revision=str(int(original_revision) - 1))
            client.call("analysis.validate_result", {"run_id": first}, stale_revision, "bad-revision",
                        extra={"expected_profile": profile}, expected="conflict")
            check(owned_facts(host.workspace) == baseline, "Rejected check wrote partial facts")
            material = client.call("entity.query", {"kind": "material"}, context)["entities"][0]["entity_id"]
            client.call("material.set_young_modulus", {"entity_id": material,
                        "young_modulus": {"value": 200, "unit": "GPa"}}, context, "stale-physics")
            changed = client.current()
            stale = client.call("analysis.get_result", {"run_id": first}, changed)
            check(not stale["input_current"] and
                  stale["numerical_checks"]["original_source_state"] == "stale", stale)
            client.call("history.undo", context=changed, key="restore-physics")
            context = client.current()
            check(client.call("analysis.get_result", {"run_id": first}, context)["input_current"],
                  "Undo did not restore compatible physical input")
            copied = root / "runs" / first / ".qcae-result" / "work/cantilever.F06"
            data = copied.read_bytes()
            copied.write_bytes(b"x" * len(data))
            baseline = owned_facts(host.workspace)
            client.call("analysis.validate_result", {"run_id": first}, context, "tampered-result",
                        extra={"expected_profile": profile}, expected="failed")
            damaged = client.call("analysis.get_result", {"run_id": first}, context)
            check(not damaged["numerical_checks"]["current_files_verified"] and
                  damaged["numerical_checks"]["latest_applicable_summary"] == runs[0]["summary"] and
                  owned_facts(host.workspace) == baseline, "Damaged files recertified or erased history")
            copied.write_bytes(data)
            saved = root / "cantilever.qcae"
            client.call("project.save", {"path": str(saved)}, context, "save")
            host.process.kill()
            host.process.wait(timeout=5)
            host.process = None
            client.instance_id = None
            check(host.start()["recovery_available"], "Durable solver facts were not recoverable")
            recovered = client.call("project.open", {"mode": "recover"}, key="recover")
            for fact in runs:
                restored = client.call("analysis.get_result", {"run_id": fact["run_id"]}, recovered)
                check(restored["input_current"] and restored["raw_resources_verified"] and
                      restored["numerical_checks"]["current_files_verified"] and
                      restored["numerical_checks"]["latest_applicable_summary"] == fact["summary"],
                      "Recovery changed original solver/numerical facts")
            request = {"api_version": "1.1", "request_id": "real-mystran-cli",
                       "operation": "analysis.get_result", "parameters": {"run_id": first},
                       "document_id": recovered["document_id"], "document_epoch": recovered["document_epoch"]}
            response = subprocess.run([str(cli), "--socket", host.endpoint, "--no-start"],
                                      input=json.dumps(request) + "\n", capture_output=True,
                                      text=True, timeout=10, check=True)
            cli_result = json.loads(response.stdout)
            check(cli_result["status"] == "success" and
                  cli_result["data"]["numerical_checks"]["latest_applicable_summary"] == runs[0]["summary"],
                  "CLI did not share engine solver results")
            check(len(list((root / "runs").glob("run-*"))) == 3,
                  "Recovery or retry launched an extra solver")
            return {"passed": True, "real_solver": True, "solver_sha256": sha(solver),
                    "engine_sha256": sha(engine), "cli_sha256": sha(cli),
                    "profile": profile, "configuration": status, "runs": runs,
                    "components_passed": 396, "frozen_inputs_unchanged": True,
                    "model_history_unchanged_by_solver": True, "stale_physics_and_undo": True,
                    "tampered_output_rejected": True, "sqlite_restart_recovery": True,
                    "cli_same_results": True}
        finally:
            (root / "ipc-transcript.json").write_text(json.dumps(host.client.transcript, indent=2) + "\n")
            host.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="real-run-", dir=args.evidence_dir.resolve()))
    root.rmdir()
    try:
        report = run(args.engine.resolve(), args.cli.resolve(), args.solver.resolve(), root)
    except Exception as error:
        (root / "report.json").write_text(json.dumps({"passed": False, "error": str(error)}, indent=2) + "\n")
        raise
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS real MYSTRAN: 3 runs, 396/396 numerical components; evidence {root}")


if __name__ == "__main__":
    main()
