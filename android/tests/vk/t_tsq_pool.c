// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract & benchmark fixture for timestamp VkQueryPool recycling (debug.s2a.nvk_tsq_pool):
//   Uses -Wl,--wrap=vkCreateQueryPool to count real driver VkQueryPool allocations directly!
//   1) 32 sequential segments with nvk_gputime=1 (c->tsq 258-slot timestamp pool + pass timestamps):
//      flag=0: creates >= 32 timestamp pools; flag=1: creates <= 4 pools (hit >= 28), 0 VVL / 0 SyncVal
//   2) 4 concurrent in-flight segments opened simultaneously before commit, followed by 8 sequential segments:
//      verifies no pool aliasing across in-flight command buffers, 0 VVL / 0 SyncVal
//   3) Aborted segments (nvn_gpu_cmd_abort) return timestamp pools cleanly after DQ drain
//   4) Report timestamp (nvn_gpu_timestamp 64-slot pool) reuse across 8 segments when calibrated timestamps supported
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void nvn_vk_dbg_tsq_pool_stat(uint64_t out[4], int reset) __attribute__((weak));

// Real linker wrap on vkCreateQueryPool to measure actual driver creations independently of internal stats!
extern VkResult __real_vkCreateQueryPool(VkDevice dev, const VkQueryPoolCreateInfo *ci,
                                         const VkAllocationCallbacks *alloc, VkQueryPool *out);
static _Atomic uint64_t g_wrap_tsq_creates = 0;
VkResult __wrap_vkCreateQueryPool(VkDevice dev, const VkQueryPoolCreateInfo *ci,
                                  const VkAllocationCallbacks *alloc, VkQueryPool *out) {
    if (ci && ci->queryType == VK_QUERY_TYPE_TIMESTAMP)
        atomic_fetch_add_explicit(&g_wrap_tsq_creates, 1, memory_order_relaxed);
    return __real_vkCreateQueryPool(dev, ci, alloc, out);
}

static nvn_gpu_err err;
static int g_tsq_flag = 1;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "tsq-pool fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
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
    if (!strcmp(n, "nvk_gputime")) return 1;
    if (!strcmp(n, "nvk_tsq_pool")) return g_tsq_flag;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    (void)l;
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static _Atomic uint64_t g_report_valid = 0;
static void on_done_ok(void *u, const nvn_gpu_done *d) {
    (void)u;
    REQUIRE(d->status == NVN_GPU_DONE_OK);
    if (d->ntimestamps > 0 && d->timestamps && d->timestamps[0].valid)
        atomic_fetch_add_explicit(&g_report_valid, 1, memory_order_relaxed);
}

static void on_done_probe(void *u, const nvn_gpu_done *d) {
    (void)u;
    (void)d;
}

static void submit_and_wait(nvn_gpu_cmd cmd) {
    uint64_t s = ++g_sig;
    GPU(nvn_gpu_cmd_commit(cmd, s, on_done_ok, NULL, &err));
    GPU(nvn_gpu_fence_wait(s, 10000000000ull, &err));
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 2);
    g_tsq_flag = atoi(argv[1]);

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = { .host_device = &dev };
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 4; td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err));
    GPU(nvn_gpu_view_create(rt, NULL, &rtv, &err));

    // ---- Case 1: 32 sequential segments with nvk_gputime=1 + render pass timestamps ----
    if (nvn_vk_dbg_tsq_pool_stat) nvn_vk_dbg_tsq_pool_stat(NULL, 1);
    atomic_store_explicit(&g_wrap_tsq_creates, 0, memory_order_relaxed);
    const uint64_t t0 = mono_ns();
    for (int i = 0; i < 32; i++) {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_pass_desc p = {0};
        p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.w = p.h = 4; p.layers = 1;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_pass_end(cmd);
        submit_and_wait(cmd);
    }
    const uint64_t c1_us = (mono_ns() - t0) / 1000ull;
    const uint64_t c1_creates = atomic_load_explicit(&g_wrap_tsq_creates, memory_order_relaxed);
    uint64_t st1[4] = {0};
    if (nvn_vk_dbg_tsq_pool_stat) nvn_vk_dbg_tsq_pool_stat(st1, 0);

    if (g_tsq_flag) {
        CHECK(nvn_vk_dbg_tsq_pool_stat != NULL, "c1(on) stat hook missing");
        CHECK(c1_creates <= 4, "c1(on) vkCreateQueryPool=%llu want <= 4 (32 segs reused)",
              (unsigned long long)c1_creates);
        CHECK(st1[0] >= 28 && st1[1] <= 4, "c1(on) hit=%llu miss=%llu want hit>=28 miss<=4",
              (unsigned long long)st1[0], (unsigned long long)st1[1]);
    } else {
        CHECK(c1_creates == 32, "c1(off) vkCreateQueryPool=%llu want == 32",
              (unsigned long long)c1_creates);
    }

    // ---- Case 2: 4 concurrent in-flight segments opened simultaneously before commit, then 8 sequential ----
    atomic_store_explicit(&g_wrap_tsq_creates, 0, memory_order_relaxed);
    {
        nvn_gpu_cmd cmds[4] = {0};
        for (int i = 0; i < 4; i++) {
            GPU(nvn_gpu_cmd_begin(NULL, &cmds[i], &err));
            nvn_gpu_pass_desc p = {0};
            p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            p.w = p.h = 4; p.layers = 1;
            nvn_gpu_pass_begin(cmds[i], &p);
            nvn_gpu_pass_end(cmds[i]);
        }
        uint64_t last_sig = 0;
        for (int i = 0; i < 4; i++) {
            last_sig = ++g_sig;
            GPU(nvn_gpu_cmd_commit(cmds[i], last_sig, on_done_ok, NULL, &err));
        }
        GPU(nvn_gpu_fence_wait(last_sig, 10000000000ull, &err));
        // 8 more sequential segments must reuse the 4 pooled pools without new allocations
        for (int i = 0; i < 8; i++) {
            nvn_gpu_cmd cmd = NULL;
            GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
            submit_and_wait(cmd);
        }
    }
    const uint64_t c2_creates = atomic_load_explicit(&g_wrap_tsq_creates, memory_order_relaxed);
    if (g_tsq_flag) {
        CHECK(c2_creates <= 4, "c2(on) creates=%llu want <= 4 (4 concurrent + 8 reused)",
              (unsigned long long)c2_creates);
    } else {
        CHECK(c2_creates == 12, "c2(off) creates=%llu want == 12", (unsigned long long)c2_creates);
    }

    // ---- Case 3: Aborted segments return timestamp pools cleanly ----
    atomic_store_explicit(&g_wrap_tsq_creates, 0, memory_order_relaxed);
    for (int i = 0; i < 8; i++) {
        nvn_gpu_cmd ab = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &ab, &err));
        nvn_gpu_cmd_abort(ab);
    }
    const uint64_t c3_creates = atomic_load_explicit(&g_wrap_tsq_creates, memory_order_relaxed);
    if (g_tsq_flag) {
        CHECK(c3_creates == 0, "c3(on) abort creates=%llu want == 0", (unsigned long long)c3_creates);
    } else {
        CHECK(c3_creates == 8, "c3(off) abort creates=%llu want == 8", (unsigned long long)c3_creates);
    }

    // ---- Case 4: Report timestamp (nvn_gpu_timestamp 64-slot pool) reuse if device supports calibrated timestamps ----
    uint64_t c4_creates = 0;
    int c4_supported = 0;
    {
        nvn_gpu_cmd probe = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &probe, &err));
        nvn_gpu_timestamp(probe, 0, NVN_COUNTER_TYPE_TIMESTAMP_TOP);
        nvn_gpu_timestamp(probe, 1, NVN_COUNTER_TYPE_TIMESTAMP);
        nvn_gpu_err pe = {0};
        uint64_t ps = ++g_sig;
        nvn_gpu_rc pr = nvn_gpu_cmd_commit(probe, ps, on_done_probe, NULL, &pe);
        if (nvn_gpu_ok(pr)) {
            GPU(nvn_gpu_fence_wait(ps, 10000000000ull, &err));
            c4_supported = 1;
            atomic_store_explicit(&g_wrap_tsq_creates, 0, memory_order_relaxed);
            for (int i = 0; i < 8; i++) {
                nvn_gpu_cmd cmd = NULL;
                GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
                nvn_gpu_timestamp(cmd, 0, NVN_COUNTER_TYPE_TIMESTAMP_TOP);
                nvn_gpu_timestamp(cmd, 1, NVN_COUNTER_TYPE_TIMESTAMP);
                submit_and_wait(cmd);
            }
            c4_creates = atomic_load_explicit(&g_wrap_tsq_creates, memory_order_relaxed);
            if (g_tsq_flag) {
                CHECK(c4_creates <= 2, "c4(on) report_ts creates=%llu want <= 2", (unsigned long long)c4_creates);
            } else {
                CHECK(c4_creates == 16, "c4(off) report_ts creates=%llu want == 16 (8 tsq + 8 report_ts)",
                      (unsigned long long)c4_creates);
            }
        }
    }

    printf("tsq-pool flag=%d c1_us=%llu c1_creates=%llu c1_hit=%llu c1_miss=%llu c2_creates=%llu c3_creates=%llu c4_sup=%d c4_creates=%llu cases=4 failures=%u\n",
           g_tsq_flag, (unsigned long long)c1_us, (unsigned long long)c1_creates,
           (unsigned long long)st1[0], (unsigned long long)st1[1],
           (unsigned long long)c2_creates, (unsigned long long)c3_creates,
           c4_supported, (unsigned long long)c4_creates, g_failures);

    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);
    return g_failures ? 1 : 0;
}
