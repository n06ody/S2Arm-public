// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

struct mirror_auto_window {
    double frame_ms, p99_ms, present_ms, acquire_ms;
    double land_bytes, draws, submits, sync_bytes;
    double copy_ms, shadow_wait_ms, maintenance_ms, wait_ms;
    uint64_t target_ns;
    int thermal;
};

enum { MIRROR_AUTO_VERSION = 2, MIRROR_AUTO_RECEIPT_SEC = 1800 };
struct mirror_auto_receipt {
    uint64_t version, created;
    uint8_t compatibility[32];
    struct mirror_auto_window baseline[2];
};

static inline int mirror_auto_valid(const struct mirror_auto_window *w) {
    const double samples[] = {w->frame_ms, w->p99_ms, w->present_ms, w->acquire_ms,
                              w->land_bytes, w->draws, w->submits, w->sync_bytes,
                              w->copy_ms, w->shadow_wait_ms, w->maintenance_ms, w->wait_ms};
    for (unsigned i = 0; i < sizeof samples / sizeof *samples; i++)
        if (!isfinite(samples[i]) || samples[i] < 0) return 0;
    return w->frame_ms > 0 && w->p99_ms > 0 && w->draws > 0 && w->submits > 0 &&
           w->target_ns && w->thermal >= 0;
}

static inline int mirror_auto_close(double a, double b) {
    return fabs(a - b) <= 0.15 * fmax(a, b);
}

static inline int mirror_auto_match(const struct mirror_auto_window *a, const struct mirror_auto_window *b) {
    return mirror_auto_valid(a) && mirror_auto_valid(b) && a->target_ns == b->target_ns &&
           a->thermal == b->thermal && mirror_auto_close(a->land_bytes, b->land_bytes) &&
           mirror_auto_close(a->draws, b->draws) && mirror_auto_close(a->submits, b->submits) &&
           mirror_auto_close(a->sync_bytes, b->sync_bytes);
}

// 只准入试测；影子压力仍需给 B 的真实缓存维护与未知开销留余量。
static inline int mirror_auto_screen(const struct mirror_auto_window *a) {
    return mirror_auto_valid(a) && a->land_bytes >= 64 * 1024 &&
           a->frame_ms > 1.05 * (a->target_ns / 1e6) &&
           a->copy_ms >= fmax(1.0, 0.05 * a->frame_ms) &&
           a->shadow_wait_ms < 0.5 * a->copy_ms;
}

static inline uint64_t mirror_auto_overlap_ns(uint64_t begin, uint64_t end, uint64_t wait_begin,
                                               uint64_t window_begin) {
    if (begin < wait_begin) begin = wait_begin;
    if (begin < window_begin) begin = window_begin;
    return end > begin ? end - begin : 0;
}

static inline const char *mirror_auto_observe(struct mirror_auto_window *w, double direct_kib,
                                               double overlap_ms) {
    const double direct_bytes = direct_kib * 1024.0;
    if (!mirror_auto_valid(w) || !isfinite(overlap_ms) || overlap_ms < 0)
        return "insufficient_headroom";
    if (!isfinite(direct_bytes) || direct_bytes < 0 || direct_bytes > w->sync_bytes ||
        direct_bytes > 0.10 * w->land_bytes) return "unmodeled_direct_upload";
    if (!w->land_bytes) return "insufficient_headroom";
    // 仅估算试测资格。对可能改走有序上传的字节，按原回写每字节耗时收两遍成本。
    const double upload_ms = 2.0 * (direct_bytes / w->land_bytes) * w->copy_ms;
    // 只给测到与等待重叠的旧映射 memcpy 折半。GPU 等待和回调其他时间照全额计。
    const double measured_overlap = fmin(overlap_ms, fmin(w->shadow_wait_ms, w->copy_ms));
    w->shadow_wait_ms += upload_ms - 0.5 * measured_overlap;
    return mirror_auto_screen(w) ? NULL : "insufficient_headroom";
}

static inline int mirror_auto_receipt_valid(const struct mirror_auto_receipt *record,
                                            const uint8_t compatibility[32], uint64_t now) {
    return record->version == MIRROR_AUTO_VERSION && record->created <= now &&
           now - record->created <= MIRROR_AUTO_RECEIPT_SEC &&
           !memcmp(record->compatibility, compatibility, sizeof record->compatibility) &&
           mirror_auto_screen(&record->baseline[0]) && mirror_auto_screen(&record->baseline[1]) &&
           mirror_auto_match(&record->baseline[0], &record->baseline[1]);
}

static inline int mirror_auto_accept(const struct mirror_auto_window off[2],
                                     const struct mirror_auto_window on[2]) {
    if (!mirror_auto_match(&off[0], &off[1]) || !mirror_auto_match(&on[0], &on[1])) return 0;
    const double frame_noise = fabs(off[0].frame_ms - off[1].frame_ms);
    const double tail_noise = fmax(1.0, fabs(off[0].p99_ms - off[1].p99_ms));
    const double acquire_noise = fabs(off[0].acquire_ms - off[1].acquire_ms);
    const double present_noise = fabs(off[0].present_ms - off[1].present_ms);
    for (unsigned i = 0; i < 2; i++) {
        const struct mirror_auto_window *a = &off[i], *b = &on[i];
        if (!mirror_auto_match(a, b) ||
            a->frame_ms - b->frame_ms <= fmax(0.05 * a->frame_ms, frame_noise) ||
            b->p99_ms > a->p99_ms + tail_noise ||
            b->acquire_ms > a->acquire_ms + fmax(acquire_noise, 0.01 * b->frame_ms) ||
            b->present_ms > a->present_ms + fmax(present_noise, 0.01 * b->frame_ms) ||
            fmax(b->copy_ms + b->maintenance_ms, b->wait_ms) >= a->copy_ms) return 0;
    }
    return 1;
}
