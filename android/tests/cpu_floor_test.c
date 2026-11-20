// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// CPU 频率托底运行体的宿主测试：生产的 cpu_floor.c / power_state.c / system_tuning.c 原样链入，
// 只替换边界——系统属性、单调时钟、核拓扑与钉核 / 降优先级系统调用、以及「满载一片」本身（计数代替真烧核）。
// 时间用模拟时钟推进，线程是真线程：验证起停、门、帧计数接线、判定接线、停局回收。
#include "cpu_floor.h"
#include "adpf_hint.h"
#include "power_state.h"
#include "system_tuning.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failed++; } } while (0)
#define SEC 1000000000ull
#define MS 1000000ull
#define PERIOD30 33333333ull

// ---- 假世界 ----
static _Atomic uint64_t fake_now;
static _Atomic int adpf_active;
int s2a_adpf_session_active(void) { return atomic_load(&adpf_active); }
static _Atomic int gate_on;                                  // 调试入口两道门（调试构建 + debug.s2a.debug_env=1）
static _Atomic(const char *) prop_mode, prop_spin;
static _Atomic int ncpu;
static unsigned caps[64];
static _Atomic int pin_calls, nice_calls, pin_ret, nice_ret, last_nice;
static _Atomic uint64_t last_pin_mask;
static _Atomic long spin_slices;
static _Atomic int last_spin_kind;
static _Atomic int evtstrm;                                  // 1 = 有事件流（HWCAP_EVTSTRM），WFE 可用
static _Atomic uint64_t fake_proc_cpu_ms, fake_thr_cpu_ms;   // 累计 CPU 时间（ms），测试按需设
static _Atomic int prop_reads;                               // 生产代码真正去读系统属性的次数（门关着应为 0）
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char log_buf[65536];

// 假 sysfs（路径由生产代码拼，这里只认「/sys/devices/system/cpu/cpu<N>/cpufreq/<名字>」这一种拼法）：
//   scaling_max_freq：按核给（0 = 读不到）；scaling_cur_freq：当前档位的频率；
//   stats/time_in_state：每个档位一行「频率 累计时间（10 ms 单位）」，当前档位随模拟时钟累计。
static pthread_mutex_t sys_mu = PTHREAD_MUTEX_INITIALIZER;
static const uint32_t opps[] = {384000, 800000, 1500000, 2000000, 2230000, 2490000};
#define NOPP ((int)(sizeof opps / sizeof opps[0]))
static uint64_t opp_ticks[NOPP], int_last_ns, int_carry_ns;
static int cur_opp;                                          // 当前档位下标；-1 = 不累计
static uint32_t cap_khz[64];
static _Atomic int tis_readable, cur_readable;
static _Atomic int reads_tis, reads_cur, reads_cap;
static _Atomic int reads_bad_path;                           // 路径不是「cpu<N>/cpufreq/<认得的名字>」的读取次数：整个测试进程累计，应为 0
static _Atomic uint64_t cap_read_mask;
// 假 KGSL：GPU 当前频率 / 上限（kHz，0 = 读不到；节点里按 Hz 写），锁频设定的调用次数、当前状态与返回值（非 0 = 设不上）
static _Atomic uint32_t gpu_cur_khz, gpu_max_khz;
static _Atomic int turbo_state, turbo_calls, turbo_ret;
static _Atomic(const char *) prop_gpu;                       // 读过 scaling_max_freq 的核（按位）
static void integrate_locked(void) {                         // 把「上次累计以来」的模拟时间记到当前档位上
    const uint64_t now = atomic_load(&fake_now);
    if (int_last_ns && now > int_last_ns) {
        const uint64_t dt = now - int_last_ns + int_carry_ns;
        if (cur_opp >= 0) opp_ticks[cur_opp] += dt / (10 * MS);
        int_carry_ns = dt % (10 * MS);
    }
    int_last_ns = now;
}
static void set_avg(uint32_t khz) {                          // 目标簇从此刻起停在这个频率（必须是 opps 里的一档）
    pthread_mutex_lock(&sys_mu); integrate_locked();
    cur_opp = -1;
    for (int i = 0; i < NOPP; i++) if (opps[i] == khz) cur_opp = i;
    pthread_mutex_unlock(&sys_mu);
}
static void set_caps_khz(uint32_t c0_5, uint32_t c6_7) {
    pthread_mutex_lock(&sys_mu);
    for (int i = 0; i < 64; i++) cap_khz[i] = i < 6 ? c0_5 : i < 8 ? c6_7 : 0;
    pthread_mutex_unlock(&sys_mu);
}
int s2a_cpu_floor_test_gate(void) { return atomic_load(&gate_on); }
int s2a_cpu_floor_test_read(const char *path, char *buf, size_t n) {
    if (!strcmp(path, "/sys/class/kgsl/kgsl-3d0/gpuclk") || !strcmp(path, "/sys/class/kgsl/kgsl-3d0/max_gpuclk")) {
        const uint32_t k = strstr(path, "max_") ? atomic_load(&gpu_max_khz) : atomic_load(&gpu_cur_khz);
        return k ? snprintf(buf, n, "%llu\n", (unsigned long long)k * 1000ull) : -1;
    }
    int cpu = -1, used = 0;
    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/cpufreq/%n", &cpu, &used) != 1 || used <= 0 || cpu < 0 || cpu >= 64) {
        atomic_fetch_add(&reads_bad_path, 1); return -1;
    }
    const char *name = path + used;
    int r = -1;
    pthread_mutex_lock(&sys_mu);
    if (!strcmp(name, "scaling_max_freq")) {
        atomic_fetch_add(&reads_cap, 1); atomic_fetch_or(&cap_read_mask, 1ull << cpu);
        if (cap_khz[cpu]) r = snprintf(buf, n, "%u\n", cap_khz[cpu]);
    } else if (!strcmp(name, "scaling_cur_freq")) {
        atomic_fetch_add(&reads_cur, 1);
        if (atomic_load(&cur_readable) && cur_opp >= 0) r = snprintf(buf, n, "%u\n", opps[cur_opp]);
    } else if (!strcmp(name, "stats/time_in_state")) {
        atomic_fetch_add(&reads_tis, 1);
        if (atomic_load(&tis_readable)) {
            integrate_locked();
            size_t o = 0;
            for (int i = 0; i < NOPP && o < n; i++) o += (size_t)snprintf(buf + o, n - o, "%u %llu\n", opps[i], (unsigned long long)opp_ticks[i]);
            r = (int)o;
        }
    } else atomic_fetch_add(&reads_bad_path, 1);
    pthread_mutex_unlock(&sys_mu);
    return r;
}

const char *s2a_cpu_floor_test_prop(const char *name) {
    atomic_fetch_add(&prop_reads, 1);
    if (!strcmp(name, "debug.s2a.cpufloor")) return atomic_load(&prop_mode);
    if (!strcmp(name, "debug.s2a.cpufloor_spin")) return atomic_load(&prop_spin);
    if (!strcmp(name, "debug.s2a.gputurbo")) return atomic_load(&prop_gpu);
    return NULL;
}
uint64_t s2a_cpu_floor_test_now(void) { return atomic_load(&fake_now); }
int s2a_cpu_floor_test_gpu_turbo(int on) {
    atomic_fetch_add(&turbo_calls, 1);
    const int r = atomic_load(&turbo_ret);
    if (!r) atomic_store(&turbo_state, on);
    return r;
}
int s2a_cpu_floor_test_ncpu(void) { return atomic_load(&ncpu); }
unsigned s2a_cpu_floor_test_capacity(int cpu) { return cpu >= 0 && cpu < 64 ? caps[cpu] : 0; }
int s2a_cpu_floor_test_pin(uint64_t mask) { atomic_fetch_add(&pin_calls, 1); atomic_store(&last_pin_mask, mask); return atomic_load(&pin_ret); }
int s2a_cpu_floor_test_nice(int nice_value) { atomic_fetch_add(&nice_calls, 1); atomic_store(&last_nice, nice_value); return atomic_load(&nice_ret); }
void s2a_cpu_floor_test_spin(int kind) {
    atomic_fetch_add(&spin_slices, 1); atomic_store(&last_spin_kind, kind);
    struct timespec ts = {0, 200000};   // 200 µs：跑单测的宿主上不真烧核
    nanosleep(&ts, NULL);
}
int s2a_cpu_floor_test_evtstrm(void) { return atomic_load(&evtstrm); }
uint64_t s2a_cpu_floor_test_cpu_ns(int thread) { return (thread ? atomic_load(&fake_thr_cpu_ms) : atomic_load(&fake_proc_cpu_ms)) * MS; }
void s2a_cpu_floor_test_log(const char *text) {
    pthread_mutex_lock(&log_mu);
    size_t n = strlen(log_buf);
    if (n + strlen(text) + 2 < sizeof log_buf) { memcpy(log_buf + n, text, strlen(text)); log_buf[n + strlen(text)] = '\n'; log_buf[n + strlen(text) + 1] = 0; }
    pthread_mutex_unlock(&log_mu);
}
static int log_has(const char *needle) {
    pthread_mutex_lock(&log_mu); int r = strstr(log_buf, needle) != NULL; pthread_mutex_unlock(&log_mu); return r;
}
static int log_count(const char *needle) {
    pthread_mutex_lock(&log_mu);
    int n = 0; for (const char *p = log_buf; (p = strstr(p, needle)); p += strlen(needle)) n++;
    pthread_mutex_unlock(&log_mu);
    return n;
}

static void sleep_us(long us) { struct timespec ts = {0, us * 1000}; nanosleep(&ts, NULL); }
// 在真实时间里等条件成立（上限 3 s）：线程是真线程，状态变化要它自己跑到
#define WAIT_UNTIL(cond) ({ int ok_ = 0; for (int i_ = 0; i_ < 6000; i_++) { if (cond) { ok_ = 1; break; } sleep_us(500); } ok_; })

static void world_reset(void) {
    atomic_store(&fake_now, 1000 * SEC);
    atomic_store(&adpf_active, 0);
    atomic_store(&gate_on, 1); atomic_store(&prop_reads, 0);        // 缺省：调试入口开着（调试构建 + debug_env=1），属性生效
    atomic_store(&prop_mode, NULL); atomic_store(&prop_spin, NULL);
    atomic_store(&prop_gpu, "0");                                    // 原有用例只管 CPU 托底：GPU 锁频关着，各自的用例里再开
    atomic_store(&gpu_cur_khz, 0); atomic_store(&gpu_max_khz, 0);
    atomic_store(&turbo_state, 0); atomic_store(&turbo_calls, 0); atomic_store(&turbo_ret, 0);
    pthread_mutex_lock(&sys_mu);                                    // 缺省的假 sysfs：目标簇停在 2230 MHz / 上限 2496 MHz（89%，同真机），三个节点都读得到
    memset(opp_ticks, 0, sizeof opp_ticks); int_last_ns = 0; int_carry_ns = 0; cur_opp = 4;
    for (int i = 0; i < 64; i++) cap_khz[i] = i < 6 ? 2496000 : i < 8 ? 2227200 : 0;
    pthread_mutex_unlock(&sys_mu);
    atomic_store(&tis_readable, 1); atomic_store(&cur_readable, 1);
    atomic_store(&reads_tis, 0); atomic_store(&reads_cur, 0); atomic_store(&reads_cap, 0); atomic_store(&cap_read_mask, 0);
    atomic_store(&ncpu, 8);
    for (int i = 0; i < 8; i++) caps[i] = i < 6 ? 762 : 1024;   // Adreno 840 平板：6 + 2
    atomic_store(&pin_calls, 0); atomic_store(&nice_calls, 0); atomic_store(&pin_ret, 0); atomic_store(&nice_ret, 0);
    atomic_store(&last_nice, -1); atomic_store(&last_pin_mask, 0);
    atomic_store(&spin_slices, 0); atomic_store(&last_spin_kind, -1); atomic_store(&evtstrm, 1);
    atomic_store(&fake_proc_cpu_ms, 0); atomic_store(&fake_thr_cpu_ms, 0);
    pthread_mutex_lock(&log_mu); log_buf[0] = 0; pthread_mutex_unlock(&log_mu);
    s2a_android_power_set(0, 0);                 // 热状态正常、省电关
    s2a_android_present_note(PERIOD30);
    s2a_cpu_floor_test_reset();
}
// 模拟时钟前进 ns，并等线程在新时刻上处理完一个判定周期：用「最近一个已完成周期读到的时钟 ≥ 新时刻」同步，
// 不会被前台 / 显示面变化触发的、读到的还是旧时钟的那个周期提前放行。
// 推进模拟时钟并等托底线程把这一步走完：叫醒它、让出 CPU 紧等，不按固定间隔睡（上千步各睡一次，负载高时整套被拖过 ctest 上限）。
//   上限同 WAIT_UNTIL：3 秒（真实时间）。
static void advance(uint64_t ns) {
    const uint64_t target = atomic_fetch_add(&fake_now, ns) + ns;
    s2a_cpu_floor_test_kick();
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = 0;
    for (unsigned i = 0;; i++) {
        if (s2a_cpu_floor_test_last_tick_ns() >= target) { ok = 1; break; }
        if ((i & 255) == 255) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            if ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) > 3000000000L) break;
            s2a_cpu_floor_test_kick();   // 叫醒时它若正忙（没在等），再叫一次
        }
        sched_yield();
    }
    CHECK(ok);
}
// 按 fps 出帧并走一个判定周期（250 ms）
static double frac;
static void tick_fps(double fps) {
    frac += fps * 0.25;
    int whole = (int)frac; frac -= whole;
    for (int i = 0; i < whole; i++) s2a_android_frame_note();
    advance(250 * MS);
}
static void run_fps(double fps, int ticks) { while (ticks-- > 0) tick_fps(fps); }
static int quiet(void) {   // 满载片数在 20 ms 内不再增加
    long a = atomic_load(&spin_slices); sleep_us(20000); return atomic_load(&spin_slices) == a;
}
static void start_game(void) { s2a_cpu_floor_set_foreground(1); s2a_cpu_floor_set_surface(1); }

static void test_start_gates(void) {
    world_reset(); atomic_store(&prop_mode, "1");
    s2a_android_frame_note();                       // 线程起来之前的帧计数不能出事
    s2a_cpu_floor_set_surface(1);
    sleep_us(30000);
    CHECK(s2a_cpu_floor_test_ticks() == 0 && !s2a_cpu_floor_test_active() && atomic_load(&pin_calls) == 0);   // 只有显示面、没有前台：线程都不起
    s2a_cpu_floor_set_surface(0);
    s2a_cpu_floor_set_foreground(1);
    sleep_us(30000);
    CHECK(s2a_cpu_floor_test_ticks() == 0 && !s2a_cpu_floor_test_active() && atomic_load(&pin_calls) == 0);   // 只有前台、没有显示面：线程都不起
    s2a_cpu_floor_set_surface(1);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(WAIT_UNTIL(atomic_load(&spin_slices) > 3));
    CHECK(atomic_load(&pin_calls) == 1 && atomic_load(&last_pin_mask) == 0x3Full);   // 钉在容量最大的那组（0–5）
    CHECK(atomic_load(&nice_calls) == 1 && atomic_load(&last_nice) == 19);
    CHECK(log_has("托底开 mode=always"));
    // 退显示面：毫秒级松开，满载片数不再增加
    s2a_cpu_floor_set_surface(0);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    s2a_cpu_floor_set_surface(1);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    // 退后台同理
    s2a_cpu_floor_set_foreground(0);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    s2a_cpu_floor_set_foreground(1);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(atomic_load(&pin_calls) == 1 && atomic_load(&nice_calls) == 1);   // 线程一直是同一条：只设置一次
    s2a_cpu_floor_stop();
    CHECK(!s2a_cpu_floor_test_active()); CHECK(quiet());
    // 停局之后：再来任何生命周期事件都不复活
    uint64_t t = s2a_cpu_floor_test_ticks();
    s2a_cpu_floor_set_surface(1); s2a_cpu_floor_set_foreground(1);
    atomic_fetch_add(&fake_now, 5 * SEC); sleep_us(30000);
    CHECK(s2a_cpu_floor_test_ticks() == t && !s2a_cpu_floor_test_active() && quiet());
    s2a_cpu_floor_stop();                           // 重复停：无副作用
}

static void test_stop_before_start(void) {
    world_reset();
    s2a_cpu_floor_stop();                           // 一次都没起过：直接停
    s2a_cpu_floor_set_foreground(1); s2a_cpu_floor_set_surface(1);
    sleep_us(30000);
    CHECK(s2a_cpu_floor_test_ticks() == 0);         // 停了就是停了
    CHECK(atomic_load(&pin_calls) == 0 && atomic_load(&nice_calls) == 0);   // 线程连起都不该起（起了会先做钉核 / 降优先级）
}

static void test_property_modes(void) {
    // 属性 0：线程在、判定在，但从不托底
    world_reset(); atomic_store(&prop_mode, "0"); start_game();
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(5.0, 80);
    CHECK(!s2a_cpu_floor_test_active() && atomic_load(&spin_slices) == 0);
    // 运行中改成 1：下一个判定周期就托；改回 0：下一个周期就松
    atomic_store(&prop_mode, "1"); advance(250 * MS);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    atomic_store(&prop_mode, "0"); advance(250 * MS);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    s2a_cpu_floor_stop();
    // 认不出的值：按缺省（自适应）走，不拦：帧率足够就不托，慢才托
    world_reset(); atomic_store(&prop_mode, "garbage"); start_game();
    run_fps(30.0, 80); CHECK(!s2a_cpu_floor_test_active());
    run_fps(15.0, 14); CHECK(s2a_cpu_floor_test_active());
    s2a_cpu_floor_stop();
}

// 关一次再开一次托底并返回新一次托底实际用的满载方式：每次托底开启时才重读满载方式属性
static int spin_kind_after_restart(void) {
    atomic_store(&prop_mode, "0"); advance(250 * MS);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active()));
    atomic_store(&prop_mode, "1"); advance(250 * MS);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    const long a = atomic_load(&spin_slices);
    CHECK(WAIT_UNTIL(atomic_load(&spin_slices) > a + 2));
    return atomic_load(&last_spin_kind);
}

static void test_spin_kind(void) {
    world_reset(); atomic_store(&prop_mode, "1"); start_game();
    CHECK(WAIT_UNTIL(atomic_load(&spin_slices) > 2)); CHECK(atomic_load(&last_spin_kind) == 1);    // 缺省 WFE
    CHECK(log_has("托底开 mode=always spin=wfe"));
    atomic_store(&prop_spin, "alu");   CHECK(spin_kind_after_restart() == 0);                      // 属性 alu
    CHECK(log_has("托底开 mode=always spin=alu"));
    atomic_store(&prop_spin, "wfe");   CHECK(spin_kind_after_restart() == 1);                      // 属性 wfe
    atomic_store(&prop_spin, "bogus"); CHECK(spin_kind_after_restart() == 1);                      // 认不出：缺省（WFE），不拦
    // 没有事件流（HWCAP_EVTSTRM 缺）：WFE 可能睡到下一次中断、片长失控 → 缺省与显式 wfe 都退回 ALU
    atomic_store(&evtstrm, 0);
    atomic_store(&prop_spin, NULL);    CHECK(spin_kind_after_restart() == 0);
    atomic_store(&prop_spin, "wfe");   CHECK(spin_kind_after_restart() == 0);
    atomic_store(&prop_spin, "alu");   CHECK(spin_kind_after_restart() == 0);
    atomic_store(&evtstrm, 1);                                                                     // 事件流回来：下一次托底又按属性 / 缺省走
    atomic_store(&prop_spin, NULL);    CHECK(spin_kind_after_restart() == 1);
    s2a_cpu_floor_stop();
}

static void test_power_gates(void) {
    // 热状态 / 省电模式不再关托底（11-13 维护者「不读电量、不设自己挡自己的闸」）：只进日志
    world_reset(); atomic_store(&prop_mode, "1"); start_game();
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    s2a_android_power_set(3, 0);                    // SEVERE：照旧托
    { unsigned long long t = s2a_cpu_floor_test_ticks(); advance(250 * MS); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() > t)); CHECK(s2a_cpu_floor_test_active()); }
    s2a_android_power_set(5, 0);                    // EMERGENCY：照旧托
    { unsigned long long t = s2a_cpu_floor_test_ticks(); advance(250 * MS); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() > t)); CHECK(s2a_cpu_floor_test_active()); }
    s2a_android_power_set(0, 1);                    // 省电模式：照旧托
    { unsigned long long t = s2a_cpu_floor_test_ticks(); advance(250 * MS); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() > t)); CHECK(s2a_cpu_floor_test_active()); }
    s2a_cpu_floor_stop(); s2a_android_power_set(0, 0);
}

static void test_auto(void) {
    // 负对照：按目标 30 帧出图，4 分钟（模拟时钟）一次都不托
    world_reset(); start_game();
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(30.0, 4 * 240);
    CHECK(!s2a_cpu_floor_test_active() && atomic_load(&spin_slices) == 0 && atomic_load(&pin_calls) == 1);
    CHECK(!log_has("auto 暂不出手") && !log_has("托底开") && !log_has("托底关"));
    s2a_cpu_floor_stop();
    // 正例：17 帧对 30 帧目标，第 13 个周期托底；带着托底帧率回到 30，保持满 60 s 后松开
    world_reset(); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 11); CHECK(!s2a_cpu_floor_test_active());
    run_fps(17.0, 3); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(WAIT_UNTIL(atomic_load(&spin_slices) > 3));
    CHECK(log_has("托底开 mode=auto spin=wfe hold=60s"));
    run_fps(30.0, 200); CHECK(s2a_cpu_floor_test_active());          // 50 s：仍在保持期
    run_fps(30.0, 60);                                               // 过 60 s
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    CHECK(log_has("托底关 mode=auto 原因=帧率到了目标 fg=1 surface=1 thermal=0 low_power=0，本次 60s"));   // 第 13 个周期托底、第 253 个周期到期松开
    CHECK(!log_has("本局不再出手"));
    s2a_cpu_floor_stop();
    // 托底中一帧都不出了（暂停 / 卡死）：连续 5 个空窗口后松开，不是「帧率到了」
    world_reset(); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    run_fps(0.0, 40);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    CHECK(log_has("托底关 mode=auto 原因=没在出图"));
    // 目标未登记（周期 0）：没有判据，慢也不托
    s2a_cpu_floor_stop();
    world_reset(); s2a_android_present_note(0); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(5.0, 80); CHECK(!s2a_cpu_floor_test_active());
    s2a_cpu_floor_stop();
    // 目标周期不是 30 帧档（50 ms = 20 fps）：慢的线是目标的 9/10 = 18 fps，19 fps 不慢、17 fps 慢；日志写出这个目标
    world_reset(); s2a_android_present_note(50 * MS); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(19.0, 80); CHECK(!s2a_cpu_floor_test_active());
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(log_has("，上屏目标 20.0 fps"));
    s2a_cpu_floor_stop();
}

static void test_topology(void) {
    // 钉核失败：照常托底，只记日志
    world_reset(); atomic_store(&prop_mode, "1"); atomic_store(&pin_ret, EPERM); atomic_store(&nice_ret, EACCES); start_game();
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active())); CHECK(log_has("钉核失败")); CHECK(log_has("降优先级失败"));
    s2a_cpu_floor_stop();
    // 同质核 / 读不到容量：不钉核，照常托底
    world_reset(); atomic_store(&prop_mode, "1");
    for (int i = 0; i < 8; i++) caps[i] = 1024;
    start_game(); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active())); CHECK(atomic_load(&pin_calls) == 0); CHECK(log_has("未分组"));
    CHECK(WAIT_UNTIL(atomic_load(&reads_cap) >= 1)); CHECK(atomic_load(&cap_read_mask) == 1ull);   // 不分组：只有一个「簇」，读核 0 的上限
    CHECK(log_has("频率读法 time_in_state cpu0 簇数 1"));
    s2a_cpu_floor_stop();
    world_reset(); atomic_store(&prop_mode, "1"); caps[3] = 0;
    start_game(); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active())); CHECK(atomic_load(&pin_calls) == 0);
    s2a_cpu_floor_stop();
}

// 模拟时钟前进 ms 毫秒后出一帧（线程只在自己的判定周期里读时钟，这里不等它）
static void frame_after(uint64_t ms) { atomic_fetch_add(&fake_now, ms * MS); s2a_android_frame_note(); }

static void test_frame_stats(void) {
    // 线程起来之前攒下的间隔：第一个判定周期开读数窗口时丢掉，不进第一行
    world_reset(); atomic_store(&prop_mode, "0");
    s2a_android_frame_note(); frame_after(40);                       // 一个 40 ms 的间隔（桶 ≤42）
    start_game();
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_last_tick_ns() > 0));        // 第一个周期（开读数窗口、丢旧间隔）整个做完
    // 第一个读数窗口：10 个 60 ms（≤67）、5 个 30 ms（≤34）、1 个 150 ms（>100）；
    // 起局后的第一帧没有上一帧，不计间隔；同一纳秒的重复帧（零间隔）不计
    s2a_android_frame_note();                                        // 起局后第一帧
    s2a_android_frame_note();                                        // 同一纳秒：不计
    for (int i = 0; i < 10; i++) frame_after(60);
    for (int i = 0; i < 5; i++) frame_after(30);
    frame_after(150);
    atomic_store(&fake_proc_cpu_ms, 200); atomic_store(&fake_thr_cpu_ms, 12);
    advance(5 * SEC);
    CHECK(log_has("帧间隔 5.9 s："));                                // 窗口起点是第一个周期的时刻（1000.04 s）：到 1005.94 s 收窗
    CHECK(log_has("≤17:0 ≤25:0 ≤34:5 ≤42:0 ≤51:0 ≤67:10 ≤100:0 >100:1"));
    CHECK(log_has("p50 ≤67 p99 >100 ms"));
    CHECK(log_has("慢帧 11"));
    CHECK(log_has("进程 CPU 200 ms 托底线程 12 ms"));
    CHECK(log_has("托底=关"));
    CHECK(log_count("帧间隔 ") == 1);
    // 一帧未出的 5 秒不打
    advance(5 * SEC); advance(5 * SEC); advance(250 * MS);
    CHECK(log_count("帧间隔 ") == 1);
    // 切前后台 / 显示面之后的第一帧不计间隔（隔着一段没在出图的时间），之后的间隔照计；
    // 没出帧的那一大段本身不进直方图（上一帧到这一帧之间隔了 1 秒也没有记成 >100）
    s2a_cpu_floor_set_foreground(0); atomic_fetch_add(&fake_now, 1 * SEC); s2a_cpu_floor_set_foreground(1);
    frame_after(33);                                                 // 切换后的第一帧：不计
    frame_after(33);                                                 // ≤34
    s2a_cpu_floor_set_surface(0); atomic_fetch_add(&fake_now, 1 * SEC); s2a_cpu_floor_set_surface(1);
    frame_after(33);                                                 // 不计
    frame_after(33);                                                 // ≤34
    atomic_store(&fake_proc_cpu_ms, 500); atomic_store(&fake_thr_cpu_ms, 15);
    advance(5 * SEC);
    CHECK(log_count("帧间隔 ") == 2);
    CHECK(log_has("帧间隔 7.4 s："));                                // 上一个窗口（含两个空窗口）收在 1015.94 s，到 1023.32 s 才有帧的窗口收窗
    CHECK(log_has("≤17:0 ≤25:0 ≤34:2 ≤42:0 ≤51:0 ≤67:0 ≤100:0 >100:0"));
    CHECK(log_has("进程 CPU 300 ms 托底线程 3 ms"));                 // 每个窗口都从上一次取样点起算（含两个空窗口的取样）
    // 托底开着时读数照打，并标出状态
    atomic_store(&prop_mode, "1"); advance(250 * MS);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    s2a_cpu_floor_set_surface(1);                                    // 重置间隔链：上一帧离现在已隔了 5 秒
    for (int i = 0; i < 11; i++) frame_after(20);                    // 第一帧不计，其后 10 个 20 ms
    atomic_store(&fake_proc_cpu_ms, 530); atomic_store(&fake_thr_cpu_ms, 20);
    advance(5 * SEC);
    CHECK(log_count("帧间隔 ") == 3);
    CHECK(log_has("帧间隔 5.5 s："));
    CHECK(log_has("≤17:0 ≤25:10 ≤34:0") && log_has("托底=开"));
    CHECK(log_has("进程 CPU 30 ms 托底线程 5 ms"));
    s2a_cpu_floor_stop();
}

// ---- 调试入口的门 ----
static void test_gate(void) {
    // 门关着（玩家包 / 没开 debug.s2a.debug_env）：两个属性不读、不认，缺省自适应 + WFE
    world_reset(); atomic_store(&gate_on, 0);
    atomic_store(&prop_mode, "1"); atomic_store(&prop_spin, "alu");
    start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(30.0, 80);                                              // 帧率到了：属性 1（常开）被无视，不托
    CHECK(!s2a_cpu_floor_test_active() && atomic_load(&spin_slices) == 0);
    run_fps(15.0, 14);                                              // 慢了：自适应托
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(WAIT_UNTIL(atomic_load(&spin_slices) > 3));
    CHECK(atomic_load(&last_spin_kind) == 1);                       // 属性 alu 被无视：缺省 WFE
    CHECK(log_has("托底开 mode=auto spin=wfe"));
    atomic_store(&prop_mode, "0"); run_fps(15.0, 20);               // 属性 0 也关不掉
    CHECK(s2a_cpu_floor_test_active());
    CHECK(atomic_load(&prop_reads) == 0);                           // 门关着：连系统属性都没去读
    // 门打开：同一个属性下一个周期就生效
    atomic_store(&gate_on, 1); advance(250 * MS);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    CHECK(log_has("托底关 mode=off 原因=开关改了"));
    CHECK(atomic_load(&prop_reads) > 0);
    // 门再关上：回到缺省自适应，慢了照样托
    atomic_store(&gate_on, 0); run_fps(15.0, 13);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    s2a_cpu_floor_stop();
}

// ---- 已顶在上限就不出手 ----
static void test_ceiling(void) {
    // 目标簇平均频率已顶在上限（2490 / 2496 MHz）：帧率再慢也不托，只记一行
    world_reset(); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 80);                                              // 第 1 个周期开窗口，之后每 4 个周期收一个窗口：第 81 个周期正好收窗
    CHECK(!s2a_cpu_floor_test_active() && atomic_load(&spin_slices) == 0 && atomic_load(&pin_calls) == 1);
    CHECK(log_has("auto 暂不出手：目标簇近 3 s 均频 / 上限 = 99%"));
    CHECK(log_count("auto 暂不出手") == 1);                         // 30 s 内不重复（80 个周期 = 20 s）
    run_fps(17.0, 120);                                             // 第 201 个周期（50 s）：每 30 s 记一行 → 第 13 / 133 个周期各一行
    CHECK(!s2a_cpu_floor_test_active() && log_count("auto 暂不出手") == 2);
    // 均频掉到上限的 89%（上限没变）：窗口边界（第 201 个周期）上改，之后每个窗口都是纯 89%；
    // 近 3 个窗口 99% / 89% / 89% → 92%，已低于 95%，第 209 个周期托底
    set_avg(2230000); run_fps(17.0, 7); CHECK(!s2a_cpu_floor_test_active());
    run_fps(17.0, 1);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(log_has("托底开 mode=auto spin=wfe hold=60s 目标簇近 3 s 均频 / 上限 = 92%，上屏目标 30.0 fps"));
    s2a_cpu_floor_stop();
    // 常开是显式指令：不看上限
    world_reset(); set_avg(2490000); atomic_store(&prop_mode, "1"); start_game();
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(log_has("托底开 mode=always") && !log_has("均频 / 上限") && !log_has("上屏目标"));
    s2a_cpu_floor_stop();
}

// ---- 频率读法与读不到 ----
static void test_freq_sources(void) {
    // time_in_state 读不到、scaling_cur_freq 可读：按周期取样，判据与 time_in_state 一致
    world_reset(); atomic_store(&tis_readable, 0); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    CHECK(log_has("频率读法 scaling_cur_freq cpu0 簇数 2"));
    run_fps(17.0, 40);
    CHECK(!s2a_cpu_floor_test_active() && log_has("均频 / 上限 = 99%"));
    s2a_cpu_floor_stop();
    world_reset(); atomic_store(&tis_readable, 0); start_game(); frac = 0;      // 缺省 2230 / 2496
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(log_has("托底开 mode=auto spin=wfe hold=60s 目标簇近 3 s 均频 / 上限 = 89%，上屏目标 30.0 fps"));
    s2a_cpu_floor_stop();

    // 两种读法都不行：判据不参与，只按帧率判定（不拦），日志写明；之后不再去撞这些节点
    world_reset(); atomic_store(&tis_readable, 0); atomic_store(&cur_readable, 0); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    CHECK(log_has("频率读法 读不到"));
    const int t0 = atomic_load(&reads_tis), c0 = atomic_load(&reads_cur);
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(log_has("托底开 mode=auto spin=wfe hold=60s 频率 / 上限读不到，只按帧率判定，上屏目标 30.0 fps"));
    run_fps(30.0, 24);
    CHECK(log_has("目标簇上限 2496 MHz，均频读不到"));
    CHECK(atomic_load(&reads_tis) == t0 && atomic_load(&reads_cur) == c0);
    s2a_cpu_floor_stop();

    // 频率读得到、上限读不到：判据不参与；每个簇连续读失败 9 次后不再读
    world_reset(); set_caps_khz(0, 0); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 40);
    CHECK(s2a_cpu_floor_test_active());
    CHECK(log_has("托底开 mode=auto spin=wfe hold=60s 频率 / 上限读不到，只按帧率判定，上屏目标 30.0 fps"));
    CHECK(atomic_load(&reads_cap) == 2 * 9);
    s2a_cpu_floor_stop();

    // 运行中 time_in_state 开始读失败：累计不再前进 → 判据读不到 → 只按帧率判定；连续失败 9 次后停读
    world_reset(); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 20); CHECK(!s2a_cpu_floor_test_active());          // 顶在上限：不托
    atomic_store(&tis_readable, 0);
    const int r0 = atomic_load(&reads_tis);
    run_fps(17.0, 16);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));                  // 近 3 个窗口都没有读数
    CHECK(atomic_load(&reads_tis) == r0 + 9);
    CHECK(log_has("托底开 mode=auto spin=wfe hold=60s 频率 / 上限读不到，只按帧率判定，上屏目标 30.0 fps"));
    s2a_cpu_floor_stop();
}

// 偶发读失败（每次连续不到 9 次）：恢复后失败计数清零，读法不丢，判据照常
static void test_freq_transient(void) {
    world_reset(); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    for (int round = 0; round < 3; round++) {                        // 3 轮 × 5 次失败 = 15 次，但每轮之间都恢复过
        atomic_store(&tis_readable, 0); run_fps(17.0, 5);
        atomic_store(&tis_readable, 1); run_fps(17.0, 5);
    }
    const int r0 = atomic_load(&reads_tis);
    run_fps(17.0, 40);
    CHECK(atomic_load(&reads_tis) == r0 + 40);                       // 还在读
    CHECK(!s2a_cpu_floor_test_active());                             // 判据还在：顶在上限 → 不托
    CHECK(log_has("auto 暂不出手：目标簇近 3 s 均频 / 上限 = 99%"));
    s2a_cpu_floor_stop();
    // 上限节点也一样：偶发读失败不累计
    world_reset(); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    for (int round = 0; round < 3; round++) {
        set_caps_khz(0, 0); run_fps(17.0, 5);
        set_caps_khz(2496000, 2227200); run_fps(17.0, 5);
    }
    const int c0 = atomic_load(&reads_cap);
    run_fps(17.0, 40);
    CHECK(atomic_load(&reads_cap) == c0 + 2 * 40);                   // 两个簇每个周期都还在读
    CHECK(!s2a_cpu_floor_test_active());
    s2a_cpu_floor_stop();
    // 按周期取样的读法（scaling_cur_freq）也一样
    world_reset(); atomic_store(&tis_readable, 0); set_avg(2490000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    for (int round = 0; round < 3; round++) {
        atomic_store(&cur_readable, 0); run_fps(17.0, 5);
        atomic_store(&cur_readable, 1); run_fps(17.0, 5);
    }
    const int u0 = atomic_load(&reads_cur);
    run_fps(17.0, 40);
    CHECK(atomic_load(&reads_cur) == u0 + 40);                       // 还在读
    CHECK(!s2a_cpu_floor_test_active());                             // 判据还在：顶在上限 → 不托
    s2a_cpu_floor_stop();
}

// ---- 自检：上限被压 ----
static void test_cap_drop(void) {
    // 托底开着，系统把目标簇上限压到托底前的 95% 以下：连续 2 个窗口（8 个周期）→ 松开，本局不再出手
    world_reset(); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    CHECK(log_has("托底开 mode=auto spin=wfe hold=60s 目标簇近 3 s 均频 / 上限 = 89%，上屏目标 30.0 fps"));
    set_caps_khz(2300000, 2227200);                                  // 第 13 个周期托底，此刻是第 14 个周期；之后第 17 / 21 个周期各收一个窗口
    run_fps(24.0, 6); CHECK(s2a_cpu_floor_test_active());            // 第 15–20 个周期：只收了 1 个窗口，不松
    run_fps(24.0, 1);                                                // 第 21 个周期：第 2 个窗口，松
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    CHECK(log_has("托底关 mode=auto 原因=系统在压上限"));
    CHECK(log_has("（簇 0 上限 2496→2300 MHz）"));
    CHECK(log_has("，本局不再出手"));
    // 本局不再出手：帧率再慢也不再托；5 秒读数标「停用」
    run_fps(17.0, 400);
    CHECK(!s2a_cpu_floor_test_active() && quiet());
    CHECK(log_count("托底开 ") == 1);
    CHECK(log_has("托底=停用"));
    // 上限回来了也不松口（本局）：门的开关、前后台切换也洗不掉
    set_caps_khz(2496000, 2227200);
    s2a_cpu_floor_set_foreground(0); advance(250 * MS); s2a_cpu_floor_set_foreground(1);
    run_fps(17.0, 100);
    CHECK(!s2a_cpu_floor_test_active() && log_count("托底开 ") == 1);
    s2a_cpu_floor_stop();

    // 另一个簇（核 6）的上限被压也算
    world_reset(); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    set_caps_khz(2496000, 2000000);
    run_fps(24.0, 8);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active()));
    CHECK(log_has("（簇 1 上限 2227→2000 MHz）"));
    s2a_cpu_floor_stop();

    // 常开是显式指令：上限被压到 1 GHz 也不松
    world_reset(); atomic_store(&prop_mode, "1"); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    set_caps_khz(1000000, 1000000);
    run_fps(17.0, 300);
    CHECK(s2a_cpu_floor_test_active() && !log_has("托底关"));
    s2a_cpu_floor_stop();
}

// ---- 自检：托底没有换来帧率 ----
static void test_no_gain(void) {
    // 托底开了、帧率没比托底前高 5%（GPU / 锁 / IO 受限）：5 s 稳定期后量 10 个出图窗口，到第 60 个周期判决 → 松开，暂缓 60 s 再试
    world_reset(); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    // 托底后帧率 17.0 → 17.5（+2.9%），不到 5%：两个数不同，日志里先写托底前、后写托底后
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));   // 第 13 个周期托底，此刻是第 14 个周期；判决在托底后第 60 个周期 = 第 73 个
    run_fps(17.5, 58); CHECK(s2a_cpu_floor_test_active());           // 第 72 个周期：还没到判决
    run_fps(17.5, 1);
    CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active())); CHECK(quiet());
    CHECK(log_has("托底关 mode=auto 原因=没有收益"));
    CHECK(log_has("（帧率 17.0→17.5，要求至少高 5%）"));
    CHECK(log_has("，暂缓 60 秒再试") && !log_has("本局不再出手"));
    // 暂缓期内（60 s = 240 个周期）：帧率再慢也不托，打一行说明；5 秒读数标「暂缓」
    run_fps(17.0, 230);
    CHECK(!s2a_cpu_floor_test_active() && quiet() && log_count("托底开 ") == 1);
    CHECK(log_has("auto 暂不出手：上一次托底没有收益，暂缓中（还剩 "));
    CHECK(log_has("托底=暂缓"));
    // 到点再试：帧率还是慢 → 再托一次
    run_fps(17.0, 20);
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active())); CHECK(log_count("托底开 ") == 2);
    s2a_cpu_floor_stop();

    // 暂缓期过了、帧率已经够：5 秒读数回到「关」，不再写「暂缓」
    world_reset(); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    run_fps(17.5, 59); CHECK(WAIT_UNTIL(!s2a_cpu_floor_test_active()));  // 第 73 个周期松开，暂缓到第 313 个周期
    run_fps(30.0, 160);                                              // 第 233 个周期：还在暂缓期
    CHECK(log_has("托底=暂缓"));
    run_fps(30.0, 100);                                              // 第 333 个周期：暂缓期已过
    {
        const int snoozed = log_count("托底=暂缓");
        run_fps(30.0, 100);
        CHECK(log_count("托底=暂缓") == snoozed && log_has("托底=关"));
    }
    s2a_cpu_floor_stop();

    // 有收益（17 → 24）：判决通过，不松；之后帧率再掉回 17 也不重复判决（续期）
    world_reset(); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 13); CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    run_fps(24.0, 80); CHECK(s2a_cpu_floor_test_active());
    run_fps(17.0, 400); CHECK(s2a_cpu_floor_test_active());
    CHECK(!log_has("没有收益") && !log_has("托底关"));
    s2a_cpu_floor_stop();

    // 常开是显式指令：不做这条自检
    world_reset(); atomic_store(&prop_mode, "1"); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    run_fps(17.0, 300);
    CHECK(s2a_cpu_floor_test_active() && !log_has("没有收益"));
    s2a_cpu_floor_stop();
}

// ---- 5 秒读数行里的频率与上限 ----
static void test_stat_freq(void) {
    // 托底关着也照打（只看读数）：目标簇均频、上限，各簇上限
    world_reset(); atomic_store(&prop_mode, "0"); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(30.0, 19);                                               // 第 20 个周期（4.75 s）：窗口还没满 5 s，不收
    CHECK(log_count("帧间隔 ") == 0);
    run_fps(30.0, 1);                                                // 第 21 个周期收第 1 个 5 秒窗口
    CHECK(log_count("帧间隔 ") == 1);
    CHECK(log_has("频率读法 time_in_state cpu0 簇数 2"));
    CHECK(log_has("目标簇 cpu0 均频 2230 上限 2496 MHz（89%）"));
    CHECK(log_has("各簇上限 2496/2227 MHz"));
    CHECK(atomic_load(&cap_read_mask) == ((1ull << 0) | (1ull << 6)));   // 每簇读一个代表核：目标簇（0–5）读核 0，另一簇读核 6
    CHECK(atomic_load(&reads_bad_path) == 0);
    set_avg(2000000); run_fps(30.0, 20);                             // 窗口边界上改均频；第 41 个周期收第 2 个窗口：只算这一个窗口，不混进上一个
    CHECK(log_has("目标簇 cpu0 均频 2000 上限 2496 MHz（80%）"));
    s2a_cpu_floor_stop();
    // 取样读法同样给出均频
    world_reset(); atomic_store(&prop_mode, "0"); atomic_store(&tis_readable, 0); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(30.0, 24);
    CHECK(log_has("目标簇 cpu0 均频 2230 上限 2496 MHz（89%）"));
    s2a_cpu_floor_stop();
    // 取样读法下，两次取样之间隔了 ≥ 2 s（线程被挂起 / 系统休眠）的那一段不记：否则整段会按取样那一刻的档位记满
    world_reset(); atomic_store(&prop_mode, "0"); atomic_store(&tis_readable, 0); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(30.0, 8);                                                // 2 s 正常取样，都在 2230 MHz
    set_avg(2490000);
    advance(10 * SEC);                                               // 一个周期跳过 10 s，取样那一刻在 2490 MHz；读数窗口在这一周期收
    CHECK(log_has("目标簇 cpu0 均频 2230 上限 2496 MHz（89%）") && !log_has("均频 24"));
    s2a_cpu_floor_stop();
    // 频率读不到 / 上限读不到：照实写
    world_reset(); atomic_store(&prop_mode, "0"); atomic_store(&tis_readable, 0); atomic_store(&cur_readable, 0); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(30.0, 24);
    CHECK(log_has("目标簇上限 2496 MHz，均频读不到"));
    s2a_cpu_floor_stop();
    world_reset(); atomic_store(&prop_mode, "0"); set_caps_khz(0, 0); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(30.0, 24);
    CHECK(log_has("频率 / 上限读不到"));
    s2a_cpu_floor_stop();
}

static void *stop_thread(void *arg) { (void)arg; s2a_cpu_floor_stop(); return NULL; }
static void test_concurrent_stop(void) {
    world_reset(); atomic_store(&prop_mode, "1"); start_game();
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_active()));
    pthread_t a, b;
    pthread_create(&a, NULL, stop_thread, NULL); pthread_create(&b, NULL, stop_thread, NULL);
    s2a_cpu_floor_stop();
    pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(!s2a_cpu_floor_test_active() && quiet());
}

// GPU 锁频（同一张判定表的第二个实例，见 cpu_floor.c gq_poll / gpu_turbo_set）
static void test_gpu_turbo(void) {
    // 常开：起局即锁频；退后台交还、回前台再锁；停局交还（设备级属性不能留在进程外）
    world_reset(); atomic_store(&prop_mode, "0"); atomic_store(&prop_gpu, "1"); start_game();
    CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1)); CHECK(log_has("GPU 锁频开 mode=always"));
    s2a_cpu_floor_set_foreground(0); CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 0));
    s2a_cpu_floor_set_foreground(1); CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1));
    s2a_cpu_floor_stop(); CHECK(atomic_load(&turbo_state) == 0);
    // 自适应：GPU 均频 300 / 上限 900 MHz（33%），17 帧对 30 帧目标 → 与 CPU 托底同一节拍，第 13 个周期锁频；CPU 托底关着不动
    world_reset(); atomic_store(&prop_mode, "0"); atomic_store(&prop_gpu, "auto");
    atomic_store(&gpu_cur_khz, 300000); atomic_store(&gpu_max_khz, 900000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 11); CHECK(atomic_load(&turbo_state) == 0);
    run_fps(17.0, 3); CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1)); CHECK(log_has("GPU 锁频开 mode=auto"));
    CHECK(!s2a_cpu_floor_test_active());
    s2a_cpu_floor_stop(); CHECK(atomic_load(&turbo_state) == 0);
    // 负对照：GPU 已贴着上限（900 / 900）→ 再慢也不锁频（没有东西可拉）
    world_reset(); atomic_store(&prop_mode, "0"); atomic_store(&prop_gpu, "auto");
    atomic_store(&gpu_cur_khz, 900000); atomic_store(&gpu_max_khz, 900000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 40); CHECK(atomic_load(&turbo_calls) == 0);
    s2a_cpu_floor_stop(); CHECK(atomic_load(&turbo_calls) == 0);
    // 设不上（没有 KGSL / 被拒）：记一行、本局不再试
    world_reset(); atomic_store(&prop_mode, "0"); atomic_store(&prop_gpu, "1"); atomic_store(&turbo_ret, 13); start_game();
    CHECK(WAIT_UNTIL(log_has("GPU 锁频设不上")));
    run_fps(30.0, 20); CHECK(atomic_load(&turbo_calls) == 1);
    s2a_cpu_floor_stop(); CHECK(atomic_load(&turbo_calls) == 1);
}

static void test_adpf_gate(void) {
    world_reset();
    atomic_store(&adpf_active, 1);
    atomic_store(&prop_gpu, "auto");
    atomic_store(&gpu_cur_khz, 300000);
    atomic_store(&gpu_max_khz, 900000);
    start_game();
    frac = 0;
    CHECK(WAIT_UNTIL(s2a_cpu_floor_test_ticks() >= 1));
    run_fps(17.0, 16);
    CHECK(!s2a_cpu_floor_test_active() && atomic_load(&spin_slices) == 0);
    CHECK(atomic_load(&turbo_state) == 1);
    atomic_store(&adpf_active, 0);
    run_fps(17.0, 16);
    CHECK(s2a_cpu_floor_test_active());
    atomic_store(&adpf_active, 1);
    run_fps(17.0, 2);
    CHECK(!s2a_cpu_floor_test_active() && quiet());
    CHECK(atomic_load(&turbo_state) == 1);
    atomic_store(&prop_mode, "1");
    run_fps(17.0, 2);
    CHECK(!s2a_cpu_floor_test_active() && quiet());
    s2a_cpu_floor_stop();
    CHECK(atomic_load(&turbo_state) == 0);
}

int main(int argc, char **argv) {
    test_adpf_gate();
    if (argc == 2 && !strcmp(argv[1], "--adpf")) return failed != 0;
    test_start_gates();
    test_stop_before_start();
    test_property_modes();
    test_spin_kind();
    test_power_gates();
    test_auto();
    test_topology();
    test_gate();
    test_ceiling();
    test_freq_sources();
    test_freq_transient();
    test_cap_drop();
    test_no_gain();
    test_stat_freq();
    test_frame_stats();
    test_concurrent_stop();
    test_gpu_turbo();
    CHECK(atomic_load(&reads_bad_path) == 0);        // 只读 cpu<N>/cpufreq/ 下认得的三个节点
    if (failed) { fprintf(stderr, "cpu floor runtime: %d checks failed\n", failed); return 1; }
    printf("cpu floor runtime: passed\n");
    return 0;
}
