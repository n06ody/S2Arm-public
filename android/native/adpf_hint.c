// SPDX-License-Identifier: Apache-2.0
// ADPF is a scheduling hint; it does not choose CPUs or prove performance.
// All session calls and the pending span are serialized by mu because the NDK
// session API is not thread safe.
#include "adpf_hint.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <limits.h>
#include <string.h>
#ifndef S2A_ADPF_TEST
#include <dlfcn.h>
#include <android/log.h>
#include <android/performance_hint.h>
#include <stdbool.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "debug_gate.h"
#endif

typedef void *(*create_fn)(const int32_t *, size_t, int64_t);
typedef int (*threads_fn)(void *, const int32_t *, size_t);
typedef int (*duration_fn)(void *, int64_t);
typedef void (*close_fn)(void *);
#ifdef S2A_ADPF_TEST
extern uint64_t s2a_adpf_test_now(void);
extern int32_t s2a_adpf_test_tid(void);
extern void *s2a_adpf_test_create(const int32_t *, size_t, int64_t);
extern int s2a_adpf_test_set_threads(void *, const int32_t *, size_t);
extern int s2a_adpf_test_update(void *, int64_t);
extern int s2a_adpf_test_report(void *, int64_t);
extern void s2a_adpf_test_close(void *);
extern int s2a_adpf_test_enabled(void);
extern int s2a_adpf_test_cpu_work_enabled(void);
extern int s2a_adpf_test_supported(void);
#define ADPF_LOG(...) ((void)0)
static uint64_t monotonic_ns(void) { return s2a_adpf_test_now(); }
static int32_t current_tid(void) { return s2a_adpf_test_tid(); }
static create_fn backend_create = s2a_adpf_test_create;
static threads_fn backend_threads = s2a_adpf_test_set_threads;
static duration_fn backend_update = s2a_adpf_test_update, backend_report = s2a_adpf_test_report;
static close_fn backend_close = s2a_adpf_test_close;
static int backend_init(void) { return s2a_adpf_test_supported(); }
static int platform_enabled(void) { return s2a_adpf_test_enabled(); }
static int platform_cpu_work_enabled(void) { return s2a_adpf_test_cpu_work_enabled(); }
#else
static __typeof__(&APerformanceHint_getManager) get_manager;
static __typeof__(&APerformanceHint_createSession) create_session;
static __typeof__(&APerformanceHint_setThreads) set_threads;
static __typeof__(&APerformanceHint_updateTargetWorkDuration) update_session;
static __typeof__(&APerformanceHint_reportActualWorkDuration) report_session;
static __typeof__(&APerformanceHint_closeSession) close_session;
extern int32_t s2a_adpf_recorder_tid(void) __attribute__((weak));
#define ADPF_LOG(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
// Default on; debug.s2a.adpf=0 switches it off at runtime (polled ~1 Hz).
static int platform_enabled(void) {
    char value[PROP_VALUE_MAX] = {0};
    return __system_property_get("debug.s2a.adpf", value) <= 0 || strcmp(value, "0") != 0;
}
static int platform_cpu_work_enabled(void) {
    char value[PROP_VALUE_MAX] = {0};
    return !s2a_debug_entries_open() ||
           __system_property_get("debug.s2a.adpf_cpu_work", value) <= 0 || strcmp(value, "0") != 0;
}
static void *android_lib;
static int attempted_load, api_supported;
static uint64_t monotonic_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static int32_t current_tid(void) {
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);   // macOS 无 SYS_gettid，仅宿主语法检查走到
    return (int32_t)tid;
#else
    return (int32_t)syscall(SYS_gettid);
#endif
}
static void *backend_create_real(const int32_t *tids, size_t count, int64_t target) {
    struct APerformanceHintManager *manager = get_manager();
    return manager ? create_session(manager, tids, count, target) : NULL;
}
static int backend_threads_real(void *s, const int32_t *tids, size_t count) {
    return set_threads ? set_threads(s, tids, count) : ENOSYS;
}
static int backend_update_real(void *s, int64_t ns) { return update_session(s, ns); }
static int backend_report_real(void *s, int64_t ns) { return report_session(s, ns); }
static void backend_close_real(void *s) { close_session(s); }
static create_fn backend_create = backend_create_real;
static threads_fn backend_threads = backend_threads_real;
static duration_fn backend_update = backend_update_real, backend_report = backend_report_real;
static close_fn backend_close = backend_close_real;
// APerformanceHint_* needs API 33+: resolve the symbols lazily so older Android
// keeps running with the hint silently absent instead of failing to load.
static int backend_init(void) {
    if (attempted_load) return api_supported;
    attempted_load = 1;
    android_lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!android_lib) { ADPF_LOG("[s2a-adpf] libandroid.so unavailable; hint disabled"); return 0; }
    *(void **)(&get_manager) = dlsym(android_lib, "APerformanceHint_getManager");
    *(void **)(&create_session) = dlsym(android_lib, "APerformanceHint_createSession");
    *(void **)(&set_threads) = dlsym(android_lib, "APerformanceHint_setThreads"); // Optional on API 33.
    *(void **)(&update_session) = dlsym(android_lib, "APerformanceHint_updateTargetWorkDuration");
    *(void **)(&report_session) = dlsym(android_lib, "APerformanceHint_reportActualWorkDuration");
    *(void **)(&close_session) = dlsym(android_lib, "APerformanceHint_closeSession");
    api_supported = get_manager && create_session && update_session && report_session && close_session;
    if (!api_supported) ADPF_LOG("[s2a-adpf] APerformanceHint_* missing (API<33); hint disabled");
    return api_supported;
}
#endif
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int foreground, surface, stopping;
static void *session;
static int32_t session_tid;
static int64_t session_target;
static int32_t session_record_tid;
#if defined(S2A_ANDROID_DEBUG_ENV) && !defined(S2A_ADPF_TEST)
static uint64_t frame_log_ns;
#endif
// One in-flight frame span: the sequence it belongs to, the CLOCK_MONOTONIC
// instant its first job began, and the tid that ran that job.
static uint64_t work_seq, work_begin;
static int32_t work_tid;
static uint64_t work_gpu_wait_ns;
static int cpu_work_enabled = 1;
// Lock-free fast path mirror of work_seq: repeat jobs of the same frame skip mu.
static _Atomic uint64_t pending_seq;
#ifndef S2A_ADPF_TEST
static _Thread_local int32_t cached_tid;
#endif
static uint64_t retry_at;
static uint64_t property_checked;
static int enabled = 1;
static int enabled_locked(uint64_t now) {
    if (!property_checked || now < property_checked || now - property_checked >= 1000000000ULL) {
        int was = enabled;
        enabled = platform_enabled(); property_checked = now;
        cpu_work_enabled = platform_cpu_work_enabled();
        if (was != enabled) ADPF_LOG("[s2a-adpf] enabled=%d (debug.s2a.adpf)", enabled);
    }
    return enabled;
}
static void clear_work_locked(void) {
    work_seq = work_begin = 0; work_tid = 0;
    work_gpu_wait_ns = 0;
    atomic_store_explicit(&pending_seq, 0, memory_order_release);
}
static void close_locked(void) {
    if (session) { ADPF_LOG("[s2a-adpf] close tid=%d", session_tid); backend_close(session); }
    session = NULL; session_tid = 0; session_target = 0;
    clear_work_locked();
}
static void fail_locked(uint64_t now) {
    close_locked();
    retry_at = now > UINT64_MAX - 1000000000ULL ? UINT64_MAX : now + 1000000000ULL;
}
void s2a_adpf_set_foreground(int v) {
    pthread_mutex_lock(&mu); foreground = !!v;
    if (!foreground) { close_locked(); retry_at = 0; }
    pthread_mutex_unlock(&mu);
}
void s2a_adpf_set_surface(int v) {
    pthread_mutex_lock(&mu); surface = !!v;
    if (!surface) { close_locked(); retry_at = 0; }
    pthread_mutex_unlock(&mu);
}
static int32_t work_thread(void) {
#ifdef S2A_ADPF_TEST
    return current_tid();
#else
    if (!cached_tid) cached_tid = current_tid();
    return cached_tid;
#endif
}
void s2a_adpf_work_begin(uint64_t seq) {
    if (!seq || atomic_load_explicit(&pending_seq, memory_order_acquire) == seq) return;
    const int32_t tid = work_thread();
    const uint64_t now = monotonic_ns();
    pthread_mutex_lock(&mu);
    if (now && !enabled_locked(now)) { close_locked(); pthread_mutex_unlock(&mu); return; }
    if (!stopping && foreground && surface && now && tid > 0 && now >= retry_at) {
        if (work_seq != seq) {
            work_seq = seq;
            work_begin = now;
            work_tid = tid;
            work_gpu_wait_ns = 0;
        }
        atomic_store_explicit(&pending_seq, work_seq, memory_order_release);
    }
    pthread_mutex_unlock(&mu);
}
void s2a_adpf_exclude_gpu_wait(uint64_t begin_ns, uint64_t end_ns) {
    if (end_ns <= begin_ns || !atomic_load_explicit(&pending_seq, memory_order_acquire)) return;
    const int32_t tid = work_thread();
    pthread_mutex_lock(&mu);
    // A wait that began before this span may have returned after a surface
    // change or a different thread's present. It belongs to neither new span.
    if (cpu_work_enabled && work_seq && tid == work_tid && begin_ns >= work_begin)
        work_gpu_wait_ns += end_ns - begin_ns;
    pthread_mutex_unlock(&mu);
}
void s2a_adpf_frame_end(int64_t target_ns, uint64_t seq) {
    const uint64_t now = monotonic_ns();
    pthread_mutex_lock(&mu);
    if (now && !enabled_locked(now)) { close_locked(); pthread_mutex_unlock(&mu); return; }
    if (work_seq != seq) {
        // A span whose present never ran (skipped/lost frame) is stale: it must
        // not be charged to a later frame. Discard it; the next matching pair
        // records a fresh span.
        clear_work_locked();
        pthread_mutex_unlock(&mu); return;
    }
    const uint64_t began = work_begin; const int32_t tid = work_tid;
    const uint64_t gpu_wait_ns = cpu_work_enabled ? work_gpu_wait_ns : 0;
    clear_work_locked();
    if (stopping || !foreground || !surface || !began || now <= began || target_ns <= 0 || now < retry_at) {
        pthread_mutex_unlock(&mu); return;
    }
    if ((cpu_work_enabled && work_thread() != tid) || gpu_wait_ns >= now - began) {
        pthread_mutex_unlock(&mu);
        return;
    }
    const uint64_t elapsed = now - began - gpu_wait_ns;
    if (elapsed > (uint64_t)INT64_MAX || !backend_init()) { pthread_mutex_unlock(&mu); return; }
    int32_t record_tid = 0;
#ifdef S2A_ADPF_TEST
    if (cpu_work_enabled) record_tid = s2a_adpf_recorder_tid();
#else
    if (cpu_work_enabled && s2a_adpf_recorder_tid) record_tid = s2a_adpf_recorder_tid();
#endif
    if (record_tid <= 0 || record_tid == tid) record_tid = 0;
    const int32_t tids[] = {tid, record_tid};
    const size_t count = record_tid ? 2 : 1;
    // A different replay owner starts a new workload. The same owner can
    // change the recorder membership without losing its timing history.
    if (session && session_tid != tid) close_locked();
    if (session && session_record_tid != record_tid) {
        const int rc = backend_threads(session, tids, count);
        if (rc == ENOSYS || rc == ENOTSUP) {
            close_locked();
        } else if (rc != 0) {
            fail_locked(now);
            pthread_mutex_unlock(&mu);
            return;
        } else {
            session_record_tid = record_tid;
        }
    }
    if (!session) {
        session = backend_create(tids, count, target_ns);
        if (!session) { fail_locked(now); pthread_mutex_unlock(&mu); return; }
        session_tid = tid; session_target = target_ns;
        session_record_tid = record_tid;
        ADPF_LOG("[s2a-adpf] session tid=%d target_ns=%lld record_tid=%d", tid, (long long)target_ns, record_tid);
    }
    if (session_target != target_ns) {
        if (backend_update(session, target_ns)) { fail_locked(now); pthread_mutex_unlock(&mu); return; }
        session_target = target_ns;
        ADPF_LOG("[s2a-adpf] target_ns=%lld", (long long)target_ns);
    }
#if defined(S2A_ANDROID_DEBUG_ENV) && !defined(S2A_ADPF_TEST)
    if (s2a_debug_entries_open() && (!frame_log_ns || now - frame_log_ns >= 5000000000ull)) {
        frame_log_ns = now;
        ADPF_LOG("[s2a-adpf] work seq=%llu tid=%d record_tid=%d target_ns=%lld wall_ns=%llu fence_ns=%llu report_ns=%llu",
                 (unsigned long long)seq, tid, record_tid, (long long)target_ns,
                 (unsigned long long)(now - began), (unsigned long long)gpu_wait_ns,
                 (unsigned long long)elapsed);
    }
#endif
    if (backend_report(session, (int64_t)elapsed)) fail_locked(now);
    pthread_mutex_unlock(&mu);
}
void s2a_adpf_discard(void) { pthread_mutex_lock(&mu); clear_work_locked(); pthread_mutex_unlock(&mu); }
void s2a_adpf_invisible(void) { pthread_mutex_lock(&mu); close_locked(); pthread_mutex_unlock(&mu); }
void s2a_adpf_stop(void) { pthread_mutex_lock(&mu); stopping = 1; foreground = surface = 0; close_locked(); pthread_mutex_unlock(&mu); }
#ifdef S2A_ADPF_TEST
void s2a_adpf_test_reset(void) {
    pthread_mutex_lock(&mu); close_locked(); foreground = surface = stopping = 0; retry_at = property_checked = 0; enabled = cpu_work_enabled = 1; pthread_mutex_unlock(&mu);
}
#endif
