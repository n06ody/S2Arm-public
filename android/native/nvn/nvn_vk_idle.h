// Graphics-queue timestamps only. An overlapping TOP/BOTTOM pair contributes no idle time.
// The completion worker feeds submissions in queue order; the caller serializes CPU counters.
#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn_backend.h"
#include "nvn_vk_ts_math.h"

enum { IDLE_IN_FRAME, IDLE_AFTER_PRESENT, IDLE_CLASSES };
struct idle_bucket {
    uint64_t gaps[IDLE_CLASSES], ticks[IDLE_CLASSES];
    uint64_t frames, samples, missing, overlap, ambiguous, submit_calls, submit_ns;
    uint64_t waits[NVN_GPU_WAIT_SITES], wait_ns[NVN_GPU_WAIT_SITES];
};
struct idle_account {
    struct idle_bucket bucket;
    uint64_t window_start, end, submit_begin;
    int have_end, after_present;
};

static inline void idle_wait(struct idle_bucket *bucket, unsigned site, uint64_t ns, int first) {
    bucket->waits[site] += first;
    bucket->wait_ns[site] += ns;
}

static inline void idle_sample(struct idle_account *a, unsigned bits, double period,
                               uint64_t start, uint64_t end, uint64_t submitted, uint64_t now, int valid, int present) {
    struct idle_bucket *b = &a->bucket;
    b->frames += present;
    if (!valid || !bits || bits > 64) {
        b->missing++;
        a->have_end = 0;
        return;
    }
    uint64_t half = UINT64_C(1) << (bits - 1);
    uint64_t mask = s2a_ts_valid_mask(bits);
    end &= mask;
    if (now < submitted || (double)(now - submitted) >= (double)half * period || ((end - start) & mask) >= half) {
        b->ambiguous++;
        a->have_end = 0;
        return;
    }
    b->samples++;
    if (a->have_end) {
        uint64_t gap = (start - a->end) & mask;
        if ((double)(now - a->submit_begin) >= (double)half * period) {
            b->ambiguous++;  // completion interval too long to distinguish wrap from overlap
        } else {
            unsigned cls = a->after_present ? IDLE_AFTER_PRESENT : IDLE_IN_FRAME;
            b->gaps[cls]++;
            if (gap < half) b->ticks[cls] += gap;
            else {
                b->overlap++;
                if (((end - a->end) & mask) >= half) {
                    end = a->end;
                    submitted = a->submit_begin;
                }
            }
        }
    }
    a->have_end = 1;
    a->end = end;
    a->submit_begin = submitted;
    a->after_present = present;
}

// Rotate only the counters. The first submission of the next window still has a predecessor.
static inline int idle_take(struct idle_account *a, uint64_t now, struct idle_bucket *out) {
    if (now - a->window_start < UINT64_C(5000000000)) return 0;
    *out = a->bucket;
    memset(&a->bucket, 0, sizeof a->bucket);
    a->window_start = now;
    return 1;
}
