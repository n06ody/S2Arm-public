// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Diagnostic queue intervals. Marker durations include driver-dependent
// execution overlap and do not measure shader occupancy or memory traffic.
#include "nvn_vk_pass_time.h"

enum gpu_pt_kind {
    GPU_PT_RENDER, GPU_PT_GUEST_COMPUTE, GPU_PT_BC_DECODE, GPU_PT_TRANSFER, GPU_PT_COPY,
    GPU_PT_BLIT, GPU_PT_FB_COPY, GPU_PT_CLEAR, GPU_PT_MIRROR_SYNC, GPU_PT_OTHER_COMPUTE,
    GPU_PT_KINDS
};
static const char *const gpu_pt_names[] = {
    "render", "guest_compute", "bc_decode", "transfer", "copy", "blit",
    "fb_copy", "clear", "mirror_sync", "other_compute",
};
enum {
    GPU_PT_SPANS = 255, GPU_PT_POOL_CAP = 32, GPU_PT_KEYS = 256, GPU_PT_TOP = 8,
    GPU_PT_PRESENTS = 60, GPU_PT_PREFIX = 2 * GPU_PT_SPANS + 2, GPU_PT_QUERIES = GPU_PT_PREFIX + 1,
};
enum { GPU_PT_FB_END = 1, GPU_PT_FB_BEGIN = 2, GPU_PT_LAYOUT_REOPEN = 4, GPU_PT_EASU = 8 };
struct gpu_pt_att {
    uint32_t format, samples, load, store, stencil_load, stencil_store;
};
struct gpu_pt_key {
    uint32_t w, h, layers, draws, colors, flags;
    struct gpu_pt_att att[9]; // Color slots 0–7, depth/stencil slot 8. Undefined format means absent.
};
struct gpu_pt_span {
    enum gpu_pt_kind kind;
    struct gpu_pt_key key;
};
struct gpu_pt_cmd {
    struct gpu_pt_cmd *next;
    VkQueryPool pool;
    uint32_t count, dropped, next_pass_flags;
    int open, prefix;
    struct gpu_pt_span spans[GPU_PT_SPANS];
};
struct gpu_pt_rank {
    struct gpu_pt_key key;
    uint64_t ticks, count;
};
struct gpu_pt_window {
    uint64_t segments, measured, presents, rejected, missing_queries, missing_spans, dropped;
    uint64_t missing_gaps, gaps, gap_ticks, overlap_ticks, segment_ticks, residual_ticks;
    uint64_t ticks[GPU_PT_KINDS], counts[GPU_PT_KINDS], unranked;
    uint32_t keys;
    struct gpu_pt_rank ranks[GPU_PT_KEYS];
};
struct gpu_pt_session {
    struct gpu_pt_cmd *free;
    uint32_t allocated, bits;
    double period;
    uint64_t queued; // g_submit_mu
    uint64_t done, end, end_submit, window_id; // completion worker
    int have_end;
    struct gpu_pt_window window;
};
static pthread_mutex_t g_gpu_pt_mu = PTHREAD_MUTEX_INITIALIZER;

static void gpu_pt_open(void) {
    if (!nvn_plat_flag("nvk_gpu_pass_time", 0)) return;
    struct s2a_vk_device *dev = g.dev;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical, &n, NULL);
    VkQueueFamilyProperties *properties = calloc(n, sizeof *properties);
    uint32_t bits = 0;
    if (properties) {
        vkGetPhysicalDeviceQueueFamilyProperties(dev->physical, &n, properties);
        if (dev->queue_family < n) bits = properties[dev->queue_family].timestampValidBits;
        free(properties);
    }
    const double period = dev->properties.limits.timestampPeriod;
    if (!bits || bits > 64 || !isfinite(period) || period <= 0) {
        nvn_plat_log(NVN_PLAT_LOG_WARN, "gpu", "nvk-gpu-pass-time unavailable=queue_timestamp");
        return;
    }
    g.gpu_pt = calloc(1, sizeof *g.gpu_pt);
    if (!g.gpu_pt) {
        nvn_plat_log(NVN_PLAT_LOG_WARN, "gpu", "nvk-gpu-pass-time unavailable=allocation");
        return;
    }
    g.gpu_pt->bits = bits;
    g.gpu_pt->period = period;
}

static void gpu_pt_start(struct nvn_gpu_cmd_s *c) {
    struct gpu_pt_session *p = g.gpu_pt;
    if (!p) return;
    pthread_mutex_lock(&g_gpu_pt_mu);
    struct gpu_pt_cmd *q = p->free;
    if (q) {
        p->free = q->next;
    } else if (p->allocated < GPU_PT_POOL_CAP) {
        q = calloc(1, sizeof *q);
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GPU_PT_QUERIES};
        if (q && vkCreateQueryPool(g.d, &qi, NULL, &q->pool) != VK_SUCCESS) {
            free(q);
            q = NULL;
        }
        if (q) p->allocated++;
    }
    pthread_mutex_unlock(&g_gpu_pt_mu);
    if (!q) return; // The worker records a missing segment. Submission continues normally.
    q->count = q->dropped = q->next_pass_flags = 0;
    q->open = q->prefix = 0;
    c->gpu_pt = q;
    vkCmdResetQueryPool(c->cb, q->pool, 0, GPU_PT_PREFIX);
    vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, q->pool, 0);
}

static void gpu_pt_release(struct nvn_gpu_cmd_s *c) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    if (!q) return;
    pthread_mutex_lock(&g_gpu_pt_mu);
    q->next = g.gpu_pt->free;
    g.gpu_pt->free = q;
    pthread_mutex_unlock(&g_gpu_pt_mu);
}

static inline int gpu_pt_begin(struct nvn_gpu_cmd_s *c, enum gpu_pt_kind kind) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    if (!q) return 0;
    if (q->count == GPU_PT_SPANS) { q->dropped++; return 0; }
    q->spans[q->count].kind = kind;
    q->open = 1;
    vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, q->pool, 1 + 2 * q->count);
    return 1;
}

static inline void gpu_pt_end(struct nvn_gpu_cmd_s *c) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    if (!q || !q->open) return;
    vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, q->pool, 2 + 2 * q->count);
    q->count++;
    q->open = 0;
}

#define GPU_PT_RECORD(c, kind, call) do { \
    gpu_pt_begin((c), (kind)); \
    call; \
    gpu_pt_end((c)); \
} while (0)

static inline void gpu_pt_last(struct nvn_gpu_cmd_s *c) {
    if (c->gpu_pt)
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->gpu_pt->pool, 1 + 2 * c->gpu_pt->count);
}

static inline void gpu_pt_prefix(struct nvn_gpu_cmd_s *c) {
    if (!c->gpu_pt) return;
    struct gpu_pt_cmd *q = c->gpu_pt;
    q->prefix = 1;
    vkCmdResetQueryPool(c->init_cb, q->pool, GPU_PT_PREFIX, 1);
    vkCmdWriteTimestamp(c->init_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, q->pool, GPU_PT_PREFIX);
}

static void gpu_pt_attachment(struct gpu_pt_key *key, uint32_t slot, const VkAttachmentDescription *att) {
    key->att[slot] = (struct gpu_pt_att){att->format, att->samples, att->loadOp, att->storeOp,
        att->stencilLoadOp, att->stencilStoreOp};
}

static inline void gpu_pt_pass_begin(struct nvn_gpu_cmd_s *c, const nvn_gpu_pass_desc *d,
        const VkAttachmentDescription *att, uint32_t colors, int has_depth, uint32_t flags) {
    if (!c->gpu_pt) return;
    flags |= c->gpu_pt->next_pass_flags;
    c->gpu_pt->next_pass_flags = 0;
    if (!gpu_pt_begin(c, GPU_PT_RENDER)) return;
    struct gpu_pt_key *key = &c->gpu_pt->spans[c->gpu_pt->count].key;
    *key = (struct gpu_pt_key){.w = d->w, .h = d->h, .layers = d->layers ? d->layers : 1,
        .colors = colors, .flags = flags};
    uint32_t a = 0;
    for (uint32_t slot = 0; slot < d->ncolor && slot < 8; slot++)
        if (d->color[slot].view) gpu_pt_attachment(key, slot, &att[a++]);
    if (has_depth) gpu_pt_attachment(key, 8, &att[a]);
}

static inline struct gpu_pt_key *gpu_pt_current_pass(struct nvn_gpu_cmd_s *c) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    return q && q->open && q->spans[q->count].kind == GPU_PT_RENDER ? &q->spans[q->count].key : NULL;
}

static inline void gpu_pt_draw(struct nvn_gpu_cmd_s *c) {
    struct gpu_pt_key *key = gpu_pt_current_pass(c);
    if (key) key->draws++;
}

// Covers actual graphics queue submissions, including empty swapchain cleanup.
static VkResult gpu_pt_submit(struct vk_segment *s, const VkSubmitInfo *info, VkFence fence) {
    if (!g.gpu_pt) return vkQueueSubmit(g.queue, 1, info, fence);
    const uint64_t before = now_ns();
    VkResult r = vkQueueSubmit(g.queue, 1, info, fence);
    if (r != VK_SUCCESS) return r;
    const uint64_t serial = ++g.gpu_pt->queued;
    if (s) {
        s->gpu_pt_serial = serial;
        s->gpu_pt_submit_ns = before;
    }
    return r;
}

static void gpu_pt_rank(struct gpu_pt_window *w, const struct gpu_pt_key *key, uint64_t ticks) {
    for (uint32_t i = 0; i < w->keys; i++) {
        struct gpu_pt_rank *r = &w->ranks[i];
        if (!memcmp(&r->key, key, sizeof *key)) { r->ticks += ticks; r->count++; return; }
    }
    if (w->keys == GPU_PT_KEYS) { w->unranked++; return; }
    w->ranks[w->keys++] = (struct gpu_pt_rank){.key = *key, .ticks = ticks, .count = 1};
}

static void gpu_pt_report(struct gpu_pt_session *p, int partial) {
    struct gpu_pt_window *w = &p->window;
    if (!w->segments) return;
    const double per = w->presents ? p->period / 1e6 / w->presents : p->period / 1e6;
    const uint64_t id = ++p->window_id;
    char line[1900];
    int len = snprintf(line, sizeof line,
        "nvk-gpu-pass-time window id=%llu partial=%d end_mono_ns=%llu presents=%llu rejected=%llu units=%s bits=%u "
        "segments=%llu measured_segments=%llu missing_queries=%llu missing_spans=%llu cap_dropped=%llu "
        "unranked=%llu missing_gaps=%llu gap_pairs=%llu segment_ms=%.8f gap_ms=%.8f overlap_ms=%.8f "
        "total_ms=%.8f residual_ms=%.8f astc_encode_ms=0 astc_encode_spans=0 astc_encode_path=absent",
        (unsigned long long)id, partial, (unsigned long long)now_ns(),
        (unsigned long long)w->presents, (unsigned long long)w->rejected,
        w->presents ? "ms/present" : "ms/session-tail", p->bits, (unsigned long long)w->segments,
        (unsigned long long)w->measured, (unsigned long long)w->missing_queries, (unsigned long long)w->missing_spans,
        (unsigned long long)w->dropped, (unsigned long long)w->unranked,
        (unsigned long long)w->missing_gaps, (unsigned long long)w->gaps,
        w->segment_ticks * per, w->gap_ticks * per, w->overlap_ticks * per,
        (w->segment_ticks + w->gap_ticks - w->overlap_ticks) * per, w->residual_ticks * per);
    for (unsigned k = 0; k < GPU_PT_KINDS; k++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %s_ms=%.8f %s_spans=%llu",
            gpu_pt_names[k], w->ticks[k] * per, gpu_pt_names[k], (unsigned long long)w->counts[k]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);

    for (uint32_t i = 0; i < w->keys && i < GPU_PT_TOP; i++) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < w->keys; j++)
            if (w->ranks[j].ticks > w->ranks[best].ticks) best = j;
        struct gpu_pt_rank r = w->ranks[best];
        w->ranks[best] = w->ranks[i];
        w->ranks[i] = r;
        const struct gpu_pt_key *k = &r.key;
        len = snprintf(line, sizeof line, "nvk-gpu-pass-time top window=%llu rank=%u size=%ux%u layers=%u "
            "colors=%u flags=%u draws=%u occurrences=%llu per_present=%.6f %s=%.8f attachments=",
            (unsigned long long)id, i + 1, k->w, k->h, k->layers, k->colors, k->flags, k->draws,
            (unsigned long long)r.count, w->presents ? (double)r.count / w->presents : 0.0,
            w->presents ? "ms_per_present" : "ms", r.ticks * per);
        for (uint32_t a = 0; a < 9; a++) {
            const struct gpu_pt_att *d = &k->att[a];
            if (d->format != VK_FORMAT_UNDEFINED)
                len += snprintf(line + len, sizeof line - (size_t)len, "%s%u:%u/%u/%u/%u/%u/%u",
                    a ? "," : "", a, d->format, d->samples, d->load, d->store, d->stencil_load, d->stencil_store);
        }
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
    memset(w, 0, sizeof *w);
}

static int gpu_pt_read(struct nvn_gpu_vk *vk, const struct vk_segment *s) {
    struct gpu_pt_session *p = vk->gpu_pt;
    struct gpu_pt_window *w = &p->window;
    const struct gpu_pt_cmd *q = s->cmd->gpu_pt;
    if (!q) return 0;
    w->dropped += q->dropped;
    const uint64_t completed = now_ns();
    if (!nvk_pass_time_host_bound(s->gpu_pt_submit_ns, completed, p->bits, p->period)) return 0;

    // Read only written slots, after the existing fence. An unavailable result
    // leaves the segment out of the measured envelope (and breaks adjacency).
    uint64_t raw[GPU_PT_PREFIX], start;
    uint32_t n = 2 + 2 * q->count;
    if (vkGetQueryPoolResults(vk->d, q->pool, 0, n, n * sizeof raw[0], raw, sizeof raw[0],
                             VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    start = raw[0];
    if (q->prefix && vkGetQueryPoolResults(vk->d, q->pool, GPU_PT_PREFIX, 1, sizeof start, &start,
                                          sizeof start, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    const uint64_t end = raw[n - 1];
    const int64_t duration = s2a_ts_wrap_diff(end, start, p->bits);
    if (duration < 0) return 0;
    if (p->have_end) {
        if (nvk_pass_time_host_bound(p->end_submit, completed, p->bits, p->period)) {
            struct nvk_pass_time_interval d = nvk_pass_time_between(p->end, start, (uint64_t)duration, p->bits);
            w->gaps++;
            w->gap_ticks += d.gap;
            w->overlap_ticks += d.overlap;
            p->have_end = !d.advances_end;
        } else {
            w->missing_gaps++;
            p->have_end = 0;
        }
    }
    if (!p->have_end) { p->end = end; p->end_submit = s->gpu_pt_submit_ns; }
    p->have_end = 1;
    w->measured++;
    w->segment_ticks += (uint64_t)duration;
    uint64_t attributed = 0, offset = 0;
    for (uint32_t i = 0; i < q->count; i++) {
        uint64_t elapsed;
        if (!nvk_pass_time_span(start, raw[1 + 2 * i], raw[2 + 2 * i], (uint64_t)duration, offset,
                                p->bits, &offset, &elapsed)) {
            w->missing_spans++;
            continue;
        }
        enum gpu_pt_kind k = q->spans[i].kind;
        attributed += elapsed;
        w->ticks[k] += elapsed;
        w->counts[k]++;
        if (k == GPU_PT_RENDER) gpu_pt_rank(w, &q->spans[i].key, elapsed);
    }
    w->residual_ticks += (uint64_t)duration - attributed;
    return 1;
}

static void gpu_pt_complete(struct nvn_gpu_vk *vk, const struct vk_segment *s, int success) {
    struct gpu_pt_session *p = vk->gpu_pt;
    if (!p || !s->submitted) return;
    struct gpu_pt_window *w = &p->window;
    const uint64_t skipped = s->gpu_pt_serial - p->done - 1;
    const int had_predecessor = p->done != 0;
    const int had_end = p->have_end;
    if (skipped) {
        w->missing_gaps += skipped + had_predecessor;
        p->have_end = 0;
    }
    p->done = s->gpu_pt_serial;
    w->segments++;
    const int measured = success && gpu_pt_read(vk, s);
    if (had_predecessor && !skipped && (!measured || !had_end)) w->missing_gaps++;
    if (!measured) {
        w->missing_queries++;
        p->have_end = 0;
    }
    if (s->internal) {
        if (s->gpu_pt_presented) w->presents++;
        else w->rejected++;
        if (w->presents == GPU_PT_PRESENTS) gpu_pt_report(p, 0);
    }
}

static void gpu_pt_close(void) {
    struct gpu_pt_session *p = g.gpu_pt;
    if (!p) return;
    gpu_pt_report(p, 1);
    while (p->free) {
        struct gpu_pt_cmd *q = p->free;
        p->free = q->next;
        vkDestroyQueryPool(g.d, q->pool, NULL);
        free(q);
    }
    free(p);
    g.gpu_pt = NULL;
}
