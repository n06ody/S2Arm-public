// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// The dirty log describes pages made writable, which can cover several disjoint
// resources in one protection chunk. Keep the bytes supplied to the last upload
// to distinguish actual content changes from another write in the same chunk.
enum { TEX_BYTES_ONE_MAX = 8 * 1024 * 1024, TEX_BYTES_TOTAL_MAX = 64 * 1024 * 1024, TEX_BYTES_MAX_RUNS = 32 };
struct tex_byte_cache {
    struct rp_tex *owner;
    struct tex_byte_cache *newer, *older;
    const uint8_t *mem;
    size_t len;
    uint64_t map, log_id, failure_epoch;
    uint8_t bytes[];
};
static struct tex_byte_cache *g_tex_bytes_newest, *g_tex_bytes_oldest;
static size_t g_tex_bytes_live;
static uint64_t g_tex_bytes_upload_sub;
static int g_tex_bytes_on = -1;   // g_rmu; latched for the session
static _Atomic uint64_t g_tex_bytes_failure_epoch;

// A completion can run while the replay thread owns g_rmu. Invalidate
// snapshots lazily at the next resolve instead of waiting for that lock.
void rp_res_byte_upload_failed(void) {
    atomic_fetch_add_explicit(&g_tex_bytes_failure_epoch, 1, memory_order_release);
}

static uint64_t tex_bytes_failure_epoch(void) {
    return atomic_load_explicit(&g_tex_bytes_failure_epoch, memory_order_acquire);
}

static void tex_bytes_unlink(struct tex_byte_cache *b) {
    if (b->newer) b->newer->older = b->older;
    else g_tex_bytes_newest = b->older;
    if (b->older) b->older->newer = b->newer;
    else g_tex_bytes_oldest = b->newer;
}

static void tex_bytes_promote(struct tex_byte_cache *b) {
    b->newer = NULL;
    b->older = g_tex_bytes_newest;
    if (b->older) b->older->newer = b;
    else g_tex_bytes_oldest = b;
    g_tex_bytes_newest = b;
}

static void tex_bytes_free(struct tex_byte_cache *b) {
    if (!b) return;
    g_tex_bytes_live -= sizeof *b + b->len;
    free(b);
}

static void tex_bytes_drop(struct rp_tex *x) {
    struct tex_byte_cache *b = x->byte_cache;
    if (!b) return;
    x->byte_cache = NULL;
    tex_bytes_unlink(b);
    tex_bytes_free(b);
}

struct tex_byte_run { uintptr_t lo, hi; };
struct tex_byte_runs {
    unsigned n;
    int overflow;
    struct tex_byte_run ranges[TEX_BYTES_MAX_RUNS];
};

// Clip to the texture window in the caller. Merge overlapping or adjacent
// intervals before comparing, including repeated events for the same chunk.
static void tex_bytes_add(struct tex_byte_runs *r, uintptr_t lo, uintptr_t hi) {
    if (r->overflow) return;
    for (unsigned i = 0; i < r->n; ) {
        struct tex_byte_run run = r->ranges[i];
        if (lo > run.hi || run.lo > hi) { i++; continue; }
        if (run.lo < lo) lo = run.lo;
        if (run.hi > hi) hi = run.hi;
        r->ranges[i] = r->ranges[--r->n];
        i = 0;   // the extended interval can now join an earlier one
    }
    if (r->n == TEX_BYTES_MAX_RUNS) { r->overflow = 1; return; }
    r->ranges[r->n++] = (struct tex_byte_run){lo, hi};
}

// The head must still equal the fully consumed cursor sampled before
// re-protection. A new reservation, even before its write generation is
// published, can cover bytes outside the compared intervals. A gap in the
// log likewise leaves the consumed cursor behind the head.
static int tex_bytes_stable(const struct rp_tex *x, const tex_t *t, uint64_t map,
                            uint64_t cursor, uint64_t stamp) {
    uint64_t id, head;
    return x->span && x->log_span == x->span &&
           map == nvn_dirty_mapping_generation() &&
           !rp_buf_gpu_pending((uintptr_t)t->mem, t->L.size) &&
           tex_pages_armed(x, t) &&
           !nvn_dirty_since(t->mem, t->L.size, stamp) &&
           nvn_dirty_log_open(x->span, &id, &head) && id == x->log_id && head == cursor;
}

static int tex_bytes_unchanged(struct rp_tex *x, const tex_t *t, const struct tex_byte_runs *r) {
    struct tex_byte_cache *b = x->byte_cache;
    if (!b || b->mem != t->mem || b->len != t->L.size || b->log_id != x->log_id ||
        b->map != nvn_dirty_mapping_generation() || r->overflow || !r->n ||
        rp_buf_gpu_pending((uintptr_t)t->mem, t->L.size)) return 0;

    const uint64_t cursor = x->log_cur, stamp = nvn_dirty_now();
    nvn_dirty_protect(t->mem, t->L.size);
    if (!tex_bytes_stable(x, t, b->map, cursor, stamp)) return 0;
    int equal = 1;
    for (unsigned i = 0; i < r->n; i++) {
        const struct tex_byte_run run = r->ranges[i];
        const size_t off = run.lo - (uintptr_t)t->mem;
        if (memcmp(t->mem + off, b->bytes + off, run.hi - run.lo)) { equal = 0; break; }
    }
    if (!tex_bytes_stable(x, t, b->map, cursor, stamp) ||
        b->failure_epoch != tex_bytes_failure_epoch()) return 0;
    if (!equal) { rp_perf_add(RP_PERF_TEX_BYTE_CHANGED, 1); return 0; }

    // Keep the pre-protect cursor and stamp, never the head after comparison.
    x->stamp = stamp;
    tex_bytes_unlink(b);
    tex_bytes_promote(b);
    rp_perf_add(RP_PERF_TEX_BYTE_EQUAL, 1);
    rp_perf_add(RP_PERF_TEX_BYTE_SKIPPED, b->len);
    return 1;
}

// Take one immutable source, shared by all upload paths. A separate memcpy
// after encoding wouldn't necessarily describe the same guest bytes.
static struct tex_byte_cache *tex_bytes_capture(struct rp_tex *x, const tex_t *t, uint64_t stamp) {
    const uint64_t map = nvn_dirty_mapping_generation(), cursor = x->log_cur;
    if (!tex_bytes_stable(x, t, map, cursor, stamp)) return NULL;
    const size_t space = sizeof(struct tex_byte_cache) + (size_t)t->L.size;
    while (g_tex_bytes_live > TEX_BYTES_TOTAL_MAX - space)
        tex_bytes_drop(g_tex_bytes_oldest->owner);
    struct tex_byte_cache *b = malloc(space);
    if (!b) return NULL;
    *b = (struct tex_byte_cache){.owner = x, .mem = t->mem, .len = (size_t)t->L.size,
                                 .map = map, .log_id = x->log_id, .failure_epoch = tex_bytes_failure_epoch()};
    g_tex_bytes_live += space;
    memcpy(b->bytes, t->mem, b->len);
    if (tex_bytes_stable(x, t, map, cursor, stamp)) return b;
    tex_bytes_free(b);
    return NULL;
}
