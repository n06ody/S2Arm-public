// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

enum account_bc_reason {
    ACCOUNT_BC_FORMAT, ACCOUNT_BC_OFF, ACCOUNT_BC_CAPS, ACCOUNT_BC_NO_SNAPSHOT, ACCOUNT_BC_SOURCE,
    ACCOUNT_BC_PUNCH, ACCOUNT_BC_ADMISSION, ACCOUNT_BC_PLAIN, ACCOUNT_BC_ASTC, ACCOUNT_BC_REASONS,
};

// 只计已接受的命令段。整图可能写入（包括清除、丢弃和 shader storage 写）推进版本；
// 不逐像素判断覆盖，也不把两个视图的不同 mip / 层当成互不影响。
enum { ACCOUNT_EVENTS_MAX = 32768, ACCOUNT_SOURCE_LINES = 128 };
struct account_event {
    struct nvn_gpu_tex_s *tex;       // 写目标或快照源，原命令已保活
    struct nvn_gpu_view_s *source;   // NULL 记作写入
    uint32_t pipeline;
    uint8_t draw_writes, pool;
};

static void account_record(struct nvn_gpu_cmd_s *c, const struct account_event *event) {
    if (c->account_gap) return;
    if (c->account_n == c->account_cap) {
        if (c->account_cap == ACCOUNT_EVENTS_MAX) { c->account_gap = 1; return; }
        uint32_t cap = c->account_cap ? c->account_cap * 2 : 64;
        struct account_event *events = realloc(c->account_events, cap * sizeof *events);
        if (!events) { c->account_gap = 1; return; }
        c->account_events = events;
        c->account_cap = cap;
    }
    c->account_events[c->account_n++] = *event;
}

static void account_write(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    if (!g.accounts_on || !t ||
        !(t->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return;
    const struct account_event e = {.tex = t};
    account_record(c, &e);
}

static void account_attachment_writes(struct nvn_gpu_cmd_s *c, uint16_t mask) {
    const nvn_gpu_pass_desc *d = &c->pass_desc;
    for (uint32_t i = 0; i < c->pass_ncolor; i++)
        if ((mask & (1u << i)) && d->color[i].view) account_write(c, d->color[i].view->tex);
    if (mask & (1u << 8)) {
        if (d->depth.view) account_write(c, d->depth.view->tex);
        if (d->stencil.view && (!d->depth.view || d->stencil.view->tex != d->depth.view->tex))
            account_write(c, d->stencil.view->tex);
    }
}

static void account_storage_writes(struct nvn_gpu_cmd_s *c, unsigned start, unsigned end) {
    for (unsigned st = start; st < end; st++) {
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if (b->cls != NVN_GPU_RES_IMAGE || !b->u.view || b->ent >= c->pipe->stage_nent[st]) continue;
            const nvn_gpu_layout_ent *e = &c->pipe->stage_ents[st][b->ent];
            if (e->cls == b->cls && (!e->access || (e->access & NVN_GPU_ACC_W))) account_write(c, b->u.view->tex);
        }
    }
}

static uint64_t access_id(uint64_t *slot);

// 在 g_submit_mu 下按 vkQueueSubmit 成功次序回放。丢事件的段推进全局缺口代，后一笔
// 观察到同一源时报告 unknown，再以此笔为新基点。该版本只供诊断，不能用来决定免拷。
static void account_commit(const struct nvn_gpu_cmd_s *c) {
    if (c->account_gap) {
        g.account_gap++;
        return;
    }
    static const char *const states[] = {"first", "0", "1", "unknown"};
    for (uint32_t i = 0; i < c->account_n; i++) {
        const struct account_event *e = &c->account_events[i];
        struct nvn_gpu_tex_s *t = e->tex;
        if (!e->source) { t->account_write++; continue; }
        const unsigned state = !t->account_seen ? 0 : t->account_gap != g.account_gap ? 3 :
            t->account_last_write == t->account_write ? 2 : 1;
        t->account_last_write = t->account_write;
        t->account_gap = g.account_gap;
        g.account_versions[state]++;
        const unsigned bit = 1u << (state * 2 + e->draw_writes);
        const int repeat = (t->account_seen & bit) != 0;
        t->account_seen |= bit;
        if (repeat || g.account_source_lines >= ACCOUNT_SOURCE_LINES) continue;
        g.account_source_lines++;
        const struct nvn_gpu_view_s *v = e->source;
        char line[320];
        snprintf(line, sizeof line,
            "nvk-fb-source image=%llu nvn=%#x vk=%d view_vk=%d extent=%ux%ux%u level=%u+%u layer=%u+%u aspect=%#x version=%llu epoch=%llu unchanged=%s draw_writes=%u pool=%u pipe=%08x",
            (unsigned long long)access_id(&t->access_id), t->nvn_format, t->format, v->format,
            t->w, t->h, t->depth, v->base_level, v->levels, v->base_layer, v->layers, v->aspect,
            (unsigned long long)t->account_write, (unsigned long long)g.account_gap, states[state],
            (unsigned)e->draw_writes, (unsigned)e->pool, e->pipeline);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
}

// 口径是相邻成功 WSI present 之间已接收的快照数。首个桶包括首呈现之前的提交。
static void account_present(void) {
    const uint64_t copies = g.fb_copy_counts.snapshots - g.account_at_present;
    g.account_at_present = g.fb_copy_counts.snapshots;
    g.account_hist[copies > 1 ? 2 : copies]++;
}

static void account_fb_log(const char *scope) {
    char line[320];
    snprintf(line, sizeof line,
        "nvk-fb-present scope=%s frames=%llu zero=%llu one=%llu multi=%llu copies_pending=%llu first=%llu changed=%llu unchanged=%llu unknown=%llu lost_segments=%llu source_lines=%u",
        scope, (unsigned long long)g.fb_copy_frames,
        (unsigned long long)g.account_hist[0], (unsigned long long)g.account_hist[1],
        (unsigned long long)g.account_hist[2], (unsigned long long)(g.fb_copy_counts.snapshots - g.account_at_present),
        (unsigned long long)g.account_versions[0], (unsigned long long)g.account_versions[1],
        (unsigned long long)g.account_versions[2], (unsigned long long)g.account_versions[3],
        (unsigned long long)g.account_gap, g.account_source_lines);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
}

static void account_bc_reason(struct nvn_gpu_tex_s *t, enum account_bc_reason reason) {
    if (g.accounts_on && g.bc1_astc_on && !g.dev->core_features.textureCompressionBC &&
        g.dev->core_features.textureCompressionASTC_LDR)
        __atomic_store_n(&t->account_bc_reason, reason, __ATOMIC_RELAXED);
}

static void account_bc_log(const char *scope, uint64_t frames) {
    static const char *const reasons[] = {
        "format", "off", "caps", "no_snapshot", "source", "punch_through", "astc_admission", "plain_required", "astc",
    };
    // 16 客体格式。每种在同一会话最多有普通解码和 ASTC 两套宿主格式。
    struct bc_bucket { uint64_t textures, bytes, shadow; VkFormat vk; }
        buckets[16][ACCOUNT_BC_REASONS][2] = {0};
    pthread_mutex_lock(&g_mu);
    for (const struct nvn_gpu_tex_s *t = g.texs; t; t = t->all_next) {
        if (!t->bc_decoded) continue;
        const unsigned fmt = t->nvn_format - 0x41;
        const unsigned reason = __atomic_load_n(&t->account_bc_reason, __ATOMIC_RELAXED);
        struct bc_bucket *b = buckets[fmt][reason];
        if (b[0].textures && b[0].vk != t->format) b++;
        b->vk = t->format;
        b->textures++;
        b->bytes += t->mem_size;
        b->shadow += t->bc_shadow_mem_size;
    }
    pthread_mutex_unlock(&g_mu);
    for (unsigned i = 0; i < 16; i++) for (unsigned j = 0; j < ACCOUNT_BC_REASONS; j++) {
        for (unsigned k = 0; k < 2; k++) {
            const struct bc_bucket *b = &buckets[i][j][k];
            if (!b->textures) continue;
            char line[224];
            snprintf(line, sizeof line, "nvk-bc-live scope=%s frames=%llu nvn=%#x vk=%d reason=%s textures=%llu image_bytes=%llu shadow_bytes=%llu",
                     scope, (unsigned long long)frames, i + 0x41, b->vk, reasons[j],
                     (unsigned long long)b->textures, (unsigned long long)b->bytes, (unsigned long long)b->shadow);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
        }
    }
}
