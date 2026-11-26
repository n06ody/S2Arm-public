// SPDX-License-Identifier: Apache-2.0
// Optional diagnostic hooks. Other GPU backends need no implementations.
#pragma once
extern int nvn_vk_history_enabled(void) __attribute__((weak));
extern void nvn_vk_history_event(nvn_gpu_cmd, nvn_gpu_tex, const char *, ...)
    __attribute__((weak, format(printf, 3, 4)));
#define RP_HISTORY_ON() (nvn_vk_history_enabled && nvn_vk_history_enabled())
#define RP_HISTORY(cmd, tex, ...) do { if (nvn_vk_history_event && RP_HISTORY_ON()) \
    nvn_vk_history_event(cmd, tex, __VA_ARGS__); } while (0)

extern unsigned long long nvn_vk_history_request(nvn_gpu_cmd) __attribute__((weak));
static inline void rp_history_texture(nvn_gpu_cmd c, nvn_gpu_tex g, const tex_t *t,
                                      uint64_t generation, const char *event) {
    if (t && t->d.target == NVN_TEXTURE_TARGET_3D)
        RP_HISTORY(c,g,"event=texture action=%s serial=%llu generation=%llu pool=%p backing=%p "
                   "pool_bytes=%llu offset=%lld bytes=%llu fmt=%u size=%u,%u,%u flags=%u",
                   event,(unsigned long long)t->serial,(unsigned long long)generation,(void *)t->pool,
                   t->pool ? (void *)t->pool->mem : NULL,(unsigned long long)(t->pool ? t->pool->size : 0),
                   (long long)t->off,(unsigned long long)t->L.size,(unsigned)t->d.format,
                   (unsigned)t->d.width,(unsigned)t->d.height,(unsigned)t->d.depth,(unsigned)t->d.flags);
}
