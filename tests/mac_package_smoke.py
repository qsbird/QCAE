#!/usr/bin/env python3
"""Run the staged macOS desktop against its own engine and imported beam fixture."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import ipc_tests as ipc


def run(bundle, evidence):
    bundle, evidence = Path(bundle).resolve(), Path(evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    binaries = bundle / "Contents/MacOS"
    ipc.ENGINE, ipc.CLI = str(binaries / "qcae-engine"), str(binaries / "qcae-cli")
    screenshot = evidence / "desktop.png"
    with tempfile.TemporaryDirectory(prefix="qcae-package-", dir="/private/tmp") as temporary:
        endpoint = str(Path(temporary) / "engine.sock")
        with (evidence / "engine.log").open("w") as engine_log, (evidence / "desktop.log").open("w") as gui_log:
            engine = subprocess.Popen([ipc.ENGINE, "--socket", endpoint, "--workspace", str(Path(temporary) / "working.sqlite")],
                                      stdout=engine_log, stderr=engine_log)
            try:
                capabilities = ipc.data_of(ipc.wait_for_cli(endpoint, engine), "capabilities.list")
                assert capabilities["durable"]
                document = ipc.data_of(ipc.call(endpoint, "project.create", {"name": "Packaged beam"},
                                                idempotency_key="package-create"), "project.create")
                context = {"document_id": document["document_id"], "document_epoch": document["document_epoch"],
                           "expected_revision": document["revision"]}
                fixture = Path(__file__).parent / "fixtures/nastran"
                resources = [{"path": str(path.relative_to(fixture)), "text": path.read_text()}
                             for path in sorted(fixture.rglob("*.bdf"))]
                preview = ipc.data_of(ipc.call(endpoint, "changes.preview",
                                              {"command": "model.import", "root_resource": "cantilever.bdf", "resources": resources,
                                               "source_profile_ref": capabilities["declared_solver_profiles"][0]["profile_ref"],
                                               "unit_system": "mm-N-MPa"}, **context), "changes.preview")
                ipc.data_of(ipc.call(endpoint, "changes.commit", {"preview_id": preview["preview_id"]},
                                    idempotency_key="package-import", **context), "changes.commit")
                desktop = subprocess.run([str(binaries / "qcae-desktop"), "--socket", endpoint,
                                          "--workspace", str(Path(temporary) / "working.sqlite"),
                                          "--engine", ipc.ENGINE, "--smoke", "--screenshot", str(screenshot),
                                          "--quit-after-ms", "6000"], stdout=gui_log, stderr=gui_log, timeout=30)
                assert desktop.returncode == 0 and screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
                assert engine.poll() is None, "Closing the GUI terminated the shared engine"
                current = ipc.data_of(ipc.call(endpoint, "project.current", {}), "project.current")
                summary = ipc.data_of(ipc.call(endpoint, "model.summary", {}, document_id=current["document_id"],
                                              document_epoch=current["document_epoch"]), "model.summary")
                assert summary["node_count"] == 2 and summary["beam_count"] == 1
                manifest = {"status": "passed", "bundle": str(bundle), "complete_p0_release": False,
                            "source_binding": "staging_manifest_binaries_not_final_frozen_source",
                            "desktop_exit_code": desktop.returncode, "engine_remained_alive": True,
                            "nodes": summary["node_count"], "beams": summary["beam_count"],
                            "screenshot_sha256": hashlib.sha256(screenshot.read_bytes()).hexdigest(),
                            "binaries": {name: hashlib.sha256((binaries / name).read_bytes()).hexdigest()
                                         for name in ("qcae-desktop", "qcae-engine", "qcae-cli", "qcae-mcp")}}
                (evidence / "facts.json").write_text(json.dumps(manifest, indent=2) + "\n")
            finally:
                if engine.poll() is None:
                    engine.terminate()
                engine.wait(timeout=10)
    print("PASS: packaged native Qt/VTK desktop, real beam packet, own SQLite engine and surviving CLI state")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--evidence-dir", required=True)
    args = parser.parse_args()
    run(args.bundle, args.evidence_dir)
