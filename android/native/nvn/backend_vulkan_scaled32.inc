// SPDX-License-Identifier: Apache-2.0
// FP32 采样表示在 2^24 起会丢低位。传输把原整数同步录入显存影子，
// 两份数据遵循同一命令的提交、撤销与栅栏顺序。影子内按 mip、层、行紧排。
static int raw_shadow_ensure(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint64_t total);

struct scaled_rows { VkBuffer buffer; uint64_t offset, pitch, plane; };

static int scaled_region(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level,
                         uint32_t layer, VkOffset3D off, struct scaled_rows *rows) {
    uint64_t total = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t w = t->w >> l, h = t->h >> l;
        uint32_t d = t->target == NVN_TEXTURE_TARGET_3D ? t->depth >> l : t->layers;
        uint64_t pitch = (uint64_t)(w ? w : 1) * t->scaled32_bytes;
        uint64_t plane, size;
        if (__builtin_mul_overflow(pitch, (uint64_t)(h ? h : 1), &plane) ||
            __builtin_mul_overflow(plane, (uint64_t)(d ? d : 1), &size) || total > UINT64_MAX - size) {
            set_sticky(c, NVN_GPU_E_ARG, "32 位展开影子尺寸溢出"); return 0;
        }
        if (l == level) {
            uint32_t slice = t->target == NVN_TEXTURE_TARGET_3D ? (uint32_t)off.z : layer;
            rows->offset = total + slice * plane + (uint32_t)off.y * pitch + (uint32_t)off.x * t->scaled32_bytes;
            rows->pitch = pitch;
            rows->plane = plane;
        }
        total += size;
    }
    if (!raw_shadow_ensure(c, t, total)) return 0;
    rows->buffer = t->bc_sbuf;
    return 1;
}

static void scaled_copy_rows(struct nvn_gpu_cmd_s *c, struct scaled_rows src, struct scaled_rows dst,
                             VkExtent3D ext, uint32_t bpp) {
    full_barrier(c);
    const uint64_t bytes = (uint64_t)ext.width * bpp;
    for (uint32_t z = 0; z < ext.depth; z++) {
        for (uint32_t y = 0; y < ext.height; y++) {
            const VkBufferCopy copy = {src.offset + z * src.plane + y * src.pitch,
                                       dst.offset + z * dst.plane + y * dst.pitch, bytes};
            vkCmdCopyBuffer(c->cb, src.buffer, dst.buffer, 1, &copy);
        }
    }
    c->xfer_dirty = 1;
}

static void scaled_upload(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer,
                          const NVNcopyRegion *r, const uint8_t *src, uint64_t pitch) {
    VkOffset3D off; VkExtent3D ext;
    if (copy_region(t, level, r, &off, &ext) || ext.depth != 1) {
        set_sticky(c, NVN_GPU_E_ARG, "32 位展开上传需要单个有效切片"); return;
    }
    if (t->target == NVN_TEXTURE_TARGET_3D) off.z = (int32_t)layer;
    struct scaled_rows dst = {0};
    if (!scaled_region(c, t, level, layer, off, &dst)) return;
    nvn_gpu_slice raw;
    nvn_gpu_err err = {0};
    const uint64_t row = (uint64_t)ext.width * t->scaled32_bytes;
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, row * ext.height, 4, &raw, &err))) {
        set_sticky(c, err.rc.code, "32 位展开原码上传：%s", err.why); return;
    }
    for (uint32_t y = 0; y < ext.height; y++)
        memcpy((uint8_t *)raw.cpu + y * row, src + y * pitch, (size_t)row);
    scaled_copy_rows(c, (struct scaled_rows){raw.buf->buffer, raw.off, row, 0}, dst, ext, t->scaled32_bytes);
}

static void scaled_clear(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageSubresourceRange range,
                         const uint32_t value[4]) {
    const uint32_t bpp = t->scaled32_bytes;
    const uint32_t max_w = t->w >> range.baseMipLevel;
    const uint32_t width = max_w ? max_w : 1;
    const uint32_t zero[4] = {0};
    nvn_gpu_slice raw;
    nvn_gpu_err err = {0};
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, (uint64_t)width * bpp, 4, &raw, &err))) {
        set_sticky(c, err.rc.code, "32 位展开清除原码：%s", err.why); return;
    }
    for (uint32_t x = 0; x < width; x++)
        memcpy((uint8_t *)raw.cpu + (size_t)x * bpp, value ? value : zero, bpp);
    const struct scaled_rows repeat = {raw.buf->buffer, raw.off, 0, 0};
    for (uint32_t l = range.baseMipLevel; l < range.baseMipLevel + range.levelCount; l++) {
        VkExtent3D ext = {t->w >> l, t->h >> l, t->target == NVN_TEXTURE_TARGET_3D ? t->depth >> l : 1};
        if (!ext.width) ext.width = 1;
        if (!ext.height) ext.height = 1;
        if (!ext.depth) ext.depth = 1;
        for (uint32_t y = range.baseArrayLayer; y < range.baseArrayLayer + range.layerCount; y++) {
            struct scaled_rows to = {0};
            if (!scaled_region(c, t, l, y, (VkOffset3D){0,0,0}, &to)) return;
            scaled_copy_rows(c, repeat, to, ext, bpp);
        }
    }
}
