#!/usr/bin/env python3
"""MCP framing and uncertain-response tests; fake peer has no business model."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading


def oneof_parameters_schema():
    return {"type": "object", "properties": {"mode": {"type": "string", "enum": ["normal", "recover"]},
            "path": {"type": "string", "minLength": 1}}, "required": ["mode"], "additionalProperties": False,
            "oneOf": [
                {"type": "object", "properties": {"mode": {"type": "string", "enum": ["normal"]},
                    "path": {"type": "string", "minLength": 1}}, "required": ["mode", "path"],
                    "additionalProperties": False},
                {"type": "object", "properties": {"mode": {"type": "string", "enum": ["recover"]}},
                    "required": ["mode"], "additionalProperties": False}]}


def conditional_arguments_schema():
    identity = {"type": "string", "minLength": 1}
    return {"type": "object", "properties": {
        "parameters": oneof_parameters_schema(), "document_id": {}, "document_epoch": {},
        "expected_revision": {}, "idempotency_key": {},
        "requested_version": {"type": "integer", "minimum": 1, "maximum": 4294967295}},
        "required": ["parameters"], "additionalProperties": False, "oneOf": [
            {"type": "object", "properties": {"parameters": {
                "type": "object", "properties": {"mode": {"enum": ["normal"]}, "path": {}},
                "required": ["mode"], "additionalProperties": False}, "document_id": identity,
                "document_epoch": identity, "expected_revision": {}, "idempotency_key": {},
                "requested_version": {}}, "required": ["parameters", "document_id", "document_epoch"],
                "additionalProperties": False},
            {"type": "object", "properties": {"parameters": {
                "type": "object", "properties": {"mode": {"enum": ["recover"]}, "path": {}},
                "required": ["mode"], "additionalProperties": False}, "document_id": {},
                "document_epoch": {}, "expected_revision": {}, "idempotency_key": {},
                "requested_version": {}}, "required": ["parameters"], "additionalProperties": False}]}


def arguments_descriptor(behavior):
    schema = conditional_arguments_schema()
    if behavior == "bad-arguments-property":
        schema["properties"]["actor"] = {}
    elif behavior == "bad-arguments-nonclosed":
        schema["additionalProperties"] = True
    elif behavior == "bad-arguments-required":
        schema["required"].append("unknown")
    elif behavior == "bad-arguments-depth":
        node = schema["properties"]["document_id"]
        for index in range(17):
            child = {"type": "object", "properties": {}}
            node["properties"] = {"next": child}
            node = child
    elif behavior == "bad-arguments-nodes":
        schema["oneOf"] = [{"properties": {"document_id": {}}} for index in range(64)]
    elif behavior == "bad-arguments-parameters":
        schema["properties"]["parameters"] = {"type": "object", "properties": {}, "additionalProperties": False}
    elif behavior == "bad-arguments-missing-parameters":
        del schema["properties"]["parameters"]
        schema["required"] = []
    elif behavior == "bad-arguments-keyword":
        schema["if"] = {}
    return {"name": "test.write", "available": True, "description": "test-only transport",
            "parameters_schema": oneof_parameters_schema(), "arguments_schema": schema,
            "version": 1, "requires_document": True, "requires_epoch": True}


def scenario(command, endpoint, behavior):
    requests = []
    error = []
    ready = threading.Event()
    done = threading.Event()

    def peer():
        try:
            with socket.socket(socket.AF_UNIX) as listener:
                listener.bind(str(endpoint))
                listener.listen(4)
                listener.settimeout(.1)
                ready.set()
                while not done.is_set():
                    try:
                        connection, _ = listener.accept()
                    except TimeoutError:
                        continue
                    with connection, connection.makefile("rwb") as stream:
                        handshake = json.loads(stream.readline())
                        data = None if behavior == "bad-handshake" else {"api_version": "1.1"}
                        stream.write(json.dumps({"request_id": handshake["request_id"], "status": "success", "data": data}).encode() + b"\n")
                        stream.flush()
                        raw = stream.readline()
                        if not raw:
                            continue
                        request = json.loads(raw)
                        requests.append(request)
                        if request["operation"] == "capabilities.list":
                            field = {"name": "value", "wire_type": "string", "required": True,
                                     "allow_empty": False, "units": []}
                            if behavior == "bad-field":
                                field["wire_type"] = []
                            if behavior == "bad-units":
                                field["units"] = "MPa"
                            data = {"operations": [{"name": "test.write", "available": True,
                                                     "description": "test-only transport", "fields": [field]}]}
                            if behavior.startswith("bad-parameters") or behavior == "valid-parameters-oneof":
                                schema = {"type": "object", "properties": {}, "additionalProperties": False}
                                if behavior == "bad-parameters-type":
                                    schema = []
                                elif behavior == "bad-parameters-property":
                                    schema["properties"] = {"value": []}
                                elif behavior == "bad-parameters-required":
                                    schema["required"] = ["unknown"]
                                elif behavior == "bad-parameters-inner-type":
                                    schema["properties"] = {"value": {"type": []}}
                                elif behavior == "bad-parameters-depth":
                                    node = schema
                                    for index in range(17):
                                        child = {"type": "object", "properties": {}}
                                        node["properties"]["next"] = child
                                        node = child
                                elif behavior == "bad-parameters-width":
                                    schema["properties"] = {str(index): {} for index in range(129)}
                                elif behavior == "bad-parameters-huge-bound":
                                    schema["properties"] = {"value": {"type": "integer", "minimum": 10 ** 1000}}
                                elif behavior == "bad-parameters-oneof-type":
                                    schema["oneOf"] = {}
                                elif behavior == "bad-parameters-oneof-empty":
                                    schema["oneOf"] = []
                                elif behavior == "bad-parameters-oneof-branch":
                                    schema["oneOf"] = [True]
                                elif behavior == "bad-parameters-oneof-width":
                                    schema["oneOf"] = [{} for index in range(129)]
                                elif behavior == "bad-parameters-oneof-depth":
                                    node = schema
                                    for index in range(17):
                                        child = {}
                                        node["oneOf"] = [child]
                                        node = child
                                elif behavior == "bad-parameters-oneof-nodes":
                                    # Each branch and its property share the existing 128-node budget.
                                    schema["oneOf"] = [{"properties": {"value": {}}} for index in range(64)]
                                elif behavior == "bad-parameters-oneof-required":
                                    schema["oneOf"] = [{"required": ["unknown"]}]
                                elif behavior == "bad-parameters-oneof-keyword":
                                    schema["oneOf"] = [{"not": {}}]
                                elif behavior == "bad-parameters-keyword":
                                    schema["const"] = {}
                                elif behavior == "valid-parameters-oneof":
                                    schema = oneof_parameters_schema()
                                    data["operations"][0]["version"] = 1
                                data["operations"][0]["parameters_schema"] = schema
                            if behavior == "bad-context-flag":
                                data["operations"][0]["requires_document"] = "false"
                            if behavior.startswith("bad-arguments") or behavior == "valid-arguments":
                                data["operations"] = [arguments_descriptor(behavior)]
                            response = {"request_id": request["request_id"], "status": "success", "data": data}
                            if behavior == "deep-metadata":
                                nested = '{"node":' * 20000 + 'null' + '}' * 20000
                                raw = ('{"request_id":' + json.dumps(request["request_id"]) +
                                       ',"status":"success","data":' + nested + '}\n').encode()
                                assert len(raw) < 1024 * 1024
                                stream.write(raw)
                                stream.flush()
                                continue
                        else:
                            if behavior == "lost-response":
                                continue
                            response = {"request_id": request["request_id"], "status": []}
                        stream.write(json.dumps(response).encode() + b"\n")
                        stream.flush()
        except Exception as problem:
            error.append(problem)
            ready.set()

    worker = threading.Thread(target=peer)
    worker.start()
    assert ready.wait(5) and not error, error
    child = subprocess.Popen([*command, "--endpoint", str(endpoint)], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def exchange(request):
        child.stdin.write(json.dumps(request).encode() + b"\n")
        child.stdin.flush()
        raw = child.stdout.readline()
        assert raw, child.stderr.read().decode()
        return json.loads(raw)

    try:
        init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                           "clientInfo": {"name": "transport-test", "version": "1"}}}
        assert "result" in exchange(init)
        child.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        child.stdin.flush()
        catalog = exchange({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        if (behavior in {"bad-handshake", "bad-field", "bad-units", "deep-metadata", "bad-context-flag"}
                or behavior.startswith(("bad-parameters", "bad-arguments"))):
            assert catalog["error"]["code"] == -32000, catalog
            assert len(requests) <= 1, requests
            rejected = exchange({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
                "name": "test.write", "arguments": {"parameters": {"value": "literal"}}}})
            assert rejected["error"]["code"] == -32000, rejected
            assert all(request["operation"] == "capabilities.list" for request in requests), requests
        else:
            assert "result" in catalog, catalog
            parameters = {"value": "literal"}
            arguments = {"parameters": parameters, "idempotency_key": "original-key"}
            if behavior == "valid-parameters-oneof":
                tool = catalog["result"]["tools"][0]["inputSchema"]
                schema = tool["properties"]["parameters"]
                assert schema == oneof_parameters_schema(), schema
                assert "requested_version" in tool["properties"] and "requested_version" not in tool["required"], tool
                assert "expected_profile" not in tool["properties"], tool
                parameters = {"mode": "recover"}
                arguments["parameters"] = parameters
            elif behavior == "valid-arguments":
                tool = catalog["result"]["tools"][0]
                assert tool["inputSchema"] == conditional_arguments_schema(), tool
                _, separator, source = tool["description"].partition("\nEngine capability: ")
                assert separator and json.loads(source) == arguments_descriptor(behavior), tool
                arguments.update(parameters={"mode": "recover"}, document_id=None,
                                 document_epoch={"ignored": None}, expected_revision=False, requested_version=2)
            calls = [arguments]
            if behavior == "valid-arguments":
                # Schema metadata never turns the transport into a business input validator.
                calls = [{**arguments, "parameters": {"mode": "recover", "path": value}} for value in
                         ("literal", "", 7, False, None, [None], {"nested": None})]
            for index, arguments in enumerate(calls):
                result = exchange({"jsonrpc": "2.0", "id": 3 + index, "method": "tools/call", "params": {
                    "name": "test.write", "arguments": arguments}})["result"]
                facts = result["structuredContent"]
                assert result["isError"] is True and facts["status"] == "failed", result
                assert facts["error"]["code"] == "mcp_transport_unknown", facts
                assert facts["error"]["outcome"] == "unknown" and facts["error"]["idempotency_key"] == "original-key", facts
                assert {key: requests[-1][key] for key in arguments} == arguments, requests
            assert [request["operation"] for request in requests] == ["capabilities.list"] + ["test.write"] * len(calls), requests
        # A malformed/lost engine reply does not kill the stdio server or cause a retry.
        assert exchange({"jsonrpc": "2.0", "id": 4, "method": "ping"})["result"] == {}
        child.stdin.close()
        child.wait(timeout=5)
        assert child.returncode == 0 and not child.stderr.read(), child.returncode
    finally:
        if child.poll() is None:
            child.kill()
        child.wait(timeout=5)
        done.set()
        worker.join(timeout=5)
    assert not worker.is_alive() and not error, error


def run(args):
    with tempfile.TemporaryDirectory(prefix="qcae-mcp-protocol-", dir=Path("/tmp").resolve()) as temporary:
        for behavior in ("bad-handshake", "bad-field", "bad-units", "bad-parameters-type",
                         "bad-parameters-property", "bad-parameters-required", "bad-parameters-inner-type",
                         "bad-parameters-depth", "bad-parameters-width", "bad-parameters-huge-bound",
                         "bad-parameters-oneof-type", "bad-parameters-oneof-empty", "bad-parameters-oneof-branch",
                         "bad-parameters-oneof-width", "bad-parameters-oneof-depth", "bad-parameters-oneof-nodes",
                         "bad-parameters-oneof-required", "bad-parameters-oneof-keyword", "bad-parameters-keyword",
                         "valid-parameters-oneof", "bad-arguments-property", "bad-arguments-nonclosed",
                         "bad-arguments-required", "bad-arguments-depth", "bad-arguments-nodes",
                         "bad-arguments-parameters", "bad-arguments-missing-parameters", "bad-arguments-keyword",
                         "valid-arguments",
                         "deep-metadata", "bad-context-flag", "lost-response", "bad-status"):
            scenario([args.mcp], Path(temporary) / f"{behavior}.sock", behavior)
    print("PASS: thirty-three metadata/transport peer scenarios, bounded conditional discovery, unchanged arguments, no retry")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mcp", required=True)
    run(parser.parse_args())
