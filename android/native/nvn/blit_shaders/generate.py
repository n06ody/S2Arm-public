#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Regenerate the embedded original depth/stencil helper shaders; no downloads."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--glslc', default='glslc')
args = parser.parse_args()
lines = ['// SPDX-License-Identifier: Apache-2.0', '// Copyright 2026 S2Arm contributors', '// Generated from blit_shaders/*.vert and *.frag by blit_shaders/generate.py.',
         '// Regenerate: python3 android/native/nvn/blit_shaders/generate.py --glslc "$GLSLC"',
         '#pragma once']


def emit(directory, source, name, out, prefix):
    target = Path(directory) / 'shader.spv'
    subprocess.run([args.glslc, '--target-env=vulkan1.1', '-O', str(here/source), '-o', str(target)], check=True)
    data = target.read_bytes()
    words = struct.unpack('<'+'I'*(len(data)//4), data)
    out.append('static const uint32_t '+prefix+name+'[] = {')
    for offset in range(0, len(words), 8):
        out.append('    '+', '.join(f'0x{word:08x}u' for word in words[offset:offset+8])+',')
    out.append('};')


with tempfile.TemporaryDirectory(prefix='s2a-depth-shaders-') as directory:
    for source, name in [('fullscreen.vert', 'vs'), ('depth.frag', 'depth'), ('stencil.frag', 'stencil'), ('depth_msaa.frag', 'depth_msaa')]:
        emit(directory, source, name, lines, 'k_dsb_')
(here.parent/'backend_vulkan_depth_blit_spv.h').write_text('\n'.join(lines)+'\n')

# EASU present-pass shader (AMD FidelityFX FSR1, MIT). Kept in its own header so the
# depth/stencil helper header above stays byte-identical.
easl = ['// SPDX-License-Identifier: Apache-2.0',
        '// Copyright 2026 S2Arm contributors',
        '// Generated from blit_shaders/fullscreen.vert and blit_shaders/easu.frag by blit_shaders/generate.py.',
        '// easu.frag is an MIT-licensed port of AMD FidelityFX FSR1 EASU; see nvn_vk_easu_license.txt.',
        '// Regenerate: python3 android/native/nvn/blit_shaders/generate.py --glslc "$GLSLC"',
        '#pragma once']
with tempfile.TemporaryDirectory(prefix='s2a-easu-shaders-') as directory:
    emit(directory, 'fullscreen.vert', 'vs', easl, 'k_easu_')
    emit(directory, 'easu.frag', 'fs', easl, 'k_easu_')
(here.parent/'nvn_vk_easu_spv.h').write_text('\n'.join(easl)+'\n')

# DrawTexture built-in quad (#1452). Own header so the headers above stay byte-identical.
dtxl = ['// SPDX-License-Identifier: Apache-2.0',
        '// Copyright 2026 S2Arm contributors',
        '// Generated from blit_shaders/draw_tex.vert and blit_shaders/draw_tex.frag by blit_shaders/generate.py.',
        '// Regenerate: python3 android/native/nvn/blit_shaders/generate.py --glslc "$GLSLC"',
        '#pragma once']
with tempfile.TemporaryDirectory(prefix='s2a-drawtex-shaders-') as directory:
    emit(directory, 'draw_tex.vert', 'vs', dtxl, 'k_dtx_')
    emit(directory, 'draw_tex.frag', 'fs', dtxl, 'k_dtx_')
(here.parent/'backend_vulkan_draw_tex_spv.h').write_text('\n'.join(dtxl)+'\n')

# Three-channel readback shares the original guest RGB bits with host RGBA.
rgb = ['// SPDX-License-Identifier: Apache-2.0',
       '// Generated from blit_shaders/rgb_readback.comp by blit_shaders/generate.py.', '#pragma once']
with tempfile.TemporaryDirectory(prefix='s2a-rgb-readback-') as directory:
    emit(directory, 'rgb_readback.comp', 'readback', rgb, 'k_rgb_')
(here.parent/'backend_vulkan_rgb_readback_spv.h').write_text('\n'.join(rgb)+'\n')

wsi = lines[:5]
wsi[2] = '// Generated from blit_shaders/wsi_rotate.frag by blit_shaders/generate.py.'
with tempfile.TemporaryDirectory(prefix='s2a-wsi-shader-') as directory:
    emit(directory, 'wsi_rotate.frag', 'fs', wsi, 'k_wsi_rotate_')
(here.parent/'nvn_vk_wsi_rotate_spv.h').write_text('\n'.join(wsi)+'\n')
