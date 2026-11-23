#!/usr/bin/env python3
"""ReadTexels 读 GPU 内容（rp_read_texels.inc / rp_exec.c 旁路段 / rp_entry.c 分支）的点变异：每个变异体在 build/ 下的副本里重编 t_nvn_replay，
对应的用例（t_read_texels_gpu / t_read_texels_gpu_owned）必须红在指定的断言上；源码树不动。

  rp_read_texels.inc 被 rp_entry.c 按引号收进来：变异时把 rp_entry.c 与变异过的 .inc 一起拷进副本目录，副本里的 rp_entry.c 先找到同目录的 .inc。
  「去掉 GPU 分支」那一条就是修前（main 上内容归 GPU 的读回只记 read_texels、不动 dst）：t_read_texels_gpu_owned 必须红。

放不进来的：后端的 READBACK 判断与 copy_tex_buf 的拒收（真后端，由 android/tests/vk/run_replay_read_texels.sh 的格式用例与核对脚本里的后端变异覆盖）。

用法：python3 read_texels_gpu_mutation_test.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/read-texels-mutations'
OUT.mkdir(parents=True, exist_ok=True)
CC = os.environ.get('CC', 'cc')
INC = 'graphics/nvn/replay/rp_read_texels.inc'
ENTRY = 'graphics/nvn/replay/rp_entry.c'
EXEC = 'graphics/nvn/replay/rp_exec.c'

# (名字, 改哪份, [(旧, 新) …], 期望红行里出现的断言文字（元组 = 任一条）)
mutations = [
    ('映射读计入目标行距', INC, [('buf, src, slice * nz,', 'buf, src, row * hb * nz,')],
     'obs_bytes[NVN_IDLE_READ_TEXELS] == 128 + 24'),
    ('不等之前交的活', INC, [('if (prior && !nvn_gpu_ok(rp_fence_wait_at(prior, 5000000000ull, &err, NVN_GPU_WAIT_READ_PRIOR)))', 'if (0 && prior && !nvn_gpu_ok(rp_fence_wait_at(prior, 5000000000ull, &err, NVN_GPU_WAIT_READ_PRIOR)))')],
     'un_cnt("read_texels_gpu_wait") == 1'),
    ('不拿提交锁', INC, [('    nvn_hle_submit_lock();\n    const uint64_t t0 = rt_now();', '    const uint64_t t0 = rt_now();')], 'g_rt_unlocks == g_rt_locks'),
    ('按紧排写 dst（不看行距）', INC, [('memcpy(dst + (size_t)z * img + (size_t)y * row, src +', 'memcpy(dst + (size_t)z * img + (size_t)y * (size_t)wb, src +')],
     'rt_rows_are(dst, 16, 0, &sub, 12, 2)'),
    ('不按片距写 dst', INC, [('memcpy(dst + (size_t)z * img + (size_t)y * row, src +', 'memcpy(dst + (size_t)z * (size_t)ss + (size_t)y * row, src +')],
     'rt_rows_are(dst + 80, 16, 2, &two, 16, 4)'),
    ('格式不查 READBACK', INC, [('if (!(nvn_gpu_format_caps((uint32_t)t->d.format) & NVN_GPU_FMT_READBACK))', 'if (0)')], 'un_cnt("read_texels_gpu_format") == 1'),
    ('块对齐不查', INC, [('if (r->xoffset % bw || r->yoffset % bh ||', 'if (0 && (r->xoffset % bw || r->yoffset % bh) &&')], 'un_cnt("read_texels_gpu_unaligned") == 1'),
    ('每层都拷起始层', INC, [('nvn_gpu_copy_tex_buf(c, g, level, first + z, &rr,', 'nvn_gpu_copy_tex_buf(c, g, level, first, &rr,')], 'g_tb[1].layer == 2'),
    ('读回段没等完就拷', INC, [('const int ok = f && nvn_gpu_ok(rp_fence_wait_at(f, 5000000000ull, &err, NVN_GPU_WAIT_READ_COPY));', 'const int ok = f != 0;')],
     'un_cnt("read_texels_gpu_wait") == 2'),
    ('旁路段不推进栅栏', EXEC, [('    g_fence = f;   // 先推进', '    (void)0;   // 变异：不推进')], ('g_last_signal == f0 + 128', 'nvn_be_last_fence() == f0 + 64')),
    ('提交里也开旁路段', EXEC, [('if (g_in_submit || g_sub || g_cmd) {', 'if (g_in_submit) {')], 'un_cnt("side_segment_inside_submit") == 1'),
    ('修前：内容归 GPU 不读（main）', ENTRY, [('        if (nvn_be_tex_gpu_owned(t->serial)) rt_gpu(', '        if (0) rt_gpu(')], '!memcmp(dst, gpu, 64)'),
]

others = ['graphics/nvn/tests/t_nvn_replay.c', 'graphics/nvn/tests/nvn_clear_view_unused.c',
          *('graphics/nvn/replay/' + x for x in ('rp_copy_t2b.c', 'rp_clear_tex.c', 'rp_draw_tex.c', 'rp_state.c', 'rp_res.c', 'rp_buf.c', 'rp_pass.c', 'rp_present.c', 'rp_draw.c', 'nvn_unimpl.c')),
          'graphics/nvn/gfx/nvn/nvn_tex.c', 'graphics/nvn/gfx/tex/nvn_deswizzle.c']
flags = ['-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-std=c11', '-D_DEFAULT_SOURCE', '-DS2A_TEST_TS_ALLOC', '-DS2A_GPU_LAND_TEST', '-DS2A_TEST_TREG_SCRUB',
         *('-I' + str(ROOT / x) for x in ('.', 'graphics/nvn/gfx/nvn', 'graphics/nvn/gfx/nvn/include', 'graphics/nvn/gfx/tex', 'graphics/nvn/replay'))]
link = subprocess.run([sys.executable, str(ROOT / 'tools/weak_link_decl.py'), 'flags', 'replay'], check=True, capture_output=True, text=True).stdout.split() if sys.platform == 'darwin' else []
objs = []


def build_others():
    for x in others:   # 不变的部分只编一次
        o = OUT / ('obj_%s.o' % Path(x).stem)
        subprocess.run([CC, *flags, '-Werror', '-c', str(ROOT / x), '-o', str(o)], check=True, cwd=ROOT)
        objs.append(str(o))


def run(name, srcs):
    d = OUT / name
    shutil.rmtree(d, ignore_errors=True); d.mkdir(parents=True)
    for rel, text in srcs.items(): (d / Path(rel).name).write_text(text, encoding='utf-8')
    o_entry, o_exec = d / 'rp_entry.o', d / 'rp_exec.o'
    extra = [] if name == 'positive' else ['-w']   # 变异副本难免带「没用到」类告警，只有正对照的告警有意义
    for src, o in ((d / 'rp_entry.c', o_entry), (d / 'rp_exec.c', o_exec)):
        subprocess.run([CC, *flags, *(['-Werror'] if name == 'positive' else extra), '-c', str(src), '-o', str(o)], check=True, cwd=ROOT)
    exe = d / 't'
    subprocess.run([CC, *objs, str(o_entry), str(o_exec), *link, '-lm', '-o', str(exe)], check=True, cwd=ROOT)
    return subprocess.run([str(exe), str(ROOT / 'docs/直连_Vulkan回放接线表.tsv')], capture_output=True, text=True, cwd=ROOT, timeout=180)


def main():
    base = {p: (ROOT / p).read_text(encoding='utf-8') for p in (INC, ENTRY, EXEC)}
    build_others()
    pos = run('positive', base)
    if pos.returncode:
        raise SystemExit('ReadTexels GPU：正对照没过\n' + pos.stdout[-2000:] + pos.stderr[-1000:])
    print('正对照：', [l for l in pos.stdout.splitlines() if '项检查' in l][-1:], flush=True)
    for i, (name, which, edits, want) in enumerate(mutations):
        srcs = dict(base)
        for old, new in edits:
            assert srcs[which].count(old) == 1, (name, '变异锚点', old[:60])
            srcs[which] = srcs[which].replace(old, new)
        r = run('m%d' % i, srcs)
        out = r.stdout + r.stderr
        wants = (want,) if isinstance(want, str) else want
        seen = next((w for w in wants if w in out), None)
        if r.returncode == 0:
            raise SystemExit(name + '：变异体居然通过了（用例没拦住）')
        if seen is None:
            reds = [l for l in out.splitlines() if '【红】' in l]
            raise SystemExit('%s：红在别处（退出码 %d），期望断言含 %s\n%s' % (name, r.returncode, ' 或 '.join(repr(w) for w in wants), '\n'.join(reds[:6])))
        print('  %s：已拦住（红在「%s」）' % (name, seen), flush=True)
    print('PASS: ReadTexels 读 GPU 内容共 %d 个变异体全部被单测拦住' % len(mutations))


main()
