#!/usr/bin/env python3
"""Extract adapter parity from actual frozen CTest evidence, never execute it."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path('/Users/qs/Documents/ChatGPT/QCAE')
OUT = Path('/private/tmp/qcae-ext04-common41')
COMMIT = 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() == COMMIT

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def item(path):
    return {'path': str(path), 'sha256': sha(path), 'byte_length': path.stat().st_size}

commands = json.loads((ROOT / 'docs/engineering/c3-closure-evidence/package/common41-final/actual-commands.json').read_text())
selected = [row for row in commands if row['name'].startswith('sqlite_headless-')]
assert len(selected) == 3 and all(row['exit_codes'] == [0] and row['source_commit'] == COMMIT for row in selected)
for row in selected:
    for log in row['logs']:
        assert sha(ROOT / log['path']) == log['sha256']
build = ROOT / 'build-c3-vtk-sqlite-release'
raw = build / 'Testing/Temporary/LastTest.log'
archived = OUT / 'sqlite-headless-LastTest.original.log'
assert not archived.exists()
shutil.copyfile(raw, archived)
text = archived.read_text()
inventory = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1'], cwd=ROOT))
compile_commands = json.loads((build / 'compile_commands.json').read_text())
result = []
for backend, name, executable in [('memory', 'geometry_mesh', 'qcae_geometry_mesh_tests'),
                                   ('sqlite', 'geometry_mesh_sqlite', 'qcae_geometry_mesh_sqlite_tests')]:
    pattern = rf'^\d+/\d+ Testing: {name}\n.*?^"{name}" time elapsed: .*?$'
    found = re.findall(pattern, text, re.M | re.S)
    assert len(found) == 1
    block = found[0]
    assert 'Test Passed.' in block and '<end of output>' in block
    assert f'Command: "{build / executable}"' in block
    rows = [json.loads(line) for line in block.splitlines() if line.startswith('{')]
    assert len(rows) == 30 and len({row['run_id'] for row in rows}) == 30
    for mode, (case, state, commits) in enumerate([('success', 'succeeded', 1),
                                                ('cancel_before_commit', 'cancelled', 0),
                                                ('stale_candidate', 'conflicted', 0)]):
        for repetition in range(10):
            assert rows[mode * 10 + repetition] == {
                'case': case, 'run_id': f'{backend}-mesh-{mode}-{repetition}',
                'backend': backend, 'final_state': state, 'task_model_commits': commits}
    for statement in ['PASS: task replay after undo removes geometry and section; new missing-source work fails',
                      'PASS: 100 undo/redo rounds; semantic differences=0; revision_delta=201',
                      'PASS: 10 success + 10 cancellation + 10 stale real mesh tasks']:
        assert statement in block
    delivered = OUT / f'{backend}-contract.original.log'
    delivered.write_text(block + '\n')
    test = [row for row in inventory['tests'] if row['name'] == name]
    assert len(test) == 1 and test[0]['command'] == [str(build / executable)]
    compilation = [row for row in compile_commands if f'CMakeFiles/{executable}.dir/' in row.get('output', row['command'])]
    assert len(compilation) == 1
    assert Path(compilation[0]['file']).resolve() == ROOT / 'tests/geometry_mesh_tests.cpp'
    assert ('-DQCAE_TEST_SQLITE=1' in compilation[0]['command']) == (backend == 'sqlite')
    result.append({'backend': backend, 'test': name, 'actual_child_command': test[0]['command'],
                   'actual_compile_command': compilation[0], 'binary': item(build / executable),
                   'extracted_original_output': item(delivered), 'observed_task_rows': rows,
                   'history_rounds': 100, 'history_revision_delta': 201})
normalized = [[{k: v for k, v in row.items() if k not in ('backend', 'run_id')}
               for row in record['observed_task_rows']] for record in result]
assert normalized[0] == normalized[1]
sources = [ROOT / name for name in ['tests/geometry_mesh_tests.cpp', 'tests/runtime_test_support.hpp',
    'modules/application/src/record_application.cpp', 'modules/contracts/include/qcae/record_store.hpp',
    'adapters/storage_sqlite/src/sqlite_store.cpp', 'CMakeLists.txt']]
for path in sources:
    assert path.read_bytes() == subprocess.check_output(['git', 'show', f'{COMMIT}:{path.relative_to(ROOT)}'], cwd=ROOT)
report = {'schema': 'qcae.ext04.same-core-adapter-contract/1', 'baseline_commit': COMMIT,
          'source_tree_sha256': selected[0]['source_tree_sha256'], 'same_test_translation_unit': True,
          'same_authority': 'RecordApplication + EditSession + record_task_publisher',
          'adapter_switch': 'FixtureStore IRecordStore construction selected by QCAE_TEST_SQLITE=1',
          'product_core_modified_for_this_collection': False,
          'observed_task_contract_parity': True, 'internal_full_record_goldens_checked_by_both_binaries': True,
          'source_files': [item(path) for path in sources], 'actual_parent_execution_records': selected,
          'original_full_ctest_log': item(archived), 'contracts': result,
          'scope_limit': 'Memory store retains heap state while reconstructing the application; only SQLite closes and reopens a persistent store. This is not cross-process memory durability, full M physics coverage, complete SK or P0 acceptance.',
          'extension_commit': None, 'final_integrated_release_retest_pending': True}
output = OUT / 'report.json'
assert not output.exists()
output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'adapter_contract_parity': True, 'actual_task_rows': 60, 'history_rounds_per_backend': 100,
                  'baseline_commit': COMMIT, 'report': str(output)}))
