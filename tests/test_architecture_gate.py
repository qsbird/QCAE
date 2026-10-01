#!/usr/bin/env python3
"""Eight SK-02 architectural mutations, checked using real temporary CMake projects."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("architecture_gate", ROOT / "tools/check_architecture.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class ArchitectureMutations(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qcae-architecture-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        self.build = Path(self.temporary.name) / "build"
        self.rows = []
        for name, path, dependencies in [
            ("qcae_foundation", "modules/foundation", []),
            ("qcae_document", "modules/document", ["qcae_foundation"]),
            ("qcae_application", "modules/application", ["qcae_document"]),
            ("qcae_query", "modules/query", ["qcae_document"]),
            ("qcae_desktop_client", "modules/clients", ["qcae_foundation"]),
            ("qcae_desktop_ui", "ui/desktop", ["qcae_desktop_client"]),
        ]:
            header = f"{path}/include/qcae/{name}.hpp"
            source = f"{path}/src/{name}.cpp"
            self.write(header, "#pragma once\n")
            self.write(source, f'#include "qcae/{name}.hpp"\nint {name}_example() {{ return 0; }}\n')
            self.rows.append({"target": name, "path": path, "kind": "STATIC", "option": None,
                              "public_include_dirs": [f"{path}/include"],
                              "private_include_dirs": [f"{path}/src"], "public_headers": [header],
                              "sources": [source], "public_dependencies": dependencies,
                              "private_dependencies": [], "external_dependencies": []})
        self.write("modules/document/src/private.hpp", "#pragma once\n")
        self.row("qcae_document")["private_headers"] = ["modules/document/src/private.hpp"]
        self.extra_cmake = ""
        self.write_configuration()
        self.assertEqual(self.run_gate(), [], "unmodified minimal project must pass")

    def write(self, name, text):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)

    def row(self, name):
        return next(row for row in self.rows if row["target"] == name)

    def write_configuration(self):
        self.write("modules/targets.json", json.dumps({"schema_version": 1, "targets": self.rows}))
        lines = ["cmake_minimum_required(VERSION 3.24)", "project(ArchitectureFixture LANGUAGES CXX)"]
        for row in self.rows:
            name = row["target"]
            lines += [f'add_library({name} STATIC {row["sources"][0]})',
                      f'target_include_directories({name} PUBLIC "${{CMAKE_CURRENT_SOURCE_DIR}}/{row["public_include_dirs"][0]}")']
            if row["public_dependencies"]:
                lines.append(f'target_link_libraries({name} PUBLIC {" ".join(row["public_dependencies"])})')
            if row["private_dependencies"]:
                lines.append(f'target_link_libraries({name} PRIVATE {" ".join(row["private_dependencies"])})')
        self.write("CMakeLists.txt", "\n".join(lines) + "\n" + self.extra_cmake)

    def run_gate(self):
        evidence, trace = GATE.cmake_evidence(self.root, self.build, refresh=True)
        return GATE.check(self.root, GATE.read_json(self.root / "modules/targets.json"), evidence, trace)

    def assert_rejected_then_restored(self, code, restore):
        self.write_configuration()
        errors = self.run_gate()
        self.assertTrue(any(error.startswith(code + ":") for error in errors), errors)
        restore()
        self.write_configuration()
        self.assertEqual(self.run_gate(), [], "restored project must pass")

    def inject_domain_header(self, header):
        path = self.row("qcae_document")["sources"][0]
        original = (self.root / path).read_text()
        self.write(path, original + f"#include <{header}>\n")
        self.assert_rejected_then_restored("core-framework", lambda: self.write(path, original))

    def test_01_domain_qt(self):
        self.inject_domain_header("QString")

    def test_02_domain_sqlite(self):
        self.inject_domain_header("sqlite3.h")

    def test_03_domain_vtk(self):
        self.inject_domain_header("vtkRenderer.h")

    def test_04_gui_links_application(self):
        deps = self.row("qcae_desktop_ui")["private_dependencies"]
        deps.append("qcae_application")
        self.assert_rejected_then_restored("client-domain", deps.clear)

    def test_05_sdk_links_document(self):
        deps = self.row("qcae_desktop_client")["private_dependencies"]
        deps.append("qcae_document")
        self.assert_rejected_then_restored("client-domain", deps.clear)

    def test_06_dependency_cycle(self):
        deps = self.row("qcae_document")["private_dependencies"]
        deps.append("qcae_application")
        self.assert_rejected_then_restored("cycle", deps.clear)

    def test_07_cross_module_private_header(self):
        path = self.row("qcae_application")["sources"][0]
        original = (self.root / path).read_text()
        self.write(path, original + '#include "../../document/src/private.hpp"\n')
        self.assert_rejected_then_restored("private-header", lambda: self.write(path, original))

    def test_08_undeclared_cmake_dependency(self):
        self.extra_cmake = "target_link_libraries(qcae_application PRIVATE qcae_query)\n"
        self.assert_rejected_then_restored("undeclared-dependency", lambda: setattr(self, "extra_cmake", ""))

    def test_conditional_public_usage_requirements(self):
        row = self.row("qcae_application")
        row["conditional_public_dependencies"] = {"OPTIONAL_PUBLIC": ["qcae_query"]}
        self.extra_cmake = "set(OPTIONAL_PUBLIC ON CACHE BOOL \"\" FORCE)\ntarget_link_libraries(qcae_application PUBLIC qcae_query)\n"
        self.write_configuration()
        self.assertEqual(self.run_gate(), [])
        self.extra_cmake = "set(OPTIONAL_PUBLIC OFF CACHE BOOL \"\" FORCE)\n"
        self.write_configuration()
        self.assertEqual(self.run_gate(), [])
        self.extra_cmake += "target_link_libraries(qcae_application PUBLIC qcae_query)\n"
        self.write_configuration()
        self.assertTrue(any(error.startswith("public-api:") for error in self.run_gate()))

    def test_unowned_production_unit(self):
        self.write("modules/document/src/unregistered.cpp", "int unregistered() { return 0; }\n")
        self.extra_cmake = "target_sources(qcae_document PRIVATE modules/document/src/unregistered.cpp)\n"
        self.write_configuration()
        errors = self.run_gate()
        self.assertTrue(any(e.startswith("ownership:") for e in errors), errors)
        self.assertTrue(any(e.startswith("file-api-ownership:") for e in errors), errors)

    def test_runtime_source_ownership(self):
        self.write("modules/clients/bridge.py", "import json\n")
        self.write_configuration()
        self.assertTrue(any(e.startswith("ownership:") for e in self.run_gate()))
        self.row("qcae_desktop_client")["runtime_sources"] = ["modules/clients/bridge.py"]
        self.write_configuration()
        self.assertEqual(self.run_gate(), [])

    def test_mcp_cannot_link_application(self):
        row = self.row("qcae_desktop_client")
        old_path = row["path"]
        row["path"] = "apps/mcp"
        row["private_dependencies"].append("qcae_application")
        self.assert_rejected_then_restored("client-domain", lambda: (row["private_dependencies"].clear(), row.update(path=old_path)))

    def test_private_include_directory_export(self):
        self.extra_cmake = 'target_include_directories(qcae_document PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/modules/document/src")\n'
        self.write_configuration()
        errors = self.run_gate()
        self.assertTrue(any(e.startswith("private-include:") for e in errors), errors)

    def test_declared_generated_include_root(self):
        self.build = self.root / "build-inside-source"
        self.row("qcae_document")["generated_public_include_dirs"] = ["generated"]
        self.extra_cmake = 'target_include_directories(qcae_document PUBLIC "${CMAKE_BINARY_DIR}/generated")\n'
        self.write_configuration()
        self.assertEqual(self.run_gate(), [])
        self.row("qcae_document")["generated_public_include_dirs"] = []
        self.write_configuration()
        self.assertTrue(any(e.startswith("private-include:") for e in self.run_gate()))

    def test_generated_root_cannot_escape_build(self):
        self.row("qcae_document")["generated_public_include_dirs"] = ["../source/modules/document/src"]
        self.extra_cmake = 'target_include_directories(qcae_document PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/modules/document/src")\n'
        self.write_configuration()
        errors = self.run_gate()
        self.assertTrue(any(e.startswith("include-root:") for e in errors), errors)
        self.assertTrue(any(e.startswith("private-include:") for e in errors), errors)


if __name__ == "__main__":
    unittest.main(verbosity=2)
