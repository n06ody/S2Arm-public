#!/usr/bin/env python3
"""落地期间显卡写区间里的页不进同步（rp_buf.c sync_run 拆段 / gw_covers / land_restale）的点变异：每条撤掉一处判据，t_nvn_replay 的 t_land_noresync 必须红在对应断言上；
另编一份 main 上的 rp_buf.c（修前）必须红在「落地期间被盖住的页照样同步」。"""
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/land-noresync-mutations'
OUT.mkdir(parents=True, exist_ok=True)
TARGET = 'graphics/nvn/replay/rp_buf.c'
source = (ROOT/TARGET).read_text()
mutations = [
    # 不按页拆：首尾半页时整串照样同步
    ('gw-no-split', 'while (k <= j && !gw_covers_page(s, mem, size, k)) k++;', 'k = j + 1;', 'moved == PG && ok'),
    # 区间拼接判错：只看起点所在那一段，盖不住跨两段的串
    ('gw-covers-start-only', 'if (g0 <= a && a < g1) { a = g1; moved = 1; } }', 'if (g0 <= a && a < g1) { a = g1 >= b ? g1 : a; moved = 0; } }', 'split: moved=4096'),
    # 跨字合并布防不看显卡写区间：被盖住的页也一起布防
    ('merge-rearm-covered', 'if (j > word_end && !gw_covers_any_page(s, mem, size, i, j)) {', 'if (j > word_end && (gw_covers_any_page(s, mem, size, i, j) || 1)) {', 'g_prot_len == 4 * PG'),
    # 先布防再拆：被盖住的页也布防
    ('rearm-before-split', '        if (s->ngw) {\n            // 盖住的页不搬', '        if (s->ngw && (st = st ? st : rearm_span(sync_span_for(s, mem, size, i, j)))) {\n            // 盖住的页不搬', 'g_gw_case_prot == 0'),
    # 拆出被盖住的页时不清旧位：登记存活期间每次解析都复查（8E b4r2 周期性大卡）
    ('gw-stale-kept', 'if (m->path == MP_LOG && m->stale && (m->stale[x >> 6] >> (x & 63) & 1)) stale_clear(m, x);', '(void)0;', 'again_all == 0 && again_small == 0'),
    # 撤登记时不重新置旧：落地后这些页永远不再重搬
    ('no-restale', '    if (k1 > k0) stale_set(m, k0, (k1 < s->npages ? k1 : s->npages) - 1);', '    (void)k0; (void)k1;', 'land restale mode 0: moved=0'),
]
sources = ['graphics/nvn/tests/t_nvn_replay.c','graphics/nvn/tests/nvn_clear_view_unused.c',
           *('graphics/nvn/replay/'+x for x in ('rp_exec.c','rp_copy_t2b.c','rp_clear_tex.c','rp_state.c','rp_entry.c','rp_res.c','rp_draw.c','rp_pass.c','rp_present.c','nvn_unimpl.c')),
           'graphics/nvn/gfx/nvn/nvn_tex.c','graphics/nvn/gfx/tex/nvn_deswizzle.c']
base = [os.environ.get('CC','cc'),'-O2','-g','-DS2A_GPU_LAND_TEST','-pthread','-Wall','-Wextra','-Wno-unused-parameter','-std=c11','-D_DEFAULT_SOURCE','-Werror',
        *('-I'+str(ROOT/x) for x in ('.','graphics/nvn/gfx/nvn','graphics/nvn/gfx/nvn/include','graphics/nvn/gfx/tex','graphics/nvn/replay'))]
# 回放核心对平台 / 后端可选件的弱引用：苹果 ld 默认不认未定义弱符号，逐个允许缺；清单只从源码登记生成（tools/weak_link_decl.py flags replay）
import sys
if sys.platform == 'darwin':
    base += subprocess.run([sys.executable, str(ROOT/'tools/weak_link_decl.py'), 'flags', 'replay'], check=True, capture_output=True, text=True).stdout.split()
def run(name, code):
    path = OUT/(name+'.c'); path.write_text(code)
    exe = OUT/name
    subprocess.run(base + [str(ROOT/x) for x in sources] + [str(path),'-lm','-o',str(exe)], check=True, cwd=ROOT)
    return subprocess.run([str(exe), str(ROOT/'docs/直连_Vulkan回放接线表.tsv')], capture_output=True, text=True, cwd=ROOT)
pos = run('positive', source)
if pos.returncode: raise SystemExit('positive failed: ' + pos.stdout + pos.stderr)
print('positive:', pos.stdout.splitlines()[-1], flush=True)
# 修前：main 上的 rp_buf.c（没有落地免重搬）必须红在「整页落地、之后没写：不再搬」
pre = subprocess.run(['git','show','origin/main:'+TARGET], cwd=ROOT, capture_output=True, text=True, check=True).stdout
if 'gw_covers' in pre: raise SystemExit('pre-fix control invalid: origin/main already has gw_covers')
r = run('pre-fix', pre)
if r.returncode == 0 or 'gw pending log path0: moved=8192' not in r.stdout: raise SystemExit('pre-fix: wrong result: ' + r.stdout[-2000:] + r.stderr[-2000:])
print('pre-fix: expected moved=8192 (2 pages in a pending GPU write synced)', flush=True)
for name, old, new, want in mutations:
    if source.count(old) != 1: raise SystemExit(name + ': mutation anchor invalid')
    r = run(name, source.replace(old,new))
    if r.returncode == 0 or want not in r.stdout: raise SystemExit(name + ': wrong failure: ' + r.stdout + r.stderr)
    print(name + ': expected assertion ' + want, flush=True)
print('PASS: 落地期间显卡写区间不进同步 修前红 + %d 个变异体全部被 t_land_noresync 拦住' % len(mutations))
