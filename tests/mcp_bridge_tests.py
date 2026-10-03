#!/usr/bin/env python3
"""Real stdio MCP clients and CLI share one QCAE engine and transaction history."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time


class Client:
    def __init__(self, command, endpoint):
        self.process = subprocess.Popen([*command, "--endpoint", str(endpoint)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.sequence = 0

    def send(self, value):
        self.process.stdin.write(json.dumps(value, separators=(",", ":")).encode() + b"\n")
        self.process.stdin.flush()

    def request(self, method, params=None):
        self.sequence += 1
        self.send({"jsonrpc": "2.0", "id": self.sequence, "method": method, "params": params or {}})
        assert select.select([self.process.stdout], [], [], 15)[0], "MCP response timeout"
        response = json.loads(self.process.stdout.readline())
        assert response["jsonrpc"] == "2.0" and response["id"] == self.sequence, response
        return response

    def raw(self, raw):
        self.process.stdin.write(raw + b"\n")
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 15)[0], "MCP raw response timeout"
        return json.loads(self.process.stdout.readline())

    def initialize(self):
        response = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                                "clientInfo": {"name": "qcae-test", "version": "1"}})
        assert response["result"]["protocolVersion"] == "2025-11-25", response
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def call(self, name, parameters=None, context=None, key=None, status="success", version=None):
        arguments = {"parameters": parameters or {}}
        if context:
            arguments.update(document_id=context["document_id"], document_epoch=context["document_epoch"],
                             expected_revision=context["revision"])
        if key:
            arguments["idempotency_key"] = key
        if version is not None:
            arguments["requested_version"] = version
        response = self.request("tools/call", {"name": name, "arguments": arguments})
        assert "result" in response, response
        result = response["result"]
        raw = result["structuredContent"]
        assert json.loads(result["content"][0]["text"]) == raw, result
        assert raw["status"] == status and result["isError"] == (status != "success"), raw
        return raw

    def current(self):
        return self.call("project.current")["data"]

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait(timeout=5)
        error = self.process.stderr.read().decode()
        assert self.process.returncode == 0 and not error, (self.process.returncode, error)


def same_commit(response, original):
    # Replays report the original transaction and the current document state.
    assert response["data"]["transaction_id"] == original["data"]["transaction_id"], (response, original)
    assert response["data"]["committed_revision"] == original["data"]["committed_revision"], (response, original)
    assert response["data"]["replayed"] is True, response


def run(args):
    command = [args.mcp] if args.mcp else [sys.executable, str(Path(__file__).resolve().parents[1] / "apps/mcp/bridge.py")]
    with tempfile.TemporaryDirectory(prefix="qcae-mcp-", dir=Path("/tmp").resolve()) as temporary:
        root = Path(temporary)
        endpoint = root / "engine.sock"
        with (root / "engine.log").open("w+") as log:
            engine_command = [args.engine, "--socket", str(endpoint)]
            if args.durable:
                engine_command += ["--workspace", str(root / "workspace.sqlite")]
            engine = subprocess.Popen(engine_command, stdout=log, stderr=log)
            clients = []
            try:
                deadline = time.monotonic() + 15
                while not endpoint.exists() and engine.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.025)
                assert endpoint.exists(), "engine did not create socket"
                first, second = Client(command, endpoint), Client(command, endpoint)
                clients.extend((first, second))
                assert first.request("tools/list")["error"]["code"] == -32002
                for client in clients:
                    client.initialize()
                discovery = first.request("tools/list")
                assert "result" in discovery, discovery
                tools = discovery["result"]["tools"]
                catalog = {tool["name"]: tool for tool in tools}
                caps = first.call("capabilities.list")["data"]["operations"]
                assert set(catalog) == {item["name"] for item in caps if item["available"] and item["name"] != "events.subscribe"}
                assert "events.subscribe" not in catalog and "events.read" in catalog
                assert "analysis.start" not in catalog
                create_schema = catalog["project.create"]["inputSchema"]
                create_descriptor = next(item for item in caps if item["name"] == "project.create")
                assert create_schema["properties"]["parameters"] == create_descriptor["parameters_schema"] == {
                    "type": "object", "properties": {"name": {"type": "string", "minLength": 1}},
                    "required": ["name"], "additionalProperties": False}, create_schema
                assert create_schema["properties"]["requested_version"] == {
                    "type": "integer", "minimum": 1, "maximum": 4294967295}, create_schema
                assert "requested_version" not in create_schema["required"], create_schema
                assert "expected_profile" not in create_schema["properties"], create_schema
                commit_schema = catalog["changes.commit"]["inputSchema"]
                commit_descriptor = next(item for item in caps if item["name"] == "changes.commit")
                assert commit_descriptor["schema_id"] == "qcae.operation.changes.commit.v1", commit_descriptor
                assert commit_descriptor["version"] == 1, commit_descriptor
                assert commit_schema["properties"]["parameters"] == commit_descriptor["parameters_schema"] == {
                    "type": "object", "properties": {"preview_id": {"type": "string", "minLength": 1}},
                    "required": ["preview_id"], "additionalProperties": False}, commit_schema
                assert commit_schema["properties"]["requested_version"] == {
                    "type": "integer", "minimum": 1, "maximum": 4294967295}, commit_schema
                assert "requested_version" not in commit_schema["required"], commit_schema
                assert all(field in commit_schema["required"] for field in
                           ("parameters", "document_id", "document_epoch", "expected_revision", "idempotency_key")), commit_schema
                assert "expected_profile" not in commit_schema["properties"], commit_schema
                open_schema = catalog["project.open"]["inputSchema"]
                open_parameters = open_schema["properties"]["parameters"]
                open_descriptor = next(item for item in caps if item["name"] == "project.open")
                assert open_parameters == open_descriptor["parameters_schema"], (open_parameters, open_descriptor)
                assert open_parameters["oneOf"] == [
                    {"type": "object", "properties": {"mode": {"type": "string", "enum": ["normal"]},
                        "path": {"type": "string", "minLength": 1}}, "required": ["mode", "path"],
                        "additionalProperties": False},
                    {"type": "object", "properties": {"mode": {"type": "string", "enum": ["recover"]}},
                        "required": ["mode"], "additionalProperties": False}], open_parameters
                assert open_schema["properties"]["requested_version"] == {
                    "type": "integer", "minimum": 1, "maximum": 4294967295}, open_schema
                assert "requested_version" not in open_schema["required"], open_schema
                assert "expected_profile" not in open_schema["properties"], open_schema
                for operation in ("project.save", "project.save_as", "project.close"):
                    schema = catalog[operation]["inputSchema"]
                    descriptor = next(item for item in caps if item["name"] == operation)
                    parameters = schema["properties"]["parameters"]
                    assert parameters == descriptor["parameters_schema"], (schema, descriptor)
                    expected = {"type": "object", "additionalProperties": False}
                    if operation == "project.close":
                        expected.update(properties={"policy": {"type": "string", "minLength": 1,
                            "enum": ["discard", "keep_recovery"]}}, required=["policy"])
                    else:
                        expected.update(properties={"path": {"type": "string"}}, required=[])
                    assert parameters == expected, schema
                    assert schema["properties"]["requested_version"] == {
                        "type": "integer", "minimum": 1, "maximum": 4294967295}, schema
                    assert "requested_version" not in schema["required"], schema
                    assert "expected_profile" not in schema["properties"], schema
                    assert {"document_id", "document_epoch", "expected_revision", "idempotency_key"} <= set(schema["required"]), schema
                material_schema = catalog["material.create"]["inputSchema"]
                assert material_schema["properties"]["parameters"]["properties"]["young_modulus"]["required"] == ["value", "unit"]
                assert "expected_revision" in material_schema["required"]
                assert "expected_profile" in material_schema["properties"], material_schema
                assert "expected_profile" not in material_schema["required"], material_schema
                assert catalog["view.render_resource"]["inputSchema"]["properties"]["requested_version"]["const"] == 1
                assert "requested_version" in catalog["view.render_resource"]["inputSchema"]["required"]
                query_schema = catalog["entity.query"]["inputSchema"]["properties"]["parameters"]
                assert query_schema["additionalProperties"] is False
                assert set(query_schema["properties"]) == {"kind", "name_contains", "ids", "view", "owner_id", "offset", "limit"}
                assert query_schema["properties"]["limit"] == {"type": "integer", "minimum": 0, "maximum": 1000}
                fields_schema = catalog["entity.fields"]["inputSchema"]["properties"]["parameters"]
                assert fields_schema["required"] == ["entity_id"] and fields_schema["additionalProperties"] is False
                assert catalog["task.status"]["inputSchema"]["properties"]["parameters"]["required"] == ["task_id"]
                for operation in ("model.summary", "project.status", "task.reconcile"):
                    empty = catalog[operation]["inputSchema"]["properties"]["parameters"]
                    assert empty["properties"] == {} and empty["additionalProperties"] is False
                if "solver.configuration" in catalog:
                    empty = catalog["solver.configuration"]["inputSchema"]["properties"]["parameters"]
                    assert empty["properties"] == {} and empty["additionalProperties"] is False
                assert second.request("tools/call", {"name": "analysis.start"})["error"]["code"] == -32602
                assert second.request("unknown.method")["error"]["code"] == -32601
                assert second.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {}})["error"]["code"] == -32600
                created = first.call("project.create", {"name": "MCP shared model"}, key="mcp-create")
                replayed = first.call("project.create", {"name": "MCP shared model"}, key="mcp-create", version=1)
                assert replayed["data"] == created["data"], (replayed, created)
                context = first.current()
                assert context == second.current()
                first.call("entity.query", {"entity_type": "Node"}, context, status="failed")
                first.call("entity.query", {"limit": 1001}, context, status="failed")
                first.call("entity.fields", context=context, status="failed")
                first.call("model.summary", {"analysis_id": "not-a-summary-filter"}, context, status="failed")
                assert first.current() == context
                if args.durable:
                    assert context["durable"] is True, context
                failed = first.call("entity.fields", {"entity_id": "missing-entity"}, context, status="failed")
                assert failed["error"]["code"] != "mcp_transport_unknown", failed
                missing = first.call("material.create", {"name": "Steel", "young_modulus": {"value": 210}},
                                     context, "unit-missing", "needs_input")
                assert missing["error"]["code"] and first.current()["revision"] == context["revision"]
                material = {"name": "Steel", "young_modulus": {"value": 210, "unit": "GPa"}}
                committed = first.call("material.create", material, context, "mcp-material")
                updated = second.current()
                assert int(updated["revision"]) == int(context["revision"]) + 1
                same_commit(first.call("material.create", material, context, "mcp-material"), committed)
                first.call("material.create", {**material, "name": "Different"}, context, "mcp-material", "conflict")
                first.call("material.create", {**material, "name": "Stale"}, context, "stale-key", "conflict")
                cli_request = {"api_version": "1.1", "request_id": "mcp-cli-undo", "operation": "history.undo",
                               "parameters": {}, "document_id": updated["document_id"],
                               "document_epoch": updated["document_epoch"], "expected_revision": updated["revision"],
                               "idempotency_key": "cli-undo"}
                cli = subprocess.run([args.cli, "--socket", str(endpoint), "--no-start"],
                                     input=json.dumps(cli_request), capture_output=True, text=True, timeout=15)
                assert cli.returncode == 0 and json.loads(cli.stdout)["status"] == "success", cli.stderr
                undone = first.current()
                assert undone == second.current() and int(undone["revision"]) == int(updated["revision"]) + 1
                # A replay after undo reports the original commit without recreating the entity.
                replay = first.call("material.create", material, context, "mcp-material")
                same_commit(replay, committed)
                assert first.current() == undone
                assert first.call("model.summary", context=undone)["data"]["materials"] == []
                second.call("history.redo", context=undone, key="mcp-redo")
                assert len(first.call("model.summary", context=first.current())["data"]["materials"]) == 1
                wrong_epoch = {**first.current(), "document_epoch": "incorrect-epoch"}
                first.call("model.summary", context=wrong_epoch, status="conflict")
                assert first.request("tools/call", {"name": "project.current", "arguments": {"shell": "false"}})["error"]["code"] == -32602
                for raw in (b'{', b'[]', b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":NaN}}',
                            b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":1e999}}',
                            b'{"jsonrpc":"2.0","id":"\\ud800","method":"ping"}'):
                    assert "error" in first.raw(raw), raw
                for invalid_id in (True, [], {}):
                    invalid = first.raw(json.dumps({"jsonrpc": "2.0", "id": invalid_id, "method": "ping"}).encode())
                    assert invalid["id"] is None and invalid["error"]["code"] == -32600, invalid
                assert first.request("ping")["result"] == {}
                if args.durable:
                    save_context = first.current()
                    first.call("project.save", {"path": str(root / "model.qcae")}, save_context,
                               "mcp-save", version=1)
                    save_context = first.current()
                    saved = first.call("project.save", context=save_context, key="mcp-save-default")
                    empty_replay = first.call("project.save", {"path": ""}, save_context,
                                              "mcp-save-default", version=1)
                    assert empty_replay["data"] == saved["data"], (empty_replay, saved)
                    assert first.current() == save_context
                    copied = first.call("project.save_as", {"path": str(root / "copy.qcae")},
                                        save_context, "mcp-save-as", version=1)
                    copied_replay = first.call("project.save_as", {"path": str(root / "copy.qcae")},
                                               save_context, "mcp-save-as")
                    assert copied_replay["data"] == copied["data"], (copied_replay, copied)
                    assert first.current()["document_id"] == save_context["document_id"]

                for client in clients:
                    client.close()
                clients.clear()
                # Ending both MCP transports preserves the engine and CLI-visible document.
                assert engine.poll() is None
                cli_request.update(operation="project.current", parameters={})
                for key in ("document_id", "document_epoch", "expected_revision", "idempotency_key"):
                    cli_request.pop(key)
                cli = subprocess.run([args.cli, "--socket", str(endpoint), "--no-start"],
                                     input=json.dumps(cli_request), capture_output=True, text=True, timeout=15)
                assert cli.returncode == 0 and json.loads(cli.stdout)["data"]["document_id"] == context["document_id"]
                closer = Client(command, endpoint)
                clients.append(closer)
                closer.initialize()
                close_context = closer.current()
                closed = closer.call("project.close", {"policy": "discard"}, close_context,
                                     "mcp-close", version=1)
                replayed_close = closer.call("project.close", {"policy": "discard"}, close_context,
                                             "mcp-close")
                assert replayed_close["data"] == closed["data"], (replayed_close, closed)
                fact = closer.call("operations.get", {"lookup_scope": "host",
                    "original_operation": "project.close", "idempotency_key": "mcp-close"})
                assert fact["data"] == closed["data"], (fact, closed)
                closer.call("project.current", status="failed")
                closer.close()
                clients.remove(closer)

            finally:
                for client in clients:
                    if client.process.poll() is None:
                        client.process.kill()
                    client.process.wait(timeout=5)
                engine.terminate()
                engine.wait(timeout=5)
    print("PASS: two actual MCP stdio clients and CLI share discovery, units, versions, idempotency and history")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--mcp")
    parser.add_argument("--durable", action="store_true")
    run(parser.parse_args())
