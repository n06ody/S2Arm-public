#!/usr/bin/env python3
"""部署签名游戏 APK 和绑定资源。默认导入后启动；--no-start 只导入。

--pack 接受 make_pack 输出并冻结资源；--resources 接受已绑定资源目录。
--save-archive 在资源安装后、起局前直传并恢复一份 .s2asave，保留现有存档备份。
--in-place：资源目录 adb push 到手机共享存储的约定位置（/storage/emulated/0/S2Arm/Games/<程序号>），
  再让 App 原地登记（不复制进私有目录，App 要「所有文件访问」，这里用 appops 给本包打开）；默认是直传进私有目录。
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
import socket
import subprocess
import sys
import tempfile
import time

import game_apk
from filecopy import copy_file

REPO = Path(__file__).resolve().parents[2]
IMPORT_RECEIVE_EXTRA = 'org.s2arm.android.IMPORT_RECEIVE'


class Adb:
    def __init__(self, serial, adb='adb'):
        if not re.fullmatch(r'[A-Za-z0-9_.:-]+', serial):
            raise ValueError('设备序列号无效。')
        self.serial = serial
        # 是现成文件就整条当路径（Windows 路径的反斜杠不能过 shlex）；否则按命令行拆（测试的「python 桩脚本」）
        self.argv = [adb, '-s', serial] if Path(adb).is_file() else [*shlex.split(adb), '-s', serial]

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


def own_lines(adb, package, logs):
    """只留这个包自己的进程打的行（brief 格式「I/S2Arm   ( pid): …」）：手机上别的 S2Arm App（通用包、别款的独立包）
    同标签的 refused / installed 不算这次导入的结论（11-13：另一个 App 的导入探针打了 refused，正在部署的一款被误判被拒）。
    取不到进程号（App 还没起来）时原样返回，不吞日志。"""
    pids = set(adb.shell('pidof', package).split())
    if not pids:
        return logs
    keep = []
    for line in logs.splitlines():
        m = re.search(r'\(\s*(\d+)\)', line)
        if m is None or m.group(1) in pids:
            keep.append(line)
    return '\n'.join(keep)


def start_activity(adb, *args):
    output = adb.shell('am', 'start', '-W', *args)
    if 'Error:' in output or 'Exception' in output:
        raise RuntimeError('Activity 启动失败：' + output)


# 资源包直传（电脑 → App 私有目录，ImportReceiver.java）：adb forward 到 App 的本地套接字逐文件送字节，不经共享存储。
#   原先 adb push 到 /sdcard/Android/data/<pkg>/files/import 再由 App 读：vivo 上 App 经共享存储（FUSE）读 1.7 GB 大文件
#   中途不返回（11-13 奥日导入卡死）；iOS 是 devicectl 直接写进 App 沙盒。
# 超时按字节估（vivo 10AF4N00TA002NC adb 实测约 30 MB/s），留 2 倍余量；命令行 --push-timeout 显式给了照用
PUSH_BYTES_PER_S = 30e6
PUSH_MARGIN = 2
PUSH_FLOOR_S = 120
PUSH_PROGRESS_S = 5
RECEIVE_CONNECT_S = 30      # App 开好套接字的最长等待
# 原地使用的约定位置（LibraryActivity.IN_PLACE_DIR）：写 canonical 路径（/sdcard 是它的链接），App 登记、回执里都是这条
IN_PLACE_DIR = '/storage/emulated/0/S2Arm/Games'
ARTIFACT = 'game-build-artifact.json'


def tree_bytes(path):
    path = Path(path)
    return path.stat().st_size if path.is_file() else sum(p.stat().st_size for p in path.rglob('*') if p.is_file())


def tree_files(path):
    path = Path(path)
    return sum(1 for p in path.rglob('*') if p.is_file()) if path.is_dir() else 1


def push_timeout_for(resources):
    return transfer_timeout(tree_bytes(resources))


def transfer_timeout(size):
    return max(PUSH_FLOOR_S, int(size / PUSH_BYTES_PER_S * PUSH_MARGIN) + 1)


def import_wait_for(resources, timeout):
    """等导入回执的秒数：App 把收进私有目录的资源按 index.tsv 逐文件核对并装进库，和传输同一量级；命令行 --timeout 当下限。"""
    return max(timeout, push_timeout_for(resources))


def _remote_sums(adb, paths):
    """设备上这几个文件的 SHA-256；不在的不出现在结果里（sha256sum 对缺的文件非零退出，所以逐个问）。"""
    sums = {}
    for path in paths:
        try:
            parts = adb.shell('sha256sum', path).split()
        except RuntimeError:
            continue
        if len(parts) == 2 and parts[1] == path:
            sums[path] = parts[0]
    return sums


def push_in_place(adb, resources, title, timeout, log):
    """把资源目录放到共享存储的约定位置，返回设备上的路径。设备上已是同一份内容（index.tsv 相同）就不再整份推：
    构建描述不同（换了 APK）只推它。整份推先进 .<程序号>.part、推完整再换名，半截不顶替旧的。"""
    resources = Path(resources)
    remote = f'{IN_PLACE_DIR}/{title}'
    have = _remote_sums(adb, [f'{remote}/index.tsv', f'{remote}/{ARTIFACT}'])
    if have.get(f'{remote}/index.tsv') == digest(resources / 'index.tsv'):
        if have.get(f'{remote}/{ARTIFACT}') != digest(resources / ARTIFACT):
            adb.call('push', resources / ARTIFACT, f'{remote}/{ARTIFACT}', timeout=120)
            log(f'设备上 {remote} 已是同一份内容，只换构建描述')
        else:
            log(f'设备上 {remote} 已是同一份内容，不再推送')
        return remote
    index = (resources / 'index.tsv').read_text(encoding='utf-8').splitlines()
    missing = [row.split('\t')[0] for row in index if row and not (resources / row.split('\t')[0]).is_file()]
    if missing:
        raise RuntimeError(f'资源目录缺 {len(missing)} 个索引文件（如 {missing[0]}），设备上也没有同一份内容；'
                           '原地使用要整份资源（去掉 --refresh-existing）')
    part = f'{IN_PLACE_DIR}/.{title}.part'
    adb.shell('mkdir', '-p', IN_PLACE_DIR)
    adb.shell('rm', '-rf', part)
    t0 = time.monotonic()
    log(f'推送资源目录到 {remote}（{tree_files(resources)} 文件，超时 {timeout}s）')
    adb.call('push', resources, part, timeout=timeout)
    adb.shell('rm', '-rf', remote)
    adb.shell('mv', part, remote)
    log(f'推送完成：{tree_bytes(resources) / 1e6:.0f} MB / {time.monotonic() - t0:.0f}s')
    return remote


def request_in_place(adb, package, remote):
    """经同一个本地套接字（只认 adb 的 shell uid）请 App 原地登记 remote。协议见 ImportReceiver.java。"""
    port = adb.call('forward', 'tcp:0', 'localabstract:s2arm-import.' + package, timeout=30).strip()
    if not port.isdigit():
        raise RuntimeError('adb forward 没给出端口：' + port[:200])
    try:
        s = _connect(port)
        with s:
            s.settimeout(60)
            s.sendall(f'S2A-INPLACE 1 {remote}\n'.encode())
            reply = _line(s)
        if reply != 'OK in-place':
            raise RuntimeError('App 拒收原地登记请求：' + reply[:300])
    finally:
        adb.call('forward', '--remove', 'tcp:' + port, timeout=30)


def _connect(port):
    """设备端还没监听时 adb 先接 TCP 再断：读到 App 的 READY 才算连上。"""
    deadline = time.monotonic() + RECEIVE_CONNECT_S
    while True:
        s = None
        try:
            s = socket.create_connection(('127.0.0.1', int(port)), timeout=10)
            if _line(s).split(' ')[0] == 'READY':
                return s
            raise ConnectionError('不是导入接收端')
        except (OSError, ConnectionError):
            if s is not None:
                s.close()
            if time.monotonic() > deadline:
                raise RuntimeError(f'App 的导入接收端 {RECEIVE_CONNECT_S} 秒内没连上')
            time.sleep(0.5)


def _line(sock):
    b = bytearray()
    while not b.endswith(b'\n'):
        c = sock.recv(1)
        if not c:
            raise ConnectionError('连接被关闭')
        b += c
    return b[:-1].decode('utf-8')


def send_tree(adb, package, resources, name, timeout, log, *, paths=None, max_bytes=None):
    """经 adb forward 把资源树直传进 App（LibraryActivity 已带 IMPORT_RECEIVE 起好）。协议见 ImportReceiver.java。"""
    resources = Path(resources)
    files = sorted(p for p in resources.rglob('*') if p.is_file()) if paths is None else paths
    entries = [(p, p.stat().st_size) for p in files]
    total = sum(size for _, size in entries)
    check_transfer_budget(total, max_bytes)
    port = adb.call('forward', 'tcp:0', 'localabstract:s2arm-import.' + package, timeout=30).strip()
    if not port.isdigit():
        raise RuntimeError('adb forward 没给出端口：' + port[:200])
    try:
        s = _connect(port)
        t0 = last = time.monotonic()
        sent = 0
        with s:
            s.settimeout(timeout)
            s.sendall(f'S2A-IMPORT 1 {name}\n'.encode())
            for p, size in entries:
                with open(p, 'rb') as f:
                    if os.fstat(f.fileno()).st_size != size:
                        raise RuntimeError(f'资源大小在直传前变化：{p.name}')
                    s.sendall(f'F {size} {p.relative_to(resources).as_posix()}\n'.encode())
                    if size and s.sendfile(f, count=size) != size:
                        raise RuntimeError(f'资源直传长度不符：{p.name}')
                sent += size
                now = time.monotonic()
                if now - t0 > timeout:
                    raise RuntimeError(f'资源直传超时（>{timeout}s）：已传 {sent / 1e6:.0f} / {total / 1e6:.0f} MB')
                if now - last >= PUSH_PROGRESS_S:
                    last = now
                    log(f'直传中：{sent / 1e6:.0f} / {total / 1e6:.0f} MB（{sent / 1e6 / max(1, now - t0):.0f} MB/s）')
            s.sendall(b'END\n')
            reply = _line(s)
        if not reply.startswith('OK '):
            raise RuntimeError('App 拒收资源：' + reply[:300])
        if reply != f'OK {len(entries)} {sent}':
            raise RuntimeError('App 收包文件数或字节数不符：' + reply[:300])
        elapsed = time.monotonic() - t0
        log(f'直传完成：{len(entries)} 文件 / {sent / 1e6:.0f} MB / {elapsed:.0f}s')
        return dict(sentFiles=len(entries), sentBytes=sent, transferMs=round(elapsed * 1000))
    finally:
        adb.call('forward', '--remove', 'tcp:' + port, timeout=30)


def save_archive_size(archive):
    if not archive.is_file():
        raise ValueError('存档备份必须是常规文件。')
    size = archive.stat().st_size
    if not 0 < size <= (32 << 30) + (64 << 20):
        raise ValueError('存档备份为空或超过存档服务上限。')
    return size


def send_save(adb, package, title, archive, timeout):
    """Send one archive and wait for the shared restore service's commit receipt."""
    size = save_archive_size(archive)
    library = package + '/org.s2arm.android.LibraryActivity'
    start_activity(adb, '-n', library, '--ez', IMPORT_RECEIVE_EXTRA, 'true')
    port = adb.call('forward', 'tcp:0', 'localabstract:s2arm-import.' + package, timeout=30).strip()
    if not port.isdigit():
        raise RuntimeError('adb forward 没给出端口。')
    try:
        with _connect(port) as sock, archive.open('rb') as source:
            sock.settimeout(timeout)
            sock.sendall(f'S2A-SAVE 1 {title} {size}\n'.encode('ascii'))
            if sock.sendfile(source, count=size) != size:
                raise RuntimeError('存档传输期间文件大小变化。')
            sock.sendall(b'END\n')
            reply = _line(sock)
        if not reply.startswith('OK '):
            raise RuntimeError('App 拒绝恢复存档：' + reply[:180])
        receipt = json.loads(reply[3:])
        if (not isinstance(receipt, dict) or receipt.get('titleId') != title
                or type(receipt.get('files')) is not int or receipt['files'] <= 0
                or type(receipt.get('bytes')) is not int or receipt['bytes'] <= 0
                or not isinstance(receipt.get('previousBackup'), str)):
            raise RuntimeError('存档回执无效或标题号不符。')
        return receipt
    finally:
        adb.call('forward', '--remove', 'tcp:' + port, timeout=30)


def check_transfer_budget(size, max_bytes):
    if max_bytes is not None and size > max_bytes:
        raise RuntimeError(f'资源直传超过预算：{size} > {max_bytes} 字节')


def deploy_resources(apk, resources, adb, work, mode='auto', timeout=180, push_timeout=None,
                     start=True, refresh_existing=False, require_current=True, log=None, in_place=False,
                     save_archive=None, *, allow_full_import=True, resident_receipt=None, max_resource_bytes=None):
    """装 APK 后把资源树直传进 App 私有目录（send_tree），等 App 按 index.tsv 核完装进库的回执。
    in_place：改为推到共享存储约定位置、请 App 原地登记（push_in_place / request_in_place），回执来源是「in-place <路径>」。
    resident_receipt：外部租约内已核的上次部署回执，用于旧接收器的同根元数据刷新。
    allow_full_import=False：必须能在安装前选定驻留刷新路径，拒绝全量传输。
    max_resource_bytes：私有目录直传的载荷字节上限。log：进度回调。"""
    log = log or (lambda *a, **k: None)
    apk, resources, work = Path(apk), Path(resources), Path(work)
    if save_archive is not None:
        save_archive = Path(save_archive).resolve(strict=True)
        save_archive_size(save_archive)
    artifact, artifact_hash = validate(apk, resources)
    title, package = artifact['titleId'], artifact['apk']['packageName']
    if adb.running(package):
        raise ValueError('设备上的游戏仍在运行，不能安装。')
    # 机上已是同一份 APK（哈希相同）就不重装：同版本重装在部分机型上既不弹确认也不返回，干等到超时（11-14 vivo）
    try:
        installed_hash = adb.installed_hash(package)
    except RuntimeError:   # 没装 / 取不到：照常装
        installed_hash = None
    previous_root = None
    if resident_receipt is not None:
        previous_root = resident_receipt.get('contentRoot')
        old_apk = resident_receipt.get('apkSha256')
        old_binding = resident_receipt.get('artifactSha256')
        if (resident_receipt.get('packageName') != package or old_apk != installed_hash or
                not all(isinstance(value, str) and re.fullmatch(pattern, value)
                        for value, pattern in ((previous_root, r'sha256-[0-9a-f]{64}'),
                                               (old_apk, r'[0-9a-f]{64}'),
                                               (old_binding, r'[0-9a-f]{64}')))):
            raise RuntimeError('驻留导入回执与当前 APK 不匹配')
        if not in_place and previous_root == artifact['contentRoot']:
            refresh_existing = True
    if not allow_full_import and (in_place or previous_root != artifact['contentRoot']):
        raise RuntimeError('全量导入已禁止：缺少目标同根的驻留回执')
    if max_resource_bytes is not None and (in_place or type(max_resource_bytes) is not int or max_resource_bytes < 0):
        raise ValueError('资源字节预算仅支持私有目录直传且必须为非负整数')
    paths = [resources / name for name in game_apk.REFRESH_PAYLOAD] if refresh_existing and not in_place else None
    if paths is not None:
        planned_bytes = sum(path.stat().st_size for path in paths)
        check_transfer_budget(planned_bytes, max_resource_bytes)
    elif max_resource_bytes is not None:
        check_transfer_budget(tree_bytes(resources), max_resource_bytes)

    adb.shell('input', 'keyevent', 'KEYCODE_WAKEUP')
    adb.shell('wm', 'dismiss-keyguard')
    if installed_hash == artifact['apk']['sha256']:
        log('APK 与机上已装的相同，跳过安装')
    else:
        try:
            installed = adb.call('install', '-r', apk)
        except RuntimeError as error:
            if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in str(error):   # 签名钥不同（别的电脑 / 旧版本出的同款）：安卓不许覆盖装
                raise RuntimeError(f'手机上已装的 {package} 是另一把签名钥签的（别的电脑或旧版本出的包），安卓不允许直接覆盖。'
                                   '先在手机上卸载它再装；卸载会删掉这款在手机上的存档，需要的话先备份。') from None
            raise
        if 'Success' not in installed:
            raise RuntimeError('设备未确认 APK 安装成功。')
    if adb.installed_hash(package) != artifact['apk']['sha256']:
        raise RuntimeError('安装后的 APK 哈希与冻结 APK 不符，停止部署')
    name = 'deploy-' + work.name
    library = package + '/org.s2arm.android.LibraryActivity'
    est = push_timeout or (push_timeout_for(resources) if paths is None else transfer_timeout(planned_bytes))
    if in_place:
        remote = push_in_place(adb, resources, title, est, log)
        # 本包要「所有文件访问」才读得到共享存储上的资源（安卓 11+；开发工具代玩家在设置页点开）
        adb.shell('appops', 'set', package, 'MANAGE_EXTERNAL_STORAGE', 'allow')
        origin = 'in-place ' + remote
    else:
        origin = 'import/' + name
    since = adb.shell('date', '+%m-%d %H:%M:%S.000')
    start_activity(adb, '-n', library, '--ez', IMPORT_RECEIVE_EXTRA, 'true')
    transfer = dict(transferMode='inPlace' if in_place else 'refresh' if refresh_existing else 'full',
                    previousContentRoot=previous_root)
    if in_place:
        request_in_place(adb, package, remote)
    else:
        files = tree_files(resources) if paths is None else len(paths)
        log(f'资源 {files} 文件直传进 App（超时 {est}s，模式 {transfer["transferMode"]}）')
        options = {}
        if paths is not None:
            options['paths'] = paths
        if max_resource_bytes is not None:
            options['max_bytes'] = max_resource_bytes
        transfer.update(send_tree(adb, package, resources, name, est, log, **options))
    import_wait = import_wait_for(resources, timeout) if paths is None else max(timeout, transfer_timeout(planned_bytes))
    deadline = time.monotonic() + import_wait
    while time.monotonic() < deadline:
        logs = own_lines(adb, package, adb.shell('logcat', '-d', '-v', 'brief', '-T', since, '-s', 'S2Arm:I'))
        (work / 'deploy.log').write_text(logs + '\n')
        if in_place and 'Game in-place registration' in logs and 'refused:' in logs:
            raise RuntimeError('原地登记被拒绝：' + next(line for line in logs.splitlines()
                                                    if 'Game in-place registration' in line and 'refused:' in line)[-300:])
        if 'Game import of requested directory refused:' in logs or f'Game import of import/{name} refused:' in logs:
            raise RuntimeError('资源导入被拒绝' + ('；资源变化或首次部署请去掉 --refresh-existing'
                                                  if refresh_existing and allow_full_import else ''))
        imported_ok = any(line.endswith(f'from {origin}') and any(
            text in line for text in ('Game installed:', 'Game already installed:', 'Game build artifact updated',
                                      'Game registered in place:'))
            for line in logs.splitlines())
        if imported_ok:
            if require_current or not allow_full_import:
                # 正式包不可调试，run-as 读不了 current.json；App 导入成功时在 logcat 打完整回执（GameLibrary「Game import receipt」）
                receipt = next((line for line in logs.splitlines()
                                if f'Game import receipt: {title} ' in line and line.endswith(f' from {origin}')), None)
                if receipt is None:
                    raise RuntimeError('导入成功但设备没打出导入回执，无法核身份。')
                if f' contentRoot={artifact["contentRoot"]} ' not in receipt or f' artifactSha256={artifact_hash} ' not in receipt:
                    raise RuntimeError('设备导入身份不匹配。')
            break
        time.sleep(1)
    else:
        raise RuntimeError(f'等待导入超时（{import_wait} 秒内没有成功回执）：导入可能还在进行，没有强停应用；'
                           f'等它做完后用同一命令加 --refresh-existing 重跑核对')
    save_receipt = None
    if save_archive is not None:
        save_receipt = send_save(adb, package, title, save_archive, max(timeout, push_timeout or 0))
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
            logs = own_lines(adb, package, adb.shell('logcat', '-d', '-v', 'brief', '-T', since, '-s', 'S2Arm:I'))
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
                   refreshExisting=refresh_existing, startupAccepted=start, inPlace=remote if in_place else None)
    receipt.update(transfer)
    if save_receipt is not None:
        receipt['saveImport'] = save_receipt
    (work / 'deployment.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def deploy(pack, apk, adb, output_root, mode='auto', refresh_existing=False, timeout=180, push_timeout=None,
           start=True, in_place=False, save_archive=None):
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
                     require_current=False, log=print, in_place=in_place, save_archive=save_archive)
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--pack', type=Path)
    source.add_argument('--resources', type=Path)
    parser.add_argument('--apk', required=True, type=Path)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--save-archive', type=Path, help='确认恢复此 .s2asave；会在起局前备份并替换目标存档')
    parser.add_argument('--mode', choices=('auto', 'direct', 'generic'), default='auto')
    parser.add_argument('--refresh-existing', action='store_true')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--push-timeout', type=int, default=None, help='资源直传超时秒数；缺省按字节算（约 30 MB/s、2 倍余量）')
    parser.add_argument('--in-place', action='store_true',
                        help='推到共享存储 ' + IN_PLACE_DIR + '/<程序号> 后让 App 原地登记（不复制进私有目录）')
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
        adb = Adb(args.serial, os.environ.get('S2A_ADB') or 'adb')   # 启动器给的 adb（android_export.fetch_adb）；命令行缺省 PATH 上的
        if args.pack:
            work = deploy(args.pack, args.apk, adb, REPO / 'build/android-deploy', args.mode,
                          args.refresh_existing, args.timeout, args.push_timeout, args.start, args.in_place,
                          save_archive=args.save_archive)
            receipt = json.loads((work / 'deployment.json').read_text())
        else:
            root = REPO / 'build/android-deploy'; root.mkdir(parents=True, exist_ok=True)
            work = Path(tempfile.mkdtemp(prefix='resources-', dir=root))
            receipt = deploy_resources(args.apk.resolve(strict=True), args.resources.resolve(strict=True), adb,
                                       work, args.mode, args.timeout, args.push_timeout, args.start, log=print,
                                       in_place=args.in_place, save_archive=args.save_archive)
        print(json.dumps(receipt, ensure_ascii=False))
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('【红】部署失败：' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
