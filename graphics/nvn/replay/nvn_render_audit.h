// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stdint.h>

enum nvn_render_audit_kind {
    NVN_RA_DESCRIPTOR, NVN_RA_VIEW, NVN_RA_ATTACHMENT, NVN_RA_SKIP,
    NVN_RA_UNIMPL, NVN_RA_STICKY, NVN_RA_SEGMENT, NVN_RA_KIND_COUNT
};

struct nvn_render_audit_binding {
    uint64_t program;
    int stage, set, binding, element;
    uint32_t format, target, aspect, level, layer;
};

// Player Android builds compile the audit sites out. Linux Vulkan fixtures exercise the same code.
// Optional references keep small backend/replay fixtures that stub nvn_unimpl linkable.
#if (defined(__ANDROID__) && defined(S2A_ANDROID_DEBUG_ENV)) || (defined(__linux__) && !defined(__ANDROID__))
#define NVN_RENDER_AUDIT_SUPPORTED 1
#ifdef S2A_RENDER_AUDIT_IMPLEMENTATION
#define NVN_RA_OPTIONAL
#else
#define NVN_RA_OPTIONAL __attribute__((weak))
#endif
extern int nvn_render_audit_enabled NVN_RA_OPTIONAL;
void nvn_render_audit_begin(int enabled) NVN_RA_OPTIONAL;
void nvn_render_audit_end(void) NVN_RA_OPTIONAL;
void nvn_render_audit_present(void) NVN_RA_OPTIONAL;
void nvn_render_audit_context(uint64_t submit, unsigned op) NVN_RA_OPTIONAL;
void nvn_render_audit_empty(uint32_t count) NVN_RA_OPTIONAL;
void nvn_render_audit_event(enum nvn_render_audit_kind kind, const char *reason,
                            const struct nvn_render_audit_binding *binding, const char *detail) NVN_RA_OPTIONAL;
void nvn_render_audit_segment(int code, uint64_t fence, const char *detail) NVN_RA_OPTIONAL;
#undef NVN_RA_OPTIONAL

static inline int nvn_render_audit_on(void) {
#ifndef S2A_RENDER_AUDIT_IMPLEMENTATION
    if (!&nvn_render_audit_enabled) return 0;
#endif
    return __atomic_load_n(&nvn_render_audit_enabled, __ATOMIC_RELAXED);
}
#define NVN_RA_IF(...) do { if (__builtin_expect(nvn_render_audit_on(), 0)) { __VA_ARGS__; } } while (0)
#define NVN_RA_BEGIN(enabled) do { if (nvn_render_audit_begin) nvn_render_audit_begin(enabled); } while (0)
#else
#define NVN_RENDER_AUDIT_SUPPORTED 0
#define NVN_RA_IF(...) ((void)0)
#define NVN_RA_BEGIN(enabled) ((void)0)
#endif
