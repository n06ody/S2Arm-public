/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 S2Arm contributors */
#pragma once
#include "../direct/jv.h"
#include <limits.h>
#include <string.h>

/* DirectShaderCompiler live result v1. A 16-byte header contains S2ADRB1\0,
 * total LE32 length and zero reserved LE32. An unnamed object follows.
 * Records: LE32 payload bytes, U8 type, U8 field id, zero LE16, then
 * payload (zero-padded to a multiple of four). Field 0 is unnamed.
 * Object members are named and unique; array items are unnamed. Scalar
 * records hold raw text, a LE32/LE64 integer, a tag-only bool/null, or raw SPIR-V.
 * Only the named "spirv" field can contain SPIR-V. No pointer crosses the ABI. */
enum nvk_result_tag {
    RV_OBJECT = 1, RV_ARRAY, RV_TEXT, RV_UINT, RV_INT, RV_FALSE, RV_TRUE, RV_NULL, RV_SPIRV
};
enum { RV_HEADER = 16, RV_LIMIT = 64u << 20 };
static const char *const rv_fields[] = {
    "",
#define RESULT_FIELD(name) name,
#include "../../../graphics/tools/direct/compiler/DirectResultFields.inc"
#undef RESULT_FIELD
};
struct nvk_result_value {
    const char *p;
    size_t n;
    unsigned tag;  /* Zero denotes the legacy JSON view. */
};
struct nvk_result_child {
    struct nvk_result_value value;
    const char *key;
    unsigned key_len;
    size_t consumed;
};

static inline uint32_t rv_le32(const char *p) {
    const uint8_t *x = (const uint8_t *)p;
    return (uint32_t)x[0] | (uint32_t)x[1] << 8 | (uint32_t)x[2] << 16 | (uint32_t)x[3] << 24;
}
static inline uint64_t rv_le64(const char *p) {
    return (uint64_t)rv_le32(p) | (uint64_t)rv_le32(p + 4) << 32;
}
static inline struct s2a_jv rv_json(struct nvk_result_value v) {
    return (struct s2a_jv){v.p, v.n};
}
static inline struct nvk_result_value rv_legacy(struct s2a_jv v) {
    return (struct nvk_result_value){v.p, v.n, 0};
}
static inline int rv_is_binary(const uint8_t *data, size_t len) {
    return data && len >= 8 && !memcmp(data, "S2ADRB1", 8);
}
static inline int rv_token(const char *p, size_t remaining, struct nvk_result_child *out) {
    if (remaining < 8 || remaining > RV_LIMIT || p[6] || p[7]) return -1;
    const uint32_t n = rv_le32(p);
    const unsigned tag = (unsigned char)p[4], field = (unsigned char)p[5];
    const size_t pad = ((size_t)n + 3u) & ~(size_t)3u;
    if (field >= sizeof rv_fields / sizeof *rv_fields || n > remaining - 8 || pad > remaining - 8) return -1;
    const char *key = rv_fields[field];
    *out = (struct nvk_result_child){{p + 8, n, tag}, key, (unsigned)strlen(key), 8 + pad};
    return 0;
}
static inline int rv_zero(const char *p, size_t n) {
    while (n--) if (*p++) return 0;
    return 1;
}
static inline int rv_validate(struct nvk_result_value v, unsigned depth) {
    if (depth > 24) return -1;
    switch (v.tag) {
    case RV_TEXT: return 0;
    case RV_UINT: return v.n == 4 || v.n == 8 ? 0 : -1;
    case RV_INT: return ((v.n == 4 && rv_le32(v.p) > INT32_MAX) ||
                         (v.n == 8 && rv_le64(v.p) > INT64_MAX)) ? 0 : -1;
    case RV_FALSE:
    case RV_TRUE:
    case RV_NULL: return v.n ? -1 : 0;
    case RV_SPIRV:
        return v.n >= 20 && !(v.n & 3u) && !((uintptr_t)v.p & 3u) &&
               rv_le32(v.p) == 0x07230203u ? 0 : -1;
    case RV_OBJECT:
    case RV_ARRAY: break;
    default: return -1;
    }
    struct nvk_result_child fields[64];
    unsigned count = 0;
    for (size_t i = 0; i < v.n;) {
        struct nvk_result_child c;
        if (rv_token(v.p + i, v.n - i, &c) ||
            !rv_zero(c.value.p + c.value.n, (size_t)(v.p + i + c.consumed - c.value.p) - c.value.n))
            return -1;
        if (v.tag == RV_OBJECT) {
            if (!c.key_len || count == sizeof fields / sizeof *fields) return -1;
            for (unsigned k = 0; k < count; ++k)
                if (fields[k].key == c.key) return -1;
            fields[count++] = c;
        } else if (c.key_len) return -1;
        if (c.value.tag == RV_SPIRV && (c.key_len != 5 || memcmp(c.key, "spirv", 5))) return -1;
        if (rv_validate(c.value, depth + 1)) return -1;
        i += c.consumed;
    }
    return 0;
}
static inline int rv_frame(const uint8_t *bytes, size_t len, struct nvk_result_value *out) {
    if (!rv_is_binary(bytes, len) || len < RV_HEADER + 8 || len > RV_LIMIT || ((uintptr_t)bytes & 3u))
        return -1;
    const char *p = (const char *)bytes;
    struct nvk_result_child root;
    if (rv_le32(p + 8) != len || rv_le32(p + 12) ||
        rv_token(p + RV_HEADER, len - RV_HEADER, &root) ||
        root.key_len || root.value.tag != RV_OBJECT || root.value.n != len - RV_HEADER - 8)
        return -1;
    *out = root.value;
    return 0;
}

static inline int rv_root(const uint8_t *bytes, size_t len, int allow_binary, struct nvk_result_value *out) {
    if (!bytes || !len) return -1;
    if (!rv_is_binary(bytes, len)) {
        *out = (struct nvk_result_value){(const char *)bytes, len, 0};
        return 0;
    }
    return !allow_binary || rv_frame(bytes, len, out) || rv_validate(*out, 0) ? -1 : 0;
}

static inline int rv_is_object(struct nvk_result_value v) {
    return v.tag ? v.tag == RV_OBJECT : s2a_jv_is_object(rv_json(v));
}
static inline int rv_is_array(struct nvk_result_value v) {
    return v.tag ? v.tag == RV_ARRAY : s2a_jv_is_array(rv_json(v));
}
static inline int rv_is_null(struct nvk_result_value v) {
    return v.tag ? v.tag == RV_NULL : s2a_jv_is_null(rv_json(v));
}
static inline int rv_is_string(struct nvk_result_value v) {
    return v.tag ? v.tag == RV_TEXT : s2a_jv_is_string(rv_json(v));
}
static inline int rv_str(struct nvk_result_value v, char *dst, size_t cap) {
    if (!v.tag) return s2a_jv_str(rv_json(v), dst, cap);
    if (v.tag != RV_TEXT || v.n >= cap || memchr(v.p, 0, v.n)) return -1;
    memcpy(dst, v.p, v.n);
    dst[v.n] = 0;
    return 0;
}
static inline int rv_u64(struct nvk_result_value v, uint64_t *out) {
    if (v.tag != RV_UINT || (v.n != 4 && v.n != 8)) return -1;
    *out = v.n == 4 ? rv_le32(v.p) : rv_le64(v.p);
    return 0;
}
static inline int rv_u32(struct nvk_result_value v, uint32_t *out) {
    if (!v.tag) return s2a_jv_u32(rv_json(v), out);
    uint64_t n;
    if (rv_u64(v, &n) || n > UINT32_MAX) return -1;
    *out = (uint32_t)n;
    return 0;
}
static inline int rv_bool(struct nvk_result_value v, int *out) {
    if (!v.tag) return s2a_jv_bool(rv_json(v), out);
    if (v.tag != RV_TRUE && v.tag != RV_FALSE) return -1;
    *out = v.tag == RV_TRUE;
    return 0;
}
static inline int rv_obj_find(struct nvk_result_value v, const char *name, struct nvk_result_value *out) {
    if (!v.tag) {
        struct s2a_jv found;
        if (s2a_jv_obj_find(rv_json(v), name, &found)) return -1;
        *out = rv_legacy(found);
        return 0;
    }
    if (v.tag != RV_OBJECT) return -1;
    size_t n = strlen(name);
    for (size_t i = 0; i < v.n;) {
        struct nvk_result_child c;
        if (rv_token(v.p + i, v.n - i, &c)) return -1;
        if (n == c.key_len && !memcmp(c.key, name, n)) { *out = c.value; return 0; }
        i += c.consumed;
    }
    return -1;
}
static inline int rv_arr_count(struct nvk_result_value v) {
    if (!v.tag) return s2a_jv_arr_count(rv_json(v));
    if (v.tag != RV_ARRAY) return -1;
    int count = 0;
    for (size_t i = 0; i < v.n; ++count) {
        struct nvk_result_child c;
        if (rv_token(v.p + i, v.n - i, &c)) return -1;
        i += c.consumed;
    }
    return count;
}
static inline int rv_arr_item(struct nvk_result_value v, int index, struct nvk_result_value *out) {
    if (!v.tag) {
        struct s2a_jv found;
        if (s2a_jv_arr_item(rv_json(v), index, &found)) return -1;
        *out = rv_legacy(found);
        return 0;
    }
    if (v.tag != RV_ARRAY || index < 0) return -1;
    for (size_t i = 0; i < v.n;) {
        struct nvk_result_child c;
        if (rv_token(v.p + i, v.n - i, &c)) return -1;
        if (index-- == 0) { *out = c.value; return 0; }
        i += c.consumed;
    }
    return -1;
}

/* Linear iteration for dependencies, which may contain thousands of entries. */
static inline int rv_array_next(struct nvk_result_value arr, size_t *position, struct nvk_result_value *out) {
    if (!rv_is_array(arr) || *position > arr.n) return -1;
    if (arr.tag) {
        struct nvk_result_child c;
        if (*position == arr.n) return 0;
        if (rv_token(arr.p + *position, arr.n - *position, &c)) return -1;
        *position += c.consumed;
        *out = c.value;
        return 1;
    }
    const char *end = arr.p + arr.n;
    const char *p = s2a_jv_ws(arr.p + (*position ? *position : 1), end), *next;
    if (p < end && *p == ']') return 0;
    if (s2a_jv_scan(p, end, &next)) return -1;
    *out = (struct nvk_result_value){p, (size_t)(next - p), 0};
    next = s2a_jv_ws(next, end);
    if (next >= end || (*next != ',' && *next != ']')) return -1;
    *position = (size_t)(next - arr.p) + (*next == ',');
    return 1;
}
