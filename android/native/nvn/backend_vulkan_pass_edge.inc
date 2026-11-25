// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 仅记录目标呈现前一帧的 CPU 命令。行按成功提交顺序输出，缺口显式报出。
enum { PASS_EDGE_ROWS = 192, PASS_EDGE_LOG_ROWS = 512 };
struct pass_edge_att {
    uint64_t image;
    uint32_t slot, level, layer, format;
};
struct pass_edge_row {
    const char *end;
    struct pass_edge_att att[10];
    uint32_t w, h, layers, natt, draws;
    uint8_t fs[32];
    VkViewport vp;
    VkRect2D sc;
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

static void pass_edge_att(struct pass_edge_row *row, unsigned slot, const struct nvn_gpu_view_s *v) {
    if (!v || !v->tex) return;
    row->att[row->natt++] = (struct pass_edge_att){
        access_id(&v->tex->access_id), slot, v->base_level,
        v->tex->target == NVN_TEXTURE_TARGET_3D ? v->att_layer : v->base_layer, v->format,
    };
}

static void pass_edge_begin(struct nvn_gpu_cmd_s *c, const nvn_gpu_pass_desc *d) {
    struct pass_edge_row *r = pass_edge_row(c);
    if (!r) return;
    c->pass_edge->open = (int)(c->pass_edge->n - 1);
    r->w = d->w;
    r->h = d->h;
    r->layers = d->layers ? d->layers : 1;
    for (unsigned i = 0; i < d->ncolor && i < 8; i++) pass_edge_att(r, i, d->color[i].view);
    pass_edge_att(r, 8, d->depth.view);
    pass_edge_att(r, 9, d->stencil.view);
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

static void pass_edge_draw(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p,
                           const VkViewport *vp, const VkRect2D *sc) {
    if (!c->pass_edge || c->pass_edge->open < 0) return;
    struct pass_edge_row *r = &c->pass_edge->rows[c->pass_edge->open];
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
        char line[960];
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
