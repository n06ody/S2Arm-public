#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Production compute builder with Vulkan mocks; not device or shader execution."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols
sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）
class ComputePipeline(unittest.TestCase):
    def test_builder_and_negative_controls(self):
        host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64' if sys.platform.startswith('linux') else None
        if not host:
            raise RuntimeError('compute host test requires Linux or macOS')
        ndk = link_symbols.find_ndk()
        if ndk is None:
            raise RuntimeError('compute host test needs Android NDK 29.0.14206865; no download or skip')
        vulkan = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include/vulkan'
        if not (vulkan / 'vulkan.h').is_file():
            raise RuntimeError('NDK Vulkan headers missing: ' + str(vulkan))
        native=ROOT/'android/native'
        with tempfile.TemporaryDirectory(prefix='compute-pipeline-') as temp:
            out=Path(temp); (out/'vulkan').symlink_to(vulkan)
            (out/'vk_video').symlink_to(vulkan.parent/'vk_video')
            source=(native/'nvn/nvn_vk_compute.c').read_text()
            variants={'positive':source,
              'negative_true_uses_false_binding':source.replace('r->kind == 1 && p.has_unaligned_storage_buffer', '0'),
              'negative_original_span_copyback':source.replace('o->guest.write(o->guest.opaque, b->guest, b->mapped, (size_t)b->size)', 'o->guest.write(o->guest.opaque, b->guest + (b->size == 12 ? 4 : 0), (uint8_t *)b->mapped + (b->size == 12 ? 4 : 0), (size_t)b->size - (b->size == 12 ? 4 : 0))'),
              'negative_fake_prefix':source.replace('    VkShaderModuleCreateInfo sm =', '    if (p.has_unaligned_storage_buffer) ((uint8_t *)o->buffers[1].mapped)[0] = 0;\n    VkShaderModuleCreateInfo sm ='),
              # 合并后变量名不同（target/pbase/psize）：按本线实现改；语义仍是“撤池查询/信指针旁长度字/
              # 不查池就放行/UBO_PTR 错走 ssbo 表”。
              'negative_indirect_no_pool_query':source.replace('uint64_t extent = psize - (target - pbase);', 'uint64_t extent = ubo_size;'),
              'negative_indirect_trust_desc_size':source.replace('uint64_t extent = psize - (target - pbase);', 'uint64_t extent = 0; if (memory->read(memory->opaque, ubo_addr + mapping->offset + 8, &extent, 4)) extent = ubo_size;'),
              'negative_indirect_accept_unmapped':source.replace('s2a_vk_adapter_pool_find(target, 1, &pbase, &psize)', '0'),
              'negative_indirect_wrong_table':source.replace('slot = (int)mapping->index; use_ubo = 1;', 'slot = (int)mapping->index; use_ubo = 0;'),
              'negative_trunc_silent':source.replace('if (extent > NVK_COMPUTE_INDIRECT_MAX_BYTES) {\n                    indirect_trunc_diag(s, mapping, extent);', 'if (extent > NVK_COMPUTE_INDIRECT_MAX_BYTES) {\n                    if (0) indirect_trunc_diag(s, mapping, extent);'),
              'negative_zero_scale':source.replace('{0, 0, 0x80, 0x3f}', '{0, 0, 0, 0}'),
              'negative_keep_unused_support':source.replace('if (!p.support_read_count && !p.support_reads_unknown) {', 'if (0) {'),
              'negative_wrong_profile':source.replace('profile_storage_alignment != props.limits.minStorageBufferOffsetAlignment','0'),
              'negative_ignore_rw':source.replace('r->kind == 1 && p.storage[i].writable','r->kind == 1'),
              'negative_fake_c0':source.replace('memcpy(c0 + r->offset, value + r->offset - base, r->size);','(void)value; memset(c0 + r->offset, 0, r->size);'),
              'negative_early_release':source.replace('if (completion == -2) return 0;', 'if (completion == -2) { destroy(o); return 0; }'),
              'negative_no_copyback':source.replace('if (o->guest.write(o->guest.opaque, b->guest, b->mapped, (size_t)b->size)) rc = -1;', 'if (0 && o->guest.write(o->guest.opaque, b->guest, b->mapped, (size_t)b->size)) rc = -1;'),
              'negative_ignore_alignment':source.replace('!align || b->guest % align', '!align'),
              'negative_slot_guess':source.replace('slot = (int)mapping->index; use_ubo = 0;', 'slot = r->binding; use_ubo = 0;'),
              # compute 纹理路径的负对照：未绑定槽、忽略解析失败、错误描述符类型、不解除保活、非 GENERAL 布局
              'negative_tex_unbound':source.replace('if (!nvk_compute_texture_snapshot_find(snapshot, (unsigned)r->tex_slot)) {', 'if (0) {'),
              'negative_tex_wrong_type':source.replace('case 2: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;', 'case 2: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;'),
              'negative_tex_leak_pin':source.replace('nvk_compute_texture_snapshot_release(&o->snapshot);', '/* mutation: keep-alive leaked */'),
              'negative_tex_not_transferred':source.replace('o->snapshot = *snapshot;', '/* mutation: ownership not transferred */'),
              'negative_tex_wrong_view':source.replace('ii[i].imageView = tb->view;', 'ii[i].imageView = (VkImageView)0xdead;'),
              'negative_tex_wrong_sampler':source.replace('ii[i].sampler = tb->sampler;', 'ii[i].sampler = (VkSampler)0x33;'),
              'negative_tex_no_general':source.replace('ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;', 'ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;'),
              # PSO 缓存负对照：永不命中（= 修前行为）→ 门开断言 pipe_calls==1 变红
              'negative_pso_never_hit':source.replace('        if (hit) {', '        if (0) {'),
              'drop-c1-binding':source.replace('} else if (r->kind == 0 && r->slot == 1) {', '} else if (0) {'),
              'c1-from-guest-read':source.replace('memcpy(b->mapped, source->c1, NVK_COMPUTE_C1_BYTES);', 'memcpy(b->mapped, b->mapped, 0);')}
            for name,code in variants.items():
                self.assertTrue(name=='positive' or code!=source)
                (out/'nvn').mkdir(exist_ok=True)
                (out/'nvn/nvn_vk_compute.c').write_text(code)
                exe=out/name
                cmd=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror']
                cmd+=['-I'+str(p) for p in (out,native,native/'nvn',native/'direct',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include')]
                cmd += [str(ROOT/'android/tests/w2_compute_pipeline_test.c'),str(native/'nvn/nvn_vk_compute_texture.c'),str(native/'direct/texture_facts.c'),'-o',str(exe)]
                host_link.run(cmd,check=True)
                result=subprocess.run([str(exe)],capture_output=True,text=True)
                print(name,result.returncode,result.stdout.strip(),result.stderr.strip())
                if name=='positive': self.assertEqual(result.returncode,0,result.stderr)
                else:self.assertNotEqual(result.returncode,0,'mutation must fail')
            # 池查询缝缺席单独构建：测试文件在 -DS2A_TEST_NO_POOL_FIND 下不提供 strong
            # s2a_vk_adapter_pool_find（weak 为 NULL），生产必须准确拒 "pool lookup is unavailable"。
            (out/'nvn/nvn_vk_compute.c').write_text(source)
            exe2=out/'positive_no_pool_seam'
            cmd2=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function']
            cmd2+=['-I'+str(q) for q in (out,native,native/'nvn',native/'direct',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include')]
            cmd2+=['-DS2A_TEST_NO_POOL_FIND',str(ROOT/'android/tests/w2_compute_pipeline_test.c'),str(native/'nvn/nvn_vk_compute_texture.c'),str(native/'direct/texture_facts.c'),'-o',str(exe2)]
            host_link.run(cmd2,check=True)
            result=subprocess.run([str(exe2)],capture_output=True,text=True)
            print('positive_no_pool_seam',result.returncode,result.stdout.strip(),result.stderr.strip())
            self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__':unittest.main()
