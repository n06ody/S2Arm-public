// Once-off frame survey and sparse timing of closed, same-CB render gaps.
// Recording hooks run before the DQ producer, so both recorder modes see the
// same order. Only the sampled frame holds CPU records. No Vulkan dependency
// commands are synthesized by this probe.
enum { PRE_SURVEY_FRAMES = 6, PRE_TIME_FIRST = 15, PRE_TIME_FRAMES = 48,
       PRE_TARGETS = 24, PRE_ROTATIONS = 4, PRE_REC_CAP = 512 * 1024,
       PRE_LOG_CAP = 12 * 1024 * 1024, PRE_IMAGE_BUCKETS = 1024 };

struct pre_pair {
    unsigned gap, expected_slot, closed, empty;
    uint64_t end_shape, begin_shape;
};

struct pre_record {
    struct pre_record *next;
    VkCommandBuffer cb;
    VkQueryPool queries;
    uint64_t frame, shape;
    unsigned phase, rotation, preamble, slot, init, emit;
    unsigned passes, gap, ordinal, open_pair, pairs, lost, next_source;
    struct pre_origin source;
    size_t len, complete_len, cap;
    char *rows;
    struct pre_pair pair[PRE_TARGETS];
};

struct pre_image {
    struct pre_image *next;
    VkImage image;
    VkExtent3D extent;
    VkFormat format;
    unsigned bw, bh, bytes;
};

static pthread_mutex_t pre_mu = PTHREAD_MUTEX_INITIALIZER;
static struct pre_record *pre_records;
static struct pre_image *pre_images[PRE_IMAGE_BUCKETS];
static struct {
    uint64_t first, start_frame, frame;
    unsigned delay, count_seconds, arm_seconds, bits, phase, sample, rotation;
    unsigned slot, leading, claimed, targets[PRE_TARGETS];
} pre_cfg;

// pre_log is owned by the fence worker. Close flushes after joining it.
static char *pre_log;
static size_t pre_log_len;
static unsigned pre_log_lost;

static void pre_append(char **buf, size_t *len, size_t *cap, size_t limit,
                       unsigned *lost, const char *line) {
    const size_t n = strlen(line) + 1;
    if (n > limit - *len) { ++*lost; return; }
    if (*cap < *len + n) {
        size_t grown = *cap ? *cap * 2 : 4096;
        if (grown < *len + n) grown = *len + n;
        if (grown > limit) grown = limit;
        char *p = realloc(*buf, grown);
        if (!p) { ++*lost; return; }
        *buf = p;
        *cap = grown;
    }
    memcpy(*buf + *len, line, n);
    *len += n;
}

static void pre_flush(void) {
    for (size_t i = 0; i < pre_log_len; i += strlen(pre_log + i) + 1)
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pre-trace", pre_log + i);
    if (pre_log_lost) {
        char line[80];
        snprintf(line, sizeof line, "log_overflow rows=%u", pre_log_lost);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "pre-trace", line);
    }
    free(pre_log);
    pre_log = NULL;
    pre_log_len = 0;
    pre_log_lost = 0;
}

static void pre_buffer(const char *line) {
    static size_t cap;
    if (!pre_log) cap = 0;
    pre_append(&pre_log, &pre_log_len, &cap, PRE_LOG_CAP, &pre_log_lost, line);
}

static void pre_open(void) {
    memset(&pre_cfg, 0, sizeof pre_cfg);
    const int delay = nvn_plat_flag("nvk_pre_trace_s", 0);
    if (delay <= 0 || nvn_plat_flag("nvk_gputime", 0) || nvn_plat_flag("nvk_gap_s", 0)) return;
    const int count_seconds = nvn_plat_flag("nvk_pre_trace_count_s", 8);
    const int arm_seconds = nvn_plat_flag("nvk_pre_trace_arm_s", 30);
    if (count_seconds < 1 || count_seconds > 180 || arm_seconds < 0 || arm_seconds > 180) return;
    pre_cfg.delay = (unsigned)delay;
    pre_cfg.count_seconds = (unsigned)count_seconds;
    pre_cfg.arm_seconds = (unsigned)arm_seconds;
    pre_cfg.phase = UINT_MAX;
    for (unsigned i = 0; i < PRE_TARGETS; ++i) {
        char name[40];
        snprintf(name, sizeof name, "nvk_pre_trace_p%u", i);
        // Each positive value is (1-based ordinary submit slot * 1024 + gap id).
        const int target = nvn_plat_flag(name, 0);
        if (target > 1024 && target % 1024) pre_cfg.targets[i] = (unsigned)target;
    }
    if (!arm_seconds || !isfinite(g.dev->properties.limits.timestampPeriod) ||
        g.dev->properties.limits.timestampPeriod <= 0) return;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.dev->physical, &n, NULL);
    if (g.dev->queue_family >= n) return;
    VkQueueFamilyProperties *q = calloc(n, sizeof *q);
    if (!q) return;
    vkGetPhysicalDeviceQueueFamilyProperties(g.dev->physical, &n, q);
    if (g.dev->queue_family < n) pre_cfg.bits = q[g.dev->queue_family].timestampValidBits;
    free(q);
    if (pre_cfg.bits > 64) pre_cfg.bits = 0;
}

static void pre_close(void) {
    pre_flush();
    for (unsigned i = 0; i < PRE_IMAGE_BUCKETS; ++i) {
        while (pre_images[i]) {
            struct pre_image *p = pre_images[i];
            pre_images[i] = p->next;
            free(p);
        }
    }
    pre_cfg.delay = 0;
}

static struct pre_record *pre_lookup(VkCommandBuffer cb) {
    pthread_mutex_lock(&pre_mu);
    struct pre_record *p = pre_records;
    while (p && p->cb != cb) p = p->next;
    pthread_mutex_unlock(&pre_mu);
    return p;
}

static void pre_cmd_begin(nvn_gpu_cmd c, const char *label) {
    if (!pre_cfg.delay) return;
    pthread_mutex_lock(&pre_mu);
    if (pre_cfg.sample) {
        struct pre_record *p = calloc(1, sizeof *p);
        if (p) {
            p->cb = c->cb;
            p->frame = pre_cfg.frame;
            p->phase = pre_cfg.phase;
            p->rotation = pre_cfg.rotation;
            p->preamble = label && !strcmp(label, "nvn submit pre");
            p->next = pre_records;
            pre_records = p;
            c->pre = p;
            atomic_fetch_add_explicit(&pre_active, 1, memory_order_relaxed);
            if (!p->phase) atomic_fetch_add_explicit(&pre_count_active, 1, memory_order_relaxed);
        }
    }
    pthread_mutex_unlock(&pre_mu);
}

static void pre_detach(nvn_gpu_cmd c) {
    if (!c->pre) return;
    pthread_mutex_lock(&pre_mu);
    struct pre_record **p = &pre_records;
    while (*p && *p != c->pre) p = &(*p)->next;
    if (*p) {
        *p = (*p)->next;
        atomic_fetch_sub_explicit(&pre_active, 1, memory_order_relaxed);
        if (!c->pre->phase) atomic_fetch_sub_explicit(&pre_count_active, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&pre_mu);
}

static void pre_cmd_free(nvn_gpu_cmd c) {
    if (!c->pre) return;
    pre_detach(c);
    if (c->pre->queries) vkDestroyQueryPool(g.d, c->pre->queries, NULL);
    free(c->pre->rows);
    free(c->pre);
}

static struct pre_origin pre_origin(nvn_gpu_cmd c, unsigned kind) {
    if (!c || !c->pre) return (struct pre_origin){0};
    struct pre_origin old = c->pre->source;
    c->pre->source = (struct pre_origin){kind, ++c->pre->next_source};
    return old;
}

static void pre_origin_restore(nvn_gpu_cmd c, struct pre_origin origin) {
    if (c && c->pre) c->pre->source = origin;
}

// Only the common replay's actual pool mirror write calls this hook. A pool
// resync that needs the main CB keeps its source there as well.
void nvn_vk_pre_trace_pool(nvn_gpu_cmd c, int active) {
    narrow_pool_source(c, active);
    if (active) pre_origin(c, PRE_POOL);
    else pre_origin_restore(c, (struct pre_origin){0});
}

static const char *pre_source(unsigned source) {
    static const char *const names[] = {"explicit", "pool", "upload", "deswizzle", "bc", "tail"};
    return names[source];
}

static size_t pre_bucket(VkImage image) {
    const uintptr_t key = (uintptr_t)image;
    return ((key >> 6) ^ (key >> 16) ^ key) % PRE_IMAGE_BUCKETS;
}

static void pre_image_add(VkImage image, VkFormat format, VkExtent3D extent) {
    if (!pre_cfg.delay || !image) return;
    struct pre_image *p = malloc(sizeof *p);
    if (!p) return;
    *p = (struct pre_image){.image = image, .format = format, .extent = extent,
                            .bw = 1, .bh = 1, .bytes = fmt_bpp(format)};
    if (!p->bytes) {
        for (unsigned f = 1; f <= NVN_FORMAT_BGRA8; ++f) {
            nvn_fmt_info fi;
            if (nvn_vk_tex_format(f) == format && !nvn_fmt_expand_target(f, NULL, NULL) &&
                nvn_fmt_info_of(f, &fi) && fi.bw && fi.bh && fi.bytes && !fi.depth && !fi.stencil) {
                p->bw = fi.bw;
                p->bh = fi.bh;
                p->bytes = fi.bytes;
                break;
            }
        }
    }
    const size_t slot = pre_bucket(image);
    pthread_mutex_lock(&pre_mu);
    p->next = pre_images[slot];
    pre_images[slot] = p;
    pthread_mutex_unlock(&pre_mu);
}

static void pre_image_del(VkImage image) {
    if (!pre_cfg.delay || !image) return;
    pthread_mutex_lock(&pre_mu);
    struct pre_image **p = &pre_images[pre_bucket(image)];
    while (*p && (*p)->image != image) p = &(*p)->next;
    if (*p) {
        struct pre_image *old = *p;
        *p = old->next;
        free(old);
    }
    pthread_mutex_unlock(&pre_mu);
}

static struct pre_image pre_image_get(VkImage image) {
    struct pre_image info = {0};
    pthread_mutex_lock(&pre_mu);
    struct pre_image *p = pre_images[pre_bucket(image)];
    while (p && p->image != image) p = p->next;
    if (p) info = *p;
    pthread_mutex_unlock(&pre_mu);
    return info;
}

static uint64_t pre_image_copied(struct pre_image info, VkExtent3D extent, VkImageSubresourceLayers sub) {
    if (info.format == VK_FORMAT_UNDEFINED) return 0;
    if (sub.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) info.bytes = 1;
    if (sub.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && info.format == VK_FORMAT_D32_SFLOAT_S8_UINT) info.bytes = 4;
    uint64_t w = ((uint64_t)extent.width + info.bw - 1) / info.bw;
    uint64_t h = ((uint64_t)extent.height + info.bh - 1) / info.bh;
    return w * h * extent.depth * sub.layerCount * info.bytes;
}

static struct pre_record *pre_count_record(VkCommandBuffer cb) {
    struct pre_record *p = pre_lookup(cb);
    return p && !p->phase && (!p->passes || p->gap) ? p : NULL;
}

static void pre_row(struct pre_record *p, const char *type, const char *fmt, ...) {
    if (p->lost) return;
    char line[512];
    const int header = snprintf(line, sizeof line, "%s gap=%u ordinal=%u origin=%s source=%u ",
                                type, p->gap, p->ordinal, pre_source(p->source.kind), p->source.id);
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line + header, sizeof line - (size_t)header, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line - (size_t)header) { ++p->lost; return; }
    pre_append(&p->rows, &p->len, &p->cap, PRE_REC_CAP, &p->lost, line);
}

static void pre_op(VkCommandBuffer cb, unsigned op) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    ++p->ordinal;
    pre_row(p, "op", "name=%s ", g_dq_names[op]);
}

static void pre_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                        VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                        uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    ++p->ordinal;
    pre_row(p, "op", "name=vkCmdPipelineBarrier src_stage=0x%x dst_stage=0x%x dependency=0x%x memory=%u buffers=%u images=%u",
            src, dst, dep, nm, nb, ni);
    for (uint32_t i = 0; i < nm; ++i)
        pre_row(p, "barrier", "type=memory index=%u src_access=0x%x dst_access=0x%x", i, m[i].srcAccessMask, m[i].dstAccessMask);
    for (uint32_t i = 0; i < nb; ++i)
        pre_row(p, "barrier", "type=buffer index=%u src_access=0x%x dst_access=0x%x off=%llu size=%llu queues=%u,%u",
                i, b[i].srcAccessMask, b[i].dstAccessMask, (unsigned long long)b[i].offset,
                (unsigned long long)b[i].size, b[i].srcQueueFamilyIndex, b[i].dstQueueFamilyIndex);
    for (uint32_t i = 0; i < ni; ++i) {
        const VkImageSubresourceRange r = im[i].subresourceRange;
        const struct pre_image info = pre_image_get(im[i].image);
        pre_row(p, "barrier", "type=image index=%u src_access=0x%x dst_access=0x%x layout=%u,%u "
                "aspect=0x%x mips=%u,%u layers=%u,%u queues=%u,%u format=%u extent=%ux%ux%u",
                i, im[i].srcAccessMask, im[i].dstAccessMask, im[i].oldLayout, im[i].newLayout,
                r.aspectMask, r.baseMipLevel, r.levelCount, r.baseArrayLayer, r.layerCount,
                im[i].srcQueueFamilyIndex, im[i].dstQueueFamilyIndex, info.format,
                info.extent.width, info.extent.height, info.extent.depth);
    }
}

static void pre_copy_buffer(VkCommandBuffer cb, uint32_t n, const VkBufferCopy *rg) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < n; ++i) bytes += rg[i].size;
    ++p->ordinal;
    pre_row(p, "op", "name=vkCmdCopyBuffer bytes=%llu regions=%u", (unsigned long long)bytes, n);
}

static void pre_buffer_image(VkCommandBuffer cb, unsigned op, VkImage image, VkImageLayout layout,
                             uint32_t n, const VkBufferImageCopy *rg) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    const struct pre_image info = pre_image_get(image);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < n; ++i) bytes += pre_image_copied(info, rg[i].imageExtent, rg[i].imageSubresource);
    ++p->ordinal;
    pre_row(p, "op", "name=%s bytes=%llu bytes_known=%u regions=%u layout=%u format=%u extent=%ux%ux%u",
            g_dq_names[op], (unsigned long long)bytes, bytes != 0, n, layout, info.format,
            info.extent.width, info.extent.height, info.extent.depth);
    for (uint32_t i = 0; i < n; ++i) {
        const VkBufferImageCopy r = rg[i];
        pre_row(p, "region", "index=%u buffer_off=%llu row=%u slice=%u aspect=0x%x mip=%u layer=%u,%u xyz=%d,%d,%d extent=%ux%ux%u",
                i, (unsigned long long)r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource.aspectMask,
                r.imageSubresource.mipLevel, r.imageSubresource.baseArrayLayer, r.imageSubresource.layerCount,
                r.imageOffset.x, r.imageOffset.y, r.imageOffset.z, r.imageExtent.width, r.imageExtent.height, r.imageExtent.depth);
    }
}

static VkExtent3D pre_box(const VkOffset3D o[2]) {
    return (VkExtent3D){(uint32_t)llabs((int64_t)o[1].x - o[0].x), (uint32_t)llabs((int64_t)o[1].y - o[0].y),
                        (uint32_t)llabs((int64_t)o[1].z - o[0].z)};
}

static void pre_copy_image(VkCommandBuffer cb, unsigned op, VkImage src, VkImageLayout sl,
                           VkImage dst, VkImageLayout dl, uint32_t n, const void *regions, VkFilter filter) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    const struct pre_image si = pre_image_get(src), di = pre_image_get(dst);
    ++p->ordinal;
    pre_row(p, "op", "name=%s regions=%u layout=%u,%u format=%u,%u filter=%u", g_dq_names[op], n, sl, dl, si.format, di.format, filter);
    for (uint32_t i = 0; i < n; ++i) {
        VkExtent3D se, de;
        VkImageSubresourceLayers ss, ds;
        VkOffset3D so, d0;
        unsigned reverse = 0;
        if (op == OP_vkCmdBlitImage) {
            const VkImageBlit *r = (const VkImageBlit *)regions + i;
            se = pre_box(r->srcOffsets); de = pre_box(r->dstOffsets);
            ss = r->srcSubresource; ds = r->dstSubresource;
            so = r->srcOffsets[0]; d0 = r->dstOffsets[0];
            reverse = (so.x > r->srcOffsets[1].x) | ((so.y > r->srcOffsets[1].y) << 1) |
                      ((so.z > r->srcOffsets[1].z) << 2) | ((d0.x > r->dstOffsets[1].x) << 3) |
                      ((d0.y > r->dstOffsets[1].y) << 4) | ((d0.z > r->dstOffsets[1].z) << 5);
        } else if (op == OP_vkCmdResolveImage) {
            const VkImageResolve *r = (const VkImageResolve *)regions + i;
            se = de = r->extent; ss = r->srcSubresource; ds = r->dstSubresource; so = r->srcOffset; d0 = r->dstOffset;
        } else {
            const VkImageCopy *r = (const VkImageCopy *)regions + i;
            se = de = r->extent; ss = r->srcSubresource; ds = r->dstSubresource; so = r->srcOffset; d0 = r->dstOffset;
        }
        pre_row(p, "region", "index=%u src_extent=%ux%ux%u dst_extent=%ux%ux%u src_xyz=%d,%d,%d dst_xyz=%d,%d,%d reverse=0x%x "
                "src_sub=%x,%u,%u,%u dst_sub=%x,%u,%u,%u src_bytes=%llu dst_bytes=%llu",
                i, se.width, se.height, se.depth, de.width, de.height, de.depth, so.x, so.y, so.z, d0.x, d0.y, d0.z, reverse,
                ss.aspectMask, ss.mipLevel, ss.baseArrayLayer, ss.layerCount, ds.aspectMask, ds.mipLevel, ds.baseArrayLayer, ds.layerCount,
                (unsigned long long)pre_image_copied(si, se, ss), (unsigned long long)pre_image_copied(di, de, ds));
    }
}

static void pre_clear(VkCommandBuffer cb, unsigned op, VkImage image, VkImageLayout layout,
                      uint32_t n, const VkImageSubresourceRange *rg) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    const struct pre_image info = pre_image_get(image);
    ++p->ordinal;
    pre_row(p, "op", "name=%s regions=%u layout=%u format=%u extent=%ux%ux%u",
            g_dq_names[op], n, layout, info.format, info.extent.width, info.extent.height, info.extent.depth);
    for (uint32_t i = 0; i < n; ++i)
        pre_row(p, "region", "index=%u aspect=0x%x mips=%u,%u layers=%u,%u",
                i, rg[i].aspectMask, rg[i].baseMipLevel, rg[i].levelCount, rg[i].baseArrayLayer, rg[i].layerCount);
}

static void pre_buffer_span(VkCommandBuffer cb, unsigned op, VkDeviceSize off, VkDeviceSize size) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    ++p->ordinal;
    pre_row(p, "op", "name=%s bytes=%llu regions=1 off=%llu bytes_known=%u",
            g_dq_names[op], size == VK_WHOLE_SIZE ? 0ull : (unsigned long long)size, (unsigned long long)off, size != VK_WHOLE_SIZE);
}

static void pre_dispatch(VkCommandBuffer cb, unsigned op, unsigned x, unsigned y, unsigned z, VkDeviceSize off) {
    struct pre_record *p = pre_count_record(cb);
    if (!p) return;
    ++p->ordinal;
    pre_row(p, "op", "name=%s groups=%u,%u,%u indirect_off=%llu", g_dq_names[op], x, y, z, (unsigned long long)off);
}

static unsigned pre_claim(const struct pre_record *p, unsigned *slot) {
    if (!p->phase || !pre_cfg.bits || p->preamble || p->gap >= 1024) return 0;
    unsigned take = 0;
    pthread_mutex_lock(&pre_mu);
    *slot = pre_cfg.slot + 1;
    if (pre_cfg.frame == p->frame && pre_cfg.claimed < PRE_TARGETS / PRE_ROTATIONS) {
        const unsigned key = *slot * 1024 + p->gap;
        for (unsigned i = p->rotation; i < PRE_TARGETS; i += PRE_ROTATIONS) {
            if (pre_cfg.targets[i] != key) continue;
            ++pre_cfg.claimed;
            take = 1;
            break;
        }
    }
    pthread_mutex_unlock(&pre_mu);
    return take;
}

static void pre_stamp(const struct pre_record *p, unsigned q) {
    // Bypass the observation hook, in the same DQ stream as the real opcode.
    w_vkCmdWriteTimestamp(p->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, p->queries, q);
}

static void pre_end_pass(VkCommandBuffer cb) {
    struct pre_record *p = pre_lookup(cb);
    if (!p || !p->passes) return;
    p->gap = p->passes;
    p->ordinal = 0;
    unsigned slot;
    if (!pre_claim(p, &slot)) return;
    if (!p->queries) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * PRE_TARGETS};
        if (vkCreateQueryPool(g.d, &qi, NULL, &p->queries) != VK_SUCCESS) { ++p->lost; return; }
        w_vkCmdResetQueryPool(cb, p->queries, 0, qi.queryCount);
    }
    const unsigned i = p->pairs++;
    p->pair[i] = (struct pre_pair){.gap = p->gap, .expected_slot = slot, .end_shape = p->shape, .empty = !(p->phase & 1)};
    p->open_pair = i + 1;
    pre_stamp(p, 2 * i);
    if (p->pair[i].empty) pre_stamp(p, 2 * i + 1);
}

static void pre_begin_pass(VkCommandBuffer cb, const VkRect2D *area, unsigned attachments, unsigned dynamic) {
    struct pre_record *p = pre_lookup(cb);
    if (!p) return;
    const uint64_t shape = ((uint64_t)area->extent.width << 40) | ((uint64_t)area->extent.height << 16) | (attachments << 1) | dynamic;
    if (p->open_pair) {
        const unsigned i = p->open_pair - 1;
        struct pre_pair *q = &p->pair[i];
        if (!q->empty) pre_stamp(p, 2 * i + 1);
        q->begin_shape = shape;
        q->closed = 1;
        p->open_pair = 0;
    }
    if (!p->passes) {
        p->len = 0;   // The leading no-pass survey cannot claim this CB.
        p->lost = 0;
    }
    if (p->gap && !p->phase) {
        pre_row(p, "gap", "ops=%u end_shape=%llx begin_shape=%llx", p->ordinal,
                (unsigned long long)p->shape, (unsigned long long)shape);
    }
    p->complete_len = p->len;   // Only closed gaps will be emitted at retirement.
    p->shape = shape;
    ++p->passes;
    p->gap = 0;
    p->ordinal = 0;
}

// Under the actual queue submission lock. Pre-opened/interleaved command
// buffers are attributed by the real submit order and frame, never open order.
static void pre_submitted(struct vk_segment *s, unsigned ncb) {
    if (!pre_cfg.delay) return;
    pthread_mutex_lock(&pre_mu);
    const unsigned slot = ++pre_cfg.slot;
    struct pre_record *p = s->cmd->pre;
    if (p && pre_cfg.sample && p->frame == pre_cfg.frame) {
        p->slot = slot;
        p->init = ncb > 1;
        p->emit = p->passes || (!p->phase && pre_cfg.leading && slot <= 2);
        if (!p->passes && p->init) p->emit = 0;
    }
    if (pre_cfg.sample && !pre_cfg.phase && pre_cfg.leading && slot <= 2) {
        if (!p || !p->emit || p->passes) {
            pre_cfg.leading = 0;
            s->pre_missing = slot;
            s->pre_frame = pre_cfg.frame;
        }
    }
    pthread_mutex_unlock(&pre_mu);
}

static void pre_wsi_tick(struct vk_segment *s, uint64_t now) {
    if (!pre_cfg.delay) return;
    if (!now) now = now_ns();
    pthread_mutex_lock(&pre_mu);
    if (!pre_cfg.first) pre_cfg.first = now;
    const uint64_t elapsed = (now - pre_cfg.first) / UINT64_C(1000000000);
    if (elapsed >= pre_cfg.delay) {
        const uint64_t local = elapsed - pre_cfg.delay;
        const uint64_t stage = local < pre_cfg.count_seconds ? 0 : pre_cfg.arm_seconds
            ? 1 + (local - pre_cfg.count_seconds) / pre_cfg.arm_seconds : 5;
        const unsigned phase = stage > 5 ? 5 : (unsigned)stage;
        if (phase != pre_cfg.phase) {
            pre_cfg.phase = phase;
            pre_cfg.start_frame = s->wsi_seq + 1;
            s->pre_cpu_ns = now;
            s->pre_phase = phase;
        }
        if (pre_cfg.sample) {
            s->pre_frame = pre_cfg.frame;
            s->pre_frame_submits = pre_cfg.slot;
            s->pre_frame_pairs = pre_cfg.claimed;
        }
        const uint64_t rel = s->wsi_seq + 1 - pre_cfg.start_frame;
        pre_cfg.sample = !phase ? rel < PRE_SURVEY_FRAMES :
            phase < 5 && rel >= PRE_TIME_FIRST && rel < PRE_TIME_FIRST + PRE_TIME_FRAMES;
        pre_cfg.rotation = rel % PRE_ROTATIONS;
        pre_cfg.slot = pre_cfg.claimed = 0;
        pre_cfg.leading = 1;
        pre_cfg.frame = s->wsi_seq + 1;
    }
    pthread_mutex_unlock(&pre_mu);
}

static void pre_finish(const struct vk_segment *s, int failed) {
    char line[680];
    if (s->internal) {
        if (s->pre_frame) {
            snprintf(line, sizeof line, "frame=%llu wsi=%llu ordinary=%u pairs=%u failed=%u",
                     (unsigned long long)s->pre_frame, (unsigned long long)s->wsi_seq,
                     s->pre_frame_submits, s->pre_frame_pairs, failed != 0);
            pre_buffer(line);
        }
        if (s->pre_cpu_ns) {
            pre_flush();
            snprintf(line, sizeof line, "phase=%u frame=%llu cpu_ns=%llu bits=%u period_ns=%.9g",
                     s->pre_phase, (unsigned long long)(s->wsi_seq + 1), (unsigned long long)s->pre_cpu_ns,
                     pre_cfg.bits, (double)g.dev->properties.limits.timestampPeriod);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "pre-trace", line);
        }
        return;
    }
    if (s->pre_missing) {
        snprintf(line, sizeof line, "missing frame=%llu signal=%llu leading_slot=%u",
                 (unsigned long long)s->pre_frame, (unsigned long long)s->signal, s->pre_missing);
        pre_buffer(line);
    }
    const struct pre_record *p = s->cmd ? s->cmd->pre : NULL;
    if (!p || !p->emit) return;
    const unsigned long long frame = (unsigned long long)p->frame, signal = (unsigned long long)s->signal;
    if (!p->phase) {
        snprintf(line, sizeof line, "submit frame=%llu signal=%llu slot=%u role=%s passes=%u init=%u lost=%u failed=%u",
                 frame, signal, p->slot, p->preamble ? "pre" : "normal", p->passes, p->init, p->lost, failed != 0);
        pre_buffer(line);
        const size_t count = p->passes ? p->complete_len : p->len;
        for (size_t i = 0; i < count; i += strlen(p->rows + i) + 1) {
            snprintf(line, sizeof line, "%s frame=%llu signal=%llu", p->rows + i, frame, signal);
            pre_buffer(line);
        }
    }
    if (p->phase && p->lost) {
        snprintf(line, sizeof line, "missing frame=%llu signal=%llu slot=%u phase=%u query_create=%u",
                 frame, signal, p->slot, p->phase, p->lost);
        pre_buffer(line);
    }
    for (unsigned i = 0; i < p->pairs; ++i) {
        const struct pre_pair *q = &p->pair[i];
        uint64_t ticks[2][2] = {{0}};
        int valid = !failed && q->closed && q->expected_slot == p->slot &&
            vkGetQueryPoolResults(g.d, p->queries, 2 * i, 2, sizeof ticks, ticks, sizeof ticks[0],
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS &&
            ticks[0][1] && ticks[1][1];
        const uint64_t mask = pre_cfg.bits == 64 ? UINT64_MAX : (UINT64_C(1) << pre_cfg.bits) - 1;
        const uint64_t delta = valid ? (ticks[1][0] - ticks[0][0]) & mask : 0;
        snprintf(line, sizeof line, "sample frame=%llu signal=%llu slot=%u predicted=%u phase=%u gap=%u empty=%u "
                 "closed=%u valid=%u ticks=%llu end_shape=%llx begin_shape=%llx",
                 frame, signal, p->slot, q->expected_slot, p->phase, q->gap, q->empty, q->closed, valid,
                 (unsigned long long)delta, (unsigned long long)q->end_shape, (unsigned long long)q->begin_shape);
        pre_buffer(line);
    }
}
