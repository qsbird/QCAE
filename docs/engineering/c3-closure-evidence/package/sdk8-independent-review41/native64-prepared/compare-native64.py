"""Read complete actual native64 reports; this program never starts a GUI."""
from pathlib import Path
import argparse
import hashlib
import json

HERE = Path(__file__).resolve().parent
CORRELATED = ('actual_engines', 'font_identities', 'glyph_ids', 'glyph_runs', 'positions', 'string_indexes')
PROVENANCE = ('run_id', 'started_utc', 'qt_libraries_path', 'loaded_sdk_images')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def encoded(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()


def differences(old, new, path='$'):
    if isinstance(old, dict) and isinstance(new, dict):
        result = []
        for key in sorted(old.keys() | new.keys()):
            if key not in old or key not in new:
                result.append({'path': path + '.' + key, 'old': old.get(key), 'current': new.get(key)})
            else:
                result.extend(differences(old[key], new[key], path + '.' + key))
        return result
    if isinstance(old, list) and isinstance(new, list):
        result = []
        if len(old) != len(new):
            result.append({'path': path + '.length', 'old': len(old), 'current': len(new)})
        for i, (a, b) in enumerate(zip(old, new)):
            result.extend(differences(a, b, path + '[' + str(i) + ']'))
        return result
    return [] if old == new else [{'path': path, 'old': old, 'current': new}]


def correlate(case):
    runs = case['glyph_runs']
    assert all(len(case[field]) == len(runs) for field in CORRELATED), case['case_id']
    for i, run in enumerate(runs):
        assert case['font_identities'][i] == run['font']
        assert case['glyph_ids'][i] == run['glyph_ids']
        assert case['positions'][i] == run['positions']
        assert case['string_indexes'][i] == run['string_indexes']
        assert case['actual_engines'][i]['family'] == run['font']['family']
        assert case['actual_engines'][i]['style'] == run['font']['style']
        assert len(run['glyph_ids']) == len(run['positions']) == len(run['string_indexes']) == len(run['unshaped_raw_advances'])


def shared_bundle_diagnostic(old, new):
    cases = []
    for a, b in zip(old['cases'], new['cases']):
        assert a['case_id'] == b['case_id']
        correlate(a)
        correlate(b)
        remaining = differences({k: v for k, v in a.items() if k not in CORRELATED}, {k: v for k, v in b.items() if k not in CORRELATED})
        used = set()
        mapping = []
        complete_hashes = []
        unmatched = []
        for i in range(len(a['glyph_runs'])):
            bundle = {k: a[k][i] for k in CORRELATED}
            matches = [j for j in range(len(b['glyph_runs'])) if j not in used and bundle == {k: b[k][j] for k in CORRELATED}]
            if matches:
                used.add(matches[0])
                mapping.append(matches[0])
            else:
                mapping.append(None)
                unmatched.append(i)
            complete_hashes.append(hashlib.sha256(encoded(bundle)).hexdigest())
        equal = not remaining and not unmatched and used == set(range(len(b['glyph_runs'])))
        cases.append({'case_id': a['case_id'], 'all_remaining_case_field_differences': remaining, 'old_to_current_run_mapping': mapping, 'old_complete_correlated_bundle_sha256': complete_hashes, 'current_complete_correlated_bundle_sha256': [hashlib.sha256(encoded({k: b[k][j] for k in CORRELATED})).hexdigest() for j in range(len(b['glyph_runs']))], 'unmatched_original_run_indexes': unmatched, 'full_bundle_bijection_equal': equal, 'raw_order_equal': all(a[k] == b[k] for k in CORRELATED)})
    return {'purpose': 'Supplemental complete correlated bundle identity diagnostic; preserves every inner value and never replaces the primary exact original-order comparison.', 'correlated_fields': list(CORRELATED), 'case_count': len(cases), 'all_full_bundles_and_remaining_case_fields_equal': all(c['full_bundle_bijection_equal'] for c in cases), 'cases': cases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--execution-record', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binding = json.loads((HERE / 'preparation-binding.json').read_text())
    execution = json.loads(args.execution_record.read_text())
    plan = json.loads((HERE / 'root-run-plan.json').read_text())
    assert execution['argv'] == plan['argv']
    assert execution['environment'] == plan['environment']
    assert execution['exit_code'] == 0 and execution['runtime_executed'] is True
    assert execution['binary_sha256'] == binding['binary_sha256'] == sha(binding['binary'])
    assert execution['log_sha256'] == sha(execution['log'])
    assert execution['report_sha256'] == sha(args.current)
    assert sha(binding['source']) == binding['source_sha256']
    assert sha(binding['qt_conf']) == binding['qt_conf_sha256']
    current = json.loads(args.current.read_text())
    assert current['probe_completed'] and current['backend_verified']
    assert current['case_count'] == len(current['cases']) == 64
    assert current['expected_engine'] == 'Freetype' and current['qt_version'] == '6.11.1'
    assert current['qt_libraries_path'] == '/private/tmp/qcae-sdk8-reviewed41/qt-prefix/lib'
    actual_images = {i['path']: i['sha256'] for i in current['loaded_sdk_images']}
    assert len(actual_images) == len(current['loaded_sdk_images'])
    assert actual_images == binding['candidate_images']
    assert all(sha(path) == digest for path, digest in actual_images.items())
    assert all(sha(path) == digest for path, digest in binding['font_files'].items())
    for entry in (binding['candidate_manifest'], binding['candidate_image_binding']):
        assert sha(entry['path']) == entry['sha256']
    comparisons = {}
    for name, base in binding['baselines'].items():
        assert sha(base['copy_path']) == base['copy_sha256']
        old = json.loads(Path(base['copy_path']).read_text())
        assert old.keys() == current.keys()
        assert [c['case_id'] for c in current['cases']] == base['case_ids_in_original_order']
        assert all(set(c) == set(binding['preserved_case_fields']) for c in current['cases'])
        # The primary comparison preserves every case field and all original array orders.
        left = {k: v for k, v in old.items() if k not in PROVENANCE}
        right = {k: v for k, v in current.items() if k not in PROVENANCE}
        diff = differences(left, right)
        comparisons[name] = {'baseline_original_path': base['original_path'], 'baseline_copy_sha256': base['copy_sha256'], 'primary_original_order_full_parsed_equal': left == right, 'primary_complete_differences': diff, 'only_isolated_provenance_fields': list(PROVENANCE), 'isolated_original_provenance': {k: old[k] for k in PROVENANCE}, 'isolated_current_provenance': {k: current[k] for k in PROVENANCE}, 'supplemental_shared_bundle_diagnostic': shared_bundle_diagnostic(old, current)}
    report = {'schema': 'qcae.sdk8-native64-complete-original-behavior-comparison/1', 'current_report': str(args.current), 'current_report_sha256': sha(args.current), 'execution_record': str(args.execution_record), 'execution_record_sha256': sha(args.execution_record), 'comparator_source': str(Path(__file__).resolve()), 'comparator_sha256': sha(Path(__file__)), 'case_count': 64, 'source_sha256': binding['source_sha256'], 'all_current_font_and_image_provenance_verified': True, 'comparisons': comparisons, 'primary_all_original_order_fields_equal': all(c['primary_original_order_full_parsed_equal'] for c in comparisons.values()), 'supplemental_all_complete_bundles_equal': all(c['supplemental_shared_bundle_diagnostic']['all_full_bundles_and_remaining_case_fields_equal'] for c in comparisons.values()), 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False}
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({k: report[k] for k in ('case_count', 'primary_all_original_order_fields_equal', 'supplemental_all_complete_bundles_equal', 'all_current_font_and_image_provenance_verified', 'whole_pipeline_owned_copy_coverage')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
