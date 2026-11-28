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

static int dq, timeline, enabled, fail_invalidate, versioned;
int s2a_land_fixture_queue_lock_available(void);
int s2a_land_fixture_timeline_active(void);

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    *s = VK_NULL_HANDLE;
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *key, int fallback) {
    if (!strcmp(key, "nvk_dq")) return dq;
    if (!strcmp(key, "nvk_timeline_fence")) return timeline;
    if (!strcmp(key, "nvk_land_cached")) return enabled;
    if (!strcmp(key, "nvk_land_upload_versions")) return versioned;
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
    uint64_t offset;
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
    atomic_int returned;
};

static void *submit_next(void *arg) {
    struct next_submit *next = arg;
    next->rc = nvn_gpu_cmd_commit(next->cmd, next->signal, next->done, next->user, &next->err);
    atomic_store(&next->returned, 1);
    return NULL;
}

struct mirror_stats {
    uint64_t waits, last_wait, upload_waits, transfer_waits, storage_waits;
    uint64_t invalidates, invalidated_bytes, invalidated_payload, flushes, buffers;
};
static void stat(void *user, const char *name, uint64_t n) {
    struct mirror_stats *s = user;
    if (!strcmp(name, "mirror_wait_calls")) s->waits = n;
    if (!strcmp(name, "mirror_wait_last_segment")) s->last_wait = n;
    if (!strcmp(name, "mirror_wait_upload_calls")) s->upload_waits = n;
    if (!strcmp(name, "mirror_wait_transfer_calls")) s->transfer_waits = n;
    if (!strcmp(name, "mirror_wait_storage_calls")) s->storage_waits = n;
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
    versioned = 0;
    fail_invalidate = fail;
    const int noncoherent = use_cached && !no_cached;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-cached"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    REQUIRE(s2a_land_fixture_timeline_active() == timeline);

    const VkDeviceSize atom = dev.properties.limits.nonCoherentAtomSize;
    REQUIRE(atom >= 16 && atom <= 4096);   // lavapipe fixture: writes below must share an atom
    uint8_t init[4096] = {0};
    nvn_gpu_buf mirror = NULL;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &mirror, &err));
    REQUIRE(mirror && nvn_gpu_mirror_visibility_enabled() == noncoherent);
    REQUIRE(nvn_gpu_mirror_state(mirror) == (noncoherent ? NVN_GPU_MIRROR_CPU_IDLE : NVN_GPU_MIRROR_COHERENT));
    REQUIRE(nvn_gpu_mirror_version_atom(mirror) == 0);
    if (noncoherent) REQUIRE(nvn_gpu_mirror_memory_cached(mirror));
    if (no_cached) REQUIRE(!nvn_gpu_mirror_memory_cached(mirror));
    nvn_gpu_buf_cpu(mirror)[15] = 0x9a;
    if (noncoherent) REQUIRE(nvn_gpu_mirror_flush(mirror, 15, 1));

    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, mirror, 0, 4, 0x11111111);
    nvn_gpu_fill_buf(cmd, mirror, 8, 4, 0x33333333);
    const nvn_gpu_readback_span spans[] = {{.src = mirror, .offset = 0, .bytes = 4}, {.src = mirror, .offset = 8, .bytes = 4}};
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
    const int returned = atomic_load(&next->returned);
    const uint64_t waits = stats().waits;
    if (!returned || waits || !nvn_gpu_ok(next->rc)) {
        fprintf(stderr, "mirror submit signal=%llu returned=%d waits=%llu\n",
                (unsigned long long)next->signal, returned, (unsigned long long)waits);
        if (returned) fprintf(stderr, "mirror submit rc=%d: %s\n", next->rc.code, next->err.why);
    }
    REQUIRE(returned && !waits && nvn_gpu_ok(next->rc));
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
    const nvn_gpu_readback_span a = {.src = mirror, .offset = 0, .bytes = 4}, b = {.src = mirror, .offset = atom, .bytes = 4}, readback = {.src = copy, .offset = 0, .bytes = 4};
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
    enabled = 1;
    fail_invalidate = 0;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-cached-storage"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    const nvn_gpu_pipe pipe = load_storage_writer(shader);
    uint8_t init[256] = {0};
    nvn_gpu_buf mirror;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_POOL_MIRROR, init, &mirror, &err));
    REQUIRE(nvn_gpu_mirror_visibility_enabled());

    nvn_gpu_cmd cmd;
    struct land_result first = {.mirror = mirror};
    const nvn_gpu_readback_span original = {.src = mirror, .offset = 0, .bytes = 4};
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, mirror, 0, 4, 0x11111111);
    nvn_gpu_mirror_visibility_record(cmd, &original, 1);
    GPU(nvn_gpu_cmd_commit(cmd, 1, land_done, &first, &err));
    wait_callback(&first);

    struct next_submit writer = {.signal = 2};
    GPU(nvn_gpu_cmd_begin(NULL, &writer.cmd, &err));
    nvn_gpu_set_pipe(writer.cmd, pipe);
    const nvn_gpu_binding bind = {.cls = NVN_GPU_RES_SSBO, .u.b = {mirror, 0, 16}};
    nvn_gpu_bind(writer.cmd, NVN_GPU_SH_CS, &bind, 1);
    nvn_gpu_dispatch(writer.cmd, pipe, 1, 1, 1, NULL, 0);
    const nvn_gpu_readback_span written = {.src = mirror, .offset = 0, .bytes = 16};
    nvn_gpu_mirror_visibility_record(writer.cmd, &written, 1);
    pthread_t thread;
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &writer));
    for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&writer.returned); i++) tick();
    const struct mirror_stats observed = stats();
    REQUIRE(observed.waits == 1 && observed.storage_waits == 1 && observed.last_wait == 1);
    REQUIRE(!atomic_load(&writer.returned));
    atomic_store(&first.release, 1);
    REQUIRE(!pthread_join(thread, NULL) && nvn_gpu_ok(writer.rc));
    GPU(nvn_gpu_fence_wait(2, WAIT_NS, &err));

    uint32_t original_args[4];
    memcpy(original_args, nvn_gpu_buf_cpu(mirror), sizeof original_args);
    REQUIRE(first.landed[0] == 0x11 && original_args[0] == 3 && original_args[1] == 1);
    nvn_gpu_buf_release(mirror, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    printf("land-cached storage dq=%d: waits=%llu waited_segment=%llu\n", dq,
            (unsigned long long)observed.storage_waits, (unsigned long long)observed.last_wait);
}

static void test_land_upload_version(const struct s2a_vk_device *physical, int reuse_old) {
    struct s2a_vk_device dev = cached_device(physical, 1, 0);
    const uint64_t atom = dev.properties.limits.nonCoherentAtomSize;
    REQUIRE(atom >= 32);
    enabled = versioned = 1;
    const nvn_gpu_open_desc desc = {.host_device = &dev, .app_label = "land-upload-version"};
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&desc, &err));
    nvn_gpu_buf old = NULL, fresh = NULL;
    GPU(nvn_gpu_buf_create(atom, NVN_GPU_MEM_POOL_MIRROR, NULL, &old, &err));
    GPU(nvn_gpu_buf_create(atom, NVN_GPU_MEM_POOL_MIRROR, NULL, &fresh, &err));
    // 只有一个内存类型的 lavapipe 上，整表屏蔽 HOST_COHERENT 会让 256 KiB 上传暂存返回 NVN_GPU_E_OOM。
    // 两份镜像创建时已保存非一致属性。录命令前恢复真表，让暂存使用物理设备的 coherent 类型。
    dev.memory = physical->memory;
    REQUIRE(nvn_gpu_mirror_version_atom(old) == atom && nvn_gpu_mirror_version_atom(fresh) == atom);

    struct land_result first = {.mirror = old};
    const nvn_gpu_readback_span original = {.src = old, .offset = 0, .bytes = 20};
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_fill_buf(cmd, old, 0, 20, 0xa7a7a7a7);
    nvn_gpu_mirror_visibility_record(cmd, &original, 1);
    GPU(nvn_gpu_cmd_commit(cmd, 1, land_done, &first, &err));
    wait_callback(&first);

    const uint8_t upload[] = {0xb8, 0xb8, 0xb8, 0xb8};
    struct land_result second = {.mirror = fresh, .offset = 1, .release = 1};
    struct next_submit next = {.signal = 2};
    GPU(nvn_gpu_cmd_begin(NULL, &next.cmd, &err));
    if (reuse_old) {
        nvn_gpu_buf_write(next.cmd, old, 20, upload, sizeof upload);
    } else {
        nvn_gpu_fill_buf(next.cmd, fresh, 0, 20, 0x55555555);
        nvn_gpu_mirror_copy_span(next.cmd, old, fresh, 1, 3);
        nvn_gpu_buf_write(next.cmd, fresh, 8, upload, sizeof upload);
        const nvn_gpu_readback_span copied = {.src = fresh, .offset = 0, .bytes = 20};
        nvn_gpu_mirror_visibility_record(next.cmd, &copied, 1);
        next.done = land_done;
        next.user = &second;
    }

    pthread_t thread;
    REQUIRE(!pthread_create(&thread, NULL, submit_next, &next));
    if (reuse_old) {
        for (int i = 0; i < 1500 && !stats().waits && !atomic_load(&next.returned); i++) tick();
        REQUIRE(stats().upload_waits == 1 && !atomic_load(&next.returned));
        REQUIRE(nvn_gpu_fence_wait(2, 0, NULL).code == NVN_GPU_E_ARG);
        int lock_available = 0;
        for (int i = 0; i < 500 && !(lock_available = s2a_land_fixture_queue_lock_available()); i++) tick();
        REQUIRE(lock_available && !atomic_load(&next.returned));
    } else {
        require_no_wait(&next);
        REQUIRE(nvn_gpu_fence_wait(2, 0, NULL).code == NVN_GPU_E_TIMEOUT);
    }
    REQUIRE(!nvn_gpu_fence_done());
    atomic_store(&first.release, 1);
    REQUIRE(!pthread_join(thread, NULL));
    GPU(next.rc);
    GPU(nvn_gpu_fence_wait(2, WAIT_NS, &err));
    REQUIRE(first.landed[0] == 0xa7 && first.landed[1] == 0xa7);
    if (!reuse_old) {
        const uint8_t inherited[] = {0x55, 0xa7, 0xa7, 0xa7, 0x55};
        REQUIRE(!memcmp(nvn_gpu_buf_cpu(fresh), inherited, sizeof inherited));
        REQUIRE(second.landed[0] == 0xa7 && second.landed[1] == 0xb8 && second.landed[2] == 0x55);
    }
    REQUIRE(stats().waits == (uint64_t)reuse_old);
    printf("land-cached upload-version dq=%d reuse-old=%d wait=%llu: %s\n",
           dq, reuse_old, (unsigned long long)stats().waits,
           reuse_old ? "queue lock available during wait" : "3 inherited bytes, both sentinels and staged upload verified");
    nvn_gpu_buf_release(fresh, NULL, NULL);
    nvn_gpu_buf_release(old, NULL, NULL);
    GPU(nvn_gpu_close(WAIT_NS, &err));
    versioned = 0;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 4);
    dq = atoi(argv[1]);
    timeline = atoi(argv[3]);
    struct s2a_vk_device dev;
    char why[256] = {0};
    const int open = s2a_vk_device_create(&dev, why, sizeof why);
    if (open) fprintf(stderr, "create device: %s\n", why);
    REQUIRE(!open);
    test_land(&dev, 0, 0, 0);
    test_land(&dev, 1, 0, 1);
    test_land(&dev, 1, 0, 0);
    test_land(&dev, 1, 1, 0);
    test_land_overlap(&dev);
    test_land_storage(&dev, argv[2]);
    test_land_upload_version(&dev, 0);
    test_land_upload_version(&dev, 1);
    printf("land-cached DQ=%d timeline=%d scenarios=8\n", dq, timeline);
    s2a_vk_device_destroy(&dev, 1);
    return 0;
}
