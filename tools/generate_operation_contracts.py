#!/usr/bin/env python3
"""Generate deterministic typed wire adapters/metadata from explicit operation schemas.

This generator owns no document authority. IDs and field ordering come from the
schema, not traversal or registration order. No external Python packages required.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

TYPES = {
    'string': 'std::string',
    'entity_id': 'EntityId',
    'quantity': 'Quantity',
    'vector3_mm': 'std::array<double, 3>',
    'positive_uint32': 'std::uint32_t',
    'finite_number': 'double',
    'entity_id_array': 'std::vector<EntityId>',
    'boolean': 'bool',
    'uint64_string': 'std::uint64_t',
    'unsigned_integer': 'std::uint64_t',
    'enum': 'std::string',
}
# Named structured types: an object, a tagged union of objects, or an array of either. Fields
# name the type with "ref"; arrays hold their elements in std::vector, so a union may contain
# itself through an array but never by value.
STRUCTURED = {'object', 'union', 'array'}
FIELD_KEYS = {'field_id', 'name', 'type', 'units', 'optional', 'allow_empty', 'values', 'ref'}
# Catalog schemas must stay inside the MCP bridge's bounds (apps/mcp/bridge.py).
SCHEMA_DEPTH, SCHEMA_NODES = 16, 128
CONTEXT_REQUIRED = ('document', 'epoch', 'expected_revision', 'idempotency_key')
CONTEXT = (*CONTEXT_REQUIRED, 'expected_profile')
EFFECTS = {'read_only', 'document_write', 'preview', 'background_task', 'auxiliary_write'}
IDENTIFIER = re.compile(r'[A-Za-z][A-Za-z0-9_]*\Z')
# Wire names that are C++ keywords get a trailing underscore in generated code only.
CPP_KEYWORDS = {'and', 'bool', 'case', 'class', 'default', 'delete', 'double', 'enum', 'explicit', 'export',
                'false', 'float', 'for', 'friend', 'if', 'int', 'long', 'namespace', 'new', 'not', 'operator',
                'or', 'private', 'protected', 'public', 'register', 'return', 'short', 'signed', 'sizeof',
                'static', 'struct', 'switch', 'template', 'this', 'throw', 'true', 'try', 'typedef', 'typename',
                'union', 'unsigned', 'using', 'virtual', 'void', 'volatile', 'while', 'xor'}


def member(field: dict) -> str:
    return field['name'] + '_' if field['name'] in CPP_KEYWORDS else field['name']
WIRE_ID = re.compile(r'[a-z][a-z0-9_.]*\Z')


def literal(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON object key: ' + key)
        result[key] = value
    return result


class OperationSchemas(list):
    """Operations sorted by ID, plus the named structured types they reference."""

    def __init__(self, operations: list[dict], types: dict[str, dict]):
        super().__init__(operations)
        self.types = types


def load_fields(file: Path, fields: object) -> list[dict]:
    if not isinstance(fields, list):
        raise ValueError(f'{file}: fields must be a list (empty for no parameters)')
    field_ids, field_names = set(), set()
    for field in fields:
        if not isinstance(field, dict) or set(field) - FIELD_KEYS or not {'field_id', 'name', 'type'} <= set(field):
            raise ValueError(f'{file}: invalid input field keys')
        if type(field['field_id']) is not int or not 1 <= field['field_id'] <= 0xFFFFFFFF:
            raise ValueError(f'{file}: field IDs must be explicit positive uint32')
        if field['field_id'] in field_ids or field['name'] in field_names:
            raise ValueError(f'{file}: duplicate field identity')
        if not isinstance(field['name'], str) or not IDENTIFIER.fullmatch(field['name']):
            raise ValueError(f'{file}: invalid field identifier')
        if 'optional' in field and type(field['optional']) is not bool:
            raise ValueError(f'{file}: optional field flag must be boolean')
        if 'allow_empty' in field and (type(field['allow_empty']) is not bool or field['type'] not in {'string', 'entity_id_array', 'array'}):
            raise ValueError(f'{file}: allow_empty must be a boolean on a string, entity ID array or array')
        if field['type'] not in TYPES and field['type'] not in STRUCTURED:
            raise ValueError(f'{file}: unknown input type')
        if (field['type'] in STRUCTURED) != ('ref' in field):
            raise ValueError(f'{file}: object, union and array fields, and only they, name their type with ref')
        if 'ref' in field and (not isinstance(field['ref'], str) or not IDENTIFIER.fullmatch(field['ref'])):
            raise ValueError(f'{file}: invalid structured type reference')
        values = field.get('values')
        if (field['type'] == 'enum') != (values is not None):
            raise ValueError(f'{file}: values are required on enum fields and only there')
        if values is not None and (not isinstance(values, list) or not values or len(values) != len(set(values))
                                   or any(not isinstance(v, str) or not WIRE_ID.fullmatch(v) for v in values)):
            raise ValueError(f'{file}: enum values must be unique lowercase identifiers')
        units = field.get('units', [])
        if not isinstance(units, list) or any(not isinstance(unit, str) or not unit for unit in units) or len(units) != len(set(units)):
            raise ValueError(f'{file}: unit choices must be unique nonempty strings')
        if field['type'] == 'quantity' and not units:
            raise ValueError(f'{file}: quantity requires explicit allowed units')
        if field['type'] == 'vector3_mm' and units != ['mm']:
            raise ValueError(f'{file}: canonical coordinate vector must declare mm')
        if field['type'] not in {'quantity', 'vector3_mm', 'finite_number'} and units:
            raise ValueError(f'{file}: units attached to nonquantity field')
        field_ids.add(field['field_id'])
        field_names.add(field['name'])
    return sorted(fields, key=lambda field: field['field_id'])


def load_union(file: Path, type_name: str, tag: object, variants: object) -> dict:
    if not isinstance(tag, str) or not IDENTIFIER.fullmatch(tag):
        raise ValueError(f'{file}: invalid union tag field')
    if not isinstance(variants, list) or not variants:
        raise ValueError(f'{file}: a union needs a nonempty variant list')
    result, values = [], set()
    for variant in variants:
        if not isinstance(variant, dict) or set(variant) != {'value', 'type_name', 'fields'}:
            raise ValueError(f'{file}: union variants have exactly value, type_name and fields')
        if not isinstance(variant['value'], str) or not WIRE_ID.fullmatch(variant['value']) or variant['value'] in values:
            raise ValueError(f'{file}: union tag values must be unique lowercase identifiers')
        if not isinstance(variant['type_name'], str) or not IDENTIFIER.fullmatch(variant['type_name']):
            raise ValueError(f'{file}: invalid generated type name')
        fields = load_fields(file, variant['fields'])
        if any(field['name'] == tag for field in fields):
            raise ValueError(f'{file}: a union variant cannot redefine its tag field')
        values.add(variant['value'])
        result.append({'value': variant['value'], 'type_name': variant['type_name'], 'fields': fields})
    return {'type_name': type_name, 'kind': 'union', 'tag': tag, 'variants': result}


def load_type(file: Path, spec: object) -> dict:
    if not isinstance(spec, dict) or spec.get('kind') not in {'object', 'union'}:
        raise ValueError(f'{file}: named types are objects or unions')
    if not isinstance(spec.get('type_name'), str) or not IDENTIFIER.fullmatch(spec['type_name']):
        raise ValueError(f'{file}: invalid generated type name')
    if spec['kind'] == 'object':
        if set(spec) != {'type_name', 'kind', 'fields'}:
            raise ValueError(f'{file}: object types have exactly type_name, kind and fields')
        return {'type_name': spec['type_name'], 'kind': 'object', 'fields': load_fields(file, spec['fields'])}
    if set(spec) != {'type_name', 'kind', 'tag', 'variants'}:
        raise ValueError(f'{file}: union types have exactly type_name, kind, tag and variants')
    return load_union(file, spec['type_name'], spec['tag'], spec['variants'])


def by_value_dependencies(spec: dict) -> list[str]:
    if spec['kind'] == 'union':
        return [variant['type_name'] for variant in spec['variants']]
    return [field['ref'] for field in spec['fields'] if field['type'] in {'object', 'union'}]


def resolve_types(types: dict[str, dict], operations: list[dict]) -> list[str]:
    """Checks every reference and returns the struct definition order."""
    named = dict(types)
    for spec in types.values():
        for variant in spec.get('variants', []):
            named[variant['type_name']] = {'type_name': variant['type_name'], 'kind': 'variant',
                                           'tag': spec['tag'], 'value': variant['value'], 'fields': variant['fields']}
    holders = [op['fields'] for op in operations] + [spec['fields'] for spec in named.values() if 'fields' in spec]
    accepted = {'object': {'object'}, 'union': {'union'}, 'array': {'object', 'union'}}
    for fields in holders:
        for field in fields:
            if field['type'] in STRUCTURED and named.get(field['ref'], {}).get('kind') not in accepted[field['type']]:
                raise ValueError(f'unknown or mismatched structured type reference: {field["ref"]}')
    order, state = [], {}

    def visit(name: str) -> None:
        if state.get(name) == 'done':
            return
        if state.get(name) == 'active':
            raise ValueError(f'structured types contain each other by value: {name}')
        state[name] = 'active'
        for dependency in by_value_dependencies(named[name]):
            visit(dependency)
        state[name] = 'done'
        order.append(name)

    for name in sorted(named):
        visit(name)
    types.clear()
    types.update({name: named[name] for name in order})
    return order


def load_schemas(directory: Path) -> OperationSchemas:
    files = sorted(directory.glob('*.json'))
    if not files:
        raise ValueError('Operation schema directory is empty')
    operations, types = [], {}
    op_ids, schema_ids, type_names = set(), set(), set()

    def claim(file: Path, name: str) -> None:
        if name in type_names:
            raise ValueError(f'{file}: duplicate operation, schema or type identity')
        type_names.add(name)

    for file in files:
        schema = json.loads(file.read_text(encoding='utf-8'), object_pairs_hook=unique_object)
        if set(schema) - {'types'} != {'schema_version', 'status', 'operations'} or schema['schema_version'] != 1:
            raise ValueError(f'{file}: unsupported operation schema envelope')
        if not isinstance(schema['operations'], list) or not schema['operations']:
            raise ValueError(f'{file}: operations must be a nonempty list')
        if not isinstance(schema.get('types', []), list) or ('types' in schema and not schema['types']):
            raise ValueError(f'{file}: types must be a nonempty list when present')
        for spec in schema.get('types', []):
            loaded = load_type(file, spec)
            claim(file, loaded['type_name'])
            for variant in loaded.get('variants', []):
                claim(file, variant['type_name'])
            types[loaded['type_name']] = loaded
        for operation in schema['operations']:
            required = {'operation_id', 'version', 'schema_id', 'type_name', 'effect', 'context'}
            union_input = isinstance(operation, dict) and 'tag' in operation
            if set(operation) != required | ({'tag', 'variants'} if union_input else {'fields'}):
                raise ValueError(f'{file}: operation fields must be exactly {sorted(required | {"fields"})} '
                                 'or tag and variants in place of fields')
            op = dict(operation)
            for key in ('operation_id', 'schema_id'):
                if not isinstance(op[key], str) or not WIRE_ID.fullmatch(op[key]):
                    raise ValueError(f'{file}: invalid {key}')
            if not isinstance(op['type_name'], str) or not IDENTIFIER.fullmatch(op['type_name']):
                raise ValueError(f'{file}: invalid generated type name')
            if op['operation_id'] in op_ids or op['schema_id'] in schema_ids:
                raise ValueError(f'{file}: duplicate operation, schema or type identity')
            claim(file, op['type_name'])
            if type(op['version']) is not int or not 1 <= op['version'] <= 0xFFFFFFFF:
                raise ValueError(f'{file}: operation version must be a positive uint32')
            if op['effect'] not in EFFECTS or set(op['context']) - set(CONTEXT) or not set(CONTEXT_REQUIRED) <= set(op['context']):
                raise ValueError(f'{file}: unknown effect/context requirement')
            op['context'] = {**op['context'], 'expected_profile': op['context'].get('expected_profile', False)}
            if any(type(op['context'][flag]) is not bool for flag in CONTEXT):
                raise ValueError(f'{file}: context flags must be boolean')
            if union_input:
                # The whole parameter object is the union; its type is the operation's input type.
                union = load_union(file, op['type_name'], op.pop('tag'), op.pop('variants'))
                for variant in union['variants']:
                    claim(file, variant['type_name'])
                types[op['type_name']] = union
                op['fields'] = []
                op['union'] = op['type_name']
            else:
                op['fields'] = load_fields(file, op['fields'])
            operations.append(op)
            op_ids.add(op['operation_id'])
            schema_ids.add(op['schema_id'])
    resolve_types(types, operations)
    return OperationSchemas(sorted(operations, key=lambda op: op['operation_id']), types)


def cpp_type(field: dict) -> str:
    if field['type'] in {'object', 'union'}:
        base = field['ref']
    elif field['type'] == 'array':
        base = f'std::vector<{field["ref"]}>'
    else:
        base = TYPES[field['type']]
    return f'std::optional<{base}>' if field.get('optional', False) else base


def decode_lines(field: dict, prefix: str, path: str, indent: str) -> list[str]:
    """Decodes one field of `fields` into `<prefix><member>`; returns early on failure."""
    key = field['name']
    local = prefix + member(field)
    optional = field.get('optional', False)
    inner = indent + '    ' if optional else indent
    lines = []
    if optional:
        lines.append(f'{indent}{cpp_type(field)} {local};')
        lines.append(f'{indent}if (const auto found = fields.find({literal(key)}); found != fields.end()) {{')
        arguments = 'found->second'
        decoded = local + '_decoded'
    else:
        arguments = f'fields.at({literal(key)})'
        decoded = local
    if field['type'] in STRUCTURED:
        if field['type'] == 'array':
            call = (f'operations::structured_array<{field["ref"]}>({arguments}, {path}, '
                    f'{str(field.get("allow_empty", False)).lower()})')
        else:
            call = f'operations::structured_input({arguments}, {path}, std::type_identity<{field["ref"]}>{{}})'
        lines.extend([f'{inner}auto {decoded} = {call};', f'{inner}if (!{decoded}.ok()) {{',
                      f'{inner}    return {{{decoded}.status, std::nullopt, {decoded}.error}};', inner + '}'])
        if optional:
            lines.extend([f'{inner}{local} = std::move(*{decoded}.value);', indent + '}'])
        return lines
    if field['type'] == 'quantity':
        units = field['units']
        lines.append(f'{inner}static constexpr std::array<std::string_view, {len(units)}> {local.rstrip("_")}_units{{' + ', '.join(literal(unit) for unit in units) + '};')
        arguments += f', {local.rstrip("_")}_units'
    if field['type'] == 'enum':
        values = field['values']
        lines.append(f'{inner}static constexpr std::array<std::string_view, {len(values)}> {local.rstrip("_")}_values{{' + ', '.join(literal(v) for v in values) + '};')
        arguments += f', {local.rstrip("_")}_values'
    decoder = {'string': 'string_value', 'vector3_mm': 'vector3', 'enum': 'enum_value'}.get(field['type'], field['type'])
    arguments += f', {path}'
    if field['type'] in {'string', 'entity_id_array'} and field.get('allow_empty', False):
        arguments += ', true'
    lines.extend([f'{inner}const auto {decoded} = wire::{decoder}({arguments});',
                  f'{inner}if (!{decoded}.ok()) {{',
                  f'{inner}    return {{{decoded}.status, std::nullopt, {decoded}.error}};',
                  inner + '}'])
    if optional:
        lines.extend([f'{inner}{local} = *{decoded}.value;',
                      f'{inner}wire::observe_input_copy(*{local}, *{local});', indent + '}'])
    return lines


def aggregate_value(field: dict, prefix: str) -> str:
    local = prefix + member(field)
    structured = field['type'] in STRUCTURED
    if field.get('optional', False):
        return f'std::move({local})' if structured else local
    return f'std::move(*{local}.value)' if structured else f'*{local}.value'


def encode_lines(field: dict, access: str, indent: str) -> list[str]:
    key = field['name']
    kind = field['type']
    optional = field.get('optional', False)
    value = f'*{access}' if optional else access
    if kind == 'entity_id':
        expression = f'Value({value}.value)' if not optional else f'Value({access}->value)'
    elif kind in {'positive_uint32', 'unsigned_integer'}:
        expression = f'Value(static_cast<std::int64_t>({value}))'
    elif kind == 'uint64_string':
        expression = f'Value(std::to_string({value}))'
    elif kind in {'quantity', 'vector3_mm', 'entity_id_array'}:
        expression = f'wire::to_value({value})'
    elif kind in STRUCTURED:
        expression = f'operations::to_value({value})'
    else:
        expression = f'Value({value})'
    if optional:
        return [f'{indent}if ({access}) {{', f'{indent}    object.emplace({literal(key)}, {expression});',
                f'{indent}    wire::observe_key({literal(key)});', indent + '}']
    return [f'{indent}object.emplace({literal(key)}, {expression});',
            f'{indent}wire::observe_key({literal(key)});']


def moved_expression(field: dict) -> str:
    if field['type'] in STRUCTURED:
        return f'operations::structured_moved_bytes(input.{member(field)})'
    return f'wire::moved_bytes(input.{member(field)})'


def field_schema(field: dict, types: dict, stack: tuple) -> dict:
    kind = field['type']
    if kind in {'string', 'entity_id', 'uint64_string'}:
        schema = {'type': 'string'}
        if not field.get('allow_empty', False):
            schema['minLength'] = 1
    elif kind == 'enum':
        schema = {'type': 'string', 'enum': field['values']}
    elif kind == 'boolean':
        schema = {'type': 'boolean'}
    elif kind == 'finite_number':
        schema = {'type': 'number'}
    elif kind == 'positive_uint32':
        schema = {'type': 'integer', 'minimum': 1, 'maximum': 4294967295}
    elif kind == 'unsigned_integer':
        schema = {'type': 'integer', 'minimum': 0}
    elif kind == 'quantity':
        schema = {'type': 'object', 'properties': {'value': {'type': 'number'},
                                                   'unit': {'type': 'string', 'enum': field['units']}},
                  'required': ['value', 'unit'], 'additionalProperties': False}
    elif kind == 'vector3_mm':
        schema = {'type': 'array', 'items': {'type': 'number'}, 'minItems': 3, 'maxItems': 3}
    elif kind == 'entity_id_array':
        schema = {'type': 'array', 'items': {'type': 'string', 'minLength': 1}}
        if not field.get('allow_empty', False):
            schema['minItems'] = 1
    elif kind == 'array':
        schema = {'type': 'array', 'items': type_schema(field['ref'], types, stack)}
        if not field.get('allow_empty', False):
            schema['minItems'] = 1
    else:
        schema = type_schema(field['ref'], types, stack)
    return schema


def object_schema(fields: list[dict], types: dict, stack: tuple, tag: tuple[str, str] | None = None) -> dict:
    properties, required = {}, []
    if tag:
        properties[tag[0]] = {'type': 'string', 'enum': [tag[1]]}
        required.append(tag[0])
    for field in fields:
        properties[field['name']] = field_schema(field, types, stack)
        if not field.get('optional', False):
            required.append(field['name'])
    return {'type': 'object', 'properties': properties, 'required': required, 'additionalProperties': False}


def type_schema(name: str, types: dict, stack: tuple) -> dict:
    if name in stack:
        # The bridge schema has no references; a nested value of an enclosing type stays opaque
        # here and the engine's generated decoder checks it fully.
        return {'type': 'object', 'description': f'A nested {name}, with the same shape as the enclosing one.'}
    spec, stack = types[name], (*stack, name)
    if spec['kind'] == 'object':
        return object_schema(spec['fields'], types, stack)
    return {'type': 'object',
            'properties': {spec['tag']: {'type': 'string', 'enum': [v['value'] for v in spec['variants']]}},
            'required': [spec['tag']],
            'oneOf': [object_schema(v['fields'], types, stack, (spec['tag'], v['value'])) for v in spec['variants']]}


def parameters_schema(op: dict, types: dict) -> str:
    if op.get('union'):
        # The catalog root must be closed (additionalProperties false), so it declares every
        # variant's properties; oneOf keeps each variant's exact shape.
        schema = type_schema(op['union'], types, ())
        merged = dict(schema['properties'])
        for branch in schema['oneOf']:
            for key, value in branch['properties'].items():
                if key in merged and merged[key] != value:
                    value = {'description': f'Shape depends on {types[op["union"]]["tag"]}; see oneOf.'}
                merged[key] = value
        schema['properties'] = merged
        schema['additionalProperties'] = False
    else:
        schema = object_schema(op['fields'], types, ())
    pending, count = [(schema, 0)], 0
    while pending:
        node, depth = pending.pop()
        count += 1
        if depth > SCHEMA_DEPTH or count > SCHEMA_NODES:
            raise ValueError(f'{op["operation_id"]}: catalog parameter schema exceeds the MCP bridge bounds')
        pending.extend((child, depth + 1) for child in node.get('properties', {}).values())
        pending.extend((child, depth + 1) for child in node.get('oneOf', []))
        if 'items' in node:
            pending.append((node['items'], depth + 1))
    text = json.dumps(schema, separators=(',', ':'), ensure_ascii=True)
    return f'    static constexpr std::string_view parameters_schema = R"json({text})json";'


def render_structured(operations: list[dict], types: dict) -> list[str]:
    holders = [op['fields'] for op in operations] + [spec['fields'] for spec in types.values() if 'fields' in spec]
    elements = sorted({field['ref'] for fields in holders for field in fields if field['type'] == 'array'})
    lines = ['// Structured inputs: objects, tagged unions and arrays of them. Helpers live in this',
             '// namespace so the wire templates reach them through argument-dependent lookup.']
    lines.extend(f'struct {name};' for name in types)
    lines.append('')
    for name, spec in types.items():
        if spec['kind'] == 'union':
            alternatives = ', '.join(variant['type_name'] for variant in spec['variants'])
            lines.extend([f'struct {name} {{', f'    std::variant<{alternatives}> value{{}};', '};', ''])
        else:
            lines.append(f'struct {name} {{')
            lines.extend(f'    {cpp_type(field)} {member(field)}{{}};' for field in spec['fields'])
            lines.extend(['};', ''])
    for name in types:
        lines.extend([f'inline Result<{name}> structured_input(const Value& source, std::string_view where, std::type_identity<{name}>);',
                      f'inline Value to_value(const {name}& input);',
                      f'inline std::size_t dynamic_bytes(const {name}& input) noexcept;',
                      f'inline std::size_t moved_payload_bytes(const {name}& input) noexcept;',
                      f'inline std::size_t moved_bytes(const {name}& input) noexcept;'])
    for element in elements:
        lines.extend([f'inline Value to_value(const std::vector<{element}>& values);',
                      f'inline std::size_t dynamic_bytes(const std::vector<{element}>& values) noexcept;',
                      f'inline std::size_t moved_bytes(const std::vector<{element}>& values) noexcept;'])
    lines.extend(['',
                  'template <class T>',
                  'Result<std::vector<T>> structured_array(const Value& source, std::string_view where, bool allow_empty) {',
                  '    const auto* array = std::get_if<Value::Array>(&source.data);',
                  '    if (!array || (!allow_empty && array->empty())) {',
                  '        return {Status::failed, std::nullopt,',
                  '                Diagnostic{ErrorCode::invalid_input, allow_empty ? "Expected an array." : "Expected a nonempty array.", std::string(where)}};',
                  '    }',
                  '    std::vector<T> result;',
                  '    result.reserve(array->size());',
                  '    for (std::size_t index = 0; index < array->size(); ++index) {',
                  '        auto item = structured_input((*array)[index], std::string(where) + "[" + std::to_string(index) + "]", std::type_identity<T>{});',
                  '        if (!item.ok()) {',
                  '            return {item.status, std::nullopt, item.error};',
                  '        }',
                  '        result.push_back(std::move(*item.value));',
                  '    }',
                  '    return {Status::success, std::move(result), std::nullopt};',
                  '}',
                  '',
                  'template <class T> std::size_t structured_moved_bytes(const T& value) noexcept {',
                  '    using wire::moved_bytes;',
                  '    return moved_bytes(value);',
                  '}', ''])
    for name, spec in types.items():
        if spec['kind'] == 'union':
            tag = spec['tag']
            values = [variant['value'] for variant in spec['variants']]
            lines.extend([f'inline Result<{name}> structured_input(const Value& source, std::string_view where, std::type_identity<{name}>) {{',
                          f'    static constexpr std::array<std::string_view, {len(values)}> tags{{' + ', '.join(literal(v) for v in values) + '};',
                          '    const auto* object = std::get_if<Value::Object>(&source.data);',
                          '    if (!object) {',
                          '        return {Status::failed, std::nullopt, Diagnostic{ErrorCode::invalid_input, "Expected an object.", std::string(where)}};',
                          '    }',
                          f'    const auto found = object->find({literal(tag)});',
                          '    if (found == object->end()) {',
                          '        return {Status::needs_input, std::nullopt,',
                          f'                Diagnostic{{ErrorCode::missing_input, "Required input field is missing.", std::string(where) + {literal("." + tag)}}}}};',
                          '    }',
                          f'    const auto tag = wire::enum_value(found->second, tags, std::string(where) + {literal("." + tag)});',
                          '    if (!tag.ok()) {',
                          '        return {tag.status, std::nullopt, tag.error};',
                          '    }'])
            for variant in spec['variants']:
                lines.extend([f'    if (*tag.value == {literal(variant["value"])}) {{',
                              f'        auto variant = operations::structured_input(source, where, std::type_identity<{variant["type_name"]}>{{}});',
                              '        if (!variant.ok()) {',
                              '            return {variant.status, std::nullopt, variant.error};',
                              '        }',
                              f'        return {{Status::success, {name}{{std::move(*variant.value)}}, std::nullopt}};',
                              '    }'])
            lines.extend([f'    return {{Status::failed, std::nullopt, Diagnostic{{ErrorCode::invalid_input, "Unknown union tag.", std::string(where) + {literal("." + tag)}}}}};',
                          '}', '',
                          f'inline Value to_value(const {name}& input) {{',
                          '    return std::visit([](const auto& alternative) { return operations::to_value(alternative); }, input.value);',
                          '}', '',
                          f'inline std::size_t dynamic_bytes(const {name}& input) noexcept {{',
                          '    return std::visit([](const auto& alternative) noexcept { return dynamic_bytes(alternative); }, input.value);',
                          '}', '',
                          f'inline std::size_t moved_payload_bytes(const {name}& input) noexcept {{',
                          '    return std::visit([](const auto& alternative) noexcept { return moved_bytes(alternative); }, input.value);',
                          '}', '',
                          f'inline std::size_t moved_bytes(const {name}& input) noexcept {{',
                          '    return moved_payload_bytes(input);',
                          '}', ''])
            continue
        fields = spec['fields']
        tag = (spec['tag'], spec['value']) if spec['kind'] == 'variant' else None
        keys = ([tag[0]] if tag else []) + [field['name'] for field in fields]
        required = ([tag[0]] if tag else []) + [field['name'] for field in fields if not field.get('optional', False)]
        parameter = ' input' if fields else ''
        lines.extend([f'inline Result<{name}> structured_input(const Value& source, std::string_view where, std::type_identity<{name}>) {{',
                      f'    static constexpr std::array<std::string_view, {len(keys)}> allowed{{' + ', '.join(literal(k) for k in keys) + '};',
                      f'    static constexpr std::array<std::string_view, {len(required)}> required{{' + ', '.join(literal(k) for k in required) + '};',
                      '    const auto object = wire::object_fields(source, allowed, required, where);',
                      '    if (!object.ok()) {',
                      '        return {object.status, std::nullopt, object.error};',
                      '    }'])
        if fields:
            lines.append('    const auto& fields = **object.value;')
        for field in fields:
            lines.extend(decode_lines(field, 'f_', f'std::string(where) + {literal("." + field["name"])}', '    '))
        lines.extend([f'    {name} input{{' + ', '.join(aggregate_value(field, 'f_') for field in fields) + '};',
                      '    wire::observe_input_copy(input' + ''.join(', input.' + member(field) for field in fields) + ');',
                      '    return {Status::success, std::move(input), std::nullopt};',
                      '}', '',
                      f'inline Value to_value(const {name}&{parameter}) {{',
                      '    Value::Object object;'])
        if tag:
            lines.extend([f'    object.emplace({literal(tag[0])}, Value({literal(tag[1])}));',
                          f'    wire::observe_key({literal(tag[0])});'])
        for field in fields:
            lines.extend(encode_lines(field, 'input.' + member(field), '    '))
        lines.extend(['    return Value(std::move(object));', '}', '',
                      f'inline std::size_t dynamic_bytes(const {name}&{parameter}) noexcept {{'])
        if fields:
            lines.extend(['    using wire::dynamic_bytes;',
                          '    return ' + ' + '.join(f'dynamic_bytes(input.{member(field)})' for field in fields) + ';'])
        else:
            lines.append('    return 0;')
        lines.extend(['}', '', f'inline std::size_t moved_payload_bytes(const {name}&{parameter}) noexcept {{'])
        if fields:
            lines.extend(['    using wire::moved_bytes;',
                          '    return ' + ' + '.join(f'moved_bytes(input.{member(field)})' for field in fields) + ';'])
        else:
            lines.append('    return 0;')
        lines.extend(['}', '', f'inline std::size_t moved_bytes(const {name}& input) noexcept {{',
                      '    return moved_payload_bytes(input);', '}', ''])
    for element in elements:
        lines.extend([f'inline Value to_value(const std::vector<{element}>& values) {{',
                      '    Value::Array array;',
                      '    array.reserve(values.size());',
                      '    for (const auto& value : values)',
                      '        array.push_back(operations::to_value(value));',
                      '    return Value(std::move(array));',
                      '}', '',
                      f'inline std::size_t dynamic_bytes(const std::vector<{element}>& values) noexcept {{',
                      f'    std::size_t bytes = values.size() * sizeof({element});',
                      '    for (const auto& value : values)',
                      '        bytes += dynamic_bytes(value);',
                      '    return bytes;',
                      '}', '',
                      '// A moved vector transfers its buffer.',
                      f'inline std::size_t moved_bytes(const std::vector<{element}>&) noexcept {{',
                      '    return 0;',
                      '}', ''])
    return lines


def render(operations: list[dict]) -> str:
    types = getattr(operations, 'types', {})
    contracts = {'operations': list(operations), 'types': list(types.values())} if types else operations
    canonical = json.dumps(contracts, sort_keys=True, separators=(',', ':'), ensure_ascii=True)
    digest = hashlib.sha256(canonical.encode('utf-8')).hexdigest()
    lines = [
        '// Generated by tools/generate_operation_contracts.py; edit schemas/operations instead.',
        '#pragma once', '#include "qcae/operation_registry.hpp"',
        *(['#include <type_traits>', '#include <variant>'] if types else []), '',
        'namespace qcae::operations {',
        f'inline constexpr std::string_view operation_schema_digest = "{digest}";', '',
    ]
    if types:
        lines.extend(render_structured(operations, types))
    for op in operations:
        name = op['type_name']
        fields = op['fields']
        structured = bool(op.get('union')) or any(field['type'] in STRUCTURED for field in fields)
        if op.get('union'):
            spec = types[name]
            descriptors = [f'{{1, {literal(spec["tag"])}, "enum", {{}}, true, false, {{' + ', '.join(literal(v['value']) for v in spec['variants']) + '}}']
        else:
            lines.append(f'struct {name} {{')
            lines.extend(f'    {cpp_type(field)} {member(field)}{{}};' for field in fields)
            lines.append('};')
            lines.append('')
            descriptors = []
            for field in fields:
                units = ', '.join(literal(unit) for unit in field.get('units', []))
                choices = ', '.join(literal(value) for value in field.get('values', []))
                descriptors.append(f'{{{field["field_id"]}, {literal(field["name"])}, {literal(field["type"])}, {{{units}}}, {str(not field.get("optional", False)).lower()}, {str(field.get("allow_empty", False)).lower()}, {{{choices}}}}}')
        lines.extend([f'template <> struct InputTraits<{name}> {{',
                      f'    static constexpr std::string_view operation_id = {literal(op["operation_id"])};',
                      f'    static constexpr std::string_view schema_id = {literal(op["schema_id"])};',
                      '    static OperationDefinition definition() {',
                      f'        return {{{literal(op["operation_id"])}, {op["version"]}, {literal(op["schema_id"])},',
                      f'                OperationEffect::{op["effect"]},',
                      '                {' + ', '.join(str(op['context'][flag]).lower() for flag in CONTEXT) + '},',
                      '                {'])
        for index, descriptor in enumerate(descriptors):
            comma = ',' if index + 1 != len(descriptors) else ''
            lines.append(f'                    {descriptor}{comma}')
        lines.extend(['                }};', '    }', ''])
        if structured:
            lines.extend([parameters_schema(op, types), ''])
        if op.get('union'):
            lines.extend([f'    static Result<{name}> from_value(const Value& value) {{',
                          f'        return operations::structured_input(value, "input", std::type_identity<{name}>{{}});',
                          '    }', '',
                          f'    static Value to_value(const {name}& input) {{',
                          '        return operations::to_value(input);',
                          '    }', '',
                          f'    static void observe_copy(const {name}& input) noexcept {{',
                          '        wire::observe_input_copy(input, input);', '    }', '',
                          f'    static std::size_t moved_bytes(const {name}& input) noexcept {{',
                          '        return operations::structured_moved_bytes(input);', '    }', '};', ''])
            continue
        lines.extend([f'    static Result<{name}> from_value(const Value& value) {{',
                      f'        static constexpr std::array<std::string_view, {len(fields)}> allowed{{' + ', '.join(literal(f['name']) for f in fields) + '};',
                      f'        static constexpr std::array<std::string_view, {sum(not field.get("optional", False) for field in fields)}> required{{' + ', '.join(literal(field['name']) for field in fields if not field.get('optional', False)) + '};',
                      '        const auto object = wire::object_fields(value, allowed, required, "input");',
                      '        if (!object.ok()) {',
                      '            return {object.status, std::nullopt, object.error};',
                      '        }'])
        if fields:
            lines.append('        const auto& fields = **object.value;')
        for field in fields:
            lines.extend(decode_lines(field, '', literal('input.' + field['name']), '        '))
        lines.extend([f'        {name} input{{' + ', '.join(aggregate_value(field, '') for field in fields) + '};',
                      '        wire::observe_input_copy(input' + ''.join(', input.' + member(field) for field in fields) + ');',
                      '        return {Status::success, std::move(input), std::nullopt};',
                      '    }', '', f'    static Value to_value(const {name}&' + (' input' if fields else '') + ') {',
                      '        Value::Object object;'])
        for field in fields:
            lines.extend(encode_lines(field, 'input.' + member(field), '        '))
        lines.extend(['        return Value(std::move(object));', '    }', ''])
        # Copy accounting for the shared feature plan helpers; covers every schema field.
        members = ''.join(', input.' + member(field) for field in fields)
        moved = ' + '.join(moved_expression(field) for field in fields) or '0'
        lines.extend([f'    static void observe_copy(const {name}& input) noexcept {{',
                      f'        wire::observe_input_copy(input{members});', '    }', '',
                      f'    static std::size_t moved_bytes(const {name}&' + (' input' if fields else '') + ') noexcept {',
                      f'        return {moved};', '    }', '};', ''])
    lines.extend(['} // namespace qcae::operations', ''])
    return '\n'.join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('schema_directory', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        expected = render(load_schemas(args.schema_directory))
        if args.check:
            if not args.output.is_file() or args.output.read_text(encoding='utf-8') != expected:
                raise ValueError('Generated operation adapters differ; regenerate from schema')
            print('PASS: generated operation contracts are byte-identical')
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(expected, encoding='utf-8')
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('operation contract generation failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
