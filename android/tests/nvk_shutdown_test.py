#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Production shutdown paths and independent fail-at-assertion controls."""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
CASES = (
    ('nvk_compile_stop_test.py', '--withdraw-stop', 'FAIL: new shader compile admitted after provider stop'),
    ('nvk_compile_stop_test.py', '--withdraw-admit', 'FAIL: stop won admission race'),
    ('nvk_compile_stop_test.py', '--withdraw-activation', 'FAIL: stop during activation must retain context'),
    ('nvk_session_budget_test.py', '--withdraw-wait', 'FAIL: in-flight resolver must wait until deadline'),
    ('nvk_device_idle_test.py', '--withdraw-deadline', 'FAIL: provider destroy exceeded budget waiting device idle'),
)
for script in ('nvk_compile_stop_test.py', 'nvk_session_budget_test.py', 'nvk_device_idle_test.py'):
    subprocess.run([sys.executable, str(HERE / script)], check=True, timeout=90)
for script, flag, assertion in CASES:
    result = subprocess.run([sys.executable, str(HERE / script), flag], capture_output=True, text=True, timeout=90)
    if result.returncode == 0 or assertion not in result.stdout + result.stderr:
        print(result.stdout, result.stderr, file=sys.stderr)
        raise SystemExit(f'FAIL: {script} {flag} did not fail at {assertion}')
    print(f'PASS: {flag}: {assertion}', flush=True)
print('PASS: shutdown admission, budget, retention and five negative controls')
