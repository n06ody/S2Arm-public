#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real plat_nvn.c + shared parser, synthetic TSVs, no device or game data."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
class ProfileTest(unittest.TestCase):
    def test_bundled_profile(self):
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            exe = temp / 'profile'
            cmd = [os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-DPROP_NAME_MAX=32',
                   '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-I' + str(ROOT / 'android/tests/hostvk'),
                   '-I' + str(ROOT / 'android/native'),
                   '-I' + str(ROOT / 'graphics/nvn/replay')]
            if os.environ.get('PROFILE_SOURCE'):
                cmd += ['-DPROFILE_SOURCE="' + os.environ['PROFILE_SOURCE'] + '"']
            if 'clang' not in subprocess.check_output([cmd[0], '--version'], text=True).lower():
                cmd += ['-Wno-format-truncation']  # Existing profiler snprintf, unrelated to this test.
            subprocess.run(cmd + [str(ROOT / 'android/tests/nvk_game_profile_test.c'), '-ldl', '-o', str(exe)], check=True)
            host_log = temp / 'host.log'
            with host_log.open('ab') as sink:
                subprocess.run([str(exe), '', 'host-log'], stderr=sink, check=True)
            self.assertEqual(host_log.read_bytes().splitlines(), [
                b'[s2a] nvk land-readback: ' + b'x' * 2000,
                b'[s2a] direct owner loop exited: stopped',
            ])
            profile = temp / 'shaders/direct/game.tsv'
            profile.parent.mkdir(parents=True)
            (temp / 'unreadable/shaders/direct/game.tsv').mkdir(parents=True)
            valid = ('app_pool\t0x12340000\nrender_cond\toff\nfps_hold\toff\n'
                     'tex_flush_upload\toff\ntex_dirty_upload\ton\ntex_dyn_slots\toff\n'
                     'gpu_timestamps\tsubmission\nvconv_range\ton\ndraw_barriers\tgame\n'
                     'present_interval\tdisplay\nalpha_from_red\tabc\tdef\n'
                     'feedback_snap\t123\nzero_skip\tabc:0:1:2\n')
            for case, text, expected, diagnosis in [
                ('valid', valid, 'valid', 'game.tsv'),
                ('allocation_failure', valid, 'oom', '缺省'),
                ('missing', None, 'default', '缺省'),
                ('empty', '', 'default', 'game.tsv'),
                ('invalid', 'render_cond\tINVALID\napp_pool\t0\n', 'default', '看不懂'),
                ('unknown', 'unknown_key\t42\n', 'default', 'game.tsv'),
                ('directory', None, 'default', '缺省'),
                ('oversized_path', None, 'default', '缺省'),
                ('unset', None, 'default', '缺省'),
            ]:
                with self.subTest(case=case):
                    if profile.is_file(): profile.unlink()
                    elif profile.is_dir(): profile.rmdir()
                    if case == 'directory': profile.mkdir()
                    elif text is not None: profile.write_text(text)
                    proc = subprocess.run([str(exe), 'unset' if case == 'unset' else str(temp) + '/x' * 4096 if case == 'oversized_path' else str(temp), expected], capture_output=True, text=True)
                    self.assertEqual(proc.returncode, 0, proc.stderr)
                    self.assertIn(diagnosis, proc.stderr)

if __name__ == '__main__': unittest.main()
