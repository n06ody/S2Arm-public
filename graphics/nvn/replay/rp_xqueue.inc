// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// The submit lock owns replay, including re-entry after a wait. The event lock
// owns wait predicates and the queue list. A completion callback only sets
// predicates and wakes the worker, so it never waits for the submit lock.
struct xq_state {
    struct rp_state state;
    struct rp_target target;
    rp_pass_state pass;
    int render_skip, render_active, occ_used, occ_counting;
};

struct xq_work {
    struct xq_work *next;
    struct xq_state *saved;
    sync_t *fence;
    const void *wait_sync;
    int count;
    uint64_t handles[];
};

struct xq_queue {
    struct xq_queue *next;
    void *queue;
    struct xq_work *first, *last;
    volatile uint32_t *event;
    uint32_t mode, value;
    int ready, failed;
    const void *waiting_sync;
};

static pthread_mutex_t g_xq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_xq_cv = PTHREAD_COND_INITIALIZER;
static struct xq_queue *g_xq_queues;
static pthread_t g_xq_thread;
static int g_xq_started, g_xq_stopping;

static struct xq_queue *xq_find(void *q) {
    for (struct xq_queue *entry = g_xq_queues; entry; entry = entry->next)
        if (entry->queue == q) return entry;
    return NULL;
}

static struct xq_queue *xq_add(void *q) {
    struct xq_queue *entry = calloc(1, sizeof *entry);
    if (entry) {
        entry->queue = q;
        entry->next = g_xq_queues;
        g_xq_queues = entry;
    }
    return entry;
}

static struct xq_work *xq_work_new(int count) {
    // A parallel array holds the recording block tokens. Ordinary submissions
    // never allocate either array.
    struct xq_work *work = calloc(1, sizeof *work + (size_t)count * 2 * sizeof *work->handles);
    if (work) work->count = count;
    return work;
}

static void xq_work_free(struct xq_work *work) {
    if (nvn_cmd_replay_release)
        for (int i = 0; i < work->count; i++) nvn_cmd_replay_release(work->handles[work->count + i]);
    free(work->saved);
    free(work);
}

static void xq_hold_handles(struct xq_work *work) {
    if (!nvn_cmd_replay_hold || !nvn_cmd_replay_release) return;
    for (int i = 0; i < work->count; i++)
        work->handles[work->count + i] = nvn_cmd_replay_hold(work->handles[i]);
}

static void xq_append(struct xq_queue *entry, struct xq_work *work) {
    if (entry->last) entry->last->next = work;
    else entry->first = work;
    entry->last = work;
}

static void xq_resolve_sync(const void *sync, int published) {
    for (struct xq_queue *q = g_xq_queues; q; q = q->next) {
        if (q->waiting_sync == sync) {
            q->waiting_sync = NULL;
            if (published) q->ready = 1;
            else q->failed = 1;
        }
        if (!published)
            for (struct xq_work *w = q->first; w; w = w->next)
                if (w->wait_sync == sync) q->failed = 1;
    }
}

static void xq_drop(struct xq_queue *entry) {
    struct xq_queue **link = &g_xq_queues;
    while (*link != entry) link = &(*link)->next;
    *link = entry->next;
    while (entry->first) {
        struct xq_work *work = entry->first;
        entry->first = work->next;
        if (work->fence) xq_resolve_sync(work->fence, 0);
        xq_work_free(work);
    }
    free(entry);
    pthread_cond_broadcast(&g_xq_cv);
}

static int xq_condition(uint32_t current, uint32_t mode, uint32_t wanted) {
    if (mode == NVN_EVENT_WAIT_MODE_EQUAL) return current == wanted;
    // Unsigned subtraction gives the signed half-range ordering without an
    // implementation-defined conversion when the high bit is set.
    return current - wanted < (UINT32_C(1) << 31);
}

static void xq_notify_locked(volatile uint32_t *event, uint32_t current) {
    int changed = 0;
    for (struct xq_queue *e = g_xq_queues; e; e = e->next) {
        if (e->event != event || e->ready || e->failed) continue;
        if (xq_condition(current, e->mode, e->value)) e->ready = changed = 1;
    }
    if (changed) pthread_cond_broadcast(&g_xq_cv);
}

static void xq_signal_event(volatile uint32_t *event, uint32_t mode, uint32_t value) {
    pthread_mutex_lock(&g_xq_mu);
    uint32_t current = value;
    if (mode == NVN_EVENT_SIGNAL_MODE_ADD) current = __atomic_add_fetch(event, value, __ATOMIC_RELEASE);
    else __atomic_store_n(event, value, __ATOMIC_RELEASE);
    xq_notify_locked(event, current);
    pthread_mutex_unlock(&g_xq_mu);
}

void nvn_be_event_signal(volatile uint32_t *event, uint32_t mode, uint32_t value) {
    xq_signal_event(event, mode, value);
}

void nvn_be_event_written(volatile uint32_t *event) {
    if (!event) return;
    pthread_mutex_lock(&g_xq_mu);
    xq_notify_locked(event, __atomic_load_n(event, __ATOMIC_ACQUIRE));
    pthread_mutex_unlock(&g_xq_mu);
}

static void *xq_worker(void *unused);

static int xq_event_wait(volatile uint32_t *event, uint32_t mode, uint32_t value, const void *rec) {
    if (!event || (mode != NVN_EVENT_WAIT_MODE_EQUAL && mode != NVN_EVENT_WAIT_MODE_GEQUAL_WRAP)) {
        pthread_mutex_lock(&g_xq_mu);
        struct xq_queue *entry = g_xq_active ? xq_find(g_xq_active) : NULL;
        if (!entry && g_xq_active) entry = xq_add(g_xq_active);
        if (entry) entry->failed = 1;
        pthread_mutex_unlock(&g_xq_mu);
        nvn_unimpl("event", "invalid_wait", rec);
        return 1;
    }
    pthread_mutex_lock(&g_xq_mu);
    if (xq_condition(__atomic_load_n(event, __ATOMIC_ACQUIRE), mode, value)) {
        pthread_mutex_unlock(&g_xq_mu);
        rp_pass_flush(g_cmd);
        if (g_cmd) nvn_gpu_barrier(g_cmd, NVN_BARRIER_ORDER_PRIMITIVES_BIT);
        return 0;
    }
    struct xq_queue *entry = g_xq_active ? xq_find(g_xq_active) : NULL;
    if (!entry && g_xq_active) entry = xq_add(g_xq_active);
    if (entry && !g_xq_started && !g_xq_stopping && nvn_hle_submit_lock && nvn_hle_submit_unlock)
        g_xq_started = pthread_create(&g_xq_thread, NULL, xq_worker, NULL) == 0;
    if (entry) {
        entry->event = event;
        entry->mode = mode;
        entry->value = value;
        entry->ready = 0;
        entry->failed = !g_xq_started || g_xq_stopping;
    }
    pthread_mutex_unlock(&g_xq_mu);
    if (!entry || entry->failed) nvn_unimpl("event", "wait_cannot_park", rec);
    return 1;
}

static struct xq_work *xq_play(void *q, const uint64_t *handles, int count, const struct xq_state *saved) {
    g_in_submit = 1;
    g_xq_active = q;
    nvn_hle_queue_enter(q);
    uint64_t t0 = now_ns();
    if (saved) {
        rp_S = saved->state;
        rp_R = saved->target;
        for (int stage = 0; stage < 6; stage++) rp_tex_sep_gen[stage]++;
        rp_pass_restore(&saved->pass);
        g_occ_used = saved->occ_used;
        g_occ_counting = saved->occ_counting;
    }
    nvn_be_sub_begin();
    nvn_hle_piece_begun();
    if (saved) {
        g_rc_skip = saved->render_skip;
        g_rc_active = saved->render_active;
        if (g_cmd) nvn_gpu_barrier(g_cmd, NVN_BARRIER_ORDER_PRIMITIVES_BIT);
    }
    uint64_t t1 = now_ns();
    nvn_be_sub_index(handles, count);
    int next = 0;
    while (next < count && !g_xq_nresume) {
        const uint64_t handle = handles[next++];
        nvn_be_exec_rest(handles + next, count - next);
        (void)exec_list((const uint8_t *)(uintptr_t)handle);
    }
    nvn_be_exec_rest(NULL, 0);
    uint64_t t2 = now_ns();
    nvn_be_sub_commit();
    uint64_t t3 = now_ns();
    struct xq_work *rest = NULL;
    if (g_xq_nresume) {
        pthread_mutex_lock(&g_xq_mu);
        struct xq_queue *entry = xq_find(q);
        if (entry && entry->event && !entry->failed) {
            const unsigned resumed = g_xq_nresume;
            if (count - next <= INT_MAX - (int)resumed) rest = xq_work_new((int)resumed + count - next);
            if (rest) rest->saved = malloc(sizeof *rest->saved);
            if (rest && rest->saved) {
                for (unsigned i = 0; i < resumed; i++) rest->handles[i] = (uintptr_t)g_xq_resume[i];
                if (next < count)
                    memcpy(rest->handles + resumed, handles + next, (size_t)(count - next) * sizeof *handles);
                xq_hold_handles(rest);
                rest->saved->state = rp_S;
                rest->saved->target = rp_R;
                rest->saved->render_skip = g_rc_skip;
                rest->saved->render_active = g_rc_active;
                rest->saved->occ_used = g_occ_used;
                rest->saved->occ_counting = g_occ_counting;
                rp_pass_save(&rest->saved->pass);
            } else {
                if (rest) xq_work_free(rest);
                rest = NULL;
                entry->failed = 1;
                nvn_unimpl("event", "wait_continuation_allocation", NULL);
            }
        }
        pthread_mutex_unlock(&g_xq_mu);
        g_xq_nresume = 0;
    }
    g_xq_active = NULL;
    g_in_submit = 0;
    pf_add(RP_PERF_SUB_PRE_NS, t1 - t0);
    pf_add(RP_PERF_SUB_EXEC_NS, t2 - t1);
    pf_add(RP_PERF_SUB_POST_NS, t3 - t2);
    pf_add(RP_PERF_SUBMIT_NS, t3 - t0);
    return rest;
}

static struct xq_queue *xq_ready(void) {
    for (struct xq_queue *entry = g_xq_queues; entry; entry = entry->next)
        if (entry->ready && !entry->failed && entry->first) return entry;
    return NULL;
}

static int xq_owed_locked(const void *sync, void **q);

// The caller owns the submit lock; other threads can still deliver GPU or CPU
// signals while replay runs. Keep the work visible until it has been submitted.
static void xq_run_ready(void) {
    pthread_mutex_lock(&g_xq_mu);
    struct xq_queue *entry;
    while (!g_xq_stopping && (entry = xq_ready())) {
        struct xq_work *work = entry->first;
        if (work->fence) {
            sync_publish(work->fence, g_fence);
            entry->first = work->next;
            xq_resolve_sync(work->fence, 1);
        } else if (work->wait_sync) {
            if (xq_owed_locked(work->wait_sync, NULL)) {
                entry->waiting_sync = work->wait_sync;
                entry->ready = 0;
                continue;
            }
            entry->first = work->next;
        } else {
            entry->event = NULL;
            pthread_mutex_unlock(&g_xq_mu);
            struct xq_work *rest = xq_play(entry->queue, work->handles, work->count, work->saved);
            pthread_mutex_lock(&g_xq_mu);
            if (rest) rest->next = work->next;
            entry->first = rest ? rest : work->next;
        }
        if (entry->last == work) entry->last = entry->first;
        xq_work_free(work);
        if (!entry->first && !entry->failed) xq_drop(entry);
        pthread_cond_broadcast(&g_xq_cv);
    }
    pthread_mutex_unlock(&g_xq_mu);
}

static void *xq_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&g_xq_mu);
    while (!g_xq_stopping) {
        if (!xq_ready()) {
            pthread_cond_wait(&g_xq_cv, &g_xq_mu);
            continue;
        }
        pthread_mutex_unlock(&g_xq_mu);
        nvn_hle_submit_lock();
        xq_run_ready();
        nvn_hle_submit_unlock();
        pthread_mutex_lock(&g_xq_mu);
    }
    pthread_mutex_unlock(&g_xq_mu);
    return NULL;
}

void nvn_be_submit(void *q, const uint64_t *h, int n) {
    __atomic_fetch_add(&g_fr_subs, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_xq_mu);
    if (g_xq_stopping) { pthread_mutex_unlock(&g_xq_mu); return; }
    struct xq_queue *entry = xq_find(q);
    if (entry) {
        if (!entry->failed) {
            struct xq_work *work = xq_work_new(n);
            if (work) {
                if (n) memcpy(work->handles, h, (size_t)n * sizeof *h);
                xq_hold_handles(work);
                xq_append(entry, work);
            } else {
                entry->failed = 1;
                nvn_unimpl("event", "queued_submission_allocation", NULL);
            }
        }
        pthread_mutex_unlock(&g_xq_mu);
        return;
    }
    pthread_mutex_unlock(&g_xq_mu);
    struct xq_work *rest = xq_play(q, h, n, NULL);
    if (rest) {
        pthread_mutex_lock(&g_xq_mu);
        xq_append(xq_find(q), rest);
        pthread_cond_broadcast(&g_xq_cv);
        pthread_mutex_unlock(&g_xq_mu);
    }
}

void nvn_be_xq_flush(void *q, const char *why) {
    (void)why;
    xq_run_ready();
    pthread_mutex_lock(&g_xq_mu);
    if (xq_find(q)) nvn_unimpl("event", "queue_still_waiting", NULL);
    pthread_mutex_unlock(&g_xq_mu);
}

int nvn_be_xq_fence(void *q, void *s) {
    pthread_mutex_lock(&g_xq_mu);
    struct xq_queue *entry = xq_find(q);
    if (entry) __atomic_store_n(&((sync_t *)s)->fenced, 0, __ATOMIC_RELEASE);
    if (entry && !entry->failed) {
        struct xq_work *work = xq_work_new(0);
        if (work) {
            work->fence = s;
            xq_append(entry, work);
        } else {
            entry->failed = 1;
            nvn_unimpl("event", "queued_fence_allocation", NULL);
        }
    }
    pthread_mutex_unlock(&g_xq_mu);
    return entry != NULL;
}

static int xq_owed_locked(const void *sync, void **q) {
    if (!sync) return 0;
    for (struct xq_queue *entry = g_xq_queues; entry; entry = entry->next) {
        for (struct xq_work *work = entry->first; work; work = work->next) {
            if (work->fence != sync) continue;
            if (q) *q = entry->queue;
            return 1;
        }
    }
    return 0;
}

int nvn_be_xq_owed(const void *sync, void **q) {
    pthread_mutex_lock(&g_xq_mu);
    int owed = xq_owed_locked(sync, q);
    pthread_mutex_unlock(&g_xq_mu);
    return owed;
}

static struct timespec xq_deadline(uint64_t ns) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += (time_t)(ns / UINT64_C(1000000000));
    t.tv_nsec += (long)(ns % UINT64_C(1000000000));
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return t;
}

int nvn_be_xq_owed_wait(const void *sync, uint64_t ns) {
    const struct timespec deadline = xq_deadline(ns);
    pthread_mutex_lock(&g_xq_mu);
    while (!g_xq_stopping && xq_owed_locked(sync, NULL)) {
        int rc = ns == UINT64_MAX ? pthread_cond_wait(&g_xq_cv, &g_xq_mu) :
                                  pthread_cond_timedwait(&g_xq_cv, &g_xq_mu, &deadline);
        if (rc) break;
    }
    int published = !xq_owed_locked(sync, NULL);
    pthread_mutex_unlock(&g_xq_mu);
    return published;
}

int nvn_be_xq_wait_queue(void *q, void *owner, const void *sync) {
    if (q == owner) return 1;
    pthread_mutex_lock(&g_xq_mu);
    int ok = 1;
    if (xq_owed_locked(sync, NULL)) {
        struct xq_queue *entry = xq_find(q);
        if (!entry) {
            entry = xq_add(q);
            if (entry) entry->ready = 1;
        }
        struct xq_work *work = entry && !entry->failed ? xq_work_new(0) : NULL;
        if (work) {
            work->wait_sync = sync;
            xq_append(entry, work);
            pthread_cond_broadcast(&g_xq_cv);
        } else {
            if (entry) entry->failed = 1;
            nvn_unimpl("event", "queue_dependency_unavailable", NULL);
            ok = 0;
        }
    }
    pthread_mutex_unlock(&g_xq_mu);
    return ok;
}

void nvn_be_xq_discard(void *q) {
    pthread_mutex_lock(&g_xq_mu);
    struct xq_queue *entry = xq_find(q);
    if (entry) {
        entry->failed = 1;
        xq_drop(entry);
    }
    pthread_mutex_unlock(&g_xq_mu);
}

void nvn_be_xq_sync_discard(const void *sync) {
    pthread_mutex_lock(&g_xq_mu);
    if (xq_owed_locked(sync, NULL)) {
        for (struct xq_queue *entry = g_xq_queues; entry; entry = entry->next)
            for (struct xq_work *work = entry->first; work; work = work->next)
                if (work->fence == sync) work->fence = NULL;
        xq_resolve_sync(sync, 0);
        pthread_cond_broadcast(&g_xq_cv);
    }
    pthread_mutex_unlock(&g_xq_mu);
}

int nvn_be_xq_wait_idle(void *q, uint64_t ns) {
    const struct timespec deadline = xq_deadline(ns);
    pthread_mutex_lock(&g_xq_mu);
    struct xq_queue *entry;
    while (!g_xq_stopping && (entry = xq_find(q)) && !entry->failed) {
        if (pthread_cond_timedwait(&g_xq_cv, &g_xq_mu, &deadline)) break;
    }
    int idle = !xq_find(q);
    pthread_mutex_unlock(&g_xq_mu);
    return idle;
}

void rp_xq_request_stop(void) {
    pthread_mutex_lock(&g_xq_mu);
    g_xq_stopping = 1;
    pthread_cond_broadcast(&g_xq_cv);
    pthread_mutex_unlock(&g_xq_mu);
}

void rp_xq_stop(void) {
    rp_xq_request_stop();
    if (g_xq_started) pthread_join(g_xq_thread, NULL);
    pthread_mutex_lock(&g_xq_mu);
    g_xq_started = 0;
    while (g_xq_queues) {
        g_xq_queues->failed = 1;
        xq_drop(g_xq_queues);
    }
    pthread_mutex_unlock(&g_xq_mu);
}

void rp_xq_start(void) {
    pthread_mutex_lock(&g_xq_mu);
    g_xq_stopping = 0;
    pthread_mutex_unlock(&g_xq_mu);
}
