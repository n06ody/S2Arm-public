#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Trace transport records byte counts and producer digests without recomputing them."""
import json
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from direct_diag_reader import read_segments


def main():
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'segment.jsonl'
        head = dict(schema='s2a-direct-trace-v2', clock='CLOCK_BOOTTIME', segment=0,
                    pid=1, session=2, uuid='a'*32, start_us=10, end_us=20, count=0, dropped=0)
        def write(h, status='complete'):
            lines = [h, dict(status=status, sha256='0'*64)]
            path.write_text(''.join(json.dumps(line, separators=(',', ':')) + '\n' for line in lines))
        write(head)
        result = read_segments([path])
        assert result['segments'][0]['bytes'] == path.stat().st_size
        assert result['segments'][0]['product_reported_sha256'] == '0'*64
        for h, status in [(dict(head, dropped=1), 'complete'), (dict(head, segment=1), 'complete'), (head, 'partial')]:
            write(h, status)
            try: read_segments([path])
            except ValueError: pass
            else: raise AssertionError('invalid segment accepted')
        write(head)
        path.write_bytes(path.read_bytes()[:-1])
        try: read_segments([path])
        except ValueError: pass
        else: raise AssertionError('truncated segment accepted')
    print('PASS: self-reported digest is recorded without checking; byte count and structural controls')

if __name__ == '__main__': main()
