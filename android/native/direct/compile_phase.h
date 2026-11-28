// Opt-in, bounded phase timings. Logcat's process/thread and monotonic times
// can be joined with the scheduler trace. No clocks or log writes in the player.
#pragma once
#include <stdint.h>

struct s2a_compile_stamp {
    uint64_t mono_ns, cpu_ns;
};

#if defined(__ANDROID__) && defined(S2A_ANDROID_DEBUG_ENV)
#include <android/log.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

enum { S2A_COMPILE_PHASE_LINES = 512 };
static _Atomic unsigned s2a_compile_phase_lines;

static inline uint64_t s2a_compile_clock_ns(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline struct s2a_compile_stamp s2a_compile_phase_begin(void) {
    static _Atomic int enabled = -1;
    int on = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (on < 0) {
        char value[PROP_VALUE_MAX];
        on = __system_property_get("debug.s2a.nvk_compile_phases", value) == 1 && value[0] == '1';
        atomic_store_explicit(&enabled, on, memory_order_relaxed);
    }
    if (!on || atomic_load_explicit(&s2a_compile_phase_lines, memory_order_relaxed) >= S2A_COMPILE_PHASE_LINES)
        return (struct s2a_compile_stamp){0};
    return (struct s2a_compile_stamp){s2a_compile_clock_ns(CLOCK_MONOTONIC), s2a_compile_clock_ns(CLOCK_THREAD_CPUTIME_ID)};
}

static inline void s2a_compile_phase_end(struct s2a_compile_stamp start, const char *phase, const uint8_t *id) {
    if (!start.mono_ns) return;
    const uint64_t cpu = s2a_compile_clock_ns(CLOCK_THREAD_CPUTIME_ID);
    const uint64_t mono = s2a_compile_clock_ns(CLOCK_MONOTONIC);
    if (atomic_fetch_add_explicit(&s2a_compile_phase_lines, 1, memory_order_relaxed) >= S2A_COMPILE_PHASE_LINES) return;
    char identity[65] = "-";
    if (id) {
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < 32; i++) {
            identity[2 * i] = hex[id[i] >> 4];
            identity[2 * i + 1] = hex[id[i] & 15];
        }
        identity[64] = 0;
    }
    char line[256];
    snprintf(line, sizeof line, "compile_phase phase=%s pid=%d tid=%d mono_ns=%llu wall_ns=%llu cpu_ns=%llu id=%s",
             phase, (int)getpid(), (int)gettid(), (unsigned long long)start.mono_ns,
             (unsigned long long)(mono - start.mono_ns),
             (unsigned long long)(cpu >= start.cpu_ns ? cpu - start.cpu_ns : 0), identity);
    __android_log_write(ANDROID_LOG_INFO, "S2Arm", line);
}
#else
static inline struct s2a_compile_stamp s2a_compile_phase_begin(void) {
    return (struct s2a_compile_stamp){0};
}
static inline void s2a_compile_phase_end(struct s2a_compile_stamp start, const char *phase, const uint8_t *id) {
    (void)start;
    (void)phase;
    (void)id;
}
#endif
