// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// 只观察调用现场已有的判定与代号。采集复用 g_rmu；每 300 帧交换账本，锁外格式化。
enum tex_upload_reason {
    UP_CLEAN, UP_STORAGE_INIT, UP_NOT_UPLOADED, UP_RETRY, UP_UNARMED,
    UP_DIRTY_LOG, UP_DIRTY_SCAN, UP_NO_SPAN, UP_SPARSE, UP_REASON_N
};

enum { UP_ROWS = 512, UP_EMIT = 32, UP_HISTORY = 4096 };
static _Atomic int g_upload_ledger_on = -1;
static uint64_t g_upload_event;
struct upload_history {
    uint64_t serial, clear_seq, cpu_seq, alias_seq, alias_uid;
    enum nvn_tex_cpu_source cpu_source;
};
static struct upload_history g_upload_history[UP_HISTORY];
struct upload_input {
    uint64_t uid, stamp, epoch, last_epoch, map, last_map, sparse_gen, last_sparse_gen;
    int was_uploaded, unreliable;
    enum tex_upload_reason reason;
};
struct upload_row {
    uint64_t uid, serial, previous_uid, attempts, bytes, recorded, recorded_bytes, repeat, deferred;
    uint64_t why[UP_REASON_N];
    uint32_t width, height, depth, format;
    struct upload_input last;
    struct upload_history history;
    uint64_t stamp_after, gpu_write, seg, unreliable;
};
struct upload_window {
    struct upload_row rows[UP_ROWS];
    uint64_t attempts, bytes;
};
static struct upload_window g_upload_windows[2];
static struct upload_window *g_upload_window = &g_upload_windows[0];
static pthread_mutex_t g_upload_dump_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_upload_frame, g_upload_window_seq;

// 持 g_rmu。开关在本局第一次相关操作读一次，避免逐次读系统属性。
static int upload_ledger_enabled(void) {
    int on = atomic_load_explicit(&g_upload_ledger_on, memory_order_relaxed);
    if (on < 0) {
        on = nvn_plat_flag("nvk_tex_upload_ledger", 0) != 0;
        atomic_store_explicit(&g_upload_ledger_on, on, memory_order_relaxed);
    }
    return on;
}

static struct upload_history *upload_history_for(uint64_t serial) {
    return &g_upload_history[serial % UP_HISTORY];
}

static struct upload_history *upload_history_note(uint64_t serial) {
    struct upload_history *h = upload_history_for(serial);
    if (h->serial != serial) *h = (struct upload_history){.serial = serial};
    return h;
}

// 只记录通过回放参数检查的 ClearTexture；GPU 归属代际仍取原路径的 wrote。
void rp_tex_upload_note_clear(const tex_t *t) {
    if (!t->serial || atomic_load_explicit(&g_upload_ledger_on, memory_order_relaxed) == 0) return;
    pthread_mutex_lock(&g_rmu);
    if (upload_ledger_enabled()) upload_history_note(t->serial)->clear_seq = ++g_upload_event;
    pthread_mutex_unlock(&g_rmu);
}

static struct upload_input upload_ledger_input(const struct rp_tex *x, enum tex_upload_reason why,
                                               uint64_t epoch, uint64_t map, uint64_t sparse_gen) {
    return (struct upload_input) {
        .uid = x->uid, .stamp = x->stamp, .epoch = epoch, .last_epoch = x->check_epoch,
        .map = map, .last_map = x->check_map,
        .sparse_gen = sparse_gen, .last_sparse_gen = x->vgen,
        .was_uploaded = x->ever_uploaded, .reason = why,
        .unreliable = why == UP_DIRTY_SCAN && nvn_dirty_tracking_unreliable(),
    };
}

static void upload_ledger_note(const tex_t *t, const struct rp_tex *x, struct upload_input in, int result) {
    struct upload_window *w = g_upload_window;
    w->attempts++;
    w->bytes += t->L.size;
    struct upload_row *r = NULL;
    for (unsigned i = 0; i < UP_ROWS; i++) {
        struct upload_row *slot = &w->rows[(x->uid + i) % UP_ROWS];
        if (!slot->uid || slot->uid == x->uid) { r = slot; break; }
    }
    if (!r) return;   // 汇总仍保留这笔，遗漏字节与次数在窗口末尾报出
    r->uid = x->uid;
    r->serial = t->serial;
    if (in.uid != x->uid) r->previous_uid = in.uid;   // 这一笔换成了新的物理存储，例如 BC1 转码
    r->width = t->d.width;
    r->height = t->d.height;
    r->depth = t->d.depth;
    r->format = t->d.format;
    r->attempts++;
    r->bytes += t->L.size;
    r->why[in.reason]++;
    r->unreliable += in.unreliable;
    if (!result) { r->recorded++; r->recorded_bytes += t->L.size; r->repeat += in.was_uploaded; }
    if (result == -2) r->deferred++;
    const struct upload_history *h = upload_history_for(t->serial);
    r->history = h->serial == t->serial ? *h : (struct upload_history){0};
    r->last = in;
    r->stamp_after = x->stamp;
    r->gpu_write = x->wrote;
    r->seg = rp_seg_serial();
}

static int upload_row_compare(const void *a, const void *b) {
    const struct upload_row *x = a, *y = b;
    if (x->bytes != y->bytes) return x->bytes > y->bytes ? -1 : 1;
    return (x->uid > y->uid) - (x->uid < y->uid);
}

static const char *upload_cpu_source(enum nvn_tex_cpu_source source) {
    switch (source) {
        case NVN_TEX_CPU_WRITE_TEXELS: return "WriteTexels";
        case NVN_TEX_CPU_FLUSH: return "FlushMappedRange";
        case NVN_TEX_CPU_POLL: return "ft_poll";
        case NVN_TEX_CPU_FIRST_USE: return "ft_written";
        case NVN_TEX_CPU_OTHER: return "other_cpu_mark";
        default: return "unseen";
    }
}

void rp_tex_upload_ledger_flush(uint64_t frame, int final) {
    if (atomic_load_explicit(&g_upload_ledger_on, memory_order_relaxed) != 1) return;
    pthread_mutex_lock(&g_upload_dump_mu);
    pthread_mutex_lock(&g_rmu);
    struct upload_window *w = g_upload_window;
    g_upload_window = w == &g_upload_windows[0] ? &g_upload_windows[1] : &g_upload_windows[0];
    pthread_mutex_unlock(&g_rmu);

    if (w->attempts || final) {
        uint64_t seq = ++g_upload_window_seq, shown_attempts = 0, shown_bytes = 0;
        unsigned rows = 0;
        char line[1536];
        if (w->attempts) qsort(w->rows, UP_ROWS, sizeof w->rows[0], upload_row_compare);
        for (; rows < UP_EMIT && w->rows[rows].uid; rows++) {
            const struct upload_row *r = &w->rows[rows];
            const struct upload_input *in = &r->last;
            const struct upload_history *h = &r->history;
            snprintf(line, sizeof line,
                "window=%llu uid=%llu ser=%llu shape=%ux%ux%u format=%#x attempts=%llu bytes=%llu "
                "recorded=%llu repeat=%llu recorded_bytes=%llu deferred=%llu storage_init=%llu not_uploaded=%llu "
                "retry=%llu unarmed=%llu dirty_log=%llu dirty_scan=%llu no_span=%llu sparse=%llu unreliable_sample=%llu "
                "last_seg=%llu stamp=%llu->%llu epoch=%llu->%llu map=%llu->%llu sparse_gen=%llu->%llu gpu_write_sub1=%llu "
                "clear_replay_seq=%llu cpu_seq=%llu cpu_source=%s alias_evict_seq=%llu alias_evict_uid=%llu replaced_uid=%llu",
                (unsigned long long)seq, (unsigned long long)r->uid, (unsigned long long)r->serial,
                r->width, r->height, r->depth, r->format, (unsigned long long)r->attempts, (unsigned long long)r->bytes,
                (unsigned long long)r->recorded, (unsigned long long)r->repeat, (unsigned long long)r->recorded_bytes,
                (unsigned long long)r->deferred, (unsigned long long)r->why[UP_STORAGE_INIT], (unsigned long long)r->why[UP_NOT_UPLOADED],
                (unsigned long long)r->why[UP_RETRY], (unsigned long long)r->why[UP_UNARMED],
                (unsigned long long)r->why[UP_DIRTY_LOG], (unsigned long long)r->why[UP_DIRTY_SCAN],
                (unsigned long long)r->why[UP_NO_SPAN], (unsigned long long)r->why[UP_SPARSE], (unsigned long long)r->unreliable,
                (unsigned long long)r->seg, (unsigned long long)in->stamp, (unsigned long long)r->stamp_after,
                (unsigned long long)in->last_epoch, (unsigned long long)in->epoch,
                (unsigned long long)in->last_map, (unsigned long long)in->map,
                (unsigned long long)in->last_sparse_gen, (unsigned long long)in->sparse_gen, (unsigned long long)r->gpu_write,
                (unsigned long long)h->clear_seq, (unsigned long long)h->cpu_seq, upload_cpu_source(h->cpu_source),
                (unsigned long long)h->alias_seq, (unsigned long long)h->alias_uid, (unsigned long long)r->previous_uid);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "tex-upload-ledger", line);
            shown_attempts += r->attempts;
            shown_bytes += r->bytes;
        }
        snprintf(line, sizeof line,
            "window=%llu after_frame=%llu through_frame=%llu final=%d rows=%u attempts=%llu bytes=%llu omitted_attempts=%llu omitted_bytes=%llu",
            (unsigned long long)seq, (unsigned long long)g_upload_frame, (unsigned long long)frame, final, rows,
            (unsigned long long)w->attempts, (unsigned long long)w->bytes,
            (unsigned long long)(w->attempts - shown_attempts), (unsigned long long)(w->bytes - shown_bytes));
        nvn_plat_log(NVN_PLAT_LOG_INFO, "tex-upload-ledger-window", line);
        memset(w, 0, sizeof *w);
    }
    g_upload_frame = final ? 0 : frame;
    if (final) g_upload_window_seq = 0;
    pthread_mutex_unlock(&g_upload_dump_mu);
}
