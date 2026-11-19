// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 画质设置来源选择（nvk_quality_cfg.h）：调试门开 + 属性非空 → 属性；否则 → 环境；都没有 → NULL。
#include "nvk_quality_cfg.h"
#include <stdio.h>
#include <string.h>
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("【红】 %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *s(const char *p) { return p ? p : "(NULL)"; }
int main(void) {
    static const struct { const char *prop; int gate; const char *env, *want; const char *what; } c[] = {
        { "0.5", 1, "2", "0.5", "门开、属性非空：属性压过环境（QA A/B）" },
        { "0.5", 0, "2", "2", "门关（玩家包 / 没设 debug_env）：属性忽略，取环境" },
        { "0.5", 0, NULL, NULL, "门关、没有环境：缺省（NULL）" },
        { "", 1, "0.75", "0.75", "门开、属性空：取环境" },
        { "", 1, NULL, NULL, "门开、属性空、没有环境：缺省" },
        { NULL, 1, "1.5", "1.5", "门开、没读属性：取环境" },
        { "Nearest", 1, NULL, "Nearest", "滤镜同一规则：门开属性" },
        { "Nearest", 0, "Bilinear", "Bilinear", "滤镜同一规则：门关取环境" },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        const char *got = nvk_quality_pick(c[i].prop, c[i].gate, c[i].env);
        int ok = (!got && !c[i].want) || (got && c[i].want && !strcmp(got, c[i].want));
        CHECK(ok, "%s：得到 %s，期望 %s", c[i].what, s(got), s(c[i].want));
    }
    printf("%s nvk_quality_cfg：%d 项检查，%d 项红\n", fails ? "✗" : "✓", checks, fails);
    return fails ? 1 : 0;
}
