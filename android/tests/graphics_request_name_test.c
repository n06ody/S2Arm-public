// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Original synthetic sources. Execute the complete production graphics compile
// path with the real REQUEST builder, canonical identity and diagnostic writer.
// The external compiler callback is controlled; no shader compilation/GPU claim.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "direct/s2a_direct.h"
#ifndef GRAPHICS_DRAW_SOURCE
#define GRAPHICS_DRAW_SOURCE "nvn/nvn_vk_draw.c"
#endif
#include GRAPHICS_DRAW_SOURCE

static const char *output;
static int compiler_failure, compiler_calls, unwired_reports;
static int provider_calls;
static uint8_t expected_identity[32];
static int graphics_provider(void *ctx, const struct s2a_compile_export_w2_key *key,
    const struct s2a_w2_query_input **input, s2a_w2_query_resolver *resolver, void **snapshot) {
    (void)ctx; (void)input; (void)resolver; (void)snapshot;
    assert(key->session == 11 && key->generation == 22);
    assert(!memcmp(key->input_sha, expected_identity, sizeof expected_identity));
    provider_calls++;
    return -1; /* Fixture intentionally has no frozen snapshot. */
}
int s2a_direct_diag_debug_open(void) { const char *v = getenv("GRAPHICS_EXPORT_GATE"); return !v || strcmp(v, "0"); }
static const char result_bytes[] = "synthetic compiler result";
int __system_property_get(const char *name, char *value) {
    const char *dir = getenv("GRAPHICS_DUMP_FAIL_DIR");
    if (strcmp(name, "debug.s2a.dump_fail")) dir = NULL;
    if (!dir) { value[0] = 0; return 0; }
    assert(strlen(dir) < PROP_VALUE_MAX); strcpy(value, dir); return (int)strlen(dir);
}
int __android_log_print(int level, const char *tag, const char *fmt, ...) {
    (void)level; (void)tag; if (strstr(fmt, "W2 单坐标诊断未接线")) unwired_reports++; return 0;
}
int __android_log_vprint(int level, const char *tag, const char *fmt, va_list ap) {
    (void)level; (void)tag; (void)fmt; (void)ap; return 0;
}
void s2a_source_log_line(const char *line) { (void)line; }
int s2a_nvn_depth_mode = 1, s2a_nvn_window_origin_mode = 1;
int s2a_compile_worker_active(void) { return 0; }
int s2a_compile_worker_run(uint64_t session, uint64_t generation, const char *json, size_t len,
    const uint8_t sha[32], int deadline, int (*pause)(void *), void (*resume)(void *), void *ctx,
    uint8_t **bytes, size_t *bytes_len, char *why, size_t cap) {
    (void)session; (void)generation; (void)json; (void)len; (void)sha; (void)deadline;
    (void)pause; (void)resume; (void)ctx; (void)bytes; (void)bytes_len; (void)why; (void)cap;
    assert(!"unexpected worker call"); return -1;
}
static void save(const char *name, const void *bytes, size_t len) {
    char path[1200]; int n = snprintf(path, sizeof path, "%s/%s", output, name);
    assert(n > 0 && (size_t)n < sizeof path);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(bytes, 1, len, f) == len); assert(!fclose(f));
}
static int32_t compile_boundary(uint64_t session, uint64_t generation,
    s2a_direct_bytes request, s2a_direct_emit_fn emit, void *sink) {
    compiler_calls++;
    assert(session == 11 && generation == 22);
    save("compiler-request.json", request.data, (size_t)request.bytes);
    if (compiler_failure) return -27;
    s2a_direct_bytes result = {(const uint8_t *)result_bytes, sizeof result_bytes - 1};
    return emit(sink, result);
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return compile_boundary; }
static void source(struct s2a_record_draw_state *state, unsigned slot) {
    uint8_t code[512], control[S2A_SOURCE_CONTROL_BYTES];
    for (unsigned i = 0; i < sizeof code; i++) code[i] = (uint8_t)(3 * i + slot + 1);
    memset(control, 0, sizeof control);
    uint32_t begin = 256, end = 512;
    memcpy(control + 0x700, &begin, 4); memcpy(control + 0x704, &end, 4);
    s2a_sha256 sha; uint8_t digest[32]; s2a_sha256_init(&sha);
    s2a_sha256_update(&sha, control, sizeof control); s2a_sha256_update(&sha, code, sizeof code);
    s2a_sha256_final(&sha, digest); memcpy(state->keys[slot], digest, 8);
    assert(!s2a_source_keep(state->keys[slot], slot, code, sizeof code, control, sizeof control));
}
int main(int argc, char **argv) {
    assert(argc == 5); output = argv[1]; int mode = atoi(argv[2]);
    compiler_failure = atoi(argv[3]); int variant = atoi(argv[4]);
    char diag[1200]; int n = snprintf(diag, sizeof diag, "%s/capture", output);
    assert(n > 0 && (size_t)n < sizeof diag);
    if (mode == 0) unsetenv("S2A_DIRECT_DIAG_DIR");
    else {
        if (mode == 2) save("capture", "not a directory", 15);
        assert(!setenv("S2A_DIRECT_DIAG_DIR", diag, 1));
    }
    static const char profile[] =
        "{\"schema\":\"s2a-direct-profile-v1\",\"directABI\":1,"
        "\"translatorClosure\":\"synthetic-closure-v1\","
        "\"runtimeCompilerABI\":\"s2a-direct-compile-v1\","
        "\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"supportLayout\":{\"hash\":\"sha256-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}}";
    struct s2a_direct_compile_context context = {0};
    context.session = 11; context.generation = 22; context.direct_abi = 1;
    context.translator_closure = "synthetic-closure-v1";
    context.profile_hash = "sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    context.profile_json = profile; context.profile_bytes = strlen(profile);
    context.shader_draw_parameters = 1; context.storage_buffer_offset_alignment = 16;
    struct s2a_record_draw_state state = {0};
    state.stages = 3; state.prim = 4; state.rt_count = 1;
    state.rt[0].key = 0x1000; state.rt[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    state.rt[0].nvn_format = 0x25; state.rt[0].obj_magic = 0x00584554u;
    state.tex_pool_n = 8 + variant; state.samp_pool_n = 8; state.state_seen = 0x3f;
    state.ubo_use[0] = state.ubo_use[1] = 1;
    source(&state, 0); source(&state, 1);
    struct s2a_source_entry_view views[2] = {0};
    for (unsigned i = 0; i < 2; i++) assert(s2a_source_acquire(state.keys[i], &views[i]));
    struct s2a_vk_device device = {0}; device.core_features.vertexPipelineStoresAndAtomics = 1;
    struct nvk_cached_program entry = {0};
    struct nvk_env env; nvk_env_fill(&context, &env);
    struct s2a_compile_graphics_src gfx = {0}; int complete = 0;
    nvk_compile_facts_fill(&gfx, &state, &context, &complete);
    uint8_t canonical[8192]; size_t canonical_len = sizeof canonical;
    assert(!nvk_input_identity(&state, &context, &env, &gfx, NULL, 0, NULL,
                              canonical, &canonical_len, entry.input_sha));
    save("canonical.bin", canonical, canonical_len);
    memcpy(expected_identity, entry.input_sha, sizeof expected_identity);
    if (getenv("GRAPHICS_EXPECT_PROVIDER")) s2a_compile_export_w2_set_provider(graphics_provider, NULL);
    // 首遇后台提交给 nvk_program_compile_inner 加了 int *skipped 出参（>0 = 本笔安全跳过、
    // 结果不入缓存）。本夹具走同步编译、必须编译到位：断言不被跳过。
    int skipped = 0;
    nvk_program_compile_inner(&device, &entry, &state, "unused", &context, NULL, views, &skipped);
    assert(skipped == 0);
    assert(compiler_calls == 1 && entry.failed == compiler_failure);
    if (getenv("GRAPHICS_EXPECT_UNWIRED")) assert(unwired_reports == 1);
    if (getenv("GRAPHICS_EXPECT_PROVIDER")) assert(provider_calls == 1 && unwired_reports == 0);
    if (!compiler_failure) {
        assert(entry.len == sizeof result_bytes - 1);
        assert(!memcmp(entry.bytes, result_bytes, entry.len));
        assert(entry.session == 11 && entry.generation == 22);
    }
    // Byte oracle excludes addresses and clock counters, includes stable result,
    // failure reason, request/result digests and source capture state.
    if (entry.len) save("program.bin", entry.bytes, entry.len);
    free(entry.bytes); entry.bytes = NULL;
    save("entry.bin", &entry, sizeof entry);
    for (int i = 0; i < 32; i++) printf("%02x", entry.input_sha[i]);
    printf("\n");
    for (unsigned i = 0; i < 2; i++) s2a_source_release(views[i].ref);
    s2a_source_clear();
    return 0;
}
