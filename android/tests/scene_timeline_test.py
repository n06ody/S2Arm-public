#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scene_timeline import analyze

def main():
    t = dict(pid=7, uuid='a'*32, coverage_start_us=50000, coverage_end_us=400000,
             frames=[dict(boot_us=100000, mono_ns=100000000), dict(boot_us=150000, mono_ns=150000000), dict(boot_us=300000, mono_ns=300000000)],
             compiles=[dict(start_boot_us=110000, end_boot_us=160000, duration_ns=50000000, success=1, submit=1)],
             inflight=[], pipeline_inflight=[], upload_inflight=[],
             pipelines=[dict(start_boot_us=170000, end_boot_us=190000, duration_ns=20000000, kind=1, warm=0, success=1, feedback=2),
                        dict(start_boot_us=200000, end_boot_us=210000, duration_ns=10000000, kind=3, warm=0, success=1, feedback=0)],
             uploads=[dict(start_boot_us=215000, end_boot_us=230000, duration_ns=15000000, kind=3, reason=20, success=1, method=1, bytes=320)],
             pack_lookups=[dict(boot_us=105000, reason=3, identity='b'*64, warm=0)])
    r = analyze(t, 7, 'a'*32, 100000, 300000)
    assert r['frames']['intervals'] == 2 and r['frames']['over_33_3_ms'] == 2 and r['frames']['over_50_ms'] == 1 and r['frames']['over_100_ms'] == 1 and r['frames']['p99_ms'] == 150
    assert r['summary']['compile_calls'] == 1 and r['summary']['guest_pipeline_creates'] == 1 and r['summary']['internal_pipeline_creates'] == 1
    assert r['summary']['application_cache_hit_reported'] == 1 and r['summary']['upload_bytes'] == 320 and len(r['timeline']) == 5
    waited = dict(t); waited['pipelines'] = t['pipelines'] + [dict(start_boot_us=240000, end_boot_us=260000, duration_ns=20000000, kind=8, warm=0, success=1, feedback=0)]
    checked = analyze(waited, 7, 'a'*32, 100000, 300000)
    assert checked['summary']['pipeline_waits'] == 1 and checked['summary']['guest_pipeline_creates'] == 1 and checked['summary']['application_cache_feedback_unknown'] == 1
    empty = dict(t); empty['frames'] = []
    assert analyze(empty, 7, 'a'*32, 100000, 300000)['status'] == 'unproven'
    incomplete = dict(t); incomplete['pipeline_inflight'] = [dict(boot_us=240000, kind=8, token=9)]
    assert analyze(incomplete, 7, 'a'*32, 100000, 300000)['status'] == 'unproven'
    edge = dict(t); edge['frames'] = [dict(boot_us=100000, mono_ns=100000000), dict(boot_us=133310, mono_ns=133310000)]
    assert analyze(edge, 7, 'a'*32, 100000, 300000)['frames']['over_33_3_ms'] == 1
    for kwargs in (dict(pid=8, uuid='a'*32, start_us=100000, end_us=300000), dict(pid=7, uuid='a'*32, start_us=1000, end_us=300000), dict(pid=7, uuid='b'*32, start_us=100000, end_us=300000)):
        try: analyze(t, **kwargs)
        except ValueError: pass
        else: raise AssertionError('invalid identity or uncovered interval accepted')
    print('PASS: scene window, exact frame thresholds, categories, and identity/coverage rejection')
if __name__ == '__main__': main()
