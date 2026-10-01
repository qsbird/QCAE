#!/usr/bin/env python3
"""MCP stdio transport over one existing QCAE local engine; no business state."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import socket
import sys
import uuid

FRAME_LIMIT = 1024 * 1024
VERSIONS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")
CONTEXT = {"document_id", "document_epoch", "expected_revision", "idempotency_key",
           "requested_version", "expected_profile"}
CONNECTION_TOOLS = {"events.subscribe"}


class ProtocolError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


class TransportError(Exception):
    pass


def finite_json(raw):
    def reject(value):
        raise ValueError(f"non-finite JSON number: {value}")
    def number(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError("non-finite JSON number")
        return parsed
    value = json.loads(raw, parse_constant=reject, parse_float=number)
    pending = [value]
    while pending:
        item = pending.pop()
        if isinstance(item, str) and any(0xD800 <= ord(character) <= 0xDFFF for character in item):
            raise ValueError("unpaired Unicode surrogate")
        if isinstance(item, dict):
            pending.extend(item.keys())
            pending.extend(item.values())
        elif isinstance(item, list):
            pending.extend(item)
    return value


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8") + b"\n"


class Engine:
    def __init__(self, endpoint: str, timeout: float):
        self.endpoint, self.timeout = endpoint, timeout

    @staticmethod
    def exchange(stream, request):
        frame = encoded(request)
        if len(frame) > FRAME_LIMIT:
            raise ProtocolError(-32602, "QCAE request exceeds its 1 MiB frame limit")
        stream.write(frame)
        stream.flush()
        raw = stream.readline(FRAME_LIMIT + 1)
        if not raw or len(raw) > FRAME_LIMIT or not raw.endswith(b"\n"):
            raise TransportError("missing, oversized or incomplete engine response")
        response = finite_json(raw)
        if not isinstance(response, dict) or response.get("request_id") != request["request_id"]:
            raise TransportError("engine response does not match request_id")
        status = response.get("status")
        if not isinstance(status, str) or status not in {"success", "failed", "needs_input", "conflict"}:
            raise TransportError("invalid engine response status")
        return response

    def call(self, operation: str, arguments: dict):
        request = {"api_version": "1.1", "request_id": f"mcp-{uuid.uuid4()}",
                   "operation": operation, "parameters": arguments.get("parameters", {})}
        request.update((key, value) for key, value in arguments.items() if key in CONTEXT)
        # Check before connecting: an invalid local frame cannot have executed.
        if len(encoded(request)) > FRAME_LIMIT:
            raise ProtocolError(-32602, "QCAE request exceeds its 1 MiB frame limit")
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(self.timeout)
                connection.connect(self.endpoint)
                with connection.makefile("rwb") as stream:
                    handshake = {"api_version": "1.1", "request_id": f"handshake-{uuid.uuid4()}",
                                 "operation": "runtime.handshake"}
                    ready = self.exchange(stream, handshake)
                    metadata = ready.get("data")
                    if ready.get("status") != "success" or not isinstance(metadata, dict) or metadata.get("api_version") != "1.1":
                        raise TransportError("incompatible engine handshake")
                    return self.exchange(stream, request)
        except (OSError, ValueError, RecursionError) as error:
            raise TransportError(f"engine transport failed: {type(error).__name__}") from error


def field_schema(field):
    kind = field.get("wire_type")
    units = field.get("units", [])
    text = "Engine field contract: " + json.dumps(field, ensure_ascii=False, separators=(",", ":"))
    if kind in {"string", "entity_id"}:
        schema = {"type": "string"}
        if not field.get("allow_empty", False):
            schema["minLength"] = 1
    elif kind == "finite_number":
        schema = {"type": "number"}
    elif kind == "quantity":
        unit = {"type": "string"}
        if units:
            unit["enum"] = units
        schema = {"type": "object", "properties": {"value": {"type": "number"}, "unit": unit},
                  "required": ["value", "unit"], "additionalProperties": False}
    elif kind == "vector3_mm":
        schema = {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}
    elif kind == "boolean":
        schema = {"type": "boolean"}
    elif kind == "entity_id_array":
        schema = {"type": "array", "items": {"type": "string", "minLength": 1}}
        if not field.get("allow_empty", False):
            schema["minItems"] = 1
    elif kind == "positive_uint32":
        schema = {"type": "integer", "minimum": 1, "maximum": 4294967295}
    else:
        # Unknown contracts remain discoverable without inventing a type restriction.
        schema = {}
    return {**schema, "description": text}


def checked_parameters_schema(candidate):
    """Bound the explicit metadata subset; business input validation stays in the engine."""
    if (not isinstance(candidate, dict) or candidate.get("type") != "object"
            or not isinstance(candidate.get("properties"), dict)
            or candidate.get("additionalProperties") is not False):
        raise TransportError("invalid engine parameters schema")
    pending, count = [(candidate, 0)], 0
    keywords = {"type", "properties", "required", "items", "additionalProperties", "enum",
                "minimum", "maximum", "minLength", "maxLength", "minItems", "maxItems", "description"}
    types = {"object", "array", "string", "number", "integer", "boolean", "null"}
    while pending:
        schema, depth = pending.pop()
        count += 1
        if not isinstance(schema, dict) or depth > 16 or count > 128 or schema.keys() - keywords:
            raise TransportError("unsupported or unbounded engine parameters schema")
        if "type" in schema:
            kind = schema["type"]
            kinds = kind if isinstance(kind, list) else [kind]
            if (not kinds or any(not isinstance(value, str) or value not in types for value in kinds)
                    or len(set(kinds)) != len(kinds)):
                raise TransportError("invalid engine schema type")
        properties = schema.get("properties", {})
        if (not isinstance(properties, dict) or len(properties) > 128
                or any(not isinstance(key, str) or not key for key in properties)):
            raise TransportError("invalid engine schema properties")
        pending.extend((value, depth + 1) for value in properties.values())
        required = schema.get("required", [])
        if (not isinstance(required, list) or any(not isinstance(key, str) for key in required)
                or len(set(required)) != len(required) or not set(required) <= properties.keys()):
            raise TransportError("invalid engine schema required fields")
        if "items" in schema:
            pending.append((schema["items"], depth + 1))
        if "additionalProperties" in schema and type(schema["additionalProperties"]) is not bool:
            raise TransportError("invalid engine schema additional properties")
        if "description" in schema and not isinstance(schema["description"], str):
            raise TransportError("invalid engine schema description")
        for key in ("minimum", "maximum"):
            if key in schema and (type(schema[key]) not in (int, float)
                                  or not -sys.float_info.max <= schema[key] <= sys.float_info.max):
                raise TransportError("invalid engine schema numeric bound")
        for key in ("minLength", "maxLength", "minItems", "maxItems"):
            if key in schema and (type(schema[key]) is not int or not 0 <= schema[key] <= FRAME_LIMIT):
                raise TransportError("invalid engine schema size bound")
        if "enum" in schema:
            values = schema["enum"]
            if (not isinstance(values, list) or not 1 <= len(values) <= 128
                    or any(type(value) not in (str, int, float, bool, type(None)) for value in values)):
                raise TransportError("invalid engine schema enumeration")
    return candidate


def tool_schema(descriptor):
    fields = descriptor.get("fields", [])
    parameters = descriptor.get("parameters_schema", {"type": "object"})
    if "parameters_schema" in descriptor:
        parameters = checked_parameters_schema(parameters)
    elif fields:
        parameters["properties"] = {field["name"]: field_schema(field) for field in fields}
        parameters["required"] = [field["name"] for field in fields if field.get("required")]
        parameters["additionalProperties"] = False
    properties = {"parameters": parameters}
    for key in ("document_id", "document_epoch", "idempotency_key"):
        properties[key] = {"type": "string", "minLength": 1}
    properties["expected_revision"] = {"type": "string", "pattern": "^[0-9]+$"}
    if "requested_version_field" in descriptor or "version" in descriptor or "requested_version" in descriptor:
        properties["requested_version"] = {"type": "integer", "minimum": 1, "maximum": 4294967295}
        if "requested_version" in descriptor:
            properties["requested_version"]["const"] = descriptor["requested_version"]
        properties["expected_profile"] = {
            "type": "object", "properties": {key: {"type": "string", "minLength": 1}
                for key in ("profile_id", "profile_version", "definition_digest")},
            "required": ["profile_id", "profile_version", "definition_digest"], "additionalProperties": False}
    required = [key for key, flag in (("document_id", "requires_document"),
                ("document_epoch", "requires_epoch"), ("expected_revision", "requires_revision"),
                ("idempotency_key", "requires_idempotency_key")) if descriptor.get(flag)]
    if descriptor.get("requires_expected_profile"):
        required.append("expected_profile")
    if parameters.get("required"):
        required.append("parameters")
    if "requested_version" in descriptor:
        required.append("requested_version")
    return {"type": "object", "properties": properties, "required": required, "additionalProperties": False}


class Bridge:
    def __init__(self, engine: Engine):
        self.engine, self.initialized, self.ready = engine, False, False
        self.catalog = {}

    def discover(self):
        response = self.engine.call("capabilities.list", {})
        if response.get("status") != "success":
            raise TransportError("engine capability discovery failed")
        metadata = response.get("data")
        operations = metadata.get("operations") if isinstance(metadata, dict) else None
        if not isinstance(operations, list) or any(not isinstance(entry, dict) or not isinstance(entry.get("name"), str)
            or not isinstance(entry.get("fields", []), list) or any(not isinstance(field, dict) or not isinstance(field.get("name"), str)
                for field in entry.get("fields", [])) for entry in operations):
            raise TransportError("invalid engine capability response shape")
        for entry in operations:
            for key in ("requires_document", "requires_epoch", "requires_revision", "requires_idempotency_key",
                        "requires_expected_profile", "requires_profile_match"):
                if key in entry and type(entry[key]) is not bool:
                    raise TransportError("invalid engine context requirement metadata")
            if "description" in entry and not isinstance(entry["description"], str):
                raise TransportError("invalid engine capability description")
            for key in ("version", "requested_version"):
                if key in entry and (type(entry[key]) is not int or not 1 <= entry[key] <= 4294967295):
                    raise TransportError("invalid engine capability version")
            for field in entry.get("fields", []):
                if not isinstance(field.get("wire_type"), str) or not isinstance(field.get("units", []), list) or any(not isinstance(unit, str) for unit in field.get("units", [])) or any(key in field and not isinstance(field[key], bool) for key in ("required", "allow_empty")):
                    raise TransportError("invalid engine field contract metadata")
        self.catalog = {entry["name"]: entry for entry in operations
                        if entry.get("available") is True and entry["name"] not in CONNECTION_TOOLS
                        and re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", entry["name"])}

    def invoke(self, request):
        method, params = request["method"], request.get("params", {})
        if not isinstance(params, dict):
            raise ProtocolError(-32602, "params must be an object")
        if method == "ping":
            return {}
        if method == "initialize":
            if self.initialized:
                raise ProtocolError(-32600, "already initialized")
            client = params.get("clientInfo")
            if not isinstance(params.get("protocolVersion"), str) or not isinstance(params.get("capabilities"), dict) or not isinstance(client, dict) or any(not isinstance(client.get(key), str) or not client[key] for key in ("name", "version")):
                raise ProtocolError(-32602, "initialize requires protocolVersion, capabilities and clientInfo")
            version = params["protocolVersion"]
            self.initialized = True
            return {"protocolVersion": version if version in VERSIONS else VERSIONS[0],
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": "qcae-local-bridge", "version": "0.1.0"},
                    "instructions": "Use the existing QCAE engine. Read capabilities and current context before edits. Do not infer units, physical directions or revisions. Engine responses are authoritative. Transport failure may leave an outcome unknown; query operations.get with the original idempotency key before retrying. Graphical selection requires the desktop."}
        if not self.initialized or not self.ready:
            raise ProtocolError(-32002, "initialize and notifications/initialized are required")
        if method == "tools/list":
            if params.get("cursor"):
                raise ProtocolError(-32602, "this catalog has no pagination cursor")
            self.discover()
            return {"tools": [{"name": name, "description": entry.get("description", name) + "\nEngine capability: " + json.dumps(entry, ensure_ascii=False, separators=(",", ":")),
                "inputSchema": tool_schema(entry)} for name, entry in self.catalog.items()]}
        if method != "tools/call":
            raise ProtocolError(-32601, "method not found")
        if not self.catalog:
            self.discover()
        name, arguments = params.get("name"), params.get("arguments", {})
        if not isinstance(name, str) or name not in self.catalog:
            raise ProtocolError(-32602, "unknown or unavailable QCAE tool")
        if not isinstance(arguments, dict) or arguments.keys() - CONTEXT - {"parameters"} or not isinstance(arguments.get("parameters", {}), dict):
            raise ProtocolError(-32602, "arguments require a parameters object and QCAE context fields")
        try:
            response = self.engine.call(name, arguments)
        except TransportError as error:
            response = {"status": "failed", "error": {"code": "mcp_transport_unknown", "message": str(error),
                        "outcome": "unknown", "idempotency_key": arguments.get("idempotency_key"),
                        "recovery": "Query operations.get and project.current; do not blindly retry a write."}}
        return {"content": [{"type": "text", "text": encoded(response).decode("utf-8").rstrip("\n")}],
                "structuredContent": response, "isError": response.get("status") != "success"}

    def dispatch(self, request):
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            raise ProtocolError(-32600, "expected one JSON-RPC 2.0 request object")
        if "id" not in request:
            if request["method"] == "notifications/initialized" and self.initialized:
                self.ready = True
            # Notifications have no response. Unadvertised subscriptions/cancellation are not enabled.
            return None
        if isinstance(request["id"], bool) or not isinstance(request["id"], (str, int)):
            raise ProtocolError(-32600, "request id must be a string or integer")
        return {"jsonrpc": "2.0", "id": request["id"], "result": self.invoke(request)}


def serve(bridge):
    while True:
        raw = sys.stdin.buffer.readline(FRAME_LIMIT + 1)
        if not raw:
            return 0
        request_id = None
        try:
            if len(raw) > FRAME_LIMIT or not raw.endswith(b"\n"):
                raise ProtocolError(-32600, "MCP input exceeds 1 MiB or lacks newline")
            try:
                request = finite_json(raw)
            except (ValueError, UnicodeError, RecursionError) as error:
                raise ProtocolError(-32700, "invalid finite UTF-8 JSON") from error
            if isinstance(request, dict):
                candidate = request.get("id")
                if not isinstance(candidate, bool) and isinstance(candidate, (str, int)):
                    request_id = candidate
            response = bridge.dispatch(request)
        except ProtocolError as error:
            response = {"jsonrpc": "2.0", "id": request_id, "error": {"code": error.code, "message": str(error)}}
        except TransportError as error:
            response = {"jsonrpc": "2.0", "id": request_id, "error": {"code": -32000, "message": str(error)}}
        if response is not None:
            sys.stdout.buffer.write(encoded(response))
            sys.stdout.buffer.flush()
        if len(raw) > FRAME_LIMIT or not raw.endswith(b"\n"):
            # Do not parse the tail of a rejected oversized frame as another request.
            return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True, help="absolute Unix socket of an existing QCAE engine")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if not Path(args.endpoint).is_absolute() or not 0 < args.timeout <= 300:
        parser.error("endpoint must be absolute and timeout must be in (0, 300] seconds")
    return serve(Bridge(Engine(args.endpoint, args.timeout)))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BrokenPipeError, KeyboardInterrupt):
        raise SystemExit(0)
