#!/usr/bin/env python3
"""Verify a real C4 source commit and write one external freeze manifest.

Git operations are read-only. Commands in execution evidence are never run.
Initial context comes exclusively from commit blobs, including AGENTS.md.
Mechanism freezing does not declare C3/SK-12 or extension acceptance complete.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PROTECTED = "docs/engineering/c4-protected-paths.json"
TARGETS = "docs/baseline/skeleton-acceptance-targets.json"
INITIAL = (
    "AGENTS.md",
    "docs/engineering/c4-executor-task.md",
    "schemas/entities/entities.json",
    "schemas/operations/basic.json",
    "features/mesh_editing/src/mesh_editing_operations.cpp",
    "modules/document/include/qcae/edit_session.hpp",
    "modules/contracts/include/qcae/render_packet.hpp",
    "modules/query/include/qcae/render_projector.hpp",
)
GENERATORS = (
    "tools/generate_entities.py",
    "tools/generate_operation_contracts.py",
    "tools/generate_operations.py",
    "tools/generate_profile.py",
)
SOURCE_ROOTS = (
    "CMakeLists.txt", ".clang-format", "cmake", "modules", "features",
    "adapters", "apps", "ui", "profiles", "schemas", "tests", "tools",
)
QUALITY_GATES = (
    "design", "cpp_format", "diff_check", "protected_ast",
    "public_api_consumers", "readability",
)
MATRICES = {
    "core": (False, False, False, False, False),
    "sqlite_headless": (False, False, True, False, False),
    "desktop": (True, True, True, False, False),
    "asan_ubsan": (False, False, True, True, True),
}
CONFIG_FIELDS = ("qt_enabled", "vtk_enabled", "sqlite_enabled", "asan_enabled", "ubsan_enabled")
ENV_FIELDS = {
    "schema", "frozen", "os", "cpu", "memory_bytes", "gpu", "graphics_backend",
    "compiler", "cmake", "clang_format", "python", "qt", "vtk", "sqlite",
    "build_type", "threads", "device_pixel_ratio", "framebuffer_pixels",
    "font_backend", "tool_images", "probe_evidence",
}
TOOL_ROLES = {"compiler", "cmake", "clang_format", "python", "qt", "vtk", "sqlite"}
HASH_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
SENSITIVE_NAME = re.compile(r"(?i)(\.env(?:\..*)?|credentials?(?:\..*)?|secrets?(?:\..*)?|.*\.(pem|p12|pfx|key))")
MAX_JSON_BYTES = 2 * 1024 * 1024
MAX_BLOB_BYTES = 64 * 1024 * 1024
COPY_TARGETS = {
    "node_block_capacity": 1024, "node_record_encoded_bytes_max": 256,
    "dirty_node_blocks_max": 2, "material_record_encoded_bytes_max": 4096,
    "metadata_payload_bytes_max": 65536, "node_edit_payload_bytes_max": 589824,
    "material_edit_payload_bytes_max": 69632,
}


class FreezeError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise FreezeError(message)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(131072), b""):
            result.update(block)
    return result.hexdigest()


def safe_name(value: str) -> str:
    require(isinstance(value, str) and value and "\\" not in value and "\0" not in value,
            "Invalid relative artifact/source path")
    parts = PurePosixPath(value).parts
    require(not value.startswith("/") and all(part not in (".", "..") for part in parts)
            and PurePosixPath(value).as_posix() == value, "Noncanonical relative path")
    require(not any(SENSITIVE_NAME.fullmatch(part) for part in parts),
            "Credential/configuration path is outside the freeze reader scope")
    return value


def source_path(name: str) -> bool:
    return any(name == prefix or name.startswith(prefix + "/") for prefix in SOURCE_ROOTS)


def json_value(data: bytes) -> dict:
    require(len(data) <= MAX_JSON_BYTES, "JSON evidence exceeds the bounded reader limit")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "Duplicate JSON key in evidence")
            result[key] = value
        return result

    def nonfinite(_):
        raise FreezeError("Nonfinite JSON value in evidence")

    value = json.loads(data.decode("utf-8"), object_pairs_hook=unique, parse_constant=nonfinite)
    require(isinstance(value, dict), "Evidence must be a JSON object")
    return value


class Repository:
    def __init__(self, root: Path, commit: str):
        self.root = root.resolve(strict=True)
        self.commit = commit
        require(COMMIT_RE.fullmatch(commit) is not None, "A full actual source commit is required")
        require(self.git("rev-parse", "--show-toplevel").decode().strip() == str(self.root),
                "Root must be the actual Git worktree root")
        require(self.git("cat-file", "-t", commit).strip() == b"commit", "Source object is not a commit")
        self.head = self.git("rev-parse", "HEAD").decode().strip()
        require(self.head == commit, "The worktree is not checked out at the supplied source commit")
        self.tree_oid = self.git("rev-parse", commit + "^{tree}").decode().strip()
        self.entries = {}
        for record in self.git("ls-tree", "-r", "-z", commit).split(b"\0"):
            if not record:
                continue
            header, name = record.split(b"\t", 1)
            mode, kind, oid = header.decode("ascii").split()
            name = safe_name(name.decode("utf-8"))
            self.entries[name] = (mode, kind, oid)
        self.blobs: dict[str, bytes] = {}

    def git(self, *arguments: str, input_bytes: bytes | None = None) -> bytes:
        # Disable optional writes and configurable filesystem-monitor commands.
        # Do not enumerate/read token or credential environment values. Preserve
        # only system executable/locale/temporary-directory lookup for Git.
        environment = {name: os.environ[name] for name in
                       ("PATH", "LANG", "LC_ALL", "TMPDIR", "SystemRoot", "WINDIR", "PATHEXT", "DEVELOPER_DIR")
                       if name in os.environ}
        environment.update(GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0",
                           GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1", GIT_NO_LAZY_FETCH="1")
        process = subprocess.run(
            ["git", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false", *arguments],
            cwd=self.root, env=environment, input=input_bytes, capture_output=True, timeout=60,
        )
        require(process.returncode == 0, "Read-only Git query failed; source/commit prerequisite unavailable")
        return process.stdout

    def blob(self, name: str) -> bytes:
        self.load_blobs([name])
        return self.blobs[self.entries[name][2]]

    def load_blobs(self, names: list[str]) -> None:
        objects = []
        for name in names:
            require(name in self.entries, "Required path is absent from the supplied commit: " + name)
            mode, kind, oid = self.entries[name]
            require(mode in ("100644", "100755") and kind == "blob",
                    "Frozen inputs must be regular commit blobs: " + name)
            if oid not in self.blobs and oid not in objects:
                objects.append(oid)
        if not objects:
            return
        result = self.git("cat-file", "--batch", input_bytes=("\n".join(objects) + "\n").encode())
        position = 0
        for oid in objects:
            end = result.index(b"\n", position)
            actual, kind, size = result[position:end].decode("ascii").split()
            count = int(size)
            require(actual == oid and kind == "blob" and count <= MAX_BLOB_BYTES,
                    "Unexpected or oversized source blob")
            position = end + 1
            self.blobs[oid] = result[position:position + count]
            require(len(self.blobs[oid]) == count and result[position + count:position + count + 1] == b"\n",
                    "Incomplete Git blob response")
            position += count + 1
        require(position == len(result), "Unexpected trailing Git blob response")

    def fact(self, name: str) -> dict:
        data = self.blob(name)
        mode, _, oid = self.entries[name]
        return {"path": name, "git_blob_oid": oid, "mode": mode,
                "bytes": len(data), "sha256": sha(data)}


def source_digest(facts: list[dict]) -> str:
    """SHA256(domain || sorted[len64be(path),path,mode6,rawSHA256(blob)]).

    Sort by UTF-8 path bytes. AGENTS and docs are separately bound inputs, not
    part of this production/test/tool tree digest. Every source blob is regular.
    """
    result = hashlib.sha256(b"QCAE-C4-SOURCE-SHA256-v1\0")
    for item in sorted(facts, key=lambda row: row["path"].encode("utf-8")):
        path = item["path"].encode("utf-8")
        result.update(len(path).to_bytes(8, "big"))
        result.update(path)
        result.update(item["mode"].encode("ascii"))
        result.update(bytes.fromhex(item["sha256"]))
    return result.hexdigest()


def committed_source_facts(repo: Repository) -> list[dict]:
    """Public read-only helper for stamping executed evidence after committing."""
    names = sorted(name for name in repo.entries if source_path(name))
    repo.load_blobs(names)
    return [repo.fact(name) for name in names]


def initial_packet(repo: Repository) -> dict:
    rows = []
    for name in INITIAL:
        data = repo.blob(name)
        data.decode("utf-8", errors="strict")
        heading = f"--- {name} ({len(data)} UTF-8 bytes) ---\n".encode("utf-8")
        trailing = b"" if data.endswith(b"\n") else b"\n"
        rows.append({**repo.fact(name), "utf8_bytes": len(data),
                     "delivered_utf8_bytes": len(heading) + len(data) + len(trailing),
                     "delivery_reads": 0, "source": "specified_commit_blob"})
    content_bytes = sum(row["utf8_bytes"] for row in rows)
    delivered_bytes = sum(row["delivered_utf8_bytes"] for row in rows)
    require(content_bytes <= 65536 and delivered_bytes <= 65536,
            "The eight-file initial packet exceeds 65536 UTF-8 delivery bytes")
    return {"files": rows, "file_count": len(rows), "content_utf8_bytes": content_bytes,
            "delivered_utf8_bytes": delivered_bytes,
            "delivery_encoding": "c4_context_read.py heading + exact UTF8 blob + optional trailing newline",
            "delivery_state": "planned bytes verified; no executor context has been delivered by this tool",
            "subsequent_read_bytes": None, "tokens": None}


class EvidenceReader:
    def __init__(self, root: Path, output: Path):
        self.root = root
        self.output = output
        self.artifacts: dict[str, dict] = {}

    def artifact(self, descriptor: dict) -> dict:
        require(isinstance(descriptor, dict) and set(descriptor) == {"path", "sha256"},
                "Artifact descriptor must contain only path and sha256")
        name = safe_name(descriptor["path"])
        expected = descriptor["sha256"]
        require(isinstance(expected, str) and HASH_RE.fullmatch(expected) is not None,
                "Artifact SHA256 is missing or malformed")
        path = (self.root / name).resolve(strict=True)
        require(path.is_relative_to(self.root) and path.is_file() and path != self.output,
                "Artifact escaped the root or referenced the output manifest itself")
        actual = file_sha(path)
        require(actual == expected, "Evidence bytes differ from their declared SHA256: " + name)
        result = {"path": name, "sha256": actual, "bytes": path.stat().st_size}
        self.artifacts[name] = result
        return result


def execution_record(record: dict, reader: EvidenceReader, build: bool) -> dict:
    expected = {"passed", "commands", "exit_codes", "logs"}
    if build:
        expected |= {"configuration", "artifacts"}
    require(isinstance(record, dict) and set(record) == expected, "Execution evidence fields are incomplete/unknown")
    commands, exits = record["commands"], record["exit_codes"]
    require(record["passed"] is True and isinstance(commands, list) and commands
            and isinstance(exits, list) and len(commands) == len(exits), "Execution record is unexecuted or failed")
    require(all(type(value) is int and value == 0 for value in exits), "Nonzero or malformed execution exit code")
    require(all(isinstance(command, list) and command and all(isinstance(arg, str) and arg for arg in command)
                for command in commands), "Commands must be original argv arrays")
    require(not any(re.search(r"(?i)(api[_-]?key|access[_-]?token|password|secret|credential|bearer|authorization)", arg)
                    for command in commands for arg in command), "Sensitive command arguments are outside this reader scope")
    require(isinstance(record["logs"], list) and record["logs"], "Executed evidence needs nonempty raw logs")
    result = {"passed": True, "command_count": len(commands), "exit_codes": exits,
              "commands_sha256": sha(json.dumps(commands, ensure_ascii=False, separators=(",", ":")).encode()),
              "raw_logs": [reader.artifact(row) for row in record["logs"]],
              "execution_provenance": "provided execution record, verified hashes; tool does not rerun commands"}
    if build:
        configuration = record["configuration"]
        require(isinstance(configuration, dict) and set(configuration) == {"build_type", *CONFIG_FIELDS},
                "Build configuration requires the explicit five feature/sanitizer flags")
        require(all(type(configuration[field]) is bool for field in CONFIG_FIELDS), "Build flags must be actual booleans")
        require(isinstance(record["artifacts"], list) and record["artifacts"], "Build needs actual artifacts")
        result.update(configuration=configuration,
                      artifacts=[reader.artifact(row) for row in record["artifacts"]])
    return result


def environment_record(data: bytes, reader: EvidenceReader) -> dict:
    environment = json_value(data)
    require(set(environment) == ENV_FIELDS and environment["schema"] == "qcae.c4-environment/1"
            and environment["frozen"] is True, "Environment is missing fields or contains unapproved configuration")
    for field in ("os", "cpu", "gpu", "graphics_backend", "compiler", "cmake", "clang_format",
                  "python", "qt", "vtk", "sqlite", "font_backend"):
        require(isinstance(environment[field], str) and 0 < len(environment[field]) <= 1024,
                "Invalid non-sensitive system/tool version field")
    require(environment["clang_format"].startswith("21"), "C4 formatting must retain clang-format21")
    require(environment["build_type"] == "Release" and environment["framebuffer_pixels"] == [1280, 720],
            "Frozen Release/framebuffer configuration mismatch")
    require(type(environment["memory_bytes"]) is int and environment["memory_bytes"] > 0
            and type(environment["threads"]) is int and environment["threads"] > 0,
            "Memory/thread counts must be positive integers")
    ratio = environment["device_pixel_ratio"]
    require(type(ratio) in (int, float) and math.isfinite(ratio) and ratio > 0, "Invalid device pixel ratio")
    images = environment["tool_images"]
    require(isinstance(images, list) and images and {row.get("id") for row in images} >= TOOL_ROLES,
            "Environment lacks actual compiler/CMake/format/Python/Qt/VTK/SQLite image hashes")
    require(len({row["id"] for row in images}) == len(images), "Duplicate tool image identity")
    for row in images:
        require(set(row) == {"id", "path", "sha256"}, "Unknown tool image fields")
        path = Path(row["path"])
        require(path.is_absolute() and not SENSITIVE_NAME.fullmatch(path.name), "Tool image must be an explicit non-sensitive absolute path")
        require(path.is_file() and HASH_RE.fullmatch(row["sha256"]) is not None
                and file_sha(path) == row["sha256"], "Actual tool image hash mismatch")
    require(isinstance(environment["probe_evidence"], list) and environment["probe_evidence"],
            "Environment needs actual non-sensitive probe evidence")
    for row in environment["probe_evidence"]:
        reader.artifact(row)
    return environment


def evidence_records(data: bytes, reader: EvidenceReader, commit: str, digest: str, env_sha: str) -> dict:
    evidence = json_value(data)
    require(set(evidence) == {"schema", "source_commit", "source_tree_sha256", "environment_manifest_sha256",
                              "quality_gates", "build_matrix"}
            and evidence["schema"] == "qcae.c4-freeze-execution/1", "Wrong or incomplete freeze execution schema")
    require(evidence["source_commit"] == commit and evidence["source_tree_sha256"] == digest
            and evidence["environment_manifest_sha256"] == env_sha, "Execution evidence source/tree/environment binding mismatch")
    require(isinstance(evidence["quality_gates"], dict) and set(evidence["quality_gates"]) == set(QUALITY_GATES),
            "Missing or unknown common quality gates")
    require(isinstance(evidence["build_matrix"], dict) and set(evidence["build_matrix"]) == set(MATRICES),
            "All four actual build/test matrices are required")
    quality = {name: execution_record(evidence["quality_gates"][name], reader, False) for name in QUALITY_GATES}
    matrices = {}
    for name, flags in MATRICES.items():
        result = execution_record(evidence["build_matrix"][name], reader, True)
        configuration = result["configuration"]
        require(tuple(configuration[field] for field in CONFIG_FIELDS) == flags,
                "Build/test matrix feature or sanitizer configuration mismatch: " + name)
        require(configuration["build_type"] == ("Debug" if name == "asan_ubsan" else "Release"),
                "Build/test matrix configuration mismatch: " + name)
        matrices[name] = result
    return {"quality_gates": quality, "build_matrix": matrices}


def working_tree_check(repo: Repository, facts: list[dict], excluded_agents: bool,
                       external_paths: set[str], frozen_inputs: set[str]) -> dict:
    require(not any(source_path(name) or name in frozen_inputs for name in external_paths),
            "External evidence cannot exempt initial/protected/product source from commit comparison")
    changed = []
    for row in facts:
        name = row["path"]
        if name == "AGENTS.md" and excluded_agents:
            continue
        path = repo.root / name
        if path.is_symlink() or not path.is_file() or file_sha(path) != row["sha256"]:
            changed.append(name)
        elif bool(path.stat().st_mode & 0o111) != (row["mode"] == "100755"):
            changed.append(name)
    untracked = [safe_name(name.decode()) for name in repo.git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0") if name]
    unexpected = sorted(name for name in untracked if name not in external_paths)
    require(not changed and not unexpected, "Working tree differs from baseline outside explicit user-instruction/artifact exclusions")
    require(repo.git("diff", "--cached", "--name-only", "-z", repo.commit) == b"",
            "The index contains staged changes outside the supplied commit")
    return {"all_nonexcluded_commit_files_exact": True, "untracked_nonartifact_files": [],
            "user_instructions_working_content_read": not excluded_agents,
            "excluded_user_instruction_path": "AGENTS.md" if excluded_agents else None,
            "initial_AGENTS_source": "committed blob; working user diff never substitutes initial context",
            "external_evidence_paths": sorted(external_paths),
            "global_worktree_clean_claimed": not excluded_agents and not external_paths}


def freeze(arguments) -> dict:
    repo = Repository(arguments.root, arguments.source_commit)
    require(arguments.output not in (arguments.environment, arguments.evidence_spec), "Output cannot be an input/self reference")
    if arguments.output.is_relative_to(repo.root):
        require(arguments.output.relative_to(repo.root).as_posix() not in repo.entries,
                "Output manifest must be external to the baseline commit")
    protected = json_value(repo.blob(PROTECTED))
    paths, fixtures = protected.get("paths"), protected.get("immutable_fixture_paths")
    require(isinstance(paths, list) and len(paths) == len(set(paths)) and len(paths) >= 38,
            "The committed protection denominator must retain the original 38-file contract minimum")
    require(set(GENERATORS) <= set(paths), "Four original generic generators must be protected")
    require(protected.get("baseline_commit") in (None, repo.commit), "Protection draft references a different baseline")
    require(isinstance(fixtures, list) and len(fixtures) >= 6 and len(fixtures) == len(set(fixtures)),
            "Immutable fixture denominator is missing or duplicated")
    require(TARGETS in fixtures,
            "Acceptance thresholds must be immutable inputs")
    targets = json_value(repo.blob(TARGETS))
    require(isinstance(targets.get("fixtures"), dict)
            and all(type(targets["fixtures"].get(name)) is int and targets["fixtures"][name] == value
                    for name, value in COPY_TARGETS.items()),
            "Committed copy thresholds/block configuration differ from the unchanged contract")
    packet = initial_packet(repo)
    names = sorted(repo.entries)
    repo.load_blobs(names)
    all_facts = [repo.fact(name) for name in names]
    sources = committed_source_facts(repo)
    require(sources and "CMakeLists.txt" in repo.entries, "Committed product/build source inventory is empty")
    digest = source_digest(sources)
    reader = EvidenceReader(repo.root, arguments.output)
    environment_bytes = arguments.environment.read_bytes()
    environment = environment_record(environment_bytes, reader)
    executions = evidence_records(arguments.evidence_spec.read_bytes(), reader, repo.commit, digest, sha(environment_bytes))
    external = set(reader.artifacts)
    for path in (arguments.environment, arguments.evidence_spec, arguments.output):
        if path.is_relative_to(repo.root):
            external.add(path.relative_to(repo.root).as_posix())
    frozen_inputs = set(INITIAL) | set(paths) | set(fixtures) | {PROTECTED}
    working = working_tree_check(repo, all_facts, arguments.exclude_user_instructions_diff, external, frozen_inputs)
    require(repo.git("rev-parse", "HEAD").decode().strip() == repo.commit, "Source commit changed during preflight")
    return {
        "schema": "qcae.c4-baseline-freeze/1", "mechanism_frozen": True,
        "baseline_commit": repo.commit, "baseline_git_tree_oid": repo.tree_oid,
        "source_tree_sha256": digest, "source_tree_hash_algorithm":
            "SHA256(b'QCAE-C4-SOURCE-SHA256-v1\\0' + UTF8-path-sorted records: uint64be(path bytes) + path + mode6 ASCII + raw32 SHA256(blob))",
        "source_tree_roots": list(SOURCE_ROOTS), "committed_source_files": sources,
        "manifest_relationship": "external/subsequent manifest references the source baseline commit; never included in its own hash",
        "initial_context": packet, "protected_definition": repo.fact(PROTECTED),
        "protected_files": [repo.fact(safe_name(name)) for name in paths],
        "protected_file_count": len(paths), "generators": [repo.fact(name) for name in GENERATORS],
        "generator_count": 4, "generator_rule": "the four generators are a subset of the actual committed protected inventory, whose original minimum is 38 files",
        "immutable_fixtures": [repo.fact(safe_name(name)) for name in fixtures],
        "unchanged_copy_targets": COPY_TARGETS,
        "environment_manifest": {"sha256": sha(environment_bytes), "environment": environment},
        "execution_spec_sha256": file_sha(arguments.evidence_spec), **executions,
        "verified_evidence_artifacts": list(reader.artifacts.values()), "working_tree": working,
        "complete_sk12_passed": False, "complete_c3_passed": False,
        "whole_pipeline_owned_copy_coverage": "unknown", "sk13_passed": False,
        "extensions_executed_by_tool": 0, "p0_passed": False,
        "validation_scope": "Git/blob/evidence byte integrity and provided executed-record conditions; independent review still owns execution provenance",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--environment", type=Path, required=True)
    parser.add_argument("--evidence-spec", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exclude-user-instructions-diff", action="store_true")
    arguments = parser.parse_args()
    if arguments.output.exists() or arguments.output.is_symlink():
        print("Refusing to overwrite an existing manifest or symlink", file=sys.stderr)
        return 2
    arguments.output = arguments.output.resolve()
    arguments.environment = arguments.environment.resolve()
    arguments.evidence_spec = arguments.evidence_spec.resolve()
    report = {"schema": "qcae.c4-baseline-freeze/1", "mechanism_frozen": False,
              "requested_source_commit": arguments.source_commit if COMMIT_RE.fullmatch(arguments.source_commit) else "invalid",
              "complete_sk12_passed": False,
              "complete_c3_passed": False, "whole_pipeline_owned_copy_coverage": "unknown"}
    try:
        require(not any(SENSITIVE_NAME.fullmatch(part) for path in
                        (arguments.environment, arguments.evidence_spec) for part in path.parts),
                "Credential/configuration paths are outside the freeze reader scope")
        report = freeze(arguments)
    except (FreezeError, ValueError, KeyError, TypeError, OSError, UnicodeError, subprocess.SubprocessError):
        # No Git diagnostics, file contents, command args or input JSON values leak.
        error = sys.exc_info()[1]
        report["blocked_reason"] = str(error) if isinstance(error, FreezeError) else "Malformed/missing source or evidence prerequisite"
    with arguments.output.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({"mechanism_frozen": report["mechanism_frozen"], "output": str(arguments.output),
                      "complete_sk12_passed": False, "complete_c3_passed": False}))
    return 0 if report["mechanism_frozen"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
