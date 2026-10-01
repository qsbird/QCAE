#!/usr/bin/env python3
"""Existing force edits, previews and persistence through production CLI/IPC."""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import time

from analysis_check_ipc_tests import Client
from real_ai_mcp_physics_scenarios import seed, snapshot, unchanged


def vector(force, y, unit="N"):
    return {"force_id": force, "x": {"value": 0, "unit": "N"},
            "y": {"value": y, "unit": unit}, "z": {"value": 0, "unit": "N"}}


def run(args, mode):
    output = args.evidence_dir / mode if args.evidence_dir else None
    if output:
        output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="qcae-force-vector-", dir="/tmp") as folder:
        root = Path(folder)
        endpoint = str(root / "engine.sock")
        client = Client(args.engine, args.cli, endpoint, mode)
        process = None
        facts = {"mode": mode, "passed": False}
        with (root / "engine.log").open("w+") as log:
            def start():
                nonlocal process
                process = subprocess.Popen([args.engine, "--socket", endpoint,
                                            "--workspace", str(root / "work.sqlite")],
                                           stdout=log, stderr=log)
                deadline = time.monotonic() + 15
                while process.poll() is None and time.monotonic() < deadline:
                    try:
                        return client.call("capabilities.list")
                    except (OSError, AssertionError, json.JSONDecodeError):
                        pass
                    time.sleep(.025)
                raise AssertionError("real engine handshake did not become available")

            try:
                caps = start()
                catalog = {item["name"]: item for item in caps["operations"]}
                assert catalog["force.set_vector"]["available"] and catalog["force.preview_vector"]["available"]
                assert catalog["force.set_vector"]["effect"] == "document_write"
                assert catalog["force.preview_vector"]["effect"] == "preview"
                fixture = seed(client)
                force = client.call("force.create", {"node_id": fixture["tip"],
                                                     **{axis: {"value": value, "unit": "N"}
                                                        for axis, value in zip("xyz", (0, -1, 0))}},
                                    client.current(), "force-vector-create")["entity_id"]
                before = snapshot(client)
                context = before["context"]
                params = vector(force, -.002, "kN")
                receipt = client.call("force.set_vector", params, context, "force-vector-edit")
                changed = snapshot(client)
                assert int(changed["context"]["revision"]) == int(context["revision"]) + 1
                assert len(changed["history"]["items"]) == len(before["history"]["items"]) + 1
                expected_rows = copy.deepcopy(before["entities"])
                for row in expected_rows:
                    if row["entity_id"] == force:
                        row["force_n"] = [0, -2, 0]
                assert changed["entities"] == expected_rows, "force setter changed an unrelated field/reference"
                replay = client.call("force.set_vector", params, context, "force-vector-edit")
                assert replay["replayed"] and replay["transaction_id"] == receipt["transaction_id"]
                assert replay["committed_revision"] == receipt["committed_revision"]
                unchanged(changed, snapshot(client))
                rejected = client.call("force.set_vector", vector(force, -3), context,
                                       "force-vector-edit", expected="conflict")
                assert rejected["error"]["code"] == "IDEMPOTENCY_KEY_CONFLICT"
                rejected = client.call("force.set_vector", params, context,
                                       "fresh-stale-force", expected="conflict")
                assert rejected["error"]["code"] == "REVISION_CONFLICT"
                negative = []
                for name, invalid, status, code in [
                    ("missing-unit", vector(force, -3, ""), "needs_input", "MISSING_INPUT"),
                    ("wrong-unit", vector(force, -3, "mm"), "failed", "INVALID_UNIT"),
                    ("unknown-force", vector("not-a-force", -3), "failed", "ENTITY_NOT_FOUND"),
                    ("wrong-kind", vector(fixture["material"], -3), "failed", "ENTITY_NOT_FOUND"),
                    ("wrong-type", vector(force, "-3"), "failed", "INVALID_INPUT"),
                ]:
                    response = client.call("force.set_vector", invalid, client.current(),
                                           "force-invalid-" + name, expected=status)
                    assert response["error"]["code"] == code
                    negative.append({"name": name, "response": response})
                    unchanged(changed, snapshot(client))
                missing = vector(force, -3)
                del missing["y"]
                response = client.call("force.set_vector", missing, client.current(),
                                       "force-missing-component", expected="needs_input")
                assert response["error"]["code"] == "MISSING_INPUT"
                unchanged(changed, snapshot(client))
                client.call("history.undo", context=client.current(), key="force-undo")
                assert client.fields(force) == {"node": fixture["tip"], "force_n": [0, -1, 0]}
                undone = snapshot(client)
                replay = client.call("force.set_vector", params, context, "force-vector-edit")
                assert replay["replayed"] and replay["transaction_id"] == receipt["transaction_id"]
                unchanged(undone, snapshot(client))
                client.call("history.redo", context=client.current(), key="force-redo")
                assert client.fields(force)["force_n"] == [0, -2, 0]
                before_preview = snapshot(client)
                preview = client.call("force.preview_vector", vector(force, -3), before_preview["context"])
                assert preview["affected_entity_id"] == force and not preview["creates_entity"]
                unchanged(before_preview, snapshot(client))
                # Cancelling a UI preview means dropping its token: no commit is issued.
                client.call("force.set_vector", vector(force, -4), client.current(), "interleaved-force")
                interleaved = snapshot(client)
                response = client.call("changes.commit", {"preview_id": preview["preview_id"]},
                                       before_preview["context"], "stale-force-preview", expected="conflict")
                assert response["error"]["code"] == "REVISION_CONFLICT"
                unchanged(interleaved, snapshot(client))
                current = client.current()
                fresh = client.call("force.preview_vector", vector(force, -3), current)
                unchanged(interleaved, snapshot(client))
                committed = client.call("changes.commit", {"preview_id": fresh["preview_id"]},
                                        current, "shared-preview-force")
                assert committed["entity_id"] == force and client.fields(force)["force_n"] == [0, -3, 0]
                client.call("history.undo", context=client.current(), key="undo-preview-force")
                assert client.fields(force)["force_n"] == [0, -4, 0]
                saved_context = client.current()
                saved_rows = snapshot(client)["entities"]
                project = root / "force.qcae"
                client.call("project.save_as", {"path": str(project)}, saved_context, "save-force-vector")
                process.kill()
                process.wait(timeout=10)
                assert start()["recovery_available"]
                recovered = client.call("project.open", {"mode": "recover"}, key="recover-force-vector")
                assert recovered["document_epoch"] != saved_context["document_epoch"]
                assert recovered["revision"] == saved_context["revision"]
                assert snapshot(client)["entities"] == saved_rows
                rejected = client.call("force.set_vector", vector(force, -9), saved_context,
                                       "old-force-epoch", expected="conflict")
                assert rejected["error"]["code"] == "DOCUMENT_EPOCH_EXPIRED"
                client.call("project.close", {"policy": "discard"}, client.current(), "close-force-vector")
                opened = client.call("project.open", {"mode": "normal", "path": str(project)}, key="normal-force-vector")
                assert opened["revision"] == "0" and opened["document_id"] != recovered["document_id"]
                assert snapshot(client)["entities"] == saved_rows
                facts.update(passed=True, fixture=fixture, force_id=force, before=before,
                             changed=changed, invalid=negative, recovered=recovered, opened=opened,
                             actual_requests=len(client.transcript))
                print(json.dumps({"mode": mode, "passed": True, "requests": len(client.transcript)}), flush=True)
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                if output:
                    (output / "facts.json").write_text(json.dumps(facts, indent=2) + "\n")
                    (output / "transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")
                    log.flush()
                    log.seek(0)
                    (output / "engine.log").write_text(log.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    args.engine, args.cli = str(Path(args.engine).resolve()), str(Path(args.cli).resolve())
    for mode in ("cli", "script"):
        run(args, mode)


if __name__ == "__main__":
    main()
