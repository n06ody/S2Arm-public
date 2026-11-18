// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "direct/w2_single_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s line %d: %s\n", __func__, __LINE__, #x); return 1; } } while (0)

struct fixture {
    struct s2a_compile_graphics_src gfx;
    struct s2a_compile_texture_src facts[64];
    struct s2a_input_identity identity;
    uint8_t canonical[S2A_INPUT_IDENTITY_MAX];
    struct s2a_w2_query_input input;
    struct s2a_w2_query_source source;
    int calls, resolver_rc, mutate;
};

static void init(struct fixture *f) {
    memset(f, 0, sizeof *f);
    f->gfx.topology = "Triangles";
    f->identity.direct_abi = 5;
    f->identity.translator_closure = "synthetic-closure";
    memset(f->identity.profile_sha, 0x21, 32);
    f->identity.stage_mask = 3;
    f->identity.source_present = 3;
    for (int i = 0; i < 2; i++) {
        memset(f->identity.source_sha[i], 0x30 + i, 32);
        memcpy(f->identity.keys[i], f->identity.source_sha[i], 8);
        f->identity.source_code_bytes[i] = 64;
        f->identity.source_control_bytes[i] = 128;
    }
    f->identity.texture_array_length = 128;
    f->identity.sampler_array_length = 64;
    f->identity.gfx = &f->gfx;
    f->identity.textures = f->facts;
    f->identity.texture_count = 2;
    f->facts[0] = (struct s2a_compile_texture_src){"vs", 8, -1, "2d", "R8G8B8A8Unorm", 0, 1, 0, 1};
    f->facts[1] = (struct s2a_compile_texture_src){"fs", 10, -1, "cube", "R8G8B8A8Unorm", 0, 1, 0, 1};
    f->input.enabled = 1;
    f->input.identity = &f->identity;
    f->input.original.stamp = (struct s2a_w2_query_stamp){11, 12, 13, 14, 15, 16};
    f->input.current = f->input.original.stamp;
    f->input.original.canonical = f->canonical;
    f->input.query = (struct s2a_w2_query){S2A_W2_QUERY_FORMAT, {S2A_W2_QUERY_FS, 12, 2}};
    f->source.requested = f->input.query;
    f->source.resolved = f->input.query.coordinate;
    f->source.source_id = 27;
    f->source.descriptor = (struct s2a_w2_query_raw_texture){S2A_W2_RAW_COMPLETE, 0x25, 1, 1};
}

static int seal(struct fixture *f) {
    size_t n = 0;
    CHECK(!s2a_input_identity_encode(&f->identity, f->canonical, sizeof f->canonical, &n));
    f->input.original.canonical_bytes = n;
    f->source.binding = f->input.original;
    return 0;
}

static int resolve(void *opaque, const struct s2a_w2_query *q, struct s2a_w2_query_source *out) {
    struct fixture *f = opaque;
    f->calls++;
    if (q->method != f->input.query.method ||
        q->coordinate.stage != f->input.query.coordinate.stage ||
        q->coordinate.handle != f->input.query.coordinate.handle ||
        q->coordinate.cbuf_slot != f->input.query.coordinate.cbuf_slot) return -1;
    *out = f->source;
    if (f->mutate == 1) f->facts[0].normalized ^= 1;
    if (f->mutate == 2) f->input.current.generation++;
    if (f->mutate == 3) f->canonical[f->input.original.canonical_bytes - 1] ^= 1;
    if (f->mutate == 4) f->facts[0].normalized = 2;
    if (f->mutate == 5) {
        f->facts[0].normalized ^= 1;
        if (seal(f)) return -1;
        *out = f->source;
    }
    if (f->mutate == 6) f->input.query.coordinate.handle++;
    if (f->mutate == 7) f->input.original.stamp.source_epoch++;
    if (f->mutate == 8) f->input.enabled = 0;
    if (f->mutate == 9) f->facts[0].is_buffer = 2;
    if (f->mutate == 10) f->facts[0].separate = 2;
    return f->resolver_rc;
}

static int run(struct fixture *f, int expected, int calls) {
    struct s2a_w2_query_receipt receipt, zero = {0};
    memset(&receipt, 0xa5, sizeof receipt);
    int rc = s2a_w2_single_query_diagnose(&f->input, resolve, f, &receipt);
    CHECK(rc == expected);
    CHECK(f->calls == calls);
    if (rc == S2A_W2_QUERY_OBSERVED) {
        CHECK(receipt.observed == 1 && receipt.source_id == 27);
        CHECK(receipt.stamp.session == f->input.original.stamp.session);
        CHECK(receipt.stamp.generation == f->input.original.stamp.generation);
        CHECK(receipt.stamp.snapshot_id == f->input.original.stamp.snapshot_id);
        CHECK(receipt.stamp.snapshot_version == f->input.original.stamp.snapshot_version);
        CHECK(receipt.stamp.source_epoch == f->input.original.stamp.source_epoch);
        CHECK(receipt.stamp.registry_generation == f->input.original.stamp.registry_generation);
        CHECK(receipt.requested.method == f->input.query.method);
        CHECK(receipt.requested.coordinate.stage == f->input.query.coordinate.stage);
        CHECK(receipt.requested.coordinate.handle == f->input.query.coordinate.handle);
        CHECK(receipt.requested.coordinate.cbuf_slot == f->input.query.coordinate.cbuf_slot);
        CHECK(receipt.resolved.stage == f->source.resolved.stage);
        CHECK(receipt.resolved.handle == f->source.resolved.handle);
        CHECK(receipt.resolved.cbuf_slot == f->source.resolved.cbuf_slot);
        CHECK(receipt.original_identity_bytes == f->input.original.canonical_bytes);
        CHECK(!memcmp(receipt.original_identity, f->canonical, receipt.original_identity_bytes));
        CHECK(!memcmp(&receipt.descriptor, &f->source.descriptor, sizeof receipt.descriptor));
        CHECK(receipt.view_present == f->source.view_present);
        CHECK(!memcmp(&receipt.view, &f->source.view, sizeof receipt.view));
    } else CHECK(!memcmp(&receipt, &zero, sizeof receipt));
    return 0;
}

static int test_positive(void) {
    struct fixture f; init(&f); CHECK(!seal(&f));
    struct s2a_compile_texture_src old[64]; memcpy(old, f.facts, sizeof old);
    uint8_t bytes[S2A_INPUT_IDENTITY_MAX]; memcpy(bytes, f.canonical, sizeof bytes);
    CHECK(!run(&f, S2A_W2_QUERY_OBSERVED, 1));
    CHECK(!memcmp(old, f.facts, sizeof old));
    CHECK(!memcmp(bytes, f.canonical, sizeof bytes));
    CHECK(f.identity.texture_count == 2);
    return 0;
}

static int test_empty_compute(void) {
    struct fixture f; init(&f);
    f.identity.texture_count = 0; f.identity.textures = NULL;
    f.identity.stage_mask = 1u << S2A_W2_QUERY_CS; f.identity.source_present = 1;
    f.identity.compute_has_unaligned_ssbo = 1;
    f.input.query.coordinate.stage = S2A_W2_QUERY_CS;
    f.source.requested = f.input.query; f.source.resolved = f.input.query.coordinate;
    f.source.descriptor.format = 0; f.source.descriptor.target = 0;
    CHECK(!seal(&f)); CHECK(!run(&f, S2A_W2_QUERY_OBSERVED, 1));
    CHECK(f.identity.texture_count == 0 && f.identity.textures == NULL);
    return 0;
}

static int test_capacity(void) {
    struct fixture f; init(&f);
    for (int i = 2; i < 64; i++) { f.facts[i] = f.facts[0]; f.facts[i].handle = (uint32_t)(100 + i); }
    f.identity.texture_count = 64; CHECK(!seal(&f));
    struct s2a_compile_texture_src old[64]; memcpy(old, f.facts, sizeof old);
    CHECK(!run(&f, S2A_W2_QUERY_OBSERVED, 1));
    CHECK(f.identity.texture_count == 64 && !memcmp(old, f.facts, sizeof old));
    return 0;
}

static int test_identity(void) {
    for (int mode = 0; mode < 18; mode++) {
        struct fixture f; init(&f); CHECK(!seal(&f));
        switch (mode) {
        case 0: f.identity.source_sha[0][31] ^= 1; break;
        case 1: f.identity.source_code_bytes[0]++; f.identity.source_control_bytes[0]--; break;
        case 2: f.identity.profile_sha[31] ^= 1; break;
        case 3: f.identity.translator_closure = "another-closure"; break;
        case 4: f.identity.direct_abi++; break;
        case 5: f.facts[0].stage = "fs"; break;
        case 6: f.facts[0].handle++; break;
        case 7: f.facts[0].cbuf_slot = 3; break;
        case 8: f.facts[0].format = "R32Float"; break;
        case 9: f.facts[0].normalized ^= 1; break;
        case 10: f.facts[0].array_length++; break;
        case 11: f.identity.texture_count = 0; break;
        case 12: f.identity.ubo_use[1]++; break;
        case 13: f.canonical[f.input.original.canonical_bytes - 1] ^= 1; break;
        case 14: f.input.original.canonical_bytes--; break;
        case 15: f.identity.texture_array_length++; break;
        case 16: f.gfx.topology = "Lines"; break;
        case 17: f.facts[0].sampler_type = "cube"; break;
        }
        CHECK(!run(&f, S2A_W2_QUERY_IDENTITY, 0));
    }
    return 0;
}

static int test_stale(void) {
    for (int mode = 0; mode < 6; mode++) {
        struct fixture f; init(&f); CHECK(!seal(&f));
        switch (mode) {
        case 0: f.input.current.session++; break;
        case 1: f.input.current.generation++; break;
        case 2: f.input.current.snapshot_id++; break;
        case 3: f.input.current.snapshot_version++; break;
        case 4: f.input.current.source_epoch++; break;
        case 5: f.input.current.registry_generation++; break;
        }
        CHECK(!run(&f, S2A_W2_QUERY_STALE, 0));
    }
    return 0;
}

static int test_missing_semantics(void) {
    for (int mode = 0; mode < 8; mode++) {
        struct fixture f; init(&f);
        f.facts[1].handle = 12;
        if (mode == 0) f.facts[1].cbuf_slot = 2; /* exact already answers */
        if (mode == 2) f.facts[1].stage = "vs"; /* never borrow across stage */
        if (mode >= 3) f.facts[1].format = "Unknown";
        if (mode == 4) f.input.query.method = S2A_W2_QUERY_NORMALIZED;
        if (mode == 5) f.input.query.method = S2A_W2_QUERY_SAMPLER_TYPE;
        if (mode == 6) f.input.query.method = S2A_W2_QUERY_IS_BUFFER;
        if (mode == 7) f.input.query.method = S2A_W2_QUERY_SAMPLED_FORMAT;
        f.source.requested = f.input.query; CHECK(!seal(&f));
        int missing = mode == 2 || mode == 3;
        CHECK(!run(&f, missing ? S2A_W2_QUERY_OBSERVED : S2A_W2_QUERY_NOT_MISSING, missing));
    }
    /* Exact Unknown takes precedence over a concrete direct fallback. */
    struct fixture f; init(&f); f.facts[0] = f.facts[1];
    f.facts[0].handle = 12; f.facts[0].cbuf_slot = 2; f.facts[0].format = "Unknown";
    f.facts[1].handle = 12; CHECK(!seal(&f));
    CHECK(!run(&f, S2A_W2_QUERY_OBSERVED, 1));
    // sampled-format 的 c4 不是 c2 表：同偏移的直接槽不能顶替。
    init(&f); f.facts[1].handle = 12; f.input.query.method = S2A_W2_QUERY_SAMPLED_FORMAT;
    f.input.query.coordinate.cbuf_slot = 4; f.source.requested = f.input.query; f.source.resolved = f.input.query.coordinate;
    CHECK(!seal(&f)); CHECK(!run(&f, S2A_W2_QUERY_OBSERVED, 1));
    f.calls = 0; f.source.resolved.cbuf_slot = -1;
    CHECK(!run(&f, S2A_W2_QUERY_NO_SOURCE, 1));
    return 0;
}

static int test_source_contract(void) {
    for (int mode = 0; mode < 20; mode++) {
        struct fixture f; init(&f); CHECK(!seal(&f));
        uint8_t wrong[S2A_INPUT_IDENTITY_MAX]; memcpy(wrong, f.canonical, sizeof wrong);
        switch (mode) {
        case 0: f.resolver_rc = -1; break;
        case 1: f.source.binding.stamp.session++; break;
        case 2: f.source.binding.stamp.generation++; break;
        case 3: f.source.binding.stamp.snapshot_id++; break;
        case 4: f.source.binding.stamp.snapshot_version++; break;
        case 5: f.source.binding.stamp.source_epoch++; break;
        case 6: f.source.binding.stamp.registry_generation++; break;
        case 7: wrong[f.input.original.canonical_bytes - 1] ^= 1; f.source.binding.canonical = wrong; break;
        case 8: f.source.requested.method = S2A_W2_QUERY_IS_BUFFER; break;
        case 9: f.source.requested.coordinate.handle++; break;
        case 10: f.source.resolved.stage = S2A_W2_QUERY_VS; break;
        case 11: f.source.resolved.handle++; break;
        case 12: f.source.resolved.cbuf_slot = 1; break;
        case 13: f.source.source_id = 0; break;
        case 14: f.source.descriptor.present &= ~S2A_W2_RAW_LAYERS; break;
        case 15: f.source.descriptor.layers = 0; break;
        case 16: f.source.requested.coordinate.stage = S2A_W2_QUERY_VS; break;
        case 17: f.source.requested.coordinate.cbuf_slot++; break;
        case 18: f.source.binding.canonical_bytes--; break;
        case 19: f.source.binding.canonical = NULL; break;
        }
        CHECK(!run(&f, S2A_W2_QUERY_NO_SOURCE, 1));
    }
    struct fixture f; init(&f); CHECK(!seal(&f));
    f.source.resolved.cbuf_slot = -1; /* documented same-stage/handle fallback */
    CHECK(!run(&f, S2A_W2_QUERY_OBSERVED, 1));
    init(&f); f.input.query.coordinate.cbuf_slot = -1;
    f.source.requested = f.input.query; f.source.resolved = f.input.query.coordinate;
    CHECK(!seal(&f)); CHECK(!run(&f, S2A_W2_QUERY_OBSERVED, 1));
    f.calls = 0; f.source.resolved.cbuf_slot = 2;
    CHECK(!run(&f, S2A_W2_QUERY_NO_SOURCE, 1));
    return 0;
}

static int test_view(void) {
    for (int mode = 0; mode < 5; mode++) {
        struct fixture f; init(&f); CHECK(!seal(&f));
        f.source.view_present = 1;
        f.source.view = (struct s2a_w2_query_raw_texture){S2A_W2_RAW_COMPLETE, 0x25, 1, 1};
        if (mode == 1) f.source.view.present &= ~S2A_W2_RAW_FORMAT;
        if (mode == 2) f.source.view.layers = 0;
        if (mode == 3) f.source.view_present = 2;
        if (mode == 4) f.source.view_present = 0; /* absent view cannot carry stale values */
        CHECK(!run(&f, mode ? S2A_W2_QUERY_NO_SOURCE : S2A_W2_QUERY_OBSERVED, 1));
    }
    return 0;
}

static int test_invalid(void) {
    for (int mode = 0; mode < 8; mode++) {
        struct fixture f; init(&f); CHECK(!seal(&f));
        switch (mode) {
        case 0: f.input.query.method = 99; break;
        case 1: f.input.query.coordinate.stage = 2; break;
        case 2: f.input.query.coordinate.cbuf_slot = -2; break;
        case 3: f.input.query.coordinate.cbuf_slot = 32; break;
        case 4: f.input.query.coordinate.handle = UINT32_MAX; break;
        case 5: f.input.original.canonical = NULL; break;
        case 6: f.input.original.canonical_bytes = 0; break;
        case 7: f.input.identity = NULL; break;
        }
        CHECK(!run(&f, S2A_W2_QUERY_INVALID, 0));
    }
    struct fixture f; init(&f); f.facts[1] = f.facts[0]; CHECK(!seal(&f));
    CHECK(!run(&f, S2A_W2_QUERY_AMBIGUOUS, 0));
    return 0;
}

static int test_nulls(void) {
    struct fixture f; init(&f); CHECK(!seal(&f));
    struct s2a_w2_query_receipt receipt, zero = {0};
    memset(&receipt, 0xa5, sizeof receipt);
    CHECK(s2a_w2_single_query_diagnose(NULL, resolve, &f, &receipt) == S2A_W2_QUERY_INVALID);
    CHECK(!memcmp(&receipt, &zero, sizeof receipt));
    CHECK(s2a_w2_single_query_diagnose(&f.input, resolve, &f, NULL) == S2A_W2_QUERY_INVALID);
    memset(&receipt, 0xa5, sizeof receipt);
    CHECK(s2a_w2_single_query_diagnose(&f.input, NULL, &f, &receipt) == S2A_W2_QUERY_INVALID);
    CHECK(!memcmp(&receipt, &zero, sizeof receipt) && f.calls == 0);
    return 0;
}

static int test_postcheck(void) {
    for (int mode = 1; mode <= 10; mode++) {
        struct fixture f; init(&f);
        if (mode == 9) f.facts[0].is_buffer = 1;
        if (mode == 10) f.facts[0].separate = 1;
        CHECK(!seal(&f)); f.mutate = mode;
        int expected = mode == 2 || (mode >= 6 && mode <= 8) ?
            S2A_W2_QUERY_STALE : S2A_W2_QUERY_IDENTITY;
        if (mode == 4 || mode == 9 || mode == 10) expected = S2A_W2_QUERY_INVALID;
        CHECK(!run(&f, expected, 1));
    }
    return 0;
}

static int test_disabled(void) {
    struct fixture f; init(&f); f.input.enabled = 0;
    CHECK(!run(&f, S2A_W2_QUERY_DISABLED, 0));
    f.input.enabled = 2; CHECK(!run(&f, S2A_W2_QUERY_DISABLED, 0));
    return 0;
}

int main(int argc, char **argv) {
    int (*tests[])(void) = {test_positive, test_capacity, test_identity, test_stale,
        test_missing_semantics, test_source_contract, test_view, test_invalid,
        test_postcheck, test_disabled, test_empty_compute, test_nulls};
    const char *names[] = {"positive", "capacity", "identity", "stale", "missing",
        "source", "view", "invalid", "postcheck", "disabled", "empty-compute", "nulls"};
    int n = (int)(sizeof tests / sizeof tests[0]);
    int ran = 0;
    if (argc > 2) return 2;
    for (int i = 0; i < n; i++) {
        if (argc == 2 && strcmp(argv[1], names[i])) continue;
        ran++;
        if (tests[i]()) return 1;
        printf("PASS %s\n", names[i]);
    }
    return ran ? 0 : 2;
}
