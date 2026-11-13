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
#include "gfx_plat/nvk_session.h"
#include "nvn_backend.h"
#include "nvn_unimpl.h"
#include "rp_exec.h"
static atomic_uint outstanding;
static atomic_int stop_seen, backend_calls;
static char logs[4096]; static size_t log_n;
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec; }
int __android_log_print(int level, const char *tag, const char *fmt, ...) {
    (void)level; (void)tag; va_list ap; va_start(ap, fmt);
    if (log_n < sizeof logs) { int n=vsnprintf(logs+log_n, sizeof logs-log_n, fmt, ap); if (n>0 && (size_t)n < sizeof logs-log_n) log_n+=(size_t)n; }
    va_end(ap); return 0;
}
void nvk_shader_stop(void) { atomic_store(&stop_seen, 1); }
unsigned nvk_shader_inflight(void) { return atomic_load(&outstanding); }
int nvk_shader_open(void) { return 0; }
void nvn_dirty_set_chunk_pages(uint32_t n) { (void)n; }
int nvn_plat_flag(const char *name, int dflt) { (void)name; return dflt; }
double s2a_direct_vk_display_refresh(void) { return 60; }
void nvn_unimpl_reset(void) {}
size_t nvn_unimpl_dump(char *b, size_t n) { if (n) *b=0; return 0; }
int nvn_unimpl_kinds(void) { return 0; }
void rp_present_set_surface(nvn_gpu_surface s, int hz) { (void)s; (void)hz; }
void rp_present_stats(uint64_t *s, uint64_t *d) { *s = *d = 0; }
nvn_gpu_rc nvn_gpu_open(const nvn_gpu_open_desc *d, nvn_gpu_err *e) { (void)d; (void)e; return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_caps_get(nvn_gpu_caps *c) { memset(c, 0, sizeof *c); }
nvn_gpu_rc nvn_gpu_close(uint64_t n, nvn_gpu_err *e) { (void)n; atomic_fetch_add(&backend_calls,1); (void)e; return (nvn_gpu_rc){NVN_GPU_OK}; }
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "control") && strcmp(argv[1], "busy"))) return 2;
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
