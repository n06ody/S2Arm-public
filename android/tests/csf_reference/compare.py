#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Execute identical private input twice per variant; compare every output word and byte.
This proves equivalence only for the captured input. It does not prove culling inputs are correct.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def shader_info(path):
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError('unaligned SPIR-V')
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V')
    local = None
    descriptors = {}
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 65535
        if not n or i + n > len(words):
            raise ValueError('invalid SPIR-V instruction')
        w = words[i:i+n]
        if op == 16 and n == 6 and w[2] == 17:
            local = w[3:6]
        # Capture does not record sampler filtering/compare state. TexelFetch ignores that state.
        if op in set(range(87, 95)) | set(range(305, 321)) | {96, 97, 98, 99, 105}:
            raise ValueError('unsupported sampled/storage image operation: capture is texel-fetch only')
        if op == 71 and n == 4 and w[2] in (33, 34):
            descriptors.setdefault(w[1], {})[w[2]] = w[3]
        i += n
    if local is None:
        raise ValueError('fixed LocalSize missing')
    return local, {(v[34], v[33]) for v in descriptors.values() if 33 in v and 34 in v}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def difference(a, b):
    if len(a) != len(b):
        return {'size_a': len(a), 'size_b': len(b)}
    offsets = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    return {'different_bytes': len(offsets), 'different_words': len({i // 4 for i in offsets}),
            'first_byte': offsets[0] if offsets else None, 'sha256_a': digest(a), 'sha256_b': digest(b)}


def run(args):
    root = args.snapshot.resolve().parent
    rows = [line.split('\t') for line in args.snapshot.read_text().splitlines()]
    if rows[0] != ['S2A-CSCAP', '1'] or ['status', 'complete'] not in rows:
        raise ValueError('incomplete or unsupported capture')
    dims = next(tuple(map(int, row[1:])) for row in rows if row[0] == 'groups')
    if len(dims) != 3 or not all(0 < n <= 65535 for n in dims):
        raise ValueError('invalid groups')
    local, orig_desc = shader_info(args.original)
    spill_local, spill_desc = shader_info(args.spill)
    scratch = (args.spill_set, args.spill_binding)
    if local != spill_local or spill_desc != orig_desc | {scratch} or scratch in orig_desc:
        raise ValueError('shader interfaces differ beyond spill binding')
    args.out.mkdir(parents=True, exist_ok=False)
    lines = ['groups ' + ' '.join(map(str, dims)), 'local ' + ' '.join(map(str, local))]
    writes, seen, buffers, hashes = [], set(), [], {}
    for row in rows:
        if row[0] not in ('buffer', 'image'):
            continue
        pair = tuple(map(int, row[1:3]))
        if pair in seen:
            raise ValueError('duplicate descriptor')
        seen.add(pair)
        names = row[9:11] if row[0] == 'buffer' else row[13:14]
        for name in names:
            if Path(name).name != name:
                raise ValueError('capture path must be a basename')
            hashes[name] = digest((root / name).read_bytes())
        if row[0] == 'buffer':
            if len(row) != 11:
                raise ValueError('malformed buffer')
            cls, source, access = map(int, row[3:6])
            alias, off, size = row[6], int(row[7]), int(row[8])
            if cls not in (0, 1) or off < 0 or size <= 0 or off + size > 512 * 1024**2:
                raise ValueError('unsupported buffer range')
            data = (root / row[9]).read_bytes()
            if len(data) != size or len((root / row[10]).read_bytes()) != size:
                raise ValueError('capture length mismatch')
            for old_alias, old_off, old_data in buffers:
                lo, hi = max(off, old_off), min(off + size, old_off + len(old_data))
                if alias == old_alias and hi > lo and data[lo-off:hi-off] != old_data[lo-old_off:hi-old_off]:
                    raise ValueError('overlapping input aliases disagree')
            buffers.append((alias, off, data))
            lines.append('buffer ' + ' '.join(map(str, (*pair, cls, access, alias, off, size, root / row[9]))))
            if access & 2:
                writes.append((pair, root / row[10]))
        else:
            if len(row) != 14:
                raise ValueError('malformed image')
            lines.append('image ' + ' '.join(row[1:13]) + ' ' + str(root / row[13]))
    if not orig_desc <= seen or not writes:
        raise ValueError('missing descriptors or no observable output')
    if any(any(c.isspace() for c in str(x)) for x in (root, args.out.resolve())):
        raise ValueError('reference paths must have no whitespace')
    report = {'snapshot_sha256': digest(args.snapshot.read_bytes()), 'input_sha256': hashes,
              'original_sha256': digest(args.original.read_bytes()), 'spill_sha256': digest(args.spill.read_bytes()),
              'groups': dims, 'local': local, 'outputs': {}, 'status': 'unproven'}
    results = {}
    for variant in ('original', 'spill'):
        for repeat in range(2):
            tag = f'{variant}-{repeat}'
            out = args.out / tag
            out.mkdir()
            content = list(lines)
            if variant == 'spill':
                scratch_size = args.spill_bytes * dims[0] * dims[1] * dims[2]
                if scratch_size <= 0 or scratch_size > 512 * 1024**2:
                    raise ValueError('spill size exceeds reference limit')
                init = out / 'scratch.bin'
                init.write_bytes(bytes([0 if repeat == 0 else 0xa5]) * scratch_size)
                content.append('buffer ' + ' '.join(map(str, (*scratch, 1, 0, 'spill-scratch', 0, scratch_size, init.resolve()))))
            spec = out / 'input.spec'
            spec.write_text('\n'.join(content) + '\n')
            shader = args.original if variant == 'original' else args.spill
            proc = subprocess.run([str(args.runner.resolve()), str(shader.resolve()), str(spec.resolve()), str(out.resolve()), str(args.shared if variant == 'original' else 0)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / 'run.log').write_text(proc.stdout)
            print(tag, proc.stdout.strip())
            if proc.returncode or any(x in proc.stdout for x in ('Validation Error', 'VUID-', 'SYNC-HAZARD-')):
                raise ValueError(f'{tag}: execution or validation failed')
            results[tag] = {pair: (out / f'{pair[0]}-{pair[1]}.bin').read_bytes() for pair, _ in writes}
    status = 'equal'
    for pair, captured in writes:
        a, a2, b, b2 = (results[k][pair] for k in ('original-0','original-1','spill-0','spill-1'))
        detail = {'original_repeat': difference(a, a2), 'spill_poison_repeat': difference(b, b2),
                  'original_vs_spill': difference(a, b), 'reference_spill_vs_device': difference(b, captured.read_bytes())}
        report['outputs'][f'{pair[0]}:{pair[1]}'] = detail
        if a != a2 or b != b2:
            status = 'unstable_or_uninitialized'
        elif a != b and status == 'equal':
            status = 'different'
    report['status'] = status
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'reference status={status} outputs={len(writes)} report={args.out / "report.json"}')
    return 0 if status == 'equal' else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('snapshot', 'original', 'spill', 'runner', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--spill-set', type=int, default=1)
    p.add_argument('--spill-binding', type=int, required=True)
    p.add_argument('--spill-bytes', type=int, required=True)
    p.add_argument('--shared', type=int, default=45056)
    args = p.parse_args()
    try:
        return run(args)
    except (ValueError, OSError, StopIteration, subprocess.CalledProcessError) as e:
        print('reference unavailable:', e)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
