#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""C4b pipeline-async first-draw seam; CPU synthetic inputs, not device/GPU proof.

Compiles the production nvn_vk_first_draw.c TU and drives fd_pipe_async_get /
fd_pipe_drain against the real nvn_vk_pipe_worker + nvn_first_use modules with a
fake Vulkan. Covers worker-side create/adopt, skip classes, drain and lifecycle.
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import host_link


class PipeAsyncDrawTest(unittest.TestCase):
    def test_pipe_async_draw(self):
        cc = shutil.which('cc')
        self.assertIsNotNone(cc, 'C compiler required for pipe-async regression')
        includes = [HERE / 'vkhost', HERE / 'hostvk', HERE.parent / 'native',
                    HERE.parent / 'native/direct', HERE.parent / 'native/nvn',
                    ROOT / 'runtime/include', ROOT / 'graphics/nvn/gfx/nvn',
                    ROOT / 'graphics/nvn/gfx/nvn/include', ROOT / 'graphics/nvn/gfx/tex',
                    ROOT / 'graphics/nvn/third_party/vulkan-headers/include']
        with tempfile.TemporaryDirectory() as td:
            binary = Path(td) / 'pipe-async-test'
            cmd = ([cc, '-std=gnu11', '-O0', '-g', '-pthread',
                    '-ffunction-sections', '-fdata-sections']
                   + [f'-I{x}' for x in includes]
                   + [str(HERE / 'nvn_vk_pipe_async_draw_test.c'),
                      str(HERE.parent / 'native/nvn/nvn_vk_pipe_worker.c'),
                      str(HERE.parent / 'native/nvn/nvn_vk_pipe_recipe.c'),
                      str(ROOT / 'graphics/nvn/gfx/nvn/nvn_first_use.c'),
                      '-Wl,--gc-sections' if sys.platform != 'darwin' else '-Wl,-dead_strip',
                      '-o', str(binary)])
            host_link.run(cmd, check=True)
            for arg in ([], ['off']):
                with self.subTest(arg=arg):
                    result = subprocess.run([str(binary)] + arg, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    sys.stdout.write(result.stdout)


if __name__ == '__main__':
    unittest.main()
