#!/usr/bin/env python3
"""Real Android package backends, rejection-before-replace, and Java consumers.

No game data or network. Uses the shared C resolver and this interpreter's pinned
Python dependencies; S2A_T05_EVIDENCE can retain command logs outside the checkout.
The shared-page contract is deliberately not skipped on an unfixed producer.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
MAKE = REPO / 'android/tools/make_pack.py'
spec = importlib.util.spec_from_file_location('android_pack', MAKE)
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def snapshot(root):
    return {str(p.relative_to(root)): p.read_bytes() for p in sorted(root.rglob('*')) if p.is_file()}


class MakePackRewriterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.retained = os.environ.get('S2A_T05_EVIDENCE')
        cls.base = Path(cls.retained or tempfile.mkdtemp(prefix='android-pack-backends-'))
        if cls.retained:
            cls.base.mkdir(parents=True, exist_ok=False)
        cls.commands = []
        cls.env = {k: v for k, v in os.environ.items() if not k.startswith('S2A_')}
        for name in ('S2A_REWRITER_DEPS', 'S2A_REWRITER_C'):
            if os.environ.get(name):
                cls.env[name] = os.environ[name]
        cls.env.update(PY=sys.executable, SOURCE_DATE_EPOCH='1700000000',
                       S2A_REWRITER_CACHE=str(cls.base / 'C cache'))
        result = cls.command([sys.executable, str(REPO / 'pack/rewriter.py'), 'c-path'], cls.env)
        if result.returncode:
            raise RuntimeError('shared C resolver failed: ' + result.stderr)
        cls.c = result.stdout.strip()

    @classmethod
    def tearDownClass(cls):
        if not cls.retained:
            shutil.rmtree(cls.base)

    @classmethod
    def command(cls, argv, env):
        result = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=90)
        number = len(cls.commands)
        (cls.base / f'{number:03}.stdout.log').write_text(result.stdout)
        (cls.base / f'{number:03}.stderr.log').write_text(result.stderr)
        cls.commands.append(dict(argv=[str(x) for x in argv], returncode=result.returncode,
                                 environment={k: v for k, v in env.items() if k in
                                              ('PY', 'SOURCE_DATE_EPOCH', 'S2A_REWRITER', 'S2A_REWRITER_C',
                                               'S2A_TARGET', 'S2A_REWRITE_SPIN', 'S2A_SDK_ADDR_CUT')}))
        (cls.base / 'commands.json').write_text(json.dumps(cls.commands, indent=2) + '\n')
        return result

    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix=self._testMethodName + '-', dir=self.base))
        self.root = Path(pack.synthetic_root(str(self.directory)))
        # A legitimate non-overlapping fixture: data begins on a 16KiB boundary.
        # The separate shared-page test retains the original layout and must pass
        # only when the shared C/Python Android producer contract is actually fixed.
        self.original_nso = (self.root / 'exefs/main').read_bytes()
        source = bytearray(self.original_nso)
        struct.pack_into('<I', source, 0x34, 0x4000)
        (self.root / 'exefs/main').write_bytes(source)
        shutil.copyfile(self.root / 'exefs/main', self.root / 'exefs/sdk')
        (self.root / 'widen_verified.handheld.tsv').write_text('authored test marker\n')
        # F7：合成安卓产物根带合法的平台能力清单。
        (self.root / 's2a_caps.tsv').write_text('plat\tandroid\nwiden\t1\nfps60\t1\n')

    def make(self, backend='c', out=None, expected=0, **settings):
        out = out or self.directory / ('package-' + backend)
        env = dict(self.env, **settings)
        if backend is not None:
            env['S2A_REWRITER'] = backend
        result = self.command([sys.executable, str(MAKE), str(out), '--root', str(self.root)], env)
        if expected == 0:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return out, result

    def table(self, heap=128, program='0100000000abcdef'):
        build = (self.root / 'exefs/main').read_bytes()[0x40:0x50].hex()
        return f'版本\t2\n程序号\t{program}\n构建号\t{build}\n别名区GB\t63\n普通堆MB\t{heap}\n'

    def old_output(self):
        out = self.directory / 'old-package'; out.mkdir()
        (out / 'manifest.json').write_text('{"version":2}')
        (out / 'index.tsv').write_text('old index\n')
        (out / 'keep.bin').write_bytes(b'previous package must survive')
        return out

    def assert_preserved(self, **settings):
        out = self.old_output(); before = snapshot(out)
        _, result = self.make(out=out, expected=1, **settings)
        self.assertEqual(snapshot(out), before)
        return result

    def wrapper(self, fault):
        path = self.directory / ('C-wrapper-' + fault)
        # Fault injection wraps the actual C CLI; no fake rewritten fixture is
        # used to establish a successful package or backend byte comparison.
        path.write_text('#!' + sys.executable + '\n' +
                        'import os,pathlib,subprocess,sys,json,re\n' +
                        f'fault={fault!r}\n' +
                        "if fault=='nonzero': sys.exit(7)\n" +
                        f'rc=subprocess.call([{self.c!r}]+sys.argv[1:])\n' +
                        'if rc: sys.exit(rc)\n' +
                        "prefix=pathlib.Path(sys.argv[2])/sys.argv[3]\n" +
                        "p=pathlib.Path(str(prefix)+'.fixups.tsv')\n" +
                        "s=p.read_text()\n" +
                        "if fault=='target': p.write_text(s.replace('# TARGET\\tandroid', '# TARGET\\tapple'))\n" +
                        "if fault=='abi': p.write_text(s.replace('android-sp-safe-v1','android-below-sp-v1'))\n" +
                        "if fault=='bias': p.write_text(s+'# LOAD_BIAS\\t0x1000\\n')\n" +
                        "if fault=='build': p.write_text(re.sub(r'(?m)^# BUILD_ID\\t.*$', '# BUILD_ID\\tinvalid', s))\n" +
                        "if fault=='duplicate': p.write_text(s+'# TARGET\\tandroid\\n')\n" +
                        "if fault=='missing': pathlib.Path(str(prefix)+'.gates.tsv').unlink()\n" +
                        "if fault=='gaps': pathlib.Path(str(prefix)+'.gaps.tsv').write_text('0x100\\tfixture\\tgap\\n')\n" +
                        "if fault=='environment':\n" +
                        " pathlib.Path(__file__+'.env.json').write_text(json.dumps({k:os.environ.get(k) for k in ('S2A_TARGET','S2A_REWRITE_SPIN','S2A_SDK_ADDR_CUT')}))\n" +
                        "if fault=='fps':\n" +
                        " pathlib.Path(__file__+'.fps.json').write_text(json.dumps(os.environ.get('S2A_FPS_TSV')))\n" +
                        "if fault=='cpatch':\n" +
                        " t=os.environ.get('S2A_CPATCH_TSV')\n" +
                        " with open(__file__+'.cpatch.jsonl','a') as f: f.write(json.dumps([sys.argv[3], t, pathlib.Path(t).read_text() if t else None])+'\\n')\n")
        path.chmod(0o700)
        return str(path)

    def test_real_backends_full_bytes_and_java_consumers(self):
        # Python 改写器 10-31 删除（ea97dc1e0）、不再对拍：C 是唯一后端；显式要 python 必须报红、不回退
        comparisons = []
        for spin, cut in ((0, 1), (1, 1), (0, 0)):
            options = dict(S2A_REWRITE_SPIN=str(spin), S2A_SDK_ADDR_CUT=str(cut))
            a, _ = self.make('c', out=self.directory / f'c-{spin}-{cut}', **options)
            comparisons.append(dict(spin=spin, sdk_addr_cut=cut, files={name:
                dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                for name, data in snapshot(a).items()}))
        (self.base / 'comparisons.json').write_text(json.dumps(comparisons, indent=2) + '\n')
        a = self.directory / 'c-0-1'
        _, refused = self.make('python', out=self.directory / 'python-refused', expected=1)
        self.assertIn('Python 改写器已删除', refused.stdout + refused.stderr)
        # Check the default backend independently of explicit c selection.
        default, _ = self.make(None, out=self.directory / 'default-backend',
                              S2A_REWRITE_SPIN='0', S2A_SDK_ADDR_CUT='1')
        self.assertEqual(snapshot(default), snapshot(self.directory / 'c-0-1'))
        self.assertEqual((a / 'modules/addr_need.tsv').read_bytes(), (a / 'exefs/addr_need.tsv').read_bytes())
        # The table is static recognition of main alone: the heap is always the application pool (tools/addr_need.py APP_POOL_MB)
        self.assertIn('普通堆MB\t3284\n', (a / 'exefs/addr_need.tsv').read_text())
        self.assertEqual((a / 'modules/widen_verified.handheld.tsv').read_text(), 'authored test marker\n')
        artifact = json.loads((a / 'game-build-artifact.json').read_text())
        self.assertEqual(artifact['contentRoot'], 'sha256-' + hashlib.sha256((a / 'index.tsv').read_bytes()).hexdigest())
        # A table lying in the root (per-game or from an earlier pack) is never read back: same bytes as c-0-1 (same backend and options)
        before = json.loads((self.directory / 'c-0-1/game-build-artifact.json').read_text())['contentRoot']
        (self.root / 'addr_need.tsv').write_text(self.table(256))
        (self.root / 'exefs/addr_need.tsv').write_text(self.table(256))
        c, _ = self.make('c', out=self.directory / 'stray-requirements', S2A_REWRITE_SPIN='0')
        self.assertEqual(before, json.loads((c / 'game-build-artifact.json').read_text())['contentRoot'])
        java = REPO / 'android/app/src/main/java/org/s2arm/android'
        tests = REPO / 'android/tests'
        classes = self.directory / 'classes'; classes.mkdir()
        sources = [str(java / (n + '.java')) for n in
                   ('Json', 'BootContract', 'RuntimeLibrary', 'GamePack', 'GameFingerprint', 'GameInstall',
                    'SaveArchive', 'ImportReceiver', 'GameStorage', 'GameMetadata')]
        sources += [str(tests / (n + '.java')) for n in ('GamePackTest','GameInstallTest')]
        compiled = self.command(['javac', '-encoding', 'UTF-8', '-d', str(classes)] + sources, self.env)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        for name in ('GamePackTest', 'GameInstallTest'):
            result = self.command(['java','-cp',str(classes),'org.s2arm.android.' + name,str(a)], self.env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_android_defaults_and_explicit_options_reach_actual_cli(self):
        wrapper = self.wrapper('environment')
        out, _ = self.make(S2A_REWRITER_C=wrapper, S2A_TARGET='apple')
        observed = json.loads(Path(wrapper + '.env.json').read_text())
        self.assertEqual(observed, dict(S2A_TARGET='android',S2A_REWRITE_SPIN='1',S2A_SDK_ADDR_CUT='1'))
        self.make(out=self.directory / 'explicit', S2A_REWRITER_C=wrapper,
                  S2A_REWRITE_SPIN='0', S2A_SDK_ADDR_CUT='0')
        observed = json.loads(Path(wrapper + '.env.json').read_text())
        self.assertEqual(observed, dict(S2A_TARGET='android',S2A_REWRITE_SPIN='0',S2A_SDK_ADDR_CUT='0'))
        self.assertIn('# TARGET\tandroid', (out / 'modules/main.fixups.tsv').read_text())

    def test_inherited_fps_table_does_not_reach_cli(self):
        # #48 D2：改写器只认产物根自己的 fps.tsv；环境里留着的 S2A_FPS_TSV 不交给改写器、也不进包
        table = self.directory / 'fps.tsv'
        table.write_text('# 夹具：内容不被读到\n')
        wrapper = self.wrapper('fps')
        out, _ = self.make(S2A_REWRITER_C=wrapper, S2A_FPS_TSV=str(table))
        self.assertIsNone(json.loads(Path(wrapper + '.fps.json').read_text()))
        self.assertFalse((out / 'fps.tsv').exists())

    def test_product_fps_table_reaches_cli_and_package(self):
        # #48 D2：产物根的 fps.tsv 交给改写器（读槽指令），同一份进游戏数据根给 runtime/src/fps.c 起局写槽；环境里的另一份不顶替它
        table = self.root / 'fps.tsv'
        table.write_text('build 0000000000000000\nslot 2041 f32 2 1\n')
        stray = self.directory / 'stray-fps.tsv'
        stray.write_text('# 环境里的另一份\n')
        wrapper = self.wrapper('fps')
        out, _ = self.make(S2A_REWRITER_C=wrapper, S2A_FPS_TSV=str(stray))
        self.assertEqual(json.loads(Path(wrapper + '.fps.json').read_text()), str(table))
        self.assertEqual((out / 'fps.tsv').read_bytes(), table.read_bytes())

    def community(self, *names):
        """合成社区补丁索引（#48 D2）：对合成 main（构建号 5332412D…，代码见 android/tools/selftest_nso.py）的三份 pchtxt：
        good = 0x200 填充字 → nop（常写 w 行）+ 0x104 ret → mov w0,#2（双路 p 行）；bad = 0x11c ret → mrs x0,tpidr_el0（补丁指令要翻译：改写器记 cpatch 缺口）；
        data = data 段 0x4010 写 AABBCCDD（装载器写，不改代码）"""
        build = (self.root / 'exefs/main').read_bytes()[0x40:0x4c].hex().upper()
        le = lambda word: struct.pack('<I', word).hex().upper()
        bodies = {'good': [f'00000200 {le(0xd503201f)}', f'00000104 {le(0x52800040)}'],
                  'bad': [f'0000011C {le(0xd53bd040)}'],
                  'data': ['00004010 AABBCCDD']}
        index = self.directory / 'community' / 'gfx'
        index.mkdir(parents=True)
        for name in names:
            (index / f'{name}.pchtxt').write_text(f'@nsobid-{build}\n@flag offset_shift 0x100\n@enabled\n' +
                                                 ''.join(line + '\n' for line in bodies[name]))
        (index / 'INDEX.tsv').write_text('title_id\tpath\tkind\tlicense\n' +
                                         ''.join(f'0100000000ABCDEF\t{n}.pchtxt\tpchtxt\tMIT\n' for n in names))
        uid = {n: 'gfx:' + hashlib.sha256((index / f'{n}.pchtxt').read_bytes()).hexdigest()[:16] for n in names}
        return str(index), uid

    def cpatch_calls(self, wrapper):
        path = Path(wrapper + '.cpatch.jsonl')
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def test_community_patches_stay_out_unless_enabled(self):
        # #48 D2：玩家包缺省不带上游补丁字节（S2A_COMMUNITY_PATCHES 没设）——有索引也不打、不进包；环境里漏进来的补丁行不交给改写器
        index, _ = self.community('good', 'data')
        stray = self.directory / 'stray-rows.tsv'
        stray.write_text('p\tmain\t104\td65f03c0\t52800040\t0\n')
        wrapper = self.wrapper('cpatch')
        out, _ = self.make(S2A_REWRITER_C=wrapper, S2A_CPATCH_DIRS=index, S2A_CPATCH_TSV=str(stray))
        self.assertEqual({m: t for m, t, _ in self.cpatch_calls(wrapper)}, {'main': None, 'sdk': None})
        self.assertFalse((out / 'modules/cpatch.tsv').exists())
        self.assertFalse((out / 'cpatch.tsv').exists())
        _, result = self.make(out=self.directory / 'package-none', S2A_REWRITER_C=wrapper, S2A_CPATCH_DIRS=index,
                              S2A_COMMUNITY_PATCHES='1', S2A_CPATCH='none')
        self.assertFalse((self.directory / 'package-none' / 'modules/cpatch.tsv').exists())

    def test_community_patches_reach_rewriter_and_package(self):
        # #48 D2：S2A_COMMUNITY_PATCHES=1 时按安卓改写器出表（同 pack/stage_rewrite.sh 的 plan → 带补丁行改写 → prune 循环）：
        #   bad 那份让改写器记了 cpatch 缺口 → 剔掉、重出表，good 与 data 进包；进包的装载器表与最后一轮交给改写器的补丁行是同一次 plan 出的
        index, uid = self.community('good', 'bad', 'data')
        plain, _ = self.make(out=self.directory / 'package-plain')
        stray = self.directory / 'stray-rows.tsv'
        stray.write_text('# 环境里的另一份\n')
        wrapper = self.wrapper('cpatch')
        out, result = self.make(S2A_REWRITER_C=wrapper, S2A_CPATCH_DIRS=index, S2A_COMMUNITY_PATCHES='1',
                                S2A_CPATCH=uid['good'], S2A_CPATCH_TSV=str(stray))
        calls = self.cpatch_calls(wrapper)
        self.assertEqual([t for _, t, _ in calls[:2]], [None, None], '第一轮不带补丁行（plan 预筛要不带补丁时的改写点）')
        rounds = [rows for _, t, rows in calls[2:] if t]
        self.assertTrue(rounds and all(t and t != str(stray) for _, t, _ in calls[2:]))
        self.assertIn(uid['bad'], rounds[0])
        self.assertNotIn(uid['bad'], rounds[-1])
        self.assertIn(f'p\tmain\t104\td65f03c0\t52800040\t0\t# {uid["good"]}', rounds[-1])
        # 表随模块进 modules/（APK 起局 loader.c 经资源读面读 modules/cpatch.tsv），包根不再放一份
        self.assertFalse((out / 'cpatch.tsv').exists())
        table = [line.split('\t') for line in (out / 'modules/cpatch.tsv').read_text().splitlines()]
        self.assertEqual(table[0][0], '编号')
        self.assertEqual([(r[0], r[1], r[2], r[7], r[8]) for r in table[1:]],
                         [(uid['good'], 'main', '0', '', '1'), (uid['data'], 'main', '1', '4010:00000000:aabbccdd', '0')])
        self.assertIn('社区补丁：2 份进包，不收 1 份', result.stdout)
        self.assertNotEqual((out / 'modules/main.patched.bin').read_bytes(), (plain / 'modules/main.patched.bin').read_bytes())

    def test_community_patches_all_code_pruned_rewrites_clean(self):
        # 带补丁行的那份全被剔了、只剩数据补丁：终稿模块必须是不带补丁行的改写（上一轮的双路字与缺口不能留在包里）
        index, uid = self.community('bad', 'data')
        plain, _ = self.make(out=self.directory / 'package-plain')
        out, _ = self.make(S2A_CPATCH_DIRS=index, S2A_COMMUNITY_PATCHES='1')
        self.assertEqual([line.split('\t')[0] for line in (out / 'modules/cpatch.tsv').read_text().splitlines()[1:]], [uid['data']])
        for name in ('main.patched.bin', 'main.fixups.tsv', 'main.gates.tsv'):
            self.assertEqual((out / 'modules' / name).read_bytes(), (plain / 'modules' / name).read_bytes(), name)

    def test_selector_and_execution_failures_preserve_previous_output(self):
        for mode, settings in [('backend',dict(backend='invalid')),('missing',dict(S2A_REWRITER_C=str(self.directory/'absent'))),
                               ('exit',dict(S2A_REWRITER_C=self.wrapper('nonzero')))]:
            with self.subTest(mode=mode):
                out = self.directory / ('old-' + mode); out.mkdir()
                (out / 'manifest.json').write_text('{"version":2}'); (out / 'index.tsv').write_text('old\n')
                before = snapshot(out)
                self.make(out=out, expected=1, **settings)
                self.assertEqual(snapshot(out), before)

    def test_actual_cli_output_faults_are_rejected_before_replace(self):
        for fault in ('target','abi','bias','build','duplicate','missing','gaps'):
            with self.subTest(fault=fault):
                out = self.directory / ('old-' + fault); out.mkdir()
                (out/'manifest.json').write_text('{"version":2}'); (out/'index.tsv').write_text('old\n')
                before = snapshot(out)
                self.make(out=out, expected=1, S2A_REWRITER_C=self.wrapper(fault))
                self.assertEqual(snapshot(out), before)

    def test_write_pack_revalidates_every_module_before_replacing_output(self):
        # Start from real backend output, then corrupt the staged copy after the
        # rewriter's own admission. This isolates write_pack's final boundary.
        valid, _ = self.make(out=self.directory / 'valid-for-admission')
        for fault in ('missing-main', 'sdk-abi', 'sdk-missing-gates'):
            with self.subTest(fault=fault):
                modules = self.directory / ('modules-' + fault)
                shutil.copytree(valid / 'modules', modules)
                if fault == 'missing-main':
                    (modules / 'main.patched.bin').unlink()
                elif fault == 'sdk-abi':
                    path = modules / 'sdk.fixups.tsv'
                    path.write_text(path.read_text().replace('android-sp-safe-v1', 'wrong-abi'))
                else:
                    (modules / 'sdk.gates.tsv').unlink()
                out = self.directory / ('previous-' + fault); out.mkdir()
                (out / 'manifest.json').write_text('{"version":2}')
                (out / 'index.tsv').write_text('old index\n')
                (out / 'keep.bin').write_bytes(b'previous package must survive')
                before = snapshot(out)
                entries = [(name, str(valid / name)) for name in ('ctl', 'exefs', 'romfs')]
                entries.append(('modules', str(modules)))
                with self.assertRaises(SystemExit):
                    try:
                        pack.write_pack(str(out), entries, force=True)
                    finally:
                        self.assertTrue(snapshot(out) == before, fault + ' changed the previous package')

    def test_bad_nso_preserves_previous_output_and_foreign_table_is_ignored(self):
        good = (self.root/'exefs/main').read_bytes()
        (self.root/'exefs/main').write_bytes(b'not an NSO')
        self.assert_preserved()
        (self.root/'exefs/main').write_bytes(good)
        clean,_=self.make(out=self.directory/'clean')
        (self.root/'addr_need.tsv').write_text(self.table(program='0100000000000000'))
        stray,_=self.make(out=self.directory/'foreign-table')
        self.assertEqual((stray/'exefs/addr_need.tsv').read_bytes(),(clean/'exefs/addr_need.tsv').read_bytes())

    def test_force_cannot_replace_input_root(self):
        before=snapshot(self.root)
        result=self.command([sys.executable,str(MAKE),str(self.root),'--root',str(self.root),'--force'],self.env)
        self.assertNotEqual(result.returncode,0);self.assertEqual(snapshot(self.root),before)

    def check_shared_page_contract(self, backend):
        (self.root/'exefs/main').write_bytes(self.original_nso)
        (self.root/'exefs/sdk').unlink()
        out,_=self.make(backend)
        header=pack.fixups_header(str(out/'modules/main.fixups.tsv'))
        self.assertEqual(int(header.get('LOAD_BIAS','0'),16),0)

    def test_shared_page_android_contract_c(self):
        self.check_shared_page_contract('c')


if __name__ == '__main__':
    unittest.main()
