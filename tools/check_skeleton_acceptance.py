#!/usr/bin/env python3
"""Validate frozen targets and acceptance evidence; never treat target values as results."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ROOT / 'docs/baseline/skeleton-acceptance-targets.json'

class EvidenceError(ValueError):
    pass

def require(condition, message):
    if not condition:
        raise EvidenceError(message)

def read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f'duplicate JSON key: {key}')
            result[key] = value
        return result
    return json.loads(path.read_text(), object_pairs_hook=unique,
                      parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError(f'nonfinite JSON number: {value}')))

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def specification(spec):
    require(spec.get('kind') == 'target_specification_not_results', 'wrong specification kind')
    gates = spec.get('gates', [])
    require([g['id'] for g in gates] == [f'SK-{i:02}' for i in range(1, 15)], 'gate set/order mismatch')
    metrics = {}
    for gate in gates:
        require(gate.get('required') is True, 'cannot disable a required gate')
        for metric in gate['metrics']:
            name = metric['id']
            require(name not in metrics, f'duplicate metric: {name}')
            require(metric['operator'] in ('eq', 'le', 'ge'), f'unknown operator: {name}')
            require(type(metric['target']) in (int, float, bool), f'invalid target: {name}')
            metrics[name] = (gate['id'], metric)
    require(len(metrics) == 97, 'frozen specification must have 97 metrics')
    f = spec['fixtures']
    require(f['node_edit_payload_bytes_max'] == f['dirty_node_blocks_max'] * f['node_block_capacity'] * f['node_record_encoded_bytes_max'] + f['metadata_payload_bytes_max'], 'node payload arithmetic')
    require(f['material_edit_payload_bytes_max'] == f['material_record_encoded_bytes_max'] + f['metadata_payload_bytes_max'], 'material payload arithmetic')
    require(f['fault_case_count'] * f['fault_repetitions_per_case'] == 220, 'fault arithmetic')
    require(f['history_initial_edit_count'] + 2 * f['history_undo_redo_rounds'] == 201, 'history arithmetic')
    return metrics

def artifact(root, relative, expected_hash):
    require(isinstance(relative, str) and relative, 'missing artifact path')
    path = Path(relative)
    require(not path.is_absolute(), f'absolute evidence path: {relative}')
    resolved = (root / path).resolve()
    require(resolved.is_relative_to(root.resolve()), f'evidence escapes root: {relative}')
    require(resolved.is_file(), f'missing evidence: {relative}')
    require(isinstance(expected_hash, str) and re.fullmatch(r'[0-9a-f]{64}', expected_hash), 'invalid SHA256')
    require(sha(resolved) == expected_hash, f'evidence hash mismatch: {relative}')
    return resolved

def samples_p95(values, expected_count):
    require(isinstance(values, list) and len(values) == expected_count, 'raw sample count mismatch')
    require(all(type(x) in (int, float) and math.isfinite(x) and x >= 0 for x in values), 'invalid timing samples')
    return sorted(values)[math.ceil(0.95 * len(values)) - 1]

def validate_report(report, spec, root, expected_commit, expected_spec_hash):
    metrics = specification(spec)
    require(report.get('kind') == 'product_acceptance_report', 'not a product acceptance report')
    require(report.get('source_commit') == expected_commit, 'release commit mismatch')
    require(report.get('spec_sha256') == expected_spec_hash, 'specification hash mismatch')
    for name in spec['report_required_fields']:
        require(name in report, f'missing report field: {name}')
    require(isinstance(report['commands'], list) and report['commands'], 'no executed commands')
    require(len(report['commands']) == len(report['exit_codes']), 'exit-code count mismatch')
    require(all(type(x) is int and x == 0 for x in report['exit_codes']), 'failed or malformed command exit code')
    evidence = report['evidence_paths']
    hashes = report['evidence_sha256']
    require(isinstance(evidence, list) and evidence and len(evidence) == len(set(evidence)), 'empty/duplicate evidence list')
    require(set(evidence) == set(hashes), 'evidence/hash list mismatch')
    for path in evidence:
        artifact(root, path, hashes[path])
    for name in ('fixture', 'environment_manifest', 'protected_paths_manifest'):
        path = report.get(name + '_path')
        require(path in evidence, f'{name} is not an evidence artifact')
        require(hashes[path] == report[name + '_sha256'], f'{name} hash mismatch')
    fixtures = read_json(root / report['fixture_path'])
    legacy = fixtures.get('legacy_fixtures', [])
    require(isinstance(legacy, list) and len(legacy) >= spec['fixtures']['legacy_fixture_minimum'], 'empty/undersized legacy fixture denominator')
    require(len({item.get('id') for item in legacy}) == len(legacy), 'duplicate legacy fixture IDs')
    supported = fixtures.get('supported_legacy_versions', [])
    require(isinstance(supported, list) and supported, 'empty supported legacy version denominator')
    covered = set()
    for item in legacy:
        require(item.get('passed') is True and item.get('executed') is True, 'unexecuted/failed legacy fixture')
        require(item.get('path') in evidence and hashes[item['path']] == item.get('sha256'), 'missing legacy fixture evidence')
        covered.update(item.get('versions', []))
    require(set(supported) <= covered, 'legacy version coverage gap')
    env = read_json(root / report['environment_manifest_path'])
    for name in ('os', 'cpu', 'memory_bytes', 'gpu', 'graphics_backend', 'compiler', 'qt', 'vtk', 'sqlite', 'build_type', 'threads', 'device_pixel_ratio', 'framebuffer_pixels'):
        require(name in env and env[name] not in (None, '', []), f'missing environment field: {name}')
    require(env['build_type'] == 'Release' and env['framebuffer_pixels'] == spec['fixtures']['framebuffer_pixels'], 'performance configuration mismatch')
    protected = read_json(root / report['protected_paths_manifest_path'])
    require(isinstance(protected.get('paths'), list) and protected['paths'], 'missing protected paths')
    measured = report['metrics']
    require(isinstance(measured, dict) and set(measured) == set(metrics), 'missing or unknown metric keys')
    failures = []
    for name, (gate, rule) in metrics.items():
        actual, target = measured[name], rule['target']
        expected_type = bool if type(target) is bool else (int, float) if rule['unit'] in ('ms', 'ratio') else int if type(target) is int else (int, float)
        require(type(actual) in ((expected_type,) if isinstance(expected_type, type) else expected_type), f'metric type mismatch: {name}')
        if type(actual) is float:
            require(math.isfinite(actual), f'nonfinite metric: {name}')
        require(type(actual) is bool or actual >= 0, f'negative metric: {name}')
        passed = {'eq': actual == target, 'le': actual <= target, 'ge': actual >= target}[rule['operator']]
        if not passed:
            failures.append(f'{gate} {name}: {actual} {rule["operator"]} {target} failed')
    raw_paths = report['raw_samples_paths']
    raw_hashes = report['raw_samples_sha256']
    require(isinstance(raw_paths, dict) and set(raw_paths) == {'rotation', 'zoom', 'input_feedback', 'task_ack', 'faults', 'representatives'}, 'raw evidence kinds missing')
    for kind, path in raw_paths.items():
        require(path in evidence and hashes[path] == raw_hashes.get(kind), f'raw sample evidence mismatch: {kind}')
    timing = [('rotation', 'rotation_samples', 'rotation_frame_p95_ms'), ('zoom', 'zoom_samples', 'zoom_frame_p95_ms'), ('input_feedback', 'input_feedback_samples', 'input_feedback_p95_ms'), ('task_ack', 'task_ack_samples', 'task_ack_p95_ms')]
    for kind, count_key, metric in timing:
        raw = read_json(root / raw_paths[kind])
        require(raw.get('source_commit') == expected_commit, f'raw sample commit mismatch: {kind}')
        require(raw.get('environment_manifest_sha256') == report['environment_manifest_sha256'], f'raw sample environment mismatch: {kind}')
        expected_warmup = spec['fixtures']['warmup_frames'] if kind in ('rotation', 'zoom') else spec['fixtures']['warmup_input_operations'] if kind == 'input_feedback' else 0
        require(raw.get('warmup_count') == expected_warmup, f'warmup mismatch: {kind}')
        value = samples_p95(raw.get('samples_ms'), spec['fixtures'][count_key])
        require(abs(value - measured[metric]) <= 1e-9, f'reported P95 differs from raw samples: {metric}')
    faults = read_json(root / raw_paths['faults'])
    require(isinstance(faults, list) and len(faults) == 220, 'need 220 fault runs')
    seen = set()
    for item in faults:
        require(item.get('source_commit') == expected_commit and item.get('passed') is True, 'fault run failed or wrong commit')
        key = (item.get('case_id'), item.get('run_id'))
        require(key[0] in {f'F{i:02}' for i in range(1, 23)} and type(key[1]) is int and 1 <= key[1] <= 10, 'unknown fault case/run')
        require(key not in seen, 'duplicate fault run')
        seen.add(key)
    representative = read_json(root / raw_paths['representatives'])
    require(isinstance(representative, list) and len(representative) == 20, 'need 20 representative cases')
    require({x.get('case_id') for x in representative} == {f'BP-{i:02}' for i in range(1, 21)}, 'representative case IDs mismatch')
    require(all(x.get('source_commit') == expected_commit and x.get('passed') is True for x in representative), 'representative failure or wrong commit')
    extensions = report['extension_records']
    require(isinstance(extensions, list) and {x.get('id') for x in extensions} == {f'EXT-{i:02}' for i in range(1,6)} and len(extensions)==5, 'extension record set mismatch')
    baselines = {x.get('baseline_commit') for x in extensions}
    require(len(baselines) == 1 and re.fullmatch(r'[0-9a-f]{40}', next(iter(baselines)) or ''), 'extension baselines differ/invalid')
    for extension in extensions:
        require(re.fullmatch(r'[0-9a-f]{40}', extension.get('extension_commit', '')), 'missing extension commit')
        require(extension.get('diff_path') in evidence and hashes[extension['diff_path']] == extension.get('diff_sha256'), 'missing extension diff evidence')
    require(not failures, '\n'.join(failures))
    return 'PASS: supplied report thresholds and evidence integrity; measurement provenance still requires collector/review evidence.'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--artifact-root', type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        spec = read_json(TARGETS)
        metrics = specification(spec)
        if args.report is None:
            print(f'PASS: target specification only ({len(spec["gates"])} gates, {len(metrics)} metrics). No product gate has been measured or passed by this command.')
        else:
            commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
            print(validate_report(read_json(args.report), spec, args.artifact_root.resolve(), commit, sha(TARGETS)))
        return 0
    except (EvidenceError, ValueError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
