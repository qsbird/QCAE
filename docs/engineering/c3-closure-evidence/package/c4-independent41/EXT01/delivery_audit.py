#!/usr/bin/env python3
"""Audit EXT-01 constraints; preserve incomplete context evidence explicitly."""
import collections
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
evidence = Path(__file__).resolve().parent
manifest = json.loads((root / "docs/engineering/c3-closure-evidence/package/c4-common40-baseline-manifest.json").read_text())
baseline = "b740b374c934f3b4624a21247b9a5deeb2ca6bf4"
assert manifest["baseline_commit"] == baseline


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(name, value):
    (evidence / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


changed = subprocess.check_output(["git", "diff", "--name-only", baseline], cwd=root, text=True).splitlines()
product = [path for path in changed if not path.startswith(("tests/", "docs/"))
           and path != "modules/document/include/qcae/records.hpp"]
assert len(product) == 6, product
protected = []
for expected in manifest["protected_files"]:
    path = root / expected["path"]
    if not path.is_file() or sha(path) != expected["sha256"] or expected["path"] in changed:
        protected.append(expected["path"])
assert not protected, protected
generators = []
for expected in manifest["generators"]:
    digest = sha(root / expected["path"])
    assert digest == expected["sha256"]
    generators.append({"path": expected["path"], "sha256": digest, "unchanged": True})
fixture = root / "tests/fixtures/ext01-material-v1.record"
assert len(fixture.read_bytes()) == 109
assert sha(fixture) == "ae21094749106dfe32cdce3ce20e0a0a728c14de2894524c820f01cb341422ed"
write("protection.json", {"baseline_commit": baseline, "protected_file_count": len(manifest["protected_files"]),
                           "protected_touch_count": 0, "handwritten_product_file_count": len(product),
                           "handwritten_product_files": product, "generators": generators,
                           "changed_tracked_paths": changed})

contexts = [json.loads(line) for line in (evidence / "context.jsonl").read_text().splitlines()]
corrected_contexts = []
for index, entry in enumerate(contexts):
    complete_second_initial = 8 <= index < 16
    corrected_contexts.append({"entry_index": index, "path": entry["path"], "phase": entry["phase"],
                              "source_utf8_bytes": entry["utf8_bytes"], "source_sha256": entry["sha256"],
                              "actual_delivered_utf8_bytes": entry["delivered_utf8_bytes"] if complete_second_initial else None,
                              "actual_complete_delivery": True if complete_second_initial else None,
                              "read_count": 1, "tokens": None,
                              "reason": "second initial packet completely delivered" if complete_second_initial
                              else "helper logged requested source; actual outer-tool ranges not completely retained"})
commands = [json.loads(line) for line in (evidence / "commands.jsonl").read_text().splitlines()]
corrected_commands = []
failures = []
for index, entry in enumerate(commands):
    path = evidence / (entry["name"] + ".log")
    modern = "disk_utf8_bytes" in entry
    corrected_commands.append({"entry_index": index, "name": entry["name"], "kind": entry["kind"],
                               "command": entry["command"], "exit_code": entry["exit_code"],
                               "disk_utf8_bytes": len(path.read_bytes()), "disk_sha256": sha(path),
                               "actual_delivered_utf8_bytes": entry["delivered_utf8_bytes"] if modern else None,
                               "actual_complete_delivery": entry["delivered_complete_output"] if modern else None,
                               "delivery_range": entry.get("delivery_range") if modern else "unknown legacy tool range",
                               "read_count": 1, "tokens": None})
    if entry["exit_code"]:
        failures.append({"name": entry["name"], "exit_code": entry["exit_code"], "log": str(path.relative_to(root))})
write("context-corrections.json", {"supersedes_delivery_claims_in": ["context.jsonl", "commands.jsonl"],
                                   "contexts": corrected_contexts, "commands": corrected_commands})
write("context-accounting-limitations.json", {
    "executor": "/root/c4_ext01_frozen41", "working_directory": str(root), "baseline_commit": baseline,
    "initial_file_count": 8, "initial_content_utf8_bytes": 42078, "initial_complete_packet_utf8_bytes": 42601,
    "initial_packet_attempts": 2, "second_packet_fully_delivered": True,
    "additional_task_and_five_parent_messages_utf8_bytes": len((evidence / "additional-context.txt").read_bytes()),
    "message_note": "Exact text retained; blank separators are evidence packaging. Messages count as additional context.",
    "unresolved_reads": [
        "First locating command read task plus baseline README and architecture listing before instrumented delivery; task was read again in both initial packets.",
        "First initial packet and architecture packet had outer-tool truncation; exact returned range/bytes unknown.",
        "Early direct c4_context_read.py/manifest inspection had truncation; exact bytes unknown.",
        "Legacy runner entries mislabel disk output as full delivery; correction entries conservatively set actual bytes/ranges null.",
        "Uninstrumented initial shell failures/searches and resume status, runner, product diff, task reads are additional cost with unknown exact return totals.",
        "Failed nonexistent-file packet returned traceback without source; earliest traceback byte accounting not retained.",
        "Session-provided instructions and compaction context have not been included in a source-read total."
    ],
    "complete_actual_context_utf8_bytes": None, "true_token_count": None,
    "complete_context_cost_verified": False, "disk_logs_are_not_complete_context_delivery": True,
    "correction_policy": "Retain original records; corrected delivery fields override their completion/byte claims. No aggregate disk length is reported as actual context."
})
read_counts = collections.Counter(entry["path"] for entry in contexts)
read_counts["docs/engineering/c4-executor-task.md"] += 2
write("source-read-counts.json", {"observed_counts_are_lower_bounds": True, "path_read_counts": dict(sorted(read_counts.items())),
                                 "extra_count_note": "Task locating read and resume task read added; searches and command-driven source ranges remain in command ledger; unknown early reads are disclosed."})
write("failures-and-repairs.json", {
    "recorded_failed_command_count": len(failures), "recorded_failed_commands": failures,
    "complete_failure_count": None,
    "implementation_repair_count": 4,
    "implementation_repairs": [
        "Inspect committed transaction fields instead of diff_record_views field list, which does not populate field IDs.",
        "Align optional description with frozen nonempty string contract; omission clears, empty string fails.",
        "Remove attempted getter returning null because frozen core Value has no null variant; descriptor-driven entity.query supplies null through Qt adapter.",
        "Rebuild generated operation contracts after removing getter before rerunning generation check."
    ],
    "environment_repairs": ["Retry freeze contract on authorized host after sandbox ps denial; preserve 90-second timeout.",
                            "Select frozen Library Python 3.14.0 in all three CMake configurations; rerun affected tools without threshold changes."],
    "other_failures": "Searches used missing paths, a transient native progress log was missing, and an evidence summary script was repaired for legacy log field names. Uninstrumented early failures are not counted as an exact total."
})
print(json.dumps({"product_files": len(product), "protected_touch_count": 0, "unchanged_generators": len(generators),
                  "recorded_failed_commands": len(failures), "context_cost_verified": False}, indent=2))
