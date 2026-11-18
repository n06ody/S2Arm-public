#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import os
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'android'))
from direct_diag_reader import read_segments


def main():
    out = ROOT/'build/direct-diag-test'
    out.mkdir(parents=True, exist_ok=True)
    exe = out/'direct-diag-test'
    source = Path(sys.argv[2]) if len(sys.argv) == 3 and sys.argv[1] == '--source' else ROOT/'android/native/gfx_plat/direct_diag.c'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-DS2A_DIRECT_DIAG_TEST', '-DS2A_DIRECT_DIAG_INTERVAL_NS=100000000ull', '-Dlink=s2a_denied_link', '-I'+str(ROOT/'android/native'), '-I'+str(ROOT/'android/native/gfx_plat'), str(ROOT/'android/tests/direct_trace_test.c'), str(ROOT/'android/tests/direct_trace_deny_link.c'), str(source), str(ROOT/'android/native/direct/sha256.c'), '-o', str(exe)], check=True, timeout=60)
    with tempfile.TemporaryDirectory(dir=out) as base:
        base = Path(base).resolve()
        def scenario(name, mode, enabled=True):
            folder = base/name; folder.mkdir()
            env = dict(os.environ, S2A_DIRECT_DIAG_TEST_ENABLE='1' if enabled else '0', S2A_DIRECT_DIAG_TEST_UUID='1234567890abcdef1234567890abcdef')
            proc = subprocess.Popen([str(exe), str(folder), mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            if enabled:
                assert proc.stdout.readline().strip() == 'READY'
                proc.kill()
            outp, err = proc.communicate(timeout=5)
            assert proc.returncode == (-signal.SIGKILL if enabled else 0), (proc.returncode, outp, err)
            return sorted(folder.glob('*.jsonl'))
        assert not scenario('off', 'off', False)
        files = scenario('on', 'normal')
        result = read_segments(files)
        assert result['uuid'] == '1234567890abcdef1234567890abcdef'
        assert len(files) >= 3 and len(result['frames']) == 2 and len(result['compiles']) == 1 and not result['inflight']
        c = result['compiles'][0]
        assert c['identity'] == 'ab'*32 and c['stages'] == 7 and c['submit'] == 1 and c['warm_hit'] == 0 and c['success'] == 1 and c['duration_ns'] >= 100000000
        assert result['frames'][1]['gap_ns'] >= 100000000
        lost = scenario('overflow', 'overflow')
        try: read_segments(lost)
        except ValueError as e: assert 'lost records' in str(e)
        else: raise AssertionError('overflow accepted')
        collision = scenario('collision', 'collision')
        assert len(collision) == 1 and collision[0].read_bytes() == b'existing'
        assert not list(collision[0].parent.glob('*.partial'))
        pending = read_segments(scenario('inflight', 'inflight'))
        assert len(pending['inflight']) == 1 and len(pending['compiles']) == 1
        try: read_segments(files[1:])
        except ValueError: pass
        else: raise AssertionError('missing segment accepted')
        corrupt = base/'corrupt.jsonl'; shutil.copy(files[0], corrupt)
        data = corrupt.read_bytes().replace(b'compile_begin', b'compile_begio')
        assert data != corrupt.read_bytes()
        corrupt.write_bytes(data)
        try: read_segments([corrupt, *files[1:]])
        except ValueError: pass
        else: raise AssertionError('corruption accepted')
        print('PASS: default off; force-kill persisted segments; frame gap; cross-segment compile; inflight; overflow, collision, missing and corrupt controls; hard links denied')
    return 0
if __name__ == '__main__':
    if sys.argv[1:] == ['--mutants']:
        out = ROOT/'build/direct-diag-test'; out.mkdir(parents=True, exist_ok=True)
        code = (ROOT/'android/native/gfx_plat/direct_diag.c').read_text()
        mutants = [('no-publication', 'if (lstat(path, &st) == 0 || errno != ENOENT || rename(tmp, path)) bad = 1;', 'if (lstat(path, &st) == 0 || errno != ENOENT || (errno = EACCES)) bad = 1;', 'no segments'),
                   ('gate-ignored', 'if (!gate()) return 0;', '(void)gate();', 'returncode')]
        for name, before, after, expected in mutants:
            assert code.count(before) == 1
            path = out / (name + '.c'); path.write_text(code.replace(before, after))
            result = subprocess.run([sys.executable, __file__, '--source', str(path)], capture_output=True, text=True, timeout=30)
            assert result.returncode and 'error:' not in result.stderr, (name, result.stdout, result.stderr)
            if name == 'no-publication': assert expected in result.stderr, result.stderr
            else: assert 'AssertionError' in result.stderr, result.stderr
            print(f'PASS: {name} causes expected behavior failure')
    else:
        raise SystemExit(main())
