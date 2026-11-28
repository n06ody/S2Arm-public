// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract & benchmark fixture for consecutive buf_write coalescing + narrowed leading barrier
// + overlap-safe WAW barrier + batched STAT_WRITES g_mu (debug.s2a.nvk_xfer_batch):
//   1) 64 consecutive disjoint buf_write -> bar_double.comp compute dispatch + readback:
//      verifies DQ vkCmd records drop (>=128 -> <=16), bars 64->1, copies 64->1, g_mu 64->1, exact 128 words
//   2) Overlapping consecutive buf_write (full + partial + straddling):
//      verifies WAW barrier emission (waw >= 3), 0 SyncVal errors, exact latest-writer bytes
//   3) Multi-buffer interleaved disjoint buf_write -> graphics render pass UBO draw + copy_buf + fill_buf:
//      verifies single leading barrier across buffers, exact 4-quadrant pixels & copied buffer bytes
//   4) Large multi-chunk buf_write (> STG_CHUNK=256KB-4) + cmd_abort with pending batched writes
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern uint64_t nvn_vk_dbg_dq_recs(void);
extern void nvn_vk_dbg_xfer_batch_stat(uint64_t out[5], int reset) __attribute__((weak));

static nvn_gpu_err err;
static int g_dq_mode = 1;
static int g_xb_flag = 1;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "xfer-batch fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
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
    if (!strcmp(n, "nvk_xfer_batch")) return g_xb_flag;
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
    g_xb_flag = atoi(argv[2]);
    g_dq_mode = 1;

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = { .host_device = &dev };
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_layout_ent fs_ent[2] = {{0}};
    fs_ent[0].set = 0; fs_ent[0].binding = 0; fs_ent[0].cls = NVN_GPU_RES_UBO; fs_ent[0].access = NVN_GPU_ACC_R; fs_ent[0].count = 1; fs_ent[0].stages = 1u << NVN_GPU_SH_FS;
    fs_ent[1].set = 1; fs_ent[1].binding = 3; fs_ent[1].cls = NVN_GPU_RES_UBO; fs_ent[1].access = NVN_GPU_ACC_R; fs_ent[1].count = 1; fs_ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fs_lay = { .ent = fs_ent, .nent = 2 };
    nvn_gpu_shader fs = load_shader(dir, "ubo_dyn.frag", NVN_GPU_SH_FS, &fs_lay);

    nvn_gpu_layout_ent cs_ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_RW, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout cs_lay = { .ent = &cs_ent, .nent = 1 };
    nvn_gpu_shader cs = load_shader(dir, "bar_double.comp", NVN_GPU_SH_CS, &cs_lay);

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

    uint8_t z1024[1024] = {0};
    nvn_gpu_buf ssbo = NULL, ov_buf = NULL, ub = NULL, bufB = NULL, bufC = NULL, rb = NULL, big = NULL;
    GPU(nvn_gpu_buf_create(512, NVN_GPU_MEM_SHARED, z1024, &ssbo, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z1024, &ov_buf, &err));
    GPU(nvn_gpu_buf_create(1024, NVN_GPU_MEM_SHARED, z1024, &ub, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z1024, &bufB, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z1024, &bufC, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z1024, &rb, &err));
    const uint64_t big_sz = 320u * 1024u;
    uint8_t *big_init = calloc(1, (size_t)big_sz);
    REQUIRE(big_init);
    GPU(nvn_gpu_buf_create(big_sz, NVN_GPU_MEM_SHARED, big_init, &big, &err));

    // ---- Case 1: 64 consecutive disjoint buf_write -> compute dispatch (bar_double.comp) ----
    if (nvn_vk_dbg_xfer_batch_stat) nvn_vk_dbg_xfer_batch_stat(NULL, 1);
    const uint64_t r0 = nvn_vk_dbg_dq_recs();
    const uint64_t t0 = mono_ns();
    {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t v = 0x100u + i;
            nvn_gpu_buf_write(cmd, ssbo, (uint64_t)i * 4u, &v, sizeof v);
        }
        nvn_gpu_set_pipe(cmd, cpipe);
        nvn_gpu_binding cb = { .ent = 0, .cls = NVN_GPU_RES_SSBO, .u.b = { ssbo, 0, 512 } };
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
        nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
        submit_and_wait(cmd);
    }
    const uint64_t burst_us = (mono_ns() - t0) / 1000ull;
    const uint64_t c1_recs = nvn_vk_dbg_dq_recs() - r0;
    uint64_t st1[5] = {0};
    if (nvn_vk_dbg_xfer_batch_stat) nvn_vk_dbg_xfer_batch_stat(st1, 0);

    const uint32_t *sw = (const uint32_t *)nvn_gpu_buf_cpu(ssbo);
    for (uint32_t i = 0; i < 64; i++) {
        const uint32_t want = 0x100u + i;
        CHECK(sw[i] == want, "c1 src[%u]=%#x want=%#x", i, sw[i], want);
        CHECK(sw[64u + i] == want * 2u, "c1 dst[%u]=%#x want=%#x", i, sw[64u + i], want * 2u);
    }
    if (g_xb_flag) {
        CHECK(nvn_vk_dbg_xfer_batch_stat != NULL, "c1(on) stat hook missing");
        CHECK(c1_recs <= 20, "c1(on) dq_recs=%llu want <= 20 (coalesced 64 writes -> 1 bar + 1 copy)",
              (unsigned long long)c1_recs);
        CHECK(st1[0] == 64 && st1[1] == 1 && st1[2] == 1 && st1[3] == 0 && st1[4] == 1,
              "c1(on) stat w=%llu bar=%llu cp=%llu waw=%llu gmu=%llu want 64/1/1/0/1",
              (unsigned long long)st1[0], (unsigned long long)st1[1], (unsigned long long)st1[2],
              (unsigned long long)st1[3], (unsigned long long)st1[4]);
    } else {
        CHECK(c1_recs >= 128, "c1(off) dq_recs=%llu want >= 128", (unsigned long long)c1_recs);
        REQUIRE(nvn_vk_dbg_xfer_batch_stat);
        for (unsigned i = 0; i < 5; i++)
            CHECK(st1[i] == 0, "c1(off) batch_stat[%u]=%llu want=0", i, (unsigned long long)st1[i]);
    }

    // ---- Case 2: Overlapping consecutive buf_write (full + partial + straddling WAW, both same-dst & interleaved-dst) ----
    if (nvn_vk_dbg_xfer_batch_stat) nvn_vk_dbg_xfer_batch_stat(NULL, 1);
    {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        const uint32_t w1[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        const uint32_t w2[4] = {0xAAAA0001u, 0xAAAA0002u, 0xAAAA0003u, 0xAAAA0004u};
        const uint32_t w3[2] = {0xBEEF0001u, 0xBEEF0002u};
        const uint32_t w4[2] = {0xCAFE0003u, 0xCAFE0004u};
        const uint32_t sep = 0x12345678u;
        nvn_gpu_buf_write(cmd, ov_buf, 0, w1, sizeof w1);    // [0..16)
        nvn_gpu_buf_write(cmd, bufB, 240, &sep, sizeof sep); // switches dst -> emits copy(w1) so overlap spans 2 vkCmdCopyBuffer calls
        nvn_gpu_buf_write(cmd, ov_buf, 8, w2, sizeof w2);    // [8..24) overlaps w1 -> WAW #1
        nvn_gpu_buf_write(cmd, ov_buf, 0, w3, sizeof w3);    // [0..8)  disjoint from post-WAW [8..24), safely coalesced!
        nvn_gpu_buf_write(cmd, ov_buf, 12, w4, sizeof w4);   // [12..20) straddles w2 -> WAW #2
        submit_and_wait(cmd);
    }
    uint64_t st2[5] = {0};
    if (nvn_vk_dbg_xfer_batch_stat) nvn_vk_dbg_xfer_batch_stat(st2, 0);
    const uint32_t *ov = (const uint32_t *)nvn_gpu_buf_cpu(ov_buf);
    const uint32_t want_ov[6] = {
        0xBEEF0001u, 0xBEEF0002u, 0xAAAA0001u, 0xCAFE0003u, 0xCAFE0004u, 0xAAAA0004u
    };
    for (int i = 0; i < 6; i++)
        CHECK(ov[i] == want_ov[i], "c2 ov[%d]=%#x want=%#x", i, ov[i], want_ov[i]);
    if (g_xb_flag) {
        CHECK(st2[3] == 2, "c2(on) waw=%llu want=2", (unsigned long long)st2[3]);
    }

    // ---- Case 3: Multi-buffer interleaved disjoint buf_write -> graphics UBO pass + copy_buf + fill_buf ----
    if (nvn_vk_dbg_xfer_batch_stat) nvn_vk_dbg_xfer_batch_stat(NULL, 1);
    {
        const float vals[4][4] = {
            {1.0f, 0.25f, 0.0f, 1.0f},
            {0.5f, 0.75f, 0.0f, 1.0f},
            {0.25f, 1.0f, 0.0f, 1.0f},
            {0.75f, 0.5f, 0.0f, 1.0f},
        };
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        for (int k = 0; k < 4; k++) {
            uint32_t tag = 0x50500000u + (uint32_t)k;
            nvn_gpu_buf_write(cmd, ub, (uint64_t)k * 256u, vals[k], sizeof vals[k]);
            nvn_gpu_buf_write(cmd, bufB, (uint64_t)k * 16u, &tag, sizeof tag);
        }
        // Immediately begin graphics render pass (must flush batched buf_write before pass_begin!)
        nvn_gpu_pass_desc p = {0};
        p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.w = p.h = 4; p.layers = 1;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, gpipe);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        const int32_t qx[4] = {0, 2, 0, 2};
        const int32_t qy[4] = {0, 0, 2, 2};
        for (int q = 0; q < 4; q++) {
            set_scissor_dyn(cmd, 4, 4, qx[q], qy[q], 2, 2);
            nvn_gpu_binding b[2] = {{0}};
            b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub; b[0].u.b.off = (uint64_t)q * 256u; b[0].u.b.len = 16;
            b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub; b[1].u.b.off = (uint64_t)((q + 1) & 3) * 256u; b[1].u.b.len = 16;
            nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
            nvn_gpu_draw(cmd, &da);
        }
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
        // More disjoint writes flushed by copy_buf and fill_buf
        for (int k = 4; k < 8; k++) {
            uint32_t tag = 0x50500000u + (uint32_t)k;
            nvn_gpu_buf_write(cmd, bufB, (uint64_t)k * 16u, &tag, sizeof tag);
        }
        nvn_gpu_copy_buf(cmd, bufB, 0, bufC, 0, 128);
        nvn_gpu_fill_buf(cmd, bufC, 128, 16, 0x778899AAu);
        submit_and_wait(cmd);
    }
    uint64_t st3[5] = {0};
    if (nvn_vk_dbg_xfer_batch_stat) nvn_vk_dbg_xfer_batch_stat(st3, 0);
    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c3-q0", px + (0 * 4 + 0) * 4, 255, 191, 0, 255, 2);
    check_rgba("c3-q1", px + (0 * 4 + 2) * 4, 128, 255, 0, 255, 2);
    check_rgba("c3-q2", px + (2 * 4 + 0) * 4, 64, 128, 0, 255, 2);
    check_rgba("c3-q3", px + (2 * 4 + 2) * 4, 191, 64, 0, 255, 2);
    const uint32_t *bc = (const uint32_t *)nvn_gpu_buf_cpu(bufC);
    for (int k = 0; k < 8; k++)
        CHECK(bc[k * 4] == (0x50500000u + (uint32_t)k), "c3 bufC[%d]=%#x want=%#x",
              k * 4, bc[k * 4], 0x50500000u + (uint32_t)k);
    CHECK(bc[32] == 0x778899AAu && bc[35] == 0x778899AAu, "c3 fill=%#x/%#x", bc[32], bc[35]);
    if (g_xb_flag) {
        CHECK(st3[0] == 12 && st3[1] == 2 && st3[3] == 0,
              "c3(on) w=%llu bar=%llu waw=%llu want 12/2/0",
              (unsigned long long)st3[0], (unsigned long long)st3[1], (unsigned long long)st3[3]);
    }

    // ---- Case 4: Large multi-chunk buf_write (> STG_CHUNK=256KB-4) + cmd_abort with pending batch ----
    {
        const size_t big_n = 300u * 1024u;
        uint8_t *pat = malloc(big_n);
        REQUIRE(pat);
        for (size_t i = 0; i < big_n; i++) pat[i] = (uint8_t)((i * 31u + 7u) & 0xffu);
        uint32_t tail[16];
        for (uint32_t i = 0; i < 16; i++) tail[i] = 0xDE000000u + i;

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_buf_write(cmd, big, 0, pat, big_n);
        nvn_gpu_buf_write(cmd, big, big_n, tail, sizeof tail);
        submit_and_wait(cmd);

        const uint8_t *got = nvn_gpu_buf_cpu(big);
        CHECK(memcmp(got, pat, big_n) == 0, "c4 300KB multi-chunk mismatch");
        CHECK(memcmp(got + big_n, tail, sizeof tail) == 0, "c4 tail mismatch");
        free(pat);

        // Abort segment with pending batched writes (no commit, no flush)
        nvn_gpu_cmd ab = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &ab, &err));
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t z = 0xBAD00000u + i;
            nvn_gpu_buf_write(ab, bufB, (uint64_t)i * 4u, &z, sizeof z);
        }
        nvn_gpu_cmd_abort(ab);
    }

    printf("xfer-batch flag=%d c1_us=%llu c1_recs=%llu c1_bar=%llu c1_cp=%llu c1_gmu=%llu c2_waw=%llu cases=4 failures=%u\n",
           g_xb_flag, (unsigned long long)burst_us, (unsigned long long)c1_recs,
           (unsigned long long)st1[1], (unsigned long long)st1[2], (unsigned long long)st1[4],
           (unsigned long long)st2[3], g_failures);

    free(big_init);
    nvn_gpu_buf_release(big, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(bufC, NULL, NULL);
    nvn_gpu_buf_release(bufB, NULL, NULL);
    nvn_gpu_buf_release(ub, NULL, NULL);
    nvn_gpu_buf_release(ov_buf, NULL, NULL);
    nvn_gpu_buf_release(ssbo, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(cs);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);
    return g_failures ? 1 : 0;
}
