#!/usr/bin/env python3
"""Scaffold a typed document-write operation in an existing feature.

Adds the schema entry, a plan stub and its registration in one step. Until the TODOs are done the
operation is listed as unavailable and its stub refuses, so a half-finished operation is never
offered to clients and never commits. The recipe is docs/engineering/adding-operations.md.

Example:
  python3 tools/new_operation.py --feature materials --schema schemas/operations/basic.json \\
      --operation material.set_name --type MaterialSetNameInput \\
      --field entity_id:entity_id --field name:string --label "Rename material"
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WRITE_CONTEXT = ('document', 'epoch', 'expected_revision', 'idempotency_key')
REGISTER_DECLARATION = '[[nodiscard]] Result<bool> register_handlers('
REGISTER_DEFINITION = 'Result<bool> register_handlers('
REGISTER_END = '    return registered;\n}'


class ScaffoldError(ValueError):
    pass


def generator(root: Path):
    sys.dont_write_bytecode = True  # leave no __pycache__ beside the generator
    spec = importlib.util.spec_from_file_location(
        'generate_operation_contracts', root / 'tools/generate_operation_contracts.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_field(text: str, field_id: int, types: dict) -> dict:
    """NAME:TYPE[:optional][:units=a,b]"""
    name, kind, *options = text.split(':')
    field = {'field_id': field_id, 'name': name, 'type': kind}
    if kind not in types:
        raise ScaffoldError(f'{text}: type must be one of {", ".join(sorted(types))}')
    for option in options:
        if option == 'optional':
            field['optional'] = True
        elif option.startswith('units='):
            field['units'] = option.removeprefix('units=').split(',')
        else:
            raise ScaffoldError(f'{text}: unknown option {option!r}')
    return field


def find_file(directory: Path, marker: str) -> Path:
    matches = [path for path in sorted(directory.rglob('*') if directory.is_dir() else [])
               if path.suffix in {'.hpp', '.cpp'} and marker in path.read_text(encoding='utf-8')]
    if len(matches) != 1:
        raise ScaffoldError(f'{directory}: expected one file containing {marker!r}, found {len(matches)}')
    return matches[0]


def insert_once(text: str, anchor: str, addition: str, path: Path) -> str:
    if text.count(anchor) != 1:
        raise ScaffoldError(f'{path}: expected exactly one {anchor!r}')
    return text.replace(anchor, addition + anchor)


def declaration(function: str, type_name: str) -> str:
    line = f'[[nodiscard]] Result<OperationPlan> {function}(const operations::{type_name}&);\n'
    if len(line) <= 101:
        return line
    return f'[[nodiscard]] Result<OperationPlan>\n{function}(const operations::{type_name}&);\n'


def stub(function: str, type_name: str, operation: str, label: str) -> str:
    signature = f'Result<OperationPlan> {function}(const {type_name}& input) {{'
    if len(signature) > 100:
        signature = f'Result<OperationPlan>\n{function}(const {type_name}& input) {{'
    return f'''{signature}
    // TODO(new_operation): build the plan as the materials feature does (canonical_copy,
    // record_plan, ChangeSummary); see docs/engineering/adding-operations.md.
    // Undo history label: "{label}".
    // Until then the operation is listed as unavailable and this stub refuses.
    (void)input;
    constexpr auto message = "{operation} is not implemented";
    return {{Status::failed,
            std::nullopt,
            Diagnostic{{ErrorCode::unsupported_capability, message, ""}}}};
}}

'''


def scaffold(arguments, root: Path) -> list[Path]:
    contracts = generator(root)
    if not contracts.WIRE_ID.fullmatch(arguments.operation) or '.' not in arguments.operation:
        raise ScaffoldError('--operation must look like entity.verb, e.g. material.set_name')
    if not contracts.IDENTIFIER.fullmatch(arguments.type) or not arguments.type.endswith('Input'):
        raise ScaffoldError('--type must be a C++ identifier ending in Input')
    function = arguments.function or 'prepare_' + arguments.operation.replace('.', '_')
    if not contracts.IDENTIFIER.fullmatch(function):
        raise ScaffoldError('--function must be a C++ identifier')
    if not 0 < len(arguments.label) <= 60 or '"' in arguments.label or '\\' in arguments.label:
        raise ScaffoldError('--label must be 1-60 characters without quotes or backslashes')
    fields = [parse_field(text, index, contracts.TYPES) for index, text in enumerate(arguments.field, 1)]

    schema_path = root / arguments.schema
    schema_text = schema_path.read_text(encoding='utf-8')
    schema = json.loads(schema_text)
    if json.dumps(schema, indent=2, ensure_ascii=False) + '\n' != schema_text:
        raise ScaffoldError(f'{schema_path}: not in canonical two-space JSON form; edit it by hand')
    context = {flag: True for flag in WRITE_CONTEXT}
    if arguments.requires_profile:
        context['expected_profile'] = True
    schema['operations'].append({
        'operation_id': arguments.operation,
        'version': 1,
        'schema_id': f'qcae.operation.{arguments.operation}.v1',
        'type_name': arguments.type,
        'effect': 'document_write',
        'context': context,
        'fields': fields,
    })
    new_schema = json.dumps(schema, indent=2, ensure_ascii=False) + '\n'
    with tempfile.TemporaryDirectory() as scratch:
        for path in (root / 'schemas/operations').glob('*.json'):
            shutil.copy(path, scratch)
        (Path(scratch) / schema_path.name).write_text(new_schema, encoding='utf-8')
        contracts.render(contracts.load_schemas(Path(scratch)))

    feature = root / 'features' / arguments.feature
    header = find_file(feature / 'include', REGISTER_DECLARATION)
    source = find_file(feature / 'src', REGISTER_DEFINITION)
    source_text = source.read_text(encoding='utf-8')
    if re.search(rf'\b{re.escape(function)}\(', source_text):
        raise ScaffoldError(f'{source}: {function} already exists; pass --function')
    registration = source_text[source_text.index(REGISTER_DEFINITION):]
    if REGISTER_END not in registration:
        raise ScaffoldError(f'{source}: register_handlers must end with {REGISTER_END!r}')
    source_text = insert_once(source_text, REGISTER_DEFINITION,
                              stub(function, arguments.type, arguments.operation, arguments.label),
                              source)
    source_text = insert_once(source_text, REGISTER_END,
                              f'    // TODO(new_operation): switch to register_plan once the plan exists.\n'
                              f'    if (registered.ok())\n'
                              f'        registered = declare_unimplemented<{arguments.type}>(registry);\n',
                              source)
    header_text = insert_once(header.read_text(encoding='utf-8'), REGISTER_DECLARATION,
                              declaration(function, arguments.type), header)
    if not arguments.dry_run:
        schema_path.write_text(new_schema, encoding='utf-8')
        header.write_text(header_text, encoding='utf-8')
        source.write_text(source_text, encoding='utf-8')
    return [schema_path, header, source]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--feature', required=True, help='directory under features/, e.g. materials')
    parser.add_argument('--schema', required=True, help='schema file, e.g. schemas/operations/basic.json')
    parser.add_argument('--operation', required=True, help='wire ID, e.g. material.set_name')
    parser.add_argument('--type', required=True, help='generated input type, e.g. MaterialSetNameInput')
    parser.add_argument('--field', action='append', default=[], help='NAME:TYPE[:optional][:units=a,b]')
    parser.add_argument('--label', required=True, help='undo history label, e.g. "Rename material"')
    parser.add_argument('--function', help='plan function name (default prepare_<operation>)')
    parser.add_argument('--requires-profile', action='store_true', help='require expected_profile context')
    parser.add_argument('--dry-run', action='store_true', help='validate without writing files')
    arguments = parser.parse_args()
    try:
        changed = scaffold(arguments, arguments.root.resolve())
    except (ValueError, OSError, KeyError) as error:
        print(f'new_operation: {error}', file=sys.stderr)
        return 1
    verb = 'Would change' if arguments.dry_run else 'Changed'
    print(verb + ': ' + ', '.join(str(path.relative_to(arguments.root.resolve())) for path in changed))
    if not arguments.dry_run:
        print('Next: implement the plan stub, switch its registration to register_plan, add tests, '
              'then build and run CTest (docs/engineering/adding-operations.md).')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
