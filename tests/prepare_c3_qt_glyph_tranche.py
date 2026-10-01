#!/usr/bin/env python3
"""Relink an isolated Qt glyph tranche from frozen Release objects.

Only qglobal's manifest and two enumerated Gui source files are recompiled.
Every reused object, command and dependent header is hashed. The preceding
prefixes/builds stay read-only. No whole-font coverage claim is made.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

import prepare_c3_qt_sdk_ledger as first

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
SECOND = BASE / "tranche2"
THIRD = BASE / "tranche3"
SOURCE = THIRD / "qt-glyph-sources"
PREFIX = THIRD / "qt-prefix"
COMPILED = ("src/corelib/global/qglobal.cpp", "src/gui/text/coretext/qfontengine_coretext.mm",
            "src/gui/text/qharfbuzzng.cpp")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def observe_stores(text: str, relative: str, sites: list[dict]) -> str:
    if relative.endswith(".mm"):
        lhs = r"(?:cgGlyphs\[[^\]\n]+\]|cgPositions\[[^\]\n]+\]\.[xy])"
    else:
        lhs = r"(?:\*(?:glyph|x|y)|(?:metrics|extents)->[A-Za-z_][A-Za-z_0-9]*|g\.numGlyphs)"
    assignment = r"=(?!=)"
    pattern = re.compile(r"(?m)^[ \t]*(?P<statement>(?P<lhs>" + lhs + r")\s*" + assignment + r"[^;{}]+;)")
    positions = first.code_positions(text)
    matches = [m for m in pattern.finditer(text) if positions[m.start("statement")]]
    chained = re.compile(r"(?P<lhs>" + lhs + r")\s*" + assignment)
    for ordinal, match in reversed(list(enumerate(matches, 1))):
        statement = match.group("statement").removesuffix(";")
        destinations = [m.group("lhs") for m in chained.finditer(statement)]
        bound = " + ".join("sizeof(" + value + ")" for value in destinations)
        site = f"{relative}:owned_glyph_adapter_store:{ordinal}"
        replacement = f'QCAE_QT_SDK_TYPED_WRITE("{site}", {bound}, {statement});'
        text = text[:match.start("statement")] + replacement + text[match.end("statement"):]
        sites.append({"site": site, "kind": "actual_owned_field_write_upper_bound",
                      "proof": "original assignment once; each chained typed destination counted",
                      "original_statement": statement, "destinations": destinations})
    return text


def prepare() -> dict:
    SOURCE.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((SECOND / "observer-manifest.json").read_text())
    sites = manifest["sites"]
    source = SECOND / f"qtbase-everywhere-src-{first.VERSION}"
    bridge = source / "qcae-sdk-observer.hpp"
    sources = {row["path"]: row for row in manifest["sources"]}
    for relative in COMPILED[1:]:
        before = (source / relative).read_text()
        after = observe_stores(before, relative, sites)
        if relative.endswith("qharfbuzzng.cpp"):
            first.ENTRIES[relative] = (
                "_hb_qt_font_get_nominal_glyph", "_hb_qt_font_get_variation_glyph",
                "_hb_qt_font_get_glyph_h_advance", "_hb_qt_font_get_glyph_h_kerning",
                "_hb_qt_font_get_glyph_extents", "_hb_qt_font_get_glyph_contour_point",
            )
            after = first.observe_entries(after, relative, sites)
            after = '#include "' + str(bridge) + '"\n' + after
        if before == after:
            raise ValueError("No reviewed adapter store found: " + relative)
        out = SOURCE / relative
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(after)
        if relative not in sources:
            sources[relative] = {"path": relative, "original_sha256": digest(before.encode())}
        body = after.removeprefix('#include "' + str(bridge) + '"\n')
        sources[relative]["instrumented_body_sha256"] = digest(body.encode())
    manifest["sources"] = sorted(sources.values(), key=lambda row: row["path"])
    manifest["sites"] = sorted(sites, key=lambda row: row["site"])
    manifest["tranche"] = 3
    manifest["scope"] += "; enumerated CoreText input/compaction and Qt-to-HB callback destination stores"
    manifest["previous_prefix_modified"] = False
    manifest["object_reuse"] = {
        "base_build": str(SECOND / "build"), "base_manifest_sha256": digest((SECOND / "observer-manifest.json").read_bytes()),
        "recompiled_sources": list(COMPILED), "compiler_abi_header_feature_flags_unchanged": True,
        "note": "Source/output argv change. Original source-directory quote lookup is restored explicitly with -iquote. ABI/header/feature flags remain frozen; exact argv/hashes are in object-reuse.json.",
    }
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    text = (source / COMPILED[0]).read_text()
    declaration = "#define QCAE_QT_SDK_MANIFEST "
    start = text.index(declaration)
    end = text.index("\n", start)
    text = text[:start] + declaration + json.dumps(compact) + text[end:]
    out = SOURCE / COMPILED[0]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    (THIRD / "qt-observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def compiler_part(command: str) -> list[str]:
    parts = shlex.split(command)
    start = next(i for i, part in enumerate(parts) if part == "/usr/bin/c++")
    end = parts.index("&&", start) if "&&" in parts[start:] else len(parts)
    return parts[start:end]


def command_inventory() -> tuple[dict, dict]:
    build = SECOND / "build"
    process = subprocess.run(["ninja", "-C", str(build), "-t", "commands", "Core", "Gui"],
                             capture_output=True, text=True, check=True)
    lines = process.stdout.splitlines()
    compile_commands = {}
    for relative in COMPILED:
        matches = [line for line in lines if ("/" + relative) in line and " -c " in line]
        if len(matches) != 1:
            raise ValueError("Expected one original compiler command for " + relative)
        compile_commands[relative] = compiler_part(matches[0])
    link_commands = {}
    for module in ("Core", "Gui"):
        matches = [compiler_part(line) for line in lines if " -dynamiclib " in line and " -o " in line]
        matches = [args for args in matches if args[args.index("-o") + 1] == f"lib/Qt{module}.framework/Versions/A/Qt{module}"]
        if len(matches) != 1:
            raise ValueError("Expected one original linker command for " + module)
        link_commands[module] = matches[0]
    return compile_commands, link_commands


def build() -> None:
    compile_commands, link_commands = command_inventory()
    build_base = SECOND / "build"
    (THIRD / "qt-objects").mkdir(exist_ok=True)
    compiled_objects = {}
    compile_jobs = []
    for relative, original in compile_commands.items():
        args = list(original)
        original_object = args[args.index("-o") + 1]
        output = THIRD / "qt-objects" / (Path(relative).name + ".o")
        args[args.index("-o") + 1] = str(output)
        args[args.index("-c") + 1] = str(SOURCE / relative)
        # Moving this source removes the compiler's original implicit quote
        # lookup directory. Restore that exact directory without changing
        # angle-bracket/system/feature include precedence.
        original_source = SECOND / f"qtbase-everywhere-src-{first.VERSION}" / relative
        args[1:1] = ["-iquote", str(original_source.parent)]
        if "-MF" in args:
            args[args.index("-MF") + 1] = str(output) + ".d"
        compiled_objects[original_object] = str(output)
        compile_jobs.append((relative, args))

    def compile_one(job):
        relative, args = job
        log = THIRD / ("qt-compile-" + Path(relative).name + ".log")
        with log.open("w") as output:
            subprocess.run(args, cwd=build_base, stdout=output, stderr=subprocess.STDOUT, check=True)

    with ThreadPoolExecutor(max_workers=2) as workers:
        list(workers.map(compile_one, compile_jobs))
    # Copy, never hardlink: install_name_tool must not mutate an earlier prefix.
    shutil.copytree(SECOND / "prefix", PREFIX, symlinks=True, dirs_exist_ok=True)
    report = {"schema": 1, "compiler_commands": compile_commands, "actual_compile_jobs": compile_jobs,
              "original_linker_commands": link_commands, "reused_objects": [], "headers": [],
              "original_prefix_modified": False, "whole_font_copy_coverage": "unknown"}
    compiler = Path("/usr/bin/c++")
    sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/SDKSettings.json")
    report["compiler"] = {
        "path": str(compiler), "sha256": digest(compiler.read_bytes()),
        "version": subprocess.run([str(compiler), "--version"], capture_output=True,
                                  text=True, check=True).stdout.strip(),
    }
    report["sdk_settings"] = {"path": str(sdk.resolve()), "sha256": digest(sdk.read_bytes())}
    features = SECOND / "feature-comparison.json"
    report["prior_features_report"] = {
        "path": str(features), "sha256": digest(features.read_bytes()),
        "unchanged_for_reused_and_recompiled_objects": True,
    }
    report["source_quote_lookup_equivalence"] = (
        "-iquote original-source-directory restores the moved source's implicit quote-header search; "
        "feature/ABI/system-header flags remain unchanged")
    report["same_run_gui_output_equivalence"] = "pending root runtime"
    reused = set()
    for module, original in link_commands.items():
        args = list(original)
        output = PREFIX / f"lib/Qt{module}.framework/Versions/A/Qt{module}"
        args[args.index("-o") + 1] = str(output)
        for index, argument in enumerate(args):
            if argument in compiled_objects:
                args[index] = compiled_objects[argument]
            elif argument.endswith((".o", ".a")):
                path = Path(argument)
                path = path if path.is_absolute() else build_base / path
                reused.add(path.resolve())
        report.setdefault("actual_linker_commands", {})[module] = args
        with (THIRD / ("qt-link-" + module + ".log")).open("w") as log:
            subprocess.run(args, cwd=build_base, stdout=log, stderr=subprocess.STDOUT, check=True)
        details = subprocess.run(["otool", "-l", str(output)], capture_output=True, text=True, check=True).stdout
        rpaths = re.findall(r"cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset", details)
        for rpath in rpaths:
            if rpath.startswith(str(BASE)):
                subprocess.run(["install_name_tool", "-delete_rpath", rpath, str(output)], check=True)
        if "@loader_path/../../.." not in rpaths:
            subprocess.run(["install_name_tool", "-add_rpath", "@loader_path/../../..", str(output)], check=True)
    for path in sorted(reused):
        report["reused_objects"].append({"path": str(path), "sha256": digest(path.read_bytes())})
    # Header hashing binds every frozen header of the two Release modules and
    # the unchanged observer ABI, not only the recompiled translation units.
    dependencies = subprocess.run(["ninja", "-C", str(build_base), "-t", "deps"],
                                  capture_output=True, text=True, check=True).stdout
    headers = set()
    for line in dependencies.splitlines():
        if line.startswith("    "):
            candidate = Path(line.strip())
            candidate = candidate if candidate.is_absolute() else build_base / candidate
            if candidate.suffix in (".h", ".hpp", ".hh", ".hxx") and candidate.is_file():
                headers.add(candidate.resolve())
    for path in sorted(headers):
        report["headers"].append({"path": str(path), "sha256": digest(path.read_bytes())})
    for suffix in (".cmake", ".pc", ".prl", ".pri"):
        for path in PREFIX.rglob("*" + suffix):
            text = path.read_text()
            replacement = text.replace(str(SECOND / "prefix"), str(PREFIX))
            if replacement != text:
                path.write_text(replacement)
    shutil.copy2(THIRD / "hb-prefix/lib/libharfbuzz.dylib", PREFIX / "lib/libharfbuzz.dylib")
    (THIRD / "object-reuse.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Recompiled three source files and linked independent Core/Gui; reused", len(reused),
          "hashed objects and", len(headers), "frozen headers")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()
    manifest = prepare()
    print("Prepared separate Qt glyph tranche:", len(manifest["sites"]), "sites; coverage remains unknown")
    if args.build:
        build()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
