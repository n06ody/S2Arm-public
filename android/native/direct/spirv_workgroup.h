// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Inspect the selected compute entry's default workgroup size before creating a
// driver shader. This is not a full SPIR-V validator. Unknown size expressions
// fail explicitly; cpipe_get separately rejects specialization overrides.
#pragma once
#include "spirv_storage.h"
#include "../../../graphics/third_party/spirv_cross/spirv.h"

static inline uint32_t s2a_spv_wg_word(const unsigned char *p, size_t i) {
    return s2a_spv_word(p + i * 4);
}

// Called after the complete instruction stream was checked for bounded lengths.
// Only a few size/type IDs are needed: scans avoid an allocation indexed by an
// untrusted (and possibly sparse) module ID bound.
static inline size_t s2a_spv_wg_def(const unsigned char *p, size_t n, uint32_t id, uint32_t type_op) {
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (type_op) {
            if (op == type_op && len >= 2 && s2a_spv_wg_word(p, i + 1) == id) return i;
        } else if (op >= SpvOpConstantTrue && op <= SpvOpSpecConstantOp && len >= 3 &&
                   s2a_spv_wg_word(p, i + 2) == id) return i;
        i += len;
    }
    return 0;
}

static inline const char *s2a_spirv_workgroup_size(const void *blob, size_t bytes, const char *entry, uint32_t size[3]) {
    const unsigned char *p = (const unsigned char *)blob;
    if (!p || bytes < 20 || (bytes & 3u) || s2a_spv_word(p) != SpvMagicNumber)
        return "compute workgroup: malformed SPIR-V header";
    const size_t n = bytes / 4;
    const uint32_t bound = s2a_spv_wg_word(p, 3);
    uint32_t function = 0, builtin = 0;
    if (!bound || !entry) return "compute workgroup: missing entry or ID bound";
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (!len || len > n - i) return "compute workgroup: malformed instruction length";
        if (op == SpvOpEntryPoint) {
            if (len < 4) return "compute workgroup: malformed entry point";
            const char *name = (const char *)(p + (i + 3) * 4);
            if (!memchr(name, 0, (len - 3) * 4)) return "compute workgroup: unterminated entry name";
            if (s2a_spv_wg_word(p, i + 1) == SpvExecutionModelGLCompute && !strcmp(name, entry)) {
                if (function) return "compute workgroup: ambiguous compute entry";
                function = s2a_spv_wg_word(p, i + 2);
                if (!function || function >= bound) return "compute workgroup: invalid entry ID";
            }
        } else if (op == SpvOpDecorate && len >= 4 && s2a_spv_wg_word(p, i + 2) == SpvDecorationBuiltIn &&
                   s2a_spv_wg_word(p, i + 3) == SpvBuiltInWorkgroupSize) {
            uint32_t id = s2a_spv_wg_word(p, i + 1);
            if (len != 4 || !id || id >= bound || (builtin && builtin != id))
                return "compute workgroup: invalid or ambiguous WorkgroupSize";
            builtin = id;
        }
        i += len;
    }
    if (!function) return "compute workgroup: selected GLCompute entry not found";
    int have_local = 0;
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if ((op == SpvOpExecutionMode || op == SpvOpExecutionModeId) && len >= 3 &&
            s2a_spv_wg_word(p, i + 1) == function) {
            const uint32_t mode = s2a_spv_wg_word(p, i + 2);
            // vk_device currently enables neither Vulkan 1.3 nor maintenance4.
            if (mode == SpvExecutionModeLocalSizeId)
                return "compute workgroup: LocalSizeId requires maintenance4 (not enabled)";
            if (mode == SpvExecutionModeLocalSize) {
                if (op != SpvOpExecutionMode || len != 6 || have_local)
                    return "compute workgroup: invalid LocalSize execution mode";
                for (unsigned k = 0; k < 3; k++) size[k] = s2a_spv_wg_word(p, i + 3 + k);
                have_local = 1;
            }
        }
        i += len;
    }
    // Vulkan WorkgroupSize overrides LocalSize, including specialization
    // constant defaults. See refpages/latest/refpages/source/WorkgroupSize.html.
    if (!builtin) return have_local ? NULL : "compute workgroup: no LocalSize or WorkgroupSize";
    size_t def = s2a_spv_wg_def(p, n, builtin, 0);
    if (!def) return "compute workgroup: WorkgroupSize is not a constant";
    uint32_t first = s2a_spv_wg_word(p, def), op = first & 65535u;
    if ((first >> 16) != 6 || (op != SpvOpConstantComposite && op != SpvOpSpecConstantComposite))
        return "compute workgroup: unsupported WorkgroupSize constant expression";
    uint32_t type = s2a_spv_wg_word(p, def + 1);
    size_t vec = s2a_spv_wg_def(p, n, type, SpvOpTypeVector);
    if (!vec || (s2a_spv_wg_word(p, vec) >> 16) != 4 || s2a_spv_wg_word(p, vec + 3) != 3)
        return "compute workgroup: WorkgroupSize must be a three-component vector";
    uint32_t component = s2a_spv_wg_word(p, vec + 2);
    size_t integer = s2a_spv_wg_def(p, n, component, SpvOpTypeInt);
    if (!integer || (s2a_spv_wg_word(p, integer) >> 16) != 4 || s2a_spv_wg_word(p, integer + 2) != 32)
        return "compute workgroup: WorkgroupSize must contain 32-bit integers";
    for (unsigned k = 0; k < 3; k++) {
        uint32_t id = s2a_spv_wg_word(p, def + 3 + k);
        size_t scalar = s2a_spv_wg_def(p, n, id, 0);
        if (!scalar) return "compute workgroup: WorkgroupSize component is not a constant";
        first = s2a_spv_wg_word(p, scalar); op = first & 65535u;
        if ((first >> 16) != 4 || (op != SpvOpConstant && op != SpvOpSpecConstant) ||
            s2a_spv_wg_word(p, scalar + 1) != component)
            return "compute workgroup: unsupported WorkgroupSize component expression";
        size[k] = s2a_spv_wg_word(p, scalar + 3);
    }
    return NULL;
}

// Total bytes of all Workgroup storage-class variables, for the device
// maxComputeSharedMemorySize gate (libpack compute emit). Sizes follow the
// declared type graph; spec-constant lengths, runtime arrays, opaque/image
// types and non-constant expressions fail explicitly rather than estimating.
static inline size_t s2a_spv_wg_type_def(const unsigned char *p, size_t n, uint32_t id) {
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (len >= 2 && op >= SpvOpTypeVoid && op <= SpvOpTypeForwardPointer &&
            s2a_spv_wg_word(p, i + 1) == id) return i;
        i += len;
    }
    return 0;
}

static inline const char *s2a_spv_wg_type_size(const unsigned char *p, size_t n,
                                               uint32_t id, int depth, uint64_t *out) {
    if (depth > 8) return "compute shared: type graph too deep";
    size_t def = s2a_spv_wg_type_def(p, n, id);
    if (!def) return "compute shared: type ID has no definition";
    uint32_t first = s2a_spv_wg_word(p, def), op = first & 65535u, len = first >> 16;
    switch (op) {
    case SpvOpTypeInt: case SpvOpTypeFloat: {
        if (len != 4) return "compute shared: malformed scalar type";
        uint32_t bits = s2a_spv_wg_word(p, def + 2);
        if (!bits || (bits & 7u)) return "compute shared: non-byte scalar width";
        *out = bits / 8; return NULL; }
    case SpvOpTypeVector: case SpvOpTypeMatrix: {
        if (len != 4) return "compute shared: malformed vector/matrix type";
        uint64_t e = 0; const char *r = s2a_spv_wg_type_size(p, n, s2a_spv_wg_word(p, def + 2), depth + 1, &e);
        if (r) return r;
        *out = e * s2a_spv_wg_word(p, def + 3); return NULL; }
    case SpvOpTypeArray: {
        if (len != 4) return "compute shared: malformed array type";
        size_t cdef = s2a_spv_wg_def(p, n, s2a_spv_wg_word(p, def + 3), 0);
        if (!cdef || (s2a_spv_wg_word(p, cdef) & 65535u) != SpvOpConstant || (s2a_spv_wg_word(p, cdef) >> 16) < 4)
            return "compute shared: array length is not a plain constant";
        uint64_t e = 0; const char *r = s2a_spv_wg_type_size(p, n, s2a_spv_wg_word(p, def + 2), depth + 1, &e);
        if (r) return r;
        *out = e * s2a_spv_wg_word(p, cdef + 3); return NULL; }
    case SpvOpTypeRuntimeArray:
        return "compute shared: runtime-sized workgroup array";
    case SpvOpTypeStruct: {
        uint64_t total = 0;
        for (uint32_t m = 2; m < len; m++) {
            uint64_t e = 0; const char *r = s2a_spv_wg_type_size(p, n, s2a_spv_wg_word(p, def + m), depth + 1, &e);
            if (r) return r;
            if (UINT64_MAX - total < e) return "compute shared: byte count overflow";
            total += e;
        }
        *out = total; return NULL; }
    default:
        return "compute shared: unsupported type in workgroup variable";
    }
}

static inline const char *s2a_spv_workgroup_bytes(const void *blob, size_t bytes, uint64_t *total) {
    const unsigned char *p = (const unsigned char *)blob;
    *total = 0;
    if (!p || bytes < 20 || (bytes & 3u) || s2a_spv_word(p) != SpvMagicNumber)
        return "compute shared: malformed SPIR-V header";
    const size_t n = bytes / 4;
    for (size_t i = 5; i < n;) {
        uint32_t first = s2a_spv_wg_word(p, i), len = first >> 16, op = first & 65535u;
        if (!len || len > n - i) return "compute shared: malformed instruction length";
        if (op == SpvOpVariable && len >= 4 && s2a_spv_wg_word(p, i + 3) == SpvStorageClassWorkgroup) {
            size_t pt = s2a_spv_wg_type_def(p, n, s2a_spv_wg_word(p, i + 1));
            if (!pt || (s2a_spv_wg_word(p, pt) & 65535u) != SpvOpTypePointer || (s2a_spv_wg_word(p, pt) >> 16) != 4)
                return "compute shared: workgroup variable without pointer type";
            uint64_t e = 0; const char *r = s2a_spv_wg_type_size(p, n, s2a_spv_wg_word(p, pt + 3), 0, &e);
            if (r) return r;
            if (UINT64_MAX - *total < e) return "compute shared: byte count overflow";
            *total += e;
        }
        i += len;
    }
    return NULL;
}
