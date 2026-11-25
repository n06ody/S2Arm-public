// nvn_vk_pipe_demand 宿主集成测：fake 建管线驱动 demand 后台线程。
// 覆盖：kick→PENDING→后台建→READY；后台失败；不同着色器配方隔离；超时 stop 保留 worker 并阻止重启，回收后可安全重开。
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nvn_backend.h"
#include "nvn_plat.h"
#include "rp_shader.h"
#include "nvn_vk_pipe_demand.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static nvn_gpu_pipe g_installed;
static nvn_gpu_pipe_desc g_inst_desc;
static int g_has_inst;
static int g_fail_next;
static int g_build_calls;
static int g_gate_hold, g_gate_entered;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *d, nvn_gpu_pipe *out) {
    if (out) *out = NULL;
    pthread_mutex_lock(&g_mu);
    int hit = g_has_inst && d && memcmp(d, &g_inst_desc, sizeof *d) == 0;
    if (hit && out) *out = g_installed;
    pthread_mutex_unlock(&g_mu);
    return hit;
}
static _Thread_local int demand_worker;
void nvk_pipe_demand_worker_enter(void) { demand_worker = 1; }
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)mode;
    pthread_mutex_lock(&g_mu);
    g_build_calls++;
    while (g_gate_hold) {
        g_gate_entered = 1;
        pthread_cond_broadcast(&g_cv);
        pthread_cond_wait(&g_cv, &g_mu);
    }
    int fail = g_fail_next || !demand_worker;
    nvn_gpu_pipe built = (nvn_gpu_pipe)(uintptr_t)(0x9000 + g_build_calls);
    if (!fail) { g_inst_desc = *d; g_installed = built; g_has_inst = 1; }
    if (out) *out = fail ? NULL : built;
    pthread_mutex_unlock(&g_mu);
    nvn_gpu_rc r; r.code = fail ? NVN_GPU_E_COMPILE : NVN_GPU_OK;
    if (fail && err) snprintf(err->why, sizeof err->why, "fake compile fail");
    return r;
}
int nvn_plat_flag(const char *name, int dflt) { if (!strcmp(name, "nvk_pipe_async")) return 1; return dflt; }
static _Atomic int g_start_denial_logs;
static char g_last_start_denial[160];
void nvn_plat_log(int level, const char *cls, const char *text) {
    if (level != NVN_PLAT_LOG_WARN || !cls || strcmp(cls, "pipe-demand") != 0 || !text) return;
    snprintf(g_last_start_denial, sizeof g_last_start_denial, "%s", text);
    atomic_fetch_add_explicit(&g_start_denial_logs, 1, memory_order_release);
}
static struct rp_prog g_prog;
int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) { (void)identity; if (out) *out = &g_prog; return 1; }
void rp_shader_prog_return(const uint8_t identity[32]) { (void)identity; }

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); fails++; } } while (0)

static void set_ident(uint8_t *id, uint8_t v) { memset(id, v, 32); }
// 墙钟只用两种方式：下沿（定时器不会提前到）和防挂死的期限。期限只在事件一直不来时才触发，
// 负载再重也只会让等待变长、不会让它误红；「有没有排进第二次等待」「是不是走了超时返回」这类上沿一律按事件计数判。
enum { HANG_MS = 60000 };
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int wait_gate_entered(uint32_t ms) {
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (time_t)(ms / 1000); until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_mu);
    while (!g_gate_entered) if (pthread_cond_timedwait(&g_cv, &g_mu, &until) != 0) break;
    int entered = g_gate_entered;
    pthread_mutex_unlock(&g_mu);
    return entered;
}
static void gate_release(void) {
    pthread_mutex_lock(&g_mu); g_gate_hold = 0; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
static int wait_demand_idle(uint32_t ms) {
    uint64_t end = mono_ns() + (uint64_t)ms * 1000000ull;
    do {
        if (nvn_vk_pipe_demand_quiescent()) return 1;
        struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL);
    } while (mono_ns() < end);
    return nvn_vk_pipe_demand_quiescent();
}
static int wait_lifecycle_busy(uint32_t ms) {
    uint64_t end = mono_ns() + (uint64_t)ms * 1000000ull;
    do {
        if (nvn_vk_pipe_demand_test_lifecycle_busy()) return 1;
        struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL);
    } while (mono_ns() < end);
    return nvn_vk_pipe_demand_test_lifecycle_busy();
}
struct start_thread_arg { _Atomic int done; };
static void *start_thread_main(void *opaque) {
    struct start_thread_arg *arg = opaque;
    nvn_vk_pipe_demand_start();
    atomic_store_explicit(&arg->done, 1, memory_order_release);
    return NULL;
}
struct stop_thread_arg { _Atomic int done; uint64_t elapsed_ms; };
static void *stop_thread_main(void *opaque) {
    struct stop_thread_arg *arg = opaque;
    uint64_t start = mono_ns();
    nvn_vk_pipe_demand_stop();
    arg->elapsed_ms = (mono_ns() - start) / 1000000ull;
    atomic_store_explicit(&arg->done, 1, memory_order_release);
    return NULL;
}
enum demand_event { DEMAND_WORKER_START, DEMAND_WORKER_EXIT, DEMAND_BACKEND_RELEASE };
static int demand_events_safe(const enum demand_event *events, size_t count) {
    int active = 0;
    for (size_t i = 0; i < count; i++) {
        if (events[i] == DEMAND_WORKER_START) {
            if (active) return 0;
            active = 1;
        } else if (events[i] == DEMAND_WORKER_EXIT) {
            if (!active) return 0;
            active = 0;
        } else if (events[i] == DEMAND_BACKEND_RELEASE && active) {
            return 0;
        }
    }
    return active == 0;
}

static int wait_cold(const nvn_gpu_pipe_desc *desc, const uint8_t id[32], const uint8_t shader[32],
                     nvn_gpu_pipe *out) {
    for (uint64_t end = mono_ns() + 2000000000ull; mono_ns() < end;) {
        int state = nvn_vk_pipe_demand_cold_kick(desc, id, shader, out);
        if (state != NVK_DEMAND_PENDING) return state;
        const struct timespec delay = {0, 1000000L};
        nanosleep(&delay, NULL);
    }
    return NVK_DEMAND_PENDING;
}

static void check_cold_pair(void) {
    static const struct rp_cold_proof variant = {
        .fragment = (nvn_gpu_shader)(uintptr_t)0x62, .variant_sha = {0x71},
    };
    uint8_t id[32], stale[32];
    set_ident(id, 0x63);
    set_ident(stale, 0x72);
    g_prog.sh[0] = (nvn_gpu_shader)(uintptr_t)0x60;
    g_prog.sh[1] = (nvn_gpu_shader)(uintptr_t)0x61;
    g_prog.cold = &variant;
    nvn_gpu_pipe_desc a = {0};
    a.st[0].sh = g_prog.sh[0];
    a.st[1].sh = g_prog.sh[1];
    a.prim = 4;
    nvn_gpu_pipe_desc b = a;
    b.st[1].sh = variant.fragment;
    nvn_gpu_pipe orig = NULL, pruned = NULL, again = NULL;
    CHECK(nvn_vk_pipe_demand_kick(&a, id, &orig) == NVK_DEMAND_PENDING, "original queues");
    CHECK(nvn_vk_pipe_demand_wait(&a, id, 2000, &orig, NULL, 0) == 0 && orig, "original is ready");
    CHECK(wait_cold(&b, id, variant.variant_sha, &pruned) == NVK_DEMAND_READY && pruned && pruned != orig,
          "same program and PSO recipe preserve a disjoint cold handle");
    CHECK(nvn_vk_pipe_demand_kick(&a, id, &again) == NVK_DEMAND_READY && again == orig,
          "the pure A demand key still resolves after building B");
    CHECK(nvn_gpu_pipe_lookup(&b, &again) && again == pruned, "worker compiled the exact variant shader");
    b.prim = 3;
    CHECK(wait_cold(&b, id, stale, &again) == NVK_DEMAND_FAILED && !again,
          "a mismatched variant cannot produce a pipeline");
}

int main(void) {
    const enum demand_event safe_order[] = {DEMAND_WORKER_START, DEMAND_WORKER_EXIT, DEMAND_BACKEND_RELEASE};
    const enum demand_event release_while_active[] = {DEMAND_WORKER_START, DEMAND_BACKEND_RELEASE, DEMAND_WORKER_EXIT};
    const enum demand_event overlapping_workers[] = {DEMAND_WORKER_START, DEMAND_WORKER_START, DEMAND_WORKER_EXIT, DEMAND_WORKER_EXIT};
    CHECK(demand_events_safe(safe_order, sizeof safe_order / sizeof safe_order[0]), "synthetic normal lifecycle order accepted");
    CHECK(!demand_events_safe(release_while_active, sizeof release_while_active / sizeof release_while_active[0]), "synthetic negative control rejects backend release while worker active");
    CHECK(!demand_events_safe(overlapping_workers, sizeof overlapping_workers / sizeof overlapping_workers[0]), "synthetic negative control rejects overlapping workers");
    nvn_vk_pipe_demand_test_set_on(0);
    uint64_t denials_before = nvn_vk_pipe_demand_test_start_lock_denials();
    int logs_before = atomic_load_explicit(&g_start_denial_logs, memory_order_acquire);
    nvn_vk_pipe_demand_start();
    CHECK(nvn_vk_pipe_demand_test_start_lock_denials() == denials_before, "disabled demand start does not count a lock denial");
    CHECK(atomic_load_explicit(&g_start_denial_logs, memory_order_acquire) == logs_before, "disabled demand start emits no diagnostic");
    CHECK(nvn_vk_pipe_demand_worker_count() == 0, "disabled demand start creates no worker");
    nvn_vk_pipe_demand_test_set_on(1);
    int start_lifecycle_lock = nvn_vk_pipe_demand_test_lock_lifecycle();
    CHECK(start_lifecycle_lock == 0, "test holds g_stop_mu for denied-start case");
    if (start_lifecycle_lock == 0) {
        struct start_thread_arg start_arg = {0};
        pthread_t start_thread;
        if (pthread_create(&start_thread, NULL, start_thread_main, &start_arg) == 0) {
            uint64_t deadline = mono_ns() + 1000000000ull;
            while (!atomic_load_explicit(&start_arg.done, memory_order_acquire) && mono_ns() < deadline) {
                struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL);
            }
            CHECK(atomic_load_explicit(&start_arg.done, memory_order_acquire), "enabled start returns while g_stop_mu remains held");
            CHECK(nvn_vk_pipe_demand_test_start_lock_denials() == denials_before + 1, "denied start increments the monotonic counter once");
            CHECK(atomic_load_explicit(&g_start_denial_logs, memory_order_acquire) == logs_before + 1, "denied start emits one warning");
            CHECK(strstr(g_last_start_denial, "g_stop_mu trylock failed rc=") != NULL && strstr(g_last_start_denial, "denials=1") != NULL, "warning names the failed trylock and cumulative count");
            CHECK(nvn_vk_pipe_demand_worker_count() == 0, "denied start creates no worker");
            nvn_vk_pipe_demand_test_unlock_lifecycle();
            pthread_join(start_thread, NULL);
        } else {
            CHECK(0, "create enabled start contention thread");
            nvn_vk_pipe_demand_test_unlock_lifecycle();
        }
    }
    memset(&g_prog, 0, sizeof g_prog);
    g_prog.sh[0] = (nvn_gpu_shader)(uintptr_t)0x1;
    g_prog.sh[1] = (nvn_gpu_shader)(uintptr_t)0x2;
    nvn_vk_pipe_demand_start();
    CHECK(nvn_vk_pipe_demand_worker_count() == 1, "initial worker is tracked");

    uint8_t idA[32], idC[32], idB[32];
    set_ident(idA, 0xA1); set_ident(idC, 0xC3); set_ident(idB, 0xB2);

    nvn_gpu_pipe_desc a; memset(&a, 0, sizeof a);
    a.st[0].sh = g_prog.sh[0]; a.st[1].sh = g_prog.sh[1]; a.prim = 3;

    nvn_gpu_pipe cached = NULL, out = NULL; char why[128];
    int k = nvn_vk_pipe_demand_kick(&a, idA, &cached);
    CHECK(k == NVK_DEMAND_PENDING, "first kick PENDING");
    int w = nvn_vk_pipe_demand_wait(&a, idA, 2000, &out, why, sizeof why);
    CHECK(w == 0 && out != NULL, "wait READY after background build");
    nvn_gpu_pipe hit = NULL;
    CHECK(nvn_gpu_pipe_lookup(&a, &hit) && hit == out, "lookup hits after build");
    CHECK(nvn_vk_pipe_demand_state(&a, idA, &cached) == NVK_DEMAND_READY, "state READY");

    k = nvn_vk_pipe_demand_kick(&a, idA, &cached);
    CHECK(k == NVK_DEMAND_READY && cached == out, "second kick READY from cache");

    // P0 回归：状态/格式相同、仅着色器句柄不同 → 必须各自建，不得串管线
    nvn_gpu_pipe outA = out;
    nvn_gpu_pipe_desc c; memset(&c, 0, sizeof c);
    c.st[0].sh = (nvn_gpu_shader)(uintptr_t)0x11; c.st[1].sh = (nvn_gpu_shader)(uintptr_t)0x22; c.prim = 3;
    int kc = nvn_vk_pipe_demand_kick(&c, idC, &cached);
    CHECK(kc == NVK_DEMAND_PENDING, "diff-shader rename kick must be PENDING (not READY from A)");
    nvn_gpu_pipe outC = NULL;
    int wc = nvn_vk_pipe_demand_wait(&c, idC, 2000, &outC, why, sizeof why);
    CHECK(wc == 0 && outC != NULL, "diff-shader wait READY");
    CHECK(outC != outA, "P0: diff-shader recipe must NOT get A's pipeline object");

    // 后台失败
    pthread_mutex_lock(&g_mu); g_fail_next = 1; g_has_inst = 0; pthread_mutex_unlock(&g_mu);
    nvn_gpu_pipe_desc b; memset(&b, 0, sizeof b); b.st[0].sh = g_prog.sh[0]; b.prim = 4;
    k = nvn_vk_pipe_demand_kick(&b, idB, &cached);
    CHECK(k == NVK_DEMAND_PENDING, "fail-recipe kick PENDING");
    w = nvn_vk_pipe_demand_wait(&b, idB, 2000, &out, why, sizeof why);
    CHECK(w == 2 && out == NULL, "wait FAILED with why");
    CHECK(why[0] != 0, "failure carries why text");

    nvn_vk_pipe_demand_stop();
    CHECK(nvn_vk_pipe_demand_quiescent(), "completed worker is reaped after stop");
    struct nvn_pipe_demand_stats settled;
    nvn_vk_pipe_demand_stats(&settled);
    CHECK(settled.published >= 2, "published stats retain admitted pipeline results");
    CHECK(nvn_vk_pipe_demand_worker_count() == 0, "completed worker count returns to zero");

    nvn_vk_pipe_demand_start();
    CHECK(nvn_vk_pipe_demand_worker_count() == 1, "new generation owns one worker");
    nvn_gpu_pipe_desc slow; memset(&slow, 0, sizeof slow);
    slow.st[0].sh = (nvn_gpu_shader)(uintptr_t)0x31; slow.prim = 8;
    uint8_t idSlow[32]; set_ident(idSlow, 0xD4);
    pthread_mutex_lock(&g_mu); g_fail_next = 0; g_has_inst = 0; g_gate_hold = 1; g_gate_entered = 0; pthread_mutex_unlock(&g_mu);
    k = nvn_vk_pipe_demand_kick(&slow, idSlow, &cached);
    CHECK(k == NVK_DEMAND_PENDING, "gated build kick PENDING");
    if (!wait_gate_entered(HANG_MS)) {
        CHECK(0, "fake build reaches deterministic gate");
        gate_release(); nvn_vk_pipe_demand_stop(); wait_demand_idle(HANG_MS);
        printf("%s (%d failures, builds=%d)\n", fails ? "RED" : "GREEN", fails, g_build_calls);
        return fails ? 1 : 0;
    }

    nvn_gpu_pipe_desc queued; memset(&queued, 0, sizeof queued);
    queued.st[0].sh = (nvn_gpu_shader)(uintptr_t)0x32; queued.prim = 9;
    uint8_t idQueued[32]; set_ident(idQueued, 0xD5);
    CHECK(nvn_vk_pipe_demand_kick(&queued, idQueued, &cached) == NVK_DEMAND_PENDING, "second gated job queues behind active build");

    struct nvn_pipe_demand_stats before, after;
    nvn_vk_pipe_demand_stats(&before);
    CHECK(before.published == 0, "held generation has no published result before stop");
    int lifecycle_lock = nvn_vk_pipe_demand_test_lock_lifecycle();
    CHECK(lifecycle_lock == 0, "test holds lifecycle serialization for busy-stop case");
    const uint64_t busy_before = nvn_vk_pipe_demand_test_stop_busy_returns();
    struct stop_thread_arg busy_arg = {0};
    pthread_t busy_thread;
    int busy_started = pthread_create(&busy_thread, NULL, stop_thread_main, &busy_arg) == 0;
    CHECK(busy_started, "create busy-lifecycle stop thread");
    if (busy_started) {
        // 锁还在测试手里时 stop 就必须返回：排队等锁的实现在这里一直等不到 done
        for (uint64_t end = mono_ns() + (uint64_t)HANG_MS * 1000000ull;
             !atomic_load_explicit(&busy_arg.done, memory_order_acquire) && mono_ns() < end;) {
            struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL);
        }
        CHECK(atomic_load_explicit(&busy_arg.done, memory_order_acquire), "stop returns while lifecycle serialization is still held");
    }
    if (lifecycle_lock == 0) nvn_vk_pipe_demand_test_unlock_lifecycle();
    if (busy_started) pthread_join(busy_thread, NULL);
    CHECK(nvn_vk_pipe_demand_test_stop_busy_returns() == busy_before + 1, "stop publishes without waiting when lifecycle serialization is busy");
    CHECK(nvn_vk_pipe_demand_kick(&queued, idQueued, &cached) == NVK_DEMAND_NONE, "busy-lifecycle stop closes queue admission");

    const uint64_t timeouts0 = nvn_vk_pipe_demand_test_stop_timeouts();
    struct stop_thread_arg stop_arg = {0};
    pthread_t stop_thread;
    uint64_t stop_ms;
    if (pthread_create(&stop_thread, NULL, stop_thread_main, &stop_arg) == 0) {
        int lifecycle_busy = wait_lifecycle_busy(HANG_MS);
        CHECK(lifecycle_busy, "first stop holds lifecycle serialization while waiting");
        if (lifecycle_busy) {
            const uint64_t busy0 = nvn_vk_pipe_demand_test_stop_busy_returns(), deny0 = nvn_vk_pipe_demand_test_start_lock_denials();
            nvn_vk_pipe_demand_stop();
            CHECK(nvn_vk_pipe_demand_test_stop_busy_returns() == busy0 + 1, "concurrent stop returns instead of queuing another bounded wait");
            nvn_vk_pipe_demand_start();
            CHECK(nvn_vk_pipe_demand_test_start_lock_denials() == deny0 + 1, "start returns instead of queuing behind stop");
            CHECK(!nvn_vk_pipe_demand_quiescent(), "quiescence is conservatively false while lifecycle serialization is busy");
            CHECK(nvn_vk_pipe_demand_worker_count() == 1, "concurrent lifecycle calls retain the single worker");
        }
        for (uint64_t end = mono_ns() + (uint64_t)HANG_MS * 1000000ull;
             !atomic_load_explicit(&stop_arg.done, memory_order_acquire) && mono_ns() < end;) {
            struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL);
        }
        CHECK(atomic_load_explicit(&stop_arg.done, memory_order_acquire), "first bounded stop completes while build is held");
        if (!atomic_load_explicit(&stop_arg.done, memory_order_acquire)) gate_release();   // 等待无界的实现：放开 build 才能回收线程，不让测试挂死
        pthread_join(stop_thread, NULL);
        stop_ms = stop_arg.elapsed_ms;
    } else {
        CHECK(0, "create concurrent stop thread");
        uint64_t stop_start = mono_ns();
        nvn_vk_pipe_demand_stop();
        stop_ms = (mono_ns() - stop_start) / 1000000ull;
    }
    // 下沿：有界等待至少等足 500 次 1 ms（定时器不会提前到）。上沿不看墙钟：build 一直被扣住，
    //   stop 能返回本身就说明等待有界；返回的路径必须是「到点未退、保留归属」那一条（超时计数 +1）。
    CHECK(stop_ms >= 450, "bounded stop waits for the build before giving up");
    CHECK(nvn_vk_pipe_demand_test_stop_timeouts() == timeouts0 + 1, "stop stays bounded while build is held (takes the timeout path)");
    printf("held-build stop: %llu ms (reading only)\n", (unsigned long long)stop_ms);
    CHECK(!nvn_vk_pipe_demand_quiescent(), "timed-out stop retains live worker ownership");
    CHECK(nvn_vk_pipe_demand_worker_count() == 1, "timed-out worker remains active");
    int builds_held = g_build_calls;
    nvn_vk_pipe_demand_start();
    CHECK(nvn_vk_pipe_demand_worker_count() == 1, "refused restart does not create a second worker");
    nvn_vk_pipe_demand_stats(&after);
    CHECK(!nvn_vk_pipe_demand_quiescent(), "start cannot replace the live worker");
    CHECK(after.kicked == before.kicked && g_build_calls == builds_held, "refused restart does not reset state or create a worker");
    CHECK(nvn_vk_pipe_demand_kick(&slow, idSlow, &cached) == NVK_DEMAND_NONE, "stop keeps queue admission closed");

    pthread_mutex_lock(&g_mu); g_fail_next = 1; pthread_mutex_unlock(&g_mu);
    gate_release();
    CHECK(wait_demand_idle(HANG_MS), "late worker completion is joined before close/restart");
    pthread_mutex_lock(&g_mu); int builds_after_stop = g_build_calls; pthread_mutex_unlock(&g_mu);
    CHECK(builds_after_stop == builds_held, "busy-lifecycle stop discards queued work behind active build");
    CHECK(nvn_vk_pipe_demand_worker_count() == 0, "reaped worker count returns to zero");
    nvn_vk_pipe_demand_stats(&after);
    CHECK(after.published == before.published, "stopped generation does not increment published stats");
    CHECK(nvn_vk_pipe_demand_state(&slow, idSlow, &cached) == NVK_DEMAND_NONE, "stopped generation does not publish late result");
    nvn_vk_pipe_demand_stop();
    nvn_vk_pipe_demand_start();
    CHECK(nvn_vk_pipe_demand_worker_count() == 1, "restarted generation owns one worker");
    nvn_gpu_pipe_desc next; memset(&next, 0, sizeof next);
    next.st[0].sh = (nvn_gpu_shader)(uintptr_t)0x41; next.prim = 9;
    uint8_t idNext[32]; set_ident(idNext, 0xE5);
    pthread_mutex_lock(&g_mu); g_fail_next = 0; pthread_mutex_unlock(&g_mu);
    CHECK(nvn_vk_pipe_demand_kick(&next, idNext, &cached) == NVK_DEMAND_PENDING, "restart after reap accepts new work");
    CHECK(nvn_vk_pipe_demand_wait(&next, idNext, 2000, &out, why, sizeof why) == 0 && out != NULL, "restarted generation builds successfully");
    check_cold_pair();
    nvn_vk_pipe_demand_stop();
    CHECK(nvn_vk_pipe_demand_quiescent(), "final stop leaves no joinable worker");
    CHECK(nvn_vk_pipe_demand_worker_count() == 0, "final stop leaves no active worker");
    printf("%s (%d failures, builds=%d)\n", fails ? "RED" : "GREEN", fails, g_build_calls);
    return fails ? 1 : 0;
}
