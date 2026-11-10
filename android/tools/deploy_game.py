#!/usr/bin/env python3
"""部署签名游戏 APK 和绑定资源。默认导入后启动；--no-start 只导入。

--pack 接受 make_pack 输出并冻结资源；--resources 接受已绑定资源目录。
所有设备操作须由外部设备租约保护。
"""
import argparse
import hashlib
import json
import os
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

    def popen(self, *args):
        return subprocess.Popen([*self.argv, *map(str, args)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

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


# 资源包推送超时按「字节 + 文件数」算（11-01：写死 600 秒，21 GB 的暗黑3 资源包必超时；
# vivo 10AF4N00TA002NC 实测 21 GB 推 715 秒 ≈ 30 MB/s；小文件密集时 adb 每文件还有
# 协议开销，8E 资源树 19,756 文件、84% <64KB，纯字节估的 251s 推不完）。留 2 倍余量。
#   下限只覆盖 adb 握手 / 建目录这类固定开销；命令行 --push-timeout 显式给了照用
PUSH_BYTES_PER_S = 30e6
PUSH_FILE_S = 0.02          # adb push 每文件协议开销估 20ms
PUSH_MARGIN = 2
PUSH_FLOOR_S = 120
PUSH_PROGRESS_S = 5         # 推送进度轮询间隔（设备侧 find/du）
PUSH_TAR_MIN_FILES = 5000   # 文件数超此线默认走 tar 单推 + 设备端解开（--push-tar 可改）


def tree_bytes(path):
    path = Path(path)
    return path.stat().st_size if path.is_file() else sum(p.stat().st_size for p in path.rglob('*') if p.is_file())


def tree_files(path):
    path = Path(path)
    return sum(1 for p in path.rglob('*') if p.is_file()) if path.is_dir() else 1


def push_timeout_for(resources):
    est = tree_bytes(resources) / PUSH_BYTES_PER_S + tree_files(resources) * PUSH_FILE_S
    return max(PUSH_FLOOR_S, int(est * PUSH_MARGIN) + 1)


def dev_tree_stat(adb, dev_dir):
    """设备端目录的 (文件数, 字节)：find + du。取不到按 (0,0)，进度条容错。"""
    files = adb.shell('find', dev_dir, '-type', 'f').splitlines()
    du = adb.shell('du', '-sk', dev_dir).split()
    return len([ln for ln in files if ln.strip()]), (int(du[0]) * 1024 if du else 0)


def push_tree(adb, resources, dst, timeout, log):
    """单条 adb push 整树，推送中按 PUSH_PROGRESS_S 轮询设备端目录打进度；
    超时杀进程并把最后的进度写进错误（能看出卡在哪）。"""
    t0 = time.monotonic()
    proc = adb.popen('push', str(resources), dst)
    nf = nb = 0
    while proc.poll() is None:
        try:
            nf, nb = dev_tree_stat(adb, dst)
        except (RuntimeError, ValueError, subprocess.SubprocessError):
            pass
        if time.monotonic() - t0 > timeout:
            proc.kill()
            raise RuntimeError(f'资源推送超时（>{timeout}s）：最后进度 {nf} 文件 / '
                               f'{nb / 1e6:.0f} MB；小文件密集可 --push-tar 单推解开')
        log(f'推送中：{nf} 文件 / {nb / 1e6:.0f} MB'
            f'（{nb / 1e6 / max(1, time.monotonic() - t0):.0f} MB/s）')
        time.sleep(PUSH_PROGRESS_S)
    out = proc.stdout.read()
    if proc.returncode != 0:
        raise RuntimeError(f'adb push 失败 rc={proc.returncode}: {out.strip()[:300]}')
    nf, nb = dev_tree_stat(adb, dst)
    log(f'推送完成：{nf} 文件 / {nb / 1e6:.0f} MB / {time.monotonic() - t0:.0f}s')


def verify_pushed_tree(adb, dev_dir, local_root, log):
    """设备端目录逐文件 SHA 与本地资源树清单一致（相对路径 → sha256 映射全等）。
    find -exec sha256sum {} + 把全部路径摊进一次 exec：几万文件的资源树触发
    toybox E2BIG（Argument list too long，8E r4 真机实测）。改走有界分批
    find -print0 | xargs -0 -n 256 sha256sum（Android toybox xargs 提供
    -0/-n、find 提供 -print0）；核验失败只回短 stderr 尾，不灌文件清单。
    管道必须包在 sh -c 里传：Adb.shell 对参数做 shlex.join，直接传整串会被
    单引号包成一个词，设备当文件名 exec → rc=127（8E r5 qa-fone 实测）。"""
    expected = {str(p.relative_to(local_root)): digest(p)
                for p in Path(local_root).rglob('*') if p.is_file()}
    try:
        out = adb.shell('sh', '-c',
                        f'find {dev_dir} -type f -print0 | xargs -0 -n 256 sha256sum')
    except RuntimeError as error:
        raise RuntimeError('设备端资源 SHA 核验失败：' + str(error)[-200:]) from error
    actual = {}
    for ln in out.splitlines():
        m = re.match(r'^([0-9a-f]{64})\s+(.+)$', ln.strip())
        if m:
            sha, dev = m.groups()
            actual[dev[len(dev_dir) + 1:] if dev.startswith(dev_dir + '/') else dev] = sha
    if actual != expected:
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        bad = sorted(k for k in expected.keys() & actual.keys() if expected[k] != actual[k])
        raise RuntimeError(f'设备端资源 SHA 核对失败：缺 {len(missing)} {missing[:3]}；'
                           f'多 {len(extra)} {extra[:3]}；sha 不符 {len(bad)} {bad[:3]}')
    log(f'设备端资源逐文件 SHA 核对通过（{len(actual)} 文件）')


def push_via_tar(adb, resources, dst, timeout, log, work):
    """小文件密集路径：本地打 tar → 单文件 adb push → 设备端解开 → 逐文件 SHA 核对。
    tar 落 work（本次 out/deploy 下）——不进系统临时目录（qa-fone：原包/打包
    中间产物都归 out 保留策略管）。"""
    work.mkdir(parents=True, exist_ok=True)
    tmp = Path(work) / 'resources.tar'
    # macOS bsdtar 会把 ._*/.DS_Store AppleDouble 合成进归档；设备解开后对不上清单
    env = dict(os.environ, COPYFILE_DISABLE='1')
    subprocess.run(['tar', '-cf', str(tmp), '--exclude', '._*', '--exclude', '.DS_Store',
                    '-C', str(resources), '.'], check=True, env=env)
    log(f'资源打 tar {tmp.stat().st_size / 1e6:.0f} MB（{tree_files(resources)} 文件）→ 单文件推送')
    adb.call('push', tmp, dst + '.tar', timeout=timeout)
    adb.shell('mkdir', '-p', dst)
    adb.shell('tar', '-xf', dst + '.tar', '-C', dst)
    adb.shell('rm', '-f', dst + '.tar')
    verify_pushed_tree(adb, dst, resources, log)
    tmp.unlink()                                  # 核对过：推送 tar 是再生材料


def deploy_resources(apk, resources, adb, work, mode='auto', timeout=180, push_timeout=None,
                     start=True, refresh_existing=False, require_current=True,
                     push_tar=None, log=None):
    """push_tar：None=按文件数自动（≥PUSH_TAR_MIN_FILES 走 tar 单推解开）；
    True/False 强制走 / 不走。log：进度回调（推送中/完成/SHA 核对都打进去）。"""
    log = log or (lambda *a, **k: None)
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
        nfiles = tree_files(resources)
        use_tar = push_tar if push_tar is not None else nfiles >= PUSH_TAR_MIN_FILES
        est = push_timeout or push_timeout_for(resources)
        if use_tar:
            why = '强制 --push-tar' if push_tar else f'文件数 ≥{PUSH_TAR_MIN_FILES}'
            log(f'资源树 {nfiles} 文件（{why}）：走 tar 单推 + 设备端解开'
                f'（超时 {est}s，按字节+文件数估）')
            push_via_tar(adb, resources, pending, est, log, work)
        else:
            log(f'资源树 {nfiles} 文件整树推送（超时 {est}s，按字节+文件数估）')
            push_tree(adb, resources, pending, est, log)
        adb.shell('chmod', '-R', 'u=rwX,g=rX,o=r', pending)
        adb.shell('sh', '-c',
                  f'find {pending} -type d -print0 | xargs -0 -n 256 chmod o=x')
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
           start=True, push_tar=None):
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
                     require_current=False, push_tar=push_tar, log=print)
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
    parser.add_argument('--push-timeout', type=int, default=None, help='资源包推送超时秒数；缺省按资源包字节+文件数算（约 30 MB/s + 20ms/文件、2 倍余量）')
    parser.add_argument('--push-tar', choices=('auto', 'always', 'never'), default='auto',
                        help='资源推送方式：auto 文件数≥%d 走 tar 单推解开（默认）；always 强制；never 永远整树推' % PUSH_TAR_MIN_FILES)
    start = parser.add_mutually_exclusive_group()
    start.add_argument('--start', dest='start', action='store_true')
    start.add_argument('--no-start', dest='start', action='store_false')
    parser.set_defaults(start=True)
    args = parser.parse_args()
    if args.timeout <= 0 or (args.push_timeout is not None and args.push_timeout <= 0) or (args.resources and args.refresh_existing):
        parser.error('超时必须大于零；--refresh-existing 只用于 --pack')
    push_tar = {'auto': None, 'always': True, 'never': False}[args.push_tar]
    def interrupt(_signal, _frame):
        raise InterruptedError('部署已取消。')
    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    try:
        adb = Adb(args.serial)
        if args.pack:
            work = deploy(args.pack, args.apk, adb, REPO / 'build/android-deploy', args.mode,
                          args.refresh_existing, args.timeout, args.push_timeout, args.start,
                          push_tar=push_tar)
            receipt = json.loads((work / 'deployment.json').read_text())
        else:
            root = REPO / 'build/android-deploy'; root.mkdir(parents=True, exist_ok=True)
            work = Path(tempfile.mkdtemp(prefix='resources-', dir=root))
            receipt = deploy_resources(args.apk.resolve(strict=True), args.resources.resolve(strict=True), adb,
                                       work, args.mode, args.timeout, args.push_timeout, args.start,
                                       push_tar=push_tar, log=print)
        print(json.dumps(receipt, ensure_ascii=False))
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('【红】部署失败：' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
