#define _GNU_SOURCE 1
// F4.4 宿主回归：单槽需求编译 worker —— 同请求去重、不同请求排队、Stop 取消、
// 超时放弃后丢弃迟到结果、可重启、pause/resume 在等待前后成对出现。
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "direct/compile_worker.h"

static int fails, checks;
#define CHECK(cond, what) do { checks++; if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static pthread_mutex_t g_fn_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_calls, g_concurrent, g_max_concurrent;
static int g_delay_ms = 40;
static int g_fail_next;

static int compile_fn(void *ctx, uint64_t session, uint64_t generation, const char *json, size_t len,
                      uint8_t **bytes, size_t *bytes_len, char *why, size_t why_size) {
    (void)ctx; (void)session; (void)generation; (void)why; (void)why_size;
    pthread_mutex_lock(&g_fn_mu);
    g_calls++;
    g_concurrent++;
    if (g_concurrent > g_max_concurrent) g_max_concurrent = g_concurrent;
    int fail = g_fail_next;
    g_fail_next = 0;
    pthread_mutex_unlock(&g_fn_mu);
    usleep((unsigned)g_delay_ms * 1000);
    pthread_mutex_lock(&g_fn_mu);
    g_concurrent--;
    pthread_mutex_unlock(&g_fn_mu);
    if (fail) return -1;
    *bytes = (uint8_t *)malloc(len + 1);
    if (!*bytes) return -1;
    memcpy(*bytes, json, len);
    (*bytes)[len] = 0;
    *bytes_len = len;
    return 0;
}

static int g_pause_count, g_resume_count;
static int pause_cb(void *ctx) { (void)ctx; __atomic_add_fetch(&g_pause_count, 1, __ATOMIC_SEQ_CST); return 0; }
static void resume_cb(void *ctx) { (void)ctx; __atomic_add_fetch(&g_resume_count, 1, __ATOMIC_SEQ_CST); }

struct run_arg {
    const char *json;
    uint8_t sha[32];
    int deadline;
    int rc;
    size_t len;
    uint8_t *bytes;
    char why[192];
    int use_pause;
};

static void *run_thread(void *p) {
    struct run_arg *a = (struct run_arg *)p;
    a->rc = s2a_compile_worker_run(1, 1, a->json, strlen(a->json), a->sha, a->deadline,
                                   a->use_pause ? pause_cb : NULL, a->use_pause ? resume_cb : NULL,
                                   NULL, &a->bytes, &a->len, a->why, sizeof a->why);
    return NULL;
}

static void sha_of(const char *s, uint8_t out[32]) {
    memset(out, 0, 32);
    memcpy(out, s, strlen(s) < 32 ? strlen(s) : 32);
}

// No sleeps establish these interleavings: the compiler publishes entry under
// a condition variable and remains blocked until the controlling test releases it.
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_entered, gate_release;
static int gated_compile(void *ctx, uint64_t session, uint64_t generation,
                         const char *json, size_t len, uint8_t **bytes,
                         size_t *bytes_len, char *why, size_t why_size) {
    pthread_mutex_lock(&gate_mu);
    gate_entered = 1;
    pthread_cond_broadcast(&gate_cv);
    while (!gate_release) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
    return compile_fn(ctx, session, generation, json, len, bytes, bytes_len, why, why_size);
}

struct pause_probe { int pauses, resumes, stop; };
static int refused_pause(void *ctx) {
    struct pause_probe *p = ctx;
    p->pauses++;
    // Public query also proves the callback is outside the worker mutex.
    (void)s2a_compile_worker_active();
    return -1;
}
static int accepted_pause(void *ctx) {
    struct pause_probe *p = ctx;
    p->pauses++;
    (void)s2a_compile_worker_active();
    if (p->stop) s2a_compile_worker_stop();
    if (p->stop == 2) (void)s2a_compile_worker_start(compile_fn, NULL, NULL, 0);
    return 0;
}
static void probe_resume(void *ctx) {
    struct pause_probe *p = ctx;
    p->resumes++;
    (void)s2a_compile_worker_active();
}
static void *owner_probe(void *ctx) {
    pthread_mutex_t *owner = ctx;
    int rc = pthread_mutex_trylock(owner);
    CHECK(rc == 0, "normal caller return releases its own owner lock");
    if (rc == 0) pthread_mutex_unlock(owner);
    return NULL;
}
static void rejected_run(const uint8_t sha[32], const char *label) {
    struct pause_probe p = {0};
    struct s2a_compile_worker_stats before, after;
    pthread_mutex_t owner = PTHREAD_MUTEX_INITIALIZER;
    uint8_t *bytes = (uint8_t *)1;
    size_t len = 99;
    char why[192] = "";
    s2a_compile_worker_stats(&before);
    pthread_mutex_lock(&owner);
    int rc = s2a_compile_worker_run(1, 1, "reject", 6, sha, 30,
                                  refused_pause, probe_resume, &p, &bytes, &len, why, sizeof why);
    // Exactly the real caller's error-return responsibility, not worker unlock.
    pthread_mutex_unlock(&owner);
    pthread_t t;
    pthread_create(&t, NULL, owner_probe, &owner);
    pthread_join(t, NULL);
    pthread_mutex_destroy(&owner);
    s2a_compile_worker_stats(&after);
    CHECK(rc == -1 && strstr(why, "pause failed"), label);
    CHECK(bytes == NULL && len == 0, "refused pause clears outputs");
    CHECK(p.pauses == 1 && p.resumes == 0, "failed pause must never be resumed");
    CHECK(before.runs == after.runs && before.dedup == after.dedup &&
          before.compiles == after.compiles && before.busy_timeouts == after.busy_timeouts,
          "refused pause publishes/attaches/waits for no job");
    if (bytes && bytes != (uint8_t *)1) free(bytes);
}
static void test_pause_contract(void) {
    char why[192];
    uint8_t sha[32] = {91};
    g_delay_ms = 0;
    CHECK(!s2a_compile_worker_start(compile_fn, NULL, why, sizeof why), "start pause tests");
    rejected_run(sha, "idle failure reports pause failure before publishing");
    for (int half = 0; half < 2; half++) {
        struct pause_probe p = {0};
        uint8_t *bytes = NULL; size_t len = 0;
        int rc = s2a_compile_worker_run(1, 1, "half", 4, sha, 100,
                    half ? NULL : accepted_pause, half ? probe_resume : NULL,
                    &p, &bytes, &len, why, sizeof why);
        CHECK(rc == -1 && strstr(why, "callback pair"), "half callback pair rejected");
        CHECK(!p.pauses && !p.resumes && !bytes && !len, "half pair has no side effects");
        free(bytes);
    }
    // Stop from a successful pause must be rechecked before publishing a job.
    struct pause_probe p = {0, 0, 1};
    uint8_t *bytes = NULL; size_t len = 0;
    int rc = s2a_compile_worker_run(1, 1, "stop", 4, sha, 100,
                 accepted_pause, probe_resume, &p, &bytes, &len, why, sizeof why);
    CHECK(rc == -1 && strstr(why, "stop") && !bytes && !len,
          "Stop during pause rejects before publishing");
    CHECK(p.pauses == 1 && p.resumes == 1, "successful pause paired even when Stop intervenes");
    free(bytes);
    s2a_compile_worker_stop();
    CHECK(!s2a_compile_worker_start(compile_fn, NULL, why, sizeof why), "start before pause restart");
    struct s2a_compile_worker_stats before, after;
    s2a_compile_worker_stats(&before);
    p = (struct pause_probe){0, 0, 2}; bytes = NULL; len = 0;
    rc = s2a_compile_worker_run(1, 1, "old-epoch", 9, sha, 100,
                 accepted_pause, probe_resume, &p, &bytes, &len, why, sizeof why);
    s2a_compile_worker_stats(&after);
    CHECK(rc == -1 && strstr(why, "restarted") && !bytes && !len,
          "pause stop/start cannot publish an old request into the new worker");
    CHECK(p.pauses == 1 && p.resumes == 1 && before.compiles == after.compiles,
          "epoch rejection resumes once without compiling");
    free(bytes);
    s2a_compile_worker_stop();

    gate_entered = gate_release = 0;
    CHECK(!s2a_compile_worker_start(gated_compile, NULL, why, sizeof why), "start gated worker");
    struct run_arg first = {.json = "in-flight", .deadline = 5000};
    memcpy(first.sha, sha, 32);
    pthread_t t;
    pthread_create(&t, NULL, run_thread, &first);
    pthread_mutex_lock(&gate_mu);
    while (!gate_entered) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
    rejected_run(sha, "same SHA refused pause cannot attach to existing job");
    sha[0]++;
    rejected_run(sha, "different SHA refused pause cannot wait on occupied slot");
    // Successful pause on the occupied slot still resumes after its deadline.
    p = (struct pause_probe){0}; bytes = NULL; len = 0;
    rc = s2a_compile_worker_run(1, 1, "busy", 4, sha, 10,
                 accepted_pause, probe_resume, &p, &bytes, &len, why, sizeof why);
    CHECK(rc == -1 && strstr(why, "slot busy timeout"), "busy-slot timeout retained");
    CHECK(p.pauses == 1 && p.resumes == 1 && !bytes && !len, "busy timeout pairs callbacks");
    pthread_mutex_lock(&gate_mu);
    gate_release = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
    pthread_join(t, NULL);
    CHECK(first.rc == 0 && first.len == strlen(first.json) &&
          !memcmp(first.bytes, first.json, first.len), "other callers' failed pauses preserve in-flight result");
    free(first.bytes);
    s2a_compile_worker_stop();
}

#ifdef S2A_WORKER_TEST_MALLOC
// Compile only compile_worker.c with -Dmalloc=s2a_worker_test_malloc. The
// compiler callback's allocations remain real malloc; no production test hook.
static int allocation_count, fail_allocation;
void *s2a_worker_test_malloc(size_t len) {
    if (++allocation_count == fail_allocation) return NULL;
    return malloc(len);
}
static void test_pause_oom(void) {
    char why[192]; uint8_t sha[32] = {71};
    CHECK(!s2a_compile_worker_start(compile_fn, NULL, why, sizeof why), "start OOM tests");
    for (int nth = 1; nth <= 2; nth++) {
        allocation_count = 0; fail_allocation = nth;
        struct pause_probe p = {0}; uint8_t *bytes = NULL; size_t len = 0;
        int rc = s2a_compile_worker_run(1, 1, "oom", 3, sha, 100,
                     accepted_pause, probe_resume, &p, &bytes, &len, why, sizeof why);
        CHECK(rc == -1 && strstr(why, "out of memory") && !bytes && !len,
              "JSON/result allocation failures clear outputs");
        CHECK(p.pauses == 1 && p.resumes == 1, "JSON/result OOM pairs successful pause");
        fail_allocation = 0;
        rc = s2a_compile_worker_run(1, 1, "ok", 2, sha, 100,
                     accepted_pause, probe_resume, &p, &bytes, &len, why, sizeof why);
        CHECK(!rc && len == 2 && !memcmp(bytes, "ok", 2), "OOM leaves worker reusable");
        free(bytes);
    }
    s2a_compile_worker_stop();
}
#endif

int main(void) {
    alarm(30); // harness watchdog only; never an expected success/failure oracle
    test_pause_contract();
    g_delay_ms = 40;
#ifdef S2A_WORKER_TEST_MALLOC
    test_pause_oom();
#endif
    char why[192] = "";
    CHECK(s2a_compile_worker_active() == 0, "worker is not active before start");
    uint8_t *pre_bytes = NULL;
    size_t pre_len = 0;
    uint8_t pre_sha[32] = {0};
    CHECK(s2a_compile_worker_run(1, 1, "x", 1, pre_sha, 100, NULL, NULL, NULL,
                                 &pre_bytes, &pre_len, why, sizeof why) != 0,
          "run without a worker fails accurately");
    CHECK(s2a_compile_worker_start(compile_fn, NULL, why, sizeof why) == 0, "start worker");
    CHECK(s2a_compile_worker_active() == 1, "worker is active");
    CHECK(s2a_compile_worker_start(compile_fn, NULL, why, sizeof why) == 0, "start is idempotent");

    // 1) 同 sha 的并发需求合并成一次编译；计数必须反映"入队 4 次、真实编译 1 次"。
    struct s2a_compile_worker_stats stats_before, stats_after;
    s2a_compile_worker_stats(&stats_before);
    g_calls = 0; g_concurrent = 0; g_max_concurrent = 0;
    struct run_arg a[4];
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        memset(&a[i], 0, sizeof a[i]);
        a[i].json = "{\"same\":1}";
        sha_of("same-request", a[i].sha);
        a[i].deadline = 5000;
        pthread_create(&th[i], NULL, run_thread, &a[i]);
    }
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < 4; i++) {
        CHECK(a[i].rc == 0 && a[i].len == strlen(a[i].json) && !memcmp(a[i].bytes, a[i].json, a[i].len),
              "deduped request returns the compile result");
        free(a[i].bytes);
    }
    CHECK(g_calls == 1, "four identical requests compile once");
    CHECK(g_max_concurrent == 1, "compiles never overlap");
    s2a_compile_worker_stats(&stats_after);
    CHECK(stats_after.compiles - stats_before.compiles == 1,
          "stats: four identical requests count exactly one real compile");
    CHECK(stats_after.runs - stats_before.runs == 4, "stats: every run() is counted");
    CHECK(stats_after.dedup - stats_before.dedup == 3,
          "stats: the three attached waiters are counted as dedupe");

    // 2) 不同请求串行（容量 1）。
    g_calls = 0; g_max_concurrent = 0; g_concurrent = 0;
    struct run_arg b[2];
    pthread_t tb[2];
    for (int i = 0; i < 2; i++) {
        memset(&b[i], 0, sizeof b[i]);
        b[i].json = i ? "{\"second\":1}" : "{\"first\":1}";
        sha_of(i ? "second-request" : "first-request", b[i].sha);
        b[i].deadline = 5000;
        pthread_create(&tb[i], NULL, run_thread, &b[i]);
    }
    for (int i = 0; i < 2; i++) pthread_join(tb[i], NULL);
    for (int i = 0; i < 2; i++) { CHECK(b[i].rc == 0, "queued request completes"); free(b[i].bytes); }
    CHECK(g_calls == 2, "two different requests compile separately");
    CHECK(g_max_concurrent == 1, "capacity is one compile");

    // 3) 等待期间调用 pause/resume（外部锁可交给 owner）。
    g_pause_count = g_resume_count = 0;
    struct run_arg c;
    memset(&c, 0, sizeof c);
    c.json = "{\"pause\":1}"; sha_of("pause-request", c.sha); c.deadline = 5000; c.use_pause = 1;
    pthread_t tc;
    pthread_create(&tc, NULL, run_thread, &c);
    pthread_join(tc, NULL);
    CHECK(c.rc == 0 && g_pause_count >= 1 && g_pause_count == g_resume_count,
          "pause and resume are paired around the wait");
    free(c.bytes);

    // 4) Stop 唤醒等待者；worker 可重启并继续工作。
    g_delay_ms = 300;
    struct run_arg d;
    memset(&d, 0, sizeof d);
    d.json = "{\"slow\":1}"; sha_of("slow-request", d.sha); d.deadline = 5000; d.use_pause = 1;
    g_pause_count = g_resume_count = 0;
    pthread_t td;
    pthread_create(&td, NULL, run_thread, &d);
    usleep(50 * 1000);
    s2a_compile_worker_stop();
    pthread_join(td, NULL);
    CHECK(d.rc != 0 && strstr(d.why, "stop"), "stop fails the waiting request accurately");
    CHECK(g_pause_count == 1 && g_resume_count == 1, "slow return pairs successful pause exactly once");
    CHECK(s2a_compile_worker_active() == 0, "worker is inactive after stop");
    CHECK(s2a_compile_worker_start(compile_fn, NULL, why, sizeof why) == 0, "worker restarts");
    g_delay_ms = 5;
    struct run_arg e;
    memset(&e, 0, sizeof e);
    e.json = "{\"after\":1}"; sha_of("after-request", e.sha); e.deadline = 5000;
    pthread_t te;
    pthread_create(&te, NULL, run_thread, &e);
    pthread_join(te, NULL);
    CHECK(e.rc == 0, "worker works after a restart");
    free(e.bytes);

    // 5) 超时放弃：返回错误；迟到结果被丢弃（下一次同 sha 会重新编译而不是取旧结果）。
    g_delay_ms = 300;
    struct run_arg f;
    memset(&f, 0, sizeof f);
    f.json = "{\"timeout\":1}"; sha_of("timeout-request", f.sha); f.deadline = 50; f.use_pause = 1;
    g_pause_count = g_resume_count = 0;
    pthread_t tf;
    pthread_create(&tf, NULL, run_thread, &f);
    pthread_join(tf, NULL);
    CHECK(f.rc != 0 && strstr(f.why, "timeout"), "deadline fails the wait");
    CHECK(g_pause_count == 1 && g_resume_count == 1, "timeout return pairs successful pause exactly once");
    usleep(400 * 1000);   // 让迟到结果在 worker 内完成并被丢弃
    g_delay_ms = 5;
    g_calls = 0;
    struct run_arg g2;
    memset(&g2, 0, sizeof g2);
    g2.json = "{\"timeout\":1}"; sha_of("timeout-request", g2.sha); g2.deadline = 5000;
    pthread_t tg;
    pthread_create(&tg, NULL, run_thread, &g2);
    pthread_join(tg, NULL);
    CHECK(g2.rc == 0 && g_calls == 1, "an abandoned result is not served to a later request");
    free(g2.bytes);

    // 6) 编译失败原样带回。
    g_delay_ms = 5;
    g_fail_next = 1;
    struct run_arg h;
    memset(&h, 0, sizeof h);
    h.json = "{\"fail\":1}"; sha_of("fail-request", h.sha); h.deadline = 5000; h.use_pause = 1;
    g_pause_count = g_resume_count = 0;
    pthread_t th2;
    pthread_create(&th2, NULL, run_thread, &h);
    pthread_join(th2, NULL);
    CHECK(h.rc != 0 && strstr(h.why, "compile failed"), "compile failure is reported");
    CHECK(g_pause_count == 1 && g_resume_count == 1, "fail return pairs successful pause exactly once");
    free(h.bytes);

    // 7) 有界 Stop：编译仍在进行时不能无界 join；未退出前拒绝重启，退出后回收。
    {
        g_delay_ms = 400;
        struct run_arg i;
        memset(&i, 0, sizeof i);
        i.json = "{\"long\":1}"; sha_of("long-request", i.sha); i.deadline = 5000;
        pthread_t ti;
        pthread_create(&ti, NULL, run_thread, &i);
        usleep(30 * 1000);
        char bwhy[192] = "";
        int brc = s2a_compile_worker_stop_bounded(30, bwhy, sizeof bwhy);
        CHECK(brc == 1, "bounded stop reports a still-running compile instead of joining forever");
        CHECK(strstr(bwhy, "still compiling") != NULL, "bounded stop explains why it did not join");
        CHECK(s2a_compile_worker_active() == 0, "worker no longer accepts demand after the stop request");
        CHECK(s2a_compile_worker_start(compile_fn, NULL, why, sizeof why) != 0,
              "restart is refused until the previous compile has actually exited");
        CHECK(s2a_compile_worker_pump(10) == -1, "pump reports no running worker");
        pthread_join(ti, NULL);
        CHECK(i.rc != 0 && strstr(i.why, "stop"), "the waiting request fails accurately");
        free(i.bytes);
        CHECK(s2a_compile_worker_stop_bounded(2000, bwhy, sizeof bwhy) == 0,
              "a later bounded stop reclaims the finished worker");
        CHECK(s2a_compile_worker_start(compile_fn, NULL, why, sizeof why) == 0,
              "worker restarts after the bounded stop completed");
        CHECK(s2a_compile_worker_pump(10) == 0, "pump reports an idle worker");
        // 在途时 pump 只等一个控制片就返回“仍有需求”，不阻塞 owner。
        g_delay_ms = 200;
        struct run_arg j;
        memset(&j, 0, sizeof j);
        j.json = "{\"pump\":1}"; sha_of("pump-request", j.sha); j.deadline = 5000;
        pthread_t tj;
        pthread_create(&tj, NULL, run_thread, &j);
        usleep(30 * 1000);
        CHECK(s2a_compile_worker_pump(10) == 1, "pump reports an in-flight demand after one 10 ms slice");
        pthread_join(tj, NULL);
        CHECK(j.rc == 0, "the pumped request still completes");
        free(j.bytes);
        g_delay_ms = 5;
    }

    // 8) 异步提交：完成环取结果、可重复 poll、同 sha 去重、编译失败如实返回。
    {
        g_delay_ms = 60;
        g_calls = 0;
        uint8_t sha_a[32];
        sha_of("async-a", sha_a);
        uint8_t *ab = NULL; size_t alen = 0;
        CHECK(s2a_compile_worker_submit_async(9, 1, "{\"a\":1}", 7, sha_a, why, sizeof why) == 0,
              "async submit succeeds");
        CHECK(s2a_compile_worker_poll(sha_a, &ab, &alen, why, sizeof why) == 1,
              "poll reports in-flight while the compile is still running");
        CHECK(s2a_compile_worker_submit_async(9, 1, "{\"a\":1}", 7, sha_a, why, sizeof why) == 0,
              "same-sha async submit dedupes instead of compiling twice");
        CHECK(s2a_compile_worker_wait_async(sha_a, 3000, &ab, &alen, why, sizeof why) == 0,
              "wait_async returns the finished result");
        CHECK(ab && alen == 7 && !memcmp(ab, "{\"a\":1}", 7), "async result carries the compile bytes");
        free(ab); ab = NULL;
        CHECK(s2a_compile_worker_poll(sha_a, &ab, &alen, why, sizeof why) == 0 && ab,
              "poll does not consume the ring entry");
        free(ab); ab = NULL;
        CHECK(s2a_compile_worker_submit_async(9, 1, "{\"a\":1}", 7, sha_a, why, sizeof why) == 2,
              "resubmitting a sha with a ring result reports it without recompiling");
        CHECK(g_calls == 1, "three async touches of one sha compile exactly once");
    }

    // 9) 队列：槽占满后 FIFO 排队，短队满返回 1（调用方回落同步通道）。
    {
        g_delay_ms = 80;
        g_calls = 0;
        uint8_t sha[6][32];
        char name[32];
        int rc[6] = {0};
        for (int i = 0; i < 6; i++) {
            snprintf(name, sizeof name, "fifo-%d", i);
            sha_of(name, sha[i]);
            snprintf(name, sizeof name, "{\"q\":%d}", i);
            rc[i] = s2a_compile_worker_submit_async(10, 1, name, strlen(name), sha[i], why, sizeof why);
        }
        CHECK(rc[0] == 0, "first async takes the slot");
        for (int i = 1; i <= 4; i++) CHECK(rc[i] == 0, "next four async fill the queue");
        CHECK(rc[5] == 1, "queue full reports backpressure for the sync fallback");
        uint8_t *qb = NULL; size_t qlen = 0;
        for (int i = 0; i < 5; i++) {
            int wrc = s2a_compile_worker_wait_async(sha[i], 5000, &qb, &qlen, why, sizeof why);
            CHECK(wrc == 0, "queued async completes in FIFO order");
            free(qb); qb = NULL;
        }
        CHECK(g_calls == 5, "every distinct queued sha compiles once");
        struct s2a_compile_worker_stats qs;
        s2a_compile_worker_stats(&qs);
        CHECK(qs.queue_full >= 1 && qs.queued >= 4 && qs.done_pushed >= 5,
              "stats count queue fill and ring pushes");
    }

    // 10) 异步失败与挤环：失败带原因返回 2；环满挤最老后查询 fail-closed 返回 3。
    {
        g_delay_ms = 5;
        uint8_t sha_f[32];
        sha_of("async-fail", sha_f);
        g_fail_next = 1;
        uint8_t *fb = NULL; size_t flen = 0;
        CHECK(s2a_compile_worker_submit_async(11, 1, "{\"f\":1}", 7, sha_f, why, sizeof why) == 0,
              "async submit of a failing compile succeeds");
        CHECK(s2a_compile_worker_wait_async(sha_f, 3000, &fb, &flen, why, sizeof why) == 2,
              "async compile failure reports through the ring");
        CHECK(strstr(why, "compile failed") != NULL, "the failure carries the compiler reason");
        CHECK(!fb, "a failed ring entry returns no bytes");
        // 挤环：9 个完成把 sha_f 挤出容量 8 的环。串行提交，避开队列背压干扰断言。
        uint8_t sha_e[9][32];
        char name[32];
        uint8_t *eb = NULL; size_t elen = 0;
        for (int i = 0; i < 9; i++) {
            snprintf(name, sizeof name, "evict-%d", i);
            sha_of(name, sha_e[i]);
            snprintf(name, sizeof name, "{\"e\":%d}", i);
            CHECK(s2a_compile_worker_submit_async(11, 1, name, strlen(name), sha_e[i], why, sizeof why) == 0,
                  "eviction batch submits");
            CHECK(s2a_compile_worker_wait_async(sha_e[i], 5000, &eb, &elen, why, sizeof why) == 0,
                  "eviction batch completes");
            free(eb); eb = NULL;
        }
        CHECK(s2a_compile_worker_poll(sha_f, &fb, &flen, why, sizeof why) == 3,
              "an evicted ring entry fails closed as unknown");
        struct s2a_compile_worker_stats es;
        s2a_compile_worker_stats(&es);
        CHECK(es.done_evicted >= 1, "stats count ring evictions");
    }

    // 11) stop 清场：在槽/在队/已完成全部作废，之后查询一律 fail-closed。
    {
        g_delay_ms = 150;
        uint8_t sha_s[3][32];
        char name[32];
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof name, "stop-%d", i);
            sha_of(name, sha_s[i]);
            snprintf(name, sizeof name, "{\"s\":%d}", i);
            s2a_compile_worker_submit_async(12, 1, name, strlen(name), sha_s[i], why, sizeof why);
        }
        usleep(50 * 1000);
        s2a_compile_worker_stop();
        uint8_t *sb = NULL; size_t slen = 0;
        for (int i = 0; i < 3; i++)
            CHECK(s2a_compile_worker_poll(sha_s[i], &sb, &slen, why, sizeof why) == 3,
                  "stop clears in-flight, queued and finished evidence");
        CHECK(s2a_compile_worker_submit_async(12, 1, "{\"s\":9}", 7, sha_s[0], why, sizeof why) == -1,
              "submit against a stopped worker fails accurately");
        CHECK(s2a_compile_worker_start(compile_fn, NULL, why, sizeof why) == 0,
              "worker restarts after the clearing stop");
        g_delay_ms = 5;
    }

    s2a_compile_worker_stop();
    if (fails) { printf("direct compile worker: %d FAILURES\n", fails); return 1; }
    printf("direct compile worker: %d checks ok (dedupe, capacity 1, pause contract, stop, timeout, bounded stop, pump, restart, async queue/ring)\n", checks);
    return 0;
}
