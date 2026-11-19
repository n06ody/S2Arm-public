#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate completed direct-trace segments and output paired compile events and frame gaps.

Only consecutive, complete segments with zero dropped records establish coverage. An
unpaired compile_begin is reported as inflight, never counted as a completed compile.
"""
import json
import sys
from pathlib import Path


def read_segments(paths):
    if not paths:
        raise ValueError('no segments')
    groups = []
    for path in paths:
        content = Path(path).read_bytes()
        lines = content.splitlines(keepends=True)
        if len(lines) < 2 or any(not line.endswith(b'\n') for line in lines):
            raise ValueError('incomplete segment')
        values = [json.loads(line) for line in lines]
        head, end = values[0], values[-1]
        if head.get('schema') not in ('s2a-direct-trace-v1', 's2a-direct-trace-v2') or head.get('clock') not in ('CLOCK_BOOTTIME', 'CLOCK_MONOTONIC'):
            raise ValueError('unsupported schema or clock')
        if end.get('status') != 'complete' or not isinstance(end.get('sha256'), str):
            raise ValueError('incomplete segment')
        records = values[1:-1]
        if head['count'] != len(records) or head['dropped'] != 0:
            raise ValueError('lost records')
        groups.append((head, records, {'segment': head['segment'], 'bytes': len(content),
                                        'product_reported_sha256': end['sha256']}))
    groups.sort(key=lambda v: v[0]['segment'])
    first = groups[0][0]
    if first['segment'] != 0:
        raise ValueError('missing initial segment')
    identity = (first['pid'], first['session'], first['clock'], first['uuid'])
    prev_end, seq, prev_mono, prev_boot = first['start_us'], 0, 0, first['start_us']
    inflight, finished, frames = {}, [], []
    pipeline_inflight, pipelines, pack_lookups = {}, [], []
    upload_inflight, uploads = {}, []
    for index, (head, records, metadata) in enumerate(groups):
        if (head['pid'], head['session'], head['clock'], head['uuid']) != identity or head['schema'] != first['schema'] or head['segment'] != index or head['start_us'] != prev_end or head['end_us'] < prev_end:
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
                if token in inflight or token in pipeline_inflight or token in upload_inflight or not isinstance(key, str) or len(key) != 64 or any(c not in '0123456789abcdef' for c in key) or r['stages'] not in (1, 3, 7, 32) or r['submit'] not in (0, 1) or r['warm_hit'] not in (0, 1):
                    raise ValueError('invalid compile begin')
                inflight[token] = r
            elif kind == 'upload_begin':
                token = r['token']
                if token in inflight or token in pipeline_inflight or token in upload_inflight or r['kind'] not in (1, 2, 3, 4, 5, 6) or not isinstance(r['reason'], int) or not 0 <= r['reason'] <= 31:
                    raise ValueError('invalid upload begin')
                upload_inflight[token] = r
            elif kind == 'upload_end':
                token = r['token']
                if token not in upload_inflight or r['success'] not in (0, 1) or r['method'] not in (0, 1, 2) or not isinstance(r['bytes'], int) or r['bytes'] < 0:
                    raise ValueError('invalid upload end')
                start = upload_inflight.pop(token)
                uploads.append({'kind': start['kind'], 'reason': start['reason'], 'start_boot_us': start['boot_us'], 'end_boot_us': r['boot_us'], 'start_mono_ns': start['mono_ns'], 'end_mono_ns': r['mono_ns'], 'duration_ns': r['mono_ns'] - start['mono_ns'], 'success': r['success'], 'method': r['method'], 'bytes': r['bytes']})
            elif kind == 'pack_lookup':
                key = r['identity']
                if not isinstance(key, str) or len(key) != 64 or any(c not in '0123456789abcdef' for c in key) or r['reason'] not in range(1, 12) or r['warm'] not in (0, 1):
                    raise ValueError('invalid pack lookup')
                pack_lookups.append(r)
            elif kind == 'pipeline_begin':
                token = r['token']
                if token in inflight or token in pipeline_inflight or token in upload_inflight or r['kind'] not in (1, 2, 3, 4, 5, 6, 7, 8) or r['warm'] not in (0, 1):
                    raise ValueError('invalid pipeline begin')
                pipeline_inflight[token] = r
            elif kind == 'pipeline_end':
                token = r['token']
                if token not in pipeline_inflight or r['success'] not in (0, 1) or r['feedback'] not in (0, 1, 2):
                    raise ValueError('invalid pipeline end')
                start = pipeline_inflight.pop(token)
                pipelines.append({'kind': start['kind'], 'warm': start['warm'], 'start_mono_ns': start['mono_ns'], 'end_mono_ns': r['mono_ns'], 'duration_ns': r['mono_ns'] - start['mono_ns'], 'start_boot_us': start['boot_us'], 'end_boot_us': r['boot_us'], 'success': r['success'], 'feedback': r['feedback']})
            elif kind == 'compile_end':
                token = r['token']
                if token not in inflight or r['success'] not in (0, 1):
                    raise ValueError('invalid compile end')
                start = inflight.pop(token)
                finished.append({'identity': start['identity'], 'stages': start['stages'], 'submit': start['submit'], 'warm_hit': start['warm_hit'], 'start_mono_ns': start['mono_ns'], 'end_mono_ns': r['mono_ns'], 'duration_ns': r['mono_ns'] - start['mono_ns'], 'start_boot_us': start['boot_us'], 'end_boot_us': r['boot_us'], 'success': r['success']})
            else:
                raise ValueError('unknown record')
        prev_end = head['end_us']
    return {'segments': [group[2] for group in groups], 'pid': identity[0], 'session': identity[1], 'clock': identity[2], 'uuid': identity[3], 'coverage_start_us': first['start_us'], 'coverage_end_us': prev_end, 'compiles': finished, 'inflight': list(inflight.values()), 'frames': frames, 'pipelines': pipelines, 'pipeline_inflight': list(pipeline_inflight.values()), 'pack_lookups': pack_lookups, 'uploads': uploads, 'upload_inflight': list(upload_inflight.values())}


def main():
    try:
        print(json.dumps(read_segments(sys.argv[1:]), ensure_ascii=False))
    except (ValueError, KeyError, TypeError, OSError) as e:
        print(f'【红】direct trace: {e}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
