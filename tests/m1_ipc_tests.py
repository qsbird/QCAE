#!/usr/bin/env python3
"""Black-box M1 model-codec and entity IPC checks."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ipc_tests as ipc  # Reuse the established CLI framing and owned-process helpers.


ROOT_DECK = """SOL 101
CEND
SUBCASE 1
SPC = 20
LOAD = 10
DISPLACEMENT = ALL
SPCFORCES = ALL
BEGIN BULK
INCLUDE 'parts/nodes.bdf'
INCLUDE 'parts/physics/beam.bdf'
FORCE,10,2,0,1000.,0.,-1.,0.
SPC1,20,123456,1
ENDDATA
"""

NODE_DECK = """GRID,1,,0.,0.,0.
GRID,2,,1000.,0.,0.
"""

BEAM_DECK = """MAT1,1,210000.,,0.3
PBAR,1,1,100.,833.333,833.333,1400.
CBAR,1,1,1,2,0.,0.,1.
"""


def data(response: dict[str, Any], operation: str) -> dict[str, Any]:
    return ipc.data_of(response, operation)


def document_call(
    socket_path: str,
    document_id: str,
    epoch: str,
    operation: str,
    parameters: dict[str, Any] | None = None,
    *,
    revision: str | None = None,
    key: str | None = None,
    expected_status: str = "success",
) -> dict[str, Any]:
    return ipc.call(
        socket_path,
        operation,
        parameters,
        document_id=document_id,
        document_epoch=epoch,
        expected_revision=revision,
        idempotency_key=key,
        expected_status=expected_status,
    )


def entities(socket_path: str, document_id: str, epoch: str, kind: str | None = None,
             *, view: str | None = None, owner_id: str | None = None) -> list[dict[str, Any]]:
    params: dict[str, Any] = {}
    if kind:
        params["kind"] = kind
    if view:
        params["view"] = view
        params["owner_id"] = owner_id
    response = document_call(socket_path, document_id, epoch, "entity.query", params)
    rows = data(response, "entity.query").get("entities")
    ipc.check(isinstance(rows, list), f"entity.query entities must be a list: {response!r}")
    return rows


def commit_edit(socket_path: str, document_id: str, epoch: str, revision: str,
                command: str, parameters: dict[str, Any], key: str) -> tuple[dict[str, Any], str, str]:
    preview = document_call(
        socket_path,
        document_id,
        epoch,
        "changes.preview",
        {"command": command, **parameters},
        revision=revision,
    )
    preview_data = data(preview, f"{command} preview")
    preview_id = ipc.string_field(preview_data, "preview_id", f"{command} preview.data")
    entity_id = ipc.string_field(preview_data, "affected_entity_id", f"{command} preview.data")
    committed = document_call(
        socket_path,
        document_id,
        epoch,
        "changes.commit",
        {"preview_id": preview_id},
        revision=revision,
        key=key,
    )
    new_revision = str(int(revision) + 1)
    ipc.check(committed.get("revision") == new_revision,
              f"{command} commit should advance to {new_revision}: {committed!r}")
    return committed, new_revision, entity_id


def run(socket_path: str, engine_log: Any) -> None:
    engine = ipc.start_engine(socket_path, engine_log, engine_log)
    try:
        capabilities = data(ipc.wait_for_cli(socket_path, engine), "capabilities.list")
        ipc.check(capabilities.get("configured_solver_profiles") == [],
                  f"no solver profile should be configured: {capabilities!r}")
        declared = capabilities.get("declared_solver_profiles")
        ipc.check(isinstance(declared, list) and len(declared) == 1,
                  f"one Nastran profile should be declared: {declared!r}")
        profile = ipc.require_mapping(declared[0], "declared Nastran profile")
        ipc.check(profile.get("configured") is False,
                  f"declared Nastran profile must remain unconfigured: {profile!r}")
        profile_ref = ipc.require_mapping(profile.get("profile_ref"), "profile_ref")

        created = ipc.call(socket_path, "project.create", {"name": "M1 codec test"},
                           idempotency_key="m1-create-" + str(uuid.uuid4()))
        create_data = data(created, "project.create")
        document_id = ipc.string_field(create_data, "document_id", "project.create.data")
        epoch = ipc.string_field(create_data, "document_epoch", "project.create.data")

        resources = [
            {"path": "model.bdf", "text": ROOT_DECK},
            {"path": "parts/nodes.bdf", "text": NODE_DECK},
            {"path": "parts/physics/beam.bdf", "text": BEAM_DECK},
        ]
        import_params = {
            "command": "model.import",
            "root_resource": "model.bdf",
            "resources": resources,
            "source_profile_ref": profile_ref,
            "unit_system": "mm-N-MPa",
        }

        # A rejected format import has a report and cannot change the document.
        invalid_resources = [dict(item) for item in resources]
        invalid_resources[1] = {"path": "parts/nodes.bdf", "text": NODE_DECK + "FOO,1,2\n"}
        invalid = document_call(
            socket_path,
            document_id,
            epoch,
            "changes.preview",
            {**import_params, "resources": invalid_resources},
            revision="0",
            expected_status="failed",
        )
        ipc.check(ipc.error_code(invalid, "unknown-card import") == "IMPORT_REJECTED",
                  f"unknown card should be rejected: {invalid!r}")
        report = ipc.require_mapping(invalid.get("report"), "unknown-card import report")
        issues = report.get("issues")
        ipc.check(isinstance(issues, list) and any(
            isinstance(issue, dict) and issue.get("code") == "unsupported_card" for issue in issues
        ), f"unknown-card report should identify unsupported_card: {report!r}")
        summary = data(document_call(socket_path, document_id, epoch, "model.summary"), "model.summary")
        ipc.check(summary.get("revision") == "0" and summary.get("node_count") == 0,
                  f"rejected import must leave the document unchanged: {summary!r}")

        import_preview = document_call(socket_path, document_id, epoch, "changes.preview",
                                       import_params, revision="0")
        import_data = data(import_preview, "model.import preview")
        preview_id = ipc.string_field(import_data, "preview_id", "model.import preview.data")
        import_report = ipc.require_mapping(import_data.get("import_report"), "import report")
        ipc.check(import_report.get("complete") is True,
                  f"valid nested-include import should be complete: {import_report!r}")
        commit = document_call(
            socket_path, document_id, epoch, "changes.commit", {"preview_id": preview_id},
            revision="0", key="m1-import-" + str(uuid.uuid4()),
        )
        ipc.check(commit.get("revision") == "1", f"import commit should produce revision 1: {commit!r}")

        node_rows = entities(socket_path, document_id, epoch, "node")
        beam_rows = entities(socket_path, document_id, epoch, "beam")
        material_rows = entities(socket_path, document_id, epoch, "material")
        section_rows = entities(socket_path, document_id, epoch, "section")
        include_rows = entities(socket_path, document_id, epoch, "include")
        ipc.check(len(node_rows) == 2 and len(beam_rows) == len(material_rows) == len(section_rows) == 1,
                  "valid deck should import 2 nodes and one beam/material/section")
        ipc.check(len(include_rows) == 3,
                  f"root and two nested INCLUDE resources should import: {include_rows!r}")
        node_ids = [ipc.string_field(row, "entity_id", "node row") for row in node_rows]
        beam_id = ipc.string_field(beam_rows[0], "entity_id", "beam row")
        material_id = ipc.string_field(material_rows[0], "entity_id", "material row")
        include_ids = {ipc.string_field(row, "entity_id", "include row") for row in include_rows}
        analysis_rows = entities(socket_path, document_id, epoch, "analysis")
        ipc.check(len(analysis_rows) == 1, f"one analysis should be imported: {analysis_rows!r}")
        analysis_id = ipc.string_field(analysis_rows[0], "entity_id", "analysis row")

        # Organization writes preserve IDs and populate scoped query views.
        part, rev, part_id = commit_edit(socket_path, document_id, epoch, "1", "part.upsert",
                                         {"name": "Cantilever", "members": [*node_ids, beam_id]}, "m1-part")
        assembly, rev, assembly_id = commit_edit(socket_path, document_id, epoch, rev, "assembly.upsert",
                                                 {"name": "Assembly", "children": [part_id]}, "m1-assembly")
        entity_set, rev, set_id = commit_edit(socket_path, document_id, epoch, rev, "set.upsert",
                                             {"name": "Tip nodes", "members": [node_ids[1]]}, "m1-set")
        part_view = entities(socket_path, document_id, epoch, view="part", owner_id=part_id)
        ipc.check({row.get("entity_id") for row in part_view} == set(node_ids + [beam_id]),
                  f"part view should contain its stable member IDs: {part_view!r}")
        assembly_view = entities(socket_path, document_id, epoch, view="assembly", owner_id=assembly_id)
        ipc.check({row.get("entity_id") for row in assembly_view} == set(node_ids + [beam_id, part_id]),
                  f"assembly view should resolve its part descendants: {assembly_view!r}")
        set_view = entities(socket_path, document_id, epoch, view="set", owner_id=set_id)
        ipc.check([row.get("entity_id") for row in set_view] == [node_ids[1]],
                  f"set view should contain its stable member ID: {set_view!r}")
        section_id = ipc.string_field(section_rows[0], "entity_id", "section row")
        property_view = entities(socket_path, document_id, epoch, view="property", owner_id=section_id)
        ipc.check({row.get("entity_id") for row in property_view} == set(node_ids + [beam_id]),
                  f"property view should resolve its beam and connected nodes: {property_view!r}")
        imported_root = next(row for row in include_rows if row.get("path") == "model.bdf")
        include_view = entities(socket_path, document_id, epoch, view="include",
                                owner_id=imported_root["entity_id"])
        ipc.check({row.get("entity_id") for row in include_view} >= set(node_ids + [beam_id]),
                  f"root include view should contain nested deck entities: {include_view!r}")
        beam_include = next(row for row in include_rows if row.get("path") == "parts/physics/beam.bdf")
        beam_include_view = entities(socket_path, document_id, epoch, view="include",
                                     owner_id=beam_include["entity_id"])
        beam_include_ids = {row.get("entity_id") for row in beam_include_view}
        ipc.check(beam_include_ids == {beam_id, section_id, material_id},
                  f"leaf INCLUDE view should contain its own cards: {beam_include_view!r}")
        ipc.check(not (beam_include_ids & set(node_ids)),
                  "leaf INCLUDE view must not pull GRID nodes owned by the sibling resource")
        ipc.check(include_ids <= {row.get("entity_id") for row in entities(socket_path, document_id, epoch)},
                  "INCLUDE entity IDs should remain queryable")

        refs = document_call(socket_path, document_id, epoch, "entity.references",
                             {"entity_id": material_id, "direction": "incoming"})
        ref_data = data(refs, "entity.references")
        ref_rows = ref_data.get("references")
        ipc.check(isinstance(ref_rows, list) and any(
            row.get("role") == "section.material" for row in ref_rows if isinstance(row, dict)
        ), f"material incoming references should include its section: {refs!r}")
        ipc.check(analysis_id in ref_data.get("affected_analyses", []),
                  f"material references should identify affected analysis: {refs!r}")

        # Referenced nodes and parts cannot be deleted through the edit path.
        for entity_id in (node_ids[0], part_id):
            rejected = document_call(
                socket_path, document_id, epoch, "changes.preview",
                {"command": "entity.delete", "entity_id": entity_id},
                revision=rev, expected_status="failed",
            )
            ipc.check(ipc.error_code(rejected, "referenced entity delete") == "INVALID_INPUT",
                      f"referenced entity deletion should fail: {rejected!r}")

        stale = document_call(
            socket_path, document_id, epoch, "changes.preview",
            {"command": "node.move", "entity_id": node_ids[1], "position_mm": [1000.0, 0.0, 10.0]},
            revision="1", expected_status="conflict",
        )
        ipc.check(ipc.error_code(stale, "stale node.move") == "REVISION_CONFLICT",
                  f"stale edit should fail on revision check: {stale!r}")

        moved_position = [1000.0, 0.0, 10.0]
        move_preview = document_call(
            socket_path, document_id, epoch, "changes.preview",
            {"command": "node.move", "entity_id": node_ids[1], "position_mm": moved_position},
            revision=rev,
        )
        move_data = data(move_preview, "node.move preview")
        ipc.check(analysis_id in move_data.get("affected_analyses", []),
                  f"moving an analysis node should report affected analysis: {move_data!r}")
        move_preview_id = ipc.string_field(move_data, "preview_id", "node.move preview.data")
        move_commit = document_call(
            socket_path, document_id, epoch, "changes.commit", {"preview_id": move_preview_id},
            revision=rev, key="m1-move",
        )
        rev = str(int(rev) + 1)
        ipc.check(move_commit.get("revision") == rev, f"node.move should advance revision: {move_commit!r}")
        moved = next(row for row in entities(socket_path, document_id, epoch, "node")
                     if row.get("entity_id") == node_ids[1])
        ipc.check(moved.get("position_mm") == moved_position, f"move was not applied: {moved!r}")

        undo = document_call(socket_path, document_id, epoch, "history.undo", {}, revision=rev, key="m1-undo")
        rev = str(int(rev) + 1)
        ipc.check(undo.get("revision") == rev, f"undo should advance revision: {undo!r}")
        restored = next(row for row in entities(socket_path, document_id, epoch, "node")
                        if row.get("entity_id") == node_ids[1])
        ipc.check(restored.get("position_mm") == [1000, 0, 0], f"undo did not restore position: {restored!r}")
        redo = document_call(socket_path, document_id, epoch, "history.redo", {}, revision=rev, key="m1-redo")
        rev = str(int(rev) + 1)
        ipc.check(redo.get("revision") == rev, f"redo should advance revision: {redo!r}")
        redone = next(row for row in entities(socket_path, document_id, epoch, "node")
                      if row.get("entity_id") == node_ids[1])
        ipc.check(redone.get("position_mm") == moved_position, f"redo did not restore moved position: {redone!r}")

        export = document_call(
            socket_path, document_id, epoch, "model.export_preview",
            {"analysis_id": analysis_id, "expected_profile_ref": profile_ref}, revision=rev,
        )
        export_data = data(export, "model.export_preview")
        ipc.check(export_data.get("published") is False,
                  f"preview export must be unpublished in memory: {export_data!r}")
        ipc.check(export_data.get("analysis_id") == analysis_id and
                  export_data.get("profile_ref") == profile_ref,
                  f"export must retain selected analysis/profile: {export_data!r}")
        exported_resources = export_data.get("resources")
        identities = export_data.get("identities")
        ipc.check(isinstance(exported_resources, list) and exported_resources,
                  f"export should return resource artifacts: {export_data!r}")
        ipc.check(isinstance(identities, list) and identities,
                  f"export should return stable identity mapping: {export_data!r}")
        identity_ids = {row.get("entity_id") for row in identities if isinstance(row, dict)}
        ipc.check(set(node_ids + [beam_id, material_id]) <= identity_ids,
                  f"export identity mapping should preserve imported physical IDs: {identities!r}")
        export_report = ipc.require_mapping(export_data.get("export_report"), "export report")
        ipc.check(export_report.get("complete") is True, f"export report should be complete: {export_report!r}")

        wrong_profile = dict(profile_ref)
        wrong_profile["definition_digest"] = "invalid-profile-digest"
        rejected_export = document_call(
            socket_path, document_id, epoch, "model.export_preview",
            {"analysis_id": analysis_id, "expected_profile_ref": wrong_profile},
            revision=rev, expected_status="failed",
        )
        ipc.check(ipc.error_code(rejected_export, "wrong-profile export") == "EXPORT_REJECTED",
                  f"wrong profile export should be rejected: {rejected_export!r}")
        wrong_report = ipc.require_mapping(rejected_export.get("report"), "wrong-profile export report")
        ipc.check(wrong_report.get("complete") is False,
                  f"wrong profile export report should be incomplete: {wrong_report!r}")
    finally:
        ipc.stop_process(engine)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, help="path to the local engine executable")
    parser.add_argument("--cli", required=True, help="path to the local CLI executable")
    args = parser.parse_args()
    ipc.ENGINE = os.path.abspath(args.engine)
    ipc.CLI = os.path.abspath(args.cli)
    for label, path in (("engine", ipc.ENGINE), ("cli", ipc.CLI)):
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            parser.error(f"{label} executable is missing or not executable: {path}")
    try:
        tmp = "/private/tmp" if os.path.isdir("/private/tmp") else tempfile.gettempdir()
        with tempfile.TemporaryDirectory(prefix="qcae-m1-", dir=tmp) as directory:
            root = Path(directory)
            with open(root / "engine.log", "w+", encoding="utf-8") as log:
                run(str(root / "engine.sock"), log)
    except Exception as exc:
        print(f"M1 IPC test failed: {exc}", file=sys.stderr)
        import traceback
        traceback.print_exc(file=sys.stderr)
        return 1
    print("M1 IPC black-box checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
