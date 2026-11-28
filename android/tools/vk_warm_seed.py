#!/usr/bin/env python3
"""安卓首局预热随包种子：vk_warm_seed.py <输出目录> <程序号 | 产物根> [来源…]

把安卓真机跑局落下的两份预热清单（运行时 <状态目录>/direct-cache/nvn-vk/ 下的
programs-v1-<闭包摘要>.bin 与 pipes-v1-352.bin，格式见 android/native/nvn/nvn_vk_{prog,pipe}_warm.c）
整理成包里的种子：<输出目录>/<闭包摘要前 8 字节 hex>-<profile 摘要前 8 字节 hex>/{programs-v1-<闭包>.bin, pipes-v1-352.bin}。
宿主首局（本机还没有清单）按本局的闭包与 profile 找同名目录读它（nvk_prog_warm_load / nvk_pipe_warm_load「随包种子」）。

为什么种子只能来自安卓真机、不从苹果的跑局知识（seen.tsv）转：安卓清单的程序身份是 S2AI1 v3 规范输入的 SHA-256
（android/native/direct/input_identity.c：设备 profile、翻译器闭包、完整源摘要、纹理事实、绘制事实），c1 常量段是现场捕获的；
seen.tsv 的 R 行只有 8 字节阶段键和 Metal 侧状态，算不出这个身份，也缺 Vulkan 管线要的图元类型、深度模板、光栅状态等字段。

来源：给了就用参数（目录，或目录里的单个文件——取它所在目录）；没给 = S2A_VK_WARM_SOURCES（系统分隔符），
再没有 = <运行根>/s2arm/vk_warm/<程序号>/。每个来源目录本身和它的一级子目录各算一份「一台设备一局的清单」：
目录里每份格式合法的程序清单配同目录的管线清单（可以没有）。
校验与宿主同一口径（不符的整份不收，打一行说明）：程序清单 magic / 版本 / 条目大小 / 条数 / 长度 / FNV-1a，文件名里的闭包摘要 = 文件头；
管线清单 magic / 格式 / 描述版本 / 条目大小 / 条数 / 长度 / FNV-1a。管线条目只收程序身份在配对程序清单里的（宿主同规则：
不在的那条首局等不到程序发布，只会白占重试槽），过滤后整份重写（条数与校验和重算）。
同一目录名（同闭包同 profile）多份来源时取程序条目多的那份，一样多取程序清单 SHA-256 小的；输出只与来源内容有关（与路径、修改时间、来源先后无关）。
不存在的来源跳过；没有任何可用来源时输出目录是空的（= 包里不带种子，首局照常首遇现建）。最后一行是汇总。
"""
import hashlib
import os
import shutil
import struct
import sys

PROG_MAGIC, PROG_VERSION, PROG_HDR, PROG_ENTRY = 0x31575053, 3, 60, 112
PROG_STAGES, PROG_C1_OK, PROG_C1_LEN, PROG_USES = 5, 76, 80, 100
PROG_C1_MIN, PROG_C1_MAX = 0x100, 0x10000   # NVK_PROG_REC_C1_MIN / MAX
PIPE_MAGIC, PIPE_FORMAT, PIPE_DESC, PIPE_HDR = b'S2APW1\0\0', 1, 352, 32
PIPE_ENTRY = 32 + PIPE_DESC + 4
PIPE_NAME = f'pipes-v{PIPE_FORMAT}-{PIPE_DESC}.bin'
PROG_PREFIX = 'programs-v1-'
PROG_MAX, PIPE_MAX = 16384, 8192   # 宿主硬顶（NVK_PROG_WARM_ABS_MAX / NVK_PW_MAX_HARD）


def read(path):
    with open(path, 'rb') as f: return f.read()


def fnv1a64(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def prog_check(name, b, records=None):
    """程序清单 → (闭包 hex16, profile hex16, 身份集合)；坏了抛 ValueError（原因）"""
    if len(b) < PROG_HDR + 8: raise ValueError('文件太短')
    magic, ver, ent, cnt = struct.unpack_from('<IIII', b, 0)
    if magic != PROG_MAGIC: raise ValueError('magic 不符')
    if ver != PROG_VERSION: raise ValueError('版本不符')
    if ent != PROG_ENTRY: raise ValueError('条目大小不符')
    if cnt > PROG_MAX: raise ValueError('条数超过硬顶')
    end, off, ids = len(b) - 8, PROG_HDR, set()
    for _ in range(cnt):   # 变长条目：逐条走，c1 长度与有效位相符、在 [MIN, MAX]、合计正好落到校验和之前
        if off + PROG_ENTRY > end: raise ValueError('长度与条数不符')
        e = b[off:off + PROG_ENTRY]; off += PROG_ENTRY; ids.add(e[:32])
        c1_start = off
        for st in range(PROG_STAGES):
            cl = struct.unpack_from('<I', e, PROG_C1_LEN + 4 * st)[0]
            if not (e[PROG_C1_OK] >> st) & 1:
                if cl: raise ValueError('条目 c1 长度与有效位不符')
                continue
            if not PROG_C1_MIN <= cl <= PROG_C1_MAX: raise ValueError('条目 c1 长度越界')
            off += (cl + 3) & ~3
        if records is not None:
            if e[:32] in records: raise ValueError('程序身份重复')
            records[e[:32]] = (e, b[c1_start:off])
    if off != end: raise ValueError('长度与条数不符')
    if struct.unpack_from('<Q', b, len(b) - 8)[0] != fnv1a64(b[:-8]): raise ValueError('FNV 校验不符')
    closure, profile = b[52:60].hex(), b[16:24].hex()
    if name != f'{PROG_PREFIX}{closure}.bin': raise ValueError(f'文件名与文件头的闭包摘要 {closure} 不符')
    return closure, profile, ids


def pipe_check(b):
    """管线清单 → 条目列表（每条 388 字节）；坏了抛 ValueError"""
    if len(b) < PIPE_HDR or b[:8] != PIPE_MAGIC: raise ValueError('magic 不符')
    fmt, desc, ent, cnt = struct.unpack_from('<IIII', b, 8)
    if fmt != PIPE_FORMAT: raise ValueError('格式版本不符')
    if desc != PIPE_DESC: raise ValueError('描述版本不符')
    if ent != PIPE_ENTRY: raise ValueError('条目大小不符')
    if cnt > PIPE_MAX: raise ValueError('条数超过硬顶')
    if len(b) != PIPE_HDR + cnt * PIPE_ENTRY: raise ValueError('文件长度与条数不符')
    if struct.unpack_from('<Q', b, 24)[0] != fnv1a64(b[PIPE_HDR:]): raise ValueError('整份校验和不符')
    return [b[PIPE_HDR + i * PIPE_ENTRY:PIPE_HDR + (i + 1) * PIPE_ENTRY] for i in range(cnt)]


def pipe_bytes(entries):
    body = b''.join(entries)
    return PIPE_MAGIC + struct.pack('<IIII', PIPE_FORMAT, PIPE_DESC, PIPE_ENTRY, len(entries)) + struct.pack('<Q', fnv1a64(body)) + body


def source_dirs(srcs):
    seen = []
    for s in srcs:
        d = s if os.path.isdir(s) else os.path.dirname(s) if os.path.isfile(s) else None
        if not d: continue
        for x in [d] + [os.path.join(d, n) for n in sorted(os.listdir(d)) if os.path.isdir(os.path.join(d, n)) and not n.startswith('.')]:
            if x not in seen: seen.append(x)
    return seen


def default_sources(tid):
    env = os.environ.get('S2A_VK_WARM_SOURCES')
    if env: return [p for p in env.split(os.pathsep) if p]
    run = os.environ.get('S2A_RUN_ROOT') or os.path.join(os.path.expanduser('~'), '.s2arm', 'run')
    return [os.path.join(run, 's2arm', 'vk_warm', tid)]


def program_id(a):   # 程序号：同 graphics/tools/direct/run_knowledge.py program_id（= pack/srcs.sh prog_id）
    if not os.path.isdir(a): return a.upper()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'graphics', 'tools', 'direct'))
    from run_knowledge import program_id as pid
    return pid(a)


def build(out, srcs, log=print):
    """→ {目录名: (排序键, 程序文件名, 程序清单字节, 管线字节或 None, 管线收下条数, 滤掉条数)}，并写出到 out"""
    pick, companions = {}, {}
    for d in source_dirs(srcs):
        names = sorted(os.listdir(d))
        pipe_entries = None
        if PIPE_NAME in names:
            try: pipe_entries = pipe_check(read(os.path.join(d, PIPE_NAME)))
            except ValueError as e: log(f'【黄】{os.path.join(d, PIPE_NAME)} 不收：{e}')
        for n in names:
            if not (n.startswith(PROG_PREFIX) and n.endswith('.bin')) or n.startswith('._'): continue
            b = read(os.path.join(d, n))
            try: closure, profile, ids = prog_check(n, b)
            except ValueError as e: log(f'【黄】{os.path.join(d, n)} 不收：{e}'); continue
            pb, kept, dropped = None, 0, 0
            if pipe_entries is not None:
                keep = [e for e in pipe_entries if e[:32] in ids]
                kept, dropped = len(keep), len(pipe_entries) - len(keep)
                if keep: pb = pipe_bytes(keep)
            key, rank = f'{closure}-{profile}', (-len(ids), hashlib.sha256(b).digest())
            companion = os.path.join(d, 'nowindow')
            present = os.path.isdir(companion)
            if key not in pick or rank < pick[key][0] or (rank == pick[key][0] and present and not companions[key]):
                companions[key] = companion if present else None
                pick[key] = (rank, n, b, pb, kept, dropped)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    for key in sorted(pick):
        _, n, b, pb, kept, dropped = pick[key]
        os.makedirs(os.path.join(out, key))
        with open(os.path.join(out, key, n), 'wb') as f: f.write(b)
        if pb:
            with open(os.path.join(out, key, PIPE_NAME), 'wb') as f: f.write(pb)
        if companions[key]:
            from vk_warm_nowindow import stage_companion
            stage_companion(companions[key], os.path.join(out, key, 'nowindow'), n, b, pb)
    return pick


def main(argv):
    if len(argv) < 2: sys.exit(__doc__)
    out, tid, srcs = argv[0], program_id(argv[1]), argv[2:]
    srcs = srcs or default_sources(tid)
    pick = build(out, srcs)
    if not pick:
        print(f'安卓首局预热种子：空（{tid} 没有安卓真机清单来源：首局首遇现建）'); return 0
    print('安卓首局预热种子：' + '；'.join(f'{k} 程序 {-v[0][0]} 条、管线 {v[4]} 条（程序不在的管线 {v[5]} 条不收）' for k, v in sorted(pick.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
