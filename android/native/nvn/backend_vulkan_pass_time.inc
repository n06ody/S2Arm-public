// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Diagnostic queue intervals. Marker durations include driver-dependent
// execution overlap and do not measure shader occupancy or memory traffic.
#include "nvn_vk_pass_time.h"

enum gpu_pt_kind {
    GPU_PT_RENDER, GPU_PT_GUEST_COMPUTE, GPU_PT_BC_DECODE, GPU_PT_ASTC_ENCODE, GPU_PT_TRANSFER, GPU_PT_COPY,
    GPU_PT_BLIT, GPU_PT_FB_COPY, GPU_PT_CLEAR, GPU_PT_MIRROR_SYNC, GPU_PT_OTHER_COMPUTE,
    GPU_PT_LAND_COPY, GPU_PT_LAND_DEP_BEFORE, GPU_PT_LAND_DEP_AFTER, GPU_PT_COLD_GUARD_COMPUTE,
    GPU_PT_COLD_GUARD_DEP, GPU_PT_BARRIER,
    GPU_PT_KINDS
};
static const char *const gpu_pt_names[] = {
    "render", "guest_compute", "bc_decode", "astc_encode", "transfer", "copy", "blit",
    "fb_copy", "clear", "mirror_sync", "other_compute",
    "land_copy", "land_dep_before", "land_dep_after", "cold_guard_compute", "cold_guard_dep", "barrier",
};
enum {
    GPU_PT_SPANS = 255, GPU_PT_POOL_CAP = 32, GPU_PT_KEYS = 256, GPU_PT_TOP = 8,
    GPU_PT_SIZES = 128, GPU_PT_DETAIL_TOP = 10, GPU_PT_DRAW_RUNS = 16, GPU_PT_PROGRAMS = 512,
    GPU_PT_PRESENTS = 60, GPU_PT_PREFIX = 2 * GPU_PT_SPANS + 2, GPU_PT_QUERIES = GPU_PT_PREFIX + 1,
};
enum { GPU_PT_FB_END = 1, GPU_PT_FB_BEGIN = 2, GPU_PT_LAYOUT_REOPEN = 4, GPU_PT_EASU = 8 };
struct gpu_pt_att {
    uint32_t format, samples, load, store, stencil_load, stencil_store;
};
struct gpu_pt_key {
    uint32_t w, h, layers, draws, colors, flags;
    VkRect2D area;
    struct gpu_pt_att att[9]; // Color slots 0–7, depth/stencil slot 8. Undefined format means absent.
};
struct gpu_pt_draw_run {
    const struct nvn_gpu_pipe_s *pipe;
    uint32_t draws;
};
struct gpu_pt_span {
    enum gpu_pt_kind kind;
    struct gpu_pt_key key;
    uint32_t runs, lost_draws;
    struct gpu_pt_draw_run run[GPU_PT_DRAW_RUNS];
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
struct gpu_pt_size {
    uint32_t w, h;
    uint64_t ticks[2], count[2];
};
struct gpu_pt_detail {
    struct gpu_pt_span pass;
    uint64_t ticks, serial, present_attempt, present;
    uint32_t ordinal;
    int internal;
};
struct gpu_pt_lane {
    uint64_t segments, measured, missing_queries, missing_spans, dropped, segment_ticks, residual_ticks;
    uint64_t ticks[GPU_PT_KINDS], counts[GPU_PT_KINDS];
};
struct gpu_pt_window {
    uint64_t segments, measured, presents, rejected, missing_queries, missing_spans, dropped;
    uint64_t missing_gaps, gaps, gap_ticks, overlap_ticks, segment_ticks, residual_ticks;
    uint64_t ticks[GPU_PT_KINDS], counts[GPU_PT_KINDS], unranked;
    uint64_t land_bytes, land_batches, land_regions, size_unlisted_ticks, size_unlisted;
    uint32_t keys;
    struct gpu_pt_rank ranks[GPU_PT_KEYS];
    uint32_t sizes, details;
    struct gpu_pt_size by_size[GPU_PT_SIZES];
    struct gpu_pt_detail detail[GPU_PT_DETAIL_TOP];
    struct gpu_pt_lane lane[2];
};
struct gpu_pt_session {
    struct gpu_pt_cmd *free;
    uint32_t allocated, bits;
    double period;
    uint64_t queued; // g_submit_mu
    uint64_t done, end, end_submit, window_id; // completion worker
    uint64_t present_attempts, presents;
    uint32_t detail_w, detail_h, programs;
    // Pipelines and their shader identities stay alive until after the worker
    // and this ledger close, as for loss_note_program(). IDs in logs are 1-based.
    const struct nvn_gpu_pipe_s *program[GPU_PT_PROGRAMS];
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
    const int width = nvn_plat_flag("nvk_gpu_pass_width", 1600);
    const int height = nvn_plat_flag("nvk_gpu_pass_height", 900);
    g.gpu_pt->detail_w = width > 0 ? (uint32_t)width : 0;
    g.gpu_pt->detail_h = height > 0 ? (uint32_t)height : 0;
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
    if (q->open || q->count == GPU_PT_SPANS) { q->dropped++; return 0; }
    q->spans[q->count].kind = kind;
    q->spans[q->count].runs = q->spans[q->count].lost_draws = 0;
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

static inline int gpu_pt_barrier_begin(struct nvn_gpu_cmd_s *c) {
    // A named operation may already own this interval. Inside a render pass,
    // its dependencies belong to that pass instead of a nested sample.
    return c->gpu_pt && !c->gpu_pt->open && !c->in_pass && gpu_pt_begin(c, GPU_PT_BARRIER);
}

#define GPU_PT_RECORD(c, kind, call) do { \
    const int gpu_pt_marked = gpu_pt_begin((c), (kind)); \
    call; \
    if (gpu_pt_marked) gpu_pt_end((c)); \
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
        const VkAttachmentDescription *att, VkRect2D area, uint32_t colors, int has_depth, uint32_t flags) {
    if (!c->gpu_pt) return;
    flags |= c->gpu_pt->next_pass_flags;
    c->gpu_pt->next_pass_flags = 0;
    if (!gpu_pt_begin(c, GPU_PT_RENDER)) return;
    struct gpu_pt_key *key = &c->gpu_pt->spans[c->gpu_pt->count].key;
    *key = (struct gpu_pt_key){.w = d->w, .h = d->h, .layers = d->layers ? d->layers : 1,
        .colors = colors, .flags = flags, .area = area};
    uint32_t a = 0;
    for (uint32_t slot = 0; slot < d->ncolor && slot < 8; slot++)
        if (d->color[slot].view) gpu_pt_attachment(key, slot, &att[a++]);
    if (has_depth) gpu_pt_attachment(key, 8, &att[a]);
}

static inline struct gpu_pt_key *gpu_pt_current_pass(struct nvn_gpu_cmd_s *c) {
    struct gpu_pt_cmd *q = c->gpu_pt;
    return q && q->open && q->spans[q->count].kind == GPU_PT_RENDER ? &q->spans[q->count].key : NULL;
}

static inline void gpu_pt_draw(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *pipe) {
    struct gpu_pt_key *key = gpu_pt_current_pass(c);
    if (!key) return;
    key->draws++;
    if (key->w != g.gpu_pt->detail_w || key->h != g.gpu_pt->detail_h) return;
    struct gpu_pt_span *span = &c->gpu_pt->spans[c->gpu_pt->count];
    if (!span->lost_draws && span->runs && span->run[span->runs - 1].pipe == pipe) {
        span->run[span->runs - 1].draws++;
    } else if (span->runs < GPU_PT_DRAW_RUNS) {
        span->run[span->runs++] = (struct gpu_pt_draw_run){pipe, 1};
    } else {
        span->lost_draws++;
    }
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

static void gpu_pt_render(struct gpu_pt_session *p, const struct vk_segment *s,
        uint32_t ordinal, const struct gpu_pt_span *span, uint64_t ticks) {
    struct gpu_pt_window *w = &p->window;
    const struct gpu_pt_key *key = &span->key;
    const int internal = !!s->internal;
    uint32_t pos = 0;
    while (pos < w->sizes && (w->by_size[pos].w != key->w || w->by_size[pos].h != key->h)) pos++;
    if (pos == GPU_PT_SIZES) {
        w->size_unlisted++;
        w->size_unlisted_ticks += ticks;
    } else {
        if (pos == w->sizes) {
            w->by_size[pos] = (struct gpu_pt_size){.w = key->w, .h = key->h};
            w->sizes++;
        }
        w->by_size[pos].ticks[internal] += ticks;
        w->by_size[pos].count[internal]++;
    }

    if (key->w != p->detail_w || key->h != p->detail_h) return;
    uint32_t rank = w->details;
    while (rank && ticks > w->detail[rank - 1].ticks) rank--;
    if (rank == GPU_PT_DETAIL_TOP) return;
    const uint32_t last = w->details < GPU_PT_DETAIL_TOP ? w->details++ : GPU_PT_DETAIL_TOP - 1;
    if (last > rank)
        memmove(&w->detail[rank + 1], &w->detail[rank], (last - rank) * sizeof w->detail[0]);
    w->detail[rank] = (struct gpu_pt_detail){.pass = *span, .ticks = ticks,
        .serial = s->gpu_pt_serial, .present_attempt = p->present_attempts + 1,
        .ordinal = ordinal, .internal = internal};
}

static void gpu_pt_hex(char *out, const uint8_t *bytes, size_t n) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        *out++ = digits[bytes[i] >> 4];
        *out++ = digits[bytes[i] & 15];
    }
    *out = '\0';
}

static uint32_t gpu_pt_program_id(struct gpu_pt_session *p, const struct nvn_gpu_pipe_s *pipe) {
    if (!pipe) return 0;
    for (uint32_t i = 0; i < p->programs; i++) if (p->program[i] == pipe) return i + 1;
    if (p->programs == GPU_PT_PROGRAMS) return 0;
    p->program[p->programs++] = pipe;
    for (unsigned stage = 0; stage < NVN_GPU_SH_CS; stage++) {
        const struct nvn_gpu_shader_s *sh = pipe->key.st[stage].sh;
        if (!sh) continue;
        char source[17], library[17], digest[65], line[240];
        gpu_pt_hex(source, sh->src_key, sizeof sh->src_key);
        gpu_pt_hex(library, sh->stage_key, sizeof sh->stage_key);
        gpu_pt_hex(digest, sh->spirv_digest, sizeof sh->spirv_digest);
        snprintf(line, sizeof line, "nvk-gpu-pass-time shader program=%u stage=%u src_key=%s stage_key=%s spirv_digest=%s",
            p->programs, stage, source, library, digest);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
    return p->programs;
}

static void gpu_pt_print_attachments(char *line, size_t size, int len, const struct gpu_pt_key *key) {
    int first = 1;
    for (unsigned a = 0; a < 9; a++) {
        const struct gpu_pt_att *att = &key->att[a];
        if (att->format == VK_FORMAT_UNDEFINED) continue;
        if (len < 0 || (size_t)len >= size) return;
        len += snprintf(line + len, size - (size_t)len, "%s%u:%u/%u/%u/%u/%u/%u",
            first ? "" : ",", a, att->format, att->samples, att->load, att->store,
            att->stencil_load, att->stencil_store);
        first = 0;
    }
}

static void gpu_pt_report_sizes(struct gpu_pt_session *p, uint64_t id, double per) {
    struct gpu_pt_window *w = &p->window;
    char line[900];
    uint64_t listed[2] = {0, 0};
    for (uint32_t i = 0; i < w->details; i++) {
        const struct gpu_pt_detail *d = &w->detail[i];
        listed[d->internal] += d->ticks;
        const struct gpu_pt_key *k = &d->pass.key;
        int len = snprintf(line, sizeof line, "nvk-gpu-pass-time pass window=%llu rank=%u lane=%s"
            " present=%llu attempt=%llu segment=%llu span=%u size=%ux%u area=%d,%d,%u,%u"
            " ms=%.8f layers=%u colors=%u flags=%u draws=%u lost_draws=%u programs=",
            (unsigned long long)id, i + 1, d->internal ? "wsi" : "game", (unsigned long long)d->present,
            (unsigned long long)d->present_attempt, (unsigned long long)d->serial, d->ordinal, k->w, k->h,
            k->area.offset.x, k->area.offset.y, k->area.extent.width, k->area.extent.height,
            d->ticks * p->period / 1e6, k->layers, k->colors, k->flags, k->draws, d->pass.lost_draws);
        // ID 0 denotes an internal program or a full shader dictionary. Keep
        // the count and order even when the identity could not be reported.
        for (uint32_t n = 0; n < d->pass.runs && len >= 0 && (size_t)len < sizeof line; n++) {
            const struct gpu_pt_draw_run *run = &d->pass.run[n];
            const uint32_t program = gpu_pt_program_id(p, run->pipe);
            len += snprintf(line + len, sizeof line - (size_t)len, "%s%u:%u", n ? "," : "", program, run->draws);
        }
        if (len >= 0 && (size_t)len < sizeof line) {
            len += snprintf(line + len, sizeof line - (size_t)len, " attachments=");
            gpu_pt_print_attachments(line, sizeof line, len, k);
        }
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
    for (uint32_t i = 0; i < w->sizes; i++) {
        const struct gpu_pt_size *s = &w->by_size[i];
        const int target = s->w == p->detail_w && s->h == p->detail_h;
        for (int lane = 0; lane < 2; lane++) {
            if (!s->count[lane]) continue;
            const uint64_t detailed = target ? listed[lane] : 0;
            snprintf(line, sizeof line, "nvk-gpu-pass-time size window=%llu lane=%s size=%ux%u spans=%llu"
                " units=%s ms=%.8f detail_ms=%.8f unlisted_ms=%.8f",
                (unsigned long long)id, lane ? "wsi" : "game", s->w, s->h,
                (unsigned long long)s->count[lane], w->presents ? "ms/present" : "ms/session-tail", s->ticks[lane] * per,
                detailed * per, (s->ticks[lane] - detailed) * per);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
        }
    }
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
        "total_ms=%.8f residual_ms=%.8f land_bytes=%llu land_batches=%llu land_regions=%llu "
        "size_unlisted=%llu size_unlisted_ms=%.8f",
        (unsigned long long)id, partial, (unsigned long long)now_ns(),
        (unsigned long long)w->presents, (unsigned long long)w->rejected,
        w->presents ? "ms/present" : "ms/session-tail", p->bits, (unsigned long long)w->segments,
        (unsigned long long)w->measured, (unsigned long long)w->missing_queries, (unsigned long long)w->missing_spans,
        (unsigned long long)w->dropped, (unsigned long long)w->unranked,
        (unsigned long long)w->missing_gaps, (unsigned long long)w->gaps,
        w->segment_ticks * per, w->gap_ticks * per, w->overlap_ticks * per,
        (w->segment_ticks + w->gap_ticks - w->overlap_ticks) * per, w->residual_ticks * per,
        (unsigned long long)w->land_bytes, (unsigned long long)w->land_batches, (unsigned long long)w->land_regions,
        (unsigned long long)w->size_unlisted, w->size_unlisted_ticks * per);
    for (unsigned k = 0; k < GPU_PT_KINDS; k++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %s_ms=%.8f %s_spans=%llu",
            gpu_pt_names[k], w->ticks[k] * per, gpu_pt_names[k], (unsigned long long)w->counts[k]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);

    for (int i = 0; i < 2; i++) {
        const struct gpu_pt_lane *lane = &w->lane[i];
        len = snprintf(line, sizeof line, "nvk-gpu-pass-time lane window=%llu lane=%s segments=%llu"
            " measured_segments=%llu missing_queries=%llu missing_spans=%llu cap_dropped=%llu segment_ms=%.8f residual_ms=%.8f",
            (unsigned long long)id, i ? "wsi" : "game", (unsigned long long)lane->segments,
            (unsigned long long)lane->measured, (unsigned long long)lane->missing_queries,
            (unsigned long long)lane->missing_spans, (unsigned long long)lane->dropped,
            lane->segment_ticks * per, lane->residual_ticks * per);
        for (unsigned k = 0; k < GPU_PT_KINDS; k++)
            len += snprintf(line + len, sizeof line - (size_t)len, " %s_ms=%.8f %s_spans=%llu",
                gpu_pt_names[k], lane->ticks[k] * per, gpu_pt_names[k], (unsigned long long)lane->counts[k]);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }

    uint64_t ranked = 0;
    for (uint32_t i = 0; i < w->keys && i < GPU_PT_TOP; i++) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < w->keys; j++)
            if (w->ranks[j].ticks > w->ranks[best].ticks) best = j;
        struct gpu_pt_rank r = w->ranks[best];
        w->ranks[best] = w->ranks[i];
        w->ranks[i] = r;
        ranked += r.ticks;
        const struct gpu_pt_key *k = &r.key;
        len = snprintf(line, sizeof line, "nvk-gpu-pass-time top window=%llu rank=%u size=%ux%u layers=%u "
            "colors=%u flags=%u draws=%u occurrences=%llu per_present=%.6f %s=%.8f attachments=",
            (unsigned long long)id, i + 1, k->w, k->h, k->layers, k->colors, k->flags, k->draws,
            (unsigned long long)r.count, w->presents ? (double)r.count / w->presents : 0.0,
            w->presents ? "ms_per_present" : "ms", r.ticks * per);
        gpu_pt_print_attachments(line, sizeof line, len, k);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
    snprintf(line, sizeof line, "nvk-gpu-pass-time ranking window=%llu listed_ms=%.8f unlisted_ms=%.8f",
        (unsigned long long)id, ranked * per, (w->ticks[GPU_PT_RENDER] - ranked) * per);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    gpu_pt_report_sizes(p, id, per);
    memset(w, 0, sizeof *w);
}

static int gpu_pt_read(struct nvn_gpu_vk *vk, const struct vk_segment *s) {
    struct gpu_pt_session *p = vk->gpu_pt;
    struct gpu_pt_window *w = &p->window;
    struct gpu_pt_lane *lane = &w->lane[!!s->internal];
    const struct gpu_pt_cmd *q = s->cmd ? s->cmd->gpu_pt : NULL;
    if (!q) return 0;
    w->dropped += q->dropped;
    lane->dropped += q->dropped;
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
    lane->measured++;
    w->segment_ticks += (uint64_t)duration;
    lane->segment_ticks += (uint64_t)duration;
    uint64_t attributed = 0, offset = 0;
    for (uint32_t i = 0; i < q->count; i++) {
        uint64_t elapsed;
        if (!nvk_pass_time_span(start, raw[1 + 2 * i], raw[2 + 2 * i], (uint64_t)duration, offset,
                                p->bits, &offset, &elapsed)) {
            w->missing_spans++;
            lane->missing_spans++;
            continue;
        }
        enum gpu_pt_kind k = q->spans[i].kind;
        attributed += elapsed;
        w->ticks[k] += elapsed;
        w->counts[k]++;
        lane->ticks[k] += elapsed;
        lane->counts[k]++;
        if (k == GPU_PT_RENDER) {
            gpu_pt_rank(w, &q->spans[i].key, elapsed);
            gpu_pt_render(p, s, i, &q->spans[i], elapsed);
        }
    }
    w->residual_ticks += (uint64_t)duration - attributed;
    lane->residual_ticks += (uint64_t)duration - attributed;
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
    struct gpu_pt_lane *lane = &w->lane[!!s->internal];
    lane->segments++;
    if (s->cmd && s->cmd->gpu_pt_land_batches) {
        w->land_bytes += s->cmd->land_bytes;
        w->land_batches += s->cmd->gpu_pt_land_batches;
        w->land_regions += s->cmd->gpu_pt_land_regions;
    }
    const int measured = success && gpu_pt_read(vk, s);
    if (had_predecessor && !skipped && (!measured || !had_end)) w->missing_gaps++;
    if (!measured) {
        w->missing_queries++;
        lane->missing_queries++;
        p->have_end = 0;
    }
    if (s->internal) {
        const uint64_t attempt = ++p->present_attempts;
        if (s->gpu_pt_presented) {
            const uint64_t present = ++p->presents;
            w->presents++;
            for (uint32_t i = 0; i < w->details; i++)
                if (w->detail[i].present_attempt == attempt) w->detail[i].present = present;
        } else {
            w->rejected++;
        }
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
