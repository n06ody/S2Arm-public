// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

static nvn_gpu_rc bc_new_storage(struct nvn_gpu_tex_s *old, int opaque, nvn_gpu_tex *tex, nvn_gpu_err *err) {
    nvn_gpu_tex_desc desc = {
        .d = old->original_desc, .usage = old->original_usage & ~NVN_GPU_TEX_BC1_OPAQUE, .swz = old->swz,
    };
    if (opaque) desc.usage |= NVN_GPU_TEX_BC1_OPAQUE;
    return nvn_gpu_tex_create(&desc, tex, err);
}

static int bc1_snapshot_eligible(const struct nvn_gpu_tex_s *t) {
    if (!t->bc_decoded || (t->nvn_format != NVN_FORMAT_RGBA_DXT1 && t->nvn_format != NVN_FORMAT_RGBA_DXT1_SRGB) ||
        t->target == NVN_TEXTURE_TARGET_3D || t->samples != 1) return 0;
    const VkPhysicalDeviceFeatures *f = &g.dev->core_features;
    return g.bc_time_on || (g.bc1_astc_on && !f->textureCompressionBC && f->textureCompressionASTC_LDR);
}

// 收齐各级各层的同一份 CPU 快照，再决定存储格式；透明块检测与后续上传使用完全相同的字节。
int nvn_gpu_tex_bc1_snapshot(nvn_gpu_cmd ch, nvn_gpu_tex tex, const void *src, uint64_t size,
                             uint64_t serial, nvn_gpu_tex *replacement) {
    struct nvn_gpu_cmd_s *c = ch;
    struct nvn_gpu_tex_s *t = tex;
    *replacement = NULL;
    if (!c || !t || !src || c->in_pass || !bc1_snapshot_eligible(t)) return 0;
    if (!nvn_tex_layout || !nvn_deswizzle) return 0;
    nvn_texlayout layout;
    nvn_tex_layout(&t->original_desc, &layout);
    if (layout.size != size || !layout.size || t->levels > 32) return 0;

    struct span { uint64_t offset, row, plane; } level[32];
    uint64_t total = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint64_t offset, row, plane, bytes;
        uint32_t layers;
        if (!bc_shadow_layout(t, 8, l, &offset, &row, &plane, &layers, &bytes) ||
            layers != t->layers || bytes > SIZE_MAX) return 0;
        level[l] = (struct span){offset, row, plane};
        total = bytes;
    }
    uint8_t *snapshot = malloc((size_t)total);
    if (!snapshot) return 0;
    int opaque = 1;
    for (uint32_t l = 0; l < t->levels; l++) {
        const struct span s = level[l];
        for (uint32_t layer = 0; layer < t->layers; layer++) {
            uint8_t *data = snapshot + s.offset + (uint64_t)layer * s.plane;
            if (nvn_deswizzle(&t->original_desc, l, layer, src, data, (size_t)s.row)) {
                free(snapshot);
                return 0;
            }
            if (opaque) opaque = bc1_astc_opaque(data, s.plane);
        }
    }
    if (g.bc_time_on) {
        char line[160];
        snprintf(line, sizeof line, "snapshot serial=%llu format=%#x alpha=%s blocks=%llu",
                 (unsigned long long)serial, t->nvn_format, opaque ? "opaque" : "punch_through",
                 (unsigned long long)(total / 8));
        nvn_plat_log(NVN_PLAT_LOG_INFO, "bc-upload", line);
    }
    opaque &= g.bc1_astc_on;
    if (!opaque && !bc1_astc_image(t)) {
        free(snapshot);
        return 0;   // 当前布局已经正确，沿用普通传输
    }
    if (!rc_is(c->sticky, NVN_GPU_OK)) {
        free(snapshot);
        return -1;
    }
    nvn_gpu_tex target = t;
    if (opaque != bc1_astc_image(t)) {
        nvn_gpu_err err = {0};
        if (!rc_is(bc_new_storage(t, opaque, &target, &err), NVN_GPU_OK)) {
            free(snapshot);
            if (!bc1_astc_image(t)) return 0;
            set_sticky(c, err.rc.code, "BC 恢复 RGBA8 失败：%s", err.why);
            return -1;
        }
        if (opaque && !bc1_astc_image(target)) {
            nvn_gpu_tex_release(target);
            free(snapshot);
            return 0;
        }
    }
    for (uint32_t l = 0; l < t->levels && rc_is(c->sticky, NVN_GPU_OK); l++) {
        const struct span s = level[l];
        for (uint32_t layer = 0; layer < t->layers && rc_is(c->sticky, NVN_GPU_OK); layer++)
            bc_upload(c, target, l, layer, NULL, snapshot + s.offset + (uint64_t)layer * s.plane, s.row, s.plane);
    }
    free(snapshot);
    if (!rc_is(c->sticky, NVN_GPU_OK)) {
        if (target != t) nvn_gpu_tex_release(target);
        return -1;
    }
    if (target != t) *replacement = target;
    return 1;
}

// 只依据原始压缩影子重建，原图此前的解码或 ASTC 误差不会进入副本。
int nvn_gpu_tex_bc1_unwrap(nvn_gpu_cmd ch, nvn_gpu_tex tex, nvn_gpu_tex *replacement) {
    struct nvn_gpu_cmd_s *c = ch;
    struct nvn_gpu_tex_s *t = tex;
    *replacement = NULL;
    if (!t || (!bc1_astc_image(t) && !t->bc7_packed)) return 0;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return -1;
    if (c->in_pass || !t->bc_sbuf) {
        set_sticky(c, NVN_GPU_E_ORDER, "BC 恢复 RGBA8 缺少已录制的压缩影子或仍在通道内");
        return -1;
    }
    nvn_gpu_tex target = NULL;
    nvn_gpu_err err = {0};
    if (!rc_is(bc_new_storage(t, 0, &target, &err), NVN_GPU_OK)) {
        set_sticky(c, err.rc.code, "BC 恢复 RGBA8 失败：%s", err.why);
        return -1;
    }
    cmd_ref(c, t, REF_TEX);
    const unsigned block = t->bc7_packed ? 16u : 8u;
    if (bc_decoder_ready(c)) for (uint32_t l = 0; l < t->levels && rc_is(c->sticky, NVN_GPU_OK); l++) {
        const uint32_t w = t->w >> l ? t->w >> l : 1;
        const uint32_t h = t->h >> l ? t->h >> l : 1;
        uint64_t offset, row, plane, total;
        uint32_t layers;
        if (!bc_shadow_layout(t, block, l, &offset, &row, &plane, &layers, &total)) {
            set_sticky(c, NVN_GPU_E_ARG, "BC 恢复 RGBA8：压缩影子布局越界");
            break;
        }
        if (!raw_shadow_ensure(c, target, total)) break;
        for (uint32_t layer = 0; layer < layers && rc_is(c->sticky, NVN_GPU_OK); layer++) {
            const uint64_t at = offset + (uint64_t)layer * plane;
            bc_record_slice(c, target, t->bc_sbuf, at, ((uint64_t)h + 3) / 4, row, at, row,
                            l, layer, (VkOffset3D){0}, (VkExtent3D){w, h, 1}, 0);
        }
    }
    if (!rc_is(c->sticky, NVN_GPU_OK)) {
        nvn_gpu_tex_release(target);
        return -1;
    }
    *replacement = target;
    return 1;
}
