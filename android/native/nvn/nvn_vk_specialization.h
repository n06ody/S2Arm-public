// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

// The selected stage owns the storage until vkCreate*Pipelines returns.
_Static_assert(sizeof(VkBool32) == sizeof(uint32_t), "Vulkan specialization bool uses VkBool32");
struct nvk_stage_specialization {
    VkSpecializationInfo info;
    VkSpecializationMapEntry entries[NVN_GPU_SPEC_MAX];
    uint32_t values[NVN_GPU_SPEC_MAX];
};

static int nvk_spv_has_specialization(const void *blob, size_t bytes) {
    const unsigned char *p = blob;
    for (size_t i = 5, n = bytes / 4; i < n;) {
        const uint32_t word = s2a_spv_wg_word(p, i), len = word >> 16;
        if (!len || len > n - i) return 0;
        if ((word & 65535u) == SpvOpDecorate && len == 4 &&
            s2a_spv_wg_word(p, i + 2) == SpvDecorationSpecId) return 1;
        if ((word & 65535u) == SpvOpFunction) return 0;
        i += len;
    }
    return 0;
}

static int nvk_spec_type(const unsigned char *p, size_t n, size_t def) {
    const uint32_t word = s2a_spv_wg_word(p, def), op = word & 65535u;
    const uint32_t type = s2a_spv_wg_word(p, def + 1);
    if (op == SpvOpSpecConstantTrue || op == SpvOpSpecConstantFalse)
        return (word >> 16) == 3 && s2a_spv_wg_def(p, n, type, SpvOpTypeBool) ? NVN_GPU_SPEC_BOOL : -1;
    if (op != SpvOpSpecConstant || (word >> 16) != 4) return -1;
    const size_t integer = s2a_spv_wg_def(p, n, type, SpvOpTypeInt);
    if (integer && (s2a_spv_wg_word(p, integer) >> 16) == 4 &&
        s2a_spv_wg_word(p, integer + 2) == 32)
        return s2a_spv_wg_word(p, integer + 3) ? NVN_GPU_SPEC_INT : NVN_GPU_SPEC_UINT;
    const size_t floating = s2a_spv_wg_def(p, n, type, SpvOpTypeFloat);
    if (floating && (s2a_spv_wg_word(p, floating) >> 16) >= 3 &&
        s2a_spv_wg_word(p, floating + 2) == 32) return NVN_GPU_SPEC_FLOAT;
    return -1;
}

static nvn_gpu_rc nvk_prepare_specialization(const nvn_gpu_stage_desc *stage,
                                            struct nvk_stage_specialization *spec, nvn_gpu_err *err) {
    // Layout and limits were checked at load time. Match each override with
    // the module's actual SpecId/type before exposing it to the driver.
    const struct nvn_gpu_shader_s *sh = stage->sh;
    const unsigned char *p = (const unsigned char *)sh->code;
    const size_t n = sh->code_bytes / 4;
    size_t defs[NVN_GPU_SPEC_MAX] = {0};
    for (size_t i = 5; i < n;) {
        const uint32_t word = s2a_spv_wg_word(p, i), len = word >> 16;
        if (!len || len > n - i) return fail(err, NVN_GPU_E_ARG, "特化常量：SPIR-V 指令长度无效");
        if ((word & 65535u) == SpvOpDecorate && len == 4 &&
            s2a_spv_wg_word(p, i + 2) == SpvDecorationSpecId) {
            uint32_t id = s2a_spv_wg_word(p, i + 3);
            for (uint32_t j = 0; j < stage->nspec; j++) {
                if (id != stage->spec[j].id) continue;
                if (defs[j]) return fail(err, NVN_GPU_E_ARG, "特化常量：SpecId %u 声明不唯一", id);
                defs[j] = s2a_spv_wg_def(p, n, s2a_spv_wg_word(p, i + 1), 0);
            }
        }
        if ((word & 65535u) == SpvOpFunction) break;
        i += len;
    }
    for (uint32_t i = 0; i < stage->nspec; i++) {
        const nvn_gpu_spec *entry = &stage->spec[i];
        if (!defs[i]) return fail(err, NVN_GPU_E_UNSUPPORTED, "特化常量：SPIR-V 无 SpecId %u", (unsigned)entry->id);
        if (nvk_spec_type(p, n, defs[i]) != entry->type)
            return fail(err, NVN_GPU_E_ARG, "特化常量：SpecId %u 的类型与 SPIR-V 不符", (unsigned)entry->id);
        spec->values[i] = entry->type == NVN_GPU_SPEC_BOOL ? (uint32_t)(entry->bits != 0) : entry->bits;
        spec->entries[i] = (VkSpecializationMapEntry){entry->id, i * sizeof spec->values[0], sizeof spec->values[0]};
    }
    for (size_t i = 5; i < n;) {
        const uint32_t word = s2a_spv_wg_word(p, i), len = word >> 16, op = word & 65535u;
        if (op == SpvOpFunction) break;
        if (op == SpvOpTypeArray && len == 4) {
            const size_t def = s2a_spv_wg_def(p, n, s2a_spv_wg_word(p, i + 3), 0);
            // The reflected descriptor and push layouts describe default sizes.
            // Reject size overrides and expressions we cannot verify.
            if (def && (s2a_spv_wg_word(p, def) & 65535u) == SpvOpSpecConstantOp)
                return fail(err, NVN_GPU_E_UNSUPPORTED, "特化常量：数组长度表达式无法核对");
            for (uint32_t j = 0; j < stage->nspec; j++)
                if (def == defs[j]) return fail(err, NVN_GPU_E_UNSUPPORTED, "特化常量：不可更改已经核对的数组长度");
        }
        i += len;
    }
    if (sh->stage == NVN_GPU_SH_CS) {
        // WorkgroupSize can use SpecIds even at Vulkan 1.1. Run the same
        // device-limit preflight with the effective scalar literals.
        uint32_t *code = malloc(sh->code_bytes);
        if (!code) return fail(err, NVN_GPU_E_OOM, "特化工作组预检分配失败");
        memcpy(code, p, sh->code_bytes);
        for (uint32_t i = 0; i < stage->nspec; i++) {
            const size_t def = defs[i];
            if (stage->spec[i].type == NVN_GPU_SPEC_BOOL)
                code[def] = (3u << 16) | (spec->values[i] ? SpvOpSpecConstantTrue : SpvOpSpecConstantFalse);
            else code[def + 3] = spec->values[i];
        }
        nvn_gpu_shader_src src = {.blob = code, .bytes = sh->code_bytes, .entry = sh->entry};
        memcpy(src.key, sh->src_key, sizeof src.key);
        nvn_gpu_rc result = compute_workgroup_check(&src, err);
        free(code);
        if (!nvn_gpu_ok(result)) return result;
    }
    spec->info = (VkSpecializationInfo){
        .mapEntryCount = stage->nspec, .pMapEntries = spec->entries,
        .dataSize = stage->nspec * sizeof spec->values[0], .pData = spec->values,
    };
    return rc(NVN_GPU_OK);
}
