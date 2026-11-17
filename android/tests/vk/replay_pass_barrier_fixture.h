// SPDX-License-Identifier: Apache-2.0
// 不链接 rp_pass.c 的图形夹具使用同语义的游戏屏障账；合并后取走清零，不伪造已发屏障。
#pragma once
#include "rp_exec.h"
static uint32_t fixture_pending_barriers;
void rp_pass_barrier(uint32_t bits) { fixture_pending_barriers |= bits; }
uint32_t rp_pass_take_barrier(void) {
    uint32_t bits = fixture_pending_barriers; fixture_pending_barriers = 0; return bits;
}
