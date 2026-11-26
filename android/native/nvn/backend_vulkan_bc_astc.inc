// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// BC 源块与 ASTC 目标块均按块行紧排。计算输出先进段内缓冲，由传输命令写入压缩图像。

static void bc_astc_close(struct bc_astc_pipeline *pipeline) {
    if (pipeline->pipe) vkDestroyPipeline(g.d, pipeline->pipe, NULL);
    if (pipeline->layout) vkDestroyPipelineLayout(g.d, pipeline->layout, NULL);
    if (pipeline->dsl) vkDestroyDescriptorSetLayout(g.d, pipeline->dsl, NULL);
    memset(pipeline, 0, sizeof *pipeline);
}

static int bc_astc_open(struct bc_astc_pipeline *pipeline, const uint32_t *code, size_t size, int has_tables) {
    const VkDescriptorSetLayoutBinding bindings[] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    const VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = has_tables ? 3u : 2u, .pBindings = bindings,
    };
    VkShaderModule shader = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(g.d, &di, NULL, &pipeline->dsl) != VK_SUCCESS) goto fail;
    const VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
    const VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &pipeline->dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    if (vkCreatePipelineLayout(g.d, &li, NULL, &pipeline->layout) != VK_SUCCESS) goto fail;
    const VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size, .pCode = code,
    };
    if (vkCreateShaderModule(g.d, &mi, NULL, &shader) != VK_SUCCESS) goto fail;
    const VkComputePipelineCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pipeline->layout,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" },
    };
    VkResult result = vkCreateComputePipelines(g.d, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline->pipe);
    vkDestroyShaderModule(g.d, shader, NULL);
    if (result == VK_SUCCESS) return 1;
fail:
    bc_astc_close(pipeline);
    return 0;
}

static int bc_astc_can_encode(uint64_t blocks) {
    const VkPhysicalDeviceLimits *limits = &g.dev->properties.limits;
    return blocks && blocks <= UINT32_MAX &&
        16 + blocks * 16 <= limits->maxStorageBufferRange &&
        (blocks + 63) / 64 <= limits->maxComputeWorkGroupCount[0] &&
        limits->minStorageBufferOffsetAlignment <= UINT32_MAX;
}

static int bc_astc_storage(uint32_t nvn_format, const nvn_gpu_tex_desc *d, VkImageType type,
                            uint32_t layers, VkImageCreateFlags flags, VkFormat *format) {
    const VkPhysicalDeviceFeatures *features = &g.dev->core_features;
    if (!(d->usage & NVN_GPU_TEX_SAMPLED) || (d->usage & (NVN_GPU_TEX_RENDER | NVN_GPU_TEX_STORAGE)) ||
        type != VK_IMAGE_TYPE_2D || d->d.samples > 1 ||
        features->textureCompressionBC || !features->textureCompressionASTC_LDR) return 0;
    const int bc7 = nvn_format == NVN_FORMAT_BPTC_UNORM;
    if (bc7) {
        if (!(d->usage & NVN_GPU_TEX_BC7_VALIDATED) || !g.bc7_astc_on) return 0;
    } else if (!(d->usage & NVN_GPU_TEX_BC1_OPAQUE) || !g.bc1_astc_on ||
               (nvn_format != NVN_FORMAT_RGBA_DXT1 && nvn_format != NVN_FORMAT_RGBA_DXT1_SRGB)) return 0;
    if (!d->d.width || !d->d.height ||
        (!bc7 && !bc_astc_can_encode(((uint64_t)d->d.width + 3) / 4 * (((uint64_t)d->d.height + 3) / 4)))) return 0;
    const VkFormat astc = nvn_format == NVN_FORMAT_RGBA_DXT1_SRGB ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d->usage & NVN_GPU_TEX_VIEW_FMT) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImageFormatProperties props;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, astc, &fp);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & required) != required ||
        vkGetPhysicalDeviceImageFormatProperties(g.phys, astc, type, VK_IMAGE_TILING_OPTIMAL, usage, flags, &props) != VK_SUCCESS ||
        (uint32_t)d->d.width > props.maxExtent.width || (uint32_t)d->d.height > props.maxExtent.height ||
        (uint32_t)d->d.levels > props.maxMipLevels || layers > props.maxArrayLayers) return 0;
    struct bc_astc_pipeline *pipeline = bc7 ? &g_bc7_astc : &g_bc1_astc;
    const uint32_t *code = bc7 ? nvk_bc7_hybrid_spv : nvk_bc1_astc_spv;
    const size_t size = bc7 ? sizeof nvk_bc7_hybrid_spv : sizeof nvk_bc1_astc_spv;
    pthread_mutex_lock(&g_submit_mu);
    int ready = pipeline->pipe || bc_astc_open(pipeline, code, size, bc7);
    pthread_mutex_unlock(&g_submit_mu);
    if (!ready) {
        nvn_plat_log(NVN_PLAT_LOG_RED, "tex", "BC ASTC 计算管线创建失败，使用原 RGBA8 解码");
        return 0;
    }
    *format = astc;
    return 1;
}

static int bc_astc_image(const struct nvn_gpu_tex_s *t) {
    return t->bc_decoded && (t->format == VK_FORMAT_ASTC_4x4_UNORM_BLOCK || t->format == VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
}

// 在宿主紧凑暂存快照上核准入。GPU 计算也标记这同一条件，供离线对拍。
static int bc1_astc_opaque(const void *source, uint64_t bytes) {
    const uint8_t *data = source;
    for (uint64_t at = 0; at < bytes; at += 8) {
        uint32_t endpoints, selectors;
        memcpy(&endpoints, data + at, 4);
        memcpy(&selectors, data + at + 4, 4);
        if ((endpoints & 0xffffu) <= (endpoints >> 16) &&
            ((selectors & (selectors >> 1)) & 0x55555555u)) return 0;
    }
    return 1;
}

static uint32_t bc_astc_alignment(void) {
    uint32_t align = (uint32_t)g.dev->properties.limits.minStorageBufferOffsetAlignment;
    return align < 16 ? 16 : align;
}

// 源来自已拷入本段且不会再动的 BC 块。整图通过准入检查后才录制。
static int bc_astc_encode(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkBuffer src, uint64_t offset,
                           uint64_t blocks, uint32_t block_bytes, uint32_t level, uint32_t layer,
                          VkOffset3D off, VkExtent3D ext) {
    const struct bc_astc_pipeline *pipeline = &g_bc1_astc;
    nvn_gpu_slice output;
    nvn_gpu_err err = {0};
    if (!bc_astc_can_encode(blocks) ||
        !rc_is(nvn_gpu_arena_alloc(c, 16 + blocks * 16, bc_astc_alignment(), &output, &err), NVN_GPU_OK)) {
        set_sticky(c, NVN_GPU_E_OOM, "BC ASTC 输出暂存不可用：%s", err.why);
        return 0;
    }
    memset(output.cpu, 0, 16);
    VkDescriptorSet set;
    if (!alloc_sets(c, &pipeline->dsl, 1, &set, NULL, 0, 0)) {
        set_sticky(c, NVN_GPU_E_OOM, "BC ASTC 描述符集分配失败");
        return 0;
    }
    const VkDescriptorBufferInfo buffers[] = {
        {src, offset, blocks * block_bytes}, {output.buf->buffer, output.off, 16 + blocks * 16},
    };
    const VkWriteDescriptorSet writes[] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[1] },
    };
    vkUpdateDescriptorSets(g.d, 2, writes, 0, NULL);
    const uint32_t count = (uint32_t)blocks;
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(c->cb, pipeline->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof count, &count);
    int time_query = bc_time_begin(c, BC_TIME_ASTC, t->nvn_format, blocks);
    GPU_PT_RECORD(c, GPU_PT_ASTC_ENCODE, vkCmdDispatch(c->cb, (count + 63) / 64, 1, 1));
    bc_time_end(c, time_query);
    const VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = output.buf->buffer, .offset = output.off + 16, .size = blocks * 16,
    };
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy copy = {
        .bufferOffset = output.off + 16, .imageOffset = off, .imageExtent = ext,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
    };
    GPU_PT_RECORD(c, GPU_PT_TRANSFER, vkCmdCopyBufferToImage(c->cb, output.buf->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy));
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    es_reset(c);
    c->desc_layout = VK_NULL_HANDLE;
    c->desc_dirty = 0xfu;
    cmd_ref(c, t, REF_TEX);
    return 1;
}
