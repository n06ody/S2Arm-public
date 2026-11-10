// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 属性替身，语义同真 bionic __system_property_get：
//   name 查不到 → 返回 0、不动 value；查到了 → 值拷进 value 并返回实际长度。
// 取值源：同名环境变量（envp 允许带点号名，run.py 按用例注入
// debug.s2a.session_uuid 以正控/负控首行 uuid 字段）。
#include <stdlib.h>
#include <string.h>

int __system_property_get(const char *name, char *value) {
    const char *v = getenv(name);
    if (!v) return 0;
    strcpy(value, v);
    return (int)strlen(v);
}
