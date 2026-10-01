#!/usr/bin/env python3
"""Real test-engine socket checks for borrowed parsing and generated/aborted frames."""
from __future__ import annotations

import argparse
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def wire(value: dict) -> bytes:
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode() + b"\n"


class Client:
    def __init__(self, endpoint: str):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(20)
        self.socket.connect(endpoint)
        self.buffer = bytearray()
        self.sequence = 0
        self.call("runtime.handshake")

    def close(self) -> None:
        self.socket.close()

    def request(self, operation: str, parameters: dict | None = None,
                context: dict | None = None) -> dict:
        self.sequence += 1
        value = {"api_version": "1.1", "request_id": f"frame-probe-{self.sequence}",
                 "operation": operation, "parameters": parameters or {}}
        if context:
            value.update(document_id=context["document_id"],
                         document_epoch=context["document_epoch"],
                         expected_revision=context["revision"])
        return value

    def line(self) -> tuple[dict, int]:
        while b"\n" not in self.buffer:
            received = self.socket.recv(65536)
            check(bool(received), "engine closed without a complete response")
            self.buffer.extend(received)
        end = self.buffer.index(b"\n") + 1
        response = bytes(self.buffer[:end])
        del self.buffer[:end]
        return json.loads(response), len(response)

    def call(self, operation: str, parameters: dict | None = None,
             context: dict | None = None, key: str | None = None) -> dict:
        request = self.request(operation, parameters, context)
        if key:
            request["idempotency_key"] = key
        self.socket.sendall(wire(request))
        response, _ = self.line()
        check(response.get("request_id") == request["request_id"], "reply ID changed")
        check(response.get("status") == "success", f"{operation} failed: {response}")
        return response["data"]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="qcae-frame-ledger-", dir="/tmp") as temporary:
        root = Path(temporary)
        endpoint = str(root / "engine.sock")
        with (root / "engine.log").open("wb") as log:
            process = subprocess.Popen([str(args.engine.resolve()), "--socket", endpoint,
                                        "--workspace", str(root / "workspace.sqlite")],
                                       stdout=log, stderr=log)
            first = second = None
            try:
                deadline = time.monotonic() + 20
                while not Path(endpoint).exists():
                    check(process.poll() is None, "engine failed to start")
                    check(time.monotonic() < deadline, "engine endpoint timed out")
                    time.sleep(0.02)
                first = Client(endpoint)
                context = first.call("project.create", {"name": "Transport frame probe"},
                                     key="frame-probe-create")
                # The engine preserves its object-only error body and accepts the
                # following ordinary request after every malformed/scalar frame.
                for malformed in (b"null\n", b"[]\n", b"1\n", b"{\"x\":1}{}\n",
                                  b"{\"x\":1,}\n"):
                    first.socket.sendall(malformed)
                    response, _ = first.line()
                    check(response == {"request_id": "", "status": "failed",
                                       "error": {"code": "INVALID_JSON",
                                                 "message": "Expected one JSON object per line"}},
                          f"object-only error wire changed: {response}")
                check(first.call("project.current")["revision"] == context["revision"],
                      "malformed input changed the document")
                first.call("test.ledger.begin",
                           {"run_id": "oversize-frame", "operation": "test.ledger.frame_probe"},
                           context)
                request = first.request("test.ledger.frame_probe", {"kind": "oversize"}, context)
                first.socket.sendall(wire(request))
                response, sent_bytes = first.line()
                check(response == {"request_id": request["request_id"], "status": "failed",
                                   "error": {"code": "RESOURCE_LIMIT",
                                             "message": "Response exceeds the M0 frame limit"}},
                      "oversize fallback changed error fields")
                snapshot = first.call("test.ledger.end", {}, context)
                metrics = snapshot["stages"]["socket_send"]["metrics"]
                original = wire({"request_id": request["request_id"], "status": "success",
                                 "data": {"payload": "x" * (2 * 1024 * 1024)}})
                check(int(metrics["encoded_bytes"]) == len(original) + sent_bytes,
                      "first large encoding or actual fallback encoding was omitted")
                check(int(metrics["socket_bytes"]) == sent_bytes,
                      "an unsent oversize frame was counted as socket traffic")
                sent = [frame for frame in snapshot["frames"] if frame["stage"] == "socket_send"]
                check(len(sent) == 1 and sent[0]["kind"] == "response" and
                      sent[0]["request_id"] == request["request_id"] and
                      int(sent[0]["bytes"]) == sent_bytes,
                      "trace did not identify the actual bounded failure frame")

                first.call("test.ledger.begin",
                           {"run_id": "queued-abort", "operation": "test.ledger.frame_probe"},
                           context)
                requests = [first.request("test.ledger.frame_probe", {"kind": "large"}, context)
                            for _ in range(6)]
                # One readyRead batch produces enough bounded replies to exceed
                # the unchanged queued-byte limit before the event loop flushes.
                first.socket.sendall(b"".join(wire(value) for value in requests))
                while first.socket.recv(65536):
                    pass
                first.close()
                first = None
                second = Client(endpoint)
                snapshot = second.call("test.ledger.end", {}, context)
                metrics = snapshot["stages"]["socket_send"]["metrics"]
                generated = sum(len(wire({"request_id": request["request_id"],
                                          "status": "success",
                                          "data": {"payload": "x" * (512 * 1024)}}))
                                for request in requests)
                check(int(metrics["encoded_bytes"]) >= generated and
                      int(metrics["encoded_bytes"]) > int(metrics["socket_bytes"]),
                      "queued abort swallowed already generated response encodings")
                check(second.call("project.current")["revision"] == context["revision"],
                      "read-only frame probes changed document history")
                print("PASS: actual object-only errors, oversize encoding/fallback/trace and "
                      "queued-abort generated bytes; not a C3 model acceptance sample")
            finally:
                if first:
                    first.close()
                if second:
                    second.close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
