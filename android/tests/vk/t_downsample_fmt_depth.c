// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Downsample（DM_COPY_RESOLVE）端到端契约测试（rp_op_copy_t2t + 真实 Vulkan 1.1 + VVL + SyncVal）：
// 1. 1x 红蓝不对称颜色换格式（RGBA8 -> BGRA8 / BGRA8 -> RGBA8 / RGBA8 -> RGBA8_SRGB）像素精确读回；
// 2. D32F / D16 4x MSAA -> 1x sample0 深度 resolve 精确读回；
// 3. 异尺寸角裁切 4x MSAA -> 1x（8x8 -> 4x4 颜色与 D32F 深度）；
// 4. 不支持边缘组合（整数 MSAA / D24S8 MSAA / 放大 MSAA / 跨格式 MSAA / 1x->4x / 整数 1x 换格式）显式记账拒收。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "nvn_deswizzle.h"
#include "nvn_mtl.h"
#include "nvn_tex.h"
#include "rp_exec.h"
#include "rp_res.h"
#include <math.h>

int rp_tm_on;
static nvn_gpu_cmd active;
static uint64_t fence_signal;
static int g_skip_for_neg;

void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_seg_serial(void) { return 1; }
uint64_t rp_sub_serial(void) { return 1; }
uint64_t rp_sub_fence(void) { return fence_signal + 1; }
int rp_fence_done(uint64_t value) { return nvn_gpu_fence_done() >= value; }
int rp_occl_want(void) { return 0; }
void rp_occl_pass_opened(int declared) { (void)declared; }
void rp_occl_pass_closed(void) {}
void rp_occl_pause(nvn_gpu_cmd c) { (void)c; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub) { *last_sub = rp_sub_serial(); return active; }
static pthread_mutex_t submit_mu = PTHREAD_MUTEX_INITIALIZER;
void nvn_hle_submit_lock(void) { pthread_mutex_lock(&submit_mu); }
void nvn_hle_submit_unlock(void) { pthread_mutex_unlock(&submit_mu); }
uint64_t nvn_be_last_fence(void) { return fence_signal; }
int rp_in_submit(void) { return active != NULL; }
nvn_gpu_cmd rp_side_begin(const char *label) { (void)label; return begin_cmd(); }
uint64_t rp_side_commit(nvn_gpu_cmd cmd) {
    nvn_gpu_err err = {0};
    const uint64_t fence = ++fence_signal;
    const nvn_gpu_rc rc = nvn_gpu_cmd_commit(cmd, fence, on_done, NULL, &err);
    CHECK(OK(rc), "downsample submit: %s", err.why);
    return OK(rc) ? fence : 0;
}

enum { POOL_STRIDE = 65536, MAX_TEX = 24 };
static uint8_t storage[MAX_TEX * POOL_STRIDE];
static pool_t pool = {.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};

static tex_t make_guest_tex(uint64_t id, int fmt, int w, int h, int samples) {
    tex_t t = {0};
    t.d.target = samples > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D;
    t.d.format = fmt;
    t.d.width = w;
    t.d.height = h;
    t.d.depth = 1;
    t.d.levels = 1;
    t.d.samples = samples > 1 ? samples : 0;
    t.serial = id;
    t.pool = &pool;
    t.off = (id - 1) * POOL_STRIDE;
    t.mem = storage + (id - 1) * POOL_STRIDE;
    t.has_layout = 1;
    nvn_tex_layout(&t.d, &t.L);
    return t;
}

static void clear_color_rt(const tex_t *t, float r, float g, float b, float a) {
    p_set_rt rt = {.n = 1, .color = {t}};
    rp_pass_set_rt(active, &rt);
    const p_clear_color cc = {.index = 0, .mask = 15, .rgba = {r, g, b, a}};
    rp_pass_clear_color(active, &cc, NULL);
    rp_pass_flush(active);
}

static void do_downsample(const tex_t *src, const tex_t *dst) {
    p_copy_t2t op = {0};
    op.src = src;
    op.dst = dst;
    op.flags = (int32_t)DM_COPY_RESOLVE;
    op.sr = (NVNcopyRegion){0, 0, 0, src->d.width, src->d.height, 1};
    op.dr = (NVNcopyRegion){0, 0, 0, dst->d.width, dst->d.height, 1};
    rp_op_copy_t2t(active, &op, &op);
}

static void readback_tex(const tex_t *t, nvn_gpu_buf out, uint64_t row_bytes, uint64_t image_bytes) {
    nvn_gpu_tex gt = rp_tex_resolve_plain(t, active);
    CHECK(gt != NULL, "resolve tex %llu for readback", (unsigned long long)t->serial);
    if (gt) nvn_gpu_copy_tex_buf(active, gt, 0, 0, NULL, out, 0, row_bytes, image_bytes);
}

static void commit_and_wait(void) {
    nvn_gpu_err err = {0};
    const uint64_t f = rp_side_commit(active);
    CHECK(f && OK(nvn_gpu_fence_wait(f, 5000000000ull, &err)), "wait fence %llu: %s", (unsigned long long)f, err.why);
    active = begin_cmd();
}

static void test_1x_color_format_convert(void) {
    tex_t t_rgba = make_guest_tex(1, NVN_FORMAT_RGBA8, 4, 4, 1);
    tex_t t_bgra = make_guest_tex(2, NVN_FORMAT_BGRA8, 4, 4, 1);
    tex_t t_back = make_guest_tex(3, NVN_FORMAT_RGBA8, 4, 4, 1);
    tex_t t_lin  = make_guest_tex(4, NVN_FORMAT_RGBA8, 4, 4, 1);
    tex_t t_srgb = make_guest_tex(5, NVN_FORMAT_RGBA8_SRGB, 4, 4, 1);

    // 红蓝不对称色：R=220, G=60, B=30, A=255；目标先清黑防止误过。
    clear_color_rt(&t_rgba, 220.0f / 255.0f, 60.0f / 255.0f, 30.0f / 255.0f, 1.0f);
    clear_color_rt(&t_bgra, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_color_rt(&t_back, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_color_rt(&t_lin, 132.0f / 255.0f, 130.0f / 255.0f, 132.0f / 255.0f, 1.0f);
    clear_color_rt(&t_srgb, 0.0f, 0.0f, 0.0f, 0.0f);

    if (!g_skip_for_neg) {
        do_downsample(&t_rgba, &t_bgra);
        do_downsample(&t_bgra, &t_back);
        do_downsample(&t_lin, &t_srgb);
    }

    nvn_gpu_buf rb_bgra = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf rb_back = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf rb_srgb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    readback_tex(&t_bgra, rb_bgra, 16, 64);
    readback_tex(&t_back, rb_back, 16, 64);
    readback_tex(&t_srgb, rb_srgb, 16, 64);
    commit_and_wait();

    const uint8_t *bg = buf_cpu(rb_bgra), *bk = buf_cpu(rb_back), *sr = buf_cpu(rb_srgb);
    unsigned bad_bg = 0, bad_bk = 0, bad_sr = 0;
    for (unsigned i = 0; i < 16; i++) {
        // BGRA8 原始字节序为 [B, G, R, A] = [30, 60, 220, 255]
        if (abs((int)bg[i * 4 + 0] - 30) > 1 || abs((int)bg[i * 4 + 1] - 60) > 1 ||
            abs((int)bg[i * 4 + 2] - 220) > 1 || bg[i * 4 + 3] != 255) bad_bg++;
        // RGBA8 原始字节序为 [R, G, B, A] = [220, 60, 30, 255]
        if (abs((int)bk[i * 4 + 0] - 220) > 1 || abs((int)bk[i * 4 + 1] - 60) > 1 ||
            abs((int)bk[i * 4 + 2] - 30) > 1 || bk[i * 4 + 3] != 255) bad_bk++;
        // 线性 0.5176 -> sRGB 编码 ~190（允许 +/-2）
        if (abs((int)sr[i * 4 + 0] - 190) > 2 || abs((int)sr[i * 4 + 1] - 189) > 2 ||
            abs((int)sr[i * 4 + 2] - 190) > 2 || sr[i * 4 + 3] != 255) bad_sr++;
    }
    CHECK(bad_bg == 0, "1x RGBA8->BGRA8 downsample mismatch=%u first=%u,%u,%u,%u (want 30,60,220,255)",
          bad_bg, bg[0], bg[1], bg[2], bg[3]);
    CHECK(bad_bk == 0, "1x BGRA8->RGBA8 downsample mismatch=%u first=%u,%u,%u,%u (want 220,60,30,255)",
          bad_bk, bk[0], bk[1], bk[2], bk[3]);
    CHECK(bad_sr == 0, "1x RGBA8->RGBA8_SRGB downsample mismatch=%u first=%u,%u,%u,%u (want ~190,189,190,255)",
          bad_sr, sr[0], sr[1], sr[2], sr[3]);
    printf("DS-1X-COLOR bgra=%u,%u,%u,%u back=%u,%u,%u,%u srgb=%u,%u,%u,%u\n",
           bg[0], bg[1], bg[2], bg[3], bk[0], bk[1], bk[2], bk[3], sr[0], sr[1], sr[2], sr[3]);

    nvn_gpu_buf_release(rb_bgra, NULL, NULL);
    nvn_gpu_buf_release(rb_back, NULL, NULL);
    nvn_gpu_buf_release(rb_srgb, NULL, NULL);
}

static void fill_depth_quadrants(const tex_t *t, float d0, float d1, float d2, float d3) {
    nvn_gpu_tex gt = rp_tex_resolve_plain(t, active);
    CHECK(gt != NULL, "resolve depth tex %llu", (unsigned long long)t->serial);
    if (!gt) return;
    nvn_gpu_view dv = make_view(gt);
    nvn_gpu_pass_desc pass = {0};
    pass.w = (uint32_t)t->d.width; pass.h = (uint32_t)t->d.height; pass.layers = 1;
    pass.depth.view = dv; pass.depth.load = NVN_GPU_LOAD_CLEAR; pass.depth.store = NVN_GPU_STORE_STORE;
    pass.clear_depth = d0;
    nvn_gpu_pass_begin(active, &pass);
    const float vals[4] = { d0, d1, d2, d3 };
    const uint32_t hw = (uint32_t)t->d.width / 2, hh = (uint32_t)t->d.height / 2;
    for (unsigned q = 1; q < 4; q++) {
        nvn_gpu_clear cl = {0};
        cl.color = -1; cl.depth = 1; cl.depth_value = vals[q];
        cl.x = (int32_t)((q % 2) * hw); cl.y = (int32_t)((q / 2) * hh); cl.w = hw; cl.h = hh;
        nvn_gpu_clear_rect(active, &cl);
    }
    nvn_gpu_pass_end(active);
    nvn_gpu_view_release(dv);
    rp_tex_mark_gpu_written(t);
}

static void test_msaa_depth_resolve(void) {
    // 1. D32F 4x MSAA -> 1x (4x4 同尺寸四象限精确深度)
    tex_t d32_ms = make_guest_tex(6, NVN_FORMAT_DEPTH32F, 4, 4, 4);
    tex_t d32_1x = make_guest_tex(7, NVN_FORMAT_DEPTH32F, 4, 4, 1);
    fill_depth_quadrants(&d32_ms, 0.125f, 0.375f, 0.625f, 0.875f);
    fill_depth_quadrants(&d32_1x, 0.9375f, 0.9375f, 0.9375f, 0.9375f);
    if (!g_skip_for_neg) do_downsample(&d32_ms, &d32_1x);

    // 2. D16 4x MSAA -> 1x (4x4 单平面 16-bit 深度)
    tex_t d16_ms = make_guest_tex(8, NVN_FORMAT_DEPTH16, 4, 4, 4);
    tex_t d16_1x = make_guest_tex(9, NVN_FORMAT_DEPTH16, 4, 4, 1);
    fill_depth_quadrants(&d16_ms, 0.25f, 0.5f, 0.75f, 0.125f);
    fill_depth_quadrants(&d16_1x, 0.9375f, 0.9375f, 0.9375f, 0.9375f);
    do_downsample(&d16_ms, &d16_1x);

    // 3. 异尺寸角裁切 MSAA -> 1x：8x8 4x MSAA -> 4x4 1x（颜色 + D32F 深度）
    tex_t c_big = make_guest_tex(10, NVN_FORMAT_RGBA8, 8, 8, 4);
    tex_t c_sml = make_guest_tex(11, NVN_FORMAT_RGBA8, 4, 4, 1);
    {
        // 左上 4x4 清纯红，其余清纯蓝
        nvn_gpu_tex gt = rp_tex_resolve_plain(&c_big, active);
        nvn_gpu_view cv = make_view(gt);
        nvn_gpu_pass_desc pass = {0};
        pass.w = 8; pass.h = 8; pass.layers = 1; pass.ncolor = 1;
        pass.color[0].view = cv; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[2] = 1.0f; pass.color[0].clear.f[3] = 1.0f;
        nvn_gpu_pass_begin(active, &pass);
        nvn_gpu_clear cl = {0}; cl.color = 0; cl.color_mask = 0xf;
        cl.value.f[0] = 1.0f; cl.value.f[3] = 1.0f; cl.w = 4; cl.h = 4;
        nvn_gpu_clear_rect(active, &cl);
        nvn_gpu_pass_end(active);
        nvn_gpu_view_release(cv);
        rp_tex_mark_gpu_written(&c_big);
    }
    clear_color_rt(&c_sml, 0.0f, 0.0f, 0.0f, 0.0f);
    do_downsample(&c_big, &c_sml);

    tex_t d_big = make_guest_tex(12, NVN_FORMAT_DEPTH32F, 8, 8, 4);
    tex_t d_sml = make_guest_tex(13, NVN_FORMAT_DEPTH32F, 4, 4, 1);
    fill_depth_quadrants(&d_big, 0.25f, 0.75f, 0.75f, 0.75f);
    fill_depth_quadrants(&d_sml, 0.9375f, 0.9375f, 0.9375f, 0.9375f);
    do_downsample(&d_big, &d_sml);

    nvn_gpu_buf rb_d32 = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf rb_d16 = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf rb_csml = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf rb_dsml = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    readback_tex(&d32_1x, rb_d32, 16, 64);
    readback_tex(&d16_1x, rb_d16, 8, 32);
    readback_tex(&c_sml, rb_csml, 16, 64);
    readback_tex(&d_sml, rb_dsml, 16, 64);
    commit_and_wait();

    const float *fd32 = (const float *)(const void *)buf_cpu(rb_d32);
    const uint16_t *ud16 = (const uint16_t *)(const void *)buf_cpu(rb_d16);
    const uint8_t *pcs = buf_cpu(rb_csml);
    const float *fds = (const float *)(const void *)buf_cpu(rb_dsml);
    const float exp32[4] = { 0.125f, 0.375f, 0.625f, 0.875f };
    const float exp16[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
    unsigned bad32 = 0, bad16 = 0, badcs = 0, badds = 0;
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned i = y * 4 + x;
            unsigned q = (y / 2) * 2 + (x / 2);
            if (fabsf(fd32[i] - exp32[q]) > 1e-5f) bad32++;
            if (fabsf((float)ud16[i] / 65535.0f - exp16[q]) > 1e-3f) bad16++;
            if (pcs[i * 4 + 0] != 255 || pcs[i * 4 + 1] != 0 || pcs[i * 4 + 2] != 0 || pcs[i * 4 + 3] != 255) badcs++;
            if (fabsf(fds[i] - 0.25f) > 1e-5f) badds++;
        }
    }
    CHECK(bad32 == 0, "D32F 4x->1x sample0 depth resolve mismatch=%u q=(%.4f,%.4f,%.4f,%.4f)",
          bad32, fd32[0], fd32[2], fd32[8], fd32[10]);
    CHECK(bad16 == 0, "D16 4x->1x sample0 depth resolve mismatch=%u q=(%u,%u,%u,%u)",
          bad16, ud16[0], ud16[2], ud16[8], ud16[10]);
    CHECK(badcs == 0, "8x8->4x4 color MSAA corner resolve mismatch=%u first=%u,%u,%u,%u",
          badcs, pcs[0], pcs[1], pcs[2], pcs[3]);
    CHECK(badds == 0, "8x8->4x4 D32F MSAA corner resolve mismatch=%u first=%.4f", badds, fds[0]);
    printf("DS-MSAA-DEPTH d32=(%.3f,%.3f,%.3f,%.3f) d16=(%.3f,%.3f,%.3f,%.3f) corner_c=%u,%u,%u,%u corner_d=%.3f\n",
           fd32[0], fd32[2], fd32[8], fd32[10],
           ud16[0] / 65535.0f, ud16[2] / 65535.0f, ud16[8] / 65535.0f, ud16[10] / 65535.0f,
           pcs[0], pcs[1], pcs[2], pcs[3], fds[0]);

    nvn_gpu_buf_release(rb_d32, NULL, NULL);
    nvn_gpu_buf_release(rb_d16, NULL, NULL);
    nvn_gpu_buf_release(rb_csml, NULL, NULL);
    nvn_gpu_buf_release(rb_dsml, NULL, NULL);
}

static void test_unsupported_rejects(void) {
    uint64_t mr0 = nvn_unimpl_count("tex", "t2t_multisample_resolve");
    uint64_t dc0 = nvn_unimpl_count("tex", "t2t_downsample_case");

    // 1. 整数 MSAA -> 1x（拒绝）
    tex_t ui_ms = make_guest_tex(14, NVN_FORMAT_R32UI, 4, 4, 4);
    tex_t ui_1x = make_guest_tex(15, NVN_FORMAT_R32UI, 4, 4, 1);
    do_downsample(&ui_ms, &ui_1x);

    // 2. 深度+模板合一 MSAA -> 1x（拒绝）
    tex_t ds_ms = make_guest_tex(16, NVN_FORMAT_DEPTH24_STENCIL8, 4, 4, 4);
    tex_t ds_1x = make_guest_tex(17, NVN_FORMAT_DEPTH24_STENCIL8, 4, 4, 1);
    do_downsample(&ds_ms, &ds_1x);

    // 3. 放大 MSAA -> 1x（4x4 -> 8x8，拒绝）
    tex_t up_ms = make_guest_tex(18, NVN_FORMAT_RGBA8, 4, 4, 4);
    tex_t up_1x = make_guest_tex(19, NVN_FORMAT_RGBA8, 8, 8, 1);
    do_downsample(&up_ms, &up_1x);

    // 4. 跨格式 MSAA -> 1x（RGBA8 4x -> BGRA8 1x，拒绝）
    tex_t xf_ms = make_guest_tex(20, NVN_FORMAT_RGBA8, 4, 4, 4);
    tex_t xf_1x = make_guest_tex(21, NVN_FORMAT_BGRA8, 4, 4, 1);
    do_downsample(&xf_ms, &xf_1x);

    // 5. 1x -> 4x 反向（拒绝）
    do_downsample(&xf_1x, &xf_ms);

    // 6. 整数 1x 换格式 Downsample（R32UI 1x -> RGBA8 1x，拒绝）
    tex_t c1x = make_guest_tex(22, NVN_FORMAT_RGBA8, 4, 4, 1);
    do_downsample(&ui_1x, &c1x);

    uint64_t mr1 = nvn_unimpl_count("tex", "t2t_multisample_resolve");
    uint64_t dc1 = nvn_unimpl_count("tex", "t2t_downsample_case");
    CHECK(mr1 - mr0 == 4, "expected 4 t2t_multisample_resolve rejects, got %llu", (unsigned long long)(mr1 - mr0));
    CHECK(dc1 - dc0 == 2, "expected 2 t2t_downsample_case rejects, got %llu", (unsigned long long)(dc1 - dc0));
    printf("DS-NEG-REJECTS ms_resolve=%llu ds_case=%llu\n",
           (unsigned long long)(mr1 - mr0), (unsigned long long)(dc1 - dc0));
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--negative")) g_skip_for_neg = 1;
    else if (argc != 1) return 64;

    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "downsample-fmt-depth"};
    if (!open_backend()) return 2;

    active = begin_cmd();
    test_1x_color_format_convert();
    test_msaa_depth_resolve();
    if (!g_skip_for_neg) test_unsupported_rejects();
    commit_and_wait();
    nvn_gpu_cmd_abort(active);
    active = NULL;

    rp_pass_reset();
    rp_res_reset();
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("downsample-fmt-depth: failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
