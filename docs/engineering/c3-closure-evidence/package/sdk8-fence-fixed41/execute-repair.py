"""Finite, headless-only SDK8 fence repair validation. No SDK image is rebuilt."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import copy
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

OUT = Path(__file__).resolve().parent
ROOT = Path('/Users/qs/Documents/ChatGPT/QCAE')
SDK = Path('/private/tmp/qcae-sdk8-reviewed41')
ADOPTION = Path('/private/tmp/qcae-sdk8-adoption41')
PREVIOUS = Path('/private/tmp/qcae-sdk8-independent-review41')
PYTHON = Path('/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14')
FORMATTER = Path('/Library/Developer/CommandLineTools/usr/bin/clang-format')
RESUME_COMPACT = sys.argv[1:] == ['--resume-compact-fixtures']
RECORDS = json.loads((OUT / 'actual-commands.json').read_text()) if RESUME_COMPACT else []


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(name, value):
    (OUT / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def fact(path):
    path = Path(path)
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)}


def execute(label, argv, log, env_delta=None, extra=None):
    env = dict(os.environ)
    env.update(env_delta or {})
    before = time.monotonic()
    run = subprocess.run(argv, cwd=OUT, env=env, capture_output=True)
    log.write_bytes(run.stdout + run.stderr)
    result = {'label': label, 'argv': argv, 'cwd': str(OUT), 'environment': env_delta or {}, 'exit_code': run.returncode, 'elapsed_seconds': time.monotonic() - before, 'log': str(log), 'log_sha256': sha(log)}
    result.update(extra or {})
    return result


def compile_many(jobs):
    # This is the only compile dispatcher. It never has more than two workers.
    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(lambda j: execute(j['label'], j['argv'], j['log'], extra=j['extra']), jobs))
    RECORDS.extend(results)
    save('actual-commands.json', RECORDS)
    assert all(r['exit_code'] == 0 for r in results), [r['label'] for r in results if r['exit_code']]
    return results


def compile_job(label, argv, log, sources, headers=()):
    return {'label': label, 'argv': argv, 'log': log, 'extra': {'phase': 'compile', 'input_sources': [fact(p) for p in sources], 'input_headers': [fact(p) for p in headers], 'compiler_parallelism_limit': 2}}


def compiler_base(plan, source):
    argv = []
    index = 0
    while index < len(plan['argv']):
        value = plan['argv'][index]
        if value == plan['source']:
            index += 1
            continue
        if value == '-o':
            index += 2
            continue
        argv.append(value.replace(str(ADOPTION), str(OUT)))
        index += 1
    return argv + [str(source)]


assert Path(sys.executable).resolve() == PYTHON.resolve()
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() == 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4'
assert sha(OUT / 'original-collector.hpp') == sha(ADOPTION / 'collector-optional-status.hpp')
protected_dirs = [ADOPTION, PREVIOUS, Path('/private/tmp/qcae-sdk8-headless41'), Path('/private/tmp/qcae-sdk8-binding-negatives41'), Path('/private/tmp/qcae-sdk8-contract-negatives41'), Path('/private/tmp/qcae-sdk8-mixed-prefix-negative41'), Path('/private/tmp/qcae-sdk8-mixed-prefix-negative41b')]
protected = json.loads((OUT / 'protected-inputs-before.json').read_text()) if RESUME_COMPACT else {str(p): sha(p) for directory in protected_dirs for p in sorted(directory.rglob('*')) if p.is_file()}
for name in ('full-behavior-compare.json', 'native64-prepared/preparation-binding.json'):
    old = json.loads((PREVIOUS / name).read_text())
    if name == 'full-behavior-compare.json':
        protected.update(old['input_hashes_after'])
    else:
        protected.update(old['candidate_images'])
        protected.update(old['font_files'])
        protected[old['source']] = old['source_sha256']
for path, digest in json.loads(Path('/private/tmp/qcae-sdk8-headless41/report.json').read_text())['images_unchanged'].items():
    assert sha(path) == digest
    protected[path] = digest
protected[str(ROOT / 'tests/c3_qt_source_bridge.hpp')] = sha(ROOT / 'tests/c3_qt_source_bridge.hpp')
if not RESUME_COMPACT:
    save('protected-inputs-before.json', protected)
else:
    assert all(sha(path) == digest for path, digest in protected.items())
print('Protected previous source, headers, candidate images and all prior evidence:', len(protected), flush=True)

manifest = json.loads((SDK / 'qt-observer-manifest.json').read_text())
variants = [
    ('boundary_false', 'noexcept_callback_boundary', False),
    ('boundary_missing', 'noexcept_callback_boundary', None),
    ('boundary_string', 'noexcept_callback_boundary', 'true'),
    ('boundary_number', 'noexcept_callback_boundary', 1),
    ('scope_layout_false', 'original_C_declarations_and_scope_layout_unchanged', False),
    ('scope_layout_missing', 'original_C_declarations_and_scope_layout_unchanged', None),
    ('scope_layout_string', 'original_C_declarations_and_scope_layout_unchanged', 'true'),
    ('scope_layout_number', 'original_C_declarations_and_scope_layout_unchanged', 1),
    ('capacity_zero', 'balanced_admission_stack_capacity', 0),
    ('capacity_missing', 'balanced_admission_stack_capacity', None),
    ('capacity_string', 'balanced_admission_stack_capacity', '1024'),
    ('capacity_fraction', 'balanced_admission_stack_capacity', 1024.5),
    ('capacity_negative', 'balanced_admission_stack_capacity', -1),
    ('capacity_boolean', 'balanced_admission_stack_capacity', True),
    ('capacity_wrong_positive', 'balanced_admission_stack_capacity', 2048),
    ('valid_v1_control', None, None),
]
fixture_root = OUT / ('synthetic-fixtures-compact' if RESUME_COMPACT else 'synthetic-fixtures')
fixture_root.mkdir()
fixtures = []
for name, field, value in variants:
    directory = fixture_root / name
    directory.mkdir()
    doc = ({'schema': 1, 'qt_version': '6.11.1', 'scope': 'Synthetic ABI negotiation metadata only; no actual Qt callback behavior', 'sites': [{'site': 'synthetic/contract_guard_only'}], 'observer_failure_fence': copy.deepcopy(manifest['observer_failure_fence'])} if RESUME_COMPACT else copy.deepcopy(manifest))
    if field:
        if value is None:
            del doc['observer_failure_fence'][field]
        else:
            doc['observer_failure_fence'][field] = value
    marker = json.dumps(doc, ensure_ascii=True, sort_keys=True, separators=(',', ':'))
    assert ')QCAEFENCE"' not in marker
    expected = directory / 'exact-embedded-manifest.json'
    expected.write_text(marker + '\n')
    source = directory / 'synthetic-abi-fixture.cpp'
    source.write_text('''// Synthetic contract metadata provider; no Qt callback behavior is proven.
#include <cstdint>
using Callback = void (*)(void*, const char*, unsigned, std::uint64_t);
extern "C" void qcae_qt_sdk_observer_install(Callback, void*) {}
extern "C" const char* qcae_qt_sdk_observer_manifest() {
    return R"QCAEFENCE(''' + marker + ''')QCAEFENCE";
}
extern "C" std::uint64_t qcae_qt_sdk_observer_status() noexcept { return 0; }
extern "C" unsigned qcae_qt_sdk_observer_depth() noexcept { return 0; }
extern "C" void qcae_qt_sdk_observer_collector_loss() noexcept {}
''')
    fixtures.append({'name': name, 'field': field, 'value': value, 'directory': directory, 'source': source, 'image': directory / 'synthetic-abi-fixture.dylib', 'manifest': expected, 'control': field is None})

formatter_version = subprocess.check_output([str(FORMATTER), '--version'], text=True)
assert 'clang-format version 21.' in formatter_version
format_sources = [OUT / 'collector-optional-status.hpp', OUT / 'contradiction-probe.cpp'] + [f['source'] for f in fixtures]
formatted = execute('format-new-private-sources', [str(FORMATTER), '-i', *map(str, format_sources)], OUT / ('format-resume.log' if RESUME_COMPACT else 'format.log'), extra={'phase': 'format', 'formatter_version': formatter_version.strip()})
RECORDS.append(formatted)
assert formatted['exit_code'] == 0
assert sha(OUT / 'original-collector.hpp') == sha(ADOPTION / 'collector-optional-status.hpp')
import difflib
old_text = (OUT / 'original-collector.hpp').read_text()
fixed_text = (OUT / 'collector-optional-status.hpp').read_text()
(OUT / 'collector-fence-semantic-fix.patch').write_text(''.join(difflib.unified_diff(old_text.splitlines(keepends=True), fixed_text.splitlines(keepends=True), fromfile='a/collector-optional-status.hpp', tofile='b/collector-optional-status.hpp')))
(OUT / 'collector-full-adoption.patch').write_text(''.join(difflib.unified_diff((ROOT / 'tests/c3_qt_source_bridge.hpp').read_text().splitlines(keepends=True), fixed_text.splitlines(keepends=True), fromfile='a/tests/c3_qt_source_bridge.hpp', tofile='b/tests/c3_qt_source_bridge.hpp')))
dry_format = execute('check-new-private-source-format', [str(FORMATTER), '--dry-run', '--Werror', *map(str, format_sources)], OUT / ('format-check-resume.log' if RESUME_COMPACT else 'format-check.log'), extra={'phase': 'format-check'})
RECORDS.append(dry_format)
assert dry_format['exit_code'] == 0

new_plans = []
plan8 = json.loads(Path('/private/tmp/qcae-sdk8-reviewed41-probes/compile-plan.json').read_text())
plan7 = json.loads(Path('/private/tmp/qcae-sdk7-legacy41-probes/compile-plan.json').read_text())
for variant, plans, sdk in [('sdk7', plan7, ROOT / 'build-c3-qt-observed/tranche7'), ('sdk8', plan8, SDK)]:
    directory = OUT / ('actual-' + variant)
    directory.mkdir(exist_ok=RESUME_COMPACT)
    (directory / 'qt.conf').write_text('[Paths]\nPrefix=' + str(sdk / 'qt-prefix') + '\n')
    for plan in plans:
        source = OUT / Path(plan['source']).name
        if not source.exists():
            shutil.copyfile(plan['source'], source)
        assert sha(source) == plan['source_sha256']
        binary = directory / source.stem
        argv = compiler_base(plan, source) + ['-o', str(binary)]
        new_plans.append({'variant': variant, 'source': source, 'binary': binary, 'argv': argv, 'directory': directory})
fixture_consumers = OUT / 'fixture-consumers'
fixture_consumers.mkdir(exist_ok=RESUME_COMPACT)
(fixture_consumers / 'qt.conf').write_text('[Paths]\nPrefix=' + str(SDK / 'qt-prefix') + '\n')
fixture_base = compiler_base(plan8[0], OUT / 'contradiction-probe.cpp')
fixture_base = [v for v in fixture_base if not v.startswith('-DQCAE_C3_QT_SDK_IMAGE_BINDING=')]
consumer_jobs = []
for label, header in [('old', OUT / 'original-collector.hpp'), ('fixed', OUT / 'collector-optional-status.hpp')]:
    binary = fixture_consumers / (label + '-contradiction-probe')
    argv = fixture_base + ['-DQCAE_TEST_COLLECTOR_HEADER="' + str(header) + '"', '-o', str(binary)]
    consumer_jobs.append(compile_job('compile-' + label + '-fixture-consumer', argv, fixture_consumers / (label + '-compile.log'), [OUT / 'contradiction-probe.cpp'], [header]))
if not RESUME_COMPACT:
    compile_many(consumer_jobs)
else:
    assert all(Path(j['argv'][j['argv'].index('-o') + 1]).is_file() for j in consumer_jobs)
print('Compiled old and fixed synthetic contract consumers; no fixture runtime yet.', flush=True)
fixture_jobs = [compile_job('compile-fixture-' + f['name'], ['/usr/bin/c++', '-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-dynamiclib', str(f['source']), '-o', str(f['image'])], f['directory'] / 'compile.log', [f['source']]) for f in fixtures]
compile_many(fixture_jobs)
for fixture in fixtures:
    binding = fixture['directory'] / 'image-binding.json'
    binding.write_text(json.dumps({'schema': 'qcae.qt-sdk-observer-image-binding/1', 'observer_manifest_sha256': sha(fixture['manifest']), 'qt_core': {'path': str(fixture['image'].resolve()), 'sha256': sha(fixture['image'])}}, indent=2) + '\n')
    fixture['binding'] = binding
print('Compiled 15 contradictory exact-marker ABI fixtures plus valid v1 control.', flush=True)

fixture_results = []
base_environment = {'DYLD_FRAMEWORK_PATH': str(SDK / 'qt-prefix/lib'), 'DYLD_LIBRARY_PATH': str(SDK / 'qt-prefix/lib'), 'QT_PLUGIN_PATH': str(SDK / 'qt-prefix/plugins')}
for consumer in ('old', 'fixed'):
    for fixture in fixtures:
        mode = 'accept' if consumer == 'old' or fixture['control'] else 'reject'
        snapshot_path = fixture['directory'] / (consumer + '-snapshot.json')
        binary = fixture_consumers / (consumer + '-contradiction-probe')
        argv = [str(binary), str(fixture['manifest']), str(fixture['image']), mode, str(snapshot_path)]
        environment = dict(base_environment, QCAE_FIXTURE_BINDING=str(fixture['binding']))
        record = execute(consumer + '-' + fixture['name'], argv, fixture['directory'] / (consumer + '-runtime.log'), environment, {'phase': 'synthetic-runtime', 'consumer': consumer, 'case': fixture['name'], 'binary_sha256': sha(binary), 'collector_header': fact(OUT / ('original-collector.hpp' if consumer == 'old' else 'collector-optional-status.hpp')), 'fixture_source': fact(fixture['source']), 'fixture_image': fact(fixture['image']), 'manifest': fact(fixture['manifest']), 'binding': fact(fixture['binding']), 'synthetic_ABI_only': True, 'actual_Qt_observer_behavior_proven': False})
        RECORDS.append(record)
        save('actual-commands.json', RECORDS)
        assert record['exit_code'] == 0, record
        if mode == 'accept':
            snapshot = json.loads(snapshot_path.read_text())
            assert snapshot['exception_boundary_complete'] is True and snapshot['observer_failure_status'] == '0' and snapshot['observer_failure_status_known'] is True
            record['snapshot'] = fact(snapshot_path)
        else:
            assert not snapshot_path.exists(), 'Rejected fixture produced a complete snapshot'
            assert 'synthetic_ABI_fixture_rejected=Unsupported SDK exception fence contract' in Path(record['log']).read_text()
        fixture_results.append({'case': fixture['name'], 'changed_field': fixture['field'], 'changed_value': fixture['value'], 'missing_field': fixture['field'] is not None and fixture['value'] is None, 'consumer': consumer, 'expected_mode': mode, 'exit_code': record['exit_code'], 'exception_boundary_complete': True if mode == 'accept' else None, 'complete_snapshot_produced': snapshot_path.exists(), 'raw_log': fact(record['log']), 'synthetic_ABI_only': True})
    print('Actual synthetic contract results for ' + consumer + ': ' + str(len(fixtures)) + ' finite fresh-process cases.', flush=True)
save('actual-counterexample-to-rejection.json', {'schema': 'qcae.sdk8-actual-exact-marker-semantic-contradictions/1', 'contradictory_case_count': 15, 'old_all_15_accepted_with_exception_boundary_complete_true': True, 'fixed_all_15_rejected_without_snapshot': True, 'valid_v1_control_accepted_by_both': True, 'runtime_results': fixture_results, 'scope': 'Synthetic ABI providers prove contract guard behavior only; no Qt callback/exception/data behavior follows.', 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False})

actual_jobs = [compile_job('compile-actual-' + p['variant'] + '-' + p['source'].stem, p['argv'], p['directory'] / (p['source'].name + '.compile.log'), [p['source']], [OUT / 'collector-optional-status.hpp']) for p in new_plans]
compile_many(actual_jobs)
print('Compiled unchanged real SDK7 negotiation and SDK8 negotiation/exception sources with fixed collector.', flush=True)
original_commands = json.loads(Path('/private/tmp/qcae-sdk8-headless41/actual-commands.json').read_text())
actual_results = []
for original in original_commands:
    name = original['name']
    argv = list(original['argv'])
    variant = 'sdk7' if name == 'legacy_normal' else 'sdk8'
    argv[0] = str(OUT / ('actual-' + variant) / Path(argv[0]).name)
    binary = Path(argv[0])
    record = execute('actual-fixed-' + name, argv, OUT / (name + '.log'), original['environment'], {'phase': 'real-SDK-headless-runtime', 'name': name, 'binary_sha256': sha(binary), 'collector_header': fact(OUT / 'collector-optional-status.hpp'), 'original_record': original, 'actual_sdk': variant, 'current_inherited_QT_HASH_SEED': os.environ.get('QT_HASH_SEED'), 'synthetic_ABI_only': False})
    RECORDS.append(record)
    save('actual-commands.json', RECORDS)
    assert record['exit_code'] == 0, record
    original_bytes = Path(original['log']).read_bytes()
    record['new_output_bytes_equal_original'] = Path(record['log']).read_bytes() == original_bytes
    actual_results.append({'name': name, 'exit_code': record['exit_code'], 'raw_log': fact(record['log']), 'new_binary_sha256': sha(binary), 'all_original_behavior_assertions_executed': True, 'new_output_bytes_equal_original': record['new_output_bytes_equal_original']})
print('Actual fixed collector real SDK headless: 15/15 passed.', flush=True)
save('actual-fixed-headless-report.json', {'schema': 'qcae.sdk8-fixed-fence-real-headless/1', 'real_SDK_fresh_process_case_count': 15, 'all_case_exit_codes': 0, 'all_original_assertions_executed': True, 'all_raw_logs_byte_equal_original': all(r['new_output_bytes_equal_original'] for r in actual_results), 'results': actual_results, 'SDK_images_rebuilt': False, 'native_GUI_used': False, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False})

changed = [{'path': p, 'expected_sha256': digest, 'current_sha256': sha(p) if Path(p).exists() else None} for p, digest in protected.items() if not Path(p).exists() or sha(p) != digest]
assert not changed, changed
save('protected-inputs-after.json', {'all_previous_sources_headers_SDK_images_and_actual_evidence_unchanged': True, 'protected_file_count': len(protected), 'hashes': protected, 'changes': changed})
hash_report = json.loads((PREVIOUS / 'full-behavior-compare.json').read_text())
native_binding = json.loads((PREVIOUS / 'native64-prepared/preparation-binding.json').read_text())
native_review = json.loads((PREVIOUS / 'independent-native64-review.json').read_text())
assert all(sha(p) == digest for p, digest in hash_report['input_hashes_after'].items())
assert sha(native_binding['source']) == native_binding['source_sha256'] and sha(native_binding['binary']) == native_binding['binary_sha256']
assert all(sha(p) == digest for p, digest in native_binding['candidate_images'].items())
assert all(sha(p) == digest for p, digest in native_binding['font_files'].items())
reuse = {'schema': 'qcae.sdk8-fixed-fence-exact-prior-behavior-reuse/1', 'reuse_reason': 'Only the optional collector manifest negotiation changes. Both original 33-site hash and direct native64 behavior probes do not include this collector; their exact source, binary, producer SDK image/manifest/helper pins, reports, commands, logs and required font files remain unchanged.', 'original_33_site_headless_report': fact(PREVIOUS / 'original-hash-report.json'), 'full_33_site_comparison': fact(PREVIOUS / 'full-behavior-compare.json'), 'original_33_site_binary': fact(PREVIOUS / 'original-hash-probe'), 'original_33_site_compiler_command': fact(PREVIOUS / 'compile-command.json'), 'original_33_site_runtime_command': fact(PREVIOUS / 'run-command.json'), 'original_native64_source': fact(native_binding['source']), 'original_native64_binary': fact(native_binding['binary']), 'original_native64_report': fact(PREVIOUS / 'native64-prepared/actual-native64.json'), 'original_native64_execution': fact(PREVIOUS / 'native64-prepared/root-execution-record.json'), 'original_native64_full_comparison': fact(PREVIOUS / 'native64-prepared/root-full-comparison.json'), 'original_native64_independent_review': fact(PREVIOUS / 'independent-native64-review.json'), 'all_required_source_binary_image_manifest_helper_and_font_pins_unchanged': True, 'original_33_site_full_parsed_equality_to_three_baselines_retained': True, 'native64_raw_original_order_equal_retained': False, 'native64_supplemental_complete_shared_bundle_semantics_equal_retained': True, 'new_hash_runtime_executed': False, 'new_native_runtime_executed': False, 'prior_evidence_is_not_claimed_as_new_runtime': True, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False}
save('exact-prior-behavior-reuse.json', reuse)
save('actual-commands.json', RECORDS)
compile_count = sum(r['phase'] == 'compile' for r in RECORDS)
save('repair-summary.json', {'schema': 'qcae.sdk8-fence-fixed-candidate/1', 'source_commit': 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4', 'fixed_collector': fact(OUT / 'collector-optional-status.hpp'), 'old_collector': fact(OUT / 'original-collector.hpp'), 'minimal_patch': fact(OUT / 'collector-fence-semantic-fix.patch'), 'full_adoption_patch': fact(OUT / 'collector-full-adoption.patch'), 'three_semantic_guard_fields_added': ['noexcept_callback_boundary=true', 'original_C_declarations_and_scope_layout_unchanged=true', 'balanced_admission_stack_capacity=numeric1024'], 'actual_contradictory_ABI_cases_old_accept_then_fixed_reject': 15, 'actual_valid_ABI_control_accepted_by_both': True, 'actual_fixed_real_SDK_headless_cases_passed': 15, 'compile_count': compile_count, 'successful_compile_count': sum(r['phase'] == 'compile' and r['exit_code'] == 0 for r in RECORDS), 'initial_fixture_preparation_compile_RED_count': sum(r['phase'] == 'compile' and r['exit_code'] != 0 for r in RECORDS), 'compile_parallelism_limit': 2, 'compiler_warnings': 'All final successful strict -Werror compilations returned zero. Initial full-manifest fixture overlength-string RED source/log/argv retained separately; compact exact-marker fixtures used for final contract proofs.', 'protected_previous_file_count': len(protected), 'all_previous_sources_headers_SDK_images_and_evidence_unchanged': True, 'native_GUI_used': False, 'repository_product_sources_modified': False, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False})
print(json.dumps({'directory': str(OUT), 'old_accept_fixed_reject_cases': 15, 'real_fixed_headless_passed': 15, 'compile_count': compile_count, 'maximum_parallel_compilers': 2, 'all_previous_protected_files_unchanged': len(protected), 'actual_15_raw_log_byte_equality': all(r['new_output_bytes_equal_original'] for r in actual_results), 'native_GUI_used': False}, indent=2), flush=True)
