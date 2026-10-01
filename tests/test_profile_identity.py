#!/usr/bin/env python3
"""Exercise semantic identity under formatting and incompatible token changes."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('generate_profile', ROOT / 'tools/generate_profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class ProfileIdentity(unittest.TestCase):
    def test_comments_and_formatting(self):
        self.assertEqual(profile.cpp_tokens('int x = 1;\n// explanation\nx += 2;'),
                         profile.cpp_tokens('int/* type */ x=1; x += /* amount */2;'))

    def test_literals_are_semantics(self):
        for before, after in [('"a b"', '"ab"'), ('1.0', '1.1'),
                              ('u8"x"', '"x"'), ('R"tag(a//b)tag"', 'R"tag(a/b)tag"')]:
            self.assertNotEqual(profile.cpp_tokens(before), profile.cpp_tokens(after))

    def test_operator_boundaries(self):
        self.assertNotEqual(profile.cpp_tokens('x + + y'), profile.cpp_tokens('x ++ y'))
        self.assertNotEqual(profile.cpp_tokens('x < < y'), profile.cpp_tokens('x << y'))

    def test_macro_boundaries(self):
        self.assertNotEqual(profile.cpp_tokens('#define f(x) x\n'),
                            profile.cpp_tokens('#define f (x) x\n'))
        self.assertNotEqual(profile.cpp_tokens('#define a 1\na + 2;'),
                            profile.cpp_tokens('#define a 1 a + 2;'))
        self.assertEqual(profile.cpp_tokens('#define f(x) x + \\\n  1\n'),
                         profile.cpp_tokens('#define f(x) x+1\n'))

    def test_incompatible_semantics(self):
        self.assertNotEqual(profile.cpp_tokens('constexpr int max_number = 99999999;'),
                            profile.cpp_tokens('constexpr int max_number = 9999999;'))

    def test_actual_manifest_identities(self):
        moves = json.loads((ROOT / 'modules/source-migration.json').read_text())['moves']
        original_semantic, original_build = profile.identities(ROOT)
        with tempfile.TemporaryDirectory(prefix='qcae-profile-identity-') as directory:
            root = Path(directory)
            (root / 'modules').mkdir()
            (root / 'modules/source-migration.json').write_text('{"moves":{}}')
            for name in profile.MANIFEST:
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((ROOT / moves.get(name, name)).read_bytes())
            codec = root / 'src/nastran_codec.cpp'
            source = codec.read_text()
            codec.write_text('// formatting-only probe\n\n' + source + '\n')
            semantic, build = profile.identities(root)
            self.assertEqual(semantic, original_semantic)
            self.assertNotEqual(build, original_build)
            self.assertIn('99999999', source)
            codec.write_text(source.replace('99999999', '99999998', 1))
            incompatible, _ = profile.identities(root)
            self.assertNotEqual(incompatible, original_semantic)

    def test_malformed_literals_fail(self):
        for text in ('/* unfinished', '"unfinished', 'R"x(unfinished'):
            with self.assertRaises(ValueError):
                profile.cpp_tokens(text)


if __name__ == '__main__':
    unittest.main()
