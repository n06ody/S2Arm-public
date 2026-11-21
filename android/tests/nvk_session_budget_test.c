// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real platform session close with a controlled outstanding shader resolver.
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "direct/compile_worker.h"
#include "gfx_plat/nvk_session.h"
#include "nvn_backend.h"
#include "nvn_unimpl.h"
#include "rp_exec.h"
static atomic_uint outstanding;
static atomic_int stop_seen, backend_calls;
static atomic_int demand_idle = ATOMIC_VAR_INIT(1), demand_starts, demand_stops, backend_open_calls, shader_open_calls;
static int log_flag = -1, log_attempts;   // log_flag 缺省 -1：control / busy 也按产品缺省（脏页日志开）跑
static char logs[4096]; static size_t log_n;
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec; }
int __android_log_print(int level, const char *tag, const char *fmt, ...) {
    (void)level; (void)tag; va_list ap; va_start(ap, fmt);
    if (log_n < sizeof logs) { int n=vsnprintf(logs+log_n, sizeof logs-log_n, fmt, ap); if (n>0 && (size_t)n < sizeof logs-log_n) log_n+=(size_t)n; }
    va_end(ap); return 0;
}
void nvk_shader_stop(void) {
    atomic_store(&stop_seen, 1);
    (void)s2a_compile_worker_stop_bounded(0, NULL, 0);
}
unsigned nvk_shader_inflight(void) { return atomic_load(&outstanding); }
int nvk_shader_open(void) { atomic_fetch_add(&shader_open_calls, 1); return 0; }
int nvk_warm_stop(void) { return 0; }   // #996 起停局先停预热带与需求建管线（本用例只量会话收尾等待）
void nvk_warm_start(void) {}
void nvn_vk_pipe_demand_start(void) { atomic_fetch_add(&demand_starts, 1); }
void nvn_vk_pipe_demand_stop(void) { atomic_fetch_add(&demand_stops, 1); }
int nvn_vk_pipe_demand_quiescent(void) { return atomic_load(&demand_idle); }
void nvn_dirty_set_chunk_pages(uint32_t n) { (void)n; }
int nvn_dirty_log_configure(int enabled) { if (enabled) log_attempts++; return 0; } // 本夹具不提供真实写跟踪，保守拒绝。
int nvn_plat_flag(const char *name, int dflt) { return !strcmp(name, "nvk_buf_dirtylog") ? (log_flag < 0 ? dflt : log_flag) : dflt; }   // log_flag -1 = 没设，取缺省
double s2a_direct_vk_display_refresh(void) { return 60; }
void nvn_unimpl_reset(void) {}
void nvn_vk_frame_log_dump(void) {}   // nvk_session.c 弱引用：本夹具调用 nvk_session_close，-dead_strip 不再裁掉它，macOS ld64 不收未定义弱引用；本用例不看帧日志
size_t nvn_unimpl_dump(char *b, size_t n) { if (n) *b=0; return 0; }
int nvn_unimpl_kinds(void) { return 0; }
void rp_present_set_surface(nvn_gpu_surface s, int hz) { (void)s; (void)hz; }
void rp_present_stats(uint64_t *s, uint64_t *d) { *s = *d = 0; }
nvn_gpu_rc nvn_gpu_open(const nvn_gpu_open_desc *d, nvn_gpu_err *e) { (void)d; (void)e; atomic_fetch_add(&backend_open_calls,1); return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_caps_get(nvn_gpu_caps *c) { memset(c, 0, sizeof *c); }
nvn_gpu_rc nvn_gpu_close(uint64_t n, nvn_gpu_err *e) { (void)n; atomic_fetch_add(&backend_calls,1); (void)e; return (nvn_gpu_rc){NVN_GPU_OK}; }
static int no_compile(void *ctx, uint64_t session, uint64_t generation, const char *json, size_t len,
                      uint8_t **bytes, size_t *bytes_len, char *why, size_t why_size) {
    (void)ctx; (void)session; (void)generation; (void)json; (void)len;
    (void)bytes; (void)bytes_len; (void)why; (void)why_size;
    return -1;
}

static int retire_idle_pool(void) {
    if (s2a_compile_worker_start_pool(no_compile, NULL, 2, NULL, 0)) return 1;
    nvk_session_request_stop();
    int rc = nvk_session_wait_compiles();
    struct s2a_compile_worker_stats stats;
    s2a_compile_worker_stats(&stats);
    const int failed = rc || stats.slots;
    if (failed)
        fprintf(stderr, "FAIL: session stop must retire idle compile slots before returning (rc=%d slots=%u)\n",
                rc, stats.slots);
    else
        puts("session/idle-pool: retired 2 slots inside Stop budget");
    s2a_compile_worker_stop();
    return failed;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "idle-pool")) return retire_idle_pool();
    if (!strcmp(argv[1], "log-off") || !strcmp(argv[1], "log-default")) {   // 关：不配置；缺省（没设开关）：要配置，夹具拒了就报黄
        log_flag = !strcmp(argv[1], "log-off") ? 0 : -1; const int want = log_flag != 0;
        if (nvk_session_open(NULL, NULL, NULL, 0)) return 1;
        if (log_attempts != want || (want && !strstr(logs, "脏页追加日志未启用"))) {
            fprintf(stderr, "FAIL: dirty-page log configure/diagnostic\n"); return 1;
        }
        printf("session/%s: attempts=%d refusal_logged=%d\n", argv[1], log_attempts, !!strstr(logs, "脏页追加日志未启用"));
        return 0;
    }
    if (!strcmp(argv[1], "demand-close")) {
        char why[128] = "";
        if (nvk_session_prepare() || atomic_load(&shader_open_calls) != 1) return 1;
        if (nvk_session_open(NULL, NULL, why, sizeof why) || atomic_load(&backend_open_calls) != 1 || atomic_load(&demand_starts) != 1) return 1;
        atomic_store(&demand_idle, 0);
        int retained = nvk_session_close();
        if (retained != 1 || atomic_load(&backend_calls) != 0 || !strstr(logs, "保留会话 / 后端资源")) {
            fprintf(stderr, "FAIL: timed-out demand worker must retain session/backend and log it\n"); return 1;
        }
        atomic_store(&demand_idle, 1);
        if (nvk_session_close() || atomic_load(&backend_calls) != 1) {
            fprintf(stderr, "FAIL: close retry after demand quiescence\n"); return 1;
        }
        atomic_store(&demand_idle, 0);
        why[0] = 0;
        if (nvk_session_prepare() != -1 || atomic_load(&shader_open_calls) != 1) {
            fprintf(stderr, "FAIL: prepare must not reset shader state while old demand worker lives\n"); return 1;
        }
        if (nvk_session_open(NULL, NULL, why, sizeof why) != -1 || atomic_load(&backend_open_calls) != 1 || !strstr(why, "后台线程")) {
            fprintf(stderr, "FAIL: open must reject reset while old demand worker lives\n"); return 1;
        }
        atomic_store(&demand_idle, 1);
        if (nvk_session_prepare() || nvk_session_open(NULL, NULL, why, sizeof why) || atomic_load(&backend_open_calls) != 2) {
            fprintf(stderr, "FAIL: prepare/open after demand quiescence\n"); return 1;
        }
        if (nvk_session_close() || atomic_load(&backend_calls) != 2) {
            fprintf(stderr, "FAIL: final close after restart\n"); return 1;
        }
        printf("session/demand-close: retained=1 retry=1 prepare_guard=1 open_guard=1 starts=%d stops=%d backend_closes=%d\n",
               atomic_load(&demand_starts), atomic_load(&demand_stops), atomic_load(&backend_calls));
        return 0;
    }
    if (strcmp(argv[1], "control") && strcmp(argv[1], "busy")) return 2;
    if (!strcmp(argv[1], "busy")) atomic_store(&outstanding, 1);
    nvk_session_request_stop();
    uint64_t t = now_ns(); int rc = nvk_session_wait_compiles(); uint64_t elapsed = now_ns() - t;
    printf("session/%s: rc=%d elapsed=%.1fms backend_calls=%d stop=%d\n", argv[1], rc, (double)elapsed/1e6, atomic_load(&backend_calls), atomic_load(&stop_seen));
    if (atomic_load(&backend_calls) || !atomic_load(&stop_seen)) { fprintf(stderr, "FAIL: shader wait must retain backend\n"); return 1; }
    if (!strcmp(argv[1], "control")) return rc != 0;
    if (!rc) { fprintf(stderr, "FAIL: in-flight resolver must wait until deadline\n"); return 1; }
    if (elapsed > 1100000000ull || !strstr(logs, "shader wait timeout")) { fprintf(stderr, "FAIL: shader wait must report its stage within budget\n"); return 1; }
    return 0;
}
