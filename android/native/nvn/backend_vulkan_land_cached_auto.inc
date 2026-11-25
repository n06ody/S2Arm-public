// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

extern int dm_thermal_now(void) __attribute__((weak));
extern int s2a_android_host_lifecycle(uint64_t *changes) __attribute__((weak));

static void mirror_auto_compatibility(uint8_t digest[32]) {
    const VkPhysicalDeviceProperties *p = &g.dev->properties;
    const VkPhysicalDeviceMemoryProperties *m = &g.dev->memory;
    const int flags[] = {nvn_plat_flag("nvk_mem_cached", -1), nvn_plat_flag("nvk_sync_lag", 0)};
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    s2a_sha256_update(&hash, p->pipelineCacheUUID, sizeof p->pipelineCacheUUID);
    s2a_sha256_update(&hash, &p->limits.nonCoherentAtomSize, sizeof p->limits.nonCoherentAtomSize);
    s2a_sha256_update(&hash, m->memoryTypes, m->memoryTypeCount * sizeof *m->memoryTypes);
    s2a_sha256_update(&hash, flags, sizeof flags);
    s2a_sha256_final(&hash, digest);
}

static int mirror_auto_take(const char *path, struct mirror_auto_receipt *result) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    // 消费一次；未完成实证的 B 会话不能让下一次启动继续试 B。
    struct stat st;
    struct mirror_auto_receipt receipt;
    const int read_ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == (off_t)sizeof receipt &&
                        read(fd, &receipt, sizeof receipt) == (ssize_t)sizeof receipt;
    close(fd);
    if (unlink(path) || !read_ok) return 0;
    const time_t now = time(NULL);
    if (now < 0 || !mirror_auto_receipt_valid(&receipt, result->compatibility, (uint64_t)now)) return 0;
    *result = receipt;
    return 1;
}

static void mirror_auto_open(const char *cache_dir) {
    struct mirror_auto_session *a = &g.mirror_auto;
    a->lock_fd = -1;
    atomic_init(&a->finished, 0);
    if (!g.land_cached_auto || !cache_dir || !*cache_dir) return;
    // R 按段改变真正落地的源和 DMA。R 手工或自动在用时不发生第二个试验。
    if (nvn_plat_flag("nvk_land_readback", 0)) {
        nvn_plat_log(NVN_PLAT_LOG_INFO, "land-cached", "缓存镜像自动：回读暂存已配置，沿用原映射");
        return;
    }
    const int n = snprintf(a->path, sizeof a->path, "%s/land-cached-trial.v1", cache_dir);
    char lock[sizeof a->path + 8];
    if (n < 0 || n >= (int)sizeof a->path) return;
    snprintf(lock, sizeof lock, "%s.lock", a->path);
    a->lock_fd = open(lock, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (a->lock_fd < 0) return;
    if (flock(a->lock_fd, LOCK_EX | LOCK_NB)) {
        close(a->lock_fd);
        a->lock_fd = -1;
        return;
    }
    a->receipt.version = MIRROR_AUTO_VERSION;
    mirror_auto_compatibility(a->receipt.compatibility);
    a->trial = mirror_auto_take(a->path, &a->receipt);
    g.mirror_shadow_mode = !a->trial;
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_MIRROR_AUTO_TRIAL_OPEN] = a->trial;
    pthread_mutex_unlock(&g_mu);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-cached", a->trial
        ? "缓存镜像自动：本次启动配对试测"
        : "缓存镜像自动：原映射，观察相交原子");
}

struct mirror_auto_save {
    int lock_fd;
    char path[512];
    struct mirror_auto_receipt receipt;
};

static void *mirror_auto_save_worker(void *arg) {
    struct mirror_auto_save *save = arg;
    char tmp[sizeof save->path + 16];
    snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", save->path);
    const int fd = mkstemp(tmp);
    if (fd >= 0) {
        const int ok = write(fd, &save->receipt, sizeof save->receipt) == (ssize_t)sizeof save->receipt && fsync(fd) == 0;
        const int closed = close(fd) == 0;
        if (!ok || !closed || rename(tmp, save->path)) unlink(tmp);
        else nvn_plat_log(NVN_PLAT_LOG_INFO, "land-cached", "缓存镜像自动：试测凭证已写入");
    }
    close(save->lock_fd);
    free(save);
    return NULL;
}

static void mirror_auto_save(const struct mirror_auto_session *a) {
    const time_t now = time(NULL);
    if (now < 0) return;
    struct mirror_auto_save *save = malloc(sizeof *save);
    if (!save) return;
    save->lock_fd = dup(a->lock_fd);   // 关后端时本线程仍持有串行文件锁。
    if (save->lock_fd < 0) { free(save); return; }
    memcpy(save->path, a->path, sizeof save->path);
    save->receipt = a->receipt;
    save->receipt.created = (uint64_t)now;
    pthread_t thread;
    if (pthread_create(&thread, NULL, mirror_auto_save_worker, save)) {
        close(save->lock_fd);
        free(save);
        return;
    }
    pthread_detach(thread);
}

void nvn_gpu_mirror_auto_sample(const struct rp_mirror_auto_sample *s) {
    struct mirror_auto_session *a = &g.mirror_auto;
    if (!g.land_cached_auto || a->lock_fd < 0 || atomic_load(&a->finished) || !s->frames) return;
    const double f = (double)s->frames;
    uint64_t life = 0;
    const int foreground = s2a_android_host_lifecycle ? s2a_android_host_lifecycle(&life) : -1;
    const int thermal = dm_thermal_now ? dm_thermal_now() : -1;
    pthread_mutex_lock(&g_mu);
    const uint64_t shadow_ns = g.stats[STAT_MIRROR_AUTO_SHADOW_NS] +
        (g.mirror_shadow_fence ? now_ns() - g.mirror_shadow_start_ns : 0);
    const uint64_t shadow_bytes = g.stats[STAT_MIRROR_AUTO_SHADOW_BYTES];
    const uint64_t missed = g.stats[STAT_MIRROR_AUTO_SHADOW_MISSED];
    const uint64_t maintenance = g.stats[STAT_MIRROR_FLUSH_NS] + g.stats[STAT_MIRROR_INV_NS];
    const uint64_t wait = g.stats[STAT_MIRROR_WAIT_NS];
    const int eligible = !g.closing && !g.failed && !g.device_lost && !a->allocation_failed &&
        (a->trial ? g.stats[STAT_MIRROR_AUTO_TRIAL_BUFFERS] != 0 : atomic_load(&g.mirror_shadow_seen));
    pthread_mutex_unlock(&g_mu);
    const uint64_t measured_bytes = shadow_bytes - a->last_shadow_bytes;
    const int missed_window = missed != a->last_missed;
    const struct mirror_auto_window w = {
        .frame_ms = s->elapsed_ns / 1e6 / f, .p99_ms = s->p99_ms,
        .present_ms = s->present_ns / 1e6 / f, .acquire_ms = s->acquire_ns / 1e6 / f,
        .land_bytes = (s->uncached_bytes + s->cached_bytes) / f, .draws = s->draws / f, .submits = s->submits / f,
        .sync_bytes = 1024.0 * s->sync_kib / f, .target_ns = s->target_ns, .thermal = thermal,
        .copy_ms = (s->uncached_ns + s->cached_ns) / 1e6 / f,
        .shadow_wait_ms = (shadow_ns - a->last_shadow_ns) / 1e6 / f,
        .maintenance_ms = (maintenance - a->last_maintenance_ns) / 1e6 / f,
        .wait_ms = (wait - a->last_wait_ns) / 1e6 / f,
    };
    const int steady = a->window_started && a->life == life && a->thermal == thermal;
    a->life = life;
    a->thermal = thermal;
    a->window_started = 1;
    a->last_shadow_ns = shadow_ns;
    a->last_shadow_bytes = shadow_bytes;
    a->last_missed = missed;
    a->last_maintenance_ns = maintenance;
    a->last_wait_ns = wait;
    if (!eligible) {
        a->n = 0;
        return;
    }

    const char *reject = NULL;
    if (!steady || foreground != 1 || thermal < 0 || s->frames < 50 || s->elapsed_ns > 6000000000ull ||
        s->compiling || s->target_changed || !mirror_auto_valid(&w) || w.land_bytes < 64 * 1024) {
        reject = "window_unqualified";
    } else if (!a->trial) {
        // 证据覆盖不足或大批 CPU 直写可能改走暂存时，影子账太乐观，放弃该窗。
        if (missed_window || measured_bytes < 0.9 * (s->uncached_bytes + s->cached_bytes) ||
            s->uncached_bytes < 0.9 * (s->uncached_bytes + s->cached_bytes)) reject = "shadow_coverage";
        else if (s->direct_kib > fmin(64.0 * f, 0.01 * s->sync_kib)) reject = "unmodeled_direct_upload";
        else if (!mirror_auto_screen(&w)) reject = "insufficient_headroom";
    } else if (s->cached_bytes < 0.9 * (s->uncached_bytes + s->cached_bytes) ||
               !mirror_auto_match(&a->receipt.baseline[a->n], &w)) {
        reject = "trial_workload";
    }
    if (!reject && a->n && !mirror_auto_match(&a->windows[0], &w)) reject = "window_workload";
    char window_line[400];
    snprintf(window_line, sizeof window_line,
             "缓存镜像自动采样 phase=%s result=%s frames=%llu thermal=%d target_ns=%llu "
             "frame_ms=%.3f p99_ms=%.3f land_bytes=%.0f shadow_bytes=%.0f direct_kib=%.2f "
             "copy_ms=%.3f shadow_wait_ms=%.3f maintenance_ms=%.3f wait_ms=%.3f",
             a->trial ? "trial" : "observe", reject ? reject : "eligible",
             (unsigned long long)s->frames, thermal, (unsigned long long)w.target_ns, w.frame_ms, w.p99_ms,
             w.land_bytes, measured_bytes / f, s->direct_kib / f,
             w.copy_ms, w.shadow_wait_ms, w.maintenance_ms, w.wait_ms);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-cached", window_line);
    if (reject) {
        a->n = 0;
        return;
    }
    a->windows[a->n++] = w;
    if (a->n < 2) return;
    atomic_store(&a->finished, 1);

    const int keep = !a->trial || mirror_auto_accept(a->receipt.baseline, a->windows);
    char line[320];
    snprintf(line, sizeof line, "缓存镜像自动决策 phase=%s result=%s frame_ms=%.3f/%.3f "
             "copy_ms=%.3f/%.3f shadow_wait_ms=%.3f/%.3f wait_ms=%.3f/%.3f",
             a->trial ? "trial" : "observe", keep ? (a->trial ? "paired_gain" : "trial_next_open") : "no_paired_gain",
             a->windows[0].frame_ms, a->windows[1].frame_ms, a->windows[0].copy_ms, a->windows[1].copy_ms,
             a->windows[0].shadow_wait_ms, a->windows[1].shadow_wait_ms, a->windows[0].wait_ms, a->windows[1].wait_ms);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-cached", line);
    if (!keep) return;
    if (!a->trial) memcpy(a->receipt.baseline, a->windows, sizeof a->windows);
    mirror_auto_save(a);
}
