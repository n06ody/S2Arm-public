// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 旧回放夹具不调用贴图清除；误入时硬失败，不能把未验证操作伪装成成功。
#include "nvn_backend.h"
#include <stdlib.h>
void nvn_gpu_clear_view(nvn_gpu_cmd c, nvn_gpu_view v, const float rgba[4], const uint32_t u4[4], int is_int) {
    (void)c; (void)v; (void)rgba; (void)u4; (void)is_int; abort();
}
