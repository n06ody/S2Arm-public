// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

#include <stdint.h>

#if defined(__ANDROID__) || defined(S2A_WAIT_TRACE_TEST)
enum { NVN_WAIT_TRACE_READY, NVN_WAIT_TRACE_ALLOWED };
void s2a_nvk_wait_trace_publish(const void *sync, uint64_t fence) __attribute__((weak));
uint64_t s2a_nvk_wait_trace_begin(const void *sync, uint64_t timeout, uintptr_t site) __attribute__((weak));
void s2a_nvk_wait_trace_end(uint64_t call, uint64_t fence, int result) __attribute__((weak));
void s2a_nvk_wait_trace_fast(uint64_t fence, uint64_t completed, uint64_t timeout, int path) __attribute__((weak));
#endif

struct nvn_wait_trace_land {
    uint64_t begin, locked, copy_begin, copy_end, relocked, end;
    uint64_t id, registered, copied, mirror, offset, readback, skipped, probe_armed;
};
struct nvn_wait_trace_sub {
    uint64_t begin, land_begin, land_end, effects_end, replay_publish, end;
    uint64_t invalidate_begin, invalidate_end;
};
#if defined(__ANDROID__) || defined(S2A_WAIT_TRACE_TEST)
struct nvn_gpu_buf_s;
uint64_t s2a_nvk_wait_trace_mirror(struct nvn_gpu_buf_s *buf, uint64_t pool, unsigned slot) __attribute__((weak));
uint64_t s2a_nvk_wait_trace_retire_stamp(void) __attribute__((weak));
void s2a_nvk_wait_trace_land(const struct nvn_wait_trace_land *row) __attribute__((weak));
void s2a_nvk_wait_trace_sub(const struct nvn_wait_trace_sub *row) __attribute__((weak));
#endif

static inline uint64_t nvn_wait_trace_stamp(void) {
#if defined(__ANDROID__) || defined(S2A_WAIT_TRACE_TEST)
    if (s2a_nvk_wait_trace_retire_stamp) return s2a_nvk_wait_trace_retire_stamp();
#endif
    return 0;
}

static inline void nvn_wait_trace_emit_land(const struct nvn_wait_trace_land *row) {
#if defined(__ANDROID__) || defined(S2A_WAIT_TRACE_TEST)
    if (s2a_nvk_wait_trace_land) s2a_nvk_wait_trace_land(row);
#else
    (void)row;
#endif
}

static inline void nvn_wait_trace_emit_sub(const struct nvn_wait_trace_sub *row) {
#if defined(__ANDROID__) || defined(S2A_WAIT_TRACE_TEST)
    if (s2a_nvk_wait_trace_sub) s2a_nvk_wait_trace_sub(row);
#else
    (void)row;
#endif
}
