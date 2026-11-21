#ifndef S2A_RP_PASS_OR_PROBE_H
#define S2A_RP_PASS_OR_PROBE_H

#include "nvn_backend.h"

int nvn_vk_pass_or_select(nvn_gpu_shader fs, uint32_t vertices, uint32_t instances,
                          uint32_t w, uint32_t h, uint32_t colors, uint32_t slots[3]);
void nvn_vk_pass_or_preflight(nvn_gpu_cmd cmd, nvn_gpu_buf buf, uint64_t off,
                              uint64_t len, uint32_t prefix, int prefix_known, int natural_boundary);

#endif
