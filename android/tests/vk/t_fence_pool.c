// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract & wrap-observer fixture for VkFence segment/present pooling (debug.s2a.nvk_fence_pool):
//   1) Sequential 64 GPU commits: steady-state vkCreateFence 64->1, vkDestroyFence 64->0, vkResetFences 0->63
//   2) Pipelined 8-in-flight burst (64 GPU commits): bounded creates <= 8, resets >= 56, exact GPU buffer results
//   3) Pool cap overflow (> SEGPOOL_FENCE_N=64 via delay_segment): respects 64-fence cap, destroys excess, 0 leaks
//   4) Fence-fail path + nvn_gpu_close drain + session reopen: failed fence destroyed (never pooled), total_create==total_destroy
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void nvn_vk_dbg_fence_pool_stat(uint64_t *hit, uint64_t *miss, uint64_t *kept, uint64_t *full, uint32_t *cur) __attribute__((weak));
extern void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms);
extern void nvn_gpu_vk_test_fail_fence(uint64_t signal);

// Linker-wrapped real Vulkan fence lifecycle counters (-Wl,--wrap=vkCreateFence,...)
extern VkResult __real_vkCreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *);
extern void __real_vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *);
extern VkResult __real_vkResetFences(VkDevice, uint32_t, const VkFence *);

static _Atomic uint64_t g_vk_create_fence;
static _Atomic uint64_t g_vk_destroy_fence;
static _Atomic uint64_t g_vk_reset_fences;

VkResult __wrap_vkCreateFence(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    VkResult r = __real_vkCreateFence(d, ci, a, out);
    if (r == VK_SUCCESS) atomic_fetch_add_explicit(&g_vk_create_fence, 1, memory_order_relaxed);
    return r;
}

void __wrap_vkDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) {
    if (f != VK_NULL_HANDLE) atomic_fetch_add_explicit(&g_vk_destroy_fence, 1, memory_order_relaxed);
    __real_vkDestroyFence(d, f, a);
}

VkResult __wrap_vkResetFences(VkDevice d, uint32_t n, const VkFence *p) {
    VkResult r = __real_vkResetFences(d, n, p);
    if (r == VK_SUCCESS) atomic_fetch_add_explicit(&g_vk_reset_fences, n, memory_order_relaxed);
    return r;
}

static nvn_gpu_err err;
static int g_dq_mode = 0;
static int g_pool_flag = 1;
static int g_wsi_test, g_timeline_flag, g_wsi_probe;
static unsigned g_wsi_fenced, g_wsi_timeline;
static uint64_t g_wsi_last_timeline;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;
static _Atomic uint32_t g_done_ok;
static _Atomic uint32_t g_done_fail;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "fence-pool fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL " __VA_ARGS__); fprintf(stderr, "\n"); g_failures++; } } while (0)

extern __typeof__(vkQueueSubmit) __real_vkQueueSubmit;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *si, VkFence fence) {
    if (g_wsi_probe) {
        REQUIRE(count == 1 && si->waitSemaphoreCount == 1 && si->commandBufferCount != 0);
        if (g_timeline_flag) {
            const VkTimelineSemaphoreSubmitInfoKHR *tl = si->pNext;
            REQUIRE(!fence && si->signalSemaphoreCount == 2 && tl &&
                    tl->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR &&
                    tl->waitSemaphoreValueCount == 1 && tl->pWaitSemaphoreValues[0] == 0 &&
                    tl->signalSemaphoreValueCount == 2 && tl->pSignalSemaphoreValues[0] == 0 &&
                    tl->pSignalSemaphoreValues[1] > g_wsi_last_timeline);
            g_wsi_last_timeline = tl->pSignalSemaphoreValues[1];
            g_wsi_timeline++;
        } else {
            REQUIRE(fence && !si->pNext && si->signalSemaphoreCount == 1);
            g_wsi_fenced++;
        }
    }
    return __real_vkQueueSubmit(queue, count, si, fence);
}

// ---- Platform stubs ----
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) {
    static const char *const exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    *n = g_wsi_test ? 2 : 0;
    return g_wsi_test ? exts : NULL;
}
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    (void)w;
    *s = VK_NULL_HANDLE;
    PFN_vkCreateHeadlessSurfaceEXT create = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(i, "vkCreateHeadlessSurfaceEXT");
    if (!g_wsi_test || !create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkHeadlessSurfaceCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    return create(i, &ci, NULL, s);
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) {
    (void)w;
    *x = 64;
    *y = 64;
    return g_wsi_test;
}
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_mode;
    if (!strcmp(n, "nvk_fence_pool")) return g_pool_flag;
    if (!strcmp(n, "nvk_timeline_fence")) return g_timeline_flag;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    (void)l;
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done_ok(void *u, const nvn_gpu_done *d) {
    (void)u;
    if (d->status == NVN_GPU_DONE_OK) atomic_fetch_add_explicit(&g_done_ok, 1, memory_order_relaxed);
    else {
        fprintf(stderr, "FAIL unexpected done status=%d rc=%d why=%s\n", (int)d->status, d->rc.code, d->why ? d->why : "");
        g_failures++;
    }
}

static void on_done_expect_fail(void *u, const nvn_gpu_done *d) {
    (void)u;
    if (d->status == NVN_GPU_DONE_FAILED) atomic_fetch_add_explicit(&g_done_fail, 1, memory_order_relaxed);
    else {
        fprintf(stderr, "FAIL expected DONE_FAILED got status=%d\n", (int)d->status);
        g_failures++;
    }
}

typedef struct {
    uint64_t creates;
    uint64_t destroys;
    uint64_t resets;
} wrap_snap;

static wrap_snap snap_wrap(void) {
    wrap_snap s;
    s.creates = atomic_load_explicit(&g_vk_create_fence, memory_order_seq_cst);
    s.destroys = atomic_load_explicit(&g_vk_destroy_fence, memory_order_seq_cst);
    s.resets = atomic_load_explicit(&g_vk_reset_fences, memory_order_seq_cst);
    return s;
}

static uint64_t submit_fill(nvn_gpu_buf buf, uint64_t off, uint32_t val, nvn_gpu_done_fn cb) {
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, buf, off, 4, val);
    uint64_t s = ++g_sig;
    GPU(nvn_gpu_cmd_commit(cmd, s, cb, NULL, &err));
    return s;
}

static void run_wsi(void) {
    enum { frames = 16 };
    nvn_gpu_surface sf = NULL;
    GPU(nvn_gpu_surface_attach((void *)1, &sf, &err));
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 64;
    td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex tex = NULL;
    nvn_gpu_view view = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    uint8_t pixels[64 * 64 * 4];
    memset(pixels, 128, sizeof pixels);
    const wrap_snap before_upload = snap_wrap();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_tex_upload(cmd, tex, 0, 0, NULL, pixels, 64 * 4, sizeof pixels);
    GPU(nvn_gpu_cmd_commit(cmd, 1, on_done_ok, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    wrap_snap before = snap_wrap();
    const nvn_gpu_present_desc p = {.src = view};
    for (unsigned i = 0; i < frames; i++) {
        nvn_gpu_drawable dr = {0};
        GPU(nvn_gpu_drawable_acquire(sf, 5000000000ull, NULL, &dr, &err));
        g_wsi_probe = 1;
        nvn_gpu_rc r = nvn_gpu_present(sf, &dr, &p, i + 1, NULL, NULL, &err);
        g_wsi_probe = 0;
        GPU(r);
    }
    const wrap_snap after = snap_wrap();
    GPU(nvn_gpu_surface_detach(sf, 5000000000ull, &err));
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    GPU(nvn_gpu_close(5000000000ull, &err));
    const wrap_snap closed = snap_wrap();
    const uint64_t creates = after.creates - before.creates;
    const uint64_t resets = after.resets - before.resets;
    CHECK(closed.creates == closed.destroys, "wsi: unbalanced create/destroy after close");
    if (g_timeline_flag) {
        CHECK(g_wsi_timeline == frames && !g_wsi_fenced && after.creates == before_upload.creates &&
              after.resets == before_upload.resets,
              "wsi timeline: upload and presents must use zero VkFence objects");
    } else {
        CHECK(g_wsi_fenced == frames && !g_wsi_timeline && resets > 0 && creates + resets == frames,
              "wsi binary: all presents must submit a fence and reuse at least one");
    }
    printf("fence-pool WSI dq=%d flag=%d tl=%d frames=%u fenced=%u timeline=%u creates=%llu resets=%llu failures=%u\n",
           g_dq_mode, g_pool_flag, g_timeline_flag, frames, g_wsi_fenced, g_wsi_timeline,
           (unsigned long long)creates, (unsigned long long)resets, g_failures);
}

int main(int argc, char **argv) {
    if (argc >= 2) g_dq_mode = atoi(argv[1]);
    if (argc >= 3) g_pool_flag = atoi(argv[2]);
    if (argc >= 4) { g_wsi_test = 1; g_timeline_flag = atoi(argv[3]); }

    struct s2a_vk_device dev;
    memset(&dev, 0, sizeof dev);
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);

    nvn_gpu_open_desc od;
    memset(&od, 0, sizeof od);
    od.host_device = &dev;
    GPU(nvn_gpu_open(&od, &err));
    if (g_wsi_test) {
        run_wsi();
        REQUIRE(!s2a_vk_device_destroy(&dev, 1));
        return g_failures ? 1 : 0;
    }

    // Shared GPU buffer (512 u32 words = 2048 bytes) for verifying real GPU completion per segment
    nvn_gpu_buf buf = NULL;
    GPU(nvn_gpu_buf_create(2048, NVN_GPU_MEM_SHARED, NULL, &buf, &err));
    volatile uint32_t *cpu = (volatile uint32_t *)nvn_gpu_buf_cpu(buf);
    REQUIRE(cpu != NULL);
    memset((void *)(uintptr_t)cpu, 0, 2048);

    // ---- Case 1: 64 sequential GPU commits ----
    wrap_snap s0 = snap_wrap();
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t want = 0xA0000000u + i;
        uint64_t sig = submit_fill(buf, (uint64_t)i * 4ull, want, on_done_ok);
        GPU(nvn_gpu_fence_wait(sig, 10000000000ull, &err));
        CHECK(cpu[i] == want, "c1[%u] got=0x%08x want=0x%08x", i, cpu[i], want);
    }
    wrap_snap s1 = snap_wrap();
    uint64_t c1_create = s1.creates - s0.creates;
    uint64_t c1_destroy = s1.destroys - s0.destroys;
    uint64_t c1_reset = s1.resets - s0.resets;

    if (g_pool_flag) {
        CHECK(c1_create == 1, "c1(on) create=%llu want=1", (unsigned long long)c1_create);
        CHECK(c1_reset == 63, "c1(on) reset=%llu want=63", (unsigned long long)c1_reset);
        CHECK(c1_destroy == 0, "c1(on) steady destroy=%llu want=0", (unsigned long long)c1_destroy);
    } else {
        CHECK(c1_create == 64, "c1(off) create=%llu want=64", (unsigned long long)c1_create);
        CHECK(c1_reset == 0, "c1(off) reset=%llu want=0", (unsigned long long)c1_reset);
        CHECK(c1_destroy >= 63 && c1_destroy <= 64, "c1(off) destroy=%llu want=63..64", (unsigned long long)c1_destroy);
    }

    // ---- Case 2: Pipelined 8-in-flight burst (64 GPU commits) ----
    wrap_snap s2a = snap_wrap();
    uint64_t pipe_sigs[64];
    for (uint32_t i = 0; i < 64; i++) {
        pipe_sigs[i] = submit_fill(buf, (uint64_t)(64 + i) * 4ull, 0xB0000000u + i, on_done_ok);
        if (i >= 7) GPU(nvn_gpu_fence_wait(pipe_sigs[i - 7], 10000000000ull, &err));
    }
    GPU(nvn_gpu_fence_wait(pipe_sigs[63], 10000000000ull, &err));
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t want = 0xB0000000u + i;
        CHECK(cpu[64 + i] == want, "c2[%u] got=0x%08x want=0x%08x", i, cpu[64 + i], want);
    }
    wrap_snap s2b = snap_wrap();
    uint64_t c2_create = s2b.creates - s2a.creates;
    uint64_t c2_destroy = s2b.destroys - s2a.destroys;
    uint64_t c2_reset = s2b.resets - s2a.resets;

    if (g_pool_flag) {
        CHECK(c2_create <= 8, "c2(on) create=%llu want<=8", (unsigned long long)c2_create);
        CHECK(c2_reset >= 56, "c2(on) reset=%llu want>=56", (unsigned long long)c2_reset);
        CHECK(c2_destroy == 0, "c2(on) steady destroy=%llu want=0", (unsigned long long)c2_destroy);
    } else {
        CHECK(c2_create == 64, "c2(off) create=%llu want=64", (unsigned long long)c2_create);
        CHECK(c2_reset == 0, "c2(off) reset=%llu want=0", (unsigned long long)c2_reset);
    }

    // ---- Case 3: Pool cap overflow (> 64 simultaneously in-flight via delay_segment) ----
    uint64_t first_burst_sig = g_sig + 1;
    nvn_gpu_vk_test_delay_segment(first_burst_sig, 200);
    uint64_t burst_sigs[72];
    for (uint32_t i = 0; i < 72; i++) {
        burst_sigs[i] = submit_fill(buf, (uint64_t)(128 + i) * 4ull, 0xC0000000u + i, on_done_ok);
    }
    GPU(nvn_gpu_fence_wait(burst_sigs[71], 10000000000ull, &err));
    nvn_gpu_vk_test_delay_segment(0, 0);
    for (uint32_t i = 0; i < 72; i++) {
        uint32_t want = 0xC0000000u + i;
        CHECK(cpu[128 + i] == want, "c3[%u] got=0x%08x want=0x%08x", i, cpu[128 + i], want);
    }
    uint64_t st_hit = 0, st_miss = 0, st_kept = 0, st_full = 0;
    uint32_t st_cur = 0;
    if (nvn_vk_dbg_fence_pool_stat) {
        nvn_vk_dbg_fence_pool_stat(&st_hit, &st_miss, &st_kept, &st_full, &st_cur);
    }
    if (g_pool_flag) {
        CHECK(nvn_vk_dbg_fence_pool_stat != NULL, "c3(on) stat hook missing");
        CHECK(st_cur <= 64, "c3(on) cur=%u exceeds cap 64", st_cur);
        CHECK(st_full >= 1, "c3(on) expected pool cap overflow drops >= 1 got=%llu", (unsigned long long)st_full);
        // After cap saturation, 16 sequential commits must take 0 new creates and 16 resets
        wrap_snap s3a = snap_wrap();
        for (uint32_t i = 0; i < 16; i++) {
            uint64_t sig = submit_fill(buf, (uint64_t)(200 + i) * 4ull, 0xD0000000u + i, on_done_ok);
            GPU(nvn_gpu_fence_wait(sig, 10000000000ull, &err));
            CHECK(cpu[200 + i] == 0xD0000000u + i, "c3post[%u] mismatch", i);
        }
        wrap_snap s3b = snap_wrap();
        CHECK(s3b.creates - s3a.creates == 0, "c3post(on) create=%llu want=0", (unsigned long long)(s3b.creates - s3a.creates));
        CHECK(s3b.resets - s3a.resets == 16, "c3post(on) reset=%llu want=16", (unsigned long long)(s3b.resets - s3a.resets));
    }

    // ---- Case 4: Fence-fail path (must destroy fence, never pool) + close drain + session reopen ----
    uint64_t fail_sig = g_sig + 1;
    wrap_snap sf0 = snap_wrap();
    nvn_gpu_vk_test_fail_fence(fail_sig);
    (void)submit_fill(buf, 220 * 4ull, 0xE0000000u, on_done_expect_fail);
    nvn_gpu_rc wrc = nvn_gpu_fence_wait(fail_sig, 10000000000ull, &err);
    CHECK(!nvn_gpu_ok(wrc), "c4 fail_fence wait unexpectedly succeeded");
    nvn_gpu_vk_test_fail_fence(0);
    // Close session 1: worker joins, seg_drain destroys all pooled fences
    GPU(nvn_gpu_close(10000000000ull, &err));
    wrap_snap sc1 = snap_wrap();
    CHECK(atomic_load_explicit(&g_done_fail, memory_order_seq_cst) == 1, "c4 done_fail=%u want=1",
          atomic_load_explicit(&g_done_fail, memory_order_seq_cst));
    CHECK(sc1.destroys > sf0.destroys, "c4 failed fence / close drain must destroy fences");
    CHECK(sc1.creates == sc1.destroys, "c4 session1 leak: creates=%llu destroys=%llu",
          (unsigned long long)sc1.creates, (unsigned long long)sc1.destroys);
    if (nvn_vk_dbg_fence_pool_stat) {
        uint32_t cur_after = 999;
        nvn_vk_dbg_fence_pool_stat(NULL, NULL, NULL, NULL, &cur_after);
        CHECK(cur_after == 0, "c4 cur_after_close=%u want=0", cur_after);
    }

    // Reopen session 2 and verify clean reuse from empty pool + clean drain on close
    g_sig = 0;
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_buf buf2 = NULL;
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, NULL, &buf2, &err));
    volatile uint32_t *cpu2 = (volatile uint32_t *)nvn_gpu_buf_cpu(buf2);
    REQUIRE(cpu2 != NULL);
    memset((void *)(uintptr_t)cpu2, 0, 256);

    wrap_snap sr0 = snap_wrap();
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t want = 0xF0000000u + i;
        uint64_t sig = submit_fill(buf2, (uint64_t)i * 4ull, want, on_done_ok);
        GPU(nvn_gpu_fence_wait(sig, 10000000000ull, &err));
        CHECK(cpu2[i] == want, "c4reopen[%u] got=0x%08x want=0x%08x", i, cpu2[i], want);
    }
    wrap_snap sr1 = snap_wrap();
    uint64_t r_create = sr1.creates - sr0.creates;
    uint64_t r_reset = sr1.resets - sr0.resets;
    if (g_pool_flag) {
        CHECK(r_create == 1, "c4reopen(on) create=%llu want=1", (unsigned long long)r_create);
        CHECK(r_reset == 15, "c4reopen(on) reset=%llu want=15", (unsigned long long)r_reset);
    } else {
        CHECK(r_create == 16, "c4reopen(off) create=%llu want=16", (unsigned long long)r_create);
        CHECK(r_reset == 0, "c4reopen(off) reset=%llu want=0", (unsigned long long)r_reset);
    }

    GPU(nvn_gpu_close(10000000000ull, &err));
    wrap_snap sc2 = snap_wrap();
    CHECK(sc2.creates == sc2.destroys, "c4 session2 leak: creates=%llu destroys=%llu",
          (unsigned long long)sc2.creates, (unsigned long long)sc2.destroys);

    s2a_vk_device_destroy(&dev, 1);

    printf("fence-pool dq=%d flag=%d c1(cr=%llu,ds=%llu,rs=%llu) c2(cr=%llu,ds=%llu,rs=%llu) "
           "cap(hit=%llu,miss=%llu,kept=%llu,full=%llu,cur=%u) reopen(cr=%llu,rs=%llu) "
           "total(cr=%llu,ds=%llu) cases=4 failures=%u\n",
           g_dq_mode, g_pool_flag,
           (unsigned long long)c1_create, (unsigned long long)c1_destroy, (unsigned long long)c1_reset,
           (unsigned long long)c2_create, (unsigned long long)c2_destroy, (unsigned long long)c2_reset,
           (unsigned long long)st_hit, (unsigned long long)st_miss, (unsigned long long)st_kept,
           (unsigned long long)st_full, st_cur,
           (unsigned long long)r_create, (unsigned long long)r_reset,
           (unsigned long long)sc2.creates, (unsigned long long)sc2.destroys,
           g_failures);
    return g_failures ? 1 : 0;
}
