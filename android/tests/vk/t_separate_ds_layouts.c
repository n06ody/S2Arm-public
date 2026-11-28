// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_fixture_main
#define nvn_plat_flag feedback_fixture_flag
#include "t_feedback.c"
#undef main
#undef nvn_plat_flag

enum { OFF, NATIVE, NO_EXTENSION, NO_FEATURE, COMBINED, NO_NONE };
enum { PIXELS_PER_PASS = 64, PASS_BYTES = PIXELS_PER_PASS * 4, PIXEL_PASSES = 7 };
static int control;
static unsigned queried, extension_enabled, feature_enabled;
static unsigned mixed_depth_read, mixed_stencil_read, depth_barriers, stencil_barriers, bad_read_dst;
static unsigned bad_store, writable_changes, none_enabled, readonly_exits, bad_read_src;
static unsigned imageless_passes;
static int sync2_mode;
static unsigned sync2_images, sync2_batches;
static VkDevice observed_device;
static PFN_vkCmdPipelineBarrier2KHR real_barrier2;
static unsigned pass_count;
static struct { VkRenderPass pass; VkImageLayout ds; } pass_layouts[24];

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_sep_ds_layout")) return control != OFF;
    if (!strcmp(name, "nvk_sync2")) return sync2_mode;
    if ((control == COMBINED || control == NO_NONE) &&
        (!strcmp(name, "nvk_load_store_op_none") || !strcmp(name, "nvk_image_barrier_batch") ||
         !strcmp(name, "nvk_feedback_loop_ext") || !strcmp(name, "nvk_desc_keep_rect_clear") ||
         !strcmp(name, "nvk_imageless_fb"))) return 1;
    return feedback_fixture_flag(name, dflt);
}

extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,
        const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (layer || (control != NO_EXTENSION && control != NO_NONE))
        return __real_vkEnumerateDeviceExtensionProperties(p, layer, count, props);
    uint32_t total = 0;
    VkResult rc = __real_vkEnumerateDeviceExtensionProperties(p, layer, &total, NULL);
    if (rc != VK_SUCCESS || !total) { *count = 0; return rc; }
    VkExtensionProperties *all = calloc(total, sizeof(*all));
    REQUIRE(all);
    rc = __real_vkEnumerateDeviceExtensionProperties(p, layer, &total, all);
    if (rc == VK_SUCCESS) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < total; i++) {
            const char *name = all[i].extensionName;
            if (control == NO_EXTENSION && !strcmp(name, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME)) continue;
            if (control == NO_NONE && (!strcmp(name, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME) ||
                                       !strcmp(name, VK_QCOM_RENDER_PASS_STORE_OPS_EXTENSION_NAME) ||
                                       !strcmp(name, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))) continue;
            if (props && kept == *count) { rc = VK_INCOMPLETE; break; }
            if (props) props[kept] = all[i];
            kept++;
        }
        *count = kept;
    }
    free(all);
    return rc;
}

extern __typeof__(vkGetPhysicalDeviceFeatures2) __real_vkGetPhysicalDeviceFeatures2;
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out) {
    __real_vkGetPhysicalDeviceFeatures2(physical, out);
    for (VkBaseOutStructure *p = out->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES) continue;
        queried++;
        if (control == NO_FEATURE) ((VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures *)p)->separateDepthStencilLayouts = VK_FALSE;
    }
}

extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci,
        const VkAllocationCallbacks *a, VkDevice *d) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        none_enabled |= !strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
        extension_enabled |= !strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
    }
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES)
            feature_enabled = ((const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures *)s)->separateDepthStencilLayouts;
    VkResult rc = __real_vkCreateDevice(p, ci, a, d);
    if (rc == VK_SUCCESS) observed_device = *d;
    return rc;
}

extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *p,
        const VkAllocationCallbacks *a, VkRenderPass *out) {
    VkResult rc = __real_vkCreateRenderPass(d, p, a, out);
    if (rc == VK_SUCCESS && p->pSubpasses[0].pDepthStencilAttachment) {
        pthread_mutex_lock(&observed_mu);
        const VkAttachmentReference *ref = p->pSubpasses[0].pDepthStencilAttachment;
        REQUIRE(pass_count < sizeof pass_layouts / sizeof pass_layouts[0]);
        pass_layouts[pass_count].pass = *out;
        pass_layouts[pass_count++].ds = ref->layout;
        if (ref->layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ||
            ref->layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL) {
            const int depth_read = ref->layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
            const VkAttachmentDescription *att = &p->pAttachments[ref->attachment];
            bad_store += (depth_read ? att->loadOp : att->stencilLoadOp) != VK_ATTACHMENT_LOAD_OP_LOAD;
            bad_store += (depth_read ? att->storeOp : att->stencilStoreOp) !=
                         (control == NO_NONE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_NONE_EXT);
            bad_store += (depth_read ? att->stencilStoreOp : att->storeOp) != VK_ATTACHMENT_STORE_OP_STORE;
        }
        pthread_mutex_unlock(&observed_mu);
    }
    return rc;
}

static void record_begin(const VkRenderPassBeginInfo *begin) {
    pthread_mutex_lock(&observed_mu);
    const VkBaseInStructure *chain = begin->pNext;
    if (chain && chain->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO_KHR) imageless_passes++;
    for (unsigned i = 0; i < pass_count; i++) if (pass_layouts[i].pass == begin->renderPass) {
        mixed_depth_read += pass_layouts[i].ds == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        mixed_stencil_read += pass_layouts[i].ds == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
        break;
    }
    pthread_mutex_unlock(&observed_mu);
}

static void record_barrier(VkImageLayout old, VkImageLayout layout, VkImageAspectFlags aspect,
                           VkAccessFlags2 src, VkAccessFlags2 dst) {
    const int writes = control == NO_NONE;
    if (old == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL || old == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL) {
        readonly_exits++;
        bad_read_src += !!(src & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT_KHR) != writes;
    }
    if (layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL) {
        depth_barriers += aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
        bad_read_dst += !!(dst & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT_KHR) != writes;
    }
    if (layout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL) {
        stencil_barriers += aspect == VK_IMAGE_ASPECT_STENCIL_BIT;
        bad_read_dst += !!(dst & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT_KHR) != writes;
    }
    writable_changes += layout == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL ||
                        layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
}

extern __typeof__(vkCmdPipelineBarrier) __real_vkCmdPipelineBarrier;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src,
        VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m,
        uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *images) {
    pthread_mutex_lock(&observed_mu);
    for (uint32_t i = 0; i < ni; i++) {
        const VkImageMemoryBarrier *v = &images[i];
        record_barrier(v->oldLayout, v->newLayout, v->subresourceRange.aspectMask, v->srcAccessMask, v->dstAccessMask);
    }
    pthread_mutex_unlock(&observed_mu);
    __real_vkCmdPipelineBarrier(c, src, dst, flags, nm, m, nb, b, ni, images);
}

static VKAPI_ATTR void VKAPI_CALL record_barrier2(VkCommandBuffer c, const VkDependencyInfoKHR *info) {
    pthread_mutex_lock(&observed_mu);
    const VkImageMemoryBarrier2KHR *images = info->pImageMemoryBarriers;
    sync2_images += info->imageMemoryBarrierCount;
    int distinct = 0;
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2KHR *v = &images[i];
        distinct |= v->image != images[0].image;
        record_barrier(v->oldLayout, v->newLayout, v->subresourceRange.aspectMask, v->srcAccessMask, v->dstAccessMask);
    }
    sync2_batches += distinct;
    pthread_mutex_unlock(&observed_mu);
    real_barrier2(c, info);
}

static PFN_vkVoidFunction command_hook(const char *name) {
    if (!strcmp(name, "vkCmdPipelineBarrier")) return (PFN_vkVoidFunction)__wrap_vkCmdPipelineBarrier;
    if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) {
        real_barrier2 = (PFN_vkCmdPipelineBarrier2KHR)__real_vkGetDeviceProcAddr(observed_device, name);
        if (real_barrier2) return (PFN_vkVoidFunction)record_barrier2;
    }
    return NULL;
}

static void split_draw(nvn_gpu_cmd cmd, nvn_gpu_pipe pipe, unsigned width, float z, uint8_t stencil) {
    nvn_gpu_dyn dyn = {.nvp = 1, .has_sc = 1};
    dyn.vp[0][2] = dyn.vp[0][3] = 8;
    dyn.vp[0][4] = dyn.vp[0][5] = z;
    dyn.sc[2] = width;
    dyn.sc[3] = 8;
    dyn.st_vmask[0] = dyn.st_vmask[1] = 255;
    dyn.st_wmask[0] = dyn.st_wmask[1] = 255;
    dyn.st_ref[0] = dyn.st_ref[1] = stencil;
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_set_dyn(cmd, &dyn);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(cmd, &draw);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 5);
    control = atoi(argv[2]);
    dq_mode = atoi(argv[3]);
    opt_mode = atoi(argv[4]);
    const char *sync2 = getenv("S2A_SEPARATE_DS_SYNC2");
    sync2_mode = sync2 && !strcmp(sync2, "1");
    observed_on_begin = record_begin;
    observed_command = command_hook;
    observed_expect_samples = 0;
    struct s2a_vk_device dev;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&dev, why, sizeof why));
    nvn_gpu_open_desc od = {.host_device = &dev, .app_label = "separate-ds"};
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_shader vs = shader(argv[1], "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(argv[1], "solid_blue.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe pipes[4] = {0};
    nvn_gpu_pipe_desc pd = {.prim = 4, .samples = 1, .dfmt = NVN_FORMAT_DEPTH24_STENCIL8,
                           .sfmt = NVN_FORMAT_DEPTH24_STENCIL8};
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = NVN_LOGIC_OP_COPY;
    pd.chmask.mask = 15;
    pd.ds.dt = pd.ds.st = 1;
    pd.ds.fail_f = pd.ds.fail_b = pd.ds.zfail_f = pd.ds.zfail_b = NVN_STENCIL_OP_KEEP;
    for (unsigned i = 0; i < 4; i++) {
        pd.ds.df = i == 2 ? NVN_DEPTH_FUNC_GREATER : NVN_DEPTH_FUNC_LESS;
        pd.ds.dw = i == 1;
        pd.ds.sf_f = pd.ds.sf_b = i == 0 ? NVN_STENCIL_FUNC_ALWAYS : NVN_STENCIL_FUNC_EQUAL;
        pd.ds.zpass_f = pd.ds.zpass_b = i == 0 ? NVN_STENCIL_OP_REPLACE : NVN_STENCIL_OP_KEEP;
        GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipes[i], &err));
    }

    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .width = 8, .height = 8, .depth = 1, .levels = 1},
                           .usage = NVN_GPU_TEX_RENDER};
    nvn_gpu_tex color = NULL, ds = NULL;
    nvn_gpu_view cv = NULL, dv = NULL;
    td.d.format = NVN_FORMAT_RGBA8;
    GPU(nvn_gpu_tex_create(&td, &color, &err));
    td.d.format = NVN_FORMAT_DEPTH24_STENCIL8;
    GPU(nvn_gpu_tex_create(&td, &ds, &err));
    GPU(nvn_gpu_view_create(color, NULL, &cv, &err));
    GPU(nvn_gpu_view_create(ds, NULL, &dv, &err));
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(PIXEL_PASSES * PASS_BYTES, NVN_GPU_MEM_SHARED, NULL, &rb, &err));
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));

    nvn_gpu_pass_desc p = {.w = 8, .h = 8, .layers = 1, .ncolor = 1};
    p.color[0].view = cv;
    p.color[0].load = NVN_GPU_LOAD_CLEAR;
    p.color[0].clear.f[3] = 1;
    p.depth.view = p.stencil.view = dv;
    p.depth.load = p.stencil.load = NVN_GPU_LOAD_CLEAR;
    p.clear_depth = 0.75f;
    p.clear_stencil = 3;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_pass_end(cmd);
    p.depth.load = p.stencil.load = NVN_GPU_LOAD_LOAD;
    for (unsigned i = 0; i < PIXEL_PASSES; i++) {
        p.ds_read_only = (i == 0 || i == 4) ? NVN_GPU_DS_DEPTH :
                         (i == 1 || i == 5) ? NVN_GPU_DS_STENCIL : 0;
        nvn_gpu_pass_begin(cmd, &p);
        if (i < 4) {
            split_draw(cmd, pipes[i], i == 0 ? 4 : 8, i < 2 ? 0.5f : 0.625f, i == 3 ? 3 : 5);
        } else if (i == 4) {
            // 第一笔需要写原先提示只读的深度：左半从 .5 改到 .375。
            split_draw(cmd, pipes[1], 8, 0.375f, 5);
        } else if (i == 5) {
            // 清除需要写原先提示只读的模板：右半从 3 改到 7，随后只有右半画蓝。
            const nvn_gpu_clear clear = {.color = -1, .stencil = 1, .stencil_mask = 255,
                                         .stencil_value = 7, .x = 4, .w = 4, .h = 8};
            nvn_gpu_clear_rect(cmd, &clear);
            split_draw(cmd, pipes[3], 8, 0.625f, 7);
        } else {
            // .4375 只能通过被更新到 .375 的深度，同时核对左半模板仍是 5。
            split_draw(cmd, pipes[2], 8, 0.4375f, 5);
        }
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, color, 0, 0, NULL, rb, i * PASS_BYTES, 32, PASS_BYTES);
    }
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    const uint8_t *pixels = nvn_gpu_buf_cpu(rb);
    REQUIRE(pixels);
    unsigned bad = 0;
    for (unsigned arm = 0; arm < PIXEL_PASSES; arm++) for (unsigned j = 0; j < PIXELS_PER_PASS; j++) {
        const uint8_t *pixel = pixels + (PIXELS_PER_PASS * arm + j) * 4;
        const int blue = (j % 8 < 4) != (arm == 3 || arm == 5);
        bad += pixel[0] || pixel[1] || pixel[2] != (blue ? 255 : 0) || pixel[3] != 255;
    }

    const int native = control == NATIVE || control == COMBINED || control == NO_NONE;
    const int available = native || control == OFF;
    const unsigned mixed = native ? 2u : 0u;
    const int ok = !bad && observed_draws == PIXEL_PASSES + 1 && extension_enabled == (unsigned)available &&
        feature_enabled == (unsigned)available && !!queried == (control != NO_EXTENSION) &&
        mixed_depth_read == mixed && mixed_stencil_read == mixed && depth_barriers == mixed && stencil_barriers == mixed &&
        none_enabled == (unsigned)(control != NO_NONE) && readonly_exits == 2 * mixed &&
        !bad_read_src && !bad_read_dst && !bad_store && writable_changes == 2 * mixed &&
        ((imageless_passes > 0) == (control == COMBINED || control == NO_NONE)) &&
        (sync2_mode ? sync2_images && sync2_batches : !sync2_images && !sync2_batches);
    const unsigned checked = PIXEL_PASSES * PIXELS_PER_PASS;
    printf("separate-ds control=%d DQ=%d OPT=%d pixels=%u/%u draws=%u depth-ro=%u stencil-ro=%u barriers=%u/%u writable=%u readonly-writes=%u store-errors=%u ext=%u feature=%u queried=%u imageless-passes=%u %s\n",
           control, dq_mode, opt_mode, checked - bad, checked, observed_draws, mixed_depth_read, mixed_stencil_read, depth_barriers,
           stencil_barriers, writable_changes, bad_read_dst, bad_store, extension_enabled, feature_enabled, queried,
           imageless_passes, ok ? "PASS" : "FAIL");
    printf("separate-ds none-enabled=%u readonly-exits=%u bad-src=%u bad-dst=%u\n", none_enabled, readonly_exits, bad_read_src, bad_read_dst);
    printf("separate-ds Sync2=%d image-barriers=%u independent-batches=%u\n", sync2_mode, sync2_images, sync2_batches);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(cv);
    nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(color);
    nvn_gpu_tex_release(ds);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&dev, 1));
    return ok ? 0 : 1;
}
