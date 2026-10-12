#!/usr/bin/env python3
"""SK-04: every operation the engine advertises as available reaches a real handler.

Each available operation is called once, with empty parameters, against a fresh engine
that has one open project. Any answer other than the dispatcher's refusal for an
operation without a handler (a decode error, needs_input, a conflict, or success)
shows that a handler received the request. One engine per operation keeps state
changes such as project.close from affecting the next call.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile

from c3_sync_ipc_tests import EngineProcess, check

NO_HANDLER_MARKERS = ("not implemented", "no handler", "handler is not installed")


def probe(engine: str, operation: dict, root: Path) -> dict:
    with (root / "engine.log").open("w+") as log:
        host = EngineProcess(engine, str(root / "engine.sock"), str(root / "workspace.sqlite"), log)
        try:
            host.start()
            document = host.client.call("project.create", {"name": "Handler probe"}, key="create")
            context = document if operation.get("requires_document") else None
            key = "probe" if operation.get("requires_idempotency_key") else None
            host.client.call(operation["name"], {}, context, key, expected=None)
            response = host.client.transcript[-1]["response"]
        finally:
            host.close()
    error = response.get("error") or {}
    message = str(error.get("message", "")).lower()
    missing = error.get("code") == "UNSUPPORTED_CAPABILITY" and any(m in message for m in NO_HANDLER_MARKERS)
    return {"operation": operation["name"], "status": response.get("status"), "error_code": error.get("code"),
            "error_message": error.get("message"), "handler_reached": not missing}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="qcae-handlers-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        with (root / "catalog.log").open("w+") as log:
            host = EngineProcess(args.engine, str(root / "catalog.sock"), str(root / "catalog.sqlite"), log)
            try:
                capabilities = host.start()
            finally:
                host.close()
        available = [item for item in capabilities["operations"] if item.get("available")]
        check(bool(available), "engine advertised no available operations")
        results = []
        for index, operation in enumerate(available):
            run = root / f"op-{index}"
            run.mkdir()
            results.append(probe(args.engine, operation, run))
    missing = [r["operation"] for r in results if not r["handler_reached"]]
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps({"available": len(results), "missing_handlers": missing,
                                         "results": results}, indent=2) + "\n")
    print(f"handler probe: available={len(results)} missing_handlers={len(missing)}")
    if missing:
        print("missing: " + ", ".join(missing))
        return 1
    print("PASS: every advertised available operation reached a handler")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
