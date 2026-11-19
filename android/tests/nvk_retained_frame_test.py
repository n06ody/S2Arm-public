#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real provider Stop and frame-log producer: retained session must export the recorded frames."""
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols
sys.path.insert(0, str(ROOT / 'android'))
import direct_framelog

def function(text, signature):
    start = text.index(signature); begin = text.index('{', start); depth = 1; end = begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]

def main():
    out = ROOT.parent / (ROOT.name + '-out') / 'retained-frame'
    out.mkdir(parents=True, exist_ok=True)
    src = (ROOT / 'android/native/gfx_plat/provider_nvk.c').read_text()
    mode = sys.argv[1:]
    if mode == ['--withdraw']:
        line = '        if (nvn_vk_frame_log_snapshot_retained) nvn_vk_frame_log_snapshot_retained(nvk_session_stop_deadline());'
        assert src.count(line) == 2
        src = src.replace(line, '')
    elif mode not in ([], ['--withdraw-deadline']):
        raise ValueError('usage: nvk_retained_frame_test.py [--withdraw|--withdraw-deadline]')
    header = '''#include <stdarg.h>\n#include <time.h>\n'''+function(src, 'struct provider_context {')+''';
static struct provider_context *g_retained_context;
static pthread_mutex_t g_window_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_stop_published, g_session_active;
#define TAG "test"
#define ANDROID_LOG_INFO 4
static int __android_log_print(int l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; return 0; }
static void nvk_session_request_stop(void) {}
static void ctl_wake_stop(void) {}
static int stop_window_lock(void) { return pthread_mutex_lock(&g_window_mu); }
static int nvk_session_wait_compiles(void) { return 0; }
static void nvk_session_flush_pipe_cache(void) {}
static __attribute__((unused)) uint64_t nvk_session_stop_deadline(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec + (test_expired ? 0 : 700000000ull);
}
static void set_window_error(struct provider_context *c, const char *s) { snprintf(c->window_error, sizeof c->window_error, "%s", s); }
'''+function(src, 'static int32_t request_stop_impl(')+'\n'
    (out/'retained_stop.h').write_text(header)
    ndk = link_symbols.find_ndk()
    if ndk is None: raise RuntimeError('NDK 29 headers required')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    inc = ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include'
    include = out/'include'; include.mkdir(exist_ok=True)
    for name in ('vulkan', 'vk_video'):
        if (inc/name).exists() and not (include/name).exists(): (include/name).symlink_to(inc/name)
    native = ROOT/'android/native'
    cc = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-I'+str(out), '-I'+str(include), '-I'+str(native), '-I'+str(native/'nvn'), '-I'+str(native/'direct')]
    exe = out/'test'
    producer = native/'nvn/nvn_vk_frame_log.c'
    if mode == ['--withdraw-deadline']:
        contents = producer.read_text()
        target = 'if (current >= deadline_ns) break;'
        assert contents.count(target) == 1
        producer = out/'without-deadline.c'
        producer.write_text(contents.replace(target, 'if (0 && current >= deadline_ns) break;'))
    subprocess.run(cc+[str(ROOT/'android/tests/nvk_retained_frame_test.c'), str(producer), str(native/'direct/sha256.c'), '-o', str(exe)], check=True)
    env = dict(os.environ); env['debug.s2a.frame_log'] = '1'
    r = subprocess.run([str(exe)], env=env, text=True, capture_output=True, timeout=10)
    print(r.stderr, end='')
    if r.returncode or 'direct frame-log meta:' not in r.stderr or 'recorded=2' not in r.stderr or 'direct frame-log end: frames=2' not in r.stderr:
        print('FAIL: retained Stop must publish both recorded frames', file=sys.stderr)
        return 1
    try:
        captures = direct_framelog.parse(r.stderr)
        if len(captures) != 1 or captures[0]['meta']['recorded'] != 2:
            raise ValueError('wrong number of records')
    except (direct_framelog.FrameLogError, KeyError, TypeError, ValueError) as exc:
        print(f'FAIL: retained capture integrity: {exc}', file=sys.stderr)
        return 1
    disabled = dict(env); disabled['debug.s2a.frame_log'] = '0'
    off = subprocess.run([str(exe)], env=disabled, text=True, capture_output=True, timeout=10)
    if off.returncode or 'direct frame-log meta:' in off.stderr:
        print('FAIL: disabled diagnostics wrote a frame log', file=sys.stderr)
        return 1
    expired = subprocess.run([str(exe), 'expired'], env=env, text=True, capture_output=True, timeout=10)
    if expired.returncode or 'direct frame-log meta:' in expired.stderr:
        print('FAIL: expired Stop deadline should not claim a snapshot', file=sys.stderr)
        return 1
    # Do not drain the pipe: a full output stream blocks the real writer. Stop
    # must still return within its 100 ms wait plus scheduling tolerance.
    start = time.monotonic()
    proc = subprocess.Popen([str(exe), 'blocked'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.communicate()
        print('FAIL: retained Stop blocked on frame-log IO', file=sys.stderr)
        return 1
    elapsed = time.monotonic() - start
    stdout, _ = proc.communicate()
    if proc.returncode or 'retained stop preserved' not in stdout or elapsed > 1.0:
        print(f'FAIL: retained Stop exceeded budget ({elapsed:.3f}s)', file=sys.stderr)
        return 1
    print(f'blocked output: Stop returned in {elapsed:.3f}s')
    return 0
if __name__ == '__main__': raise SystemExit(main())
