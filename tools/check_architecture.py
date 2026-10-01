#!/usr/bin/env python3
"""Check production ownership, includes and the configured CMake target graph.

The file API is the source of truth for compiled sources and build dependencies.
An expanded CMake trace supplements it with direct/public/interface links, which
codemodel-v2 intentionally omits for INTERFACE libraries. No product files change.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import subprocess
import sys

PRODUCTION_DIRS = ("modules", "features", "profiles", "adapters", "ui", "apps")
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".c"}
HEADER_SUFFIXES = {".hpp", ".h", ".hh", ".hxx"}
RUNTIME_SUFFIXES = {".py"}
FRAMEWORK = re.compile(r"(?:^|[/ :])(?:Qt[0-9]*(?:::|/)|Q[A-Z][a-z][A-Za-z0-9_]*(?:[/.]|$)|vtk|VTK|sqlite|SQLite|MCP)")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
SCOPE = {"PUBLIC", "PRIVATE", "INTERFACE"}


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def cmake_evidence(root: Path, build: Path, refresh: bool) -> tuple[dict, list[dict]]:
    root, build = root.resolve(), build.resolve()
    query = build / ".cmake/api/v1/query"
    trace = build / ".cmake/api/v1/architecture-trace.jsonl"
    if refresh:
        query.mkdir(parents=True, exist_ok=True)
        (query / "codemodel-v2").touch()
        (query / "cache-v2").touch()
        command = ["cmake", "-S", str(root), "-B", str(build), "--trace-expand",
                   "--trace-format=json-v1", f"--trace-redirect={trace}"]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode:
            raise ValueError("CMake evidence refresh failed:\n" + result.stdout + result.stderr)
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    if not indices or not trace.is_file():
        raise ValueError("Missing CMake file API/trace evidence; rerun without --no-refresh")
    if not refresh:
        cmake_inputs = [root / "CMakeLists.txt"]
        cmake_inputs.extend(path for directory in PRODUCTION_DIRS
                            for path in (root / directory).rglob("CMakeLists.txt"))
        if any(path.stat().st_mtime > trace.stat().st_mtime for path in cmake_inputs if path.exists()):
            raise ValueError("CMake evidence is stale; rerun without --no-refresh")
    index = read_json(indices[-1])
    refs = {entry["kind"]: entry["jsonFile"] for entry in index["objects"]}
    model = read_json(reply / refs["codemodel"])
    if Path(model["paths"]["source"]).resolve() != root:
        raise ValueError("CMake evidence belongs to a different source directory")
    cache = read_json(reply / refs["cache"])
    evidence = {"configurations": [], "build": str(build),
                "cache": {e["name"]: e["value"] for e in cache["entries"]}}
    for configuration in model["configurations"]:
        targets = [read_json(reply / entry["jsonFile"]) for entry in configuration["targets"]]
        evidence["configurations"].append({"name": configuration["name"], "targets": targets})
    commands = [json.loads(line) for line in trace.read_text().splitlines() if line.startswith("{")]
    return evidence, commands


def closure(graph: dict[str, set[str]], start: str) -> set[str]:
    found: set[str] = set()
    pending = list(graph.get(start, ()))
    while pending:
        name = pending.pop()
        if name in found:
            continue
        found.add(name)
        pending.extend(graph.get(name, ()))
    return found


def enabled(row: dict, cache: dict) -> bool:
    option = row.get("option")
    return not option or cache.get(option, "OFF").upper() not in {"OFF", "FALSE", "0", "NO", ""}


def links(row: dict, cache: dict) -> set[str]:
    result = set(row.get("public_dependencies", []) + row.get("private_dependencies", []))
    for field in ("conditional_dependencies", "conditional_public_dependencies"):
        for option, names in row.get(field, {}).items():
            if enabled({"option": option}, cache):
                result.update(names)
    return result


def external_allowed(row: dict, dependency: str) -> bool:
    allowed = set(row.get("external_dependencies", []))
    if dependency in allowed:
        return True
    # VTK's imported component list varies with the selected installation.
    return "${VTK_LIBRARIES}" in allowed and dependency.startswith("VTK::")


def core(row: dict) -> bool:
    path = row["path"]
    return path.startswith("modules/") and not path.startswith("modules/clients")


def client(row: dict) -> bool:
    return row["path"].startswith(("modules/clients", "ui/", "apps/cli", "apps/desktop", "apps/mcp"))


def domain_or_storage(row: dict) -> bool:
    return row["path"].startswith(("modules/document", "modules/application", "modules/query",
                                   "adapters/storage", "adapters/engine_api", "profiles/"))


def check(root: Path, manifest: dict, evidence: dict, commands: list[dict]) -> list[str]:
    root = root.resolve()
    errors: list[str] = []
    def reject(code: str, message: str):
        errors.append(f"{code}: {message}")
    rows = manifest.get("targets", [])
    targets = {row["target"]: row for row in rows}
    if not rows or len(targets) != len(rows):
        reject("manifest", "target list is empty or contains duplicate names")
    cache = evidence["cache"]
    active = {name: row for name, row in targets.items() if enabled(row, cache)}
    owners: dict[str, str] = {}
    public: dict[str, str] = {}
    for name, row in targets.items():
        for key in ("sources", "public_headers", "private_headers", "runtime_sources"):
            for filename in row.get(key, []):
                if filename in owners:
                    reject("ownership", f"{filename} owned by {owners[filename]} and {name}")
                owners[filename] = name
                path = root / filename
                if not path.is_file() or not path.resolve().is_relative_to(root):
                    reject("ownership", f"missing/outside source file: {filename}")
                if key == "public_headers":
                    public[filename] = name
                    if not any(path.is_relative_to(root / directory) for directory in row["public_include_dirs"]):
                        reject("public-api", f"{filename} is outside its public include roots")
    for name, row in targets.items():
        for directory in row["public_include_dirs"]:
            for filename, owner in owners.items():
                if (root / filename).is_relative_to(root / directory):
                    if owner != name or filename not in public:
                        reject("private-include", f"{name} public include root exposes {filename}")
    production = {str(path.relative_to(root)) for directory in PRODUCTION_DIRS
                  for path in (root / directory).rglob("*")
                  if path.is_file() and path.suffix in SOURCE_SUFFIXES | HEADER_SUFFIXES | RUNTIME_SUFFIXES}
    for filename in sorted(production - owners.keys()):
        reject("ownership", f"unregistered production file: {filename}")

    direct: dict[str, set[str]] = defaultdict(set)
    public_links: dict[str, set[str]] = defaultdict(set)
    includes: dict[str, set[str]] = defaultdict(set)
    declared: dict[str, str] = {}
    for command in commands:
        operation, args = command.get("cmd", ""), command.get("args", [])
        if not args:
            continue
        name = args[0]
        if operation in {"add_library", "add_executable"} and name in active:
            declared[name] = command["file"]
        if name not in active:
            continue
        if operation == "target_link_libraries":
            scope = "PUBLIC"
            for value in args[1:]:
                if value in SCOPE:
                    scope = value
                elif value not in {"debug", "optimized", "general"}:
                    for dependency in value.split(";"):
                        if dependency:
                            direct[name].add(dependency)
                            if scope != "PRIVATE":
                                public_links[name].add(dependency)
        if operation == "target_include_directories":
            scope = "PRIVATE"
            for value in args[1:]:
                if value in SCOPE:
                    scope = value
                elif scope != "PRIVATE" and value not in {"SYSTEM", "BEFORE", "AFTER"}:
                    includes[name].update(value.split(";"))
    for name, row in active.items():
        if name not in declared:
            reject("cmake-target", f"manifest target not declared by CMake: {name}")
        expected = links(row, cache)
        literal = {d for d in expected if not d.startswith("${")}
        for dependency in sorted(direct[name] - literal):
            if not external_allowed(row, dependency):
                reject("undeclared-dependency", f"{name} links undeclared {dependency}")
        for variable in sorted(expected - literal):
            if variable == "${VTK_LIBRARIES}":
                matches = any(d.startswith("VTK::") for d in direct[name])
            elif variable == "${QCAE_SQLITE_TARGET}":
                matches = bool(direct[name] & {"SQLite3::SQLite3", "SQLite::SQLite3"})
            else:
                matches = False
            if not matches:
                reject("manifest-link", f"{name} has no recognized expansion for {variable}")
        for dependency in sorted(literal - direct[name]):
            reject("manifest-link", f"{name} declares absent direct dependency {dependency}")
        expected_public = {d for d in row.get("public_dependencies", []) if not d.startswith("${")}
        for option, names in row.get("conditional_public_dependencies", {}).items():
            if enabled({"option": option}, cache):
                expected_public.update(names)
        for dependency in sorted(expected_public - public_links[name]):
            reject("public-api", f"{name} does not export declared public dependency {dependency}")
        for dependency in sorted(public_links[name] - expected_public):
            if not external_allowed(row, dependency):
                reject("public-api", f"{name} exports undeclared dependency {dependency}")
        for path in includes[name]:
            if path.startswith("$<"):
                reject("include-root", f"unresolved public include expression on {name}: {path}")
                continue
            absolute = Path(path).resolve()
            build = Path(evidence["build"]).resolve()
            generated = set()
            for directory in row.get("generated_public_include_dirs", []):
                candidate = (build / directory).resolve()
                if Path(directory).is_absolute() or not candidate.is_relative_to(build) or candidate == build:
                    reject("include-root", f"{name} declares unsafe generated include root {directory}")
                else:
                    generated.add(candidate)
            if absolute in generated:
                continue
            if absolute.is_relative_to(build):
                reject("private-include", f"{name} exports undeclared generated include root {absolute}")
            elif absolute.is_relative_to(root):
                relative = str(absolute.relative_to(root))
                if relative not in row["public_include_dirs"]:
                    reject("private-include", f"{name} exports undeclared include root {relative}")
            if core(row) and FRAMEWORK.search(path):
                reject("core-framework", f"{name} exports framework include directory {path}")

    graph = {name: {d for d in direct[name] if d in active} for name in active}
    for name, row in active.items():
        reached = closure(graph, name)
        if name in reached:
            reject("cycle", f"dependency cycle reaches {name}")
        if client(row):
            for dependency in sorted(reached):
                if domain_or_storage(active[dependency]):
                    reject("client-domain", f"{name} transitively links {dependency}")
        if core(row):
            for dependency in {name} | reached:
                for external in direct[dependency]:
                    if FRAMEWORK.search(external):
                        reject("core-framework", f"{name} reaches framework dependency {external}")

    # Resolve quoted/angle project includes against explicit public roots and the
    # including file's own directory. Private filenames are never global roots.
    public_graph = {name: {d for d in row.get("public_dependencies", []) if d in targets}
                    for name, row in targets.items()}
    for name, row in targets.items():
        for names in row.get("conditional_public_dependencies", {}).values():
            public_graph[name].update(d for d in names if d in targets)
    public_roots = [root / p for row in targets.values() for p in row["public_include_dirs"]]
    for filename, owner in sorted(owners.items()):
        path = root / filename
        if not path.is_file():
            continue
        for include in INCLUDE.findall(path.read_text()):
            if core(targets[owner]) and FRAMEWORK.search(include):
                reject("core-framework", f"{filename} includes {include}")
            candidates = [(path.parent / include).resolve(), (root / include).resolve()]
            candidates.extend((directory / include).resolve() for directory in public_roots)
            matches = [str(p.relative_to(root)) for p in candidates
                       if p.is_relative_to(root) and str(p.relative_to(root)) in owners]
            if not matches:
                # Recognize private header basenames even when a forbidden -I
                # makes the include resolve outside the expected public roots.
                matches = [f for f in owners if f not in public and Path(f).name == include]
            for included in set(matches):
                dependency = owners[included]
                if filename in public and included not in public:
                    reject("private-header", f"public {filename} exposes private {included}")
                if dependency == owner:
                    continue
                if included not in public:
                    reject("private-header", f"{filename} includes private {included}")
                allowed = links(targets[owner], cache)
                # Static scanning sees all preprocessor branches, including
                # includes guarded by disabled adapter options.
                for conditional in targets[owner].get("conditional_dependencies", {}).values():
                    allowed.update(conditional)
                for conditional in targets[owner].get("conditional_public_dependencies", {}).values():
                    allowed.update(conditional)
                for direct_dependency in list(allowed):
                    allowed.update(closure(public_graph, direct_dependency))
                if dependency not in allowed:
                    reject("undeclared-dependency", f"{filename} includes {dependency} outside declared public usage requirements")

    for configuration in evidence["configurations"]:
        compiled = configuration["targets"]
        by_id = {t["id"]: t["name"] for t in compiled}
        present = {t["name"] for t in compiled}
        for name, row in active.items():
            if row["kind"] != "INTERFACE" and name not in present:
                reject("file-api", f"configured target missing from codemodel: {name}")
        for actual in compiled:
            name = actual["name"]
            actual_sources: set[str] = set()
            for source in actual.get("sources", []):
                path = Path(source["path"])
                path = path if path.is_absolute() else root / path
                if source.get("isGenerated") or path.suffix not in SOURCE_SUFFIXES:
                    continue
                if path.is_relative_to(root):
                    relative = str(path.relative_to(root))
                    if not relative.startswith("tests/"):
                        actual_sources.add(relative)
                        if owners.get(relative) != name:
                            reject("file-api-ownership", f"{name} compiles {relative}, owned by {owners.get(relative)}")
            if name not in active:
                continue
            expected_kind = {"STATIC": "STATIC_LIBRARY", "SHARED": "SHARED_LIBRARY",
                             "MODULE": "MODULE_LIBRARY", "OBJECT": "OBJECT_LIBRARY",
                             "EXECUTABLE": "EXECUTABLE"}.get(active[name]["kind"])
            if actual["type"] != expected_kind:
                reject("file-api", f"{name} kind is {actual['type']}, expected {expected_kind}")
            expected_sources = set(active[name].get("sources", []))
            if actual_sources != expected_sources:
                reject("file-api-ownership", f"{name} compiled sources differ: {sorted(actual_sources ^ expected_sources)}")
            actual_dependencies = {by_id[d["id"]] for d in actual.get("dependencies", []) if d["id"] in by_id}
            expected_reachable = closure(graph, name)
            for dependency in actual_dependencies:
                # Generated helper targets are build-order prerequisites, not
                # architectural libraries. Production targets must be registered.
                target = next(t for t in compiled if t["name"] == dependency)
                if target["type"] == "UTILITY":
                    continue
                if dependency not in expected_reachable:
                    reject("file-api-dependency", f"{name} actually depends on undeclared {dependency}")
            for group in actual.get("compileGroups", []):
                for item in group.get("includes", []):
                    path = Path(item["path"]).resolve()
                    if core(active[name]) and FRAMEWORK.search(str(path)):
                        reject("core-framework", f"{name} compiles with framework include {path}")
                    for other, row in targets.items():
                        if other == name:
                            continue
                        for private in row.get("private_include_dirs", []):
                            if path == (root / private).resolve():
                                reject("private-include", f"{name} compiles with {other}'s private include root")
    return sorted(set(errors))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--no-refresh", action="store_true", help="use existing configured evidence")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root, build = args.root.resolve(), args.build_dir.resolve()
    try:
        manifest = read_json(root / "modules/targets.json")
        evidence, commands = cmake_evidence(root, build, not args.no_refresh)
        errors = check(root, manifest, evidence, commands)
    except (OSError, ValueError, KeyError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 2
    report = {"status": "failed" if errors else "passed", "errors": errors,
              "registered_targets": len(manifest["targets"]),
              "configured_targets": sum(enabled(row, evidence["cache"]) for row in manifest["targets"]),
              "production_units": sum(len(row.get("sources", [])) for row in manifest["targets"]),
              "source": str(root), "build": str(build),
              "evidence": "CMake codemodel-v2/cache-v2 + expanded configure trace + actual source includes"}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    if errors:
        print("\n".join("FAIL: " + error for error in errors), file=sys.stderr)
        return 1
    print(f"PASS: {report['production_units']} production units owned; "
          f"{report['configured_targets']} configured target boundaries checked against CMake")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
