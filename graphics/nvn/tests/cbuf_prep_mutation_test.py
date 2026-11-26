#!/usr/bin/env python3
"""图形阶段常量缓冲「GPU 待写回」必须在开通道前解析（rp_draw.c 准备阶段前置，#2100）的点变异：
每个变异体改坏一处，t_nvn_replay 的 t_graphics_cbuf_prep_before_pass 必须红在指定断言上。
每个变异体在 build/ 下的副本里重编 rp_draw.c 再链接，源码树不动。

用法：python3 cbuf_prep_mutation_test.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/cbuf-prep-mutations'
OUT.mkdir(parents=True, exist_ok=True)
CC = os.environ.get('CC', 'cc')
DRAW = 'graphics/nvn/replay/rp_draw.c'

PREP = 'if (rp_buf_resolve_binding(c, ub->addr, len, &mb, &mo, RP_BUF_UNIFORM)) { g_ub_buf[st][i] = mb; g_ub_off[st][i] = mo; g_ub_ok[st][i] = 1; }'
GUARD = 'else if (st >= RP_PROG_STAGES) {                                   // 计算：本来就在通道外，现场解析安全'
HOLD = '''                    if (!ub->addr || !ub->size) continue;
                    const nvn_gpu_caps *cp = caps_cached();
                    if (cp->max_ubo_range && ub->size > cp->max_ubo_range) continue;
                    if (!rp_buf_gpu_pending(ub->addr, ub->size)) continue;   // 没有 GPU 待写回：照旧暂存，不需要镜像'''

mutations = [
    ('撤掉准备阶段的预解析（回到修前：发绑定时才发现待写回）', [(PREP, 'if (0 && rp_buf_resolve_binding(c, ub->addr, len, &mb, &mo, RP_BUF_UNIFORM)) { g_ub_buf[st][i] = mb; g_ub_off[st][i] = mo; g_ub_ok[st][i] = 1; }')],
     'uniform_gpu_pending_unprepared") == 0'),   # 撤掉预解析后：主场景这一笔走「不解析 + 记账」兜底
    ('准备阶段解析不出来时又允许在通道内现场解析（去掉「只许计算阶段」的限定）',
     [(PREP, 'if (0 && rp_buf_resolve_binding(c, ub->addr, len, &mb, &mo, RP_BUF_UNIFORM)) { g_ub_buf[st][i] = mb; g_ub_off[st][i] = mo; g_ub_ok[st][i] = 1; }'),
      (GUARD, 'else if (1) {')],
     'uniform_gpu_pending_unprepared") == 1'),   # 去掉限定后：兜底场景会在通道内解析，记账没了
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
    print('PASS: 图形阶段常量缓冲准备前置共 %d 个变异体全部被单测拦住' % len(mutations))


main()
