// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../../graphics/third_party/spirv_cross/spirv.h"

// Insert a specialization-gated fragment epilogue, keeping the SPIR-V interfaces
// and bindings fixed. Only outputs definitely stored on every normal exit are
// eligible. Return 0 for an unsupported shape, and -1 for allocation failure.
struct nvk_nan_spv {
    uint32_t *words;
    size_t count;
    uint32_t spec_id;
    uint8_t slots;
};

struct nvk_nan_id {
    uint32_t def, stores, block;
};

struct nvk_nan_block {
    uint32_t term, stores, incoming, next;
    uint8_t seen, pending;
};

static inline uint32_t nvk_nan_op(const uint32_t *inst) { return inst[0] & 0xffffu; }

static inline const uint32_t *nvk_nan_def(const uint32_t *words, const struct nvk_nan_id *ids,
                                          uint32_t bound, uint32_t id, SpvOp op, unsigned length) {
    if (!id || id >= bound || !ids[id].def) return NULL;
    const uint32_t *inst = words + ids[id].def;
    return nvk_nan_op(inst) == (uint32_t)op && inst[0] >> 16 == length ? inst : NULL;
}

static inline void nvk_nan_add_edge(struct nvk_nan_block *blocks, uint32_t target,
                                     uint32_t stores, uint32_t *pending) {
    struct nvk_nan_block *block = &blocks[target];
    if (block->seen && (block->incoming & stores) == block->incoming) return;
    block->incoming = block->seen ? block->incoming & stores : stores;
    block->seen = 1;
    if (!block->pending) {
        block->pending = 1;
        block->next = *pending;
        *pending = target;
    }
}

static inline int nvk_nan_flow(const uint32_t *words, const struct nvk_nan_id *ids,
                               uint32_t bound, struct nvk_nan_block *blocks) {
    uint32_t pending = 0;
    nvk_nan_add_edge(blocks, 1, 0, &pending);
    while (pending) {
        struct nvk_nan_block *from = &blocks[pending];
        pending = from->next;
        from->pending = 0;
        if (!from->term) return 0;
        const uint32_t *inst = words + from->term;
        const unsigned size = inst[0] >> 16;
        const uint32_t stores = from->incoming | from->stores;
        const uint32_t op = nvk_nan_op(inst);
        unsigned first, end;
        switch (op) {
        case SpvOpBranch: first = 1; end = 2; break;
        case SpvOpBranchConditional: first = 2; end = 4; break;
        case SpvOpSwitch: first = 2; end = 3; break;
        case SpvOpReturn: case SpvOpKill: case SpvOpTerminateInvocation: case SpvOpUnreachable:
            continue;
        default: return 0;
        }
        if (size < end) return 0;
        for (unsigned i = first; i < end; i++) {
            if (inst[i] >= bound || !ids[inst[i]].block) return 0;
            nvk_nan_add_edge(blocks, ids[inst[i]].block, stores, &pending);
        }
        if (op != SpvOpSwitch) continue;
        // Switch literals occupy one or two words. Without the selector's type,
        // use the union of valid interpretations. Extra edges only narrow proof.
        unsigned valid = 0;
        for (unsigned step = 2; step <= 3; step++) {
            if ((size - 3) % step) continue;
            unsigned i = 2 + step;
            for (; i < size; i += step)
                if (inst[i] >= bound || !ids[inst[i]].block) break;
            if (i < size) continue;
            valid = 1;
            for (i = 2 + step; i < size; i += step)
                nvk_nan_add_edge(blocks, ids[inst[i]].block, stores, &pending);
        }
        if (!valid) return 0;
    }
    return 1;
}

// All instructions below have literal operands or use IDs smaller than next_id.
// Keep the types introduced by the epilogue in the module's global section.
#define NVK_NAN_EMIT(dst, op, ...) do { \
    const uint32_t operands[] = {__VA_ARGS__}; \
    const uint32_t count = sizeof operands / sizeof operands[0]; \
    *(dst)++ = ((1 + count) << 16) | (op); \
    memcpy((dst), operands, sizeof operands); \
    (dst) += count; \
} while (0)

static inline uint32_t *nvk_nan_exit(uint32_t *out, uint32_t *next_id, uint32_t boolean, uint32_t uint,
                                     uint32_t scalar, uint32_t vec4, uint32_t var, uint32_t slot_bit,
                                     uint32_t spec, uint32_t zero, uint32_t abs_mask, uint32_t exp_mask) {
    uint32_t id = *next_id;
    const uint32_t value = id++, alpha = id++, alpha_bits = id++, abs_alpha = id++;
    const uint32_t zero_alpha = id++, slot_mask = id++, protected_slot = id++, transparent = id++;
    NVK_NAN_EMIT(out, SpvOpLoad, vec4, value, var);
    NVK_NAN_EMIT(out, SpvOpCompositeExtract, scalar, alpha, value, 3);
    NVK_NAN_EMIT(out, SpvOpBitcast, uint, alpha_bits, alpha);
    NVK_NAN_EMIT(out, SpvOpBitwiseAnd, uint, abs_alpha, alpha_bits, abs_mask);
    NVK_NAN_EMIT(out, SpvOpIEqual, boolean, zero_alpha, abs_alpha, zero);
    NVK_NAN_EMIT(out, SpvOpBitwiseAnd, uint, slot_mask, spec, slot_bit);
    NVK_NAN_EMIT(out, SpvOpINotEqual, boolean, protected_slot, slot_mask, zero);
    NVK_NAN_EMIT(out, SpvOpLogicalAnd, boolean, transparent, zero_alpha, protected_slot);
    uint32_t channels[3];
    for (unsigned i = 0; i < 3; i++) {
        const uint32_t lane = id++, bits = id++, exp = id++, nonfinite = id++, replace = id++, clean = id++;
        NVK_NAN_EMIT(out, SpvOpCompositeExtract, scalar, lane, value, i);
        NVK_NAN_EMIT(out, SpvOpBitcast, uint, bits, lane);
        NVK_NAN_EMIT(out, SpvOpBitwiseAnd, uint, exp, bits, exp_mask);
        NVK_NAN_EMIT(out, SpvOpIEqual, boolean, nonfinite, exp, exp_mask);
        NVK_NAN_EMIT(out, SpvOpLogicalAnd, boolean, replace, transparent, nonfinite);
        NVK_NAN_EMIT(out, SpvOpSelect, uint, clean, replace, zero, bits);
        channels[i] = id++;
        NVK_NAN_EMIT(out, SpvOpBitcast, scalar, channels[i], clean);
    }
    const uint32_t color = id++;
    NVK_NAN_EMIT(out, SpvOpCompositeConstruct, vec4, color, channels[0], channels[1], channels[2], alpha);
    NVK_NAN_EMIT(out, SpvOpStore, var, color);
    *next_id = id;
    return out;
}

static inline int nvk_nan_rewrite(const uint32_t *words, size_t n, const char *entry, struct nvk_nan_spv *result) {
    *result = (struct nvk_nan_spv){0};
    if (n < 5 || n > UINT32_MAX || words[0] != SpvMagicNumber || !words[3] || words[3] > 1048576) return 0;
    const uint32_t bound = words[3];
    size_t annotations = 0, functions = 0, interface = 0, interface_end = 0;
    uint32_t function = 0, uint = 0, boolean = 0, spec_id = 0;
    uint32_t block_count = 0, fragment_count = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t *inst = words + i;
        unsigned size = inst[0] >> 16;
        if (!size || size > n - i) return 0;
        const uint32_t op = nvk_nan_op(inst);
        if (op == SpvOpEntryPoint && size >= 4 && inst[1] == SpvExecutionModelFragment) {
            fragment_count++;
            const char *name = (const char *)(inst + 3);
            const char *end = memchr(name, 0, (size - 3) * sizeof(uint32_t));
            if (!end) return 0;
            if (!strcmp(name, entry)) {
                function = inst[2];
                interface = i + 3 + ((size_t)(end - name) + 4) / 4;
                interface_end = i + size;
            }
        } else if (op == SpvOpDecorate && size >= 3) {
            annotations = i + size;
            if (inst[2] == SpvDecorationSpecId && size == 4 && inst[3] >= spec_id) {
                if (inst[3] == UINT32_MAX) return 0;
                spec_id = inst[3] + 1;
            }
        } else if (op == SpvOpGroupDecorate || op == SpvOpGroupMemberDecorate) {
            return 0;   // Output decorations must belong to the named variable directly.
        } else if (op == SpvOpTypeInt && size == 4 && inst[2] == 32 && inst[3] == 0) {
            uint = inst[1];
        } else if (op == SpvOpTypeBool && size == 2) {
            boolean = inst[1];
        } else if (op == SpvOpFunction && !functions) {
            functions = i;
        } else if (op == SpvOpLabel) {
            block_count++;
        }
        i += size;
    }
    if (fragment_count != 1 || !function || !block_count || !annotations || annotations > functions) return 0;

    struct nvk_nan_id *ids = calloc(bound, sizeof *ids);
    struct nvk_nan_block *blocks = calloc((size_t)block_count + 1, sizeof *blocks);
    int status = -1;
    if (!ids || !blocks) goto done;
    status = 0;
    for (size_t i = 5; i < functions; i += words[i] >> 16) {
        const uint32_t *inst = words + i;
        size_t size = inst[0] >> 16;
        const uint32_t op = nvk_nan_op(inst);
        unsigned k = 0;
        if (op == SpvOpTypeFloat || op == SpvOpTypeInt || op == SpvOpTypeVector || op == SpvOpTypePointer) k = 1;
        else if (op == SpvOpConstant || op == SpvOpConstantNull || op == SpvOpVariable) k = 2;
        if (k && size > k && inst[k] < bound) ids[inst[k]].def = (uint32_t)i;
    }
    uint32_t var[8] = {0}, vec[8] = {0}, scalar[8] = {0};
    for (size_t p = interface; p < interface_end; p++) {
        const uint32_t id = words[p];
        const uint32_t *v = nvk_nan_def(words, ids, bound, id, SpvOpVariable, 4);
        if (!v || v[3] != SpvStorageClassOutput) continue;
        const uint32_t *ptr = nvk_nan_def(words, ids, bound, v[1], SpvOpTypePointer, 4);
        const uint32_t *type = ptr ? nvk_nan_def(words, ids, bound, ptr[3], SpvOpTypeVector, 4) : NULL;
        if (!type || type[3] != 4) continue;
        const uint32_t *f = nvk_nan_def(words, ids, bound, type[2], SpvOpTypeFloat, 3);
        if (!f || f[2] != 32) continue;
        uint32_t location = 8, index = 0, component = 0;
        for (size_t i = 5; i < annotations; i += words[i] >> 16) {
            const uint32_t *inst = words + i;
            if (nvk_nan_op(inst) != SpvOpDecorate || inst[0] >> 16 != 4 || inst[1] != id) continue;
            if (inst[2] == SpvDecorationLocation) location = inst[3];
            if (inst[2] == SpvDecorationIndex) index = inst[3];
            if (inst[2] == SpvDecorationComponent) component = inst[3];
        }
        if (location >= 8 || index || component) continue;
        if (var[location]) goto done;
        var[location] = id;
        scalar[location] = type[2];
        vec[location] = ptr[3];
        ids[id].stores = 15u << (4 * location);
    }
    size_t start = 0, end = 0;
    uint32_t block = 0;
    for (size_t i = functions; i < n; i += words[i] >> 16) {
        const uint32_t *inst = words + i;
        unsigned size = inst[0] >> 16;
        const uint32_t op = nvk_nan_op(inst);
        if (op == SpvOpFunction && size >= 3 && inst[2] == function) start = i;
        if (!start) continue;
        if (op == SpvOpFunctionEnd) { end = i; break; }
        if (op == SpvOpLabel && size == 2 && inst[1] < bound) {
            ids[inst[1]].block = ++block;
        } else if ((op == SpvOpAccessChain || op == SpvOpInBoundsAccessChain) && size == 5 &&
                   inst[2] < bound && inst[3] < bound && ids[inst[3]].stores) {
            const uint32_t target = ids[inst[3]].stores;
            const uint32_t *lane = nvk_nan_def(words, ids, bound, inst[4], SpvOpConstant, 4);
            const uint32_t *zero = nvk_nan_def(words, ids, bound, inst[4], SpvOpConstantNull, 3);
            const unsigned offset = lane ? lane[3] : zero ? 0 : 4;
            if (offset < 4 && (target & (target - 1))) ids[inst[2]].stores = (target & (0u - target)) << offset;
        } else if (op == SpvOpStore && block && size >= 3 && inst[1] < bound) {
            blocks[block].stores |= ids[inst[1]].stores;
        } else if (block && ((op >= SpvOpBranch && op <= SpvOpUnreachable) || op == SpvOpTerminateInvocation)) {
            blocks[block].term = (uint32_t)i;
        }
    }
    if (!end || !block || !nvk_nan_flow(words, ids, bound, blocks)) goto done;
    uint32_t definite = UINT32_MAX;
    size_t returns = 0;
    for (uint32_t i = 1; i <= block; i++) {
        if (blocks[i].seen && nvk_nan_op(words + blocks[i].term) == SpvOpReturn) {
            definite &= blocks[i].incoming | blocks[i].stores;
            returns++;
        }
    }
    uint8_t slots = 0;
    unsigned targets = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (var[i] && (definite >> (4 * i) & 15u) == 15u) { slots |= 1u << i; targets++; }
    }
    // Match the SPIRV-Tools universal ID bound and reserve enough space for the
    // 30 IDs and 150 words emitted per output per return, plus declarations.
    if (!slots || !returns || returns > (0x3fffffu - bound - 16u) / (targets * 30u)) goto done;
    const size_t extra = returns * targets * 150 + 64;
    if (extra > SIZE_MAX / sizeof(uint32_t) - n) goto done;
    uint32_t *patched = malloc((n + extra) * sizeof *patched);
    if (!patched) { status = -1; goto done; }
    memcpy(patched, words, 5 * sizeof *words);
    uint32_t *out = patched + 5;
    uint32_t next = bound;
    const uint32_t spec = next++;
    uint32_t zero = 0, abs_mask = 0, exp_mask = 0, slot_bits[8] = {0};
    uint32_t cur_block = 0;
    for (size_t i = 5; i < n; i += words[i] >> 16) {
        const uint32_t *inst = words + i;
        unsigned size = inst[0] >> 16;
        if (i == annotations) NVK_NAN_EMIT(out, SpvOpDecorate, spec, SpvDecorationSpecId, spec_id);
        if (i == functions) {
            if (!boolean) { boolean = next++; NVK_NAN_EMIT(out, SpvOpTypeBool, boolean); }
            if (!uint) { uint = next++; NVK_NAN_EMIT(out, SpvOpTypeInt, uint, 32, 0); }
            zero = next++; abs_mask = next++; exp_mask = next++;
            NVK_NAN_EMIT(out, SpvOpSpecConstant, uint, spec, 0);
            NVK_NAN_EMIT(out, SpvOpConstant, uint, zero, 0);
            NVK_NAN_EMIT(out, SpvOpConstant, uint, abs_mask, 0x7fffffffu);
            NVK_NAN_EMIT(out, SpvOpConstant, uint, exp_mask, 0x7f800000u);
            for (unsigned j = 0; j < 8; j++) if (slots & (1u << j)) {
                slot_bits[j] = next++;
                NVK_NAN_EMIT(out, SpvOpConstant, uint, slot_bits[j], 1u << j);
            }
        }
        if (i >= start && i < end) {
            unsigned op = nvk_nan_op(inst);
            if (op == SpvOpLabel) cur_block++;
            if (op == SpvOpReturn && blocks[cur_block].seen) {
                for (unsigned j = 0; j < 8; j++) if (slots & (1u << j))
                    out = nvk_nan_exit(out, &next, boolean, uint, scalar[j], vec[j], var[j], slot_bits[j],
                                       spec, zero, abs_mask, exp_mask);
            }
        }
        memcpy(out, inst, size * sizeof *out);
        out += size;
    }
    patched[3] = next;
    *result = (struct nvk_nan_spv){.words = patched, .count = (size_t)(out - patched),
                                 .spec_id = spec_id, .slots = slots};
    status = 1;
done:
    free(blocks);
    free(ids);
    return status;
}
#undef NVK_NAN_EMIT
