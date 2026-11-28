// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "backend_vulkan_mdi_spv.h"

uint32_t nvn_vk_mdi_native_limit(void) {
    return (g.caps.caps & NVN_GPU_CAP_INDIRECT_COUNT) ? g.dev->properties.limits.maxDrawIndirectCount : 0;
}

static nvn_gpu_pipe mdi_mask_pipe(void) {
    if (!g.mdi_mask_shader) {
        const nvn_gpu_layout_ent entries[] = {
            {.binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
            {.binding = 1, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
            {.binding = 2, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        };
        const nvn_gpu_layout layout = {.ent = entries, .nent = 3, .push_bytes = 5 * sizeof(uint32_t)};
        const nvn_gpu_shader_src src = {.stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .entry = "main",
                                        .blob = mdi_mask_spv, .bytes = sizeof mdi_mask_spv, .layout = &layout};
        if (!nvn_gpu_ok(nvn_gpu_shader_load(&src, &g.mdi_mask_shader, NULL))) return NULL;
    }
    const nvn_gpu_stage_desc stage = {.sh = g.mdi_mask_shader};
    nvn_gpu_pipe pipe = NULL;
    return nvn_gpu_ok(nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &pipe, NULL)) ? pipe : NULL;
}

static int mdi_read_binding(nvn_gpu_binding *binding, nvn_gpu_buf buf, uint64_t offset, uint64_t bytes, uint32_t align) {
    if (!buf || !buf_range(buf, offset, bytes)) return 0;
    const uint64_t first = offset - offset % align;
    const uint64_t length = (offset - first + bytes + 3) & ~UINT64_C(3);
    if (length > UINT32_MAX || length > g.caps.max_ssbo_range || !buf_range(buf, first, length)) return 0;
    binding->u.b.buf = buf;
    binding->u.b.off = first;
    binding->u.b.len = length;
    return 1;
}

// 普通 Vulkan 间接绘制也能在没有 KHR count 的设备上消费 GPU 计数：
// 先按计数把无效命令置零，统一拷到紧密且对齐的间接参数，再沿用原绘制路径。
int nvn_vk_mdi_prepare(nvn_gpu_cmd c, nvn_gpu_draw_args *a, int mask) {
    if (!c || c->in_pass || !rc_is(c->sticky, NVN_GPU_OK) || !a->max_draws ||
        !g.dev->core_features.drawIndirectFirstInstance) return 0;
    const uint32_t size = a->ibuf ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
    const uint32_t groups = mask ? (a->max_draws - 1) / 64 + 1 : 1;
    const uint64_t src_bytes = (uint64_t)(a->max_draws - 1) * a->stride + size;
    const uint64_t result_bytes = mask ? (uint64_t)a->max_draws * size : sizeof(uint32_t);
    const uint32_t align = g.caps.min_ssbo_align > 4 ? g.caps.min_ssbo_align : 4;
    if (groups > g.dev->properties.limits.maxComputeWorkGroupCount[0] ||
        result_bytes > UINT32_MAX || result_bytes > g.caps.max_ssbo_range) return 0;
    nvn_gpu_binding bindings[] = {
        {.ent = 0, .cls = NVN_GPU_RES_SSBO}, {.ent = 1, .cls = NVN_GPU_RES_SSBO},
        {.ent = 2, .cls = NVN_GPU_RES_SSBO},
    };
    if (!mdi_read_binding(&bindings[1], a->cnt, a->cnt_off, 4, align)) return 0;
    if (mask) {
        if (!mdi_read_binding(&bindings[0], a->ind, a->ind_off, src_bytes, align)) return 0;
    } else {
        bindings[0].u.b = bindings[1].u.b;   // 只钳计数，但描述符布局保持相同
    }
    nvn_gpu_pipe pipe = mdi_mask_pipe();
    if (!pipe) return 0;
    nvn_gpu_slice result;
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, result_bytes, align, &result, NULL))) return 0;
    bindings[2].u.b.buf = result.buf;
    bindings[2].u.b.off = result.off;
    bindings[2].u.b.len = result_bytes;
    const uint32_t push[] = {mask ? (uint32_t)(a->ind_off - bindings[0].u.b.off) : 0,
                             (uint32_t)(a->cnt_off - bindings[1].u.b.off),
                             a->stride, mask ? size / 4 : 0, a->max_draws};
    // 同一段里计数及参数的最近写者可能是计算、传输或前一通道。
    full_barrier(c);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bindings, 3);
    nvn_gpu_push(c, NVN_GPU_SH_CS, 0, push, sizeof push);
    nvn_gpu_dispatch(c, pipe, groups, 1, 1, NULL, 0);
    nvn_gpu_compute_to_graphics(c);
    if (!rc_is(c->sticky, NVN_GPU_OK)) return -1;
    if (mask) {
        a->ind = result.buf;
        a->ind_off = result.off;
        a->stride = size;
        a->cnt = NULL;
        a->cnt_off = 0;
    } else {
        // VK 的上限限制实际计数缓冲内容，不能仅依赖 maxDrawCount 参数。
        a->cnt = result.buf;
        a->cnt_off = result.off;
    }
    return 1;
}
