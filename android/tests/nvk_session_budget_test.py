#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/nvk_shutdown_test.py 在进程内按表逐个跑（含 --withdraw-* 撤回对照）
"""Baseline/current platform session timing under an in-flight resolver."""
import os
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'android/core-link'))
import link_symbols
import host_link

def main():
    args=sys.argv[1:]
    withdraw=args==['--withdraw-wait']
    withdraw_pool=args==['--withdraw-pool-retire']
    demand_close=args==['--demand-close']
    if args and not (withdraw or withdraw_pool or demand_close):
        raise ValueError('usage: nvk_session_budget_test.py [--withdraw-wait|--withdraw-pool-retire|--demand-close]')
    if demand_close:
        out=Path(os.environ.get('S2A_TEST_OUT_DIR',ROOT/'build/1581-session'))
        out.mkdir(parents=True,exist_ok=True)
        paths=[ROOT/'android/tests/vkhost',ROOT/'android/tests/hostvk',ROOT/'graphics/nvn/third_party/vulkan-headers/include',ROOT/'android/native',ROOT/'android/native/gfx_plat',ROOT/'graphics/nvn',ROOT/'graphics/nvn/replay',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include']
        flags=[os.environ.get('CC','cc'),'-D_POSIX_C_SOURCE=200809L','-std=c11','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-pthread']+['-I'+str(p) for p in paths]
        strip='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
        exe=out/'demand-close'
        host_link.run(flags+[str(ROOT/'android/tests/nvk_session_budget_test.c'),str(ROOT/'android/native/gfx_plat/nvk_session.c'),strip,'-o',str(exe)],check=True,timeout=30)
        r=subprocess.run([str(exe),'demand-close'],capture_output=True,text=True,timeout=10)
        print(r.stdout+r.stderr,end='')
        return r.returncode
    out=ROOT/'build/860-session-budget'; out.mkdir(parents=True,exist_ok=True)
    ndk=link_symbols.find_ndk()
    if ndk is None: raise RuntimeError('NDK 29 headers required')
    host='darwin-x86_64' if sys.platform=='darwin' else 'linux-x86_64'
    sysinc=ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include'; inc=out/'include'; inc.mkdir(exist_ok=True)
    for n in ('vulkan','vk_video'):
        if (sysinc/n).exists() and not (inc/n).exists(): (inc/n).symlink_to(sysinc/n)
    rel='android/native/gfx_plat/nvk_session.c'
    session=ROOT/rel
    if withdraw:
        text=(ROOT/rel).read_text(); old='while (nvk_shader_inflight()) {'; assert text.count(old)==1
        session=out/'without-wait.c'; session.write_text(text.replace(old,'while (0 && nvk_shader_inflight()) {'))
    if withdraw_pool:
        text=(ROOT/rel).read_text()
        old='if (s2a_compile_worker_stop_bounded(remaining_ms, why, sizeof why)) {'
        assert text.count(old)==1
        session=out/'without-pool-retire.c'
        session.write_text(text.replace(old, 'if (0 && s2a_compile_worker_stop_bounded(remaining_ms, why, sizeof why)) {'))
    paths=[inc,ROOT/'android/tests/hostvk',ROOT/'android/native',ROOT/'android/native/gfx_plat',ROOT/'graphics/nvn',ROOT/'graphics/nvn/replay',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include']
    flags=[os.environ.get('CC','cc'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-pthread']+['-I'+str(p) for p in paths]
    strip='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
    exe=out/'current'
    host_link.run(flags+[str(ROOT/'android/tests/nvk_session_budget_test.c'),str(session),strip,'-o',str(exe)],check=True,timeout=30)
    rc=0
    for mode in ('control','busy','log-off','log-default','idle-pool'):
        r=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=10);print(r.stdout+r.stderr,end='');rc|=r.returncode
    return rc
if __name__=='__main__': raise SystemExit(main())
