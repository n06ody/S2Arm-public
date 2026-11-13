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
        raise RuntimeError('requires pinned NDK 29.0.14206865 headers (no download or skip): set ANDROID_NDK_HOME to it, '
                           'or install it at $ANDROID_HOME/ndk/29.0.14206865 or ~/Android/Sdk/ndk/29.0.14206865')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    sysinc = ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include'
    if not (sysinc/'vulkan/vulkan.h').is_file():
        raise RuntimeError('NDK Vulkan headers missing')
    include = out/'include'
    include.mkdir(exist_ok=True)
    for name in ('vulkan', 'vk_video'):
        if (sysinc/name).exists() and not (include/name).exists():
            (include/name).symlink_to(sysinc/name)
    inc = (include, ROOT/'runtime/include', native, native/'nvn', native/'direct', gfx, gfx/'replay', gfx/'gfx/nvn',
           gfx/'gfx/nvn/include', ROOT/'android/tests/vkhost')
    libs = [gfx/'replay/nvn_unimpl.c', gfx/'replay/rp_state.c', native/'nvn/nvn_vk_compute_request.c']
    libs += [ROOT/'runtime/src/direct_host.c']
    libs += [native/'direct'/f for f in ('source_store.c', 'control.c', 'input_identity.c',
                                        'texture_facts.c', 'sha256.c', 'json.c', 'compile_request.c')]
    strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    base = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections', '-pthread']
    # glibc 在 -std=c11 下不暴露 POSIX.1-2008 声明（O_CLOEXEC / openat / fstatat / strdup 等），
    # host_link 自动补链的 offline_library.c、program_cache.c 要用。宏只加在测试侧、只加给非 macOS：
    # 这两个文件还要给 NDK 和苹果宿主编，在源文件里定义会改变 macOS 上的可见声明；macOS 的参数与原来逐字相同。
    if sys.platform != 'darwin':
        base.append('-D_POSIX_C_SOURCE=200809L')
    base += ['-I'+str(p) for p in inc]
    original = native/'gfx_plat/nvk_shader.c'

    def run(name, source, extra_libs=None, args=()):
        exe = out/name
        src = [ROOT/'android/tests/nvk_shader_pool_test.c', source, *(extra_libs if extra_libs is not None else libs)]
        host_link.run(base + [str(p) for p in src] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        return subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=30)

    if len(sys.argv) in (3, 4) and sys.argv[1] == '--tbuf-layout':
        exe = out/'tbuf-layout'
        src = [ROOT/'android/tests/tbuf_layout_test.c', *libs]
        host_link.run(base + [str(p) for p in src] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        result = subprocess.run([str(exe), *sys.argv[2:]], capture_output=True, text=True, timeout=30)
        sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
        return result.returncode

    if sys.argv[1:] == ['--legacy-compare']:
        # Compare actual unchanged legacy REQUEST and S2AI1 bytes against merge-base.
        base_sha = subprocess.check_output(['git', 'merge-base', 'HEAD', 'origin/main'], cwd=ROOT, text=True).strip()
        paths = [original, native/'direct/input_identity.c', native/'direct/compile_request.c', native/'nvn/nvn_vk_compute_request.c']
        replacements = {}
        for path in paths:
            relative = path.relative_to(ROOT).as_posix()
            old = subprocess.check_output(['git', 'show', base_sha + ':' + relative], cwd=ROOT, text=True)
            dest = out/('legacy-' + path.name)
            dest.write_text(old)
            replacements[path] = dest
        baseline_libs = [replacements.get(path, path) for path in libs]
        before = run('legacy-before', replacements[original], baseline_libs, ('--legacy-bytes',))
        after = run('legacy-after', original, args=('--legacy-bytes',))
        assert before.returncode == after.returncode == 0, (before.stderr, after.stderr)
        before_lines, after_lines = before.stdout.splitlines(), after.stdout.splitlines()
        assert len(before_lines) == len(after_lines) == 2
        assert before_lines[0] == after_lines[0], 'legacy REQUEST bytes changed'
        assert before_lines[1] == after_lines[1], 'legacy S2AI1 bytes changed'
        print(f'legacy request and canonical identity byte-for-byte unchanged versus {base_sha}')
        return 0
    if len(sys.argv) == 6 and sys.argv[1] in ('--emit-real', '--emit-real-tbuf', '--execute-tbuf', '--emit-compute-tbuf', '--execute-compute-tbuf'):
        result = run('emit-real', original, args=sys.argv[1:])
        sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
        return result.returncode

    if len(sys.argv) == 7 and sys.argv[1] == '--tbuf-withdraw':
        profile, closure, src_dir, gfx_result, cs_result = sys.argv[2:]
        code = original.read_text(encoding='utf-8')
        layout_anchor = 'if (b->kind == 2 && b->texel_buffer && b->count == 1 && b->tex_slot >= 0)'
        assert code.count(layout_anchor) == 1
        mutated = out/'tbuf-withdraw-layout.c'
        mutated.write_text(code.replace(layout_anchor, layout_anchor.replace('&& b->texel_buffer', '&& 0 && b->texel_buffer')))
        fixture = out/'tbuf-withdraw-layout-test.c'
        fixture.write_text((ROOT/'android/tests/tbuf_layout_test.c').read_text().replace(
            '#include "../native/gfx_plat/nvk_shader.c"', '#include "' + str(mutated) + '"'))
        exe = out/'tbuf-withdraw-layout-test'
        host_link.run(base + [str(p) for p in [fixture, *libs]] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        for path, extra in [(gfx_result, ()), (cs_result, ('--compute',))]:
            result = subprocess.run([str(exe), path, *extra], capture_output=True, text=True, timeout=30)
            assert result.returncode == 1 and 'FAIL: BUFFER layout must be TBUF' in result.stderr, result.stderr
        print('NEGATIVE CONTROL PASS: removing TBUF layout fails graphics and compute assertions')
        guard = 'if (le->cls == NVN_GPU_RES_TBUF) { red1("texture_buffer_replay_unsupported",'
        assert code.count(guard) == 2
        mutated = out/'tbuf-withdraw-replay.c'
        mutated.write_text(code.replace(guard, guard.replace('le->cls ==', '0 && le->cls ==')))
        for mode, path in [('--execute-tbuf', gfx_result), ('--execute-compute-tbuf', cs_result)]:
            result = run('tbuf-withdraw-replay', mutated, args=(mode, profile, closure, src_dir, path))
            assert result.returncode == 1 and 'FAIL: real' in result.stderr and 'refuses unsupported replay' in result.stderr, result.stderr
        print('NEGATIVE CONTROL PASS: removing TBUF replay guards fails graphics and compute assertions')
        return 0

    if sys.argv[1:] == ['--withdraw']:
        text = original.read_text(encoding='utf-8')
        changes = [
            ('gs-source-epoch', 'MIX(s2a_source_epoch());', '(void)s2a_source_epoch();', 1, 'poisoned GS source cannot hit fast cache'),
            ('gs-slot', '(const char *const[]){"vs", "fs", "gs"}[st]',
             '(const char *const[]){"vs", "fs", "fs"}[st]', 3, 'GS production request reaches compiler'),
            ('gs-wrong-stage', 'src.stage = (nvn_gpu_stage)st;',
             'src.stage = st == 2 ? NVN_GPU_SH_FS : (nvn_gpu_stage)st;', 1, 'result loads VS FS and GS'),
            ('gs-key-omitted', '((stages & 4u) ? 3 : 2)',
             '((stages & 4u) ? 2 : 2)', 1, 'changed GS key recompiles'),
            ('gs-texture-omitted', 'if (stages & 4u) MIX(rp_tex_fact_sig(rp_S.tex[2], 32, rp_S.tex_base, rp_S.tex_pool_n));',
             'if (0) MIX(rp_tex_fact_sig(rp_S.tex[2], 32, rp_S.tex_base, rp_S.tex_pool_n));', 1, 'changed GS texture fact recompiles'),
            ('gs-separate-fast-omitted', 'if (stages & 4u) MIX(sep_fact_sig(2));',
             'if (0) MIX(sep_fact_sig(2));', 1, 'GS separate texture fact reaches compiler'),
            ('gs-separate-generation-omitted', 'const uint64_t sg = rp_tex_sep_gen[st], rg = rp_tex_reg_gen();',
             'const uint64_t sg = st == 2 ? 0 : rp_tex_sep_gen[st], rg = rp_tex_reg_gen();', 1, 'changed GS separate slot invalidates cached signature and recompiles'),
            ('gs-separate-stage-wrong', '(const char *const[]){"vs", "fs", "gs"}[st], (uint32_t)(90',
             '(const char *const[]){"vs", "fs", "fs"}[st], (uint32_t)(90', 1, 'GS separate texture fact reaches compiler'),
            ('gs-cap-ignored', 'if (!(caps.caps & NVN_GPU_CAP_GEOMETRY))',
             'if (0 && !(caps.caps & NVN_GPU_CAP_GEOMETRY))', 1, 'disabled GS fails before compiler'),
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
            ('wrong-array-red', 'nvn_unimpl("shader", "texture_pool_array_unsupported", NULL);',
             'nvn_unimpl("shader", "texture_pool_array_wrong", NULL);', 1,
             'reports the dedicated unimplemented'),
            ('drop-final-summary', 's2a_direct_summary_register(pool_final_summary);',
             '(void)pool_final_summary;', 1, 'normal process-exit callback emits final stats'),
            ('timeout-is-not-final', 'if (normal && (g_pool_programs || g_pool_request_rejected)) pool_stats("final");',
             'if (g_pool_programs || g_pool_request_rejected) pool_stats("final");', 1, 'timeout summary is never labelled final'),
            ('flatten-missing-fact', 'else if (!strcmp(code, "MISSING_FACT")) {',
             'else if (0 && !strcmp(code, "MISSING_FACT")) {', 1, 'compiler reason counted accurately'),
            ('drop-request-count', 'g_pool_request_rejected++;',
             '(void)g_pool_request_rejected;', 1, 'request failure is counted separately'),
            ('drop-separate-vs', 'for (int st = 0; st < nst; st++) for (int slot = 0; slot < 128; slot++) {',
             'for (int st = 0; st < nst; st++) for (int slot = 0; slot < 0; slot++) {', 1, 'separate VS registered view fact is sent'),
            ('drop-separate-cs', 'for (int slot = 0; slot < 128; slot++) {\n        uint64_t h = rp_S.tex[5][128 + slot];',
             'for (int slot = 0; slot < 0; slot++) {\n        uint64_t h = rp_S.tex[5][128 + slot];', 1, 'separate CS registered texture fact is sent'),
            ('drop-separate-fast-identity', 'MIX(sep_fact_sig(0));',
             '(void)rp_S.tex[0][128];', 1, 'separate texture rebind invalidates fast identity'),
        ]
        for name, before, after, count, expected in changes:
            assert text.count(before) == count, (name, 'mutation anchor')
            path = out/(name+'.c')
            path.write_text(text.replace(before, after), encoding='utf-8')
            result = run(name, path)
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print(f'PASS: {len(changes)} negative controls')
        return 0
    if sys.argv[1:]:
        raise ValueError('usage: nvk_shader_pool_test.py [--withdraw]')
    result = run('test', original)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    return result.returncode

if __name__ == '__main__':
    raise SystemExit(main())
