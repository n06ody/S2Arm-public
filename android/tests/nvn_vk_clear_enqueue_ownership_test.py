#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Production clear recorder + first-draw queue; Vulkan boundaries are mocked, no GPU/device."""
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE));import host_link
sys.path.insert(0,str(ROOT/'android/core-link'));import link_symbols

def main():
    ndk=link_symbols.find_ndk()
    if ndk is None: raise RuntimeError('NDK 29 required')
    platform='darwin-x86_64' if sys.platform=='darwin' else 'linux-x86_64'
    hdr=ndk/'toolchains/llvm/prebuilt'/platform/'sysroot/usr/include'
    build=ROOT/'build/clear-ownership-test';build.mkdir(parents=True,exist_ok=True);tempfile.tempdir=str(build)
    with tempfile.TemporaryDirectory(dir=build) as t:
        td=Path(t);(td/'vulkan').symlink_to(hdr/'vulkan');(td/'vk_video').symlink_to(hdr/'vk_video')
        inc=[td,HERE/'vkhost',HERE/'hostvk',ROOT/'android/native',ROOT/'android/native/direct',ROOT/'android/native/nvn',ROOT/'runtime/include',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include',ROOT/'graphics/nvn/gfx/tex']
        exe=td/'test';cmd=['cc','-std=gnu11','-O0','-ffunction-sections','-fdata-sections','-pthread']+['-I'+str(p) for p in inc]
        cmd += [str(HERE/'nvn_vk_clear_enqueue_ownership_test.c'),str(ROOT/'android/native/direct/vk_clear.c')]
        cmd += (['-Wl,-dead_strip','-L/opt/homebrew/lib','-lvulkan'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-Wl,-l:libvulkan.so.1'])+['-lm','-o',str(exe)]
        host_link.run(cmd,check=True)
        failures=[]
        for mode in ('refused','full','queued','submitted','injected','submit-fail','prior-fail'):
            for aspect in ('color','depth'):
                p=subprocess.run([str(exe),mode,aspect],capture_output=True,text=True)
                print(mode,aspect,p.returncode,p.stdout.strip(),p.stderr.strip(),flush=True)
                if p.returncode: failures.append(mode+'-'+aspect)
        if failures: raise SystemExit('FAILED: '+','.join(failures))
        first_draw=(ROOT/'android/native/nvn/nvn_vk_first_draw.c').read_text()
        clear=(ROOT/'android/native/direct/vk_clear.c').read_text()
        fixture=(HERE/'nvn_vk_clear_enqueue_ownership_test.c').read_text()
        variants=(
            ('wrong-queue-ownership',first_draw.replace('return S2A_FQ_ENQUEUE_FAILED_OWNED;',
                                                       'return S2A_FQ_ENQUEUE_NOT_TAKEN;'),clear),
            ('caller-frees-owned',first_draw,clear.replace('enqueued == S2A_FQ_ENQUEUE_NOT_TAKEN',
                                                         'enqueued != S2A_FQ_ENQUEUE_ACCEPTED')),
        )
        for name, first, clear_src in variants:
            assert (first!=first_draw or clear_src!=clear), name+' mutation did not apply'
            first_path=td/(name+'-first.c');first_path.write_text(first)
            clear_path=td/(name+'-clear.c');clear_path.write_text(clear_src)
            fixture_path=td/(name+'-fixture.c')
            fixture_path.write_text(fixture.replace('#include "../native/nvn/nvn_vk_first_draw.c"',
                                                    '#include "'+str(first_path)+'"').replace(
                                                    '#include "../native/direct/vk_clear.h"',
                                                    '#include "vk_clear.h"'))
            mutant=td/name
            mcmd=['cc','-std=gnu11','-O0','-ffunction-sections','-fdata-sections','-pthread']
            mcmd+=['-I'+str(p) for p in inc]+[str(fixture_path),str(clear_path)]
            mcmd+=(['-Wl,-dead_strip','-L/opt/homebrew/lib','-lvulkan'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-Wl,-l:libvulkan.so.1'])+['-lm','-o',str(mutant)]
            host_link.run(mcmd,check=True)
            p=subprocess.run([str(mutant),'submit-fail','color'],capture_output=True,text=True)
            print(name,'expected red',p.returncode,p.stderr.strip(),flush=True)
            assert p.returncode and 'freed command remains owned by frame queue' in p.stderr, name+' control missed ownership assertion'
if __name__=='__main__':main()
