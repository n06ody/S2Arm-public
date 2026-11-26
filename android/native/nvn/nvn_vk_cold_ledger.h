// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stdint.h>

enum {
    COLD_READ_HOST = 1u << 0,
    COLD_READ_TRANSFER = 1u << 1,
    COLD_READ_COMPUTE = 1u << 2,
    COLD_READ_GRAPHICS = 1u << 3,
    COLD_LEDGER_SPANS = 96,
};
struct cold_interval {
    uint64_t lo, hi, epoch;
    unsigned writers;
};
struct cold_ledger {
    unsigned n;
    struct cold_interval span[COLD_LEDGER_SPANS];
};

static inline int cold_interval_add(struct cold_ledger *ledger, uint64_t lo, uint64_t hi,
                                    uint64_t epoch, unsigned writers, uint64_t write_epoch) {
    if (lo >= hi || !writers) return 1;
    if (ledger->n) {
        struct cold_interval *last = &ledger->span[ledger->n - 1];
        if (lo == last->hi && writers == last->writers &&
            (epoch == last->epoch || (epoch < write_epoch && last->epoch < write_epoch))) {
            // The update stream's epochs never decrease. Two older epochs
            // cannot match this or a later write's epoch. Preserve the newest
            // epoch boundary, which still detects unordered GPU overlap.
            last->hi = hi;
            if (last->epoch < epoch) last->epoch = epoch;
            return 1;
        }
    }
    if (ledger->n == COLD_LEDGER_SPANS) return 0;
    ledger->span[ledger->n++] = (struct cold_interval){lo, hi, epoch, writers};
    return 1;
}

// Exact host/transfer writes establish initialized bytes. A declared shader
// store can cover only some bytes, so it preserves prior producers and never
// fills a hole. A possible overlap with a previous unordered GPU write drops
// the proof for that overlap. An unsupported write invalidates its footprint.
static inline void cold_ledger_write(struct cold_ledger *ledger, uint64_t lo, uint64_t hi,
                                     uint64_t epoch, unsigned writer, int exact) {
    struct cold_ledger next = {0};
    uint64_t cursor = lo;
    for (unsigned i = 0; i < ledger->n; i++) {
        const struct cold_interval *s = &ledger->span[i];
        if (s->hi <= lo || s->lo >= hi) {
            if (s->lo >= hi && cursor < hi) {
                if (!cold_interval_add(&next, cursor, hi, epoch, exact ? writer : 0, epoch)) goto unknown;
                cursor = hi;
            }
            if (!cold_interval_add(&next, s->lo, s->hi, s->epoch, s->writers, epoch)) goto unknown;
            continue;
        }
        if (s->lo < lo && !cold_interval_add(&next, s->lo, lo, s->epoch, s->writers, epoch)) goto unknown;
        const uint64_t begin = s->lo > lo ? s->lo : lo;
        const uint64_t end = s->hi < hi ? s->hi : hi;
        if (!cold_interval_add(&next, cursor, begin, epoch, exact ? writer : 0, epoch)) goto unknown;
        const int ordered = s->epoch != epoch || (s->writers & ~COLD_READ_HOST) == 0 || writer == COLD_READ_HOST;
        const unsigned mask = ordered && writer ? writer | (exact ? 0 : s->writers) : 0;
        if (!cold_interval_add(&next, begin, end, epoch, mask, epoch)) goto unknown;
        cursor = end;
        if (s->hi > hi && !cold_interval_add(&next, hi, s->hi, s->epoch, s->writers, epoch)) goto unknown;
    }
    if (!cold_interval_add(&next, cursor, hi, epoch, exact ? writer : 0, epoch)) goto unknown;
    *ledger = next;
    return;
unknown:
    ledger->n = 0;
}
