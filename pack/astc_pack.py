#!/usr/bin/env python3
"""BC → ASTC 预转包（出包时、目标是 Apple TV 等不支持 BC 压缩贴图的设备才调）。机制与包形态参考 brad-n/TVPort（MIT）dm/gfx/tools/bntx_astc.py：
在电脑上把游戏资源里的 BC 贴图逐平面（级 × 层）解成线性、用 Arm astc-encoder 编成 ASTC 4x4，写成一个按键排序、末尾带索引的只读包；
运行时渲染器（graphics/metal/gal_tex.m「BC → ASTC」）上传 BC 数据时按同一口径算键查包，命中直接传 ASTC，不再在 CPU 上把 BC 整张解成未压缩。

用法：
  python3 pack/astc_pack.py build <romfs 目录或单个文件> <输出包> [--quality fastest|fast|medium|thorough|<0–100>] [--jobs N] [--tool-dir 目录]
  python3 pack/astc_pack.py info <包>
  python3 pack/astc_pack.py bc7-build <清单.json> <bc7-astc.pack> [--tool-dir 目录]
  python3 pack/astc_pack.py bc7-scan <RomFS 或容器> <bc7-astc.pack> --report <读数.tsv> [--tool-dir 目录]
  python3 pack/astc_pack.py bc7-compare <会话日志> <读数.tsv>
BC7 清单版本 1 的 images 项为 {width,height,levels,layers,path}。路径指向紧排行的原 BC7 块，级在外、层在内。
整张图通过 UNORM／sRGB 双路检查时才写入。Android 的包版本、键和逐项负载摘要由 bc7_astc_pack.inc 校验。
Android 把 bc7-astc.pack 作为资源索引的一项放在已激活的 revision 根，仅在 debug.s2a.nvk_bc7_astc_pack 打开时读取。
bc7-scan 只收结构与布局对得上的 BNTX（NX）与 XTX（DFvN 1.1），输出整图键与画质结论。
随包部署 <bc7-astc.pack>.rejects 时，Android 诊断可以把同键离线画质拒收与未知键分开统计。
debug.s2a.nvk_bc7_workset_diag 缺省关闭，打开时输出每个唯一整图键与累计读数，字节口径是去排后的原 BC7 块。
Metal 旧包运行时：NXGAL_ASTC_PACK=<包路径>（宿主设；没设 = 行为不变）。

口径（与 graphics/metal/gal_astc.h 一字不差，改一边必须改另一边）：
  平面 = 一张贴图第 level 级第 layer 层的 BC 块，按行紧排（行 = ceil(宽/4) 块，块 8 / 16 字节）——就是 Ryujinx Gpu 层把显存里的块线性数据
         解成线性后交给渲染器的那一段（LayoutConverter.ConvertBlockLinearToLinear；级在外、层在内）。
  键   = SHA-256( b"s2a-bc-astc-v1" ‖ u32le 族 ‖ u32le 宽 ‖ u32le 高 ‖ 平面 BC 字节 )，族：BC1=1 BC2=2 BC3=3 无符号 BC5=5 BC7=7
         （sRGB 与否不进键；BC4、带符号 BC5、BC6H 不进包，运行时照旧解压——BC4 解压后的 R8 与 ASTC 4x4 一样每 texel 1 字节，进包不省内存）。
  包   = 头 64 字节 | 各平面 ASTC 4x4 块（16 字节对齐，按键序）| 索引 项数 × {键 32, u64 偏移, u64 字节}（按键升序），格式见 gal_astc.h。
贴图来源：romfs 里的 BNTX（单独的 .bntx，或嵌在 BFRES 等文件里），外面可以套 zstd（含 Pack/ZsDic.pack.zs 字典）/ Yaz0 / SARC，可嵌套。
块线性 → 线性、BC 解压、ASTC 编码在 C 库 graphics/tools/astc/bc2astc.c（本脚本按源码哈希编出、缓存在 build/astc_tool/）。
"""
import os as _pw_os, sys as _pw_sys; _pw_sys.path.append(_pw_os.path.join(_pw_os.path.dirname(_pw_os.path.abspath(__file__)), '..', 'tools')); from pool_workers import cores  # noqa: E402  核数 / 进程池路数唯一来源（tools/pool_workers.py，#1332）
import argparse
import concurrent.futures as cf
import ctypes
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
ASTC_DIR = os.path.join(ROOT, 'graphics', 'third_party', 'astc-encoder')
LIB_C = [os.path.join(ROOT, 'graphics', 'tools', 'astc', 'bc2astc.c'), os.path.join(ROOT, 'graphics', 'tools', 'yaz0dec.c')]
LIB_H = [os.path.join(ROOT, 'graphics', 'nvn', 'third_party', 'bcdec', 'bcdec.h')]
ASTC_FLAGS = ['-O3', '-DNDEBUG', '-std=c++14', '-ffp-contract=off', '-fPIC', '-DASTCENC_BLOCK_MAX_TEXELS=16']   # 只编 4x4
C_FLAGS = ['-O2', '-DNDEBUG', '-fPIC', '-std=c11']

KEY_TAG = b's2a-bc-astc-v1'
HEAD, ENT = 64, 48
MAGIC = b'S2ABCAST'
BC7_MAGIC = b'S2ABCAQ2'
BC7_KEY_TAG = b's2a-bc7-astc-image-v1'
BC7_ENT = ENT + 32
BC7_IMAGE_MAX = 64 << 20
BC7_INDEX_MAX = 1 << 18
QUALITY = {'fastest': 0.0, 'fast': 10.0, 'medium': 60.0, 'thorough': 98.0}

# BNTX 格式（类型 << 8 | 数据类型）→ 族；只收包能存且省内存的（BC1/2/3/7、无符号 BC5；BC4、BC6H 与带符号的不收，同 gal_astc_family）
BNTX_FAMILY = {0x1a01: 1, 0x1a06: 1, 0x1b01: 2, 0x1b06: 2, 0x1c01: 3, 0x1c06: 3, 0x1e01: 5, 0x2001: 7, 0x2006: 7}
FAMILY_BPP = {1: 8, 2: 16, 3: 16, 5: 16, 7: 16}
FAMILY_NAME = {1: 'BC1', 2: 'BC2', 3: 'BC3', 5: 'BC5', 7: 'BC7'}
# BNTX 的贴图维度（nn::gfx ImageDimension）：2D 1、立方体 3、2D 数组 5、立方体数组 8 收；1D / 3D / 多重采样不收
BNTX_DIMS = (1, 3, 5, 8)


def plane_key(family, w, h, bc):
    return hashlib.sha256(KEY_TAG + struct.pack('<III', family, w, h) + bc).digest()


def astc_bytes(w, h):
    return ((w + 3) // 4) * ((h + 3) // 4) * 16


# ---- C 库 -------------------------------------------------------------------------------------------------------------------------------------------------------------------
def _sources():
    # 「._」开头：exFAT 等盘上 macOS 配的 AppleDouble 附属文件（引擎副本可以在外置盘上），不是源码
    cpp = sorted(os.path.join(ASTC_DIR, f) for f in os.listdir(ASTC_DIR) if f.startswith('astcenc_') and f.endswith('.cpp'))
    hdr = sorted(os.path.join(ASTC_DIR, f) for f in os.listdir(ASTC_DIR) if f.endswith('.h') and not f.startswith('._'))
    return cpp, hdr


def tool_hash():
    cpp, hdr = _sources()
    h = hashlib.sha256(b'bc2astc 1\0' + ' '.join(ASTC_FLAGS + C_FLAGS).encode() + sys.platform.encode())
    for p in cpp + hdr + LIB_C + LIB_H:
        h.update(os.path.relpath(p, ROOT).encode() + b'\0')
        with open(p, 'rb') as f: h.update(f.read())
    return h.hexdigest()


def _run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: raise RuntimeError(f'{cmd[0]} 失败（{r.returncode}）：{" ".join(cmd)[:300]}\n{r.stderr[-2000:]}')


EXPORTS = ('b2a_version', 'b2a_level_offset', 'b2a_layer_size', 'b2a_deswizzle', 'b2a_ctx_new', 'b2a_ctx_free', 'b2a_encode', 'b2a_bc7_verify', 'yaz0_decompress')   # Lib 用到的全部函数


def lib_name():
    """C 库文件名（名字即源码与编译参数的身份）：现编缓存与 Windows 发版预编件同名"""
    ext = '.dll' if os.name == 'nt' else ('.dylib' if sys.platform == 'darwin' else '.so')
    return f'libbc2astc-{tool_hash()[:16]}{ext}'


def prebuilt():
    """启动器随发版预编的一份（S2A_PREBUILT_TOOLS/astc/<lib_name>，launchers/windows/bundle_prepare_win.py）：同名在就用，玩家机不要编译器"""
    tools = os.environ.get('S2A_PREBUILT_TOOLS')
    p = os.path.join(tools, 'astc', lib_name()) if tools else ''
    return p if p and os.path.isfile(p) else None


def build_tool(tool_dir=None):
    """编出 C 库（按源码与编译参数的哈希缓存），返回路径；有同名预编件直接用"""
    hit = prebuilt()
    if hit: return hit
    d = tool_dir or os.path.join(ROOT, 'build', 'astc_tool')
    lib = os.path.join(d, lib_name())
    if os.path.exists(lib): return lib
    tag = tool_hash()[:16]
    work = os.path.join(d, f'obj-{tag}'); os.makedirs(work, exist_ok=True)
    cxx, cc = os.environ.get('CXX', 'clang++'), os.environ.get('CC', 'clang')
    cpp, _ = _sources()
    # Windows（clang 出 MSVC 目标）不认 -fPIC，且共享库默认不导出函数：编时去掉 -fPIC、链接时按名导出（同 graphics/tools/nxfmt.py 的 Yaz0 解码库）
    strip = (lambda f: [x for x in f if x != '-fPIC']) if os.name == 'nt' else (lambda f: f)
    jobs = []
    for src in cpp: jobs.append([cxx] + strip(ASTC_FLAGS) + ['-c', src, '-o', os.path.join(work, os.path.basename(src)[:-4] + '.o')])
    for src in LIB_C: jobs.append([cc] + strip(C_FLAGS) + ['-c', src, '-o', os.path.join(work, os.path.basename(src)[:-2] + '.o')])
    with cf.ThreadPoolExecutor(cores()) as ex:
        for f in [ex.submit(_run, j) for j in jobs]: f.result()
    objs = [j[-1] for j in jobs]
    part = f'{lib}.{os.getpid()}.part'
    shared = ['-dynamiclib'] if sys.platform == 'darwin' else ['-shared'] + ([f'-Wl,/EXPORT:{n}' for n in EXPORTS] if os.name == 'nt' else [])
    _run([cxx] + shared + ['-o', part] + objs)
    os.replace(part, lib)
    return lib


class Lib:
    def __init__(self, path):
        L = self.L = ctypes.CDLL(path)
        u32, u64, vp = ctypes.c_uint32, ctypes.c_uint64, ctypes.c_void_p
        L.b2a_version.restype = ctypes.c_int
        if L.b2a_version() != 1: raise RuntimeError('bc2astc 库版本不对')
        L.b2a_level_offset.argtypes = [u32, u32, u32, u32, u32]; L.b2a_level_offset.restype = u64
        L.b2a_layer_size.argtypes = [u32, u32, u32, u32, u32, u32]; L.b2a_layer_size.restype = u64
        L.b2a_deswizzle.argtypes = [vp, u64, u32, u32, u32, u32, u64, u32, u32, vp, u64]; L.b2a_deswizzle.restype = ctypes.c_int
        L.b2a_ctx_new.argtypes = [ctypes.c_float]; L.b2a_ctx_new.restype = vp
        L.b2a_ctx_free.argtypes = [vp]
        L.b2a_encode.argtypes = [vp, u32, u32, u32, vp, u64, vp, u64]; L.b2a_encode.restype = ctypes.c_int
        L.b2a_bc7_verify.argtypes = [vp, u32, u32, vp, u64, vp, u64]; L.b2a_bc7_verify.restype = ctypes.c_int
        L.yaz0_decompress.argtypes = [vp, ctypes.c_int64, vp, ctypes.c_int64]; L.yaz0_decompress.restype = ctypes.c_int64

    def deswizzle(self, blob, bpp, w, h, gob_log2, layer_stride, layer, level):
        """blob：一张贴图的块线性数据（ctypes 数组）→ 第 layer 层第 level 级的线性 BC 平面（bytes），越界返回 None"""
        lw, lh = max(1, w >> level), max(1, h >> level)
        out = ctypes.create_string_buffer(((lw + 3) // 4) * ((lh + 3) // 4) * bpp)
        r = self.L.b2a_deswizzle(ctypes.addressof(blob), len(blob), bpp, w, h, gob_log2, layer_stride, layer, level, out, len(out))
        return None if r else out.raw

    def yaz0(self, d):
        n = int.from_bytes(d[4:8], 'big')
        out = ctypes.create_string_buffer(n)
        if self.L.yaz0_decompress(d, len(d), out, n) != n: raise ValueError('Yaz0 数据损坏')
        return out.raw


_TLS = threading.local()


def _ctx(lib, quality):
    c = getattr(_TLS, 'ctx', None)
    if c is None:
        c = _TLS.ctx = lib.L.b2a_ctx_new(quality)
        if not c: raise RuntimeError('astc-encoder 上下文建不起来')
    return c


def encode(lib, quality, family, w, h, bc):
    out = ctypes.create_string_buffer(astc_bytes(w, h))
    if lib.L.b2a_encode(_ctx(lib, quality), family, w, h, bc, len(bc), out, len(out)): raise RuntimeError(f'ASTC 编码失败：族 {family} {w}x{h}')
    return out.raw


# ---- 容器 ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
def _zstd():
    try:
        from compression import zstd   # Python 3.14
        return zstd
    except ImportError:
        return None


def unzstd(d, dicts):
    fhd = d[4] if len(d) > 5 else 0
    n, o = (0, 1, 2, 4)[fhd & 3], 6 - (fhd >> 5 & 1)
    did = int.from_bytes(d[o:o + n], 'little')
    want = dicts.get(did) if did else None
    z = _zstd()
    if z is not None:
        return z.decompress(d, zstd_dict=z.ZstdDict(want)) if want is not None else z.decompress(d)
    # 没有 Python 3.14 的 compression.zstd（Windows 随包 Python 3.12）：退到 zstd 命令行，带字典的帧要把字典写成文件交 -D，不然整帧解不开
    if want is None:
        return subprocess.run(['zstd', '-d', '-c', '-q'], input=d, capture_output=True, check=True).stdout
    with tempfile.NamedTemporaryFile(delete=False, suffix='.zdict') as f:
        f.write(want)
    try:
        return subprocess.run(['zstd', '-d', '-c', '-q', '-D', f.name], input=d, capture_output=True, check=True).stdout
    finally:
        os.unlink(f.name)


def sarc_members(d):
    hl = struct.unpack_from('<H', d, 4)[0]
    doff = struct.unpack_from('<I', d, 0xC)[0]
    n = struct.unpack_from('<H', d, hl + 6)[0]
    names = hl + 12 + 16 * n + 8
    if not names <= doff <= len(d): raise ValueError('SARC 损坏')
    for i in range(n):
        _h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        if not s <= e <= len(d) - doff: raise ValueError('SARC 成员越界')
        if attr >> 24:
            o = names + (attr & 0xffffff) * 4; z = d.find(b'\0', o)
            nm = d[o:z if z >= 0 else len(d)].decode('utf-8', 'replace')
        else: nm = f'#{i}'
        yield nm, d[doff + s:doff + e]


ZSTD_MAGIC = b'\x28\xb5\x2f\xfd'
KNOWN = (ZSTD_MAGIC, b'Yaz0', b'SARC', b'BNTX', b'FRES')
HINTS = ('.bntx', '.bfres', '.bkres', '.zs', '.szs', '.sarc', '.pack', '.arc', '.bars')
MAX_DEPTH = 8


def bntx_textures(d, p):
    """BNTX（从 d[p] 起）里的贴图：dict 列表；解析不了的是 {'bad': 原因}"""
    size = struct.unpack_from('<I', d, p + 0x1c)[0]; end = p + size
    count, info = struct.unpack_from('<IQ', d, p + 0x24)
    if count > BC7_INDEX_MAX or info < 0x40 or info + count * 8 > size:
        return [{'bad': 'BNTX 贴图指针表越界'}]
    out = []
    for i in range(count):
        try:
            t = p + struct.unpack_from('<Q', d, p + info + 8 * i)[0]
            if t < p + 0x40 or d[t:t + 4] != b'BRTI' or t + 0x78 > end: raise ValueError('没有 BRTI')
            flags, storage_dim, tile, swz, mips, samples, fmt, _acc, w, h, depth, arr, layout0, layout1 = struct.unpack_from('<BBHHH9I', d, t + 0x10)
            image_size, align, _chan, dimension = struct.unpack_from('<IIII', d, t + 0x50)
            mip_ptr = struct.unpack_from('<Q', d, t + 0x70)[0]
            if not 0 < mips <= 32 or mip_ptr < 0x40 or mip_ptr + 8 * mips > size:
                raise ValueError('BNTX 级指针表越界')
            offs = struct.unpack_from('<%dQ' % mips, d, p + mip_ptr) if 0 < mips <= 32 else ()
            out.append({'platform': d[p + 0x20:p + 0x24], 'fmt': fmt, 'w': w, 'h': h, 'depth': depth, 'layers': max(arr, 1), 'mips': mips, 'tile': tile, 'samples': samples, 'dim': dimension,
                        'gob': layout0 & 7, 'layout0': layout0, 'layout1': layout1, 'flags': flags, 'swz': swz,
                        'storage_dim': storage_dim, 'array_count': arr, 'metadata_end': max(t + 0x78, p + mip_ptr + mips * 8, p + info + count * 8),
                        'size': image_size, 'align': align, 'offs': offs, 'start': p + offs[0] if offs else 0, 'end': end})
        except (struct.error, ValueError) as e:
            out.append({'bad': str(e)})
    return out


def bntx_ok(d, p):
    if d[p:p + 8] != b'BNTX\0\0\0\0' or len(d) - p < 0x40: return False
    bom = struct.unpack_from('<H', d, p + 0xc)[0]; size = struct.unpack_from('<I', d, p + 0x1c)[0]
    return bom == 0xfeff and d[p + 0x20:p + 0x24] in (b'NX  ', b'Ounc') and 0x40 <= size <= len(d) - p


class Scan:
    """扫资源、出平面任务、编码、收结果（多线程：编码与哈希都在 C / hashlib 里放开 GIL）"""
    def __init__(self, lib, quality, jobs, spill):
        self.lib, self.quality, self.spill = lib, quality, spill
        self.c = {}; self.formats = {}; self.errors = []
        self.mu = threading.Lock(); self.keys = {}   # 键 → (在 spill 里的偏移, 字节)；值为 None = 正在编
        self.pool = cf.ThreadPoolExecutor(jobs); self.slots = threading.BoundedSemaphore(jobs * 4); self.futs = []
        self.dicts = {}

    def count(self, k, n=1):
        with self.mu: self.c[k] = self.c.get(k, 0) + n

    # -- 一张贴图：校验布局 → 各平面解成线性、算键、没见过的编码 --
    def texture(self, d, t, name):
        self.count('textures')
        if 'bad' in t: self.count('unparsed'); return
        fam = BNTX_FAMILY.get(t['fmt'])
        if not fam: return
        with self.mu: self.formats[FAMILY_NAME[fam]] = self.formats.get(FAMILY_NAME[fam], 0) + 1
        if t['platform'] != b'NX  ': self.count('skip_switch2'); return   # Switch 2 的 BNTX：GOB 内 16 字节片的次序不同（TVPort gob_switch2），本工具与 Ryujinx 都按 Switch 1 解，先不收
        if t['dim'] not in BNTX_DIMS or t['samples'] > 1 or t['depth'] > 1 or t['tile'] != 0: self.count('skip_shape'); return
        if not (0 < t['w'] <= 16384 and 0 < t['h'] <= 16384 and 0 < t['mips'] <= 16 and t['offs']): self.count('unparsed'); return
        bpp, L = FAMILY_BPP[fam], self.lib.L
        # 布局核对：各级起点与 BNTX 记的一致、整张大小（按对齐）与 BNTX 记的一致——GOB 高度判错时这里对不上，不出错键
        rel = [o - t['offs'][0] for o in t['offs']]
        want = [L.b2a_level_offset(bpp, t['w'], t['h'], t['gob'], i) for i in range(t['mips'])]
        stride = L.b2a_layer_size(bpp, t['w'], t['h'], t['mips'], t['layers'], t['gob'])
        total = stride * t['layers']; al = max(t['align'], 1)
        if rel != want or (total + al - 1) // al * al != t['size']: self.count('layout_differs'); self.errors.append(f'{name}: 布局与 BNTX 记的对不上（级起点 {rel[:4]} 对 {want[:4]}，大小 {t["size"]} 对 {total}）'); return
        if t['start'] + total > t['end'] or t['start'] + total > len(d): self.count('short'); return
        self.count('converted')
        blob = (ctypes.c_char * total).from_buffer_copy(d, t['start'])
        self.slots.acquire()
        self.futs.append(self.pool.submit(self._planes, blob, fam, bpp, t, stride))

    def _planes(self, blob, fam, bpp, t, stride):
        try:
            for layer in range(t['layers']):
                for level in range(t['mips']):
                    w, h = max(1, t['w'] >> level), max(1, t['h'] >> level)
                    bc = self.lib.deswizzle(blob, bpp, t['w'], t['h'], t['gob'], stride, layer, level)
                    if bc is None: self.count('plane_errors'); continue
                    k = plane_key(fam, w, h, bc)
                    with self.mu:
                        self.c['planes'] = self.c.get('planes', 0) + 1
                        if k in self.keys: self.c['planes_repeat'] = self.c.get('planes_repeat', 0) + 1; continue
                        self.keys[k] = None
                    a = encode(self.lib, self.quality, fam, w, h, bc)
                    with self.mu:
                        off = self.spill.tell(); self.spill.write(a); self.keys[k] = (off, len(a))
                        self.c['bc_bytes'] = self.c.get('bc_bytes', 0) + len(bc); self.c['astc_bytes'] = self.c.get('astc_bytes', 0) + len(a)
        finally:
            self.slots.release()

    # -- 一段字节：按魔数拆容器 --
    def blob(self, d, name, depth=0):
        if depth > MAX_DEPTH: self.count('too_deep'); return
        m = d[:4]
        try:
            if m == ZSTD_MAGIC:
                self.count('zstd'); return self.blob(unzstd(d, self.dicts), name, depth + 1)
            if m == b'Yaz0':
                self.count('yaz0'); return self.blob(self.lib.yaz0(d), name, depth + 1)
            if m == b'SARC':
                self.count('sarc')
                for nm, member in sarc_members(d): self.blob(member, name + '/' + nm, depth + 1)
                return
            found = False
            for mt in re.finditer(b'BNTX\0\0\0\0', d):
                if bntx_ok(d, mt.start()):
                    found = True; self.count('bntx')
                    for t in bntx_textures(d, mt.start()): self.texture(d, t, name)
            if not found: self.count('other_files')
        except Exception as e:   # 坏文件：跳过并计数，不中断整包
            self.count('container_errors'); self.errors.append(f'{name}: {type(e).__name__}: {e}')

    def finish(self):
        for f in self.futs: f.result()
        self.pool.shutdown()


def load_dicts(root):
    p = os.path.join(root, 'Pack', 'ZsDic.pack.zs')
    if not os.path.isfile(p):
        return {}
    with open(p, 'rb') as f:
        d = unzstd(f.read(), {})
    return {struct.unpack_from('<I', m, 4)[0]: m for _, m in sarc_members(d)
            if len(m) >= 8 and m[:4] == bytes.fromhex('37a430ec')}


def files_of(path):
    if os.path.isfile(path): return [path]
    out = []
    for r, ds, fs in os.walk(path):
        ds.sort()
        out += [os.path.join(r, f) for f in sorted(fs)]
    return out


def write_pack(out, spill_path, keys, fingerprint, bc7=False):
    """键序写包（同样的输入出同样的字节）：头 | 负载（16 字节对齐）| 索引"""
    order = sorted(keys)
    part = out + '.part'
    ent = BC7_ENT if bc7 else ENT
    with open(spill_path, 'rb') as sp, open(part, 'wb') as f:
        f.write(b'\0' * HEAD); pos = HEAD; index = []
        for k in order:
            off, n, *digest = keys[k]
            sp.seek(off); a = sp.read(n)
            if len(a) != n: raise RuntimeError('暂存文件读不全')
            index.append(k + struct.pack('<QQ', pos, n) + b''.join(digest)); f.write(a); pad = (-n) & 15; f.write(b'\0' * pad); pos += n + pad
        ioff = pos
        f.write(b''.join(index))
        total = ioff + ent * len(index)
        f.seek(0)
        f.write((BC7_MAGIC if bc7 else MAGIC) +
                struct.pack('<IIQQQBBHI', 2 if bc7 else 1, ent, len(index), ioff, total, 4, 4, 0, 0) +
                fingerprint[:16])
    os.replace(part, out)
    return total


def read_index(path):
    """包 → (头字段, [(键, 偏移, 字节)])；格式不对抛 ValueError（与运行时 gal_astc_open 同一组判据）"""
    with open(path, 'rb') as f: b = f.read()
    if len(b) < HEAD or b[:8] != MAGIC: raise ValueError('魔数不对')
    ver, ent, count, ioff, total, bw, bh = struct.unpack_from('<IIQQQBB', b, 8)
    if ver != 1 or ent != ENT or total != len(b) or (bw, bh) != (4, 4) or ioff < HEAD or ioff + ENT * count != total: raise ValueError('头不对')
    rows = [(b[ioff + ENT * i:ioff + ENT * i + 32],) + struct.unpack_from('<QQ', b, ioff + ENT * i + 32) for i in range(count)]
    for i, (k, off, n) in enumerate(rows):
        if (i and rows[i - 1][0] >= k) or off < HEAD or off & 15 or not n or n & 15 or off + n > ioff: raise ValueError(f'第 {i} 项不对')
    return {'count': count, 'index_offset': ioff, 'size': total, 'fingerprint': b[48:64].hex()}, rows


def build(src, out, quality=QUALITY['fast'], jobs=None, tool_dir=None, say=print):
    t0 = time.time()
    lib = Lib(build_tool(tool_dir))
    jobs = jobs or cores()
    os.makedirs(os.path.dirname(os.path.abspath(out)) or '.', exist_ok=True)
    fd, spill_path = tempfile.mkstemp(prefix='.astc-spill-', dir=os.path.dirname(os.path.abspath(out)))
    try:
        with os.fdopen(fd, 'w+b') as spill:
            s = Scan(lib, quality, jobs, spill)
            if os.path.isdir(src): s.dicts = load_dicts(src)
            for p in files_of(src):
                with open(p, 'rb') as f: head = f.read(4)
                if head not in KNOWN and not p.lower().endswith(HINTS): s.count('skipped_files'); continue
                with open(p, 'rb') as f: d = f.read()
                s.blob(d, os.path.relpath(p, src) if os.path.isdir(src) else os.path.basename(p))
            s.finish(); spill.flush()
            fp = hashlib.sha256(tool_hash().encode() + struct.pack('<f', quality)).digest()
            size = write_pack(out, spill_path, {k: v for k, v in s.keys.items() if v}, fp)
    finally:
        try: os.unlink(spill_path)
        except FileNotFoundError: pass
    rep = dict(sorted(s.c.items())); rep['formats'] = s.formats; rep['entries'] = sum(1 for v in s.keys.values() if v); rep['pack_bytes'] = size
    rep['seconds'] = round(time.time() - t0, 1); rep['errors'] = s.errors[:20]
    say(json.dumps(rep, ensure_ascii=False))
    return rep



def bc7_dims(w, h, levels, layers):
    if (any(type(n) is not int for n in (w, h, levels, layers)) or
            not (1 <= w <= 16384 and 1 <= h <= 16384 and
                 1 <= levels <= max(w, h).bit_length() and 1 <= layers <= 2048)):
        raise ValueError('BC7 整图尺寸／级数／层数越界')
    return [(max(1, w >> level), max(1, h >> level)) for level in range(levels)]


def bc7_image_key(w, h, levels, layers, source):
    # 两个 Vulkan 视图均按同一块负载验过。按级在外、层在内连接紧排平面。
    key = hashlib.sha256(BC7_KEY_TAG + struct.pack('<IIII', w, h, levels, layers))
    key.update(source)
    return key.digest()


def encode_bc7_image(lib, contexts, w, h, levels, layers, source):
    dims = bc7_dims(w, h, levels, layers)
    total = sum(astc_bytes(pw, ph) * layers for pw, ph in dims)
    if total > BC7_IMAGE_MAX or len(source) != total:
        raise ValueError('BC7 整图源字节数与布局不符')
    if any(mode == 0 for mode in source[::16]):
        return None
    result = bytearray()
    for pw, ph in dims:
        size = astc_bytes(pw, ph)
        for _ in range(layers):
            start = len(result)
            plane = source[start:start + size]
            encoded = ctypes.create_string_buffer(size)
            for ctx in contexts:
                if lib.L.b2a_encode(ctx, 7, pw, ph, plane, size, encoded, size):
                    raise RuntimeError('BC7 预转编码失败')
                rc = lib.L.b2a_bc7_verify(ctx, pw, ph, plane, size, encoded, size)
                if rc < 0:
                    raise RuntimeError('BC7 预转质量检查失败')
                if rc == 0:
                    result.extend(encoded.raw)
                    break
            else:
                return None
    return result


def _count(stats, key):
    stats[key] = stats.get(key, 0) + 1


def _xtx_textures(data):
    # Switch Toolbox XTX.cs：DFvN 1.1 的 2/3 段，120 字节描述，
    # 17 个相对 mip 偏移后是布局字。数据块的偏移相对本段头。
    if len(data) < 16 or struct.unpack_from('<4sIII', data) != (b'DFvN', 16, 1, 1):
        raise ValueError('XTX 头或版本未证')
    pos, pending, textures = 16, None, []
    while pos < len(data):
        magic, head, size, offset, kind, _, _ = struct.unpack_from('<4sIQQIII', data, pos)
        begin, end = pos + offset, pos + offset + size
        if magic != b'HBvN' or not 36 <= head <= offset or end > len(data):
            raise ValueError('XTX 段头或负载越界')
        if kind == 2 and pending is None and size == 120:
            pending = begin
        elif kind == 3 and pending is not None and size:
            total, align, w, h, depth, target, fmt, levels, stride = struct.unpack_from('<Q8I', data, pending)
            offsets = struct.unpack_from('<17I', data, pending + 40)
            layout0, layout1, flag = struct.unpack_from('<III', data, pending + 108)
            layers = total // stride if stride and not total % stride else 0
            shape = {1: layers == 1, 4: layers >= 1, 8: layers == 6, 9: layers >= 6 and not layers % 6}
            if fmt in (0x4d, 0x4e):
                if not (depth == 1 and shape.get(target) and (target not in (8, 9) or w == h) and
                        1 <= levels <= 17 and not any(offsets[levels:]) and size == total and flag == 0):
                    textures.append({'bad': 'XTX 目标、数组跨度或布局未证'})
                else:
                    textures.append(dict(w=w, h=h, mips=levels, layers=layers, size=total,
                                         stride=stride, align=align, gob=layout0 & 7, layout0=layout0,
                                         layout1=layout1, offs=offsets[:levels], start=begin, end=end,
                                         metadata_end=pos+head, base=0))
            pending = None
        elif kind != 5 or pending is not None or end != len(data):
            raise ValueError('XTX 纹理段与数据段无法一一配对')
        if len(textures) > BC7_INDEX_MAX:
            raise ValueError('XTX 贴图项数超限')
        pos = end
    if pending is not None:
        raise ValueError('XTX 缺少图像数据段')
    return textures


def _bc7_canonical(lib, data, t):
    w, h, levels, layers = (t[k] for k in ('w', 'h', 'mips', 'layers'))
    dims = bc7_dims(w, h, levels, layers)
    expected = sum(astc_bytes(pw, ph) * layers for pw, ph in dims)
    if expected > BC7_IMAGE_MAX or t['layout0'] > 5 or t['layout1'] not in (0, 7, 0x010007):
        raise ValueError('BC7 布局字或整图大小未证')
    align, start, size, gob = (t[k] for k in ('align', 'start', 'size', 'gob'))
    L = lib.L
    offsets = tuple(L.b2a_level_offset(16, w, h, gob, l) for l in range(levels))
    stride = L.b2a_layer_size(16, w, h, levels, layers, gob)
    raw = stride * layers
    if (not 1 <= align <= 65536 or align & (align - 1) or
            (start - t['base']) % align or start < t['metadata_end'] or start + size > t['end'] or
            t['offs'] != offsets or t.get('stride', stride) != stride or
            (raw + align - 1) // align * align != size):
        raise ValueError('BC7 级起点、层跨度或负载范围与块线性布局不符')
    blob = (ctypes.c_char * raw).from_buffer_copy(data, start)
    result = bytearray()
    for l in range(levels):
        for y in range(layers):
            plane = lib.deswizzle(blob, 16, w, h, gob, stride, y, l)
            if plane is None or len(plane) != astc_bytes(*dims[l]):
                raise ValueError('BC7 平面解排失败')
            result.extend(plane)
    return w, h, levels, layers, bytes(result)


def bc7_container_images(lib, data, stats, dicts=None, depth=0):
    """只枚举边界、块尺寸、级偏移与层跨度可对证的 BC7 整图。"""
    if depth > MAX_DEPTH:
        _count(stats, 'container_reject')
        return
    magic = data[:4]
    try:
        if magic == ZSTD_MAGIC:
            yield from bc7_container_images(lib, unzstd(data, dicts or {}), stats, dicts, depth+1)
            return
        if magic == b'Yaz0':
            yield from bc7_container_images(lib, lib.yaz0(data), stats, dicts, depth+1)
            return
        if magic == b'SARC':
            for _, child in sarc_members(data):
                yield from bc7_container_images(lib, child, stats, dicts, depth+1)
            return
        textures = []
        if magic == b'DFvN':
            textures = _xtx_textures(data)
        else:
            for match in re.finditer(b'BNTX\0\0\0\0', data):
                base = match.start()
                if not bntx_ok(data, base):
                    _count(stats, 'container_reject')
                    continue
                for t in bntx_textures(data, base):
                    if 'bad' in t:
                        _count(stats, 'container_reject')
                    elif t['fmt'] in (0x2001, 0x2006):
                        shape = {1: t['layers'] == 1, 3: t['layers'] == 6,
                                 5: t['layers'] >= 1, 8: t['layers'] >= 6 and not t['layers'] % 6}
                        if (t['platform'] != b'NX  ' or t['tile'] or t['samples'] != 1 or t['depth'] != 1 or
                                t['storage_dim'] != 2 or t['flags'] & ~1 or t['swz'] or not t['array_count'] or
                                not shape.get(t['dim']) or t['dim'] in (3, 8) and t['w'] != t['h']):
                            _count(stats, 'layout_reject')
                            continue
                        t.update(base=base, offs=tuple(x-t['offs'][0] for x in t['offs']))
                        textures.append(t)
        for t in textures:
            try:
                if 'bad' in t:
                    raise ValueError(t['bad'])
                yield _bc7_canonical(lib, data, t)
            except ValueError:
                _count(stats, 'layout_reject')
    except (ValueError, struct.error, OSError, subprocess.SubprocessError):
        _count(stats, 'container_reject')


def _bc7_manifest_images(manifest):
    inputs = json.loads(manifest.read_text(encoding='utf-8'))
    if inputs.get('version') != 1 or not isinstance(inputs.get('images'), list):
        raise ValueError('BC7 输入清单版本错误')
    if len(inputs['images']) > BC7_INDEX_MAX:
        raise ValueError('BC7 输入项数超限')
    for image in inputs['images']:
        w, h, levels, layers = (image[k] for k in ('width', 'height', 'levels', 'layers'))
        expected = sum(astc_bytes(pw, ph) * layers for pw, ph in bc7_dims(w, h, levels, layers))
        if expected > BC7_IMAGE_MAX:
            yield w, h, levels, layers, None
            continue
        with (manifest.parent / image['path']).open('rb') as src:
            source = src.read(expected + 1)
        if len(source) != expected:
            raise ValueError('BC7 输入缺少完整级／层')
        yield w, h, levels, layers, source


def _build_bc7_images(lib, images, out, report=None):
    from pathlib import Path

    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    contexts, keys, rejected_keys, rows = [], {}, set(), []
    rejected = 0
    try:
        for quality in (99.0, 100.0):
            ctx = lib.L.b2a_ctx_new(quality)
            if not ctx:
                raise RuntimeError('ASTC 编码上下文建不起来')
            contexts.append(ctx)
        with tempfile.TemporaryDirectory(prefix='.bc7-astc-', dir=out.parent) as tmp:
            spill, packed = Path(tmp) / 'payload', Path(tmp) / 'pack'
            with spill.open('wb') as f:
                for w, h, levels, layers, source in images:
                    if source is None:
                        rejected += 1
                        continue
                    key = bc7_image_key(w, h, levels, layers, source)
                    if key in keys or key in rejected_keys:
                        continue
                    if len(keys) + len(rejected_keys) >= BC7_INDEX_MAX:
                        raise ValueError('BC7 唯一图项数超限')
                    checked = encode_bc7_image(lib, contexts, w, h, levels, layers, source)
                    if checked is None:
                        rejected += 1
                        rejected_keys.add(key)
                    else:
                        keys[key] = (f.tell(), len(checked), hashlib.sha256(checked).digest())
                        f.write(checked)
                    if report is not None:
                        rows.append((key.hex(), w, h, levels, layers, len(source),
                                     'accepted' if checked is not None else 'quality_reject'))
            fingerprint = hashlib.sha256(b'BC7 4x4 quality v1\0' + tool_hash().encode()).digest()
            size = write_pack(str(packed), spill, keys, fingerprint, bc7=True)
            if report is not None:
                # 与这次包的索引字节绑定，旧报告不能污染新包的拒收分因。
                with packed.open('rb') as f:
                    digest = hashlib.sha256(f.read(HEAD))
                    f.seek(size - len(keys) * BC7_ENT)
                    digest.update(f.read())
                index_hash = digest.digest()
                receipt = Path(tmp) / 'rejects'
                receipt.write_bytes(b'S2ABCQR1' + struct.pack('<II',len(rejected_keys),0) +
                                    index_hash + b''.join(sorted(rejected_keys)))
                os.replace(receipt, str(out) + '.rejects')
                report = Path(report)
                report.parent.mkdir(parents=True, exist_ok=True)
                with report.open('w', encoding='utf-8') as f:
                    f.write('key\twidth\theight\tlevels\tlayers\tbc_bytes\tdecision\n')
                    for row in sorted(rows):
                        f.write('\t'.join(map(str, row)) + '\n')
            os.replace(packed, out)
    finally:
        for ctx in contexts:
            lib.L.b2a_ctx_free(ctx)
    return {'entries': len(keys), 'rejected': rejected, 'pack_bytes': size}


def build_bc7(manifest, out, tool_dir=None):
    from pathlib import Path
    lib = Lib(build_tool(tool_dir))
    return _build_bc7_images(lib, _bc7_manifest_images(Path(manifest)), out)


def build_bc7_containers(src, out, report, tool_dir=None):
    lib = Lib(build_tool(tool_dir))
    stats = {}
    dicts = load_dicts(src) if os.path.isdir(src) else {}

    def images():
        for path in files_of(src):
            with open(path, 'rb') as f:
                head = f.read(4)
                if head not in KNOWN + (b'DFvN',) and not path.lower().endswith(HINTS + ('.xtx',)):
                    continue
                f.seek(0)
                yield from bc7_container_images(lib, f.read(), stats, dicts)
    return {**_build_bc7_images(lib, images(), out, report), **stats}


def bc7_compare(log, report):
    """按同一整图键、形状及 BC 字节数关联会话与离线报告。盘点不完整则失败。"""
    import csv
    indexed = {}
    with open(report, encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            key = row['key']
            shape = tuple(int(row[k]) for k in ('width','height','levels','layers','bc_bytes'))
            decision = row['decision']
            if key in indexed or not re.fullmatch('[0-9a-f]{64}', key) or decision not in ('accepted', 'quality_reject'):
                raise ValueError('BC7 离线清单项非法或重复')
            indexed[key] = shape, decision
    images, summary, started = {}, {}, False
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'bc7-workset' not in line:
                continue
            if 'session schema=1' in line:
                images, summary, started = {}, {}, True
            if not started:
                continue
            image = re.search(r'\bimage key=([0-9a-f]{64}) width=(\d+) height=(\d+) levels=(\d+) layers=(\d+) bc_bytes=(\d+)\b', line)
            if image:
                key = image[1]
                if key in images:
                    raise ValueError('BC7 会话重复唯一键')
                images[key] = tuple(map(int, image.groups()[1:]))
            if 'summary final=' in line:
                summary = {k: int(v) for k,v in re.findall(r'([a-z_]+)=(\d+)', line)}
    if (summary.get('unique') != len(images) or summary.get('identity_lost') != 0 or
            summary.get('unique_bytes') != sum(shape[-1] for shape in images.values())):
        raise ValueError('BC7 会话摘要缺失、溢出或日志未收齐')
    counts = {k: {'images': 0, 'bc_bytes': 0} for k in ('accepted', 'quality_reject', 'unknown')}
    mismatches = []
    for key, shape in images.items():
        if key in indexed:
            known_shape, decision = indexed[key]
            if known_shape != shape:
                raise ValueError(f'BC7 键的形状／字节对不上：{key}')
        else:
            decision = 'unknown'
            mismatches.append(key)
        counts[decision]['images'] += 1
        counts[decision]['bc_bytes'] += shape[-1]
    return {'session': summary, 'offline': counts, 'unknown_keys': sorted(mismatches)}


def main(argv):
    ap = argparse.ArgumentParser(description='BC → ASTC 预转包')
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build'); b.add_argument('src'); b.add_argument('out')
    b.add_argument('--quality', default='fast', help='fastest / fast / medium / thorough，或 0–100')
    b.add_argument('--jobs', type=int, default=0); b.add_argument('--tool-dir')
    q = sub.add_parser('bc7-build'); q.add_argument('manifest'); q.add_argument('out'); q.add_argument('--tool-dir')
    scan = sub.add_parser('bc7-scan'); scan.add_argument('src'); scan.add_argument('out'); scan.add_argument('--report', required=True)
    scan.add_argument('--tool-dir')
    compare = sub.add_parser('bc7-compare'); compare.add_argument('log'); compare.add_argument('report')
    i = sub.add_parser('info'); i.add_argument('pack')
    a = ap.parse_args(argv)
    if a.cmd == 'build':
        q = QUALITY[a.quality] if a.quality in QUALITY else float(a.quality)
        rep = build(a.src, a.out, q, a.jobs or None, a.tool_dir)
        return 0 if rep['entries'] or not rep.get('converted') else 1
    if a.cmd == 'bc7-build':
        print(json.dumps(build_bc7(a.manifest, a.out, a.tool_dir), ensure_ascii=False))
        return 0
    if a.cmd == 'bc7-scan':
        print(json.dumps(build_bc7_containers(a.src, a.out, a.report, a.tool_dir), ensure_ascii=False))
        return 0
    if a.cmd == 'bc7-compare':
        print(json.dumps(bc7_compare(a.log, a.report), ensure_ascii=False))
        return 0
    head, rows = read_index(a.pack)
    print(json.dumps(head, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
