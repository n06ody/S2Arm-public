// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Diagnostic only. Replay may record a prelude after part of the main command;
// apply its image operations at physical retirement, in actual queue order.
enum { DEF_CELLS = 32768, DEF_OPS = 8192, DEF_BOXES = 4, DEF_PROBES = 32 };
enum def_kind { DEF_READ, DEF_WRITE, DEF_MAY_READ, DEF_MAY_WRITE, DEF_DISCARD, DEF_COPY, DEF_POOL };
enum def_certainty { DEF_UNDEFINED, DEF_DEFINED, DEF_UNKNOWN };

struct def_rect { uint32_t x0, y0, x1, y1; };
struct def_span {
    uint64_t image;
    struct def_rect rect;
    uint32_t aspect, mip, first, layers, width, height, format, target;
    int uncertain;
};
struct def_op {
    enum def_kind kind;
    const char *reason;
    struct def_span src, dst;
    struct nvn_render_audit_binding binding;
    uint64_t submit;
    unsigned opcode;
};
struct def_batch {
    nvn_gpu_cmd_effect effect;
    struct def_op *ops;
    uint32_t count, cap;
    uint64_t fence;
    uint64_t candidates[128];
};
struct def_cell {
    uint64_t image;
    uint32_t aspect, mip, layer;
    uint8_t count, uncertain;
    struct def_rect valid[DEF_BOXES];
};
static struct def_cell def_cells[DEF_CELLS];
static int def_record_lost;
static int def_index_lost;

static uint64_t access_id(uint64_t *slot);

static int def_contains(struct def_rect a, struct def_rect b) {
    return a.x0 <= b.x0 && a.y0 <= b.y0 && a.x1 >= b.x1 && a.y1 >= b.y1;
}
static int def_overlaps(struct def_rect a, struct def_rect b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
static int def_whole(const struct def_span *s) {
    return s->rect.x0 == 0 && s->rect.y0 == 0 && s->rect.x1 == s->width && s->rect.y1 == s->height;
}

static struct def_cell *def_find(const struct def_span *s, uint32_t aspect, uint32_t layer, int add) {
    const uint32_t hash = (uint32_t)pool_hash64(s->image ^ ((uint64_t)s->mip << 40) ^
                                               ((uint64_t)aspect << 32) ^ ((uint64_t)layer << 16), DEF_CELLS);
    for (uint32_t i = 0; i < DEF_PROBES; i++) {
        struct def_cell *cell = &def_cells[(hash + i) & (DEF_CELLS - 1)];
        if (cell->image == s->image && cell->aspect == aspect && cell->mip == s->mip && cell->layer == layer) return cell;
        if (cell->image) continue;
        if (!add) return NULL;
        *cell = (struct def_cell){.image = s->image, .aspect = aspect, .mip = s->mip, .layer = layer, .uncertain = s->uncertain};
        return cell;
    }
    if (!def_index_lost) {
        def_index_lost = 1;
        nvn_render_audit_event(NVN_RA_POTENTIAL_READ, "definition_index_capacity", NULL, NULL);
    }
    return NULL;
}

static enum def_certainty def_check(const struct def_span *s, uint32_t aspect, uint32_t layer) {
    if (__atomic_load_n(&def_record_lost, __ATOMIC_RELAXED)) return DEF_UNKNOWN;
    const struct def_cell *cell = def_find(s, aspect, layer, 0);
    if (!cell) return s->uncertain || def_index_lost ? DEF_UNKNOWN : DEF_UNDEFINED;
    int overlaps = 0;
    for (uint32_t i = 0; i < cell->count; i++) {
        if (def_contains(cell->valid[i], s->rect)) return DEF_DEFINED;
        overlaps |= def_overlaps(cell->valid[i], s->rect);
    }
    // An overlap may be covered by the union of several recorded rectangles.
    return cell->uncertain || overlaps ? DEF_UNKNOWN : DEF_UNDEFINED;
}

static void def_update(const struct def_span *s, uint32_t aspect, uint32_t layer, enum def_kind kind) {
    struct def_cell *cell = def_find(s, aspect, layer, 1);
    if (!cell) return;
    if (kind == DEF_DISCARD) {
        uint8_t kept = 0;
        for (uint8_t i = 0; i < cell->count; i++)
            if (!def_overlaps(cell->valid[i], s->rect)) cell->valid[kept++] = cell->valid[i];
        cell->count = kept;
        cell->uncertain = !def_whole(s);
        return;
    }
    if (kind == DEF_MAY_WRITE) {
        cell->uncertain = 1;
        return;
    }
    if (def_whole(s)) {
        cell->valid[0] = s->rect;
        cell->count = 1;
        cell->uncertain = 0;
        return;
    }
    for (uint8_t i = 0; i < cell->count; i++)
        if (def_contains(cell->valid[i], s->rect)) return;
    if (cell->count == DEF_BOXES) {
        cell->uncertain = 1;
        return;
    }
    cell->valid[cell->count++] = s->rect;
}

static void def_event(const struct def_op *op, uint64_t fence, uint32_t aspect, uint32_t layer, enum def_certainty state) {
    struct nvn_render_audit_binding site = op->binding;
    site.aspect = aspect;
    site.layer = layer;
    site.level = op->src.mip;
    site.format = op->src.format;
    site.target = op->src.target;
    char detail[144];
    snprintf(detail, sizeof detail, "image=%llu region=%u,%u-%u,%u coverage=%s",
             (unsigned long long)op->src.image, op->src.rect.x0, op->src.rect.y0, op->src.rect.x1, op->src.rect.y1,
             state == DEF_UNDEFINED ? "unwritten" : state == DEF_DEFINED ? "defined" : "unknown");
    nvn_render_audit_event_at(op->kind == DEF_MAY_READ || state != DEF_UNDEFINED ?
                              NVN_RA_POTENTIAL_READ : NVN_RA_UNDEFINED_READ, op->reason, &site, detail,
                              op->submit, op->opcode, fence);
}

static void def_retire(nvn_gpu_cmd_effect *effect, int completed) {
    struct def_batch *batch = (struct def_batch *)effect;
    for (uint32_t i = 0; completed && i < batch->count; i++) {
        const struct def_op *op = &batch->ops[i];
        if (op->kind == DEF_POOL) {
            nvn_render_audit_event_at(NVN_RA_POTENTIAL_READ, op->reason, &op->binding, NULL, op->submit, op->opcode, batch->fence);
            continue;
        }
        const struct def_span *s = &op->src;
        for (uint32_t a = VK_IMAGE_ASPECT_COLOR_BIT; a <= VK_IMAGE_ASPECT_STENCIL_BIT; a <<= 1) {
            if (!(s->aspect & a)) continue;
            for (uint32_t j = 0; j < s->layers; j++) {
                const uint32_t layer = s->first + j;
                if (op->kind == DEF_WRITE || op->kind == DEF_MAY_WRITE || op->kind == DEF_DISCARD) {
                    def_update(s, a, layer, op->kind);
                    continue;
                }
                const enum def_certainty state = def_check(s, a, layer);
                if (state != DEF_DEFINED) def_event(op, batch->fence, a, layer, state);
                if (op->kind != DEF_COPY) continue;
                const uint32_t dest_layer = op->dst.first + j;
                if (state == DEF_DEFINED) def_update(&op->dst, a, dest_layer, DEF_WRITE);
                else {
                    def_update(&op->dst, a, dest_layer, DEF_DISCARD);
                    if (state == DEF_UNKNOWN) def_update(&op->dst, a, dest_layer, DEF_MAY_WRITE);
                }
            }
        }
    }
    free(batch->ops);
    free(batch);
}

static void def_record(struct nvn_gpu_cmd_s *c, struct def_op op) {
    if (op.kind != DEF_POOL && (!op.src.image || !op.src.layers)) return;
    struct def_batch *batch = c->definition;
    if (!batch) {
        batch = calloc(1, sizeof *batch);
        if (!batch) goto lost;
        batch->effect.retire = def_retire;
        nvn_gpu_cmd_attach_effect(c, &batch->effect);
        c->definition = batch;
    }
    if (op.kind == DEF_POOL || op.kind == DEF_MAY_READ || op.kind == DEF_MAY_WRITE) {
        const uint64_t binding = (uint64_t)(uint32_t)op.binding.stage << 32 |
            (uint32_t)op.binding.set << 24 | (uint32_t)op.binding.binding << 8 | (uint8_t)op.binding.element;
        const uint64_t key = ((op.src.image * 0xd6e8feb86659fd93ull) ^ (binding * 0xa0761d6478bd642full) ^
                              ((uint64_t)op.src.mip << 25) ^ ((uint64_t)op.src.first << 9) ^
                              ((uint64_t)op.src.layers << 41) ^ ((uint64_t)op.kind << 57)) | 1;
        const uint32_t slot = pool_hash64(key, 128);
        if (batch->candidates[slot] == key) return;
        batch->candidates[slot] = key;
    } else {
        // An exact content update may change what a later candidate can see.
        memset(batch->candidates, 0, sizeof batch->candidates);
    }
    if (batch->count == batch->cap) {
        if (batch->cap == DEF_OPS) goto lost;
        const uint32_t cap = batch->cap ? batch->cap * 2 : 64;
        struct def_op *next = realloc(batch->ops, cap * sizeof *next);
        if (!next) goto lost;
        batch->ops = next;
        batch->cap = cap;
    }
    nvn_render_audit_get_context(&op.submit, &op.opcode);
    batch->ops[batch->count++] = op;
    return;
lost:
    if (!__atomic_exchange_n(&def_record_lost, 1, __ATOMIC_RELAXED))
        nvn_render_audit_event(NVN_RA_POTENTIAL_READ, "definition_record_capacity", NULL, NULL);
}

static uint32_t def_mip(uint32_t pixels, uint32_t level) {
    uint32_t size = pixels >> level;
    return size ? size : 1;
}
static struct def_span def_image(struct nvn_gpu_tex_s *t, VkImageSubresourceLayers sub, VkOffset3D off, VkExtent3D size) {
    if (!t || !t->image || sub.mipLevel >= t->levels || sub.mipLevel >= 32 || !size.width || !size.height)
        return (struct def_span){0};
    const int volume = t->target == NVN_TEXTURE_TARGET_3D;
    return (struct def_span){
        .image = access_id(&t->access_id), .format = t->nvn_format, .target = t->target,
        .rect = {(uint32_t)off.x, (uint32_t)off.y, (uint32_t)off.x + size.width, (uint32_t)off.y + size.height},
        .aspect = sub.aspectMask, .mip = sub.mipLevel, .first = volume ? (uint32_t)off.z : sub.baseArrayLayer,
        .layers = volume ? size.depth : sub.layerCount, .width = def_mip(t->w, sub.mipLevel), .height = def_mip(t->h, sub.mipLevel),
        .uncertain = t->bc_decoded || t->fb_snap || (t->original_usage & NVN_GPU_TEX_BC1_OPAQUE),
    };
}

static struct def_span def_full(struct nvn_gpu_tex_s *t, VkImageSubresourceLayers sub) {
    if (!t || sub.mipLevel >= 32) return (struct def_span){0};
    return def_image(t, sub, (VkOffset3D){0},
                     (VkExtent3D){def_mip(t->w, sub.mipLevel), def_mip(t->h, sub.mipLevel), def_mip(t->depth, sub.mipLevel)});
}

static void def_transfer(struct nvn_gpu_cmd_s *c, enum def_kind kind, const char *reason,
                         struct nvn_gpu_tex_s *t, const VkBufferImageCopy *region) {
    def_record(c, (struct def_op){.kind = kind, .reason = reason,
        .src = def_image(t, region->imageSubresource, region->imageOffset, region->imageExtent),
        .binding = {.stage = -1, .set = -1, .binding = -1, .element = -1}});
}

static void def_explicit_read(struct nvn_gpu_cmd_s *c, const char *reason, struct def_span src) {
    def_record(c, (struct def_op){.kind = DEF_READ, .reason = reason, .src = src,
        .binding = {.stage = -1, .set = -1, .binding = -1, .element = -1}});
}

static void def_copy(struct nvn_gpu_cmd_s *c, const char *reason, struct def_span src, struct def_span dst) {
    if (src.layers == dst.layers && src.aspect == dst.aspect) {
        def_record(c, (struct def_op){.kind = DEF_COPY, .reason = reason, .src = src, .dst = dst,
            .binding = {.stage = -1, .set = -1, .binding = -1, .element = -1}});
    } else {
        def_explicit_read(c, reason, src);
        def_record(c, (struct def_op){.kind = DEF_DISCARD, .src = dst});
        def_record(c, (struct def_op){.kind = DEF_MAY_WRITE, .src = dst});
    }
}

static void def_range_write(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageSubresourceRange range) {
    for (uint32_t i = 0; i < range.levelCount; i++) {
        VkImageSubresourceLayers sub = {range.aspectMask, range.baseMipLevel + i, range.baseArrayLayer, range.layerCount};
        def_record(c, (struct def_op){.kind = DEF_WRITE, .src = def_full(t, sub)});
    }
}

static void def_attachment(struct nvn_gpu_cmd_s *c, enum def_kind kind, struct nvn_gpu_view_s *view,
                           uint32_t aspect, uint32_t layers, VkRect2D area, int slot) {
    if (!view || !view->tex) return;
    const VkImageSubresourceLayers sub = {aspect, view->base_level, view->base_layer, layers ? layers : 1};
    const VkOffset3D off = {area.offset.x, area.offset.y, (int32_t)view->att_layer};
    const VkExtent3D size = {area.extent.width, area.extent.height, layers ? layers : 1};
    def_record(c, (struct def_op){.kind = kind, .reason = "attachment_load",
        .src = def_image(view->tex, sub, off, size),
        .binding = {.stage = -1, .set = -1, .binding = slot, .element = -1}});
}

static void def_load(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *view, VkAttachmentLoadOp op, uint32_t aspect,
                     uint32_t layers, VkRect2D area, int slot) {
    if (op == VK_ATTACHMENT_LOAD_OP_NONE_EXT) return;
    const enum def_kind kind = op == VK_ATTACHMENT_LOAD_OP_CLEAR ? DEF_WRITE :
                               op == VK_ATTACHMENT_LOAD_OP_DONT_CARE ? DEF_DISCARD : DEF_READ;
    def_attachment(c, kind, view, aspect, layers, area, slot);
}

static void def_pass_begin(struct nvn_gpu_cmd_s *c, const nvn_gpu_pass_desc *d, const VkAttachmentDescription *a,
                           VkRect2D area) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++)
        if (d->color[i].view) def_load(c, d->color[i].view, a[n++].loadOp, VK_IMAGE_ASPECT_COLOR_BIT, d->layers, area, (int)i);
    if (d->depth.view) def_load(c, d->depth.view, a[n].loadOp, VK_IMAGE_ASPECT_DEPTH_BIT, d->layers, area, 8);
    if (d->stencil.view) def_load(c, d->stencil.view, a[n].stencilLoadOp, VK_IMAGE_ASPECT_STENCIL_BIT, d->layers, area, 8);
}

static void def_pass_rest(struct nvn_gpu_cmd_s *c, int discard) {
    const nvn_gpu_pass_desc *d = &c->pass_desc;
    const VkRect2D area = {{d->area_x, d->area_y}, {d->area_w ? d->area_w : d->w, d->area_h ? d->area_h : d->h}};
    const enum def_kind kind = discard ? DEF_DISCARD : DEF_MAY_WRITE;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++)
        if (!discard || d->color[i].store == NVN_GPU_STORE_DONTCARE)
            def_attachment(c, kind, d->color[i].view, VK_IMAGE_ASPECT_COLOR_BIT, d->layers, area, (int)i);
    if (!discard || d->depth.store == NVN_GPU_STORE_DONTCARE)
        def_attachment(c, kind, d->depth.view, VK_IMAGE_ASPECT_DEPTH_BIT, d->layers, area, 8);
    if (!discard || d->stencil.store == NVN_GPU_STORE_DONTCARE)
        def_attachment(c, kind, d->stencil.view, VK_IMAGE_ASPECT_STENCIL_BIT, d->layers, area, 8);
}

// Only a direct compute entry's unconditional first block is proof that the
// operation executes. Require a single storage descriptor and literal 2D
// coordinates. Everything else remains a candidate, including indirect work.
static void def_spirv_reads(nvn_gpu_shader shader, const uint32_t *words, size_t bytes) {
    if (bytes < 20 || (bytes & 3u) || words[0] != SpvMagicNumber || !words[3] || words[3] > 65536) return;
    struct wb_module m = {.words = words, .count = bytes / 4, .bound = words[3]};
    m.ids = calloc(m.bound, sizeof *m.ids);
    if (!m.ids) return;
    if (!wb_index(&m)) { free(m.ids); return; }
    const uint32_t *end = words + m.count;
    uint32_t entry = 0;
    for (const uint32_t *p = words + 5; p < end; p += wb_len(p)) {
        if (wb_op(p) == SpvOpEntryPoint && wb_len(p) >= 4 && p[1] == SpvExecutionModelGLCompute &&
            memchr(p + 3, 0, (wb_len(p) - 3) * 4) && !strcmp((const char *)(p + 3), shader->entry)) {
            if (entry) { free(m.ids); return; }
            entry = p[2];
        }
    }
    const uint32_t *first = NULL;
    for (const uint32_t *p = words + 5; entry && p < end; p += wb_len(p)) {
        if (wb_op(p) != SpvOpFunction || wb_len(p) < 5 || p[2] != entry) continue;
        first = p + wb_len(p);
        break;
    }
    if (first && first < end && wb_op(first) == SpvOpLabel) {
        for (const uint32_t *p = first + wb_len(first); p < end; p += wb_len(p)) {
            const unsigned op = wb_op(p);
            if (op == SpvOpFunctionCall || op == SpvOpLabel || op == SpvOpFunctionEnd ||
                op == SpvOpBranch || op == SpvOpBranchConditional || op == SpvOpSwitch ||
                op == SpvOpReturn || op == SpvOpReturnValue || op == SpvOpUnreachable ||
                op == SpvOpKill || op == SpvOpTerminateInvocation) break;
            if (op != SpvOpImageRead || wb_len(p) != 5) continue;
            const uint32_t *load = wb_def(&m, p[3], SpvOpLoad, 4);
            if (!load) continue;
            const uint32_t *variable = wb_def(&m, load[3], SpvOpVariable, 4);
            if (!variable || variable[3] != SpvStorageClassUniformConstant) continue;
            const struct wb_id *descriptor = wb_id(&m, load[3]);
            if (!descriptor->set || !descriptor->binding) continue;
            const uint32_t *xy = wb_def(&m, p[4], SpvOpConstantComposite, 5);
            uint32_t x = 0, y = 0;
            if (xy) {
                if (wb_len(xy) != 5 || !wb_constant(&m, xy[3], &x) || !wb_constant(&m, xy[4], &y)) continue;
            } else if (!(xy = wb_def(&m, p[4], SpvOpConstantNull, 3))) continue;
            const uint32_t *vec = wb_def(&m, xy[1], SpvOpTypeVector, 4);
            if (!vec || vec[3] != 2 || !wb_def(&m, vec[2], SpvOpTypeInt, 4) || !wb_scalar(&m, vec[2])) continue;
            const uint32_t i = shader->def_read_count++;
            shader->def_reads[i].set = descriptor->set - 1;
            shader->def_reads[i].binding = descriptor->binding - 1;
            shader->def_reads[i].x = x;
            shader->def_reads[i].y = y;
            if (shader->def_read_count == sizeof shader->def_reads / sizeof shader->def_reads[0]) break;
        }
    }
    free(m.ids);
}

static void def_shader(struct nvn_gpu_cmd_s *c, int compute, int direct) {
    const struct nvn_gpu_pipe_s *p = c->pipe;
    for (int stage = compute ? NVN_GPU_SH_CS : 0; stage < (compute ? NVN_GPU_SH_CS + 1 : 5); stage++) {
        for (uint32_t i = 0; i < p->stage_nent[stage] && i < 64; i++) {
            const nvn_gpu_pool_table pool = c->pool_binds[stage][i];
            if (pool && pool->kind == NVN_GPU_POOL_IMAGE) {
                const nvn_gpu_layout_ent *e = &p->stage_ents[stage][i];
                def_record(c, (struct def_op){.kind = DEF_POOL, .reason = "dynamic_image_pool",
                    .binding = {.stage = stage, .set = e->set, .binding = e->binding, .element = -1}});
            }
        }
        for (uint32_t i = 0; i < c->nbinds[stage]; i++) {
            const nvn_gpu_binding *b = &c->binds[stage][i].b;
            if (b->ent >= p->stage_nent[stage]) continue;
            const nvn_gpu_layout_ent *e = &p->stage_ents[stage][b->ent];
            nvn_gpu_view v = NULL;
            if (b->cls == NVN_GPU_RES_TEX_SAMP) v = b->u.ts.view;
            else if (b->cls == NVN_GPU_RES_TEX || b->cls == NVN_GPU_RES_IMAGE) v = b->u.view;
            if (!v || !v->tex) continue;
            const int storage = b->cls == NVN_GPU_RES_IMAGE;
            for (uint32_t mip = 0; mip < v->levels; mip++) {
                struct def_op op = {.kind = DEF_MAY_READ, .reason = storage ? "storage_image_candidate" : "static_sampled_image",
                    .src = def_full(v->tex, (VkImageSubresourceLayers){v->aspect, v->base_level + mip, v->base_layer, v->layers}),
                    .binding = {.stage = stage, .set = e->set, .binding = e->binding, .element = (int)b->elem}};
                if (!storage || (e->access & NVN_GPU_ACC_R)) {
                    const nvn_gpu_shader sh = direct && storage ? p->cshader : NULL;
                    int proven = 0;
                    if (sh && e->count == 1 && v->type == VK_IMAGE_VIEW_TYPE_2D && v->levels == 1 && v->layers == 1 && v->tex->samples == 1) {
                        for (uint32_t k = 0; k < sh->def_read_count; k++) {
                            const uint32_t x = sh->def_reads[k].x, y = sh->def_reads[k].y;
                            if (sh->def_reads[k].set != e->set || sh->def_reads[k].binding != e->binding ||
                                x >= op.src.width || y >= op.src.height) continue;
                            struct def_op exact = op;
                            exact.kind = DEF_READ;
                            exact.reason = "storage_image_read";
                            exact.src.rect = (struct def_rect){x, y, x + 1, y + 1};
                            def_record(c, exact);
                            proven = 1;
                        }
                    }
                    if (!proven) def_record(c, op);
                }
                if (storage && (e->access & NVN_GPU_ACC_W)) {
                    op.kind = DEF_MAY_WRITE;
                    def_record(c, op);
                }
            }
        }
    }
}

static void def_reset(void) {
    memset(def_cells, 0, sizeof def_cells);
    __atomic_store_n(&def_record_lost, 0, __ATOMIC_RELAXED);
    def_index_lost = 0;
}
