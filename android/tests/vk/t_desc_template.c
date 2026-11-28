// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract fixture for Vulkan 1.1 core vkUpdateDescriptorSetWithTemplate (debug.s2a.nvk_desc_template).
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err err;
static int g_dq_mode = 0;
static int g_memo_mode = 1;
static int g_tpl_flag = 1;
static int g_hide_fn = 0;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "desc-template fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

// ---- Vulkan call observers via -Wl,--wrap ----
static _Atomic uint64_t g_tpl_create = 0;
static _Atomic uint64_t g_tpl_destroy = 0;
static _Atomic uint64_t g_tpl_update = 0;
static _Atomic uint64_t g_tpl_entries = 0;
static _Atomic uint64_t g_legacy_update = 0;
static _Atomic uint64_t g_legacy_writes = 0;

static PFN_vkCreateDescriptorUpdateTemplate g_real_create = NULL;
static PFN_vkDestroyDescriptorUpdateTemplate g_real_destroy = NULL;
static PFN_vkUpdateDescriptorSetWithTemplate g_real_update = NULL;

static void obs_reset(void) {
    atomic_store(&g_tpl_create, 0);
    atomic_store(&g_tpl_destroy, 0);
    atomic_store(&g_tpl_update, 0);
    atomic_store(&g_tpl_entries, 0);
    atomic_store(&g_legacy_update, 0);
    atomic_store(&g_legacy_writes, 0);
}

static VKAPI_ATTR VkResult VKAPI_CALL obs_create(VkDevice dev, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                 const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out) {
    VkResult r = g_real_create(dev, ci, a, out);
    if (r == VK_SUCCESS) {
        atomic_fetch_add(&g_tpl_create, 1);
        if (ci) {
            uint64_t n = 0;
            for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++) n += ci->pDescriptorUpdateEntries[i].descriptorCount;
            atomic_fetch_add(&g_tpl_entries, n);
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL obs_destroy(VkDevice dev, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a) {
    if (t) atomic_fetch_add(&g_tpl_destroy, 1);
    g_real_destroy(dev, t, a);
}

static VKAPI_ATTR void VKAPI_CALL obs_update(VkDevice dev, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data) {
    atomic_fetch_add(&g_tpl_update, 1);
    g_real_update(dev, set, t, data);
}

PFN_vkVoidFunction __real_vkGetDeviceProcAddr(VkDevice, const char *);
PFN_vkVoidFunction __wrap_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    if (name && g_hide_fn &&
        (!strcmp(name, "vkCreateDescriptorUpdateTemplate") ||
         !strcmp(name, "vkDestroyDescriptorUpdateTemplate") ||
         !strcmp(name, "vkUpdateDescriptorSetWithTemplate"))) {
        return NULL;
    }
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(dev, name);
    if (!fn || !name) return fn;
    if (!strcmp(name, "vkCreateDescriptorUpdateTemplate")) {
        g_real_create = (PFN_vkCreateDescriptorUpdateTemplate)fn;
        return (PFN_vkVoidFunction)obs_create;
    }
    if (!strcmp(name, "vkDestroyDescriptorUpdateTemplate")) {
        g_real_destroy = (PFN_vkDestroyDescriptorUpdateTemplate)fn;
        return (PFN_vkVoidFunction)obs_destroy;
    }
    if (!strcmp(name, "vkUpdateDescriptorSetWithTemplate")) {
        g_real_update = (PFN_vkUpdateDescriptorSetWithTemplate)fn;
        return (PFN_vkVoidFunction)obs_update;
    }
    return fn;
}

void __real_vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t, const VkCopyDescriptorSet *);
void __wrap_vkUpdateDescriptorSets(VkDevice dev, uint32_t nw, const VkWriteDescriptorSet *w,
                                   uint32_t nc, const VkCopyDescriptorSet *c) {
    atomic_fetch_add(&g_legacy_update, 1);
    atomic_fetch_add(&g_legacy_writes, nw);
    __real_vkUpdateDescriptorSets(dev, nw, w, nc, c);
}

// ---- Platform stubs ----
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_mode;
    if (!strcmp(n, "nvk_desc_memo")) return g_memo_mode;
    if (!strcmp(n, "nvk_desc_template")) return g_tpl_flag;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done(void *u, const nvn_gpu_done *d) { REQUIRE(d->status == NVN_GPU_DONE_OK); }

static void submit_and_wait(nvn_gpu_cmd cmd) {
    uint64_t s = ++g_sig;
    GPU(nvn_gpu_cmd_commit(cmd, s, on_done, NULL, &err));
    GPU(nvn_gpu_fence_wait(s, 10000000000ull, &err));
}

static nvn_gpu_shader load_shader(const char *dir, const char *name, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb");
    REQUIRE(f);
    REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    REQUIRE(n > 0);
    rewind(f);
    void *data = malloc((size_t)n);
    REQUIRE(data);
    REQUIRE(fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    nvn_gpu_shader_src src = {0};
    src.stage = stage;
    src.kind = NVN_GPU_BLOB_SPIRV;
    src.entry = "main";
    src.blob = data;
    src.bytes = (size_t)n;
    src.layout = layout;
    nvn_gpu_shader sh = NULL;
    GPU(nvn_gpu_shader_load(&src, &sh, &err));
    free(data);
    return sh;
}

static int near_u8(uint8_t a, uint8_t b, int tol) {
    int d = (int)a - (int)b;
    return d >= -tol && d <= tol;
}

static void check_rgba(const char *tag, const uint8_t *px, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tol) {
    if (!near_u8(px[0], r, tol) || !near_u8(px[1], g, tol) || !near_u8(px[2], b, tol) || !near_u8(px[3], a, tol)) {
        fprintf(stderr, "FAIL %s: got=(%u,%u,%u,%u) want=(%u,%u,%u,%u)+-%d\n",
                tag, px[0], px[1], px[2], px[3], r, g, b, a, tol);
        g_failures++;
    }
}

static void set_full_dyn(nvn_gpu_cmd cmd, uint32_t w, uint32_t h) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1;
    dyn.vp[0][2] = (float)w;
    dyn.vp[0][3] = (float)h;
    dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1;
    dyn.sc[2] = w;
    dyn.sc[3] = h;
    nvn_gpu_set_dyn(cmd, &dyn);
}

// Case 1: 2-set UBO (static when memo=0, dynamic UBO when memo=1), 2 draws with distinct buffers/offsets
static void case_ubo_multi_set(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].set = 1; ent[1].binding = 3; ent[1].cls = NVN_GPU_RES_UBO; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "ubo_dyn.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rt0 = NULL, rt1 = NULL;
    nvn_gpu_view rtv0 = NULL, rtv1 = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt0, &err));
    GPU(nvn_gpu_tex_create(&td, &rt1, &err));
    GPU(nvn_gpu_view_create(rt0, NULL, &rtv0, &err));
    GPU(nvn_gpu_view_create(rt1, NULL, &rtv1, &err));

    uint8_t udata[512] = {0};
    const float u0[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float u1[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    memcpy(udata, u0, sizeof u0);
    memcpy(udata + 256, u1, sizeof u1);
    uint8_t udata2[512] = {0};
    const float u2[4] = {0.5f, 0.0f, 0.0f, 1.0f};
    const float u3[4] = {0.0f, 0.25f, 0.0f, 1.0f};
    memcpy(udata2, u2, sizeof u2);
    memcpy(udata2 + 256, u3, sizeof u3);

    uint8_t zero[64] = {0};
    nvn_gpu_buf ub0 = NULL, ub1 = NULL, rb0 = NULL, rb1 = NULL;
    GPU(nvn_gpu_buf_create(sizeof udata, NVN_GPU_MEM_SHARED, udata, &ub0, &err));
    GPU(nvn_gpu_buf_create(sizeof udata2, NVN_GPU_MEM_SHARED, udata2, &ub1, &err));
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb0, &err));
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb1, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;

    // Draw 1: ub0 -> (255, 255, 0, 255)
    p.color[0].view = rtv0;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub0; b[0].u.b.off = 0; b[0].u.b.len = 16;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub0; b[1].u.b.off = 256; b[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);

    // Draw 2: ub1 -> (128, 64, 0, 255)
    p.color[0].view = rtv1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    set_full_dyn(cmd, 4, 4);
    b[0].u.b.buf = ub1; b[0].u.b.off = 0;
    b[1].u.b.buf = ub1; b[1].u.b.off = 256;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);

    nvn_gpu_copy_tex_buf(cmd, rt0, 0, 0, NULL, rb0, 0, 16, 64);
    nvn_gpu_copy_tex_buf(cmd, rt1, 0, 0, NULL, rb1, 0, 16, 64);
    submit_and_wait(cmd);

    check_rgba("case1-ubo-draw0", nvn_gpu_buf_cpu(rb0), 255, 255, 0, 255, 1);
    check_rgba("case1-ubo-draw1", nvn_gpu_buf_cpu(rb1), 128, 64, 0, 255, 2);

    nvn_gpu_buf_release(rb1, NULL, NULL);
    nvn_gpu_buf_release(rb0, NULL, NULL);
    nvn_gpu_buf_release(ub1, NULL, NULL);
    nvn_gpu_buf_release(ub0, NULL, NULL);
    nvn_gpu_view_release(rtv1);
    nvn_gpu_view_release(rtv0);
    nvn_gpu_tex_release(rt1);
    nvn_gpu_tex_release(rt0);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 2: SAMPLED_IMAGE + SAMPLER + layout template deduplication across 2 distinct pipelines sharing 1 dlay
static void case_tex_samp_dedup(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[0].binding = 0; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "feedback.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    const uint64_t tc0 = atomic_load(&g_tpl_create);
    nvn_gpu_pipe pipe1 = NULL, pipe2 = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe1, &err));
    const uint64_t tc1 = atomic_load(&g_tpl_create);
    // Second pipeline with different polygon cull state, SAME descriptor set layout -> must reuse dlay template!
    pd.poly.cull = 1;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe2, &err));
    const uint64_t tc2 = atomic_load(&g_tpl_create);
    if (g_tpl_flag && !g_hide_fn && (tc1 != tc0 + 1 || tc2 != tc1)) {
        fprintf(stderr, "FAIL case2-dedup: tc0=%llu tc1=%llu tc2=%llu\n",
                (unsigned long long)tc0, (unsigned long long)tc1, (unsigned long long)tc2);
        g_failures++;
    }

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex src_t = NULL, dst_t = NULL;
    nvn_gpu_view src_v = NULL, dst_v = NULL;
    GPU(nvn_gpu_tex_create(&td, &src_t, &err));
    GPU(nvn_gpu_tex_create(&td, &dst_t, &err));
    GPU(nvn_gpu_view_create(src_t, NULL, &src_v, &err));
    GPU(nvn_gpu_view_create(dst_t, NULL, &dst_v, &err));
    samp_par sp = {0};
    nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));
    uint8_t zero[64] = {0};
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    const float c0[4] = {0.25f, 0.5f, 0.25f, 1.0f};
    nvn_gpu_clear_view(cmd, src_v, c0, NULL, 0);
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = dst_v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe2);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = src_v;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = samp;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, dst_t, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    check_rgba("case2-tex-samp", nvn_gpu_buf_cpu(rb), 128, 128, 64, 255, 2);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(dst_v);
    nvn_gpu_view_release(src_v);
    nvn_gpu_tex_release(dst_t);
    nvn_gpu_tex_release(src_t);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 3: COMBINED_IMAGE_SAMPLER (feedback_combined.frag)
static void case_combined_image_sampler(const char *dir) {
    nvn_gpu_layout_ent ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_FS };
    nvn_gpu_layout lay = { .ent = &ent, .nent = 1 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "feedback_combined.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex src_t = NULL, dst_t = NULL;
    nvn_gpu_view src_v = NULL, dst_v = NULL;
    GPU(nvn_gpu_tex_create(&td, &src_t, &err));
    GPU(nvn_gpu_tex_create(&td, &dst_t, &err));
    GPU(nvn_gpu_view_create(src_t, NULL, &src_v, &err));
    GPU(nvn_gpu_view_create(dst_t, NULL, &dst_v, &err));
    samp_par sp = {0};
    nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));
    uint8_t zero[64] = {0};
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    const float c0[4] = {0.0f, 0.75f, 0.25f, 1.0f};
    nvn_gpu_clear_view(cmd, src_v, c0, NULL, 0);
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = dst_v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_binding b = {0};
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = src_v; b.u.ts.samp = samp;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, dst_t, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    check_rgba("case3-combined", nvn_gpu_buf_cpu(rb), 64, 191, 64, 255, 2);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(dst_v);
    nvn_gpu_view_release(src_v);
    nvn_gpu_tex_release(dst_t);
    nvn_gpu_tex_release(src_t);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 4: Compute SSBO + Graphics SSBO
static void case_ssbo_compute_gfx(const char *dir) {
    nvn_gpu_layout_ent cent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout clay = { .ent = &cent, .nent = 1 };
    nvn_gpu_shader cs = load_shader(dir, "compute_read_payload.comp", NVN_GPU_SH_CS, &clay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    nvn_gpu_layout_ent fent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_FS };
    nvn_gpu_layout flay = { .ent = &fent, .nent = 1 };
    nvn_gpu_shader vs = load_shader(dir, "compute_read_vertex.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "compute_read_buffer.frag", NVN_GPU_SH_FS, &flay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F;
    pd.attr[0].offset = 0;
    pd.attr[0].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe gpipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &gpipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rt = NULL;
    nvn_gpu_view rtv = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err));
    GPU(nvn_gpu_view_create(rt, NULL, &rtv, &err));
    uint8_t zero[64] = {0};
    nvn_gpu_buf ssbo = NULL, rb = NULL;
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &ssbo, &err));
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_binding cb = {0};
    cb.ent = 0; cb.cls = NVN_GPU_RES_SSBO; cb.u.b.buf = ssbo; cb.u.b.off = 0; cb.u.b.len = 64;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    nvn_gpu_compute_to_graphics(cmd);

    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, gpipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_set_vbuf(cmd, 0, ssbo, 0);
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &cb, 1);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    check_rgba("case4-ssbo", nvn_gpu_buf_cpu(rb), 0, 255, 0, 255, 1);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(ssbo, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(cs);
}

// Case 5: STORAGE_IMAGE (img_rgba8.comp)
static void case_storage_image(const char *dir) {
    nvn_gpu_layout_ent ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_IMAGE, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout lay = { .ent = &ent, .nent = 1 };
    nvn_gpu_shader cs = load_shader(dir, "img_rgba8.comp", NVN_GPU_SH_CS, &lay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex img = NULL;
    nvn_gpu_view view = NULL;
    GPU(nvn_gpu_tex_create(&td, &img, &err));
    GPU(nvn_gpu_view_create(img, NULL, &view, &err));
    uint8_t zero[64] = {0};
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_binding b = {0};
    b.ent = 0; b.cls = NVN_GPU_RES_IMAGE; b.u.view = view;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    nvn_gpu_copy_tex_buf(cmd, img, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    // Pixel (1, 0) -> k=1 -> (16, 128, 254, 255)
    check_rgba("case5-storage-image", px + 4, 16, 128, 254, 255, 1);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(img);
    nvn_gpu_shader_release(cs);
}

// Case 6: UNIFORM_TEXEL_BUFFER + STORAGE_BUFFER (tbuf.comp)
static void case_texel_buffer(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_TBUF; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].set = 0; ent[1].binding = 1; ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader cs = load_shader(dir, "tbuf.comp", NVN_GPU_SH_CS, &lay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    const uint32_t in_words[4] = {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
    uint32_t out_zero[4] = {0};
    nvn_gpu_buf tb = NULL, ob = NULL;
    GPU(nvn_gpu_buf_create(sizeof in_words, NVN_GPU_MEM_SHARED, in_words, &tb, &err));
    GPU(nvn_gpu_buf_create(sizeof out_zero, NVN_GPU_MEM_SHARED, out_zero, &ob, &err));
    nvn_gpu_view tv = NULL;
    GPU(nvn_gpu_view_create_buf(tb, 0, sizeof in_words, NVN_FORMAT_R32UI, &tv, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TBUF; b[0].u.view = tv;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = ob; b[1].u.b.off = 0; b[1].u.b.len = sizeof out_zero;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, b, 2);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    submit_and_wait(cmd);

    const uint32_t *got = (const uint32_t *)nvn_gpu_buf_cpu(ob);
    for (int i = 0; i < 4; i++) {
        if (got[i] != in_words[i]) {
            fprintf(stderr, "FAIL case6-tbuf[%d]: got=0x%08x want=0x%08x\n", i, got[i], in_words[i]);
            g_failures++;
        }
    }

    nvn_gpu_view_release(tv);
    nvn_gpu_buf_release(ob, NULL, NULL);
    nvn_gpu_buf_release(tb, NULL, NULL);
    nvn_gpu_shader_release(cs);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 6);
    const char *dir = argv[1];
    g_dq_mode = atoi(argv[2]);
    g_memo_mode = atoi(argv[3]);
    g_tpl_flag = atoi(argv[4]);
    g_hide_fn = atoi(argv[5]);

    struct s2a_vk_device dev = {0};
    char why[192] = "";
    if (s2a_vk_device_create(&dev, why, sizeof why) != 0) {
        fprintf(stderr, "s2a_vk_device_create failed: %s\n", why);
        return 2;
    }

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "desc-template-contract";
    GPU(nvn_gpu_open(&od, &err));
    obs_reset();

    case_ubo_multi_set(dir);
    case_tex_samp_dedup(dir);
    case_combined_image_sampler(dir);
    case_ssbo_compute_gfx(dir);
    case_storage_image(dir);
    case_texel_buffer(dir);

    GPU(nvn_gpu_close(5000000000ull, &err));

    const uint64_t tc = atomic_load(&g_tpl_create);
    const uint64_t td = atomic_load(&g_tpl_destroy);
    const uint64_t tu = atomic_load(&g_tpl_update);
    const uint64_t te = atomic_load(&g_tpl_entries);
    const uint64_t lu = atomic_load(&g_legacy_update);
    const uint64_t lw = atomic_load(&g_legacy_writes);

    const int expect_cap = g_hide_fn ? 0 : 1;
    if (dev.desc_update_template_enabled != expect_cap) {
        fprintf(stderr, "FAIL cap handshake: enabled=%d want=%d hide=%d\n",
                dev.desc_update_template_enabled, expect_cap, g_hide_fn);
        g_failures++;
    }
    if (tc != td) {
        fprintf(stderr, "FAIL template leak: created=%llu destroyed=%llu\n",
                (unsigned long long)tc, (unsigned long long)td);
        g_failures++;
    }

    const int expect_on = (g_tpl_flag != 0) && (g_hide_fn == 0);
    if (expect_on) {
        // Expect 0 legacy vkUpdateDescriptorSets across all 6 cases (complete sets), >= 7 distinct layouts & >= 8 template updates
        if (tc == 0 || tu == 0 || te == 0 || lu != 0 || lw != 0) {
            fprintf(stderr, "FAIL active template counts: create=%llu destroy=%llu update=%llu entries=%llu legacy_upd=%llu legacy_wr=%llu\n",
                    (unsigned long long)tc, (unsigned long long)td, (unsigned long long)tu,
                    (unsigned long long)te, (unsigned long long)lu, (unsigned long long)lw);
            g_failures++;
        }
    } else {
        if (tc != 0 || td != 0 || tu != 0 || lu == 0 || lw == 0) {
            fprintf(stderr, "FAIL fallback counts: create=%llu destroy=%llu update=%llu legacy_upd=%llu legacy_wr=%llu\n",
                    (unsigned long long)tc, (unsigned long long)td, (unsigned long long)tu,
                    (unsigned long long)lu, (unsigned long long)lw);
            g_failures++;
        }
    }

    printf("desc-template dq=%d memo=%d flag=%d hide=%d cap=%d tpl_create=%llu tpl_destroy=%llu "
           "tpl_update=%llu tpl_entries=%llu legacy_upd=%llu legacy_wr=%llu cases=6 failures=%u\n",
           g_dq_mode, g_memo_mode, g_tpl_flag, g_hide_fn, dev.desc_update_template_enabled,
           (unsigned long long)tc, (unsigned long long)td, (unsigned long long)tu,
           (unsigned long long)te, (unsigned long long)lu, (unsigned long long)lw, g_failures);

    REQUIRE(s2a_vk_device_destroy(&dev, 0) == 0);
    return g_failures ? 1 : 0;
}
