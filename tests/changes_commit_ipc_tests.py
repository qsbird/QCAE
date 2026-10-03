#!/usr/bin/env python3
"""Real SQLite commit-contract refusals, replay, history and explicit recovery.

The transport calls the existing preview/commit coordinator. Full logical SQLite
rows, including generation, are compared around every refusal and replay.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile

from c3_sync_ipc_tests import EngineProcess, check
from package_contributions_ipc_tests import sqlite_rows


def snapshot(client, workspace):
    document = client.current()
    return {"document": document, "history": client.call("history.list", context=document),
            "sqlite": sqlite_rows(workspace)}


def generation(state):
    table = state["sqlite"]["record_state"]
    check(len(table["rows"]) == 1, "Expected one actual record workspace")
    return table["rows"][0][table["columns"].index("generation")]


def same_receipt(original, replay, document):
    check(replay["replayed"] and replay["transaction_id"] == original["transaction_id"] and
          replay["entity_id"] == original["entity_id"] and
          replay["committed_revision"] == original["committed_revision"] and
          replay["current_revision"] == document["revision"] and
          replay["current_content_state"] == document["content_state"],
          "Commit replay lost its original outcome or current authority")


def preview(client, name):
    context = client.current()
    result = client.call("changes.preview", {"command": "material.create", "name": name,
        "young_modulus": {"value": 210, "unit": "GPa"}}, context)
    return context, {"preview_id": result["preview_id"]}


def entities(client):
    context = client.current()
    result = client.call("entity.query", {}, context)
    rows = result["entities"]
    check(len(rows) == result["total"] and len({row["entity_id"] for row in rows}) == len(rows),
          "Recovery comparison requires all entities and unique stable IDs")
    # Storage positions and iteration order can change during recovery; identity cannot.
    rows.sort(key=lambda row: row["entity_id"])
    return [{"entity": row, "fields": client.call("entity.fields", {
        "entity_id": row["entity_id"]}, context)["fields"]} for row in rows]


def run(args):
    evidence = None
    if args.evidence_dir:
        parent = Path(args.evidence_dir).resolve()
        parent.mkdir(parents=True, exist_ok=True)
        evidence = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    observations = {"scope": "actual SQLite commit contract; no GUI or solver execution",
                    "refusals": [], "replays": [], "engine_exit_codes": []}
    transcripts = []
    with tempfile.TemporaryDirectory(prefix="qcc-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        workspace = root / "work.sqlite"
        with (root / "engine.log").open("w+") as log:
            engine = EngineProcess(args.engine, str(root / "engine.sock"), str(workspace), log)
            try:
                caps = engine.start()
                client = engine.client
                check(caps["durable"] and caps["storage_mode"] == "sqlite",
                      "Commit contract requires the actual SQLite host")
                descriptor = next(item for item in caps["operations"]
                                  if item["name"] == "changes.commit")
                check(descriptor["available"] and descriptor["version"] == 1 and
                      descriptor["requested_version_field"] == "requested_version" and
                      descriptor["omitted_version_policy"] == "installed_version" and
                      descriptor["schema_id"] == "qcae.operation.changes.commit.v1" and
                      descriptor["wire_input_type"] == "ChangesCommitInput",
                      "Installed commit descriptor lost generated identity")
                schema = descriptor["parameters_schema"]
                check(schema["type"] == "object" and schema["additionalProperties"] is False and
                      schema["required"] == ["preview_id"] and
                      schema["properties"] == {"preview_id": {"type": "string", "minLength": 1}},
                      "Commit discovery differs from the actual non-empty input")
                observations["descriptor"] = descriptor
                context = client.call("project.create", {"name": "Commit contract"}, key="create")
                client.call("geometry.create_line", {
                    "start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]}, context, "unrelated-line")
                for name in ("Existing", "Redo tail"):
                    client.call("material.create", {"name": name,
                        "young_modulus": {"value": 200, "unit": "GPa"}},
                        client.current(), name)
                client.call("history.undo", context=client.current(), key="prepare-redo")
                initial = snapshot(client, workspace)
                check(initial["history"]["cursor"] == 2 and
                      len(initial["history"]["items"]) == 3, "Missing real redo tail")
                context, parameters = preview(client, "Validated commit")
                before = snapshot(client, workspace)
                check(before == initial, "Preview modified persisted state or history")

                def refuse(parameters, extra, code, field=None, ctx=context, key="commit-after-refusal"):
                    before_refusal = snapshot(client, workspace)
                    response = client.call("changes.commit", parameters, ctx, key,
                                           extra=extra, expected=None)
                    request = client.transcript[-1]["request"]
                    check(response.get("status") in {"failed", "conflict"} and
                          response["error"]["code"] == code, f"Wrong refusal: {response}")
                    if field:
                        check(response["error"].get("field") == field,
                              f"Wrong diagnostic field: {response}")
                    after_refusal = snapshot(client, workspace)
                    check(after_refusal == before_refusal,
                          "Refusal changed complete SQLite rows, history or document")
                    observations["refusals"].append({"request": request,
                        "response": response, "complete_state_unchanged": True})

                for bad in (True, "1", None, 0, -1, 1.5, 4294967296):
                    refuse(parameters, {"requested_version": bad}, "INVALID_INPUT", "requested_version")
                refuse(parameters, {"requested_version": 2}, "SCHEMA_UNSUPPORTED", "requested_version")
                for bad in ({}, {"preview_id": ""}, {"preview_id": None}, {"preview_id": 1},
                            {"preview_id": []}, {"preview_id": parameters["preview_id"], "extra": "unknown"}):
                    field = "input.extra" if "extra" in bad else "input.preview_id"
                    refuse(bad, {"requested_version": 1}, "INVALID_INPUT", field)

                committed = client.call("changes.commit", parameters, context,
                    "commit-after-refusal", extra={"requested_version": 1})
                after_commit = snapshot(client, workspace)
                check(not committed["replayed"] and
                      int(after_commit["document"]["revision"]) == int(context["revision"]) + 1 and
                      generation(after_commit) == generation(before) + 1 and
                      after_commit["history"]["cursor"] == 3 and
                      len(after_commit["history"]["items"]) == 3 and
                      after_commit["history"]["items"][:2] == before["history"]["items"][:2] and
                      after_commit["history"]["items"][-1]["transaction_id"] == committed["transaction_id"] and
                      committed["current_content_state"] == after_commit["document"]["content_state"],
                      "Valid same-key commit must publish once and truncate only the redo tail")

                def replay(extra, current_state):
                    result = client.call("changes.commit", parameters, context,
                                         "commit-after-refusal", extra=extra)
                    same_receipt(committed, result, current_state["document"])
                    check(snapshot(client, workspace) == current_state,
                          "Replay changed complete persisted state")
                    observations["replays"].append(result)

                replay(None, after_commit)
                replay({"requested_version": 1}, after_commit)
                other_context, other_parameters = preview(client, "Other preview")
                refuse(other_parameters, {"requested_version": 1}, "IDEMPOTENCY_KEY_CONFLICT",
                       "idempotency_key", other_context)
                client.call("history.undo", context=client.current(), key="undo-commit")
                after_undo = snapshot(client, workspace)
                check(after_undo["history"]["cursor"] == 2 and
                      committed["entity_id"] not in {row["entity"]["entity_id"] for row in entities(client)},
                      "Undo did not remove the committed material")
                replay(None, after_undo)
                replay({"requested_version": 1}, after_undo)

                omitted_context, omitted_parameters = preview(client, "Omitted version")
                refuse(omitted_parameters, {"requested_version": 1}, "REVISION_CONFLICT",
                       ctx=context, key="omitted-after-refusal")
                refuse(omitted_parameters, {"requested_version": 1}, "DOCUMENT_EPOCH_EXPIRED",
                       ctx={**omitted_context, "document_epoch": "expired"}, key="omitted-after-refusal")
                before_omitted = snapshot(client, workspace)
                omitted = client.call("changes.commit", omitted_parameters, omitted_context,
                                      "omitted-after-refusal")
                final = snapshot(client, workspace)
                check(not omitted["replayed"] and generation(final) == generation(before_omitted) + 1 and
                      int(final["document"]["revision"]) == int(omitted_context["revision"]) + 1,
                      "Omitted version changed the authoritative commit path")
                explicit_replay = client.call("changes.commit", omitted_parameters, omitted_context,
                    "omitted-after-refusal", extra={"requested_version": 1})
                same_receipt(omitted, explicit_replay, final["document"])
                check(snapshot(client, workspace) == final, "Omitted/v1 replay published twice")
                observations["omitted_commit"] = omitted
                observations["initial_state"] = before
                observations["final_state"] = final
                final_entities = entities(client)
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
                      entities(client) == final_entities and
                      client.call("history.list", context=recovered) == final["history"],
                      "Explicit process recovery lost records or history")
                recovered_state = snapshot(client, workspace)
                for key, original in (("commit-after-refusal", committed), ("omitted-after-refusal", omitted)):
                    fact = client.call("operations.get", {"lookup_scope": "document",
                        "original_operation": "changes.commit", "idempotency_key": key}, recovered)
                    same_receipt(original, fact, recovered)
                    check(snapshot(client, workspace) == recovered_state,
                          "Persistent receipt lookup mutated recovered state")
                refuse(parameters, {"requested_version": 1}, "DOCUMENT_EPOCH_EXPIRED", ctx=context)
                observations["recovered_state"] = recovered_state
                observations["recovered_entities"] = final_entities
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
    print("PASS: actual SQLite commit schema/version refusals, one commit, replay, undo and recovery")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--evidence-dir")
    run(parser.parse_args())
