#!/usr/bin/env python3
"""Observe actual FT cache Node/Span/rehash statements in an isolated Qt7 SDK.

Only the FT cache insertion scopes enable these template hooks. Original stores
and libc calls execute once. Parent entries carry no bytes, child destinations
own the facts, and type/limit/unseen-action failures remain explicit unknowns.
Earlier prefixes, probes, headers and sources are never edited.
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
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build-c3-qt-observed"
SECOND = BASE / "tranche2"
SIXTH = BASE / "tranche6"
SEVENTH = BASE / "tranche7"
SOURCE = SEVENTH / "qt-ft-hash-sources"
SHADOW = SEVENTH / "qt-headers"
PREFIX = SEVENTH / "qt-prefix"
FT = "src/gui/text/freetype/qfontengine_ft.cpp"
CORE = "src/corelib/global/qglobal.cpp"
QHASH = "src/corelib/tools/qhash.h"
FT_HEADER = "src/gui/text/freetype/qfontengine_ft_p.h"
ARCHIVE_SHA = "d9594a31228aa23ad6b531719a29b45f0f3989fe6c136d45767ea179f233c1ac"

HELPER = r'''
#pragma once
#include "QCAE_BRIDGE_PATH"
#include <limits>
namespace QcaeFtHash {
struct Scope;
inline thread_local Scope *active = nullptr;
struct Scope {
    Scope *previous;
    unsigned cache;
    std::uint64_t remaining;
    unsigned actions = 0;
    bool unknown = false;
    bool finished = false;
    QcaeQtSdkScope sdk;
    explicit Scope(unsigned kind, std::uint64_t byteLimit = (std::numeric_limits<std::uint64_t>::max)())
        : previous(active), cache(kind), remaining(byteLimit), sdk(kind == 1 ? "ft_hash/glyph_operation" : "ft_hash/missing_operation")
    { active = this; }
    void finish() { finished = true; }
    ~Scope() {
        if (!finished || actions != 1)
            markUnknown("ft_hash/unseen_or_multiple_node_action");
        qcae_qt_sdk_observer_emit(cache == 1 ? "ft_hash/glyph_operation_complete" : "ft_hash/missing_operation_complete", unknown ? 3 : 0, 0);
        active = previous;
    }
    void markUnknown(const char *site) {
        unknown = true;
        qcae_qt_sdk_observer_emit(site, 3, 0);
    }
};
inline void record(const char *site, unsigned kind, std::uint64_t bytes) {
    if (!active)
        return;
    if (bytes > active->remaining) {
        active->markUnknown("ft_hash/observer_byte_limit");
        return;
    }
    active->remaining -= bytes;
    qcae_qt_sdk_observer_emit(site, kind, bytes);
}
template<class Key, class = void> struct HasGlyphFields : std::false_type {};
template<class Key> struct HasGlyphFields<Key, std::void_t<decltype(std::declval<Key>().glyph), decltype(std::declval<Key>().subPixelPosition)>> : std::true_type {};
template<class Node, class Value, class = void> struct HasDirectValue : std::false_type {};
template<class Node, class Value> struct HasDirectValue<Node, Value, std::void_t<decltype(std::declval<Node>().value)>> : std::is_same<std::decay_t<decltype(std::declval<Node>().value)>, Value> {};
template<class Node> bool knownNode() {
    if (!active)
        return false;
    using Key = typename Node::KeyType;
    using Value = typename Node::ValueType;
    constexpr bool glyphShape = HasGlyphFields<Key>::value;
    constexpr bool trivialKey = std::is_trivially_copyable_v<Key> && std::is_standard_layout_v<Key>;
    constexpr bool directValue = HasDirectValue<Node, Value>::value;
    constexpr bool glyphNode = glyphShape && trivialKey && sizeof(Key) == 12 && std::is_pointer_v<Value> && directValue;
    constexpr bool missingNode = trivialKey && std::is_unsigned_v<Key> && sizeof(Key) == 4 && std::is_empty_v<Value>;
    if ((active->cache == 1 && glyphNode) || (active->cache == 2 && missingNode))
        return true;
    active->markUnknown("ft_hash/unsupported_node_type");
    return false;
}
template<class Node> void field(const char *site, unsigned kind, std::uint64_t bytes) {
    if (knownNode<Node>())
        record(site, kind, bytes);
}
template<class Node> void nodeAction(const char *site, unsigned kind, std::uint64_t bytes) {
    if (knownNode<Node>()) {
        ++active->actions;
        record(site, kind, bytes);
    }
}
template<class Node> void *copy(const char *site, void *dest, const void *src, std::size_t bytes) {
    auto *result = std::memcpy(dest, src, bytes);
    field<Node>(site, 1, bytes);
    return result;
}
template<class Node> void *initialize(const char *site, void *dest, int value, std::size_t bytes) {
    auto *result = std::memset(dest, value, bytes);
    field<Node>(site, 2, bytes);
    return result;
}
} // namespace QcaeFtHash
'''


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def add_site(sites: list[dict], site: str, kind: str, original: str, proof: str) -> None:
    if any(row["site"] == site for row in sites):
        raise ValueError("Duplicate observed site " + site)
    sites.append({"site": site, "kind": kind, "original_statement": original, "proof": proof,
                  "scope": "only FT glyph_data or missing_glyphs insertion operation; unrelated hashes inactive"})


def transform_hash(text: str, sites: list[dict], helper: Path) -> str:
    # Only the first Node/Span/Data/QHash definitions, never QMultiHash.
    stop = text.index("template <typename Key, typename T>\nclass QMultiHash") if "template <typename Key, typename T>\nclass QMultiHash" in text else text.index("class QMultiHash\n")
    head, tail = text[:stop], text[stop:]
    prefix = "ft_hash/"
    def replace(old: str, new: str, name: str, kind: str, proof: str, count: int = 1):
        nonlocal head
        if head.count(old) != count:
            raise ValueError(f"Exact hash anchor {name} changed: {head.count(old)} != {count}")
        head = head.replace(old, new)
        add_site(sites, prefix + name, kind, old, proof)
    def after(old: str, expression: str, name: str, kind: str = "actual_owned_field_write_upper_bound", action: bool = False):
        operation = "nodeAction" if action else "field"
        metric = 1 if "copy" in kind else 2
        new = old + ("\n        " if "//" in old else " ") + f'QcaeFtHash::{operation}<Node>("{prefix + name}", {metric}, {expression});'
        replace(old, new, name, kind, "original statement once; typed original destination only; no parent byte charge")
    # Named fresh-node aggregate construction. Creation and later relocation are disjoint.
    for old, name, expr in (
        ("{ new (n) Node{ std::move(k), T(std::forward<Args>(args)...) }; }", "node_create_move", "sizeof(Node)"),
        ("{ new (n) Node{ Key(k), T(std::forward<Args>(args)...) }; }", "node_create_copy", "sizeof(Node)"),
        ("{ new (n) Node{ std::move(k) }; }", "set_node_create_move", "sizeof(Node)"),
        ("{ new (n) Node{ k }; }", "set_node_create_copy", "sizeof(Node)")):
        new = old[:-1] + f' QcaeFtHash::nodeAction<Node>("{prefix + name}", 1, {expr}); }}'
        replace(old, new, name, "actual_typed_aggregate_copy_upper_bound", "placement aggregate executes once; sizeof Node bounds actual typed representation, no element-count estimate")
    after("        value = T(std::forward<Args>(args)...);", "sizeof(value)", "node_value_assign", "actual_typed_field_copy", True)
    old = "    void emplaceValue(Args &&...)\n    {\n    }"
    new = "    void emplaceValue(Args &&...)\n    {\n        QcaeFtHash::nodeAction<Node>(\"ft_hash/set_node_existing_no_payload\", 0, 0);\n    }"
    replace(old, new, "set_node_existing_no_payload", "actual_known_empty_value_action", "original empty dummy-value body has no payload store; action proves this branch was reached")
    # Original byte operations: memcpy owns relocation, never also count child fields.
    for old, name, replacement in (
        ("memset(offsets, SpanConstants::UnusedEntry, sizeof(offsets));", "span_offsets_initialize", 'QcaeFtHash::initialize<Node>("ft_hash/span_offsets_initialize", offsets, SpanConstants::UnusedEntry, sizeof(offsets));'),
        ("memcpy(&toEntry, &fromEntry, sizeof(Entry));", "span_relocate_entry", 'QcaeFtHash::copy<Node>("ft_hash/span_relocate_entry", &toEntry, &fromEntry, sizeof(Entry));'),
        ("memcpy(newEntries, entries, allocated * sizeof(Entry));", "span_storage_relocate_bytes", 'QcaeFtHash::copy<Node>("ft_hash/span_storage_relocate_bytes", newEntries, entries, allocated * sizeof(Entry));')):
        replace(old, replacement, name, "actual_bulk_argument_bytes", "original libc call once with original byte argument; replaces no capacity/allocator estimate")
    # Explicit non-libc aggregate copies/moves are bounded by the actual destination type.
    for old, name, expr in (
        ("new (&toEntry.node()) Node(std::move(fromEntry.node()));", "span_move_nonrelocatable", "sizeof(Node)"),
        ("new (&newEntries[i].node()) Node(std::move(entries[i].node()));", "span_grow_move_nonrelocatable", "sizeof(Node)"),
        ("new (newNode) Node(n);", "data_detach_node_copy", "sizeof(Node)"),
        ("new (newNode) Node(std::move(n));", "data_rehash_node_move", "sizeof(Node)")):
        after(old, expr, name, "actual_typed_aggregate_copy_upper_bound")
    # All owning Span persistent scalar/pointer writes in these insertion/growth paths.
    pattern = re.compile(r"(?m)^[ \t]*(?P<statement>(?P<lhs>(?:entries|allocated|nextFree|offsets\[[^\]\n]+\]|fromSpan\.offsets\[[^\]\n]+\]|fromSpan\.nextFree|fromEntry\.nextFree\(\)|newEntries\[i\]\.nextFree\(\)))\s*=(?!=)[^;{}]+;)")
    # Restrict to the Span definition; indexes/iterators remain borrowed descriptors.
    span_start = head.index("template<typename Node>\nstruct Span {")
    span_end = head.index("// QHash uses a power of two growth policy.", span_start)
    body = head[span_start:span_end]
    matches = list(pattern.finditer(body))
    for i, m in reversed(list(enumerate(matches, 1))):
        statement = m.group('statement')
        dest = m.group('lhs')
        name = f"span_owned_field:{i}"
        replacement = statement + f' QcaeFtHash::field<Node>("ft_hash/{name}", 2, sizeof({dest}));'
        body = body[:m.start('statement')] + replacement + body[m.end('statement'):]
        add_site(sites, prefix + name, "actual_owned_field_write_upper_bound", statement,
                 "original owning index/pointer field store once; original loop iterations observed individually")
    head = head[:span_start] + body + head[span_end:]
    # Default-member initializers execute for each actual constructor, not for capacity estimates.
    old='QcaeFtHash::initialize<Node>("ft_hash/span_offsets_initialize", offsets, SpanConstants::UnusedEntry, sizeof(offsets));'
    head=head.replace(old,old+'\n        QcaeFtHash::field<Node>("ft_hash/span_default_members", 2, sizeof(entries) + sizeof(allocated) + sizeof(nextFree));')
    add_site(sites,prefix+"span_default_members","actual_owned_field_initialization_upper_bound","Entry *entries = nullptr; unsigned char allocated = 0; unsigned char nextFree = 0;","one actual Span constructor: typed default-member destinations; offsets counted separately")
    # Data constructor initializer lists and original member stores.
    after("numBuckets = GrowthPolicy::bucketsForCapacity(reserve);", "sizeof(size) + sizeof(numBuckets) + sizeof(seed) + sizeof(spans) + sizeof(ref)", "data_default_members")
    old="Data(const Data &other) : size(other.size), numBuckets(other.numBuckets), seed(other.seed)\n    {"
    new=old+'\n        QcaeFtHash::field<Node>("ft_hash/data_copy_initializer", 1, sizeof(size) + sizeof(numBuckets) + sizeof(seed));\n        QcaeFtHash::field<Node>("ft_hash/data_copy_defaults", 2, sizeof(ref) + sizeof(spans));'
    replace(old,new,"data_copy_initializer","actual_typed_aggregate_copy_upper_bound","actual constructor member copies, not node relocation or span initialization")
    add_site(sites,prefix+"data_copy_defaults","actual_owned_field_write_upper_bound","ref={{1}}; spans=nullptr;","default member destinations not overridden by this initializer list")
    old="Data(const Data &other, size_t reserved) : size(other.size), seed(other.seed)\n    {"
    new=old+'\n        QcaeFtHash::field<Node>("ft_hash/data_reserved_initializer", 1, sizeof(size) + sizeof(seed));\n        QcaeFtHash::field<Node>("ft_hash/data_reserved_defaults", 2, sizeof(ref) + sizeof(numBuckets) + sizeof(spans));'
    replace(old,new,"data_reserved_initializer","actual_typed_aggregate_copy_upper_bound","actual copy constructor initialized fields, separate from its fresh-node copies")
    add_site(sites,prefix+"data_reserved_defaults","actual_owned_field_write_upper_bound","ref={{1}}; numBuckets=0; spans=nullptr;","only default-member destinations not overridden by this initializer list")
    data_start = head.index("struct Data\n{")
    data_end = head.index("template <typename Node>\nstruct iterator", data_start)
    body = head[data_start:data_end]
    pattern = re.compile(r"(?m)^[ \t]*(?P<statement>(?P<lhs>numBuckets|spans|seed|size)\s*=(?!=)[^;{}]+;)")
    for i, m in reversed(list(enumerate(pattern.finditer(body), 1))):
        statement = m.group('statement')
        dest = m.group('lhs')
        name = f"data_owned_field:{i}"
        replacement = statement + f' QcaeFtHash::field<Node>("ft_hash/{name}", 2, sizeof({dest}));'
        body = body[:m.start('statement')] + replacement + body[m.end('statement'):]
        add_site(sites, prefix + name, "actual_owned_field_write_upper_bound", statement,
                 "original persistent Data scalar/pointer store once; distinct from copied Node payload")
    old = "        ++size;"
    if body.count(old) != 1:
        raise ValueError('Data insertion size changed')
    body = body.replace(old, old + ' QcaeFtHash::field<Node>("ft_hash/data_insert_size", 2, sizeof(size));')
    add_site(sites, prefix + 'data_insert_size', "actual_owned_field_write_upper_bound", old.strip(),
             "actual insertion count field write, never an element-count byte estimate")
    head = head[:data_start] + body + head[data_end:]
    # A const-key overload constructs an owned local copy before Node creation.
    after("Key copy = key; // Needs to be explicit for MSVC 2019", "sizeof(Key)", "emplace_key_temporary", "actual_typed_aggregate_copy_upper_bound")
    # Outer owning pointer stores. Borrowed hash guard/iterators and refcount increments
    # are immutable handle/control bookkeeping, not deep copies of cache payload.
    old="inline void detach() { if (!d || d->ref.isShared()) d = Data::detached(d); }"
    new='inline void detach() { if (!d || d->ref.isShared()) { d = Data::detached(d); QcaeFtHash::field<Node>("ft_hash/hash_detach_pointer", 2, sizeof(d)); } }'
    replace(old,new,"hash_detach_pointer","actual_owned_field_write_upper_bound","original owning cache-data handle store once; no charge for shared guard or borrowed iterators")
    return '#include "' + str(helper) + '"\n' + head + tail


def official_headers() -> dict[str, bytes]:
    archive = BASE / "downloads/qtbase-everywhere-src-6.11.1.tar.xz"
    if sha(archive.read_bytes()) != ARCHIVE_SHA:
        raise ValueError("Official Qt archive changed")
    originals = {}
    with tarfile.open(archive) as archive_file:
        for relative in (QHASH, FT_HEADER):
            entry = archive_file.extractfile('qtbase-everywhere-src-6.11.1/' + relative)
            if entry is None:
                raise ValueError('Missing source ' + relative)
            originals[relative] = entry.read()
    return originals


def prepare() -> dict:
    if PREFIX.exists() or (SEVENTH / "qt-observer-manifest.json").exists():
        raise ValueError("Refusing to overwrite a prepared or frozen tranche7")
    originals = official_headers()
    frozen_hash = SIXTH / "qt-prefix/lib/QtCore.framework/Headers/qhash.h"
    if frozen_hash.read_bytes() != originals[QHASH]:
        raise ValueError('Frozen Qt hash header differs from official source')
    SOURCE.mkdir(parents=True, exist_ok=True)
    (SHADOW / 'QtCore').mkdir(parents=True, exist_ok=True)
    bridge = SECOND / "qtbase-everywhere-src-6.11.1/qcae-sdk-observer.hpp"
    helper = SEVENTH / "qcae-ft-hash-observer.hpp"
    helper.write_text(HELPER.replace('QCAE_BRIDGE_PATH', str(bridge)))
    manifest = json.loads((SIXTH / 'qt-observer-manifest.json').read_text())
    sites = manifest['sites']
    hash_after = transform_hash(originals[QHASH].decode(), sites, helper)
    (SHADOW / 'QtCore/qhash.h').write_text(hash_after)
    ft_source = (SIXTH / 'qt-ft-sources' / FT).read_text()
    insertions = (
        (1, 'glyph_data.insert(GlyphAndSubPixelPosition(index, subPixelPosition), glyph);', 'glyph_operation'),
        (2, 'set->setGlyphMissing(glyph);', 'missing_operation'),
    )
    for cache, original, name in insertions:
        previous = (f'do {{ {original} qcae_qt_sdk_observer_emit('
                    f'"{FT}:ft_hash_cache_growth_unknown:{cache}", 3, 0); }} while (false);')
        if ft_source.count(previous) != 1:
            raise ValueError('Previous actual unknown anchor changed: ' + name)
        replacement = (f'do {{ QcaeFtHash::Scope qcaeHashScope({cache}); '
                       f'{original} qcaeHashScope.finish(); }} while (false);')
        ft_source = ft_source.replace(previous, replacement)
        add_site(sites, 'ft_hash/' + name, 'actual_entry', original,
                 'parent operation admission only; children own all copy/write bytes')
        add_site(sites, 'ft_hash/' + name + '_complete', 'actual_conditional_coverage', original,
                 'only finished original call with exactly one observed compatible node action '
                 'and no limit/type gap; otherwise kind3')
    ft_source = '#include "' + str(helper) + '"\n' + ft_source
    output = SOURCE / FT
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(ft_source)
    ft_header = originals[FT_HEADER].decode()
    original = 'GlyphAndSubPixelPosition(glyph_t g, const QFixedPoint spp) : glyph(g), subPixelPosition(spp) {}'
    if ft_header.count(original) != 1:
        raise ValueError('Exact FT key constructor changed')
    replacement = (original[:-2] + '{ QcaeFtHash::record("ft_hash/glyph_key_constructor", 1, '
                   'sizeof(glyph) + 2 * sizeof(subPixelPosition)); }')
    ft_header = ft_header.replace(original, replacement)
    (output.parent / 'qfontengine_ft_p.h').write_text(ft_header)
    add_site(sites, 'ft_hash/glyph_key_constructor', 'actual_typed_aggregate_copy_upper_bound',
             original, 'original glyph scalar member and two distinct QFixedPoint owned copies: '
             'by-value spp and member initializer; original constructor once')
    for name in ('unseen_or_multiple_node_action', 'observer_byte_limit', 'unsupported_node_type'):
        add_site(sites, 'ft_hash/' + name, 'actual_reached_unknown', name,
                 'sticky per-operation failure; never replaced by known zero or lowered threshold')
    source_rows = {row['path']: row for row in manifest['sources']}
    changed_sources = (
        (QHASH, originals[QHASH], hash_after),
        (FT_HEADER, originals[FT_HEADER], ft_header),
        (FT, (SIXTH / 'qt-ft-sources' / FT).read_bytes(), ft_source),
    )
    for relative, data, after in changed_sources:
        source_rows[relative] = {
            **source_rows.get(relative, {}),
            'path': relative,
            'prior_frozen_sha256': sha(data),
            'instrumented_body_sha256': sha(after.encode()),
        }
    manifest.update(
        tranche=7, sdk_prefix=str(PREFIX), previous_prefix_modified=False,
        coverage_complete=False,
        sources=sorted(source_rows.values(), key=lambda row: row['path']),
        sites=sorted(sites, key=lambda row: row['site']),
    )
    manifest['scope'] += '; FT insertion-scoped actual compatible Node/Span/Data cache payload copies, writes and relocations'
    manifest['ft_hash_coverage'] = {
        'type_gate': 'glyph key trivially copyable standard-layout 12B with glyph/subPixelPosition '
                     '+ pointer value; missing-set unsigned4B key + empty value',
        'parent_child_rule': 'operation parents carry zero bytes; each actual original owning '
                             'destination/copy site emits once; memcpy relocation does not emit '
                             'per-field child stores',
        'remaining': 'unrelated hash types, limit exhaustion, unseen/multiple action are kind3; '
                     'font/FreeType internal and non-insertion cache paths remain unclosed',
        'abi_layout_changed': False,
    }
    core = (SIXTH / 'qt-ft-sources' / CORE).read_text()
    declaration = '#define QCAE_QT_SDK_MANIFEST '
    start = core.index(declaration)
    end = core.index('\n', start)
    compact = json.dumps(manifest, sort_keys=True, separators=(',', ':'))
    core_output = SOURCE / CORE
    core_output.parent.mkdir(parents=True, exist_ok=True)
    core_output.write_text(core[:start] + declaration + json.dumps(compact) + core[end:])
    (SEVENTH / 'qt-observer-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def compile_translation_unit(job: tuple[str, list[str]]) -> None:
    relative, arguments = job
    log_path = SEVENTH / ('compile-' + Path(relative).name + '.log')
    with log_path.open('w') as log:
        subprocess.run(arguments, cwd=SECOND / 'build', stdout=log,
                       stderr=subprocess.STDOUT, check=True)


def bind_local_framework(output: Path) -> None:
    details = subprocess.check_output(['otool', '-l', str(output)], text=True)
    rpaths = re.findall(r'cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset', details)
    for rpath in rpaths:
        if rpath.startswith(str(BASE)) or rpath == '/opt/homebrew/lib':
            subprocess.run(['install_name_tool', '-delete_rpath', rpath, str(output)], check=True)
    if '@loader_path/../../..' not in rpaths:
        subprocess.run(['install_name_tool', '-add_rpath', '@loader_path/../../..', str(output)],
                       check=True)


def build(manifest: dict) -> None:
    prior = json.loads((SIXTH / 'object-reuse.json').read_text())
    base_build = SECOND / 'build'
    objects = SEVENTH / 'qt-objects'
    objects.mkdir(exist_ok=True)
    jobs = []
    replacements = {}
    report = {
        'schema': 1,
        'manifest_sha256': sha((SEVENTH / 'qt-observer-manifest.json').read_bytes()),
        'prior_inventory_sha256': sha((SIXTH / 'object-reuse.json').read_bytes()),
        'actual_compile_commands': {}, 'actual_link_commands': {},
        'compiler': prior['compiler'], 'sdk_settings': prior['sdk_settings'],
        'previous_prefix_modified': False,
        'unchanged_ABI_flags_except_observer_header_search': True,
        'observer_headers': [
            {'path': str(path), 'sha256': sha(path.read_bytes())}
            for path in (SEVENTH / 'qcae-ft-hash-observer.hpp', SHADOW / 'QtCore/qhash.h',
                         SOURCE / FT_HEADER)
        ],
    }
    for relative in (CORE, FT):
        arguments = list(prior['actual_compile_commands'][relative])
        old_object = arguments[arguments.index('-o') + 1]
        output = objects / (Path(relative).name + '.o')
        arguments[arguments.index('-o') + 1] = str(output)
        arguments[arguments.index('-c') + 1] = str(SOURCE / relative)
        if relative == FT:
            arguments[1:1] = ['-I' + str(SHADOW)]
        if '-MF' in arguments:
            arguments[arguments.index('-MF') + 1] = str(output) + '.d'
        replacements[old_object] = str(output)
        report['actual_compile_commands'][relative] = arguments
        jobs.append((relative, arguments))
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile_translation_unit, jobs))
    shutil.copytree(SIXTH / 'qt-prefix', PREFIX, symlinks=True)
    for suffix in ('.cmake', '.pc', '.prl', '.pri'):
        for path in PREFIX.rglob('*' + suffix):
            metadata = path.read_text()
            changed = metadata.replace(str(SIXTH / 'qt-prefix'), str(PREFIX))
            if changed != metadata:
                path.write_text(changed)
    reused = set()
    for module, original in prior['actual_link_commands'].items():
        arguments = list(original)
        output = PREFIX / f'lib/Qt{module}.framework/Versions/A/Qt{module}'
        arguments[arguments.index('-o') + 1] = str(output)
        for index, value in enumerate(arguments):
            if value in replacements:
                arguments[index] = replacements[value]
            elif value.endswith(('.o', '.a')):
                path = Path(value)
                reused.add((path if path.is_absolute() else base_build / path).resolve())
            elif value.endswith('libharfbuzz.dylib'):
                arguments[index] = str(PREFIX / 'lib/libharfbuzz.dylib')
        report['actual_link_commands'][module] = arguments
        with (SEVENTH / ('link-' + module + '.log')).open('w') as log:
            subprocess.run(arguments, cwd=base_build, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        bind_local_framework(output)
    report['reused_objects'] = [
        {'path': str(path), 'sha256': sha(path.read_bytes())} for path in sorted(reused)
    ]
    report['recompiled_sources'] = [
        {'path': str(SOURCE / relative), 'sha256': sha((SOURCE / relative).read_bytes())}
        for relative in (CORE, FT)
    ]
    (SEVENTH / 'object-reuse.json').write_text(json.dumps(report, indent=2) + '\n')
    print('SDK7 linked', len(manifest['sites']), 'sites,', len(reused), 'reused objects')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', action='store_true')
    arguments = parser.parse_args()
    manifest = prepare()
    print('Prepared', len(manifest['sites']), 'sites; coverage remains false')
    if arguments.build:
        build(manifest)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
