#!/usr/bin/env python3
"""Collect product-acceptance evidence for the committed source tree.

Builds every matrix this machine supports from a `git archive` export of HEAD, runs
the regression suites and the skeleton evidence runners, hashes every artifact and
writes a report in the format tools/check_skeleton_acceptance.py validates.

Rules:
- Only metrics derived from evidence collected in this run are reported. Every other
  metric is listed in gaps.json with the reason it is missing; nothing is defaulted,
  and the checker rejects a report with missing metrics.
- A zero-violation metric is reported only when the measurement covers its whole
  denominator. A pass-count metric may be reported with partial coverage, because
  cases that did not run count as not passed.
- Every command and its exit code is recorded; a failing command fails the report.

Usage:
  python3 tools/collect_acceptance.py --out /path/outside/repo [--jobs 2] [--reuse]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ROOT / 'docs/baseline/skeleton-acceptance-targets.json'
PROTECTED = ROOT / 'docs/engineering/c4-protected-paths.json'
QT_PREFIX = os.environ.get('QCAE_QT_PREFIX', '/opt/qt/6.11.1/gcc_64')

# Matrix name -> CMake cache arguments. The contract's minimum set is core (all frameworks
# off), SQLite/headless, desktop and core/SQLite under ASan+UBSan; ipc and nastran_off add
# the Qt engine suites and the package-disabled build. The sanitizer matrix includes the Qt
# client so F17 runs instrumented.
MATRICES = {
    'core': ['-DQCAE_BUILD_IPC=OFF', '-DQCAE_BUILD_STORAGE=OFF', '-DQCAE_BUILD_DESKTOP=OFF'],
    'sqlite': ['-DQCAE_BUILD_IPC=OFF', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=OFF'],
    'ipc': ['-DQCAE_BUILD_IPC=ON', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=OFF'],
    'nastran_off': ['-DQCAE_BUILD_IPC=ON', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=OFF',
                    '-DQCAE_ENABLE_NASTRAN_PACKAGE=OFF'],
    'asan': ['-DQCAE_BUILD_IPC=ON', '-DQCAE_BUILD_STORAGE=ON', '-DQCAE_BUILD_DESKTOP=OFF',
             '-DQCAE_ENABLE_SANITIZERS=ON'],
}
SANITIZER_ENV = {'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1',
                 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
ARCHITECTURE_CODES = {
    'unowned_product_translation_units': {'ownership', 'file-api-ownership'},
    'dependency_cycles': {'cycle'},
    'forbidden_dependency_edges': {'core-framework', 'client-domain', 'undeclared-dependency',
                                   'file-api-dependency'},
    'cross_module_private_includes': {'private-header', 'private-include'},
}


class Missing(Exception):
    """A metric cannot be derived from this run's evidence."""


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Collection:
    def __init__(self, out: Path, jobs: int, reuse: bool):
        self.commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        self.root = out.resolve() / self.commit[:12]
        self.source = self.root / 'source'
        self.evidence = self.root / 'evidence'
        self.jobs = jobs
        self.reuse = reuse
        self.commands: list[str] = []
        self.exit_codes: list[int] = []
        self.results: dict[str, int] = {}

    def build_dir(self, matrix: str) -> Path:
        return self.root / 'build' / matrix

    def run(self, name: str, argv: list[str], cwd: Path, env: dict | None = None) -> int:
        """Run one recorded command; its combined output is an evidence log."""
        log = self.evidence / 'logs' / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        started = time.time()
        with log.open('w') as handle:
            result = subprocess.run(argv, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT,
                                    env={**os.environ, **(env or {})}, check=False)
        self.commands.append(' '.join(argv))
        self.exit_codes.append(result.returncode)
        self.results[name] = result.returncode
        print(f'{name}: exit {result.returncode} ({time.time() - started:.0f}s)', flush=True)
        return result.returncode

    def export_source(self) -> None:
        status = subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)
        if status.strip():
            sys.exit('collect_acceptance: the working tree has uncommitted changes; commit first')
        if self.source.exists() and self.reuse:
            return
        shutil.rmtree(self.source, ignore_errors=True)
        self.source.mkdir(parents=True)
        archive = subprocess.run(['git', 'archive', self.commit], cwd=ROOT, capture_output=True, check=True)
        subprocess.run(['tar', '-x', '-C', str(self.source)], input=archive.stdout, check=True)

    def build_matrix(self, matrix: str) -> bool:
        build = self.build_dir(matrix)
        if self.reuse and (build / 'CMakeCache.txt').is_file():
            configured = 0
        else:
            configured = self.run(f'{matrix}-configure', [
                'cmake', '-S', str(self.source), '-B', str(build), '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_PREFIX_PATH={QT_PREFIX}', *MATRICES[matrix]],
                self.root)
        if configured:
            return False
        return self.run(f'{matrix}-build', ['cmake', '--build', str(build), f'-j{self.jobs}'], self.root) == 0

    def test_matrix(self, matrix: str) -> None:
        env = SANITIZER_ENV if matrix == 'asan' else None
        junit = self.evidence / 'ctest' / f'{matrix}.xml'
        junit.parent.mkdir(parents=True, exist_ok=True)
        self.run(f'{matrix}-ctest', ['ctest', '--test-dir', str(self.build_dir(matrix)), f'-j{self.jobs}',
                                     '--timeout', '1800', '--output-on-failure', '--output-junit', str(junit),
                                     '--output-log', str(self.evidence / 'ctest' / f'{matrix}.log')],
                 self.root, env)

    def architecture(self, matrix: str) -> None:
        self.run(f'{matrix}-architecture', [sys.executable, str(self.source / 'tools/check_architecture.py'),
                                            '--root', str(self.source), '--build-dir', str(self.build_dir(matrix)),
                                            '--report', str(self.evidence / 'architecture' / f'{matrix}.json')],
                 self.root)

    def faults(self, tree: str) -> None:
        """Run the 22x10 fault runners (application, Qt client, IPC) and the F17 sanitizer client."""
        out = self.evidence / 'faults'
        out.mkdir(parents=True, exist_ok=True)
        ipc, asan = self.build_dir('ipc'), self.build_dir('asan')
        self.run('faults-application', [str(ipc / 'qcae_skeleton_fault_tests'), str(out / 'application.jsonl'), tree],
                 self.root)
        self.run('faults-client', [str(ipc / 'qcae_skeleton_fault_client_tests'), str(out / 'client.jsonl'), tree],
                 self.root)
        self.run('faults-ipc', [sys.executable, str(self.source / 'tests/skeleton_fault_ipc_tests.py'),
                                '--engine', str(ipc / 'qcae-engine'), '--cli', str(ipc / 'qcae-cli'),
                                '--evidence-dir', str(out / 'ipc'), '--source-tree-sha256', tree], self.root)
        self.run('faults-client-sanitizer', [str(asan / 'qcae_skeleton_fault_client_tests'),
                                             str(out / 'client-sanitizer.jsonl'), tree],
                 self.root, SANITIZER_ENV)
        self.run('faults-report', [sys.executable, str(self.source / 'tests/skeleton_fault_report.py'),
                                   '--application', str(out / 'application.jsonl'),
                                   '--client', str(out / 'client.jsonl'),
                                   '--results', str(out / 'ipc' / 'result-faults.jsonl'),
                                   '--sanitizer-client', str(out / 'client-sanitizer.jsonl'),
                                   '--sanitizer-log', str(self.evidence / 'logs' / 'faults-client-sanitizer.log'),
                                   '--source-tree-sha256', tree, '--output', str(out / 'report.json')], self.root)

    def gates(self) -> None:
        self.run('architecture-mutations', [sys.executable, str(self.source / 'tests/test_architecture_gate.py'), '-v'],
                 self.source)
        self.run('cpp-format', [sys.executable, str(self.source / 'tools/check_cpp_format.py')], self.source)
        self.run('design', [sys.executable, str(self.source / 'tools/check_design.py')], self.source)
        for matrix in ('core', 'ipc'):
            consumer = self.root / 'build' / f'consumers-{matrix}'
            if self.run(f'consumers-{matrix}-configure', [
                    'cmake', '-S', str(self.source / 'tests/public_api_consumers'), '-B', str(consumer), '-G', 'Ninja',
                    f'-DCMAKE_PREFIX_PATH={QT_PREFIX}', *MATRICES[matrix]], self.root) == 0:
                self.run(f'consumers-{matrix}-build', ['cmake', '--build', str(consumer), '--target',
                                                      'qcae_public_consumers', f'-j{self.jobs}'], self.root)


def source_tree_sha(source: Path) -> str:
    """SHA-256 over the sorted (path, content hash) list of the exported tree."""
    digest = hashlib.sha256()
    for path in sorted(p for p in source.rglob('*') if p.is_file()):
        digest.update(f'{path.relative_to(source).as_posix()}\0{sha(path)}\n'.encode())
    return digest.hexdigest()


def read_jsonl(path: Path) -> list[dict]:
    if not path.is_file():
        raise Missing(f'{path.name} was not produced')
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def log_text(c: Collection, name: str) -> str:
    path = c.evidence / 'logs' / f'{name}.log'
    if not path.is_file():
        raise Missing(f'{name} did not run')
    return path.read_text(errors='replace')


def junit_failures(c: Collection) -> tuple[int, int]:
    total = failed = 0
    for matrix in MATRICES:
        path = c.evidence / 'ctest' / f'{matrix}.xml'
        if not path.is_file():
            raise Missing(f'ctest for matrix {matrix} did not run')
        for case in ElementTree.parse(path).getroot().iter('testcase'):
            total += 1
            failed += case.find('failure') is not None or case.find('error') is not None
    return total, failed


def metric_rules(c: Collection) -> dict:
    """Metric name -> zero-argument function returning (value, evidence paths)."""
    def architecture_count(metric: str):
        def rule():
            reports = [c.evidence / 'architecture' / f'{m}.json' for m in MATRICES]
            counts = 0
            for report in reports:
                if not report.is_file():
                    raise Missing(f'architecture report {report.name} missing')
                errors = json.loads(report.read_text())['errors']
                codes = {error.split(':', 1)[0] for error in errors}
                known = set().union(*ARCHITECTURE_CODES.values())
                if codes - known:
                    raise Missing(f'uncategorised architecture errors: {sorted(codes - known)}')
                counts += sum(error.split(':', 1)[0] in ARCHITECTURE_CODES[metric] for error in errors)
            return counts, reports
        return rule

    def negative_cases():
        text = log_text(c, 'architecture-mutations')
        passed = re.findall(r'^test_0[1-8]_\w+ .*\.\.\. ok$', text, re.M)
        return len(passed), [c.evidence / 'logs' / 'architecture-mutations.log']

    def core_build():
        if 'core-build' not in c.results:
            raise Missing('core matrix was not built in this run')
        cache = (c.build_dir('core') / 'CMakeCache.txt').read_text()
        frameworks_off = all(f'{option}:BOOL=OFF' in cache for option in
                             ('QCAE_BUILD_IPC', 'QCAE_BUILD_STORAGE', 'QCAE_BUILD_DESKTOP'))
        return frameworks_off and c.results['core-build'] == 0, [c.evidence / 'logs' / 'core-build.log']

    def consumer_ratio():
        raise Missing('desktop public headers need a Qt/VTK desktop configuration (VTK not installed)')

    def fault_report():
        path = c.evidence / 'faults' / 'report.json'
        if not path.is_file():
            raise Missing('fault report was not produced')
        return json.loads(path.read_text()), path

    def fault_cases():
        report, path = fault_report()
        return sum(v['passed'] == 10 for v in report['cases'].values()), [path]

    def fault_runs():
        report, path = fault_report()
        return sum(v['passed'] for v in report['cases'].values()), [path]

    def warnings():
        count = 0
        logs = []
        for matrix in MATRICES:
            text = log_text(c, f'{matrix}-build')
            logs.append(c.evidence / 'logs' / f'{matrix}-build.log')
            count += sum(1 for line in text.splitlines()
                         if ' warning: ' in line and str(c.source) in line)
        return count, logs

    def format_diffs():
        text = log_text(c, 'cpp-format')
        if re.search(r'^PASS: \d+ C\+\+ files match', text, re.M):
            return 0, [c.evidence / 'logs' / 'cpp-format.log']
        raise Missing('cpp_format did not pass; differing-file count not parsed')

    def sanitizer(marker: str):
        def rule():
            text = (c.evidence / 'ctest' / 'asan.log')
            if not text.is_file():
                raise Missing('sanitizer ctest log missing')
            content = text.read_text(errors='replace') + log_text(c, 'faults-client-sanitizer')
            return len(re.findall(marker, content)), [text, c.evidence / 'logs' / 'faults-client-sanitizer.log']
        return rule

    def test_failures():
        _, failed = junit_failures(c)
        return failed, [c.evidence / 'ctest' / f'{m}.xml' for m in MATRICES]

    rules = {name: architecture_count(name) for name in ARCHITECTURE_CODES}
    rules.update({
        'negative_dependency_cases_detected': negative_cases,
        'core_no_framework_build_pass': core_build,
        'public_api_consumer_pass_ratio': consumer_ratio,
        'fault_cases_passed': fault_cases,
        'fault_runs_passed': fault_runs,
        'first_party_compiler_warnings': warnings,
        'format_diff_files': format_diffs,
        'asan_findings': sanitizer(r'ERROR: AddressSanitizer'),
        'ubsan_findings': sanitizer(r'runtime error:'),
        'applicable_test_failures': test_failures,
    })
    return rules


def environment(c: Collection) -> dict:
    def first(command: list[str]) -> str | None:
        try:
            return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).splitlines()[0].strip()
        except (OSError, subprocess.CalledProcessError, IndexError):
            return None
    os_release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    cpu = next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                if line.startswith('model name')), None)
    memory = next(int(line.split()[1]) * 1024 for line in Path('/proc/meminfo').read_text().splitlines()
                  if line.startswith('MemTotal'))
    sqlite_header = Path('/usr/include/sqlite3.h')
    sqlite = re.search(r'#define SQLITE_VERSION\s+"([^"]+)"', sqlite_header.read_text()).group(1) \
        if sqlite_header.is_file() else None
    qt = re.search(r'/(\d+\.\d+\.\d+)/', QT_PREFIX)
    return {'os': f"{os_release.get('PRETTY_NAME', '').strip(chr(34))} {platform.release()}", 'cpu': cpu,
            'memory_bytes': memory, 'gpu': None, 'graphics_backend': None,
            'compiler': first(['c++', '--version']), 'qt': qt.group(1) if qt else None, 'vtk': None,
            'sqlite': sqlite, 'build_type': 'Release', 'threads': os.cpu_count(),
            'device_pixel_ratio': None, 'framebuffer_pixels': None,
            'notes': 'Headless VM without GPU or VTK; null fields are absent here, not unknown.'}


def finish(c: Collection, tree: str) -> int:
    spec = json.loads(TARGETS.read_text())
    metrics, gaps, provenance = {}, {}, {}
    rules = metric_rules(c)
    for gate in spec['gates']:
        for metric in gate['metrics']:
            name = metric['id']
            try:
                if name not in rules:
                    raise Missing('no collector rule yet')
                value, paths = rules[name]()
                metrics[name] = value
                provenance[name] = [str(Path(p).relative_to(c.root)) for p in paths]
            except Missing as reason:
                gaps[name] = {'gate': gate['id'], 'reason': str(reason)}
    manifests = c.evidence / 'manifests'
    manifests.mkdir(parents=True, exist_ok=True)
    (manifests / 'environment.json').write_text(json.dumps(environment(c), indent=2) + '\n')
    shutil.copy2(c.source / PROTECTED.relative_to(ROOT), manifests / 'protected-paths.json')
    (c.root / 'gaps.json').write_text(json.dumps(gaps, indent=2, ensure_ascii=False) + '\n')
    (c.root / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    evidence = sorted(str(p.relative_to(c.root)) for p in c.evidence.rglob('*') if p.is_file())
    hashes = {path: sha(c.root / path) for path in evidence}
    raw = {'faults': 'evidence/faults/report.json'}
    report = {'kind': 'product_acceptance_report', 'source_commit': c.commit, 'source_tree_sha256': tree,
              'spec_sha256': sha(TARGETS), 'fixture_path': None, 'fixture_sha256': None,
              'environment_manifest_path': 'evidence/manifests/environment.json',
              'environment_manifest_sha256': hashes['evidence/manifests/environment.json'],
              'protected_paths_manifest_path': 'evidence/manifests/protected-paths.json',
              'protected_paths_manifest_sha256': hashes['evidence/manifests/protected-paths.json'],
              'commands': c.commands, 'exit_codes': c.exit_codes,
              'raw_samples_paths': {k: v for k, v in raw.items() if v in hashes},
              'raw_samples_sha256': {k: hashes[v] for k, v in raw.items() if v in hashes},
              'metrics': metrics, 'evidence_paths': evidence, 'evidence_sha256': hashes,
              'extension_records': []}
    (c.root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    rules_by_name = {m['id']: m for g in spec['gates'] for m in g['metrics']}
    failing = {name: value for name, value in metrics.items() if not _passes(rules_by_name[name], value)}
    print(f'measured {len(metrics)}/97 metrics; {len(metrics) - len(failing)} meet target; '
          f'{len(failing)} miss target; {len(gaps)} missing')
    for name, value in sorted(failing.items()):
        print(f'  MISS {name} = {value}')
    print(f'report: {c.root / "report.json"}\ngaps:   {c.root / "gaps.json"}')
    return 0


def _passes(metric: dict, value) -> bool:
    target = metric['target']
    return {'eq': value == target, 'le': value <= target, 'ge': value >= target}[metric['operator']]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True, help='evidence root outside the repository')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--reuse', action='store_true', help='reuse an existing export and configured builds')
    parser.add_argument('--matrices', default=','.join(MATRICES), help='comma-separated subset to build')
    arguments = parser.parse_args()
    if arguments.out.resolve().is_relative_to(ROOT):
        sys.exit('collect_acceptance: --out must be outside the repository')
    c = Collection(arguments.out, arguments.jobs, arguments.reuse)
    c.export_source()
    tree = source_tree_sha(c.source)
    built = {m: c.build_matrix(m) for m in arguments.matrices.split(',')}
    for matrix, ok in built.items():
        if ok:
            c.test_matrix(matrix)
            c.architecture(matrix)
    c.gates()
    if built.get('ipc') and built.get('asan'):
        c.faults(tree)
    return finish(c, tree)


if __name__ == '__main__':
    sys.exit(main())
