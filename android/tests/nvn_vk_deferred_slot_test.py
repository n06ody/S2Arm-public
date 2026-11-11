#!/usr/bin/env python3
"""Actual slot retirement plus a mutation proving the fence gate is tested."""
import shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = HERE.parent / 'native/nvn/nvn_vk_first_draw.c'
class DeferredSlotTest(unittest.TestCase):
    def test_slot_proof_and_mutant(self):
        cc = shutil.which('cc')
        self.assertIsNotNone(cc)
        includes = [HERE/'vkhost', HERE/'hostvk', HERE.parent/'native', HERE.parent/'native/direct', HERE.parent/'native/nvn', ROOT/'runtime/include', ROOT/'graphics/nvn/gfx/nvn', ROOT/'graphics/nvn/gfx/nvn/include', ROOT/'graphics/nvn/gfx/tex', ROOT/'graphics/nvn/third_party/vulkan-headers/include']
        build = ROOT/'build/deferred-slot-test'
        build.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build) as td:
            td = Path(td)
            # Mutate exactly the production failure branch: a missing proof is ignored.
            source = SOURCE.read_text()
            old = 'if (bad) {\n        FD_LOG("present defer: the previous frame has no completion proof; resources retained");'
            new = 'if (0 && bad) {\n        FD_LOG("present defer: the previous frame has no completion proof; resources retained");'
            self.assertEqual(source.count(old), 1)
            mutant = td/'nvn_vk_first_draw_mutant.c'
            mutant.write_text(source.replace(old, new))
            ref_old = 'if (first || g_frame_ref_refresh) (void)nvn_vk_first_draw_submit_seq();'
            self.assertEqual(source.count(ref_old), 1)
            ref_mutant = td/'nvn_vk_first_draw_ref_mutant.c'
            ref_mutant.write_text(source.replace(ref_old, 'if (first) (void)nvn_vk_first_draw_submit_seq();'))
            lock_mutant = td/'nvn_vk_first_draw_lock_mutant.c'
            lock_mutant.write_text(source.replace(ref_old, '(void)first; (void)nvn_vk_first_draw_submit_seq();'))
            frame_old = 's2a_record_fq_retire(s->frame_id);'
            self.assertEqual(source.count(frame_old), 1)
            frame_mutant = td/'nvn_vk_first_draw_frame_mutant.c'
            frame_mutant.write_text(source.replace(frame_old, 's2a_record_fq_retire(g_frame_id);'))
            cases = [('production', SOURCE, None),
                     ('mutant', mutant, 'nvn_vk_first_draw_retire_all()!=0'),
                     ('ref_mutant', ref_mutant, 'g_frame_ref_seq==2 && next_pool_seq==2'),
                     ('lock_mutant', lock_mutant, 'begin_calls==1'),
                     ('frame_mutant', frame_mutant, 'retired_frame==100 && retired_count==1 && unknown_notices==1')]
            for name, target, failure in cases:
                binary = td/name
                cmd = [cc, '-std=gnu11', '-ffunction-sections', '-fdata-sections'] + [f'-I{x}' for x in includes] + [f'-DFIRST_DRAW_SOURCE="{target}"', str(HERE/'nvn_vk_deferred_slot_test.c'), ('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'), '-o', str(binary)]
                if sys.platform == 'darwin': cmd.append('-Wl,-undefined,dynamic_lookup')
                subprocess.run(cmd, check=True)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                if not failure:
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0, 'mutant escaped ownership assertions')
                    self.assertIn(failure, result.stderr)
if __name__ == '__main__': unittest.main()
