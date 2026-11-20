// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// CPU 频率托底的运行体（判定在 cpu_floor_policy.h，宿主测试能在模拟时间线上单独验证）。
//
// 一条线程，生命周期内只起一次：
//   · 不托底时每 250 ms 醒一次，读开关 / 热状态 / 帧计数 / 目标簇频率与各簇上限，交给判定表；其余时间睡在条件变量上。
//   · 托底时在选定的那组核上「满载」（缺省 WFE 空转，调度器当它是满载线程、核时钟却门控；可选 ALU 满载）：
//     nice 19（游戏线程与它同核时让给游戏线程），每片约 1 ms，片间看停止标志、门与判定周期，
//     所以松开 / 停局的延迟是毫秒级。
// 不做的事：不改游戏线程的优先级与亲和，不碰 guest 语义；读不到核容量就不钉核（照样托底），
// 钉核失败只记日志；读不到频率 / 上限就让对应的判据不参与（不拦，日志里记一笔）。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // sched_setaffinity / CPU_SET（同 capacity_affinity.cpp）
#endif
#include "cpu_floor.h"
#include "cpu_floor_policy.h"
#include "adpf_hint.h"
#include "power_state.h"
#include "system_tuning.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef S2A_CPU_FLOOR_TEST
// 宿主测试只替换边界：调试入口的门、系统属性、sysfs 文件内容、时钟、线程 / 核拓扑系统调用与「满载一片」；
// 路径拼写、解析、判定、线程与生命周期都是生产代码。
extern int s2a_cpu_floor_test_gate(void);                      // 1 = 调试入口两道门都开
extern const char *s2a_cpu_floor_test_prop(const char *name);   // NULL = 没设
extern int s2a_cpu_floor_test_read(const char *path, char *buf, size_t n);   // sysfs 文件内容（NUL 结尾，返回字节数）；<= 0 = 读不到
extern uint64_t s2a_cpu_floor_test_now(void);
extern int s2a_cpu_floor_test_ncpu(void);
extern unsigned s2a_cpu_floor_test_capacity(int cpu);          // 0 = 读不到
extern int s2a_cpu_floor_test_pin(uint64_t mask);              // 0 = 成功，否则 errno
extern int s2a_cpu_floor_test_nice(int nice_value);            // 0 = 成功，否则 errno
extern void s2a_cpu_floor_test_spin(int kind);                 // 一片满载（kind: 0 alu / 1 wfe）
extern int s2a_cpu_floor_test_evtstrm(void);                   // 1 = 有事件流（WFE 可用）
extern uint64_t s2a_cpu_floor_test_cpu_ns(int thread);         // 进程（0）/ 托底线程自己（1）的累计 CPU 时间
extern void s2a_cpu_floor_test_log(const char *text);
extern int s2a_cpu_floor_test_gpu_turbo(int on);             // GPU 锁频设定：0 = 成功，否则 errno
#define FLOOR_PROP_MAX 92
static int gate_open(void) { return s2a_cpu_floor_test_gate(); }
static int raw_prop_get(const char *name, char *out) {
    const char *v = s2a_cpu_floor_test_prop(name);
    if (!v) { out[0] = 0; return 0; }
    snprintf(out, FLOOR_PROP_MAX, "%s", v);
    return (int)strlen(out);
}
static int sysfs_read(const char *path, char *buf, size_t n) { return s2a_cpu_floor_test_read(path, buf, n); }
static uint64_t now_ns(void) { return s2a_cpu_floor_test_now(); }
static void floor_log(const char *text) { s2a_cpu_floor_test_log(text); }
#define FLOOR_WAIT_CAP_NS 1000000ull   // 测试里真实等待最多 1 ms：模拟时钟由测试推进
#else
#include <android/log.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include "debug_gate.h"
#define FLOOR_PROP_MAX PROP_VALUE_MAX
static int gate_open(void) { return s2a_debug_entries_open(); }
static int raw_prop_get(const char *name, char *out) { return __system_property_get(name, out); }
static int sysfs_read(const char *path, char *buf, size_t n) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    const ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    return (int)r;
}
static uint64_t now_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void floor_log(const char *text) { __android_log_print(ANDROID_LOG_INFO, "S2Arm", "[s2a-cpufloor] %s", text); }
#define FLOOR_WAIT_CAP_NS UINT64_MAX
#endif

// 调试属性（debug.s2a.cpufloor / debug.s2a.cpufloor_spin）只在调试入口两道门都开时才读（调试构建 + debug.s2a.debug_env=1，
// 见 debug_gate.h）：玩家包恒为缺省（自适应 + WFE），不新开一个发布包上也认的属性。门关着 = 当作没设。
static int prop_get(const char *name, char *out) {
    out[0] = 0;
    if (!gate_open()) return 0;
    return raw_prop_get(name, out);
}

// 没设属性时的缺省模式与缺省满载方式。
// 缺省 WFE：真机 A/B 里 WFE 与 ALU 的帧率、提交耗时、频率读数一致（见 PR）；WFE 让核时钟门控、不占满流水线，
// 预期功耗不高于 ALU（插电设备上量不出功耗，未实测，见 PR 的限制）；机器没有事件流（见 wfe_usable）时自动退回 ALU。
#ifndef S2A_CPU_FLOOR_DEFAULT_MODE
#define S2A_CPU_FLOOR_DEFAULT_MODE S2A_FLOOR_AUTO
#endif
enum { SPIN_ALU = 0, SPIN_WFE = 1 };
#ifndef S2A_CPU_FLOOR_DEFAULT_SPIN
#define S2A_CPU_FLOOR_DEFAULT_SPIN SPIN_WFE
#endif
#define FLOOR_NICE 19

static int read_mode(void) {
    char v[FLOOR_PROP_MAX] = {0};
    if (prop_get("debug.s2a.cpufloor", v) <= 0) return S2A_CPU_FLOOR_DEFAULT_MODE;
    if (!strcmp(v, "0") || !strcmp(v, "off")) return S2A_FLOOR_OFF;
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "always")) return S2A_FLOOR_ALWAYS;
    if (!strcmp(v, "auto")) return S2A_FLOOR_AUTO;
    return S2A_CPU_FLOOR_DEFAULT_MODE;   // 认不出的值：按缺省走，不拦
}

// WFE 满载：核时钟门控、功耗低，调度器仍当它是满载线程；要有「事件流」（HWCAP_EVTSTRM，约 10 kHz 自动唤醒），
// 否则 WFE 可能睡到下一次中断，片长失控——没有就退回 ALU。
static int wfe_usable(void) {
#if defined(S2A_CPU_FLOOR_TEST)
    return s2a_cpu_floor_test_evtstrm();
#elif defined(__aarch64__)
    return (getauxval(AT_HWCAP) & (1ul << 2)) != 0;
#else
    return 0;
#endif
}
static int read_spin(void) {
    char v[FLOOR_PROP_MAX] = {0};
    int kind = S2A_CPU_FLOOR_DEFAULT_SPIN;
    if (prop_get("debug.s2a.cpufloor_spin", v) > 0) {
        if (!strcmp(v, "wfe")) kind = SPIN_WFE;
        else if (!strcmp(v, "alu")) kind = SPIN_ALU;
    }
    return kind == SPIN_WFE && !wfe_usable() ? SPIN_ALU : kind;
}

#ifndef S2A_CPU_FLOOR_TEST
// 一片 ALU 满载：乘加 + 移位异或的相依链，约 1 ms（2.5 GHz）；结果写回静态量，编译器不能把循环当死代码。
static uint64_t spin_state = 88172645463325252ull;
static void burn_alu(void) {
    uint64_t x = spin_state;
    for (unsigned i = 0; i < 400000u; i++) { x = x * 6364136223846793005ull + 1442695040888963407ull; x ^= x >> 29; }
    spin_state = x;
}
static void burn_slice(int kind) {
#if defined(__aarch64__)
    if (kind == SPIN_WFE) { for (int i = 0; i < 10; i++) __asm__ volatile("wfe" ::: "memory"); return; }
#endif
    (void)kind;
    burn_alu();
}
#else
static void burn_slice(int kind) { s2a_cpu_floor_test_spin(kind); }
#endif

// 各核的 cpu_capacity（容量相同 = 同一档核）。返回核数，读不到 / 核数不合理返回 0；单个核读不到记 0。
static int read_caps(unsigned *cap) {
#ifdef S2A_CPU_FLOOR_TEST
    const int n = s2a_cpu_floor_test_ncpu();
    if (n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; i++) cap[i] = s2a_cpu_floor_test_capacity(i);
    return n;
#else
    const long cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (cpus <= 0 || cpus > 64) return 0;
    const int n = (int)cpus;
    for (int i = 0; i < n; i++) {
        char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
        FILE *f = fopen(path, "r"); unsigned v = 0;
        cap[i] = f && fscanf(f, "%u", &v) == 1 ? v : 0;
        if (f) fclose(f);
    }
    return n;
#endif
}

static void apply_thread_setup(uint64_t mask) {
    char text[160];
    int pin_err = 0, nice_err = 0;
#ifdef S2A_CPU_FLOOR_TEST
    if (mask) pin_err = s2a_cpu_floor_test_pin(mask);
    nice_err = s2a_cpu_floor_test_nice(FLOOR_NICE);
#else
    prctl(PR_SET_NAME, "s2a-cpufloor", 0, 0, 0);
    if (mask) {
        cpu_set_t set; CPU_ZERO(&set);
        for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &set);
        if (sched_setaffinity(0, sizeof set, &set)) pin_err = errno;
    }
    if (setpriority(PRIO_PROCESS, 0, FLOOR_NICE)) nice_err = errno;
#endif
    snprintf(text, sizeof text, "线程就绪 cpus=0x%llx%s nice=%d%s", (unsigned long long)mask,
             !mask ? "（未分组：不钉核）" : pin_err ? "（钉核失败，照常运行）" : "", FLOOR_NICE,
             nice_err ? "（降优先级失败，照常运行）" : "");
    floor_log(text);
    if (pin_err || nice_err) {
        snprintf(text, sizeof text, "pin errno=%d nice errno=%d", pin_err, nice_err);
        floor_log(text);
    }
}

// ---- 目标簇频率与各簇上限（sysfs，应用身份可读；读不到就让对应的判据不参与）----
enum { FQ_UNPROBED = 0, FQ_TIS = 1, FQ_CUR = 2, FQ_NONE = 3 };
#define FQ_MAX_FAILS 8      // 连续读失败这么多次就不再读（不在每个周期去撞一次被拒的 sysfs 节点）
struct fq_src {
    int kind;                                   // FQ_*
    int cpu;                                    // 读目标簇频率用的核
    uint64_t work, time;                        // 累计：time_in_state 直接取文件里的累计（10 ms 单位）；scaling_cur_freq 按 频率 × 间隔 累加（ms）
    uint64_t last_ns;
    int fails;
    int ncaps, cap_cpu[S2A_FLOOR_MAX_CAPS], cap_fails[S2A_FLOOR_MAX_CAPS];
    uint32_t max_khz[S2A_FLOOR_MAX_CAPS];
};

static uint32_t read_khz(int cpu, const char *name) {
    char path[96], buf[32];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, name);
    if (sysfs_read(path, buf, sizeof buf) <= 0) return 0;
    char *end = NULL;
    const unsigned long v = strtoul(buf, &end, 10);
    return end != buf && v > 0 && v < 100000000ul ? (uint32_t)v : 0;
}
static int read_tis(int cpu, uint64_t *work, uint64_t *time) {
    char path[112], buf[4096];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/stats/time_in_state", cpu);
    const int r = sysfs_read(path, buf, sizeof buf);
    return r > 0 && s2a_floor_parse_tis(buf, (size_t)r, work, time) > 0 ? 0 : -1;
}

// 起线程时探一次读法：优先 time_in_state（时间加权，精确），其次 scaling_cur_freq 按周期取样，都不行 = 频率读不到。
static void fq_init(struct fq_src *f, const unsigned *cap, int n, uint64_t mask) {
    memset(f, 0, sizeof *f);
    f->ncaps = s2a_floor_cap_cpus(cap, n, mask, f->cap_cpu, S2A_FLOOR_MAX_CAPS);
    int cand[64], nc = 0;
    if (mask) { for (int i = 0; i < 64 && nc < 64; i++) if (mask >> i & 1) cand[nc++] = i; }
    else cand[nc++] = 0;
    f->kind = FQ_NONE;
    uint64_t w, t;
    for (int i = 0; i < nc && f->kind == FQ_NONE; i++)
        if (read_tis(cand[i], &w, &t) == 0) { f->kind = FQ_TIS; f->cpu = cand[i]; f->work = w; f->time = t; }
    for (int i = 0; i < nc && f->kind == FQ_NONE; i++)
        if (read_khz(cand[i], "scaling_cur_freq")) { f->kind = FQ_CUR; f->cpu = cand[i]; }
}

static void fq_poll(struct fq_src *f, uint64_t now) {
    if (f->kind == FQ_TIS) {
        uint64_t w, t;
        if (read_tis(f->cpu, &w, &t) == 0) { f->work = w; f->time = t; f->fails = 0; }
        else if (++f->fails > FQ_MAX_FAILS) f->kind = FQ_NONE;
    } else if (f->kind == FQ_CUR) {
        const uint32_t khz = read_khz(f->cpu, "scaling_cur_freq");
        if (khz) {
            const uint64_t ms = f->last_ns && now > f->last_ns && now - f->last_ns < 2000000000ull ? (now - f->last_ns) / 1000000ull : 0;
            f->work += (uint64_t)khz * ms; f->time += ms; f->fails = 0;
        } else if (++f->fails > FQ_MAX_FAILS) f->kind = FQ_NONE;
        f->last_ns = now;
    }
    for (int i = 0; i < f->ncaps; i++) {
        f->max_khz[i] = 0;
        if (f->cap_fails[i] > FQ_MAX_FAILS) continue;
        const uint32_t v = read_khz(f->cap_cpu[i], "scaling_max_freq");
        if (v) { f->max_khz[i] = v; f->cap_fails[i] = 0; } else f->cap_fails[i]++;
    }
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static int cv_ready, started, joining, exited, cv_clock;
static pthread_t thr;
static _Atomic int g_fg, g_surface, g_stop, g_kick;
static _Atomic uint64_t g_frames;
static _Atomic int g_active;
static _Atomic uint64_t g_ticks;
#ifdef S2A_CPU_FLOOR_TEST
static _Atomic uint64_t g_last_tick_ns;
#endif
static _Atomic uint64_t g_last_frame_ns;                 // 上一帧的时刻；0 = 没有（起局 / 前后台切换后第一帧不计间隔）
static _Atomic unsigned g_hist[S2A_FLOOR_NBUCKET];       // 帧间隔直方图，托底线程每 5 秒取走

// 每帧一次（present 线程）：帧计数 + 与上一帧的间隔进直方图。零间隔（同一纳秒）不计。
void s2a_android_frame_note(void) {
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    const uint64_t now = now_ns();
    const uint64_t prev = atomic_exchange_explicit(&g_last_frame_ns, now, memory_order_relaxed);
    if (prev && now > prev) atomic_fetch_add_explicit(&g_hist[s2a_floor_bucket(now - prev)], 1, memory_order_relaxed);
}

static const char *mode_name(int m) { return m == S2A_FLOOR_OFF ? "off" : m == S2A_FLOOR_ALWAYS ? "always" : "auto"; }
static const char *why_text(int why) {
    switch (why) {
    case S2A_FLOOR_WHY_GATE: return "门关了";
    case S2A_FLOOR_WHY_MODE: return "开关改了";
    case S2A_FLOOR_WHY_DONE: return "帧率到了目标";
    case S2A_FLOOR_WHY_IDLE: return "没在出图";
    case S2A_FLOOR_WHY_CAP_DROP: return "系统在压上限";
    case S2A_FLOOR_WHY_NO_GAIN: return "没有收益";
    case S2A_FLOOR_WHY_ADPF: return "ADPF 已接受帧耗时";
    default: return "-";
    }
}

// 睡到 ns 之后，或被生命周期变化 / 停局叫醒。
static void wait_for(uint64_t ns) {
#ifdef S2A_CPU_FLOOR_TEST
    if (ns > FLOOR_WAIT_CAP_NS) ns = FLOOR_WAIT_CAP_NS;
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
#ifdef S2A_CPU_FLOOR_TEST
    return s2a_cpu_floor_test_cpu_ns(id == CLOCK_THREAD_CPUTIME_ID);
#else
    struct timespec t;
    return clock_gettime(id, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

static uint64_t drain_hist(unsigned *h) {
    uint64_t total = 0;
    for (int i = 0; i < S2A_FLOOR_NBUCKET; i++) { h[i] = atomic_exchange_explicit(&g_hist[i], 0, memory_order_relaxed); total += h[i]; }
    return total;
}

static void bucket_label(int b, char *out, size_t n) {
    if (b < 0) snprintf(out, n, "-");
    else if (b >= S2A_FLOOR_NBUCKET - 1) snprintf(out, n, ">%u", s2a_floor_bucket_limit_ms(S2A_FLOOR_NBUCKET - 2));
    else snprintf(out, n, "≤%u", s2a_floor_bucket_limit_ms(b));
}

// 一个 5 秒读数窗口的汇总
struct stat_win {
    uint64_t span_ns, proc_cpu_ns, thread_cpu_ns;
    const unsigned *hist;
    int active, latched, backoff;               // backoff：没有收益之后的暂缓期内
    int fq_kind, fq_cpu;
    uint32_t avg_khz;                           // 窗口内目标簇的均频（0 = 没读到）
    int ncaps; uint32_t max_khz[S2A_FLOOR_MAX_CAPS];
};

// 每 5 秒一行读数：帧间隔直方 + p50 / p99 所在桶 + 慢帧数 + 这 5 秒的进程 CPU 与托底线程自己的 CPU + 目标簇均频与各簇上限。
// 这 5 秒一帧都没出就不打（暂停 / 加载）。
static void log_window(const struct stat_win *s) {
    char p50[8], p99[8], text[560];
    bucket_label(s2a_floor_hist_quantile(s->hist, 500), p50, sizeof p50);
    bucket_label(s2a_floor_hist_quantile(s->hist, 990), p99, sizeof p99);
    size_t o = (size_t)snprintf(text, sizeof text, "帧间隔 %.1f s：", (double)s->span_ns / 1e9);
    for (int i = 0; i < S2A_FLOOR_NBUCKET && o < sizeof text; i++) {
        char lab[8]; bucket_label(i, lab, sizeof lab);
        o += (size_t)snprintf(text + o, sizeof text - o, "%s:%u ", lab, s->hist[i]);
    }
    if (o < sizeof text)
        o += (size_t)snprintf(text + o, sizeof text - o, "| p50 %s p99 %s ms | 慢帧 %llu | 进程 CPU %llu ms 托底线程 %llu ms | 托底=%s", p50, p99,
                              (unsigned long long)s2a_floor_hist_slow(s->hist), (unsigned long long)(s->proc_cpu_ns / 1000000ull),
                              (unsigned long long)(s->thread_cpu_ns / 1000000ull), s->active ? "开" : s->latched ? "停用" : s->backoff ? "暂缓" : "关");
    if (o < sizeof text) {
        if (s->avg_khz && s->max_khz[0])
            o += (size_t)snprintf(text + o, sizeof text - o, " | 目标簇 cpu%d 均频 %u 上限 %u MHz（%u%%）", s->fq_cpu, s->avg_khz / 1000, s->max_khz[0] / 1000,
                                  (unsigned)((uint64_t)s->avg_khz * 100 / s->max_khz[0]));
        else if (s->max_khz[0])
            o += (size_t)snprintf(text + o, sizeof text - o, " | 目标簇上限 %u MHz，均频读不到", s->max_khz[0] / 1000);
        else
            o += (size_t)snprintf(text + o, sizeof text - o, " | 频率 / 上限读不到");
    }
    if (o < sizeof text && s->ncaps > 1) {
        o += (size_t)snprintf(text + o, sizeof text - o, " | 各簇上限");
        for (int i = 0; i < s->ncaps && o < sizeof text; i++)
            o += (size_t)snprintf(text + o, sizeof text - o, "%c%u", i ? '/' : ' ', s->max_khz[i] / 1000);
        if (o < sizeof text) snprintf(text + o, sizeof text - o, " MHz");
    }
    floor_log(text);
}

// ---- GPU 锁频（同一张判定表的第二个实例）----
// 手机 GPU 的调频器同样按利用率定频：一帧做完就空闲的游戏，GPU 频率被压到「刚好做完这一帧」，于是跑不到 60 帧时也上不去
//   （外部参考 nx2android perf_android.c DM_GPU_TURBO，MIT：Adreno 830 上调频器给 222–443 MHz，关卡里 300 帧掉 31–115 帧，
//   最高频下掉 3–5 帧）。做法：KGSL 设备属性 PWRCTRL 置 0 = 关掉功耗控制，GPU 跑最高频（热限仍生效）；置 1 = 交还调频器。
// 判定与 CPU 托底同一张表（cpu_floor_policy.h），输入换成 GPU：频率 = /sys/class/kgsl/kgsl-3d0/gpuclk 逐拍采样的均值，
//   上限 = max_gpuclk（热 / 功耗限频时它会降，表里「上限在掉」自检照用）。帧率慢 3 个窗口且 GPU 均频没贴着上限才开；
//   保持、翻倍、没有收益暂缓、上限被压本局不再出手、前后台 / 显示面 / 停局即松，全部沿用。
// 属性是设备级、进程退出也不会自己复原：退后台 / 丢显示面由判定表的门松开，停局在线程退出前交还。
// 没有 KGSL（Mali、Xclipse）或 ioctl 被拒：记一行、本局不再试。调试属性 debug.s2a.gputurbo（0 / 1 / auto），门同 cpufloor。
#ifdef S2A_CPU_FLOOR_TEST
static int gpu_turbo_set(int on) { return s2a_cpu_floor_test_gpu_turbo(on); }
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
    char v[FLOOR_PROP_MAX] = {0};
    if (prop_get("debug.s2a.gputurbo", v) <= 0) return S2A_FLOOR_AUTO;
    if (!strcmp(v, "0") || !strcmp(v, "off")) return S2A_FLOOR_OFF;
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "always")) return S2A_FLOOR_ALWAYS;
    return S2A_FLOOR_AUTO;
}
struct gq_src { uint64_t work, time, last_ns; uint32_t cur_khz, max_khz; };
static uint32_t sysfs_hz_khz(const char *path) {
    char b[32] = {0};
    if (sysfs_read(path, b, sizeof b) <= 0) return 0;
    unsigned long long hz = strtoull(b, NULL, 10);
    return hz >= 1000 && hz / 1000 <= UINT32_MAX ? (uint32_t)(hz / 1000) : 0;
}
static void gq_poll(struct gq_src *g, uint64_t now) {
    uint32_t cur = sysfs_hz_khz("/sys/class/kgsl/kgsl-3d0/gpuclk"), max = sysfs_hz_khz("/sys/class/kgsl/kgsl-3d0/max_gpuclk");
    if (cur) {
        if (g->cur_khz && g->last_ns && now > g->last_ns) { uint64_t dt = (now - g->last_ns) / 1000; g->work += (uint64_t)g->cur_khz * dt; g->time += dt; }
        g->cur_khz = cur; g->last_ns = now;
    }
    if (max) g->max_khz = max;
}

static void *floor_main(void *arg) {
    (void)arg;
    unsigned cap[64] = {0};
    const int ncap = read_caps(cap);
    const uint64_t mask = ncap > 0 ? s2a_floor_pick_cpus(cap, ncap) : 0;
    apply_thread_setup(mask);
    struct fq_src fq; fq_init(&fq, cap, ncap, mask);
    {
        char text[160];
        snprintf(text, sizeof text, "频率读法 %s cpu%d 簇数 %d", fq.kind == FQ_TIS ? "time_in_state" : fq.kind == FQ_CUR ? "scaling_cur_freq" : "读不到（auto 只按帧率判定，靠「没有收益」自检兜底）",
                 fq.cpu, fq.ncaps);
        floor_log(text);
    }
    struct s2a_floor_state st; s2a_floor_reset_all(&st);
    struct s2a_floor_state gst; s2a_floor_reset_all(&gst);   // GPU 锁频那一份判定状态
    struct gq_src gq; memset(&gq, 0, sizeof gq);
    int gactive = 0, gdead = 0;
    int active = 0, kind = S2A_CPU_FLOOR_DEFAULT_SPIN, kick = atomic_load(&g_kick);
    uint64_t next_tick = 0, active_since = 0, block_log_ns = 0;
    uint64_t next_stat = 0, stat_ns = 0, stat_proc = 0, stat_thread = 0, stat_fw = 0, stat_ft = 0;   // 5 秒读数窗口：起点、起点处的进程 / 线程 CPU 与频率累计
    while (!atomic_load(&g_stop)) {
        uint64_t now = now_ns();
        int k = atomic_load_explicit(&g_kick, memory_order_relaxed);
        if (k != kick) { kick = k; next_tick = 0; }   // 前台 / 显示面变了：马上重判，不等下一个周期
        if (now >= next_tick) {
            fq_poll(&fq, now);
            struct s2a_floor_in in = {
                .now_ns = now,
                .frames = atomic_load_explicit(&g_frames, memory_order_relaxed),
                .period_ns = s2a_android_present_ns(),
                .mode = read_mode(),
                .foreground = atomic_load(&g_fg), .surface = atomic_load(&g_surface), .stopped = atomic_load(&g_stop),
                .thermal = dm_thermal_now(), .low_power = dm_low_power_now(),
                .fq_work = fq.kind == FQ_TIS || fq.kind == FQ_CUR ? fq.work : 0,
                .fq_time = fq.kind == FQ_TIS || fq.kind == FQ_CUR ? fq.time : 0,
                .ncaps = fq.ncaps,
            };
            memcpy(in.max_khz, fq.max_khz, sizeof in.max_khz);
            const int was = active;
            active = s2a_floor_cpu_step(&st, &in, s2a_adpf_session_active());
            {   // GPU 锁频：同一份帧率 / 生命周期输入，频率与上限换成 GPU
                gq_poll(&gq, now);
                struct s2a_floor_in gin = in;
                gin.mode = gdead ? S2A_FLOOR_OFF : read_gpu_mode();
                gin.fq_work = gq.work; gin.fq_time = gq.time;
                memset(gin.max_khz, 0, sizeof gin.max_khz);
                gin.ncaps = gq.max_khz ? 1 : 0; gin.max_khz[0] = gq.max_khz;
                const int gw = s2a_floor_step(&gst, &gin);
                if (gw != gactive) {
                    const int e = gpu_turbo_set(gw);
                    char text[240];
                    if (e && gw) { gdead = 1; snprintf(text, sizeof text, "GPU 锁频设不上（errno %d：没有 KGSL 或被拒），本局不再试", e); }
                    else if (e) snprintf(text, sizeof text, "GPU 锁频交还失败（errno %d）", e);
                    else if (gw) snprintf(text, sizeof text, "GPU 锁频开 mode=%s GPU 近 %d s 均频 / 上限 = %s%d%%%s", mode_name(gin.mode), S2A_FLOOR_ENTER_WINDOWS,
                                          gst.ceil_permille >= 0 ? "" : "（读不到）", gst.ceil_permille >= 0 ? gst.ceil_permille / 10 : 0, "");
                    else snprintf(text, sizeof text, "GPU 锁频关 原因=%s%s", why_text(gst.why), gst.latched ? "，本局不再出手" : "");
                    floor_log(text);
                    if (!e) gactive = gw; else if (!gw) gactive = 0;
                }
            }
            next_tick = now + S2A_FLOOR_TICK_NS;
            if (active != was) {
                char text[360];
                if (active) {
                    kind = read_spin(); active_since = now;
                    size_t o = (size_t)snprintf(text, sizeof text, "托底开 mode=%s spin=%s hold=%llus", mode_name(in.mode), kind == SPIN_WFE ? "wfe" : "alu",
                                                (unsigned long long)(st.hold_ns / 1000000000ull));
                    if (in.mode == S2A_FLOOR_AUTO) {
                        if (st.ceil_permille >= 0) o += (size_t)snprintf(text + o, sizeof text - o, " 目标簇近 %d s 均频 / 上限 = %d%%", S2A_FLOOR_ENTER_WINDOWS, st.ceil_permille / 10);
                        else o += (size_t)snprintf(text + o, sizeof text - o, " 频率 / 上限读不到，只按帧率判定");
                        // 「慢」是相对游戏登记的上屏周期说的：把这个目标写进日志，读日志的人不用猜
                        if (in.period_ns && o < sizeof text) snprintf(text + o, sizeof text - o, "，上屏目标 %.1f fps", 1e9 / (double)in.period_ns);
                    }
                } else {
                    size_t o = (size_t)snprintf(text, sizeof text, "托底关 mode=%s 原因=%s fg=%d surface=%d thermal=%d low_power=%d，本次 %llus", mode_name(in.mode),
                                                why_text(st.why), in.foreground, in.surface, in.thermal, in.low_power,
                                                (unsigned long long)((now - active_since) / 1000000000ull));
                    if (st.why == S2A_FLOOR_WHY_CAP_DROP)
                        o += (size_t)snprintf(text + o, sizeof text - o, "（簇 %d 上限 %u→%u MHz）", st.cap_idx, st.cap_from / 1000, st.cap_to / 1000);
                    else if (st.why == S2A_FLOOR_WHY_NO_GAIN)
                        o += (size_t)snprintf(text + o, sizeof text - o, "（帧率 %.1f→%.1f，要求至少高 %d%%）", (double)st.pre_mfps / 1000.0, (double)st.post_mfps / 1000.0,
                                              S2A_FLOOR_BENEFIT_MIN_PCT);
                    if (st.latched) snprintf(text + o, sizeof text - o, "，本局不再出手");
                    else if (st.why == S2A_FLOOR_WHY_NO_GAIN)
                        snprintf(text + o, sizeof text - o, "，暂缓 %llu 秒再试", (unsigned long long)(st.nogain_backoff_ns / 1000000000ull));
                }
                floor_log(text);
                atomic_store(&g_active, active);
            } else if (!active && st.block == S2A_FLOOR_BLOCK_CEILING && (!block_log_ns || now - block_log_ns >= 30000000000ull)) {
                char text[200];
                snprintf(text, sizeof text, "auto 暂不出手：目标簇近 %d s 均频 / 上限 = %d%%，已顶在上限（≥ %d%%），没有东西可托", S2A_FLOOR_ENTER_WINDOWS,
                         st.ceil_permille / 10, S2A_FLOOR_CEIL_PERMILLE / 10);
                floor_log(text);
                block_log_ns = now;
            } else if (!active && st.block == S2A_FLOOR_BLOCK_BACKOFF && (!block_log_ns || now - block_log_ns >= 30000000000ull)) {
                char text[200];
                snprintf(text, sizeof text, "auto 暂不出手：上一次托底没有收益，暂缓中（还剩 %llu 秒）",
                         (unsigned long long)((st.nogain_until_ns > now ? st.nogain_until_ns - now : 0) / 1000000000ull));
                floor_log(text);
                block_log_ns = now;
            } else if (st.block == S2A_FLOOR_BLOCK_ADPF && (!block_log_ns || now - block_log_ns >= 30000000000ull)) {
                floor_log("CPU 托底暂停，ADPF 已接受帧耗时");
                block_log_ns = now;
            }
            if (!next_stat) {   // 第一次：开读数窗口，丢掉之前攒下的间隔
                unsigned h0[S2A_FLOOR_NBUCKET]; drain_hist(h0);
                stat_ns = now; next_stat = now + S2A_FLOOR_STAT_NS;
                stat_proc = cpu_time_ns(CLOCK_PROCESS_CPUTIME_ID); stat_thread = cpu_time_ns(CLOCK_THREAD_CPUTIME_ID);
                stat_fw = in.fq_work; stat_ft = in.fq_time;
            } else if (now >= next_stat) {
                unsigned h[S2A_FLOOR_NBUCKET];
                const uint64_t total = drain_hist(h);
                const uint64_t pc = cpu_time_ns(CLOCK_PROCESS_CPUTIME_ID), tc = cpu_time_ns(CLOCK_THREAD_CPUTIME_ID);
                if (total) {
                    struct stat_win sw = { .span_ns = now - stat_ns, .proc_cpu_ns = pc - stat_proc, .thread_cpu_ns = tc - stat_thread, .hist = h,
                                           .active = active, .latched = st.latched, .backoff = !active && st.nogain_until_ns > now,
                                           .fq_kind = fq.kind, .fq_cpu = fq.cpu, .ncaps = fq.ncaps };
                    if (in.fq_time > stat_ft && in.fq_work >= stat_fw) sw.avg_khz = (uint32_t)((in.fq_work - stat_fw) / (in.fq_time - stat_ft));
                    memcpy(sw.max_khz, fq.max_khz, sizeof sw.max_khz);
                    log_window(&sw);
                }
                stat_ns = now; next_stat = now + S2A_FLOOR_STAT_NS; stat_proc = pc; stat_thread = tc; stat_fw = in.fq_work; stat_ft = in.fq_time;
            }
#ifdef S2A_CPU_FLOOR_TEST
            atomic_store(&g_last_tick_ns, now);
#endif
            atomic_fetch_add_explicit(&g_ticks, 1, memory_order_release);   // 一个周期的全部事做完才计数（测试靠它同步）
        }
        if (active && !s2a_adpf_session_active()) { burn_slice(kind); continue; }
        uint64_t t = now_ns();
        wait_for(next_tick > t ? next_tick - t : 0);
    }
    if (gactive) (void)gpu_turbo_set(0);   // 停局：设备级属性在进程外也留着，退出前交还调频器
    atomic_store(&g_active, 0);
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
        if (pthread_create(&thr, NULL, floor_main, NULL) == 0) started = 1;
        else floor_log("线程起不来：本局不托底");
    }
    pthread_cond_broadcast(&cv);
}

void s2a_cpu_floor_set_foreground(int foreground) {
    pthread_mutex_lock(&mu); ensure_cv_locked();
    atomic_store(&g_fg, !!foreground); lifecycle_changed_locked();
    pthread_mutex_unlock(&mu);
}
void s2a_cpu_floor_set_surface(int present) {
    pthread_mutex_lock(&mu); ensure_cv_locked();
    atomic_store(&g_surface, !!present); lifecycle_changed_locked();
    pthread_mutex_unlock(&mu);
}
// 停局：收线程并等它退出。并发的多个调用者都阻塞到线程真的退出为止（只有一个去 join）。
void s2a_cpu_floor_stop(void) {
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

#ifdef S2A_CPU_FLOOR_TEST
uint64_t s2a_cpu_floor_test_ticks(void) { return atomic_load(&g_ticks); }
uint64_t s2a_cpu_floor_test_last_tick_ns(void) { return atomic_load(&g_last_tick_ns); }
// 测试推进了模拟时钟：叫醒在 wait_for 里等的托底线程，不用等真实的等待上限过去
void s2a_cpu_floor_test_kick(void) { pthread_mutex_lock(&mu); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); }
int s2a_cpu_floor_test_active(void) { return atomic_load(&g_active); }
void s2a_cpu_floor_test_reset(void) {
    pthread_mutex_lock(&mu);
    started = joining = exited = 0;
    atomic_store(&g_fg, 0); atomic_store(&g_surface, 0); atomic_store(&g_stop, 0);
    atomic_store(&g_kick, 0); atomic_store(&g_frames, 0); atomic_store(&g_active, 0); atomic_store(&g_ticks, 0);
    atomic_store(&g_last_frame_ns, 0); atomic_store(&g_last_tick_ns, 0);
    for (int i = 0; i < S2A_FLOOR_NBUCKET; i++) atomic_store(&g_hist[i], 0);
    pthread_mutex_unlock(&mu);
}
#endif
