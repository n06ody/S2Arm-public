// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_vk_compute_phase.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <android/log.h>
#else
/* 宿主弱缝（与本线 bridge.c 同口径）：测试桩提供强符号，无桩时静默不丢。 */
extern int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((weak));
#endif

#define PHASE_PRINT_N 64u    // 每 64 笔完成样本打一行
#define PHASE_CAP 256u       // 窗内样本上限：理论到不了（64 就结算），防呆不防用

static _Atomic int g_gate = -1;
int nvk_compute_phase_on(void) {
    int v = atomic_load(&g_gate);
    if (v < 0) {
#ifdef __ANDROID__
        char b[PROP_VALUE_MAX] = {0};
        v = __system_property_get("debug.s2a.compute_phase", b) > 0 && b[0] == '1';
#else
        const char *e = getenv("S2A_COMPUTE_PHASE");
        v = e && e[0] == '1';
#endif
        atomic_store(&g_gate, v);
    }
    return v;
}
uint64_t nvk_compute_phase_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// 在途单笔：9 段向量 + 起笔钟。armed=0 时 add() 自动起笔（pass_break 缺席的宿主/替身路径）。
static uint64_t g_seg[NVK_PHASE_SEGS], g_t0;
static int g_armed;
// 窗口样本：0..8 = 九段，9 = wall（opcode 墙钟或内部窗宽），10 = unattr（wall-Σseg）。
static uint64_t g_samp[NVK_PHASE_SEGS + 2][PHASE_CAP], g_wsum[NVK_PHASE_SEGS + 2];
static unsigned g_n;
static uint64_t g_cum[NVK_PHASE_SEGS], g_hits[NVK_PHASE_SEGS], g_samples, g_lines;
static const char *const g_name[NVK_PHASE_SEGS + 2] = {
    "prep", "lookup", "parse", "alloc", "desc", "record", "submit", "wb", "dtor", "wall", "unattr"
};

static void emit(void) {
    if (!g_n) return;
#ifndef __ANDROID__
    if (!__android_log_print) { g_n = 0; return; }
#endif
    char line[1024];
    int n = snprintf(line, sizeof line, "compute phase n=%u avg-us/p95-us:", g_n);
    for (int s = 0; s < NVK_PHASE_SEGS + 2 && n > 0 && (size_t)n < sizeof line - 48; s++) {
        uint64_t tmp[PHASE_CAP];
        memcpy(tmp, g_samp[s], g_n * sizeof tmp[0]);
        for (unsigned i = 1; i < g_n; i++) {
            uint64_t v = tmp[i];
            unsigned j = i;
            while (j && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; j--; }
            tmp[j] = v;
        }
        unsigned p95 = (g_n * 95 + 99) / 100;
        if (!p95 || p95 > g_n) p95 = g_n;
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%.1f/%.1f", g_name[s],
                      g_wsum[s] / 1e3 / g_n, tmp[p95 - 1] / 1e3);
    }
    __android_log_print(4 /* ANDROID_LOG_INFO */, "S2Arm", "%s", line);
    g_lines++;
    g_n = 0;
    memset(g_wsum, 0, sizeof g_wsum);
}

static void settle(uint64_t wall_ns) {
    uint64_t sum = 0;
    for (int s = 0; s < NVK_PHASE_SEGS; s++) sum += g_seg[s];
    if (g_n < PHASE_CAP) {
        for (int s = 0; s < NVK_PHASE_SEGS; s++) {
            g_wsum[s] += g_seg[s];
            g_samp[s][g_n] = g_seg[s];
        }
        g_wsum[NVK_PHASE_SEGS] += wall_ns;
        g_samp[NVK_PHASE_SEGS][g_n] = wall_ns;
        const uint64_t unattr = wall_ns > sum ? wall_ns - sum : 0;
        g_wsum[NVK_PHASE_SEGS + 1] += unattr;
        g_samp[NVK_PHASE_SEGS + 1][g_n] = unattr;
        g_n++;
    }
    g_samples++;
    g_armed = 0;
    memset(g_seg, 0, sizeof g_seg);
    if (g_n >= PHASE_PRINT_N) emit();
}

void nvk_compute_phase_begin(void) {
    if (!nvk_compute_phase_on()) return;
    if (g_armed) settle(nvk_compute_phase_now() - g_t0);
    g_armed = 1;
    g_t0 = nvk_compute_phase_now();
}
void nvk_compute_phase_add(int seg, uint64_t ns) {
    if (!nvk_compute_phase_on() || seg < 0 || seg >= NVK_PHASE_SEGS) return;
    if (!g_armed) { g_armed = 1; g_t0 = nvk_compute_phase_now(); }
    g_seg[seg] += ns;
    g_cum[seg] += ns;   // 累计随 add 即走：宿主测试不依赖结算点也能断言分段
    g_hits[seg]++;
}
void nvk_compute_phase_flush(void) {
    if (!nvk_compute_phase_on() || !g_armed) return;
    settle(nvk_compute_phase_now() - g_t0);
}
void nvk_compute_phase_wall(uint64_t op_wall_ns) {
    if (!nvk_compute_phase_on() || !g_armed) return;
    settle(op_wall_ns);
}
void nvk_compute_phase_stats(uint64_t seg_sum[NVK_PHASE_SEGS], uint64_t seg_hits[NVK_PHASE_SEGS],
                             uint64_t *samples, uint64_t *lines) {
    if (seg_sum) memcpy(seg_sum, g_cum, sizeof g_cum);
    if (seg_hits) memcpy(seg_hits, g_hits, sizeof g_hits);
    if (samples) *samples = g_samples;
    if (lines) *lines = g_lines;
}
