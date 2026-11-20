#!/usr/bin/env python3
"""主机回归：真实资源绑定逻辑 + 模拟 adb；不使用游戏数据、不安装 APK。

签名解析在这些合成 ZIP 用例中替身为证书摘要；真实签名由 adb install 校验。
"""
import contextlib
import errno
import hashlib
import io
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import deploy_game
import game_apk

TITLE = '0100000000ABCDEF'


class FakeAdb:
    """模拟 adb；forward 时在本机起一个假的 App 接收端（照 ImportReceiver.java 的协议），收下的文件记在 received。"""
    serial = 'test-device'

    def __init__(self, wrong_hash=False, refused=False, stale_start=False, slow_import=False,
                 receive_reply=None, drop_first=0):
        self.calls = []
        self.wrong_hash, self.refused, self.stale_start = wrong_hash, refused, stale_start
        self.slow_import = slow_import   # 导入一直没做完：logcat 里没有成功行
        self.receive_reply = receive_reply   # 非空：接收端回这一行（如 ERR …）而不是 OK
        self.drop_first = drop_first         # 前几次连接一接上就断（设备端还没监听时 adb forward 的样子）
        self.started = False
        self.receipt = None   # (contentRoot, artifactSha256)：App 导入成功时在 logcat 打的回执（正式包读不了 current.json）
        self.received, self.name, self.connects = {}, None, 0
        self.digest = None    # 机上没装：真 adb 的 installed_hash 此时抛 RuntimeError（pm path 为空）
        self.remote = {}      # 设备共享存储上已有文件的 SHA-256（路径 → 摘要），sha256sum 照它答
        self.in_place = None  # App 收到的原地登记请求路径
        self.in_place_refused = False
        self.save_received = None
        self.save_reply = None

    def _serve(self, server):
        while True:
            try:
                conn, _ = server.accept()
            except OSError:
                return
            with conn:
                self.connects += 1
                if self.connects <= self.drop_first:
                    continue
                f = conn.makefile('rb')
                conn.sendall(b'READY 1\n')
                head = f.readline().decode().rstrip('\n').split(' ', 2)
                if head[0] == 'S2A-INPLACE':
                    self.in_place = head[2]
                    conn.sendall(b'OK in-place\n')
                    continue
                if head[0] == 'S2A-SAVE':
                    title, size_text = head[2].split(' ')
                    size = int(size_text)
                    received = f.read(size)
                    if head[1] != '1' or title != TITLE or size <= 0 or len(received) != size or f.readline() != b'END\n':
                        conn.sendall(b'ERR save_header\n')
                        continue
                    self.save_received = received, self.started
                    backup = f'save-transfer/{title}/save-backups/{title}/test/before-restore.s2asave'
                    receipt = dict(titleId=title, files=2, bytes=16, previousBackup=backup)
                    conn.sendall(((self.save_reply or 'OK ' + json.dumps(receipt)) + '\n').encode())
                    continue
                self.name = head[2]
                while True:
                    line = f.readline().decode().rstrip('\n')
                    if line == 'END':
                        break
                    _, size, path = line.split(' ', 2)
                    self.received[path] = f.read(int(size))
                conn.sendall(((self.receive_reply or f'OK {len(self.received)} 0') + '\n').encode())

    def call(self, *args, timeout=600):
        self.calls.append(args)
        if args[0] == 'install':
            self.digest = game_apk.sha256(args[-1])
        if args[:2] == ('forward', 'tcp:0'):
            server = socket.socket(); server.bind(('127.0.0.1', 0)); server.listen(4)
            self.server = server
            threading.Thread(target=self._serve, args=(server,), daemon=True).start()
            return f'{server.getsockname()[1]}\n'
        if args[:2] == ('forward', '--remove'):
            self.server.close()
        return 'Success'

    def running(self, package):
        return False

    def installed_hash(self, package):
        if self.wrong_hash:
            return '0' * 64
        if self.digest is None:
            raise RuntimeError('需要单体 APK；无法确定已安装 base APK 或发现 split APK')
        return self.digest

    def shell(self, *args):
        self.calls.append(args)
        if args[:3] == ('am', 'start', '-W') and 'MainActivity' in ' '.join(args):
            self.started = True
        if args[0] == 'date':
            return '10-26 12:00:00.000'
        if args[0] == 'sha256sum':
            if args[1] not in self.remote:
                raise RuntimeError('adb shell failed: no such file')
            return f'{self.remote[args[1]]}  {args[1]}'
        if args[0] == 'logcat' and self.in_place:
            if self.in_place_refused:
                return f'Game in-place registration of {self.in_place} refused: in-place file does not match index.tsv (SHA-256): romfs/data'
            imported = (f'Game registered in place: test [{TITLE}] -> sha256-0000 (files read where they are, nothing copied)'
                        f' from in-place {self.in_place}')
            accepted = f'Game startup accepted: {TITLE} session 1'
            return '\n'.join((imported, accepted) if self.started else (imported,))
        if args[0] == 'logcat':
            accepted = f'Game startup accepted: {TITLE} session 1'
            if self.refused:
                return f'Game import of import/{self.name} refused: missing resource'
            if self.slow_import:
                return ''
            imported = f'Game build artifact updated (same files): test [{TITLE}] from import/{self.name}'
            if self.receipt:
                imported += f'\nGame import receipt: {TITLE} contentRoot={self.receipt[0]} artifactSha256={self.receipt[1]} from import/{self.name}'
            return '\n'.join((accepted, imported) if self.stale_start else ((imported, accepted) if self.started else (imported,)))
        return ''


class OwnLinesTest(unittest.TestCase):
    """导入结论只认目标包自己的进程：别的 S2Arm App 同标签的 refused 不算（11-13 误判）。期望按日志行手算。"""
    class Adb:
        def __init__(self, pids): self.pids = pids
        def shell(self, *args): return self.pids if args[0] == 'pidof' else ''

    LOGS = ('I/S2Arm   (  999): Game import of requested directory refused: package has no index.tsv\n'
            'I/S2Arm   ( 4242): Game installed: test [0100000000ABCDEF] from import/deploy-x')

    def test_other_app_refusal_is_ignored(self):
        kept = deploy_game.own_lines(self.Adb('4242'), 'org.s2arm.game.t0100000000abcdef', self.LOGS)
        self.assertNotIn('refused', kept)
        self.assertIn('Game installed', kept)

    def test_no_pid_keeps_everything(self):
        self.assertEqual(deploy_game.own_lines(self.Adb(''), 'p', self.LOGS), self.LOGS)


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

    def test_save_transfer_commit_precedes_launch(self):
        archive = self.root / 'test.s2asave'
        archive.write_bytes(b'synthetic wire payload')
        adb = FakeAdb()
        out = self.deploy(adb, save_archive=archive)
        self.assertEqual(adb.save_received, (archive.read_bytes(), False))
        self.assertTrue(adb.started)
        result = json.loads((out / 'deployment.json').read_text())['saveImport']
        self.assertEqual((result['titleId'], result['files'], result['bytes']), (TITLE, 2, 16))
        self.assertTrue(result['previousBackup'].endswith('/before-restore.s2asave'))
        self.assertNotIn('run-as', ' '.join(str(call) for call in adb.calls))

    def test_save_refusal_does_not_launch_game(self):
        archive = self.root / 'test.s2asave'
        archive.write_bytes(b'synthetic wire payload')
        adb = FakeAdb()
        adb.save_reply = 'ERR save_wrong_game'
        with self.assertRaisesRegex(RuntimeError, 'save_wrong_game'):
            self.deploy(adb, save_archive=archive)
        self.assertFalse(adb.started)

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

    def test_same_apk_already_installed_skips_install(self):
        # 机上已是同一份 APK：不再 adb install（同版本重装在 vivo 上不弹确认也不返回、干等超时），照常直传、核哈希
        adb = FakeAdb()
        adb.digest = game_apk.sha256(self.apk)
        out = self.deploy(adb, mode='direct')
        self.assertFalse(any(x[0] == 'install' for x in adb.calls))
        self.assertTrue(json.loads((out / 'deployment.json').read_text())['startupAccepted'])
        self.assertTrue(adb.received)

    def test_direct_transfer_and_wake_before_install(self):
        # 直传：资源树逐字节进 App 的接收端；不碰共享存储（不 push、不出现 /sdcard）；LibraryActivity 只起一次且带 IMPORT_RECEIVE
        adb = FakeAdb()
        out = self.deploy(adb)
        install = next(i for i, x in enumerate(adb.calls) if x[0] == 'install')
        self.assertLess(adb.calls.index(('input', 'keyevent', 'KEYCODE_WAKEUP')), install)
        self.assertLess(adb.calls.index(('wm', 'dismiss-keyguard')), install)
        imports = [x for x in adb.calls if x[:3] == ('am', 'start', '-W') and 'org.s2arm.android.LibraryActivity' in ' '.join(x)]
        self.assertEqual(len(imports), 1)
        self.assertIn('org.s2arm.android.IMPORT_RECEIVE', imports[0])
        self.assertIn(('forward', 'tcp:0', f'localabstract:s2arm-import.org.s2arm.game.t{TITLE.lower()}'), adb.calls)
        self.assertTrue(any(x[:2] == ('forward', '--remove') for x in adb.calls))
        self.assertFalse(any(x[0] == 'push' or '/sdcard' in ' '.join(map(str, x)) for x in adb.calls))
        res = out / 'resources'
        want = {p.relative_to(res).as_posix(): p.read_bytes() for p in res.rglob('*') if p.is_file()}
        self.assertEqual(adb.received, want)
        self.assertTrue(adb.name.startswith('deploy-'))
        launches = [x for x in adb.calls if x[:3] == ('am', 'start', '-W') and 'org.s2arm.android.MainActivity' in ' '.join(x)]
        self.assertEqual(len(launches), 1)

    REMOTE = '/storage/emulated/0/S2Arm/Games/' + TITLE

    def test_in_place_pushes_to_convention_dir_then_registers(self):
        # 原地：整份推进 .<程序号>.part、推完换名成约定目录；给本包开「所有文件访问」；只发登记请求、不直传字节；照常启动
        adb = FakeAdb()
        out = self.deploy(adb, in_place=True)
        part = '/storage/emulated/0/S2Arm/Games/.' + TITLE + '.part'
        pushes = [x for x in adb.calls if x[0] == 'push']
        self.assertEqual(pushes, [('push', out / 'resources', part)])
        mv = adb.calls.index(('mv', part, self.REMOTE))
        self.assertLess(adb.calls.index(('rm', '-rf', part)), adb.calls.index(pushes[0]))
        self.assertLess(adb.calls.index(pushes[0]), adb.calls.index(('rm', '-rf', self.REMOTE)))
        self.assertLess(adb.calls.index(('rm', '-rf', self.REMOTE)), mv)
        package = f'org.s2arm.game.t{TITLE.lower()}'
        self.assertIn(('appops', 'set', package, 'MANAGE_EXTERNAL_STORAGE', 'allow'), adb.calls)
        self.assertEqual(adb.in_place, self.REMOTE)
        self.assertEqual(adb.received, {})
        receipt = json.loads((out / 'deployment.json').read_text())
        self.assertEqual(receipt['inPlace'], self.REMOTE)
        self.assertTrue(receipt['startupAccepted'])

    def test_in_place_same_content_is_not_pushed_again(self):
        # 设备上 index.tsv 与构建描述都相同：一个字节都不推；只有构建描述不同：只推它（覆盖到约定目录里）
        resources = self.root / 'resources'
        game_apk.resources(self.pack, self.apk, resources)
        for artifact_same in (True, False):
            with self.subTest(artifact_same=artifact_same):
                adb = FakeAdb()
                adb.remote = {f'{self.REMOTE}/index.tsv': deploy_game.digest(resources / 'index.tsv'),
                              f'{self.REMOTE}/game-build-artifact.json':
                                  deploy_game.digest(resources / 'game-build-artifact.json') if artifact_same else '0' * 64}
                work = self.root / f'work-{artifact_same}'; work.mkdir()
                deploy_game.deploy_resources(self.apk, resources, adb, work, require_current=False, in_place=True)
                pushes = [x for x in adb.calls if x[0] == 'push']
                want = [] if artifact_same else [('push', resources / 'game-build-artifact.json',
                                                  f'{self.REMOTE}/game-build-artifact.json')]
                self.assertEqual(pushes, want)
                self.assertFalse(any(x[0] == 'mv' for x in adb.calls))
                self.assertEqual(adb.in_place, self.REMOTE)

    def test_in_place_partial_refresh_is_red_before_push(self):
        # --refresh-existing 的资源目录没有 romfs：设备上又没有同一份内容，原地登记必缺文件，推之前就报
        with self.assertRaisesRegex(RuntimeError, '原地使用要整份资源'):
            self.deploy(FakeAdb(), in_place=True, refresh_existing=True)

    def test_in_place_refusal_is_red_and_never_starts(self):
        adb = FakeAdb(); adb.in_place_refused = True
        with self.assertRaisesRegex(RuntimeError, '原地登记被拒绝：.*romfs/data'):
            self.deploy(adb, in_place=True)
        self.assertFalse(any('MainActivity' in ' '.join(map(str, x)) for x in adb.calls))

    def test_no_start_imports_only(self):
        adb = FakeAdb()
        out = self.deploy(adb, start=False)
        self.assertFalse(json.loads((out / 'deployment.json').read_text())['startupAccepted'])
        self.assertFalse(any('MainActivity' in ' '.join(map(str, x)) for x in adb.calls))

    def test_transfer_waits_until_app_is_listening(self):
        # 设备端还没监听时 adb forward 先接 TCP 再断：前两次连接被断，第三次读到 READY 才传
        adb = FakeAdb(drop_first=2)
        out = self.deploy(adb)
        self.assertEqual(adb.connects, 3)
        self.assertEqual(len(adb.received), sum(1 for p in (out / 'resources').rglob('*') if p.is_file()))

    def test_app_refusal_is_red_and_never_starts(self):
        adb = FakeAdb(receive_reply='ERR bad path: ../x')
        with self.assertRaisesRegex(RuntimeError, 'App 拒收资源：ERR bad path'):
            self.deploy(adb)
        self.assertFalse(any('MainActivity' in ' '.join(map(str, x)) for x in adb.calls))
        self.assertTrue(any(x[:2] == ('forward', '--remove') for x in adb.calls))

    def test_push_timeout_scales_with_resource_size(self):
        # 期望按 30 MB/s、2 倍余量手算：21 GB → 21e9 / 30e6 × 2 = 1400 秒（+1 取整）；小包取下限 120；显式 --push-timeout 照用
        for size, want in ((21_000_000_000, 1401), (1_000_000, 120)):
            seen = []
            def fake_send(adb, package, resources, name, timeout, log):
                adb.name = name; seen.append(timeout)
            with self.subTest(size=size), patch.object(deploy_game, 'tree_bytes', return_value=size), \
                    patch.object(deploy_game, 'send_tree', fake_send):
                self.deploy(FakeAdb())
                self.assertEqual(seen, [want])
        seen = []
        def fake_send50(adb, package, resources, name, timeout, log):
            adb.name = name; seen.append(timeout)
        with patch.object(deploy_game, 'tree_bytes', return_value=21_000_000_000), \
                patch.object(deploy_game, 'send_tree', fake_send50):
            self.deploy(FakeAdb(), push_timeout=50)
            self.assertEqual(seen, [50])

    def test_import_wait_scales_with_resource_size(self):
        # 期望手算，同传输：17 GB → 17e9 / 30e6 × 2 = 1133.3 → 1134 秒；小包取 --timeout（180）；--timeout 更大时照用
        for size, timeout, want in ((17_000_000_000, 180, 1134), (1_000_000, 180, 180), (1_000_000, 3600, 3600)):
            with self.subTest(size=size, timeout=timeout), patch.object(deploy_game, 'tree_bytes', return_value=size):
                self.assertEqual(deploy_game.import_wait_for(self.pack, timeout), want)

    def test_import_timeout_leaves_running_import_alone(self):
        # 等回执超时：不强停应用，报错里说清楚
        adb = FakeAdb(slow_import=True)
        with patch.object(deploy_game, 'import_wait_for', return_value=1), \
                patch.object(deploy_game, 'send_tree'), \
                patch.object(deploy_game.time, 'monotonic', side_effect=[0] * 2 + [5] * 50), \
                patch.object(deploy_game.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, '等待导入超时.*没有强停应用'):
                self.deploy(adb)
        started = max(i for i, x in enumerate(adb.calls) if x[:3] == ('am', 'start', '-W') and 'IMPORT_RECEIVE' in ' '.join(map(str, x)))
        self.assertFalse(any(x[:2] == ('am', 'force-stop') for x in adb.calls[started + 1:]))

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

    def test_wrong_installed_hash_stops_before_transfer(self):
        adb = FakeAdb(wrong_hash=True)
        with self.assertRaisesRegex(RuntimeError, '安装后的 APK 哈希'):
            self.deploy(adb)
        self.assertFalse(any(x[0] == 'forward' for x in adb.calls))

    def test_refused_refresh_has_no_success_receipt(self):
        with self.assertRaisesRegex(RuntimeError, '去掉 --refresh-existing'):
            self.deploy(FakeAdb(refused=True), refresh_existing=True)
        self.assertEqual(list((self.root / 'out').glob('*/deployment.json')), [])

    def test_startup_before_this_import_is_not_success(self):
        # 直传换成替身（它也走 time.monotonic）；导入轮询读到回执即停，启动轮询处跳到超过 deadline
        adb0 = FakeAdb(stale_start=True); adb0.name = None
        def fake_send(adb, package, resources, name, timeout, log):
            adb.name = name
        with patch.object(deploy_game, 'send_tree', fake_send), \
                patch.object(deploy_game.time, 'monotonic', side_effect=[0] * 3 + [5] * 10), \
                patch.object(deploy_game.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, '超时'):
                self.deploy(adb0, timeout=1)
        self.assertEqual(list((self.root / 'out').glob('*/deployment.json')), [])

    def test_corrupt_metadata_refused(self):
        (self.pack / 'ctl/control.nacp').write_bytes(b'damaged')
        with self.assertRaises(SystemExit):
            game_apk.resources(self.pack, self.apk, self.root / 'refresh', refresh_existing=True)

    def test_output_must_not_replace_input(self):
        with self.assertRaises(SystemExit):
            game_apk.resources(self.pack, self.apk, self.pack, refresh_existing=True)
        self.assertEqual((self.pack / 'modules/main.patched.bin').read_bytes(), b'code')

    # 出包的资源包向上一版取共用文件（game_apk.py --resources --reuse-from）：
    # ext4 没有 reflink，每个 release 原先各存一整份 RomFS（17 GiB）；只读文件在两版之间共用同一个 inode。

    def seal_romfs(self):
        """RomFS 共享存储里的文件是 0444（pack/roots.sh 去写位）；合成包里的 romfs/data 同样封存。"""
        (self.pack / 'romfs/data').chmod(0o444)

    def test_reuse_links_unchanged_read_only_files_from_the_previous_package(self):
        self.seal_romfs()
        first, second = self.root / 'first', self.root / 'second'
        game_apk.resources(self.pack, self.apk, first)
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            game_apk.resources(self.pack, self.apk, second, reuse_from=first)
        shared, linked = first / 'romfs/data', second / 'romfs/data'
        self.assertEqual((shared.stat().st_ino, shared.stat().st_nlink), (linked.stat().st_ino, 2))
        self.assertEqual(linked.read_bytes(), b'synthetic resource')
        for name in ('exefs/main.npdm', 'ctl/control.nacp'):   # 可写的小文件各自独立
            self.assertNotEqual((first / name).stat().st_ino, (second / name).stat().st_ino, name)
            self.assertEqual((second / name).stat().st_nlink, 1, name)
        for name in ('index.tsv', 'manifest.json', 'game-build-artifact.json'):   # 复用不改任何一个字节
            self.assertEqual((first / name).read_bytes(), (second / name).read_bytes(), name)
        self.assertIn('reused 1 of 3 files', printed.getvalue())   # 共用了几个要报出来，不静默

    def test_reuse_copies_when_the_previous_file_changed_after_it_was_indexed(self):
        self.seal_romfs()
        first, second = self.root / 'first', self.root / 'second'
        game_apk.resources(self.pack, self.apk, first)
        old = first / 'romfs/data'
        old.chmod(0o644)
        old.write_bytes(b'tampered resource!')   # 同大小、仍只读，上一版 index 里的哈希没变
        old.chmod(0o444)
        game_apk.resources(self.pack, self.apk, second, reuse_from=first)
        new = second / 'romfs/data'
        self.assertEqual(new.read_bytes(), b'synthetic resource')   # 新包仍按自己的 index 校验
        self.assertNotEqual(old.stat().st_ino, new.stat().st_ino)
        self.assertEqual(old.read_bytes(), b'tampered resource!')   # 上一版原样不动
        self.assertEqual(old.stat().st_nlink, 1)

    def set_pack_file(self, name, data):
        """改输入包里的一个文件并重写它的 index / manifest：新一版游戏出包时的样子。"""
        self.files[name] = data
        path = self.pack / name
        path.chmod(0o644)
        path.write_bytes(data)
        index = ''.join(f'{n}\t{len(d)}\t{hashlib.sha256(d).hexdigest()}\n' for n, d in sorted(self.files.items())).encode()
        (self.pack / 'index.tsv').write_bytes(index)
        (self.pack / 'manifest.json').write_text(json.dumps(dict(version=2, index_sha256=hashlib.sha256(index).hexdigest())))

    def test_reuse_skips_a_file_whose_content_changed_in_the_new_package(self):
        self.seal_romfs()
        first, second = self.root / 'first', self.root / 'second'
        game_apk.resources(self.pack, self.apk, first)
        self.set_pack_file('romfs/data', b'updated resource!!')   # 同大小；新一版游戏改了这个文件
        self.seal_romfs()
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            game_apk.resources(self.pack, self.apk, second, reuse_from=first)
        old, new = first / 'romfs/data', second / 'romfs/data'
        self.assertEqual(new.read_bytes(), b'updated resource!!')
        self.assertEqual(old.read_bytes(), b'synthetic resource')
        self.assertNotEqual(old.stat().st_ino, new.stat().st_ino)
        self.assertEqual((old.stat().st_nlink, new.stat().st_nlink), (1, 1))
        self.assertIn('reused 0 of 3 files', printed.getvalue())

    def test_reuse_never_shares_a_writable_file(self):
        first, second = self.root / 'first', self.root / 'second'
        game_apk.resources(self.pack, self.apk, first)   # romfs/data 是 0644
        game_apk.resources(self.pack, self.apk, second, reuse_from=first)
        a, b = first / 'romfs/data', second / 'romfs/data'
        self.assertNotEqual(a.stat().st_ino, b.stat().st_ino)
        self.assertEqual((a.stat().st_nlink, b.stat().st_nlink), (1, 1))

    def test_reuse_falls_back_to_a_copy_when_the_link_is_refused(self):
        self.seal_romfs()
        first = self.root / 'first'
        game_apk.resources(self.pack, self.apk, first)
        for code in (errno.EXDEV, errno.EPERM, errno.EMLINK):   # 跨盘 / 文件系统或内核不许 / 链接数到顶
            with self.subTest(errno=errno.errorcode[code]):
                out = self.root / f'second-{code}'
                with patch.object(game_apk.os, 'link', side_effect=OSError(code, 'link refused')):
                    game_apk.resources(self.pack, self.apk, out, reuse_from=first)
                self.assertEqual((out / 'romfs/data').read_bytes(), b'synthetic resource')
                self.assertEqual((out / 'romfs/data').stat().st_nlink, 1)

    def test_reuse_from_an_unusable_package_is_ignored_with_a_note(self):
        self.seal_romfs()
        broken = self.root / 'broken'
        broken.mkdir()
        (broken / 'manifest.json').write_text('not json')
        for label, previous in (('missing', self.root / 'nope'), ('broken', broken)):
            with self.subTest(label):
                out, printed = self.root / f'out-{label}', io.StringIO()
                with contextlib.redirect_stdout(printed):
                    game_apk.resources(self.pack, self.apk, out, reuse_from=previous)
                self.assertIn('not reusing', printed.getvalue())
                self.assertEqual((out / 'romfs/data').read_bytes(), b'synthetic resource')
                self.assertEqual((out / 'romfs/data').stat().st_nlink, 1)

    def test_reuse_from_the_package_being_replaced_is_ignored(self):
        self.seal_romfs()
        out, printed = self.root / 'out', io.StringIO()
        game_apk.resources(self.pack, self.apk, out)
        with contextlib.redirect_stdout(printed):
            game_apk.resources(self.pack, self.apk, out, reuse_from=out)   # 原地替换：上一版随 rmtree 一起没了
        self.assertIn('not reusing', printed.getvalue())
        self.assertEqual((out / 'romfs/data').read_bytes(), b'synthetic resource')

    def test_command_line_takes_reuse_from_before_the_positional_arguments(self):
        self.seal_romfs()
        first, second = self.root / 'first', self.root / 'second'
        game_apk.resources(self.pack, self.apk, first)
        argv = ['game_apk.py', '--resources', '--reuse-from', str(first), str(self.pack), str(self.apk), str(second)]
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
            game_apk.main()
        self.assertEqual((first / 'romfs/data').stat().st_ino, (second / 'romfs/data').stat().st_ino)
        missing = ['game_apk.py', '--resources', str(self.pack), str(self.apk), str(self.root / 'third'), '--reuse-from']
        with patch.object(sys, 'argv', missing), self.assertRaises(SystemExit):   # 缺目录：报用法，不当位置参数吞掉
            game_apk.main()


if __name__ == '__main__':
    unittest.main()
