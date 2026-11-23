static pthread_mutex_t g_idle_mu = PTHREAD_MUTEX_INITIALIZER;

// Called with g_mu held on entry and return. Only actual condition-variable sleeps are charged.
static int idle_cond_wait(enum nvn_gpu_wait_site site, const struct timespec *deadline, int *first) {
    if (!g.idle_on) return pthread_cond_timedwait(&g_cv, &g_mu, deadline);
    uint64_t begin = now_ns();
    int r = pthread_cond_timedwait(&g_cv, &g_mu, deadline);
    uint64_t elapsed = now_ns() - begin;
    pthread_mutex_lock(&g_idle_mu);
    idle_wait(&g.idle.bucket, site, elapsed, *first);
    pthread_mutex_unlock(&g_idle_mu);
    *first = 0;
    return r;
}

// All tracked submissions are enqueued and assigned serials under g_submit_mu.
// Unmeasured WSI maintenance also advances the serial, breaking adjacency.
static VkResult idle_submit(struct vk_segment *s, const VkSubmitInfo *info, VkFence fence) {
    if (!g.idle_on) return vkQueueSubmit(g.queue, 1, info, fence);
    uint64_t begin = now_ns();
    VkResult r = vkQueueSubmit(g.queue, 1, info, fence);
    uint64_t elapsed = now_ns() - begin;
    if (r == VK_SUCCESS) {
        ++g.idle_serial;
        if (s) { s->idle_serial = g.idle_serial; s->idle_submit_begin = begin; }
    }
    pthread_mutex_lock(&g_idle_mu);
    g.idle.bucket.submit_calls++;
    g.idle.bucket.submit_ns += elapsed;
    pthread_mutex_unlock(&g_idle_mu);
    return r;
}

static void idle_complete(struct vk_segment *s, int success) {
    struct nvn_gpu_cmd_s *c = s->cmd;
    uint64_t start = 0, end = 0;
    int valid = success && c && c->tsq && g.idle_bits;
    if (valid) {
        uint64_t pair[2];
        valid = vkGetQueryPoolResults(g.d, c->tsq, 0, 2, sizeof pair, pair, sizeof *pair,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
        if (valid) {
            start = pair[0]; end = pair[1];
            if (c->idle_first)
                valid = vkGetQueryPoolResults(g.d, c->tsq, c->idle_first, 1, sizeof start, &start,
                                              sizeof start, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
        }
    }
    struct idle_bucket b;
    const uint64_t now = now_ns();
    const double period = g.dev->properties.limits.timestampPeriod;
    pthread_mutex_lock(&g_idle_mu);
    if (s->submitted && s->idle_serial != g.idle_done_serial + 1) {
        g.idle.have_end = 0;
        g.idle.bucket.missing += s->idle_serial - g.idle_done_serial - 1;
    }
    if (s->submitted) g.idle_done_serial = s->idle_serial;
    idle_sample(&g.idle, g.idle_bits, period, start, end, s->idle_submit_begin, now, valid, s->internal && success);
    double seconds = (now - g.idle.window_start) / 1e9;
    int emit = idle_take(&g.idle, now, &b);
    pthread_mutex_unlock(&g_idle_mu);
    if (!emit) return;
    char msg[800];
    snprintf(msg, sizeof msg, "GPU 空闲账 dt_s=%.3f bits=%u cal_ext=%d frames=%llu samples=%llu missing=%llu ambiguous=%llu "
             "gap_count/ms intra=%llu/%.3f inter=%llu/%.3f overlap=%llu "
             "cpu_wait_count/ms finish=%llu/%.3f sync=%llu/%.3f read_prior=%llu/%.3f read_copy=%llu/%.3f "
             "wsi_slot=%llu/%.3f wsi_drain=%llu/%.3f other=%llu/%.3f submit_call=%llu/%.3f",
             seconds, g.idle_bits, !!g.dev->calibrated_timestamps, (unsigned long long)b.frames, (unsigned long long)b.samples,
             (unsigned long long)b.missing, (unsigned long long)b.ambiguous,
             (unsigned long long)b.gaps[0], b.ticks[0] * period / 1e6,
             (unsigned long long)b.gaps[1], b.ticks[1] * period / 1e6, (unsigned long long)b.overlap,
#define IDLE_WAIT_PAIR(site) (unsigned long long)b.waits[site], b.wait_ns[site] / 1e6
             IDLE_WAIT_PAIR(NVN_GPU_WAIT_FINISH), IDLE_WAIT_PAIR(NVN_GPU_WAIT_SYNC),
             IDLE_WAIT_PAIR(NVN_GPU_WAIT_READ_PRIOR), IDLE_WAIT_PAIR(NVN_GPU_WAIT_READ_COPY),
             IDLE_WAIT_PAIR(NVN_GPU_WAIT_WSI_SLOT), IDLE_WAIT_PAIR(NVN_GPU_WAIT_WSI_DRAIN),
             IDLE_WAIT_PAIR(NVN_GPU_WAIT_OTHER), (unsigned long long)b.submit_calls, b.submit_ns / 1e6);
#undef IDLE_WAIT_PAIR
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", msg);
}
