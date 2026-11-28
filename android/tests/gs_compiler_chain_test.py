#!/usr/bin/env python3
"""Original Maxwell -> production NVN request -> real compiler -> production result parser."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = root / 'build/696-real-compiler'
if out.exists():
    shutil.rmtree(out)
out.mkdir(parents=True)
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dotnet_chain  # noqa: E402
source_dir = out / 'maxwell'
source_dir.mkdir()

def guest(fragment, ops):
    blocks = (len(ops) + 3) // 3
    code = bytearray(0x50 + blocks * 32)
    struct.pack_into('<I', code, 0, (2 if fragment else 1) | (3 << 5) | ((5 if fragment else 1) << 10))
    if fragment:
        struct.pack_into('<I', code, 72, 15)
    for i in range(blocks * 4):
        oi = (i // 4) * 3 + i % 4 - 1
        op = 0x001f8000fc0007e0 if i % 4 == 0 else ops[oi] if oi < len(ops) else 0xe30000000007000f
        struct.pack_into('<Q', code, 0x50 + i * 8, op)
    return code

def fadd(reg, value):
    bits = struct.unpack('<I', struct.pack('<f', value))[0]
    return 0x0800000000000000 | (bits << 20) | (7 << 16) | (0xff << 8) | reg

def ald():
    return 0xefd8000000000000 | (0xff << 39) | (0x80 << 20) | (7 << 16) | (0xff << 8)

def ast():
    return 0xeff0000000000000 | (0xff << 39) | (0x80 << 20) | (7 << 16) | (0xff << 8)

def ast_pos():
    return 0xeff0000000000000 | (0xff << 39) | (3 << 47) | (0x70 << 20) | (7 << 16) | (0xff << 8) | 4

def emit(cut=False):
    return 0xfbe0000000000000 | ((3 if cut else 1) << 39) | (0xff << 20) | (7 << 16) | (0xff << 8) | 0xff

vs = guest(False, [])
fs = guest(True, [fadd(0,0), fadd(1,1), fadd(2,0), fadd(3,1)])
gs = guest(False, [ald(), ast(), fadd(4,-1), fadd(5,-1), fadd(6,0), fadd(7,1), ast_pos(), emit(), fadd(4,3), ast_pos(), emit(), fadd(4,-1), fadd(5,3), ast_pos(), emit(True)])
struct.pack_into('<I', gs, 0, 1 | (3 << 5) | (4 << 10))
struct.pack_into('<III', gs, 8, 1 << 24, 7 << 24, 3)
for name, code in [('vs', vs), ('fs', fs), ('gs', gs)]:
    (source_dir / (name + '.bin')).write_bytes(code)

cli = root / 'graphics/tools/direct/compiler/cli'
fixtures = root / 'graphics/tools/direct/tests/compiler'
dotnet_chain.run(fixtures, ['--emit-caps', out/'caps'], out)
caps = json.loads((out/'caps/caps.json').read_text())
caps['features']['geometryShader'] = True
caps['features']['geometryShaderOcclusionQuery'] = True
(out/'caps-gs.json').write_text(json.dumps(caps, separators=(',', ':')))
dotnet_chain.run(cli, ['profile', '--caps', out/'caps-gs.json', '--output', out/'profile.json'], out)
identity = dotnet_chain.run(cli, ['identity'], out, capture=True)
closure = json.loads(identity)['translatorClosure']
subprocess.run([sys.executable,str(root/'android/tests/nvk_shader_pool_test.py'),'--emit-real',str(out/'profile.json'),closure,str(source_dir),str(out/'request.json')], check=True)
req = json.loads((out/'request.json').read_text())
assert req['schema'] == 's2a-direct-compile-v2' and [s and s['stage'] for s in req['program']['stages']] == ['vs','fs','gs',None,None]
dotnet_chain.run(cli, ['compile', '--request', out/'request.json', '--output', out/'compiled'], out)
dotnet_chain.run(fixtures, ['--emit-live-result', out/'request.json', out/'compiled/program.live'], out)
cc = ['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'android/native'),'-I'+str(root/'android/native/direct'),'-I'+str(root/'android/native/nvn')]
subprocess.run(cc + [str(root/'android/tests/gs_result_extract.c'),str(root/'android/native/nvn/nvn_vk_program_result.c'),str(root/'android/native/direct/jv.c'),str(root/'android/native/direct/json.c'),'-o',str(out/'parse')], check=True)
subprocess.run([str(out/'parse'),str(out/'compiled/program.json'),str(out)], check=True)
print('PASS: original Maxwell -> production v2 request -> real compiler -> production result parser (synthetic enabled profile; no Vulkan draw)')

subprocess.run([sys.executable,str(root/'android/tests/nvk_shader_pool_test.py'),'--execute-reuse',str(out/'profile.json'),closure,str(source_dir),str(out/'compiled/program.json')], check=True)

# Each relaxed input is changed alone, then compiled independently. Only transport
# metadata (requestId) may differ; executable content and dependency trace must be
# byte-for-byte equal, including SPIR-V, layout, and queried specialization facts.
canonical = lambda value: json.dumps(value, sort_keys=True, separators=(',', ':')).encode()
expected = canonical(json.loads((out/'compiled/program.json').read_text())['content'])
for variant in ('texture', 'rt', 'inactive'):
    request = out/('reuse-' + variant + '.json')
    result_dir = out/('reuse-' + variant)
    subprocess.run([sys.executable,str(root/'android/tests/nvk_shader_pool_test.py'),
                    '--emit-reuse-' + variant,str(out/'profile.json'),closure,str(source_dir),str(request)], check=True)
    changed = json.loads(request.read_text()) != req
    assert changed == (variant == 'texture'), 'only texture changes submitted compiler facts: ' + variant
    dotnet_chain.run(cli, ['compile', '--request', request, '--output', result_dir], out)
    actual = canonical(json.loads((result_dir/'program.json').read_text())['content'])
    assert actual == expected, 'compiler content changed for relaxed input: ' + variant
    print('PASS: independently compiled content byte-for-byte equal: ' + variant)

# c2 word 72 aliases image slot 0 and combined slot 32. Use an original
# TEX instruction and the real request producer, compiler, parser and layout.
high = out / 'high-slot'
high.mkdir()
for name in ('vs', 'gs'):
    (high / (name + '.bin')).write_bytes((source_dir / (name + '.bin')).read_bytes())
tex72 = 0xC000000000000000 | (7 << 16) | (2 << 28) | (0xF << 31) | (72 << 36) | (2 << 8)
(high / 'fs.bin').write_bytes(guest(True, [fadd(2, 0.5), fadd(3, 0.5), tex72]))
request = out / 'high-slot-request.json'
compiled = out / 'high-slot-compiled'
subprocess.run([sys.executable, str(root/'android/tests/nvk_shader_pool_test.py'), '--emit-high-slot',
                str(out/'profile.json'), closure, str(high), str(request)], check=True)
facts = json.loads(request.read_text())['program']['textures']
assert any(t['stage'] == 'fs' and t['handle'] == 72 and t['cbufSlot'] == -1 and
           t['format'] == 'R8G8B8A8Unorm' for t in facts)
subprocess.run(['dotnet', 'run', '--no-build', '-c', 'Release', '--project', str(cli), '--', 'compile',
                '--request', str(request), '--output', str(compiled)], check=True)
subprocess.run([sys.executable, str(root/'android/tests/nvk_shader_pool_test.py'), '--execute-high-slot',
                str(out/'profile.json'), closure, str(high), str(compiled/'program.json')], check=True)
print('PASS: original Maxwell TEX c2 word 72 -> real producer/compiler/parser -> FS combined slot 32')
