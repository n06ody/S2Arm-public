// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stddef.h>
int nvk_quality_read_properties(char *scale, size_t scale_cap, char *filter, size_t filter_cap);
// 画质设置（渲染倍率 S2A_RES_SCALE / 上屏滤镜 S2A_SCALING_FILTER）的来源选择，开会话时用（nvk_session.c）。
//   调试门开着（调试构建 + debug.s2a.debug_env=1，debug_gate.h）且同名调试属性（debug.s2a.res_scale / debug.s2a.scaling_filter）非空：取属性
//   ——QA 同包 A/B 只改属性，不用改进程环境；
//   其余（玩家包、门关、属性空）：取进程环境（启动器 / 玩家设置写的同名变量），没有 = NULL（回放核心按缺省）。
//   纯逻辑，宿主单测 android/tests/nvk_quality_cfg_test.c。
static inline const char *nvk_quality_pick(const char *prop, int gate_open, const char *env) {
    if (gate_open && prop && prop[0]) return prop;
    return env;
}
