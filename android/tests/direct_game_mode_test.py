#!/usr/bin/env python3
"""Run production direct present_period with a fake clock and game interval."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'graphics/nvn/gfx/nvn/hle_nvn.c').read_text()
def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

preamble = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
typedef struct { int interval; } win_t;
static _Atomic int g_game_itv = 1;
static _Atomic uint64_t g_itv2_since;
static int g_hz = 60;
static void present_cfg(const win_t *o) { (void)o; }
static int present_itv(const win_t *o) { return o->interval > 0 ? o->interval : 1; }
static uint64_t now_ns(void) { return 1000000000; }
static uint64_t nvn_fps_period_ns(uint64_t input) { return input; }
uint64_t s2a_android_limit_frame_period(uint64_t input);
void s2a_android_set_battery_limit(int enabled);
void s2a_android_present_note(uint64_t period_ns);
uint64_t s2a_android_present_ns(void);
'''
# Before production exists, an isolated API stub lets the real old present_period run
# and fail on behavior, rather than stopping at a missing symbol.
policy = root / 'android/native/system_tuning.c'
if policy.exists():
    support = '#include "' + str(policy) + '"\n'
else:
    support = r'''
static int battery_cap;
static uint64_t noted_ns;
void s2a_android_set_battery_limit(int enabled) { battery_cap = enabled; }
uint64_t s2a_android_limit_frame_period(uint64_t input) { return battery_cap && input < 33333334 ? 33333334 : input; }
void s2a_android_present_note(uint64_t period_ns) { noted_ns = period_ns; }
uint64_t s2a_android_present_ns(void) { return noted_ns; }
'''
main = r'''
static int fail;
static void check(int cap, int hz, int interval, uint64_t expect, const char *name) {
    s2a_android_set_battery_limit(cap); g_hz=hz; win_t w={interval};
    uint64_t actual=present_period(&w);
    if (actual != expect) { fprintf(stderr, "%s: expected %llu ns, got %llu ns\n", name, (unsigned long long)expect, (unsigned long long)actual); fail++; }
}
int main(void) {
    if (s2a_android_present_ns() != 0) { fputs("present note published before any present\n", stderr); fail++; }
    check(0,60,1,16666666,"off 60");
    check(1,60,1,33333334,"battery cap 60 to 30");
    check(1,60,2,33333334,"native 30 stays 30");
    check(1,60,3,50000000,"native 20 stays 20");
    check(1,0,0,33333334,"unbounded capped");
    check(0,0,0,0,"unbounded restored");
    // A3 投票源：登记的正是最终周期（含省电档上限）
    s2a_android_set_battery_limit(1); g_hz=60; win_t w={1};
    if (s2a_android_present_ns() != 0) { fputs("present note leaked across checks\n", stderr); fail++; }
    present_period(&w);
    if (s2a_android_present_ns() != 33333334) { fprintf(stderr, "present note: expected capped 33333334 ns, got %llu ns\n", (unsigned long long)s2a_android_present_ns()); fail++; }
    s2a_android_set_battery_limit(0); present_period(&w);
    if (s2a_android_present_ns() != 16666666) { fprintf(stderr, "present note: expected restored 16666666 ns, got %llu ns\n", (unsigned long long)s2a_android_present_ns()); fail++; }
    if (fail) return 1;
    puts("direct production present period / battery cap / present note passed"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='direct-game-mode-') as temp:
    d=Path(temp); (d/'test.c').write_text(preamble + support + function('static uint64_t present_period(const win_t *o) {') + main)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-D__ANDROID__','-I',str(root/'android/native'),str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
