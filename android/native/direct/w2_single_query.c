// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "w2_single_query.h"

#include <limits.h>
#include <string.h>

static int stamp_equal(const struct s2a_w2_query_stamp *a,
                       const struct s2a_w2_query_stamp *b) {
    return a->session == b->session && a->generation == b->generation &&
        a->snapshot_id == b->snapshot_id && a->snapshot_version == b->snapshot_version &&
        a->source_epoch == b->source_epoch && a->registry_generation == b->registry_generation;
}

static int coordinate_equal(const struct s2a_w2_query_coordinate *a,
                            const struct s2a_w2_query_coordinate *b) {
    return a->stage == b->stage && a->handle == b->handle && a->cbuf_slot == b->cbuf_slot;
}

static int query_equal(const struct s2a_w2_query *a, const struct s2a_w2_query *b) {
    return a->method == b->method && coordinate_equal(&a->coordinate, &b->coordinate);
}

static int stage_number(const char *stage) {
    if (!stage) return -1;
    if (!strcmp(stage, "vs")) return S2A_W2_QUERY_VS;
    if (!strcmp(stage, "fs")) return S2A_W2_QUERY_FS;
    if (!strcmp(stage, "cs")) return S2A_W2_QUERY_CS;
    return -1;
}

static int coordinate_valid(const struct s2a_w2_query_coordinate *c) {
    return (c->stage == S2A_W2_QUERY_VS || c->stage == S2A_W2_QUERY_FS ||
            c->stage == S2A_W2_QUERY_CS) && c->handle <= INT_MAX &&
        c->cbuf_slot >= -1 && c->cbuf_slot < 32;
}

static int canonical_matches(const struct s2a_input_identity *identity,
                             const uint8_t *original, size_t bytes) {
    uint8_t encoded[S2A_INPUT_IDENTITY_MAX];
    size_t n = 0;
    if (s2a_input_identity_encode(identity, encoded, sizeof encoded, &n)) return 0;
    return n == bytes && !memcmp(encoded, original, n);
}

static int facts_valid(const struct s2a_input_identity *identity) {
    for (int i = 0; i < identity->texture_count; i++) {
        const struct s2a_compile_texture_src *f = &identity->textures[i];
        int stage = stage_number(f->stage);
        if (stage < 0 || !(identity->stage_mask & (1u << stage)) ||
            f->handle > INT_MAX || f->cbuf_slot < -1 || f->cbuf_slot >= 32 ||
            (f->is_buffer != 0 && f->is_buffer != 1) ||
            (f->normalized != 0 && f->normalized != 1) ||
            (f->separate != 0 && f->separate != 1)) return S2A_W2_QUERY_INVALID;
        for (int j = 0; j < i; j++) {
            const struct s2a_compile_texture_src *other = &identity->textures[j];
            if (stage_number(other->stage) == stage && other->handle == f->handle &&
                other->cbuf_slot == f->cbuf_slot) return S2A_W2_QUERY_AMBIGUOUS;
        }
    }
    return 0;
}

/* Mirrors the EXISTING missingness predicate, not a new query evaluator:
 * DirectGpuAccessor.NeedTexture chooses exact first, then same-stage/handle -1
 * only for a cbuf query. QueryTextureFormat alone still misses on Unknown;
 * sampled-format queries accept Unknown for known non-integer formats.
 * Do not borrow across stage/handle or let a concrete fallback override an
 * exact Unknown fact. No new fact is synthesized here.
 */
static int already_answered(const struct s2a_input_identity *identity,
                            const struct s2a_w2_query *query) {
    const struct s2a_compile_texture_src *exact = NULL, *direct = NULL;
    for (int i = 0; i < identity->texture_count; i++) {
        const struct s2a_compile_texture_src *f = &identity->textures[i];
        if (stage_number(f->stage) != (int)query->coordinate.stage ||
            f->handle != query->coordinate.handle) continue;
        if (f->cbuf_slot == query->coordinate.cbuf_slot) exact = f;
        if (f->cbuf_slot == -1) direct = f;
    }
    const struct s2a_compile_texture_src *fact = exact;
    if (!fact && query->coordinate.cbuf_slot >= 0 &&
        (query->method != S2A_W2_QUERY_SAMPLED_FORMAT || query->coordinate.cbuf_slot == 2)) fact = direct;
    if (!fact) return 0;
    return query->method != S2A_W2_QUERY_FORMAT || strcmp(fact->format, "Unknown") != 0;
}

static int raw_complete(const struct s2a_w2_query_raw_texture *raw) {
    /* Deliberately narrow raw-snapshot contract: observed zero format/target
     * are kept verbatim, but absent/zero layers are never defaulted to one.
     * No NVN target/format mapping or cubemap arithmetic is performed.
     */
    return raw->present == S2A_W2_RAW_COMPLETE && raw->layers > 0;
}

static int source_matches(const struct s2a_w2_query_source *source,
                          const struct s2a_w2_query *query,
                          const struct s2a_w2_query_stamp *stamp,
                          const uint8_t *canonical, size_t bytes) {
    if (!source->binding.canonical || source->binding.canonical_bytes != bytes ||
        memcmp(source->binding.canonical, canonical, bytes) ||
        !stamp_equal(&source->binding.stamp, stamp) ||
        !query_equal(&source->requested, query) || !source->source_id) return 0;
    const struct s2a_w2_query_coordinate *r = &source->resolved, *q = &query->coordinate;
    if (r->stage != q->stage || r->handle != q->handle ||
        !(r->cbuf_slot == q->cbuf_slot || (q->cbuf_slot >= 0 && r->cbuf_slot == -1 &&
          (query->method != S2A_W2_QUERY_SAMPLED_FORMAT || q->cbuf_slot == 2)))) return 0;
    if (!raw_complete(&source->descriptor)) return 0;
    if (source->view_present == 1) return raw_complete(&source->view);
    if (source->view_present != 0) return 0;
    return source->view.present == 0 && source->view.format == 0 &&
        source->view.target == 0 && source->view.layers == 0;
}

int s2a_w2_single_query_diagnose(const struct s2a_w2_query_input *input,
                               s2a_w2_query_resolver resolver, void *snapshot,
                               struct s2a_w2_query_receipt *receipt) {
    if (!receipt) return S2A_W2_QUERY_INVALID;
    memset(receipt, 0, sizeof *receipt);
    if (!input) return S2A_W2_QUERY_INVALID;
    if (input->enabled != 1) return S2A_W2_QUERY_DISABLED;
    if (!input->identity || !resolver || !input->original.canonical ||
        !input->original.canonical_bytes || input->original.canonical_bytes > S2A_INPUT_IDENTITY_MAX ||
        !input->original.stamp.session || !input->original.stamp.generation ||
        !input->original.stamp.snapshot_id ||
        !coordinate_valid(&input->query.coordinate) ||
        input->query.method < S2A_W2_QUERY_SAMPLER_TYPE || input->query.method > S2A_W2_QUERY_SAMPLED_FORMAT ||
        !(input->identity->stage_mask & (1u << input->query.coordinate.stage)))
        return S2A_W2_QUERY_INVALID;
    if (!stamp_equal(&input->original.stamp, &input->current)) return S2A_W2_QUERY_STALE;

    /* Retain the original bytes across the callback. Replacing both the live
     * facts and the caller's expected bytes must not turn a mismatch into pass.
     * These sequential checks only detect visible mutation; they are NOT a
     * replacement for the producer's ownership/synchronization, nor ABA proof.
     */
    uint8_t original[S2A_INPUT_IDENTITY_MAX];
    size_t bytes = input->original.canonical_bytes;
    memcpy(original, input->original.canonical, bytes);
    struct s2a_w2_query_stamp stamp = input->original.stamp;
    struct s2a_w2_query query = input->query;
    if (!canonical_matches(input->identity, original, bytes)) return S2A_W2_QUERY_IDENTITY;
    int rc = facts_valid(input->identity);
    if (rc) return rc;
    if (already_answered(input->identity, &query)) return S2A_W2_QUERY_NOT_MISSING;

    struct s2a_w2_query_source source = {0};
    if (resolver(snapshot, &query, &source)) return S2A_W2_QUERY_NO_SOURCE;
    if (!stamp_equal(&input->current, &stamp) || !stamp_equal(&input->original.stamp, &stamp) ||
        !query_equal(&input->query, &query) || input->enabled != 1) return S2A_W2_QUERY_STALE;
    if (!input->original.canonical || input->original.canonical_bytes != bytes ||
        memcmp(input->original.canonical, original, bytes) ||
        !canonical_matches(input->identity, original, bytes)) return S2A_W2_QUERY_IDENTITY;
    rc = facts_valid(input->identity);
    if (rc) return rc;
    if (!source_matches(&source, &query, &stamp, original, bytes)) return S2A_W2_QUERY_NO_SOURCE;

    receipt->stamp = stamp;
    receipt->requested = query;
    receipt->resolved = source.resolved;
    receipt->source_id = source.source_id;
    receipt->descriptor = source.descriptor;
    receipt->view_present = source.view_present;
    receipt->view = source.view;
    receipt->original_identity_bytes = bytes;
    memcpy(receipt->original_identity, original, bytes);
    receipt->observed = 1;
    return S2A_W2_QUERY_OBSERVED;
}
