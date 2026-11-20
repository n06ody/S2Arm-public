#!/usr/bin/env python3
# 直连库每对着色器（离线配对表 ∪ 管线种子表）的拉取基础管线归档（出包时离线编，随包放 shaders/direct/pipelines/）：
#   python3 base_pipelines.py <shaders/direct 目录> <输出 pipelines 目录> --plat ios|macos --os <系统大版本> --arch <GPU 架构> [--arch …] [--cache <目录>] [-j <路数>]
# 为什么：混合管线（nvn_hybrid.inc hybrid_get）首遇一对着色器时要它的「拉取基础管线」（拉取式顶点 <键>_bp + 片元、颜色状态留空、不带顶点描述），
#   原先只能在设备上后台用 Metal 4 编译器现建，建好之前这对的首遇照旧跳过 / 等精确管线（Ori 2 菜单进序章：提交线程首遇现建 42 条、合计 825 ms）。
#   这里在出包机上按设备同一描述离线编好，设备首遇先查随包归档（MTL4Archive 只查不编），命中直接按本笔颜色状态特化，不再后台建。
#   做法照上游参考实现（MIT）pso_offline.py：每对一个基础管线、Metal 4 管线脚本（.mtl4-json）交 metal-tt。
# 描述 = 设备 nvn_mtl.m m4_render_desc(d, 拉取顶点, 片元, 无顶点描述, unspec=1)，混合管线只给能用拉取管线的笔建，所以这些字段离线就定得下：
#   顶点 = <键>_bp 库（容器种类 55，逐键编、入口不带前缀）里的 pull.entry（反射写死 main0，nvn_mtl.m pull_refl_ok）；
#   片元 = 库段（种类 0）里 fn_prefix + entry（设备 fs->plib / fs->fn）；函数声明了函数常量的，套一层特化描述、常量全取缺省（设备 spec_probe_fn 同规则：按库里原函数判）；
#   8 个颜色附件全部留空（Unspecialized；设备 spec_probe_color unspec 同，不再按本笔画布把没绑的写 Invalid——离线不知道画布，写了也对不上）；
#   不带顶点描述、没有深度 / 模板格式（Metal 4 描述本来就没有）；光栅采样数 1、alpha-to-coverage / alpha-to-one 关、图元拓扑类未指定（写层号的顶点才指定，混合管线不收）。
#   离线不枚举、交给设备的维度：多重采样（2 / 4 / 8）、开 alpha-to-coverage / alpha-to-one 的笔——描述不同，查归档不中，照旧后台建；
#   近平面 0 深度模式（游戏调 nvnDeviceSetDepthMode，出包时不知道）的入口 main0_z：文件名带入口名，这里只出 main0（NVN 缺省的 GL 模式），设什么模式的游戏都不会拿错。
# 文件名 = 设备 hybrid_get 现算：「p」+ sha256("<顶点键>_b\t<片元键>_b\t<顶点入口>") 前 20 字节十六进制，扩展名 .mtl4archive（与计算管线的 .metalarchive 分开：
#   那种是 Metal 3 MTLBinaryArchive，pso_archive.h par_open_dir 只收 .metalarchive；这种是 Metal 4 MTL4Archive，设备 newArchiveWithURL 打开）。
# iPhone 包（--plat ios）逐对 lzfse 压缩，文件名再加 .lzfse、不留原样（11-19 实测真实基础管线 lzfse 后约 53%）；设备首遇当场解这一份（nvn_hybrid.inc hybrid_pkg_open：
#   Foundation 解压 → 临时文件 → 打开 → 立即删，真机 0.41–0.51 ms / 对）。Mac 包（--plat macos）不压：盘位宽裕，省掉设备上写临时文件。
#   压缩用 libcompression（ctypes 进程内调：与设备 Foundation 的 NSDataCompressionAlgorithmLZFSE 同一个库、同一种裸 lzfse 流，11-19 本机核过与
#   compression_tool -encode -a lzfse 逐字节相同）：上万对不必每对起一个子进程、落一个中间文件。
# 首查预热对（s2a_warm.metallib + s2a_warm.mtl4archive[.lzfse]）：进程里第一次从 Metal 4 归档查中管线有一次性初始化（真机 16–25 ms），
#   设备开局在后台查中这一对把它付掉（nvn_hybrid.inc hybrid_pkg_warm），不落在提交线程的第一次首遇上。只有查中才付这笔（11-19 本机：拿别的函数 / 别的附件写法查、
#   查不中，之后第一次查中照样 6–9 ms；先查中一对不相干的小库，之后 0.16 ms），所以预热对必须查得中：函数库随包（设备现编出来的函数哈希对不上归档）。
# 一对一个文件、只拿用到的两个函数各自的模块出（cs_pipelines.py 同法：metal-tt 会把交给它的库原样装进归档，批库一段被上百个键共用）：
#   MTL4Archive 打开时按文件大小 1:1 占地址（graphics/metal/gal_core.m 10-21 巫师3 低区崩），一对一个文件 = 设备只映射本局真用到的对。
#   代价：同一片元配几个顶点时它的库在每个文件里各带一份（11-19 本机实测 3 对共用片元：分开 249696 字节、合进一个文件 169664 字节，多约 1/3）。
# metal-tt / 缓存 / 进程组 / 重试与 cs_pipelines.py 同一套（从那里引用）。缓存键 = 本工具格式号 + 两个函数的模块 sha256（或整段库）+ 特化与否 + 入口 + 平台 + 大版本 + 架构表 + metal-tt 版本。
#   缓存存的就是随包的那份（iPhone 压过的、Mac 原样）：键里本来就有平台，两种不会互相命中；命中只克隆、不再压，iPhone 那份缓存也省一半盘。
# 全装：plan 能出的对全部编、全部随包，不设体积上限（11-20 维护者：「规定包里最多放原件体积的 10%，这是啥规定，没这个规定啊，玩家玩着不卡更重要」）。
#   原先按原件 10% 预算按组装填（11-19 我方自定的工程上限），Ori 2 可出 14935 对只装进 4776 对，序章真用到的 181 对里只有 44 对随包；全装后 181 对全在。
#   代价（11-20 本机实测 Ori 2，Mac 原样、单架构）：随包 464 MB → 1422 MB（原件的 30.6%），这一步 CPU 1641 → 4951 秒。
# 输出：标准输出最后一行一句读数（总对数、跳过、出 / 失败对数、随包字节、每对字节）；有对（或预热对）编不出 → 照出其余、退出 3（调用方打黄）；参数 / 环境错 → 其他非 0。
import argparse
import ctypes
import hashlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cs_pipelines import container, lib_functions, tt_command, tt_suffix, run_tt, clone   # 同一套库读法与 metal-tt 调法
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from pool_workers import cores   # 核数 / 进程池路数唯一来源（tools/pool_workers.py，#1332）

FORMAT = 'base-pipelines 2'   # 改了产物形状（描述、文件名、脚本格式、压缩）就加一，旧缓存全部不中（2：iPhone 逐对 lzfse，缓存存压过的）
K_B, K_JSON, K_BP = 0, 1, 55   # 库容器种类：库段、反射、拉取式顶点库（graphics/nvn/lib_pack_core.inc）
PULL_SLOT = 16                 # 布局块槽 = VB_SLOT(0)（nvn_hybrid.inc PULL_SLOT），反射 pull.slot 不是它设备不用
ENTRY = 'main0'
EXT = '.mtl4archive'
WARM = 's2a_warm'   # 首查预热对的文件名（设备 nvn_hybrid.inc hybrid_pkg_warm 同名）：<WARM>.metallib + <WARM>.mtl4archive[.lzfse]
WARM_SRC = ('#include <metal_stdlib>\nusing namespace metal;\n'   # 无输入、无绑定，只为让设备开局查中一次（函数名设备同名）
            'vertex float4 s2a_warm_v(uint v [[vertex_id]]) { return float4(0, 0, 0, 1); }\n'
            'fragment float4 s2a_warm_f() { return float4(0); }\n')
COLOR = ('pixel_format', 'write_mask', 'blending_state', 'alpha_blend_operation', 'rgb_blend_operation', 'destination_alpha_blend_factor',
         'destination_rgb_blend_factor', 'source_alpha_blend_factor', 'source_rgb_blend_factor')


def bkey(vs, fs, entry=ENTRY):
    """设备 hybrid_get 的随包文件名（不含扩展名）：vs / fs 是 16 位键"""
    return 'p' + hashlib.sha256(f'{vs}_b\t{fs}_b\t{entry}'.encode()).hexdigest()[:40]


def declares_constants(b):
    """metallib 每个函数声不声明函数常量 → {函数名: bool}。公开元数据区（头偏移 0x28 / 0x30）逐函数一段：u32 长度 + 标签串，CNST 标签 = u16 个数 + 各常量；
    顺序同函数表。设备按 functionConstantsDictionary.count 判（spec_probe_fn），这里按 CNST 个数判。读不了 → ValueError"""
    if len(b) < 88 or b[:4] != b'MTLB':
        raise ValueError('不是 metallib')
    flo, _, po, ps = struct.unpack_from('<4Q', b, 24)
    n = struct.unpack_from('<I', b, flo)[0]; p = flo + 4; names = []

    def tags(q, end):
        t = {}
        while q + 4 <= end:
            tag = b[q:q + 4]; q += 4
            if tag == b'ENDT': return t
            ln = struct.unpack_from('<H', b, q)[0]; t[tag] = b[q + 2:q + 2 + ln]; q += 2 + ln
        raise ValueError('标签串没有 ENDT')
    for _ in range(n):
        sz = struct.unpack_from('<I', b, p)[0]; names.append(tags(p + 4, p + sz)[b'NAME'].rstrip(b'\0').decode('utf-8', 'replace')); p += sz
    out, p = {}, po
    for nm in names:
        sz = struct.unpack_from('<I', b, p)[0]; t = tags(p + 4, p + 4 + sz)
        out[nm] = bool(t.get(b'CNST')) and struct.unpack_from('<H', t[b'CNST'], 0)[0] > 0; p += 4 + sz
    if p != po + ps:
        raise ValueError('公开元数据与函数表对不上')
    return out


def table_pairs(direct):
    """离线配对表 shader_pairs.tsv：每行「顶点键\t片元键」（空行 / # 注释跳过）；没有这个文件 = 空"""
    f = direct / 'shader_pairs.tsv'
    if not f.is_file(): return []
    out = []
    for line in f.read_text(encoding='utf-8').splitlines():
        c = line.split('\t')
        if len(c) >= 2 and not line.startswith('#') and len(c[0]) == 16 and len(c[1]) == 16: out.append((c[0], c[1]))
    return out


def seed_pairs(direct):
    """管线种子表 pso_manifest.tsv（种子阶段 pso_seed.py 按本包库键滤过：我方真跑 + 众包 + 外部公开清单）R 行里的顶点 / 片元着色器键。
    R 行 = pso_skey 十六进制，前 24 字节顶点键串、再 24 字节片元键串（C 字符串「<16 位键>_b」，nvn_pso_skey.h）；没有种子表 = 空"""
    f = direct / 'pso_manifest.tsv'
    if not f.is_file(): return []
    out = []
    for line in f.read_text(encoding='utf-8').splitlines():
        c = line.split('\t')
        if len(c) < 2 or c[0] != 'R' or len(c[1]) < 96: continue
        try: raw = bytes.fromhex(c[1][:96])
        except ValueError: continue
        v, fr = (raw[o:o + 24].split(b'\0')[0].decode('latin1') for o in (0, 24))
        v, fr = v[:-2] if v.endswith('_b') else v, fr[:-2] if fr.endswith('_b') else fr
        if len(v) == 16 and len(fr) == 16: out.append((v, fr))
    return out


def read_pairs(direct):
    """要出的对 = 离线配对表 ∪ 管线种子表，去重保序（配对表在前）→ ([(顶点键, 片元键)], {来源: 对数})。
    11-19 维护者定「全对」：BNSH 系游戏的配对表是离线认出的真对；Unity 系的配对表几乎是空的，先靠种子（真跑见过的对）。两边并起来。
    体积靠 iPhone 逐对 lzfse 压（真实基础管线约 53%：全对动森约 320 MB、王国之泪约 1.04 GB，单架构）；Mac 不压"""
    t, s = table_pairs(direct), seed_pairs(direct)
    ts, ss = set(t), set(s)
    seen, out = set(), []
    for pr in t + s:
        if pr not in seen: seen.add(pr); out.append(pr)
    return out, {'只在配对表': len(ts - ss), '只在种子': len(ss - ts), '两边都有': len(ts & ss)}


_LC = None


def lzfse(data):
    """裸 lzfse 流（libcompression compression_encode_buffer，COMPRESSION_LZFSE = 0x801；设备 NSData decompressedDataUsingAlgorithm:LZFSE 解）。压不出 → ValueError"""
    global _LC
    if _LC is None:
        lc = ctypes.CDLL('/usr/lib/libcompression.dylib')
        lc.compression_encode_buffer.restype = ctypes.c_size_t
        lc.compression_encode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
        _LC = lc
    cap = len(data) + 4096   # 压不动的块 lzfse 原样存（bvx- 块头 + 结束标记），多不出几十字节
    dst = ctypes.create_string_buffer(cap)
    n = _LC.compression_encode_buffer(dst, cap, data, len(data), None, 0x801)
    if not n: raise ValueError('libcompression 压不出（返回 0）')
    return dst.raw[:n]


def plan(lib, pairs):
    """[(顶点键, 片元键)] → 要出的 [(顶点键, 片元键, 顶点入口, 片元函数名)] 与跳过计数。跳过 = 设备也不会给这对建拉取基础管线的（pull_fn / hybrid_fits）"""
    js, jobs = {}, []
    left = dict.fromkeys(('顶点没有拉取库', '写层号', '片元不在库里'), 0)

    def refl(k):
        if k not in js:
            try: js[k] = json.loads(lib.read(int(k, 16), K_JSON))
            except (KeyError, ValueError): js[k] = None
        return js[k]
    for v, f in pairs:
        vj, fj = refl(v), refl(f)
        p = vj.get('pull') if isinstance(vj, dict) else None   # 没有反射 = 设备载不出这支顶点，更没有拉取库
        if not (isinstance(p, dict) and p.get('slot') == PULL_SLOT and p.get('entry') == ENTRY) or (int(v, 16), K_BP) not in lib.records:
            left['顶点没有拉取库'] += 1; continue
        if vj.get('writes_layer') or vj.get('entry_layer'): left['写层号'] += 1; continue   # 设备 layer_out：pull_fn 不给
        if not isinstance(fj, dict) or (int(f, 16), K_B) not in lib.records or not fj.get('entry'): left['片元不在库里'] += 1; continue
        jobs.append((v, f, ENTRY, (fj.get('fn_prefix') or '') + fj['entry']))
    return jobs, left


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('direct', type=Path); ap.add_argument('out', type=Path)
    ap.add_argument('--plat', choices=('ios', 'macos'), required=True); ap.add_argument('--os', type=int, required=True)
    ap.add_argument('--arch', action='append', required=True); ap.add_argument('--cache', type=Path)
    ap.add_argument('-j', type=int, default=cores())
    a = ap.parse_args()
    archs = sorted(set(a.arch))
    lib = container(a.direct)
    pairs, src = read_pairs(a.direct)
    jobs, left = plan(lib, pairs)
    skip = '、'.join(f'{k} {n}' for k, n in left.items() if n) or '无'
    head = f'拉取基础管线归档：着色器对 {len(pairs)} 对（{"、".join(f"{k} {n}" for k, n in src.items())}；跳过：{skip}）'
    a.out.mkdir(parents=True, exist_ok=True)
    ext = EXT + '.lzfse' if a.plat == 'ios' else EXT   # 随包文件扩展名：iPhone 逐对压，Mac 原样
    path = lambda job: a.out / f'{bkey(job[0], job[1], job[2])}{ext}'
    res = {}   # 对 → (结果, 按整段库带函数次数)；结果 'hit' / 'built' / (退出码, 谁, 原因)
    used, warm = 0, '没出（0 对）'
    if jobs:
        argv0, ttpath, ttver = tt_command(a.plat)
        env = dict(os.environ, DIRHELPER_USER_DIR_SUFFIX=tt_suffix(ttpath)) if ttver != 'fake' else dict(os.environ)
    work = Path(tempfile.mkdtemp(prefix='base_pipelines.', dir=a.out.parent))
    try:
        sdk = 'iphoneos' if a.plat == 'ios' else 'macosx'
        tv = ['-arch'] * (2 * len(archs)); tv[1::2] = archs
        flag = f'-{a.plat}-version-min'
        segs, info = {}, {}   # 段 sha256 → (字节, 函数表, 常量表)
        smu = threading.Lock()

        def seg(key, kind):
            data = lib.read(int(key, 16), kind); sh = hashlib.sha256(data).hexdigest()
            with smu:
                if sh not in info:
                    try: info[sh] = (lib_functions(data), declares_constants(data))
                    except (ValueError, struct.error, KeyError, UnicodeDecodeError): info[sh] = ({}, None)
            return data, sh, info[sh]

        def fn_lib(key, kind, fn):
            """这个函数的库：能取出它自己的模块（sha256 = 函数表 HASH）就包成单函数库，否则整段；→ (库路径, 身份串, 声明常量吗, 整段吗) 或 None（库里没有这个函数 / 读不出常量表）"""
            data, sh, (fns, consts) = seg(key, kind)
            if fn not in fns or consts is None or fn not in consts: return None
            h, o, sz = fns[fn]; mod = data[o:o + sz]
            if mod and hashlib.sha256(mod).hexdigest() == h:
                ml = work / f'm{h}.metallib'
                if not ml.is_file():   # 两路同时包同一个模块：各写各的临时件、结果相同，后改名的覆盖先的
                    t = f'm{h}.{threading.get_ident()}'; air, tmp = work / f'{t}.air', work / f'{t}.metallib'
                    air.write_bytes(mod)
                    p = subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(tmp)], capture_output=True)
                    try: ok = not p.returncode and lib_functions(tmp.read_bytes()).get(fn, ('',))[0] == h
                    except (OSError, ValueError, struct.error): ok = False
                    if ok: os.replace(tmp, ml)
                if ml.is_file(): return ml, 'm' + h, consts[fn], False
            sp = work / f's{sh}.metallib'
            with smu:
                if not sp.is_file(): sp.write_bytes(data)
            return sp, 's' + sh, consts[fn], True

        def build(dst, vl, entry, fl, fname):
            """一条基础管线 → dst（随包那份：iPhone 压过的）；→ 'hit' 缓存命中、'built' 现编，或 (退出码, 谁, 原因)"""
            ck = hashlib.sha256('\n'.join([FORMAT, vl[1], entry, str(vl[2]), fl[1], fname, str(fl[2]), a.plat, str(a.os), ' '.join(archs), ttver]).encode()).hexdigest()
            cf = a.cache / ck[:2] / f'{ck}.e' if a.cache else None
            if cf and cf.is_file() and cf.stat().st_size:
                clone(cf, dst); os.utime(cf); return 'hit'
            fds, sps = [], []

            def fd(label, name, spec):
                fds.append({'label': label, 'name': name, 'library': 'L' + label})
                if not spec: return 'fnd:' + label
                sps.append({'label': label + 'c', 'function_descriptor': 'fnd:' + label, 'constant_values': []})   # 常量全取缺省 = 设备 [MTLFunctionConstantValues new]
                return 'fnd:' + label + 'c'
            pipe = {'vertex_function_descriptor': fd('V', entry, vl[2]), 'fragment_function_descriptor': fd('F', fname, fl[2]),
                    'color_attachments': [dict.fromkeys(COLOR, 'Unspecialized') for _ in range(8)]}
            fdd = {'library_function_descriptors': fds}
            if sps: fdd['specialized_function_descriptors'] = sps
            stem = dst.name.split('.')[0]
            script = work / f'{stem}.mtl4-json'
            script.write_text(json.dumps({'version': {'major': 0, 'minor': 1, 'sub_minor': 0}, 'generator': 'MetalFramework',
                                          'libraries': [{'label': 'LV', 'path': str(vl[0])}, {'label': 'LF', 'path': str(fl[0])}],
                                          'function_descriptors': fdd, 'pipeline_descriptors': {'render_pipeline_descriptors': [pipe]}}))
            tmp = work / f'{stem}.bin'
            for n in range(6):
                rc, text = run_tt(argv0 + tv + [flag, f'{a.os}.0', str(script), '-o', str(tmp)], env, work / f'{stem}.log')
                if rc == 0 or n == 5 or 'No available translator' not in text: break
                time.sleep(0.2 * (n + 1) + random.random() * 0.2)
            if rc or not tmp.is_file() or not tmp.stat().st_size:
                return (rc, 'metal-tt', text.strip().splitlines()[-1][:160] if text.strip() else '无输出')
            if a.plat == 'ios':
                try: tmp.write_bytes(lzfse(tmp.read_bytes()))
                except (OSError, ValueError) as e: return ('-', 'lzfse', str(e)[:160])
            if cf:
                cf.parent.mkdir(parents=True, exist_ok=True); clone(tmp, cf)
            os.replace(tmp, dst); return 'built'

        def one(job):
            v, f, entry, fname = job
            vl, fl = fn_lib(v, K_BP, entry), fn_lib(f, K_B, fname)
            whole = sum(1 for x in (vl, fl) if x and x[3])
            if not vl or not fl:
                return ('-', '顶点' if not vl else '片元', f'库里没有函数 {entry if not vl else fname}（或读不出它的常量表）'), whole
            return build(path(job), vl, entry, fl, fname), whole

        warm_files = (a.out / f'{WARM}{ext}', a.out / f'{WARM}.metallib')
        if jobs:   # 首查预热对先出（本工具现编小库，与目标同 SDK、同最低系统版本，库与归档一起随包）
            wm = work / f'{WARM}.metal'; wm.write_text(WARM_SRC); wair, wlib = work / f'{WARM}.air', work / f'{WARM}.metallib'
            p = subprocess.run(['xcrun', '-sdk', sdk, 'metal', f'-m{a.plat}-version-min={a.os}.0', '-c', str(wm), '-o', str(wair)], capture_output=True, text=True)
            if not p.returncode:
                p = subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(wair), '-o', str(wlib)], capture_output=True, text=True)
            if p.returncode:
                r = (p.returncode, 'metal', (p.stderr.strip().splitlines() or ['无输出'])[-1][:160])
            else:
                h = 'w' + hashlib.sha256(wlib.read_bytes()).hexdigest()
                r = build(warm_files[0], (wlib, h, False), 's2a_warm_v', (wlib, h, False), 's2a_warm_f')
            if isinstance(r, tuple):
                warm = '没编成'
                print(f'【黄】首查预热对没编成（{r[1]}，退出 {r[0]}）：{r[2]}；设备第一次查随包归档照旧付一次性初始化', file=sys.stderr)
            else:
                clone(wlib, warm_files[1]); warm = '1 份' + ('（缓存命中）' if r == 'hit' else '')
                used = sum(x.stat().st_size for x in warm_files)
        with ThreadPoolExecutor(max(1, a.j)) as ex:   # 全装：plan 出的对全部并行编
            for job, r in zip(jobs, ex.map(one, jobs)): res[job] = r
        for job in jobs:   # 没编成的不留同名旧文件
            if isinstance(res[job][0], tuple): path(job).unlink(missing_ok=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    stat = {'hit': 0, 'built': 0, 'whole': 0}
    fail = []
    for job in jobs:
        r, whole = res[job]; stat['whole'] += whole
        if isinstance(r, tuple): fail.append(job[:2] + r)
        else: stat[r] += 1
    ok = len(jobs) - len(fail)
    for v, f, rc, who, why in fail[:8]:
        print(f'【黄】拉取基础管线 {v} × {f} 没编成（{who}，退出 {rc}）：{why}', file=sys.stderr)
    how = '逐对 lzfse 压缩' if a.plat == 'ios' else '原样不压'
    pair_bytes = sum(path(job).stat().st_size for job in jobs if path(job).is_file())
    used += pair_bytes
    print(f'{head}，出 {ok} 对（缓存命中 {stat["hit"]}、现编 {stat["built"]}，'
          f'其中按整段库带函数 {stat["whole"]} 次）、失败 {len(fail)} 对；首查预热对 {warm}；'
          f'{a.plat} {a.os}.0 × {len(archs)} 个架构（{" ".join(archs)}）；{how}，随包共 {used} 字节（含预热对），每对 {pair_bytes // ok if ok else 0} 字节')
    return 3 if fail or warm == '没编成' else 0


if __name__ == '__main__':
    sys.exit(main())
