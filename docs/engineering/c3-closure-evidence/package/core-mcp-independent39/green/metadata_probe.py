from pathlib import Path
import json
import socket
import subprocess
import threading
import time

ROOT = Path('/private/tmp/qcae-core-mcp-readonly-review/green')
BRIDGE = ROOT / 'bridge-fixed-source.py'


def scenario(name):
    endpoint = ROOT / (name + '.sock')
    ready, stop = threading.Event(), threading.Event()
    requests, problems = [], []

    def peer():
        try:
            with socket.socket(socket.AF_UNIX) as listener:
                listener.bind(str(endpoint))
                listener.listen(2)
                listener.settimeout(.1)
                ready.set()
                while not stop.is_set():
                    try:
                        connection, _ = listener.accept()
                    except TimeoutError:
                        continue
                    with connection, connection.makefile('rwb') as stream:
                        handshake = json.loads(stream.readline())
                        stream.write(json.dumps({'request_id': handshake['request_id'], 'status': 'success',
                                                 'data': {'api_version': '1.1'}}).encode() + b'\n')
                        stream.flush()
                        request = json.loads(stream.readline())
                        requests.append(request['operation'])
                        descriptor = {'name': 'probe.read', 'available': True, 'fields': []}
                        if name == 'inner-type-array':
                            descriptor['parameters_schema'] = {'type': 'object', 'properties': {
                                'value': {'type': []}}, 'required': [], 'additionalProperties': False}
                        elif name == 'invalid-context-flag':
                            descriptor['requires_document'] = 'false'
                        response = {'request_id': request['request_id'], 'status': 'success',
                                    'data': {'operations': [descriptor]}}
                        raw = json.dumps(response).encode()
                        if name == 'deep-engine-json':
                            # 141KiB total, far below the bridge's 1MiB bound.
                            nested = b'{"x":' * 20000 + b'0' + b'}' * 20000
                            raw = raw[:-1] + b',"untrusted_metadata":' + nested + b'}'
                        stream.write(raw + b'\n')
                        stream.flush()
        except Exception as error:
            problems.append(repr(error))
            ready.set()

    worker = threading.Thread(target=peer)
    worker.start()
    assert ready.wait(5) and not problems
    child = subprocess.Popen(['python3', str(BRIDGE), '--endpoint', str(endpoint)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    init = {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
        'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'read-only-probe', 'version': '1'}}}
    child.stdin.write(json.dumps(init).encode() + b'\n')
    child.stdin.flush()
    assert 'result' in json.loads(child.stdout.readline())
    child.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    child.stdin.write(b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}\n')
    child.stdin.flush()
    response = child.stdout.readline()
    result = json.loads(response) if response else None
    assert result is not None and result.get("error", {}).get("code") == -32000, result
    child.stdin.write(b'{"jsonrpc":"2.0","id":4,"method":"ping"}\n')
    child.stdin.flush()
    assert json.loads(child.stdout.readline())["result"] == {}
    child.stdin.close()
    child.wait(timeout=5)
    stderr = child.stderr.read().decode()
    stop.set()
    worker.join(timeout=5)
    endpoint.unlink(missing_ok=True)
    assert not worker.is_alive() and not problems, problems
    assert child.returncode == 0 and not stderr and all(name == 'capabilities.list' for name in requests)
    assert len(requests) == (2 if name == 'cached-bad-schema' else 1), requests
    observation = {'scenario': name, 'exit_code': child.returncode, 'tools_list_response': result,
                   'engine_business_requests': requests, 'stderr': stderr,
                   'real_engine': False, 'business_model': False, 'writes': 0}
    (ROOT / (name + '.json')).write_text(json.dumps(observation, indent=2) + '\n')
    print(json.dumps({'scenario': name, 'exit_code': child.returncode,
                      'tools_list_replied': result is not None, 'requests': requests,
                      'recursion_error': 'RecursionError' in stderr}))


for name in ('deep-engine-json', 'inner-type-array', 'invalid-context-flag'):
    scenario(name)
