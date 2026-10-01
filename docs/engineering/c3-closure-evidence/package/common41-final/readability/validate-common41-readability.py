#!/usr/bin/env python3
"""Read-only independent source/execution/log binding; never runs build/tests/GUI."""
from pathlib import Path
import datetime
import hashlib
import importlib.util
import json
import re
import subprocess
import sys

ROOT = Path('/Users/qs/Documents/ChatGPT/QCAE')
OUT = Path('/private/tmp/qcae-common40-readability-review')
COMMIT = 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4'
DIGEST = '36335df61c1d57bec11ba620b957d25b0ba07e3242b63f7951f891ebe3a3e5ff'
PACKAGE = ROOT / 'docs/engineering/c3-closure-evidence/package/common41-final'
NAMES = {f'{name}-{suffix}' for name in ('core', 'sqlite_headless', 'asan_ubsan', 'desktop')
         for suffix in ('configure', 'build', 'tests')} | {
    'design', 'cpp-format', 'diff-check', 'architecture', 'protected-ast',
    'public-consumers-configure', 'public-consumers-build'}
COUNTS = {'core': 39, 'sqlite_headless': 45, 'asan_ubsan': 45, 'desktop': 86}
BUILD_DIRS = {'core': 'build-c3-closure-core', 'sqlite_headless': 'build-c3-vtk-sqlite-release',
              'asan_ubsan': 'build-c4-baseline-sqlite-sanitizers40', 'desktop': 'build-c3-closure-desktop'}

def file_hash(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()

spec = importlib.util.spec_from_file_location('common41_independent_binding', ROOT / 'tools/freeze_c4_baseline.py')
freeze = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = freeze
spec.loader.exec_module(freeze)
repo = freeze.Repository(ROOT, COMMIT)
assert repo.git('rev-parse', 'HEAD').decode().strip() == COMMIT
sources = freeze.committed_source_facts(repo)
assert len(sources) == 399 and freeze.source_digest(sources) == DIGEST
for row in sources:
    path = ROOT / row['path']
    assert not path.is_symlink() and path.is_file()
    assert file_hash(path) == row['sha256'], row['path']
    assert bool(path.stat().st_mode & 0o111) == (row['mode'] == '100755'), row['path']

commands_path = PACKAGE / 'actual-commands.json'
commands = json.loads(commands_path.read_text())
assert len(commands) == len(NAMES) and {row['name'] for row in commands} == NAMES
rows = []
for row in commands:
    assert row['source_commit'] == COMMIT and row['source_tree_sha256'] == DIGEST
    assert row['exit_codes'] == [0]
    assert len(row['commands']) == 1 and all(isinstance(arg, str) and arg for arg in row['commands'][0])
    assert row['logs']
    logs = []
    for item in row['logs']:
        path = ROOT / item['path']
        assert path.resolve(strict=True).is_relative_to(ROOT)
        assert file_hash(path) == item['sha256'], item['path']
        logs.append({**item, 'bytes': path.stat().st_size})
    rows.append({**row, 'observed_argv_sha256': hashlib.sha256(
        json.dumps(row['commands'], ensure_ascii=False, separators=(',', ':')).encode()).hexdigest(),
        'verified_logs': logs})

matrices = []
for name, count in COUNTS.items():
    text = (PACKAGE / (name + '-tests.log')).read_text()
    assert re.search(r'100% tests passed, 0 tests failed out of ' + str(count) + r'\b', text)
    tests = [{'name': a, 'status': b, 'seconds': float(c)} for a, b, c in re.findall(
        r'Test\s+#?\d+:\s+(\S+)[ .]+(Passed|\*\*\*Failed)\s+([0-9.]+)', text)]
    assert len(tests) == count and all(row['status'] == 'Passed' for row in tests)
    elapsed = re.search(r'Total Test time \(real\) =\s+([0-9.]+) sec', text)
    assert elapsed
    diagnostics = [line for line in (PACKAGE / (name + '-build.log')).read_text().splitlines()
                   if re.search(r'warning:|error:', line, re.I)]
    assert not diagnostics, diagnostics
    build = ROOT / BUILD_DIRS[name]
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if '=' in line and ':' in line.split('=', 1)[0] and not line.startswith('//'):
            key, value = line.split('=', 1)
            cache[key.split(':', 1)[0]] = value
    expected = {'QCAE_BUILD_DESKTOP': name == 'desktop', 'QCAE_BUILD_IPC': name == 'desktop',
                'QCAE_BUILD_STORAGE': name != 'core', 'QCAE_ENABLE_SANITIZERS': name == 'asan_ubsan'}
    for key, value in expected.items():
        assert (cache[key] == 'ON') == value, (name, key)
    assert cache['CMAKE_BUILD_TYPE'] == ('Debug' if name == 'asan_ubsan' else 'Release')
    assert not cache.get('QCAE_C3_SQLITE_OBSERVED_OBJECT')
    if name != 'core':
        assert Path(cache['SQLite3_LIBRARY']).resolve() == Path('/opt/homebrew/opt/sqlite/lib/libsqlite3.dylib').resolve()
    compile_records = json.loads((build / 'compile_commands.json').read_text())
    if name == 'asan_ubsan':
        assert any('-fsanitize=address,undefined' in row['command'] for row in compile_records)
    matrices.append({'name': name, 'tests_passed': count, 'tests_failed': 0,
                     'raw_elapsed_seconds': float(elapsed.group(1)), 'tests': tests,
                     'build_warning_error_lines': diagnostics,
                     'cache_sha256': file_hash(build / 'CMakeCache.txt'),
                     'compile_commands_sha256': file_hash(build / 'compile_commands.json')})

public_build = (PACKAGE / 'public-consumers-build.log').read_text()
assert not any(re.search(r'warning:|error:', line, re.I) for line in public_build.splitlines())
reports = []
for name in ('architecture.json', 'protected-ast.json'):
    path = PACKAGE / name
    value = json.loads(path.read_text())
    reports.append({'path': str(path.relative_to(ROOT)), 'sha256': file_hash(path), 'value': value})
env_path = ROOT / 'docs/engineering/c3-closure-evidence/package/c4-common40-environment.json'
environment = json.loads(env_path.read_text())
assert environment['sqlite'].startswith('3.53.4')
images = []
for image in environment['tool_images']:
    assert file_hash(Path(image['path'])) == image['sha256'], image['id']
    images.append(dict(image))

initial_bindings = json.loads((OUT / 'common41-review-hash-bindings.json').read_text())
assert initial_bindings['commit'] == COMMIT and initial_bindings['source_tree_sha256'] == DIGEST
matching, historical, missing = [], [], []
for manifest in initial_bindings['manifests']:
    assert file_hash(ROOT / manifest['manifest']) == manifest['manifest_sha256']
    for row in manifest['records']:
        path = ROOT / row['path']
        if not path.is_file():
            missing.append(row['path'])
            continue
        assert file_hash(path) == row['current_sha256'], row['path']
        (matching if row['status'] == 'matches' else historical).append({'manifest': manifest['manifest'], **row})
assert len(matching) == 44 and len(historical) == 6 and not missing

old_commands = ROOT / 'docs/engineering/c3-closure-evidence/package/common40-final/actual-commands.json'
old_desktop = ROOT / 'docs/engineering/c3-closure-evidence/package/common40-final/desktop-tests.log'
old_text = old_desktop.read_text()
assert '99% tests passed, 1 tests failed out of 85' in old_text
assert "'apply->isEnabled()' returned FALSE" in old_text
assert file_hash(old_desktop) == file_hash(OUT / 'desktop-tests-observed.log')
old_files = json.loads((OUT / 'artifact-sha256.json').read_text())['files']
for artifact in old_files:
    assert file_hash(OUT / artifact['path']) == artifact['sha256'], artifact['path']

result = {'schema': 'qcae.independent-common-readability-execution/1',
          'observed_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'passed': True, 'review_scope': 'common bounded production/tool slice source and executed normal regression matrix; original excluded SDK and real solver/performance acceptances remain open',
          'source_commit': COMMIT, 'source_tree_sha256': DIGEST, 'source_file_count': 399,
          'actual_commands_path': str(commands_path.relative_to(ROOT)),
          'actual_commands_sha256': file_hash(commands_path), 'executed_record_count': len(rows),
          'executed_records': rows, 'matrices': matrices, 'json_quality_reports': reports,
          'environment_manifest_path': str(env_path.relative_to(ROOT)),
          'environment_manifest_sha256': file_hash(env_path), 'verified_tool_images': images,
          'matching_historical_review_rows': len(matching), 'superseded_historical_rows': historical,
          'missing_historical_review_sources': missing,
          'original_common40_red': {'path': str(old_desktop.relative_to(ROOT)), 'sha256': file_hash(old_desktop),
                                    'actual_commands_sha256': file_hash(old_commands)},
          'original_private_receipt_file_count': len(old_files), 'old_private_receipts_unchanged': True,
          'source_authority_review': {'path': str(OUT / 'common41-qg02-review.md'),
                                      'sha256': file_hash(OUT / 'common41-qg02-review.md')},
          'reviewer_ran_shared_build_or_native_gui': False, 'reviewer_edited_repo_in_this_turn': False,
          'complete_c3_passed': False, 'complete_sk12_passed': False, 'complete_p0_passed': False,
          'whole_pipeline_owned_copy_coverage': 'unknown'}
with (OUT / 'common41-execution-validation.json').open('x') as stream:
    stream.write(json.dumps(result, indent=2) + '\n')
print(json.dumps({'passed': True, 'source_commit': COMMIT, 'source_tree_sha256': DIGEST,
                  'source_file_count': len(sources), 'executed_record_count': len(rows),
                  'matrices': [{k: v for k, v in row.items() if k in ('name', 'tests_passed', 'tests_failed', 'raw_elapsed_seconds')} for row in matrices],
                  'matching_review_rows': len(matching), 'historical_differences_explained': len(historical),
                  'old_common40_red_preserved': True, 'old_private_receipts_unchanged': True,
                  'complete_c3_passed': False, 'complete_sk12_passed': False, 'complete_p0_passed': False}))
