#!/usr/bin/env python3
"""F5.2 宿主回归：直连离线程序库的生产端（direct_library.py）与出包接点。

覆盖：
  * 从捕获的 request/result 生成 <abi>/<profileHash>/<identity>.program + library.v1.json +
    graphics.v1.json，产物由同一套 verify 规则复检（容器头/身份/负载摘要/envelope/索引/文件清单）；
  * 负例：坏 SHA、缺文件、多文件、重复 identity、同 revision 原地重建、损坏容器、描述清单与
    index/字节不符、gameContentRoot/moduleSetDigest 不是从真实 index 推导；
  * 用运行时的 s2a_program_cache_load（program_cache.c）直接读生成的 .program，证明 F5.3 的
    读侧不需要新格式；用 native direct_graphics_test 读生成的描述，证明消费端接受；
  * make_pack.py / game_apk.py / attach_shaders.py 的三个必要接点。

不建 Vulkan 设备、不装 APK、不碰设备；真机消费仍属 F5.3 联合交付（本文件不宣布其完成）。
合成 request/result 只用于测试，不是运行时占位 shader；真实编译器对拍用
S2A_DIRECT_LIBRARY_REAL_COMPILER=1 打开（dotnet emit-requests + CLI compile）。
"""
import base64
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / 'tools'
REPO = HERE.parents[1]
sys.path.insert(0, str(TOOLS))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


direct_library = load('direct_library', TOOLS / 'direct_library.py')
make_pack = load('android_make_pack', TOOLS / 'make_pack.py')
game_apk = load('android_game_apk', TOOLS / 'game_apk.py')

TITLE = '0100000000ABCDEF'
PROFILE = 'sha256-' + 'a' * 64
SUPPORT = 'sha256-' + 'b' * 64
CLOSURE = 'synthetic-closure-v1'
COMPILER_ABI = 's2a-direct-compile-v1'
SPIRV = b'\x03\x02\x23\x07' + struct.pack('<IIII', 0x00010000, 0, 1, 0)
CC = shutil.which('cc') or shutil.which('clang')
REAL_COMPILER = os.environ.get('S2A_DIRECT_LIBRARY_REAL_COMPILER') == '1'


class StorageStageTest(unittest.TestCase):
    @staticmethod
    def valid_module(name):
        # Generated from the adjacent synthetic .vert with glslangValidator
        # 16.6.0; validated with spirv-val --target-env vulkan1.1.
        path = HERE / 'fixtures' / 'storage-stages' / (name + '.spv.hex')
        return bytes.fromhex(path.read_text())
    @staticmethod
    def module(readonly=True, atomic=False, model=0):
        def op(code, *args): return [(len(args) + 1) << 16 | code, *args]
        words = [0x07230203, 0x00010000, 0, 16, 0]
        words += op(15, model, 5, 0x6e69616d, 0)  # main
        words += op(22, 1, 32)  # float
        words += op(25, 2, 1, 1, 0, 0, 0, 2, 4)  # storage image
        words += op(32, 3, 0, 2) + op(59, 3, 4, 0)
        if readonly: words += op(71, 4, 24)  # NonWritable
        if atomic: words += op(227, 1, 5, 4, 0, 0)  # atomic load is still atomic
        return struct.pack('<%dI' % len(words), *words)

    def test_final_module_storage_and_atomic(self):
        self.assertFalse(direct_library.spirv_requires_storage_feature(self.module()))
        self.assertTrue(direct_library.spirv_requires_storage_feature(self.module(readonly=False)))
        self.assertTrue(direct_library.spirv_requires_storage_feature(self.module(atomic=True)))
        # A contradictory NonWritable decoration must not hide an actual store.
        def op(code, *args): return struct.pack('<%dI' % (len(args) + 1), (len(args) + 1) << 16 | code, *args)
        self.assertTrue(direct_library.spirv_requires_storage_feature(self.module() + op(65, 3, 6, 4) + op(62, 6, 1)))
        # Ordinary Output stores are not storage writes.
        self.assertFalse(direct_library.spirv_requires_storage_feature(
            self.module() + op(32, 7, 3, 1) + op(59, 7, 8, 3) + op(62, 8, 1)))
        with self.assertRaises(ValueError):
            direct_library.spirv_requires_storage_feature(self.module()[:-1])
        with self.assertRaises(ValueError):
            direct_library.spirv_requires_storage_feature(self.valid_module('fragment'), 0)

    def test_native_classifier_agrees(self):
        if not CC:
            self.skipTest('native compiler unavailable')
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / 'probe.c'
            src.write_text('''#include <stdio.h>\n#include <stdlib.h>\n#include "spirv_storage.h"\nint main(int n,char **v){if(n!=3)return 2;FILE*f=fopen(v[1],"rb");if(!f)return 3;unsigned char b[4096];size_t z=fread(b,1,sizeof b,f);fclose(f);int r=s2a_spirv_storage_need(b,z,(uint32_t)atoi(v[2]));printf("%d\\n",r);return 0;}\n''')
            binary = root / 'probe'
            subprocess.run([CC, '-Wall', '-Wextra', '-Werror', '-I', str(HERE.parent / 'native/direct'),
                            str(src), '-o', str(binary)], check=True)
            def op(code, *args): return struct.pack('<%dI' % (len(args) + 1), (len(args) + 1) << 16 | code, *args)
            for i, (data, expected) in enumerate(((self.module(), 0), (self.module(False), 1),
                                                   (self.module(atomic=True), 1), (self.module()[:-1], -1),
                                                   (self.module() + op(65, 3, 6, 4) + op(62, 6, 1), 1),
                                                   (self.module() + op(32, 7, 3, 1) + op(59, 7, 8, 3) + op(62, 8, 1), 0),
                                                   (self.valid_module('read'), 0), (self.valid_module('store'), 1),
                                                   (self.valid_module('atomic'), 1),
                                                   (self.valid_module('fragment'), -1))):
                module = root / str(i)
                module.write_bytes(data)
                result = subprocess.check_output([str(binary), str(module), '0'], text=True).strip()
                self.assertEqual(int(result), expected)
                if expected >= 0:
                    self.assertEqual(int(direct_library.spirv_requires_storage_feature(data, 0)), expected)
            # 前光栅其它阶段同样按各自 ExecutionModel 判；错传模型必须按畸形拒。
            for model, name in ((1, 'tcs'), (2, 'tes'), (3, 'gs')):
                for data, expected in ((self.module(readonly=False, model=model), 1),
                                       (self.module(model=model), 0),
                                       (self.module(model=model), -1)):
                    probe_model = model if expected >= 0 else 0
                    module.write_bytes(data)
                    result = subprocess.check_output([str(binary), str(module), str(probe_model)], text=True).strip()
                    self.assertEqual(int(result), expected, name)
                    if expected >= 0:
                        self.assertEqual(int(direct_library.spirv_requires_storage_feature(data, probe_model)), expected)
                    else:
                        with self.assertRaises(ValueError):
                            direct_library.spirv_requires_storage_feature(data, probe_model)

    def test_coverage_contract(self):
        parse = direct_library.parse_storage_requirements
        self.assertEqual(parse(None), {'schema': 1, 'writeStages': 0, 'coverage': 'unknown'})
        self.assertEqual(parse({'schema': 1, 'writeStages': 1, 'coverage': 'unknown'})['writeStages'], 1)
        for bad in ({'schema': 2, 'writeStages': 0, 'coverage': 'unknown'},
                    {'schema': 1, 'writeStages': 64, 'coverage': 'unknown'},
                    {'schema': 1, 'writeStages': 0, 'coverage': 'partial'}):
            with self.assertRaises(ValueError): parse(bad)

    def test_package_aggregates_scanned_stage_bits(self):
        result = {'content': {'stages': [
            {'stage': 'vs', 'spirv': base64.b64encode(self.module(False)).decode()},
            {'stage': 'fs', 'spirv': base64.b64encode(self.module(model=4)).decode()},
        ]}}
        self.assertEqual(direct_library.storage_stages_for_result(result), 1)
        result['content']['stages'][0]['spirv'] = base64.b64encode(self.module()).decode()
        self.assertEqual(direct_library.storage_stages_for_result(result), 0)

    def test_missing_feature_never_submits_vertex_module(self):
        if not CC: self.skipTest('native compiler unavailable')
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / 'vkprobe.c'
            src.write_text(r'''#include <stdio.h>
#include <stdlib.h>
#include "spirv_storage_vk.h"
static int submits;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *m){(void)d;(void)i;(void)a;(void)m;submits++;return VK_SUCCESS;}
int main(int n,char**v){if(n!=3)return 2;FILE*f=fopen(v[1],"rb");if(!f)return 3;unsigned char b[4096];size_t z=fread(b,1,sizeof b,f);fclose(f);VkShaderModuleCreateInfo ci={0};ci.codeSize=z;ci.pCode=(const uint32_t*)b;VkShaderModule m=VK_NULL_HANDLE;VkResult rc=s2a_vk_create_checked_vertex(VK_NULL_HANDLE,atoi(v[2]),&ci,&m,NULL);printf("%d %d\n",(int)rc,submits);return 0;}
''')
            binary = root / 'vkprobe'
            subprocess.run([CC, '-Wall', '-Wextra', '-Werror', '-I', str(HERE.parent / 'native/direct'),
                            '-I', str(HERE / 'vkhost'), '-I', str(REPO / 'graphics/nvn/third_party/vulkan-headers/include'),
                            str(src), '-o', str(binary)], check=True)
            shader = root / 'shader'; shader.write_bytes(self.valid_module('store'))
            result = subprocess.check_output([str(binary), str(shader), '0'], text=True).split()
            self.assertEqual(int(result[1]), 0, 'unsupported vertex module must never reach the driver')
            self.assertNotEqual(int(result[0]), 0)
            result = subprocess.check_output([str(binary), str(shader), '1'], text=True).split()
            self.assertEqual((int(result[0]), int(result[1])), (0, 1), 'supported device accepts vertex store')
            shader.write_bytes(self.valid_module('read'))
            result = subprocess.check_output([str(binary), str(shader), '0'], text=True).split()
            self.assertEqual((int(result[0]), int(result[1])), (0, 1), 'clean module reaches the driver')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def encode_container_for_test(identity_hex, payload):
    header = (b'S2APC1\n\x00' + struct.pack('<II', 2, 88) + bytes.fromhex(identity_hex)
              + bytes.fromhex(sha256(payload)) + struct.pack('<Q', len(payload)))
    return header + payload


def identity(seed):
    return sha256(b'capture-' + seed.encode())


def make_request(request_id=None, profile_hash=PROFILE, closure=CLOSURE, abi=1, kind='graphics'):
    names = ['vs', 'fs'] if kind == 'graphics' else ['cs']
    stages = []
    for index, name in enumerate(names):
        stages.append({
            'stage': name,
            # 设备请求里的 code 已去掉 0x30 图形头；这里只是可解码的非空字节
            'code': base64.b64encode(b'\x78\x56\x34\x12' + bytes(64)).decode(),
            'control': base64.b64encode(bytes(0x880)).decode(),
            'c1': '',
            'constantBufferUse': 1 if index == 0 else 0,
        })
    return {
        'schema': 's2a-direct-compile-v1',
        'session': 1,
        'generation': 1,
        'requestId': request_id or ('0123456789abcdef' * len(names)),
        'directABI': abi,
        'translatorClosure': closure,
        'runtimeCompilerABI': COMPILER_ABI,
        'sourceParserVersion': 1,
        'profile': {
            'schema': 's2a-direct-profile-v1',
            'profileHash': profile_hash,
            'directABI': abi,
            'translatorClosure': closure,
            'runtimeCompilerABI': COMPILER_ABI,
            'supportLayout': {'hash': SUPPORT},
        },
        'program': {
            'kind': kind,
            'stages': stages,
            'graphicsState': {},
            'textures': [],
            'samplerArrayLength': 1,
            'textureArrayLength': 1,
            'transformFeedbackEnabled': False,
        },
    }


def make_geometry_request():
    """v2 request (five NVN slots, null = unbound) for a vs+fs+gs program, as the device sends it for native geometry."""
    request = make_request()
    vs, fs = request['program']['stages']
    gs = dict(fs, constantBufferUse=2)
    gs['stage'] = 'gs'
    request['schema'] = 's2a-direct-compile-v2'
    request['program']['stages'] = [vs, fs, gs, None, None]
    request['requestId'] = '0123456789abcdef' * 3
    return request


def make_result(request, request_id=None, corrupt=None):
    kind = request['program']['kind']
    requested = {stage['stage']: stage for stage in request['program']['stages'] if stage is not None}
    cb_use = {name: stage['constantBufferUse'] for name, stage in requested.items()}
    # the compiler writes graphics stages in pipeline order (vs, gs, fs)
    order = [stage for stage in ('vs', 'gs', 'fs', 'cs') if stage in requested]
    stages = []
    for name in order:
        spirv_sha = '0' * 64 if corrupt == 'spirv_sha' else sha256(SPIRV)
        stage = requested[name]
        code_sha = sha256(base64.b64decode(stage['code']))
        control_sha = sha256(base64.b64decode(stage['control']))
        c1_sha = sha256(base64.b64decode(stage['c1'])) if stage['c1'] else sha256(b'')
        if corrupt == 'source_ref':
            code_sha = '0' * 64
        stages.append({
            'stage': name,
            'codeSource': 'code:sha256:' + code_sha,
            'controlSource': 'control:sha256:' + control_sha,
            'c1Source': 'c1:sha256:' + c1_sha,
            'constantBufferUse': cb_use[name],
            'spirv': base64.b64encode(SPIRV).decode(),
            'spirvSha256': spirv_sha,
            'info': {},
            'stateReadFlags': 0,
            'attributeReadMask': 0,
        })
    content = {
        'contract': 's2a-direct-program-content-v1',
        'identity': {
            'directABI': request['directABI'],
            'translatorClosure': request['translatorClosure'],
            'runtimeCompilerABI': request['runtimeCompilerABI'],
            'profileHash': request['profile']['profileHash'],
        },
        'kind': kind,
        'stages': stages,
        'bindings': [],
        'supportLayout': {'requiredSize': 16, 'binding': 0, 'hash': SUPPORT, 'fields': []},
        'dependencies': [],
    }
    body = json.dumps(content, separators=(',', ':')).encode()
    content_hash = 'sha256-' + sha256(body)
    if corrupt == 'content_hash':
        content_hash = 'sha256-' + '0' * 64
    if corrupt == 'content_bytes':
        body = body.replace(b'"bindings":[]', b'"bindings":[] ')
    transport = json.dumps({'session': 1, 'generation': 1,
                            'requestId': request_id or request['requestId']},
                           separators=(',', ':')).encode()
    return (b'{"schema":"s2a-direct-program-v1","transport":' + transport
            + b',"contentHash":"' + content_hash.encode() + b'","content":' + body + b'}')


class LibraryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        # describe 的闭包只取编译器构建身份（cli identity），不读手写常量
        self.identity = self.base / 'compiler-identity.json'
        self.identity.write_text(json.dumps({'schema': 's2a-direct-identity-v1', 'translatorClosure': CLOSURE}))
        self.capture = self.base / 'capture'
        self.lib = self.base / 'lib'
        self.descriptor = self.base / 'graphics.v1.json'
        self.base_pack = self.base / 'base-pack'
        self.entries = []
        self.write_base_pack()
        self.write_capture([(identity('one'), make_request(), make_result(make_request()))])

    # -- fixtures -------------------------------------------------------------------
    def module_images(self):
        return {
            'modules/main.patched.bin': b'synthetic module',
            'modules/main.fixups.tsv': b'# TARGET\tandroid\n# REWRITE_ABI\tandroid-sp-safe-v1\n',
            'modules/main.gates.tsv': b'# fixture\n',
        }

    def write_base_pack(self, extra_romfs=b'data', name='base-pack', extra_entries=()):
        """A minimal non-direct pack. Its index is the base the library's
        gameContentRoot/moduleSetDigest are derived from (two-pass packaging, as in §4.2).

        extra_entries 追加到同一份 index（库身份从这份 index 推，所以超大 index 用例要先在这里加行）。"""
        sources = self.base / ('sources-' + name)
        sources.mkdir(exist_ok=True)
        nacp = bytearray(0x4000)
        struct.pack_into('<Q', nacp, 0x3038, int(TITLE, 16))
        nacp[0x3060:0x3063] = b'0.1'
        files = {'ctl/control.nacp': bytes(nacp), 'romfs/data.bin': extra_romfs}
        files.update(self.module_images())
        entries = []
        for relative, data in files.items():
            path = sources / relative.replace('/', '_')
            path.write_bytes(data)
            entries.append((relative, str(path)))
        entries.extend(extra_entries)
        pack = self.base / name
        make_pack.write_pack(str(pack), entries, False, None)
        if name == 'base-pack':
            self.entries = entries
            self.base_pack = pack
        return pack, entries

    def write_capture(self, pairs):
        self.capture.mkdir(exist_ok=True)
        manifest = {'schema': 's2a-direct-capture-v1', 'directABI': 1,
                    'translatorClosure': CLOSURE, 'sourceParserVersion': 1,
                    'profileHash': PROFILE, 'entries': []}
        for index, (ident, request, result) in enumerate(pairs):
            request_name = f'r{index}.compile-request.json'
            result_name = f'r{index}.program.json'
            (self.capture / request_name).write_bytes(json.dumps(request).encode())
            (self.capture / result_name).write_bytes(result)
            manifest['entries'].append({'identity': ident, 'request': request_name,
                                        'result': result_name})
        (self.capture / 'capture.v1.json').write_bytes(
            (json.dumps(manifest, indent=1) + '\n').encode())
        return manifest

    def rows(self):
        return direct_library.read_index(self.base_pack / 'index.tsv')

    def build(self, out=None, descriptor=None, **overrides):
        return direct_library.build(self.capture, str(out or self.lib),
                                    str(descriptor or self.descriptor), TITLE,
                                    rows=overrides.pop('rows', self.rows()), **overrides)

    def load_descriptor(self, path=None):
        return direct_library.load_descriptor(path or self.descriptor)

    def write_direct_pack(self, out, library=None, descriptor=None):
        library = library or self.lib
        descriptor = descriptor or self.descriptor
        entries = list(self.entries)
        entries.append(('shaders/vulkan/direct', str(library)))
        entries.append((make_pack.GRAPHICS_MANIFEST, str(descriptor)))
        graphics = make_pack.load_graphics(str(descriptor))
        make_pack.write_pack(str(out), entries, False, graphics)
        return Path(out)

    def read_pack_rows(self, pack):
        return direct_library.read_index(Path(pack) / 'index.tsv')

    # -- 原生几何：v2 请求（五个 NVN 槽）与 vs / gs / fs 结果 -----------------------------
    def test_vertex_only_request_result(self):
        request = make_request(request_id='vertex-only')
        request['program']['stages'] = request['program']['stages'][:1]
        parsed = direct_library.validate_request(json.dumps(request).encode(), 'vertex request')
        self.assertEqual(parsed['_stages'], ['vs'])
        direct_library.validate_result(make_result(request), 'vertex result', request=parsed)
        request['requiresLinkedVaryingInterface'] = True
        with self.assertRaisesRegex(direct_library.LibraryError, 'VS/FS pair'):
            direct_library.validate_request(json.dumps(request).encode(), 'linked vertex')
        request.pop('requiresLinkedVaryingInterface')
        request['program']['stages'][0]['stage'] = 'fs'
        with self.assertRaisesRegex(direct_library.LibraryError, 'program.stages'):
            direct_library.validate_request(json.dumps(request).encode(), 'fragment only')
        with self.assertRaisesRegex(direct_library.LibraryError, 'content.stages'):
            direct_library.validate_result(make_result(request), 'fragment only')

    def test_geometry_program_request_result_and_build(self):
        request = make_geometry_request()
        parsed = direct_library.validate_request(json.dumps(request).encode(), 'gs request')
        self.assertEqual(parsed['_stages'], ['vs', 'fs', 'gs'])
        result = make_result(request)
        self.assertEqual([s['stage'] for s in json.loads(result)['content']['stages']], ['vs', 'gs', 'fs'])
        direct_library.validate_result(result, 'gs result', request=parsed)
        # 整条生产链：带几何的捕获对照常建库、复检
        self.write_capture([(identity('gs'), request, result)])
        built = self.build()
        self.assertEqual(built['entries'], 1)
        direct_library.verify_library(str(self.lib), self.load_descriptor())

    def test_geometry_program_negative_cases(self):
        good = make_geometry_request()
        def bad_request(mutate, pattern):
            request = json.loads(json.dumps(good))
            mutate(request)
            with self.assertRaisesRegex(direct_library.LibraryError, pattern):
                direct_library.validate_request(json.dumps(request).encode(), 'bad')
        # v1 请求没有几何（编译器只在 v2 收 gs）
        def v1_with_gs(r):
            r['schema'] = 's2a-direct-compile-v1'
            r['program']['stages'] = [s for s in r['program']['stages'] if s is not None]
        bad_request(v1_with_gs, "not one of vs/fs/cs")
        # v2 槽位与阶段名对不上（gs 放进 fs 槽）
        def swapped(r):
            st = r['program']['stages']
            st[1], st[2] = st[2], st[1]
        bad_request(swapped, 'does not match its NVN slot')
        # v2 不是五个槽 / 不是图形
        bad_request(lambda r: r['program']['stages'].pop(), 'five NVN slots')
        bad_request(lambda r: r['program'].__setitem__('kind', 'compute'), 'five NVN slots')
        # vs + gs 没有 fs
        bad_request(lambda r: r['program']['stages'].__setitem__(1, None), r"is not \['vs', 'fs'\] or")
        # 结果：阶段顺序不是 vs, gs, fs；结果缺 gs 而请求有
        parsed = direct_library.validate_request(json.dumps(good).encode(), 'gs request')
        result = json.loads(make_result(good))
        def raw():
            body = json.dumps(result['content'], separators=(',', ':')).encode()
            result['contentHash'] = 'sha256-' + sha256(body)
            return json.dumps(result, separators=(',', ':')).encode()
        st = result['content']['stages']
        st[1], st[2] = st[2], st[1]
        with self.assertRaisesRegex(direct_library.LibraryError, 'content.stages is'):
            direct_library.validate_result(raw(), 'order')
        st[1], st[2] = st[2], st[1]
        result['content']['stages'] = [s for s in st if s['stage'] != 'gs']
        with self.assertRaisesRegex(direct_library.LibraryError, 'are not the request stages'):
            direct_library.validate_result(raw(), 'missing gs', request=parsed)

    # -- 32 位 constantBufferUse 合同 ------------------------------------------------
    def test_constant_buffer_use_is_full_32_bit(self):
        # 编译器 DirectProgram.Parse 上限已到 0xFFFFFFFF：>=5 号 cbuf（NVN 用户槽 >=2）不再丢信息，
        # 离线库的请求 / 结果校验必须接受同一范围，并仍拒绝超过 32 位的值。
        request = make_request()
        request['program']['stages'][0]['constantBufferUse'] = 0x21  # cbuf 0 + cbuf 5
        validated = direct_library.validate_request(json.dumps(request).encode(), 'request')
        self.assertEqual(validated['_constantBufferUse']['vs'], 0x21)
        direct_library.validate_result(make_result(request), 'result', request=validated)
        too_big = make_request()
        too_big['program']['stages'][0]['constantBufferUse'] = 0x100000000
        with self.assertRaises(direct_library.LibraryError):
            direct_library.validate_request(json.dumps(too_big).encode(), 'request')

    def test_v2_separate_source_contract(self):
        request = make_request()
        result = json.loads(make_result(request))
        result['schema'] = 's2a-direct-program-v2'
        result['content']['contract'] = 's2a-direct-program-content-v2'
        result['content']['bindings'] = [{'stage': 'fs', 'kind': 'texture', 'set': 2,
                                           'binding': 0, 'slot': 90, 'samp_slot': 3, 'count': 1}]
        def raw():
            body = json.dumps(result['content'], separators=(',', ':')).encode()
            result['contentHash'] = 'sha256-' + sha256(body)
            return json.dumps(result, separators=(',', ':')).encode()
        direct_library.validate_result(raw(), 'v2')
        result['content']['bindings'][0]['samp_slot'] = 128
        with self.assertRaises(direct_library.LibraryError): direct_library.validate_result(raw(), 'bad slot')
        result['content']['bindings'][0]['samp_slot'] = 3
        result['content']['bindings'][0]['count'] = 2
        with self.assertRaises(direct_library.LibraryError): direct_library.validate_result(raw(), 'bad array')
        result['content']['bindings'][0]['count'] = 1
        result['schema'] = 's2a-direct-program-v1'
        result['content']['contract'] = 's2a-direct-program-content-v1'
        with self.assertRaises(direct_library.LibraryError): direct_library.validate_result(raw(), 'v1 extra source')

    def test_v2_const_sampler_contract(self):
        # #1594：类型 3 句柄的 constSampler（采样器池下标 0..4095）：与宿主解析同一份合同——v2、单个贴图；贴图字来源二选一：
        #   c2 分离贴图表（slot 90..345 偶数）或游戏 UBO（平铺 uboSlot 0..15 + uboOffset 4 的倍数，没有 slot）；
        #   不能同时有 samp_slot / uboHandle / sampler 对象；计算结果两种形态都收（c2 形态片 7、游戏 UBO 形态片 10，宿主计算侧同一份合同）。
        #   下标 0 与 4095 都是真下标。
        #   build 的 program_kind 是程序类型（graphics / compute）；fields 里的 kind 是绑定自己的种类（texture / uniform ...）。
        def build(program_kind='graphics', drop=(), **fields):
            request = make_request(kind=program_kind)
            result = json.loads(make_result(request))
            result['schema'] = 's2a-direct-program-v2'
            result['content']['contract'] = 's2a-direct-program-content-v2'
            binding = {'stage': 'fs' if program_kind == 'graphics' else 'cs', 'kind': 'texture', 'set': 2, 'binding': 0,
                       'slot': 90, 'constSampler': 257, 'count': 1}
            binding.update(fields)
            for key in drop:
                binding.pop(key, None)
            result['content']['bindings'] = [binding]
            def raw():
                body = json.dumps(result['content'], separators=(',', ':')).encode()
                result['contentHash'] = 'sha256-' + sha256(body)
                return json.dumps(result, separators=(',', ':')).encode()
            return result, raw
        # 接受：典型值、两端、最大贴图字
        for sampler in (257, 0, 4095):
            _, raw = build(constSampler=sampler)
            direct_library.validate_result(raw(), f'const sampler {sampler}')
        _, raw = build(slot=344)
        direct_library.validate_result(raw(), 'last separate texture word')
        # 拒绝：每一条都是宿主解析会拒的形态，库校验要在入库前同样拒掉
        bad = {
            'sampler above 12 bits': dict(constSampler=4096),
            'negative sampler': dict(constSampler=-1),
            'fractional sampler': dict(constSampler=1.5),
            'string sampler': dict(constSampler='7'),
            'boolean sampler': dict(constSampler=True),
            'odd texture word': dict(slot=91),
            'texture word before the table': dict(slot=88),
            'texture word after the table': dict(slot=346),
            'no texture word': dict(slot=None),
            'array': dict(count=2),
            'with samp_slot': dict(samp_slot=3),
            'with uboHandle': dict(uboHandle={'slot': 1, 'offset': 16}),
            'with sampler object': dict(sampler={'source': 'id', 'id': 3}),
            'c2 slot together with uboSlot': dict(uboSlot=1, uboOffset=16),
            'not a texture': dict(kind='uniform'),
        }
        for name, change in bad.items():
            result, raw = build(**{k: v for k, v in change.items() if v is not None})
            if 'slot' in change and change['slot'] is None:
                del result['content']['bindings'][0]['slot']
            with self.assertRaises(direct_library.LibraryError, msg=name):
                direct_library.validate_result(raw(), name)
        # v1 结果没有这个字段的合同
        result, raw = build()
        result['schema'] = 's2a-direct-program-v1'
        result['content']['contract'] = 's2a-direct-program-content-v1'
        with self.assertRaises(direct_library.LibraryError, msg='v1'): direct_library.validate_result(raw(), 'v1 const sampler')
        # 片 7：计算结果收 c2 形态——典型值、两端、最后一个贴图字；反例与图形的 c2 形态同一张表（宿主计算解析同样拒）
        for sampler in (257, 0, 4095):
            _, raw = build(program_kind='compute', constSampler=sampler)
            direct_library.validate_result(raw(), f'compute const sampler {sampler}')
        _, raw = build(program_kind='compute', slot=344)
        direct_library.validate_result(raw(), 'compute last separate texture word')
        for name, change in bad.items():
            result, raw = build(program_kind='compute', **{k: v for k, v in change.items() if v is not None})
            if 'slot' in change and change['slot'] is None:
                del result['content']['bindings'][0]['slot']
            with self.assertRaises(direct_library.LibraryError, msg=f'compute: {name}'):
                direct_library.validate_result(raw(), f'compute: {name}')
        result, raw = build(program_kind='compute')
        result['schema'] = 's2a-direct-program-v1'
        result['content']['contract'] = 's2a-direct-program-content-v1'
        with self.assertRaises(direct_library.LibraryError, msg='compute v1'): direct_library.validate_result(raw(), 'compute v1 const sampler')
        result, raw = build(program_kind='compute')
        del result['content']['bindings'][0]['constSampler']
        direct_library.validate_result(raw(), 'compute plain separate texture word')
        # 没有 constSampler 的同槽条目照旧接受（只改了带这个字段的）
        result, raw = build()
        del result['content']['bindings'][0]['constSampler']
        direct_library.validate_result(raw(), 'plain separate texture word')
        # 片 3：贴图字在游戏 UBO 里（平铺 uboSlot / uboOffset，没有 slot）。接受：典型值、两端
        for ubo, offset, sampler in ((2, 160, 260), (0, 0, 0), (15, 65532, 4095)):
            _, raw = build(drop=('slot',), uboSlot=ubo, uboOffset=offset, constSampler=sampler)
            direct_library.validate_result(raw(), f'UBO const sampler {ubo}/{offset}/{sampler}')
        # 拒绝：每一条都是宿主解析会拒的形态
        bad_ubo = {
            'uboSlot after the last UBO': dict(uboSlot=16),
            'negative uboSlot': dict(uboSlot=-1),
            'fractional uboSlot': dict(uboSlot=1.5),
            'uboOffset not a word offset': dict(uboOffset=6),
            'uboOffset above 64 KiB': dict(uboOffset=65536),
            'negative uboOffset': dict(uboOffset=-4),
            'sampler above 12 bits': dict(constSampler=4096),
            'array': dict(count=2),
            'with samp_slot': dict(samp_slot=3),
            'with uboHandle': dict(uboHandle={'slot': 1, 'offset': 16}),
            'with sampler object': dict(sampler={'source': 'id', 'id': 3}),
            'with c2 slot': dict(slot=90),
            'not a texture': dict(kind='uniform'),
        }
        for name, change in bad_ubo.items():
            _, raw = build(drop=() if 'slot' in change else ('slot',), **{'uboSlot': 2, 'uboOffset': 160, **change})
            with self.assertRaises(direct_library.LibraryError, msg=f'UBO form: {name}'):
                direct_library.validate_result(raw(), f'UBO form: {name}')
        _, raw = build(drop=('slot', 'uboOffset'), uboSlot=2)
        with self.assertRaises(direct_library.LibraryError, msg='UBO form: no uboOffset'): direct_library.validate_result(raw(), 'UBO form: no uboOffset')
        # 片 10：计算结果也收游戏 UBO 形态（宿主计算解析同一份合同）——典型值、两端；反例与图形的 UBO 形态同一张表
        for ubo, offset, sampler in ((2, 160, 260), (0, 0, 0), (15, 65532, 4095)):
            _, raw = build(program_kind='compute', drop=('slot',), uboSlot=ubo, uboOffset=offset, constSampler=sampler)
            direct_library.validate_result(raw(), f'compute UBO const sampler {ubo}/{offset}/{sampler}')
        for name, change in bad_ubo.items():
            _, raw = build(program_kind='compute', drop=() if 'slot' in change else ('slot',), **{'uboSlot': 2, 'uboOffset': 160, **change})
            with self.assertRaises(direct_library.LibraryError, msg=f'compute UBO form: {name}'):
                direct_library.validate_result(raw(), f'compute UBO form: {name}')
        _, raw = build(program_kind='compute', drop=('slot', 'uboOffset'), uboSlot=2)
        with self.assertRaises(direct_library.LibraryError, msg='compute UBO form: no uboOffset'): direct_library.validate_result(raw(), 'compute UBO form: no uboOffset')
        result, raw = build(program_kind='compute', drop=('slot',), uboSlot=2, uboOffset=160)
        result['schema'] = 's2a-direct-program-v1'
        result['content']['contract'] = 's2a-direct-program-content-v1'
        with self.assertRaises(direct_library.LibraryError, msg='compute UBO form: v1'): direct_library.validate_result(raw(), 'compute UBO form: v1')
        result, raw = build(drop=('slot',), uboSlot=2, uboOffset=160)
        result['schema'] = 's2a-direct-program-v1'
        result['content']['contract'] = 's2a-direct-program-content-v1'
        with self.assertRaises(direct_library.LibraryError, msg='UBO form: v1'): direct_library.validate_result(raw(), 'UBO form: v1')

    # -- build / layout -------------------------------------------------------------
    def test_descriptor_never_omits_a_scanned_vertex_write(self):
        request = make_request()
        result = json.loads(make_result(request))
        spv = StorageStageTest.module(readonly=False)
        stage = result['content']['stages'][0]
        stage['spirv'] = base64.b64encode(spv).decode()
        stage['spirvSha256'] = sha256(spv)
        frag = StorageStageTest.module(model=4)
        result['content']['stages'][1]['spirv'] = base64.b64encode(frag).decode()
        result['content']['stages'][1]['spirvSha256'] = sha256(frag)
        body = json.dumps(result['content'], separators=(',', ':')).encode()
        result['contentHash'] = 'sha256-' + sha256(body)
        self.write_capture([(identity('write'), request, json.dumps(result, separators=(',', ':')).encode())])
        self.build()
        doc = json.loads(self.descriptor.read_text())
        self.assertEqual(doc['shaderStorageRequirements'], {'schema': 1, 'writeStages': 1, 'coverage': 'unknown'})
        doc['shaderStorageRequirements']['writeStages'] = 0
        self.descriptor.write_text(json.dumps(doc))
        with self.assertRaisesRegex(direct_library.LibraryError, 'omits scanned write stages'):
            direct_library.verify_library(str(self.lib), self.load_descriptor())

    def test_describe_rejects_library_from_different_translator(self):
        other_closure = 'older-translator-v1'
        request = make_request(closure=other_closure)
        self.write_capture([(identity('old'), request, make_result(request))])
        capture = json.loads((self.capture / 'capture.v1.json').read_text())
        capture['translatorClosure'] = other_closure
        (self.capture / 'capture.v1.json').write_text(json.dumps(capture))
        self.build()
        pack = self.write_direct_pack(self.base / 'old-translator-pack')
        self.assertEqual(direct_library.verify_pack(str(pack))['library']['entries'], 1)
        output = self.base / 'stale-descriptor.json'
        with self.assertRaisesRegex(direct_library.LibraryError, 'translator closure'):
            direct_library.describe_pack(str(pack), str(output), str(self.identity))
        self.assertFalse(output.exists())

    def test_multiple_profile_files_validate_against_their_own_directory(self):
        self.build()
        program_a = next(self.lib.rglob('*.program'))
        profile_b = 'sha256-' + 'c' * 64
        request_b = make_request(profile_hash=profile_b)
        self.write_capture([(identity('two'), request_b, make_result(request_b))])
        capture = json.loads((self.capture / 'capture.v1.json').read_text()); capture['profileHash'] = profile_b
        (self.capture / 'capture.v1.json').write_text(json.dumps(capture))
        other_lib = self.base / 'lib-b'; other_desc = self.base / 'desc-b.json'
        self.build(out=other_lib, descriptor=other_desc)
        program_b = next(other_lib.rglob('*.program'))
        doc = json.loads(self.descriptor.read_text())
        doc['profiles'] = [{'profileHash': PROFILE}, {'profileHash': profile_b}]
        rel_a = f'{direct_library.DIRECT_PREFIX}1/{PROFILE}/{program_a.name}'
        rel_b = f'{direct_library.DIRECT_PREFIX}1/{profile_b}/{program_b.name}'
        doc['files'] = [{'path': rel, 'size': p.stat().st_size, 'sha256': sha256(p.read_bytes())}
                        for rel, p in ((rel_a, program_a), (rel_b, program_b))]
        descriptor = self.base / 'multi.json'; descriptor.write_text(json.dumps(doc))
        entries = list(self.entries) + [(rel_a, str(program_a)), (rel_b, str(program_b)),
                                       (make_pack.GRAPHICS_MANIFEST, str(descriptor))]
        out = self.base / 'multi-pack'
        make_pack.write_pack(str(out), entries, False, make_pack.load_graphics(str(descriptor)))
        self.assertIsNone(direct_library.verify_pack(str(out))['library'])
        # Valid B bytes placed under A's profile must not pass just because B is declared.
        bad = direct_library.load_descriptor(str(descriptor))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.validate_result(make_result(request_b), 'bad folder', descriptor=bad,
                                           expected_profile=PROFILE)

    def test_empty_library_descriptor_and_profile_contract(self):
        target = self.base / 'empty.json'
        direct_library.describe_pack(str(self.base_pack), str(target), str(self.identity))
        empty = direct_library.load_descriptor(str(target))
        self.assertEqual(empty['doc']['files'], [])
        self.assertEqual(empty['doc']['profiles'], [])
        self.assertEqual(empty['translator_closure'], CLOSURE)
        self.assertEqual(empty['game_content_root'], direct_library.index_content_root(self.rows()))
        self.assertEqual(empty['module_set_digest'], direct_library.index_module_digest(self.rows()))
        output = self.base / 'empty-direct-pack'
        entries = list(self.entries) + [(make_pack.GRAPHICS_MANIFEST, str(target))]
        make_pack.write_pack(str(output), entries, False, make_pack.load_graphics(str(target)))
        self.assertIsNone(direct_library.verify_pack(str(output))['library'])
        doc = dict(empty['doc']); doc['profiles'] = [{'profileHash': PROFILE}]
        invalid = self.base / 'invalid-empty.json'; invalid.write_text(json.dumps(doc))
        with self.assertRaises(direct_library.LibraryError): direct_library.load_descriptor(str(invalid))
        self.build()
        doc = json.loads(self.descriptor.read_text())
        doc['profiles'] = [{'profileHash': 'sha256-' + 'c' * 64}]
        mismatch = self.base / 'profile-mismatch.json'; mismatch.write_text(json.dumps(doc))
        with self.assertRaises(direct_library.LibraryError): direct_library.load_descriptor(str(mismatch))
        doc['profiles'] = [{'profileHash': PROFILE}, {'profileHash': 'sha256-' + 'c' * 64}]
        mismatch.write_text(json.dumps(doc))
        with self.assertRaises(direct_library.LibraryError): direct_library.load_descriptor(str(mismatch))

    def test_describe_existing_library_from_readonly_pack(self):
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')
        before = {str(p.relative_to(pack)): sha256(p.read_bytes()) for p in pack.rglob('*') if p.is_file()}
        target = self.base / 'derived.json'
        direct_library.describe_pack(str(pack), str(target), str(self.identity))
        self.assertEqual(json.loads(target.read_text()), json.loads(self.descriptor.read_text()))
        self.assertEqual(before, {str(p.relative_to(pack)): sha256(p.read_bytes()) for p in pack.rglob('*') if p.is_file()})
        direct_library.verify_library(str(pack / direct_library.DIRECT_PREFIX), direct_library.load_descriptor(str(target)))
        cli_target = self.base / 'derived-cli.json'
        command = subprocess.run([sys.executable, str(TOOLS / 'direct_library.py'), 'describe',
                                  '--pack', str(pack), '--identity', str(self.identity), '--descriptor', str(cli_target)], capture_output=True, text=True)
        self.assertEqual(command.returncode, 0, command.stderr)
        self.assertEqual(cli_target.read_bytes(), target.read_bytes())
        with self.assertRaises(direct_library.LibraryError):
            direct_library.describe_pack(str(pack), str(pack / 'bad.json'), str(self.identity))
        control = pack / 'ctl/control.nacp'
        original_control = control.read_bytes(); control.write_bytes(b'changed')
        with self.assertRaises(direct_library.LibraryError):
            direct_library.describe_pack(str(pack), str(self.base / 'bad-control.json'), str(self.identity))
        self.assertFalse((self.base / 'bad-control.json').exists())
        control.write_bytes(original_control)
        (pack / direct_library.DIRECT_PREFIX / 'library.v1.json').write_bytes(b'corrupt')
        with self.assertRaises(direct_library.LibraryError):
            direct_library.describe_pack(str(pack), str(self.base / 'bad.json'), str(self.identity))
        self.assertFalse((self.base / 'bad.json').exists())

    def test_build_writes_the_contract_layout_and_verifies(self):
        result = self.build()
        self.assertEqual(result['entries'], 1)
        entry = next(self.lib.rglob('*.program'))
        self.assertEqual(entry.parent,
                         self.lib / '1' / PROFILE)
        self.assertEqual(entry.name, identity('one') + '.program')
        descriptor = self.load_descriptor()
        self.assertEqual(descriptor['title_id'], TITLE)
        self.assertEqual(descriptor['direct_abi'], 1)
        self.assertEqual(descriptor['profile_hashes'], [PROFILE])
        self.assertEqual(descriptor['translator_closure'], CLOSURE)
        self.assertEqual(descriptor['source_parser_version'], 1)
        self.assertEqual(descriptor['game_content_root'], result['gameContentRoot'])
        self.assertEqual(descriptor['module_set_digest'], result['moduleSetDigest'])
        paths = [item['path'] for item in descriptor['files']]
        self.assertIn('shaders/vulkan/direct/library.v1.json', paths)
        self.assertIn('shaders/vulkan/direct/1/' + PROFILE + '/' + identity('one') + '.program', paths)
        index = json.loads((self.lib / 'library.v1.json').read_text())
        self.assertEqual(index['schema'], 's2a-direct-library-v1')
        self.assertEqual(index['entries'][0]['identity'], identity('one'))
        self.assertEqual(index['entries'][0]['path'], '1/' + PROFILE + '/' + identity('one') + '.program')
        self.assertEqual(index['entries'][0]['resultSha256'],
                         sha256(make_result(make_request())))
        # 产物再验一次（库内 + 出包后）
        direct_library.verify_library(str(self.lib), descriptor)
        pack = self.write_direct_pack(self.base / 'pack')
        checked = direct_library.verify_pack(str(pack))
        self.assertEqual(checked['library']['entries'], 1)
        self.assertEqual(checked['gameContentRoot'], result['gameContentRoot'])
        # CLI 面也走一遍：verify --lib/--pack 返回 0，退出码能当门禁用。
        for arguments in (['verify', '--lib', str(self.lib), '--descriptor', str(self.descriptor)],
                          ['verify', '--lib', str(self.lib), '--descriptor', str(self.descriptor),
                           '--index', str(pack / 'index.tsv')],
                          ['verify', '--pack', str(pack)]):
            completed = subprocess.run([sys.executable, str(TOOLS / 'direct_library.py'), *arguments],
                                       capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_build_is_deterministic_and_immutable(self):
        first = self.build()
        second = self.build(out=self.base / 'lib2', descriptor=self.base / 'graphics2.v1.json')
        self.assertEqual((self.lib / 'library.v1.json').read_bytes(),
                         (self.base / 'lib2' / 'library.v1.json').read_bytes())
        self.assertEqual(self.descriptor.read_bytes(),
                         (self.base / 'graphics2.v1.json').read_bytes())
        self.assertEqual(first['gameContentRoot'], second['gameContentRoot'])
        before = {path.relative_to(self.lib): path.read_bytes()
                  for path in self.lib.rglob('*') if path.is_file()}
        with self.assertRaises(direct_library.LibraryError):
            self.build()
        self.assertFalse((self.base / ('lib.tmp-' + str(os.getpid()))).exists(),
                         'a refused rebuild must not leave a staging directory')
        after = {path.relative_to(self.lib): path.read_bytes()
                 for path in self.lib.rglob('*') if path.is_file()}
        self.assertEqual(before, after, 'an existing library must never be edited in place')

    def test_multiple_entries_keep_one_profile(self):
        first = make_request()
        second = make_request(request_id='fedcba9876543210' * 2)
        self.write_capture([(identity('two'), first, make_result(first)),
                            (identity('one'), second, make_result(second))])
        result = self.build()
        self.assertEqual(result['entries'], 2)
        index = json.loads((self.lib / 'library.v1.json').read_text())
        self.assertEqual([entry['identity'] for entry in index['entries']],
                         sorted([identity('one'), identity('two')]))
        descriptor = self.load_descriptor()
        self.assertEqual(len(descriptor['files']), 3)  # two programs + library.v1.json
        direct_library.verify_library(str(self.lib), descriptor)
        pack = self.write_direct_pack(self.base / 'pack-multi')
        self.assertEqual(direct_library.verify_pack(str(pack))['library']['entries'], 2)

    def test_unknown_capture_identity_is_rejected(self):
        # 捕获清单里的 abi/closure 必须与请求一致
        manifest = json.loads((self.capture / 'capture.v1.json').read_text())
        manifest['translatorClosure'] = 'other-closure-v1'
        (self.capture / 'capture.v1.json').write_text(json.dumps(manifest))
        with self.assertRaises(direct_library.LibraryError):
            self.build()

    def test_capture_negative_cases(self):
        base_request, base_result = make_request(), make_result(make_request())
        cases = {
            'duplicate identity': [(identity('dup'), base_request, base_result),
                                   (identity('dup'), base_request, base_result)],
            'bad identity': [('ABC', base_request, base_result)],
            'result for another request': [(identity('mismatch'), base_request,
                                            make_result(base_request, request_id='f' * 32))],
            'tampered content hash': [(identity('hash'), base_request,
                                       make_result(base_request, corrupt='content_hash'))],
            'tampered content bytes': [(identity('bytes'), base_request,
                                        make_result(base_request, corrupt='content_bytes'))],
            'tampered spirv digest': [(identity('spirv'), base_request,
                                       make_result(base_request, corrupt='spirv_sha'))],
            'result built from other source bytes': [(identity('source'), base_request,
                                                     make_result(base_request, corrupt='source_ref'))],
        }
        for label, pairs in cases.items():
            with self.subTest(label):
                shutil.rmtree(self.capture)
                self.write_capture(pairs)
                slug = label.replace(' ', '-')
                with self.assertRaises(direct_library.LibraryError):
                    self.build(out=self.base / ('lib-' + slug),
                               descriptor=self.base / (slug + '.json'))

    def test_capture_files_must_not_escape(self):
        manifest = json.loads((self.capture / 'capture.v1.json').read_text())
        manifest['entries'][0]['request'] = '../escape.json'
        (self.capture / 'capture.v1.json').write_text(json.dumps(manifest))
        with self.assertRaises(direct_library.LibraryError):
            self.build()

    def test_mixed_profiles_are_rejected(self):
        first, second = make_request(), make_request(profile_hash=PROFILE[:-1] + 'c')
        self.write_capture([(identity('a'), first, make_result(first)),
                            (identity('b'), second, make_result(second))])
        with self.assertRaises(direct_library.LibraryError):
            self.build()

    # -- verification negatives ------------------------------------------------------
    def corrupt(self, mutate):
        self.build()
        mutate(self.lib)
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_library(str(self.lib), self.load_descriptor())

    def program_path(self):
        return next(self.lib.rglob('*.program'))

    def test_verify_rejects_corrupt_payload(self):
        def flip(root):
            path = next(root.rglob('*.program'))
            data = bytearray(path.read_bytes())
            data[100] ^= 0x01
            path.write_bytes(data)
        self.corrupt(flip)

    def test_verify_rejects_truncated_container(self):
        def truncate(root):
            path = next(root.rglob('*.program'))
            data = path.read_bytes()
            path.write_bytes(data[:-8])
        self.corrupt(truncate)

    def test_verify_rejects_container_with_wrong_identity(self):
        def rename(root):
            path = next(root.rglob('*.program'))
            path.rename(path.with_name('f' * 64 + '.program'))
        self.corrupt(rename)

    def test_verify_rejects_missing_file(self):
        def remove(root):
            next(root.rglob('*.program')).unlink()
        self.corrupt(remove)

    def test_verify_rejects_extra_file(self):
        def add(root):
            (root / '1' / PROFILE / 'stray.bin').write_bytes(b'stray')
        self.corrupt(add)

    def test_verify_rejects_index_sha_mismatch(self):
        def rewrite(root):
            index_path = root / 'library.v1.json'
            index = json.loads(index_path.read_text())
            index['entries'][0]['resultSha256'] = '0' * 64
            index_path.write_text(json.dumps(index, indent=1) + '\n')
        # 索引自身的 SHA 也被描述绑定，改索引会先在“列出的文件字节不符”处失败
        self.corrupt(rewrite)

    def test_verify_rejects_stale_descriptor(self):
        self.build()
        (self.lib / 'graphics.v1.json').write_bytes(b'{"schema":"s2a-graphics-v1"}')
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_library(str(self.lib), self.load_descriptor())

    def test_descriptor_file_entry_rules(self):
        self.build()
        doc = json.loads(self.descriptor.read_text())
        good_files = doc['files']

        def bad(label, files, **overrides):
            changed = dict(doc, files=files, **overrides)
            path = self.base / (label + '.json')
            path.write_text(json.dumps(changed))
            with self.assertRaises(direct_library.LibraryError):
                direct_library.load_descriptor(str(path))

        entry = dict(good_files[0])
        bad('absolute', ['/etc/passwd'])
        bad('traversal', [dict(entry, path='shaders/vulkan/direct/../../x')])
        bad('outside direct', [dict(entry, path='shaders/vulkan/other/x.program')])
        bad('descriptor itself', [dict(entry, path=make_pack.GRAPHICS_MANIFEST)])
        bad('missing sha', [{'path': entry['path']}])
        bad('uppercase sha', [dict(entry, sha256=entry['sha256'].upper())])
        bad('unknown key', [dict(entry, mode='fast')])
        bad('boolean size', [dict(entry, size=True)])
        bad('duplicate', [entry, entry])

    # -- pack-level verification -----------------------------------------------------
    def refresh_pack(self, pack_root):
        """Recompute index/manifest/artifact from the current directory (test helper)."""
        rows = []
        for path in sorted(Path(pack_root).rglob('*')):
            if not path.is_file():
                continue
            if path.is_symlink():
                continue
            relative = path.relative_to(pack_root).as_posix()
            if relative in ('index.tsv', 'manifest.json', 'game-build-artifact.json'):
                continue
            data = path.read_bytes()
            rows.append((relative, len(data), sha256(data)))
        index = ''.join(f'{p}\t{s}\t{d}\n' for p, s, d in rows).encode()
        (Path(pack_root) / 'index.tsv').write_bytes(index)
        index_sha = sha256(index)
        manifest = json.loads((Path(pack_root) / 'manifest.json').read_text())
        manifest.update(files=len(rows), total=sum(s for _, s, _ in rows), index_sha256=index_sha)
        (Path(pack_root) / 'manifest.json').write_text(json.dumps(manifest, indent=1))
        descriptor_bytes = (Path(pack_root) / direct_library.DESCRIPTOR_REL).read_bytes()
        artifact = json.loads((Path(pack_root) / 'game-build-artifact.json').read_text())
        artifact['contentRoot'] = 'sha256-' + index_sha
        artifact['package'] = dict(artifact.get('package', {}), indexSha256=index_sha,
                                  files=len(rows), bytes=sum(s for _, s, _ in rows))
        artifact['extensions'][direct_library.GRAPHICS_FEATURE] = {
            'manifest': direct_library.DESCRIPTOR_REL,
            'sha256': sha256(descriptor_bytes), 'bytes': len(descriptor_bytes)}
        (Path(pack_root) / 'game-build-artifact.json').write_text(json.dumps(artifact, indent=1) + '\n')
        return descriptor_bytes

    def test_verify_pack_negative_cases(self):
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')

        def copy_pack(name):
            target = self.base / name
            shutil.copytree(pack, target)
            return target

        # 缺文件（index 仍列着它）
        missing = copy_pack('missing')
        next(missing.rglob('*.program')).unlink()
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(missing))

        # 多文件（未列出的直连文件）
        extra = copy_pack('extra')
        (extra / 'shaders/vulkan/direct/1' / PROFILE / 'unlisted.bin').write_bytes(b'x')
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(extra))

        # 描述里列的 SHA 与 index 行不符（重写 index/manifest/artifact 让它们自洽）
        mismatched = copy_pack('mismatched')
        descriptor_doc = json.loads((mismatched / direct_library.DESCRIPTOR_REL).read_text())
        descriptor_doc['files'][0]['sha256'] = '0' * 64
        (mismatched / direct_library.DESCRIPTOR_REL).write_text(json.dumps(descriptor_doc, indent=1) + '\n')
        self.refresh_pack(mismatched)
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(mismatched))

        # 身份字段不是从 index 推导（用另一份 base 的 gameContentRoot 出包）
        other_base, other_entries = self.write_base_pack(extra_romfs=b'different data', name='other-base')
        rows2 = direct_library.read_index(other_base / 'index.tsv')
        self.build(out=self.base / 'lib2', descriptor=self.base / 'graphics-other.json', rows=rows2)
        mismatched_roots = self.base / 'mismatched-roots'
        entries = list(self.entries) + [('shaders/vulkan/direct', str(self.base / 'lib2')),
                                        (make_pack.GRAPHICS_MANIFEST, str(self.base / 'graphics-other.json'))]
        make_pack.write_pack(str(mismatched_roots), entries, False,
                             make_pack.load_graphics(str(self.base / 'graphics-other.json')))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(mismatched_roots))

        # artifact 的扩展摘要与描述文件不符
        tampered = copy_pack('tampered')
        artifact = json.loads((tampered / 'game-build-artifact.json').read_text())
        artifact['extensions'][direct_library.GRAPHICS_FEATURE]['sha256'] = 'f' * 64
        (tampered / 'game-build-artifact.json').write_text(json.dumps(artifact, indent=1) + '\n')
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(tampered))

        # 描述本身没有进 index
        unindexed = copy_pack('unindexed')
        (unindexed / direct_library.DESCRIPTOR_REL).unlink()
        (unindexed / direct_library.DESCRIPTOR_REL).write_bytes(b'{}')  # 文件仍在，但内容变了
        self.refresh_pack(unindexed)
        rows = [row for row in self.read_pack_rows(unindexed)
                if row[0] != direct_library.DESCRIPTOR_REL]
        index = ''.join(f'{p}\t{s}\t{d}\n' for p, s, d in rows).encode()
        (unindexed / 'index.tsv').write_bytes(index)
        manifest = json.loads((unindexed / 'manifest.json').read_text())
        manifest.update(index_sha256=sha256(index), files=len(rows), total=sum(s for _, s, _ in rows))
        (unindexed / 'manifest.json').write_text(json.dumps(manifest, indent=1))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(unindexed))

    def test_verify_pack_checks_containers_without_the_library_index(self):
        """删掉库索引不能让内容校验失效：容器/信封仍要逐条验。"""
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')
        (pack / 'shaders/vulkan/direct' / direct_library.LIBRARY_INDEX_REL).unlink()
        descriptor_path = pack / direct_library.DESCRIPTOR_REL
        doc = json.loads(descriptor_path.read_text())
        doc['files'] = [entry for entry in doc['files']
                        if not entry['path'].endswith('/' + direct_library.LIBRARY_INDEX_REL)]
        descriptor_path.write_text(json.dumps(doc, indent=1) + '\n')
        self.refresh_pack(pack)
        direct_library.verify_pack(str(pack))   # 无索引但容器合法：仍可通过
        program = next((pack / 'shaders/vulkan/direct').rglob('*.program'))
        data = bytearray(program.read_bytes())
        data[100] ^= 0x01
        program.write_bytes(data)
        self.refresh_pack(pack)                 # 外部哈希都自洽，只有容器解析能发现
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(pack))

    def test_make_pack_rejects_a_corrupt_container(self):
        """出包器也要证明列出的 program 真是容器，而不只是哈希对得上。"""
        self.build()
        bogus = self.base / 'bogus-lib'
        (bogus / '1' / PROFILE).mkdir(parents=True)
        program = bogus / '1' / PROFILE / (identity('one') + '.program')
        program.write_bytes(b'not a S2APC1 container')
        doc = json.loads(self.descriptor.read_text())
        doc['files'] = [{'path': 'shaders/vulkan/direct/' + program.relative_to(bogus).as_posix(),
                         'size': program.stat().st_size,
                         'sha256': sha256(program.read_bytes())}]
        bogus_descriptor = self.base / 'bogus-graphics.json'
        bogus_descriptor.write_text(json.dumps(doc, indent=1) + '\n')
        with self.assertRaises(SystemExit):
            self.pack_from(self.base / 'pack-bogus', bogus, bogus_descriptor)
        self.assertFalse((self.base / 'pack-bogus' / 'manifest.json').exists())

        # 合法容器放在错误的 <abi>/<profileHash> 目录也拒绝（运行时按自己的目录派生）
        wrong = self.base / 'wrong-lib'
        wrong_dir = wrong / '9' / ('sha256-' + '9' * 64)
        wrong_dir.mkdir(parents=True)
        wrong_program = wrong_dir / program.name
        wrong_program.write_bytes(encode_container_for_test(identity('one'), make_result(make_request())))
        doc = json.loads(self.descriptor.read_text())
        doc['files'] = [{'path': 'shaders/vulkan/direct/' + wrong_program.relative_to(wrong).as_posix(),
                         'size': wrong_program.stat().st_size,
                         'sha256': sha256(wrong_program.read_bytes())}]
        wrong_descriptor = self.base / 'wrong-graphics.json'
        wrong_descriptor.write_text(json.dumps(doc, indent=1) + '\n')
        with self.assertRaises(SystemExit):
            self.pack_from(self.base / 'pack-wrong', wrong, wrong_descriptor)

    def test_verify_pack_rejects_links_ghost_rows_and_a_wrong_title(self):
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')

        # 直连目录被换成指向库外的符号链接：列出的字节仍相符，但枚举/包含性必须拒绝
        linked = self.base / 'linked'
        shutil.copytree(pack, linked)
        outside = self.base / 'outside-profile'
        shutil.move(str(linked / 'shaders/vulkan/direct/1'), str(outside))
        os.symlink(str(outside), str(linked / 'shaders/vulkan/direct/1'))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(linked))

        # 整个 direct 根是链接（内容完整、哈希不变）也必须拒绝
        linked_root = self.base / 'linked-root'
        shutil.copytree(pack, linked_root)
        outside_root = self.base / 'outside-direct'
        shutil.move(str(linked_root / 'shaders/vulkan/direct'), str(outside_root))
        os.symlink(str(outside_root), str(linked_root / 'shaders/vulkan/direct'))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(linked_root))

        # 列出的 program 本身是符号链接
        linked_file = self.base / 'linked-file'
        shutil.copytree(pack, linked_file)
        program = next((linked_file / 'shaders/vulkan/direct').rglob('*.program'))
        target = self.base / 'program-copy.bin'
        target.write_bytes(program.read_bytes())
        program.unlink()
        os.symlink(str(target), str(program))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(linked_file))

        # 只在 index 里的幽灵直连行（磁盘和描述都没有）
        ghost = self.base / 'ghost'
        shutil.copytree(pack, ghost)
        rows = direct_library.read_index(ghost / 'index.tsv')
        rows.append(('shaders/vulkan/direct/1/' + PROFILE + '/' + '0' * 64 + '.program',
                     10, '1' * 64))
        index = ''.join(f'{p}\t{s}\t{d}\n' for p, s, d in rows).encode()
        (ghost / 'index.tsv').write_bytes(index)
        manifest = json.loads((ghost / 'manifest.json').read_text())
        manifest.update(index_sha256=sha256(index), files=len(rows))
        (ghost / 'manifest.json').write_text(json.dumps(manifest, indent=1))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(ghost))

        # 描述 title 与 artifact title 不一致
        titled = self.base / 'titled'
        shutil.copytree(pack, titled)
        artifact = json.loads((titled / 'game-build-artifact.json').read_text())
        artifact['titleId'] = '0100000000000001'
        (titled / 'game-build-artifact.json').write_text(json.dumps(artifact, indent=1) + '\n')
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(titled))

    def test_verify_pack_rejects_a_program_outside_the_abi_profile_path(self):
        """运行时按自己的 ABI/profile 派生目录；条目挪到别的目录即使哈希自洽也不能接受。"""
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')
        program = next((pack / 'shaders/vulkan/direct').rglob('*.program'))
        wrong_dir = pack / 'shaders/vulkan/direct/99' / ('sha256-' + '9' * 64)
        wrong_dir.mkdir(parents=True)
        moved = wrong_dir / program.name
        program.rename(moved)
        descriptor_path = pack / direct_library.DESCRIPTOR_REL
        doc = json.loads(descriptor_path.read_text())
        for entry in doc['files']:
            if entry['path'].endswith('/' + program.name):
                entry['path'] = 'shaders/vulkan/direct/99/' + ('sha256-' + '9' * 64) + '/' + program.name
        descriptor_path.write_text(json.dumps(doc, indent=1) + '\n')
        self.refresh_pack(pack)
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(pack))

        # 把容器改名成非 .program 会跳过内容解析，必须直接拒绝这种文件类型
        renamed = self.base / 'renamed'
        shutil.copytree(pack, renamed)
        target = next((renamed / 'shaders/vulkan/direct').rglob('*.program'))
        hidden = target.with_suffix('.bin')
        target.rename(hidden)
        descriptor_path = renamed / direct_library.DESCRIPTOR_REL
        doc = json.loads(descriptor_path.read_text())
        for entry in doc['files']:
            if entry['path'].endswith('/' + target.name):
                entry['path'] = entry['path'][:-len('.program')] + '.bin'
        descriptor_path.write_text(json.dumps(doc, indent=1) + '\n')
        self.refresh_pack(renamed)
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(renamed))

    def test_verify_pack_rejects_an_indexed_size_that_does_not_match_bytes(self):
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')
        # 兼容手写描述省略 size；但 index 行的长度仍必须等于磁盘字节数
        descriptor_path = pack / direct_library.DESCRIPTOR_REL
        doc = json.loads(descriptor_path.read_text())
        for entry in doc['files']:
            entry.pop('size', None)
        descriptor_path.write_text(json.dumps(doc, indent=1) + '\n')
        self.refresh_pack(pack)
        direct_library.verify_pack(str(pack))
        rows = direct_library.read_index(pack / 'index.tsv')
        target = next(row for row in rows if row[0].endswith('.program'))
        rows = [(path, size + 1 if path == target[0] else size, digest)
                for path, size, digest in rows]
        index = ''.join(f'{p}\t{s}\t{d}\n' for p, s, d in rows).encode()
        (pack / 'index.tsv').write_bytes(index)
        manifest = json.loads((pack / 'manifest.json').read_text())
        manifest['index_sha256'] = sha256(index)
        (pack / 'manifest.json').write_text(json.dumps(manifest, indent=1))
        with self.assertRaises(direct_library.LibraryError):
            direct_library.verify_pack(str(pack))

    def test_index_has_no_read_limit(self):
        """index.tsv 不设读取上限（原先 32 MiB、要设变量才放开，超大型 RomFS 不设就出不了包）。

        三处读取点（read_index / describe_pack / verify_pack）同口径：不设任何环境变量，大 index 都能读。
        用 400 条深路径把 index 撑到数百 KiB（真撑过 32 MiB 要数万文件或超长路径）：谁把一个小于它的上限加回来就红。"""
        payload = self.base / 'one-byte.bin'
        payload.write_bytes(b'x')
        deep = '/'.join('d' * 200 for _ in range(4))   # 约 800 字节，各平台 PATH_MAX 之内
        extra = [(f'romfs/{deep}/n{index:05d}.bin', str(payload)) for index in range(400)]
        big_pack, entries = self.write_base_pack(name='big-base-pack', extra_entries=extra)
        self.base_pack, self.entries = big_pack, entries
        self.build()
        pack = self.write_direct_pack(self.base / 'huge-index-pack')
        index = pack / 'index.tsv'
        self.assertGreater(index.stat().st_size, 256 << 10)
        derived = self.base / 'derived-huge.json'
        self.assertGreater(len(direct_library.read_index(index)), len(extra))
        direct_library.describe_pack(str(pack), str(derived), str(self.identity))
        direct_library.verify_pack(str(pack))
        self.assertTrue(derived.is_file())

    def pack_from(self, output, library, descriptor):
        entries = list(self.entries) + [('shaders/vulkan/direct', str(library)),
                                        (make_pack.GRAPHICS_MANIFEST, str(descriptor))]
        make_pack.write_pack(str(output), entries, False,
                             make_pack.load_graphics(str(descriptor)))
        return Path(output)

    def test_make_pack_checks_the_descriptor_file_list(self):
        """三条接点负例：缺文件 / 多文件 / 坏 SHA。write_pack 必须拒绝且不留完整 manifest。"""
        self.build()
        self.pack_from(self.base / 'pack', self.lib, self.descriptor)

        # 缺文件：库里删掉一条，描述仍列出它
        missing = self.base / 'lib-missing'
        shutil.copytree(self.lib, missing)
        next(missing.rglob('*.program')).unlink()
        with self.assertRaises(SystemExit):
            self.pack_from(self.base / 'pack-missing', missing, self.descriptor)
        self.assertFalse((self.base / 'pack-missing' / 'manifest.json').exists(),
                         'a refused pack must not look complete')

        # 多文件：库里多出一条未列出的直连文件
        extra = self.base / 'lib-extra'
        shutil.copytree(self.lib, extra)
        (extra / '1' / PROFILE / 'unlisted.bin').write_bytes(b'x')
        with self.assertRaises(SystemExit):
            self.pack_from(self.base / 'pack-extra', extra, self.descriptor)

        # 坏 SHA：描述里列的摘要与真实字节不符（路径仍在 index 里）
        with open(self.descriptor, 'rb') as stream:
            tampered = json.load(stream)
        tampered['files'][0]['sha256'] = '0' * 64
        bad_descriptor = self.base / 'graphics-bad-sha.json'
        bad_descriptor.write_text(json.dumps(tampered, indent=1) + '\n')
        with self.assertRaises(SystemExit):
            self.pack_from(self.base / 'pack-bad-sha', self.lib, bad_descriptor)

    # -- runtime-side reuse ----------------------------------------------------------
    def test_runtime_program_cache_loader_reads_every_entry(self):
        """F5.3 的读侧应是“同一 s2a_program_cache_load 换目录”，而不是新格式。"""
        if not CC:
            self.skipTest('no C compiler; runtime loader check NOT RUN')
        self.build()
        loader_c = self.base / 'loader.c'
        loader_c.write_text('''
#include <stdio.h>
#include <stdlib.h>
#include "direct/program_cache.h"

static int from_hex(const char *text, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned value;
        if (sscanf(text + 2 * i, "%2x", &value) != 1) return -1;
        out[i] = (uint8_t)value;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: loader <dir> <identity-hex>\\n"); return 2; }
    uint8_t id[32];
    if (from_hex(argv[2], id)) { printf("bad identity\\n"); return 2; }
    uint8_t *bytes = NULL;
    size_t len = 0;
    uint8_t digest[32];
    char why[256] = "";
    if (s2a_program_cache_load(argv[1], id, &bytes, &len, digest, why, sizeof why)) {
        printf("FAIL %s\\n", why);
        return 1;
    }
    printf("ok %zu %02x\\n", len, bytes[0]);
    free(bytes);
    return 0;
}
''')
        binary = self.base / 'loader'
        sources = [str(loader_c), str(HERE.parent / 'native/direct/program_cache.c'),
                   str(HERE.parent / 'native/direct/sha256.c')]
        subprocess.run([CC, '-O1', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE',
                        '-I', str(HERE.parent / 'native'), '-I', str(REPO / 'runtime/include'),
                        *sources, '-pthread', '-o', str(binary)], check=True)
        index = json.loads((self.lib / 'library.v1.json').read_text())
        directory = self.lib / '1' / PROFILE
        for entry in index['entries']:
            completed = subprocess.run([str(binary), str(directory), entry['identity']],
                                       capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn(f"ok {entry['size'] - 88}", completed.stdout)
        # 身份不符：文件还在，但用错的身份读必须失败（运行时按完整身份准入）
        wrong = 'f' * 64
        completed = subprocess.run([str(binary), str(directory), wrong], capture_output=True, text=True)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn('FAIL', completed.stdout)

    def test_native_graphics_consumer_accepts_the_descriptor(self):
        if not CC:
            self.skipTest('no C compiler; native consumer check NOT RUN')
        self.build()
        binary = self.base / 'direct_graphics_test'
        native = HERE.parent / 'native'
        subprocess.run([CC, '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(native), '-I', str(REPO / 'runtime/include'),
                        str(HERE / 'direct_graphics_test.c'), str(native / 'direct/graphics.c'),
                        str(native / 'direct/json.c'), str(native / 'direct/sha256.c'),
                        str(native / 'direct/s2a_direct_abi.c'), '-o', str(binary)], check=True)
        completed = subprocess.run([str(binary), str(self.descriptor)], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn('direct graphics file: ok', completed.stdout)
        self.assertIn(f'title={TITLE}', completed.stdout)

    # -- packaging contacts ----------------------------------------------------------
    def make_apk(self):
        apk = self.base / 'input.apk'
        with zipfile.ZipFile(apk, 'w') as archive:
            archive.writestr('assets/game/game.properties', f'titleId={TITLE}\nversionCode=2\n')
            for relative, data in self.module_images().items():
                archive.writestr('assets/game/' + relative, data)
        return apk

    def make_resources(self, pack, name='resources'):
        """The installed revision (modules/ dropped, rows sorted) for a direct pack."""
        out = self.base / name
        with patch.object(game_apk, 'apk_signer_sha256', return_value='a' * 64), \
             patch.object(game_apk, 'apk_identity', return_value=('org.s2arm.game.t' + TITLE.lower(), 2, False)):
            game_apk.resources(str(pack), str(self.make_apk()), str(out))
        return out

    def test_game_apk_resources_verifies_the_direct_pack(self):
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')
        resources = self.make_resources(pack)
        self.assertTrue((resources / direct_library.DESCRIPTOR_REL).is_file())
        self.assertTrue(any(path.name.endswith('.program')
                            for path in (resources / 'shaders/vulkan/direct').rglob('*')))
        # 资源包（安装 revision）算出的 GameContentRoot 必须等于描述里的值：
        # 模块行只进 APK，game_apk.py 会丢掉它们并按字母序重写 index。
        rows = direct_library.read_index(resources / 'index.tsv')
        self.assertEqual(direct_library.index_content_root(rows),
                         self.load_descriptor()['game_content_root'])
        # 缺一条库文件：最后一个宿主关口必须拒绝，而不是把不完整资源发到设备
        next((pack / 'shaders/vulkan/direct').rglob('*.program')).unlink()
        with self.assertRaises(SystemExit):
            self.make_resources(pack, name='resources2')

    def test_attach_shaders_adds_generic_pack_to_a_direct_pack(self):
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')
        resources = self.make_resources(pack)
        before = direct_library.read_index(resources / 'index.tsv')
        direct_rows = [row for row in before if row[0].startswith(direct_library.DIRECT_PREFIX)]
        root = direct_library.index_content_root(before)   # no modules/ rows in a revision
        generic = self.base / 'generic-pack'
        profile = generic / 'vulkan' / 'generic' / 'profile-key'
        profile.mkdir(parents=True)
        (profile / 'manifest.json').write_text(json.dumps(
            {'schema': 's2a-vulkan-pack-v1', 'contentRoot': root}))
        (profile / 'payload.bin').write_bytes(b'generic cache')
        completed = subprocess.run([sys.executable, str(TOOLS / 'attach_shaders.py'),
                                    str(resources), str(generic)], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        attached = direct_library.read_index(resources / 'index.tsv')
        self.assertTrue(any(row[0].startswith('shaders/vulkan/generic/') for row in attached))
        self.assertEqual([row for row in attached if row[0].startswith(direct_library.DIRECT_PREFIX)],
                         direct_rows)
        self.assertEqual(direct_library.index_content_root(attached),
                         self.load_descriptor()['game_content_root'])
        direct_library.verify_pack(str(resources))
        # 再挂一次通用包：不允许两代通用包混在同一个 revision
        completed = subprocess.run([sys.executable, str(TOOLS / 'attach_shaders.py'),
                                    str(resources), str(generic)], capture_output=True, text=True)
        self.assertNotEqual(completed.returncode, 0)

    def test_attach_shaders_refuses_an_unlisted_direct_file(self):
        self.build()
        pack = self.write_direct_pack(self.base / 'pack')
        (pack / 'shaders/vulkan/direct/1' / PROFILE / 'stray.bin').write_bytes(b'stray')
        generic = self.base / 'generic-pack2'
        profile = generic / 'vulkan/generic/key'
        profile.mkdir(parents=True)
        (profile / 'manifest.json').write_text(json.dumps({'schema': 's2a-vulkan-pack-v1',
                                                          'contentRoot': 'sha256-' + '0' * 64}))
        completed = subprocess.run([sys.executable, str(TOOLS / 'attach_shaders.py'),
                                    str(pack), str(generic)], capture_output=True, text=True)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn('unlisted', completed.stderr + completed.stdout)

    # -- real compiler ---------------------------------------------------------------
    @unittest.skipUnless(REAL_COMPILER, 'set S2A_DIRECT_LIBRARY_REAL_COMPILER=1 to run the dotnet compiler')
    def test_real_compiler_capture_round_trip(self):
        """真实编译器输出（26087 B 级）也要过同一套生产端规则，避免只用合成 JSON 自证。"""
        dotnet = shutil.which('dotnet')
        if not dotnet:
            self.skipTest('dotnet is not installed')
        requests = self.base / 'requests'
        subprocess.run([dotnet, 'run', '-c', 'Release', '--project',
                        str(REPO / 'graphics/tools/direct/tests/compiler'),
                        '--', '--emit-requests', str(requests)], cwd=str(REPO), check=True,
                       capture_output=True, text=True)
        compiled = self.base / 'compiled'
        subprocess.run([dotnet, 'run', '-c', 'Release', '--project',
                        str(REPO / 'graphics/tools/direct/compiler/cli'),
                        '--', 'compile', '--request', str(requests / 'graphics.json'),
                        '--output', str(compiled)], cwd=str(REPO), check=True,
                       capture_output=True, text=True)
        request = json.loads((requests / 'graphics.json').read_bytes())
        result = (compiled / 'program.json').read_bytes()
        manifest = self.write_capture([(identity('real'), request, result)])
        # The capture manifest must carry the real request's identity, not the synthetic constants.
        manifest['translatorClosure'] = request['translatorClosure']
        manifest['sourceParserVersion'] = request['sourceParserVersion']
        manifest['directABI'] = request['directABI']
        manifest['profileHash'] = request['profile']['profileHash']
        (self.capture / 'capture.v1.json').write_bytes((json.dumps(manifest, indent=1) + '\n').encode())
        built = self.build()
        self.assertGreater(built['entries'], 0)
        descriptor = self.load_descriptor()
        direct_library.verify_library(str(self.lib), descriptor)
        payload = next(self.lib.rglob('*.program')).read_bytes()
        self.assertEqual(payload[88:], result)


if __name__ == '__main__':
    unittest.main()
