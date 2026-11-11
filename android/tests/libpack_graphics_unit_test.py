#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Build the LibPack graphics contract test with the shared host linker."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'android/tests'))
import host_link


def main():
    build=ROOT/'build'
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='libpack-unit-',dir=build) as directory:
        work=Path(directory)
        exe=work/'test'
        sources=[ROOT/'android/tests/libpack_vk_test.c',
                 *(ROOT/'android/native/direct'/p for p in ('libpack_vk.c','jv.c','json.c')),
                 ROOT/'android/native/nvn/nvn_vk_program_result.c']
        cmd=[os.environ.get('CC','cc'),'-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
             '-I'+str(ROOT/'android/native'),'-I'+str(ROOT/'android/native/direct'),'-I'+str(ROOT/'android/native/nvn'),
             '-o',str(exe),*map(str,sources)]
        host_link.run(cmd,check=True,timeout=120)
        subprocess.run([str(exe)],check=True,timeout=120,env={**os.environ,'TMPDIR':str(work)})


if __name__=='__main__': main()
