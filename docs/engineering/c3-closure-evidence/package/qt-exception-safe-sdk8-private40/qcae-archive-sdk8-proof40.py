#!/usr/bin/env python3
"""Archive original private diagnostic bytes without libraries/runtime files."""
import hashlib
import json
from pathlib import Path
import shutil

root = Path('/Users/qs/Documents/ChatGPT/QCAE')
out = root / 'docs/engineering/c3-closure-evidence/package/qt-exception-safe-sdk8-private40'
out.mkdir(exist_ok=False)
entries = []
for label, directory in [('private', root / 'build-c3-qt-observed/tranche8b-private'),
                         ('native64', root / 'build-c3-qt-observed/tranche8b-native64')]:
    dest = out / label
    dest.mkdir()
    for path in sorted(directory.iterdir()):
        if not path.is_file() or path.suffix not in ('.json', '.log', '.py', '.cpp', '.hpp', '.diff'):
            continue
        # Full external Qt source remains in the ignored dependency build.
        if path.name == 'first-linked-source-qglobal.cpp':
            continue
        target = dest / path.name
        shutil.copyfile(path, target)
        assert target.read_bytes() == path.read_bytes()
        entries.append({'original_path': str(path), 'path': target.relative_to(root).as_posix(),
                        'bytes': target.stat().st_size,
                        'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
for path in [Path('/private/tmp/qcae-sdk8-native64-semantic-compare.py'), Path(__file__).resolve()]:
    target = out / path.name
    shutil.copyfile(path, target)
    entries.append({'original_path': str(path), 'path': target.relative_to(root).as_posix(),
                    'bytes': target.stat().st_size,
                    'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
native = json.loads((out / 'native64/root-correlated-bijection-compare.json').read_text())
assert native['passed'] is True and native['case_count'] == 64
facts = json.loads((out / 'native64/actual-source-facts.json').read_text())
assert facts['collector_complete'] is True and facts['observer_failure_status'] == 0
receipt = {'schema': 'qcae.sdk8-private-diagnostic-archive/1', 'files': entries,
           'source_repository_commit_observed_at_archive': '82526d8f851acb9c2b837b5133fa1e50a2d5948c',
           'product_source_changes': 0,
           'headless_actual_linked_cases': 12,
           'normal_hash_full_parsed_report_unchanged': True,
           'native_actual_backend_cases': 64, 'native_complete_correlated_permutation_cases': 31,
           'native_semantic_differences': 0,
           'observer_sticky_failure_status': 0, 'collector_complete_in_native_run': True,
           'old_raw_order_comparison_red_preserved': True,
           'whole_pipeline_owned_copy_coverage': 'unknown',
           'FreeType_internal_copy_coverage': 'unknown', 'default_CoreText_copy_gap_closed': False,
           'complete_c3_passed': False, 'complete_sk12_passed': False,
           'integration_state': 'private diagnostic prefix only; repository SDK preparation/collector adoption remains pending'}
(out / 'archive-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'files': len(entries), 'archive': str(out), 'native_cases': 64, 'semantic_differences': 0}))
