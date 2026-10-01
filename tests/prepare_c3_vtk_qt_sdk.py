#!/usr/bin/env python3
"""Bind an isolated VTK installation copy to the exact observed Qt prefix."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def command(*arguments):
    return subprocess.check_output([str(value) for value in arguments], text=True, stderr=subprocess.STDOUT)


def prepare(source, destination, qt):
    source, destination, qt = (Path(value).resolve() for value in (source, destination, qt))
    allowed = ROOT / "build-c3-qt-observed"
    if not destination.is_relative_to(allowed) or destination == allowed or destination.exists():
        raise ValueError("destination must be a new isolated directory below build-c3-qt-observed/")
    if destination.is_relative_to(source):
        raise ValueError("the copied installation cannot be inside its source")
    shutil.copytree(source, destination, symlinks=True)
    changes = []
    for binary in sorted((destination / "lib").glob("*.dylib")):
        if binary.is_symlink():
            continue
        original = source / binary.relative_to(destination)
        replacements = []
        for line in command("otool", "-L", binary).splitlines()[1:]:
            dependency = line.strip().split(" (compatibility version", 1)[0]
            if dependency.startswith("/") and "/Qt" in dependency and ".framework/" in dependency:
                relative = dependency.split("/lib/", 1)[1]
                replacement = qt / "lib" / relative
                if not replacement.is_file():
                    raise ValueError(f"observed Qt dependency is missing: {replacement}")
                command("install_name_tool", "-change", dependency, replacement, binary)
                replacements.append({"before": dependency, "after": str(replacement)})
        if replacements:
            command("codesign", "--force", "--sign", "-", "--timestamp=none", binary)
            changes.append({"binary": str(binary.relative_to(destination)), "replacements": replacements,
                            "original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                            "copy_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                            "actual_dependencies": command("otool", "-L", binary).splitlines()[1:]})
    if not changes:
        raise ValueError("no Qt-dependent VTK image was found; the claimed binding was not established")
    manifest = {"source": str(source), "copy": str(destination), "qt_prefix": str(qt),
                "original_installation_modified": False, "changes": changes,
                "complete_sdk_copy_coverage": False}
    (destination / "qcae-observed-qt-binding.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"copy": str(destination), "modified_images": len(changes)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    parser.add_argument("--destination", required=True)
    parser.add_argument("--qt-prefix", required=True)
    args = parser.parse_args()
    prepare(args.source, args.destination, args.qt_prefix)
