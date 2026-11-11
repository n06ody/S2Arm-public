#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real record default hook to worker/pipeline/retirement. Compiler/cache/Vulkan are fixtures, not GPU proof."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
NATIVE=ROOT/'android/native'
sys.path.insert(0,str(ROOT/'android/core-link'))
import link_symbols
spec=importlib.util.spec_from_file_location('program',Path(__file__).with_name('w2_compute_program_test.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def main():
    if not sys.platform.startswith('linux'):
        raise RuntimeError('record chain host test requires Linux libvulkan.so.1; no device/GPU claim')
    ndk=link_symbols.find_ndk()
    if ndk is None:raise RuntimeError('compute bridge host test needs pinned NDK29, no download/skip')
    host='darwin-x86_64' if sys.platform=='darwin' else 'linux-x86_64'
    vulkan=ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include/vulkan'
    good=m.fixture()
    # 合并后 lookup 要求结果封套带嵌套 transport（编译器 DirectShaderCompiler.cs:536 形状），
    # 且 fresh 结果必须回显 REQUEST 顶层 requestId；<RID> 由 integration 夹具里的 worker 桩按
    # 活的 requestId 原地回填（record_chain 复用同一份 compile_fixture）。
    good['transport']={'session':11,'generation':22,'requestId':'<RID>'}
    good['content']['identity']={'directABI':1,'translatorClosure':'closure','runtimeCompilerABI':'s2a-direct-compile-v1','profileHash':'sha256-'+'a'*64}
    unaligned=copy.deepcopy(good);unaligned['content']['hasUnalignedStorageBuffer']=True
    texture=copy.deepcopy(good);texture['content']['bindings'].append({'stage':'cs','kind':'texture','set':2,'binding':3,'slot':8,'count':1})
    source=(NATIVE/'direct/vk_record.c').read_text()
    variants={'positive':source,
        'raw-compute-reason':source.replace('const char *reason = compute_reason_code(why);', 'const char *reason = why ? why : compute_reason_code(why);'),
        'raw-compute-trace':source.replace('source_names[source],outcome_name,reason);', 'source_names[source],outcome_name,why?why:"");'),
        'withdraw-default-hook':source.replace('if (!builder && g_keep_state && nvn_vk_compute_build) builder = nvn_vk_compute_build;', '/* withdrawn default hook */'),
        'wrong-cs-snapshot':source.replace('memcpy(ds.ssbo, S->st.ssbo[5], sizeof ds.ssbo);', 'memcpy(ds.ssbo, S->st.ssbo[4], sizeof ds.ssbo);'),
        'omit-compute-outcome':source.replace('g_compute.dispatches++;', '/* withdrawn outcome accounting */'),
        'unbounded-compute-trace':source.replace('g_compute.trace_lines < 64', 'g_compute.trace_lines < 64000'),
        'reset-clears-session':source.replace('memset(&g_state, 0, sizeof g_state);', 's2a_record_compute_session_end(); (void)s2a_record_compute_session_begin(11,22); memset(&g_state, 0, sizeof g_state);'),
        'omit-retirement':source.replace('e->keep.compute_finish(d, e->keep.compute_owner, REC_SUBMIT_COMPLETED)', '0')}
    with tempfile.TemporaryDirectory(prefix='w2-record-chain-') as d:
        td=Path(d);inc=td/'include';inc.mkdir();(inc/'android').mkdir();(inc/'sys').mkdir();(inc/'vulkan').symlink_to(vulkan)
        (inc/'android'/'native_window.h').write_text('#pragma once\ntypedef struct ANativeWindow ANativeWindow;\n')
        if (vulkan.parent/'vk_video').exists():(inc/'vk_video').symlink_to(vulkan.parent/'vk_video')
        (inc/'android/log.h').write_text('#pragma once\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_WARN 5\n#define ANDROID_LOG_ERROR 6\n#include <stdarg.h>\n#include <stdio.h>\nstatic inline int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;char b[4096];va_list a;va_start(a,f);int n=vsnprintf(b,sizeof b,f,a);va_end(a);fprintf(stderr,"%s\\n",b);return n;}\n')
        (inc/'sys/system_properties.h').write_text('#pragma once\n#define PROP_VALUE_MAX 92\nstatic inline int __system_property_get(const char*k,char*v){(void)k;*v=0;return 0;}\n')
        for name,obj in [('good',good),('unaligned',unaligned),('texture',texture)]:
            (td/(name+'.json')).write_text(json.dumps(obj))
        (td/'source-store.c').write_text('#define s2a_source_acquire real_source_acquire\n#define s2a_source_release real_source_release\n#include \"'+str(NATIVE/'direct/source_store.c')+'\"\n')
        # Reuse only Vulkan boundary functions from the existing component test.
        # No fake parser/pipeline/worker is linked in this integration executable.
        component=Path(__file__).with_name('w2_compute_pipeline_test.c').read_text()
        prefix=component[:component.index('int nvn_vk_program_parse_compute(')]
        guest_io=component[component.index('static int rd('):component.index('static uint32_t spv[')]
        prefix=prefix.replace('static int live,','static int profile_mismatch, pipeline;\nstatic int live,')
        prefix=prefix.replace('minStorageBufferOffsetAlignment=16','minStorageBufferOffsetAlignment=profile_mismatch?32:16')
        prefix=prefix.replace('assert(c->codeSize==20);CREATE','assert(c->codeSize==20);pipeline++;CREATE')
        # 本线 w2_compute_pipeline_test.c 的 Vulkan mock 是分支版（不是 main 的 fixture.binding_count-drop
        # 形态）：同样只保留“n==1”的弱断言，并跳过逐绑定的描述符断言（本用例的 fixture 自己管绑定）。
        old='assert(n<=(uint32_t)fixture.binding_count);for(uint32_t i=0;i<n;i++){int infix=0;for(int j=0;j<fixture.binding_count;j++)if(w[i].dstBinding==(uint32_t)fixture.bindings[j].binding)infix=1;assert(infix);}'  # #473 起夹具按 fixture.bindings 校验；本测试只走单绑定
        assert old in prefix;prefix=prefix.replace(old,'assert(n==1);')
        old='for(uint32_t i=0;i<n;i++) if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)'
        assert old in prefix;prefix=prefix.replace(old,'for(uint32_t i=0;i<n&&0;i++) if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)')
        # Use the production vk_device memory-type chooser, avoiding duplicate definitions.
        prefix='\n'.join(line for line in prefix.splitlines() if not line.startswith('uint32_t s2a_vk_memory_type('))+'\n'
        prefix=prefix.replace('*out=(void*)m;return VK_SUCCESS;', '*out=(void*)m;mapped_storage=*out;return VK_SUCCESS;')
        # 本用例链接真实 nvn_vk_mrt.c（compile_facts 需要 nvn_vk_bcrt_view_format），所以把抽到的
        # 组件测试 prefix 里的 nvn_vk_guest_buffer_extent 替身去掉，避免与生产定义重复。
        start=prefix.find('uint64_t nvn_vk_guest_buffer_extent(')
        assert start>=0,'component prefix lost nvn_vk_guest_buffer_extent stub'
        end=prefix.index('\n}\n',start)+3
        prefix=prefix[:start]+prefix[end:]
        # Assert completion copyback happens only after record frees its command buffer.
        guest_io=guest_io.replace('(void)opaque;writes++;', '(void)opaque;assert(freed==1);writes++;')
        (td/'integration_vk_mock.h').write_text(prefix+guest_io)
        base=[os.environ.get('CC','cc'),'-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-variable','-pthread']
        base+=['-I'+str(p) for p in (td,inc,NATIVE,NATIVE/'direct',NATIVE/'nvn',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include')]
        base+=[str(Path(__file__).with_suffix('.c')),str(td/'record.c'),str(td/'source-store.c'),str(NATIVE/'direct/compile_worker.c'),str(NATIVE/'nvn/nvn_vk_compute_bridge.c')]
        base+=[str(NATIVE/'nvn'/p) for p in ('nvn_vk_compute_request.c','nvn_vk_compute_lookup.c','nvn_vk_program_result.c','nvn_vk_compile_facts.c','nvn_vk_compute_texture.c')]
        base+=[str(NATIVE/'direct'/p) for p in ('bind_defer.c','control.c','diag.c','compile_export.c','input_identity.c','texture_facts.c','sha256.c','jv.c','json.c','t2t_plan.c','vk_clear.c','vk_device.c','gs_path.c')]
        base += [str(NATIVE/'nvn'/p) for p in ('nvn_vk_format.c','nvn_vk_query.c','nvn_vk_mrt.c')]
        for name,text in variants.items():
            if name!='positive':assert text!=source,name
            (td/'record.c').write_text(text);exe=td/name
            subprocess.run(base+['-Wl,-l:libvulkan.so.1','-lm','-o',str(exe)],check=True,timeout=30)
            failures=[]
            for mode in (0,2,11,12,20,21,22,23):
                p=subprocess.run([str(exe),str(mode),str(td/('unaligned.json' if mode==2 else 'good.json')),str(td/'texture.json')],text=True,capture_output=True,timeout=5)
                if any(secret in p.stderr for secret in ('sensitive_shader_bytes','/private/title/secret','asset-secret','shader{secret}')):
                    assert name!='positive', 'diagnostic leaked arbitrary reason'
                    failures.append(mode)
                if p.returncode:
                    failures.append(mode)
                    if name!='positive':print(name,'case',mode,'exit',p.returncode,p.stderr.strip())
                if name=='positive':print(p.stdout+p.stderr,end='')
            if name=='positive':assert not failures,failures
            else:assert failures,(name,'mutant survived');print(name,'expected red',failures)
        # Explicit production source list and gate registration (weak hook is insufficient).
        links=(ROOT/'android/core-link/LinkSources.cmake').read_text()
        for source_name in ('nvn_vk_compute_bridge.c','nvn_vk_compute_request.c','nvn_vk_compute_lookup.c','nvn_vk_compile_facts.c'):
            assert links.count('native/nvn/'+source_name)==1
        assert 'w2_record_chain_test.py' in (ROOT/'android/test.sh').read_text()
        print('PASS: 8 dynamic record default-hook cases with real worker, 8 effective negative controls; concurrent bounded compute accounting')
if __name__=='__main__':main()
