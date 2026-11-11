// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "compile_request.h"

#ifdef __cplusplus
extern "C" {
#endif

// Metadata only: no source, canonical identity encoding, request or result payload.
#define S2A_W2_ENTRY_CAPACITY 128
#define S2A_W2_SOURCE_COUNT 5

enum s2a_w2_kind { S2A_W2_GRAPHICS = 1, S2A_W2_COMPUTE = 2 };
enum s2a_w2_source {
    S2A_W2_UNKNOWN = -1, S2A_W2_MEMO = 0, S2A_W2_MEMORY, S2A_W2_OFFLINE, S2A_W2_PRIVATE, S2A_W2_LIVE
};
enum s2a_w2_state { S2A_W2_ADMIT = 1, S2A_W2_CLOSING, S2A_W2_SEALED };
struct s2a_w2_window;
struct s2a_w2_scope {
    struct s2a_w2_window *window;
    struct s2a_w2_scope *previous;
    uint64_t expected_shaders, begun_shaders, active_shaders;
    unsigned entered, blocked;
};
struct s2a_w2_shader {
    struct s2a_w2_window *window;
    struct s2a_w2_scope *scope;
    struct s2a_w2_shader *previous;
    unsigned entered, blocked, lookup_seen, lookup_accepted, rejected, kind;
    unsigned lookup_slot, lookup_source; // 已计的有效 lookup（1 起，用于撤销内部同步重试）
};
struct s2a_w2_entry {
    uint8_t identity[32];
    uint32_t kind;
    // Accepted lookups and rejected lookups are separate; neither is GPU proof.
    uint64_t source_counts[S2A_W2_SOURCE_COUNT];
    uint64_t rejected_source_counts[S2A_W2_SOURCE_COUNT], unknown_rejects;
};
struct s2a_w2_snapshot {
    uint64_t instance, revision, session, generation;
    uint32_t direct_abi;
    char closure[128], profile_hash[72];
    uint64_t scopes, inflight_scopes, shaders, finished_shaders, inflight_shaders;
    uint64_t observations, accepted_builds, gaps, rejects, overflow, late, unbound;
    uint64_t missing_identity, cohort_mismatch, build_failures, protocol_errors;
    uint64_t stop_failures, drain_failures, output_failures;
    uint32_t state, entry_count;
    int stop_ok, drained, metadata_complete, window_complete;
    // These claims are intentionally never proven by this metadata-only observer.
    int request_set_proven, strict_proven, gpu_proven, w2_acceptance_proven;
    struct s2a_w2_entry entries[S2A_W2_ENTRY_CAPACITY];
};

#if defined(S2A_ANDROID_VK_ENGINE) && S2A_ANDROID_VK_ENGINE && \
    defined(S2A_ANDROID_VK_BACKEND_NVN_VK) && S2A_ANDROID_VK_BACKEND_NVN_VK && \
    defined(S2A_ANDROID_DEBUG_ENV)
// Debug has one strong implementation in provider_vk.c; release/engine-off use inline no-ops.
struct s2a_w2_window *s2a_w2_start(int enabled, const struct s2a_direct_compile_context *context);
void s2a_w2_close(struct s2a_w2_window *window);
int s2a_w2_seal(struct s2a_w2_window *window, int stop_ok, int drained);
void s2a_w2_release(struct s2a_w2_window *window);
void s2a_w2_scope_enter(struct s2a_w2_scope *scope);
void s2a_w2_scope_expect_shader(struct s2a_w2_scope *scope);
void s2a_w2_scope_leave(struct s2a_w2_scope *scope, int success);
void s2a_w2_shader_begin(struct s2a_w2_shader *shader, enum s2a_w2_kind kind);
void s2a_w2_shader_lookup(const struct s2a_direct_compile_context *context,
                         const uint8_t identity[32], enum s2a_w2_source source, int accepted);
// success is a boolean derived from the unchanged runtime return code, not GPU proof.
// success: 1 = 构建完成，0 = 失败，-1 = 内部同步后重试（有效查找才撤销本次尝试）。
void s2a_w2_shader_end(struct s2a_w2_shader *shader, int success);
int s2a_w2_snapshot(struct s2a_w2_window *window, struct s2a_w2_snapshot *snapshot);
// Ack accepts only the same live instance/revision and an actually successful output.
int s2a_w2_ack(struct s2a_w2_window *window, const struct s2a_w2_snapshot *snapshot,
               int output_ok);
// The sink must return 0 only after the complete snapshot has actually been written.
// It runs without the observer mutex. The immutable snapshot never claims persisted completion.
int s2a_w2_export(struct s2a_w2_window *window,
                  int (*writer)(void *, const char *, size_t), void *opaque);
#else
static inline struct s2a_w2_window *s2a_w2_start(
    int enabled, const struct s2a_direct_compile_context *context) {
    (void)enabled; (void)context; return NULL;
}
static inline void s2a_w2_close(struct s2a_w2_window *w) { (void)w; }
static inline int s2a_w2_seal(struct s2a_w2_window *w, int ok, int drained) {
    (void)w; (void)ok; (void)drained; return 0;
}
static inline void s2a_w2_release(struct s2a_w2_window *w) { (void)w; }
static inline void s2a_w2_scope_enter(struct s2a_w2_scope *s) { (void)s; }
static inline void s2a_w2_scope_expect_shader(struct s2a_w2_scope *s) { (void)s; }
static inline void s2a_w2_scope_leave(struct s2a_w2_scope *s, int success) {
    (void)s; (void)success;
}
static inline void s2a_w2_shader_begin(struct s2a_w2_shader *s, enum s2a_w2_kind k) {
    (void)s; (void)k;
}
static inline void s2a_w2_shader_lookup(const struct s2a_direct_compile_context *c,
                                      const uint8_t id[32], enum s2a_w2_source src, int accepted) {
    (void)c; (void)id; (void)src; (void)accepted;
}
static inline void s2a_w2_shader_end(struct s2a_w2_shader *s, int success) {
    (void)s; (void)success;
}
static inline int s2a_w2_snapshot(struct s2a_w2_window *w, struct s2a_w2_snapshot *s) {
    (void)w; (void)s; return -1;
}
static inline int s2a_w2_ack(struct s2a_w2_window *w, const struct s2a_w2_snapshot *s, int ok) {
    (void)w; (void)s; (void)ok; return -1;
}
static inline int s2a_w2_export(struct s2a_w2_window *w,
                                int (*writer)(void *, const char *, size_t), void *opaque) {
    (void)w; (void)writer; (void)opaque; return -1;
}
#endif

#ifdef __cplusplus
}
#endif
