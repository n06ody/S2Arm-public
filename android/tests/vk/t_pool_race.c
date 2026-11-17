// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 池表 bind 的竞态用例：不建 VkDevice，只跑后端里的宿主自检入口
// （backend_vulkan_pool.inc 的 nvn_gpu_vk_test_pool_race）。
// 受控交错在 bind 取到快照引用之后执行 release；判据是「取到的是不是已释放/已销毁的表」。
#include <stddef.h>
#include <stdio.h>

#include "nvn_plat.h"

extern int nvn_gpu_vk_test_pool_race(void);

// 平台替身：这条用例只走句柄/锁路径，不读系统属性、不写日志、不落盘。
// 签名照 graphics/nvn/replay/nvn_plat.h，和 t_backend_vk.c 里的宿主实现同口径。
int nvn_plat_flag(const char *name, int dflt) { (void)name; return dflt; }
void nvn_plat_log(int level, const char *cls, const char *text) { (void)level; (void)cls; (void)text; }
const char *nvn_plat_dir(int which) { (void)which; return NULL; }
void nvn_plat_safepoint(void) {}

int main(void) {
    int bad = nvn_gpu_vk_test_pool_race();
    if (bad) {
        printf("pool bind race: %d failure(s)\n", bad);
        return 1;
    }
    printf("pool bind race: PASS\n");
    return 0;
}
