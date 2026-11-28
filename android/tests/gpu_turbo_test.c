// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// 真监控线程与 GPU 判定。替换系统属性、时钟和 KGSL 边界。
#include "gpu_turbo.h"
#include "system_tuning.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failed++; } } while (0)
#define SEC 1000000000ull
#define MS 1000000ull
#define PERIOD30 33333333ull

static _Atomic uint64_t fake_now, fake_proc_cpu_ms, fake_thr_cpu_ms;
static _Atomic int gate_on, prop_reads, legacy_prop_reads, unexpected_reads;
static _Atomic uint32_t gpu_cur_khz, gpu_max_khz;
static _Atomic int gpu_cur_error, gpu_max_error, gpu_cur_reads, gpu_max_reads;
static _Atomic int turbo_state, turbo_calls, turbo_ret;
static _Atomic(const char *) prop_gpu, prop_cpu;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char log_buf[65536];

int s2a_gpu_turbo_test_gate(void) { return atomic_load(&gate_on); }
int s2a_gpu_turbo_test_read(const char *path, char *buf, size_t n) {
    uint32_t k = 0;
    int error = 0;
    if (!strcmp(path, "/sys/class/kgsl/kgsl-3d0/gpuclk")) {
        atomic_fetch_add(&gpu_cur_reads, 1);
        error = atomic_load(&gpu_cur_error);
        k = atomic_load(&gpu_cur_khz);
    } else if (!strcmp(path, "/sys/class/kgsl/kgsl-3d0/max_gpuclk")) {
        atomic_fetch_add(&gpu_max_reads, 1);
        error = atomic_load(&gpu_max_error);
        k = atomic_load(&gpu_max_khz);
    } else {
        atomic_fetch_add(&unexpected_reads, 1);
    }
    if (error) return -error;
    return k ? snprintf(buf, n, "%llu\n", (unsigned long long)k * 1000ull) : 0;
}
const char *s2a_gpu_turbo_test_prop(const char *name) {
    atomic_fetch_add(&prop_reads, 1);
    if (!strcmp(name, "debug.s2a.gputurbo")) return atomic_load(&prop_gpu);
    if (!strcmp(name, "debug.s2a.cpufloor") || !strcmp(name, "debug.s2a.cpufloor_spin")) {
        atomic_fetch_add(&legacy_prop_reads, 1);
        return atomic_load(&prop_cpu);
    }
    return NULL;
}
uint64_t s2a_gpu_turbo_test_now(void) { return atomic_load(&fake_now); }
int s2a_gpu_turbo_test_set(int on) {
    atomic_fetch_add(&turbo_calls, 1);
    const int r = atomic_load(&turbo_ret);
    if (!r) atomic_store(&turbo_state, on);
    return r;
}
uint64_t s2a_gpu_turbo_test_cpu_ns(int thread) { return (thread ? atomic_load(&fake_thr_cpu_ms) : atomic_load(&fake_proc_cpu_ms)) * MS; }
void s2a_gpu_turbo_test_log(const char *text) {
    pthread_mutex_lock(&log_mu);
    size_t n = strlen(log_buf);
    if (n + strlen(text) + 2 < sizeof log_buf) { memcpy(log_buf + n, text, strlen(text)); log_buf[n + strlen(text)] = '\n'; log_buf[n + strlen(text) + 1] = 0; }
    pthread_mutex_unlock(&log_mu);
}
static int log_has(const char *needle) {
    pthread_mutex_lock(&log_mu); int r = strstr(log_buf, needle) != NULL; pthread_mutex_unlock(&log_mu); return r;
}
static void sleep_us(long us) { struct timespec ts = {0, us * 1000}; nanosleep(&ts, NULL); }
// 在真实时间里等条件成立（上限 3 s）：线程是真线程，状态变化要它自己跑到
#define WAIT_UNTIL(cond) ({ int ok_ = 0; for (int i_ = 0; i_ < 6000; i_++) { if (cond) { ok_ = 1; break; } sleep_us(500); } ok_; })

static void world_reset(void) {
    atomic_store(&fake_now, 1000 * SEC);
    atomic_store(&gate_on, 1);
    atomic_store(&prop_reads, 0);
    atomic_store(&legacy_prop_reads, 0);
    atomic_store(&prop_gpu, "0");
    atomic_store(&prop_cpu, NULL);
    atomic_store(&gpu_cur_khz, 0);
    atomic_store(&gpu_max_khz, 0);
    atomic_store(&gpu_cur_error, 0);
    atomic_store(&gpu_max_error, 0);
    atomic_store(&gpu_cur_reads, 0);
    atomic_store(&gpu_max_reads, 0);
    atomic_store(&turbo_state, 0);
    atomic_store(&turbo_calls, 0);
    atomic_store(&turbo_ret, 0);
    atomic_store(&fake_proc_cpu_ms, 0);
    atomic_store(&fake_thr_cpu_ms, 0);
    pthread_mutex_lock(&log_mu); log_buf[0] = 0; pthread_mutex_unlock(&log_mu);
    s2a_android_present_note(PERIOD30);
    s2a_gpu_turbo_test_reset();
}
// 等监控线程处理完新的模拟时刻再继续。
static void advance(uint64_t ns) {
    const uint64_t target = atomic_fetch_add(&fake_now, ns) + ns;
    s2a_gpu_turbo_test_kick();
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = 0;
    for (unsigned i = 0;; i++) {
        if (s2a_gpu_turbo_test_last_tick_ns() >= target) { ok = 1; break; }
        if ((i & 255) == 255) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            if ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) > 3000000000L) break;
            s2a_gpu_turbo_test_kick();   // 叫醒时它若正忙（没在等），再叫一次
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
static void start_game(void) { s2a_gpu_turbo_set_foreground(1); s2a_gpu_turbo_set_surface(1); }

static void test_gpu_turbo(void) {
    // 常开：起局即锁频；退后台交还、回前台再锁；停局交还（设备级属性不能留在进程外）
    world_reset(); atomic_store(&prop_gpu, "1"); start_game();
    CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1)); CHECK(log_has("GPU 锁频开 mode=always"));
    s2a_gpu_turbo_set_foreground(0); CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 0));
    s2a_gpu_turbo_set_foreground(1); CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1));
    s2a_gpu_turbo_stop(); CHECK(atomic_load(&turbo_state) == 0);
    // 自适应：GPU 均频 300 / 上限 900 MHz（33%），17 帧对 30 帧目标 → 与 CPU 托底同一节拍，第 13 个周期锁频；CPU 托底关着不动
    world_reset(); atomic_store(&prop_gpu, "auto");
    atomic_store(&gpu_cur_khz, 300000); atomic_store(&gpu_max_khz, 900000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_gpu_turbo_test_ticks() >= 1));
    run_fps(17.0, 11); CHECK(atomic_load(&turbo_state) == 0);
    run_fps(17.0, 3); CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1)); CHECK(log_has("GPU 锁频开 mode=auto"));
    s2a_gpu_turbo_stop(); CHECK(atomic_load(&turbo_state) == 0);
    // 负对照：GPU 已贴着上限（900 / 900）→ 再慢也不锁频（没有东西可拉）
    world_reset(); atomic_store(&prop_gpu, "auto");
    atomic_store(&gpu_cur_khz, 900000); atomic_store(&gpu_max_khz, 900000); start_game(); frac = 0;
    CHECK(WAIT_UNTIL(s2a_gpu_turbo_test_ticks() >= 1));
    run_fps(17.0, 40); CHECK(atomic_load(&turbo_calls) == 0);
    s2a_gpu_turbo_stop(); CHECK(atomic_load(&turbo_calls) == 0);
    // 设不上（没有 KGSL / 被拒）：记一行、本局不再试
    world_reset(); atomic_store(&prop_gpu, "1"); atomic_store(&turbo_ret, 13); start_game();
    CHECK(WAIT_UNTIL(log_has("GPU 锁频设不上")));
    run_fps(30.0, 20); CHECK(atomic_load(&turbo_calls) == 1);
    s2a_gpu_turbo_stop(); CHECK(atomic_load(&turbo_calls) == 1);
}

static void test_no_cpu_floor(void) {
    world_reset();
    atomic_store(&prop_cpu, "1");
    atomic_store(&prop_gpu, "1");
    start_game();
    CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1));
    run_fps(17.0, 16);
    CHECK(atomic_load(&legacy_prop_reads) == 0);
    CHECK(atomic_load(&unexpected_reads) == 0);
    s2a_gpu_turbo_stop();
    CHECK(atomic_load(&turbo_state) == 0);
}

static void test_lifecycle(void) {
    world_reset();
    atomic_store(&prop_gpu, "1");
    s2a_gpu_turbo_set_surface(1);
    CHECK(s2a_gpu_turbo_test_ticks() == 0 && atomic_load(&turbo_calls) == 0);
    s2a_gpu_turbo_set_foreground(1);
    CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1));
    s2a_gpu_turbo_set_surface(0);
    CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 0));
    s2a_gpu_turbo_set_surface(1);
    CHECK(WAIT_UNTIL(atomic_load(&turbo_state) == 1));
    s2a_gpu_turbo_stop();
    const int stopped_calls = atomic_load(&turbo_calls);
    start_game();
    CHECK(stopped_calls == 4 && atomic_load(&turbo_state) == 0);
    CHECK(atomic_load(&turbo_calls) == stopped_calls);
    s2a_gpu_turbo_stop();
}

static void test_debug_gate(void) {
    world_reset();
    atomic_store(&gate_on, 0);
    atomic_store(&prop_gpu, "0");
    atomic_store(&gpu_cur_khz, 300000);
    atomic_store(&gpu_max_khz, 900000);
    start_game();
    CHECK(WAIT_UNTIL(s2a_gpu_turbo_test_ticks() >= 1));
    run_fps(17.0, 16);
    CHECK(atomic_load(&prop_reads) == 0);
    CHECK(atomic_load(&turbo_state) == 1);
    s2a_gpu_turbo_stop();
}

static void test_frame_stats(void) {
    world_reset();
    start_game();
    CHECK(WAIT_UNTIL(s2a_gpu_turbo_test_ticks() >= 1));
    atomic_store(&fake_proc_cpu_ms, 244);
    atomic_store(&fake_thr_cpu_ms, 3);
    run_fps(20.0, 21);
    CHECK(log_has("帧间隔 5.0 s"));
    CHECK(log_has("进程 CPU 244 ms 监控线程 3 ms"));
    CHECK(log_has("GPU 锁频=关"));
    s2a_gpu_turbo_stop();
}

static void test_denied_frequency_nodes(void) {
    world_reset();
    atomic_store(&prop_gpu, "1");
    atomic_store(&gpu_cur_error, EACCES);
    atomic_store(&gpu_max_error, EAGAIN);
    start_game();
    CHECK(WAIT_UNTIL(s2a_gpu_turbo_test_ticks() >= 1));
    CHECK(atomic_load(&turbo_state) == 1);
    atomic_store(&gpu_max_khz, 800000);
    atomic_store(&gpu_max_error, 0);
    run_fps(30.0, 21);
    CHECK(atomic_load(&gpu_cur_reads) == 1);
    CHECK(atomic_load(&gpu_max_reads) > 1 && log_has("GPU 上限 800 MHz，均频读不到"));

    atomic_store(&gpu_max_error, EPERM);
    advance(250 * MS);
    const int cap_reads = atomic_load(&gpu_max_reads);
    advance(250 * MS);
    advance(250 * MS);
    CHECK(atomic_load(&gpu_max_reads) == cap_reads);
    CHECK(atomic_load(&turbo_state) == 1 && atomic_load(&turbo_calls) == 1);
    s2a_gpu_turbo_stop();
    CHECK(atomic_load(&turbo_state) == 0 && atomic_load(&turbo_calls) == 2);
}

int main(void) {
    test_gpu_turbo();
    test_no_cpu_floor();
    test_lifecycle();
    test_debug_gate();
    test_frame_stats();
    test_denied_frequency_nodes();
    CHECK(atomic_load(&unexpected_reads) == 0);
    if (failed) { fprintf(stderr, "gpu turbo runtime: %d checks failed\n", failed); return 1; }
    printf("gpu turbo runtime: passed (%d assertions)\n", checks);
    return 0;
}
