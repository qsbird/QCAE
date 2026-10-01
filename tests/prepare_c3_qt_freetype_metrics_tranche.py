#!/usr/bin/env python3
"""Build an isolated diagnostic Qt SDK with native FT bitmap line metrics.

Only the FreeType height initializer and QtCore manifest are recompiled. The
scalable path, public ABI, shaping, glyph rasterization and fixed case inputs
remain unchanged. All earlier SDK objects, sources and prefixes are read-only.
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
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
SECOND = BASE / "tranche2"
THIRD = BASE / "tranche3"
FOURTH = BASE / "tranche4"
FIFTH = BASE / "tranche5"
SOURCE = FIFTH / "qt-metrics-sources"
PREFIX = FIFTH / "qt-prefix"
RELATIVE = "src/gui/text/freetype/qfontengine_ft.cpp"
ARCHIVE_SHA = "d9594a31228aa23ad6b531719a29b45f0f3989fe6c136d45767ea179f233c1ac"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def compiler_part(command: str) -> list[str]:
    parts = shlex.split(command)
    start = parts.index("/usr/bin/c++")
    end = parts.index("&&", start) if "&&" in parts[start:] else len(parts)
    return parts[start:end]


def prepare() -> dict:
    SOURCE.mkdir(parents=True, exist_ok=True)
    archive = BASE / "downloads/qtbase-everywhere-src-6.11.1.tar.xz"
    if digest(archive.read_bytes()) != ARCHIVE_SHA:
        raise ValueError("Official Qt 6.11.1 source archive digest changed")
    with tarfile.open(archive) as compressed:
        member = compressed.extractfile("qtbase-everywhere-src-6.11.1/" + RELATIVE)
        if member is None:
            raise ValueError("The official FT implementation is missing")
        original = member.read()
    frozen = SECOND / "qtbase-everywhere-src-6.11.1" / RELATIVE
    if original != frozen.read_bytes():
        raise ValueError("The FT source was not identical to the official release")
    old = """    QFontEngine::initializeHeightMetrics();

    if (scalableBitmapScaleFactor != 1) {"""
    replacement = """    // Diagnostic SDK only: FT bitmap metrics already use selected-strike
    // pixels. emSquareSize() is ppem for these faces, not SFNT design UPEM;
    // applying HHEA/OS2 design metrics here mixes those different units.
    if (FT_IS_SCALABLE(freetype->face))
        QFontEngine::initializeHeightMetrics();
    else
        m_heightMetricsQueried = true;

    if (scalableBitmapScaleFactor != 1) {"""
    text = original.decode()
    if text.count(old) != 1:
        raise ValueError("The bounded original height initializer changed")
    patched = text.replace(old, replacement)
    path = SOURCE / RELATIVE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(patched)
    manifest = json.loads((THIRD / "qt-observer-manifest.json").read_text())
    manifest.update({
        "tranche": 5,
        "previous_prefix_modified": False,
        "diagnostic_semantic_repair": {
            "source": RELATIVE,
            "original_source_sha256": digest(original),
            "patched_source_sha256": digest(patched.encode()),
            "original": old,
            "replacement": replacement,
            "scope": "non-scalable FT bitmap height metrics only; scalable path and emSquareSize unchanged",
            "purpose": "preserve native selected-strike pixel metrics before existing bitmap scale",
            "production_default_backend_changed": False,
            "observer_sites_added": 0,
        },
        "coverage_complete": False,
        "sdk_prefix": str(PREFIX),
        "harfbuzz_manifest": str(FOURTH / "harfbuzz-observer-manifest.json"),
    })
    marker = (THIRD / "qt-glyph-sources/src/corelib/global/qglobal.cpp").read_text()
    declaration = "#define QCAE_QT_SDK_MANIFEST "
    start = marker.index(declaration)
    end = marker.index("\n", start)
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    output = SOURCE / "src/corelib/global/qglobal.cpp"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(marker[:start] + declaration + json.dumps(compact) + marker[end:])
    (FIFTH / "qt-observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def build(manifest: dict) -> None:
    if PREFIX.exists():
        raise ValueError("Refusing to overwrite a prepared diagnostic SDK prefix")
    inventory = json.loads((THIRD / "object-reuse.json").read_text())
    original_prefix = FOURTH / "qt-prefix"
    shutil.copytree(original_prefix, PREFIX, symlinks=True)
    for suffix in (".cmake", ".pc", ".prl", ".pri"):
        for path in PREFIX.rglob("*" + suffix):
            contents = path.read_text()
            changed = contents.replace(str(original_prefix), str(PREFIX))
            if changed != contents:
                path.write_text(changed)
    base_build = SECOND / "build"
    commands = subprocess.run(["ninja", "-C", str(base_build), "-t", "commands", "Gui"],
                              capture_output=True, text=True, check=True).stdout.splitlines()
    ft_commands = [row for row in commands if ("/" + RELATIVE) in row and " -c " in row]
    if len(ft_commands) != 1:
        raise ValueError("Expected one original FT compiler command")
    compile_commands = {
        "src/corelib/global/qglobal.cpp": inventory["compiler_commands"]["src/corelib/global/qglobal.cpp"],
        RELATIVE: compiler_part(ft_commands[0]),
    }
    objects = FIFTH / "qt-objects"
    objects.mkdir(exist_ok=True)
    replacements = {}
    jobs = []
    report = {"manifest_sha256": digest((FIFTH / "qt-observer-manifest.json").read_bytes()),
              "actual_compile_commands": {}, "actual_link_commands": {},
              "compiler": inventory["compiler"], "sdk_settings": inventory["sdk_settings"],
              "prior_header_inventory": str(THIRD / "object-reuse.json"),
              "prior_header_inventory_sha256": digest((THIRD / "object-reuse.json").read_bytes()),
              "abi_feature_header_flags_unchanged": True, "earlier_prefix_modified": False,
              "original_source_archive_sha256": ARCHIVE_SHA, "coverage_complete": False}
    for relative, original in compile_commands.items():
        args = list(original)
        old_object = args[args.index("-o") + 1]
        output = objects / (Path(relative).name + ".o")
        args[args.index("-o") + 1] = str(output)
        args[args.index("-c") + 1] = str(SOURCE / relative)
        args[1:1] = ["-iquote", str((SECOND / "qtbase-everywhere-src-6.11.1" / relative).parent)]
        if "-MF" in args:
            args[args.index("-MF") + 1] = str(output) + ".d"
        replacements[old_object] = str(output)
        report["actual_compile_commands"][relative] = args
        jobs.append((relative, args))
    def compile_one(job):
        relative, args = job
        with (FIFTH / ("compile-" + Path(relative).name + ".log")).open("w") as log:
            subprocess.run(args, cwd=base_build, stdout=log, stderr=subprocess.STDOUT, check=True)
    with ThreadPoolExecutor(max_workers=2) as workers:
        list(workers.map(compile_one, jobs))
    # The old third-tranche manifest object differs from the original base name.
    replacements[str(THIRD / "qt-objects/qglobal.cpp.o")] = str(objects / "qglobal.cpp.o")
    reused = set()
    for module, original in inventory["actual_linker_commands"].items():
        args = list(original)
        output = PREFIX / f"lib/Qt{module}.framework/Versions/A/Qt{module}"
        args[args.index("-o") + 1] = str(output)
        for index, argument in enumerate(args):
            if argument in replacements:
                args[index] = replacements[argument]
            elif argument.endswith((".o", ".a")):
                path = Path(argument)
                reused.add((path if path.is_absolute() else base_build / path).resolve())
            elif argument.endswith("libharfbuzz.dylib"):
                args[index] = str(PREFIX / "lib/libharfbuzz.dylib")
        report["actual_link_commands"][module] = args
        with (FIFTH / ("link-" + module + ".log")).open("w") as log:
            subprocess.run(args, cwd=base_build, stdout=log, stderr=subprocess.STDOUT, check=True)
        details = subprocess.check_output(["otool", "-l", str(output)], text=True)
        rpaths = re.findall(r"cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset", details)
        for rpath in rpaths:
            if rpath.startswith(str(BASE)) or rpath == "/opt/homebrew/lib":
                subprocess.run(["install_name_tool", "-delete_rpath", rpath, str(output)], check=True)
        if "@loader_path/../../.." not in rpaths:
            subprocess.run(["install_name_tool", "-add_rpath", "@loader_path/../../..", str(output)], check=True)
    report["reused_objects"] = [{"path": str(path), "sha256": digest(path.read_bytes())}
                                for path in sorted(reused)]
    report["recompiled_sources"] = [{"path": str(SOURCE / relative),
                                      "sha256": digest((SOURCE / relative).read_bytes())}
                                     for relative in compile_commands]
    report["sdk_libraries"] = []
    for path in sorted((PREFIX / "lib").glob("Qt*.framework/Versions/A/Qt*")):
        if path.is_file() and not path.suffix:
            original = original_prefix / path.relative_to(PREFIX)
            report["sdk_libraries"].append({"path": str(path), "sha256": digest(path.read_bytes()),
                                             "prior_sha256": digest(original.read_bytes()),
                                             "unchanged": digest(path.read_bytes()) == digest(original.read_bytes())})
    (FIFTH / "object-reuse.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Diagnostic metrics SDK linked;", len(reused), "frozen objects reused")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()
    manifest = prepare()
    print("Prepared Qt5 diagnostic bitmap metrics; observer sites:", len(manifest["sites"]))
    if args.build:
        build(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
