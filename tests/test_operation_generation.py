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
                self.assertIn("allow_empty must be a boolean on a string or entity ID array",
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
