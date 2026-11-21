// r3 diagnostic. Capture only recorded game draws. The retirement thread
// consumes each segment in GPU order and writes after the 24-frame window.
enum { DRAW_EXPORT_LIMIT = 64000, DRAW_EXPORT_TEXTURE_BYTES = 4096 };
#define DRAW_EXPORT_MODULE_BYTES (128u * 1024u * 1024u)

static void pass_draw_module_note(struct nvn_gpu_shader_s *sh, const void *code, size_t size) {
    if (!sh->capture_sha[0]) {
        s2a_sha256 hash;
        uint8_t digest[32];
        s2a_sha256_init(&hash);
        s2a_sha256_update(&hash, code, size);
        s2a_sha256_final(&hash, digest);
        s2a_sha256_hex(digest, sh->capture_sha);
    }
    if (sh->stage != NVN_GPU_SH_VS && sh->stage != NVN_GPU_SH_FS) return;

    pthread_mutex_lock(&g_mu);
    struct pass_draw_module *mod = g.pass_draw_modules;
    while (mod && strcmp(mod->sha, sh->capture_sha)) mod = mod->next;
    if (!mod && size <= DRAW_EXPORT_MODULE_BYTES - g.pass_draw_module_bytes) {
        mod = calloc(1, sizeof *mod + size);
        if (mod) {
            mod->size = size;
            memcpy(mod->sha, sh->capture_sha, sizeof mod->sha);
            memcpy(mod->code, code, size);
            mod->next = g.pass_draw_modules;
            g.pass_draw_modules = mod;
            g.pass_draw_module_bytes += size;
        }
    }
    sh->draw_module = mod;
    pthread_mutex_unlock(&g_mu);
}

// Use actual module digests and the effective pipeline description. Handles
// and the backing address of a compile request are deliberately absent.
static void pass_draw_pipe_note(struct nvn_gpu_pipe_s *p, const nvn_gpu_pipe_desc *d) {
    s2a_sha256 hash;
    uint8_t digest[32];
    s2a_sha256_init(&hash);
    const uint32_t state[] = {d->prim, d->dfmt, d->sfmt, p->actual_samples, d->patch_size,
                              (uint32_t)eff_restart(d), d->rast_discard, d->clip_neg1,
                              d->origin_ll, d->depth_clamp, d->nvnbar, d->poly.cull,
                              d->poly.front, d->poly.mode, d->poly.offset_en, p->dyn_all};
    s2a_sha256_update(&hash, state, sizeof state);
    s2a_sha256_update(&hash, d->cfmt, sizeof d->cfmt);
    s2a_sha256_update(&hash, d->lmap, sizeof d->lmap);
    s2a_sha256_update(&hash, d->blend, sizeof d->blend);
    s2a_sha256_update(&hash, &d->color, sizeof d->color);
    s2a_sha256_update(&hash, &d->chmask, sizeof d->chmask);
    s2a_sha256_update(&hash, &d->ms, sizeof d->ms);
    s2a_sha256_update(&hash, &d->ds, sizeof d->ds);
    s2a_sha256_update(&hash, d->attr, sizeof d->attr);
    s2a_sha256_update(&hash, d->stream, sizeof d->stream);
    const char absent[65] = {0};
    for (unsigned st = 0; st < 5; st++) {
        const nvn_gpu_stage_desc *stage = &d->st[st];
        const struct nvn_gpu_shader_s *sh = stage->sh;
        s2a_sha256_update(&hash, sh ? sh->capture_sha : absent, sizeof absent);
        if (!sh) continue;
        const uint32_t layout[] = {sh->nent, sh->push_off, sh->push_bytes, stage->nspec};
        s2a_sha256_update(&hash, layout, sizeof layout);
        s2a_sha256_update(&hash, sh->entry, sizeof sh->entry);
        for (uint32_t i = 0; i < sh->nent; i++) {
            const nvn_gpu_layout_ent *e = &sh->ents[i];
            const uint32_t desc[] = {e->set, e->binding, e->cls, e->count, e->stages, e->access};
            s2a_sha256_update(&hash, desc, sizeof desc);
        }
        if (stage->nspec) s2a_sha256_update(&hash, stage->spec, stage->nspec * sizeof *stage->spec);
    }
    s2a_sha256_final(&hash, digest);
    s2a_sha256_hex(digest, p->draw_sha);
}

static int pass_draw_grow(struct pass_draw_record **rows, uint32_t *capacity, uint32_t count) {
    if (count < *capacity) return 1;
    if (count >= DRAW_EXPORT_LIMIT) return 0;
    uint32_t cap = *capacity ? *capacity * 2 : 32;
    if (cap > DRAW_EXPORT_LIMIT) cap = DRAW_EXPORT_LIMIT;
    struct pass_draw_record *next = realloc(*rows, cap * sizeof *next);
    if (!next) return 0;
    *rows = next;
    *capacity = cap;
    return 1;
}

static void pass_draw_free(struct pass_draw_record *rows, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) free(rows[i].textures);
    free(rows);
}

static void pass_draw_close(struct nvn_gpu_vk *vk) {
    struct pass_capture *p = vk->pass_capture;
    if (p) pass_draw_free(p->draw, p->draw_count);
    while (vk->pass_draw_modules) {
        struct pass_draw_module *m = vk->pass_draw_modules;
        vk->pass_draw_modules = m->next;
        free(m);
    }
}

// The views here are the draw's resolved sampled descriptors. A runtime-indexed
// pool provides a bound table, with the sampled slot unresolved by this probe.
static int pass_draw_textures(const struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p,
                              char text[DRAW_EXPORT_TEXTURE_BYTES], uint32_t *fs_n, uint32_t *fs_fmt) {
    size_t at = 0;
    text[0] = 0;
    for (unsigned st = 0; st < 5; st++) {
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if (b->ent >= p->stage_nent[st]) continue;
            const nvn_gpu_layout_ent *e = &p->stage_ents[st][b->ent];
            const struct nvn_gpu_view_s *v = e->cls == NVN_GPU_RES_TEX_SAMP ? b->u.ts.view :
                e->cls == NVN_GPU_RES_TEX || e->cls == NVN_GPU_RES_TBUF ? b->u.view : NULL;
            if (!v) continue;
            const uint32_t fmt = v->tex ? v->format : v->buffer_format;
            const uint32_t nvnfmt = v->tex ? v->tex->nvn_format : v->nvn_format;
            const char *bc = v->tex && v->tex->bc_decoded ? "decoded" :
                v->tex && nvnfmt >= 0x41 && nvnfmt <= 0x50 ? "native" : "plain";
            uint64_t id = v->tex ? access_id(&v->tex->access_id) : 0;
            uint64_t last = v->tex ? atomic_load_explicit(&v->tex->capture_rt, memory_order_relaxed) : 0;
            if (st == NVN_GPU_SH_FS) { (*fs_n)++; *fs_fmt = v->tex ? fmt : 0; }
            int n = snprintf(text + at, DRAW_EXPORT_TEXTURE_BYTES - at,
                "%s%u/%u/%u/%u:%u:%u:%s:%llu:%llu", at ? "," : "", st, e->set, e->binding,
                b->elem, fmt, nvnfmt, bc, (unsigned long long)id, (unsigned long long)last);
            if (n < 0 || (size_t)n >= DRAW_EXPORT_TEXTURE_BYTES - at) return 0;
            at += (size_t)n;
        }
        for (uint32_t i = 0; i < p->stage_nent[st] && i < 64; i++) {
            const struct nvn_gpu_pool_table_s *table = c->pool_binds[st][i];
            const nvn_gpu_layout_ent *e = &p->stage_ents[st][i];
            if (!table || e->cls != NVN_GPU_RES_TEX) continue;
            if (st == NVN_GPU_SH_FS) { (*fs_n)++; *fs_fmt = 0; }
            int n = snprintf(text + at, DRAW_EXPORT_TEXTURE_BYTES - at, "%s%u/%u/%u/*:pool:%u",
                at ? "," : "", st, e->set, e->binding, table->count);
            if (n < 0 || (size_t)n >= DRAW_EXPORT_TEXTURE_BYTES - at) return 0;
            at += (size_t)n;
        }
    }
    return 1;
}

static void pass_draw_note(struct nvn_gpu_cmd_s *c, struct pass_capture_row *pass,
                           const struct nvn_gpu_pipe_s *p, VkPipeline selected,
                           const uint32_t before[ES_KINDS]) {
    const int geometry = pass->n == 5 && pass->att[4].slot == 8;
    const uint32_t target = pass->att[0].format;
    const int copy = pass->n == 1 && !pass->draws &&
        (target == VK_FORMAT_D32_SFLOAT || target == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    if (!geometry && !copy) return;
    char textures[DRAW_EXPORT_TEXTURE_BYTES];
    uint32_t fs_n = 0, fs_fmt = 0;
    if (!pass_draw_textures(c, p, textures, &fs_n, &fs_fmt)) { c->draw_failed = 1; return; }
    if (copy && (fs_n != 1 || fs_fmt != target)) return;
    const struct nvn_gpu_shader_s *vs = p->key.st[NVN_GPU_SH_VS].sh;
    const struct nvn_gpu_shader_s *fs = p->key.st[NVN_GPU_SH_FS].sh;
    if (selected != p->pipe || !vs || !vs->draw_module || !fs || !fs->draw_module || !p->draw_sha[0] ||
        !pass_draw_grow(&c->draw, &c->draw_cap, c->draw_count)) {
        c->draw_failed = 1;
        return;
    }
    struct pass_draw_record row = {
        .vs = vs->draw_module, .fs = fs->draw_module, .ordinal = pass->draws,
        .descriptors = c->draw_descriptors, .run = 1, .format = copy ? target : 0,
    };
    row.textures = strdup(textures);
    if (!row.textures) { c->draw_failed = 1; return; }
    memcpy(row.pso, p->draw_sha, sizeof row.pso);
    memcpy(row.vs_entry, vs->entry, sizeof row.vs_entry);
    memcpy(row.fs_entry, fs->entry, sizeof row.fs_entry);
    for (unsigned i = 0; i < ES_KINDS; i++)
        if (before[i] != c->es_count[i]) row.changes |= 1u << i;

    // Local to a physical pass. Hash the encoder's current pipeline, dynamic
    // state, vertex/index/push state and actually bound descriptor groups.
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    s2a_sha256_update(&hash, row.pso, sizeof row.pso);
    s2a_sha256_update(&hash, &c->es, sizeof c->es);
    if (p->nset) {
        s2a_sha256_update(&hash, c->desc_sets, p->nset * sizeof *c->desc_sets);
        for (uint32_t i = 0; i < p->nset; i++)
            s2a_sha256_update(&hash, c->dyn_off[i], p->ndyn[i] * sizeof *c->dyn_off[i]);
    }
    s2a_sha256_final(&hash, row.state);
    if (pass->draw_count) {
        const struct pass_draw_record *prev = &c->draw[c->draw_count - 1];
        if (prev->ordinal + 1 == row.ordinal && !memcmp(prev->state, row.state, sizeof row.state))
            row.run = prev->run + 1;
    } else {
        pass->draw_first = c->draw_count;
    }
    c->draw[c->draw_count++] = row;
    pass->draw_count++;
}

static void pass_draw_retire(struct pass_capture *p, const struct nvn_gpu_cmd_s *c,
                             const struct pass_capture_row *pass) {
    uint32_t frame = p->frames;
    const uint32_t base = p->frame[frame].ordinal;
    p->frame[frame].ordinal += pass->draws;
    if (pass->n == 5 && pass->att[4].slot == 8 && pass->draw_count != pass->draws)
        p->draw_failed = 1;
    for (uint32_t i = 0; i < pass->draw_count; i++) {
        struct pass_draw_record *row = &c->draw[pass->draw_first + i];
        if (row->format && pass->draws != 1) continue;
        if (!pass_draw_grow(&p->draw, &p->draw_cap, p->draw_count)) { p->draw_failed = 1; return; }
        struct pass_draw_record *dst = &p->draw[p->draw_count++];
        *dst = *row;
        row->textures = NULL;
        dst->seq = p->frame[frame].seq;
        dst->pass = p->frame[frame].count;
        dst->ordinal += base;
    }
}

static int pass_draw_write_module(const char *directory, struct pass_draw_module *module,
                                   uint32_t *count) {
    if (module->written) return 1;
    char path[4096];
    const int n = snprintf(path, sizeof path, "%s/%s.spv", directory, module->sha);
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    const int bytes_ok = fwrite(module->code, 1, module->size, file) == module->size;
    if (fclose(file) || !bytes_ok) return 0;
    module->written = 1;
    (*count)++;
    return 1;
}

static void pass_draw_write(const struct pass_capture *p) {
    uint32_t modules = 0;
    int failed = p->draw_failed || !p->draw_count;
    for (uint32_t i = 0; i < p->frames; i++) failed |= p->frame[i].failed != 0;
    char directory[4096] = {0}, path[4096], line[192];
    const char *root = getenv("S2A_FRAME_LOG_DIR");
    int n = root && root[0] == '/' ?
        snprintf(directory, sizeof directory, "%s/nvk-draw-export-%ld-XXXXXX", root, (long)getpid()) : -1;
    if (n < 0 || (size_t)n >= sizeof directory || !mkdtemp(directory)) {
        failed = 1;
    } else {
        const char *leaf = strrchr(directory, '/') + 1;
        snprintf(line, sizeof line, "directory=%.64s", leaf);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-draw-export", line);
        n = snprintf(path, sizeof path, "%s/draws.tsv", directory);
        FILE *file = n >= 0 && (size_t)n < sizeof path ? fopen(path, "w") : NULL;
        if (!file) {
            failed = 1;
        } else {
            fputs("seq\tpass\tdraw\tcopy_vkfmt\tvs\tvs_entry\tfs\tfs_entry\tpso\tstate\tchanged\tdescriptor_sets\trun\ttextures\n", file);
            for (uint32_t i = 0; i < p->draw_count; i++) {
                const struct pass_draw_record *row = &p->draw[i];
                char state[65];
                s2a_sha256_hex(row->state, state);
                if (!pass_draw_write_module(directory, row->vs, &modules) ||
                    !pass_draw_write_module(directory, row->fs, &modules)) { failed = 1; break; }
                if (fprintf(file, "%llu\t%u\t%u\t%u\t%s\t%s\t%s\t%s\t%s\t%s\t%x\t%x\t%u\t%s\n",
                    (unsigned long long)row->seq, row->pass, row->ordinal, row->format, row->vs->sha,
                    row->vs_entry, row->fs->sha, row->fs_entry, row->pso, state, row->changes,
                    row->descriptors, row->run, row->textures) < 0) { failed = 1; break; }
            }
            if (fclose(file)) failed = 1;
        }
    }
    snprintf(line, sizeof line, "end frames=%u rows=%u modules=%u failed=%d",
             p->frames, p->draw_count, modules, failed);
    nvn_plat_log(failed ? NVN_PLAT_LOG_RED : NVN_PLAT_LOG_INFO, "pass-draw-export", line);
}
