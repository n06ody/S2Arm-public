// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 同一次绘制读写附件：GENERAL 不是像素级反馈保证。照 iOS fb_prepare，
// 在 draw 前把采样范围存成独立图像；同一逻辑通道下一笔重新取快照。
// 此处在 nvn_gpu_* 契约之下，使用真实 VkImage / 子资源范围与段完成寿命。
static int feedback_overlap(struct nvn_gpu_view_s *v, struct nvn_gpu_view_s *a) {
    if (!v || !a || !v->tex || !a->tex || v->tex->image != a->tex->image ||
        !(v->aspect & a->tex->aspect) || a->base_level < v->base_level ||
        (uint64_t)a->base_level >= (uint64_t)v->base_level + v->levels) return 0;
    // view_attachment 挂的是一级一层的 2D 形态；3D 采样形态覆盖该级全部 z。
    if (v->type == VK_IMAGE_VIEW_TYPE_3D) return 1;
    uint32_t layer = a->tex->target == NVN_TEXTURE_TARGET_3D && a->tex->depth > 1 ? a->att_layer : a->base_layer;
    return layer >= v->base_layer && (uint64_t)layer < (uint64_t)v->base_layer + v->layers;
}
static int feedback_attached(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *v) {
    const nvn_gpu_pass_desc *d = &c->pass_desc;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) if (feedback_overlap(v, d->color[i].view)) return 1;
    return feedback_overlap(v, d->depth.view) || feedback_overlap(v, d->stencil.view);
}
static int feedback_add(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *v, uint32_t set) {
    if (!feedback_attached(c, v)) return 1;
    c->desc_dirty |= 1u << set;
    for (struct feedback_copy *f = c->feedback; f != c->feedback_stop; f = f->next)
        if (f->source == v) return 1;
    struct feedback_copy *f = calloc(1, sizeof *f);
    if (!f) { set_sticky(c, NVN_GPU_E_OOM, "feedback snapshot list allocation failed"); return 0; }
    f->source = v; f->next = c->feedback; c->feedback = f;
    return 1;
}
static int feedback_make(struct nvn_gpu_cmd_s *c, struct feedback_copy *f) {
    struct nvn_gpu_view_s *v = f->source;
    struct nvn_gpu_tex_s *s = v->tex;
    nvn_gpu_tex_desc td = {0};
    td.d.target = s->target; td.d.format = s->nvn_format;
    td.d.width = s->w; td.d.height = s->h; td.d.depth = s->depth;
    td.d.levels = s->levels; td.d.samples = s->samples;
    td.usage = NVN_GPU_TEX_SAMPLED | (s->view_fmt ? NVN_GPU_TEX_VIEW_FMT : 0);
    nvn_gpu_tex t = NULL; nvn_gpu_err err = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &err);
    if (!nvn_gpu_ok(r)) { set_sticky(c, r.code, "feedback texture: %s", err.why); return 0; }
    cmd_ref(c, t, REF_TEX);
    if (!rc_is(c->sticky, NVN_GPU_OK)) { nvn_gpu_tex_release(t); return 0; }
    // 深度 / 多采样保持物理格式和样本数，绝不 resolve / 近似格式替代。
    if (t->format != s->format || t->samples != s->samples) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "feedback snapshot format/sample mismatch"); nvn_gpu_tex_release(t); return 0;
    }
    VkImageViewCreateInfo vi = {0}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image; vi.viewType = v->type; vi.format = v->format; vi.components = v->components;
    vi.subresourceRange = (VkImageSubresourceRange){v->aspect, v->base_level, v->levels, v->base_layer, v->layers};
    VkResult vr = vkCreateImageView(g.d, &vi, NULL, &f->view);
    if (vr != VK_SUCCESS) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "feedback vkCreateImageView failed %d", vr); nvn_gpu_tex_release(t); return 0;
    }
    image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!rc_is(c->sticky, NVN_GPU_OK)) { nvn_gpu_tex_release(t); return 0; }
    for (uint32_t lv = v->base_level; lv < v->base_level + v->levels; lv++) {
        // 3D 图像的 arrayLayer 必须为 0；范围位于 z，保留整级的全部切片。
        const int image3d = s->target == NVN_TEXTURE_TARGET_3D;
        uint32_t w = s->w >> lv, h = s->h >> lv, z = image3d ? s->depth >> lv : 1;
        for (VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) {
            if (!(v->aspect & aspect)) continue; // Vulkan 拷贝的每条 region 只能指定一个面
            VkImageCopy copy = {0};
            copy.srcSubresource = (VkImageSubresourceLayers){aspect, lv, image3d ? 0 : v->base_layer, image3d ? 1 : v->layers};
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = (VkExtent3D){w ? w : 1, h ? h : 1, z ? z : 1};
            vkCmdCopyImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
    }
    image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    nvn_gpu_tex_release(t); // cmd 引用保活到 fence；raw view 在 finish_cmd 中先释放
    return rc_is(c->sticky, NVN_GPU_OK);
}
static int feedback_prepare(struct nvn_gpu_cmd_s *c, int compute) {
    // 退出反馈或进入计算时，恢复原视图描述符。原绑定表永不被快照改写。
    if (c->feedback != c->feedback_stop) c->desc_dirty = 0xfu;
    c->feedback_stop = c->feedback;
    if (compute || !c->feedback_on || !c->in_pass || !c->pipe->nset) return 1;
    for (int st = 0; st < 5; st++) {
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if (b->ent >= c->pipe->stage_nent[st]) continue;
            const nvn_gpu_layout_ent *e = &c->pipe->stage_ents[st][b->ent];
            if (e->cls != b->cls) continue;
            struct nvn_gpu_view_s *v = b->cls == NVN_GPU_RES_TEX_SAMP ? b->u.ts.view :
                (b->cls == NVN_GPU_RES_TEX || b->cls == NVN_GPU_RES_IMAGE) ? b->u.view : NULL;
            if (b->cls == NVN_GPU_RES_IMAGE && feedback_attached(c, v)) {
                set_sticky(c, NVN_GPU_E_UNSUPPORTED, "feedback storage image writes cannot use a sampled snapshot"); return 0;
            }
            if (b->cls != NVN_GPU_RES_IMAGE && !feedback_add(c, v, e->set)) return 0;
        }
        if (c->pool_active) for (uint32_t i = 0; i < c->pipe->stage_nent[st] && i < 64; i++) {
            nvn_gpu_pool_table t = c->pool_binds[st][i];
            if (!t || t->kind != NVN_GPU_POOL_IMAGE) continue;
            for (uint32_t j = 0; j <= t->count; j++)
                if (!feedback_add(c, t->objects[j], c->pipe->stage_ents[st][i].set)) return 0;
        }
    }
    if (c->feedback == c->feedback_stop) return 1;
    // 同一 occlusion query 不能跨 VkRenderPass；不截断查询后假报成功。
    if (c->q_active >= 0) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "feedback snapshot inside active occlusion query"); return 0; }
    nvn_gpu_pass_desc d = c->pass_desc;
    for (uint32_t i = 0; i < d.ncolor && i < 8; i++) if (d.color[i].view) d.color[i].load = NVN_GPU_LOAD_LOAD;
    if (d.depth.view) d.depth.load = NVN_GPU_LOAD_LOAD;
    if (d.stencil.view) d.stencil.load = NVN_GPU_LOAD_LOAD;
    nvn_gpu_pass_end(c); // CLEAR 已在旧通道落地；重开必须 LOAD
    for (struct feedback_copy *f = c->feedback; f != c->feedback_stop; f = f->next)
        if (!feedback_make(c, f)) return 0;
    nvn_gpu_pass_begin(c, &d);
    return rc_is(c->sticky, NVN_GPU_OK);
}
