from pathlib import Path
import hashlib
import json
import sys
sys.path.insert(0, '/Users/qs/Documents/ChatGPT/QCAE/tests')
from c3_sync_ipc_tests import EngineProcess

root = Path('/private/tmp/qcae-core-mcp-readonly-review/actual-engine')
root.mkdir(exist_ok=False)
engine = Path('/Users/qs/Documents/ChatGPT/QCAE/build-c3-contracts-release39/qcae-engine')
before = hashlib.sha256(engine.read_bytes()).hexdigest()
with (root/'engine.log').open('w+') as log:
    host = EngineProcess(str(engine), str(root/'engine.sock'), str(root/'work.sqlite'), log)
    try:
        caps = host.start()
        catalog = {entry['name']: entry for entry in caps['operations']}
        actual = {name:catalog[name] for name in ('entity.query','entity.references','model.summary',
                                                'project.status','entity.fields','task.status','task.cancel','task.reconcile')}
        context = host.client.call('project.create', {'name':'read-only isolated contract probe'}, key='create')
        cases = []
        for operation, parameters, expected in (
            ('entity.query', {'limit':0,'offset':100000}, 'success'),
            ('entity.query', {'limit':1001}, 'failed'),
            ('entity.query', {'offset':100001}, 'failed'),
            ('entity.query', {'limit':1.5}, 'failed'),
            ('entity.query', {'limit':True}, 'failed'),
            ('entity.query', {'kind':''}, 'failed'),
            ('entity.query', {'ids':[42]}, 'failed'),
            ('entity.query', {'entity_type':'Node'}, 'failed'),
            ('entity.references', {'entity_id':'missing','direction':'sideways'}, 'failed'),
            ('model.summary', {'analysis_id':'unexpected'}, 'failed'),
            ('project.status', {'unexpected':False}, 'failed'),
            ('task.reconcile', {'unexpected':False}, 'failed'),
            ('task.reconcile', ['wrong-object-type'], 'failed'),
            ('entity.fields', {'entity_id':'missing','extra':1}, 'failed'),
            ('task.status', {'task_id':'missing','extra':1}, 'failed'),
            ('task.cancel', {'task_id':'missing','extra':1}, 'failed')):
            response = host.client.call(operation, parameters, context, expected=expected)
            cases.append({'operation':operation,'parameters':parameters,'expected':expected,'response':response})
        unchanged = host.client.current()
        assert unchanged['revision'] == context['revision']
        (root/'observations.json').write_text(json.dumps({'engine_sha256':before,'source_kind':'actual_local_engine',
          'isolated_workspace':True,'read_negative_cases':len(cases),'capabilities':actual,'cases':cases,
          'no_model_change':True,'no_real_solver':True},indent=2)+'\n')
        print(f'PASS {len(cases)} actual local dispatch boundary cases; no model revision change')
    finally:
        (root/'transcript.json').write_text(json.dumps(host.client.transcript,indent=2)+'\n')
        host.close()
assert hashlib.sha256(engine.read_bytes()).hexdigest() == before
