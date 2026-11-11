#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Exercise production record/replay with frame-queue and Vulkan boundary mocks, not a GPU test."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols
sys.path.insert(0, str(ROOT / "android/tests"))
import host_link

class DispatchFq(unittest.TestCase):
    def test_record(self):
        host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64' if sys.platform.startswith('linux') else None
        self.assertIsNotNone(host, 'Linux or macOS host required')
        ndk = link_symbols.find_ndk()
        self.assertIsNotNone(ndk, 'NDK 29 required')
        headers = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include'
        self.assertTrue((headers/'vulkan/vulkan.h').is_file())
        build = ROOT / 'build/dispatch-fq-test'
        build.mkdir(parents=True, exist_ok=True)
        tempfile.tempdir = str(build)
        with tempfile.TemporaryDirectory(prefix='fixture-', dir=build) as temp:
            td = Path(temp); inc = td/'include'; (inc/'android').mkdir(parents=True); (inc/'sys').mkdir()
            (inc/'vulkan').symlink_to(headers/'vulkan'); (inc/'vk_video').symlink_to(headers/'vk_video')
            (inc/'android/log.h').write_text('#pragma once\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_WARN 5\n#define ANDROID_LOG_ERROR 6\nstatic inline int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}\n')
            (inc/'sys/system_properties.h').write_text('#pragma once\n#define PROP_VALUE_MAX 92\nstatic inline int __system_property_get(const char*k,char*v){(void)k;*v=0;return 0;}\n')
            common = [os.environ.get('CC','cc'),'-std=c11','-D_POSIX_C_SOURCE=200809L','-O0']
            common += ['-I'+str(p) for p in (inc,NATIVE/'direct',NATIVE/'nvn',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include')]
            common += [str(ROOT/'android/tests/nvn_vk_dispatch_fq_test.c'),str(NATIVE/'direct/vk_record.c')]
            common += [str(NATIVE/p) for p in ('direct/bind_defer.c','direct/t2t_plan.c','direct/vk_clear.c','direct/vk_device.c','direct/gs_path.c','nvn/nvn_vk_format.c','nvn/nvn_vk_query.c')]
            common += (['-L/opt/homebrew/lib','-lvulkan'] if sys.platform == 'darwin' else ['-Wl,-l:libvulkan.so.1']) + ['-lm']
            builds=[('normal',None),('no-enqueue','TEST_NO_ENQUEUE'),('no-frame-id','TEST_NO_FRAMEID'),
                    ('no-detach','TEST_NO_DETACH'),('no-pool-complete','TEST_NO_POOL_COMPLETE')]
            for label, absent in builds:
                exe=td/label
                built=host_link.run(common+(['-D'+absent] if absent else [])+['-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                modes=['no-seam'] if absent else ['off','enqueue','retire','fallback','deferred','reject','reject-prior','reject-unknown','accepted-failure','full']
                for mode in modes:
                    with self.subTest(mode=mode):
                        run=subprocess.run([str(exe),mode],capture_output=True,text=True)
                        print(mode,run.returncode,run.stdout.strip(),run.stderr.strip(),flush=True)
                        self.assertEqual(run.returncode,0,run.stderr)
            source=(NATIVE/'direct/vk_record.c').read_text()
            controls = [
                ('gate-disabled', 'header.op == OP_DISPATCH && dispatch_fq_on() &&', 'header.op == OP_DISPATCH && 0 &&', 'enqueue', 'breaks[0]==1'),
                ('off-not-synchronous', '(header.op == OP_DISPATCH && !dispatch_fq_try);', '(header.op == OP_DISPATCH && 0);', 'off', 'breaks[1]==1'),
                ('retire-too-new', 'e.frame_id != completed_frame', 'e.frame_id >= nvn_vk_first_draw_frame_id()', 'retire', 'command_freed'),
                ('owned-failure-retried', 'enqueue_rc != S2A_FQ_ENQUEUE_NOT_TAKEN', 'enqueue_rc == S2A_FQ_ENQUEUE_ACCEPTED', 'accepted-failure', '!command_freed'),
                ('drop-reference-hold', 'detached_pool_seq = nvn_vk_first_draw_detach_submit_seq();', 'detached_pool_seq = 0;', 'reject-prior', 'pooled backing retired'),
                ('release-unknown', 'sr != REC_SUBMIT_INCOMPLETE && detached_pool_seq', 'detached_pool_seq', 'reject-unknown', 'pool_active'),
                ('skip-fallback-rebuild', 'if (dispatch_fq_try && (rc == S2A_RECORD_DISPATCH_RETRY_SYNC || (rc == 0 && !out.fq)))', 'if (0 && (rc == S2A_RECORD_DISPATCH_RETRY_SYNC || (rc == 0 && !out.fq)))', 'fallback', 'built_value==guest_value'),
                ('drop-host-barrier', 'VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &tail', 'VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &tail', 'enqueue', 'VK_PIPELINE_STAGE_HOST_BIT'),
            ]
            for name, before, after, mode, assertion in controls:
                with self.subTest(control=name):
                    self.assertEqual(source.count(before), 1, name)
                    mutated=td/(name+'.c');mutated.write_text(source.replace(before,after))
                    exe=td/name
                    cmd=[str(mutated) if x==str(NATIVE/'direct/vk_record.c') else x for x in common]
                    built=host_link.run(cmd+['-o',str(exe)],capture_output=True,text=True)
                    self.assertEqual(built.returncode,0,built.stderr)
                    run=subprocess.run([str(exe),mode],capture_output=True,text=True)
                    print(name,'expected red',run.returncode,run.stderr.strip(),flush=True)
                    self.assertNotEqual(run.returncode,0,name+' negative control unexpectedly passed')
                    self.assertIn(assertion,run.stderr)

if __name__ == '__main__': unittest.main()
