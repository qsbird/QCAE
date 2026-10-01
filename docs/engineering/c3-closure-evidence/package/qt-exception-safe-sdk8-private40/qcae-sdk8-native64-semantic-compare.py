#!/usr/bin/env python3
"""Compare complete correlated glyph-run records without dropping fields."""
import hashlib
import json
from pathlib import Path

root = Path('/Users/qs/Documents/ChatGPT/QCAE')
old_path = root / 'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-causal-64.json'
new_path = root / 'build-c3-qt-observed/tranche8b-native64/causal-64.json'
output = root / 'build-c3-qt-observed/tranche8b-native64/root-correlated-bijection-compare.json'
fields = ('actual_engines', 'font_identities', 'glyph_ids', 'glyph_runs', 'positions', 'string_indexes')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def encoded(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()

def rows(document):
    result = {row['case_id']: row for row in document['cases']}
    assert len(result) == len(document['cases']) == 64
    return result

old = json.loads(old_path.read_text())
new = json.loads(new_path.read_text())
left, right = rows(old), rows(new)
assert left.keys() == right.keys()
assert new['backend_verified'] is True and new['probe_completed'] is True
case_results = []
for case_id, a in left.items():
    b = right[case_id]
    assert a.keys() == b.keys(), case_id
    for k in a.keys() - set(fields):
        assert a[k] == b[k], (case_id, k)
    for row in (a, b):
        runs = row['glyph_runs']
        assert all(len(row[k]) == len(runs) for k in fields), case_id
        for i, run in enumerate(runs):
            assert row['font_identities'][i] == run['font']
            assert row['glyph_ids'][i] == run['glyph_ids']
            assert row['positions'][i] == run['positions']
            assert row['string_indexes'][i] == run['string_indexes']
            assert row['actual_engines'][i]['family'] == run['font']['family']
            assert row['actual_engines'][i]['style'] == run['font']['style']
    # One shared permutation is applied to all six complete outer run arrays.
    # No glyph, coordinate, advance, font-table or inner record is normalized.
    used = set()
    mapping = []
    full_run_hashes = []
    for i in range(len(a['glyph_runs'])):
        bundle = {k: a[k][i] for k in fields}
        matches = [j for j in range(len(b['glyph_runs'])) if j not in used
                   and bundle == {k: b[k][j] for k in fields}]
        assert matches, (case_id, i)
        j = matches[0]
        used.add(j)
        mapping.append(j)
        full_run_hashes.append(hashlib.sha256(encoded(bundle)).hexdigest())
    assert used == set(range(len(b['glyph_runs'])))
    case_results.append({'case_id': case_id, 'complete_case_key_count': len(a),
                         'raw_order_diff_fields': [k for k in fields if a[k] != b[k]],
                         'old_run_to_new_run': mapping,
                         'complete_correlated_run_sha256': full_run_hashes,
                         'semantic_difference_count': 0, 'passed': True})
status = json.loads((new_path.parent / 'native-run-status.json').read_text())
assert status['exit_code'] == 0
native_log = new_path.parent / 'native-64.log'
assert 'SDK8_native64 original_exit=0 sticky_status=0 collector_complete=1' in native_log.read_text()
report = {'schema': 'qcae.font-complete-glyph-run-bijection/1',
          'scope': 'actual native SDK7 versus private SDK8b; full correlated glyph-run records and every remaining case field',
          'old': {'path': str(old_path), 'sha256': sha(old_path)},
          'new': {'path': str(new_path), 'sha256': sha(new_path)},
          'native_run_status': {'path': str(new_path.parent / 'native-run-status.json'),
                                'sha256': sha(new_path.parent / 'native-run-status.json')},
          'native_log': {'path': str(native_log), 'sha256': sha(native_log)},
          'comparator': {'path': str(Path(__file__).resolve()), 'sha256': sha(Path(__file__))},
          'case_count': len(case_results),
          'raw_order_changed_case_count': sum(bool(row['raw_order_diff_fields']) for row in case_results),
          'normalization': 'bijective permutation of complete outer glyph-run bundles, shared across all six arrays; no inner-field or value changes',
          'cases': case_results, 'semantic_difference_count': 0, 'passed': True,
          'product_default_changed': False, 'default_coretext_copy_gap_closed': False,
          'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_sk12_passed': False}
with output.open('x') as stream:
    stream.write(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({k: report[k] for k in ('passed', 'case_count', 'raw_order_changed_case_count', 'semantic_difference_count', 'whole_pipeline_owned_copy_coverage')}))
