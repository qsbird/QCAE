#!/usr/bin/env python3
"""Require a consistent clang-format layout for handwritten C++ sources."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = ("modules", "profiles", "adapters", "ui", "apps", "tests")
SUFFIXES = {".cpp", ".hpp", ".h", ".cc"}


def formatter() -> str | None:
    configured = os.environ.get("QCAE_CLANG_FORMAT")
    if configured:
        return configured
    named = shutil.which("clang-format-21")
    if named:
        return named
    xcrun = shutil.which("xcrun")
    if xcrun:
        found = subprocess.run(
            [xcrun, "--find", "clang-format"], capture_output=True, text=True, check=False
        )
        if found.returncode == 0:
            return found.stdout.strip()
    return shutil.which("clang-format")


def main() -> int:
    if not (ROOT / ".clang-format").is_file():
        print("Missing repository .clang-format", file=sys.stderr)
        return 2
    executable = formatter()
    if not executable:
        print("clang-format 21 is required for the C++ readability gate", file=sys.stderr)
        return 2
    try:
        version = subprocess.run(
            [executable, "--version"], capture_output=True, text=True, check=False
        )
    except OSError as exc:
        print(f"Cannot run clang-format: {exc}", file=sys.stderr)
        return 2
    if version.returncode != 0 or "clang-format version 21." not in version.stdout:
        print(f"clang-format 21 required; found: {version.stdout.strip() or executable}", file=sys.stderr)
        return 2
    paths = sorted(
        path
        for folder in SOURCE_DIRS
        for path in (ROOT / folder).rglob("*")
        if path.is_file() and path.suffix in SUFFIXES
    )
    if not paths:
        print("No handwritten C++ sources found", file=sys.stderr)
        return 2
    result = subprocess.run([executable, "--dry-run", "--Werror", *map(str, paths)], check=False)
    if result.returncode == 0:
        print(f"PASS: {len(paths)} C++ files match clang-format 21")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
