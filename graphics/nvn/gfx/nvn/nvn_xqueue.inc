// ---------- Cross-queue event ordering (nvn_be_submit; included by nvn_mtl.m after nvn_be_exec) ----------
// NVN has several queues that run side by side; a command buffer orders itself after another queue's work with WaitEvent, and the other queue's SignalEvent may be submitted later.
// Here every queue is replayed into the one Metal queue, in the order the game submits. A wait whose signal was not replayed yet cannot be honoured in place (the GPU would wait
// for work queued behind it), so it used to be released at once (ev_wait ④) and the waiting queue ran ahead of the work it waits for. 11-07 Ocarina of Time (Switch 2): the compute
// queue's screen-space shadow dispatch waits for the event the graphics queue signals once the half-resolution linear depth is drawn; replayed first, it read the previous frame's
// depth every frame, and on a dynamic-resolution change (a new texture, nothing in it) it saw no occluders: the room's shadows vanished for one frame (31 of 40 resolution changes).
// Now such a wait parks the rest of that queue: what was replayed so far is committed, the remaining command streams (innermost first, then the callers after their CALL, then the
// submission's other handles) and the replay state (S, R, the render-enable block and the timestamp reports of the submission) are kept per queue, and later submissions on that
// queue go behind them. Whenever a submission ends, parked queues whose event now holds are replayed as their own submissions, in order, until none is ready.
// Kept safe: a park that would close a cycle (the parked queue signals what this one waits for and waits for what this one signals) is released at once as before, with a red line;
// a queue parked behind 256 other submissions, or that the CPU waits for (SyncWait on a fence taken while it was parked, QueueFinish, a present on it), is released at once too.
// Fences taken on a parked queue (QueueFenceSync) are owed: published with the fence of the submission that replays the parked work, and SyncWait waits for that.
// Only nvn_be_submit parks (hle_nvn.c QueueSubmitCommands); direct nvn_be_exec callers (frame replay, unit tests) keep the old release.
enum { XQ_MAX = 16, XQ_UNWIND_MAX = 64, XQ_WATCHDOG = 256 };
typedef struct {
    void *q;                          // NVN queue (opaque here)
    uint64_t *cont; int ncont, ccap;  // command streams still to replay, in order (each runs to its OP_END)
    void *state;                      // replay state at the park point (xq_state_save)
    int parked;                       // 1 = waiting for (val, mode, v); 2 = waiting for queue dep to drain (QueueWaitSync on its owed fence); 0 = runnable
    volatile uint32_t *val; uint32_t mode, v; void *dep;
    uint64_t since;                   // g_xq_subs when parked
    sync_t **owed; int nowed, ocap;   // fences taken on this queue while it had parked work
    volatile uint32_t *sig[64]; int nsig, sigall;   // events the parked streams signal from the GPU (sigall: more than fit, assume any): the cycle check reads this, not the streams
} xq_t;
static xq_t g_xq[XQ_MAX]; static int g_nxq;
static void *g_xq_cur;   // queue being replayed by nvn_be_submit (NULL: callers that do not park)
static const uint8_t *g_xq_uc[XQ_UNWIND_MAX]; static int g_xq_nuc, g_xq_ucover;   // continuation collected while unwinding, innermost first (g_xq_unwind / g_xq_pval: the wait that parked, with ev_wait)
static uint64_t g_xq_pieces, g_xq_check_ns, g_xq_piece_ns;   // replayed pieces beyond one per submission; time in the cycle check / in pieces after the first (stats line)
static uint64_t g_xq_subs, g_xq_parks, g_xq_resumes, g_xq_busy, g_xq_cycles, g_xq_forced, g_xq_owed_n, g_xq_deps;
static pthread_mutex_t g_xq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_xq_cv = PTHREAD_COND_INITIALIZER;   // owed fences: hle SyncWait threads
__attribute__((weak)) void nvn_hle_queue_enter(void *q) { (void)q; }   // hle_nvn.c: per-queue device, raster flip, virtual pool copies before each replayed piece (before sub_begin)
__attribute__((weak)) void nvn_hle_piece_begun(void) {}   // hle_nvn.c: texture write polling (after sub_begin)

static void xq_state_free(void *p);
static xq_t *xq_find(void *q) { for (int i = 0; i < g_nxq; i++) if (g_xq[i].q == q) return &g_xq[i]; return NULL; }
static int xq_busy_q(void *q) { xq_t *e = xq_find(q); return e && e->ncont; }
static void xq_drop(xq_t *e) { pthread_mutex_lock(&g_xq_mu); free(e->cont); xq_state_free(e->state); free(e->owed); *e = g_xq[--g_nxq]; pthread_mutex_unlock(&g_xq_mu); }   // g_xq_mu: owed lookups from other threads
// Does the command stream from p (to its END, through CALLs and JUMPs) signal val from the GPU?
static int xq_signals(const uint8_t *p, volatile uint32_t *val, int depth) {
    for (int steps = 0; p && steps < 1000000; steps++) {
        op_hdr h; memcpy(&h, p, sizeof h); if (rec_bad(h)) return 0;
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return 0;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); if (depth < 8 && xq_signals(t, val, depth + 1)) return 1; break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *x; uint32_t m; memcpy(&x, a, 8); memcpy(&m, a + 8, 4); if (x == val && !(m & NVN_EV_WAIT)) return 1; break; }
            default: break;
        }
        p += h.size;
    }
    return 0;
}
static void xq_sig_walk(xq_t *e, const uint8_t *p, int depth) {   // add the events stream p signals to e's set
    for (int steps = 0; p && steps < 1000000 && !e->sigall; steps++) {
        op_hdr h; memcpy(&h, p, sizeof h); if (rec_bad(h)) return;
        const uint8_t *a = p + 8;
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_CALL) { const uint8_t *t; memcpy(&t, a, 8); if (depth < 8) xq_sig_walk(e, t, depth + 1); else e->sigall = 1; }
        if (h.op == OP_SIGNAL_EVENT) { volatile uint32_t *x; uint32_t m; memcpy(&x, a, 8); memcpy(&m, a + 8, 4);
            if (!(m & NVN_EV_WAIT)) { int k = 0; while (k < e->nsig && e->sig[k] != x) k++; if (k == e->nsig) { if (e->nsig == 64) e->sigall = 1; else e->sig[e->nsig++] = x; } } }
        p += h.size;
    }
}
static int xq_sig_has(const xq_t *e, volatile uint32_t *val) { if (e->sigall) return 1; for (int k = 0; k < e->nsig; k++) if (e->sig[k] == val) return 1; return 0; }
static void xq_push(xq_t *e, uint64_t h) {
    if (e->ncont == e->ccap) { e->ccap = e->ccap ? e->ccap * 2 : 16; e->cont = realloc(e->cont, sizeof *e->cont * e->ccap); if (!e->cont) MDIE("no memory for parked command streams"); }
    e->cont[e->ncont++] = h; xq_sig_walk(e, (const uint8_t *)(uintptr_t)h, 0);
}
// Replay state carried across a park: what the commands after the wait read from the commands before it. A bound vertex range is also a dependency of the already-submitted piece:
// NVN lets the guest Finalize its pool while the queue is waiting, but this host has not encoded the commands after the wait yet. Keep only those 16 bound ranges and expose them only
// while this exact parked state is replayed; the finalized pool does not return to the global pool table. buf has one xq_buf_hold_add per non-empty entry.
typedef struct { uint64_t lo, hi, base, gpu; void *buf; } xq_vbuf;
typedef struct {
    typeof(S) s; typeof(R) r; int rc_skip; size_t nrc;
    xq_vbuf vb[16];
    uint64_t rc[];
} xq_state;
static _Thread_local const xq_state *g_xq_state_active;   // only the submit thread replaying this piece may see the finalized ranges

static int xq_range_end(uint64_t lo, uint64_t n, uint64_t *hi) { if (!n || lo > UINT64_MAX - n) return 0; *hi = lo + n; return 1; }
static const xq_vbuf *xq_vbuf_find(const xq_state *x, uint64_t lo, uint64_t hi) {
    if (!x) return NULL;
    for (int i = 0; i < 16; i++) if (x->vb[i].buf && lo >= x->vb[i].lo && hi <= x->vb[i].hi) return &x->vb[i];
    return NULL;
}
static void xq_vbuf_save(xq_state *x, int slot) {
    NVNbufferRange v = S.vbuf[slot]; uint64_t lo = nvn_ga2ca(v.address), hi;
    if (!v.address || !xq_range_end(lo, v.size, &hi)) return;
    pthread_mutex_lock(&g_mu);
    int i = pool_find(lo, 1);
    if (i >= 0) {
        pool_res(i);
        uint64_t end = MIN(hi, (uint64_t)g_pools[i].hi);
        if (end > lo) {
            x->vb[slot] = (xq_vbuf){lo, end, g_pools[i].base, g_pools[i].gpu, (__bridge void *)g_pools[i].buf};
            xq_buf_hold_add(x->vb[slot].buf);
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (x->vb[slot].buf) return;
    const xq_vbuf *old = xq_vbuf_find(g_xq_state_active, lo, hi);   // the pool was finalized after the preceding park; transfer its held dependency if this piece parks again
    if (old) { x->vb[slot] = (xq_vbuf){lo, hi, old->base, old->gpu, old->buf}; xq_buf_hold_add(old->buf); }
}
static void *xq_state_save(void) {
    size_t k = g_rc_npend, n = sizeof(xq_state) + k * sizeof(uint64_t); xq_state *x = calloc(1, n); if (!x) MDIE("no memory for a parked queue's state");
    memcpy(&x->s, &S, sizeof S); memcpy(&x->r, &R, sizeof R); x->rc_skip = g_rc_skip; x->nrc = k; if (k) memcpy(x->rc, g_rc_pend, k * sizeof(uint64_t));
    for (int i = 0; i < 16; i++) xq_vbuf_save(x, i);
    return x;
}
static void xq_state_load(const void *p) {
    const xq_state *x = p; memcpy(&S, &x->s, sizeof S); memcpy(&R, &x->r, sizeof R); g_rc_skip = x->rc_skip;
    for (size_t i = 0; i < x->nrc; i++) rc_note(x->rc[i], 1);
    for (int i = 0; i < 16; i++) if (x->vb[i].buf) res_add((__bridge id<MTLBuffer>)x->vb[i].buf);
    res_commit(); g_xq_state_active = x;
    g_pso_dirty = g_dss_dirty = 1; args_dirty_all();
}
static void xq_state_deactivate(void) { g_xq_state_active = NULL; }
static void xq_state_free(void *p) {
    xq_state *x = p; if (!x) return;
    if (g_xq_state_active == x) g_xq_state_active = NULL;
    for (int i = 0; i < 16; i++) if (x->vb[i].buf) xq_buf_hold_del(x->vb[i].buf);
    free(x);
}
static int xq_addr_want(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu, uint64_t *avail) {
    uint64_t hi; if (!xq_range_end(addr, len, &hi)) return 0;
    const xq_vbuf *v = xq_vbuf_find(g_xq_state_active, addr, hi); if (!v) return 0;
    *buf = v->buf; *off = addr - v->base; *gpu = v->gpu + *off; *avail = v->hi - addr; return 1;
}
// ev_wait ④: may this unmet wait park the queue being replayed? p = the operation after the wait
static int xq_may_park_(const uint8_t *p, volatile uint32_t *val, uint32_t mode, uint32_t v);
static int xq_may_park(const uint8_t *p, volatile uint32_t *val, uint32_t mode, uint32_t v) {
    if (!g_xq_cur) return 0;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); int r = xq_may_park_(p, val, mode, v); g_xq_check_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0; return r;
}
static int xq_may_park_(const uint8_t *p, volatile uint32_t *val, uint32_t mode, uint32_t v) {
    if (g_defer_nact || g_hoist_n || g_ret_n > XQ_UNWIND_MAX - 2) { g_xq_busy++; return 0; }   // a lookahead (deferred draws, hoisted clears) spans the wait: keep the old release
    for (int i = 0; i < g_nxq; i++) {   // cycle: a parked queue signals what this one waits for, and waits for what the rest of this one signals
        const xq_t *e = &g_xq[i]; if (e->q == g_xq_cur || e->parked != 1 || !xq_sig_has(e, val) || ev_cond(ev_now_locked(e->val), e->mode, e->v)) continue;   // a parked queue that is ready already is no cycle (holds g_evw_mu: ev_wait)
        int mine = xq_signals(p, e->val, 0); for (int k = 0; !mine && k < g_nrest; k++) mine = xq_signals((const uint8_t *)(uintptr_t)g_rest[k], e->val, 0);
        for (int k = g_ret_n - 1; !mine && k >= 0 && k < 16; k--) mine = xq_signals(g_ret[k], e->val, 0);
        if (mine) { g_xq_cycles++; static int n; if (n++ < 4) MLOG("【红】 cross-queue event wait on %p: queue %p is parked waiting for %p, which only this queue signals later, and signals this one: a cycle, released at once", val, e->q, e->val); return 0; }
    }
    (void)mode; (void)v; return 1;
}
static void xq_unwind_push(const uint8_t *p) { if (g_xq_nuc < XQ_UNWIND_MAX) g_xq_uc[g_xq_nuc++] = p; else g_xq_ucover = 1; }
static void xq_publish_owed(xq_t *e, uint64_t fence) {
    if (!e->nowed) return;
    pthread_mutex_lock(&g_xq_mu); for (int i = 0; i < e->nowed; i++) sync_publish(e->owed[i], fence); e->nowed = 0; pthread_cond_broadcast(&g_xq_cv); pthread_mutex_unlock(&g_xq_mu);
}
// One piece: replay streams h[0..n) of queue q as a submission; returns 1 if it parked (the rest is in q's entry)
static int xq_piece(void *q, const uint64_t *h, int n, const void *state) {
    nvn_hle_queue_enter(q);
    nvn_be_sub_begin(); nvn_hle_piece_begun(); xq_state_deactivate(); if (state) xq_state_load(state);
    nvn_be_sub_index(h, n);
    g_xq_cur = q; g_xq_unwind = 0; g_xq_nuc = 0; g_xq_ucover = 0;
    int i = 0;
    for (; i < n; i++) { nvn_be_exec_rest(h + i + 1, n - i - 1); nvn_be_exec((const uint8_t *)(uintptr_t)h[i]); if (g_xq_unwind) break; }
    nvn_be_exec_rest(NULL, 0); g_xq_cur = NULL;
    void *st = g_xq_unwind ? xq_state_save() : NULL;
    uint64_t fence = nvn_be_sub_commit();
    xq_state_deactivate();
    xq_t *e = xq_find(q);
    if (!g_xq_unwind) { if (e) { xq_publish_owed(e, fence); if (!e->ncont && !e->parked) xq_drop(e); } return 0; }
    g_xq_unwind = 0;
    if (g_xq_ucover) MDIE("cross-queue park: CALL nesting deeper than %d", XQ_UNWIND_MAX);
    if (!e) { if (g_nxq == XQ_MAX) MDIE("cross-queue park: more than %d queues", XQ_MAX); pthread_mutex_lock(&g_xq_mu); e = &g_xq[g_nxq++]; memset(e, 0, sizeof *e); e->q = q; pthread_mutex_unlock(&g_xq_mu); }
    uint64_t *rest = e->cont; int nrest = e->ncont; e->cont = NULL; e->ncont = e->ccap = 0; e->nsig = e->sigall = 0;   // work queued behind (none on the first park)
    for (int k = 0; k < g_xq_nuc; k++) xq_push(e, (uint64_t)(uintptr_t)g_xq_uc[k]);
    for (int k = i + 1; k < n; k++) xq_push(e, h[k]);
    for (int k = 0; k < nrest; k++) xq_push(e, rest[k]);
    free(rest); xq_state_free(e->state); e->state = st;
    e->parked = 1; e->val = g_xq_pval; e->mode = g_xq_pmode; e->v = g_xq_pv; e->since = g_xq_subs; g_xq_parks++;
    { static int n_; if (!n_++) MLOG("cross-queue event waits: queue %p parked at a wait on %p (mode %u, value %u, now %u) until another queue's signal is replayed (first case)", q, g_xq_pval, g_xq_pmode, g_xq_pv, ev_now(g_xq_pval)); }
    return 1;
}
static int xq_ready(const xq_t *e) {
    if (e->parked == 1) { pthread_mutex_lock(&g_evw_mu); int ok = ev_cond(ev_now_locked(e->val), e->mode, e->v); pthread_mutex_unlock(&g_evw_mu); return ok; }
    if (e->parked == 2) return !xq_busy_q(e->dep);
    return e->ncont > 0;
}
// Replay a queue's parked work (it may park again); force = released without its wait being met
static void xq_run(xq_t *e, int force) {
    void *q = e->q; uint64_t *c = e->cont; int nc = e->ncont; void *st = e->state;
    e->cont = NULL; e->ncont = e->ccap = 0; e->state = NULL; e->parked = 0; e->nsig = e->sigall = 0;
    if (force) { g_xq_forced++; } else g_xq_resumes++;
    if (nc) { uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); g_xq_pieces++; xq_piece(q, c, nc, st); g_xq_piece_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0; }
    else if ((e = xq_find(q)) && !e->parked) { xq_publish_owed(e, nvn_be_last_fence()); xq_drop(e); }
    free(c); xq_state_free(st);
}
static void xq_drain_ready(void) {
    for (int again = 1; again;) { again = 0;
        for (int i = 0; i < g_nxq; i++) if (xq_ready(&g_xq[i])) { xq_run(&g_xq[i], 0); again = 1; break; } }
}
static void xq_stats(void) {
    if (g_xq_subs % 3000 || !(g_xq_parks | g_xq_busy | g_xq_cycles | g_xq_forced)) return;
    MLOG("cross-queue event waits up to submission %llu: parked %llu, resumed %llu, kept released (lookahead spans the wait) %llu, cycles %llu, released by force %llu, owed fences %llu, queue waits behind a parked queue %llu, parked now %d; extra pieces %llu (%.1f ms submit thread in them), cycle checks %.1f ms",
         (unsigned long long)g_xq_subs, (unsigned long long)g_xq_parks, (unsigned long long)g_xq_resumes, (unsigned long long)g_xq_busy, (unsigned long long)g_xq_cycles, (unsigned long long)g_xq_forced,
         (unsigned long long)g_xq_owed_n, (unsigned long long)g_xq_deps, g_nxq, (unsigned long long)g_xq_pieces, g_xq_piece_ns / 1e6, g_xq_check_ns / 1e6);
}
// Release parked work now (CPU waits for it, or the watchdog); why = the reason for the red line. Caller holds the submit lock
static void xq_force(xq_t *e, const char *why) {
    static int n; if (n++ < 8) MLOG("【红】 cross-queue event waits: queue %p released without its wait (%p, value %u, now %u): %s", e->q, e->val, e->v, e->val ? ev_now(e->val) : 0, why);
    xq_run(e, 1); xq_drain_ready();
}
void nvn_be_submit(void *q, const uint64_t *h, int n) {
    g_xq_subs++;
    xq_t *e = xq_find(q);
    if (e && (e->ncont || e->parked)) { for (int i = 0; i < n; i++) xq_push(e, h[i]); }   // behind its parked work, in order
    else xq_piece(q, h, n, NULL);
    xq_drain_ready();
    for (int i = 0; i < g_nxq; i++) if (g_xq[i].parked && g_xq_subs - g_xq[i].since > XQ_WATCHDOG) { xq_force(&g_xq[i], "still parked after 256 submissions"); i = -1; }
    xq_stats();
}
// QueueFinish / present on q: its parked work goes now
void nvn_be_xq_flush(void *q, const char *why) { xq_t *e = xq_find(q); if (e && (e->ncont || e->parked)) xq_force(e, why); }
// QueueFenceSync on q: 1 = q has parked work, the fence is owed (published when that work is replayed)
int nvn_be_xq_fence(void *q, void *sv) {
    sync_t *s = sv; xq_t *e = xq_find(q); if (!e || !(e->ncont || e->parked)) return 0;
    pthread_mutex_lock(&g_xq_mu);
    if (e->nowed == e->ocap) { e->ocap = e->ocap ? e->ocap * 2 : 8; e->owed = realloc(e->owed, sizeof *e->owed * e->ocap); if (!e->owed) MDIE("no memory for owed fences"); }
    s->fenced = 0; s->ext_ev = NULL; e->owed[e->nowed++] = s; pthread_mutex_unlock(&g_xq_mu); g_xq_owed_n++;
    return 1;
}
// Is s owed by parked work? (any thread)
static int xq_owed_locked(const void *s, void **q) { for (int i = 0; i < g_nxq; i++) for (int k = 0; k < g_xq[i].nowed; k++) if ((const void *)g_xq[i].owed[k] == s) { if (q) *q = g_xq[i].q; return 1; } return 0; }
int nvn_be_xq_owed(const void *s, void **q) { pthread_mutex_lock(&g_xq_mu); int r = xq_owed_locked(s, q); pthread_mutex_unlock(&g_xq_mu); return r; }
// SyncWait on an owed fence: wait up to ns for it to be published; 1 = published
int nvn_be_xq_owed_wait(const void *s, uint64_t ns) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec + ns; ts.tv_sec = (time_t)(t / 1000000000ull); ts.tv_nsec = (long)(t % 1000000000ull);
    pthread_mutex_lock(&g_xq_mu); int r = 0;
    while ((r = !xq_owed_locked(s, NULL)) == 0) if (pthread_cond_timedwait(&g_xq_cv, &g_xq_mu, &ts)) { r = !xq_owed_locked(s, NULL); break; }
    pthread_mutex_unlock(&g_xq_mu); return r;
}
// QueueWaitSync(q, s) with s owed by owner: q's later submissions go behind owner's parked work
int nvn_be_xq_wait_queue(void *q, void *owner, const void *sync) {
    (void)sync;
    if (q == owner || !xq_busy_q(owner)) return 1;
    xq_t *e = xq_find(q);
    if (e && (e->ncont || e->parked)) return 1;   // already behind its own parked work, which is replayed in order
    if (!e) { if (g_nxq == XQ_MAX) MDIE("cross-queue park: more than %d queues", XQ_MAX); pthread_mutex_lock(&g_xq_mu); e = &g_xq[g_nxq++]; memset(e, 0, sizeof *e); e->q = q; pthread_mutex_unlock(&g_xq_mu); }
    e->parked = 2; e->dep = owner; e->since = g_xq_subs; g_xq_deps++;
    return 1;
}
