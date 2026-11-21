#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Public synthetic extraction ingestion tests; not real-program acceptance."""
import hashlib
import base64
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'tools/direct_prepare.py'


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'extract'
        (self.root / 'pkg').mkdir(parents=True)
        (self.root / 'split').mkdir()
        (self.root / 'ok').write_text('split\t1\n')
        self.ctl = bytearray(0x880)
        struct.pack_into('<I', self.ctl, 0x20, 0x880)
        self.code = b'public synthetic shader bytes!!!'
        self.key = hashlib.sha256(self.ctl + self.code).hexdigest()[:16]
        header = bytearray(b'BTCDBTCD' + bytes(0x30))
        struct.pack_into('<II', header, 0x28, len(self.ctl), len(self.code))
        self.record = header + self.code + self.ctl
        (self.root / 'pkg/vs.bin').write_bytes(self.record)
        self.map = self.root / 'split/shader_map.tsv'
        self.row = f'vs.bin\t0\tvertex\t{self.key}\tfixture#BNSH@0x0#var0\n'
        self.map.write_text('package\tindex\tstage\tdm_ctl_sha16\tsource\n' + self.row)
        (self.root / 'split/shader_pairs.tsv').write_text('# evidence\n')
        (self.root / 'split/shader_facts.tsv').write_text('# facts\n')

    def run_cli(self, report=None):
        self.report = report or Path(self.tmp.name) / 'report.json'
        return subprocess.run([sys.executable, str(TOOL), '--extract-root', str(self.root),
                               '--report', str(self.report)], capture_output=True, text=True)

    def test_all_deferred_no_program(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 2, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual(report['requests'], 0)
        self.assertEqual(report['deferred'], 1)
        self.assertIn('programMembership', report['entries'][0]['missing'])
        self.assertEqual(report['entries'][0]['source']['codeBytes'], len(self.code))
        self.assertFalse(list(Path(self.tmp.name).rglob('programs.v1.json')))

    def test_wrong_key(self):
        self.map.write_text(self.row.replace(self.key, '0' * 16))
        self.assertEqual(self.run_cli().returncode, 1)
        self.assertFalse(self.report.exists())

    def test_negative_index(self):
        self.map.write_text(self.row.replace('\t0\t', '\t-1\t'))
        self.assertEqual(self.run_cli().returncode, 1)

    def test_truncated_record(self):
        (self.root / 'pkg/vs.bin').write_bytes(self.record[:-1])
        self.assertEqual(self.run_cli().returncode, 1)

    def test_duplicate_row(self):
        self.map.write_text(self.row * 2)
        self.assertEqual(self.run_cli().returncode, 1)

    def test_escape(self):
        self.map.write_text(self.row.replace('vs.bin', '../../outside'))
        self.assertEqual(self.run_cli().returncode, 1)

    def test_symlink(self):
        p = self.root / 'pkg/vs.bin'
        p.unlink()
        p.symlink_to(Path(self.tmp.name) / 'outside')
        self.assertEqual(self.run_cli().returncode, 1)

    def test_duplicate_fact_keys(self):
        (self.root / 'split/shader_facts.tsv').write_text(self.key + '\t{"local":[],"local":[1,1,1]}\n')
        self.assertEqual(self.run_cli().returncode, 1)

    def test_nonfinite_fact_numbers_rejected(self):
        # Exponent overflow is also nonfinite, although parse_constant does not
        # see it: Python's float conversion yields +/-inf for valid JSON tokens.
        for i, number in enumerate(('NaN', 'Infinity', '-Infinity', '1e400', '-1e400')):
            with self.subTest(number=number):
                (self.root / 'split/shader_facts.tsv').write_text(
                    self.key + '\t{"nested":[{"value":' + number + '}]}\n')
                target = Path(self.tmp.name) / f'nonfinite-{i}.json'
                result = self.run_cli(target)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertFalse(target.exists())
                self.assertFalse(list(Path(self.tmp.name).glob('.direct-prepare-*')))
                error = json.loads(result.stderr)
                self.assertEqual(error['error'], 'INVALID_INPUT_OR_REPORT')
                self.assertIn('nonfinite', error['message'])

    def test_finite_fact_numbers_remain_deferred(self):
        # Do not reject large finite values or change valid input into requests.
        (self.root / 'split/shader_facts.tsv').write_text(
            self.key + '\t{"values":[1.7976931348623157e308,-1e-300,0.0,123]}\n')
        result = self.run_cli()
        self.assertEqual(result.returncode, 2, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual(report['requests'], 0)
        self.assertEqual(report['entries'][0]['status'], 'deferred')

    def test_excessive_fact_nesting_structured_failure(self):
        # Small public input, below file limits, exceeding the JSON parser's
        # recursion budget. No report and no traceback / partial publication.
        nested = '{"value":' + '[' * 20000 + '0' + ']' * 20000 + '}'
        (self.root / 'split/shader_facts.tsv').write_text(self.key + '\t' + nested + '\n')
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.report.exists())
        error = json.loads(result.stderr)
        self.assertEqual(error['error'], 'INVALID_INPUT_OR_REPORT')
        self.assertIn('nesting', error['message'])
        self.assertFalse(list(Path(self.tmp.name).glob('.direct-prepare-*')))

    def test_existing_report(self):
        target = Path(self.tmp.name) / 'report.json'
        target.write_text('keep')
        self.assertEqual(self.run_cli(target).returncode, 1)
        self.assertEqual(target.read_text(), 'keep')

    def test_missing_success_marker(self):
        (self.root / 'ok').unlink()
        self.assertEqual(self.run_cli().returncode, 1)

    def test_deterministic(self):
        self.assertEqual(self.run_cli().returncode, 2)
        first = self.report.read_bytes()
        self.assertEqual(self.run_cli(Path(self.tmp.name) / 'second.json').returncode, 2)
        self.assertEqual(first, self.report.read_bytes())

    def test_cover_and_manual_defaults_never_prove_state(self):
        # Second synthetic stage; cover TSV is structurally valid, not proof.
        code = self.code + b'fs'
        key = hashlib.sha256(self.ctl + code).hexdigest()[:16]
        header = bytearray(b'BTCDBTCD' + bytes(0x30))
        struct.pack_into('<II', header, 0x28, len(self.ctl), len(code))
        (self.root / 'pkg/fs.bin').write_bytes(header + code + self.ctl)
        self.map.write_text(self.row + f'fs.bin\t0\tpixel\t{key}\tfixture#UE0\n')
        (self.root / 'split/shader_pairs.tsv').write_text(self.key + '\t' + key + '\n')
        (self.root / 'split/shader_facts.tsv').write_text(
            self.key + '\t{"textures":[],"textureArrayLength":1,"samplerArrayLength":1}\n')
        self.assertEqual(self.run_cli().returncode, 2)
        report = json.loads(self.report.read_text())
        self.assertEqual(report['candidatePairs'], 1)
        self.assertEqual(report['provenPairs'], 0)
        self.assertEqual(report['requests'], 0)
        self.assertEqual(report['deferred'], 2)
        for entry in report['entries']:
            self.assertIn('textures', entry['missing'])
            self.assertIn('programMembership', entry['missing'])

    def test_report_inside_input_rejected(self):
        self.assertEqual(self.run_cli(self.root / 'report.json').returncode, 1)
        self.assertFalse(self.report.exists())

    def test_runtime_control_window_boundaries(self):
        # hle_nvn.c prog_control_len: own in [0x760,0x880), otherwise
        # 0x880. Raw record length is NOT the runtime captured control length.
        for own, captured in ((0x75f, 0x880), (0x760, 0x760),
                              (0x87f, 0x87f), (0x880, 0x880), (0x88c, 0x880)):
            with self.subTest(own=own):
                control = bytearray(0x88c)
                struct.pack_into('<I', control, 0x20, own)
                control[-12:] = b'rawtailbytes'
                key = hashlib.sha256(control[:captured] + self.code).hexdigest()[:16]
                header = bytearray(b'BTCDBTCD' + bytes(0x30))
                struct.pack_into('<II', header, 0x28, len(control), len(self.code))
                (self.root / 'pkg/vs.bin').write_bytes(header + self.code + control)
                self.map.write_text(self.row.replace(self.key, key))
                self.assertEqual(self.run_cli(Path(self.tmp.name) / f'{own}.json').returncode, 2)
                source = json.loads(self.report.read_text())['entries'][0]['source']
                # Current report describes raw extraction, not runtime identity.
                self.assertEqual(source['controlBytes'], 0x88c)
                self.assertEqual(source['controlSha256'], hashlib.sha256(control).hexdigest())
                self.assertNotEqual(source['controlSha256'], hashlib.sha256(control[:captured]).hexdigest())

    def test_extraction_trailing_control_is_not_in_runtime_key(self):
        def emit(tail, name):
            control = self.ctl + tail
            header = bytearray(b'BTCDBTCD' + bytes(0x30))
            struct.pack_into('<II', header, 0x28, len(control), len(self.code))
            (self.root / 'pkg/vs.bin').write_bytes(header + self.code + control)
            self.assertEqual(self.run_cli(Path(self.tmp.name) / name).returncode, 2)
            return json.loads(self.report.read_text())['entries'][0]
        a = emit(b'first tail!!', 'tail-a.json')
        b = emit(b'other tail!!', 'tail-b.json')
        self.assertEqual(a['extractKey'], b['extractKey'])
        self.assertNotEqual(a['source']['controlSha256'], b['source']['controlSha256'])
        self.assertNotIn('identity', a)

    def test_dangling_pair(self):
        (self.root / 'split/shader_pairs.tsv').write_text(self.key + '\t' + '0' * 16 + '\n')
        self.assertEqual(self.run_cli().returncode, 1)



class CandidateTests(unittest.TestCase):
    setUp = PrepareTests.setUp

    def candidate(self, *, bad_control=False, bad_c1=False, mismatch=False, local_memory=0):
        # Public EXIT/BRA synthetic CS also used by the existing compiler tests.
        self.code = bytes.fromhex('e00700fc00801f000f000700000000e3000f07000000b0500f0007ffff0f40e2')
        self.ctl[:4] = bytes.fromhex('34127698')
        struct.pack_into('<3I', self.ctl, 0x748, 0 if bad_control else 2, 3, 4)
        struct.pack_into('<I', self.ctl, 0x754, 17)
        struct.pack_into('<I', self.ctl, 0x758, local_memory)
        struct.pack_into('<II', self.ctl, 0x700, 999999 if bad_c1 else len(self.code), len(self.code))
        self.key = hashlib.sha256(self.ctl + self.code).hexdigest()[:16]
        header = bytearray(b'BTCDBTCD' + bytes(0x30))
        struct.pack_into('<II', header, 0x28, len(self.ctl), len(self.code))
        (self.root / 'pkg/vs.bin').write_bytes(header + self.code + self.ctl)
        self.map.write_text(f'vs.bin\t0\tcompute\t{self.key}\tpublic-fixture\n')
        self.profile = Path(self.tmp.name) / 'profile.json'
        self.identity = Path(self.tmp.name) / 'identity.json'
        self.output = Path(self.tmp.name) / 'candidates'
        self.report = Path(self.tmp.name) / 'candidate-report.json'
        closure = 's2a-dc1-' + 'a' * 64
        self.profile.write_text(json.dumps({'schema': 's2a-direct-profile-v1',
            'profileHash': 'sha256-' + 'b' * 64, 'directABI': 1,
            'translatorClosure': closure, 'runtimeCompilerABI': 's2a-direct-compile-v1',
            'supportLayout': {'hash': 'sha256-' + 'c' * 64}}))
        self.identity.write_text(json.dumps({'schema': 's2a-direct-identity-v1',
            'translatorClosure': ('s2a-dc1-' + 'd' * 64) if mismatch else closure,
            'compilerABI': 's2a-direct-compile-v1', 'directABI': 1,
            'supportLayout': 'sha256-' + 'c' * 64}))
        return [sys.executable, str(TOOL), '--extract-root', str(self.root),
                '--report', str(self.report), '--profile', str(self.profile),
                '--identity', str(self.identity), '--candidate-dir', str(self.output)]

    def test_compute_candidate_explicit_assumptions_not_library(self):
        result = subprocess.run(self.candidate(), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual((report['requests'], report['deferred']), (2, 0))
        entry = report['entries'][0]
        self.assertEqual(entry['status'], 'enumerated-candidate')
        self.assertFalse(entry['runtimeIdentityProven'])
        req = json.loads((self.output / entry['candidateRequest']).read_text())
        self.assertEqual(req['program']['computeState'],
                         {'localSize': [2, 3, 4], 'sharedMemoryBytes': 32, 'localMemoryBytes': 0, 'hasUnalignedStorageBuffer': False})
        self.assertEqual(req['program']['textures'], [])
        self.assertEqual(req['program']['textureArrayLength'], 1)
        self.assertEqual(req['program']['samplerArrayLength'], 1)
        self.assertEqual(req['program']['stages'][0]['c1'], '')
        self.assertNotIn('graphicsState', req['program'])
        self.assertNotIn('identity', entry)
        self.assertNotIn('assumptions', req)
        self.assertNotIn('computeState.localMemoryBytes', entry['assumptions'])
        self.assertNotIn('computeState.hasUnalignedStorageBuffer', entry['assumptions'])
        self.assertEqual(entry['compilerValidation'], 'pending')
        self.assertFalse(list(self.output.glob('*.program')))
        self.assertFalse(list(self.output.glob('*manifest*')))

    def test_compute_local_memory_is_control_fact(self):
        result = subprocess.run(self.candidate(local_memory=32), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        entry = json.loads(self.report.read_text())['entries'][0]
        req = json.loads((self.output / entry['candidateRequest']).read_text())
        self.assertEqual(req['program']['computeState']['localMemoryBytes'], 32)
        self.assertNotIn('computeState.localMemoryBytes', entry['assumptions'])
        self.assertEqual(entry['staticSources']['computeState.localMemoryBytes'], 'control+0x758:u32le')

    def test_compute_local_memory_out_of_range_deferred(self):
        result = subprocess.run(self.candidate(local_memory=0x100001), capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.output.exists())

    def test_invalid_compute_remains_deferred(self):
        result = subprocess.run(self.candidate(bad_control=True), capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(json.loads(self.report.read_text())['requests'], 0)
        self.assertFalse(self.output.exists())

    def test_invalid_c1_range_remains_deferred(self):
        result = subprocess.run(self.candidate(bad_c1=True), capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.output.exists())

    def test_profile_identity_mismatch_no_publish(self):
        result = subprocess.run(self.candidate(mismatch=True), capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.report.exists())
        self.assertFalse(self.output.exists())

    def test_candidate_directory_no_clobber(self):
        cmd = self.candidate()
        self.output.mkdir()
        (self.output / 'keep').write_text('existing')
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual((self.output / 'keep').read_text(), 'existing')
        self.assertFalse(self.report.exists())

    def test_report_collision_does_not_leave_candidates(self):
        cmd = self.candidate()
        self.report.write_text('existing')
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.report.read_text(), 'existing')
        self.assertFalse(self.output.exists())

    def test_candidate_bytes_preserved_and_c1_selected(self):
        cmd = self.candidate()
        struct.pack_into('<II', self.ctl, 0x700, 24, len(self.code))
        key = hashlib.sha256(self.ctl + self.code).hexdigest()[:16]
        raw = bytearray((self.root / 'pkg/vs.bin').read_bytes())
        raw[-len(self.ctl):] = self.ctl
        (self.root / 'pkg/vs.bin').write_bytes(raw)
        self.map.write_text(f'vs.bin\t0\tcompute\t{key}\tpublic-fixture\n')
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        req = json.loads((self.output / (key + '.unaligned-false.request.json')).read_text())
        stage = req['program']['stages'][0]
        self.assertEqual(base64.b64decode(stage['code']), self.code)
        self.assertEqual(base64.b64decode(stage['control']), self.ctl)
        self.assertEqual(base64.b64decode(stage['c1']), bytes.fromhex('0f0007ffff0f40e2'))

    def test_candidate_symlink_directory_rejected(self):
        cmd = self.candidate()
        self.output.symlink_to(self.root, target_is_directory=True)
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.report.exists())
        self.assertFalse(list(self.root.glob('*.request.json')))

    def test_partial_candidate_options_rejected(self):
        cmd = self.candidate()[:-2]
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.report.exists())

    def test_invalid_target_schema_rejected(self):
        cmd = self.candidate()
        self.profile.write_text('{"schema":"not-a-profile"}')
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.report.exists())

    def test_nonfinite_target_no_publish(self):
        cmd = self.candidate()
        self.profile.write_text(self.profile.read_text()[:-1] + ', "unknown": 1e400}')
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.report.exists())
        self.assertFalse(self.output.exists())

    def test_extraction_control_magic_and_code_extent(self):
        for offset, value in ((0, 0), (0x704, 33)):
            with self.subTest(offset=offset):
                cmd = self.candidate()
                self.report = Path(self.tmp.name) / f'report-{offset}.json'
                self.output = Path(self.tmp.name) / f'candidates-{offset}'
                cmd[cmd.index('--report') + 1] = str(self.report)
                cmd[cmd.index('--candidate-dir') + 1] = str(self.output)
                struct.pack_into('<I', self.ctl, offset, value)
                key = hashlib.sha256(self.ctl + self.code).hexdigest()[:16]
                raw = bytearray((self.root / 'pkg/vs.bin').read_bytes())
                raw[-len(self.ctl):] = self.ctl
                (self.root / 'pkg/vs.bin').write_bytes(raw)
                self.map.write_text(f'vs.bin\t0\tcompute\t{key}\tpublic-fixture\n')
                result = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(json.loads(self.report.read_text())['requests'], 0)
                self.report.unlink()
                self.assertFalse(self.output.exists())

    def test_target_support_layout_mismatch(self):
        cmd = self.candidate()
        identity = json.loads(self.identity.read_text())
        identity['supportLayout'] = 'sha256-' + 'd' * 64
        self.identity.write_text(json.dumps(identity))
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.report.exists())
        self.assertFalse(self.output.exists())


class GraphicsSourceAuditTests(unittest.TestCase):
    """Static source windows and unknown draw facts, never a graphics REQUEST."""
    setUp = PrepareTests.setUp
    run_cli = PrepareTests.run_cli

    def put_source(self, code, control=None, stage='vertex'):
        control = self.ctl if control is None else control
        own = struct.unpack_from('<I', control, 0x20)[0]
        own = own if 0x760 <= own < 0x880 else 0x880
        key = hashlib.sha256(control[:own] + code).hexdigest()[:16]
        header = bytearray(b'BTCDBTCD' + bytes(0x30))
        struct.pack_into('<II', header, 0x28, len(control), len(code))
        (self.root / 'pkg/vs.bin').write_bytes(header + code + control)
        self.map.write_text(f'vs.bin\t0\t{stage}\t{key}\tpublic-source-window\n')

    def audit(self, suffix=''):
        result = self.run_cli(Path(self.tmp.name) / ('source-audit' + suffix + '.json'))
        self.assertEqual(result.returncode, 2, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual((report['requests'], report['provenPairs'], report['deferred']), (0, 0, 1))
        entry = report['entries'][0]
        self.assertEqual(entry['status'], 'deferred')
        self.assertNotIn('identity', entry)
        self.assertFalse(list(Path(self.tmp.name).rglob('*.request.json')))
        return entry

    def source_window(self, suffix=''):
        entry = self.audit(suffix)
        self.assertIn('requestSource', entry['source'])
        window = entry['source']['requestSource']
        self.assertEqual(window['scope'], 'static-source-window; not runtime identity or capture proof')
        return entry, window

    def test_map_source_label_is_audited_not_program_membership(self):
        entry = self.audit()
        self.assertEqual(entry['source'].get('sourceLabel'), 'fixture#BNSH@0x0#var0')
        self.assertIn('programMembership', entry['missing'])

    def test_graphics_missing_paths_cover_current_contract(self):
        # Independent list from DirectGraphicsFacts.Keys; do not import the
        # producer's list as the expected result. No values are invented.
        fields = set('earlyZForce topology tessCw tessPatchType tessSpacing '
            'alphaToCoverageEnable alphaToCoverageDitherEnable viewportTransformDisable depthMode '
            'programPointSizeEnable pointSize alphaTestCompare alphaTestReference attributeTypes '
            'hasConstantBufferDrawParameters fragmentOutputTypes dualSourceBlendEnable '
            'yNegateEnabled originUpperLeft halvePrimitiveId hasUnalignedStorageBuffer'.split())
        entry = self.audit()
        actual = {p[len('graphicsState.'):] for p in entry['missing'] if p.startswith('graphicsState.')}
        self.assertEqual(actual, fields)
        self.assertNotIn('graphicsState', entry['missing'])
        self.assertIn('programMembership', entry['missing'])
        self.assertNotIn('textureBufferLengths', entry['missing'])
        self.assertEqual(entry.get('conditionalMissing'),
                         {'textureBufferLengths': 'required only for slots queried by translation; no static source'})
        self.assertIn('targetProfile', entry['missing'])
        self.assertIn('compilerIdentity', entry['missing'])

    def test_supplied_target_not_reported_missing_for_graphics(self):
        cmd = CandidateTests.candidate(self)
        self.map.write_text(self.map.read_text().replace('\tcompute\t', '\tvertex\t'))
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        report = json.loads(self.report.read_text())
        entry = report['entries'][0]
        self.assertNotIn('targetProfile', entry['missing'])
        self.assertNotIn('compilerIdentity', entry['missing'])
        self.assertIn('programMembership', entry['missing'])
        self.assertEqual(report['requests'], 0)
        self.assertFalse(self.output.exists())

    def test_graphics_strip_only_nvn_header_keep_maxwell_header(self):
        nvn = bytes.fromhex('78563412') + b'N' * 44
        sph = bytes(range(80))
        instructions = bytes.fromhex('0f0007ffff0f40e2') * 2
        code = nvn + sph + instructions
        self.put_source(code, stage='pixel')
        entry, window = self.source_window()
        self.assertEqual(entry['source']['codeSha256'], hashlib.sha256(code).hexdigest())
        self.assertEqual(entry['source']['codeBytes'], 144)
        self.assertEqual(window['stage'], 'fs')
        self.assertEqual(window['code'], {'offset': 48, 'bytes': 96,
                         'sha256': hashlib.sha256(sph + instructions).hexdigest()})

    def test_graphics_nonmatching_and_boundary_headers_are_not_stripped(self):
        for i, code in enumerate((bytes.fromhex('79563412') + b'C' * 140,
                                  bytes.fromhex('78563412') + b'N' * 44,
                                  b'raw-headerless-graphics-source!!!')):
            with self.subTest(case=i):
                self.put_source(code)
                _, window = self.source_window(str(i))
                self.assertEqual(window['code'], {'offset': 0, 'bytes': len(code),
                                 'sha256': hashlib.sha256(code).hexdigest()})

    def test_c1_range_uses_raw_code_before_header_strip(self):
        code = bytes.fromhex('78563412') + b'N' * 44 + bytes(range(96))
        control = bytearray(self.ctl)
        control[:4] = bytes.fromhex('34127698')
        struct.pack_into('<II', control, 0x700, len(code) - 4, len(code) + 4)
        self.put_source(code, control)
        _, window = self.source_window()
        self.assertEqual(window['c1'], {'status': 'available',
            'range': [140, 148], 'bytes': 8,
            'sha256': hashlib.sha256(bytes([92, 93, 94, 95]) + bytes.fromhex('34127698')).hexdigest()})

    def test_uncaptured_control_tail_cannot_supply_c1(self):
        code = b'0123456789abcdef'
        control = bytearray(self.ctl) + b'TAIL'
        struct.pack_into('<II', control, 0x700, len(code) + 0x880, len(code) + 0x884)
        self.put_source(code, control)
        entry, window = self.source_window()
        self.assertEqual(entry['source']['controlBytes'], 0x884)
        self.assertEqual(window['control'], {'bytes': 0x880, 'uncapturedTailBytes': 4,
            'sha256': hashlib.sha256(control[:0x880]).hexdigest()})
        self.assertEqual(window['c1']['status'], 'unavailable')
        self.assertEqual(window['c1']['reason'], 'outside-captured-source')
        self.assertNotIn('sha256', window['c1'])

    def test_control_own_size_boundaries_and_c1_wholly_in_control(self):
        code = b'0123456789abcdef'
        for own, captured in ((0x75f, 0x880), (0x760, 0x760),
                              (0x87f, 0x87f), (0x880, 0x880)):
            with self.subTest(own=own):
                control = bytearray(self.ctl) + b'TAIL'
                struct.pack_into('<I', control, 0x20, own)
                control[0x710:0x714] = b'C1OK'
                begin = len(code) + 0x710
                struct.pack_into('<II', control, 0x700, begin, begin + 4)
                self.put_source(code, control)
                _, window = self.source_window(str(own))
                self.assertEqual(window['control'], {'bytes': captured,
                    'uncapturedTailBytes': len(control) - captured,
                    'sha256': hashlib.sha256(control[:captured]).hexdigest()})
                self.assertEqual(window['c1'], {'status': 'available',
                    'range': [begin, begin + 4], 'bytes': 4,
                    'sha256': hashlib.sha256(b'C1OK').hexdigest()})

    def test_c1_exact_64k_and_source_end_boundaries_are_available(self):
        code = bytes(range(256)) * 273
        for begin, end in ((0, 65536), (len(code) + 0x87c, len(code) + 0x880)):
            with self.subTest(begin=begin, end=end):
                control = bytearray(self.ctl)
                control[-4:] = b'END!'
                struct.pack_into('<II', control, 0x700, begin, end)
                self.put_source(code, control)
                _, window = self.source_window(str(begin))
                expected = code[:65536] if begin == 0 else b'END!'
                self.assertEqual(window['c1'], {'status': 'available', 'range': [begin, end],
                    'bytes': len(expected), 'sha256': hashlib.sha256(expected).hexdigest()})

    def test_invalid_c1_ranges_remain_unknown_not_zero_filled(self):
        code = b'A' * 70000
        for i, (begin, end, reason) in enumerate(((4, 4, 'empty-or-reversed'),
                (8, 4, 'empty-or-reversed'), (0, 65537, 'exceeds-64KiB'),
                (0, len(code) + 0x881, 'outside-captured-source'))):
            with self.subTest(begin=begin, end=end):
                control = bytearray(self.ctl)
                struct.pack_into('<II', control, 0x700, begin, end)
                self.put_source(code, control)
                _, window = self.source_window(str(i))
                self.assertEqual(window['c1'], {'status': 'unavailable', 'range': [begin, end],
                                              'reason': reason})


class NativeIdentityTests(unittest.TestCase):
    setUp = PrepareTests.setUp
    candidate = CandidateTests.candidate
    # Reuse only fixture setup, not all inherited candidate tests.
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.encoder = Path(cls.build.name) / 'identity-host'
        android = TOOL.parents[1]
        subprocess.run(['bash', str(android / 'tools/build_direct_identity_host.sh'),
                        str(cls.encoder)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def encoded_candidate(self):
        return self.candidate() + ['--identity-encoder', str(self.encoder)]

    def test_alignment_enumeration_distinct_identities(self):
        result = subprocess.run(self.encoded_candidate(), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual((report['requests'], report['deferred']), (2, 0))
        entries = report['entries']
        self.assertEqual(len(entries), 2)
        values = []
        for entry in entries:
            request = json.loads((self.output / entry['candidateRequest']).read_text())
            value = request['program']['computeState']['hasUnalignedStorageBuffer']
            values.append(value)
            self.assertEqual(entry['enumeratedFacts'],
                             {'computeState.hasUnalignedStorageBuffer': value})
            self.assertEqual(entry['enumerationScope'], 'pack-time exhaustive enumeration')
            self.assertNotIn('computeState.hasUnalignedStorageBuffer', entry['assumptions'])
        self.assertEqual(values, [False, True])
        self.assertNotEqual(entries[0]['identity'], entries[1]['identity'])
        self.assertNotEqual(entries[0]['canonical'], entries[1]['canonical'])
        manifest = json.loads((self.output / 'programs.v1.json').read_text())
        self.assertEqual(len(manifest['entries']), 2)
        self.assertEqual({e['identity'] for e in manifest['entries']},
                         {e['identity'] for e in entries})

    def test_native_manifest_and_canonical(self):
        result = subprocess.run(self.encoded_candidate(), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.report.read_text())
        entry = report['entries'][0]
        manifest = json.loads((self.output / 'programs.v1.json').read_text())
        self.assertEqual(manifest['entries'], [{'identity': e['identity'], 'request': e['candidateRequest']}
                                               for e in report['entries']])
        self.assertEqual(hashlib.sha256(bytes.fromhex(entry['canonical'])).hexdigest(), entry['identity'])
        self.assertFalse(entry['runtimeIdentityProven'])
        self.assertEqual(entry['candidateIdentityFacts']['stage_mask'], 32)
        self.assertEqual(entry['candidateIdentityFacts']['stages'][0]['source_sha'],
                         hashlib.sha256(self.ctl + self.code).hexdigest())
        sys.path.insert(0, str(TOOL.parent))
        import direct_batch
        self.assertEqual(len(direct_batch.load_programs(self.output / 'programs.v1.json')['entries']), 2)

    def test_encoder_failure_no_partial_outputs(self):
        cmd = self.encoded_candidate()
        cmd[-1] = str(Path(self.tmp.name) / 'missing-encoder')
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.report.exists())

    def test_native_control_tail_refused_without_publication(self):
        cmd = self.encoded_candidate()
        path = self.root / 'pkg/vs.bin'
        raw = bytearray(path.read_bytes())
        struct.pack_into('<I', raw, 0x28, len(self.ctl) + 4)
        path.write_bytes(raw + b'tail')
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('exact captured control length', result.stderr)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.report.exists())

    def test_malformed_encoder_output_and_timeout_no_publication(self):
        cmd = self.encoded_candidate()
        sys.path.insert(0, str(TOOL.parent))
        import direct_prepare
        from unittest.mock import patch
        for outcome in (subprocess.CompletedProcess([], 0, b'not JSON', b''),
                        subprocess.CompletedProcess([], 0, b'null', b''),
                        subprocess.CompletedProcess([], 0, b'{"identity":"bad","canonical":"00"}', b''),
                        subprocess.TimeoutExpired('native encoder', 30)):
            with self.subTest(outcome=type(outcome).__name__):
                options = {'side_effect': outcome} if isinstance(outcome, Exception) else {'return_value': outcome}
                with patch.object(direct_prepare.subprocess, 'run', **options):
                    self.assertEqual(direct_prepare.main(cmd[2:]), 1)
                self.assertFalse(self.output.exists())
                self.assertFalse(self.report.exists())

    def test_native_parity_existing_runtime_test_fixture(self):
        # Reuse the pre-existing C runtime encoder regression's independent
        # concrete input, not an expected value derived from this wrapper.
        android = TOOL.parents[1]
        source = (android / 'tests/direct_input_identity_test.c').read_text()
        binary_out = Path(self.tmp.name) / 'runtime.bin'
        source = source.replace('return fails ? 1 : 0;',
            'FILE *out=fopen("' + str(binary_out) + '","wb"); '
            'if(!out) return 2; fwrite(base,1,n,out); fclose(out); return fails ? 1 : 0;')
        fixture = Path(self.tmp.name) / 'runtime.c'
        fixture.write_text(source)
        exe = Path(self.tmp.name) / 'runtime'
        repo = android.parents[0]
        import os
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I' + str(android / 'native'), '-I' + str(repo / 'runtime/include'),
            '-I' + str(repo / 'graphics/nvn/gfx/nvn/include'), str(fixture),
            str(android / 'native/direct/input_identity.c'), str(android / 'native/direct/texture_facts.c'),
            str(android / 'native/direct/sha256.c'), '-o', str(exe)], check=True, capture_output=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        sys.path.insert(0, str(TOOL.parent))
        import direct_prepare
        g = dict.fromkeys(direct_prepare.GRAPHICS_INTS, 0)
        g.update(rt_count=2, topology='Triangles', has_tess_mode=1, tess_mode=1,
                 fragment_output_types=['Float']*2 + [None]*6,
                 attribute_types=['Float']*3 + [None]*29, alpha_test_compare='Always',
                 point_size=1.0, alpha_test_reference=0.5)
        for k in ('has_alpha_to_coverage alpha_to_coverage_enable alpha_to_coverage_dither '
                  'has_alpha_test has_blend has_depth_mode depth_mode has_y_negate has_origin '
                  'origin_upper_left has_halve_primitive_id has_cb_draw_params cb_draw_params '
                  'has_point_size has_alpha_test_reference has_unaligned_ssbo has_geometry_shader '
                  'host_supports_quads').split():
            g[k] = 1
        facts = dict(direct_abi=5, translator_closure='closure-5',
            profile_sha=bytes(i*3+1 for i in range(32)).hex(), stage_mask=3, source_present=3,
            keys=[bytes(0x10*j+i for i in range(8)).hex() for j in range(2)] + ['0'*16]*4,
            stages=[dict(source_sha=bytes((0x40*j+i*7+1)&255 for i in range(32)).hex(),
                         code_bytes=1024+j*512, control_bytes=0x880) for j in range(2)],
            ubo_use=[0,31,0,0,0,0], texture_array_length=10256, sampler_array_length=1256,
            transform_feedback_enabled=0, gfx=g, textures=[
                dict(stage='fs',handle=8,cbuf_slot=-1,sampler_type='2d',format='R8G8B8A8Unorm',
                     is_buffer=0,normalized=1,separate=0,array_length=1),
                dict(stage='fs',handle=10,cbuf_slot=-1,sampler_type='2d-array',format='R16G16B16A16Float',
                     is_buffer=0,normalized=1,separate=0,array_length=4)])
        def encode(value):
            return subprocess.run([str(self.encoder)], input=json.dumps(value), capture_output=True, text=True)
        encoded = encode(facts)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        output = json.loads(encoded.stdout)
        self.assertEqual(bytes.fromhex(output['canonical'])[5], 5, 'pair must use v5')
        self.assertEqual(bytes.fromhex(output['canonical']), binary_out.read_bytes())
        self.assertEqual(output['identity'], hashlib.sha256(binary_out.read_bytes()).hexdigest())
        # Repeat parity with wide cbuf usage: paired policy remains v5, not v4.
        wide_source = source.replace('in.ubo_use[0] = 0;', 'in.ubo_use[0] = 0x38u;')
        self.assertNotEqual(wide_source, source, 'wide fixture anchor must match')
        fixture.write_text(wide_source)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I' + str(android / 'native'), '-I' + str(repo / 'runtime/include'),
            '-I' + str(repo / 'graphics/nvn/gfx/nvn/include'), str(fixture),
            str(android / 'native/direct/input_identity.c'), str(android / 'native/direct/texture_facts.c'),
            str(android / 'native/direct/sha256.c'), '-o', str(exe)], check=True, capture_output=True)
        wide_run = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(wide_run.returncode, 0, wide_run.stderr + wide_run.stdout)
        facts['ubo_use'][0] = 0x38
        wide_encoded = encode(facts)
        self.assertEqual(wide_encoded.returncode, 0, wide_encoded.stderr)
        wide_output = json.loads(wide_encoded.stdout)
        self.assertEqual(bytes.fromhex(wide_output['canonical'])[5], 5)
        self.assertEqual(bytes.fromhex(wide_output['canonical']), binary_out.read_bytes())
        facts['ubo_use'][0] = 0
        # Repeated input and semantic variations exercise the transport mapping.
        self.assertEqual(encode(facts).stdout, encoded.stdout)
        for group, name, value in [(facts,'texture_array_length',10257),
                                   (g,'point_size',2.0), (g,'alpha_test_compare',None),
                                   (facts['textures'][0],'format','R16G16B16A16Float')]:
            old=group[name]; group[name]=value
            changed=encode(facts)
            self.assertEqual(changed.returncode,0,changed.stderr)
            self.assertNotEqual(json.loads(changed.stdout)['identity'],output['identity'])
            group[name]=old
        old_key = facts['keys'][5]
        facts['keys'][5] = '01' + '00'*7
        changed = encode(facts)
        self.assertEqual(changed.returncode, 0, changed.stderr)
        self.assertNotEqual(json.loads(changed.stdout)['identity'], output['identity'])
        facts['keys'][5] = old_key
        for name,value in [('keys',['0'*16]*5), ('source_present',256),('stage_mask',64),('ubo_use',[0]*5),
                           ('texture_array_length',2**32),('translator_closure','a\0b')]:
            old=facts[name]; facts[name]=value
            self.assertNotEqual(encode(facts).returncode,0,name)
            facts[name]=old
        del g['has_blend']
        self.assertNotEqual(encode(facts).returncode,0)

if __name__ == '__main__':
    unittest.main()
