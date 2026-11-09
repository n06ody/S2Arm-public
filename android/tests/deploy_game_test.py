#!/usr/bin/env python3
"""主机回归：真实资源绑定逻辑 + 模拟 adb；不使用游戏数据、不安装 APK。

签名解析在这些合成 ZIP 用例中替身为证书摘要；真实签名由 adb install 校验。
"""
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import deploy_game
import game_apk

TITLE = '0100000000ABCDEF'


class _FakeProc:
    """push_tree 的 Popen 替身：即时完成、空输出。"""
    def __init__(self):
        self.returncode = 0
        self.stdout = io.StringIO('')

    def poll(self):
        return self.returncode

    def kill(self):
        self.returncode = -9


class FakeAdb:
    serial = 'test-device'

    def __init__(self, wrong_hash=False, refused=False, stale_start=False):
        self.calls = []
        self.wrong_hash, self.refused, self.stale_start = wrong_hash, refused, stale_start
        self.started = False
        self.receipt = None   # (contentRoot, artifactSha256)：App 导入成功时在 logcat 打的回执（正式包读不了 current.json）

    def call(self, *args, timeout=600):   # 与 deploy_game.Adb.call 同签名：tar 推送单独传超时
        self.calls.append(args)
        if args[0] == 'push': self.push_timeout = timeout
        if args[0] == 'install':
            self.digest = game_apk.sha256(args[-1])
        return 'Success'

    def popen(self, *args):   # 整树推送走 adb.popen（轮询进度+到点杀进程）；记进 calls 供断言
        self.calls.append(args)
        return _FakeProc()

    def running(self, package):
        return False

    def installed_hash(self, package):
        return '0' * 64 if self.wrong_hash else self.digest

    def shell(self, *args):
        self.calls.append(args)
        if args[:3] == ('am', 'start', '-W') and 'MainActivity' in ' '.join(args):
            self.started = True
        if args[0] == 'mv':
            self.name = args[-1].rsplit('/', 1)[-1]
        if args[0] == 'date':
            return '10-26 12:00:00.000'
        if args[0] == 'logcat':
            accepted = f'Game startup accepted: {TITLE} session 1'
            if self.refused:
                return f'Game import of import/{self.name} refused: missing resource'
            imported = f'Game build artifact updated (same files): test [{TITLE}] from import/{self.name}'
            if self.receipt:
                imported += f'\nGame import receipt: {TITLE} contentRoot={self.receipt[0]} artifactSha256={self.receipt[1]} from import/{self.name}'
            return '\n'.join((accepted, imported) if self.stale_start else ((imported, accepted) if self.started else (imported,)))
        return ''


class DeployTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.pack = self.root / 'pack'; self.pack.mkdir()
        self.apk = self.root / 'input.apk'
        nacp = bytearray(0x4000)
        nacp[:4] = b'Test'
        struct.pack_into('<Q', nacp, 0x3038, int(TITLE, 16))
        nacp[0x3060:0x3063] = b'1.0'
        self.files = {'ctl/control.nacp': bytes(nacp), 'exefs/main.npdm': b'npdm',
                      'romfs/data': b'synthetic resource', 'modules/main.patched.bin': b'code'}
        rows = []
        for name, data in sorted(self.files.items()):
            path = self.pack / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
            rows.append(f'{name}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}\n')
        index = ''.join(rows).encode(); (self.pack / 'index.tsv').write_bytes(index)
        (self.pack / 'manifest.json').write_text(json.dumps(
            dict(version=2, index_sha256=hashlib.sha256(index).hexdigest())))
        (self.pack / 'game-build-artifact.json').write_text(json.dumps(dict(
            titleId=TITLE, programId=TITLE, modules=[dict(name='main', buildId='b' * 64, image='modules/main.patched.bin')], toolchain={})))
        self.write_apk(b'code')
        self.signer = patch.object(game_apk, 'apk_signer_sha256', return_value='a' * 64)
        self.signer.start(); self.addCleanup(self.signer.stop)
        self.identity = patch.object(game_apk, 'apk_identity', return_value=('org.s2arm.game.t0100000000abcdef', 2, False), create=True)
        self.identity_mock = self.identity.start(); self.addCleanup(self.identity.stop)

    def write_apk(self, code):
        with zipfile.ZipFile(self.apk, 'w') as z:
            z.writestr('assets/game/game.properties', f'titleId={TITLE}\nversionCode=2\n')
            z.writestr('assets/game/modules/main.patched.bin', code)

    def deploy(self, adb, **kw):
        return deploy_game.deploy(self.pack, self.apk, adb, self.root / 'out', **kw)

    def test_debug_application_id_is_bound_without_changing_game_identity(self):
        self.identity_mock.return_value = ('org.s2arm.game.t0100000000abcdef.w2', 2, True)
        out = self.deploy(FakeAdb())
        artifact = json.loads((out / 'resources/game-build-artifact.json').read_text())
        self.assertEqual(artifact['apk']['packageName'], 'org.s2arm.game.t0100000000abcdef.w2')
        self.assertEqual(artifact['titleId'], TITLE)
        self.assertEqual(artifact['programId'], TITLE)
        self.assertEqual(artifact['modules'][0]['name'], 'main')
        self.assertEqual(artifact['modules'][0]['buildId'], 'b' * 64)
        self.assertEqual(artifact['modules'][0]['image'], 'main.patched.bin')
        self.assertEqual(json.loads((out / 'deployment.json').read_text())['packageName'],
                         'org.s2arm.game.t0100000000abcdef.w2')

    def test_suffix_release_or_wrong_title_is_rejected_before_resource_output(self):
        for package, debug in [('org.s2arm.game.t0100000000abcdef.w2', False),
                               ('org.s2arm.game.t0100000000000001.w2', True),
                               ('org.s2arm.game.t0100000000abcdef..w2', True)]:
            with self.subTest(package=package, debug=debug):
                self.identity_mock.return_value = (package, 2, debug)
                with self.assertRaises((ValueError, SystemExit)):
                    game_apk.resources(self.pack, self.apk, self.root / 'invalid')
                self.assertFalse((self.root / 'invalid').exists())

    def test_deploy_rejects_forged_artifact_package_before_adb(self):
        resources = self.root / 'resources'
        game_apk.resources(self.pack, self.apk, resources)
        path = resources / 'game-build-artifact.json'
        artifact = json.loads(path.read_text())
        artifact['apk']['packageName'] += '.w2'
        path.write_text(json.dumps(artifact))
        adb = FakeAdb()
        with self.assertRaisesRegex(ValueError, 'manifest'):
            deploy_game.deploy_resources(self.apk, resources, adb, self.root / 'deployment')
        self.assertEqual(adb.calls, [])

    def test_manifest_version_mismatch_is_rejected(self):
        self.identity_mock.return_value = ('org.s2arm.game.t0100000000abcdef', 3, False)
        with self.assertRaises((ValueError, SystemExit)):
            game_apk.resources(self.pack, self.apk, self.root / 'invalid')

    def test_full_and_refresh_bind_identically_without_recopying_romfs(self):
        full, refresh = self.root / 'full', self.root / 'refresh'
        game_apk.resources(self.pack, self.apk, full)
        game_apk.resources(self.pack, self.apk, refresh, refresh_existing=True)
        for name in ('manifest.json', 'index.tsv', 'game-build-artifact.json', 'ctl/control.nacp'):
            self.assertEqual((full / name).read_bytes(), (refresh / name).read_bytes())
        self.assertTrue((full / 'romfs/data').exists())
        self.assertFalse((refresh / 'romfs').exists())

    def test_deploy_freezes_apk_and_records_matching_identity(self):
        adb = FakeAdb()
        original = game_apk.sha256(self.apk)
        call = adb.call
        def replace_build_output(*args, **kw):
            result = call(*args, **kw)
            if args[0] == 'install':
                self.apk.write_bytes(b'another concurrent build')
            return result
        adb.call = replace_build_output
        out = self.deploy(adb, refresh_existing=True, mode='direct')
        receipt = json.loads((out / 'deployment.json').read_text())
        self.assertEqual(receipt['apkSha256'], original)
        self.assertEqual(game_apk.sha256(out / 'game.apk'), original)
        self.assertTrue(receipt['startupAccepted'])
        self.assertIn(('install', '-r', out / 'game.apk'), adb.calls)
        self.assertTrue(any(x[-2:] == ('s2arm.graphics_mode', 'direct') for x in adb.calls))
        self.assertFalse(any('clear' in x or 'uninstall' in x for x in adb.calls))

    def test_explicit_staging_and_wake_before_install(self):
        adb = FakeAdb()
        self.deploy(adb)
        install = next(i for i, x in enumerate(adb.calls) if x[0] == 'install')
        self.assertLess(adb.calls.index(('input', 'keyevent', 'KEYCODE_WAKEUP')), install)
        self.assertLess(adb.calls.index(('wm', 'dismiss-keyguard')), install)
        imports = [x for x in adb.calls if x[:3] == ('am', 'start', '-W') and 'org.s2arm.android.LibraryActivity' in ' '.join(x)]
        self.assertEqual(len(imports), 2)
        self.assertNotIn('org.s2arm.android.IMPORT_STAGING', imports[0])
        self.assertIn('org.s2arm.android.IMPORT_STAGING', imports[1])
        self.assertEqual(imports[1][-1],
                         f'/sdcard/Android/data/org.s2arm.game.t{TITLE.lower()}/files/import/{adb.name}')
        launches = [x for x in adb.calls if x[:3] == ('am', 'start', '-W') and 'org.s2arm.android.MainActivity' in ' '.join(x)]
        self.assertEqual(len(launches), 1)
        self.assertNotIn('org.s2arm.android.IMPORT_STAGING', launches[0])
        cleanups = [x for x in adb.calls if x[:2] == ('rm', '-rf')]
        self.assertEqual(len(cleanups), 1)
        self.assertTrue(all(adb.name in path for path in cleanups[0][2:]))

    def test_no_start_imports_and_cleans_only_its_tree(self):
        adb = FakeAdb()
        out = self.deploy(adb, start=False)
        self.assertFalse(json.loads((out / 'deployment.json').read_text())['startupAccepted'])
        self.assertFalse(any('MainActivity' in ' '.join(map(str, x)) for x in adb.calls))
        clean = next(x for x in adb.calls if x[:2] == ('rm', '-rf'))
        self.assertTrue(all(adb.name in p for p in clean[2:]))

    def test_push_timeout_scales_with_resource_size(self):
        # 期望按 30 MB/s、2 倍余量手算：21 GB → 21e9 / 30e6 × 2 = 1400 秒（+1 取整）；小包取下限 120；显式 --push-timeout 照用
        # （超时现在在 push_tree 轮询里执行，截获它的 timeout 入参核对估值管线）
        for size, want in ((21_000_000_000, 1401), (1_000_000, 120)):
            with self.subTest(size=size), patch.object(deploy_game, 'tree_bytes', return_value=size), \
                    patch.object(deploy_game, 'push_tree') as pt:
                adb = FakeAdb(); self.deploy(adb)
                self.assertEqual(pt.call_args[0][3], want)
        with patch.object(deploy_game, 'tree_bytes', return_value=21_000_000_000), \
                patch.object(deploy_game, 'push_tree') as pt:
            adb = FakeAdb(); self.deploy(adb, push_timeout=50)
            self.assertEqual(pt.call_args[0][3], 50)

    def test_failed_or_interrupted_push_cleans_own_paths(self):
        for failure in (RuntimeError('push failed'), InterruptedError('cancelled')):
            with self.subTest(failure=failure):
                adb = FakeAdb()
                def popen(*args, **kwargs):
                    raise failure
                adb.popen = popen
                with self.assertRaises(type(failure)):
                    self.deploy(adb)
                clean = next(x for x in adb.calls if x[:2] == ('rm', '-rf'))
                self.assertEqual(len(clean), 4)
                self.assertIn('/.deploy-', clean[2])
                self.assertIn('/import/deploy-', clean[3])

    def test_permission_failure_still_cleans_and_cleanup_failure_is_visible(self):
        adb = FakeAdb()
        original = adb.shell
        def shell(*args):
            if args[0] == 'chmod':
                raise RuntimeError('chmod failed')
            if args[:2] == ('rm', '-rf'):
                raise RuntimeError('rm failed')
            return original(*args)
        adb.shell = shell
        with self.assertRaisesRegex(RuntimeError, 'chmod failed.*设备临时副本清理失败.*rm failed.*deploy-'):
            self.deploy(adb)

    def test_resources_current_identity_and_result(self):
        resources = self.root / 'resources'
        game_apk.resources(self.pack, self.apk, resources)
        artifact = json.loads((resources / 'game-build-artifact.json').read_text())
        artifact_sha = deploy_game.digest(resources / 'game-build-artifact.json')
        adb = FakeAdb()
        adb.receipt = (artifact['contentRoot'], artifact_sha)
        work = self.root / 'work'; work.mkdir()
        receipt = deploy_game.deploy_resources(self.apk, resources, adb, work)
        self.assertEqual(receipt['artifactSha256'], artifact_sha)
        self.assertEqual(receipt['contentRoot'], artifact['contentRoot'])
        self.assertTrue(receipt['startupAccepted'])

    def test_resources_mismatch_rejected_before_device(self):
        resources = self.root / 'resources'
        game_apk.resources(self.pack, self.apk, resources)
        self.apk.write_bytes(b'different apk')
        adb = FakeAdb()
        with self.assertRaisesRegex(ValueError, 'APK 与资源包不匹配'):
            deploy_game.deploy_resources(self.apk, resources, adb, self.root)
        self.assertEqual(adb.calls, [])

    def test_bad_current_identity_cleans_and_never_starts(self):
        resources = self.root / 'resources'
        game_apk.resources(self.pack, self.apk, resources)
        adb = FakeAdb(); adb.receipt = ('sha256-' + '0' * 64, '1' * 64)
        with self.assertRaisesRegex(RuntimeError, '设备导入身份不匹配'):
            deploy_game.deploy_resources(self.apk, resources, adb, self.root)
        self.assertTrue(any(x[:2] == ('rm', '-rf') for x in adb.calls))
        self.assertFalse(any('MainActivity' in ' '.join(map(str, x)) for x in adb.calls))

    def test_missing_import_receipt_is_red_and_never_starts(self):
        # 正式包不可调试、没有 run-as：设备不打回执就不能当身份核过
        resources = self.root / 'resources'
        game_apk.resources(self.pack, self.apk, resources)
        adb = FakeAdb()
        with self.assertRaisesRegex(RuntimeError, '没打出导入回执'):
            deploy_game.deploy_resources(self.apk, resources, adb, self.root)
        self.assertFalse(any('MainActivity' in ' '.join(map(str, x)) for x in adb.calls))
        self.assertFalse(any('run-as' in map(str, x) for x in adb.calls))

    def test_module_mismatch_stops_before_install(self):
        self.write_apk(b'wrong module'); adb = FakeAdb()
        with self.assertRaises(SystemExit):
            self.deploy(adb)
        self.assertEqual(adb.calls, [])

    def test_wrong_installed_hash_stops_before_push(self):
        adb = FakeAdb(wrong_hash=True)
        with self.assertRaisesRegex(RuntimeError, '安装后的 APK 哈希'):
            self.deploy(adb)
        self.assertFalse(any(x[0] == 'push' for x in adb.calls))

    def test_refused_refresh_has_no_success_receipt(self):
        with self.assertRaisesRegex(RuntimeError, '去掉 --refresh-existing'):
            self.deploy(FakeAdb(refused=True), refresh_existing=True)
        self.assertEqual(list((self.root / 'out').glob('*/deployment.json')), [])

    def test_startup_before_this_import_is_not_success(self):
        # push_tree 计时也走 time.monotonic，给足返回值：前段都回 0，启动轮询处跳到超过 deadline
        with patch.object(deploy_game.time, 'monotonic', side_effect=[0] * 8 + [5]), \
                patch.object(deploy_game.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, '超时'):
                self.deploy(FakeAdb(stale_start=True), timeout=1)
        self.assertEqual(list((self.root / 'out').glob('*/deployment.json')), [])

    def test_corrupt_metadata_refused(self):
        (self.pack / 'ctl/control.nacp').write_bytes(b'damaged')
        with self.assertRaises(SystemExit):
            game_apk.resources(self.pack, self.apk, self.root / 'refresh', refresh_existing=True)

    def test_output_must_not_replace_input(self):
        with self.assertRaises(SystemExit):
            game_apk.resources(self.pack, self.apk, self.pack, refresh_existing=True)
        self.assertEqual((self.pack / 'modules/main.patched.bin').read_bytes(), b'code')


if __name__ == '__main__':
    unittest.main()
