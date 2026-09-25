#!/usr/bin/env python3
"""Real-process SQLite recovery and shared query/view contract integration."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import ipc_tests as ipc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', required=True)
    parser.add_argument('--cli', required=True)
    args = parser.parse_args()
    ipc.ENGINE, ipc.CLI = args.engine, args.cli
    with tempfile.TemporaryDirectory(prefix='qcae-m23-') as folder:
        root = Path(folder)
        endpoint = str(root / 'engine.sock')
        workspace = str(root / 'working.sqlite')
        process = None
        with (root / 'engine.log').open('w+') as log:
            def start():
                nonlocal process
                process = subprocess.Popen([args.engine, '--socket', endpoint, '--workspace', workspace], stdout=log, stderr=log)
                return ipc.data_of(ipc.wait_for_cli(endpoint, process), 'capabilities.list')

            def call(op, params=None, context=None, *, key=None, expected='success', revision=True):
                extra = {}
                if context:
                    extra.update(document_id=context['document_id'], document_epoch=context['document_epoch'])
                    if revision:
                        extra['expected_revision'] = context['revision']
                result = ipc.call(endpoint, op, params or {}, idempotency_key=key, expected_status=expected, **extra)
                return result if expected != 'success' else ipc.data_of(result, op)

            def current():
                return call('project.current')

            def commit(context, command, params, key):
                preview = call('changes.preview', {'command': command, **params}, context)
                result = call('changes.commit', {'preview_id': preview['preview_id']}, context, key=key)
                return current(), result

            try:
                cap = start()
                assert cap['durable'] and cap['storage_mode'] == 'sqlite'
                doc = call('project.create', {'name': 'Persistent beam'}, key='create')
                profile = cap['declared_solver_profiles'][0]['profile_ref']
                fixtures = Path(__file__).parent / 'fixtures' / 'nastran'
                resources = [{'path': str(path.relative_to(fixtures)), 'text': path.read_text()} for path in sorted(fixtures.rglob('*.bdf'))]
                doc, imported = commit(doc, 'model.import', {'root_resource': 'cantilever.bdf', 'resources': resources, 'source_profile_ref': profile, 'unit_system': 'mm-N-MPa'}, 'import')
                original_id = doc['document_id']
                nodes = call('entity.query', {'kind': 'node'}, doc, revision=False)['entities']
                material = call('entity.query', {'kind': 'material'}, doc, revision=False)['entities'][0]['entity_id']
                node_ids = [n['entity_id'] for n in nodes]
                view = call('view.create', {'hidden_ids': [], 'camera_fingerprint': 'front'}, doc)
                render = call('view.render_data', {'view_session_id': view['view_session_id'], 'expected_view_revision': view['view_revision']}, doc)
                assert len(render['points']) == 2 and len(render['beams']) == 1
                assert {p['entity_id'] for p in render['points']} == set(node_ids)
                def select(predicate, scope=None, view_context=None):
                    use = view_context or view
                    return call('selection.evaluate', {'view_session_id': use['view_session_id'], 'expected_view_revision': use['view_revision'], 'predicate': predicate, 'scope': scope or {'visibility': 'through', 'include_hidden': True}}, doc)
                chosen = select({'op': 'and', 'children': [{'op': 'kind', 'value': 'node'}, {'op': 'not', 'children': [{'op': 'ids', 'ids': [node_ids[0]]}]}]})
                page = call('selection.get', {'selection_handle': chosen['selection_handle']}, doc, revision=False)
                assert page['entity_ids'] == [node_ids[1]]
                view2 = call('view.update', {'view_session_id': view['view_session_id'], 'expected_view_revision': view['view_revision'], 'hidden_ids': [node_ids[0]], 'camera_fingerprint': 'front'}, doc)
                call('selection.get', {'selection_handle': chosen['selection_handle']}, doc, expected='conflict', revision=False)
                hidden = select({'op': 'all'}, {'candidate_ids': node_ids, 'include_hidden': False, 'visibility': 'through', 'invert': False}, view2)
                assert hidden['count'] == 1
                inverse = select({'op': 'ids', 'ids': [node_ids[0]]}, {'candidate_ids': node_ids, 'include_hidden': True, 'visibility': 'through', 'invert': True}, view2)
                assert inverse['count'] == 1
                invisible = call('selection.evaluate', {'view_session_id': view2['view_session_id'], 'expected_view_revision': view2['view_revision'], 'predicate': {'op': 'all'}, 'scope': {'visibility': 'visible_only'}}, doc, expected='failed')
                assert invisible['error']['code'] == 'UNSUPPORTED_CAPABILITY'
                saved = call('project.save', {'path': str(root / 'beam.qcae')}, doc, key='save')
                assert saved['revision'] == doc['revision'] and not saved['dirty']
                doc, changed = commit(saved, 'material.set_young_modulus', {'entity_id': material, 'young_modulus': {'value': 200, 'unit': 'GPa'}}, 'material-change')
                old = dict(doc)
                process.kill()
                process.wait(timeout=10)
                cap = start()
                assert cap['recovery_available']
                call('project.current', expected='failed')
                doc = call('project.open', {'mode': 'recover'}, key='recover')
                assert doc['document_id'] == original_id and doc['document_epoch'] != old['document_epoch']
                assert doc['revision'] == old['revision'] and doc['dirty']
                fact = call('operations.get', {'lookup_scope': 'document', 'original_operation': 'changes.commit', 'idempotency_key': 'material-change'}, doc, revision=False)
                assert fact['transaction_id'] == changed['transaction_id']
                call('history.undo', {}, old, key='old-epoch', expected='conflict')
                call('history.undo', {}, doc, key='undo')
                doc = current()
                assert not doc['dirty']
                call('history.redo', {}, doc, key='redo')
                doc = current()
                assert doc['dirty']
                saved_as = call('project.save_as', {'path': str(root / 'copy.qcae')}, doc, key='save-as')
                assert saved_as['project_id'] != saved['project_id'] and saved_as['document_id'] == original_id
                assert saved_as['revision'] == doc['revision'] and not saved_as['dirty']
                call('project.close', {'policy': 'discard'}, saved_as, key='discard')
                fact = call('operations.get', {'lookup_scope': 'host', 'original_operation': 'project.close', 'idempotency_key': 'discard'})
                assert fact['document_id'] == original_id
                doc = call('project.open', {'mode': 'normal', 'path': str(root / 'beam.qcae')}, key='open')
                assert doc['document_id'] != original_id and doc['revision'] == '0'
                summary = call('model.summary', {}, doc, revision=False)
                assert summary['node_count'] == 2 and summary['materials'][0]['young_modulus_mpa'] == 210000
                replay = call('project.create', {'name': 'Persistent beam'}, key='create')
                assert replay['document_id'] == original_id and current()['document_id'] == doc['document_id']
                print('M2 durable recovery/save and M3 view/selection IPC passed')
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == '__main__':
    main()
