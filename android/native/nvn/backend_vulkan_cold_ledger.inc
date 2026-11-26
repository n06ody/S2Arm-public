// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// Mirror writes in a command become persistent evidence only after successful
// vkQueueSubmit. The planner also considers the prefix of the current main
// segment and the pre segment, in that physical submission order.
enum { COLD_LEDGER_EVENTS_MAX = 8192 };
#ifdef S2A_NVK_COLD_GUARD_HOST_TEST
static unsigned cold_test_event_limit = COLD_LEDGER_EVENTS_MAX;
void nvn_gpu_vk_test_cold_event_limit(unsigned limit) {
    cold_test_event_limit = limit ? limit : COLD_LEDGER_EVENTS_MAX;
}
#endif
struct cold_event {
    nvn_gpu_buf buf;
    uint64_t off, bytes, barriers;
    unsigned writer;
    int exact;
};

void nvn_vk_cold_mirror_track(nvn_gpu_buf buf) {
    if (g.cold_enabled && buf && !buf->cold_ledger) buf->cold_ledger = calloc(1, sizeof *buf->cold_ledger);
}

void nvn_vk_cold_mirror_host_write(nvn_gpu_buf buf, uint64_t off, uint64_t bytes) {
    if (!buf || !buf->cold_ledger || !bytes || !buf_range(buf, off, bytes)) return;
    pthread_mutex_lock(&g_submit_mu);
    __atomic_add_fetch(&buf->cold_generation, 1, __ATOMIC_RELAXED);
    // Replay only exposes this after a completed preceding mirror use, and
    // takes the same lock/interval read reservation for further CPU updates.
    if (!cold_ledger_write(buf->cold_ledger, off, off + bytes, g.cold_epoch, COLD_READ_HOST, 1))
        cold_diag_reject(NULL, COLD_DIAG_HOST_SPAN_LIMIT);
    pthread_mutex_unlock(&g_submit_mu);
}

static void cold_note_write(nvn_gpu_cmd c, nvn_gpu_buf buf, uint64_t off, uint64_t bytes,
                            unsigned writer, int exact) {
    if (!buf || !buf->cold_ledger || !bytes) return;
    __atomic_add_fetch(&buf->cold_generation, 1, __ATOMIC_RELAXED);
    if (c->cold_events_lost) return;
#ifdef S2A_NVK_COLD_GUARD_HOST_TEST
    if (c->cold_nevents == cold_test_event_limit) { c->cold_events_lost = COLD_EVENTS_LIMIT; return; }
#endif
    if (!buf_range(buf, off, bytes)) { off = 0; bytes = buf->bytes; writer = 0; exact = 0; }
    if (c->cold_nevents == c->cold_cap) {
        unsigned cap = c->cold_cap ? c->cold_cap * 2 : 32;
        if (cap > COLD_LEDGER_EVENTS_MAX) { c->cold_events_lost = COLD_EVENTS_LIMIT; return; }
        struct cold_event *p = realloc(c->cold_events, (size_t)cap * sizeof *p);
        if (!p) { c->cold_events_lost = COLD_EVENTS_OOM; return; }
        c->cold_events = p;
        c->cold_cap = cap;
    }
    c->cold_events[c->cold_nevents++] = (struct cold_event){buf, off, bytes, c->cold_barriers, writer, exact};
}

static void cold_track_shader_writes(nvn_gpu_cmd c, const struct nvn_gpu_pipe_s *p,
                                     unsigned stage, unsigned writer) {
    for (unsigned i = 0; i < c->nbinds[stage]; i++) {
        const nvn_gpu_binding *b = &c->binds[stage][i].b;
        if (b->ent >= p->stage_nent[stage] || !(p->stage_ents[stage][b->ent].access & NVN_GPU_ACC_W)) continue;
        if (b->cls == NVN_GPU_RES_SSBO && b->u.b.buf)
            cold_note_write(c, b->u.b.buf, b->u.b.off, b->u.b.len, writer, 0);
        if (b->cls == NVN_GPU_RES_IMAGE_TBUF && b->u.view)
            cold_note_write(c, b->u.view->buf, b->u.view->buffer_off, b->u.view->buffer_bytes, writer, 0);
    }
}

static uint64_t cold_replay_events(struct nvn_gpu_cmd_s *c, uint64_t base, nvn_gpu_buf buffer,
                                   struct cold_ledger *target) {
    if (!c) return base;
    for (unsigned i = 0; i < c->cold_nevents; i++) {
        const struct cold_event *e = &c->cold_events[i];
        if (buffer && e->buf != buffer) continue;
        if (!cold_ledger_write(target ? target : e->buf->cold_ledger, e->off, e->off + e->bytes,
                               base + e->barriers, e->writer, e->exact))
            cold_diag_reject(c, target ? COLD_DIAG_SNAPSHOT_SPAN_LIMIT : COLD_DIAG_COMMITTED_SPAN_LIMIT);
    }
    return base + c->cold_barriers;
}

static void cold_commit_writes(struct nvn_gpu_cmd_s *c) {
    if (c->cold_events_lost) {
        // The current segment might have changed any tracked physical buffer.
        // Keep rendering normally and retire the optional plan for this session.
        g.cold_writes_lost = 1;
        const enum cold_diag_reason reason = c->cold_events_lost == COLD_EVENTS_LIMIT ?
            COLD_DIAG_EVENT_LIMIT : COLD_DIAG_EVENT_OOM;
        cold_diag_reject(c, reason);
        return;
    }
    g.cold_epoch = cold_replay_events(c, g.cold_epoch, NULL, NULL);
}
