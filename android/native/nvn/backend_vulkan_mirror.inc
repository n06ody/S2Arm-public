// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

int nvn_gpu_mirror_memory_cached(nvn_gpu_buf b) {
    return b && (b->memory_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
}

static int mirror_is_noncoherent(nvn_gpu_buf b) {
    return b && b->mem_kind == NVN_GPU_MEM_POOL_MIRROR &&
           !(b->memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

uint64_t nvn_gpu_mirror_version_atom(nvn_gpu_buf b) {
    if (!g.land_upload_versions || !mirror_is_noncoherent(b)) return 0;
    return g.dev->properties.limits.nonCoherentAtomSize;
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

// 依 vkMapMemory 的访问约束，主机 invalidate 与拷贝会读取非一致原子。
// 后继只读可与它并行，写同一原子的段须等回调返回。
// 列表可能同时有多个前驱。用最大的相交信号等待，队列内更早的相交段也会先完成。
static void mirror_wait_for_landing(struct nvn_gpu_cmd_s *c) {
    uint64_t fence = 0;
    enum mirror_write_kind reason = MIRROR_INDIRECT;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < c->mirror_write_count; i++) {
        const struct vk_mirror_write *w = &c->mirror_writes[i];
        for (const struct vk_mirror_range *r = w->buf->mirror_land; r; r = r->next) {
            if (r->signal <= g.completed || r->signal <= fence) break;
            if (w->lo >= r->range.offset + r->range.size || r->range.offset >= w->hi) continue;
            fence = r->signal;
            reason = w->kind;
            break;
        }
    }
    if (fence) {
        g.stats[STAT_MIRROR_WAIT_N]++;
        g.stats[STAT_MIRROR_WAIT_LAST_SEGMENT] = fence;
        g.stats[reason]++;
    }
    pthread_mutex_unlock(&g_mu);
    if (!fence) return;

    const uint64_t t0 = now_ns();
    nvn_gpu_err err = {0};
    // 客体提交顺序仍由 g_guest_commit_mu 保证。回写未完成时允许无关的 WSI 提交。
    if (g.land_upload_versions) pthread_mutex_unlock(&g_submit_mu);
    const nvn_gpu_rc result = nvn_gpu_fence_wait(fence, 5000000000ull, &err);
    if (g.land_upload_versions) pthread_mutex_lock(&g_submit_mu);
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
}
