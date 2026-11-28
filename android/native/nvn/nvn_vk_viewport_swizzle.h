// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

// Viewport swizzle is applied to the last raster shader's clip Position,
// before primitive clipping. The backend has one viewport and no native XFB.
struct nvk_position {
    uint32_t entry, variable, vector, scalar, member;
    int block;
};

static int nvk_position_output(const unsigned char *code, size_t n, size_t first,
                               size_t last, struct nvk_position *pos) {
    for (size_t k = first; k < last; k++) {
        uint32_t id = s2a_spv_wg_word(code, k);
        size_t var = 0;
        for (size_t i = 5; i < n;) {
            uint32_t word = s2a_spv_wg_word(code, i), len = word >> 16, op = word & 65535u;
            if (op == SpvOpFunction) break;
            if (op == SpvOpVariable && len >= 4 && s2a_spv_wg_word(code, i + 2) == id &&
                s2a_spv_wg_word(code, i + 3) == SpvStorageClassOutput) {
                var = i;
                break;
            }
            i += len;
        }
        if (!var) continue;
        size_t ptr = s2a_spv_wg_def(code, n, s2a_spv_wg_word(code, var + 1), SpvOpTypePointer);
        if (!ptr || (s2a_spv_wg_word(code, ptr) >> 16) != 4) continue;
        uint32_t pointee = s2a_spv_wg_word(code, ptr + 3);
        uint32_t vector = 0, member = 0;
        int block = 0;
        for (size_t i = 5; i < n;) {
            uint32_t word = s2a_spv_wg_word(code, i), len = word >> 16, op = word & 65535u;
            if (op == SpvOpFunction) break;
            if (op == SpvOpDecorate && len == 4 && s2a_spv_wg_word(code, i + 1) == id &&
                s2a_spv_wg_word(code, i + 2) == SpvDecorationBuiltIn &&
                s2a_spv_wg_word(code, i + 3) == SpvBuiltInPosition) {
                vector = pointee;
                break;
            }
            if (op == SpvOpMemberDecorate && len == 5 && s2a_spv_wg_word(code, i + 1) == pointee &&
                s2a_spv_wg_word(code, i + 3) == SpvDecorationBuiltIn &&
                s2a_spv_wg_word(code, i + 4) == SpvBuiltInPosition) {
                size_t st = s2a_spv_wg_def(code, n, pointee, SpvOpTypeStruct);
                member = s2a_spv_wg_word(code, i + 2);
                if (!st || member >= (s2a_spv_wg_word(code, st) >> 16) - 2) return 0;
                block = 1;
                vector = s2a_spv_wg_word(code, st + 2 + member);
                break;
            }
            i += len;
        }
        if (!vector) continue;
        if (pos->variable) return 0;
        size_t vt = s2a_spv_wg_def(code, n, vector, SpvOpTypeVector);
        if (!vt || (s2a_spv_wg_word(code, vt) >> 16) != 4 || s2a_spv_wg_word(code, vt + 3) != 4) return 0;
        uint32_t scalar = s2a_spv_wg_word(code, vt + 2);
        size_t ft = s2a_spv_wg_def(code, n, scalar, SpvOpTypeFloat);
        if (!ft || (s2a_spv_wg_word(code, ft) >> 16) < 3 || s2a_spv_wg_word(code, ft + 2) != 32) return 0;
        pos->variable = id;
        pos->vector = vector;
        pos->scalar = scalar;
        pos->member = member;
        pos->block = block;
    }
    return pos->variable != 0;
}

struct nvk_swizzle_code {
    uint32_t *out;
    size_t at;
    uint32_t id;
};

static void nvk_spv_emit(struct nvk_swizzle_code *dst, SpvOp op, const uint32_t *args, size_t count) {
    dst->out[dst->at++] = ((uint32_t)(count + 1) << 16) | (uint32_t)op;
    memcpy(dst->out + dst->at, args, count * sizeof *args);
    dst->at += count;
}

#define NVK_SPV_EMIT(dst, op, ...) do { \
    const uint32_t args_[] = {__VA_ARGS__}; \
    nvk_spv_emit(dst, op, args_, sizeof args_ / sizeof args_[0]); \
} while (0)

static void nvk_swizzle_position(struct nvk_swizzle_code *dst, const struct nvk_position *pos,
                                 uint32_t ptr, uint32_t member, uint32_t half, uint16_t packed) {
    uint32_t addr = pos->variable;
    if (pos->block) {
        addr = dst->id++;
        NVK_SPV_EMIT(dst, SpvOpAccessChain, ptr, addr, pos->variable, member);
    }
    uint32_t value = dst->id++;
    NVK_SPV_EMIT(dst, SpvOpLoad, pos->vector, value, addr);
    uint32_t src[4], out[4];
    for (unsigned k = 0; k < 4; k++) {
        src[k] = dst->id++;
        NVK_SPV_EMIT(dst, SpvOpCompositeExtract, pos->scalar, src[k], value, k);
    }
    const uint32_t host_z = src[2];
    // Translators without depth clip control have already converted z to
    // (z+w)/2. Work in guest clip coordinates and convert the result back.
    if (half) {
        const uint32_t twice_z = dst->id++;
        NVK_SPV_EMIT(dst, SpvOpFAdd, pos->scalar, twice_z, host_z, host_z);
        src[2] = dst->id++;
        NVK_SPV_EMIT(dst, SpvOpFSub, pos->scalar, src[2], twice_z, src[3]);
    }
    for (unsigned k = 0; k < 4; k++) {
        unsigned sel = ((packed >> (3 * k)) & 7u) ^ (2 * k);
        out[k] = src[sel >> 1];
        if (sel & 1) {
            uint32_t id = dst->id++;
            NVK_SPV_EMIT(dst, SpvOpFNegate, pos->scalar, id, out[k]);
            out[k] = id;
        }
    }
    if (half) {
        if (((packed >> 6) & 63u) == 0) out[2] = host_z;
        else {
            uint32_t sum = dst->id++;
            NVK_SPV_EMIT(dst, SpvOpFAdd, pos->scalar, sum, out[2], out[3]);
            out[2] = dst->id++;
            NVK_SPV_EMIT(dst, SpvOpFMul, pos->scalar, out[2], sum, half);
        }
    }
    value = dst->id++;
    NVK_SPV_EMIT(dst, SpvOpCompositeConstruct, pos->vector, value, out[0], out[1], out[2], out[3]);
    NVK_SPV_EMIT(dst, SpvOpStore, addr, value);
}

static nvn_gpu_rc nvk_swizzle_module(const struct nvn_gpu_shader_s *shader, uint16_t packed,
                                    VkShaderModule *out, nvn_gpu_err *err) {
    *out = VK_NULL_HANDLE;
    const unsigned char *code = (const unsigned char *)shader->code;
    const size_t n = shader->code_bytes / 4;
    if (!code || n < 5) return fail(err, NVN_GPU_E_UNSUPPORTED, "viewport_swizzle：缺少光栅阶段 SPIR-V");
    const int is_gs = shader->stage == NVN_GPU_SH_GS;
    const SpvExecutionModel model = is_gs ? SpvExecutionModelGeometry : SpvExecutionModelVertex;
    struct nvk_position pos = {0};
    size_t iface = 0, iface_end = 0, first_func = 0, inserts = 0;
    unsigned entry_count = 0;
    uint32_t func = 0;
    for (size_t i = 5; i < n;) {
        uint32_t word = s2a_spv_wg_word(code, i), len = word >> 16, op = word & 65535u;
        if (!len || len > n - i) return fail(err, NVN_GPU_E_ARG, "viewport_swizzle：SPIR-V 指令长度无效");
        if (op == SpvOpEntryPoint && len >= 4) {
            ++entry_count;
            const char *name = (const char *)code + (i + 3) * 4;
            const char *end = memchr(name, 0, (len - 3) * 4);
            if (!end) return fail(err, NVN_GPU_E_ARG, "viewport_swizzle：SPIR-V 入口名不完整");
            if (s2a_spv_wg_word(code, i + 1) == (uint32_t)model && !strcmp(name, shader->entry)) {
                pos.entry = s2a_spv_wg_word(code, i + 2);
                iface = i + 3 + (size_t)(end - name) / 4 + 1;
                iface_end = i + len;
            }
        } else if (op == SpvOpFunction && len == 5) {
            if (!first_func) first_func = i;
            func = s2a_spv_wg_word(code, i + 2);
        } else if (is_gs ? op == SpvOpEmitVertex || op == SpvOpEmitStreamVertex
                         : op == SpvOpReturn && func && func == pos.entry) {
            ++inserts;
        }
        i += len;
    }
    if (!pos.entry || !first_func || !inserts || (is_gs && entry_count != 1) ||
        !nvk_position_output(code, n, iface, iface_end, &pos))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "viewport_swizzle：无法确定光栅阶段 Position 写出点");
    // Reserve a strict upper bound for globals and each injected sequence.
    const uint32_t bound = s2a_spv_wg_word(code, 3);
    enum { GLOBAL_WORDS = 32, INSERT_WORDS = 80, GLOBAL_IDS = 4, INSERT_IDS = 16 };
    if (n > SIZE_MAX / 4 - GLOBAL_WORDS || inserts > (SIZE_MAX / 4 - GLOBAL_WORDS - n) / INSERT_WORDS ||
        !bound || bound > 0x3fffffu - GLOBAL_IDS || inserts > (0x3fffffu - GLOBAL_IDS - bound) / INSERT_IDS)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "viewport_swizzle：SPIR-V 变体超过模块上限");
    struct nvk_swizzle_code dst = {malloc((n + GLOBAL_WORDS + inserts * INSERT_WORDS) * 4), 0, bound};
    if (!dst.out) return fail(err, NVN_GPU_E_OOM, "viewport_swizzle：SPIR-V 变体分配失败");
    memcpy(dst.out, code, first_func * 4);
    dst.at = first_func;
    uint32_t ptr = 0, member = 0, half = 0;
    if (pos.block) {
        uint32_t index_type = 0;
        for (size_t i = 5; i < first_func;) {
            uint32_t word = s2a_spv_wg_word(code, i), len = word >> 16, op = word & 65535u;
            if (op == SpvOpTypeInt && len == 4 && s2a_spv_wg_word(code, i + 2) == 32)
                index_type = s2a_spv_wg_word(code, i + 1);
            if (op == SpvOpTypePointer && len == 4 &&
                s2a_spv_wg_word(code, i + 2) == SpvStorageClassOutput && s2a_spv_wg_word(code, i + 3) == pos.vector)
                ptr = s2a_spv_wg_word(code, i + 1);
            i += len;
        }
        if (!ptr) {
            ptr = dst.id++;
            NVK_SPV_EMIT(&dst, SpvOpTypePointer, ptr, SpvStorageClassOutput, pos.vector);
        }
        if (!index_type) {
            index_type = dst.id++;
            NVK_SPV_EMIT(&dst, SpvOpTypeInt, index_type, 32, 0);
        }
        member = dst.id++;
        NVK_SPV_EMIT(&dst, SpvOpConstant, index_type, member, pos.member);
    }
    if (packed & NVN_GPU_VP_SWIZZLE_Z_REMAP) {
        half = dst.id++;
        NVK_SPV_EMIT(&dst, SpvOpConstant, pos.scalar, half, 0x3f000000u);
    }
    for (size_t i = first_func; i < n;) {
        uint32_t word = s2a_spv_wg_word(code, i), len = word >> 16, op = word & 65535u;
        if (op == SpvOpFunction && len == 5) func = s2a_spv_wg_word(code, i + 2);
        if (is_gs ? op == SpvOpEmitVertex || op == SpvOpEmitStreamVertex
                  : op == SpvOpReturn && func == pos.entry)
            nvk_swizzle_position(&dst, &pos, ptr, member, half, packed);
        memcpy(dst.out + dst.at, code + i * 4, len * 4);
        dst.at += len;
        i += len;
    }
    dst.out[3] = dst.id;
    const VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = dst.at * 4, .pCode = dst.out,
    };
    const VkResult vr = vkCreateShaderModule(g.d, &ci, NULL, out);
    free(dst.out);
    if (vr != VK_SUCCESS)
        return fail(err, pipe_vk_res_code(vr, NVN_GPU_E_COMPILE), "viewport_swizzle：vkCreateShaderModule 失败 (%d)", (int)vr);
    return rc(NVN_GPU_OK);
}

#undef NVK_SPV_EMIT
