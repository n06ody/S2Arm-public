#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate direct traces and account for synchronous CS translation by presentation.

Only consecutive, complete segments with zero dropped records establish trace coverage.
Optionally read Vulkan compute pipeline diagnostics from same-process monotonic logcat.
"""
import argparse
from bisect import bisect_left
import hashlib
import json
import re
import sys
from pathlib import Path


def cs_sync_by_frame(frames, frame_seq, compiles):
    """Count completions by the next present, and charge wall time to each overlapping gap."""
    windows = [
        {'frame_index': i, 'start_mono_ns': prev['mono_ns'], 'end_mono_ns': current['mono_ns'],
         'completed': 0, 'succeeded': 0, 'wall_ns': 0, 'completions': [], 'overlaps': []}
        for i, (prev, current) in enumerate(zip(frames, frames[1:]), 1)
    ]
    outside = []
    outside_wall_ns = 0
    for start_seq, end_seq, event in compiles:
        # The producer records clocks and sequence under one lock. Sequence breaks clock ties.
        first = bisect_left(frame_seq, start_seq)
        last = bisect_left(frame_seq, end_seq)
        if 1 <= last < len(frames):
            window = windows[last - 1]
            window['completed'] += 1
            window['succeeded'] += event['success']
            window['completions'].append(event)
        else:
            outside.append(event)
        remaining = event['duration_ns']
        for n in range(max(first, 1), min(last + 1, len(frames))):
            window = windows[n - 1]
            overlap = min(event['end_mono_ns'], window['end_mono_ns']) - max(
                event['start_mono_ns'], window['start_mono_ns'])
            if overlap:
                window['wall_ns'] += overlap
                window['overlaps'].append({'identity': event['identity'], 'wall_ns': overlap})
                remaining -= overlap
        outside_wall_ns += remaining
    return {'frames': windows, 'unattributed_completions': outside,
            'unattributed_wall_ns': outside_wall_ns}


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
    frame_seq, cs_sync = [], []
    for index, (head, records) in enumerate(groups):
        if (head['pid'], head['session'], head['clock'], head['uuid']) != identity or head['segment'] != index or head['start_us'] != prev_end or head['end_us'] < prev_end:
            raise ValueError('segment identity, ordering or coverage mismatch')
        for r in records:
            if r['seq'] != seq + 1 or r['mono_ns'] < prev_mono or not max(prev_boot, head['start_us']) <= r['boot_us'] <= head['end_us']:
                raise ValueError('record sequence or timestamps invalid')
            seq, prev_mono, prev_boot = r['seq'], r['mono_ns'], r['boot_us']
            kind = r['type']
            if kind == 'frame':
                frame_seq.append(seq)
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
                event = {'identity': start['identity'], 'stages': start['stages'],
                         'submit': start['submit'], 'warm_hit': start['warm_hit'],
                         'start_mono_ns': start['mono_ns'], 'end_mono_ns': r['mono_ns'],
                         'duration_ns': r['mono_ns'] - start['mono_ns'],
                         'start_boot_us': start['boot_us'], 'end_boot_us': r['boot_us'],
                         'success': r['success']}
                finished.append(event)
                if event['stages'] == 32 and event['submit'] == 1:
                    cs_sync.append((start['seq'], r['seq'], event))
            else:
                raise ValueError('unknown record')
        prev_end = head['end_us']
    return {'pid': identity[0], 'session': identity[1], 'clock': identity[2], 'uuid': identity[3],
            'coverage_start_us': first['start_us'], 'coverage_end_us': prev_end, 'compiles': finished,
            'inflight': list(inflight.values()), 'frames': frames,
            'cs_sync_compile': cs_sync_by_frame(frames, frame_seq, cs_sync)}


# backend_vulkan.c emits thresholded successful slow calls rounded to one decimal place.
# 键 is the 8-byte stage key. 现编 means the module did not come from LibPack; it does
# not attest a synchronous translation or distinguish ordinary caches.
_LOGCAT = re.compile(r'^\s*(\d+)\.(\d{3})\s+(\d+)\s+(\d+)\s+[VDIWEF]\s+S2Arm\s*:\s+(.*)$')
_SLOW_PIPE = re.compile(
    r'nvk pipe: 计算管线编译 (\d+)\.(\d) 毫秒'
    r'(?: 键 ([0-9a-f]{16}|-) (现编|出库 摘要 ([0-9a-f]{8})…))?')
_FAILED_PIPE = re.compile(r'(?:【红】)?nvk gpu: vkCreateComputePipelines 失败（VkResult (-?\d+)，\d+ 项资源）')
_PIPE_COUNTER_FIELDS = ('created', 'valid', 'no_feedback', 'app_hit', 'driver_ns', 'p95_le_ns', 'wall_ns')
_PIPE_COUNTER_VALUES = r'\d+(?:/\d+){6}'
_PIPE_COUNTERS = re.compile(
    r'nvk warm: nvk warm ([^:]+): .*；PSO feedback=(on|off|unavailable)'
    + re.escape('（' + '/'.join(_PIPE_COUNTER_FIELDS) + '）')
    + rf' gfx_warm={_PIPE_COUNTER_VALUES} compute_warm=({_PIPE_COUNTER_VALUES})'
      rf' gfx_draw={_PIPE_COUNTER_VALUES} compute_draw=({_PIPE_COUNTER_VALUES})')


def read_compute_pipeline_log(path, pid, tid=None):
    """Read process counter samples and TID-filtered thresholded PSO lines."""
    slow, failures, counters = [], [], []
    with Path(path).open(encoding='utf-8', errors='replace') as log:
        for lineno, line in enumerate(log, 1):
            match = _LOGCAT.match(line.rstrip('\n'))
            if not match:
                continue
            sec, ms, log_pid, log_tid, msg = match.groups()
            log_tid = int(log_tid)
            if int(log_pid) != pid:
                continue
            stamp = int(sec) * 1000 + int(ms)
            if m := _PIPE_COUNTERS.fullmatch(msg):
                phase, mode, warm, draw = m.groups()
                counters.append({'line': lineno, 'log_mono_ms': stamp, 'tid': log_tid,
                                 'phase': phase, 'mode': mode,
                                 'compute_warm': dict(zip(_PIPE_COUNTER_FIELDS, map(int, warm.split('/')))),
                                 'compute_draw': dict(zip(_PIPE_COUNTER_FIELDS, map(int, draw.split('/'))))})
            if tid is not None and log_tid != tid:
                continue
            if m := _SLOW_PIPE.fullmatch(msg):
                ms, tenth, key, source, digest = m.groups()
                reported_ns = (int(ms) * 10 + int(tenth)) * 100000
                slow.append({'line': lineno, 'log_mono_ms': stamp, 'tid': log_tid,
                             'reported_wall_ns': reported_ns, 'stage_key': None if key == '-' else key,
                             'source': 'libpack' if digest else 'non_libpack' if source else None,
                             'module_digest_prefix': digest})
            elif m := _FAILED_PIPE.fullmatch(msg):
                failures.append({'line': lineno, 'log_mono_ms': stamp,
                                 'tid': log_tid, 'vk_result': int(m[1])})
    return {'pid': pid, 'tid': tid, 'all_calls_covered': False,
            'slow_success_count': len(slow),
            'slow_reported_wall_ns': sum(e['reported_wall_ns'] for e in slow),
            'slow_max_reported_wall_ns': max((e['reported_wall_ns'] for e in slow), default=None),
            'slow_successes': slow, 'failure_lines': failures, 'counter_snapshots': counters}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('segments', nargs='*', help='complete direct-trace JSONL segments')
    parser.add_argument('--pipeline-log', help='logcat captured with -v monotonic')
    parser.add_argument('--pid', type=int, help='verified process, defaults to segment PID')
    parser.add_argument('--tid', type=int, help='restrict individual pipeline log lines to the replay thread')
    args = parser.parse_args(argv)
    try:
        if not args.segments and not args.pipeline_log:
            raise ValueError('no segments or pipeline log')
        result = read_segments(args.segments) if args.segments else {'cs_sync_compile': None}
        pid = result.get('pid', args.pid)
        if args.pid is not None and pid != args.pid:
            raise ValueError('pipeline log PID does not match trace session')
        if args.pipeline_log:
            if pid is None or pid <= 0 or (args.tid is not None and args.tid <= 0):
                raise ValueError('pipeline log requires a verified positive PID and optional positive TID')
            result['compute_pipelines'] = read_compute_pipeline_log(args.pipeline_log, pid, args.tid)
        print(json.dumps(result, ensure_ascii=False))
    except (ValueError, KeyError, TypeError, OSError) as e:
        print(f'【红】direct trace: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
