#!/usr/bin/env python3
"""Compile generated optional reference collections and reject invalid schema variants."""
from __future__ import annotations

import copy
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("entity_generator", ROOT / "tools/generate_entities.py")
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class OptionalReferenceGeneration(unittest.TestCase):
    def schema(self):
        return {"schema_version": 1, "reserved_type_ids": [], "entities": [
            {"id": 20480, "name": "Target", "version": 1, "fields": []},
            {"id": 20481, "name": "Collections", "version": 2, "fields": [
                {"id": 2, "name": "items", "kind": "references", "targets": ["Target"],
                 "optional": True, "introduced_version": 2},
                {"id": 3, "name": "pair", "kind": "references", "targets": ["Target"],
                 "optional": True, "count": 2, "introduced_version": 2}]}]}

    def test_optional_vector_and_array_descriptor_compile(self):
        with tempfile.TemporaryDirectory(prefix="qcae-entity-generation-") as folder:
            source = Path(folder)
            (source / "generated.hpp").write_text(GENERATOR.render(self.schema()))
            (source / "probe.cpp").write_text('''\
#include "generated.hpp"
qcae::RecordDescriptor generated_collections() {
    return qcae::RecordTraits<qcae::records::Collections>::descriptor();
}
''')
            (source / "CMakeLists.txt").write_text(f'''\
cmake_minimum_required(VERSION 3.24)
project(OptionalReferences LANGUAGES CXX)
add_library(probe OBJECT probe.cpp)
target_compile_features(probe PRIVATE cxx_std_20)
target_include_directories(probe PRIVATE "{ROOT / 'modules/document/include'}"
  "{ROOT / 'modules/foundation/include'}" "{ROOT / 'modules/contracts/include'}")
''')
            for command in (["cmake", "-S", str(source), "-B", str(source / "build")],
                            ["cmake", "--build", str(source / "build")]):
                result = subprocess.run(command, capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_optional_reference_schema_rejects_bad_targets_and_versions(self):
        for change in (lambda field: field.update(targets=["Missing"]),
                       lambda field: field.update(targets=[]),
                       lambda field: field.update(introduced_version=3)):
            with self.subTest(change=change):
                schema = copy.deepcopy(self.schema())
                change(schema["entities"][1]["fields"][0])
                with self.assertRaises(ValueError):
                    GENERATOR.render(schema)


if __name__ == "__main__":
    unittest.main(verbosity=2)
