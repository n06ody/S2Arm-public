// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for Vulkan 1.1 core Multiview features.
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_mv;
static int g_hide_mv_gs;
static int g_gs_off;
static int g_hw_mv;
static int g_hw_mv_gs;
static int g_hw_mv_ts;
static int g_dev_struct;
static int g_dev_mv;
static int g_dev_mv_gs;
static int g_dev_mv_ts;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】multiview line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

const char *s2a_capprobe_gs_occ_state(void) {
    return g_gs_off ? "multiview-test:unsupported" : "multiview-test:supported";
}
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pipe_cache")) return 0;
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level; fprintf(stderr, "[%s] %s\n", cls ? cls : "", text ? text : "");
}

extern __typeof__(vkGetPhysicalDeviceFeatures2) __real_vkGetPhysicalDeviceFeatures2;
extern __typeof__(vkCreateDevice) __real_vkCreateDevice;

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) {
    __real_vkGetPhysicalDeviceFeatures2(p, f);
    if (!f) return;
    for (VkBaseOutStructure *b = f->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
            VkPhysicalDeviceMultiviewFeatures *mv = (VkPhysicalDeviceMultiviewFeatures *)b;
            g_hw_mv    = (mv->multiview == VK_TRUE);
            g_hw_mv_gs = (mv->multiviewGeometryShader == VK_TRUE);
            g_hw_mv_ts = (mv->multiviewTessellationShader == VK_TRUE);
            // hide-mv intentionally clears multiview while leaving GS/TS untouched to prove VUID-00580/00581 guard.
            if (g_hide_mv)    mv->multiview = VK_FALSE;
            if (g_hide_mv_gs) mv->multiviewGeometryShader = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    g_dev_struct = 0; g_dev_mv = 0; g_dev_mv_gs = 0; g_dev_mv_ts = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
            const VkPhysicalDeviceMultiviewFeatures *mv = (const VkPhysicalDeviceMultiviewFeatures *)b;
            g_dev_struct = 1;
            if (mv->multiview == VK_TRUE) g_dev_mv = 1;
            if (mv->multiviewGeometryShader == VK_TRUE) g_dev_mv_gs = 1;
            if (mv->multiviewTessellationShader == VK_TRUE) g_dev_mv_ts = 1;
        }
    }
    return __real_vkCreateDevice(p, ci, a, d);
}

static uint32_t *load_spv(const char *dir, const char *name, size_t *bytes) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 20 || (n & 3)) { fclose(f); return NULL; }
    uint32_t *w = malloc((size_t)n);
    if (!w || fread(w, 1, (size_t)n, f) != (size_t)n) { free(w); fclose(f); return NULL; }
    fclose(f);
    *bytes = (size_t)n;
    return w;
}

static int check_shader_gate(const char *dir, int expect_mv) {
    size_t vbytes = 0;
    uint32_t *vwords = load_spv(dir, "mv_layer.vert.spv", &vbytes);
    if (!vwords) return 0;
    static const nvn_gpu_layout empty = {0};
    nvn_gpu_shader_src src = {
        .stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV,
        .blob = vwords, .bytes = vbytes, .entry = "main", .layout = &empty,
    };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(vwords);
    if (!expect_mv) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && strstr(g_err.why, "VK_KHR_multiview") != NULL);
        if (!ok) {
            fprintf(stderr, "【红】mv_layer.vert expected reject(VK_KHR_multiview) got rc=%d why=%s\n", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】mv_layer.vert expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_shader_release(sh);
    return 1;
}

// Render two layers in a single draw call via VkRenderPassMultiviewCreateInfo (viewMask=0x3)
// and verify layer 0 == red (255,0,0,255) and layer 1 == blue (0,0,255,255).
static int run_multiview_render(struct s2a_vk_device *dev, const char *dir) {
    enum { W = 4, H = 4, LAYERS = 2, LAYER_BYTES = W * H * 4, TOTAL_BYTES = LAYER_BYTES * LAYERS };
    size_t vbytes = 0, fbytes = 0;
    uint32_t *vwords = load_spv(dir, "mv_layer.vert.spv", &vbytes);
    uint32_t *fwords = load_spv(dir, "mv_layer.frag.spv", &fbytes);
    if (!vwords || !fwords) { free(vwords); free(fwords); return 0; }

    VkDevice d = dev->device;
    int ok = 0;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE, buf_mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkShaderModuleCreateInfo sm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vbytes, .pCode = vwords };
    if (vkCreateShaderModule(d, &sm, NULL, &vs) != VK_SUCCESS) goto cleanup;
    sm.codeSize = fbytes; sm.pCode = fwords;
    if (vkCreateShaderModule(d, &sm, NULL, &fs) != VK_SUCCESS) goto cleanup;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = LAYERS,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(d, &ici, NULL, &img) != VK_SUCCESS) goto cleanup;
    VkMemoryRequirements imr = {0};
    vkGetImageMemoryRequirements(d, img, &imr);
    uint32_t imt = s2a_vk_memory_type(dev, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (imt == UINT32_MAX) imt = s2a_vk_memory_type(dev, imr.memoryTypeBits, 0, NULL);
    if (imt == UINT32_MAX) goto cleanup;
    VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = imr.size, .memoryTypeIndex = imt };
    if (vkAllocateMemory(d, &imai, NULL, &img_mem) != VK_SUCCESS) goto cleanup;
    if (vkBindImageMemory(d, img, img_mem, 0) != VK_SUCCESS) goto cleanup;

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS },
    };
    if (vkCreateImageView(d, &ivci, NULL, &view) != VK_SUCCESS) goto cleanup;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = TOTAL_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(d, &bci, NULL, &buf) != VK_SUCCESS) goto cleanup;
    VkMemoryRequirements bmr = {0};
    vkGetBufferMemoryRequirements(d, buf, &bmr);
    uint32_t bmt = s2a_vk_memory_type(dev, bmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (bmt == UINT32_MAX) goto cleanup;
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size, .memoryTypeIndex = bmt };
    if (vkAllocateMemory(d, &bmai, NULL, &buf_mem) != VK_SUCCESS) goto cleanup;
    if (vkBindBufferMemory(d, buf, buf_mem, 0) != VK_SUCCESS) goto cleanup;

    VkAttachmentDescription att = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference cref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref };
    VkSubpassDependency dep = {
        .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    const uint32_t view_mask = 0x3u;
    const uint32_t corr_mask = 0x3u;
    VkRenderPassMultiviewCreateInfo mv_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount = 1,
        .pViewMasks = &view_mask,
        .correlationMaskCount = 1,
        .pCorrelationMasks = &corr_mask,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = &mv_rp,
        .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    if (vkCreateRenderPass(d, &rpci, NULL, &rp) != VK_SUCCESS) goto cleanup;

    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &view,
        .width = W, .height = H, .layers = 1, // Multiview VUID-VkFramebufferCreateInfo-renderPass-04536 requires layers=1
    };
    if (vkCreateFramebuffer(d, &fbci, NULL, &fb) != VK_SUCCESS) goto cleanup;

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    if (vkCreatePipelineLayout(d, &plci, NULL, &pl) != VK_SUCCESS) goto cleanup;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    VkRect2D sc = { {0, 0}, {W, H} };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xFu };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cbs,
        .layout = pl, .renderPass = rp, .subpass = 0,
    };
    if (vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS) goto cleanup;

    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = dev->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (vkAllocateCommandBuffers(d, &cbai, &cb) != VK_SUCCESS) goto cleanup;
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(cb, &cbbi) != VK_SUCCESS) goto cleanup;
    VkClearValue clear = { .color = { .float32 = {0, 1, 0, 1} } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp, .framebuffer = fb,
        .renderArea = { {0, 0}, {W, H} },
        .clearValueCount = 1, .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy copies[2] = {
        { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } },
        { .bufferOffset = LAYER_BYTES, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1 }, .imageExtent = { W, H, 1 } },
    };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 2, copies);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) goto cleanup;

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(d, &fci, NULL, &fence) != VK_SUCCESS) goto cleanup;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (vkQueueSubmit(dev->queue, 1, &si, fence) != VK_SUCCESS) goto cleanup;
    if (vkWaitForFences(d, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) goto cleanup;

    void *mapped = NULL;
    if (vkMapMemory(d, buf_mem, 0, TOTAL_BYTES, 0, &mapped) != VK_SUCCESS) goto cleanup;
    const uint8_t *px = (const uint8_t *)mapped;
    ok = 1;
    for (unsigned i = 0; i < W * H; i++) {
        const uint8_t *l0 = px + i * 4;
        const uint8_t *l1 = px + LAYER_BYTES + i * 4;
        if (l0[0] != 255 || l0[1] != 0 || l0[2] != 0 || l0[3] != 255) {
            fprintf(stderr, "【红】multiview layer0[%u]=%u,%u,%u,%u want=255,0,0,255\n", i, l0[0], l0[1], l0[2], l0[3]);
            ok = 0; break;
        }
        if (l1[0] != 0 || l1[1] != 0 || l1[2] != 255 || l1[3] != 255) {
            fprintf(stderr, "【红】multiview layer1[%u]=%u,%u,%u,%u want=0,0,255,255\n", i, l1[0], l1[1], l1[2], l1[3]);
            ok = 0; break;
        }
    }
    vkUnmapMemory(d, buf_mem);

cleanup:
    if (fence) vkDestroyFence(d, fence, NULL);
    if (cb) vkFreeCommandBuffers(d, dev->command_pool, 1, &cb);
    if (pipe) vkDestroyPipeline(d, pipe, NULL);
    if (pl) vkDestroyPipelineLayout(d, pl, NULL);
    if (fb) vkDestroyFramebuffer(d, fb, NULL);
    if (rp) vkDestroyRenderPass(d, rp, NULL);
    if (buf) vkDestroyBuffer(d, buf, NULL);
    if (buf_mem) vkFreeMemory(d, buf_mem, NULL);
    if (view) vkDestroyImageView(d, view, NULL);
    if (img) vkDestroyImage(d, img, NULL);
    if (img_mem) vkFreeMemory(d, img_mem, NULL);
    if (vs) vkDestroyShaderModule(d, vs, NULL);
    if (fs) vkDestroyShaderModule(d, fs, NULL);
    free(vwords);
    free(fwords);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-mv|hide-mv-gs|gs-off|withdraw>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *mode = argv[2];
    if (!strcmp(mode, "off") || !strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_multiview", "0", 1);
    else setenv("debug.s2a.nvk_multiview", "1", 1);
    g_hide_mv    = !strcmp(mode, "hide-mv");
    g_hide_mv_gs = !strcmp(mode, "hide-mv-gs");
    g_gs_off     = !strcmp(mode, "gs-off");

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    if (!strcmp(mode, "on") || !strcmp(mode, "withdraw")) {
        REQ(g_hw_mv);
    }
    const int active = !strcmp(mode, "on") || !strcmp(mode, "withdraw") || g_hide_mv || g_hide_mv_gs || g_gs_off;
    const int expect_mv    = (active && !g_hide_mv) ? g_hw_mv : 0;
    const int expect_mv_gs = (expect_mv && !g_hide_mv_gs && dev.core_features.geometryShader == VK_TRUE) ? g_hw_mv_gs : 0;
    const int expect_mv_ts = (expect_mv && dev.core_features.tessellationShader == VK_TRUE) ? g_hw_mv_ts : 0;
    const int expect_struct = expect_mv;

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "multiview-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));

    int gate_ok = check_shader_gate(dir, expect_mv);
    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));

    int render_ok = 1;
    if (expect_mv && g_dev_mv) {
        render_ok = run_multiview_render(&dev, dir);
    } else if (expect_mv && !g_dev_mv) {
        render_ok = 0;
    }
    printf("multiview mode=%s hw=%d/%d/%d core_gs=%d core_ts=%d dev_s=%d dev_bits=%d/%d/%d en_bits=%d/%d/%d gate=%d render=%d\n",
           mode, g_hw_mv, g_hw_mv_gs, g_hw_mv_ts,
           dev.core_features.geometryShader == VK_TRUE, dev.core_features.tessellationShader == VK_TRUE,
           g_dev_struct, g_dev_mv, g_dev_mv_gs, g_dev_mv_ts,
           dev.multiview_enabled, dev.multiview_geometry_shader_enabled, dev.multiview_tessellation_shader_enabled,
           gate_ok, render_ok);

    int rc = 0;
    if (g_dev_struct != expect_struct || g_dev_mv != expect_mv ||
        g_dev_mv_gs != expect_mv_gs || g_dev_mv_ts != expect_mv_ts ||
        dev.multiview_enabled != expect_mv ||
        dev.multiview_geometry_shader_enabled != expect_mv_gs ||
        dev.multiview_tessellation_shader_enabled != expect_mv_ts ||
        !gate_ok || !render_ok) {
        rc = 1;
    }

    REQ(!s2a_vk_device_destroy(&dev, 0));
    return rc;
}
