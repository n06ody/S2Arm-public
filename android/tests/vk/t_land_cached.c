// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_backend.h"
#include "nvn_plat.h"
#include "nvn_plat_vk.h"
#include "nvn_fmt_info.h"
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

static int dq, enabled, fail_invalidate, wait_yield, wait_probe, edge_snapshot;
static atomic_int presented_submits, guest_submits;
static atomic_int fail_next_flush, device_loss_gate, release_device_loss;
static atomic_int fail_next_submit;
static _Atomic VkFence edge_last_fence;

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) {
    static const char *const exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    *n = 2;
    return exts;
}
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    PFN_vkCreateHeadlessSurfaceEXT create =
        (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(i, "vkCreateHeadlessSurfaceEXT");
    const VkHeadlessSurfaceCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    return create ? create(i, &ci, NULL, s) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) {
    *x = 320;
    *y = 180;
    return 1;
}
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *key, int fallback) {
    if (!strcmp(key, "nvk_dq")) return dq;
    if (!strcmp(key, "nvk_land_cached")) return enabled;
    if (!strcmp(key, "nvk_land_wait_yield_wsi")) return wait_yield;
    if (!strcmp(key, "nvk_land_wait_probe")) return wait_probe;
    if (!strcmp(key, "nvk_land_edge_snapshot")) return edge_snapshot;
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

extern __typeof__(vkQueueSubmit) __real_vkQueueSubmit;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit(VkQueue queue, uint32_t count,
                                                   const VkSubmitInfo *info, VkFence fence) {
    if (atomic_exchange(&fail_next_submit, 0)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkResult result = __real_vkQueueSubmit(queue, count, info, fence);
    if (result == VK_SUCCESS) {
        if (edge_snapshot && fence) atomic_store(&edge_last_fence, fence);
        for (uint32_t i = 0; i < count; i++) {
            if (info[i].signalSemaphoreCount) atomic_fetch_add(&presented_submits, 1);
            else atomic_fetch_add(&guest_submits, 1);
        }
    }
    return result;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkQueueSubmit), PFN_vkQueueSubmit),
               "vkQueueSubmit ABI");

extern __typeof__(vkFlushMappedMemoryRanges) __real_vkFlushMappedMemoryRanges;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkFlushMappedMemoryRanges(VkDevice device, uint32_t count,
                                                               const VkMappedMemoryRange *ranges) {
    const VkResult result = __real_vkFlushMappedMemoryRanges(device, count, ranges);
    return result == VK_SUCCESS && atomic_exchange(&fail_next_flush, 0) ? VK_ERROR_OUT_OF_HOST_MEMORY : result;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkFlushMappedMemoryRanges),
                                           PFN_vkFlushMappedMemoryRanges), "vkFlushMappedMemoryRanges ABI");

struct land_result {
    nvn_gpu_buf mirror;
    uint64_t offset;
    atomic_int entered, release;
    unsigned callbacks;
    uint32_t status;
    uint8_t landed[3];
};

static void tick(void) {
    const struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
}

extern __typeof__(vkWaitForFences) __real_vkWaitForFences;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences,
                                                    VkBool32 all, uint64_t timeout) {
    const VkResult result = __real_vkWaitForFences(device, count, fences, all, timeout);
    int armed = 1;
    // 真实段已退休后才报告模拟丢失，防止真驱动仍使用即将回收的对象。
    if (result != VK_SUCCESS || !atomic_compare_exchange_strong(&device_loss_gate, &armed, 2)) return result;
    for (int i = 0; i < 4000 && !atomic_load(&release_device_loss); i++) tick();
    REQUIRE(atomic_load(&release_device_loss));
    return VK_ERROR_DEVICE_LOST;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkWaitForFences), PFN_vkWaitForFences),
               "vkWaitForFences ABI");

static void land_done(void *user, const nvn_gpu_done *done) {
    struct land_result *result = user;
    result->callbacks++;
    result->status = done->status;
    atomic_store(&result->entered, 1);
    if (done->status == NVN_GPU_DONE_FAILED) return;
    for (int i = 0; i < 4000 && !atomic_load(&result->release); i++) tick();
    REQUIRE(atomic_load(&result->release));
    const uint8_t *p = nvn_gpu_buf_cpu(result->mirror) + result->offset;
    result->landed[0] = p[0];
    result->landed[1] = p[8];
    result->landed[2] = p[15];
}

struct next_submit {
    nvn_gpu_cmd cmd;
    uint64_t signal;
    nvn_gpu_done_fn done;
    void *user;
    nvn_gpu_err err;
    nvn_gpu_rc rc;
    atomic_int started, returned;
};

static void *submit_next(void *arg) {
    struct next_submit *next = arg;
    atomic_store(&next->started, 1);
    next->rc = nvn_gpu_cmd_commit(next->cmd, next->signal, next->done, next->user, &next->err);
    atomic_store(&next->returned, 1);
    return NULL;
}

struct mirror_stats {
    uint64_t waits, last_wait, transfer_waits, storage_waits;
    uint64_t invalidates, invalidated_bytes, invalidated_payload, flushes, buffers;
    uint64_t wsi_locks, wsi_lock_ns, wsi_during_wait, wsi_overtakes;
    uint64_t sampled, dropped, atoms, edge_atoms, edge_waits;
    uint64_t snapshot_bytes, snapshot_waits_elided, snapshot_full;
};
static void stat(void *user, const char *name, uint64_t n) {
    struct mirror_stats *s = user;
    if (!strcmp(name, "mirror_wait_calls")) s->waits = n;
    if (!strcmp(name, "mirror_wait_last_segment")) s->last_wait = n;
    if (!strcmp(name, "mirror_wait_transfer_calls")) s->transfer_waits = n;
    if (!strcmp(name, "mirror_wait_storage_calls")) s->storage_waits = n;
    if (!strcmp(name, "mirror_invalidate_calls")) s->invalidates = n;
    if (!strcmp(name, "mirror_invalidate_bytes")) s->invalidated_bytes = n;
    if (!strcmp(name, "mirror_invalidate_payload")) s->invalidated_payload = n;
    if (!strcmp(name, "mirror_flush_calls")) s->flushes = n;
    if (!strcmp(name, "bufs_created")) s->buffers = n;
    if (!strcmp(name, "mirror_wsi_lock_calls")) s->wsi_locks = n;
    if (!strcmp(name, "mirror_wsi_lock_ns")) s->wsi_lock_ns = n;
    if (!strcmp(name, "mirror_wsi_during_wait")) s->wsi_during_wait = n;
    if (!strcmp(name, "mirror_wsi_overtakes")) s->wsi_overtakes = n;
    if (!strcmp(name, "mirror_probe_sampled")) s->sampled = n;
    if (!strcmp(name, "mirror_probe_dropped")) s->dropped = n;
    if (!strcmp(name, "mirror_probe_atoms")) s->atoms = n;
    if (!strcmp(name, "mirror_probe_edge_atoms")) s->edge_atoms = n;
    if (!strcmp(name, "mirror_probe_edge_waits")) s->edge_waits = n;
    if (!strcmp(name, "mirror_edge_bytes")) s->snapshot_bytes = n;
    if (!strcmp(name, "mirror_edge_wait_elided")) s->snapshot_waits_elided = n;
    if (!strcmp(name, "mirror_edge_fallback_full")) s->snapshot_full = n;
}
static struct mirror_stats stats(void) {
    struct mirror_stats s = {0};
    nvn_gpu_stats(stat, &s, 0);
    return s;
}

static struct s2a_vk_device cached_device(const struct s2a_vk_device *physical, int use_cached, int no_cached) {
    // 保留真设备、类型索引、要求位和对象，只对分配器使用的属性表改 capability。
    // 同步校验检查实际 Vulkan 调用，非一致数据的可见性由严格的回放夹具检查。
    struct s2a_vk_device dev = *physical;
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount; i++) {
        VkMemoryPropertyFlags *flags = &dev.memory.memoryTypes[i].propertyFlags;
        if (no_cached) *flags &= ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        else if (use_cached && (*flags & cached) == cached) *flags &= ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    return dev;
}

static void test_land(const struct s2a_vk_device *physical, int use_cached, int fail, int no_cached) {
    struct s2a_vk_device dev = cached_device(physical, use_cached, no_cached);
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
        struct next_submit next = {.signal = 2};
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

static void wait_callback(const struct land_result *land) {
    for (int i = 0; i < 1500 && !atomic_load(&land->entered); i++) tick();
    REQUIRE(atomic_load(&land->entered));
}

static void require_no_wait(const struct next_submit *next) {
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&next->returned); i++) tick();
    if (!atomic_load(&next->returned) || stats().waits)
        fprintf(stderr, "disjoint write or read-only segment blocked on mirror callback: signal=%llu waits=%llu\n",
                (unsigned long long)next->signal, (unsigned long long)stats().waits);
    REQUIRE(atomic_load(&next->returned) && !stats().waits && nvn_gpu_ok(next->rc));
}

static void test_land_overlap(const struct s2a_vk_device *physical) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    const VkDeviceSize atom = dev.properties.limits.nonCoherentAtomSize;
    REQUIRE(atom >= 16 && atom <= 2048);
    enabled = 1;
    fail_invalidate = 0;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-cached-overlap"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    uint8_t init[4096] = {0};
    nvn_gpu_buf mirror, copy;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &mirror, &err));
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &copy, &err));
    REQUIRE(nvn_gpu_mirror_visibility_enabled());

    nvn_gpu_cmd cmd;
    struct land_result first = {.mirror = mirror};
    const nvn_gpu_mirror_span a = {mirror, 0, 4}, b = {mirror, atom, 4}, readback = {copy, 0, 4};
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, mirror, 0, 4, 0x11111111);
    nvn_gpu_mirror_visibility_record(cmd, &a, 1);
    GPU(nvn_gpu_cmd_commit(cmd, 1, land_done, &first, &err));
    wait_callback(&first);

    struct land_result second = {.mirror = mirror, .offset = atom};
    struct next_submit disjoint = {.signal = 2, .done = land_done, .user = &second};
    GPU(nvn_gpu_cmd_begin(NULL, &disjoint.cmd, &err));
    nvn_gpu_fill_buf(disjoint.cmd, mirror, atom, 4, 0x22222222);
    nvn_gpu_mirror_visibility_record(disjoint.cmd, &b, 1);
    pthread_t thread;
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &disjoint));
    require_no_wait(&disjoint);
    REQUIRE(!pthread_join(thread, NULL));
    REQUIRE(!nvn_gpu_fence_done());

    struct next_submit read = {.signal = 3};
    GPU(nvn_gpu_cmd_begin(NULL, &read.cmd, &err));
    nvn_gpu_copy_buf(read.cmd, mirror, 0, copy, 0, 4);   // 作传输源，与还在回写的原子重叠
    nvn_gpu_mirror_visibility_record(read.cmd, &readback, 1);
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &read));
    require_no_wait(&read);
    REQUIRE(!pthread_join(thread, NULL));

    struct next_submit neighbor = {.signal = 4};
    GPU(nvn_gpu_cmd_begin(NULL, &neighbor.cmd, &err));
    nvn_gpu_copy_buf(neighbor.cmd, copy, 0, mirror, 4, 4);   // 作传输目标，字节不同但与段 1 同原子
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &neighbor));
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&neighbor.returned); i++) tick();
    REQUIRE(stats().waits == 1 && !atomic_load(&neighbor.returned));
    REQUIRE(nvn_gpu_fence_wait(4, 0, NULL).code == NVN_GPU_E_ARG);

    atomic_store(&first.release, 1);
    wait_callback(&second);
    for (int i = 0; i < 1500 && !atomic_load(&neighbor.returned); i++) tick();
    REQUIRE(atomic_load(&neighbor.returned) && nvn_gpu_ok(neighbor.rc) && nvn_gpu_fence_done() == 1);
    REQUIRE(!pthread_join(thread, NULL));
    atomic_store(&second.release, 1);
    GPU(nvn_gpu_fence_wait(4, WAIT_NS, &err));
    REQUIRE(first.landed[0] == 0x11 && second.landed[0] == 0x22 && nvn_gpu_buf_cpu(copy)[0] == 0x11);
    const struct mirror_stats observed = stats();
    REQUIRE(observed.waits == 1 && observed.transfer_waits == 1 && observed.last_wait == 1);
    nvn_gpu_buf_release(copy, NULL, NULL);
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    printf("land-cached overlap dq=%d atom=%llu: disjoint=0 read=0 neighbor=1 waited_segment=%llu\n",
            dq, (unsigned long long)atom, (unsigned long long)observed.last_wait);
}

static nvn_gpu_pipe load_storage_writer(const char *path) {
    uint32_t spirv[2048];
    FILE *f = fopen(path, "rb");
    REQUIRE(f);
    const size_t bytes = fread(spirv, 1, sizeof spirv, f);
    REQUIRE(bytes && feof(f) && !ferror(f));
    fclose(f);
    const nvn_gpu_layout_ent entry = {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W,
                                      .count = 1, .stages = 1u << NVN_GPU_SH_CS};
    const nvn_gpu_layout layout = {.ent = &entry, .nent = 1};
    const nvn_gpu_shader_src source = {.stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV,
                                      .blob = spirv, .bytes = bytes, .entry = "main", .layout = &layout};
    nvn_gpu_err err = {0};
    nvn_gpu_shader shader;
    GPU(nvn_gpu_shader_load(&source, &shader, &err));
    const nvn_gpu_stage_desc stage = {.sh = shader};
    nvn_gpu_pipe pipe;
    GPU(nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_shader_release(shader);   // 管线保活。
    return pipe;
}

static void test_land_storage(const struct s2a_vk_device *physical, const char *shader) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    const VkDeviceSize atom = dev.properties.limits.nonCoherentAtomSize;
    enabled = 1;
    wait_probe = 1;
    fail_invalidate = 0;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-cached-storage"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    const nvn_gpu_pipe pipe = load_storage_writer(shader);
    uint8_t init[256] = {0};
    REQUIRE(atom >= 16 && atom <= sizeof init);
    nvn_gpu_buf mirror;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &mirror, &err));
    REQUIRE(nvn_gpu_mirror_visibility_enabled());

    nvn_gpu_cmd cmd;
    struct land_result first = {.mirror = mirror};
    const nvn_gpu_mirror_span original = {mirror, 0, atom};
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, mirror, 0, atom, 0x11111111);
    nvn_gpu_mirror_visibility_record(cmd, &original, 1);
    GPU(nvn_gpu_cmd_commit(cmd, 1, land_done, &first, &err));
    wait_callback(&first);

    struct next_submit writer = {.signal = 2};
    GPU(nvn_gpu_cmd_begin(NULL, &writer.cmd, &err));
    nvn_gpu_set_pipe(writer.cmd, pipe);
    const nvn_gpu_binding bind = {.cls = NVN_GPU_RES_SSBO, .u.b = {mirror, 0, 16}};
    nvn_gpu_bind(writer.cmd, NVN_GPU_SH_CS, &bind, 1);
    nvn_gpu_dispatch(writer.cmd, pipe, 1, 1, 1, NULL, 0);
    const nvn_gpu_mirror_span written = {mirror, 0, 16};
    nvn_gpu_mirror_visibility_record(writer.cmd, &written, 1);
    pthread_t thread;
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &writer));
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&writer.returned); i++) tick();
    const struct mirror_stats observed = stats();
    REQUIRE(observed.waits == 1 && observed.storage_waits == 1 && observed.last_wait == 1);
    REQUIRE(observed.sampled == 1 && !observed.dropped && observed.atoms == 1);
    REQUIRE(!observed.edge_atoms && !observed.edge_waits);   // 整原子落地，没有可窄取的非对齐边缘。
    REQUIRE(!atomic_load(&writer.returned));
    atomic_store(&first.release, 1);
    REQUIRE(!pthread_join(thread, NULL) && nvn_gpu_ok(writer.rc));
    GPU(nvn_gpu_fence_wait(2, WAIT_NS, &err));

    uint32_t original_args[4];
    memcpy(original_args, nvn_gpu_buf_cpu(mirror), sizeof original_args);
    REQUIRE(first.landed[0] == 0x11 && original_args[0] == 3 && original_args[1] == 1);
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    wait_probe = 0;
    printf("land-cached storage dq=%d: waits=%llu waited_segment=%llu\n", dq,
            (unsigned long long)observed.storage_waits, (unsigned long long)observed.last_wait);
}

// The edge optimization introduces a callback-scoped copy source. Weak lookup lets this
// same fixture run on the old backend and reach the behavior assertion without a link error.
extern int nvn_gpu_mirror_copy(const nvn_gpu_done *, nvn_gpu_buf, uint64_t, uint64_t, void *)
    __attribute__((weak));
extern void nvn_gpu_mirror_visibility_record_copy(nvn_gpu_cmd, const nvn_gpu_mirror_span *, uint32_t)
    __attribute__((weak));

struct edge_land {
    nvn_gpu_buf mirror;
    uint64_t off;
    atomic_int entered, release;
    unsigned callbacks, status;
    uint8_t guest[16];
};

static void edge_done(void *user, const nvn_gpu_done *done) {
    struct edge_land *land = user;
    land->callbacks++;
    land->status = done->status;
    atomic_store(&land->entered, 1);
    if (done->status == NVN_GPU_DONE_FAILED) return;
    for (int i = 0; i < 4000 && !atomic_load(&land->release); i++) tick();
    REQUIRE(atomic_load(&land->release));
    for (unsigned i = 3; i <= 11; i += 8) {
        if (!nvn_gpu_mirror_copy || !nvn_gpu_mirror_copy(done, land->mirror, land->off + i, 3, land->guest + i))
            memcpy(land->guest + i, nvn_gpu_buf_cpu(land->mirror) + land->off + i, 3);
    }
}

static void wait_edge_callback(const struct edge_land *land) {
    for (int i = 0; i < 1500 && !atomic_load(&land->entered); i++) tick();
    REQUIRE(atomic_load(&land->entered));
}

static void edge_record(nvn_gpu_cmd cmd, struct edge_land *land, uint32_t pattern) {
    memset(land->guest, 0x7e, sizeof land->guest);
    nvn_gpu_fill_buf(cmd, land->mirror, land->off, 16, pattern);
    const nvn_gpu_mirror_span spans[] = {{land->mirror, land->off + 3, 3}, {land->mirror, land->off + 11, 3}};
    if (nvn_gpu_mirror_visibility_record_copy) nvn_gpu_mirror_visibility_record_copy(cmd, spans, 2);
    else nvn_gpu_mirror_visibility_record(cmd, spans, 2);
}

static void edge_expected(const struct edge_land *land, uint8_t pattern) {
    const uint8_t expected[] = {0x7e, 0x7e, 0x7e, pattern, pattern, pattern, 0x7e, 0x7e,
                                0x7e, 0x7e, 0x7e, pattern, pattern, pattern, 0x7e, 0x7e};
    REQUIRE(land->callbacks == 1 && land->status == NVN_GPU_DONE_OK);
    REQUIRE(memcmp(land->guest, expected, sizeof expected) == 0);
}

static void test_edge_snapshot(const struct s2a_vk_device *physical, int on) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    const VkDeviceSize atom = dev.properties.limits.nonCoherentAtomSize;
    REQUIRE(atom >= 16 && atom <= 2048);
    enabled = 1;
    edge_snapshot = on;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-edge-snapshot"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    nvn_gpu_buf mirror;
    uint8_t initial[4096] = {0};
    GPU(nvn_gpu_buf_create(sizeof initial, NVN_GPU_MEM_POOL_MIRROR, initial, &mirror, &err));
    REQUIRE(nvn_gpu_mirror_state(mirror) == NVN_GPU_MIRROR_CPU_IDLE);
    // The buffer retains its noncoherent flag. Restore the real types for the upload arena,
    // which requires coherent memory even on lavapipe with a single memory type.
    dev.memory = physical->memory;

    nvn_gpu_cmd cmd;
    struct edge_land first = {.mirror = mirror};
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    edge_record(cmd, &first, 0x11111111);
    GPU(nvn_gpu_cmd_commit(cmd, 1, edge_done, &first, &err));
    wait_edge_callback(&first);

    struct next_submit disjoint = {.signal = 2};
    GPU(nvn_gpu_cmd_begin(NULL, &disjoint.cmd, &err));
    nvn_gpu_fill_buf(disjoint.cmd, mirror, atom, 4, 0x22222222);
    pthread_t thread;
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &disjoint));
    require_no_wait(&disjoint);
    REQUIRE(!pthread_join(thread, NULL));

    struct edge_land second = {.mirror = mirror, .release = 1};
    struct next_submit next = {.signal = 3, .done = edge_done, .user = &second};
    GPU(nvn_gpu_cmd_begin(NULL, &next.cmd, &err));
    edge_record(next.cmd, &second, 0x33333333);  // 后继 GPU 写与旧段覆盖同一颗 atom。
    const uint32_t upload = 0x44444444;
    nvn_gpu_buf_write(next.cmd, mirror, 0, &upload, sizeof upload);  // 同颗 atom 的主机上传。
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &next));
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&next.returned); i++) tick();
    if (atomic_load(&next.returned) && !nvn_gpu_ok(next.rc))
        fprintf(stderr, "land-edge-snapshot: successor failed before submission: %s\n", next.err.why);
    if (on) {
        if (stats().waits)
            fprintf(stderr, "land-edge-snapshot: fully covered atom still waited for callback (dq=%d)\n", dq);
        REQUIRE(atomic_load(&next.returned) && !stats().waits && nvn_gpu_ok(next.rc));
        // 旧、后两段同时在途，第三次 GPU 写同一 atom。
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_fill_buf(cmd, mirror, 0, 16, 0x55555555);
        GPU(nvn_gpu_cmd_commit(cmd, 4, NULL, NULL, &err));
        REQUIRE(nvn_gpu_fence_wait(4, 0, &err).code == NVN_GPU_E_TIMEOUT);
        // 等真驱动写完后再放行旧回调，强制回写取旧快照，消除快驱动碰巧没覆盖的假绿。
        const VkFence written = atomic_load(&edge_last_fence);
        REQUIRE(written && __real_vkWaitForFences(dev.device, 1, &written, VK_TRUE, WAIT_NS) == VK_SUCCESS);
    } else {
        REQUIRE(stats().waits == 1 && !atomic_load(&next.returned));
    }
    REQUIRE(!nvn_gpu_fence_done());
    atomic_store(&first.release, 1);
    REQUIRE(!pthread_join(thread, NULL) && nvn_gpu_ok(next.rc));
    GPU(nvn_gpu_fence_wait(on ? 4 : 3, WAIT_NS, &err));
    edge_expected(&first, 0x11);
    const uint8_t expected[16] = {0x7e, 0x7e, 0x7e, 0x44, 0x33, 0x33, 0x7e, 0x7e,
                                  0x7e, 0x7e, 0x7e, 0x33, 0x33, 0x33, 0x7e, 0x7e};
    REQUIRE(second.callbacks == 1 && second.status == NVN_GPU_DONE_OK);
    REQUIRE(!memcmp(second.guest, expected, sizeof expected));
    const struct mirror_stats observed = stats();
    REQUIRE(observed.snapshot_bytes == (on ? 2 * atom : 0));
    REQUIRE(observed.snapshot_waits_elided == (on ? 2 : 0));
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    edge_snapshot = 0;
    printf("land-cached edge dq=%d on=%d: disjoint=0 same_atom=%d exact=1\n", dq, on, !on);
}

static void test_edge_snapshot_full(const struct s2a_vk_device *physical) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    const VkDeviceSize atom = dev.properties.limits.nonCoherentAtomSize;
    REQUIRE(atom >= 16 && atom <= 2048);
    enabled = edge_snapshot = 1;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-edge-full"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    uint8_t initial[10240] = {0};
    nvn_gpu_buf mirror;
    GPU(nvn_gpu_buf_create(sizeof initial, NVN_GPU_MEM_POOL_MIRROR, initial, &mirror, &err));
    struct edge_land pending[5] = {0};
    nvn_gpu_cmd cmd;
    for (unsigned i = 0; i < 5; i++) {
        pending[i].mirror = mirror;
        pending[i].off = i * atom;
        atomic_init(&pending[i].release, i != 0);
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        edge_record(cmd, &pending[i], 0x66666666);
        GPU(nvn_gpu_cmd_commit(cmd, i + 1, edge_done, &pending[i], &err));
        if (!i) wait_edge_callback(&pending[0]);
    }

    pthread_t thread;
    struct next_submit covered = {.signal = 6};
    GPU(nvn_gpu_cmd_begin(NULL, &covered.cmd, &err));
    nvn_gpu_fill_buf(covered.cmd, mirror, 0, 16, 0x44444444);
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &covered));
    require_no_wait(&covered);
    REQUIRE(!pthread_join(thread, NULL));

    struct next_submit fallback = {.signal = 7};
    GPU(nvn_gpu_cmd_begin(NULL, &fallback.cmd, &err));
    nvn_gpu_fill_buf(fallback.cmd, mirror, 4 * atom, 16, 0x44444444);
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &fallback));
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&fallback.returned); i++) tick();
    REQUIRE(stats().waits == 1 && stats().last_wait == 5 && !atomic_load(&fallback.returned));
    atomic_store(&pending[0].release, 1);
    REQUIRE(!pthread_join(thread, NULL) && nvn_gpu_ok(fallback.rc));
    GPU(nvn_gpu_fence_wait(7, WAIT_NS, &err));
    for (unsigned i = 0; i < 5; i++) edge_expected(&pending[i], 0x66);
    const struct mirror_stats observed = stats();
    REQUIRE(observed.snapshot_bytes == 4 * atom && observed.snapshot_full == 1);
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    edge_snapshot = 0;
    printf("land-cached edge-full dq=%d: five pending segments, fallback on fifth\n", dq);
}

struct mixed_land {
    nvn_gpu_buf mirror;
    VkDeviceSize bytes;
    atomic_int entered, release;
    uint8_t guest[6144];
};

static void mixed_done(void *user, const nvn_gpu_done *done) {
    struct mixed_land *land = user;
    REQUIRE(done->status == NVN_GPU_DONE_OK);
    atomic_store(&land->entered, 1);
    for (int i = 0; i < 4000 && !atomic_load(&land->release); i++) tick();
    REQUIRE(atomic_load(&land->release));
    if (!nvn_gpu_mirror_copy || !nvn_gpu_mirror_copy(done, land->mirror, 3, land->bytes - 6, land->guest + 3))
        memcpy(land->guest + 3, nvn_gpu_buf_cpu(land->mirror) + 3, land->bytes - 6);
}

static void test_edge_mixed(const struct s2a_vk_device *physical) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    const VkDeviceSize atom = dev.properties.limits.nonCoherentAtomSize;
    REQUIRE(atom >= 16 && atom <= 2048);
    enabled = edge_snapshot = 1;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-edge-mixed"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    struct mixed_land first = {.bytes = 3 * atom};
    memset(first.guest, 0x7e, sizeof first.guest);
    GPU(nvn_gpu_buf_create(first.bytes, NVN_GPU_MEM_POOL_MIRROR, NULL, &first.mirror, &err));
    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, first.mirror, 0, first.bytes, 0x11111111);
    const nvn_gpu_mirror_span span = {first.mirror, 3, first.bytes - 6};
    if (nvn_gpu_mirror_visibility_record_copy) nvn_gpu_mirror_visibility_record_copy(cmd, &span, 1);
    else nvn_gpu_mirror_visibility_record(cmd, &span, 1);
    GPU(nvn_gpu_cmd_commit(cmd, 1, mixed_done, &first, &err));
    for (int i = 0; i < 1500 && !atomic_load(&first.entered); i++) tick();
    REQUIRE(atomic_load(&first.entered));

    pthread_t thread;
    struct next_submit edges = {.signal = 2};
    GPU(nvn_gpu_cmd_begin(NULL, &edges.cmd, &err));
    nvn_gpu_fill_buf(edges.cmd, first.mirror, 0, atom, 0x22222222);
    nvn_gpu_fill_buf(edges.cmd, first.mirror, 2 * atom, atom, 0x33333333);
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &edges));
    require_no_wait(&edges);
    REQUIRE(!pthread_join(thread, NULL));

    struct next_submit interior = {.signal = 3};
    GPU(nvn_gpu_cmd_begin(NULL, &interior.cmd, &err));
    nvn_gpu_fill_buf(interior.cmd, first.mirror, atom, atom, 0x44444444);
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &interior));
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&interior.returned); i++) tick();
    REQUIRE(stats().waits == 1 && stats().last_wait == 1 && !atomic_load(&interior.returned));
    atomic_store(&first.release, 1);
    REQUIRE(!pthread_join(thread, NULL) && nvn_gpu_ok(interior.rc));
    GPU(nvn_gpu_fence_wait(3, WAIT_NS, &err));
    for (VkDeviceSize i = 0; i < first.bytes; i++)
        REQUIRE(first.guest[i] == (i >= 3 && i < first.bytes - 3 ? 0x11 : 0x7e));
    const struct mirror_stats observed = stats();
    REQUIRE(observed.snapshot_bytes == 2 * atom && observed.snapshot_waits_elided == 1);
    nvn_gpu_buf_release(first.mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    edge_snapshot = 0;
    printf("land-cached edge-mixed dq=%d: two edges elided, interior waited, exact bytes\n", dq);
}

static void test_edge_snapshot_close(const struct s2a_vk_device *physical) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    enabled = edge_snapshot = 1;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-edge-close"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    uint8_t initial[4096] = {0};
    nvn_gpu_buf mirror;
    GPU(nvn_gpu_buf_create(sizeof initial, NVN_GPU_MEM_POOL_MIRROR, initial, &mirror, &err));
    struct edge_land first = {.mirror = mirror}, failed = {.mirror = mirror};
    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    edge_record(cmd, &first, 0x11111111);
    GPU(nvn_gpu_cmd_commit(cmd, 1, edge_done, &first, &err));
    wait_edge_callback(&first);

    struct next_submit next = {.signal = 2, .done = edge_done, .user = &failed};
    GPU(nvn_gpu_cmd_begin(NULL, &next.cmd, &err));
    edge_record(next.cmd, &failed, 0x33333333);
    atomic_store(&fail_next_submit, 1);
    pthread_t thread;
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &next));
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&next.returned); i++) tick();
    REQUIRE(atomic_load(&next.returned) && !stats().waits && !atomic_load(&fail_next_submit));
    REQUIRE(!pthread_join(thread, NULL) && next.rc.code == NVN_GPU_E_DEVICE);
    REQUIRE(stats().snapshot_bytes == dev.properties.limits.nonCoherentAtomSize);
    REQUIRE(nvn_gpu_close(0, &err).code == NVN_GPU_E_TIMEOUT);
    atomic_store(&first.release, 1);
    REQUIRE(nvn_gpu_fence_wait(2, WAIT_NS, &err).code == NVN_GPU_E_FAILED);
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    edge_expected(&first, 0x11);
    REQUIRE(failed.callbacks == 1 && failed.status == NVN_GPU_DONE_FAILED);
    for (unsigned i = 0; i < sizeof failed.guest; i++) REQUIRE(failed.guest[i] == 0x7e);
    GPU(nvn_gpu_open(&desc, &err));
    REQUIRE(!nvn_gpu_fence_done() && !nvn_gpu_fence_failed());
    GPU(nvn_gpu_close(WAIT_NS, &err));
    edge_snapshot = 0;
    printf("land-cached edge-close dq=%d: failed submit, callback race, clean reopen\n", dq);
}

struct present_submit {
    nvn_gpu_surface surface;
    nvn_gpu_drawable drawable;
    nvn_gpu_present_desc desc;
    nvn_gpu_err err;
    nvn_gpu_rc rc;
    atomic_int attempted;
};

static void *submit_present(void *arg) {
    struct present_submit *present = arg;
    atomic_store(&present->attempted, 1);
    present->rc = nvn_gpu_present(present->surface, &present->drawable, &present->desc, 1,
                                  NULL, NULL, &present->err);
    return NULL;
}

static void test_wait_yield(const struct s2a_vk_device *physical, int yield) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    enabled = 1;
    wait_yield = yield;
    wait_probe = 1;
    atomic_store(&presented_submits, 0);
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-wait-wsi"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    atomic_store(&guest_submits, 0);

    struct present_submit present = {0};
    GPU(nvn_gpu_surface_attach((void *)1, &present.surface, &err));
    GPU(nvn_gpu_drawable_acquire(present.surface, WAIT_NS, NULL, &present.drawable, &err));
    const nvn_gpu_tex_desc texture_desc = {
        .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
              .width = 4, .height = 4, .depth = 1, .levels = 1},
        .usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER,
    };
    nvn_gpu_tex tex;
    GPU(nvn_gpu_tex_create(&texture_desc, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &present.desc.src, &err));

    uint8_t init[256] = {0};
    nvn_gpu_buf mirror;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &mirror, &err));
    REQUIRE(nvn_gpu_mirror_visibility_enabled());
    struct land_result first = {.mirror = mirror};
    const nvn_gpu_mirror_span span = {mirror, 0, 4};
    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    // 只需一张此前客体段已生产的图。清屏不依赖夹具刻意移除的 coherent 暂存类型。
    const nvn_gpu_pass_desc pass = {
        .color = {{.view = present.desc.src, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE,
                   .clear.f = {0.25f, 0.5f, 0.75f, 1.0f}}},
        .ncolor = 1, .w = 4, .h = 4, .layers = 1,
    };
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_fill_buf(cmd, mirror, 0, 4, 0x11111111);
    nvn_gpu_mirror_visibility_record(cmd, &span, 1);
    GPU(nvn_gpu_cmd_commit(cmd, 1, land_done, &first, &err));
    wait_callback(&first);   // GPU 已完成，客体完成和回写尚未发布。

    struct next_submit next = {.signal = 2};
    GPU(nvn_gpu_cmd_begin(NULL, &next.cmd, &err));
    nvn_gpu_fill_buf(next.cmd, mirror, 4, 4, 0x22222222);   // 字节不同，同一原子。
    pthread_t guest_thread, later_thread, wsi_thread;
    REQUIRE(!pthread_create(&guest_thread, NULL, submit_next, &next));
    for (int i = 0; i < 1000 && !stats().waits && !atomic_load(&next.returned); i++) tick();
    REQUIRE(stats().last_wait == 1 && !atomic_load(&next.returned));
    struct next_submit later = {.signal = 3};
    GPU(nvn_gpu_cmd_begin(NULL, &later.cmd, &err));
    REQUIRE(!pthread_create(&later_thread, NULL, submit_next, &later));
    for (int i = 0; i < 1000 && !atomic_load(&later.started); i++) tick();
    REQUIRE(atomic_load(&later.started));
    REQUIRE(!pthread_create(&wsi_thread, NULL, submit_present, &present));
    for (int i = 0; i < 1000 && !atomic_load(&presented_submits); i++) tick();
    const int crossed = atomic_load(&presented_submits);
    const int guest_pending = !atomic_load(&next.returned) && !atomic_load(&later.returned) && !nvn_gpu_fence_done() &&
                              atomic_load(&guest_submits) == 1 &&
                              nvn_gpu_fence_wait(2, 0, NULL).code == NVN_GPU_E_ARG &&
                              nvn_gpu_fence_wait(3, 0, NULL).code == NVN_GPU_E_ARG;
    atomic_store(&first.release, 1);
    REQUIRE(!pthread_join(guest_thread, NULL) && !pthread_join(later_thread, NULL) && !pthread_join(wsi_thread, NULL));
    REQUIRE(atomic_load(&present.attempted) && atomic_load(&presented_submits) == 1);
    REQUIRE(nvn_gpu_ok(next.rc) && nvn_gpu_ok(later.rc) && nvn_gpu_ok(present.rc));
    GPU(nvn_gpu_fence_wait(3, WAIT_NS, &err));
    REQUIRE(guest_pending && first.landed[0] == 0x11);
    const struct mirror_stats observed = stats();
    GPU(nvn_gpu_surface_detach(present.surface, WAIT_NS, &err));
    nvn_gpu_view_release(present.desc.src);
    nvn_gpu_tex_release(tex);
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    wait_probe = 0;
    if (crossed != yield)
        fprintf(stderr, "WSI did not reach the Vulkan queue while the old writeback was gated: yield=%d submit=%d\n",
                yield, crossed);
    REQUIRE(crossed == yield);
    if (observed.wsi_locks != 1 || observed.wsi_during_wait != 1)
        fprintf(stderr, "WSI probe lock=%llu ns=%llu during_wait=%llu\n",
                (unsigned long long)observed.wsi_locks, (unsigned long long)observed.wsi_lock_ns,
                (unsigned long long)observed.wsi_during_wait);
    REQUIRE(observed.wsi_locks == 1 && observed.wsi_during_wait == 1);
    REQUIRE(observed.wsi_overtakes == (unsigned)crossed && observed.buffers == 1);
    REQUIRE(observed.sampled == 1 && !observed.dropped && observed.atoms == 1 &&
            observed.edge_atoms == 1 && observed.edge_waits == 1);
    printf("land-cached wait-yield dq=%d yield=%d wsi_before_guest=%d wsi_lock_ns=%llu edge_atoms=1/1 buffers=1\n",
           dq, yield, crossed, (unsigned long long)observed.wsi_lock_ns);
}

enum wait_error { WAIT_CLOSE, WAIT_DEVICE_LOST, WAIT_ASYNC_FLUSH };

static void test_wait_error(const struct s2a_vk_device *physical, enum wait_error fault) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    enabled = wait_yield = 1;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-wait-error"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    atomic_store(&guest_submits, 0);

    uint8_t init[256] = {0};
    nvn_gpu_buf mirror, other = NULL;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &mirror, &err));
    if (fault == WAIT_ASYNC_FLUSH)
        GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &other, &err));
    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, mirror, 0, 4, 0x11111111);
    const nvn_gpu_mirror_span span = {mirror, 0, 4};
    nvn_gpu_mirror_visibility_record(cmd, &span, 1);
    struct land_result first = {.mirror = mirror};
    struct land_result second = {.mirror = mirror};
    atomic_init(&second.release, 1);   // 若回归误报成功，尽快取到错误而非等测试超时。
    if (fault == WAIT_DEVICE_LOST) {
        atomic_store(&release_device_loss, 0);
        atomic_store(&device_loss_gate, 1);
    }
    GPU(nvn_gpu_cmd_commit(cmd, 1, land_done, &first, &err));
    if (fault == WAIT_DEVICE_LOST) {
        for (int i = 0; i < 1000 && atomic_load(&device_loss_gate) != 2; i++) tick();
        REQUIRE(atomic_load(&device_loss_gate) == 2);
    } else {
        wait_callback(&first);
    }

    struct next_submit next = {.signal = 2, .user = &second, .done = land_done};
    GPU(nvn_gpu_cmd_begin(NULL, &next.cmd, &err));
    nvn_gpu_fill_buf(next.cmd, mirror, 4, 4, 0x22222222);
    pthread_t thread;
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &next));
    for (int i = 0; i < 1000 && !stats().waits && !atomic_load(&next.returned); i++) tick();
    REQUIRE(stats().last_wait == 1 && !atomic_load(&next.returned));

    if (fault == WAIT_CLOSE) {
        REQUIRE(nvn_gpu_close(0, &err).code == NVN_GPU_E_TIMEOUT);
    } else if (fault == WAIT_ASYNC_FLUSH) {
        nvn_gpu_buf_cpu(other)[0] = 0x5a;
        atomic_store(&fail_next_flush, 1);
        REQUIRE(!nvn_gpu_mirror_flush(other, 0, 1));
        REQUIRE(!atomic_load(&fail_next_flush));
    }
    atomic_store(&release_device_loss, 1);
    atomic_store(&first.release, 1);
    REQUIRE(!pthread_join(thread, NULL));
    const int expected = fault == WAIT_CLOSE ? NVN_GPU_E_STOPPED : NVN_GPU_E_FAILED;
    if (next.rc.code != expected)
        fprintf(stderr, "land-wait fault=%d: guest commit must fail with %d, got %d (%s)\n",
                fault, expected, next.rc.code, next.err.why);
    REQUIRE(next.rc.code == expected);
    REQUIRE(atomic_load(&guest_submits) == 1);
    REQUIRE(nvn_gpu_fence_wait(2, WAIT_NS, &err).code == NVN_GPU_E_FAILED);
    REQUIRE(nvn_gpu_fence_done() == (fault == WAIT_DEVICE_LOST ? 0u : 1u));
    REQUIRE(nvn_gpu_fence_failed() == (fault == WAIT_DEVICE_LOST ? 1u : 2u));
    if (fault == WAIT_ASYNC_FLUSH) REQUIRE(strstr(next.err.why, "flush"));
    nvn_gpu_buf_release(other, NULL, NULL);
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    REQUIRE(first.callbacks == 1 && first.status == (fault == WAIT_DEVICE_LOST ? NVN_GPU_DONE_FAILED : NVN_GPU_DONE_OK));
    REQUIRE(second.callbacks == 1 && second.status == NVN_GPU_DONE_FAILED);
    atomic_store(&device_loss_gate, 0);
    GPU(nvn_gpu_open(&desc, &err));
    REQUIRE(!nvn_gpu_fence_done() && !nvn_gpu_fence_failed());
    GPU(nvn_gpu_close(WAIT_NS, &err));
    printf("land-cached wait-error dq=%d fault=%d: failed=%d callbacks=1/1\n", dq, fault, next.rc.code);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3 || argc == 4);
    dq = atoi(argv[1]);
    struct s2a_vk_device dev;
    char why[256] = {0};
    const int open = s2a_vk_device_create(&dev, why, sizeof why);
    if (open) fprintf(stderr, "create device: %s\n", why);
    REQUIRE(!open);
    if (argc == 4) {
        const char *scenario = argv[3];
        if (!strcmp(scenario, "edge-off")) test_edge_snapshot(&dev, 0);
        else if (!strcmp(scenario, "edge-on")) test_edge_snapshot(&dev, 1);
        else if (!strcmp(scenario, "edge-full")) test_edge_snapshot_full(&dev);
        else if (!strcmp(scenario, "edge-mixed")) test_edge_mixed(&dev);
        else if (!strcmp(scenario, "edge-close")) test_edge_snapshot_close(&dev);
        else REQUIRE(0);
        s2a_vk_device_destroy(&dev, 1);
        return 0;
    }
    test_land(&dev, 0, 0, 0);
    test_land(&dev, 1, 0, 1);
    test_land(&dev, 1, 0, 0);
    test_land(&dev, 1, 1, 0);
    test_land_overlap(&dev);
    test_land_storage(&dev, argv[2]);
    test_wait_yield(&dev, 0);
    test_wait_yield(&dev, 1);
    test_wait_error(&dev, WAIT_CLOSE);
    test_wait_error(&dev, WAIT_ASYNC_FLUSH);
    test_wait_error(&dev, WAIT_DEVICE_LOST);
    s2a_vk_device_destroy(&dev, 1);
    return 0;
}
