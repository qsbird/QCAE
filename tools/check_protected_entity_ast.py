#!/usr/bin/env python3
"""Audit handwritten protected C++ type references using the installed Clang AST.

Requires existing libclang and its Python bindings; does not add a product
dependency. Compiler arguments come from the real configured Ninja build.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def arguments(command: dict, extra: list[str]) -> list[str]:
    original = shlex.split(command['command'])[1:]
    result = []
    index = 0
    while index < len(original):
        item = original[index]
        if item in ('-o', '-MF', '-MT', '-MQ'):
            index += 2
            continue
        if item not in ('-c', '-MD', '-MMD', '-MP', command['file']):
            result.append(item)
        index += 1
    return result + extra


def qualified(cursor) -> str:
    names = []
    while cursor is not None and cursor.spelling and cursor.kind.name != 'TRANSLATION_UNIT':
        names.append(cursor.spelling)
        cursor = cursor.semantic_parent
    return '::'.join(reversed(names))


def inspect(unit, protected: set[Path], entity_names: set[str], clang) -> tuple[list, set]:
    hits, visited = {}, set()

    def type_names(type_, depth=0):
        if depth > 16 or type_.kind == clang.TypeKind.INVALID:
            return set()
        canonical = type_.get_canonical()
        names = {qualified(canonical.get_declaration())}
        if canonical.kind in (clang.TypeKind.POINTER, clang.TypeKind.LVALUEREFERENCE,
                              clang.TypeKind.RVALUEREFERENCE):
            names.update(type_names(canonical.get_pointee(), depth + 1))
        if canonical.kind in (clang.TypeKind.CONSTANTARRAY, clang.TypeKind.INCOMPLETEARRAY):
            names.update(type_names(canonical.element_type, depth + 1))
        for index in range(max(0, canonical.get_num_template_arguments())):
            names.update(type_names(canonical.get_template_argument_type(index), depth + 1))
        return names

    def walk(cursor):
        file = Path(cursor.location.file.name).resolve() if cursor.location.file else None
        if file not in protected:
            # Includes have their own top-level cursors. Do not visit unrelated
            # headers or follow referenced declarations/template instances.
            return
        visited.add(file)
        if cursor.kind == clang.CursorKind.TYPE_REF and cursor.referenced:
            declaration = cursor.referenced
            names = {qualified(declaration)} | type_names(declaration.type)
            for name in names & entity_names:
                key = (str(file.relative_to(ROOT)), cursor.location.line,
                       cursor.location.column, name)
                hits[key] = {'file': key[0], 'line': key[1], 'column': key[2],
                             'entity': name, 'cursor_kind': 'TYPE_REF'}
        for child in cursor.get_children():
            walk(child)

    for cursor in unit.cursor.get_children():
        walk(cursor)
    return list(hits.values()), visited


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--protected-manifest', type=Path,
                        default=ROOT / 'docs/engineering/c4-protected-paths.json')
    parser.add_argument('--libclang', required=True, type=Path)
    parser.add_argument('--bindings-dir', required=True, type=Path)
    parser.add_argument('--clang-argument', action='append', default=[])
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(args.bindings_dir))
    from clang import cindex
    cindex.Config.set_library_file(str(args.libclang))
    manifest = json.loads(args.protected_manifest.read_text())
    protected = {(ROOT / path).resolve() for path in manifest['paths']
                 if Path(path).suffix in ('.cpp', '.hpp', '.h')}
    entity_names = set()
    for path in manifest['entity_schemas']:
        schema = json.loads((ROOT / path).read_text())
        entity_names.update(schema['namespace'] + '::' + entity['name']
                            for entity in schema['entities'])
    if not protected or not entity_names:
        raise ValueError('Protected paths and persistent type denominators must be nonempty')
    process = subprocess.run(['ninja', '-C', str(args.build_dir), '-t', 'compdb'],
                             capture_output=True, text=True, check=True)
    extra_sources = {(ROOT / path).resolve()
                     for path in manifest.get('additional_ast_translation_units', [])}
    commands = [command for command in json.loads(process.stdout)
                if Path(command['file']).resolve() in protected | extra_sources
                and Path(command['file']).suffix == '.cpp']
    expected_sources = {path for path in protected if path.suffix == '.cpp'} | extra_sources
    actual_sources = {Path(command['file']).resolve() for command in commands}
    errors = []
    if expected_sources != actual_sources:
        errors.append('Missing compiled protected sources: ' +
                      ', '.join(str(path.relative_to(ROOT))
                                for path in sorted(expected_sources - actual_sources)))
    index = cindex.Index.create()
    hits, visited, units = {}, set(), []
    for command in commands:
        unit = index.parse(command['file'], args=arguments(command, args.clang_argument))
        diagnostics = [str(item) for item in unit.diagnostics if item.severity >= 3]
        errors.extend(diagnostics)
        found, files = inspect(unit, protected, entity_names, cindex)
        for hit in found:
            hits[(hit['file'], hit['line'], hit['column'], hit['entity'])] = hit
        visited.update(files)
        units.append({'source': str(Path(command['file']).relative_to(ROOT)),
                      'sha256': sha(Path(command['file'])),
                      'diagnostics': diagnostics, 'type_reference_hits': len(found)})
    unvisited = protected - visited
    if unvisited:
        errors.append('Protected C++ files absent from original AST: ' +
                      ', '.join(str(path.relative_to(ROOT)) for path in sorted(unvisited)))
    self_test = None
    if args.self_test and commands:
        command = commands[0]
        source = Path(command['file'])
        addition = ('\n#include "qcae/records.hpp"\nnamespace qcae {\n'
                    'using SkeletonForbiddenProbe = records::Node;\n}\n')
        unit = index.parse(str(source), args=arguments(command, args.clang_argument),
                           unsaved_files=[(str(source), source.read_text() + addition)])
        diagnostics = [str(item) for item in unit.diagnostics if item.severity >= 3]
        found, _ = inspect(unit, protected, entity_names, cindex)
        probe = [item for item in found if item['entity'] == 'qcae::records::Node'
                 and item['line'] > len(source.read_text().splitlines())]
        self_test = {'injected_type': 'qcae::records::Node', 'source_file_modified': False,
                     'diagnostics': diagnostics, 'rejected': bool(probe) and not diagnostics}
        if not self_test['rejected']:
            errors.append('AST gate failed to reject the unsaved forbidden type reference')
    report = {'schema_version': 1, 'method': 'libclang original source cursor TYPE_REF',
              'template_policy': 'Visit source children only; never follow referenced declarations or instantiated specializations.',
              'protected_manifest_sha256': sha(args.protected_manifest),
              'entity_schema_sha256': {path: sha(ROOT / path) for path in manifest['entity_schemas']},
              'libclang_path': str(args.libclang), 'libclang_sha256': sha(args.libclang),
              'extra_compiler_arguments': args.clang_argument,
              'persistent_type_count': len(entity_names), 'translation_units': units,
              'direct_entity_type_references': len(hits), 'hits': list(hits.values()),
              'errors': errors, 'self_test': self_test,
              'passed': not errors and not hits}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'translation_units': len(units),
                      'direct_entity_type_references': len(hits), 'errors': errors}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
