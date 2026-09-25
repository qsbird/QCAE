#!/usr/bin/env python3
"""Check the versioned design baseline without product or third-party dependencies."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit


class DesignError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise DesignError(message)


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise DesignError(f"{path}: {exc}") from exc


def check_links(root: Path) -> int:
    canonical = [root / name for name in ("README.md", "CONTRIBUTING.md", "AGENTS.md")]
    for folder in ("docs/baseline", "docs/architecture", "docs/contracts"):
        canonical.extend(sorted((root / folder).glob("*.md")))
    canonical.append(root / "docs/README.md")
    require((root / "docs/baseline/requirements.md").is_file(), "Missing requirements baseline")
    require((root / "docs/baseline/acceptance.md").is_file(), "Missing acceptance baseline")
    count = 0
    for path in canonical:
        require(path.is_file(), f"Missing document: {path}")
        content = path.read_text(encoding="utf-8")
        require(content.count("```") % 2 == 0, f"Unbalanced code fences: {path}")
        require(all(line == line.rstrip() for line in content.splitlines()),
                f"Trailing whitespace: {path}")
        for raw in re.findall(r"\]\(([^)]+)\)", content):
            target = raw.strip("<>")
            parsed = urlsplit(target)
            if parsed.scheme or target.startswith("#"):
                continue
            local = unquote(parsed.path)
            require(not Path(local).is_absolute(), f"Non-portable baseline link: {path}: {target}")
            resolved = (path.parent / local).resolve()
            require(resolved.is_relative_to(root), f"Link escapes repository: {path}: {target}")
            require(resolved.is_file(), f"Broken baseline link: {path}: {target}")
            count += 1
    return count


def check_dependencies(root: Path) -> int:
    graph = load_json(root / "docs/architecture/module-dependencies.json")
    rows = graph["modules"]
    modules = {row["name"]: row for row in rows}
    require(len(rows) == len(modules), "Duplicate module names")
    active: set[str] = set()
    visited: set[str] = set()
    closures: dict[str, set[str]] = {}

    def visit(name: str) -> set[str]:
        require(name in modules, f"Unknown module: {name}")
        require(name not in active, f"Dependency cycle at {name}")
        if name in visited:
            return closures[name]
        active.add(name)
        closure = {name}
        for dep in modules[name]["dependencies"]:
            closure.update(visit(dep))
        active.remove(name)
        visited.add(name)
        closures[name] = closure
        return closure

    for name in modules:
        visit(name)
    for name, row in modules.items():
        if row["layer"] == "core":
            require(all(modules[dep]["layer"] == "core"
                        and not modules[dep]["external_dependencies"]
                        for dep in closures[name]), f"Core framework leakage: {name}")
    for name in ("desktop", "mcp", "cli", "snapshot_host"):
        require(not {"domain", "application", "render_data"} & closures[name],
                f"Client links core implementation: {name}")
    engine_external = {dep for name in closures["engine_host"]
                       for dep in modules[name]["external_dependencies"]}
    require(not {"VTK", "QtWidgets"} & engine_external, "Engine depends on graphics")
    require("solver_profiles" in modules and "profile_nastran" in modules,
            "Missing profile registry/provider boundary")
    require(not {"profile_nastran", "nastran_codec"} & closures["solver_profiles"],
            "Generic profile registry depends on a concrete solver")
    require("profile_nastran" in closures["engine_host"], "Host does not assemble the P0 profile")
    return len(modules)


def check_contracts(root: Path) -> tuple[int, int]:
    registry = load_json(root / "docs/contracts/operations.json")
    rows = registry["operations"]
    operations = {row["name"]: row for row in rows}
    require(len(rows) == len(operations) == registry["operation_count"],
            "Operation count mismatch or duplicate name")
    for name, row in operations.items():
        for key in ("requires_document", "requires_epoch", "requires_revision", "requires_idempotency_key"):
            require(type(row[key]) is bool, f"Invalid operation flag: {name}.{key}")
        if row["effect"] == "model_write":
            require(all(row[key] for key in ("requires_document", "requires_epoch",
                                            "requires_revision", "requires_idempotency_key")),
                    f"Model write lacks consistency fields: {name}")
        require(row["target_context"] in {"none", "optional_profile", "source_profile", "analysis",
                                          "conditional", "from_preview", "from_run", "from_history"},
                f"Invalid target context: {name}")
        require(type(row["requires_profile_match"]) is bool, f"Invalid profile flag: {name}")

    samples = load_json(root / "docs/contracts/current-examples.json")["examples"]
    examples = {row["name"]: row for row in samples}
    require(len(samples) == len(examples), "Duplicate example names")
    required_fields = {"requires_document": "document_id", "requires_epoch": "document_epoch",
                       "requires_revision": "expected_revision", "requires_idempotency_key": "idempotency_key"}
    for row in samples:
        request, response = row["request"], row["response"]
        operation = request["operation"]
        require(operation in operations, f"Example references unknown operation: {operation}")
        require(request["api_version"] == registry["api_version"] == "1.1", "Unexpected API version")
        require(response["request_id"] == request["request_id"], "Response correlation mismatch")
        for flag, field in required_fields.items():
            if operations[operation][flag]:
                require(bool(request.get(field)), f"Missing {field}: {row['name']}")
        if operations[operation]["target_context"] == "analysis":
            require(bool(request["parameters"].get("analysis_id")), f"Missing analysis identity: {row['name']}")
        if operations[operation]["requires_profile_match"]:
            ref = request["parameters"].get("expected_profile_ref", {})
            require(all(ref.get(key) for key in ("profile_id", "profile_version", "definition_digest")),
                    f"Missing immutable profile reference: {row['name']}")
        require("solver_profile_id" not in request["parameters"],
                f"Semantic profile conflated with local run configuration: {row['name']}")
        require(response["status"] in {"success", "needs_input", "conflict", "accepted", "failed"},
                f"Unknown response status: {row['name']}")

    first, retry = examples["commit_once"], examples["retry_same_intent"]
    require(first["request"]["request_id"] != retry["request"]["request_id"], "Retry reuses correlation ID")
    for field in ("idempotency_key", "parameters", "document_id", "document_epoch"):
        require(first["request"][field] == retry["request"][field], f"Retry changes {field}")
    for field in ("transaction_id", "revision"):
        require(first["response"][field] == retry["response"][field], f"Retry duplicates {field}")
    require(not examples["missing_unit"]["response"]["mutation_committed"], "Missing unit commits model")
    require(examples["stale_preview"]["response"]["status"] == "conflict", "Stale preview not rejected")
    require(examples["expired_document_epoch"]["response"]["error"]["code"] == "DOCUMENT_EPOCH_EXPIRED",
            "Expired document context not rejected")
    require(not examples["stale_result"]["response"]["data"]["current_model_match"], "Old result treated as current")
    save_as = examples["save_as_metadata_only"]
    require(save_as["request"]["expected_revision"] == save_as["response"]["revision"],
            "SaveAs changes model revision")
    require(not save_as["response"]["data"]["model_history_added"], "SaveAs creates model undo entry")
    lookup = examples["lookup_lost_open_response"]
    require(not operations["operations.get"]["requires_document"],
            "Host operation lookup incorrectly requires an unknown document ID")
    require("document_id" not in lookup["request"]
            and lookup["request"]["parameters"]["lookup_scope"] == "host"
            and bool(lookup["response"]["data"]["document_id"]),
            "Lost open response cannot recover the document identity")
    for name in ("project.create", "project.open", "project.close"):
        require(operations[name].get("idempotency_scope") == "host",
                f"Missing host lifecycle idempotency: {name}")
    start = examples["job_start"]
    require(bool(start["request"]["parameters"].get("run_config_id")), "Missing local run configuration")
    require(start["request"]["parameters"]["expected_profile_ref"] == start["response"]["data"]["profile_ref"],
            "Run does not freeze its semantic profile")
    for key in ("export_identity_map_id", "mapping_rules_version", "codec_version"):
        require(bool(start["response"]["data"].get(key)), f"Run lacks {key}")
    require(examples["stale_result"]["response"]["data"]["export_identity_map_id"]
            == start["response"]["data"]["export_identity_map_id"],
            "Result switched away from its run's frozen identity map")
    capability = examples["target_capabilities"]["response"]["data"]
    require(capability["production_backend_count"] == 1 and not capability["validated"],
            "Design example claims a validated or additional production backend")
    require(examples["profile_mismatch"]["response"]["error"]["code"] == "PROFILE_MISMATCH",
            "Inconsistent expected profile is not rejected")
    return len(operations), len(examples)


def check_profile_fixtures(root: Path) -> None:
    fixture = load_json(root / "docs/contracts/solver-profile-examples.json")
    require(fixture["schema_version"] == "1.1", "Unexpected profile fixture version")
    scope = fixture["p0_scope"]
    require(scope["production_backend_count"] == 1 and scope["solver_family"] == "nastran",
            "P0 silently expands the production solver scope")
    require(fixture["analysis_definition"]["target_binding"]["profile_ref"] == fixture["profile_ref"],
            "Target is not bound to the analysis definition")
    source_keys = [(entry["source_model_id"], entry["namespace"], entry["external_id"])
                   for entry in fixture["source_identifiers"]]
    require(len(source_keys) == len(set(source_keys)), "Source identifier namespace collision")
    maps = fixture["export_identity_maps"]
    require(sum(not item["test_only"] for item in maps) == 1, "More than one production mapping in P0 fixture")
    reference_entities = {entry["entity_id"] for entry in maps[0]["entries"]}
    for item in maps:
        keys = [(entry["namespace"], entry["external_id"]) for entry in item["entries"]]
        require(len(keys) == len(set(keys)), f"Ambiguous P0 export identifiers: {item['map_id']}")
        require({entry["entity_id"] for entry in item["entries"]} == reference_entities,
                "Fixture renumbers platform entities when changing export targets")
        if not item["test_only"]:
            require(item["profile_ref"] == fixture["profile_ref"], "Production map uses the wrong profile")
    fields = fixture["field_ownership"]
    require(len({field["semantic_field"] for field in fields}) == len(fields), "Multiple owners of one field")
    for field in fields:
        require(field["owner"] in {"core", "extension", "derived"}, "Unknown field authority")
        if field["owner"] == "core":
            require(field["projection_authority"] == "derived" and not field["independently_writable"],
                    "Native projection competes with its core field")
    for report in fixture["conversion_reports"]:
        require(report["equivalence"] in {"exact", "conditional", "approximate", "unsupported"},
                "Invalid mapping equivalence")
        if report["equivalence"] == "unsupported" or not report["resolved"]:
            require(not report["publishable"], "Unresolved conversion can be published as valid")
    result = fixture["result_field"]
    for key in ("quantity_id", "unit", "shape", "component_names", "location", "coordinate_basis",
                "analysis_case", "export_identity_map_id", "reader_version", "values_resource"):
        require(bool(result.get(key)), f"Missing result semantics: {key}")
    require(result["export_identity_map_id"] in {item["map_id"] for item in maps}, "Unknown result identity map")
    require("sample_axis" in result and "derivation" in result, "Implicit result sampling or derivation")


def check_traceability(root: Path) -> tuple[int, int]:
    requirements = (root / "docs/baseline/requirements.md").read_text(encoding="utf-8")
    acceptance = (root / "docs/baseline/acceptance.md").read_text(encoding="utf-8")
    req_rows = re.findall(r"^\| (REQ-\d{2}) \|", requirements, re.MULTILINE)
    require(len(req_rows) == len(set(req_rows)) == 20, "Expected 20 unique requirements")
    require(set(req_rows) == {f"REQ-{n:02}" for n in range(1, 21)}, "Requirement ID gap")
    tests = re.findall(r"^\| (TST-[FAIP]\d{2})\b", acceptance, re.MULTILINE)
    expected = {f"TST-{kind}{n:02}" for kind, count in (("F", 8), ("A", 12), ("I", 10), ("P", 8))
                for n in range(1, count + 1)}
    require(len(tests) == len(set(tests)) == 38 and set(tests) == expected, "Acceptance ID mismatch")
    for req in req_rows:
        require(re.search(rf"^\| {req} [^|]*\| TST-", acceptance, re.MULTILINE) is not None,
                f"Missing coverage mapping: {req}")
    for prefix, count in (("AR", 12), ("AI", 10)):
        for n in range(1, count + 1):
            require(f"/ {prefix}-{n:02} " in acceptance, f"Missing historical mapping: {prefix}-{n:02}")
    return len(req_rows), len(tests)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        links = check_links(root)
        json_files = list((root / "docs").rglob("*.json"))
        for path in json_files:
            load_json(path)
        modules = check_dependencies(root)
        operations, examples = check_contracts(root)
        check_profile_fixtures(root)
        requirements, tests = check_traceability(root)
    except (DesignError, KeyError, TypeError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: {links} portable links; {len(json_files)} JSON files; {modules}-module acyclic graph")
    print(f"PASS: {operations} operation descriptors; {examples} contract examples; {requirements} requirements; {tests} acceptance cases")
    print("Design checks only. This command does not run product builds, solver runs or AI acceptance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
