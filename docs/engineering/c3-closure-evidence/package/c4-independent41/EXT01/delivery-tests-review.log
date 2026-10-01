#!/usr/bin/env python3
"""EXT-01 production IPC, SQLite recovery and normal-open contract."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

from c2_workflow_tests import Client


def run(engine, cli, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="qe1-", dir="/private/tmp") as folder:
        root = Path(folder)
        endpoint = str(root / "engine.sock")
        workspace = str(root / "workspace.sqlite")
        client = Client("script", cli, endpoint)
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
                        time.sleep(.02)
                raise AssertionError("EXT-01 engine startup timed out")

            def description():
                queried = next(row for row in client.entities("material")
                               if row["entity_id"] == material)["description"]
                assert queried == client.fields(material).get("description")
                return queried

            def history():
                return client.call("history.list", context=client.current())

            def engineering():
                fields = client.fields(material)
                return {key: value for key, value in fields.items() if key != "description"}

            def reject(parameters, key, code, context=None):
                before, previous_history = client.current(), history()
                response = client.call("material.set_description", parameters,
                                       context or before, key,
                                       expected="conflict" if code in {
                                           "REVISION_CONFLICT", "DOCUMENT_EPOCH_EXPIRED",
                                           "IDEMPOTENCY_KEY_CONFLICT"} else "failed")
                assert response["error"]["code"] == code, response
                assert client.current() == before and history() == previous_history

            try:
                assert start()["durable"]
                client.call("project.create", {"name": "Material description"}, key="create")
                material = client.call("material.create", {
                    "name": "Steel", "young_modulus": {"value": 210, "unit": "GPa"},
                    "poisson_ratio": .3}, client.current(), "material")["entity_id"]
                other = client.call("material.create", {
                    "name": "Aluminium", "young_modulus": {"value": 70, "unit": "GPa"}},
                    client.current(), "other")["entity_id"]
                other_fields = client.fields(other)
                assert description() is None
                old_engineering = engineering()
                before, prior_history = client.current(), history()
                parameters = {"entity_id": material, "description": "悬臂梁用钢材"}
                receipt = client.call("material.set_description", parameters, before, "set")
                assert description() == parameters["description"]
                assert engineering() == old_engineering and client.fields(other) == other_fields
                assert int(client.current()["revision"]) == int(before["revision"]) + 1
                assert len(history()["items"]) == len(prior_history["items"]) + 1
                queried = next(row for row in client.entities("material")
                               if row["entity_id"] == material)
                assert queried["description"] == parameters["description"]
                replay = client.call("material.set_description", parameters, before, "set")
                assert replay["replayed"] and replay["transaction_id"] == receipt["transaction_id"]
                reject({**parameters, "description": "different"}, "set", "IDEMPOTENCY_KEY_CONFLICT")
                for index, invalid in enumerate((True, 7, None, [], {}, "", "bad\ncontrol", "x" * 1025)):
                    reject({**parameters, "description": invalid}, f"bad-type-{index}", "INVALID_INPUT")
                reject({"entity_id": "missing", "description": "text"}, "missing", "ENTITY_NOT_FOUND")
                reject(parameters, "stale", "REVISION_CONFLICT", before)
                expired = {**client.current(), "document_epoch": "expired"}
                reject(parameters, "epoch", "DOCUMENT_EPOCH_EXPIRED", expired)
                client.call("history.undo", context=client.current(), key="undo")
                assert description() is None
                replay = client.call("material.set_description", parameters, before, "set")
                assert replay["transaction_id"] == receipt["transaction_id"] and description() is None
                client.call("history.redo", context=client.current(), key="redo")
                assert description() == parameters["description"]
                client.call("material.set_description", {"entity_id": material},
                            client.current(), "clear")
                assert description() is None and engineering() == old_engineering
                client.call("history.undo", context=client.current(), key="undo-clear")
                assert description() == parameters["description"]
                client.call("history.redo", context=client.current(), key="redo-clear")
                assert description() is None
                client.call("material.set_description", parameters, client.current(), "set-again")
                project = str(root / "material.qcae")
                client.call("project.save", {"path": project}, client.current(), "save")
                client.call("project.close", {"policy": "discard"}, client.current(), "close")
                client.call("project.open", {"mode": "normal", "path": project}, key="open")
                assert description() == parameters["description"]
                assert engineering() == old_engineering and client.fields(other) == other_fields
                client.call("material.set_description", {"entity_id": material},
                            client.current(), "unsaved-clear")
                before_crash = client.current()
                process.kill()
                process.wait(timeout=10)
                assert start()["recovery_available"]
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                assert recovered["revision"] == before_crash["revision"]
                assert recovered["document_epoch"] != before_crash["document_epoch"]
                assert description() is None
                reject(parameters, "old-epoch", "DOCUMENT_EPOCH_EXPIRED", before_crash)
                client.call("history.undo", context=client.current(), key="recovery-undo")
                assert description() == parameters["description"]
                client.call("history.redo", context=client.current(), key="recovery-redo")
                assert description() is None
                client.call("material.set_description", {**parameters, "description": "Recovered text"},
                            client.current(), "recover-text")
                current = client.current()
                process.kill()
                process.wait(timeout=10)
                assert start()["recovery_available"]
                client.call("project.open", {"mode": "recover"}, key="recover-again")
                assert client.current()["revision"] == current["revision"]
                assert description() == "Recovered text"
                assert engineering() == old_engineering and client.fields(other) == other_fields
                print(f"PASS: EXT-01 production IPC, null/string, single transaction, rejects, "
                      f"retry, history, normal open and SQLite recovery ({len(client.transcript)} requests)")
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                (evidence / "ipc-transcript.json").write_text(
                    json.dumps(client.transcript, indent=2, ensure_ascii=False) + "\n")
                log.flush()
                log.seek(0)
                (evidence / "ipc-engine.log").write_text(log.read())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    run(str(Path(args.engine).resolve()), str(Path(args.cli).resolve()), args.evidence_dir)
