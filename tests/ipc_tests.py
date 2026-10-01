#!/usr/bin/env python3
"""Black-box integration checks for the QCAE local engine and CLI IPC."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
from pathlib import Path
from typing import Any


ENGINE: str
CLI: str
TIMEOUT = 15.0


class CheckFailure(AssertionError):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def require_mapping(value: Any, description: str) -> dict[str, Any]:
    check(isinstance(value, dict), f"{description} must be a JSON object; got {value!r}")
    return value


def require_field(mapping: dict[str, Any], field: str, description: str) -> Any:
    check(field in mapping, f"{description} is missing required field {field!r}: {mapping!r}")
    return mapping[field]


def string_field(mapping: dict[str, Any], field: str, description: str) -> str:
    value = require_field(mapping, field, description)
    check(isinstance(value, str) and value, f"{description}.{field} must be a non-empty string")
    return value


def json_response(proc: subprocess.CompletedProcess[str], request_id: str) -> dict[str, Any]:
    check(proc.stdout.strip(), f"CLI emitted no JSON (exit={proc.returncode}, stderr={proc.stderr!r})")
    try:
        response = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise CheckFailure(f"CLI stdout is not one JSON response: {proc.stdout!r}") from exc
    require_mapping(response, "response")
    check(response.get("request_id") == request_id,
          f"response request_id mismatch: expected {request_id!r}, got {response.get('request_id')!r}")
    return response


def call(
    socket_path: str,
    operation: str,
    parameters: dict[str, Any] | None = None,
    *,
    document_id: str | None = None,
    document_epoch: str | None = None,
    expected_revision: str | None = None,
    idempotency_key: str | None = None,
    extra: dict[str, Any] | None = None,
    expected_status: str = "success",
) -> dict[str, Any]:
    request_id = f"ipc-test-{uuid.uuid4()}"
    request: dict[str, Any] = {
        "api_version": "1.1",
        "request_id": request_id,
        "operation": operation,
        "parameters": parameters if parameters is not None else {},
    }
    if document_id is not None:
        request["document_id"] = document_id
    if document_epoch is not None:
        request["document_epoch"] = document_epoch
    if expected_revision is not None:
        request["expected_revision"] = expected_revision
    if idempotency_key is not None:
        request["idempotency_key"] = idempotency_key
    if extra:
        request.update(extra)

    try:
        proc = subprocess.run(
            [CLI, "--socket", socket_path, "--no-start"],
            input=json.dumps(request, separators=(",", ":")) + "\n",
            text=True,
            capture_output=True,
            timeout=TIMEOUT,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise CheckFailure(f"CLI timed out for {operation}") from exc

    response = json_response(proc, request_id)
    status = response.get("status")
    check(status == expected_status,
          f"{operation}: expected status {expected_status!r}, got {status!r}; response={response!r}")
    expected_exit = 0 if expected_status == "success" else 2
    check(proc.returncode == expected_exit,
          f"{operation}: status {status!r} requires exit {expected_exit}, got {proc.returncode}; "
          f"stderr={proc.stderr!r}, response={response!r}")
    return response


def data_of(response: dict[str, Any], operation: str) -> dict[str, Any]:
    return require_mapping(require_field(response, "data", operation), f"{operation}.data")


def error_code(response: dict[str, Any], operation: str) -> str:
    error = require_mapping(require_field(response, "error", operation), f"{operation}.error")
    return string_field(error, "code", f"{operation}.error")


def start_engine(socket_path: str, stdout: Any, stderr: Any) -> subprocess.Popen[bytes]:
    return subprocess.Popen(
        [ENGINE, "--socket", socket_path],
        stdin=subprocess.DEVNULL,
        stdout=stdout,
        stderr=stderr,
        close_fds=True,
    )


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def wait_for_cli(socket_path: str, engine: subprocess.Popen[bytes]) -> dict[str, Any]:
    deadline = time.monotonic() + TIMEOUT
    last_problem = "engine has not accepted a CLI request yet"
    while time.monotonic() < deadline:
        exit_code = engine.poll()
        if exit_code is not None:
            raise CheckFailure(f"engine exited before IPC became ready (exit={exit_code})")
        try:
            return call(socket_path, "capabilities.list")
        except CheckFailure as exc:
            last_problem = str(exc)
            time.sleep(0.05)
    raise CheckFailure(f"IPC readiness timed out after {TIMEOUT:g}s: {last_problem}")


def check_summary(socket_path: str, document_id: str, epoch: str, revision: str,
                  expected_materials: int) -> dict[str, Any]:
    response = call(
        socket_path,
        "model.summary",
        document_id=document_id,
        document_epoch=epoch,
    )
    check(response.get("revision") == revision,
          f"summary revision should be {revision}, got {response.get('revision')!r}")
    data = data_of(response, "model.summary")
    materials = require_field(data, "materials", "model.summary.data")
    check(isinstance(materials, list), f"summary materials must be a list, got {materials!r}")
    check(len(materials) == expected_materials,
          f"summary should contain {expected_materials} materials, found {len(materials)}")
    return response


def check_revision(response: dict[str, Any], expected: str, operation: str) -> dict[str, Any]:
    check(response.get("revision") == expected,
          f"{operation}: top-level revision should be {expected}, got {response.get('revision')!r}")
    return data_of(response, operation)


def integration_scenario(socket_path: str, engine_stdout: Any, engine_stderr: Any) -> None:
    engine = start_engine(socket_path, engine_stdout, engine_stderr)
    try:
        caps_response = wait_for_cli(socket_path, engine)
        caps = data_of(caps_response, "capabilities.list")
        check(caps.get("storage_mode") == "memory",
              f"capabilities.list storage_mode must be 'memory': {caps!r}")
        check(caps.get("durable") is False,
              f"capabilities.list durable must be false: {caps!r}")
        operations = require_field(caps, "operations", "capabilities.list.data")
        check(isinstance(operations, list), "capabilities.list.data.operations must be a list")
        statuses: dict[str, str] = {}
        for entry in operations:
            item = require_mapping(entry, "capability operation")
            name = string_field(item, "name", "capability operation")
            implementation = string_field(item, "implementation_status", f"operation {name}")
            check(implementation in {"implemented", "partial", "planned", "unavailable"},
                  f"operation {name} has invalid implementation_status {implementation!r}")
            if implementation == "unavailable":
                check(item.get("available") is False,
                      f"unavailable operation {name} must not advertise executability")
            statuses[name] = implementation
        for name in ("project.create", "model.summary", "changes.preview", "changes.commit",
                     "history.undo", "history.redo", "history.list", "capabilities.list"):
            check(statuses.get(name) in {"implemented", "partial"},
                  f"implemented M0 operation {name} must not be marked planned; got {statuses.get(name)!r}")
        nastran_available = any(item.get("solver_family") == "Nastran"
                                for item in caps.get("declared_solver_profiles", []))
        expected_export_status = "implemented" if nastran_available else "planned"
        check(statuses.get("model.export") == expected_export_status,
              f"model.export package status must be {expected_export_status!r}, "
              f"got {statuses.get('model.export')!r}")

        create_key = "ipc-project-create"
        create1 = call(socket_path, "project.create", {"name": "demo"},
                       idempotency_key=create_key)
        check_revision(create1, "0", "project.create")
        create_data = data_of(create1, "project.create")
        document_id = string_field(create_data, "document_id", "project.create.data")
        epoch = string_field(create_data, "document_epoch", "project.create.data")
        if statuses.get("analysis.start") == "unavailable" and nastran_available:
            profile = next(item["profile_ref"] for item in caps["declared_solver_profiles"]
                           if item.get("solver_family") == "Nastran")
            unavailable = call(socket_path, "analysis.start", {"artifact_id": "never-start"},
                               document_id=document_id, document_epoch=epoch, expected_revision="0",
                               idempotency_key="unconfigured-solver", extra={"expected_profile": profile},
                               expected_status="failed")
            check(unavailable["error"]["code"] == "UNSUPPORTED_CAPABILITY",
                  "an unavailable solver capability must reject before execution")
            check(data_of(call(socket_path, "project.current"), "project.current")["revision"] == "0",
                  "unavailable solver invocation must not mutate the model")
        check(create_data.get("revision") == "0", "project.create data revision must be '0'")
        check(create_data.get("durable") is False, "project.create data durable must be false")

        create2 = call(socket_path, "project.create", {"name": "demo"},
                       idempotency_key=create_key)
        check_revision(create2, "0", "project.create retry")
        create2_data = data_of(create2, "project.create retry")
        check(create2_data.get("document_id") == document_id,
              "retrying project.create with the same key must return the original document")
        check(create2_data.get("document_epoch") == epoch,
              "retrying project.create must return the original epoch")
        check_summary(socket_path, document_id, epoch, "0", 0)

        def document_call(operation: str, parameters: dict[str, Any] | None = None,
                          *, revision: str | None = None, key: str | None = None,
                          expected_status: str = "success", extra: dict[str, Any] | None = None) -> dict[str, Any]:
            return call(
                socket_path, operation, parameters,
                document_id=document_id, document_epoch=epoch,
                expected_revision=revision, idempotency_key=key,
                expected_status=expected_status, extra=extra,
            )

        preview_params = {
            "command": "material.create",
            "name": "Steel",
            "young_modulus": {"value": 210, "unit": "GPa"},
        }
        preview = document_call("changes.preview", preview_params, revision="0")
        check_revision(preview, "0", "changes.preview")
        preview_data = data_of(preview, "changes.preview")
        preview_id = string_field(preview_data, "preview_id", "changes.preview.data")
        affected_entity_id = string_field(preview_data, "affected_entity_id", "changes.preview.data")
        modulus = require_field(preview_data, "normalized_young_modulus_mpa", "changes.preview.data")
        check(isinstance(modulus, (int, float)) and abs(float(modulus) - 210000.0) < 1e-9,
              f"210 GPa should normalize to 210000 MPa, got {modulus!r}")

        commit_key = "ipc-material-create"
        commit_params = {"preview_id": preview_id}
        committed = document_call("changes.commit", commit_params, revision="0", key=commit_key)
        receipt = check_revision(committed, "1", "changes.commit")
        transaction_id = string_field(receipt, "transaction_id", "changes.commit.data")
        check(receipt.get("committed_revision") == "1", "commit receipt committed_revision must be '1'")
        check(receipt.get("current_revision") == "1", "commit receipt current_revision must be '1'")
        check(receipt.get("replayed") is False, "first commit must report replayed=false")
        content_state_after_commit = string_field(receipt, "current_content_state", "changes.commit.data")

        # A second CLI process performs each request, so this also checks shared engine state and reconnect.
        check_summary(socket_path, document_id, epoch, "1", 1)

        stale = document_call("changes.commit", {"preview_id": preview_id},
                              revision="0", key="ipc-stale-preview", expected_status="conflict")
        check(error_code(stale, "stale changes.commit") == "REVISION_CONFLICT",
              f"stale revision should return REVISION_CONFLICT: {stale!r}")
        check_summary(socket_path, document_id, epoch, "1", 1)

        replay = document_call("changes.commit", commit_params, revision="0", key=commit_key)
        replay_data = check_revision(replay, "1", "same-key changes.commit retry")
        check(replay_data.get("transaction_id") == transaction_id,
              "same-key retry must return the original transaction")
        check(replay_data.get("replayed") is True, "same-key retry must report replayed=true")
        check(replay_data.get("current_content_state") == content_state_after_commit,
              "same-key retry must report the unchanged current content state")

        looked_up = document_call(
            "operations.get",
            {"lookup_scope": "document", "original_operation": "changes.commit",
             "idempotency_key": commit_key},
        )
        lookup_data = check_revision(looked_up, "1", "operations.get for changes.commit")
        check(lookup_data.get("transaction_id") == transaction_id,
              "operations.get must resolve changes.commit to the original transaction")

        set_preview = document_call(
            "changes.preview",
            {"command": "material.set_young_modulus", "entity_id": affected_entity_id,
             "young_modulus": {"value": 200, "unit": "GPa"}},
            revision="1",
        )
        set_preview_data = data_of(set_preview, "set modulus changes.preview")
        set_preview_id = string_field(set_preview_data, "preview_id", "set modulus changes.preview.data")
        conflicting_replay = document_call(
            "changes.commit", {"preview_id": set_preview_id}, revision="1", key=commit_key,
            expected_status="conflict",
        )
        check(error_code(conflicting_replay, "same-key different-parameters retry") == "IDEMPOTENCY_KEY_CONFLICT",
              f"same key with different parameters must be rejected: {conflicting_replay!r}")
        check_summary(socket_path, document_id, epoch, "1", 1)

        undo = document_call("history.undo", {}, revision="1", key="ipc-undo-create")
        undo_data = check_revision(undo, "2", "history.undo")
        check(undo_data.get("current_revision") == "2", "undo receipt current_revision must be '2'")
        check_summary(socket_path, document_id, epoch, "2", 0)

        replay_after_undo = document_call("changes.commit", commit_params, revision="0", key=commit_key)
        replay_after_undo_data = check_revision(replay_after_undo, "2", "retry after undo")
        check(replay_after_undo_data.get("transaction_id") == transaction_id,
              "retry after undo must return the original transaction")
        check(replay_after_undo_data.get("replayed") is True,
              "retry after undo must report replayed=true")
        check(replay_after_undo_data.get("current_revision") == "2",
              "retry after undo must report the current revision")
        check_summary(socket_path, document_id, epoch, "2", 0)

        redo = document_call("history.redo", {}, revision="2", key="ipc-redo-create")
        redo_data = check_revision(redo, "3", "history.redo")
        check(redo_data.get("current_revision") == "3", "redo receipt current_revision must be '3'")
        check_summary(socket_path, document_id, epoch, "3", 1)

        history = document_call("history.list")
        history_data = data_of(history, "history.list")
        items = require_field(history_data, "items", "history.list.data")
        check(isinstance(items, list), f"history.list.data.items must be a list, got {items!r}")
        require_field(history_data, "cursor", "history.list.data")

        invalid_unit = document_call(
            "changes.preview",
            {"command": "material.create", "name": "BadUnit",
             "young_modulus": {"value": 1, "unit": "fortnight"}},
            revision="3", expected_status="failed",
        )
        check(error_code(invalid_unit, "invalid-unit preview") == "INVALID_UNIT",
              f"unsupported unit should return INVALID_UNIT: {invalid_unit!r}")
        missing_unit = document_call(
            "changes.preview",
            {"command": "material.create", "name": "MissingUnit",
             "young_modulus": {"value": 1}},
            revision="3", expected_status="needs_input",
        )
        check(error_code(missing_unit, "missing-unit preview") == "MISSING_INPUT",
              f"missing unit should return MISSING_INPUT: {missing_unit!r}")

        actor_injection = document_call(
            "changes.preview", preview_params, revision="3", expected_status="failed",
            extra={"actor": {"principal": "forged", "approved": True}, "approved": True},
        )
        check(error_code(actor_injection, "forged actor preview") in {"INVALID_INPUT", "UNAUTHORIZED"},
              f"forged actor fields must be rejected explicitly: {actor_injection!r}")
        check_summary(socket_path, document_id, epoch, "3", 1)

        export = document_call(
            "model.export", {}, revision="3", key="ipc-unsupported-export",
            expected_status="needs_input" if nastran_available else "failed",
        )
        expected_export_error = "MISSING_INPUT" if nastran_available else "UNSUPPORTED_CAPABILITY"
        check(error_code(export, "incomplete model.export") == expected_export_error,
              f"incomplete model.export must report {expected_export_error}: {export!r}")
        if nastran_available:
            check(export["error"].get("field") == "expected_profile",
                  f"incomplete export must identify its missing target profile: {export!r}")
        check_summary(socket_path, document_id, epoch, "3", 1)
    finally:
        stop_process(engine)


def single_instance_check(root: Path, engine_stdout: Any, engine_stderr: Any) -> None:
    socket_path = str(root / "single-instance.sock")
    first = start_engine(socket_path, engine_stdout, engine_stderr)
    second: subprocess.Popen[bytes] | None = None
    try:
        wait_for_cli(socket_path, first)
        second = start_engine(socket_path, engine_stdout, engine_stderr)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline and second.poll() is None:
            time.sleep(0.05)
        check(first.poll() is None, "first engine must remain alive after a duplicate launch")
        check(second.poll() is not None,
              "a second engine launched with the same socket must exit; two instances are active")
    finally:
        stop_process(second)
        stop_process(first)


def receive_json_line(connection: socket.socket) -> dict[str, Any]:
    data = bytearray()
    while b"\n" not in data:
        chunk = connection.recv(65536)
        check(chunk, "engine closed the raw IPC connection before sending a response")
        data.extend(chunk)
        check(len(data) <= 1024 * 1024 + 1, "engine response exceeded the IPC frame limit")
    line = bytes(data).split(b"\n", 1)[0]
    try:
        response = json.loads(line)
    except json.JSONDecodeError as exc:
        raise CheckFailure(f"raw IPC response was not JSON: {line!r}") from exc
    return require_mapping(response, "raw IPC response")


def send_raw(connection: socket.socket, request: bytes) -> dict[str, Any]:
    connection.sendall(request + b"\n")
    return receive_json_line(connection)


def raw_protocol_checks(socket_path: str, engine: subprocess.Popen[bytes]) -> None:
    mismatch = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mismatch.settimeout(3.0)
    try:
        mismatch.connect(socket_path)
        response = send_raw(
            mismatch,
            b'{"api_version":"99.0","request_id":"bad-version",'
            b'"operation":"runtime.handshake"}',
        )
        check(response.get("request_id") == "bad-version", "version mismatch response must retain request_id")
        check(response.get("status") == "failed", f"version mismatch should fail: {response!r}")
        check(error_code(response, "raw version mismatch") == "API_VERSION_UNSUPPORTED",
              f"version mismatch should return API_VERSION_UNSUPPORTED: {response!r}")
    finally:
        mismatch.close()

    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(3.0)
    try:
        connection.connect(socket_path)
        malformed = send_raw(connection, b"{this is not JSON")
        check(malformed.get("status") == "failed", f"malformed JSON should fail: {malformed!r}")
        check(error_code(malformed, "malformed JSON") == "INVALID_JSON",
              f"malformed JSON should return INVALID_JSON: {malformed!r}")
        handshake = send_raw(
            connection,
            b'{"api_version":"1.1","request_id":"raw-handshake",'
            b'"operation":"runtime.handshake"}',
        )
        check(handshake.get("status") == "success", f"engine should accept handshake after malformed input: {handshake!r}")
        handshake_data = data_of(handshake, "raw runtime.handshake")
        check(handshake_data.get("api_version") == "1.1", "raw handshake should return API version 1.1")
        caps = send_raw(
            connection,
            b'{"api_version":"1.1","request_id":"raw-capabilities",'
            b'"operation":"capabilities.list","parameters":{}}',
        )
        check(caps.get("status") == "success", f"engine should process valid request after malformed input: {caps!r}")
        check(engine.poll() is None, "malformed JSON must not terminate the engine")
    finally:
        connection.close()

    oversized = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    oversized.settimeout(5)
    try:
        oversized.connect(socket_path)
        try:
            oversized.sendall(b"x" * (1024 * 1024 + 2))
        except BrokenPipeError:
            pass  # The engine can reject before the peer finishes the oversized frame.
        response = receive_json_line(oversized)
        check(error_code(response, "oversized input") == "FRAME_TOO_LARGE",
              f"oversized input was not bounded: {response!r}")
    finally:
        oversized.close()
    check(engine.poll() is None, "oversized request terminated the engine")


def command_for_pid(pid: int) -> list[str]:
    result = subprocess.run(
        ["ps", "-p", str(pid), "-o", "command="],
        text=True,
        capture_output=True,
        timeout=3,
        check=False,
    )
    check(result.returncode == 0 and result.stdout.strip(),
          f"could not inspect auto-started engine process {pid}: {result.stderr.strip()}")
    return shlex.split(result.stdout.strip())


def verify_engine_process(pid: int, socket_path: str) -> None:
    command = command_for_pid(pid)
    check(command and os.path.realpath(command[0]) == ENGINE,
          f"handshake PID {pid} does not run the requested engine: {command!r}")
    check("--socket" in command and command[command.index("--socket") + 1] == socket_path,
          f"handshake PID {pid} does not own the test socket: {command!r}")


def stop_handshaken_engine(pid: int, socket_path: str, instance_id: str) -> None:
    # Reconfirm the server identity and command before signaling a detached process.
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(3.0)
    response: dict[str, Any] | None = None
    try:
        connection.connect(socket_path)
        response = send_raw(
            connection,
            b'{"api_version":"1.1","request_id":"cleanup-handshake",'
            b'"operation":"runtime.handshake"}',
        )
    finally:
        connection.close()
    check(response is not None and response.get("status") == "success",
          f"cleanup handshake failed: {response!r}")
    data = data_of(response, "cleanup runtime.handshake")
    check(data.get("engine_instance_id") == instance_id,
          "engine instance changed before cleanup; refusing to signal an unverified process")
    check(str(data.get("pid")) == str(pid), "engine PID changed before cleanup; refusing to signal it")
    verify_engine_process(pid, socket_path)
    os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    raise CheckFailure(f"auto-started engine PID {pid} did not exit after SIGTERM")


def identify_engine(socket_path: str) -> tuple[int, str] | None:
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(1.0)
    try:
        connection.connect(socket_path)
        response = send_raw(
            connection,
            b'{"api_version":"1.1","request_id":"cleanup-probe",'
            b'"operation":"runtime.handshake"}',
        )
    except (OSError, CheckFailure):
        return None
    finally:
        connection.close()
    if response.get("status") != "success":
        return None
    data = data_of(response, "cleanup probe handshake")
    try:
        pid = int(string_field(data, "pid", "cleanup probe handshake.data"))
    except (ValueError, CheckFailure):
        return None
    instance_id = data.get("engine_instance_id")
    if not isinstance(instance_id, str) or not instance_id:
        return None
    return pid, instance_id


def auto_start_race_check(root: Path) -> None:
    socket_path = str(root / "autostart.sock")
    request = {
        "api_version": "1.1",
        "request_id": "autostart-" + str(uuid.uuid4()),
        "operation": "runtime.handshake",
        "parameters": {},
    }
    encoded_request = json.dumps(request, separators=(",", ":")) + "\n"
    clients: list[subprocess.Popen[str]] = []
    owner: tuple[int, str] | None = None
    try:
        for _ in range(2):
            clients.append(subprocess.Popen(
                [CLI, "--socket", socket_path, "--engine", ENGINE],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            ))
        # Supply both requests before waiting: both clients really race to start/connect.
        for client in clients:
            check(client.stdin is not None, "missing auto-start stdin")
            client.stdin.write(encoded_request)
            client.stdin.close()
            client.stdin = None
        results: list[dict[str, Any]] = []
        for client in clients:
            try:
                stdout, stderr = client.communicate(timeout=TIMEOUT)
            except subprocess.TimeoutExpired as exc:
                client.kill()
                client.wait(timeout=3)
                if client.stdout:
                    client.stdout.close()
                if client.stderr:
                    client.stderr.close()
                raise CheckFailure("CLI timed out while racing to auto-start one engine") from exc
            check(client.returncode == 0,
                  f"auto-start CLI exited {client.returncode}; stdout={stdout!r}; stderr={stderr!r}")
            try:
                response = json.loads(stdout)
            except json.JSONDecodeError as exc:
                raise CheckFailure(f"auto-start CLI stdout was not JSON: {stdout!r}") from exc
            require_mapping(response, "auto-start CLI response")
            check(response.get("status") == "success", f"auto-start handshake failed: {response!r}")
            results.append(response)
        first_data = data_of(results[0], "auto-start handshake 1")
        second_data = data_of(results[1], "auto-start handshake 2")
        instance_id = string_field(first_data, "engine_instance_id", "auto-start handshake 1.data")
        pid_text = string_field(first_data, "pid", "auto-start handshake 1.data")
        check(second_data.get("engine_instance_id") == instance_id,
              "simultaneous auto-start CLIs connected to different engine instances")
        check(str(second_data.get("pid")) == pid_text,
              "simultaneous auto-start CLIs connected to different engine PIDs")
        pid = int(pid_text)
        verify_engine_process(pid, socket_path)
        owner = (pid, instance_id)
    finally:
        for client in clients:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=3)
        if owner is None:
            owner = identify_engine(socket_path)
            if owner is not None:
                verify_engine_process(owner[0], socket_path)
        if owner is not None:
            stop_handshaken_engine(owner[0], socket_path, owner[1])


def ordinary_file_safety_check(root: Path, engine_stdout: Any, engine_stderr: Any) -> None:
    endpoint = root / "ordinary-file.sock"
    contents = b"user data that must not be deleted\n"
    endpoint.write_bytes(contents)
    proc = subprocess.run(
        [ENGINE, "--socket", str(endpoint)],
        stdin=subprocess.DEVNULL,
        stdout=engine_stdout,
        stderr=engine_stderr,
        timeout=5,
        check=False,
    )
    check(proc.returncode != 0, "engine must reject an endpoint path occupied by a regular file")
    check(endpoint.is_file(), "engine deleted or replaced the pre-existing regular file")
    check(endpoint.read_bytes() == contents, "engine modified the pre-existing regular file")


def response_correlation_check(root: Path, wrong_handshake: bool) -> None:
    endpoint = str(root / ("wrong-handshake.sock" if wrong_handshake else "wrong-response.sock"))
    errors: list[Exception] = []
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(endpoint)
    listener.listen(1)
    listener.settimeout(5)

    def serve() -> None:
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5)
                handshake = receive_json_line(connection)
                response = {"request_id": "wrong-id" if wrong_handshake else handshake["request_id"],
                            "status": "success", "data": {"api_version": "1.1"}}
                connection.sendall(json.dumps(response).encode() + b"\n")
                if not wrong_handshake:
                    receive_json_line(connection)
                    connection.sendall(b'{"request_id":"wrong-id","status":"success","data":{}}\n')
        except Exception as exc:
            errors.append(exc)

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        request = {"api_version": "1.1", "request_id": "expected-id", "operation": "capabilities.list", "parameters": {}}
        result = subprocess.run([CLI, "--socket", endpoint, "--no-start"], input=json.dumps(request),
                                text=True, capture_output=True, timeout=8)
        check(result.returncode == 3, f"CLI accepted a misattributed response: {result!r}")
    finally:
        listener.close()
        worker.join(timeout=6)
    check(not worker.is_alive() and not errors, f"fake endpoint did not finish cleanly: {errors!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, help="path to the local Qt engine executable")
    parser.add_argument("--cli", required=True, help="path to the JSON-line CLI executable")
    args = parser.parse_args()

    global ENGINE, CLI
    ENGINE = os.path.abspath(args.engine)
    CLI = os.path.abspath(args.cli)
    for label, path in (("engine", ENGINE), ("cli", CLI)):
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            parser.error(f"{label} executable is missing or not executable: {path}")

    try:
        short_tmp = "/private/tmp" if os.path.isdir("/private/tmp") else tempfile.gettempdir()
        with tempfile.TemporaryDirectory(prefix="qip-", dir=short_tmp) as directory:
            root = Path(directory)
            engine_log = open(root / "engine.log", "w+", encoding="utf-8")
            try:
                integration_scenario(str(root / "engine.sock"), engine_log, engine_log)
                single_instance_check(root, engine_log, engine_log)
                raw_endpoint = str(root / "raw-protocol.sock")
                raw_engine = start_engine(raw_endpoint, engine_log, engine_log)
                try:
                    wait_for_cli(raw_endpoint, raw_engine)
                    raw_protocol_checks(raw_endpoint, raw_engine)
                finally:
                    stop_process(raw_engine)
                auto_start_race_check(root)
                ordinary_file_safety_check(root, engine_log, engine_log)
                response_correlation_check(root, True)
                response_correlation_check(root, False)
            except Exception as exc:
                engine_log.flush()
                engine_log.seek(0)
                log_text = engine_log.read()
                raise CheckFailure(f"{exc}\nengine stderr log:\n{log_text or '(empty)'}") from exc
            finally:
                engine_log.close()
    except Exception as exc:  # print the first failing assertion with useful context
        print(f"FAIL: {exc}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        return 1

    print("PASS: QCAE local IPC integration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
