// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// #14: In-pass self-sampling feedback loop via VK_EXT_attachment_feedback_loop_layout.
// Real Vulkan (lavapipe) + Khronos VVL/SyncVal pixel readback regression and
// --wrap extension-hiding fallback control.
#define main snapshot_fixture_main
#define nvn_plat_flag snapshot_plat_flag
#include "t_feedback.c"
#undef main
#undef nvn_plat_flag

static int loop_mode, hide_extension, enabled_loop_ext, enabled_loop_feat;
static unsigned loop_cases, loop_img_usage_cnt, loop_pipe_color_cnt, loop_pipe_ds_cnt;
static unsigned loop_rp_layout_cnt, loop_barrier_cnt;
static unsigned loop_begins_pass;

extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci,
        const VkAllocationCallbacks *a, VkDevice *d) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME))
            enabled_loop_ext = 1;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT) {
            const VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT *f = (const void *)s;
            if (f->attachmentFeedbackLoopLayout == VK_TRUE) enabled_loop_feat = 1;
        }
    }
    return __real_vkCreateDevice(p, ci, a, d);
}

extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p, const char *layer,
        uint32_t *count, VkExtensionProperties *props) {
    if (!hide_extension || layer) return __real_vkEnumerateDeviceExtensionProperties(p, layer, count, props);
    uint32_t n = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all);
    REQUIRE(all);
    r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, all);
    if (r != VK_SUCCESS) { free(all); return r; }
    uint32_t visible = 0;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(all[i].extensionName, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME))
            all[visible++] = all[i];
    if (!props) { *count = visible; free(all); return VK_SUCCESS; }
    uint32_t written = *count < visible ? *count : visible;
    memcpy(props, all, written * sizeof *all);
    *count = written;
    free(all);
    return written < visible ? VK_INCOMPLETE : VK_SUCCESS;
}

extern __typeof__(vkCreateImage) __real_vkCreateImage;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImage(VkDevice d, const VkImageCreateInfo *ci,
        const VkAllocationCallbacks *a, VkImage *img) {
    if (ci->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
        __atomic_fetch_add(&loop_img_usage_cnt, 1u, __ATOMIC_RELAXED);
    return __real_vkCreateImage(d, ci, a, img);
}

extern __typeof__(vkCreateGraphicsPipelines) __real_vkCreateGraphicsPipelines;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *p, const VkAllocationCallbacks *a, VkPipeline *out) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i].flags & VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
            __atomic_fetch_add(&loop_pipe_color_cnt, 1u, __ATOMIC_RELAXED);
        if (p[i].flags & VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
            __atomic_fetch_add(&loop_pipe_ds_cnt, 1u, __ATOMIC_RELAXED);
    }
    return __real_vkCreateGraphicsPipelines(d, cache, n, p, a, out);
}

extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *p,
        const VkAllocationCallbacks *a, VkRenderPass *out) {
    for (uint32_t i = 0; i < p->attachmentCount; i++) {
        if (p->pAttachments[i].initialLayout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT ||
            p->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT) {
            __atomic_fetch_add(&loop_rp_layout_cnt, 1u, __ATOMIC_RELAXED);
            break;
        }
    }
    return __real_vkCreateRenderPass(d, p, a, out);
}

extern __typeof__(vkCmdPipelineBarrier) __real_vkCmdPipelineBarrier;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdPipelineBarrier(VkCommandBuffer cb,
        VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
        uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
        uint32_t ni, const VkImageMemoryBarrier *im) {
    if ((dep & VK_DEPENDENCY_BY_REGION_BIT) && nm > 0)
        __atomic_fetch_add(&loop_barrier_cnt, 1u, __ATOMIC_RELAXED);
    __real_vkCmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im);
}

_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDevice), PFN_vkCreateDevice), "vkCreateDevice sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkEnumerateDeviceExtensionProperties), PFN_vkEnumerateDeviceExtensionProperties), "vkEnumerateDeviceExtensionProperties sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateImage), PFN_vkCreateImage), "vkCreateImage sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateGraphicsPipelines), PFN_vkCreateGraphicsPipelines), "vkCreateGraphicsPipelines sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass), "vkCreateRenderPass sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdPipelineBarrier), PFN_vkCmdPipelineBarrier), "vkCmdPipelineBarrier sig");

static PFN_vkVoidFunction loop_command(const char *name) {
    if (!strcmp(name, "vkCmdPipelineBarrier")) return (PFN_vkVoidFunction)__wrap_vkCmdPipelineBarrier;
    return NULL;
}

static unsigned loop_sample_sets(void) { return 0; }

static void loop_begin_cb(const VkRenderPassBeginInfo *p) {
    (void)p;
    loop_begins_pass++;
}

static int batch_mode, ls_none_mode;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_image_barrier_batch")) return batch_mode;
    if (!strcmp(name, "nvk_load_store_op_none")) return ls_none_mode;
    if (!strcmp(name, "nvk_feedback_loop_ext")) return loop_mode;
    return snapshot_plat_flag(name, dflt);
}

static void reset_loop_obs(void) {
    observer_reset();
    loop_img_usage_cnt = loop_pipe_color_cnt = loop_pipe_ds_cnt = 0;
    loop_rp_layout_cnt = loop_barrier_cnt = loop_begins_pass = 0;
}

static void check_solid_rgba(const unsigned char *bytes, unsigned r, unsigned g, unsigned b, unsigned a, unsigned *bad) {
    for (unsigned i = 0; i < 16; i++)
        if (bytes[i * 4] != r || bytes[i * 4 + 1] != g || bytes[i * 4 + 2] != b || bytes[i * 4 + 3] != a) (*bad)++;
    for (unsigned i = 64; i < 80; i++)
        if (bytes[i] != 0xa5) (*bad)++;
}

// Case 0: Color RGBA8 steady-state across 2 passes (Pass 1: 2 self-draws; Pass 2: 1 self-draw).
// With extension enabled: 0 copies, Pass 2 has 0 splits (total begins = 4: clear + pass1_init + pass1_reopen + pass2_zero_split).
static void case_color_steady(struct s2a_vk_device *dev, const char *dir) {
    reset_loop_obs();
    nvn_gpu_open_desc od = {0}; od.host_device = dev; od.app_label = "fbloop-color-steady";
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1;
    for (unsigned i = 0; i < 2; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout layout = {.ent = ent, .nent = 2};
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "feedback.frag", NVN_GPU_SH_FS, &layout);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL; GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_tex tex = NULL; nvn_gpu_view view = NULL, sample = NULL;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &sample, &err));
    samp_par sp = {0}; nvn_gpu_samp sampler = NULL; GPU(nvn_gpu_samp_create(&sp, &sampler, &err));
    unsigned char init[80]; memset(init, 0xa5, sizeof init);
    nvn_gpu_buf rb = NULL; GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &rb, &err));
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4); nvn_gpu_pass_end(cmd); // red = 0.25 (64)
    nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1; dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    nvn_gpu_binding b[2] = {{0}}; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = sample; b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = sampler;
    nvn_gpu_draw_args a = {0}; a.prim = 4; a.count = 3; a.instances = 1;
    // Pass 1: 2 self-sampling draws (64 -> 128 -> 192)
    begin_pass(cmd, view, NVN_GPU_LOAD_LOAD, 4);
    nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a); nvn_gpu_draw(cmd, &a);
    nvn_gpu_pass_end(cmd);
    // Pass 2 (steady-state next frame): 1 self-sampling draw (192 -> 255), must have 0 splits when loop_ext active!
    begin_pass(cmd, view, NVN_GPU_LOAD_LOAD, 4);
    nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_samp_release(sampler); nvn_gpu_view_release(sample); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    unsigned bad_px = 0; check_solid_rgba(nvn_gpu_buf_cpu(rb), 255, 0, 0, 255, &bad_px);
    const int active = loop_mode && !hide_extension;
    const unsigned want_copies = active ? 0u : 3u;
    const unsigned want_begins = active ? 4u : 6u; // clear(1) + pass1(1+1 split on first seen / 2 splits snap) + pass2(1+0 split / 1+1 snap)
    unsigned bad = bad_px || observed_copies != want_copies || loop_begins_pass != want_begins ||
        (active && (!loop_img_usage_cnt || !loop_pipe_color_cnt || !loop_rp_layout_cnt || !loop_barrier_cnt)) ||
        (!active && (loop_img_usage_cnt || loop_pipe_color_cnt || loop_rp_layout_cnt || loop_barrier_cnt));
    printf("fbloop case=color-steady active=%d copies=%u/%u begins=%u/%u img_u=%u pipe_c=%u rp_l=%u bar=%u px_bad=%u bad=%u\n",
           active, observed_copies, want_copies, loop_begins_pass, want_begins,
           loop_img_usage_cnt, loop_pipe_color_cnt, loop_rp_layout_cnt, loop_barrier_cnt, bad_px, bad);
    failures += !!bad; loop_cases++;
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err)); REQUIRE(!observed_early_destroy);
}

// Case 1: Single pass LOAD_CLEAR(0.25) -> 2 self-sampling draws -> 192.
static void case_color_clear_self(struct s2a_vk_device *dev, const char *dir) {
    reset_loop_obs();
    nvn_gpu_open_desc od = {0}; od.host_device = dev; od.app_label = "fbloop-clear-self";
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1;
    for (unsigned i = 0; i < 2; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout layout = {.ent = ent, .nent = 2};
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "feedback.frag", NVN_GPU_SH_FS, &layout);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL; GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_tex tex = NULL; nvn_gpu_view view = NULL, sample = NULL;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &sample, &err));
    samp_par sp = {0}; nvn_gpu_samp sampler = NULL; GPU(nvn_gpu_samp_create(&sp, &sampler, &err));
    unsigned char init[80]; memset(init, 0xa5, sizeof init);
    nvn_gpu_buf rb = NULL; GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &rb, &err));
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4);
    nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1; dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    nvn_gpu_binding b[2] = {{0}}; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = sample; b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = sampler;
    nvn_gpu_draw_args a = {0}; a.prim = 4; a.count = 3; a.instances = 1;
    nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a); nvn_gpu_draw(cmd, &a);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_samp_release(sampler); nvn_gpu_view_release(sample); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    unsigned bad_px = 0; check_solid_rgba(nvn_gpu_buf_cpu(rb), 192, 0, 0, 255, &bad_px);
    const int active = loop_mode && !hide_extension;
    const unsigned want_copies = active ? 0u : 2u;
    unsigned bad = bad_px || observed_copies != want_copies;
    printf("fbloop case=color-clear-self active=%d copies=%u/%u begins=%u bar=%u px_bad=%u bad=%u\n",
           active, observed_copies, want_copies, loop_begins_pass, loop_barrier_cnt, bad_px, bad);
    failures += !!bad; loop_cases++;
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err)); REQUIRE(!observed_early_destroy);
}

// Case 2: Self-sample -> plain overwrite write (WAR) -> self-sample (RAW) + clear_rect (WAR) -> self-sample.
static void case_color_war_clear_rect(struct s2a_vk_device *dev, const char *dir) {
    reset_loop_obs();
    nvn_gpu_open_desc od = {0}; od.host_device = dev; od.app_label = "fbloop-war-clear";
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1;
    for (unsigned i = 0; i < 2; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout layout = {.ent = ent, .nent = 2};
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs_self = shader(dir, "feedback.frag", NVN_GPU_SH_FS, &layout);
    nvn_gpu_shader fs_write = shader(dir, "write_color.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs_self; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe_self = NULL; GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe_self, &err));
    pd.st[NVN_GPU_SH_FS].sh = fs_write;
    nvn_gpu_pipe pipe_write = NULL; GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe_write, &err));
    nvn_gpu_tex tex = NULL; nvn_gpu_view view = NULL, sample = NULL;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &sample, &err));
    samp_par sp = {0}; nvn_gpu_samp sampler = NULL; GPU(nvn_gpu_samp_create(&sp, &sampler, &err));
    unsigned char init[80]; memset(init, 0xa5, sizeof init);
    nvn_gpu_buf rb = NULL; GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &rb, &err));
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4); // 0.25
    nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1; dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    nvn_gpu_binding b[2] = {{0}}; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = sample; b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = sampler;
    nvn_gpu_draw_args a = {0}; a.prim = 4; a.count = 3; a.instances = 1;
    // Draw 1: self-sample 0.25 -> 0.50
    nvn_gpu_set_pipe(cmd, pipe_self); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a);
    // Draw 2: plain overwrite (write_color.frag writes 0.50 to loc 0) -> tests WAR barrier
    nvn_gpu_set_pipe(cmd, pipe_write); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, NULL, 0);
    nvn_gpu_draw(cmd, &a);
    // Draw 3: self-sample 0.50 -> 0.75
    nvn_gpu_set_pipe(cmd, pipe_self); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a);
    // Clear rect to 0.25 -> tests clear_rect WAR barrier
    nvn_gpu_clear cl = {0}; cl.color = 0; cl.color_mask = 15; cl.value.f[0] = 0.25f; cl.value.f[3] = 1.0f; cl.w = cl.h = 4;
    nvn_gpu_clear_rect(cmd, &cl);
    // Draw 4: self-sample 0.25 -> 0.50 (128)
    nvn_gpu_set_pipe(cmd, pipe_self); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_samp_release(sampler); nvn_gpu_view_release(sample); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    unsigned bad_px = 0; check_solid_rgba(nvn_gpu_buf_cpu(rb), 128, 0, 0, 255, &bad_px);
    const int active = loop_mode && !hide_extension;
    const unsigned want_copies = active ? 0u : 3u;
    unsigned bad = bad_px || observed_copies != want_copies;
    printf("fbloop case=color-war-clear active=%d copies=%u/%u begins=%u bar=%u px_bad=%u bad=%u\n",
           active, observed_copies, want_copies, loop_begins_pass, loop_barrier_cnt, bad_px, bad);
    failures += !!bad; loop_cases++;
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs_self); nvn_gpu_shader_release(fs_write);
    GPU(nvn_gpu_close(5000000000ull, &err)); REQUIRE(!observed_early_destroy);
}

// Case 3/4: Swizzle view & BGRA8 format-reinterpreted view self-sampling.
static void case_color_view_variants(struct s2a_vk_device *dev, const char *dir, int fmt_cast) {
    reset_loop_obs();
    nvn_gpu_open_desc od = {0}; od.host_device = dev; od.app_label = fmt_cast ? "fbloop-fmtcast" : "fbloop-swizzle";
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1;
    for (unsigned i = 0; i < 2; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout layout = {.ent = ent, .nent = 2};
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "feedback.frag", NVN_GPU_SH_FS, &layout);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL; GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_tex tex = NULL; nvn_gpu_view view = NULL, sample = NULL;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | (fmt_cast ? NVN_GPU_TEX_VIEW_FMT : 0);
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    view_t sv = {0};
    if (fmt_cast) { sv.set_format = 1; sv.format = NVN_FORMAT_BGRA8; }
    else { sv.set_swizzle = 1; sv.swz[0] = 3; sv.swz[1] = 2; sv.swz[2] = 4; sv.swz[3] = 5; } // R<-G(0), G<-R(0.25) -> output (0.25, 0.25, 0, 1)
    GPU(nvn_gpu_view_create(tex, &sv, &sample, &err));
    samp_par sp = {0}; nvn_gpu_samp sampler = NULL; GPU(nvn_gpu_samp_create(&sp, &sampler, &err));
    unsigned char init[80]; memset(init, 0xa5, sizeof init);
    nvn_gpu_buf rb = NULL; GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &rb, &err));
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4);
    nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1; dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    nvn_gpu_binding b[2] = {{0}}; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = sample; b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = sampler;
    nvn_gpu_draw_args a = {0}; a.prim = 4; a.count = 3; a.instances = 1;
    nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_samp_release(sampler); nvn_gpu_view_release(sample); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    unsigned bad_px = 0; check_solid_rgba(nvn_gpu_buf_cpu(rb), 64, fmt_cast ? 0 : 64, fmt_cast ? 64 : 0, 255, &bad_px);
    const int active = loop_mode && !hide_extension;
    const unsigned want_copies = active ? 0u : 1u;
    unsigned bad = bad_px || observed_copies != want_copies;
    printf("fbloop case=%s active=%d copies=%u/%u begins=%u px_bad=%u bad=%u\n",
           fmt_cast ? "color-fmtcast" : "color-swizzle", active, observed_copies, want_copies, loop_begins_pass, bad_px, bad);
    failures += !!bad; loop_cases++;
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err)); REQUIRE(!observed_early_destroy);
}

// Case 5/6: Depth D32F and D32F_S8 self-sampling (2 draws in same pass: 0.25 -> 0.50 -> 0.75 => 191).
static void case_depth_self(struct s2a_vk_device *dev, const char *dir, unsigned fmt, const char *label) {
    reset_loop_obs();
    nvn_gpu_open_desc od = {0}; od.host_device = dev; od.app_label = label;
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1;
    for (unsigned i = 0; i < 2; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout layout = {.ent = ent, .nent = 2};
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "feedback_depth.frag", NVN_GPU_SH_FS, &layout);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.dfmt = fmt;
    pd.ds.dt = pd.ds.dw = 1; pd.ds.df = 8;
    nvn_gpu_pipe pipe = NULL; GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_tex images[2] = {0}; nvn_gpu_view views[2] = {0}, sample = NULL;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    for (unsigned i = 0; i < 2; i++) {
        td.d.format = i ? fmt : NVN_FORMAT_RGBA8;
        GPU(nvn_gpu_tex_create(&td, &images[i], &err));
        GPU(nvn_gpu_view_create(images[i], NULL, &views[i], &err));
    }
    GPU(nvn_gpu_view_create(images[1], NULL, &sample, &err));
    samp_par sp = {0}; nvn_gpu_samp sampler = NULL; GPU(nvn_gpu_samp_create(&sp, &sampler, &err));
    unsigned char init[80]; memset(init, 0xa5, sizeof init);
    nvn_gpu_buf rb = NULL; GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &rb, &err));
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {0}; p.ncolor = 1; p.color[0].view = views[0]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.depth.view = views[1]; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE; p.clear_depth = 0.25f; p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_binding b[2] = {{0}}; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = sample; b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = sampler;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw_args a = {0}; a.prim = 4; a.count = 3; a.instances = 1;
    nvn_gpu_draw(cmd, &a); nvn_gpu_draw(cmd, &a);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, images[0], 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_samp_release(sampler); nvn_gpu_view_release(sample);
    for (unsigned i = 0; i < 2; i++) { nvn_gpu_view_release(views[i]); nvn_gpu_tex_release(images[i]); }
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    unsigned bad_px = 0; check_solid_rgba(nvn_gpu_buf_cpu(rb), 191, 0, 0, 255, &bad_px);
    const int active = loop_mode && !hide_extension;
    const unsigned want_copies = active ? 0u : 2u;
    unsigned bad = bad_px || observed_copies != want_copies ||
        (active && !loop_pipe_ds_cnt) || (!active && loop_pipe_ds_cnt);
    printf("fbloop case=%s active=%d copies=%u/%u begins=%u pipe_ds=%u bar=%u px_bad=%u bad=%u\n",
           label, active, observed_copies, want_copies, loop_begins_pass, loop_pipe_ds_cnt, loop_barrier_cnt, bad_px, bad);
    failures += !!bad; loop_cases++;
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err)); REQUIRE(!observed_early_destroy);
}

// Case 7: Pass 1 self-samples images[0] (0.25 -> 0.50), Pass 2 draws into images[1] sampling images[0] (0.50 -> 0.75).
static void case_next_pass_sample(struct s2a_vk_device *dev, const char *dir) {
    reset_loop_obs();
    nvn_gpu_open_desc od = {0}; od.host_device = dev; od.app_label = "fbloop-next-pass";
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1;
    for (unsigned i = 0; i < 2; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout layout = {.ent = ent, .nent = 2};
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "feedback.frag", NVN_GPU_SH_FS, &layout);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL; GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_tex images[2] = {0}; nvn_gpu_view views[2] = {0}, sample = NULL;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    for (unsigned i = 0; i < 2; i++) {
        GPU(nvn_gpu_tex_create(&td, &images[i], &err));
        GPU(nvn_gpu_view_create(images[i], NULL, &views[i], &err));
    }
    GPU(nvn_gpu_view_create(images[0], NULL, &sample, &err));
    samp_par sp = {0}; nvn_gpu_samp sampler = NULL; GPU(nvn_gpu_samp_create(&sp, &sampler, &err));
    unsigned char init[80]; memset(init, 0xa5, sizeof init);
    nvn_gpu_buf rb = NULL; GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &rb, &err));
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_dyn dyn = {0}; dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1; dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    nvn_gpu_binding b[2] = {{0}}; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = sample; b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = sampler;
    nvn_gpu_draw_args a = {0}; a.prim = 4; a.count = 3; a.instances = 1;
    begin_pass(cmd, views[0], NVN_GPU_LOAD_CLEAR, 4);
    nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a); // 0.25 -> 0.50
    nvn_gpu_pass_end(cmd);
    begin_pass(cmd, views[1], NVN_GPU_LOAD_CLEAR, 4);
    nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn); nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &a); // samples images[0] (0.50) -> writes images[1] (0.75 = 192)
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, images[1], 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_samp_release(sampler); nvn_gpu_view_release(sample);
    for (unsigned i = 0; i < 2; i++) { nvn_gpu_view_release(views[i]); nvn_gpu_tex_release(images[i]); }
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    unsigned bad_px = 0; check_solid_rgba(nvn_gpu_buf_cpu(rb), 192, 0, 0, 255, &bad_px);
    const int active = loop_mode && !hide_extension;
    const unsigned want_copies = active ? 0u : 1u;
    unsigned bad = bad_px || observed_copies != want_copies;
    printf("fbloop case=next-pass-sample active=%d copies=%u/%u begins=%u px_bad=%u bad=%u\n",
           active, observed_copies, want_copies, loop_begins_pass, bad_px, bad);
    failures += !!bad; loop_cases++;
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err)); REQUIRE(!observed_early_destroy);
}

int main(int argc, char **argv) {
    if (argc != 6 && argc != 7) return 2;
    const unsigned switches = argc == 7 ? (unsigned)atoi(argv[6]) : 0;
    batch_mode = (switches & 1u) != 0;
    ls_none_mode = (switches & 2u) != 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    dq_mode = atoi(argv[2]);
    opt_mode = atoi(argv[3]);
    loop_mode = atoi(argv[4]);
    hide_extension = atoi(argv[5]);
    copy_mode = 1;
    pool_fb_mode = 1;
    observed_on_begin = loop_begin_cb;
    observed_sample_sets = loop_sample_sets;
    observed_command = loop_command;
    struct s2a_vk_device device;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    // Verify real driver advertises VK_EXT_attachment_feedback_loop_layout so the test never silently skips.
    uint32_t n = 0;
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, NULL) == VK_SUCCESS);
    VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext); REQUIRE(ext);
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, ext) == VK_SUCCESS);
    int advertised = 0;
    for (uint32_t i = 0; i < n; i++)
        advertised |= !strcmp(ext[i].extensionName, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
    free(ext);
    REQUIRE(advertised);
    REQUIRE(enabled_loop_ext == !hide_extension);
    REQUIRE(enabled_loop_feat == !hide_extension);
    REQUIRE(device.attachment_feedback_loop_enabled == !hide_extension);

    case_color_steady(&device, argv[1]);
    case_color_clear_self(&device, argv[1]);
    case_color_war_clear_rect(&device, argv[1]);
    case_color_view_variants(&device, argv[1], 0);
    case_color_view_variants(&device, argv[1], 1);
    case_depth_self(&device, argv[1], NVN_FORMAT_DEPTH32F, "depth-d32f");
    case_depth_self(&device, argv[1], NVN_FORMAT_DEPTH32F_STENCIL8, "depth-d32s8");
    case_next_pass_sample(&device, argv[1]);

    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("feedback-loop-ext cases=%u failures=%u dq=%d opt=%d loop=%d hidden=%d ext-enabled=%d feat-enabled=%d batch=%d ls-none=%d\n",
           loop_cases, failures, dq_mode, opt_mode, loop_mode, hide_extension, enabled_loop_ext, enabled_loop_feat, batch_mode, ls_none_mode);
    return failures ? 1 : 0;
}
