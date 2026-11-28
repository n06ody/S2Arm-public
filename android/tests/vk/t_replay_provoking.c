// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main indirect_fixture_main
#include "t_replay_indirect.c"
#undef main

enum control { NORMAL, NO_EXTENSION, NO_LAST, NO_PER_PIPELINE };
static enum control control;
static unsigned ext_enabled, feature_queried, feature_enabled, raster_last;

extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice d, const char *layer, uint32_t *n, VkExtensionProperties *out) {
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(d, layer, n, out);
    if (control != NO_EXTENSION || r != VK_SUCCESS || layer || !out) return r;
    for (uint32_t i = 0; i < *n; i++) {
        if (strcmp(out[i].extensionName, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME)) continue;
        memmove(out + i, out + i + 1, (size_t)(*n - i - 1) * sizeof *out);
        --*n;
        break;
    }
    return r;
}

extern __typeof__(vkGetPhysicalDeviceFeatures2) __real_vkGetPhysicalDeviceFeatures2;
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2 *f) {
    __real_vkGetPhysicalDeviceFeatures2(d, f);
    for (VkBaseOutStructure *p = f->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT) continue;
        feature_queried++;
        if (control == NO_LAST) ((VkPhysicalDeviceProvokingVertexFeaturesEXT *)p)->provokingVertexLast = VK_FALSE;
    }
}

extern __typeof__(vkGetPhysicalDeviceProperties2) __real_vkGetPhysicalDeviceProperties2;
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *f) {
    __real_vkGetPhysicalDeviceProperties2(d, f);
    for (VkBaseOutStructure *p = f->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT && control == NO_PER_PIPELINE)
            ((VkPhysicalDeviceProvokingVertexPropertiesEXT *)p)->provokingVertexModePerPipeline = VK_FALSE;
    }
}

extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice d, const VkDeviceCreateInfo *ci,
    const VkAllocationCallbacks *allocator, VkDevice *out) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        ext_enabled += !strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT) continue;
        const VkPhysicalDeviceProvokingVertexFeaturesEXT *f = (const VkPhysicalDeviceProvokingVertexFeaturesEXT *)p;
        feature_enabled += !!f->provokingVertexLast;
        REQUIRE(!f->transformFeedbackPreservesProvokingVertex);
    }
    return __real_vkCreateDevice(d, ci, allocator, out);
}

extern __typeof__(vkCreateGraphicsPipelines) __real_vkCreateGraphicsPipelines;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache,
    uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    for (uint32_t i = 0; i < n; i++) {
        if (!ci[i].pRasterizationState) continue;
        for (const VkBaseInStructure *p = ci[i].pRasterizationState->pNext; p; p = p->pNext) {
            if (p->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT) continue;
            const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *pv = (const void *)p;
            raster_last += pv->provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
            REQUIRE(control == NORMAL);  // 缺扩展、缺位或驱动限制时不挂扩展结构。
        }
    }
    return __real_vkCreateGraphicsPipelines(d, cache, n, ci, alloc, out);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    control = atoi(argv[2]);
    struct s2a_vk_device dev;
    char why[256];
    if (s2a_vk_device_create(&dev, why, sizeof why)) {
        fprintf(stderr, "device: %s\n", why);
        return 2;
    }
    REQUIRE(nvn_gpu_ok(nvn_gpu_open(&(nvn_gpu_open_desc){.host_device = &dev, .app_label = "replay-provoking"}, &err)));
    memset(&prog, 0, sizeof prog);
    prog.sh[0] = shader(argv[1], "provoking.flat.vert", NVN_GPU_SH_VS, NULL);
    prog.sh[1] = shader(argv[1], "provoking.flat.frag", NVN_GPU_SH_FS, NULL);
    prog.color_out_mask = 15;

    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                                .width = 64, .height = 64, .depth = 1, .levels = 1},
                           .usage = NVN_GPU_TEX_RENDER};
    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &target_view, &err));
    nvn_gpu_buf rb = buffer(64 * 64 * 4, NULL);
    target_tex.d = td.d;
    target_tex.serial = 1;
    target.n = 1;
    target.color[0] = &target_tex;
    rp_draw_reset();
    rp_state_reset();
    nvn_unimpl_reset();
    rp_S.color.logic_op = NVN_LOGIC_OP_COPY;
    rp_S.chmask.mask = 15;

    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    pass(active, 1);
    const p_draw d = {.prim = 4, .count = 3, .instances = 1};
    for (unsigned col = 0; col < 4; col++) {
        rp_R.has_sc = 1;
        rp_R.sc = (p_scissor){(int)col * 16, 0, 16, 64};
        if (col) {
            const uint32_t raw_mode = col == 2 ? 1u : 0u;
            // 宿主专用新操作码接在 OP_MULTI_DRAW 后。基线能跑同一条带记录的像素测试。
            (void)rp_state_op(OP_MULTI_DRAW + 1, &raw_mode, 16);
        }
        rp_draw(active, &d, NULL, &d);
    }
    rp_pass_flush(active);
    nvn_gpu_copy_tex_buf(active, tex, 0, 0, NULL, rb, 0, 256, 16384);
    GPU(nvn_gpu_cmd_commit(active, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    const uint8_t *pixels = nvn_gpu_buf_cpu(rb);
    REQUIRE(pixels);
    const uint8_t red[4] = {255,0,0,255}, blue[4] = {0,0,255,255}, black[4] = {0,0,0,255};
    const uint8_t *want[4] = {control == NORMAL ? blue : red, red, control == NORMAL ? blue : black, red};
    unsigned bad = 0;
    for (unsigned col = 0; col < 4; col++) {
        unsigned wrong = 0;
        for (unsigned y = 0; y < 64; y++) for (unsigned x = col * 16; x < (col + 1) * 16; x++)
            wrong += memcmp(pixels + 4 * (y * 64 + x), want[col], 4) != 0;
        printf("provoking control=%u col=%u pixels=%u/1024\n", control, col, 1024 - wrong);
        bad += wrong;
    }
    uint64_t draws, skips;
    rp_draw_stats(&draws, &skips);
    const int enabled = control == NORMAL;
    const uint64_t dq = nvn_vk_dbg_dq_recs();
    const uint64_t unsupported = nvn_unimpl_count("draw", "provoking_vertex_last_unsupported");
    printf("provoking ext=%u queried=%u last_feature=%u last_pipelines=%u draws=%llu skips=%llu unsupported=%llu dq=%llu bad=%u\n",
           ext_enabled, feature_queried, feature_enabled, raster_last,
           (unsigned long long)draws, (unsigned long long)skips, (unsigned long long)unsupported,
           (unsigned long long)dq, bad);
    const int ok = !bad && ext_enabled == (unsigned)enabled && feature_enabled == (unsigned)enabled &&
        !!feature_queried == (control != NO_EXTENSION) && !!raster_last == enabled && draws == (enabled ? 4u : 3u) &&
        skips == (enabled ? 0u : 1u) && unsupported == skips && !!dq == !!getenv("S2A_REPLAY_INDIRECT_DQ");
    rp_draw_reset();
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(target_view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(prog.sh[0]);
    nvn_gpu_shader_release(prog.sh[1]);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&dev, 1));
    return ok ? 0 : 1;
}
