// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

int nvn_gpu_mirror_memory_cached(nvn_gpu_buf b) {
    return b && (b->memory_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
}

static int mirror_is_noncoherent(nvn_gpu_buf b) {
    return b && b->mem_kind == NVN_GPU_MEM_POOL_MIRROR &&
           !(b->memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

static uint32_t mirror_memory_type(const struct s2a_vk_device *dev, const VkMemoryRequirements *req) {
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t bits = req->memoryTypeBits;
    for (uint32_t i = 0; i < dev->memory.memoryTypeCount; i++) {
        const uint32_t heap = dev->memory.memoryTypes[i].heapIndex;
        if (req->size > dev->memory.memoryHeaps[heap].size) bits &= ~(1u << i);
    }
    const VkDeviceSize atom = dev->properties.limits.nonCoherentAtomSize;
    const VkMemoryPropertyFlags choices[] = {cached | coherent | local, cached | coherent, cached | local, cached};
    const unsigned count = atom && !(atom & (atom - 1)) ? 4 : 2;
    for (unsigned i = 0; i < count; i++) {
        const uint32_t type = s2a_vk_memory_type(dev, bits, choices[i], NULL);
        if (type != UINT32_MAX) return type;
    }
    return UINT32_MAX;
}

static int mirror_range(nvn_gpu_buf b, VkDeviceSize off, VkDeviceSize bytes, VkMappedMemoryRange *out) {
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    if (!bytes || off > b->bytes || bytes > b->bytes - off || !atom || (atom & (atom - 1))) return 0;
    const VkDeviceSize lo = off & ~(atom - 1);
    const VkDeviceSize end = off + bytes;
    if (end > b->allocation_bytes) return 0;
    const VkDeviceSize padding = (0 - end) & (atom - 1);
    // 镜像绑定在专用分配第 0 字节，映射覆盖全分配。到分配末尾时允许最后一颗原子不满。
    const VkDeviceSize hi = padding > b->allocation_bytes - end ? b->allocation_bytes : end + padding;
    *out = (VkMappedMemoryRange){
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->memory, .offset = lo, .size = hi - lo,
    };
    return 1;
}

int nvn_gpu_mirror_visibility_enabled(void) {
    return g_open && atomic_load_explicit(&g.mirror_noncoherent_seen, memory_order_relaxed);
}

int nvn_gpu_mirror_state(nvn_gpu_buf b) {
    if (!mirror_is_noncoherent(b)) return NVN_GPU_MIRROR_COHERENT;
    pthread_mutex_lock(&g_mu);
    const int idle = g.completed >= b->mirror_use_fence && !g.mirror_flush_error && !g.failed;
    pthread_mutex_unlock(&g_mu);
    return idle ? NVN_GPU_MIRROR_CPU_IDLE : NVN_GPU_MIRROR_GPU_BUSY;
}

int nvn_gpu_mirror_flush(nvn_gpu_buf b, uint64_t off, uint64_t bytes) {
    VkMappedMemoryRange range;
    const uint64_t t0 = now_ns();
    VkResult result = VK_ERROR_UNKNOWN;
    if (nvn_gpu_mirror_state(b) == NVN_GPU_MIRROR_CPU_IDLE && mirror_range(b, off, bytes, &range))
        result = vkFlushMappedMemoryRanges(g.d, 1, &range);
    pthread_mutex_lock(&g_mu);
    if (result == VK_SUCCESS) {
        g.stats[STAT_MIRROR_FLUSH_N]++;
        g.stats[STAT_MIRROR_FLUSH_PAYLOAD] += bytes;
        g.stats[STAT_MIRROR_FLUSH_BYTES] += range.size;
        g.stats[STAT_MIRROR_FLUSH_NS] += now_ns() - t0;
    } else if (g.mirror_flush_error == VK_SUCCESS) g.mirror_flush_error = result;
    pthread_mutex_unlock(&g_mu);
    return result == VK_SUCCESS;
}

static int compare_mirror_range(const void *pa, const void *pb) {
    const struct vk_mirror_range *a = pa, *b = pb;
    const uintptr_t x = (uintptr_t)a->buf, y = (uintptr_t)b->buf;
    if (x != y) return (x > y) - (x < y);
    return (a->range.offset > b->range.offset) - (a->range.offset < b->range.offset);
}

void nvn_gpu_mirror_visibility_record(nvn_gpu_cmd c, const nvn_gpu_mirror_span *spans, uint32_t count) {
    if (!c || !count || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!spans || c->mirror_ranges || c->in_pass) {
        set_sticky(c, spans ? NVN_GPU_E_ORDER : NVN_GPU_E_OOM, "缓存镜像回写快照不可用");
        return;
    }
    uint32_t needed = 0;
    for (uint32_t i = 0; i < count; i++) needed += mirror_is_noncoherent(spans[i].src);
    if (!needed) return;
    struct vk_mirror_range *ranges = calloc(needed, sizeof *ranges);
    if (!ranges) {
        set_sticky(c, NVN_GPU_E_OOM, "缓存镜像回写范围分配失败");
        return;
    }
    c->mirror_ranges = ranges;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const nvn_gpu_mirror_span *s = &spans[i];
        if (!mirror_is_noncoherent(s->src)) continue;
        if (!mirror_range(s->src, s->offset, s->bytes, &ranges[n].range)) {
            set_sticky(c, NVN_GPU_E_ARG, "缓存镜像回写范围越界");
            return;
        }
        ranges[n++].buf = s->src;
        c->mirror_payload_bytes += s->bytes;
    }
    qsort(ranges, n, sizeof *ranges, compare_mirror_range);
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct vk_mirror_range r = ranges[i];
        struct vk_mirror_range *last = used ? &ranges[used - 1] : NULL;
        if (!last || last->buf != r.buf) cmd_ref(c, r.buf, REF_BUF);
        else if (r.range.offset <= last->range.offset + last->range.size) {
            const VkDeviceSize hi = r.range.offset + r.range.size;
            if (hi > last->range.offset + last->range.size) last->range.size = hi - last->range.offset;
            continue;
        }
        ranges[used++] = r;
    }
    c->mirror_range_count = used;
}

static VkResult mirror_invalidate(struct nvn_gpu_vk *vk, const struct nvn_gpu_cmd_s *c) {
    if (!c || !c->mirror_range_count) return VK_SUCCESS;
    const uint64_t t0 = now_ns();
    uint64_t bytes = 0, calls = 0;
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < c->mirror_range_count;) {
        VkMappedMemoryRange batch[32];
        uint32_t n = 0;
        while (i < c->mirror_range_count && n < sizeof batch / sizeof batch[0]) {
            batch[n] = c->mirror_ranges[i++].range;
            bytes += batch[n++].size;
        }
        result = vkInvalidateMappedMemoryRanges(vk->d, n, batch);
        calls++;
        if (result != VK_SUCCESS) break;
    }
    pthread_mutex_lock(&g_mu);
    vk->stats[STAT_MIRROR_INV_N] += calls;
    vk->stats[STAT_MIRROR_INV_PAYLOAD] += c->mirror_payload_bytes;
    vk->stats[STAT_MIRROR_INV_BYTES] += bytes;
    vk->stats[STAT_MIRROR_INV_NS] += now_ns() - t0;
    pthread_mutex_unlock(&g_mu);
    return result;
}

static nvn_gpu_buf mirror_ref(struct vk_ref ref) {
    if (ref.type == REF_BUF) return ref.obj;
    if (ref.type == REF_VIEW) return ((const struct nvn_gpu_view_s *)ref.obj)->buf;
    return NULL;
}

// 按镜像独占，含纹素视图。提交线程等软件完成值，原生 fence 到达后 CPU 仍要 invalidate 和落地。
static void mirror_wait_for_landing(struct nvn_gpu_cmd_s *c) {
    uint64_t fence = 0;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < c->nrefs; i++) {
        nvn_gpu_buf b = mirror_ref(c->refs[i]);
        if (mirror_is_noncoherent(b) && b->mirror_land_fence > fence) fence = b->mirror_land_fence;
    }
    const int wait = fence > g.completed;
    if (wait) g.stats[STAT_MIRROR_WAIT_N]++;
    pthread_mutex_unlock(&g_mu);
    if (!wait) return;

    const uint64_t t0 = now_ns();
    nvn_gpu_err err = {0};
    const nvn_gpu_rc result = nvn_gpu_fence_wait(fence, 5000000000ull, &err);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_MIRROR_WAIT_NS] += now_ns() - t0;
    pthread_mutex_unlock(&g_mu);
    if (!nvn_gpu_ok(result)) set_sticky(c, result.code, "缓存镜像等回写失败（%llu）：%s",
                                       (unsigned long long)fence, err.why);
}

static void mirror_mark_submitted(struct nvn_gpu_cmd_s *c, uint64_t signal) {
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < c->nrefs; i++) {
        nvn_gpu_buf b = mirror_ref(c->refs[i]);
        if (mirror_is_noncoherent(b)) b->mirror_use_fence = signal;
    }
    for (uint32_t i = 0; i < c->mirror_range_count; i++) c->mirror_ranges[i].buf->mirror_land_fence = signal;
    pthread_mutex_unlock(&g_mu);
}
