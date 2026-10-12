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
# R0 reference inventory and AP-01..20 module contracts, frozen before the cleanup and kept only in this tag.
R0_TAG = 'archive/pre-cleanup-2026-10-10'
R0_FILES = ['docs/engineering/reference-inventory.json',
            *[f'docs/engineering/module-contracts/AP-{i:02}.json' for i in range(1, 21)]]
C4_ARCHIVE = 'docs/engineering/c3-closure-evidence/package'
C4_DIFFS = {'EXT-01': 'c4-independent41/EXT01/extension-baseline.diff',
            'EXT-02': 'c4-independent41/EXT02/extension.diff',
            'EXT-03': 'c4-independent41/EXT03/extension-baseline.diff'}
GENERATED_COMMITTED = {'modules/document/include/qcae/records.hpp'}
CONTRACT_FIELDS = ('reference_evidence', 'owner_module', 'authoritative_data', 'public_contracts',
                   'allowed_dependencies', 'extension_points', 'state_failure_version_rules', 'acceptance_case_ids')
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
        self.suffix = ''

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
        log_path = self.evidence / 'commands.json'
        records = json.loads(log_path.read_text()) if log_path.is_file() else []
        records.append({'name': name, 'argv': argv, 'exit': result.returncode})
        log_path.write_text(json.dumps(records, indent=2) + '\n')
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

    def probes(self, tree: str) -> None:
        """Run evidence-producing tests directly so their own output is the evidence."""
        ipc, out = self.build_dir('ipc'), self.evidence / 'probes'
        out.mkdir(parents=True, exist_ok=True)
        self.run('probe-schema-rejection', [str(ipc / 'qcae_schema_rejection_sqlite_tests')], self.root)
        self.run('probe-legacy', [str(ipc / 'qcae_legacy_migration_tests'),
                                  str(self.source / 'tests/fixtures/legacy')], self.root)
        self.run('probe-mesh-tasks', [str(ipc / 'qcae_geometry_mesh_sqlite_tests')], self.root)
        for probe, binary in (('probe-bp04-regeneration', 'qcae_line_mesh_regeneration_tests'),
                              ('probe-bp09-parameters', 'qcae_parameter_tests'),
                              ('probe-bp11-14-analysis', 'qcae_analysis_check_tests'),
                              ('probe-bp19-results', 'qcae_result_fixture_tests')):
            self.run(probe, [str(ipc / binary)], self.root)
        self.run('probe-package-contributions', [sys.executable,
                                                 str(self.source / 'tests/package_contributions_ipc_tests.py'),
                                                 '--engine', str(ipc / 'qcae-engine'),
                                                 '--evidence-dir', str(out / 'package-contributions')], self.root)
        self.run('probe-package-disabled', [sys.executable, str(self.source / 'tests/nastran_disabled_ipc_tests.py'),
                                            '--engine', str(ipc / 'qcae_platform_engine')], self.root)
        self.run('probe-resources', [str(ipc / 'qcae_resource_tests')], self.root)
        self.run('probe-sync-ipc', [sys.executable, str(self.source / 'tests/c3_sync_ipc_tests.py'),
                                    '--engine', str(ipc / 'qcae-engine'), '--cli', str(ipc / 'qcae-cli'),
                                    '--evidence-dir', str(out / 'sync-ipc')], self.root)
        self.run('probe-operation-handlers', [sys.executable, str(self.source / 'tests/operation_handler_probe.py'),
                                              '--engine', str(ipc / 'qcae-engine'),
                                              '--evidence', str(out / 'operation-handlers.json')],
                 self.source / 'tests')
        self.run('probe-workflow-cli', [sys.executable, str(self.source / 'tests/skeleton_workflow_ipc_tests.py'),
                                        '--engine', str(ipc / 'qcae-engine'), '--cli', str(ipc / 'qcae-cli'),
                                        '--evidence-dir', str(out / 'workflow-cli'), '--source-tree-sha256', tree],
                 self.root)
        self.run('probe-profile-identity', [sys.executable, str(self.source / 'tests/test_profile_identity.py'), '-v'],
                 self.source)
        self.run('probe-representatives-cpp', [str(ipc / 'qcae_skeleton_representative_tests'),
                                               str(out / 'representatives-cpp'), tree], self.root)
        self.run('probe-representatives-ipc', [sys.executable,
                                               str(self.source / 'tests/skeleton_representative_ipc_tests.py'),
                                               '--engine', str(ipc / 'qcae-engine'), '--cli', str(ipc / 'qcae-cli'),
                                               '--evidence-dir', str(out / 'representatives-ipc'),
                                               '--source-tree-sha256', tree], self.root)
        self.run('probe-entity-permutations', [sys.executable, str(self.source / 'tools/generate_entities.py'),
                                               '--schema', str(self.source / 'schemas/entities/entities.json'),
                                               '--output', str(self.source / 'modules/document/include/qcae/records.hpp'),
                                               '--check', '--permutation-check'], self.source)
        for index in (1, 2):
            self.run(f'probe-entity-generation-{index}', [
                sys.executable, str(self.source / 'tools/generate_entities.py'),
                '--schema', str(self.source / 'schemas/entities/entities.json'),
                '--output', str(out / f'records-{index}.hpp')], self.source)
        libclang = Path('/usr/lib/llvm-21/lib/libclang.so')
        if libclang.is_file():
            self.run('probe-protected-ast', [sys.executable, str(self.source / 'tools/check_protected_entity_ast.py'),
                                             '--build-dir', str(ipc), '--libclang', str(libclang),
                                             '--bindings-dir', '/usr/lib/python3/dist-packages',
                                             '--protected-manifest', str(self.source / PROTECTED.relative_to(ROOT)),
                                             '--report', str(out / 'protected-ast.json')], self.root)

    def r0(self) -> None:
        """Copy the frozen R0 inventory and module contracts from the archive tag into the evidence."""
        out = self.evidence / 'r0'
        out.mkdir(parents=True, exist_ok=True)
        tag = subprocess.check_output(['git', 'rev-parse', f'{R0_TAG}^{{commit}}'], cwd=ROOT, text=True).strip()
        for path in R0_FILES:
            data = subprocess.run(['git', 'show', f'{R0_TAG}:{path}'], cwd=ROOT, capture_output=True, check=True).stdout
            (out / Path(path).name).write_bytes(data)
        (out / 'source.json').write_text(json.dumps({'tag': R0_TAG, 'commit': tag, 'files': R0_FILES,
                                                     'note': 'Frozen R0 artifacts; not re-verified against the '
                                                             'reference source tree, which is not on this machine.'},
                                                    indent=2) + '\n')

    def extensions(self) -> None:
        """Archived independent C4 runs (EXT-01..03) and a fresh EXT-04 memory/SQLite contract parity run."""
        out = self.evidence / 'extensions'
        out.mkdir(parents=True, exist_ok=True)
        for name in ('c4-independent41/archive-index.json', 'c4-common40-baseline-manifest.json', *C4_DIFFS.values()):
            data = subprocess.run(['git', 'show', f'{R0_TAG}:{C4_ARCHIVE}/{name}'], cwd=ROOT,
                                  capture_output=True, check=True).stdout
            target = out / name.replace('/', '_')
            target.write_bytes(data)
        ipc = self.build_dir('ipc')
        self.run('ext04-memory', [str(ipc / 'qcae_geometry_mesh_tests')], self.root)
        self.run('ext04-sqlite', [str(ipc / 'qcae_geometry_mesh_sqlite_tests')], self.root)

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


def legacy_fixtures(c: Collection) -> tuple[list[dict], list[str], str]:
    """The frozen legacy fixtures with their schema versions and whether this run migrated them."""
    manifest = json.loads((c.source / 'tests/fixtures/legacy/manifest.json').read_text())
    text = log_text(c, 'probe-legacy')
    checked = set(re.findall(r'^Checking (\S+)$', text, re.M))
    passed = bool(re.search(r'^PASS: .*legacy fixtures migrated; source byte changes 0', text, re.M))
    fixtures = []
    for item in manifest['fixtures']:
        schema = item['schema']
        fixtures.append({'id': item['fixture_id'], 'kind': item['kind'],
                         'path': f"evidence/fixtures/legacy/{item['path']}", 'sha256': item['sha256'],
                         'versions': [f"{item['kind']}:{schema['payload_version']}",
                                      f"model_records:{schema['model_record_schema']}",
                                      f"sqlite_envelope:{schema['sqlite_user_version']}"],
                         'executed': item['fixture_id'] in checked,
                         'passed': passed and item['fixture_id'] in checked})
    declared = sorted(f'{kind}:{version}' for kind, versions in manifest['declared_legacy_schemas'].items()
                      for version in versions)
    return fixtures, declared, text


def representative_records(c: Collection) -> dict[str, list[dict]]:
    """BP-xx records written by the representative probes, grouped by case."""
    probes = c.evidence / 'probes'
    records: dict[str, list[dict]] = {}
    for path in (probes / 'representatives-cpp' / 'mechanism-representatives.jsonl',
                 *sorted((probes / 'representatives-ipc').glob('*.jsonl'))):
        if path.is_file():
            for record in read_jsonl(path):
                if re.fullmatch(r'BP-\d\d', record.get('case_id', '')):
                    records.setdefault(record['case_id'], []).append(record)
    return records


def fault_runs(c: Collection) -> list[dict]:
    """All fault run records, numbered 1..10 within each case in execution order."""
    out = c.evidence / 'faults'
    runs, seen = [], {}
    for path in (out / 'application.jsonl', out / 'client.jsonl', out / 'ipc' / 'result-faults.jsonl'):
        for record in read_jsonl(path):
            case = record.get('case_id')
            seen[case] = seen.get(case, 0) + 1
            runs.append({'case_id': case, 'run_id': seen[case], 'source_run_id': record.get('run_id'),
                         'passed': record.get('passed') is True, 'source_commit': c.commit})
    return runs


def test_only_cmake(path: str, diff: str) -> bool:
    """A CMake change whose every changed line belongs to a test-registration statement is test
    infrastructure, not a product file. Continuation lines belong to the statement above them."""
    if Path(path).name != 'CMakeLists.txt':
        return False
    section = re.search(rf'^diff --git a/{re.escape(path)} b/.*?(?=^diff --git |\Z)', diff, re.M | re.S)
    statement_test, changed = False, False
    for line in (section.group(0) if section else '').splitlines()[1:]:
        if line.startswith(('+++', '---', '@@', 'index ', 'new file', 'deleted file')):
            continue
        body = line[1:] if line[:1] in '+- ' else line
        start = re.match(r'\s*(\w+)\s*\(\s*([^\s)]*)', body)
        if start:
            command, target = start.group(1).lower(), start.group(2)
            statement_test = command in ('add_test', 'set_tests_properties') or target.endswith('_tests') \
                or (command == 'add_executable' and 'tests/' in body)
        if line[:1] in '+-' and body.strip():
            changed = True
            if not statement_test:
                return False
    return changed


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

    probes = c.evidence / 'probes'

    def schema_cases():
        text = log_text(c, 'probe-schema-rejection')
        required = {'missing-required', 'wrong-field-type', 'missing-unit', 'unknown-unit', 'out-of-range',
                    'wrong-reference-type', 'dangling-reference', 'future-record-version'}
        passed = set(re.findall(r'^PASS schema_rejection (\S+) ', text, re.M)) & required
        return len(passed), [c.evidence / 'logs' / 'probe-schema-rejection.log']

    def protected_refs():
        path = probes / 'protected-ast.json'
        if not path.is_file():
            raise Missing('protected AST audit did not run (needs libclang 21 and its Python bindings)')
        report = json.loads(path.read_text())
        manifest = json.loads((c.source / PROTECTED.relative_to(ROOT)).read_text())
        protected_units = sum(p.endswith('.cpp') for p in manifest['paths'])
        if report.get('errors') or report.get('translation_units', 0) < protected_units:
            raise Missing(f"AST audit covered {report.get('translation_units')} of {protected_units} protected units")
        return report['direct_entity_type_references'], [path]

    def permutations():
        text = log_text(c, 'probe-entity-permutations')
        if 'ten registration permutations identical' not in text:
            raise Missing('registration permutation check did not pass; mismatch count not parsed')
        return 0, [c.evidence / 'logs' / 'probe-entity-permutations.log']

    def repeat_generation():
        first, second = probes / 'records-1.hpp', probes / 'records-2.hpp'
        committed = c.source / 'modules/document/include/qcae/records.hpp'
        if not (first.is_file() and second.is_file()):
            raise Missing('repeat entity generation did not produce both outputs')
        def differing(a: bytes, b: bytes) -> int:
            return sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
        return differing(first.read_bytes(), second.read_bytes()) + \
            differing(first.read_bytes(), committed.read_bytes()), [first, second]

    def legacy():
        return legacy_fixtures(c)

    def legacy_ratio():
        fixtures, _, _ = legacy()
        if not fixtures:
            raise Missing('legacy fixture manifest is empty')
        return sum(f['passed'] for f in fixtures) / len(fixtures), [c.evidence / 'logs' / 'probe-legacy.log']

    def legacy_modified():
        _, _, text = legacy()
        match = re.search(r'source byte changes (\d+)', text)
        if not match:
            raise Missing('legacy run did not report source byte changes')
        return int(match.group(1)), [c.evidence / 'logs' / 'probe-legacy.log']

    def legacy_total():
        fixtures, _, _ = legacy()
        return len(fixtures), [c.source / 'tests/fixtures/legacy/manifest.json']

    def legacy_coverage():
        fixtures, declared, _ = legacy()
        covered = {v for f in fixtures if f['passed'] for v in f['versions']}
        return len(covered & set(declared)) / len(declared), [c.evidence / 'logs' / 'probe-legacy.log']

    def legacy_empty():
        fixtures, _, _ = legacy()
        return int(not fixtures), [c.source / 'tests/fixtures/legacy/manifest.json']

    def fault_zero(field: str):
        def rule():
            report, path = fault_report()
            if not report['passed']:
                raise Missing(f'fault runs did not all pass; {field} cannot be attributed')
            return 0, [path]
        return rule

    def representatives():
        return representative_records(c)

    def representative_count():
        records = representatives()
        passed = [case for case, items in records.items() if items and all(r.get('passed') is True for r in items)]
        return len(passed), sorted(p for p in probes.rglob('*representatives*.jsonl'))

    def history_rounds():
        records = representatives().get('BP-12')
        if not records:
            raise Missing('BP-12 (100 undo/redo rounds) did not run')
        failed = sum(a.get('passed') is not True for r in records for a in r.get('assertions', []))
        return failed, sorted(p for p in probes.rglob('*representatives*.jsonl'))

    def mesh_runs():
        lines = [json.loads(line) for line in log_text(c, 'probe-mesh-tasks').splitlines()
                 if line.startswith('{"case"')]
        return lines, [c.evidence / 'logs' / 'probe-mesh-tasks.log']

    def task_runs():
        lines, paths = mesh_runs()
        expected = {'success': 'succeeded', 'cancel_before_commit': 'cancelled', 'stale_candidate': 'conflicted'}
        passed = [r for r in lines if expected.get(r['case']) == r['final_state']]
        return len(passed), paths

    def task_commits(case: str):
        def rule():
            lines, paths = mesh_runs()
            selected = [r for r in lines if r['case'] == case]
            if len(selected) != 10:
                raise Missing(f'{case}: {len(selected)} of 10 runs completed')
            return sum(r['task_model_commits'] for r in selected), paths
        return rule

    def invalid_artifacts():
        negative = [json.loads(line) for line in
                    (probes / 'representatives-cpp' / 'mechanism-representatives.jsonl').read_text().splitlines()
                    if '"BP-05-negative"' in line] if (probes / 'representatives-cpp').is_dir() else []
        if not negative:
            raise Missing('BP-05 negative candidates did not run')
        lines, paths = mesh_runs()
        if len([r for r in lines if r['case'] == 'success']) != 10:
            raise Missing('successful mesh runs incomplete')
        failed = sum(a.get('passed') is not True for r in negative for a in r.get('assertions', []))
        return failed, [*paths, probes / 'representatives-cpp' / 'mechanism-representatives.jsonl']

    def bp20(field: str, limit: int):
        def rule():
            path = probes / 'representatives-cpp' / 'mechanism-representatives.jsonl'
            record = next((r for r in read_jsonl(path) if r.get('case_id') == 'BP-20'), None)
            if not record or not record.get('passed'):
                raise Missing('BP-20 bounded worker/queue case did not pass')
            return int(int(record['actual'][field]) > limit), [path]
        return rule

    def passed_line(probe: str, pattern: str) -> Path:
        """The probe's own summary line; the test's assertions define what passing establishes."""
        if not re.search(pattern, log_text(c, probe), re.M):
            raise Missing(f'{probe} did not pass')
        return c.evidence / 'logs' / f'{probe}.log'

    def record_failures(case: str, source: str):
        path = probes / source
        records = [r for r in read_jsonl(path) if r.get('case_id') == case] if path.is_file() else []
        if not records:
            raise Missing(f'{case} record missing')
        return sum(a.get('passed') is not True for r in records for a in r.get('assertions', [])), path

    def ipc_record_failures(case: str):
        records = representatives().get(case)
        if not records:
            raise Missing(f'{case} did not run')
        failed = sum(a.get('passed') is not True for r in records for a in r.get('assertions', []))
        return failed + sum(r.get('passed') is not True for r in records), \
            sorted((probes / 'representatives-ipc').glob('*.jsonl'))

    def line_meshing():
        lines, paths = mesh_runs()
        failed, records = ipc_record_failures('BP-05')
        return len([r for r in lines if r['case'] == 'success' and r['final_state'] == 'succeeded']) == 10 \
            and failed == 0, [*paths, *records]

    def alias_ids():
        lines, paths = mesh_runs()
        if len([r for r in lines if r['case'] == 'success']) != 10:
            raise Missing('successful mesh runs incomplete')
        regeneration = passed_line('probe-bp04-regeneration', r'^PASS: BP-04 ')
        return 0, [*paths, regeneration]

    def organization_duplicates():
        failed, records = ipc_record_failures('BP-08')
        if failed:
            raise Missing('BP-08 failed; duplicate count cannot be attributed')
        return 0, records

    def bp04_zero():
        regeneration = passed_line('probe-bp04-regeneration', r'^PASS: BP-04 .*reject_unmapped')
        failed, records = ipc_record_failures('BP-04')
        if failed:
            raise Missing('IPC BP-04 failed')
        return 0, [regeneration, *records]

    def replacement_mismatches():
        report, path = fault_report()
        if report['cases'].get('F21') != {'runs': 10, 'passed': 10}:
            raise Missing('F21 replacement runs did not all pass')
        value, paths = bp04_zero()
        return value, [*paths, path]

    def parameter_fixture():
        failed, path = record_failures('BP-09', 'representatives-cpp/mechanism-representatives.jsonl')
        logs = [passed_line('probe-bp09-parameters', r'^PASS: BP-09 '),
                passed_line('probe-bp11-14-analysis', r'^PASS: BP-11/14 ')]
        return failed == 0, [path, *logs]

    def unit_mismatches():
        failed, path = record_failures('BP-09', 'representatives-cpp/mechanism-representatives.jsonl')
        return failed, [path, passed_line('probe-bp09-parameters', r'^PASS: BP-09 ')]

    def analysis_zero():
        return 0, [passed_line('probe-bp11-14-analysis', r'^PASS: BP-11/14 ')]

    def results_zero():
        return 0, [passed_line('probe-bp19-results', r'^PASS: BP-19 .*F22')]

    def wrong_location():
        report, path = fault_report()
        if report['cases'].get('F22') != {'runs': 10, 'passed': 10}:
            raise Missing('F22 runs did not all pass')
        return 0, [passed_line('probe-bp19-results', r'^PASS: BP-19 .*F22'), path]

    def nastran_categories():
        passed_line('probe-package-contributions', r'^PASS: six-category package discovery')
        observations = sorted((probes / 'package-contributions').rglob('observations.json'))
        if not observations:
            raise Missing('package contribution observations missing')
        entries = json.loads(observations[-1].read_text())['capabilities']['package_contributions']
        nastran = next((e for e in entries if e.get('contribution_id') == 'qcae.nastran'), None)
        if nastran is None:
            raise Missing('Nastran package entry missing from capabilities')
        categories = ('core', 'operations', 'codecs', 'validation', 'ui', 'render')
        return sum(bool(nastran.get(category)) for category in categories), \
            [observations[-1], c.evidence / 'logs' / 'probe-package-contributions.log']

    def package_disabled_build():
        if 'nastran_off-build' not in c.results or 'nastran_off-ctest' not in c.results:
            raise Missing('package-disabled matrix was not built and tested in this run')
        disabled = passed_line('probe-package-disabled', r'^PASS: disabled package advertises no Nastran')
        return c.results['nastran_off-build'] == 0 and c.results['nastran_off-ctest'] == 0, \
            [c.evidence / 'logs' / 'nastran_off-build.log', c.evidence / 'ctest' / 'nastran_off.xml', disabled]

    def disabled_false_available():
        return 0, [passed_line('probe-package-disabled', r'^PASS: disabled package advertises no Nastran')]

    def profile_test(name: str):
        def rule():
            text = log_text(c, 'probe-profile-identity')
            if not re.search(rf'^{name} .*\.\.\. ok$', text, re.M):
                raise Missing(f'{name} did not pass')
            return 0, [c.evidence / 'logs' / 'probe-profile-identity.log']
        return rule

    def incompatible_semantics():
        value, paths = profile_test('test_incompatible_semantics')()
        report, path = fault_report()
        if report['cases'].get('F19') != {'runs': 10, 'passed': 10}:
            raise Missing('F19 incompatible-profile runs did not all pass')
        return value, [*paths, path]

    def resource_evidence():
        match = re.search(r'^resource evidence: raw_bytes=(\d+) chunks=(\d+) max_raw_chunk=(\d+) '
                          r'max_framed_base64_bytes=(\d+) sha256=([0-9a-f]+)', log_text(c, 'probe-resources'), re.M)
        passed_line('probe-resources', r'^PASS: versioned resources')
        if not match:
            raise Missing('resource round-trip evidence line missing')
        return match, [c.evidence / 'logs' / 'probe-resources.log']

    def bulk_bytes():
        match, paths = resource_evidence()
        return int(match.group(1)), paths

    def chunk_max():
        # Framed wire bytes bound the payload from above, so this does not understate the chunk size.
        match, paths = resource_evidence()
        return max(int(match.group(3)), int(match.group(4))), paths

    def event_gaps():
        path = probes / 'sync-ipc' / 'c3-sync-ipc-summary.json'
        if not path.is_file():
            raise Missing('sync IPC summary missing')
        gap = json.loads(path.read_text()).get('event_gap') or {}
        report, faults = fault_report()
        if report['cases'].get('F16') != {'runs': 10, 'passed': 10}:
            raise Missing('F16 event-gap fault runs did not all pass')
        handled = (gap.get('dropped_retained_events') is True and gap.get('resync_required') is True and
                   gap.get('reconnect_required_resync') is True and
                   gap.get('fresh_full_resource_revision') == gap.get('authoritative_revision'))
        return int(not handled), [path, faults]

    def locality():
        raise Missing('needs end-to-end measurement through socket frames and derived display; '
                      'the in-process bench leaves socket bytes unmeasured and the end-to-end bench needs VTK')

    def handler_probe():
        path = probes / 'operation-handlers.json'
        if not path.is_file():
            raise Missing('operation handler probe did not run')
        return json.loads(path.read_text()), path

    def contract_ratio():
        probe, path = handler_probe()
        typed = {op['operation_id'] for schema in (c.source / 'schemas/operations').glob('*.json')
                 for op in json.loads(schema.read_text())['operations']}
        available = [r['operation'] for r in probe['results']]
        return sum(name in typed for name in available) / len(available), [path, c.source / 'schemas/operations']

    def missing_handlers():
        probe, path = handler_probe()
        return len(probe['missing_handlers']), [path]

    def workflow_runs():
        # Only the pure-CLI mode runs here; the GUI and GUI/script modes need the VTK desktop and count as
        # not passed.
        runs = [json.loads(line) for line in log_text(c, 'probe-workflow-cli').splitlines()
                if line.startswith('{"mode"')]
        return sum(r.get('passed') is True for r in runs), [c.evidence / 'logs' / 'probe-workflow-cli.log']

    def delivery_categories():
        present = {
            'reference_inventory_and_module_contracts': False,
            'dependency_and_public_api_checks': (c.evidence / 'architecture').is_dir() and
                                                c.results.get('consumers-core-build') == 0,
            'schema_and_generated_results': (probes / 'records-1.hpp').is_file(),
            'main_workflow_inputs_outputs': (probes / 'workflow-cli').is_dir(),
            'fault_records_220': (c.evidence / 'raw' / 'faults.json').is_file() and
                                 len(json.loads((c.evidence / 'raw' / 'faults.json').read_text())) == 220,
            'extension_diffs_and_records': False,
            'performance_raw_samples': False,
            'build_regression_migration_logs': (c.evidence / 'ctest').is_dir() and
                                               (c.evidence / 'logs' / 'probe-legacy.log').is_file(),
        }
        (c.evidence / 'raw').mkdir(parents=True, exist_ok=True)
        (c.evidence / 'raw' / 'delivery-categories.json').write_text(json.dumps(present, indent=2) + '\n')
        return sum(present.values()), [c.evidence / 'raw' / 'delivery-categories.json']

    r0_dir = c.evidence / 'r0'

    def contracts():
        paths = [r0_dir / f'AP-{i:02}.json' for i in range(1, 21)]
        if not all(p.is_file() for p in paths):
            raise Missing('R0 module contracts were not extracted')
        return {p.stem: json.loads(p.read_text()) for p in paths}, paths

    def contract_count():
        loaded, paths = contracts()
        return len(loaded), paths

    def required_fields():
        loaded, paths = contracts()
        return min(sum(field in contract for field in CONTRACT_FIELDS) for contract in loaded.values()), paths

    def missing_fields():
        loaded, paths = contracts()
        def empty(value) -> bool:
            text = json.dumps(value)
            return value in (None, '', [], {}) or 'TODO' in text or 'placeholder' in text.lower()
        return sum(field not in contract or empty(contract[field])
                   for contract in loaded.values() for field in CONTRACT_FIELDS), paths

    def inventory():
        path = r0_dir / 'reference-inventory.json'
        if not path.is_file():
            raise Missing('R0 reference inventory was not extracted')
        return json.loads(path.read_text()), path

    def mapping_ratio():
        frozen, path = inventory()
        items = frozen['items'] + frozen['sdk_public_boundaries']
        if not items:
            raise Missing('R0 inventory is empty')
        mapped = sum(bool(item.get('ap_domains')) and bool(item.get('qcae_owner')) for item in items)
        return mapped / len(items), [path]

    def unclassified():
        frozen, path = inventory()
        items = frozen['items'] + frozen['sdk_public_boundaries']
        return sum(not item.get('ap_domains') or not item.get('qcae_owner') for item in items), [path]

    def level2():
        # An AP counts once its representative BP case passed in this run; public headers are built by the
        # consumer projects.
        loaded, paths = contracts()
        cases = representative_records(c)
        passed = {case for case, items in cases.items() if items and all(r.get('passed') is True for r in items)}
        if c.results.get('consumers-core-build') != 0:
            raise Missing('public API consumers did not build')
        count = sum(any(bp in passed for bp in contract['acceptance_case_ids'] if str(bp).startswith('BP-'))
                    for contract in loaded.values())
        return count, [*paths, c.evidence / 'raw' / 'representatives.json']

    ext_dir = c.evidence / 'extensions'

    def archive_index():
        path = ext_dir / 'c4-independent41_archive-index.json'
        if not path.is_file():
            raise Missing('archived C4 extension index was not extracted')
        return json.loads(path.read_text()), path

    def extension_diff(ext: str):
        index, index_path = archive_index()
        path = ext_dir / C4_DIFFS[ext].replace('/', '_')
        key = ext.replace('-', '')
        if sha(path) != index['independent_full_diff_sha256'][key]:
            raise Missing(f'{ext} archived diff hash differs from the archive index')
        text = path.read_text(errors='replace')
        files = re.findall(r'^diff --git a/(\S+) b/', text, re.M)
        return files, [path, index_path], text

    def protected_paths():
        manifest = json.loads((ext_dir / 'c4-common40-baseline-manifest.json').read_text())
        return {item['path'] for item in manifest['protected_files']}

    def ext_protected(ext: str):
        def rule():
            files, paths, _ = extension_diff(ext)
            return len(set(files) & protected_paths()), [*paths, ext_dir / 'c4-common40-baseline-manifest.json']
        return rule

    def ext_handwritten(ext: str):
        def rule():
            files, paths, text = extension_diff(ext)
            index, _ = archive_index()
            if index.get('generator_change_count') != 0:
                raise Missing('generators changed; generated files are not exempt')
            handwritten = [f for f in files if not f.startswith(('tests/', 'docs/')) and not f.endswith('.md')
                           and f not in GENERATED_COMMITTED and not test_only_cmake(f, text)]
            return len(handwritten), paths
        return rule

    def ext04_parity() -> bool:
        def runs(name: str) -> list[tuple]:
            return [(r['case'], r['final_state'], r['task_model_commits']) for r in
                    (json.loads(line) for line in log_text(c, name).splitlines() if line.startswith('{"case"'))]
        memory, sqlite = runs('ext04-memory'), runs('ext04-sqlite')
        return len(memory) == 30 and memory == sqlite and c.results.get('ext04-memory') == 0 \
            and c.results.get('ext04-sqlite') == 0

    def exercises():
        index, path = archive_index()
        archived = sum(1 for ext in C4_DIFFS if index['independent_commits'].get(ext.replace('-', '')))
        ext04 = ext04_parity()
        # EXT-05 reruns the end-to-end locality measurement, which needs the VTK desktop; it counts as not passed.
        return archived + int(ext04), [path, c.evidence / 'logs' / 'ext04-memory.log',
                                       c.evidence / 'logs' / 'ext04-sqlite.log']

    def independent_runs():
        index, path = archive_index()
        return sum(1 for ext in C4_DIFFS if index['independent_commits'].get(ext.replace('-', ''))), [path]

    def initial_context_bytes():
        index, path = archive_index()
        return index['initial_eight_file_content_bytes'], [path]

    def initial_context_files():
        index, path = archive_index()
        if 'initial_eight_file_content_bytes' not in index:
            raise Missing('archived initial context record missing')
        return 8, [path]

    def author_patches():
        index, path = archive_index()
        return index['skeleton_author_product_patches_during_experiments'], [path]

    rules = {name: architecture_count(name) for name in ARCHITECTURE_CODES}
    rules.update({
        'extension_exercises_passed': exercises,
        'ext01_protected_files_changed': ext_protected('EXT-01'),
        'ext02_protected_files_changed': ext_protected('EXT-02'),
        'ext03_protected_files_changed': ext_protected('EXT-03'),
        'ext01_handwritten_product_files_changed': ext_handwritten('EXT-01'),
        'ext02_handwritten_product_files_changed': ext_handwritten('EXT-02'),
        'ext03_handwritten_product_files_changed': ext_handwritten('EXT-03'),
        'independent_extension_runs_passed': independent_runs,
        'independent_initial_context_bytes': initial_context_bytes,
        'independent_initial_context_files': initial_context_files,
        'skeleton_author_product_patches_during_extension': author_patches,
        'ap01_20_contract_count': contract_count,
        'ap_contract_required_fields': required_fields,
        'ap_contract_missing_fields': missing_fields,
        'inventory_mapping_ratio': mapping_ratio,
        'inventory_unclassified_count': unclassified,
        'ap_level2_count': level2,
        'main_workflow_runs_passed': workflow_runs,
        'delivery_artifact_categories': delivery_categories,
        'registered_operation_contract_ratio': contract_ratio,
        'advertised_available_missing_handlers': missing_handlers,
        'bulk_resource_roundtrip_bytes': bulk_bytes,
        'bulk_chunk_payload_bytes_max': chunk_max,
        'event_gap_silent_stale_views': event_gaps,
        'locality_fixture_sizes_passed': locality,
        'locality_measurement_runs_passed': locality,
        'locality_payload_bound_violations': locality,
        'whole_document_serialization_calls_on_local_edit': locality,
        'nastran_contribution_categories': nastran_categories,
        'package_disabled_core_build_pass': package_disabled_build,
        'disabled_package_false_available_operations': disabled_false_available,
        'format_only_semantic_digest_changes': profile_test('test_comments_and_formatting'),
        'incompatible_semantics_silently_accepted': incompatible_semantics,
        'line_meshing_fixture_pass': line_meshing,
        'geometry_mesh_alias_id_count': alias_ids,
        'organization_generated_duplicate_nodes': organization_duplicates,
        'unreported_invalid_geometry_bindings': bp04_zero,
        'replacement_reference_mismatches': replacement_mismatches,
        'parameter_analysis_issue_fixture_pass': parameter_fixture,
        'unit_coordinate_value_mismatches': unit_mismatches,
        'issue_entity_resolution_errors': analysis_zero,
        'stale_issues_marked_current': analysis_zero,
        'repair_operations_bypassing_coordinator': analysis_zero,
        'result_fixture_value_mismatches': results_zero,
        'result_metadata_missing_fields': results_zero,
        'old_results_marked_current': results_zero,
        'wrong_location_or_mapping_accepted': wrong_location,
        'fixture_results_labeled_solver_runs': results_zero,
        'entity_specific_type_refs_in_protected_sources': protected_refs,
        'repeat_generation_diff_bytes': repeat_generation,
        'registration_order_identity_mismatches': permutations,
        'schema_validation_cases_passed': schema_cases,
        'current_old_schema_fixture_pass_ratio': legacy_ratio,
        'partial_commit_count': fault_zero('partial commits'),
        'duplicate_commit_count': fault_zero('duplicate commits'),
        'history_100_rounds_mismatches': history_rounds,
        'legacy_project_fixture_pass_ratio': legacy_ratio,
        'legacy_inputs_modified_bytes': legacy_modified,
        'legacy_fixture_total': legacy_total,
        'legacy_version_coverage_ratio': legacy_coverage,
        'legacy_fixture_empty_set': legacy_empty,
        'real_task_runs_passed': task_runs,
        'cancel_before_commit_model_mutations': task_commits('cancel_before_commit'),
        'stale_task_result_commits': task_commits('stale_candidate'),
        'invalid_artifacts_marked_success': invalid_artifacts,
        'queue_limit_violations': bp20('queued_before', 8),
        'active_worker_limit_violations': bp20('active_workers_before', 2),
        'representative_cases_passed': representative_count,
    })

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
    # SK-14's prerequisite count: SK-01..13 gates with every metric measured and meeting its target.
    by_name = {m['id']: m for g in spec['gates'] for m in g['metrics']}
    complete = [g['id'] for g in spec['gates'][:13]
                if all(m['id'] in metrics and _passes(by_name[m['id']], metrics[m['id']]) for m in g['metrics'])]
    metrics['prerequisite_gates_passed'] = len(complete)
    provenance['prerequisite_gates_passed'] = complete
    gaps.pop('prerequisite_gates_passed', None)
    manifests = c.evidence / 'manifests'
    manifests.mkdir(parents=True, exist_ok=True)
    (manifests / 'environment.json').write_text(json.dumps(environment(c), indent=2) + '\n')
    shutil.copy2(c.source / PROTECTED.relative_to(ROOT), manifests / 'protected-paths.json')
    try:
        fixtures, declared, _ = legacy_fixtures(c)
        for fixture in fixtures:
            copy = c.root / fixture['path']
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(c.source / 'tests/fixtures/legacy' / copy.name, copy)
        (manifests / 'fixtures.json').write_text(json.dumps(
            {'legacy_fixtures': fixtures, 'supported_legacy_versions': declared}, indent=2) + '\n')
    except Missing as reason:
        gaps['_fixture_manifest'] = {'gate': 'report', 'reason': str(reason)}
    raw_dir = c.evidence / 'raw'
    raw_dir.mkdir(parents=True, exist_ok=True)
    cases = representative_records(c)
    (raw_dir / 'representatives.json').write_text(json.dumps(
        [{'case_id': case, 'passed': all(r.get('passed') is True for r in items), 'source_commit': c.commit,
          'source_run_ids': [r.get('run_id') for r in items]} for case, items in sorted(cases.items())],
        indent=2) + '\n')
    try:
        (raw_dir / 'faults.json').write_text(json.dumps(fault_runs(c), indent=2) + '\n')
    except Missing as reason:
        gaps['_fault_runs'] = {'gate': 'report', 'reason': str(reason)}
    (c.root / f'gaps{c.suffix}.json').write_text(json.dumps(gaps, indent=2, ensure_ascii=False) + '\n')
    (c.root / f'provenance{c.suffix}.json').write_text(json.dumps(provenance, indent=2) + '\n')
    evidence = sorted(str(p.relative_to(c.root)) for p in c.evidence.rglob('*') if p.is_file())
    hashes = {path: sha(c.root / path) for path in evidence}
    raw = {'faults': 'evidence/raw/faults.json', 'representatives': 'evidence/raw/representatives.json'}
    report = {'kind': 'product_acceptance_report', 'source_commit': c.commit, 'source_tree_sha256': tree,
              'spec_sha256': sha(TARGETS),
              'fixture_path': 'evidence/manifests/fixtures.json',
              'fixture_sha256': hashes.get('evidence/manifests/fixtures.json'),
              'environment_manifest_path': 'evidence/manifests/environment.json',
              'environment_manifest_sha256': hashes['evidence/manifests/environment.json'],
              'protected_paths_manifest_path': 'evidence/manifests/protected-paths.json',
              'protected_paths_manifest_sha256': hashes['evidence/manifests/protected-paths.json'],
              'commands': c.commands, 'exit_codes': c.exit_codes,
              'raw_samples_paths': {k: v for k, v in raw.items() if v in hashes},
              'raw_samples_sha256': {k: hashes[v] for k, v in raw.items() if v in hashes},
              'metrics': metrics, 'evidence_paths': evidence, 'evidence_sha256': hashes,
              'extension_records': extension_records(c, hashes)}
    (c.root / f'report{c.suffix}.json').write_text(json.dumps(report, indent=2) + '\n')
    rules_by_name = {m['id']: m for g in spec['gates'] for m in g['metrics']}
    failing = {name: value for name, value in metrics.items() if not _passes(rules_by_name[name], value)}
    print(f'measured {len(metrics)}/97 metrics; {len(metrics) - len(failing)} meet target; '
          f'{len(failing)} miss target; {len(gaps)} missing')
    for name, value in sorted(failing.items()):
        print(f'  MISS {name} = {value}')
    print(f'report: {c.root / f"report{c.suffix}.json"}\ngaps:   {c.root / f"gaps{c.suffix}.json"}')
    return 0


def extension_records(c: Collection, hashes: dict) -> list[dict]:
    """EXT-01..03 from the archived independent C4 runs. EXT-04 has no extension commit and EXT-05 has
    not run, so the checker correctly rejects the incomplete set."""
    index_path = c.evidence / 'extensions' / 'c4-independent41_archive-index.json'
    if not index_path.is_file():
        return []
    index = json.loads(index_path.read_text())
    records = []
    for ext, diff in C4_DIFFS.items():
        relative = f"evidence/extensions/{diff.replace('/', '_')}"
        records.append({'id': ext, 'baseline_commit': index['frozen_baseline_commit'],
                        'extension_commit': index['independent_commits'][ext.replace('-', '')],
                        'diff_path': relative, 'diff_sha256': hashes.get(relative),
                        'source': 'archived independent C4 run (archive/pre-cleanup-2026-10-10)'})
    return records


def _passes(metric: dict, value) -> bool:
    target = metric['target']
    return {'eq': value == target, 'le': value <= target, 'ge': value >= target}[metric['operator']]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True, help='evidence root outside the repository')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--reuse', action='store_true', help='reuse an existing export and configured builds')
    parser.add_argument('--matrices', default=','.join(MATRICES), help='comma-separated subset to build')
    parser.add_argument('--rescore', type=Path,
                        help='recompute metrics from an existing collection directory without building or running; '
                             'writes report/gaps/provenance with a .rescore suffix')
    arguments = parser.parse_args()
    if arguments.out.resolve().is_relative_to(ROOT):
        sys.exit('collect_acceptance: --out must be outside the repository')
    c = Collection(arguments.out, arguments.jobs, arguments.reuse)
    if arguments.rescore:
        c.root = arguments.rescore.resolve()
        c.source, c.evidence, c.suffix = c.root / 'source', c.root / 'evidence', '.rescore'
        previous = c.root / 'report.json'
        if previous.is_file():
            c.commit = json.loads(previous.read_text())['source_commit']
        commands = c.evidence / 'commands.json'
        for record in json.loads(commands.read_text()) if commands.is_file() else []:
            c.commands.append(' '.join(record['argv']))
            c.exit_codes.append(record['exit'])
            c.results[record['name']] = record['exit']
        return finish(c, source_tree_sha(c.source))
    c.export_source()
    tree = source_tree_sha(c.source)
    built = {m: c.build_matrix(m) for m in arguments.matrices.split(',')}
    for matrix, ok in built.items():
        if ok:
            c.test_matrix(matrix)
            c.architecture(matrix)
    c.gates()
    c.r0()
    if built.get('ipc'):
        c.probes(tree)
        c.extensions()
    if built.get('ipc') and built.get('asan'):
        c.faults(tree)
    return finish(c, tree)


if __name__ == '__main__':
    sys.exit(main())
