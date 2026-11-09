/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 S2Arm contributors */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// A4（#48）：系统游戏模式 = 省电档时直连 present 周期取 max(原周期, 33.3 ms)。
// Java 轮询 GameManager.getGameMode() 后经 NativeBridge.setBatteryCap 写；直连 present_period 每帧读。
void s2a_android_set_battery_limit(int enabled);
int s2a_android_battery_limit(void);
uint64_t s2a_android_limit_frame_period(uint64_t period_ns);

// A3（#48）：直连 present_period 每帧登记最终周期，Java 轮询后经 Surface.setFrameRate 投显示刷新率。
// 0 = 还没上过屏 / 无上限路径。
void s2a_android_present_note(uint64_t period_ns);
uint64_t s2a_android_present_ns(void);

#ifdef __cplusplus
}
#endif
