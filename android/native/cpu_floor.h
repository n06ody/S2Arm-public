// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// CPU 频率托底：一条低优先级线程，在游戏线程所在的那组核上保持满载，让按利用率定频的调频器
// 不把这一组核压在上限以下（判定表与背景见 cpu_floor_policy.h）。
// 生命周期与 adpf_hint 同口径：前台 + 有显示面才可能托底，退后台 / 丢显示面 / 停局即时松开。
// 已有成功上报的 ADPF 会话时暂停 CPU 托底，GPU 提频仍独立判定。
// 缺省自适应（auto）：帧率连着 3 个窗口低于目标 9/10、且目标簇近 3 s 均频还没顶在上限（≥ 95%）才托；托了之后帧率到目标就松；
// 上限被系统压低就松开并本局不再出手；托了没换来帧率也松开，暂缓 60 s 再试（再没有收益就翻倍，封顶 600 s）。
// 调试开关：系统属性 debug.s2a.cpufloor（0 关 / 1 常开 / auto 或未设 = 自适应）、debug.s2a.cpufloor_spin（alu / wfe），约每 250 ms 重读一次；
// 属性只走现有的调试入口两道门（android/native/debug_gate.h：调试构建 + debug.s2a.debug_env=1），
// 门没开（玩家包）时连属性都不读，一律缺省自适应 + WFE。
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void s2a_cpu_floor_set_foreground(int foreground);
void s2a_cpu_floor_set_surface(int present);
// 停局：线程收掉并等它退出；之后不再起。
void s2a_cpu_floor_stop(void);
// 共用前端每帧调一次（graphics/nvn/gfx/nvn/hle_nvn.c，只在 __ANDROID__ 下）：一次原子加 + 一次时钟读取 + 一次直方图原子加。
// 托底线程据此量实际出图帧率，再与游戏登记的目标周期（s2a_android_present_ns）比；
// 并每 5 秒打一行帧间隔读数（直方图 / p50 / p99 / 慢帧数 / 进程 CPU / 托底线程 CPU），任何模式（含 0）都打。
void s2a_android_frame_note(void);
#ifdef S2A_CPU_FLOOR_TEST
// 宿主测试：线程与判定的观测口，以及整套状态复位（必须在已停之后调）。
uint64_t s2a_cpu_floor_test_ticks(void);        // 已完成的判定周期数（一个周期的全部事做完才加）
uint64_t s2a_cpu_floor_test_last_tick_ns(void); // 最近一个已完成周期读到的时钟
void s2a_cpu_floor_test_kick(void);             // 模拟时钟推进后叫醒托底线程
int s2a_cpu_floor_test_active(void);
void s2a_cpu_floor_test_reset(void);
#endif
#ifdef __cplusplus
}
#endif
