// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Immutable pool snapshots. All object ownership changes are made under g_mu.
static void pool_destroy_locked(struct nvn_gpu_pool_table_s *t) {
    struct nvn_gpu_pool_table_s **pp = &g.pools;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp) *pp = t->next;
    if (t->objects) for (uint32_t i = 0; i <= t->count; i++)
        if (t->objects[i]) unref_locked(&g, t->objects[i], t->kind == NVN_GPU_POOL_IMAGE ? REF_VIEW : REF_SAMP);
    free(t->uviews); free(t->unext); free(t->ikey); free(t->ihead);
    free(t->objects); free(t->images); free(t);
}

// 图像池的按图索引（建表时调一次，之后只读）：objects[0..count] 先按视图指针去重（占位视图会占掉很多项），再按 VkImage 串链。
// 建不出（内存不足）不算错：表照常可用，反馈检测退回逐项扫（ikey == NULL）。
static void pool_build_index(struct nvn_gpu_pool_table_s *t) {
    const uint32_t n = t->count + 1;
    uint32_t cap = 16; while (cap < n * 2) cap <<= 1;
    uint64_t *seen = calloc(cap, sizeof *seen), *ik = calloc(cap, sizeof *ik);
    void **uv = malloc((size_t)n * sizeof *uv); uint32_t *un = malloc((size_t)n * sizeof *un), *ih = malloc((size_t)cap * sizeof *ih);
    if (!seen || !ik || !uv || !un || !ih) { free(seen); free(ik); free(uv); free(un); free(ih); return; }
    uint32_t nu = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct nvn_gpu_view_s *v = t->objects[i];
        const uint64_t img = v && v->tex ? (uint64_t)(uintptr_t)v->tex->image : 0, vk = (uint64_t)(uintptr_t)v;
        if (!img) continue;
        uint32_t h = pool_hash64(vk, cap);
        while (seen[h] && seen[h] != vk) h = (h + 1) & (cap - 1);
        if (seen[h]) continue;   // 这个视图已收过
        seen[h] = vk; uv[nu] = v;
        uint32_t g = pool_hash64(img, cap);
        while (ik[g] && ik[g] != img) g = (g + 1) & (cap - 1);
        if (!ik[g]) { ik[g] = img; ih[g] = UINT32_MAX; }
        un[nu] = ih[g]; ih[g] = nu; nu++;
    }
    free(seen);
    t->uviews = uv; t->unext = un; t->ikey = ik; t->ihead = ih; t->nuniq = nu; t->idx_cap = cap;
}

int nvn_gpu_pool_has_texture(nvn_gpu_pool_table t, nvn_gpu_tex tex) {
    if (!t || t->kind != NVN_GPU_POOL_IMAGE || !tex || !tex->image) return 0;
    if (t->ikey) return pool_img_first(t, (uint64_t)(uintptr_t)tex->image) != UINT32_MAX;
    for (uint32_t i = 0; i <= t->count; i++) {
        const struct nvn_gpu_view_s *v = t->objects[i];
        if (v && v->tex && v->tex->image == tex->image) return 1;
    }
    return 0;
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
    uint32_t element = UINT32_MAX;
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
        element = i;
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
            VkSampler sampler = d->sampler_integer ? s->int_sampler : s->sampler;
            if (!sampler) { error = NVN_GPU_E_LAYOUT; reason = "pool sampler unavailable"; goto failed; }
            t->objects[i] = s; ref_bump(s, REF_SAMP); t->images[i].sampler = sampler;
        }
    }
    if (d->kind == NVN_GPU_POOL_IMAGE) pool_build_index(t);
    t->uid = ++g_pool_uid;
    t->next = g.pools; g.pools = t;
    pthread_mutex_unlock(&g_mu);
    *out = t; return rc(NVN_GPU_OK);
failed:
    pool_destroy_locked(t);
    pthread_mutex_unlock(&g_mu);
    if (element != UINT32_MAX)
        return fail(err, error, "%s: kind=%u element=%u", reason, d->kind, element);
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
    if (images > l->maxDescriptorSetSampledImages) return "本机 pool maxDescriptorSetSampledImages exceeded";
    if (samplers > l->maxDescriptorSetSamplers) return "本机 pool maxDescriptorSetSamplers exceeded";
    for (int st = 0; st <= NVN_GPU_SH_CS; st++) {
        if (stage_images[st] > l->maxPerStageDescriptorSampledImages) return "本机 pool maxPerStageDescriptorSampledImages exceeded";
        if (stage_samplers[st] > l->maxPerStageDescriptorSamplers) return "本机 pool maxPerStageDescriptorSamplers exceeded";
        if (resources[st] > l->maxPerStageResources) return "本机 pool maxPerStageResources exceeded";
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

// 整池组的键里「这个池绑定」那一项：（绑定号, 池快照序号）混成 64 位，两路独立混合（两路都撞才会误认；备忘 / 回收没有「内容再核对」这一步，键要足够强）。
// 用序号不用指针：池快照销毁后地址会被新表复用，指针相同不代表内容相同；序号全进程单调增、不复用。每组各项相加（与阶段顺序无关）。
static inline uint64_t pool_key_mix1(uint32_t binding, uint64_t uid) {
    uint64_t x = uid * 0x9E3779B97F4A7C15ull + binding + 1u;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static inline uint64_t pool_key_mix2(uint32_t binding, uint64_t uid) {
    uint64_t x = (uid + 0x632BE59BD9B4E019ull) * 0xD6E8FEB86659FD93ull ^ ((uint64_t)binding << 32 | 0x85EBCA6Bu);
    x ^= x >> 32; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull;
    return x ^ (x >> 29);
}

// 本笔哪些组带整池数组（mask），以及每组的键与第一张池快照（info[set]；组内同一池绑定被多个阶段共用只算一次）。
static int pool_set_mask(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute, uint32_t *mask, struct pool_set_info *info) {
    *mask = 0; for (int s = 0; s < 4; s++) { info[s].k1 = info[s].k2 = 0; info[s].own = NULL; }
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
                info[e->set].k1 += pool_key_mix1(e->binding, t->uid); info[e->set].k2 += pool_key_mix2(e->binding, t->uid);
                if (!info[e->set].own) info[e->set].own = t;
            } else if (seen[e->set][k] != t) { set_sticky(c, NVN_GPU_E_LAYOUT, "shared pool binding differs across stages"); return 0; }
            *mask |= 1u << e->set;
        }
    }
    return 1;
}

static int pool_alloc_set(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, uint32_t set, VkDescriptorSet *out) {
    const int tm = g_draw_tm; const uint64_t t0 = tm ? now_ns() : 0;   // 读数：建池 + 分组用多久（nvk_gputime 开着才计）
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
    __atomic_fetch_add(&g_pool_n[PN_NEW], 1, __ATOMIC_RELAXED);
    if (tm) __atomic_fetch_add(&g_pool_ns[0], now_ns() - t0, __ATOMIC_RELAXED);
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// 整池组回收（nvk_pool_rec，缺省开；0 = 关，回到「每笔新建一个单组专用池、整表重写、段完成销毁」的老路径，A/B 与负对照用）。
// 真机读数（一款 3D 游戏的标题场景，稳态每帧；同一个包只差本开关，3 局中位）：关回收（只剩段内备忘）时要 289.2 个整池组，段内备忘命中 127.3 个，向驱动新建 164.2 个；整池写入 241 万项（85.0 毫秒），建池 + 分组 12.1 毫秒，上屏 6.3 帧 / 秒；开回收后新建 0 个、整池写入 424 项（0.0 毫秒），上屏 23.7 帧 / 秒。
// 池快照建好就不可变，整池里写的内容只由「布局 + 池快照序号」决定，
// 所以整池组用完不销毁，按布局放回空闲串；下一笔要同样的池快照（键同 pool_set_mask）时取回来，整张表不用重写，只写这一笔的普通绑定（它们本来就要写）。
// 键对不上的空闲组也拿来用（省掉建池、分组），整表重写。
// 反馈拷贝在用时，整池里被拷贝的那几个视图要换成快照（老路径：整表复制一份再换）：回收的组只对那几项发补丁写入，补丁记在本段名下，
// 段完成时在 finish_cmd 最先一步按正典内容写回，再放回空闲串；空闲串里的组内容永远是「纯正典」。
// 为什么对：
// 1) 回收组里的整池条目只会是（a）正典（池快照里的视图 / 采样器）或（b）本段的补丁；补丁段内打、段完成还原，其间这个组只属于这一段。
// 2) 取到的组键与这一笔相同，就是里面写着的正是这几张池快照（序号全进程单调增、不复用）；这几张表本段已持引用（pool_cmd_ref），表里的视图 / 采样器活到段完成。
//    键对不上就整表重写：整池数组每一项都被覆盖，空闲组里不会残留已销毁表的句柄被用到。键只在整表写完之后才记（prs_mark）。
// 3) 普通绑定部分与老的按布局回收（dset_take）同一套「写法签名」守卫：同一布局下这一笔的写法与表项里定下的不同，不从这一项取、走老路径。
// 4) 放回发生在段完成之后（GPU 不再用）；放弃的段在 finish_cmd 最开头先清空两级回放队列。补丁还原先于快照视图销毁、先于表引用归还。
// 总量按字节封顶：在用 + 空闲的回收组估算字节合计 ≤ g_prs_cap（缺省 384 MB）；空闲串另有 g_prs_idle_cap，缺省与总量封顶相等（空闲 ≤ 总量，等于只有总量封顶在起作用：
// 一帧用到的整池组在各段完成时一起放回空闲串，空闲封顶设成总量的一半就可能比一帧的用量还小，每帧放回时淘汰超出的、下一帧又重建；这个口子留给用例和调参）。要新建而放不下时，先淘汰全局最久没用的空闲组
// （各表项空闲串的头最老，seq 全局递增），淘汰完还放不下才退回老路径；段完成放回时空闲超封顶，同样先淘汰最久没用的。碰锁、写法不同、补丁太多、内存不够也一律退回老路径，不拦任何操作。
// ---------------------------------------------------------------------------------------------------------------------------------------------------
struct pool_patch { uint32_t binding, idx; VkDescriptorImageInfo canon; };   // 本段在回收组里改过的一项：位置 + 它的正典内容（段完成时写回）
struct pool_rs { struct pool_rs *next; VkDescriptorPool pool; VkDescriptorSet set; uint64_t k1, k2; uint64_t bytes, seq; };   // 一个可回收的整池组：独占一个池（maxSets = 1）；k1 / k2 = 里面各池绑定现在写着哪几张表（0, 0 = 内容未知）；bytes = 这个池的估算字节；seq = 最近一次放回空闲串的序号（越小越久没用）
struct pool_rbin { struct pool_rbin *next; const struct dset_lay *lay; struct pool_rs **free; uint32_t nfree, cap; uint64_t sig; uint8_t sig_set; uint64_t bytes; };   // 每个布局结构一项：空闲串（先放回的在前，seq 递增）+ 普通绑定的写法签名 + 这个布局一个整池组的估算字节
struct prs_use { struct pool_rs *rs; struct pool_rbin *bin; struct pool_patch *patch; uint32_t npatch; };   // 本段取过的一个回收组
static struct pool_rbin *g_prs_bins;
static int g_prs_closed = 1;

// 向驱动建一个独占池（maxSets = 1）并分出一组；NULL = 建不出（调用方退回老路径）。
static struct pool_rs *prs_make(const struct dset_lay *l) {
    VkDescriptorPoolSize sizes[32] = {{0}}; uint32_t ns = 0;
    for (uint32_t i = 0; i < l->nb; i++) {
        uint32_t j = 0; while (j < ns && sizes[j].type != (VkDescriptorType)l->b[i].type) j++;
        if (j == ns) { if (ns == 32) return NULL; sizes[ns].type = (VkDescriptorType)l->b[i].type; ns++; }
        if (UINT32_MAX - sizes[j].descriptorCount < l->b[i].count) return NULL;
        sizes[j].descriptorCount += l->b[i].count;
    }
    struct pool_rs *rs = calloc(1, sizeof *rs);
    if (!rs) return NULL;
    VkDescriptorPoolCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 1; ci.poolSizeCount = ns; ci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g.d, &ci, NULL, &rs->pool) != VK_SUCCESS) { free(rs); return NULL; }
    VkDescriptorSetAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = rs->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &l->dsl;
    if (vkAllocateDescriptorSets(g.d, &ai, &rs->set) != VK_SUCCESS) { vkDestroyDescriptorPool(g.d, rs->pool, NULL); free(rs); return NULL; }
    return rs;
}
static int prs_push(struct pool_rbin *b, struct pool_rs *rs) {   // 持 g_prs_mu；0 = 内存不够
    if (b->nfree == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 16;
        struct pool_rs **n = realloc(b->free, (size_t)cap * sizeof *n);
        if (!n) return 0;
        b->free = n; b->cap = cap;
    }
    b->free[b->nfree++] = rs;
    return 1;
}
// 一个整池组的估算字节：布局里全部描述符项数 × PRS_DESC_BYTES，至少 PRS_GROUP_MIN_BYTES（绑定数有上限，乘法不会溢出 64 位）。
static uint64_t prs_lay_bytes(const struct dset_lay *l) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < l->nb; i++) n += l->b[i].count;
    const uint64_t bytes = n * PRS_DESC_BYTES;
    return bytes < PRS_GROUP_MIN_BYTES ? PRS_GROUP_MIN_BYTES : bytes;
}
// 持 g_prs_mu：从空闲串里摘走最久没用的一个组（各表项空闲串的头是该串里最老的，seq 递增；全局取 seq 最小的头）并把账扣掉；没有空闲组返回 NULL。
// 返回的组已不在任何串里，由调用方在锁外销毁（空闲组的段都已完成，GPU 不再用）。
static struct pool_rs *prs_evict_oldest_locked(void) {
    struct pool_rbin *ob = NULL;
    for (struct pool_rbin *b = g_prs_bins; b; b = b->next)
        if (b->nfree && (!ob || b->free[0]->seq < ob->free[0]->seq)) ob = b;
    if (!ob) return NULL;
    struct pool_rs *rs = ob->free[0];
    memmove(ob->free, ob->free + 1, (size_t)(ob->nfree - 1) * sizeof *ob->free);
    ob->nfree--;
    g_prs_idle--; g_prs_total--; g_prs_idle_bytes -= rs->bytes; g_prs_bytes -= rs->bytes;
    __atomic_fetch_add(&g_prs_evict, 1, __ATOMIC_RELAXED);
    return rs;
}
static void prs_destroy_list(struct pool_rs *dead) {   // 锁外销毁（建出来的池连 CPU 侧结构一起）
    while (dead) { struct pool_rs *nx = dead->next; vkDestroyDescriptorPool(g.d, dead->pool, NULL); free(dead); dead = nx; }
}
// 取组。1 = 取到（*out = 组；*valid = 里面的整池内容已经是这一笔要的，不用再写；*uidx = 本段取用记录里的下标），0 = 没取到（调用方走 pool_alloc_set）。
// 先在空闲串里找键完全相同的（命中，整池免写）；没有就拿最老的一个（整表重写，省建池）；空闲串空了才向驱动要，到上限就不要了。
static int prs_take(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, uint32_t set, uint64_t sig, const struct pool_set_info *si,
                    VkDescriptorSet *out, int *valid, uint32_t *uidx) {
    const struct dset_lay *l = set < 4 ? p->dlay[set] : NULL;
    if (!l || !si->own) return 0;
    const int tm = g_draw_tm; const uint64_t t0 = tm ? now_ns() : 0;
    if (c->prs_n == c->prs_cap) {   // 先给本段的取用记录留好位置，之后取到的组一定记得下来
        const uint32_t cap = c->prs_cap ? c->prs_cap * 2 : 64;
        struct prs_use *n = realloc(c->prs_use, (size_t)cap * sizeof *n);
        if (!n) { __atomic_fetch_add(&g_prs_why[0], 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_pool_n[PN_RFALL], 1, __ATOMIC_RELAXED); return 0; }
        c->prs_use = n; c->prs_cap = cap;
    }
    struct pool_rs *rs = NULL, *dead = NULL; struct pool_rbin *b = NULL; int hit = 0, reserve = 0, why = -1; uint64_t nb = 0;
    if (pthread_mutex_trylock(&g_prs_mu) != 0) why = 0;
    else {
        if (g_prs_closed) why = 0;
        else {
            for (b = g_prs_bins; b && b->lay != l; b = b->next) {}
            if (!b && (b = calloc(1, sizeof *b))) { b->lay = l; b->bytes = prs_lay_bytes(l); b->next = g_prs_bins; g_prs_bins = b; }
            if (!b) why = 0;
            else if (b->sig_set && b->sig != sig) { why = 1; b = NULL; }   // 写法不同：不从这一项取
            else {
                uint32_t at = b->nfree;
                for (uint32_t i = b->nfree; i-- > 0;) if (b->free[i]->k1 == si->k1 && b->free[i]->k2 == si->k2) { at = i; hit = 1; break; }
                if (!hit && b->nfree) at = 0;   // 键都对不上：拿最老的
                if (at < b->nfree) {
                    rs = b->free[at];
                    memmove(b->free + at, b->free + at + 1, (size_t)(b->nfree - at - 1) * sizeof *b->free);
                    b->nfree--; g_prs_idle--; g_prs_idle_bytes -= rs->bytes;
                } else {
                    // 要向驱动新建一个：总量按字节封顶，放不下先淘汰全局最久没用的空闲组（任何布局的都算），淘汰完还放不下才退回老路径
                    nb = b->bytes;
                    for (struct pool_rs *ev; g_prs_bytes + nb > g_prs_cap && (ev = prs_evict_oldest_locked()); ) { ev->next = dead; dead = ev; }
                    if (g_prs_bytes + nb > g_prs_cap) { why = 2; b = NULL; }   // 到上限
                    else { g_prs_total++; g_prs_bytes += nb; reserve = 1; }
                }
                if (b && !b->sig_set) { b->sig = sig; b->sig_set = 1; }   // 第一次取到组：这一项的写法定为这一笔的
            }
        }
        pthread_mutex_unlock(&g_prs_mu);
    }
    prs_destroy_list(dead);   // 被淘汰的空闲组在锁外销毁
    if (reserve) {   // 向驱动要（锁外）：建池 + 分组
        rs = prs_make(l);
        if (!rs) {
            pthread_mutex_lock(&g_prs_mu);
            if (g_prs_total) g_prs_total--;
            g_prs_bytes = g_prs_bytes >= nb ? g_prs_bytes - nb : 0;
            pthread_mutex_unlock(&g_prs_mu);
            why = 0;
        } else { rs->bytes = nb; if (tm) __atomic_fetch_add(&g_pool_ns[0], now_ns() - t0, __ATOMIC_RELAXED); }
    }
    if (!rs) {
        if (why < 0) why = 0;
        __atomic_fetch_add(&g_prs_why[why], 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_pool_n[PN_RFALL], 1, __ATOMIC_RELAXED);
        return 0;
    }
    c->prs_use[c->prs_n].rs = rs; c->prs_use[c->prs_n].bin = b; c->prs_use[c->prs_n].patch = NULL; c->prs_use[c->prs_n].npatch = 0;
    *uidx = c->prs_n++; *out = rs->set; *valid = hit;
    __atomic_fetch_add(&g_pool_n[hit ? PN_RHIT : reserve ? PN_RNEW : PN_RMISS], 1, __ATOMIC_RELAXED);
    if (tm) __atomic_fetch_add(&g_pool_ns[2], now_ns() - t0, __ATOMIC_RELAXED);
    return 1;
}
// 整表写完才记「里面写着哪几张表」：写之前 / 写到一半失败都保持原键（或未知），空闲串里不会有「键对、内容错」的组。
static void prs_mark(struct nvn_gpu_cmd_s *c, uint32_t uidx, const struct pool_set_info *si) {
    c->prs_use[uidx].rs->k1 = si->k1; c->prs_use[uidx].rs->k2 = si->k2;
}
// 反馈拷贝在用时，这一笔整池（图像池）里要换成快照的项：按反馈记录的顺序、与老路径 feedback_view 同一口径（同一个视图出现在几处就换几处；同一项只认第一条记录）。
// 返回 0 = 要换的项太多（超过 PRS_PATCH_MAX），这一笔整个走老路径。
static int prs_plan(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute, uint32_t mask, struct prs_plan *pl) {
    pl->n = 0;
    uint32_t nseen = 0; struct { uint32_t set, binding; } seen[24];   // 已经算过的（组, 绑定）：同一池绑定被几个阶段共用只算一次
    for (int st = compute ? NVN_GPU_SH_CS : 0; st < (compute ? NVN_GPU_SH_CS + 1 : 5); st++) {
        for (uint32_t i = 0; i < 64; i++) {
            nvn_gpu_pool_table t = c->pool_binds[st][i];
            if (!t || t->kind != NVN_GPU_POOL_IMAGE || i >= p->stage_nent[st]) continue;
            const nvn_gpu_layout_ent *e = &p->stage_ents[st][i];
            if (!(mask & (1u << e->set))) continue;
            uint32_t q = 0; while (q < nseen && !(seen[q].set == e->set && seen[q].binding == e->binding)) q++;
            if (q < nseen) continue;
            if (nseen == 24) { __atomic_fetch_add(&g_prs_why[3], 1, __ATOMIC_RELAXED); return 0; }
            seen[nseen].set = e->set; seen[nseen].binding = e->binding; nseen++;
            for (const struct feedback_copy *f = c->feedback; f != c->feedback_stop; f = f->next) {
                for (uint32_t k = 0; k <= t->count; k++) {
                    if (t->objects[k] != (void *)f->source) continue;
                    uint32_t d = 0; while (d < pl->n && !(pl->e[d].set == e->set && pl->e[d].binding == e->binding && pl->e[d].idx == k)) d++;
                    if (d < pl->n) continue;   // 更靠前的记录已经认领了这一项
                    if (pl->n == PRS_PATCH_MAX) { __atomic_fetch_add(&g_prs_why[3], 1, __ATOMIC_RELAXED); return 0; }
                    pl->e[pl->n].set = e->set; pl->e[pl->n].binding = e->binding; pl->e[pl->n].idx = k;
                    pl->e[pl->n].view = f->view; pl->e[pl->n].canon = t->images[k];
                    pl->n++;
                }
            }
        }
    }
    return 1;
}
// 把计划里属于回收组的补丁写进去（正典内容已经就绪），并记在本段取用记录名下（段完成时写回）。0 = 内存不够（置粘滞错误，这一段不会提交）。
static int prs_apply(struct nvn_gpu_cmd_s *c, const struct prs_plan *pl, uint32_t rec_mask, const uint32_t *uidx) {
    for (uint32_t set = 0; set < 4; set++) {
        if (!(rec_mask >> set & 1)) continue;
        uint32_t n = 0; for (uint32_t k = 0; k < pl->n; k++) if (pl->e[k].set == set) n++;
        if (!n) continue;
        struct prs_use *u = &c->prs_use[uidx[set]];
        struct pool_patch *pp = malloc((size_t)n * sizeof *pp);
        if (!pp) { set_sticky(c, NVN_GPU_E_OOM, "pool patch allocation failed"); return 0; }
        VkWriteDescriptorSet w[PRS_PATCH_MAX]; VkDescriptorImageInfo ii[PRS_PATCH_MAX]; uint32_t m = 0;
        for (uint32_t k = 0; k < pl->n; k++) {
            if (pl->e[k].set != set) continue;
            pp[m].binding = pl->e[k].binding; pp[m].idx = pl->e[k].idx; pp[m].canon = pl->e[k].canon;
            ii[m] = pl->e[k].canon; ii[m].imageView = pl->e[k].view;
            memset(&w[m], 0, sizeof w[m]); w[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[m].dstSet = u->rs->set; w[m].dstBinding = pl->e[k].binding; w[m].dstArrayElement = pl->e[k].idx;
            w[m].descriptorCount = 1; w[m].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[m].pImageInfo = &ii[m];
            m++;
        }
        vkUpdateDescriptorSets(g.d, m, w, 0, NULL);
        u->patch = pp; u->npatch = m;
        __atomic_fetch_add(&g_pool_n[PN_RPATCH], m, __ATOMIC_RELAXED);
    }
    return 1;
}
// 段结束（finish_cmd 最先一步，放弃的段已先清空两级回放队列）：补丁按正典内容写回，再把组放回各自表项的空闲串；放不回（会话已收尾 / 空闲到上限 / 内存不够）的销毁。
static void prs_release(struct nvn_gpu_cmd_s *c, int executed) {
    (void)executed;
    if (c->prs_n) {
        const int tm = g_draw_tm; const uint64_t t0 = tm ? now_ns() : 0;
        for (uint32_t i = 0; i < c->prs_n; i++) {
            struct prs_use *u = &c->prs_use[i];
            if (!u->npatch) { free(u->patch); u->patch = NULL; continue; }
            VkWriteDescriptorSet w[PRS_PATCH_MAX]; VkDescriptorImageInfo ii[PRS_PATCH_MAX];
            for (uint32_t k = 0; k < u->npatch; k++) {
                ii[k] = u->patch[k].canon;
                memset(&w[k], 0, sizeof w[k]); w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[k].dstSet = u->rs->set; w[k].dstBinding = u->patch[k].binding; w[k].dstArrayElement = u->patch[k].idx;
                w[k].descriptorCount = 1; w[k].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[k].pImageInfo = &ii[k];
            }
            vkUpdateDescriptorSets(g.d, u->npatch, w, 0, NULL);
            free(u->patch); u->patch = NULL; u->npatch = 0;
        }
        struct pool_rs *dead = NULL;
        pthread_mutex_lock(&g_prs_mu);
        for (uint32_t i = 0; i < c->prs_n; i++) {
            struct pool_rs *rs = c->prs_use[i].rs;
            if (!g_prs_closed && rs->bytes <= g_prs_idle_cap) {
                // 放回后空闲占用会超过空闲封顶：先淘汰空闲串里最久没用的组（刚用完的这一个最新，留下），再放回
                for (struct pool_rs *ev; g_prs_idle_bytes + rs->bytes > g_prs_idle_cap && (ev = prs_evict_oldest_locked()); ) { ev->next = dead; dead = ev; }
                if (prs_push(c->prs_use[i].bin, rs)) { rs->seq = ++g_prs_seq; g_prs_idle++; g_prs_idle_bytes += rs->bytes; continue; }
            }
            if (g_prs_total) g_prs_total--;
            g_prs_bytes = g_prs_bytes >= rs->bytes ? g_prs_bytes - rs->bytes : 0;
            rs->next = dead; dead = rs;
        }
        pthread_mutex_unlock(&g_prs_mu);
        prs_destroy_list(dead);
        if (tm) __atomic_fetch_add(&g_pool_ns[2], now_ns() - t0, __ATOMIC_RELAXED);
    }
    free(c->prs_use); c->prs_use = NULL; c->prs_n = c->prs_cap = 0;
}
static void prs_open(void) {
    pthread_mutex_lock(&g_prs_mu);
    g_prs_closed = 0; g_prs_total = g_prs_idle = 0; g_prs_bytes = g_prs_idle_bytes = 0; g_prs_seq = 0;
    int mb = nvn_plat_flag("nvk_pool_rec_mb", PRS_CAP_MB);   // 调试包压低封顶验淘汰；玩家包恒取缺省。不收 ≤ 0（没有「不封顶」）
    if (mb < 1 || mb > 65536) mb = PRS_CAP_MB;
    g_prs_cap = (uint64_t)mb << 20; g_prs_idle_cap = g_prs_cap;
    for (int i = 0; i < 4; i++) g_prs_why[i] = 0;
    pthread_mutex_unlock(&g_prs_mu);
}
static void prs_close(void) {   // 会话收尾：所有段都完成了、设备空闲；空闲组连池一起销毁
    pthread_mutex_lock(&g_prs_mu);
    g_prs_closed = 1;
    for (struct pool_rbin *b = g_prs_bins, *nx; b; b = nx) {
        nx = b->next;
        for (uint32_t i = 0; i < b->nfree; i++) { vkDestroyDescriptorPool(g.d, b->free[i]->pool, NULL); free(b->free[i]); }
        free(b->free); free(b);
    }
    g_prs_bins = NULL; g_prs_total = g_prs_idle = 0; g_prs_bytes = g_prs_idle_bytes = 0;
    pthread_mutex_unlock(&g_prs_mu);
}

// canon：这些组（位）整池写正典内容，不换反馈快照（回收的组：快照另走补丁，见 prs_plan / prs_apply）。
static int pool_write_sets(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute, uint32_t dirty, uint32_t canon) {
    VkWriteDescriptorSet writes[6 * 64]; uint32_t n = 0; uint64_t elems = 0;   // elems：这一次整池写入的描述符项数（读数）
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
            if (t->kind == NVN_GPU_POOL_IMAGE && c->feedback != c->feedback_stop && !(canon & (1u << e->set))) {
                VkDescriptorImageInfo *images = malloc(((size_t)t->count + 1) * sizeof *images);
                if (!images) {
                    for (uint32_t k = 0; k < n; k++) free(copies[k]);
                    set_sticky(c, NVN_GPU_E_OOM, "feedback pool descriptors allocation failed"); return 0;
                }
                memcpy(images, t->images, ((size_t)t->count + 1) * sizeof *images);
                for (uint32_t k = 0; k <= t->count; k++) images[k].imageView = feedback_view(c, t->objects[k]);
                copies[n] = images; w.pImageInfo = images;
            }
            elems += w.descriptorCount;
            writes[n++] = w;
        }
    }
    if (n) {
        const int tm = g_draw_tm; const uint64_t t0 = tm ? now_ns() : 0;
        vkUpdateDescriptorSets(g.d, n, writes, 0, NULL);
        if (tm) __atomic_fetch_add(&g_pool_ns[1], now_ns() - t0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_pool_n[PN_ELEMS], elems, __ATOMIC_RELAXED);
    }
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
// 用例读整池组读数（只增不清零，用例自取前后差）：0 要的组数 1 段内备忘命中 2 新建 3 因反馈拷贝在用而绕开备忘 4 整池写入的描述符项数
// 5 回收取到键相同的空闲组 6 取到键不同的 7 向驱动新建 8 没走回收（退回老路径）9 反馈补丁写入的项数。
uint64_t nvn_gpu_vk_test_pool_sets(int which) { return which >= 0 && which < PN_N ? __atomic_load_n(&g_pool_n[which], __ATOMIC_RELAXED) : 0; }
void nvn_gpu_vk_test_pool_memo_refresh(void) { g_pmemo_cur = -1; }   // 用例改了 nvk_pool_memo / nvk_pool_rec 的读值后，让后端下一笔绑定就重读，不等 256 次
// 整池组回收读数：0 现存组（在用 + 空闲）1 其中空闲 2 没走回收：碰锁或内存不够 3 写法不同 4 到上限 5 补丁太多
// 6 现存组的估算字节 7 其中空闲的估算字节 8 按字节封顶淘汰的空闲组（累计，用例自取前后差）9 每个描述符项估多少字节（常量）
// 10 当前总量封顶（字节）11 当前空闲封顶（字节）。
uint64_t nvn_gpu_vk_test_prs(int which) {
    uint64_t v = 0;
    pthread_mutex_lock(&g_prs_mu);
    if (which == 0) v = g_prs_total; else if (which == 1) v = g_prs_idle;
    else if (which >= 2 && which <= 5) v = __atomic_load_n(&g_prs_why[which - 2], __ATOMIC_RELAXED);
    else if (which == 6) v = g_prs_bytes; else if (which == 7) v = g_prs_idle_bytes;
    else if (which == 8) v = __atomic_load_n(&g_prs_evict, __ATOMIC_RELAXED);
    else if (which == 9) v = PRS_DESC_BYTES;
    else if (which == 10) v = g_prs_cap; else if (which == 11) v = g_prs_idle_cap;
    pthread_mutex_unlock(&g_prs_mu);
    return v;
}
// 销毁全部空闲的回收组（用例要一张干净的空闲串：设备空闲、没有在途段时调）。
void nvn_gpu_vk_test_prs_trim(void) {
    pthread_mutex_lock(&g_prs_mu);
    for (struct pool_rbin *b = g_prs_bins; b; b = b->next) {
        for (uint32_t i = 0; i < b->nfree; i++) {
            vkDestroyDescriptorPool(g.d, b->free[i]->pool, NULL);
            if (g_prs_total) g_prs_total--;
            g_prs_bytes = g_prs_bytes >= b->free[i]->bytes ? g_prs_bytes - b->free[i]->bytes : 0;
            free(b->free[i]);
        }
        b->nfree = 0;
    }
    g_prs_idle = 0; g_prs_idle_bytes = 0;
    pthread_mutex_unlock(&g_prs_mu);
}
// 压低 / 还原回收的封顶（cap = 0 还原缺省）：cap = 总量封顶字节，keep = 空闲封顶字节；验「放不下先淘汰最久没用的空闲组」「淘汰完还放不下退回老路径」「空闲超过空闲封顶就淘汰」。
void nvn_gpu_vk_test_prs_limit(uint64_t cap, uint64_t keep) {
    pthread_mutex_lock(&g_prs_mu);
    g_prs_cap = cap ? cap : (uint64_t)PRS_CAP_MB << 20; g_prs_idle_cap = cap ? keep : (uint64_t)PRS_CAP_MB << 20;
    pthread_mutex_unlock(&g_prs_mu);
}
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
