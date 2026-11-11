#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real pthread worker and full native compute bridge chain; Vulkan/compiler boundary fixtures, not GPU proof."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）
ROOT=Path(__file__).resolve().parents[2]
NATIVE=ROOT/'android/native'
sys.path.insert(0,str(ROOT/'android/core-link'))
import link_symbols
spec=importlib.util.spec_from_file_location('program',Path(__file__).with_name('w2_compute_program_test.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def main():
    ndk=link_symbols.find_ndk()
    if ndk is None:raise RuntimeError('compute bridge host test needs pinned NDK29, no download/skip')
    host='darwin-x86_64' if sys.platform=='darwin' else 'linux-x86_64'
    vulkan=ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include/vulkan'
    good=m.fixture()
    # 真实结果信封：transport 是嵌套对象（DirectShaderCompiler.cs:536），顶层没有 session/generation。
    # requestId 用 <RID> 占位，worker 桩按 REQUEST 顶层 requestId 回填——fresh 回显检查跑在活身份上。
    good['transport']={'session':11,'generation':22,'requestId':'<RID>'}
    good['content']['identity']={'directABI':1,'translatorClosure':'closure','runtimeCompilerABI':'s2a-direct-compile-v1','profileHash':'sha256-'+'a'*64}
    unaligned=copy.deepcopy(good);unaligned['content']['hasUnalignedStorageBuffer']=True
    texture=copy.deepcopy(good);texture['content']['bindings'].append({'stage':'cs','kind':'texture','set':2,'binding':3,'slot':0,'count':1})
    source=(NATIVE/'direct/compile_worker.c').read_text()
    bridge=(NATIVE/'nvn/nvn_vk_compute_bridge.c').read_text()
    variants={'positive':(source,bridge),
        'ignore-unused-slot':(source,bridge.replace('slot < 16','slot < 8')),
        'ignore-worker-pause-result':(source.replace('if (pause(pause_ctx) != 0) {','if ((pause(pause_ctx), 0)) {'),bridge),
        'ignore-stop':(source,bridge.replace('if (s2a_vk_adapter_stop_requested())','if (0)'))}
    with tempfile.TemporaryDirectory(prefix='w2-bridge-') as d:
        td=Path(d);inc=td/'include';inc.mkdir();(inc/'vulkan').symlink_to(vulkan);(inc/'sys').mkdir();(inc/'sys/system_properties.h').write_text('#pragma once\n#define PROP_VALUE_MAX 92\nint __system_property_get(const char*,char*);\n')
        (inc/'android').mkdir()
        (inc/'android/log.h').write_text('#pragma once\n'
            'enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_ERROR = 6 };\n'
            'int __android_log_print(int, const char*, const char*, ...);\n')
        if (vulkan.parent/'vk_video').exists():(inc/'vk_video').symlink_to(vulkan.parent/'vk_video')
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
        # Integration drives the real bridge+pipeline: give the shared Vulkan-mock prefix a real texture
        # registry + resolver so the REQUEST carries facts (0x25 = NVN_FORMAT_RGBA8, 0x1 = TARGET_2D).
        fallback_facts = ('int s2a_vk_adapter_tex_facts(int id, struct s2a_vk_tex_facts *out, char *why, size_t cap) {\n'
                          '    (void)id;(void)out;snprintf(why,cap,"host fixture");return -1;\n}'
                          '\nuint64_t s2a_vk_adapter_registry_generation(void){return 0;}')
        assert fallback_facts in prefix
        prefix=prefix.replace(fallback_facts,
            'int s2a_vk_adapter_tex_facts(int id, struct s2a_vk_tex_facts *out, char *why, size_t cap) {\n'
            '    (void)id;(void)why;(void)cap;if(!out)return -1;memset(out,0,sizeof *out);\n'
            '    out->nvn_format=0x25;out->nvn_target=0x1;out->layers=1;out->levels=1;out->samples=1;return 0;\n}'
            '\nuint64_t s2a_vk_adapter_registry_generation(void){return 7;}')
        fallback_bind = ('int s2a_vk_nvn_texture_bind_for_compute(struct s2a_vk_device *d, uint64_t handle,\n'
                         '    struct nvn_vk_tex **tex_out, VkImageView *view_out, VkSampler *sampler_out, char *why, size_t cap) {\n'
                         '    (void)d;(void)handle;(void)tex_out;(void)view_out;(void)sampler_out;snprintf(why,cap,"host fixture");return -1;\n}')
        assert fallback_bind in prefix
        prefix=prefix.replace(fallback_bind,
            'int s2a_vk_nvn_texture_bind_for_compute(struct s2a_vk_device *d, uint64_t handle,\n'
            '    struct nvn_vk_tex **tex_out, VkImageView *view_out, VkSampler *sampler_out, char *why, size_t cap) {\n'
            '    (void)d;(void)why;(void)cap;memset(&mock_tex,0,sizeof mock_tex);\n'
            '    mock_tex.nvn_format=0x25;mock_tex.nvn_target=0x1;mock_tex.view=(VkImageView)0x11;mock_tex.view_att=(VkImageView)0x12;\n'
            '    *tex_out=&mock_tex;*view_out=(VkImageView)(uintptr_t)(0x1000+handle);*sampler_out=(VkSampler)0x22;return 0;\n}')
        old='assert(n<=(uint32_t)fixture.binding_count);for(uint32_t i=0;i<n;i++){int infix=0;for(int j=0;j<fixture.binding_count;j++)if(w[i].dstBinding==(uint32_t)fixture.bindings[j].binding)infix=1;assert(infix);}'  # #473 起夹具按 fixture.bindings 校验；本测试只走单绑定
        assert old in prefix;prefix=prefix.replace(old,'assert(n==1);')
        (td/'integration_vk_mock.h').write_text(prefix+guest_io)
        # macOS 弱引用放行由 host_link 按目标文件实算；本支保留消费点纹理快照依赖
        # nvn_vk_compute_texture.c 且不启用 S2A_COMPILE_EXPORT_HOST_TEST（本夹具没有
        # s2a_direct_diag_debug_open 桩，生产 compile_export_open() 走非主机分支返回 0，
        # 诊断门关，与本支 fixture 的 diag 断言一致）；main 新增的 compile_export_w2.c /
        # w2_single_query.c 是 compile_export 拆分后的生产依赖，保留。
        base=[os.environ.get('CC','cc')]+['-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-variable','-pthread']
        base+=['-I'+str(p) for p in (td,inc,NATIVE,NATIVE/'direct',NATIVE/'nvn',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include')]
        base+=[str(Path(__file__).with_suffix('.c')),str(td/'bridge.c'),str(td/'source-store.c'),str(td/'worker.c')]
        base+=[str(NATIVE/'nvn'/p) for p in ('nvn_vk_compute_request.c','nvn_vk_compute_lookup.c','nvn_vk_program_result.c','nvn_vk_compute_texture.c')]
        base+=[str(NATIVE/'direct'/p) for p in ('control.c','diag.c','compile_export.c','compile_export_w2.c','w2_single_query.c','input_identity.c','texture_facts.c','sha256.c','jv.c','json.c')]
        for name,(worker_text,text) in variants.items():
            if name!='positive':assert (worker_text,text)!=(source,bridge),name
            (td/'bridge.c').write_text(text);(td/'worker.c').write_text(worker_text);exe=td/name
            host_link.run(base+['-lm','-o',str(exe)],check=True,timeout=30)
            failures=[]
            for mode in range(21):
                try:
                    p=subprocess.run([str(exe),str(mode),str(td/('unaligned.json' if mode in (1,2) else 'good.json')),str(td/'texture.json')],text=True,capture_output=True,timeout=3)
                except subprocess.TimeoutExpired:
                    assert name!='positive';print(name,'case',mode,'bounded timeout after 3s');failures.append(mode);continue
                if p.returncode:failures.append(mode)
                if name=='positive':print(p.stdout+p.stderr,end='')
            if name=='positive':assert not failures,failures
            else:assert failures,(name,'mutant survived');print(name,'expected red',failures)
        # Compile only the real bridge as Android; other production sources
        # retain their actual host implementation. Properties are fixture IO,
        # never device/system mutation. Debug/release builds share assertions.
        for debug,mutation in ((0,"positive"),(1,"positive"),(0,"bypass-gate"),(1,"loose-flag")):
            obj=td/('bridge-debug%d.o'%debug)
            text=bridge
            if mutation=='bypass-gate':text=text.replace('const int debug_open = s2a_debug_entries_open();','const int debug_open = 1;')
            if mutation=="loose-flag":text=text.replace('__system_property_get("debug.s2a.no_live_compile", flag) == 1 && flag[0] == \'1\'', '__system_property_get("debug.s2a.no_live_compile", flag) > 0 && atoi(flag)')
            if mutation!='positive':assert text!=bridge
            (td/'bridge.c').write_text(text)
            includes=[x for x in base if x.startswith('-I')]
            subprocess.run([base[0],'-std=c11','-Wall','-Wextra','-Werror','-D__ANDROID__']+
                (['-DS2A_ANDROID_DEBUG_ENV=1'] if debug else [])+includes+
                ['-c',str(td/'bridge.c'),'-o',str(obj)],check=True,timeout=30)
            (td/'worker.c').write_text(source)
            cmd=[x for x in base if x!=str(td/'bridge.c')]+['-DW2_EXPECT_DEBUG='+str(debug),str(obj)]
            exe=td/('gate%d'%debug)
            host_link.run(cmd+['-lm','-o',str(exe)],check=True,timeout=30)
            failed=[]
            for mode in range(21,29):
                r=subprocess.run([str(exe),str(mode),str(td/'good.json'),str(td/'texture.json')],capture_output=True,text=True,timeout=3)
                print('gate',debug,mutation,mode,r.returncode,r.stdout+r.stderr)
                if r.returncode:failed.append(mode)
            if mutation=='positive':assert not failed,failed
            else:assert failed,(mutation,'survived');print(mutation,'expected red',failed)
        # Explicit production source list and gate registration (weak hook is insufficient).
        links=(ROOT/'android/core-link/LinkSources.cmake').read_text()
        for source_name in ('nvn_vk_compute_bridge.c','nvn_vk_compute_request.c','nvn_vk_compute_lookup.c','nvn_vk_compute_texture.c'):
            assert links.count('native/nvn/'+source_name)==1
        assert 'w2_compute_integration_test.py' in (ROOT/'android/test.sh').read_text()
        print('PASS: 21 native integrated cases with real worker, 3 effective negative controls; 16 Android debug/release gate cases and 2 gate negative controls')
if __name__=='__main__':main()
