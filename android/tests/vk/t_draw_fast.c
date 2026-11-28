// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract & benchmark fixture for per-draw fast paths (debug.s2a.nvk_draw_fast):
//   1) prepare_bind_layouts dirty-bitmap skip (graphics & compute)
//   2) bind_descriptor_groups dynamic-UBO-only fast path (skips pool_set_mask & write_binds)
//   3) Exact invalidation on feedback loop, pool bind, layout transition, clear_rect, barrier, pass_begin
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void nvn_vk_dbg_draw_fast_stat(uint64_t out[4], int reset) __attribute__((weak));
extern void nvn_vk_dbg_cmd_pool_stat(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint32_t *) __attribute__((weak));
extern void nvn_vk_dbg_fence_pool_stat(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint32_t *) __attribute__((weak));
extern uint64_t nvn_vk_dbg_dq_recs(void);
extern void nvn_vk_dbg_dq_opt_stat(uint64_t out[4], int reset);

static nvn_gpu_err err;
static int g_dq_mode = 0;
static int g_opt_layout = 1;
static int g_fast_flag = 1;
static int g_combo;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;
static PFN_vkCmdPushDescriptorSetKHR real_push_desc;
static PFN_vkUpdateDescriptorSetWithTemplate real_update_template;
static _Atomic uint32_t push_calls, template_calls;

static VKAPI_ATTR void VKAPI_CALL tap_push_desc(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                               uint32_t set, uint32_t n, const VkWriteDescriptorSet *writes) {
    atomic_fetch_add(&push_calls, 1);
    real_push_desc(cb, bp, layout, set, n, writes);
}

static VKAPI_ATTR void VKAPI_CALL tap_update_template(VkDevice device, VkDescriptorSet set,
                                                     VkDescriptorUpdateTemplate tpl, const void *data) {
    atomic_fetch_add(&template_calls, 1);
    real_update_template(device, set, tpl, data);
}

extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(device, name);
    if (!g_combo || !fn) return fn;
    if (!strcmp(name, "vkCmdPushDescriptorSetKHR")) {
        real_push_desc = (PFN_vkCmdPushDescriptorSetKHR)fn;
        return (PFN_vkVoidFunction)tap_push_desc;
    }
    if (!strcmp(name, "vkUpdateDescriptorSetWithTemplate")) {
        real_update_template = (PFN_vkUpdateDescriptorSetWithTemplate)fn;
        return (PFN_vkVoidFunction)tap_update_template;
    }
    return fn;
}

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "draw-fast fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---- Platform stubs ----
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_mode;
    if (!strcmp(n, "nvk_opt_layout")) return g_opt_layout;
    if (!strcmp(n, "nvk_draw_fast")) return g_fast_flag;
    if (!strcmp(n, "nvk_bind_dedup") || !strcmp(n, "nvk_push_desc") || !strcmp(n, "nvk_desc_template"))
        return g_combo != 0;
    if (!strcmp(n, "nvk_hot_hash")) return g_combo >= 2;
    if (!strcmp(n, "nvk_cmd_pool") || !strcmp(n, "nvk_fence_pool")) return g_combo >= 3;
    if (!strcmp(n, "nvk_dq_opt")) return g_combo == 4;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    (void)l;
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done(void *u, const nvn_gpu_done *d) { (void)u; REQUIRE(d->status == NVN_GPU_DONE_OK); }

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

static void check_u64(const char *tag, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got=%llu want=%llu\n", tag, (unsigned long long)got, (unsigned long long)want);
        g_failures++;
    }
}

static void set_scissor_dyn(nvn_gpu_cmd cmd, uint32_t w, uint32_t h, int32_t sx, int32_t sy, uint32_t sw, uint32_t sh) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1;
    dyn.vp[0][2] = (float)w; dyn.vp[0][3] = (float)h; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1;
    dyn.sc[0] = sx; dyn.sc[1] = sy; dyn.sc[2] = (int32_t)sw; dyn.sc[3] = (int32_t)sh;
    nvn_gpu_set_dyn(cmd, &dyn);
}

static void make_rt(uint32_t w, uint32_t h, nvn_gpu_tex *t, nvn_gpu_view *v, nvn_gpu_buf *rb) {
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = w; td.d.height = h; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    GPU(nvn_gpu_tex_create(&td, t, &err));
    GPU(nvn_gpu_view_create(*t, NULL, v, &err));
    if (rb) {
        size_t bytes = (size_t)w * (size_t)h * 4;
        void *z = calloc(1, bytes);
        REQUIRE(z);
        GPU(nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, z, rb, &err));
        free(z);
    }
}

// Case 1: 256-draw dynamic UBO loop in 1 pass — measures lay_skip, lay_full, dyn_hit, wb_calls, wall time & 4-quadrant pixels
static void case1_dyn_ubo_256(const char *dir, uint64_t *out_us, uint64_t out_st[4]) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].set = 1; ent[1].binding = 3; ent[1].cls = NVN_GPU_RES_UBO; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "ubo_dyn.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL; nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &rt, &rtv, &rb);

    // 4 dynamic UBO slices at 0, 256, 512, 768
    uint8_t udata[1024] = {0};
    const float vals[4][4] = {
        {1.0f, 0.25f, 0.0f, 1.0f},
        {0.5f, 0.75f, 0.0f, 1.0f},
        {0.25f, 1.0f, 0.0f, 1.0f},
        {0.75f, 0.5f, 0.0f, 1.0f},
    };
    for (int k = 0; k < 4; k++) memcpy(udata + (size_t)k * 256, vals[k], sizeof vals[k]);
    nvn_gpu_buf ub = NULL;
    GPU(nvn_gpu_buf_create(sizeof udata, NVN_GPU_MEM_SHARED, udata, &ub, &err));

    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    uintptr_t recycled_cmd = 0;
    if (g_combo >= 3) {
        // 让同一贴图留在上一段的布局索引中。放弃段的附件布局从未执行，下一段须重新迁移。
        nvn_gpu_cmd aborted = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &aborted, &err));
        nvn_gpu_pass_begin(aborted, &p);
        nvn_gpu_pass_end(aborted);
        recycled_cmd = (uintptr_t)aborted;
        nvn_gpu_cmd_abort(aborted);
    }
    if (nvn_vk_dbg_draw_fast_stat) nvn_vk_dbg_draw_fast_stat(NULL, 1);
    const uint64_t t0 = mono_ns();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    if (g_combo >= 3) REQUIRE((uintptr_t)cmd == recycled_cmd);
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    const int32_t qx[4] = {0, 2, 0, 2};
    const int32_t qy[4] = {0, 0, 2, 2};
    for (int i = 0; i < 256; i++) {
        const int q = i & 3;
        set_scissor_dyn(cmd, 4, 4, qx[q], qy[q], 2, 2);
        nvn_gpu_binding b[2] = {{0}};
        b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub; b[0].u.b.off = (uint64_t)q * 256u; b[0].u.b.len = 16;
        b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub; b[1].u.b.off = (uint64_t)((q + 1) & 3) * 256u; b[1].u.b.len = 16;
        if (g_combo) {
            // 最后一笔改推送组，同时让普通组偏移回到首笔值，抓出快路径漏记绑定影子。
            b[0].u.b.off = i == 255 ? 512u : 0u;
            if (i == 255) b[1].u.b.off = 256u;
        }
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
        nvn_gpu_draw(cmd, &da);
    }
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    const uint64_t t1 = mono_ns();
    submit_and_wait(cmd);
    if (g_combo >= 3) {
        // 第二次真正的读回提交应使用已经完成的 fence，仍要读到同一份渲染结果。
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
        submit_and_wait(cmd);
        REQUIRE(nvn_vk_dbg_cmd_pool_stat && nvn_vk_dbg_fence_pool_stat);
        uint64_t cmd_hit = 0, fence_hit = 0, fence_miss = 0;
        nvn_vk_dbg_cmd_pool_stat(&cmd_hit, NULL, NULL, NULL, NULL, NULL);
        nvn_vk_dbg_fence_pool_stat(&fence_hit, &fence_miss, NULL, NULL, NULL);
        REQUIRE(cmd_hit && fence_hit == 1 && fence_miss == 1);
        printf("draw-fast pooled dq=%d same_cmd=1 cmd_hit=%llu fence_hit=%llu fence_miss=%llu\n",
               g_dq_mode, (unsigned long long)cmd_hit, (unsigned long long)fence_hit, (unsigned long long)fence_miss);
    }

    // Verify all 4 quadrants: R = vals[q].x, G = vals[(q+1)&3].y
    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c1-q0", px + (0 * 4 + 0) * 4, 255, 191, 0, 255, 2);
    check_rgba("c1-q1", px + (0 * 4 + 2) * 4, g_combo ? 255 : 128, 255, 0, 255, 2);
    check_rgba("c1-q2", px + (2 * 4 + 0) * 4, g_combo ? 255 : 64, 128, 0, 255, 2);
    check_rgba("c1-q3", px + (2 * 4 + 2) * 4, g_combo ? 64 : 191, g_combo ? 191 : 64, 0, 255, 2);
    if (g_combo) {
        check_u64("c1-push-calls", atomic_load(&push_calls), 2);
        check_u64("c1-template-calls", atomic_load(&template_calls), 1);
    }

    uint64_t st[4] = {0};
    if (nvn_vk_dbg_draw_fast_stat) nvn_vk_dbg_draw_fast_stat(st, 0);
    *out_us = (t1 - t0) / 1000ull;
    memcpy(out_st, st, sizeof st);
    if (g_fast_flag) {
        if (!nvn_vk_dbg_draw_fast_stat) { fprintf(stderr, "FAIL c1-stat-missing\n"); g_failures++; }
        else {
            check_u64("c1-lay-skip", st[0], 255);
            check_u64("c1-lay-full", st[1], 1);
            check_u64("c1-dyn-hit", st[2], g_combo ? 254 : 255);
            check_u64("c1-wb-calls", st[3], g_combo ? 10 : 5);
        }
    }

    nvn_gpu_buf_release(ub, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 2: Exact invalidation on feedback-loop entry, consecutive feedback draws, and feedback exit!
static void case2_feedback_invalidation(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[0].binding = 0; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "feedback.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex ext_t = NULL, rt_t = NULL;
    nvn_gpu_view ext_v = NULL, rt_v = NULL;
    nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &ext_t, &ext_v, NULL);
    make_rt(4, 4, &rt_t, &rt_v, &rb);
    samp_par sp = {0}; nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    const float ext_col[4] = {0.0f, 0.5f, 0.25f, 1.0f};
    nvn_gpu_clear_view(cmd, ext_v, ext_col, NULL, 0);

    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rt_v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    // Draw 1: sample external texture -> R=0.25 (64), G=128, B=64; clears layout_dirty!
    set_scissor_dyn(cmd, 4, 4, 0, 0, 4, 4);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = ext_v;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = samp;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 2: bind current render target rt_v (FEEDBACK!) -> MUST invalidate layout_dirty & snapshot -> R=0.50 (128)
    b[0].u.view = rt_v;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 3: SAME feedback binding rt_v -> MUST NOT skip prepare_bind_layouts (feedback active) -> R=0.75 (191)
    nvn_gpu_draw(cmd, &da);

    // Draw 4: switch right half back to ext_v -> exits feedback cleanly -> right=(64,128,64), left stays (191,128,64)
    set_scissor_dyn(cmd, 4, 4, 2, 0, 2, 4);
    b[0].u.view = ext_v;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt_t, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c2-fb-left", px + 0, 191, 128, 64, 255, 2);
    check_rgba("c2-fb-right", px + 2 * 4, 64, 128, 64, 255, 2);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(rt_v);
    nvn_gpu_tex_release(rt_t);
    nvn_gpu_view_release(ext_v);
    nvn_gpu_tex_release(ext_t);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 3: 64-draw pool-bound loop changing only push constant index — skips pool_check_layouts & feedback_prepare 63/64 times
static void case3_pool_64_draws(const char *dir, uint64_t *out_pool_skip, uint64_t *out_pool_full) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[0].binding = 0; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 3; ent[0].src_kind = NVN_RB_TEXP; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2, .push_bytes = 4 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "feedback_pool_dyn.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex t0 = NULL, t1 = NULL, rt = NULL;
    nvn_gpu_view v0 = NULL, v1 = NULL, rtv = NULL;
    nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &t0, &v0, NULL);
    make_rt(4, 4, &t1, &v1, NULL);
    make_rt(4, 4, &rt, &rtv, &rb);
    samp_par sp = {0}; nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));

    nvn_gpu_pool_item items[2] = {{ .state = NVN_GPU_POOL_READY }, { .state = NVN_GPU_POOL_READY }};
    items[0].resource.view = v0;
    items[1].resource.view = v1;
    nvn_gpu_pool_desc pdesc = { .kind = NVN_GPU_POOL_IMAGE, .count = 2, .items = items };
    pdesc.dummy.view = v0;
    nvn_gpu_pool_table pt = NULL;
    GPU(nvn_gpu_pool_create(&pdesc, &pt, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    const float c0[4] = {0.25f, 0.25f, 0.5f, 1.0f};
    const float c1[4] = {0.5f, 0.75f, 0.25f, 1.0f};
    nvn_gpu_clear_view(cmd, v0, c0, NULL, 0);
    nvn_gpu_clear_view(cmd, v1, c1, NULL, 0);

    if (nvn_vk_dbg_draw_fast_stat) nvn_vk_dbg_draw_fast_stat(NULL, 1);
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    set_scissor_dyn(cmd, 4, 4, 0, 0, 4, 4);
    nvn_gpu_binding sb = {0}; sb.ent = 1; sb.cls = NVN_GPU_RES_SAMP; sb.u.samp = samp;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &sb, 1);
    GPU(nvn_gpu_pool_bind(cmd, NVN_GPU_SH_FS, 0, pt, &err));
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t pidx = i & 1u;
        nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, &pidx, sizeof pidx);
        nvn_gpu_draw(cmd, &da);
    }
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    // Last draw i=63 -> pidx=1 -> c1 (0.5, 0.75, 0.25) + (0.25, 0, 0) = (191, 191, 64, 255)
    check_rgba("c3-pool-pixel", nvn_gpu_buf_cpu(rb), 191, 191, 64, 255, 2);

    uint64_t st[4] = {0};
    if (nvn_vk_dbg_draw_fast_stat) nvn_vk_dbg_draw_fast_stat(st, 0);
    *out_pool_skip = st[0];
    *out_pool_full = st[1];
    if (g_fast_flag && nvn_vk_dbg_draw_fast_stat) {
        check_u64("c3-pool-skip", st[0], 63);
        check_u64("c3-pool-full", st[1], 1);
    }

    nvn_gpu_pool_release(pt);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(rtv); nvn_gpu_tex_release(rt);
    nvn_gpu_view_release(v1); nvn_gpu_tex_release(t1);
    nvn_gpu_view_release(v0); nvn_gpu_tex_release(t0);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 4: Layout transition invalidation (pass1 renders texA -> pass2 binds texB then texA, clear_rect + compute barrier)
static void case4_layout_transition_and_compute(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[0].binding = 0; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "feedback.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_layout_ent cent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout clay = { .ent = &cent, .nent = 1, .push_bytes = 4 };
    nvn_gpu_shader cs = load_shader(dir, "pushc.comp", NVN_GPU_SH_CS, &clay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    nvn_gpu_tex texA = NULL, texB = NULL, rt = NULL;
    nvn_gpu_view viewA = NULL, viewB = NULL, rtv = NULL;
    nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &texA, &viewA, NULL);
    make_rt(4, 4, &texB, &viewB, NULL);
    make_rt(4, 4, &rt, &rtv, &rb);
    samp_par sp = {0}; nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));

    uint32_t zero[64] = {0};
    nvn_gpu_buf ssbo = NULL;
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &ssbo, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    const float cb_col[4] = {0.0f, 0.25f, 0.75f, 1.0f};
    nvn_gpu_clear_view(cmd, viewB, cb_col, NULL, 0);

    // Pass 1: clear texA via render pass -> leaves texA in COLOR_ATTACHMENT_OPTIMAL (when opt=1)
    nvn_gpu_pass_desc p1 = {0};
    p1.ncolor = 1; p1.color[0].view = viewA; p1.color[0].load = NVN_GPU_LOAD_CLEAR; p1.color[0].store = NVN_GPU_STORE_STORE;
    p1.color[0].clear.f[0] = 0.5f; p1.color[0].clear.f[1] = 0.75f; p1.color[0].clear.f[2] = 0.25f; p1.color[0].clear.f[3] = 1.0f;
    p1.w = p1.h = 4; p1.layers = 1;
    nvn_gpu_pass_begin(cmd, &p1);
    nvn_gpu_pass_end(cmd);

    // Pass 2: Draw 1 samples viewB (clears layout_dirty), Draw 2 switches to viewA (MUST invalidate & transition!), then clear_rect
    nvn_gpu_pass_desc p2 = {0};
    p2.ncolor = 1; p2.color[0].view = rtv; p2.color[0].load = NVN_GPU_LOAD_CLEAR; p2.color[0].store = NVN_GPU_STORE_STORE;
    p2.w = p2.h = 4; p2.layers = 1;
    nvn_gpu_pass_begin(cmd, &p2);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    set_scissor_dyn(cmd, 4, 4, 0, 0, 2, 4);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = viewB;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = samp;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    set_scissor_dyn(cmd, 4, 4, 2, 0, 2, 4);
    b[0].u.view = viewA;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    nvn_gpu_clear cl = {0};
    cl.color = 0; cl.color_mask = 0xf; cl.x = 0; cl.y = 2; cl.w = 2; cl.h = 2;
    cl.value.f[2] = 1.0f; cl.value.f[3] = 1.0f;
    nvn_gpu_clear_rect(cmd, &cl);
    nvn_gpu_pass_end(cmd);

    // Compute: 2 dispatches separated by barrier
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_binding cb = {0}; cb.ent = 0; cb.cls = NVN_GPU_RES_SSBO; cb.u.b.buf = ssbo; cb.u.b.off = 0; cb.u.b.len = sizeof zero;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
    uint32_t pv = 10;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &pv, sizeof pv);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    nvn_gpu_barrier(cmd, NVN_BARRIER_INVALIDATE_SHADER_BIT);
    pv = 42;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &pv, sizeof pv);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);

    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c4-texB-tl", px + (0 * 4 + 0) * 4, 64, 64, 191, 255, 2);
    check_rgba("c4-texA-tr", px + (0 * 4 + 2) * 4, 191, 191, 64, 255, 2);
    check_rgba("c4-clear-bl", px + (2 * 4 + 0) * 4, 0, 0, 255, 255, 2);
    const uint32_t *out = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(ssbo);
    for (uint32_t i = 0; i < 64; i++) {
        if (out[i] != 42u + i) { fprintf(stderr, "FAIL c4-cs[%u]: got=%u want=%u\n", i, out[i], 42u + i); g_failures++; break; }
    }

    nvn_gpu_buf_release(ssbo, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(rtv); nvn_gpu_tex_release(rt);
    nvn_gpu_view_release(viewB); nvn_gpu_tex_release(texB);
    nvn_gpu_view_release(viewA); nvn_gpu_tex_release(texA);
    nvn_gpu_shader_release(cs);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 5);
    const char *dir = argv[1];
    g_dq_mode = atoi(argv[2]);
    g_opt_layout = atoi(argv[3]);
    g_fast_flag = atoi(argv[4]);
    g_combo = argc > 5 ? atoi(argv[5]) : 0;
    if (g_combo == 4) REQUIRE(g_dq_mode == 1);

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    if (g_combo) REQUIRE(dev.push_descriptor_enabled && dev.desc_update_template_enabled && g_fast_flag);
    nvn_gpu_open_desc od = { .host_device = &dev };
    GPU(nvn_gpu_open(&od, &err));

    uint64_t dq_start[4] = {0};
    const uint64_t recs_start = nvn_vk_dbg_dq_recs();
    if (g_combo == 4) nvn_vk_dbg_dq_opt_stat(dq_start, 0);
    uint64_t ubo_us = 0, st[4] = {0}, p_skip = 0, p_full = 0;
    case1_dyn_ubo_256(dir, &ubo_us, st);
    if (g_combo == 4) {
        case2_feedback_invalidation(dir);
        uint64_t dq_end[4] = {0};
        nvn_vk_dbg_dq_opt_stat(dq_end, 0);
        const uint64_t recs = nvn_vk_dbg_dq_recs() - recs_start;
        const uint64_t pub = dq_end[0] - dq_start[0];
        const uint64_t rd = dq_end[1] - dq_start[1];
        const uint64_t flush = dq_end[3] - dq_start[3];
        REQUIRE(recs > 256 && pub > 0 && pub < recs && rd > 0 && rd < recs && flush > 0);
        printf("draw-fast dq-opt combo=%d recs=%llu pub=%llu rd=%llu flush=%llu\n", g_combo,
               (unsigned long long)recs, (unsigned long long)pub, (unsigned long long)rd, (unsigned long long)flush);
    }
    if (!g_combo) {
        case2_feedback_invalidation(dir);
        case3_pool_64_draws(dir, &p_skip, &p_full);
        case4_layout_transition_and_compute(dir);
    }

    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);

    printf("draw-fast dq=%d opt=%d flag=%d combo=%d ubo_us=%llu lay_skip=%llu lay_full=%llu dyn_hit=%llu wb=%llu pool_skip=%llu/%llu push=%u tpl=%u cases=%u failures=%u\n",
           g_dq_mode, g_opt_layout, g_fast_flag, g_combo,
           (unsigned long long)ubo_us,
           (unsigned long long)st[0], (unsigned long long)st[1],
           (unsigned long long)st[2], (unsigned long long)st[3],
           (unsigned long long)p_skip, (unsigned long long)p_full, atomic_load(&push_calls),
           atomic_load(&template_calls), g_combo == 4 ? 2u : g_combo ? 1u : 4u,
           g_failures);
    return g_failures ? 1 : 0;
}
