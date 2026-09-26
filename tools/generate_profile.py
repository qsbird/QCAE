#!/usr/bin/env python3
"""Hash the codec/semantic implementation manifest into an immutable profile reference."""
import hashlib
import json
import sys
from pathlib import Path

root, output = Path(sys.argv[1]), Path(sys.argv[2])
manifest = [
    'include/qcae/types.hpp', 'include/qcae/model.hpp', 'include/qcae/model_codec.hpp',
    'include/qcae/profile_provider.hpp', 'include/qcae/nastran_codec.hpp',
    'src/model.cpp', 'src/nastran_codec.cpp', 'docs/implementation/nastran-subset.md',
]
# Logical manifest names are compatibility identities, independent of physical layout.
# Preserve byte-for-byte digest semantics until explicit profile version migration.
moves = json.loads((root / "modules/source-migration.json").read_text())["moves"]
h = hashlib.sha256()
for name in manifest:
    content = (root / moves.get(name, name)).read_bytes()
    h.update(name.encode() + b'\0' + str(len(content)).encode() + b'\0' + content)
text = '#pragma once\n#include <string_view>\nnamespace qcae {\ninline constexpr std::string_view nastran_definition_digest = "sha256:' + h.hexdigest() + '";\n}\n'
if '--check' in sys.argv:
    if not output.exists() or output.read_text() != text:
        raise SystemExit('Generated profile digest is stale')
else:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
