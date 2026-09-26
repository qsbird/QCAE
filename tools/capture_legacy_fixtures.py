#!/usr/bin/env python3
"""Freeze real pre-refactor IPC outputs; verify golden copies without migrating them.

Only the explicitly authorized tests/fixtures/legacy golden data is written.
Regeneration intentionally refuses to overwrite an existing frozen manifest.
"""

from __future__ import annotations

import argparse
from collections import Counter
from contextlib import closing
import hashlib
import json
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import ipc_tests as ipc

BASELINE = '62e2b1d'
BINARY_HASHES = {
    'engine': 'b4f2cca68f2c83abdfd6d404f70f0d638613dc9890a69cd8a00623d5945b9799',
    'cli': '78f32e39651588b9fd77aefb85e56dfabb82d2c25222c3531a9e8217fc583825',
}
KINDS = ['node', 'beam', 'material', 'section', 'part', 'assembly', 'set',
         'include', 'force', 'constraint', 'analysis']
CASES = [
    ('saved-empty', 'project', False, False),
    ('saved-beam', 'project', True, False),
    ('saved-organization', 'project', True, True),
    ('unsaved-workspace', 'workspace', True, False),
    ('redo-workspace', 'workspace', True, False),
    ('all-supported-entities', 'project', True, True),
]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n')


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def database_metadata(path, kind):
    """Read format markers; this is metadata inspection, never a new encoder."""
    with closing(sqlite3.connect(f'{path.as_uri()}?mode=ro&immutable=1', uri=True)) as db:
        check(db.execute('PRAGMA integrity_check').fetchall() == [('ok',)],
              f'{path}: failed integrity_check')
        row = db.execute(f'SELECT payload FROM {kind} WHERE id=1').fetchone()
        check(row and row[0], f'{path}: missing state payload')
        payload = row[0]
        size = struct.unpack_from('<Q', payload)[0]
        marker = payload[8:8 + size].decode('ascii')
        version = struct.unpack_from('<Q', payload, 8 + size)[0]
        return {
            'sqlite_application_id': db.execute('PRAGMA application_id').fetchone()[0],
            'sqlite_user_version': db.execute('PRAGMA user_version').fetchone()[0],
            'payload_marker': marker,
            'payload_version': version,
            'model_record_schema': 1,
            'payload_bytes': len(payload),
            'payload_sha256': hashlib.sha256(payload).hexdigest(),
            'integrity_check': 'ok',
        }


def capture_closed_database(source, target, kind):
    """Checkpoint after writer exit, validate an API backup, preserve exact bytes."""
    with closing(sqlite3.connect(source)) as db:
        checkpoint = db.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone()
        check(checkpoint[0] == 0, f'{source}: busy WAL checkpoint')
        backup_path = source.with_suffix('.consistent-backup.sqlite')
        with closing(sqlite3.connect(backup_path)) as backup:
            db.backup(backup)
        check(database_metadata(source, kind) == database_metadata(backup_path, kind),
              f'{source}: SQLite backup differs from original payload/schema')
    # The owned engine has exited and the WAL was checkpointed, so this preserves
    # the actual old engine output, including its original SQLite header bytes.
    shutil.copyfile(source, target)
    check(sha256(source) == sha256(target), 'original database byte preservation failed')
    metadata = database_metadata(target, kind)
    metadata['capture'] = 'writer-exited; WAL checkpoint; SQLite API backup validated; exact-byte copy'
    metadata['checkpoint_result'] = list(checkpoint)
    return metadata


class Session:
    def __init__(self, engine, cli, folder, workspace=None):
        self.engine, self.cli = engine, cli
        self.folder = folder
        self.endpoint = str(folder / 'engine.sock')
        self.workspace = workspace or folder / 'working.sqlite'
        self.transcript = []
        self.log = (folder / 'engine.log').open('w+')
        self.process = subprocess.Popen(
            [str(engine), '--socket', self.endpoint, '--workspace', str(self.workspace)],
            stdout=self.log, stderr=self.log)
        ipc.CLI = str(cli)
        try:
            self.capabilities = ipc.data_of(ipc.wait_for_cli(self.endpoint, self.process),
                                           'capabilities.list')
        except Exception as error:
            self.stop()
            raise RuntimeError((folder / 'engine.log').read_text()) from error
        check(self.capabilities['durable'], 'legacy engine must have SQLite enabled')

    def call(self, operation, parameters=None, context=None, key=None):
        arguments = {}
        if context:
            arguments.update(document_id=context['document_id'],
                             document_epoch=context['document_epoch'],
                             expected_revision=context['revision'])
        response = ipc.call(self.endpoint, operation, parameters or {},
                            idempotency_key=key, **arguments)
        self.transcript.append({'operation': operation, 'parameters': parameters or {},
                                'context': arguments, 'idempotency_key': key,
                                'response': response})
        return ipc.data_of(response, operation)

    def current(self):
        return self.call('project.current')

    def commit(self, document, command, parameters, key):
        preview = self.call('changes.preview', {'command': command, **parameters}, document)
        self.call('changes.commit', {'preview_id': preview['preview_id']}, document, key)
        return self.current(), preview.get('affected_entity_id')

    def snapshot(self, document):
        entities = self.call('entity.query', {'limit': 1000}, document)['entities']
        references = []
        for entity in entities:
            references.extend(self.call('entity.references',
                              {'entity_id': entity['entity_id'], 'direction': 'outgoing'},
                              document)['references'])
        exports = []
        profile = self.capabilities['declared_solver_profiles'][0]['profile_ref']
        for entity in entities:
            if entity['kind'] == 'analysis':
                exported = self.call('model.export_preview',
                                    {'analysis_id': entity['entity_id'],
                                     'expected_profile_ref': profile}, document)
                for field in ('document_id', 'document_epoch', 'content_state'):
                    exported.pop(field, None)
                exports.append(exported)
        return {'document': document,
                'summary': self.call('model.summary', {}, document),
                'history': self.call('history.list', {}, document),
                'entities': entities,
                'references': references,
                'exports': exports,
                'counts': dict(sorted(Counter(e['kind'] for e in entities).items())),
                'source_identifier_count': sum(len(e['sources']) for e in entities),
                'profile_ref': profile}

    def stop(self, document=None):
        if self.process.poll() is None:
            if document:
                self.call('project.close', {'policy': 'keep_recovery'}, document, 'capture-close')
            self.process.terminate()
            self.process.wait(timeout=10)
        self.log.close()


def model_semantics(snapshot):
    semantic = {key: snapshot[key] for key in ('entities', 'references', 'counts',
                                             'source_identifier_count', 'profile_ref')}
    # Normal-open resets document revision to zero; all export resources, target,
    # source identities and engineering fields are still compared exactly.
    semantic['exports'] = [{key: value for key, value in exported.items() if key != 'revision'}
                           for exported in snapshot['exports']]
    return semantic


def verify(output, engine, cli, manifest):
    check(len(manifest['fixtures']) >= 6, 'legacy fixture denominator must be at least six')
    check({case[0] for case in CASES} <=
          {fixture['fixture_id'] for fixture in manifest['fixtures']}, 'minimum fixture coverage')
    check({fixture['kind'] for fixture in manifest['fixtures']} == {'project', 'workspace'},
          'both legacy fixture kinds must have nonempty denominators')
    results = []
    for fixture in manifest['fixtures']:
        path = output / fixture['path']
        before = sha256(path)
        check(before == fixture['sha256'], f'{path}: golden SHA-256 mismatch')
        check(path.stat().st_size == fixture['bytes'], f'{path}: byte count mismatch')
        check(database_metadata(path, fixture['kind']) ==
              {k: v for k, v in fixture['schema'].items()
               if k not in ('capture', 'checkpoint_result')}, f'{path}: schema mismatch')
        expected = json.loads((output / fixture['expectation_path']).read_text())
        check(expected['snapshot']['counts'] == fixture['counts'], 'manifest count mismatch')
        with tempfile.TemporaryDirectory(prefix='qcae-legacy-verify-') as folder:
            root = Path(folder)
            copied = root / path.name
            shutil.copyfile(path, copied)
            # The legacy adapter validates through READONLY before opening its
            # writer. Prime missing WAL sidecars on the disposable copy only.
            priming = None
            if fixture['kind'] == 'workspace':
                priming = sqlite3.connect(copied)
                priming.execute('SELECT payload FROM workspace WHERE id=1').fetchall()
            try:
                session = Session(engine, cli, root,
                                  copied if fixture['kind'] == 'workspace' else None)
            finally:
                if priming:
                    priming.close()
            try:
                parameters = {'mode': 'recover'} if fixture['kind'] == 'workspace' else {
                    'mode': 'normal', 'path': str(copied)}
                document = session.call('project.open', parameters, key='verify-open')
                actual = session.snapshot(document)
                actual_semantics = model_semantics(actual)
                expected_semantics = model_semantics(expected['snapshot'])
                differences = [key for key in actual_semantics
                               if actual_semantics[key] != expected_semantics[key]]
                check(not differences,
                      f'{path}: legacy round-trip model semantics differ: {differences}')
                if fixture['kind'] == 'workspace':
                    check(actual['history'] == expected['snapshot']['history'],
                          f'{path}: recovered history differs')
                    for field in ('document_id', 'revision', 'dirty', 'saved_path'):
                        check(document[field] == expected['snapshot']['document'][field],
                              f'{path}: recovered {field} differs')
                    if 'after_redo' in expected:
                        session.call('history.redo', {}, document, 'verify-redo')
                        document = session.current()
                        check(model_semantics(session.snapshot(document)) ==
                              model_semantics(expected['after_redo']),
                              f'{path}: redo model differs')
                else:
                    check(document['revision'] == '0' and actual['history']['cursor'] == 0
                          and actual['history']['items'] == [],
                          f'{path}: normal-open must start with empty history')
            finally:
                session.stop()
        check(sha256(path) == before, f'{path}: golden was modified during verification')
        results.append({'fixture_id': fixture['fixture_id'], 'sha256': before,
                        'integrity_check': 'ok', 'legacy_round_trip': 'passed',
                        'golden_bytes_unchanged': True,
                        'redo_replay': 'passed' if 'after_redo' in expected else 'not applicable'})
        print(f"verified {fixture['path']}: {fixture['bytes']} bytes, "
              f"{sum(expected['snapshot']['counts'].values())} entities")
    return {'validation_scope': 'pinned old binary; not a migration test',
            'passed_fixture_count': len(results), 'fixtures': results}


def capture(output, engine, cli):
    check(not (output / 'manifest.json').exists(), 'frozen manifest exists; use --verify-only')
    if not (output / 'inputs').exists():
        shutil.copytree(ROOT / 'tests/fixtures/legacy/inputs', output / 'inputs')
    resources = [{'path': str(path.relative_to(output / 'inputs')), 'text': path.read_text()}
                 for path in sorted((output / 'inputs').rglob('*.bdf'))]
    check(len(resources) == 4, 'expected four frozen source resources')
    fixtures = []
    for name, kind, beam, organization in CASES:
        with tempfile.TemporaryDirectory(prefix='qcae-legacy-capture-') as folder:
            root = Path(folder)
            session = Session(engine, cli, root)
            document = None
            try:
                document = session.call('project.create', {'name': f'R0 legacy {name}'}, key='create')
                profile = session.capabilities['declared_solver_profiles'][0]['profile_ref']
                if beam:
                    document, _ = session.commit(document, 'model.import', {
                        'root_resource': 'cantilever.bdf', 'resources': resources,
                        'source_profile_ref': profile, 'unit_system': 'mm-N-MPa'}, 'import')
                if organization:
                    entities = session.call('entity.query', {}, document)['entities']
                    members = [e['entity_id'] for e in entities if e['kind'] in ('node', 'beam')]
                    document, part_id = session.commit(document, 'part.upsert',
                        {'name': 'Cantilever', 'members': members}, 'part')
                    document, _ = session.commit(document, 'assembly.upsert',
                        {'name': 'Assembly', 'children': [part_id]}, 'assembly')
                    document, _ = session.commit(document, 'set.upsert',
                        {'name': 'Tip nodes', 'members': [members[1]]}, 'set')
                redo_snapshot = None
                if kind == 'workspace':
                    material = session.call('entity.query', {'kind': 'material'}, document)['entities'][0]
                    document, _ = session.commit(document, 'material.set_young_modulus',
                        {'entity_id': material['entity_id'],
                         'young_modulus': {'value': 200, 'unit': 'GPa'}}, 'unsaved-edit')
                    check(document['saved_path'] == '', 'recovery fixture has saved-path dependency')
                    if name == 'redo-workspace':
                        redo_snapshot = session.snapshot(document)
                        session.call('history.undo', {}, document, 'undo-edit')
                        document = session.current()
                source = root / f'{name}.qcae' if kind == 'project' else session.workspace
                if kind == 'project':
                    document = session.call('project.save', {'path': str(source)}, document, 'save')
                snapshot = session.snapshot(document)
                check(snapshot['counts'].get('node', 0) == (2 if beam else 0), 'node count')
                if beam:
                    for entity_kind in ('beam', 'material', 'section', 'force', 'constraint', 'analysis'):
                        check(snapshot['counts'][entity_kind] == 1, f'{entity_kind} count')
                    check(snapshot['counts']['include'] == 4, 'INCLUDE graph count')
                    check(snapshot['source_identifier_count'] == 7,
                          f"source identifier count: {snapshot['source_identifier_count']}")
                if organization:
                    check(set(snapshot['counts']) == set(KINDS), 'all entity kinds must be present')
                    for entity_kind in ('part', 'assembly', 'set'):
                        check(snapshot['counts'][entity_kind] == 1, f'{entity_kind} count')
                if kind == 'workspace':
                    modulus = 210000 if name == 'redo-workspace' else 200000
                    check(snapshot['summary']['materials'][0]['young_modulus_mpa'] == modulus,
                          'workspace material value')
                if name == 'redo-workspace':
                    check(snapshot['history']['cursor'] == 1 and len(snapshot['history']['items']) == 2,
                          'redo branch must have exactly one unapplied item')
                    check(not snapshot['history']['items'][1]['applied'], 'redo branch lost')
                expectation = {'snapshot': snapshot}
                if redo_snapshot:
                    expectation['after_redo'] = redo_snapshot
                session.stop(document)
                target = output / source.name
                if kind == 'workspace':
                    target = output / f'{name}.sqlite'
                schema = capture_closed_database(source, target, kind)
                expected_path = output / f'{name}.expected.json'
                transcript_path = output / f'{name}.ipc.json'
                write_json(expected_path, expectation)
                write_json(transcript_path, session.transcript)
                fixtures.append({'fixture_id': name, 'kind': kind, 'path': target.name,
                                 'bytes': target.stat().st_size, 'sha256': sha256(target),
                                 'schema': schema, 'profile_ref': profile,
                                 'counts': snapshot['counts'],
                                 'expectation_path': expected_path.name,
                                 'expectation_sha256': sha256(expected_path),
                                 'transcript_path': transcript_path.name,
                                 'transcript_sha256': sha256(transcript_path)})
            finally:
                session.stop()
    return {'manifest_version': 1, 'baseline_commit': BASELINE,
            'same_product_source_commit': 'afb9829',
            'producer': {'engine_sha256': sha256(engine), 'cli_sha256': sha256(cli),
                         'binary_origin': 'existing build-desktop binaries; no rebuild',
                         'api_version': '1.1', 'sqlite_python_version': sqlite3.sqlite_version},
            'capture_command': 'python3 tools/capture_legacy_fixtures.py --engine <pinned-legacy-engine> --cli <pinned-legacy-cli>',
            'relocation_policy': 'project snapshots open at any new path; workspace fixtures never saved, saved_path empty; old-engine READONLY startup requires WAL sidecars primed by SQLite on a disposable copy; do not rewrite golden bytes',
            'golden_database_exception': 'Explicitly authorized immutable legacy test fixtures; ordinary runtime databases remain excluded from commits.',
            'declared_legacy_schemas': {'sqlite_envelope': [1], 'workspace': [2],
                                       'project': [1], 'model_records': [1]},
            'supported_entity_kinds': KINDS,
            'inputs': [{'path': str(path.relative_to(output)), 'sha256': sha256(path)}
                       for path in sorted((output / 'inputs').rglob('*.bdf'))],
            'fixtures': fixtures,
            'validation_scope': 'old-engine normal-open/recover/redo on copies and SQLite integrity; migration not tested'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'tests/fixtures/legacy')
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--verification-report', type=Path,
                        help='write verification evidence, independently of golden database bytes')
    args = parser.parse_args()
    for kind in ('engine', 'cli'):
        path = getattr(args, kind).resolve()
        check(sha256(path) == BINARY_HASHES[kind], f'{kind}: not the frozen legacy binary')
        setattr(args, kind, path)
    args.output = args.output.resolve()
    # Pin both executables before any other agents can rebuild their original paths.
    with tempfile.TemporaryDirectory(prefix='qcae-legacy-binaries-') as folder:
        pinned = Path(folder)
        engine, cli = pinned / 'qcae-engine', pinned / 'qcae-cli'
        shutil.copy2(args.engine, engine)
        shutil.copy2(args.cli, cli)
        if args.verify_only:
            manifest = json.loads((args.output / 'manifest.json').read_text())
        else:
            manifest = capture(args.output, engine, cli)
            write_json(args.output / 'manifest.json', manifest)
        for fixture in manifest['fixtures']:
            for path_key, hash_key in (('expectation_path', 'expectation_sha256'),
                                       ('transcript_path', 'transcript_sha256')):
                check(sha256(args.output / fixture[path_key]) == fixture[hash_key], 'companion hash mismatch')
        for item in manifest['inputs']:
            check(sha256(args.output / item['path']) == item['sha256'], 'input hash mismatch')
        result = verify(args.output, engine, cli, manifest)
        if args.verification_report:
            write_json(args.verification_report.resolve(), result)


if __name__ == '__main__':
    main()
