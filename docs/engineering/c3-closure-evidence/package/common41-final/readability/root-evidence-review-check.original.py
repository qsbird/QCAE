#!/usr/bin/env python3
"""Check actual common41 evidence and independent review without rerunning it."""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import subprocess

ROOT = Path('/Users/qs/Documents/ChatGPT/QCAE')
OUT = ROOT / 'docs/engineering/c3-closure-evidence/package/common41-final'
REVIEW = Path('/private/tmp/qcae-common40-readability-review')
COMMIT = 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4'
DIGEST = '36335df61c1d57bec11ba620b957d25b0ba07e3242b63f7951f891ebe3a3e5ff'

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

spec = importlib.util.spec_from_file_location('freeze', ROOT / 'tools/freeze_c4_baseline.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
repo = module.Repository(ROOT, COMMIT)
assert repo.git('rev-parse', 'HEAD').decode().strip() == COMMIT
sources = module.committed_source_facts(repo)
assert len(sources) == 399 and module.source_digest(sources) == DIGEST
for source in sources:
    assert sha(ROOT / source['path']) == source['sha256']
records = json.loads((OUT / 'actual-commands.json').read_text())
expected = {f'{matrix}-{phase}' for matrix in ('core', 'sqlite_headless', 'asan_ubsan', 'desktop')
            for phase in ('configure', 'build', 'tests')} | {
    'design', 'cpp-format', 'diff-check', 'architecture', 'protected-ast',
    'public-consumers-configure', 'public-consumers-build'}
assert len(records) == 19 and {row['name'] for row in records} == expected
for row in records:
    assert row['source_commit'] == COMMIT and row['source_tree_sha256'] == DIGEST
    assert row['exit_codes'] == [0] and len(row['commands']) == 1
    assert row['logs'] and row['elapsed_seconds'] >= 0
    for log in row['logs']:
        assert sha(ROOT / log['path']) == log['sha256']
for name, count in [('core', 39), ('sqlite_headless', 45), ('asan_ubsan', 45), ('desktop', 86)]:
    text = (OUT / f'{name}-tests.log').read_text()
    assert f'100% tests passed, 0 tests failed out of {count}' in text
    tests = re.findall(r'Test\s+#?\d+:\s+(\S+)[ .]+Passed\s+[0-9.]+', text)
    assert len(tests) == count and len(set(tests)) == count
    assert not re.search('warning:|error:', (OUT / f'{name}-build.log').read_text(), re.I)
independent = json.loads((REVIEW / 'common41-execution-validation.json').read_text())
assert independent['passed'] is True and independent['source_commit'] == COMMIT
assert independent['source_tree_sha256'] == DIGEST and independent['executed_record_count'] == 19
assert independent['actual_commands_sha256'] == sha(OUT / 'actual-commands.json')
assert independent['source_authority_review']['sha256'] == sha(REVIEW / 'common41-qg02-review.md')
assert independent['old_private_receipts_unchanged'] is True
assert independent['original_common40_red']['sha256'] == sha(ROOT / independent['original_common40_red']['path'])
assert not independent['missing_historical_review_sources']
assert independent['complete_c3_passed'] is False and independent['complete_sk12_passed'] is False
assert independent['complete_p0_passed'] is False
for image in independent['verified_tool_images']:
    assert sha(Path(image['path'])) == image['sha256']
assert sha(ROOT / 'AGENTS.md') == 'eaa469194e6952cc8e7a73859f8023216379fa65085ccd8bfd7efc075fe9eaa9'
result = {'passed': True, 'source_commit': COMMIT, 'source_tree_sha256': DIGEST,
          'committed_source_files': len(sources), 'executed_records_verified': len(records),
          'independent_reviewer': '/root/lean_refresh_review',
          'author_separation': 'Reviewer authored package preflight; root independently reviewed and executed its thirteen actual tests. Root authored metadata guards; reviewer independently reviewed and probed twelve boundaries.',
          'old_red_and_user_changes_preserved': True,
          'complete_c3_passed': False, 'complete_sk12_passed': False, 'complete_p0_passed': False}
path = OUT / 'root-evidence-review-validation.json'
with path.open('x') as stream:
    stream.write(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
