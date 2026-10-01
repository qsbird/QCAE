#!/usr/bin/env python3
"""Complete the byte-exact archive; retain the first archiver type assertion RED."""
import hashlib
import json
from pathlib import Path
import shutil

root = Path('/Users/qs/Documents/ChatGPT/QCAE')
out = root / 'docs/engineering/c3-closure-evidence/package/qt-exception-safe-sdk8-private40'
receipt_path = out / 'archive-receipt.json'
assert out.is_dir() and not receipt_path.exists()
facts = json.loads((out / 'native64/actual-source-facts.json').read_text())
assert facts['collector_complete'] is True and facts['observer_failure_status'] == '0'
native = json.loads((out / 'native64/root-correlated-bijection-compare.json').read_text())
assert native['passed'] is True and native['case_count'] == 64
target = out / Path(__file__).name
with target.open('xb') as stream:
    stream.write(Path(__file__).read_bytes())
entries = []
for directory, source in [('private', root / 'build-c3-qt-observed/tranche8b-private'),
                          ('native64', root / 'build-c3-qt-observed/tranche8b-native64')]:
    for path in sorted((out / directory).iterdir()):
        original = source / path.name
        assert path.read_bytes() == original.read_bytes()
        entries.append({'original_path': str(original), 'path': path.relative_to(root).as_posix(),
                        'bytes': path.stat().st_size,
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
for path in sorted(out.iterdir()):
    if path.is_file():
        original = Path('/private/tmp') / path.name
        assert path.read_bytes() == original.read_bytes()
        entries.append({'original_path': str(original), 'path': path.relative_to(root).as_posix(),
                        'bytes': path.stat().st_size,
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
receipt = {'schema': 'qcae.sdk8-private-diagnostic-archive/1', 'files': entries,
           'source_repository_commit_observed_at_archive': '82526d8f851acb9c2b837b5133fa1e50a2d5948c',
           'product_source_changes': 0, 'headless_actual_linked_cases': 12,
           'normal_hash_full_parsed_report_unchanged': True,
           'native_actual_backend_cases': 64, 'native_complete_correlated_permutation_cases': 31,
           'native_semantic_differences': 0,
           'observer_sticky_failure_status': '0', 'collector_complete_in_native_run': True,
           'old_raw_order_comparison_red_preserved': True,
           'archive_fixture_type_assertion_red': 'first archiver assumed numeric status 0, but actual JSON status is string "0"; first source retained byte-exact; no evidence value changed',
           'whole_pipeline_owned_copy_coverage': 'unknown',
           'FreeType_internal_copy_coverage': 'unknown', 'default_CoreText_copy_gap_closed': False,
           'complete_c3_passed': False, 'complete_sk12_passed': False,
           'integration_state': 'private diagnostic prefix only; repository SDK preparation/collector adoption remains pending'}
with receipt_path.open('x') as stream:
    stream.write(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'files': len(entries), 'archive': str(out), 'native_cases': 64, 'semantic_differences': 0}))
