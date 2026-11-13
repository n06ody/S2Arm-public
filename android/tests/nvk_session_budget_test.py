#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Baseline/current platform session timing under an in-flight resolver."""
import os
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'android/core-link'))
import link_symbols

def main():
    withdraw=sys.argv[1:]==['--withdraw-wait']
    if sys.argv[1:] and not withdraw: raise ValueError('usage: nvk_session_budget_test.py [--withdraw-wait]')
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
    paths=[inc,ROOT/'android/tests/hostvk',ROOT/'android/native',ROOT/'android/native/gfx_plat',ROOT/'graphics/nvn',ROOT/'graphics/nvn/replay',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include']
    flags=[os.environ.get('CC','cc'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-pthread']+['-I'+str(p) for p in paths]
    strip='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
    exe=out/'current'
    subprocess.run(flags+[str(ROOT/'android/tests/nvk_session_budget_test.c'),str(session),strip,'-o',str(exe)],check=True,timeout=30)
    rc=0
    for mode in ('control','busy'):
        r=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=10);print(r.stdout+r.stderr,end='');rc|=r.returncode
    return rc
if __name__=='__main__': raise SystemExit(main())
