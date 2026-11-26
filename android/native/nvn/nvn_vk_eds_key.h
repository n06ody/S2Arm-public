// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string.h>
#include "nvn_backend.h"

static inline uint32_t nvk_eds_prim(const nvn_gpu_pipe_desc *d, uint32_t prim) {
    // 保留 GS 的既有图元约束以及邻接、点和补丁的独立键。
    if (!d->extended_dynamic || d->st[NVN_GPU_SH_GS].sh) return prim;
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return NVN_DRAW_PRIMITIVE_LINES;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return NVN_DRAW_PRIMITIVE_TRIANGLES;
    default: return prim;
    }
}

static inline void nvk_eds_normalize(nvn_gpu_pipe_desc *d) {
    if (!d->extended_dynamic) return;
    d->prim = nvk_eds_prim(d, d->prim);
    d->prim_restart = 0;
    d->rast_discard = 0;
    memset(&d->ds, 0, sizeof d->ds);
    d->poly.cull = 0;
    d->poly.front = 0;
    d->poly.offset_en = 0;
}
