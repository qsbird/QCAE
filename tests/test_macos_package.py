#!/usr/bin/env python3
"""Actual staging preflight in temporary fixtures; no deployment tool or GUI runs."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("macos_stage_contract", ROOT / "tools/stage_macos_package.py")
STAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAGE)


class CopyBoundaryReached(RuntimeError):
    pass


class StagePreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qcae-stage-contract-", dir="/private/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "workspace"
        self.root.mkdir()
        self.out = self.root / "out"
        self.out.mkdir()
        self.build = self.base / "build"
        self.build.mkdir()
        self.cache = self.build / "CMakeCache.txt"
        self.cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\nQCAE_C3_SQLITE_OBSERVED_OBJECT:FILEPATH=\n")
        self.addCleanup(patch.stopall)
        patch.object(STAGE, "ROOT", self.root).start()
        patch.object(STAGE.sys, "platform", "darwin").start()

    def invoke(self, target):
        return STAGE.stage(SimpleNamespace(build_dir=self.build, bundle=target))

    def tree(self):
        return sorted(str(p.relative_to(self.base)) for p in self.base.rglob("*"))

    def rejected_before_effects(self, target):
        before = self.tree()
        with patch.object(STAGE.shutil, "copy2", side_effect=CopyBoundaryReached) as copying, \
                patch.object(STAGE, "command") as command:
            with self.assertRaises(ValueError):
                self.invoke(target)
            copying.assert_not_called()
            command.assert_not_called()
        self.assertEqual(before, self.tree())

    def admitted_before_copy_only(self, target, resolved):
        with patch.object(STAGE.shutil, "copy2", side_effect=CopyBoundaryReached) as copying, \
                patch.object(STAGE, "command") as command:
            with self.assertRaises(CopyBoundaryReached):
                self.invoke(target)
            copying.assert_called_once_with(self.build / "qcae-desktop", resolved / "Contents/MacOS/qcae-desktop")
            command.assert_not_called()
        self.assertTrue((resolved / "Contents/MacOS").is_dir())
        self.assertTrue((resolved / "Contents/Frameworks").is_dir())
        self.assertTrue((resolved / "Contents/Resources").is_dir())

    def test_dotdot_target_cannot_escape_out(self):
        self.rejected_before_effects(self.out / ".." / "escaped.app")

    def test_parent_symlink_cannot_escape_out(self):
        other = self.base / "elsewhere"
        other.mkdir()
        (self.out / "linked").symlink_to(other, target_is_directory=True)
        self.rejected_before_effects(self.out / "linked" / "escaped.app")

    def test_out_root_symlink_cannot_escape_workspace(self):
        self.out.rmdir()
        other = self.base / "elsewhere"
        other.mkdir()
        self.out.symlink_to(other, target_is_directory=True)
        self.rejected_before_effects(self.out / "escaped.app")

    def test_existing_app_directory_is_not_overwritten(self):
        target = self.out / "existing.app"
        target.mkdir()
        (target / "marker").write_text("preserve")
        self.rejected_before_effects(target)

    def test_existing_regular_file_is_not_overwritten(self):
        target = self.out / "existing.app"
        target.write_text("preserve")
        self.rejected_before_effects(target)

    def test_existing_target_symlink_is_not_followed(self):
        other = self.base / "elsewhere"
        other.mkdir()
        target = self.out / "linked.app"
        target.symlink_to(other, target_is_directory=True)
        self.rejected_before_effects(target)

    def test_dangling_target_symlink_is_not_followed(self):
        target = self.out / "dangling.app"
        target.symlink_to(self.base / "absent", target_is_directory=True)
        self.rejected_before_effects(target)

    def test_out_itself_and_wrong_suffix_are_rejected(self):
        for target in (self.out, self.out / "new.zip"):
            with self.subTest(target=target):
                self.rejected_before_effects(target)

    def test_new_nested_app_keeps_normal_preflight(self):
        target = self.out / "nested" / "QCAE.app"
        self.admitted_before_copy_only(target, target)

    def test_absent_out_root_keeps_normal_preflight(self):
        self.out.rmdir()
        target = self.out / "QCAE.app"
        self.admitted_before_copy_only(target, target)

    def test_parent_alias_inside_out_is_resolved_before_effects(self):
        actual = self.out / "actual"
        actual.mkdir()
        (self.out / "alias").symlink_to(actual, target_is_directory=True)
        self.admitted_before_copy_only(self.out / "alias" / "QCAE.app", actual / "QCAE.app")

    def test_release_gate_is_preserved(self):
        self.cache.write_text("CMAKE_BUILD_TYPE:STRING=Debug\nQCAE_C3_SQLITE_OBSERVED_OBJECT:FILEPATH=\n")
        self.rejected_before_effects(self.out / "QCAE.app")

    def test_nonempty_or_missing_observer_gate_is_preserved(self):
        for value in ("QCAE_C3_SQLITE_OBSERVED_OBJECT:FILEPATH=/tmp/observer.o\n", ""):
            with self.subTest(value=value):
                self.cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\n" + value)
                self.rejected_before_effects(self.out / "QCAE.app")


if __name__ == "__main__":
    unittest.main()
