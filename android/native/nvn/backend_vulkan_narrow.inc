// Default-off experiment for range-proven Vulkan transfers between render passes.
enum { NARROW_BATCH = 16, NARROW_PENDING = 64 };
enum narrow_stop { NAR_OP, NAR_OVERLAP, NAR_CAP, NAR_HISTORY, NAR_ALIAS, NAR_OUTSIDE, NAR_STOPS };

struct narrow_copy {
    nvn_gpu_buf src, dst;
    VkBufferCopy region;
    unsigned pool, cpu_source;
};
struct narrow_report {
    uint64_t writes, copies, bytes, pool, narrow_pool, narrow_copy, narrow_clear;
    uint64_t pre, post, post_folded, batches, batched_ops, max_batch;
    uint64_t stop[NAR_STOPS];
};
struct narrow_cmd {
    struct narrow_cmd *next;
    nvn_gpu_cmd owner;
    pthread_t encoder;
    uint64_t tag;
    unsigned ended, pool, unknown, emitting, isolated_clear, changed, ncopy, npending;
    struct narrow_copy copy[NARROW_BATCH];
    VkBufferMemoryBarrier pending[NARROW_PENDING];
    struct narrow_report report;
};

static pthread_mutex_t narrow_mu = PTHREAD_MUTEX_INITIALIZER;
static struct narrow_cmd *narrow_cmds;
static _Atomic uint64_t narrow_tag;
static _Atomic unsigned narrow_epoch;
static _Thread_local uint64_t narrow_submit_tag;
static _Thread_local unsigned narrow_submit_epoch;
static struct {
    unsigned target, delay, seconds, phase, edges;
    uint64_t first;
} narrow_cfg;

static void narrow_open(void) {
    memset(&narrow_cfg, 0, sizeof narrow_cfg);
    atomic_fetch_add_explicit(&narrow_epoch, 1, memory_order_relaxed);
    const int mode = nvn_plat_flag("nvk_barrier_narrow", 0);
    const int seconds = nvn_plat_flag("nvk_barrier_narrow_ab", 0);
    const int delay = nvn_plat_flag("nvk_barrier_narrow_s", 205);
    if (mode < 1 || mode > 3 || seconds < 0 || seconds > 180 || delay < 0) {
        atomic_store(&narrow_tag, 0);
        return;
    }
    narrow_cfg.target = (unsigned)mode;
    narrow_cfg.delay = (unsigned)delay;
    narrow_cfg.seconds = (unsigned)seconds;
    narrow_cfg.edges = seconds && nvn_plat_flag("nvk_barrier_narrow_timing", 0);
    atomic_store(&narrow_tag, seconds ? 1u : (unsigned)mode);
}

void nvn_vk_narrow_submit(int begin) {
    narrow_submit_tag = begin ? atomic_load_explicit(&narrow_tag, memory_order_acquire) : 0;
    narrow_submit_epoch = atomic_load_explicit(&narrow_epoch, memory_order_relaxed);
}

static unsigned narrow_timing_windows(void) {
    return narrow_cfg.edges ? (narrow_cfg.target == 1 ? 8u : 4u) : 0;
}

static unsigned narrow_phase(void) { return narrow_cfg.phase; }

static void narrow_wsi_time(struct vk_segment *s, uint64_t now) {
    if (!narrow_cfg.target) return;
    s->narrow_prev = atomic_load_explicit(&narrow_tag, memory_order_relaxed);
    if (!narrow_cfg.seconds) return;
    if (!narrow_cfg.first) narrow_cfg.first = now;
    const uint64_t elapsed = (now - narrow_cfg.first) / UINT64_C(1000000000);
    if (elapsed < narrow_cfg.delay) return;
    const uint64_t pos = (elapsed - narrow_cfg.delay) / narrow_cfg.seconds;
    const unsigned windows = narrow_cfg.target == 1 ? 8 : 4;
    const unsigned phase = pos >= windows ? windows + 1 : (unsigned)pos + 1;
    if (phase == narrow_cfg.phase) return;
    const unsigned alternate = narrow_cfg.target != 1 ? narrow_cfg.target : phase <= 4 ? 2 : 3;
    const unsigned mode = phase <= windows && !(phase & 1) ? alternate : 1;
    narrow_cfg.phase = phase;
    s->narrow_next = ((uint64_t)phase << 8) | mode;
    s->narrow_ns = now;
    atomic_store_explicit(&narrow_tag, s->narrow_next, memory_order_release);
}

static void narrow_wsi_tick(struct vk_segment *s) {
    if (narrow_cfg.target) narrow_wsi_time(s, narrow_cfg.seconds ? now_ns() : 0);
}

static void narrow_cmd_begin(nvn_gpu_cmd c) {
    uint64_t tag = atomic_load_explicit(&narrow_tag, memory_order_acquire);
    if (!tag) return;
    if (narrow_submit_tag && narrow_submit_epoch == atomic_load_explicit(&narrow_epoch, memory_order_relaxed))
        tag = narrow_submit_tag;
    struct narrow_cmd *n = calloc(1, sizeof *n);
    if (!n) return;   // Baseline semantics if optional diagnostic bookkeeping cannot be allocated.
    n->owner = c;
    n->encoder = pthread_self();
    n->tag = tag;
    n->unknown = 1;   // A queued earlier CB may write unrelated memory until a real full barrier covers it.
    c->narrow = n;
    if ((tag & 255) <= 1) return;
    pthread_mutex_lock(&narrow_mu);
    n->next = narrow_cmds;
    narrow_cmds = n;
    atomic_fetch_add_explicit(&narrow_registered, 1, memory_order_relaxed);
    pthread_mutex_unlock(&narrow_mu);
}

static void narrow_detach(nvn_gpu_cmd c) {
    if (!c->narrow || (c->narrow->tag & 255) <= 1) return;
    pthread_mutex_lock(&narrow_mu);
    struct narrow_cmd **n = &narrow_cmds;
    while (*n && *n != c->narrow) n = &(*n)->next;
    if (*n) {
        struct narrow_cmd *removed = *n;
        *n = removed->next;
        if (removed->ncopy) atomic_fetch_sub_explicit(&narrow_queued_batches, 1, memory_order_relaxed);
        removed->ncopy = 0;
        removed->npending = 0;
        atomic_fetch_sub_explicit(&narrow_registered, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&narrow_mu);
}

static void narrow_cmd_free(nvn_gpu_cmd c) {
    if (!c->narrow) return;
    narrow_detach(c);
    free(c->narrow);
}

static struct narrow_cmd *narrow_lookup(VkCommandBuffer cb) {
    const pthread_t self = pthread_self();
    pthread_mutex_lock(&narrow_mu);
    struct narrow_cmd *n = narrow_cmds;
    while (n && n->owner->cb != cb) n = n->next;
    if (n && (!pthread_equal(n->encoder, self) || n->emitting || n->isolated_clear)) n = NULL;
    pthread_mutex_unlock(&narrow_mu);
    // The encoding contract keeps an active command owned by this thread.
    // Commit detaches it before passing ownership to the retirement worker.
    return n;
}

static VkBufferMemoryBarrier narrow_buffer(nvn_gpu_buf buffer, VkDeviceSize off, VkDeviceSize size,
                                           VkAccessFlags src, VkAccessFlags dst) {
    return (VkBufferMemoryBarrier){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = src, .dstAccessMask = dst,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer->buffer, .offset = off, .size = size};
}

// The queue and buffer creation path expose only these consumers. A graphics
// pass can fetch indirect/index/vertex data before any shader executes.
static VkPipelineStageFlags narrow_consumer_stages(void) {
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (g.dev->core_features.geometryShader) stages |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    if (g.dev->core_features.tessellationShader)
        stages |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    return stages;
}

static void narrow_publish(struct narrow_cmd *n) {
    if (!n->npending) return;
    if (!n->changed) { n->npending = 0; return; }
    n->emitting = 1;
    vkCmdPipelineBarrier(n->owner->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, narrow_consumer_stages(), 0,
                         0, NULL, n->npending, n->pending, 0, NULL);
    n->emitting = 0;
    n->npending = 0;
    n->changed = 0;
    ++n->report.post;
}

static void narrow_add_pending(struct narrow_cmd *n, const struct narrow_copy *r) {
    const VkAccessFlags readers = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    n->pending[n->npending++] = narrow_buffer(r->dst, r->region.dstOffset, r->region.size,
                                             VK_ACCESS_TRANSFER_WRITE_BIT, readers);
}

static void narrow_flush(struct narrow_cmd *n) {
    if (!n->ncopy) return;
    if (n->npending + n->ncopy > NARROW_PENDING) narrow_publish(n);
    VkBufferMemoryBarrier pre[2 * NARROW_BATCH];
    unsigned nr = 0;
    for (unsigned i = 0; i < n->ncopy; ++i) {
        const struct narrow_copy *r = &n->copy[i];
        if (!r->cpu_source)
            pre[nr++] = narrow_buffer(r->src, r->region.srcOffset, r->region.size,
                                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        pre[nr++] = narrow_buffer(r->dst, r->region.dstOffset, r->region.size,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    n->emitting = 1;
    n->changed = 1;
    vkCmdPipelineBarrier(n->owner->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, nr, pre, 0, NULL);
    for (unsigned i = 0; i < n->ncopy;) {
        VkBufferCopy regions[NARROW_BATCH];
        const unsigned first = i;
        const struct narrow_copy *r = &n->copy[first];
        do {
            regions[i - first] = n->copy[i].region;
            narrow_add_pending(n, &n->copy[i]);
            ++i;
        } while (i < n->ncopy && n->copy[i].src == r->src && n->copy[i].dst == r->dst);
        vkCmdCopyBuffer(n->owner->cb, r->src->buffer, r->dst->buffer, i - first, regions);
    }
    n->emitting = 0;
    struct narrow_report *report = &n->report;
    ++report->pre;
    ++report->batches;
    report->batched_ops += n->ncopy;
    if (n->ncopy > report->max_batch) report->max_batch = n->ncopy;
    n->ncopy = 0;
    atomic_fetch_sub_explicit(&narrow_queued_batches, 1, memory_order_relaxed);
}

static int narrow_overlap(nvn_gpu_buf a, VkDeviceSize off_a, nvn_gpu_buf b, VkDeviceSize off_b,
                          VkDeviceSize size_a, VkDeviceSize size_b) {
    // All candidates bind their own VkDeviceMemory at offset zero. The copy
    // ranges were checked against allocation size before this call.
    return a->memory == b->memory && off_a < off_b + size_b && off_b < off_a + size_a;
}
static int narrow_conflict(const struct narrow_copy *a, const struct narrow_copy *b) {
    const VkBufferCopy x = a->region, y = b->region;
    return narrow_overlap(a->dst, x.dstOffset, b->dst, y.dstOffset, x.size, y.size) ||
           narrow_overlap(a->dst, x.dstOffset, b->src, y.srcOffset, x.size, y.size) ||
           narrow_overlap(a->src, x.srcOffset, b->dst, y.dstOffset, x.size, y.size);
}

static int narrow_try_copy(nvn_gpu_cmd c, nvn_gpu_buf src, nvn_gpu_buf dst, VkBufferCopy region, unsigned staged) {
    struct narrow_cmd *n = c->narrow;
    if (!n) return 0;
    n->report.bytes += region.size;
    if (staged) {
        ++n->report.writes;
        n->report.pool += n->pool != 0;
    } else ++n->report.copies;
    if ((n->tag & 255) < 2) return 0;
    if (!n->ended) { ++n->report.stop[NAR_OUTSIDE]; return 0; }
    if (n->unknown) { ++n->report.stop[NAR_HISTORY]; return 0; }
    if ((staged && !n->pool) || !src->memory || !dst->memory || src->memory == dst->memory) {
        ++n->report.stop[NAR_ALIAS];
        return 0;
    }
    const struct narrow_copy copy = {src, dst, region, n->pool, staged};
    for (unsigned i = 0; i < n->ncopy; ++i) {
        if (!narrow_conflict(&n->copy[i], &copy)) continue;
        ++n->report.stop[NAR_OVERLAP];
        narrow_flush(n);
        break;
    }
    if (n->ncopy == NARROW_BATCH) { ++n->report.stop[NAR_CAP]; narrow_flush(n); }
    if (!n->ncopy) atomic_fetch_add_explicit(&narrow_queued_batches, 1, memory_order_relaxed);
    n->copy[n->ncopy++] = copy;
    if (copy.pool) ++n->report.narrow_pool;
    else ++n->report.narrow_copy;
    if ((n->tag & 255) == 2) narrow_flush(n);
    return 1;
}

static void narrow_legacy_copy(nvn_gpu_cmd c, nvn_gpu_buf src, nvn_gpu_buf dst, VkBufferCopy region) {
    struct narrow_cmd *n = c->narrow;
    const int track = n && (n->tag & 255) > 1 && src->memory && dst->memory && src->memory != dst->memory;
    if (track) n->emitting = 1;
    vkCmdCopyBuffer(c->cb, src->buffer, dst->buffer, 1, &region);
    if (track) {
        const struct narrow_copy copy = {.dst = dst, .region = region};
        n->emitting = 0;
        narrow_add_pending(n, &copy);
    }
}

static void narrow_pool_source(nvn_gpu_cmd c, int active) {
    if (c && c->narrow) c->narrow->pool = active != 0;
}

static int narrow_clear_begin(nvn_gpu_cmd c, const struct cmd_lay *layout, int color) {
    struct narrow_cmd *n = c->narrow;
    if (!n || (n->tag & 255) == 1) return 0;
    if (n->ncopy) { ++n->report.stop[NAR_OP]; narrow_flush(n); }
    // Use the real, already recorded whole-image layout, and keep the
    // conservative path if this clear was also a global publication point.
    if (!n->ended || n->unknown || c->xfer_dirty || !layout || !color ||
        layout->cur == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return 0;
    ++n->report.narrow_clear;
    return 1;
}

static void narrow_clear_record(nvn_gpu_cmd c, int scoped_color) {
    if (c->narrow && (c->narrow->tag & 255) > 1) c->narrow->isolated_clear = scoped_color != 0;
}

static void narrow_clear_end(nvn_gpu_cmd c) {
    if (c->narrow) c->narrow->isolated_clear = 0;
}

static void narrow_before_cmd(VkCommandBuffer cb, unsigned op) {
    struct narrow_cmd *n = narrow_lookup(cb);
    if (!n) return;
    if (n->ncopy) { ++n->report.stop[NAR_OP]; narrow_flush(n); }
    switch (op) {
    case OP_vkCmdBeginRenderPass:
    case OP_vkCmdBeginRendering:
        narrow_publish(n);
        break;
    case OP_vkCmdEndRenderPass:
    case OP_vkCmdEndRendering:
        n->ended = 1;
        n->unknown = 1;
        break;
    case OP_vkCmdDispatch:
    case OP_vkCmdDispatchIndirect:
    case OP_vkCmdCopyBuffer:
    case OP_vkCmdCopyImage:
    case OP_vkCmdCopyBufferToImage:
    case OP_vkCmdCopyImageToBuffer:
    case OP_vkCmdBlitImage:
    case OP_vkCmdResolveImage:
    case OP_vkCmdClearColorImage:
    case OP_vkCmdClearDepthStencilImage:
    case OP_vkCmdFillBuffer:
    case OP_vkCmdUpdateBuffer:
        narrow_publish(n);
        n->unknown = 1;
        break;
    default:
        break;
    }
}

static void narrow_before_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                  uint32_t nm, const VkMemoryBarrier *m, unsigned images) {
    struct narrow_cmd *n = narrow_lookup(cb);
    if (!n) return;
    if (n->ncopy) { ++n->report.stop[NAR_OP]; narrow_flush(n); }
    for (uint32_t i = 0; i < nm; ++i) {
        const VkAccessFlags rw = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        if (src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
            (m[i].srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT) && (m[i].dstAccessMask & rw) == rw) {
            n->unknown = 0;
            n->report.post_folded += n->changed != 0;
            n->npending = 0;
            n->changed = 0;
            return;
        }
    }
    // An arbitrary image layout transition can write backing image memory.
    if (images) n->unknown = 1;
}

static void narrow_commit_end(nvn_gpu_cmd c) {
    struct narrow_cmd *n = c->narrow;
    if (!n || (n->tag & 255) == 1) return;
    if (n->ncopy) { ++n->report.stop[NAR_OP]; narrow_flush(n); }
    narrow_publish(n);
    narrow_detach(c);
}

static struct narrow_report narrow_totals;
static struct {
    struct narrow_report totals;
    uint64_t tag;
    unsigned segments, invalid;
} narrow_frame;
static uint64_t narrow_report_tag, narrow_report_frames;
static void narrow_log_counts(void) {
    if (!narrow_report_tag || !narrow_report_frames) return;
    const struct narrow_report a = narrow_totals;
    char line[570];
    snprintf(line, sizeof line, "counts phase=%llu mode=%u frames=%llu writes=%llu copies=%llu bytes=%llu pool=%llu "
             "narrow_pool=%llu narrow_copy=%llu narrow_clear=%llu pre=%llu post_added=%llu post_folded=%llu "
             "batches=%llu batched_ops=%llu max_batch=%llu stop_op=%llu stop_overlap=%llu stop_capacity=%llu "
             "stop_history=%llu stop_alias=%llu stop_outside=%llu",
             (unsigned long long)(narrow_report_tag >> 8), (unsigned)(narrow_report_tag & 255),
             (unsigned long long)narrow_report_frames, (unsigned long long)a.writes, (unsigned long long)a.copies,
             (unsigned long long)a.bytes, (unsigned long long)a.pool, (unsigned long long)a.narrow_pool,
             (unsigned long long)a.narrow_copy, (unsigned long long)a.narrow_clear, (unsigned long long)a.pre,
             (unsigned long long)a.post, (unsigned long long)a.post_folded, (unsigned long long)a.batches,
             (unsigned long long)a.batched_ops, (unsigned long long)a.max_batch, (unsigned long long)a.stop[NAR_OP],
             (unsigned long long)a.stop[NAR_OVERLAP], (unsigned long long)a.stop[NAR_CAP], (unsigned long long)a.stop[NAR_HISTORY],
             (unsigned long long)a.stop[NAR_ALIAS], (unsigned long long)a.stop[NAR_OUTSIDE]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "narrow-sync", line);
    memset(&narrow_totals, 0, sizeof narrow_totals);
    narrow_report_frames = 0;
}

static void narrow_accumulate(struct narrow_report *t, const struct narrow_report *a) {
    t->writes += a->writes; t->copies += a->copies; t->bytes += a->bytes; t->pool += a->pool;
    t->narrow_pool += a->narrow_pool; t->narrow_copy += a->narrow_copy; t->narrow_clear += a->narrow_clear;
    t->pre += a->pre; t->post += a->post; t->post_folded += a->post_folded;
    t->batches += a->batches; t->batched_ops += a->batched_ops;
    if (a->max_batch > t->max_batch) t->max_batch = a->max_batch;
    for (unsigned i = 0; i < NAR_STOPS; ++i) t->stop[i] += a->stop[i];
}

static void narrow_drop(uint64_t frame, uint64_t expected, const char *why) {
    char line[210];
    snprintf(line, sizeof line, "excluded wsi=%llu expected_phase=%llu actual_phase=%llu segments=%u reason=%s",
             (unsigned long long)frame, (unsigned long long)(expected >> 8),
             (unsigned long long)(narrow_frame.tag >> 8), narrow_frame.segments, why);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "narrow-sync", line);
}

static void narrow_close(void) {
    if (narrow_frame.segments) narrow_drop(0, 0, "no_wsi");
    memset(&narrow_frame, 0, sizeof narrow_frame);
    narrow_log_counts();
    narrow_report_tag = 0;
    atomic_store_explicit(&narrow_tag, 0, memory_order_release);
}

static void narrow_finish(const struct vk_segment *s, int failed) {
    if (!narrow_cfg.target) return;
    if (!s->internal) {
        const struct narrow_cmd *n = s->cmd ? s->cmd->narrow : NULL;
        ++narrow_frame.segments;
        if (failed || !n) narrow_frame.invalid = 1;
        else {
            if (!narrow_frame.tag) narrow_frame.tag = n->tag;
            if (narrow_frame.tag != n->tag) narrow_frame.invalid = 1;
            narrow_accumulate(&narrow_frame.totals, &n->report);
        }
        return;
    }
    if (narrow_frame.segments) {
        if (failed || narrow_frame.invalid || narrow_frame.tag != s->narrow_prev) {
            narrow_drop(s->wsi_seq, s->narrow_prev, "frame_or_phase");
        } else {
            if (narrow_report_tag && narrow_report_tag != narrow_frame.tag) narrow_log_counts();
            narrow_report_tag = narrow_frame.tag;
            narrow_accumulate(&narrow_totals, &narrow_frame.totals);
            if (++narrow_report_frames >= 120) narrow_log_counts();
        }
        memset(&narrow_frame, 0, sizeof narrow_frame);
    }
    if (s->narrow_next) {
        narrow_log_counts();
        char line[160];
        snprintf(line, sizeof line, "phase=%llu mode=%u wsi=%llu cpu_ns=%llu",
                 (unsigned long long)(s->narrow_next >> 8), (unsigned)(s->narrow_next & 255),
                 (unsigned long long)(s->wsi_seq + 1), (unsigned long long)s->narrow_ns);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "narrow-sync", line);
    }
}
