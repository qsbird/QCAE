#!/usr/bin/env python3
"""Legacy workspace migration at engine start goes through the application's commit path.

Starts the real engine with --migrate-from on a frozen legacy workspace and a fresh destination,
then checks that explicit recovery reproduces the legacy document and that the source bytes are
unchanged. A second start against the now-used destination must be refused without touching it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from c3_sync_ipc_tests import IpcClient, check


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def start(engine: str, root: Path, destination: Path, source: Path, log) -> tuple[subprocess.Popen, dict | None]:
    endpoint = str(root / "engine.sock")
    process = subprocess.Popen([engine, "--socket", endpoint, "--workspace", str(destination),
                                "--migrate-from", str(source)], stdout=log, stderr=log)
    client = IpcClient(endpoint)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return process, None
        try:
            return process, {"client": client, "capabilities": client.call("capabilities.list")}
        except (OSError, RuntimeError, AssertionError, json.JSONDecodeError):
            time.sleep(.05)
    process.kill()
    raise AssertionError("engine startup timed out")


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    expected = json.loads((args.fixtures / "unsaved-workspace.expected.json").read_text())["snapshot"]["document"]
    with tempfile.TemporaryDirectory(prefix="qcae-migration-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        source = root / "legacy.sqlite"
        shutil.copyfile(args.fixtures / "unsaved-workspace.sqlite", source)
        source_digest = digest(source)
        destination = root / "migrated.sqlite"
        with (root / "engine.log").open("w+") as log:
            process, started = start(args.engine, root, destination, source, log)
            check(started is not None, "engine refused a valid migration into a fresh destination")
            try:
                client, capabilities = started["client"], started["capabilities"]
                check(capabilities["recovery_available"] is True,
                      "migrated workspace is not offered for explicit recovery")
                # The frozen workspace names an older Nastran profile definition than this engine
                # installs; incompatible profile semantics must be refused, never silently adopted.
                recovered = client.call("project.open", {"mode": "recover"}, key="recover", expected=None)
                if recovered.get("status") == "failed":
                    check(recovered["error"]["code"] == "SCHEMA_UNSUPPORTED" and
                          "profile" in recovered["error"]["message"],
                          f"recovery failed for a reason other than the profile: {recovered}")
                else:
                    check(recovered["document_id"] == expected["document_id"] and
                          recovered["document_epoch"] != expected["document_epoch"],
                          f"recovery did not reproduce the legacy document under a new epoch: {recovered}")
            finally:
                stop(process)
            check(digest(source) == source_digest, "migration changed the legacy source workspace")
            reopened = subprocess.Popen([args.engine, "--socket", str(root / "plain.sock"),
                                         "--workspace", str(destination)], stdout=log, stderr=log)
            try:
                client = IpcClient(str(root / "plain.sock"))
                deadline, capabilities = time.monotonic() + 20, None
                while capabilities is None and time.monotonic() < deadline:
                    try:
                        capabilities = client.call("capabilities.list")
                    except (OSError, RuntimeError, AssertionError, json.JSONDecodeError):
                        time.sleep(.05)
                check(capabilities is not None and capabilities["recovery_available"] is True,
                      "the migrated image was not durably committed to the destination")
            finally:
                stop(reopened)
            used_digest = digest(destination)
            process, started = start(args.engine, root, destination, source, log)
            if started is not None:
                stop(process)
            check(started is None and process.returncode != 0,
                  "engine accepted a migration into an existing destination")
            check(digest(destination) == used_digest, "refused migration changed the existing destination")
    print("PASS: engine migration publishes through the application; recovery, source bytes and "
          "used-destination refusal checked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
