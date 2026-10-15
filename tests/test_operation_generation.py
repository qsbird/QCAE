#!/usr/bin/env python3
"""Exercise real CMake schema dependencies and generated parameterless inputs."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


# A recursive union through an array, an optional object array and a union-typed input.
STRUCTURED_PROGRAM = '''\
#include "qcae/operation_inputs.hpp"
using namespace qcae;
using namespace qcae::operations;
Value object(Value::Object fields) { return Value(std::move(fields)); }
Value leaf(const char* first) {
    return object({{"op", Value("leaf")}, {"range", object({{"first", Value(first)}})}});
}
int main() {
    const auto nested = object({{"node", object({{"op", Value("and")}, {"children", Value(Value::Array{
        leaf("1"), object({{"op", Value("and")}, {"children", Value(Value::Array{leaf("2")})}})})}})}});
    const auto good = InputTraits<TestEmptyInput>::from_value(nested);
    const auto& root = std::get<TestAnd>(good.value->node.value);
    const auto& inner = std::get<TestAnd>(root.children[1].value);
    const bool shape = good.ok() && root.children.size() == 2 &&
        std::get<TestLeaf>(inner.children[0].value).range.first == 2 && !good.value->ranges &&
        InputTraits<TestEmptyInput>::to_value(*good.value) == nested;
    const auto bad_tag = InputTraits<TestEmptyInput>::from_value(object({{"node", object({{"op", Value("and")},
        {"children", Value(Value::Array{object({{"op", Value("or")}})})}})}}));
    const auto missing = InputTraits<TestEmptyInput>::from_value(object({{"node", object({{"op", Value("leaf")}})}}));
    const auto extra = InputTraits<TestEmptyInput>::from_value(object({{"node", leaf("1")},
        {"ranges", Value(Value::Array{object({{"first", Value("1")}, {"x", Value(true)}})})}}));
    const auto empty_ranges = InputTraits<TestEmptyInput>::from_value(object({{"node", leaf("1")},
        {"ranges", Value(Value::Array{})}}));
    const auto rename = object({{"command", Value("rename")}, {"name", Value("beam")}});
    const auto command = InputTraits<TestCommandInput>::from_value(rename);
    const auto no_command = InputTraits<TestCommandInput>::from_value(object({{"name", Value("beam")}}));
    const auto wrong_variant = InputTraits<TestCommandInput>::from_value(
        object({{"command", Value("noop")}, {"name", Value("beam")}}));
    const auto fields = InputTraits<TestCommandInput>::definition().fields;
    return shape && bad_tag.error && bad_tag.error->field == "input.node.children[0].op" &&
        missing.status == Status::needs_input && missing.error->field == "input.node.range" &&
        extra.error && extra.error->field == "input.ranges[0].x" && !empty_ranges.ok() &&
        command.ok() && std::get<TestRename>(command.value->value).name == "beam" &&
        InputTraits<TestCommandInput>::to_value(*command.value) == rename &&
        no_command.status == Status::needs_input && no_command.error->field == "input.command" &&
        wrong_variant.error && wrong_variant.error->field == "input.name" &&
        fields.size() == 1 && fields[0].name == "command" &&
        fields[0].values == std::vector<std::string>{"rename", "noop"} ? 0 : 1;
}
'''


class OperationGeneration(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qcae-operation-generation-")
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name) / "source"
        self.build = Path(self.temporary.name) / "build"
        for directory in ("modules/operations", "modules/foundation/include",
                          "modules/contracts/include", "schemas/operations"):
            shutil.copytree(ROOT / directory, self.source / directory)
        (self.source / "tools").mkdir()
        shutil.copy2(ROOT / "tools/generate_operation_contracts.py", self.source / "tools")
        (self.source / "CMakeLists.txt").write_text('''\
cmake_minimum_required(VERSION 3.24)
project(OperationGeneration LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(QCAE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
function(qcae_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()
add_library(qcae_foundation INTERFACE)
target_include_directories(qcae_foundation INTERFACE modules/foundation/include)
add_library(qcae_contracts INTERFACE)
target_include_directories(qcae_contracts INTERFACE modules/contracts/include)
add_subdirectory(modules/operations)
''', encoding="utf-8")
        self.generator = ["-G", "Ninja"] if shutil.which("ninja") else []
        self.run_command("cmake", "-S", str(self.source), "-B", str(self.build), *self.generator)
        self.header = self.build / "generated/qcae/operation_inputs.hpp"

    def run_command(self, *arguments):
        result = subprocess.run(arguments, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def generate(self):
        self.run_command("cmake", "--build", str(self.build), "--target",
                         "qcae_generated_operation_inputs")
        return self.header.read_text(encoding="utf-8")

    def write_probe_schema(self, *, version=1):
        # Older Make implementations compare only seconds; model an observable source edit.
        if not self.generator:
            time.sleep(1.1)
        schema = {
            "schema_version": 1,
            "status": "test_only",
            "operations": [{
                "operation_id": "test.empty",
                "version": version,
                "schema_id": f"qcae.operation.test.empty.v{version}",
                "type_name": "TestEmptyInput",
                "effect": "read_only",
                "context": {"document": False, "epoch": False,
                            "expected_revision": False, "idempotency_key": False},
                "fields": [],
            }],
        }
        target = self.source / "schemas/operations/probe.json"
        target.write_text(json.dumps(schema), encoding="utf-8")
        return target

    def test_schema_add_modify_remove_regenerates_without_manual_configure(self):
        original = self.generate()
        probe = self.write_probe_schema()
        added = self.generate()
        self.assertNotEqual(added, original)
        self.assertIn("struct TestEmptyInput", added)
        self.write_probe_schema(version=2)
        modified = self.generate()
        self.assertTrue(modified != added, "modified schema must regenerate header")
        self.assertIn('"test.empty", 2', modified)
        if not self.generator:
            time.sleep(1.1)
        probe.unlink()
        self.assertEqual(self.generate(), original)
        timestamp = self.header.stat().st_mtime_ns
        self.assertEqual(self.generate(), original)
        self.assertEqual(self.header.stat().st_mtime_ns, timestamp,
                         "unchanged schemas should not regenerate")

    def test_allow_empty_requires_boolean_string_or_entity_array_schema(self):
        probe = self.write_probe_schema()
        for kind, allowed in (("entity_id_array", "true"), ("string", "true"),
                              ("entity_id", True), ("finite_number", True),
                              ("positive_uint32", False)):
            with self.subTest(kind=kind, allowed=allowed):
                schema = json.loads(probe.read_text())
                schema["operations"][0]["fields"] = [{"field_id": 1, "name": "items",
                                                     "type": kind, "allow_empty": allowed}]
                probe.write_text(json.dumps(schema))
                result = subprocess.run(["cmake", "--build", str(self.build), "--target",
                                         "qcae_generated_operation_inputs"],
                                        capture_output=True, text=True, check=False)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("allow_empty must be a boolean on a string, entity ID array or array",
                              result.stdout + result.stderr)

    def test_allow_empty_string_preserves_optional_presence_and_default_rejection(self):
        probe = self.write_probe_schema()
        schema = json.loads(probe.read_text())
        schema["operations"][0]["fields"] = [
            {"field_id": 1, "name": "title", "type": "string"},
            {"field_id": 2, "name": "path", "type": "string", "optional": True, "allow_empty": True},
            {"field_id": 3, "name": "ids", "type": "entity_id_array", "allow_empty": True},
        ]
        probe.write_text(json.dumps(schema))
        self.generate()
        (self.source / "empty_string.cpp").write_text('''\
#include "qcae/operation_inputs.hpp"
using namespace qcae;
using namespace qcae::operations;
int main() {
    const Value omitted(Value::Object{{"title", Value("Save")}, {"ids", Value(Value::Array{})}});
    const Value explicit_empty(Value::Object{{"title", Value("Save")}, {"path", Value("")},
                                            {"ids", Value(Value::Array{})}});
    const auto absent = InputTraits<TestEmptyInput>::from_value(omitted);
    const auto empty = InputTraits<TestEmptyInput>::from_value(explicit_empty);
    const auto bad_title = InputTraits<TestEmptyInput>::from_value(
        Value(Value::Object{{"title", Value("")}, {"ids", Value(Value::Array{})}}));
    const auto bad_path = InputTraits<TestEmptyInput>::from_value(
        Value(Value::Object{{"title", Value("Save")}, {"path", Value(true)},
                           {"ids", Value(Value::Array{})}}));
    const auto fields = InputTraits<TestEmptyInput>::definition().fields;
    return absent.ok() && !absent.value->path && empty.ok() && empty.value->path &&
        empty.value->path->empty() && empty.value->ids.empty() && !bad_title.ok() &&
        !bad_path.ok() && !fields[0].allow_empty && fields[1].allow_empty &&
        fields[2].allow_empty && InputTraits<TestEmptyInput>::to_value(*absent.value) == omitted &&
        InputTraits<TestEmptyInput>::to_value(*empty.value) == explicit_empty ? 0 : 1;
}
''', encoding="utf-8")
        with (self.source / "CMakeLists.txt").open("a", encoding="utf-8") as file:
            file.write('''\
add_executable(empty_string empty_string.cpp)
target_link_libraries(empty_string PRIVATE qcae_operations)
qcae_warnings(empty_string)
''')
        self.run_command("cmake", "-S", str(self.source), "-B", str(self.build))
        self.run_command("cmake", "--build", str(self.build), "--target", "empty_string")
        executable = self.build / ("empty_string.exe" if shutil.which("cl") else "empty_string")
        if not executable.is_file():
            executable = self.build / "Debug" / "empty_string.exe"
        self.run_command(str(executable))

    def test_boolean_uint64_string_unsigned_and_enum_fields(self):
        probe = self.write_probe_schema()
        schema = json.loads(probe.read_text())
        schema["operations"][0]["fields"] = [
            {"field_id": 1, "name": "visible", "type": "boolean"},
            {"field_id": 2, "name": "revision", "type": "uint64_string"},
            {"field_id": 3, "name": "offset", "type": "unsigned_integer", "optional": True},
            {"field_id": 4, "name": "operator", "type": "enum", "values": ["through", "visible_only"]},
        ]
        probe.write_text(json.dumps(schema))
        self.generate()
        (self.source / "typed_fields.cpp").write_text('''\
#include "qcae/operation_inputs.hpp"
#include <cstdint>
using namespace qcae;
using namespace qcae::operations;
Value input(Value visible, Value revision, Value op) {
    return Value(Value::Object{{"visible", std::move(visible)}, {"revision", std::move(revision)},
                               {"operator", std::move(op)}});
}
int main() {
    const auto good = InputTraits<TestEmptyInput>::from_value(
        input(Value(true), Value("18446744073709551615"), Value("visible_only")));
    const auto with_offset = InputTraits<TestEmptyInput>::from_value(Value(Value::Object{
        {"visible", Value(false)}, {"revision", Value("0")}, {"operator", Value("through")},
        {"offset", Value(std::int64_t{7})}}));
    const bool rejects =
        !InputTraits<TestEmptyInput>::from_value(input(Value("yes"), Value("1"), Value("through"))).ok() &&
        !InputTraits<TestEmptyInput>::from_value(input(Value(true), Value(std::int64_t{1}), Value("through"))).ok() &&
        !InputTraits<TestEmptyInput>::from_value(input(Value(true), Value("-1"), Value("through"))).ok() &&
        !InputTraits<TestEmptyInput>::from_value(
            input(Value(true), Value("18446744073709551616"), Value("through"))).ok() &&
        !InputTraits<TestEmptyInput>::from_value(input(Value(true), Value("1"), Value("all"))).ok() &&
        !InputTraits<TestEmptyInput>::from_value(Value(Value::Object{
            {"visible", Value(true)}, {"revision", Value("1")}, {"operator", Value("through")},
            {"offset", Value(std::int64_t{-1})}})).ok();
    const auto fields = InputTraits<TestEmptyInput>::definition().fields;
    return good.ok() && good.value->visible && good.value->revision == UINT64_MAX &&
        good.value->operator_ == "visible_only" && !good.value->offset && with_offset.ok() &&
        with_offset.value->offset == 7u && rejects &&
        fields[3].values == std::vector<std::string>{"through", "visible_only"} &&
        fields[0].values.empty() &&
        InputTraits<TestEmptyInput>::to_value(*good.value) ==
            input(Value(true), Value("18446744073709551615"), Value("visible_only")) ? 0 : 1;
}
''', encoding="utf-8")
        with (self.source / "CMakeLists.txt").open("a", encoding="utf-8") as file:
            file.write('''\
add_executable(typed_fields typed_fields.cpp)
target_link_libraries(typed_fields PRIVATE qcae_operations)
qcae_warnings(typed_fields)
''')
        self.run_command("cmake", "-S", str(self.source), "-B", str(self.build))
        self.run_command("cmake", "--build", str(self.build), "--target", "typed_fields")
        executable = self.build / ("typed_fields.exe" if shutil.which("cl") else "typed_fields")
        if not executable.is_file():
            executable = self.build / "Debug" / "typed_fields.exe"
        self.run_command(str(executable))
        for bad in ({"field_id": 1, "name": "mode", "type": "enum"},
                    {"field_id": 1, "name": "flag", "type": "boolean", "values": ["on"]}):
            schema["operations"][0]["fields"] = [bad]
            probe.write_text(json.dumps(schema))
            result = subprocess.run(["cmake", "--build", str(self.build), "--target",
                                     "qcae_generated_operation_inputs"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, bad)

    def test_structured_objects_recursive_unions_and_union_inputs(self):
        self.write_probe_schema()
        probe = self.source / "schemas/operations/probe.json"
        schema = json.loads(probe.read_text())
        context = schema["operations"][0]["context"]
        schema["types"] = [
            {"type_name": "TestRange", "kind": "object", "fields": [
                {"field_id": 1, "name": "first", "type": "uint64_string"},
                {"field_id": 2, "name": "label", "type": "string", "optional": True}]},
            {"type_name": "TestNode", "kind": "union", "tag": "op", "variants": [
                {"value": "leaf", "type_name": "TestLeaf", "fields": [
                    {"field_id": 1, "name": "range", "type": "object", "ref": "TestRange"}]},
                {"value": "and", "type_name": "TestAnd", "fields": [
                    {"field_id": 1, "name": "children", "type": "array", "ref": "TestNode",
                     "allow_empty": True}]}]},
        ]
        schema["operations"][0]["fields"] = [
            {"field_id": 1, "name": "node", "type": "union", "ref": "TestNode"},
            {"field_id": 2, "name": "ranges", "type": "array", "ref": "TestRange", "optional": True}]
        schema["operations"].append({
            "operation_id": "test.command", "version": 1, "schema_id": "qcae.operation.test.command.v1",
            "type_name": "TestCommandInput", "effect": "preview", "context": context, "tag": "command",
            "variants": [
                {"value": "rename", "type_name": "TestRename",
                 "fields": [{"field_id": 1, "name": "name", "type": "string"}]},
                {"value": "noop", "type_name": "TestNoop", "fields": []}]})
        probe.write_text(json.dumps(schema))
        header = self.generate()
        self.assertIn("static constexpr std::string_view parameters_schema", header)
        (self.source / "structured.cpp").write_text(STRUCTURED_PROGRAM, encoding="utf-8")
        with (self.source / "CMakeLists.txt").open("a", encoding="utf-8") as file:
            file.write("add_executable(structured structured.cpp)\n"
                       "target_link_libraries(structured PRIVATE qcae_operations)\n"
                       "qcae_warnings(structured)\n")
        self.run_command("cmake", "-S", str(self.source), "-B", str(self.build))
        self.run_command("cmake", "--build", str(self.build), "--target", "structured")
        executable = self.build / ("structured.exe" if shutil.which("cl") else "structured")
        if not executable.is_file():
            executable = self.build / "Debug" / "structured.exe"
        self.run_command(str(executable))
        cycle = json.loads(json.dumps(schema))
        cycle["types"][0]["fields"].append({"field_id": 3, "name": "node", "type": "union", "ref": "TestNode"})
        mismatched = json.loads(json.dumps(schema))
        mismatched["operations"][0]["fields"][0]["ref"] = "TestRange"
        unbounded = json.loads(json.dumps(schema))
        unbounded["operations"][1]["variants"] = [
            {"value": f"v{index}", "type_name": f"TestWide{index}",
             "fields": [{"field_id": 1, "name": "name", "type": "string"}]} for index in range(70)]
        for bad, message in ((cycle, "contain each other by value"), (mismatched, "mismatched structured type"),
                             (unbounded, "exceeds the MCP bridge bounds")):
            probe.write_text(json.dumps(bad))
            result = subprocess.run(["cmake", "--build", str(self.build), "--target",
                                     "qcae_generated_operation_inputs"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, message)
            self.assertIn(message, result.stdout + result.stderr)

    def test_parameterless_input_compiles_and_checks_object_shape(self):
        self.write_probe_schema()
        self.generate()
        (self.source / "empty.cpp").write_text('''\
#include "qcae/operation_inputs.hpp"
using namespace qcae;
using namespace qcae::operations;
int main() {
    const auto empty = InputTraits<TestEmptyInput>::from_value(Value{});
    const auto extra = InputTraits<TestEmptyInput>::from_value(
        Value(Value::Object{{"extra", Value(1)}}));
    const auto scalar = InputTraits<TestEmptyInput>::from_value(Value(true));
    return empty.ok() && !extra.ok() && !scalar.ok() &&
        InputTraits<TestEmptyInput>::to_value({}) == Value{} ? 0 : 1;
}
''', encoding="utf-8")
        with (self.source / "CMakeLists.txt").open("a", encoding="utf-8") as file:
            file.write('''\
add_executable(empty_input empty.cpp)
target_link_libraries(empty_input PRIVATE qcae_operations)
qcae_warnings(empty_input)
''')
        self.run_command("cmake", "-S", str(self.source), "-B", str(self.build))
        self.run_command("cmake", "--build", str(self.build), "--target", "empty_input")
        executable = self.build / ("empty_input.exe" if shutil.which("cl") else "empty_input")
        if not executable.is_file():
            executable = self.build / "Debug" / "empty_input.exe"
        self.run_command(str(executable))


if __name__ == "__main__":
    unittest.main(verbosity=2)
