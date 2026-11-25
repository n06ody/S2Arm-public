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

static void mirror_write(struct nvn_gpu_cmd_s *c, nvn_gpu_buf b, VkDeviceSize off, VkDeviceSize bytes,
                         enum mirror_write_kind kind) {
    if (!mirror_is_noncoherent(b) || !rc_is(c->sticky, NVN_GPU_OK)) return;
    VkMappedMemoryRange range;
    if (!mirror_range(b, off, bytes, &range)) {
        set_sticky(c, NVN_GPU_E_ARG, "缓存镜像写入范围越界");
        return;
    }
    const VkDeviceSize lo = range.offset, hi = lo + range.size;
    if (c->mirror_write_count) {
        struct vk_mirror_write *last = &c->mirror_writes[c->mirror_write_count - 1];
        if (last->buf == b && last->kind == kind && lo <= last->hi && hi >= last->lo) {
            if (lo < last->lo) last->lo = lo;
            if (hi > last->hi) last->hi = hi;
            return;
        }
    }
    if (c->mirror_write_count == c->mirror_write_cap) {
        const size_t cap = c->mirror_write_cap ? (size_t)c->mirror_write_cap * 2 : 8;
        struct vk_mirror_write *writes = cap <= UINT32_MAX && cap <= SIZE_MAX / sizeof *writes
            ? realloc(c->mirror_writes, cap * sizeof *writes) : NULL;
        if (!writes) { set_sticky(c, NVN_GPU_E_OOM, "缓存镜像写入范围分配失败"); return; }
        c->mirror_writes = writes;
        c->mirror_write_cap = (uint32_t)cap;
    }
    c->mirror_writes[c->mirror_write_count++] = (struct vk_mirror_write){b, lo, hi, kind};
}

// 按 Vulkan 实际编码的行长和切片高记写集。行间填充从未由设备写入。
static void mirror_write_rows(struct nvn_gpu_cmd_s *c, nvn_gpu_buf b, uint64_t off, uint64_t packed,
                              uint64_t pitch, uint32_t rows, uint32_t slice_rows, uint32_t slices) {
    if (!rows || !slices || !packed || packed > pitch || rows > slice_rows ||
        off > b->bytes || packed > b->bytes - off ||
        (uint64_t)(slices - 1) * slice_rows + (rows - 1) > (b->bytes - off - packed) / pitch) {
        set_sticky(c, NVN_GPU_E_ARG, "缓存镜像图像读回范围越界");
        return;
    }
    if (packed == pitch && (rows == slice_rows || slices == 1)) {
        mirror_write(c, b, off, packed * rows * slices, MIRROR_TRANSFER);
        return;
    }
    for (uint32_t z = 0; z < slices; z++) {
        const uint64_t row = (uint64_t)z * slice_rows;
        for (uint32_t y = 0; y < rows; y++)
            mirror_write(c, b, off + (row + y) * pitch, packed, MIRROR_TRANSFER);
    }
}

static void mirror_storage_writes(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute) {
    const int first = compute ? NVN_GPU_SH_CS : NVN_GPU_SH_VS;
    const int end = compute ? NVN_GPU_SH_CS + 1 : NVN_GPU_SH_CS;
    for (int st = first; st < end; st++) {
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if ((b->cls != NVN_GPU_RES_SSBO && b->cls != NVN_GPU_RES_IMAGE_TBUF) ||
                b->ent >= p->stage_nent[st]) continue;
            const nvn_gpu_layout_ent *entry = &p->stage_ents[st][b->ent];
            if (entry->cls != b->cls || entry->access == NVN_GPU_ACC_R) continue;
            if (b->cls == NVN_GPU_RES_SSBO) {
                mirror_write(c, b->u.b.buf, b->u.b.off, b->u.b.len, MIRROR_STORAGE);
            } else if (b->u.view) {
                const struct nvn_gpu_view_s *v = b->u.view;
                mirror_write(c, v->buf, v->buffer_off, v->buffer_bytes, MIRROR_STORAGE);
            }
        }
    }
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

enum { MIRROR_PROBE_SPANS = 256, MIRROR_PROBE_OVERLAPS = 128, MIRROR_PROBE_SCANS = 2048 };

static int compare_mirror_edge(const void *pa, const void *pb) {
    const struct vk_mirror_edge *a = pa, *b = pb;
    const uintptr_t x = (uintptr_t)a->buf, y = (uintptr_t)b->buf;
    if (x != y) return (x > y) - (x < y);
    return (a->off > b->off) - (a->off < b->off);
}

static void mirror_record_edge(struct nvn_gpu_cmd_s *c, nvn_gpu_buf buf, VkDeviceSize off) {
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    if (!(off % atom)) return;
    const VkDeviceSize lo = off - off % atom;
    if (atom <= buf->bytes - lo)
        c->mirror_edges[c->mirror_edge_count++] = (struct vk_mirror_edge){buf, lo};
}

static void mirror_visibility_record(nvn_gpu_cmd c, const nvn_gpu_mirror_span *spans, uint32_t count,
                                     int copy_callback) {
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
    c->mirror_edge_copy = g.land_edge_snapshot && copy_callback;
    // 边界元数据不可用时保留所有原映射的 invalidate 和等待。
    if ((g.land_wait_probe || c->mirror_edge_copy) && needed <= MIRROR_PROBE_SPANS)
        c->mirror_edges = malloc((size_t)2 * needed * sizeof *c->mirror_edges);
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const nvn_gpu_mirror_span *s = &spans[i];
        if (!mirror_is_noncoherent(s->src)) continue;
        if (!mirror_range(s->src, s->offset, s->bytes, &ranges[n].range)) {
            set_sticky(c, NVN_GPU_E_ARG, "缓存镜像回写范围越界");
            return;
        }
        ranges[n].buf = s->src;
        ranges[n++].owner = c;
        if (c->mirror_edges) {
            mirror_record_edge(c, s->src, s->offset);
            mirror_record_edge(c, s->src, s->offset + s->bytes);
        }
        c->mirror_payload_bytes += s->bytes;
    }
    if (c->mirror_edge_count > 1) {
        qsort(c->mirror_edges, c->mirror_edge_count, sizeof *c->mirror_edges, compare_mirror_edge);
        uint32_t unique = 1;
        for (uint32_t i = 1; i < c->mirror_edge_count; i++)
            if (compare_mirror_edge(&c->mirror_edges[i], &c->mirror_edges[unique - 1]))
                c->mirror_edges[unique++] = c->mirror_edges[i];
        c->mirror_edge_count = unique;
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

void nvn_gpu_mirror_visibility_record(nvn_gpu_cmd c, const nvn_gpu_mirror_span *spans, uint32_t count) {
    mirror_visibility_record(c, spans, count, 0);
}

void nvn_gpu_mirror_visibility_record_copy(nvn_gpu_cmd c, const nvn_gpu_mirror_span *spans, uint32_t count) {
    mirror_visibility_record(c, spans, count, 1);
}

static uint32_t mirror_edge_lower_bound(const struct nvn_gpu_cmd_s *c, nvn_gpu_buf b, VkDeviceSize off) {
    const struct vk_mirror_edge key = {b, off};
    uint32_t first = 0, end = c->mirror_edge_count;
    while (first < end) {
        const uint32_t mid = first + (end - first) / 2;
        if (compare_mirror_edge(&c->mirror_edges[mid], &key) < 0) first = mid + 1;
        else end = mid;
    }
    return first;
}

static void mirror_slot_destroy(struct vk_mirror_slot *slot) {
    if (slot->mapped) vkUnmapMemory(g.d, slot->memory);
    if (slot->buffer) vkDestroyBuffer(g.d, slot->buffer, NULL);
    if (slot->memory) vkFreeMemory(g.d, slot->memory, NULL);
    *slot = (struct vk_mirror_slot){0};
}

static void mirror_slots_close(void) {
    for (uint32_t i = 0; i < MIRROR_EDGE_SLOTS; i++) mirror_slot_destroy(&g.mirror_slots[i]);
}

static VkResult mirror_slot_create(struct vk_mirror_slot *slot, int *over_budget) {
    const VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MIRROR_EDGE_SLOT_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g.d, &bi, NULL, &buffer);
    if (r != VK_SUCCESS) return r;
    slot->buffer = buffer;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.d, slot->buffer, &req);
    if (req.size > MIRROR_EDGE_SLOT_BYTES) {
        *over_budget = 1;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags choices[] = {visible | cached | coherent, visible | cached, visible | coherent, visible};
    uint32_t type = UINT32_MAX;
    for (size_t i = 0; i < sizeof choices / sizeof choices[0] && type == UINT32_MAX; i++)
        type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, choices[i], NULL);
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    slot->coherent = (g.dev->memory.memoryTypes[type].propertyFlags & coherent) != 0;
    const VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    r = vkAllocateMemory(g.d, &ai, NULL, &memory);
    if (r != VK_SUCCESS) return r;
    slot->memory = memory;
    r = vkBindBufferMemory(g.d, slot->buffer, slot->memory, 0);
    if (r != VK_SUCCESS) return r;
    void *mapped = NULL;
    r = vkMapMemory(g.d, slot->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r == VK_SUCCESS) slot->mapped = mapped;
    return r;
}

// 仅从旧段末尾拷贝候选边界。本段拥有槽到完成回调结束，后继段无法改写这些数据。
static void mirror_record_snapshot(struct nvn_gpu_cmd_s *c) {
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    if (!c->mirror_edges || !c->mirror_edge_count) {
        pthread_mutex_lock(&g_mu);
        g.stats[c->mirror_edges ? STAT_MIRROR_EDGE_NO_EDGE : STAT_MIRROR_EDGE_UNKNOWN]++;
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (atom < 4 || c->mirror_edge_count > MIRROR_EDGE_REGIONS ||
        atom > MIRROR_EDGE_SLOT_BYTES / c->mirror_edge_count) {
        pthread_mutex_lock(&g_mu);
        g.stats[STAT_MIRROR_EDGE_BUDGET]++;
        pthread_mutex_unlock(&g_mu);
        return;
    }

    struct vk_mirror_slot *slot = NULL;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < MIRROR_EDGE_SLOTS; i++) {
        if (g.mirror_slots[i].busy) continue;
        slot = &g.mirror_slots[i];
        slot->busy = 1;
        slot->first_skip_ns = 0;
        break;
    }
    if (!slot) g.stats[STAT_MIRROR_EDGE_FULL]++;
    pthread_mutex_unlock(&g_mu);
    if (!slot) return;
    c->mirror_edge_reused = slot->buffer != VK_NULL_HANDLE;
    if (!c->mirror_edge_reused) {
        struct vk_mirror_slot fresh = {0};
        int over_budget = 0;
        VkResult r = mirror_slot_create(&fresh, &over_budget);
        if (r != VK_SUCCESS) {
            mirror_slot_destroy(&fresh);
            pthread_mutex_lock(&g_mu);
            slot->busy = 0;
            g.stats[over_budget ? STAT_MIRROR_EDGE_BUDGET : STAT_MIRROR_EDGE_ALLOC]++;
            pthread_mutex_unlock(&g_mu);
            if (r == VK_ERROR_DEVICE_LOST) set_sticky(c, NVN_GPU_E_DEVICE, "缓存镜像边界快照分配时设备丢失");
            return;
        }
        fresh.busy = 1;
        pthread_mutex_lock(&g_mu);
        *slot = fresh;
        pthread_mutex_unlock(&g_mu);
    }
    c->mirror_slot = slot;

    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copies[MIRROR_EDGE_REGIONS];
    for (uint32_t i = 0; i < c->mirror_edge_count;) {
        const nvn_gpu_buf b = c->mirror_edges[i].buf;
        uint32_t ncopy = 0;
        do {
            const VkDeviceSize off = c->mirror_edges[i].off;
            if (ncopy && copies[ncopy - 1].srcOffset + copies[ncopy - 1].size == off)
                copies[ncopy - 1].size += atom;
            else
                copies[ncopy++] = (VkBufferCopy){off, i * atom, atom};
            i++;
        } while (i < c->mirror_edge_count && c->mirror_edges[i].buf == b);
        vkCmdCopyBuffer(c->cb, b->buffer, slot->buffer, ncopy, copies);
    }
    // 显式 WAR 执行依赖：后继同队列的上传、传输或着色器写必须排在旧段快照读之后。
    // 段尾既有 ALL_COMMANDS -> HOST 屏障另负责槽的写入对 CPU 可见。
    mem_barrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0);
}

int nvn_gpu_mirror_copy(const nvn_gpu_done *done, nvn_gpu_buf b, uint64_t off, uint64_t bytes, void *dst) {
    const struct nvn_gpu_cmd_s *c = done->mirror_copy_context;
    if (!c) return 0;
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    uint32_t i = mirror_edge_lower_bound(c, b, off - off % atom);
    if (i == c->mirror_edge_count || c->mirror_edges[i].buf != b || c->mirror_edges[i].off >= off + bytes)
        return 0;

    uint8_t *data = dst;
    const VkDeviceSize end = off + bytes;
    while (i < c->mirror_edge_count && c->mirror_edges[i].buf == b && c->mirror_edges[i].off < end) {
        const VkDeviceSize edge = c->mirror_edges[i].off;
        if (off < edge) {
            memcpy(data, b->mapped + off, edge - off);
            data += edge - off;
            off = edge;
        }
        const VkDeviceSize size = (end < edge + atom ? end : edge + atom) - off;
        memcpy(data, c->mirror_slot->mapped + i * atom + off - edge, size);
        data += size;
        off += size;
        i++;
    }
    if (off < end) memcpy(data, b->mapped + off, end - off);
    return 1;
}

static int mirror_next_direct_range(const struct nvn_gpu_cmd_s *c, uint32_t *index, VkDeviceSize *cursor,
                                    VkMappedMemoryRange *out) {
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    while (*index < c->mirror_range_count) {
        const struct vk_mirror_range *r = &c->mirror_ranges[*index];
        *out = r->range;
        if (!c->mirror_slot) {
            ++*index;
            return 1;
        }
        const VkDeviceSize end = r->range.offset + r->range.size;
        VkDeviceSize lo = *cursor ? *cursor : r->range.offset;
        uint32_t i = mirror_edge_lower_bound(c, r->buf, lo);
        while (lo < end && i < c->mirror_edge_count && c->mirror_edges[i].buf == r->buf && c->mirror_edges[i].off == lo) {
            lo += atom;
            i++;
        }
        VkDeviceSize hi = end;
        if (i < c->mirror_edge_count && c->mirror_edges[i].buf == r->buf && c->mirror_edges[i].off < end)
            hi = c->mirror_edges[i].off;
        *cursor = hi == end ? 0 : hi;
        if (!*cursor) ++*index;
        if (lo < hi) {
            out->offset = lo;
            out->size = hi - lo;
            return 1;
        }
    }
    return 0;
}

static VkResult mirror_invalidate(struct nvn_gpu_vk *vk, const struct nvn_gpu_cmd_s *c) {
    if (!c || !c->mirror_range_count) return VK_SUCCESS;
    const uint64_t t0 = now_ns();
    uint64_t bytes = 0, calls = 0;
    VkResult result = VK_SUCCESS;
    VkDeviceSize cursor = 0;
    for (uint32_t i = 0; i < c->mirror_range_count;) {
        VkMappedMemoryRange batch[32];
        uint32_t n = 0;
        while (n < sizeof batch / sizeof batch[0] && mirror_next_direct_range(c, &i, &cursor, &batch[n]))
            bytes += batch[n++].size;
        if (!n) break;
        result = vkInvalidateMappedMemoryRanges(vk->d, n, batch);
        calls++;
        if (result != VK_SUCCESS) break;
    }
    if (result == VK_SUCCESS && c->mirror_slot && !c->mirror_slot->coherent) {
        const VkMappedMemoryRange slot = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = c->mirror_slot->memory,
            .size = c->mirror_edge_count * g.dev->properties.limits.nonCoherentAtomSize,
        };
        result = vkInvalidateMappedMemoryRanges(vk->d, 1, &slot);
        calls++;
        bytes += slot.size;
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

struct vk_mirror_overlap {
    const struct vk_mirror_range *land;
    VkDeviceSize lo, hi;
};

static int compare_mirror_overlap(const void *pa, const void *pb) {
    const struct vk_mirror_overlap *a = pa, *b = pb;
    const uintptr_t x = (uintptr_t)a->land, y = (uintptr_t)b->land;
    if (x != y) return (x > y) - (x < y);
    return (a->lo > b->lo) - (a->lo < b->lo);
}

static uint64_t mirror_edges_in(const struct vk_mirror_overlap *overlap) {
    const struct nvn_gpu_cmd_s *old = overlap->land->owner;
    const struct vk_mirror_edge a = {overlap->land->buf, overlap->lo};
    const struct vk_mirror_edge b = {overlap->land->buf, overlap->hi};
    uint32_t lo = 0, hi = old->mirror_edge_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (compare_mirror_edge(&old->mirror_edges[mid], &a) < 0) lo = mid + 1;
        else hi = mid;
    }
    uint32_t end = lo;
    while (end < old->mirror_edge_count && compare_mirror_edge(&old->mirror_edges[end], &b) < 0) end++;
    return end - lo;
}

// 持 g_mu。按待回写段、镜像和原子对所有冲突去重。即使只等最大的信号，
// 边界快照要消除这一笔等待，仍需覆盖更早的相交段。这里只量几何覆盖，不录复制。
static int mirror_probe_wait(const struct nvn_gpu_cmd_s *c) {
    struct vk_mirror_overlap overlaps[MIRROR_PROBE_OVERLAPS];
    uint32_t n = 0, scans = 0;
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    for (uint32_t i = 0; i < c->mirror_write_count; i++) {
        const struct vk_mirror_write *w = &c->mirror_writes[i];
        if (++scans > MIRROR_PROBE_SCANS) goto incomplete;
        for (const struct vk_mirror_range *r = w->buf->mirror_land; r && r->signal > g.completed; r = r->next) {
            if (++scans > MIRROR_PROBE_SCANS) goto incomplete;
            const VkDeviceSize end = r->range.offset + r->range.size;
            const VkDeviceSize lo = w->lo > r->range.offset ? w->lo : r->range.offset;
            const VkDeviceSize hi = w->hi < end ? w->hi : end;
            if (hi <= lo) continue;
            if (n == MIRROR_PROBE_OVERLAPS || !r->owner->mirror_edges) goto incomplete;
            overlaps[n++] = (struct vk_mirror_overlap){r, lo, hi};
        }
    }

    qsort(overlaps, n, sizeof *overlaps, compare_mirror_overlap);
    uint64_t atoms = 0, edges = 0;
    for (uint32_t i = 0; i < n;) {
        struct vk_mirror_overlap hit = overlaps[i++];
        while (i < n && overlaps[i].land == hit.land && overlaps[i].lo <= hit.hi) {
            if (overlaps[i].hi > hit.hi) hit.hi = overlaps[i].hi;
            i++;
        }
        atoms += (hit.hi - 1) / atom - hit.lo / atom + 1;
        edges += mirror_edges_in(&hit);
    }
    g.stats[STAT_MIRROR_PROBE_SAMPLED]++;
    g.stats[STAT_MIRROR_PROBE_ATOMS] += atoms;
    g.stats[STAT_MIRROR_PROBE_EDGE_ATOMS] += edges;
    if (!atoms || atoms != edges) return 0;
    g.stats[STAT_MIRROR_PROBE_EDGE_WAITS]++;
    return 1;

incomplete:
    g.stats[STAT_MIRROR_PROBE_DROPPED]++;
    return 0;
}

static int mirror_edge_covers(const struct vk_mirror_range *r, VkDeviceSize lo, VkDeviceSize hi) {
    const struct nvn_gpu_cmd_s *c = r->owner;
    if (!c->mirror_slot) return 0;
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    if ((hi - lo) % atom) return 0;   // 分配末尾不满的原子仍从原映射读取。
    const VkDeviceSize count = (hi - lo) / atom;
    const uint32_t i = mirror_edge_lower_bound(c, r->buf, lo);
    if (count > c->mirror_edge_count - i) return 0;
    const struct vk_mirror_edge *first = &c->mirror_edges[i], *last = first + count - 1;
    // 已按物理缓冲和原子排序且去重；两端位置和个数相等才可能没有洞。
    return first->buf == r->buf && first->off == lo && last->buf == r->buf && last->off == hi - atom;
}

// 依 vkMapMemory 的访问约束，主机 invalidate 与拷贝会读取非一致原子。
// 后继只读可与它并行，写同一原子的段须等回调返回。
// 列表可能同时有多个前驱。用最大的相交信号等待，队列内更早的相交段也会先完成。
static void mirror_wait_for_landing(struct nvn_gpu_cmd_s *c) {
    uint64_t fence = 0;
    int edge_wait = 0;
    uint64_t skipped_fence = 0;
    struct vk_mirror_slot *skipped = NULL;
    enum mirror_write_kind reason = MIRROR_INDIRECT;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < c->mirror_write_count; i++) {
        const struct vk_mirror_write *w = &c->mirror_writes[i];
        for (const struct vk_mirror_range *r = w->buf->mirror_land; r; r = r->next) {
            if (r->signal <= g.completed || r->signal <= fence) break;
            const VkDeviceSize end = r->range.offset + r->range.size;
            const VkDeviceSize lo = w->lo > r->range.offset ? w->lo : r->range.offset;
            const VkDeviceSize hi = w->hi < end ? w->hi : end;
            if (lo >= hi) continue;
            if (mirror_edge_covers(r, lo, hi)) {
                if (r->signal > skipped_fence) {
                    skipped_fence = r->signal;
                    skipped = r->owner->mirror_slot;
                }
                continue;
            }
            fence = r->signal;
            reason = w->kind;
            break;
        }
    }
    if (fence) {
        g.stats[STAT_MIRROR_WAIT_N]++;
        g.stats[STAT_MIRROR_WAIT_LAST_SEGMENT] = fence;
        g.stats[reason]++;
        if (g.land_wait_probe) {
            edge_wait = mirror_probe_wait(c);
            atomic_store(&g.mirror_wait_signal, fence);
        }
    } else if (skipped) {
        g.stats[STAT_MIRROR_EDGE_WAIT_ELIDED]++;
        if (!skipped->first_skip_ns) skipped->first_skip_ns = now_ns();
    }
    pthread_mutex_unlock(&g_mu);
    if (!fence) return;

    const uint64_t t0 = now_ns();
    nvn_gpu_err err = {0};
    // 客体段仍持提交顺序锁。队列可接收已提交渲染的内部 WSI 段。
    if (g.land_wait_yield_wsi) pthread_mutex_unlock(&g_submit_mu);
    const nvn_gpu_rc result = nvn_gpu_fence_wait(fence, 5000000000ull, &err);
    const uint64_t waited = g.land_wait_probe ? now_ns() - t0 : 0;
    if (g.land_wait_probe) atomic_store(&g.mirror_wait_signal, 0);
    if (g.land_wait_yield_wsi) pthread_mutex_lock(&g_submit_mu);
    const uint64_t elapsed = now_ns() - t0;
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_MIRROR_WAIT_NS] += elapsed;
    if (g.land_wait_probe && g.land_wait_yield_wsi) g.stats[STAT_MIRROR_WAIT_RELOCK_NS] += elapsed - waited;
    if (edge_wait && nvn_gpu_ok(result)) g.stats[STAT_MIRROR_PROBE_EDGE_WAIT_NS] += waited;
    pthread_mutex_unlock(&g_mu);
    if (!nvn_gpu_ok(result)) set_sticky(c, result.code, "缓存镜像等回写失败（%llu）：%s",
                                       (unsigned long long)fence, err.why);
}

static void mirror_mark_submitted(struct nvn_gpu_cmd_s *c, uint64_t signal) {
    pthread_mutex_lock(&g_mu);
    if (c->mirror_slot) {
        g.stats[STAT_MIRROR_EDGE_BYTES] += c->mirror_edge_count * g.dev->properties.limits.nonCoherentAtomSize;
        g.stats[STAT_MIRROR_EDGE_REUSE] += c->mirror_edge_reused;
    }
    for (uint32_t i = 0; i < c->nrefs; i++) {
        nvn_gpu_buf b = mirror_ref(c->refs[i]);
        if (mirror_is_noncoherent(b)) b->mirror_use_fence = signal;
    }
    for (uint32_t i = 0; i < c->mirror_range_count; i++) {
        struct vk_mirror_range *r = &c->mirror_ranges[i];
        r->signal = signal;
        r->link = &r->buf->mirror_land;
        r->next = *r->link;
        if (r->next) r->next->link = &r->next;
        *r->link = r;
    }
    pthread_mutex_unlock(&g_mu);
}

static void mirror_unlink_locked(struct nvn_gpu_cmd_s *c) {
    for (uint32_t i = 0; i < c->mirror_range_count; i++) {
        struct vk_mirror_range *r = &c->mirror_ranges[i];
        if (!r->link) continue;   // 未提交的段从未把范围挂到缓冲。
        *r->link = r->next;
        if (r->next) r->next->link = r->link;
    }
    if (c->mirror_slot) c->mirror_slot->busy = 0;
}
