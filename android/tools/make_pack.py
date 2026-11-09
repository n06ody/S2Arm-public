#!/usr/bin/env python3
"""Android game package: the 资源包 v2 data directory (pack/pack.py format) plus the
Android-rewritten modules, all under one index.

  make_pack.py <out dir> --root <product root> [--force]
  make_pack.py <out dir> --synthetic [--force]             a test package with no game data

An existing <out dir> is replaced only when it is a package (manifest.json version 2 next to
index.tsv); anything else is refused unless --force, so a mistyped path never deletes a
product root or game data.

Layout = what pack/ipa.sh packs (exefs/main.npdm, romfs/, romfs.s2aidx, ctl/control.nacp,
systemdata/, aoc.tsv, aoc/, shaders/) + modules/: <module>.patched.bin / .fixups.tsv /
.gates.tsv (/ .widen.tsv) from runtime/rewriter with S2A_TARGET=android, main.npdm and the
widen tables. index.tsv lists every file as path<TAB>bytes<TAB>sha256; game-build-artifact.json
(A0 v1, android/contracts; contentRoot = sha256-<index hash>) sits beside it, outside
the index it names; manifest.json ({"version": 2, files, total, index_sha256}) is written last,
so its presence means complete. ctl/control.nacp is required (the artifact's title id).
pack/pack.py writes the same format but clones files with macOS clonefile; this copies.
Rewriting uses the shared host C CLI (pack/rewriter.py); the Python rewriter was removed 10-31.
Game files, keys and firmware never go into the repository: packages are built from the
player's own dumps (pack/nx_unpack) and stay outside it."""
import argparse, datetime, hashlib, importlib.util, json, os, re, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)   # filecopy.py / direct_library.py 与本脚本同目录；测试用 importlib 从路径载入本脚本时 sys.path 里没有它们
from filecopy import copy_file   # noqa: E402
import direct_library   # noqa: E402  F5.2：图形描述的文件清单与离线库校验只有这一份实现
sys.path.insert(0, os.path.join(REPO, 'tools'))
from module_order import load_order   # noqa: E402  Horizon 装载序（main → subsdk0..N → sdk）：排序规则只有这一份
MODULES = ['main', 'sdk'] + [f'subsdk{i}' for i in range(10)]
REWRITE_ABI = 'android-sp-safe-v1'   # nothing stashed below sp (fixups "# REWRITE_ABI"); BootContract requires it
RUNTIME_VERSION = '0.1.0'
# 直连（NVN->Vulkan）产物：requiredFeatures 里唯一允许出现的能力，descriptor 位置由 BootContract 固定。
GRAPHICS_FEATURE = 'org.s2arm.graphics.v1'
GRAPHICS_MANIFEST = direct_library.DESCRIPTOR_REL
DIRECT_ABI = 1                      # native/direct/s2a_direct.h S2A_DIRECT_ABI


def load_graphics(path):
    """Read and check a graphics.v1.json; the rules live in direct_library.load_descriptor, the
    one implementation shared with the offline library producer, game_apk.py and the tests.
    Returns the summary the packer needs; `files` is the exact file list that must appear in the
    written index with the same bytes. The packer still invents no field: profile hashes,
    translatorClosure and file lists come from the direct pipeline that built it."""
    try:
        descriptor = direct_library.load_descriptor(path)
    except direct_library.LibraryError as error:
        sys.exit(f'graphics descriptor {path}: {error}')
    if descriptor['direct_abi'] != DIRECT_ABI:
        sys.exit(f'graphics descriptor {path}: directABI {descriptor["direct_abi"]} is not {DIRECT_ABI}')
    return descriptor



def rewrite_command():
    """The C rewriter, built and selected the same way as the production refresh pipeline (the Python rewriter was removed 10-31)."""
    spec = importlib.util.spec_from_file_location('android_pack_rewriter', os.path.join(REPO, 'pack/rewriter.py'))
    selector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(selector)
    try:
        selector.backend()
        return [str(selector.c_path())]
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit('【红】Android 改写器：' + str(error))


def validate_module(directory, name, diagnostics=False):
    """Reject incomplete/wrong-platform output before replacing any old package."""
    required = ['patched.bin', 'fixups.tsv', 'gates.tsv']
    if diagnostics:
        required += ['sites.tsv', 'gaps.tsv']
    for suffix in required:
        path = Path(directory) / f'{name}.{suffix}'
        if path.is_symlink() or not path.is_file() or (suffix == 'patched.bin' and not path.stat().st_size):
            sys.exit(f'【红】module {name}: missing/invalid {suffix}')
    h = fixups_header(os.path.join(directory, name + '.fixups.tsv'))
    if h.get('TARGET') != 'android' or h.get('REWRITE_ABI') != REWRITE_ABI:
        sys.exit(f'【红】module {name}: expected TARGET android / REWRITE_ABI {REWRITE_ABI} '
                 f'(actual: TARGET={h.get("TARGET")} REWRITE_ABI={h.get("REWRITE_ABI")})')
    try:
        bias = int(h.get('LOAD_BIAS', '0'), 16)
    except ValueError:
        sys.exit(f'【红】module {name}: invalid LOAD_BIAS')
    if bias:
        sys.exit(f'【红】module {name}: Android does not support nonzero LOAD_BIAS')
    build = h.get('BUILD_ID', '').lower()
    if build and not re.fullmatch(r'[0-9a-f]{32}|[0-9a-f]{40}', build):
        sys.exit(f'【红】module {name}: invalid BUILD_ID')
    if diagnostics:
        with open(os.path.join(directory, name + '.gaps.tsv'), encoding='utf-8') as stream:
            if any(line.strip() and not line.startswith('#') for line in stream):
                sys.exit(f'【红】module {name}: rewriter reported gaps')
    return h


def android_widen_enabled(exefs):
    """Read the same per-product Android capability manifest that enters the package."""
    caps = Path(exefs).parent / 's2a_caps.tsv'
    try:
        lines = caps.read_text(encoding='utf-8').splitlines()
    except (OSError, UnicodeError) as error:
        sys.exit(f'【红】无法读取 Android 能力清单 {caps}: {error}')
    if not lines or lines[0].split('\t')[:2] != ['plat', 'android']:
        sys.exit('【红】s2a_caps.tsv 不是安卓清单')
    rows = [line.split('\t') for line in lines[1:] if line.split('\t', 1)[0] == 'widen']
    if len(rows) != 1 or len(rows[0]) < 2 or rows[0][1] not in ('0', '1'):
        sys.exit('【红】Android 能力清单 widen 缺失、重复或值无效')
    return rows[0][1] == '1'


def rewrite(exefs, staging):
    """Rewrite with explicit Android semantics; never fall back after C failure."""
    widen_enabled = android_widen_enabled(exefs)
    os.makedirs(staging, exist_ok=True)
    if list(Path(staging).glob('*.widen.tsv')):
        sys.exit('【红】Android 改写目录包含残留宽屏点表')
    # 帧率表不从环境继承（#48 D2）：改写器按 fps.tsv 打的读槽指令要 runtime/src/fps.c 往键页槽里写值（fps.c 文件头：
    #   槽没人写游戏就读到 0，塞尔达速度 0 冻住），安卓没链 fps.c（android/core-link/Runtime.cmake）。接上 fps.c 之前不交给改写器。
    env = {k: v for k, v in os.environ.items() if k != 'S2A_FPS_TSV'}
    env.update(S2A_TARGET='android',
               S2A_REWRITE_SPIN=os.environ.get('S2A_REWRITE_SPIN', '0'),
               S2A_SDK_ADDR_CUT=os.environ.get('S2A_SDK_ADDR_CUT', '1'))
    command = rewrite_command()
    # widen=0 不扫描也不传点表；启用时临时点表保活到所有模块改写完成。
    with tempfile.TemporaryDirectory(prefix='s2a-widen-sites-') as tmp:
        sites = None
        if widen_enabled:
            sites = os.path.join(tmp, 'widen_sites.tsv')
            try:
                scan = subprocess.run([os.environ.get('PY', sys.executable), os.path.join(REPO, 'graphics/widen/scan/wide_scan.py'),
                                       exefs, '--tsv', sites], env=env, capture_output=True, text=True)
            except OSError as error:
                sys.exit(f'【红】宽屏候选扫描无法启动: {error}')
            if scan.returncode:
                sys.exit(f'【红】宽屏候选扫描失败 ({scan.returncode}): '
                         f'{(scan.stdout + scan.stderr).strip().splitlines()[-1:]}')
            try:
                path = Path(sites)
                if path.is_symlink() or not path.is_file():
                    raise ValueError('未输出常规点表')
                lines = path.read_text(encoding='utf-8').splitlines()
                if not lines or lines[0] != '模块\t偏移\t种类\t证据':
                    raise ValueError('点表格式无效')
            except (OSError, UnicodeError, ValueError) as error:
                sys.exit(f'【红】宽屏候选扫描结果无效: {error}')
        done = []
        for m in MODULES:
            src = os.path.join(exefs, m)
            if not os.path.isfile(src):
                continue
            try:
                r = subprocess.run(command + [src, staging, m] + ([sites] if sites is not None else []), env=env, capture_output=True, text=True)
            except OSError as error:
                sys.exit(f'【红】rewrite of {m} could not start: {error}')
            if r.returncode:
                sys.exit(f'【红】rewrite of {m} failed ({r.returncode}): '
                         f'{(r.stdout + r.stderr).strip().splitlines()[-1:]}')
            tail = (r.stdout + r.stderr).strip().splitlines()
            if tail:
                print(f'rewrite {m}: {tail[-1]}')
            validate_module(staging, m, diagnostics=True)
            for diag in ('sites.tsv', 'gaps.tsv'):
                os.remove(os.path.join(staging, f'{m}.{diag}'))
            done.append(m)
    if 'main' not in done:
        sys.exit(f'no exefs/main in {exefs}')
    shutil.copy2(os.path.join(exefs, 'main.npdm'), os.path.join(staging, 'main.npdm'))
    return done


def stage_address_requirements(root, staging):
    """The shared v2 collector on the root's own main: static recognition only, no tables, no host logs."""
    root = Path(root)
    main = root / 'exefs/main'
    try:
        raw = main.read_bytes()
        if len(raw) < 0x50 or raw[:4] != b'NSO0':
            raise ValueError('exefs/main 不是 NSO')
    except (OSError, ValueError) as error:
        sys.exit('【红】Android 地址需求采集失败：' + str(error))
    destination = Path(staging) / 'addr_need.tsv'
    command = [os.environ.get('PY', sys.executable), os.path.join(REPO, 'tools/addr_need.py'),
               raw[0x40:0x50].hex(), str(destination), '--npdm', str(root / 'exefs/main.npdm'), '--main', str(main)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        sys.exit('【红】Android 地址需求采集失败：' + (result.stderr or result.stdout).strip())
    return str(destination)


def stage_system_data(root, staging, sysdata=None):
    """Consume system data already prepared in the product root by refresh."""
    del staging
    if sysdata:
        sys.exit('【红】Android 打包只读取产品根 systemdata；请先通过 refresh 的 sysdata 阶段导入')
    existing = Path(root) / 'systemdata'
    result = subprocess.run([sys.executable, os.path.join(REPO, 'pack/sysdata_synth.py'), '--ids'],
                            capture_output=True, text=True)
    if result.returncode:
        sys.exit('【红】无法读取共用系统数据要求：' + result.stderr.strip())
    ids = result.stdout.split()
    if not ids or any(not re.fullmatch(r'[0-9A-F]{16}', sid) for sid in ids):
        sys.exit('【红】共用系统数据要求为空或格式无效')
    def has_payload(path):
        if path.is_symlink() or not path.is_dir():
            return False
        payload = False
        for directory, dirs, files in os.walk(path):
            for name in dirs + files:
                item = Path(directory) / name
                if item.is_symlink() or not (item.is_dir() or item.is_file()):
                    return False
            payload |= any(name != '.DS_Store' and not name.startswith('._') for name in files)
        return payload
    missing = [sid for sid in ids if not has_payload(existing / sid)]
    if missing:
        sys.exit('【红】产品根缺少已准备的系统数据：' + ', '.join(missing) + '；请先运行 refresh 的 sysdata 阶段')
    return str(existing)


def entries_from_root(root, staging, sysdata=None, direct_vulkan=False):
    """(pack path, source) pairs, as pack/ipa.sh chooses them, plus modules/."""
    need = [('exefs/main.npdm', 'exefs/main.npdm'), ('romfs', 'romfs'), ('ctl/control.nacp', 'ctl/control.nacp')]
    for _, rel in need:
        if not os.path.exists(os.path.join(root, rel)):
            sys.exit(f'{root} has no {rel}')
    system_data = stage_system_data(root, staging, sysdata)
    out = [(name, os.path.join(root, rel)) for name, rel in need]
    overlay = os.path.join(root, 'ui_overlay', 'romfs')
    if os.path.isdir(overlay):   # 界面布局覆盖层（pack/orch/stages.py _uilayout）：同 pack/pkg_srcs.py，同包内路径后者优先（romfs.s2aidx 按覆盖层大小建）
        out.append(('romfs', overlay))
    # s2a_caps.tsv: the per-game capability list (fullscreen / 60 fps on unless a strong signal; rewrite stage writes it into the
    # product root for every platform) -- runtime.c applies it from the game data dir before loading modules, like iOS / Mac
    for name in ('romfs.s2aidx', 'systemdata', 'aoc.tsv', 'aoc', 's2a_caps.tsv'):
        if name == 'systemdata':
            if system_data:
                out.append((name, system_data))
        elif name == 's2a_caps.tsv':
            caps_path = os.path.join(root, name)
            # F7 #1574（#996 防重演）：安卓包只收安卓头清单（refresh 走 S2A_PACK_PLAT=android → stage_caps.sh 首行写 plat<TAB>android）。
            #   苹果产物根的清单（plat apple 或没有 plat 行的老格式）混进安卓包、或压根没出清单 = 苹果口径 / 没有口径的能力结论进了安卓 → 红，不出包
            if not os.path.exists(caps_path):
                sys.exit('【红】产物根没有 s2a_caps.tsv：安卓包必须带安卓能力清单；先按 S2A_PACK_PLAT=android 跑 pack/refresh.sh 出清单')
            with open(caps_path, encoding='utf-8', errors='replace') as caps_file:
                if caps_file.readline().rstrip('\n').split('\t')[:2] != ['plat', 'android']:
                    sys.exit('【红】s2a_caps.tsv 不是安卓清单（首行要 plat\\tandroid）：可能混进了苹果产物根的清单；'
                             '先按 S2A_PACK_PLAT=android 跑 pack/refresh.sh 出安卓清单')
            out.append((name, caps_path))
        elif os.path.exists(os.path.join(root, name)):
            out.append((name, os.path.join(root, name)))
    shaders = os.path.join(root, 'shaders')
    if os.path.isdir(shaders):
        if os.environ.get('S2A_ANDROID_SKIP_APPLE_ARTIFACTS') == '1':
            # Preserve the Android direct offline library only; do not bundle Metal outputs.
            direct = os.path.join(shaders, 'vulkan', 'direct')
            if os.path.isdir(direct): out.append(('shaders/vulkan/direct', direct))
        else:
            out += [(f'shaders/{n}', os.path.join(shaders, n)) for n in sorted(os.listdir(shaders)) if n != 'work']
    rewrite(os.path.join(root, 'exefs'), staging)
    # Reuse the Apple NRO export survey, not replacement SDK implementations.
    nro_table = os.path.join(os.path.dirname(staging), 'nro_exports.tsv')
    result = subprocess.run([sys.executable, os.path.join(REPO, 'tools/nro_exports.py'),
                             os.path.join(root, 'romfs'), nro_table], capture_output=True, text=True)
    if result.returncode:
        sys.exit('【红】NRO导出表生成失败：' + result.stderr.strip() + result.stdout.strip())
    if os.path.isfile(nro_table):
        out.append(('nro_exports.tsv', nro_table))
    if direct_vulkan:
        from vulkan_modules import replaced_modules
        replaced = replaced_modules(os.path.join(root, 'exefs'))
        with open(os.path.join(staging, 'direct-vulkan.txt'), 'w', encoding='ascii') as f:
            f.write('S2ARM_VULKAN_DIRECT_V1\n' + ''.join(n + '\n' for n in replaced))
        # modules/ is removed from the external resource package after binding
        # signed APK assets. Keep an indexed data copy for cold-start selection.
        out.append(('direct-vulkan.txt', os.path.join(staging, 'direct-vulkan.txt')))
        print('Vulkan direct candidate: retain all signed modules; direct load excludes ' + ', '.join(replaced))
    requirements = stage_address_requirements(root, staging)
    out.append(('exefs/addr_need.tsv', requirements))
    for t in sorted(os.listdir(root)):   # widescreen tables travel with the modules (runtime/src/widen.c reads them there)
        if t.startswith(('widen_enable', 'widen_full', 'widen_verified')):
            if t == 'widen_verified.tsv':
                # F7：验证标记带平台列，只收含安卓行的那份；苹果行（或老的 5 列行）不是安卓验证 → 不进包
                with open(os.path.join(root, t), encoding='utf-8', errors='replace') as vf:
                    if not any(len(c) > 3 and c[3].strip().lower() == 'android'
                               for c in (line.split('\t') for line in vf)
                               if c[0].strip() and not c[0].startswith('#')):
                        continue
            shutil.copy2(os.path.join(root, t), os.path.join(staging, t))
    out.append(('modules', staging))
    return out


def synthetic_root(tmp):
    """A product root with no game data: the rewrite self-test module as main, a two-file
    romfs and a minimal NACP (title 0x0100000000ABCDEF, "S2Arm synthetic", version 0.1)."""
    root = os.path.join(tmp, 'root')
    # Synthetic fixtures explicitly prepare their own synthetic system data.
    system = os.path.join(root, 'systemdata')
    os.makedirs(system)
    ids = subprocess.run([sys.executable, os.path.join(REPO, 'pack/sysdata_synth.py'), '--ids'],
                         capture_output=True, text=True, check=True).stdout.split()
    for sid in ids:
        subprocess.run([sys.executable, os.path.join(REPO, 'pack/sysdata_synth.py'), sid,
                        os.path.join(system, sid)], check=True, capture_output=True)
    # Synthetic input has no game capability claims, but must satisfy the Android manifest contract.
    with open(os.path.join(root, 's2a_caps.tsv'), 'w') as f:
        f.write('plat\tandroid\n')
    os.makedirs(os.path.join(root, 'exefs'))
    with open(os.path.join(root, 'unpack.tsv'), 'w') as f:
        f.write('title_id\t0100000000ABCDEF\ndisplay_version\t0.1\n')
    # S2A_SYNTHETIC_SDK=1：合成模块额外导出运行时要求的 7 个 SDK 入口（直连链路探针要走到 managed 侧）。
    nso = [os.environ.get('PY', sys.executable), os.path.join(HERE, 'selftest_nso.py'),
           os.path.join(root, 'exefs', 'main')]
    if os.environ.get('S2A_SYNTHETIC_SDK') == '1':
        nso.append('--sdk-entries')
    subprocess.run(nso, check=True)
    with open(os.path.join(root, 'exefs', 'main.npdm'), 'wb') as f:
        meta = bytearray(0x98); meta[:4] = b'META'
        struct.pack_into('<I', meta, 0x70, 0x80)
        meta[0x80:0x84] = b'ACI0'
        struct.pack_into('<Q', meta, 0x90, 0x0100000000ABCDEF)
        f.write(meta)
    os.makedirs(os.path.join(root, 'romfs', 'data'))
    with open(os.path.join(root, 'romfs', 'readme.txt'), 'w') as f:
        f.write('S2Arm synthetic package: no game data.\n')
    with open(os.path.join(root, 'romfs', 'data', 'blob.bin'), 'wb') as f:
        f.write(bytes(range(256)) * 64)
    nacp = bytearray(0x4000)
    nacp[0:15] = b'S2Arm synthetic'                 # title entry 0 (American English) name
    nacp[0x200:0x205] = b'S2Arm'                     # its publisher
    struct.pack_into('<Q', nacp, 0x3038, 0x0100000000ABCDEF)   # presence group id = application id
    nacp[0x3060:0x3063] = b'0.1'                     # display version
    os.makedirs(os.path.join(root, 'ctl'))
    with open(os.path.join(root, 'ctl', 'control.nacp'), 'wb') as f:
        f.write(nacp)
    synthetic_caps(root)
    return root


def synthetic_caps(root):
    """Give a synthetic product root its Android capability list the way refresh does
    (pack/gen_caps.sh with S2A_PACK_PLAT=android), so test roots meet the same packing
    precondition as real ones instead of each fixture hand-writing a copy."""
    result = subprocess.run(['bash', os.path.join(REPO, 'pack/gen_caps.sh'), 'synthetic', root,
                             os.path.join(root, 's2a_caps.tsv')],
                            env=dict(os.environ, S2A_PACK_PLAT='android', S2A_CAPS_TARGET='android'),
                            capture_output=True, text=True)
    if result.returncode:
        sys.exit('【红】合成产物根能力清单生成失败（pack/gen_caps.sh）：' + (result.stdout + result.stderr).strip())


def is_pack(path):
    """A directory this tool (or pack/pack.py) wrote: a version 2 manifest beside index.tsv."""
    try:
        with open(os.path.join(path, 'manifest.json')) as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        return False
    return (isinstance(manifest, dict) and manifest.get('version') == 2
            and os.path.isfile(os.path.join(path, 'index.tsv')))


def check_output(out, force):
    """Exit unless out is absent, a package, or --force was given."""
    if os.path.lexists(out) and not (force or (os.path.isdir(out) and not os.path.islink(out) and is_pack(out))):
        sys.exit(f'{out} exists and is not a package (no version 2 manifest.json + index.tsv); '
                 'pick another output, or pass --force to replace it')


def write_pack(out, entries, force=False, graphics=None):
    # main 的替换前准入：旧包只有在所有模块都通过硬件/ABI 校验后才删除。
    for name, source in entries:
        if name == 'modules':
            images = sorted(Path(source).glob('*.patched.bin'))
            if not any(p.name == 'main.patched.bin' for p in images):
                sys.exit('【红】package has no modules/main.patched.bin')
            for image in images:
                validate_module(source, image.name[:-len('.patched.bin')])
    if os.path.lexists(out):
        check_output(out, force)
        if os.path.isdir(out) and not os.path.islink(out):
            shutil.rmtree(out)
        else:
            os.remove(out)
    os.makedirs(out)
    files = []
    for name, src in entries:
        if os.path.isfile(src):
            files.append((name, src))
            continue
        for d, ds, fs in os.walk(src):
            ds.sort()
            for f in sorted(fs):
                if f == '.DS_Store' or f.startswith('._'):
                    continue
                p = os.path.join(d, f)
                rel = name + '/' + os.path.relpath(p, src).replace(os.sep, '/')
                if '\t' in rel or '\n' in rel:
                    sys.exit(f'path with a tab / newline cannot go into index.tsv: {rel!r}')
                files.append((rel, p))
    # 同包内路径后给的源优先、位置不变（界面布局覆盖层叠在 romfs/ 上，同 pack/pack.py）
    pos = {}
    for i, (rel, src) in enumerate(files):
        if rel in pos:
            files[pos[rel]] = (rel, src)
            files[i] = None
        else:
            pos[rel] = i
    files = [f for f in files if f is not None]
    lines = []
    rows = []
    total = 0
    for rel, src in files:
        dst = os.path.join(out, rel)
        h = hashlib.sha256()
        # copy_file 两条路都已把 dst 复制好（CoW 共享数据块或 shutil.copy2，权限随源）：
        # 这里只读 dst 算哈希，不再二次写入——源是只读（0444，RomFS 共享存储）时二次写会 PermissionError。
        copy_file(src, dst)
        with open(dst, 'rb') as a:
            for chunk in iter(lambda: a.read(1 << 22), b''):
                h.update(chunk)
        size = os.path.getsize(dst)
        total += size
        lines.append(f'{rel}\t{size}\t{h.hexdigest()}\n')
        rows.append((rel, size, h.hexdigest()))
    index = ''.join(lines)
    with open(os.path.join(out, 'index.tsv'), 'w', newline='\n') as f:
        f.write(index)
    index_sha = hashlib.sha256(index.encode()).hexdigest()
    write_artifact(out, index_sha, len(files), total, graphics)
    if graphics is not None:
        # 描述里列出的每个文件都必须真的进了 index 且字节/摘要相符，也不能有未列出的文件：
        # 之前 files[] 只验“是数组”，于是手写描述可以和包内容完全脱节。
        try:
            direct_library.verify_pack_files(out, graphics, rows)
        except direct_library.LibraryError as error:
            sys.exit(str(error))
    with open(os.path.join(out, 'manifest.json.tmp'), 'w') as f:   # last: a manifest means a complete pack
        json.dump({'version': 2, 'files': len(files), 'total': total, 'index_sha256': index_sha}, f, indent=1)
    os.replace(os.path.join(out, 'manifest.json.tmp'), os.path.join(out, 'manifest.json'))
    return len(files), total


def fixups_header(path):
    """The '# KEY<TAB>value' lines a fixups.tsv starts with."""
    out = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            if not line.startswith('# '):
                break
            key, _, value = line[2:].rstrip('\n').partition('\t')
            if not _ or key in out:
                sys.exit(f'【红】invalid/duplicate fixups header {key}')
            out[key] = value
    return out


def write_artifact(out, index_sha, files, total, graphics=None):
    """game-build-artifact.json (A0 v1, android/contracts) beside manifest.json: what the
    device installer checks before activating the package, and what BootContract reads at launch.
    It names index.tsv's hash, so it is not listed in index.tsv itself. contentRoot is that hash."""
    with open(os.path.join(out, 'ctl', 'control.nacp'), 'rb') as f:
        nacp = f.read()
    if len(nacp) < 0x3040 or not struct.unpack_from('<Q', nacp, 0x3038)[0]:
        sys.exit('ctl/control.nacp has no application id')
    # modules 顺序 = 装载顺序（BootContract → load_modules_from_source → loader.c 全局符号解析），按 Horizon 序写：
    # main → subsdk0..N（数字序）→ sdk；字母序会让 subsdk 里和 sdk 重名的符号错绑到 sdk。
    modules = []
    # Path.glob 只把 pattern 段当模式、基路径按字面量走：out 带 [ ] 等元字符时
    # glob.glob(join(out, …)) 会把目录名当字符类 → 静默零匹配 → modules:[] 出包（qa-169163 实测）。
    for name in load_order(p.name[:-len('.patched.bin')]
                           for p in Path(out, 'modules').glob('*.patched.bin')):
        h = validate_module(os.path.join(out, 'modules'), name)
        m = {'name': name, 'source': 'revision', 'image': f'modules/{name}.patched.bin',
             'fixups': f'modules/{name}.fixups.tsv', 'gates': f'modules/{name}.gates.tsv', 'target': 'android'}
        if os.path.isfile(os.path.join(out, 'modules', name + '.widen.tsv')):
            m['widen'] = f'modules/{name}.widen.tsv'
        build = h.get('BUILD_ID', '').lower()
        if build:
            m['buildId'] = build
        modules.append(m)
    # SOURCE_DATE_EPOCH keeps the artifact (and its hash) reproducible across identical builds.
    stamp = datetime.datetime.fromtimestamp(int(os.environ.get('SOURCE_DATE_EPOCH', '0')) or datetime.datetime.now().timestamp(),
                                            datetime.timezone.utc)
    artifact = {
        'version': 1, 'titleId': f'{struct.unpack_from("<Q", nacp, 0x3038)[0]:016X}', 'contentRoot': 'sha256-' + index_sha,
        'target': 'android', 'guestAbi': 'aarch64', 'rewriteAbi': REWRITE_ABI,
        'runtime': {'version': RUNTIME_VERSION, 'backend': 'vulkan'},
        'package': {'manifest': 'manifest.json', 'indexSha256': index_sha, 'files': files, 'bytes': total},
        'npdm': 'exefs/main.npdm', 'control': 'ctl/control.nacp', 'romfs': 'romfs', 'modules': modules,
        'createdAt': stamp.isoformat(timespec='seconds').replace('+00:00', 'Z'),
        'toolchain': {'rewriter': f'runtime/rewriter@{REWRITE_ABI}', 'packer': 'android/tools/make_pack.py@1'},
    }
    for key, rel in (('systemData', 'systemdata'), ('shaders', 'shaders')):
        if os.path.isdir(os.path.join(out, rel)):
            artifact[key] = rel
    if os.path.isfile(os.path.join(out, 'aoc.tsv')) and os.path.isdir(os.path.join(out, 'aoc')):
        artifact['aoc'] = {'index': 'aoc.tsv', 'root': 'aoc'}
    if graphics is not None:
        # 只有图形这一项可选能力由本仓实现；requiredFeatures 里出现别的名字，BootContract 会拒绝整个包。
        if graphics['title_id'] != artifact['titleId']:
            sys.exit(f"graphics descriptor titleId {graphics['title_id']} != package {artifact['titleId']}")
        descriptor = os.path.join(out, GRAPHICS_MANIFEST)
        if not os.path.isfile(descriptor):
            sys.exit(f'graphics descriptor was not packed at {GRAPHICS_MANIFEST}')
        with open(descriptor, 'rb') as f:
            packed = f.read()
        if len(packed) != graphics['bytes'] or hashlib.sha256(packed).hexdigest() != graphics['sha256']:
            sys.exit('packed graphics descriptor does not match the input bytes')
        artifact['requiredFeatures'] = [GRAPHICS_FEATURE]
        artifact['extensions'] = {GRAPHICS_FEATURE: {'manifest': GRAPHICS_MANIFEST,
                                                    'sha256': graphics['sha256'],
                                                    'bytes': graphics['bytes']}}
    with open(os.path.join(out, 'game-build-artifact.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(artifact, f, ensure_ascii=False, indent=1)
        f.write('\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--root')
    source.add_argument('--synthetic', action='store_true')
    parser.add_argument('--sysdata', help='unsupported here; pass player system data to refresh')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--direct-vulkan', action='store_true', help='experimental direct Vulkan module-selection descriptor')
    parser.add_argument('--direct-graphics', metavar='DESCRIPTOR',
                        help='graphics.v1.json from the direct pipeline: packs it at ' + GRAPHICS_MANIFEST +
                             ' and declares requiredFeatures [' + GRAPHICS_FEATURE + ']')
    args = parser.parse_args()
    if args.sysdata:
        parser.error('--sysdata must be supplied to refresh; make_pack reads the prepared product root')
    if args.synthetic and args.direct_vulkan:
        parser.error('--direct-vulkan requires a real Vulkan product root')
    out = os.path.abspath(args.out)
    check_output(out, args.force)   # before any rewriting, so a wrong path fails at once
    if args.root:
        for source_dir in (args.root,):
            if not source_dir:
                continue
            src, dst = os.path.realpath(source_dir), os.path.realpath(out)
            try:
                overlap = os.path.commonpath((src, dst)) in (src, dst)
            except ValueError:  # different Windows drives cannot overlap
                overlap = False
            if overlap:
                parser.error('output must not overlap the product root')
    graphics = load_graphics(args.direct_graphics) if args.direct_graphics else None
    with tempfile.TemporaryDirectory() as tmp:
        root = args.root if args.root else synthetic_root(tmp)
        if graphics is not None and os.path.isfile(os.path.join(root, GRAPHICS_MANIFEST)):
            parser.error(f'{GRAPHICS_MANIFEST} already exists under the product root; '
                         'keep one source: pass it with --direct-graphics instead')
        entries = entries_from_root(root, os.path.join(tmp, 'modules'),
                                    '' if args.synthetic else args.sysdata, args.direct_vulkan)
        if graphics is not None:
            entries.append((GRAPHICS_MANIFEST, args.direct_graphics))
        n, total = write_pack(out, entries, args.force, graphics)
    print(f'{n} files, {total} bytes: {out}')


if __name__ == '__main__':
    main()
