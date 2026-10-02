#!/usr/bin/env python3
"""SK-11: disabled Nastran package leaves real common services usable."""
from __future__ import annotations

import argparse
from pathlib import Path
import tempfile

from c3_sync_ipc_tests import EngineProcess, check


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="qcae-no-nastran-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        with (root / "engine.log").open("w+") as log:
            host = EngineProcess(args.engine, str(root / "engine.sock"), str(root / "workspace.sqlite"), log)
            try:
                capabilities = host.start()
                check(capabilities["declared_solver_profiles"] == [], "Disabled package advertised a profile")
                operations = {item["name"]: item for item in capabilities["operations"]}
                for name in ("model.export", "model.export_preview", "artifact.get", "artifact.reconcile",
                             "nastran.ui", "project.migrate_profile", "results.read_fixture", "results.get"):
                    check(name not in operations or not operations[name]["available"],
                          f"Disabled package advertised {name}")
                client = host.client
                document = client.call("project.create", {"name": "Common services"}, key="create")
                check("model.import" not in operations["changes.preview"]["supported_commands"],
                      "Uninstalled codec was advertised as an import command")
                before_codec_requests = client.current()
                before_history = client.call("history.list", context=before_codec_requests)
                rejected_import = client.call("changes.preview", {
                    "command": "model.import", "root_resource": "unused.bdf",
                    "resources": [{"path": "unused.bdf", "text": "GRID,1,,0.,0.,0.\n"}],
                    "source_profile_ref": {"profile_id": "not-installed", "profile_version": "0",
                                           "definition_digest": "unused"},
                    "unit_system": "mm-N-MPa"}, before_codec_requests, expected="failed")
                check(rejected_import["error"]["code"] == "UNSUPPORTED_CAPABILITY" and
                      rejected_import["error"]["message"] == "No model codec registered",
                      f"Import did not refuse the missing registered codec: {rejected_import}")
                rejected_export = client.call("model.export_preview", context=before_codec_requests,
                                              expected="failed")
                check(rejected_export["error"]["code"] == "UNSUPPORTED_CAPABILITY",
                      f"Uninstalled codec accepted export preview: {rejected_export}")
                check(client.current() == before_codec_requests and
                      client.call("history.list", context=before_codec_requests) == before_history,
                      "Rejected codec operations changed the common document or history")
                line = client.call("geometry.create_line", {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]},
                                   document, "line")
                current = client.current()
                check(int(current["revision"]) == int(document["revision"]) + 1, "Common transaction missing")
                identities = client.call("entity.query", {"kind": "geometry"}, current)["entities"]
                check([item["entity_id"] for item in identities] == [line["entity_id"]], "Common query missing")
                client.call("history.undo", context=current, key="undo")
                check(client.call("entity.query", {"kind": "geometry"}, client.current())["entities"] == [],
                      "Common history missing")
                print("PASS: disabled package advertises no Nastran capabilities; common transaction/query/history run")
            finally:
                host.close()


if __name__ == "__main__":
    main()
