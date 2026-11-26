// SPDX-License-Identifier: Apache-2.0
// Android Vulkan replay extension. Other backends need not provide these hooks.
#pragma once
#include "nvn_backend.h"
#define NVN_GPU_TEX_BC7_VALIDATED (UINT32_C(1) << 31)
int nvn_gpu_bc7_enabled(void);
int nvn_gpu_tex_bc7_snapshot(nvn_gpu_cmd, nvn_gpu_tex, const void *, uint64_t, uint64_t, int, nvn_gpu_tex *);
int nvn_gpu_tex_bc7_unwrap(nvn_gpu_cmd, nvn_gpu_tex, nvn_gpu_tex *);
int nvn_gpu_bc7_step(nvn_gpu_cmd);
int nvn_gpu_tex_bc7_ready(nvn_gpu_tex);
nvn_gpu_tex nvn_gpu_tex_bc7_take(nvn_gpu_tex);
