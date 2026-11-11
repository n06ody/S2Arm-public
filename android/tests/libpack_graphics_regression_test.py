#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Run the descriptor unit cases and the same-source compiler comparison."""
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
for name in ('libpack_graphics_unit_test.py', 'libpack_graphics_compare_test.py'):
    subprocess.run([sys.executable, str(here / name)], check=True)
