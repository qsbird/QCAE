#!/usr/bin/env python3
"""NEXT-01 real engine/CLI record queries and M1 organization compatibility."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

from c2_workflow_tests import Client
from changes_commit_ipc_tests import snapshot
from m1_ipc_tests import ROOT_DECK, NODE_DECK, BEAM_DECK


@contextmanager
def engine_client(engine, cli, root, evidence):
    endpoint = str(root / "engine.sock")
    client = Client("cli", cli, endpoint)
    with (root / "engine.log").open("w+") as log:
        process = subprocess.Popen(
            [engine, "--socket", endpoint, "--workspace", str(root / "work.sqlite")],
            stdout=log, stderr=log,
        )
        try:
            deadline = time.monotonic() + 15
            while not Path(endpoint).exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    log.seek(0)
                    raise AssertionError(f"Engine startup failed: {log.read()}")
                time.sleep(.02)
            yield client
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            if evidence:
                evidence.mkdir(parents=True, exist_ok=True)
                (evidence / f"{root.name}-transcript.json").write_text(
                    json.dumps(client.transcript, indent=2) + "\n")
                (evidence / f"{root.name}-contract-observations.json").write_text(
                    json.dumps(getattr(client, "contract_observations", []), indent=2) + "\n")
                if hasattr(client, "contract_initial_state"):
                    (evidence / f"{root.name}-authority-initial.json").write_text(
                        json.dumps(client.contract_initial_state, indent=2) + "\n")


def contract_call(client, operation, parameters, document, extra=None, expected="success"):
    """A thin CLI request with only the read context; application validation stays in engine."""
    client.sequence += 1
    request = {"api_version": "1.1", "request_id": f"entity-contract-{client.sequence}",
               "operation": operation, "parameters": parameters,
               "document_id": document["document_id"], "document_epoch": document["document_epoch"]}
    request.update(extra or {})
    argv = [client.cli, "--socket", client.endpoint, "--no-start"]
    process = subprocess.run(argv, input=json.dumps(request), text=True, capture_output=True, timeout=15)
    assert process.stdout, process.stderr
    response = json.loads(process.stdout)
    assert response["request_id"] == request["request_id"] and response["status"] == expected, response
    assert process.returncode == (0 if expected == "success" else 2), (response, process.stderr)
    client.transcript.append({"request": request, "response": response,
                              "actual_exit_code": process.returncode, "argv": argv})
    return response


def contract_pair(client, workspace, operation, parameters, expected="success", extra=None):
    before = snapshot(client, workspace)
    omitted = contract_call(client, operation, parameters, before["document"], extra, expected)
    explicit = contract_call(client, operation, parameters, before["document"],
                             {**(extra or {}), "requested_version": 1}, expected)
    assert {key: value for key, value in omitted.items() if key != "request_id"} == {
        key: value for key, value in explicit.items() if key != "request_id"}, (omitted, explicit)
    after = snapshot(client, workspace)
    assert after == before, "Read/refusal changed complete SQLite authority"
    observations = getattr(client, "contract_observations", [])
    observations.append({"operation": operation, "parameters": parameters, "omitted": omitted,
                         "explicit": explicit, "complete_state_unchanged": True,
                         "authority_before_sha256": state_digest(before),
                         "authority_after_sha256": state_digest(after)})
    client.contract_observations = observations
    return explicit


def state_digest(state):
    return hashlib.sha256(json.dumps(state, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def read_contracts(client, workspace, entity):
    client.contract_initial_state = snapshot(client, workspace)
    assert not client.contract_initial_state["document"]["dirty"]
    assert client.contract_initial_state["document"]["saved_content_state"]
    catalog = client.call("capabilities.list")
    for operation, input_type, output_type in (
            ("entity.query", "EntityQueryInput", "SelectionResult"),
            ("entity.references", "EntityReferencesInput", "ReferenceResult")):
        entries = [item for item in catalog["operations"] if item["name"] == operation]
        assert len(entries) == 1
        descriptor = entries[0]
        assert descriptor["available"] and descriptor["version"] == 1
        assert descriptor["schema_id"] == f"qcae.operation.{operation}.v1"
        assert descriptor["wire_input_type"] == input_type and descriptor["wire_output_type"] == output_type
        assert descriptor["requires_document"] and descriptor["requires_epoch"]
        assert all(descriptor[name] is False for name in (
            "requires_revision", "requires_idempotency_key", "requires_profile_match"))
        schema = descriptor["arguments_schema"]
        required = {"document_id", "document_epoch"}
        if operation == "entity.references":
            required.add("parameters")
        assert set(schema["required"]) == required and schema["additionalProperties"] is False
        assert schema["properties"]["parameters"] == descriptor["parameters_schema"]
        assert schema["properties"]["expected_revision"] == schema["properties"]["idempotency_key"] == {}
    for parameters in ({}, {"ids": []}, {"ids": [entity, entity, "unknown", " "]},
                       {"name_contains": ""}, {"offset": 0, "limit": 0},
                       {"offset": -0.0, "limit": 1000}, {"offset": 100000.0}):
        contract_pair(client, workspace, "entity.query", parameters,
                      extra={"expected_revision": None, "idempotency_key": {"ignored": None}})
    duplicated = contract_pair(client, workspace, "entity.query", {"ids": [entity, entity]})
    unique = contract_pair(client, workspace, "entity.query", {"ids": [entity]})
    assert duplicated["data"] == unique["data"]
    for parameters in ({"entity_id": entity}, {"entity_id": entity, "direction": "incoming"},
                       {"entity_id": entity, "direction": "outgoing"}):
        contract_pair(client, workspace, "entity.references", parameters,
                      extra={"expected_revision": {"ignored": None}, "idempotency_key": []})
    invalid = {
        "entity.query": ({"extra": None}, {"kind": None}, {"kind": "\t"},
                         {"name_contains": 7}, {"ids": None}, {"ids": [None]}, {"ids": [""]},
                         {"view": ""}, {"owner_id": None}, {"offset": .5}, {"offset": -1},
                         {"offset": 100001}, {"limit": True}, {"limit": 1001}),
        "entity.references": ({}, {"entity_id": None}, {"entity_id": " "},
                              {"entity_id": entity, "direction": None},
                              {"entity_id": entity, "direction": "both"},
                              {"entity_id": entity, "extra": {"nested": None}}),
    }
    for operation, parameters in (("entity.query", {"kind": None}),
                                  ("entity.references", {"entity_id": None})):
        for bad in (True, "1", None, 0, -1, .5, 4294967296, 2, 4294967295):
            before = snapshot(client, workspace)
            response = contract_call(client, operation, parameters, before["document"],
                                     {"document_epoch": "old", "requested_version": bad}, "failed")
            code = "SCHEMA_UNSUPPORTED" if type(bad) is int and bad in (2, 4294967295) else "INVALID_INPUT"
            assert response["error"]["code"] == code and response["error"]["field"] == "requested_version"
            after = snapshot(client, workspace)
            assert after == before
            client.contract_observations.append({"operation": operation, "response": response,
                                                "complete_state_unchanged": True,
                                                "authority_before_sha256": state_digest(before),
                                                "authority_after_sha256": state_digest(after)})
        expired = contract_pair(client, workspace, operation, parameters, "conflict",
                                {"document_epoch": "old"})
        assert expired["error"]["code"] == "DOCUMENT_EPOCH_EXPIRED"
        for parameters in invalid[operation]:
            rejected = contract_pair(client, workspace, operation, parameters, "failed")
            assert rejected["error"]["code"] == "INVALID_INPUT"


def query(client, **parameters):
    return client.call("entity.query", parameters, client.current())


def identities(rows):
    return {row["entity_id"] for row in rows}


def references(client, entity, direction):
    return client.call("entity.references", {"entity_id": entity, "direction": direction},
                       client.current())["references"]


def check_fields(client, row):
    fields = client.fields(row["entity_id"])
    for name, value in fields.items():
        if name in {"position", "start", "end"}:
            name += "_mm"
        elif name in {"material", "section", "mesh", "geometry", "parent", "node"}:
            name += "_id"
        assert row[name] == value, (row, name, value)
    assert row["sources"] is not None


def generate_mesh(client, line, segments, key):
    task = client.call("mesh.generate_line", {"geometry_id": line, "segments": segments},
                       client.current(), key)
    deadline = time.monotonic() + 15
    while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
        assert time.monotonic() < deadline, task
        time.sleep(.01)
        task = client.call("task.status", {"task_id": task["task_id"]}, client.current())
    assert task["state"] == "succeeded", task


def geometry_queries(client):
    doc = client.call("project.create", {"name": "Record query regressions"}, key="new")
    line = client.call("geometry.create_line", {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]},
                       doc, "line")["entity_id"]
    # The three original C2 counterexamples must now return the same entity and fields.
    native = query(client, kind="geometry")
    unfiltered = query(client)
    filtered = query(client, kind="geometry", name_contains="")
    assert native["total"] == unfiltered["total"] == filtered["total"] == 1
    assert native["entities"] == unfiltered["entities"] == filtered["entities"]
    assert native["entities"][0]["entity_id"] == line
    check_fields(client, native["entities"][0])
    assert references(client, line, "incoming") == references(client, line, "outgoing") == []

    generate_mesh(client, line, 3, "mesh")
    all_rows = query(client)["entities"]
    assert len(all_rows) == 9  # Geometry, Mesh, four Nodes, three Beams.
    for kind in ("node", "beam", "geometry", "mesh"):
        rows = query(client, kind=kind)["entities"]
        assert rows == query(client, kind=kind, name_contains="")["entities"]
        assert identities(rows) == {row["entity_id"] for row in all_rows if row["kind"] == kind}
        for row in rows:
            assert query(client, ids=[row["entity_id"]])["entities"] == [row]
            assert query(client, kind=kind, ids=[row["entity_id"], "unknown"])["entities"] == [row]
            check_fields(client, row)
    mesh = query(client, kind="mesh")["entities"][0]
    client.call("project.save_as", {"path": str(Path(client.endpoint).parent / "read-contracts.qcae")},
                client.current(), "save-before-read-contracts")
    read_contracts(client, Path(client.endpoint).parent / "work.sqlite", line)
    assert query(client, name_contains=mesh["name"])["entities"] == [mesh]
    assert query(client, kind="geometry", name_contains="missing")["total"] == 0
    assert query(client, ids=[])["total"] == query(client, ids=["unknown"])["total"] == 0
    assert query(client, limit=0)["total"] == 9 and query(client, limit=0)["entities"] == []
    pages = []
    for offset in range(0, 10, 2):
        page = query(client, offset=offset, limit=2)
        assert page["total"] == 9 and page["offset"] == offset and page["limit"] == 2
        pages.extend(page["entities"])
    assert pages == all_rows and len(identities(pages)) == 9
    assert query(client, offset=100000)["entities"] == []
    assert references(client, line, "incoming") == [
        {"from": mesh["entity_id"], "to": line, "role": "mesh.geometry"}]
    assert references(client, mesh["entity_id"], "outgoing") == [
        {"from": mesh["entity_id"], "to": line, "role": "mesh.geometry"}]
    incoming = references(client, mesh["entity_id"], "incoming")
    assert len(incoming) == 7 and {ref["role"] for ref in incoming} == {"node.mesh", "beam.mesh"}
    node = query(client, kind="node")["entities"][0]
    assert references(client, node["entity_id"], "outgoing") == [
        {"from": node["entity_id"], "to": mesh["entity_id"], "role": "node.mesh"}]
    before = client.current()
    for params in ({"kind": "unknown"}, {"kind": "unknown", "name_contains": ""},
                   {"where": {}}, {"owner_id": line}, {"view": "spatial", "owner_id": line},
                   {"view": "part", "owner_id": line}, {"limit": 1001}, {"offset": -1},
                   {"offset": .5}):
        response = client.call("entity.query", params, before, expected="failed")
        assert response["error"]["code"] == "INVALID_INPUT", response
    for operation, params in (
            ("entity.query", {"view": "part", "owner_id": "unknown"}),
            ("entity.references", {"entity_id": "unknown", "direction": "incoming"})):
        response = client.call(operation, params, before, expected="failed")
        assert response["error"]["code"] == "ENTITY_NOT_FOUND", response
    response = client.call("entity.references", {"entity_id": line, "direction": "both"},
                           before, expected="failed")
    assert response["error"]["code"] == "INVALID_INPUT", response
    assert client.current()["revision"] == before["revision"]
    expired = {**before, "document_epoch": "expired"}
    response = client.call("entity.query", {}, expired, expected="conflict")
    assert response["error"]["code"] == "DOCUMENT_EPOCH_EXPIRED", response
    # entity.references retains the complete legacy response across its bounded internal pages.
    generate_mesh(client, line, 501, "mesh-many-references")
    second_mesh = next(row for row in query(client, kind="mesh")["entities"]
                       if row["entity_id"] != mesh["entity_id"])
    incoming = references(client, second_mesh["entity_id"], "incoming")
    assert len(incoming) == 1003 and len({ref["from"] for ref in incoming}) == 1003
    assert all(ref["to"] == second_mesh["entity_id"] for ref in incoming)
    complete = contract_pair(client, Path(client.endpoint).parent / "work.sqlite", "entity.references",
                             {"entity_id": second_mesh["entity_id"], "direction": "incoming"})
    assert complete["data"]["references"] == incoming


def edit(client, command, parameters):
    doc = client.current()
    preview = client.call("changes.preview", {"command": command, **parameters}, doc)
    client.call("changes.commit", {"preview_id": preview["preview_id"]}, doc,
                key=f"{command}-{doc['revision']}")
    return preview["affected_entity_id"]


def organization_queries(client):
    profile = client.call("capabilities.list")["declared_solver_profiles"][0]["profile_ref"]
    client.call("project.create", {"name": "Organization query preservation"}, key="new")
    edit(client, "model.import", {
        "root_resource": "model.bdf", "source_profile_ref": profile, "unit_system": "mm-N-MPa",
        "resources": [{"path": "model.bdf", "text": ROOT_DECK},
                      {"path": "parts/nodes.bdf", "text": NODE_DECK},
                      {"path": "parts/physics/beam.bdf", "text": BEAM_DECK}],
    })
    nodes = query(client, kind="node")["entities"]
    beam = query(client, kind="beam")["entities"][0]
    section = query(client, kind="section")["entities"][0]
    material = query(client, kind="material")["entities"][0]
    analysis = query(client, kind="analysis")["entities"][0]
    part = edit(client, "part.upsert", {"name": "Part", "members": [beam["entity_id"]]})
    assembly = edit(client, "assembly.upsert", {"name": "Assembly", "children": [part]})
    root = edit(client, "assembly.upsert", {"name": "Root", "children": [assembly]})
    group = edit(client, "set.upsert", {"name": "Beams", "members": [beam["entity_id"]]})
    connected = identities(nodes) | {beam["entity_id"]}
    expected = {("part", part): connected,
                ("assembly", root): connected | {part, assembly},
                ("set", group): {beam["entity_id"]},
                ("property", section["entity_id"]): connected,
                ("material", material["entity_id"]): connected | {section["entity_id"]}}
    includes = query(client, kind="include")["entities"]
    root_include = next(row for row in includes if row["path"] == "model.bdf")
    leaf = next(row for row in includes if row["path"] == "parts/physics/beam.bdf")
    expected[("include", leaf["entity_id"])] = {
        beam["entity_id"], section["entity_id"], material["entity_id"]}
    expected[("include", root_include["entity_id"])] = (
        identities(includes) - {root_include["entity_id"]}) | {
            entity for row in includes for entity in row["members"]}
    for (view, owner), wanted in expected.items():
        rows = query(client, view=view, owner_id=owner)["entities"]
        assert identities(rows) == wanted, (view, rows, wanted)
        assert owner not in identities(rows)
        narrowed = query(client, view=view, owner_id=owner, kind="beam", ids=list(connected),
                         name_contains="")["entities"]
        assert identities(narrowed) == wanted & {beam["entity_id"]}
    for row in query(client)["entities"]:
        check_fields(client, row)
    assert sorted(source["number"] for row in nodes for source in row["sources"]) == ["1", "2"]
    assert all(row["sources"][0]["namespace"] == "GRID" for row in nodes)
    by_id = query(client, ids=[beam["entity_id"]])["entities"][0]
    assert by_id["sources"] == beam["sources"]
    outgoing = references(client, beam["entity_id"], "outgoing")
    assert {ref["role"] for ref in outgoing} == {"beam.section", "beam.node", "beam.mesh", "source.include"}
    assert any(ref["role"] == "source.include" and ref["to"] == leaf["entity_id"]
               for ref in outgoing)
    incoming = client.call("entity.references", {"entity_id": material["entity_id"],
                                                "direction": "incoming"}, client.current())
    assert any(ref["role"] == "section.material" for ref in incoming["references"])
    assert analysis["entity_id"] in incoming["affected_analyses"]
    workspace = Path(client.endpoint).parent / "work.sqlite"
    contract_pair(client, workspace, "entity.query", {"view": "material", "owner_id": material["entity_id"]})
    result = contract_pair(client, workspace, "entity.references", {"entity_id": material["entity_id"]})
    assert result["data"]["affected_analyses"] == incoming["affected_analyses"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--only-case", choices=("geometry", "organization"))
    args = parser.parse_args()
    engine, cli = str(Path(args.engine).resolve()), str(Path(args.cli).resolve())
    with tempfile.TemporaryDirectory(prefix="qquery-", dir=Path("/tmp").resolve()) as folder:
        for name, run in (("geometry", geometry_queries), ("organization", organization_queries)):
            if args.only_case and name != args.only_case:
                continue
            root = Path(folder) / name
            root.mkdir()
            with engine_client(engine, cli, root, args.evidence_dir) as client:
                run(client)
    print(f"PASS: NEXT-01 real engine/CLI queries: {args.only_case or 'geometry and organization'}")


if __name__ == "__main__":
    main()
