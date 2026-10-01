#!/usr/bin/env python3
"""Generate typed records and codecs from explicit persistent schema IDs."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCHEMA = ROOT / "schemas/entities/entities.json"
DEFAULT_OUTPUT = ROOT / "modules/document/include/qcae/records.hpp"
KINDS = {
    "text": "std::string",
    "real": "double",
    "unsigned_integer": "std::uint64_t",
    "boolean": "bool",
    "vector3": "std::array<double, 3>",
    "profile": "ProfileRef",
    "target": "TargetBinding",
}
WIRE = {"unsigned_integer": "number", **{name: name for name in KINDS if name != "unsigned_integer"}}


def cpp_type(field: dict, qualified: bool = False) -> str:
    kind = field["kind"]
    if kind in ("reference", "references"):
        value = field.get("id_type", "EntityId")
        if qualified and value != "EntityId":
            value = "records::" + value
        if kind == "references":
            value = f"std::array<{value}, {field['count']}>" if "count" in field else f"std::vector<{value}>"
    else:
        value = KINDS[kind]
    return f"std::optional<{value}>" if field.get("optional") else value


def canonical(schema: dict) -> dict:
    schema = copy.deepcopy(schema)
    if schema.get("schema_version") != 1:
        raise ValueError("unsupported schema definition version")
    seen_ids, seen_names = set(schema.get("reserved_type_ids", [])), set()
    for entity in schema["entities"]:
        if (entity["id"] in seen_ids or entity["name"] in seen_names or
                not 0 < entity["id"] <= 0xFFFFFFFF or
                not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", entity["name"])):
            raise ValueError("duplicate or missing entity identity")
        seen_ids.add(entity["id"])
        seen_names.add(entity["name"])
        ids, names = {1, *entity.get("reserved_field_ids", [])}, {"id"}
        for field in entity["fields"]:
            if (field["id"] in ids or field["name"] in names or
                    not 0 < field["id"] <= 0xFFFFFFFF or
                    not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", field["name"])):
                raise ValueError("duplicate or reserved field identity")
            ids.add(field["id"])
            names.add(field["name"])
            cpp_type(field)
            if field.get("default") is not None:
                raise ValueError("optional fields currently support only the explicit null default")
            if field["kind"] in ("reference", "references") and not field.get("targets"):
                raise ValueError("reference field requires a declared target type")
            if field["kind"] in ("real", "vector3") and "unit" not in field:
                raise ValueError("numeric field requires an explicit unit")
            if field.get("unit") not in (None, "1", "mm", "N", "MPa", "mm2", "mm4"):
                raise ValueError("unknown schema unit")
            if field.get("introduced_version", 1) > entity["version"]:
                raise ValueError("field introduced after its record version")
        entity["fields"].sort(key=lambda field: field["id"])
    for entity in schema["entities"]:
        for field in entity["fields"]:
            if any(target not in seen_names for target in field.get("targets", [])):
                raise ValueError("reference names an unknown entity type")
    schema["entities"].sort(key=lambda entity: entity["id"])
    return schema


HELPERS = r'''
namespace records_detail {
inline void invalid(const char* message, const char* field) {
    throw RecordError(ErrorCode::invalid_input, message, field);
}
inline void check_text(std::string_view value, bool required, const char* field) {
    if ((required && value.empty()) || value.size() > 1024 ||
        std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
        invalid("Record text is missing, too long, or contains controls", field);
}
inline void check_profile(const ProfileRef& value, const char* field) {
    check_text(value.profile_id, true, field);
    check_text(value.profile_version, true, field);
    check_text(value.definition_digest, true, field);
}
template <class Range> void check_references(const Range& values, const char* field) {
    std::set<std::string_view> seen;
    for (const auto& value : values) {
        check_text(value.value, true, field);
        if (!seen.insert(value.value).second)
            invalid("Duplicate record reference", field);
    }
}
template <class Range> std::string pack_references(const Range& values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& value : values)
        result.push_back(value.value);
    return record_wire::strings(result);
}
template <class Range> Range unpack_references(std::string_view bytes) {
    const auto values = record_wire::read_strings(bytes);
    Range result;
    if constexpr (requires { result.reserve(values.size()); }) {
        result.reserve(values.size());
        for (const auto& value : values)
            result.emplace_back(value);
    } else {
        if (result.size() != values.size())
            invalid("Wrong reference cardinality", "references");
        for (std::size_t index = 0; index < values.size(); ++index)
            result[index] = typename Range::value_type(values[index]);
    }
    return result;
}
template <class T> std::size_t dynamic_bytes(const T&) { return 0; }
inline std::size_t dynamic_bytes(const std::string& value) { return value.size(); }
template <class Tag> std::size_t dynamic_bytes(const Id<Tag>& value) { return value.value.size(); }
inline std::size_t dynamic_bytes(const ProfileRef& value) {
    return value.profile_id.size() + value.profile_version.size() + value.definition_digest.size();
}
inline std::size_t dynamic_bytes(const TargetBinding& value) {
    return dynamic_bytes(value.profile) + value.analysis_kind.size();
}
template <class T> std::size_t dynamic_bytes(const std::optional<T>& value) {
    return value ? dynamic_bytes(*value) : 0;
}
template <class T, std::size_t N> std::size_t dynamic_bytes(const std::array<T, N>& values) {
    std::size_t result = 0;
    for (const auto& value : values) result += dynamic_bytes(value);
    return result;
}
template <class T> std::size_t dynamic_bytes(const std::vector<T>& values) {
    std::size_t result = values.size() * sizeof(T);
    for (const auto& value : values) result += dynamic_bytes(value);
    return result;
}
} // namespace records_detail
'''


def validator(entity: dict) -> list[str]:
    lines = ["    static void validate_fields(const Value& value) {",
             '        records_detail::check_text(value.id.value, true, "id");']
    for field in entity["fields"]:
        expression = f"value.{field['name']}"
        indent = "        "
        if field.get("optional"):
            lines.append(f"        if ({expression}) {{")
            expression = f"*{expression}"
            indent += "    "
        kind, name = field["kind"], field["name"]
        if kind in ("text", "reference"):
            actual = f"({expression}).value" if kind == "reference" else expression
            required = "true" if field.get("nonempty") or kind == "reference" else "false"
            lines.append(f'{indent}records_detail::check_text({actual}, {required}, "{name}");')
        if kind == "real":
            lines.append(f'{indent}if (!std::isfinite({expression})) records_detail::invalid("Number must be finite", "{name}");')
        if kind == "vector3":
            lines.append(f'{indent}for (double part : {expression}) if (!std::isfinite(part)) records_detail::invalid("Vector must be finite", "{name}");')
        if kind == "references":
            lines.append(f'{indent}records_detail::check_references({expression}, "{name}");')
            if field.get("nonempty"):
                lines.append(f'{indent}if (({expression}).empty()) records_detail::invalid("References are required", "{name}");')
        if kind in ("profile", "target"):
            actual = f"({expression}).profile" if kind == "target" else expression
            lines.append(f'{indent}records_detail::check_profile({actual}, "{name}");')
            if kind == "target":
                lines.append(f'{indent}records_detail::check_text(({expression}).analysis_kind, true, "{name}");')
        for key, comparison in (("minimum", "<"), ("maximum", ">"), ("exclusive_min", "<="), ("exclusive_max", ">=")):
            if key in field:
                lines.append(f'{indent}if ({expression} {comparison} {field[key]}) records_detail::invalid("Field is outside its allowed range", "{name}");')
        if "choices" in field:
            condition = " && ".join(f'{expression} != {json.dumps(choice)}' for choice in field["choices"])
            lines.append(f'{indent}if ({condition}) records_detail::invalid("Unknown field value", "{name}");')
        if field.get("optional"):
            lines.append("        }")
    lines.append("    }")
    return lines


def render(schema: dict) -> str:
    schema = canonical(schema)
    entities = schema["entities"]
    by_name = {entity["name"]: entity["id"] for entity in entities}
    digest = hashlib.sha256(json.dumps(schema, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    lines = ["// Generated by tools/generate_entities.py; do not edit.",
             f"// Semantic schema SHA-256: {digest}", "#pragma once", '#include "qcae/document_view.hpp"',
             "#include <algorithm>", "#include <cmath>", "#include <set>", "#include <type_traits>",
             "namespace qcae {", "namespace records {",
             "struct GeometryTag; struct MeshTag;",
             "using GeometryId = Id<GeometryTag>; using MeshId = Id<MeshTag>;"]
    for entity in entities:
        lines += [f"struct {entity['name']} {{", f"    {entity.get('identity_type', 'EntityId')} id{{}};"]
        lines += [f"    {cpp_type(field)} {field['name']}" +
                  (";" if field["kind"] == "references" and "count" in field else "{};")
                  for field in entity["fields"]]
        lines += [f"    bool operator==(const {entity['name']}&) const = default;", "};"]
        if entity.get("custom_rule"):
            lines.append(f"void validate_record(const {entity['name']}&, const DocumentView&);")
    lines += ["void validate_relations(const DocumentView&);", "} // namespace records", HELPERS]
    for entity in entities:
        name, fields = entity["name"], entity["fields"]
        lines += [f"template <> struct RecordTraits<records::{name}> {{", f"    using Value = records::{name};",
                  f"    static constexpr RecordTypeId type_id{{{entity['id']}}};",
                  "    inline static const unsigned char token_value{};",
                  "    static const void* token() noexcept { return &token_value; }",
                  "    static const std::string& identity(const Value& value) noexcept { return value.id.value; }"]
        lines += validator(entity)
        lines += ["    static RecordDescriptor descriptor() {", "        RecordDescriptor result;",
                  "        result.type = type_id;", f'        result.name = "{name}";',
                  f'        result.query_kind = "{entity.get("query_kind", "")}";',
                  (f'        result.display_name = [](const void* object) -> std::string_view {{ return static_cast<const Value*>(object)->{entity["display_field"]}; }};'
                   if entity.get("display_field") else
                   '        result.display_name = [](const void*) -> std::string_view { return {}; };'),
                  f"        result.current_version = {entity['version']};", "        result.cpp_type_token = token();",
                  f"        result.maximum_encoded_bytes = {entity.get('maximum_encoded_bytes', 16 * 1024 * 1024)};",
                  "        result.fields = {"]
        for field in fields:
            targets = ", ".join(f"RecordTypeId{{{by_name[target]}}}" for target in field.get("targets", []))
            lines.append(f'            {{RecordFieldId{{{field["id"]}}}, "{field["name"]}", RecordFieldKind::{field["kind"]}, '
                         f'{str(field.get("optional", False)).lower()}, "{field.get("unit", "")}", '
                         f'{{{targets}}}, {field.get("introduced_version", 1)}}},')
        lines += ["        };", "        result.encode = [](const void* object) {",
                  "            const auto& value = *static_cast<const Value*>(object);",
                  "            validate_fields(value);", f"            RecordInput input{{{{type_id, value.id.value}}, {entity['version']}, {{}}}};"]
        for field in fields:
            expression = f"value.{field['name']}"
            if field.get("optional"):
                lines.append(f"            if ({expression}) {{")
                expression = "*" + expression
            kind = field["kind"]
            if kind == "reference":
                payload = f"record_wire::text(({expression}).value)"
            elif kind == "references":
                payload = f"records_detail::pack_references({expression})"
            else:
                payload = f"record_wire::{WIRE[kind]}({expression})"
            lines.append(f'            input.fields.push_back({{RecordFieldId{{{field["id"]}}}, RecordFieldKind::{kind}, "{field.get("unit", "")}", {payload}}});')
            if field.get("optional"):
                lines.append("            }")
        lines += ["            return input;", "        };", "        result.decode = [](const RecordInput& input) -> std::shared_ptr<const void> {",
                  "            Value value;", "            value.id = decltype(value.id)(input.key.identity);"]
        for field in fields:
            name = field["name"]
            getter = "find" if field.get("optional") else "require"
            if field.get("optional"):
                lines.append(f"            if (const auto* field = record_wire::{getter}(input, RecordFieldId{{{field['id']}}})) {{")
                payload = "field->payload"
            else:
                payload = f"record_wire::require(input, RecordFieldId{{{field['id']}}}).payload"
            kind = field["kind"]
            if kind == "reference":
                id_type = field.get("id_type", "EntityId")
                if id_type != "EntityId":
                    id_type = "records::" + id_type
                expression = f"{id_type}(record_wire::read_text({payload}))"
            elif kind == "references":
                value_field = {**field, "optional": False}
                expression = f"records_detail::unpack_references<{cpp_type(value_field, True)}>({payload})"
            else:
                expression = f"record_wire::read_{WIRE[kind]}({payload})"
            lines.append(f"            value.{name} = {expression};")
            if field.get("optional"):
                lines.append("            }")
        lines += ["            validate_fields(value);", "            return std::make_shared<const Value>(std::move(value));", "        };",
                  "        result.owned_bytes = [](const void* object) {", "            const auto& value = *static_cast<const Value*>(object);",
                  "            return sizeof(Value) + records_detail::dynamic_bytes(value.id)"]
        lines += [f"                + records_detail::dynamic_bytes(value.{field['name']})" for field in fields]
        lines += ["                ;", "        };", "        result.references = [](const void* object, const RecordReferenceVisitor& visitor) {"]
        references = [field for field in fields if field.get("targets")]
        if references:
            lines.append("            const auto& value = *static_cast<const Value*>(object);")
        else:
            lines.append("            (void)object; (void)visitor;")
        for field in references:
            name, number = field["name"], field["id"]
            targets = ", ".join(f"RecordTypeId{{{by_name[target]}}}" for target in field["targets"])
            lines.append(f"            static constexpr std::array targets_{number}{{{targets}}};")
            if field["kind"] == "references":
                expression = f"*value.{name}" if field.get("optional") else f"value.{name}"
                if field.get("optional"):
                    lines.append(f"            if (value.{name}) {{")
                lines.append(f"            for (const auto& item : {expression}) visitor(RecordFieldId{{{number}}}, item.value, targets_{number});")
                if field.get("optional"):
                    lines.append("            }")
            elif field.get("optional"):
                lines.append(f"            if (value.{name}) visitor(RecordFieldId{{{number}}}, value.{name}->value, targets_{number});")
            else:
                lines.append(f"            visitor(RecordFieldId{{{number}}}, value.{name}.value, targets_{number});")
        lines += ["        };", "        result.validate = [](const void* object, const DocumentView& view) {",
                  "            const auto& value = *static_cast<const Value*>(object);", "            validate_fields(value);"]
        lines.append("            records::validate_record(value, view);" if entity.get("custom_rule") else "            (void)view;")
        lines += ["        };", "        return result;", "    }", "};"]
    lines += ["inline std::vector<RecordDescriptor> generated_record_descriptors() {", "    return {"]
    lines += [f"        RecordTraits<records::{entity['name']}>::descriptor()," for entity in entities]
    lines += ["    };", "}", "std::shared_ptr<const RecordRegistry> make_record_registry();", "} // namespace qcae", ""]
    return "\n".join(lines)


def formatter() -> str:
    candidate = shutil.which("clang-format-21")
    if not candidate:
        result = subprocess.run(["xcrun", "--find", "clang-format"], capture_output=True, text=True, check=False)
        candidate = result.stdout.strip() if result.returncode == 0 else shutil.which("clang-format")
    if not candidate:
        raise ValueError("clang-format 21 is required for deterministic generated formatting")
    version = subprocess.check_output([candidate, "--version"], text=True)
    if "clang-format version 21." not in version:
        raise ValueError("clang-format 21 is required")
    return candidate


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--permutation-check", action="store_true")
    args = parser.parse_args()
    schema = json.loads(args.schema.read_text())
    source = render(schema)
    if args.permutation_check:
        for seed in range(10):
            altered = copy.deepcopy(schema)
            rng = random.Random(seed)
            rng.shuffle(altered["entities"])
            for entity in altered["entities"]:
                rng.shuffle(entity["fields"])
            if render(altered) != source:
                raise ValueError(f"registration-order generation changed for seed {seed}")
    output = subprocess.run([formatter(), f"--assume-filename={args.output}"], input=source,
                            capture_output=True, text=True, check=True).stdout
    if args.check:
        if not args.output.exists() or args.output.read_text() != output:
            print(f"Generated entity output differs: {args.output}", file=sys.stderr)
            return 1
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print("PASS: typed entity generation" + ("; ten registration permutations identical" if args.permutation_check else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print(f"Entity generation failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
