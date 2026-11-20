// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
// 直连编译器翻译器调试模式开关（单变量排查用）：native 侧判定，编译请求 JSON 带 debugMode=true。
// 必须过调试门（debug_gate.h 的两道门）：
//   - 门关着（正式构建没有 S2A_ANDROID_DEBUG_ENV，或运行期没有 debug.s2a.debug_env=1）恒为关，
//     玩家包里 adb 设 debug.s2a.xlate_debug=1 不会开调试模式；
//   - 门开着且 debug.s2a.xlate_debug=1 才开，由请求 JSON 的 `debugMode:true` 交给编译器，不写环境变量。
//   请求里该键恒显式写（关着也是 `"debugMode":false`）：sha256 覆盖整个请求 JSON，不带键的请求与
//   「编译器 DebugMode 恒开」时期逐字节相同会命中旧缓存；显式写让翻译旗口径进身份。
// DebugMode 关掉翻译器表达式传播、临时量全部物化成局部变量，SPIR-V 约大一倍；
// 只用于离线对拍现编/出库产物，正常路径必须走缺省。
// 阶段口径（出库链 11-07 起同规则）：顶点/片元关，含几何段的程序整条保留 DebugMode
// （组合翻译一条程序一个旗，没法只给 gs 留），计算路径本就走 flags=0 不变。
#include <stdbool.h>
#include <stdlib.h>
#include "../debug_gate.h"

static inline bool s2a_direct_xlate_debug_enabled(void) {
    if (!s2a_debug_entries_open()) return false;
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.s2a.xlate_debug", value) == 1 && value[0] && atoi(value) != 0) return true;
#endif
    return false;
}
