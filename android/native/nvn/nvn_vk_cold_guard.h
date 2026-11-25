// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include "nvn_backend.h"
#include "nvn_vk_cold_ledger.h"

// A publication covers one interval of the resolved buffer's current contents.
// The caller must derive it from ordered, same-queue producers and exclude
// concurrent writes and aliasing until the guarded draw is encoded.
int nvn_vk_cold_guard_mode(void);
int nvn_vk_cold_shader_digest(nvn_gpu_shader shader, uint8_t sha[32]);

void nvn_vk_cold_mirror_track(nvn_gpu_buf);
void nvn_vk_cold_mirror_host_write(nvn_gpu_buf, uint64_t off, uint64_t bytes);
struct nvn_vk_cold_publication {
    nvn_gpu_access_span span;
    unsigned writers;
};
// These Vulkan-only operations are for a caller with a validated variant and
// a frozen binding/producer plan. A successful scan leaves its references and
// read-before-write protection in the segment even when its draw is abandoned.
// Call at a natural pass boundary. A false return leaves the original draw
// available, unless recording itself set a sticky error.
#ifdef S2A_NVK_COLD_GUARD_HOST_TEST
int nvn_vk_cold_guard_scan(nvn_gpu_cmd cmd, nvn_gpu_pipe original, nvn_gpu_pipe variant,
                          const nvn_gpu_draw_args *draw, const struct nvn_gpu_cold_domain *domain,
                          const struct nvn_vk_cold_publication *published, uint32_t npublished);
#endif
// Prove the same queue's committed writes, pre segment, and the main
// segment's already encoded prefix. Replay must pin its resolved mirror and
// exclude host overwrites of the footprint through both scans and draws.
int nvn_vk_cold_guard_auto(nvn_gpu_cmd cmd, nvn_gpu_cmd pre, nvn_gpu_pipe original, nvn_gpu_pipe variant,
                          const nvn_gpu_draw_args *draw, const struct nvn_gpu_cold_domain *domain);
// After setting up the original draw's pass, pipeline and bindings, consume
// the most recent scan once. False means the caller should record the original.
int nvn_vk_cold_guard_draw(nvn_gpu_cmd cmd, const nvn_gpu_draw_args *draw);
