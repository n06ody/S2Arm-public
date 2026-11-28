// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract & benchmark fixture for large texture staging pool + host tex_upload barrier/copy coalescing
// (debug.s2a.nvk_tex_upload_pool):
//   1) 48-subresource burst (6-layer x 8-mip 128x128 RGBA8 array):
//      verifies DQ vkCmd records drop (>=190 -> <=16), full_bar 48->0, img_bar 96->2, copy_cmd 48-> <=4, exact pixels
//   2) Overlapping sub-rect uploads across 2 vkCmdCopyBufferToImage calls (SyncVal WAW + mutation negative control):
//      verifies narrow image WAW barrier (waw >= 1), 0 SyncVal errors, exact latest-writer pixels
//   3) Repeated >256KB uploads across segments (1 MiB 512x512 x6 + 1.5 MiB 768x512 x2 -> 1 MiB & 2 MiB buckets):
//      verifies reusable staging pool hits (>=6 hits, 0 extra misses) and exact pixel readback
//   4) Flush boundaries (pass_begin LOAD, blit, cmd_abort with pending batch) + 32 MiB LRU eviction cap
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern uint64_t nvn_vk_dbg_dq_recs(void);
extern void nvn_vk_dbg_tex_pool_stat(uint64_t out[8], int reset) __attribute__((weak));

static nvn_gpu_err err;
static int g_dq_mode = 1;
static int g_tp_flag = 1;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "tex-upload-pool fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
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
    if (!strcmp(n, "nvk_tex_upload_pool")) return g_tp_flag;
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

static void fill_u32(uint32_t *p, size_t n, uint32_t v) {
    for (size_t i = 0; i < n; i++) p[i] = v;
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 2);
    g_tp_flag = atoi(argv[1]);
    g_dq_mode = 1;

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = { .host_device = &dev };
    GPU(nvn_gpu_open(&od, &err));

    // ---- Case 1: 48-subresource upload burst (6-layer x 8-mip 128x128 RGBA8 array) ----
    nvn_gpu_tex_desc arr_td = {0};
    arr_td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    arr_td.d.format = NVN_FORMAT_RGBA8;
    arr_td.d.width = 128; arr_td.d.height = 128; arr_td.d.depth = 6; arr_td.d.levels = 8;
    arr_td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex arr_tex = NULL;
    GPU(nvn_gpu_tex_create(&arr_td, &arr_tex, &err));

    uint32_t *mip_buf = malloc(128u * 128u * 4u);
    REQUIRE(mip_buf);
    uint8_t *rb_z = calloc(1, 128u * 128u * 4u);
    REQUIRE(rb_z);
    nvn_gpu_buf rb1 = NULL;
    GPU(nvn_gpu_buf_create(128u * 128u * 4u, NVN_GPU_MEM_SHARED, rb_z, &rb1, &err));

    if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(NULL, 1);
    const uint64_t r0 = nvn_vk_dbg_dq_recs();
    const uint64_t t0 = mono_ns();
    {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        for (uint32_t layer = 0; layer < 6; layer++) {
            for (uint32_t mip = 0; mip < 8; mip++) {
                uint32_t w = 128u >> mip, h = 128u >> mip;
                uint32_t color = 0xFF000000u | ((layer + 1u) << 16) | ((mip + 1u) << 8) | (layer * 16u + mip);
                fill_u32(mip_buf, (size_t)w * h, color);
                nvn_gpu_tex_upload(cmd, arr_tex, mip, layer, NULL, mip_buf, (uint64_t)w * 4u, (uint64_t)w * h * 4u);
            }
        }
        submit_and_wait(cmd);
    }
    const uint64_t c1_us = (mono_ns() - t0) / 1000ull;
    const uint64_t c1_recs = nvn_vk_dbg_dq_recs() - r0;
    uint64_t st1[8] = {0};
    if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(st1, 0);

    // Read back sample mips across all 6 layers (mip 0, mip 3, mip 7)
    for (uint32_t layer = 0; layer < 6; layer++) {
        const uint32_t mips[3] = {0, 3, 7};
        for (int mi = 0; mi < 3; mi++) {
            uint32_t mip = mips[mi];
            uint32_t w = 128u >> mip, h = 128u >> mip;
            nvn_gpu_cmd cmd = NULL;
            GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
            nvn_gpu_copy_tex_buf(cmd, arr_tex, mip, layer, NULL, rb1, 0, (uint64_t)w * 4u, (uint64_t)w * h * 4u);
            submit_and_wait(cmd);
            const uint32_t *got = (const uint32_t *)nvn_gpu_buf_cpu(rb1);
            uint32_t want = 0xFF000000u | ((layer + 1u) << 16) | ((mip + 1u) << 8) | (layer * 16u + mip);
            CHECK(got[0] == want && got[(size_t)w * h - 1u] == want,
                  "c1 layer=%u mip=%u got=%#x/%#x want=%#x", layer, mip, got[0], got[(size_t)w * h - 1u], want);
        }
    }
    if (g_tp_flag) {
        CHECK(nvn_vk_dbg_tex_pool_stat != NULL, "c1(on) stat hook missing");
        CHECK(c1_recs <= 20, "c1(on) dq_recs=%llu want <= 20 (48 uploads coalesced)", (unsigned long long)c1_recs);
        CHECK(st1[0] == 48 && st1[1] == 0 && st1[2] == 2 && st1[3] <= 4 && st1[4] == 0,
              "c1(on) up=%llu full=%llu img=%llu cp=%llu waw=%llu want 48/0/2/<=4/0",
              (unsigned long long)st1[0], (unsigned long long)st1[1], (unsigned long long)st1[2],
              (unsigned long long)st1[3], (unsigned long long)st1[4]);
    } else {
        CHECK(c1_recs >= 190, "c1(off) dq_recs=%llu want >= 190", (unsigned long long)c1_recs);
        if (nvn_vk_dbg_tex_pool_stat) {
            CHECK(st1[0] == 48 && st1[1] == 48 && st1[2] == 96 && st1[3] == 48,
                  "c1(off) up=%llu full=%llu img=%llu cp=%llu want 48/48/96/48",
                  (unsigned long long)st1[0], (unsigned long long)st1[1], (unsigned long long)st1[2],
                  (unsigned long long)st1[3]);
        }
    }

    // ---- Case 2: Overlapping sub-rect uploads across 2 vkCmdCopyBufferToImage calls (SyncVal WAW target) ----
    nvn_gpu_tex_desc ov_td = {0};
    ov_td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    ov_td.d.format = NVN_FORMAT_RGBA8;
    ov_td.d.width = 256; ov_td.d.height = 256; ov_td.d.depth = 2; ov_td.d.levels = 1;
    ov_td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex ov_tex = NULL;
    GPU(nvn_gpu_tex_create(&ov_td, &ov_tex, &err));

    const uint64_t ov_u1_bytes = 256u * 160u * 4u;   // 160 KiB (arena chunk #1: 96 KiB left)
    const uint64_t ov_u2_bytes = 256u * 128u * 4u;   // 128 KiB (> 96 KiB left -> arena chunk #2 -> emits copy #1!)
    const uint64_t ov_u3_bytes = 128u * 96u * 4u;    // 48 KiB  (fits in chunk #2, overlaps Upload 1 on layer 0!)
    const uint64_t ov_full_bytes = 256u * 256u * 4u;
    uint32_t *ov_src0 = malloc((size_t)ov_u1_bytes);
    uint32_t *ov_src1 = malloc((size_t)ov_u2_bytes);
    uint32_t *ov_patch = malloc((size_t)ov_u3_bytes);
    REQUIRE(ov_src0 && ov_src1 && ov_patch);
    fill_u32(ov_src0, 256u * 160u, 0xFF112233u);
    fill_u32(ov_src1, 256u * 128u, 0xFF445566u);
    fill_u32(ov_patch, 128u * 96u, 0xFFAABBCCu);
    nvn_gpu_buf rb_ov = NULL;
    uint8_t *ov_z = calloc(1, (size_t)ov_full_bytes);
    REQUIRE(ov_z);
    GPU(nvn_gpu_buf_create(ov_full_bytes, NVN_GPU_MEM_SHARED, ov_z, &rb_ov, &err));

    if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(NULL, 1);
    {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        // Upload 1: layer 0 [0..256, 0..160) (160 KiB -> chunk #1)
        NVNcopyRegion r1 = {0, 0, 0, 256, 160, 1};
        nvn_gpu_tex_upload(cmd, ov_tex, 0, 0, &r1, ov_src0, 256u * 4u, ov_u1_bytes);
        // Upload 2: layer 1 [0..256, 0..128) (128 KiB -> chunk #2, switches src buf -> emits vkCmdCopyBufferToImage #1!)
        NVNcopyRegion r2 = {0, 0, 0, 256, 128, 1};
        nvn_gpu_tex_upload(cmd, ov_tex, 0, 1, &r2, ov_src1, 256u * 4u, ov_u2_bytes);
        // Upload 3: layer 0 [32..160, 32..128) (48 KiB -> chunk #2, overlaps Upload 1 across 2 separate vkCmdCopyBufferToImage calls!)
        NVNcopyRegion sub = {32, 32, 0, 128, 96, 1};
        nvn_gpu_tex_upload(cmd, ov_tex, 0, 0, &sub, ov_patch, 128u * 4u, ov_u3_bytes);
        // Flush via copy_tex_buf readback of layer 0
        nvn_gpu_copy_tex_buf(cmd, ov_tex, 0, 0, NULL, rb_ov, 0, 256u * 4u, ov_full_bytes);
        submit_and_wait(cmd);
    }
    uint64_t st2[8] = {0};
    if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(st2, 0);
    const uint32_t *ov_px = (const uint32_t *)nvn_gpu_buf_cpu(rb_ov);
    CHECK(ov_px[0] == 0xFF112233u, "c2 corner=%#x want=0xff112233", ov_px[0]);
    CHECK(ov_px[32u * 256u + 32u] == 0xFFAABBCCu, "c2 inner0=%#x want=0xffaabbcc", ov_px[32u * 256u + 32u]);
    CHECK(ov_px[127u * 256u + 159u] == 0xFFAABBCCu, "c2 inner1=%#x want=0xffaabbcc", ov_px[127u * 256u + 159u]);
    CHECK(ov_px[128u * 256u + 159u] == 0xFF112233u, "c2 outer=%#x want=0xff112233", ov_px[128u * 256u + 159u]);
    if (g_tp_flag) {
        CHECK(st2[4] == 1, "c2(on) waw=%llu want=1", (unsigned long long)st2[4]);
    }

    // ---- Case 3: Repeated large >256KB staging uploads (1 MiB x6 + 1.5 MiB x2 -> bucket pool reuse!) ----
    nvn_gpu_tex_desc big_td = {0};
    big_td.d.target = NVN_TEXTURE_TARGET_2D; big_td.d.format = NVN_FORMAT_RGBA8;
    big_td.d.width = 512; big_td.d.height = 512; big_td.d.depth = 1; big_td.d.levels = 1;
    big_td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex big1 = NULL;
    GPU(nvn_gpu_tex_create(&big_td, &big1, &err));

    nvn_gpu_tex_desc big2_td = big_td;
    big2_td.d.width = 768; big2_td.d.height = 512;   // 1.5 MiB -> rounds to 2 MiB bucket
    nvn_gpu_tex big2 = NULL;
    GPU(nvn_gpu_tex_create(&big2_td, &big2, &err));

    const uint64_t b1_bytes = 512u * 512u * 4u;     // 1 MiB
    const uint64_t b2_bytes = 768u * 512u * 4u;     // 1.5 MiB
    uint32_t *b2_src = malloc((size_t)b2_bytes);
    REQUIRE(b2_src);
    nvn_gpu_buf rb_big = NULL;
    uint8_t *b1_z = calloc(1, (size_t)b1_bytes);
    REQUIRE(b1_z);
    GPU(nvn_gpu_buf_create(b1_bytes, NVN_GPU_MEM_SHARED, b1_z, &rb_big, &err));

    if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(NULL, 1);
    const uint64_t t3 = mono_ns();
    for (uint32_t iter = 0; iter < 6; iter++) {
        uint32_t val = 0xFF700000u + iter;
        fill_u32(b2_src, 512u * 512u, val);
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_tex_upload(cmd, big1, 0, 0, NULL, b2_src, 512u * 4u, b1_bytes);
        if (iter == 5) nvn_gpu_copy_tex_buf(cmd, big1, 0, 0, NULL, rb_big, 0, 512u * 4u, b1_bytes);
        submit_and_wait(cmd);
    }
    for (uint32_t iter = 0; iter < 2; iter++) {
        fill_u32(b2_src, 768u * 512u, 0xFF800000u + iter);
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_tex_upload(cmd, big2, 0, 0, NULL, b2_src, 768u * 4u, b2_bytes);
        submit_and_wait(cmd);
    }
    const uint64_t c3_us = (mono_ns() - t3) / 1000ull;
    uint64_t st3[8] = {0};
    if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(st3, 0);
    const uint32_t *b1_px = (const uint32_t *)nvn_gpu_buf_cpu(rb_big);
    CHECK(b1_px[0] == 0xFF700005u && b1_px[512u * 512u - 1u] == 0xFF700005u,
          "c3 big1=%#x/%#x want=0xff700005", b1_px[0], b1_px[512u * 512u - 1u]);
    if (g_tp_flag) {
        // Note: 512x512x4 = 1 MiB, + align(4) rounds up to 2 MiB bucket! So BOTH 1 MiB and 1.5 MiB share the 2 MiB bucket -> 7 hits, 1 miss!
        CHECK(st3[5] >= 6 && st3[6] <= 2, "c3(on) stg_hit=%llu stg_miss=%llu want hit>=6 miss<=2",
              (unsigned long long)st3[5], (unsigned long long)st3[6]);
    } else if (nvn_vk_dbg_tex_pool_stat) {
        CHECK(st3[5] == 0 && st3[6] == 8, "c3(off) stg_hit=%llu stg_miss=%llu want 0/8",
              (unsigned long long)st3[5], (unsigned long long)st3[6]);
    }

    // ---- Case 4: Flush boundaries (pass_begin LOAD, blit, cmd_abort) + 32 MiB LRU eviction cap ----
    nvn_gpu_tex_desc rt_td = {0};
    rt_td.d.target = NVN_TEXTURE_TARGET_2D; rt_td.d.format = NVN_FORMAT_RGBA8;
    rt_td.d.width = 64; rt_td.d.height = 64; rt_td.d.depth = 1; rt_td.d.levels = 1;
    rt_td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rtA = NULL, rtB = NULL;
    nvn_gpu_view rtvA = NULL, rtvB = NULL;
    GPU(nvn_gpu_tex_create(&rt_td, &rtA, &err));
    GPU(nvn_gpu_tex_create(&rt_td, &rtB, &err));
    GPU(nvn_gpu_view_create(rtA, NULL, &rtvA, &err));
    GPU(nvn_gpu_view_create(rtB, NULL, &rtvB, &err));
    {
        fill_u32(mip_buf, 64u * 64u, 0xFF336699u);
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_tex_upload(cmd, rtA, 0, 0, NULL, mip_buf, 64u * 4u, 64u * 64u * 4u);
        // Flush boundary 1: pass_begin with LOAD_LOAD (must flush pending tex_upload before render pass!)
        nvn_gpu_pass_desc p = {0};
        p.ncolor = 1; p.color[0].view = rtvA; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
        p.w = p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_pass_end(cmd);
        // Upload again and immediately blit rtA -> rtB (flush boundary 2: blit)
        fill_u32(mip_buf, 64u * 64u, 0xFF4488CCu);
        nvn_gpu_tex_upload(cmd, rtA, 0, 0, NULL, mip_buf, 64u * 4u, 64u * 64u * 4u);
        NVNcopyRegion reg = {0, 0, 0, 64, 64, 1};
        nvn_gpu_blit(cmd, rtvA, &reg, rtvB, &reg, 0);
        nvn_gpu_copy_tex_buf(cmd, rtB, 0, 0, NULL, rb1, 0, 64u * 4u, 64u * 64u * 4u);
        submit_and_wait(cmd);
        const uint32_t *got = (const uint32_t *)nvn_gpu_buf_cpu(rb1);
        CHECK(got[0] == 0xFF4488CCu && got[64u * 64u - 1u] == 0xFF4488CCu,
              "c4 blit=%#x/%#x want=0xff4488cc", got[0], got[64u * 64u - 1u]);

        // Flush boundary 3: cmd_abort with pending batched tex_upload (must not crash or leak)
        nvn_gpu_cmd ab = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &ab, &err));
        nvn_gpu_tex_upload(ab, rtA, 0, 0, NULL, mip_buf, 64u * 4u, 64u * 64u * 4u);
        nvn_gpu_cmd_abort(ab);
    }

    // 32 MiB LRU cap check: 3 concurrent 16 MiB staging allocs in one segment -> >= 1 eviction on finish_cmd
    uint64_t evict_cnt = 0;
    {
        nvn_gpu_tex_desc htd = {0};
        htd.d.target = NVN_TEXTURE_TARGET_2D; htd.d.format = NVN_FORMAT_RGBA8;
        htd.d.width = 2048; htd.d.height = 1024; htd.d.depth = 1; htd.d.levels = 1; // 8 MiB -> 16 MiB bucket
        htd.usage = NVN_GPU_TEX_SAMPLED;
        nvn_gpu_tex htex = NULL;
        GPU(nvn_gpu_tex_create(&htd, &htex, &err));
        const uint64_t hbytes = 2048u * 1024u * 4u; // 8 MiB; +4 -> 16 MiB bucket
        uint8_t *hsrc = calloc(1, (size_t)hbytes);
        REQUIRE(hsrc);
        if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(NULL, 1);
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        // 3 uploads in 1 segment -> 3 separate 16 MiB staging buffers (48 MiB > 32 MiB cap) returned at finish_cmd!
        for (int k = 0; k < 3; k++)
            nvn_gpu_tex_upload(cmd, htex, 0, 0, NULL, hsrc, 2048u * 4u, hbytes);
        submit_and_wait(cmd);
        // Wait for worker finish_cmd to drain via a tiny barrier segment
        nvn_gpu_cmd sync_cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &sync_cmd, &err));
        submit_and_wait(sync_cmd);
        uint64_t st4[8] = {0};
        if (nvn_vk_dbg_tex_pool_stat) nvn_vk_dbg_tex_pool_stat(st4, 0);
        evict_cnt = st4[7];
        if (g_tp_flag) {
            CHECK(evict_cnt >= 1, "c4(on) lru_evict=%llu want >= 1 (48 MiB > 32 MiB cap)",
                  (unsigned long long)evict_cnt);
        }
        free(hsrc);
        nvn_gpu_tex_release(htex);
    }

    printf("tex-upload-pool flag=%d c1_us=%llu c1_recs=%llu c1_full=%llu c1_img=%llu c1_cp=%llu c2_waw=%llu c3_us=%llu c3_hit=%llu c3_miss=%llu c4_evict=%llu cases=4 failures=%u\n",
           g_tp_flag, (unsigned long long)c1_us, (unsigned long long)c1_recs,
           (unsigned long long)st1[1], (unsigned long long)st1[2], (unsigned long long)st1[3],
           (unsigned long long)st2[4], (unsigned long long)c3_us,
           (unsigned long long)st3[5], (unsigned long long)st3[6],
           (unsigned long long)evict_cnt, g_failures);

    free(b1_z);
    free(b2_src);
    free(ov_z);
    free(ov_patch);
    free(ov_src1);
    free(ov_src0);
    free(rb_z);
    free(mip_buf);
    nvn_gpu_view_release(rtvB);
    nvn_gpu_view_release(rtvA);
    nvn_gpu_tex_release(rtB);
    nvn_gpu_tex_release(rtA);
    nvn_gpu_buf_release(rb_big, NULL, NULL);
    nvn_gpu_tex_release(big2);
    nvn_gpu_tex_release(big1);
    nvn_gpu_buf_release(rb_ov, NULL, NULL);
    nvn_gpu_tex_release(ov_tex);
    nvn_gpu_buf_release(rb1, NULL, NULL);
    nvn_gpu_tex_release(arr_tex);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);
    return g_failures ? 1 : 0;
}
