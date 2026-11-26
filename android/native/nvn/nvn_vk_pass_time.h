// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stdbool.h>
#include "nvn_vk_ts_math.h"

// A host bound shorter than half the counter cycle disambiguates signed GPU
// differences, including the sign of the time between two queue submissions.
static inline bool nvk_pass_time_host_bound(uint64_t submitted, uint64_t completed, uint32_t bits, double period) {
    return completed >= submitted && (double)(completed - submitted) < (double)(UINT64_C(1) << (bits - 1)) * period;
}

struct nvk_pass_time_interval {
    uint64_t gap, overlap;
    bool advances_end;
};

static inline struct nvk_pass_time_interval nvk_pass_time_between(uint64_t previous_end, uint64_t begin,
        uint64_t duration, uint32_t bits) {
    int64_t delta = s2a_ts_wrap_diff(begin, previous_end, bits);
    if (delta >= 0) return (struct nvk_pass_time_interval){(uint64_t)delta, 0, true};
    uint64_t shared = (uint64_t)(-(delta + 1)) + 1;
    return (struct nvk_pass_time_interval){0, shared < duration ? shared : duration, shared < duration};
}

// Individual operations must lie inside the same monotonic segment envelope.
// Prefix/tail barriers and unmeasured commands can leave holes between them.
static inline bool nvk_pass_time_span(uint64_t origin, uint64_t start, uint64_t end, uint64_t duration,
        uint64_t previous_end, uint32_t bits, uint64_t *offset, uint64_t *elapsed) {
    const uint64_t mask = s2a_ts_valid_mask(bits);
    const uint64_t a = (start - origin) & mask;
    const uint64_t b = (end - origin) & mask;
    if (a < previous_end || b < a || b > duration) return false;
    *offset = b;
    *elapsed = b - a;
    return true;
}
