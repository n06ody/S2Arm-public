// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "backend_vulkan_rgb_readback_spv.h"

static void rgb_readback_close(void) {
    if (g_rgb_readback.pipe) vkDestroyPipeline(g.d, g_rgb_readback.pipe, NULL);
    if (g_rgb_readback.layout) vkDestroyPipelineLayout(g.d, g_rgb_readback.layout, NULL);
    if (g_rgb_readback.dsl) vkDestroyDescriptorSetLayout(g.d, g_rgb_readback.dsl, NULL);
    memset(&g_rgb_readback, 0, sizeof g_rgb_readback);
}

static int rgb_readback_open(void) {
    const VkDescriptorSetLayoutBinding binds[] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    const VkDescriptorSetLayoutCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = 2, .pBindings = binds };
    if (vkCreateDescriptorSetLayout(g.d, &di, NULL, &g_rgb_readback.dsl) != VK_SUCCESS) goto fail;
    const VkPushConstantRange range = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 6 * sizeof(uint32_t) };
    const VkPipelineLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1, .pSetLayouts = &g_rgb_readback.dsl,
                                            .pushConstantRangeCount = 1, .pPushConstantRanges = &range };
    if (vkCreatePipelineLayout(g.d, &li, NULL, &g_rgb_readback.layout) != VK_SUCCESS) goto fail;
    const VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .codeSize = sizeof k_rgb_readback, .pCode = k_rgb_readback };
    VkShaderModule shader;
    if (vkCreateShaderModule(g.d, &mi, NULL, &shader) != VK_SUCCESS) goto fail;
    const VkComputePipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = shader, .pName = "main" }, .layout = g_rgb_readback.layout };
    VkResult r = vkCreateComputePipelines(g.d, VK_NULL_HANDLE, 1, &pi, NULL, &g_rgb_readback.pipe);
    vkDestroyShaderModule(g.d, shader, NULL);
    if (r == VK_SUCCESS) return 1;
fail:
    rgb_readback_close();
    return 0;
}

// Copy a strip of host RGBA texels to scratch, remove alpha on the GPU,
// and copy only the three guest components into each destination row.
static void rgb_readback_copy(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer,
                              const NVNcopyRegion *region, struct nvn_gpu_buf_s *dst, uint64_t dst_off,
                              uint64_t row, uint64_t image_bytes) {
    const int volume = t->target == NVN_TEXTURE_TARGET_3D;
    NVNcopyRegion uvw = {0};
    if (volume && layer < t->depth) {
        if (region) uvw = *region;
        uvw.zoffset = (int)layer;
        if (!uvw.depth) uvw.depth = 1;
        region = &uvw;
    }
    VkOffset3D off;
    VkExtent3D ext;
    nvn_fmt_info fi;
    int host_pixel;
    if (!nvn_fmt_info_of(t->nvn_format, &fi) || !nvn_fmt_expand_target(t->nvn_format, NULL, &host_pixel)) {
        set_sticky(c, NVN_GPU_E_ARG, "RGB 回读格式非法"); return;
    }
    nvn_fmt_info host;
    if (!nvn_fmt_info_of(fi.expand, &host)) { set_sticky(c, NVN_GPU_E_ARG, "展开存储格式未知"); return; }
    uint64_t plane;
    if (t->samples != 1) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "RGB 回读不支持多重采样"); return;
    }
    if (!dst || level >= t->levels || level >= 32 || layer >= (volume ? t->depth : t->layers) ||
        (region && (region->width < 0 || region->height < 0 || region->depth < 0)) ||
        copy_region(t, level, region, &off, &ext) || !ext.width || !ext.height || !ext.depth ||
        row < (uint64_t)ext.width * fi.bytes ||
        __builtin_mul_overflow(row, (uint64_t)ext.height, &plane) || ext.depth > image_bytes / plane ||
        !buf_range(dst, dst_off, image_bytes)) {
        set_sticky(c, NVN_GPU_E_ARG, "RGB 回读：区域、偏移或行距非法"); return;
    }
    if (t->scaled32_bytes) {
        if (t->scaled32_unknown || (t->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "32 位展开原码无法确认最近的 GPU 写入"); return;
        }
        struct scaled_rows from = {0};
        if (!scaled_region(c, t, level, layer, off, &from)) return;
        scaled_copy_rows(c, from, (struct scaled_rows){dst->buffer, dst_off, row, plane}, ext, t->scaled32_bytes);
        for (uint32_t z = 0; z < ext.depth; z++) for (uint32_t y = 0; y < ext.height; y++)
            cold_note_write(c, dst, dst_off + z * plane + y * row, (uint64_t)ext.width * fi.bytes, 0, 0);
        cmd_ref(c, t, REF_TEX); cmd_ref(c, dst, REF_BUF);
        return;
    }
    const VkPhysicalDeviceLimits *limits = &g.dev->properties.limits;
    const uint64_t used_row = (uint64_t)ext.width * fi.bytes;
    const uint64_t packed_row = (used_row + 3u) & ~UINT64_C(3);
    const uint64_t host_row = (uint64_t)ext.width * host_pixel;
    const uint32_t groups_x = (uint32_t)((packed_row + 255) / 256);
    uint64_t widest_row = packed_row > host_row ? packed_row : host_row;
    uint32_t max_rows = (uint32_t)((limits->maxStorageBufferRange & ~3u) / widest_row);
    if (max_rows > limits->maxComputeWorkGroupCount[1]) max_rows = limits->maxComputeWorkGroupCount[1];
    if (max_rows > ext.height) max_rows = ext.height;
    if (!max_rows || groups_x > limits->maxComputeWorkGroupCount[0] || limits->minStorageBufferOffsetAlignment > UINT32_MAX) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "RGB 回读：超过设备的计算缓冲或派发上限"); return;
    }
    uint32_t align = (uint32_t)limits->minStorageBufferOffsetAlignment;
    if (align < 4) align = 4;
    if (align < (uint32_t)host_pixel) align = (uint32_t)host_pixel;
    const uint64_t packed_bytes = packed_row * max_rows;
    const uint64_t source_bytes = (host_row * max_rows + 3u) & ~UINT64_C(3);
    nvn_gpu_slice input, packed;
    nvn_gpu_err err = {0};
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, source_bytes, align, &input, &err)) ||
        !nvn_gpu_ok(nvn_gpu_arena_alloc(c, packed_bytes, align, &packed, &err))) {
        set_sticky(c, err.rc.code, "RGB 回读暂存：%s", err.why); return;
    }
    VkDescriptorSet set;
    if (!alloc_sets(c, &g_rgb_readback.dsl, 1, &set, NULL, 0, 0)) {
        set_sticky(c, NVN_GPU_E_OOM, "RGB 回读描述符分配失败"); return;
    }
    const VkDescriptorBufferInfo bi[] = {
        {input.buf->buffer, input.off, source_bytes}, {packed.buf->buffer, packed.off, packed_bytes},
    };
    const VkWriteDescriptorSet wr[] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .pBufferInfo = &bi[1] },
    };
    const int contiguous = row == used_row && packed_row == used_row;
    VkBufferCopy *copies = calloc(contiguous ? 1 : max_rows, sizeof *copies);
    if (!copies) { set_sticky(c, NVN_GPU_E_OOM, "RGB 回读行表分配失败"); return; }
    vkUpdateDescriptorSets(g.d, 2, wr, 0, NULL);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    for (uint32_t z = 0; z < ext.depth; z++) for (uint32_t y = 0; y < ext.height;) {
        uint32_t rows = ext.height - y;
        if (rows > max_rows) rows = max_rows;
        VkBufferImageCopy strip = {
            .bufferOffset = input.off,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, volume ? 0 : layer, 1},
            .imageOffset = {off.x, off.y + (int32_t)y, off.z + (int32_t)z},
            .imageExtent = {ext.width, rows, 1},
        };
        full_barrier(c);   // 重用同一组暂存前，前一组计算与传输均须完成。
        vkCmdCopyImageToBuffer(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input.buf->buffer, 1, &strip);
        mem_barrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        const uint32_t params[] = {(uint32_t)used_row, fi.bytes, (uint32_t)host_pixel, t->nvn_format, fi.channels, host.bytes / host.channels};
        vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rgb_readback.pipe);
        vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rgb_readback.layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(c->cb, g_rgb_readback.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, params);
        vkCmdDispatch(c->cb, groups_x, rows, 1);
        mem_barrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const uint64_t base = dst_off + z * plane + y * row;
        const uint32_t count = contiguous ? 1 : rows;
        for (uint32_t i = 0; i < count; i++)
            copies[i] = (VkBufferCopy){packed.off + i * packed_row, base + i * row, count == 1 ? used_row * rows : used_row};
        vkCmdCopyBuffer(c->cb, packed.buf->buffer, dst->buffer, count, copies);
        for (uint32_t i = 0; i < count; i++) cold_note_write(c, dst, copies[i].dstOffset, copies[i].size, 0, 0);
        y += rows;
    }
    free(copies);
    c->xfer_dirty = 1;
    es_reset(c);
    c->desc_layout = VK_NULL_HANDLE;
    c->desc_dirty = 0xfu;
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, t, REF_TEX);
    cmd_ref(c, dst, REF_BUF);
}
