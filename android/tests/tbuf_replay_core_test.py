#!/usr/bin/env python3
"""Exercise production replay with buffer textures using the host fixture."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = root/'build/tbuf-replay-core'
out.mkdir(parents=True, exist_ok=True)
sources = ['graphics/nvn/tests/t_nvn_replay.c', 'graphics/nvn/tests/nvn_clear_view_unused.c',
           *('graphics/nvn/replay/'+x for x in ('rp_exec.c', 'rp_copy_t2b.c', 'rp_clear_tex.c', 'rp_state.c', 'rp_entry.c', 'rp_res.c', 'rp_buf.c', 'rp_pass.c', 'rp_present.c', 'nvn_unimpl.c')),
           'graphics/nvn/gfx/nvn/nvn_tex.c', 'graphics/nvn/gfx/tex/nvn_deswizzle.c']
inc = ('.', 'graphics/nvn/gfx/nvn', 'graphics/nvn/gfx/nvn/include', 'graphics/nvn/gfx/tex', 'graphics/nvn/replay')
def run(name, draw):
    exe = out/name
    args = [os.environ.get('CC', 'cc'), '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-std=c11', '-D_DEFAULT_SOURCE', '-Werror',
            *('-I'+str(root/p) for p in inc), *(str(root/p) for p in sources), str(draw), '-lm', '-o', str(exe)]
    subprocess.run(args, check=True, cwd=root)
    return subprocess.run([str(exe), str(root/'docs/直连_Vulkan回放接线表.tsv')], capture_output=True, text=True, cwd=root)

draw = root/'graphics/nvn/replay/rp_draw.c'
result = run('replay', draw)
sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
if sys.argv[1:] == ['--expect-red']:
    expected = ('g_gdraws == 2 && g_tbuf_binds[NVN_GPU_SH_FS] == 2',
                'g_ndisp == n + 1 && g_tbuf_binds[NVN_GPU_SH_CS] == 1')
    if result.returncode != 1 or not all(s in result.stdout for s in expected):
        raise SystemExit('SETUP FAILED: expected graphics and compute TBUF behavior failures')
    print('EXPECTED RED: real replay skips TBUF graphics and compute bindings')
elif result.returncode:
    raise SystemExit(result.returncode)
elif sys.argv[1:] == ['--withdraw']:
    code = draw.read_text()
    changes = [
        ('resolve', 'if (!rp_buf_resolve(c, (uint64_t)(uintptr_t)t.mem, bytes, &buf, &off)) return skip("tbuf_buffer_unresolved", rec);',
         'return skip("tbuf_buffer_unresolved", rec);', 'g_gdraws == 2 && g_tbuf_binds[NVN_GPU_SH_FS] == 2'),
        ('format', 'uint32_t fmt = hv && v.set_format ? v.format : (uint32_t)t.d.format;',
         'uint32_t fmt = NVN_FORMAT_RGBA8UI;', 'g_tbuf_bound[NVN_GPU_SH_FS].format == NVN_FORMAT_R32UI'),
        ('elements', 'uint64_t bytes = (uint64_t)t.d.width * base.bytes;',
         'uint64_t bytes = (uint64_t)t.d.width * base.bytes * 2;', 'g_tbuf_bound[NVN_GPU_SH_FS].bytes == 16'),
    ]
    # Withdraw every TBUF cache guard together: the unchanged tracked range
    # keeps the pass open and must still bind a fresh view for both draws.
    mutant = code
    for old, new in [
        ('case NVN_RB_TBUF: return 0;  // 临时缓冲视图每笔重新建并绑定', 'case NVN_RB_TBUF: break;'),
        ('g_lb[pi].ok = !has_tbuf;', 'g_lb[pi].ok = 1 + 0 * has_tbuf;'),
        ('const int same = keep && !has_tbuf &&', 'const int same = keep &&'),
    ]:
        if mutant.count(old) != 1:
            raise SystemExit('mutation anchor changed: fast-cache')
        mutant = mutant.replace(old, new)
    path = out/'fast-cache.c'; path.write_text(mutant)
    mutated = run('fast-cache', path)
    expected = 'g_tbuf_binds[NVN_GPU_SH_FS] == old_binds + 2'
    if mutated.returncode != 1 or expected not in mutated.stdout:
        raise SystemExit('fast-cache: wrong failure\n'+mutated.stdout+mutated.stderr)
    print('NEGATIVE CONTROL PASS: fast-cache ->', expected, flush=True)
    for name, old, new, expected in changes:
        if code.count(old) != 1:
            raise SystemExit('mutation anchor changed: '+name)
        path = out/(name+'.c'); path.write_text(code.replace(old, new))
        mutated = run(name, path)
        if mutated.returncode != 1 or expected not in mutated.stdout:
            raise SystemExit(name+': wrong failure\n'+mutated.stdout+mutated.stderr)
        print('NEGATIVE CONTROL PASS:', name, '->', expected, flush=True)
elif sys.argv[1:]:
    raise SystemExit('usage: tbuf_replay_core_test.py [--expect-red|--withdraw]')
