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
    build=ROOT/'build/clear-fv-test';build.mkdir(parents=True,exist_ok=True);tempfile.tempdir=str(build)
    with tempfile.TemporaryDirectory(dir=build) as t:
        td=Path(t);(td/'vulkan').symlink_to(hdr/'vulkan');(td/'vk_video').symlink_to(hdr/'vk_video')
        inc=[td,HERE/'vkhost',HERE/'hostvk',ROOT/'android/native',ROOT/'android/native/direct',ROOT/'android/native/nvn',ROOT/'runtime/include',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include',ROOT/'graphics/nvn/gfx/tex']
        exe=td/'test';cmd=['cc','-std=gnu11','-O0','-ffunction-sections','-fdata-sections','-pthread']+['-I'+str(p) for p in inc]
        cmd += [str(HERE/'nvn_vk_clear_fv_test.c'),str(ROOT/'android/native/direct/vk_clear.c'),str(ROOT/'android/native/nvn/nvn_vk_texcache.c'),str(ROOT/'android/native/direct/vk_record.c')]
        cmd += (['-Wl,-dead_strip','-L/opt/homebrew/lib','-lvulkan'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-Wl,-l:libvulkan.so.1'])+['-lm','-o',str(exe)]
        host_link.run(cmd,check=True)
        failures=[]
        for mode in ('full','off','not-taken','failed-owned','timeout','record'):
          for aspect in ('color','depth'):
            p=subprocess.run([str(exe),mode,aspect],capture_output=True,text=True)
            print(mode,aspect,p.returncode,p.stdout.strip(),p.stderr.strip(),flush=True)
            if p.returncode: failures.append(mode+'-'+aspect)
        if failures: raise SystemExit('FAILED: '+','.join(failures))
        first=(ROOT/'android/native/nvn/nvn_vk_first_draw.c').read_text()
        clear=(ROOT/'android/native/direct/vk_clear.c').read_text()
        fixture=(HERE/'nvn_vk_clear_fv_test.c').read_text()
        cases=[]
        def changed(src, old, new):
            assert old in src, 'missing mutation anchor: '+old
            return src.replace(old,new,1)
        cases.append(('full-sync',first,changed(clear,'if (!slot && !q_on)', 'if (!slot)'), 'full', 'target 25'))
        cases.append(('forget-disabled',first,changed(clear,'if (!clr_fv_q_on() || !d || !d->device || g_fv_dev != d->device || !image || !generation) return;', 'return;'), 'full', 'fb_freed==2'))
        pos=first.index('int nvn_vk_first_draw_enqueue_clear(')
        end=first.index('// 返回 1 = 这个 draw',pos)
        prefix, body, suffix=first[:pos], first[pos:end], first[end:]
        cases.append(('skip-image-pin',prefix+changed(body,'if (fd_pin_tex(tex) != 0)', 'if (0 && fd_pin_tex(tex) != 0)')+suffix,clear,'full','g_pin_n==25'))
        cases.append(('wrong-owned-result',prefix+changed(body,'return S2A_FQ_ENQUEUE_FAILED_OWNED;', 'return S2A_FQ_ENQUEUE_NOT_TAKEN;')+suffix,clear,'failed-owned','freed command remains owned'))
        for name,first_src,clear_src,mode,expected in cases:
            a=td/(name+'-first.c'); a.write_text(first_src)
            b=td/(name+'-clear.c'); b.write_text(clear_src)
            f=td/(name+'-test.c'); f.write_text(fixture.replace('#include "../native/nvn/nvn_vk_first_draw.c"', '#include "'+str(a)+'"').replace('#include "../native/direct/vk_clear.h"', '#include "vk_clear.h"'))
            mutant=td/name
            cmd=['cc','-std=gnu11','-O0','-ffunction-sections','-fdata-sections','-pthread']+['-I'+str(p) for p in inc]+[str(f),str(b),str(ROOT/'android/native/nvn/nvn_vk_texcache.c'),str(ROOT/'android/native/direct/vk_record.c')]
            cmd+=(['-Wl,-dead_strip','-L/opt/homebrew/lib','-lvulkan'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-Wl,-l:libvulkan.so.1'])+['-lm','-o',str(mutant)]
            host_link.run(cmd,check=True)
            proc=subprocess.run([str(mutant),mode,'color'],capture_output=True,text=True)
            print(name,'negative',proc.returncode,proc.stderr.strip(),flush=True)
            assert proc.returncode and expected in proc.stderr, name+' did not fail at expected assertion'
if __name__=='__main__':main()
