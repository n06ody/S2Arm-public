// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Unified pre-load SPIR-V declaration scan for nvn_gpu_shader_load (#1865):
// one bounded pass collects every device-facing declaration — OpCapability
// set, OpExtension names, PushConstant variable bytes, header version.
// The evaluator (backend_vulkan.c spv_compliance_check) compares the result
// against enabled features / limits / extensions; this header stays
// device-free so the host test can drive it with synthetic modules.
#pragma once
#include <stdio.h>
#include "spirv_workgroup.h"

enum { S2A_SPV_CAPS_MAX = 96, S2A_SPV_EXT_MAX = 12, S2A_SPV_EXT_NAME = 64 };
struct s2a_spv_decl {
    uint32_t version;                              // header word 1 (e.g. 0x10300)
    uint32_t caps[S2A_SPV_CAPS_MAX];
    unsigned n_caps;
    unsigned caps_truncated;                       // declarations beyond CAPS_MAX
    char exts[S2A_SPV_EXT_MAX][S2A_SPV_EXT_NAME];
    unsigned n_ext;
    unsigned ext_truncated;
    uint64_t push_bytes;                           // PushConstant storage-class total
    int has_push_bytes;                            // 0 = no PushConstant variable
    int has_workgroup_init;                        // 1 = Workgroup OpVariable with Initializer operand
    int has_scalar_block_layout;                   // 1 = layout violates relaxed_block_layout (needs scalarBlockLayout)
    int has_ubo_std430_layout;                     // 1 = Uniform Block uses std430-compact array stride (<16B aligned)
    int has_local_size_id;                         // 1 = OpExecutionModeId LocalSizeId declared
    uint32_t float_controls_modes;                 // 15-bit SPV_KHR_float_controls execution mode mask
};

// Requirement classes the evaluator maps onto device fields. REQ_CORE means
// "satisfied by Vulkan 1.0 core". UNLISTED is a result, not a table entry.
enum s2a_spv_req {
    SPV_REQ_UNLISTED = -1, SPV_REQ_CORE = 0,
    SPV_REQ_INT16, SPV_REQ_FLOAT16, SPV_REQ_INT64, SPV_REQ_FLOAT64, SPV_REQ_INT8,
    SPV_REQ_GEOMETRY, SPV_REQ_TESSELLATION, SPV_REQ_TG_POINTSIZE,
    SPV_REQ_IMAGE_GATHER_EXT, SPV_REQ_STORAGE_IMAGE_MS, SPV_REQ_STORAGE_IMG_EXT_FMT,
    SPV_REQ_UBO_DYN, SPV_REQ_SAMPLED_DYN, SPV_REQ_SBO_DYN, SPV_REQ_IMG_DYN,
    SPV_REQ_CLIP_DIST, SPV_REQ_CULL_DIST, SPV_REQ_CUBE_ARRAY, SPV_REQ_MULTI_VIEWPORT,
    SPV_REQ_STORAGE_READ_WOF, SPV_REQ_STORAGE_WRITE_WOF,
    SPV_REQ_DRAW_PARAMETERS, SPV_REQ_API_1_1, SPV_REQ_MAINTENANCE4,
    SPV_REQ_FLOAT_CONTROLS, SPV_REQ_16BIT_STORAGE,
    SPV_REQ_16BIT_STORAGE_SSBO, SPV_REQ_16BIT_STORAGE_UBO_SSBO,
    SPV_REQ_16BIT_STORAGE_PUSH, SPV_REQ_16BIT_STORAGE_IO, SPV_REQ_8BIT_STORAGE,
    SPV_REQ_8BIT_STORAGE_SSBO, SPV_REQ_8BIT_STORAGE_UBO_SSBO, SPV_REQ_8BIT_STORAGE_PUSH,
    SPV_REQ_DESCRIPTOR_INDEXING, SPV_REQ_SBC_CLASS, SPV_REQ_POST_DEPTH,
    SPV_REQ_MULTIVIEW, SPV_REQ_STENCIL_EXPORT, SPV_REQ_SHADING_RATE,
    SPV_REQ_VARIABLE_POINTERS_SB, SPV_REQ_VARIABLE_POINTERS, SPV_REQ_BUFFER_DEVICE_ADDRESS,
};
static inline const char *s2a_spv_req_name(int req) {
    switch (req) {
    case SPV_REQ_INT16: return "shaderInt16";
    case SPV_REQ_FLOAT16: return "shaderFloat16";
    case SPV_REQ_INT64: return "shaderInt64";
    case SPV_REQ_FLOAT64: return "shaderFloat64";
    case SPV_REQ_INT8: return "shaderInt8";
    case SPV_REQ_GEOMETRY: return "geometryShader";
    case SPV_REQ_TESSELLATION: return "tessellationShader";
    case SPV_REQ_TG_POINTSIZE: return "shaderTessellationAndGeometryPointSize";
    case SPV_REQ_IMAGE_GATHER_EXT: return "shaderImageGatherExtended";
    case SPV_REQ_STORAGE_IMAGE_MS: return "shaderStorageImageMultisample";
    case SPV_REQ_STORAGE_IMG_EXT_FMT: return "shaderStorageImageExtendedFormats";
    case SPV_REQ_UBO_DYN: return "shaderUniformBufferArrayDynamicIndexing";
    case SPV_REQ_SAMPLED_DYN: return "shaderSampledImageArrayDynamicIndexing";
    case SPV_REQ_SBO_DYN: return "shaderStorageBufferArrayDynamicIndexing";
    case SPV_REQ_IMG_DYN: return "shaderStorageImageArrayDynamicIndexing";
    case SPV_REQ_CLIP_DIST: return "shaderClipDistance";
    case SPV_REQ_CULL_DIST: return "shaderCullDistance";
    case SPV_REQ_CUBE_ARRAY: return "imageCubeArray";
    case SPV_REQ_MULTI_VIEWPORT: return "multiViewport";
    case SPV_REQ_STORAGE_READ_WOF: return "shaderStorageImageReadWithoutFormat";
    case SPV_REQ_STORAGE_WRITE_WOF: return "shaderStorageImageWriteWithoutFormat";
    case SPV_REQ_DRAW_PARAMETERS: return "shaderDrawParameters";
    case SPV_REQ_API_1_1: return "apiVersion>=1.1";
    case SPV_REQ_MAINTENANCE4: return "VK_KHR_maintenance4";
    case SPV_REQ_FLOAT_CONTROLS: return "VK_KHR_shader_float_controls";
    case SPV_REQ_16BIT_STORAGE: return "VK_KHR_16bit_storage";
    case SPV_REQ_16BIT_STORAGE_SSBO: return "storageBuffer16BitAccess";
    case SPV_REQ_16BIT_STORAGE_UBO_SSBO: return "uniformAndStorageBuffer16BitAccess";
    case SPV_REQ_16BIT_STORAGE_PUSH: return "storagePushConstant16";
    case SPV_REQ_16BIT_STORAGE_IO: return "storageInputOutput16";
    case SPV_REQ_8BIT_STORAGE: return "VK_KHR_8bit_storage";
    case SPV_REQ_8BIT_STORAGE_SSBO: return "storageBuffer8BitAccess";
    case SPV_REQ_8BIT_STORAGE_UBO_SSBO: return "uniformAndStorageBuffer8BitAccess";
    case SPV_REQ_8BIT_STORAGE_PUSH: return "storagePushConstant8";
    case SPV_REQ_DESCRIPTOR_INDEXING: return "VK_EXT_descriptor_indexing";
    case SPV_REQ_SBC_CLASS: return "VK_KHR_storage_buffer_storage_class";
    case SPV_REQ_POST_DEPTH: return "VK_EXT_post_depth_coverage";
    case SPV_REQ_MULTIVIEW: return "VK_KHR_multiview";
    case SPV_REQ_STENCIL_EXPORT: return "VK_EXT_shader_stencil_export";
    case SPV_REQ_SHADING_RATE: return "VK_KHR_fragment_shading_rate";
    case SPV_REQ_VARIABLE_POINTERS_SB: return "variablePointersStorageBuffer";
    case SPV_REQ_VARIABLE_POINTERS: return "variablePointers";
    case SPV_REQ_BUFFER_DEVICE_ADDRESS: return "VK_KHR_buffer_device_address";
    default: return "?";
    }
}

// Declared capability → required feature/extension. Mapped entries are hard
// requirements on every source; anything not listed is UNLISTED and the
// caller applies the two-tier rule (precompiled: reject; runtime: warn+count).
static inline int s2a_spv_cap_req(uint32_t cap) {
    switch (cap) {
    case SpvCapabilityMatrix: case SpvCapabilityShader:
    case SpvCapabilitySampled1D: case SpvCapabilityImage1D:
    case SpvCapabilitySampledCubeArray: case SpvCapabilitySampledBuffer:
    case SpvCapabilityImageBuffer: case SpvCapabilityImageQuery:
    case SpvCapabilityImageRect: case SpvCapabilitySampledRect:
    case SpvCapabilityDerivativeControl: case SpvCapabilityInterpolationFunction:
        return SPV_REQ_CORE;
    case SpvCapabilityGeometry: case SpvCapabilityGeometryStreams: return SPV_REQ_GEOMETRY;
    case SpvCapabilityTessellation: return SPV_REQ_TESSELLATION;
    case SpvCapabilityFloat16Buffer: case SpvCapabilityFloat16: return SPV_REQ_FLOAT16;
    case SpvCapabilityFloat64: return SPV_REQ_FLOAT64;
    case SpvCapabilityInt64: case SpvCapabilityInt64Atomics: return SPV_REQ_INT64;
    case SpvCapabilityInt16: return SPV_REQ_INT16;
    case SpvCapabilityInt8: return SPV_REQ_INT8;
    case SpvCapabilityTessellationPointSize: case SpvCapabilityGeometryPointSize: return SPV_REQ_TG_POINTSIZE;
    case SpvCapabilityImageGatherExtended: return SPV_REQ_IMAGE_GATHER_EXT;
    case SpvCapabilityStorageImageMultisample: return SPV_REQ_STORAGE_IMAGE_MS;
    case SpvCapabilityStorageImageExtendedFormats: return SPV_REQ_STORAGE_IMG_EXT_FMT;
    case SpvCapabilityUniformBufferArrayDynamicIndexing: return SPV_REQ_UBO_DYN;
    case SpvCapabilitySampledImageArrayDynamicIndexing: return SPV_REQ_SAMPLED_DYN;
    case SpvCapabilityStorageBufferArrayDynamicIndexing: return SPV_REQ_SBO_DYN;
    case SpvCapabilityStorageImageArrayDynamicIndexing: return SPV_REQ_IMG_DYN;
    case SpvCapabilityClipDistance: return SPV_REQ_CLIP_DIST;
    case SpvCapabilityCullDistance: return SPV_REQ_CULL_DIST;
    case SpvCapabilityImageCubeArray: return SPV_REQ_CUBE_ARRAY;
    case SpvCapabilityMultiViewport: return SPV_REQ_MULTI_VIEWPORT;
    case SpvCapabilityStorageImageReadWithoutFormat: return SPV_REQ_STORAGE_READ_WOF;
    case SpvCapabilityStorageImageWriteWithoutFormat: return SPV_REQ_STORAGE_WRITE_WOF;
    case SpvCapabilitySubgroupDispatch: case SpvCapabilityDeviceGroup:
    case SpvCapabilityGroupNonUniform: case SpvCapabilityGroupNonUniformVote:
    case SpvCapabilityGroupNonUniformArithmetic: case SpvCapabilityGroupNonUniformBallot:
    case SpvCapabilityGroupNonUniformShuffle: case SpvCapabilityGroupNonUniformShuffleRelative:
    case SpvCapabilityGroupNonUniformClustered: case SpvCapabilityGroupNonUniformQuad:
    case SpvCapabilitySubgroupBallotKHR: case SpvCapabilitySubgroupVoteKHR:
        return SPV_REQ_API_1_1;   // 声明级：subgroup/device-group 是 Vulkan 1.1 核心
    case SpvCapabilityStorageBuffer16BitAccess: return SPV_REQ_16BIT_STORAGE_SSBO;
    case SpvCapabilityUniformAndStorageBuffer16BitAccess: return SPV_REQ_16BIT_STORAGE_UBO_SSBO;
    case SpvCapabilityStoragePushConstant16: return SPV_REQ_16BIT_STORAGE_PUSH;
    case SpvCapabilityStorageInputOutput16: return SPV_REQ_16BIT_STORAGE_IO;
        return SPV_REQ_16BIT_STORAGE;
    case SpvCapabilityStorageBuffer8BitAccess: return SPV_REQ_8BIT_STORAGE_SSBO;
    case SpvCapabilityUniformAndStorageBuffer8BitAccess: return SPV_REQ_8BIT_STORAGE_UBO_SSBO;
    case SpvCapabilityStoragePushConstant8: return SPV_REQ_8BIT_STORAGE_PUSH;
    case SpvCapabilityDenormPreserve: case SpvCapabilityDenormFlushToZero:
    case SpvCapabilitySignedZeroInfNanPreserve: case SpvCapabilityRoundingModeRTE:
    case SpvCapabilityRoundingModeRTZ:
        return SPV_REQ_FLOAT_CONTROLS;
    case SpvCapabilityDrawParameters: return SPV_REQ_DRAW_PARAMETERS;
    case SpvCapabilityMultiView: return SPV_REQ_MULTIVIEW;
    case SpvCapabilityVariablePointersStorageBuffer: return SPV_REQ_VARIABLE_POINTERS_SB;
    case SpvCapabilityVariablePointers: return SPV_REQ_VARIABLE_POINTERS;
    case SpvCapabilityPhysicalStorageBufferAddresses: return SPV_REQ_BUFFER_DEVICE_ADDRESS;
    case SpvCapabilityFragmentShadingRateKHR: return SPV_REQ_SHADING_RATE;
    case SpvCapabilityStencilExportEXT: return SPV_REQ_STENCIL_EXPORT;
    case SpvCapabilityUniformBufferArrayNonUniformIndexing:
    case SpvCapabilitySampledImageArrayNonUniformIndexing:
    case SpvCapabilityStorageBufferArrayNonUniformIndexing:
    case SpvCapabilityStorageImageArrayNonUniformIndexing:
    case SpvCapabilityInputAttachmentArrayNonUniformIndexing:
        return SPV_REQ_DESCRIPTOR_INDEXING;
    default: return SPV_REQ_UNLISTED;
    }
}

// OpExtension literal name → required enabling. Same two-tier rule as caps.
static inline int s2a_spv_ext_req(const char *name) {
    if (!strcmp(name, "SPV_KHR_shader_draw_parameters")) return SPV_REQ_DRAW_PARAMETERS;
    if (!strcmp(name, "SPV_KHR_16bit_storage")) return SPV_REQ_16BIT_STORAGE;
    if (!strcmp(name, "SPV_KHR_8bit_storage")) return SPV_REQ_8BIT_STORAGE;
    if (!strcmp(name, "SPV_KHR_multiview")) return SPV_REQ_MULTIVIEW;
    if (!strcmp(name, "SPV_KHR_post_depth_coverage")) return SPV_REQ_POST_DEPTH;
    if (!strcmp(name, "SPV_EXT_descriptor_indexing")) return SPV_REQ_DESCRIPTOR_INDEXING;
    if (!strcmp(name, "SPV_KHR_storage_buffer_storage_class")) return SPV_REQ_SBC_CLASS;
    if (!strcmp(name, "SPV_KHR_shader_stencil_export")) return SPV_REQ_STENCIL_EXPORT;
    if (!strcmp(name, "SPV_KHR_float_controls")) return SPV_REQ_FLOAT_CONTROLS;
    if (!strcmp(name, "SPV_KHR_variable_pointers")) return SPV_REQ_VARIABLE_POINTERS_SB;
    if (!strcmp(name, "SPV_KHR_buffer_device_address")) return SPV_REQ_BUFFER_DEVICE_ADDRESS;
    if (!strcmp(name, "SPV_KHR_device_group") || !strcmp(name, "SPV_KHR_shader_ballot") ||
        !strcmp(name, "SPV_KHR_subgroup_vote") || !strcmp(name, "SPV_KHR_vulkan_memory_model"))
        return SPV_REQ_API_1_1;   // 这些 SPIR-V 扩展是 Vulkan 1.1+ 核心语义
    return SPV_REQ_UNLISTED;
}

static inline int s2a_spv_vec_info(const unsigned char *p, size_t n, uint32_t ty_id,
                                   uint32_t *vec_bytes, uint32_t *nat_align) {
    size_t vdef = s2a_spv_wg_type_def(p, n, ty_id);
    if (!vdef) return 0;
    uint32_t vf = s2a_spv_wg_word(p, vdef);
    if ((vf & 65535u) != SpvOpTypeVector || (vf >> 16) != 4) return 0;
    uint32_t cty = s2a_spv_wg_word(p, vdef + 2);
    uint32_t vlen = s2a_spv_wg_word(p, vdef + 3);
    if (vlen < 2 || vlen > 4) return 0;
    size_t cdef = s2a_spv_wg_type_def(p, n, cty);
    if (!cdef) return 0;
    uint32_t cf = s2a_spv_wg_word(p, cdef), cop = cf & 65535u;
    if ((cop != SpvOpTypeFloat && cop != SpvOpTypeInt) || (cf >> 16) < 3) return 0;
    uint32_t bits = s2a_spv_wg_word(p, cdef + 2);
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) return 0;
    uint32_t sb = bits / 8;
    *vec_bytes = vlen * sb;
    *nat_align = (vlen == 3 ? 4u : vlen) * sb;
    return 1;
}

// relaxed_block_layout 的向量跨界规则按 16B 计算；超过 16B 的向量要从 16B 边界开始。
// 向量数组的 ArrayStride 仍须是元素基础对齐的倍数（如 vec3[] 的步长不能是 12）。
static inline int s2a_spv_needs_scalar_layout(const unsigned char *p, size_t n) {
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (op == SpvOpDecorate && len == 4 && s2a_spv_wg_word(p, i + 2) == SpvDecorationArrayStride) {
            uint32_t arr_id = s2a_spv_wg_word(p, i + 1), stride = s2a_spv_wg_word(p, i + 3);
            size_t adef = s2a_spv_wg_type_def(p, n, arr_id);
            if (adef) {
                uint32_t af = s2a_spv_wg_word(p, adef), aop = af & 65535u, alen = af >> 16;
                if ((aop == SpvOpTypeArray && alen == 4) || (aop == SpvOpTypeRuntimeArray && alen == 3)) {
                    uint32_t vbytes = 0, nalign = 0;
                    if (s2a_spv_vec_info(p, n, s2a_spv_wg_word(p, adef + 2), &vbytes, &nalign) &&
                        stride != 0 && (stride % nalign) != 0)
                        return 1;
                }
            }
        } else if (op == SpvOpMemberDecorate && len == 5 && s2a_spv_wg_word(p, i + 3) == SpvDecorationOffset) {
            uint32_t st_id = s2a_spv_wg_word(p, i + 1), mbr = s2a_spv_wg_word(p, i + 2), off = s2a_spv_wg_word(p, i + 4);
            size_t sdef = s2a_spv_wg_type_def(p, n, st_id);
            if (sdef) {
                uint32_t sf = s2a_spv_wg_word(p, sdef), slen = sf >> 16;
                if ((sf & 65535u) == SpvOpTypeStruct && slen > 2 && mbr < slen - 2) {
                    uint32_t vbytes = 0, nalign = 0;
                    if (s2a_spv_vec_info(p, n, s2a_spv_wg_word(p, sdef + 2 + mbr), &vbytes, &nalign) &&
                        (vbytes <= 16 ? off % 16 + vbytes > 16 : off % 16 != 0))
                        return 1;
                }
            }
        }
        i += len;
    }
    return 0;
}

static inline int s2a_spv_has_dec(const unsigned char *p, size_t n, uint32_t id, uint32_t dec) {
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (op == SpvOpDecorate && len >= 3 && s2a_spv_wg_word(p, i + 1) == id && s2a_spv_wg_word(p, i + 2) == dec)
            return 1;
        i += len;
    }
    return 0;
}

static inline uint32_t s2a_spv_array_stride(const unsigned char *p, size_t n, uint32_t id) {
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (op == SpvOpDecorate && len == 4 && s2a_spv_wg_word(p, i + 1) == id &&
            s2a_spv_wg_word(p, i + 2) == SpvDecorationArrayStride)
            return s2a_spv_wg_word(p, i + 3);
        i += len;
    }
    return 0;
}

// Vulkan 1.1 std140 / relaxed_block_layout 要求 Uniform Block（非 BufferBlock）内数组成员的 ArrayStride 必须为 16 的倍数；
// 出现步长非 16 倍数（如 float[8] 步长 4、vec2[2] 步长 8）需要 VK_KHR_uniform_buffer_standard_layout。
static inline int s2a_spv_needs_ubo_std430(const unsigned char *p, size_t n) {
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (op == SpvOpVariable && len >= 4 && s2a_spv_wg_word(p, i + 3) == SpvStorageClassUniform) {
            size_t pt = s2a_spv_wg_type_def(p, n, s2a_spv_wg_word(p, i + 1));
            if (pt && (s2a_spv_wg_word(p, pt) & 65535u) == SpvOpTypePointer && (s2a_spv_wg_word(p, pt) >> 16) == 4) {
                uint32_t ty = s2a_spv_wg_word(p, pt + 3);
                size_t tdef = s2a_spv_wg_type_def(p, n, ty);
                if (tdef) {
                    uint32_t tf = s2a_spv_wg_word(p, tdef), top = tf & 65535u, tlen = tf >> 16;
                    if ((top == SpvOpTypeArray && tlen == 4) || (top == SpvOpTypeRuntimeArray && tlen == 3)) {
                        ty = s2a_spv_wg_word(p, tdef + 2);
                        tdef = s2a_spv_wg_type_def(p, n, ty);
                    }
                }
                if (tdef) {
                    uint32_t sf = s2a_spv_wg_word(p, tdef), slen = sf >> 16;
                    if ((sf & 65535u) == SpvOpTypeStruct && slen > 2 &&
                        s2a_spv_has_dec(p, n, ty, SpvDecorationBlock) &&
                        !s2a_spv_has_dec(p, n, ty, SpvDecorationBufferBlock)) {
                        for (uint32_t m = 2; m < slen; m++) {
                            uint32_t mty = s2a_spv_wg_word(p, tdef + m);
                            size_t mdef = s2a_spv_wg_type_def(p, n, mty);
                            if (!mdef) continue;
                            uint32_t mf = s2a_spv_wg_word(p, mdef), mop = mf & 65535u, mlen = mf >> 16;
                            if ((mop == SpvOpTypeArray && mlen == 4) || (mop == SpvOpTypeRuntimeArray && mlen == 3)) {
                                uint32_t st = s2a_spv_array_stride(p, n, mty);
                                if (st != 0 && (st % 16u) != 0) return 1;
                            }
                        }
                    }
                }
            }
        }
        i += len;
    }
    return 0;
}

static inline const char *s2a_spv_scan(const void *blob, size_t bytes, struct s2a_spv_decl *d) {
    memset(d, 0, sizeof *d);
    const unsigned char *p = (const unsigned char *)blob;
    if (!p || bytes < 20 || (bytes & 3u) || s2a_spv_word(p) != SpvMagicNumber)
        return "spv compliance: malformed SPIR-V header";
    d->version = s2a_spv_word(p + 4);
    const size_t n = bytes / 4;
    uint64_t push = 0;
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (!len || len > n - i) return "spv compliance: malformed instruction length";
        if (op == SpvOpCapability && len >= 2) {
            if (d->n_caps < S2A_SPV_CAPS_MAX) d->caps[d->n_caps++] = s2a_spv_wg_word(p, i + 1);
            else d->caps_truncated++;
        } else if (op == SpvOpExtension && len >= 2) {
            const char *name = (const char *)(p + (i + 1) * 4);
            size_t maxl = (len - 1) * 4;
            if (!memchr(name, 0, maxl)) return "spv compliance: unterminated OpExtension name";
            if (d->n_ext < S2A_SPV_EXT_MAX) {
                snprintf(d->exts[d->n_ext], S2A_SPV_EXT_NAME, "%s", name);
                d->n_ext++;
            } else d->ext_truncated++;
        } else if (op == SpvOpExecutionModeId && len >= 3 && s2a_spv_wg_word(p, i + 2) == SpvExecutionModeLocalSizeId) {
            d->has_local_size_id = 1;
        } else if (op == SpvOpExecutionMode && len == 4) {
            uint32_t mode = s2a_spv_wg_word(p, i + 2), width = s2a_spv_wg_word(p, i + 3);
            if (mode >= SpvExecutionModeDenormPreserve && mode <= SpvExecutionModeRoundingModeRTZ) {
                uint32_t widx = width == 16u ? 0u : width == 32u ? 1u : width == 64u ? 2u : 3u;
                if (widx >= 3u) return "spv compliance: invalid float controls bit width";
                d->float_controls_modes |= 1u << ((mode - SpvExecutionModeDenormPreserve) * 3u + widx);
            }
        } else if (op == SpvOpVariable && len >= 4 && s2a_spv_wg_word(p, i + 3) == SpvStorageClassPushConstant) {
            size_t pt = s2a_spv_wg_type_def(p, n, s2a_spv_wg_word(p, i + 1));
            if (!pt || (s2a_spv_wg_word(p, pt) & 65535u) != SpvOpTypePointer || (s2a_spv_wg_word(p, pt) >> 16) != 4)
                return "spv compliance: push-constant variable without pointer type";
            uint64_t e = 0; const char *r = s2a_spv_wg_type_size(p, n, s2a_spv_wg_word(p, pt + 3), 0, &e);
            if (r) return r;
            if (UINT64_MAX - push < e) return "spv compliance: push-constant byte count overflow";
            push += e;
        } else if (op == SpvOpVariable && len >= 5 && s2a_spv_wg_word(p, i + 3) == SpvStorageClassWorkgroup) {
            d->has_workgroup_init = 1;
        }
        i += len;
    }
    d->push_bytes = push;
    d->has_push_bytes = push ? 1 : 0;
    d->has_scalar_block_layout = s2a_spv_needs_scalar_layout(p, n);
    d->has_ubo_std430_layout = s2a_spv_needs_ubo_std430(p, n);
    return NULL;
}
