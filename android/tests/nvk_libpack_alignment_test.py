#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real resolver and package parser; synthetic source, pack and shader loader."""
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/tests'))
import host_link
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols


def main():
    out = ROOT / 'build/nvk-libpack-alignment-test'
    out.mkdir(parents=True, exist_ok=True)
    tempfile.tempdir = str(out)
    native, gfx = ROOT/'android/native', ROOT/'graphics/nvn'
    ctl = bytearray(0x760)
    for offset, value in ((0x20, 0x760), (0x748, 8), (0x74c, 2), (0x750, 1), (0x754, 17), (0x758, 32), (0x700, 4), (0x704, 8)):
        struct.pack_into('<I', ctl, offset, value)
    cs = hashlib.sha256(ctl + bytes([1, 2, 3, 4]) + bytes(12)).digest()[:8].hex()
    spv = struct.pack('<5I', 0x07230203, 0x10000, 0, 1, 0)  # parser fixture; fake loader, no GPU execution
    entries = [
        (cs, 'cs', [{'msl_name':'cp_s0', 'kind':'sbuf', 'index':0, 'reg':2, 'rw':True, 'arg_id':2}]),
        ('0123456789abcdef', 'vs', []), ('1032547698badcfe', 'ps', []),
    ]
    chain = (gfx/'gfx/nvn/dmsh_chain.h').read_text().split('DMSH_CHAIN "')[1].split('"')[0]
    with tempfile.TemporaryDirectory(prefix='pack-', dir=out) as directory:
        root = Path(directory)
        pack = root/'shaders'/'direct'/('b-'+chain+'-vk'); pack.mkdir(parents=True)
        payload = b''; records = []
        for key, stage, bindings in entries:
            data = json.dumps(dict(key=key+'_b', stage=stage, dm_ctl=key, bindings=bindings), separators=(',', ':')).encode()
            for kind, block in ((1, data), (52, spv)):
                records.append((int(key, 16), kind, len(block), len(payload))); payload += block
        (pack/'lib.000.pak').write_bytes(payload)
        idx = struct.pack('<8sIIII12s12xQQ', b'S2ALIB\0\0', 1, len(records), 1, 0, chain.encode(), len(payload), 0)
        for key, kind, length, offset in sorted(records): idx += struct.pack('<QBBHIQ', key, kind, 0, 0, length, offset)
        (pack/'lib.idx').write_bytes(idx)
        ndk = link_symbols.find_ndk()
        if not ndk: raise RuntimeError('requires pinned NDK 29 headers')
        host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
        sysinc = ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include'
        include = out/'include'; include.mkdir(exist_ok=True)
        for name in ('vulkan', 'vk_video'):
            if (sysinc/name).exists() and not (include/name).exists(): (include/name).symlink_to(sysinc/name)
        inc = (include, ROOT/'runtime/include', native, native/'nvn', native/'direct', gfx, gfx/'replay', gfx/'gfx/nvn', gfx/'gfx/nvn/include', ROOT/'android/tests/vkhost', native/'gfx_plat')
        libs = [gfx/'replay/nvn_unimpl.c', gfx/'replay/rp_state.c', native/'nvn/nvn_vk_compute_request.c', native/'nvn/nvn_vk_prog_warm.c', ROOT/'runtime/src/direct_host.c']
        libs += [native/'direct'/f for f in ('source_store.c', 'control.c', 'input_identity.c', 'texture_facts.c', 'sha256.c', 'json.c', 'compile_request.c')]
        flags = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections', '-pthread']
        if sys.platform != 'darwin': flags += ['-D_GNU_SOURCE']
        flags += ['-I'+str(p) for p in inc]
        source = native/'gfx_plat/nvk_shader.c'
        if sys.argv[1:] == ['--warm-mutant']:
            code = source.read_text()
            before = 'compute ? 0 : nst, compute ? rec->keys[0] : NULL, rec->lp_ok == 2, &pc, &bytes, &len)'
            after = 'compute ? 0 : nst, compute ? rec->keys[0] : NULL, (compute ? 1 : rec->lp_ok == 2), &pc, &bytes, &len)'
            assert code.count(before) == 1
            source = out/'warm-mutant.c'; source.write_text(code.replace(before, after))
        elif sys.argv[1:]: raise ValueError('unknown argument')
        exe = out/'nvk-libpack-alignment'
        strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        host_link.run(flags + [str(p) for p in [ROOT/'android/tests/nvk_shader_pool_test.c', source, *libs]] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        env = dict(os.environ, S2A_SHADER_REVISION_ROOT=str(root))
        failed = []
        for case in ('live-aligned', 'live-unaligned', 'live-absent', 'warm-allowed', 'warm-disallowed', 'graphics-allowed', 'graphics-disallowed'):
            result = subprocess.run([str(exe), '--libpack-alignment', case], env=env, capture_output=True, text=True, timeout=30)
            print(result.stdout.strip(), result.stderr.strip(), flush=True)
            if result.returncode: failed.append(case)
        if sys.argv[1:] == ['--warm-mutant']:
            assert failed == ['warm-disallowed'], failed
            print('PASS: warm-only gate withdrawal fails only the warm disallowed assertion')
            return 0
        return bool(failed)

if __name__ == '__main__': raise SystemExit(main())
