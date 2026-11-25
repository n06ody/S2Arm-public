// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "direct/sha256.h"
#include "nvn_backend.h"
#include "nvn_refl.h"

enum { COLD_ASSET_HEADER = 184, COLD_ASSET_MAX = 16 * 1024 * 1024 };
struct nvk_cold_recipe {
    uint32_t fragment_ent, first_word, stride_words, count;
    uint32_t prefix_ent, prefix_byte, prefix_mask;
};
struct nvk_cold_asset {
    const uint8_t *variant, *variant_sha;
    size_t variant_size;
    struct nvk_cold_recipe recipe;
};

static inline uint32_t cold_asset_u32(const uint8_t *s) {
    return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
}

static inline void cold_asset_digest(const void *buf, size_t bytes, uint8_t out[32]) {
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    s2a_sha256_update(&hash, buf, bytes);
    s2a_sha256_final(&hash, out);
}

static inline int cold_asset_binding(const nvn_gpu_layout *layout, uint32_t set, uint32_t binding,
                                     uint8_t cls, uint8_t kind, uint32_t *ent) {
    int found = 0;
    for (uint32_t i = 0; i < layout->nent; i++) {
        const nvn_gpu_layout_ent *item = &layout->ent[i];
        if (item->set != set || item->binding != binding) continue;
        if (found || item->cls != cls || item->src_kind != kind || item->count != 1 ||
            item->access != NVN_GPU_ACC_R) return 0;
        *ent = i;
        found = 1;
    }
    return found;
}

static inline int nvk_cold_asset_parse(const uint8_t *data, size_t size, const uint8_t identity[32],
                                      const uint8_t original_sha[32], const nvn_gpu_layout *layout,
                                      struct nvk_cold_asset *out) {
    *out = (struct nvk_cold_asset){0};
    if (size <= COLD_ASSET_HEADER || size > COLD_ASSET_MAX || !data ||
        memcmp(data, "S2ACOLD1", 8) ||
        cold_asset_u32(data + 8) != size - COLD_ASSET_HEADER ||
        ((size - COLD_ASSET_HEADER) & 3) || cold_asset_u32(data + 12) ||
        memcmp(data + 16, identity, 32) || memcmp(data + 48, original_sha, 32)) return 0;

    uint8_t sha[32];
    cold_asset_digest(data + COLD_ASSET_HEADER, size - COLD_ASSET_HEADER, sha);
    if (memcmp(sha, data + 80, 32)) return 0;
    // The digest covers the recipe and both identities, plus the complete B.
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    s2a_sha256_update(&hash, data, 152);
    s2a_sha256_update(&hash, data + COLD_ASSET_HEADER, size - COLD_ASSET_HEADER);
    s2a_sha256_final(&hash, sha);
    if (memcmp(sha, data + 152, 32)) return 0;

    const uint32_t first = cold_asset_u32(data + 120);
    const uint32_t stride = cold_asset_u32(data + 124);
    const uint32_t count = cold_asset_u32(data + 128);
    const uint32_t prefix_set = cold_asset_u32(data + 132);
    const uint32_t prefix_binding = cold_asset_u32(data + 136);
    const uint32_t prefix_byte = cold_asset_u32(data + 140);
    const uint32_t prefix_mask = cold_asset_u32(data + 144);
    if (!count || count > 65536 || !stride || first >= stride || cold_asset_u32(data + 148) ||
        (uint64_t)first + (uint64_t)(count - 1) * stride + prefix_mask / 4 >= UINT32_MAX) return 0;

    struct nvk_cold_recipe recipe = {.first_word = first, .stride_words = stride, .count = count};
    if (!cold_asset_binding(layout, cold_asset_u32(data + 112), cold_asset_u32(data + 116),
                            NVN_GPU_RES_SSBO, NVN_RB_SBUFP, &recipe.fragment_ent)) return 0;
    if (prefix_mask) {
        if (prefix_mask > 255 || (prefix_mask & (prefix_mask + 1)) || (prefix_byte & 3) ||
            !cold_asset_binding(layout, prefix_set, prefix_binding, NVN_GPU_RES_UBO,
                                NVN_RB_DRV0, &recipe.prefix_ent)) return 0;
        recipe.prefix_byte = prefix_byte;
        recipe.prefix_mask = prefix_mask;
    } else if (prefix_set || prefix_binding || prefix_byte) {
        return 0;
    }
    out->recipe = recipe;
    out->variant = data + COLD_ASSET_HEADER;
    out->variant_size = size - COLD_ASSET_HEADER;
    out->variant_sha = data + 80;
    return 1;
}
