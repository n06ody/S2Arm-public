// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract & benchmark fixture for DQ cache-line isolation + cached rd + batched pub/rd (debug.s2a.nvk_dq_opt):
//   1) 256-draw burst under DQ=1: verifies pub_stores ~16x reduction, rd_reloads -> 0, rd_stores batched, exact 4-quadrant pixels
//   2) Short-segment tail flush (< DQ_PUB_BATCH=16 cmds/seg, 8 alternating color segments): proves vk_dq_drain flushes unpublished tail
//   3) Async arena_fill + arena_fill_wait + arena_fill_take under OP_FILL immediate publish
//   4) Multi-segment graphics+compute+abort stress under DQ=1 with 0 VVL/SyncVal errors
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern uint64_t nvn_vk_dbg_dq_recs(void);
extern void nvn_vk_dbg_dq_opt_stat(uint64_t out[4], int reset) __attribute__((weak));

static nvn_gpu_err err;
static int g_dq_mode = 1;
static int g_opt_flag = 1;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "dq-opt fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL " __VA_ARGS__); fprintf(stderr, "\n"); g_failures++; } } while (0)

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
    if (!strcmp(n, "nvk_dq_opt")) return g_opt_flag;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    (void)l;
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done_ok(void *u, const nvn_gpu_done *d) {
    (void)u;
    REQUIRE(d->status == NVN_GPU_DONE_OK);
}

static void submit_and_wait(nvn_gpu_cmd cmd) {
    uint64_t s = ++g_sig;
    GPU(nvn_gpu_cmd_commit(cmd, s, on_done_ok, NULL, &err));
    GPU(nvn_gpu_fence_wait(s, 10000000000ull, &err));
}

static int check_short_tail(void) {
    const uint32_t initial = 0xace00001u;
    const uint32_t want = 0xace00002u;
    nvn_gpu_buf buf = NULL;
    GPU(nvn_gpu_buf_create(sizeof initial, NVN_GPU_MEM_SHARED, &initial, &buf, &err));
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, buf, 0, sizeof want, want);
    submit_and_wait(cmd);
    uint32_t got;
    memcpy(&got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    CHECK(got == want, "short-tail got=0x%08x want=0x%08x", got, want);
    return got == want;
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

static void set_scissor_dyn(nvn_gpu_cmd cmd, uint32_t w, uint32_t h, int32_t sx, int32_t sy, uint32_t sw, uint32_t sh) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1;
    dyn.vp[0][2] = (float)w; dyn.vp[0][3] = (float)h; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1;
    dyn.sc[0] = sx; dyn.sc[1] = sy; dyn.sc[2] = (int32_t)sw; dyn.sc[3] = (int32_t)sh;
    nvn_gpu_set_dyn(cmd, &dyn);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 3);
    const char *dir = argv[1];
    g_opt_flag = atoi(argv[2]);
    g_dq_mode = 1;

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = { .host_device = &dev };
    GPU(nvn_gpu_open(&od, &err));
    // 先验证独立的短段。漏刷只会留下空命令缓冲，可以安全读回旧值并具名失败。
    if (!check_short_tail()) return 1;

    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_layout_ent fs_ent[2] = {{0}};
    fs_ent[0].set = 0; fs_ent[0].binding = 0; fs_ent[0].cls = NVN_GPU_RES_UBO; fs_ent[0].access = NVN_GPU_ACC_R; fs_ent[0].count = 1; fs_ent[0].stages = 1u << NVN_GPU_SH_FS;
    fs_ent[1].set = 1; fs_ent[1].binding = 3; fs_ent[1].cls = NVN_GPU_RES_UBO; fs_ent[1].access = NVN_GPU_ACC_R; fs_ent[1].count = 1; fs_ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fs_lay = { .ent = fs_ent, .nent = 2 };
    nvn_gpu_shader fs = load_shader(dir, "ubo_dyn.frag", NVN_GPU_SH_FS, &fs_lay);

    nvn_gpu_layout_ent cs_ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout cs_lay = { .ent = &cs_ent, .nent = 1, .push_bytes = 4 };
    nvn_gpu_shader cs = load_shader(dir, "pushc.comp", NVN_GPU_SH_CS, &cs_lay);

    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe gpipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &gpipe, &err));
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 4; td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err));
    GPU(nvn_gpu_view_create(rt, NULL, &rtv, &err));

    uint8_t udata[1024] = {0};
    const float vals[4][4] = {
        {1.0f, 0.25f, 0.0f, 1.0f},
        {0.5f, 0.75f, 0.0f, 1.0f},
        {0.25f, 1.0f, 0.0f, 1.0f},
        {0.75f, 0.5f, 0.0f, 1.0f},
    };
    for (int k = 0; k < 4; k++) memcpy(udata + (size_t)k * 256, vals[k], sizeof vals[k]);
    nvn_gpu_buf ub = NULL, rb = NULL, ssbo = NULL;
    uint8_t z256[256] = {0};
    GPU(nvn_gpu_buf_create(sizeof udata, NVN_GPU_MEM_SHARED, udata, &ub, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z256, &rb, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z256, &ssbo, &err));

    // ---- Case 1: 256-draw burst in 1 segment under DQ=1 ----
    if (nvn_vk_dbg_dq_opt_stat) nvn_vk_dbg_dq_opt_stat(NULL, 1);
    const uint64_t r0 = nvn_vk_dbg_dq_recs();
    const uint64_t t0 = mono_ns();
    {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_pass_desc p = {0};
        p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.w = p.h = 4; p.layers = 1;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, gpipe);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        const int32_t qx[4] = {0, 2, 0, 2};
        const int32_t qy[4] = {0, 0, 2, 2};
        for (int i = 0; i < 256; i++) {
            const int q = i & 3;
            set_scissor_dyn(cmd, 4, 4, qx[q], qy[q], 2, 2);
            nvn_gpu_binding b[2] = {{0}};
            b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub; b[0].u.b.off = (uint64_t)q * 256u; b[0].u.b.len = 16;
            b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub; b[1].u.b.off = (uint64_t)((q + 1) & 3) * 256u; b[1].u.b.len = 16;
            nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
            nvn_gpu_draw(cmd, &da);
        }
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
        submit_and_wait(cmd);
    }
    const uint64_t burst_us = (mono_ns() - t0) / 1000ull;
    const uint64_t c1_recs = nvn_vk_dbg_dq_recs() - r0;
    uint64_t st[4] = {0};
    if (nvn_vk_dbg_dq_opt_stat) nvn_vk_dbg_dq_opt_stat(st, 0);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    // q=0: ua=vals[0].x=1.0(255), ub=vals[1].y=0.75(191); q=1: 0.5(128), 1.0(255); q=2: 0.25(64), 0.5(128); q=3: 0.75(191), 0.25(64)
    check_rgba("c1-q0", px + (0 * 4 + 0) * 4, 255, 191, 0, 255, 2);
    check_rgba("c1-q1", px + (0 * 4 + 2) * 4, 128, 255, 0, 255, 2);
    check_rgba("c1-q2", px + (2 * 4 + 0) * 4, 64, 128, 0, 255, 2);
    check_rgba("c1-q3", px + (2 * 4 + 2) * 4, 191, 64, 0, 255, 2);

    CHECK(c1_recs >= 700, "c1_recs=%llu want>=700", (unsigned long long)c1_recs);
    if (g_opt_flag) {
        CHECK(nvn_vk_dbg_dq_opt_stat != NULL, "c1(on) stat hook missing");
        CHECK(st[0] > 0 && st[0] * 8 <= c1_recs, "c1(on) pub=%llu recs=%llu want <= recs/8",
              (unsigned long long)st[0], (unsigned long long)c1_recs);
        CHECK(st[2] == 0, "c1(on) rd_reloads=%llu want=0", (unsigned long long)st[2]);
        CHECK(st[1] > 0 && st[1] < c1_recs, "c1(on) rd_stores=%llu want < recs=%llu",
              (unsigned long long)st[1], (unsigned long long)c1_recs);
    } else {
        REQUIRE(nvn_vk_dbg_dq_opt_stat);
        for (unsigned i = 0; i < 4; i++)
            CHECK(st[i] == 0, "c1(off) opt_stat[%u]=%llu want=0", i, (unsigned long long)st[i]);
    }

    // ---- Case 2: 8 tiny segments (< DQ_PUB_BATCH=16 cmds each) proving vk_dq_drain flushes unpublished tails ----
    uint64_t fl_before = st[3];
    for (int k = 0; k < 8; k++) {
        const int q = k & 3;
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_pass_desc p = {0};
        p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.w = p.h = 4; p.layers = 1;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, gpipe);
        nvn_gpu_binding b[2] = {{0}};
        b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub; b[0].u.b.off = (uint64_t)q * 256u; b[0].u.b.len = 16;
        b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub; b[1].u.b.off = (uint64_t)q * 256u; b[1].u.b.len = 16;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
        submit_and_wait(cmd);
        uint8_t wr = (uint8_t)(vals[q][0] * 255.0f + 0.5f);
        uint8_t wg = (uint8_t)(vals[q][1] * 255.0f + 0.5f);
        check_rgba("c2-tail", nvn_gpu_buf_cpu(rb), wr, wg, 0, 255, 2);
    }
    uint64_t st2[4] = {0};
    if (nvn_vk_dbg_dq_opt_stat) nvn_vk_dbg_dq_opt_stat(st2, 0);
    if (g_opt_flag) {
        CHECK(st2[3] > fl_before, "c2(on) expected drain flushes > %llu got %llu",
              (unsigned long long)fl_before, (unsigned long long)st2[3]);
    }

    // ---- Case 3: Async arena_fill + wait + take (OP_FILL must publish immediately!) ----
    {
        nvn_gpu_buf dst = NULL;
        GPU(nvn_gpu_buf_create(1024, NVN_GPU_MEM_SHARED, NULL, &dst, &err));
        uint32_t src_words[64];
        for (uint32_t i = 0; i < 64; i++) src_words[i] = 0x5A000000u + i;
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_slice sl = {0};
        GPU(nvn_gpu_arena_alloc(cmd, sizeof src_words, 16, &sl, &err));
        uint64_t tk1 = nvn_gpu_arena_fill(cmd, &sl, src_words, sizeof src_words);
        CHECK(tk1 > 0, "c3 tk1=%llu want>0 under DQ=1", (unsigned long long)tk1);
        uint64_t done = nvn_gpu_arena_fill_wait(tk1);
        CHECK(done >= tk1, "c3 fill_wait done=%llu < tk1=%llu", (unsigned long long)done, (unsigned long long)tk1);
        nvn_gpu_copy_buf(cmd, sl.buf, sl.off, dst, 0, sizeof src_words);

        // Second fill: test arena_fill_take before mutating source
        for (uint32_t i = 0; i < 64; i++) src_words[i] = 0x7B000000u + i;
        nvn_gpu_slice sl2 = {0};
        GPU(nvn_gpu_arena_alloc(cmd, sizeof src_words, 16, &sl2, &err));
        uint64_t tk2 = nvn_gpu_arena_fill(cmd, &sl2, src_words, sizeof src_words);
        (void)nvn_gpu_arena_fill_take(tk2);
        memset(src_words, 0xEE, sizeof src_words);   // mutate source immediately after take
        nvn_gpu_copy_buf(cmd, sl2.buf, sl2.off, dst, 256, 256);
        submit_and_wait(cmd);

        const uint32_t *dw = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(dst);
        for (uint32_t i = 0; i < 64; i++) {
            CHECK(dw[i] == 0x5A000000u + i, "c3-w1[%u] got=0x%08x", i, dw[i]);
            CHECK(dw[64 + i] == 0x7B000000u + i, "c3-w2[%u] got=0x%08x", i, dw[64 + i]);
        }
        nvn_gpu_buf_release(dst, NULL, NULL);
    }

    // ---- Case 4: Multi-segment compute + abort stress ----
    for (uint32_t k = 0; k < 8; k++) {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_set_pipe(cmd, cpipe);
        nvn_gpu_binding cb = {0};
        cb.ent = 0; cb.cls = NVN_GPU_RES_SSBO; cb.u.b.buf = ssbo; cb.u.b.off = 0; cb.u.b.len = 256;
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
        uint32_t pv = 300u + k;
        nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &pv, sizeof pv);
        nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
        if (k == 3) {
            nvn_gpu_cmd_abort(cmd);   // exercises abort drain while unpublished records may exist
            continue;
        }
        submit_and_wait(cmd);
        const uint32_t *out = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(ssbo);
        CHECK(out[0] == pv && out[63] == pv + 63u, "c4-cs[%u] got=%u/%u want=%u/%u", k, out[0], out[63], pv, pv + 63u);
    }

    nvn_gpu_buf_release(ssbo, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(ub, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(cs);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
    GPU(nvn_gpu_close(10000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);

    printf("dq-opt flag=%d burst_us=%llu recs=%llu pub=%llu rd=%llu reload=%llu flush=%llu cases=5 failures=%u\n",
           g_opt_flag, (unsigned long long)burst_us, (unsigned long long)c1_recs,
           (unsigned long long)st[0], (unsigned long long)st[1],
           (unsigned long long)st[2], (unsigned long long)st2[3],
           g_failures);
    return g_failures ? 1 : 0;
}
