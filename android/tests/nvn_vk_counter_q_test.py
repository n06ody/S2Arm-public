#!/usr/bin/env python3
"""Real first-draw timestamp entry; Vulkan calls use strict host stand-ins."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import host_link
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols

ndk = link_symbols.find_ndk()
if ndk is None:
    raise RuntimeError('NDK 29 required')
platform = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
headers = ndk / 'toolchains/llvm/prebuilt' / platform / 'sysroot/usr/include'
build = ROOT / 'build/counter-q-test'
build.mkdir(parents=True, exist_ok=True)
tempfile.tempdir = str(build)
with tempfile.TemporaryDirectory(dir=build) as t:
    td = Path(t)
    (td / 'vulkan').symlink_to(headers / 'vulkan')
    (td / 'vk_video').symlink_to(headers / 'vk_video')
    inc = [td, HERE / 'vkhost', HERE / 'hostvk', ROOT / 'android/native',
           ROOT / 'android/native/direct', ROOT / 'android/native/nvn', ROOT / 'runtime/include',
           ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/gfx/nvn/include',
           ROOT / 'graphics/nvn/gfx/tex']
    exe = td / 'counter-q-test'
    cmd = ['cc', '-std=gnu11', '-O0', '-ffunction-sections', '-fdata-sections', '-pthread']
    cmd += ['-I' + str(p) for p in inc]
    cmd += [str(HERE / 'nvn_vk_counter_q_test.c'),
            str(ROOT / 'android/native/direct/vk_clear.c'),
            str(ROOT / 'android/native/direct/vk_record.c')]
    cmd += (['-Wl,-dead_strip', '-L/opt/homebrew/lib', '-lvulkan'] if sys.platform == 'darwin'
            else ['-Wl,--gc-sections', '-Wl,-l:libvulkan.so.1'])
    cmd += ['-lm', '-o', str(exe)]
    host_link.run(cmd, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    print(result.stdout, result.stderr, sep='', flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
