// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Production provider destroy body with a controlled, conservative device wait.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "direct/s2a_direct.h"
#include "direct/vk_device.h"
#include "gfx_plat/nvk_session.h"
#include "provider_context.h"
#define TAG "test"
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
static struct provider_context *g_active_context, *g_retained_context;
static pthread_mutex_t g_window_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_stop_published, g_session_active;
static atomic_int idle_entered, idle_release, destroy_done, frees, device_destroys, close_calls;
static int idle_block, unbind_block, destroy_rc;
static atomic_int unbind_entered;
static char last_log[512];
static uint64_t test_deadline;
static int64_t ctl_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec; }
static void pause_ms(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static int __android_log_print(int level, const char *tag, const char *fmt, ...) { (void)level; (void)tag; va_list ap; va_start(ap, fmt); int r = vsnprintf(last_log, sizeof last_log, fmt, ap); va_end(ap); return r; }
int nvk_session_wait_compiles(void) { return 0; }
void nvk_session_request_stop(void) { if (!test_deadline) test_deadline = (uint64_t)ctl_now_ns() + NVK_SHUTDOWN_BUDGET_NS; }
uint64_t nvk_session_stop_deadline(void) { return test_deadline; }
static void diag_log_manifest(const char *a, const char *b) { (void)a; (void)b; }
int32_t s2a_direct_unbind_compiler_v1(void *ctx) { (void)ctx; atomic_store(&unbind_entered, 1); while (unbind_block && !atomic_load(&idle_release)) pause_ms(); return 0; }
static void ctl_wake(void) {}
static void ctl_wake_stop(void) {}
static int stop_window_lock(void) { return pthread_mutex_lock(&g_window_mu); }
int nvk_session_close(void) { atomic_fetch_add(&close_calls, 1); return 0; }
static void release_lease_window(struct provider_context *ctx) { (void)ctx; }
int s2a_vk_device_wait_idle(struct s2a_vk_device *d) {
    (void)d; atomic_store(&idle_entered, 1);
    while (idle_block && !atomic_load(&idle_release)) pause_ms();
    return VK_SUCCESS;
}
int s2a_vk_device_destroy(struct s2a_vk_device *d, int idle_proven) {
    if (!idle_proven && s2a_vk_device_wait_idle(d) != VK_SUCCESS) return -1;
    atomic_fetch_add(&device_destroys, 1); return 0;
}
static void tracked_free(void *p) { atomic_fetch_add(&frees, 1); free(p); }
#define free tracked_free
#include "provider_destroy.h"
#undef free
static void *do_destroy(void *p) { destroy_rc = destroy_impl(p); atomic_store(&destroy_done, 1); return NULL; }
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "control") && strcmp(argv[1], "blocked") && strcmp(argv[1], "active") && strcmp(argv[1], "unbind"))) return 2;
    struct provider_context *ctx = calloc(1, sizeof *ctx); if (!ctx) return 2;
    ctx->state = S2A_DIRECT_ACTIVE; ctx->engine_ok = !strcmp(argv[1], "active"); ctx->device_ok = 1; g_active_context = ctx;
    if (ctx->engine_ok) {
        int rc = destroy_impl(ctx);
        printf("active: rc=%d close=%d idle=%d destroy=%d free=%d retained=%d\n", rc, atomic_load(&close_calls), atomic_load(&idle_entered), atomic_load(&device_destroys), atomic_load(&frees), g_retained_context == ctx);
        if (rc != S2A_DIRECT_RES_RETIREMENT || atomic_load(&close_calls) || atomic_load(&idle_entered) || atomic_load(&device_destroys) || atomic_load(&frees) || g_retained_context != ctx) {
            fprintf(stderr, "FAIL: active session must retain backend and device until process exit\n"); return 1;
        }
        return 0;
    }
    idle_block = !strcmp(argv[1], "blocked"); unbind_block = !strcmp(argv[1], "unbind"); pthread_t t;
    if (pthread_create(&t, NULL, do_destroy, ctx)) return 2;
    for (int i = 0; i < 3000 && !(unbind_block ? atomic_load(&unbind_entered) : atomic_load(&idle_entered)); i++) pause_ms();
    if (!(unbind_block ? atomic_load(&unbind_entered) : atomic_load(&idle_entered))) { fprintf(stderr, "SETUP: device idle not entered\n"); atomic_store(&idle_release, 1); pthread_join(t, NULL); return 2; }
    if (!idle_block && !unbind_block) {
        pthread_join(t, NULL);
        int ok = destroy_rc == S2A_DIRECT_RES_OK && atomic_load(&device_destroys) == 1 && atomic_load(&frees) == 1;
        printf("idle/control: rc=%d destroyed=%d freed=%d\n", destroy_rc, atomic_load(&device_destroys), atomic_load(&frees));
        return ok ? 0 : 1;
    }
    uint64_t limit = (uint64_t)ctl_now_ns() + 1200000000ull;
    while (!atomic_load(&destroy_done) && (uint64_t)ctl_now_ns() < limit) pause_ms();
    int returned = atomic_load(&destroy_done), freed = atomic_load(&frees), destroyed = atomic_load(&device_destroys);
    char timeout_log[sizeof last_log]; snprintf(timeout_log, sizeof timeout_log, "%s", last_log);
    atomic_store(&idle_release, 1); pthread_join(t, NULL);
    printf("idle/blocked: before release returned=%d freed=%d destroyed=%d rc=%d\n", returned, freed, destroyed, destroy_rc);
    int fail = 0;
    if (!returned) { fprintf(stderr, "FAIL: provider destroy exceeded budget waiting device idle\n"); fail = 1; }
    if (returned && (destroy_rc != S2A_DIRECT_RES_RETIREMENT || freed || destroyed)) { fprintf(stderr, "FAIL: device/context must remain retained after idle timeout\n"); fail = 1; }
#ifdef HAS_IDLE_WORKER
    if (returned && (!strstr(timeout_log, "device retirement timeout") || !strstr(timeout_log, unbind_block ? "compiler unbind" : "device idle"))) { fprintf(stderr, "FAIL: retirement timeout stage must be logged\n"); fail = 1; }
    if (returned && !unbind_block) {
        while (!atomic_load_explicit(&ctx->idle_done, memory_order_acquire)) pause_ms();
        test_deadline = (uint64_t)ctl_now_ns() + NVK_SHUTDOWN_BUDGET_NS;
        if (destroy_impl(ctx) != S2A_DIRECT_RES_OK || atomic_load(&frees) != 1 || atomic_load(&device_destroys) != 1) { fprintf(stderr, "FAIL: completed idle can be reaped exactly once\n"); fail = 1; }
    }
#endif
    return fail;
}
