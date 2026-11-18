#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""direct_input_identity 的变异门禁：正例编过并通过，两处关键修复撤掉必须红。

P0-3：constantBufferUse 的宽掩码版本分叉（S2AI1 v3/v4）现在计算与图形共用。
- `identity-compute-only`：把分叉退回“只在 compute 阶段判断” → 图形高位不再落 v4，正例必须红。
- `identity-always-wide`：把 wide 恒置 1 → 旧窗口输入也会被写成 v4，正例必须红。
"""
import os
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
TEST = ROOT / 'android/tests/direct_input_identity_test.c'
CC = os.environ.get('CC', 'cc')
BASE = [CC, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(ROOT / 'android/native'),
        '-I' + str(ROOT / 'android/native/direct'),
        '-I' + str(ROOT / 'graphics/nvn/gfx/nvn/include'),
        '-I' + str(ROOT / 'graphics/nvn/gfx/nvn'),
        str(TEST), str(NATIVE / 'direct/input_identity.c'),
        str(NATIVE / 'direct/texture_facts.c'), str(NATIVE / 'direct/sha256.c'), '-lm']

source = (NATIVE / 'direct/input_identity.c').read_text()
mutations = {
    'positive': source,
    'identity-compute-only': source.replace(
        'for (int i = 0; i < 6; i++) if (in->ubo_use[i] & ~0x1Fu) wide_cbuf = 1;',
        'for (int i = 0; i < 6; i++) if (in->stage_mask == 0x20 && (in->ubo_use[i] & ~0x1Fu)) wide_cbuf = 1;'),
    'identity-always-wide': source.replace('int wide_cbuf = 0;', 'int wide_cbuf = 1;'),
    'identity-paired-reverted': source.replace(
        'in->requires_linked_varying_interface ? 5 : (wide_cbuf ? 4 : 3)', 'wide_cbuf ? 4 : 3'),
    'identity-all-stages-v5': source.replace(
        'in->requires_linked_varying_interface ? 5 : (wide_cbuf ? 4 : 3)', 'wide_cbuf ? 5 : 5'),
}

expected_failures = {
    'identity-compute-only': 'FAIL: graphics identity version-bumps to v4 when a high cbuf bit is set',
    'identity-always-wide': 'FAIL: compute identity inside the old 5-bit window stays v3',
    'identity-paired-reverted': 'FAIL: paired VS->FS identity carries the S2AI1 version 5 marker',
    'identity-all-stages-v5': 'FAIL: compute identity inside the old 5-bit window stays v3',
}

with tempfile.TemporaryDirectory(prefix='w2-identity-mut-') as td:
    td = Path(td)
    for name, code in mutations.items():
        if name != 'positive':
            assert code != source, name
        c = td / (name + '.c')
        c.write_text(code)
        cmd = [x if x != str(NATIVE / 'direct/input_identity.c') else str(c) for x in BASE]
        cmd += ['-o', str(td / name)]
        built = subprocess.run(cmd, text=True, capture_output=True)
        if built.returncode != 0:
            sys.exit(f'{name}: build failed\n{built.stderr}')
        run = subprocess.run([str(td / name)], text=True, capture_output=True)
        print(json.dumps({"case": name, "argv": [str(td / name)],
                          "returncode": run.returncode,
                          "stdout": run.stdout, "stderr": run.stderr}))
        diagnostics = run.stdout + run.stderr
        if run.stderr or any(marker in diagnostics for marker in
                ("AddressSanitizer", "UndefinedBehaviorSanitizer", "LeakSanitizer", "runtime error:")):
            sys.exit(name + " emitted unexpected runtime diagnostics")
        if name == 'positive':
            if run.returncode != 0:
                sys.exit(f'positive failed\n{run.stdout}{run.stderr}')
            print('positive PASS', run.stdout.strip())
        else:
            if run.returncode != 1 or expected_failures[name] not in (run.stdout + run.stderr).splitlines():
                sys.exit(f'{name}: did not fail at expected assertion\n{run.stdout}{run.stderr}')
            print(name, 'correctly red:', (run.stdout + run.stderr).strip().splitlines()[0] if (run.stdout or run.stderr).strip() else 'assert')
print('PASS: identity version split (v3/v4, paired v5) positive + 4 mutation controls')
