#!/usr/bin/env python3
"""热点计数 ②：portable.c 的 CFDictionary 兼容表查找计数（宿主 C，秒级）。--withdraw：逐处撤掉计数，用例必须红。"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/cfdict-hot'
SRC = ROOT / 'android/native/nvn/portable.c'


def build_run(name, portable):
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / name
    cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
           '-I', str(ROOT / 'android/native/nvn'), '-I', str(ROOT / 'android/native/nvn/compat'), '-I', str(ROOT / 'android/native/direct'),
           str(ROOT / 'android/tests/cfdict_hot_test.c'), str(portable), str(ROOT / 'android/native/direct/sha256.c'), '-o', str(exe)]
    subprocess.run(cmd, check=True, timeout=60)
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)


def main():
    if sys.argv[1:] == ['--withdraw']:
        text = SRC.read_text(encoding='utf-8')
        changes = [
            ('hit-not-counted', '    if (hit) __atomic_fetch_add(&g_hot_hits, 1, __ATOMIC_RELAXED);\n', '    (void)hit;\n', 'on: lookups'),
            ('miss-compares-zero', 'hot_note(dict, key, dict->count, 0);', 'hot_note(dict, key, 0, 0);', 'on: lookups'),
            ('hit-compares-index', 'hot_note(dict, key, i + 1, 1);', 'hot_note(dict, key, i, 1);', 'on: lookups'),
            ('same-key-global', 'if (dict->has_last && dict->last_key == key)', 'if (dict->has_last)', 'on: lookups'),
            ('max-not-reset', 'out->max_cmp = __atomic_exchange_n(&g_hot_max, 0, __ATOMIC_RELAXED);', 'out->max_cmp = __atomic_load_n(&g_hot_max, __ATOMIC_RELAXED);', 'remove: lookups'),
            ('remove-keeps-key', '            __atomic_fetch_sub(&g_keys, 1, __ATOMIC_RELAXED);\n', '', 'remove: lookups'),
            ('release-keeps-keys', '        __atomic_fetch_sub(&g_keys, (uint64_t)dict->count, __ATOMIC_RELAXED);\n', '', 'release: lookups'),
            ('counts-while-off', 'if (__atomic_load_n(&g_hot, __ATOMIC_RELAXED)) hot_note(dict, key, i + 1, 1);', 'hot_note(dict, key, i + 1, 1);', 'off: lookups'),
        ]
        for name, before, after, expected in changes:
            assert text.count(before) == 1, (name, 'mutation anchor')
            path = OUT / (name + '.c'); OUT.mkdir(parents=True, exist_ok=True)
            path.write_text(text.replace(before, after), encoding='utf-8')
            result = build_run('mut-' + name, path)
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print(f'PASS: {len(changes)} negative controls')
        return 0
    if sys.argv[1:]:
        raise ValueError('usage: cfdict_hot_test.py [--withdraw]')
    result = build_run('cfdict-hot', SRC)
    sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
