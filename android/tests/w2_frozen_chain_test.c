// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define _POSIX_C_SOURCE 200809L
/* Original whole-chain host fixture. The source store, facts/capsule, identity,
 * request, worker, strict parser, shared gate and W2 resolver are all real.
 * Only registered GPU metadata and the managed compiler are synthetic. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "direct/compile_export.h"
#include "direct/compile_export_w2.h"
#include "direct/compile_request.h"
#include "direct/compile_worker.h"
#include "direct/s2a_direct.h"
#include "direct/source_store.h"
#include "direct/sha256.h"
#include "nvn/nvn_vk_compile_facts.h"

static int failures, compiler_calls;
#define CHECK(x, label) do { if (!(x)) { fprintf(stderr, "ASSERT whole-chain:%d: %s\n", __LINE__, label); failures++; } } while (0)
static int host_gate = 1;
int s2a_direct_diag_debug_open(void) { return host_gate; }
int s2a_nvn_window_origin_mode = 1, s2a_nvn_depth_mode = 1;
static uint64_t registry_generation = 17;
static struct nvk_w2_raw_registration registration = {1, 0, 1, 0x25, 0xA, 1, 0, 0, 0};
uint64_t s2a_vk_adapter_registry_generation(void) { return registry_generation; }
int s2a_vk_adapter_w2_capture_registered(const int *ids, size_t count,
    struct nvk_w2_raw_registration *out, uint64_t *generation) {
    for (size_t i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof out[i]);
        if (ids[i] == 4) out[i] = registration;
    }
    *generation = registry_generation;
    return 0;
}
static int registry_lookup(int id, struct nvk_tex_desc *out, void *unused) {
    (void)unused;
    if (id != 4) return -1;
    out->nvn_format = registration.format;
    out->nvn_target = registration.target;
    out->layers = registration.layers;
    return 0;
}
static const uint8_t *expected_json;
static size_t expected_json_len;
static int32_t managed_compile(uint64_t session, uint64_t generation, s2a_direct_bytes request,
                               s2a_direct_emit_fn emit, void *sink) {
    CHECK(session == 31 && generation == 7, "original transport reaches managed callback");
    CHECK(request.bytes == expected_json_len && !memcmp(request.data, expected_json, expected_json_len),
          "managed callback receives exact original request bytes");
    compiler_calls++;
    static const uint8_t error[] = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureCoordNormalized has no source (stage=fs,handle=8,cbufSlot=-1)\"}";
    s2a_direct_bytes result = {error, sizeof error - 1};
    CHECK(emit(sink, result) == 0, "real native emit copies managed error");
    return S2A_DIRECT_RES_ARGUMENT;
}

#include "whole_chain_adapter.inc"

static void source_key(const uint8_t *code, size_t code_bytes, const uint8_t *control,
                        size_t control_bytes, uint8_t key[8]) {
    s2a_sha256 hash; uint8_t digest[32];
    s2a_sha256_init(&hash); s2a_sha256_update(&hash, control, control_bytes);
    s2a_sha256_update(&hash, code, code_bytes); s2a_sha256_final(&hash, digest);
    memcpy(key, digest, 8);
}
static int execute(const char *json, size_t bytes, int worker) {
    if (worker) {
        uint8_t digest[32]; s2a_sha256 hash;
        s2a_sha256_init(&hash); s2a_sha256_update(&hash, (const uint8_t *)json, bytes); s2a_sha256_final(&hash, digest);
        uint8_t *result = NULL; size_t result_len = 0; char why[512] = "";
        int rc = s2a_compile_worker_run(31, 7, json, bytes, digest, 1000,
            NULL, NULL, NULL, &result, &result_len, why, sizeof why);
        CHECK(rc == -1 && !result && !result_len, "actual worker wrapper preserves compiler rejection");
        free(result); return rc;
    }
    struct s2a_compile_outcome out;
    int rc = s2a_compile_request_execute(31, 7, json, bytes, &out);
    CHECK(rc == S2A_DIRECT_RES_ARGUMENT && !out.bytes && !out.bytes_len,
          "sync rejection never becomes successful compiler output");
    s2a_compile_outcome_free(&out); return rc;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *mode = argv[1];
    int worker = !strncmp(mode, "worker", 6);
    int stale = strstr(mode, "stale") != NULL, stopped = strstr(mode, "stop") != NULL;
    int wrong = strstr(mode, "wrong") != NULL, tess = strstr(mode, "tess") != NULL;
    int stripped = strstr(mode, "stripped") != NULL;
    int tess_bad = strstr(mode, "tess_bad_") != NULL;
    int existing = strstr(mode, "existing") != NULL;
    int existing_bad = strstr(mode, "existing_bad") != NULL;
    uint8_t code[2][128], control[2][S2A_SOURCE_CONTROL_BYTES], keys[2][8];
    struct s2a_source_entry_view views[2];
    for (unsigned i = 0; i < 2; i++) {
        memset(code[i], (int)(0x31 + i), sizeof code[i]);
        memset(control[i], (int)(0x41 + i), sizeof control[i]);
        memset(control[i] + 0x700, 0, 8);
        uint32_t tess_value = strstr(mode, "tess_zero") ? 0 : (i ? 0x2B6Du : 0x1A5Cu);
        uint32_t wrong_offset_value = 0x7E91u;
        memcpy(control[i] + 0x744, &tess_value, 4);
        memcpy(control[i] + 0x740, &wrong_offset_value, 4);
        if (stripped) { code[i][0]=0x78; code[i][1]=0x56; code[i][2]=0x34; code[i][3]=0x12; }
        source_key(code[i], sizeof code[i], control[i], sizeof control[i], keys[i]);
        CHECK(!s2a_source_keep(keys[i], i, code[i], sizeof code[i], control[i], sizeof control[i]), "real source store captures complete bytes");
        CHECK(s2a_source_acquire(keys[i], &views[i]) == 1, "real source pin acquired");
    }
    nvk_w2_diagnostic_init();
    struct s2a_record_draw_state state = {0};
    state.stages = 3; state.prim = 4; state.tex_pool_base = 4;
    state.tex_pool_n = 8; state.samp_pool_n = 8; memcpy(state.keys, keys, sizeof keys);
    struct s2a_direct_compile_context pc = {0};
    pc.session = 31; pc.generation = 7; pc.direct_abi = 5;
    pc.profile_json = "{\"needsFragmentOutputSpecialization\":true}";
    pc.profile_bytes = strlen(pc.profile_json); pc.translator_closure = "whole-chain-public-synthetic";
    struct nvk_env env; nvk_env_fill(&pc, &env);
    struct s2a_compile_graphics_src gfx = {0}; gfx.topology = "Triangles";
    if (existing) { gfx.has_tess_mode = 1; gfx.tess_mode = 0x3456u; }
    struct nvk_texture_set textures;
    nvk_texture_facts_fill(&state, registry_lookup, NULL, &textures);
    CHECK(textures.count == 0, "real converter omits unsupported BUFFER facts");
    uint8_t canonical[S2A_INPUT_IDENTITY_MAX], identity[32]; size_t canonical_bytes = 0;
    CHECK(!nvk_input_identity(&state, &pc, &env, &gfx, textures.items, textures.count,
        NULL, canonical, &canonical_bytes, identity), "real original identity captured");
    struct s2a_compile_request_input in = {0};
    in.session = pc.session; in.generation = pc.generation; in.request_id = "whole-chain-synthetic";
    in.profile_json = pc.profile_json; in.profile_bytes = pc.profile_bytes;
    in.direct_abi = (int)pc.direct_abi; in.translator_closure = pc.translator_closure;
    in.stage_count = 2; in.gfx = gfx; in.texture_count = textures.count;
    memcpy(in.textures, textures.items, sizeof textures.items);
    in.texture_array_length = state.tex_pool_n; in.sampler_array_length = state.samp_pool_n;
    for (unsigned i = 0; i < 2; i++) {
        in.stages[i].stage = i ? "fs" : "vs";
        in.stages[i].code = views[i].code + (stripped ? 0x30 : 0);
        in.stages[i].code_bytes = views[i].code_bytes - (stripped ? 0x30 : 0);
        in.stages[i].control = views[i].control; in.stages[i].control_bytes = views[i].control_bytes;
        in.stages[i].c1 = views[i].c1; in.stages[i].c1_bytes = views[i].c1_bytes;
    }
    if (tess) {
        struct s2a_source_entry_view v = views[0];
#include "whole_chain_tess.inc"
        CHECK(!gfx.has_tess_mode && in.gfx.has_tess_mode == 1 && in.gfx.tess_mode == (strstr(mode, "tess_zero") ? 0u : 0x1A5Cu),
              "exact draw fragment derives tess from first original source");
    }
    if (strstr(mode, "tess_bad_source")) memcpy(&in.gfx.tess_mode, views[1].control + 0x744, 4);
    if (strstr(mode, "tess_bad_offset")) memcpy(&in.gfx.tess_mode, views[0].control + 0x740, 4);
    if (strstr(mode, "tess_bad_value")) in.gfx.tess_mode ^= 1;
    if (strstr(mode, "tess_bad_field")) in.gfx.rt_count = 1;
    if (strstr(mode, "tess_bad_control")) in.stages[0].control = views[1].control;
    if (existing_bad) in.gfx.tess_mode ^= 1;
    if (strstr(mode, "existing_bad_source")) memcpy(&in.gfx.tess_mode, views[0].control + 0x744, 4);
    if (strstr(mode, "existing_bad_clear")) { in.gfx.has_tess_mode = 0; in.gfx.tess_mode = gfx.tess_mode; }
    char *json = NULL; size_t json_bytes = 0;
    CHECK(!s2a_compile_request_build(&in, &json, &json_bytes, NULL, 0) && json, "actual request builder succeeds");
    if (!json) return 1;
    expected_json = (const uint8_t *)json; expected_json_len = json_bytes;
    CHECK(!s2a_direct_bind_compiler_v1(&compiler_calls, managed_compile), "real compiler binding installed");
    if (worker) CHECK(!s2a_compile_worker_start(adapter_worker_compile, NULL, NULL, 0), "real worker started");
    registration.format = 1; registration.target = 2; registration.layers = 9; registry_generation++;
    (void)execute(json, json_bytes, worker);
    if (stale) {
        char *copy = malloc(json_bytes); CHECK(copy != NULL, "unattributed new request copy allocated");
        if (copy) { memcpy(copy, json, json_bytes); (void)execute(copy, json_bytes, worker); free(copy); }
    }
    CHECK(compiler_calls == (stale ? 2 : 1), "exact expected compiler call count");
    if (stopped) nvk_w2_diagnostic_cancel();
    struct s2a_compile_export_w2_key key = {.session=31, .generation=7};
    memcpy(key.input_sha, identity, sizeof identity);
    if (wrong) key.input_sha[31] ^= 1;
    struct s2a_w2_query_receipt receipt;
    int status = s2a_compile_export_w2_diagnose(&key, &receipt);
    int reject = stale || stopped || wrong || tess_bad || existing_bad;
    CHECK(status == (reject ? S2A_W2_QUERY_NO_SOURCE : S2A_W2_QUERY_OBSERVED), "whole real diagnostic chain returns expected status");
    CHECK(receipt.observed == !reject, "receipt truth agrees with result");
    if (!reject) {
        CHECK(receipt.descriptor.format == 0x25 && receipt.descriptor.target == 0xA && receipt.descriptor.layers == 1,
              "resolver reads original frozen registration, not changed registry");
        CHECK(receipt.original_identity_bytes == canonical_bytes && !memcmp(receipt.original_identity, canonical, canonical_bytes),
              "receipt carries original full canonical bytes");
        CHECK(receipt.requested.method == S2A_W2_QUERY_NORMALIZED && receipt.requested.coordinate.stage == 1 &&
              receipt.requested.coordinate.handle == 8 && receipt.requested.coordinate.cbuf_slot == -1,
              "actual error propagated unchanged into actual W2 query");
    }
    if (wrong) key.input_sha[31] ^= 1;
    CHECK(s2a_compile_export_w2_diagnose(&key, &receipt) == S2A_W2_QUERY_NO_SOURCE && !receipt.observed,
          "returned receipt is single use including wrong-key rejection");
    if (worker) CHECK(!s2a_compile_worker_stop_bounded(1000, NULL, 0), "real worker drains");
    CHECK(!s2a_direct_unbind_compiler_v1(&compiler_calls), "real compiler unbound");
    free(json);
    for (unsigned i = 0; i < 2; i++) s2a_source_release(views[i].ref);
    nvk_w2_diagnostic_cancel(); s2a_source_clear();
    printf("whole_chain %s: %s status=%d compiler_calls=%d\n", mode, failures ? "FAIL" : "PASS", status, compiler_calls);
    return failures ? 1 : 0;
}
