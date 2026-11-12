#!/usr/bin/env python3
"""Real new-layer shader producer and REQUEST builders; synthetic source/compiler boundary."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/tests'))
import host_link
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols

def main():
    out = ROOT / 'build/nvk-shader-pool-test'
    out.mkdir(parents=True, exist_ok=True)
    # host_link's transient objects must also remain in this worktree.
    tempfile.tempdir = str(out)
    native = ROOT / 'android/native'
    gfx = ROOT / 'graphics/nvn'
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError('requires pinned NDK 29 headers')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    sysinc = ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include'
    if not (sysinc/'vulkan/vulkan.h').is_file():
        raise RuntimeError('NDK Vulkan headers missing')
    include = out/'include'
    include.mkdir(exist_ok=True)
    for name in ('vulkan', 'vk_video'):
        if (sysinc/name).exists() and not (include/name).exists():
            (include/name).symlink_to(sysinc/name)
    inc = (include, native, native/'nvn', native/'direct', gfx, gfx/'replay', gfx/'gfx/nvn',
           gfx/'gfx/nvn/include', ROOT/'android/tests/vkhost')
    libs = [gfx/'replay/nvn_unimpl.c', gfx/'replay/rp_state.c', native/'nvn/nvn_vk_compute_request.c']
    libs += [native/'direct'/f for f in ('source_store.c', 'control.c', 'input_identity.c',
                                        'texture_facts.c', 'sha256.c', 'json.c', 'compile_request.c')]
    strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    base = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections', '-pthread']
    base += ['-I'+str(p) for p in inc]
    original = native/'gfx_plat/nvk_shader.c'

    def run(name, source):
        exe = out/name
        src = [ROOT/'android/tests/nvk_shader_pool_test.c', source, *libs]
        host_link.run(base + [str(p) for p in src] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)

    if sys.argv[1:] == ['--withdraw']:
        text = original.read_text(encoding='utf-8')
        changes = [
            ('withdraw-guard', 'if (!shader_texture_pool_ready()) return -1;',
             'if (0 && !shader_texture_pool_ready()) return -1;', 2, 'expected accounting=texture_pool_over_limit'),
            ('wrong-gfx-length', 'in.texture_array_length = (uint32_t)rp_S.tex_pool_n;',
             'in.texture_array_length = 1;', 1, 'gfx n=7 request retains exact capacity'),
            ('wrong-cs-length', 'st.tex_pool_n = (uint32_t)rp_S.tex_pool_n;',
             'st.tex_pool_n = 1;', 1, 'cs n=7 request retains exact capacity'),
            ('wrong-identity', 'id.texture_array_length = (uint32_t)rp_S.tex_pool_n;',
             'id.texture_array_length = 1;', 1, 'rebind changes request and identity'),
            ('drop-debug-log', 'nvn_plat_flag("nvk_texpool_log", 0)',
             '(0 && nvn_plat_flag("nvk_texpool_log", 0))', 1, 'debug one line records first bound value'),
        ]
        for name, before, after, count, expected in changes:
            assert text.count(before) == count, (name, 'mutation anchor')
            path = out/(name+'.c')
            path.write_text(text.replace(before, after), encoding='utf-8')
            result = run(name, path)
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print('PASS: 5 negative controls')
        return 0
    if sys.argv[1:]:
        raise ValueError('usage: nvk_shader_pool_test.py [--withdraw]')
    result = run('test', original)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    return result.returncode

if __name__ == '__main__':
    raise SystemExit(main())
