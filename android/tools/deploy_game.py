#!/usr/bin/env python3
"""部署签名游戏 APK 和绑定资源。默认导入后启动；--no-start 只导入。

--pack 接受 make_pack 输出并冻结资源；--resources 接受已绑定资源目录。
所有设备操作须由外部设备租约保护。
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time

import game_apk
from filecopy import copy_file

REPO = Path(__file__).resolve().parents[2]
IMPORT_EXTRA = 'org.s2arm.android.IMPORT_STAGING'


class Adb:
    def __init__(self, serial, adb='adb'):
        if not re.fullmatch(r'[A-Za-z0-9_.:-]+', serial):
            raise ValueError('设备序列号无效。')
        self.serial = serial
        self.argv = [*shlex.split(adb), '-s', serial]

    def call(self, *args, timeout=600):
        result = subprocess.run([*self.argv, *map(str, args)],
                                capture_output=True, text=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f'adb {args[0]} failed: {result.stdout.strip()} {result.stderr.strip()}')
        return result.stdout.strip()

    def shell(self, *args):
        return self.call('shell', shlex.join(map(str, args)))

    def running(self, package):
        for name in (package, package + ':game'):
            result = subprocess.run([*self.argv, 'shell', 'pidof', name],
                                    capture_output=True, text=True, timeout=120)
            if result.returncode not in (0, 1) or result.stderr.strip():
                raise RuntimeError('无法确认设备游戏进程状态：' + result.stderr.strip())
            if result.stdout.strip():
                return True
        return False

    def installed_hash(self, package):
        paths = self.shell('pm', 'path', package).splitlines()
        if len(paths) != 1 or not paths[0].startswith('package:'):
            raise RuntimeError('需要单体 APK；无法确定已安装 base APK 或发现 split APK')
        digest = self.shell('sha256sum', paths[0][len('package:'):]).split()
        if not digest:
            raise RuntimeError('无法计算已安装 APK 的 SHA-256')
        return digest[0]


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 << 20), b''):
            h.update(block)
    return h.hexdigest()


def validate(apk, resources):
    artifact_path = resources / 'game-build-artifact.json'
    artifact = json.loads(artifact_path.read_text())
    title, package = artifact['titleId'], artifact['apk']['packageName']
    actual_package, version_code, debug = game_apk.apk_identity(apk)
    game_apk.validate_apk_package(title, actual_package, debug)
    if package != actual_package or artifact['apk']['versionCode'] != version_code:
        raise ValueError('资源包的应用身份与 APK manifest 不匹配。')
    if digest(apk) != artifact['apk']['sha256']:
        raise ValueError('APK 与资源包不匹配。')
    index_hash = digest(resources / 'index.tsv')
    manifest = json.loads((resources / 'manifest.json').read_text())
    if artifact['contentRoot'] != 'sha256-' + index_hash or manifest.get('index_sha256') != index_hash:
        raise ValueError('资源包索引与身份不一致。')
    return artifact, digest(artifact_path)


def start_activity(adb, *args):
    output = adb.shell('am', 'start', '-W', *args)
    if 'Error:' in output or 'Exception' in output:
        raise RuntimeError('Activity 启动失败：' + output)


# 资源包推送超时按大小算（11-01：写死 600 秒，21 GB 的暗黑3 资源包必超时；vivo 10AF4N00TA002NC 实测 21 GB 推 715 秒 ≈ 30 MB/s，留 2 倍余量）。
#   下限只覆盖 adb 握手 / 建目录这类固定开销；命令行 --push-timeout 显式给了照用
PUSH_BYTES_PER_S = 30e6
PUSH_MARGIN = 2
PUSH_FLOOR_S = 120


def tree_bytes(path):
    path = Path(path)
    return path.stat().st_size if path.is_file() else sum(p.stat().st_size for p in path.rglob('*') if p.is_file())


def push_timeout_for(resources):
    return max(PUSH_FLOOR_S, int(tree_bytes(resources) / PUSH_BYTES_PER_S * PUSH_MARGIN) + 1)


def deploy_resources(apk, resources, adb, work, mode='auto', timeout=180, push_timeout=None,
                     start=True, refresh_existing=False, require_current=True):
    apk, resources, work = Path(apk), Path(resources), Path(work)
    artifact, artifact_hash = validate(apk, resources)
    title, package = artifact['titleId'], artifact['apk']['packageName']
    if adb.running(package):
        raise ValueError('设备上的游戏仍在运行，不能安装。')
    adb.shell('input', 'keyevent', 'KEYCODE_WAKEUP')
    adb.shell('wm', 'dismiss-keyguard')
    if 'Success' not in adb.call('install', '-r', apk):
        raise RuntimeError('设备未确认 APK 安装成功。')
    if adb.installed_hash(package) != artifact['apk']['sha256']:
        raise RuntimeError('安装后的 APK 哈希与冻结 APK 不符，停止部署')
    name = 'deploy-' + work.name
    external = '/sdcard/Android/data/' + package + '/files'
    pending, imported = external + '/.' + name, external + '/import/' + name
    library = package + '/org.s2arm.android.LibraryActivity'
    library_active = False
    try:
        # LibraryActivity creates its own import parent; no extra means no import.
        library_active = True
        start_activity(adb, '-n', library)
        adb.shell('am', 'force-stop', package)
        library_active = False
        adb.call('push', resources, pending, timeout=push_timeout or push_timeout_for(resources))
        adb.shell('chmod', '-R', 'u=rwX,g=rX,o=r', pending)
        adb.shell('find', pending, '-type', 'd', '-exec', 'chmod', 'o=x', '{}', '+')
        adb.shell('mv', pending, imported)
        since = adb.shell('date', '+%m-%d %H:%M:%S.000')
        library_active = True
        start_activity(adb, '-n', library, '--es', IMPORT_EXTRA, imported)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            logs = adb.shell('logcat', '-d', '-v', 'brief', '-T', since, '-s', 'S2Arm:I')
            (work / 'deploy.log').write_text(logs + '\n')
            if 'Game import of requested directory refused:' in logs or f'Game import of import/{name} refused:' in logs:
                raise RuntimeError('资源导入被拒绝' + ('；资源变化或首次部署请去掉 --refresh-existing' if refresh_existing else ''))
            imported_ok = any(f'from import/{name}' in line and any(
                text in line for text in ('Game installed:', 'Game already installed:', 'Game build artifact updated'))
                for line in logs.splitlines())
            if imported_ok:
                if require_current:
                    # 正式包不可调试，run-as 读不了 current.json；App 导入成功时在 logcat 打完整回执（GameLibrary「Game import receipt」）
                    receipt = next((line for line in logs.splitlines()
                                    if f'Game import receipt: {title} ' in line and line.endswith(f' from import/{name}')), None)
                    if receipt is None:
                        raise RuntimeError('导入成功但设备没打出导入回执，无法核身份。')
                    if f' contentRoot={artifact["contentRoot"]} ' not in receipt or f' artifactSha256={artifact_hash} ' not in receipt:
                        raise RuntimeError('设备导入身份不匹配。')
                break
            time.sleep(1)
        else:
            raise RuntimeError('等待导入超时；未生成成功回执')
    finally:
        original = sys.exc_info()[1]
        try:
            if library_active:
                adb.shell('am', 'force-stop', package)
            adb.shell('rm', '-rf', pending, imported)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            raise RuntimeError((f'导入失败：{original}；' if original else '') +
                               f'设备临时副本清理失败：{error}；路径 {pending} {imported}') from error
    if start:
        since = adb.shell('date', '+%m-%d %H:%M:%S.000')
        before = adb.shell('logcat', '-d', '-v', 'brief', '-T', since, '-s', 'S2Arm:I')
        prior = set(before.splitlines())
        launch = ['-n', package + '/org.s2arm.android.MainActivity']
        if mode != 'auto':
            launch += ['--es', 's2arm.graphics_mode', mode]
        start_activity(adb, *launch)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            logs = adb.shell('logcat', '-d', '-v', 'brief', '-T', since, '-s', 'S2Arm:I')
            (work / 'deploy.log').write_text(logs + '\n')
            if 'refused: this installed APK does not match artifact.apk.sha256' in logs:
                raise RuntimeError('导入后 APK 身份不匹配，停止部署')
            lines = logs.splitlines()
            if any(f'Game startup accepted: {title} session ' in line and line not in prior for line in lines):
                break
            time.sleep(1)
        else:
            raise RuntimeError('等待启动超时；未生成成功回执')
    if adb.installed_hash(package) != artifact['apk']['sha256']:
        raise RuntimeError('部署期间 APK 被其他安装替换，停止部署')
    receipt = dict(serial=adb.serial, package=package, packageName=package, apkSha256=artifact['apk']['sha256'],
                   contentRoot=artifact['contentRoot'], artifactSha256=artifact_hash, mode=mode,
                   refreshExisting=refresh_existing, startupAccepted=start)
    (work / 'deployment.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def deploy(pack, apk, adb, output_root, mode='auto', refresh_existing=False, timeout=180, push_timeout=None,
           start=True):
    pack, apk = Path(pack).resolve(), Path(apk).resolve()
    title, _, _ = game_apk.nacp_info(pack / 'ctl/control.nacp')
    output_root = Path(output_root).resolve()
    if output_root == pack or pack in output_root.parents:
        raise RuntimeError('部署输出不能放在输入资源包内')
    output_root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=title + '-', dir=output_root))
    print(f'部署目录：{work}', flush=True)
    frozen = work / 'game.apk'
    copy_file(apk, frozen)
    resources = work / 'resources'
    game_apk.resources(pack, frozen, resources, refresh_existing=refresh_existing)
    deploy_resources(frozen, resources, adb, work, mode, timeout, push_timeout, start, refresh_existing,
                     require_current=False)
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--pack', type=Path)
    source.add_argument('--resources', type=Path)
    parser.add_argument('--apk', required=True, type=Path)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--mode', choices=('auto', 'direct', 'generic'), default='auto')
    parser.add_argument('--refresh-existing', action='store_true')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--push-timeout', type=int, default=None, help='资源包推送超时秒数；缺省按资源包大小算（约 30 MB/s、2 倍余量）')
    start = parser.add_mutually_exclusive_group()
    start.add_argument('--start', dest='start', action='store_true')
    start.add_argument('--no-start', dest='start', action='store_false')
    parser.set_defaults(start=True)
    args = parser.parse_args()
    if args.timeout <= 0 or (args.push_timeout is not None and args.push_timeout <= 0) or (args.resources and args.refresh_existing):
        parser.error('超时必须大于零；--refresh-existing 只用于 --pack')
    def interrupt(_signal, _frame):
        raise InterruptedError('部署已取消。')
    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    try:
        adb = Adb(args.serial)
        if args.pack:
            work = deploy(args.pack, args.apk, adb, REPO / 'build/android-deploy', args.mode,
                          args.refresh_existing, args.timeout, args.push_timeout, args.start)
            receipt = json.loads((work / 'deployment.json').read_text())
        else:
            root = REPO / 'build/android-deploy'; root.mkdir(parents=True, exist_ok=True)
            work = Path(tempfile.mkdtemp(prefix='resources-', dir=root))
            receipt = deploy_resources(args.apk.resolve(strict=True), args.resources.resolve(strict=True), adb,
                                       work, args.mode, args.timeout, args.push_timeout, args.start)
        print(json.dumps(receipt, ensure_ascii=False))
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('【红】部署失败：' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
