#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Owned capsule contract with real source/facts/identity and synthetic registry.
No game data, GPU, live descriptor-pool lifetime or device claim.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
TEST = Path(__file__).with_suffix('.c')
CASES = ('original', 'stripped', 'allocation', 'identity', 'race', 'stop', 'tess_derived',
         'partial_view', 'full_view', 'zero_layers', 'missing_raw', 'source_epoch', 'no_return',
         'wrong_key', 'stale_return', 'closed', 'packed', 'cbuf_fallback', 'replaced_same_source',
         'binding', 'claim_replay', 'collection_stop', 'identity_error',
         'policy_same', 'policy_single_legacy', 'policy_different', 'policy_negative',
         'policy_two', 'policy_max', 'policy_gate_closed', 'policy_no_pending')


def extract(source, marker):
    if source.count(marker) != 1:
        raise RuntimeError('production fragment changed: ' + marker)
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        if source[end] == '{':
            depth += 1
        if source[end] == '}':
            depth -= 1
            if not depth:
                return source[start:end + 1] + '\n'
    raise RuntimeError('unclosed production fragment')


def main():
    source = (NATIVE / 'nvn/nvn_vk_compile_facts.c').read_text()
    controls = [
        ('skip-linked-policy', 'policy_different', [(
            'in->requires_linked_varying_interface != cap->identity.requires_linked_varying_interface) goto reject;',
            '0) goto reject;')]),
        ('wrong-allocation', 'allocation', [('match = original || stripped;', 'match = 1; (void)original; (void)stripped;')]),
        ('skip-request-correspondence', 'identity', [('if (n != cap->canonical_bytes || memcmp(bytes, cap->canonical, n)) {', 'if (0) {')]),
        ('reuse-returned', 'original', [('if (!t || t->consumed) return -1;', 'if (!t) return -1;')]),
        ('short-key', 'wrong_key', [('memcmp(key->input_sha, cap->sha, 32)) return -1;', '0) return -1;')]),
        ('renew-old-collection', 'collection_stop', [
            ('atomic_load(&w2_stopped) || t->collection.epoch != atomic_load(&w2_epoch) ||', 'atomic_load(&w2_stopped) ||'),
            ('!collected || t->collection.epoch != atomic_load(&w2_epoch) || !w2_open()', '!collected || !w2_open()'),
            ('cap->epoch = t->collection.epoch;', 'cap->epoch = atomic_load(&w2_epoch);')]),
        ('reuse-failed-collection', 'identity_error', [('t->collection.valid = 0; // even an early identity error consumes this collection', '/* wrong: retain failed collection */')]),
        ('partial-view', 'partial_view', [('(!r->raw.view_complete || !r->raw.view_layers)', '(!r->raw.view_layers)')]),
    ]
    with tempfile.TemporaryDirectory(prefix='s2a-w2-frozen-') as folder:
        build = Path(folder)
        (build / 'w2_tess_source.inc').write_text(extract((NATIVE / 'nvn/nvn_vk_draw.c').read_text(),
            'if (!in.gfx.has_tess_mode && v.control_bytes >= 0x748)'))
        includes = (NATIVE, NATIVE / 'direct', NATIVE / 'nvn', TEST.parent / 'vkhost',
                    ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/gfx/nvn/include',
                    ROOT / 'graphics/nvn/third_party/vulkan-headers/include', build)
        # nvn_vk_compile_facts.c 的 compile facts 填充要 nvn_vk_bcrt_view_format（强符号，定义在
        # android/native/nvn/nvn_vk_mrt.c）；漏链在 Linux/macOS 都是 undefined reference。
        sources = [NATIVE / 'nvn/nvn_vk_format.c', NATIVE / 'nvn/nvn_vk_mrt.c'] + [NATIVE / ('direct/' + name + '.c') for name in
            ('source_store', 'sha256', 'input_identity', 'texture_facts', 'compile_export_w2', 'w2_single_query')]

        def run(label, text, cases, negative=False):
            implementation = build / 'nvn_vk_compile_facts.c'
            implementation.write_text(text)
            exe = build / label
            cmd = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-missing-field-initializers', '-pthread'] + shlex.split(os.environ.get('CFLAGS', ''))
            cmd += ['-I' + str(path) for path in includes]
            host_link.run(cmd + [str(TEST), str(implementation), *map(str, sources), '-o', str(exe)], check=True, timeout=60)
            for case in cases:
                result = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=10)
                if negative:
                    expected = 'claimed!=0 && !capsule' if label == 'skip-linked-policy' else 'FAIL line '
                    if result.returncode != 1 or expected not in result.stderr:
                        raise RuntimeError(label + ': expected functional failure\n' + result.stdout + result.stderr)
                elif result.returncode:
                    raise RuntimeError(case + ': ' + result.stdout + result.stderr)
            print(label + ': ' + str(len(cases)) + '/' + str(len(cases)) + ' expected functional outcomes', flush=True)

        run('production', source, CASES)
        for label, case, edits in controls:
            mutated = source
            for old, new in edits:
                if mutated.count(old) != 1:
                    raise RuntimeError('mutation anchor changed: ' + label)
                mutated = mutated.replace(old, new)
            run(label, mutated, (case,), True)
    print('capsule: ' + str(len(CASES)) + ' cases and ' + str(len(controls)) + ' compiled functional mutation controls passed')


if __name__ == '__main__':
    main()
