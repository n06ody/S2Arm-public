#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real bridge/request/lookup/parser with worker and pipeline boundary mocks, not GPU proof."""
import copy
import importlib.util
import json
import os
import re
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
    # requestId 用 <RID> 占位，worker 桩按 REQUEST 的顶层 requestId 原地回填——和真编译器写封套
    # 同一语义，fresh 结果的 requestId 回显检查因此跑在活的 requestId 上，而不是夹具里写死的串。
    good['transport']={'session':11,'generation':22,'requestId':'<RID>'}
    good['content']['identity']={'directABI':1,'translatorClosure':'synthetic-closure-v1','runtimeCompilerABI':'s2a-direct-compile-v1','profileHash':'sha256-'+'a'*64}
    unaligned=copy.deepcopy(good);unaligned['content']['hasUnalignedStorageBuffer']=True
    texture=copy.deepcopy(good);texture['content']['bindings'].append({'stage':'cs','kind':'texture','set':2,'binding':3,'slot':0,'count':1})
    source=(NATIVE/'nvn/nvn_vk_compute_bridge.c').read_text()
    lsource=(NATIVE/'nvn/nvn_vk_compute_lookup.c').read_text()
    # 每个变异体 = (bridge.c 变体, lookup.c 变体)；只改一侧时另一侧取原文。
    # 落盘合同（F4.1）的负控：请求侧门/落盘、结果侧门/落盘/文件名配对、唯一请求集发布失败红行，
    # 每条都必须让 mode 0/29/30 的对应断言变红；注意变异体必须仍能编译（-Werror 下用 (void) 保活）。
    mutations={'positive':(source,lsource),
       'ignore-unused-ssbo':(source.replace('slot < 16','slot < 8'),lsource),
       'ignore-seen-mask':(source.replace('(state->ssbo_seen & (1u << slot))','1'),lsource),
       'constant-aligned':(source.replace('unaligned = 1;', 'unaligned = 0;'),lsource),
       'accept-unresolved-snapshot':(source.replace('if (nvk_compute_texture_snapshot_acquire(device, state, &snapshot, why, sizeof why))','if ((nvk_compute_texture_snapshot_acquire(device, state, &snapshot, why, sizeof why), 0))'),lsource),
       'drop-texture-fact':(source.replace('    nvk_compute_texture_facts(&snapshot, &texture_set);','    /* mutation: facts dropped */'),lsource),
       'claim-empty-complete':(source.replace('.count = texture_set.count, .complete = 1 };','.count = 0, .complete = 1 };'),lsource),
       'leak-snapshot':(source.replace('    nvk_compute_texture_snapshot_release(&snapshot);\n    if (rc) return rejected','    /* mutation: keep-alive leaked */\n    if (rc) return rejected'),lsource),
       'omit-opcode':(source.replace('result->opcode = OP_DISPATCH;', ''),lsource),
       'ignore-stop':(source.replace('if (s2a_vk_adapter_stop_requested())','if (0)'),lsource),
       'leak-source-pin':(source.replace('nvk_compute_request_release(&request);','/* leaked pin */'),lsource),
       # 请求侧落盘门失效 → 门关（mode29）仍写请求文件必红
       'request-diag-gate-off':(source.replace('if (!diag_open || !diag || !diag[0]) return;',
          'if (!diag || !diag[0]) return; (void)diag_open;'),lsource),
       # 请求侧落盘恒关 → 门开（mode0）没有请求/结果配对必红
       'request-diag-off':(source.replace('    diag_save(diag_open, request_id, request.identity, "compile-request.json",',
          '    diag_save(0, request_id, request.identity, "compile-request.json",'),lsource),
       # 唯一请求集入口没接 → mode0 的 requests-* 目录/导入合同必红
       'omit-export':(source.replace('        if (s2a_compile_export_request(dir, context.session, context.generation, context.direct_abi, 1,',
          '        if (0 && s2a_compile_export_request(dir, context.session, context.generation, context.direct_abi, 1,'),lsource),
       # 发布失败被吞掉 → mode30 没有红行必红
       'ignore-export-failure':(source.replace('            diag_note("【红】唯一请求集发布失败");','            (void)0;'),lsource),
       # 请求文件名退回 4 字节 → mode0 的“前缀=requestId + 身份全64hex”断言必红
       'request-name-4byte':(source.replace('snprintf(dname, sizeof dname, "%s-%s.%s", request_id, hid, suffix);',
          'snprintf(dname, sizeof dname, "%s-%02x%02x%02x%02x.%s", request_id, identity[0], identity[1], identity[2], identity[3], suffix); (void)hid;'),lsource),
       # 请求文件名缓冲缩到放不下 → 截断守卫跳写，文件计数断言随之转红
       'request-name-trunc':(source.replace('char dname[224], dwhy[160] = "";','char dname[48], dwhy[160] = "";'),lsource),
       # 结果侧落盘门失效 → 门关（mode29）仍写结果封套必红
       'result-diag-gate-off':(source,lsource.replace('if (options->diag_open && diag && diag[0] && bytes) {',
          'if (diag && diag[0] && bytes) {')),
       # 结果封套没写 → mode0 请求/结果配对必红
       'result-diag-off':(source,lsource.replace('if (options->diag_open && diag && diag[0] && bytes) {',
          'if (0 && diag && diag[0] && bytes) {')),
       # 结果文件名退回 4 字节 → mode0 同名配对（身份全 64hex）必红
       'result-name-4byte':(source,lsource.replace('snprintf(dname, sizeof dname, "%s-%s.compile-result.json", e.request_id, hid);',
          'snprintf(dname, sizeof dname, "%s-%02x%02x%02x%02x.compile-result.json", e.request_id, r->identity[0], r->identity[1], r->identity[2], r->identity[3]); (void)hid;')),
       # 结果文件名缓冲缩到放不下 → 守卫跳写，结果文件计数断言随之转红
       'result-name-trunc':(source,lsource.replace('char dname[256], dwhy[160] = "";','char dname[48], dwhy[160] = "";')),
       # REQUEST 顶层 requestId 不采集 → 空 want 与活的回显不一致，全部 fresh 成功用例必红
       'requestid-never-captured':(source,lsource.replace('''            const s2a_json_field *idf = s2a_json_get(j, "requestId");
            if (idf && idf->kind == S2A_JSON_STRING)
                snprintf(request_id, id_cap, "%s", idf->text);''',
          '''            /* mutation: requestId never captured */ (void)id_cap;''')),
    }
    with tempfile.TemporaryDirectory(prefix='w2-bridge-') as d:
        td=Path(d);inc=td/'include';inc.mkdir();(inc/'vulkan').symlink_to(vulkan)
        if (vulkan.parent/'vk_video').exists():(inc/'vk_video').symlink_to(vulkan.parent/'vk_video')
        for name,obj in [('good',good),('unaligned',unaligned),('texture',texture)]:
            (td/(name+'.json')).write_text(json.dumps(obj))
        (td/'source-store.c').write_text('#define s2a_source_acquire real_source_acquire\n#define s2a_source_release real_source_release\n#include \"'+str(NATIVE/'direct/source_store.c')+'\"\n')
        # mode30 的诊断根落在一个普通文件下面：create/write 都失败，但编译结果必须照常接受。
        (td/'blocked').write_text('this is a regular file, not a diagnostics directory\n')
        base=[os.environ.get('CC','cc'),'-DS2A_COMPILE_EXPORT_HOST_TEST=1','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread']
        base+=['-I'+str(p) for p in (inc,NATIVE,NATIVE/'nvn',NATIVE/'direct',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include')]
        base+=[str(Path(__file__).with_suffix('.c')),str(td/'bridge.c'),str(td/'lookup.c'),str(td/'source-store.c')]
        # 合并取舍：本支用真实的 nvn_vk_compute_request.c + nvn_vk_compute_texture.c（消费点纹理
        # 快照），main 在这条链上没有加 wrapper 依赖；main 侧新增的 compile_export_w2.c /
        # w2_single_query.c 是 compile_export 拆分后的新生产依赖，必须保留（缺则 undefined）。
        base+=[str(NATIVE/'nvn'/p) for p in ('nvn_vk_compute_request.c','nvn_vk_program_result.c','nvn_vk_compute_texture.c')]
        base+=[str(NATIVE/'direct'/p) for p in ('control.c','diag.c','compile_export.c','compile_export_w2.c','w2_single_query.c','input_identity.c','texture_facts.c','sha256.c','jv.c','json.c')]
        for name,(btext,ltext) in mutations.items():
            if name!='positive':assert btext!=source or ltext!=lsource,name
            (td/'bridge.c').write_text(btext);(td/'lookup.c').write_text(ltext);exe=td/name
            # 编译失败把 cc 原文打出来（11-06 main 全套 / 二分高并发时两次 cc 非零，门禁日志只剩 Python 堆栈，定不了因）
            cc=host_link.run(base+['-lm','-o',str(exe)],capture_output=True,text=True,errors='replace',timeout=30)
            if cc.returncode:
                print(f'【红】w2 bridge {name} 编译失败 rc={cc.returncode}（负数 = 被信号杀）：\n{cc.stdout[-2000:]}{cc.stderr[-6000:]}',file=sys.stderr,flush=True)
                raise SystemExit(1)
            failures=[]
            for mode in range(31):
                diag_dir = td/'blocked'/'diag' if mode==30 else td/(name+'-diag'+str(mode))
                p=subprocess.run([str(exe),str(mode),str(td/('unaligned.json' if mode in (1,2) else 'good.json')),
                    str(td/'texture.json'),str(diag_dir)],
                    text=True,capture_output=True,timeout=5,env=dict(os.environ, FULL_EXPORT_PROFILE='1'))
                if p.returncode:failures.append(mode)
                if name=='positive':print(p.stdout+p.stderr,end='')
            if name=='positive':assert not failures,failures
            else:assert failures,(name,'mutant survived');print(name,'expected red',failures)
        # 批编导入合同（direct_batch.py import-requests 的同名正则 + 前缀=正文 requestId）：
        # 对 positive 变体 mode-0 实落盘的请求文件过一遍原 importer 的判定式。
        reqfiles=[p for p in (td/'positive-diag0').iterdir() if p.name.endswith('.compile-request.json')]
        assert len(reqfiles)==1,reqfiles
        mname=re.fullmatch(r'(.+)-([0-9a-f]{64})\.compile-request\.json',reqfiles[0].name)
        assert mname,reqfiles[0].name
        assert mname.group(1)==json.loads(reqfiles[0].read_text())['requestId'],reqfiles[0].name
        # The separate unique request set is fed by the actual compute prepare path.
        sets=list((td/'positive-diag0').glob('requests-*'))
        assert len(sets)==1,sets
        exported=list(sets[0].glob('*.compile-request.json'))
        assert len(exported)==1 and exported[0].name==reqfiles[0].name
        assert exported[0].read_bytes()==reqfiles[0].read_bytes()
        sys.path.insert(0,str(ROOT/'android/tools'))
        import direct_batch
        manifest=direct_batch.import_requests(sets[0],td/'actual-import')
        assert len(manifest['entries'])==1
        imported=manifest['entries'][0]
        assert imported['identity']==mname.group(2)
        assert (td/'actual-import'/imported['request']).read_bytes()==reqfiles[0].read_bytes()
        # Explicit production source list and gate registration (weak hook is insufficient).
        links=(ROOT/'android/core-link/LinkSources.cmake').read_text()
        for source_name in ('nvn_vk_compute_bridge.c','nvn_vk_compute_request.c','nvn_vk_compute_lookup.c','nvn_vk_compute_texture.c'):
            assert links.count('native/nvn/'+source_name)==1
        assert 'w2_compute_bridge_test.py' in (ROOT/'android/test.sh').read_text()
        print('PASS: 31 bridge cases, %d effective negative controls, request/result diag pairing, import/gate registration'%(len(mutations)-1))
if __name__=='__main__':main()
