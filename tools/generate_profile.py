#!/usr/bin/env python3
"""Separate source build identity from the controlled profile's lexical semantics."""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

MANIFEST = (
    'include/qcae/types.hpp', 'include/qcae/model.hpp', 'include/qcae/model_codec.hpp',
    'include/qcae/profile_provider.hpp', 'include/qcae/nastran_codec.hpp',
    'src/model.cpp', 'src/nastran_codec.cpp', 'docs/implementation/nastran-subset.md',
)
# Maximal-munch preprocessing tokens preserve distinctions such as + + vs ++.
TOKEN = re.compile(r'''(?:[A-Za-z_$][A-Za-z_0-9$]*|(?:\.[0-9]|[0-9])(?:[eEpP][+-]|[A-Za-z_0-9.])*|%:%:|<=>|>>=|<<=|->\*|\.\.\.|::|\.\*|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\*=|/=|%=|\+=|-=|&=|\^=|\|=|##|<:|:>|<%|%>|%:|[^\s])''')
LITERAL = re.compile(r'(?:u8|u|U|L)?([\"\'])')
RAW = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')


def cpp_tokens(source: str) -> tuple[str, ...]:
    """Canonicalize comments/spacing while preserving literals and macro boundaries.

    This is lexical identity, not an assertion that arbitrary rewritten C++ is
    equivalent. Semantic token changes invalidate the profile conservatively.
    """
    result: list[str] = []
    position = 0
    line_start = True
    directive = False
    directive_tokens: list[str] = []
    previous_end = 0
    while position < len(source):
        if source.startswith('\\\n', position) or source.startswith('\\\r\n', position):
            position += 3 if source.startswith('\\\r\n', position) else 2
            continue
        character = source[position]
        if character.isspace():
            if character == '\n':
                if directive:
                    result.append('<directive-end>')
                directive = False
                directive_tokens = []
                line_start = True
            position += 1
            continue
        if source.startswith('//', position):
            end = source.find('\n', position + 2)
            position = len(source) if end < 0 else end
            continue
        if source.startswith('/*', position):
            end = source.find('*/', position + 2)
            if end < 0:
                raise ValueError('Unterminated C++ comment')
            # A block comment is one space in preprocessing, including its newlines.
            position = end + 2
            continue
        raw = RAW.match(source, position)
        if raw:
            ending = ')' + raw.group(1) + '"'
            end = source.find(ending, raw.end())
            if end < 0:
                raise ValueError('Unterminated C++ raw literal')
            end += len(ending)
            token = source[position:end]
        else:
            literal = LITERAL.match(source, position)
            if literal:
                quote = literal.group(1)
                end = literal.end()
                while end < len(source):
                    if source[end] == '\\':
                        end += 2
                    elif source[end] == quote:
                        end += 1
                        break
                    else:
                        end += 1
                else:
                    raise ValueError('Unterminated C++ literal')
                token = source[position:end].replace('\\\n', '').replace('\\\r\n', '')
            else:
                match = TOKEN.match(source, position)
                if match is None:
                    raise ValueError('Unsupported C++ token')
                end = match.end()
                token = match.group()
        if line_start and token in ('#', '%:'):
            directive = True
        if directive and len(directive_tokens) == 3 and directive_tokens[1] == 'define' and token == '(':
            result.append('<function-macro>' if not source[previous_end:position].replace('\\\n', '').replace('\\\r\n', '') else '<object-macro>')
        result.append(token)
        if directive:
            directive_tokens.append(token)
        line_start = False
        previous_end = end
        position = end
    if directive:
        result.append('<directive-end>')
    return tuple(result)


def identities(root: Path) -> tuple[str, str]:
    moves = json.loads((root / 'modules/source-migration.json').read_text())['moves']
    semantic = hashlib.sha256(b'qcae.nastran.lexical-semantics.v2\0')
    implementation = hashlib.sha256()
    for name in MANIFEST:
        content = (root / moves.get(name, name)).read_bytes()
        implementation.update(name.encode() + b'\0' + str(len(content)).encode() + b'\0' + content)
        text = content.decode('utf-8')
        canonical = (' '.join(re.sub(r'<!--.*?-->', '', text, flags=re.S).split())
                     if name.endswith('.md') else
                     json.dumps(cpp_tokens(text), ensure_ascii=False, separators=(',', ':')))
        encoded = canonical.encode('utf-8')
        semantic.update(name.encode() + b'\0' + str(len(encoded)).encode() + b'\0' + encoded)
    return 'sha256:' + semantic.hexdigest(), 'sha256:' + implementation.hexdigest()


def main() -> None:
    root, output = Path(sys.argv[1]), Path(sys.argv[2])
    semantic, implementation = identities(root)
    text = ('#pragma once\n#include <string_view>\nnamespace qcae {\n'
            f'inline constexpr std::string_view nastran_definition_digest = "{semantic}";\n'
            f'inline constexpr std::string_view nastran_implementation_fingerprint = "{implementation}";\n'
            '}\n')
    if '--check' in sys.argv:
        if not output.exists() or output.read_text() != text:
            raise SystemExit('Generated profile digest is stale')
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text)


if __name__ == '__main__':
    main()
