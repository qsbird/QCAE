"""Tests of report validation only; all report data below is synthetic, never product evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
module_spec = importlib.util.spec_from_file_location('acceptance', ROOT / 'tools/check_skeleton_acceptance.py')
checker = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(checker)

class AcceptanceCheckerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='qcae-checker-selftest-')
        self.root = Path(self.temp.name)
        self.spec = checker.read_json(checker.TARGETS)
        self.commit = 'a' * 40
        self.report = {'kind': 'product_acceptance_report', 'source_commit': self.commit,
                       'spec_sha256': checker.sha(checker.TARGETS), 'commands': ['synthetic-checker-selftest'],
                       'exit_codes': [0], 'evidence_paths': [], 'evidence_sha256': {},
                       'metrics': {m['id']: m['target'] for g in self.spec['gates'] for m in g['metrics']},
                       'raw_samples_paths': {}, 'raw_samples_sha256': {}, 'extension_records': []}
        legacy = []
        for i in range(6):
            path, digest = self.add(f'legacy/{i}.json', {'test_only': True})
            legacy.append({'id': str(i), 'path': path, 'sha256': digest, 'passed': True, 'executed': True, 'versions': ['project-v1']})
        for name, data in (
            ('fixture', {'legacy_fixtures': legacy, 'supported_legacy_versions': ['project-v1']}),
            ('environment_manifest', {'os':'test','cpu':'test','memory_bytes':1024,'gpu':'test','graphics_backend':'test','compiler':'test','qt':'test','vtk':'test','sqlite':'test','build_type':'Release','threads':1,'device_pixel_ratio':1,'framebuffer_pixels':[1280,720]}),
            ('protected_paths_manifest', {'paths':['generic/']})):
            path, digest = self.add(name+'.json', data)
            self.report[name+'_path'] = path
            self.report[name+'_sha256'] = digest
        for kind, count_key, metric, warmup in (
            ('rotation','rotation_samples','rotation_frame_p95_ms',60),
            ('zoom','zoom_samples','zoom_frame_p95_ms',60),
            ('input_feedback','input_feedback_samples','input_feedback_p95_ms',5),
            ('task_ack','task_ack_samples','task_ack_p95_ms',0)):
            path, digest = self.add(kind+'.json', {'source_commit':self.commit,'environment_manifest_sha256':self.report['environment_manifest_sha256'], 'warmup_count':warmup,'samples_ms':[self.report['metrics'][metric]]*self.spec['fixtures'][count_key]})
            self.report['raw_samples_paths'][kind] = path
            self.report['raw_samples_sha256'][kind] = digest
        for kind, data in (
            ('faults',[{'case_id':f'F{i:02}','run_id':j,'source_commit':self.commit,'passed':True} for i in range(1,23) for j in range(1,11)]),
            ('representatives',[{'case_id':f'BP-{i:02}','source_commit':self.commit,'passed':True} for i in range(1,21)])):
            path, digest = self.add(kind+'.json', data)
            self.report['raw_samples_paths'][kind] = path
            self.report['raw_samples_sha256'][kind] = digest
        for i in range(1,6):
            path, digest = self.add(f'ext{i}.json', {'synthetic_diff':True})
            self.report['extension_records'].append({'id':f'EXT-{i:02}','baseline_commit':'b'*40,'extension_commit':str(i)*40,'diff_path':path,'diff_sha256':digest})

    def tearDown(self):
        self.temp.cleanup()

    def add(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data))
        digest = checker.sha(path)
        if name not in self.report['evidence_paths']:
            self.report['evidence_paths'].append(name)
        self.report['evidence_sha256'][name] = digest
        return name, digest

    def validate(self):
        return checker.validate_report(self.report, self.spec, self.root, self.commit, checker.sha(checker.TARGETS))

    def test_consistent_synthetic_report(self):
        self.assertIn('PASS', self.validate())

    def test_fractional_latency_and_ratio(self):
        self.report['metrics']['rotation_frame_p95_ms'] = 32.5
        self.report['metrics']['inventory_mapping_ratio'] = 1.0
        raw = checker.read_json(self.root / 'rotation.json')
        raw['samples_ms'] = [32.5] * 300
        _, digest = self.add('rotation.json', raw)
        self.report['raw_samples_sha256']['rotation'] = digest
        self.assertIn('PASS', self.validate())

    def test_missing_metric(self):
        self.report['metrics'].pop('partial_commit_count')
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_boolean_not_an_integer_measurement(self):
        self.report['metrics']['partial_commit_count'] = False
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_one_failure_is_not_accepted(self):
        self.report['metrics']['partial_commit_count'] = 1
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_raw_percentile_cannot_be_claimed_lower(self):
        self.report['metrics']['rotation_frame_p95_ms'] = 1
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_empty_legacy_denominator(self):
        name, digest = self.add('fixture.json', {'legacy_fixtures':[], 'supported_legacy_versions':[]})
        self.report['fixture_sha256'] = digest
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_missing_legacy_version(self):
        fixture = checker.read_json(self.root/'fixture.json')
        fixture['supported_legacy_versions'].append('workspace-v2')
        _, digest = self.add('fixture.json', fixture)
        self.report['fixture_sha256'] = digest
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_duplicate_fault_run(self):
        faults = checker.read_json(self.root/'faults.json')
        faults[-1] = faults[0]
        _, digest = self.add('faults.json', faults)
        self.report['raw_samples_sha256']['faults'] = digest
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_tampered_artifact(self):
        (self.root/'rotation.json').write_text('{}')
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_traversal(self):
        self.report['evidence_paths'].append('../outside.json')
        self.report['evidence_sha256']['../outside.json'] = '0'*64
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_wrong_release_commit(self):
        self.report['source_commit'] = 'c'*40
        with self.assertRaises(checker.EvidenceError): self.validate()

    def test_target_spec_is_not_a_report(self):
        with self.assertRaises(checker.EvidenceError): checker.validate_report(self.spec, self.spec, self.root, self.commit, checker.sha(checker.TARGETS))

    def test_nearest_rank_p95(self):
        self.assertEqual(checker.samples_p95(list(range(1,101)),100),95)
        with self.assertRaises(checker.EvidenceError): checker.samples_p95([1,2],100)

    def test_duplicate_json_key(self):
        path=self.root/'duplicate.json';path.write_text('{"a":1,"a":2}')
        with self.assertRaises(checker.EvidenceError): checker.read_json(path)

if __name__ == '__main__': unittest.main()
