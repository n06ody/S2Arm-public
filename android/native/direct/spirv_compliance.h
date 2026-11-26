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
    SPV_REQ_DRAW_PARAMETERS, SPV_REQ_API_1_1,
    SPV_REQ_FLOAT_CONTROLS, SPV_REQ_16BIT_STORAGE, SPV_REQ_8BIT_STORAGE,
    SPV_REQ_DESCRIPTOR_INDEXING, SPV_REQ_SBC_CLASS, SPV_REQ_POST_DEPTH,
    SPV_REQ_MULTIVIEW, SPV_REQ_STENCIL_EXPORT, SPV_REQ_SHADING_RATE,
    SPV_REQ_VARIABLE_POINTERS, SPV_REQ_BUFFER_DEVICE_ADDRESS,
    SPV_REQ_VIEWPORT_INDEX_LAYER, SPV_REQ_SHADER_LAYER,
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
    case SPV_REQ_FLOAT_CONTROLS: return "VK_KHR_shader_float_controls";
    case SPV_REQ_16BIT_STORAGE: return "VK_KHR_16bit_storage";
    case SPV_REQ_8BIT_STORAGE: return "VK_KHR_8bit_storage";
    case SPV_REQ_DESCRIPTOR_INDEXING: return "VK_EXT_descriptor_indexing";
    case SPV_REQ_SBC_CLASS: return "VK_KHR_storage_buffer_storage_class";
    case SPV_REQ_POST_DEPTH: return "VK_EXT_post_depth_coverage";
    case SPV_REQ_MULTIVIEW: return "VK_KHR_multiview";
    case SPV_REQ_STENCIL_EXPORT: return "VK_EXT_shader_stencil_export";
    case SPV_REQ_SHADING_RATE: return "VK_KHR_fragment_shading_rate";
    case SPV_REQ_VARIABLE_POINTERS: return "VK_KHR_variable_pointers";
    case SPV_REQ_BUFFER_DEVICE_ADDRESS: return "VK_KHR_buffer_device_address";
    case SPV_REQ_VIEWPORT_INDEX_LAYER: return "VK_EXT_shader_viewport_index_layer";
    case SPV_REQ_SHADER_LAYER: return "shaderOutputLayer (Vulkan 1.2)";
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
    case SpvCapabilityShaderViewportIndexLayerEXT: return SPV_REQ_VIEWPORT_INDEX_LAYER;
    case SpvCapabilityShaderLayer: return SPV_REQ_SHADER_LAYER;
    case SpvCapabilityStorageImageReadWithoutFormat: return SPV_REQ_STORAGE_READ_WOF;
    case SpvCapabilityStorageImageWriteWithoutFormat: return SPV_REQ_STORAGE_WRITE_WOF;
    case SpvCapabilitySubgroupDispatch: case SpvCapabilityDeviceGroup:
    case SpvCapabilityGroupNonUniform: case SpvCapabilityGroupNonUniformVote:
    case SpvCapabilityGroupNonUniformArithmetic: case SpvCapabilityGroupNonUniformBallot:
    case SpvCapabilityGroupNonUniformShuffle: case SpvCapabilityGroupNonUniformShuffleRelative:
    case SpvCapabilityGroupNonUniformClustered: case SpvCapabilityGroupNonUniformQuad:
    case SpvCapabilitySubgroupBallotKHR: case SpvCapabilitySubgroupVoteKHR:
        return SPV_REQ_API_1_1;   // 声明级：subgroup/device-group 是 Vulkan 1.1 核心
    case SpvCapabilityStorageBuffer16BitAccess:
    case SpvCapabilityUniformAndStorageBuffer16BitAccess:
    case SpvCapabilityStoragePushConstant16:
    case SpvCapabilityStorageInputOutput16:
        return SPV_REQ_16BIT_STORAGE;
    case SpvCapabilityStorageBuffer8BitAccess:
    case SpvCapabilityUniformAndStorageBuffer8BitAccess:
    case SpvCapabilityStoragePushConstant8:
        return SPV_REQ_8BIT_STORAGE;
    case SpvCapabilityDenormPreserve: case SpvCapabilityDenormFlushToZero:
    case SpvCapabilitySignedZeroInfNanPreserve: case SpvCapabilityRoundingModeRTE:
    case SpvCapabilityRoundingModeRTZ:
        return SPV_REQ_FLOAT_CONTROLS;
    case SpvCapabilityDrawParameters: return SPV_REQ_DRAW_PARAMETERS;
    case SpvCapabilityVariablePointersStorageBuffer: return SPV_REQ_VARIABLE_POINTERS;
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
    if (!strcmp(name, "SPV_EXT_shader_viewport_index_layer")) return SPV_REQ_VIEWPORT_INDEX_LAYER;
    if (!strcmp(name, "SPV_KHR_16bit_storage")) return SPV_REQ_16BIT_STORAGE;
    if (!strcmp(name, "SPV_KHR_8bit_storage")) return SPV_REQ_8BIT_STORAGE;
    if (!strcmp(name, "SPV_KHR_multiview")) return SPV_REQ_MULTIVIEW;
    if (!strcmp(name, "SPV_KHR_post_depth_coverage")) return SPV_REQ_POST_DEPTH;
    if (!strcmp(name, "SPV_EXT_descriptor_indexing")) return SPV_REQ_DESCRIPTOR_INDEXING;
    if (!strcmp(name, "SPV_KHR_storage_buffer_storage_class")) return SPV_REQ_SBC_CLASS;
    if (!strcmp(name, "SPV_KHR_shader_stencil_export")) return SPV_REQ_STENCIL_EXPORT;
    if (!strcmp(name, "SPV_KHR_float_controls")) return SPV_REQ_FLOAT_CONTROLS;
    if (!strcmp(name, "SPV_KHR_variable_pointers")) return SPV_REQ_VARIABLE_POINTERS;
    if (!strcmp(name, "SPV_KHR_buffer_device_address")) return SPV_REQ_BUFFER_DEVICE_ADDRESS;
    if (!strcmp(name, "SPV_KHR_device_group") || !strcmp(name, "SPV_KHR_shader_ballot") ||
        !strcmp(name, "SPV_KHR_subgroup_vote") || !strcmp(name, "SPV_KHR_vulkan_memory_model"))
        return SPV_REQ_API_1_1;   // 这些 SPIR-V 扩展是 Vulkan 1.1+ 核心语义
    return SPV_REQ_UNLISTED;
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
        } else if (op == SpvOpVariable && len >= 4 && s2a_spv_wg_word(p, i + 3) == SpvStorageClassPushConstant) {
            size_t pt = s2a_spv_wg_type_def(p, n, s2a_spv_wg_word(p, i + 1));
            if (!pt || (s2a_spv_wg_word(p, pt) & 65535u) != SpvOpTypePointer || (s2a_spv_wg_word(p, pt) >> 16) != 4)
                return "spv compliance: push-constant variable without pointer type";
            uint64_t e = 0; const char *r = s2a_spv_wg_type_size(p, n, s2a_spv_wg_word(p, pt + 3), 0, &e);
            if (r) return r;
            if (UINT64_MAX - push < e) return "spv compliance: push-constant byte count overflow";
            push += e;
        }
        i += len;
    }
    d->push_bytes = push;
    d->has_push_bytes = push ? 1 : 0;
    return NULL;
}
