#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 门禁手动：要带 compute 队列的真实 Vulkan ICD（门禁机没有；缺设备时本测试直接失败、不跳过，放进默认门禁会红）。单跑：python3 android/tests/w2_compute_texture_gpu_test.py
# Copyright 2026 S2Arm contributors
"""Host-GPU byte-level proof: a real texture sampled by compute lands in an SSBO with exact bytes.

Not part of the device-free gate (needs a Vulkan ICD with a compute queue). It builds the
production compute builder (nvn_vk_compute.c + nvn_vk_compute_texture.c + nvn_vk_program_result.c)
against a real device; only the Android session/texcache resolution is replaced by test-owned
VkImage/VkImageView/VkSampler objects. Exit codes: 0 pass, 1 failed checks, 2 no usable device.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols


def main():
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64' if sys.platform.startswith('linux') else None
    if not host:
        raise RuntimeError('compute GPU test requires Linux or macOS')
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError('compute GPU test needs Android NDK 29.0.14206865 for Vulkan headers; no download or skip')
    vulkan = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include/vulkan'
    if not (vulkan / 'vulkan.h').is_file():
        raise RuntimeError('NDK Vulkan headers missing: ' + str(vulkan))
    with tempfile.TemporaryDirectory(prefix='w2-tex-gpu-') as td:
        tmp = Path(td)
        inc = tmp / 'include'
        inc.mkdir()
        (inc / 'vulkan').symlink_to(vulkan)
        if (vulkan.parent / 'vk_video').exists():
            (inc / 'vk_video').symlink_to(vulkan.parent / 'vk_video')
        exe = tmp / 'w2_compute_texture_gpu_test'
        cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-o', str(exe)]
        cmd += ['-I' + str(p) for p in (inc, NATIVE, NATIVE / 'nvn', NATIVE / 'direct',
                                        ROOT / 'android/tests/hostvk',   # nvn_vk_compute.c 宿主独立 TU 面：__has_include 命中 android/log.h 桩才引
                                        ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/gfx/nvn/include')]
        cmd += [str(Path(__file__).with_suffix('.c'))]
        cmd += [str(NATIVE / 'nvn' / p) for p in
                ('nvn_vk_compute.c', 'nvn_vk_compute_texture.c', 'nvn_vk_program_result.c')]
        cmd += [str(NATIVE / 'direct' / p) for p in ('texture_facts.c', 'jv.c', 'json.c')]
        cmd += ['-lvulkan', '-lm']
        subprocess.run(cmd, check=True, timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        sys.stderr.write(result.stderr)
        sys.stdout.write(result.stdout)
        if result.returncode == 2:
            raise RuntimeError('no usable host Vulkan device for the compute texture byte proof')
        if result.returncode != 0:
            raise SystemExit(result.returncode)
        print('PASS: host-GPU byte-level compute texture sampling')


if __name__ == '__main__':
    main()
