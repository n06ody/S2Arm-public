// Full-frame Vulkan timing in a diagnostic session. The ordinary pass anchors
// use the same command positions as nvk_gputime. Category pairs are additive.
enum { GAP_RANGES = 2 * NVK_GAP_PAIRS, GAP_PART = GAP_RANGES + 2, GAP_REGISTRY = 8 };
enum { GAP_PHASE_SHIFT = 2, GAP_PHASE_MASK = 15, GAP_EMIT = 64, GAP_RUN = 128, GAP_CALIBRATE = 256, GAP_SEQ_SHIFT = 9 };
enum { GAP_SAMPLE_FIRST = 15, GAP_SAMPLE_COUNT = 25 };

struct gap_part {
    VkCommandBuffer cb;
    VkQueryPool pool;
    uint32_t base;
    uint8_t in_render;
    struct nvk_gap_plan plan;
};

struct gap_cmd {
    VkQueryPool pool;
    struct gap_part main, init;
    unsigned registry_lost;
};

static struct {
    _Atomic uint64_t tag;
    uint64_t first, phase_seq;
    unsigned phase, delay, off_seconds, arm_seconds, bits;
    int enabled;
    unsigned edge_windows;
} gap_cfg;

// A replay thread can encode multiple active command buffers, notably the
// preamble and main command from the same NVN submission. Lookup never
// attributes a command to the most recently opened buffer.
static _Atomic unsigned gap_generation;
static _Thread_local unsigned gap_registry_generation;
static _Thread_local struct gap_part *gap_reg[GAP_REGISTRY];
static char *gap_log;
static size_t gap_log_len;
static unsigned gap_log_phase, gap_log_dropped;
static int gap_report_started;

static void gap_registry_check(void) {
    unsigned epoch = atomic_load_explicit(&gap_generation, memory_order_relaxed);
    if (gap_registry_generation == epoch) return;
    memset(gap_reg, 0, sizeof gap_reg);
    gap_registered = 0;
    gap_registry_generation = epoch;
}

static int gap_insert(struct gap_part *p) {
    gap_registry_check();
    if (gap_registered == GAP_REGISTRY) return 0;
    gap_reg[gap_registered++] = p;
    return 1;
}

static void gap_remove(struct gap_part *p) {
    gap_registry_check();
    for (unsigned i = 0; i < gap_registered; i++) {
        if (gap_reg[i] != p) continue;
        gap_reg[i] = gap_reg[--gap_registered];
        return;
    }
}

static struct gap_part *gap_lookup(VkCommandBuffer cb) {
    gap_registry_check();
    for (unsigned i = gap_registered; i; i--) {
        struct gap_part *p = gap_reg[i - 1];
        if (p->cb == cb) return p;
    }
    return NULL;
}

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

static void gap_open(void) {
    memset(&gap_cfg, 0, sizeof gap_cfg);
    atomic_fetch_add_explicit(&gap_generation, 1, memory_order_relaxed);
    const int delay = nvn_plat_flag("nvk_gap_s", 0);
    gap_cfg.edge_windows = narrow_timing_windows();
    if ((!gap_cfg.edge_windows && delay <= 0) || nvn_plat_flag("nvk_gputime", 0) != 0) return;
    const int arm = nvn_plat_flag("nvk_gap_seconds", 30);
    const int off = nvn_plat_flag("nvk_gap_off_seconds", 8);
    if (!gap_cfg.edge_windows && (arm < 3 || arm > 180 || off < 1 || off > 180)) return;
    if (!isfinite(g.dev->properties.limits.timestampPeriod) || g.dev->properties.limits.timestampPeriod <= 0) return;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.dev->physical, &n, NULL);
    if (g.dev->queue_family >= n) return;
    VkQueueFamilyProperties *props = calloc(n, sizeof *props);
    if (!props) return;
    vkGetPhysicalDeviceQueueFamilyProperties(g.dev->physical, &n, props);
    if (g.dev->queue_family < n) gap_cfg.bits = props[g.dev->queue_family].timestampValidBits;
    free(props);

    gap_cfg.delay = (unsigned)delay;
    gap_cfg.off_seconds = (unsigned)off;
    gap_cfg.arm_seconds = (unsigned)arm;
    gap_cfg.enabled = gap_cfg.bits > 0 && gap_cfg.bits <= 64;
}

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
    gap_report_started = 0;
    atomic_fetch_add_explicit(&gap_generation, 1, memory_order_relaxed);
}

static void gap_buffer(const char *line) {
    enum { LOG_CAP = 4 * 1024 * 1024 };
    if (!gap_log) gap_log = malloc(LOG_CAP);
    const size_t n = strlen(line) + 1;
    if (!gap_log || n > LOG_CAP - gap_log_len) { gap_log_dropped++; return; }
    memcpy(gap_log + gap_log_len, line, n);
    gap_log_len += n;
}

// Called under the submit lock. The WSI submission keeps the same tag as all
// preceding ordinary submissions. Only later submissions see the next tag.
// In the barrier experiment, timestamps enclose just the submit and pass
// edges of the same set of sampled frames in every treatment window.
static void gap_narrow_edges(const struct vk_segment *s) {
    const unsigned phase = narrow_phase();
    if (!phase) return;
    if (!gap_cfg.phase_seq || phase != gap_cfg.phase) {
        gap_cfg.phase = phase;
        gap_cfg.phase_seq = s->wsi_seq + 1;
    }
    const uint64_t frame = s->wsi_seq + 1;
    const uint64_t rel = frame - gap_cfg.phase_seq;
    const unsigned sample = phase <= gap_cfg.edge_windows &&
        rel >= GAP_SAMPLE_FIRST && rel < GAP_SAMPLE_FIRST + GAP_SAMPLE_COUNT;
    const uint64_t tag = (frame << GAP_SEQ_SHIFT) | GAP_RUN | (phase << GAP_PHASE_SHIFT) |
                         (sample ? GAP_EMIT | NVK_GAP_TIME : NVK_GAP_OFF);
    atomic_store_explicit(&gap_cfg.tag, tag, memory_order_release);
}

static void gap_wsi_time(struct vk_segment *s, uint64_t now) {
    if (!gap_cfg.enabled) return;
    s->gap_tag = atomic_load_explicit(&gap_cfg.tag, memory_order_relaxed);
    if (gap_cfg.edge_windows) { gap_narrow_edges(s); return; }
    if (!gap_cfg.first) gap_cfg.first = now;
    const uint64_t elapsed = (now - gap_cfg.first) / UINT64_C(1000000000);
    if (elapsed < gap_cfg.delay) return;
    const uint64_t since = elapsed - gap_cfg.delay;
    const uint64_t phase = since < gap_cfg.off_seconds ? 0 : 1 + (since - gap_cfg.off_seconds) / gap_cfg.arm_seconds;
    const unsigned p = phase > 5 ? 5 : (unsigned)phase;
    if (!gap_cfg.phase_seq || p != gap_cfg.phase) {
        gap_cfg.phase = p;
        gap_cfg.phase_seq = s->wsi_seq + 1;
    }
    const uint64_t rel = s->wsi_seq + 1 - gap_cfg.phase_seq;
    const unsigned mode = p && p < 5 ? (p & 1 ? NVK_GAP_TIME : NVK_GAP_EMPTY) : NVK_GAP_OFF;
    const unsigned emit = mode && rel >= GAP_SAMPLE_FIRST && rel < GAP_SAMPLE_FIRST + GAP_SAMPLE_COUNT ? GAP_EMIT : 0;
    const unsigned calibration = emit && (rel == GAP_SAMPLE_FIRST || rel + 1 == GAP_SAMPLE_FIRST + GAP_SAMPLE_COUNT)
                                 ? GAP_CALIBRATE : 0;
    const uint64_t tag = ((s->wsi_seq + 1) << GAP_SEQ_SHIFT) | GAP_RUN |
                         (p << GAP_PHASE_SHIFT) | emit | calibration | mode;
    atomic_store_explicit(&gap_cfg.tag, tag, memory_order_release);
}

static void gap_wsi_record(struct vk_segment *s) {
    if (gap_cfg.enabled) gap_wsi_time(s, s->gap_cpu_return);
}

static VkResult gap_queue_submit(struct vk_segment *s, const VkSubmitInfo *si, VkFence fence) {
    if (!gap_cfg.enabled) return vkQueueSubmit(g.queue, 1, si, fence);
    s->gap_tag = atomic_load_explicit(&gap_cfg.tag, memory_order_relaxed);
    s->gap_cpu_submit = now_ns();
    VkResult rc = vkQueueSubmit(g.queue, 1, si, fence);
    s->gap_cpu_return = now_ns();
    return rc;
}

static void gap_stamp(void *user, uint32_t q) {
    const struct gap_part *p = user;
    vkCmdWriteTimestamp(p->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, p->pool, p->base + q);
}

static void gap_step(struct gap_part *p, enum nvk_gap_kind kind) {
    nvk_gap_plan_step(&p->plan, kind, 0, gap_stamp, p);
}

static void gap_begin_part(nvn_gpu_cmd c, struct gap_part *p, VkCommandBuffer cb, unsigned init) {
    *p = (struct gap_part){.cb = cb, .pool = c->gap->pool, .base = init ? GAP_PART : 0,
                           .plan.mode = (uint8_t)(c->gap_tag & 3)};
    if (gap_cfg.edge_windows) {
        if (init) vkCmdResetQueryPool(cb, p->pool, p->base + GAP_RANGES, 2);
        return;
    }
    vkCmdResetQueryPool(cb, p->pool, p->base, GAP_PART);
    if (!gap_insert(p)) c->gap->registry_lost++;
}

static void gap_cmd_begin(nvn_gpu_cmd c) {
    if (!gap_cfg.enabled) return;
    const uint64_t tag = atomic_load_explicit(&gap_cfg.tag, memory_order_acquire);
    if (!(tag & 3) || c->tsq) return;
    c->gap_tag = tag;
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                               .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 + 2 * TS_PASS_MAX};
    VkQueryPool pool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(g.d, &qi, NULL, &pool) != VK_SUCCESS) return;
    c->tsq = pool;
    vkCmdResetQueryPool(c->cb, c->tsq, 0, qi.queryCount);
    vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->tsq, 0);
    c->gap = calloc(1, sizeof *c->gap);
    if (!c->gap) return;
    qi.queryCount = 2 * GAP_PART;
    if (vkCreateQueryPool(g.d, &qi, NULL, &c->gap->pool) != VK_SUCCESS) {
        free(c->gap);
        c->gap = NULL;
        return;
    }
    gap_begin_part(c, &c->gap->main, c->cb, 0);
}

static void gap_unbind_main(nvn_gpu_cmd c) {
    if (c->gap) gap_remove(&c->gap->main);
}

static void gap_cmd_free(nvn_gpu_cmd c) {
    if (!c->gap) return;
    gap_unbind_main(c);
    vkDestroyQueryPool(g.d, c->gap->pool, NULL);
    free(c->gap);
}

// Close the preceding category before the legacy pass-start anchor. The
// actual vkCmdBegin edge gets a separate interval inside the pass envelope.
static void gap_pass_boundary(nvn_gpu_cmd c) {
    if (c->gap) gap_step(&c->gap->main, NVK_GAP_NONE);
}

static void gap_main_end(nvn_gpu_cmd c) {
    gap_pass_boundary(c);
    gap_unbind_main(c);
}

static void gap_init_start(nvn_gpu_cmd c) {
    c->gap_has_init = c->gap_tag != 0;
    if (!c->gap) return;
    struct gap_part *p = &c->gap->init;
    gap_begin_part(c, p, c->init_cb, 1);
    vkCmdWriteTimestamp(p->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, p->pool, p->base + GAP_RANGES);
}

static void gap_init_end(nvn_gpu_cmd c) {
    if (!c->gap) return;
    struct gap_part *p = &c->gap->init;
    gap_step(p, NVK_GAP_NONE);
    vkCmdWriteTimestamp(p->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, p->pool, p->base + GAP_RANGES + 1);
    gap_remove(p);
}

static void gap_before_barrier(VkCommandBuffer cb, uint32_t n, const VkImageMemoryBarrier *images) {
    struct gap_part *p = gap_lookup(cb);
    if (!p || p->in_render) return;
    for (uint32_t i = 0; images && i < n; i++) {
        if (images[i].oldLayout == images[i].newLayout) continue;
        gap_step(p, NVK_GAP_LAYOUT);
        return;
    }
    gap_step(p, NVK_GAP_BARRIER);
}

static void gap_before_op(VkCommandBuffer cb, unsigned op) {
    enum nvk_gap_kind kind;
    switch (op) {
    case OP_vkCmdCopyBuffer: case OP_vkCmdCopyBufferToImage: case OP_vkCmdCopyImageToBuffer:
    case OP_vkCmdCopyImage: case OP_vkCmdBlitImage: case OP_vkCmdResolveImage: case OP_vkCmdUpdateBuffer:
        kind = NVK_GAP_TRANSFER; break;
    case OP_vkCmdFillBuffer: case OP_vkCmdClearColorImage: case OP_vkCmdClearDepthStencilImage:
        kind = NVK_GAP_CLEAR; break;
    case OP_vkCmdDispatch: case OP_vkCmdDispatchIndirect:
        kind = NVK_GAP_COMPUTE; break;
    default: return;
    }
    struct gap_part *p = gap_lookup(cb);
    if (p && !p->in_render) gap_step(p, kind);
}

static void gap_render_edge(VkCommandBuffer cb, int begin, int after) {
    struct gap_part *p = gap_lookup(cb);
    if (!p) return;
    if (after) {
        gap_step(p, NVK_GAP_NONE);
        p->in_render = (uint8_t)begin;
    } else gap_step(p, begin ? NVK_GAP_PASS_BEGIN : NVK_GAP_PASS_END);
}

static void gap_rows(const struct gap_part *p, unsigned long long seq, unsigned long long signal, unsigned part) {
    if (!p->cb) return;
    uint64_t q[GAP_RANGES][2] = {{0}};
    if (p->plan.n)
        (void)vkGetQueryPoolResults(g.d, p->pool, p->base, 2 * p->plan.n,
                                    2 * p->plan.n * sizeof q[0], q, sizeof q[0],
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    char line[256];
    if (part) {
        uint64_t t[2][2] = {{0}};
        (void)vkGetQueryPoolResults(g.d, p->pool, p->base + GAP_RANGES, 2, sizeof t, t, sizeof t[0],
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        snprintf(line, sizeof line, "init frame=%llu submit=%llu valid=%u t0=%llu t1=%llu",
                 seq, signal, t[0][1] && t[1][1], (unsigned long long)t[0][0], (unsigned long long)t[1][0]);
        gap_buffer(line);
    }
    for (uint32_t i = 0; i < p->plan.n; i++) {
        snprintf(line, sizeof line, "range frame=%llu submit=%llu cb=%u id=%u kind=%u valid=%u t0=%llu t1=%llu",
                 seq, signal, part, i, p->plan.rows[i].kind, q[2*i][1] && q[2*i+1][1],
                 (unsigned long long)q[2*i][0], (unsigned long long)q[2*i+1][0]);
        gap_buffer(line);
    }
    if (p->plan.dropped) {
        snprintf(line, sizeof line, "missing frame=%llu submit=%llu source=range_capacity count=%u",
                 seq, signal, p->plan.dropped);
        gap_buffer(line);
    }
}

static void gap_finish_submit(const struct vk_segment *s, int failed, unsigned long long seq) {
    nvn_gpu_cmd c = s->cmd;
    if (!c) return; // only the host fixture advances virtual WSI without a command buffer
    const unsigned long long signal = s->internal ? 0 : s->signal;
    char line[340];
    if (failed || s->gap_tag != c->gap_tag || !c->tsq) {
        snprintf(line, sizeof line, "missing frame=%llu submit=%llu source=submit_or_phase count=1", seq, signal);
        gap_buffer(line);
        return;
    }

    uint64_t t[2 + 2 * TS_PASS_MAX][2] = {{0}};
    const uint32_t count = 2 + 2 * c->ntsp;
    (void)vkGetQueryPoolResults(g.d, c->tsq, 0, count, count * sizeof t[0], t, sizeof t[0],
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    snprintf(line, sizeof line,
             "submit frame=%llu signal=%llu cpu_submit_ns=%llu cpu_return_ns=%llu cpu_done_ns=%llu wsi=%u valid=%u t0=%llu t1=%llu pass_count=%u init_expected=%u",
             seq, signal, (unsigned long long)s->gap_cpu_submit, (unsigned long long)s->gap_cpu_return,
             (unsigned long long)s->gap_cpu_done, s->internal != 0, t[0][1] && t[1][1],
             (unsigned long long)t[0][0], (unsigned long long)t[1][0], c->ntsp, c->gap_has_init);
    gap_buffer(line);
    for (uint32_t i = 0; i < c->ntsp; i++) {
        const uint64_t *begin = t[2+2*i], *end = t[3+2*i];
        snprintf(line, sizeof line, "pass frame=%llu submit=%llu id=%u valid=%u t0=%llu t1=%llu w=%u h=%u draws=%u",
                 seq, signal, i, begin[1] && end[1], (unsigned long long)begin[0], (unsigned long long)end[0],
                 c->tsp[i].w, c->tsp[i].h, c->tsp[i].draws);
        gap_buffer(line);
    }
    if (c->ntsp == TS_PASS_MAX || !c->gap || c->gap->registry_lost) {
        snprintf(line, sizeof line, "missing frame=%llu submit=%llu source=%s count=1", seq, signal,
                 c->ntsp == TS_PASS_MAX ? "pass_capacity" : !c->gap ? "range_pool" : "registry");
        gap_buffer(line);
    }
    if (c->gap) {
        gap_rows(&c->gap->init, seq, signal, 1);
        gap_rows(&c->gap->main, seq, signal, 0);
    }
}

static void gap_calibrate(unsigned long long seq) {
    uint64_t t[2], deviation;
    if (g.report_bits != gap_cfg.bits || !report_calibration(t, &deviation)) return;
    const uint64_t corrected = (t[0] - (uint64_t)g.report_corr_ticks) & s2a_ts_valid_mask(g.report_bits);
    char line[192];
    snprintf(line, sizeof line, "calibrate frame=%llu gpu_tick=%llu mono_ns=%llu uncertainty_ns=%llu",
             seq, (unsigned long long)corrected, (unsigned long long)t[1],
             (unsigned long long)add_sat(deviation, g.report_corr_dev));
    gap_buffer(line);
}

static void gap_segment_done(const struct vk_segment *s, int failed) {
    const uint64_t tag = s->gap_tag;
    if (!(tag & GAP_RUN)) return;
    const unsigned phase = (unsigned)(tag >> GAP_PHASE_SHIFT) & GAP_PHASE_MASK;
    const unsigned mode = (unsigned)tag & 3;
    const unsigned long long seq = tag >> GAP_SEQ_SHIFT;
    char line[228];
    if (s->internal && (!gap_report_started || phase != gap_log_phase)) {
        // Guest rows for the new phase may already be in the previous flush;
        // every row independently carries the exact WSI sequence.
        gap_flush_log();
        gap_log_phase = phase;
        gap_report_started = 1;
        snprintf(line, sizeof line, "phase=%u mode=%u frame=%llu period_ns=%.9g bits=%u cpu_submit_ns=%llu calibrated=%u",
                 phase, mode, seq, (double)g.dev->properties.limits.timestampPeriod, gap_cfg.bits,
                 (unsigned long long)s->gap_cpu_submit, g.report_calibrate != NULL);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gap-gpu", line);
    }
    if (!(tag & GAP_EMIT)) return;
    gap_finish_submit(s, failed, seq);
    if (s->internal) {
        snprintf(line, sizeof line, "frame=%llu phase=%u mode=%u wsi=%llu failed=%u", seq,
                 phase, mode, (unsigned long long)s->wsi_seq, failed != 0 || seq != s->wsi_seq);
        gap_buffer(line);
        // Compare CPU and GPU clocks only if a checked pair was returned.
        if (tag & GAP_CALIBRATE) gap_calibrate(seq);
    }
}

#ifdef S2A_NVK_GAP_HOST_TEST
void nvk_gap_test_present(uint64_t seq, uint64_t elapsed_seconds) {
    struct vk_segment s = {.internal = 1, .wsi_seq = seq};
    narrow_wsi_time(&s, elapsed_seconds * UINT64_C(1000000000));
    gap_wsi_time(&s, elapsed_seconds * UINT64_C(1000000000));
    gap_segment_done(&s, 0);
    pre_wsi_tick(&s, elapsed_seconds * UINT64_C(1000000000));
    pre_finish(&s, 0);
    narrow_finish(&s, 0);
}
#endif
