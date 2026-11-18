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
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import deploy_game
import game_apk
import resident_warm

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


class ResidentAdb(FakeAdb):
    """Resident fixture: model current.json, revision hashes and an import receipt."""
    def __init__(self, resources):
        super().__init__()
        self.artifact_bytes = (resources / 'game-build-artifact.json').read_bytes()
        self.new_artifact = json.loads(self.artifact_bytes)
        self.new_sha = hashlib.sha256(self.artifact_bytes).hexdigest()
        self.old_artifact = json.loads(self.artifact_bytes)
        self.old_artifact['apk']['sha256'] = 'd' * 64
        self.old_bytes = json.dumps(self.old_artifact).encode()
        self.old_sha = hashlib.sha256(self.old_bytes).hexdigest()
        self.digest = 'd' * 64
        self.root = self.new_artifact['contentRoot']
        self.current = dict(version=1, titleId=TITLE, contentRoot=self.root,
                            revisionDir='revisions/' + self.root,
                            artifact='old.json', artifactSha256=self.old_sha)
        self.receipt = (self.root, self.new_sha)
        self.hash_calls = 0
        self.fail_hash_call = 0
        self.corrupt_current_after_import = False
        self.empty_pid = False
        self.suppress_game_receipt = False
        self.raw_import_log = False
        self.date_calls = 0

    def installed_hash(self, package):
        self.hash_calls += 1
        if self.hash_calls == self.fail_hash_call:
            raise RuntimeError('device hash unavailable')
        return super().installed_hash(package)

    def shell(self, *args):
        if args[:2] == ('date', '+%s%3N'):
            self.calls.append(args)
            self.date_calls += 1
            return '1700000000000' if self.date_calls == 1 else '1700000002000'
        if args[0] == 'logcat' and '--pid=777' in args:
            self.calls.append(args)
            if self.suppress_game_receipt:
                return ''
            return (f'1700000001.250 777 888 I S2Arm: '
                    f'Game startup accepted: {TITLE} session 7 consoleMode handheld')
        if args[0] == 'pidof':
            self.calls.append(args)
            return '' if self.empty_pid else '777'
        if len(args) == 5 and args[0] == 'run-as' and args[2:4] == ('sh', '-c'):
            self.calls.append(args)
            if 'echo PRESENT' in args[4] and 'echo ABSENT' in args[4]:
                return 'ABSENT'
            raise RuntimeError('unexpected cache command')
        if len(args) == 4 and args[0] == 'run-as':
            self.calls.append(args)
            action, path = args[2:]
            if action == 'cat' and path.endswith('/current.json'):
                return json.dumps(self.current)
            if action == 'sha256sum' and path.endswith('/index.tsv'):
                return self.root[7:] + '  ' + path
            if action == 'sha256sum' and path.endswith('/' + self.current['artifact']):
                return self.current['artifactSha256'] + '  ' + path
            if action == 'cat' and path.endswith('/' + self.current['artifact']):
                return (self.artifact_bytes if self.current['artifact'] == 'new.json'
                        else self.old_bytes).decode()
            raise RuntimeError('missing device file')
        result = super().shell(*args)
        if args[0] == 'logcat' and self.name and not self.refused and not self.slow_import:
            self.current.update(artifact='new.json', artifactSha256=(
                'f' * 64 if self.corrupt_current_after_import else self.new_sha))
        if args[0] == 'logcat' and not self.raw_import_log:
            return '\n'.join('I/S2Arm   (  777): ' + line for line in result.splitlines())
        return result


class GenericResidentAdb(FakeAdb):
    def __init__(self, resources):
        super().__init__()
        self.artifact_bytes = (resources / 'game-build-artifact.json').read_bytes()
        self.artifact = json.loads(self.artifact_bytes)
        self.artifact_sha = hashlib.sha256(self.artifact_bytes).hexdigest()
        self.root = self.artifact['contentRoot']
        self.current = dict(version=1, titleId=TITLE, contentRoot=self.root,
                            revisionDir='revisions/' + self.root,
                            artifact='game-build-artifact.json', artifactSha256=self.artifact_sha)
        self.digest = 'd' * 64
        self.log_epoch = '1700000001.250'
        self.switch_pid = False
        self.pid_calls = 0
        self.date_calls = 0
        self.update_fingerprint_on_start = False

    def shell(self, *args):
        self.calls.append(args)
        if args[:2] == ('date', '+%s%3N'):
            self.date_calls += 1
            return '1700000000000' if self.date_calls == 1 else '1700000002000'
        if args[0] == 'pidof':
            if not self.started:
                raise RuntimeError('pidof: absent')
            self.pid_calls += 1
            return '778' if self.switch_pid and self.pid_calls > 1 else '777'
        if args[:4] == ('am', 'start', '-W', '-n'):
            self.started = True
            return 'Status: ok'
        if args[0] == 'logcat':
            if not self.started:
                return ''
            if self.update_fingerprint_on_start:
                self.current['fingerprint'] = {'version': 1, 'sha256': 'f' * 64}
            return (f'{self.log_epoch} 777 888 I S2Arm: '
                    f'Game startup accepted: {TITLE} session 7 consoleMode handheld')
        if len(args) == 5 and args[0] == 'run-as' and args[2:4] == ('sh', '-c'):
            if 'echo PRESENT' in args[4] and 'echo ABSENT' in args[4]:
                return 'ABSENT'
            raise RuntimeError('unexpected cache command')
        if len(args) == 4 and args[0] == 'run-as':
            action, path = args[2:]
            if action == 'cat' and path.endswith('/current.json'):
                return json.dumps(self.current)
            if action == 'sha256sum' and path.endswith('/index.tsv'):
                return self.root[7:] + '  ' + path
            if action == 'sha256sum' and path.endswith('/' + self.current['artifact']):
                return self.current['artifactSha256'] + '  ' + path
            if action == 'cat' and path.endswith('/' + self.current['artifact']):
                return self.artifact_bytes.decode()
        raise RuntimeError('unsupported fake adb shell call')


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


class PidofSemanticsTest(unittest.TestCase):
    def test_pidof_requires_consistent_rc_and_numeric_output(self):
        adb = deploy_game.Adb('test-device')
        for rc, out in ((0, ''), (0, 'not-a-pid'), (1, '42')):
            with self.subTest(rc=rc, out=out), patch.object(
                    deploy_game.subprocess, 'run',
                    return_value=subprocess.CompletedProcess([], rc, out, '')):
                with self.assertRaises(RuntimeError):
                    adb.running('test.package')

    def test_pidof_absent_and_present(self):
        adb = deploy_game.Adb('test-device')
        with patch.object(deploy_game.subprocess, 'run',
                          return_value=subprocess.CompletedProcess([], 1, '', '')):
            self.assertFalse(adb.running('test.package'))
        with patch.object(deploy_game.subprocess, 'run',
                          return_value=subprocess.CompletedProcess([], 0, '42 43\n', '')):
            self.assertTrue(adb.running('test.package'))


class CacheInventoryTest(unittest.TestCase):
    class LocalAdb:
        def __init__(self, root):
            self.root = root
            self.stat = shutil.which('gstat') or (shutil.which('stat') if sys.platform != 'darwin' else None)

        def shell(self, *args):
            if args[:4] == ('run-as', 'test.package', 'sh', '-c') and len(args) == 5:
                # Execute the actual find/test expression. macOS stat has a
                # different CLI; gstat matches Android's -c %s/%n contract.
                command = args[4].replace('-exec stat -c', '-exec ' + shlex.quote(self.stat) + ' -c')
                result = subprocess.run(['/bin/sh', '-c', command], cwd=self.root,
                                        text=True, capture_output=True)
                if result.returncode:
                    raise RuntimeError(result.stderr)
                return result.stdout.strip()
            if args[:3] == ('run-as', 'test.package', 'sha256sum') and len(args) == 4:
                path = self.root / args[3]
                if not path.is_file():
                    raise RuntimeError('missing file')
                return game_apk.sha256(path) + '  ' + args[3]
            raise RuntimeError('unsupported fake adb command')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.adb = self.LocalAdb(self.root)
        if not self.adb.stat:
            self.skipTest('GNU stat is required to model Android stat -c')
        self.cache = (self.root / 'files/user/0/saves' / TITLE /
                      'ryujinx-android/direct-cache/nvn-vk')

    def test_absent_cache_is_distinct_from_empty_existing_cache(self):
        self.assertEqual(resident_warm.inventory_cache(self.adb, 'test.package', TITLE)['state'], 'absent')
        self.cache.mkdir(parents=True)
        result = resident_warm.inventory_cache(self.adb, 'test.package', TITLE)
        self.assertEqual(result, {'state': 'present', 'files': {}, 'sample': {}})

    def test_sorted_sizes_and_first_middle_last_hashes(self):
        self.cache.mkdir(parents=True)
        for name in 'abcde':
            (self.cache / name).write_bytes(name.encode() * (ord(name) - 96))
        result = resident_warm.inventory_cache(self.adb, 'test.package', TITLE)
        self.assertEqual(result['files'], {'a': 1, 'b': 2, 'c': 3, 'd': 4, 'e': 5})
        self.assertEqual(set(result['sample']), {'a', 'c', 'e'})
        self.assertEqual(result['sample']['c'], hashlib.sha256(b'ccc').hexdigest())

    def test_symlink_in_cache_is_rejected(self):
        self.cache.mkdir(parents=True)
        (self.cache / 'target').write_bytes(b'content')
        (self.cache / 'link').symlink_to('target')
        with self.assertRaisesRegex(ValueError, '链接'):
            resident_warm.inventory_cache(self.adb, 'test.package', TITLE)

    def test_symlink_at_cache_root_is_rejected(self):
        self.cache.parent.mkdir(parents=True)
        target = self.root / 'unrelated'
        target.mkdir()
        self.cache.symlink_to(target)
        with self.assertRaisesRegex(ValueError, '缓存路径'):
            resident_warm.inventory_cache(self.adb, 'test.package', TITLE)


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

    def test_resident_mismatched_root_refuses_before_install_or_import(self):
        resources = self.root / 'refresh'
        game_apk.resources(self.pack, self.apk, resources, refresh_existing=True)
        adb = FakeAdb()
        calls = []
        current = json.dumps(dict(version=1, titleId=TITLE,
                                  contentRoot='sha256-' + '0' * 64,
                                  revisionDir='revisions/sha256-' + '0' * 64,
                                  artifact='game-build-artifact.json',
                                  artifactSha256='a' * 64))
        shell = adb.shell
        def with_current(*args):
            if args[:3] == ('run-as', 'org.s2arm.game.t0100000000abcdef', 'cat'):
                return current
            return shell(*args)
        adb.shell = with_current
        with self.assertRaisesRegex(ValueError, '内容根'):
            self.resident_deploy(self.apk, resources, adb, self.root / 'resident',
                                 lambda *args: calls.append(args))
        self.assertEqual(calls, [])
        self.assertFalse(any(call[0] in ('install', 'forward') for call in adb.calls))
        self.assertFalse(any('start' in call for call in adb.calls))

    def resident_deploy(self, *args, **kwargs):
        kwargs.setdefault('expected_package', 'org.s2arm.game.t0100000000abcdef')
        return resident_warm.deploy(*args, **kwargs)

    def resident_fixture(self):
        resources = self.root / 'refresh'
        game_apk.resources(self.pack, self.apk, resources, refresh_existing=True)
        return resources, ResidentAdb(resources)

    def generic_fixture(self):
        self.identity_mock.return_value = ('org.s2arm.android.w2', 1, True)
        with zipfile.ZipFile(self.apk, 'w') as z:
            z.writestr('assets/container.txt', b'synthetic container')
        resources = self.root / 'generic'
        (resources / 'ctl').mkdir(parents=True)
        (resources / 'ctl/control.nacp').write_bytes(self.files['ctl/control.nacp'])
        entries = dict(self.files)
        entries['modules/main.fixups.tsv'] = b'fixups'
        entries['modules/main.gates.tsv'] = b'gates'
        index = ''.join(f'{name}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}\n'
                        for name, data in sorted(entries.items())).encode()
        index_sha = hashlib.sha256(index).hexdigest()
        (resources / 'index.tsv').write_bytes(index)
        (resources / 'manifest.json').write_text(json.dumps(
            dict(version=2, files=len(entries), total=sum(map(len, entries.values())), index_sha256=index_sha)))
        artifact = dict(titleId=TITLE, contentRoot='sha256-' + index_sha,
                        package={'manifest': 'manifest.json', 'indexSha256': index_sha,
                                 'files': len(entries), 'bytes': sum(map(len, entries.values()))},
                        modules=[{'name': 'main', 'source': 'revision',
                                  'image': 'modules/main.patched.bin',
                                  'fixups': 'modules/main.fixups.tsv',
                                  'gates': 'modules/main.gates.tsv'}])
        (resources / 'game-build-artifact.json').write_text(json.dumps(artifact))
        return resources, GenericResidentAdb(resources)

    def test_resident_generic_revision_reuses_without_import_and_binds_pid(self):
        resources, adb = self.generic_fixture()
        calls = []
        def install(apk, package):
            calls.append((apk, package))
            adb.digest = game_apk.sha256(apk)
        receipt = resident_warm.deploy_container(
            self.apk, resources, adb, self.root / 'container', install,
            expected_package='org.s2arm.android.w2', expected_prior_apk='d' * 64,
            expected_signer='a' * 64, expected_title=TITLE)
        self.assertIsInstance(receipt, dict)
        self.assertEqual(receipt['pid'], 777)
        self.assertEqual(receipt['session'], 7)
        self.assertEqual(len(calls), 1)
        self.assertFalse(any(call[0] in ('install', 'forward') for call in adb.calls))
        self.assertTrue((self.root / 'container/resident.json').is_file())

    def generic_deploy(self, resources, adb, install, **kwargs):
        return resident_warm.deploy_container(
            self.apk, resources, adb, self.root / 'container', install,
            expected_package='org.s2arm.android.w2', expected_prior_apk='d' * 64,
            expected_signer='a' * 64, expected_title=TITLE, **kwargs)

    def test_resident_container_metadata_copies_only_identity_files(self):
        pack, _ = self.generic_fixture()
        (pack / 'romfs').mkdir()
        (pack / 'romfs/data').write_bytes(b'synthetic payload')
        out = self.root / 'identity'
        self.assertEqual(resident_warm.container_metadata(pack, out), out)
        self.assertEqual({p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file()},
                         {'manifest.json', 'index.tsv', 'game-build-artifact.json', 'ctl/control.nacp'})
        self.assertFalse((out / 'romfs').exists())
        with self.assertRaisesRegex(ValueError, '不能覆盖'):
            resident_warm.container_metadata(pack, pack / 'child')

    def test_resident_container_metadata_rejects_symlink(self):
        pack, _ = self.generic_fixture()
        src = pack / 'index.tsv'
        target = self.root / 'index-copy'
        target.write_bytes(src.read_bytes())
        src.unlink()
        src.symlink_to(target)
        with self.assertRaisesRegex(ValueError, '链接'):
            resident_warm.container_metadata(pack, self.root / 'identity')

    def test_resident_generic_wrong_revision_refuses_before_install(self):
        resources, adb = self.generic_fixture()
        adb.current['contentRoot'] = 'sha256-' + '0' * 64
        called = []
        with self.assertRaisesRegex(ValueError, '内容根'):
            self.generic_deploy(resources, adb, lambda *args: called.append(args))
        self.assertFalse(called)
        self.assertFalse(adb.started)

    def test_resident_generic_invalid_package_metadata_refuses_before_install(self):
        resources, _ = self.generic_fixture()
        path = resources / 'game-build-artifact.json'
        artifact = json.loads(path.read_text())
        artifact['package']['manifest'] = 'wrong.json'
        path.write_text(json.dumps(artifact))
        adb = GenericResidentAdb(resources)
        with self.assertRaisesRegex(ValueError, '资源身份'):
            self.generic_deploy(resources, adb, lambda *args: self.fail('must not install'))
        self.assertFalse(adb.calls)

    def test_resident_generic_allows_fingerprint_refresh_but_keeps_revision(self):
        resources, adb = self.generic_fixture()
        adb.update_fingerprint_on_start = True
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        receipt = self.generic_deploy(resources, adb, install)
        self.assertEqual(receipt['pid'], 777)
        self.assertIn('fingerprint', adb.current)

    def test_resident_generic_wrong_prior_apk_refuses_before_install(self):
        resources, adb = self.generic_fixture()
        adb.digest = 'e' * 64
        called = []
        with self.assertRaisesRegex(ValueError, '原 APK'):
            self.generic_deploy(resources, adb, lambda *args: called.append(args))
        self.assertFalse(called)
        self.assertFalse(adb.started)

    def test_resident_generic_apk_asset_module_does_not_use_revision_path(self):
        resources, adb = self.generic_fixture()
        path = resources / 'game-build-artifact.json'
        artifact = json.loads(path.read_text())
        artifact['modules'][0]['source'] = 'apk-asset'
        path.write_text(json.dumps(artifact))
        with self.assertRaisesRegex(ValueError, 'revision 模块'):
            self.generic_deploy(resources, adb, lambda *args: self.fail('must not install'))
        self.assertFalse(adb.calls)

    def test_resident_generic_duplicate_index_refused_before_device(self):
        resources, adb = self.generic_fixture()
        index = resources / 'index.tsv'
        lines = index.read_text().splitlines()
        index.write_text('\n'.join(lines + [lines[0]]) + '\n')
        digest = game_apk.sha256(index)
        manifest_path = resources / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['index_sha256'] = digest
        manifest_path.write_text(json.dumps(manifest))
        artifact_path = resources / 'game-build-artifact.json'
        artifact = json.loads(artifact_path.read_text())
        artifact['contentRoot'] = 'sha256-' + digest
        artifact['package']['indexSha256'] = digest
        artifact_path.write_text(json.dumps(artifact))
        with self.assertRaisesRegex(ValueError, '索引条目'):
            self.generic_deploy(resources, adb, lambda *args: self.fail('must not install'))
        self.assertFalse(adb.calls)

    def test_resident_generic_stale_log_does_not_accept_startup(self):
        resources, adb = self.generic_fixture()
        adb.log_epoch = '1699999999.999'
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with patch.object(resident_warm.time, 'monotonic', side_effect=[0, 0, 2]), \
                patch.object(resident_warm.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, '启动回执'):
                self.generic_deploy(resources, adb, install, timeout=1)
        self.assertFalse((self.root / 'container/resident.json').exists())

    def test_resident_generic_pid_change_is_not_accepted(self):
        resources, adb = self.generic_fixture()
        adb.switch_pid = True
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with self.assertRaisesRegex(ValueError, 'PID 已变化'):
            self.generic_deploy(resources, adb, install)
        self.assertFalse((self.root / 'container/resident.json').exists())

    def test_resident_generic_future_dated_log_does_not_accept_startup(self):
        resources, adb = self.generic_fixture()
        adb.log_epoch = '1700000999.000'
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with self.assertRaisesRegex(ValueError, '日志时间'):
            self.generic_deploy(resources, adb, install)
        self.assertFalse((self.root / 'container/resident.json').exists())

    def test_resident_generic_changed_cache_stops_before_start(self):
        resources, adb = self.generic_fixture()
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with patch.object(resident_warm, 'inventory_cache', side_effect=[
                {'state': 'present', 'files': {'a': 1}, 'sample': {'a': 'a' * 64}},
                {'state': 'present', 'files': {'a': 2}, 'sample': {'a': 'b' * 64}}]):
            with self.assertRaisesRegex(ValueError, '缓存'):
                self.generic_deploy(resources, adb, install)
        self.assertFalse(adb.started)

    def test_resident_matching_root_installs_once_and_verifies_receipt(self):
        resources, adb = self.resident_fixture()
        installs = []
        def install(apk, package):
            installs.append((apk, package))
            adb.digest = game_apk.sha256(apk)
        work = self.root / 'resident'
        receipt = self.resident_deploy(self.apk, resources, adb, work, install)
        self.assertEqual(len(installs), 1)
        self.assertEqual(receipt['contentRoot'], adb.root)
        self.assertTrue((work / 'resident.json').is_file())
        timing = json.loads((work / 'timing.json').read_text())
        stamps = [timing[k] for k in ('frozen_ms', 'preflight_ms', 'install_verified_ms',
                                      'refresh_verified_ms', 'startup_verified_ms')]
        self.assertEqual(stamps, sorted(stamps))
        self.assertEqual(set(adb.received),
                         {'manifest.json', 'index.tsv', 'game-build-artifact.json', 'ctl/control.nacp'})
        self.assertFalse(any(call[0] == 'install' for call in adb.calls))
        start = next(call for call in adb.calls if 'MainActivity' in ' '.join(map(str, call)))
        self.assertIn(('s2arm.graphics_mode', 'direct'),
                      list(zip(start, start[1:])))
        self.assertIn(('s2arm.title_id', TITLE), list(zip(start, start[1:])))

    def test_resident_hash_read_failure_never_falls_back_to_ordinary_install(self):
        resources, adb = self.resident_fixture()
        adb.fail_hash_call = 3
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with self.assertRaisesRegex(RuntimeError, '禁止普通安装'):
            self.resident_deploy(self.apk, resources, adb, self.root / 'resident', install)
        self.assertFalse(any(call[0] == 'install' for call in adb.calls))
        self.assertFalse((self.root / 'resident/resident.json').exists())

    def test_resident_wrong_receipt_never_publishes_success(self):
        resources, adb = self.resident_fixture()
        adb.receipt = ('sha256-' + 'e' * 64, adb.new_sha)
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        work = self.root / 'resident'
        with self.assertRaisesRegex(RuntimeError, '导入身份不匹配'):
            self.resident_deploy(self.apk, resources, adb, work, install)
        self.assertFalse((work / 'resident.json').exists())
        self.assertFalse(list(work.rglob('deployment.json')))

    def test_resident_per_game_requires_fresh_game_pid_receipt(self):
        resources, adb = self.resident_fixture()
        adb.suppress_game_receipt = True
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        def prepare():
            monotonic = patch.object(resident_warm.time, 'monotonic', side_effect=[0, 0, 2])
            sleep = patch.object(resident_warm.time, 'sleep')
            monotonic.start(); sleep.start()
            self.addCleanup(monotonic.stop); self.addCleanup(sleep.stop)
        work = self.root / 'resident'
        with self.assertRaisesRegex(RuntimeError, '启动回执'):
            self.resident_deploy(self.apk, resources, adb, work, install, prepare, timeout=1)
        self.assertFalse((work / 'resident.json').exists())

    def test_resident_import_log_without_pid_is_not_trusted(self):
        _, adb = self.resident_fixture()
        adb.name = 'deploy-nonce'
        adb.raw_import_log = True
        wrapped = resident_warm._NoInstall(adb, 'org.s2arm.game.t0100000000abcdef')
        self.assertEqual(wrapped.shell('logcat', '-d', '-v', 'brief', '-s', 'S2Arm:I'), '')

    def test_resident_freezes_inputs_before_installer_can_change_sources(self):
        resources, adb = self.resident_fixture()
        original = game_apk.sha256(self.apk)
        def install(frozen, package):
            self.assertNotEqual(frozen, self.apk)
            self.apk.write_bytes(b'changed while installing')
            (resources / 'game-build-artifact.json').write_text('{}')
            adb.digest = game_apk.sha256(frozen)
        work = self.root / 'resident'
        receipt = self.resident_deploy(self.apk, resources, adb, work, install)
        self.assertEqual(receipt['apkSha256'], original)
        self.assertEqual(game_apk.sha256(work / 'frozen/game.apk'), original)

    def test_resident_bad_current_after_import_stops_before_game_start(self):
        resources, adb = self.resident_fixture()
        adb.corrupt_current_after_import = True
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with self.assertRaisesRegex(ValueError, '刷新后身份不匹配'):
            self.resident_deploy(self.apk, resources, adb, self.root / 'resident', install)
        self.assertFalse(any('MainActivity' in ' '.join(map(str, call)) for call in adb.calls))

    def test_resident_cache_changes_stop_before_game_start(self):
        resources, adb = self.resident_fixture()
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with patch.object(resident_warm, 'inventory_cache', side_effect=[
                {'state': 'present', 'files': {'one': 10}, 'sample': {'one': 'a' * 64}},
                {'state': 'present', 'files': {'one': 11}, 'sample': {'one': 'b' * 64}}]):
            with self.assertRaisesRegex(ValueError, '缓存'):
                self.resident_deploy(self.apk, resources, adb, self.root / 'resident', install)
        self.assertFalse(any('MainActivity' in ' '.join(map(str, call)) for call in adb.calls))

    def test_resident_refresh_cache_changes_stop_before_game_start(self):
        resources, adb = self.resident_fixture()
        unchanged = {'state': 'present', 'files': {'one': 10}, 'sample': {'one': 'a' * 64}}
        changed = {'state': 'present', 'files': {'one': 10}, 'sample': {'one': 'b' * 64}}
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with patch.object(resident_warm, 'inventory_cache', side_effect=[unchanged, unchanged, changed]):
            with self.assertRaisesRegex(ValueError, '刷新前后缓存'):
                self.resident_deploy(self.apk, resources, adb, self.root / 'resident', install)
        self.assertFalse(any('MainActivity' in ' '.join(map(str, call)) for call in adb.calls))

    def test_resident_same_apk_does_not_install_again(self):
        resources, adb = self.resident_fixture()
        adb.old_artifact['apk']['sha256'] = game_apk.sha256(self.apk)
        adb.old_bytes = json.dumps(adb.old_artifact).encode()
        adb.old_sha = hashlib.sha256(adb.old_bytes).hexdigest()
        adb.current['artifactSha256'] = adb.old_sha
        adb.digest = game_apk.sha256(self.apk)
        with patch.object(resident_warm, 'inventory_cache', return_value={'state': 'absent'}):
            calls = []
            self.resident_deploy(self.apk, resources, adb, self.root / 'resident',
                                 lambda *args: calls.append(args))
        self.assertEqual(calls, [])
        self.assertFalse(any(call[0] == 'install' for call in adb.calls))

    def test_resident_wrong_signer_refuses_before_device_mutation(self):
        resources, adb = self.resident_fixture()
        with patch.object(game_apk, 'apk_signer_sha256', return_value='b' * 64):
            with self.assertRaisesRegex(ValueError, '签名'):
                self.resident_deploy(self.apk, resources, adb, self.root / 'resident',
                                     lambda *args: self.fail('must not install'))
        self.assertFalse(adb.calls)

    def test_resident_requires_the_station_fixed_package(self):
        resources, adb = self.resident_fixture()
        with self.assertRaisesRegex(ValueError, '固定包名'):
            self.resident_deploy(self.apk, resources, adb, self.root / 'resident',
                                 lambda *args: self.fail('must not install'),
                                 expected_package='test.wrong.package')
        self.assertFalse(adb.calls)

    def test_resident_missing_import_process_is_not_accepted(self):
        resources, adb = self.resident_fixture()
        adb.empty_pid = True
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        with self.assertRaisesRegex(RuntimeError, '无法绑定导入进程'):
            self.resident_deploy(self.apk, resources, adb, self.root / 'resident', install)
        self.assertFalse(any('MainActivity' in ' '.join(map(str, call)) for call in adb.calls))

    def test_resident_invalid_revision_and_old_apk_identity_refuse(self):
        for mutation in ('revision', 'old_apk'):
            with self.subTest(mutation=mutation):
                resources = self.root / ('refresh-' + mutation)
                game_apk.resources(self.pack, self.apk, resources, refresh_existing=True)
                adb = ResidentAdb(resources)
                if mutation == 'revision':
                    adb.current['revisionDir'] = '../outside'
                else:
                    adb.digest = 'e' * 64
                called = []
                with self.assertRaises(ValueError):
                    self.resident_deploy(self.apk, resources, adb, self.root / ('resident-' + mutation),
                                         lambda *args: called.append(args))
                self.assertFalse(called)
                self.assertFalse(any(call[0] in ('install', 'forward') for call in adb.calls))

    def test_resident_rejects_symlinked_refresh_file_before_device(self):
        resources, adb = self.resident_fixture()
        control = resources / 'ctl/control.nacp'
        copied = self.root / 'other-control'
        copied.write_bytes(control.read_bytes())
        control.unlink()
        control.symlink_to(copied)
        with self.assertRaisesRegex(ValueError, '链接'):
            self.resident_deploy(self.apk, resources, adb, self.root / 'resident',
                                 lambda *args: self.fail('must not install'))
        self.assertFalse(adb.calls)

    def test_resident_prepares_session_once_before_game_start(self):
        resources, adb = self.resident_fixture()
        prepared = []
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        def prepare():
            self.assertFalse(adb.started)
            prepared.append(True)
        self.resident_deploy(self.apk, resources, adb, self.root / 'resident', install, prepare)
        self.assertEqual(prepared, [True])
        self.assertTrue(adb.started)

    def test_resident_session_preparation_failure_never_starts_game(self):
        resources, adb = self.resident_fixture()
        def install(apk, package):
            adb.digest = game_apk.sha256(apk)
        def prepare():
            raise RuntimeError('fixture prepare failed')
        work = self.root / 'resident'
        with self.assertRaisesRegex(RuntimeError, 'prepare failed'):
            self.resident_deploy(self.apk, resources, adb, work, install, prepare)
        self.assertFalse(adb.started)
        self.assertFalse((work / 'resident.json').exists())

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
