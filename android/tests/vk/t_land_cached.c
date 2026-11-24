// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_backend.h"
#include "nvn_plat.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define GPU(x) do { nvn_gpu_rc result_ = (x); if (!nvn_gpu_ok(result_)) { fprintf(stderr, "FAIL %d: %s: %s\n", __LINE__, #x, err.why); exit(1); } } while (0)
#define WAIT_NS 5000000000ull

static int dq, enabled, fail_invalidate;

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    *s = VK_NULL_HANDLE;
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *key, int fallback) {
    if (!strcmp(key, "nvk_dq")) return dq;
    if (!strcmp(key, "nvk_land_cached")) return enabled;
    return fallback;
}
void nvn_plat_log(int level, const char *category, const char *line) {
    if (line) fprintf(stderr, "[%s] %s\n", category, line);
}

extern __typeof__(vkInvalidateMappedMemoryRanges) __real_vkInvalidateMappedMemoryRanges;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t count,
                                                                    const VkMappedMemoryRange *ranges) {
    const VkResult result = __real_vkInvalidateMappedMemoryRanges(device, count, ranges);
    return fail_invalidate && result == VK_SUCCESS ? VK_ERROR_OUT_OF_HOST_MEMORY : result;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkInvalidateMappedMemoryRanges),
                                           PFN_vkInvalidateMappedMemoryRanges), "vkInvalidateMappedMemoryRanges ABI");

struct land_result {
    nvn_gpu_buf mirror;
    atomic_int entered, release;
    uint32_t status;
    uint8_t landed[3];
};

static void tick(void) {
    const struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
}

static void land_done(void *user, const nvn_gpu_done *done) {
    struct land_result *result = user;
    result->status = done->status;
    atomic_store(&result->entered, 1);
    if (done->status == NVN_GPU_DONE_FAILED) return;
    for (int i = 0; i < 4000 && !atomic_load(&result->release); i++) tick();
    REQUIRE(atomic_load(&result->release));
    const uint8_t *p = nvn_gpu_buf_cpu(result->mirror);
    result->landed[0] = p[0];
    result->landed[1] = p[8];
    result->landed[2] = p[15];
}

struct next_submit {
    nvn_gpu_cmd cmd;
    nvn_gpu_err err;
    nvn_gpu_rc rc;
    atomic_int returned;
};

static void *submit_next(void *arg) {
    struct next_submit *next = arg;
    next->rc = nvn_gpu_cmd_commit(next->cmd, 2, NULL, NULL, &next->err);
    atomic_store(&next->returned, 1);
    return NULL;
}

struct mirror_stats {
    uint64_t waits, invalidates, invalidated_bytes, invalidated_payload, flushes, buffers;
};
static void stat(void *user, const char *name, uint64_t n) {
    struct mirror_stats *s = user;
    if (!strcmp(name, "mirror_wait_calls")) s->waits = n;
    if (!strcmp(name, "mirror_invalidate_calls")) s->invalidates = n;
    if (!strcmp(name, "mirror_invalidate_bytes")) s->invalidated_bytes = n;
    if (!strcmp(name, "mirror_invalidate_payload")) s->invalidated_payload = n;
    if (!strcmp(name, "mirror_flush_calls")) s->flushes = n;
    if (!strcmp(name, "bufs_created")) s->buffers = n;
}
static struct mirror_stats stats(void) {
    struct mirror_stats s = {0};
    nvn_gpu_stats(stat, &s, 0);
    return s;
}

static void test_land(const struct s2a_vk_device *physical, int use_cached, int fail, int no_cached) {
    // 保留真设备、类型索引、要求位和对象，只对分配器使用的属性表改 capability。
    // 同步校验检查实际 Vulkan 调用，非一致数据的可见性由严格的回放夹具检查。
    struct s2a_vk_device dev = *physical;
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount; i++) {
        VkMemoryPropertyFlags *flags = &dev.memory.memoryTypes[i].propertyFlags;
        if (no_cached) *flags &= ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        else if (use_cached && (*flags & cached) == cached) *flags &= ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    enabled = use_cached;
    fail_invalidate = fail;
    const int noncoherent = use_cached && !no_cached;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-cached"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));

    const VkDeviceSize atom = dev.properties.limits.nonCoherentAtomSize;
    REQUIRE(atom >= 16 && atom <= 4096);   // lavapipe fixture: writes below must share an atom
    uint8_t init[4096] = {0};
    nvn_gpu_buf mirror = NULL;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &mirror, &err));
    REQUIRE(mirror && nvn_gpu_mirror_visibility_enabled() == noncoherent);
    REQUIRE(nvn_gpu_mirror_state(mirror) == (noncoherent ? NVN_GPU_MIRROR_CPU_IDLE : NVN_GPU_MIRROR_COHERENT));
    if (noncoherent) REQUIRE(nvn_gpu_mirror_memory_cached(mirror));
    if (no_cached) REQUIRE(!nvn_gpu_mirror_memory_cached(mirror));
    nvn_gpu_buf_cpu(mirror)[15] = 0x9a;
    if (noncoherent) REQUIRE(nvn_gpu_mirror_flush(mirror, 15, 1));

    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, mirror, 0, 4, 0x11111111);
    nvn_gpu_fill_buf(cmd, mirror, 8, 4, 0x33333333);
    const nvn_gpu_mirror_span spans[] = {{mirror, 0, 4}, {mirror, 8, 4}};
    if (noncoherent) nvn_gpu_mirror_visibility_record(cmd, spans, 2);
    struct land_result result = {.mirror = mirror};
    atomic_init(&result.release, !noncoherent);
    GPU(nvn_gpu_cmd_commit(cmd, 1, land_done, &result, &err));

    if (fail) {
        REQUIRE(nvn_gpu_fence_wait(1, WAIT_NS, &err).code == NVN_GPU_E_FAILED);
        REQUIRE(!nvn_gpu_fence_done());
    } else if (noncoherent) {
        for (int i = 0; i < 2000 && !atomic_load(&result.entered); i++) tick();
        REQUIRE(atomic_load(&result.entered) && !nvn_gpu_fence_done());
        REQUIRE(nvn_gpu_mirror_state(mirror) == NVN_GPU_MIRROR_GPU_BUSY);
        struct next_submit next = {0};
        GPU(nvn_gpu_cmd_begin(NULL, &next.cmd, &err));
        nvn_gpu_fill_buf(next.cmd, mirror, 4, 4, 0x55555555);
        pthread_t thread;
        REQUIRE(!pthread_create(&thread, NULL, submit_next, &next));
        for (int i = 0; i < 2000 && !stats().waits && !atomic_load(&next.returned); i++) tick();
        REQUIRE(stats().waits == 1 && !atomic_load(&next.returned));
        REQUIRE(nvn_gpu_fence_wait(2, 0, NULL).code == NVN_GPU_E_ARG);
        atomic_store(&result.release, 1);
        REQUIRE(!pthread_join(thread, NULL));
        if (!nvn_gpu_ok(next.rc)) fprintf(stderr, "second commit: %s\n", next.err.why);
        REQUIRE(nvn_gpu_ok(next.rc));
        GPU(nvn_gpu_fence_wait(2, WAIT_NS, &err));
    } else GPU(nvn_gpu_fence_wait(1, WAIT_NS, &err));

    const struct mirror_stats observed = stats();
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    REQUIRE(atomic_load(&result.entered));
    REQUIRE(result.status == (fail ? NVN_GPU_DONE_FAILED : NVN_GPU_DONE_OK));
    if (!fail) REQUIRE(result.landed[0] == 0x11 && result.landed[1] == 0x33 && result.landed[2] == 0x9a);
    REQUIRE(observed.buffers == 1);
    REQUIRE(observed.invalidates == (unsigned)noncoherent && observed.flushes == (unsigned)noncoherent);
    REQUIRE(observed.invalidated_bytes == (noncoherent ? atom : 0) &&
            observed.invalidated_payload == (noncoherent ? 8 : 0));
    REQUIRE(observed.waits == (unsigned)(noncoherent && !fail));
    printf("land-cached dq=%d on=%d fallback=%d fail=%d status=%u buffers=1 flush=%llu invalidate=%llu wait=%llu\n",
           dq, enabled, no_cached, fail, result.status, (unsigned long long)observed.flushes,
           (unsigned long long)observed.invalidates, (unsigned long long)observed.waits);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    dq = atoi(argv[1]);
    struct s2a_vk_device dev;
    char why[256] = {0};
    const int open = s2a_vk_device_create(&dev, why, sizeof why);
    if (open) fprintf(stderr, "create device: %s\n", why);
    REQUIRE(!open);
    test_land(&dev, 0, 0, 0);
    test_land(&dev, 1, 0, 1);
    test_land(&dev, 1, 0, 0);
    test_land(&dev, 1, 1, 0);
    s2a_vk_device_destroy(&dev, 1);
    return 0;
}
