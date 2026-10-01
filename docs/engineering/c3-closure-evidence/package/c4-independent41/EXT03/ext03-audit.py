import collections
import hashlib
import json
from pathlib import Path
import re
import subprocess

root = Path('/private/tmp/qcae-c4-ext03-41')
evidence = Path('/private/tmp/qcae-ext03-41-evidence')
baseline = 'b740b374c934f3b4624a21247b9a5deeb2ca6bf4'
manifest = json.loads((root / 'docs/engineering/c3-closure-evidence/package/c4-common40-baseline-manifest.json').read_text())

def digest(data):
    return hashlib.sha256(data).hexdigest()

def save(name, value):
    (evidence / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')

protected = []
for kind in ('protected_files', 'generators', 'immutable_fixtures'):
    for row in manifest[kind]:
        data = (root / row['path']).read_bytes()
        protected.append({'kind': kind, 'path': row['path'], 'baseline_sha256': row['sha256'],
                          'current_sha256': digest(data), 'equal': digest(data) == row['sha256']})
assert all(row['equal'] for row in protected)
save('frozen-boundaries.json', {'baseline': baseline, 'files': protected,
                               'protected_touches': 0, 'generator_touches': 0})

commands = [json.loads(line) for line in (evidence / 'context-commands.jsonl').read_text().splitlines()]
files = [json.loads(line) for line in (evidence / 'context-files.jsonl').read_text().splitlines()]
# Early batches used an additional functions.exec output cap; their exact outer range was
# not persisted. These values remain tool-stage upper bounds, never model-context totals.
early_names = {'locate', 'manifest-summary', 'architecture', 'rules-read', 'projector-read',
               'build-env', 'task-context', 'lookup', 'entry-locations', 'operations-test-read',
               'operation-tests', 'view-tests', 'nastran-locate', 'checks-read', 'read-export',
               'generate', 'test-support', 'core-config', 'test-support2', 'ipc-find',
               'ipc-support', 'options-save'}
framed_reads = []
for command in commands:
    raw = (evidence / (command['name'] + '.log')).read_bytes()
    marker = b'\n[bounded delivery; full raw log retained]\n'
    prefix_limit = command['utf8_bytes'] - (len(marker) if not command['complete'] else 0)
    for match in re.finditer(rb'^--- (.+?) \((\d+) UTF-8 bytes\) ---\n', raw, re.M):
        content_start = match.end()
        content_bytes = int(match.group(2))
        framed_reads.append({'command_name': command['name'], 'path': match.group(1).decode(),
                             'content_utf8_bytes': content_bytes,
                             'tool_stage_delivered_content_utf8_bytes': max(0, min(content_bytes, prefix_limit-content_start)),
                             'outer_delivery_known': command['name'] not in early_names,
                             'outer_actual_content_utf8_bytes': None if command['name'] in early_names else max(0, min(content_bytes, prefix_limit-content_start))})
messages = json.loads((evidence / 'resource-coordination.json').read_text())
for path in sorted(evidence.glob('coordination-*.txt')):
    text = path.read_text().rstrip('\n')
    if not any(message['text'] == text for message in messages['messages']):
        messages['messages'].append({'sender': '/root', 'text': text})
for message in messages['messages']:
    message['utf8_bytes'] = len(message['text'].encode())
    message['sha256'] = digest(message['text'].encode())
save('resource-coordination.json', messages)

save('context-accounting.json', {
    'schema': 'qcae.ext03.context-accounting.v1', 'executor': '/root/c4_ext03_frozen41',
    'tokens': None,
    'initial_eight_source_utf8_bytes': manifest['initial_context']['content_utf8_bytes'],
    'initial_eight_framed_utf8_bytes': manifest['initial_context']['delivered_utf8_bytes'],
    'initial_packet_attempts': [
        {'attempt': 1, 'phase': 'initial', 'file': 'initial-context.txt', 'tool_stage_bytes': 42601,
         'sha256': digest((evidence/'initial-context.txt').read_bytes()),
         'actual_outer_bytes': None, 'outer_truncated': True},
        {'attempt': 2, 'phase': 'initial', 'file': 'initial-context-complete.txt', 'actual_returned_utf8_bytes': 42601,
         'sha256': digest((evidence/'initial-context-complete.txt').read_bytes()),
         'outer_truncated': False}],
    'source_event_count': len(files),
    'source_reads_per_path': dict(collections.Counter(row['path'] for row in files)),
    'file_ledger_semantics': 'context-files.jsonl records source framing before runner/outer truncation. It must not be summed as actual model delivery. Framed-read overlay records runner-delivered ranges.',
    'command_event_count': len(commands),
    'command_read_count_per_name': dict(collections.Counter(row['name'] for row in commands)),
    'runner_returned_utf8_bytes_upper_bound': sum(row['utf8_bytes'] for row in commands),
    'raw_command_log_bytes_on_disk': sum(row['raw_log_bytes'] for row in commands),
    'runner_bounded_commands': [row['name'] for row in commands if not row['complete']],
    'early_outer_range_uncertain_commands': sorted(early_names),
    'byte_count_convention': 'Command values measure runner stdout at the nested tool stage, including its truncation marker. Tool envelopes/JSON escaping and runtime-injected system/developer context are not measured as stdout. Total model-context cost is unknown.',
    'framed_read_overlay': framed_reads,
    'extra_context_message_utf8_bytes': sum(row['utf8_bytes'] for row in messages['messages']),
    'unlogged_early_reads': [
        {'paths': ['docs/engineering/c4-executor-task.md', 'AGENTS.md', 'docs/baseline/README.md'],
         'reads': 1, 'actual_returned_utf8_bytes': None,
         'note': 'Initial package locator and baseline reads preceded the per-command runner; task package/AGENTS repeated reads are not omitted. Exact combined return was not saved.'},
        {'paths': ['docs/engineering/c3-closure-evidence/package/c4-common40-baseline-manifest.json', 'tools/c4_context_read.py'],
         'reads': 1, 'nested_tool_returned_utf8_bytes': 72110,
         'saved_nested_return': 'manifest-read-returned.txt', 'actual_outer_utf8_bytes': None,
         'note': 'Nested exec_command and then functions.exec both truncated; saved nested return contains its truncation notice. Full manifest disk size is not delivered context.'}],
    'actual_total_context_bytes': None,
    'measurement_limitation': 'Exact final model-context total cannot be certified because early outer truncation ranges and two early command returns were not persisted. Raw upper bounds, repeat attempts, file reads, later exact runner delivery and coordination messages are all disclosed; initial package size is never described as total cost.',
    'ledger_snapshot_sha256': {'context-files.jsonl': digest((evidence/'context-files.jsonl').read_bytes()), 'context-commands.jsonl': digest((evidence/'context-commands.jsonl').read_bytes())}})
print('PASS: protected/generator/immutable hashes unchanged; context ranges and unknown early delivery explicitly accounted')
