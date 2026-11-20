// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// Adreno GPU 提频。前台有显示面时按游戏节拍和实际 GPU 频率判断。
// 调试构建可用 debug.s2a.gputurbo 切换 0（关）、1（常开）或 auto。
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void s2a_gpu_turbo_set_foreground(int foreground);
void s2a_gpu_turbo_set_surface(int present);
// 停局：线程收掉并等它退出；之后不再起。
void s2a_gpu_turbo_stop(void);
// 共用前端每次请求上屏时登记一帧。
void s2a_android_frame_note(void);
#ifdef S2A_GPU_TURBO_TEST
// 宿主测试：线程与判定的观测口，以及整套状态复位（必须在已停之后调）。
uint64_t s2a_gpu_turbo_test_ticks(void);        // 已完成的判定周期数（一个周期的全部事做完才加）
uint64_t s2a_gpu_turbo_test_last_tick_ns(void); // 最近一个已完成周期读到的时钟
void s2a_gpu_turbo_test_kick(void);             // 模拟时钟推进后叫醒托底线程
void s2a_gpu_turbo_test_reset(void);
#endif
#ifdef __cplusplus
}
#endif
