// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "input_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standalone, opt-in diagnostic contract. Not wired into a provider or draw.
 * All pointed-to data must be an owned immutable snapshot, or remain protected
 * by the producer's existing synchronization for the entire call. Matching
 * stamps alone DO NOT prove that a real producer froze the underlying data.
 * The host tests prove this contract's checks, not a device capture protocol.
 */
enum s2a_w2_query_method {
    S2A_W2_QUERY_SAMPLER_TYPE = 1,
    S2A_W2_QUERY_IS_BUFFER,
    S2A_W2_QUERY_NORMALIZED,
    S2A_W2_QUERY_FORMAT,
    S2A_W2_QUERY_SAMPLED_FORMAT
};

enum s2a_w2_query_stage {
    S2A_W2_QUERY_VS = 0,
    S2A_W2_QUERY_FS = 1,
    S2A_W2_QUERY_CS = 5
};

struct s2a_w2_query_coordinate {
    uint32_t stage, handle;
    int32_t cbuf_slot;
};

struct s2a_w2_query {
    enum s2a_w2_query_method method;
    struct s2a_w2_query_coordinate coordinate;
};

struct s2a_w2_query_stamp {
    uint64_t session, generation;
    uint64_t snapshot_id, snapshot_version;
    uint64_t source_epoch, registry_generation;
};

struct s2a_w2_query_binding {
    struct s2a_w2_query_stamp stamp;
    const uint8_t *canonical;
    size_t canonical_bytes;
};

enum {
    S2A_W2_RAW_FORMAT = 1,
    S2A_W2_RAW_TARGET = 2,
    S2A_W2_RAW_LAYERS = 4,
    S2A_W2_RAW_COMPLETE = 7
};

/* Raw observations, NOT compiler texture facts or a translated query answer.
 * Do not infer normalized/isBuffer/samplerType, map formats, or default layers.
 * Every raw field must be present; layers must be positive even for non-arrays.
 * This avoids the production texture converter's memo writes/default layer=1.
 */
struct s2a_w2_query_raw_texture {
    uint32_t present, format, target, layers;
};

struct s2a_w2_query_source {
    struct s2a_w2_query_binding binding;
    struct s2a_w2_query requested;
    struct s2a_w2_query_coordinate resolved;
    uint64_t source_id; /* Producer-owned opaque snapshot entry id, not a guest address. */
    struct s2a_w2_query_raw_texture descriptor;
    int view_present; /* Explicit absence (0) or a complete resolved view (1). */
    struct s2a_w2_query_raw_texture view;
};

/* Called at most once, for exactly one missing coordinate. The future platform
 * adapter must read ONLY its frozen snapshot, never current global state. It
 * returns nonzero when the source is missing, unsupported, stale or ambiguous.
 * Returned canonical storage must remain readable until diagnose returns.
 * A matching echo is not independent proof of the producer's provenance.
 */
typedef int (*s2a_w2_query_resolver)(void *snapshot,
                                  const struct s2a_w2_query *query,
                                  struct s2a_w2_query_source *source);

struct s2a_w2_query_input {
    int enabled; /* Only exactly 1 opts in; zero-initialized inputs are disabled. */
    const struct s2a_input_identity *identity; /* The ONLY existing-facts array. */
    struct s2a_w2_query_binding original;
    struct s2a_w2_query_stamp current;
    struct s2a_w2_query query;
};

enum s2a_w2_query_status {
    S2A_W2_QUERY_OBSERVED = 0,
    S2A_W2_QUERY_DISABLED = 1,
    S2A_W2_QUERY_INVALID = -1,
    S2A_W2_QUERY_IDENTITY = -2,
    S2A_W2_QUERY_STALE = -3,
    S2A_W2_QUERY_NOT_MISSING = -4,
    S2A_W2_QUERY_NO_SOURCE = -5,
    S2A_W2_QUERY_AMBIGUOUS = -6
};

/* Diagnostic-only receipt. No compile input, new identity, cache key, or
 * replacement fact is produced. OBSERVED means raw source metadata was checked,
 * NOT that its NVN format/target can be translated or the query can compile.
 * Any adoption needs a separately approved producer and a NEW full identity.
 */
struct s2a_w2_query_receipt {
    int observed;
    struct s2a_w2_query_stamp stamp;
    struct s2a_w2_query requested;
    struct s2a_w2_query_coordinate resolved;
    uint64_t source_id;
    struct s2a_w2_query_raw_texture descriptor;
    int view_present;
    struct s2a_w2_query_raw_texture view;
    size_t original_identity_bytes;
    uint8_t original_identity[S2A_INPUT_IDENTITY_MAX];
};

/* Output must not overlap any input/snapshot storage. Cleared on every failure
 * and when disabled. The module performs no allocation, logging, file I/O,
 * compilation, cache access, or mutation of existing facts/identity inputs.
 */
int s2a_w2_single_query_diagnose(const struct s2a_w2_query_input *input,
                               s2a_w2_query_resolver resolver, void *snapshot,
                               struct s2a_w2_query_receipt *receipt);

#ifdef __cplusplus
}
#endif
