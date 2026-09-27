import json
from pathlib import Path
import subprocess
import sys
import time

root = Path(sys.argv[1]).resolve()
folder = Path(sys.argv[2]).resolve()
folder.mkdir()
endpoint = str(folder / 'engine.sock')
engine_log = (folder / 'engine.log').open('w')
process = subprocess.Popen([str(root / 'build-c2-desktop/qcae-engine'), '--socket', endpoint,
                           '--workspace', str(folder / 'work.sqlite')], stdout=engine_log, stderr=engine_log)
transcript = []

def call(operation, parameters=None, context=None, key=None):
    request = {'api_version': '1.1', 'request_id': f'probe-{len(transcript)}',
               'operation': operation, 'parameters': parameters or {}}
    if context:
        request.update(document_id=context['document_id'], document_epoch=context['document_epoch'],
                       expected_revision=context['revision'])
    if key:
        request['idempotency_key'] = key
    output = subprocess.run([str(root / 'build-c2-desktop/qcae-cli'), '--socket', endpoint, '--no-start'],
                            input=json.dumps(request), text=True, capture_output=True, timeout=10)
    if not output.stdout:
        raise RuntimeError(output.stderr)
    response = json.loads(output.stdout)
    transcript.append({'request': request, 'response': response})
    return response

try:
    deadline = time.monotonic() + 10
    while not Path(endpoint).exists():
        if process.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError('engine did not start')
        time.sleep(.05)
    doc = call('project.create', {'name': 'IPC audit'}, key='create')['data']
    line = call('geometry.create_line', {'start_mm': [0, 0, 0], 'end_mm': [1000, 0, 0]}, doc, 'line')
    assert line['status'] == 'success', line
    doc = call('project.current')['data']
    basic = call('entity.query', {'kind': 'geometry'}, doc)
    all_entities = call('entity.query', {}, doc)
    filtered = call('entity.query', {'kind': 'geometry', 'name_contains': ''}, doc)
    assert basic['status'] == 'success' and basic['data']['total'] == 1
    assert all_entities['status'] == 'success' and all_entities['data']['total'] == 0
    assert filtered['status'] != 'success'
    print('REPRODUCED: kind=geometry returns one line; no-kind enumeration returns zero; geometry+name_contains fails.')
    print('These are observed current limitations, not passing generic-query acceptance.')
finally:
    process.terminate()
    process.wait(timeout=10)
    engine_log.close()
    Path('/private/tmp/qcae-extension-audit/ipc-transcript.json').write_text(json.dumps(transcript, indent=2) + '\n')
