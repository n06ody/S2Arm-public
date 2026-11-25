// SPDX-License-Identifier: Apache-2.0
// Local experiment only. PROT_NONE can make kernel reads of these buffers
// return EFAULT without a signal (see t_land_probe.c). An expired sample only
// means no qualifying first userspace fault was observed for that page/version.
// write_first includes write-direction atomic operations; it does not exclude RMW.
#include "nvn_land_probe.h"
extern __typeof__(nvn_probe_before) nvn_probe_before __attribute__((weak));
extern __typeof__(nvn_probe_after) nvn_probe_after __attribute__((weak));
extern __typeof__(nvn_probe_drain) nvn_probe_drain __attribute__((weak));
static int sentinel_enabled = -1;
static int sentinel_on(void) {
    if (sentinel_enabled < 0) {
        sentinel_enabled = nvn_plat_flag("nvk_land_sentinel", 0) == 1;
        if (sentinel_enabled && (!nvn_probe_before || !nvn_probe_after || !nvn_probe_drain)) {
            nvn_plat_log(NVN_PLAT_LOG_RED, "land-sentinel", "requested page probe unavailable");
            sentinel_enabled = 0;
        }
    }
    return sentinel_enabled;
}
static void sentinel_start(struct shadow *s, struct gpu_write *g) {
    if (!s->origin_pool) s->origin_pool = ++origin_next_pool;
    const uintptr_t ps = nvn_dirty_page_size(), a = (uintptr_t)s->mem + g->off;
    for (int i = 0; i < s->ngw; i++) {
        struct gpu_write *other = &s->gw[i];
        if (!other->landing) continue;
        uintptr_t b = (uintptr_t)s->mem + other->off;
        if (a / ps <= (b + other->len - 1) / ps && b / ps <= (a + g->len - 1) / ps) {
            other->probe_overlap = g->probe_overlap = 1;
        }
    }
    nvn_probe_before((const void *)a, g->len, s->origin_pool, g->id);
}
static void sentinel_emit(const struct nvn_probe_sample *s) {
    static const char *const names[] = {"pending", "guest_read", "host_read", "write_first", "next_version",
        "expired", "alias-overlap", "neighbor", "unavailable"};
    char line[256];
    snprintf(line, sizeof line, "pool=%" PRIu64 " write=%" PRIu64 " off=%" PRIu64 " bytes=%" PRIu64
             " sub=%" PRIu64 " ops=%x stages=%x slots=%x page_index=%" PRIu64 " pages=%" PRIu64 " armed=%u result=%s", s->pool, s->write, s->offset,
             s->bytes, s->source.submission, s->source.ops, s->source.stages, s->source.slots, s->page_index, s->pages, s->armed,
             s->result < sizeof names / sizeof *names ? names[s->result] : "unknown");
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-sentinel", line);
}
static void sentinel_report(void) {
    if (sentinel_enabled != 1) return;
    uint64_t dropped = nvn_probe_drain(sentinel_emit);
    char line[96];
    snprintf(line, sizeof line, "skipped_full=%" PRIu64, dropped);
    if (dropped == UINT64_MAX) nvn_plat_log(NVN_PLAT_LOG_RED, "land-sentinel", "could not restore all sampled pages");
    else nvn_plat_log(NVN_PLAT_LOG_INFO, "land-sentinel", line);
}
