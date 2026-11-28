// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// 只量已有操作。字节统一用客体源布局大小，段回调只能证明该段状态。
enum tex_upload_reason {
    TU_NONE, TU_FIRST, TU_RETRY, TU_LOG, TU_PAGES, TU_LOG_FALLBACK,
    TU_UNTRACKED, TU_UNRELIABLE, TU_NON_CPU_INIT, TU_SPARSE_DIRTY, TU_SPARSE_FALLBACK, TU_REASON_N
};
enum tex_upload_route {
    TU_NO_ROUTE, TU_CPU_LINEAR, TU_CPU_SWIZZLE, TU_GPU, TU_MIXED, TU_DESWIZZLE_UNKNOWN,
    TU_BC1, TU_BC7, TU_ROUTE_N
};
enum tex_upload_result { TU_RECORDED, TU_RECORD_UNKNOWN, TU_DEFERRED, TU_REJECTED, TU_RESULT_N };
enum tex_upload_done { TU_COMPLETE, TU_SEG_FAILED, TU_FAULT, TU_ABORTED, TU_SEG_UNKNOWN, TU_DONE_N };
enum tex_upload_cache {
    TU_CACHE_EQUAL, TU_CACHE_UNARMED, TU_CACHE_EVICT, TU_CACHE_REJECT,
    TU_CMP_EARLY, TU_CMP_UNSTABLE, TU_CMP_DIFF, TU_CMP_EQUAL, TU_CACHE_N
};
enum { TU_RES_MAX = 64, TU_SEG_MAX = 64, TU_REPORT_MAX = 8 };

struct tu_count { uint64_t n, bytes; };
struct tu_resource {
    uint64_t serial;
    nvn_texdesc d;
    uint32_t layers;
    struct tu_count total, bc_decode, reason[TU_REASON_N], route[TU_ROUTE_N], result[TU_RESULT_N];
};
struct tu_window {
    uint64_t id, cache_peak;
    struct tu_count attempt, overflow, linear_full, gpu_fallback, bc_decode;
    struct tu_count reason[TU_REASON_N], route[TU_ROUTE_N], result[TU_RESULT_N], done[TU_DONE_N], cache[TU_CACHE_N];
    struct tu_resource resources[TU_RES_MAX];
    unsigned nres;
};
struct tu_segment { nvn_gpu_cmd cmd; uint64_t fence; struct tu_count layouts; };

// 写入端可持 g_rmu；段回调及呈现线程只拿这把锁，锁内不调后端或日志。
static pthread_mutex_t g_tu_mu = PTHREAD_MUTEX_INITIALIZER;
static struct tu_window g_tu;
static struct tu_segment g_tu_segments[TU_SEG_MAX];
static int g_tu_latched;
static uint64_t g_tu_cache_live;
int rp_tex_upload_diag_enabled;
static inline int tex_upload_diag_on(void) {
    return __atomic_load_n(&rp_tex_upload_diag_enabled, __ATOMIC_RELAXED);
}
static void tu_add(struct tu_count *c, uint64_t bytes) {
    c->n++;
    c->bytes += bytes;
}
static void tu_merge(struct tu_count *c, const struct tu_count *in) {
    c->n += in->n;
    c->bytes += in->bytes;
}

static inline void tex_upload_diag_cache(enum tex_upload_cache what, uint64_t bytes, uint64_t live) {
    if (!tex_upload_diag_on()) return;
    pthread_mutex_lock(&g_tu_mu);
    if (what != TU_CACHE_N) tu_add(&g_tu.cache[what], bytes);
    g_tu_cache_live = live;
    if (live > g_tu.cache_peak) g_tu.cache_peak = live;
    pthread_mutex_unlock(&g_tu_mu);
}

static void tex_upload_diag_record(const tex_t *t, nvn_gpu_cmd cmd, enum tex_upload_reason why,
                                  enum tex_upload_route route, enum tex_upload_result result,
                                  int gpu_fallback, int bc_decode) {
    const uint64_t bytes = t->L.size;
    pthread_mutex_lock(&g_tu_mu);
    tu_add(&g_tu.attempt, bytes);
    tu_add(&g_tu.reason[why], bytes);
    tu_add(&g_tu.route[route], bytes);
    tu_add(&g_tu.result[result], bytes);
    if ((t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) &&
        why != TU_FIRST && why != TU_NON_CPU_INIT)
        tu_add(&g_tu.linear_full, bytes);
    if (gpu_fallback) tu_add(&g_tu.gpu_fallback, bytes);
    if (bc_decode) tu_add(&g_tu.bc_decode, bytes);

    unsigned i;
    for (i = 0; i < g_tu.nres; i++)
        if (g_tu.resources[i].serial == t->serial) break;
    if (i < TU_RES_MAX) {
        struct tu_resource *r = &g_tu.resources[i];
        if (i == g_tu.nres) {
            r->serial = t->serial;
            r->d = t->d;
            r->layers = t->L.layers ? t->L.layers : 1;
            g_tu.nres++;
        }
        tu_add(&r->total, bytes);
        if (bc_decode) tu_add(&r->bc_decode, bytes);
        tu_add(&r->reason[why], bytes);
        tu_add(&r->route[route], bytes);
        tu_add(&r->result[result], bytes);
    } else tu_add(&g_tu.overflow, bytes);

    if (result == TU_RECORD_UNKNOWN) tu_add(&g_tu.done[TU_SEG_UNKNOWN], bytes);
    if (result == TU_RECORDED) {
        struct tu_segment *slot = NULL;
        for (i = 0; i < TU_SEG_MAX; i++) {
            struct tu_segment *seg = &g_tu_segments[i];
            if (seg->cmd == cmd) { slot = seg; break; }
            if (!slot && !seg->cmd && !seg->fence) slot = seg;
        }
        if (slot) {
            slot->cmd = cmd;
            tu_add(&slot->layouts, bytes);
        } else tu_add(&g_tu.done[TU_SEG_UNKNOWN], bytes);
    }
    pthread_mutex_unlock(&g_tu_mu);
}

void rp_tex_upload_diag_submit(nvn_gpu_cmd cmd, uint64_t fence) {
    if (!cmd || !tex_upload_diag_on()) return;
    pthread_mutex_lock(&g_tu_mu);
    for (unsigned i = 0; i < TU_SEG_MAX; i++) {
        struct tu_segment *seg = &g_tu_segments[i];
        if (seg->cmd != cmd) continue;
        if (fence) {
            seg->cmd = NULL;
            seg->fence = fence;
        } else {
            tu_merge(&g_tu.done[TU_ABORTED], &seg->layouts);
            *seg = (struct tu_segment){0};
        }
        break;
    }
    pthread_mutex_unlock(&g_tu_mu);
}

void rp_tex_upload_diag_done(const nvn_gpu_done *d) {
    if (!tex_upload_diag_on() || !d->signal) return;
    pthread_mutex_lock(&g_tu_mu);
    for (unsigned i = 0; i < TU_SEG_MAX; i++) {
        struct tu_segment *seg = &g_tu_segments[i];
        if (seg->fence != d->signal) continue;
        enum tex_upload_done what = d->status == NVN_GPU_DONE_OK ? TU_COMPLETE :
                                   d->status == NVN_GPU_DONE_FAULT ? TU_FAULT : TU_SEG_FAILED;
        tu_merge(&g_tu.done[what], &seg->layouts);
        *seg = (struct tu_segment){0};
        break;
    }
    pthread_mutex_unlock(&g_tu_mu);
}

static void tu_append_counts(char *line, size_t cap, const char *const *names,
                          const struct tu_count *counts, unsigned n, int print_zero) {
    size_t used = strlen(line);
    for (unsigned i = 0; i < n && used < cap; i++) {
        if (!print_zero && !counts[i].n) continue;
        used += snprintf(line + used, cap - used, " %s=%" PRIu64 "/%" PRIu64,
                         names[i], counts[i].n, counts[i].bytes);
    }
}
static void tu_log_counts(char *line, size_t cap, const char *const *names,
                          const struct tu_count *counts, unsigned n, int print_zero) {
    tu_append_counts(line, cap, names, counts, n, print_zero);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "tex-upload-diag", line);
}

void rp_tex_upload_diag_window(uint64_t frames) {
    if (!tex_upload_diag_on()) return;
    static const char *const reasons[TU_REASON_N] = {
        [TU_FIRST] = "first", [TU_RETRY] = "retry", [TU_LOG] = "dirty_log", [TU_PAGES] = "dirty_pages",
        [TU_LOG_FALLBACK] = "log_fallback", [TU_UNTRACKED] = "untracked", [TU_UNRELIABLE] = "unreliable",
        [TU_NON_CPU_INIT] = "non_cpu_init", [TU_SPARSE_DIRTY] = "sparse_dirty", [TU_SPARSE_FALLBACK] = "sparse_fallback",
    };
    static const char *const routes[TU_ROUTE_N] = {
        "no_route", "cpu_linear", "cpu_swizzle", "gpu_deswizzle", "mixed_deswizzle", "deswizzle_unknown",
        "bc1_snapshot", "bc7_snapshot",
    };
    static const char *const results[TU_RESULT_N] = {"recorded", "record_unknown", "deferred", "rejected"};
    static const char *const done[TU_DONE_N] = {"complete", "segment_failed", "gpu_fault", "aborted", "segment_unknown"};
    static const char *const caches[TU_CACHE_N] = {
        "byte_equal_capture", "unarmed_capture", "evict", "capture_rejected", "unarmed_early", "unarmed_unstable", "unarmed_diff", "unarmed_equal",
    };

    struct tu_count pending = {0};
    pthread_mutex_lock(&g_tu_mu);
    if (!tex_upload_diag_on()) {
        pthread_mutex_unlock(&g_tu_mu);
        return;
    }
    struct tu_window w = g_tu;
    g_tu = (struct tu_window){.id = w.id + 1, .cache_peak = g_tu_cache_live};
    for (unsigned i = 0; i < TU_SEG_MAX; i++) tu_merge(&pending, &g_tu_segments[i].layouts);
    if (!frames) {
        __atomic_store_n(&rp_tex_upload_diag_enabled, 0, __ATOMIC_RELAXED);
        g_tu_cache_live = 0;
        memset(g_tu_segments, 0, sizeof g_tu_segments);
        g_tu = (struct tu_window){0};
    }
    pthread_mutex_unlock(&g_tu_mu);

    char line[1536];
    snprintf(line, sizeof line, "window=%" PRIu64 " frames=%" PRIu64 " attempt=%" PRIu64 "/%" PRIu64
             " overflow=%" PRIu64 "/%" PRIu64 " linear_full=%" PRIu64 "/%" PRIu64
             " gpu_fallback=%" PRIu64 "/%" PRIu64 " bc_decode=%" PRIu64 "/%" PRIu64,
             w.id, frames, w.attempt.n, w.attempt.bytes, w.overflow.n, w.overflow.bytes,
             w.linear_full.n, w.linear_full.bytes, w.gpu_fallback.n, w.gpu_fallback.bytes, w.bc_decode.n, w.bc_decode.bytes);
    tu_log_counts(line, sizeof line, results, w.result, TU_RESULT_N, 1);
    snprintf(line, sizeof line, "window=%" PRIu64 " pending=%" PRIu64 "/%" PRIu64, w.id, pending.n, pending.bytes);
    tu_log_counts(line, sizeof line, done, w.done, TU_DONE_N, 1);
    if (w.attempt.n) {
        snprintf(line, sizeof line, "window=%" PRIu64 " reason", w.id);
        tu_log_counts(line, sizeof line, reasons, w.reason, TU_REASON_N, 0);
        snprintf(line, sizeof line, "window=%" PRIu64 " route", w.id);
        tu_log_counts(line, sizeof line, routes, w.route, TU_ROUTE_N, 0);
    }
    snprintf(line, sizeof line, "window=%" PRIu64 " cache_peak_B=%" PRIu64, w.id, w.cache_peak);
    tu_log_counts(line, sizeof line, caches, w.cache, TU_CACHE_N, 0);

    struct tu_count omitted = {w.attempt.n - w.overflow.n, w.attempt.bytes - w.overflow.bytes};
    for (unsigned rank = 0; rank < TU_REPORT_MAX && rank < w.nres; rank++) {
        struct tu_resource *r = &w.resources[0];
        for (unsigned i = 1; i < w.nres; i++) {
            struct tu_resource *next = &w.resources[i];
            if (next->total.n && (!r->total.n || next->total.bytes > r->total.bytes)) r = next;
        }
        snprintf(line, sizeof line, "window=%" PRIu64 " ser=%" PRIu64 " fmt=%#x target=%d shape=%ux%ux%u levels=%u layers=%u"
                 " attempt=%" PRIu64 "/%" PRIu64 " bc_decode=%" PRIu64 "/%" PRIu64,
                 w.id, r->serial, (unsigned)r->d.format, r->d.target, r->d.width, r->d.height, r->d.depth,
                 r->d.levels ? r->d.levels : 1, r->layers, r->total.n, r->total.bytes, r->bc_decode.n, r->bc_decode.bytes);
        tu_append_counts(line, sizeof line, reasons, r->reason, TU_REASON_N, 0);
        tu_append_counts(line, sizeof line, routes, r->route, TU_ROUTE_N, 0);
        tu_log_counts(line, sizeof line, results, r->result, TU_RESULT_N, 0);
        omitted.n -= r->total.n;
        omitted.bytes -= r->total.bytes;
        r->total.n = 0;
    }
    if (omitted.n) {
        snprintf(line, sizeof line, "window=%" PRIu64 " other_tracked=%" PRIu64 "/%" PRIu64, w.id, omitted.n, omitted.bytes);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "tex-upload-diag", line);
    }
}
