#!/usr/bin/env python3
# 直连库容器打包单测（C# LibPack.PackFiles，graphics/tools/direct/dsh/batch/LibPack.cs，经 dsh_batch --libpack pack 调；11-01 出库链挪进 C#，原 lib_pack.py 随之删除）。
#   手造按键文件目录（两键真编的小 metallib + json + _bf + _ft + _b_t1 + _bg3 两样 + inputs.tsv），打包进另一个目录后按格式（宿主读法 graphics/nvn/lib_pack.inc 文件头）自己解析 lib.idx（不调被测实现）：
#   期望：记录按 (键, 种类) 升序、种类号 = 0 b.metallib / 1 b.json / 2 bf / 50 ft / 5 b_t1 / 19 bg3.metallib / 35 bg3.json、每段字节 = 原文件、16 字节对齐、链版本 = 给的链号；
#   输出目录只有 lib.000.pak + lib.idx（inputs.tsv 不是键文件、不进包）；切片能建库（Mac 上按偏移 newLibraryWithData 取到 main0，同运行时读法）；
#   种类 51 = _se.metallib（宿主 LP_K_SE），_se.json 不是键文件
#   反证：认不出的键文件 → 退出 1、输出目录不留分片 / 索引 / 临时文件；再打一次（已有 lib.idx）→ 退出 1；
#   两段 40 MB 跨过 64 MB 分片上限 → 第二段进新分片、相同字节只存一段（分片号按手算）；成批合编（json fn_lib）两键种类 0 同一段 = 批库字节，批库不在 → 退出 1
import os, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../../../translator')); import dsh_batch
CMD, _ = dsh_batch.get()
fails = []
def check(cond, msg):
    print(('  ✓ ' if cond else '  ✗ ') + msg)
    if not cond: fails.append(msg)
def pack(src, out, chain): return subprocess.run([*CMD, '--libpack', 'pack', src, out, chain], capture_output=True, text=True)

K1, K2, CHAIN = '0000000000000abc', 'fedcba9876543210', '0a7dae3c1e7b'
_inc = open(os.path.join(HERE, '../../../nvn/lib_pack_core.inc'), encoding='utf-8').read()
check('LP_K_SE = 51' in _inc, '宿主 graphics/nvn/lib_pack_core.inc 的 LP_K_SE = 51（下面打包用同号）')
MSL = '#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(%s); }\n'

def build(t):
    d = os.path.join(t, 'src'); os.makedirs(d)
    for i, k in enumerate((K1, K2)):
        src = os.path.join(t, f'{k}.metal'); open(src, 'w').write(MSL % f'{i}.0')
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-o', os.path.join(d, f'{k}_b.metallib'), src], check=True)
        open(os.path.join(d, f'{k}_b.json'), 'w').write('{"key": "%s_b", "stage": "ps"}' % k)
    shutil.copyfile(os.path.join(d, f'{K1}_b.metallib'), os.path.join(d, f'{K1}_bf.metallib'))
    open(os.path.join(d, f'{K1}_ft.metallib'), 'wb').write(b'typed-output variants lib bytes')   # 片元输出类型变体（种类 50）
    open(os.path.join(d, f'{K2}_b_t1.metallib'), 'wb').write(b'tess domain 1 lib bytes')   # 曲面外壳另一种域（种类 4 + 1 = 5）
    open(os.path.join(d, f'{K2}_bg3.metallib'), 'wb').write(b'bg3 lib bytes')
    open(os.path.join(d, f'{K2}_bg3.json'), 'w').write('{"gs_slice": 1}')
    open(os.path.join(d, 'inputs.tsv'), 'w').write('game\tx\n')
    orig = {n: open(os.path.join(d, n), 'rb').read() for n in os.listdir(d)}
    return d, orig

def parse(d):
    idx = open(os.path.join(d, 'lib.idx'), 'rb').read()
    magic, ver, n, ns, _, chain = struct.unpack_from('<8sIIII12s', idx, 0)
    sizes = [struct.unpack_from('<QQ', idx, 48 + 16 * s)[0] for s in range(ns)]
    recs = [struct.unpack_from('<QBBHIQ', idx, 48 + 16 * ns + 24 * i) for i in range(n)]
    return magic, ver, chain, sizes, recs, len(idx) == 48 + 16 * ns + 24 * n

with tempfile.TemporaryDirectory() as t:
    d, orig = build(t); o = os.path.join(t, f'b-{CHAIN}')
    r = pack(d, o, CHAIN)
    check(r.returncode == 0, f'退出 0（{(r.stdout + r.stderr).strip()[-160:]}）')
    check(sorted(os.listdir(o)) == ['lib.000.pak', 'lib.idx'], f'输出目录只有 lib.000.pak / lib.idx（{sorted(os.listdir(o))}）')
    magic, ver, chain, sizes, recs, whole = parse(o)
    check(magic == b'S2ALIB\0\0' and ver == 1 and chain == CHAIN.encode() and whole, f'头：魔数 / 版本 1 / 链版本 = 给的链号 / 长度 = 48 + 16·分片 + 24·记录（{magic} {ver} {chain}）')
    want = [(int(K1, 16), 0, f'{K1}_b.metallib'), (int(K1, 16), 1, f'{K1}_b.json'), (int(K1, 16), 2, f'{K1}_bf.metallib'), (int(K1, 16), 50, f'{K1}_ft.metallib'),
            (int(K2, 16), 0, f'{K2}_b.metallib'), (int(K2, 16), 1, f'{K2}_b.json'), (int(K2, 16), 5, f'{K2}_b_t1.metallib'), (int(K2, 16), 19, f'{K2}_bg3.metallib'), (int(K2, 16), 35, f'{K2}_bg3.json')]
    check([(x[0], x[1]) for x in recs] == [(w[0], w[1]) for w in want], f'记录 = 9 段、按 (键, 种类) 升序、种类号照表（{[(hex(x[0]), x[1]) for x in recs]}）')
    pak = open(os.path.join(o, 'lib.000.pak'), 'rb').read()
    check(sizes == [len(pak)], f'分片表字节数 = 分片实际大小（{sizes} / {len(pak)}）')
    check(all(x[5] % 16 == 0 for x in recs), '每段起点 16 字节对齐')
    check(all(pak[x[5]:x[5] + x[4]] == orig[w[2]] for x, w in zip(recs, want)), '每段字节 = 原文件')
    check(sorted(os.listdir(d)) == sorted(orig), '输入目录原样不动')
    # 切片建库：同运行时读法（映射分片，按偏移切 newLibraryWithData）
    k1, k2 = (next(x for x in recs if x[0] == int(k, 16) and x[1] == 0) for k in (K1, K2))
    m = os.path.join(t, 'slice.m'); exe = os.path.join(t, 'slice')
    open(m, 'w').write('#import <Metal/Metal.h>\n#include <sys/mman.h>\n#include <fcntl.h>\n#include <sys/stat.h>\n#include <stdlib.h>\n'
                       'int main(int c, char **v) { @autoreleasepool { int fd = open(v[1], O_RDONLY); struct stat st; fstat(fd, &st); const char *p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);\n'
                       '  id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); int ok = 0;\n'
                       '  for (int i = 2; i + 1 < c; i += 2) { dispatch_data_t d = dispatch_data_create(p + atoll(v[i]), atoll(v[i + 1]), NULL, ^{}); NSError *e = nil;\n'
                       '    id<MTLLibrary> l = [dev newLibraryWithData:d error:&e]; ok += l && [l newFunctionWithName:@"main0"]; }\n'
                       '  printf("%d\\n", ok); } return 0; }\n')
    subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', m, '-o', exe], check=True, capture_output=True)
    out = subprocess.run([exe, os.path.join(o, 'lib.000.pak'), str(k1[5]), str(k1[4]), str(k2[5]), str(k2[4])], capture_output=True, text=True).stdout.strip()
    check(out == '2', f'两键 _b 切片各建得了库、取到 main0（{out}/2）')
    r = pack(d, o, CHAIN)
    check(r.returncode == 1 and '已有 lib.idx' in r.stderr, f'反证：再打一次 → 退出 1、报已有 lib.idx（{r.returncode}）')

with tempfile.TemporaryDirectory() as t:   # 种类 51：采样模拟变体 <键>_se.metallib；变体没有自己的反射，_se.json 不是键文件
    d = os.path.join(t, 'src'); os.makedirs(d)
    open(os.path.join(d, f'{K1}_b.json'), 'w').write('{}'); open(os.path.join(d, f'{K1}_se.metallib'), 'wb').write(b'se lib bytes')
    r = pack(d, os.path.join(t, 'o1'), CHAIN); recs = parse(os.path.join(t, 'o1'))[4] if r.returncode == 0 else []
    check(r.returncode == 0 and [(x[0], x[1]) for x in recs] == [(int(K1, 16), 1), (int(K1, 16), 51)], f'种类表：_se.metallib → 51（{[(hex(x[0]), x[1]) for x in recs]}）')
    open(os.path.join(d, f'{K1}_se.json'), 'w').write('{}')
    r = pack(d, os.path.join(t, 'o2'), CHAIN)
    check(r.returncode == 1 and '认不出的键文件' in r.stderr, f'种类表：_se.json 不认 → 退出 1（{r.returncode}）')

with tempfile.TemporaryDirectory() as t:
    d, orig = build(t); open(os.path.join(d, f'{K1}_bx.metallib'), 'wb').write(b'?'); o = os.path.join(t, f'b-{CHAIN}')
    r = pack(d, o, CHAIN)
    check(r.returncode == 1 and '认不出的键文件' in r.stderr, f'反证：认不出的键文件 → 退出 1（{r.returncode}，{r.stderr.strip()[-80:]}）')
    check(os.listdir(o) == [], f'反证：输出目录没有分片 / 索引 / 临时文件（{os.listdir(o)}）')

with tempfile.TemporaryDirectory() as t:   # 分片上限 64 MB（LibPack SHARD_MAX）：两段各 40 MB 放不进同一片
    d = os.path.join(t, 'src'); os.makedirs(d); MB = 1 << 20
    A, B = os.urandom(40 * MB), os.urandom(40 * MB)
    files = {f'{K1}_b.metallib': A, f'{K1}_b.json': b'{"k": 1}', f'{K1}_bf.metallib': A, f'{K2}_b.metallib': B, f'{K2}_b.json': b'{"k": 2}'}
    for n, b in files.items(): open(os.path.join(d, n), 'wb').write(b)
    o = os.path.join(t, 'o'); r = pack(d, o, CHAIN); magic, ver, chain, sizes, recs, whole = parse(o)
    # 手算：K1_b 进片 0 偏移 0；K1_b.json 接在 40 MB 处（40 MB 是 16 的倍数）；K1_bf 与 K1_b 同字节 → 同一段；K2_b 时片 0 已 40 MB + 8 字节，再放 40 MB 超 64 MB → 片 1 偏移 0；K2_b.json 接在片 1 的 40 MB 处
    check(r.returncode == 0 and whole and [(x[1], x[2], x[5]) for x in recs] == [(0, 0, 0), (1, 0, 40 * MB), (2, 0, 0), (0, 1, 0), (1, 1, 40 * MB)] and sizes == [40 * MB + 8, 40 * MB + 8],
          f'分片：两段 40 MB 跨上限 → 2 片，(种类, 分片, 偏移) 照手算、K1_bf 与 K1_b 共一段（{[(x[1], x[2], x[5]) for x in recs]}，片长 {sizes}）')
    paks = [open(os.path.join(o, f'lib.{s:03d}.pak'), 'rb').read() for s in range(len(sizes))]
    check(all(paks[x[2]][x[5]:x[5] + x[4]] == files[n] for x, n in zip(recs, files)), '多分片仍逐段 = 原文件')

# 成批合编：两键的 _b.metallib 都是批库 u<首键>.metallib 的克隆、json 记 fn_lib → 两条种类 0 记录同一（分片, 偏移, 长度），那段 = 批库字节；json 指向的批库不在 → 退出 1
UB = f'u{K1}'
def build_unity(t):
    d = os.path.join(t, 'src'); os.makedirs(d)
    src = os.path.join(t, 'u.metal'); open(src, 'w').write(MSL % '1.0')
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-o', os.path.join(d, UB + '.metallib'), src], check=True)
    for k in (K1, K2):
        shutil.copyfile(os.path.join(d, UB + '.metallib'), os.path.join(d, f'{k}_b.metallib'))
        open(os.path.join(d, f'{k}_b.json'), 'w').write('{"key": "%s_b", "stage": "ps", "fn_prefix": "s%s_", "fn_lib": "%s"}' % (k, k, UB))
    return d, open(os.path.join(d, UB + '.metallib'), 'rb').read()
with tempfile.TemporaryDirectory() as t:
    d, ub = build_unity(t); o = os.path.join(t, 'o')
    r = pack(d, o, CHAIN); magic, ver, chain, sizes, recs, whole = parse(o)
    b0 = [x for x in recs if x[1] == 0]; pak = open(os.path.join(o, 'lib.000.pak'), 'rb').read()
    check(r.returncode == 0 and len(b0) == 2 and b0[0][2:] == b0[1][2:] and pak[b0[0][5]:b0[0][5] + b0[0][4]] == ub, f'成批：两键种类 0 同一段且 = 批库字节（{[x[2:] for x in b0]}）')
    check(sorted(os.listdir(o)) == ['lib.000.pak', 'lib.idx'] and len(recs) == 4, f'成批：批库不单独成记录，输出只有分片与索引（{len(recs)} 条，{sorted(os.listdir(o))}）')
with tempfile.TemporaryDirectory() as t:
    d, ub = build_unity(t); os.remove(os.path.join(d, UB + '.metallib')); o = os.path.join(t, 'o')
    r = pack(d, o, CHAIN)
    check(r.returncode == 1 and '批库' in r.stderr and os.listdir(o) == [], f'成批反证：批库不在 → 退出 1、输出目录空（{r.returncode}，{r.stderr.strip()[-60:]}）')

print(f't_lib_pack：{"失败 " + str(len(fails)) if fails else "全过"}')
sys.exit(1 if fails else 0)
