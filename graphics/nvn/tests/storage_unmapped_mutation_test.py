#!/usr/bin/env python3
"""存储缓冲槽长度语义（rp_draw.c sb_len / sb_unmapped）的点变异：地址有效、长度 0 按硬件语义绑真实内存（池剩余长度，回写按它登记）；
没有可绑真实内存（地址 0 / 不在池里）才绑 64 KiB 空缓冲照画；统一缓冲来源（UBOP）不补长度。
每个变异体在 build/ 下的副本里重编 t_nvn_replay，t_storage_null_binding 必须红在指定的断言上；源码树不动。

用法：python3 storage_unmapped_mutation_test.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/storage-unmapped-mutations'
OUT.mkdir(parents=True, exist_ok=True)
CC = os.environ.get('CC', 'cc')
DRAW = 'graphics/nvn/replay/rp_draw.c'

mutations = [
    ('退回 Ryujinx 口径（地址有效、长度 0 当没映射、绑空缓冲）', [('    if (sb->size || !sbufp) return sb->size;', '    if (sb->size || !sbufp || 1) return sb->size;')],
     'g_fb[1][0].u.b.off == g[k].want_off && g_fb[1][0].u.b.len == g[k].want_len'),
    ('统一缓冲来源（UBOP）长度 0 也补池剩余长度', [('    if (sb->size || !sbufp) return sb->size;', '    if (sb->size) return sb->size;')],
     'nvn_unimpl_count("draw", "storage_buffer_unbound") == ub0 + 1'),
    ('可写长度 0 的回写仍按槽里的 0 登记', [('rp_post_gpu_write_from(sb->addr, reach < blen ? reach : blen, source); }', 'rp_post_gpu_write_from(sb->addr, reach < sb->size ? reach : sb->size, source); }')],
     'g_pmem[4 * PG] == 0x5a'),
    ('统一缓冲当存储（UBOP）也走空缓冲', [('if (sb && e->src_kind == NVN_RB_SBUFP && sb_unmapped(sb)) {', 'if (sb && sb_unmapped(sb)) {')],
     'nvn_unimpl_count("draw", "storage_buffer_unbound") == unbound + 1'),
    ('空缓冲的长度不是 64 KiB', [('o->u.b.off = 0; o->u.b.len = 0x10000; break;', 'o->u.b.off = 0; o->u.b.len = 0x100; break;')], 'g_fb[0][0].u.b.len == 0x10000'),
]
others = ['graphics/nvn/tests/t_nvn_replay.c', 'graphics/nvn/tests/nvn_clear_view_unused.c',
          *('graphics/nvn/replay/' + x for x in ('rp_exec.c', 'rp_copy_t2b.c', 'rp_clear_tex.c', 'rp_draw_tex.c', 'rp_state.c', 'rp_entry.c', 'rp_res.c', 'rp_buf.c', 'rp_pass.c', 'rp_present.c', 'nvn_unimpl.c')),
          'graphics/nvn/gfx/nvn/nvn_tex.c', 'graphics/nvn/gfx/tex/nvn_deswizzle.c']
flags = ['-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-std=c11', '-D_DEFAULT_SOURCE', '-DS2A_TEST_TS_ALLOC', '-DS2A_GPU_LAND_TEST', '-DS2A_TEST_TREG_SCRUB',
         *('-I' + str(ROOT / x) for x in ('.', 'graphics/nvn/gfx/nvn', 'graphics/nvn/gfx/nvn/include', 'graphics/nvn/gfx/tex', 'graphics/nvn/replay'))]
link = subprocess.run([sys.executable, str(ROOT / 'tools/weak_link_decl.py'), 'flags', 'replay'], check=True, capture_output=True, text=True).stdout.split() if sys.platform == 'darwin' else []
objs = []


def run(name, code):
    d = OUT / name
    shutil.rmtree(d, ignore_errors=True); d.mkdir(parents=True)
    src = d / 'rp_draw.c'; src.write_text(code, encoding='utf-8')
    o = d / 'rp_draw.o'
    subprocess.run([CC, *flags, *(['-Werror'] if name == 'positive' else ['-w']), '-c', str(src), '-o', str(o)], check=True, cwd=ROOT)
    exe = d / 't'
    subprocess.run([CC, *objs, str(o), *link, '-lm', '-o', str(exe)], check=True, cwd=ROOT)
    return subprocess.run([str(exe), str(ROOT / 'docs/直连_Vulkan回放接线表.tsv')], capture_output=True, text=True, cwd=ROOT, timeout=180)


def main():
    for x in others:
        o = OUT / ('obj_%s.o' % Path(x).stem)
        subprocess.run([CC, *flags, '-Werror', '-c', str(ROOT / x), '-o', str(o)], check=True, cwd=ROOT)
        objs.append(str(o))
    base = (ROOT / DRAW).read_text(encoding='utf-8')
    pos = run('positive', base)
    if pos.returncode:
        raise SystemExit('正对照没过\n' + pos.stdout[-2000:] + pos.stderr[-1000:])
    print('正对照：', [l for l in pos.stdout.splitlines() if '项检查' in l][-1:], flush=True)
    for i, (name, edits, want) in enumerate(mutations):
        code = base
        for old, new in edits:
            assert code.count(old) == 1, (name, '变异锚点', old[:60])
            code = code.replace(old, new)
        r = run('m%d' % i, code)
        out = r.stdout + r.stderr
        if r.returncode == 0:
            raise SystemExit(name + '：变异体居然通过了')
        if want not in out:
            reds = [l for l in out.splitlines() if '【红】' in l]
            raise SystemExit('%s：红在别处，期望含 %r\n%s' % (name, want, '\n'.join(reds[:6])))
        print('  %s：已拦住（红在「%s」）' % (name, want), flush=True)
    print("PASS: 存储缓冲槽长度语义共 %d 个变异体全部被单测拦住" % len(mutations))


main()
