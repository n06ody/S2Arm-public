#!/usr/bin/env python3
"""安卓首局预热随包种子整理（android/tools/vk_warm_seed.py）单测：python3 android/tests/vk_warm_seed_test.py

文件按宿主格式手写（本夹具程序清单 60 + n*600 + 8、管线清单 32 + n*388，见 android/native/nvn/nvn_vk_{prog,pipe}_warm.c），
期望的目录名、条数、字节由测试自己算，不调被测实现：
  ① 一台设备一份（程序 P1 P2 + 管线 P1 P2 P9）→ <闭包 hex16>-<profile hex16>/ 下：程序清单逐字节原样，管线清单只剩 P1 P2（P9 的程序不在），
     条数 2、校验和按手算 FNV-1a 重算
  ② 坏文件不收不崩：程序清单 FNV 坏、文件名闭包与文件头不符；管线清单条目大小不符 → 程序照收、不带管线
  ③ 同闭包同 profile 多份来源：先比配对的图形管线条目数，再比程序；换来源先后、换子目录名结果逐字节相同
  ④ 不同 profile 两份：各出一个目录
  ⑤ 没有来源 / 来源不存在：输出目录是空的、退 0、汇总行写「空」
  ⑥ 默认来源：S2A_VK_WARM_SOURCES 没给时读 <S2A_RUN_ROOT>/s2arm/vk_warm/<程序号>/
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'tools', 'vk_warm_seed.py')


def read(path):
    with open(path, 'rb') as f: return f.read()


def fnv(b):   # 宿主清单的 FNV-1a 起始值是 1469598103934665603（nvn_vk_{prog,pipe}_warm.c 原样，不是标准的 14695981039346656037），照抄
    h = 1469598103934665603
    for x in b: h = ((h ^ x) * 0x100000001b3) % (1 << 64)
    return h


def ident(n): return bytes((n * 31 + i + 1) & 0xFF for i in range(32))


def sha8(s): return hashlib.sha256(s.encode()).digest()[:8]


def prog_file(ids, profile='profile-A', closure='closure-A', corrupt=False, window=0):
    hdr = struct.pack('<IIII', 0x31575053, 2, 88, len(ids)) + hashlib.sha256(profile.encode()).digest() + struct.pack('<i', 7) + sha8(closure)
    assert len(hdr) == 60
    body = b''.join(ident(n) + struct.pack('<I', 3) + bytes(24) + bytes([3, 2, window, 0]) + struct.pack('<IIIIQ', 0x100, 0x100, 0, 9, 1) + bytes([n]) * 0x200 for n in ids)   # 88 字节头 + 两段 0x100 的 c1
    b = hdr + body
    return b + struct.pack('<Q', fnv(b) ^ (1 if corrupt else 0)), 'programs-v1-' + sha8(closure).hex() + '.bin'


def pipe_entry(n): return ident(n) + bytes([3]) + bytes([n]) * 351 + struct.pack('<I', 5)


def pipe_file(ids, entry_size=388):
    body = b''.join(pipe_entry(n) for n in ids)
    return b'S2APW1\0\0' + struct.pack('<IIII', 1, 352, entry_size, len(ids)) + struct.pack('<Q', fnv(body)) + body


def put(d, name, b):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), 'wb') as f: f.write(b)


def run(out, *srcs, env=None):
    e = dict(os.environ); e.pop('S2A_VK_WARM_SOURCES', None); e.update(env or {})
    r = subprocess.run([sys.executable, TOOL, out, '0100AAAA00000000', *srcs], capture_output=True, text=True, env=e)
    return r.returncode, r.stdout


def tree(d):
    res = {}
    for root, _, files in os.walk(d):
        for f in files:
            p = os.path.join(root, f); res[os.path.relpath(p, d)] = read(p)
    return res


class T(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory(); self.d = self.t.name
        self.dir_a = sha8('closure-A').hex() + '-' + sha8('profile-A').hex()

    def tearDown(self): self.t.cleanup()

    def test_1_pair_and_filter(self):
        pb, pn = prog_file([1, 2]); src = os.path.join(self.d, 'src')
        put(src, pn, pb); put(src, 'pipes-v1-352.bin', pipe_file([1, 2, 9]))
        rc, out = run(os.path.join(self.d, 'out'), src)
        self.assertEqual(rc, 0, out)
        got = tree(os.path.join(self.d, 'out'))
        self.assertEqual(sorted(got), sorted([f'{self.dir_a}/{pn}', f'{self.dir_a}/pipes-v1-352.bin']))
        self.assertEqual(got[f'{self.dir_a}/{pn}'], pb)
        body = pipe_entry(1) + pipe_entry(2)
        want = b'S2APW1\0\0' + struct.pack('<IIII', 1, 352, 388, 2) + struct.pack('<Q', fnv(body)) + body
        self.assertEqual(got[f'{self.dir_a}/pipes-v1-352.bin'], want)
        self.assertIn('程序 2 条、管线 2 条（程序不在的管线 1 条不收）', out)

    def test_2_bad_files_skipped(self):
        pb, pn = prog_file([1, 2]); bad, _ = prog_file([3], corrupt=True); other, on = prog_file([4], closure='closure-B')
        src = os.path.join(self.d, 'src')
        put(src, pn, pb); put(src, 'pipes-v1-352.bin', pipe_file([1], entry_size=400))
        put(os.path.join(src, 'b'), pn, bad)                                  # 校验和坏
        put(os.path.join(src, 'c'), 'programs-v1-0000000000000000.bin', other)   # 文件名的闭包与文件头不符
        rc, out = run(os.path.join(self.d, 'out'), src)
        self.assertEqual(rc, 0, out)
        got = tree(os.path.join(self.d, 'out'))
        self.assertEqual(sorted(got), [f'{self.dir_a}/{pn}'])
        self.assertEqual(out.count('【黄】'), 3, out)

    def test_3_more_programs_break_ties_order_free(self):
        p2, pn = prog_file([1, 2]); p3, _ = prog_file([1, 2, 3])
        a, b = os.path.join(self.d, 'a'), os.path.join(self.d, 'b')
        put(a, pn, p2); put(a, 'pipes-v1-352.bin', pipe_file([1]))
        put(b, pn, p3); put(b, 'pipes-v1-352.bin', pipe_file([3]))
        run(os.path.join(self.d, 'o1'), a, b); run(os.path.join(self.d, 'o2'), b, a)
        x, y = os.path.join(self.d, 'x'), os.path.join(self.d, 'y')
        put(os.path.join(x, 'zz'), pn, p2); put(os.path.join(x, 'zz'), 'pipes-v1-352.bin', pipe_file([1]))
        put(os.path.join(x, 'aa'), pn, p3); put(os.path.join(x, 'aa'), 'pipes-v1-352.bin', pipe_file([3]))
        run(os.path.join(self.d, 'o3'), x)
        o1, o2, o3 = (tree(os.path.join(self.d, k)) for k in ('o1', 'o2', 'o3'))
        self.assertEqual(o1[f'{self.dir_a}/{pn}'], p3)
        self.assertEqual(o1, o2); self.assertEqual(o1, o3)

    def test_graphics_recipes_win_over_program_only_capture(self):
        src = os.path.join(self.d, 'src')
        p_big, pn = prog_file([1, 2, 3])
        p_route, _ = prog_file([4, 5], window=1)
        put(os.path.join(src, 'other'), pn, p_big)
        put(os.path.join(src, 'route'), pn, p_route)
        pipes = pipe_file([4, 5])
        put(os.path.join(src, 'route'), 'pipes-v1-352.bin', pipes)
        out = os.path.join(self.d, 'out')
        rc, log = run(out, src)
        self.assertEqual(rc, 0, log)
        got = tree(out)
        self.assertIn(f'{self.dir_a}/pipes-v1-352.bin', list(got), '登阶配方被程序更多的无管线清单盖掉')
        self.assertEqual(got[f'{self.dir_a}/pipes-v1-352.bin'], pipes)
        self.assertEqual(got[f'{self.dir_a}/{pn}'], p_route, '窗口合同字节和 C1 必须原样随包')

    def test_tied_programs_with_different_recipes_are_order_independent(self):
        src1, src2 = (os.path.join(self.d, name) for name in ('a', 'b'))
        programs, pn = prog_file([1, 2])
        for src, ident in ((src1, 1), (src2, 2)):
            put(src, pn, programs)
            put(src, 'pipes-v1-352.bin', pipe_file([ident]))
        out1, out2 = (os.path.join(self.d, name) for name in ('out1', 'out2'))
        self.assertEqual(run(out1, src1, src2)[0], 0)
        self.assertEqual(run(out2, src2, src1)[0], 0)
        relative = f'{self.dir_a}/pipes-v1-352.bin'
        self.assertEqual(hashlib.sha256(tree(out1)[relative]).hexdigest(),
                         hashlib.sha256(tree(out2)[relative]).hexdigest(),
                         '相同程序字节不能让来源顺序决定随包配方')

    def test_4_two_profiles(self):
        pa, pn = prog_file([1]); pbb, _ = prog_file([2], profile='profile-B')
        put(os.path.join(self.d, 's', '1'), pn, pa); put(os.path.join(self.d, 's', '2'), pn, pbb)
        run(os.path.join(self.d, 'out'), os.path.join(self.d, 's'))
        dir_b = sha8('closure-A').hex() + '-' + sha8('profile-B').hex()
        self.assertEqual(sorted(tree(os.path.join(self.d, 'out'))), sorted([f'{self.dir_a}/{pn}', f'{dir_b}/{pn}']))

    def test_5_no_source(self):
        rc, out = run(os.path.join(self.d, 'out'), os.path.join(self.d, 'nope'))
        self.assertEqual(rc, 0); self.assertEqual(tree(os.path.join(self.d, 'out')), {}); self.assertIn('空', out)

    def test_6_default_source(self):
        pb, pn = prog_file([1]); put(os.path.join(self.d, 'run', 's2arm', 'vk_warm', '0100AAAA00000000'), pn, pb)
        rc, out = run(os.path.join(self.d, 'out'), env={'S2A_RUN_ROOT': os.path.join(self.d, 'run')})
        self.assertEqual(rc, 0, out); self.assertEqual(sorted(tree(os.path.join(self.d, 'out'))), [f'{self.dir_a}/{pn}'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
