// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// 只有诊断开着才建查询池。每个槽只用一次，随本段释放。
enum { BC_TIME_CHUNK = 64 };
enum { BC_TIME_RGBA, BC_TIME_ASTC, BC_TIME_PATHS };
enum { BC_TIME_FIRST_FMT = 0x41, BC_TIME_FORMATS = 16 };
struct bc_time_chunk {
    struct bc_time_chunk *next;
    VkQueryPool pool;
    uint32_t count;
    struct {
        uint64_t blocks;
        uint32_t format, path;
    } event[BC_TIME_CHUNK];
};

static const char *const bc_time_path[] = {"rgba8", "astc4x4"};
static pthread_mutex_t g_bc_time_mu = PTHREAD_MUTEX_INITIALIZER;
static struct bc_time_bucket {
    uint64_t count, missing, blocks;
    double ns;
} g_bc_time[BC_TIME_PATHS][BC_TIME_FORMATS];
static uint64_t g_bc_time_start;

static void bc_time_log_window(uint64_t now, int final) {
    if (!g_bc_time_start || (!final && now - g_bc_time_start < UINT64_C(5000000000))) return;
    char line[256];
    for (uint32_t path = 0; path < BC_TIME_PATHS; path++) {
        for (uint32_t i = 0; i < BC_TIME_FORMATS; i++) {
            const struct bc_time_bucket b = g_bc_time[path][i];
            if (!b.count) continue;
            snprintf(line, sizeof line, "summary path=%s format=%#x count=%llu missing=%llu blocks=%llu gpu_ms=%.6f window_ms=%.0f",
                     bc_time_path[path], i + BC_TIME_FIRST_FMT, (unsigned long long)b.count,
                     (unsigned long long)b.missing, (unsigned long long)b.blocks, b.ns / 1e6,
                     (double)(now - g_bc_time_start) / 1e6);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "bc-upload", line);
        }
    }
    uint64_t live_count[BC_TIME_PATHS] = {0}, image[BC_TIME_PATHS] = {0}, shadow[BC_TIME_PATHS] = {0};
    pthread_mutex_lock(&g_mu);
    for (const struct nvn_gpu_tex_s *t = g.texs; t; t = t->all_next) {
        if (!t->bc_decoded) continue;
        const uint32_t path = t->format == VK_FORMAT_ASTC_4x4_UNORM_BLOCK ||
            t->format == VK_FORMAT_ASTC_4x4_SRGB_BLOCK ? BC_TIME_ASTC : BC_TIME_RGBA;
        live_count[path]++;
        image[path] += t->mem_size;
        shadow[path] += t->bc_shadow_mem_size;
    }
    pthread_mutex_unlock(&g_mu);
    for (uint32_t path = 0; path < BC_TIME_PATHS; path++) {
        snprintf(line, sizeof line, "live path=%s textures=%llu image_bytes=%llu shadow_bytes=%llu",
                 bc_time_path[path], (unsigned long long)live_count[path], (unsigned long long)image[path],
                 (unsigned long long)shadow[path]);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "bc-upload", line);
    }
    memset(g_bc_time, 0, sizeof g_bc_time);
    g_bc_time_start = final ? 0 : now;
}

static void bc_time_event(uint32_t path, uint32_t format, uint64_t blocks, int valid, double ns) {
    struct bc_time_bucket *bucket = &g_bc_time[path][format - BC_TIME_FIRST_FMT];
    bucket->count++;
    bucket->blocks += blocks;
    bucket->missing += !valid;
    bucket->ns += ns;
    char line[192];
    if (valid)
        snprintf(line, sizeof line, "dispatch path=%s format=%#x blocks=%llu gpu_ns=%.0f",
                 bc_time_path[path], format, (unsigned long long)blocks, ns);
    else
        snprintf(line, sizeof line, "dispatch path=%s format=%#x blocks=%llu gpu_ns=missing",
                 bc_time_path[path], format, (unsigned long long)blocks);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "bc-upload", line);
}

static void bc_time_open(void) {
    g.bc1_astc_on = nvn_plat_flag("nvk_bc1_astc", 0) != 0;
    g.bc_time_on = nvn_plat_flag("nvk_bc_upload_time", 0) != 0;
    if (!g.bc_time_on) return;
    const VkPhysicalDeviceLimits *limits = &g.dev->properties.limits;
    if (!isfinite(limits->timestampPeriod) || limits->timestampPeriod <= 0) return;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, NULL);
    if (g.dev->queue_family >= n) return;
    VkQueueFamilyProperties *properties = calloc(n, sizeof *properties);
    if (!properties) return;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, properties);
    if (g.dev->queue_family < n) g.bc_time_bits = properties[g.dev->queue_family].timestampValidBits;
    free(properties);
}

static int bc_time_begin(struct nvn_gpu_cmd_s *c, uint32_t path, uint32_t format, uint64_t blocks) {
    if (!g.bc_time_on || format < BC_TIME_FIRST_FMT || format >= BC_TIME_FIRST_FMT + BC_TIME_FORMATS) return -1;
    struct bc_time_chunk *chunk = c->bc_time;
    if (!chunk || chunk->count == BC_TIME_CHUNK) {
        chunk = calloc(1, sizeof *chunk);
        if (!chunk) {
            pthread_mutex_lock(&g_bc_time_mu);
            const uint64_t now = now_ns();
            if (!g_bc_time_start) g_bc_time_start = now;
            bc_time_log_window(now, 0);
            bc_time_event(path, format, blocks, 0, 0);
            pthread_mutex_unlock(&g_bc_time_mu);
            return -1;
        }
        if (g.bc_time_bits && g.bc_time_bits <= 64) {
            const VkQueryPoolCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * BC_TIME_CHUNK,
            };
            if (vkCreateQueryPool(g.d, &info, NULL, &chunk->pool) == VK_SUCCESS)
                vkCmdResetQueryPool(c->cb, chunk->pool, 0, info.queryCount);
        }
        // 保持完成时的输出和派发次序相同。
        if (c->bc_time) c->bc_time->next = chunk;
        else c->bc_first_time = chunk;
        c->bc_time = chunk;
    }
    const uint32_t i = chunk->count++;
    chunk->event[i].format = format;
    chunk->event[i].path = path;
    chunk->event[i].blocks = blocks;
    if (!chunk->pool) return -1;
    // 两个记录点之间只有这笔派发；使用计算阶段，避免用段首尾或图像拷贝的范围。
    vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, chunk->pool, 2 * i);
    return (int)(2 * i + 1);
}

static void bc_time_end(struct nvn_gpu_cmd_s *c, int query) {
    if (query >= 0)
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, c->bc_time->pool, (uint32_t)query);
}

static void bc_time_finish(struct nvn_gpu_cmd_s *c, int completed) {
    if (!g.bc_time_on || !c) return;
    pthread_mutex_lock(&g_bc_time_mu);
    const uint64_t now = now_ns();
    bc_time_log_window(now, 0);
    while (c->bc_first_time) {
        if (!g_bc_time_start) g_bc_time_start = now;
        struct bc_time_chunk *chunk = c->bc_first_time;
        c->bc_first_time = chunk->next;
        uint64_t samples[2 * BC_TIME_CHUNK][2] = {{0}};
        VkResult result = VK_NOT_READY;
        if (completed && chunk->pool)
            result = vkGetQueryPoolResults(g.d, chunk->pool, 0, 2 * chunk->count,
                sizeof samples, samples, sizeof samples[0], VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const uint64_t mask = s2a_ts_valid_mask(g.bc_time_bits);
        for (uint32_t i = 0; i < chunk->count; i++) {
            const int valid = (result == VK_SUCCESS || result == VK_NOT_READY) && samples[2 * i][1] && samples[2 * i + 1][1];
            const uint64_t ticks = valid ? (samples[2 * i + 1][0] - samples[2 * i][0]) & mask : 0;
            bc_time_event(chunk->event[i].path, chunk->event[i].format, chunk->event[i].blocks,
                          valid, (double)ticks * g.dev->properties.limits.timestampPeriod);
        }
        if (chunk->pool) vkDestroyQueryPool(g.d, chunk->pool, NULL);
        free(chunk);
    }
    c->bc_time = NULL;
    pthread_mutex_unlock(&g_bc_time_mu);
}
