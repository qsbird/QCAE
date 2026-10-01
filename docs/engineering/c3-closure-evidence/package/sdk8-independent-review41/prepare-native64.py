"""Compile original native64 behavior probe; never execute a GUI process."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys
import time

ROOT = Path('/Users/qs/Documents/ChatGPT/QCAE')
SDK = Path('/private/tmp/qcae-sdk8-reviewed41')
OUT = Path(__file__).resolve().parent / 'native64-prepared'
OLD = ROOT / 'build-c3-qt-observed/tranche7'
PYTHON = Path('/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def save(name, obj):
    (OUT / name).write_text(json.dumps(obj, indent=2, ensure_ascii=False) + '\n')


assert Path(sys.executable).resolve() == PYTHON.resolve()
OUT.mkdir(exist_ok=False)
source = ROOT / 'tests/qt_font_backend_behavior_probe.cpp'
reviewed = ROOT / 'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-independent-review/reviewed-source-sha256.json'
old_source_pin = next(x for x in json.loads(reviewed.read_text()) if x['path'] == 'tests/qt_font_backend_behavior_probe.cpp')
assert sha(source) == old_source_pin['sha256']
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() == 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4'
old_command = OLD / 'native-probe-compile.json'
command = json.loads(old_command.read_text())['command']
binary = OUT / 'original-native64-probe'
argv = [x.replace(str(OLD), str(SDK)) for x in command]
argv[argv.index(str(ROOT / 'tests/qt_freetype_store_probe.cpp'))] = str(source)
argv[argv.index('-o') + 1] = str(binary)
save('compile-command.json', {'argv': argv, 'cwd': str(ROOT), 'original_command': command, 'original_command_path': str(old_command), 'original_command_sha256': sha(old_command), 'source': str(source), 'source_sha256': sha(source), 'source_matches_original_reviewed_pin': True, 'old_source_pin_file': str(reviewed), 'old_source_pin_file_sha256': sha(reviewed), 'python_executable': sys.executable, 'compile_attempt_in_independent_review': 2, 'compile_limit': 2, 'original_probe_compiled_directly': True})
start = time.monotonic()
result = subprocess.run(argv, cwd=ROOT, capture_output=True)
(OUT / 'compile.log').write_bytes(result.stdout + result.stderr)
save('compile-result.json', {'exit_code': result.returncode, 'elapsed_seconds': time.monotonic() - start, 'log_sha256': sha(OUT / 'compile.log')})
assert result.returncode == 0
(OUT / 'qt.conf').write_text('[Paths]\nPrefix=' + str(SDK / 'qt-prefix') + '\n')
baselines = {'sdk7': ROOT / 'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-causal-64.json', 'sdk8b': ROOT / 'docs/engineering/c3-closure-evidence/package/qt-exception-safe-sdk8-private40/native64/causal-64.json'}
binding = {'schema': 'qcae.sdk8-native64-preparation/1', 'source_commit': 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4', 'source': str(source), 'source_sha256': sha(source), 'binary': str(binary), 'binary_sha256': sha(binary), 'qt_conf': str(OUT / 'qt.conf'), 'qt_conf_sha256': sha(OUT / 'qt.conf'), 'compile_exit_code': 0, 'runtime_executed': False, 'native_GUI_used_by_preparer': False, 'baselines': {}, 'candidate_images': {}, 'font_files': {}, 'preserved_case_fields': [], 'isolated_provenance_fields': ['run_id', 'started_utc', 'qt_libraries_path', 'loaded_sdk_images'], 'whole_pipeline_owned_copy_coverage': 'unknown', 'complete_c3_passed': False, 'complete_sk12_passed': False}
for name, p in baselines.items():
    copy = OUT / ('baseline-' + name + '-64.json')
    shutil.copyfile(p, copy)
    doc = json.loads(copy.read_text())
    assert doc['case_count'] == len(doc['cases']) == 64 and doc['probe_completed'] and doc['backend_verified']
    binding['baselines'][name] = {'original_path': str(p), 'original_sha256': sha(p), 'copy_path': str(copy), 'copy_sha256': sha(copy), 'case_ids_in_original_order': [c['case_id'] for c in doc['cases']], 'complete_cases_sha256': hashlib.sha256(json.dumps(doc['cases'], ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()).hexdigest(), 'loaded_sdk_images': doc['loaded_sdk_images']}
    binding['preserved_case_fields'] = sorted(doc['cases'][0])
    for case in doc['cases']:
        path = case['input']['registered_font_path']
        if path.startswith('/'):
            expected = case['input']['registered_font_sha256']
            assert sha(path) == expected
            binding['font_files'][path] = expected
    for image in doc['loaded_sdk_images']:
        assert sha(image['path']) == image['sha256']
for image in json.loads((OUT / 'baseline-sdk8b-64.json').read_text())['loaded_sdk_images']:
    path = Path(image['path'].replace(str(ROOT / 'build-c3-qt-observed/tranche8b-private'), str(SDK)))
    binding['candidate_images'][str(path)] = sha(path)
binding['candidate_manifest'] = {'path': str(SDK / 'qt-observer-manifest.json'), 'sha256': sha(SDK / 'qt-observer-manifest.json')}
binding['candidate_image_binding'] = {'path': str(SDK / 'qt-observer-image-binding.json'), 'sha256': sha(SDK / 'qt-observer-image-binding.json')}
save('preparation-binding.json', binding)
plan = {'argv': [str(binary), '--output', str(OUT / 'actual-native64.json'), '--expected-engine', 'Freetype'], 'cwd': str(OUT), 'environment': {'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'QT_QPA_PLATFORM': 'cocoa:fontengine=freetype', 'QT_PLUGIN_PATH': str(SDK / 'qt-prefix/plugins'), 'DYLD_FRAMEWORK_PATH': str(SDK / 'qt-prefix/lib'), 'DYLD_LIBRARY_PATH': str(SDK / 'qt-prefix/lib'), 'QCAE_FT_AAT_TRACE': '1'}, 'binary_sha256': sha(binary), 'qt_conf_sha256': sha(OUT / 'qt.conf'), 'execution_authority': 'Root only after EXT-03 releases the native GUI slot; preparer has not executed this argv.', 'runtime_executed': False}
save('root-run-plan.json', plan)
print(json.dumps({'prepared_directory': str(OUT), 'compiler_exit_code': 0, 'binary_sha256': sha(binary), 'source_sha256': sha(source), 'native_runtime_executed': False, 'review_total_compiles': 2}, indent=2))
