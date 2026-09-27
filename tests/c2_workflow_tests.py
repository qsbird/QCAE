#!/usr/bin/env python3
"""C2: six real-engine runs through CLI and a transport-only Python client."""
from __future__ import annotations

import argparse
import hashlib
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path


class Client:
    def __init__(self, mode, cli, endpoint):
        self.mode, self.cli, self.endpoint = mode, cli, endpoint
        self.transcript = []
        self.sequence = 0

    def call(self, operation, parameters=None, context=None, key=None, expected="success"):
        self.sequence += 1
        request = {"api_version": "1.1", "request_id": f"c2-{self.sequence}",
                   "operation": operation, "parameters": parameters or {}}
        if context:
            request.update(document_id=context["document_id"],
                           document_epoch=context["document_epoch"],
                           expected_revision=context["revision"])
        if key is not None:
            request["idempotency_key"] = key
        if self.mode == "cli":
            process = subprocess.run([self.cli, "--socket", self.endpoint, "--no-start"],
                                     input=json.dumps(request), text=True, capture_output=True,
                                     timeout=15)
            if not process.stdout:
                raise RuntimeError(f"CLI {operation}: {process.stderr}")
            response = json.loads(process.stdout)
            assert process.returncode == (0 if response.get("status") == "success" else 2)
        else:
            # No domain model, state or business rules live in this client.
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(15)
                connection.connect(self.endpoint)
                with connection.makefile("rwb") as stream:
                    handshake = {"api_version": "1.1", "request_id": "handshake",
                                 "operation": "runtime.handshake"}
                    stream.write(json.dumps(handshake).encode() + b"\n")
                    stream.flush()
                    answer = json.loads(stream.readline())
                    assert answer["status"] == "success" and answer["request_id"] == "handshake"
                    stream.write(json.dumps(request).encode() + b"\n")
                    stream.flush()
                    response = json.loads(stream.readline())
        self.transcript.append({"request": request, "response": response})
        assert response.get("request_id") == request["request_id"], response
        assert response.get("status") == expected, (operation, expected, response)
        return response["data"] if expected == "success" else response

    def current(self):
        return self.call("project.current")

    def fields(self, entity):
        return self.call("entity.fields", {"entity_id": entity}, self.current())["fields"]

    def entities(self, kind):
        return self.call("entity.query", {"kind": kind}, self.current())["entities"]


def canonical(value, identities):
    if isinstance(value, str):
        return identities.get(value, value)
    if isinstance(value, list):
        return [canonical(item, identities) for item in value]
    if isinstance(value, dict):
        return {key: canonical(item, identities) for key, item in value.items()}
    return value


def run_once(engine, cli, mode, number, evidence):
    # A short canonical Unix path avoids socket-length limits and SQLite symlink checks.
    with tempfile.TemporaryDirectory(prefix="qc2-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint, workspace = str(root / "engine.sock"), str(root / "work.sqlite")
        client = Client(mode, cli, endpoint)
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
                    except (OSError, RuntimeError):
                        time.sleep(.05)
                raise AssertionError("Engine startup timed out")

            try:
                caps = start()
                assert caps["durable"] and caps["storage_mode"] == "sqlite"
                doc = client.call("project.create", {"name": "C2 line mesh"}, key="new")
                initial = client.call("model.summary", context=doc)
                assert initial["node_count"] == 0
                line_args = {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]}
                line = client.call("geometry.create_line", line_args, doc, "line")
                replay = client.call("geometry.create_line", line_args, doc, "line")
                assert replay["entity_id"] == line["entity_id"]
                assert replay["transaction_id"] == line["transaction_id"] and replay["replayed"]
                client.call("geometry.create_line", {**line_args, "end_mm": [2000, 0, 0]},
                            doc, "line", expected="conflict")
                before_mesh = client.current()
                mesh_args = {"geometry_id": line["entity_id"], "segments": 10}
                task = client.call("mesh.generate_line", mesh_args, before_mesh, "mesh")
                deadline = time.monotonic() + 15
                while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
                    assert time.monotonic() < deadline, task
                    time.sleep(.01)
                    task = client.call("task.status", {"task_id": task["task_id"]}, client.current())
                assert task["state"] == "succeeded", task
                assert int(client.current()["revision"]) == int(before_mesh["revision"]) + 1
                duplicate_task = client.call("mesh.generate_line", mesh_args, before_mesh, "mesh")
                assert duplicate_task["task_id"] == task["task_id"]
                nodes, beams = client.entities("node"), client.entities("beam")
                assert len(nodes) == 11 and len(beams) == 10
                node_fields = {n["entity_id"]: client.fields(n["entity_id"]) for n in nodes}
                node_ids = sorted(node_fields, key=lambda identity: node_fields[identity]["position"][0])
                for index, identity in enumerate(node_ids):
                    assert node_fields[identity]["position"] == [index * 100, 0, 0]
                material_args = {"name": "Steel", "young_modulus": {"value": 210, "unit": "GPa"},
                                 "poisson_ratio": .3}
                material = client.call("material.create", material_args, client.current(), "material")
                section_args = {"name": "Beam section", "material_id": material["entity_id"],
                                "area_mm2": 100, "i1_mm4": 833.333, "i2_mm4": 833.333,
                                "torsion_mm4": 1400}
                section = client.call("section.create", section_args, client.current(), "section")
                client.call("beam.assign_section", {"beam_ids": [b["entity_id"] for b in beams],
                                                     "section_id": section["entity_id"]},
                            client.current(), "assign")
                before_edit = client.current()
                client.call("node.move", {"entity_id": node_ids[5], "position_mm": [500, 1, 0]},
                            before_edit, "move")
                assert client.fields(node_ids[5])["position"] == [500, 1, 0]
                client.call("history.undo", context=client.current(), key="undo")
                assert client.fields(node_ids[5])["position"] == [500, 0, 0]
                client.call("history.redo", context=client.current(), key="redo")
                assert client.fields(node_ids[5])["position"] == [500, 1, 0]
                assert int(client.current()["revision"]) == int(before_edit["revision"]) + 3
                client.call("node.move", {"entity_id": node_ids[5], "position_mm": [500, 2, 0]},
                            before_edit, "stale-move", expected="conflict")
                identities = {line["entity_id"]: "geometry", material["entity_id"]: "material",
                              section["entity_id"]: "section"}
                identities.update({identity: f"node-{index}" for index, identity in enumerate(node_ids)})
                mesh_entities = client.entities("mesh")
                assert len(mesh_entities) == 1
                identities[mesh_entities[0]["entity_id"]] = "mesh"
                beam_fields = {b["entity_id"]: client.fields(b["entity_id"]) for b in beams}
                beam_ids = sorted(beam_fields, key=lambda identity: node_ids.index(beam_fields[identity]["nodes"][0]))
                identities.update({identity: f"beam-{index}" for index, identity in enumerate(beam_ids)})
                for index, identity in enumerate(beam_ids):
                    assert beam_fields[identity]["nodes"] == node_ids[index:index + 2]
                    assert beam_fields[identity]["section"] == section["entity_id"]
                def model():
                    return {name: canonical(client.fields(identity), identities)
                            for identity, name in sorted(identities.items(), key=lambda item: item[1])}
                baseline = model()
                project = str(root / "line.qcae")
                saved = client.call("project.save", {"path": project}, client.current(), "save")
                assert not saved["dirty"]
                client.call("material.set_young_modulus",
                            {"entity_id": material["entity_id"], "young_modulus": {"value": 200, "unit": "GPa"}},
                            client.current(), "unsaved-E")
                before_crash = client.current()
                changed = model()
                process.kill()
                process.wait(timeout=10)
                assert start()["recovery_available"]
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                assert recovered["document_id"] == before_crash["document_id"]
                assert recovered["document_epoch"] != before_crash["document_epoch"]
                assert recovered["revision"] == before_crash["revision"] and recovered["dirty"]
                assert model() == changed
                restored_task = client.call("task.status", {"task_id": task["task_id"]}, recovered)
                assert restored_task["state"] == "succeeded" and restored_task["receipt"] == task["receipt"]
                fact = client.call("operations.get", {"lookup_scope": "document",
                                   "original_operation": "material.create", "idempotency_key": "material"}, recovered)
                assert fact["entity_id"] == material["entity_id"]
                assert fact["transaction_id"] == material["transaction_id"]
                client.call("history.undo", context=before_crash, key="old-epoch", expected="conflict")
                client.call("history.undo", context=client.current(), key="undo-unsaved")
                assert model() == baseline and not client.current()["dirty"]
                client.call("project.close", {"policy": "discard"}, client.current(), "close")
                opened = client.call("project.open", {"mode": "normal", "path": project}, key="normal-open")
                assert opened["document_id"] != recovered["document_id"] and opened["revision"] == "0"
                assert model() == baseline
                summary = client.call("model.summary", context=opened)
                assert summary["geometry_count"] == 1 and summary["mesh_count"] == 1
                digest = hashlib.sha256(json.dumps(baseline, sort_keys=True).encode()).hexdigest()
                result = {"mode": mode, "run": number, "passed": True, "semantic_sha256": digest,
                          "nodes": 11, "beams": 10, "task_state": restored_task["state"],
                          "recovery_verified": True, "normal_open_verified": True,
                          "requests": len(client.transcript)}
                print(json.dumps(result), flush=True)
                return result, baseline
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                if evidence:
                    evidence.mkdir(parents=True, exist_ok=True)
                    (evidence / f"{mode}-{number}-transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")
                    log.flush()
                    log.seek(0)
                    (evidence / f"{mode}-{number}-engine.log").write_text(log.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    results, golden = [], None
    for mode in ("cli", "script"):
        for run in range(1, 4):
            result, model = run_once(str(Path(args.engine).resolve()), str(Path(args.cli).resolve()),
                                     mode, run, args.evidence_dir)
            if golden is None:
                golden = model
            assert model == golden, (mode, run, "cross-entry engineering semantics differ")
            results.append(result)
    report = {"checkpoint": "C2", "passed": len(results), "total": 6,
              "semantic_differences": 0, "runs": results}
    if args.evidence_dir:
        (args.evidence_dir / "workflow-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: C2 CLI/script SQLite workflows 6/6; semantic differences 0")


if __name__ == "__main__":
    main()
