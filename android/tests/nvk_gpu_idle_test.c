#include "nvn_backend.h"
#include "nvn_vk_idle.h"
#include "../../graphics/nvn/replay/rp_idle.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

// The first clock read is after EBUSY. Only then may the holding thread unlock.
static struct idle_bucket observed;
static pthread_mutex_t resource = PTHREAD_MUTEX_INITIALIZER, handoff = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned clock_calls;
static int held, enabled;
static uint64_t tick(void) {
    pthread_mutex_lock(&handoff);
    unsigned n = ++clock_calls;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&handoff);
    return n * 37;
}
static void note(enum nvn_gpu_idle_site site, nvn_gpu_buf buf, const void *src, uint64_t bytes, uint64_t ns) {
    (void)buf; (void)src;
    idle_cpu_note(&observed, site, IDLE_UNKNOWN, bytes, ns);
}
const struct nvn_gpu_idle_observer *nvn_gpu_idle_observer(void) {
    static const struct nvn_gpu_idle_observer o = {tick, note};
    return enabled ? &o : NULL;
}
static void *hold(void *unused) {
    (void)unused;
    pthread_mutex_lock(&resource);
    pthread_mutex_lock(&handoff);
    held = 1;
    pthread_cond_broadcast(&cv);
    while (!clock_calls) pthread_cond_wait(&cv, &handoff);
    pthread_mutex_unlock(&handoff);
    pthread_mutex_unlock(&resource);
    return NULL;
}

int main(void) {
    rp_idle_lock(&resource, NVN_IDLE_REG_LOCK); pthread_mutex_unlock(&resource);
    enabled = 1;
    rp_idle_lock(&resource, NVN_IDLE_REG_LOCK); pthread_mutex_unlock(&resource);
    CHECK(!clock_calls && !observed.lock_count[NVN_IDLE_REG_LOCK]);
    pthread_t holder;
    CHECK(!pthread_create(&holder, NULL, hold, NULL));
    pthread_mutex_lock(&handoff);
    while (!held) pthread_cond_wait(&cv, &handoff);
    pthread_mutex_unlock(&handoff);
    rp_idle_lock(&resource, NVN_IDLE_VIEW_LOCK); pthread_mutex_unlock(&resource);
    CHECK(!pthread_join(holder, NULL));
    CHECK(observed.lock_count[NVN_IDLE_VIEW_LOCK] == 1 && observed.lock_ns[NVN_IDLE_VIEW_LOCK] == 37 && clock_calls == 2);
    CHECK(!idle_resident_due(60000000008ull, 9) && idle_resident_due(60000000009ull, 9));
    struct idle_account a = {.window_start = 10};
    struct idle_bucket b;
    idle_sample(&a, 8, 1, 220, 245, 10, 40, 1, 0);
    idle_sample(&a, 8, 1, 250, 3, 41, 55, 1, 1);
    idle_sample(&a, 8, 1, 9, 28, 56, 79, 1, 0);
    idle_sample(&a, 8, 1, 15, 20, 60, 81, 1, 0); // entirely overlaps prior segment
    idle_sample(&a, 8, 1, 30, 40, 82, 95, 1, 0);
    CHECK(a.bucket.frames == 1 && a.bucket.samples == 5);
    CHECK(a.bucket.gaps[IDLE_IN_FRAME] == 3 && a.bucket.ticks[IDLE_IN_FRAME] == 7);
    CHECK(a.bucket.gaps[IDLE_AFTER_PRESENT] == 1 && a.bucket.ticks[IDLE_AFTER_PRESENT] == 6);
    CHECK(a.bucket.overlap == 1);
    idle_sample(&a, 8, 1, 0, 0, 96, 100, 0, 0); // unavailable breaks the chain
    idle_sample(&a, 8, 1, 75, 100, 101, 127, 1, 0);
    CHECK(a.bucket.missing == 1 && a.bucket.gaps[0] == 3);
    idle_sample(&a, 8, 1, 120, 126, 257, 265, 1, 1); // half-cycle ambiguity
    CHECK(a.bucket.ambiguous == 1 && a.bucket.gaps[0] == 3);
    idle_sample(&a, 0, 1, 0, 0, 270, 280, 1, 0);
    CHECK(a.bucket.missing == 2 && !a.have_end);
    idle_sample(&a, 64, 1, 400, 410, 4999999980ull, 4999999990ull, 1, 1);
    idle_wait(&a.bucket, NVN_GPU_WAIT_SYNC, 9, 1);
    idle_wait(&a.bucket, NVN_GPU_WAIT_SYNC, 11, 0); // same wait, second wake
    idle_wait(&a.bucket, NVN_GPU_WAIT_FINISH, 37, 1);
    idle_cpu_note(&a.bucket, NVN_IDLE_LAND, IDLE_UNCACHED, 16, 5);
    idle_cpu_note(&a.bucket, NVN_IDLE_READ_TEXELS, IDLE_CACHED, 24, 7);
    CHECK(!idle_take(&a, 5000000009ull, &b));
    CHECK(idle_take(&a, 5000000010ull, &b));
    CHECK(b.waits[NVN_GPU_WAIT_SYNC] == 1 && b.wait_ns[NVN_GPU_WAIT_SYNC] == 20);
    CHECK(b.waits[NVN_GPU_WAIT_FINISH] == 1 && b.wait_ns[NVN_GPU_WAIT_FINISH] == 37);
    CHECK(b.read[0][IDLE_UNCACHED].bytes == 16 && b.read[0][IDLE_UNCACHED].ns == 5);
    CHECK(b.read[1][IDLE_CACHED].bytes == 24 && b.read[1][IDLE_CACHED].count == 1);
    CHECK(!b.read[0][IDLE_CACHED].count && !a.bucket.read[0][IDLE_UNCACHED].count);
    CHECK(!a.bucket.samples && !a.bucket.waits[NVN_GPU_WAIT_SYNC]);
    idle_sample(&a, 64, 1, 415, 430, 5000000010ull, 5000000030ull, 1, 0);
    CHECK(a.bucket.gaps[IDLE_AFTER_PRESENT] == 1 && a.bucket.ticks[IDLE_AFTER_PRESENT] == 5);
    puts("PASS GPU idle: wrap, overlap, present boundary, lost sample, ambiguity, wait sites and bucket rollover");
    return 0;
}
