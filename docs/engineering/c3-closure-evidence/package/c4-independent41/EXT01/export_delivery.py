#!/usr/bin/env python3
"""Export the complete committed baseline diff and separate raw execution evidence."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[3]
evidence = Path(__file__).resolve().parent
output = Path("/private/tmp/qcae-c4-ext01-delivery41")
output.mkdir(parents=True, exist_ok=True)
baseline = "b740b374c934f3b4624a21247b9a5deeb2ca6bf4"
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
subprocess.run(["git", "diff", "--check", baseline, commit], cwd=root, check=True)
diff = subprocess.check_output(["git", "diff", "--binary", baseline, commit], cwd=root)
(output / "ext01-full.diff").write_bytes(diff)
shutil.copytree(evidence, output / "evidence", dirs_exist_ok=True)
shutil.copy2(root / "tests/fixtures/ext01-material-v1.record", output / "ext01-material-v1.record")
audit = json.loads((evidence / "protection.json").read_text())
caches = {}
for name in ("build-ext01-core", "build-ext01-local", "build-ext01-consumers"):
    selected = [line for line in (root / name / "CMakeCache.txt").read_text().splitlines()
                if line.startswith(("Python3_EXECUTABLE:", "_Python3_EXECUTABLE:"))]
    assert selected and all(line.endswith("/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14")
                            for line in selected), selected
    caches[name] = selected
manifest = {
    "task": "EXT-01", "executor": "/root/c4_ext01_frozen41", "baseline_commit": baseline,
    "extension_commit": commit, "branch": "codex/c4-ext01-baseline41", "working_directory": str(root),
    "diff_path": str(output / "ext01-full.diff"), "diff_bytes": len(diff),
    "diff_sha256": hashlib.sha256(diff).hexdigest(),
    "handwritten_product_file_count": 6, "handwritten_product_files": audit["handwritten_product_files"],
    "protected_file_count": 39, "protected_touch_count": 0, "generators": audit["generators"],
    "fixture_bytes": 109, "fixture_sha256": "ae21094749106dfe32cdce3ce20e0a0a728c14de2894524c820f01cb341422ed",
    "frozen_python_selection": caches,
    "validation": {
        "baseline_relevant": {"passed": 2, "total": 2, "log": "baseline-relevant-tests.log"},
        "frozen_core_sqlite_headless": {"passed": 45, "total": 45, "log": "frozen-core-tests.log"},
        "frozen_affected_tool_ipc": {"passed": 20, "total": 20, "log": "frozen-local-affected-tests.log"},
        "native_qt_vtk": {"passed": 7, "total": 7, "log": "native-desktop-tests.log", "gui_slot_released": True},
        "independent_public_headers": {"count": 48, "log": "frozen-consumers-build.log"},
        "production_ext01_ipc": {"request_count": 211, "log": "ext01-production-ipc.log", "transcript": "ipc-transcript.json"},
        "design": "delivery-design-check.log", "cpp_format_21": "delivery-format-check.log", "diff_check": "delivery-diff-check.log",
        "qg02": "root read-only review passed; exact message retained in additional-context.txt"
    },
    "raw_evidence_directory": str(output / "evidence"),
    "complete_context_cost_verified": False, "complete_actual_context_utf8_bytes": None, "true_token_count": None,
    "initial_context_content_utf8_bytes": 42078, "second_initial_complete_delivery_utf8_bytes": 42601,
    "failure_and_context_details": ["failures-and-repairs.json", "context-corrections.json", "context-accounting-limitations.json"],
    "final_release_integration_run": False,
    "note": "Product behavior and QG checks pass; early actual context delivery is incomplete evidence. Original ledger/failed logs/fixture bytes preserved. Common frozen manifest not modified."
}
(output / "delivery-manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
print(json.dumps({"extension_commit": commit, "diff_bytes": len(diff), "diff_sha256": manifest["diff_sha256"],
                  "delivery_directory": str(output)}, indent=2))
