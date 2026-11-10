// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// session-log 宿主测试的系统属性头桩：签名与 PROP_VALUE_MAX 同真 bionic（92）。
// 只在本测试的 -I 路径下生效；真机/NDK 构建用系统头，不走这里。
#pragma once

#define PROP_VALUE_MAX 92

#ifdef __cplusplus
extern "C" {
#endif
int __system_property_get(const char *name, char *value);
#ifdef __cplusplus
}
#endif
