#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys
import time

root = Path(__file__).parent
label, *command = sys.argv[1:]
start = time.time()
with (root / (label + '.log')).open('wb') as log:
    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
output = (root / (label + '.log')).read_bytes()
delivered = output[-10000:].decode('utf-8', errors='replace')
record = {'label': label, 'command': command, 'cwd': str(Path.cwd()),
          'exit_code': result.returncode, 'elapsed_seconds': time.time()-start,
          'raw_log_utf8_bytes': len(output), 'raw_log': str(root / (label + '.log')),
          'diagnostic_delivery_utf8_bytes': len(delivered.encode('utf-8')),
          'diagnostic_range': [max(0, len(output)-10000), len(output)], 'tokens': None}
with (root / 'commands.jsonl').open('a') as log:
    log.write(json.dumps(record) + '\n')
print(json.dumps(record), flush=True)
print(delivered, end='', flush=True)
sys.exit(result.returncode)
