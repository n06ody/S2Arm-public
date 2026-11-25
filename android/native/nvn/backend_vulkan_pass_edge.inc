// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 仅记录目标呈现前一帧的 CPU 命令。行按成功提交顺序输出，缺口显式报出。
enum { PASS_EDGE_ROWS = 192, PASS_EDGE_LOG_ROWS = 512 };
struct pass_edge_att {
    uint64_t image;
    uint32_t slot, level, layer, format;
    uint32_t samples, load, store;
};
struct pass_edge_row {
    const char *end;
    struct pass_edge_att att[10];
    uint32_t w, h, layers, natt, draws;
    uint8_t fs[32];
    VkViewport vp;
    VkRect2D sc, render_area, draw_union, clear_union;
    uint32_t builtin_draws, clear_calls;
    uint32_t mirror_ranges;
    uint64_t mirror_bytes;
};
struct pass_edge_trace {
    uint32_t n, dropped;
    int open;
    struct pass_edge_row rows[PASS_EDGE_ROWS];
};
static uint64_t access_id(uint64_t *slot);

static struct pass_edge_row *pass_edge_row(struct nvn_gpu_cmd_s *c) {
    if (!c || !c->pass_edge_frame || c->pass_edge_oom) return NULL;
    if (!c->pass_edge) {
        c->pass_edge = calloc(1, sizeof *c->pass_edge);
        if (!c->pass_edge) { c->pass_edge_oom = 1; return NULL; }
        c->pass_edge->open = -1;
    }
    struct pass_edge_trace *t = c->pass_edge;
    if (t->n == PASS_EDGE_ROWS) { t->dropped++; return NULL; }
    return &t->rows[t->n++];
}

static void pass_edge_att(struct pass_edge_row *row, unsigned slot, const struct nvn_gpu_view_s *v,
                          const VkAttachmentDescription *att) {
    row->att[row->natt++] = (struct pass_edge_att){
        access_id(&v->tex->access_id), slot, v->base_level,
        v->tex->target == NVN_TEXTURE_TARGET_3D ? v->att_layer : v->base_layer, att->format,
        att->samples, slot == 9 ? att->stencilLoadOp : att->loadOp,
        slot == 9 ? att->stencilStoreOp : att->storeOp,
    };
}

static void pass_edge_begin(struct nvn_gpu_cmd_s *c, const nvn_gpu_pass_desc *d, VkRect2D area,
                            const VkAttachmentDescription *att) {
    struct pass_edge_row *r = pass_edge_row(c);
    if (!r) return;
    c->pass_edge->open = (int)(c->pass_edge->n - 1);
    r->w = d->w;
    r->h = d->h;
    r->layers = d->layers ? d->layers : 1;
    r->render_area = area;
    unsigned index = 0;
    for (unsigned i = 0; i < d->ncolor && i < 8; i++)
        if (d->color[i].view) pass_edge_att(r, i, d->color[i].view, &att[index++]);
    // 深度和模板共用一个物理描述，但分别报告实际操作。
    if (d->depth.view) pass_edge_att(r, 8, d->depth.view, &att[index]);
    if (d->stencil.view) pass_edge_att(r, 9, d->stencil.view, &att[index]);
}

// 弱钩供共享回放打关遍原因。只写当前物理遍，标签不会流到别的遍。
void nvn_gpu_pass_edge_reason(nvn_gpu_cmd c, const char *reason) {
    if (!c || !c->pass_edge || c->pass_edge->open < 0) return;
    struct pass_edge_row *r = &c->pass_edge->rows[c->pass_edge->open];
    if (!r->end) r->end = reason;
}

static void pass_edge_end(struct nvn_gpu_cmd_s *c) {
    if (!c->pass_edge || c->pass_edge->open < 0) return;
    nvn_gpu_pass_edge_reason(c, "other");
    c->pass_edge->open = -1;
}

static void pass_edge_cover(VkRect2D *box, VkRect2D rect, uint32_t w, uint32_t h) {
    int64_t left = rect.offset.x < 0 ? 0 : rect.offset.x, top = rect.offset.y < 0 ? 0 : rect.offset.y;
    int64_t right = (int64_t)rect.offset.x + rect.extent.width, bottom = (int64_t)rect.offset.y + rect.extent.height;
    if (right > w) right = w;
    if (bottom > h) bottom = h;
    if (right <= left || bottom <= top) return;
    if (box->extent.width) {
        const int64_t old_right = (int64_t)box->offset.x + box->extent.width;
        const int64_t old_bottom = (int64_t)box->offset.y + box->extent.height;
        if (left > box->offset.x) left = box->offset.x;
        if (top > box->offset.y) top = box->offset.y;
        if (right < old_right) right = old_right;
        if (bottom < old_bottom) bottom = old_bottom;
    }
    *box = (VkRect2D){{(int32_t)left, (int32_t)top}, {(uint32_t)(right - left), (uint32_t)(bottom - top)}};
}

static void pass_edge_builtin(struct nvn_gpu_cmd_s *c, VkRect2D rect, int clear) {
    if (!c->pass_edge || c->pass_edge->open < 0) return;
    struct pass_edge_row *r = &c->pass_edge->rows[c->pass_edge->open];
    if (clear) {
        r->clear_calls++;
        pass_edge_cover(&r->clear_union, rect, r->w, r->h);
    } else {
        r->builtin_draws++;
        pass_edge_cover(&r->draw_union, rect, r->w, r->h);
    }
}

static void pass_edge_draw(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p,
                           const VkViewport *vp, const VkRect2D *sc) {
    if (!c->pass_edge || c->pass_edge->open < 0) return;
    struct pass_edge_row *r = &c->pass_edge->rows[c->pass_edge->open];
    pass_edge_cover(&r->draw_union, *sc, r->w, r->h);
    if (r->draws++) return;
    const struct nvn_gpu_shader_s *fs = p->key.st[NVN_GPU_SH_FS].sh;
    if (fs) memcpy(r->fs, fs->spirv_digest, sizeof r->fs);
    r->vp = *vp;
    r->sc = *sc;
}

void nvn_gpu_pass_edge_mirror(nvn_gpu_cmd c, unsigned ranges, uint64_t bytes) {
    if (!ranges) return;
    struct pass_edge_row *r = pass_edge_row(c);
    if (!r) return;
    r->mirror_ranges = ranges;
    r->mirror_bytes = bytes;
}

static int pass_edge_next(void) {
    return g.pass_edge_target
        && __atomic_load_n(&g.pass_edge_present, __ATOMIC_RELAXED) + 1 == g.pass_edge_target;
}

// g_submit_mu 下，日志顺序与物理提交一致。没有成功提交的段不发布。
static void pass_edge_commit(const struct nvn_gpu_cmd_s *c, uint64_t signal) {
    if (!pass_edge_next()) return;
    g.pass_edge_submits++;
    if (!c->pass_edge_frame || c->pass_edge_oom) {
        g.pass_edge_missing++;
        return;
    }
    const struct pass_edge_trace *t = c->pass_edge;
    if (!t) return;
    const char *segment = c->pass_edge_pre ? "pre" : "main";
    g.pass_edge_dropped += t->dropped;
    for (uint32_t i = 0; i < t->n; i++) {
        if (g.pass_edge_rows == PASS_EDGE_LOG_ROWS) { g.pass_edge_dropped += t->n - i; break; }
        unsigned seq = ++g.pass_edge_rows;
        const struct pass_edge_row *r = &t->rows[i];
        char line[1536];
        if (r->mirror_ranges) {
            snprintf(line, sizeof line, "nvk-pass-edge mirror frame=%u seq=%u signal=%llu segment=%s ranges=%u bytes=%llu",
                g.pass_edge_target, seq, (unsigned long long)signal, segment,
                r->mirror_ranges, (unsigned long long)r->mirror_bytes);
        } else {
            char fs[65];
            s2a_sha256_hex(r->fs, fs);
            int n = snprintf(line, sizeof line,
                "nvk-pass-edge pass frame=%u seq=%u signal=%llu segment=%s extent=%ux%ux%u draws=%u first_fs=%s vp=%.2f,%.2f,%.2f,%.2f sc=%d,%d,%u,%u end=%s attachments=",
                g.pass_edge_target, seq, (unsigned long long)signal, segment, r->w, r->h, r->layers, r->draws,
                fs, r->vp.x, r->vp.y, r->vp.width, r->vp.height, r->sc.offset.x, r->sc.offset.y,
                r->sc.extent.width, r->sc.extent.height, r->end ? r->end : "unclosed");
            for (unsigned j = 0; j < r->natt && n > 0 && (size_t)n < sizeof line; j++) {
                const struct pass_edge_att *a = &r->att[j];
                n += snprintf(line + n, sizeof line - (size_t)n, "%s%u:%llu/%u/%u/%u", j ? "," : "",
                    a->slot, (unsigned long long)a->image, a->format, a->level, a->layer);
            }
            if (n > 0 && (size_t)n < sizeof line) {
                const VkRect2D area = r->render_area, draw = r->draw_union, clear = r->clear_union;
                n += snprintf(line + n, sizeof line - (size_t)n,
                    " ra=%d,%d,%u,%u sc_union=%d,%d,%u,%u clear_union=%d,%d,%u,%u builtin_draws=%u clear_calls=%u ops=",
                    area.offset.x, area.offset.y, area.extent.width, area.extent.height,
                    draw.offset.x, draw.offset.y, draw.extent.width, draw.extent.height,
                    clear.offset.x, clear.offset.y, clear.extent.width, clear.extent.height, r->builtin_draws, r->clear_calls);
            }
            for (unsigned j = 0; j < r->natt && n > 0 && (size_t)n < sizeof line; j++) {
                const struct pass_edge_att *a = &r->att[j];
                n += snprintf(line + n, sizeof line - (size_t)n, "%s%u:%u/%u/%u", j ? "," : "",
                    a->slot, a->samples, a->load, a->store);
            }
            if (n <= 0 || (size_t)n >= sizeof line) {
                g.pass_edge_dropped++;
                continue;
            }
        }
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
}

static void pass_edge_presented(void) {
    if (!g.pass_edge_target) return;
    const unsigned frame = __atomic_load_n(&g.pass_edge_present, __ATOMIC_RELAXED);
    if (frame >= g.pass_edge_target) return;
    __atomic_store_n(&g.pass_edge_present, frame + 1, __ATOMIC_RELAXED);
    if (frame + 1 != g.pass_edge_target) return;
    char line[180];
    snprintf(line, sizeof line, "nvk-pass-edge frame=%u submits=%u rows=%u missing=%u dropped=%u",
        frame + 1, g.pass_edge_submits, g.pass_edge_rows, g.pass_edge_missing, g.pass_edge_dropped);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
}
