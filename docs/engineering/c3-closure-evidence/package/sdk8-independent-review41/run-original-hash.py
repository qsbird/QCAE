"""Independent SDK8 review: compile the frozen original probe once, headless."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = Path('/Users/qs/Documents/ChatGPT/QCAE')
SDK = Path('/private/tmp/qcae-sdk8-reviewed41')
OUT = Path(__file__).resolve().parent
OLD = ROOT / 'build-c3-qt-observed/tranche8b-private'
PYTHON = Path('/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(name, value):
    (OUT / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def differences(a, b, path='$'):
    result = []
    if type(a) is not type(b):
        return [{'path': path, 'old': a, 'candidate': b}]
    if isinstance(a, dict):
        for key in sorted(a.keys() | b.keys()):
            if key not in a or key not in b:
                result.append({'path': path + '.' + key, 'old': a.get(key), 'candidate': b.get(key)})
            else:
                result.extend(differences(a[key], b[key], path + '.' + key))
    elif isinstance(a, list):
        if len(a) != len(b):
            result.append({'path': path + '.length', 'old': len(a), 'candidate': len(b)})
        for i, (av, bv) in enumerate(zip(a, b)):
            result.extend(differences(av, bv, path + '[' + str(i) + ']'))
    elif a != b:
        result.append({'path': path, 'old': a, 'candidate': b})
    return result


assert Path(sys.executable).resolve() == PYTHON.resolve(), sys.executable
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() == 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4'
source = ROOT / 'tests/qt_freetype_hash_probe.cpp'
binary = OUT / 'original-hash-probe'
images = [SDK / 'qt-prefix/lib' / f'{mod}.framework/Versions/A/{mod}' for mod in ('QtCore', 'QtGui')]
pins = images + [SDK / 'qt-observer-manifest.json', SDK / 'qt-observer-image-binding.json', source]
before = {str(p): sha(p) for p in pins}
old_log = OLD / 'normal-hash-compile.log'
original = json.loads(old_log.read_text().splitlines()[0][len('command: '):])
argv = [x.replace(str(OLD), str(SDK)) for x in original]
argv[argv.index('-o') + 1] = str(binary)
save('compile-command.json', {'argv': argv, 'cwd': str(ROOT), 'old_compile_command_log': str(old_log), 'old_compile_command_log_sha256': sha(old_log), 'source': str(source), 'source_sha256': sha(source), 'python_executable': sys.executable, 'python_version': sys.version, 'compile_attempt': 1, 'compile_limit': 2})
start = time.monotonic()
compiled = subprocess.run(argv, cwd=ROOT, capture_output=True)
(OUT / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
save('compile-result.json', {'exit_code': compiled.returncode, 'elapsed_seconds': time.monotonic() - start, 'log_sha256': sha(OUT / 'compile.log')})
assert compiled.returncode == 0, 'Compile failed; raw log retained'
env_delta = {'QCAE_FT_QT_MANIFEST': str(SDK / 'qt-observer-manifest.json'), 'QT_HASH_SEED': '0', 'DYLD_FRAMEWORK_PATH': str(SDK / 'qt-prefix/lib'), 'DYLD_LIBRARY_PATH': str(SDK / 'qt-prefix/lib'), 'DYLD_PRINT_LIBRARIES': '1'}
env = dict(os.environ)
env.update(env_delta)
report = OUT / 'original-hash-report.json'
run_argv = [str(binary), str(report)]
save('run-command.json', {'argv': run_argv, 'cwd': str(OUT), 'environment': env_delta, 'binary_sha256': sha(binary), 'native_GUI_used': False, 'original_probe_source_unchanged': True})
start = time.monotonic()
result = subprocess.run(run_argv, cwd=OUT, env=env, capture_output=True)
(OUT / 'runtime.stdout.log').write_bytes(result.stdout)
(OUT / 'runtime.stderr.log').write_bytes(result.stderr)
save('run-result.json', {'exit_code': result.returncode, 'elapsed_seconds': time.monotonic() - start, 'stdout_sha256': sha(OUT / 'runtime.stdout.log'), 'stderr_sha256': sha(OUT / 'runtime.stderr.log'), 'binary_sha256': sha(binary)})
assert result.returncode == 0, 'Runtime failed; raw logs retained'
actual = json.loads(report.read_text())
baselines = {'sdk7': ROOT / 'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-headless.json', 'sdk8b_actual': OLD / 'normal-hash-runtime.json', 'sdk8b_archive': ROOT / 'docs/engineering/c3-closure-evidence/package/qt-exception-safe-sdk8-private40/private/normal-hash-runtime.json'}
comparisons = {}
for name, path in baselines.items():
    dest = OUT / ('baseline-' + name + '.json')
    shutil.copyfile(path, dest)
    expected = json.loads(dest.read_text())
    diffs = differences(expected, actual)
    comparisons[name] = {'original_path': str(path), 'original_sha256': sha(path), 'copied_path': str(dest), 'copied_sha256': sha(dest), 'full_parsed_equal': expected == actual, 'full_parsed_differences': diffs, 'all_top_level_fields_compared': sorted(expected), 'all_actual_sites_compared': len(expected.get('actual_sites', []))}
after = {str(p): sha(p) for p in pins}
library_lines = [line for line in result.stderr.decode(errors='replace').splitlines() if 'QtCore.framework/' in line or 'QtGui.framework/' in line]
save('full-behavior-compare.json', {'schema': 'qcae.sdk8-independent-original-hash-review/1', 'source_commit': 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4', 'report_path': str(report), 'report_sha256': sha(report), 'compile_count': 1, 'all_inputs_unchanged': before == after, 'input_hashes_before': before, 'input_hashes_after': after, 'actual_loaded_Qt_image_lines': library_lines, 'comparisons': comparisons, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False, 'native_GUI_used': False})
assert before == after
assert all(str(SDK / 'qt-prefix/lib') in line for line in library_lines)
assert len(library_lines) == 2, library_lines
assert all(item['full_parsed_equal'] for item in comparisons.values()), comparisons
print(json.dumps({'output_directory': str(OUT), 'compile_count': 1, 'compiler_exit': compiled.returncode, 'runtime_exit': result.returncode, 'actual_sites_compared': len(actual['actual_sites']), 'all_three_full_parsed_baselines_equal': True, 'loaded_images': library_lines, 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False}, indent=2))
