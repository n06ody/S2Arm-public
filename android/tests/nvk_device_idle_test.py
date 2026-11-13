#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Test the original/current provider destroy with a stallable idle driver."""
import os
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'android/core-link'))
import link_symbols

def function(text, signature):
    start=text.index(signature); begin=text.index('{',start); depth=1; end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]

def main():
    baseline = sys.argv[1:] == ['--baseline']
    withdraw = sys.argv[1:] == ['--withdraw-deadline']
    if sys.argv[1:] and not (baseline or withdraw): raise ValueError('usage: nvk_device_idle_test.py [--baseline|--withdraw-deadline]')
    rel='android/native/gfx_plat/provider_nvk.c'
    src=subprocess.check_output(['git','show','HEAD:'+rel], cwd=ROOT, text=True) if baseline else (ROOT/rel).read_text()
    out=ROOT/'build/860-device-idle'; out.mkdir(parents=True,exist_ok=True)
    (out/'provider_context.h').write_text(function(src,'struct provider_context {')+';\n')
    helper=''
    if 'static void *device_idle_worker(' in src:
        helper=function(src,'static void *device_idle_worker(')+'\n'+function(src,'static int device_idle_bounded(')+'\n'
        if withdraw:
            old='if ((uint64_t)n >= deadline) {'; assert helper.count(old)==1
            helper=helper.replace(old,'if (0 && (uint64_t)n >= deadline) {')
    (out/'provider_destroy.h').write_text(helper+function(src,'static int32_t destroy_impl(')+'\n')
    ndk=link_symbols.find_ndk()
    if ndk is None: raise RuntimeError('NDK 29 headers required')
    host='darwin-x86_64' if sys.platform=='darwin' else 'linux-x86_64'
    sysinc=ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include'
    inc=out/'include'; inc.mkdir(exist_ok=True)
    for name in ('vulkan','vk_video'):
        if (sysinc/name).exists() and not (inc/name).exists(): (inc/name).symlink_to(sysinc/name)
    cc=[os.environ.get('CC','cc'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread', '-I'+str(out),'-I'+str(inc),'-I'+str(ROOT/'android/native'),'-I'+str(ROOT/'android/native/nvn')]
    if helper: cc+=['-DHAS_IDLE_WORKER']
    exe=out/('baseline' if baseline else 'current')
    subprocess.run(cc+[str(ROOT/'android/tests/nvk_device_idle_test.c'),'-o',str(exe)],check=True,timeout=30)
    rc=0
    for mode in ('control','blocked','unbind','active'):
        r=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=15)
        print(r.stdout+r.stderr,end=''); rc|=r.returncode
    return rc
if __name__=='__main__': raise SystemExit(main())
