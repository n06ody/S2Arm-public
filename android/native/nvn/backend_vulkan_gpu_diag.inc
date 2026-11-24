// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Physical Vulkan pass and feedback diagnostics. Read results on the existing
// completion worker, after the fence, with no new GPU wait.
static void gpu_diag_open(void) {
    g.gputime = nvn_plat_flag("nvk_gputime", 0) != 0;
    g.pass_diag = nvn_plat_flag("nvk_gpu_passes", 0) != 0;
    g.fb_diag = nvn_plat_flag("nvk_fb_snapshots", 0) != 0;
    if (!g.gputime && !g.pass_diag && !g.idle_on) return;
    g.ts_period = g.dev->properties.limits.timestampPeriod;
    if (isfinite(g.ts_period) && g.ts_period > 0) g.ts_bits = queue_timestamp_bits(g.dev);
    if (g.idle_on) g.idle_bits = g.ts_bits;
    if (!g.ts_bits)
        nvn_plat_log(NVN_PLAT_LOG_WARN, "gpu", "nvk-pass timestamps unavailable on the selected Vulkan queue");
    else if (g.pass_diag) {
        char line[160];
        snprintf(line, sizeof line, "nvk-pass-clock queue=%u valid_bits=%u period_ns=%.9g", g.dev->queue_family,
                 g.ts_bits, g.ts_period);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
}

static uint32_t gpu_diag_mip(uint32_t size, uint32_t level) {
    size >>= level;
    return size ? size : 1;
}

static struct gpu_pass_meta *gpu_diag_record_pass(struct nvn_gpu_cmd_s *c, uint32_t w, uint32_t h, uint32_t layers) {
    c->diag_npasses++;
    if (!c->tsq || c->ntsp >= TS_PASS_MAX) return NULL;
    if (c->ntsp >= c->diag_pass_cap) {
        uint32_t cap = c->diag_pass_cap ? 2 * c->diag_pass_cap : 8;
        if (cap > TS_PASS_MAX) cap = TS_PASS_MAX;
        struct gpu_pass_meta *mem = realloc(c->diag_passes, cap * sizeof *mem);
        if (!mem) return NULL;
        // Holes after a failed growth must have known=0 even if a later allocation succeeds.
        memset(mem + c->diag_pass_cap, 0, (cap - c->diag_pass_cap) * sizeof *mem);
        c->diag_passes = mem;
        c->diag_pass_cap = cap;
    }
    struct gpu_pass_meta *p = &c->diag_passes[c->ntsp];
    *p = (struct gpu_pass_meta){.w = w, .h = h, .layers = layers};
    return p;
}

static void gpu_diag_attachment(struct gpu_pass_meta *p, uint32_t slot, const VkAttachmentDescription *att,
                                uint32_t w, uint32_t h) {
    p->att[p->natt++] = (struct gpu_diag_att){
        .slot = slot, .format = att->format, .samples = att->samples, .w = w, .h = h,
        .load = att->loadOp, .store = att->storeOp,
        .stencil_load = att->stencilLoadOp, .stencil_store = att->stencilStoreOp,
    };
}

// Called immediately before vkCmdBeginRenderPass. Attachment descriptions are
// the ones used by Vulkan, including any LOAD/STORE overrides on a split.
static inline void gpu_diag_pass_begin(struct nvn_gpu_cmd_s *c, const nvn_gpu_pass_desc *d,
                                       const VkAttachmentDescription *att, uint32_t natt, int has_depth) {
    if (g.pass_diag) {
        struct gpu_pass_meta *p = gpu_diag_record_pass(c, d->w, d->h, d->layers ? d->layers : 1);
        if (p) {
            uint32_t a = 0;
            for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
                const struct nvn_gpu_view_s *v = d->color[i].view;
                if (v) gpu_diag_attachment(p, i, &att[a++],
                    gpu_diag_mip(v->tex->w, v->base_level), gpu_diag_mip(v->tex->h, v->base_level));
            }
            if (has_depth) {
                const struct nvn_gpu_view_s *v = d->depth.view ? d->depth.view : d->stencil.view;
                gpu_diag_attachment(p, 8, &att[a],
                    gpu_diag_mip(v->tex->w, v->base_level), gpu_diag_mip(v->tex->h, v->base_level));
            }
        }
    }
    if ((g.gputime || g.pass_diag) && c->tsq && c->ntsp < TS_PASS_MAX) {
        c->tsp[c->ntsp].w = (uint16_t)d->w;
        c->tsp[c->ntsp].h = (uint16_t)d->h;
        c->tsp[c->ntsp].ncol = (uint8_t)(natt - has_depth);
        c->tsp[c->ntsp].ds = (uint8_t)has_depth;
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 2 + 2 * c->ntsp);
    }
}

static inline void gpu_diag_pass_end(struct nvn_gpu_cmd_s *c) {
    if ((g.gputime || g.pass_diag) && c->tsq && c->ntsp < TS_PASS_MAX) {
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 3 + 2 * c->ntsp);
        c->ntsp++;
    }
}

static int gpu_diag_elapsed(const struct nvn_gpu_vk *vk, uint64_t start, uint64_t end, uint64_t *ns) {
    // A negative modular difference is ambiguous. Mark it unavailable.
    const int64_t ticks = s2a_ts_wrap_diff(end, start, vk->ts_bits);
    const double value = ticks * vk->ts_period;
    if (value < 0 || value >= 0x1p64) return 0;
    *ns = (uint64_t)value;
    return 1;
}

static void gpu_diag_rank(struct gpu_diag_frame *frame, uint64_t signal, int internal, uint32_t pass,
                          uint64_t ns, const struct gpu_pass_meta *meta) {
    uint32_t i = 0;
    while (i < frame->ntop && frame->top[i].ns >= ns) i++;
    if (i == GPU_DIAG_TOP) return;
    if (frame->ntop < GPU_DIAG_TOP) frame->ntop++;
    memmove(frame->top + i + 1, frame->top + i, (frame->ntop - i - 1) * sizeof frame->top[0]);
    frame->top[i] = (struct gpu_diag_rank){.signal = signal, .internal = internal, .index = pass, .ns = ns};
    if (meta) frame->top[i].meta = *meta;
}

static int gpu_diag_segment_ticks(struct nvn_gpu_vk *vk, const struct vk_segment *s, uint64_t ticks[2]) {
    const struct nvn_gpu_cmd_s *c = s->cmd;
    if (!c || !c->tsq || (s->internal && !vk->pass_diag && !vk->idle_bits)) return 0;
    if (vkGetQueryPoolResults(vk->d, c->tsq, 0, 2, 2 * sizeof ticks[0], ticks, sizeof ticks[0],
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    if (!c->diag_prefix) return 1;
    return vkGetQueryPoolResults(vk->d, c->tsq, c->diag_prefix, 1, sizeof ticks[0], ticks,
                                 sizeof ticks[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
}

static void gpu_diag_pass_retire(struct nvn_gpu_vk *vk, const struct vk_segment *s, uint64_t id,
                                 const uint64_t ticks[2], int timed_segment) {
    const struct nvn_gpu_cmd_s *c = s->cmd;
    struct gpu_diag_frame *frame = &vk->diag_frame;
    uint64_t submit_ns = 0;
    const int submit_ok = timed_segment && gpu_diag_elapsed(vk, ticks[0], ticks[1], &submit_ns);

    uint32_t timed = 0;
    uint64_t sum = 0, pts[2 * TS_PASS_MAX];
    char line[1024];
    // No WAIT_BIT: if results are unavailable after completion, report missing
    // coverage instead of logging a zero or stalling the completion thread.
    if (c->ntsp && vkGetQueryPoolResults(vk->d, c->tsq, 2, 2 * c->ntsp,
            2 * c->ntsp * sizeof pts[0], pts, sizeof pts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        for (uint32_t first = 0; first < c->ntsp; first += 16) {
            size_t off = (size_t)snprintf(line, sizeof line, "nvk-pass-ms frame=%llu submit=%llu internal=%d first=%u ms=",
                (unsigned long long)id, (unsigned long long)s->signal, s->internal, first);
            for (uint32_t i = first; i < c->ntsp && i < first + 16; i++) {
                uint64_t ns;
                if (gpu_diag_elapsed(vk, pts[2 * i], pts[2 * i + 1], &ns)) {
                    sum += ns;
                    timed++;
                    gpu_diag_rank(frame, s->signal, s->internal, i, ns, i < c->diag_pass_cap ? &c->diag_passes[i] : NULL);
                    off += (size_t)snprintf(line + off, sizeof line - off, "%s%.6f", i == first ? "" : ",", ns / 1e6);
                } else {
                    off += (size_t)snprintf(line + off, sizeof line - off, "%sNA", i == first ? "" : ",");
                }
            }
            nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
        }
    }
    snprintf(line, sizeof line, "nvk-pass-segment frame=%llu submit=%llu internal=%d passes=%u timed=%u pass_ms=%.6f "
             "submit_timed=%d prefix=%d begin_ticks=%llu end_ticks=%llu submit_ms=%.6f",
             (unsigned long long)id, (unsigned long long)s->signal, s->internal, c->diag_npasses, timed,
             sum / 1e6, submit_ok, !!c->diag_prefix,
             (unsigned long long)ticks[0], (unsigned long long)ticks[1], submit_ns / 1e6);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    frame->passes += c->diag_npasses;
    frame->timed += timed;
    frame->pass_ns += sum;
    frame->submit_ns += submit_ns;
    frame->submit_timed += submit_ok;
}

static void gpu_diag_feedback_retire(struct gpu_diag_frame *frame, const struct vk_segment *s, uint64_t id) {
    const struct fb_diag_cmd *c = s->cmd->diag_fb;
    if (!c) {
        frame->fb_missing++;
        return;
    }
    frame->fb.snapshots += c->total.snapshots;
    frame->fb.regions += c->total.regions;
    frame->fb.bytes += c->total.bytes;
    frame->fb.unknown += c->total.unknown;
    frame->fb_unattributed += c->unattributed;
    for (uint32_t i = 0; i < c->nsites; i++) {
        const struct fb_diag_site *site = &c->sites[i];
        char line[300];
        snprintf(line, sizeof line, "nvk-fb-site frame=%llu submit=%llu pipe=%08x stage=%u set=%u binding=%u "
                 "pool=%u snapshots=%llu regions=%llu bytes=%llu unknown=%llu",
                 (unsigned long long)id, (unsigned long long)s->signal, site->pipe, site->stage, site->set,
                 site->binding, site->pool, (unsigned long long)site->total.snapshots,
                 (unsigned long long)site->total.regions, (unsigned long long)site->total.bytes, (unsigned long long)site->total.unknown);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
}

static void gpu_diag_retire(struct nvn_gpu_vk *vk, const struct vk_segment *s, const uint64_t ticks[2], int timed) {
    if (!s->cmd) return;
    const uint64_t id = vk->wsi_done + 1;   // the completion worker owns wsi_done writes
    vk->diag_frame.segments++;
    if (vk->pass_diag) gpu_diag_pass_retire(vk, s, id, ticks, timed);
    if (vk->fb_diag) gpu_diag_feedback_retire(&vk->diag_frame, s, id);
}

static void gpu_diag_frame_end(struct nvn_gpu_vk *vk, uint64_t id, int partial) {
    const struct gpu_diag_frame *f = &vk->diag_frame;
    char line[1536];
    if (vk->pass_diag) {
        snprintf(line, sizeof line, "nvk-pass-frame frame=%llu partial=%d segments=%llu submit_timed=%llu submit_ms=%.6f "
                 "passes=%llu timed=%llu pass_ms=%.6f",
                 (unsigned long long)id, partial, (unsigned long long)f->segments, (unsigned long long)f->submit_timed,
                 f->submit_ns / 1e6, (unsigned long long)f->passes, (unsigned long long)f->timed, f->pass_ns / 1e6);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
        for (uint32_t i = 0; i < f->ntop; i++) {
            const struct gpu_diag_rank *r = &f->top[i];
            const struct gpu_pass_meta *p = &r->meta;
            size_t off = (size_t)snprintf(line, sizeof line, "nvk-pass-top frame=%llu rank=%u submit=%llu internal=%d "
                "pass=%u ms=%.6f known=%d size=%ux%u layers=%u", (unsigned long long)id, i + 1,
                (unsigned long long)r->signal, r->internal, r->index, r->ns / 1e6, p->w != 0, p->w, p->h, p->layers);
            for (uint32_t a = 0; a < p->natt; a++) {
                const struct gpu_diag_att *d = &p->att[a];
                off += (size_t)snprintf(line + off, sizeof line - off,
                    " [slot=%u fmt=%d extent=%ux%u samples=%u load=%d store=%d stencil_load=%d stencil_store=%d]",
                    d->slot, d->format, d->w, d->h, d->samples, d->load, d->store, d->stencil_load, d->stencil_store);
            }
            nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
        }
    }
    if (vk->fb_diag) {
        snprintf(line, sizeof line, "nvk-fb-frame frame=%llu partial=%d snapshots=%llu regions=%llu bytes=%llu unknown=%llu "
                 "unattributed=%llu missing_segments=%llu", (unsigned long long)id, partial,
                 (unsigned long long)f->fb.snapshots, (unsigned long long)f->fb.regions, (unsigned long long)f->fb.bytes,
                 (unsigned long long)f->fb.unknown, (unsigned long long)f->fb_unattributed, (unsigned long long)f->fb_missing);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
    memset(&vk->diag_frame, 0, sizeof vk->diag_frame);
}
