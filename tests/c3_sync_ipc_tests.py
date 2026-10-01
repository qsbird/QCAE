#!/usr/bin/env python3
"""C3 resource, event-gap, and task-ack acceptance over the real SQLite engine socket."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path


RESOURCE_CHUNK_BYTES = 128 * 1024
RESOURCE_MAX_FRAME_BYTES = 256 * 1024
TASK_ACK_LIMIT_MS = 100.0


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def integer(value: object) -> int:
    return int(value)


class IpcClient:
    """JSON-line transport only; all application behavior stays in the engine."""

    def __init__(self, endpoint: str) -> None:
        self.endpoint = endpoint
        self.sequence = 0
        self.instance_id: str | None = None
        self.transcript: list[dict] = []
        self.frames: list[dict] = []
        self.last_frame: dict | None = None

    def call(self, operation: str, parameters: dict | None = None,
             context: dict | None = None, key: str | None = None,
             extra: dict | None = None, expected: str | None = "success") -> dict:
        self.sequence += 1
        request_id = f"c3-sync-{self.sequence}"
        request = {"api_version": "1.1", "request_id": request_id,
                   "operation": operation, "parameters": parameters or {}}
        if context:
            request.update(document_id=context["document_id"],
                           document_epoch=context["document_epoch"],
                           expected_revision=str(context["revision"]))
        if key is not None:
            request["idempotency_key"] = key
        if extra:
            request.update(extra)
        request_bytes = json.dumps(request, separators=(",", ":"),
                                   ensure_ascii=False).encode("utf-8") + b"\n"
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(30)
            connection.connect(self.endpoint)
            with connection.makefile("rwb") as stream:
                self.sequence += 1
                handshake_id = f"c3-handshake-{self.sequence}"
                handshake = {"api_version": "1.1", "request_id": handshake_id,
                             "operation": "runtime.handshake"}
                handshake_bytes = json.dumps(handshake, separators=(",", ":")).encode() + b"\n"
                stream.write(handshake_bytes)
                stream.flush()
                handshake_line = stream.readline()
                check(bool(handshake_line), "engine closed during runtime.handshake")
                handshake_response = json.loads(handshake_line)
                check(handshake_response.get("status") == "success" and
                      handshake_response.get("request_id") == handshake_id,
                      f"runtime.handshake failed: {handshake_response}")
                instance = handshake_response["data"]["engine_instance_id"]
                if self.instance_id is None:
                    self.instance_id = instance
                check(instance == self.instance_id,
                      "engine instance changed without restarting the engine")
                stream.write(request_bytes)
                stream.flush()
                response_line = stream.readline()
        check(bool(response_line), f"engine closed without replying to {operation}")
        response = json.loads(response_line)
        self.last_frame = {"operation": operation,
                           "request_bytes": len(request_bytes),
                           "response_bytes": len(response_line)}
        self.frames.append(self.last_frame)
        self.transcript.append({"request": request, "response": response,
                                "request_frame_bytes": len(request_bytes),
                                "response_frame_bytes": len(response_line)})
        check(response.get("request_id") == request_id, response)
        if expected is not None:
            check(response.get("status") == expected,
                  f"{operation}: expected {expected}, got {response}")
        if response.get("status") == "success":
            return response["data"]
        return response

    def current(self) -> dict:
        return self.call("project.current")


class EngineProcess:
    def __init__(self, engine: str, endpoint: str, workspace: str, log_file) -> None:
        self.engine = engine
        self.endpoint = endpoint
        self.workspace = workspace
        self.log_file = log_file
        self.client = IpcClient(endpoint)
        self.process: subprocess.Popen | None = None

    def start(self) -> dict:
        check(self.process is None, "engine already started")
        self.process = subprocess.Popen(
            [self.engine, "--socket", self.endpoint, "--workspace", self.workspace],
            stdout=self.log_file, stderr=self.log_file)
        deadline = time.monotonic() + 20
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log_file.flush()
                self.log_file.seek(0)
                raise AssertionError(f"engine exited during startup: {self.log_file.read()}")
            try:
                return self.client.call("capabilities.list")
            except (OSError, RuntimeError, AssertionError, json.JSONDecodeError) as error:
                last_error = error
                time.sleep(.05)
        raise AssertionError(f"engine startup timed out: {last_error}")

    def close(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process = None


class BinaryReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def u64(self) -> int:
        check(self.offset + 8 <= len(self.data), "truncated render-resource integer")
        value = struct.unpack_from("<Q", self.data, self.offset)[0]
        self.offset += 8
        return value

    def text(self) -> str:
        size = self.u64()
        check(0 < size <= 1024 and self.offset + size <= len(self.data),
              "invalid render-resource string")
        result = self.data[self.offset:self.offset + size].decode("utf-8")
        self.offset += size
        return result

    def point(self) -> list[float]:
        check(self.offset + 24 <= len(self.data), "truncated render-resource point")
        result = list(struct.unpack_from("<ddd", self.data, self.offset))
        self.offset += 24
        return result

    def count(self) -> int:
        result = self.u64()
        check(result <= 500000, "render-resource item count exceeds codec limit")
        return result

    def finished(self) -> None:
        check(self.offset == len(self.data), "trailing render-resource bytes")


def decode_packet(data: bytes) -> dict:
    reader = BinaryReader(data)
    magic = reader.u64()
    check(magic in (0x3154454B43415051, 0x3254454B43415051, 0x3354454B43415051), "unexpected full render-resource version")
    extended = magic != 0x3154454B43415051
    packet = {"document_id": reader.text(), "document_epoch": reader.text(),
              "revision": reader.u64(), "view_session_id": reader.text(),
              "view_revision": reader.u64(), "points": [], "beams": [],
              "geometry_lines": [], "cells": []}
    for _ in range(reader.count()):
        entity_id = reader.text()
        position = reader.point()
        visible = reader.u64()
        check(visible in (0, 1), "invalid point visibility bit")
        packet["points"].append({"entity_id": entity_id, "position_mm": position,
                                 "visible": bool(visible)})
    for _ in range(reader.count()):
        beam = {"entity_id": reader.text(), "points": [reader.u64(), reader.u64()]}
        if extended:
            visible = reader.u64()
            check(visible in (0, 1), "invalid beam visibility bit")
            beam["visible"] = bool(visible)
        packet["beams"].append(beam)
    for _ in range(reader.count()):
        line = {"entity_id": reader.text(), "start_mm": reader.point(), "end_mm": reader.point()}
        if extended:
            visible = reader.u64()
            check(visible in (0, 1), "invalid geometry visibility bit")
            line["visible"] = bool(visible)
        packet["geometry_lines"].append(line)
    if magic == 0x3354454B43415051:
        for _ in range(reader.count()):
            cell = {"entity_id": reader.text(), "kind": reader.u64()}
            arity = reader.count()
            check(cell["kind"] in (0, 1) and 2 + cell["kind"] <= arity <= 4096,
                  "invalid generic cell topology")
            cell["points"] = [reader.u64() for _ in range(arity)]
            check(len(set(cell["points"])) == arity and
                  all(index < len(packet["points"]) for index in cell["points"]),
                  "invalid generic cell point")
            visible = reader.u64()
            check(visible in (0, 1), "invalid generic cell visibility")
            cell["visible"] = bool(visible)
            packet["cells"].append(cell)
    reader.finished()
    return packet


def decode_delta(data: bytes) -> dict:
    reader = BinaryReader(data)
    magic = reader.u64()
    check(magic in (0x3141544C45444351, 0x3241544C45444351, 0x3341544C45444351), "unexpected render-delta resource version")
    extended = magic != 0x3141544C45444351
    delta = {"document_id": reader.text(), "document_epoch": reader.text(),
             "revision": reader.u64(), "view_session_id": reader.text(),
             "view_revision": reader.u64(), "base_revision": reader.u64(),
             "base_view_revision": reader.u64(), "points": [], "geometry_lines": []}
    for _ in range(reader.count()):
        index, entity_id = reader.u64(), reader.text()
        position = reader.point()
        visible = reader.u64()
        check(visible in (0, 1), "invalid delta visibility bit")
        delta["points"].append({"index": index, "entity_id": entity_id,
                                "position_mm": position, "visible": bool(visible)})
    for _ in range(reader.count()):
        line = {"index": reader.u64(), "entity_id": reader.text(),
                "start_mm": reader.point(), "end_mm": reader.point()}
        if extended:
            visible = reader.u64()
            check(visible in (0, 1), "invalid geometry delta visibility bit")
            line["visible"] = bool(visible)
        delta["geometry_lines"].append(line)
    delta["visibility"] = []
    if extended:
        for _ in range(reader.count()):
            primitive, index, identity, visible = reader.u64(), reader.u64(), reader.text(), reader.u64()
            check(0 <= primitive <= (3 if magic == 0x3341544C45444351 else 2) and visible in (0, 1), "invalid visibility delta")
            delta["visibility"].append({"primitive": primitive, "index": index,
                                        "entity_id": identity, "visible": bool(visible)})
    reader.finished()
    return delta


def fetch_resource(client: IpcClient, manifest: dict, release: bool = True) -> tuple[bytes, dict]:
    context = {"document_id": manifest["document_id"],
               "document_epoch": manifest["document_epoch"],
               "revision": manifest["revision"]}
    base_params = {"resource_id": manifest["resource_id"],
                   "view_session_id": manifest["view_session_id"],
                   "expected_view_revision": manifest["view_revision"]}
    described = client.call("resources.describe", base_params, context,
                            extra={"requested_version": 1})
    check(described == manifest, "resource describe changed its immutable manifest")
    length = integer(manifest["byte_length"])
    chunk_bytes = integer(manifest["chunk_bytes"])
    chunk_count = integer(manifest["chunk_count"])
    check(chunk_bytes == RESOURCE_CHUNK_BYTES and
          chunk_count == math.ceil(length / RESOURCE_CHUNK_BYTES),
          f"invalid resource chunk contract: {manifest}")
    collected = bytearray()
    max_request_frame = 0
    max_response_frame = 0
    max_raw_chunk = 0
    for offset in range(0, length, RESOURCE_CHUNK_BYTES):
        params = {**base_params, "offset": str(offset)}
        data = client.call("resources.read", params, context,
                           extra={"requested_version": 1})
        frame = client.last_frame or {}
        max_request_frame = max(max_request_frame, integer(frame.get("request_bytes", 0)))
        max_response_frame = max(max_response_frame, integer(frame.get("response_bytes", 0)))
        check(frame.get("operation") == "resources.read", "resource read frame was not recorded")
        check(frame["request_bytes"] <= RESOURCE_MAX_FRAME_BYTES and
              frame["response_bytes"] <= RESOURCE_MAX_FRAME_BYTES,
              f"resource chunk JSON frame exceeded 256KiB: {frame}")
        check(data.get("manifest") == manifest and data.get("encoding") == "base64" and
              integer(data.get("offset")) == offset,
              f"resource chunk manifest, encoding or offset differs: {data}")
        raw = base64.b64decode(data["data_base64"], validate=True)
        expected_size = min(RESOURCE_CHUNK_BYTES, length - offset)
        check(len(raw) == expected_size and integer(data.get("raw_length")) == expected_size,
              f"resource chunk length mismatch at offset {offset}")
        max_raw_chunk = max(max_raw_chunk, len(raw))
        collected.extend(raw)
    digest = hashlib.sha256(collected).hexdigest()
    check(len(collected) == length and digest == manifest["sha256"],
          "resource byte length or SHA-256 did not match the manifest")
    if release:
        released = client.call("resources.release", {"resource_id": manifest["resource_id"]},
                               extra={"requested_version": 1})
        check(released.get("released") is True, "resource lease was not released")
    return bytes(collected), {"byte_length": length, "chunk_count": chunk_count,
                              "max_raw_chunk_bytes": max_raw_chunk,
                              "max_request_frame_bytes": max_request_frame,
                              "max_response_frame_bytes": max_response_frame,
                              "sha256": digest, "media_type": manifest["media_type"]}


def render_resource(client: IpcClient, view: dict, context: dict,
                    base_revision: str | None = None,
                    base_view_revision: str | None = None,
                    wire_version: int | None = 3) -> dict:
    parameters = {"view_session_id": view["view_session_id"],
                  "expected_view_revision": view["view_revision"]}
    if wire_version is not None:
        parameters["render_wire_version"] = wire_version
    if base_revision is not None or base_view_revision is not None:
        check(base_revision is not None and base_view_revision is not None,
              "render resource base requires both model and view revisions")
        parameters.update(base_revision=str(base_revision),
                          base_view_revision=str(base_view_revision))
    return client.call("view.render_resource", parameters, context,
                       extra={"requested_version": 1})


def update_view(client: IpcClient, view: dict, context: dict) -> dict:
    return client.call("view.update",
                       {"view_session_id": view["view_session_id"],
                        "expected_view_revision": view["view_revision"],
                        "hidden_ids": [], "camera_fingerprint": "c3-sync"}, context)


def lean_refresh_workload(client: IpcClient, material_id: str,
                          geometry_id: str, node_id: str) -> dict:
    baseline = client.current()
    view = client.call("view.create", {"hidden_ids": [geometry_id],
                                       "camera_fingerprint": "lean-sync"}, baseline)
    full = render_resource(client, view, baseline)
    fetch_resource(client, full["manifest"])
    old_selection = client.call("selection.evaluate", {
        "view_session_id": view["view_session_id"],
        "expected_view_revision": view["view_revision"],
        "predicate": {"op": "all"}, "scope": {"visibility": "through"}}, baseline)
    cursor = client.call("events.read", {"engine_instance_id": client.instance_id,
                                        "after_sequence": "0", "limit": 64})["current_sequence"]
    client.call("material.set_young_modulus", {
        "entity_id": material_id, "young_modulus": {"value": 203000, "unit": "MPa"}},
        baseline, "c3-lean-material")
    current = client.current()
    history = client.call("history.list", context=current)
    event_params = {"engine_instance_id": client.instance_id,
                    "after_sequence": cursor, "limit": 64}
    legacy_events = client.call("events.read", event_params)
    events = client.call("events.read", {**event_params, "include_document_summary": True})
    changed = [event for event in events["events"] if event["event"] == "DocumentChanged"]
    check(len(changed) == 1 and changed[0]["revision"] == current["revision"] and
          changed[0]["data"]["base_revision"] == baseline["revision"] and
          changed[0]["data"]["document_summary"] == current,
          "opted-in event did not carry the exact same-version authoritative summary")
    legacy_copy = json.loads(json.dumps(events))
    for event in legacy_copy["events"]:
        event["data"].pop("document_summary", None)
    check(legacy_copy == legacy_events, "summary opt-in changed legacy event order or fields")
    wrong_summary_type = client.call("events.read", {**event_params, "include_document_summary": "true"},
                                     expected=None)
    check(wrong_summary_type.get("status") == "failed", "nonboolean summary opt-in succeeded")
    parameters = {"view_session_id": view["view_session_id"],
                  "expected_view_revision": view["view_revision"],
                  "base_revision": baseline["revision"],
                  "base_view_revision": view["view_revision"],
                  "render_wire_version": 3, "allow_inline_empty": True,
                  "allow_model_rebase": True, "include_changed_rows": True}
    legacy = {key: value for key, value in parameters.items()
              if key not in {"allow_model_rebase", "include_changed_rows"}}
    check(client.call("view.render_resource", legacy, current,
                      extra={"requested_version": 1}, expected=None).get("status") == "conflict",
          "a legacy render request silently rebound a stale model")
    malformed = [{**parameters, field: "true"}
                 for field in ("allow_model_rebase", "include_changed_rows")]
    malformed += [{key: value for key, value in parameters.items() if key != missing}
                  for missing in ("base_revision", "base_view_revision")]
    malformed += [{**parameters, "base_revision": str(integer(baseline["revision"]) - 1)},
                  {**parameters, "expected_view_revision": str(integer(view["view_revision"]) + 1)},
                  {**parameters, "base_view_revision": str(integer(view["view_revision"]) + 1)},
                  {**parameters, "hidden_ids": []}]
    refusals = []
    for candidate in malformed:
        response = client.call("view.render_resource", candidate, current,
                               extra={"requested_version": 1}, expected=None)
        check(response.get("status") != "success", "invalid rebase request changed the view")
        refusals.append(response["error"]["code"])
    for key in ("document_id", "document_epoch"):
        response = client.call("view.render_resource", parameters, {**current, key: "wrong-identity"},
                               extra={"requested_version": 1}, expected=None)
        check(response.get("status") != "success", "cross-document/epoch rebase succeeded")
        refusals.append(response["error"]["code"])
    refreshed = client.call("view.render_resource", parameters, current,
                            extra={"requested_version": 1})
    next_view = {"view_session_id": view["view_session_id"],
                 "view_revision": str(integer(view["view_revision"]) + 1)}
    target = {"document_id": current["document_id"], "document_epoch": current["document_epoch"],
              "revision": current["revision"], **next_view}
    check(refreshed.get("mode") == "version_only" and "manifest" not in refreshed and
          refreshed["acknowledgement"] == {**target, "base_revision": baseline["revision"],
                                             "base_view_revision": view["view_revision"]},
          "material rebase did not advance precisely once with an empty pinned-free delta")
    row = client.call("entity.query", {"ids": [material_id]}, current)["entities"]
    check(refreshed.get("rows_complete") is True and refreshed.get("refresh_tree") is False and
          refreshed.get("rows_version") == target and refreshed.get("changed_ids") == [material_id] and
          refreshed.get("changed_rows") == row,
          "render rows differed from the same-version production entity serializer")
    for operation, selection_params in (
            ("selection.get", {"selection_handle": old_selection["selection_handle"]}),
            ("selection.combine", {**next_view, "expected_view_revision": next_view["view_revision"],
                                   "left": old_selection["selection_handle"],
                                   "right": old_selection["selection_handle"], "operator": "union"})):
        selection_params.pop("view_revision", None)
        response = client.call(operation, selection_params, current, expected=None)
        check(response.get("status") == "conflict", "rebase accepted a stale selection handle")
    repeated = client.call("view.render_resource", parameters, current,
                           extra={"requested_version": 1}, expected=None)
    check(repeated.get("status") == "conflict", "same stale rebase advanced its version twice")
    unchanged = client.call("view.update", {"view_session_id": next_view["view_session_id"],
                                            "expected_view_revision": next_view["view_revision"],
                                            "hidden_ids": [geometry_id], "camera_fingerprint": "lean-sync"}, current)
    check(all(unchanged[key] == value for key, value in next_view.items()) and
          unchanged["hidden_ids"] == [geometry_id] and unchanged["camera_fingerprint"] == "lean-sync" and
          unchanged["revision"] == current["revision"] and client.current() == current and
          client.call("history.list", context=current) == history,
          "rebase changed hidden/camera intent or model history")
    client.call("node.move", {"entity_id": node_id, "position_mm": [500, 2, 0]}, current,
                "c3-lean-node")
    moved = client.current()
    node_parameters = {**parameters, "expected_view_revision": next_view["view_revision"],
                       "base_revision": current["revision"], "base_view_revision": next_view["view_revision"]}
    node_resource = client.call("view.render_resource", node_parameters, moved,
                                extra={"requested_version": 1})
    node_bytes, node_transfer = fetch_resource(client, node_resource["manifest"])
    delta = decode_delta(node_bytes)
    check(node_resource.get("mode") == "delta" and node_resource.get("rows_complete") is True and
          delta["base_revision"] == integer(current["revision"]) and delta["revision"] == integer(moved["revision"]) and
          delta["view_revision"] == integer(next_view["view_revision"]) + 1 and
          len(delta["points"]) == 1 and delta["points"][0]["entity_id"] == node_id and
          delta["points"][0]["position_mm"] == [500, 2, 0],
          "nonempty rebase did not preserve its exact local binary delta")
    moved_view = {"view_session_id": next_view["view_session_id"],
                  "view_revision": str(delta["view_revision"])}
    client.call("node.move", {"entity_id": node_id, "position_mm": [500, 1, 0]}, moved,
                "c3-lean-node-restore")
    restored = client.current()
    restored_view = update_view(client, moved_view, restored)
    restored_resource = render_resource(client, restored_view, restored, moved["revision"], moved_view["view_revision"])
    fetch_resource(client, restored_resource["manifest"])
    return {"summary": changed[0], "legacy_events_preserved": True, "rebase_refusals": refusals,
            "material": refreshed, "nonempty_node_delta": delta, "node_transfer": node_transfer,
            "unchanged_hidden_camera_and_model_history": True, "node_fixture_restored": True}


def wait_for_mesh(client: IpcClient, context: dict, geometry_id: str) -> dict:
    task = client.call("mesh.generate_line", {"geometry_id": geometry_id, "segments": 2},
                       context, "c3-sync-basic-mesh")
    task_id = task["task_id"]
    deadline = time.monotonic() + 20
    while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
        check(time.monotonic() < deadline, f"mesh generation timed out: {task}")
        time.sleep(.01)
        task = client.call("task.status", {"task_id": task_id}, client.current())
    check(task["state"] == "succeeded", f"small mesh setup task did not succeed: {task}")
    return task


def wait_for_cancel(client: IpcClient, task_id: str, context: dict, desired: str,
                    timeout: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        task = client.call("task.status", {"task_id": task_id}, context)
        if task["state"] == desired:
            return task
        check(time.monotonic() < deadline,
              f"task {task_id} did not reach {desired}: {task}")
        time.sleep(.001)


def task_ack_workload(client: IpcClient, context: dict, geometry_id: str, *, warmups: int = 5) -> dict:
    start_samples: list[float] = []
    cancel_samples: list[float] = []
    records: list[dict] = []
    measured = 50
    for index in range(warmups + measured):
        key = f"c3-sync-task-ack-{index}"
        started = time.perf_counter_ns()
        task = client.call("mesh.generate_line",
                           {"geometry_id": geometry_id, "segments": 100000},
                           context, key)
        start_ms = (time.perf_counter_ns() - started) / 1_000_000
        check(task.get("state") in {"queued", "running"},
              f"task start ACK was not queued/running: {task}")
        task_id = task["task_id"]
        running = wait_for_cancel(client, task_id, context, "running")
        cancel_start = time.perf_counter_ns()
        cancelled = client.call("task.cancel", {"task_id": task_id}, context)
        cancel_ms = (time.perf_counter_ns() - cancel_start) / 1_000_000
        check(cancelled.get("accepted") is True and cancelled.get("state") == "cancel_requested",
              f"cancel ACK was not the requested cancel_requested state: {cancelled}")
        final = wait_for_cancel(client, task_id, context, "cancelled")
        record = {"iteration": index, "warmup": index < warmups,
                  "start_ack_state": task["state"], "start_ack_ms": start_ms,
                  "running_observed_state": running["state"],
                  "cancel_ack_state": cancelled["state"], "cancel_ack_ms": cancel_ms,
                  "terminal_state_after_ack": final["state"]}
        records.append(record)
        if index >= warmups:
            start_samples.append(start_ms)
            cancel_samples.append(cancel_ms)
    rank = math.ceil(.95 * measured) - 1
    start_p95 = sorted(start_samples)[rank]
    cancel_p95 = sorted(cancel_samples)[rank]
    return {"warmups": warmups, "sample_count": measured,
            "start_ack_ms": start_samples, "cancel_ack_ms": cancel_samples,
            "start_ack_p95_ms": start_p95, "cancel_ack_p95_ms": cancel_p95,
            "limit_ms": TASK_ACK_LIMIT_MS,
            "passed": start_p95 <= TASK_ACK_LIMIT_MS and cancel_p95 <= TASK_ACK_LIMIT_MS,
            "records": records}


def assert_resource_negative(client: IpcClient, manifest: dict, current: dict) -> list[dict]:
    context = {"document_id": current["document_id"],
               "document_epoch": current["document_epoch"], "revision": current["revision"]}
    params = {"resource_id": manifest["resource_id"],
              "view_session_id": manifest["view_session_id"],
              "expected_view_revision": manifest["view_revision"]}
    probes = []

    def reject(label: str, parameters: dict, target_context: dict, code: str | None = None) -> None:
        result = client.call("resources.read", parameters, target_context,
                             extra={"requested_version": 1}, expected=None)
        check(result.get("status") in {"failed", "conflict"},
              f"negative resource sample {label} was accepted: {result}")
        actual = result.get("error", {}).get("code")
        if code:
            check(actual == code, f"negative resource sample {label} returned {actual}, expected {code}")
        probes.append({"sample": label, "status": result.get("status"), "code": actual,
                       "rejected": True})

    reject("missing-offset", params, context)
    reject("unaligned-offset", {**params, "offset": "1"}, context)
    reject("out-of-range-offset", {**params, "offset": manifest["byte_length"]}, context)
    reject("wrong-document", {**params, "offset": "0"},
           {**context, "document_id": "not-the-current-document"})
    reject("wrong-epoch", {**params, "offset": "0"},
           {**context, "document_epoch": "not-the-current-epoch"})
    reject("wrong-revision", {**params, "offset": "0"},
           {**context, "revision": str(integer(context["revision"]) + 1)})
    return probes


def cli_probe(cli: str | None, endpoint: str) -> dict | None:
    if not cli:
        return None
    request = {"api_version": "1.1", "request_id": "c3-sync-cli-probe",
               "operation": "capabilities.list", "parameters": {}}
    result = subprocess.run([cli, "--socket", endpoint, "--no-start"],
                            input=json.dumps(request), text=True, capture_output=True,
                            timeout=20)
    check(bool(result.stdout), f"CLI probe returned no JSON: {result.stderr}")
    response = json.loads(result.stdout)
    check(result.returncode == 0 and response.get("status") == "success",
          f"CLI could not connect to the SQLite engine: {response} {result.stderr}")
    return {"status": response["status"], "return_code": result.returncode}


def run(engine: str, cli: str | None, evidence: Path | None) -> dict:
    summary: dict = {"acceptance": "C3-resource-event-task-IPC", "passed": False,
                     "resource_roundtrips": [], "resource_negative_samples": [],
                     "task_ack": None, "event_gap": None, "requests": 0}
    # Keep the Unix-domain endpoint and SQLite path short and canonical.
    with tempfile.TemporaryDirectory(prefix="qc3-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint, workspace = str(root / "engine.sock"), str(root / "work.sqlite")
        with (root / "engine.log").open("w+") as log:
            host = EngineProcess(engine, endpoint, workspace, log)
            try:
                capabilities = host.start()
                check(capabilities.get("durable") is True and
                      capabilities.get("storage_mode") == "sqlite",
                      f"IPC acceptance is not using the real SQLite engine: {capabilities}")
                summary["engine_instance_id"] = host.client.instance_id
                summary["storage_mode"] = capabilities["storage_mode"]
                summary["cli_probe"] = cli_probe(cli, endpoint)

                context = host.client.call("project.create", {"name": "C3 sync IPC"},
                                           key="c3-sync-project")
                line = host.client.call("geometry.create_line",
                                        {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]},
                                        context, "c3-sync-line")
                geometry_id = line["entity_id"]
                material = host.client.call(
                    "material.create",
                    {"name": "C3 sync steel", "young_modulus": {"value": 210, "unit": "GPa"},
                     "poisson_ratio": 0.3},
                    host.client.current(), "c3-sync-material")
                view = host.client.call("view.create",
                                        {"hidden_ids": [], "camera_fingerprint": "c3-sync"},
                                        host.client.current())

                direct_geometry = host.client.call(
                    "view.render_data",
                    {"view_session_id": view["view_session_id"],
                     "expected_view_revision": view["view_revision"]},
                    host.client.current())
                check(direct_geometry.get("points") == [] and direct_geometry.get("beams") == [] and
                      direct_geometry.get("geometry_lines") == [
                          {"entity_id": geometry_id, "start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]}],
                      "initial geometry display fabricated or lost entities")
                full = render_resource(host.client, view, host.client.current())
                check(full.get("mode") == "full", f"first render resource was not full: {full}")
                manifest = full["manifest"]
                check(manifest["media_type"] == "qcae.render.packet.v1",
                      f"full render packet has wrong media type: {manifest}")
                packet_bytes, transfer = fetch_resource(host.client, manifest)
                packet = decode_packet(packet_bytes)
                check(packet["document_id"] == manifest["document_id"] and
                      packet["document_epoch"] == manifest["document_epoch"] and
                      packet["revision"] == integer(manifest["revision"]) and
                      packet["view_session_id"] == view["view_session_id"] and
                      packet["view_revision"] == integer(view["view_revision"]),
                      "full packet bytes disagree with the manifest's document/view version")
                check(packet["points"] == direct_geometry["points"] and
                      packet["beams"] == direct_geometry["beams"] and
                      packet["geometry_lines"] == direct_geometry["geometry_lines"],
                      "production binary render packet differs from the authoritative JSON render")
                summary["resource_roundtrips"].append({"mode": "full-geometry-only",
                                                       **transfer,
                                                       "stable_geometry_id": geometry_id,
                                                       "fake_points": len(packet["points"]),
                                                       "fake_beams": len(packet["beams"])})

                old_manifest = manifest
                mesh = wait_for_mesh(host.client, host.client.current(), geometry_id)
                check(mesh["state"] == "succeeded", "two-segment display setup did not finish")
                after_mesh = host.client.current()
                view = update_view(host.client, view, after_mesh)
                structural = render_resource(host.client, view, after_mesh,
                                            old_manifest["revision"], old_manifest["view_revision"])
                check(structural.get("mode") == "full",
                      "mesh topology change was incorrectly represented as a point delta")
                structural_manifest = structural["manifest"]
                structural_bytes, structural_transfer = fetch_resource(host.client,
                                                                       structural_manifest)
                structural_packet = decode_packet(structural_bytes)
                check(len(structural_packet["points"]) == 3 and
                      structural_packet["beams"] == [] and len(structural_packet["cells"]) == 2 and
                      all(cell["kind"] == 0 and len(cell["points"]) == 2 for cell in structural_packet["cells"]) and
                      [item["entity_id"] for item in structural_packet["geometry_lines"]] == [geometry_id],
                      "mesh display lost geometry or generated FE entities")
                summary["resource_roundtrips"].append({"mode": "full-after-topology-change",
                                                       **structural_transfer,
                                                       "points": len(structural_packet["points"]),
                                                       "generic_line_cells": len(structural_packet["cells"]),
                                                       "geometry_id_preserved": geometry_id})

                mesh_view = view
                mesh_context = after_mesh
                node_rows = host.client.call("entity.query", {"kind": "node"}, after_mesh)["entities"]
                check(len(node_rows) == 3, f"two line segments did not create three nodes: {node_rows}")
                node_fields = {row["entity_id"]: host.client.call(
                    "entity.fields", {"entity_id": row["entity_id"]}, after_mesh)["fields"]
                               for row in node_rows}
                middle = next(identity for identity, fields in node_fields.items()
                              if fields["position"][0] == 500)
                before_node = host.client.current()
                host.client.call("node.move",
                                 {"entity_id": middle, "position_mm": [500, 1, 0]},
                                 before_node, "c3-sync-move")
                after_move = host.client.current()
                moved_view = update_view(host.client, mesh_view, after_move)
                move_resource = render_resource(host.client, moved_view, after_move,
                                               mesh_context["revision"], mesh_view["view_revision"])
                check(move_resource.get("mode") == "delta",
                      f"single node edit was not emitted as a render delta: {move_resource}")
                move_manifest = move_resource["manifest"]
                check(move_manifest["media_type"] == "qcae.render.delta.v1",
                      "node render delta has wrong media type")
                move_bytes, move_transfer = fetch_resource(host.client, move_manifest)
                delta = decode_delta(move_bytes)
                check(delta["base_revision"] == integer(mesh_context["revision"]) and
                      delta["revision"] == integer(after_move["revision"]) and
                      delta["base_view_revision"] == integer(mesh_view["view_revision"]) and
                      delta["view_revision"] == integer(moved_view["view_revision"]),
                      "node render delta lost base or current version context")
                check(len(delta["points"]) == 1 and delta["points"][0]["entity_id"] == middle and
                      delta["points"][0]["position_mm"] == [500, 1, 0] and
                      delta["geometry_lines"] == [],
                      f"node render delta did not contain exactly its local coordinate edit: {delta}")
                summary["resource_roundtrips"].append({"mode": "delta-node-move",
                                                       **move_transfer,
                                                       "stable_node_id": middle,
                                                       "changed_points": len(delta["points"])})

                # A material edit advances current versions but does not resend display geometry.
                old_view = moved_view
                old_context = after_move
                host.client.call("material.set_young_modulus",
                                 {"entity_id": material["entity_id"],
                                  "young_modulus": {"value": 200000, "unit": "MPa"}},
                                 host.client.current(), "c3-sync-material-edit")
                after_material = host.client.current()
                material_view = update_view(host.client, old_view, after_material)
                material_resource = render_resource(host.client, material_view, after_material,
                                                    old_context["revision"], old_view["view_revision"])
                check(material_resource.get("mode") == "delta",
                      "material-only local edit triggered a full render packet")
                material_manifest = material_resource["manifest"]
                material_bytes, material_transfer = fetch_resource(host.client, material_manifest)
                material_delta = decode_delta(material_bytes)
                check(material_delta["points"] == [] and material_delta["geometry_lines"] == [],
                      "material-only update resent render records")
                summary["resource_roundtrips"].append({"mode": "delta-material-metadata-only",
                                                       **material_transfer,
                                                       "changed_points": 0,
                                                       "changed_geometry_lines": 0})
                inline_parameters = {"view_session_id": material_view["view_session_id"],
                                     "expected_view_revision": material_view["view_revision"],
                                     "base_revision": after_material["revision"],
                                     "base_view_revision": material_view["view_revision"],
                                     "allow_inline_empty": True}
                acknowledged = host.client.call("view.render_resource", inline_parameters,
                                                 after_material, extra={"requested_version": 1})
                check(acknowledged.get("mode") == "version_only" and "manifest" not in acknowledged and
                      acknowledged.get("changed_ids") == [] and acknowledged.get("refresh_tree") is False,
                      "opted-in empty delta created a binary resource or tree rebuild")
                expected_ack = {"document_id": after_material["document_id"],
                                "document_epoch": after_material["document_epoch"],
                                "revision": after_material["revision"],
                                "view_session_id": material_view["view_session_id"],
                                "view_revision": material_view["view_revision"],
                                "base_revision": after_material["revision"],
                                "base_view_revision": material_view["view_revision"]}
                check(acknowledged.get("acknowledgement") == expected_ack,
                      "empty acknowledgement lost exact from/to authority")
                invalid_inline = {**inline_parameters, "allow_inline_empty": "true"}
                refused_inline = host.client.call("view.render_resource", invalid_inline, after_material,
                                                  extra={"requested_version": 1}, expected=None)
                check(refused_inline.get("status") == "failed" and
                      refused_inline.get("error", {}).get("code") == "INVALID_INPUT",
                      "nonboolean empty-ack opt-in was accepted")
                summary["version_only_acknowledgement"] = acknowledged

                stale = host.client.call(
                    "resources.read",
                    {"resource_id": move_manifest["resource_id"],
                     "view_session_id": move_manifest["view_session_id"],
                     "expected_view_revision": move_manifest["view_revision"], "offset": "0"},
                    {"document_id": move_manifest["document_id"],
                     "document_epoch": move_manifest["document_epoch"],
                     "revision": move_manifest["revision"]},
                    extra={"requested_version": 1}, expected=None)
                check(stale.get("status") == "conflict" and
                      stale.get("error", {}).get("code") == "REVISION_CONFLICT",
                      f"stale render resource was not refused: {stale}")
                summary["resource_negative_samples"] = assert_resource_negative(
                    host.client, material_manifest, after_material)

                # An old client sends no wire capability. Actual full+visibility
                # responses preserve the original Line2 IDs and v1/v2 codec.
                legacy_view = host.client.call("view.create", {"hidden_ids": [], "camera_fingerprint": "legacy-wire"}, after_material)
                legacy_full = render_resource(host.client, legacy_view, after_material, wire_version=None)
                legacy_bytes, legacy_transfer = fetch_resource(host.client, legacy_full["manifest"])
                legacy_packet = decode_packet(legacy_bytes)
                check(legacy_full["manifest"]["media_type"] == "qcae.render.packet.v1" and
                      legacy_packet["cells"] == [] and len(legacy_packet["beams"]) == 2 and
                      {beam["entity_id"] for beam in legacy_packet["beams"]} ==
                      {cell["entity_id"] for cell in structural_packet["cells"]},
                      "old client full Line2 resource was not represented in v1/v2")
                legacy_hidden_id = legacy_packet["beams"][0]["entity_id"]
                legacy_hidden = host.client.call("view.update", {"view_session_id": legacy_view["view_session_id"],
                    "expected_view_revision": legacy_view["view_revision"], "hidden_ids": [legacy_hidden_id],
                    "camera_fingerprint": "legacy-wire"}, after_material)
                legacy_delta = render_resource(host.client, legacy_hidden, after_material,
                                               after_material["revision"], legacy_view["view_revision"], wire_version=None)
                legacy_delta_bytes, _ = fetch_resource(host.client, legacy_delta["manifest"])
                legacy_visibility = decode_delta(legacy_delta_bytes)
                check(legacy_delta["mode"] == "delta" and legacy_delta["manifest"]["media_type"] == "qcae.render.delta.v2" and
                      legacy_visibility["visibility"] == [{"primitive": 1, "index": 0, "entity_id": legacy_hidden_id, "visible": False}],
                      "old client visibility did not retain beam index/identity in v2")
                for refused_version in (1, 4):
                    refused_wire = host.client.call("view.render_resource", {"view_session_id": legacy_hidden["view_session_id"],
                        "expected_view_revision": legacy_hidden["view_revision"], "render_wire_version": refused_version}, after_material,
                        extra={"requested_version": 1}, expected=None)
                    check(refused_wire.get("status") == "failed" and refused_wire.get("error", {}).get("code") == "UNSUPPORTED_CAPABILITY",
                          "unsupported old/future wire version silently lost a visible primitive")
                summary["legacy_wire_roundtrip"] = {"full": legacy_transfer, "visibility": legacy_visibility}

                # Visibility toggles use the same model revision and retain topology/coordinates.
                hidden_ids = [middle, structural_packet["cells"][0]["entity_id"], geometry_id]
                hidden_view = host.client.call(
                    "view.update", {"view_session_id": material_view["view_session_id"],
                                    "expected_view_revision": material_view["view_revision"],
                                    "hidden_ids": hidden_ids, "camera_fingerprint": "c3-sync"},
                    after_material)
                hidden_resource = render_resource(host.client, hidden_view, after_material,
                                                  after_material["revision"], material_view["view_revision"])
                check(hidden_resource["mode"] == "delta" and not hidden_resource["refresh_tree"] and
                      hidden_resource["manifest"]["media_type"] == "qcae.render.delta.v3",
                      "visibility update rebuilt scene or tree")
                hidden_bytes, hidden_transfer = fetch_resource(host.client, hidden_resource["manifest"])
                hidden_delta = decode_delta(hidden_bytes)
                check(hidden_delta["points"] == [] and hidden_delta["geometry_lines"] == [] and
                      {row["entity_id"] for row in hidden_delta["visibility"]} == set(hidden_ids) and
                      all(row["visible"] is False for row in hidden_delta["visibility"]),
                      "visibility delta resent coordinates or lost identities")
                shown_view = update_view(host.client, hidden_view, after_material)
                shown_resource = render_resource(host.client, shown_view, after_material,
                                                 after_material["revision"], hidden_view["view_revision"])
                shown_bytes, _ = fetch_resource(host.client, shown_resource["manifest"])
                shown_delta = decode_delta(shown_bytes)
                check(shown_resource["mode"] == "delta" and
                      all(row["visible"] is True for row in shown_delta["visibility"]) and
                      len(shown_delta["visibility"]) == 3,
                      "unhide failed to reuse retained entity indices")
                summary["resource_roundtrips"].append({"mode": "delta-visibility",
                                                       **hidden_transfer, "visible_updates": 3})

                # Start and cancel real background mesh jobs over IPC, timing only the ACKs.
                summary["task_ack"] = task_ack_workload(host.client, after_material, geometry_id)

                summary["lean_refresh"] = lean_refresh_workload(
                    host.client, material["entity_id"], geometry_id, middle)

                # Read a cursor, deliberately leave it behind more than the 256-event retention.
                event_baseline = host.client.call(
                    "events.read", {"engine_instance_id": host.client.instance_id,
                                    "after_sequence": "0", "limit": 64})
                cursor = integer(event_baseline["current_sequence"])
                # The engine's durable history has a 128-entry cap. Exercise 64
                # forward edits plus 64 undo/redo pairs to exceed the 256-event
                # retention while keeping the model history within that cap.
                for index in range(64):
                    current = host.client.current()
                    value = 211000 if index % 2 == 0 else 212000
                    host.client.call("material.set_young_modulus",
                                     {"entity_id": material["entity_id"],
                                      "young_modulus": {"value": value, "unit": "MPa"}},
                                     current, f"c3-sync-event-gap-{index}")
                for index in range(64):
                    current = host.client.current()
                    host.client.call("history.undo", {}, current,
                                     f"c3-sync-event-gap-undo-{index}")
                for index in range(64):
                    current = host.client.current()
                    host.client.call("history.redo", {}, current,
                                     f"c3-sync-event-gap-redo-{index}")
                gap = host.client.call(
                    "events.read", {"engine_instance_id": host.client.instance_id,
                                    "after_sequence": str(cursor), "limit": 64})
                check(gap.get("resync_required") is True and gap.get("events") == [],
                      f"event-retention gap was not reported: {gap}")
                reconnected = host.client.call(
                    "events.subscribe", {"engine_instance_id": host.client.instance_id,
                                         "after_sequence": str(cursor), "limit": 64})
                check(reconnected.get("resync_required") is True and
                      integer(reconnected.get("next_sequence")) == integer(gap["current_sequence"]),
                      f"reconnect did not request an authoritative resync: {reconnected}")
                authoritative = host.client.current()
                fresh_view = host.client.call(
                    "view.create", {"hidden_ids": [], "camera_fingerprint": "c3-sync-reconnect"},
                    authoritative)
                fresh_resource = render_resource(host.client, fresh_view, authoritative)
                check(fresh_resource.get("mode") == "full",
                      "reconnect after a retained-event gap did not produce a fresh full view")
                fresh_manifest = fresh_resource["manifest"]
                fresh_bytes, fresh_transfer = fetch_resource(host.client, fresh_manifest)
                fresh_packet = decode_packet(fresh_bytes)
                check(fresh_packet["revision"] == integer(authoritative["revision"]) and
                      fresh_packet["document_id"] == authoritative["document_id"] and
                      fresh_packet["document_epoch"] == authoritative["document_epoch"] and
                      fresh_manifest["revision"] == authoritative["revision"],
                      "resynchronized render resource does not match authoritative current versions")
                fresh_fields = host.client.call("entity.fields", {"entity_id": middle}, authoritative)
                restored_position = fresh_fields["fields"]["position"]
                check(restored_position == [500, 1, 0],
                      "stable node identity/value changed across event-gap resynchronization")
                summary["resource_roundtrips"].append({"mode": "full-after-event-gap-resync",
                                                       **fresh_transfer,
                                                       "current_revision": authoritative["revision"],
                                                       "document_id": authoritative["document_id"],
                                                       "document_epoch": authoritative["document_epoch"]})
                summary["event_gap"] = {
                    "cursor_before_gap": cursor,
                    "current_sequence": integer(gap["current_sequence"]),
                    "oldest_sequence": integer(gap["oldest_sequence"]),
                    "dropped_retained_events": True,
                    "resync_required": True,
                    "reconnect_required_resync": True,
                    "authoritative_revision": authoritative["revision"],
                    "fresh_full_resource_revision": fresh_manifest["revision"],
                    "stable_node_id_preserved": middle,
                }
                summary["requests"] = len(host.client.transcript)
                summary["max_resource_request_frame_bytes"] = max(
                    item["request_frame_bytes"] for item in host.client.transcript
                    if item["request"].get("operation") == "resources.read")
                summary["max_resource_response_frame_bytes"] = max(
                    item["response_frame_bytes"] for item in host.client.transcript
                    if item["request"].get("operation") == "resources.read")
                summary["digest_negative_sample"] = (
                    "covered by qcae_resource_tests ResourceClient corruption test over local socket")
                summary["task_ack_passed"] = summary["task_ack"]["passed"]
                summary["passed"] = summary["task_ack_passed"]
                return summary
            finally:
                host.close()
                if evidence:
                    evidence.mkdir(parents=True, exist_ok=True)
                    (evidence / "c3-sync-ipc-transcript.json").write_text(
                        json.dumps(host.client.transcript, indent=2, ensure_ascii=False) + "\n")
                    (evidence / "c3-sync-ipc-summary.json").write_text(
                        json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
                    log.flush()
                    log.seek(0)
                    (evidence / "c3-sync-engine.log").write_text(log.read())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli")
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    report = run(str(Path(args.engine).resolve()),
                 str(Path(args.cli).resolve()) if args.cli else None,
                 args.evidence_dir)
    print(json.dumps(report, sort_keys=True))
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
        (args.evidence_dir / "c3-sync-ipc-summary.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
