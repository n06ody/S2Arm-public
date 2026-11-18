#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Fail-closed warm resident deployment; caller supplies its leased installer."""
import json
import math
from pathlib import Path
import re
import secrets
import shlex
import time
import zipfile

import deploy_game
import game_apk


_HEX = re.compile(r'^[0-9a-f]{64}$')
_REFRESH_FILES = {'manifest.json', 'index.tsv', 'game-build-artifact.json', 'ctl/control.nacp'}


def container_metadata(pack, output):
    """Copy only revision identity files; do not transfer resource payloads."""
    pack, output = Path(pack), Path(output)
    if pack.is_symlink() or not pack.is_dir():
        raise ValueError('资源包路径无效')
    pack, output = pack.resolve(), output.resolve()
    if output == pack or pack in output.parents or output in pack.parents:
        raise ValueError('身份输出不能覆盖资源包')
    sources = []
    for name in sorted(_REFRESH_FILES):
        source = pack / name
        if any(parent.is_symlink() for parent in (source, *source.parents) if parent != pack.parent):
            raise ValueError('资源身份包含链接')
        if not source.is_file():
            raise ValueError('资源身份文件缺失')
        sources.append((name, source))
    output.mkdir(parents=True, exist_ok=False)
    for name, source in sources:
        target = output / name
        before = deploy_game.digest(source)
        deploy_game.copy_file(source, target)
        if deploy_game.digest(target) != before or deploy_game.digest(source) != before:
            raise ValueError('复制资源身份时文件变化')
        target.chmod(0o444)
    return output


def inventory_cache(adb, package, title):
    """List files/sizes and three deterministic SHA samples without changing them."""
    if not re.fullmatch(r'[0-9A-F]{16}', title):
        raise ValueError('程序号无效')
    root = 'files/user/0/saves/' + title + '/ryujinx-android/direct-cache/nvn-vk'
    parts = root.split('/')
    checks = []
    for i in range(1, len(parts) + 1):
        path = shlex.quote('/'.join(parts[:i]))
        checks.append(f'if [ -L {path} ]; then echo BAD; exit; fi; '
                      f'if [ ! -e {path} ]; then echo ABSENT; exit; fi; '
                      f'if [ ! -d {path} ] || [ ! -r {path} ] || [ ! -x {path} ]; then echo BAD; exit; fi;')
    probe = adb.shell('run-as', package, 'sh', '-c', ' '.join(checks) + ' echo PRESENT')
    if probe == 'ABSENT':
        return {'state': 'absent', 'files': {}, 'sample': {}}
    if probe != 'PRESENT':
        raise ValueError('缓存路径状态不可证')
    cd = 'cd ' + shlex.quote(root) + ' && '
    odd = adb.shell('run-as', package, 'sh', '-c',
                    cd + 'find . -mindepth 1 ! -type f ! -type d -print')
    if odd:
        raise ValueError('缓存含链接或特殊文件')
    raw = adb.shell('run-as', package, 'sh', '-c',
                    cd + "find . -type f -exec stat -c '%s  %n' {} +")
    files = {}
    for line in raw.splitlines():
        match = re.fullmatch(r'(\d+)  \./([A-Za-z0-9._/-]+)', line)
        if not match:
            raise ValueError('缓存清单格式错误')
        name = match[2]
        if name in files or any(part in ('', '.', '..') for part in name.split('/')):
            raise ValueError('缓存清单路径无效')
        files[name] = int(match[1])
    names = sorted(files)
    indices = sorted({0, len(names) // 2, len(names) - 1}) if names else []
    sample = {}
    for i in indices:
        name = names[i]
        path = root + '/' + name
        result = adb.shell('run-as', package, 'sha256sum', path).split()
        if len(result) != 2 or result[1] != path or not _HEX.fullmatch(result[0]):
            raise ValueError('缓存抽样摘要不可证')
        sample[name] = result[0]
    return {'state': 'present', 'files': files, 'sample': sample}


def _launch_session(adb, package, title, timeout):
    stamp = adb.shell('date', '+%s%3N')
    if not re.fullmatch(r'[0-9]{13}', stamp):
        raise ValueError('设备启动时间不可证')
    deploy_game.start_activity(adb, '-n', package + '/org.s2arm.android.MainActivity',
                               '--es', 's2arm.title_id', title,
                               '--es', 's2arm.graphics_mode', 'direct')
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            pid_text = adb.shell('pidof', package + ':game')
        except RuntimeError:
            time.sleep(0.5)
            continue
        if not re.fullmatch(r'[1-9][0-9]*', pid_text):
            raise ValueError('游戏 PID 不唯一')
        log = adb.shell('logcat', '-d', '-v', 'epoch', '--pid=' + pid_text, '-s', 'S2Arm:I')
        pattern = (r'^\s*(\d+)\.(\d+)\s+' + pid_text +
                   r'\s+\d+\s+I\s+S2Arm\s*:\s+Game startup accepted: ' +
                   re.escape(title) + r' session ([1-9][0-9]*)(?:\s|$)')
        matches = [m for line in log.splitlines() if (m := re.match(pattern, line)) and
                   int(m[1]) * 1000 + int(m[2][:3].ljust(3, '0')) >= int(stamp)]
        if matches:
            after = adb.shell('date', '+%s%3N')
            if not re.fullmatch(r'[0-9]{13}', after) or int(after) < int(stamp) or any(
                    int(m[1]) * 1000 + int(m[2][:3].ljust(3, '0')) > int(after) for m in matches):
                raise ValueError('设备启动日志时间不可证')
            if len({m[3] for m in matches}) != 1:
                raise ValueError('启动会话不唯一')
            if adb.shell('pidof', package + ':game') != pid_text:
                raise ValueError('游戏 PID 已变化')
            return int(pid_text), int(matches[-1][3])
        time.sleep(0.5)
    raise RuntimeError('当前游戏 PID 的启动回执不可证')


def deploy_container(apk, resources, adb, work, install, prepare=None, *, expected_package,
                     expected_prior_apk, expected_signer, expected_title, timeout=60):
    """Reuse a revision-backed generic container under a caller-held device lease."""
    if type(timeout) not in (int, float) or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('启动超时无效')
    if not isinstance(expected_title, str) or not re.fullmatch(r'[0-9A-F]{16}', expected_title):
        raise ValueError('程序号无效')
    started = time.monotonic_ns()
    apk, resources, work = Path(apk), Path(resources), Path(work)
    apk, resources = _freeze(apk, resources, work)
    actual_package, version, debug = game_apk.apk_identity(apk)
    if actual_package != expected_package or not debug or version < 1:
        raise ValueError('通用容器 APK 与机位身份不匹配')
    if not _HEX.fullmatch(expected_prior_apk) or not _HEX.fullmatch(expected_signer):
        raise ValueError('机位 APK 身份无效')
    if game_apk.apk_signer_sha256(apk) != expected_signer:
        raise ValueError('通用容器 APK 签名不匹配')
    with zipfile.ZipFile(apk) as z:
        if 'assets/game/game.properties' in z.namelist():
            raise ValueError('APK 不是通用容器')
    artifact_path = resources / 'game-build-artifact.json'
    artifact = json.loads(artifact_path.read_text())
    rows = _index_entries(resources)
    index_sha = deploy_game.digest(resources / 'index.tsv')
    root = 'sha256-' + index_sha
    pkg = artifact.get('package')
    if (artifact.get('titleId') != expected_title or artifact.get('contentRoot') != root or
            not isinstance(pkg, dict) or pkg.get('manifest') != 'manifest.json' or
            pkg.get('indexSha256') != index_sha or
            any(type(pkg.get(k)) is not int or not 1 <= pkg[k] <= 2**63 - 1
                for k in ('files', 'bytes')) or 'apk' in artifact):
        raise ValueError('通用资源身份不匹配')
    modules = artifact.get('modules')
    if not isinstance(modules, list) or not modules or any(
            not isinstance(mod, dict) or mod.get('source') != 'revision' or
            any(mod.get(k) not in rows for k in ('image', 'fixups', 'gates')) for mod in modules):
        raise ValueError('资源不是 revision 模块')
    nacp = resources / 'ctl/control.nacp'
    if rows.get('ctl/control.nacp') != (nacp.stat().st_size, deploy_game.digest(nacp)) or \
            game_apk.nacp_info(nacp)[0] != expected_title:
        raise ValueError('控制文件身份不匹配')
    if adb.running(expected_package):
        raise ValueError('游戏进程仍在运行')
    current, old = _current(adb, expected_package, expected_title, root)
    artifact_sha = deploy_game.digest(artifact_path)
    if current['artifactSha256'] != artifact_sha or old != artifact:
        raise ValueError('当前 revision 构建描述不匹配')
    if adb.installed_hash(expected_package) != expected_prior_apk:
        raise ValueError('机位原 APK 身份不匹配')
    cache_before = inventory_cache(adb, expected_package, expected_title)
    (work / 'cache-before.json').write_text(json.dumps(cache_before, sort_keys=True) + '\n')
    preflight_ms = (time.monotonic_ns() - started) / 1_000_000
    target = deploy_game.digest(apk)
    if target != expected_prior_apk:
        install(apk, expected_package)
    if adb.installed_hash(expected_package) != target:
        raise ValueError('受控安装后的 APK 摘要不匹配')
    current2, old2 = _current(adb, expected_package, expected_title, root)
    if current2 != current or old2 != old:
        raise ValueError('安装后 revision 身份发生变化')
    cache_after = inventory_cache(adb, expected_package, expected_title)
    (work / 'cache-after-install.json').write_text(json.dumps(cache_after, sort_keys=True) + '\n')
    if cache_after != cache_before:
        raise ValueError('安装前后缓存清单或抽样发生变化')
    install_ms = (time.monotonic_ns() - started) / 1_000_000
    if prepare is not None:
        prepare()
    if _current(adb, expected_package, expected_title, root) != (current, old):
        raise ValueError('准备局时 revision 身份发生变化')
    if inventory_cache(adb, expected_package, expected_title) != cache_before:
        raise ValueError('准备局时缓存清单或抽样发生变化')
    pid, session = _launch_session(adb, expected_package, expected_title, timeout)
    after_current, after_artifact = _current(adb, expected_package, expected_title, root)
    stable = ('version', 'titleId', 'contentRoot', 'revisionDir', 'artifact', 'artifactSha256')
    if (adb.installed_hash(expected_package) != target or after_artifact != old or
            any(after_current.get(key) != current.get(key) for key in stable)):
        raise ValueError('启动后身份不匹配')
    receipt = {'mode': 'revision-container', 'package': expected_package, 'titleId': expected_title,
               'apkSha256': target, 'contentRoot': root, 'artifactSha256': artifact_sha,
               'pid': pid, 'session': session, 'startupAccepted': True,
               'timing': {'preflight_ms': preflight_ms, 'install_verified_ms': install_ms,
                          'startup_verified_ms': (time.monotonic_ns() - started) / 1_000_000},
               'cacheEvidence': 'file names, sizes and three sampled SHA-256; other content unproven'}
    (work / 'resident.json').write_text(json.dumps(receipt, sort_keys=True) + '\n')
    return receipt


def _freeze(apk, resources, work):
    if apk.is_symlink() or resources.is_symlink():
        raise ValueError('输入路径无效')
    apk, resources, work = apk.resolve(), resources.resolve(), work.resolve()
    if work == resources or resources in work.parents or work == apk or apk in work.parents:
        raise ValueError('冻结输出不能覆盖输入')
    if apk.is_symlink() or not apk.is_file() or resources.is_symlink() or not resources.is_dir():
        raise ValueError('输入路径无效')
    paths = list(resources.rglob('*'))
    if any(p.is_symlink() or not (p.is_file() or p.is_dir()) for p in paths):
        raise ValueError('刷新资源包含链接或特殊文件')
    files = {p.relative_to(resources).as_posix() for p in paths if p.is_file()}
    if files != _REFRESH_FILES:
        raise ValueError('常驻刷新必须使用精简资源包')
    work.mkdir(parents=True, exist_ok=False)
    frozen = work / 'frozen'
    frozen_apk = frozen / 'game.apk'
    frozen_resources = frozen / 'resources'
    manifest = {}
    for source, target in [(apk, frozen_apk)] + [
            (resources / name, frozen_resources / name) for name in sorted(files)]:
        before = deploy_game.digest(source)
        deploy_game.copy_file(source, target)
        if deploy_game.digest(target) != before or deploy_game.digest(source) != before:
            raise ValueError('冻结输入期间文件变化')
        target.chmod(0o444)
        manifest[target.relative_to(frozen).as_posix()] = before
    (frozen / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True) + '\n')
    return frozen_apk, frozen_resources


def _index_entries(resources):
    rows = game_apk.read_index(resources)
    seen = set()
    lines = (resources / 'index.tsv').read_text().splitlines()
    for line in lines:
        fields = line.split('\t')
        if len(fields) != 3:
            raise ValueError('资源索引格式无效')
        name, size, sha = fields
        if (not name or name.startswith('/') or
                any(part in ('', '.', '..') for part in name.split('/')) or
                name in seen or not re.fullmatch(r'[0-9]+', size) or not _HEX.fullmatch(sha)):
            raise ValueError('资源索引条目无效')
        seen.add(name)
    if seen != set(rows):
        raise ValueError('资源索引条目不一致')
    return rows


def _current(adb, package, title, expected_root):
    if not re.fullmatch(r'[0-9A-F]{16}', title):
        raise ValueError('程序号无效')
    root = f'files/games/{title}'
    try:
        current = json.loads(adb.shell('run-as', package, 'cat', root + '/current.json'))
    except (RuntimeError, ValueError, TypeError) as error:
        raise ValueError('当前资源身份不可读') from error
    if not isinstance(current, dict) or current.get('version') != 1 or current.get('titleId') != title:
        raise ValueError('当前资源身份无效')
    if current.get('contentRoot') != expected_root:
        raise ValueError('当前内容根与目标不一致')
    if not re.fullmatch(r'sha256-[0-9a-f]{64}', expected_root):
        raise ValueError('目标内容根无效')
    if current.get('revisionDir') != 'revisions/' + expected_root:
        raise ValueError('当前资源目录不匹配')
    artifact = current.get('artifact')
    sha = current.get('artifactSha256')
    if not isinstance(artifact, str) or not re.fullmatch(r'[A-Za-z0-9._-]+\.json', artifact):
        raise ValueError('当前构建描述路径无效')
    if not isinstance(sha, str) or not _HEX.fullmatch(sha):
        raise ValueError('当前构建描述摘要无效')
    revision = root + '/' + current['revisionDir']
    for path, expected in ((revision + '/index.tsv', expected_root[7:]),
                           (revision + '/' + artifact, sha)):
        try:
            parts = adb.shell('run-as', package, 'sha256sum', path).split()
        except RuntimeError as error:
            raise ValueError('当前资源身份不可读') from error
        if len(parts) != 2 or parts != [expected, path]:
            raise ValueError('当前资源身份摘要不匹配')
    try:
        old = json.loads(adb.shell('run-as', package, 'cat', revision + '/' + artifact))
    except (RuntimeError, ValueError, TypeError) as error:
        raise ValueError('当前构建描述不可读') from error
    if not isinstance(old, dict) or old.get('titleId') != title or old.get('contentRoot') != expected_root:
        raise ValueError('当前构建描述身份不匹配')
    return current, old


class _NoInstall:
    """Reuse the existing importer while refusing its ordinary installer fallback."""
    def __init__(self, adb, package):
        self._adb = adb
        self._package = package
        self.serial = adb.serial

    def call(self, *args, **kwargs):
        if args and args[0] == 'install':
            raise RuntimeError('常驻部署禁止普通安装退路')
        return self._adb.call(*args, **kwargs)

    def shell(self, *args):
        result = self._adb.shell(*args)
        if args and args[0] == 'logcat' and 'brief' in args:
            pids = self._adb.shell('pidof', self._package).split()
            if not pids or not all(re.fullmatch(r'[0-9]+', pid) for pid in pids):
                raise RuntimeError('无法绑定导入进程')
            kept = []
            for line in result.splitlines():
                match = re.match(r'^[VDIWEF]/S2Arm\s*\(\s*([0-9]+)\s*\):', line)
                if match and match[1] in pids:
                    kept.append(line)
            result = '\n'.join(kept)
        if len(args) == 2 and args[0] == 'pidof':
            if not result or not all(re.fullmatch(r'[0-9]+', value) for value in result.split()):
                raise RuntimeError('无法绑定导入进程')
        return result

    def running(self, package):
        return self._adb.running(package)

    def installed_hash(self, package):
        return self._adb.installed_hash(package)


def deploy(apk, resources, adb, work, install, prepare=None, *, expected_package, timeout=60):
    """Refresh verified resident resources. The installer must use the held lease."""
    if type(timeout) not in (int, float) or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('启动超时无效')
    started = time.monotonic_ns()
    timings = {}
    apk, resources, work = Path(apk), Path(resources), Path(work)
    apk, resources = _freeze(apk, resources, work)
    def mark(name):
        timings[name] = (time.monotonic_ns() - started) / 1_000_000
        (work / 'timing.json').write_text(json.dumps(timings, sort_keys=True) + '\n')
    mark('frozen_ms')
    artifact, artifact_sha = deploy_game.validate(apk, resources)
    title, package = artifact['titleId'], artifact['apk']['packageName']
    if package != expected_package:
        raise ValueError('资源 APK 与机位固定包名不匹配')
    if game_apk.apk_signer_sha256(apk) != artifact['apk'].get('signingCertSha256'):
        raise ValueError('APK 签名与资源身份不匹配')
    entries = _index_entries(resources)
    nacp = resources / 'ctl/control.nacp'
    if entries.get('ctl/control.nacp') != (nacp.stat().st_size, deploy_game.digest(nacp)):
        raise ValueError('控制文件与索引不匹配')
    if game_apk.nacp_info(nacp)[0] != title:
        raise ValueError('控制文件程序号不匹配')
    if adb.running(package):
        raise ValueError('游戏进程仍在运行')
    _, old = _current(adb, package, title, artifact['contentRoot'])
    if not isinstance(old.get('apk'), dict) or old['apk'].get('packageName') != package:
        raise ValueError('当前构建描述包名不匹配')
    installed = adb.installed_hash(package)
    if not isinstance(installed, str) or not _HEX.fullmatch(installed) or installed != old['apk'].get('sha256'):
        raise ValueError('已装 APK 与当前资源身份不匹配')
    before_cache = inventory_cache(adb, package, title)
    (work / 'cache-before.json').write_text(json.dumps(before_cache, sort_keys=True) + '\n')
    mark('preflight_ms')
    target = artifact['apk']['sha256']
    if installed != target:
        install(apk, package)
    if adb.installed_hash(package) != target:
        raise ValueError('受控安装后的 APK 摘要不匹配')
    after_install = inventory_cache(adb, package, title)
    (work / 'cache-after-install.json').write_text(json.dumps(after_install, sort_keys=True) + '\n')
    if after_install != before_cache:
        raise ValueError('安装前后缓存清单或抽样发生变化')
    mark('install_verified_ms')
    attempt = work / ('import-' + secrets.token_hex(16))
    attempt.mkdir()
    def before_start():
        current, _ = _current(adb, package, title, artifact['contentRoot'])
        if current['artifactSha256'] != artifact_sha or adb.installed_hash(package) != target:
            raise ValueError('刷新后身份不匹配')
        if prepare is not None:
            prepare()
            current, _ = _current(adb, package, title, artifact['contentRoot'])
            if current['artifactSha256'] != artifact_sha or adb.installed_hash(package) != target:
                raise ValueError('准备局时资源身份发生变化')
        after_refresh = inventory_cache(adb, package, title)
        (work / 'cache-after-refresh.json').write_text(json.dumps(after_refresh, sort_keys=True) + '\n')
        if after_refresh != before_cache:
            raise ValueError('刷新前后缓存清单或抽样发生变化')
        mark('refresh_verified_ms')
    try:
        receipt = deploy_game.deploy_resources(apk, resources, _NoInstall(adb, package), attempt,
                                               mode='direct', start=False, refresh_existing=True, require_current=True)
        before_start()
        pid, session = _launch_session(adb, package, title, timeout)
        current, _ = _current(adb, package, title, artifact['contentRoot'])
        if current['artifactSha256'] != artifact_sha or adb.installed_hash(package) != target:
            raise ValueError('启动后身份不匹配')
        mark('startup_verified_ms')
        receipt.update(pid=pid, session=session, startupAccepted=True)
    except BaseException:
        # The import receipt alone is not a successful resident deployment.
        (attempt / 'deployment.json').unlink(missing_ok=True)
        raise
    (work / 'resident.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt
