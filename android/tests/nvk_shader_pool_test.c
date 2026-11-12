// 新回放着色器入口 + 真实图形/计算请求构造与身份；编译器边界只捕获 JSON。
// 合成来源/设备，不声称运行真实游戏或着色器。
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "direct/compile_request.h"
#include "direct/source_store.h"
#include "direct/sha256.h"
#include "direct/s2a_direct.h"

static int checks, failures, compiler_calls, diag_enabled, diag_lines, diag_queries;
static char captured[262144], last_red[512], last_diag[512];
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
}
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
// 此夹具只登记一个起点为 0 的池；空池也按真实 nvn_dpool_base 返回 0。
int nvn_dpool_base(const void *pool, int tex) { (void)pool; (void)tex; return 0; }
uint64_t nvn_ga2ca(uint64_t address) { return address; }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) { (void)id; (void)t; (void)v; (void)h; return 0; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int count) { (void)h; (void)n; (void)b; return (uint64_t)count; }
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    memset(c, 0, sizeof *c); c->session = 4; c->generation = 6; c->direct_abi = 1;
    c->profile_json = profile; c->profile_bytes = strlen(profile); c->profile_hash = phash;
    c->translator_closure = "closure-test"; c->storage_buffer_offset_alignment = 16; return 0;
}
static int32_t fake_compile(uint64_t session, uint64_t gen, s2a_direct_bytes req, s2a_direct_emit_fn emit, void *sink) {
    (void)emit; (void)sink; CHECK(session == 4 && gen == 6, "compile session");
    CHECK(req.bytes < sizeof captured, "request fits capture");
    if (req.bytes >= sizeof captured) return -1;
    memcpy(captured, req.data, (size_t)req.bytes); captured[req.bytes] = 0; compiler_calls++;
    return -27; // deliberately no result: capture input only
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return fake_compile; }
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *o, nvn_gpu_err *e) {
    (void)s; (void)o; (void)e; CHECK(0, "unexpected shader load"); return (nvn_gpu_rc){NVN_GPU_E_UNSUPPORTED};
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
        if (pool > 65536) {
            char num[32]; snprintf(num, sizeof num, "%d", pool);
            CHECK(strstr(last_red, num) && strstr(last_red, "65536"), "%s n=%d error states actual and limit: %s", compute ? "cs" : "gfx", pool, last_red);
        }
    }
}
int main(void) {
    unsetenv("S2A_SHADER_REVISION_ROOT"); unsetenv("S2A_DIRECT_CACHE_DIR"); rp_state_reset(); source();
    for (int cs = 0; cs < 2; cs++) {
        run_case(cs, 0, 0, "texture_pool_missing");
        run_case(cs, -1, 0, "texture_pool_missing");
        run_case(cs, 1, 1, ""); run_case(cs, 7, 1, "");
        run_case(cs, 65535, 1, ""); run_case(cs, 65536, 1, "");
        run_case(cs, 65537, 0, "texture_pool_over_limit");
        run_case(cs, 524288, 0, "texture_pool_over_limit");
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
    bind_pools(65537); rp_shader_resolve(&q, &p);
    CHECK(strstr(last_red, "65537") && strstr(last_red, "65536"), "after normal pool, overflow reports real value");
    bind_pools(524288); rp_shader_resolve(&q, &p);
    CHECK(strstr(last_red, "524288") && strstr(last_red, "65536"), "changed overflow reports new real value");
    CHECK(diag_lines == 1, "over-limit logging does not duplicate one-time debug line");
    CHECK(diag_queries == 1, "debug property is read once per session, got %d", diag_queries);
    rp_shader_reset(); diag_lines = 0; bind_pools(0); rp_shader_resolve(&q, &p);
    CHECK(diag_lines == 0, "unbound pool does not consume diagnostic");
    bind_pools(524288); rp_shader_resolve(&q, &p);
    CHECK(diag_lines == 1 && strstr(last_diag, "texture_pool_capacity=524288"), "first bound overflow is diagnosed accurately");
    rp_shader_reset(); diag_enabled = 0; diag_lines = diag_queries = 0; bind_pools(7); rp_shader_resolve(&q, &p); rp_shader_resolve(&q, &p);
    CHECK(diag_lines == 0, "disabled diagnostic emits no line");
    CHECK(diag_queries == 1, "disabled debug property read only once, got %d", diag_queries);
    rp_shader_reset(); s2a_source_clear();
    fprintf(stderr, "nvk shader pool: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
