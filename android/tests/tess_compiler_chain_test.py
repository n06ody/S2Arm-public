#!/usr/bin/env python3
"""生产解析生成的细分请求 → 真编译器 → 生产结果解析与阶段装载。"""
import base64
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

import dotnet_chain

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/tess-compiler'
CLI = ROOT / 'graphics/tools/direct/compiler/cli'
NATIVE = ROOT / 'android/tests/nvk_shader_pool_test.py'


def execution_modes(blob, model):
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    offset = 5
    entries, modes = [], {}
    while offset < len(words):
        count, opcode = divmod(words[offset], 1 << 16)
        assert count and offset + count <= len(words), (count, offset)
        if opcode == 15:
            entries.append((words[offset + 1], words[offset + 2]))
        if opcode == 16:
            modes.setdefault(words[offset + 1], {})[words[offset + 2]] = words[offset + 3:offset + count]
        offset += count
    assert len(entries) == 1 and entries[0][0] == model, entries
    return modes[entries[0][1]]


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    dotnet_chain.run(ROOT / 'graphics/tools/direct/tests/compiler', ['--emit-caps', OUT/'caps'], OUT)
    caps = json.loads((OUT/'caps/caps.json').read_text())
    closure = json.loads(dotnet_chain.run(CLI, ['identity'], OUT, capture=True))['translatorClosure']
    for enabled in (True, False):
        caps['features']['tessellationShader'] = enabled
        arm = OUT / ('enabled' if enabled else 'disabled')
        arm.mkdir()
        (arm/'caps.json').write_text(json.dumps(caps))
        profile, request, compiled = arm/'profile.json', arm/'request.json', arm/'compiled'
        dotnet_chain.run(CLI, ['profile', '--caps', arm/'caps.json', '--output', profile], OUT)
        subprocess.run([sys.executable, str(NATIVE), '--emit-tess', str(profile), closure, str(request)], check=True)
        command = ['compile', '--request', request, '--output', compiled]
        if not enabled:
            result = dotnet_chain.run_result(CLI, command, OUT)
            assert result.returncode and 'tessellationShader is not enabled in the profile' in result.stderr, result
            print('PASS: real compiler rejects a profile with tessellation disabled')
            continue

        doc = json.loads(request.read_text())
        assert len(doc['requestId']) == 64, doc['requestId']
        assert [stage and stage['stage'] for stage in doc['program']['stages']] == ['vs', 'fs', None, 'tcs', 'tes']
        dotnet_chain.run(CLI, command, OUT)
        result_path = compiled/'program.json'
        stages = json.loads(result_path.read_text())['content']['stages']
        for stage_name, model in [('tcs', 1), ('tes', 2)]:
            stage = next(row for row in stages if row['stage'] == stage_name)
            blob = base64.b64decode(stage['spirv'])
            (OUT/f'{stage_name}.spv').write_bytes(blob)
            modes = execution_modes(blob, model)
            if stage_name == 'tcs':
                assert modes[26] == (3,), modes  # OutputVertices 3
            else:
                # Ryujinx Vulkan 翻转绕向，客体 CW 应生成 VertexOrderCcw。
                assert {2, 5, 24} <= modes.keys(), modes  # SpacingFractionalEven, VertexOrderCcw, Quads
        subprocess.run([sys.executable, str(NATIVE), '--execute-tess', str(profile), closure, str(result_path)], check=True)
        print('PASS: production resolver and real compiler load TCS with 3 output vertices and TES with Quads/FractionalEven/host CCW (guest CW)')


if __name__ == '__main__':
    main()
