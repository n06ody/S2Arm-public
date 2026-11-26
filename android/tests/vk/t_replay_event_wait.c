// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Production replay, GPU texture-to-pool writeback, and Vulkan completion.
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "nvn_plat.h"
#include "rp_exec.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"

static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;
static nvn_gpu_err err;
static unsigned checks;

#define REQUIRE(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "event fixture line %d: %s (%d %s)\n", __LINE__, #expr, err.rc.code, err.why); \
    exit(2); \
} } while (0)

// This workload records no shaders or timestamp reports.
void nvn_hle_queue_enter(void *q) { REQUIRE(q != NULL); }
void nvn_hle_piece_begun(void) {}
const char *nvn_plat_dir(int which) { (void)which; return NULL; }
void nvn_hle_submit_lock(void) { pthread_mutex_lock(&submit_lock); }
void nvn_hle_submit_unlock(void) { pthread_mutex_unlock(&submit_lock); }
int nvn_cmd_call_resolve(uint64_t h, uint64_t *out) { *out = h; return 0; }
int nvn_dpool_base(const void *pool, int tex) { (void)pool; (void)tex; REQUIRE(0); return 0; }
int s2a_nvn_rast_flip = 1;
int s2a_nvn_render_cond(void) { return 0; }
int s2a_guest_tick_from_host_ns(uint64_t ns, uint64_t *out) { (void)ns; (void)out; REQUIRE(0); return 0; }
uint64_t s2a_guest_tick(void) { REQUIRE(0); return 0; }
uint64_t nvn_ga2ca(uint64_t a) { return a; }
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { REQUIRE(fn != NULL); }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { REQUIRE(fn != NULL); }
int rp_shader_resolve(const struct rp_shader_req *req, const struct rp_prog **out) {
    (void)req; (void)out; REQUIRE(0); return -1;
}
int rp_shader_resolve_compute(const struct rp_prog **out) { (void)out; REQUIRE(0); return -1; }
void rp_shader_reset(void) {}

typedef struct { _Alignas(8) uint8_t records[256]; size_t used; } stream;
static void record(stream *s, uint32_t op, const void *payload, size_t bytes) {
    const size_t size = (sizeof(op_hdr) + bytes + 7) & ~(size_t)7;
    REQUIRE(s->used + size <= sizeof s->records);
    op_hdr header = {op, (uint32_t)size};
    uint8_t *dst = s->records + s->used;
    memcpy(dst, &header, sizeof header);
    if (bytes) memcpy(dst + sizeof header, payload, bytes);
    s->used += size;
}

static void submit(void *q, const stream *s) {
    const uint64_t handle = (uintptr_t)s->records;
    nvn_be_submit(q, &handle, 1);
}

static void record_event(stream *s, volatile uint32_t *event, uint32_t mode, uint32_t value) {
    const struct { volatile uint32_t *event; uint32_t mode, value; } args = {event, mode, value};
    record(s, OP_SIGNAL_EVENT, &args, sizeof args);
}

static void record_copy(stream *s, const void *src, void *dst, size_t size) {
    const uint64_t args[] = {(uintptr_t)src, (uintptr_t)dst, size};
    record(s, OP_COPY_B2B, args, sizeof args);
}

int main(int argc, char **argv) {
    const int negative = argc == 2 && !strcmp(argv[1], "skip-wait");
    REQUIRE(argc == 1 || negative);
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&g_dev, why, sizeof why));
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "replay-event-wait";
    REQUIRE(open_backend());

    size_t page = nvn_dirty_page_size();
    uint8_t *guest = NULL;
    REQUIRE(!posix_memalign((void **)&guest, page, 3 * page));
    memset(guest, 0, 3 * page);
    uint8_t *source = guest, *result = guest + page;
    memset(source, 0x11, 64);
    pool_t pool = {.mem = guest, .size = 3 * page};
    tex_t tex = {
        .magic = MAGIC('T', 'E', 'X', 'T'),
        .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
              .width = 4, .height = 4, .depth = 1, .levels = 1},
        .pool = &pool, .off = (int64_t)(2 * page), .mem = guest + 2 * page, .serial = 1,
    };
    nvn_tex_layout(&tex.d, &tex.L);
    REQUIRE(tex.L.size > 0 && tex.L.size <= page);
    memset(tex.mem, 0x22, (size_t)tex.L.size);
    nvn_be_pool_add(guest, 3 * page);
    nvn_be_tex_init(&tex);

    queue_t a = {0}, b = {0};
    volatile uint32_t event = 0;
    stream wait = {0}, signal = {0};
    if (!negative) record_event(&wait, &event, NVN_EV_WAIT | NVN_EVENT_WAIT_MODE_EQUAL, 1);
    record_copy(&wait, source, result, 64);
    record(&wait, OP_END, NULL, 0);
    const p_copy_b2t transfer = {.src = (uintptr_t)source, .tex = &tex, .r = {0, 0, 0, 4, 4, 1}};
    record(&signal, OP_COPY_T2B, &transfer, sizeof transfer);
    record_event(&signal, &event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    record(&signal, OP_END, NULL, 0);

    nvn_hle_submit_lock();
    submit(&a, &wait);
    if (!negative) REQUIRE(!nvn_be_xq_wait_idle(&a, 0) && result[0] == 0);
    submit(&b, &signal);
    const uint64_t producer = nvn_be_last_fence();
    nvn_hle_submit_unlock();
    REQUIRE(OK(nvn_gpu_fence_wait(producer, 5000000000ull, &err)));
    REQUIRE(nvn_be_xq_wait_idle(&a, 5000000000ull));
    nvn_hle_submit_lock();
    const uint64_t consumer = nvn_be_last_fence();
    nvn_hle_submit_unlock();
    REQUIRE(OK(nvn_gpu_fence_wait(consumer, 5000000000ull, &err)));
    if (!negative) REQUIRE(consumer > producer);
    REQUIRE(event == 1 && source[0] == 0x22 && source[63] == 0x22);
    for (int i = 0; i < 64; i++) CHECK(result[i] == 0x22, "consumer byte %d = %02x expected 22", i, result[i]);
    printf("EVENT_ORDER producer=%02x consumer=%02x expected=22 failures=%d\n", source[0], result[0], g_fail);

    // Recycle a parked queue's address while the GPU is alive. The signal
    // afterwards cannot re-enter the discarded stream or publish its fence.
    event = 0;
    sync_t canceled = {0};
    nvn_hle_submit_lock();
    if (!negative) {
        submit(&a, &wait);
        REQUIRE(nvn_be_xq_fence(&a, &canceled));
        nvn_be_xq_discard(&a);
        nvn_be_event_signal(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
        REQUIRE(nvn_be_xq_wait_idle(&a, 0) && !sync_published(&canceled, NULL));
    }
    const uint64_t final = nvn_be_last_fence();
    nvn_hle_submit_unlock();
    REQUIRE(OK(nvn_gpu_fence_wait(final, 5000000000ull, &err)));
    REQUIRE(!nvn_unimpl_kinds());
    nvn_be_tex_release(&tex);
    nvn_be_pool_del(guest);
    nvn_be_shutdown_session();
    REQUIRE(OK(nvn_gpu_close(5000000000ull, &err)));
    s2a_vk_device_destroy(&g_dev, 1);
    free(guest);
    printf("replay event Vulkan setup_checks=%u failures=%d\n", checks, g_fail);
    return g_fail ? 1 : 0;
}
