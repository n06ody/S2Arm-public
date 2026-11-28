// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract fixture for widescreen host dimensions (nvn_gpu_tex_desc.host_w / host_h).
// Runs on real Lavapipe + Khronos VVL + SyncVal under both nvk_dq={0,1} and nvk_opt_layout={0,1}.
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err g_err;
static int g_dq_mode;
static int g_opt_mode;
static uint64_t g_fence_seq;

#define REQUIRE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL line %d: %s (rc=%d why=%s)\n", __LINE__, #x, g_err.rc.code, g_err.why); \
        exit(1); \
    } \
} while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_mode;
    if (!strcmp(n, "nvk_opt_layout")) return g_opt_mode;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    (void)l;
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done(void *u, const nvn_gpu_done *d) {
    (void)u;
    REQUIRE(d->status == NVN_GPU_DONE_OK);
}

static void on_done_expect_arg(void *u, const nvn_gpu_done *d) {
    REQUIRE(d->status == NVN_GPU_DONE_FAILED && d->rc.code == NVN_GPU_E_ARG);
    if (u) *(int *)u = 1;
}

static nvn_gpu_shader load_shader(const char *dir, const char *name, nvn_gpu_stage stage) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb");
    REQUIRE(f != NULL);
    REQUIRE(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    REQUIRE(n > 0);
    rewind(f);
    void *data = malloc((size_t)n);
    REQUIRE(data != NULL);
    REQUIRE(fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    nvn_gpu_shader_src s = {0};
    s.stage = stage;
    s.kind = NVN_GPU_BLOB_SPIRV;
    s.entry = "main";
    s.blob = data;
    s.bytes = (size_t)n;
    nvn_gpu_shader sh = NULL;
    GPU(nvn_gpu_shader_load(&s, &sh, &g_err));
    free(data);
    return sh;
}

static nvn_gpu_pipe make_solid_pipe(nvn_gpu_shader vs, nvn_gpu_shader fs, uint32_t dfmt) {
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.chmask.mask = 0x0f;
    pd.prim = 4;
    pd.samples = 1;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.dfmt = dfmt;
    nvn_gpu_pipe p = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &g_err));
    return p;
}

static void submit_and_wait(nvn_gpu_cmd cmd) {
    const uint64_t seq = ++g_fence_seq;
    GPU(nvn_gpu_cmd_commit(cmd, seq, on_done, NULL, &g_err));
    GPU(nvn_gpu_fence_wait(seq, 5000000000ull, &g_err));
}

static void expect_rgba(const uint8_t *px, uint32_t w, uint32_t x, uint32_t y,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t *p = px + ((size_t)y * w + x) * 4u;
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        fprintf(stderr, "FAIL pixel (%u,%u): got (%u,%u,%u,%u) want (%u,%u,%u,%u)\n",
                x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        exit(1);
    }
}

/* Case 1: Widen X only (guest 16x8, host_w=24, host_h=0) — render guest left GREEN,
 * widened right strip [16..24) BLUE, read back full 24x8 and verify every pixel. */
static void test_widen_x_render(nvn_gpu_pipe p_green, nvn_gpu_pipe p_blue) {
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 16;
    td.d.height = 8;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    td.host_w = 24;
    td.host_h = 0;

    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &g_err));

    nvn_gpu_tex_info info = {0};
    nvn_gpu_tex_info_get(tex, &info);
    REQUIRE(info.w == 24 && info.h == 8 && info.levels == 1 && info.layers == 1);
    REQUIRE(info.nvn_format == NVN_FORMAT_RGBA8);

    nvn_gpu_view view = NULL;
    GPU(nvn_gpu_view_create(tex, NULL, &view, &g_err));

    const uint32_t rb_bytes = 24u * 8u * 4u;
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(rb_bytes, NVN_GPU_MEM_SHARED, NULL, &rb, &g_err));
    memset(nvn_gpu_buf_cpu(rb), 0xaa, rb_bytes);

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));

    nvn_gpu_pass_desc pass = {0};
    pass.ncolor = 1;
    pass.color[0].view = view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = 1.0f;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 24;
    pass.h = 8;
    pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass);

    nvn_gpu_draw_args da = {0};
    da.prim = 4;
    da.count = 3;
    da.instances = 1;

    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1;
    dyn.vp[0][0] = 0.0f; dyn.vp[0][1] = 0.0f; dyn.vp[0][2] = 16.0f; dyn.vp[0][3] = 8.0f; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1;
    dyn.sc[0] = 0; dyn.sc[1] = 0; dyn.sc[2] = 16; dyn.sc[3] = 8;
    nvn_gpu_set_pipe(cmd, p_green);
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_draw(cmd, &da);

    dyn.vp[0][0] = 16.0f; dyn.vp[0][1] = 0.0f; dyn.vp[0][2] = 8.0f; dyn.vp[0][3] = 8.0f;
    dyn.sc[0] = 16; dyn.sc[1] = 0; dyn.sc[2] = 8; dyn.sc[3] = 8;
    nvn_gpu_set_pipe(cmd, p_blue);
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_draw(cmd, &da);

    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 24u * 4u, rb_bytes);
    submit_and_wait(cmd);

    const uint8_t *px = (const uint8_t *)nvn_gpu_buf_cpu(rb);
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 16; x++) expect_rgba(px, 24, x, y, 0, 255, 0, 255);
        for (uint32_t x = 16; x < 24; x++) expect_rgba(px, 24, x, y, 0, 0, 255, 255);
    }

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
}

/* Case 2: Tallen Y only (host_w=0, host_h=12) + both axes with 2 mips (24x12),
 * tex_upload sub-regions, copy_tex across widened mips, and scaled blit to unwidened 16x8. */
static void test_tallen_mip_copy_blit(nvn_gpu_pipe p_blue) {
    /* 2a: Tallen Y only */
    {
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D;
        td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = 16;
        td.d.height = 8;
        td.d.depth = 1;
        td.d.levels = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
        td.host_w = 0;
        td.host_h = 12;

        nvn_gpu_tex tex = NULL;
        GPU(nvn_gpu_tex_create(&td, &tex, &g_err));
        nvn_gpu_tex_info info = {0};
        nvn_gpu_tex_info_get(tex, &info);
        REQUIRE(info.w == 16 && info.h == 12);

        nvn_gpu_view view = NULL;
        GPU(nvn_gpu_view_create(tex, NULL, &view, &g_err));
        const uint32_t rb_bytes = 16u * 12u * 4u;
        nvn_gpu_buf rb = NULL;
        GPU(nvn_gpu_buf_create(rb_bytes, NVN_GPU_MEM_SHARED, NULL, &rb, &g_err));

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
        nvn_gpu_pass_desc pass = {0};
        pass.ncolor = 1;
        pass.color[0].view = view;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[0] = 1.0f;
        pass.color[0].clear.f[3] = 1.0f;
        pass.w = 16;
        pass.h = 12;
        pass.layers = 1;
        nvn_gpu_pass_begin(cmd, &pass);

        nvn_gpu_dyn dyn = {0};
        dyn.nvp = 1;
        dyn.vp[0][0] = 0.0f; dyn.vp[0][1] = 8.0f; dyn.vp[0][2] = 16.0f; dyn.vp[0][3] = 4.0f; dyn.vp[0][5] = 1.0f;
        dyn.has_sc = 1;
        dyn.sc[0] = 0; dyn.sc[1] = 8; dyn.sc[2] = 16; dyn.sc[3] = 4;
        nvn_gpu_set_pipe(cmd, p_blue);
        nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_draw_args da = {0};
        da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);

        nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16u * 4u, rb_bytes);
        submit_and_wait(cmd);

        const uint8_t *px = (const uint8_t *)nvn_gpu_buf_cpu(rb);
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 16; x++) expect_rgba(px, 16, x, y, 255, 0, 0, 255);
        for (uint32_t y = 8; y < 12; y++)
            for (uint32_t x = 0; x < 16; x++) expect_rgba(px, 16, x, y, 0, 0, 255, 255);

        nvn_gpu_buf_release(rb, NULL, NULL);
        nvn_gpu_view_release(view);
        nvn_gpu_tex_release(tex);
    }

    /* 2b: Both axes 16x8 -> 24x12 with levels=2, sub-region uploads, copy_tex, and blit -> 16x8 */
    {
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D;
        td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = 16;
        td.d.height = 8;
        td.d.depth = 1;
        td.d.levels = 2;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
        td.host_w = 24;
        td.host_h = 12;

        nvn_gpu_tex src = NULL, dst = NULL, small = NULL;
        GPU(nvn_gpu_tex_create(&td, &src, &g_err));
        GPU(nvn_gpu_tex_create(&td, &dst, &g_err));

        nvn_gpu_tex_info info = {0};
        nvn_gpu_tex_info_get(src, &info);
        REQUIRE(info.w == 24 && info.h == 12 && info.levels == 2);

        nvn_gpu_tex_desc sd = td;
        sd.d.levels = 1;
        sd.host_w = 0;
        sd.host_h = 0;
        GPU(nvn_gpu_tex_create(&sd, &small, &g_err));

        view_t v0 = {0};
        v0.set_levels = 1; v0.base_level = 0; v0.nlevels = 1;
        nvn_gpu_view v_dst = NULL, v_small = NULL;
        GPU(nvn_gpu_view_create(dst, &v0, &v_dst, &g_err));
        GPU(nvn_gpu_view_create(small, &v0, &v_small, &g_err));

        uint8_t green_16x8[16u * 8u * 4u];
        uint8_t blue_8x12[8u * 12u * 4u];
        uint8_t red_16x4[16u * 4u * 4u];
        uint8_t yellow_12x6[12u * 6u * 4u];
        for (size_t i = 0; i < sizeof green_16x8; i += 4) { green_16x8[i]=0; green_16x8[i+1]=255; green_16x8[i+2]=0; green_16x8[i+3]=255; }
        for (size_t i = 0; i < sizeof blue_8x12; i += 4) { blue_8x12[i]=0; blue_8x12[i+1]=0; blue_8x12[i+2]=255; blue_8x12[i+3]=255; }
        for (size_t i = 0; i < sizeof red_16x4; i += 4) { red_16x4[i]=255; red_16x4[i+1]=0; red_16x4[i+2]=0; red_16x4[i+3]=255; }
        for (size_t i = 0; i < sizeof yellow_12x6; i += 4) { yellow_12x6[i]=255; yellow_12x6[i+1]=255; yellow_12x6[i+2]=0; yellow_12x6[i+3]=255; }

        nvn_gpu_buf rb0 = NULL, rb1 = NULL, rbs = NULL;
        GPU(nvn_gpu_buf_create(24u * 12u * 4u, NVN_GPU_MEM_SHARED, NULL, &rb0, &g_err));
        GPU(nvn_gpu_buf_create(12u * 6u * 4u, NVN_GPU_MEM_SHARED, NULL, &rb1, &g_err));
        GPU(nvn_gpu_buf_create(16u * 8u * 4u, NVN_GPU_MEM_SHARED, NULL, &rbs, &g_err));

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));

        NVNcopyRegion r_tl = {0, 0, 0, 16, 8, 1};
        NVNcopyRegion r_rt = {16, 0, 0, 8, 12, 1};
        NVNcopyRegion r_bl = {0, 8, 0, 16, 4, 1};
        nvn_gpu_tex_upload(cmd, src, 0, 0, &r_tl, green_16x8, 16u * 4u, sizeof green_16x8);
        nvn_gpu_tex_upload(cmd, src, 0, 0, &r_rt, blue_8x12, 8u * 4u, sizeof blue_8x12);
        nvn_gpu_tex_upload(cmd, src, 0, 0, &r_bl, red_16x4, 16u * 4u, sizeof red_16x4);
        nvn_gpu_tex_upload(cmd, src, 1, 0, NULL, yellow_12x6, 12u * 4u, sizeof yellow_12x6);

        NVNcopyRegion full0 = {0, 0, 0, 24, 12, 1};
        NVNcopyRegion full1 = {0, 0, 0, 12, 6, 1};
        NVNcopyRegion dst_s = {0, 0, 0, 16, 8, 1};
        nvn_gpu_copy_tex(cmd, src, 0, 0, &full0, dst, 0, 0, &full0);
        nvn_gpu_copy_tex(cmd, src, 1, 0, &full1, dst, 1, 0, &full1);
        nvn_gpu_blit(cmd, v_dst, &full0, v_small, &dst_s, 0);

        nvn_gpu_copy_tex_buf(cmd, dst, 0, 0, NULL, rb0, 0, 24u * 4u, 24u * 12u * 4u);
        nvn_gpu_copy_tex_buf(cmd, dst, 1, 0, NULL, rb1, 0, 12u * 4u, 12u * 6u * 4u);
        nvn_gpu_copy_tex_buf(cmd, small, 0, 0, NULL, rbs, 0, 16u * 4u, 16u * 8u * 4u);
        submit_and_wait(cmd);

        const uint8_t *p0 = (const uint8_t *)nvn_gpu_buf_cpu(rb0);
        for (uint32_t y = 0; y < 12; y++) {
            for (uint32_t x = 0; x < 24; x++) {
                if (x >= 16) expect_rgba(p0, 24, x, y, 0, 0, 255, 255);
                else if (y >= 8) expect_rgba(p0, 24, x, y, 255, 0, 0, 255);
                else expect_rgba(p0, 24, x, y, 0, 255, 0, 255);
            }
        }
        const uint8_t *p1 = (const uint8_t *)nvn_gpu_buf_cpu(rb1);
        for (uint32_t y = 0; y < 6; y++)
            for (uint32_t x = 0; x < 12; x++) expect_rgba(p1, 12, x, y, 255, 255, 0, 255);

        const uint8_t *ps = (const uint8_t *)nvn_gpu_buf_cpu(rbs);
        expect_rgba(ps, 16, 2, 2, 0, 255, 0, 255);
        expect_rgba(ps, 16, 14, 2, 0, 0, 255, 255);
        expect_rgba(ps, 16, 2, 6, 255, 0, 0, 255);

        nvn_gpu_buf_release(rbs, NULL, NULL);
        nvn_gpu_buf_release(rb1, NULL, NULL);
        nvn_gpu_buf_release(rb0, NULL, NULL);
        nvn_gpu_view_release(v_small);
        nvn_gpu_view_release(v_dst);
        nvn_gpu_tex_release(small);
        nvn_gpu_tex_release(dst);
        nvn_gpu_tex_release(src);
    }
}

/* Case 3: Widened color + matching widened depth + clear_rect in widened strip +
 * 6-layer non-square widened 2D array + pass_begin extent mismatch guard. */
static void test_depth_clear_array_and_pass_guard(void) {
    nvn_gpu_tex_desc cd = {0};
    cd.d.target = NVN_TEXTURE_TARGET_2D;
    cd.d.format = NVN_FORMAT_RGBA8;
    cd.d.width = 16;
    cd.d.height = 8;
    cd.d.depth = 1;
    cd.d.levels = 1;
    cd.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    cd.host_w = 24;
    cd.host_h = 8;

    nvn_gpu_tex_desc dd = cd;
    dd.d.format = NVN_FORMAT_DEPTH24_STENCIL8;

    nvn_gpu_tex ctex = NULL, dtex = NULL;
    GPU(nvn_gpu_tex_create(&cd, &ctex, &g_err));
    GPU(nvn_gpu_tex_create(&dd, &dtex, &g_err));

    nvn_gpu_tex_info dinfo = {0};
    nvn_gpu_tex_info_get(dtex, &dinfo);
    REQUIRE(dinfo.w == 24 && dinfo.h == 8);

    nvn_gpu_view cview = NULL, dview = NULL;
    GPU(nvn_gpu_view_create(ctex, NULL, &cview, &g_err));
    GPU(nvn_gpu_view_create(dtex, NULL, &dview, &g_err));

    const uint32_t rb_bytes = 24u * 8u * 4u;
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(rb_bytes, NVN_GPU_MEM_SHARED, NULL, &rb, &g_err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
    nvn_gpu_pass_desc pass = {0};
    pass.ncolor = 1;
    pass.color[0].view = cview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = 1.0f;
    pass.color[0].clear.f[3] = 1.0f;
    pass.depth.view = dview;
    pass.depth.load = NVN_GPU_LOAD_CLEAR;
    pass.depth.store = NVN_GPU_STORE_STORE;
    pass.clear_depth = 1.0f;
    pass.w = 24;
    pass.h = 8;
    pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass);

    nvn_gpu_clear clr = {0};
    clr.color = 0;
    clr.color_mask = 0x0f;
    clr.value.f[1] = 1.0f;
    clr.value.f[3] = 1.0f;
    clr.depth = 1;
    clr.depth_value = 0.5f;
    clr.x = 16; clr.y = 0; clr.w = 8; clr.h = 8;
    nvn_gpu_clear_rect(cmd, &clr);
    nvn_gpu_pass_end(cmd);

    nvn_gpu_copy_tex_buf(cmd, ctex, 0, 0, NULL, rb, 0, 24u * 4u, rb_bytes);
    submit_and_wait(cmd);

    const uint8_t *px = (const uint8_t *)nvn_gpu_buf_cpu(rb);
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 16; x++) expect_rgba(px, 24, x, y, 255, 0, 0, 255);
        for (uint32_t x = 16; x < 24; x++) expect_rgba(px, 24, x, y, 0, 255, 0, 255);
    }

    /* 6-layer 2D array widened from square 16x16 -> non-square 24x16 must not set CUBE_COMPATIBLE */
    nvn_gpu_tex_desc ad = {0};
    ad.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    ad.d.format = NVN_FORMAT_RGBA8;
    ad.d.width = 16;
    ad.d.height = 16;
    ad.d.depth = 6;
    ad.d.levels = 1;
    ad.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    ad.host_w = 24;
    ad.host_h = 16;
    nvn_gpu_tex atex = NULL;
    GPU(nvn_gpu_tex_create(&ad, &atex, &g_err));
    nvn_gpu_tex_info ainfo = {0};
    nvn_gpu_tex_info_get(atex, &ainfo);
    REQUIRE(ainfo.w == 24 && ainfo.h == 16 && ainfo.layers == 6);
    nvn_gpu_tex_release(atex);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(dview);
    nvn_gpu_view_release(cview);
    nvn_gpu_tex_release(dtex);
    nvn_gpu_tex_release(ctex);
}

/* Unwidened 16x8 attachment paired with pass 24x8 must fail cleanly with NVN_GPU_E_ARG (0 VVL).
 * Run at end of session because committing a sticky-error command fails the session timeline. */
static void test_pass_extent_mismatch_guard(int *got_arg_cb) {
    nvn_gpu_tex_desc ud = {0};
    ud.d.target = NVN_TEXTURE_TARGET_2D;
    ud.d.format = NVN_FORMAT_RGBA8;
    ud.d.width = 16;
    ud.d.height = 8;
    ud.d.depth = 1;
    ud.d.levels = 1;
    ud.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

    nvn_gpu_tex utex = NULL;
    GPU(nvn_gpu_tex_create(&ud, &utex, &g_err));
    nvn_gpu_view uview = NULL;
    GPU(nvn_gpu_view_create(utex, NULL, &uview, &g_err));

    nvn_gpu_cmd bad = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &bad, &g_err));
    nvn_gpu_pass_desc bpass = {0};
    bpass.ncolor = 1;
    bpass.color[0].view = uview;
    bpass.color[0].load = NVN_GPU_LOAD_CLEAR;
    bpass.color[0].store = NVN_GPU_STORE_STORE;
    bpass.w = 24;
    bpass.h = 8;
    bpass.layers = 1;
    nvn_gpu_pass_begin(bad, &bpass);
    nvn_gpu_rc commit_rc = nvn_gpu_cmd_commit(bad, ++g_fence_seq, on_done_expect_arg, got_arg_cb, &g_err);
    REQUIRE(commit_rc.code == NVN_GPU_E_ARG);

    nvn_gpu_view_release(uview);
    nvn_gpu_tex_release(utex);
}

/* Case 4: Boundary and invalid host_w/host_h rejections + explicit same-size no-ops. */
static void test_boundaries(void) {
    nvn_gpu_caps caps = {0};
    nvn_gpu_caps_get(&caps);
    REQUIRE(caps.max_tex_dim > 0);

    nvn_gpu_tex_desc base = {0};
    base.d.target = NVN_TEXTURE_TARGET_2D;
    base.d.format = NVN_FORMAT_RGBA8;
    base.d.width = 16;
    base.d.height = 8;
    base.d.depth = 1;
    base.d.levels = 1;
    base.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

    nvn_gpu_tex t = NULL;
    nvn_gpu_tex_desc d;

    /* Shrink X -> NVN_GPU_E_ARG */
    d = base; d.host_w = 8; d.host_h = 0;
    REQUIRE(nvn_gpu_tex_create(&d, &t, &g_err).code == NVN_GPU_E_ARG && t == NULL);

    /* Shrink Y -> NVN_GPU_E_ARG */
    d = base; d.host_w = 0; d.host_h = 4;
    REQUIRE(nvn_gpu_tex_create(&d, &t, &g_err).code == NVN_GPU_E_ARG && t == NULL);

    /* 3D widened -> NVN_GPU_E_UNSUPPORTED */
    d = base; d.d.target = NVN_TEXTURE_TARGET_3D; d.d.depth = 4; d.usage = NVN_GPU_TEX_SAMPLED;
    d.host_w = 24; d.host_h = 8;
    REQUIRE(nvn_gpu_tex_create(&d, &t, &g_err).code == NVN_GPU_E_UNSUPPORTED && t == NULL);

    /* 3D explicit same-size -> OK */
    d.host_w = 16; d.host_h = 8;
    GPU(nvn_gpu_tex_create(&d, &t, &g_err));
    nvn_gpu_tex_info info = {0};
    nvn_gpu_tex_info_get(t, &info);
    REQUIRE(info.w == 16 && info.h == 8 && info.depth == 4);
    nvn_gpu_tex_release(t); t = NULL;

    /* Non-square cubemap -> NVN_GPU_E_UNSUPPORTED */
    d = base; d.d.target = NVN_TEXTURE_TARGET_CUBEMAP; d.d.width = 16; d.d.height = 16;
    d.host_w = 24; d.host_h = 16;
    REQUIRE(nvn_gpu_tex_create(&d, &t, &g_err).code == NVN_GPU_E_UNSUPPORTED && t == NULL);

    /* Square widened cubemap -> OK */
    d.host_w = 24; d.host_h = 24;
    GPU(nvn_gpu_tex_create(&d, &t, &g_err));
    nvn_gpu_tex_info_get(t, &info);
    REQUIRE(info.w == 24 && info.h == 24 && info.layers == 6);
    nvn_gpu_tex_release(t); t = NULL;

    /* Compressed BC1 widened -> NVN_GPU_E_UNSUPPORTED */
    d = base; d.d.format = NVN_FORMAT_RGBA_DXT1; d.d.width = 16; d.d.height = 16; d.usage = NVN_GPU_TEX_SAMPLED;
    d.host_w = 24; d.host_h = 16;
    REQUIRE(nvn_gpu_tex_create(&d, &t, &g_err).code == NVN_GPU_E_UNSUPPORTED && t == NULL);

    /* Compressed BC1 explicit same-size -> OK */
    d.host_w = 16; d.host_h = 16;
    GPU(nvn_gpu_tex_create(&d, &t, &g_err));
    nvn_gpu_tex_info_get(t, &info);
    REQUIRE(info.w == 16 && info.h == 16);
    nvn_gpu_tex_release(t); t = NULL;

    /* Over device max_tex_dim -> NVN_GPU_E_UNSUPPORTED */
    d = base; d.host_w = caps.max_tex_dim + 1u; d.host_h = 0;
    REQUIRE(nvn_gpu_tex_create(&d, &t, &g_err).code == NVN_GPU_E_UNSUPPORTED && t == NULL);

    /* Excess mip levels for widened extent -> NVN_GPU_E_ARG */
    d = base; d.d.levels = 10; d.host_w = 24; d.host_h = 12;
    REQUIRE(nvn_gpu_tex_create(&d, &t, &g_err).code == NVN_GPU_E_ARG && t == NULL);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <shader_dir> <dq:0|1> <opt:0|1>\n", argv[0]);
        return 2;
    }
    const char *shader_dir = argv[1];
    g_dq_mode = atoi(argv[2]);
    g_opt_mode = atoi(argv[3]);

    struct s2a_vk_device dev;
    memset(&dev, 0, sizeof dev);
    char dmsg[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, dmsg, sizeof dmsg) == 0);

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "widescreen-host-size";
    GPU(nvn_gpu_open(&od, &g_err));

    nvn_gpu_shader vs = load_shader(shader_dir, "fullscreen.vert", NVN_GPU_SH_VS);
    nvn_gpu_shader fs_g = load_shader(shader_dir, "solid_green.frag", NVN_GPU_SH_FS);
    nvn_gpu_shader fs_b = load_shader(shader_dir, "solid_blue.frag", NVN_GPU_SH_FS);
    nvn_gpu_pipe p_green = make_solid_pipe(vs, fs_g, 0);
    nvn_gpu_pipe p_blue = make_solid_pipe(vs, fs_b, 0);

    int got_arg_cb = 0;
    test_widen_x_render(p_green, p_blue);
    test_tallen_mip_copy_blit(p_blue);
    test_depth_clear_array_and_pass_guard();
    test_boundaries();
    test_pass_extent_mismatch_guard(&got_arg_cb);

    nvn_gpu_shader_release(fs_b);
    nvn_gpu_shader_release(fs_g);
    nvn_gpu_shader_release(vs);
    GPU(nvn_gpu_close(5000000000ull, &g_err));
    REQUIRE(got_arg_cb == 1);
    s2a_vk_device_destroy(&dev, 1);

    printf("widescreen-host-size dq=%d opt=%d cases=4 failures=0\n", g_dq_mode, g_opt_mode);
    return 0;
}
