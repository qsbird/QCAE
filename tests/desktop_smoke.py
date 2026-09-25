#!/usr/bin/env python3
"""Launch the real desktop against an engine-owned imported beam, capture, then clean up."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import ipc_tests as ipc


def main():
    parser = argparse.ArgumentParser()
    for name in ('engine', 'cli', 'desktop', 'screenshot'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    ipc.ENGINE, ipc.CLI = args.engine, args.cli
    screenshot = Path(args.screenshot).resolve()
    screenshot.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='qcae-desktop-smoke-') as tmp:
        root = Path(tmp)
        endpoint = str(root / 'engine.sock')
        with (root / 'engine.log').open('w+') as log:
            engine = subprocess.Popen([args.engine, '--socket', endpoint, '--workspace', str(root / 'working.sqlite')], stdout=log, stderr=log)
            try:
                cap = ipc.data_of(ipc.wait_for_cli(endpoint, engine), 'capabilities')
                created = ipc.data_of(ipc.call(endpoint, 'project.create', {'name': 'Cantilever · M2/M3'}, idempotency_key='smoke-create'), 'create')
                fixture = Path(__file__).parent / 'fixtures' / 'nastran'
                resources = [{'path': str(path.relative_to(fixture)), 'text': path.read_text()} for path in sorted(fixture.rglob('*.bdf'))]
                context = dict(document_id=created['document_id'], document_epoch=created['document_epoch'], expected_revision=created['revision'])
                preview = ipc.data_of(ipc.call(endpoint, 'changes.preview', {'command': 'model.import', 'root_resource': 'cantilever.bdf', 'resources': resources, 'unit_system': 'mm-N-MPa', 'source_profile_ref': cap['declared_solver_profiles'][0]['profile_ref']}, **context), 'preview')
                ipc.call(endpoint, 'changes.commit', {'preview_id': preview['preview_id']}, idempotency_key='smoke-import', **context)
                proc = subprocess.run([args.desktop, '--smoke', '--socket', endpoint, '--screenshot', str(screenshot), '--quit-after-ms', '5000'], capture_output=True, text=True, timeout=25)
                if proc.returncode != 0:
                    raise RuntimeError(f'Desktop smoke failed ({proc.returncode}): {proc.stdout}\n{proc.stderr}')
                if not screenshot.exists() or screenshot.stat().st_size < 5000:
                    raise RuntimeError('Desktop did not produce a meaningful PNG artifact')
                status = ipc.data_of(ipc.call(endpoint, 'project.current'), 'current')
                assert status['document_id'] == created['document_id'] and status['revision'] == '1'
                print(f'Real engine/desktop shared-model smoke passed: {screenshot}')
            finally:
                if engine.poll() is None:
                    engine.terminate()
                    engine.wait(timeout=10)


if __name__ == '__main__':
    main()
