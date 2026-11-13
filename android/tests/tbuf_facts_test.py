#!/usr/bin/env python3
"""Buffer fact and canonical identity regression, including source mutations."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/tbuf-facts-test'
OUT.mkdir(parents=True, exist_ok=True)
SOURCE = ROOT / 'android/native/direct/texture_facts.c'
TEST = ROOT / 'android/tests/direct_texture_facts_test.c'
INCLUDES = ['-I' + str(ROOT/'android/native'), '-I' + str(ROOT/'android/native/direct'),
            '-I' + str(ROOT/'graphics/nvn/gfx/nvn/include')]


def run(name, source):
    exe = OUT/name
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    *INCLUDES, str(TEST), str(source), '-o', str(exe)], check=True)
    return subprocess.run([str(exe)], text=True, capture_output=True)


if sys.argv[1:] == ['--withdraw']:
    source = SOURCE.read_text(encoding='utf-8')
    mutations = [
        ('remove-buffer-target', '{ NVN_TEXTURE_TARGET_BUFFER, "buffer", LAYERS_ONE },',
         '', 'buffer fact retains target'),
        ('wrong-buffer-class', 'out->is_buffer = nvn_target == NVN_TEXTURE_TARGET_BUFFER;',
         'out->is_buffer = 0;', 'buffer fact retains target'),
        ('wrong-normalized', 'out->normalized = nvn_target != NVN_TEXTURE_TARGET_RECTANGLE;',
         'out->normalized = nvn_target != NVN_TEXTURE_TARGET_RECTANGLE && nvn_target != NVN_TEXTURE_TARGET_BUFFER;',
         'buffer fact retains target'),
        ('unguarded-runtime', 'if (nvn_target == NVN_TEXTURE_TARGET_BUFFER) { setwhy(why, why_size, "buffer texture execution is not implemented"); return -1; }',
         '', 'existing runtime producer rejects buffer'),
    ]
    for name, before, after, expected in mutations:
        assert source.count(before) == 1, (name, 'mutation anchor')
        path = OUT/(name+'.c')
        path.write_text(source.replace(before, after), encoding='utf-8')
        result = run(name, path)
        assert result.returncode != 0 and expected in result.stdout, (name, result.stdout, result.stderr)
        print(f'{name}: expected assertion failure: {expected}', flush=True)
    print(f'PASS: {len(mutations)} negative controls')
elif not sys.argv[1:]:
    result = run('facts', SOURCE)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode:
        sys.exit(result.returncode)
    subprocess.run([sys.executable, str(ROOT/'android/tests/direct_input_identity_compat_test.py')], check=True)
else:
    raise SystemExit('usage: tbuf_facts_test.py [--withdraw]')
