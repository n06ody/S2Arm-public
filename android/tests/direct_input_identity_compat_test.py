#!/usr/bin/env python3
"""Verify canonical identity bytes against pre-GS synthetic fixtures."""
from pathlib import Path
import os
import subprocess
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/identity-compat'
OUT.mkdir(parents=True, exist_ok=True)
inc = ['-I' + str(ROOT/'android/native'), '-I' + str(ROOT/'android/native/direct'), '-I' + str(ROOT/'graphics/nvn/gfx/nvn/include')]
cc = os.environ.get('CC', 'cc')
subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', *inc, str(ROOT/'android/tests/direct_input_identity_compat_test.c'), str(ROOT/'android/native/direct/input_identity.c'), str(ROOT/'android/native/direct/texture_facts.c'), '-o', str(OUT/'test')], check=True)
fixtures = ROOT / 'android/tests/fixtures/input-identity-v1'
subprocess.run([str(OUT/'test'), *(str(fixtures / (x + '.bin')) for x in ('vs', 'vsfs', 'cs'))], check=True)
