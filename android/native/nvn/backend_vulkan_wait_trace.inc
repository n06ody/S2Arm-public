// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// One capture per session. Use the v5 frame log's clock and emit after the window.
enum { WT_OFF, WT_ARMED, WT_ACTIVE, WT_FINISHED };
enum { WT_LIMIT = 16384 };
struct wait_trace_row {
    const char *kind;
    uint64_t begin, end, call, object, site, target, before, after, wsi, timeout;
    int result;
    uint32_t tid;
    uint64_t extra[12];
};
struct wait_trace_retire {
    uint64_t target, driver_end, cpu_begin, done_enter, callback_enter, callback_exit, done_exit, completed_locked;
};
static pthread_mutex_t wait_trace_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int wait_trace_phase;
static unsigned wait_trace_delay;
static uint64_t wait_trace_begin_ns, wait_trace_end_ns, wait_trace_first;
static uint64_t wait_trace_count, wait_trace_dropped;
static _Atomic uint64_t wait_trace_calls;
static struct wait_trace_row *wait_trace_ring;
static _Thread_local uint64_t wait_trace_call;
static _Thread_local struct wait_trace_retire wait_trace_retire;

static uint64_t wait_trace_clock(void) {
    struct timespec ts;
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t wait_trace_stamp(void) {
    return atomic_load_explicit(&wait_trace_phase, memory_order_relaxed) == WT_ACTIVE ? wait_trace_clock() : 0;
}

static void wait_trace_add(struct wait_trace_row row) {
    if (!row.begin) return;
    pthread_mutex_lock(&wait_trace_mu);
    if (atomic_load_explicit(&wait_trace_phase, memory_order_relaxed) == WT_ACTIVE &&
        row.begin >= wait_trace_begin_ns && row.end < wait_trace_end_ns) {
#ifdef __ANDROID__
        row.tid = (uint32_t)gettid();
#else
        row.tid = (uint32_t)(uintptr_t)pthread_self();
#endif
        if (wait_trace_count < WT_LIMIT) wait_trace_ring[wait_trace_count++] = row;
        else ++wait_trace_dropped;
    }
    pthread_mutex_unlock(&wait_trace_mu);
}

static void wait_trace_open(int delay) {
    wait_trace_delay = delay > 0 && delay <= 600 ? (unsigned)delay : 0;
    wait_trace_first = 0;
    atomic_store_explicit(&wait_trace_phase, wait_trace_delay ? WT_ARMED : WT_OFF, memory_order_relaxed);
}

static void wait_trace_dump(uint64_t actual_end) {
    pthread_mutex_lock(&wait_trace_mu);
    if (atomic_load_explicit(&wait_trace_phase, memory_order_relaxed) != WT_ACTIVE) {
        pthread_mutex_unlock(&wait_trace_mu);
        return;
    }
    atomic_store_explicit(&wait_trace_phase, WT_FINISHED, memory_order_relaxed);
    pthread_mutex_unlock(&wait_trace_mu);
    char line[768];
    snprintf(line, sizeof line, "begin clock=boottime start_ns=%llu stop_ns=%llu cut_ns=%llu rows=%llu dropped=%llu",
             (unsigned long long)wait_trace_begin_ns, (unsigned long long)wait_trace_end_ns,
             (unsigned long long)actual_end, (unsigned long long)wait_trace_count,
             (unsigned long long)wait_trace_dropped);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "wait-trace", line);
    for (uint64_t i = 0; i < wait_trace_count; ++i) {
        const struct wait_trace_row *r = &wait_trace_ring[i];
        int n = snprintf(line, sizeof line,
                 "event i=%llu kind=%s begin_ns=%llu end_ns=%llu tid=%u call=%llu sync=%llx site=%llx target=%llu done0=%llu done1=%llu wsi=%llu timeout_ns=%llu rc=%d",
                 (unsigned long long)i, r->kind, (unsigned long long)r->begin, (unsigned long long)r->end,
                 r->tid, (unsigned long long)r->call, (unsigned long long)r->object, (unsigned long long)r->site,
                 (unsigned long long)r->target, (unsigned long long)r->before, (unsigned long long)r->after,
                 (unsigned long long)r->wsi, (unsigned long long)r->timeout, r->result);
        if (n > 0 && (size_t)n < sizeof line) {
            if (!strcmp(r->kind, "retire"))
                snprintf(line+n, sizeof line-(size_t)n, " done_enter=%llu callback_enter=%llu callback_exit=%llu done_exit=%llu completed_locked=%llu cpu_ns=%llu",
                         (unsigned long long)r->extra[0], (unsigned long long)r->extra[1], (unsigned long long)r->extra[2],
                         (unsigned long long)r->extra[3], (unsigned long long)r->extra[4], (unsigned long long)r->extra[5]);
            else if (!strcmp(r->kind, "sub_done"))
                snprintf(line+n, sizeof line-(size_t)n, " land_begin=%llu land_end=%llu effects_end=%llu replay_publish=%llu invalidate_begin=%llu invalidate_end=%llu",
                         (unsigned long long)r->extra[0], (unsigned long long)r->extra[1],
                         (unsigned long long)r->extra[2], (unsigned long long)r->extra[3],
                         (unsigned long long)r->extra[4], (unsigned long long)r->extra[5]);
            else if (!strcmp(r->kind, "land"))
                snprintf(line+n, sizeof line-(size_t)n, " locked=%llu copy_begin=%llu copy_end=%llu relocked=%llu id=%llu registered=%llu copied=%llu mirror=%llu offset=%llu readback=%llu skipped=%llu probe_armed=%llu",
                         (unsigned long long)r->extra[0], (unsigned long long)r->extra[1], (unsigned long long)r->extra[2],
                         (unsigned long long)r->extra[3], (unsigned long long)r->extra[4], (unsigned long long)r->extra[5],
                         (unsigned long long)r->extra[6], (unsigned long long)r->extra[7], (unsigned long long)r->extra[8],
                         (unsigned long long)r->extra[9], (unsigned long long)r->extra[10], (unsigned long long)r->extra[11]);
        }
        nvn_plat_log(NVN_PLAT_LOG_INFO, "wait-trace", line);
    }
    snprintf(line, sizeof line, "end rows=%llu dropped=%llu", (unsigned long long)wait_trace_count,
             (unsigned long long)wait_trace_dropped);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "wait-trace", line);
    free(wait_trace_ring);
    wait_trace_ring = NULL;
}

static void wait_trace_present(uint64_t wsi) {
    int phase = atomic_load_explicit(&wait_trace_phase, memory_order_relaxed);
    if (phase != WT_ARMED && phase != WT_ACTIVE) return;
    uint64_t now = wait_trace_clock();
    if (phase == WT_ARMED) {
        if (!wait_trace_first) wait_trace_first = now;
        if (now - wait_trace_first < (uint64_t)wait_trace_delay * 1000000000ull) return;
        wait_trace_ring = calloc(WT_LIMIT, sizeof *wait_trace_ring);
        if (!wait_trace_ring) {
            atomic_store_explicit(&wait_trace_phase, WT_FINISHED, memory_order_relaxed);
            nvn_plat_log(NVN_PLAT_LOG_WARN, "wait-trace", "buffer allocation failed");
            return;
        }
        pthread_mutex_lock(&wait_trace_mu);
        wait_trace_begin_ns = now;
        wait_trace_end_ns = now + 10000000000ull;
        wait_trace_count = wait_trace_dropped = 0;
        atomic_store_explicit(&wait_trace_phase, WT_ACTIVE, memory_order_relaxed);
        pthread_mutex_unlock(&wait_trace_mu);
    }
    if (now >= wait_trace_end_ns) { wait_trace_dump(now); return; }
    wait_trace_add((struct wait_trace_row){.kind="present", .begin=now, .end=now, .wsi=wsi});
}

void s2a_nvk_wait_trace_publish(const void *sync, uint64_t fence) {
    uint64_t now = wait_trace_stamp();
    if (!now) return;
    wait_trace_add((struct wait_trace_row){.kind="sync_publish", .begin=now, .end=now,
        .object=(uintptr_t)sync, .target=fence});
}

uint64_t s2a_nvk_wait_trace_begin(const void *sync, uint64_t timeout, uintptr_t site) {
    uint64_t now = wait_trace_stamp();
    if (!now) return 0;
    uint64_t previous = wait_trace_call;
    wait_trace_call = atomic_fetch_add_explicit(&wait_trace_calls, 1, memory_order_relaxed) + 1;
    wait_trace_add((struct wait_trace_row){.kind="guest_begin", .begin=now, .end=now,
        .call=wait_trace_call, .object=(uintptr_t)sync, .site=site, .timeout=timeout});
    return previous + 1;
}

void s2a_nvk_wait_trace_end(uint64_t saved, uint64_t fence, int result) {
    if (!saved) return;
    uint64_t now = wait_trace_stamp();
    wait_trace_add((struct wait_trace_row){.kind="guest_end", .begin=now, .end=now,
        .call=wait_trace_call, .target=fence, .result=result});
    wait_trace_call = saved - 1;
}

void s2a_nvk_wait_trace_fast(uint64_t fence, uint64_t completed, uint64_t timeout, int path) {
    uint64_t now = wait_trace_stamp();
    if (!now) return;
    wait_trace_add((struct wait_trace_row){.kind=path ? "replay_allowed" : "replay_ready", .begin=now, .end=now,
        .call=wait_trace_call, .target=fence, .before=completed, .after=completed, .timeout=timeout});
}

static uint64_t wait_trace_thread_cpu(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t s2a_nvk_wait_trace_retire_stamp(void) {
    return wait_trace_retire.target ? wait_trace_clock() : 0;
}

void s2a_nvk_wait_trace_land(const struct nvn_wait_trace_land *row) {
    if (!row->begin || !wait_trace_retire.target) return;
    wait_trace_add((struct wait_trace_row){.kind="land", .begin=row->begin, .end=row->end,
        .target=wait_trace_retire.target, .extra={row->locked, row->copy_begin, row->copy_end,
        row->relocked, row->id, row->registered, row->copied, row->mirror, row->offset, row->readback, row->skipped, row->probe_armed}});
}

void s2a_nvk_wait_trace_sub(const struct nvn_wait_trace_sub *row) {
    if (!row->begin || !wait_trace_retire.target) return;
    wait_trace_add((struct wait_trace_row){.kind="sub_done", .begin=row->begin, .end=row->end,
        .target=wait_trace_retire.target, .extra={row->land_begin, row->land_end, row->effects_end, row->replay_publish,
        row->invalidate_begin, row->invalidate_end}});
}

static void wait_trace_retire_end(uint64_t published, uint64_t cpu_end) {
    struct wait_trace_retire t = wait_trace_retire;
    wait_trace_retire = (struct wait_trace_retire){0};
    if (!t.target) return;
    uint64_t cpu = t.cpu_begin && cpu_end >= t.cpu_begin ? cpu_end - t.cpu_begin : 0;
    wait_trace_add((struct wait_trace_row){.kind="retire", .begin=t.driver_end, .end=published,
        .target=t.target, .extra={t.done_enter, t.callback_enter, t.callback_exit,
        t.done_exit, t.completed_locked, cpu}});
}
