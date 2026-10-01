#!/usr/bin/env python3
"""EXT-03 through production engine/CLI, real meshed M and explicit SQLite."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

from analysis_check_ipc_tests import Client
from result_fixture_ipc_tests import create_m


def run(args):
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="qcae-tri3-ipc-", dir="/private/tmp") as folder:
        root = Path(folder)
        client = Client(args.engine, args.cli, str(root / "engine.sock"), "cli")
        process = None
        with (args.evidence_dir / "engine.log").open("w+") as log:
            def start():
                nonlocal process
                process = subprocess.Popen([args.engine, "--socket", client.endpoint,
                                            "--workspace", str(root / "work.sqlite")],
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

            def rows():
                return client.call("entity.query", context=client.current())["entities"]

            try:
                capabilities = start()
                assert capabilities["durable"]
                declared = {item["name"]: item for item in capabilities["operations"]}
                assert declared["mesh.create_tri3"]["available"]
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                nodes, beams, material, part, analysis = create_m(client, profile)
                assert len(nodes) == 11 and len(beams) == 10
                mesh = client.entities("mesh")[0]["entity_id"]
                client.call("node.move", {"entity_id": nodes[5], "position_mm": [500, 1, 0]},
                            client.current(), "bend-node5")
                before = client.current()
                original_rows = rows()
                history = client.call("history.list", context=before)
                valid = {"node_ids": [nodes[4], nodes[5], nodes[6]], "mesh_id": mesh}
                for index, invalid in enumerate((
                        {"node_ids": valid["node_ids"][:2], "mesh_id": mesh},
                        {"node_ids": [nodes[4], nodes[5], nodes[5]], "mesh_id": mesh},
                        {"node_ids": [nodes[4], nodes[5], "unknown"], "mesh_id": mesh},
                        {"node_ids": [nodes[4], nodes[5], material], "mesh_id": mesh},
                        {"node_ids": [nodes[0], nodes[1], nodes[2]], "mesh_id": mesh},
                        {"node_ids": valid["node_ids"], "mesh_id": material},
                        {"node_ids": valid["node_ids"], "mesh_id": "unknown"},
                        {"node_ids": "bad", "mesh_id": mesh},
                )):
                    client.call("mesh.create_tri3", invalid, before, f"invalid-{index}", expected="failed")
                    assert rows() == original_rows and client.current() == before
                    assert client.call("history.list", context=before) == history
                view = client.call("view.create", {"hidden_ids": [], "camera_fingerprint": "tri3"}, before)
                selection = client.call("selection.evaluate", {
                    "view_session_id": view["view_session_id"],
                    "expected_view_revision": view["view_revision"],
                    "predicate": {"op": "all"},
                    "scope": {"visibility": "through", "candidate_ids": [nodes[5]], "include_hidden": False}}, before)
                created = client.call("mesh.create_tri3", valid, before, "create-triangle")
                triangle = created["entity_id"]
                after = client.current()
                assert int(after["revision"]) == int(before["revision"]) + 1
                assert client.fields(triangle) == {"nodes": valid["node_ids"], "mesh": mesh}
                queried = client.entities("tri3")
                assert len(queried) == 1 and queried[0]["entity_id"] == triangle
                assert queried[0]["nodes"] == valid["node_ids"] and queried[0]["mesh_id"] == mesh
                after_rows = rows()
                assert [row for row in after_rows if row["kind"] != "tri3"] == original_rows
                incoming = client.call("entity.references", {"entity_id": nodes[5], "direction": "incoming"}, after)["references"]
                assert {"from": triangle, "to": nodes[5], "role": "tri3.node"} in incoming
                retry = client.call("mesh.create_tri3", valid, before, "create-triangle")
                assert retry["transaction_id"] == created["transaction_id"] and retry["replayed"]
                assert client.current() == after
                changed = {**valid, "node_ids": list(reversed(valid["node_ids"]))}
                conflict = client.call("mesh.create_tri3", changed, before, "create-triangle", expected="conflict")
                assert conflict["error"]["code"] == "IDEMPOTENCY_KEY_CONFLICT"
                client.call("mesh.create_tri3", valid, before, "stale-revision", expected="conflict")
                client.call("mesh.create_tri3", valid, {**after, "document_epoch": "old"}, "stale-epoch", expected="conflict")
                client.call("selection.get", {"selection_handle": selection["selection_handle"]}, after, expected="conflict")
                collapse = client.call("node.move", {"entity_id": nodes[5], "position_mm": [500, 0, 0]},
                                       after, "collapse", expected="failed")
                assert collapse["error"]["code"] == "INVALID_INPUT" and rows() == after_rows
                deleted = client.call("changes.preview", {"command": "entity.delete", "entity_id": nodes[5]}, after, expected="failed")
                assert deleted["error"]["code"] == "INVALID_INPUT" and rows() == after_rows
                preview = client.call("model.export_preview", {"analysis_id": analysis, "expected_profile_ref": profile}, after, expected="failed")
                assert preview["error"]["code"] == "UNSUPPORTED_CAPABILITY"
                for operation, parameters, key in (
                        ("analysis.check", {"analysis_id": analysis}, "check-shell"),
                        ("model.export", {"analysis_id": analysis, "output_directory": str(root / "shell-export")}, "export-shell")):
                    reply = client.call(operation, parameters, after, key, expected="failed", profile=profile)
                    assert reply["error"]["code"] == "UNSUPPORTED_CAPABILITY"
                assert not (root / "shell-export").exists() and rows() == after_rows
                client.call("history.undo", context=after, key="undo-triangle")
                assert rows() == original_rows
                client.call("mesh.create_tri3", valid, before, "create-triangle")
                assert rows() == original_rows
                client.call("history.redo", context=client.current(), key="redo-triangle")
                assert rows() == after_rows
                saved = client.call("project.save", {"path": str(root / "tri3.qcae")}, client.current(), "save")
                client.call("project.close", {"policy": "discard"}, saved, "close")
                opened = client.call("project.open", {"mode": "normal", "path": str(root / "tri3.qcae")}, key="normal-open")
                assert opened["document_id"] != saved["document_id"] and opened["revision"] == "0"
                assert rows() == after_rows
                process.kill()
                process.wait(timeout=10)
                assert start()["recovery_available"]
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                assert recovered["document_id"] == opened["document_id"]
                assert recovered["document_epoch"] != opened["document_epoch"] and rows() == after_rows
                client.call("mesh.create_tri3", valid, opened, "old-open-epoch", expected="conflict")
                print("PASS: production CLI/IPC Tri3 on real 11-node/10-Line2 M; query/references, atomic errors, history, actual save/open/process recovery, stale selection, explicit analysis/export refusal")
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                (args.evidence_dir / "transcript.json").write_text(json.dumps(client.transcript, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    run(parser.parse_args())
