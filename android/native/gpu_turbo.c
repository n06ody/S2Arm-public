// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// Adreno GPU 频率控制。独立的低频监控线程采样 KGSL 频率、出图速率和前台状态。
// 每 250 ms 判定一次，其余时间在条件变量上睡眠。
#include "gpu_turbo.h"
#include "gpu_turbo_policy.h"
#include "system_tuning.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef S2A_GPU_TURBO_TEST
// 宿主测试用模拟时钟、属性与 KGSL，保留监控线程和判定体。
extern int s2a_gpu_turbo_test_gate(void);
extern const char *s2a_gpu_turbo_test_prop(const char *name);
extern int s2a_gpu_turbo_test_read(const char *path, char *buf, size_t n);
extern uint64_t s2a_gpu_turbo_test_now(void);
extern uint64_t s2a_gpu_turbo_test_cpu_ns(int thread);
extern void s2a_gpu_turbo_test_log(const char *text);
extern int s2a_gpu_turbo_test_set(int on);
#define TURBO_PROP_MAX 92
static int gate_open(void) { return s2a_gpu_turbo_test_gate(); }
static int raw_prop_get(const char *name, char *out) {
    const char *v = s2a_gpu_turbo_test_prop(name);
    if (!v) { out[0] = 0; return 0; }
    snprintf(out, TURBO_PROP_MAX, "%s", v);
    return (int)strlen(out);
}
static int sysfs_read(const char *path, char *buf, size_t n) { return s2a_gpu_turbo_test_read(path, buf, n); }
static uint64_t now_ns(void) { return s2a_gpu_turbo_test_now(); }
static void turbo_log(const char *text) { s2a_gpu_turbo_test_log(text); }
#define TURBO_WAIT_CAP_NS 1000000ull   // 测试里真实等待最多 1 ms：模拟时钟由测试推进
#else
#include <android/log.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include "debug_gate.h"
#define TURBO_PROP_MAX PROP_VALUE_MAX
static int gate_open(void) { return s2a_debug_entries_open(); }
static int raw_prop_get(const char *name, char *out) { return __system_property_get(name, out); }
static int sysfs_read(const char *path, char *buf, size_t n) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    const ssize_t r = read(fd, buf, n - 1);
    const int error = r < 0 ? errno : 0;
    close(fd);
    if (r <= 0) return -error;
    buf[r] = 0;
    return (int)r;
}
static uint64_t now_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void turbo_log(const char *text) { __android_log_print(ANDROID_LOG_INFO, "S2Arm", "[s2a-gpu-turbo] %s", text); }
#endif

// debug.s2a.gputurbo 只在调试入口打开时读取。
static int prop_get(const char *name, char *out) {
    out[0] = 0;
    return gate_open() ? raw_prop_get(name, out) : 0;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static int cv_ready, started, joining, exited, cv_clock;
static pthread_t thr;
static _Atomic int g_fg, g_surface, g_stop, g_kick;
static _Atomic uint64_t g_frames;
#ifdef S2A_GPU_TURBO_TEST
static _Atomic uint64_t g_ticks;
static _Atomic uint64_t g_last_tick_ns;
#endif
static _Atomic uint64_t g_last_frame_ns;                 // 上一帧的时刻；0 = 没有（起局 / 前后台切换后第一帧不计间隔）
static _Atomic unsigned g_hist[S2A_TURBO_NBUCKET];       // 帧间隔直方图，监控线程每 5 秒取走

// 每帧一次（present 线程）：帧计数 + 与上一帧的间隔进直方图。零间隔（同一纳秒）不计。
void s2a_android_frame_note(void) {
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    const uint64_t now = now_ns();
    const uint64_t prev = atomic_exchange_explicit(&g_last_frame_ns, now, memory_order_relaxed);
    if (prev && now > prev) atomic_fetch_add_explicit(&g_hist[s2a_turbo_bucket(now - prev)], 1, memory_order_relaxed);
}

static const char *mode_name(int m) { return m == S2A_TURBO_OFF ? "off" : m == S2A_TURBO_ALWAYS ? "always" : "auto"; }
static const char *why_text(int why) {
    switch (why) {
    case S2A_TURBO_WHY_GATE: return "门关了";
    case S2A_TURBO_WHY_MODE: return "开关改了";
    case S2A_TURBO_WHY_DONE: return "帧率到了目标";
    case S2A_TURBO_WHY_IDLE: return "没在出图";
    case S2A_TURBO_WHY_CAP_DROP: return "系统在压上限";
    case S2A_TURBO_WHY_NO_GAIN: return "没有收益";
    default: return "-";
    }
}

// 睡到 ns 之后，或被生命周期变化 / 停局叫醒。
static void wait_for(uint64_t ns) {
#ifdef S2A_GPU_TURBO_TEST
    if (ns > TURBO_WAIT_CAP_NS) ns = TURBO_WAIT_CAP_NS;
#endif
    struct timespec ts;
    pthread_mutex_lock(&mu);
    if (!atomic_load(&g_stop)) {
        clock_gettime(cv_clock, &ts);
        uint64_t sec = ns / 1000000000ull, nsec = ns % 1000000000ull;
        ts.tv_sec += (time_t)sec; ts.tv_nsec += (long)nsec;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&cv, &mu, &ts);
    }
    pthread_mutex_unlock(&mu);
}

static uint64_t cpu_time_ns(clockid_t id) {
#ifdef S2A_GPU_TURBO_TEST
    return s2a_gpu_turbo_test_cpu_ns(id == CLOCK_THREAD_CPUTIME_ID);
#else
    struct timespec t;
    return clock_gettime(id, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

static uint64_t drain_hist(unsigned *h) {
    uint64_t total = 0;
    for (int i = 0; i < S2A_TURBO_NBUCKET; i++) { h[i] = atomic_exchange_explicit(&g_hist[i], 0, memory_order_relaxed); total += h[i]; }
    return total;
}

static void bucket_label(int b, char *out, size_t n) {
    if (b < 0) snprintf(out, n, "-");
    else if (b >= S2A_TURBO_NBUCKET - 1) snprintf(out, n, ">%u", s2a_turbo_bucket_limit_ms(S2A_TURBO_NBUCKET - 2));
    else snprintf(out, n, "≤%u", s2a_turbo_bucket_limit_ms(b));
}

// 一个 5 秒读数窗口的汇总
struct stat_win {
    uint64_t span_ns, proc_cpu_ns, thread_cpu_ns;
    const unsigned *hist;
    int active, latched, backoff;               // backoff：没有收益之后的暂缓期内
    uint32_t avg_khz, max_khz;
};

static void log_window(const struct stat_win *s) {
    char p50[8], p99[8], text[560];
    bucket_label(s2a_turbo_hist_quantile(s->hist, 500), p50, sizeof p50);
    bucket_label(s2a_turbo_hist_quantile(s->hist, 990), p99, sizeof p99);
    size_t o = (size_t)snprintf(text, sizeof text, "帧间隔 %.1f s：", (double)s->span_ns / 1e9);
    for (int i = 0; i < S2A_TURBO_NBUCKET && o < sizeof text; i++) {
        char lab[8]; bucket_label(i, lab, sizeof lab);
        o += (size_t)snprintf(text + o, sizeof text - o, "%s:%u ", lab, s->hist[i]);
    }
    if (o < sizeof text)
        o += (size_t)snprintf(text + o, sizeof text - o, "| p50 %s p99 %s ms | 慢帧 %llu | 进程 CPU %llu ms 监控线程 %llu ms | GPU 锁频=%s",
                              p50, p99, (unsigned long long)s2a_turbo_hist_slow(s->hist),
                              (unsigned long long)(s->proc_cpu_ns / 1000000ull),
                              (unsigned long long)(s->thread_cpu_ns / 1000000ull),
                              s->active ? "开" : s->latched ? "停用" : s->backoff ? "暂缓" : "关");
    if (o < sizeof text && s->max_khz) {
        if (s->avg_khz)
            snprintf(text + o, sizeof text - o, " | GPU 均频 %u 上限 %u MHz", s->avg_khz / 1000, s->max_khz / 1000);
        else
            snprintf(text + o, sizeof text - o, " | GPU 上限 %u MHz，均频读不到", s->max_khz / 1000);
    }
    turbo_log(text);
}

// KGSL PWRCTRL 为设备级属性。退出监控前交还调频器。
#ifdef S2A_GPU_TURBO_TEST
static int gpu_turbo_set(int on) { return s2a_gpu_turbo_test_set(on); }
#else
#include <sys/ioctl.h>
static int gpu_turbo_set(int on) {
    struct { unsigned type; void *value; size_t size; } prop;
    uint32_t enable = on ? 0u : 1u;   // KGSL_PROP_PWRCTRL（0xE）：0 = 关功耗控制（最高频）
    prop.type = 0xE; prop.value = &enable; prop.size = sizeof enable;
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return errno ? errno : -1;
    int r = ioctl(fd, _IOW(0x09, 0x32, prop), &prop);   // IOCTL_KGSL_SETPROPERTY
    int e = r ? (errno ? errno : -1) : 0;
    close(fd);
    return e;
}
#endif
static int read_gpu_mode(void) {
    char v[TURBO_PROP_MAX] = {0};
    if (prop_get("debug.s2a.gputurbo", v) <= 0) return S2A_TURBO_AUTO;
    if (!strcmp(v, "0") || !strcmp(v, "off")) return S2A_TURBO_OFF;
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "always")) return S2A_TURBO_ALWAYS;
    return S2A_TURBO_AUTO;
}
struct gq_src {
    uint64_t work, time, last_ns;
    uint32_t cur_khz, max_khz;
    int cur_denied, max_denied;
};
static uint32_t sysfs_hz_khz(const char *path, int *denied) {
    if (*denied) return 0;
    char b[32] = {0};
    const int r = sysfs_read(path, b, sizeof b);
    // A permission failure applies to this node in this monitor session.
    if (r == -EACCES || r == -EPERM) *denied = 1;
    if (r <= 0) return 0;
    unsigned long long hz = strtoull(b, NULL, 10);
    return hz >= 1000 && hz / 1000 <= UINT32_MAX ? (uint32_t)(hz / 1000) : 0;
}
static void gq_poll(struct gq_src *g, uint64_t now) {
    const uint32_t cur = sysfs_hz_khz("/sys/class/kgsl/kgsl-3d0/gpuclk", &g->cur_denied);
    const uint32_t max = sysfs_hz_khz("/sys/class/kgsl/kgsl-3d0/max_gpuclk", &g->max_denied);
    if (cur) {
        if (g->cur_khz && g->last_ns && now > g->last_ns) { uint64_t dt = (now - g->last_ns) / 1000; g->work += (uint64_t)g->cur_khz * dt; g->time += dt; }
        g->cur_khz = cur; g->last_ns = now;
    }
    if (max) g->max_khz = max;
}

static void *gpu_turbo_main(void *arg) {
    (void)arg;
#ifndef S2A_GPU_TURBO_TEST
    prctl(PR_SET_NAME, "s2a-gpu-turbo", 0, 0, 0);
#endif
    struct s2a_turbo_state state;
    s2a_turbo_reset_all(&state);
    struct gq_src gq = {0};
    int active = 0, unavailable = 0, kick = atomic_load(&g_kick);
    uint64_t next_tick = 0, next_stat = 0, stat_ns = 0;
    uint64_t stat_proc = 0, stat_thread = 0, stat_fw = 0, stat_ft = 0;
    while (!atomic_load(&g_stop)) {
        uint64_t now = now_ns();
        int k = atomic_load_explicit(&g_kick, memory_order_relaxed);
        if (k != kick) { kick = k; next_tick = 0; }
        if (now >= next_tick) {
            gq_poll(&gq, now);
            const struct s2a_turbo_in in = {
                .now_ns = now,
                .frames = atomic_load_explicit(&g_frames, memory_order_relaxed),
                .period_ns = s2a_android_session_period_ns(),   // 本局最短周期，不是当前周期（system_tuning.h）
                .mode = unavailable ? S2A_TURBO_OFF : read_gpu_mode(),
                .foreground = atomic_load(&g_fg), .surface = atomic_load(&g_surface), .stopped = atomic_load(&g_stop),
                .fq_work = gq.work, .fq_time = gq.time, .max_khz = gq.max_khz,
            };
            const int want = s2a_turbo_step(&state, &in);
            if (want != active) {
                const int e = gpu_turbo_set(want);
                char text[240];
                if (e && want) {
                    unavailable = 1;
                    snprintf(text, sizeof text, "GPU 锁频设不上（errno %d：没有 KGSL 或被拒），本局不再试", e);
                } else if (e) {
                    snprintf(text, sizeof text, "GPU 锁频交还失败（errno %d）", e);
                } else if (want) {
                    snprintf(text, sizeof text, "GPU 锁频开 mode=%s GPU 近 %d s 均频 / 上限 = %s%d%%",
                             mode_name(in.mode), S2A_TURBO_ENTER_WINDOWS,
                             state.ceil_permille >= 0 ? "" : "（读不到）",
                             state.ceil_permille >= 0 ? state.ceil_permille / 10 : 0);
                } else {
                    snprintf(text, sizeof text, "GPU 锁频关 原因=%s%s", why_text(state.why),
                             state.latched ? "，本局不再出手" : "");
                }
                turbo_log(text);
                if (!e) active = want;
                else if (!want) active = 0;
            }
            next_tick = now + S2A_TURBO_TICK_NS;
            if (!next_stat || now >= next_stat) {
                unsigned hist[S2A_TURBO_NBUCKET];
                const uint64_t total = drain_hist(hist);
                const uint64_t proc = cpu_time_ns(CLOCK_PROCESS_CPUTIME_ID);
                const uint64_t thread = cpu_time_ns(CLOCK_THREAD_CPUTIME_ID);
                if (next_stat && total) {
                    struct stat_win win = {
                        .span_ns = now - stat_ns, .proc_cpu_ns = proc - stat_proc,
                        .thread_cpu_ns = thread - stat_thread, .hist = hist,
                        .active = active, .latched = state.latched, .backoff = !active && state.nogain_until_ns > now,
                        .max_khz = gq.max_khz,
                    };
                    if (gq.time > stat_ft && gq.work >= stat_fw)
                        win.avg_khz = (uint32_t)((gq.work - stat_fw) / (gq.time - stat_ft));
                    log_window(&win);
                }
                stat_ns = now;
                next_stat = now + S2A_TURBO_STAT_NS;
                stat_proc = proc; stat_thread = thread; stat_fw = gq.work; stat_ft = gq.time;
            }
#ifdef S2A_GPU_TURBO_TEST
            atomic_store(&g_last_tick_ns, now);
            atomic_fetch_add_explicit(&g_ticks, 1, memory_order_release);
#endif
        }
        uint64_t t = now_ns();
        if (next_tick > t) wait_for(next_tick - t);
    }
    if (active) (void)gpu_turbo_set(0);
    return NULL;
}

static void ensure_cv_locked(void) {
    if (cv_ready) return;
    pthread_condattr_t a; pthread_condattr_init(&a);
#if defined(__APPLE__)
    cv_clock = CLOCK_REALTIME;   // macOS 没有 pthread_condattr_setclock：仅宿主语法 / 测试
#else
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC); cv_clock = CLOCK_MONOTONIC;
#endif
    pthread_cond_init(&cv, &a); pthread_condattr_destroy(&a);
    cv_ready = 1;
}

static void lifecycle_changed_locked(void) {
    atomic_fetch_add_explicit(&g_kick, 1, memory_order_relaxed);
    atomic_store_explicit(&g_last_frame_ns, 0, memory_order_relaxed);   // 前后台 / 显示面变化后的第一帧不计间隔（那一段不是游戏在出图）
    if (!started && !atomic_load(&g_stop) && atomic_load(&g_fg) && atomic_load(&g_surface)) {
        if (pthread_create(&thr, NULL, gpu_turbo_main, NULL) == 0) started = 1;
        else turbo_log("监控线程起不来，本局不锁频");
    }
    pthread_cond_broadcast(&cv);
}

void s2a_gpu_turbo_set_foreground(int foreground) {
    pthread_mutex_lock(&mu); ensure_cv_locked();
    atomic_store(&g_fg, !!foreground); lifecycle_changed_locked();
    pthread_mutex_unlock(&mu);
}
void s2a_gpu_turbo_set_surface(int present) {
    pthread_mutex_lock(&mu); ensure_cv_locked();
    atomic_store(&g_surface, !!present); lifecycle_changed_locked();
    pthread_mutex_unlock(&mu);
}
// 停局：收线程并等它退出。并发的多个调用者都阻塞到线程真的退出为止（只有一个去 join）。
void s2a_gpu_turbo_stop(void) {
    pthread_mutex_lock(&mu); ensure_cv_locked();
    atomic_store(&g_stop, 1);
    pthread_cond_broadcast(&cv);
    if (!started) { pthread_mutex_unlock(&mu); return; }
    if (!joining) {
        joining = 1;
        pthread_mutex_unlock(&mu);
        pthread_join(thr, NULL);
        pthread_mutex_lock(&mu);
        exited = 1;
        pthread_cond_broadcast(&cv);
    } else {
        while (!exited) pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu);
}

#ifdef S2A_GPU_TURBO_TEST
uint64_t s2a_gpu_turbo_test_ticks(void) { return atomic_load(&g_ticks); }
uint64_t s2a_gpu_turbo_test_last_tick_ns(void) { return atomic_load(&g_last_tick_ns); }
// 测试推进了模拟时钟：叫醒在 wait_for 里等的托底线程，不用等真实的等待上限过去
void s2a_gpu_turbo_test_kick(void) { pthread_mutex_lock(&mu); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); }
void s2a_gpu_turbo_test_reset(void) {
    pthread_mutex_lock(&mu);
    started = joining = exited = 0;
    atomic_store(&g_fg, 0); atomic_store(&g_surface, 0); atomic_store(&g_stop, 0);
    atomic_store(&g_kick, 0); atomic_store(&g_frames, 0); atomic_store(&g_ticks, 0);
    atomic_store(&g_last_frame_ns, 0); atomic_store(&g_last_tick_ns, 0);
    for (int i = 0; i < S2A_TURBO_NBUCKET; i++) atomic_store(&g_hist[i], 0);
    pthread_mutex_unlock(&mu);
}
#endif
