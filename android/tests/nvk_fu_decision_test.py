#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""C4a first-use wait decision loop; CPU synthetic inputs, not device/GPU proof.

Drives the production nvk_fu_compile inside nvn_vk_draw.c against the real
compile worker + the real shared first-use module. Assertions cover the iOS
verdict classes (late/budget/stuck), pending marks, adopt and failure paths.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导（见 host_link.py）

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
HERE = Path(__file__).with_suffix('.c')
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols


def run(command, runner=subprocess.run, **kwargs):
    result = runner(command, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError('fu decision test needs pinned NDK29 Vulkan headers; no download or skip')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    with tempfile.TemporaryDirectory(prefix='nvk-fu-decision-') as tmp:
        td = Path(tmp)
        inc = td / 'include'
        (inc / 'android').mkdir(parents=True)
        (inc / 'sys').mkdir()
        vulkan = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include/vulkan'
        (inc / 'vulkan').symlink_to(vulkan)
        if (vulkan.parent / 'vk_video').exists():
            (inc / 'vk_video').symlink_to(vulkan.parent / 'vk_video')
        (inc / 'android/native_window.h').write_text('typedef struct ANativeWindow ANativeWindow;\n')
        (inc / 'android/log.h').write_text(
            '#include <stdarg.h>\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_ERROR 6\n'
            'int __android_log_print(int,const char*,const char*,...);\n'
            'int __android_log_vprint(int,const char*,const char*,va_list);\n')
        (inc / 'sys/system_properties.h').write_text(
            '#define PROP_VALUE_MAX 92\nint __system_property_get(const char*,char*);\n')
        base = [os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-O0', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-pthread',
                '-DS2A_ANDROID_VK_ENGINE=1', '-DS2A_ANDROID_VK_BACKEND_NVN_VK=1', '-DS2A_ANDROID_DEBUG_ENV=1']
        base += ['-I' + str(p) for p in (inc, NATIVE, NATIVE / 'nvn', NATIVE / 'direct',
                                       ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/gfx/nvn/include')]
        binary = td / 'nvk_fu_decision'
        run(base + [str(HERE), '-lm', '-o', str(binary)], runner=host_link.run, capture_output=True)
        p = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        if p.returncode:
            raise SystemExit(p.returncode)


if __name__ == '__main__':
    main()
