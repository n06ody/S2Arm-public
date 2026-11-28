// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 大块纹理上传暂存池 + 宿主纹理上传窄屏障与多区域 vkCmdCopyBufferToImage 合并
// 开关：debug.s2a.nvk_tex_upload_pool（缺省 0，nvn_gpu_open 锁存；0 = 100% 退回原路径）。
// 动机（通用 Vulkan 1.1，无任何设备/游戏特化）：
//   1) 原 seg_give_stg 只留 VK_ARENA_MIN(256KB) 标准块，所有 >256KB 的纹理/BC/解排上传每段新建
//      vkCreateBuffer+vkAllocateMemory+vkMapMemory 并在段末立刻销毁，引发高频驱动分配与缺页。
//      这里按 2 的幂桶（512KB..16MB，总空闲上限 32MB、LRU 淘汰）复用大块暂存缓冲，会话收尾随 g.bufs 统一销毁。
//   2) nvn_gpu_tex_upload 从宿主 memcpy 写入的新鲜暂存片拷贝到贴图，原先走 source_ready=0 逐 mip/层插
//      full_barrier(ALL_COMMANDS) + GENERAL↔TRANSFER_DST 往返屏障 + 单区域 vkCmdCopyBufferToImage。
//      Vulkan 1.1 中提交前宿主写对设备隐式可见，无需 full_barrier；同目标贴图连续不相交子区保持
//      TRANSFER_DST_OPTIMAL 并合入单条多区域 vkCmdCopyBufferToImage（上限 64 段），重叠子区插窄
//      TRANSFER_WRITE→TRANSFER_WRITE 图像 WAW 屏障，任何其它录制原语或段末前刷新并恢复 GENERAL。

enum {
    TEX_STG_MIN = 512u << 10,       // 512 KiB（> VK_ARENA_MIN = 256 KiB）
    TEX_STG_MAX = 16u << 20,        // 16 MiB 单块上限
    TEX_STG_SLOTS = 32,             // 空闲池最大块数
};
static const uint64_t TEX_STG_CAP_BYTES = 32ull << 20;   // 32 MiB 空闲池总字节上限

enum {
    TP_UPLOADS = 0,     // 宿主 tex_upload 拷贝次数
    TP_FULL_BAR,        // 宿主 tex_upload 触发的 full_barrier 次数（开池后应为 0）
    TP_IMG_BAR,         // 宿主 tex_upload 触发的图像屏障次数
    TP_COPY_CMD,        // 实际录制的 vkCmdCopyBufferToImage 条数
    TP_WAW,             // 同图重叠子区触发的窄 WAW 屏障次数
    TP_STG_HIT,         // 大块（>256KB）暂存池命中次数
    TP_STG_MISS,        // 大块（>256KB）暂存池未命中（新建驱动缓冲）次数
    TP_STG_EVICT,       // 达上限 LRU 淘汰的大块暂存数
    TP_N
};
static uint64_t g_tp_stat[TP_N];
static struct nvn_gpu_buf_s *g_tex_stg[TEX_STG_SLOTS];   // 持 g_seg_mu；[0] 最久未用，尾部最新归还
static uint32_t g_tex_nstg;
static uint64_t g_tex_stg_bytes;

static uint64_t tex_stg_round_cap(uint64_t cap) {
    if (!g.tex_upload_pool || cap <= VK_ARENA_MIN || cap > TEX_STG_MAX) return cap;
    uint64_t b = TEX_STG_MIN;
    while (b < cap && b < TEX_STG_MAX) b <<= 1;
    return b;
}

static int tex_stg_is_bucket(uint64_t bytes) {
    return bytes >= TEX_STG_MIN && bytes <= TEX_STG_MAX && (bytes & (bytes - 1)) == 0;
}

static void tex_stg_open(void) {
    for (int i = 0; i < TP_N; i++) __atomic_store_n(&g_tp_stat[i], 0, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_seg_mu);
    g_tex_nstg = 0;
    g_tex_stg_bytes = 0;
    pthread_mutex_unlock(&g_seg_mu);
}

static void tex_stg_note_miss(void) {
    __atomic_fetch_add(&g_tp_stat[TP_STG_MISS], 1, __ATOMIC_RELAXED);
}

static void tex_stg_drain_locked(void) {
    // 缓冲均挂在 g.bufs 链上，由 nvn_gpu_close 统一销毁（与 g_seg_stg 同口径）
    g_tex_nstg = 0;
    g_tex_stg_bytes = 0;
}

static struct nvn_gpu_buf_s *tex_stg_take(uint64_t bytes) {
    struct nvn_gpu_buf_s *b = NULL;
    pthread_mutex_lock(&g_seg_mu);
    // 从尾部（最近归还、TLB/缓存更热）找同桶大小
    for (uint32_t i = g_tex_nstg; i > 0; i--) {
        if (g_tex_stg[i - 1]->bytes == bytes) {
            b = g_tex_stg[i - 1];
            memmove(&g_tex_stg[i - 1], &g_tex_stg[i], (size_t)(g_tex_nstg - i) * sizeof *g_tex_stg);
            g_tex_nstg--;
            g_tex_stg_bytes -= bytes;
            break;
        }
    }
    pthread_mutex_unlock(&g_seg_mu);
    __atomic_fetch_add(&g_tp_stat[b ? TP_STG_HIT : TP_STG_MISS], 1, __ATOMIC_RELAXED);
    return b;
}

static int tex_stg_give(struct nvn_gpu_buf_s *b) {
    if (!g.tex_upload_pool || !b || !b->mapped || !seg_reuse_on() || !tex_stg_is_bucket(b->bytes)) return 0;
    struct nvn_gpu_buf_s *evict[TEX_STG_SLOTS];
    uint32_t nevict = 0;
    int kept = 0;
    pthread_mutex_lock(&g_seg_mu);
    if (!g_seg_closed && b->bytes <= TEX_STG_CAP_BYTES) {
        while (g_tex_nstg > 0 && (g_tex_nstg >= TEX_STG_SLOTS || g_tex_stg_bytes + b->bytes > TEX_STG_CAP_BYTES)) {
            struct nvn_gpu_buf_s *old = g_tex_stg[0];
            memmove(&g_tex_stg[0], &g_tex_stg[1], (size_t)(g_tex_nstg - 1) * sizeof *g_tex_stg);
            g_tex_nstg--;
            g_tex_stg_bytes -= old->bytes;
            evict[nevict++] = old;
        }
        g_tex_stg[g_tex_nstg++] = b;
        g_tex_stg_bytes += b->bytes;
        kept = 1;
    }
    pthread_mutex_unlock(&g_seg_mu);
    for (uint32_t i = 0; i < nevict; i++) {
        struct nvn_gpu_buf_s *e = evict[i];
        pthread_mutex_lock(&g_mu);
        unlink_buf_locked(&g, e);
        pthread_mutex_unlock(&g_mu);
        if (e->mapped) vkUnmapMemory(g.d, e->memory);
        if (e->buffer) vkDestroyBuffer(g.d, e->buffer, NULL);
        if (e->memory) vkFreeMemory(g.d, e->memory, NULL);
        free(e);
        __atomic_fetch_add(&g_tp_stat[TP_STG_EVICT], 1, __ATOMIC_RELAXED);
    }
    return kept;
}

static int tex_batch_overlaps(const struct nvn_gpu_cmd_s *c, uint32_t aspect, uint32_t level, uint32_t layer,
                              VkOffset3D o, VkExtent3D e) {
    const int32_t x0 = o.x, y0 = o.y, z0 = o.z;
    const int32_t x1 = o.x + (int32_t)e.width;
    const int32_t y1 = o.y + (int32_t)e.height;
    const int32_t z1 = o.z + (int32_t)e.depth;
    for (uint32_t i = 0; i < c->tb_nranges; i++) {
        if ((c->tb_ranges[i].aspect & aspect) &&
            c->tb_ranges[i].level == level &&
            c->tb_ranges[i].layer == layer &&
            x0 < c->tb_ranges[i].x1 && c->tb_ranges[i].x0 < x1 &&
            y0 < c->tb_ranges[i].y1 && c->tb_ranges[i].y0 < y1 &&
            z0 < c->tb_ranges[i].z1 && c->tb_ranges[i].z0 < z1)
            return 1;
    }
    return 0;
}

static void tex_batch_emit_copies(struct nvn_gpu_cmd_s *c) {
    if (!c->tb_n || !c->tb_dst) return;
    vkCmdCopyBufferToImage(c->cb, c->tb_src, c->tb_dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, c->tb_n, c->tb_reg);
    __atomic_fetch_add(&g_tp_stat[TP_COPY_CMD], 1, __ATOMIC_RELAXED);
    c->tb_n = 0;
    c->tb_src = VK_NULL_HANDLE;
}

static void tex_batch_waw_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    VkImageMemoryBarrier ib;
    memset(&ib, 0, sizeof ib);
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = t->image;
    ib.subresourceRange = (VkImageSubresourceRange){t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &ib);
    __atomic_fetch_add(&g_tp_stat[TP_IMG_BAR], 1, __ATOMIC_RELAXED);
}

static void tex_batch_flush(struct nvn_gpu_cmd_s *c) {
    if (!c->tb_active) return;
    struct nvn_gpu_tex_s *dst = c->tb_dst;
    tex_batch_emit_copies(c);
    // 先清 tb_active 再调 image_barrier，防止 image_barrier_from 递归调用 tex_batch_flush
    c->tb_active = 0;
    c->tb_dst = NULL;
    c->tb_nranges = 0;
    if (dst) {
        image_barrier(c, dst, VK_IMAGE_LAYOUT_GENERAL);
        __atomic_fetch_add(&g_tp_stat[TP_IMG_BAR], 1, __ATOMIC_RELAXED);
    }
}

static void tex_batch_record(struct nvn_gpu_cmd_s *c, struct nvn_gpu_buf_s *b, struct nvn_gpu_tex_s *t,
                             const VkBufferImageCopy *bc) {
    __atomic_fetch_add(&g_tp_stat[TP_UPLOADS], 1, __ATOMIC_RELAXED);
    if (c->tb_active && c->tb_dst != t) tex_batch_flush(c);
    const uint32_t aspect = (uint32_t)bc->imageSubresource.aspectMask;
    const uint32_t level = bc->imageSubresource.mipLevel;
    const uint32_t layer = bc->imageSubresource.baseArrayLayer;
    if (!c->tb_active) {
        if (lay_cur(c, t) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            tex_batch_waw_barrier(c, t);
        } else {
            image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            __atomic_fetch_add(&g_tp_stat[TP_IMG_BAR], 1, __ATOMIC_RELAXED);
        }
        c->tb_active = 1;
        c->tb_dst = t;
    } else {
        int need_waw = 0;
        if (tex_batch_overlaps(c, aspect, level, layer, bc->imageOffset, bc->imageExtent)) need_waw = 1;
        if (need_waw || c->tb_nranges >= TB_RANGE_MAX) {
            tex_batch_emit_copies(c);
            tex_batch_waw_barrier(c, t);
            if (need_waw) __atomic_fetch_add(&g_tp_stat[TP_WAW], 1, __ATOMIC_RELAXED);
            c->tb_nranges = 0;
        }
    }
    if (c->tb_n > 0 && (c->tb_src != b->buffer || c->tb_n >= TB_REG_MAX))
        tex_batch_emit_copies(c);
    if (!c->tb_n) c->tb_src = b->buffer;
    c->tb_reg[c->tb_n++] = *bc;
    c->tb_ranges[c->tb_nranges].aspect = aspect;
    c->tb_ranges[c->tb_nranges].level = level;
    c->tb_ranges[c->tb_nranges].layer = layer;
    c->tb_ranges[c->tb_nranges].x0 = bc->imageOffset.x;
    c->tb_ranges[c->tb_nranges].y0 = bc->imageOffset.y;
    c->tb_ranges[c->tb_nranges].z0 = bc->imageOffset.z;
    c->tb_ranges[c->tb_nranges].x1 = bc->imageOffset.x + (int32_t)bc->imageExtent.width;
    c->tb_ranges[c->tb_nranges].y1 = bc->imageOffset.y + (int32_t)bc->imageExtent.height;
    c->tb_ranges[c->tb_nranges].z1 = bc->imageOffset.z + (int32_t)bc->imageExtent.depth;
    c->tb_nranges++;
    cmd_ref(c, b, REF_BUF);
    cmd_ref(c, t, REF_TEX);
}

// 只读诊断出口（大块纹理暂存池 + 宿主上传合并，debug.s2a.nvk_tex_upload_pool）
void nvn_vk_dbg_tex_pool_stat(uint64_t out[8], int reset);
void nvn_vk_dbg_tex_pool_stat(uint64_t out[8], int reset) {
    for (int i = 0; i < TP_N; i++) {
        uint64_t v = reset ? __atomic_exchange_n(&g_tp_stat[i], 0, __ATOMIC_RELAXED)
                           : __atomic_load_n(&g_tp_stat[i], __ATOMIC_RELAXED);
        if (out) out[i] = v;
    }
}
