#!/usr/bin/env python3
"""GS replay point mutations; each source copy is built in this worktree."""
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/gs-replay-mutations'
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT/'graphics/nvn/replay/rp_draw.c').read_text()
mutations = [
    ('gs-dirty', 'int args_dirty = rp_args_dirty[0] || rp_args_dirty[1] || rp_args_dirty[2];',
     'int args_dirty = rp_args_dirty[0] || rp_args_dirty[1];', 'g_resolves == 2'),
    ('gs-fs-bind', 'st == 5 ? NVN_GPU_SH_CS : (nvn_gpu_stage)st',
     'st == 5 ? NVN_GPU_SH_CS : st == 2 ? NVN_GPU_SH_FS : (nvn_gpu_stage)st', 'g_bind_calls[NVN_GPU_SH_GS] == 2'),
    ('gs-pipeline', 'for (int st = 0; st < RP_PROG_STAGES; st++) pd.st[st].sh = g_prog->sh[st];',
     'for (int st = 0; st < 2; st++) pd.st[st].sh = g_prog->sh[st];', 'g_last_pdesc.st[NVN_GPU_SH_GS].sh'),
    ('gs-bind-omitted', 'for (int st = 0; st < RP_PROG_STAGES; st++) if (!bind_stage',
     'for (int st = 0; st < 2; st++) if (!bind_stage', 'g_bind_calls[NVN_GPU_SH_GS] == 2'),
    ('gs-topology', 'if ((rp_shader_graphics_stages() & 4u) && prim != 0 && prim != 1 && prim != 4)',
     'if (0 && (rp_shader_graphics_stages() & 4u) && prim != 0 && prim != 1 && prim != 4)', 'geometry_topology_unimplemented'),
]
sources = ['graphics/nvn/tests/t_nvn_replay.c','graphics/nvn/tests/nvn_clear_view_unused.c',
           *('graphics/nvn/replay/'+x for x in ('rp_exec.c','rp_copy_t2b.c','rp_clear_tex.c','rp_state.c','rp_entry.c','rp_res.c','rp_buf.c','rp_pass.c','rp_present.c','nvn_unimpl.c')),
           'graphics/nvn/gfx/nvn/nvn_tex.c','graphics/nvn/gfx/tex/nvn_deswizzle.c']
base = [os.environ.get('CC','cc'),'-O2','-g','-Wall','-Wextra','-Wno-unused-parameter','-std=c11','-D_DEFAULT_SOURCE','-Werror',
        *('-I'+str(ROOT/x) for x in ('.','graphics/nvn/gfx/nvn','graphics/nvn/gfx/nvn/include','graphics/nvn/gfx/tex','graphics/nvn/replay'))]
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
print('PASS: GS replay 5 independent behavior controls')
