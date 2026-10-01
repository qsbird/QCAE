#!/usr/bin/env python3
"""Compare nine real workflow records at eight stages, preserving every model field."""
from __future__ import annotations

import argparse
from collections import Counter
import copy
import hashlib
import json
from pathlib import Path

from skeleton_workflow_ipc_tests import canonical, identities_for_m


STAGES = ("M_baseline", "selection_applied", "selection_undone", "repaired_M",
          "normal_open_M", "recovered_M", "published_M", "stale_result_input_M")
KINDS = Counter({"node": 11, "beam": 10, "geometry": 1, "mesh": 1, "material": 1,
                 "section": 1, "part": 1, "assembly": 1, "set": 2, "force": 1,
                 "constraint": 1, "load_case": 1, "analysis": 1})


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def differences(expected, actual, path="$", result=None):
    """Enumerate leaf differences; sequence order and all dictionary keys are significant."""
    result = [] if result is None else result
    if isinstance(expected, dict) and isinstance(actual, dict):
        for key in sorted(expected.keys() | actual.keys()):
            child = f"{path}.{key}"
            if key not in expected or key not in actual:
                result.append({"path": child, "expected_present": key in expected,
                               "actual_present": key in actual, "expected": expected.get(key),
                               "actual": actual.get(key)})
            else:
                differences(expected[key], actual[key], child, result)
    elif isinstance(expected, list) and isinstance(actual, list):
        if len(expected) != len(actual):
            result.append({"path": path + ".length", "expected": len(expected), "actual": len(actual)})
        for index in range(min(len(expected), len(actual))):
            differences(expected[index], actual[index], f"{path}[{index}]", result)
    elif expected != actual or (isinstance(expected, bool) != isinstance(actual, bool)):
        result.append({"path": path, "expected": expected, "actual": actual})
    return result


def bijection(rows):
    if Counter(row["kind"] for row in rows) != KINDS:
        raise ValueError("M must contain exactly the specified 33 records and kind counts")
    identities = [row["entity_id"] for row in rows]
    if len(set(identities)) != len(identities) or any(not isinstance(item, str) or not item for item in identities):
        raise ValueError("Original EntityIds are not unique nonempty strings")
    nodes = sorted((row for row in rows if row["kind"] == "node"), key=lambda row: row["position_mm"])
    if [row["position_mm"] for row in nodes] != [[100 * index, 0, 0] for index in range(11)]:
        raise ValueError("Baseline node coordinates do not match the fixed M")
    labels = [(row["kind"], row.get("name", "")) for row in rows if row["kind"] not in {"node", "beam"}]
    if len(set(labels)) != len(labels):
        raise ValueError("Organization identity labels are ambiguous; an explicit bijection is required")
    return identities_for_m(rows)


def normalized(rows, mapping):
    identities = [row["entity_id"] for row in rows]
    if len(set(identities)) != len(rows) or set(identities) != set(mapping):
        raise ValueError("Checkpoint changes the original EntityId population")
    return sorted((canonical(row, mapping) for row in rows), key=lambda row: row["entity_id"])


def stages_for(record):
    raw = record.get("raw_semantic_checkpoints", record.get("semantic_checkpoints", {}))
    # This is a stage-name alias only. No model key, field, value or reference is removed.
    result = {("repaired_M" if name == "check_repaired" else name): rows for name, rows in raw.items()}
    if set(result) != set(STAGES) or len(raw) != len(STAGES):
        raise ValueError(f"Expected exactly eight checkpoints; received {sorted(raw)}")
    return result


def self_test(rows):
    """Test comparer behavior with explicit synthetic mutations, never workflow evidence."""
    mapping = bijection(rows)
    baseline = normalized(rows, mapping)
    renamed = {row["entity_id"]: f"synthetic-id-{index}" for index, row in enumerate(reversed(rows))}
    changed = list(reversed(canonical(rows, renamed)))
    checks = [{"case": "complete-ID-bijection-and-storage-order", "passed":
               differences(baseline, normalized(changed, bijection(changed))) == []}]
    for name, kind, mutate in (
        ("organization-name", "part", lambda row: row.update(name="Different name")),
        ("ordered-beam-connection", "beam", lambda row: row["nodes"].reverse()),
        ("ordered-part-members", "part", lambda row: row["members"].reverse()),
        ("constraint-reference", "constraint", lambda row: row.update(nodes=["node:01"])),
        ("source-field-removal", "section", lambda row: row.pop("sources")),
        ("unit-field-addition", "section", lambda row: row.update(area_unit="cm2")),
        ("material-reference", "section", lambda row: row.update(material_id="missing-material")),
        ("boolean-vs-numeric", "mesh", lambda row: row.update(stale=0)),
    ):
        mutation = copy.deepcopy(baseline)
        row = next(row for row in mutation if row["kind"] == kind)
        mutate(row)
        found = differences(baseline, mutation)
        checks.append({"case": name, "passed": bool(found), "actual_differences": found})
    return {"kind": "synthetic-comparer-self-test", "actual_workflow_runs": 0,
            "passed": all(check["passed"] for check in checks), "checks": checks}


def compare(args):
    source_files, runs, errors = [], [], []
    cli = json.loads(args.cli_report.read_text())
    source_files.append(args.cli_report)
    for record in cli.get("runs_detail", []):
        runs.append((args.cli_report, "CLI", record))
    for mode in ("GUI", "MIX"):
        for path in sorted(args.desktop_evidence_dir.rglob(f"{mode}-M-run-*.json")):
            record = json.loads(path.read_text())
            runs.append((path, mode, record))
            source_files.append(path)
    mode_counts = Counter(mode for _, mode, _ in runs)
    if mode_counts != Counter({mode: args.expected_runs for mode in ("CLI", "GUI", "MIX")}):
        errors.append(f"Expected {args.expected_runs} real runs per mode; received {dict(mode_counts)}")
    run_ids = [record.get("run_id") for _, _, record in runs]
    if any(not item for item in run_ids) or len(set(run_ids)) != len(run_ids):
        errors.append("Every real workflow record must have a distinct nonempty run_id")
    canonical_runs = []
    for path, mode, record in runs:
        item = {"mode": mode, "run_id": record.get("run_id"), "input": str(path.resolve()),
                "identity_bijection": {}, "checkpoints": {}, "passed": False}
        try:
            if record.get("source_tree_sha256") != args.source_tree_sha256:
                raise ValueError("Workflow record is not bound to the requested source tree SHA256")
            if mode == "CLI" and record.get("passed") is not True:
                raise ValueError("CLI workflow did not pass its actual action assertions")
            if record.get("mode", "").upper() != mode:
                raise ValueError("Workflow mode does not match its evidence population")
            if mode == "CLI" and "raw_semantic_checkpoints" not in record:
                raise ValueError("CLI evidence must retain raw checkpoints before ID canonicalization")
            checkpoints = stages_for(record)
            mapping = bijection(checkpoints["M_baseline"])
            item["identity_bijection"] = mapping
            for stage in STAGES:
                item["checkpoints"][stage] = normalized(checkpoints[stage], mapping)
            if mode == "CLI":
                if record.get("identity_bijection") != mapping:
                    raise ValueError("Recorded CLI identity bijection differs from independently derived mapping")
                saved = record.get("semantic_checkpoints", {})
                if differences(item["checkpoints"], saved):
                    raise ValueError("CLI canonical checkpoints differ from retained raw records")
            item["passed"] = True
        except (KeyError, ValueError, TypeError) as error:
            item["error"] = str(error)
            errors.append(f"{mode}/{record.get('run_id')}: {error}")
        canonical_runs.append(item)
    reference = next((item for item in canonical_runs if item["mode"] == "CLI" and item["passed"]), None)
    comparisons = []
    if reference:
        for item in canonical_runs:
            if not item["passed"]:
                continue
            for stage in STAGES:
                found = differences(reference["checkpoints"][stage], item["checkpoints"][stage])
                comparisons.append({"reference_run_id": reference["run_id"], "run_id": item["run_id"],
                                    "mode": item["mode"], "stage": stage, "passed": not found,
                                    "difference_count": len(found), "differences": found})
    else:
        errors.append("No complete raw CLI reference workflow is available")
    difference_count = sum(item["difference_count"] for item in comparisons)
    report = {"source_tree_sha256": args.source_tree_sha256, "mode_counts": dict(mode_counts),
              "expected_runs_per_mode": args.expected_runs, "expected_checkpoints_per_run": 8,
              "comparisons": comparisons, "semantic_difference_count": difference_count,
              "errors": errors, "real_solver": False, "real_ai_client": False,
              "identity_bijections": [{key: item[key] for key in ("mode", "run_id", "input", "identity_bijection")}
                                      for item in canonical_runs],
              "input_files": [{"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                              for path in dict.fromkeys(source_files)]}
    report["passed"] = not errors and difference_count == 0
    frozen = len(args.source_tree_sha256) == 64 and all(
        char in "0123456789abcdef" for char in args.source_tree_sha256)
    report["acceptance_ready"] = report["passed"] and frozen and args.expected_runs == 3
    write(args.output, report)
    print(json.dumps({key: report[key] for key in ("passed", "acceptance_ready", "mode_counts", "semantic_difference_count", "errors")}, ensure_ascii=False))
    return 0 if report["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli-report", type=Path)
    parser.add_argument("--desktop-evidence-dir", type=Path)
    parser.add_argument("--source-tree-sha256", default="mutable-diagnostic")
    parser.add_argument("--expected-runs", default=3, type=int)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--self-test-m", type=Path,
                        help="Synthetic comparer unit checks using raw M; contributes zero workflow runs")
    args = parser.parse_args()
    if args.self_test_m:
        report = self_test(json.loads(args.self_test_m.read_text()))
        write(args.output, report)
        print(json.dumps(report))
        return 0 if report["passed"] else 1
    if not args.cli_report or not args.desktop_evidence_dir:
        parser.error("Actual comparison requires --cli-report and --desktop-evidence-dir")
    return compare(args)


if __name__ == "__main__":
    raise SystemExit(main())
