#!/usr/bin/env python3
"""Check tools/new_operation.py against copies of the real features and schemas."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FEATURES = ("materials", "mesh_editing", "analysis")
MATERIAL = ["--feature", "materials",
            "--operation", "material.set_name", "--type", "MaterialSetNameInput",
            "--field", "entity_id:entity_id", "--field", "name:string",
            "--label", "Rename material"]


class NewOperation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qcae-new-operation-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for feature in FEATURES:
            shutil.copytree(ROOT / "features" / feature, self.root / "features" / feature)
        shutil.copytree(ROOT / "schemas/operations", self.root / "schemas/operations")
        (self.root / "tools").mkdir()
        for tool in ("new_operation.py", "generate_operation_contracts.py"):
            shutil.copy2(ROOT / "tools" / tool, self.root / "tools")

    def run_tool(self, *arguments):
        return subprocess.run([sys.executable, str(self.root / "tools/new_operation.py"),
                               "--root", str(self.root), *arguments],
                              capture_output=True, text=True, check=False)

    def snapshot(self):
        return {path: path.read_bytes() for path in sorted(self.root.rglob("*")) if path.is_file()}

    def test_material_operation_is_scaffolded_and_listed_unavailable(self):
        result = self.run_tool(*MATERIAL)
        self.assertEqual(result.returncode, 0, result.stderr)
        schema = json.loads((self.root / "schemas/operations/materials.json").read_text())
        operation = schema["operations"][-1]
        self.assertEqual(operation["operation_id"], "material.set_name")
        self.assertEqual(operation["schema_id"], "qcae.operation.material.set_name.v1")
        self.assertEqual(operation["effect"], "document_write")
        self.assertEqual([field["field_id"] for field in operation["fields"]], [1, 2])
        self.assertTrue(all(operation["context"].values()))
        header = (self.root / "features/materials/include/qcae/material_operations.hpp").read_text()
        self.assertIn("prepare_material_set_name(const operations::MaterialSetNameInput&);", header)
        self.assertLess(header.index("prepare_material_set_name"),
                        header.index("Result<bool> register_handlers("))
        source = (self.root / "features/materials/src/material_operations.cpp").read_text()
        self.assertIn("Result<OperationPlan> prepare_material_set_name(const MaterialSetNameInput& input)",
                      source)
        self.assertIn("ErrorCode::unsupported_capability", source)
        registration = "registered = declare_unimplemented<MaterialSetNameInput>(registry);\n"
        self.assertIn(registration + "    return registered;\n}", source)
        self.assertTrue(all(len(line) <= 100 for line in source.splitlines() + header.splitlines()))

    def test_every_standard_feature_accepts_a_scaffold(self):
        for feature, operation, type_name in (
                ("mesh_editing", "node.scale_position", "NodeScalePositionInput"),
                ("analysis", "force.scale", "ForceScaleInput")):
            result = self.run_tool("--feature", feature, "--operation", operation, "--type", type_name,
                                   "--field", "factor:finite_number:optional", "--label", "Scale")
            self.assertEqual(result.returncode, 0, f"{feature}: {result.stderr}")
            schema = json.loads((self.root / f"schemas/operations/{feature}.json").read_text())
            self.assertEqual(schema["operations"][-1]["operation_id"], operation, feature)

    def test_explicit_schema_and_missing_feature_schema(self):
        result = self.run_tool(*MATERIAL, "--schema", "schemas/operations/mesh_editing.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        schema = json.loads((self.root / "schemas/operations/mesh_editing.json").read_text())
        self.assertEqual(schema["operations"][-1]["operation_id"], "material.set_name")
        (self.root / "schemas/operations/materials.json").unlink()
        before = self.snapshot()
        result = self.run_tool(*MATERIAL)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("schemas/operations/<feature>.json", result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_dry_run_and_rejections_leave_files_unchanged(self):
        before = self.snapshot()
        self.assertEqual(self.run_tool(*MATERIAL, "--dry-run").returncode, 0)
        self.assertEqual(self.snapshot(), before)
        for arguments, reason in (
                (["--field", "name:text"], "unknown field type"),
                (["--field", "modulus:quantity"], "quantity without units"),
                (["--operation", "material.create", "--type", "OtherInput"], "duplicate operation"),
                (["--type", "MaterialSetName"], "type without Input suffix"),
                (["--label", 'say "hi"'], "quoted label")):
            result = self.run_tool(*(MATERIAL + arguments))
            self.assertNotEqual(result.returncode, 0, reason)
            self.assertIn("new_operation:", result.stderr, reason)
            self.assertEqual(self.snapshot(), before, reason)

    def test_second_run_of_the_same_operation_is_refused(self):
        self.assertEqual(self.run_tool(*MATERIAL).returncode, 0)
        after_first = self.snapshot()
        self.assertNotEqual(self.run_tool(*MATERIAL).returncode, 0)
        self.assertEqual(self.snapshot(), after_first)


if __name__ == "__main__":
    unittest.main()
