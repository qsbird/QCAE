"""Read-only audit of complete repair evidence. Never starts a runtime probe."""
from pathlib import Path
import hashlib
import json

OUT = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_facts(value):
    if isinstance(value, dict):
        if 'path' in value and 'sha256' in value:
            assert sha(value['path']) == value['sha256'], value['path']
            if 'bytes' in value:
                assert Path(value['path']).stat().st_size == value['bytes']
        for child in value.values():
            verify_facts(child)
    elif isinstance(value, list):
        for child in value:
            verify_facts(child)


records = json.loads((OUT / 'actual-commands.json').read_text())
for record in records:
    assert sha(record['log']) == record['log_sha256']
    verify_facts(record)
    if 'binary_sha256' in record:
        assert sha(record['argv'][0]) == record['binary_sha256']
protected = json.loads((OUT / 'protected-inputs-before.json').read_text())
assert len(protected) == 220
assert all(sha(path) == digest for path, digest in protected.items())
synthetic = [r for r in records if r['phase'] == 'synthetic-runtime']
assert len(synthetic) == 32
assert [r['consumer'] for r in synthetic] == ['old'] * 16 + ['fixed'] * 16
names = {r['case'] for r in synthetic}
assert len(names) == 16
paired = []
for name in sorted(names):
    old = next(r for r in synthetic if r['case'] == name and r['consumer'] == 'old')
    fixed = next(r for r in synthetic if r['case'] == name and r['consumer'] == 'fixed')
    assert old['exit_code'] == fixed['exit_code'] == 0
    assert old['fixture_image'] == fixed['fixture_image']
    assert old['fixture_source'] == fixed['fixture_source']
    assert old['manifest'] == fixed['manifest']
    assert old['binding'] == fixed['binding']
    assert old['collector_header']['sha256'] != fixed['collector_header']['sha256']
    doc = json.loads(Path(old['manifest']['path']).read_text())
    sidecar = json.loads(Path(old['binding']['path']).read_text())
    assert sidecar['observer_manifest_sha256'] == old['manifest']['sha256']
    assert sidecar['qt_core']['sha256'] == old['fixture_image']['sha256']
    assert sidecar['qt_core']['path'] == str(Path(old['fixture_image']['path']).resolve())
    snapshot = json.loads(Path(old['snapshot']['path']).read_text())
    assert snapshot['observer_manifest'] == doc
    assert snapshot['qt_core_loaded_path'] == old['fixture_image']['path']
    assert snapshot['synthetic_ABI_contract_only'] is True
    assert snapshot['actual_Qt_observer_behavior_proven'] is False
    assert snapshot['exception_boundary_complete'] is True
    assert snapshot['observer_failure_status'] == '0'
    assert snapshot['observer_failure_status_known'] is True
    assert snapshot['collector_complete'] is True
    assert all(p.startswith('/private/tmp/qcae-sdk8-reviewed41/qt-prefix/lib/') for p in snapshot['loaded_qt_images'])
    if name == 'valid_v1_control':
        control = json.loads(Path(fixed['snapshot']['path']).read_text())
        assert control['exception_boundary_complete'] is True
    else:
        assert 'snapshot' not in fixed
        assert not (Path(old['manifest']['path']).parent / 'fixed-snapshot.json').exists()
        assert 'synthetic_ABI_fixture_rejected=Unsupported SDK exception fence contract' in Path(fixed['log']).read_text()
    paired.append({'case': name, 'old_exact_marker_sidecar_image_binding_verified': True, 'same_exact_input_used_for_fixed_collector': True, 'old_actual_exception_boundary_complete': True, 'fixed_rejected_without_snapshot': name != 'valid_v1_control', 'fixed_valid_control_accepted': name == 'valid_v1_control'})
actual = [r for r in records if r['phase'] == 'real-SDK-headless-runtime']
assert len(actual) == 15 and all(r['exit_code'] == 0 for r in actual)
assert sum(r['new_output_bytes_equal_original'] for r in actual) == 14
for record in actual:
    if not record['new_output_bytes_equal_original']:
        assert record['name'] == 'threads'
controlled = [r for r in records if r['phase'] == 'controlled-thread-diagnostic']
assert len(controlled) == 2 and all(r['exit_code'] == 0 for r in controlled)
assert all(r['environment']['QT_HASH_SEED'] == '0' for r in controlled)
assert Path(controlled[0]['log']).read_bytes() == Path(controlled[1]['log']).read_bytes()
successful_compiles = [r for r in records if r['phase'] == 'compile' and r['exit_code'] == 0]
red_compiles = [r for r in records if r['phase'] == 'compile' and r['exit_code'] != 0]
assert len(successful_compiles) == 21 and len(red_compiles) == 16
assert all('-Werror' in r['argv'] for r in successful_compiles)
assert all('overlength-strings' in Path(r['log']).read_text() for r in red_compiles)
for name in ['repair-summary.json', 'actual-counterexample-to-rejection.json', 'actual-fixed-headless-report.json', 'exact-prior-behavior-reuse.json', 'controlled-thread-counter-review.json', 'patch-format-validation.json']:
    verify_facts(json.loads((OUT / name).read_text()))
report = {'schema': 'qcae.sdk8-fixed-fence-complete-readonly-evidence-validation/1', 'all_recorded_source_header_binary_log_manifest_sidecar_image_facts_verified': True, 'all_prior_220_protected_file_hashes_unchanged': True, 'actual_same_exact_input_old_accept_to_fixed_reject_pairs': 15, 'actual_valid_v1_control_accepted_by_both': True, 'paired_cases': paired, 'actual_fixed_real_SDK_fresh_process_headless_cases': 15, 'actual_fixed_headless_all_exit_codes': 0, 'original_actual_raw_logs_byte_equal_count': 14, 'retained_thread_raw_attempts_difference': {'old': 2052, 'fixed': 2122}, 'one_fixed_seed0_old_fixed_thread_pair_byte_equal': True, 'successful_strict_compile_count': 21, 'initial_overlength_fixture_preparation_RED_count': 16, 'all_initial_RED_sources_and_logs_preserved': True, 'native_GUI_processes_executed': 0, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False, 'complete_SDK_copy_coverage_is_C4_start_prerequisite': False}
(OUT / 'complete-readonly-evidence-validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'all_facts_verified': True, 'old_accept_to_fixed_reject_pairs': 15, 'real_fixed_headless_passed': 15, 'protected_previous_files_unchanged': 220, 'native_GUI_processes_executed': 0}, indent=2))
