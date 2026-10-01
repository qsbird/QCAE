#!/usr/bin/env python3
"""BP-11/14: explicit M load case and versioned checks through real CLI/IPC."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


class Client:
    def __init__(self, engine, cli, endpoint, mode):
        self.engine, self.cli, self.endpoint, self.mode = engine, cli, endpoint, mode
        self.sequence, self.transcript = 0, []

    def call(self, operation, parameters=None, context=None, key=None, expected="success", profile=None):
        self.sequence += 1
        request = {"api_version": "1.1", "request_id": f"analysis-{self.sequence}",
                   "operation": operation, "parameters": parameters or {}}
        if context:
            request.update(document_id=context["document_id"], document_epoch=context["document_epoch"],
                           expected_revision=context["revision"])
        if key:
            request["idempotency_key"] = key
        if profile:
            request["expected_profile"] = profile
        if self.mode == "cli":
            completed = subprocess.run([self.cli, "--socket", self.endpoint, "--no-start"],
                                       input=json.dumps(request), capture_output=True, text=True, timeout=15)
            assert completed.stdout, completed.stderr
            response = json.loads(completed.stdout)
            assert completed.returncode == (0 if response.get("status") == "success" else 2), response
        else:
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(15)
                connection.connect(self.endpoint)
                with connection.makefile("rwb") as stream:
                    handshake = {"api_version": "1.1", "request_id": "handshake", "operation": "runtime.handshake"}
                    stream.write(json.dumps(handshake).encode() + b"\n")
                    stream.flush()
                    assert json.loads(stream.readline())["status"] == "success"
                    stream.write(json.dumps(request).encode() + b"\n")
                    stream.flush()
                    response = json.loads(stream.readline())
        self.transcript.append({"request": request, "response": response})
        assert response.get("request_id") == request["request_id"] and response.get("status") == expected, response
        return response["data"] if expected == "success" else response

    def current(self):
        return self.call("project.current")

    def fields(self, entity):
        return self.call("entity.fields", {"entity_id": entity}, self.current())["fields"]

    def entities(self, kind):
        return self.call("entity.query", {"kind": kind}, self.current())["entities"]

    def edit(self, command, parameters, key):
        context = self.current()
        preview = self.call("changes.preview", {"command": command, **parameters}, context)
        return self.call("changes.commit", {"preview_id": preview["preview_id"]}, context, key)


def run_once(engine, cli, mode, evidence):
    with tempfile.TemporaryDirectory(prefix="qcae-analysis-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint, workspace = str(root / "engine.sock"), str(root / "work.sqlite")
        client = Client(engine, cli, endpoint, mode)
        process = None
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
                for name in ("force.create", "constraint.create", "load_case.create", "load_case.set_references",
                             "analysis.create", "analysis.check", "analysis.get_issues"):
                    assert operations[name]["available"], (name, operations[name])
                assert operations["analysis.check"]["effect"] == "auxiliary_write"
                assert operations["analysis.check"]["requires_revision"]
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                document = client.call("project.create", {"name": "M analysis"}, key="create")
                line = client.call("geometry.create_line", {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]},
                                   document, "line")
                task = client.call("mesh.generate_line", {"geometry_id": line["entity_id"], "segments": 10},
                                   client.current(), "mesh")
                deadline = time.monotonic() + 15
                while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
                    assert time.monotonic() < deadline
                    time.sleep(.01)
                    task = client.call("task.status", {"task_id": task["task_id"]}, client.current())
                assert task["state"] == "succeeded", task
                nodes = sorted((item["entity_id"] for item in client.entities("node")),
                               key=lambda identity: client.fields(identity)["position"][0])
                beams = [item["entity_id"] for item in client.entities("beam")]
                assert len(nodes) == 11 and len(beams) == 10
                for index, node in enumerate(nodes):
                    assert client.fields(node)["position"] == [index * 100, 0, 0]
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
                                                       "y": {"value": -.001, "unit": "kN"}, "z": {"value": 0, "unit": "N"}},
                                    client.current(), "force")["entity_id"]
                constraint = client.call("constraint.create", {"node_ids": [nodes[0]], "dofs": "123456"},
                                         client.current(), "constraint")["entity_id"]
                load_case = client.call("load_case.create", {"name": "M static", "force_ids": [force],
                                                               "constraint_ids": [constraint]}, client.current(), "load-case")["entity_id"]
                analysis = client.call("analysis.create", {"name": "M linear static", "load_case_ids": [load_case]},
                                       client.current(), "analysis", profile=profile)["entity_id"]
                assert analysis != task["task_id"]
                assert client.fields(force) == {"node": nodes[10], "force_n": [0, -1, 0]}
                assert client.fields(constraint) == {"nodes": [nodes[0]], "dofs": "123456"}
                case_fields = {"name": "M static", "forces": [force], "constraints": [constraint]}
                assert client.fields(load_case) == case_fields
                analysis_fields = client.fields(analysis)
                assert analysis_fields["load_cases"] == [load_case] and not analysis_fields["forces"]
                assert analysis_fields["target"]["profile"] == profile
                initial = client.current()
                history_count = len(client.call("history.list", context=initial)["items"])
                checked = client.call("analysis.check", {"analysis_id": analysis}, initial, "ready-check")
                assert checked["issues"] == [] and checked["state"] == "current"
                assert client.current()["revision"] == initial["revision"]
                assert len(client.call("history.list", context=initial)["items"]) == history_count
                rejection = client.call("changes.preview", {"command": "entity.delete", "entity_id": force}, initial,
                                        expected="failed")
                assert rejection["error"]["code"] == "INVALID_INPUT"
                assert client.current()["revision"] == initial["revision"]
                client.call("analysis.check", {"analysis_id": task["task_id"]}, initial, "task-is-not-analysis", expected="failed")
                saved = client.call("project.save", {"path": str(root / "M.qcae")}, initial, "save")
                client.call("load_case.set_references", {"load_case_id": load_case, "force_ids": [],
                                                          "constraint_ids": [constraint]}, client.current(), "remove-load")
                missing_input = client.current()
                missing = client.call("analysis.check", {"analysis_id": analysis}, missing_input, "missing-check", expected="needs_input")["data"]
                assert len(missing["issues"]) == 1
                issue = missing["issues"][0]
                assert issue["rule_id"] == "missing-load" and issue["rule_version"] == "1"
                assert issue["entity_id"] == analysis and issue["state"] == "current"
                assert issue["input_version"]["revision"] == missing_input["revision"]
                stale = client.call("analysis.get_issues", {"check_id": checked["check_id"]}, client.current())
                assert stale["state"] == "stale"
                process.kill()
                process.wait(timeout=10)
                assert start()["recovery_available"]
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                assert recovered["document_id"] == missing_input["document_id"]
                assert recovered["document_epoch"] != missing_input["document_epoch"]
                assert recovered["revision"] == missing_input["revision"]
                recovered_missing = client.call("analysis.get_issues", {"check_id": missing["check_id"]}, recovered)
                assert recovered_missing["state"] == "stale" and recovered_missing["issues"][0]["state"] == "stale"
                assert recovered_missing["input_version"] == missing["input_version"]
                assert client.fields(analysis)["load_cases"] == [load_case]
                assert client.fields(load_case)["forces"] == []
                fresh = client.call("analysis.check", {"analysis_id": analysis}, recovered, "recovered-check", expected="needs_input")["data"]
                assert len(fresh["issues"]) == 1 and fresh["state"] == "current"
                client.call("load_case.set_references", {"load_case_id": load_case, "force_ids": [force],
                                                          "constraint_ids": [constraint]}, recovered, "repair")
                repaired = client.current()
                rechecked = client.call("analysis.check", {"analysis_id": analysis}, repaired, "recheck")
                assert rechecked["issues"] == [] and rechecked["state"] == "current"
                assert client.call("analysis.get_issues", {"check_id": fresh["check_id"]}, repaired)["state"] == "stale"
                client.call("history.undo", context=repaired, key="undo-repair")
                assert client.fields(load_case)["forces"] == []
                client.call("history.redo", context=client.current(), key="redo-repair")
                assert client.fields(load_case) == case_fields
                client.call("analysis.check", {"analysis_id": analysis}, missing_input, "old-epoch", expected="conflict")
                saved_as = client.call("project.save_as", {"path": str(root / "M-copy.qcae")}, client.current(), "save-as")
                assert saved_as["project_id"] != saved["project_id"]
                client.call("project.close", {"policy": "discard"}, saved_as, "close")
                opened = client.call("project.open", {"mode": "normal", "path": str(root / "M.qcae")}, key="normal-open")
                assert opened["document_id"] != saved["document_id"] and opened["revision"] == "0"
                assert client.fields(load_case) == case_fields and client.fields(analysis) == analysis_fields
                reopened_report = client.call("analysis.get_issues", {"check_id": checked["check_id"]}, opened)
                assert reopened_report["state"] == "stale" and reopened_report["input_version"] == checked["input_version"]
                assert client.call("analysis.check", {"analysis_id": analysis}, opened, "normal-check")["issues"] == []
                result = {"mode": mode, "passed": True, "nodes": 11, "beams": 10, "load_cases": 1,
                          "missing_load_issues": 1, "rechecked_issues": 0, "recovery_verified": True,
                          "normal_open_verified": True, "requests": len(client.transcript)}
                print(json.dumps(result), flush=True)
                return result
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                if evidence:
                    evidence.mkdir(parents=True, exist_ok=True)
                    (evidence / f"{mode}-transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")
                    log.flush()
                    log.seek(0)
                    (evidence / f"{mode}-engine.log").write_text(log.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    results = [run_once(str(Path(args.engine).resolve()), str(Path(args.cli).resolve()), mode, args.evidence_dir)
               for mode in ("cli", "script")]
    if args.evidence_dir:
        (args.evidence_dir / "analysis-check-report.json").write_text(json.dumps({"passed": 2, "total": 2, "runs": results}, indent=2) + "\n")
    print("PASS: BP-11/14 M load case, missing-load/repair and durable CLI/IPC workflows 2/2")


if __name__ == "__main__":
    main()
