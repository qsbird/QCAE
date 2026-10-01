#!/usr/bin/env python3
"""Prepare a separate fourth HB tranche for the frozen 23 alias-store sites.

The original assignment executes once. Its destination sizeof is unevaluated;
the callback runs after the original write and observes only that typed store.
This does not remove the broader shaping/cache/backend coverage placeholder.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import prepare_c3_harfbuzz_typed_tranche as previous
from prepare_c3_qt_sdk_ledger import code_positions

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
FOURTH = BASE / "tranche4"
FROZEN = BASE / "tranche3"
INVENTORY = ROOT / "docs/engineering/c3-closure-evidence/package/font-copy-remaining-source-inventory.json"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def wrap_stores(text: str, relative: str, entries: list[dict], sites: list[dict]) -> str:
    expected = Counter(entry["statement"] for entry in entries)
    positions = code_positions(text)
    edits = []
    for prefix, count in expected.items():
        matches = [m.start() for m in re.finditer(re.escape(prefix), text) if positions[m.start()]]
        if len(matches) != count:
            raise ValueError(f"Reviewed alias occurrence mismatch: {relative}: {prefix}: {len(matches)} != {count}")
        for occurrence, start in enumerate(matches, 1):
            end = start
            while end < len(text) and not (text[end] == ";" and positions[end]):
                end += 1
            if end == len(text):
                raise ValueError("Reviewed alias assignment has no statement terminator")
            statement = text[start:end]
            lhs_match = re.match(r"(.+?)\s*(?:&=|\|=|=(?!=))", statement)
            if not lhs_match:
                raise ValueError("Reviewed alias has no supported typed assignment")
            destination = lhs_match.group(1).strip()
            site = f"harfbuzz/{relative}:owned_alias_store:{len(edits) + 1}"
            replacement = (f'QCAE_HB_SDK_TYPED_WRITE("{site}", '
                           f"sizeof({destination}), {statement});")
            edits.append((start, end + 1, replacement))
            sites.append({
                "site": site,
                "kind": "actual_owned_alias_typed_store_upper_bound",
                "original_statement": statement + ";",
                "destination": destination,
                "destination_bytes": "sizeof exact lvalue type, unevaluated",
                "source_prefix_occurrence": occurrence,
                "proof": "original statement once; callback after write; no getter/index evaluated for sizeof",
                "origin": "frozen 23-entry alias/mutator inventory",
            })
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def prepare() -> dict:
    inventory = json.loads(INVENTORY.read_text())
    entries = inventory["entries"]
    if len(entries) != 23 or inventory["coverage_complete"]:
        raise ValueError("The frozen finite inventory has changed")
    grouped: dict[str, list[dict]] = {}
    frozen_hashes = {}
    for entry in entries:
        relative = entry["path"]
        frozen = FROZEN / f"harfbuzz-{previous.first.VERSION}" / relative
        actual_hash = digest(frozen.read_bytes())
        if actual_hash != entry["source_sha256"]:
            raise ValueError("Frozen tranche3 source does not match the reviewed inventory: " + relative)
        grouped.setdefault(relative, []).append(entry)
        frozen_hashes[relative] = actual_hash
    previous.THIRD = FOURTH
    manifest = previous.prepare()
    source = FOURTH / f"harfbuzz-{previous.first.VERSION}"
    bridge = source / "qcae-hb-sdk-observer.hpp"
    include = '#include "' + str(bridge) + '"\n'
    sources = {item["path"]: item for item in manifest["sources"]}
    sites = manifest["sites"]
    count_before = len(sites)
    for relative, records in grouped.items():
        path = source / relative
        before = path.read_text()
        after = wrap_stores(before, relative, records, sites)
        if not after.startswith(include):
            after = include + after
        path.write_text(after)
        if relative not in sources:
            sources[relative] = {"path": relative, "original_sha256": digest(before.encode())}
        sources[relative]["instrumented_body_sha256"] = digest(after.removeprefix(include).encode())
    if len(sites) - count_before != 23:
        raise ValueError("The finite alias tranche did not observe all 23 reviewed assignments")
    manifest.update({
        "tranche": 4,
        "previous_prefix_modified": False,
        "sources": sorted(sources.values(), key=lambda row: row["path"]),
        "sites": sorted(sites, key=lambda row: row["site"]),
        "alias_inventory_sha256": digest(INVENTORY.read_bytes()),
        "frozen_tranche3_source_sha256": frozen_hashes,
        "alias_store_scope": sorted(grouped),
        "alias_store_count": 23,
        "coverage_complete": False,
        "remaining": [
            "unlisted OT/generated glyph alias assignments, increments and indirect stores",
            "scratch/cache vector initialized writes, nontrivial constructor/assignment copies and relocation inventory",
            "backend model-derived CoreText/Graphite/FreeType input/output ownership boundaries",
            "shape-entry placeholder retained; finite typed sites are not complete shaper ownership proof",
        ],
    })
    marker = source / "src/hb-common.cc"
    text = marker.read_text()
    declaration = "#define QCAE_HB_SDK_MANIFEST "
    start = text.index(declaration)
    end = text.index("\n", start)
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    marker.write_text(text[:start] + declaration + json.dumps(compact) + text[end:])
    (FOURTH / "harfbuzz-observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for relative, expected in frozen_hashes.items():
        if digest((FROZEN / f"harfbuzz-{previous.first.VERSION}" / relative).read_bytes()) != expected:
            raise ValueError("An earlier source prefix changed")
    return manifest


def bind_qt() -> None:
    original = BASE / "tranche3b/qt-prefix"
    target = FOURTH / "qt-prefix"
    if target.exists():
        raise ValueError("Refusing to overwrite a prepared Qt/HB SDK prefix")
    hb = FOURTH / "hb-prefix/lib/libharfbuzz.dylib"
    if not hb.is_file():
        raise ValueError("The independent fourth HB library has not been built")
    shutil.copytree(original, target, symlinks=True)
    metadata = []
    for suffix in (".cmake", ".pc", ".prl", ".pri"):
        for path in target.rglob("*" + suffix):
            text = path.read_text()
            changed = text.replace(str(original), str(target))
            if changed != text:
                path.write_text(changed)
                metadata.append(str(path.relative_to(target)))
    shutil.copy2(hb, target / "lib/libharfbuzz.dylib")
    images = []
    for path in sorted((target / "lib").glob("Qt*.framework/Versions/A/Qt*")):
        if path.is_file() and not path.suffix:
            frozen = original / path.relative_to(target)
            before, after = digest(frozen.read_bytes()), digest(path.read_bytes())
            if before != after:
                raise ValueError("A copied Qt framework payload changed")
            images.append({"path": str(path), "sha256": after,
                           "matches_frozen_tranche3b": True})
    report = {"schema": 1, "original_prefix": str(original), "isolated_prefix": str(target),
              "qt_framework_payloads": images, "retargeted_metadata": metadata,
              "qt_manifest": str(FROZEN / "qt-observer-manifest.json"),
              "hb_manifest": str(FOURTH / "harfbuzz-observer-manifest.json"),
              "hb_library": str(target / "lib/libharfbuzz.dylib"),
              "hb_sha256": digest(hb.read_bytes()), "earlier_prefixes_modified": False,
              "coverage_complete": False, "actual_qapplication_same_run": "pending root"}
    (FOURTH / "qt-hb-binding.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Prepared independent Qt/HB4 binding;", len(images), "Qt framework payloads unchanged")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configure", action="store_true")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--bind-qt", action="store_true")
    args = parser.parse_args()
    if args.bind_qt and not (args.configure or args.build):
        bind_qt()
        return 0
    manifest = prepare()
    print("Prepared independent HB alias tranche:", len(manifest["sites"]),
          "sites; all 23 finite aliases covered; full coverage remains unknown")
    if args.configure or args.build:
        previous.first.configure()
    if args.build:
        subprocess.run(["cmake", "--build", str(FOURTH / "hb-build"), "--parallel", "2",
                        "--target", "harfbuzz"], check=True)
        subprocess.run(["cmake", "--install", str(FOURTH / "hb-build")], check=True)
    if args.bind_qt:
        bind_qt()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
