// SPDX-License-Identifier: Apache-2.0
// A process-local metadata stream preserves previous generations, including
// aliases that disappeared before the target draw. Never write texture bytes.
static pthread_mutex_t history_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int history_on;
static _Atomic uint64_t history_ids;
static uint64_t history_seq;
#ifdef __ANDROID__
static FILE *history_file;
#endif

static void history_close(void) {
    pthread_mutex_lock(&history_mu);
    atomic_store(&history_on, 0);
#ifdef __ANDROID__
    if (history_file) fclose(history_file);
    history_file = NULL;
#endif
    pthread_mutex_unlock(&history_mu);
}

static void history_open(void) {
    history_close();
    if (!ROI_CONFIGURED || !nvn_plat_flag("nvk_roi_history", 0)) return;
#ifdef __ANDROID__
    const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE);
    char path[1024];
    if (!dir || snprintf(path, sizeof path, "%s/roi-history-%ld-XXXXXX", dir, (long)getpid()) >= (int)sizeof path)
        goto fail;
    int fd = mkstemp(path);
    if (fd < 0) goto fail;
    history_file = fdopen(fd, "w");
    if (!history_file) { close(fd); goto fail; }
    nvn_plat_log(NVN_PLAT_LOG_INFO, "history", path);
#endif
    history_seq = 0;
    atomic_store(&history_on, 1);
    return;
#ifdef __ANDROID__
fail:
    nvn_plat_log(NVN_PLAT_LOG_RED, "history", "event=gap reason=open");
#endif
}

int nvn_vk_history_enabled(void) {
    return atomic_load(&history_on) && atomic_load(&roi_next) <= (unsigned)roi_target;
}

static void history_line(nvn_gpu_cmd c, nvn_gpu_tex t, const char *payload) {
    pthread_mutex_lock(&history_mu);
    if (atomic_load(&history_on)) {
        char line[1024];
        int n = snprintf(line, sizeof line, "%s seq=%llu writer_frame=%u segment=%llu image=%llu",
                         payload, (unsigned long long)++history_seq,
                         c ? c->history_frame : atomic_load(&roi_next),
                         (unsigned long long)(c ? c->history_id : 0),
                         (unsigned long long)(t ? access_id(&t->access_id) : 0));
        if (n < 0 || n >= (int)sizeof line) strcpy(line, "event=history_gap reason=line_overflow");
#ifdef __ANDROID__
        if (fprintf(history_file, "%s\n", line) < 0 ||
            ((history_seq % 128 == 0 || (c && c->history_frame == (unsigned)roi_target)) &&
             fflush(history_file) != 0)) {
            atomic_store(&history_on, 0);
            nvn_plat_log(NVN_PLAT_LOG_RED, "history", "event=gap reason=write");
        }
#else
        nvn_plat_log(NVN_PLAT_LOG_INFO, "roi", line);
#endif
    }
    pthread_mutex_unlock(&history_mu);
}

void nvn_vk_history_event(nvn_gpu_cmd c, nvn_gpu_tex t, const char *fmt, ...) {
    char text[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    history_line(c, t, n < 0 || n >= (int)sizeof text ? "event=history_gap reason=event_overflow" : text);
}

static void history_writer(nvn_gpu_cmd c, nvn_gpu_tex t, const char *kind, uint32_t mip,
                           VkOffset3D pos, VkExtent3D size, const void *src, size_t bytes) {
    if (!c->history_id || t->target != NVN_TEXTURE_TARGET_3D) return;
    const uint8_t *data = src;
    size_t i = 0;
    if (data) while (i < bytes && data[i] == 0) ++i;
    nvn_vk_history_event(c, t, "event=volume_history kind=%s mip=%u origin=%d,%d,%d extent=%u,%u,%u "
                         "source_all_zero=%d first_nonzero=%lld source_bytes=%zu",
                         kind, mip, pos.x,pos.y,pos.z, size.width,size.height,size.depth,
                         data ? (i == bytes) : -1, data && i < bytes ? (long long)i : -1LL, bytes);
}
static void history_finish(nvn_gpu_cmd c, uint64_t signal, const char *result) {
    if (!c || !c->history_id || c->history_finished) return;
    c->history_finished = 1;
    nvn_vk_history_event(c, NULL, "event=history_submit signal=%llu outcome=%s",
                         (unsigned long long)signal, result);
}

unsigned long long nvn_vk_history_request(nvn_gpu_cmd c) {
    c->history_request = atomic_fetch_add(&history_ids, 1) + 1;
    return (unsigned long long)c->history_request;
}
