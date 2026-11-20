#!/usr/bin/env python3
"""镜像同步分账（rp_buf.c nvk_sync_acct）的点变异：每条撤掉 / 改坏一处计数逻辑，t_nvn_replay 的 t_sync_acct 必须红在对应断言上。"""
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/sync-acct-mutations'
OUT.mkdir(parents=True, exist_ok=True)
TARGET = 'graphics/nvn/replay/rp_buf.c'
source = (ROOT/TARGET).read_text()
mutations = [
    # 落地不记位：落地后重搬算不出来
    ('land-not-noted', 'if (ok && src) acct_note_land(s, off, len);', 'if (0 && ok && src) acct_note_land(s, off, len);', 'RP_PERF_SYNC_LANDED_KB] == 2 * kb'),
    # 同步后不清落地位：之后普通的写也被算成「落地后重搬」
    ('land-bit-kept', 'landed++; s->acct_landed[k >> 6] &= ~(1ull << (k & 63));', 'landed++;', 'pf[RP_PERF_SYNC_LANDED_KB] == 0'),
    # 同提交重搬不看提交号：上一次提交同步过的页也算重搬
    ('resub-any-sub', 'if (m->synced[k] && m->sub[k] == cur) resub++;', 'if (m->synced[k] && (m->sub[k] == cur || cur)) resub++;', 'RP_PERF_SYNC_RESUB_KB] == 0'),
    # 原因判错：上次提交没做完记成「本提交已用过」
    ('fence-as-sub', 'sub_hit ? RP_PERF_SYNC_ORD_SUB_KB : fence_open ? RP_PERF_SYNC_ORD_FENCE_KB',
     '(sub_hit || fence_open) ? RP_PERF_SYNC_ORD_SUB_KB : 0 ? RP_PERF_SYNC_ORD_FENCE_KB', 'RP_PERF_SYNC_ORD_FENCE_KB] == kb'),
    # 抽样摘要比较坏掉：原样重搬永远看不出来
    ('sample-never-same', 'if (src && s->acct_hash[x] == h) {', 'if (src && s->acct_hash[x] == h + 1) {', 'RP_PERF_SYNC_SAMP_SAME_N] == 1'),
    # 整页落地的摘要记成「来自同步」：落地那一类抽样数不出来
    ('land-src-as-sync', 's->acct_src[k / ACCT_SAMPLE] = 2;', 's->acct_src[k / ACCT_SAMPLE] = 1;', 'RP_PERF_SYNC_SAMP_LAND_N] == 1'),
]
sources = ['graphics/nvn/tests/t_nvn_replay.c','graphics/nvn/tests/nvn_clear_view_unused.c',
           *('graphics/nvn/replay/'+x for x in ('rp_exec.c','rp_copy_t2b.c','rp_clear_tex.c','rp_state.c','rp_entry.c','rp_res.c','rp_draw.c','rp_pass.c','rp_present.c','nvn_unimpl.c')),
           'graphics/nvn/gfx/nvn/nvn_tex.c','graphics/nvn/gfx/tex/nvn_deswizzle.c']
base = [os.environ.get('CC','cc'),'-O2','-g','-Wall','-Wextra','-Wno-unused-parameter','-std=c11','-D_DEFAULT_SOURCE','-Werror',
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
for name, old, new, want in mutations:
    if source.count(old) != 1: raise SystemExit(name + ': mutation anchor invalid')
    r = run(name, source.replace(old,new))
    if r.returncode == 0 or want not in r.stdout: raise SystemExit(name + ': wrong failure: ' + r.stdout + r.stderr)
    print(name + ': expected assertion ' + want, flush=True)
print('PASS: 镜像同步分账 %d 个变异体全部被 t_sync_acct 拦住' % len(mutations))
