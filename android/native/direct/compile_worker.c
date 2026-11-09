#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // clock_gettime / pthread 条件变量在严格 -std=c11 下也要可见
#endif
#include "compile_worker.h"
#include "compile_request.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static struct job g_job;
static struct s2a_compile_worker_stats g_stats;
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_started, g_stop;
static uint64_t g_start_epoch; // distinguishes stop/restart while a caller pauses outside g_wmu
// 线程是否存在/是否已经走完（不能 join 一个还在编译的线程来收尾）。worker 在返回前
// 持锁置 done；stop 用条件变量按预算等它，而不是无界 join。
static int g_thread_running, g_thread_done;
static s2a_compile_worker_fn g_fn;
static void *g_ctx;

/* C4a 非阻塞提交侧：槽位被别的身份占用时落进短队列（FIFO 提升进槽）；完成环按 sha 持有
   结果副本，poll/wait_async 按 sha 取（不消费），满时挤掉最老的（fail-closed：查不到 = 丢失）。 */
struct qi {
    uint64_t session, generation;
    uint8_t sha[32];
    char *json;
    size_t len;
    struct s2a_compile_diag_attempt *diagnostic;   /* 与 run() 同一诊断生命周期 */
};
static struct qi g_queue[S2A_CW_QUEUE_MAX];
static unsigned g_q_len;                            /* 头=0，FIFO 提升 */

struct done_item {
    uint8_t sha[32];
    uint8_t *bytes;
    size_t bytes_len;
    int failed;
    char why[512];
};
static struct done_item g_done[S2A_CW_DONE_MAX];
static unsigned g_done_n;                           /* ring：挤最老 */

static void queue_clear_locked(void) {
    while (g_q_len) {
        struct qi *q = &g_queue[--g_q_len];
        diag_cancel(q->diagnostic, 4);
        /* 诊断释放不持锁：先记账，出锁由调用方统一 drop（与 worker 主路径同规矩）。 */
        q->diagnostic = NULL;
        free(q->json); q->json = NULL;
    }
}
static void done_clear_locked(void) {
    for (unsigned i = 0; i < g_done_n; i++) { free(g_done[i].bytes); g_done[i].bytes = NULL; }
    g_done_n = 0;
}
/* 槽位空出时提升队头进槽（worker 主循环靠 g_work_cv 接着编）。仅 state==0 且非 stop 时做。 */
static void queue_promote_locked(void) {
    if (g_stop || g_job.state != 0 || !g_q_len) return;
    struct qi *q = &g_queue[0];
    g_job.session = q->session;
    g_job.generation = q->generation;
    memcpy(g_job.sha, q->sha, sizeof g_job.sha);
    g_job.json = q->json; q->json = NULL;
    g_job.len = q->len;
    g_job.diagnostic = q->diagnostic; q->diagnostic = NULL;
    g_job.refs = 0;
    g_job.abandoned = 0;
    g_job.failed = 0;
    g_job.why[0] = 0;
    g_job.async = 1;
    g_job.state = 1;
    memmove(&g_queue[0], &g_queue[1], (g_q_len - 1) * sizeof g_queue[0]);
    g_q_len--;
    pthread_cond_signal(&g_work_cv);
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
    d->failed = failed;
    snprintf(d->why, sizeof d->why, "%s", why ? why : "");
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
            if (*bytes) { memcpy(*bytes, d->bytes, d->bytes_len); if (bytes_len) *bytes_len = d->bytes_len; }
        }
        return 0;
    }
    return -1;
}
/* 在途证据：槽位在编译同 sha（1）或队列里有同 sha（2）；0 = 不在途。 */
static int inflight_locked(const uint8_t sha[32]) {
    if (g_job.state == 1 && !memcmp(g_job.sha, sha, 32)) return 1;
    for (unsigned i = 0; i < g_q_len; i++)
        if (!memcmp(g_queue[i].sha, sha, 32)) return 2;
    return 0;
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
    (void)arg;
    pthread_mutex_lock(&g_wmu);
    for (;;) {
        while (!g_stop && g_job.state != 1) pthread_cond_wait(&g_work_cv, &g_wmu);
        if (g_stop) break;
        struct job job;
        memset(&job, 0, sizeof job);
        job.session = g_job.session;
        job.generation = g_job.generation;
        memcpy(job.sha, g_job.sha, sizeof job.sha);
        job.json = g_job.json;
        job.len = g_job.len;
        job.diagnostic = g_job.diagnostic; /* borrowed from the still-live job */
        g_job.json = NULL;   // 运行期间由工作副本持有
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
        if (!g_stop && !g_job.abandoned) {
            /* Publish only an accepted result, outside the worker lock. Stop,
             * timeout or dedup may atomically revoke its capsule meanwhile. */
            pthread_mutex_unlock(&g_wmu);
            if (job.diagnostic) s2a_compile_diag_finish(job.diagnostic, rc != 0);
            pthread_mutex_lock(&g_wmu);
        }
        if (g_stop || g_job.abandoned) {
            g_stats.late_discarded++;
            if (g_job.async && !g_stop) {
                /* 等待者全弃但 worker 还活着：完成环如实记一条终局失败，poll 侧拿到答案
                   而不是干等；stop 清场后环已被清空——迟到结果不再留痕（在途按丢失论）。 */
                done_push_locked(g_job.sha, NULL, 0, 1, "compile worker: late result discarded");
                g_stats.done_pushed++;
            }
            diag_cancel(g_job.diagnostic, 4);
            free(bytes);
            struct s2a_compile_diag_attempt *retired = job_free(&g_job);
            queue_promote_locked();
            pthread_cond_broadcast(&g_done_cv);
            pthread_mutex_unlock(&g_wmu);
            diag_drop(retired);
            pthread_mutex_lock(&g_wmu);
            if (g_stop) break;
            continue;
        }
        g_job.bytes = bytes;
        g_job.bytes_len = bytes_len;
        g_job.failed = rc != 0;
        snprintf(g_job.why, sizeof g_job.why, "%s", why);
        if (g_job.async) {
            /* 先投进完成环（poll 通道），再按等待者语义处理槽位：
               refs>0 = 有 run() 附着者等 state==2；refs==0 = 纯在途，释放槽位提升队头。 */
            done_push_locked(g_job.sha, g_job.bytes, g_job.bytes_len, g_job.failed, g_job.why);
            g_stats.done_pushed++;
            if (g_job.refs <= 0) {
                struct s2a_compile_diag_attempt *retired = job_free(&g_job);
                queue_promote_locked();
                pthread_cond_broadcast(&g_done_cv);
                pthread_mutex_unlock(&g_wmu);
                diag_drop(retired);
                pthread_mutex_lock(&g_wmu);
                continue;
            }
        }
        g_job.state = 2;
        pthread_cond_broadcast(&g_done_cv);
    }
    g_thread_done = 1;
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_wmu);
    return NULL;
}

int s2a_compile_worker_start(s2a_compile_worker_fn fn, void *ctx, char *why, size_t why_size) {
    if (!fn) { if (why && why_size) snprintf(why, why_size, "compile worker: no compile function"); return -1; }
    struct s2a_compile_diag_attempt *retired = NULL;
    pthread_mutex_lock(&g_wmu);
    if (g_started) { pthread_mutex_unlock(&g_wmu); return 0; }
    if (g_thread_running) {
        // 上一次有界 Stop 没等到退出：线程可能还在编译。只有确认它已经走完才能回收并重启，
        // 不能把 g_stop 清零后让旧线程看到一份新任务。
        if (!g_thread_done) {
            pthread_mutex_unlock(&g_wmu);
            if (why && why_size) snprintf(why, why_size, "compile worker: previous compile still running");
            return -1;
        }
        pthread_join(g_thread, NULL);   // 线程已置 done 且不再需要锁，join 不会阻塞
        g_thread_running = 0;
        g_thread_done = 0;
        retired = job_free(&g_job);
    }
    g_fn = fn;
    g_ctx = ctx;
    g_stop = 0;
    if (!retired) retired = job_free(&g_job);
    else (void)job_free(&g_job); /* already empty after the completed thread */
    // 统一编译器对复杂 shader 是递归的：默认 1 MiB 线程栈会把 worker 打爆（真机实测一次
    // 178 KB 请求后进程直接死亡）。给足够大的栈；虚拟内存按需提交。
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    int cr = pthread_create(&g_thread, &attr, worker_main, NULL);
    pthread_attr_destroy(&attr);
    if (cr != 0 && pthread_create(&g_thread, NULL, worker_main, NULL) != 0) {
        if (why && why_size) snprintf(why, why_size, "compile worker: thread creation failed");
        pthread_mutex_unlock(&g_wmu);
        diag_drop(retired);
        return -1;
    }
    g_start_epoch++;
    g_started = 1;
    g_thread_running = 1;
    g_thread_done = 0;
    pthread_mutex_unlock(&g_wmu);
    diag_drop(retired);
    return 0;
}

int s2a_compile_worker_active(void) {
    pthread_mutex_lock(&g_wmu);
    int active = g_started && !g_stop;
    pthread_mutex_unlock(&g_wmu);
    return active;
}

// 收尾状态机：请求停止 → 按预算等 worker 走完 → 只有确认走完才 join/清编译入口。
// 返回 0 = 已退出并回收；1 = 仍在编译（迟到结果会被丢弃，下一次 start/stop 先尝试回收）；
// -1 = 参数问题。预算 0 表示不等待（只发停止请求）。
static int worker_stop_locked(int budget_ms, char *why, size_t why_size,
                              struct s2a_compile_diag_attempt **retired) {
    if (!g_started && !g_thread_running) return 0;
    g_stop = 1;
    queue_clear_locked();   // C4a：stop 清空短队列与完成环——在途按「丢失」由 poll 如实返回
    done_clear_locked();
    diag_cancel(g_job.diagnostic, 4);
    g_started = 0;   // 不再接收新任务；run 会准确报 "not running"
    pthread_cond_signal(&g_work_cv);
    pthread_cond_broadcast(&g_done_cv);
    if (budget_ms > 0 && g_thread_running && !g_thread_done) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += budget_ms / 1000;
        dl.tv_nsec += (long)(budget_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (g_thread_running && !g_thread_done) {
            if (pthread_cond_timedwait(&g_done_cv, &g_wmu, &dl) == ETIMEDOUT) break;
        }
    }
    if (g_thread_running && !g_thread_done) {
        // 仍在编译：保留 g_fn/g_ctx（正在跑的调用还要用），线程退出时会丢弃迟到结果。
        if (why && why_size)
            snprintf(why, why_size, "compile worker: still compiling after %d ms", budget_ms);
        return 1;
    }
    if (g_thread_running) {
        pthread_join(g_thread, NULL);   // 已置 done：不会阻塞
        g_thread_running = 0;
        g_thread_done = 0;
    }
    *retired = job_free(&g_job);
    g_fn = NULL;
    g_ctx = NULL;
    return 0;
}

int s2a_compile_worker_stop_bounded(uint32_t budget_ms, char *why, size_t why_size) {
    struct s2a_compile_diag_attempt *retired = NULL;
    pthread_mutex_lock(&g_wmu);
    int rc = worker_stop_locked(budget_ms > 0x7fffffffu ? 0x7fffffff : (int)budget_ms, why, why_size, &retired);
    pthread_mutex_unlock(&g_wmu);
    diag_drop(retired);
    return rc;
}

void s2a_compile_worker_stop(void) {
    struct s2a_compile_diag_attempt *retired = NULL;
    pthread_mutex_lock(&g_wmu);
    (void)worker_stop_locked(0x7fffffff, NULL, 0, &retired);   // 兼容旧调用：等到真正退出
    pthread_mutex_unlock(&g_wmu);
    diag_drop(retired);
}

int s2a_compile_worker_pump(uint32_t budget_ms) {
    if (budget_ms > 10) budget_ms = 10;   // 契约：owner 主动等待最多一个 10 ms 控制片
    pthread_mutex_lock(&g_wmu);
    if (!g_started) { pthread_mutex_unlock(&g_wmu); return -1; }
    if (g_job.state) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)budget_ms * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (g_job.state && !g_stop) {
            if (pthread_cond_timedwait(&g_done_cv, &g_wmu, &dl) == ETIMEDOUT) break;
        }
    }
    int pending = g_job.state ? 1 : 0;
    pthread_mutex_unlock(&g_wmu);
    return pending;
}

static void deadline_at(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    if (ms <= 0) ms = 10000;
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
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
    deadline_at(&deadline, deadline_ms);
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
    int mine = 0;   // 1 = 本调用启动了这个 job；2 = 附着到同 sha 的在途 job
    for (;;) {
        if (g_stop) break;
        if (g_job.state == 1 && !memcmp(g_job.sha, sha, 32)) {
            /* Compilation dedup is unchanged. Its diagnostic attribution is
             * deliberately refused, even when the attaching call has no token. */
            diag_cancel(g_job.diagnostic, 2); diag_cancel(diagnostic, 2);
            g_job.refs++; mine = 2; break;
        }
        queue_promote_locked();   // C4a：异步短队列优先于新同步发布（同一互斥下，不会插队）
        if (g_job.state == 0) {
            g_job.session = session;
            g_job.generation = generation;
            memcpy(g_job.sha, sha, sizeof g_job.sha);
            g_job.len = len;
            g_job.json = (char *)malloc(len);
            if (!g_job.json) { if (why && why_size) snprintf(why, why_size, "compile worker: out of memory"); goto out; }
            memcpy(g_job.json, json, len);
            g_job.diagnostic = diagnostic;
            if (diagnostic) s2a_compile_diag_retain(diagnostic);
            g_job.state = 1;
            g_job.refs = 1;
            g_job.abandoned = 0;
            g_job.failed = 0;
            g_job.why[0] = 0;
            g_job.async = 0;
            mine = 1;
            pthread_cond_signal(&g_work_cv);
            break;
        }
        // 槽被另一个请求占用（state 1 不同 sha / state 2 待取）：容量 1，等它让出。
        if (pthread_cond_timedwait(&g_done_cv, &g_wmu, &deadline) == ETIMEDOUT) {
            if (why && why_size) snprintf(why, why_size, "compile worker: slot busy timeout");
            g_stats.busy_timeouts++;
            goto out;
        }
    }
    g_stats.runs++;
    if (mine == 2) g_stats.dedup++;
    if (!mine) {
        if (why && why_size) snprintf(why, why_size, "compile worker: stopped");
        goto out;
    }
    // 等待本次/在途 job 完成；外部锁已经成功交出。
    while (g_job.state == 1 && !g_stop) {
        if (pthread_cond_timedwait(&g_done_cv, &g_wmu, &deadline) == ETIMEDOUT) break;
    }
    if (g_job.state == 2) {
        if (!g_job.failed && g_job.bytes_len) {
            *bytes = (uint8_t *)malloc(g_job.bytes_len);
            if (*bytes) { memcpy(*bytes, g_job.bytes, g_job.bytes_len); *bytes_len = g_job.bytes_len; rc = 0; }
            else if (why && why_size) snprintf(why, why_size, "compile worker: out of memory");
        } else {
            g_stats.failures++;
            if (why && why_size)
                snprintf(why, why_size, "%s", g_job.why[0] ? g_job.why : "compile worker: compile failed");
            return_failure = mine == 1 && g_job.failed && diagnostic == g_job.diagnostic;
        }
        g_job.refs--;
        if (g_job.refs <= 0) { retired = job_free(&g_job); queue_promote_locked(); pthread_cond_broadcast(&g_done_cv); }
    } else {
        // Stop 或超时：放弃本次等待，worker 完成时按 abandoned 丢弃结果。
        if (g_job.state == 1) {
            diag_cancel(g_job.diagnostic, g_stop ? 4 : 3);
            g_job.refs--;
            if (g_job.refs <= 0) g_job.abandoned = 1;
        }
        if (why && why_size)
            snprintf(why, why_size, g_stop ? "compile worker: stopped while waiting" : "compile worker: timeout");
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

int s2a_compile_worker_submit_async(uint64_t session, uint64_t generation,
                                    const char *json, size_t len, const uint8_t sha[32],
                                    char *why, size_t why_size) {
    /* 与 run() 同一诊断契约：提交这一刻就绑好本调用的尝试引用。 */
    struct s2a_compile_diag_attempt *diagnostic = diag_hooks() ? s2a_compile_diag_take(session, generation, json, len) : NULL;
    if (!json || !len || !sha) {
        if (why && why_size) snprintf(why, why_size, "compile worker: bad arguments");
        diag_cancel(diagnostic, 1); diag_drop(diagnostic);
        return -1;
    }
    pthread_mutex_lock(&g_wmu);
    if (!g_started || g_stop) {
        pthread_mutex_unlock(&g_wmu);
        if (why && why_size) snprintf(why, why_size, "compile worker: not running");
        diag_cancel(diagnostic, 4); diag_drop(diagnostic);
        return -1;
    }
    /* 完成环命中 = 结果已在册：不重复编译，也不重复提交。 */
    if (done_take_locked(sha, NULL, NULL, NULL, 0) >= 0) {
        pthread_mutex_unlock(&g_wmu);
        diag_cancel(diagnostic, 2); diag_drop(diagnostic);
        return 2;
    }
    /* 同 sha 在途（在槽或在队）：去重附着——不补等待者计数，结果仍会进完成环。 */
    if (inflight_locked(sha)) {
        if (g_job.state == 1 && !memcmp(g_job.sha, sha, 32)) g_job.async = 1;
        g_stats.dedup++;
        pthread_mutex_unlock(&g_wmu);
        diag_cancel(diagnostic, 2); diag_drop(diagnostic);
        return 0;
    }
    if (g_job.state == 0) {
        /* 槽位空：直接发布（队列已空——promote 在本函数之前必然已做过）。 */
        g_job.session = session;
        g_job.generation = generation;
        memcpy(g_job.sha, sha, sizeof g_job.sha);
        g_job.len = len;
        g_job.json = (char *)malloc(len);
        if (!g_job.json) {
            pthread_mutex_unlock(&g_wmu);
            if (why && why_size) snprintf(why, why_size, "compile worker: out of memory");
            diag_cancel(diagnostic, 1); diag_drop(diagnostic);
            return -1;
        }
        memcpy(g_job.json, json, len);
        g_job.diagnostic = diagnostic;
        if (diagnostic) s2a_compile_diag_retain(diagnostic);
        g_job.state = 1;
        g_job.refs = 0;
        g_job.abandoned = 0;
        g_job.failed = 0;
        g_job.why[0] = 0;
        g_job.async = 1;
        pthread_cond_signal(&g_work_cv);
        pthread_mutex_unlock(&g_wmu);
        return 0;
    }
    if (g_q_len >= S2A_CW_QUEUE_MAX) {
        g_stats.queue_full++;
        pthread_mutex_unlock(&g_wmu);
        if (why && why_size) snprintf(why, why_size, "compile worker: async queue full (%u)", S2A_CW_QUEUE_MAX);
        diag_cancel(diagnostic, 3); diag_drop(diagnostic);
        return 1;
    }
    struct qi *q = &g_queue[g_q_len++];
    memset(q, 0, sizeof *q);
    q->session = session;
    q->generation = generation;
    memcpy(q->sha, sha, 32);
    q->json = (char *)malloc(len);
    if (!q->json) {
        g_q_len--;
        pthread_mutex_unlock(&g_wmu);
        if (why && why_size) snprintf(why, why_size, "compile worker: out of memory");
        diag_cancel(diagnostic, 1); diag_drop(diagnostic);
        return -1;
    }
    memcpy(q->json, json, len);
    q->len = len;
    q->diagnostic = diagnostic;
    if (diagnostic) s2a_compile_diag_retain(diagnostic);
    g_stats.queued++;
    pthread_mutex_unlock(&g_wmu);
    return 0;
}

/* poll/wait_async 共用的状态查询（持 g_wmu 调用）：
   0 = 完成环成功命中；2 = 完成环失败命中；1 = 在途；3 = 不在册。 */
static int query_locked(const uint8_t sha[32], uint8_t **bytes, size_t *bytes_len,
                        char *why, size_t why_size) {
    int rc = done_take_locked(sha, bytes, bytes_len, why, why_size);
    if (rc >= 0) return rc;
    if (inflight_locked(sha)) return 1;
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
    int rc = query_locked(sha, bytes, bytes_len, why, why_size);
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
    deadline_at(&deadline, (int)(deadline_ms > 0x7fffffffu ? 0x7fffffff : deadline_ms));
    pthread_mutex_lock(&g_wmu);
    int rc;
    for (;;) {
        rc = query_locked(sha, bytes, bytes_len, why, why_size);
        if (rc != 1 || g_stop) break;
        if (pthread_cond_timedwait(&g_done_cv, &g_wmu, &deadline) == ETIMEDOUT) {
            /* 超时：结果可能恰在醒来前入环——以最后一次查询为准。 */
            rc = query_locked(sha, bytes, bytes_len, why, why_size);
            break;
        }
    }
    if (g_stop && rc == 1) rc = 3;   // stop 清场后在途证据即「丢失」
    pthread_mutex_unlock(&g_wmu);
    return rc;
}
