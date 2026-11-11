#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real graphics draw lookup A/B; CPU synthetic inputs, not device/GPU proof.

With --baseline-ref, the frozen baseline is compiled and executed with the new observer core.
The normal gate needs no historical Git object: it compares current off/on.
It must fail the *functional* shader/receipt assertion, never a build check. All
16 production result/state/callback byte streams must equal baseline with the
observation window off and on. No source, request or program enters the receipt.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
BASELINE = '1563dcbb9d60903de54c6d636a38796a58787df7'
DRAW = 'android/native/nvn/nvn_vk_draw.c'
HERE = Path(__file__).with_suffix('.c')
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols


def run(command, runner=subprocess.run, **kwargs):
    result = runner(command, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n{result.stdout}\n{result.stderr}")
    return result


def prepare(td):
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError('real draw test needs pinned NDK29 Vulkan headers; no download or skip')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    vulkan = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include/vulkan'
    inc = td / 'include'
    (inc / 'android').mkdir(parents=True)
    (inc / 'sys').mkdir()
    (inc / 'vulkan').symlink_to(vulkan)
    if (vulkan.parent / 'vk_video').exists():
        (inc / 'vk_video').symlink_to(vulkan.parent / 'vk_video')
    (inc / 'android/native_window.h').write_text('typedef struct ANativeWindow ANativeWindow;\n')
    (inc / 'android/log.h').write_text(
        '#include <stdarg.h>\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_ERROR 6\n'
        'int __android_log_print(int,const char*,const char*,...);\n'
        'int __android_log_vprint(int,const char*,const char*,va_list);\n')
    (inc / 'sys/system_properties.h').write_text(
        '#define PROP_VALUE_MAX 92\nint __system_property_get(const char*,char*);\n')
    # This tiny wrapper is copied verbatim from the production TU. It merely
    # calls the linked real graphics parser/free; its algorithm is never mocked.
    text = (NATIVE / 'nvn/nvn_vk_first_draw.c').read_text()
    first = text.index('int nvn_vk_first_draw_program_valid(')
    last = text.index('\n}', first) + 2
    helper = td / 'program-valid.c'
    helper.write_text('#include <string.h>\n#include "nvn/nvn_vk_program_result.h"\n' +
                      text[first:last].replace('nvn_vk_first_draw_program_valid', 'w2_real_program_valid') + '\n')
    base = [os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
            '-O0', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
            '-pthread']
    compiler = run([os.environ.get('CC', 'cc'), '--version'], capture_output=True).stdout
    if 'clang' not in compiler.lower():
        # The unchanged old logger has a stage>1 temporary-lifetime warning.
        # This vs/fs test preserves it rather than modifying production code.
        base += ['-Wno-format-truncation', '-Wno-dangling-pointer']
    base += ['-I' + str(p) for p in (inc, NATIVE, NATIVE / 'nvn', NATIVE / 'direct',
                                   ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/gfx/nvn/include')]
    sources = [helper]
    # nvn_vk_mrt.c：nvn_vk_compile_facts.c 调用的强符号 nvn_vk_bcrt_view_format 定义在这里
    # （72aac1147 引入）；宿主单测不链就会在 ELF/ld64 都报 undefined reference。
    sources += [NATIVE / 'nvn' / (p + '.c') for p in
                ('nvn_vk_compile_facts', 'nvn_vk_format', 'nvn_vk_mrt', 'nvn_vk_threadbuf', 'nvn_vk_program_result')]
    sources += [NATIVE / 'direct' / (p + '.c') for p in
                ('source_store', 'input_identity', 'compile_request', 'texture_facts', 'control', 'sha256', 'jv', 'json',
                 'compile_export', 'compile_export_w2', 'w2_single_query')]   # nvn_vk_draw.c 直接调导出门（#1596）与 W2 诊断（moongrant W2），链真实现
    return base, list(map(str, sources))


def compile_one(td, base, sources, name, draw, harness=HERE, observer=True):
    source = td / (name + '-draw.c')
    source.write_text(draw)
    binary = td / name
    defs = ['-DW2_DRAW_SOURCE="' + str(source) + '"']
    if observer:
        defs += ['-DW2_HAVE_OBSERVER=1', '-DS2A_ANDROID_VK_ENGINE=1', '-DS2A_ANDROID_VK_BACKEND_NVN_VK=1', '-DS2A_ANDROID_DEBUG_ENV=1']
    run(base + defs + [str(harness)] + sources + ['-lm', '-o', str(binary)], runner=host_link.run, capture_output=True)
    return binary


def invoke(td, exe, case, enabled):
    stem = f'{exe.name}-{case}-{enabled}'
    data, receipt = td / (stem + '.bin'), td / (stem + '.json')
    result = subprocess.run([str(exe), str(case), str(enabled), str(data), str(receipt)],
                            text=True, capture_output=True, timeout=8)
    return result, data, receipt


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--baseline-ref', help='optional frozen Git ref for historical A/B; default uses current off')
    args.add_argument('--baseline-smoke', action='store_true', help='run frozen production behavior before the observer exists')
    args.add_argument('--keep-output', type=Path, help='write a persistent evidence directory instead of temporary files')
    options = args.parse_args()
    temporary = None
    if options.keep_output:
        td = options.keep_output.resolve(); td.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='w2-observation-draw-'); td = Path(temporary.name)
    try:
        base, sources = prepare(td)
        current = (ROOT / DRAW).read_text()
        ref = options.baseline_ref or (BASELINE if options.baseline_smoke else None)
        frozen = run(['git', '-C', str(ROOT), 'show', ref + ':' + DRAW], capture_output=True).stdout if ref else current
        old = compile_one(td, base, sources, 'frozen', frozen, observer=not options.baseline_smoke)
        baseline = []
        for case in range(16):
            p, data, _ = invoke(td, old, case, 0)
            assert p.returncode == 0, p.stdout + p.stderr
            baseline.append(data.read_bytes())
        if options.baseline_smoke:
            print('PASS: frozen production draw 16/16 synthetic behavior paths; no observer claimed')
            return
        if ref:
            p, data, _ = invoke(td, old, 0, 1)
            assert p.returncode != 0 and 'snap.shaders == 1' in p.stderr, p.stdout + p.stderr
            assert data.read_bytes() == baseline[0], 'frozen receipt red changed production bytes'
            (td / 'frozen-functional-red.log').write_text(p.stdout + p.stderr)
            print('frozen functional expected red: real draw completed, observed shaders=0 (expected 1)')
        positive = compile_one(td, base, sources, 'positive', current) if ref else old
        compared = 0
        for case in range(16):
            for enabled in (0, 1):
                p, data, receipt = invoke(td, positive, case, enabled)
                assert p.returncode == 0, p.stdout + p.stderr
                actual = data.read_bytes()
                assert actual == baseline[case], (case, enabled, 'full result/state/guest/callback bytes differ')
                compared += len(actual)
                if enabled:
                    doc = json.loads(receipt.read_text())
                    assert doc['schema'] == 's2a-w2-observation-v1'
                    assert doc['window_complete'] is False and doc['receipt_not_persisted'] is True
                    for key in ('request_set_complete', 'strict_acceptance', 'gpu_execution', 'w2_acceptance'):
                        assert doc[key] == 'not_proven'
                    assert all(len(e['identity']) == 64 for e in doc['entries'])
                    assert 'synthetic device' not in receipt.read_text() and 'AwIjBw' not in receipt.read_text()
        # Effective controls must compile and execute. Removing only an existing
        # probe or relabeling memo as live must not affect production byte output.
        mutations = {
            'omit-lookup': current.replace('s2a_w2_shader_lookup(', 'w2_disabled_lookup('),
            'memo-as-live': current.replace('hit->input_sha, S2A_W2_MEMO', 'hit->input_sha, S2A_W2_LIVE'),
            'omit-shader': current.replace('s2a_w2_shader_begin(&observation, S2A_W2_GRAPHICS);', '(void)observation;').replace('s2a_w2_shader_end(&observation, rc == S2A_RECORD_OK);', '(void)observation;'),
        }
        disabled = ('#include "direct/w2_observation.h"\n'
                    'static void w2_disabled_lookup(const struct s2a_direct_compile_context *c, '
                    'const uint8_t id[32], enum s2a_w2_source s, int a) '
                    '{ (void)c; (void)id; (void)s; (void)a; }\n')
        for name, mutated in mutations.items():
            assert mutated != current, (name, 'mutation did not apply')
            binary = compile_one(td, base, sources, name, (disabled if name == 'omit-lookup' else '') + mutated)
            case = 3 if name == 'memo-as-live' else 0
            p, data, _ = invoke(td, binary, case, 1)
            assert p.returncode != 0 and 'Assertion' in p.stderr, (name, p.stdout, p.stderr)
            assert data.read_bytes() == baseline[case], (name, 'changed production behavior')
            (td / (name + '-expected-red.log')).write_text(p.stdout + p.stderr)
            print(name, 'expected red (production bytes unchanged)')
        no_seal = td / 'no-seal.c'
        original = HERE.read_text()
        changed = original.replace('assert(!s2a_w2_seal(window, 1, 1));', '/* negative control: no seal */')
        assert changed != original
        no_seal.write_text(changed)
        binary = compile_one(td, base, sources, 'no-seal', current, no_seal)
        p, data, _ = invoke(td, binary, 0, 1)
        assert p.returncode != 0 and 'snap.metadata_complete == success' in p.stderr, p.stderr
        assert data.read_bytes() == baseline[0]
        (td / 'no-seal-expected-red.log').write_text(p.stdout + p.stderr)
        print('no-seal expected red (production bytes unchanged)')
        print(f'PASS: real draw 16 cases x off/on, {compared} binary bytes matched {ref or "current-off"}; '
              f'5 lookup sources, {int(bool(ref))} frozen functional red, 4 effective negative controls')
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    main()
