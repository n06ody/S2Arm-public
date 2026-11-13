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
subprocess.run(['dotnet','run','-c','Release','--project',str(fixtures),'--','--emit-caps',str(out/'caps')], check=True)
caps = json.loads((out/'caps/caps.json').read_text())
caps['features']['geometryShader'] = True
caps['features']['geometryShaderOcclusionQuery'] = True
(out/'caps-gs.json').write_text(json.dumps(caps, separators=(',', ':')))
subprocess.run(['dotnet','run','-c','Release','--project',str(cli),'--','profile','--caps',str(out/'caps-gs.json'),'--output',str(out/'profile.json')], check=True)
identity = subprocess.check_output(['dotnet','run','-c','Release','--project',str(cli),'--','identity'], text=True)
closure = json.loads(identity)['translatorClosure']
subprocess.run([sys.executable,str(root/'android/tests/nvk_shader_pool_test.py'),'--emit-real',str(out/'profile.json'),closure,str(source_dir),str(out/'request.json')], check=True)
req = json.loads((out/'request.json').read_text())
assert req['schema'] == 's2a-direct-compile-v2' and [s and s['stage'] for s in req['program']['stages']] == ['vs','fs','gs',None,None]
subprocess.run(['dotnet','run','-c','Release','--project',str(cli),'--','compile','--request',str(out/'request.json'),'--output',str(out/'compiled')], check=True)
cc = ['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'android/native'),'-I'+str(root/'android/native/direct'),'-I'+str(root/'android/native/nvn')]
subprocess.run(cc + [str(root/'android/tests/gs_result_extract.c'),str(root/'android/native/nvn/nvn_vk_program_result.c'),str(root/'android/native/direct/jv.c'),str(root/'android/native/direct/json.c'),'-o',str(out/'parse')], check=True)
subprocess.run([str(out/'parse'),str(out/'compiled/program.json'),str(out)], check=True)
print('PASS: original Maxwell -> production v2 request -> real compiler -> production result parser (synthetic enabled profile; no Vulkan draw)')
