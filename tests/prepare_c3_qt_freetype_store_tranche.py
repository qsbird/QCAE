#!/usr/bin/env python3
"""Prepare a finite Qt-FT owned-store diagnostic, never full FreeType coverage.

The frozen Qt5 bitmap repair is retained. Original expressions execute once;
new observers separate byte copies, typed writes and reached unknown growth.
A gated HB message trace records actual transient AAT glyphs without replacing
shaping. All prior sources, object files and installed prefixes are read-only.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import prepare_c3_qt_sdk_ledger as first
import prepare_c3_qt_freetype_metrics_tranche as fifth

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
SECOND = BASE / "tranche2"
THIRD = BASE / "tranche3"
FIFTH = BASE / "tranche5"
SIXTH = BASE / "tranche6"
SOURCE = SIXTH / "qt-ft-sources"
PREFIX = SIXTH / "qt-prefix"
FT_SOURCE = "src/gui/text/freetype/qfontengine_ft.cpp"
ENGINE_SOURCE = "src/gui/text/qtextengine.cpp"
CORE_SOURCE = "src/corelib/global/qglobal.cpp"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def insert_owned_stores(text: str, sites: list[dict]) -> str:
    positions = first.code_positions(text)
    lhs = (r"(?:info\.[A-Za-z_]\w*|g->(?:data|linearAdvance|width|height|x|y|advance|format)|"
           r"glyphs->(?:glyphs\[glyph_pos\]|advances\[i\])|"
           r"freetype->cmapCache\[(?:uc|ucs4)\]|(?:dst|dd)\[x\]|\*dstPixel|"
           r"fast_glyph_data\[(?:i|index)\]|fast_glyph_count|"
           r"m_(?:ascent|descent|leading|heightMetricsQueried))")
    assignment = r"(?:=(?!=)|\*=|\+=)"
    pattern = re.compile(r"(?m)^[ \t]*(?P<statement>(?P<lhs>" + lhs + r")\s*" + assignment + r"[^;{}]+;)")
    matches = [m for m in pattern.finditer(text) if positions[m.start("statement")]]
    chain = re.compile(r"(?P<lhs>" + lhs + r")\s*" + assignment)
    for ordinal, match in reversed(list(enumerate(matches, 1))):
        statement = match.group("statement").removesuffix(";")
        destinations = [m.group("lhs") for m in chain.finditer(statement)]
        site = f"{FT_SOURCE}:ft_typed_owned_store:{ordinal}"
        bound = " + ".join("sizeof(" + dest + ")" for dest in destinations)
        replacement = f'QCAE_QT_SDK_TYPED_WRITE("{site}", {bound}, {statement});'
        sites.append({"site": site, "kind": "actual_owned_field_write_upper_bound",
                      "original_statement": statement, "destinations": destinations,
                      "proof": "original assignment once; sizeof operands unevaluated; chained destinations counted",
                      "ownership": "Qt glyph record, transient glyph metrics, bitmap output, map/advance output or cache field",
                      "not_observed": "allocator and indirect Qt container growth; FreeType internal stores"})
        text = text[:match.start("statement")] + replacement + text[match.end("statement"):]
    # Explicit aggregate assignments copy the exact source-owned typed payload.
    ordinal = 0
    for statement, expected in (("metrics = face->size->metrics;", 1),
                                 ("freetype->matrix = matrix;", 2),
                                 ("gs->transformationMatrix = m;", 1)):
        if text.count(statement) != expected:
            raise ValueError("Aggregate boundary changed: " + statement)
        for _ in range(expected):
            ordinal += 1
            dest = statement.split(" = ")[0]
            site = f"{FT_SOURCE}:ft_aggregate_copy:{ordinal}"
            replacement = f'do {{ {statement} qcae_qt_sdk_observer_emit("{site}", 1, sizeof({dest})); }} while (false);'
            # Search only original lines; an already-wrapped statement remains in the wrapper.
            match = re.search(r"(?m)^[ \t]*" + re.escape(statement), text)
            if match is None:
                raise ValueError("Aggregate original line missing")
            offset = text.index(statement, match.start())
            text = text[:offset] + replacement + text[offset + len(statement):]
            sites.append({"site": site, "kind": "actual_typed_aggregate_copy",
                          "original_statement": statement, "destination": dest,
                          "proof": "original typed aggregate copy once; sizeof actual destination; no allocator footprint"})
    # Hash mutation can move owned keys/payload. An unproved upper bound stays unknown.
    for ordinal, statement in enumerate(("glyph_data.insert(GlyphAndSubPixelPosition(index, subPixelPosition), glyph);",
                                          "set->setGlyphMissing(glyph);"), 1):
        if text.count(statement) != 1:
            raise ValueError("Unknown cache boundary changed: " + statement)
        site = f"{FT_SOURCE}:ft_hash_cache_growth_unknown:{ordinal}"
        text = text.replace(statement, f'do {{ {statement} qcae_qt_sdk_observer_emit("{site}", 3, 0); }} while (false);')
        sites.append({"site": site, "kind": "actual_reached_unknown",
                      "original_statement": statement,
                      "reason": "Qt hash bucket/key moves and growth are not measured by this FT translation unit"})
    before = dict(first.ENTRIES)
    first.ENTRIES[FT_SOURCE] = ("QFontEngineFT::loadGlyph", "QFontEngineFT::loadColrv1Glyph",
                               "QFontEngineFT::recalcAdvances", "QFontEngineFT::boundingBox",
                               "QFontEngineFT::alphaMapBoundingBox", "QFontEngineFT::loadGlyphFor",
                               "QFontEngineFT::QGlyphSet::setGlyph",
                               "QFontEngineFT::QGlyphSet::clear",
                               "QFontEngineFT::initializeHeightMetrics")
    try:
        text = first.observe_entries(text, FT_SOURCE, sites)
        text = first.observe_bulk(text, FT_SOURCE, sites)
    finally:
        first.ENTRIES.clear()
        first.ENTRIES.update(before)
    bridge = SECOND / "qtbase-everywhere-src-6.11.1/qcae-sdk-observer.hpp"
    return '#include "' + str(bridge) + '"\n' + text


TRACE_HELPER = r'''
// Independent diagnostic only. Returning true preserves the original HB path.
struct QcaeFtShapeTrace {
    const ushort *text;
    int textLength;
    QFontEngine *engine;
    bool sawDeleted = false;
};
static hb_bool_t qcaeFtShapeTrace(hb_buffer_t *b, hb_font_t *, const char *message, void *opaque)
{
    auto *trace = static_cast<QcaeFtShapeTrace *>(opaque);
    unsigned length = 0;
    auto *info = hb_buffer_get_glyph_infos(b, &length);
    bool containsDeleted = false;
    for (unsigned i = 0; i < length; ++i)
        containsDeleted |= info[i].codepoint == 0xffff;
    if (!containsDeleted && !trace->sawDeleted)
        return true;
    trace->sawDeleted |= containsDeleted;
    std::fprintf(stderr, "QCAE_FT_AAT_TRACE {\"message\":\"%s\",\"engine\":\"%p\",\"engine_type\":%d,\"pixel_size\":%.17g,\"utf16\":[",
                 message, static_cast<void *>(trace->engine), int(trace->engine->type()),
                 trace->engine->fontDef.pixelSize);
    for (int i = 0; i < trace->textLength; ++i)
        std::fprintf(stderr, "%s%u", i ? "," : "", unsigned(trace->text[i]));
    std::fprintf(stderr, "],\"glyphs\":[");
    for (unsigned i = 0; i < length; ++i)
        std::fprintf(stderr, "%s{\"id\":%u,\"cluster\":%u}", i ? "," : "", info[i].codepoint, info[i].cluster);
    std::fprintf(stderr, "]}\n");
    return true;
}
'''


def insert_causal_trace(text: str) -> str:
    anchor = "int QTextEngine::shapeTextWithHarfbuzzNG("
    if text.count(anchor) != 1:
        raise ValueError("The original shape entry changed")
    text = text.replace(anchor, TRACE_HELPER + "\n" + anchor)
    old = "            bool shapedOk = hb_shape_full(hb_font,"
    if text.count(old) != 1:
        raise ValueError("The original HB invocation changed")
    replacement = '''            QcaeFtShapeTrace qcaeTrace{string, stringLength, actualFontEngine};
            const bool qcaeTraceEnabled = qEnvironmentVariableIsSet("QCAE_FT_AAT_TRACE");
            if (qcaeTraceEnabled)
                hb_buffer_set_message_func(buffer, qcaeFtShapeTrace, &qcaeTrace, nullptr);
            const auto qcaeTraceCleanup = qScopeGuard([&]() {
                if (qcaeTraceEnabled)
                    hb_buffer_set_message_func(buffer, nullptr, nullptr, nullptr);
            });
            bool shapedOk = hb_shape_full(hb_font,'''
    text = text.replace(old, replacement)
    old = "            if (Q_UNLIKELY(!shapedOk))\n                return 0;"
    if text.count(old) != 1:
        raise ValueError("The original HB result branch changed")
    text = text.replace(old, '''            if (qcaeTraceEnabled && qcaeTrace.sawDeleted)
                qcaeFtShapeTrace(buffer, hb_font, "final-shape-result", &qcaeTrace);
''' + old)
    return '#include <cstdio>\n#include <QtCore/qscopeguard.h>\n' + text


def prepare() -> dict:
    if PREFIX.exists() or (SIXTH / "qt-observer-manifest.json").exists():
        raise ValueError("Refusing to mutate an existing or frozen tranche6")
    SOURCE.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((FIFTH / "qt-observer-manifest.json").read_text())
    sites = manifest["sites"]
    ft = FIFTH / "qt-metrics-sources" / FT_SOURCE
    engine = SECOND / "qtbase-everywhere-src-6.11.1" / ENGINE_SOURCE
    sources = {r["path"]: r for r in manifest["sources"]}
    for rel, path, transform in ((FT_SOURCE, ft, lambda text: insert_owned_stores(text, sites)),
                                  (ENGINE_SOURCE, engine, insert_causal_trace)):
        original = path.read_bytes()
        after = transform(original.decode())
        target = SOURCE / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(after)
        old = sources.get(rel, {})
        sources[rel] = {**old, "path": rel, "prior_frozen_path": str(path),
                        "prior_frozen_sha256": digest(original),
                        "instrumented_body_sha256": digest(after.encode())}
    manifest.update(tranche=6, sdk_prefix=str(PREFIX), coverage_complete=False,
                    previous_prefix_modified=False,
                    ft_internal_copy_coverage="unknown; installed FreeType2.14.3 implementation is unobserved",
                    sources=sorted(sources.values(), key=lambda row: row["path"]),
                    sites=sorted(sites, key=lambda row: row["site"]))
    manifest["scope"] += "; finite Qt-FT byte copies, typed stores, exact aggregate copies and reached hash-growth unknowns"
    manifest["causal_trace"] = {"source": ENGINE_SOURCE, "gate": "QCAE_FT_AAT_TRACE",
                               "purpose": "record actual intermediate 65535 glyphs and final buffer; original hb_shape_full once",
                               "not_a_copy_metric": True, "always_returns_true": True}
    core = (FIFTH / "qt-metrics-sources" / CORE_SOURCE).read_text()
    declaration = "#define QCAE_QT_SDK_MANIFEST "
    start = core.index(declaration)
    end = core.index("\n", start)
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    target = SOURCE / CORE_SOURCE
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(core[:start] + declaration + json.dumps(compact) + core[end:])
    (SIXTH / "qt-observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def build(manifest: dict) -> None:
    # Reuse exact Qt5 commands and immutable objects, replacing only three TUs.
    inventory = json.loads((FIFTH / "object-reuse.json").read_text())
    base_build = SECOND / "build"
    command_lines = subprocess.check_output(["ninja", "-C", str(base_build), "-t", "commands", "Gui"], text=True).splitlines()
    candidates = [c for c in command_lines if "/" + ENGINE_SOURCE in c and " -c " in c]
    if len(candidates) != 1:
        raise ValueError("Expected one original qtextengine compiler command")
    commands = dict(inventory["actual_compile_commands"])
    commands[ENGINE_SOURCE] = fifth.compiler_part(candidates[0])
    objects = SIXTH / "qt-objects"
    objects.mkdir(exist_ok=True)
    jobs = []
    replacements = {}
    report = {"schema": 1, "manifest_sha256": digest((SIXTH / "qt-observer-manifest.json").read_bytes()),
              "prior_inventory": str(FIFTH / "object-reuse.json"),
              "prior_inventory_sha256": digest((FIFTH / "object-reuse.json").read_bytes()),
              "actual_compile_commands": {}, "actual_link_commands": {},
              "compiler": inventory["compiler"], "sdk_settings": inventory["sdk_settings"],
              "abi_feature_header_flags_unchanged": True, "previous_prefix_modified": False}
    for rel, original in commands.items():
        args = list(original)
        old_object = args[args.index("-o") + 1]
        output = objects / (Path(rel).name + ".o")
        args[args.index("-o") + 1] = str(output)
        args[args.index("-c") + 1] = str(SOURCE / rel)
        if rel == ENGINE_SOURCE:
            args[1:1] = ["-iquote", str((SECOND / "qtbase-everywhere-src-6.11.1" / rel).parent)]
        if "-MF" in args:
            args[args.index("-MF") + 1] = str(output) + ".d"
        replacements[old_object] = str(output)
        report["actual_compile_commands"][rel] = args
        jobs.append((rel, args))
    def one(job):
        rel, args = job
        with (SIXTH / ("compile-" + Path(rel).name + ".log")).open("w") as log:
            subprocess.run(args, cwd=base_build, stdout=log, stderr=subprocess.STDOUT, check=True)
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(one, jobs))
    shutil.copytree(FIFTH / "qt-prefix", PREFIX, symlinks=True)
    for suffix in (".cmake", ".pc", ".prl", ".pri"):
        for path in PREFIX.rglob("*" + suffix):
            text = path.read_text()
            changed = text.replace(str(FIFTH / "qt-prefix"), str(PREFIX))
            if text != changed:
                path.write_text(changed)
    reused = set()
    for module, original in inventory["actual_link_commands"].items():
        args = list(original)
        output = PREFIX / f"lib/Qt{module}.framework/Versions/A/Qt{module}"
        args[args.index("-o") + 1] = str(output)
        for i, value in enumerate(args):
            if value in replacements:
                args[i] = replacements[value]
            elif value.endswith((".o", ".a")):
                path = Path(value)
                reused.add((path if path.is_absolute() else base_build / path).resolve())
            elif value.endswith("libharfbuzz.dylib"):
                args[i] = str(PREFIX / "lib/libharfbuzz.dylib")
        report["actual_link_commands"][module] = args
        with (SIXTH / ("link-" + module + ".log")).open("w") as log:
            subprocess.run(args, cwd=base_build, stdout=log, stderr=subprocess.STDOUT, check=True)
        details = subprocess.check_output(["otool", "-l", str(output)], text=True)
        rpaths = re.findall(r"cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset", details)
        for rpath in rpaths:
            if rpath.startswith(str(BASE)) or rpath == "/opt/homebrew/lib":
                subprocess.run(["install_name_tool", "-delete_rpath", rpath, str(output)], check=True)
        if "@loader_path/../../.." not in rpaths:
            subprocess.run(["install_name_tool", "-add_rpath", "@loader_path/../../..", str(output)], check=True)
    report["reused_objects"] = [{"path": str(p), "sha256": digest(p.read_bytes())} for p in sorted(reused)]
    report["recompiled_sources"] = [{"path": str(SOURCE / rel), "sha256": digest((SOURCE / rel).read_bytes())} for rel in commands]
    (SIXTH / "object-reuse.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Qt6 FT store diagnostic linked:", len(manifest["sites"]), "sites;", len(reused), "reused objects")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()
    manifest = prepare()
    print("Prepared", len(manifest["sites"]), "Qt sites; whole coverage remains false")
    if args.build:
        build(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
