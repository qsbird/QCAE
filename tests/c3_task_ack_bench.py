#!/usr/bin/env python3
"""Collect the frozen SK-12 fifty task ACKs, without designated warmup runs."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from c3_sync_ipc_tests import EngineProcess, check, task_ack_workload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--environment-manifest", required=True, type=Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    check(source == args.source_commit, "Task collector source commit differs from this checkout")
    paths = ["CMakeLists.txt", "modules", "features", "adapters", "apps", "ui", "profiles", "schemas", "tests", "tools"]
    check(subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *paths], cwd=root).returncode == 0,
          "Task collector cannot bind modified source to an old commit")
    untracked = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "--", *paths],
                                        cwd=root, text=True)
    check(not untracked.strip(), "Task collector cannot bind untracked product/test source to a commit")
    environment_bytes = args.environment_manifest.read_bytes()
    environment = json.loads(environment_bytes)
    check(environment.get("build_type") == "Release" and environment.get("frozen") is True,
          "Task collector requires the actual frozen Release environment")
    engine = args.engine.resolve(strict=True)
    cache = engine.parent / "CMakeCache.txt"
    check(cache.is_file() and "CMAKE_BUILD_TYPE:STRING=Release" in cache.read_text(),
          "Task collector engine must come from the actual Release build")
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    report = {"source_commit": source,
              "engine_path": str(engine), "engine_sha256": hashlib.sha256(engine.read_bytes()).hexdigest(),
              "environment_manifest_sha256": hashlib.sha256(environment_bytes).hexdigest(),
              "warmup_count": 0, "samples_ms": [], "passed": False,
              "measurement": "wall time from actual client call through real socket handshake, request and queued/running response"}
    error = None
    with tempfile.TemporaryDirectory(prefix="qc3-task-", dir=Path("/tmp").resolve()) as folder:
        temp = Path(folder)
        with (args.evidence_dir / "engine.log").open("w+") as log:
            host = EngineProcess(str(engine), str(temp / "engine.sock"), str(temp / "work.sqlite"), log)
            try:
                capabilities = host.start()
                check(capabilities.get("durable") is True and capabilities.get("storage_mode") == "sqlite",
                      "Task ACK sampling requires an explicit real SQLite workspace")
                document = host.client.call("project.create", {"name": "Task ACK fixture"}, key="create")
                geometry = host.client.call("geometry.create_line", {
                    "start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]}, document, "line")
                before = host.client.current()
                history = host.client.call("history.list", context=before)
                measured = task_ack_workload(host.client, before, geometry["entity_id"], warmups=0)
                check(host.client.current() == before and host.client.call("history.list", context=before) == history,
                      "Cancelled task ACK fixture changed model revision or history")
                report.update(samples_ms=measured["start_ack_ms"], p95_ms=measured["start_ack_p95_ms"],
                              cancel_samples_ms=measured["cancel_ack_ms"], cancel_p95_ms=measured["cancel_ack_p95_ms"],
                              actual_task_records=measured["records"], passed=measured["passed"],
                              fixture_before=before, fixture_after=host.client.current())
            except Exception as caught:
                error = caught
                report["error"] = str(caught)
            finally:
                host.close()
                (args.evidence_dir / "transcript.json").write_text(json.dumps(host.client.transcript, indent=2) + "\n")
                (args.evidence_dir / "task-ack.json").write_text(json.dumps(report, indent=2) + "\n")
    if error:
        raise error
    print(json.dumps({"samples": len(report["samples_ms"]), "warmup_count": 0,
                      "start_p95_ms": report["p95_ms"], "cancel_p95_ms": report["cancel_p95_ms"],
                      "passed": report["passed"]}))
    return 0 if report["passed"] and len(report["samples_ms"]) == 50 else 1


if __name__ == "__main__":
    raise SystemExit(main())
