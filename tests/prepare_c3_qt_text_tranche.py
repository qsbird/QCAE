#!/usr/bin/env python3
"""Prepare a second isolated Qt text tranche while the first prefix is frozen.

Real piece-table entries and individual owned glyph-array stores are added.
Every original statement still executes exactly once. Native/shaper/indirect
copy gaps remain explicit; the manifest is a source inventory, not a blanket
coverage assertion. Nothing is installed into the global SDK or first prefix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

import prepare_c3_qt_sdk_ledger as first


ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
SECOND = BASE / "tranche2"
EXTRA_SOURCES = ("src/gui/text/qtextdocument_p.cpp", "src/gui/text/qtextcursor.cpp",
                 "src/widgets/widgets/qwidgettextcontrol.cpp")
EXTRA_ENTRIES = {
    "src/gui/text/qtextdocument_p.cpp": (
        "QTextDocumentPrivate::insert", "QTextDocumentPrivate::insertBlock"),
    "src/gui/text/qtextcursor.cpp": ("QTextCursor::insertText",),
    "src/widgets/widgets/qwidgettextcontrol.cpp": ("QWidgetTextControlPrivate::append",),
}
WRITE_MACRO = r'''
#include <type_traits>
#include <utility>
// A sizeof operand does not evaluate an index expression. The original
// assignment executes once, including any post-increment, then reports the
// source-owned field's write upper bound. A bitfield uses its containing byte.
#define QCAE_QT_SDK_TYPED_WRITE(site, bytes, ...) \
    do { __VA_ARGS__; qcae_qt_sdk_observer_emit(site, 2, std::uint64_t(bytes)); } while (false)
template<class Invoke>
decltype(auto) qcae_qt_sdk_native_output(const char *site, const char *opaque,
                                        std::uint64_t bytes, Invoke &&invoke) {
    qcae_qt_sdk_observer_emit(site, 0, 0);
    if constexpr (std::is_void_v<std::invoke_result_t<Invoke>>) {
        std::forward<Invoke>(invoke)();
        qcae_qt_sdk_observer_emit(site, 2, bytes);
        qcae_qt_sdk_observer_emit(opaque, 3, 0);
    } else {
        auto result = std::forward<Invoke>(invoke)();
        qcae_qt_sdk_observer_emit(site, 2, bytes);
        qcae_qt_sdk_observer_emit(opaque, 3, 0);
        return result;
    }
}
'''


def digest(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def glyph_stores(text: str, relative: str, sites: list[dict]) -> str:
    arrays = r"(?:g|glyphs|initialGlyphs)(?:\.|->)(?:glyphs|advances|offsets|attributes|justifications)"
    lhs = rf"(?P<array>{arrays})\[(?P<index>[^\]\n]+)\](?P<field>\.[A-Za-z_][A-Za-z_0-9]*)?"
    pattern = re.compile(rf"(?P<lhs>{lhs})\s*(?:=(?!=)|\|=|\+=|-=)\s*[^;{{}}]+;")
    positions = first.code_positions(text)
    matches = [match for match in pattern.finditer(text) if positions[match.start()]]
    for ordinal, match in reversed(list(enumerate(matches, 1))):
        target = match.group("lhs")
        # sizeof(bitfield) is ill-formed. Its enclosing QGlyphAttributes is one
        # owned byte; charging it once per original bitfield assignment is a
        # documented upper bound, not a claim about emitted CPU instructions.
        if match.group("field") and match.group("array").endswith(("attributes", "justifications")):
            target = match.group("array") + "[" + match.group("index") + "]"
        site = f"{relative}:owned_glyph_store:{ordinal}"
        statement = text[match.start():match.end()].removesuffix(";")
        replacement = f'QCAE_QT_SDK_TYPED_WRITE("{site}", sizeof({target}), {statement});'
        text = text[:match.start()] + replacement + text[match.end():]
        sites.append({"site": site, "kind": "actual_owned_field_write_upper_bound",
                      "proof": "original assignment once; field sizeof; bitfield containing record",
                      "original_statement": statement})
    # Log-cluster writes are a separate ushort owned array. sizeof does not
    # re-evaluate the original str_pos++ expression.
    pattern = re.compile(r"(?P<lhs>log_clusters\[[^\]\n]+\])\s*=(?!=)\s*[^;{}]+;")
    positions = first.code_positions(text)
    matches = [match for match in pattern.finditer(text) if positions[match.start()]]
    for ordinal, match in reversed(list(enumerate(matches, 1))):
        site = f"{relative}:owned_cluster_store:{ordinal}"
        statement = text[match.start():match.end()].removesuffix(";")
        replacement = f'QCAE_QT_SDK_TYPED_WRITE("{site}", sizeof({match.group("lhs")}), {statement});'
        text = text[:match.start()] + replacement + text[match.end():]
        sites.append({"site": site, "kind": "actual_owned_field_write_upper_bound",
                      "proof": "original ushort array assignment once; unevaluated sizeof index",
                      "original_statement": statement})
    return text


def native_outputs(text: str, relative: str, sites: list[dict]) -> str:
    pattern = re.compile(r"(?P<name>CTFontGetGlyphsForCharacters|CTFontGetAdvancesForGlyphs)\([^;\n]+\)")
    positions = first.code_positions(text)
    matches = [match for match in pattern.finditer(text) if positions[match.start()]]
    for ordinal, match in reversed(list(enumerate(matches, 1))):
        invocation = match.group()
        # These exact calls have simple len/numGlyphs/1 counts and pointer
        # destinations. No argument with side effects is re-evaluated.
        count = invocation.rsplit(",", 1)[1].removesuffix(")").strip()
        if count not in ("len", "numGlyphs", "1"):
            raise ValueError("Unreviewed native output-count expression: " + invocation)
        element = "CGGlyph" if match.group("name") == "CTFontGetGlyphsForCharacters" else "CGSize"
        site = f"{relative}:native_owned_output:{ordinal}"
        opaque = site + ":opaque_internal"
        replacement = (f'qcae_qt_sdk_native_output("{site}", "{opaque}", '
                       f'std::uint64_t({count}) * sizeof({element}), [&] {{ return {invocation}; }})')
        text = text[:match.start()] + replacement + text[match.end():]
        sites += [
            {"site": site, "kind": "actual_native_call_and_owned_output_write_upper_bound",
             "proof": "original invocation once; supplied Qt-owned output count; partial fill bounded",
             "original_invocation": invocation},
            {"site": opaque, "kind": "unsupported",
             "proof": "native CoreText internal ownership is opaque; output-array observation does not close it"},
        ]
    return text


def prepare() -> dict:
    SECOND.mkdir(exist_ok=True)
    (SECOND / "downloads").mkdir(exist_ok=True)
    archive = BASE / "downloads" / f"qtbase-everywhere-src-{first.VERSION}.tar.xz"
    target = SECOND / "downloads" / archive.name
    if not target.exists():
        target.symlink_to(archive)
    first.SOURCES += EXTRA_SOURCES
    first.ENTRIES.update(EXTRA_ENTRIES)
    manifest = first.prepare(SECOND)
    source = SECOND / f"qtbase-everywhere-src-{first.VERSION}"
    bridge = source / "qcae-sdk-observer.hpp"
    bridge.write_text(first.BRIDGE_HEADER + WRITE_MACRO)
    sites = manifest["sites"]
    for relative in ("src/gui/text/qtextengine.cpp", "src/gui/text/coretext/qfontengine_coretext.mm"):
        path = source / relative
        text = glyph_stores(path.read_text(), relative, sites)
        if relative.endswith(".mm"):
            text = native_outputs(text, relative, sites)
        path.write_text(text)
        for row in manifest["sources"]:
            if row["path"] == relative:
                body = text.removeprefix('#include "' + str(bridge) + '"\n')
                row["instrumented_body_sha256"] = digest(body.encode())
    manifest["tranche"] = 2
    manifest["first_prefix_modified"] = False
    manifest["bridge_header_sha256"] = digest((first.BRIDGE_HEADER + WRITE_MACRO).encode())
    manifest["scope"] = "actual piece-table/text/layout/delegate/native font entries; nested owned payload operations"
    manifest["sites"] = sorted(sites, key=lambda row: row["site"])
    manifest["remaining"] = [
        "nontrivial container deep-copy/generated stores require source/IR inventory",
        "glyph/cache typed writes outside the enumerated qtextengine assignments",
        "external HarfBuzz calls require exact observed-library marker and remaining typed-store inventory",
        "native CoreText opaque storage; Qt-owned API input/output must be separately classified",
    ]
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    with tarfile.open(archive) as compressed:
        original = compressed.extractfile(f"qtbase-everywhere-src-{first.VERSION}/src/corelib/global/qglobal.cpp")
        if original is None:
            raise RuntimeError("The verified bridge owner source is missing")
        core = original.read().decode()
    core += '\n#include "' + str(bridge) + '"\n#define QCAE_QT_SDK_MANIFEST ' + json.dumps(compact) + "\n" + first.BRIDGE_IMPLEMENTATION
    (source / "src/corelib/global/qglobal.cpp").write_text(core)
    (SECOND / "observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def configure() -> None:
    # Reuse the recorded first-tranche Release/Cocoa feature choices and exact
    # dependency hints. Paths change only inside this second ignored tree.
    command = json.loads((BASE / "configure-command.json").read_text())
    command = [argument.replace(str(BASE), str(SECOND)) for argument in command]
    command += ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                f"-Dharfbuzz_DIR={BASE / 'hb-prefix/lib/cmake/harfbuzz'}",
                f"-DHARFBUZZ_INCLUDE_DIRS={BASE / 'hb-prefix/include'}",
                f"-DHARFBUZZ_LIBRARIES={BASE / 'hb-prefix/lib/libharfbuzz.dylib'}"]
    (SECOND / "configure-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (SECOND / "configure.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    first.feature_report(SECOND)
    report = json.loads((SECOND / "feature-comparison.json").read_text())
    missing = sum(bool(row.get("missing_generated_header")) for row in report)
    if missing:
        print(f"Feature comparison still pending: {missing} generated headers need the initial build")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configure", action="store_true")
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()
    manifest = prepare()
    print(f"Prepared second text tranche: {len(manifest['sites'])} sites; first prefix unchanged; coverage unknown")
    if args.configure:
        configure()
    if args.build:
        command = ["cmake", "--build", str(SECOND / "build"), "--parallel", "2", "--target",
                   "Core", "Gui", "Widgets", "Network", "OpenGL", "OpenGLWidgets", "Test",
                   "moc", "rcc", "uic", "QCocoaIntegrationPlugin", "QOffscreenIntegrationPlugin", "QMacStylePlugin"]
        with (SECOND / "build.log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        print("Second text libraries compiled; installed output and exact actual marker checks remain pending")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
