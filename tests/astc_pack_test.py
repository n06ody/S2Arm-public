"""BC → ASTC 预转包出包工具（pack/astc_pack.py + graphics/tools/astc/bc2astc.c）单测。
合成 BC1 / BC3 / BC7（外加该忽略的 RGBA8、BC4、Switch 2 BNTX、布局对不上的贴图），按 Tegra 块线性规则自己摆进一个最小 BNTX，
套上 SARC / Yaz0 / zstd，跑 build → 用本文件自己的包解析器读回：键集合、每项 ASTC 字节数与块数、排序与对齐都按期望核。
期望值都不经被测实现：平面是本文件先生成的线性字节，键由 hashlib 按 gal_astc.h 的口径现算，块线性摆放与各级起点由本文件按 GOB 规则另写一份；
纯红 BC1 块编出的 ASTC 应是常量色「空区块」（规范 C.2.23：FC FD FF×6 + RGBA16），由此核 BC 解压 → 编码的通道次序。
"""
import ctypes
import hashlib
import json
import subprocess
import importlib.util
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('astc_pack', ROOT / 'pack/astc_pack.py')
ap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ap)
TOOL_DIR = os.environ.get('S2A_ASTC_TOOL_DIR')   # 缺省 build/astc_tool；门禁外手跑可指到外置盘

GOB_W, GOB_H = 64, 8


def key(fam, w, h, plane):
    return hashlib.sha256(b's2a-bc-astc-v1' + struct.pack('<III', fam, w, h) + plane).digest()


def gob_y_at(hb, gy):
    while gy > 1 and hb <= (gy // 2) * GOB_H: gy //= 2
    return gy


def level_bytes(bpp, w, h, gy0, level):
    """(本级字节, 本级 GOB 高)：宽按 GOB（64 字节）取整、高按「gy 个 GOB」取整"""
    wb, hb = -(-max(1, w >> level) // 4), -(-max(1, h >> level) // 4)
    gy = gob_y_at(hb, gy0)
    return -(-wb * bpp // GOB_W) * GOB_W * (-(-hb // (GOB_H * gy)) * GOB_H * gy), gy


def swizzle_plane(lin, bpp, wb, hb, gy, out, base):
    """线性 BC 块 → Tegra 块线性：GOB = 64 字节 × 8 行，GOB 内 512 字节按 (x/32, y/2, x/16, y%2, x%16) 交错；gy 个 GOB 竖着成一组，组在行内从左到右"""
    gobs_w = -(-wb * bpp // GOB_W)
    for y in range(hb):
        for x in range(wb):
            xb = x * bpp
            gob_x, gob_y = xb // GOB_W, y // GOB_H
            group = (gob_y // gy) * gobs_w + gob_x
            inner = ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)
            o = base + group * gy * 512 + (gob_y % gy) * 512 + inner
            out[o:o + bpp] = lin[(y * wb + x) * bpp:(y * wb + x + 1) * bpp]


def make_tex(fmt, fam, bpp, w, h, mips, layers, gob_log2, dim, gen, break_offsets=False):
    """一张贴图：各平面的线性字节（gen(level, layer, n) 生成）、块线性数据、各级起点"""
    gy0 = 1 << gob_log2
    sizes = [level_bytes(bpp, w, h, gy0, l) for l in range(mips)]
    offs, o = [], 0
    for sz, _ in sizes: offs.append(o); o += sz
    layer = o
    if layers > 1:
        g = gob_y_at(-(-h // 4), gy0) * 512
        layer = -(-o // g) * g
    data = bytearray(layer * layers)
    planes = {}
    for L in range(layers):
        for l in range(mips):
            lw, lh = max(1, w >> l), max(1, h >> l); wb, hb = -(-lw // 4), -(-lh // 4)
            lin = gen(l, L, wb * hb * bpp)
            planes[(l, L)] = (lw, lh, lin)
            swizzle_plane(lin, bpp, wb, hb, sizes[l][1], data, L * layer + offs[l])
    if break_offsets and mips > 1: offs[1] += 512
    return {'fmt': fmt, 'fam': fam, 'w': w, 'h': h, 'mips': mips, 'layers': layers, 'gob': gob_log2, 'dim': dim, 'data': bytes(data), 'offs': offs, 'planes': planes}


def bntx(textures, platform=b'NX  '):
    """最小 BNTX：头 0x40 | 贴图指针表 | 各 BRTI（0x90）| 各级起点表 | 数据（512 对齐）"""
    n = len(textures); p_table = 0x40; p_brti = p_table + 8 * n; p_mips = p_brti + 0x90 * n
    mip_tab = [p_mips + sum(8 * t['mips'] for t in textures[:i]) for i in range(n)]
    pos = -(-(p_mips + sum(8 * t['mips'] for t in textures)) // 512) * 512
    starts = []
    for t in textures: starts.append(pos); pos += -(-len(t['data']) // 512) * 512
    d = bytearray(pos)
    d[0:8] = b'BNTX\0\0\0\0'; struct.pack_into('<H', d, 0xc, 0xfeff); struct.pack_into('<I', d, 0x1c, pos); d[0x20:0x24] = platform
    struct.pack_into('<IQ', d, 0x24, n, p_table)
    for i, t in enumerate(textures):
        b = p_brti + 0x90 * i
        struct.pack_into('<Q', d, p_table + 8 * i, b)
        d[b:b + 4] = b'BRTI'
        struct.pack_into('<BBHHH9I', d, b + 0x10, 0, 2, 0, 0, t['mips'], 1, t['fmt'], 0, t['w'], t['h'], 1, t['layers'], t['gob'], 0)
        struct.pack_into('<IIII', d, b + 0x50, len(t['data']), 512, 0, t['dim'])
        struct.pack_into('<Q', d, b + 0x70, mip_tab[i])
        for l, o in enumerate(t['offs']): struct.pack_into('<Q', d, mip_tab[i] + 8 * l, starts[i] + o)
        d[starts[i]:starts[i] + len(t['data'])] = t['data']
    return bytes(d)


def sarc(members):
    names = b''.join(nm.encode() + b'\0' * (4 - len(nm) % 4) for nm, _ in members)
    hl, n = 0x14, len(members)
    sfat = b'SFAT' + struct.pack('<HHI', 0xc, n, 0x65)
    nofs, dofs, entries, blob = 0, 0, b'', b''
    for nm, data in members:
        entries += struct.pack('<IIII', 0, 0x01000000 | (nofs // 4), dofs, dofs + len(data))
        nofs += len(nm) + 4 - len(nm) % 4
        blob += data + b'\0' * ((-len(data)) % 16); dofs = len(blob)
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0)
    head_len = hl + len(sfat) + len(entries) + len(sfnt) + len(names)
    doff = -(-head_len // 16) * 16
    head = b'SARC' + struct.pack('<HHIIHH', hl, 0xfeff, doff + len(blob), doff, 0x100, 0)
    out = head + sfat + entries + sfnt + names
    return out + b'\0' * (doff - len(out)) + blob


def yaz0_literal(d):
    """全字面量的 Yaz0（每组标志 0xFF + 8 字节原样）"""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(d)) + b'\0' * 8)
    for i in range(0, len(d), 8): out += b'\xff' + d[i:i + 8]
    return bytes(out)


def parse_pack(path):
    b = Path(path).read_bytes()
    assert b[:8] == b'S2ABCAST', '魔数'
    ver, ent, count, ioff, total, bw, bh = struct.unpack_from('<IIQQQBB', b, 8)
    assert (ver, ent, bw, bh) == (1, 48, 4, 4) and total == len(b) and ioff + 48 * count == total, '头'
    rows = {}
    last = b''
    for i in range(count):
        k = b[ioff + 48 * i:ioff + 48 * i + 32]; off, n = struct.unpack_from('<QQ', b, ioff + 48 * i + 32)
        assert k > last and off >= 64 and off % 16 == 0 and n % 16 == 0 and off + n <= ioff, f'索引第 {i} 项'
        last = k; rows[k] = b[off:off + n]
    return rows


RED_BC1 = struct.pack('<HHI', 0xF800, 0xF800, 0)   # 两端点都是 565 纯红、索引全 0 → 16 个 (255,0,0,255)


class AstcPackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='astc_pack_test.'))
        rnd = random.Random(1115)
        rand = lambda l, L, n: bytes(rnd.getrandbits(8) for _ in range(n))
        def bc7(l, L, n):   # BC7 模式 6（首字节低 7 位 1000000）随机端点：合法块
            out = bytearray()
            for _ in range(n // 16): blk = bytearray(rnd.getrandbits(8) for _ in range(16)); blk[0] = 0x40; out += blk
            return bytes(out)
        cls.bc1 = make_tex(0x1a01, 1, 8, 64, 32, 3, 1, 2, 1, rand)              # 3 级、非方形、GOB 高 4
        cls.bc3 = make_tex(0x1c06, 3, 16, 24, 20, 2, 2, 1, 5, rand)             # 2 层数组、非 4 整倍尺寸、sRGB
        cls.bc7 = make_tex(0x2001, 7, 16, 8, 8, 1, 1, 0, 1, bc7)
        cls.red = make_tex(0x1a06, 1, 8, 8, 4, 1, 1, 0, 1, lambda l, L, n: RED_BC1 * (n // 8))
        cls.rgba = make_tex(0x0b01, 0, 4, 16, 16, 1, 1, 0, 1, rand)             # RGBA8：不收
        cls.bc4 = make_tex(0x1d01, 4, 8, 16, 16, 1, 1, 0, 1, rand)              # BC4：不收（不省内存）
        cls.bad = make_tex(0x1a01, 1, 8, 64, 64, 2, 1, 1, 1, rand, break_offsets=True)   # 级起点对不上：整张不收
        cls.sw2 = make_tex(0x1a01, 1, 8, 32, 32, 1, 1, 0, 1, rand)              # 放进 Switch 2 BNTX：不收
        romfs = cls.tmp / 'romfs'; (romfs / 'a').mkdir(parents=True)
        (romfs / 'a' / 'one.bntx').write_bytes(bntx([cls.bc1, cls.rgba, cls.bad]))
        inner = sarc([('x/two.bntx', bntx([cls.bc3, cls.bc4])), ('readme.txt', b'not a texture')])
        (romfs / 'a' / 'pack.szs').write_bytes(yaz0_literal(inner))
        model = b'FRES' + b'\0' * 60 + bntx([cls.bc7, cls.red])                  # 嵌在 BFRES 里的 BNTX
        try:
            from compression import zstd
            (romfs / 'model.bfres.zs').write_bytes(zstd.compress(model)); cls.zstd = True
        except ImportError:
            (romfs / 'model.bfres').write_bytes(model); cls.zstd = False
        (romfs / 'sw2.bntx').write_bytes(bntx([cls.sw2], platform=b'Ounc'))
        (romfs / 'big.dat').write_bytes(b'\0' * 4096)
        cls.romfs = romfs
        cls.out = cls.tmp / 'out.pack'
        cls.rep = ap.build(str(romfs), str(cls.out), ap.QUALITY['fast'], 4, TOOL_DIR, say=lambda s: None)
        cls.rows = parse_pack(cls.out)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def expected(self):
        want = {}
        for t in (self.bc1, self.bc3, self.bc7, self.red):
            for (l, L), (w, h, lin) in t['planes'].items(): want[key(t['fam'], w, h, lin)] = (w, h)
        return want

    def test_key_golden(self):
        # 与 graphics/metal/tests/t_astc.c 同一个黄金值（printf … | shasum -a 256）：出包与运行时口径一致
        self.assertEqual(ap.plane_key(1, 4, 4, bytes(range(8))).hex(), 'cf2afb54f06542892aba31a135661c3ef9c351863bde6e7267e6643cb26d109f')

    def test_keys_and_sizes(self):
        want = self.expected()
        self.assertEqual(set(self.rows), set(want), '包里的键应正好是 BC1(3 级) + BC3(2 级 × 2 层) + BC7 + 纯红 BC1 的各平面')
        self.assertEqual(len(want), 3 + 4 + 1 + 1)
        for k, (w, h) in want.items():
            blocks = -(-w // 4) * -(-h // 4)
            self.assertEqual(len(self.rows[k]), blocks * 16, f'{w}x{h} 平面的 ASTC 应是 {blocks} 块 × 16 字节')

    def test_skips_counted(self):
        r = self.rep
        self.assertEqual(r.get('layout_differs'), 1, r)
        self.assertEqual(r.get('skip_switch2'), 1, r)
        self.assertEqual(r.get('converted'), 4, r)
        self.assertEqual(r.get('yaz0'), 1, r); self.assertEqual(r.get('sarc'), 1, r)
        if self.zstd: self.assertEqual(r.get('zstd'), 1, r)
        self.assertEqual(r['entries'], len(self.rows))

    def test_red_is_constant_block(self):
        (w, h, lin), = self.red['planes'].values()
        a = self.rows[key(1, w, h, lin)]
        self.assertEqual(len(a), 32)
        for i in range(0, 32, 16):
            self.assertEqual(a[i:i + 2], b'\xfc\xfd', '纯色块应编成空区块（块模式 0x1FC、LDR）')
            r, g, b_, al = struct.unpack_from('<4H', a, i + 8)
            self.assertEqual((r, g, b_, al), (0xFFFF, 0, 0, 0xFFFF), '颜色应是红 (1,0,0,1)')

    def test_deterministic(self):
        again = self.tmp / 'again.pack'
        ap.build(str(self.romfs), str(again), ap.QUALITY['fast'], 2, TOOL_DIR, say=lambda s: None)
        self.assertEqual(again.read_bytes(), self.out.read_bytes(), '同样的输入（换并行度）应出同样的字节')

    def test_reader_agrees(self):
        head, rows = ap.read_index(str(self.out))
        self.assertEqual(head['count'], len(self.rows))
        self.assertEqual({k for k, _, _ in rows}, set(self.rows))



def bc7_constant(r, g, b, a):
    # Mode 6 uses shared P bits. Use 8-bit endpoints with the same low bit.
    assert len({v & 1 for v in (r, g, b, a)}) == 1
    block = 0x40
    for i, v in enumerate((r, g, b, a)):
        block |= (v >> 1) << (7 + 14 * i)
        block |= (v >> 1) << (14 + 14 * i)
    return (block | ((a & 1) * 3 << 63)).to_bytes(16, 'little')


def bc7_cutout(w, h):
    # Deliberately difficult independent alpha and RGB axes. The first alpha
    # index must fit the one-bit anchor; the remaining indices can be 0 or 3.
    blocks = []
    for y in range((h + 3) // 4):
        for x in range((w + 3) // 4):
            rgb = (21 + 7*x, 27 + 14*y, 101 + (3*x + 5*y) % 57)
            block = 0x20 | 255 << 58
            for ch, (v, delta) in enumerate(zip(rgb, (30, 18, 7))):
                block |= (v >> 1) << (8 + 14*ch)
                block |= ((v + delta) >> 1) << (15 + 14*ch)
            for i in range(1,16):
                u, v = i % 4, i // 4
                block |= u << (65 + 2*i)
                block |= (3 * (u + v >= 2 + (x+y) % 4)) << (96 + 2*i)
            blocks.append(block.to_bytes(16, 'little'))
    return b''.join(blocks)


@unittest.skipIf(os.name == 'nt', 'Android 的只读 POSIX 文件接口')
class BC7VerifiedPackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.dir = Path(temporary.name)
        helper = cls.dir / 'probe.c'
        helper.write_text("""
#include "bc7_astc_pack.inc"
int lookup(const char *path, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers,
           const uint8_t *source, size_t bytes, uint8_t *out) {
    struct bc7_astc_pack *p = bc7_pack_open(path);
    if (!p) return -1;
    uint8_t key[32];
    bc7_pack_key(w,h,levels,layers,source,bytes,key);
    int hit = bc7_pack_lookup(p,key,out,bytes);
    bc7_pack_close(p);
    return hit;
}
int rejected(const char *path, const char *receipt, const uint8_t key[32]) {
    struct bc7_astc_pack *p = bc7_pack_open(path);
    if (!p) return -1;
    bc7_pack_load_rejects(p, receipt);
    int is_rejected = bc7_pack_rejected(p, key);
    bc7_pack_close(p);
    return is_rejected;
}
""", encoding='utf-8')
        cls.reader = cls.dir / 'reader.so'
        direct = ROOT / 'android/native/direct'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                        '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        '-I', str(direct.parent), '-I', str(ROOT / 'android/native/nvn'),
                        '-o', str(cls.reader), str(helper), str(direct / 'sha256.c')], check=True)
        cls.lookup = ctypes.CDLL(str(cls.reader)).lookup
        cls.lookup.argtypes = [ctypes.c_char_p] + [ctypes.c_uint32] * 4 + [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lookup.restype = ctypes.c_int
        cls.rejected = ctypes.CDLL(str(cls.reader)).rejected
        cls.rejected.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
        cls.rejected.restype = ctypes.c_int

    def test_whole_image_quality_and_lookup(self):
        dims = (65, 33, 2, 2)
        big, small = ap.astc_bytes(65, 33), ap.astc_bytes(32, 16)
        red, blue, clear = (bc7_constant(*v) for v in [
            (255, 1, 1, 255), (1, 1, 255, 255), (0, 0, 0, 0)])
        good = red * (big//16) + blue * (big//16) + clear * (small//16) + red * (small//16)
        # Only the last mip and layer is out of gate. A partially good image
        # must never add a row or any of its successful planes to the index.
        bad = good[:-small] + bc7_cutout(32,16)
        reserved = good[:-16] + bytes(16)
        for name, payload in (('good',good),('bad',bad),('reserved',reserved)):
            (self.dir / name).write_bytes(payload)
        geom = dict(zip(('width','height','levels','layers'),dims))
        manifest = self.dir / 'images.json'
        manifest.write_text(json.dumps({'version': 1, 'images':
            [dict(geom, path=name) for name in ('good','bad','reserved','good')]}), encoding='utf-8')
        out = self.dir / 'checked.pack'
        rep = ap.build_bc7(manifest,out,TOOL_DIR)
        self.assertEqual(rep, {'entries':1,'rejected':2,'pack_bytes':64+len(good)+80})
        disk = out.read_bytes()
        head = struct.unpack_from('<8sIIQQQ',disk)
        self.assertEqual(head, (b'S2ABCAQ2',2,80,1,64+len(good),len(disk)))
        index = disk[-80:]
        key = hashlib.sha256(b's2a-bc7-astc-image-v1' + struct.pack('<4I',*dims) + good).digest()
        self.assertEqual(index[:32], key)
        self.assertEqual(index[48:], hashlib.sha256(disk[64:-80]).digest())

        result = ctypes.create_string_buffer(len(good))
        def lookup(path, source=good, shape=dims):
            return type(self).lookup(os.fsencode(path), *shape, source, len(source), result)
        self.assertEqual(lookup(out), 1)
        self.assertEqual(result.raw,disk[64:-80])
        self.assertEqual(lookup(out,bad),0)
        self.assertEqual(lookup(out,reserved),0)
        self.assertEqual(lookup(out,shape=(64,33,2,2)),0)
        self.assertEqual(lookup(self.dir/'missing'),-1)
        damaged = self.dir/'damaged.pack'
        damaged.write_bytes(disk[:-1])
        self.assertEqual(lookup(damaged),-1)
        mutated = bytearray(disk)
        mutated[-48] ^= 16  # Index payload offset.
        damaged.write_bytes(mutated)
        self.assertEqual(lookup(damaged),-1)
        mutated = bytearray(disk)
        mutated[64] ^= 1    # Payload bytes with an otherwise sound index.
        damaged.write_bytes(mutated)
        self.assertEqual(lookup(damaged),0)
        second = self.dir/'again.pack'
        ap.build_bc7(manifest,second,TOOL_DIR)
        self.assertEqual(second.read_bytes(),disk)

    def test_quality_gate_checks_alpha_and_color(self):
        lib = ap.Lib(ap.build_tool(TOOL_DIR)).L
        ctx = lib.b2a_ctx_new(99.0)
        self.assertTrue(ctx)
        self.addCleanup(lib.b2a_ctx_free,ctx)
        red = bc7_constant(255,1,1,255)
        transparent_red = bc7_constant(254,0,0,0)
        blue = bc7_constant(1,1,255,255)
        encoded = ctypes.create_string_buffer(16)
        self.assertEqual(lib.b2a_encode(ctx,7,4,4,red,16,encoded,16),0)
        self.assertEqual(lib.b2a_bc7_verify(ctx,4,4,red,16,encoded,16),0)
        self.assertEqual(lib.b2a_bc7_verify(ctx,4,4,transparent_red,16,encoded,16),1)
        self.assertEqual(lib.b2a_bc7_verify(ctx,4,4,blue,16,encoded,16),1)
    def test_container_canonical_planes_and_skips(self):
        def plane(level, layer, size):
            return b''.join(bc7_constant(2*((i + 5*level) % 100)+1, 1+2*layer, 3, 255)
                            for i in range(size // 16))

        # Quantized coordinates differ between blocks, mips and layers.
        tex = make_tex(0x2001, 7, 16, 65, 65, 3, 2, 2, 5, plane)
        gold = b''.join(tex['planes'][l, y][2] for l in range(3) for y in range(2))
        key = hashlib.sha256(b's2a-bc7-astc-image-v1' + struct.pack('<4I',65,65,3,2) + gold).hexdigest()

        def xtx(tex, layout=2):
            def section(kind, index, body, pad=0):
                return struct.pack('<4sIQQIII',b'HBvN',36,len(body),36+pad,kind,index,0) + bytes(pad) + body
            data = tex['data']
            head = struct.pack('<Q8I',len(data),512,65,65,1,4,0x4d,3,len(data)//2)
            offsets = struct.pack('<17I', *tex['offs'], *([0]*14))
            return (struct.pack('<4sIII',b'DFvN',16,1,1) +
                    section(2,0,head+offsets+struct.pack('<III',layout,7,0)) +
                    section(3,1,data,304) + section(5,2,bytes(24)))

        lib = ap.Lib(ap.build_tool(TOOL_DIR))
        cases = {'bntx': bntx([tex]), 'xtx': xtx(tex)}
        for name, data in cases.items():
            with self.subTest(name=name):
                stats = {}
                self.assertEqual(list(ap.bc7_container_images(lib, data, stats)), [(65,65,3,2,gold)])
                self.assertFalse(stats.get('layout_reject'))
        # 声明 GOB=0 与级起点冲突。GOB=1 可有同尺寸填充，
        # 但实际块坐标变了，独立 canonical SHA 仍应明确不同。
        self.assertFalse(list(ap.bc7_container_images(lib, xtx(tex,layout=0), {})))
        alternate, = ap.bc7_container_images(lib, xtx(tex,layout=1), {})
        self.assertNotEqual(ap.bc7_image_key(*alternate).hex(), key)
        self.assertEqual(list(ap.bc7_container_images(lib, bntx([dict(tex, offs=[0,4096,8192])]), {})), [])
        self.assertEqual(list(ap.bc7_container_images(lib, bntx([tex], platform=b'Ounc'), {})), [])
        self.assertEqual(list(ap.bc7_container_images(lib, xtx(tex)[:-512], {})), [])
        self.assertEqual(list(ap.bc7_container_images(lib, bntx([tex])[:-1], {})), [])
        self.assertEqual(list(ap.bc7_container_images(lib, b'BAD!' + xtx(tex)[4:], {})), [])

        sources = self.dir / 'containers'
        sources.mkdir()
        for name, data in cases.items():
            (sources / f'image.{name}').write_bytes(data)
        bad = make_tex(0x2006, 7, 16, 65, 33, 2, 1, 1, 1,
                       lambda l, y, n: bc7_cutout(65,33) if l == 0 else plane(l,y,n))
        (sources/'quality.bntx').write_bytes(bntx([bad]))
        rejected_source = b''.join(bad['planes'][l,0][2] for l in range(2))
        rejected_key = ap.bc7_image_key(65,33,2,1,rejected_source)
        out = self.dir / 'container.pack'
        report = self.dir / 'container.tsv'
        rep = ap.build_bc7_containers(sources,out,report,TOOL_DIR)
        self.assertEqual((rep['entries'], rep['rejected']), (1,1))
        self.assertIn(f'{key}\t65\t65\t3\t2\t{len(gold)}\taccepted', report.read_text())
        log = self.dir / 'workset.log'
        content = (f'[bc7-workset] session schema=1\n'
                   f'[bc7-workset] image key={key} width=65 height=65 levels=3 layers=2 bc_bytes={len(gold)} first=disabled\n'
                   f'[bc7-workset] image key={rejected_key.hex()} width=65 height=33 levels=2 layers=1 bc_bytes={len(rejected_source)} first=quality_reject\n'
                   f'[bc7-workset] summary final=1 whole=2 unique=2 unique_bytes={len(gold)+len(rejected_source)} identity_lost=0\n')
        log.write_text(content)
        mapped = ap.bc7_compare(log,report)
        self.assertEqual(mapped['offline']['accepted'], {'images':1,'bc_bytes':len(gold)})
        self.assertEqual(mapped['offline']['quality_reject'], {'images':1,'bc_bytes':len(rejected_source)})
        log.write_text(content.replace('identity_lost=0','identity_lost=1'))
        with self.assertRaises(ValueError):
            ap.bc7_compare(log,report)

        result = ctypes.create_string_buffer(len(gold))
        self.assertEqual(type(self).lookup(os.fsencode(out),65,65,3,2,gold,len(gold),result),1)
        wrong = b''.join(tex['planes'][l,y][2] for y in range(2) for l in range(3))
        self.assertNotEqual(wrong,gold)
        receipt = Path(str(out)+'.rejects')
        probe = type(self).rejected
        self.assertEqual(probe(os.fsencode(out),os.fsencode(receipt),rejected_key),1)
        self.assertEqual(probe(os.fsencode(out),os.fsencode(receipt),bytes.fromhex(key)),0)
        damaged = self.dir / 'damaged.rejects'
        damaged.write_bytes(receipt.read_bytes()[:-1])
        self.assertEqual(probe(os.fsencode(out),os.fsencode(damaged),rejected_key),0)
        mutated = bytearray(receipt.read_bytes())
        mutated[16] ^= 1
        damaged.write_bytes(mutated)
        self.assertEqual(probe(os.fsencode(out),os.fsencode(damaged),rejected_key),0)
        self.assertEqual(type(self).lookup(os.fsencode(out),65,65,3,2,wrong,len(wrong),result),0)


class UnzstdCliTest(unittest.TestCase):
    """没有 compression.zstd 时（Windows 随包 Python 3.12）退到 zstd 命令行：带字典的帧也要解得开"""

    def test_dict_frame_via_cli(self):
        try:
            from compression import zstd
        except ImportError:
            self.skipTest('这台机器的 Python 没有 compression.zstd，造不了带字典的样本')
        if not shutil.which('zstd'):
            self.skipTest('没有 zstd 命令行')
        plain = b'texture-' * 512 + bytes(range(256)) * 8
        zd = zstd.train_dict([plain[i:i + 300] + bytes([i % 251]) * 40 for i in range(0, 4000, 37)], 2048)
        frame = zstd.compress(plain, zstd_dict=zd)
        did = zd.dict_id
        self.assertNotEqual(did, 0, '训练出的字典应带编号')
        real = ap._zstd
        ap._zstd = lambda: None   # 装作没有 compression.zstd
        try:
            self.assertEqual(ap.unzstd(frame, {did: zd.dict_content}), plain)
            self.assertEqual(ap.unzstd(zstd.compress(plain), {}), plain)
        finally:
            ap._zstd = real


if __name__ == '__main__':
    unittest.main(verbosity=2)
