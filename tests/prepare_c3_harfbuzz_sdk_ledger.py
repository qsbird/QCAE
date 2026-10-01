#!/usr/bin/env python3
"""Prepare the already installed HarfBuzz version as an isolated font observer.

This is a second SDK tranche, independent of the frozen first Qt prefix.
Explicit bulk calls and logical container growth are observed, with typed
store/deep-copy gaps retained for source/IR review. The original shaper list
must match before the library can be selected by an observed QCAE process.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

from prepare_c3_qt_sdk_ledger import BRIDGE_HEADER, BRIDGE_IMPLEMENTATION, code_positions, replace_once

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
VERSION = "14.4.0"
ARCHIVE_SHA = "2357ed966c6ced7bfa720b0640c0231065af01158fbea215093ffa15aed44371"


def digest(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def prepare() -> dict:
    archive = BASE / "downloads" / f"harfbuzz-{VERSION}.tar.xz"
    if digest(archive.read_bytes()) != ARCHIVE_SHA:
        raise ValueError("Official HarfBuzz release digest mismatch")
    source = BASE / f"harfbuzz-{VERSION}"
    if not source.exists():
        with tarfile.open(archive) as compressed:
            compressed.extractall(BASE, filter="data")
    originals = {}
    with tarfile.open(archive) as compressed:
        for member in compressed.getmembers():
            relative = member.name.removeprefix(f"harfbuzz-{VERSION}/")
            if relative.startswith("src/") and Path(relative).suffix in (".hh", ".cc") and member.isfile():
                contents = compressed.extractfile(member)
                if contents is not None:
                    originals[relative] = contents.read()

    header = BRIDGE_HEADER.replace("QcaeQtSdk", "QcaeHbSdk").replace("qcae_qt_sdk", "qcae_hb_sdk")
    header = header.replace("#include <QtCore/qtcoreexports.h>\n", "")
    header = header.replace("Q_CORE_EXPORT", '__attribute__((visibility("default")))')
    implementation = BRIDGE_IMPLEMENTATION.replace("QcaeQtSdk", "QcaeHbSdk").replace("qcae_qt_sdk", "qcae_hb_sdk")
    implementation = implementation.replace("Q_CORE_EXPORT", '__attribute__((visibility("default")))')
    implementation = implementation.replace("QCAE_QT_SDK_MANIFEST", "QCAE_HB_SDK_MANIFEST")
    bridge = source / "qcae-hb-sdk-observer.hpp"
    include = '#include "' + str(bridge) + '"\n'
    patched = {}
    sites = []
    bulk_pattern = re.compile(r"(?<![\w])(?:(?:std)::|::)?(memcpy|memmove|memset)\s*\(")
    api_pattern = re.compile(r"(?m)^(hb_(?:buffer|font|face|shape)[a-z_0-9]*)\s*\([^;{}]*?\)\s*\n\{")
    for relative, original in originals.items():
        text = original.decode()
        positions = code_positions(text)
        calls = [match for match in bulk_pattern.finditer(text) if positions[match.start()]]
        for ordinal, match in reversed(list(enumerate(calls, 1))):
            operation = match.group(1)
            site = f"harfbuzz/{relative}:{operation}:{ordinal}"
            text = text[:match.start()] + f'qcae_hb_sdk_{operation}("{site}", ' + text[match.end():]
            sites.append({"site": site, "kind": "actual_bulk_argument_bytes"})
        if relative.endswith(".cc"):
            positions = code_positions(text)
            searchable = "".join(char if positions[index] or char == "\n" else " "
                                 for index, char in enumerate(text))
            for match in reversed(list(api_pattern.finditer(searchable))):
                name = match.group(1)
                site = f"harfbuzz/{relative}:{name}"
                text = text[:match.end()] + f'\n  QcaeHbSdkScope qcaeHbSdkScope("{site}");' + text[match.end():]
                sites.append({"site": site, "kind": "actual_entry"})
        if text != original.decode():
            patched[relative] = text

    relative = "src/hb-vector.hh"
    text = patched.get(relative, originals[relative].decode())
    text = replace_once(text, "    return (Type *) hb_realloc (arrayZ, new_allocated * sizeof (Type));",
        '''    const std::uintptr_t qcaeOldAddress = reinterpret_cast<std::uintptr_t>(arrayZ);
    Type *qcaeReallocated = (Type *) hb_realloc (arrayZ, new_allocated * sizeof (Type));
    if (qcaeReallocated && reinterpret_cast<std::uintptr_t>(qcaeReallocated) != qcaeOldAddress)
      qcae_hb_sdk_observer_emit("harfbuzz/hb_vector::owned_prefix_relocation", 1, std::uint64_t(length) * sizeof(Type));
    return qcaeReallocated;''')
    patched[relative] = text
    sites.append({"site": "harfbuzz/hb_vector::owned_prefix_relocation", "kind": "actual_moved_logical_prefix_upper_bound"})
    relative = "src/hb-buffer.cc"
    text = patched[relative]
    text = replace_once(text, "  bool separate_out = out_info != info;",
        '''  bool separate_out = out_info != info;
  const std::uintptr_t qcaeOldPos = reinterpret_cast<std::uintptr_t>(pos);
  const std::uintptr_t qcaeOldInfo = reinterpret_cast<std::uintptr_t>(info);''')
    text = replace_once(text,
        "  new_pos = (hb_glyph_position_t *) hb_realloc (pos, new_bytes);\n  new_info = (hb_glyph_info_t *) hb_realloc (info, new_bytes);",
        '''  new_pos = (hb_glyph_position_t *) hb_realloc (pos, new_bytes);
  new_info = (hb_glyph_info_t *) hb_realloc (info, new_bytes);
  // Prior source-owned glyph array storage, never allocator headers/footprint.
  if (qcaeOldPos && new_pos && reinterpret_cast<std::uintptr_t>(new_pos) != qcaeOldPos)
    qcae_hb_sdk_observer_emit("harfbuzz/hb_buffer::position_array_relocation", 1, std::uint64_t(allocated) * sizeof(pos[0]));
  if (qcaeOldInfo && new_info && reinterpret_cast<std::uintptr_t>(new_info) != qcaeOldInfo)
    qcae_hb_sdk_observer_emit("harfbuzz/hb_buffer::info_array_relocation", 1, std::uint64_t(allocated) * sizeof(info[0]));''')
    patched[relative] = text
    sites += [{"site": f"harfbuzz/hb_buffer::{name}_array_relocation", "kind": "actual_moved_source_owned_array_upper_bound"}
              for name in ("position", "info")]

    # Map/typed stores are not bulk copies. This first tranche exposes their
    # actual shaping entry and retains the remaining coverage gap explicitly.
    relative = "src/hb-shape.cc"
    text = patched[relative]
    text = replace_once(text, "  buffer->enter ();",
        '''  qcae_hb_sdk_observer_emit("harfbuzz/remaining_typed_glyph_and_cache_stores", 3, 0);
  buffer->enter ();''')
    patched[relative] = text
    sites.append({"site": "harfbuzz/remaining_typed_glyph_and_cache_stores", "kind": "unsupported"})
    patched.setdefault("src/hb-common.cc", originals["src/hb-common.cc"].decode())

    manifest = {
        "schema": 1, "harfbuzz_version": VERSION, "archive_sha256": ARCHIVE_SHA,
        "callback_abi": "void(void*, const char*, unsigned, uint64_t)",
        "kind_codes": {"entry": 0, "copy_upper_bound": 1, "write_upper_bound": 2, "unsupported": 3},
        "coverage_complete": False,
        "whole_pipeline_owned_copy_coverage": "unknown",
        "remaining": ["typed glyph/cache writes and nontrivial vector element deep copies",
                      "indirect/generated copy sites from source/IR review",
                      "external native CoreText/Graphite/FreeType input-output ownership boundaries"],
        "sources": [{"path": name, "original_sha256": digest(originals[name]),
                     "instrumented_body_sha256": digest(text.encode())} for name, text in sorted(patched.items())],
        "sites": sorted(sites, key=lambda item: item["site"]),
        "bridge_header_sha256": digest(header.encode()),
        "bridge_implementation_sha256": digest(implementation.encode()),
    }
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    bridge.write_text(header)
    for relative, text in patched.items():
        text = include + text
        if relative == "src/hb-common.cc":
            text += "\n#define QCAE_HB_SDK_MANIFEST " + json.dumps(compact) + "\n" + implementation
        (source / relative).write_text(text)
    (BASE / "harfbuzz-observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def configure() -> None:
    command = ["cmake", "-S", str(BASE / f"harfbuzz-{VERSION}"), "-B", str(BASE / "hb-build"), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON", "-DCMAKE_OSX_ARCHITECTURES=arm64",
               f"-DCMAKE_INSTALL_PREFIX={BASE / 'hb-prefix'}", "-DCMAKE_PREFIX_PATH=/opt/homebrew",
               "-DHB_HAVE_FREETYPE=ON", "-DHB_HAVE_GRAPHITE2=ON", "-DHB_HAVE_CORETEXT=ON", "-DHB_HAVE_GLIB=ON",
               "-DGLIB_INCLUDE_DIR=/opt/homebrew/opt/glib/include/glib-2.0",
               "-DGLIBCONFIG_INCLUDE_DIR=/opt/homebrew/opt/glib/lib/glib-2.0/include",
               "-DGLIB_LIBRARIES=/opt/homebrew/opt/glib/lib/libglib-2.0.dylib",
               "-DHB_BUILD_SUBSET=OFF", "-DHB_BUILD_UTILS=OFF", "-DHB_BUILD_RASTER=OFF", "-DHB_BUILD_VECTOR=OFF",
               "-DHB_BUILD_GPU=OFF", "-DHB_BUILD_GPU_DEMO=OFF", "-DHB_HAVE_GOBJECT=OFF"]
    (BASE / "hb-configure-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (BASE / "hb-configure.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configure", action="store_true")
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()
    manifest = prepare()
    print(f"Prepared {len(manifest['sites'])} source-bound HarfBuzz sites; full coverage unknown")
    if args.configure:
        configure()
    if args.build:
        with (BASE / "hb-build.log").open("w") as log:
            subprocess.run(["cmake", "--build", str(BASE / "hb-build"), "--parallel", "2", "--target", "harfbuzz"],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        print("Isolated HarfBuzz compiled; runtime shaper/marker and output equivalence still required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
