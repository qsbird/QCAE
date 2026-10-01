#!/usr/bin/env python3
"""One actual thin-script write through the shared engine's public IPC contract."""
from __future__ import annotations

import json
from pathlib import Path
import socket
import sys


def main() -> int:
    endpoint, request_path = sys.argv[1:3]
    request = json.loads(Path(request_path).read_text())
    transcript = []
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5)
        connection.connect(endpoint)
        reader = connection.makefile("rb")

        def exchange(frame):
            connection.sendall(json.dumps(frame, separators=(",", ":")).encode() + b"\n")
            raw = reader.readline(1024 * 1024 + 1)
            if len(raw) > 1024 * 1024 or not raw.endswith(b"\n"):
                raise RuntimeError("Incomplete or oversized engine response")
            response = json.loads(raw)
            if response.get("request_id") != frame["request_id"]:
                raise RuntimeError("Response identity mismatch")
            transcript.append({"request": frame, "response": response})
            return response

        handshake = exchange({"api_version": "1.1", "request_id": "thin-handshake",
                              "operation": "runtime.handshake"})
        if handshake.get("status") != "success":
            raise RuntimeError("Engine handshake failed")
        reply = exchange(request)
    print(json.dumps({"entry": "thin_script", "response": reply, "transcript": transcript}))
    return 0 if reply.get("status") == "success" else 2


if __name__ == "__main__":
    raise SystemExit(main())
