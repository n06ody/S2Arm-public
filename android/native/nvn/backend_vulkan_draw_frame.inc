// One replay frame bounded by two actual WSI submissions. The legacy draw export
// used a per-segment pass index and a frame-global draw ordinal at retirement.
// Here publish only after the corresponding queue submits and present succeed.
#include "backend_vulkan_frame_trigger.inc"
enum { FRAME_DRAW_LIMIT = 4096 };
struct frame_draw_row {
    uint8_t request[32], owner[32], module[3][32], library, stages;
    uint32_t pass, choice;
    uint64_t signal;
};
static struct {
    struct frame_identity id;
    uint64_t target_seq;
    uint64_t previous_ticket, previous_wsi, generation;
    nvn_gpu_surface surface;
    struct frame_draw_row *rows;
    uint32_t count, submitted;
    int finished;
    const char *bad;
} frame_capture;
static pthread_mutex_t frame_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int frame_enabled;

int nvn_gpu_draw_observation_enabled(void) { return atomic_load(&frame_enabled)!=0; }

static int frame_digest(const uint8_t *digest);
#include "backend_vulkan_frame_choices.inc"

static void frame_reject(const char *why) {
    if (!frame_capture.bad) frame_capture.bad = why;
}

#include "backend_vulkan_frame_resources.inc"

static void frame_reset(int enable) {
    pthread_mutex_lock(&frame_mu);
    if (frame_capture.rows && !frame_capture.finished)
        nvn_plat_log(NVN_PLAT_LOG_RED, "draw-frame", "missing=present");
    free(frame_capture.rows);
    memset(&frame_capture, 0, sizeof frame_capture);
    fr_reset();
    roi_forget();
    memset(&roi_open_map,0,sizeof roi_open_map);
    free(frame_choices);
    frame_choices = NULL;
    fr_option = enable == 2 || enable == 3;
    int ready = (enable >= 1 && enable <= 3) && frame_identity_read(&frame_capture.id);
    if (enable && !ready) nvn_plat_log(NVN_PLAT_LOG_RED, "draw-frame", "missing=process_identity");
    if (ready && enable==3 && !(frame_choices=calloc(1,sizeof *frame_choices))) frame_reject("choice_allocation");
    atomic_store(&frame_enabled, ready ? enable : 0);
    pthread_mutex_unlock(&frame_mu);
}

// Also covers replay acquire/no-window failures, before vkQueuePresentKHR.
void nvn_gpu_draw_observation_drop(uint64_t ticket) {
    if (!nvn_gpu_draw_observation_enabled()) return;
    pthread_mutex_lock(&frame_mu);
    if (frame_capture.previous_ticket && !frame_capture.finished && ticket >= frame_capture.previous_ticket + 1) {
        frame_reject("present_dropped");
        nvn_plat_log(NVN_PLAT_LOG_RED, "draw-frame", "missing=present_dropped");
        free(frame_capture.rows);
        frame_capture.rows = NULL;
        frame_capture.finished = 1;
    }
    pthread_mutex_unlock(&frame_mu);
}

static void frame_cmd_begin(struct nvn_gpu_cmd_s *c) {
    pthread_mutex_lock(&frame_mu);
    c->observation_ticket = frame_capture.previous_ticket;
    if (fr && frame_capture.rows && !frame_capture.finished) {
        c->resources = calloc(1, sizeof *c->resources);
        if (!c->resources) frame_reject("resource_allocation");
    }
    pthread_mutex_unlock(&frame_mu);
}

static int frame_digest(const uint8_t *digest) {
    static const uint8_t zero[32];
    return digest && memcmp(digest, zero, sizeof zero);
}

static void frame_record(struct nvn_gpu_cmd_s *c, const nvn_gpu_draw_observation *o,
                         struct nvn_gpu_pipe_s *p, int draw_mode, int discard_static) {
    pthread_mutex_lock(&frame_mu);
    if (!frame_capture.rows || frame_capture.finished) goto done;
    if (!o || o->seq != frame_capture.target_seq) { frame_reject("draw_seq"); goto done; }
    if (c->observation_ticket != frame_capture.previous_ticket) { frame_reject("segment_boundary"); goto done; }
    if (draw_mode || !c->observation_pass || !p->pipe || c->es.gpipe != p->pipe ||
        !o->stages || !frame_digest(o->request) || !frame_digest(o->owner)) {
        frame_reject("draw_identity"); goto done;
    }
    if (frame_capture.count == FRAME_DRAW_LIMIT) { frame_reject("draw_limit"); goto done; }
    if (!c->observation_count) c->observation_first = frame_capture.count;
    if (c->observation_first + c->observation_count != frame_capture.count) {
        frame_reject("interleaved_segments"); goto done;
    }
    struct frame_draw_row *r = &frame_capture.rows[frame_capture.count++];
    c->observation_count++;
    r->pass = c->observation_pass - 1;
    memcpy(r->request, o->request, 32);
    memcpy(r->owner, o->owner, 32);
    r->library = o->lib_sourced;
    for (unsigned st = 0; st < 3; st++) {
        nvn_gpu_shader sh = p->key.st[st].sh;
        if (sh != o->stages[st]) { frame_reject("pipeline_module"); goto done; }
        if (st == NVN_GPU_SH_FS && discard_static) continue; // omitted by vkCreateGraphicsPipelines
        if (!sh) continue;
        if (!sh->mod || !frame_digest(sh->spirv_digest)) { frame_reject("module_digest"); goto done; }
        r->stages |= 1u << st;
        memcpy(r->module[st], sh->spirv_digest, 32);
    }
    if (frame_choices) r->choice=frame_draw_choice(o,p->key.st[NVN_GPU_SH_FS].sh);
    if (c->resources) fr_draw(c,p,frame_capture.count-1,discard_static);
    if (!(r->stages & 1) || p->key.st[NVN_GPU_SH_TCS].sh || p->key.st[NVN_GPU_SH_TES].sh)
        frame_reject("untracked_stage");
done:
    pthread_mutex_unlock(&frame_mu);
}

static void frame_submit(struct nvn_gpu_cmd_s *c, uint64_t signal) {
    pthread_mutex_lock(&frame_mu);
    fr_commit(c,signal);
    if (frame_capture.rows && !frame_capture.finished && c->observation_count) {
        if (!signal || frame_capture.submitted != c->observation_first ||
            c->observation_ticket != frame_capture.previous_ticket) frame_reject("segment_submit");
        else for (uint32_t i = 0; i < c->observation_count; i++)
            frame_capture.rows[frame_capture.submitted++].signal = signal;
    }
    pthread_mutex_unlock(&frame_mu);
}

static int frame_write(uint64_t seq, uint64_t ticket, uint64_t wsi, const nvn_gpu_drawable *dr,
                       int mode, uint32_t timing_id, char leaf[192], struct frame_write_error *error) {
    const struct frame_identity *id = &frame_capture.id;
    char path[4096], final[4096];
    snprintf(leaf, 192, "%s-%s.done", id->stem, id->nonce);
    if (!frame_path(final, leaf) || !frame_path(path, ".sf-part-XXXXXX"))
        return frame_fail(error, "path", errno);
    int fd = mkstemp(path);
    if (fd < 0) return frame_fail(error, "mkstemp", errno);
    FILE *f = fdopen(fd, "w+");
    if (!f) {
        frame_fail(error, "fdopen", errno);
        close(fd);
        unlink(path);
        return 0;
    }
    int ok = fprintf(f, "v=%u pid=%ld birth=%llu session=%s nonce=%s seq=%llu ticket=%llu wsi=%llu gen=%llu token=%llu mode=%d timing_id=%u "
                     "queue_present=accepted display=unproven rows=%u\n",
                     frame_choices ? 5u : fr ? (fr->partial ? 4u : 3u) : 2u, id->pid, id->birth, id->session, id->nonce,
                     (unsigned long long)seq, (unsigned long long)ticket, (unsigned long long)wsi,
                     (unsigned long long)dr->gen, (unsigned long long)dr->token,
                     mode, timing_id, frame_capture.count) >= 0;
    ok = ok && fputs("seq\tsubmit\tpass_in_submit\tdraw\trequest\towner\tlibrary\tvs\tfs\tgs\n", f) >= 0;
    for (uint32_t i = 0; ok && i < frame_capture.count; i++) {
        const struct frame_draw_row *r = &frame_capture.rows[i];
        char request[65], owner[65], stage[3][65] = {"-", "-", "-"};
        s2a_sha256_hex(r->request, request);
        s2a_sha256_hex(r->owner, owner);
        for (unsigned st = 0; st < 3; st++)
            if (r->stages & (1u << st)) s2a_sha256_hex(r->module[st], stage[st]);
        ok = fprintf(f, "%llu\t%llu\t%u\t%u\t%s\t%s\t%u\t%s\t%s\t%s\n",
                     (unsigned long long)seq, (unsigned long long)r->signal, r->pass, i,
                     request, owner, r->library, stage[0], stage[1], stage[2]) >= 0;
    }
    if (!ok) frame_fail(error, "emit_draws", errno);
    else if (!frame_emit_choices(f)) frame_fail(error, "emit_choices", errno);
    else if (!fr_emit(f)) frame_fail(error, "emit_resources", errno);
    else if (!roi_emit(f)) frame_fail(error, "emit_roi", errno);
    else frame_seal(f, id, (frame_choices ? 16u : 4u)*1024*1024, error);
    if (fclose(f) && !error->stage) frame_fail(error, "fclose", errno);
    if (!error->stage && frame_publish(path, final, error)) return 1;

    // fclose has released/flushed stdio. Preserve the first errno across cleanup.
    struct stat st;
    if (!stat(path, &st)) error->bytes = (size_t)st.st_size;
    unlink(path);
    return 0;
}

static void frame_present(nvn_gpu_surface s, const nvn_gpu_drawable *dr, uint64_t seq, uint64_t ticket,
                          uint64_t wsi, int accepted, int mode, uint32_t timing_id) {
    pthread_mutex_lock(&frame_mu);
    if (!atomic_load(&frame_enabled) || frame_capture.finished) goto done;
    if (!frame_capture.previous_ticket) {
        if (!accepted || !ticket || !dr->gen || !dr->token || seq == UINT64_MAX) goto done;
        int claimed = frame_claim(&frame_capture.id);
        if (claimed < 0) {
            frame_capture.finished = 1;
            nvn_plat_log(NVN_PLAT_LOG_RED, "draw-frame", "missing=trigger_identity");
        }
        if (claimed != 1) goto done;
        if (fr_option && !(fr=calloc(1,sizeof *fr))) frame_reject("resource_allocation");
        if (frame_choices) roi_load(&frame_capture.id);
        frame_capture.target_seq = seq + 1;
        frame_capture.previous_ticket = ticket;
        frame_capture.previous_wsi = wsi;
        frame_capture.generation = dr->gen;
        frame_capture.surface = s;
        frame_capture.rows = calloc(FRAME_DRAW_LIMIT, sizeof *frame_capture.rows);
        if (!frame_capture.rows) frame_reject("allocation");
        goto done;
    }
    uint64_t target = frame_capture.target_seq;
    if (!frame_capture.previous_ticket || seq != target || frame_capture.previous_ticket + 1 != ticket ||
        frame_capture.previous_wsi + 1 != wsi || frame_capture.surface != s || frame_capture.generation != dr->gen ||
        !dr->token || !accepted) frame_reject("present_identity");
    if (!frame_capture.count || frame_capture.count != frame_capture.submitted) frame_reject("unsubmitted_draws");
    if (fr && (!fr->n || fr->events[fr->n-1].op != FR_PRESENT)) frame_reject("present_source");
    char leaf[192], line[320];
    struct frame_write_error error = {0};
    if (!frame_capture.bad && !frame_write(seq, ticket, wsi, dr, mode, timing_id, leaf, &error))
        frame_reject("write");
    if (error.stage) snprintf(line, sizeof line, "seq=%llu missing=write stage=%s errno=%d bytes=%zu",
                              (unsigned long long)target, error.stage, error.err, error.bytes);
    else if (frame_capture.bad) snprintf(line, sizeof line, "seq=%llu missing=%s", (unsigned long long)target, frame_capture.bad);
    else snprintf(line, sizeof line, "seq=%llu file=%s rows=%u display=unproven", (unsigned long long)seq, leaf, frame_capture.count);
    nvn_plat_log(frame_capture.bad ? NVN_PLAT_LOG_RED : NVN_PLAT_LOG_INFO, "draw-frame", line);
    free(frame_capture.rows);
    frame_capture.rows = NULL;
    frame_capture.finished = 1;
    fr_reset();
    if (roi && roi->buffer) nvn_gpu_buf_release(roi->buffer,NULL,NULL);
    roi_forget();
done:
    pthread_mutex_unlock(&frame_mu);
}
