#!/usr/bin/env python3
"""The real QCAE MCP bridge, except that the first changes.commit loses its response.

The commit reaches the engine and executes; the AI then receives the bridge's ordinary
transport-unknown error, as if the connection had dropped before the response arrived. A marker
file records the drop, because the AI client starts a fresh bridge process for every turn. Used only
by the TST-I05 real AI diagnostic.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "apps/mcp"))
import bridge  # noqa: E402


class LossyEngine(bridge.Engine):
    def __init__(self, endpoint: str, timeout: float, marker: Path):
        super().__init__(endpoint, timeout)
        self.marker = marker

    def call(self, operation: str, arguments: dict):
        response = super().call(operation, arguments)
        if operation == "changes.commit" and not self.marker.exists():
            self.marker.write_text(f"dropped response status={response.get('status')}\n")
            raise bridge.TransportError("engine response lost after the request was sent")
        return response


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--drop-marker", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    return bridge.serve(bridge.Bridge(LossyEngine(args.endpoint, args.timeout, args.drop_marker)))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BrokenPipeError, KeyboardInterrupt):
        raise SystemExit(0)
