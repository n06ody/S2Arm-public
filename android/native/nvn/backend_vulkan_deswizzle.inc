// Vulkan 1.1 GPU block-linear decode. The source layout is common NVN layout;
// the shader writes a linear buffer, which is copied to the image afterwards.
static void deswizzle_close(void) {
    if (g_deswizzle_count || g_deswizzle_fallbacks) {
        char line[300]; snprintf(line, sizeof line, "GPU 解排：次数=%llu 源字节=%llu 回退=%llu（管线=%llu BC=%llu 格式展开=%llu 深度=%llu 设备上限=%llu）",
            (unsigned long long)g_deswizzle_count, (unsigned long long)g_deswizzle_bytes, (unsigned long long)g_deswizzle_fallbacks,
            (unsigned long long)g_deswizzle_reasons[0], (unsigned long long)g_deswizzle_reasons[1], (unsigned long long)g_deswizzle_reasons[2],
            (unsigned long long)g_deswizzle_reasons[3], (unsigned long long)g_deswizzle_reasons[4]); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
    if (g_deswizzle.pipe) vkDestroyPipeline(g.d, g_deswizzle.pipe, NULL);
    if (g_deswizzle.module) vkDestroyShaderModule(g.d, g_deswizzle.module, NULL);
    if (g_deswizzle.layout) vkDestroyPipelineLayout(g.d, g_deswizzle.layout, NULL);
    if (g_deswizzle.dsl) vkDestroyDescriptorSetLayout(g.d, g_deswizzle.dsl, NULL);
    memset(&g_deswizzle, 0, sizeof g_deswizzle);
}
static int deswizzle_open(void) {
    if (!nvn_deswizzle_geom || !nvn_deswizzle || !nvn_tex_layout || !nvn_tex_level_offset || !nvn_tex_level_gobs || !&nvn_gob_switch2) return 0;
    VkDescriptorSetLayoutBinding b[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b };
    if (vkCreateDescriptorSetLayout(g.d, &di, NULL, &g_deswizzle.dsl) != VK_SUCCESS) goto bad;
    VkPushConstantRange pr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 32 };
    VkPipelineLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_deswizzle.dsl,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (vkCreatePipelineLayout(g.d, &li, NULL, &g_deswizzle.layout) != VK_SUCCESS) goto bad;
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof nvk_deswizzle_spv, .pCode = nvk_deswizzle_spv };
    if (vkCreateShaderModule(g.d, &mi, NULL, &g_deswizzle.module) != VK_SUCCESS) goto bad;
    VkComputePipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = g_deswizzle.module, .pName = "main" }, .layout = g_deswizzle.layout };
    if (vkCreateComputePipelines(g.d, VK_NULL_HANDLE, 1, &pi, NULL, &g_deswizzle.pipe) != VK_SUCCESS) goto bad;
    return 1;
bad:
    deswizzle_close(); return 0;
}
uint64_t nvn_gpu_vk_test_deswizzle_count(void) { return __atomic_load_n(&g_deswizzle_count, __ATOMIC_RELAXED); }
uint64_t nvn_gpu_vk_test_deswizzle_fallback_count(void) { return __atomic_load_n(&g_deswizzle_fallbacks, __ATOMIC_RELAXED); }
static void deswizzle_fallback(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer,
                               const void *src, uint32_t row, uint32_t rows, uint32_t slices, unsigned reason, const char *why) {
    __atomic_add_fetch(&g_deswizzle_fallbacks, 1, __ATOMIC_RELAXED);
    uint64_t count = __atomic_add_fetch(&g_deswizzle_reasons[reason], 1, __ATOMIC_RELAXED);
    if (count == 1) { char b[192]; snprintf(b, sizeof b, "GPU 解排回退 CPU：%s", why); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
    uint64_t bytes = (uint64_t)row * rows * slices;
    if (!bytes || bytes > SIZE_MAX) { set_sticky(c, NVN_GPU_E_ARG, "解排回退尺寸非法"); return; }
    uint8_t *linear = malloc((size_t)bytes);
    if (!linear) { set_sticky(c, NVN_GPU_E_OOM, "解排回退分配失败"); return; }
    NVNcopyRegion region = {0, 0, 0, (int)(t->w >> level ? t->w >> level : 1), (int)(t->h >> level ? t->h >> level : 1), (int)slices};
    if (nvn_deswizzle(&t->original_desc, level, layer, src, linear, row)) set_sticky(c, NVN_GPU_E_ARG, "解排回退失败");
    else nvn_gpu_tex_upload(c, t, level, layer, &region, linear, row, bytes);
    free(linear);
}
void nvn_gpu_tex_upload_blocklinear(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const nvn_texlayout *L,
                                    const void *src, uint64_t bytes) {
    struct nvn_gpu_cmd_s *c = ch; struct nvn_gpu_tex_s *t = th;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "GPU 解排在通道内"); return; }
    if (!t || !src || !L || bytes < L->size || !L->size || !nvn_deswizzle_geom || !nvn_deswizzle || !nvn_tex_layout || !nvn_tex_level_offset || !nvn_tex_level_gobs) {
        set_sticky(c, NVN_GPU_E_ARG, "GPU 解排参数或布局来源无效"); return;
    }
    nvn_texlayout expected; nvn_tex_layout(&t->original_desc, &expected);
    if (L->size != expected.size || L->layer_size != expected.layer_size || L->align != expected.align || L->layers != expected.layers ||
        L->bpb != expected.bpb || L->tileH != expected.tileH || L->tileD != expected.tileD) {
        set_sticky(c, NVN_GPU_E_ARG, "GPU 解排布局不匹配贴图描述"); return;
    }
    uint32_t row, rows, slices, bpb; uint64_t base; unsigned h, d;
    if (level >= 32 || nvn_deswizzle_geom(&t->original_desc, level, layer, &row, &rows, &slices, &base, &h, &d, &bpb) || !row || !rows || !slices || !bpb) {
        set_sticky(c, NVN_GPU_E_ARG, "GPU 解排子资源几何非法"); return;
    }
    uint64_t lay_base;
    if (__builtin_mul_overflow((uint64_t)layer, L->layer_size, &lay_base)) { set_sticky(c, NVN_GPU_E_ARG, "GPU 解排层偏移溢出"); return; }
    uint64_t end_offset = level + 1 < t->levels ? nvn_tex_level_offset(&t->original_desc, level + 1) : L->layer_size;
    uint64_t next;
    if (__builtin_add_overflow(lay_base, end_offset, &next)) { set_sticky(c, NVN_GPU_E_ARG, "GPU 解排末尾溢出"); return; }
    if (base > next || next > L->size || next > bytes) { set_sticky(c, NVN_GPU_E_ARG, "GPU 解排源子资源越界"); return; }
    uint64_t raw = next - base, linear;
    if (__builtin_mul_overflow((uint64_t)row, (uint64_t)rows, &linear) || __builtin_mul_overflow(linear, (uint64_t)slices, &linear) || linear > SIZE_MAX - 3 || raw > SIZE_MAX - 3) {
        set_sticky(c, NVN_GPU_E_ARG, "GPU 解排字节范围溢出"); return;
    }
    uint64_t padded = (linear + 3u) & ~3ull;
    uint64_t src_padded = (raw + 3u) & ~3ull;
    uint64_t groups = ((linear + 3u) / 4u + 63u) / 64u;
    uint32_t gobs = nvn_tex_level_gobs(&t->original_desc, level);
    if (gobs < ((uint64_t)row + 63u) / 64u) gobs = (row + 63u) / 64u;
    const char *why = NULL; unsigned fallback = 0;
    if (!g_deswizzle.pipe) { fallback = 0; why = "pipeline_unavailable"; }
    else if (t->bc_decoded) { fallback = 1; why = "bc_decode"; }
    else if (nvn_fmt_expand_target(t->nvn_format, NULL, NULL)) { fallback = 2; why = "format_expand"; }
    else if (nvn_vk_format_substituted(t->nvn_format) || t->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { fallback = 3; why = "depth_stencil"; }
    else if (!raw || h > 15 || d > 15 || h + d > 22 || !gobs || !linear || linear > UINT32_MAX - 3u ||
        src_padded > UINT32_MAX || src_padded > g.dev->properties.limits.maxStorageBufferRange || padded > g.dev->properties.limits.maxStorageBufferRange ||
        groups > g.dev->properties.limits.maxComputeWorkGroupCount[0] ||
        g.dev->properties.limits.minStorageBufferOffsetAlignment > UINT32_MAX) { fallback = 4; why = "device_limits"; }
    if (why) { deswizzle_fallback(c, t, level, layer, src, row, rows, slices, fallback, why); return; }
    uint32_t align = (uint32_t)g.dev->properties.limits.minStorageBufferOffsetAlignment; if (align < 4) align = 4;
    if (bpb > align && !(bpb & (bpb - 1))) align = bpb;
    nvn_gpu_slice input, output; nvn_gpu_err e = {0};
    if (!rc_is(nvn_gpu_arena_alloc(c, src_padded, align, &input, &e), NVN_GPU_OK) ||
        !rc_is(nvn_gpu_arena_alloc(c, padded, align, &output, &e), NVN_GPU_OK)) {
        set_sticky(c, e.rc.code, "GPU 解排暂存：%s", e.why); return;
    }
    memcpy(input.cpu, (const uint8_t *)src + base, (size_t)raw);
    if (src_padded > raw) memset(input.cpu + raw, 0, (size_t)(src_padded - raw));
    VkDescriptorSet set;
    if (!alloc_sets(c, &g_deswizzle.dsl, 1, &set, NULL, 0, 0)) return;
    VkDescriptorBufferInfo bi[2] = { { input.buf->buffer, input.off, src_padded }, { output.buf->buffer, output.off, padded } };
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] },
    };
    vkUpdateDescriptorSets(g.d, 2, wr, 0, NULL);
    VkBufferMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = input.buf->buffer, .offset = input.off, .size = src_padded };
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
    uint32_t params[8] = { row, rows, slices, h, d, gobs, (uint32_t)nvn_gob_switch2, (uint32_t)linear };
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_deswizzle.pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_deswizzle.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(c->cb, g_deswizzle.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, params);
    vkCmdDispatch(c->cb, (uint32_t)groups, 1, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.buffer = output.buf->buffer; barrier.offset = output.off; barrier.size = padded;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
    es_reset(c); c->desc_layout = VK_NULL_HANDLE; c->desc_dirty = 0xfu;
    NVNcopyRegion region = {0, 0, 0, (int)(t->w >> level ? t->w >> level : 1), (int)(t->h >> level ? t->h >> level : 1), (int)slices};
    copy_buf_tex_impl(c, output.buf, output.off, row, linear, t, level, layer, &region, COPY_FACE_DEFAULT, 1);
    if (rc_is(c->sticky, NVN_GPU_OK)) { __atomic_add_fetch(&g_deswizzle_count, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_deswizzle_bytes, raw, __ATOMIC_RELAXED); }
}
