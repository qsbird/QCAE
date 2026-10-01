#!/usr/bin/env python3
"""Execute actual same-commit build gates; stop on any failure/source mutation."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--source-commit', required=True)
args = parser.parse_args()
root = Path('/Users/qs/Documents/ChatGPT/QCAE')
out = root / 'docs/engineering/c3-closure-evidence/package/common40-final'
out.mkdir(exist_ok=False)
python = '/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14'
spec = importlib.util.spec_from_file_location('c4_freeze', root / 'tools/freeze_c4_baseline.py')
freeze = importlib.util.module_from_spec(spec)
spec.loader.exec_module(freeze)
repo = freeze.Repository(root, args.source_commit)
source_sha = freeze.source_digest(freeze.committed_source_facts(repo))
records = []

def verify_source():
    current = freeze.Repository(root, args.source_commit)
    for item in freeze.committed_source_facts(current):
        path = root / item['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError('Committed source changed: ' + item['path'])

def run(name, command):
    verify_source()
    path = out / (name + '.log')
    started = time.monotonic()
    with path.open('xb') as stream:
        env = os.environ.copy()
        if name == 'desktop-tests':
            env['QCAE_AI_UNITS_SNAPSHOT'] = str(root / 'build-c3-ai-units-snapshots/session5.qcae')
            env['QCAE_SOURCE_TREE_SHA256'] = source_sha
        result = subprocess.run(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT, env=env)
    record = {'name': name, 'commands': [command], 'exit_codes': [result.returncode],
              'source_commit': args.source_commit, 'source_tree_sha256': source_sha,
              'elapsed_seconds': time.monotonic() - started,
              'task_environment': {key: env[key] for key in ('QCAE_AI_UNITS_SNAPSHOT', 'QCAE_SOURCE_TREE_SHA256') if key in env},
              'logs': [{'path': path.relative_to(root).as_posix(),
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}]}
    records.append(record)
    (out / 'actual-commands.json').write_text(json.dumps(records, indent=2) + '\n')
    print(json.dumps({'stage': name, 'exit_code': result.returncode,
                      'elapsed_seconds': record['elapsed_seconds']}), flush=True)
    if result.returncode:
        raise RuntimeError('Gate failed: ' + name)
    verify_source()

base = ['cmake', '-S', '.', '-G', 'Ninja', '-DPython3_EXECUTABLE=' + python]
sqlite = ['-DSQLite3_LIBRARY=/opt/homebrew/opt/sqlite/lib/libsqlite3.dylib',
          '-DSQLite3_INCLUDE_DIR=/opt/homebrew/opt/sqlite/include']
qtvtk = ['-DQt6_DIR=/opt/homebrew/opt/qt/lib/cmake/Qt6',
         '-DVTK_DIR=' + str(root / 'build-vtk-deps/install/lib/cmake/vtk-9.7')]
matrices = [
 ('core', 'build-c3-closure-core', ['-DCMAKE_BUILD_TYPE=Release', '-DQCAE_BUILD_IPC=OFF',
     '-DQCAE_BUILD_STORAGE=OFF', '-DQCAE_BUILD_DESKTOP=OFF', '-DQCAE_ENABLE_SANITIZERS=OFF']),
 ('sqlite_headless', 'build-c3-vtk-sqlite-release', ['-DCMAKE_BUILD_TYPE=Release',
     '-DQCAE_BUILD_IPC=OFF', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=OFF',
     '-DQCAE_ENABLE_SANITIZERS=OFF'] + sqlite),
 ('asan_ubsan', 'build-c4-baseline-sqlite-sanitizers40', ['-DCMAKE_BUILD_TYPE=Debug',
     '-DQCAE_BUILD_IPC=OFF', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=OFF',
     '-DQCAE_ENABLE_SANITIZERS=ON'] + sqlite),
 ('desktop', 'build-c3-closure-desktop', ['-DCMAKE_BUILD_TYPE=Release',
     '-DQCAE_BUILD_IPC=ON', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=ON',
     '-DQCAE_ENABLE_SANITIZERS=OFF'] + sqlite + qtvtk),
]
for name, build, flags in matrices:
    run(name + '-configure', base + ['-B', build] + flags)
    run(name + '-build', ['cmake', '--build', build, '-j', '2'])
    run(name + '-tests', ['ctest', '--test-dir', build, '--output-on-failure', '-j', '1' if name == 'desktop' else '2'])
run('design', [python, 'tools/check_design.py'])
run('cpp-format', [python, 'tools/check_cpp_format.py'])
run('diff-check', ['git', 'diff', '--check'])
run('architecture', [python, 'tools/check_architecture.py', '--build-dir',
 'build-c3-closure-desktop', '--report', str((out / 'architecture.json').relative_to(root))])
run('protected-ast', [python, 'tools/check_protected_entity_ast.py', '--build-dir',
 'build-c3-closure-desktop', '--libclang', '/opt/homebrew/opt/llvm/lib/libclang.dylib',
 '--bindings-dir', '/opt/homebrew/opt/llvm/lib/python3.12/site-packages',
 '--clang-argument=-resource-dir', '--clang-argument=/Library/Developer/CommandLineTools/usr/lib/clang/21',
 '--clang-argument=-isysroot', '--clang-argument=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk',
 '--clang-argument=-isystem', '--clang-argument=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1',
 '--report', str((out / 'protected-ast.json').relative_to(root)), '--self-test'])
run('public-consumers-configure', ['cmake', '-S', 'tests/public_api_consumers', '-B',
 'build-c3-public-consumers35', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
 '-DQCAE_BUILD_IPC=ON', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=ON',
 '-DPython3_EXECUTABLE=' + python] + sqlite + qtvtk)
run('public-consumers-build', ['cmake', '--build', 'build-c3-public-consumers35', '-j', '2'])
print(json.dumps({'all_executed_stages_passed': True, 'source_commit': args.source_commit,
                  'source_tree_sha256': source_sha}), flush=True)
