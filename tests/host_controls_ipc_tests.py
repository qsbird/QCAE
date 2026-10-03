#!/usr/bin/env python3
"""Host controls and outcome lookup through production SQLite IPC.

Shared snapshots include every logical SQLite row, document metadata and history.
Reads omit write context; undo/redo keep the application's original replay facts.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile

from c3_sync_ipc_tests import EngineProcess, check
from changes_commit_ipc_tests import entities, generation, preview, same_receipt, snapshot
from package_contributions_ipc_tests import sqlite_rows


CONTRACTS = (
    ("capabilities.list", "CapabilitiesListInput", "CapabilityCatalog", False, False),
    ("project.current", "ProjectCurrentInput", "DocumentInfo", False, False),
    ("project.status", "ProjectStatusInput", "ProjectStatus", True, False),
    ("model.summary", "ModelSummaryInput", "ModelSummary", True, False),
    ("history.list", "HistoryListInput", "HistorySnapshot", True, False),
    ("history.undo", "HistoryUndoInput", "ChangeReceipt", True, True),
    ("history.redo", "HistoryRedoInput", "ChangeReceipt", True, True),
)


def read_context(document):
    return {name: document[name] for name in ("document_id", "document_epoch")}


def check_catalog(catalog):
    for operation, input_type, output_type, document, write in CONTRACTS:
        entries = [item for item in catalog["operations"] if item["name"] == operation]
        check(len(entries) == 1, f"Expected one descriptor for {operation}")
        item = entries[0]
        check(item["available"] and item["version"] == 1 and
              item["schema_id"] == f"qcae.operation.{operation}.v1" and
              item["wire_input_type"] == input_type and item["wire_output_type"] == output_type and
              item["requested_version_field"] == "requested_version" and
              item["omitted_version_policy"] == "installed_version" and item["fields"] == [] and
              item["effect"] == ("model_write" if write else "query") and
              item["requires_document"] == document and item["requires_epoch"] == document and
              item["requires_revision"] == write and item["requires_idempotency_key"] == write and
              item["requires_profile_match"] is False,
              f"Generated contract or existing authority differs for {operation}: {item}")
        check(item["parameters_schema"] == {"type": "object", "properties": {},
              "required": [], "additionalProperties": False},
              f"Empty server input and discovered schema differ for {operation}")
    lookup = [item for item in catalog["operations"] if item["name"] == "operations.get"]
    check(len(lookup) == 1, "Expected one outcome lookup descriptor")
    item = lookup[0]
    check(item["available"] and item["version"] == 1 and
          item["schema_id"] == "qcae.operation.operations.get.v1" and
          item["wire_input_type"] == "OperationsGetInput" and
          item["wire_output_types"] == ["DocumentInfo", "ChangeReceipt"] and
          item["output_by_lookup_scope"] == {"host": "DocumentInfo", "document": "ChangeReceipt"} and
          all(item[name] is False for name in ("requires_document", "requires_epoch",
              "requires_revision", "requires_idempotency_key", "requires_profile_match")),
          "Outcome discovery must retain conditional context and existing output types")
    parameters = item["parameters_schema"]
    arguments = item["arguments_schema"]
    check(parameters["additionalProperties"] is False and len(parameters["oneOf"]) == 3 and
          parameters["properties"]["original_mode"] == {} and
          arguments["additionalProperties"] is False and arguments["required"] == ["parameters"] and
          arguments["properties"]["parameters"] == parameters and len(arguments["oneOf"]) == 2 and
          {tuple(branch["required"]) for branch in arguments["oneOf"]} == {
              ("parameters",), ("parameters", "document_id", "document_epoch")},
          "Lookup discovery must describe raw modes and branch-specific document context")


def run(args):
    evidence = None
    if args.evidence_dir:
        parent = Path(args.evidence_dir).resolve()
        parent.mkdir(parents=True, exist_ok=True)
        evidence = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    observations = {"scope": "Actual host controls, outcome lookup and durable history; no GUI or external AI",
                    "refusals": [], "reads": [], "replays": [], "history_writes": [],
                    "lookup_reads": [], "lookup_refusals": [],
                    "engine_exit_codes": []}
    transcripts = []
    with tempfile.TemporaryDirectory(prefix="qhc-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        workspace = root / "work.sqlite"
        with (root / "engine.log").open("w+") as log:
            engine = EngineProcess(args.engine, str(root / "engine.sock"), str(workspace), log)
            try:
                catalog = engine.start()
                client = engine.client
                check(catalog["durable"] and catalog["storage_mode"] == "sqlite",
                      "Host contract evidence requires the production SQLite workspace")
                check_catalog(catalog)
                check(client.call("capabilities.list", extra={"requested_version": 1}) == catalog,
                      "Explicit v1 changed the global catalog")
                for version in ({}, {"requested_version": 1}):
                    absent = client.call("project.current", extra=version, expected=None)
                    check(absent["error"]["code"] == "DOCUMENT_NOT_FOUND",
                          "Current requires no document context and retains the no-active result")

                document = client.call("project.create", {"name": "Host controls"}, key="create")
                created = document
                line = client.call("geometry.create_line", {
                    "start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]}, document, "line")
                commit_context, parameters = preview(client, "Saved material")
                committed = client.call("changes.commit", parameters, commit_context, "material")
                saved = client.call("project.save_as", {"path": str(root / "saved.qcae")},
                                    client.current(), "save")
                initial = snapshot(client, workspace)
                initial_entities = entities(client)
                check(not initial["document"]["dirty"] and initial["history"]["cursor"] == 2 and
                      len(initial["history"]["items"]) == 2,
                      "Need a saved model and two real history entries")

                def refuse(operation, parameters=None, extra=None, code="INVALID_INPUT",
                           field=None, context=None, key=None, bucket="refusals"):
                    before = snapshot(client, workspace)
                    response = client.call(operation, parameters, context, key,
                                           extra=extra, expected=None)
                    request = client.transcript[-1]["request"]
                    check(response.get("status") in {"failed", "conflict", "needs_input"} and
                          response["error"]["code"] == code, f"Wrong refusal: {response}")
                    if field:
                        check(response["error"].get("field") == field, f"Wrong field: {response}")
                    check(snapshot(client, workspace) == before,
                          "Refusal changed complete SQLite rows, document or history")
                    observations[bucket].append({"request": request, "response": response,
                                                "complete_state_unchanged": True})

                def lookup(scope, operation, key, original, document=None, extra=None,
                           mode=None, include_mode=False):
                    before = snapshot(client, workspace)
                    parameters = {"lookup_scope": scope, "original_operation": operation,
                                  "idempotency_key": key}
                    if include_mode:
                        parameters["original_mode"] = mode
                    envelope = read_context(document) if document else {}
                    envelope.update(extra or {})
                    result = client.call("operations.get", parameters, extra=envelope)
                    frame = client.transcript[-1]
                    if scope == "host":
                        check(result == original, "Host lookup changed its retained DocumentInfo")
                    else:
                        same_receipt(original, result, before["document"])
                    mapped = scope == "document" and operation in {
                        "changes.commit", "history.undo", "history.redo"}
                    check(("revision" in frame["response"]) == mapped and
                          snapshot(client, workspace) == before,
                          "Lookup changed its existing envelope or complete authoritative state")
                    observations["lookup_reads"].append({**frame, "complete_state_unchanged": True})

                for version in ({}, {"requested_version": 1}):
                    lookup("host", "project.create", "create", created, extra=version)
                    lookup("host", "project.save_as", "save", saved, extra=version)
                    lookup("document", "geometry.create_line", "line", line, saved, version)
                    lookup("document", "changes.commit", "material", committed, saved, version)
                ignored_context = {"document_id": None, "document_epoch": {"ignored": None},
                                   "expected_revision": [], "idempotency_key": False,
                                   "requested_version": 1}
                ignored_modes = ("recover", "", 7, False, None, [], [None], {"nested": None})
                for mode in ignored_modes:
                    lookup("host", "project.create", "create", created, extra=ignored_context,
                           mode=mode, include_mode=True)
                    lookup("document", "geometry.create_line", "line", line, saved,
                           {"requested_version": 1, "expected_revision": None,
                            "idempotency_key": {"ignored": None}}, mode, True)
                valid_lookup = {"lookup_scope": "document", "original_operation": "changes.commit",
                                "idempotency_key": "material"}
                for bad in (True, "1", None, 0, -1, 1.5, 4294967296, 2, 4294967295):
                    refuse("operations.get", {"lookup_scope": "invalid"},
                           {"requested_version": bad}, code=("SCHEMA_UNSUPPORTED"
                           if type(bad) is int and bad in (2, 4294967295) else "INVALID_INPUT"),
                           field="requested_version", bucket="lookup_refusals")
                for field in ("lookup_scope", "original_operation", "idempotency_key"):
                    missing = {name: value for name, value in valid_lookup.items() if name != field}
                    for lookup_parameters in (missing, {**valid_lookup, field: ""},
                                              {**valid_lookup, field: None}, {**valid_lookup, field: 7}):
                        refuse("operations.get", lookup_parameters, read_context(saved),
                               bucket="lookup_refusals")
                refuse("operations.get", {**valid_lookup, "extra": None}, read_context(saved),
                       bucket="lookup_refusals")
                refuse("operations.get", valid_lookup, bucket="lookup_refusals")
                refuse("operations.get", valid_lookup, {**read_context(saved), "document_epoch": "old"},
                       code="DOCUMENT_EPOCH_EXPIRED", bucket="lookup_refusals")
                for mode in ("invalid", "", None, False, 7, [], {"nested": None}):
                    refuse("operations.get", {"lookup_scope": "host", "original_operation": "project.open",
                           "idempotency_key": "not-recorded", "original_mode": mode},
                           {"requested_version": 1}, bucket="lookup_refusals")

                for operation, _, _, needs_document, write in CONTRACTS:
                    context = initial["document"] if write else None
                    base = read_context(initial["document"]) if needs_document and not write else {}
                    key = operation + "-after-refusal" if write else None
                    for bad in (True, "1", None, 0, -1, 1.5, 4294967296):
                        refuse(operation, extra={**base, "requested_version": bad},
                               field="requested_version", context=context, key=key)
                    for future in (2, 4294967295):
                        refuse(operation, extra={**base, "requested_version": future},
                               code="SCHEMA_UNSUPPORTED", field="requested_version",
                               context=context, key=key)
                    for value in ("unknown", 7, None):
                        refuse(operation, {"extra": value}, {**base, "requested_version": 1},
                               field="input.extra", context=context, key=key)
                    if not write:
                        omitted = client.call(operation, extra=base)
                        explicit = client.call(operation, extra={**base, "requested_version": 1})
                        check(omitted == explicit and snapshot(client, workspace) == initial,
                              "Read v1 changed its payload or complete authoritative state")
                        observations["reads"].append({"operation": operation, "result": explicit,
                                                      "complete_state_unchanged": True})
                        if needs_document:
                            refuse(operation, extra={**base, "document_epoch": "expired"},
                                   code="DOCUMENT_EPOCH_EXPIRED")
                            refuse(operation, extra={"requested_version": 1})

                def move(operation, key, version):
                    before = snapshot(client, workspace)
                    result = client.call(operation, context=before["document"], key=key,
                                         extra=version)
                    after = snapshot(client, workspace)
                    delta = -1 if operation == "history.undo" else 1
                    check(not result["replayed"] and generation(after) == generation(before) + 1 and
                          int(after["document"]["revision"]) == int(before["document"]["revision"]) + 1 and
                          after["history"]["cursor"] == before["history"]["cursor"] + delta and
                          [{key: value for key, value in item.items() if key != "applied"}
                           for item in after["history"]["items"]] ==
                          [{key: value for key, value in item.items() if key != "applied"}
                           for item in before["history"]["items"]] and
                          after["document"]["saved_content_state"] == initial["document"]["saved_content_state"] and
                          result["current_content_state"] == after["document"]["content_state"],
                          "History write must publish once and preserve saved identity and entries")
                    observations["history_writes"].append(result)
                    return before["document"], result, after

                refuse("history.redo", context=initial["document"], key="history.redo-after-refusal",
                       code="NOTHING_TO_REDO")
                undo_context, undone, after_undo = move("history.undo", "history.undo-after-refusal",
                                                       {"requested_version": 1})
                check(after_undo["document"]["dirty"] and committed["entity_id"] not in
                      {item["entity"]["entity_id"] for item in entities(client)},
                      "Undo must remove the material and mark the saved document dirty")
                redo_context, redone, after_redo = move("history.redo", "history.redo-after-refusal", {})
                check(not after_redo["document"]["dirty"] and entities(client) == initial_entities,
                      "Redo must restore the saved content state")
                facts = (("history.undo", "history.undo-after-refusal", undo_context, undone),
                         ("history.redo", "history.redo-after-refusal", redo_context, redone),
                         ("changes.commit", "material", commit_context, committed))
                for operation, key, context, original in facts:
                    for version in ({}, {"requested_version": 1}):
                        result = client.call(operation, parameters if operation == "changes.commit" else {},
                                             context, key, extra=version)
                        same_receipt(original, result, after_redo["document"])
                        check(snapshot(client, workspace) == after_redo,
                              "Old undo/redo/commit replay changed the redone model or persistent cursor")
                        observations["replays"].append(result)
                    fact = client.call("operations.get", {"lookup_scope": "document",
                        "original_operation": operation, "idempotency_key": key}, after_redo["document"])
                    same_receipt(original, fact, after_redo["document"])
                    check(snapshot(client, workspace) == after_redo, "Outcome lookup changed state")
                    lookup("document", operation, key, original, after_redo["document"],
                           {"requested_version": 1}, {"ignored": None}, True)
                refuse("operations.get", {"lookup_scope": "document",
                    "original_operation": "history.undo", "idempotency_key": "unrecorded"},
                    context=after_redo["document"], code="ENTITY_NOT_FOUND")
                refuse("history.undo", context=after_redo["document"], key="history.undo-after-refusal",
                       code="IDEMPOTENCY_KEY_CONFLICT")
                for operation in ("history.undo", "history.redo"):
                    refuse(operation, context=initial["document"], key="stale-" + operation,
                           code="REVISION_CONFLICT")
                move("history.undo", "omitted-undo", {})
                move("history.redo", "explicit-redo", {"requested_version": 1})
                for index in range(2):
                    move("history.undo", "exhaust-" + str(index), {"requested_version": 1})
                refuse("history.undo", context=client.current(), key="restore-0", code="NOTHING_TO_UNDO")
                for index in range(2):
                    move("history.redo", "restore-" + str(index), {})

                final = snapshot(client, workspace)
                final_entities = entities(client)
                observations["initial_state"] = initial
                observations["final_state"] = final
                process = engine.process
                engine.close()
                observations["engine_exit_codes"].append(process.returncode)
                transcripts.append(client.transcript)
                engine = EngineProcess(args.engine, str(root / "engine.sock"), str(workspace), log)
                engine.start()
                client = engine.client
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                check(recovered["document_id"] == final["document"]["document_id"] and
                      recovered["document_epoch"] != final["document"]["document_epoch"] and
                      recovered["revision"] == final["document"]["revision"] and
                      client.call("history.list", extra=read_context(recovered)) == final["history"] and
                      entities(client) == final_entities,
                      "Independent recovery lost stable records or history")
                recovered_state = snapshot(client, workspace)
                lookup("host", "project.open", "recover", recovered,
                       extra={"requested_version": 1}, mode="recover", include_mode=True)
                for operation, key, context, original in facts:
                    fact = client.call("operations.get", {"lookup_scope": "document",
                        "original_operation": operation, "idempotency_key": key}, recovered)
                    same_receipt(original, fact, recovered)
                    check(snapshot(client, workspace) == recovered_state, "Recovered fact read mutated state")
                    lookup("document", operation, key, original, recovered,
                           {"requested_version": 1}, None, True)
                    if operation != "changes.commit":
                        refuse(operation, context=context, key=key, code="DOCUMENT_EPOCH_EXPIRED")
                for operation in ("project.status", "model.summary", "history.list"):
                    refuse(operation, extra=read_context(final["document"]), code="DOCUMENT_EPOCH_EXPIRED")
                observations["recovered_state"] = recovered_state
                observations["recovered_entities"] = final_entities
                closed = client.call("project.close", {"policy": "discard"}, recovered, "close")
                closed_rows = sqlite_rows(workspace)
                for version in ({}, {"requested_version": 1}):
                    outcome = client.call("operations.get", {"lookup_scope": "host",
                        "original_operation": "project.close", "idempotency_key": "close",
                        "original_mode": {"ignored": None}}, extra=version)
                    frame = client.transcript[-1]
                    check(outcome == closed and "revision" not in frame["response"] and
                          sqlite_rows(workspace) == closed_rows,
                          "Host lookup after close needs no active document and must not write")
                    observations["lookup_reads"].append({**frame, "complete_state_unchanged": True})
                absent = client.call("project.current", expected=None)
                check(absent["error"]["code"] == "DOCUMENT_NOT_FOUND", "Close retained an active document")
                opened = client.call("project.open", {"mode": "normal", "path": str(root / "saved.qcae")},
                                     key="normal-open")
                check(opened["document_id"] != recovered["document_id"] and
                      opened["document_epoch"] != recovered["document_epoch"] and
                      entities(client) == initial_entities,
                      "Normal open must import the saved model into a fresh document identity")
                lookup("host", "project.open", "normal-open", opened)
                lookup("host", "project.open", "normal-open", opened,
                       extra={"requested_version": 1}, mode="normal", include_mode=True)
                observations["normal_open_state"] = snapshot(client, workspace)
            finally:
                if engine.process:
                    process = engine.process
                    engine.close()
                    observations["engine_exit_codes"].append(process.returncode)
                    transcripts.append(engine.client.transcript)
                if evidence:
                    log.flush()
                    log.seek(0)
                    (evidence / "engine.log").write_text(log.read())
                    (evidence / "observations.json").write_text(json.dumps(observations, indent=2) + "\n")
                    (evidence / "transcripts.json").write_text(json.dumps(transcripts, indent=2) + "\n")
    print("PASS: SQLite host contracts and outcome lookup, atomic reads, retained facts and recovery")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--evidence-dir")
    run(parser.parse_args())
