// 首用管线后台构建（安卓新回放层）。合同见 nvn_vk_pipe_demand.h。
#include "nvn_vk_pipe_demand.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "nvn_pipe_pend.h"
#include "nvn_plat.h"     // nvn_plat_flag（debug.s2a.*）
#include "rp_shader.h"    // rp_shader_prog_borrow/return / struct rp_prog
// 弱链接[pipedemand]：s2a_fu_rt_reset（nvn_first_use.c 提供；android/tools/nvn_vk_pipe_demand_test.sh 不链它）
void s2a_fu_rt_reset(void) __attribute__((weak));   // 首用 fresh/seen 历史清表（nvn_first_use.c；弱引用，宿主缺实现就跳过）

#define DEM_MAX NVK_PP_MAX
#define DEM_STAGES 5
#define DEM_WORKERS_MAX 4

// 待定键：管线描述（阶段着色器句柄置空）＋ 程序内容身份。必须带身份，见头文件「关键」。
struct dem_key {
    nvn_gpu_pipe_desc desc;
    uint8_t ident[32], variant_sha[32];
};
struct dem_job {
    nvn_gpu_pipe_desc desc;   // st[].sh 置空；后台线程用借到的程序填
    uint8_t ident[32];
    uint8_t stagemask, cold;
    uint8_t variant_sha[32];
    uint64_t gen;             // 入队时的会话代号：停局后迟到的结果按它丢弃
};

static nvk_pp g_pp;
static _Atomic int g_pp_init;
static _Atomic int g_accepting, g_stop;
static _Atomic int g_workers;    // 尚未退出的线程；减到 0 才能回收整代
static pthread_t g_threads[DEM_WORKERS_MAX];
static int g_thread_count;       // joinable 线程，由 g_life_mu 保护
static pthread_mutex_t g_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_q_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_life_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_stop_mu = PTHREAD_MUTEX_INITIALIZER;
static int q[DEM_MAX], qh, qn;
static struct dem_job g_jobs[DEM_MAX];
static struct nvn_pipe_demand_stats g_st;   // 原子累加
static _Atomic uint64_t g_start_lock_denials;
// 停局走了哪条路（只计数，供宿主测试按事件判，不按墙钟）：
//   busy = 另一个 stop 正持着生命周期锁，本次只关入口、不再排第二次有界等待；timeout = 有界等待到点、worker 还没退，保留归属
static _Atomic uint64_t g_stop_busy_returns, g_stop_timeouts;
#if defined(S2A_NVN_PIPE_DEMAND_TEST)
static _Atomic int g_test_on = -1;
#endif
// 会话代号：每次 start / stop 加一。迟到的结果不会发布进下一局的待定表。
static _Atomic uint64_t g_gen;
static int dem_gen_live(uint64_t gen) { return atomic_load_explicit(&g_gen, memory_order_acquire) == gen; }
static void dem_publish(uint64_t gen, int h, void *obj, int code, const char *why) {
    pthread_mutex_lock(&g_life_mu);   // start 在这把锁下换代 + reset：判代号与发布之间不会插进换代
    if (atomic_load_explicit(&g_accepting, memory_order_acquire) && dem_gen_live(gen)) {
        nvk_pp_publish(&g_pp, h, obj, code, why);
        __atomic_fetch_add(&g_st.published, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_life_mu);
}

static int dem_eq(const void *a, const void *b) { return memcmp(a, b, sizeof(struct dem_key)) == 0; }

static pthread_once_t g_on_once = PTHREAD_ONCE_INIT;
static int g_on;
static void dem_on_init(void) { g_on = nvn_plat_flag("nvk_pipe_async", 1) != 0; }

// A driver forced to use the backend's serial policy should occupy only one
// worker. No driver name, vendor or device model participates in the decision.
static int dem_worker_limit(void) {
    if (nvn_plat_flag("nvk_pipe_serial", 0)) return 1;
    int requested = nvn_plat_flag("nvk_pipe_workers", 0);
    if (!requested) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus < 4) return 1;
        requested = cpus >= DEM_WORKERS_MAX * 2 ? DEM_WORKERS_MAX : (int)(cpus / 2);
    }
    if (requested < 1) return 1;
    return requested > DEM_WORKERS_MAX ? DEM_WORKERS_MAX : requested;
}

int nvn_vk_pipe_demand_on(void) {
#if defined(S2A_NVN_PIPE_DEMAND_TEST)
    int test_on = atomic_load_explicit(&g_test_on, memory_order_acquire);
    if (test_on >= 0) return test_on;
#endif
    pthread_once(&g_on_once, dem_on_init);
    return g_on;
}
uint32_t nvk_fu_item_ms(void)   { static int v = -2; if (v == -2) v = nvn_plat_flag("fu_item_ms", 2000); return v > 0 ? (uint32_t)v : 0; }
uint32_t nvk_fu_frame_ms(void)  { static int v = -2; if (v == -2) v = nvn_plat_flag("fu_frame_ms", 100); return v > 0 ? (uint32_t)v : 0; }
uint32_t nvk_fu_unsafe_ms(void) { static int v = -2; if (v == -2) v = nvn_plat_flag("fu_unsafe_ms", 10000); return v > 0 ? (uint32_t)v : 0; }

static void dem_key(const nvn_gpu_pipe_desc *d, const uint8_t ident[32], struct dem_key *k) {
    memset(k, 0, sizeof *k);
    k->desc = *d;
    for (int st = 0; st < DEM_STAGES; st++) k->desc.st[st].sh = NULL;
    memcpy(k->ident, ident, 32);
}

static void dem_build(int h, struct dem_job j) {
    if (!dem_gen_live(j.gen)) return;   // 上一局的活：不建、不发布
    const struct rp_prog *prog = NULL;
    int br = rp_shader_prog_borrow(j.ident, &prog);
    if (br == 0) {
        __atomic_fetch_add(&g_st.failed, 1, __ATOMIC_RELAXED);
        dem_publish(j.gen, h, NULL, NVN_GPU_E_COMPILE, "程序还没发布（借用失败；下一笔现场建）");
        return;
    }
    if (br < 0 || !prog) {
        __atomic_fetch_add(&g_st.failed, 1, __ATOMIC_RELAXED);
        dem_publish(j.gen, h, NULL, NVN_GPU_E_STOPPED, "程序借用不可用（停局 / 借用表满）");
        return;
    }
    if (j.cold && (!prog->cold || memcmp(prog->cold->variant_sha, j.variant_sha, sizeof j.variant_sha))) {
        rp_shader_prog_return(j.ident);
        __atomic_fetch_add(&g_st.failed, 1, __ATOMIC_RELAXED);
        dem_publish(j.gen, h, NULL, NVN_GPU_E_COMPILE, "cold fragment identity changed");
        return;
    }
    nvn_gpu_pipe_desc pd = j.desc;
    for (int st = 0; st < DEM_STAGES; st++) pd.st[st].sh = (j.stagemask >> st & 1u) ? prog->sh[st] : NULL;
    if (j.cold) pd.st[NVN_GPU_SH_FS].sh = prog->cold->fragment;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err err = {0};
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err);
    rp_shader_prog_return(j.ident);
    if (nvn_gpu_ok(r) && pipe) {
        __atomic_fetch_add(&g_st.built, 1, __ATOMIC_RELAXED);
        dem_publish(j.gen, h, pipe, 0, NULL);
    } else {
        __atomic_fetch_add(&g_st.failed, 1, __ATOMIC_RELAXED);
        dem_publish(j.gen, h, NULL, r.code ? r.code : NVN_GPU_E_COMPILE, err.why[0] ? err.why : "后台建管线失败");
    }
}

static void *dem_main(void *arg) {
    nvk_pipe_demand_worker_enter();
    const uint64_t mine = (uint64_t)(uintptr_t)arg;
    for (;;) {
        pthread_mutex_lock(&g_q_mu);
        while (qn == 0 && !atomic_load_explicit(&g_stop, memory_order_acquire) && dem_gen_live(mine)) pthread_cond_wait(&g_q_cv, &g_q_mu);
        if (!dem_gen_live(mine) || (atomic_load_explicit(&g_stop, memory_order_acquire) && qn == 0)) { pthread_mutex_unlock(&g_q_mu); break; }
        int h = q[qh]; qh = (qh + 1) % DEM_MAX; qn--;
        struct dem_job job = g_jobs[h];
        pthread_mutex_unlock(&g_q_mu);
        dem_build(h, job);
    }
    atomic_fetch_sub_explicit(&g_workers, 1, memory_order_acq_rel);
    return NULL;
}

static void dem_reap_locked(void) {
    if (atomic_load_explicit(&g_workers, memory_order_acquire)) return;
    while (g_thread_count) {
        if (pthread_join(g_threads[g_thread_count - 1], NULL) != 0) return;
        g_thread_count--;
    }
}

void nvn_vk_pipe_demand_start(void) {
    if (!nvn_vk_pipe_demand_on()) return;
    int lock_rc = pthread_mutex_trylock(&g_stop_mu);
    if (lock_rc != 0) {
        uint64_t denials = atomic_fetch_add_explicit(&g_start_lock_denials, 1, memory_order_relaxed) + 1;
        char msg[160];
        snprintf(msg, sizeof msg, "nvn_vk_pipe_demand_start skipped: g_stop_mu trylock failed rc=%d denials=%llu",
                 lock_rc, (unsigned long long)denials);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "pipe-demand", msg);
        return;
    }
    pthread_mutex_lock(&g_life_mu);
    dem_reap_locked();
    if (g_thread_count) {
        pthread_mutex_unlock(&g_life_mu);
        pthread_mutex_unlock(&g_stop_mu);
        return;
    }
    const uint64_t gen = atomic_fetch_add_explicit(&g_gen, 1, memory_order_acq_rel) + 1;
    if (!atomic_load_explicit(&g_pp_init, memory_order_acquire)) { nvk_pp_init(&g_pp, sizeof(struct dem_key)); atomic_store_explicit(&g_pp_init, 1, memory_order_release); }
    else nvk_pp_reset(&g_pp);
    if (s2a_fu_rt_reset) s2a_fu_rt_reset();   // 跨会话：清 fresh/seen 历史，别依赖帧号单调
    __atomic_store_n(&g_st.kicked, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_st.dedup, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_st.built, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_st.failed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_st.timeouts, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_st.full, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_st.published, 0, __ATOMIC_RELAXED);
    qh = qn = 0;
    atomic_store_explicit(&g_stop, 0, memory_order_release);
    atomic_store_explicit(&g_accepting, 0, memory_order_release);
    const int limit = dem_worker_limit();
    for (int i = 0; i < limit; i++) {
        atomic_fetch_add_explicit(&g_workers, 1, memory_order_relaxed);
        if (pthread_create(&g_threads[i], NULL, dem_main, (void *)(uintptr_t)gen) != 0) {
            atomic_fetch_sub_explicit(&g_workers, 1, memory_order_relaxed);
            break;
        }
        g_thread_count++;
    }
    // Partial creation preserves the workers already created. With none,
    // kick returns NONE and the replay caller falls back to SYNC.
    atomic_store_explicit(&g_stop, g_thread_count == 0, memory_order_release);
    atomic_store_explicit(&g_accepting, g_thread_count != 0, memory_order_release);
    char msg[112];
    snprintf(msg, sizeof msg, "首用管线后台线程：%d/%d", g_thread_count, limit);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "pipe-demand", msg);
    pthread_mutex_unlock(&g_life_mu);
    pthread_mutex_unlock(&g_stop_mu);
}

void nvn_vk_pipe_demand_stop(void) {
    int owns_wait = pthread_mutex_trylock(&g_stop_mu) == 0;
    pthread_mutex_lock(&g_life_mu);
    atomic_store_explicit(&g_accepting, 0, memory_order_release);
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    atomic_fetch_add_explicit(&g_gen, 1, memory_order_acq_rel);
    pthread_mutex_lock(&g_q_mu);
    qn = 0;   // 丢弃未开工的作业，所有已开工线程共用一次停局等待
    pthread_cond_broadcast(&g_q_cv);
    pthread_mutex_unlock(&g_q_mu);
    if (atomic_load_explicit(&g_pp_init, memory_order_acquire)) nvk_pp_reset(&g_pp);
    const int running = g_thread_count != 0;
    pthread_mutex_unlock(&g_life_mu);
    if (!owns_wait) { atomic_fetch_add_explicit(&g_stop_busy_returns, 1, memory_order_relaxed); return; }
    if (!running) { pthread_mutex_unlock(&g_stop_mu); return; }
    // 有界等待：超时后保留 joinable worker，直到之后确认退出再回收。
    int done = 0;
    for (int i = 0; i < 500; i++) {
        if (!atomic_load_explicit(&g_workers, memory_order_acquire)) { done = 1; break; }
        struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL);
    }
    if (done) {
        pthread_mutex_lock(&g_life_mu);
        dem_reap_locked();
        pthread_mutex_unlock(&g_life_mu);
    } else atomic_fetch_add_explicit(&g_stop_timeouts, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_stop_mu);
}

int nvn_vk_pipe_demand_quiescent(void) {
    if (pthread_mutex_trylock(&g_stop_mu) != 0) return 0;
    if (pthread_mutex_trylock(&g_life_mu) != 0) {
        pthread_mutex_unlock(&g_stop_mu);
        return 0;
    }
    dem_reap_locked();
    int idle = g_thread_count == 0;
    pthread_mutex_unlock(&g_life_mu);
    pthread_mutex_unlock(&g_stop_mu);
    return idle;
}

#if defined(S2A_NVN_PIPE_DEMAND_TEST)
int nvn_vk_pipe_demand_worker_count(void) {
    return atomic_load_explicit(&g_workers, memory_order_acquire);
}

uint64_t nvn_vk_pipe_demand_test_start_lock_denials(void) {
    return atomic_load_explicit(&g_start_lock_denials, memory_order_acquire);
}

uint64_t nvn_vk_pipe_demand_test_stop_busy_returns(void) {
    return atomic_load_explicit(&g_stop_busy_returns, memory_order_acquire);
}

uint64_t nvn_vk_pipe_demand_test_stop_timeouts(void) {
    return atomic_load_explicit(&g_stop_timeouts, memory_order_acquire);
}

void nvn_vk_pipe_demand_test_set_on(int on) {
    atomic_store_explicit(&g_test_on, on ? 1 : 0, memory_order_release);
}

int nvn_vk_pipe_demand_test_lifecycle_busy(void) {
    if (pthread_mutex_trylock(&g_stop_mu) != 0) return 1;
    pthread_mutex_unlock(&g_stop_mu);
    return 0;
}

int nvn_vk_pipe_demand_test_lock_lifecycle(void) {
    return pthread_mutex_lock(&g_stop_mu);
}

void nvn_vk_pipe_demand_test_unlock_lifecycle(void) {
    pthread_mutex_unlock(&g_stop_mu);
}
#endif

static int dem_kick(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32],
                    const uint8_t *variant_sha, nvn_gpu_pipe *cached) {
    if (cached) *cached = NULL;
    if (!d || !prog_identity) return NVK_DEMAND_NONE;
    if (nvn_gpu_pipe_lookup(d, cached)) return NVK_DEMAND_READY;
    if (!nvn_vk_pipe_demand_on()) return NVK_DEMAND_NONE;
    pthread_mutex_lock(&g_life_mu);
    if (!atomic_load_explicit(&g_accepting, memory_order_acquire)) {
        pthread_mutex_unlock(&g_life_mu);
        return NVK_DEMAND_NONE;
    }

    // 只产生 VS/FS/GS：带 TCS/TES 的描述本层不建，回退 SYNC（不静默截阶段）。
    uint8_t mask = 0;
    for (int st = 0; st < DEM_STAGES; st++) if (d->st[st].sh) mask |= (uint8_t)(1u << st);
    if (mask & ~0x7u) { pthread_mutex_unlock(&g_life_mu); return NVK_DEMAND_NONE; }

    struct dem_key key; dem_key(d, prog_identity, &key);
    if (variant_sha) memcpy(key.variant_sha, variant_sha, sizeof key.variant_sha);
    int h = -1;
    int r = nvk_pp_enter(&g_pp, &key, dem_eq, &h);
    if (r == NVK_PP_ENTER_FOLLOW) {
        void *obj = NULL; int code = 0; char why[NVK_PP_WHY];
        int st = nvk_pp_state(&g_pp, &key, dem_eq, &obj, &code, why, sizeof why);
        if (st == NVK_PP_READY) { if (cached) *cached = (nvn_gpu_pipe)obj; pthread_mutex_unlock(&g_life_mu); return NVK_DEMAND_READY; }
        if (st == NVK_PP_FAILED) { pthread_mutex_unlock(&g_life_mu); return NVK_DEMAND_FAILED; }
        if (st == NVK_PP_COMPILING) { __atomic_fetch_add(&g_st.dedup, 1, __ATOMIC_RELAXED); pthread_mutex_unlock(&g_life_mu); return NVK_DEMAND_PENDING; }
        pthread_mutex_unlock(&g_life_mu);
        return NVK_DEMAND_NONE;
    }
    if (r != NVK_PP_ENTER_BUILDER) { __atomic_fetch_add(&g_st.full, 1, __ATOMIC_RELAXED); pthread_mutex_unlock(&g_life_mu); return NVK_DEMAND_NONE; }

    pthread_mutex_lock(&g_q_mu);
    if (qn == DEM_MAX || (variant_sha && qn >= 8)) {
        pthread_mutex_unlock(&g_q_mu);
        nvk_pp_abandon(&g_pp, h);
        __atomic_fetch_add(&g_st.full, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_life_mu);
        return NVK_DEMAND_NONE;
    }
    struct dem_job *j = &g_jobs[h];
    j->desc = *d;
    for (int st = 0; st < DEM_STAGES; st++) j->desc.st[st].sh = NULL;
    memcpy(j->ident, prog_identity, 32);
    j->stagemask = (uint8_t)(mask & 0x7u);
    j->cold = variant_sha != NULL;
    memcpy(j->variant_sha, key.variant_sha, sizeof j->variant_sha);
    j->gen = atomic_load_explicit(&g_gen, memory_order_acquire);

    q[(qh + qn) % DEM_MAX] = h;
    qn++;
    pthread_cond_signal(&g_q_cv);
    pthread_mutex_unlock(&g_q_mu);
    __atomic_fetch_add(&g_st.kicked, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_life_mu);
    return NVK_DEMAND_PENDING;
}

int nvn_vk_pipe_demand_kick(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], nvn_gpu_pipe *cached) {
    return dem_kick(d, prog_identity, NULL, cached);
}

int nvn_vk_pipe_demand_cold_kick(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32],
                                const uint8_t variant_sha[32], nvn_gpu_pipe *cached) {
    return variant_sha ? dem_kick(d, prog_identity, variant_sha, cached) : NVK_DEMAND_NONE;
}

int nvn_vk_pipe_demand_state(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], nvn_gpu_pipe *cached) {
    if (cached) *cached = NULL;
    if (!d || !prog_identity) return NVK_DEMAND_NONE;
    if (nvn_gpu_pipe_lookup(d, cached)) return NVK_DEMAND_READY;
    if (!nvn_vk_pipe_demand_on() || !atomic_load_explicit(&g_accepting, memory_order_acquire)) return NVK_DEMAND_NONE;
    struct dem_key key; dem_key(d, prog_identity, &key);
    void *obj = NULL; int code = 0; char why[NVK_PP_WHY];
    int st = nvk_pp_state(&g_pp, &key, dem_eq, &obj, &code, why, sizeof why);
    if (st == NVK_PP_READY) { if (cached) *cached = (nvn_gpu_pipe)obj; return NVK_DEMAND_READY; }
    if (st == NVK_PP_FAILED) return NVK_DEMAND_FAILED;
    if (st == NVK_PP_COMPILING) return NVK_DEMAND_PENDING;
    return NVK_DEMAND_NONE;
}

int nvn_vk_pipe_demand_wait(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], uint32_t wait_ms, nvn_gpu_pipe *out, char *why, size_t why_cap) {
    if (out) *out = NULL;
    if (why && why_cap) why[0] = 0;
    if (!d || !prog_identity) return 3;
    if (nvn_gpu_pipe_lookup(d, out)) return 0;
    if (!nvn_vk_pipe_demand_on() || !atomic_load_explicit(&g_accepting, memory_order_acquire)) return 3;
    struct dem_key key; dem_key(d, prog_identity, &key);
    void *obj = NULL; int code = 0;
    int w = nvk_pp_wait(&g_pp, &key, dem_eq, wait_ms, &obj, &code, why, why_cap);
    if (w == NVK_PP_WAIT_READY) { if (out) *out = (nvn_gpu_pipe)obj; return 0; }
    if (w == NVK_PP_WAIT_FAILED) return 2;
    if (w == NVK_PP_WAIT_TIMEOUT) { __atomic_fetch_add(&g_st.timeouts, 1, __ATOMIC_RELAXED); return 1; }
    return 3;
}

void nvn_vk_pipe_demand_mark(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], int is_stuck) {
    if (!d || !prog_identity || !atomic_load_explicit(&g_pp_init, memory_order_acquire)) return;
    struct dem_key key; dem_key(d, prog_identity, &key);
    nvk_pp_mark(&g_pp, &key, dem_eq, is_stuck);
}
int nvn_vk_pipe_demand_marks(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], int *late, int *stuck) {
    if (late) *late = 0;
    if (stuck) *stuck = 0;
    if (!d || !prog_identity || !atomic_load_explicit(&g_pp_init, memory_order_acquire)) return 0;
    struct dem_key key; dem_key(d, prog_identity, &key);
    return nvk_pp_marks(&g_pp, &key, dem_eq, late, stuck);
}

void nvn_vk_pipe_demand_stats(struct nvn_pipe_demand_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->kicked = __atomic_load_n(&g_st.kicked, __ATOMIC_RELAXED);
    out->dedup = __atomic_load_n(&g_st.dedup, __ATOMIC_RELAXED);
    out->published = __atomic_load_n(&g_st.published, __ATOMIC_RELAXED);
    out->built = __atomic_load_n(&g_st.built, __ATOMIC_RELAXED);
    out->failed = __atomic_load_n(&g_st.failed, __ATOMIC_RELAXED);
    out->timeouts = __atomic_load_n(&g_st.timeouts, __ATOMIC_RELAXED);
    out->full = __atomic_load_n(&g_st.full, __ATOMIC_RELAXED);
}