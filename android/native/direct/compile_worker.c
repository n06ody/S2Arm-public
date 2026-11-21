#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // clock_gettime / pthread 条件变量在严格 -std=c11 下也要可见
#endif
#include "compile_worker.h"
#include "compile_request.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <sched.h>
#endif

/* Optional request-owned diagnostic lifetime. The standalone worker has no
 * mandatory dependency on a compiler, parser, registry or diagnostic provider. */
extern struct s2a_compile_diag_attempt *s2a_compile_diag_take(uint64_t, uint64_t, const char *, size_t) __attribute__((weak));
extern void s2a_compile_diag_retain(struct s2a_compile_diag_attempt *) __attribute__((weak));
extern void s2a_compile_diag_enter(struct s2a_compile_diag_attempt *, uint64_t, uint64_t, const char *, size_t) __attribute__((weak));
extern void s2a_compile_diag_leave(void) __attribute__((weak));
extern void s2a_compile_diag_finish(struct s2a_compile_diag_attempt *, int) __attribute__((weak));
extern void s2a_compile_diag_abandon(struct s2a_compile_diag_attempt *, int) __attribute__((weak));
extern void s2a_compile_diag_returned(struct s2a_compile_diag_attempt *) __attribute__((weak));
extern void s2a_compile_diag_release(struct s2a_compile_diag_attempt *) __attribute__((weak));
// 弱链接[androiddraw]：s2a_compile_diag_take（W2 宿主单测 w2_compute_integration_test 不链 compile_request.c；macOS ld64 不接受未定义弱引用，按 tools/weak_link_decl.py 登记放行）
// 弱链接[androiddraw]：s2a_compile_diag_retain
// 弱链接[androiddraw]：s2a_compile_diag_enter
// 弱链接[androiddraw]：s2a_compile_diag_leave
// 弱链接[androiddraw]：s2a_compile_diag_finish
// 弱链接[androiddraw]：s2a_compile_diag_abandon
// 弱链接[androiddraw]：s2a_compile_diag_returned
// 弱链接[androiddraw]：s2a_compile_diag_release
static int diag_hooks(void) {
    return s2a_compile_diag_take && s2a_compile_diag_retain && s2a_compile_diag_enter &&
        s2a_compile_diag_leave && s2a_compile_diag_finish && s2a_compile_diag_abandon &&
        s2a_compile_diag_returned && s2a_compile_diag_release;
}
static void diag_cancel(struct s2a_compile_diag_attempt *a, int reason) {
    if (a) s2a_compile_diag_abandon(a, reason); /* atomic-only, including capsule */
}
static void diag_drop(struct s2a_compile_diag_attempt *a) {
    if (a) s2a_compile_diag_release(a); /* caller must not hold g_wmu */
}

struct job {
    int state;              // 0 空闲 / 1 运行中 / 2 已完成待取
    int refs;               // 已附着、会取走结果的等待者
    int abandoned;          // 最后一个等待者放弃（Stop/超时）
    int async;              // C4a：完成时把结果副本（失败含原因）也投进完成环（refs 不计在途提交）
    uint64_t session, generation;
    uint8_t sha[32];
    char *json;
    size_t len;
    uint8_t *bytes;
    size_t bytes_len;
    int failed;
    // 本支把 why 从 192 加到 512：编译失败时把 C# 侧 s2a-direct-error-v1 的 code/message 摘要也放进
    // result.why，192 字节常被截掉结论（见 compile_request_execute 的 reason 落地）。main 同时加了
    // W2 诊断尝试引用（每个 job 持一份），两者互不冲突，合并保留。
    char why[512];
    struct s2a_compile_diag_attempt *diagnostic; /* one job reference */
};

struct slot {
    struct job job;
    pthread_t thread;
};
static struct slot g_slots[S2A_CW_SLOTS_MAX];
static unsigned g_slots_n, g_slots_done;
static unsigned g_sync_waiters;
static struct s2a_compile_worker_stats g_stats;
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static int g_started, g_stop;
static uint64_t g_start_epoch; // distinguishes stop/restart while a caller pauses outside g_wmu
static s2a_compile_worker_fn g_fn;
static void *g_ctx;

/* C4a 非阻塞提交侧：槽位被别的身份占用时落进短队列（FIFO 提升进槽）；完成环按 sha 持有
   结果副本，poll/wait_async 按 sha 取（不消费），满时挤掉最老的（fail-closed：查不到 = 丢失）。 */
struct qi {
    int background;
    uint64_t session, generation;
    uint8_t sha[32];
    char *json;
    size_t len;
    struct s2a_compile_diag_attempt *diagnostic;   /* 与 run() 同一诊断生命周期 */
};
static struct qi g_queue[S2A_CW_QUEUE_MAX];
static unsigned g_q_len;   /* 同优先级按入队顺序 */

struct done_item {
    uint8_t sha[32];
    uint8_t *bytes;
    size_t bytes_len;
    int failed;
    char why[512];
};
static struct done_item g_done[S2A_CW_DONE_MAX];
static unsigned g_done_n;                           /* ring：挤最老 */

// Stop can retire both queued and assigned work. Release hooks can call
// back into the compiler, so ownership is transferred and released unlocked.
struct retired_diag {
    struct s2a_compile_diag_attempt *refs[S2A_CW_QUEUE_MAX + S2A_CW_SLOTS_MAX];
    unsigned count;
};
static void retire_diag(struct retired_diag *retired, struct s2a_compile_diag_attempt *ref) {
    if (ref) retired->refs[retired->count++] = ref;
}
static void drop_retired(const struct retired_diag *retired) {
    for (unsigned i = 0; i < retired->count; i++) diag_drop(retired->refs[i]);
}

static void queue_clear_locked(struct retired_diag *retired) {
    while (g_q_len) {
        struct qi *q = &g_queue[--g_q_len];
        diag_cancel(q->diagnostic, 4);
        retire_diag(retired, q->diagnostic);
        free(q->json);
        memset(q, 0, sizeof *q);
    }
}
static void done_clear_locked(void) {
    for (unsigned i = 0; i < g_done_n; i++) { free(g_done[i].bytes); g_done[i].bytes = NULL; }
    g_done_n = 0;
}
static struct job *idle_job_locked(int background) {
    // Lane 0 is always available to foreground work. On a single slot there
    // is no compile capacity for background work.
    for (unsigned i = background ? 1 : 0; i < g_slots_n; i++)
        if (!g_slots[i].job.state) return &g_slots[i].job;
    return NULL;
}

static void assign_locked(struct job *job, const struct qi *q, int refs) {
    job->session = q->session;
    job->generation = q->generation;
    memcpy(job->sha, q->sha, sizeof job->sha);
    job->json = q->json;
    job->len = q->len;
    job->diagnostic = q->diagnostic;
    job->refs = refs;
    job->async = refs == 0;
    job->state = 1;
    pthread_cond_broadcast(&g_work_cv);
}

static struct qi *queued_locked(const uint8_t sha[32]) {
    for (unsigned i = 0; i < g_q_len; i++)
        if (!memcmp(g_queue[i].sha, sha, 32)) return &g_queue[i];
    return NULL;
}

static void queue_promote_locked(void) {
    while (!g_stop && g_q_len) {
        unsigned i = 0;
        while (i < g_q_len && g_queue[i].background) i++;
        if (i == g_q_len) {
            if (g_sync_waiters) return;
            i = 0;
        }
        struct job *job = idle_job_locked(g_queue[i].background);
        if (!job) return;
        assign_locked(job, &g_queue[i], 0);
        g_q_len--;
        memmove(&g_queue[i], &g_queue[i + 1], (g_q_len - i) * sizeof g_queue[0]);
    }
}
/* 完成环入项：同 sha 覆盖；满则挤最老。bytes 是复制（worker 的结果仍归槽位原通道）。 */
static void done_push_locked(const uint8_t sha[32], const uint8_t *bytes, size_t len,
                             int failed, const char *why) {
    struct done_item *d = NULL;
    for (unsigned i = 0; i < g_done_n; i++)
        if (!memcmp(g_done[i].sha, sha, 32)) { d = &g_done[i]; break; }
    if (!d) {
        if (g_done_n == S2A_CW_DONE_MAX) {
            free(g_done[0].bytes);
            memmove(&g_done[0], &g_done[1], (g_done_n - 1) * sizeof g_done[0]);
            g_done_n--;
            g_stats.done_evicted++;
        }
        d = &g_done[g_done_n++];
        memset(d, 0, sizeof *d);
    }
    free(d->bytes); d->bytes = NULL;
    d->bytes_len = 0;
    if (bytes && len) {
        d->bytes = (uint8_t *)malloc(len);
        if (d->bytes) { memcpy(d->bytes, bytes, len); d->bytes_len = len; }
    }
    memcpy(d->sha, sha, 32);
    d->failed = failed || !d->bytes_len;
    snprintf(d->why, sizeof d->why, "%s",
             failed ? (why ? why : "") : !d->bytes_len ? "compile worker: missing result bytes" : "");
    pthread_cond_broadcast(&g_done_cv);
}
/* 环内按 sha 查：0 成功命中（拷出 bytes 副本）/ 2 失败命中（拷 why）/ -1 不在环。 */
static int done_take_locked(const uint8_t sha[32], uint8_t **bytes, size_t *bytes_len,
                            char *why, size_t why_size) {
    for (unsigned i = 0; i < g_done_n; i++) {
        struct done_item *d = &g_done[i];
        if (memcmp(d->sha, sha, 32)) continue;
        if (d->failed) {
            /* 与 run() 同一兜底：编译器没写原因时给出 worker 自己的失败定性。 */
            if (why && why_size)
                snprintf(why, why_size, "%s", d->why[0] ? d->why : "compile worker: compile failed");
            return 2;
        }
        if (bytes) {
            *bytes = (uint8_t *)malloc(d->bytes_len ? d->bytes_len : 1u);
            if (!*bytes) {
                if (why && why_size) snprintf(why, why_size, "compile worker: out of memory");
                return -2;
            }
            memcpy(*bytes, d->bytes, d->bytes_len);
            if (bytes_len) *bytes_len = d->bytes_len;
        }
        return 0;
    }
    return -1;
}
static struct job *job_for_locked(const uint8_t sha[32], int state) {
    for (unsigned i = 0; i < g_slots_n; i++) {
        struct job *job = &g_slots[i].job;
        if (job->state == state && !memcmp(job->sha, sha, 32)) return job;
    }
    return NULL;
}

void s2a_compile_worker_stats(struct s2a_compile_worker_stats *out) {
    if (!out) return;
    pthread_mutex_lock(&g_wmu);
    *out = g_stats;
    pthread_mutex_unlock(&g_wmu);
}

static struct s2a_compile_diag_attempt *job_free(struct job *j) {
    struct s2a_compile_diag_attempt *retired = j->diagnostic;
    free(j->json);
    free(j->bytes);
    memset(j, 0, sizeof *j);
    return retired; /* release after unlocking g_wmu */
}

static void *worker_main(void *arg) {
    struct job *current = &((struct slot *)arg)->job;
    pthread_mutex_lock(&g_wmu);
    for (;;) {
        while (!g_stop && current->state != 1) pthread_cond_wait(&g_work_cv, &g_wmu);
        if (g_stop) break;
        struct job job;
        memset(&job, 0, sizeof job);
        job.session = current->session;
        job.generation = current->generation;
        memcpy(job.sha, current->sha, sizeof job.sha);
        job.json = current->json;
        job.len = current->len;
        job.diagnostic = current->diagnostic; /* borrowed from the still-live job */
        current->json = NULL;   // 运行期间由工作副本持有
        pthread_mutex_unlock(&g_wmu);

        uint8_t *bytes = NULL;
        size_t bytes_len = 0;
        char why[512] = "";
        pthread_mutex_lock(&g_wmu);
        g_stats.compiles++;
        pthread_mutex_unlock(&g_wmu);
        if (diag_hooks()) s2a_compile_diag_enter(job.diagnostic, job.session, job.generation, job.json, job.len);
        int rc = g_fn(g_ctx, job.session, job.generation, job.json, job.len,
                      &bytes, &bytes_len, why, sizeof why);
        if (diag_hooks()) s2a_compile_diag_leave();
        free(job.json);

        pthread_mutex_lock(&g_wmu);
        if (!g_stop && !current->abandoned) {
            /* Publish only an accepted result, outside the worker lock. Stop,
             * timeout or dedup may atomically revoke its capsule meanwhile. */
            pthread_mutex_unlock(&g_wmu);
            if (job.diagnostic) s2a_compile_diag_finish(job.diagnostic, rc != 0);
            pthread_mutex_lock(&g_wmu);
        }
        if (g_stop || current->abandoned) {
            g_stats.late_discarded++;
            if (current->async && !g_stop) {
                /* 等待者全弃但 worker 还活着：完成环如实记一条终局失败，poll 侧拿到答案
                   而不是干等；stop 清场后环已被清空——迟到结果不再留痕（在途按丢失论）。 */
                done_push_locked(current->sha, NULL, 0, 1, "compile worker: late result discarded");
                g_stats.done_pushed++;
            }
            diag_cancel(current->diagnostic, 4);
            free(bytes);
            struct s2a_compile_diag_attempt *retired = job_free(current);
            queue_promote_locked();
            pthread_cond_broadcast(&g_done_cv);
            pthread_mutex_unlock(&g_wmu);
            diag_drop(retired);
            pthread_mutex_lock(&g_wmu);
            if (g_stop) break;
            continue;
        }
        current->bytes = bytes;
        current->bytes_len = bytes_len;
        current->failed = rc != 0;
        snprintf(current->why, sizeof current->why, "%s", why);
        if (current->async) {
            /* 先投进完成环（poll 通道），再按等待者语义处理槽位：
               refs>0 = 有 run() 附着者等 state==2；refs==0 = 纯在途，释放槽位提升队头。 */
            done_push_locked(current->sha, current->bytes, current->bytes_len, current->failed, current->why);
            g_stats.done_pushed++;
            if (current->refs <= 0) {
                struct s2a_compile_diag_attempt *retired = job_free(current);
                queue_promote_locked();
                pthread_cond_broadcast(&g_done_cv);
                pthread_mutex_unlock(&g_wmu);
                diag_drop(retired);
                pthread_mutex_lock(&g_wmu);
                continue;
            }
        }
        current->state = 2;
        pthread_cond_broadcast(&g_done_cv);
    }
    g_slots_done++;
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_wmu);
    return NULL;
}

// Workers mark themselves done under g_wmu as their last shared operation.
static void reap_slots_locked(struct retired_diag *retired) {
    for (unsigned i = 0; i < g_slots_n; i++) {
        pthread_join(g_slots[i].thread, NULL);
        retire_diag(retired, job_free(&g_slots[i].job));
    }
    g_slots_n = g_slots_done = 0;
}

unsigned s2a_compile_worker_capacity_slots(const unsigned *caps, size_t count) {
    // Capacity classes come from permitted CPUs. With a prime tier and at
    // least two other classes, the top two classes count as big cores.
    if (!caps) return 1;
    unsigned smallest = UINT32_MAX, greatest = 0, second = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned cap = caps[i];
        if (!cap) return 1;
        if (cap < smallest) smallest = cap;
        if (cap > greatest) { second = greatest; greatest = cap; }
        else if (cap < greatest && cap > second) second = cap;
    }
    if (greatest <= smallest) return 1;
    unsigned threshold = second > smallest ? second : greatest;
    unsigned big = 0;
    for (size_t i = 0; i < count; i++) big += caps[i] >= threshold;
    if (big <= 1) return 1;
    return big > S2A_CW_SLOTS_MAX ? S2A_CW_SLOTS_MAX : big - 1;
}

unsigned s2a_compile_worker_recommended_slots(void) {
#ifdef __ANDROID__
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof allowed, &allowed)) return 1;
    unsigned caps[CPU_SETSIZE];
    size_t count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        FILE *file = fopen(path, "r");
        if (!file) return 1;
        unsigned cap = 0;
        int n = fscanf(file, "%u", &cap);
        fclose(file);
        if (n != 1 || !cap) return 1;
        caps[count++] = cap;
    }
    return s2a_compile_worker_capacity_slots(caps, count);
#else
    return 1;
#endif
}

int s2a_compile_worker_start_pool(s2a_compile_worker_fn fn, void *ctx, unsigned slots,
                                  char *why, size_t why_size) {
    if (!fn || !slots || slots > S2A_CW_SLOTS_MAX) {
        if (why && why_size) snprintf(why, why_size, "compile worker: invalid function or slot count");
        return -1;
    }
    struct retired_diag retired = {0};
    pthread_mutex_lock(&g_wmu);
    if (g_started) { pthread_mutex_unlock(&g_wmu); return 0; }
    if (g_slots_n != g_slots_done) {
        pthread_mutex_unlock(&g_wmu);
        if (why && why_size) snprintf(why, why_size, "compile worker: previous compile still running");
        return -1;
    }
    reap_slots_locked(&retired);
    g_fn = fn;
    g_ctx = ctx;
    g_stop = 0;
    // The translator is recursive. Keep the verified worker stack size on
    // every lane; a default 1 MiB Android stack can terminate the process.
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (!rc) {
        rc = pthread_attr_setstacksize(&attr, 64u << 20);
        while (!rc && g_slots_n < slots) {
            struct slot *slot = &g_slots[g_slots_n];
            rc = pthread_create(&slot->thread, &attr, worker_main, slot);
            if (!rc) g_slots_n++;
        }
        pthread_attr_destroy(&attr);
    }
    if (g_slots_n) {
        g_start_epoch++;
        g_sync_waiters = 0;
        g_started = 1;
    }
    g_stats.slots = g_slots_n;
    const int started = g_started;
    if (rc && why && why_size)
        snprintf(why, why_size, "compile worker: started %u of %u slots (pthread error %d)", g_slots_n, slots, rc);
    pthread_mutex_unlock(&g_wmu);
    drop_retired(&retired);
    return started ? 0 : -1;
}

int s2a_compile_worker_start(s2a_compile_worker_fn fn, void *ctx, char *why, size_t why_size) {
    return s2a_compile_worker_start_pool(fn, ctx, 1, why, why_size);
}

int s2a_compile_worker_active(void) {
    pthread_mutex_lock(&g_wmu);
    int active = g_started && !g_stop;
    pthread_mutex_unlock(&g_wmu);
    return active;
}

// Android minSdk 29 提供 monotonic_np；旧层和新层的超时都不受系统时间校准影响。
static void deadline_at(struct timespec *ts, int ms) {
#ifdef __ANDROID__
    clock_gettime(CLOCK_MONOTONIC, ts);
#else
    clock_gettime(CLOCK_REALTIME, ts);
#endif
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static int wait_until(const struct timespec *deadline) {
#ifdef __ANDROID__
    return pthread_cond_timedwait_monotonic_np(&g_done_cv, &g_wmu, deadline);
#else
    return pthread_cond_timedwait(&g_done_cv, &g_wmu, deadline);
#endif
}

// 收尾状态机：请求停止 → 按预算等 worker 走完 → 只有确认走完才 join/清编译入口。
// 返回 0 = 已退出并回收；1 = 仍在编译（迟到结果会被丢弃，下一次 start/stop 先尝试回收）；
// -1 = 参数问题。预算 0 表示不等待（只发停止请求）。
static int worker_stop_locked(int budget_ms, char *why, size_t why_size, struct retired_diag *retired) {
    if (!g_slots_n) return 0;
    uint64_t epoch = g_start_epoch;
    g_stop = 1;
    g_started = 0;
    queue_clear_locked(retired);
    done_clear_locked();
    for (unsigned i = 0; i < g_slots_n; i++) diag_cancel(g_slots[i].job.diagnostic, 4);
    pthread_cond_broadcast(&g_work_cv);
    pthread_cond_broadcast(&g_done_cv);
    if (budget_ms > 0 && g_slots_n != g_slots_done) {
        struct timespec dl;
        deadline_at(&dl, budget_ms);
        while (g_start_epoch == epoch && g_slots_n != g_slots_done)
            if (wait_until(&dl)) break;
    }
    if (g_start_epoch != epoch) return 0;
    if (g_slots_n != g_slots_done) {
        if (why && why_size)
            snprintf(why, why_size, "compile worker: still compiling after %d ms", budget_ms);
        return 1;
    }
    reap_slots_locked(retired);
    g_fn = NULL;
    g_ctx = NULL;
    g_stats.slots = 0;
    return 0;
}

int s2a_compile_worker_stop_bounded(uint32_t budget_ms, char *why, size_t why_size) {
    struct retired_diag retired = {0};
    pthread_mutex_lock(&g_wmu);
    int rc = worker_stop_locked(budget_ms > 0x7fffffffu ? 0x7fffffff : (int)budget_ms, why, why_size, &retired);
    pthread_mutex_unlock(&g_wmu);
    drop_retired(&retired);
    return rc;
}

void s2a_compile_worker_stop(void) {
    (void)s2a_compile_worker_stop_bounded(0x7fffffffu, NULL, 0);
}

static int jobs_pending_locked(void) {
    for (unsigned i = 0; i < g_slots_n; i++)
        if (g_slots[i].job.state) return 1;
    return g_q_len != 0;
}

int s2a_compile_worker_pump(uint32_t budget_ms) {
    if (budget_ms > 10) budget_ms = 10;
    pthread_mutex_lock(&g_wmu);
    if (!g_started) { pthread_mutex_unlock(&g_wmu); return -1; }
    uint64_t epoch = g_start_epoch;
    struct timespec dl;
    deadline_at(&dl, (int)budget_ms);
    while (!g_stop && g_start_epoch == epoch && jobs_pending_locked())
        if (wait_until(&dl)) break;
    int pending = !g_started || g_start_epoch != epoch ? -1 : jobs_pending_locked();
    pthread_mutex_unlock(&g_wmu);
    return pending;
}

int s2a_compile_worker_run(uint64_t session, uint64_t generation, const char *json, size_t len,
                           const uint8_t sha[32], int deadline_ms,
                           int (*pause)(void *), void (*resume)(void *), void *pause_ctx,
                           uint8_t **bytes, size_t *bytes_len, char *why, size_t why_size) {
    /* Consume/clear this caller's exact build binding even on an early refusal.
     * Caller retains its own reference through resume and returned-receipt
     * handoff; a newly published job takes a separate reference. */
    struct s2a_compile_diag_attempt *diagnostic = diag_hooks() ? s2a_compile_diag_take(session, generation, json, len) : NULL;
    struct s2a_compile_diag_attempt *retired = NULL;
    int return_failure = 0;
    if (!json || !len || !sha || !bytes || !bytes_len) {
        if (why && why_size) snprintf(why, why_size, "compile worker: bad arguments");
        diag_cancel(diagnostic, 1); diag_drop(diagnostic);
        return -1;
    }
    *bytes = NULL;
    *bytes_len = 0;
    if (!!pause != !!resume) {
        if (why && why_size) snprintf(why, why_size, "compile worker: incomplete callback pair");
        diag_cancel(diagnostic, 5); diag_drop(diagnostic);
        return -1;
    }
    struct timespec deadline;
    deadline_at(&deadline, deadline_ms > 0 ? deadline_ms : 10000);
    int paused = 0, rc = -1;
    pthread_mutex_lock(&g_wmu);
    if (!g_started || g_stop) {
        pthread_mutex_unlock(&g_wmu);
        if (why && why_size) snprintf(why, why_size, "compile worker: not running");
        diag_cancel(diagnostic, 4); diag_drop(diagnostic);
        return -1;
    }
    // A failed pause must not publish work, attach a reference, or wait on a
    // busy slot. Callbacks run outside g_wmu and only a successful pause owns
    // one matching resume. The caller retains responsibility for its own lock
    // when pause fails; resume is not failure compensation.
    uint64_t epoch = g_start_epoch;
    if (pause) {
        pthread_mutex_unlock(&g_wmu);
        if (pause(pause_ctx) != 0) {
            if (why && why_size) snprintf(why, why_size, "compile worker: pause failed");
            diag_cancel(diagnostic, 5); diag_drop(diagnostic);
            return -1;
        }
        paused = 1;
        pthread_mutex_lock(&g_wmu);
        if (!g_started || g_stop || g_start_epoch != epoch) {
            if (why && why_size) snprintf(why, why_size, "compile worker: stopped or restarted during pause");
            goto out;
        }
    }
    int mine = 0;   // 1 = started here, 2 = attached to in-flight work
    int queued = 0;
    struct job *job = NULL;
    g_sync_waiters++;
    while (!g_stop && g_start_epoch == epoch) {
        struct qi *q = queued_locked(sha);
        if (q) { q->background = 0; queued = 1; }
        queue_promote_locked();   // Earlier foreground requests retain FIFO order.
        job = job_for_locked(sha, 1);
        if (!job) job = job_for_locked(sha, 2);
        if (job) {
            diag_cancel(job->diagnostic, 2);
            diag_cancel(diagnostic, 2);
            job->refs++;
            job->abandoned = 0;
            mine = 2;
            break;
        }
        if (queued) {
            int done = done_take_locked(sha, bytes, bytes_len, why, why_size);
            if (done != -1) {
                rc = done == 0 && *bytes_len ? 0 : -1;
                break;
            }
        }
        job = idle_job_locked(0);
        if (job) {
            struct qi input = {.session = session, .generation = generation, .len = len, .diagnostic = diagnostic};
            memcpy(input.sha, sha, sizeof input.sha);
            input.json = malloc(len);
            if (!input.json) {
                if (why && why_size) snprintf(why, why_size, "compile worker: out of memory");
                break;
            }
            memcpy(input.json, json, len);
            if (diagnostic) s2a_compile_diag_retain(diagnostic);
            assign_locked(job, &input, 1);
            mine = 1;
            break;
        }
        if (wait_until(&deadline)) {
            if (why && why_size) snprintf(why, why_size, "compile worker: slot busy timeout");
            g_stats.busy_timeouts++;
            break;
        }
    }
    if (g_start_epoch == epoch) {
        g_sync_waiters--;
        queue_promote_locked();
    }
    g_stats.runs++;
    if (!mine) {
        if ((g_stop || g_start_epoch != epoch) && why && why_size)
            snprintf(why, why_size, "compile worker: stopped");
        goto out;
    }
    if (mine == 2) g_stats.dedup++;
    // Recheck the epoch before touching this slot: stop or restart may have
    // retired it while the caller waited for the mutex.
    while (g_start_epoch == epoch && job->state == 1 && !g_stop)
        if (wait_until(&deadline)) break;
    if (g_start_epoch == epoch && job->state == 2 && !g_stop) {
        if (!job->failed && job->bytes_len) {
            *bytes = malloc(job->bytes_len);
            if (*bytes) { memcpy(*bytes, job->bytes, job->bytes_len); *bytes_len = job->bytes_len; rc = 0; }
            else if (why && why_size) snprintf(why, why_size, "compile worker: out of memory");
        } else {
            g_stats.failures++;
            if (why && why_size)
                snprintf(why, why_size, "%s", job->why[0] ? job->why : "compile worker: compile failed");
            return_failure = mine == 1 && job->failed && diagnostic == job->diagnostic;
        }
        job->refs--;
        if (job->refs <= 0) {
            retired = job_free(job);
            queue_promote_locked();
            pthread_cond_broadcast(&g_done_cv);
        }
    } else {
        if (g_start_epoch == epoch && job->state == 1) {
            diag_cancel(job->diagnostic, g_stop ? 4 : 3);
            job->refs--;
            if (job->refs <= 0 && !job->async) job->abandoned = 1;
        }
        if (why && why_size)
            snprintf(why, why_size, g_stop || g_start_epoch != epoch ?
                     "compile worker: stopped while waiting" : "compile worker: timeout");
    }
out:
    if (!return_failure) diag_cancel(diagnostic, g_stop ? 4 : 3);
    pthread_mutex_unlock(&g_wmu);
    if (paused) resume(pause_ctx);
    if (diag_hooks()) s2a_compile_diag_returned(return_failure ? diagnostic : NULL);
    diag_drop(diagnostic);
    diag_drop(retired);
    if (rc != 0) { free(*bytes); *bytes = NULL; *bytes_len = 0; }
    return rc;
}

static int submit_async(uint64_t session, uint64_t generation, const char *json, size_t len,
                        const uint8_t sha[32], int background, char *why, size_t why_size) {
    struct s2a_compile_diag_attempt *diagnostic = diag_hooks() ? s2a_compile_diag_take(session, generation, json, len) : NULL;
    if (!json || !len || !sha) {
        if (why && why_size) snprintf(why, why_size, "compile worker: bad arguments");
        diag_cancel(diagnostic, 1); diag_drop(diagnostic);
        return -1;
    }
    int rc = 0, cancel = 0;
    pthread_mutex_lock(&g_wmu);
    if (!g_started || g_stop) {
        if (why && why_size) snprintf(why, why_size, "compile worker: not running");
        rc = -1; cancel = 4;
    } else if (done_take_locked(sha, NULL, NULL, NULL, 0) >= 0) {
        rc = 2; cancel = 2;
    } else {
        struct job *running = job_for_locked(sha, 1);
        struct qi *queued = queued_locked(sha);
        const struct job *ready = job_for_locked(sha, 2);
        if (ready) {
            done_push_locked(sha, ready->bytes, ready->bytes_len, ready->failed, ready->why);
            g_stats.done_pushed++;
            rc = 2; cancel = 2;
        } else if (running || queued) {
            if (running) { running->async = 1; running->abandoned = 0; }
            if (queued && !background) queued->background = 0;
            g_stats.dedup++;
            cancel = 2;
        } else {
            // Start older eligible work first, except when a foreground
            // arrival can use a slot that would otherwise go to background.
            if (background) queue_promote_locked();
            struct job *idle = idle_job_locked(background);
            unsigned queue_limit = S2A_CW_QUEUE_MAX - (background ? 1u : 0u);
            const int direct = idle && !g_q_len && (!background || !g_sync_waiters);
            if ((background && g_slots_n == 1) || (!direct && g_q_len >= queue_limit)) {
                g_stats.queue_full++;
                if (why && why_size) snprintf(why, why_size, "compile worker: %s capacity exhausted",
                                              background ? "background" : "foreground");
                rc = 1; cancel = 3;
            } else {
                struct qi input = {.background = background, .session = session, .generation = generation,
                                   .len = len, .diagnostic = diagnostic};
                memcpy(input.sha, sha, sizeof input.sha);
                input.json = malloc(len);
                if (!input.json) {
                    if (why && why_size) snprintf(why, why_size, "compile worker: out of memory");
                    rc = -1; cancel = 1;
                } else {
                    memcpy(input.json, json, len);
                    if (diagnostic) s2a_compile_diag_retain(diagnostic);
                    if (direct)
                        assign_locked(idle, &input, 0);
                    else {
                        g_queue[g_q_len++] = input;
                        g_stats.queued++;
                    }
                }
            }
        }
        queue_promote_locked();
    }
    pthread_mutex_unlock(&g_wmu);
    if (cancel) diag_cancel(diagnostic, cancel);
    diag_drop(diagnostic);
    return rc;
}

int s2a_compile_worker_submit_async(uint64_t session, uint64_t generation,
                                    const char *json, size_t len, const uint8_t sha[32],
                                    char *why, size_t why_size) {
    return submit_async(session, generation, json, len, sha, 0, why, why_size);
}

int s2a_compile_worker_submit_background(uint64_t session, uint64_t generation,
                                         const char *json, size_t len, const uint8_t sha[32],
                                         char *why, size_t why_size) {
    return submit_async(session, generation, json, len, sha, 1, why, why_size);
}

void s2a_compile_worker_promote(const uint8_t sha[32]) {
    if (!sha) return;
    pthread_mutex_lock(&g_wmu);
    struct qi *job = queued_locked(sha);
    if (job) {
        job->background = 0;
        queue_promote_locked();
    }
    pthread_mutex_unlock(&g_wmu);
}

/* poll/wait_async 共用的状态查询（持 g_wmu 调用）：
   0 = 完成环成功命中；2 = 完成环失败命中；1 = 在途；3 = 不在册。 */
static int query_locked(const uint8_t sha[32], uint8_t **bytes, size_t *bytes_len,
                        char *why, size_t why_size) {
    int rc = done_take_locked(sha, bytes, bytes_len, why, why_size);
    if (rc != -1) return rc == -2 ? -1 : rc;
    if (job_for_locked(sha, 1) || queued_locked(sha)) return 1;
    /* 槽位 state==2 同 sha：等待者正在取——异步通道的结果副本已在环里（先投环再发布），
       done_take 上面已经命中；走到这里就是不在册。 */
    return 3;
}

int s2a_compile_worker_poll(const uint8_t sha[32], uint8_t **bytes, size_t *bytes_len,
                            char *why, size_t why_size) {
    if (!sha || !bytes || !bytes_len) {
        if (why && why_size) snprintf(why, why_size, "compile worker: bad arguments");
        return -1;
    }
    *bytes = NULL; *bytes_len = 0;
    pthread_mutex_lock(&g_wmu);
    int rc = g_started ? query_locked(sha, bytes, bytes_len, why, why_size) : 3;
    pthread_mutex_unlock(&g_wmu);
    return rc;
}

int s2a_compile_worker_wait_async(const uint8_t sha[32], uint32_t deadline_ms,
                                  uint8_t **bytes, size_t *bytes_len,
                                  char *why, size_t why_size) {
    if (!sha || !bytes || !bytes_len) {
        if (why && why_size) snprintf(why, why_size, "compile worker: bad arguments");
        return -1;
    }
    *bytes = NULL; *bytes_len = 0;
    struct timespec deadline;
    if (!deadline_ms) deadline_ms = 10000;
    deadline_at(&deadline, deadline_ms > 0x7fffffffu ? 0x7fffffff : (int)deadline_ms);
    pthread_mutex_lock(&g_wmu);
    uint64_t epoch = g_start_epoch;
    int rc = 3;
    while (!g_stop && g_start_epoch == epoch) {
        rc = query_locked(sha, bytes, bytes_len, why, why_size);
        if (rc != 1) break;
        if (wait_until(&deadline)) {
            rc = !g_stop && g_start_epoch == epoch ?
                 query_locked(sha, bytes, bytes_len, why, why_size) : 3;
            break;
        }
    }
    if (g_stop || g_start_epoch != epoch) rc = 3;
    pthread_mutex_unlock(&g_wmu);
    return rc;
}
