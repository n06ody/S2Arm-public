// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Diagnostic queue intervals. Marker durations include driver-dependent
// execution overlap and do not measure shader occupancy or memory traffic.
#include "nvn_vk_pass_time.h"

enum gpu_pt_kind {
    GPU_PT_RENDER, GPU_PT_GUEST_COMPUTE, GPU_PT_BC_DECODE, GPU_PT_ASTC_ENCODE, GPU_PT_TRANSFER, GPU_PT_COPY,
    GPU_PT_BLIT, GPU_PT_FB_COPY, GPU_PT_CLEAR_WSI, GPU_PT_CLEAR_TEXTURE, GPU_PT_CLEAR_BUFFER,
    GPU_PT_CLEAR_BC_SHADOW, GPU_PT_MIRROR_SYNC, GPU_PT_OTHER_COMPUTE,
    GPU_PT_KINDS
};
static const char *const gpu_pt_names[] = {
    "render", "guest_compute", "bc_decode", "astc_encode", "transfer", "copy", "blit",
    "fb_copy", "clear_wsi", "clear_texture", "clear_buffer", "clear_bc_shadow", "mirror_sync", "other_compute",
};
enum {
    GPU_PT_SPANS = 255, GPU_PT_POOL_CAP = 32, GPU_PT_KEYS = 256, GPU_PT_TOP = 8,
    GPU_PT_PRESENTS = 60, GPU_PT_PREFIX = 2 * GPU_PT_SPANS + 2, GPU_PT_QUERIES = GPU_PT_PREFIX + 1,
    GPU_PT_LIGHT_PREFIX = 2, GPU_PT_CLEAR_PENDING = 64,
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
// First recorded possible use of overlapping subresources in the same segment.
// Pool indexing and accesses across a segment boundary remain explicit unknowns.
enum gpu_pt_clear_use {
    GPU_PT_USE_ATTACHMENT, GPU_PT_USE_SAMPLED, GPU_PT_USE_STORAGE, GPU_PT_USE_POOL,
    GPU_PT_USE_READ, GPU_PT_USE_WRITE, GPU_PT_USE_RECLEAR, GPU_PT_USE_UNRESOLVED, GPU_PT_USES
};
static const char *const gpu_pt_use_names[] = {
    "attachment", "sampled", "storage", "pool_possible", "transfer_read", "transfer_write", "reclear", "pool_unknown",
};
struct gpu_pt_pending_clear {
    const struct nvn_gpu_tex_s *tex;
    VkImageSubresourceRange range;
};
struct gpu_pt_cmd {
    struct gpu_pt_cmd *next;
    VkQueryPool pool;
    uint32_t count, dropped, next_pass_flags;
    int open, prefix, light;
    uint32_t clear_count, clear_dropped;
    uint32_t clear_uses[GPU_PT_USES];
    struct gpu_pt_pending_clear clears[GPU_PT_CLEAR_PENDING];
    struct gpu_pt_span spans[];
};
struct gpu_pt_rank {
    struct gpu_pt_key key;
    uint64_t ticks, count;
};
struct gpu_pt_window {
    uint64_t segments, measured, presents, rejected, missing_queries, missing_spans, dropped;
    uint64_t missing_gaps, gaps, gap_ticks, overlap_ticks, segment_ticks, residual_ticks;
    uint64_t ticks[GPU_PT_KINDS], counts[GPU_PT_KINDS], unranked;
    uint64_t clear_uses[GPU_PT_USES], clear_tail, clear_dropped;
    uint32_t keys;
    struct gpu_pt_rank ranks[GPU_PT_KEYS];
};
struct gpu_pt_session {
    struct gpu_pt_cmd *free;
    uint32_t allocated, bits;
    int light;
    double period;
    uint64_t queued; // g_submit_mu
    uint64_t done, end, end_submit, window_id; // completion worker
    int have_end;
    struct gpu_pt_window window;
};
static pthread_mutex_t g_gpu_pt_mu = PTHREAD_MUTEX_INITIALIZER;

static void gpu_pt_open(void) {
    // 0 = disabled, 1 = full ledger, 2 = only segment/queue intervals.
    const int mode = nvn_plat_flag("nvk_gpu_pass_time", 0);
    if (!mode) return;
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
    g.gpu_pt->light = mode == 2;
}

static void gpu_pt_start(struct nvn_gpu_cmd_s *c) {
    struct gpu_pt_session *p = g.gpu_pt;
    if (!p) return;
    pthread_mutex_lock(&g_gpu_pt_mu);
    struct gpu_pt_cmd *q = p->free;
    if (q) {
        p->free = q->next;
    } else if (p->allocated < GPU_PT_POOL_CAP) {
        q = calloc(1, sizeof *q + (p->light ? 0 : GPU_PT_SPANS * sizeof q->spans[0]));
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = p->light ? GPU_PT_LIGHT_PREFIX + 1 : GPU_PT_QUERIES};
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
    q->light = p->light;
    if (!q->light) {
        q->clear_count = q->clear_dropped = 0;
        memset(q->clear_uses, 0, sizeof q->clear_uses);
    }
    c->gpu_pt = q;
    vkCmdResetQueryPool(c->cb, q->pool, 0, q->light ? GPU_PT_LIGHT_PREFIX : GPU_PT_PREFIX);
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
    if (!q || q->light) return 0;
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
    q->prefix = q->light ? GPU_PT_LIGHT_PREFIX : GPU_PT_PREFIX;
    vkCmdResetQueryPool(c->init_cb, q->pool, (uint32_t)q->prefix, 1);
    vkCmdWriteTimestamp(c->init_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, q->pool, (uint32_t)q->prefix);
}

static int gpu_pt_clear_overlap(const VkImageSubresourceRange *a, const VkImageSubresourceRange *b) {
    return (a->aspectMask & b->aspectMask) &&
        a->baseMipLevel < b->baseMipLevel + b->levelCount &&
        b->baseMipLevel < a->baseMipLevel + a->levelCount &&
        a->baseArrayLayer < b->baseArrayLayer + b->layerCount &&
        b->baseArrayLayer < a->baseArrayLayer + a->layerCount;
}

static void gpu_pt_clear_consume(struct gpu_pt_cmd *q, uint32_t i, enum gpu_pt_clear_use use) {
    q->clear_uses[use]++;
    q->clears[i] = q->clears[--q->clear_count];
}

static inline void gpu_pt_clear_use(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_tex_s *t,
        const VkImageSubresourceRange *range, enum gpu_pt_clear_use use) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    if (!q || !q->clear_count || !t) return;
    for (uint32_t i = 0; i < q->clear_count;) {
        const struct gpu_pt_pending_clear *p = &q->clears[i];
        if (p->tex == t && (!range || gpu_pt_clear_overlap(&p->range, range))) gpu_pt_clear_consume(q, i, use);
        else i++;
    }
}

static inline void gpu_pt_clear_view_use(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_view_s *v,
        enum gpu_pt_clear_use use) {
    if (!v || !c->gpu_pt || !c->gpu_pt->clear_count) return;
    const VkImageSubresourceRange r = {v->aspect, v->base_level, v->levels, v->base_layer, v->layers};
    gpu_pt_clear_use(c, v->tex, &r, use);
}

static inline void gpu_pt_clear_layers_use(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_tex_s *t,
        const VkImageSubresourceLayers *layer, enum gpu_pt_clear_use use) {
    if (!c->gpu_pt || !c->gpu_pt->clear_count) return;
    const VkImageSubresourceRange r = {layer->aspectMask, layer->mipLevel, 1,
        layer->baseArrayLayer, layer->layerCount};
    gpu_pt_clear_use(c, t, &r, use);
}

static inline void gpu_pt_record_clear_texture(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_tex_s *t,
        const VkImageSubresourceRange *range) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    if (!q || q->light) return;
    gpu_pt_clear_use(c, t, range, GPU_PT_USE_RECLEAR);
    if (q->clear_count == GPU_PT_CLEAR_PENDING) { q->clear_dropped++; return; }
    q->clears[q->clear_count++] = (struct gpu_pt_pending_clear){t, *range};
}

static void gpu_pt_clear_attachment_use(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_view_s *v,
        VkImageAspectFlags aspect, uint32_t layers) {
    if (!v) return;
    const VkImageSubresourceRange range = {aspect, v->base_level, 1, v->att_layer, layers ? layers : 1};
    gpu_pt_clear_use(c, v->tex, &range, GPU_PT_USE_ATTACHMENT);
}

static inline void gpu_pt_clear_bound_use(struct nvn_gpu_cmd_s *c, unsigned start, unsigned end) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    if (!q || !q->clear_count) return;
    for (unsigned st = start; st < end && q->clear_count; st++) {
        const uint32_t count = c->pipe->stage_nent[st];
        for (uint32_t i = 0; i < c->nbinds[st] && q->clear_count; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if (b->ent >= count || b->cls != c->pipe->stage_ents[st][b->ent].cls) continue;
            switch (b->cls) {
            case NVN_GPU_RES_TEX: gpu_pt_clear_view_use(c, b->u.view, GPU_PT_USE_SAMPLED); break;
            case NVN_GPU_RES_TEX_SAMP: gpu_pt_clear_view_use(c, b->u.ts.view, GPU_PT_USE_SAMPLED); break;
            case NVN_GPU_RES_IMAGE: gpu_pt_clear_view_use(c, b->u.view, GPU_PT_USE_STORAGE); break;
            default: break;
            }
        }
        for (uint32_t e = 0; e < count && q->clear_count && e < 64; e++) {
            const nvn_gpu_pool_table pool = c->pool_binds[st][e];
            if (!pool || pool->kind != NVN_GPU_POOL_IMAGE) continue;
            for (uint32_t i = 0; i < q->clear_count;) {
                if (!pool->ikey) gpu_pt_clear_consume(q, i, GPU_PT_USE_UNRESOLVED);
                else if (nvn_gpu_pool_has_texture(pool, (nvn_gpu_tex)q->clears[i].tex))
                    gpu_pt_clear_consume(q, i, GPU_PT_USE_POOL);
                else i++;
            }
        }
    }
}

static void gpu_pt_attachment(struct gpu_pt_key *key, uint32_t slot, const VkAttachmentDescription *att) {
    key->att[slot] = (struct gpu_pt_att){att->format, att->samples, att->loadOp, att->storeOp,
        att->stencilLoadOp, att->stencilStoreOp};
}

static inline void gpu_pt_pass_begin(struct nvn_gpu_cmd_s *c, const nvn_gpu_pass_desc *d,
        const VkAttachmentDescription *att, uint32_t colors, int has_depth, uint32_t flags) {
    if (!c->gpu_pt) return;
    if (c->gpu_pt->clear_count) {
        for (uint32_t i = 0; i < d->ncolor && i < 8; i++)
            gpu_pt_clear_attachment_use(c, d->color[i].view, VK_IMAGE_ASPECT_COLOR_BIT, d->layers);
        gpu_pt_clear_attachment_use(c, d->depth.view, VK_IMAGE_ASPECT_DEPTH_BIT, d->layers);
        gpu_pt_clear_attachment_use(c, d->stencil.view, VK_IMAGE_ASPECT_STENCIL_BIT, d->layers);
    }
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
        "nvk-gpu-pass-time window id=%llu partial=%d end_mono_ns=%llu mode=%s presents=%llu rejected=%llu units=%s bits=%u "
        "segments=%llu measured_segments=%llu missing_queries=%llu missing_spans=%llu cap_dropped=%llu "
        "unranked=%llu missing_gaps=%llu gap_pairs=%llu segment_ms=%.8f gap_ms=%.8f overlap_ms=%.8f "
        "total_ms=%.8f",
        (unsigned long long)id, partial, (unsigned long long)now_ns(), p->light ? "segment" : "full",
        (unsigned long long)w->presents, (unsigned long long)w->rejected,
        w->presents ? "ms/present" : "ms/session-tail", p->bits, (unsigned long long)w->segments,
        (unsigned long long)w->measured, (unsigned long long)w->missing_queries, (unsigned long long)w->missing_spans,
        (unsigned long long)w->dropped, (unsigned long long)w->unranked,
        (unsigned long long)w->missing_gaps, (unsigned long long)w->gaps,
        w->segment_ticks * per, w->gap_ticks * per, w->overlap_ticks * per,
        (w->segment_ticks + w->gap_ticks - w->overlap_ticks) * per);
    if (p->light) {
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
        memset(w, 0, sizeof *w);
        return;
    }
    uint64_t clear_ticks = 0, clear_spans = 0;
    for (unsigned k = GPU_PT_CLEAR_WSI; k <= GPU_PT_CLEAR_BC_SHADOW; k++) {
        clear_ticks += w->ticks[k];
        clear_spans += w->counts[k];
    }
    len += snprintf(line + len, sizeof line - (size_t)len, " residual_ms=%.8f clear_ms=%.8f clear_spans=%llu",
        w->residual_ticks * per, clear_ticks * per, (unsigned long long)clear_spans);
    for (unsigned k = 0; k < GPU_PT_KINDS; k++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %s_ms=%.8f %s_spans=%llu",
            gpu_pt_names[k], w->ticks[k] * per, gpu_pt_names[k], (unsigned long long)w->counts[k]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);

    uint64_t tracked = w->clear_tail;
    for (unsigned k = 0; k < GPU_PT_USES; k++) tracked += w->clear_uses[k];
    if (tracked || w->clear_dropped) {
        len = snprintf(line, sizeof line,
            "nvk-gpu-pass-time clear-first-use window=%llu scope=segment tracked=%llu tail=%llu cap_dropped=%llu",
            (unsigned long long)id, (unsigned long long)tracked, (unsigned long long)w->clear_tail,
            (unsigned long long)w->clear_dropped);
        for (unsigned k = 0; k < GPU_PT_USES; k++)
            len += snprintf(line + len, sizeof line - (size_t)len, " %s=%llu",
                gpu_pt_use_names[k], (unsigned long long)w->clear_uses[k]);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }

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
    if (!q->light) {
        w->dropped += q->dropped;
        w->clear_tail += q->clear_count;
        w->clear_dropped += q->clear_dropped;
        for (unsigned k = 0; k < GPU_PT_USES; k++) w->clear_uses[k] += q->clear_uses[k];
    }
    const uint64_t completed = now_ns();
    if (!nvk_pass_time_host_bound(s->gpu_pt_submit_ns, completed, p->bits, p->period)) return 0;

    // Read only written slots, after the existing fence. An unavailable result
    // leaves the segment out of the measured envelope (and breaks adjacency).
    uint64_t raw[GPU_PT_PREFIX], start;
    uint32_t n = 2 + 2 * q->count;
    if (vkGetQueryPoolResults(vk->d, q->pool, 0, n, n * sizeof raw[0], raw, sizeof raw[0],
                             VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    start = raw[0];
    if (q->prefix && vkGetQueryPoolResults(vk->d, q->pool, (uint32_t)q->prefix, 1, sizeof start, &start,
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
    if (q->light) return 1;
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
