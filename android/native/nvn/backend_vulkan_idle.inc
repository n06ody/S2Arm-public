static pthread_mutex_t g_idle_mu = PTHREAD_MUTEX_INITIALIZER;

static _Atomic int g_idle_active;

// A staged landing can supply an address in a different allocation from its pool mirror.
// Use the selected allocation's flags, including coherent-but-uncached fallback types.
static int idle_contains(nvn_gpu_buf b, const void *p) {
    return b && b->mapped && (uintptr_t)p - (uintptr_t)b->mapped < b->bytes;
}
static void idle_note_cpu(enum nvn_gpu_idle_site site, nvn_gpu_buf hint, const void *source, uint64_t bytes, uint64_t ns) {
    enum idle_cache_class cache = IDLE_UNKNOWN;
    if (source) {
        if (idle_contains(hint, source)) {
            cache = hint->memory_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? IDLE_CACHED : IDLE_UNCACHED;
        } else {
            pthread_mutex_lock(&g_mu);
            for (nvn_gpu_buf b = g.bufs; b; b = b->all_next) {
                if (!idle_contains(b, source)) continue;
                cache = b->memory_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? IDLE_CACHED : IDLE_UNCACHED;
                break;
            }
            pthread_mutex_unlock(&g_mu);
        }
    }
    pthread_mutex_lock(&g_idle_mu);
    if (atomic_load_explicit(&g_idle_active, memory_order_relaxed)) idle_cpu_note(&g.idle.bucket, site, cache, bytes, ns);
    pthread_mutex_unlock(&g_idle_mu);
}
const struct nvn_gpu_idle_observer *nvn_gpu_idle_observer(void) {
    static const struct nvn_gpu_idle_observer o = {now_ns, idle_note_cpu};
    return atomic_load_explicit(&g_idle_active, memory_order_relaxed) ? &o : NULL;
}

static uint64_t resident_clock(void) {
#ifdef CLOCK_BOOTTIME
    struct timespec ts;
    if (!clock_gettime(CLOCK_BOOTTIME, &ts)) return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
    return 0;
#else
    return now_ns();
#endif
}

// Called by the completion worker or after joining it on close. /proc I/O is outside g_mu.
static void resident_emit(const char *reason) {
    uint64_t stamp = resident_clock();
    g.resident_last = stamp;
    pthread_mutex_lock(&g_mu);
    unsigned graphics = g_pipe_cnt, compute = g.compute_resident;
    uint64_t warm = nvn_gpu_pipe_warm_created() - g.warm_created_base;
    pthread_mutex_unlock(&g_mu);
    long rss = -1, maps = -1;
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    if (f) {
        while (fgets(line, sizeof line, f)) if (sscanf(line, "VmRSS: %ld kB", &rss) == 1) break;
        fclose(f);
    }
    f = fopen("/proc/self/maps", "r");
    if (f) {
        char buf[8192];
        size_t n;
        maps = 0;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; i++) maps += buf[i] == '\n';
        if (ferror(f)) maps = -1;
        fclose(f);
    }
#endif
#ifdef CLOCK_BOOTTIME
    const char *clock_name = "BOOTTIME";
#else
    const char *clock_name = "MONOTONIC";
#endif
    char msg[256];
    snprintf(msg, sizeof msg, "管线常驻 reason=%s clock=%s ts_ns=%llu graphics=%u compute=%u warm_built=%llu VmRSS_kB=%ld maps=%ld",
             reason, clock_name, (unsigned long long)stamp, graphics, compute, (unsigned long long)warm, rss, maps);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", msg);
}


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
    snprintf(msg, sizeof msg, "GPU 空闲账 dt_s=%.3f end_mono_ns=%llu bits=%u cal_ext=%d frames=%llu samples=%llu missing=%llu ambiguous=%llu "
             "gap_count/ms intra=%llu/%.3f inter=%llu/%.3f overlap=%llu "
             "cpu_wait_count/ms finish=%llu/%.3f sync=%llu/%.3f read_prior=%llu/%.3f read_copy=%llu/%.3f "
             "wsi_slot=%llu/%.3f wsi_drain=%llu/%.3f other=%llu/%.3f submit_call=%llu/%.3f",
             seconds, (unsigned long long)now, g.idle_bits, !!g.dev->calibrated_timestamps, (unsigned long long)b.frames, (unsigned long long)b.samples,
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
    snprintf(msg, sizeof msg, "GPU 映射读账 end_mono_ns=%llu rmu_busy_count/ms reg=%llu/%.3f view=%llu/%.3f resolve=%llu/%.3f "
             "bc_readback_cpu=NA bc_gpu_record_count/bytes=%llu/%llu frame_log_cpu_map=NA",
             (unsigned long long)now,
#define LOCK_PAIR(site) (unsigned long long)b.lock_count[site], b.lock_ns[site] / 1e6
             LOCK_PAIR(NVN_IDLE_REG_LOCK), LOCK_PAIR(NVN_IDLE_VIEW_LOCK), LOCK_PAIR(NVN_IDLE_RESOLVE_LOCK),
#undef LOCK_PAIR
             (unsigned long long)b.bc_copies, (unsigned long long)b.bc_bytes);
    size_t pos = strlen(msg);
    const char *sites[] = {"land", "ReadTexels"};
    const char *classes[] = {"cached", "uncached", "unknown"};
    for (unsigned site = 0; site < NVN_IDLE_SITES - NVN_IDLE_LAND; site++) {
        for (unsigned c = 0; c < IDLE_CACHE_CLASSES; c++) {
            const struct idle_read *r = &b.read[site][c];
            int n = snprintf(msg + pos, sizeof msg - pos, " %s.%s=%llu/%llu/%.3f", sites[site], classes[c],
                             (unsigned long long)r->count, (unsigned long long)r->bytes, r->ns / 1e6);
            if (n < 0 || (size_t)n >= sizeof msg - pos) return;
            pos += (size_t)n;
        }
    }
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", msg);
    if (g.land_cached) {
        // 累计量由相邻窗口做差，与上面的有效落地字节口径相同。
        pthread_mutex_lock(&g_mu);
        snprintf(msg, sizeof msg, "缓存镜像累计 end_mono_ns=%llu "
                 "flush_calls/payload/aligned/ms=%llu/%llu/%llu/%.3f "
                 "invalidate_calls/payload/aligned/ms=%llu/%llu/%llu/%.3f wait_calls/ms=%llu/%.3f",
                 (unsigned long long)now,
                 (unsigned long long)g.stats[STAT_MIRROR_FLUSH_N],
                 (unsigned long long)g.stats[STAT_MIRROR_FLUSH_PAYLOAD],
                 (unsigned long long)g.stats[STAT_MIRROR_FLUSH_BYTES], g.stats[STAT_MIRROR_FLUSH_NS] / 1e6,
                 (unsigned long long)g.stats[STAT_MIRROR_INV_N],
                 (unsigned long long)g.stats[STAT_MIRROR_INV_PAYLOAD],
                 (unsigned long long)g.stats[STAT_MIRROR_INV_BYTES], g.stats[STAT_MIRROR_INV_NS] / 1e6,
                 (unsigned long long)g.stats[STAT_MIRROR_WAIT_N], g.stats[STAT_MIRROR_WAIT_NS] / 1e6);
        pthread_mutex_unlock(&g_mu);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "land-cached", msg);
    }
}
