// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Immutable pool snapshots. All object ownership changes are made under g_mu.
static void pool_destroy_locked(struct nvn_gpu_pool_table_s *t) {
    struct nvn_gpu_pool_table_s **pp = &g.pools;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp) *pp = t->next;
    if (t->objects) for (uint32_t i = 0; i <= t->count; i++)
        if (t->objects[i]) unref_locked(&g, t->objects[i], t->kind == NVN_GPU_POOL_IMAGE ? REF_VIEW : REF_SAMP);
    free(t->objects); free(t->images); free(t);
}

static int pool_view_class(const struct nvn_gpu_view_s *v) {
    nvn_fmt_info info;
    if (!v || !v->tex || !v->view || !nvn_fmt_info_of(v->nvn_format, &info)) return -1;
    return info.kind == NVN_FMT_UINT ? 1 : info.kind == NVN_FMT_SINT ? 2 : 0;
}
static struct nvn_gpu_view_s *pool_live_view(nvn_gpu_view h) {
    struct nvn_gpu_view_s *v = g.views;
    while (v && v != h) v = v->all_next;
    return v && !v->released ? v : NULL;
}
static struct nvn_gpu_samp_s *pool_live_samp(nvn_gpu_samp h) {
    struct nvn_gpu_samp_s *s = g.samps;
    while (s && s != h) s = s->all_next;
    return s && !s->released ? s : NULL;
}

nvn_gpu_rc nvn_gpu_pool_create(const nvn_gpu_pool_desc *d, nvn_gpu_pool_table *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "pool out is null");
    *out = NULL;
    if (!d || (d->kind != NVN_GPU_POOL_IMAGE && d->kind != NVN_GPU_POOL_SAMPLER) || (d->count && !d->items) || d->sampler_integer > 1 || (d->kind == NVN_GPU_POOL_IMAGE && d->sampler_integer))
        return fail(err, NVN_GPU_E_ARG, "pool kind or items invalid");
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "pool session closed");
    if (!g.dev->descriptor_indexing_enabled)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "pool requires enabled descriptor indexing");
    const VkPhysicalDeviceLimits *l = &g.dev->properties.limits;
    uint32_t lim1 = d->kind == NVN_GPU_POOL_IMAGE ? l->maxPerStageDescriptorSampledImages : l->maxPerStageDescriptorSamplers;
    uint32_t lim2 = d->kind == NVN_GPU_POOL_IMAGE ? l->maxDescriptorSetSampledImages : l->maxDescriptorSetSamplers;
    if (d->count >= UINT16_MAX || (uint64_t)d->count + 1 > lim1 || (uint64_t)d->count + 1 > lim2 ||
        (d->kind == NVN_GPU_POOL_IMAGE && (uint64_t)d->count + 1 > l->maxPerStageResources))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "pool descriptor count exceeds stage, set or layout limit");
    struct nvn_gpu_pool_table_s *t = calloc(1, sizeof *t);
    if (!t) return fail(err, NVN_GPU_E_OOM, "pool table allocation failed");
    t->kind = d->kind; t->count = d->count;
    t->objects = calloc((size_t)d->count + 1, sizeof *t->objects);
    t->images = calloc((size_t)d->count + 1, sizeof *t->images);
    if (!t->objects || !t->images) { free(t->objects); free(t->images); free(t); return fail(err, NVN_GPU_E_OOM, "pool entries allocation failed"); }
    int error = 0; const char *reason = NULL;
    pthread_mutex_lock(&g_mu);
    if (g.closing) { error = NVN_GPU_E_STOPPED; reason = "pool session closing"; goto failed; }
    struct nvn_gpu_view_s *dv = NULL; struct nvn_gpu_samp_s *ds = NULL; int cls = -1;
    if (d->kind == NVN_GPU_POOL_IMAGE) {
        dv = pool_live_view(d->dummy.view); cls = pool_view_class(dv);
        if (cls < 0 || !(dv->tex->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) { error = NVN_GPU_E_ARG; reason = "pool dummy image invalid"; goto failed; }
    } else {
        ds = pool_live_samp(d->dummy.samp);
        if (!ds) { error = NVN_GPU_E_ARG; reason = "pool dummy sampler invalid"; goto failed; }
    }
    for (uint32_t i = 0; i <= d->count; i++) {
        uint32_t state = i == d->count ? NVN_GPU_POOL_EMPTY : d->items[i].state;
        if (state == NVN_GPU_POOL_UNAVAILABLE) { error = NVN_GPU_E_UNSUPPORTED; reason = "pool registered resource unavailable"; goto failed; }
        if (state != NVN_GPU_POOL_EMPTY && state != NVN_GPU_POOL_READY) { error = NVN_GPU_E_ARG; reason = "pool item state invalid"; goto failed; }
        if (d->kind == NVN_GPU_POOL_IMAGE) {
            struct nvn_gpu_view_s *v = dv;
            if (state == NVN_GPU_POOL_READY) {
                v = pool_live_view(d->items[i].resource.view);
                if (!v) { error = NVN_GPU_E_ARG; reason = "pool image handle invalid or released"; goto failed; }
                int got = pool_view_class(v);
                if (v->buffer_view) v = dv;
                else if (got < 0) { error = NVN_GPU_E_UNSUPPORTED; reason = "pool image numeric type unproven"; goto failed; }
                else if (got != cls || v->type != dv->type || v->aspect != dv->aspect || v->tex->samples != dv->tex->samples) v = dv;
                else if (!(v->tex->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) { error = NVN_GPU_E_UNSUPPORTED; reason = "pool image is not sampleable"; goto failed; }
            }
            t->objects[i] = v; ref_bump(v, REF_VIEW);
            t->images[i].imageView = v->view; t->images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        } else {
            struct nvn_gpu_samp_s *s = state == NVN_GPU_POOL_READY ? pool_live_samp(d->items[i].resource.samp) : ds;
            if (!s) { error = NVN_GPU_E_ARG; reason = "pool sampler handle invalid or released"; goto failed; }
            t->objects[i] = s; ref_bump(s, REF_SAMP); t->images[i].sampler = d->sampler_integer ? s->int_sampler : s->sampler;
        }
    }
    t->next = g.pools; g.pools = t;
    pthread_mutex_unlock(&g_mu);
    *out = t; return rc(NVN_GPU_OK);
failed:
    pool_destroy_locked(t);
    pthread_mutex_unlock(&g_mu);
    return fail(err, error, "%s", reason);
}

// Limits apply to the full pipeline layout, including bindings outside the pool.
// Samplers count against sampler limits but not maxPerStageResources.
static const char *pool_pipeline_limit(const struct nvn_gpu_pipe_s *p, const nvn_gpu_pipe_desc *graphics) {
    const VkPhysicalDeviceLimits *l = &g.dev->properties.limits;
    uint64_t images = 0, samplers = 0, stage_images[6] = {0}, stage_samplers[6] = {0}, resources[6] = {0};
    for (uint32_t set = 0; set < p->nset; set++) {
        const struct dset_lay *layout = p->dlay[set];
        if (!layout) return "pool layout missing";
        for (uint32_t i = 0; i < layout->nb; i++) {
            const struct dset_lb *b = &layout->b[i];
            int image = b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            int sampler = b->type == VK_DESCRIPTOR_TYPE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            int resource = b->type != VK_DESCRIPTOR_TYPE_SAMPLER;
            if (image) images += b->count;
            if (sampler) samplers += b->count;
            for (int st = 0; st <= NVN_GPU_SH_CS; st++) if (b->stages & vk_shader_stage((nvn_gpu_stage)st)) {
                if (image) stage_images[st] += b->count;
                if (sampler) stage_samplers[st] += b->count;
                if (resource) resources[st] += b->count;
            }
        }
    }
    if (graphics) for (uint32_t i = 0; i < 8; i++) if (graphics->cfmt[i]) resources[NVN_GPU_SH_FS]++;
    if (images > l->maxDescriptorSetSampledImages) return "pool maxDescriptorSetSampledImages exceeded";
    if (samplers > l->maxDescriptorSetSamplers) return "pool maxDescriptorSetSamplers exceeded";
    for (int st = 0; st <= NVN_GPU_SH_CS; st++) {
        if (stage_images[st] > l->maxPerStageDescriptorSampledImages) return "pool maxPerStageDescriptorSampledImages exceeded";
        if (stage_samplers[st] > l->maxPerStageDescriptorSamplers) return "pool maxPerStageDescriptorSamplers exceeded";
        if (resources[st] > l->maxPerStageResources) return "pool maxPerStageResources exceeded";
    }
    return NULL;
}

// Pool-sourced bindings opt in to preflight even when their individual counts are small.
static int pool_shader_source(const struct nvn_gpu_shader_s *sh) {
    if (!sh) return 0;
    for (uint32_t i = 0; i < sh->nent; i++)
        if (sh->ents[i].src_kind == NVN_RB_TEXP || sh->ents[i].src_kind == NVN_RB_SAMPP) return 1;
    return 0;
}
static const char *pool_pipeline_preflight(const struct nvn_gpu_pipe_s *p, const nvn_gpu_pipe_desc *graphics,
                                           const struct nvn_gpu_shader_s *compute) {
    if (pool_shader_source(compute)) return pool_pipeline_limit(p, graphics);
    if (graphics) for (int st = 0; st < 5; st++)
        if (pool_shader_source(graphics->st[st].sh)) return pool_pipeline_limit(p, graphics);
    return NULL;
}

static nvn_gpu_rc pool_bind_error(nvn_gpu_cmd c, nvn_gpu_err *err, int code, const char *message) {
    if (c) set_sticky(c, code, "%s", message);
    return fail(err, code, "%s", message);
}

// 池表不在「句柄活到 cmd 完成」的核心保证内（release 线程：[任意]），所以「在册 + 未释放 + 加引用」
// 必须在同一个 g_mu 临界区里完成；在此之前对入参只比较指针、不解引用（销毁前一定先摘链）。
// 成功：本段持一份引用（登记在 c->refs）；失败：0，不入引用表。
#ifdef S2A_NVK_POOL_HOST_TEST
static void (*g_pool_test_hook)(nvn_gpu_pool_table);
void nvn_gpu_vk_test_pool_hook(void (*fn)(nvn_gpu_pool_table)) { g_pool_test_hook = fn; }
#endif
static int pool_cmd_ref(struct nvn_gpu_cmd_s *c, nvn_gpu_pool_table t) {
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pool_table_s *p = g.pools;
    while (p && p != t) p = p->next;
    if (!p || p->released) { pthread_mutex_unlock(&g_mu); return 0; }
    p->refs++;
    pthread_mutex_unlock(&g_mu);
    return cmd_ref_append(c, t, REF_POOL);   // OOM 时内部已归还这一份引用
}

// 布局 / 限额检查失败时归还刚加的那一份（录制线程单线程，引用表最后一项就是它）。
static void pool_cmd_unref(struct nvn_gpu_cmd_s *c, nvn_gpu_pool_table t) {
    if (c->nrefs && c->refs[c->nrefs - 1].obj == t && c->refs[c->nrefs - 1].type == REF_POOL) c->nrefs--;
    pthread_mutex_lock(&g_mu);
    unref_locked(&g, t, REF_POOL);
    pthread_mutex_unlock(&g_mu);
}

nvn_gpu_rc nvn_gpu_pool_bind(nvn_gpu_cmd c, nvn_gpu_stage st, uint32_t ent, nvn_gpu_pool_table t, nvn_gpu_err *err) {
    if (!c || !t || (uint32_t)st > NVN_GPU_SH_CS || ent >= 64 || !c->pipe || ent >= c->pipe->stage_nent[st])
        return pool_bind_error(c, err, NVN_GPU_E_ARG, "pool bind command, stage or entry invalid");
    if (!rc_is(c->sticky, NVN_GPU_OK)) return fail(err, c->sticky.code, "%s", c->why);
    if (!pool_cmd_ref(c, t)) {
        if (!rc_is(c->sticky, NVN_GPU_OK)) return fail(err, c->sticky.code, "%s", c->why);
        return pool_bind_error(c, err, NVN_GPU_E_ARG, "pool table invalid or released");
    }
#ifdef S2A_NVK_POOL_HOST_TEST
    if (g_pool_test_hook) { void (*hook)(nvn_gpu_pool_table) = g_pool_test_hook; g_pool_test_hook = NULL; hook(t); }
#endif
    // 下面可以安全解引用 t：本段已持一份引用，release 只能置 released，销毁推迟到段完成 / abort。
    const nvn_gpu_layout_ent *e = &c->pipe->stage_ents[st][ent];
    if (e->set >= 4 || e->count != t->count + 1 ||
        (t->kind == NVN_GPU_POOL_IMAGE ? (e->cls != NVN_GPU_RES_TEX || e->src_kind != NVN_RB_TEXP)
                                        : (e->cls != NVN_GPU_RES_SAMP || e->src_kind != NVN_RB_SAMPP))) {
        pool_cmd_unref(c, t);
        return pool_bind_error(c, err, NVN_GPU_E_LAYOUT, "pool table type, size or set conflicts with shader layout");
    }
    const char *limit = pool_pipeline_limit(c->pipe, c->pipe->pass ? &c->pipe->key : NULL);
    if (limit) { pool_cmd_unref(c, t); return pool_bind_error(c, err, NVN_GPU_E_UNSUPPORTED, limit); }
    for (uint32_t i = 0; i < c->nbinds[st]; i++)
        if (c->binds[st][i].b.ent == ent) {
            pool_cmd_unref(c, t);
            return pool_bind_error(c, err, NVN_GPU_E_LAYOUT, "pool conflicts with ordinary binding");
        }
    c->pool_binds[st][ent] = t; c->pool_active = 1; c->desc_dirty |= 1u << e->set;
    return rc(NVN_GPU_OK);
}
void nvn_gpu_pool_release(nvn_gpu_pool_table t) {
    if (!t) return;
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pool_table_s *found = g.pools;
    while (found && found != t) found = found->next;
    if (found && !found->released) {
        found->released = 1; rel_epoch_bump();
        if (!found->refs) pool_destroy_locked(found);
    }
    pthread_mutex_unlock(&g_mu);
}

static int pool_set_mask(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute, uint32_t *mask) {
    *mask = 0;
    nvn_gpu_pool_table seen[4][32] = {{0}}; uint32_t bind[4][32] = {{0}}, n[4] = {0};
    for (int st = compute ? NVN_GPU_SH_CS : 0; st < (compute ? NVN_GPU_SH_CS + 1 : 5); st++) {
        for (uint32_t i = 0; i < 64; i++) {
            nvn_gpu_pool_table t = c->pool_binds[st][i]; if (!t) continue;
            if (i >= p->stage_nent[st]) { set_sticky(c, NVN_GPU_E_LAYOUT, "pool entry missing from pipeline"); return 0; }
            const nvn_gpu_layout_ent *e = &p->stage_ents[st][i];
            if (e->set >= p->nset || e->count != t->count + 1 ||
                (t->kind == NVN_GPU_POOL_IMAGE ? e->cls != NVN_GPU_RES_TEX : e->cls != NVN_GPU_RES_SAMP)) {
                set_sticky(c, NVN_GPU_E_LAYOUT, "pool table does not match pipeline"); return 0;
            }
            uint32_t k = 0; while (k < n[e->set] && bind[e->set][k] != e->binding) k++;
            if (k == n[e->set]) {
                if (k == 32) { set_sticky(c, NVN_GPU_E_LAYOUT, "too many pool bindings"); return 0; }
                bind[e->set][k] = e->binding; seen[e->set][k] = t; n[e->set]++;
            } else if (seen[e->set][k] != t) { set_sticky(c, NVN_GPU_E_LAYOUT, "shared pool binding differs across stages"); return 0; }
            *mask |= 1u << e->set;
        }
    }
    return 1;
}

static int pool_alloc_set(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, uint32_t set, VkDescriptorSet *out) {
    const struct dset_lay *l = p->dlay[set]; VkDescriptorPoolSize sizes[32] = {{0}}; uint32_t ns = 0;
    if (!l) { set_sticky(c, NVN_GPU_E_LAYOUT, "pool set layout missing"); return 0; }
    for (uint32_t i = 0; i < l->nb; i++) {
        uint32_t j = 0; while (j < ns && sizes[j].type != (VkDescriptorType)l->b[i].type) j++;
        if (j == ns) { sizes[ns].type = (VkDescriptorType)l->b[i].type; ns++; }
        if (UINT32_MAX - sizes[j].descriptorCount < l->b[i].count) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "pool descriptor count overflow"); return 0; }
        sizes[j].descriptorCount += l->b[i].count;
    }
    if (c->pool_ndpools == c->pool_dpool_cap) {
        uint32_t cap = c->pool_dpool_cap ? c->pool_dpool_cap * 2 : 4;
        VkDescriptorPool *v = realloc(c->pool_dpools, (size_t)cap * sizeof *v);
        if (!v) { set_sticky(c, NVN_GPU_E_OOM, "pool descriptor ownership allocation failed"); return 0; }
        c->pool_dpools = v; c->pool_dpool_cap = cap;
    }
    VkDescriptorPoolCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 1; ci.poolSizeCount = ns; ci.pPoolSizes = sizes; VkDescriptorPool dp = VK_NULL_HANDLE;
    VkResult vr = vkCreateDescriptorPool(g.d, &ci, NULL, &dp);
    if (vr != VK_SUCCESS) { set_sticky(c, NVN_GPU_E_OOM, "pool vkCreateDescriptorPool failed %d", vr); return 0; }
    c->pool_dpools[c->pool_ndpools++] = dp;
    VkDescriptorSetAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = dp; ai.descriptorSetCount = 1; ai.pSetLayouts = &p->dsl[set];
    vr = vkAllocateDescriptorSets(g.d, &ai, out);
    if (vr != VK_SUCCESS) { set_sticky(c, NVN_GPU_E_OOM, "pool vkAllocateDescriptorSets failed %d", vr); return 0; }
    return 1;
}

static int pool_write_sets(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute, uint32_t dirty) {
    VkWriteDescriptorSet writes[6 * 64]; uint32_t n = 0;
    VkDescriptorImageInfo *copies[6 * 64] = {0};
    for (int st = compute ? NVN_GPU_SH_CS : 0; st < (compute ? NVN_GPU_SH_CS + 1 : 5); st++) {
        for (uint32_t i = 0; i < 64; i++) {
            nvn_gpu_pool_table t = c->pool_binds[st][i]; if (!t) continue;
            const nvn_gpu_layout_ent *e = &p->stage_ents[st][i]; if (!(dirty & (1u << e->set))) continue;
            uint32_t j = 0; for (; j < n; j++) if (writes[j].dstSet == c->desc_sets[e->set] && writes[j].dstBinding == e->binding) break;
            if (j < n) continue;
            VkWriteDescriptorSet w = {0}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = c->desc_sets[e->set]; w.dstBinding = e->binding; w.descriptorCount = t->count + 1;
            w.descriptorType = t->kind == NVN_GPU_POOL_IMAGE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            w.pImageInfo = t->images;
            if (t->kind == NVN_GPU_POOL_IMAGE && c->feedback != c->feedback_stop) {
                VkDescriptorImageInfo *images = malloc(((size_t)t->count + 1) * sizeof *images);
                if (!images) {
                    for (uint32_t k = 0; k < n; k++) free(copies[k]);
                    set_sticky(c, NVN_GPU_E_OOM, "feedback pool descriptors allocation failed"); return 0;
                }
                memcpy(images, t->images, ((size_t)t->count + 1) * sizeof *images);
                for (uint32_t k = 0; k <= t->count; k++) images[k].imageView = feedback_view(c, t->objects[k]);
                copies[n] = images; w.pImageInfo = images;
            }
            writes[n++] = w;
        }
    }
    if (n) vkUpdateDescriptorSets(g.d, n, writes, 0, NULL);
    for (uint32_t k = 0; k < n; k++) free(copies[k]);
    return 1;
}
static int pool_check_layouts(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute) {
    (void)p;
    if (!c->opt) return 1;
    for (int st = compute ? NVN_GPU_SH_CS : 0; st < (compute ? NVN_GPU_SH_CS + 1 : 5); st++)
        for (uint32_t i = 0; i < 64; i++) {
            nvn_gpu_pool_table t = c->pool_binds[st][i]; if (!t || t->kind != NVN_GPU_POOL_IMAGE) continue;
            for (uint32_t j = 0; j <= t->count; j++) {
                struct nvn_gpu_view_s *v = t->objects[j];
                if (feedback_view(c, v) != v->view) continue;
                if (lay_cur(c, v->tex) != VK_IMAGE_LAYOUT_GENERAL) {
                    set_sticky(c, NVN_GPU_E_UNSUPPORTED, "pool image requires GENERAL layout"); return 0;
                }
            }
        }
    return 1;
}

#ifdef S2A_NVK_POOL_HOST_TEST
int nvn_gpu_vk_test_pool_refs(nvn_gpu_pool_table t) {
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pool_table_s *p = g.pools;
    while (p && p != t) p = p->next;
    int refs = p ? p->refs : 0;
    pthread_mutex_unlock(&g_mu);
    return refs;
}
int nvn_gpu_vk_test_pool_live(nvn_gpu_pool_table t) {
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pool_table_s *p = g.pools;
    while (p && p != t) p = p->next;
    int live = p != NULL;
    pthread_mutex_unlock(&g_mu);
    return live;
}

// ---------------------------------------------------------------- bind 竞态夹具
// 不建 VkDevice：手工造在册空表 + 假 cmd/pipe，只跑 nvn_gpu_pool_bind 的句柄与锁路径。
// 受控交错由 g_pool_test_hook 提供：bind 取到引用之后（修前等价于「验活之后、取引用之前」）
// 同线程执行 release，判据直接看「bind 取到的是不是已释放/已销毁的表」。
static void pool_test_release_hook(nvn_gpu_pool_table t) { nvn_gpu_pool_release(t); }

// 放掉本段最后加的那一份池表引用。与修法无关，修前修后都能编过。
static void pool_test_drop_cmd_ref(struct nvn_gpu_cmd_s *c, nvn_gpu_pool_table t) {
    if (c->nrefs && c->refs[c->nrefs - 1].obj == t && c->refs[c->nrefs - 1].type == REF_POOL) c->nrefs--;
    pthread_mutex_lock(&g_mu);
    unref_locked(&g, t, REF_POOL);
    pthread_mutex_unlock(&g_mu);
}

static struct nvn_gpu_pool_table_s *pool_test_make_table(void) {
    struct nvn_gpu_pool_table_s *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->kind = NVN_GPU_POOL_IMAGE; t->count = 0;
    t->objects = calloc(1, sizeof *t->objects);
    t->images = calloc(1, sizeof *t->images);
    if (!t->objects || !t->images) { free(t->objects); free(t->images); free(t); return NULL; }
    pthread_mutex_lock(&g_mu);
    t->next = g.pools; g.pools = t;
    pthread_mutex_unlock(&g_mu);
    return t;
}

static struct nvn_gpu_pipe_s *pool_test_make_pipe(uint16_t count) {
    struct nvn_gpu_pipe_s *p = calloc(1, sizeof *p);
    nvn_gpu_layout_ent *e = calloc(1, sizeof *e);
    struct dset_lay *l = calloc(1, sizeof *l + sizeof(struct dset_lb));
    if (!p || !e || !l) { free(p); free(e); free(l); return NULL; }
    p->nset = 1; p->stage_nent[NVN_GPU_SH_CS] = 1;
    p->stage_ents[NVN_GPU_SH_CS] = e;
    p->dlay[0] = l;
    l->nb = 1;
    l->b[0] = (struct dset_lb){ .binding = 0, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                .count = 1, .stages = VK_SHADER_STAGE_COMPUTE_BIT };
    *e = (nvn_gpu_layout_ent){ .set = 0, .cls = NVN_GPU_RES_TEX, .count = count,
                               .binding = 0, .src_kind = NVN_RB_TEXP, .stages = 1u << NVN_GPU_SH_CS };
    return p;
}

// 假设备：bind 的管线限额预检只读 limits；不建实例 / 设备。
static struct s2a_vk_device *pool_test_fake_dev(void) {
    struct s2a_vk_device *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->properties.limits.maxDescriptorSetSampledImages = 1000000;
    d->properties.limits.maxDescriptorSetSamplers = 1000000;
    d->properties.limits.maxPerStageDescriptorSampledImages = 1000000;
    d->properties.limits.maxPerStageDescriptorSamplers = 1000000;
    d->properties.limits.maxPerStageResources = 1000000;
    return d;
}

static void pool_test_free_cmd(struct nvn_gpu_cmd_s *c) {
    if (!c) return;
    free(c->refs); free(c->refseen); free(c);
}

int nvn_gpu_vk_test_pool_race(void) {
    int bad = 0;
    struct s2a_vk_device *dev = pool_test_fake_dev();
    struct s2a_vk_device *saved_dev = g.dev;
    if (!dev) return 1;
    g.dev = dev;
    struct nvn_gpu_pool_table_s *t = pool_test_make_table();
    struct nvn_gpu_pipe_s *p = pool_test_make_pipe(1);
    struct nvn_gpu_cmd_s *c = calloc(1, sizeof *c);
    if (!t || !p || !c) return 1;
    c->pipe = p;

    nvn_gpu_err err = {0};
    nvn_gpu_vk_test_pool_hook(pool_test_release_hook);
    nvn_gpu_rc r = nvn_gpu_pool_bind(c, NVN_GPU_SH_CS, 0, t, &err);
    if (!nvn_gpu_ok(r)) { printf("FAIL race bind: rc=%d %s\n", r.code, err.why); bad++; }
    if (nvn_gpu_vk_test_pool_refs(t) != 1) { printf("FAIL race refs\n"); bad++; }
    if (!nvn_gpu_vk_test_pool_live(t)) { printf("FAIL race table died while cmd holds it\n"); bad++; }
    nvn_gpu_pool_release(t);   // 幂等：引用仍在 cmd 手里，表不能销毁
    if (!nvn_gpu_vk_test_pool_live(t)) { printf("FAIL repeated release while ref held\n"); bad++; }
    pool_test_drop_cmd_ref(c, t);
    if (nvn_gpu_vk_test_pool_live(t)) { printf("FAIL table not retired after ref drop\n"); bad++; }
    pool_test_free_cmd(c); free(p->stage_ents[NVN_GPU_SH_CS]); free(p->dlay[0]); free(p);

    // 对照：表已 release 但仍有别的持有者（refs=1）时，bind 必须拒绝，不能复活已释放表。
    struct nvn_gpu_pool_table_s *t2 = pool_test_make_table();
    struct nvn_gpu_pipe_s *p2 = pool_test_make_pipe(1);
    struct nvn_gpu_cmd_s *c2 = calloc(1, sizeof *c2);
    if (!t2 || !p2 || !c2) return 1;
    c2->pipe = p2;
    pthread_mutex_lock(&g_mu); t2->refs = 1; pthread_mutex_unlock(&g_mu);
    nvn_gpu_pool_release(t2);   // released=1，refs=1：留在册上，但已不可再 bind
    memset(&err, 0, sizeof err);
    r = nvn_gpu_pool_bind(c2, NVN_GPU_SH_CS, 0, t2, &err);
    if (r.code != NVN_GPU_E_ARG) { printf("FAIL released table resurrected: rc=%d\n", r.code); bad++; }
    pthread_mutex_lock(&g_mu); unref_locked(&g, t2, REF_POOL); pthread_mutex_unlock(&g_mu);
    pool_test_free_cmd(c2); free(p2->stage_ents[NVN_GPU_SH_CS]); free(p2->dlay[0]); free(p2);

    // 对照：布局不符时 bind 失败且不得留下引用（表应能被 release 立即销毁）。
    struct nvn_gpu_pool_table_s *t3 = pool_test_make_table();
    struct nvn_gpu_pipe_s *p3 = pool_test_make_pipe(2);   // 池 count=0，布局 count 必须是 1
    struct nvn_gpu_cmd_s *c3 = calloc(1, sizeof *c3);
    if (!t3 || !p3 || !c3) return 1;
    c3->pipe = p3;
    memset(&err, 0, sizeof err);
    r = nvn_gpu_pool_bind(c3, NVN_GPU_SH_CS, 0, t3, &err);
    if (r.code != NVN_GPU_E_LAYOUT) { printf("FAIL layout mismatch: rc=%d\n", r.code); bad++; }
    if (nvn_gpu_vk_test_pool_refs(t3) != 0) { printf("FAIL layout failure leaked ref\n"); bad++; }
    nvn_gpu_pool_release(t3);
    if (nvn_gpu_vk_test_pool_live(t3)) { printf("FAIL layout failure kept table alive\n"); bad++; }
    pool_test_free_cmd(c3); free(p3->stage_ents[NVN_GPU_SH_CS]); free(p3->dlay[0]); free(p3);

    g.dev = saved_dev;
    free(dev);
    return bad;
}
#endif
