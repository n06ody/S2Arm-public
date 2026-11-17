// 新回放着色器入口 + 真实图形/计算请求构造与身份；编译器边界只捕获 JSON。
// 合成来源/设备，不声称运行真实游戏或着色器。
#define _DARWIN_C_SOURCE   // macOS：-std=c11 下 mkdtemp 要它才有声明（Linux 由测试脚本加 _GNU_SOURCE）
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
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
#include "direct/jv.h"
#include "direct/program_cache.h"
#include "nvn/nvn_vk_prog_warm.h"
#include "tbuf_spirv_fixtures.h"

// 实现在 nvk_shader.c，声明在 nvk_session.h。这里不包 nvk_session.h：nvk_compile_stop_test.py 拿本文件当夹具，
// 会在同一个翻译单元里把 nvk_session_request_stop 等会话函数定义成 static，包了头就是「static 声明跟在非 static 声明后面」，编不过。
uint64_t nvk_shader_cs_fast_hits(void);
int nvk_shader_last_allocated(void);
void nvk_shader_last_test_put(uint64_t key, const struct rp_prog *prog);
const struct rp_prog *nvk_shader_last_test_find(uint64_t key);

static int checks, failures, compiler_calls, diag_enabled, diag_lines, diag_queries;
static unsigned fake_array_count;
static int separate_fixture;
static int fake_gs_success, fake_gs_result_mode, gs_loads[6], fake_geometry_enabled = 1, fake_depth_clip;
static int fake_gs_fact_deps;
static int fixture_alias_flag = -1;   // 结果合并开关（debug.s2a.nvk_alias）：-1 = 取生产缺省，0 = 关
static int fake_gs_result_tag;        // 非 0：结果里多带一个无关字段，同一份输入的结果字节（摘要）就不同
static int fixture_depth_zero, fixture_origin_lower_left, shader_load_count, shader_release_count;
static int fake_cs_success, cs_loads;
int g_s2a_logfd = 2;
static const char *fake_compile_error;
static const char *fake_compile_file;
static char captured[262144], last_red[512], last_diag[512], last_stats[512], last_reason[512], last_request_reason[512];
static char reason_history[8192];
static const char profile[] = "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
static char external_profile[65536], external_closure[128];
static char external_profile_hash[80];
static const char *capture_output;
static const char phash[] = "sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static prog_t gs_program;
static uint8_t gs_keys[6][8], gs_stages[6], gs_keys_ok;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

void nvn_plat_safepoint(void) {}
// 宿主夹具没有跨局缓存目录：程序预热清单不落盘（nvk_prog_warm 只读平台目录，不读环境变量）。
const char *nvn_plat_dir(int which) { return which == NVN_PLAT_DIR_CACHE ? getenv("S2A_PW_TEST_DIR") : NULL; }
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_compute_warm")) return getenv("S2A_COMPUTE_WARM") ? atoi(getenv("S2A_COMPUTE_WARM")) : dflt;
    if (!strcmp(name, "nvk_texpool_log")) { diag_queries++; return diag_enabled; }
    if (fixture_alias_flag >= 0 && !strcmp(name, "nvk_alias")) return fixture_alias_flag;
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
int rp_depth_zero(void) { return fixture_depth_zero; }
int rp_origin_lower_left(void) { return fixture_origin_lower_left; }
// 此夹具只登记一个起点为 0 的池；空池也按真实 nvn_dpool_base 返回 0。
int nvn_dpool_base(const void *pool, int tex) { (void)pool; (void)tex; return 0; }
uint64_t nvn_ga2ca(uint64_t address) { return address; }
// 只模拟有来源的程序快照和已启用能力，编译器替身仍明确失败；不宣称 Vulkan 或真机通过。
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    if (serial != gs_program.serial) return NULL;
    memcpy(keys, gs_keys, sizeof gs_keys); memcpy(stage, gs_stages, sizeof gs_stages); *ok = gs_keys_ok;
    return &gs_program;
}
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); out->caps = (fake_geometry_enabled ? NVN_GPU_CAP_GEOMETRY : 0) | (fake_depth_clip ? NVN_GPU_CAP_DEPTH_CLIP_CTL : 0); }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) {
    // 77：带视图（改目标为 RECTANGLE）；78 / 79：同一组格式 / 目标 / 深度的两个不同句柄（快路按事实认，不按句柄认）。
    if (!separate_fixture || (id != 77 && id != 78 && id != 79 && !(separate_fixture == 2 && id == 0))) return 0;
    memset(t, 0, sizeof *t); memset(v, 0, sizeof *v); t->d.format = NVN_FORMAT_RGBA8; t->d.target = NVN_TEXTURE_TARGET_2D; t->d.depth = 1;
    if (separate_fixture == 3) t->d.target = NVN_TEXTURE_TARGET_BUFFER;
    *h = id == 77 && separate_fixture != 3; if (*h) { v->set_target = 1; v->target = NVN_TEXTURE_TARGET_RECTANGLE; } return 1;
}
// 用户常量缓冲夹具：rp_pool_find 只认这块宿主内存（真实实现在 rp_entry.c，查登记过的池）
static uint32_t ubo_mem[128];
int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size) {
    (void)mem; (void)size;
    const uint64_t lo = (uint64_t)(uintptr_t)ubo_mem, hi = lo + sizeof ubo_mem;
    return addr >= lo && addr + len <= hi;
}
static int fake_ubo_missing;   // 1：请求里没有 (fs, 92, c3) 的事实就照真编译器报 MISSING_FACT
static uint64_t fake_reg_generation;
static int fake_reg_changes = 1, gs_sep_hash_calls;
uint64_t rp_tex_reg_gen(void) { return fake_reg_changes ? ++fake_reg_generation : fake_reg_generation; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int count) {
    uint64_t hash = 1469598103934665603ull;
    if (h == &rp_S.tex[2][128]) gs_sep_hash_calls++;
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
    c->profile_json = external_profile[0] ? external_profile : profile; c->profile_bytes = strlen(c->profile_json); c->profile_hash = external_profile_hash[0] ? external_profile_hash : phash;
    c->translator_closure = external_closure[0] ? external_closure : "closure-test"; c->storage_buffer_offset_alignment = 16; return 0;
}
static int32_t fake_compile(uint64_t session, uint64_t gen, s2a_direct_bytes req, s2a_direct_emit_fn emit, void *sink) {
    CHECK(session == 4 && gen == 6, "compile session");
    CHECK(req.bytes < sizeof captured, "request fits capture");
    if (req.bytes >= sizeof captured) return -1;
    memcpy(captured, req.data, (size_t)req.bytes); captured[req.bytes] = 0; compiler_calls++;
    if (fake_ubo_missing && !strstr(captured, "\"stage\":\"fs\",\"handle\":92,\"cbufSlot\":3,")) {
        static const char err[] = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QuerySamplerType has no source (stage=fs,handle=92,cbufSlot=3)\"}";
        s2a_direct_bytes payload = {(const uint8_t *)err, sizeof err - 1};
        (void)emit(sink, payload); return -1;
    }
    if (capture_output) {
        FILE *f = fopen(capture_output, "wb");
        CHECK(f != NULL, "real compiler request output opens");
        if (f) { CHECK(fwrite(captured, 1, (size_t)req.bytes, f) == (size_t)req.bytes, "real compiler request output complete"); fclose(f); }
        return -27;
    }
    if (fake_compile_file) {
        FILE *f = fopen(fake_compile_file, "rb");
        if (!f || fseek(f, 0, SEEK_END)) return -1;
        long n = ftell(f);
        if (n <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
        uint8_t *bytes = malloc((size_t)n);
        if (!bytes || fread(bytes, 1, (size_t)n, f) != (size_t)n) { free(bytes); fclose(f); return -1; }
        fclose(f);
        s2a_direct_bytes payload = {bytes, (uint64_t)n};
        int32_t rc = emit(sink, payload); free(bytes); return rc;
    }
    if (fake_gs_success) {
        char result[1536];
        const char *extra = fake_gs_result_mode == 1 ? "" :
            fake_gs_result_mode == 2 ? ",{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"},{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}" :
            fake_gs_result_mode == 3 ? ",{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"},{\"stage\":\"tcs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}" :
            ",{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}";
        char depbuf[768] = "";
        const char *source = fake_gs_fact_deps == 2 ? "texture-fact:zz:8:-1" :
            fake_gs_fact_deps == 3 ? "texture-fact:vs:71:-1" :
            fake_gs_fact_deps == 4 ? "texture-fact:vs:8:99" :
            fake_gs_fact_deps == 5 ? "future-texture-state" : "texture-fact:vs:8:-1";
        if (fake_gs_fact_deps) snprintf(depbuf, sizeof depbuf,
            ",\"contract\":\"s2a-direct-program-content-v1\",\"dependencies\":[{\"source\":\"code:sha256:synthetic\",\"method\":\"GetCode\"},{\"source\":\"%s\",\"method\":\"QuerySamplerType\"},{\"source\":\"texture-fact:fs:10:2\",\"method\":\"QueryTextureCoordNormalized\"},{\"source\":\"code:sha256:synthetic#cbUse\",\"method\":\"QueryConstantBufferUse\",\"arguments\":\"stage=vs\"}]", source);
        if (fake_gs_result_tag) { size_t dn = strlen(depbuf); snprintf(depbuf + dn, sizeof depbuf - dn, ",\"fixtureTag\":%d", fake_gs_result_tag); }
        const char *deps = depbuf;
        int n = snprintf(result, sizeof result, "{\"schema\":\"s2a-direct-program-v1\",\"content\":{\"stages\":[{\"stage\":\"vs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"},{\"stage\":\"fs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}%s],\"bindings\":[]%s}}", extra, deps);
        CHECK(n > 0 && (size_t)n < sizeof result, "GS result fixture fits");
        s2a_direct_bytes payload = {(const uint8_t *)result, (uint64_t)n};
        return emit(sink, payload);
    }
    if (fake_cs_success && strstr(captured, "\"program\":{\"kind\":\"compute\"")) {
        // 合法的计算程序结果（形状同 nvn_vk_program_binding_cap_test.c 的 build_compute）：一个 cs 阶段、零个绑定。
        static const char result[] = "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
            "\"contract\":\"s2a-direct-program-content-v1\",\"kind\":\"compute\",\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],"
            "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}],\"bindings\":[],"
            "\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":[]}}";
        s2a_direct_bytes payload = {(const uint8_t *)result, (uint64_t)(sizeof result - 1)};
        return emit(sink, payload);
    }
    if (fake_compile_error) {
        s2a_direct_bytes payload = {(const uint8_t *)fake_compile_error, (uint64_t)strlen(fake_compile_error)};
        (void)emit(sink, payload); return -1;
    }
    if (fake_array_count) {
        char result[4096];
        int n = snprintf(result, sizeof result,
            "{\"schema\":\"s2a-direct-program-v1\",\"content\":{\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"}],"
            "\"bindings\":[{\"stage\":\"vs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":0,\"count\":%u}]}}", S2A_TEST_VERT_64, fake_array_count);
        s2a_direct_bytes payload = {(const uint8_t *)result, (uint64_t)n};
        return emit(sink, payload);
    }
    return -27; // deliberately no result: capture input only
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return fake_compile; }
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *o, nvn_gpu_err *e) {
    (void)e;
    if (fake_gs_success && (unsigned)s->stage < 6) {
        shader_load_count++; gs_loads[s->stage]++; *o = (nvn_gpu_shader)(uintptr_t)(s->stage + 1); return (nvn_gpu_rc){NVN_GPU_OK};
    }
    if (fake_array_count) { *o = (nvn_gpu_shader)(uintptr_t)1; return (nvn_gpu_rc){NVN_GPU_OK}; }
    if (fake_cs_success && s->stage == NVN_GPU_SH_CS) { cs_loads++; *o = (nvn_gpu_shader)(uintptr_t)7; return (nvn_gpu_rc){NVN_GPU_OK}; }
    CHECK(0, "unexpected shader load"); return (nvn_gpu_rc){NVN_GPU_E_UNSUPPORTED};
}
void nvn_gpu_shader_release(nvn_gpu_shader s) { if (s) shader_release_count++; }
static void wr(uint8_t *p, unsigned x) { p[0]=x; p[1]=x>>8; p[2]=x>>16; p[3]=x>>24; }
static void wr64(uint8_t *p, uint64_t x) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(x >> (8 * i)); }
static void bind_pools(int n);
static void bind_separate(int stage, int first, int count, uint64_t handle);
static int count_substr(const char *s, const char *needle);
static int fact_matches(const char *stage, int handle, const char *sampler, int normalized);

// 原创 Maxwell 头+EXIT；对应阶段与控制快照都来自真实 source_store 入口。
// c1fill 非 0：控制段 +0x700/+0x704 声明一段 0x100 字节的 c1（落在控制段 0x200..0x2ff），内容全填 c1fill；为 0 = 没有 c1（原行为）。
static void gs_source_ex(unsigned st, unsigned variant, unsigned c1fill, uint8_t key[8]) {
    uint8_t ctl[0x760] = {0}, code[0x70] = {0}, digest[32];
    const unsigned type[3] = {1, 5, 4};
    wr(code, (st == 1 ? 2u : 1u) | (3u << 5) | (type[st] << 10));
    if (st == 1) wr(code + 72, 15);
    if (st == 2) { wr(code + 8, 1u << 24); wr(code + 12, 7u << 24); wr(code + 16, 3); }
    wr64(code + 0x50, 0x001f8000fc0007e0ull); wr64(code + 0x58, 0xe30000000007000full);
    wr64(code + 0x60, 0xe30000000007000full); wr64(code + 0x68, 0xe30000000007000full);
    wr(ctl + 0x20, sizeof ctl); ctl[0x100] = (uint8_t)variant;
    if (c1fill) { wr(ctl + 0x700, sizeof code + 0x200); wr(ctl + 0x704, sizeof code + 0x300); memset(ctl + 0x200, (int)c1fill, 0x100); }
    s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h, ctl, sizeof ctl);
    s2a_sha256_update(&h, code, sizeof code); s2a_sha256_final(&h, digest);
    CHECK(!s2a_source_keep(digest, st, code, sizeof code, ctl, sizeof ctl), "GS fixture source stage=%u", st);
    memcpy(key, digest, 8);
}
static void gs_source(unsigned st, unsigned variant, uint8_t key[8]) { gs_source_ex(st, variant, 0, key); }

static void gs_request_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    uint8_t by_stage[3][8], changed[8]; for (unsigned st = 0; st < 3; st++) gs_source(st, 0, by_stage[st]);
    gs_source(2, 1, changed); // 两个来源均在缓存前登记，单独检验 GS 键。
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = 101; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    // 快照着色器顺序刻意为 GS/VS/FS，stage_of 才是阶段真值。
    const unsigned order[3] = {2, 0, 1};
    for (unsigned i = 0; i < 3; i++) { gs_stages[i] = (uint8_t)order[i]; memcpy(gs_keys[i], by_stage[order[i]], 8); }
    gs_keys_ok = 7;
    p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "GS fixture binds program record");
    CHECK(rp_shader_graphics_stages() == 7, "GS fixture effective stage mask is 0x7");
    bind_pools(7); captured[0] = 0; compiler_calls = 0;
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    int rc = rp_shader_resolve(&q, &p);
    CHECK(rc == -1 && p == NULL, "GS compiler boundary deliberately fails closed");
    CHECK(compiler_calls == 1, "GS production request reaches compiler, not stages_0x7_not_vs_fs");
    if (!compiler_calls) return;
    struct s2a_jv root = {captured, strlen(captured)}, program, stages, textures, v;
    char schema[48] = {0};
    CHECK(!s2a_jv_obj_find(root, "schema", &v) && !s2a_jv_str(v, schema, sizeof schema) &&
          !strcmp(schema, "s2a-direct-compile-v2"), "GS request uses v2 schema");
    if (s2a_jv_obj_find(root, "program", &program) || s2a_jv_obj_find(program, "stages", &stages)) {
        CHECK(0, "GS request has program stages"); return;
    }
    CHECK(s2a_jv_arr_count(stages) == 5, "GS request has exactly five NVN slots");
    const char *expected[3] = {"vs", "fs", "gs"};
    for (int i = 0; i < 3; i++) {
        struct s2a_jv obj, field; char name[8] = {0};
        CHECK(!s2a_jv_arr_item(stages, i, &obj) && !s2a_jv_obj_find(obj, "stage", &field) &&
              !s2a_jv_str(field, name, sizeof name) && !strcmp(name, expected[i]), "GS request stage slot %d", i);
    }
    for (int i = 3; i < 5; i++) { struct s2a_jv obj; CHECK(!s2a_jv_arr_item(stages, i, &obj) && s2a_jv_is_null(obj), "unused stage slot %d is null", i); }
    if (s2a_jv_obj_find(program, "textures", &textures)) { CHECK(0, "GS request has texture facts"); return; }
    CHECK(s2a_jv_arr_count(textures) == 96, "GS request retains 96 texture facts");
    int counts[3] = {0};
    for (int i = 0; i < s2a_jv_arr_count(textures); i++) {
        struct s2a_jv obj, field; char name[8] = {0};
        if (!s2a_jv_arr_item(textures, i, &obj) && !s2a_jv_obj_find(obj, "stage", &field) && !s2a_jv_str(field, name, sizeof name))
            for (int st = 0; st < 3; st++) if (!strcmp(name, expected[st])) counts[st]++;
    }
    CHECK(counts[0] == 32 && counts[1] == 32 && counts[2] == 32, "GS request texture facts are 32 per actual stage");
    rp_shader_reset(); fake_gs_success = 1; memset(gs_loads, 0, sizeof gs_loads); p = NULL;
    rc = rp_shader_resolve(&q, &p);
    CHECK(rc == 0 && p, "three-stage compiler result resolves into a replay program");
    CHECK(gs_loads[NVN_GPU_SH_VS] == 1 && gs_loads[NVN_GPU_SH_FS] == 1 && gs_loads[NVN_GPU_SH_GS] == 1,
          "result loads VS FS and GS with their actual stage IDs");
    int calls = compiler_calls;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == calls, "identical GS request hits cache");
    memcpy(gs_keys[0], changed, 8);
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "rebind changed GS source");
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls, "changed GS key recompiles");
    rp_S.tex[2][0] = 99;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls, "changed GS texture fact recompiles");
    bind_pools(100); CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls, "GS pool size update recompiles");
    separate_fixture = 1; bind_separate(2, 0, 1, 77);
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls && fact_matches("gs", 90, "2d", 0), "GS separate texture fact reaches compiler");
    fake_reg_changes = 0; int hashes = gs_sep_hash_calls;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == calls && gs_sep_hash_calls == hashes, "unchanged GS separate facts hit cached signature");
    uint64_t gs_gen = rp_tex_sep_gen[2]; bind_separate(2, 0, 1, 78);
    CHECK(rp_tex_sep_gen[2] == gs_gen + 1, "changed GS separate slot advances its generation");
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls && gs_sep_hash_calls == hashes + 1 && fact_matches("gs", 90, "2d", 1), "changed GS separate slot invalidates cached signature and recompiles");
    calls = compiler_calls; fake_reg_changes = 1;
    bind_separate(0, 0, 128, 77); bind_separate(1, 0, 128, 78); bind_separate(2, 0, 128, 77);
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls &&
          count_substr(captured, "\"stage\":\"vs\",\"handle\":") == 160 &&
          count_substr(captured, "\"stage\":\"fs\",\"handle\":") == 160 &&
          count_substr(captured, "\"stage\":\"gs\",\"handle\":") == 160 &&
          fact_matches("gs", 344, "2d", 0), "480 combined and separate texture facts survive all three stages");
    bind_separate(0, 0, 128, 0); bind_separate(1, 0, 128, 0); bind_separate(2, 0, 128, 0); separate_fixture = 0; fake_reg_changes = 1;
    fake_geometry_enabled = 0; p = NULL;
    CHECK(rp_shader_resolve(&q, &p) != 0 && !p && compiler_calls == calls, "disabled GS fails before compiler");
    fake_geometry_enabled = 1; gs_keys_ok = 3;
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "rebind missing GS key");
    CHECK(rp_shader_resolve(&q, &p) != 0 && compiler_calls == calls, "missing GS key fails before compiler");
    gs_keys_ok = 7; CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "restore GS key");
    for (int mode = 1; mode <= 3; mode++) {
        rp_shader_reset(); nvn_unimpl_reset(); fake_gs_result_mode = mode; p = NULL; last_red[0] = 0;
        CHECK(rp_shader_resolve(&q, &p) != 0 && !p, "missing, duplicate or extra result stage rejected");
        CHECK(nvn_unimpl_count("shader", mode == 2 ? "program_parse_failed" : "result_stage_mismatch") == 1,
              "rejected result stage reports the correct boundary");
    }
    fake_gs_result_mode = 0; rp_shader_reset();
    CHECK(!rp_shader_resolve(&q, &p), "cached GS program exists before source ambiguity");
    uint8_t wrong_code[0x70] = {1}, wrong_control[0x760] = {0};
    CHECK(s2a_source_keep(changed, 2, wrong_code, sizeof wrong_code, wrong_control, sizeof wrong_control) != 0,
          "conflicting GS source rejected and key poisoned");
    p = NULL;
    CHECK(rp_shader_resolve(&q, &p) != 0 && !p, "poisoned GS source cannot hit fast cache");
    fake_gs_success = 0;
}
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
// 计算程序快路：正向路径 + 负对照。夹具让编译器替身对计算请求回一份合法程序（零绑定），第一次派发编成，
// 之后看「不再走完整请求」——两个证据：编译器没再被调用，且 nvk_shader_cs_fast_hits() 加一（只有快路命中才加）。
// reg_changes=1 模拟真机：贴图登记代号每次读都推进（贴图每帧都在登记）；=0 登记表不动，事实摘要的缓存有效。两种都要对。
static void cs_variant_source(unsigned variant, uint8_t digest[32]) {
    uint8_t ctl[0x760] = {0}, code[16] = {1,2,3,4};
    wr(ctl+0x20,0x760); wr(ctl+0x748,8); wr(ctl+0x74c,2); wr(ctl+0x750,1);
    wr(ctl+0x754,17); wr(ctl+0x758,32); wr(ctl+0x700,4); wr(ctl+0x704,8); ctl[0x100] = (uint8_t)variant;
    s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h,ctl,sizeof ctl); s2a_sha256_update(&h,code,sizeof code); s2a_sha256_final(&h,digest);
    CHECK(!s2a_source_keep(digest, 0, code, sizeof code, ctl, sizeof ctl), "CS fixture source variant=%u", variant);
}
// 绑了图像槽 2 的那次编译请求里必须有 c2 字 76、cbufSlot −1 的事实（夹具 78 号是 RGBA8 2D）
static void check_image_fact(int rc_mode) {
    const char *t = strstr(captured, "\"textures\"");
    CHECK(strstr(captured, "{\"stage\":\"cs\",\"handle\":76,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\"") != NULL,
          "CS request carries the bound image's fact at c2 word 76 (reg_changes=%d): %.160s", rc_mode, t ? t : "(none)");
}
static void compute_fast_path_test(void) {
    for (int rc_mode = 1; rc_mode >= 0; rc_mode--) {
        const struct rp_prog *a = NULL, *b = NULL; uint8_t dig2[32];
        rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset(); source(); cs_variant_source(1, dig2);
        fake_cs_success = 1; separate_fixture = 1; fake_reg_changes = rc_mode; compiler_calls = cs_loads = 0; captured[0] = last_red[0] = 0;
        bind_pools(100);
        rp_S.ubo[5][0].addr = 0x1000; rp_S.ubo[5][0].size = 64; rp_S.ssbo[5][0].addr = 0x10000; rp_S.ssbo[5][0].size = 256; rp_S.tex[5][0] = 78;
        CHECK(rp_shader_resolve_compute(&a) == 0 && a && a->is_compute && compiler_calls == 1 && cs_loads == 1,
              "CS first dispatch compiles and loads (reg_changes=%d) calls=%d loads=%d red=%s", rc_mode, compiler_calls, cs_loads, last_red);
        if (!a) continue;
        int calls = compiler_calls; uint64_t hits = nvk_shader_cs_fast_hits();
        CHECK(hits == 0, "CS first dispatch is not a fast-path hit (reg_changes=%d)", rc_mode);
        // 地址 / 长度 / 句柄变了，编译请求读的输入没变：请求与身份逐字节不变，必须直接复用。
        rp_S.ubo[5][0].addr = 0x2000; rp_S.ubo[5][0].size = 128;      // 「有没有」不变
        rp_S.ssbo[5][0].addr = 0x30000; rp_S.ssbo[5][0].size = 1024;  // 仍按 16 对齐
        rp_S.ssbo[5][1].addr = 0x50000; rp_S.ssbo[5][1].size = 64;    // 多绑一个对齐的存储缓冲：不进请求
        rp_S.tex[5][0] = 79;                                          // 另一个句柄，格式 / 目标 / 深度同 78
        CHECK(rp_shader_resolve_compute(&b) == 0 && b == a && compiler_calls == calls && nvk_shader_cs_fast_hits() == hits + 1,
              "CS new addresses and same-fact handles reuse the program without a request (reg_changes=%d)", rc_mode);
        hits = nvk_shader_cs_fast_hits();
#define CS_MISS(msg, setup, undo) do { \
            setup; \
            CHECK(rp_shader_resolve_compute(&b) == 0 && b && b != a && compiler_calls == calls + 1 && nvk_shader_cs_fast_hits() == hits, \
                  msg " (reg_changes=%d) calls=%d expected=%d hits=%llu/%llu", rc_mode, compiler_calls, calls + 1, \
                  (unsigned long long)nvk_shader_cs_fast_hits(), (unsigned long long)hits); \
            calls = compiler_calls; \
            undo; \
            CHECK(rp_shader_resolve_compute(&b) == 0 && b == a && compiler_calls == calls && nvk_shader_cs_fast_hits() == hits + 1, \
                  "CS restored input hits the first entry again after: " msg " (reg_changes=%d)", rc_mode); \
            hits = nvk_shader_cs_fast_hits(); \
        } while (0)
        // 编译请求读的输入，每一项变了都必须重新走请求（编译器被调用、拿到新程序、不算命中）。
        CS_MISS("changed CS source key recompiles", memcpy(rp_S.skey[5], dig2, 8), memcpy(rp_S.skey[5], rp_S.skey[0], 8));
        CS_MISS("changed CS constant-buffer use recompiles", (rp_S.ubo[5][1].addr = 0x6000, rp_S.ubo[5][1].size = 32), (rp_S.ubo[5][1].addr = 0, rp_S.ubo[5][1].size = 0));
        CS_MISS("unaligned CS storage address recompiles", (rp_S.ssbo[5][2].addr = 0x70001, rp_S.ssbo[5][2].size = 16), (rp_S.ssbo[5][2].addr = 0, rp_S.ssbo[5][2].size = 0));
        CS_MISS("CS pool size update recompiles", bind_pools(101), bind_pools(100));
        CS_MISS("changed CS texture fact recompiles", rp_S.tex[5][3] = 77, rp_S.tex[5][3] = 0);
        // 分离槽 100 的下标是 228：旧键只拷了 rp_S.tex[5] 的前 160 项，覆盖不到。
        CS_MISS("changed CS separate texture fact recompiles", bind_separate(5, 100, 1, 78), bind_separate(5, 100, 1, 0));
        // 图像槽 2（c2 字 76）：绑上图像要进请求（图像事实）、快路也要认它；换成同格式的另一张不重编
        captured[0] = 0;
        CS_MISS("binding a CS image recompiles", rp_S.img[5][2] = 78, (check_image_fact(rc_mode), rp_S.img[5][2] = 0));
        { const struct rp_prog *with_img = NULL; rp_S.img[5][2] = 78;
          CHECK(rp_shader_resolve_compute(&with_img) == 0 && with_img && with_img != a, "CS image input has its own entry (reg_changes=%d)", rc_mode);
          calls = compiler_calls; hits = nvk_shader_cs_fast_hits();
          rp_S.img[5][2] = 79;   // 另一张同格式 / 目标 / 深度的图像：请求不变
          CHECK(rp_shader_resolve_compute(&b) == 0 && b == with_img && compiler_calls == calls && nvk_shader_cs_fast_hits() == hits + 1,
                "CS same-fact image handle reuses the program (reg_changes=%d)", rc_mode);
          rp_S.img[5][2] = 0; calls = compiler_calls; hits = nvk_shader_cs_fast_hits(); }
#undef CS_MISS
        // 来源登记冲突会毒化短键并推进代际：同一份状态不能再命中推进之前存下的快路表项（慢路仍给同一个程序、不重编）。
        { uint8_t wrong_code[16] = {9}, wrong_control[0x760] = {0};
          CHECK(s2a_source_keep(dig2, 0, wrong_code, sizeof wrong_code, wrong_control, sizeof wrong_control) != 0,
                "conflicting CS source rejected and key poisoned"); }
        calls = compiler_calls; hits = nvk_shader_cs_fast_hits();
        CHECK(rp_shader_resolve_compute(&b) == 0 && b == a && compiler_calls == calls && nvk_shader_cs_fast_hits() == hits,
              "CS source poison advances the epoch so the cached entry is not reused (reg_changes=%d)", rc_mode);
        CHECK(rp_shader_resolve_compute(&b) == 0 && b == a && compiler_calls == calls && nvk_shader_cs_fast_hits() == hits + 1,
              "CS entry is stored again under the new epoch (reg_changes=%d)", rc_mode);
        // 会话清空：快表与读数一并清掉，同样的状态必须重新编译（里面存的是 rp_prog*，不能跨会话留着）。
        rp_shader_reset();
        CHECK(nvk_shader_cs_fast_hits() == 0, "session reset clears the CS fast-path counter (reg_changes=%d)", rc_mode);
        calls = compiler_calls;
        CHECK(rp_shader_resolve_compute(&b) == 0 && b && compiler_calls == calls + 1 && nvk_shader_cs_fast_hits() == 0,
              "after session reset the same CS state compiles again (reg_changes=%d)", rc_mode);
    }
    fake_cs_success = 0; separate_fixture = 0; fake_reg_changes = 1;
    bind_separate(5, 100, 1, 0); memset(rp_S.img[5], 0, sizeof rp_S.img[5]); memset(rp_S.ubo[5], 0, sizeof rp_S.ubo[5]); memset(rp_S.ssbo[5], 0, sizeof rp_S.ssbo[5]); memset(rp_S.tex[5], 0, sizeof rp_S.tex[5]);
    rp_shader_reset();
}
// 图形与计算共用的快表（#1146）：一帧里依次访问落在同一个旧桶（桶号低 9 位相同）的 5 个键。
// 旧表 512 桶 × 4 路装不下 5 个键，轮转替换遇上周期访问就是每次都落空；4096 桶把它们散开（第 9..11 位不同），
// 8 轮里只有第一轮冷启动的 5 次落空。同时钉死表的生命周期：会话外没有分配、第一次存项时才分配、会话收尾释放，
// 收尾后旧项（里面存的是 rp_prog*）一个都取不到。
static void last_table_test(void) {
    static struct rp_prog progs[5];
    uint64_t keys[5];
    rp_shader_reset();
    CHECK(!nvk_shader_last_allocated(), "fast table is not allocated outside a session");
    CHECK(nvk_shader_last_test_find(0x15) == NULL, "empty fast table finds nothing");
    for (int i = 0; i < 5; i++) keys[i] = ((uint64_t)i << 9) | 0x15;
    int misses = 0;
    for (int round = 0; round < 8; round++)
        for (int i = 0; i < 5; i++)
            if (nvk_shader_last_test_find(keys[i]) != &progs[i]) { misses++; nvk_shader_last_test_put(keys[i], &progs[i]); }
    CHECK(misses == 5, "5 keys sharing the low 9 bits of the bucket index coexist in the fast table: %d misses in 8 rounds (cold start only = 5)", misses);
    CHECK(nvk_shader_last_allocated(), "fast table is allocated on the first store");
    rp_shader_reset();
    CHECK(!nvk_shader_last_allocated(), "fast table is released at session retirement");
    CHECK(nvk_shader_last_test_find(keys[0]) == NULL, "entries holding rp_prog pointers do not survive session retirement");
}
static int emit_real_request(const char *profile_path, const char *closure, const char *dir, const char *output, int tbuf) {
    FILE *pf = fopen(profile_path, "rb"); if (!pf) return 1;
    size_t pn = fread(external_profile, 1, sizeof external_profile - 1, pf);
    int extra = fgetc(pf); fclose(pf); if (!pn || extra != EOF || strlen(closure) >= sizeof external_closure) return 1;
    external_profile[pn] = 0; snprintf(external_closure, sizeof external_closure, "%s", closure);
    struct s2a_jv profile_obj = {external_profile, pn}, hash_field;
    if (s2a_jv_obj_find(profile_obj, "profileHash", &hash_field) || s2a_jv_str(hash_field, external_profile_hash, sizeof external_profile_hash)) return 1;
    // GS 整链（--emit-real 及其复用变体）：生成的请求要与 vk/t_replay_gs 在 lavapipe 上发出的逐字节相同；lavapipe 有 VK_EXT_depth_clip_control，
    //   有这项能力时现编不改写 z（nvk_shader.c fill_facts），所以这里同样报有（设备没有时 t_replay_gs 的逐字节比对会直接红）
    fake_depth_clip = tbuf == 0 || tbuf >= 3;   // GS 整链的基准（0）、执行复用（3）、三种宽松输入（4–6）同一口径，内容才可比；贴图缓冲链（1 / 2）不变
    rp_state_reset(); rp_shader_reset(); s2a_source_clear();
    uint8_t keys[3][8];
    for (unsigned st = 0; st < 3; st++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s.bin", dir, (const char *const[]){"vs","fs","gs"}[st]);
        FILE *f = fopen(path, "rb"); if (!f) return 1;
        uint8_t code[4096], ctl[0x760] = {0}, digest[32]; size_t n = fread(code, 1, sizeof code, f);
        int beyond = fgetc(f); fclose(f); if (!n || beyond != EOF) return 1;
        wr(ctl + 0x20, sizeof ctl); s2a_sha256 h; s2a_sha256_init(&h);
        s2a_sha256_update(&h, ctl, sizeof ctl); s2a_sha256_update(&h, code, n); s2a_sha256_final(&h, digest);
        if (s2a_source_keep(digest, st, code, (uint32_t)n, ctl, sizeof ctl)) return 1;
        memcpy(keys[st], digest, 8);
    }
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G'); gs_program.serial = 101; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < 3; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = 7; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    if (!rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8)) return 1;
    bind_pools(tbuf == 1 || tbuf == 2 || tbuf == 7 ? 100 : 7);
    if (tbuf == 1 || tbuf == 2 || tbuf == 7) { separate_fixture = 3; bind_separate(1, 0, 1, 77); }
    if (tbuf == 2 || tbuf == 3 || tbuf == 7) { fake_compile_file = output; fake_gs_success = 1; nvn_unimpl_reset(); memset(gs_loads, 0, sizeof gs_loads); }
    else capture_output = output;
    if (tbuf == 4) rp_S.tex[0][0] = 99;
    if (tbuf == 6) rp_S.ubo[5][15].size = 16;
    compiler_calls = 0;
    struct rp_shader_req q = {0}; q.prim = 0; q.rt_count = 1; q.rt_format[0] = NVN_FORMAT_RGBA8;
    if (tbuf == 5) { q.rt_count=2; q.rt_format[1]=NVN_FORMAT_RGBA8; }
    const struct rp_prog *prog = NULL; int rc = rp_shader_resolve(&q, &prog);
    if (tbuf == 7) {
        int rejected = rc == -1 && !prog && compiler_calls == 1 &&
            nvn_unimpl_count("shader", "texture_buffer_replay_unsupported") == 1 && gs_loads[1] == 0;
        fprintf(stderr, "%s: graphics TBUFU is rejected before shader load\n", rejected ? "PASS" : "FAIL");
        return !rejected;
    }
    if (tbuf == 3) {
        CHECK(!rc && prog && compiler_calls == 1, "real compiler result loads for dependency reuse");
        const struct rp_prog *first = prog;
        rp_S.tex[0][0] = 99;
        CHECK(!rp_shader_resolve(&q, &prog) && prog == first && compiler_calls == 1,
              "real compiler dependency trace permits unused texture reuse");
        rp_S.ubo[0][15].size = 16;
        CHECK(!rp_shader_resolve(&q, &prog) && prog != first && compiler_calls == 2,
              "real compiler active-stage UBO mask remains strict without dependency queries");
        fprintf(stderr, "real compiler dependency reuse: checks=%d failures=%d\n", checks, failures);
        return failures != 0;
    }
    if (tbuf == 2) {
        int found = 0;
        if (prog) for (uint32_t i = 0; i < prog->nent[1]; i++)
            if (prog->ent[1][i].cls == NVN_GPU_RES_TBUF && prog->ent[1][i].src_reg == 128 &&
                prog->dtt[1][i] == NVN_TEXTURE_TARGET_BUFFER + 1 && prog->dcls[1][i] == 0) found++;
        int accepted = rc == 0 && prog && compiler_calls == 1 && gs_loads[1] == 1 && found == 1;
        fprintf(stderr, "%s: real BUFFER result loads FS with declared TBUF layout\n", accepted ? "PASS" : "FAIL");
        return !accepted;
    }
    printf("production Maxwell request: rc=%d captured=%d failures=%d\n", rc, compiler_calls, failures);
    return !(rc == -1 && compiler_calls == 1 && failures == 0);
}

static int emit_compute_request(const char *profile_path, const char *closure, const char *dir, const char *output, int replay) {
    FILE *pf = fopen(profile_path, "rb"); if (!pf) return 1;
    size_t pn = fread(external_profile, 1, sizeof external_profile - 1, pf);
    int extra = fgetc(pf); fclose(pf); if (!pn || extra != EOF || strlen(closure) >= sizeof external_closure) return 1;
    external_profile[pn] = 0; snprintf(external_closure, sizeof external_closure, "%s", closure);
    struct s2a_jv profile_obj = {external_profile, pn}, hash_field;
    if (s2a_jv_obj_find(profile_obj, "profileHash", &hash_field) || s2a_jv_str(hash_field, external_profile_hash, sizeof external_profile_hash)) return 1;
    rp_state_reset(); rp_shader_reset(); s2a_source_clear();
    char path[1024]; snprintf(path, sizeof path, "%s/cs.bin", dir);
    FILE *f = fopen(path, "rb"); if (!f) return 1;
    uint8_t code[4096], ctl[0x760] = {0}, digest[32]; size_t n = fread(code, 1, sizeof code, f);
    extra = fgetc(f); fclose(f); if (!n || extra != EOF) return 1;
    wr(ctl + 0x20, sizeof ctl); wr(ctl + 0x748, 1); wr(ctl + 0x74c, 1); wr(ctl + 0x750, 1);
    s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h, ctl, sizeof ctl); s2a_sha256_update(&h, code, n); s2a_sha256_final(&h, digest);
    if (s2a_source_keep(digest, 0, code, (uint32_t)n, ctl, sizeof ctl)) return 1;
    memcpy(rp_S.skey[5], digest, 8); rp_S.skey_ok = 1 << 5;
    bind_pools(100); separate_fixture = 3;
    p_bind_handles handle = {0}; handle.stage = 5; handle.first = 0; handle.n = 1; handle.h[0] = 77;
    if (!rp_state_op(OP_BIND_TEX, &handle, sizeof handle)) return 1;
    if (replay) { fake_compile_file = output; fake_gs_success = 1; nvn_unimpl_reset(); memset(gs_loads, 0, sizeof gs_loads); }
    else capture_output = output;
    compiler_calls = 0; const struct rp_prog *prog = NULL; int rc = rp_shader_resolve_compute(&prog);
    if (replay == 2) {
        int rejected = rc == -1 && !prog && compiler_calls == 1 &&
            nvn_unimpl_count("shader", "compute_program_parse_failed") == 1 &&
            strstr(last_red, "compute binding has no reflected guest slot") && gs_loads[NVN_GPU_SH_CS] == 0;
        fprintf(stderr, "%s: compute TBUFU is rejected by parser before shader load (rc=%d red=%s)\n", rejected ? "PASS" : "FAIL", rc, last_red);
        return !rejected;
    }
    if (replay) {
        int found = 0;
        if (prog) for (uint32_t i = 0; i < prog->nent[0]; i++)
            if (prog->ent[0][i].cls == NVN_GPU_RES_TBUF && prog->ent[0][i].src_reg == 0 &&
                prog->dtt[0][i] == NVN_TEXTURE_TARGET_BUFFER + 1 && prog->dcls[0][i] == 0) found++;
        int accepted = rc == 0 && prog && compiler_calls == 1 && gs_loads[NVN_GPU_SH_CS] == 1 && found == 1;
        fprintf(stderr, "%s: real compute BUFFER result loads CS with declared TBUF layout\n", accepted ? "PASS" : "FAIL");
        return !accepted;
    }
    printf("production compute Maxwell request: rc=%d captured=%d failures=%d red=%s\n", rc, compiler_calls, failures, last_red);
    return !(rc == -1 && compiler_calls == 1 && failures == 0);
}

static void graphics_fact_reuse_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    uint8_t keys[3][8]; for (unsigned st = 0; st < 3; st++) gs_source(st, 0, keys[st]);
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = 900; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < 3; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = 7; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "fact reuse program binds");
    bind_pools(100); fake_gs_success = fake_gs_fact_deps = 1; separate_fixture = 1; fake_reg_changes = 0;
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    compiler_calls = 0; CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact reuse initial real producer resolves");
    const struct rp_prog *first = p;
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "unqueried combined texture reuses compiled program");
    rp_S.tex[1][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "other stage unqueried combined texture reuses program");
    rp_S.tex[0][0] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "queried texture format changes recompile");
    rp_S.tex[1][1] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 3, "queried c2 fallback texture changes recompile");
    rp_S.ubo[0][15].size = 16;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 4, "active UBO change still recompiles under fact reuse");
    rp_S.ubo[1][15].size = 16;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 5, "unqueried active-stage UBO mask remains strict");
    q.rt_count = 1; q.rt_format[0] = NVN_FORMAT_RGBA8;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 5, "attachment count alone does not split shader identity");
    q.rt_format[0] = NVN_FORMAT_R32UI;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 6, "changed compiler output numeric type recompiles");
    // A BUFFER fact excludes the entire request from the dependency shortcut,
    // even if the mock result itself does not reflect a buffer binding.
    rp_shader_reset(); rp_state_reset();
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "TBUF fact identity program binds");
    bind_pools(100); separate_fixture = 3; fake_reg_changes = 0; compiler_calls = 0;
    rp_S.tex[0][3] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "TBUF fact identity initial compile");
    rp_S.tex[0][4] = 78;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 2, "TBUF input fact prevents dependency identity reuse");
    rp_shader_reset(); rp_state_reset();
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "fact reuse program rebinds after TBUF test");
    bind_pools(100); separate_fixture = 1;
    for (int mode = 0; mode <= 5; mode++) if (mode != 1) {
        rp_shader_reset(); fake_gs_fact_deps = mode; compiler_calls = 0; rp_S.tex[0][3] = 0;
        CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 1, "unsupported dependency fixture resolves");
        rp_S.tex[0][3] = 77;
        CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "unsupported dependency metadata preserves exact identity: mode=%d", mode);
    }
    rp_shader_reset(); fake_gs_fact_deps = 1; compiler_calls = shader_load_count = shader_release_count = 0;
    rp_S.tex[0][3] = 0;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 1, "reset strict-state fixture");
#define STRICT_GFX(label, change) do { int before=compiler_calls; change; CHECK(!rp_shader_resolve(&q,&p) && compiler_calls==before+1, "strict graphics state: " label); } while (0)
    STRICT_GFX("depth mode", fixture_depth_zero=1);
    STRICT_GFX("Y negate", fixture_origin_lower_left=1);
    STRICT_GFX("alpha comparison", rp_S.color.alpha_func=2);
    STRICT_GFX("alpha reference", rp_S.alpha_ref=0.5f);
    STRICT_GFX("alpha coverage", rp_S.ms.a2c=1);
    STRICT_GFX("alpha coverage dither", rp_S.ms.a2c_dither=1);
    STRICT_GFX("dual source", rp_S.blend[0].src_rgb=0x10);
    STRICT_GFX("vertex numeric type", rp_S.nattr=1; rp_S.attr[0].fmt=NVN_FORMAT_R32UI);
    STRICT_GFX("output numeric type", q.rt_format[0]=NVN_FORMAT_RGBA8);
#undef STRICT_GFX
    const struct rp_prog *last=p; int before=compiler_calls;
    rp_S.tex[0][3]=77;
    CHECK(!rp_shader_resolve(&q,&p) && p==last && compiler_calls==before,"alias reuse before reset");
    rp_shader_reset();
    CHECK(shader_release_count==shader_load_count,"owned shaders released once despite aliases: %d/%d",shader_release_count,shader_load_count);
    CHECK(!rp_shader_resolve(&q,&p) && compiler_calls==before+1,"reset clears aliases and dependency index before next resolve");
    rp_shader_reset();
    CHECK(shader_release_count==shader_load_count,"second session releases each shader once");
    fixture_depth_zero=fixture_origin_lower_left=0;
    fake_gs_success = fake_gs_fact_deps = 0; separate_fixture = 0; fake_reg_changes = 1;
    rp_shader_reset(); rp_state_reset();
}

// ---------- 结果合并（#1355）：前台路径 ----------
// 真 program_cache.c + 临时缓存目录。会话 1 = 「上一局」：几份身份各自现编、落盘；会话 2 = 「本局」：从盘上读头合并。
// 「同一份编译结果、不同身份」= 只差一项绘制状态（深度模式 / Y 翻转），fake_compile 给出的结果字节逐位相同；
// 「结果不同」= 结果里多带一个无关字段（fake_gs_result_tag）；「c1 不同」= 控制段里 c1 范围的内容不同（来源摘要也随之不同）。
static void alias_bind(uint8_t keys[3][8]) {
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = 1355; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < 3; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = 7;
    p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "alias fixture binds program");
}
static void alias_rmdir(const char *dir) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e; char path[512];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name); (void)unlink(path);
    }
    closedir(d); (void)rmdir(dir);
}
static void graphics_alias_test(int flag_on) {
    const char *tag = flag_on ? "on" : "off";
    char dir[] = "/tmp/s2a-alias-fg-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "alias[%s]: temp cache dir", tag); return; }
    setenv("S2A_DIRECT_CACHE_DIR", dir, 1);
    uint8_t k1[3][8], k2[3][8];
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    for (unsigned st = 0; st < 3; st++) { gs_source_ex(st, 0, 0x11, k1[st]); gs_source_ex(st, 0, 0x22, k2[st]); }
    fake_gs_success = 1; fake_gs_fact_deps = 0; fake_gs_result_tag = 1; separate_fixture = 0; fake_reg_changes = 0;   // 结果带 1 位数字的标记字段：换标记 = 摘要变、长度不变
    fixture_depth_zero = fixture_origin_lower_left = 0;
    alias_bind(k1); bind_pools(100);
    struct rp_shader_req q = {0}; q.prim = 4;
    const struct rp_prog *a = NULL, *b = NULL, *c = NULL, *d = NULL;
    // —— 会话 1（上一局）：四份身份各自现编（盘上还没有它们的文件）、落盘 ——
    compiler_calls = 0;
    CHECK(!rp_shader_resolve(&q, &a) && a && compiler_calls == 1, "alias[%s] s1: A compiles live", tag);
    fixture_depth_zero = 1;                                                    // B：只差深度模式 → 身份不同，结果与 A 逐位相同
    CHECK(!rp_shader_resolve(&q, &b) && b && b != a && compiler_calls == 2, "alias[%s] s1: B (other identity, same result) compiles live", tag);
    fixture_depth_zero = 0; fixture_origin_lower_left = 1; fake_gs_result_tag = 2;   // C：身份不同，结果也不同（同长度、摘要不同）
    CHECK(!rp_shader_resolve(&q, &c) && c && compiler_calls == 3, "alias[%s] s1: C (other identity, other result) compiles live", tag);
    fixture_origin_lower_left = 0; fake_gs_result_tag = 1; alias_bind(k2);     // D：同一份结果，来源的 c1 内容不同
    CHECK(!rp_shader_resolve(&q, &d) && d && compiler_calls == 4, "alias[%s] s1: D (same result, other c1) compiles live", tag);
    CHECK(s2a_program_cache_drain(5000) == 0, "alias[%s] s1: cache writes drained", tag);
    rp_shader_reset();
    CHECK(shader_release_count == shader_load_count, "alias[%s] s1 end: every backend shader released exactly once (%d/%d)", tag, shader_release_count, shader_load_count);
    // —— 会话 2（本局）——
    shader_load_count = shader_release_count = 0; compiler_calls = 0;
    uint64_t hits0 = s2a_program_cache_hits();
    alias_bind(k1);
    const struct rp_prog *a2 = NULL, *b2 = NULL, *c2 = NULL, *d2 = NULL;
    CHECK(!rp_shader_resolve(&q, &a2) && a2 && compiler_calls == 0 && s2a_program_cache_hits() == hits0 + 1 && shader_load_count == 3,
          "alias[%s] s2: A comes from its cache file (one read, three backend shaders, no live compile)", tag);
    fixture_depth_zero = 1;
    CHECK(!rp_shader_resolve(&q, &b2) && b2 && compiler_calls == 0, "alias[%s] s2: B resolves without a live compile", tag);
    if (flag_on) {
        CHECK(b2 == a2, "alias[on] s2: same result -> same program object (shared shader handles, so backend pipelines merge)");
        CHECK(s2a_program_cache_hits() == hits0 + 1, "alias[on] s2: merged identity reads no file (foreground reads stay at 1, now %d)", (int)(s2a_program_cache_hits() - hits0));
        CHECK(shader_load_count == 3, "alias[on] s2: merged identity builds no backend shaders (now %d)", shader_load_count);
    } else {
        CHECK(b2 != a2 && s2a_program_cache_hits() == hits0 + 2 && shader_load_count == 6,
              "alias[off] s2: B reads its own file and builds its own shaders (reads %d, shaders %d)", (int)(s2a_program_cache_hits() - hits0), shader_load_count);
    }
    uint64_t hits1 = s2a_program_cache_hits(); int loads1 = shader_load_count;
    fixture_depth_zero = 0; fixture_origin_lower_left = 1;                      // C：结果不同 → 不合并
    CHECK(!rp_shader_resolve(&q, &c2) && c2 && c2 != a2 && c2 != b2 && compiler_calls == 0 && s2a_program_cache_hits() == hits1 + 1 && shader_load_count == loads1 + 3,
          "alias[%s] s2: different result is not merged (reads its own file, builds its own shaders)", tag);
    hits1 = s2a_program_cache_hits(); loads1 = shader_load_count;
    fixture_origin_lower_left = 0; alias_bind(k2);                              // D：结果相同、c1 不同 → 不合并
    CHECK(!rp_shader_resolve(&q, &d2) && d2 && d2 != a2 && compiler_calls == 0 && s2a_program_cache_hits() == hits1 + 1 && shader_load_count == loads1 + 3,
          "alias[%s] s2: different c1 is not merged (reads its own file, builds its own shaders)", tag);
    // —— 本局收尾后再来（常驻的程序没了）：合并的只是「谁先来谁常驻」，不是永久指向 ——
    rp_shader_reset();
    CHECK(shader_release_count == shader_load_count, "alias[%s] s2 end: aliased identities release each backend shader once (%d/%d)", tag, shader_release_count, shader_load_count);
    shader_load_count = shader_release_count = 0;
    hits0 = s2a_program_cache_hits(); alias_bind(k1); fixture_depth_zero = 1;
    const struct rp_prog *b3 = NULL, *a3 = NULL;
    CHECK(!rp_shader_resolve(&q, &b3) && b3 && compiler_calls == 0 && s2a_program_cache_hits() == hits0 + 1 && shader_load_count == 3,
          "alias[%s] s3: after the resident program is gone, B reads its own file again", tag);
    fixture_depth_zero = 0;
    if (flag_on) CHECK(!rp_shader_resolve(&q, &a3) && a3 == b3 && s2a_program_cache_hits() == hits0 + 1 && shader_load_count == 3,
                       "alias[on] s3: A now merges into the returned B");
    else CHECK(!rp_shader_resolve(&q, &a3) && a3 && a3 != b3 && s2a_program_cache_hits() == hits0 + 2 && shader_load_count == 6,
               "alias[off] s3: A reads its own file");
    rp_shader_reset();
    CHECK(shader_release_count == shader_load_count, "alias[%s] s3 end: each backend shader released once (%d/%d)", tag, shader_release_count, shader_load_count);
    unsetenv("S2A_DIRECT_CACHE_DIR"); alias_rmdir(dir);
    fixture_depth_zero = fixture_origin_lower_left = 0; fake_gs_success = 0; fake_gs_result_tag = 0; fake_reg_changes = 1;
    rp_shader_reset(); rp_state_reset();
}
// 事实复用（#1187）与结果合并同时在时：合并命中的身份也要进事实复用表，否则「只差编译器没读的贴图事实」的新身份（盘上没有它的文件）
// 退回现场编（真机一局现编 3 → 198 次的根因）。
static void graphics_alias_fact_test(int flag_on) {
    const char *tag = flag_on ? "on" : "off";
    char dir[] = "/tmp/s2a-alias-fact-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "alias fact[%s]: temp cache dir", tag); return; }
    setenv("S2A_DIRECT_CACHE_DIR", dir, 1);
    uint8_t k1[3][8];
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    for (unsigned st = 0; st < 3; st++) gs_source_ex(st, 0, 0x11, k1[st]);
    fake_gs_success = 1; fake_gs_fact_deps = 1; fake_gs_result_tag = 0; separate_fixture = 1; fake_reg_changes = 0;
    fixture_depth_zero = fixture_origin_lower_left = 0;
    alias_bind(k1); bind_pools(100);
    struct rp_shader_req q = {0}; q.prim = 4;
    const struct rp_prog *f = NULL, *g = NULL, *h = NULL;
    compiler_calls = 0;
    CHECK(!rp_shader_resolve(&q, &f) && f && compiler_calls == 1, "alias fact[%s] s1: F compiles live", tag);
    fixture_depth_zero = 1;
    CHECK(!rp_shader_resolve(&q, &g) && g && compiler_calls == 2, "alias fact[%s] s1: G (only the depth mode differs) compiles live", tag);
    CHECK(s2a_program_cache_drain(5000) == 0, "alias fact[%s] s1: cache writes drained", tag);
    rp_shader_reset(); compiler_calls = 0; alias_bind(k1); fixture_depth_zero = 0;
    CHECK(!rp_shader_resolve(&q, &f) && f && compiler_calls == 0, "alias fact[%s] s2: F comes from its cache file", tag);
    fixture_depth_zero = 1;
    CHECK(!rp_shader_resolve(&q, &g) && g && compiler_calls == 0 && (flag_on ? g == f : g != f), "alias fact[%s] s2: G %s", tag, flag_on ? "merges into F" : "reads its own file");
    rp_S.tex[0][2] = 77;   // 编译器没读的贴图事实（同 graphics_fact_reuse_test）：只差它的新身份，盘上没有它的文件
    CHECK(!rp_shader_resolve(&q, &h) && h && compiler_calls == 0 && h == g,
          "alias fact[%s] s2: identity differing only in an unread texture fact reuses the program (no live compile, calls=%d)", tag, compiler_calls);
    unsetenv("S2A_DIRECT_CACHE_DIR"); alias_rmdir(dir);
    fixture_depth_zero = fixture_origin_lower_left = 0; fake_gs_success = fake_gs_fact_deps = 0; separate_fixture = 0; fake_reg_changes = 1;
    rp_shader_reset(); rp_state_reset();
}
// 同上，但常驻的那份是预热线程（nvk_prog_warm_build）从缓存文件建出来的：真机上被合并的程序绝大多数是预热线程建的，
// 它们也要带着「编译器读了哪些绑定事实」的掩码（warm_work_run），合并命中的前台身份才能登记事实复用。
static void graphics_alias_fact_bg_test(int flag_on) {
    const char *tag = flag_on ? "on" : "off";
    char dir[] = "/tmp/s2a-alias-factbg-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "alias fact-bg[%s]: temp cache dir", tag); return; }
    setenv("S2A_DIRECT_CACHE_DIR", dir, 1);
    uint8_t k1[3][8];
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    for (unsigned st = 0; st < 3; st++) gs_source_ex(st, 0, 0x11, k1[st]);
    fake_gs_success = 1; fake_gs_fact_deps = 1; fake_gs_result_tag = 0; separate_fixture = 1; fake_reg_changes = 0;
    fixture_depth_zero = fixture_origin_lower_left = 0;
    alias_bind(k1); bind_pools(100);
    struct rp_shader_req q = {0}; q.prim = 4;
    const struct rp_prog *f = NULL, *g = NULL, *h = NULL;
    compiler_calls = 0;
    CHECK(!rp_shader_resolve(&q, &f) && f && compiler_calls == 1, "alias fact-bg[%s] s1: F compiles live", tag);
    struct nvk_prog_rec rec; memset(&rec, 0, sizeof rec);   // 清单里 F 的那一条（身份、阶段掩码、源键、c1），预热线程照它建
    if (!f || !rp_shader_prog_identity(f, rec.identity)) { CHECK(0, "alias fact-bg[%s] s1: F has an identity", tag); goto done; }   // F 没建出来：别往下解引用（变异体下的夹具会走到这里）
    rec.stage_mask = 7u; rec.c1_ok = f->c1_ok;
    for (unsigned st = 0; st < 3; st++) { memcpy(rec.keys[st], k1[st], 8); memcpy(rec.c1[st], f->c1[st], NVK_PROG_REC_C1_BYTES); }
    fixture_depth_zero = 1;
    CHECK(!rp_shader_resolve(&q, &g) && g && compiler_calls == 2, "alias fact-bg[%s] s1: G (only the depth mode differs) compiles live", tag);
    CHECK(s2a_program_cache_drain(5000) == 0, "alias fact-bg[%s] s1: cache writes drained", tag);
    rp_shader_reset(); compiler_calls = 0; alias_bind(k1); fixture_depth_zero = 0;
    CHECK(nvk_prog_warm_build(&rec) == 0 && compiler_calls == 0, "alias fact-bg[%s] s2: the warm thread builds F from its cache file", tag);
    fixture_depth_zero = 1;
    CHECK(!rp_shader_resolve(&q, &g) && g && compiler_calls == 0, "alias fact-bg[%s] s2: G resolves without a live compile", tag);
    rp_S.tex[0][2] = 77;   // 编译器没读的贴图事实：只差它的新身份，盘上没有它的文件
    CHECK(!rp_shader_resolve(&q, &h) && h && compiler_calls == 0 && h == g,
          "alias fact-bg[%s] s2: identity differing only in an unread texture fact reuses the program (no live compile, calls=%d)", tag, compiler_calls);
done:
    unsetenv("S2A_DIRECT_CACHE_DIR"); alias_rmdir(dir);
    fixture_depth_zero = fixture_origin_lower_left = 0; fake_gs_success = fake_gs_fact_deps = 0; separate_fixture = 0; fake_reg_changes = 1;
    rp_shader_reset(); rp_state_reset();
}

/* 句柄在游戏用户常量缓冲里的贴图（#999 第 4 片）：编译器按 (fs, 字 92, c3) 报缺事实 → 宿主读 c3 第 92 字 → 贴图号 → 补事实当场重编。 */
static int ubo_fact_in_request(const char *sampler, int normalized) {
    const char *p = strstr(captured, "\"stage\":\"fs\",\"handle\":92,\"cbufSlot\":3,"), *end = p ? strchr(p, '}') : NULL;
    char type[64], norm[32];
    snprintf(type, sizeof type, "\"samplerType\":\"%s\"", sampler); snprintf(norm, sizeof norm, "\"normalized\":%s", normalized ? "true" : "false");
    const char *t = p ? strstr(p, type) : NULL, *n = p ? strstr(p, norm) : NULL;
    return end && t && t < end && n && n < end;
}
static void graphics_ubo_handle_fact_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    uint8_t keys[2][8]; for (unsigned st = 0; st < 2; st++) gs_source(st, 0, keys[st]);
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = 950; gs_program.nstage = 2;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < 2; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = 3; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "UBO handle fixture binds");
    bind_pools(100); separate_fixture = 1; fake_reg_changes = 0;
    fake_gs_success = 1; fake_gs_result_mode = 1; fake_ubo_missing = 1;
    memset(ubo_mem, 0, sizeof ubo_mem); ubo_mem[92] = 78;   // 池起点 0：句柄低 20 位就是贴图号
    rp_S.ubo[1][0].addr = (uint64_t)(uintptr_t)ubo_mem; rp_S.ubo[1][0].size = sizeof ubo_mem;
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    compiler_calls = 0; nvn_unimpl_reset();
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 2, "missing c3 handle fact is learned and compiled in the same draw: calls=%d", compiler_calls);
    CHECK(nvn_unimpl_count("shader", "compile_failed") == 0, "a rejection that taught a coordinate is a learning step, not an unimplemented compile_failed (count=%llu)", (unsigned long long)nvn_unimpl_count("shader", "compile_failed"));
    CHECK(ubo_fact_in_request("2d", 1), "retry request carries (fs,92,c3) fact from texture 78");
    const struct rp_prog *first = p;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 2, "same UBO texture takes the fast path");
    ubo_mem[92] = 77;   // 77 带 RECTANGLE 视图：坐标不归一，编译事实不同
    CHECK(!rp_shader_resolve(&q, &p) && p && p != first && compiler_calls == 3, "UBO now names a texture with other facts: recompiles (calls=%d)", compiler_calls);
    CHECK(ubo_fact_in_request("2d", 0), "recompile carries texture 77's unnormalized fact");
    ubo_mem[92] = 79;   // 79 与 78 事实相同：回到第一份程序，不重编
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 3, "same facts as 78 reuse the first program (calls=%d)", compiler_calls);
    // 反例：句柄字读不到（槽长度不够盖住第 92 字）→ 不给事实：请求与第一次缺事实那份逐字节相同，命中那份失败项，不编、不猜、不循环
    rp_S.ubo[1][0].size = 92 * 4;
    int before = compiler_calls; captured[0] = 0;
    CHECK(rp_shader_resolve(&q, &p) == -1 && compiler_calls == before && !captured[0], "unreadable handle word reuses the fact-less failure (calls=%d)", compiler_calls - before);
    CHECK(nvn_unimpl_count("shader", "ubo_handle_unreadable") >= 1, "unreadable handle word is reported by name, not skipped silently");
    // 换局清表：同样的输入要重新学（两次编译）
    rp_shader_reset(); rp_S.ubo[1][0].size = sizeof ubo_mem; ubo_mem[92] = 78; compiler_calls = 0;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 2, "session reset forgets learned coordinates (calls=%d)", compiler_calls);
    // 反例：c2 / 组合槽坐标的缺事实不进这张表（不当成用户常量缓冲去读）
    rp_shader_reset(); fake_ubo_missing = 0; fake_gs_success = 0; compiler_calls = 0;
    fake_compile_error = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QuerySamplerType has no source (stage=fs,handle=92,cbufSlot=2)\"}";
    nvn_unimpl_reset();
    CHECK(rp_shader_resolve(&q, &p) == -1 && compiler_calls == 1, "c2 missing fact is not retried as a user UBO handle (calls=%d)", compiler_calls);
    CHECK(nvn_unimpl_count("shader", "compile_failed") == 1, "a rejection nothing can be learned from is still reported as compile_failed (count=%llu)", (unsigned long long)nvn_unimpl_count("shader", "compile_failed"));
    fake_compile_error = NULL; fake_gs_result_mode = 0; separate_fixture = 0; fake_reg_changes = 1;
    memset(&rp_S.ubo[1][0], 0, sizeof rp_S.ubo[1][0]);
    rp_shader_reset(); rp_state_reset();
}

/* The compiler request contains only stages in this graphics program. Inactive
 * UBO state (including compute) must not create new graphics identities. */
static void graphics_active_ubo_test(void) {
    for (unsigned nst = 1; nst <= 3; nst++) {
        rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
        uint8_t keys[3][8]; for (unsigned st = 0; st < nst; st++) gs_source(st, 0, keys[st]);
        memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
        gs_program.serial = 800 + nst; gs_program.nstage = nst;
        memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
        for (unsigned st = 0; st < nst; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
        gs_keys_ok = (1u << nst) - 1;
        p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
        CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "active UBO graphics fixture binds");
        bind_pools(7); compiler_calls = 0;
        struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
        rp_shader_resolve(&q, &p); CHECK(compiler_calls == 1, "initial graphics compiler attempt");
        for (unsigned st = nst; st < 6; st++) {
            rp_S.ubo[st][15].size = 16;
            rp_shader_resolve(&q, &p);
            CHECK(compiler_calls == 1, "inactive stage UBO keeps graphics identity: stages=%u changed=%u calls=%d", nst, st, compiler_calls);
        }
        for (unsigned st = 0; st < nst; st++) {
            int before = compiler_calls; rp_S.ubo[st][15].size = 16;
            rp_shader_resolve(&q, &p);
            CHECK(compiler_calls == before + 1, "active high UBO changes identity: stages=%u changed=%u", nst, st);
            char expected[64]; snprintf(expected, sizeof expected, "\"constantBufferUse\":%u", 1u << 18);
            CHECK(strstr(captured, expected), "active cbuf18 reaches real request");
        }
    }
    rp_shader_reset(); rp_state_reset();
}

// 真实计算解析 → 跨局缓存 → 下一局后台预热发布；不让第二局依赖再次现场编译。
static int compute_warm_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    source(); bind_pools(7); fake_cs_success = 1; compiler_calls = cs_loads = 0;
    const struct rp_prog *first = NULL, *pre = NULL, *again = NULL;
    CHECK(!rp_shader_resolve_compute(&first) && first && first->is_compute && compiler_calls == 1,
          "compute warm first session resolves and compiles");
    if (!first) return 1;
    uint8_t identity[32]; memcpy(identity, first->identity, 32);
    struct nvk_prog_warm_stats st; nvk_prog_warm_stats(&st);
    CHECK(st.recorded == 1, "compute program must be recorded in warm manifest");
    nvk_prog_warm_save();
    CHECK(s2a_program_cache_drain(5000) == 0, "compute cache write drains");
    rp_shader_reset(); compiler_calls = cs_loads = 0;
    CHECK(nvk_prog_warm_load() == 1, "compute program manifest survives session reset");
    struct nvk_prog_rec rec = {0};
    CHECK(nvk_prog_warm_take(&rec) == 1 && rec.stage_mask == 32 && rec.uses == 1 &&
          !memcmp(rec.identity, identity, 32) && !memcmp(rec.keys[0], rp_S.skey[5], 8) && rec.lp_ok == 2,
          "persisted compute manifest retains identity, CS key, use and aligned libpack fact");
    CHECK(nvk_prog_warm_build(&rec) == 0, "compute program warm builds from prior cache");
    CHECK(rp_shader_prog_borrow(rec.identity, &pre) == 1 && pre && pre->is_compute && pre->sh[0],
          "compute warm publishes borrowable CS");
    if (pre) rp_shader_prog_return(rec.identity);
    CHECK(!rp_shader_resolve_compute(&again) && again == pre && compiler_calls == 0 && cs_loads == 1,
          "compute resolve reuses prewarmed program without compiling or reloading");
    rp_shader_reset();
    printf("compute warm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

static int compute_warm_off_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    source(); bind_pools(7); fake_cs_success = 1; compiler_calls = 0;
    CHECK(nvk_prog_warm_load() == 1 && nvk_prog_warm_pending() == 0,
          "compute off retains prior manifest without scheduling CS prewarm");
    const struct rp_prog *p = NULL;
    CHECK(!rp_shader_resolve_compute(&p) && p && compiler_calls == 1, "compute off still resolves");
    struct nvk_prog_warm_stats st; nvk_prog_warm_stats(&st);
    CHECK(st.recorded == 0, "compute warm off must not record programs");
    rp_shader_reset();
    printf("compute warm off: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--compute-warm")) return compute_warm_test();
    if (argc == 2 && !strcmp(argv[1], "--compute-warm-off")) return compute_warm_off_test();
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
    if (argc == 6 && !strcmp(argv[1], "--emit-reuse-texture"))
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], 4);
    if (argc == 6 && !strcmp(argv[1], "--emit-reuse-rt"))
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], 5);
    if (argc == 6 && !strcmp(argv[1], "--emit-reuse-inactive"))
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], 6);
    if (argc == 6 && !strcmp(argv[1], "--execute-reuse"))
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], 3);
    if (argc == 6 && !strcmp(argv[1], "--reject-graphics-tbufu"))
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], 7);
    if (argc == 6 && !strcmp(argv[1], "--reject-compute-tbufu"))
        return emit_compute_request(argv[2], argv[3], argv[4], argv[5], 2);
    if (argc == 6 && (!strcmp(argv[1], "--emit-real") || !strcmp(argv[1], "--emit-real-tbuf") || !strcmp(argv[1], "--execute-tbuf")))
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], !strcmp(argv[1], "--execute-tbuf") ? 2 : !strcmp(argv[1], "--emit-real-tbuf"));
    if (argc == 6 && (!strcmp(argv[1], "--emit-compute-tbuf") || !strcmp(argv[1], "--execute-compute-tbuf")))
        return emit_compute_request(argv[2], argv[3], argv[4], argv[5], !strcmp(argv[1], "--execute-compute-tbuf"));
    if (argc == 2 && !strcmp(argv[1], "--alias-off")) {
        // 结果合并开关关闭（debug.s2a.nvk_alias=0）：alias_on() 进程内只读一次，关闭态单独起一个进程
        fixture_alias_flag = 0;
        graphics_alias_test(0); graphics_alias_fact_test(0); graphics_alias_fact_bg_test(0);
        printf("nvk shader pool (alias off): %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc != 1) return 2;
    graphics_fact_reuse_test();
    graphics_active_ubo_test();
    graphics_alias_test(1);
    graphics_alias_fact_test(1);
    graphics_alias_fact_bg_test(1);
    graphics_ubo_handle_fact_test();
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
    fake_array_count = 0; fake_compile_error = NULL;
    compute_fast_path_test();
    last_table_test();
    gs_request_test();
    rp_shader_reset(); s2a_source_clear();
    fprintf(stderr, "nvk shader pool: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
