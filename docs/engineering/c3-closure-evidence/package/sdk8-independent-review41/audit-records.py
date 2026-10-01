"""Verify retained SDK8 records and independently replay headless argv only."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

OUT = Path(__file__).resolve().parent
SDK = Path('/private/tmp/qcae-sdk8-reviewed41')
ROOT = Path('/Users/qs/Documents/ChatGPT/QCAE')
PYTHON = Path('/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def save(name, obj):
    (OUT / name).write_text(json.dumps(obj, indent=2, ensure_ascii=False) + '\n')


assert Path(sys.executable).resolve() == PYTHON.resolve()
all_checks = []
replays = []
pins = {}
for group in ['headless', 'binding-negatives', 'contract-negatives']:
    directory = Path('/private/tmp/qcae-sdk8-' + group + '41')
    command_file = directory / 'actual-commands.json'
    pins[str(command_file)] = sha(command_file)
    report_file = directory / 'report.json'
    pins[str(report_file)] = sha(report_file)
    records = json.loads(command_file.read_text())
    report = json.loads(report_file.read_text())
    for key in ('images_unchanged', 'sdk_inputs_unchanged', 'real_sdk_inputs_unchanged'):
        for path, expected in report.get(key, {}).items():
            actual = sha(path)
            all_checks.append({'group': group, 'kind': 'reported_input_sha256', 'path': path, 'expected': expected, 'actual': actual, 'matched': actual == expected})
            pins[path] = actual
    for i, record in enumerate(records):
        name = record.get('name', record.get('case'))
        if group == 'binding-negatives':
            files = {'binary_sha256': directory / name / 'collector_negotiation_probe', 'compile_log_sha256': directory / name / 'compile.log', 'runtime_log_sha256': directory / name / 'actual-runtime.log'}
            argv = record['runtime_argv']
            original_log = files['runtime_log_sha256']
            runtime_exit = record['runtime_exit_code']
            assert record['compile_exit_code'] == 0
            all_checks.append({'group': group, 'case': name, 'kind': 'compile_and_runtime_exit', 'compile_exit_code': record['compile_exit_code'], 'runtime_exit_code': runtime_exit, 'compile_argv': record['compile_argv'], 'runtime_argv': argv, 'expected_rejection': record['expected_rejection']})
        else:
            files = {'log_sha256': Path(record['log'])}
            if 'binary_sha256' in record:
                files['binary_sha256'] = Path(record['argv'][0])
            if 'fixture_binary_sha256' in record:
                files.update({'fixture_binary_sha256': Path(record['argv'][2]), 'fixture_source_sha256': directory / name / 'negative-abi-fixture.cpp', 'manifest_sha256': Path(record['argv'][1]), 'binding_sha256': directory / name / 'image-binding.json', 'probe_sha256': Path(record['argv'][0])})
            all_checks.append({'group': group, 'case': name, 'phase': record.get('phase', 'runtime'), 'kind': 'recorded_exit_and_argv', 'argv': record['argv'], 'exit_code': record['exit_code']})
            assert record['exit_code'] == 0
            if group == 'contract-negatives' and 'environment' not in record:
                argv = None
            else:
                argv = record['argv']
                original_log = files['log_sha256']
                runtime_exit = record['exit_code']
        for field, path in files.items():
            actual = sha(path)
            expected = record[field]
            all_checks.append({'group': group, 'case': name, 'kind': field, 'path': str(path), 'actual': actual, 'expected': expected, 'matched': actual == expected})
            pins[str(path)] = actual
        if argv is not None:
            env = dict(os.environ)
            env.update(record['environment'])
            start = time.monotonic()
            result = subprocess.run(argv, cwd=OUT, env=env, capture_output=True)
            log = OUT / ('replay-' + group + '-' + name + '.log')
            log.write_bytes(result.stdout + result.stderr)
            text = log.read_text()
            expected_rejection = record.get('expected_rejection', argv[-1] if group == 'contract-negatives' and argv[3] == 'reject' else '')
            assert result.returncode == runtime_exit == 0, name
            if expected_rejection:
                assert expected_rejection in text, name
            replays.append({'group': group, 'case': name, 'argv': argv, 'environment': record['environment'], 'recorded_exit_code': runtime_exit, 'independent_exit_code': result.returncode, 'elapsed_seconds': time.monotonic() - start, 'independent_log': str(log), 'independent_log_sha256': sha(log), 'original_log': str(original_log), 'original_log_sha256': sha(original_log), 'independent_output_bytes_equal_original': log.read_bytes() == original_log.read_bytes(), 'expected_rejection': expected_rejection, 'synthetic_ABI_only': group == 'contract-negatives'})
assert all(c.get('matched', True) for c in all_checks)
assert all(sha(path) == digest for path, digest in pins.items())
save('independent-evidence-audit.json', {'schema': 'qcae.sdk8-independent-evidence-audit/1', 'source_commit': 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4', 'python_executable': sys.executable, 'checks': all_checks, 'all_sha_checks_match': True, 'pinned_inputs_after_replay_unchanged': True, 'independent_replays': replays, 'headless_replayed': sum(r['group'] == 'headless' for r in replays), 'real_binding_replayed': sum(r['group'] == 'binding-negatives' for r in replays), 'synthetic_contract_replayed': sum(r['group'] == 'contract-negatives' for r in replays), 'native_GUI_used': False, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False, 'scope': 'Real Qt ABI/runtime headless and real binding failures are separate from synthetic contract guard cases; no GUI behavior or whole-copy proof inferred.'})
print(json.dumps({'all_sha_checks_match': True, 'sha_checks': sum('matched' in c for c in all_checks), 'independent_replays': len(replays), 'headless': 15, 'real_binding': 7, 'synthetic_contract': 5, 'all_replay_exit_codes': 0, 'output_bytes_equal_original_count': sum(r['independent_output_bytes_equal_original'] for r in replays), 'report': str(OUT / 'independent-evidence-audit.json')}, indent=2))
