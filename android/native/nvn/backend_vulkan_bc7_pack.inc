// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// 上传前核对完整原块。预转块只进物理 ASTC 图，客体回读始终从 BC7 影子取。
int nvn_gpu_tex_bc7_snapshot(nvn_gpu_cmd ch, nvn_gpu_tex tex, const void *src, uint64_t size,
                            nvn_gpu_bc7_source source, nvn_gpu_tex *replacement) {
    struct nvn_gpu_cmd_s *c = ch;
    struct nvn_gpu_tex_s *t = tex;
    *replacement = NULL;
    if (!c || !t || (!g.bc7_pack && !g.bc7_diag_on) ||
        (t->nvn_format != NVN_FORMAT_BPTC_UNORM && t->nvn_format != NVN_FORMAT_BPTC_UNORM_SRGB)) return 0;
    if (source == NVN_GPU_BC7_SPARSE || (t->original_desc.flags & NVN_TEXTURE_FLAGS_SPARSE)) {
        bc7_diag_note(t, NULL, 0, BC7_DIAG_SPARSE);
        return 0;
    }
    if (source == NVN_GPU_BC7_PARTIAL || !src) {
        bc7_diag_note(t, NULL, 0, BC7_DIAG_PARTIAL);
        return 0;
    }
    const int plain = source != NVN_GPU_BC7_FULL || c->in_pass || !(t->original_usage & NVN_GPU_TEX_SAMPLED) ||
                      (t->original_usage & (NVN_GPU_TEX_RENDER | NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_ATOMIC));
    if (!g.bc7_diag_on && (plain || !t->bc_decoded)) return 0;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return -1;
    if (t->target == NVN_TEXTURE_TARGET_3D || t->samples != 1 || t->levels > 32 ||
        !nvn_tex_layout || !nvn_deswizzle) goto bad_layout;
    nvn_texlayout layout;
    nvn_tex_layout(&t->original_desc, &layout);
    if (!size || size != layout.size) goto bad_layout;

    struct plane { uint64_t offset, row, bytes; } planes[32];
    uint64_t total = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t layers;
        struct plane *p = &planes[l];
        if (!bc_shadow_layout(t, 16, l, &p->offset, &p->row, &p->bytes, &layers, &total) ||
            layers != t->layers || total > BC7_PACK_MAX_IMAGE) goto bad_layout;
    }
    if (!total) goto bad_layout;
    uint8_t *snapshot = malloc((size_t)total);
    if (!snapshot) {
        bc7_diag_note(t, NULL, 0, BC7_DIAG_RESOURCE);
        return 0;
    }
    for (uint32_t l = 0; l < t->levels; l++) for (uint32_t y = 0; y < t->layers; y++) {
        const struct plane p = planes[l];
        if (nvn_deswizzle(&t->original_desc, l, y, src, snapshot + p.offset + y * p.bytes, (size_t)p.row)) {
            free(snapshot);
            goto bad_layout;
        }
    }
    uint8_t key[32];
    bc7_pack_key(t->w, t->h, t->levels, t->layers, snapshot, (size_t)total, key);
    enum bc7_diag_reason reason;
    if (!g.bc7_pack_capable || !t->bc_decoded) reason = BC7_DIAG_LOW_CAP;
    else if (!g.bc7_pack) reason = BC7_DIAG_NO_PACK;
    else if (!bc7_pack_find(g.bc7_pack, key, (size_t)total))
        reason = bc7_pack_rejected(g.bc7_pack, key) ? BC7_DIAG_QUALITY : BC7_DIAG_KEY;
    else if (!g.bc7_pack_on) reason = BC7_DIAG_DISABLED;
    else if (plain) reason = BC7_DIAG_PLAIN;
    else reason = BC7_DIAG_HIT;
    if (reason != BC7_DIAG_HIT) goto miss;

    nvn_gpu_err err = {0};
    nvn_gpu_slice input;
    if (!rc_is(nvn_gpu_arena_alloc(c, total * 2, 16, &input, &err), NVN_GPU_OK)) {
        reason = BC7_DIAG_RESOURCE;
        goto miss;
    }
    uint8_t *staging = input.cpu;
    if (!bc7_pack_lookup(g.bc7_pack, key, staging + total, (size_t)total)) {
        reason = BC7_DIAG_PAYLOAD;
        goto miss;
    }
    memcpy(staging, snapshot, (size_t)total);
    free(snapshot);
    snapshot = NULL;

    const nvn_gpu_tex_desc desc = {.d = t->original_desc, .usage = t->original_usage, .swz = t->swz};
    struct nvn_gpu_tex_s *next = NULL;
    if (!rc_is(tex_create_impl(&desc, NULL, &next, &err, 1), NVN_GPU_OK)) {
        reason = err.rc.code == NVN_GPU_E_UNSUPPORTED ? BC7_DIAG_LOW_CAP : BC7_DIAG_RESOURCE;
        goto miss;
    }
    if (!raw_shadow_ensure(c, next, total)) {
        nvn_gpu_tex_release(next);
        bc7_diag_note(t, key, total, BC7_DIAG_RESOURCE);
        return -1;
    }
    full_barrier(c);
    const VkBufferCopy shadow = {input.off, 0, total};
    vkCmdCopyBuffer(c->cb, input.buf->buffer, next->bc_sbuf, 1, &shadow);
    image_barrier(c, next, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (uint32_t l = 0; l < t->levels; l++) {
        const uint32_t w = t->w >> l, h = t->h >> l;
        VkBufferImageCopy copy = {
            .bufferOffset = input.off + total + planes[l].offset,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, t->layers},
            .imageExtent = {w ? w : 1, h ? h : 1, 1},
        };
        vkCmdCopyBufferToImage(c->cb, input.buf->buffer, next->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    image_barrier(c, next, VK_IMAGE_LAYOUT_GENERAL);
    c->xfer_dirty = 1;
    cmd_ref(c, input.buf, REF_BUF);
    cmd_ref(c, next, REF_TEX);
    if (!rc_is(c->sticky, NVN_GPU_OK)) {
        nvn_gpu_tex_release(next);
        bc7_diag_note(t, key, total, BC7_DIAG_RESOURCE);
        return -1;
    }
    __atomic_fetch_add(&g.bc7_pack_hits, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g.bc7_pack_bytes, total, __ATOMIC_RELAXED);
    bc7_diag_note(t, key, total, BC7_DIAG_HIT);
    *replacement = next;
    return 1;
miss:
    free(snapshot);
    bc7_diag_note(t, key, total, reason);
    __atomic_fetch_add(&g.bc7_pack_misses, 1, __ATOMIC_RELAXED);
    return 0;
bad_layout:
    bc7_diag_note(t, NULL, 0, BC7_DIAG_LAYOUT);
    return 0;
}
