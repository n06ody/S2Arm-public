// SPDX-License-Identifier: Apache-2.0
// 提交线程持有的计算写集合；不借用资源指针，不随 fence 完成丢弃尚未向图形发布的写。
#pragma once
#include "nvn_backend.h"
#include <stdlib.h>
#include <string.h>
struct rp_compute_access { nvn_gpu_access_span *span; size_t n, cap; };
static inline int rp_access_intersects(const nvn_gpu_access_span *a, const nvn_gpu_access_span *b) {
    if (!a->storage || a->storage != b->storage || !!a->aspects != !!b->aspects) return 0;
    if (!a->aspects) return a->off < b->off + b->bytes && b->off < a->off + a->bytes;
    return (a->aspects & b->aspects) && a->level < (uint64_t)b->level + b->levels && b->level < (uint64_t)a->level + a->levels &&
           a->layer < (uint64_t)b->layer + b->layers && b->layer < (uint64_t)a->layer + a->layers;
}
static inline int rp_access_read(const struct rp_compute_access *w, const nvn_gpu_access_span *r) {
    for (size_t i = 0; i < w->n; i++) if (rp_access_intersects(&w->span[i], r)) return 1;
    return 0;
}
static inline int rp_access_reserve(struct rp_compute_access *w, size_t add) {
    if (add > SIZE_MAX / sizeof *w->span - w->n) return 0;
    size_t need = w->n + add;
    if (need <= w->cap) return 1;
    size_t cap = w->cap ? w->cap : 16;
    while (cap < need) { if (cap > SIZE_MAX / sizeof *w->span / 2) { cap = need; break; } cap *= 2; }
    nvn_gpu_access_span *p = realloc(w->span, cap * sizeof *p);
    if (!p) return 0;
    w->span = p; w->cap = cap; return 1;
}
// 调用前 reserve 成功；重复覆盖不增长，缓冲的相交 / 相邻窗口可合并（不把两个窗口之间的洞当成写过）。
static inline void rp_access_write(struct rp_compute_access *w, const nvn_gpu_access_span *s) {
    for (size_t i = 0; i < w->n; i++) {
        nvn_gpu_access_span *p = &w->span[i];
        if (p->storage != s->storage || p->aspects != s->aspects) continue;
        if (!s->aspects && p->off <= s->off + s->bytes && s->off <= p->off + p->bytes) {
            uint64_t end = p->off + p->bytes > s->off + s->bytes ? p->off + p->bytes : s->off + s->bytes;
            if (s->off < p->off) p->off = s->off;
            p->bytes = end - p->off; return;
        }
        if (!memcmp(p, s, sizeof *s)) return;
    }
    w->span[w->n++] = *s;
}
static inline void rp_access_reset(struct rp_compute_access *w) { free(w->span); memset(w, 0, sizeof *w); }
