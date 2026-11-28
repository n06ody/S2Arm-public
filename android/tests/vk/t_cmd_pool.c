// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract fixture for command-segment object & dynamic-table pool (debug.s2a.nvk_cmd_pool):
//   1) 32 sequential graphics+compute+UBO+push+occlusion segments: verifies cmd_s reuse (hit>=29, miss<=3) and exact GPU pixels/SSBO
//   2) Cross-segment isolation under nvk_dset_rec=0: desc_memo O(1) memo_gen invalidation, refseen reset (live buf refcount), push_data zero-reset
//   3) Pool cap > SEGPOOL_CMD_OBJ_N=16 burst (via delay_segment) + nvn_gpu_cmd_abort recycling
//   4) nvn_gpu_close drain (cur==0) + fresh session reopen reuse
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void nvn_vk_dbg_cmd_pool_stat(uint64_t *hit, uint64_t *miss, uint64_t *kept, uint64_t *full, uint64_t *trimmed, uint32_t *cur) __attribute__((weak));
extern void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms);
extern uint32_t nvn_gpu_vk_test_buf_refs(nvn_gpu_buf bh);

static nvn_gpu_err err;
static int g_dq_mode = 0;
static int g_pool_flag = 1;
static int g_dset_rec = 1;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "cmd-pool fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL " __VA_ARGS__); fprintf(stderr, "\n"); g_failures++; } } while (0)

// ---- Platform stubs ----
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_mode;
    if (!strcmp(n, "nvk_cmd_pool")) return g_pool_flag;
    if (!strcmp(n, "nvk_dset_rec")) return g_dset_rec;
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

typedef struct {
    nvn_gpu_shader vs, fs, cs;
    nvn_gpu_pipe gpipe, cpipe;
    nvn_gpu_tex rt;
    nvn_gpu_view rtv;
    nvn_gpu_buf ubo1, ubo2, ssbo, rb;
} gfx_res;

static void gfx_res_init(gfx_res *r, const char *dir) {
    memset(r, 0, sizeof *r);
    r->vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);

    nvn_gpu_layout_ent fs_ent[2] = {{0}};
    fs_ent[0].set = 0; fs_ent[0].binding = 0; fs_ent[0].cls = NVN_GPU_RES_UBO; fs_ent[0].access = NVN_GPU_ACC_R; fs_ent[0].count = 1; fs_ent[0].stages = 1u << NVN_GPU_SH_FS;
    fs_ent[1].set = 1; fs_ent[1].binding = 3; fs_ent[1].cls = NVN_GPU_RES_UBO; fs_ent[1].access = NVN_GPU_ACC_R; fs_ent[1].count = 1; fs_ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fs_lay = { .ent = fs_ent, .nent = 2 };
    r->fs = load_shader(dir, "ubo_dyn.frag", NVN_GPU_SH_FS, &fs_lay);

    nvn_gpu_layout_ent cs_ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout cs_lay = { .ent = &cs_ent, .nent = 1, .push_bytes = 4 };
    r->cs = load_shader(dir, "pushc.comp", NVN_GPU_SH_CS, &cs_lay);

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 4; td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    GPU(nvn_gpu_tex_create(&td, &r->rt, &err));
    GPU(nvn_gpu_view_create(r->rt, NULL, &r->rtv, &err));

    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = r->vs;
    pd.st[NVN_GPU_SH_FS].sh = r->fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &r->gpipe, &err));

    nvn_gpu_stage_desc csd = { .sh = r->cs };
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &r->cpipe, &err));

    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float grn[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    uint8_t u1[256] = {0}, u2[256] = {0}, z256[256] = {0};
    memcpy(u1, red, sizeof red);
    memcpy(u2, grn, sizeof grn);
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, u1, &r->ubo1, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, u2, &r->ubo2, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z256, &r->ssbo, &err));
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, z256, &r->rb, &err));
}

static void gfx_res_free(gfx_res *r) {
    nvn_gpu_buf_release(r->rb, NULL, NULL);
    nvn_gpu_buf_release(r->ssbo, NULL, NULL);
    nvn_gpu_buf_release(r->ubo2, NULL, NULL);
    nvn_gpu_buf_release(r->ubo1, NULL, NULL);
    nvn_gpu_view_release(r->rtv);
    nvn_gpu_tex_release(r->rt);
    nvn_gpu_shader_release(r->cs);
    nvn_gpu_shader_release(r->fs);
    nvn_gpu_shader_release(r->vs);
}

static void bind_fs_ubo(nvn_gpu_cmd cmd, nvn_gpu_buf ubo) {
    nvn_gpu_binding gb[2] = {{0}};
    gb[0].ent = 0; gb[0].cls = NVN_GPU_RES_UBO; gb[0].u.b.buf = ubo; gb[0].u.b.off = 0; gb[0].u.b.len = 16;
    gb[1].ent = 1; gb[1].cls = NVN_GPU_RES_UBO; gb[1].u.b.buf = ubo; gb[1].u.b.off = 0; gb[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, gb, 2);
}

static void record_draw_and_cs(nvn_gpu_cmd cmd, const gfx_res *r, nvn_gpu_buf ubo, uint32_t push_val) {
    nvn_gpu_pass_desc pass = {0};
    pass.w = 4; pass.h = 4; pass.layers = 1; pass.ncolor = 1;
    pass.color[0].view = r->rtv;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, r->gpipe);
    bind_fs_ubo(cmd, ubo);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    // Draw twice with identical bindings to exercise desc_memo insert + hit
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);

    nvn_gpu_set_pipe(cmd, r->cpipe);
    nvn_gpu_binding cb = {0};
    cb.ent = 0; cb.cls = NVN_GPU_RES_SSBO; cb.u.b.buf = r->ssbo; cb.u.b.off = 0; cb.u.b.len = 256;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &push_val, sizeof push_val);
    nvn_gpu_dispatch(cmd, r->cpipe, 1, 1, 1, NULL, 0);

    nvn_gpu_copy_tex_buf(cmd, r->rt, 0, 0, NULL, r->rb, 0, 16, 64);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 4);
    const char *dir = argv[1];
    g_dq_mode = atoi(argv[2]);
    g_pool_flag = atoi(argv[3]);

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = { .host_device = &dev };

    // ---- Session 1 (dset_rec = 1): Case 1 (32 seq gfx+cs+occlusion) + Case 3 (24 burst > cap 16 + abort) ----
    g_dset_rec = 1;
    g_sig = 0;
    GPU(nvn_gpu_open(&od, &err));
    gfx_res res;
    gfx_res_init(&res, dir);

    // Case 1: 32 sequential graphics+compute+occlusion segments
    for (uint32_t i = 0; i < 32; i++) {
        nvn_gpu_cmd cmd = NULL;
        nvn_gpu_cmd_desc cd = { .occlusion_slots = 4 };
        GPU(nvn_gpu_cmd_begin(&cd, &cmd, &err));
        nvn_gpu_buf ubo = (i & 1) ? res.ubo2 : res.ubo1;
        uint32_t pv = 100u + i;
        record_draw_and_cs(cmd, &res, ubo, pv);
        submit_and_wait(cmd);
        const uint8_t *px = nvn_gpu_buf_cpu(res.rb);
        if (i & 1) check_rgba("c1-grn", px, 0, 255, 0, 255, 2);
        else check_rgba("c1-red", px, 255, 0, 0, 255, 2);
        const uint32_t *out = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(res.ssbo);
        CHECK(out[0] == pv && out[63] == pv + 63u, "c1-cs[%u] got=%u/%u want=%u/%u", i, out[0], out[63], pv, pv + 63u);
    }

    uint64_t c1_hit = 0, c1_miss = 0, c1_kept = 0, c1_full = 0, c1_trim = 0;
    uint32_t c1_cur = 0;
    if (nvn_vk_dbg_cmd_pool_stat) nvn_vk_dbg_cmd_pool_stat(&c1_hit, &c1_miss, &c1_kept, &c1_full, &c1_trim, &c1_cur);
    if (g_pool_flag) {
        CHECK(nvn_vk_dbg_cmd_pool_stat != NULL, "c1(on) stat hook missing");
        CHECK(c1_hit >= 29, "c1(on) hit=%llu want>=29", (unsigned long long)c1_hit);
        CHECK(c1_miss <= 3, "c1(on) miss=%llu want<=3", (unsigned long long)c1_miss);
        CHECK(c1_kept >= 31, "c1(on) kept=%llu want>=31", (unsigned long long)c1_kept);
    } else {
        CHECK(c1_hit == 0 && c1_miss == 0 && c1_kept == 0, "c1(off) expected zero pool activity");
    }

    // Case 3: Pool cap overflow (> 16 simultaneously in-flight via delay_segment) + abort recycling
    nvn_gpu_buf bbuf = NULL;
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, NULL, &bbuf, &err));
    volatile uint32_t *bcpu = (volatile uint32_t *)(void *)nvn_gpu_buf_cpu(bbuf);
    memset((void *)(uintptr_t)bcpu, 0, 256);

    uint64_t burst_first = g_sig + 1;
    nvn_gpu_vk_test_delay_segment(burst_first, 200);
    uint64_t burst_sigs[24];
    for (uint32_t i = 0; i < 24; i++) {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_fill_buf(cmd, bbuf, (uint64_t)i * 4ull, 4, 0xCAFE0000u + i);
        uint64_t s = ++g_sig;
        GPU(nvn_gpu_cmd_commit(cmd, s, on_done_ok, NULL, &err));
        burst_sigs[i] = s;
    }
    GPU(nvn_gpu_fence_wait(burst_sigs[23], 10000000000ull, &err));
    nvn_gpu_vk_test_delay_segment(0, 0);
    for (uint32_t i = 0; i < 24; i++) {
        CHECK(bcpu[i] == 0xCAFE0000u + i, "c3-burst[%u] got=0x%08x want=0x%08x", i, bcpu[i], 0xCAFE0000u + i);
    }
    // Exercise cmd_abort on a pooled cmd with occlusion + buffer refs
    {
        nvn_gpu_cmd acmd = NULL;
        nvn_gpu_cmd_desc cd = { .occlusion_slots = 4 };
        GPU(nvn_gpu_cmd_begin(&cd, &acmd, &err));
        nvn_gpu_fill_buf(acmd, bbuf, 25 * 4ull, 4, 0xDEADu);
        nvn_gpu_cmd_abort(acmd);
    }

    uint64_t c3_hit = 0, c3_miss = 0, c3_kept = 0, c3_full = 0, c3_trim = 0;
    uint32_t c3_cur = 0;
    if (nvn_vk_dbg_cmd_pool_stat) nvn_vk_dbg_cmd_pool_stat(&c3_hit, &c3_miss, &c3_kept, &c3_full, &c3_trim, &c3_cur);
    if (g_pool_flag) {
        CHECK(c3_cur <= 16, "c3(on) cur=%u exceeds cap 16", c3_cur);
        CHECK(c3_full >= 1, "c3(on) expected cap overflow drops >= 1 got=%llu", (unsigned long long)c3_full);
    }

    nvn_gpu_buf_release(bbuf, NULL, NULL);
    gfx_res_free(&res);
    GPU(nvn_gpu_close(10000000000ull, &err));
    if (nvn_vk_dbg_cmd_pool_stat) {
        uint32_t cur_after = 999;
        nvn_vk_dbg_cmd_pool_stat(NULL, NULL, NULL, NULL, NULL, &cur_after);
        CHECK(cur_after == 0, "c4-close1 cur=%u want=0", cur_after);
    }

    // ---- Session 2 (dset_rec = 0): Case 2 (strict desc_memo / refseen / push_data cross-segment isolation) ----
    // With nvk_dset_rec=0, segment 1's VkDescriptorPool is returned to g_seg_desc and vkResetDescriptorPool'd
    // in segment 2 when draw A misses desc_memo (ubo2). Draw B then binds ubo1 (same key as segment 1):
    // if memo_gen was NOT bumped on reuse, draw B hits segment 1's reset/freed VkDescriptorSet -> VVL fires!
    g_dset_rec = 0;
    g_sig = 0;
    GPU(nvn_gpu_open(&od, &err));
    gfx_res res2;
    gfx_res_init(&res2, dir);

    nvn_gpu_buf tb = NULL;
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, NULL, &tb, &err));
    {
        nvn_gpu_cmd cmd1 = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd1, &err));
        nvn_gpu_fill_buf(cmd1, tb, 0, 4, 0x11111111u);
        record_draw_and_cs(cmd1, &res2, res2.ubo1, 777u);
        submit_and_wait(cmd1);
        check_rgba("c2-seg1-red", nvn_gpu_buf_cpu(res2.rb), 255, 0, 0, 255, 2);
    }

    for (uint32_t k = 0; k < 16; k++) {
        nvn_gpu_cmd cmd2 = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd2, &err));
        // Verify refseen was cleared across segment reuse: referencing tb in cmd2 MUST bump tb->refs >= 1!
        nvn_gpu_fill_buf(cmd2, tb, 0, 4, 0x22220000u + k);
        CHECK(nvn_gpu_vk_test_buf_refs(tb) >= 1, "c2-refseen[%u] tb refs=%u want>=1", k, nvn_gpu_vk_test_buf_refs(tb));

        nvn_gpu_pass_desc pass = {0};
        pass.w = 4; pass.h = 4; pass.layers = 1; pass.ncolor = 1;
        pass.color[0].view = res2.rtv;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(cmd2, &pass);
        nvn_gpu_set_pipe(cmd2, res2.gpipe);
        // Draw A: ubo2 (GREEN) -> forces seg_take_desc + vkResetDescriptorPool on recycled pool from previous segment
        bind_fs_ubo(cmd2, res2.ubo2);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd2, &da);
        // Draw B: ubo1 (RED) -> exact same desc_memo key as previous segment; MUST miss (memo_gen++) and allocate fresh set!
        bind_fs_ubo(cmd2, res2.ubo1);
        nvn_gpu_draw(cmd2, &da);
        nvn_gpu_pass_end(cmd2);

        // Push isolation check: write distinct compute push value
        uint32_t pv = 500u + k;
        nvn_gpu_set_pipe(cmd2, res2.cpipe);
        nvn_gpu_binding cb = {0};
        cb.ent = 0; cb.cls = NVN_GPU_RES_SSBO; cb.u.b.buf = res2.ssbo; cb.u.b.off = 0; cb.u.b.len = 256;
        nvn_gpu_bind(cmd2, NVN_GPU_SH_CS, &cb, 1);
        nvn_gpu_push(cmd2, NVN_GPU_SH_CS, 0, &pv, sizeof pv);
        nvn_gpu_dispatch(cmd2, res2.cpipe, 1, 1, 1, NULL, 0);

        nvn_gpu_copy_tex_buf(cmd2, res2.rt, 0, 0, NULL, res2.rb, 0, 16, 64);
        submit_and_wait(cmd2);
        check_rgba("c2-seg2-red", nvn_gpu_buf_cpu(res2.rb), 255, 0, 0, 255, 2);
        const uint32_t *out = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(res2.ssbo);
        CHECK(out[0] == pv, "c2-cs[%u] got=%u want=%u", k, out[0], pv);
    }

    uint64_t r_hit = 0, r_miss = 0;
    if (nvn_vk_dbg_cmd_pool_stat) nvn_vk_dbg_cmd_pool_stat(&r_hit, &r_miss, NULL, NULL, NULL, NULL);
    if (g_pool_flag) {
        CHECK(r_hit >= 14, "c4reopen(on) hit=%llu want>=14", (unsigned long long)r_hit);
        CHECK(r_miss <= 3, "c4reopen(on) miss=%llu want<=3", (unsigned long long)r_miss);
    } else {
        CHECK(r_hit == 0 && r_miss == 0, "c4reopen(off) expected zero pool activity");
    }

    nvn_gpu_buf_release(tb, NULL, NULL);
    gfx_res_free(&res2);
    GPU(nvn_gpu_close(10000000000ull, &err));
    if (nvn_vk_dbg_cmd_pool_stat) {
        uint32_t cur_after = 999;
        nvn_vk_dbg_cmd_pool_stat(NULL, NULL, NULL, NULL, NULL, &cur_after);
        CHECK(cur_after == 0, "c4-close2 cur=%u want=0", cur_after);
    }
    s2a_vk_device_destroy(&dev, 1);

    printf("cmd-pool dq=%d flag=%d c1(hit=%llu,miss=%llu,kept=%llu) cap(full=%llu,cur=%u,trim=%llu) "
           "reopen(hit=%llu,miss=%llu) cases=4 failures=%u\n",
           g_dq_mode, g_pool_flag,
           (unsigned long long)c1_hit, (unsigned long long)c1_miss, (unsigned long long)c1_kept,
           (unsigned long long)c3_full, c3_cur, (unsigned long long)c3_trim,
           (unsigned long long)r_hit, (unsigned long long)r_miss,
           g_failures);
    return g_failures ? 1 : 0;
}
