"""Independent read-only audit of root's completed native64 and mixed images."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

OUT = Path(__file__).resolve().parent
NATIVE = OUT / 'native64-prepared'
CORRELATED = ('actual_engines', 'font_identities', 'glyph_ids', 'glyph_runs', 'positions', 'string_indexes')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def encoded(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()


binding = json.loads((NATIVE / 'preparation-binding.json').read_text())
run_path = NATIVE / 'root-execution-record.json'
run = json.loads(run_path.read_text())
plan = json.loads((NATIVE / 'root-run-plan.json').read_text())
current_path = NATIVE / 'actual-native64.json'
current = json.loads(current_path.read_text())
comparison_path = NATIVE / 'root-full-comparison.json'
comparison = json.loads(comparison_path.read_text())
assert run['argv'] == plan['argv'] and run['environment'] == plan['environment']
assert run['runtime_executed'] and run['exit_code'] == 0
assert sha(binding['binary']) == binding['binary_sha256'] == run['binary_sha256']
assert sha(run['log']) == run['log_sha256']
assert sha(current_path) == run['report_sha256'] == comparison['current_report_sha256']
assert sha(binding['source']) == binding['source_sha256']
assert sha(binding['qt_conf']) == binding['qt_conf_sha256']
assert current['probe_completed'] and current['backend_verified'] and len(current['cases']) == current['case_count'] == 64
assert {x['path']: x['sha256'] for x in current['loaded_sdk_images']} == binding['candidate_images']
assert all(sha(p) == digest for p, digest in binding['candidate_images'].items())
assert all(sha(p) == digest for p, digest in binding['font_files'].items())
results = {}
for name, base in binding['baselines'].items():
    assert sha(base['copy_path']) == base['copy_sha256']
    old = json.loads(Path(base['copy_path']).read_text())
    primary = comparison['comparisons'][name]
    fields = {}
    changed_cases = set()
    for diff in primary['primary_complete_differences']:
        match = re.match(r'\$\.cases\[(\d+)\]\.([^\.\[]+)', diff['path'])
        assert match and match[2] in CORRELATED, diff['path']
        fields[match[2]] = fields.get(match[2], 0) + 1
        changed_cases.add(int(match[1]))
    diag_cases = primary['supplemental_shared_bundle_diagnostic']['cases']
    mappings = []
    for a, b, diag in zip(old['cases'], current['cases'], diag_cases):
        assert a['case_id'] == b['case_id'] == diag['case_id']
        assert a.keys() == b.keys()
        assert all(a[k] == b[k] for k in a if k not in CORRELATED)
        assert not diag['all_remaining_case_field_differences'] and diag['full_bundle_bijection_equal']
        mapping = diag['old_to_current_run_mapping']
        assert len(mapping) == len(a['glyph_runs']) == len(b['glyph_runs'])
        assert set(mapping) == set(range(len(b['glyph_runs'])))
        for field in CORRELATED:
            assert len(a[field]) == len(b[field]) == len(mapping)
        for i, j in enumerate(mapping):
            complete_a = {k: a[k][i] for k in CORRELATED}
            complete_b = {k: b[k][j] for k in CORRELATED}
            assert complete_a == complete_b
            assert hashlib.sha256(encoded(complete_a)).hexdigest() == diag['old_complete_correlated_bundle_sha256'][i]
            assert hashlib.sha256(encoded(complete_b)).hexdigest() == diag['current_complete_correlated_bundle_sha256'][j]
        mappings.append({'case_id': a['case_id'], 'shared_bijective_mapping': mapping, 'complete_bundle_count': len(mapping), 'raw_order_equal': all(a[k] == b[k] for k in CORRELATED)})
    isolated = set(binding['isolated_provenance_fields'])
    assert old.keys() == current.keys()
    assert all(old[k] == current[k] for k in old if k not in isolated | {'cases'})
    assert primary['primary_original_order_full_parsed_equal'] is False
    results[name] = {'raw_original_order_full_parsed_equal': False, 'raw_leaf_difference_count': len(primary['primary_complete_differences']), 'changed_case_count': len(changed_cases), 'raw_changed_case_fields': fields, 'all_raw_differences_strictly_within_six_correlated_outer_arrays': True, 'all_remaining_case_and_nonprovenance_top_fields_exactly_equal': True, 'all_64_complete_correlated_bundle_shared_permutations_independently_verified': True, 'only_permutation_differences': True, 'mappings': mappings}
native_review = {'schema': 'qcae.sdk8-independent-root-native64-review/1', 'review_method': 'Read completed root evidence only; no native subprocess executed by reviewer.', 'actual_report': str(current_path), 'actual_report_sha256': sha(current_path), 'root_execution_record': str(run_path), 'root_execution_record_sha256': sha(run_path), 'root_comparison': str(comparison_path), 'root_comparison_sha256': sha(comparison_path), 'original_source_sha256': binding['source_sha256'], 'binary_sha256': binding['binary_sha256'], 'all_actual_image_and_font_hashes_verified': True, 'actual_case_count': 64, 'root_native_exit_code': 0, 'primary_raw_original_order_equal': False, 'supplemental_complete_bundle_semantics_equal': True, 'comparisons': results, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False}
(OUT / 'independent-native64-review.json').write_text(json.dumps(native_review, indent=2, ensure_ascii=False) + '\n')
mixed = Path('/private/tmp/qcae-sdk8-mixed-prefix-negative41b')
mixed_report_path = mixed / 'report.json'
mixed_report = json.loads(mixed_report_path.read_text())
mixed_commands_path = mixed / 'actual-commands.json'
commands = json.loads(mixed_commands_path.read_text())
for record in commands:
    assert record['exit_code'] == 0
    assert sha(record['log']) == record['log_sha256']
assert sha(mixed / 'actual-mixed-Qt-probe.cpp') == mixed_report['probe_source_sha256']
assert sha(mixed / 'actual-mixed-Qt-probe') == mixed_report['probe_binary_sha256']
for path, digest in mixed_report['sdk_images_and_inputs_unchanged'].items():
    assert sha(path) == digest
copy = mixed / 'other-runtime/QtGuiSecond.framework/Versions/A/QtGuiSecond'
install_id = subprocess.check_output(['/usr/bin/otool', '-D', str(copy)], text=True)
assert '@rpath/QtGuiSecond.framework/Versions/A/QtGuiSecond' in install_id
log = (mixed / 'actual-runtime.log').read_text()
assert 'actual_second_image=' + str(copy) in log
assert 'candidate_rejected=A second Qt framework installation is loaded' in log
assert 'implemented in both' in log and str(copy) in log
first_red = Path('/private/tmp/qcae-sdk8-mixed-prefix-negative41/actual-runtime.log')
assert 'Invalid manifest/image/symbol fixture was accepted' in first_red.read_text()
mixed_review = {'schema': 'qcae.sdk8-independent-mixed-prefix-review/1', 'review_method': 'Read completed root evidence and Mach-O install-id only; no runtime replay or GUI executed by reviewer.', 'actual_report': str(mixed_report_path), 'actual_report_sha256': sha(mixed_report_path), 'actual_commands': str(mixed_commands_path), 'actual_commands_sha256': sha(mixed_commands_path), 'all_source_binary_log_image_manifest_hashes_verified': True, 'unique_actual_old_QtGui_copy_loaded': True, 'guard_rejected_real_second_framework': True, 'install_id_readback': install_id, 'initial_preparation_red_retained': str(first_red), 'initial_preparation_red_sha256': sha(first_red), 'initial_preparation_red_is_not_a_guard_runtime_pass': True, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False}
(OUT / 'independent-mixed-prefix-review.json').write_text(json.dumps(mixed_review, indent=2, ensure_ascii=False) + '\n')
print(json.dumps({'native64_case_count': 64, 'raw_original_order_equal': False, 'strictly_shared_complete_bundle_permutations_only': True, 'baseline_changed_cases': {k: v['changed_case_count'] for k, v in results.items()}, 'all_actual_hashes_verified': True, 'actual_mixed_prefix_guard_verified': True, 'native_processes_executed_by_reviewer': 0}, indent=2))
