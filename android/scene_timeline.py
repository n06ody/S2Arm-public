#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Summarize an independently anchored scene interval from complete direct-trace segments."""
import argparse
import json
import math
import sys
from direct_diag_reader import read_segments


def analyze(trace, pid, uuid, start_us, end_us):
    if trace['pid'] != pid or trace['uuid'] != uuid or len(uuid) != 32 or any(c not in '0123456789abcdef' for c in uuid):
        raise ValueError('PID or session UUID mismatch')
    if not trace['coverage_start_us'] <= start_us < end_us <= trace['coverage_end_us']:
        raise ValueError('scene markers are outside complete trace coverage')
    frames = trace['frames']
    gaps = [(b['mono_ns'] - a['mono_ns']) / 1e6 for a, b in zip(frames, frames[1:])
            if start_us <= a['boot_us'] <= b['boot_us'] <= end_us]
    if any(g < 0 for g in gaps):
        raise ValueError('frame timestamps are not monotonic')
    ordered = sorted(gaps)
    p99 = ordered[math.ceil(len(ordered) * .99) - 1] if ordered else None
    timeline = []
    def add_spans(name, entries):
        for e in entries:
            if e['start_boot_us'] <= end_us and e['end_boot_us'] >= start_us:
                timeline.append({'type': name, **e})
    add_spans('translation', trace['compiles'])
    add_spans('pipeline', trace['pipelines'])
    add_spans('texture_upload', trace['uploads'])
    for e in trace['pack_lookups']:
        if start_us <= e['boot_us'] <= end_us:
            timeline.append({'type': 'pack_lookup', **e})
    timeline.sort(key=lambda e: e.get('start_boot_us', e.get('boot_us', 0)))
    pipes = [e for e in timeline if e['type'] == 'pipeline']
    uploads = [e for e in timeline if e['type'] == 'texture_upload']
    creates = [e for e in pipes if e['kind'] in (1, 2, 3, 4, 5, 6)]
    pending = []
    for name in ('inflight', 'pipeline_inflight', 'upload_inflight'):
        for event in trace[name]:
            if event['boot_us'] <= end_us:
                pending.append({'type': name, **event})
    return {
        'segments': trace.get('segments', []),
        'pid': pid, 'uuid': uuid, 'scene_start_boot_us': start_us, 'first_complete_anchor_boot_us': end_us,
        'scene_elapsed_ms': (end_us - start_us) / 1000,
        'frames': {'intervals': len(gaps), 'over_33_3_ms': sum(g > 33.3 for g in gaps),
                   'over_50_ms': sum(g > 50 for g in gaps), 'over_100_ms': sum(g > 100 for g in gaps), 'p99_ms': p99},
        'summary': {'compile_calls': sum(e['type'] == 'translation' for e in timeline),
                    'guest_pipeline_creates': sum(e['kind'] in (1, 2) for e in pipes),
                    'guest_foreground_pipeline_creates': sum(e['kind'] in (1, 2) and not e['warm'] for e in pipes),
                    'guest_warm_pipeline_creates': sum(e['kind'] in (1, 2) and e['warm'] for e in pipes),
                    'internal_pipeline_creates': sum(e['kind'] in (3, 4, 5, 6) for e in pipes),
                    'pipeline_waits': sum(e['kind'] in (7, 8) for e in pipes),
                    'application_cache_hit_reported': sum(e['success'] and e['feedback'] == 2 for e in creates),
                    'application_cache_no_hit_reported': sum(e['success'] and e['feedback'] == 1 for e in creates),
                    'application_cache_feedback_unknown': sum(e['success'] and e['feedback'] == 0 for e in creates),
                    'upload_bytes': sum(e['bytes'] for e in uploads),
                    'conservative_uploads': sum(e['kind'] == 3 and bool(e['reason'] & 12) for e in uploads)},
        'status': 'unproven' if pending or not gaps else 'covered',
        'inflight_at_coverage_end': pending, 'timeline': timeline,
        'limits': ['Compare source and received segment byte counts externally; self-reported digests are not verified.',
                   'First complete scene is an external visual anchor, not inferred from CPU present.',
                   'CPU upload records preparation and command recording, not GPU completion.',
                   'No-hit-reported is not proof of a driver cache miss; overlapping boundary events contribute their full duration and bytes.'],
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--uuid', required=True)
    p.add_argument('--start-us', type=int, required=True, help='scene transition anchor, CLOCK_BOOTTIME microseconds')
    p.add_argument('--complete-us', type=int, required=True, help='independently verified complete-scene anchor, CLOCK_BOOTTIME microseconds')
    p.add_argument('segments', nargs='+')
    a = p.parse_args()
    try:
        result = analyze(read_segments(a.segments), a.pid, a.uuid, a.start_us, a.complete_us)
        print(json.dumps(result, ensure_ascii=False))
    except (ValueError, KeyError, TypeError, OSError) as e:
        print(f'【红】scene timeline: {e}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__': raise SystemExit(main())
