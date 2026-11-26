// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stdint.h>
#include "alpha_q48.h"

enum { NVK_BC7_RESERVED_MODE = 8, NVK_BC7_MODES = 9 };

static inline unsigned nvk_bc7_mode(uint8_t prefix) {
    return prefix ? (unsigned)__builtin_ctz((unsigned)prefix) : NVK_BC7_RESERVED_MODE;
}

static inline unsigned nvk_bc7_bits(const uint8_t block[16], unsigned offset, unsigned width) {
    const unsigned byte = offset / 8, shift = offset % 8;
    unsigned value = (unsigned)block[byte] >> shift;
    if (shift + width > 8) value |= (unsigned)block[byte + 1] << (8 - shift);
    return value & ((1u << width) - 1u);
}

// 保住源的透明/不透明端值和半透明分界。留半个 UNORM8 码值的分界余量，
// 以覆盖 ASTC 权重插值、输出精度和 UNORM8 解码方式的舍入。
static inline int nvk_bc7_astc_alpha_ok(const uint8_t block[16], unsigned mode) {
    static const uint8_t w6_bc[] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    static const uint8_t w6_astc[] = {0, 4, 8, 12, 17, 21, 25, 29, 35, 39, 43, 47, 52, 56, 60, 64};
    static const uint8_t w5[] = {0, 21, 43, 64};
    const uint8_t *bc_weights = mode == 6 ? w6_bc : w5, *astc_weights = mode == 6 ? w6_astc : w5;
    unsigned a0, a1, anchor, start, bits;
    if (mode == 6) {
        a0 = 2 * nvk_bc7_bits(block, 49, 7) + nvk_bc7_bits(block, 63, 1);
        a1 = 2 * nvk_bc7_bits(block, 56, 7) + nvk_bc7_bits(block, 64, 1);
        anchor = 65;
        start = 64;
        bits = 4;
    } else {
        const unsigned rotation = nvk_bc7_bits(block, 6, 2);
        if (rotation) {
            unsigned offset = 8 + 14 * (rotation - 1);
            a0 = nvk_bc7_bits(block, offset, 7);
            a1 = nvk_bc7_bits(block, offset + 7, 7);
            a0 = (a0 << 1) | (a0 >> 6);
            a1 = (a1 << 1) | (a1 >> 6);
        } else {
            a0 = nvk_bc7_bits(block, 50, 8);
            a1 = nvk_bc7_bits(block, 58, 8);
        }
        anchor = rotation ? 66 : 97;
        start = anchor - 1;
        bits = 2;
    }
    const unsigned q0 = nvk_bc7_alpha_q48[a0], q1 = nvk_bc7_alpha_q48[a1];
    if (a0 == a1 && a0 == q0) return 1;
    // 先检查权重实际选出的像素。端点很近且全在同一半域时一次放行常见的恒定 alpha。
    if (a0 && a0 < 255 && a1 && a1 < 255 && q0 && q0 < 255 && q1 && q1 < 255 &&
        ((a0 < 128 && a1 < 128 && q0 <= 127 && q1 <= 127) ||
         (a0 >= 128 && a1 >= 128 && q0 >= 128 && q1 >= 128))) return 1;
    for (unsigned p = 0; p < 16; p++) {
        const unsigned i = p ? nvk_bc7_bits(block, start + p * bits, bits)
                             : nvk_bc7_bits(block, anchor, bits - 1);
        const unsigned w = bc_weights[i], wa = astc_weights[i];
        const unsigned bc = ((64 - w) * a0 + w * a1 + 32) / 64;
        const unsigned astc = (64 - wa) * q0 + wa * q1;
        if ((bc == 0 ? astc != 0 : astc < 64) ||
            (bc == 255 ? astc != 64 * 255 : astc > 64 * 254) ||
            (bc < 128 ? astc > 64 * 127 : astc < 64 * 128)) return 0;
    }
    return 1;
}

static inline int nvk_bc7_astc_block_eligible(const uint8_t block[16], unsigned mode) {
    return (mode == 5 || mode == 6) && nvk_bc7_astc_alpha_ok(block, mode);
}
