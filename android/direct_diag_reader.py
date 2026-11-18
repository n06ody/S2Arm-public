#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate completed direct-trace segments and output paired compile events and frame gaps.

Only consecutive, complete segments with zero dropped records establish coverage. An
unpaired compile_begin is reported as inflight, never counted as a completed compile.
"""
import hashlib
import json
import sys
from pathlib import Path


def read_segments(paths):
    if not paths:
        raise ValueError('no segments')
    groups = []
    for path in paths:
        lines = Path(path).read_bytes().splitlines(keepends=True)
        if len(lines) < 2 or any(not line.endswith(b'\n') for line in lines):
            raise ValueError('incomplete segment')
        values = [json.loads(line) for line in lines]
        head, end = values[0], values[-1]
        if head.get('schema') != 's2a-direct-trace-v1' or head.get('clock') not in ('CLOCK_BOOTTIME', 'CLOCK_MONOTONIC'):
            raise ValueError('unsupported schema or clock')
        if end != {'status': 'complete', 'sha256': hashlib.sha256(b''.join(lines[:-1])).hexdigest()}:
            raise ValueError('incomplete segment or checksum mismatch')
        records = values[1:-1]
        if head['count'] != len(records) or head['dropped'] != 0:
            raise ValueError('lost records')
        groups.append((head, records))
    groups.sort(key=lambda v: v[0]['segment'])
    first = groups[0][0]
    if first['segment'] != 0:
        raise ValueError('missing initial segment')
    identity = (first['pid'], first['session'], first['clock'], first['uuid'])
    prev_end, seq, prev_mono, prev_boot = first['start_us'], 0, 0, first['start_us']
    inflight, finished, frames = {}, [], []
    for index, (head, records) in enumerate(groups):
        if (head['pid'], head['session'], head['clock'], head['uuid']) != identity or head['segment'] != index or head['start_us'] != prev_end or head['end_us'] < prev_end:
            raise ValueError('segment identity, ordering or coverage mismatch')
        for r in records:
            if r['seq'] != seq + 1 or r['mono_ns'] < prev_mono or not max(prev_boot, head['start_us']) <= r['boot_us'] <= head['end_us']:
                raise ValueError('record sequence or timestamps invalid')
            seq, prev_mono, prev_boot = r['seq'], r['mono_ns'], r['boot_us']
            kind = r['type']
            if kind == 'frame':
                frames.append({'mono_ns': r['mono_ns'], 'boot_us': r['boot_us'], 'gap_ns': r['mono_ns'] - frames[-1]['mono_ns'] if frames else None})
            elif kind == 'compile_begin':
                token, key = r['token'], r['identity']
                if token in inflight or not isinstance(key, str) or len(key) != 64 or any(c not in '0123456789abcdef' for c in key) or r['stages'] not in (1, 3, 7, 32) or r['submit'] not in (0, 1) or r['warm_hit'] not in (0, 1):
                    raise ValueError('invalid compile begin')
                inflight[token] = r
            elif kind == 'compile_end':
                token = r['token']
                if token not in inflight or r['success'] not in (0, 1):
                    raise ValueError('invalid compile end')
                start = inflight.pop(token)
                finished.append({'identity': start['identity'], 'stages': start['stages'], 'submit': start['submit'], 'warm_hit': start['warm_hit'], 'start_mono_ns': start['mono_ns'], 'end_mono_ns': r['mono_ns'], 'duration_ns': r['mono_ns'] - start['mono_ns'], 'start_boot_us': start['boot_us'], 'end_boot_us': r['boot_us'], 'success': r['success']})
            else:
                raise ValueError('unknown record')
        prev_end = head['end_us']
    return {'pid': identity[0], 'session': identity[1], 'clock': identity[2], 'uuid': identity[3], 'coverage_start_us': first['start_us'], 'coverage_end_us': prev_end, 'compiles': finished, 'inflight': list(inflight.values()), 'frames': frames}


def main():
    try:
        print(json.dumps(read_segments(sys.argv[1:]), ensure_ascii=False))
    except (ValueError, KeyError, TypeError, OSError) as e:
        print(f'【红】direct trace: {e}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
