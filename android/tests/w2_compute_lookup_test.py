#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Production lookup control flow with cache/worker boundary mocks, not device proof."""
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
spec=importlib.util.spec_from_file_location('program',Path(__file__).with_name('w2_compute_program_test.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
good=m.fixture()
# 真实编译结果的 transport 是嵌套对象（编译器 DirectShaderCompiler.cs:536:
# {"transport":{session,generation,requestId}}），顶层只有 schema/contentHash/content/identity；
# 夹具必须照这个形状，否则测不到真实现。requestId 必须等于 REQUEST 顶层的 requestId——
# 它是“这份结果回答的是哪笔请求”的关联字段，fresh 路径按合同逐项比对 session/generation/requestId。
good['transport']={'session':11,'generation':22,'requestId':'rid-lookup'}
good['content']['identity']={'directABI':1,'translatorClosure':'closure','runtimeCompilerABI':'s2a-direct-compile-v1','profileHash':'sha256-'+'a'*64}
variants={}
x=copy.deepcopy(good);x['content']['stages'][0]['stage']='fs';variants['bad-stage']=x
x=copy.deepcopy(good);x['content']['identity']['profileHash']='sha256-'+'b'*64;variants['bad-profile']=x
x=copy.deepcopy(good);x['content']['identity']['translatorClosure']='other';variants['bad-closure']=x
x=copy.deepcopy(good);x['transport']['generation']=21;variants['old-generation']=x
x=copy.deepcopy(good);x['content']['hasUnalignedStorageBuffer']=True;variants['true-alignment']=x
x=copy.deepcopy(good);del x['transport'];variants['no-transport']=x
# 回显检查的负控：结果里回显的 requestId 与 REQUEST 顶层不一致（同会话另一笔请求/伪造）。
x=copy.deepcopy(good);x['transport']['requestId']='other';variants['bad-requestid']=x
cases=[(i,'bad-stage') for i in range(18)]
cases += [(18,'true-alignment'),(19,'true-alignment'),(20,'bad-stage'),(21,'bad-stage'),(22,'bad-stage'),(23,'bad-stage'),(24,'bad-stage'),(25,'bad-stage'),(26,'bad-stage')]
cases += [(28,'bad-stage'),(29,'bad-stage'),(30,'bad-stage'),(31,'no-transport'),(32,'bad-requestid'),(39,'bad-stage')]
cases[11]=(11,'bad-profile');cases[12]=(12,'bad-closure');cases[13]=(13,'old-generation');cases[17]=(17,'old-generation')
source=(NATIVE/'nvn/nvn_vk_compute_lookup.c').read_text()
mutations={'positive':source,
 'ignore-no-live-policy':source.replace('!bytes && options->no_live_compile', '!bytes && 0'),
 'ignore-prepared-profile':source.replace('strncmp(r->profile.hash, c->profile_hash, sizeof r->profile.hash)', '0'),
 'drop-profile-facts':source.replace('out->profile=r->profile;', 'memset(&out->profile,0,sizeof out->profile);'),
 'skip-semantic-validation':source.replace('rc = nvn_vk_program_parse_compute(bytes, length, &p, why, cap);','rc = 0;'),
 'ignore-alignment-reflection':source.replace('p.has_unaligned_storage_buffer != e->unaligned','0'),
 # 只比 session/generation、不看 transport 嵌套（旧实现口径）→ fresh 用例全红
 'transport-top-level':source.replace('if (s2a_json_parse_object(ft->raw, ft->raw_bytes, j, why, cap)) goto done;','/* mutation: 旧实现直接读顶层 session/generation */'),
 # 整个 transport 检查被跳过（skip-transport-check）→ no-transport/old-generation 变体必红
 'skip-transport-check':source.replace('if (fresh) {','if (fresh && 0) {'),
 # 只去掉 requestId 比对（skip-requestid-check）→ bad-requestid 变体必红
 'skip-requestid-check':source.replace(' ||\n            !string_is(j, "requestId", e->request_id)',''),
 # REQUEST 顶层 requestId 不采集 → 空 want 与活回显不一致，fresh 成功用例全红
 'skip-requestid-capture':source.replace('''            const s2a_json_field *idf = s2a_json_get(j, "requestId");
            if (idf && idf->kind == S2A_JSON_STRING)
                snprintf(request_id, id_cap, "%s", idf->text);''',
     '''            /* mutation: requestId never captured */ (void)id_cap;'''),
 'ignore-provider-generation':source.replace('live.generation == e->generation','1'),
 'accept-wrong-profile':source.replace('!string_is(j, "profileHash", e->profile)','0')}
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols
host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64' if sys.platform.startswith('linux') else None
if not host:
    raise RuntimeError('compute host test requires Linux or macOS')
ndk = link_symbols.find_ndk()
if ndk is None:
    raise RuntimeError('compute host test needs Android NDK 29.0.14206865; no download or skip')
vulkan = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include/vulkan'
if not (vulkan / 'vulkan.h').is_file():
    raise RuntimeError('NDK Vulkan headers missing: ' + str(vulkan))
with tempfile.TemporaryDirectory(prefix='w2-lookup-') as folder:
    td=Path(folder);inc=td/'include';inc.mkdir();(inc/'vulkan').symlink_to(vulkan)
    if (vulkan.parent/'vk_video').exists():(inc/'vk_video').symlink_to(vulkan.parent/'vk_video')
    (td/'good.json').write_text(json.dumps(good))
    for name,obj in variants.items():(td/(name+'.json')).write_text(json.dumps(obj))
    for name,src in mutations.items():
        if name!='positive':assert src!=source
        (td/'lookup.c').write_text(src);exe=td/name
        cmd=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
          '-I'+str(inc),'-I'+str(NATIVE),'-I'+str(NATIVE/'nvn'),'-I'+str(ROOT/'graphics/nvn/gfx/nvn'),'-I'+str(ROOT/'graphics/nvn/gfx/nvn/include'),str(Path(__file__).with_suffix('.c')),str(td/'lookup.c'),
          str(NATIVE/'nvn/nvn_vk_program_result.c'),str(NATIVE/'direct/jv.c'),str(NATIVE/'direct/json.c'),
          str(NATIVE/'direct/diag.c'),str(NATIVE/'direct/sha256.c'),'-lm','-o',str(exe)]
        subprocess.run(cmd,check=True)
        failures=[]
        for case,bad in cases:
            p=subprocess.run([str(exe),str(case),str(td/('true-alignment.json' if case==21 else 'good.json')),str(td/(bad+'.json'))],capture_output=True,text=True)
            if p.returncode:failures.append(case)
            if name=='positive':print(p.stdout+p.stderr,end='')
        if name=='positive':assert not failures,failures
        else:assert failures,(name,'negative control did not fail');print(name,'correctly red:',failures)
    # Producer contract: bytes from the real compiler CLI, not a hand-shaped
    # envelope. session/generation/requestId read back from the REQUEST file and
    # passed via env so any producer-side transport drift is caught here first.
    import shutil
    if shutil.which('dotnet'):
        spec2=importlib.util.spec_from_file_location('batch',Path(__file__).with_name('direct_batch_test.py'))
        b=importlib.util.module_from_spec(spec2);spec2.loader.exec_module(b)
        tests_dll,cli_dll,identity=b.built_tools()
        req_dir=td/'emit'
        b.checked(['dotnet',str(tests_dll),'--emit-requests',str(req_dir)])
        req=json.loads((req_dir/'compute.json').read_text())
        prof=json.loads((req_dir/'profile.json').read_text())
        assert req['translatorClosure']==identity['translatorClosure']
        real_dir=td/'compiled'
        b.checked(['dotnet',str(cli_dll),'compile','--request',str(req_dir/'compute.json'),'--output',str(real_dir)])
        real=(real_dir/'program.json')
        env=dict(os.environ,W2_REQ=str(req_dir/'compute.json'),W2_S=str(req['session']),
                 W2_G=str(req['generation']),W2_CLOSURE=req['translatorClosure'],
                 W2_PROFILE=prof['profileHash'],
                 W2_ALIGN=str(prof['capabilities']['storageBufferOffsetAlignment']))
        obj=json.loads(real.read_text())
        tampered=copy.deepcopy(obj);tampered['transport']['session']=tampered['transport']['session']+999
        (td/'real-bad-session.json').write_text(json.dumps(tampered))
        notrans=copy.deepcopy(obj);del notrans['transport']
        (td/'real-no-transport.json').write_text(json.dumps(notrans))
        forged=copy.deepcopy(obj);forged['transport']['requestId']='forged'
        (td/'real-bad-rid.json').write_text(json.dumps(forged))
        exe=td/'positive'
        for mode,bad_name in [(34,None),(35,'real-bad-session'),(36,'real-no-transport'),(37,'real-bad-rid'),(38,None)]:
            bad_arg=str(td/(bad_name+'.json')) if bad_name else str(td/'bad-stage.json')
            env['S2A_DIRECT_DIAG_DIR']=str(td/('diag'+str(mode)))
            p=subprocess.run([str(exe),str(mode),str(real),bad_arg],env=env,capture_output=True,text=True)
            assert p.returncode==0,(mode,p.stdout,p.stderr)
            print(p.stdout+p.stderr,end='')
            # 结果封套在 accept 之前落盘：通过的与被 transport 拒掉的都留实据；
            # mode38 调试门关——目录在位也一片不落（正式包行为）。
            results=list((td/('diag'+str(mode))).glob('*compile-result.json'))
            assert len(results)==(0 if mode==38 else 1),(mode,results)
        print('PASS producer contract: real CLI envelope accepted fresh; session/transport/requestId tamper rejected; rejected envelopes persisted')
    else:
        print('SKIP producer contract: dotnet is not installed')
    # No provider definition at link time, as in the two real engine=OFF targets.
    # An unavailable provider must reject before cache/worker access, not succeed
    # through a fabricated context or silently use an offline entry.
    # 未定义弱引用解析为空是 ELF / Android 链接语义：macOS ld64 不接受未定义弱引用（要放行得手写链接参数，仓库规定只能走 weak_link_decl 登记、
    # 而登记只收 native/ graphics/ 宿主源），所以这三例只在 Linux 宿主（安卓 Linux CTest）跑，Mac 上明说跳过
    if sys.platform != 'darwin':
        absent_cmd = [arg for arg in cmd]
        absent_cmd.insert(1, '-DW2_NO_PROVIDER')
        # 下面会把源文本里的 __attribute__((weak)) 去掉以制造“强依赖”表述；去掉后
        # `if (__android_log_print)` 变成恒真地址判断，-Waddress 只是变异体编译噪声，
        # 与本负控要证明的链接语义无关，单独关掉。
        absent_cmd.insert(2, '-Wno-address')
        (td/'lookup.c').write_text(source)
        absent_cmd[-1] = str(td/'no-provider')
        subprocess.run(absent_cmd, check=True)
        subprocess.run([absent_cmd[-1], '27', str(td/'good.json'), str(td/'bad-stage.json')], check=True)
        # Restore the original dependency: this must fail in the linker, not merely
        # warn about a function address always being true.
        strong = source.replace(' __attribute__((weak))', '')
        strong = strong.replace('s2a_direct_provider_compile_context && ', '')
        strong = strong.replace('    if (!s2a_direct_provider_compile_context)\n        return error(why, cap, "compute provider context unavailable");\n', '')
        assert strong != source
        (td/'lookup.c').write_text(strong)
        p = subprocess.run(absent_cmd, capture_output=True, text=True)
        assert p.returncode and 'undefined' in p.stderr.lower() and 's2a_direct_provider_compile_context' in p.stderr, p.stderr
        print('restore-strong-provider correctly red: linker undefined provider')
        # Keeping weak linkage but omitting both absence guards must not pass the
        # fail-closed assertion (normally a null-call signal on this host).
        unchecked = source.replace('s2a_direct_provider_compile_context && ', '')
        unchecked = unchecked.replace('    if (!s2a_direct_provider_compile_context)\n        return error(why, cap, "compute provider context unavailable");\n', '')
        assert unchecked != source
        (td/'lookup.c').write_text(unchecked)
        subprocess.run(absent_cmd, check=True)
        p = subprocess.run([absent_cmd[-1], '27', str(td/'good.json'), str(td/'bad-stage.json')], capture_output=True, text=True)
        assert p.returncode, 'unchecked absent provider unexpectedly succeeded'
        print('omit-provider-guards correctly red:', p.returncode)
    else:
        print('SKIP absent-provider / restore-strong / omit-guards: ELF weak-undefined semantics, Linux host only')
    # Real read-only container consumer: two distinct entries, then remove false.
    # IDs are controlled fixture keys here; canonical same-CS value distinction is
    # covered separately by direct_compute_identity_test and real producer evidence.
    import hashlib
    import struct
    exact=td/'exact-lookup'
    cmd=[os.environ.get('CC','cc'),'-D_GNU_SOURCE','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
         '-I'+str(inc),'-I'+str(NATIVE),'-I'+str(NATIVE/'nvn'),'-I'+str(ROOT/'graphics/nvn/gfx/nvn'),'-I'+str(ROOT/'graphics/nvn/gfx/nvn/include'),
         str(Path(__file__).with_name('w2_compute_exact_lookup_test.c')),
         str(NATIVE/'nvn/nvn_vk_compute_lookup.c'),str(NATIVE/'nvn/nvn_vk_program_result.c'),
         str(NATIVE/'direct/jv.c'),str(NATIVE/'direct/json.c'),str(NATIVE/'direct/offline_library.c'),str(NATIVE/'direct/sha256.c'),
         str(NATIVE/'direct/diag.c'),'-lm','-o',str(exact)]
    subprocess.run(cmd,check=True)
    revision=td/'revision';profile=good['content']['identity']['profileHash'];closure='closure'
    directory=revision/'shaders/vulkan/direct/1'/profile;directory.mkdir(parents=True)
    paths=[]
    for value,key in [(False,'11'*32),(True,'22'*32)]:
        obj=copy.deepcopy(good);obj['content']['hasUnalignedStorageBuffer']=value
        data=json.dumps(obj).encode();path=directory/(key+'.program');paths.append(path)
        path.write_bytes(b'S2APC1\n\0'+struct.pack('<II',2,88)+bytes.fromhex(key)+hashlib.sha256(data).digest()+struct.pack('<Q',len(data))+data)
        request=td/('request-'+str(value)+'.json')
        request.write_text(json.dumps({'program':{'computeState':{'hasUnalignedStorageBuffer':value}}}))
        subprocess.run([str(exact),str(revision),key,str(request),closure,profile,'1','16'],check=True)
    paths[0].unlink()
    subprocess.run([str(exact),str(revision),'11'*32,str(td/'request-False.json'),closure,profile,'0','16'],check=True)
    # Explicit no-live test policy: missing identity cannot invoke the worker;
    # the untouched other identity still hits. Normal fallback above remains.
    subprocess.run([str(exact),str(revision),'11'*32,str(td/'request-False.json'),closure,profile,'0','16','1'],check=True)
    subprocess.run([str(exact),str(revision),'22'*32,str(td/'request-True.json'),closure,profile,'1','16','1'],check=True)
    # Even if true bytes are falsely keyed under false, reflection mismatch rejects.
    data=json.dumps(variants['true-alignment']).encode()
    paths[0].write_bytes(b'S2APC1\n\0'+struct.pack('<II',2,88)+bytes.fromhex('11'*32)+hashlib.sha256(data).digest()+struct.pack('<Q',len(data))+data)
    subprocess.run([str(exact),str(revision),'11'*32,str(td/'request-False.json'),closure,profile,'0','16'],check=True)
    print('PASS real offline 6 cases: false/true hit, normal deleted miss, no-live deleted miss/no worker, other identity hit, mislabeled true rejected')
