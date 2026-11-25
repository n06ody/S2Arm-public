// SPDX-License-Identifier: Apache-2.0
// 默认关的缓冲回写来源账。调用者持 g_bmu；不取游戏内存、不改登记/合并条件。
// 五秒窗按事件发生时记：raw 在登记成功时记，merged/copied/discarded 在一条最终区间撤账时记。
// 上个窗口未完成的登记可能在本窗落地。settled 是本窗最终区间所含的原始登记，可跨窗核账。
enum { ORIGIN_MIXED = RP_W_COUNT, ORIGIN_ALL };
static const char *const origin_names[] = {"unknown", "ssbo", "ubo_ptr", "image_tbuf", "update", "copy", "fill", "t2b", "mixed", "all"};
_Static_assert(sizeof origin_names / sizeof *origin_names == ORIGIN_ALL + 1, "write sources");
struct origin_pair { uint64_t n, bytes; };
struct origin_counts { struct origin_pair raw, merged, copied, discarded, settled; };
static struct origin_counts origin_window[ORIGIN_ALL + 1], origin_capture;
static struct origin_pair origin_pending;
static int origin_enabled = -1, origin_frame = -1, origin_frame_done;
static uint64_t origin_next_pool, origin_cross_frame, origin_selected_closed;

static void origin_add(struct origin_pair *p, uint64_t n, uint64_t bytes) {
    p->n += n;
    p->bytes += bytes;
}
static int origin_on(void) {
    if (origin_enabled < 0) {
        origin_enabled = nvn_plat_flag("nvk_land_origin", 0) != 0;
        if (origin_enabled) origin_frame = nvn_plat_flag("nvk_land_origin_frame", -1);
    }
    return origin_enabled;
}
static int origin_shader(enum rp_write_op op) {
    return op == RP_W_SSBO || op == RP_W_UBOP || op == RP_W_IMAGE_TBUF;
}
static struct write_origin origin_raw(struct rp_write_source src, uint64_t len) {
    const int shader = origin_shader(src.op);
    const int capture = origin_frame >= 0 && !origin_frame_done && rp_frame() == (uint64_t)origin_frame;
    return (struct write_origin){.n = 1, .bytes = len, .selected = capture,
        .ops = 1u << src.op, .stages = shader ? 1u << src.stage : 0, .slots = shader ? 1u << src.slot : 0};
}
static void origin_union(struct write_origin *dst, const struct write_origin *src) {
    dst->n += src->n; dst->bytes += src->bytes; dst->selected += src->selected;
    dst->ops |= src->ops; dst->stages |= src->stages; dst->slots |= src->slots;
}
static void origin_accept(struct shadow *s, uint64_t id, uint64_t sub, uint64_t off, uint64_t len,
                          struct rp_write_source src, int selected) {
    if (!s->origin_pool) s->origin_pool = ++origin_next_pool;
    origin_add(&origin_window[src.op].raw, 1, len);
    origin_add(&origin_window[ORIGIN_ALL].raw, 1, len);
    origin_add(&origin_pending, 1, len);
    if (selected) {
        origin_add(&origin_capture.raw, 1, len);
        int shader = origin_shader(src.op);
        char line[256];
        snprintf(line, sizeof line, "raw frame=%d pool=%" PRIu64 " write=%" PRIu64 " sub=%" PRIu64
                 " off=%" PRIu64 " bytes=%" PRIu64 " op=%s stage=%d slot=%d", origin_frame, s->origin_pool,
                 id, sub, off, len, origin_names[src.op], shader ? src.stage : -1, shader ? src.slot : -1);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "land-origin", line);
    }
}
static void origin_absorb(uint64_t pool, uint64_t from, uint64_t into) {
    char line[160];
    snprintf(line, sizeof line, "absorb frame=%d pool=%" PRIu64 " from=%" PRIu64 " into=%" PRIu64,
             origin_frame, pool, from, into);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-origin", line);
}
static void origin_close_counts(struct origin_counts *c, const struct gpu_write *g, int copied) {
    origin_add(&c->settled, g->origin.n, g->origin.bytes);
    origin_add(&c->merged, 1, g->len);
    origin_add(copied ? &c->copied : &c->discarded, 1, g->len);
}
static int origin_multiple(unsigned mask) { return (mask & (mask - 1)) != 0; }
static void origin_close(const struct shadow *s, const struct gpu_write *g, int copied) {
    const struct write_origin *w = &g->origin;
    if (!w->n) return;
    int group = ORIGIN_MIXED;
    if (!origin_multiple(w->ops) && !origin_multiple(w->stages) && !origin_multiple(w->slots))
        group = __builtin_ctz(w->ops);
    origin_close_counts(&origin_window[group], g, copied);
    origin_close_counts(&origin_window[ORIGIN_ALL], g, copied);
    origin_pending.n -= w->n; origin_pending.bytes -= w->bytes;
    if (w->selected) {
        origin_selected_closed += w->selected;
        if (w->selected != w->n) origin_cross_frame++;
        origin_close_counts(&origin_capture, g, copied);
        char line[352];
        snprintf(line, sizeof line, "segment frame=%d pool=%" PRIu64 " write=%" PRIu64 " sub=%" PRIu64
                 " off=%" PRIu64 " bytes=%" PRIu64 " copied=%" PRIu64 " source=%s ops=%x stages=%x slots=%x"
                 " raw=%" PRIu64 "/%" PRIu64 " selected=%" PRIu64, origin_frame, s->origin_pool, g->id, g->sub,
                 g->off, g->len, copied ? g->len : UINT64_C(0), origin_names[group], w->ops, w->stages, w->slots,
                 w->n, w->bytes, w->selected);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "land-origin", line);
    }
}
static void origin_print(const char *scope, const char *name, const struct origin_counts *c) {
    char line[448];
    snprintf(line, sizeof line, "%s source=%s raw=%" PRIu64 "/%" PRIu64 " merged=%" PRIu64 "/%" PRIu64
             " copied=%" PRIu64 "/%" PRIu64 " discarded=%" PRIu64 "/%" PRIu64 " settled=%" PRIu64 "/%" PRIu64,
             scope, name, c->raw.n, c->raw.bytes, c->merged.n, c->merged.bytes, c->copied.n, c->copied.bytes,
             c->discarded.n, c->discarded.bytes, c->settled.n, c->settled.bytes);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-origin", line);
}
static void origin_report_locked(int final) {
    if (origin_enabled != 1) return;
    for (int i = 0; i <= ORIGIN_ALL; i++) {
        if (origin_window[i].raw.n || origin_window[i].merged.n || i == ORIGIN_ALL)
            origin_print("window", origin_names[i], &origin_window[i]);
    }
    memset(origin_window, 0, sizeof origin_window);
    char line[160];
    snprintf(line, sizeof line, "pending raw=%" PRIu64 "/%" PRIu64, origin_pending.n, origin_pending.bytes);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-origin", line);
    if (origin_frame >= 0 && !origin_frame_done && (final || rp_frame() > (uint64_t)origin_frame) &&
        (final || origin_selected_closed == origin_capture.raw.n)) {
        const char *status = !origin_capture.raw.n ? "no_samples" : origin_cross_frame ? "cross_frame" :
            rp_frame() <= (uint64_t)origin_frame || origin_selected_closed != origin_capture.raw.n ? "partial" : "complete";
        snprintf(line, sizeof line, "frame=%d closed=%" PRIu64 " cross_frame=%" PRIu64 " status=%s", origin_frame,
                 origin_selected_closed, origin_cross_frame, status);
        origin_print(line, "frame", &origin_capture);
        origin_frame_done = 1;
    }
}
void rp_land_origin_report(void) {
    pthread_mutex_lock(&g_bmu);
    origin_report_locked(0);
    pthread_mutex_unlock(&g_bmu);
}
