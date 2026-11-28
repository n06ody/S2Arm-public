// 管线首用待定表（纯逻辑）。合同见 nvn_pipe_pend.h。
#include "nvn_pipe_pend.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static void pp_ensure(nvk_pp *t) {
    if (t->ready) return;
    if (t->key_bytes == 0 || t->key_bytes > NVK_PP_KEY_MAX) return;   // 停用
    int ok = 1;
    for (int i = 0; i < NVK_PP_MAX; i++) {
        if (pthread_cond_init(&t->ent[i].cv, NULL) != 0) { ok = 0; break; }
    }
    t->ready = ok;
}

void nvk_pp_init(nvk_pp *t, size_t key_bytes) {
    if (!t) return;
    memset(t, 0, sizeof *t);
    pthread_mutex_init(&t->mu, NULL);
    t->key_bytes = key_bytes;
    pthread_mutex_lock(&t->mu);
    pp_ensure(t);
    pthread_mutex_unlock(&t->mu);
}

void nvk_pp_reset(nvk_pp *t) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    t->reset_seq++;
    for (int i = 0; i < NVK_PP_MAX; i++) {
        struct nvk_pp_ent *e = &t->ent[i];
        if (!e->used) continue;
        e->used = 0; e->state = NVK_PP_NONE; e->obj = NULL; e->late = e->stuck = 0;
        pthread_cond_broadcast(&e->cv);   // 唤醒等待者，免得 reset 后还睡到自己的超时
    }
    pthread_mutex_unlock(&t->mu);
}

static int pp_find_locked(nvk_pp *t, const void *key, int (*eq)(const void *, const void *)) {
    for (int i = 0; i < NVK_PP_MAX; i++) {
        if (t->ent[i].used && eq(t->ent[i].key, key)) return i;
    }
    return -1;
}

// 找可放新项的位置：空槽优先；否则淘汰最久未触的已终态（READY/FAILED）项；全是 COMPILING → -1。
static int pp_alloc_locked(nvk_pp *t) {
    int victim = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < NVK_PP_MAX; i++) {
        struct nvk_pp_ent *e = &t->ent[i];
        if (!e->used) return i;
        if (e->state != NVK_PP_COMPILING && e->last < oldest) { oldest = e->last; victim = i; }
    }
    if (victim >= 0) { t->evicted++; t->ent[victim].used = 0; return victim; }
    return -1;
}

int nvk_pp_enter(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), int *handle) {
    if (!t || !key || !eq || !handle) return NVK_PP_ENTER_FULL;
    *handle = -1;
    pthread_mutex_lock(&t->mu);
    pp_ensure(t);
    if (!t->ready) { t->full++; pthread_mutex_unlock(&t->mu); return NVK_PP_ENTER_FULL; }
    t->enters++;
#ifdef NVK_PP_NEG_NO_DEDUP
    int at = -1;   // 负对照：关掉同键合并 → 同键也新占一格
#else
    int at = pp_find_locked(t, key, eq);
#endif
    if (at >= 0) {
        t->ent[at].last = t->enters;
        t->followers++;
        *handle = at;
        pthread_mutex_unlock(&t->mu);
        return NVK_PP_ENTER_FOLLOW;
    }
    at = pp_alloc_locked(t);
    if (at < 0) { t->full++; pthread_mutex_unlock(&t->mu); return NVK_PP_ENTER_FULL; }
    struct nvk_pp_ent *e = &t->ent[at];
    e->used = 1;
    e->state = NVK_PP_COMPILING;
    e->obj = NULL;
    e->code = 0;
    e->why[0] = 0;
    e->late = e->stuck = 0;
    e->last = t->enters;
    memcpy(e->key, key, t->key_bytes);
    *handle = at;
    pthread_mutex_unlock(&t->mu);
    return NVK_PP_ENTER_BUILDER;
}

int nvk_pp_reclaim_ready(nvk_pp *t, int handle) {
    if (!t || handle < 0 || handle >= NVK_PP_MAX) return 0;
    pthread_mutex_lock(&t->mu);
    struct nvk_pp_ent *e = &t->ent[handle];
    const int ready = e->used && e->state == NVK_PP_READY;
    if (ready) {
        e->state = NVK_PP_COMPILING;
        e->obj = NULL;
        e->late = e->stuck = 0;
    }
    pthread_mutex_unlock(&t->mu);
    return ready;
}

void nvk_pp_publish(nvk_pp *t, int handle, void *obj, int code, const char *why) {
    if (!t || handle < 0 || handle >= NVK_PP_MAX) return;
    pthread_mutex_lock(&t->mu);
    struct nvk_pp_ent *e = &t->ent[handle];
    if (e->used) {
        e->obj = code ? NULL : obj;
        e->code = code;
        if (why && code) {
            strncpy(e->why, why, sizeof e->why - 1);
            e->why[sizeof e->why - 1] = 0;
        }
        e->state = code ? NVK_PP_FAILED : NVK_PP_READY;
        if (code) t->failed++; else t->published++;
        pthread_cond_broadcast(&e->cv);
    }
    pthread_mutex_unlock(&t->mu);
}

void nvk_pp_abandon(nvk_pp *t, int handle) {
    if (!t || handle < 0 || handle >= NVK_PP_MAX) return;
    pthread_mutex_lock(&t->mu);
    struct nvk_pp_ent *e = &t->ent[handle];
    if (e->used && e->state == NVK_PP_COMPILING) {
        e->used = 0;
        e->state = NVK_PP_NONE;
        pthread_cond_broadcast(&e->cv);
    }
    pthread_mutex_unlock(&t->mu);
}

static void pp_set_out(void *obj, int code, const char *why, char *w, size_t cap, void **outobj, int *outcode) {
    if (outobj) *outobj = obj;
    if (outcode) *outcode = code;
    if (w && cap) {
        if (why) { strncpy(w, why, cap - 1); w[cap - 1] = 0; } else { w[0] = 0; }
    }
}

int nvk_pp_state(nvk_pp *t, const void *key, int (*eq)(const void *, const void *),
                 void **obj, int *code, char *why, size_t why_cap) {
    pp_set_out(NULL, 0, NULL, why, why_cap, obj, code);
    if (!t || !key || !eq) return NVK_PP_NONE;
    pthread_mutex_lock(&t->mu);
    int at = t->ready ? pp_find_locked(t, key, eq) : -1;
    int st = NVK_PP_NONE;
    if (at >= 0) {
        struct nvk_pp_ent *e = &t->ent[at];
        st = e->state;
        if (st == NVK_PP_READY) pp_set_out(e->obj, 0, NULL, why, why_cap, obj, code);
        else if (st == NVK_PP_FAILED) pp_set_out(NULL, e->code, e->why, why, why_cap, obj, code);
    }
    pthread_mutex_unlock(&t->mu);
    return st;
}

int nvk_pp_wait(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), uint32_t wait_ms,
                void **obj, int *code, char *why, size_t why_cap) {
    pp_set_out(NULL, 0, NULL, why, why_cap, obj, code);
    if (!t || !key || !eq) return NVK_PP_WAIT_ABSENT;
    pthread_mutex_lock(&t->mu);
    int at = t->ready ? pp_find_locked(t, key, eq) : -1;
    if (at < 0) { pthread_mutex_unlock(&t->mu); return NVK_PP_WAIT_ABSENT; }
    struct nvk_pp_ent *e = &t->ent[at];
    const uint64_t reset_seq = t->reset_seq;
    if (e->state == NVK_PP_COMPILING && wait_ms) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += (time_t)(wait_ms / 1000);
        dl.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (t->reset_seq == reset_seq && e->state == NVK_PP_COMPILING) {
            if (pthread_cond_timedwait(&e->cv, &t->mu, &dl) == ETIMEDOUT) break;
        }
    }
    int rc;
    // reset 已取消本次等待；新一轮条目即使已完成，也不是旧等待的结果。
    if (t->reset_seq != reset_seq) {
        rc = NVK_PP_WAIT_ABSENT;
    } else if (e->state == NVK_PP_READY) {
        rc = NVK_PP_WAIT_READY;
        pp_set_out(e->obj, 0, NULL, why, why_cap, obj, code);
    } else if (e->state == NVK_PP_FAILED) {
        rc = NVK_PP_WAIT_FAILED;
        pp_set_out(NULL, e->code, e->why, why, why_cap, obj, code);
    } else {
        rc = NVK_PP_WAIT_TIMEOUT;
        if (e->late) t->timeouts_late++;
        if (e->stuck) t->timeouts_stuck++;
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}

void nvk_pp_mark(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), int is_stuck) {
    if (!t || !key || !eq) return;
    pthread_mutex_lock(&t->mu);
    int at = t->ready ? pp_find_locked(t, key, eq) : -1;
    if (at >= 0) {
        if (is_stuck) t->ent[at].stuck = 1; else t->ent[at].late = 1;
        t->ent[at].last = ++t->enters;
    }
    pthread_mutex_unlock(&t->mu);
}

int nvk_pp_marks(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), int *late, int *stuck) {
    if (late) *late = 0;
    if (stuck) *stuck = 0;
    if (!t || !key || !eq) return 0;
    pthread_mutex_lock(&t->mu);
    int at = t->ready ? pp_find_locked(t, key, eq) : -1;
    int found = at >= 0;
    if (found) {
        if (late) *late = t->ent[at].late;
        if (stuck) *stuck = t->ent[at].stuck;
    }
    pthread_mutex_unlock(&t->mu);
    return found;
}

void nvk_pp_stats(nvk_pp *t, uint64_t *enters, uint64_t *followers, uint64_t *published, uint64_t *failed,
                  uint64_t *timeouts_late, uint64_t *timeouts_stuck, uint64_t *full, uint64_t *evicted) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    if (enters) *enters = t->enters;
    if (followers) *followers = t->followers;
    if (published) *published = t->published;
    if (failed) *failed = t->failed;
    if (timeouts_late) *timeouts_late = t->timeouts_late;
    if (timeouts_stuck) *timeouts_stuck = t->timeouts_stuck;
    if (full) *full = t->full;
    if (evicted) *evicted = t->evicted;
    pthread_mutex_unlock(&t->mu);
}
