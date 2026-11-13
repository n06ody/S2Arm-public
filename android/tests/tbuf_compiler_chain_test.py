#!/usr/bin/env python3
"""Real producer -> real Maxwell compiler -> result parser; synthetic inputs only."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/tbuf-real-red'
if OUT.exists():
    shutil.rmtree(OUT)
OUT.mkdir(parents=True)
SRC = OUT / 'maxwell'
SRC.mkdir()


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


def ldc(reg, offset):
    return 0xef90000000000000 | (4 << 48) | (2 << 36) | (offset << 20) | (7 << 16) | (0xff << 8) | reg


def lop_or(dest, a, b):
    return 0x5c40000000000000 | (7 << 48) | (1 << 41) | (b << 20) | (7 << 16) | (a << 8) | dest


def tld_bindless_1d(dest, coord, handle):
    # InstTable TldB opcode 0xdd; InstTldB fields: dim=0 (1D), lod=0, RGBA mask.
    return 0xdd00000000000000 | (7 << 16) | (15 << 31) | (handle << 20) | (coord << 8) | dest


def emit(cut=False):
    return 0xfbe0000000000000 | ((3 if cut else 1) << 39) | (0xff << 20) | (7 << 16) | (0xff << 8) | 0xff


vs = guest(False, [])
fs = guest(True, [fadd(2, 0), ldc(6, 0x168), ldc(7, 0x568), lop_or(4, 6, 7), tld_bindless_1d(0, 2, 4)])
ald = 0xefd8000000000000 | (0xff << 39) | (0x80 << 20) | (7 << 16) | (0xff << 8)
ast = 0xeff0000000000000 | (0xff << 39) | (0x80 << 20) | (7 << 16) | (0xff << 8)
ast_pos = 0xeff0000000000000 | (0xff << 39) | (3 << 47) | (0x70 << 20) | (7 << 16) | (0xff << 8) | 4
gs = guest(False, [ald, ast, fadd(4, -1), fadd(5, -1), fadd(6, 0), fadd(7, 1), ast_pos, emit(),
                   fadd(4, 3), ast_pos, emit(), fadd(4, -1), fadd(5, 3), ast_pos, emit(True)])
struct.pack_into('<I', gs, 0, 1 | (3 << 5) | (4 << 10))
struct.pack_into('<III', gs, 8, 1 << 24, 7 << 24, 3)
# Bound TLD uses an immediate c2 word (8 = combined texture slot 0). This
# avoids adding an unrelated explicit c2 uniform read to the compute shader.
tld_bound_1d = 0xdc00000000000000 | (7 << 16) | (15 << 31) | (8 << 36) | (8 << 8) | 2
cs_ops = [0xef9500310007ff00, fadd(8, 0), tld_bound_1d,
          0xeedc200000070002, 0xe30000000007000f]
cs = bytearray(((len(cs_ops) + 2) // 3) * 32)
for i, op in enumerate(cs_ops):
    struct.pack_into('<Q', cs, (i // 3 * 4 + i % 3 + 1) * 8, op)
for name, code in [('vs', vs), ('fs', fs), ('gs', gs), ('cs', cs)]:
    (SRC / (name + '.bin')).write_bytes(code)

cli = ROOT / 'graphics/tools/direct/compiler/cli'
tests = ROOT / 'graphics/tools/direct/tests/compiler'
subprocess.run(['dotnet', 'run', '-c', 'Release', '--project', str(tests), '--', '--emit-caps', str(OUT/'caps')], check=True)
caps = json.loads((OUT/'caps/caps.json').read_text())
caps['features']['geometryShader'] = True
caps['features']['geometryShaderOcclusionQuery'] = True
(OUT/'caps-gs.json').write_text(json.dumps(caps, separators=(',', ':')))
subprocess.run(['dotnet', 'run', '-c', 'Release', '--project', str(cli), '--', 'profile', '--caps', str(OUT/'caps-gs.json'), '--output', str(OUT/'profile.json')], check=True)
identity = json.loads(subprocess.check_output(['dotnet', 'run', '-c', 'Release', '--project', str(cli), '--', 'identity'], text=True))
subprocess.run([sys.executable, str(ROOT/'android/tests/nvk_shader_pool_test.py'), '--emit-real-tbuf', str(OUT/'profile.json'),
                identity['translatorClosure'], str(SRC), str(OUT/'production.json')], check=True)
req = json.loads((OUT/'production.json').read_text())


def without_target_fact(request):
    copy = json.loads(json.dumps(request))
    copy['program']['textures'] = [t for t in copy['program']['textures']
                                    if not (t['stage'] == 'fs' and t['handle'] == 90 and t['cbufSlot'] == 2)]
    return copy


def compile_request(request, name):
    path = OUT / (name + '.json')
    path.write_text(json.dumps(request, separators=(',', ':')))
    return subprocess.run(['dotnet', 'run', '-c', 'Release', '--project', str(cli), '--', 'compile', '--request', str(path),
                           '--output', str(OUT/name)], capture_output=True, text=True)


fact = {'stage': 'fs', 'handle': 90, 'cbufSlot': 2, 'samplerType': 'buffer', 'format': 'R8G8B8A8Unorm',
        'isBuffer': True, 'normalized': False, 'separate': True, 'arrayLength': 1}
controls = []
control_bindings = []
for name, sampler, is_buffer in [('buffer-control', 'buffer', True), ('image-control', '1d', False)]:
    control = without_target_fact(req)
    item = dict(fact, samplerType=sampler, isBuffer=is_buffer)
    control['program']['textures'].append(item)
    result = compile_request(control, name)
    if result.returncode:
        print('SETUP FAILED:', name, result.stdout, result.stderr, file=sys.stderr)
        sys.exit(2)
    content = json.loads((OUT/name/'program.json').read_text())['content']
    bindings = [b for b in content['bindings'] if b['stage'] == 'fs' and b['kind'] == 'texture']
    expected = 'buffer-texture' if is_buffer else 'texture'
    if len(bindings) != 1 or bindings[0]['source'] != expected:
        print('SETUP FAILED: compiler did not emit the expected real', expected, 'binding:', bindings, file=sys.stderr)
        sys.exit(2)
    controls.append(OUT/name/'program.json')
    control_bindings.append(bindings[0])
    print('CONTROL PASS: real compiler emitted', expected, flush=True)

common = ('stage', 'kind', 'set', 'binding', 'slot', 'count', 'samp_slot')
if any(control_bindings[0].get(k) != control_bindings[1].get(k) for k in common):
    print('SETUP FAILED: buffer/image bindings use different coordinates', control_bindings, file=sys.stderr)
    sys.exit(2)

removed = compile_request(without_target_fact(req), 'without-fact')
if not removed.returncode or 'MISSING_FACT' not in removed.stderr or 'QuerySamplerType' not in removed.stderr:
    print('SETUP FAILED: removing the exact fact must trigger QuerySamplerType MISSING_FACT:', removed.stderr, file=sys.stderr)
    sys.exit(2)
print('NEGATIVE CONTROL PASS: remove exact fact -> QuerySamplerType MISSING_FACT', flush=True)

result = compile_request(req, 'production')
failed = 0
if result.returncode:
    print('FAIL: production BUFFER fact must reach real compiler:', result.stderr.strip(), flush=True)
    if 'MISSING_FACT' not in result.stderr or 'QuerySamplerType' not in result.stderr:
        print('SETUP FAILED: wrong compiler failure', file=sys.stderr)
        sys.exit(2)
    failed += 1
else:
    content = json.loads((OUT/'production/program.json').read_text())['content']
    bindings = [b for b in content['bindings'] if b['stage'] == 'fs' and b['kind'] == 'texture']
    if len(bindings) != 1 or bindings[0]['source'] != 'buffer-texture':
        print('FAIL: production BUFFER must compile as buffer-texture:', bindings, flush=True)
        failed += 1
    else:
        print('PASS: production BUFFER fact reaches real compiler', flush=True)

exe = OUT/'parse'
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'android/native'),
                str(ROOT/'android/tests/tbuf_result_test.c'), str(ROOT/'android/native/nvn/nvn_vk_program_result.c'),
                str(ROOT/'android/native/direct/jv.c'), str(ROOT/'android/native/direct/json.c'), '-o', str(exe)], check=True)
same = subprocess.run([str(exe), str(controls[1]), str(controls[1])], text=True, capture_output=True)
if same.returncode != 1 or 'parser loses buffer-texture classification' not in same.stderr:
    print('SETUP FAILED: parser comparison negative control did not fail:', same.stderr, file=sys.stderr)
    sys.exit(2)
print('NEGATIVE CONTROL PASS: identical parsed binding is rejected by comparator', flush=True)
parsed = subprocess.run([str(exe), *(str(p) for p in controls)], text=True, capture_output=True)
sys.stdout.write(parsed.stdout)
sys.stderr.write(parsed.stderr)
if parsed.returncode == 2:
    sys.exit(2)
failed += parsed.returncode != 0
for source_index, lie in [(0, 'texture'), (1, 'buffer-texture')]:
    data = json.loads(controls[source_index].read_text())
    binding = next(b for b in data['content']['bindings'] if b['stage'] == 'fs' and b['kind'] == 'texture')
    binding['source'] = lie
    path = OUT / ('false-source-' + lie + '.json')
    path.write_text(json.dumps(data, separators=(',', ':')))
    bad = subprocess.run([str(exe), str(path), str(controls[1])], text=True, capture_output=True)
    if bad.returncode != 2 or 'disagrees with SPIR-V' not in bad.stderr:
        print('FAIL: contradictory source must be rejected:', bad.stderr, file=sys.stderr)
        failed += 1
    else:
        print('NEGATIVE CONTROL PASS: rejects contradictory', lie, 'source')

data = json.loads(controls[0].read_text())
binding = next(b for b in data['content']['bindings'] if b['stage'] == 'fs' and b['kind'] == 'texture')
del binding['source']
no_source = OUT/'buffer-without-source.json'
no_source.write_text(json.dumps(data, separators=(',', ':')))
result = subprocess.run([str(exe), str(no_source), str(controls[1])], text=True, capture_output=True)
if result.returncode:
    print('FAIL: legacy result must classify buffer from SPIR-V:', result.stderr, file=sys.stderr)
    failed += 1
else:
    print('PASS: source-free result classifies buffer from SPIR-V')

data = json.loads(no_source.read_text())
next(s for s in data['content']['stages'] if s['stage'] == 'fs')['spirv'] = 'AwIjBwABAwAAAAAAAQAAAAAAAAA='
unproven = OUT/'buffer-unproven.json'
unproven.write_text(json.dumps(data, separators=(',', ':')))
result = subprocess.run([str(exe), str(unproven), str(controls[1])], text=True, capture_output=True)
if result.returncode != 2 or 'descriptor type unproven' not in result.stderr:
    print('FAIL: source-free unproven descriptor must be rejected:', result.stderr, file=sys.stderr)
    failed += 1
else:
    print('NEGATIVE CONTROL PASS: source-free unproven descriptor rejected')

for args in [('--tbuf-layout', str(controls[0])),
             ('--execute-tbuf', str(OUT/'profile.json'), identity['translatorClosure'], str(SRC), str(OUT/'production/program.json'))]:
    if args[0] == '--execute-tbuf' and not (OUT/'production/program.json').exists():
        continue
    result = subprocess.run([sys.executable, str(ROOT/'android/tests/nvk_shader_pool_test.py'), *args])
    failed += result.returncode != 0

subprocess.run([sys.executable, str(ROOT/'android/tests/nvk_shader_pool_test.py'), '--emit-compute-tbuf',
                str(OUT/'profile.json'), identity['translatorClosure'], str(SRC), str(OUT/'compute-production.json')], check=True)
compute = json.loads((OUT/'compute-production.json').read_text())
compute_facts = [t for t in compute['program']['textures'] if t['stage'] == 'cs' and t['handle'] == 8 and t['cbufSlot'] == -1]
if len(compute_facts) != 1 or compute_facts[0]['samplerType'] != 'buffer' or not compute_facts[0]['isBuffer']:
    print('FAIL: production compute request lacks BUFFER fact:', compute_facts, file=sys.stderr)
    failed += 1
else:
    print('PASS: production compute request contains BUFFER fact')
without = json.loads(json.dumps(compute))
without['program']['textures'] = [t for t in without['program']['textures']
                                  if not (t['stage'] == 'cs' and t['handle'] == 8 and t['cbufSlot'] == -1)]
negative = compile_request(without, 'compute-without-fact')
if not negative.returncode or 'MISSING_FACT' not in negative.stderr or 'QueryTextureIsBuffer' not in negative.stderr:
    print('SETUP FAILED: compute missing-fact control:', negative.stderr, file=sys.stderr)
    sys.exit(2)
print('NEGATIVE CONTROL PASS: compute without exact BUFFER fact -> QueryTextureIsBuffer MISSING_FACT')
compiled = compile_request(compute, 'compute-production')
if compiled.returncode:
    print('FAIL: production compute BUFFER compile:', compiled.stderr, file=sys.stderr)
    failed += 1
else:
    result_path = OUT/'compute-production/program.json'
    result_json = json.loads(result_path.read_text())
    binding = [b for b in result_json['content']['bindings'] if b['stage'] == 'cs' and b['kind'] == 'texture']
    if len(binding) != 1 or binding[0]['source'] != 'buffer-texture':
        print('FAIL: compute compiler output lacks buffer-texture:', binding, file=sys.stderr)
        failed += 1
    else:
        print('PASS: production compute BUFFER compiles to buffer-texture')
    for args in [('--tbuf-layout', str(result_path), '--compute'),
                 ('--execute-compute-tbuf', str(OUT/'profile.json'), identity['translatorClosure'], str(SRC), str(result_path))]:
        result = subprocess.run([sys.executable, str(ROOT/'android/tests/nvk_shader_pool_test.py'), *args])
        failed += result.returncode != 0
    result = subprocess.run([sys.executable, str(ROOT/'android/tests/nvk_shader_pool_test.py'), '--tbuf-withdraw',
                             str(OUT/'profile.json'), identity['translatorClosure'], str(SRC),
                             str(OUT/'production/program.json'), str(result_path)])
    failed += result.returncode != 0
sys.exit(1 if failed else 0)
