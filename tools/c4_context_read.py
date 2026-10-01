#!/usr/bin/env python3
"""Deliver source text while recording actual UTF-8 context bytes for C4 runs."""

import argparse
import hashlib
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--phase", choices=("initial", "subsequent"), required=True)
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    # Read everything first: an invalid path or non-text file delivers no partial packet.
    texts = []
    for name in args.paths:
        path = (root / name).resolve(strict=True)
        relative = path.relative_to(root).as_posix()
        content = path.read_bytes()
        value = content.decode("utf-8")
        texts.append((relative, content, value))
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open("a", encoding="utf-8") as log:
        for name, content, value in texts:
            heading = f"--- {name} ({len(content)} UTF-8 bytes) ---\n"
            trailing = "" if value.endswith("\n") else "\n"
            log.write(json.dumps({
                "phase": args.phase,
                "path": name,
                "utf8_bytes": len(content),
                "delivered_utf8_bytes": len(heading.encode("utf-8")) + len(content) + len(trailing),
                "sha256": hashlib.sha256(content).hexdigest(),
                "delivered_complete_file": True,
            }, sort_keys=True) + "\n")
            sys.stdout.write(heading)
            sys.stdout.write(value)
            sys.stdout.write(trailing)


if __name__ == "__main__":
    main()
