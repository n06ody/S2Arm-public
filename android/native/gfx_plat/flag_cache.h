// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
// 开关读数的线程内小缓存（plat_nvn.c 的 nvn_plat_flag 用；纯 C，无平台依赖，宿主单测直接包含它）。
//
// 调试入口开着时 nvn_plat_flag 每次调用都要读两次系统属性（门 debug.s2a.debug_env + 开关本身）并 snprintf 拼键名；
// 回放热路径上每个通道、每次绘制前都要读若干个开关，真机取样里属性专用函数占回放线程样本的 5.6%，
// 连同它们调的字符串函数最多 12.0%（32620 个样本里 1822 / 3921 个，见 docs/坑记录.md）。玩家包里这道门编译期就是关的，不受影响；
// 但调试包的读数因此比玩家包慢，所有「调试包上量出来的帧时间」都带着这一块。
//
// 做法：每个线程一张小表（无锁），按开关名记「读到的值 / 没设」，TTL 之内直接返回；过期了重读，所以运行中 adb setprop
// 改开关仍然生效（延迟不超过 TTL）。没设时返回的是「这次调用传的缺省」，不是缓存里的某个缺省（同一个开关在不同调用点缺省可以不同）。
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Only the readback switch has an explicit named third mode.
static inline int fc_value(const char *name, const char *value) {
    if (!strcmp(name, "nvk_land_readback")) {
        if (!strcmp(value, "auto")) return 2;
        return atoi(value) == 1;
    }
    return atoi(value);
}

enum { FC_SLOTS = 64, FC_PROBE = 4, FC_NAME_MAX = 40 };   // FC_SLOTS 必须是 2 的幂；名字长到 FC_NAME_MAX 个字节以上的不进表

struct fc_ent { char name[FC_NAME_MAX]; int32_t val; uint8_t has, used; uint64_t until; };
struct fc_tab { struct fc_ent e[FC_SLOTS]; };

// 真读：返回 1 = 有值（写 *val），0 = 没设或门关着。
typedef int (*fc_read_fn)(const char *name, int *val, void *user);

// now / ttl 同一单位（调用方给，纳秒）。命中且没过期不调 rd；其余情况调一次 rd 并写回表项。
static inline int fc_get(struct fc_tab *t, const char *name, int dflt, uint64_t now, uint64_t ttl, fc_read_fn rd, void *user) {
    size_t n = 0; uint32_t h = 2166136261u; int v = 0;
    while (n < FC_NAME_MAX && name[n]) { h = (h ^ (uint8_t)name[n]) * 16777619u; n++; }
    if (n >= FC_NAME_MAX) return rd(name, &v, user) ? v : dflt;   // 名字太长：不进表，每次直读
    struct fc_ent *slot = NULL;
    for (uint32_t p = 0; p < FC_PROBE; p++) {
        struct fc_ent *e = &t->e[(h + p) & (FC_SLOTS - 1)];
        if (!e->used) { slot = e; break; }   // 探测链上的项从不删除，遇到空位说明表里没有这个名字
        if (!strcmp(e->name, name)) {
            if (now < e->until) return e->has ? e->val : dflt;
            slot = e; break;   // 过期：就地重读
        }
    }
    if (!slot) slot = &t->e[h & (FC_SLOTS - 1)];   // 探测窗口里没空位：挤掉窗口第一项（只会多读一次，不会读错）
    const int has = rd(name, &v, user) != 0;
    memcpy(slot->name, name, n + 1);
    slot->val = has ? v : 0; slot->has = (uint8_t)has; slot->used = 1; slot->until = now + ttl;
    return has ? v : dflt;
}
