#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import hashlib
import json
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


def reader_fixture(base):
    """Synthetic, complete events independent of the C clock and writer."""
    def begin(us, token, stages=32, submit=1):
        return dict(type='compile_begin', mono_ns=us*1000, boot_us=us, token=token,
                    identity=f'{token:064x}', stages=stages, submit=submit, warm_hit=0)

    def end(us, token, success=1):
        return dict(type='compile_end', mono_ns=us*1000, boot_us=us, token=token, success=success)

    def frame(us):
        return dict(type='frame', mono_ns=us*1000, boot_us=us)

    seq = 0

    def segment(index, low, high, records):
        nonlocal seq
        head = dict(schema='s2a-direct-trace-v1', clock='CLOCK_BOOTTIME', pid=71, session=2,
                    uuid='1234567890abcdef1234567890abcdef', segment=index, start_us=low,
                    end_us=high, dropped=0, count=len(records))
        lines = [head]
        for record in records:
            seq += 1
            lines.append(dict(record, seq=seq))
        body = ''.join(json.dumps(item)+'\n' for item in lines).encode()
        footer = json.dumps(dict(status='complete', sha256=hashlib.sha256(body).hexdigest()))
        path = base / f'trace-{index}.jsonl'
        path.write_bytes(body + footer.encode() + b'\n')
        return path

    # A synchronous result and a failed translation complete in frame 2. The first straddles
    # frame 1, so only its wall-time overlap belongs there. A zero-length call at the same
    # timestamp as frame 2, but queued after it, belongs to frame 3.
    first = segment(0, 1, 5, [begin(1, 1), end(2, 1), frame(2), begin(3, 2),
                              begin(4, 3, submit=0), frame(5)])
    second = segment(1, 5, 19, [end(6, 2), end(7, 3), begin(7, 4), end(9, 4, success=0),
                               begin(9, 5, stages=7), end(10, 5), frame(10),
                               begin(10, 6), end(10, 6), frame(14),
                               begin(15, 7), end(16, 7), begin(17, 8)])
    result = read_segments([second, first])
    assert len(result['compiles']) == 7 and len(result['inflight']) == 1
    cs = result.get('cs_sync_compile')
    assert cs is not None, 'paired CS submission compiles must be accounted in presentation windows'
    assert [(f['frame_index'], f['completed'], f['succeeded'], f['wall_ns']) for f in cs['frames']] == [
        (1, 0, 0, 2000), (2, 2, 1, 3000), (3, 1, 1, 0)], 'only paired CS submission calls contribute'
    assert [[c['identity'] for c in f['completions']] for f in cs['frames']] == [
        [], [f'{2:064x}', f'{4:064x}'], [f'{6:064x}']]
    assert cs['frames'][0]['overlaps'] == [{'identity': f'{2:064x}', 'wall_ns': 2000}]
    assert cs['unattributed_wall_ns'] == 2000
    assert [c['identity'] for c in cs['unattributed_completions']] == [f'{1:064x}', f'{7:064x}']

    log = base/'logcat-mono.txt'
    feedback = ('nvk warm: nvk warm 收尾: 程序 载入 0；PSO feedback=on'
                '（created/valid/no_feedback/app_hit/driver_ns/p95_le_ns/wall_ns）'
                ' gfx_warm=0/0/0/0/0/0/0 compute_warm=2/1/1/0/3/4/5'
                ' gfx_draw=6/6/0/0/1/2/3 compute_draw=13/13/0/9/100/50/120')
    log.write_text('\n'.join([
        '100.040 71 77 I S2Arm : nvk pipe: 计算管线编译 3.0 毫秒 键 - 现编',
        '100.043 71 77 I S2Arm : nvk pipe: 计算管线编译 12.3 毫秒 键 0123456789abcdef 出库 摘要 12345678…',
        '100.044 71 77 I S2Arm : nvk pipe: 图形管线编译 20.0 毫秒',
        '100.045 71 77 E S2Arm : 【红】nvk gpu: vkCreateComputePipelines 失败（VkResult -4，2 项资源）',
        '100.046 72 77 I S2Arm : nvk pipe: 计算管线编译 31.7 毫秒 键 - 现编',
        '100.047 71 78 I S2Arm : nvk pipe: 计算管线编译 4.0 毫秒 键 - 现编',
        '100.048 71 77 I DifferentTag : nvk pipe: 计算管线编译 50.0 毫秒 键 - 现编',
        f'100.049 72 79 I S2Arm : {feedback}',
        f'100.050 71 79 I S2Arm : {feedback}',
    ])+'\n', encoding='utf-8')
    output = subprocess.run([sys.executable, str(ROOT/'android/direct_diag_reader.py'),
                             '--pipeline-log', str(log), '--pid', '71', '--tid', '77'],
                            text=True, capture_output=True)
    assert output.returncode == 0, output.stderr
    readout = json.loads(output.stdout)
    assert readout['cs_sync_compile'] is None, 'without paired trace, absence of CS compiles is unproven'
    pipes = readout['compute_pipelines']
    assert pipes['all_calls_covered'] is False
    assert pipes['slow_success_count'] == 2 and pipes['slow_reported_wall_ns'] == 15300000
    assert pipes['slow_max_reported_wall_ns'] == 12300000
    assert [e['log_mono_ms'] for e in pipes['slow_successes']] == [100040, 100043]
    assert pipes['slow_successes'][1]['stage_key'] == '0123456789abcdef'
    assert pipes['slow_successes'][1]['source'] == 'libpack'
    assert pipes['failure_lines'][0]['vk_result'] == -4 and len(pipes['failure_lines']) == 1
    snapshots = pipes.get('counter_snapshots')
    assert snapshots, 'process-wide compute PSO counters must be read even when the logger has a different TID'
    snap, = snapshots
    assert snap['log_mono_ms'] == 100050 and snap['tid'] == 79 and snap['mode'] == 'on'
    warm, draw = snap['compute_warm'], snap['compute_draw']
    assert warm['created'] == 2 and warm['no_feedback'] == 1 and warm['wall_ns'] == 5
    assert draw == {'created': 13, 'valid': 13, 'no_feedback': 0, 'app_hit': 9,
                    'driver_ns': 100, 'p95_le_ns': 50, 'wall_ns': 120}
    print('PASS: CS frame overlap and completions, background/graphics/unpaired exclusions, and separate pipeline observations')


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
        reader_fixture(base)
    return 0
if __name__ == '__main__':
    if sys.argv[1:] == ['--reader']:
        out = ROOT/'build/direct-diag-test'; out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as base:
            reader_fixture(Path(base))
    elif sys.argv[1:] == ['--mutants']:
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
