#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/nvk_shutdown_test.py 在进程内按表逐个跑（含 --withdraw-* 撤回对照）
"""Run actual nvk shader producer with actual provider Stop/context bodies.

Only scheduling is intercepted: provider context return or first compile entry.
The fake close returns retirement failure, matching an outstanding compiler.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android'))
import direct_framelog
sys.path.insert(0, str(ROOT / 'android/tests'))
import host_link
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols


def function(text, signature):
    start = text.index(signature); begin = text.index('{', start); depth = 1; end = begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]


def main():
    out = ROOT / 'build/860-compile-stop'; out.mkdir(parents=True, exist_ok=True); tempfile.tempdir = str(out)
    native = ROOT / 'android/native'; gfx = ROOT / 'graphics/nvn'
    ndk = link_symbols.find_ndk()
    if ndk is None: raise RuntimeError('NDK 29 headers required')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    sysinc = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include'
    include = out / 'include'; include.mkdir(exist_ok=True)
    for name in ('vulkan', 'vk_video'):
        if (sysinc/name).exists() and not (include/name).exists(): (include/name).symlink_to(sysinc/name)
    provider = (native/'gfx_plat/provider_nvk.c').read_text()
    context_struct = function(provider, 'struct provider_context {') + ';'
    context_fn = function(provider, 'int s2a_direct_provider_compile_context(').replace('s2a_direct_provider_compile_context(', 'real_provider_compile_context(', 1)
    stop_fn = function(provider, 'static int32_t request_stop_impl(')
    activate_fn = function(provider, 'static int32_t activate(void *context)')
    if sys.argv[1:] == ['--withdraw-activation']:
        old = 'if (ctx->activation_started) {'; assert stop_fn.count(old) == 1
        stop_fn = stop_fn.replace(old, 'if (0 && ctx->activation_started) {')
    elif sys.argv[1:] == ['--withdraw-frame-dump']:
        hook = 'nvn_vk_frame_log_dump_retained();'
        assert stop_fn.count(hook) == 1
        stop_fn = stop_fn.replace(hook, '(void)0;')
    header = '''#include <pthread.h>\n#include <stdatomic.h>\n#include <stdarg.h>\n#include "direct/vk_device.h"\n#include "nvn/nvn_vk_frame_log.h"\n'''+context_struct+'''
static struct provider_context *g_active_context, *g_retained_context;
static pthread_mutex_t g_window_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_stop_published, g_session_active;
#define TAG "test"
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
static int __android_log_print(int level, const char *tag, const char *fmt, ...) { (void)level; (void)tag; (void)fmt; return 0; }
void nvk_shader_stop(void);
static void nvk_session_request_stop(void) { nvk_shader_stop(); }
static void ctl_wake_stop(void) {}
static void ctl_wake(void) {}
static int stop_window_lock(void) { return pthread_mutex_lock(&g_window_mu); }
static int nvk_session_wait_compiles(void) { return 0; } // timing exercised separately
#define LAND_TEST_LINKED_SHADER
#include "nvk_land_auto_fixture.h"
static void nvk_session_flush_pipe_cache(void) {}
static void set_window_error(struct provider_context *c, const char *s) { snprintf(c->window_error, sizeof c->window_error, "%s", s); }
static pthread_mutex_t stop_test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_test_cv = PTHREAD_COND_INITIALIZER;
static int stop_test_mode, stop_test_entered, stop_test_release;
static void stop_test_pause(int point) {
    if (stop_test_mode != point) return;
    pthread_mutex_lock(&stop_test_mu); stop_test_entered = 1; pthread_cond_broadcast(&stop_test_cv);
    while (!stop_test_release) pthread_cond_wait(&stop_test_cv, &stop_test_mu);
    pthread_mutex_unlock(&stop_test_mu);
}
static int32_t activate_impl(void *context) {
    (void)context; stop_test_pause(3); return S2A_DIRECT_RES_OK;
}
'''+activate_fn+'\n'+context_fn+'''
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    int r = real_provider_compile_context(c); stop_test_pause(2); return r;
}
'''+stop_fn+'\n'
    (out/'provider_stop.h').write_text(header)
    source = (ROOT/'android/tests/nvk_shader_pool_test.c').read_text()
    old_context = function(source, 'int s2a_direct_provider_compile_context(')
    source = source.replace(old_context, '')
    needle = '#include "direct/jv.h"\n'
    assert source.count(needle) == 1
    source = source.replace(needle, needle+'#include "provider_stop.h"\n')
    needle = 'captured[req.bytes] = 0; compiler_calls++;'
    assert source.count(needle) == 1
    source = source.replace(needle, needle+' stop_test_pause(1);')
    assert source.count('int main(int argc, char **argv) {') == 1
    source = source.replace('int main(int argc, char **argv) {', 'int unused_shader_pool_main(int argc, char **argv) {')
    source += '\n#include "nvk_compile_stop_test.c"\n'
    variant = out/'fixture.c'; variant.write_text(source)
    inc = (out, include, ROOT/'android/tests', ROOT/'runtime/include', native, native/'nvn', native/'direct', gfx, gfx/'replay', gfx/'gfx/nvn', gfx/'gfx/nvn/include', ROOT/'android/tests/vkhost', native/'gfx_plat')
    libs = [gfx/'replay/nvn_unimpl.c', gfx/'replay/rp_state.c', native/'nvn/nvn_vk_compute_request.c',
            native/'nvn/nvn_vk_prog_warm.c', native/'nvn/nvn_vk_frame_log.c', ROOT/'runtime/src/direct_host.c']
    libs += [native/'direct'/f for f in ('source_store.c', 'control.c', 'input_identity.c', 'texture_facts.c', 'sha256.c', 'json.c', 'compile_request.c')]
    strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    cc = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections', '-pthread']
    # 与 nvk_shader_pool_test.py 同一条：host_link 自动补链的 offline_library.c / program_cache.c
    # 在 strict c11 glibc 下要特性宏（realpath / O_CLOEXEC / openat …）；只加给非 macOS（苹果 libc 不需要）。
    if sys.platform != 'darwin':
        cc.append('-D_GNU_SOURCE')
    cc += ['-I'+str(p) for p in inc]
    test_shader = native/'gfx_plat/nvk_shader.c'
    if sys.argv[1:] == ['--withdraw-stop']:
        src = test_shader.read_text()
        before = function(src, 'void nvk_shader_stop(void) {')
        test_shader = out/'without-stop.c'; test_shader.write_text(src.replace(before, 'void nvk_shader_stop(void) {}', 1))
    elif sys.argv[1:] == ['--withdraw-admit']:
        src = test_shader.read_text()
        # 停局挡编译有两道：准入（停局位）与三段式认领时的停局检查（#996 起，后者在更前面先拦住）。
        #   只撤准入，这条用例走不到准入就被认领挡下，负对照红不起来；所以两道一起撤，证明「停局会挡住编译」
        for before, after in (('if (s & NVK_SHADER_STOP_BIT) return 0;', 'if (0 && (s & NVK_SHADER_STOP_BIT)) return 0;'),
                              ('if (shader_stopped() || (bg && g_warm_quiesced)) return -1;', 'if (bg && g_warm_quiesced) return -1;'),
                              ('if (claim < 0 && shader_stopped()) return -1;', 'if (0 && claim < 0) return -1;')):
            assert src.count(before) == 1, before
            src = src.replace(before, after)
        test_shader = out/'without-admit.c'; test_shader.write_text(src)
    elif sys.argv[1:] in (['--withdraw-activation'], ['--withdraw-frame-dump']):
        pass
    elif sys.argv[1:]:
        raise ValueError('usage: nvk_compile_stop_test.py [--withdraw-stop|--withdraw-admit|--withdraw-activation|--withdraw-frame-dump]')
    exe = out/'test'; host_link.run(cc+[str(variant), str(test_shader), *map(str, libs), strip, '-lm', '-o', str(exe)], check=True, timeout=60)
    rc = 0
    for mode in ('control', 'late', 'race', 'activate', 'frame', 'frame-off', 'frame-empty'):
        env = dict(os.environ)
        env['debug.s2a.frame_log'] = '1' if mode in ('frame', 'frame-empty') else '0'
        r = subprocess.run([str(exe), mode], text=True, capture_output=True, timeout=15, env=env)
        print(r.stdout+r.stderr, end=''); rc |= r.returncode
        if r.returncode == 0 and mode.startswith('frame'):
            captures = direct_framelog.parse(r.stderr)
            if mode == 'frame':
                if len(captures) != 1:
                    print('FAIL: live provider Stop must emit one complete frame-log dump', file=sys.stderr)
                    rc = 1
                else:
                    cap = captures[0]
                    if (len(cap['ts']) != 2 or cap['acquire_us'] != [3, 5]
                            or cap['present_us'] != [7, 11] or cap['fence_us'] != [0, 2500]):
                        print('FAIL: retained Stop changed the two captured frame records', file=sys.stderr)
                        rc = 1
                    else:
                        print('PASS: retained Stop emitted two records in one verified frame-log dump')
            elif captures:
                print(f'FAIL: {mode} emitted a frame-log dump', file=sys.stderr)
                rc = 1
            else:
                print(f'PASS: {mode} emitted no frame-log dump')
    return rc

if __name__ == '__main__': raise SystemExit(main())
