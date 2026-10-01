#!/usr/bin/env python3
"""Validate the actual 22×10 fault run records without duplicating test evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", required=True, type=Path)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--results", required=True, type=Path)
    parser.add_argument("--sanitizer-client", type=Path)
    parser.add_argument("--sanitizer-log", type=Path)
    parser.add_argument("--source-tree-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    paths = [args.application, args.client, args.results]
    runs = [item for path in paths for item in records(path)]
    errors = []
    identifiers = [item.get("run_id") for item in runs]
    if len(runs) != 220:
        errors.append(f"Expected 220 real run records; found {len(runs)}")
    if len(set(identifiers)) != len(identifiers):
        errors.append("Fault run IDs are not unique")
    if {item.get("source_tree_sha256") for item in runs} != {args.source_tree_sha256}:
        errors.append("Fault runs mix different source tree bindings")
    counts = {}
    for index in range(1, 23):
        case = f"F{index:02}"
        selected = [item for item in runs if item.get("case_id") == case]
        counts[case] = {"runs": len(selected), "passed": sum(item.get("passed") is True for item in selected)}
        if counts[case] != {"runs": 10, "passed": 10}:
            errors.append(case + " does not have ten successful actual runs")
    for item in runs:
        if not item.get("input") or not item.get("assertions") or not item.get("actual"):
            errors.append(str(item.get("run_id")) + " lacks input/assertions/actual evidence")
        if any(assertion.get("passed") is not True or "expected" not in assertion or "actual" not in assertion
               for assertion in item.get("assertions", [])):
            errors.append(str(item.get("run_id")) + " has a failed or incomplete assertion")
        if item.get("case_id") in {"F09", "F10"} and item.get("actual", {}).get("child_exit_code") != "86":
            errors.append(str(item.get("run_id")) + " lacks the deterministic real-process termination")
        if item.get("case_id") == "F05":
            actual = item.get("actual", {})
            if (actual.get("successful_write_response_read") is not False or actual.get("writing_connection_closed") is not True
                    or actual.get("retry", {}).get("replayed") is not True
                    or actual.get("retry", {}).get("transaction_id") != actual.get("original_transaction")):
                errors.append(str(item.get("run_id")) + " lacks actual IPC response loss and same original transaction replay")
        if item.get("case_id") == "F10":
            actual = item.get("actual", {})
            if not actual.get("intent_token") or actual.get("intent_token") != actual.get("project_token"):
                errors.append(str(item.get("run_id")) + " lacks matching publication/intent tokens")
        if item.get("case_id") == "F22":
            mutations = {value.get("mutation") for value in item.get("input", [])}
            if not {"wrong_input_fingerprint", "wrong_location", "wrong_number_mapping"}.issubset(mutations):
                errors.append(str(item.get("run_id")) + " did not inject all frozen R negative mutations")
    sanitizer = {"verified": False, "reason": "F17 sanitizer process evidence was not supplied"}
    if args.sanitizer_client and args.sanitizer_log:
        instrumented = [item for item in records(args.sanitizer_client) if item.get("case_id") == "F17"]
        text = args.sanitizer_log.read_text()
        clean = not any(marker in text for marker in ("ERROR: AddressSanitizer", "SUMMARY: AddressSanitizer",
                                                      "runtime error:", "UndefinedBehaviorSanitizer:"))
        matching = all(item.get("source_tree_sha256") == args.source_tree_sha256 for item in instrumented)
        complete = len(instrumented) == 10 and all(item.get("passed") is True and
                    item.get("actual", {}).get("asan_instrumented") is True and
                    item.get("actual", {}).get("ubsan_instrumented") is True for item in instrumented)
        sanitizer = {"verified": clean and matching and complete,
                     "runs": len(instrumented), "instrumented_and_passed": complete,
                     "matching_source": matching, "log_clean": clean,
                     "client_path": str(args.sanitizer_client), "client_sha256": sha(args.sanitizer_client),
                     "log_path": str(args.sanitizer_log), "log_sha256": sha(args.sanitizer_log)}
    if not sanitizer["verified"]:
        errors.append("F17 does not yet have ten passing, source-bound ASan+UBSan runs")
    diagnostic = args.source_tree_sha256 == "mutable-diagnostic"
    if diagnostic:
        errors.append("Source tree is mutable diagnostic; frozen product acceptance is not established")
    report = {"checkpoint": "SK-06 fault assertions", "source_tree_sha256": args.source_tree_sha256,
              "diagnostic": diagnostic, "passed": not errors, "runs": len(runs), "cases": counts,
              "sanitizer": sanitizer, "errors": errors,
              "raw_evidence": [{"path": str(path), "sha256": sha(path)} for path in paths]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"runs": len(runs), "cases_passing": sum(value["passed"] == 10 for value in counts.values()),
                      "fault_contract_passed": report["passed"], "errors": errors}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
