// 新回放着色器入口 + 真实图形/计算请求构造与身份；编译器边界只捕获 JSON。
// 合成来源/设备，不声称运行真实游戏或着色器。
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn.h"
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "direct/compile_request.h"
#include "direct/input_identity.h"
#include "direct/source_store.h"
#include "direct/sha256.h"
#include "direct/s2a_direct.h"
#include "../../runtime/include/s2a_direct_host.h"

static int checks, failures, compiler_calls, diag_enabled, diag_lines, diag_queries;
static unsigned fake_array_count;
static int separate_fixture;
int g_s2a_logfd = 2;
static const char *fake_compile_error;
static char captured[262144], last_red[512], last_diag[512], last_stats[512], last_reason[512], last_request_reason[512];
static char reason_history[8192];
static const char profile[] = "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
static const char phash[] = "sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_texpool_log")) { diag_queries++; return diag_enabled; }
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    if (level == NVN_PLAT_LOG_RED && !strcmp(cls, "shader")) snprintf(last_red, sizeof last_red, "%s", text);
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "texture_pool_capacity=")) {
        diag_lines++; snprintf(last_diag, sizeof last_diag, "%s", text);
    }
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "texture_pool_program_stats"))
        snprintf(last_stats, sizeof last_stats, "%s", text);
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "texture_pool_compile_reject")) {
        snprintf(last_reason, sizeof last_reason, "%s", text);
        size_t n = strlen(reason_history);
        snprintf(reason_history + n, sizeof reason_history - n, "%s\n", text);
    }
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "texture_pool_request_reject"))
        snprintf(last_request_reason, sizeof last_request_reason, "%s", text);
}
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
// 此夹具只登记一个起点为 0 的池；空池也按真实 nvn_dpool_base 返回 0。
int nvn_dpool_base(const void *pool, int tex) { (void)pool; (void)tex; return 0; }
uint64_t nvn_ga2ca(uint64_t address) { return address; }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) {
    if (!separate_fixture || (id != 77 && id != 78 && !(separate_fixture == 2 && id == 0))) return 0;
    memset(t, 0, sizeof *t); memset(v, 0, sizeof *v); t->d.format = NVN_FORMAT_RGBA8; t->d.target = NVN_TEXTURE_TARGET_2D; t->d.depth = 1;
    *h = id == 77; if (*h) { v->set_target = 1; v->target = NVN_TEXTURE_TARGET_RECTANGLE; } return 1;
}
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int count) {
    uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        int id = h[i] ? (int)(h[i] & 0xfffff) : b; tex_t t; view_t v; int has = 0;
        if (rp_tex_snap_by_id(id, &t, &v, &has)) {
            hash = (hash ^ 1) * 1099511628211ull;
            hash = (hash ^ (uint64_t)(has && v.set_format ? v.format : t.d.format)) * 1099511628211ull;
            hash = (hash ^ (uint64_t)(has && v.set_target ? v.target : t.d.target)) * 1099511628211ull;
            hash = (hash ^ (uint64_t)t.d.depth) * 1099511628211ull;
        } else { hash = (hash ^ 2) * 1099511628211ull; hash = (hash ^ (id >= b && (uint64_t)(id - b) < (uint64_t)count)) * 1099511628211ull; }
    }
    return hash;
}
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    memset(c, 0, sizeof *c); c->session = 4; c->generation = 6; c->direct_abi = 1;
    c->profile_json = profile; c->profile_bytes = strlen(profile); c->profile_hash = phash;
    c->translator_closure = "closure-test"; c->storage_buffer_offset_alignment = 16; return 0;
}
static int32_t fake_compile(uint64_t session, uint64_t gen, s2a_direct_bytes req, s2a_direct_emit_fn emit, void *sink) {
    CHECK(session == 4 && gen == 6, "compile session");
    CHECK(req.bytes < sizeof captured, "request fits capture");
    if (req.bytes >= sizeof captured) return -1;
    memcpy(captured, req.data, (size_t)req.bytes); captured[req.bytes] = 0; compiler_calls++;
    if (fake_compile_error) {
        s2a_direct_bytes payload = {(const uint8_t *)fake_compile_error, (uint64_t)strlen(fake_compile_error)};
        (void)emit(sink, payload); return -1;
    }
    if (fake_array_count) {
        char result[1024];
        int n = snprintf(result, sizeof result,
            "{\"schema\":\"s2a-direct-program-v1\",\"content\":{\"stages\":[{\"stage\":\"vs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}],"
            "\"bindings\":[{\"stage\":\"vs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":0,\"count\":%u}]}}", fake_array_count);
        s2a_direct_bytes payload = {(const uint8_t *)result, (uint64_t)n};
        return emit(sink, payload);
    }
    return -27; // deliberately no result: capture input only
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return fake_compile; }
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *o, nvn_gpu_err *e) {
    (void)s; (void)e;
    if (fake_array_count) { *o = (nvn_gpu_shader)(uintptr_t)1; return (nvn_gpu_rc){NVN_GPU_OK}; }
    CHECK(0, "unexpected shader load"); return (nvn_gpu_rc){NVN_GPU_E_UNSUPPORTED};
}
void nvn_gpu_shader_release(nvn_gpu_shader s) { (void)s; }
static void wr(uint8_t *p, unsigned x) { p[0]=x; p[1]=x>>8; p[2]=x>>16; p[3]=x>>24; }
static void source(void) {
    uint8_t ctl[0x760] = {0}, code[16] = {1,2,3,4}, sha[32];
    wr(ctl+0x20,0x760); wr(ctl+0x748,8); wr(ctl+0x74c,2); wr(ctl+0x750,1);
    wr(ctl+0x754,17); wr(ctl+0x758,32); wr(ctl+0x700,4); wr(ctl+0x704,8);
    s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h,ctl,sizeof ctl); s2a_sha256_update(&h,code,sizeof code); s2a_sha256_final(&h,sha);
    CHECK(!s2a_source_keep(sha, 0, code, sizeof code, ctl, sizeof ctl), "source snapshot");
    memcpy(rp_S.skey[0], sha, 8); memcpy(rp_S.skey[5], sha, 8); rp_S.skey_ok = 1 | (1 << 5); rp_S.bser[0] = 1;
}
static void bind_pools(int n) {
    static dpool_t tex, samp;
    tex.n = n; samp.n = 9;
    p_set_pool t = {n ? &tex : NULL}, s = {&samp};
    CHECK(rp_state_op(OP_SET_TEX_POOL, &t, sizeof t), "texture pool bind handled");
    CHECK(rp_state_op(OP_SET_SAMP_POOL, &s, sizeof s), "sampler pool bind handled");
    CHECK(rp_S.tex_pool_n == n && rp_S.samp_pool_n == 9, "bound state retains pool sizes %d/9", n);
}
static void bind_separate(int stage, int first, int count, uint64_t handle) {
    while (count) {
        p_bind_handles b = {0}; b.stage = stage; b.first = 128 + first; b.n = count > 32 ? 32 : count;
        for (int i = 0; i < b.n; i++) b.h[i] = handle;
        CHECK(rp_state_op(OP_BIND_TEX, &b, sizeof b), "separate bind is recorded");
        first += b.n; count -= b.n;
    }
}
static int count_substr(const char *s, const char *needle) {
    int n = 0; size_t len = strlen(needle); while ((s = strstr(s, needle))) { n++; s += len; } return n;
}
static int fact_matches(const char *stage, int handle, const char *sampler, int normalized) {
    char key[96], norm[32], type[96];
    snprintf(key, sizeof key, "\"stage\":\"%s\",\"handle\":%d,\"cbufSlot\":2", stage, handle);
    snprintf(norm, sizeof norm, "\"normalized\":%s", normalized ? "true" : "false");
    snprintf(type, sizeof type, "\"samplerType\":\"%s\"", sampler);
    const char *p = strstr(captured, key), *end = p ? strchr(p, '}') : NULL;
    const char *n = p ? strstr(p, norm) : NULL, *t = p ? strstr(p, type) : NULL;
    return end && n && n < end && t && t < end;
}
static void run_case(int compute, int pool, int want_called, const char *expected_account) {
    const struct rp_prog *prog = NULL; struct rp_shader_req q = {0}; q.prim = 4;
    rp_shader_reset(); nvn_unimpl_reset(); captured[0] = last_red[0] = 0; compiler_calls = 0;
    bind_pools(pool);
    int rc = compute ? rp_shader_resolve_compute(&prog) : rp_shader_resolve(&q, &prog);
    CHECK(rc == -1 && !prog, "%s n=%d returns failure (fake compiler)", compute ? "cs" : "gfx", pool);
    CHECK(compiler_calls == want_called, "%s n=%d compiler calls=%d expected=%d", compute ? "cs" : "gfx", pool, compiler_calls, want_called);
    if (want_called) {
        char needle[80]; snprintf(needle, sizeof needle, "\"textureArrayLength\":%d,", pool);
        CHECK(strstr(captured, needle) != NULL, "%s n=%d request retains exact capacity", compute ? "cs" : "gfx", pool);
        CHECK(strstr(captured, "\"samplerArrayLength\":9") != NULL, "%s n=%d sampler stays 9", compute ? "cs" : "gfx", pool);
    } else {
        CHECK(nvn_unimpl_count("shader", expected_account) == 1, "%s n=%d expected accounting=%s red=%s", compute ? "cs" : "gfx", pool, expected_account, last_red);
        if (pool > 1048576) {
            char num[32]; snprintf(num, sizeof num, "%d", pool);
            CHECK(strstr(last_red, num) && strstr(last_red, "1048576"), "%s n=%d error states actual and limit: %s", compute ? "cs" : "gfx", pool, last_red);
        }
    }
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--legacy-bytes")) {
        // A baseline-vs-change comparison of the actual legacy REQUEST and canonical bytes.
        unsetenv("S2A_SHADER_REVISION_ROOT"); unsetenv("S2A_DIRECT_CACHE_DIR");
        rp_state_reset(); source(); bind_pools(7); struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
        rp_shader_resolve(&q, &p); if (compiler_calls != 1 || failures) return 1;
        fputs(captured, stdout); putchar('\n');
        struct s2a_compile_graphics_src gfx = {0};
        struct s2a_compile_texture_src tex[] = {{"vs",8,-1,"2d","Unknown",0,1,0,1}};
        struct s2a_input_identity id = {0}; id.gfx = &gfx; id.direct_abi = 1; id.translator_closure = "fixture";
        id.stage_mask = 1; id.source_present = 1; id.source_code_bytes[0] = 16; id.source_control_bytes[0] = 32;
        id.texture_array_length = 7; id.sampler_array_length = 9; id.textures = tex; id.texture_count = 1;
        uint8_t bytes[S2A_INPUT_IDENTITY_MAX]; size_t n = 0;
        if (s2a_input_identity_encode(&id, bytes, sizeof bytes, &n)) return 1;
        for (size_t i = 0; i < n; i++) printf("%02x", bytes[i]);
        putchar('\n'); return 0;
    }
    if (argc != 1) return 2;
    unsetenv("S2A_SHADER_REVISION_ROOT"); unsetenv("S2A_DIRECT_CACHE_DIR"); rp_state_reset(); source();
    for (int cs = 0; cs < 2; cs++) {
        run_case(cs, 0, 0, "texture_pool_missing");
        run_case(cs, -1, 0, "texture_pool_missing");
        run_case(cs, 1, 1, ""); run_case(cs, 7, 1, "");
        run_case(cs, 65535, 1, ""); run_case(cs, 65536, 1, "");
        run_case(cs, 65537, 1, ""); run_case(cs, 524288, 1, "");
        run_case(cs, 1048576, 1, ""); run_case(cs, 1048577, 0, "texture_pool_over_limit");
    }
    rp_shader_reset(); nvn_unimpl_reset(); diag_enabled = 1; diag_lines = diag_queries = 0; last_diag[0] = 0;
    bind_pools(7); compiler_calls = 0;
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    rp_shader_resolve(&q, &p);
    CHECK(compiler_calls == 1 && strstr(captured, "\"textureArrayLength\":7"), "first pool request=7");
    bind_pools(11); rp_shader_resolve(&q, &p);
    CHECK(compiler_calls == 2 && strstr(captured, "\"textureArrayLength\":11"), "rebind changes request and identity");
    bind_pools(7); rp_shader_resolve(&q, &p);
    CHECK(compiler_calls == 2, "return to prior pool reuses prior identity");
    CHECK(diag_lines == 1 && strstr(last_diag, "texture_pool_capacity=7"), "debug one line records first bound value: count=%d line=%s", diag_lines, last_diag);
    bind_pools(1048577); rp_shader_resolve(&q, &p);
    CHECK(strstr(last_red, "1048577") && strstr(last_red, "1048576"), "after normal pool, overflow reports real value");
    bind_pools(1048578); rp_shader_resolve(&q, &p);
    CHECK(strstr(last_red, "1048578") && strstr(last_red, "1048576"), "changed overflow reports new real value");
    CHECK(diag_lines == 1, "over-limit logging does not duplicate one-time debug line");
    CHECK(diag_queries == 1, "debug property is read once per session, got %d", diag_queries);
    rp_shader_reset(); diag_lines = 0; bind_pools(0); rp_shader_resolve(&q, &p);
    CHECK(diag_lines == 0, "unbound pool does not consume diagnostic");
    bind_pools(524288); rp_shader_resolve(&q, &p);
    CHECK(diag_lines == 1 && strstr(last_diag, "texture_pool_capacity=524288"), "first bound overflow is diagnosed accurately");
    rp_shader_reset(); diag_enabled = 0; diag_lines = diag_queries = 0; bind_pools(7); rp_shader_resolve(&q, &p); rp_shader_resolve(&q, &p);
    CHECK(diag_lines == 0, "disabled diagnostic emits no line");
    CHECK(diag_queries == 1, "disabled debug property read only once, got %d", diag_queries);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned lengths[] = {64, 65, 524288};
        rp_shader_reset(); nvn_unimpl_reset(); last_red[0] = 0; compiler_calls = 0;
        fake_array_count = lengths[i]; bind_pools((int)fake_array_count);
        int rc = rp_shader_resolve(&q, &p);
        CHECK(compiler_calls == 1, "array length %u reaches compiler", fake_array_count);
        if (fake_array_count == 64) CHECK(rc == 0 && p, "array at limit still loads");
        else {
            char number[32]; snprintf(number, sizeof number, "%u", fake_array_count);
            CHECK(rc != 0 && !p, "array length %u refuses program", fake_array_count);
            CHECK(nvn_unimpl_count("shader", "texture_pool_array_unsupported") == 1, "array length %u reports the dedicated unimplemented", fake_array_count);
            CHECK(strstr(last_red, number) && strstr(last_red, "vs"), "array red reports exact length %u and vs: %s", fake_array_count, last_red);
        }
    }
    rp_shader_reset(); diag_enabled = 1; last_stats[0] = 0; fake_array_count = 65; bind_pools(65);
    rp_shader_resolve(&q, &p); rp_shader_resolve(&q, &p);
    last_stats[0] = 0; s2a_direct_summary(0);
    CHECK(!strstr(last_stats, "phase=final"), "timeout summary is never labelled final");
    s2a_direct_summary(1);
    CHECK(strstr(last_stats, "phase=final") && strstr(last_stats, "array_unsupported=1"),
          "normal process-exit callback emits final stats without a session reset: %s", last_stats);
    rp_shader_reset();
    CHECK(strstr(last_stats, "programs=1") && strstr(last_stats, "compile_attempts=1") &&
          strstr(last_stats, "compile_successes=1") && strstr(last_stats, "array_unsupported=1"),
          "session stats count unique failed program once: %s", last_stats);
    fake_array_count = 0;
    {
        const char *errors[] = {
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"fs: texture handle from cbuf slot 0 is not implemented\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"fs: separate sampler slot is not implemented\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureArrayLengthFromBuffer has no source (slot=2)\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureFormat has no source (stage=fs,handle=3,cbufSlot=-1,format=Unknown)\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureCoordNormalized has no source (stage=fs,handle=3,cbufSlot=-1)\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"missing fact\"}",
            "invalid error"};
        const char *reasons[] = {"cbuf_handle_unsupported", "separate_sampler_unsupported",
            "MISSING_FACT.QueryTextureArrayLengthFromBuffer", "MISSING_FACT.QueryTextureFormat",
            "MISSING_FACT.QueryTextureCoordNormalized", "MISSING_FACT.unclassified", "unclassified"};
        for (unsigned i = 0; i < sizeof errors / sizeof errors[0]; i++) {
            rp_shader_reset(); nvn_unimpl_reset(); last_stats[0] = last_reason[0] = 0;
            fake_compile_error = errors[i]; bind_pools(524288); compiler_calls = 0;
            CHECK(rp_shader_resolve(&q, &p) != 0 && rp_shader_resolve(&q, &p) != 0 && compiler_calls == 1,
                  "compiler failure cached once for %s", reasons[i]);
            rp_shader_reset();
            CHECK(strstr(last_stats, "compile_rejected=1") && strstr(last_stats, "array_unsupported=0"),
                  "compiler error stays distinct from layout rejection: %s", last_stats);
            CHECK(strstr(last_reason, reasons[i]) && strstr(last_reason, "count=1"),
                  "compiler reason counted accurately: %s", last_reason);
        }
        rp_shader_reset(); nvn_unimpl_reset(); fake_compile_error = errors[2]; bind_pools(101);
        CHECK(rp_shader_resolve(&q, &p) != 0, "first missing fact rejected");
        fake_compile_error = errors[3]; bind_pools(102);
        CHECK(rp_shader_resolve(&q, &p) != 0, "second missing fact rejected");
        fake_compile_error = NULL; fake_array_count = 65; bind_pools(65);
        CHECK(rp_shader_resolve(&q, &p) != 0, "separate oversized layout rejected");
        bind_pools(101); CHECK(rp_shader_resolve(&q, &p) != 0, "reused failure remains rejected");
        last_stats[0] = reason_history[0] = 0; s2a_direct_summary(1);
        CHECK(strstr(last_stats, "phase=final") && strstr(last_stats, "compile_attempts=3") &&
              strstr(last_stats, "compile_rejected=2") && strstr(last_stats, "array_unsupported=1"),
              "final separates compiled and rejected identities: %s", last_stats);
        CHECK(strstr(reason_history, "reason=MISSING_FACT.QueryTextureArrayLengthFromBuffer count=1") &&
              strstr(reason_history, "reason=MISSING_FACT.QueryTextureFormat count=1"),
              "final reports both distinct missing facts: %s", reason_history);
        fake_array_count = 0;
        fake_compile_error = NULL;
    }
    {
        rp_shader_reset(); diag_enabled = 1; bind_pools(7);
        p_set_pool missing_sampler = {NULL};
        CHECK(rp_state_op(OP_SET_SAMP_POOL, &missing_sampler, sizeof missing_sampler), "sampler pool unbound");
        last_stats[0] = last_request_reason[0] = 0; compiler_calls = 0;
        CHECK(rp_shader_resolve_compute(&p) != 0 && compiler_calls == 0, "invalid pool fact refuses before compiler");
        s2a_direct_summary(1);
        CHECK(strstr(last_stats, "phase=final") && strstr(last_stats, "request_rejected=1") && strstr(last_stats, "compile_rejected=0"),
              "request failure is counted separately from compiler rejection: %s", last_stats);
        CHECK(strstr(last_request_reason, "descriptor_pool_facts") && strstr(last_request_reason, "count=1"),
              "request failure names descriptor-pool facts: %s", last_request_reason);
    }
    {
        // Synthetic registrations model two real descriptor snapshots, not GPU objects.
        rp_shader_reset(); separate_fixture = 1; fake_compile_error = NULL; fake_array_count = 0;
        bind_separate(0, 0, 1, 77); bind_separate(5, 1, 1, 78); bind_pools(100); compiler_calls = 0;
        CHECK(rp_shader_resolve(&q, &p) != 0 && compiler_calls == 1, "separate VS reaches compiler");
        CHECK(fact_matches("vs", 90, "2d", 0), "separate VS registered view fact is sent");
        CHECK(rp_shader_resolve_compute(&p) != 0 && compiler_calls == 2, "separate CS reaches compiler");
        CHECK(fact_matches("cs", 92, "2d", 1), "separate CS registered texture fact is sent");
        CHECK(!fact_matches("cs", 90, "2d", 1), "unregistered separate slot is not invented");
        rp_shader_reset(); fake_array_count = 64; compiler_calls = 0; bind_pools(100);
        CHECK(rp_shader_resolve(&q, &p) == 0 && compiler_calls == 1, "separate facts first program succeeds");
        bind_separate(0, 0, 1, 78);
        CHECK(rp_shader_resolve(&q, &p) == 0 && compiler_calls == 2 && fact_matches("vs", 90, "2d", 1), "separate texture rebind invalidates fast identity");
        fake_array_count = 0;
        rp_shader_reset(); bind_separate(0, 0, 128, 77); bind_separate(5, 0, 128, 78); bind_pools(100); compiler_calls = 0;
        CHECK(rp_shader_resolve(&q, &p) != 0 && compiler_calls == 1 && count_substr(captured, "\"stage\":\"vs\",\"handle\":") == 160 && fact_matches("vs", 344, "2d", 0), "all 128 VS separate slots survive request and identity capacity");
        CHECK(rp_shader_resolve_compute(&p) != 0 && compiler_calls == 2 && count_substr(captured, "\"stage\":\"cs\",\"handle\":") == 160 && fact_matches("cs", 344, "2d", 1), "all 128 CS separate slots survive request and identity capacity");
        rp_shader_reset(); bind_separate(1, 0, 128, 78); memcpy(rp_S.skey[1], rp_S.skey[0], 8); rp_S.skey_ok |= 2; rp_S.bser[1] = 1; compiler_calls = 0;
        int dual_rc = rp_shader_resolve(&q, &p);
        int vs_count = count_substr(captured, "\"stage\":\"vs\",\"handle\":"), fs_count = count_substr(captured, "\"stage\":\"fs\",\"handle\":");
        CHECK(dual_rc != 0 && compiler_calls == 1 && vs_count == 160 && fs_count == 160 && fact_matches("fs", 344, "2d", 1), "320 VS+FS texture facts fit without truncation rc=%d calls=%d vs=%d fs=%d last_red=%s", dual_rc, compiler_calls, vs_count, fs_count, last_red);
        rp_S.skey_ok &= ~2; rp_S.bser[1] = 0; bind_separate(1, 0, 128, 0);
        rp_shader_reset(); bind_separate(0, 0, 128, 0); bind_separate(5, 0, 128, 0); separate_fixture = 2; bind_pools(100); compiler_calls = 0;
        CHECK(rp_shader_resolve(&q, &p) != 0 && compiler_calls == 1 && fact_matches("vs", 90, "2d", 1), "zero handle uses the registered pool-zero descriptor");
        separate_fixture = 0;
    }
    rp_shader_reset(); s2a_source_clear();
    fprintf(stderr, "nvk shader pool: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
