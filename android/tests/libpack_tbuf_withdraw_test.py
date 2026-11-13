#!/usr/bin/env python3
"""The offline TBUF fixture must fail if the parser drops its SPIR-V class."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / 'build/libpack-tbuf-withdraw'
out.mkdir(parents=True, exist_ok=True)
parser = (root / 'android/native/nvn/nvn_vk_program_result.c').read_text()
old = 'out->texel_buffer = actual == 1;'
assert parser.count(old) == 1
mutant = out / 'nvn_vk_program_result.c'
mutant.write_text(parser.replace(old, 'out->texel_buffer = 0;'))
exe = out / 'libpack-test'
native = root / 'android/native'
sources = [root/'android/tests/libpack_vk_test.c', native/'direct/libpack_vk.c',
           native/'direct/json.c', native/'direct/jv.c', mutant]
flags = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror']
includes = [native, native/'direct', native/'nvn', root/'runtime/include']
subprocess.run([os.environ.get('CC', 'cc'), *flags, *('-I'+str(p) for p in includes),
                *(str(p) for p in sources), '-o', str(exe)], check=True)
env = os.environ.copy()
env['TMPDIR'] = str(out)
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
expected = 'FAIL: real offline TBUF preserved at VS combined texture slot 5'
if result.returncode != 1 or expected not in result.stdout:
    raise SystemExit(f'wrong negative control outcome rc={result.returncode}\n{result.stdout}\n{result.stderr}')
print('NEGATIVE CONTROL PASS: dropping SPIR-V TBUF class fails the real offline binding assertion')
