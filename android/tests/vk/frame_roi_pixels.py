# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Check the sealed bytes against the fixture shader and independently specified WSI blit."""
import hashlib
import sys
from fractions import Fraction
from math import ceil, floor
from pathlib import Path


def span(left, right):
    lo, hi = floor(min(left, right)), ceil(max(left, right))
    length = min(4, hi - lo)
    return lo + (hi - lo - length) // 2, length


def check(arm, payload):
    body, seal = payload.rsplit(b'sealed bytes=', 1)
    assert seal == f'{len(body)} sha256={hashlib.sha256(body).hexdigest()}\n'.encode()
    rows = [line.split('\t') for line in body.decode().splitlines()]
    assert next(r for r in rows if r[0] == 'roi-status')[1] == 'ready'
    flip = arm == 'res-roi-flip'
    sx, sy, sw, sh = (4, 4, 56, 56) if flip else (0, 0, 64, 64)
    dx, dy, dw, dh = (16, 10, 288, 160) if flip else (0, 0, 320, 180)
    transform = next(r for r in rows if r[0] == 'roi-map')
    assert list(map(int, transform[1:])) == [320, 180, 64, 64, sx, sy, sw, sh, dx, dy, dw, dh, int(flip), 0]

    def source(x, y):
        u, v = (x - dx) / dw, (y - dy) / dh
        return sx + u * sw, sy + (1 - v if flip else v) * sh

    refs = [(8, 8), (13, 9), (22, 18), (37, 31), (51, 43)]
    sampled = set()
    color = next(r for r in rows if r[:2] == ['res', 'color'])
    present = next(r for r in rows if r[:2] == ['res', 'present'])
    for row in rows:
        if row[0] != 'roi':
            continue
        region, stage = int(row[1]), row[2]
        if stage == 'attachment' and row[3] != color[2]:
            continue
        writer = present if stage == 'swap' else color
        assert row[3:7] == writer[2:6] and row[7:10] == writer[7:10]
        assert (region, stage) not in sampled
        sampled.add((region, stage))
        fmt, width, height, x, y, w, h = map(int, row[11:18])
        assert fmt in (37, 44)
        assert (width, height) == ((320, 180) if stage == 'swap' else (64, 64))
        rx, ry = refs[region]
        limits = [(Fraction(a * 320, 64), Fraction(b * 180, 64)) for a, b in [(rx, ry), (rx + 2, ry + 2)]]
        if stage == 'attachment':
            limits = [source(a, b) for a, b in limits]
        assert (x, w) == span(limits[0][0], limits[1][0])
        assert (y, h) == span(limits[0][1], limits[1][1])
        expected = bytearray()
        for py in range(y, y + h):
            for px in range(x, x + w):
                u, v = px, py
                if stage == 'swap':
                    u, v = map(floor, source(Fraction(2 * px + 1, 2), Fraction(2 * py + 1, 2)))
                channels = [u, v, u ^ v]
                expected.extend((channels[::-1] if fmt == 44 else channels) + [255])
        assert expected == bytes.fromhex(row[18]), (arm, stage, region, expected.hex(), row[18])
    assert sampled == {(i, stage) for i in range(5) for stage in ('attachment', 'swap')}
    print(f'{arm} five regions x two stages verified including geometry and every channel')


if __name__ == '__main__':
    check(sys.argv[1], Path(sys.argv[2]).read_bytes())
