// Focused diagnostics for GPU command intervals between existing render passes.
enum { GAP_ANCHORS = 20, GAP_QUERIES = 2 * NVK_GAP_PAIRS + GAP_ANCHORS };
enum { GAP_SOURCE_END, GAP_DEST_BEGIN, GAP_SEG_HEAD, GAP_SEG_TAIL, GAP_INIT_HEAD, GAP_INIT_TAIL };
enum { GAP_DEPTH = 1, GAP_GEOMETRY, GAP_SMALL_POST };
enum { GAP_TAG_PHASE_SHIFT = 2, GAP_TAG_EMIT = 32, GAP_TAG_RUN = 64, GAP_TAG_SEQ_SHIFT = 8 };

struct gap_anchor { uint16_t window; uint8_t event; };
struct gap_part {
    struct nvk_gap_plan plan;
    struct gap_anchor anchors[GAP_ANCHORS];
    VkCommandBuffer cb;
    VkQueryPool pool;
    uint32_t base, nanchors, lost;
};
struct gap_match { uint16_t window; uint8_t focus, shape_ok, park; uint32_t draws; };
struct gap_cmd {
    VkQueryPool pool;
    struct gap_part main, init;
    uint16_t dest, source, dest_pair;
    uint8_t expect_begin, dest_focus;
    uint32_t nmatch;
    struct gap_match matches[GAP_ANCHORS];
};

static struct {
    _Atomic uint64_t tag;
    int enabled;
    uint32_t delay, off_seconds, seconds, bits, w, h, small_w, small_h, depth_size, geom_draws, quad_vertices;
    uint64_t first, phase_seq;
    unsigned phase;
} gap_cfg;
static _Thread_local struct {
    uint64_t tag;
    uint16_t window, next;
    uint8_t focus;
} gap_pending;

void nvk_gap_plan_step(struct nvk_gap_plan *p, enum nvk_gap_kind kind,
                        uint16_t window, nvk_gap_stamp_fn stamp, void *user) {
    if (p->mode == NVK_GAP_OFF || (p->active == kind && p->window == window)) return;
    if (p->active && p->mode == NVK_GAP_TIME && p->current < NVK_GAP_PAIRS)
        stamp(user, 2u * p->current + 1u);
    p->active = (uint8_t)kind;
    p->window = window;
    p->current = NVK_GAP_PAIRS;
    if (!kind) return;
    if (p->n >= NVK_GAP_PAIRS) { p->dropped++; return; }
    p->current = p->n++;
    p->rows[p->current] = (struct nvk_gap_row){(uint8_t)kind, window};
    stamp(user, 2u * p->current);
    if (p->mode == NVK_GAP_EMPTY) stamp(user, 2u * p->current + 1u);
}

static uint32_t gap_shape_arg(const char *name) {
    const int value = nvn_plat_flag(name, 0);
    return value > 0 ? (uint32_t)value : 0;
}

static void gap_open(void) {
    memset(&gap_cfg, 0, sizeof gap_cfg);
    memset(&gap_pending, 0, sizeof gap_pending);
    gap_record_cmd = NULL;
    const int delay = nvn_plat_flag("nvk_gap_s", 0);
    if (delay <= 0 || nvn_plat_flag("nvk_gputime", 0) != 0) return;
    const int seconds = nvn_plat_flag("nvk_gap_seconds", 30);
    const int off = nvn_plat_flag("nvk_gap_off_seconds", 8);
    if (seconds < 3 || seconds > 180 || off < 1 || off > 180) return;
    gap_cfg.delay = (uint32_t)delay;
    gap_cfg.seconds = (uint32_t)seconds;
    gap_cfg.off_seconds = (uint32_t)off;
    gap_cfg.w = gap_shape_arg("nvk_gap_large_w");
    gap_cfg.h = gap_shape_arg("nvk_gap_large_h");
    gap_cfg.small_w = gap_shape_arg("nvk_gap_small_w");
    gap_cfg.small_h = gap_shape_arg("nvk_gap_small_h");
    gap_cfg.depth_size = gap_shape_arg("nvk_gap_depth_size");
    gap_cfg.geom_draws = gap_shape_arg("nvk_gap_geom_draws");
    gap_cfg.quad_vertices = gap_shape_arg("nvk_gap_quad_vertices");
    if (!gap_cfg.w || !gap_cfg.h || !gap_cfg.small_w || !gap_cfg.small_h || !gap_cfg.depth_size ||
        !gap_cfg.geom_draws || !gap_cfg.quad_vertices) {
        nvn_plat_log(NVN_PLAT_LOG_WARN, "gap-gpu", "invalid focus shape");
        return;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.dev->physical, &n, NULL);
    if (g.dev->queue_family >= n || !isfinite(g.dev->properties.limits.timestampPeriod) ||
        g.dev->properties.limits.timestampPeriod <= 0) return;
    VkQueueFamilyProperties *props = calloc(n, sizeof *props);
    if (!props) return;
    vkGetPhysicalDeviceQueueFamilyProperties(g.dev->physical, &n, props);
    if (g.dev->queue_family < n) gap_cfg.bits = props[g.dev->queue_family].timestampValidBits;
    free(props);
    gap_cfg.enabled = gap_cfg.bits > 0 && gap_cfg.bits <= 64;
}

static char *gap_log;
static size_t gap_log_len, gap_log_cap;
static unsigned gap_log_phase;
static int gap_report_started;
static uint32_t gap_log_dropped;

static void gap_flush_log(void) {
    for (size_t i = 0; i < gap_log_len;) {
        const char *line = gap_log + i;
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gap-gpu", line);
        i += strlen(line) + 1;
    }
    if (gap_log_dropped) {
        char line[112];
        snprintf(line, sizeof line, "log_overflow phase=%u lost=%u", gap_log_phase, gap_log_dropped);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "gap-gpu", line);
    }
    gap_log_len = 0;
    gap_log_dropped = 0;
}

static void gap_close(void) {
    gap_flush_log();
    free(gap_log);
    gap_log = NULL;
    gap_log_cap = 0;
    gap_log_phase = 0;
    gap_report_started = 0;
    gap_record_cmd = NULL;
}

static void gap_buffer(const char *line) {
    if (!gap_log) {
        gap_log_cap = 512 * 1024;
        gap_log = malloc(gap_log_cap);
    }
    size_t n = strlen(line) + 1;
    if (!gap_log || n > gap_log_cap - gap_log_len) { gap_log_dropped++; return; }
    memcpy(gap_log + gap_log_len, line, n);
    gap_log_len += n;
}

// Called in WSI submission order. The next guest frame observes a single tag;
// command buffers retain their tag through completion and phase changes.
static void gap_wsi_time(struct vk_segment *s, uint64_t now) {
    if (!gap_cfg.enabled) return;
    s->gap_tag = atomic_load_explicit(&gap_cfg.tag, memory_order_relaxed);
    if (!gap_cfg.first) gap_cfg.first = now;
    const uint64_t elapsed = (now - gap_cfg.first) / UINT64_C(1000000000);
    if (elapsed < gap_cfg.delay) return;
    const uint64_t w = elapsed - gap_cfg.delay;
    const unsigned phase = w < gap_cfg.off_seconds ? 0 : 1 + (unsigned)((w - gap_cfg.off_seconds) / gap_cfg.seconds);
    const unsigned p = phase > 5 ? 5 : phase;
    if (!gap_cfg.phase_seq || p != gap_cfg.phase) { gap_cfg.phase = p; gap_cfg.phase_seq = s->wsi_seq + 1; }
    const uint64_t rel = s->wsi_seq + 1 - gap_cfg.phase_seq;
    const unsigned mode = p && p < 5 ? (p & 1 ? NVK_GAP_TIME : NVK_GAP_EMPTY) : NVK_GAP_OFF;
    const uint64_t tag = ((s->wsi_seq + 1) << GAP_TAG_SEQ_SHIFT) | GAP_TAG_RUN | (p << GAP_TAG_PHASE_SHIFT) |
                         (mode && rel >= 15 && rel < 39 ? GAP_TAG_EMIT : 0) | mode;
    atomic_store_explicit(&gap_cfg.tag, tag, memory_order_release);
}

static void gap_wsi_record(struct vk_segment *s) {
    if (gap_cfg.enabled) gap_wsi_time(s, now_ns());
}

static void gap_stamp(void *user, uint32_t q) {
    const struct gap_part *p = user;
    vkCmdWriteTimestamp(p->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, p->pool, p->base + q);
}
static void gap_step(struct gap_part *p, enum nvk_gap_kind kind, uint16_t id) {
    if (p->cb) nvk_gap_plan_step(&p->plan, kind, id, gap_stamp, p);
}
static void gap_anchor(struct gap_part *p, uint16_t id, unsigned event, VkPipelineStageFlagBits stage) {
    if (!id || !p->cb) return;
    if (p->nanchors >= GAP_ANCHORS) { p->lost++; return; }
    const uint32_t q = p->nanchors++;
    p->anchors[q] = (struct gap_anchor){id, (uint8_t)event};
    vkCmdWriteTimestamp(p->cb, stage, p->pool, p->base + 2 * NVK_GAP_PAIRS + q);
}

static uint16_t gap_window(nvn_gpu_cmd c) {
    return c->gap_tag && c->gap_tag == gap_pending.tag ? gap_pending.window : 0;
}
static void gap_cmd_begin(nvn_gpu_cmd c) {
    if (gap_skip_wsi || !gap_cfg.enabled) return;
    c->gap_tag = atomic_load_explicit(&gap_cfg.tag, memory_order_acquire);
    if (gap_pending.tag != c->gap_tag) {
        memset(&gap_pending, 0, sizeof gap_pending);
        gap_pending.tag = c->gap_tag;
    }
    gap_record_cmd = NULL;
    if (!(c->gap_tag & 3u)) return;
    c->gap = calloc(1, sizeof *c->gap);
    if (!c->gap) return;
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                               .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * GAP_QUERIES};
    if (vkCreateQueryPool(g.d, &qi, NULL, &c->gap->pool) != VK_SUCCESS) {
        free(c->gap);
        c->gap = NULL;
        return;
    }
    c->gap->main.cb = c->cb;
    c->gap->main.pool = c->gap->pool;
    c->gap->main.plan.mode = (uint8_t)(c->gap_tag & 3u);
    vkCmdResetQueryPool(c->cb, c->gap->pool, 0, GAP_QUERIES);
    c->gap_head_window = gap_window(c);
    gap_record_cmd = c;
    gap_anchor(&c->gap->main, c->gap_head_window, GAP_SEG_HEAD, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
}

static void gap_cmd_free(nvn_gpu_cmd c) {
    if (gap_record_cmd == c) gap_record_cmd = NULL;
    if (!c->gap) return;
    vkDestroyQueryPool(g.d, c->gap->pool, NULL);
    free(c->gap);
}
static void gap_main_end(nvn_gpu_cmd c) {
    if (!c->gap) return;
    gap_step(&c->gap->main, NVK_GAP_NONE, 0);
    gap_anchor(&c->gap->main, gap_window(c), GAP_SEG_TAIL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (gap_record_cmd == c) gap_record_cmd = NULL;
}
static void gap_init_start(nvn_gpu_cmd c) {
    if (!c->gap || !c->gap_head_window) return;
    struct gap_part *p = &c->gap->init;
    p->cb = c->init_cb;
    p->pool = c->gap->pool;
    p->base = GAP_QUERIES;
    p->plan.mode = (uint8_t)(c->gap_tag & 3u);
    vkCmdResetQueryPool(p->cb, p->pool, p->base, GAP_QUERIES);
    gap_record_cmd = c;
    gap_anchor(p, c->gap_head_window, GAP_INIT_HEAD, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
}
static void gap_init_end(nvn_gpu_cmd c) {
    if (!c->gap || !c->gap->init.cb) return;
    gap_step(&c->gap->init, NVK_GAP_NONE, 0);
    gap_anchor(&c->gap->init, c->gap_head_window, GAP_INIT_TAIL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (gap_record_cmd == c) gap_record_cmd = NULL;
}
static struct gap_part *gap_op_part(nvn_gpu_cmd c, VkCommandBuffer cb, uint16_t *id) {
    if (!c || !c->gap) return NULL;
    if (cb == c->gap->init.cb) { *id = c->gap_head_window; return &c->gap->init; }
    if (cb == c->cb && !c->in_pass && (*id = gap_window(c))) return &c->gap->main;
    return NULL;
}
static void gap_before_barrier(VkCommandBuffer cb, uint32_t n, const VkImageMemoryBarrier *images) {
    uint16_t id = 0;
    struct gap_part *p = gap_op_part(gap_record_cmd, cb, &id);
    if (!p) return;
    enum nvk_gap_kind kind = NVK_GAP_BARRIER;
    for (uint32_t i = 0; images && i < n; i++)
        if (images[i].oldLayout != images[i].newLayout) { kind = NVK_GAP_LAYOUT; break; }
    gap_step(p, kind, id);
}
static void gap_before_op(VkCommandBuffer cb, unsigned op) {
    uint16_t id = 0;
    nvn_gpu_cmd c = gap_record_cmd;
    struct gap_part *p = gap_op_part(c, cb, &id);
    if (!p) return;
    enum nvk_gap_kind kind;
    switch (op) {
    case OP_vkCmdCopyBuffer: case OP_vkCmdCopyBufferToImage: case OP_vkCmdCopyImageToBuffer:
    case OP_vkCmdCopyImage: case OP_vkCmdBlitImage: case OP_vkCmdResolveImage: case OP_vkCmdUpdateBuffer:
        kind = NVK_GAP_TRANSFER; break;
    case OP_vkCmdFillBuffer: case OP_vkCmdClearColorImage: case OP_vkCmdClearDepthStencilImage:
        kind = NVK_GAP_CLEAR; break;
    case OP_vkCmdDispatch: case OP_vkCmdDispatchIndirect:
        kind = NVK_GAP_COMPUTE; break;
    case OP_vkCmdBeginRenderPass: case OP_vkCmdBeginRendering:
        if (c->gap->expect_begin) return;
        // No observed destination for this candidate: close before an
        // auxiliary pass instead of assigning its work to an outside gap.
        gap_step(p, NVK_GAP_NONE, 0);
        if (p == &c->gap->main) gap_pending.window = 0;
        return;
    default: return;
    }
    gap_step(p, kind, id);
}

static void gap_pass_begin_before(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d) {
    if (!c->gap) return;
    struct gap_cmd *gcmd = c->gap;
    gcmd->dest = gap_window(c);
    gcmd->expect_begin = 1;
    c->gap_draws = c->gap_first_count = 0;
    if (!gcmd->dest) return;
    gcmd->dest_focus = gap_pending.focus;
    uint32_t colors = 0;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) colors += d->color[i].view != NULL;
    int fit = 0;
    switch (gcmd->dest_focus) {
    case GAP_DEPTH: fit = !colors && d->depth.view; break;
    case GAP_GEOMETRY: fit = colors == 4 && d->depth.view && d->w == gap_cfg.w && d->h == gap_cfg.h; break;
    case GAP_SMALL_POST: fit = colors == 1 && !d->depth.view && d->w == gap_cfg.small_w && d->h == gap_cfg.small_h; break;
    }
    if (gcmd->nmatch < GAP_ANCHORS) {
        gcmd->matches[gcmd->nmatch++] = (struct gap_match){gcmd->dest, gcmd->dest_focus, (uint8_t)fit, 0, 0};
    } else gcmd->main.lost++;
    gap_step(&gcmd->main, NVK_GAP_NONE, 0);
    gap_anchor(&gcmd->main, gcmd->dest, GAP_DEST_BEGIN, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    gcmd->dest_pair = gcmd->main.plan.n;
    gap_step(&gcmd->main, NVK_GAP_PASS_BEGIN, gcmd->dest);
    gap_pending.window = 0;
}

static void gap_pass_begin_after(nvn_gpu_cmd c) {
    if (!c->gap) return;
    c->gap->expect_begin = 0;
    gap_step(&c->gap->main, NVK_GAP_NONE, 0);
}

static void gap_reason(nvn_gpu_cmd c, const char *reason) {
    if (!c || !c->gap || !c->gap->dest || !reason || strcmp(reason, "park-flush")) return;
    struct gap_cmd *p = c->gap;
    if (p->nmatch && p->matches[p->nmatch - 1].window == p->dest) p->matches[p->nmatch - 1].park = 1;
    if (p->dest_pair < p->main.plan.n) p->main.plan.rows[p->dest_pair].kind = NVK_GAP_PARK;
}

static void gap_pass_end_before(nvn_gpu_cmd c) {
    if (!c->gap) return;
    struct gap_cmd *p = c->gap;
    if (p->dest && p->nmatch && p->matches[p->nmatch - 1].window == p->dest)
        p->matches[p->nmatch - 1].draws = c->gap_draws;
    p->dest = 0;
    const unsigned colors = (unsigned)__builtin_popcount(c->pass_color_mask);
    const int large = c->pass_w == gap_cfg.w && c->pass_h == gap_cfg.h;
    const int quad = c->gap_draws == 1 && c->gap_first_count == gap_cfg.quad_vertices;
    uint8_t focus = 0;
    if (quad && !colors && c->pass_dsfmt == NVN_FORMAT_DEPTH16 &&
        c->pass_w == gap_cfg.depth_size && c->pass_h == gap_cfg.depth_size) focus = GAP_DEPTH;
    else if (large && colors == 4 && c->pass_dsfmt && c->gap_draws == gap_cfg.geom_draws) focus = GAP_GEOMETRY;
    else if (large && quad && colors == 1 && !c->pass_dsfmt) {
        for (unsigned i = 0; i < 8; i++)
            if (c->pass_cfmt[i] == NVN_FORMAT_R11G11B10F) focus = GAP_SMALL_POST;
    }
    p->source = 0;
    if (!focus || c->gap_tag != gap_pending.tag) return;
    if (gap_pending.next == UINT16_MAX) { p->main.lost++; return; }
    p->source = ++gap_pending.next;
    gap_pending.focus = focus;
    gap_step(&p->main, NVK_GAP_PASS_END, p->source);
}

static void gap_pass_end_after(nvn_gpu_cmd c) {
    if (!c->gap || !c->gap->source) return;
    struct gap_cmd *p = c->gap;
    gap_step(&p->main, NVK_GAP_NONE, 0);
    gap_anchor(&p->main, p->source, GAP_SOURCE_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    gap_pending.window = p->source;
}

static void gap_part_done(const struct gap_part *p, uint64_t seq, uint64_t signal, unsigned part, int failed) {
    if (!p->cb || (!p->nanchors && !p->plan.n && !p->lost && !p->plan.dropped)) return;
    uint64_t q[GAP_QUERIES][2] = {{0}};
    if (!failed && p->plan.n)
        (void)vkGetQueryPoolResults(g.d, p->pool, p->base, p->plan.n * 2,
                                    p->plan.n * 2 * sizeof q[0], q, sizeof q[0],
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (!failed && p->nanchors)
        (void)vkGetQueryPoolResults(g.d, p->pool, p->base + 2 * NVK_GAP_PAIRS, p->nanchors,
                                    p->nanchors * sizeof q[0], q[2 * NVK_GAP_PAIRS], sizeof q[0],
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    char line[256];
    for (uint32_t i = 0; i < p->plan.n; i++) {
        snprintf(line, sizeof line, "range frame=%llu submit=%llu cb=%u id=%u kind=%u valid=%u t0=%llu t1=%llu",
                 (unsigned long long)seq, (unsigned long long)signal, part, p->plan.rows[i].window, p->plan.rows[i].kind,
                 q[2*i][1] && q[2*i+1][1], (unsigned long long)q[2*i][0], (unsigned long long)q[2*i+1][0]);
        gap_buffer(line);
    }
    for (uint32_t i = 0; i < p->nanchors; i++) {
        const uint64_t *x = q[2 * NVK_GAP_PAIRS + i];
        snprintf(line, sizeof line, "anchor frame=%llu submit=%llu id=%u event=%u valid=%u tick=%llu",
                 (unsigned long long)seq, (unsigned long long)signal, p->anchors[i].window, p->anchors[i].event,
                 x[1] != 0, (unsigned long long)x[0]);
        gap_buffer(line);
    }
    if (p->lost || p->plan.dropped) {
        snprintf(line, sizeof line, "missing frame=%llu submit=%llu cb=%u lost=%u",
                 (unsigned long long)seq, (unsigned long long)signal, part, p->lost + p->plan.dropped);
        gap_buffer(line);
    }
}

static void gap_segment_done(const struct vk_segment *s, int failed) {
    if (!gap_cfg.enabled) return;
    const uint64_t tag = s->internal ? s->gap_tag : s->cmd ? s->cmd->gap_tag : 0;
    if (!(tag & GAP_TAG_RUN)) return;
    const unsigned phase = (unsigned)(tag >> GAP_TAG_PHASE_SHIFT) & 7;
    const unsigned mode = (unsigned)tag & 3;
    const uint64_t seq = tag >> GAP_TAG_SEQ_SHIFT;
    char line[320];
    if (s->internal) {
        if (!gap_report_started || phase != gap_log_phase) {
            gap_flush_log();
            gap_log_phase = phase;
            gap_report_started = 1;
            snprintf(line, sizeof line, "phase=%u mode=%u frame=%llu period_ns=%.9g bits=%u",
                     phase, mode, (unsigned long long)seq, (double)g.dev->properties.limits.timestampPeriod, gap_cfg.bits);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "gap-gpu", line);
        }
        if (tag & GAP_TAG_EMIT) {
            snprintf(line, sizeof line, "frame=%llu phase=%u mode=%u wsi=%llu failed=%u", (unsigned long long)seq,
                     phase, mode, (unsigned long long)s->wsi_seq, failed != 0 || seq != s->wsi_seq);
            gap_buffer(line);
        }
        return;
    }
    if (!(tag & GAP_TAG_EMIT)) return;
    const struct gap_cmd *c = s->cmd->gap;
    if (!c) {
        snprintf(line, sizeof line, "missing frame=%llu submit=%llu cb=0 lost=1", (unsigned long long)seq, (unsigned long long)s->signal);
        gap_buffer(line);
        return;
    }
    if (!c->main.plan.n && !c->main.nanchors && !c->nmatch && !c->init.nanchors) return;
    snprintf(line, sizeof line, "submit frame=%llu signal=%llu cpu_submit_ns=%llu cpu_done_ns=%llu failed=%u",
             (unsigned long long)seq, (unsigned long long)s->signal, (unsigned long long)s->gap_cpu_submit,
             (unsigned long long)now_ns(), failed != 0);
    gap_buffer(line);
    gap_part_done(&c->init, seq, s->signal, 1, failed);
    gap_part_done(&c->main, seq, s->signal, 0, failed);
    for (uint32_t i = 0; i < c->nmatch; i++) {
        const struct gap_match *m = &c->matches[i];
        snprintf(line, sizeof line, "match frame=%llu submit=%llu id=%u focus=%u shape=%u park=%u draws=%u",
                 (unsigned long long)seq, (unsigned long long)s->signal, m->window, m->focus, m->shape_ok, m->park, m->draws);
        gap_buffer(line);
    }
}

#ifdef S2A_NVK_GAP_HOST_TEST
void nvk_gap_test_present(uint64_t seq, uint64_t elapsed_seconds) {
    struct vk_segment s = {.internal = 1, .wsi_seq = seq};
    gap_wsi_time(&s, elapsed_seconds * UINT64_C(1000000000));
    gap_segment_done(&s, 0);
}
#endif
