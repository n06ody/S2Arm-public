// 调试探针：沿用通道时间戳，按队列里的呈现边界截取 24 帧。采完再输出，避免在取样期间刷日志。
static void pass_capture_frame(struct nvn_gpu_vk *vk, const struct vk_segment *s, int failed) {
    if (vk->pass_capture_delay <= 0) return;
    uint64_t now = now_ns();
    if (!vk->pass_capture) {
        vk->pass_capture = calloc(1, sizeof *vk->pass_capture);
        if (!vk->pass_capture) {
            nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-capture", "采样缓冲分配失败");
            return;
        }
        vk->pass_capture->deadline = now + (uint64_t)vk->pass_capture_delay * 1000000000ull;
        char line[128];
        snprintf(line, sizeof line, "armed first_present_ns=%llu planned_ns=%llu",
                 (unsigned long long)now, (unsigned long long)vk->pass_capture->deadline);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-capture", line);
    }
    struct pass_capture *p = vk->pass_capture;
    if (p->frames == PASS_CAPTURE_FRAMES) return;
    if (now + 2000000000ull >= p->deadline)
        atomic_store_explicit(&vk->pass_capture_record, 1, memory_order_relaxed);
    if (now < p->deadline) return;
    uint32_t f = p->frames;
    if (f == 0 && !p->frame[0].seq) {
        p->frame[0].seq = s->wsi_seq + 1;
        p->frame[0].begin = now;
        p->failed = 0;
        atomic_store_explicit(&vk->pass_capture_replace, vk->pass_capture_blank, memory_order_relaxed);
        return;
    }
    p->frame[f].failed = p->failed || failed || !p->frame[f].count || p->frame[f].seq != s->wsi_seq;
    p->frame[f].end = now;
    p->frames++;
    p->failed = 0;
    if (p->frames < PASS_CAPTURE_FRAMES) {
        p->frame[p->frames].seq = s->wsi_seq + 1;
        p->frame[p->frames].begin = now;
        p->frame[p->frames].first = p->rows;
        return;
    }
    atomic_store_explicit(&vk->pass_capture_record, 0, memory_order_relaxed);
    atomic_store_explicit(&vk->pass_capture_replace, 0, memory_order_relaxed);
    for (uint32_t j = 0; j < p->frames; j++) {
        char line[2048];
        const uint64_t seq = p->frame[j].seq;
        const uint32_t n = p->frame[j].count;
        snprintf(line, sizeof line, "frame seq=%llu begin_ns=%llu end_ns=%llu rows=%u incomplete=%u period_ns=%.9g",
                 (unsigned long long)seq, (unsigned long long)p->frame[j].begin,
                 (unsigned long long)p->frame[j].end, n, p->frame[j].failed, p->period);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-capture", line);
        for (uint32_t i = 0; i < n; i++) {
            const struct pass_capture_row *r = &p->row[p->frame[j].first + i];
            size_t o = (size_t)snprintf(line, sizeof line,
                     "pass seq=%llu i=%u ns=%llu fb=%ux%ux%u area=%d,%d,%u,%u n=%u draw=%u clear=%u partial=%u aux=%u reopen=%u prim=%u count=%u inst=%u ind=%u idx=%u pipe=%x why=%s sc=%d,%d,%d,%d vp=%.1f,%.1f,%.1f,%.1f tick0=%llu tick1=%llu submit=%llu record=%llu substituted=%u",
                     (unsigned long long)seq, i, (unsigned long long)r->ns, r->w, r->h, r->layers,
                     r->x, r->y, r->rw, r->rh, r->n, r->draws, r->clears, r->partial_clears,
                     r->aux_draws, r->reopen, r->primitive, r->count, r->instances, r->indirect, r->indexed, r->pipe,
                     r->reason ? r->reason : "normal", r->sc[0], r->sc[1], r->sc[2], r->sc[3],
                     r->vp[0], r->vp[1], r->vp[2], r->vp[3],
                     (unsigned long long)r->tick0, (unsigned long long)r->tick1, (unsigned long long)r->submit,
                     (unsigned long long)r->record, r->substituted);
            for (uint32_t k = 0; k < r->n && o < sizeof line; k++) {
                const struct pass_capture_att *a = &r->att[k];
                o += (size_t)snprintf(line + o, sizeof line - o, " a=%u,%u,%u,%ux%u,%u,%u,%u,%u,%u,%u,%llu",
                     a->slot, a->format, a->samples, a->w, a->h, a->mip, a->layer,
                     a->load, a->store, a->stencil_load, a->stencil_store, (unsigned long long)a->tex);
            }
            nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-capture", line);
            if (!r->fs_sha[0]) continue;
            snprintf(line, sizeof line, "fs seq=%llu i=%u key=%016llx sha=%s entry=%s flags=%u blend=%x mask=%x bindings=%u omitted=%u",
                     (unsigned long long)seq, i, (unsigned long long)r->fs_key, r->fs_sha, r->fs_entry,
                     r->fs_flags, r->fs_blend, r->fs_writemask, r->nbind, r->lost_bind);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-capture", line);
            for (uint32_t k = 0; k < r->nbind; k++) {
                const struct pass_capture_binding *b = &r->binds[k];
                snprintf(line, sizeof line,
                         "bind seq=%llu i=%u set=%u binding=%u elem=%u cls=%u tex=%llu last_rt=%llu fmt=%u viewfmt=%u nvnfmt=%u sz=%ux%u level=%u levels=%u layer=%u layers=%u bc=%u feedback=%u pool=%u samp=%u min=%u mag=%u mip=%u lod_min=%g lod_max=%g lod_bias=%g aniso=%g",
                         (unsigned long long)seq, i, b->set, b->binding, b->elem, b->cls,
                         (unsigned long long)b->tex, (unsigned long long)b->last_rt,
                         b->fmt, b->viewfmt, b->nvnfmt, b->w, b->h, b->mip, b->levels, b->base_layer, b->layers,
                         b->bc_decoded, b->feedback, b->pool_count, b->has_sampler, b->min_filter, b->mag_filter, b->mip_filter,
                         b->lod_min, b->lod_max, b->lod_bias, b->anisotropy);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-capture", line);
            }
        }
    }
    if (vk->pass_draw_export) pass_draw_write(p);
    char line[96];
    snprintf(line, sizeof line, "end frames=%u rows=%u", p->frames, p->rows);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-capture", line);
}

static void pass_capture_segment(struct pass_capture *p, const struct nvn_gpu_cmd_s *c,
                                 const uint64_t *pts, double period, uint64_t signal) {
    if (!p || p->frames == PASS_CAPTURE_FRAMES || !p->frame[p->frames].seq) return;
    if (!c || !c->tsq || c->pass_capture_failed || (c->ntsp && (!c->pass_capture || !pts))) {
        p->failed = 1;
        return;
    }
    if (g.pass_draw_export && c->draw_failed) p->draw_failed = 1;
    for (uint32_t i = 0; i < c->ntsp; i++) {
        if (p->frame[p->frames].count == PASS_CAPTURE_MAX || pts[2 * i + 1] < pts[2 * i]) {
            p->failed = 1;
            return;
        }
        struct pass_capture_row *r = &p->row[p->rows++];
        *r = c->pass_capture[i];
        if (g.pass_draw_export) pass_draw_retire(p, c, r);
        r->tick0 = pts[2 * i];
        r->tick1 = pts[2 * i + 1];
        r->submit = signal;
        p->period = period;
        r->ns = (uint64_t)((double)(pts[2 * i + 1] - pts[2 * i]) * period);
        p->frame[p->frames].count++;
    }
}
