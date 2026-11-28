// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract & benchmark fixture for hot-path O(N) -> O(1) hash indices:
//   1) pc_pass (RenderPass cache)
//   2) lay_get (per-command image layout map)
//   3) view_find_locked (cmd_ref_texel_view + pool_live_view)
// Switch: debug.s2a.nvk_hot_hash (default 0).
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern uint64_t nvn_vk_dbg_created(int which);
extern uint64_t nvn_vk_dbg_view_stat(int which);
extern void nvn_vk_dbg_hot_hash_stat(uint64_t out[6], int reset) __attribute__((weak));

static nvn_gpu_err err;
static int g_dq_mode = 0;
static int g_hash_flag = 1;
static int g_opt_layout = 1;
static int g_check_steps = 1;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "hot-hash fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
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
    if (!strcmp(n, "nvk_hot_hash")) return g_hash_flag;
    if (!strcmp(n, "nvk_opt_layout")) return g_opt_layout;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    (void)l;
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done(void *u, const nvn_gpu_done *d) { (void)u; REQUIRE(d->status == NVN_GPU_DONE_OK); }
static void on_done_expect_fail(void *u, const nvn_gpu_done *d) { (void)u; REQUIRE(d->status != NVN_GPU_DONE_OK); }

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
    dyn.vp[0][2] = (float)w; dyn.vp[0][3] = (float)h; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1;
    dyn.sc[2] = (int32_t)w; dyn.sc[3] = (int32_t)h;
    nvn_gpu_set_dyn(cmd, &dyn);
}

static void make_2d_tex(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, nvn_gpu_tex *t, nvn_gpu_view *v) {
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = fmt;
    td.d.width = w; td.d.height = h; td.d.depth = 1; td.d.levels = 1;
    td.usage = usage;
    GPU(nvn_gpu_tex_create(&td, t, &err));
    GPU(nvn_gpu_view_create(*t, NULL, v, &err));
}

// Case 1: pc_pass O(N) vs O(1) — 72 distinct RenderPass keys (forces dynamic array + hash table resize) + 8 replay rounds (576 lookups)
static void case1_pc_pass(const char *dir, uint64_t *out_us, uint64_t *out_lookups, uint64_t *out_steps) {
    const uint32_t fmts[4] = { NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA16F, NVN_FORMAT_R8, NVN_FORMAT_RG8 };
    nvn_gpu_tex ct[4] = {0}; nvn_gpu_view cv[4] = {0};
    for (int i = 0; i < 4; i++)
        make_2d_tex(fmts[i], 4, 4, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED, &ct[i], &cv[i]);
    nvn_gpu_tex c2t = NULL; nvn_gpu_view c2v = NULL;
    make_2d_tex(NVN_FORMAT_RGBA8, 4, 4, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED, &c2t, &c2v);
    nvn_gpu_tex dt = NULL; nvn_gpu_view dv = NULL;
    make_2d_tex(NVN_FORMAT_DEPTH32F, 4, 4, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED, &dt, &dv);

    // Build 72 genuinely distinct RenderPass keys: 4 color0 formats x 3 color0 loadOps x 2 color counts (1 vs 2) x 3 depth variants
    enum { NRP = 72 };
    nvn_gpu_pass_desc descs[NRP];
    const uint8_t loads[3] = { NVN_GPU_LOAD_CLEAR, NVN_GPU_LOAD_LOAD, NVN_GPU_LOAD_DONTCARE };
    int idx = 0;
    for (int f = 0; f < 4; f++) {
        for (int cl = 0; cl < 3; cl++) {
            for (int nc = 1; nc <= 2; nc++) {
                for (int dm = 0; dm < 3; dm++) {
                    nvn_gpu_pass_desc p = {0};
                    p.ncolor = (uint32_t)nc;
                    p.color[0].view = cv[f];
                    p.color[0].load = loads[cl];
                    p.color[0].store = NVN_GPU_STORE_STORE;
                    p.color[0].clear.f[0] = 0.25f; p.color[0].clear.f[1] = 0.5f; p.color[0].clear.f[2] = 0.75f; p.color[0].clear.f[3] = 1.0f;
                    if (nc == 2) {
                        p.color[1].view = c2v;
                        p.color[1].load = NVN_GPU_LOAD_CLEAR;
                        p.color[1].store = NVN_GPU_STORE_STORE;
                    }
                    if (dm > 0) {
                        p.depth.view = dv;
                        p.depth.load = dm == 1 ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD;
                        p.depth.store = NVN_GPU_STORE_STORE;
                        p.clear_depth = 1.0f;
                    }
                    p.w = p.h = 4; p.layers = 1;
                    descs[idx++] = p;
                }
            }
        }
    }
    REQUIRE(idx == NRP);

    // Warmup: populate all 72 unique RenderPasses in cache (verify OC_RP == 72, zero false merges!)
    const uint64_t rp_before = nvn_vk_dbg_created(0);
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    for (int i = 0; i < NRP; i++) {
        nvn_gpu_pass_begin(cmd, &descs[i]);
        nvn_gpu_pass_end(cmd);
    }
    submit_and_wait(cmd);
    const uint64_t rp_after_warm = nvn_vk_dbg_created(0);
    if (rp_after_warm - rp_before != NRP) {
        fprintf(stderr, "FAIL c1-rp-unique: created=%llu want=%u\n", (unsigned long long)(rp_after_warm - rp_before), (unsigned)NRP);
        g_failures++;
    }

    // Benchmark cached lookups: 8 rounds x 72 passes = 576 hits (0 new vkCreateRenderPass)
    if (nvn_vk_dbg_hot_hash_stat) nvn_vk_dbg_hot_hash_stat(NULL, 1);
    const uint64_t t0 = mono_ns();
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    for (int r = 0; r < 8; r++) {
        for (int i = 0; i < NRP; i++) {
            nvn_gpu_pass_begin(cmd, &descs[i]);
            nvn_gpu_pass_end(cmd);
        }
    }
    const uint64_t t1 = mono_ns();
    submit_and_wait(cmd);
    const uint64_t rp_after_hit = nvn_vk_dbg_created(0);
    if (rp_after_hit != rp_after_warm) {
        fprintf(stderr, "FAIL c1-rp-miss-on-hit: extra=%llu\n", (unsigned long long)(rp_after_hit - rp_after_warm));
        g_failures++;
    }

    uint64_t st[6] = {0};
    if (nvn_vk_dbg_hot_hash_stat) nvn_vk_dbg_hot_hash_stat(st, 0);
    *out_us = (t1 - t0) / 1000ull;
    *out_lookups = st[0];
    *out_steps = st[1];
    if (g_hash_flag && g_check_steps) {
        if (!nvn_vk_dbg_hot_hash_stat || st[0] < 576 || st[1] > st[0] * 2) {
            fprintf(stderr, "FAIL c1-rp-steps: lookups=%llu steps=%llu\n", (unsigned long long)st[0], (unsigned long long)st[1]);
            g_failures++;
        }
    }

    // Pixel correctness: verify CLEAR pass vs LOAD pass + real draw produce distinct exact outputs
    nvn_gpu_layout play = { .push_bytes = 16 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "pushc.frag", NVN_GPU_SH_FS, &play);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    uint8_t zero[64] = {0};
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &rb, &err));
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    // Pass 1: CLEAR to (64, 128, 191, 255) -> STORE (hits cached pass descs[0]: f=0, cl=0, nc=1, dm=0)
    nvn_gpu_pass_begin(cmd, &descs[0]);
    nvn_gpu_pass_end(cmd);
    // Pass 2: LOAD -> draw left half green (0, 255, 0, 255) -> STORE (hits cached pass descs[6]: f=0, cl=1, nc=1, dm=0)
    nvn_gpu_pass_begin(cmd, &descs[6]);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1; dyn.vp[0][2] = 4; dyn.vp[0][3] = 4; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1; dyn.sc[0] = 0; dyn.sc[1] = 0; dyn.sc[2] = 2; dyn.sc[3] = 4;
    nvn_gpu_set_dyn(cmd, &dyn);
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, green, sizeof green);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, ct[0], 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);
    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c1-left-drawn", px + 0, 0, 255, 0, 255, 2);
    check_rgba("c1-right-loaded", px + 2 * 4, 64, 128, 191, 255, 2);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
    nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(dt);
    nvn_gpu_view_release(c2v);
    nvn_gpu_tex_release(c2t);
    for (int i = 0; i < 4; i++) { nvn_gpu_view_release(cv[i]); nvn_gpu_tex_release(ct[i]); }
}

// Case 2: lay_get O(N) vs O(1) — 128 textures in ONE command segment (forces c->lay_ht resize 64->128->256) + per-draw lay_cur lookups
static void case2_lay_get(const char *dir, uint64_t *out_us, uint64_t *out_lookups, uint64_t *out_steps) {
    enum { NTEX = 128 };
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

    nvn_gpu_tex tex[NTEX]; nvn_gpu_view view[NTEX];
    for (int i = 0; i < NTEX; i++)
        make_2d_tex(NVN_FORMAT_RGBA8, 4, 4, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED, &tex[i], &view[i]);
    nvn_gpu_tex dst = NULL; nvn_gpu_view dstv = NULL;
    make_2d_tex(NVN_FORMAT_RGBA8, 4, 4, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED, &dst, &dstv);
    samp_par sp = {0}; nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));
    uint8_t zero[64] = {0}; nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    if (nvn_vk_dbg_hot_hash_stat) nvn_vk_dbg_hot_hash_stat(NULL, 1);
    const uint64_t t0 = mono_ns();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    // Populate all 128 textures into c->lay via nvn_gpu_clear_view (transitions to TRANSFER_DST -> GENERAL in c->lay)
    for (int i = 0; i < NTEX; i++) {
        const float col[4] = { (i == NTEX - 1) ? 0.25f : 0.0f, (i == NTEX - 1) ? 0.5f : 0.25f, 0.25f, 1.0f };
        nvn_gpu_clear_view(cmd, view[i], col, NULL, 0);
    }
    // Single pass sampling across all 128 textures x 4 rounds = 512 draws (each draw calls lay_cur in prepare_bind_layouts + bind_image_layout!)
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = dstv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < NTEX; i++) {
            nvn_gpu_binding b[2] = {{0}};
            b[0].ent = 0; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = view[i];
            b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = samp;
            nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
            nvn_gpu_draw(cmd, &da);
        }
    }
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, dst, 0, 0, NULL, rb, 0, 16, 64);
    const uint64_t t1 = mono_ns();
    submit_and_wait(cmd);

    // feedback.frag outputs vec4(sample.r + 0.25, sample.g, sample.b, 1.0) -> last tex (0.25, 0.5, 0.25) -> (128, 128, 64, 255)
    check_rgba("c2-lay-pixel", nvn_gpu_buf_cpu(rb), 128, 128, 64, 255, 2);

    uint64_t st[6] = {0};
    if (nvn_vk_dbg_hot_hash_stat) nvn_vk_dbg_hot_hash_stat(st, 0);
    *out_us = (t1 - t0) / 1000ull;
    *out_lookups = st[2];
    *out_steps = st[3];
    if (g_hash_flag && g_check_steps) {
        if (!nvn_vk_dbg_hot_hash_stat || st[2] < 512 || st[3] > st[2] * 2) {
            fprintf(stderr, "FAIL c2-lay-steps: lookups=%llu steps=%llu\n", (unsigned long long)st[2], (unsigned long long)st[3]);
            g_failures++;
        }
    }

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(dstv);
    nvn_gpu_tex_release(dst);
    for (int i = 0; i < NTEX; i++) { nvn_gpu_view_release(view[i]); nvn_gpu_tex_release(tex[i]); }
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 3: view_find_locked (pool_live_view + cmd_ref_texel_view) — 2048 live views (resize 1024->2048->4096), unlink 1024, reject stale/bogus handles
static void case3_live_views(const char *dir, uint64_t *out_us, uint64_t *out_lookups, uint64_t *out_steps) {
    enum { NVIEWS = 2048, NPOOL = 256, NTBUF = 8 };
    nvn_gpu_tex base_tex = NULL; nvn_gpu_view base_view = NULL;
    make_2d_tex(NVN_FORMAT_RGBA8, 4, 4, NVN_GPU_TEX_SAMPLED, &base_tex, &base_view);

    // Create 2048 live views on base_tex (forces g.view_ht dynamic resize 1024 -> 2048 -> 4096)
    nvn_gpu_view *views = calloc(NVIEWS, sizeof *views);
    REQUIRE(views);
    for (int i = 0; i < NVIEWS; i++) GPU(nvn_gpu_view_create(base_tex, NULL, &views[i], &err));

    // Create 8 texel buffer views (R32UI)
    uint32_t tdata[NTBUF][4];
    nvn_gpu_buf tbufs[NTBUF] = {0};
    nvn_gpu_view tviews[NTBUF] = {0};
    for (int i = 0; i < NTBUF; i++) {
        for (int k = 0; k < 4; k++) tdata[i][k] = (uint32_t)((i + 1) * 100 + k);
        GPU(nvn_gpu_buf_create(sizeof tdata[i], NVN_GPU_MEM_SHARED, tdata[i], &tbufs[i], &err));
        GPU(nvn_gpu_view_create_buf(tbufs[i], 0, sizeof tdata[i], NVN_FORMAT_R32UI, &tviews[i], &err));
    }

    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_TBUF; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].set = 0; ent[1].binding = 1; ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader cs = load_shader(dir, "tbuf.comp", NVN_GPU_SH_CS, &lay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    uint32_t zero[4] = {0};
    nvn_gpu_buf outb = NULL;
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &outb, &err));

    // Benchmark pool_live_view (4 x 256 = 1024 lookups) + cmd_ref_texel_view (64 binds across 8 tviews)
    nvn_gpu_pool_item items[NPOOL];
    memset(items, 0, sizeof items);
    for (int i = 0; i < NPOOL; i++) {
        items[i].state = NVN_GPU_POOL_READY;
        items[i].resource.view = views[i * (NVIEWS / NPOOL)];
    }
    nvn_gpu_pool_desc pdesc = { .kind = NVN_GPU_POOL_IMAGE, .count = NPOOL, .items = items };
    pdesc.dummy.view = views[0];

    if (nvn_vk_dbg_hot_hash_stat) nvn_vk_dbg_hot_hash_stat(NULL, 1);
    const uint64_t t0 = mono_ns();
    for (int r = 0; r < 4; r++) {
        nvn_gpu_pool_table pt = NULL;
        GPU(nvn_gpu_pool_create(&pdesc, &pt, &err));
        nvn_gpu_pool_release(pt);
    }
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, cpipe);
    for (int r = 0; r < 64; r++) {
        nvn_gpu_binding b[2] = {{0}};
        b[0].ent = 0; b[0].cls = NVN_GPU_RES_TBUF; b[0].u.view = tviews[r % NTBUF];
        b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = outb; b[1].u.b.off = 0; b[1].u.b.len = sizeof zero;
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, b, 2);
    }
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    const uint64_t t1 = mono_ns();
    submit_and_wait(cmd);

    // Last bound tview was r=63 -> 63 % 8 = 7 -> (7+1)*100 + k = 800..803
    const uint32_t *res = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(outb);
    for (uint32_t k = 0; k < 4; k++) {
        if (res[k] != 800u + k) {
            fprintf(stderr, "FAIL c3-tbuf[%u]: got=%u want=%u\n", k, res[k], 800u + k);
            g_failures++;
        }
    }

    uint64_t st[6] = {0};
    if (nvn_vk_dbg_hot_hash_stat) nvn_vk_dbg_hot_hash_stat(st, 0);
    *out_us = (t1 - t0) / 1000ull;
    *out_lookups = st[4];
    *out_steps = st[5];
    if (g_hash_flag && g_check_steps) {
        if (!nvn_vk_dbg_hot_hash_stat || st[4] < 1024 || st[5] > st[4] * 3) {
            fprintf(stderr, "FAIL c3-view-steps: lookups=%llu steps=%llu\n", (unsigned long long)st[4], (unsigned long long)st[5]);
            g_failures++;
        }
    }

    // Unlink 1024 views (odd indices), check list integrity + verify remaining views & reject stale/bogus handles
    nvn_gpu_view stale_view = views[1];
    nvn_gpu_view stale_tview = tviews[0];
    nvn_gpu_view_release(tviews[0]);
    tviews[0] = NULL;
    for (int i = 1; i < NVIEWS; i += 2) { nvn_gpu_view_release(views[i]); views[i] = NULL; }
    if (nvn_vk_dbg_view_stat(4) != 0) {
        fprintf(stderr, "FAIL c3-view-list-integrity: bad=%llu\n", (unsigned long long)nvn_vk_dbg_view_stat(4));
        g_failures++;
    }

    // Even views must still be found by pool_live_view
    nvn_gpu_pool_item it2 = { .state = NVN_GPU_POOL_READY };
    it2.resource.view = views[NVIEWS - 2];
    nvn_gpu_pool_desc pd2 = { .kind = NVN_GPU_POOL_IMAGE, .count = 1, .items = &it2 };
    pd2.dummy.view = views[0];
    nvn_gpu_pool_table pt2 = NULL;
    GPU(nvn_gpu_pool_create(&pd2, &pt2, &err));
    nvn_gpu_pool_release(pt2);

    // Released/unlinked view & bogus buffer pointer must be rejected by pool_live_view (without poisoning timeline!)
    nvn_gpu_err pe = {0};
    it2.resource.view = stale_view;
    nvn_gpu_rc r_pool_stale = nvn_gpu_pool_create(&pd2, &pt2, &pe);
    if (nvn_gpu_ok(r_pool_stale)) {
        fprintf(stderr, "FAIL c3-pool-stale-reject: unexpectedly succeeded\n");
        nvn_gpu_pool_release(pt2);
        g_failures++;
    }
    it2.resource.view = (nvn_gpu_view)(void *)outb;
    nvn_gpu_rc r_pool_bogus = nvn_gpu_pool_create(&pd2, &pt2, &pe);
    if (nvn_gpu_ok(r_pool_bogus)) {
        fprintf(stderr, "FAIL c3-pool-bogus-reject: unexpectedly succeeded\n");
        nvn_gpu_pool_release(pt2);
        g_failures++;
    }

    // Released/stale tview must be cleanly rejected by cmd_ref_texel_view (terminal sticky error, last cmd in session)
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_binding bad_b = {0};
    bad_b.ent = 0; bad_b.cls = NVN_GPU_RES_TBUF; bad_b.u.view = stale_tview;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &bad_b, 1);
    nvn_gpu_err ce = {0};
    nvn_gpu_rc r_stale = nvn_gpu_cmd_commit(cmd, ++g_sig, on_done_expect_fail, NULL, &ce);
    if (r_stale.code != NVN_GPU_E_UNSUPPORTED) {
        fprintf(stderr, "FAIL c3-stale-view-reject: rc=%d want=%d\n", r_stale.code, NVN_GPU_E_UNSUPPORTED);
        g_failures++;
    }

    for (int i = 0; i < NVIEWS; i++) if (views[i]) nvn_gpu_view_release(views[i]);
    free(views);
    for (int i = 0; i < NTBUF; i++) {
        if (tviews[i]) nvn_gpu_view_release(tviews[i]);
        if (tbufs[i]) nvn_gpu_buf_release(tbufs[i], NULL, NULL);
    }
    nvn_gpu_buf_release(outb, NULL, NULL);
    nvn_gpu_shader_release(cs);
    nvn_gpu_view_release(base_view);
    nvn_gpu_tex_release(base_tex);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 4);
    const char *dir = argv[1];
    g_dq_mode = atoi(argv[2]);
    g_hash_flag = atoi(argv[3]);
    g_check_steps = argc >= 5 ? atoi(argv[4]) : 1;

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = { .host_device = &dev };
    GPU(nvn_gpu_open(&od, &err));

    uint64_t rp_us = 0, rp_l = 0, rp_s = 0;
    uint64_t lay_us = 0, lay_l = 0, lay_s = 0;
    uint64_t vw_us = 0, vw_l = 0, vw_s = 0;
    case1_pc_pass(dir, &rp_us, &rp_l, &rp_s);
    case2_lay_get(dir, &lay_us, &lay_l, &lay_s);
    case3_live_views(dir, &vw_us, &vw_l, &vw_s);

    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);

    printf("hot-hash dq=%d flag=%d chk=%d rp_us=%llu rp=%llu/%llu lay_us=%llu lay=%llu/%llu view_us=%llu view=%llu/%llu cases=3 failures=%u\n",
           g_dq_mode, g_hash_flag, g_check_steps,
           (unsigned long long)rp_us, (unsigned long long)rp_l, (unsigned long long)rp_s,
           (unsigned long long)lay_us, (unsigned long long)lay_l, (unsigned long long)lay_s,
           (unsigned long long)vw_us, (unsigned long long)vw_l, (unsigned long long)vw_s,
           g_failures);
    return g_failures ? 1 : 0;
}
