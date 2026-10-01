#!/usr/bin/env python3
"""Build a separate HarfBuzz tranche with enumerated owned glyph stores.

The previously measured Qt/HarfBuzz prefixes remain immutable. Statements are
wrapped once and their lvalue sizeof operands are unevaluated. This adds real
typed-store facts; it deliberately retains indirect/cache/native coverage gaps.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import prepare_c3_harfbuzz_sdk_ledger as first
from prepare_c3_qt_sdk_ledger import code_positions, replace_once

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
THIRD = BASE / "tranche3"
FILES = ("src/hb-buffer.hh", "src/hb-buffer.cc", "src/hb-ot-shape.cc",
         "src/hb-ot-shape-normalize.cc", "src/hb-ot-shape-fallback.cc")
MACRO = r'''
// sizeof never evaluates an index, getter or increment a second time. A
// chained assignment reports every enumerated destination's typed store.
#define QCAE_HB_SDK_TYPED_WRITE(site, bytes, ...) \
  do { __VA_ARGS__; qcae_hb_sdk_observer_emit(site, 2, std::uint64_t(bytes)); } while (false)
'''


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def typed_stores(text: str, relative: str, sites: list[dict]) -> str:
    index = r"\[[^\]\n]+\]"
    field = r"\.[A-Za-z_][A-Za-z_0-9]*(?:\(\))?(?:" + index + r")?"
    array = r"(?:(?:buffer|c->buffer)->)?(?:info|pos|out_info)" + index + "(?:" + field + ")*"
    pointer = r"(?:\*pinfo|pinfo->[A-Za-z_][A-Za-z_0-9]*)"
    lhs = "(?:" + array + "|" + pointer + ")"
    assignment = r"(?:=(?!=)|\|=|&=|\+=|-=)"
    statement = re.compile(r"(?m)^[ \t]*(?P<statement>(?P<lhs>" + lhs + r")\s*" +
                           assignment + r"[^;{}]+;)")
    positions = code_positions(text)
    matches = [m for m in statement.finditer(text) if positions[m.start("statement")]]
    chained = re.compile(r"(?P<lhs>" + lhs + r")\s*" + assignment)
    for ordinal, match in reversed(list(enumerate(matches, 1))):
        original = match.group("statement").removesuffix(";")
        destinations = [m.group("lhs") for m in chained.finditer(original)]
        bound = " + ".join("sizeof(" + dest + ")" for dest in destinations)
        site = f"harfbuzz/{relative}:owned_glyph_store:{ordinal}"
        replacement = f'QCAE_HB_SDK_TYPED_WRITE("{site}", {bound}, {original});'
        text = text[:match.start("statement")] + replacement + text[match.end("statement"):]
        sites.append({"site": site, "kind": "actual_owned_typed_store_upper_bound",
                      "proof": "original statement once; unevaluated sizeof of each chained destination",
                      "original_statement": original, "destinations": destinations})
    return text


def prepare() -> dict:
    THIRD.mkdir(exist_ok=True)
    (THIRD / "downloads").mkdir(exist_ok=True)
    archive = BASE / "downloads" / f"harfbuzz-{first.VERSION}.tar.xz"
    target = THIRD / "downloads" / archive.name
    if not target.exists():
        target.symlink_to(archive)
    first.BASE = THIRD
    manifest = first.prepare()
    source = THIRD / f"harfbuzz-{first.VERSION}"
    bridge = source / "qcae-hb-sdk-observer.hpp"
    bridge.write_text(bridge.read_text() + MACRO)
    sites = manifest["sites"]
    include = '#include "' + str(bridge) + '"\n'
    sources = {item["path"]: item for item in manifest["sources"]}
    for relative in FILES:
        path = source / relative
        before = path.read_text()
        after = typed_stores(before, relative, sites)
        if relative == "src/hb-ot-shape.cc":
            after = replace_once(after, "hb_ot_shape_internal (hb_ot_shape_context_t *c)\n{",
                                 'hb_ot_shape_internal (hb_ot_shape_context_t *c)\n{\n'
                                 '  QcaeHbSdkScope qcaeHbSdkScope("harfbuzz/hb_ot_shape_internal");')
            sites.append({"site": "harfbuzz/hb_ot_shape_internal", "kind": "actual_entry"})
        if after == before:
            raise ValueError("No reviewed typed store found in " + relative)
        if not after.startswith(include):
            after = include + after
        path.write_text(after)
        if relative not in sources:
            sources[relative] = {"path": relative, "original_sha256": digest(before.encode())}
        sources[relative]["instrumented_body_sha256"] = digest(after.removeprefix(include).encode())
    manifest["sources"] = sorted(sources.values(), key=lambda row: row["path"])
    manifest["sites"] = sorted(sites, key=lambda row: row["site"])
    manifest["tranche"] = 3
    manifest["previous_prefix_modified"] = False
    manifest["bridge_header_sha256"] = digest(bridge.read_bytes())
    manifest["typed_store_scope"] = list(FILES)
    manifest["remaining"] = [
        "unlisted glyph aliases, field mutators, OT layout/shaper/cache/generated stores",
        "nontrivial container constructor/assignment deep copies and indirect relocation inventory",
        "native CoreText/Graphite/FreeType model-derived input/output ownership boundaries",
    ]
    marker = source / "src/hb-common.cc"
    text = marker.read_text()
    declaration = "#define QCAE_HB_SDK_MANIFEST "
    start = text.index(declaration)
    end = text.index("\n", start)
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    marker.write_text(text[:start] + declaration + json.dumps(compact) + text[end:])
    (THIRD / "harfbuzz-observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configure", action="store_true")
    args = parser.parse_args()
    manifest = prepare()
    print("Prepared separate HB typed tranche:", len(manifest["sites"]), "sites; coverage remains unknown")
    if args.configure:
        first.configure()
        print("Configured independent Release build:", THIRD / "hb-build")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
