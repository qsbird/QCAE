#!/usr/bin/env python3
"""Prepare an isolated, source-bound Qt SDK observer without replacing the SDK.

The first source tranche observes actual text/layout entry points, explicit
bulk writes and glyph-buffer growth. The manifest deliberately retains the
remaining indirect, shaper and native backend gaps. No successful build is a
claim that those gaps have been closed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "6.11.1"
ARCHIVE_SHA = "d9594a31228aa23ad6b531719a29b45f0f3989fe6c136d45767ea179f233c1ac"

BRIDGE_HEADER = r'''#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if !defined(QT_BOOTSTRAPPED)
#include <QtCore/qtcoreexports.h>
extern "C" {
using QcaeQtSdkObserver = void (*)(void *, const char *, unsigned, std::uint64_t);
Q_CORE_EXPORT void qcae_qt_sdk_observer_install(QcaeQtSdkObserver callback, void *context);
Q_CORE_EXPORT const char *qcae_qt_sdk_observer_manifest();
Q_CORE_EXPORT void qcae_qt_sdk_observer_emit(const char *site, unsigned kind, std::uint64_t bytes);
Q_CORE_EXPORT void qcae_qt_sdk_observer_enter(const char *site);
Q_CORE_EXPORT void qcae_qt_sdk_observer_leave();
}
#else
inline void qcae_qt_sdk_observer_emit(const char *, unsigned, std::uint64_t) {}
inline void qcae_qt_sdk_observer_enter(const char *) {}
inline void qcae_qt_sdk_observer_leave() {}
#endif
struct QcaeQtSdkScope {
    explicit QcaeQtSdkScope(const char *site) { qcae_qt_sdk_observer_enter(site); }
    ~QcaeQtSdkScope() { qcae_qt_sdk_observer_leave(); }
    QcaeQtSdkScope(const QcaeQtSdkScope &) = delete;
    QcaeQtSdkScope &operator=(const QcaeQtSdkScope &) = delete;
};
inline void *qcae_qt_sdk_memcpy(const char *site, void *destination, const void *source, std::size_t bytes) {
    qcae_qt_sdk_observer_emit(site, 1, bytes);
    return std::memcpy(destination, source, bytes);
}
inline void *qcae_qt_sdk_memmove(const char *site, void *destination, const void *source, std::size_t bytes) {
    qcae_qt_sdk_observer_emit(site, 1, bytes);
    return std::memmove(destination, source, bytes);
}
inline void *qcae_qt_sdk_memset(const char *site, void *destination, int value, std::size_t bytes) {
    qcae_qt_sdk_observer_emit(site, 2, bytes);
    return std::memset(destination, value, bytes);
}
'''

BRIDGE_IMPLEMENTATION = r'''
#if !defined(QT_BOOTSTRAPPED)
#include <atomic>
namespace {
std::atomic<QcaeQtSdkObserver> qcae_qt_sdk_callback{};
std::atomic<void *> qcae_qt_sdk_context{};
thread_local unsigned qcae_qt_sdk_depth{};
thread_local bool qcae_qt_sdk_dispatching{};
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_install(QcaeQtSdkObserver callback, void *context) {
    // Installation is outside any measured operation. No callback owns Qt objects.
    qcae_qt_sdk_callback.store(nullptr, std::memory_order_release);
    qcae_qt_sdk_context.store(context, std::memory_order_release);
    qcae_qt_sdk_callback.store(callback, std::memory_order_release);
}
extern "C" Q_CORE_EXPORT const char *qcae_qt_sdk_observer_manifest() {
    return QCAE_QT_SDK_MANIFEST;
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_emit(const char *site, unsigned kind, std::uint64_t bytes) {
    if (!qcae_qt_sdk_depth || qcae_qt_sdk_dispatching)
        return;
    if (auto callback = qcae_qt_sdk_callback.load(std::memory_order_acquire)) {
        qcae_qt_sdk_dispatching = true;
        callback(qcae_qt_sdk_context.load(std::memory_order_acquire), site, kind, bytes);
        qcae_qt_sdk_dispatching = false;
    }
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_enter(const char *site) {
    ++qcae_qt_sdk_depth;
    qcae_qt_sdk_observer_emit(site, 0, 0);
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_leave() {
    if (qcae_qt_sdk_depth)
        --qcae_qt_sdk_depth;
}
#endif
'''

SOURCES = (
    "src/corelib/global/qglobal.cpp",
    "src/corelib/text/qstring.cpp",
    "src/corelib/tools/qarraydataops.h",
    "src/corelib/tools/qarraydatapointer.h",
    "src/corelib/tools/qvarlengtharray.h",
    "src/corelib/tools/qcontainertools_impl.h",
    "src/gui/text/qtextobject.cpp",
    "src/gui/text/qtextengine.cpp",
    "src/gui/text/qtextengine_p.h",
    "src/gui/text/qtextlayout.cpp",
    "src/gui/text/qfontengine.cpp",
    "src/gui/text/qfontengineglyphcache.cpp",
    "src/gui/text/coretext/qfontengine_coretext.mm",
    "src/widgets/widgets/qplaintextedit.cpp",
    "src/widgets/itemviews/qstyleditemdelegate.cpp",
    "src/widgets/styles/qcommonstyle.cpp",
)

ENTRIES = {
    "src/gui/text/qtextobject.cpp": ("QTextBlock::text",),
    "src/gui/text/qtextengine.cpp": (
        "QTextEngine::validate", "QTextEngine::shapeText", "QTextEngine::shapeTextWithHarfbuzzNG",
        "QTextEngine::LayoutData::reallocate", "QGlyphLayout::copy", "QGlyphLayout::grow",
    ),
    "src/gui/text/qtextlayout.cpp": (
        "QTextLayout::beginLayout", "QTextLayout::endLayout", "QTextLayout::draw", "QTextLayout::drawCursor",
    ),
    "src/widgets/widgets/qplaintextedit.cpp": (
        "QPlainTextDocumentLayout::layoutBlock", "QPlainTextEdit::paintEvent",
    ),
    "src/widgets/itemviews/qstyleditemdelegate.cpp": (
        "QStyledItemDelegate::paint", "QStyledItemDelegate::sizeHint", "QStyledItemDelegate::initStyleOption",
    ),
    "src/widgets/styles/qcommonstyle.cpp": ("QCommonStylePrivate::calculateElidedText",),
    "src/gui/text/coretext/qfontengine_coretext.mm": (
        "QCoreTextFontEngine::stringToCMap", "QCoreTextFontEngine::recalcAdvances",
    ),
}


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def replace_once(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise ValueError(f"Exact Qt source anchor missing or duplicated: {before[:100]}")
    return text.replace(before, after, 1)


def code_positions(text: str) -> bytearray:
    """Exclude comments and string/character literals from instrumentation."""
    positions = bytearray(b"\1") * len(text)
    index = 0
    while index < len(text):
        begin = index
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = len(text) if end < 0 else end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise ValueError("Unterminated source comment")
            index = end + 2
        elif text.startswith('R"', index):
            delimiter_end = text.find("(", index + 2)
            delimiter = text[index + 2:delimiter_end]
            end = text.find(")" + delimiter + '"', delimiter_end + 1)
            if delimiter_end < 0 or end < 0:
                raise ValueError("Unterminated raw source string")
            index = end + len(delimiter) + 2
        elif text[index] in "\"'":
            quote = text[index]
            index += 1
            while index < len(text):
                if text[index] == "\\":
                    index += 2
                elif text[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
        else:
            index += 1
            continue
        positions[begin:index] = b"\0" * (index - begin)
    return positions


def observe_entries(text: str, relative: str, sites: list[dict]) -> str:
    for name in ENTRIES.get(relative, ()):
        # Definitions only: matching ends at the opening function body, with
        # no semicolon or brace allowed in the parameter/qualifier span.
        pattern = re.compile(r"(?m)^([^\n]*\b" + re.escape(name) + r"\([^;{}]*?\)\s*(?:const\s*)?)\n\{")
        positions = code_positions(text)
        searchable = "".join(char if positions[index] or char == "\n" else " "
                             for index, char in enumerate(text))
        matches = list(pattern.finditer(searchable))
        if not matches:
            raise ValueError(f"No exact function definitions for {relative}:{name}")
        for ordinal, match in reversed(list(enumerate(matches, 1))):
            site = f"{relative}:{name}:{ordinal}"
            after = text[match.start():match.end()] + f'\n    QcaeQtSdkScope qcaeSdkScope("{site}");'
            text = text[:match.start()] + after + text[match.end():]
            sites.append({"site": site, "kind": "actual_entry", "proof": "original body executes unchanged"})
    return text


def observe_bulk(text: str, relative: str, sites: list[dict]) -> str:
    # Explicit libc bulk operations only. The original function and arguments
    # are passed through once. Source hashes and an inventory permit review of
    # each replacement; indirect/template stores remain separate gaps.
    pattern = re.compile(r"(?<![\w])(?:(?:std|QtPrivate)::|::)?(memcpy|memmove|memset)\s*\(")
    positions = code_positions(text)
    matches = [match for match in pattern.finditer(text) if positions[match.start()]]
    for ordinal, match in reversed(list(enumerate(matches, 1))):
        operation = match.group(1)
        site = f"{relative}:{operation}:{ordinal}"
        text = text[:match.start()] + f'qcae_qt_sdk_{operation}("{site}", ' + text[match.end():]
        sites.append({"site": site, "kind": "actual_bulk_argument_bytes", "proof": "original bulk call exactly once"})
    return text


def prepare(base: Path) -> dict:
    archive = base / "downloads" / f"qtbase-everywhere-src-{VERSION}.tar.xz"
    if digest(archive.read_bytes()) != ARCHIVE_SHA:
        raise ValueError("Qt official archive SHA-256 mismatch")
    source = base / f"qtbase-everywhere-src-{VERSION}"
    if not source.exists():
        with tarfile.open(archive) as compressed:
            compressed.extractall(base, filter="data")
    # Read anchors directly from the verified archive. Re-running preparation
    # replaces only this script's isolated owned files, never an installed SDK.
    originals = {}
    with tarfile.open(archive) as compressed:
        for relative in SOURCES:
            member = compressed.extractfile(f"qtbase-everywhere-src-{VERSION}/{relative}")
            if member is None:
                raise ValueError(f"Missing verified source: {relative}")
            originals[relative] = member.read()
    sites: list[dict] = []
    patched = {}
    for relative, original in originals.items():
        text = original.decode()
        if relative != "src/corelib/global/qglobal.cpp":
            text = observe_entries(text, relative, sites)
            text = observe_bulk(text, relative, sites)
        patched[relative] = text

    relative = "src/gui/text/qtextengine.cpp"
    text = patched[relative]
    text = replace_once(text,
        "    void **newMem = (void **)::realloc(memory_on_stack ? nullptr : memory, newAllocated*sizeof(void *));",
        '''    // Realloc may relocate the owned logical LayoutData storage. It
    // does not query allocator footprint; the bound is its prior source field.
    if (!memory_on_stack && memory && allocated > 0)
        qcae_qt_sdk_observer_emit("QTextEngine::LayoutData::owned_realloc", 1, std::uint64_t(allocated) * sizeof(void *));
    void **newMem = (void **)::realloc(memory_on_stack ? nullptr : memory, newAllocated*sizeof(void *));''')
    text = replace_once(text, "    hb_buffer_pre_allocate(buffer, itemLength);",
        '''    qcae_qt_sdk_observer_emit("system_harfbuzz::buffer_and_shape_internal", 3, 0);
    hb_buffer_pre_allocate(buffer, itemLength);''')
    patched[relative] = text
    sites += [
        {"site": "QTextEngine::LayoutData::owned_realloc", "kind": "logical_storage_relocation_upper_bound",
         "proof": "prior allocated field; excludes allocator headers and capacities outside this logical layout"},
        {"site": "system_harfbuzz::buffer_and_shape_internal", "kind": "unsupported",
         "proof": "system library source/IR observer required; not claimed by Qt hooks"},
    ]

    # POD repetition and fast realloc do not necessarily invoke memcpy.
    relative = "src/corelib/tools/qarraydataops.h"
    patched[relative] = replace_once(patched[relative],
        "        T *where = this->end();\n        this->size += qsizetype(n);\n        while (n--)",
        '''        qcae_qt_sdk_observer_emit("QPodArrayOps::copyAppend_repeat", 2, std::uint64_t(n) * sizeof(T));
        T *where = this->end();
        this->size += qsizetype(n);
        while (n--)''')
    relative = "src/corelib/tools/qarraydatapointer.h"
    patched[relative] = replace_once(patched[relative],
        "                (*this)->reallocate(constAllocatedCapacity() - freeSpaceAtEnd() + n, QArrayData::Grow); // fast path",
        '''                qcae_qt_sdk_observer_emit("QArrayDataPointer::fast_realloc_initialized_prefix", 1, std::uint64_t(size) * sizeof(T));
                (*this)->reallocate(constAllocatedCapacity() - freeSpaceAtEnd() + n, QArrayData::Grow); // fast path''')
    sites += [
        {"site": "QPodArrayOps::copyAppend_repeat", "kind": "actual_loop_write_bytes", "proof": "n initialized T assignments"},
        {"site": "QArrayDataPointer::fast_realloc_initialized_prefix", "kind": "logical_prefix_relocation_upper_bound", "proof": "size * sizeof(T), not allocation footprint"},
    ]

    manifest = {
        "schema": 1, "qt_version": VERSION, "archive_sha256": ARCHIVE_SHA,
        "callback_abi": "void(void*, const char*, unsigned, uint64_t)",
        "kind_codes": {"entry": 0, "copy_upper_bound": 1, "write_upper_bound": 2, "unsupported": 3},
        "scope": "actual text/layout/delegate/native font entries; nested model-payload operations only",
        "whole_pipeline_owned_copy_coverage": "unknown",
        "coverage_complete": False,
        "remaining": [
            "nontrivial container constructor/assignment deep copies and generated store loops",
            "all scalar glyph/layout state writes and Qt font/backend buffer inventory",
            "installed system HarfBuzz 14.4.0 internal initialized payload moves",
            "native CoreText opaque internal storage (Qt-owned input/output buffers need separate classification)",
        ],
        "sources": [{"path": name, "original_sha256": digest(originals[name]),
                     "instrumented_body_sha256": digest(patched[name].encode())} for name in SOURCES],
        "sites": sorted(sites, key=lambda item: item["site"]),
        "bridge_header_sha256": digest(BRIDGE_HEADER.encode()),
        "bridge_implementation_sha256": digest(BRIDGE_IMPLEMENTATION.encode()),
    }
    compact = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    bridge = source / "qcae-sdk-observer.hpp"
    bridge.write_text(BRIDGE_HEADER)
    include = '#include "' + str(bridge) + '"\n'
    for relative, text in patched.items():
        if relative == "src/corelib/global/qglobal.cpp":
            text += "\n" + include + "#define QCAE_QT_SDK_MANIFEST " + json.dumps(compact) + "\n" + BRIDGE_IMPLEMENTATION
        else:
            text = include + text
        (source / relative).write_text(text)
    (base / "observer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    installed = Path("/opt/homebrew/opt/qt/lib")
    features = {}
    for framework in ("QtCore", "QtGui", "QtWidgets", "QtNetwork", "QtOpenGL", "QtOpenGLWidgets"):
        headers = installed / f"{framework}.framework/Headers"
        for header in (headers / "qconfig.h", headers / f"{framework.lower()}-config.h",
                       headers / VERSION / framework / "private" / f"{framework.lower()}-config_p.h"):
            if header.exists():
                features[str(header)] = {"sha256": digest(header.read_bytes()), "features": dict(
                    re.findall(r"#define QT_FEATURE_(\w+) (-?1)", header.read_text()))}
    (base / "installed-feature-baseline.json").write_text(json.dumps(features, indent=2) + "\n")
    return manifest


def configure(base: Path) -> None:
    source = base / f"qtbase-everywhere-src-{VERSION}"
    command = [
        "cmake", "-S", str(source), "-B", str(base / "build"), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={base / 'prefix'}",
        "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0",
        "-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/icu4c@78;/opt/homebrew", "-DICU_ROOT=/opt/homebrew/opt/icu4c@78",
        "-DICU_INCLUDE_DIR=/opt/homebrew/opt/icu4c@78/include",
        "-DICU_I18N_LIBRARY_RELEASE=/opt/homebrew/opt/icu4c@78/lib/libicui18n.dylib",
        "-DICU_UC_LIBRARY_RELEASE=/opt/homebrew/opt/icu4c@78/lib/libicuuc.dylib",
        "-DICU_DATA_LIBRARY_RELEASE=/opt/homebrew/opt/icu4c@78/lib/libicudata.dylib",
        "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_EXAMPLES=OFF",
        "-DQT_BUILD_BENCHMARKS=OFF", "-DBUILD_WITH_PCH=OFF",
        "-DFEATURE_framework=ON", "-DFEATURE_opengl=ON", "-DFEATURE_widgets=ON",
        "-DFEATURE_icu=ON", "-DFEATURE_system_harfbuzz=ON", "-DFEATURE_system_freetype=ON",
        "-DFEATURE_fontconfig=OFF", "-DFEATURE_openssl_linked=ON",
    ]
    xcode = subprocess.run(["xcrun", "xcodebuild", "-version"], capture_output=True, text=True)
    sdk = subprocess.run(["xcrun", "--show-sdk-version"], capture_output=True, text=True, check=True)
    compiler = subprocess.run(["c++", "--version"], capture_output=True, text=True, check=True)
    # This machine has Command Line Tools, as used by the QCAE/VTK builds.
    # Keep the actual SDK check; bypass only the unavailable full-Xcode check,
    # using Qt's documented build switch rather than inventing its version.
    if xcode.returncode:
        command.append("-DQT_NO_XCODE_MIN_VERSION_CHECK=ON")
    (base / "actual-toolchain.json").write_text(json.dumps({
        "compiler": compiler.stdout, "sdk_version": sdk.stdout.strip(),
        "full_xcode_available": xcode.returncode == 0,
        "full_xcode_output": xcode.stdout + xcode.stderr,
    }, indent=2) + "\n")
    (base / "configure-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (base / "configure.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)


def feature_report(base: Path) -> None:
    baseline = json.loads((base / "installed-feature-baseline.json").read_text())
    report = []
    for installed, expected in baseline.items():
        original = Path(installed)
        framework = next(part.split(".")[0] for part in original.parts if part.endswith(".framework"))
        if "private" in original.parts:
            generated = base / "build/include" / framework / VERSION / framework / "private" / original.name
        else:
            generated = base / "build/include" / framework / original.name
        if not generated.exists():
            report.append({"installed": installed, "missing_generated_header": True})
            continue
        text = generated.read_text()
        # Qt build-tree include files forward to generated feature headers.
        forwarded = re.fullmatch(r'#include "([^"]+)"[^\n]*\n?', text)
        actual = Path(forwarded.group(1)) if forwarded else generated
        features = dict(re.findall(r"#define QT_FEATURE_(\w+) (-?1)", actual.read_text()))
        differences = {name: {"installed": value, "observed": features.get(name)}
                       for name, value in expected["features"].items() if features.get(name) != value}
        report.append({"installed": installed, "observed": str(actual), "differences": differences})
    (base / "feature-comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    differences = sum(len(item.get("differences", {})) for item in report)
    print(f"Required feature comparison: {differences} differences (recorded, not silently accepted)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=ROOT / "build-c3-qt-observed")
    parser.add_argument("--configure", action="store_true")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args()
    base = args.directory.resolve()
    if base != ROOT / "build-c3-qt-observed":
        raise ValueError("Use the explicitly isolated ignored build-c3-qt-observed directory")
    if args.report_only:
        feature_report(base)
        return 0
    manifest = prepare(base)
    print(f"Prepared {len(manifest['sites'])} observed sites; full SDK coverage remains unknown")
    if args.configure:
        configure(base)
        feature_report(base)
        print("Qt observed Release configuration completed; compare required SDK feature baseline")
    if args.build:
        command = ["cmake", "--build", str(base / "build"), "--parallel", "2", "--target",
                   "Core", "Gui", "Widgets", "Network", "OpenGL", "OpenGLWidgets", "Test",
                   "moc", "rcc", "uic", "QCocoaIntegrationPlugin", "QOffscreenIntegrationPlugin", "QMacStylePlugin"]
        with (base / "build.log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        print("Observed Qt libraries built; installation/loading and behavioral checks remain required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
