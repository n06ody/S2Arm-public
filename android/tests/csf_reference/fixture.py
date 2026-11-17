#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
import argparse
from pathlib import Path
import struct
p=argparse.ArgumentParser();p.add_argument('out',type=Path);p.add_argument('--groups',type=int,default=8);args=p.parse_args()
args.out.mkdir(parents=True,exist_ok=False)
g=args.groups
pack=lambda values: struct.pack('<%dI'%len(values),*values)
inputs=list(range(g*1024)); initial=[0xdeadbeef]*(g*1026); expected=initial.copy()
mask=[1.0 if i%5!=0 else 0.0 for i in range(256)]
for k in range(g):
    visible=[v for i,v in enumerate(inputs[k*1024:(k+1)*1024]) if v%3==0 and mask[i%256]>.5]
    expected[k*1026]=len(visible);expected[k*1026+1]=k+91
    expected[k*1026+2:k*1026+2+len(visible)]=visible
for name,data in [('input.bin',pack(inputs)),('output.before.bin',pack(initial)),('output.after.bin',pack(expected)),('mask.bin',struct.pack('<256f',*mask))]:
    (args.out/name).write_bytes(data)
rows=['S2A-CSCAP\t1',f'groups\t{g}\t1\t1',f'buffer\t1\t0\t1\t0\t1\tinput\t0\t{len(inputs)*4}\tinput.bin\tinput.bin',
      f'buffer\t1\t1\t1\t0\t3\toutput\t0\t{len(initial)*4}\toutput.before.bin\toutput.after.bin',
      # VK_FORMAT_R32_SFLOAT=100, COLOR=1, components IDENTITY=0 (verified NDK headers).
      'image\t2\t0\t100\t1\t4\t16\t16\t1\t0\t0\t0\t0\tmask.bin','status\tcomplete']
(args.out/'capture.tsv').write_text('\n'.join(rows)+'\n')
