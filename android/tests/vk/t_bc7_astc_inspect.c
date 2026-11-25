// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "bc7_astc_shaders/inspect.h"
#include <stdio.h>
#include <stdlib.h>

static void check(int condition, const char *name) {
    if (condition) return;
    fprintf(stderr, "BC7 eligibility: %s\n", name);
    exit(1);
}

int main(void) {
    uint8_t mode6[16] = {0x40};
    uint8_t mode5[16] = {0x20};
    uint8_t mode4[16] = {0x10};
    uint8_t invalid[16] = {0};
    check(nvk_bc7_astc_block_eligible(mode6, nvk_bc7_mode(mode6[0])), "mode 6 exact transparent alpha");
    check(nvk_bc7_astc_block_eligible(mode5, nvk_bc7_mode(mode5[0])), "mode 5 exact transparent alpha");
    check(!nvk_bc7_astc_block_eligible(mode4, nvk_bc7_mode(mode4[0])), "mode 4 fallback");
    check(!nvk_bc7_astc_block_eligible(invalid, nvk_bc7_mode(invalid[0])), "reserved prefix fallback");
    // Mode 6 interpolation of alpha endpoints 0 and 5 at BC7 weight 4
    // rounds to zero in BC7. ASTC can expose nonzero alpha at this texel.
    mode6[7] = 2;
    mode6[8] = 3;
    check(!nvk_bc7_astc_block_eligible(mode6, nvk_bc7_mode(mode6[0])), "rounded-to-zero alpha fallback");
    // Rotation 1 把向量 R 搬到 A，必须看第一平面的权重。
    mode5[0] = 0x60;
    mode5[1] = 0x80;
    mode5[8] = 0x04;
    check(!nvk_bc7_astc_block_eligible(mode5, 5), "rotated alpha uses the primary weight plane");
    for (unsigned mode = 0; mode < NVK_BC7_RESERVED_MODE; mode++)
        check(nvk_bc7_mode((uint8_t)(0xffu << mode)) == mode, "first set bit classifies mode");
    puts("BC7 eligibility: all cases passed");
    return 0;
}
