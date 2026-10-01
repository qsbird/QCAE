import hashlib
import json
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).parent
ROOT = Path('/private/tmp/qcae-c4-integrated41')
commands = []

def run(argv, name):
    start = time.monotonic()
    result = subprocess.run(argv, cwd=HERE, capture_output=True)
    log = HERE / (name + '.log')
    log.write_bytes(result.stdout + result.stderr)
    commands.append({'argv': argv, 'cwd': str(HERE), 'exit': result.returncode,
                     'elapsed_s': time.monotonic() - start, 'log': str(log),
                     'log_sha256': hashlib.sha256(log.read_bytes()).hexdigest()})
    (HERE / 'actual-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    if result.returncode:
        raise RuntimeError(name + ' failed; raw evidence retained')
    return result.stdout

run(['xcrun', 'clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wpedantic',
     '-Werror', '-I' + str(ROOT / 'tests'), str(HERE / 'probe.cpp'),
     '-o', str(HERE / 'probe')], 'compile')
observation = json.loads(run([str(HERE / 'probe')], 'observe'))
report = {'schema': 'qcae.actual-macos-peak-rss-calibration/1',
          'observation': observation, 'actual_commands': commands,
          'scope': 'Actual macOS getrusage ru_maxrss byte unit and self PID; no Linux run or product acceptance',
          'files': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in [HERE / 'probe.cpp', ROOT / 'tests/c3_process_observation.hpp']}}
(HERE / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'actual_native_exit': 0, **observation}))
