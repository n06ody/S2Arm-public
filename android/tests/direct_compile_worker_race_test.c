// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real single-slot worker with a gated compiler. The wait adapter delays only
// reacquisition after a condition wait: this models legal scheduler delay.
#define _GNU_SOURCE 1
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int test_cond_wait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
#define pthread_cond_timedwait test_cond_wait
#include "../native/direct/compile_worker.c"
#undef pthread_cond_timedwait

static int failures, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", msg); fflush(stdout); } } while (0)
static pthread_mutex_t ctl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ctl_cv = PTHREAD_COND_INITIALIZER;
static _Thread_local int role;
static int entered, allow_compile, attach_waiting;
static int arm_old, old_paused, allow_old, arm_new, new_paused, allow_new;
static uint8_t new_sha[32];

static void deadline(struct timespec *t, int ms) {
    clock_gettime(CLOCK_REALTIME, t); t->tv_sec += ms / 1000; t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}
static int wait_flag(int *p) {
    struct timespec t; deadline(&t, 4000);
    pthread_mutex_lock(&ctl_mu);
    while (!*p) if (pthread_cond_timedwait(&ctl_cv, &ctl_mu, &t) == ETIMEDOUT) break;
    int ok = *p; pthread_mutex_unlock(&ctl_mu); return ok;
}
static void set_flag(int *p, int v) { pthread_mutex_lock(&ctl_mu); *p = v; pthread_cond_broadcast(&ctl_cv); pthread_mutex_unlock(&ctl_mu); }
static void reset_gate(void) {
    pthread_mutex_lock(&ctl_mu);
    entered = allow_compile = attach_waiting = arm_old = old_paused = allow_old = arm_new = new_paused = allow_new = 0;
    pthread_mutex_unlock(&ctl_mu);
}
static int test_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *t) {
    pthread_mutex_lock(&ctl_mu);
    if (role == 3) { attach_waiting = 1; pthread_cond_broadcast(&ctl_cv); }
    pthread_mutex_unlock(&ctl_mu);
    int r = pthread_cond_timedwait(cv, mu, t);
    pthread_mutex_lock(&ctl_mu);
    int hold_old = role == 1 && arm_old && r == ETIMEDOUT;
    int hold_new = role == 2 && arm_new && r == 0 && g_job.state == 2 && !memcmp(g_job.sha, new_sha, 32);
    pthread_mutex_unlock(&ctl_mu);
    if (hold_old || hold_new) {
        pthread_mutex_unlock(mu);  // simulate scheduler delay before reacquiring the worker mutex
        pthread_mutex_lock(&ctl_mu);
        if (hold_old) { old_paused = 1; pthread_cond_broadcast(&ctl_cv); while (!allow_old) pthread_cond_wait(&ctl_cv, &ctl_mu); }
        else { new_paused = 1; pthread_cond_broadcast(&ctl_cv); while (!allow_new) pthread_cond_wait(&ctl_cv, &ctl_mu); }
        pthread_mutex_unlock(&ctl_mu);
        pthread_mutex_lock(mu);
    }
    return r;
}
static int gated_compile(void *ctx, uint64_t session, uint64_t generation, const char *json, size_t len,
                         uint8_t **bytes, size_t *n, char *why, size_t wn) {
    (void)ctx; (void)session; (void)generation; (void)why; (void)wn;
    pthread_mutex_lock(&ctl_mu); entered = 1; pthread_cond_broadcast(&ctl_cv);
    while (!allow_compile) pthread_cond_wait(&ctl_cv, &ctl_mu);
    pthread_mutex_unlock(&ctl_mu);
    *bytes = malloc(len); if (!*bytes) return -1; memcpy(*bytes, json, len); *n = len; return 0;
}
struct call { const char *json; uint8_t sha[32]; int ms, role, rc; uint8_t *bytes; size_t n; char why[128]; };
static void init_call(struct call *c, const char *s, int ms, int which) {
    memset(c, 0, sizeof *c); c->json = s; c->ms = ms; c->role = which; memcpy(c->sha, s, strlen(s));
}
static void *run_call(void *arg) {
    struct call *c = arg; role = c->role;
    c->rc = s2a_compile_worker_run(1, 1, c->json, strlen(c->json), c->sha, c->ms,
                                   NULL, NULL, NULL, &c->bytes, &c->n, c->why, sizeof c->why);
    return NULL;
}
static int result_is(struct call *c, const char *s) { return c->rc == 0 && c->bytes && c->n == strlen(s) && !memcmp(c->bytes, s, c->n); }
static void release_compile(void) { set_flag(&allow_compile, 1); }

static void reattach(int async_mode) {
    reset_gate(); CHECK(!s2a_compile_worker_start(gated_compile, NULL, NULL, 0), "reattach start");
    struct call first, next; pthread_t a, b; init_call(&first, "same", 40, 0); init_call(&next, "same", 2000, 3);
    if (async_mode == 2) {
        CHECK(s2a_compile_worker_submit_async(1, 1, first.json, strlen(first.json), first.sha, NULL, 0) == 0, "async initial submit");
        CHECK(wait_flag(&entered), "async initial compiler entered");
        first.ms = 40; pthread_create(&a, NULL, run_call, &first); pthread_join(a, NULL);
        CHECK(first.rc != 0 && strstr(first.why, "timeout"), "sync waiter timed out while async subscribed");
    } else {
        pthread_create(&a, NULL, run_call, &first); CHECK(wait_flag(&entered), "initial compiler entered"); pthread_join(a, NULL);
        CHECK(first.rc != 0 && strstr(first.why, "timeout"), "first sync waiter timed out");
        if (!async_mode) { pthread_create(&b, NULL, run_call, &next); CHECK(wait_flag(&attach_waiting), "same SHA sync reattached"); }
        else CHECK(!s2a_compile_worker_submit_async(1, 1, next.json, strlen(next.json), next.sha, NULL, 0), "same SHA async reattached");
    }
    pthread_mutex_lock(&g_wmu); int abandoned = g_job.abandoned, refs = g_job.refs; pthread_mutex_unlock(&g_wmu);
    printf("F reattach mode=%d abandoned=%d refs=%d\n", async_mode, abandoned, refs);
    CHECK(!abandoned && (async_mode || refs == 1), "reattached subscriber keeps job live and its own ref");
    release_compile();
    if (!async_mode) { pthread_join(b, NULL); CHECK(result_is(&next, "same"), "sync reattach receives original compile result"); }
    else {
        uint8_t *bytes = NULL; size_t n = 0; char why[128] = "";
        int rc = s2a_compile_worker_wait_async(first.sha, 2000, &bytes, &n, why, sizeof why);
        printf("F async mode=%d rc=%d reason=%s\n", async_mode, rc, why);
        CHECK(rc == 0 && bytes && n == 4 && !memcmp(bytes, "same", 4), "async reattach publishes successful result, not extra failure"); free(bytes);
    }
    free(first.bytes); free(next.bytes); s2a_compile_worker_stop();
}
static void replacement(int completed) {
    reset_gate(); set_flag(&arm_old, 1); CHECK(!s2a_compile_worker_start(gated_compile, NULL, NULL, 0), "replacement start A");
    struct call a, b; pthread_t ta, tb; init_call(&a, "A", 40, 1); init_call(&b, "B", 2000, 2); memcpy(new_sha, b.sha, 32);
    pthread_create(&ta, NULL, run_call, &a); CHECK(wait_flag(&entered), "A compiler entered"); CHECK(wait_flag(&old_paused), "A timed out before reacquiring worker mutex");
    CHECK(s2a_compile_worker_stop_bounded(1, NULL, 0) == 1, "stop leaves gated A in flight"); release_compile();
    CHECK(s2a_compile_worker_stop_bounded(2000, NULL, 0) == 0, "stopped A is reclaimed");
    pthread_mutex_lock(&ctl_mu); entered = 0; allow_compile = 0; arm_new = completed; pthread_mutex_unlock(&ctl_mu);
    CHECK(!s2a_compile_worker_start(gated_compile, NULL, NULL, 0), "replacement start B");
    pthread_create(&tb, NULL, run_call, &b); CHECK(wait_flag(&entered), "B compiler entered");
    if (completed) { release_compile(); CHECK(wait_flag(&new_paused), "B completed before A resumes"); }
    set_flag(&allow_old, 1); pthread_join(ta, NULL);
    pthread_mutex_lock(&g_wmu); int refs = g_job.refs, abandoned = g_job.abandoned; pthread_mutex_unlock(&g_wmu);
    printf("F replacement completed=%d old_rc=%d old_len=%zu old_first=%02x B_refs=%d B_abandoned=%d\n", completed, a.rc, a.n, a.bytes && a.n ? a.bytes[0] : 0, refs, abandoned);
    CHECK(a.rc != 0 && !a.bytes && !a.n, "old A never receives B bytes");
    CHECK(refs == 1 && !abandoned, "old A never removes B reference or abandons B");
    if (completed) set_flag(&allow_new, 1); else release_compile();
    pthread_join(tb, NULL); CHECK(result_is(&b, "B"), "B receives its own compile result");
    free(a.bytes); free(b.bytes); s2a_compile_worker_stop();
}
int main(void) {
    alarm(25); reattach(0); reattach(1); reattach(2); replacement(0); replacement(1);
    printf("compile worker races: %d checks %d failures\n", checks, failures); return failures ? 1 : 0;
}
