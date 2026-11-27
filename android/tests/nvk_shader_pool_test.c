// 新回放着色器入口 + 真实图形/计算请求构造与身份；编译器边界只捕获 JSON。
// 合成来源/设备，不声称运行真实游戏或着色器。
#define _DARWIN_C_SOURCE   // macOS：-std=c11 下 mkdtemp 要它才有声明（Linux 由测试脚本加 _GNU_SOURCE）
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include "nvn.h"
#include "libpack_texture_spirv.h"
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "direct/compile_request.h"
#include "direct/compile_worker.h"
#include "direct/input_identity.h"
#include "direct/source_store.h"
#include "direct/sha256.h"
#include "direct/s2a_direct.h"
#include "../../runtime/include/s2a_direct_host.h"
#include "direct/jv.h"
#include "direct/program_cache.h"
#include "direct/libpack_vk.h"   // s2a_libpack_hits / misses / rejects：状态闸探测不许动库自己的计数
#include "nvn/nvn_vk_prog_warm.h"
#include "dmsh_chain.h"   // DMSH_CHAIN：出包预编库目录 b-<链>-vk 与容器头的链号
#include <sys/stat.h>
#include "tbuf_spirv_fixtures.h"
#include "gfx_plat/spirv_sampler_use.h"
#include "sampler_use_fixtures.h"

// 实现在 nvk_shader.c，声明在 nvk_session.h。这里不包 nvk_session.h：nvk_compile_stop_test.py 拿本文件当夹具，
// 会在同一个翻译单元里把 nvk_session_request_stop 等会话函数定义成 static，包了头就是「static 声明跟在非 static 声明后面」，编不过。
int nvk_shader_open(void);
void nvk_shader_stop(void);
unsigned nvk_shader_inflight(void);
uint64_t nvk_shader_cs_fast_hits(void);
int nvk_shader_last_allocated(void);
void nvk_shader_last_test_put(uint64_t key, const struct rp_prog *prog);
const struct rp_prog *nvk_shader_last_test_find(uint64_t key);

static int checks, failures, compiler_calls, diag_enabled, diag_lines, diag_queries;
static int trace_on, trace_begin_count, trace_end_count;
static uint32_t trace_stage; static int trace_submit, trace_warm, trace_success; static uint8_t trace_id[32];
int s2a_direct_diag_enabled(void) { return trace_on; }
uint64_t s2a_direct_diag_compile_begin(const uint8_t identity[32], unsigned stages, int submit, int warm) {
    trace_begin_count++; memcpy(trace_id, identity, 32); trace_stage=stages; trace_submit=submit; trace_warm=warm; return 1;
}
void s2a_direct_diag_compile_end(uint64_t token, int success) { if (token) trace_end_count++; trace_success=success; }
static unsigned fake_array_count;
static int separate_fixture;
static int record_link_refused;
static int link_map_flag;   // nvk_link_map_diag 开关替身
static int fake_gs_success, fake_gs_result_mode, gs_loads[6], fake_geometry_enabled = 1, fake_depth_clip;
static size_t gs_blob_bytes[6];   // 每个阶段最后一次装载的 SPIR-V 字节数（出包预编库命中时认出是哪一份）
static int fake_gs_fact_deps;
// 与 fake_gs_fact_deps 一起用：结果的依赖清单末尾再多一条（真编译器的形状）：1 = 请求开关 requiresLinkedVaryingInterface；
//   2 = 句柄 72 的查询、来源 unbound（没有这条事实）；3 = 句柄 72 的查询、来源 texture-fact:fs:72:-1（找到了）；4 = 不认得的 request:* 来源
static int fake_gs_dep_extra;
static int fixture_fact_wide = -1;    // 事实复用放宽开关（debug.s2a.nvk_fact_wide）：-1 = 取生产缺省（NVK_FACT_WIDE_DEFAULT），1 = 开，0 = 关
static int fixture_alias_flag = -1;   // 结果合并开关（debug.s2a.nvk_alias）：-1 = 取生产缺省，0 = 关
static int fake_gs_result_tag;        // 非 0：结果里多带一个无关字段，同一份输入的结果字节（摘要）就不同
static int fixture_depth_zero, fixture_origin_lower_left, shader_load_count, shader_release_count;
static int fixture_gate_probe;   // nvk_gate_probe：只读探测开关（缺省 0）
static int fixture_used_slots = -1;   // nvk_lp_used_slots：-1 = 取生产缺省（关 = 「任何一个非对齐绑定都算」的旧口径），0 / 1 = 读成这个值（缺省关之后，新口径的用例要显式设 1，就像调试包设属性）
static int fake_cs_success, cs_loads;
int g_s2a_logfd = 2;
static const char *fake_compile_error;
static const char *fake_compile_file;
static pthread_t last_compiler_thread;
static int fixture_live_compile, fixture_live_slots;
static int fixture_submit_compile, fixture_submit_slots;
static pthread_t last_shader_load_thread;
static uint64_t fixture_compile_generation = 6;
static int fixture_live_frame_ms = 100, fixture_live_unsafe_ms = 10000;
static pthread_mutex_t live_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t live_gate_cv = PTHREAD_COND_INITIALIZER;
static int live_gate_hold, live_gate_entered;
static void live_gate_pause(void) {
    pthread_mutex_lock(&live_gate_mu);
    if (live_gate_hold) {
        live_gate_entered++;
        pthread_cond_broadcast(&live_gate_cv);
        while (live_gate_hold) pthread_cond_wait(&live_gate_cv, &live_gate_mu);
    }
    pthread_mutex_unlock(&live_gate_mu);
}
static int live_gate_wait(int count) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&live_gate_mu);
    while (live_gate_entered < count)
        if (pthread_cond_timedwait(&live_gate_cv, &live_gate_mu, &deadline)) break;
    int entered = live_gate_entered >= count;
    pthread_mutex_unlock(&live_gate_mu);
    return entered;
}
static void live_gate_release(void) {
    pthread_mutex_lock(&live_gate_mu);
    live_gate_hold = 0;
    pthread_cond_broadcast(&live_gate_cv);
    pthread_mutex_unlock(&live_gate_mu);
}
static char captured[262144], last_red[512], last_diag[512], last_stats[512], last_reason[512], last_request_reason[512];
static int ubo_default_lines; static char last_ubo_default_line[512];   // 常量缓冲句柄指向没登记的贴图：每局首次的那一行（片 4）
static char reason_history[8192];
static char reject_lines[65536], last_trunc[512];   // #999：编译被拒的上下文日志（程序键 / 图像槽原因）；last_trunc：超预算时「已截断，共 N 条」那一行
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
static int fixture_warm_nowindow, fixture_warm_capture, fixture_no_libpack;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "no_libpack") && fixture_no_libpack) return 1;
    if (!strcmp(name, "nvk_warm_capture")) return fixture_warm_capture;
    if (!strcmp(name, "nvk_warm_nowindow")) return fixture_warm_nowindow;
    if (!strcmp(name, "nvk_submit_compile")) return fixture_submit_compile;
    if (!strcmp(name, "nvk_submit_compile_slots") && fixture_submit_slots) return fixture_submit_slots;
    if (!strcmp(name, "nvk_live_compile_async") && fixture_live_compile >= 0) return fixture_live_compile;
    if (!strcmp(name, "nvk_live_compile_slots") && fixture_live_slots) return fixture_live_slots;
    if (!strcmp(name, "fu_frame_ms")) return fixture_live_frame_ms;
    if (!strcmp(name, "fu_unsafe_ms")) return fixture_live_unsafe_ms;
    if (!strcmp(name, "nvk_compute_warm")) return getenv("S2A_COMPUTE_WARM") ? atoi(getenv("S2A_COMPUTE_WARM")) : dflt;
    if (!strcmp(name, "nvk_texpool_log")) { diag_queries++; return diag_enabled; }
    if (fixture_alias_flag >= 0 && !strcmp(name, "nvk_alias")) return fixture_alias_flag;
    if (!strcmp(name, "nvk_link_refused_file") && record_link_refused) return 1;
    if (!strcmp(name, "nvk_link_map_diag")) return link_map_flag;
    if (!strcmp(name, "nvk_gate_probe")) return fixture_gate_probe;
    if (fixture_used_slots >= 0 && !strcmp(name, "nvk_lp_used_slots")) return fixture_used_slots;
    if (fixture_fact_wide >= 0 && !strcmp(name, "nvk_fact_wide")) return fixture_fact_wide;
    return dflt;
}
static int link_map_lines; static char last_link_map[700];   // 配对接口表诊断行（nvk_link_map_diag）
static int lp_missing_lines, lp_open_lines, lp_unref_lines;   // 出包预编库「不在」（黄）/「已打开」/计算对齐闸拒查 三种行各打了几次
static char last_unref[256], last_unref_sum[256]; static int lp_unref_sums;   // 计算对齐闸拒查的「累计」汇总行（每 64 次一行）
void nvn_plat_log(int level, const char *cls, const char *text) {
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "配对接口表 ")) { link_map_lines++; snprintf(last_link_map, sizeof last_link_map, "%s", text); }
    if (!strcmp(cls, "shader") && strstr(text, "出包预编库不在：")) lp_missing_lines += level == NVN_PLAT_LOG_WARN ? 1 : 1000;   // 不是黄就记成大数（必红）
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "出包预编库已打开：")) lp_open_lines++;
    if (!strcmp(cls, "shader") && strstr(text, "libpack_cs_unaligned_refused")) { lp_unref_lines += level == NVN_PLAT_LOG_INFO ? 1 : 1000; snprintf(last_unref, sizeof last_unref, "%s", text); }
    if (!strcmp(cls, "shader") && strstr(text, "libpack_cs_unaligned_refused 累计")) { lp_unref_sums++; snprintf(last_unref_sum, sizeof last_unref_sum, "%s", text); }
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
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "常量缓冲句柄指向没登记的贴图")) {
        ubo_default_lines++; snprintf(last_ubo_default_line, sizeof last_ubo_default_line, "%s", text);
    }
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && (strstr(text, "texture_pool_reject_program") || strstr(text, "image_fact_reject"))) {
        size_t n = strlen(reject_lines);
        snprintf(reject_lines + n, sizeof reject_lines - n, "%s\n", text);
    }
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "shader") && strstr(text, "texture_pool_reject_lines_truncated"))
        snprintf(last_trunc, sizeof last_trunc, "%s", text);
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
static uint32_t fixture_ssbo_align;   // 设备 minStorageBufferOffsetAlignment（非窗口补件夹具要非对齐的存储缓冲基址，请求才带 hasUnalignedStorageBuffer）
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); out->min_ssbo_align = fixture_ssbo_align; out->caps = (fake_geometry_enabled ? NVN_GPU_CAP_GEOMETRY : 0) | (fake_depth_clip ? NVN_GPU_CAP_DEPTH_CLIP_CTL : 0); }
static uint32_t sampled_fixture_format;
static int fixture_alias_texture;
static tex_t fixture_alias_snapshot;
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) {
    if (fixture_alias_texture && id == 0) {
        if (t) *t = fixture_alias_snapshot;
        if (v) memset(v, 0, sizeof *v);
        if (h) *h = 0;
        return 1;
    }

    // 77：带视图（改目标为 RECTANGLE）；78 / 79：同一组格式 / 目标 / 深度的两个不同句柄（快路按事实认，不按句柄认）。
    if (!separate_fixture || (id != 77 && id != 78 && id != 79 && !(separate_fixture == 2 && id == 0))) return 0;
    memset(t, 0, sizeof *t); memset(v, 0, sizeof *v); t->d.format = NVN_FORMAT_RGBA8; t->d.target = NVN_TEXTURE_TARGET_2D; t->d.depth = 1;
    if (sampled_fixture_format && id == 78) t->d.format = sampled_fixture_format;
    if (separate_fixture == 3) t->d.target = NVN_TEXTURE_TARGET_BUFFER;
    if (separate_fixture == 4) t->d.format = NVN_FORMAT_RGBA8_UI2F;   // 登记了，但 UI2F 不出编译事实
    if (separate_fixture == 5 && id != 77) { t->d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t->d.depth = id == 78 ? 2 : 8; }   // 78 / 79：二维数组贴图，层数 2 / 8（事实里的 array_length）
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
static int fake_cs_ubo_missing;   // 1：计算请求里没有 (cs, 40, c6) 的事实就照真机那条报 MISSING_FACT（#1594 片 9）
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
    memset(c, 0, sizeof *c); c->session = 4; c->generation = fixture_compile_generation; c->direct_abi = 1;
    c->profile_json = external_profile[0] ? external_profile : profile; c->profile_bytes = strlen(c->profile_json); c->profile_hash = external_profile_hash[0] ? external_profile_hash : phash;
    c->translator_closure = external_closure[0] ? external_closure : "closure-test"; c->storage_buffer_offset_alignment = 16; return 0;
}
static int32_t fake_compile(uint64_t session, uint64_t gen, s2a_direct_bytes req, s2a_direct_emit_fn emit, void *sink) {
    last_compiler_thread = pthread_self();
    CHECK(session == 4 && (gen == 6 || (fixture_submit_compile && gen == 7)), "compile session");
    CHECK(req.bytes < sizeof captured, "request fits capture");
    if (req.bytes >= sizeof captured) return -1;
    memcpy(captured, req.data, (size_t)req.bytes); captured[req.bytes] = 0; compiler_calls++;
    live_gate_pause();
    if (fake_ubo_missing && !strstr(captured, "\"stage\":\"fs\",\"handle\":92,\"cbufSlot\":3,")) {
        static const char err[] = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QuerySamplerType has no source (stage=fs,handle=92,cbufSlot=3)\"}";
        s2a_direct_bytes payload = {(const uint8_t *)err, sizeof err - 1};
        (void)emit(sink, payload); return -1;
    }
    if (fake_cs_ubo_missing && strstr(captured, "\"program\":{\"kind\":\"compute\"") && !strstr(captured, "\"stage\":\"cs\",\"handle\":40,\"cbufSlot\":6,")) {
        // 真机原文（0100559011740000 标题 3D 的计算着色器）：QueryS2ASampledTextureFormat has no source (stage=cs,handle=40,cbufSlot=6)
        static const char err[] = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryS2ASampledTextureFormat has no source (stage=cs,handle=40,cbufSlot=6)\"}";
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
    if (fake_cs_success > 1) {
        const char *code = fake_cs_success == 2 ? S2A_SAMPLER_QUERY_SPV : S2A_SAMPLER_SAMPLE_SPV;
        // 4：整池数组（v2 结果：贴图池 8448 项 + 采样器池 1280 项，#1594 片 8）；5：同样 8448 项但不带 pool 的普通数组（计算侧仍不接，ARRAY_UNSUPPORTED）；
        // 6：带 pool 但池长只有 1（形状不对，整份拒，走 compute_program_parse_failed 而不是 ARRAY_UNSUPPORTED）
        // 7：贴图字在游戏常量缓冲里的常量采样器（uboSlot 2 / uboOffset 160 / 池下标 260，#1594 片 10）；
        // 8：同一个计算程序三项（两个 UBO 形态 + 一个 c2 形态 slot 100 / 池下标 259）；9：UBO 形态但没有 constSampler（计算侧不接，整份拒）
        const char *bindings = fake_cs_success == 4 ?
            "[{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"count\":8448,\"pool\":\"texture\"},"
            "{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":1,\"count\":1280,\"pool\":\"sampler\"}]" :
            fake_cs_success == 5 ?
            "[{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"slot\":8,\"count\":8448}]" :
            fake_cs_success == 6 ?
            "[{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"pool\":\"texture\"}]" :
            fake_cs_success == 7 ?
            "[{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":260}]" :
            fake_cs_success == 8 ?
            "[{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":260},"
            "{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":1,\"count\":1,\"uboSlot\":3,\"uboOffset\":164,\"constSampler\":262},"
            "{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":2,\"slot\":100,\"count\":1,\"constSampler\":259}]" :
            fake_cs_success == 9 ?
            "[{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160}]" :
            "[{\"stage\":\"cs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,\"count\":1,\"rw\":true,\"cbSlot\":0,\"cbOffset\":784,\"nvnSlot\":0},"
            "{\"stage\":\"cs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"slot\":72,\"count\":1}]";
        const int ver = fake_cs_success >= 4 ? 2 : 1;   // 4..9 是 v2 结果（pool / constSampler 字段只在 v2 里有合同）
        char result[8192];
        int n = snprintf(result, sizeof result, "{\"schema\":\"s2a-direct-program-v%d\",\"content\":{\"contract\":\"s2a-direct-program-content-v%d\",\"kind\":\"compute\",\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],\"supportReads\":[],\"supportLayout\":{\"requiredSize\":16},\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\"}],\"bindings\":%s}}", ver, ver, code, bindings);
        CHECK(n > 0 && (size_t)n < sizeof result, "compute result fixture fits");
        s2a_direct_bytes payload = {(const uint8_t *)result, (uint64_t)n};
        return emit(sink, payload);
    }
    if (fake_gs_success) {
        char result[2048];
        const char *extra = fake_gs_result_mode == 1 ? "" :
            fake_gs_result_mode == 2 ? ",{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"},{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}" :
            fake_gs_result_mode == 3 ? ",{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"},{\"stage\":\"tcs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}" :
            ",{\"stage\":\"gs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}";
        char depbuf[1024] = "";
        const char *source = fake_gs_fact_deps == 2 ? "texture-fact:zz:8:-1" :
            fake_gs_fact_deps == 3 ? "texture-fact:vs:71:-1" :
            fake_gs_fact_deps == 4 ? "texture-fact:vs:8:99" :
            fake_gs_fact_deps == 5 ? "future-texture-state" : "texture-fact:vs:8:-1";
        const char *extra_dep = fake_gs_dep_extra == 1 ? ",{\"source\":\"request:requiresLinkedVaryingInterface\",\"method\":\"QueryHostRequiresLinkedVaryingInterface\",\"value\":\"true\"}" :
            fake_gs_dep_extra == 2 ? ",{\"source\":\"unbound\",\"method\":\"QueryS2ASampledTextureFormat\",\"arguments\":\"handle=72,cbufSlot=-1\"}" :
            fake_gs_dep_extra == 3 ? ",{\"source\":\"texture-fact:fs:72:-1\",\"method\":\"QueryS2ASampledTextureFormat\",\"arguments\":\"handle=72,cbufSlot=-1\"}" :
            fake_gs_dep_extra == 4 ? ",{\"source\":\"request:futureFlag\",\"method\":\"QueryHostFutureFlag\"}" : "";
        if (fake_gs_fact_deps) snprintf(depbuf, sizeof depbuf,
            ",\"contract\":\"s2a-direct-program-content-v1\",\"dependencies\":[{\"source\":\"code:sha256:synthetic\",\"method\":\"GetCode\"},{\"source\":\"%s\",\"method\":\"QuerySamplerType\"},{\"source\":\"texture-fact:fs:10:2\",\"method\":\"QueryTextureCoordNormalized\"},{\"source\":\"code:sha256:synthetic#cbUse\",\"method\":\"QueryConstantBufferUse\",\"arguments\":\"stage=vs\"}%s]", source, extra_dep);
        if (fake_gs_result_tag) { size_t dn = strlen(depbuf); snprintf(depbuf + dn, sizeof depbuf - dn, ",\"fixtureTag\":%d", fake_gs_result_tag); }
        if (fixture_warm_capture) {
            size_t used = strlen(depbuf);
            snprintf(depbuf + used, sizeof depbuf - used,
                     ",\"identity\":{\"directABI\":1,\"translatorClosure\":\"closure-test\",\"profileHash\":\"%s\"}", phash);
        }
        const char *deps = depbuf;
        // 请求带 linkMapDiag 时照编译器口径在信封外层附 linkMap（VS 写逻辑 0/3/7、FS 读 3/7 → 压缩后宿主 0/1/2 与 1/2）
        const char *link_map = strstr(captured, "\"linkMapDiag\":true") ? ",\"linkMap\":{\"vs\":[[0,0],[3,1],[7,2]],\"fs\":[[3,1],[7,2]]}" : "";
        int n = snprintf(result, sizeof result, "{\"schema\":\"s2a-direct-program-v1\",\"content\":{\"stages\":[{\"stage\":\"vs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"},{\"stage\":\"fs\",\"spirv\":\"AwIjBwABAwAAAAAAAQAAAAAAAAA=\"}%s],\"bindings\":[]%s}%s}", extra, deps, link_map);
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
static int32_t fake_parallel_compile(uint64_t session, uint64_t gen, s2a_direct_bytes req,
                                     s2a_direct_emit_fn emit, void *sink) {
    (void)session; (void)gen; (void)req;
    live_gate_pause();
    static const char result[] =
        "{\"schema\":\"s2a-direct-program-v1\",\"content\":{\"stages\":[{\"stage\":\"vs\",\"spirv\":\"" S2A_TEST_VERT_64 "\"}],"
        "\"bindings\":[{\"stage\":\"vs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":0,\"count\":64}]}}";
    s2a_direct_bytes payload = {(const uint8_t *)result, sizeof result - 1};
    return emit(sink, payload);
}
static int fixture_parallel_compile;
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return fixture_parallel_compile ? fake_parallel_compile : fake_compile; }
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *o, nvn_gpu_err *e) {
    (void)e;
    if (fake_gs_success && (unsigned)s->stage < 6) {
        shader_load_count++; gs_loads[s->stage]++; gs_blob_bytes[s->stage] = s->bytes; *o = (nvn_gpu_shader)(uintptr_t)(s->stage + 1); return (nvn_gpu_rc){NVN_GPU_OK};
    }
    if (fake_array_count) {
        pthread_mutex_lock(&live_gate_mu);
        last_shader_load_thread = pthread_self();
        shader_load_count++;
        pthread_cond_broadcast(&live_gate_cv);
        pthread_mutex_unlock(&live_gate_mu);
        *o = (nvn_gpu_shader)(uintptr_t)1;
        return (nvn_gpu_rc){NVN_GPU_OK};
    }
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
static unsigned gs_c1_len = 0x100;   // c1 段长度（c1fill 非 0 时）：缺省 0x100；长 c1 用例临时改大（段落在控制段 0x200 起，最长 0x500）
static void gs_source_ex(unsigned st, unsigned variant, unsigned c1fill, uint8_t key[8]) {
    uint8_t ctl[0x760] = {0}, code[0x70] = {0}, digest[32];
    const unsigned type[3] = {1, 5, 4};
    wr(code, (st == 1 ? 2u : 1u) | (3u << 5) | (type[st] << 10));
    if (st == 1) wr(code + 72, 15);
    if (st == 2) { wr(code + 8, 1u << 24); wr(code + 12, 7u << 24); wr(code + 16, 3); }
    wr64(code + 0x50, 0x001f8000fc0007e0ull); wr64(code + 0x58, 0xe30000000007000full);
    wr64(code + 0x60, 0xe30000000007000full); wr64(code + 0x68, 0xe30000000007000full);
    wr(ctl + 0x20, sizeof ctl); ctl[0x100] = (uint8_t)variant;
    if (c1fill) { wr(ctl + 0x700, sizeof code + 0x200); wr(ctl + 0x704, sizeof code + 0x200 + gs_c1_len); memset(ctl + 0x200, (int)c1fill, gs_c1_len); }
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
    // Run the same captured program as a VS+FS pair first. This tests the
    // production new-layer producer, not just the shared JSON builder.
    uint64_t gs_serial = rp_S.bser[2]; rp_S.bser[2] = 0;
    CHECK(rp_shader_graphics_stages() == 3, "paired fixture stage mask is 0x3");
    (void)rp_shader_resolve(&q, &p);
    CHECK(compiler_calls == 1 && strstr(captured, "\"requiresLinkedVaryingInterface\":true"),
          "new-layer paired request opts in");
    rp_S.bser[2] = gs_serial; captured[0] = 0; compiler_calls = 0; p = NULL; trace_begin_count = trace_end_count = 0;
    int rc = rp_shader_resolve(&q, &p);
    CHECK(rc == -1 && p == NULL, "GS compiler boundary deliberately fails closed");
    CHECK(compiler_calls == 1, "GS production request reaches compiler, not stages_0x7_not_vs_fs");
    CHECK(!strstr(captured, "\"requiresLinkedVaryingInterface\""), "GS production request omits opt-in");
    if (trace_on) CHECK(trace_begin_count == 1 && trace_end_count == 1 && trace_stage == 7 && trace_submit == 1 && trace_warm == 0 && trace_success == 0, "graphics live compile emits paired event including failure and stage");
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
    int trace_before_hit = trace_begin_count;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == calls, "identical GS request hits cache");
    if (trace_on) CHECK(trace_begin_count == trace_before_hit, "cached graphics resolve does not create compile event");
    memcpy(gs_keys[0], changed, 8);
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "rebind changed GS source");
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls, "changed GS key recompiles");
    rp_S.tex[2][0] = 99;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls, "changed GS texture fact recompiles");
    bind_pools(100); CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == ++calls, "GS pool size update recompiles");
    separate_fixture = 1; fake_reg_changes = 0; bind_separate(2, 0, 1, 77);
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
static int fact_matches_at(const char *stage, int handle, int cb, const char *sampler, int normalized) {
    char key[96], norm[32], type[96];
    snprintf(key, sizeof key, "\"stage\":\"%s\",\"handle\":%d,\"cbufSlot\":%d", stage, handle, cb);
    snprintf(norm, sizeof norm, "\"normalized\":%s", normalized ? "true" : "false");
    snprintf(type, sizeof type, "\"samplerType\":\"%s\"", sampler);
    const char *p = strstr(captured, key), *end = p ? strchr(p, '}') : NULL;
    const char *n = p ? strstr(p, norm) : NULL, *t = p ? strstr(p, type) : NULL;
    return end && n && n < end && t && t < end;
}
static int fact_matches(const char *stage, int handle, const char *sampler, int normalized) {
    return fact_matches_at(stage, handle, 2, sampler, normalized);
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
// 计算阶段组合槽里没登记的号 → 默认事实（#1594 片 4 把三处各写一遍的默认事实收成 texture_facts.c 的一个函数，这里钉住计算这一处）：
//   池容量 100，号 50 池内没登记 = {1d, 不归一化}，号 0xFFFFF 池外 = {2d, 归一化}；已登记的 78（2D RGBA8）照旧取登记。
//   槽 3 的事实句柄 = 8 + 2 × 3 = 14（cbufSlot −1，同图形组合槽）。
static void compute_default_fact_test(void) {
    const struct rp_prog *a = NULL; uint8_t dig2[32];
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset(); source(); cs_variant_source(1, dig2);
    fake_cs_success = 1; separate_fixture = 1; fake_reg_changes = 1; compiler_calls = cs_loads = 0; captured[0] = last_red[0] = 0;
    bind_pools(100);
    rp_S.ubo[5][0].addr = 0x1000; rp_S.ubo[5][0].size = 64; rp_S.ssbo[5][0].addr = 0x10000; rp_S.ssbo[5][0].size = 256;
    rp_S.tex[5][3] = 50;
    CHECK(rp_shader_resolve_compute(&a) == 0 && a && compiler_calls == 1 && fact_matches_at("cs", 14, -1, "1d", 0),
          "CS slot naming an in-pool unregistered id carries the default {1d, unnormalized} (calls=%d)", compiler_calls);
    rp_S.tex[5][3] = 0xFFFFF;
    CHECK(rp_shader_resolve_compute(&a) == 0 && a && compiler_calls == 2 && fact_matches_at("cs", 14, -1, "2d", 1),
          "CS slot naming an out-of-pool id carries the default {2d, normalized} (calls=%d)", compiler_calls);
    rp_S.tex[5][3] = 78;
    CHECK(rp_shader_resolve_compute(&a) == 0 && a && compiler_calls == 3 && fact_matches_at("cs", 14, -1, "2d", 1) && !fact_matches_at("cs", 14, -1, "1d", 0),
          "CS slot naming a registered 2D texture takes the registered fact, not the default (calls=%d)", compiler_calls);
    fake_cs_success = 0; separate_fixture = 0; fake_reg_changes = 1;
    memset(rp_S.ubo[5], 0, sizeof rp_S.ubo[5]); memset(rp_S.ssbo[5], 0, sizeof rp_S.ssbo[5]); memset(rp_S.tex[5], 0, sizeof rp_S.tex[5]);
    rp_shader_reset();
}
// 句柄在计算阶段用户常量缓冲里的贴图（#1594 片 9，图形那条是片 4 的 graphics_ubo_handle_fact_test）：
//   编译器按 (cs, 字 40, c6) 报缺事实 → 宿主读 c6（用户槽 3 = rp_S.ubo[5][3]）第 40 字 → 贴图号 → 补事实当场重编。
//   之前计算阶段不认这条（ubo_need_learn 只认 vs / fs / gs）：同一份着色器每次派发都被拒，整笔派发被 compute_program_unresolved 跳掉。
static int cs_ubo_fact_in_request(const char *sampler, int normalized) {
    return fact_matches_at("cs", 40, 6, sampler, normalized);
}
static void compute_ubo_handle_fact_test(void) {
    const struct rp_prog *a = NULL, *b = NULL;
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset(); source();
    fake_cs_success = 1; separate_fixture = 1; fake_reg_changes = 0; fake_cs_ubo_missing = 1; fake_compile_error = NULL;
    compiler_calls = cs_loads = 0; captured[0] = last_red[0] = 0; reject_lines[0] = 0;
    bind_pools(100);
    memset(ubo_mem, 0, sizeof ubo_mem); ubo_mem[40] = 78;   // 池起点 0：句柄低 20 位就是贴图号；78 登记为 2D RGBA8
    rp_S.ubo[5][3].addr = (uint64_t)(uintptr_t)ubo_mem; rp_S.ubo[5][3].size = sizeof ubo_mem;
    CHECK(rp_shader_resolve_compute(&a) == 0 && a && a->is_compute && compiler_calls == 2 && cs_loads == 1,
          "missing cs c6 handle fact is learned and compiled in the same dispatch: calls=%d loads=%d red=%s", compiler_calls, cs_loads, last_red);
    CHECK(!reject_lines[0], "a cs rejection that only taught a coordinate prints no program-key line (it recompiles at once): %s", reject_lines);
    CHECK(nvn_unimpl_count("shader", "compute_compile_failed") == 0,
          "a rejection that taught a coordinate is a learning step, not an unimplemented compute_compile_failed (count=%llu)", (unsigned long long)nvn_unimpl_count("shader", "compute_compile_failed"));
    CHECK(cs_ubo_fact_in_request("2d", 1), "retry request carries the (cs,40,c6) fact of texture 78");
    // 之后同一份着色器：句柄指向同一张贴图 → 不再编（第一次是学习那一笔留下的旧键，第二次起走快路）
    const int calls = compiler_calls;
    CHECK(rp_shader_resolve_compute(&b) == 0 && b == a && compiler_calls == calls, "same UBO texture reuses the program (calls=%d)", compiler_calls - calls);
    const uint64_t hits = nvk_shader_cs_fast_hits();
    CHECK(rp_shader_resolve_compute(&b) == 0 && b == a && compiler_calls == calls && nvk_shader_cs_fast_hits() == hits + 1, "same UBO texture takes the fast path");
    ubo_mem[40] = 77;   // 77 带 RECTANGLE 视图：坐标不归一，编译事实不同 → 键跟着变，重编
    CHECK(rp_shader_resolve_compute(&b) == 0 && b && b != a && compiler_calls == calls + 1, "UBO now names a texture with other facts: recompiles (calls=%d)", compiler_calls - calls);
    CHECK(cs_ubo_fact_in_request("2d", 0), "recompile carries texture 77's unnormalized fact");
    ubo_mem[40] = 79;   // 79 与 78 事实相同：回到第一份程序，不重编（按事实认，不按句柄认）
    CHECK(rp_shader_resolve_compute(&b) == 0 && b == a && compiler_calls == calls + 1, "same facts as 78 reuse the first program (calls=%d)", compiler_calls - calls);
    // 反例：句柄字读不到（槽长度盖不住第 40 字）→ 不给事实：请求与第一次缺事实那份逐字节相同，命中那份失败项，不编、不猜、不循环
    rp_S.ubo[5][3].size = 40 * 4;
    const int before = compiler_calls; captured[0] = 0; nvn_unimpl_reset();
    CHECK(rp_shader_resolve_compute(&b) == -1 && compiler_calls == before && !captured[0], "unreadable cs handle word reuses the fact-less failure (calls=%d)", compiler_calls - before);
    CHECK(nvn_unimpl_count("shader", "ubo_handle_unreadable") >= 1, "unreadable cs handle word is reported by name, not skipped silently");
    // 换局清表：同样的输入要重新学（两次编译）
    rp_shader_reset(); rp_S.ubo[5][3].size = sizeof ubo_mem; ubo_mem[40] = 78; compiler_calls = 0;
    CHECK(rp_shader_resolve_compute(&b) == 0 && b && compiler_calls == 2, "session reset forgets learned cs coordinates (calls=%d)", compiler_calls);
    // 反例：编译器报的不是用户常量缓冲坐标（c2，cbufSlot 2）→ 不学、不重试，照旧 compute_compile_failed
    rp_shader_reset(); fake_cs_ubo_missing = 0; fake_cs_success = 0; compiler_calls = 0; nvn_unimpl_reset(); reject_lines[0] = 0;
    fake_compile_error = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QuerySamplerType has no source (stage=cs,handle=40,cbufSlot=2)\"}";
    CHECK(rp_shader_resolve_compute(&b) == -1 && !b && compiler_calls == 1 && nvn_unimpl_count("shader", "compute_compile_failed") == 1,
          "a cs c2 missing fact is not retried as a user UBO handle and stays compute_compile_failed (calls=%d count=%llu)", compiler_calls, (unsigned long long)nvn_unimpl_count("shader", "compute_compile_failed"));
    // 反例：编译器按另一个阶段（fs）报缺 c6 坐标，计算请求不学它（图形阶段的坐标不进计算着色器的键名下）
    rp_shader_reset(); compiler_calls = 0; nvn_unimpl_reset(); reject_lines[0] = 0;
    fake_compile_error = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QuerySamplerType has no source (stage=fs,handle=40,cbufSlot=6)\"}";
    CHECK(rp_shader_resolve_compute(&b) == -1 && !b && compiler_calls == 1 && nvn_unimpl_count("shader", "compute_compile_failed") == 1,
          "a missing fact for another stage is not learned by a compute dispatch (calls=%d)", compiler_calls);
    fake_compile_error = NULL; fake_cs_success = 0; separate_fixture = 0; fake_reg_changes = 1; fake_cs_ubo_missing = 0;
    memset(rp_S.ubo[5], 0, sizeof rp_S.ubo[5]); memset(rp_S.ssbo[5], 0, sizeof rp_S.ssbo[5]); memset(rp_S.tex[5], 0, sizeof rp_S.tex[5]);
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
// 普通 VS/FS 冷局：先交出真实前台请求，再把宿主编译器的原样结果送回同一入口。
static int emit_nowindow_pair, execute_nowindow_pair;
static const char *arms_root, *arms_manifest;   // --nowindow-arms：种子目录树根 / 程序清单目录（非空 = 执行完后接着跑补件各臂）
static void test_nowindow_seed(const char *root, const char *manifest, const struct rp_shader_req *q);

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
    unsigned stages = emit_nowindow_pair || execute_nowindow_pair ? 2 : 3;
    uint8_t keys[3][8];
    for (unsigned st = 0; st < stages; st++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s.bin", dir, (const char *const[]){"vs","fs","gs"}[st]);
        FILE *f = fopen(path, "rb"); if (!f) return 1;
        uint8_t code[4096], ctl[0x760] = {0}, digest[32]; size_t n = fread(code, 1, sizeof code, f);
        int beyond = fgetc(f); fclose(f); if (!n || beyond != EOF) return 1;
        wr(ctl + 0x20, sizeof ctl); s2a_sha256 h; s2a_sha256_init(&h);
        s2a_sha256_update(&h, ctl, sizeof ctl); s2a_sha256_update(&h, code, n); s2a_sha256_final(&h, digest);
        if (s2a_source_keep(digest, st, code, (uint32_t)n, ctl, sizeof ctl)) return 1;
        memcpy(keys[st], digest, 8);
    }
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G'); gs_program.serial = 101; gs_program.nstage = stages;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < stages; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = (uint8_t)((1u << stages) - 1); p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    if (!rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8)) return 1;
    bind_pools(tbuf == 1 || tbuf == 2 || tbuf == 7 || tbuf >= 8 ? 100 : 7);
    if (tbuf >= 8) { separate_fixture = 1; rp_S.img[1][0] = 78; }
    if (tbuf == 1 || tbuf == 2 || tbuf == 7) { separate_fixture = 3; bind_separate(1, 0, 1, 77); }
    if (stages == 2) {   // 普通 VS/FS：存储缓冲基址取非对齐的，请求带 hasUnalignedStorageBuffer
        fixture_ssbo_align = 16;
        fixture_warm_capture = execute_nowindow_pair;
        rp_S.ssbo[0][0].addr = 0x10004;
        rp_S.ssbo[0][0].size = 64;
        if (execute_nowindow_pair && nvk_prog_warm_load() != 0) return 1;
    }
    if (execute_nowindow_pair || tbuf == 2 || tbuf == 3 || tbuf == 7 || tbuf == 9) { fake_compile_file = output; fake_gs_success = 1; nvn_unimpl_reset(); memset(gs_loads, 0, sizeof gs_loads); }
    else capture_output = output;
    if (tbuf == 4) rp_S.tex[0][0] = 99;
    if (tbuf == 6) rp_S.ubo[5][15].size = 16;
    compiler_calls = 0;
    struct rp_shader_req q = {0}; q.prim = 0; q.rt_count = 1; q.rt_format[0] = NVN_FORMAT_RGBA8;
    if (tbuf == 5) { q.rt_count=2; q.rt_format[1]=NVN_FORMAT_RGBA8; }
    const struct rp_prog *prog = NULL; int rc = rp_shader_resolve(&q, &prog);
    if (execute_nowindow_pair) {
        if (rc || !prog || compiler_calls != 1 || prog->lib_sourced) return 1;
        nvk_prog_warm_save();
        if (arms_root) {
            CHECK(s2a_program_cache_drain(5000) == 0, "ordinary draw manifest drains");
            test_nowindow_seed(arms_root, arms_manifest, &q);
        }
        return failures != 0;
    }
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
    if (tbuf == 9) {
        int found = 0;
        if (prog) for (uint32_t i = 0; i < prog->nent[1]; i++)
            if (prog->ent[1][i].src_kind == NVN_RB_TEX && prog->ent[1][i].src_reg == 32 &&
                prog->ent[1][i].src_reg2 == UINT16_MAX) found++;
        int accepted = !rc && prog && compiler_calls == 1 && gs_loads[1] == 1 && found == 1;
        fprintf(stderr, "%s: real c2 word 72 result loads FS with combined slot 32\n", accepted ? "PASS" : "FAIL");
        return !accepted;
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

// 组合槽里的二维数组贴图：事实的 array_length 是贴图层数（texture_facts.c LAYERS_COUNT），不是「槽是数组」。
// 编译器没查过的槽换一张层数不同的数组贴图，复用同一份程序、不再编；查过的槽整条事实（含层数）仍在键里，变了照常重编。
static void graphics_fact_array_length_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    uint8_t keys[3][8]; for (unsigned st = 0; st < 3; st++) gs_source(st, 0, keys[st]);
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = 900; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < 3; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = 7; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "array length program binds");
    bind_pools(100); fake_gs_success = fake_gs_fact_deps = 1; separate_fixture = 5; fake_reg_changes = 0;
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    rp_S.tex[0][2] = 78;   // 没查过的槽里常驻一张 2 层的数组贴图
    compiler_calls = 0; CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "array length initial real producer resolves");
    const struct rp_prog *first = p;
    rp_S.tex[0][2] = 79;   // 同一槽换成 8 层：身份只差没查过的槽的 array_length
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "unqueried 2d-array layer count reuses compiled program");
    rp_S.tex[1][5] = 78;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "other stage unqueried 2d-array texture reuses program");
    // 负对照：查过的槽（依赖里的 texture-fact:vs:8:-1）
    rp_S.tex[0][0] = 78;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "queried slot becoming a 2d-array recompiles");
    rp_S.tex[0][0] = 79;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 3, "queried 2d-array layer count stays in the key and recompiles");
    fake_gs_success = fake_gs_fact_deps = 0; separate_fixture = 0; fake_reg_changes = 1;
    rp_shader_reset(); rp_state_reset();
}

// ---------- 事实复用放宽（#2166）：高槽直连事实不拆程序；请求开关依赖来源可复用；该拦的仍然拦 ----------
// 真游戏里挡住复用的两道闸：① 完整组合表后面多出的高槽直连事实（如 fs 句柄 72，c2 字 72..86）让身份形状不合；
// ② vs+fs 配对程序的依赖里带 request:requiresLinkedVaryingInterface，来源不在白名单。夹具：编译器只查过 vs 槽 0
// （texture-fact:vs:8:-1）与 fs 槽 1（texture-fact:fs:10:2，c2 回退），其余组合槽与所有高槽都没问过。
// pair=1：两段程序（VS+FS；请求开关只在这个形状里出现，input_identity.c）；pair=0：三段程序。dep_extra 见 fake_gs_dep_extra。
static void fact_wide_begin(int pair, int dep_extra, uint64_t serial) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    const unsigned nst = pair ? 2 : 3;
    uint8_t keys[3][8]; for (unsigned st = 0; st < nst; st++) gs_source(st, 0, keys[st]);
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = serial; gs_program.nstage = nst;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < nst; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = pair ? 3 : 7; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "fact wide program binds (pair=%d)", pair);
    bind_pools(100); separate_fixture = 1; fake_reg_changes = 0;
    fake_gs_success = fake_gs_fact_deps = 1; fake_gs_dep_extra = dep_extra; fake_gs_result_mode = pair ? 1 : 0;
    compiler_calls = 0;
}
static void fact_wide_end(void) {
    fake_gs_success = fake_gs_fact_deps = fake_gs_dep_extra = fake_gs_result_mode = 0; fake_ubo_missing = 0;
    separate_fixture = 0; fake_reg_changes = 1; memset(&rp_S.ubo[1][0], 0, sizeof rp_S.ubo[1][0]);
    rp_shader_reset(); rp_state_reset();
}
// 「程序累计」行里的「事实复用 命中 N」：新身份被已编程序接走的次数（没有这段文字 = ~0）
static unsigned long long fact_hits_now(void) {
    extern void nvk_shader_test_tally_line(char *out, size_t n);
    char tl[640]; nvk_shader_test_tally_line(tl, sizeof tl);
    const char *h = strstr(tl, "事实复用 命中 ");
    return h ? strtoull(h + strlen("事实复用 命中 "), NULL, 10) : ~0ull;
}
static void graphics_fact_wide_test(void) {
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL, *first = NULL;
    // 1) vs+fs 配对、依赖里带请求开关：没查过的组合槽、各种高槽事实都不拆程序（真游戏 4,416 份 vs+fs 的形状）
    fact_wide_begin(1, 1, 960);
    const unsigned long long hits0 = fact_hits_now();
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: pair initial compile (calls=%d)", compiler_calls);
    first = p;
    rp_S.tex[0][2] = 77;    // 没查过的组合槽（77 带矩形视图，事实不同）
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: vs+fs program naming the request flag reuses for an unqueried slot (calls=%d)", compiler_calls);
    rp_S.tex[1][32] = 78;   // fs 高槽 0（c2 字 72）：身份多出 1 条编译器没问过的直连事实
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: a trailing fs high-slot fact does not split the program (calls=%d)", compiler_calls);
    rp_S.tex[1][32] = 77;   // 同一高槽换一张事实不同的贴图
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: a different trailing fact still reuses (calls=%d)", compiler_calls);
    rp_S.tex[0][34] = 78;   // vs 高槽 2（c2 字 76）：两段各有高槽事实
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: trailing facts in both stages still reuse (calls=%d)", compiler_calls);
    CHECK(fact_hits_now() - hits0 == 4, "fact wide: tally counts the 4 new identities served by reuse (got %llu)", fact_hits_now() - hits0);
    // 负对照：编译器查过的槽整条事实仍在键里（相对计数：不依赖前面有几次复用，主线上同样成立）
    int before = compiler_calls;
    rp_S.tex[0][0] = 77;    // 查过的 vs 槽 0
    CHECK(!rp_shader_resolve(&q, &p) && p && p != first && compiler_calls == before + 1, "fact wide: a queried slot stays in the key (calls=%d)", compiler_calls - before);
    before = compiler_calls;
    rp_S.tex[0][0] = 0; rp_S.tex[1][1] = 77;   // 查过的 fs 槽 1（c2 回退）
    CHECK(!rp_shader_resolve(&q, &p) && p && p != first && compiler_calls == before + 1, "fact wide: the queried c2 fallback slot stays in the key (calls=%d)", compiler_calls - before);
    fact_wide_end();

    // 2) 先带高槽事实编、再不带：口径对称（掩码键从不含高槽事实）
    fact_wide_begin(1, 1, 961);
    rp_S.tex[1][32] = 78; rp_S.tex[0][34] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: pair compiled with trailing facts (calls=%d)", compiler_calls);
    first = p;
    rp_S.tex[1][32] = 0; rp_S.tex[0][34] = 0;   // 高槽事实全撤：身份只剩完整组合表
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: an identity without trailing facts reuses a program compiled with them (calls=%d)", compiler_calls);
    rp_S.tex[1][33] = 78;   // 换另一个高槽（fs 高槽 1，c2 字 74）
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: another high slot reuses too (calls=%d)", compiler_calls);
    fact_wide_end();

    // 3) 三段程序：完整组合表 96 条，高槽事实排第 97 条（下标 96）。掩码第 4 个字是「常量缓冲使用位」（QueryConstantBufferUse stage=vs
    //    置了第 0 位），不是贴图事实掩码：按掩码挑事实时若越过 96 条，下标 96 会被它选进键里，换一张高槽贴图就不复用了
    fact_wide_begin(0, 0, 962);
    rp_S.tex[2][32] = 78;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: 3-stage initial compile (calls=%d)", compiler_calls);
    first = p;
    rp_S.tex[2][32] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: the 97th fact is outside the key although a constant-buffer bit is set (calls=%d)", compiler_calls);
    rp_S.tex[1][33] = 78;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide: 3-stage with trailing facts in two stages reuses (calls=%d)", compiler_calls);
    fact_wide_end();

    // 4) 编译器问过高槽句柄（没有这条事实 / 找到了）的程序不进复用：高槽事实不在键里，之后这条事实冒出来就该重编
    fact_wide_begin(1, 2, 963);   // QueryS2ASampledTextureFormat handle=72 → unbound
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: program that queried handle 72 (unbound) compiles (calls=%d)", compiler_calls);
    first = p;
    rp_S.tex[1][32] = 78;   // 句柄 72 的事实现在存在了
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2 && p != first, "fact wide: a handle-72 query is never answered from another identity (unbound; calls=%d)", compiler_calls);
    fact_wide_end();
    fact_wide_begin(1, 3, 964);   // handle=72 → texture-fact:fs:72:-1（找到了）
    rp_S.tex[1][32] = 78;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: program that queried handle 72 (found) compiles (calls=%d)", compiler_calls);
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "fact wide: a handle-72 query is never answered from another identity (found; calls=%d)", compiler_calls);
    fact_wide_end();

    // 5) 不认得的 request:* 来源仍走完整身份（只认请求开关 requiresLinkedVaryingInterface 这一个）
    fact_wide_begin(1, 4, 965);
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: program with another request:* source compiles (calls=%d)", compiler_calls);
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "fact wide: another request:* source keeps the exact identity path (calls=%d)", compiler_calls);
    fact_wide_end();

    // 6) 分离表事实（c2 字 90 起，cbuf 2）仍走完整身份
    fact_wide_begin(1, 1, 966);
    bind_separate(1, 0, 1, 77);
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: pair with a separate-table fact compiles (calls=%d)", compiler_calls);
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "fact wide: a separate-table fact keeps the exact identity path (calls=%d)", compiler_calls);
    fact_wide_end();

    // 7) 高槽里的缓冲贴图事实仍走完整身份
    fact_wide_begin(1, 1, 967); separate_fixture = 3;
    rp_S.tex[1][32] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide: pair with a trailing buffer-texture fact compiles (calls=%d)", compiler_calls);
    rp_S.tex[1][33] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "fact wide: a trailing buffer-texture fact keeps the exact identity path (calls=%d)", compiler_calls);
    fact_wide_end();

    // 8) 句柄在用户常量缓冲里的贴图（#999 第 4 片学来的事实，c3）仍走完整身份
    fact_wide_begin(1, 1, 968); fake_ubo_missing = 1;
    memset(ubo_mem, 0, sizeof ubo_mem); ubo_mem[92] = 78;
    rp_S.ubo[1][0].addr = (uint64_t)(uintptr_t)ubo_mem; rp_S.ubo[1][0].size = sizeof ubo_mem;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 2, "fact wide: UBO handle fact is learned and compiled in one draw (calls=%d)", compiler_calls);
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 3, "fact wide: a UBO-learned fact keeps the exact identity path (calls=%d)", compiler_calls);
    fact_wide_end();
}

// 关闭态（debug.s2a.nvk_fact_wide=0）：fact_wide() 进程内只读一次，单独起进程。要证「关 = 原来的两道闸，一个字节不差」
static void graphics_fact_wide_off_test(void) {
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL, *first = NULL;
    fact_wide_begin(1, 1, 970);   // 闸②：请求开关来源不在白名单 → 配对程序不进复用
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide off: pair initial compile (calls=%d)", compiler_calls);
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "fact wide off: the request-flag source still blocks reuse (calls=%d)", compiler_calls);
    fact_wide_end();
    fact_wide_begin(0, 0, 971);   // 严格形状、白名单内的来源：老路径照常复用
    const unsigned long long hits0 = fact_hits_now();
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide off: strict-shape initial compile (calls=%d)", compiler_calls);
    first = p;
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide off: strict shape still reuses (calls=%d)", compiler_calls);
    CHECK(fact_hits_now() - hits0 == 1, "fact wide off: tally still counts the reuse (got %llu)", fact_hits_now() - hits0);
    fact_wide_end();
    fact_wide_begin(0, 0, 972);   // 闸①：完整组合表后面多一条高槽事实 → 整个身份走完整路径
    rp_S.tex[1][32] = 78;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide off: trailing-fact initial compile (calls=%d)", compiler_calls);
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "fact wide off: a trailing high-slot fact still blocks reuse (calls=%d)", compiler_calls);
    fact_wide_end();
    fact_wide_begin(0, 2, 973);   // 高槽句柄的查询：关闭态不加这条拦截（严格形状下原来就复用）
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 1, "fact wide off: handle-72 query initial compile (calls=%d)", compiler_calls);
    first = p;
    rp_S.tex[0][2] = 77;
    CHECK(!rp_shader_resolve(&q, &p) && p == first && compiler_calls == 1, "fact wide off: no extra handle guard, old behavior unchanged (calls=%d)", compiler_calls);
    fact_wide_end();
}
static void graphics_high_slot_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = 901; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < 3; st++) { gs_stages[st] = (uint8_t)st; gs_source(st, 0, gs_keys[st]); }
    gs_keys_ok = 7; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "high-slot program binds");
    bind_pools(100); separate_fixture = 1; fake_gs_success = 1; fake_reg_changes = 0;
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    const char *stages[] = {"vs", "fs", "gs"};
    for (unsigned st = 0; st < 3; st++) for (unsigned slot = 0; slot < 8; slot++) {
        rp_shader_reset(); memset(rp_S.tex, 0, sizeof rp_S.tex); memset(rp_S.img, 0, sizeof rp_S.img);
        rp_S.tex[st][32 + slot] = 78; compiler_calls = 0;
        CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 1, "high-slot initial request st=%u slot=%u", st, slot);
        CHECK(fact_matches_at(stages[st], 72 + 2 * slot, -1, "2d", 1), "high-slot texture fact st=%u slot=%u", st, slot);
        CHECK(!fact_matches_at(stages[(st + 1) % 3], 72 + 2 * slot, -1, "2d", 1), "high-slot fact keeps its stage");
        CHECK(!fact_matches_at(stages[st], 88, -1, "2d", 1), "high-slot word 88 stays outside the image table");
        // iOS tex_slot_h: a nonzero image takes precedence over the combined texture.
        rp_S.img[st][slot] = 77;
        CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2 && fact_matches_at(stages[st], 72 + 2 * slot, -1, "2d", 0),
              "high-slot image precedence invalidates identity st=%u slot=%u", st, slot);
        rp_S.tex[st][32 + slot] = 99;
        CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 2, "shadowed high-slot texture keeps identity");
        rp_S.img[st][slot] = 99;
        CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 3 && !fact_matches_at(stages[st], 72 + 2 * slot, -1, "2d", 1) &&
              !fact_matches_at(stages[st], 72 + 2 * slot, -1, "2d", 0), "unregistered high-slot source does not invent a fact");
    }
    rp_shader_reset(); memset(rp_S.tex, 0, sizeof rp_S.tex); memset(rp_S.img, 0, sizeof rp_S.img);
    for (int st = 0; st < 3; st++) {
        for (int slot = 0; slot < 8; slot++) rp_S.img[st][slot] = 78;
        bind_separate(st, 0, 128, 78);
    }
    compiler_calls = 0;
    CHECK(!rp_shader_resolve(&q, &p) && compiler_calls == 1, "full high-slot request reaches compiler");
    for (int st = 0; st < 3; st++) {
        char needle[64]; snprintf(needle, sizeof needle, "\"stage\":\"%s\",\"handle\":", stages[st]);
        CHECK(count_substr(captured, needle) == 168 && fact_matches_at(stages[st], 86, -1, "2d", 1) && fact_matches_at(stages[st], 344, 2, "2d", 1),
              "all 504 graphics facts survive request st=%d", st);
    }
    rp_shader_reset(); rp_state_reset(); fake_gs_success = 0; separate_fixture = 0; fake_reg_changes = 1;
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
// c1 整段（八方旅人调色 LUT 的片元读 c1 到第 2304 字节；原先程序对象只拷前 0x100，后段读 0 → 场景整幅黑）。
// 期望值手算：c1 段 0x100 字节、全填 0x33 → 程序对象三个阶段 c1_len = 0x100、内容全 0x33，预热清单收下 1 条；
// c1 段 0x200 字节、全填 0x44 → c1_len = 0x200、第 0 与第 0x1ff 字节都是 0x44，预热清单同样收下 1 条（清单按整段记 c1；
// 整段落盘 / 读回见 nvn_vk_prog_warm_test.c「c1 整段往返」）。
static void graphics_c1_full_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    uint8_t ks[3][8], kl[3][8];
    gs_c1_len = 0x100; for (unsigned st = 0; st < 3; st++) gs_source_ex(st, 7, 0x33, ks[st]);
    gs_c1_len = 0x200; for (unsigned st = 0; st < 3; st++) gs_source_ex(st, 8, 0x44, kl[st]);
    gs_c1_len = 0x100;
    fake_gs_success = 1; fake_gs_fact_deps = 0; fake_gs_result_tag = 0; separate_fixture = 0; fake_reg_changes = 0;
    fixture_depth_zero = fixture_origin_lower_left = 0;
    struct nvk_prog_warm_stats w0, w1, w2; nvk_prog_warm_stats(&w0);
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    alias_bind(ks); bind_pools(100);
    CHECK(!rp_shader_resolve(&q, &p) && p, "c1 full: 0x100 program resolves");
    if (p) { int ok = (p->c1_ok & 7) == 7; for (int st = 0; st < 3 && ok; st++) { ok = p->c1_len[st] == 0x100; for (unsigned i = 0; i < 0x100 && ok; i++) ok = p->c1[st][i] == 0x33; }
             CHECK(ok, "c1 full: 0x100 c1 kept whole in every stage"); }
    nvk_prog_warm_stats(&w1);
    CHECK(w1.recorded == w0.recorded + 1, "c1 full: 0x100 program recorded in warm manifest (%llu -> %llu)", (unsigned long long)w0.recorded, (unsigned long long)w1.recorded);
    p = NULL; alias_bind(kl);
    CHECK(!rp_shader_resolve(&q, &p) && p, "c1 full: 0x200 program resolves");
    if (p) { int ok = (p->c1_ok & 7) == 7; for (int st = 0; st < 3 && ok; st++) ok = p->c1_len[st] == 0x200 && p->c1[st][0] == 0x44 && p->c1[st][0x1ff] == 0x44;
             CHECK(ok, "c1 full: 0x200 c1 kept whole (not cut at 0x100)"); }
    nvk_prog_warm_stats(&w2);
    CHECK(w2.recorded == w1.recorded + 1, "c1 full: long-c1 program is recorded in the warm manifest too (%llu -> %llu)", (unsigned long long)w1.recorded, (unsigned long long)w2.recorded);
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
    static uint8_t c1copy[3][0x400];   // F 随后被 rp_shader_reset 释放：c1 先拷到夹具自己的缓冲（生产里预热记录的 c1 指向清单的只读副本）
    for (unsigned st = 0; st < 3; st++) { memcpy(rec.keys[st], k1[st], 8);
        if ((f->c1_ok & (1u << st)) && f->c1_len[st] <= sizeof c1copy[st]) { memcpy(c1copy[st], f->c1[st], f->c1_len[st]); rec.c1[st] = c1copy[st]; rec.c1_len[st] = f->c1_len[st]; } }
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

// ---------- 编译被拒时的上下文日志（#999，只加日志）----------
// 程序键 = 发给编译器的 requestId（各阶段键的十六进制连起来）；image 句柄被拒时，按宿主当时记下的「为什么没出事实」分类计数并打一行。
static int request_id_in_captured(char *out, size_t n) {
    const char *p = strstr(captured, "\"requestId\":\"");
    if (!p) return 0;
    p += strlen("\"requestId\":\"");
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= n) return 0;
    memcpy(out, p, (size_t)(e - p)); out[e - p] = 0;
    return 1;
}
// 与真机日志同形的拒收消息：「<阶段>: texture handle from cbuf slot <槽> is not implemented (handle=<字>, …) [kind=image word=<字> key=…]」
static const char *img_reject_error_cbuf(const char *stage, unsigned cbuf, unsigned word) {
    static char b[640];
    snprintf(b, sizeof b, "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"%s: texture handle from cbuf slot %u is not implemented"
             " (handle=%u, arrayLength=1, count=1, request=%064d) [kind=image word=%u key=0123456789abcdef0123456789abcdef]\"}", stage, cbuf, word, 0, word);
    return b;
}
static const char *img_reject_error(const char *stage, unsigned word) { return img_reject_error_cbuf(stage, 2, word); }   // c2：驱动句柄表
static const char *const img_counters[] = {"image_fact_unbound", "image_fact_no_texture", "image_fact_no_fact", "image_fact_given", "image_fact_word_unexpected",
                                           "image_fact_cbuf_other"};
static void reject_log_reset(void) { reject_lines[0] = 0; nvn_unimpl_reset(); compiler_calls = 0; }
static void graphics_reject_context_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    uint8_t keys[2][8]; for (unsigned st = 0; st < 2; st++) gs_source(st, 0, keys[st]);
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G');
    gs_program.serial = 960; gs_program.nstage = 2;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned st = 0; st < 2; st++) { gs_stages[st] = (uint8_t)st; memcpy(gs_keys[st], keys[st], 8); }
    gs_keys_ok = 3; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "reject-context program binds");
    bind_pools(100); fake_reg_changes = 0;
    struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
    // 期望的程序键：两个阶段键的十六进制连起来（与 requestId 同一个字符串）
    char want_key[40] = ""; { int o = 0; for (unsigned st = 0; st < 2; st++) for (int b = 0; b < 8; b++) o += snprintf(want_key + o, sizeof want_key - (size_t)o, "%02x", keys[st][b]); }
    struct { const char *label; int st, slot; uint64_t img, tex; int fixture; const char *stage; unsigned word; int want; const char *reason, *at, *tail; } cases[] = {
        {"slot unbound, pool entry 0 not registered", 1, 1, 0, 0, 1, "fs", 74, 0, "reason=unbound", "stage=fs cbuf=2 slot=1 word=74", "handle=0x0 texture=0"},
        {"image handle names an unregistered texture", 1, 1, 99, 0, 1, "fs", 74, 1, "reason=no_texture", "stage=fs cbuf=2 slot=1 word=74", "handle=0x63 texture=99"},
        {"combined handle (no image) names an unregistered texture", 1, 1, 0, 99, 1, "fs", 74, 1, "reason=no_texture", "stage=fs cbuf=2 slot=1 word=74", "handle=0x63 texture=99"},
        {"registered texture whose format has no fact", 1, 1, 78, 0, 4, "fs", 74, 2, "reason=no_fact", "stage=fs cbuf=2 slot=1 word=74", "handle=0x4e texture=78 format=0x63"},
        {"registered texture, fact given, compiler still rejects", 1, 1, 78, 0, 1, "fs", 74, 3, "reason=given", "stage=fs cbuf=2 slot=1 word=74", "handle=0x4e texture=78 format="},
        {"unbound slot but pool entry 0 registered: fact given", 1, 1, 0, 0, 2, "fs", 74, 3, "reason=given", "stage=fs cbuf=2 slot=1 word=74", "handle=0x0 texture=0 format="},
        {"vertex stage slot 0 (word 72)", 0, 0, 99, 0, 1, "vs", 72, 1, "reason=no_texture", "stage=vs cbuf=2 slot=0 word=72", "handle=0x63 texture=99"},
        {"highest slot 7 (word 86)", 1, 7, 99, 0, 1, "fs", 86, 1, "reason=no_texture", "stage=fs cbuf=2 slot=7 word=86", "handle=0x63 texture=99"},
        {"word 88 is outside the image table", 1, 1, 78, 0, 1, "fs", 88, 4, "reason=word_unexpected", "stage=fs cbuf=2 slot=-1 word=88", ""},
        {"odd word", 1, 1, 78, 0, 1, "fs", 75, 4, "reason=word_unexpected", "stage=fs cbuf=2 slot=-1 word=75", ""},
        {"message names a stage this program does not have", 1, 1, 78, 0, 1, "gs", 74, 4, "reason=word_unexpected", "stage=gs cbuf=2 slot=1 word=74", ""},
    };
    fake_gs_success = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *L = cases[i].label;
        rp_shader_reset(); memset(rp_S.tex, 0, sizeof rp_S.tex); memset(rp_S.img, 0, sizeof rp_S.img);
        rp_S.img[cases[i].st][cases[i].slot] = cases[i].img; rp_S.tex[cases[i].st][32 + cases[i].slot] = cases[i].tex;
        separate_fixture = cases[i].fixture; fake_compile_error = img_reject_error(cases[i].stage, cases[i].word); reject_log_reset();
        CHECK(rp_shader_resolve(&q, &p) != 0 && compiler_calls == 1, "%s: the compiler rejection reaches the host once (calls=%d)", L, compiler_calls);
        for (unsigned k = 0; k < sizeof img_counters / sizeof img_counters[0]; k++)
            CHECK(nvn_unimpl_count("shader", img_counters[k]) == (k == (unsigned)cases[i].want ? 1u : 0u), "%s: counter %s = %llu", L, img_counters[k],
                  (unsigned long long)nvn_unimpl_count("shader", img_counters[k]));
        char rid[64] = "", keyeq[96];
        CHECK(request_id_in_captured(rid, sizeof rid) && !strcmp(rid, want_key), "%s: requestId is the stage keys in hex (%s vs %s)", L, rid, want_key);
        snprintf(keyeq, sizeof keyeq, "key=%s", want_key);
        CHECK(count_substr(reject_lines, "texture_pool_reject_program reason=cbuf_handle_unsupported stages=2") == 1 && count_substr(reject_lines, "image_fact_reject") == 1 &&
              count_substr(reject_lines, keyeq) == 2, "%s: one program line and one image line, both with the program key: %s", L, reject_lines);
        CHECK(strstr(reject_lines, cases[i].reason) && strstr(reject_lines, cases[i].at) && strstr(reject_lines, cases[i].tail), "%s: image line says why: %s", L, reject_lines);
    }
    // 常量缓冲槽号不是 2：消息里的 word 是那一槽自己的字偏移，不是 c2 图像句柄表（字 72 + 2×槽）的坐标，不能拿它去查「高槽为什么没出事实」。
    // 这里把 c2 槽 1 摆成「登记了、事实给了、编译器仍拒」（把 word 74 当成 c2 槽 1 去查就会归成 given）：槽号不是 2 必须归 cbuf_other，
    // 行里带槽号（slot=-1），不带 handle / texture / format（没有可看的 c2 记录）。
    {
        struct { const char *label; unsigned cbuf, word; } other[] = {
            {"user cbuf slot 3, word 74", 3, 74}, {"cbuf slot 0, word 72", 0, 72}, {"cbuf slot 9, word 86", 9, 86}, {"packed slot number (high 16 bits set)", 65538, 74},
        };
        for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {
            const char *L = other[i].label;
            rp_shader_reset(); memset(rp_S.tex, 0, sizeof rp_S.tex); memset(rp_S.img, 0, sizeof rp_S.img);
            rp_S.img[1][1] = 78; separate_fixture = 1;
            fake_compile_error = img_reject_error_cbuf("fs", other[i].cbuf, other[i].word); reject_log_reset();
            CHECK(rp_shader_resolve(&q, &p) != 0 && compiler_calls == 1, "%s: the compiler rejection reaches the host once (calls=%d)", L, compiler_calls);
            for (unsigned k = 0; k < sizeof img_counters / sizeof img_counters[0]; k++)
                CHECK(nvn_unimpl_count("shader", img_counters[k]) == (k == 5 ? 1u : 0u), "%s: counter %s = %llu", L, img_counters[k],
                      (unsigned long long)nvn_unimpl_count("shader", img_counters[k]));
            char at[128]; snprintf(at, sizeof at, "reason=cbuf_other stage=fs cbuf=%u slot=-1 word=%u key=%s", other[i].cbuf, other[i].word, want_key);
            CHECK(count_substr(reject_lines, "image_fact_reject") == 1 && strstr(reject_lines, at) && !strstr(reject_lines, "handle=") &&
                  !strstr(reject_lines, "texture=") && !strstr(reject_lines, "format="),
                  "%s: the image line names the cbuf slot and reads no c2 record: %s", L, reject_lines);
        }
    }
    // 事实给了 / 格式出不了事实：行里带格式和目标；没登记 / 没绑：不带（没有可看的）
    rp_shader_reset(); memset(rp_S.tex, 0, sizeof rp_S.tex); memset(rp_S.img, 0, sizeof rp_S.img); rp_S.img[1][1] = 99; separate_fixture = 1;
    fake_compile_error = img_reject_error("fs", 74); reject_log_reset();
    CHECK(rp_shader_resolve(&q, &p) != 0 && !strstr(reject_lines, "format="), "an unregistered texture has no format to print: %s", reject_lines);
    // 反例：拒收不是 image 句柄 → 只有程序键那行，没有 image 行、没有 image 计数
    rp_shader_reset(); fake_compile_error = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"fs: texture handle from cbuf slot 2 is not implemented"
                                       " (handle=90, arrayLength=1, count=1, request=0) [kind=texture word=90 word2=257 type=3 cbuf=2/0 key=0123456789abcdef0123456789abcdef]\"}";
    reject_log_reset();
    CHECK(rp_shader_resolve(&q, &p) != 0 && count_substr(reject_lines, "texture_pool_reject_program reason=cbuf_handle_unsupported") == 1 && !strstr(reject_lines, "image_fact_reject"),
          "a texture-kind rejection prints only the program line: %s", reject_lines);
    for (unsigned k = 0; k < sizeof img_counters / sizeof img_counters[0]; k++)
        CHECK(nvn_unimpl_count("shader", img_counters[k]) == 0, "a texture-kind rejection does not count as %s", img_counters[k]);
    // 拒收消息里没有程序键的类别（译码器未接的指令）：照样补出程序键，对得上请求
    rp_shader_reset(); fake_compile_error = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"TRANSLATION_DIAGNOSTIC\",\"message\":\"Shader instruction Sam is not implemented. (stage fs)\"}";
    reject_log_reset();
    { char key2[96]; snprintf(key2, sizeof key2, "reason=TRANSLATION_DIAGNOSTIC stages=2 key=%s", want_key);
      CHECK(rp_shader_resolve(&q, &p) != 0 && strstr(reject_lines, key2) && !strstr(reject_lines, "image_fact_reject"), "a rejection whose message carries no key still prints the program key: %s", reject_lines); }
    // 没被拒：不打这两类日志、不记这五个计数
    fake_compile_error = NULL; fake_gs_success = 1; fake_gs_result_mode = 1; rp_shader_reset(); reject_log_reset();
    CHECK(!rp_shader_resolve(&q, &p) && p && !reject_lines[0], "an accepted program prints no rejection context: %s", reject_lines);
    for (unsigned k = 0; k < sizeof img_counters / sizeof img_counters[0]; k++)
        CHECK(nvn_unimpl_count("shader", img_counters[k]) == 0, "an accepted program does not count as %s", img_counters[k]);
    // 配对接口表诊断（nvk_link_map_diag）：关时请求不带 linkMapDiag、不打行；开时配对 VS+FS 请求带上，结果信封里的两侧表原样打成一行（带程序键）
    CHECK(!strstr(captured, "linkMapDiag") && link_map_lines == 0, "link map diag off: request omits linkMapDiag and nothing is printed");
    link_map_flag = 1; rp_shader_reset(); reject_log_reset(); link_map_lines = 0; last_link_map[0] = 0;
    CHECK(!rp_shader_resolve(&q, &p) && p && strstr(captured, "\"requiresLinkedVaryingInterface\":true,\"linkMapDiag\":true"), "link map diag on: paired request asks for the map");
    { char want[200]; snprintf(want, sizeof want, "配对接口表 程序键=vs:%.16s fs:%.16s 身份=", want_key, want_key + 16);
      CHECK(link_map_lines == 1 && strstr(last_link_map, want) == last_link_map &&
            strstr(last_link_map, " VS输出(逻辑>宿主)=0>0,3>1,7>2 FS输入(逻辑>宿主)=3>1,7>2") && !strstr(last_link_map, "?"),
            "link map diag on: one line with program key and both sides: %s", last_link_map); }
    // 档位：替身编译器给的表两侧对得上（FS 3>1,7>2 都在 VS 里）、VS 有 3 个 varying →
    //   3 档（只打对不上的）不打、不占名额；2 档（≥2 个 varying 或对不上）打。手算：3 档后行数不变，2 档后 +1
    { int before = link_map_lines;
      link_map_flag = 3; bind_pools(201); (void)rp_shader_resolve(&q, &p);
      CHECK(link_map_lines == before, "link map diag level 3 skips matching pairs (lines %d -> %d)", before, link_map_lines);
      link_map_flag = 2; bind_pools(202); (void)rp_shader_resolve(&q, &p);
      CHECK(link_map_lines == before + 1, "link map diag level 2 prints pairs with >=2 varyings (lines %d -> %d)", before, link_map_lines);
      link_map_flag = 1; }
    // 上限：同一进程只打前 40 对（换身份重编也不再请求、不再打）
    for (int i = 0; i < 45; i++) { bind_pools(300 + i); (void)rp_shader_resolve(&q, &p); }
    CHECK(link_map_lines == 40 && !strstr(captured, "linkMapDiag"), "link map diag stops after 40 pairs and stops asking (lines=%d)", link_map_lines);
    link_map_flag = 0; bind_pools(100);
    // 行数预算：140 个不同程序（换池长度就换身份）连续被拒，两类日志各只打 128 行，计数不设上限；
    // 超出预算时在拒绝原因读数里补一行「已截断，共 N 条」（诊断开关开着，进程正常收尾的回调才注册，用它触发那一组读数）；换会话预算归位
    fake_gs_success = 0; fake_gs_result_mode = 0; separate_fixture = 1;
    rp_shader_reset(); memset(rp_S.tex, 0, sizeof rp_S.tex); memset(rp_S.img, 0, sizeof rp_S.img); reject_log_reset();
    fake_compile_error = img_reject_error("fs", 74); diag_enabled = 1; last_trunc[0] = 0;
    for (int i = 0; i < 140; i++) { bind_pools(200 + i); CHECK(rp_shader_resolve(&q, &p) != 0, "budget loop %d is rejected", i); }
    CHECK(compiler_calls == 140, "140 different programs compile 140 times (calls=%d)", compiler_calls);
    CHECK(count_substr(reject_lines, "texture_pool_reject_program") == 128 && count_substr(reject_lines, "image_fact_reject") == 128,
          "each kind of line stops at 128 per session (program=%d image=%d)", count_substr(reject_lines, "texture_pool_reject_program"), count_substr(reject_lines, "image_fact_reject"));
    CHECK(nvn_unimpl_count("shader", "image_fact_unbound") == 140, "the counter keeps counting past the line budget (count=%llu)", (unsigned long long)nvn_unimpl_count("shader", "image_fact_unbound"));
    last_trunc[0] = 0; s2a_direct_summary(1);
    CHECK(strstr(last_trunc, "已截断，共 程序行 140 条（只打了前 128 条）、image 行 140 条（只打了前 128 条）"), "a truncated session says how many rejections there were: %s", last_trunc);
    diag_enabled = 0;
    rp_shader_reset(); bind_pools(100); reject_lines[0] = 0; last_trunc[0] = 0;
    CHECK(rp_shader_resolve(&q, &p) != 0 && count_substr(reject_lines, "texture_pool_reject_program") == 1 && count_substr(reject_lines, "image_fact_reject") == 1 && !last_trunc[0],
          "a new session gets its line budget back (and no stale truncation notice): %s | %s", reject_lines, last_trunc);
    fake_compile_error = NULL; separate_fixture = 0; fake_reg_changes = 1;
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
    compiler_calls = 0; nvn_unimpl_reset(); reject_lines[0] = 0;
    CHECK(!rp_shader_resolve(&q, &p) && p && compiler_calls == 2, "missing c3 handle fact is learned and compiled in the same draw: calls=%d", compiler_calls);
    CHECK(!reject_lines[0], "a rejection that only taught a coordinate prints no program-key line (it recompiles at once): %s", reject_lines);
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
    // 片 4（#1594）：句柄字指向没登记的贴图号。Ryujinx GetForBinding / GetTextureDescriptor 取到默认描述符、整笔照画，事实同组合槽
    //   （默认事实只此一份，texture_facts.c），不再是 ubo_handle_no_texture 红行、不再整笔 program_unresolved。
    //   池夹具：起点 0、容量 100；夹具只登记 77 / 78 / 79，其余号都是「没登记」。
    {
        const struct rp_prog *reg = p;   // 上一步用已登记的 78 编出的那份
        uint64_t cnt0 = rp_shader_ubo_default_fact_count();
        nvn_unimpl_reset(); ubo_default_lines = 0; last_ubo_default_line[0] = 0; last_red[0] = 0;
        CHECK(cnt0 == 0, "换局后默认事实计数清零 (count=%llu)", (unsigned long long)cnt0);
        ubo_mem[92] = 0xFFFFF;   // 20 位全 1：游戏常拿它当「这一路不用贴图」的哨兵；池容量 100 → 池外
        int calls0 = compiler_calls;
        CHECK(!rp_shader_resolve(&q, &p) && p && p != reg && compiler_calls == calls0 + 1, "out-of-pool handle word (0xFFFFF) compiles with the default fact, draw is not left unresolved (calls=%d)", compiler_calls - calls0);
        CHECK(ubo_fact_in_request("2d", 1), "out-of-pool id carries the combined-slot default {2d, normalized}");
        CHECK(strstr(captured, "\"format\":\"Unknown\"") != NULL, "default fact format is Unknown");
        CHECK(nvn_unimpl_count("shader", "ubo_handle_no_texture") == 0 && nvn_unimpl_count("shader", "compile_failed") == 0 && last_red[0] == 0,
              "out-of-pool handle is not a red line any more (compile_failed=%llu)", (unsigned long long)nvn_unimpl_count("shader", "compile_failed"));
        CHECK(rp_shader_ubo_default_fact_count() >= 1, "default-fact counter counts it (count=%llu)", (unsigned long long)rp_shader_ubo_default_fact_count());
        CHECK(ubo_default_lines == 1 && strstr(last_ubo_default_line, "贴图号 1048575") && strstr(last_ubo_default_line, "容量 100") && strstr(last_ubo_default_line, "fs c3 第 92 字"),
              "first line per session names the id, the pool capacity and the coordinate: %s", last_ubo_default_line);
        const struct rp_prog *beyond = p;
        calls0 = compiler_calls;
        CHECK(!rp_shader_resolve(&q, &p) && p == beyond && compiler_calls == calls0, "same out-of-pool id takes the fast path");
        ubo_mem[92] = 0xFFFFE;   // 另一个池外号：同一份默认事实 → 同一份程序，不重编
        CHECK(!rp_shader_resolve(&q, &p) && p == beyond && compiler_calls == calls0, "another out-of-pool id has identical facts: no recompile (calls=%d)", compiler_calls - calls0);
        ubo_mem[92] = 50;        // 池内、没登记：{1d, 不归一化}，与池外事实不同 → 重编（快路摘要把两种默认事实分开）
        CHECK(!rp_shader_resolve(&q, &p) && p && p != beyond && p != reg && compiler_calls == calls0 + 1, "in-pool unregistered id has other facts: recompiles (calls=%d)", compiler_calls - calls0);
        CHECK(ubo_fact_in_request("1d", 0), "in-pool unregistered id carries the combined-slot default {1d, unnormalized}");
        CHECK(ubo_default_lines == 1, "only the first default fact of the session prints a line (lines=%d)", ubo_default_lines);
        uint64_t cnt1 = rp_shader_ubo_default_fact_count();
        ubo_mem[92] = 78;        // 反例：已登记的号不受影响，不记默认事实
        CHECK(!rp_shader_resolve(&q, &p) && p == reg && compiler_calls == calls0 + 1 && rp_shader_ubo_default_fact_count() == cnt1,
              "registered id is untouched and not counted (calls=%d)", compiler_calls - calls0);
        // 反例：读不到句柄字仍是「没有事实」（物理上读不到），不当成默认事实去猜；读得到的池外号之后仍编得出，不被那份失败项挡住
        rp_S.ubo[1][0].size = 92 * 4; ubo_mem[92] = 0xFFFFF;
        nvn_unimpl_reset();
        CHECK(rp_shader_resolve(&q, &p) == -1 && nvn_unimpl_count("shader", "ubo_handle_unreadable") >= 1 && rp_shader_ubo_default_fact_count() == cnt1,
              "unreadable word is still fact-less and not counted as a default fact");
        rp_S.ubo[1][0].size = sizeof ubo_mem;
        CHECK(!rp_shader_resolve(&q, &p) && p == beyond, "readable again with an out-of-pool id: back to the default-fact program, not the stale fact-less failure");
        // 换局：计数与首行重来
        rp_shader_reset(); ubo_default_lines = 0; ubo_mem[92] = 0xFFFFF;
        CHECK(rp_shader_ubo_default_fact_count() == 0, "session reset clears the default-fact counter");
        CHECK(!rp_shader_resolve(&q, &p) && p && ubo_default_lines == 1 && rp_shader_ubo_default_fact_count() >= 1, "next session prints its own first line (lines=%d)", ubo_default_lines);
        ubo_mem[92] = 78; rp_shader_reset(); rp_S.ubo[1][0].size = sizeof ubo_mem; compiler_calls = 0;
    }
    // 反例：c2 / 组合槽坐标的缺事实不进这张表（不当成用户常量缓冲去读）
    rp_shader_reset(); fake_ubo_missing = 0; fake_gs_success = 0; compiler_calls = 0;
    fake_compile_error = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QuerySamplerType has no source (stage=fs,handle=92,cbufSlot=2)\"}";
    nvn_unimpl_reset(); reject_lines[0] = 0;
    CHECK(rp_shader_resolve(&q, &p) == -1 && compiler_calls == 1, "c2 missing fact is not retried as a user UBO handle (calls=%d)", compiler_calls);
    { char rid[64] = "";
      CHECK(request_id_in_captured(rid, sizeof rid) && count_substr(reject_lines, "texture_pool_reject_program reason=MISSING_FACT.QuerySamplerType") == 1 && strstr(reject_lines, rid),
            "a rejection nothing can be learned from prints its program key (requestId %s): %s", rid, reject_lines); }
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

static void query_sampler_test(void) {
    CHECK(s2a_spirv_sampler_unused(sampler_query_spv, sizeof sampler_query_spv, 2, 0), "query descriptor does not use sampler");
    CHECK(!s2a_spirv_sampler_unused(sampler_sample_spv, sizeof sampler_sample_spv, 2, 0), "actual sampling still needs sampler");
    CHECK(s2a_spirv_sampler_unused(sampler_mixed_spv, sizeof sampler_mixed_spv, 2, 0) &&
          !s2a_spirv_sampler_unused(sampler_mixed_spv, sizeof sampler_mixed_spv, 2, 1), "sampler use proof is per binding");
    CHECK(!s2a_spirv_sampler_unused(sampler_query_spv, sizeof sampler_query_spv, 0, 0) &&
          !s2a_spirv_sampler_unused(sampler_query_spv, sizeof sampler_query_spv - 1, 2, 0), "unknown descriptor and malformed word stream do not prove sampler unused");
    for (int query = 0; query < 2; ++query) {
        rp_state_reset(); rp_shader_reset(); source(); bind_pools(100); separate_fixture = 1;
        fake_cs_success = query ? 2 : 3;
        p_bind_handles b = {0}; b.stage = 5; b.n = 1; b.first = 0; b.h[0] = 77;
        CHECK(rp_state_op(OP_BIND_IMG, &b, sizeof b), "query image handle binding");
        const struct rp_prog *p = NULL;
        CHECK(!rp_shader_resolve_compute(&p) && p && p->nent[0] == 2, "sampler proof fixture loads: %s", last_red);
        if (p) CHECK(p->sampler_unused[0][1] == query, "production compute builder retains sampler use proof: query=%d", query);
    }
    fake_cs_success = 0;
}

// #1594 片 8：计算阶段整池数组的布局与日志键（真解析 + 生产 build_compute / layout_ent）。
//   期望（手算）：v2 结果里贴图池 count 8448、采样器池 count 1280 → 两个布局项：NVN_RB_TEXP / 贴图 / count 8449（池长 + 表尾 dummy）、
//   NVN_RB_SAMPP / 采样器 / count 1281，只给计算阶段，着色器装载 1 次，没有任何未接 / 解析失败记账；
//   同样 8448 项但不带 pool 的普通数组：仍拒（装载 0 次）、记 texture_pool_array_unsupported 一次，原因带 stage / 长度与这份代码的 8 字节程序键
//   （rp_S.skey[5] 按字节十六进制，同现编日志里的键），多个身份 / 多份代码的同类拒绝才能逐个对上
static void compute_pool_test(void) {
    for (int mode = 4; mode <= 6; ++mode) {
        rp_state_reset(); rp_shader_reset(); nvn_unimpl_reset(); source(); bind_pools(100); separate_fixture = 1; last_red[0] = 0;
        rp_S.skey[0][0] ^= 0xff;   // source() 把图形 VS 阶段的键也设成同一个值；错开它，日志里取错阶段的键才会被抓到（计算只认 skey[5]）
        char needle[48];   // 键是 source() 登记的那份快照的哈希前 8 字节（计算阶段键 rp_S.skey[5]，与固定来源一一对应，不能随意改）
        snprintf(needle, sizeof needle, "程序键 %02x%02x%02x%02x%02x%02x%02x%02x", rp_S.skey[5][0], rp_S.skey[5][1], rp_S.skey[5][2], rp_S.skey[5][3],
                 rp_S.skey[5][4], rp_S.skey[5][5], rp_S.skey[5][6], rp_S.skey[5][7]);
        fake_cs_success = mode; cs_loads = 0;
        const struct rp_prog *p = NULL;
        const int rc = rp_shader_resolve_compute(&p);
        if (mode == 4) {
            CHECK(rc == 0 && p && p->is_compute && p->nent[0] == 2, "compute pool fixture loads with two layout entries (rc=%d red=%s)", rc, last_red);
            if (rc == 0 && p && p->nent[0] == 2) {
                const nvn_gpu_layout_ent *t = &p->ent[0][0], *s = &p->ent[0][1];
                CHECK(t->src_kind == NVN_RB_TEXP && t->cls == NVN_GPU_RES_TEX && t->count == 8449 && t->set == 2 && t->binding == 0 && t->stages == (1u << NVN_GPU_SH_CS),
                      "compute texture pool entry: TEXP, texture class, 8448 + 1 dummy, CS only");
                CHECK(s->src_kind == NVN_RB_SAMPP && s->cls == NVN_GPU_RES_SAMP && s->count == 1281 && s->set == 2 && s->binding == 1 && s->stages == (1u << NVN_GPU_SH_CS),
                      "compute sampler pool entry: SAMPP, sampler class, 1280 + 1 dummy, CS only");
            }
            CHECK(cs_loads == 1 && nvn_unimpl_count("shader", "texture_pool_array_unsupported") == 0 && nvn_unimpl_count("shader", "compute_program_parse_failed") == 0,
                  "compute pool result loads the shader once with no unimplemented accounting (loads=%d red=%s)", cs_loads, last_red);
        } else if (mode == 5) {
            CHECK(rc == -1 && !p && cs_loads == 0 && nvn_unimpl_count("shader", "texture_pool_array_unsupported") == 1,
                  "plain 8448-element compute array is still rejected as array_unsupported (rc=%d loads=%d)", rc, cs_loads);
            CHECK(strstr(last_red, "stage cs descriptor array length=8448 exceeds 64") && strstr(last_red, needle),
                  "rejection line carries stage, length and the 8-byte program key (%s): %s", needle, last_red);
        } else {   // 6：形状不对的池数组走「解析失败」的具名行，原因 + 程序键同样齐全，不记成数组未接
            CHECK(rc == -1 && !p && cs_loads == 0 && nvn_unimpl_count("shader", "compute_program_parse_failed") == 1 &&
                  nvn_unimpl_count("shader", "texture_pool_array_unsupported") == 0,
                  "malformed compute pool array is a named parse failure, not array_unsupported (rc=%d loads=%d)", rc, cs_loads);
            CHECK(strstr(last_red, "compute pool array length is outside 2..65534") && strstr(last_red, needle),
                  "parse-failure line carries the shape reason and the 8-byte program key (%s): %s", needle, last_red);
        }
    }
    fake_cs_success = 0; separate_fixture = 0;
}

// #1594 片 10：计算阶段、贴图字在游戏常量缓冲里的类型 3 句柄（真解析 + 生产 build_compute / layout_ent / usrc_put）。
//   期望（手算）：7：一个 UBO 形态的常量采样器（uboSlot 2 = cbuf 5、uboOffset 160、池下标 260）→ 一个布局项：TEXU / 贴图采样 / 数量 1、src_reg = 2、src_off = 160、
//   src_reg2 = 0（usrc 表下标）、只给计算阶段；程序 usrc 表一项：贴图字 {UBO 2, +160}、采样器 SAMPI + 池下标 260 + samp_pool = 1；着色器装载 1 次，没有解析失败记账。
//   8：同一个计算程序三项（两个 UBO 形态 + 一个 c2 形态 slot 100 / 池下标 259）→ UBO 两项 src_reg2 = 0 / 1、usrc 两项各取各的（UBO 3 +164 / 池下标 262），
//   c2 形态仍是 NVN_RB_TEX（贴图槽 133，src_reg2 = 标记位 | 259），不进 usrc 表。
//   9：UBO 形态但没有 constSampler（组合句柄在游戏 UBO 里，计算侧不接）→ 整份拒，走 compute_program_parse_failed，不装载。
static void compute_ubo_const_sampler_test(void) {
    for (int mode = 7; mode <= 9; ++mode) {
        rp_state_reset(); rp_shader_reset(); nvn_unimpl_reset(); source(); bind_pools(100); separate_fixture = 1; last_red[0] = 0;
        fake_cs_success = mode; cs_loads = 0;
        const struct rp_prog *p = NULL;
        const int rc = rp_shader_resolve_compute(&p);
        if (mode == 7) {
            CHECK(rc == 0 && p && p->is_compute && p->nent[0] == 1, "compute UBO constant sampler fixture loads with one layout entry (rc=%d red=%s)", rc, last_red);
            if (rc == 0 && p && p->nent[0] == 1) {
                const nvn_gpu_layout_ent *t = &p->ent[0][0];
                CHECK(t->src_kind == NVN_RB_TEXU && t->cls == NVN_GPU_RES_TEX_SAMP && t->count == 1 && t->src_reg == 2 && t->src_off == 160 && t->src_reg2 == 0 &&
                      t->set == 2 && t->binding == 0 && t->stages == (1u << NVN_GPU_SH_CS),
                      "compute UBO constant sampler layout entry: TEXU, UBO 2 + 160, usrc index 0, CS only");
                CHECK(p->nusrc == 1 && p->usrc[0].tex.reg == 2 && p->usrc[0].tex.off == 160 && !p->usrc[0].tex.sid && !p->usrc[0].tex.pair &&
                      p->usrc[0].samp_kind == NVN_RB_SAMPI && p->usrc[0].samp_reg == 260 && p->usrc[0].samp_pool == 1,
                      "compute UBO constant sampler usrc entry: texture word at UBO 2 + 160, immediate pool index 260 (SAMPI, samp_pool 1)");
            }
            CHECK(cs_loads == 1 && nvn_unimpl_count("shader", "compute_program_parse_failed") == 0,
                  "compute UBO constant sampler result loads the shader once (loads=%d red=%s)", cs_loads, last_red);
        } else if (mode == 8) {
            CHECK(rc == 0 && p && p->is_compute && p->nent[0] == 3, "compute mixed UBO / c2 constant sampler fixture loads with three layout entries (rc=%d red=%s)", rc, last_red);
            if (rc == 0 && p && p->nent[0] == 3) {
                const nvn_gpu_layout_ent *a = &p->ent[0][0], *b = &p->ent[0][1], *c = &p->ent[0][2];
                CHECK(a->src_kind == NVN_RB_TEXU && a->src_reg == 2 && a->src_off == 160 && a->src_reg2 == 0 &&
                      b->src_kind == NVN_RB_TEXU && b->src_reg == 3 && b->src_off == 164 && b->src_reg2 == 1,
                      "two compute UBO constant sampler entries keep their own usrc indexes");
                CHECK(p->nusrc == 2 && p->usrc[0].tex.reg == 2 && p->usrc[0].tex.off == 160 && p->usrc[0].samp_reg == 260 && p->usrc[0].samp_pool == 1 &&
                      p->usrc[1].tex.reg == 3 && p->usrc[1].tex.off == 164 && p->usrc[1].samp_reg == 262 && p->usrc[1].samp_pool == 1,
                      "two compute usrc entries keep their own texture words and pool indexes");
                CHECK(c->src_kind == NVN_RB_TEX && c->src_reg == 133 && c->src_reg2 == (uint16_t)(NVN_GPU_SAMP2_CONST | 259u),
                      "the c2 constant sampler entry of the same program stays a plain TEX entry (not in the usrc table)");
            }
        } else {
            CHECK(rc == -1 && !p && cs_loads == 0 && nvn_unimpl_count("shader", "compute_program_parse_failed") == 1,
                  "compute UBO texture without a constant sampler is a named parse failure (rc=%d loads=%d)", rc, cs_loads);
        }
    }
    fake_cs_success = 0; separate_fixture = 0;
}

// ---- 新层查出包预编库（LibPack，shaders/direct/b-<DMSH_CHAIN>-vk）的三段程序：vs + fs + gs ----
// 容器在临时目录里现写（格式同 libpack_vk_test.c / LibPack.cs：头 48 字节 + 分片表 + (键, 种类) 升序记录），走真实 libpack_vk.c。
// 期望手算：键按 NVN 位序紧排 vs / fs / gs；几何段按输入图元取 _vg<t>（种类 112+t / 120+t）——_vg0 是 24 字节（头 + 1 条 OpNop）、
// _vg3 是 28 字节（头 + 2 条 OpNop），装载到 GS 的字节数就认出是哪一份；_vg1 不放，线输入必 miss。编译器替身此时会成功（fake_gs_success），
// 所以「命中」的证据是编译器调用数不变，miss 的证据是编译器被调用一次。
static void lp_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void lp_put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static int lp_write(const char *dir, uint64_t key[], const uint8_t kind[], const uint8_t *data[], const size_t len[], int n) {
    char path[640]; snprintf(path, sizeof path, "%s/lib.000.pak", dir);   // 调用方的 dir 最长 599 字节，加 "/lib.000.pak" 含结尾 0 要 612 字节；600 会被 -Werror=format-truncation 拦在编译（#1701）
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    uint64_t off[16], at = 0;
    for (int i = 0; i < n; i++) { off[i] = at; fwrite(data[i], 1, len[i], f); at += len[i]; }
    fclose(f);
    uint8_t idx[48 + 16 + 24 * 16]; memset(idx, 0, sizeof idx);
    memcpy(idx, "S2ALIB\0\0", 8); lp_put32(idx + 8, 1); lp_put32(idx + 12, (uint32_t)n); lp_put32(idx + 16, 1);
    memcpy(idx + 24, DMSH_CHAIN, 12); lp_put64(idx + 48, at);
    for (int i = 0; i < n; i++) {   // 调用方已按 (键, 种类) 升序给
        uint8_t *r = idx + 64 + 24 * i;
        lp_put64(r, key[i]); r[8] = kind[i]; r[9] = 0; lp_put32(r + 12, (uint32_t)len[i]); lp_put64(r + 16, off[i]);
    }
    snprintf(path, sizeof path, "%s/lib.idx", dir);
    f = fopen(path, "wb"); if (!f) return -1;
    fwrite(idx, 1, 64 + 24 * (size_t)n, f); fclose(f);
    return 0;
}
static uint64_t lp_key(const uint8_t k[8]) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | k[i]; return v; }
// Exercise the actual warm VS/FS refusal and foreground GS refusal on the same library.
static void link_refused_test(const char *root, uint8_t keys[3][8], char hex[3][17]) {
    char path[320];
    snprintf(path,sizeof path,"%s/libpack-link-refused-v1.tsv",root);
    setenv("S2A_PW_TEST_DIR", root, 1);
    struct nvk_prog_rec rec = {0};
    memcpy(rec.keys,keys,2 * sizeof keys[0]);
    rec.stage_mask = 3;
    rec.lp_ok = 2;
    rec.identity[0] = 1;
    CHECK(nvk_prog_warm_build(&rec) < 0, "unlinked warm program refused with switch unset");
    CHECK(access(path,F_OK) < 0, "default-off creates no refusal file");
    record_link_refused = 1;
    for (unsigned i=0;i<6;i++) {
        rec.identity[0]=(uint8_t)(2+i);
        CHECK(nvk_prog_warm_build(&rec) < 0, "unlinked warm program %u still refused", i);
    }
    rp_shader_reset();
    const struct rp_prog *prog = NULL;
    struct rp_shader_req q = {.prim=4};
    compiler_calls = 0;
    CHECK(!rp_shader_resolve(&q,&prog) && prog && !prog->lib_sourced && compiler_calls==1,
          "unlinked GS falls back to live compilation with diagnostics enabled");
#ifdef S2A_ANDROID_DEBUG_ENV
    FILE *f = fopen(path,"r");
    CHECK(f!=NULL,"debug diagnostic records refused program pairs");
    if (f) {
        char row[180], expected[180];
        unsigned lines=0;
        while (fgets(row,sizeof row,f)) {
            snprintf(expected,sizeof expected,"%s\t%s\t0x0000002000000000\t%s\t%s\n",hex[0],hex[1],
                     DMSH_CHAIN,lines<6?"-":hex[2]);
            CHECK(!strcmp(row,expected),"VS/FS, missing bit 37, chain and actual GS on row %u",lines);
            lines++;
        }
        fclose(f);
        CHECK(lines==7,"all seven rejects recorded beyond the four-line log limit");
        const off_t limit=1024*1024;
        CHECK(truncate(path,limit-1)==0,"seed almost-full refusal file");
        rec.identity[0]=31;
        CHECK(nvk_prog_warm_build(&rec)<0,"full diagnostic still refuses unlinked program");
        struct stat st;
        CHECK(!stat(path,&st) && st.st_size==limit-1,"persistent cap declines entire next row");
    }
#else
    (void)hex;
    CHECK(access(path,F_OK)<0,"player build has no refusal file even with forced flag");
#endif
    record_link_refused=0;
    unlink(path);
    unsetenv("S2A_PW_TEST_DIR");
}

// 采集：成功的冷局普通绘制落盘一份完整请求（调试包才有；玩家包永不记录着色器请求）。
static void check_warm_capture(const char *dir, const uint8_t identity[32], const char *hex) {
    char path[1024];
    snprintf(path, sizeof path, "%s/warm-nowindow-capture/%s.request", dir, hex);
    FILE *f = fopen(path, "rb");
#ifdef S2A_ANDROID_DEBUG_ENV
    CHECK(f != NULL, "successful cold ordinary draw publishes one record");
    if (!f) return;
    uint8_t header[76], preimage[S2A_INPUT_IDENTITY_MAX], digest[32];
    int valid = fread(header, 1, sizeof header, f) == sizeof header && !memcmp(header, "S2AWREQ1", 8);
    size_t n = valid ? (size_t)header[8] | (size_t)header[9] << 8 |
                       (size_t)header[10] << 16 | (size_t)header[11] << 24 : 0;
    valid = n && n <= sizeof preimage && fread(preimage, 1, n, f) == n;
    CHECK(valid, "capture contains complete identity preimage");
    if (valid) {
        s2a_sha256 h;
        s2a_sha256_init(&h); s2a_sha256_update(&h, preimage, n); s2a_sha256_final(&h, digest);
        CHECK(!memcmp(digest, identity, 32), "all 32 bytes of the identity match the preimage");
        size_t len = strlen(captured);
        char *raw = malloc(len + 1);
        CHECK(raw && fread(raw, 1, len, f) == len && !memcmp(raw, captured, len) && fgetc(f) == EOF,
              "capture contains the exact request passed to the foreground compiler");
        s2a_sha256_init(&h); s2a_sha256_update(&h, header, 44);
        s2a_sha256_update(&h, preimage, n); s2a_sha256_update(&h, captured, len);
        s2a_sha256_final(&h, digest);
        CHECK(!memcmp(digest, header + 44, 32), "request, preimage and result digest bound together");
        free(raw);
    }
#else
    (void)identity;
    CHECK(f == NULL, "player build never records shader requests");
#endif
    if (f) fclose(f);
}
// 种子补件各臂：同一份完整身份，关 / 开 / 别的 profile / 内部身份被改 / 摘要被改 / 文件缺失。只有「开且补件完好」一臂免前台翻译。
//   donor = 一次真实冷局落下的程序清单（nvk_prog_warm_save）；捐出来放进「闭包-profile」种子目录，补件放在它的 nowindow/ 子目录。
static void test_nowindow_seed(const char *root, const char *manifest, const struct rp_shader_req *q) {
    DIR *dir = opendir(manifest);
    CHECK(dir != NULL, "open donor program manifest");
    if (!dir) return;
    char name[80] = "";
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "programs-v1-", 12) == 0 && strlen(de->d_name) < sizeof name)
            strcpy(name, de->d_name);
    }
    closedir(dir);
    char donor[560], parent[640], cohort[680], shipped[780], companion[720];
    snprintf(donor, sizeof donor, "%s/%s", manifest, name);
    FILE *f = fopen(donor, "rb");
    uint8_t header[60];
    int valid = f && fread(header, 1, sizeof header, f) == sizeof header;
    if (f) fclose(f);
    CHECK(valid, "read donor seed header");
    if (!valid) return;
    char profile[17], closure[17];
    for (int i = 0; i < 8; i++) {
        snprintf(profile + 2*i, 3, "%02x", header[16+i]);
        snprintf(closure + 2*i, 3, "%02x", header[52+i]);
    }
    snprintf(parent, sizeof parent, "%s/shaders/direct/warm-vk", root);
    snprintf(cohort, sizeof cohort, "%s/%s-%s", parent, closure, profile);
    snprintf(shipped, sizeof shipped, "%s/%s", cohort, name);
    snprintf(companion, sizeof companion, "%s/nowindow", cohort);
    CHECK(!mkdir(parent, 0700) && !mkdir(cohort, 0700) && !mkdir(companion, 0700) &&
          !rename(donor, shipped), "install the original seed");
    fake_gs_result_mode = 1;  // The real draw has VS + FS.
    rp_shader_reset();
    setenv("S2A_DIRECT_CACHE_DIR", companion, 1);
    fixture_warm_capture = 1;
#ifdef S2A_ANDROID_DEBUG_ENV
    struct nvk_prog_rec prior = {0};
    CHECK(!nvk_prog_warm_load() && !nvk_prog_warm_take(&prior), "capture ignores shipped historical recipes");
#endif
    compiler_calls = 0;
    const struct rp_prog *ordinary = NULL;
    CHECK(!rp_shader_resolve(q, &ordinary) && ordinary && !ordinary->lib_sourced && compiler_calls == 1,
          "record one ordinary nonaligned program");
    uint8_t ordinary_id[32];
    if (!ordinary) return;
    memcpy(ordinary_id, ordinary->identity, sizeof ordinary_id);
    CHECK(!s2a_program_cache_drain(5000), "finish writing donor bytes");
    unsetenv("S2A_DIRECT_CACHE_DIR");
    char hex[65], program[sizeof companion + 75];
    for (unsigned i = 0; i < sizeof ordinary_id; ++i)
        snprintf(hex + 2*i, 3, "%02x", ordinary_id[i]);
    check_warm_capture(manifest, ordinary_id, hex);
    fixture_warm_capture = 0;
    snprintf(program, sizeof program, "%s/%s.program", companion, hex);
    // 完好补件的字节留一份：BUILD_FAIL 一臂要在「摘要按改后内容重算」下改坏阶段（过得了读入核对、过不了 build）
    uint8_t *good = NULL; size_t good_len = 0; uint8_t good_sha[32]; char gwhy[128] = "";
    CHECK(!s2a_program_cache_load(companion, ordinary_id, &good, &good_len, good_sha, gwhy, sizeof gwhy), "keep the intact companion bytes: %s", gwhy);
    enum { DISABLED, VALID, OTHER_PROFILE, WRONG_ID, BAD_DIGEST, ABSENT, BUILD_FAIL };
    for (int arm = DISABLED; arm <= BUILD_FAIL; ++arm) {
        fixture_warm_nowindow = arm != DISABLED;
        if (arm == WRONG_ID || arm == BAD_DIGEST) {
            FILE *file = fopen(program, "r+b");
            CHECK(file != NULL, "open companion to exercise integrity checks");
            if (!file) return;
            // Internal identity occupies bytes 16..47. Preserve its prefix so
            // the first control detects any accidental short-identity lookup.
            int changed = !fseek(file, 47, SEEK_SET) &&
                          fputc(ordinary_id[31] ^ (arm == WRONG_ID), file) != EOF;
            if (arm == BAD_DIGEST) {
                int last = EOF;
                if (!fseek(file, -1, SEEK_END)) last = fgetc(file);
                changed = changed && last != EOF && !fseek(file, -1, SEEK_END) &&
                          fputc(last ^ 1, file) != EOF;
            }
            CHECK(!fclose(file) && changed, "prepare control %d", arm);
        } else if (arm == ABSENT) {
            CHECK(!unlink(program), "remove companion for missing-file control");
        } else if (arm == BUILD_FAIL && good) {
            // 把 FS 的阶段名改成不认识的（等长，摘要由 store 按改后的内容重算）：读入核对照过，build() 解析失败
            char *fs = memmem(good, good_len, "\"stage\":\"fs\"", 12);
            CHECK(fs != NULL, "find the FS stage in the intact companion");
            if (fs) fs[10] = 'x';
            s2a_program_cache_store_async(companion, ordinary_id, good, good_len);
            CHECK(!s2a_program_cache_drain(5000), "write the stage-broken companion");
        }
        rp_shader_reset();
        compiler_calls = 0;
        struct nvk_prog_rec rec = {0};
        CHECK(nvk_prog_warm_load() == 1 && nvk_prog_warm_take(&rec) &&
              !memcmp(rec.identity, ordinary_id, 32), "load the same full identity");
        if (arm == OTHER_PROFILE) strcpy(external_profile_hash, "sha256-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        int built = nvk_prog_warm_build(&rec);
        external_profile_hash[0] = 0;
        const struct rp_prog *adopted = NULL;
        int rc = rp_shader_resolve(q, &adopted);
        int expected_calls = arm == VALID ? 0 : 1;   // BUILD_FAIL：补件建失败不发布，前台现编 1 次、绘制不被跳过（rc == 0 且拿到程序）
        CHECK((built == 0) == (arm == VALID) && !rc && adopted && !adopted->lib_sourced &&
              compiler_calls == expected_calls,
              "non-window companion arm=%d: warm rc=%d, first resolve rc=%d, foreground translations=%d (expected %d)",
              arm, built, rc, compiler_calls, expected_calls);
    }
    free(good);
    fixture_warm_nowindow = 0;
    fake_gs_result_mode = 0;
}

static const char *output_fixture_path;
static int output_fixture_class;

static void test_libpack_outputs(const uint8_t keys[3][8]) {
    struct rp_shader_req q = {.prim=4, .rt_count=5,
        .rt_format={NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8, NVN_FORMAT_RG16, NVN_FORMAT_RGBA8I}};
    const struct rp_prog *p = NULL;
    rp_S.bser[2] = 0;
    rp_shader_reset();
    fake_gs_result_mode = 1;
    compiler_calls = 0;
    int rc = rp_shader_resolve(&q, &p);
    CHECK(!rc && p && p->lib_sourced == !output_fixture_class,
          "float MRT resolves matching output class, unused signed slot ignored");
    q.rt_format[1] = q.rt_format[2] = NVN_FORMAT_RGBA8UI;
    q.rt_format[3] = NVN_FORMAT_RG16UI;
    rp_S.chmask.mask = 15; // the integer attachments are bound, but only float output 0 is enabled
    rc = rp_shader_resolve(&q, &p);
    CHECK(!rc && p && p->lib_sourced, "masked MRT locations allow either module");
    rp_S.chmask.mask = 0xffffffffu;
    for (int repeat = 0; repeat < 2; repeat++) {
        rc = rp_shader_resolve(&q, &p);
        CHECK(!rc && p && p->lib_sourced == (output_fixture_class == 1),
              "integer MRT resolves matching output class after float pass (repeat=%d)", repeat);
    }
    q.rt_format[1] = q.rt_format[2] = NVN_FORMAT_RGBA8I;
    q.rt_format[3] = NVN_FORMAT_RG16I;
    rc = rp_shader_resolve(&q, &p);
    CHECK(!rc && p && p->lib_sourced == (output_fixture_class == 2), "signed MRT selects only signed outputs");
    // With the slots unbound, the canonical compiler input is the initial Float
    // input again. A resident ordinary program is valid and must stay reusable.
    int previous_calls = compiler_calls;
    q.rt_format[1] = q.rt_format[2] = q.rt_format[3] = 0;
    rc = rp_shader_resolve(&q, &p);
    CHECK(!rc && p && compiler_calls == previous_calls, "unbinding slots keeps a compatible resident result");
    // Isolate cold admission from resident/disk reuse: both modules are legal
    // with these holes. Rebinding the holes to Float must recheck the candidate.
    rp_shader_reset();
    bind_pools(103);
    rc = rp_shader_resolve(&q, &p);
    CHECK(!rc && p && p->lib_sourced, "unbound outputs and unwritten slot allow either module");
    q.rt_format[1] = q.rt_format[2] = NVN_FORMAT_RGBA8;
    q.rt_format[3] = NVN_FORMAT_RG16;
    rc = rp_shader_resolve(&q, &p);
    CHECK(!rc && p && p->lib_sourced == !output_fixture_class, "rebind Float into previously empty slots");

    rp_shader_reset();
    bind_pools(102);
    q.rt_format[1] = q.rt_format[2] = NVN_FORMAT_RGBA8UI;
    q.rt_format[3] = NVN_FORMAT_RG16UI;
    fixture_no_libpack = 1;
    rc = rp_shader_resolve(&q, &p);
    fixture_no_libpack = 0;
    CHECK(!rc && p && !p->lib_sourced, "ordinary MRT warm identity");
    if (rc || !p) return;
    struct nvk_prog_rec rec = {.stage_mask=3, .lp_ok=2};
    memcpy(rec.identity, p->identity, 32);
    memcpy(rec.keys, keys, 16);
    rp_shader_reset();
    CHECK(!nvk_prog_warm_build(&rec), "build prewarmed MRT candidate");
    rc = rp_shader_resolve(&q, &p);
    CHECK(!rc && p && p->lib_sourced == (output_fixture_class == 1), "prewarmed integer MRT selects compatible module");
}

static int gs_libpack_test(int diagnose_refusals, int numeric) {
    fake_depth_clip = 1;   // 深度 near=-w 也能用预编库（libpack_state_ok），夹具不带深度零
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    uint8_t k[3][8]; for (unsigned st = 0; st < 3; st++) gs_source(st, 0, k[st]);
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G'); gs_program.serial = 101; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned i = 0; i < 3; i++) { gs_stages[i] = (uint8_t)i; memcpy(gs_keys[i], k[i], 8); }
    gs_keys_ok = 7; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "libpack GS fixture binds program");
    CHECK(rp_shader_graphics_stages() == 7, "libpack GS fixture stage mask is 0x7");
    bind_pools(7);
    // 容器：<根>/shaders/direct/b-<链>-vk
    char root[256], dir[600];
    snprintf(root, sizeof root, "%s/nvk_gs_libpack.XXXXXX", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { CHECK(0, "libpack temp root"); return 1; }
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    static const uint32_t spv0[5] = {0x07230203u, 0x00010300u, 0, 1, 0};
    static const uint32_t spv_vg0[6] = {0x07230203u, 0x00010300u, 0, 1, 0, 0x00010000u};
    static const uint32_t spv_vg3[7] = {0x07230203u, 0x00010300u, 0, 1, 0, 0x00010000u, 0x00010000u};
    char hx[3][17]; for (int st = 0; st < 3; st++) for (int b = 0; b < 8; b++) snprintf(hx[st] + 2 * b, 3, "%02x", k[st][b]);
    char jvs[160], jfs[512], jg0[200], jg3[200];
    snprintf(jvs, sizeof jvs, "{\"stage\":\"vs\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hx[0]);
    snprintf(jfs, sizeof jfs, "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hx[1]);
    snprintf(jg0, sizeof jg0, "{\"stage\":\"gs\",\"dm_ctl\":\"%s\",\"gs_input_topology\":0,\"bindings\":[]}", hx[2]);
    snprintf(jg3, sizeof jg3, "{\"stage\":\"gs\",\"dm_ctl\":\"%s\",\"gs_input_topology\":3,\"bindings\":[]}", hx[2]);
    if (numeric) snprintf(jfs, sizeof jfs,
        "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":["
        "{\"msl_name\":\"fp_c3\",\"kind\":\"cbuf\",\"reg\":0,\"arg_id\":0},"
        "{\"msl_name\":\"fp_t_sep\",\"kind\":\"tex\",\"reg\":128,\"arg_id\":1},"
        "{\"msl_name\":\"fp_i0\",\"kind\":\"img\",\"reg\":2,\"arg_id\":2}]}", hx[1]);
    struct { uint64_t key; uint8_t kind; const uint8_t *d; size_t n; } rec[8] = {
        {lp_key(k[0]), 1, (const uint8_t *)jvs, strlen(jvs)}, {lp_key(k[0]), 52, (const uint8_t *)spv0, sizeof spv0},
        {lp_key(k[1]), 1, (const uint8_t *)jfs, strlen(jfs)}, {lp_key(k[1]), 52, numeric ? libpack_frag_spv : (const uint8_t *)spv0, numeric ? sizeof libpack_frag_spv : sizeof spv0},
        {lp_key(k[2]), 112, (const uint8_t *)jg0, strlen(jg0)}, {lp_key(k[2]), 115, (const uint8_t *)jg3, strlen(jg3)},
        {lp_key(k[2]), 120, (const uint8_t *)spv_vg0, sizeof spv_vg0}, {lp_key(k[2]), 123, (const uint8_t *)spv_vg3, sizeof spv_vg3},
    };
    // The fragment consumes input location 37; neither synthetic producer exports a user location.
    static const uint32_t fragment_input[] = {
        0x07230203, 0x00010300, 0, 8, 0, 0x00040047, 4, 30, 37, 0x00030016, 1, 32,
        0x00040020, 3, 1, 1, 0x0004003b, 3, 4, 1,
    };
    uint32_t output_spv[4096];
    if (output_fixture_path) {
        FILE *file = fopen(output_fixture_path, "rb");
        if (!file) return 1;
        size_t bytes = fread(output_spv, 1, sizeof output_spv, file);
        int valid = bytes && bytes < sizeof output_spv && !ferror(file) && bytes % 4 == 0;
        fclose(file);
        if (!valid) return 1;
        rec[3].d = (const uint8_t *)output_spv;
        rec[3].n = bytes;
    }
    if (diagnose_refusals) { rec[3].d=(const uint8_t*)fragment_input; rec[3].n=sizeof fragment_input; }
    for (int i = 0; i < 8; i++) for (int j = i + 1; j < 8; j++)   // (键, 种类) 升序
        if (rec[j].key < rec[i].key || (rec[j].key == rec[i].key && rec[j].kind < rec[i].kind)) { __typeof__(rec[0]) t = rec[i]; rec[i] = rec[j]; rec[j] = t; }
    uint64_t keys[8]; uint8_t kinds[8]; const uint8_t *data[8]; size_t lens[8];
    for (int i = 0; i < 8; i++) { keys[i] = rec[i].key; kinds[i] = rec[i].kind; data[i] = rec[i].d; lens[i] = rec[i].n; }
    // 库目录还不在：取程序照常走现编；「出包预编库不在」打一行黄、同一资源根只打一次；程序累计行的库状态 = 不在
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
    {
        extern void nvk_shader_test_tally_line(char *out, size_t n);
        char tl[640];
        nvk_shader_test_tally_line(tl, sizeof tl);
        CHECK(strstr(tl, "出包预编（库没查过）") != NULL, "libpack tally before any lookup says not looked up: %s", tl);
        fake_gs_success = 1; lp_missing_lines = lp_open_lines = 0;
        for (int i = 0; i < 2; i++) {
            rp_shader_reset(); compiler_calls = 0;
            struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
            CHECK(rp_shader_resolve(&q, &p) == 0 && p && compiler_calls == 1, "libpack missing (%d): live compile resolves (calls=%d)", i, compiler_calls);
        }
        CHECK(lp_missing_lines == 1, "missing libpack directory is one yellow line per root (got %d)", lp_missing_lines);
        nvk_shader_test_tally_line(tl, sizeof tl);
        CHECK(strstr(tl, "出包预编（库不在）") != NULL, "libpack tally says the library is missing: %s", tl);
        fake_gs_success = 0;
    }
    CHECK(lp_write(dir, keys, kinds, data, lens, 8) == 0, "libpack GS container written");
    static const char prof_on[] = "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16,\"supportsGeometryShader\":true}}";
    static const char prof_off[] = "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16,\"supportsGeometryShader\":false}}";
    snprintf(external_profile, sizeof external_profile, "%s", prof_on);
    fake_gs_success = 1;
    if (output_fixture_path) {
        test_libpack_outputs(k);
        goto cleanup_gs;
    }
    if (numeric) {
        rp_S.bser[2] = 0;
        bind_pools(100);
        rp_S.tex[1][128] = 78;
        separate_fixture = 1;
        fake_gs_result_mode = 1;
        const uint32_t formats[] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8UI, NVN_FORMAT_RGBA8I};
        for (unsigned i = 0; i < 3; i++) {
            bind_pools(100);
            sampled_fixture_format = formats[i];
            rp_shader_reset(); compiler_calls = 0;
            setenv("S2A_SHADER_REVISION_ROOT", root, 1);
            struct rp_shader_req q = {0}; q.prim = 4;
            const struct rp_prog *pg = NULL;
            int rc = rp_shader_resolve(&q, &pg);
            CHECK(!rc && pg && pg->lib_sourced == (i == 0) && compiler_calls == (i != 0),
                  "cold sampled numeric admission format %u, rc=%d translations=%d", formats[i], rc, compiler_calls);
            // 旧清单从一次普通编译取得；独立池长让它不会命中上面冷路径刚写的拒收表。
            bind_pools(101); rp_shader_reset(); fixture_no_libpack = 1;
            rc = rp_shader_resolve(&q, &pg); fixture_no_libpack = 0;
            CHECK(!rc && pg && !pg->lib_sourced, "obtain the warm identity from ordinary compilation");
            if (rc || !pg) continue;
            struct nvk_prog_rec rec = {0}; memcpy(rec.identity, pg->identity, 32);
            rec.stage_mask = 3; rec.lp_ok = 2; memcpy(rec.keys, k, 16);
            rp_shader_reset(); compiler_calls = 0;
            CHECK(!nvk_prog_warm_build(&rec), "warm candidate built before request facts");
            pg = NULL; rc = rp_shader_resolve(&q, &pg);
            CHECK(!rc && pg && pg->lib_sourced == (i == 0) && compiler_calls == (i != 0),
                  "warm sampled numeric admission format %u, rc=%d translations=%d", formats[i], rc, compiler_calls);
        }
        goto cleanup_gs;
    }
    if (diagnose_refusals) {
        link_refused_test(root,k,hx);
        goto cleanup_gs;
    }
    const uint32_t prims[4] = {4, 0, 1, 4};   // 三角列表 / 点 / 线 / 三角列表但 profile 没开几何
    const char *what[4] = {"triangles", "points", "lines", "triangles, geometryShader off"};
    for (int c = 0; c < 4; c++) {
        if (c == 3) snprintf(external_profile, sizeof external_profile, "%s", prof_off);
        rp_shader_reset(); nvn_unimpl_reset(); compiler_calls = 0; memset(gs_loads, 0, sizeof gs_loads); memset(gs_blob_bytes, 0, sizeof gs_blob_bytes);
        struct rp_shader_req q = {0}; q.prim = prims[c]; const struct rp_prog *p = NULL;
        int rc = rp_shader_resolve(&q, &p);
        CHECK(rc == 0 && p, "libpack GS %s: program resolves", what[c]);
        CHECK(gs_loads[NVN_GPU_SH_VS] == 1 && gs_loads[NVN_GPU_SH_FS] == 1 && gs_loads[NVN_GPU_SH_GS] == 1, "libpack GS %s: three stages load", what[c]);
        if (c < 2) {
            CHECK(compiler_calls == 0, "libpack GS %s: hit from the library, compiler not called (calls=%d)", what[c], compiler_calls);
            CHECK(gs_blob_bytes[NVN_GPU_SH_VS] == sizeof spv0 && gs_blob_bytes[NVN_GPU_SH_FS] == sizeof spv0, "libpack GS %s: vs / fs are the library's", what[c]);
            CHECK(gs_blob_bytes[NVN_GPU_SH_GS] == (c == 0 ? sizeof spv_vg3 : sizeof spv_vg0),
                  "libpack GS %s: GS stage is the _vg%d variant (%zu bytes)", what[c], c == 0 ? 3 : 0, gs_blob_bytes[NVN_GPU_SH_GS]);
        } else {
            CHECK(compiler_calls == 1, "libpack GS %s: library misses, live compile runs (calls=%d)", what[c], compiler_calls);
        }
    }
    {   // 只有 vs（0x1：只写深度的绘制）：库里 vs 那一条，不造 fs / gs；库里 vs 的两条记录都在，编译器替身此时会成功，
        //   所以「命中」的证据仍是编译器调用数不变（撤掉取库入口的 0x1u 闸，这里就多一次现编）
        rp_shader_reset(); nvn_unimpl_reset(); compiler_calls = 0; memset(gs_loads, 0, sizeof gs_loads); memset(gs_blob_bytes, 0, sizeof gs_blob_bytes);
        const uint64_t fs_serial = rp_S.bser[1], gs_serial = rp_S.bser[2];
        rp_S.bser[1] = rp_S.bser[2] = 0;
        CHECK(rp_shader_graphics_stages() == 1, "libpack VS-only fixture stage mask is 0x1");
        struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
        int rc = rp_shader_resolve(&q, &p);
        CHECK(rc == 0 && p, "libpack VS-only: program resolves");
        CHECK(gs_loads[NVN_GPU_SH_VS] == 1 && gs_loads[NVN_GPU_SH_FS] == 0 && gs_loads[NVN_GPU_SH_GS] == 0, "libpack VS-only: only the vertex stage loads");
        CHECK(compiler_calls == 0, "libpack VS-only: hit from the library, compiler not called (calls=%d)", compiler_calls);
        CHECK(gs_blob_bytes[NVN_GPU_SH_VS] == sizeof spv0 && gs_blob_bytes[NVN_GPU_SH_FS] == 0, "libpack VS-only: the vertex stage is the library's");
        // 库件结果备忘（nvk_shader.c lp_memo_*）：程序表清空后同一组阶段键再取（真机上是换了状态的新身份），
        //   不再读库件、不再合成——库自己的命中计数不动，结果仍是库里那份，编译器仍不调
        {
            extern void nvk_shader_test_tally_line(char *out, size_t n);
            char tl[640]; nvk_shader_test_tally_line(tl, sizeof tl);
            const char *mh = strstr(tl, "库件备忘 命中 "); unsigned long long memo0 = mh ? strtoull(mh + strlen("库件备忘 命中 "), NULL, 10) : ~0ull;
            CHECK(mh != NULL, "libpack memo: tally prints the memo counter: %s", tl);
            const uint64_t lib_hits = s2a_libpack_hits();
            rp_shader_reset(); compiler_calls = 0; memset(gs_loads, 0, sizeof gs_loads); memset(gs_blob_bytes, 0, sizeof gs_blob_bytes);
            const struct rp_prog *p2 = NULL;
            CHECK(rp_shader_resolve(&q, &p2) == 0 && p2, "libpack memo: program resolves again after the program table is cleared");
            CHECK(s2a_libpack_hits() == lib_hits, "libpack memo: the same stage keys are served from the memo, library not re-read (hits %llu -> %llu)",
                  (unsigned long long)lib_hits, (unsigned long long)s2a_libpack_hits());
            CHECK(compiler_calls == 0 && gs_loads[NVN_GPU_SH_VS] == 1 && gs_blob_bytes[NVN_GPU_SH_VS] == sizeof spv0,
                  "libpack memo: memo result is the library's vertex stage, compiler not called (calls=%d)", compiler_calls);
            nvk_shader_test_tally_line(tl, sizeof tl);
            mh = strstr(tl, "库件备忘 命中 ");
            CHECK(mh && strtoull(mh + strlen("库件备忘 命中 "), NULL, 10) == memo0 + 1, "libpack memo: tally counts exactly one more memo hit: %s", tl);
        }
        rp_S.bser[1] = fs_serial; rp_S.bser[2] = gs_serial;
    }
    {
        extern void nvk_shader_test_tally_line(char *out, size_t n);
        char tl[640]; nvk_shader_test_tally_line(tl, sizeof tl);
        CHECK(lp_open_lines >= 1 && strstr(tl, "出包预编（库已打开）") != NULL, "libpack tally says opened after the container appears (%d open lines): %s", lp_open_lines, tl);
        // 另一个资源根里的库是坏的（lib.idx 不是容器）：照旧报红 libpack_container_rejected，库状态 = 坏，不打「不在」
        char root2[300], d2[700], f2[760];
        snprintf(root2, sizeof root2, "%s-bad", root); mkdir(root2, 0700);
        snprintf(d2, sizeof d2, "%s/shaders", root2); mkdir(d2, 0700); snprintf(d2, sizeof d2, "%s/shaders/direct", root2); mkdir(d2, 0700);
        snprintf(d2, sizeof d2, "%s/shaders/direct/b-%s-vk", root2, DMSH_CHAIN); mkdir(d2, 0700);
        snprintf(f2, sizeof f2, "%s/lib.idx", d2); { FILE *g = fopen(f2, "wb"); if (g) { fputs("not a libpack index", g); fclose(g); } }
        setenv("S2A_SHADER_REVISION_ROOT", root2, 1); lp_missing_lines = 0; last_red[0] = 0;
        rp_shader_reset(); compiler_calls = 0;
        { struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL; rp_shader_resolve(&q, &p); }
        nvk_shader_test_tally_line(tl, sizeof tl);
        CHECK(strstr(tl, "出包预编（库坏）") != NULL && lp_missing_lines == 0, "broken container: tally says bad, no missing line (%d): %s", lp_missing_lines, tl);
        unlink(f2); rmdir(d2); snprintf(d2, sizeof d2, "%s/shaders/direct", root2); rmdir(d2); snprintf(d2, sizeof d2, "%s/shaders", root2); rmdir(d2); rmdir(root2);
    }
cleanup_gs:
    fake_gs_success = 0; external_profile[0] = 0; unsetenv("S2A_SHADER_REVISION_ROOT");
    char path[700];
    snprintf(path, sizeof path, "%s/lib.idx", dir); unlink(path); snprintf(path, sizeof path, "%s/lib.000.pak", dir); unlink(path);
    rmdir(dir); snprintf(dir, sizeof dir, "%s/shaders/direct", root); rmdir(dir); snprintf(dir, sizeof dir, "%s/shaders", root); rmdir(dir); rmdir(root);
    printf("nvk shader pool (libpack GS): %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
// ---- 计算出包预编库的非对齐闸（#496085）：库件只有对齐变体（libpack_vk.c 恒写 hasUnalignedStorageBuffer:false），
//   sbuf 绑定实况非对齐时出库 = 无钳制对齐码跑非对齐缓冲（9000 DeviceLost 元凶 cs 50c91227a1082f2c 库版同型）。
//   state_ok 对 ckey 必须生效：非对齐 → 不取库走现编加固件（请求带 hasUnalignedStorageBuffer:true），对齐 → 照常出库 ----
// 另起一个资源根放一份计算库件并指过去（库句柄按资源根缓存，同一根里改写文件读不到新内容）。root 由调用方给容量 256 的缓冲
static int cs_lib_root(char *root, uint64_t key[], const uint8_t kind[], const uint8_t *data[], const size_t len[], int n) {
    char dir[600];
    snprintf(root, 256, "%s/nvk_cs_libpack2.XXXXXX", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) return -1;
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    if (lp_write(dir, key, kind, data, len, n)) return -1;
    setenv("S2A_SHADER_REVISION_ROOT", root, 1);
    return 0;
}
static void cs_lib_root_rm(const char *root) {
    char path[700];
    snprintf(path, sizeof path, "%s/shaders/direct/b-%s-vk/lib.idx", root, DMSH_CHAIN); unlink(path);
    snprintf(path, sizeof path, "%s/shaders/direct/b-%s-vk/lib.000.pak", root, DMSH_CHAIN); unlink(path);
    snprintf(path, sizeof path, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); rmdir(path);
    snprintf(path, sizeof path, "%s/shaders/direct", root); rmdir(path);
    snprintf(path, sizeof path, "%s/shaders", root); rmdir(path); rmdir(root);
}
const char *nvk_shader_cs_lib_note(const uint8_t key[8]);   // nvk_shader.c：计算阶段键没走出包预编库的原因（诊断）
static int cs_libpack_test(void) {
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    source(); fake_cs_success = 1; compiler_calls = cs_loads = 0; captured[0] = last_red[0] = 0;
    bind_pools(100);
    rp_S.ubo[5][0].addr = 0x1000; rp_S.ubo[5][0].size = 64;
    rp_S.ssbo[5][0].addr = 0x10000; rp_S.ssbo[5][0].size = 256;   // 16 对齐（夹具 storage_buffer_offset_alignment=16）
    char root[256], dir[600];
    snprintf(root, sizeof root, "%s/nvk_cs_libpack.XXXXXX", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { CHECK(0, "cs libpack temp root"); return 1; }
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    // 最小计算库件：_b.json 只列 drvcb（.spv 声明 support_buffer b47 → emit 按 .spv 补出，同 libpack_vk_test 的 CS_NOSUP 夹具）。
    //   dm_ctl 必须等于键 hex（libpack_vk.c 命中判据），按 skey[5] 现拼
    char khex[17]; for (int b = 0; b < 8; b++) snprintf(khex + 2 * b, 3, "%02x", rp_S.skey[5][b]);
    char csjson[512];
    snprintf(csjson, sizeof csjson,
        "{\"key\":\"%s_b\",\"stage\":\"cs\",\"dm_ctl\":\"%s\",\"bindings\":["
        "{\"msl_name\":\"cp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":0}]}", khex, khex);
    static const uint32_t csspv[] = {
        0x07230203u, 0x00010000u, 0u, 3u, 0u,
        0x00060005u, 2u, 0x70707573u, 0x5f74726fu, 0x66667562u, 0x00007265u,   // OpName %2 "support_buffer"
        0x00040047u, 2u, 33u, 47u,                                             // OpDecorate %2 Binding 47
        0x00040047u, 2u, 34u, 0u,                                              // OpDecorate %2 DescriptorSet 0
    };
    uint64_t ck[2] = { lp_key(rp_S.skey[5]), lp_key(rp_S.skey[5]) };
    uint8_t ckind[2] = { 1, 52 };
    const uint8_t *cd[2] = { (const uint8_t *)csjson, (const uint8_t *)csspv };
    size_t cn[2] = { strlen(csjson), sizeof csspv };
    CHECK(lp_write(dir, ck, ckind, cd, cn, 2) == 0, "cs libpack container written");
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
    const struct rp_prog *p = NULL;
    CHECK(rp_shader_resolve_compute(&p) == 0 && p && compiler_calls == 0 && cs_loads == 1,
          "aligned CS storage hits the library, compiler not called (calls=%d red=%s)", compiler_calls, last_red);
    CHECK(lp_unref_lines == 0, "aligned library hit does not log the unaligned-refuse line (got %d)", lp_unref_lines);
    int calls = compiler_calls, unref0 = lp_unref_lines;
    rp_S.ssbo[5][0].addr = 0x10001;   // 同键同绑定、只有对齐实况变：16 非对齐
    p = NULL;
    CHECK(rp_shader_resolve_compute(&p) == 0 && p && compiler_calls == calls + 1,
          "unaligned CS storage refuses the library, falls back to live compile (calls=%d)", compiler_calls);
    CHECK(strstr(captured, "\"hasUnalignedStorageBuffer\":true") != NULL,
          "unaligned CS compile request carries the unaligned fact (hardened variant)");
    {   char want[80]; snprintf(want, sizeof want, "libpack_cs_unaligned_refused 键 %s", khex);
        CHECK(lp_unref_lines == unref0 + 1 && strstr(last_unref, want) != NULL,
              "unaligned refuse logs one named line with the program key: %s", last_unref); }
    // 诊断：对齐闸的拒查原因进备忘（回放层 spill 派发行用）；日志前 8 条逐条（含身份）、第 9 次一行「已截断」说明、之后每 64 次一行带最近键与身份的汇总
    {   const char *note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "非对齐"), "unaligned refuse is remembered for the spill dispatch line: %s", note ? note : "(none)");
        CHECK(strstr(last_unref, "身份 ") != NULL, "unaligned refuse line carries the program identity: %s", last_unref); }
    for (int i = 0; i < 140; i++) { rp_shader_reset(); p = NULL; (void)rp_shader_resolve_compute(&p); }   // 同一程序再拒 140 次：共 141 次，第 64、128 次各一行汇总
    {   char want[80]; snprintf(want, sizeof want, "累计 128 次（最近一次：键 %s", khex);
        CHECK(lp_unref_sums == 2 && strstr(last_unref_sum, want) != NULL && strstr(last_unref_sum, "身份 ") != NULL,
              "141 refusals log two summary lines (64 / 128) with the latest key and identity (sums=%d): %s", lp_unref_sums, last_unref_sum); }
    rp_S.ssbo[5][0].addr = 0x10000;   // 回到对齐：库照常出
    p = NULL; calls = compiler_calls;
    CHECK(rp_shader_resolve_compute(&p) == 0 && p && compiler_calls == calls,
          "realigned CS storage uses the library again (calls=%d)", compiler_calls);
    CHECK(nvk_shader_cs_lib_note(rp_S.skey[5]) == NULL, "a library hit clears the remembered reason");
    // 共享内存闸：同一键换一份 spill 库件（Workgroup 45056 > 本机上限）→ 出库拒，备忘写「共享内存超设备上限」；库里没有这一键 → 「库里没有这一键」
    {   static const uint32_t wgspv[] = {
            0x07230203u, 0x00010000u, 0u, 7u, 0u,
            0x00020011u, 1u, 0x00040015u, 1u, 32u, 1u, 0x0004002bu, 1u, 2u, 11264u,
            0x0004001cu, 3u, 1u, 2u, 0x00040020u, 4u, 4u, 3u, 0x0004003bu, 4u, 5u, 4u,   // Workgroup u32[11264] = 45056 字节
        };
        char sjson[512];
        snprintf(sjson, sizeof sjson, "{\"key\":\"%s_b\",\"stage\":\"cs\",\"dm_ctl\":\"%s\",\"spill\":45056,\"spill_slot\":27,\"bindings\":["
                 "{\"msl_name\":\"cp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":0}]}", khex, khex);
        const uint8_t *sd[2] = { (const uint8_t *)sjson, (const uint8_t *)wgspv };
        size_t sn[2] = { strlen(sjson), sizeof wgspv };
        char sroot[256], mroot[256];
        CHECK(cs_lib_root(sroot, ck, ckind, sd, sn, 2) == 0, "spill library piece written (own root)");
        rp_shader_reset(); p = NULL;
        (void)rp_shader_resolve_compute(&p);
        const char *note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "共享内存"), "shared-memory gate refusal is remembered: %s", note ? note : "(none)");
        uint64_t other[2] = { ck[0] ^ 0x5555, ck[0] ^ 0x5555 };   // 库里只有别的键
        CHECK(cs_lib_root(mroot, other, ckind, cd, cn, 2) == 0, "library without this key written (own root)");
        rp_shader_reset(); p = NULL;
        (void)rp_shader_resolve_compute(&p);
        note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "没有这一键"), "missing key is remembered: %s", note ? note : "(none)");
        cs_lib_root_rm(sroot); cs_lib_root_rm(mroot); setenv("S2A_SHADER_REVISION_ROOT", root, 1); }
    // 原因按码分类（libpack_vk.h enum s2a_libpack_why），不按 why 文本：每一类各出一份真库件，核备忘归对类
    {   char rroot[256], oroot[256];
        // 反射 / 能力核拒：模块用 64 位浮点（OpCapability Float64 + OpTypeFloat 64）→ 出库拒（WHY_REFLECT）
        static const uint32_t f64spv[] = {
            0x07230203u, 0x00010000u, 0u, 2u, 0u,
            0x00020011u, 10u,                 // OpCapability Float64
            0x00030016u, 1u, 64u,             // %1 = OpTypeFloat 64
        };
        const uint8_t *rd[2] = { (const uint8_t *)csjson, (const uint8_t *)f64spv };
        size_t rn[2] = { strlen(csjson), sizeof f64spv };
        CHECK(cs_lib_root(rroot, ck, ckind, rd, rn, 2) == 0, "reflection-refused library piece written (own root)");
        rp_shader_reset(); p = NULL;
        CHECK(rp_shader_resolve_compute(&p) == 0 && p, "reflection-refused piece falls back to live compile");
        const char *note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "反射"), "SPIR-V capability refusal is remembered as a reflection refusal: %s", note ? note : "(none)");
        // 其它拒收：_b.json 的 dm_ctl 与键不符（WHY_OTHER，既不是反射也不是共享内存）→ 备忘「库件被拒收」且不带反射 / 共享内存字样
        char badjson[512];
        snprintf(badjson, sizeof badjson, "{\"key\":\"%s_b\",\"stage\":\"cs\",\"dm_ctl\":\"0000000000000000\",\"bindings\":["
                 "{\"msl_name\":\"cp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":0}]}", khex);
        const uint8_t *od[2] = { (const uint8_t *)badjson, (const uint8_t *)csspv };
        size_t on[2] = { strlen(badjson), sizeof csspv };
        CHECK(cs_lib_root(oroot, ck, ckind, od, on, 2) == 0, "other-refused library piece written (own root)");
        rp_shader_reset(); p = NULL;
        CHECK(rp_shader_resolve_compute(&p) == 0 && p, "other-refused piece falls back to live compile");
        note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "被拒收") && !strstr(note, "反射") && !strstr(note, "共享内存"),
              "other refusal is remembered as plain rejection: %s", note ? note : "(none)");
        cs_lib_root_rm(rroot); cs_lib_root_rm(oroot); setenv("S2A_SHADER_REVISION_ROOT", root, 1); }
    // 取到库件但 build 不过（129 个 cbuf 绑定超过程序条目上限）：记坏时留原因，备忘「库件 build 失败」；之后命中记坏表也按这个归类，
    //   不再留着上一次的状态（修前：对齐闸拒查留下「非对齐」后，命中记坏表直接返回，备忘不更新；命中库后清空的备忘则只剩「没有出库记录」）
    {   const char *note; char broot[256]; static char bjson[8192]; int o = snprintf(bjson, sizeof bjson, "{\"key\":\"%s_b\",\"stage\":\"cs\",\"dm_ctl\":\"%s\",\"bindings\":[", khex, khex);
        for (int i = 0; i < 129; i++)
            o += snprintf(bjson + o, sizeof bjson - (size_t)o, "%s{\"msl_name\":\"cp_c%d\",\"kind\":\"cbuf\",\"reg\":%d,\"arg_id\":%d}", i ? "," : "", i, i % 16, i);
        snprintf(bjson + o, sizeof bjson - (size_t)o, "]}");
        const uint8_t *bd[2] = { (const uint8_t *)bjson, (const uint8_t *)csspv };
        size_t bn[2] = { strlen(bjson), sizeof csspv };
        CHECK(cs_lib_root(broot, ck, ckind, bd, bn, 2) == 0, "library piece that cannot be built written (own root)");
        rp_shader_reset(); p = NULL; calls = compiler_calls;
        CHECK(rp_shader_resolve_compute(&p) == 0 && p && compiler_calls == calls + 1,
              "library piece that cannot be built falls back to live compile (calls=%d)", compiler_calls);
        note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "build 失败"), "a marked-bad library program is remembered as a build failure: %s", note ? note : "(none)");
        // 现在把备忘换成别的状态（非对齐闸拒查）：命中记坏表时必须改回「build 失败」，而不是留着「非对齐」或空
        rp_S.ssbo[5][0].addr = 0x10001;
        rp_shader_reset(); p = NULL; (void)rp_shader_resolve_compute(&p);
        note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "非对齐"), "unaligned refuse replaces the note: %s", note ? note : "(none)");
        rp_S.ssbo[5][0].addr = 0x10000;
        rp_shader_reset(); p = NULL; (void)rp_shader_resolve_compute(&p);
        note = nvk_shader_cs_lib_note(rp_S.skey[5]);
        CHECK(note && strstr(note, "build 失败"), "a bad-table hit is reported as a build failure, not as a stale reason or as no record: %s", note ? note : "(none)");
        cs_lib_root_rm(broot); setenv("S2A_SHADER_REVISION_ROOT", root, 1); }
    unsetenv("S2A_SHADER_REVISION_ROOT");
    char path[700];
    snprintf(path, sizeof path, "%s/lib.idx", dir); unlink(path); snprintf(path, sizeof path, "%s/lib.000.pak", dir); unlink(path);
    rmdir(dir); snprintf(dir, sizeof dir, "%s/shaders/direct", root); rmdir(dir); snprintf(dir, sizeof dir, "%s/shaders", root); rmdir(dir); rmdir(root);
    printf("nvk shader pool (libpack CS unaligned gate): %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
static int diag_compile_test(void) {
    trace_on = 1; trace_begin_count = trace_end_count = 0; fake_gs_success = 0;
    gs_request_test();
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset(); source(); bind_pools(7);
    trace_begin_count = trace_end_count = 0; fake_cs_success = 1;
    const struct rp_prog *p = NULL; CHECK(!rp_shader_resolve_compute(&p) && p, "compute diagnostic fixture compiles");
    uint8_t zero[32] = {0};
    CHECK(trace_begin_count == 1 && trace_end_count == 1 && trace_stage == 32 && trace_submit == 1 && trace_warm == 0 && trace_success == 1 && memcmp(trace_id, zero, 32),
          "compute live compile emits paired event with identity and stage");
    trace_on = 0; rp_shader_reset();
    printf("compile diagnostic: %d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}

static int live_compile_thread_test(int asynchronous) {
    const uint64_t before = rp_shader_compiled_programs();
    fixture_live_compile = asynchronous ? 1 : -1;   // explicit opt-in or an unset property
    fixture_live_slots = 2;
    CHECK(nvk_shader_open() == 0, "open shader session");
    struct s2a_compile_worker_stats slots;
    s2a_compile_worker_stats(&slots);
    const unsigned expected_slots = asynchronous ? 2 : 0;
    CHECK(slots.slots == expected_slots, "%s must start %u compiler workers (got %u)",
          asynchronous ? "explicit async opt-in" : "unset async property", expected_slots, slots.slots);
    rp_state_reset(); source(); bind_pools(64);
    fake_array_count = 64;
    fake_cs_success = 1;
    struct rp_shader_req q = {.prim = 4};
    const struct rp_prog *graphics = NULL, *compute = NULL, *again = NULL;
    int rc = rp_shader_resolve(&q, &graphics);
    CHECK(rc == 0 && graphics && !graphics->is_compute && compiler_calls == 1,
          "graphics first use publishes the real compiled program (rc=%d calls=%d)", rc, compiler_calls);
    if (compiler_calls == 1) {
        const int on_submit = pthread_equal(last_compiler_thread, pthread_self()) != 0;
        CHECK(on_submit == !asynchronous, "graphics live compiler %s on the submitting thread",
              on_submit ? "ran" : "did not run");
    }
    rc = rp_shader_resolve_compute(&compute);
    CHECK(rc == 0 && compute && compute->is_compute && compiler_calls == 2,
          "compute first use publishes the real compiled program (rc=%d calls=%d)", rc, compiler_calls);
    if (compiler_calls == 2) {
        const int on_submit = pthread_equal(last_compiler_thread, pthread_self()) != 0;
        CHECK(on_submit == !asynchronous, "compute live compiler %s on the submitting thread",
              on_submit ? "ran" : "did not run");
    }
    CHECK(rp_shader_resolve(&q, &again) == 0 && again == graphics && compiler_calls == 2,
          "second graphics lookup reuses the published program");
    CHECK(rp_shader_resolve_compute(&again) == 0 && again == compute && compiler_calls == 2,
          "second compute lookup reuses the published program");
    CHECK(rp_shader_compiled_programs() - before == 2, "two live programs across graphics, compute and their cache hits");
    nvk_shader_stop(); rp_shader_reset(); s2a_source_clear();
    printf("live-compile-%s checks=%d failures=%d\n", asynchronous ? "thread" : "default", checks, failures);
    return failures ? 1 : 0;
}

static int fixture_defer_frame(uint64_t *frame) {
    *frame = 1;
    return 1;
}

static int submit_compile_test(int slots, int variant) {
    fixture_submit_compile = 1;
    fake_reg_changes = 0;
    live_gate_hold = 1;
    fixture_submit_slots = slots;
    fixture_live_frame_ms = 0;
    fixture_live_unsafe_ms = 0;
    CHECK(nvk_shader_open() == 0, "open strict submit session");
    CHECK(rp_shader_lookahead_budget() == (unsigned)slots, "requested compile slots=%d", slots);
    struct s2a_compile_worker_stats worker;
    s2a_compile_worker_stats(&worker);
    CHECK(worker.slots == (unsigned)slots, "live worker slots=%u expected=%d", worker.slots, slots);
    rp_state_reset();
    source();
    bind_pools(64);
    fake_array_count = 64;
    struct rp_state state = rp_S;
    struct rp_shader_req q = {.prim = 4, .compile_defer_frame = fixture_defer_frame};
    uint8_t memory[4096];
    pool_t pool = {0};
    tex_t color = {.mem = memory, .pool = &pool};
    color.L.size = sizeof memory;
    p_set_rt targets = {.n = 0, .depth = &color};
    if (variant == 4) {
        fixture_parallel_compile = 1;
        for (int i = 0; i < slots; i++) {
            state.ubo[0][i].size = 4;
            CHECK(rp_shader_lookahead(&state, &q, &targets) == 1, "queued distinct immutable request %d", i);
        }
        CHECK(live_gate_wait(slots), "all %d workers entered before the first actual Draw", slots);
        state.ubo[0][slots].size = 4;
        CHECK(rp_shader_lookahead(&state, &q, &targets) < 0, "submit mask does not exceed the requested slots");
        live_gate_release();
        for (int i = 0; i < slots; i++) {
            rp_S.ubo[0][i].size = 4;
            const struct rp_prog *program = NULL;
            CHECK(!rp_shader_resolve(&q, &program) && program, "real draw %d took its future", i);
        }
        struct rp_shader_live_compile_stats stats;
        rp_shader_live_compile_take(&stats);
        CHECK(stats.compiled == (uint64_t)slots && stats.enqueued == (uint64_t)slots && !stats.pending && !stats.fallback,
              "only the requested workers compiled: expected=%d got=%llu", slots, (unsigned long long)stats.compiled);
        CHECK(shader_load_count == slots, "only %d shaders constructed, got %d", slots, shader_load_count);
        goto cleanup;
    }
    if (variant == 2) {
        // Alias (same physical bytes through different texture objects) terminates pre-scan.
        fixture_alias_texture = 1;
        fixture_alias_snapshot = color;
        CHECK(rp_shader_lookahead(&state, &q, &targets) < 0, "attachment alias is a boundary");
        fixture_alias_texture = 0;
        live_gate_release();
    } else {
        CHECK(rp_shader_lookahead(&state, &q, &targets) == 1, "offline immutable request enqueued");
        CHECK(rp_shader_lookahead(&state, &q, &targets) == 0, "duplicate draw joins a single future");
        CHECK(live_gate_wait(1), "provider actually started before Draw");
        if (variant == 1) rp_S.ubo[0][3].size = 4;  // Full identity differs despite the same source/RT.
        live_gate_release();
        if (variant == 1 || variant == 3) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 2;
            pthread_mutex_lock(&live_gate_mu);
            while (!shader_load_count)
                if (pthread_cond_timedwait(&live_gate_cv, &live_gate_mu, &deadline)) break;
            const int built = shader_load_count;
            pthread_mutex_unlock(&live_gate_mu);
            CHECK(built == 1, "first identity was actually built before the draw changed it");
            if (variant == 3) fixture_compile_generation = 7;
        }
    }
    const struct rp_prog *program = NULL;
    CHECK(rp_shader_resolve(&q, &program) == 0 && program, "Draw is never deferred at zero budget");
    struct rp_shader_live_compile_stats stats;
    rp_shader_live_compile_take(&stats);
    CHECK(stats.pending == 0 && stats.timeout == 0, "pending=%llu timeout=%llu",
          (unsigned long long)stats.pending, (unsigned long long)stats.timeout);
    if (!variant) {
        CHECK(compiler_calls == 1 && shader_load_count == 1 &&
              !pthread_equal(last_compiler_thread, pthread_self()) &&
              !pthread_equal(last_shader_load_thread, pthread_self()),
              "same future compiles/builds once on the worker: compile=%d module=%d", compiler_calls, shader_load_count);
        CHECK(stats.fallback == 0, "exact request used prefetched result");
    } else CHECK(stats.fallback == 1, "real changed or declined request runs synchronous fallback");
cleanup:
    rp_shader_lookahead_end();
    nvk_shader_stop();
    (void)s2a_compile_worker_stop_bounded(2000, NULL, 0);
    rp_shader_reset();
    s2a_source_clear();
    printf("submit-compile slots=%d variant=%d checks=%d failures=%d\n", slots, variant, checks, failures);
    return failures != 0;
}

static int live_compile_pending_test(int stop) {
    fixture_live_compile = 1;
    fixture_live_frame_ms = 0;
    fixture_live_unsafe_ms = stop ? 20 : 2000;
    live_gate_hold = 1;
    CHECK(nvk_shader_open() == 0, "open async session");
    rp_state_reset(); source(); bind_pools(64); fake_array_count = 64;
    struct rp_shader_req q = {.prim = 4, .compile_defer_frame = fixture_defer_frame};
    const struct rp_prog *p = NULL;
    CHECK(rp_shader_resolve(&q, &p) == 1 && !p, "exhausted safe budget must defer, with no program");
    CHECK(live_gate_wait(1), "compiler is still gated on worker");
    CHECK(rp_shader_resolve(&q, &p) == 1 && !p, "second use while pending must not publish a program");
    struct rp_shader_live_compile_stats stats;
    rp_shader_live_compile_take(&stats);
    CHECK(stats.enqueued == 1 && stats.pending == 2, "same pending request must enqueue once (enqueued=%llu pending=%llu)",
          (unsigned long long)stats.enqueued, (unsigned long long)stats.pending);
    CHECK(stats.compiled == 0 && stats.compile_ns == 0 && stats.compile_max_ns == 0,
          "compile still held in the worker: nothing counted as finished yet (compiled=%llu ns=%llu max=%llu)",
          (unsigned long long)stats.compiled, (unsigned long long)stats.compile_ns, (unsigned long long)stats.compile_max_ns);
    q.compile_defer_frame = NULL;
    if (stop) {
        CHECK(rp_shader_resolve(&q, &p) < 0 && !p, "unsafe hard deadline must report failure, not skip or publish");
        rp_shader_live_compile_take(&stats);
        CHECK(stats.timeout == 1 && stats.pending == 0, "unsafe deadline must count timeout separately");
        // Keep the active translator held while saturating the real worker queue.
        for (unsigned i = 0; i < S2A_CW_QUEUE_MAX; i++) {
            uint8_t filler_key[32] = {(uint8_t)i, 0xff};
            CHECK(s2a_compile_worker_submit_async(1, 1, "{}", 2, filler_key, NULL, 0) == 0,
                  "queue accepts filler %u while active compile is held", i);
        }
        fake_cs_success = 1;
        CHECK(rp_shader_resolve_compute(&p) < 0 && !p, "full queue must fail a bounded compute first use explicitly");
        rp_shader_live_compile_take(&stats);
        CHECK(stats.timeout == 1 && stats.pending == 0 && stats.enqueued == 0,
              "full queue must not count the rejected compute as queued or skippable");
        CHECK(nvk_shader_inflight() == 1, "waiting resolver gone but worker retains session admission");
        nvk_shader_stop();
        CHECK(nvk_shader_open() < 0, "must not reopen while retired compile is still in provider");
        live_gate_release();
        CHECK(s2a_compile_worker_stop_bounded(2000, NULL, 0) == 0, "stopped worker eventually retires");
        rp_shader_reset();
        CHECK(nvk_shader_open() == 0, "session may reopen after old worker quiesces");
        CHECK(rp_shader_resolve(&q, &p) == 0 && p && compiler_calls == 2,
              "completion from stopped session must not satisfy the same request after reopen");
        CHECK(rp_shader_resolve_compute(&p) == 0 && p && p->is_compute && compiler_calls == 3,
              "a compute first use refused by the full queue must be admitted on retry");
    } else {
        live_gate_release();
        CHECK(rp_shader_resolve(&q, &p) == 0 && p && compiler_calls == 1,
              "deferred result must be installed on re-use without recompiling");
        rp_shader_live_compile_take(&stats);
        CHECK(stats.compiled == 1 && stats.compile_ns > 0 && stats.compile_max_ns == stats.compile_ns,
              "one finished compile is counted once, its total equals its longest (compiled=%llu ns=%llu max=%llu)",
              (unsigned long long)stats.compiled, (unsigned long long)stats.compile_ns, (unsigned long long)stats.compile_max_ns);
        rp_shader_live_compile_take(&stats);
        CHECK(stats.compiled == 0 && stats.compile_ns == 0 && stats.compile_max_ns == 0, "reading the compile timing clears it");
    }
    nvk_shader_stop();
    CHECK(s2a_compile_worker_stop_bounded(2000, NULL, 0) == 0, "stop retires worker");
    rp_shader_reset(); s2a_source_clear();
    printf("live-compile-%s checks=%d failures=%d\n", stop ? "stop" : "pending", checks, failures);
    return failures ? 1 : 0;
}

// ---- 状态闸读数（nvk_shader.c lp_gate_*）----
// 出包预编库只在六条状态闸全部放行时才会被查；被挡下的程序以前在「程序累计」里完全看不见（库没查过、也不是库里没有），
// 只能看到现场编译很多。这里用 VS + FS 两段程序（0x3）+ 一份只含这两段的库，把每条闸的「挡下」与「放行后库里没有」分开读出来，
// 同时锁住取程序的决定没变：放行且库里有 → 命中（编译器不调）；放行但库里没有 → 现编；任何一条闸单独挡下 → 库里明明有也不查，现编。
// 单独起进程：直方图是进程累计的；深度能力缓存只读一次，这里按「设备没有 CAP_DEPTH_CLIP_CTL」定死，深度模式闸才测得到
// （基准 fixture_depth_zero = 1，深度零到一，不挡；改成 0 即 near=-w，被挡）。
static uint8_t gate_key_hit[8], gate_key_miss[8], gate_key_fs[8], gate_key_fsjson[8];   // fsjson：库里只有 _b.json、没有 _b.spv 的片元键
static uint8_t gate_key_vssb[8], gate_key_fssb[8];   // 库里有、且真用到存储缓冲槽的顶点键（槽 3）/ 片元键（槽 0）；基准的 vs / fs 键一个存储槽也不用
static uint8_t gate_key_fspsb[8];   // 库里有、_b.json 一个存储槽也不列、但 _b.spv 声明了 PhysicalStorageBufferAddresses（按地址直接访问内存）的片元键
static unsigned gate_alpha0, gate_src0;   // rp_state_reset 的默认值（位域，不能用 typeof）：每个用例结束时原样还原
static void gc_none(void) {}
static void gc_miss_on(void) { memcpy(rp_S.skey[0], gate_key_miss, 8); }   // 取程序读的是绑定时抄进 rp_S.skey 的键
static void gc_miss_off(void) { memcpy(rp_S.skey[0], gate_key_hit, 8); }
static void gc_depth_on(void) { fixture_depth_zero = 0; }
static void gc_depth_off(void) { fixture_depth_zero = 1; }
static void gc_origin_on(void) { fixture_origin_lower_left = 1; }
static void gc_origin_off(void) { fixture_origin_lower_left = 0; }
static void gc_alpha_on(void) { rp_S.color.alpha_func = 2; }
static void gc_alpha_off(void) { rp_S.color.alpha_func = gate_alpha0; }
static void gc_a2c_on(void) { rp_S.ms.a2c = 1; }
static void gc_a2c_off(void) { rp_S.ms.a2c = 0; }
static void gc_dual_on(void) { rp_S.blend[0].src_rgb = 0x10; }
static void gc_dual_off(void) { rp_S.blend[0].src_rgb = gate_src0; }
static void gc_ssbo_set(unsigned st, unsigned slot, uint64_t addr) { rp_S.ssbo[st][slot].addr = addr; rp_S.ssbo[st][slot].size = addr ? 16 : 0; }   // 夹具 storage_buffer_offset_alignment = 16：0x…0 对齐，0x…1 非对齐
static void gc_ssbo_on(void) { gc_ssbo_set(1, 0, 0x10001); }   // 片元段槽 0 绑了非对齐地址；基准程序一个存储槽也不用（旧口径下这就足以挡住库）
static void gc_ssbo_off(void) { gc_ssbo_set(1, 0, 0); }
static void gc_fssb_key(int on) { memcpy(rp_S.skey[1], on ? gate_key_fssb : gate_key_fs, 8); }
static void gc_vssb_key(int on) { memcpy(rp_S.skey[0], on ? gate_key_vssb : gate_key_hit, 8); }
static void gc_fssb_un_on(void) { gc_fssb_key(1); gc_ssbo_set(1, 0, 0x10001); }   // 片元程序用槽 0，槽 0 非对齐：真受影响
static void gc_fssb_un_off(void) { gc_fssb_key(0); gc_ssbo_set(1, 0, 0); }
static void gc_fssb_al_on(void) { gc_fssb_key(1); gc_ssbo_set(1, 0, 0x10000); }   // 片元程序用槽 0，槽 0 对齐
static void gc_fssb_stage_on(void) { gc_fssb_key(1); gc_ssbo_set(1, 0, 0x10000); gc_ssbo_set(0, 0, 0x30001); }   // 片元用槽 0（对齐）；顶点段槽 0 非对齐，但顶点程序不用存储
static void gc_fssb_stage_off(void) { gc_fssb_key(0); gc_ssbo_set(1, 0, 0); gc_ssbo_set(0, 0, 0); }
static void gc_vssb_un_on(void) { gc_vssb_key(1); gc_ssbo_set(0, 3, 0x20001); }   // 顶点程序用槽 3，槽 3 非对齐：真受影响
static void gc_vssb_un_off(void) { gc_vssb_key(0); gc_ssbo_set(0, 3, 0); }
static void gc_vssb_slot_on(void) { gc_vssb_key(1); gc_ssbo_set(0, 3, 0x20000); gc_ssbo_set(0, 0, 0x20001); }   // 顶点程序用槽 3（对齐）；同一阶段的槽 0 非对齐，但没用
static void gc_vssb_slot_off(void) { gc_vssb_key(0); gc_ssbo_set(0, 3, 0); gc_ssbo_set(0, 0, 0); }
// 顶点程序用槽 3、片元程序用槽 0；非对齐的恰好是顶点段的槽 0 和片元段的槽 3——两段各算各的，谁也没碰到非对齐的槽（把两段的槽并起来看就会误挡）
static void gc_cross_on(void) { gc_vssb_key(1); gc_fssb_key(1); gc_ssbo_set(0, 0, 0x20001); gc_ssbo_set(1, 3, 0x10001); }
static void gc_cross_off(void) { gc_vssb_key(0); gc_fssb_key(0); gc_ssbo_set(0, 0, 0); gc_ssbo_set(1, 3, 0); }
static void gc_miss_ssbo_on(void) { gc_miss_on(); gc_ssbo_on(); }   // 顶点键在库里没有 + 片元段有一个非对齐绑定（顶点段的绑定全对齐：它用哪些槽都不受影响，不必读它）
static void gc_miss_ssbo_off(void) { gc_miss_off(); gc_ssbo_off(); }
static void gc_miss_vsun_on(void) { gc_miss_on(); gc_ssbo_set(0, 0, 0x20001); }   // 顶点键在库里没有（读不出用到哪几个槽）+ 顶点段自己有一个非对齐绑定：必须读它，读不出就不猜
static void gc_miss_vsun_off(void) { gc_miss_off(); gc_ssbo_set(0, 0, 0); }
static void gc_fspsb_key(int on) { memcpy(rp_S.skey[1], on ? gate_key_fspsb : gate_key_fs, 8); }
static void gc_fspsb_un_on(void) { gc_fspsb_key(1); gc_ssbo_set(1, 0, 0x10001); }   // 片元程序的 SPIR-V 带按地址访问内存的能力、片元段有非对齐绑定：绑定表不代表它碰得到的全部存储
static void gc_fspsb_un_off(void) { gc_fspsb_key(0); gc_ssbo_set(1, 0, 0); }
static void gc_fspsb_al_on(void) { gc_fspsb_key(1); gc_ssbo_set(1, 0, 0x10000); }   // 同一份程序、绑定全对齐：不涉及非对齐，照常取库
static void gc_flagoff_ssbo_on(void) { fixture_used_slots = 0; gc_ssbo_on(); }   // 开关显式设 0：退回旧口径（任何一个非对齐绑定都挡库）
static void gc_flagoff_ssbo_off(void) { fixture_used_slots = 1; gc_ssbo_off(); }   // 还原成整个用例表的底色（显式设 1）
static void gc_default_ssbo_on(void) { fixture_used_slots = -1; gc_ssbo_on(); }   // 属性没设：读生产缺省（关）= 旧口径；把缺省改回开，这一行就会放行、命中库而变红
static void gc_default_ssbo_off(void) { fixture_used_slots = 1; gc_ssbo_off(); }
static void gc_alpha_ssbo_on(void) { gc_alpha_on(); gc_ssbo_on(); }   // 透明测试挡下 + 基准程序没用的槽非对齐
static void gc_alpha_ssbo_off(void) { gc_alpha_off(); gc_ssbo_off(); }
static void gc_alpha_a2c_on(void) { gc_alpha_on(); gc_a2c_on(); }
static void gc_alpha_a2c_off(void) { gc_alpha_off(); gc_a2c_off(); }
static void gc_alpha_miss_on(void) { gc_alpha_on(); gc_miss_on(); }
static void gc_alpha_miss_off(void) { gc_alpha_off(); gc_miss_off(); }
static void gc_alpha_fsjson_on(void) { gc_alpha_on(); memcpy(rp_S.skey[1], gate_key_fsjson, 8); }
static void gc_alpha_fsjson_off(void) { gc_alpha_off(); memcpy(rp_S.skey[1], gate_key_fs, 8); }
// 读数行里「判定读库 N 次、命中缓存 M 次」的两个数（行里没有就给 0）
static void ssbo_cost_counts(unsigned long long *reads, unsigned long long *hits) {
    extern void nvk_shader_test_ssbo_line(char *out, size_t n);
    char b[800]; nvk_shader_test_ssbo_line(b, sizeof b);
    const char *p = strstr(b, "判定读库 ");
    *reads = *hits = 0;
    if (p) sscanf(p, "判定读库 %llu 次、命中缓存 %llu 次", reads, hits);
}
static int lp_gate_test(void) {
    extern void nvk_shader_test_gate_line(char *out, size_t n);
    extern void nvk_shader_test_gate_probe_line(char *out, size_t n);
    extern void nvk_shader_test_ssbo_line(char *out, size_t n);
    fake_depth_clip = 0; fixture_depth_zero = 1; fixture_origin_lower_left = 0; fake_geometry_enabled = 1;
    fixture_used_slots = 1;   // 开关缺省关之后，整张用例表按「调试包设了 nvk_lp_used_slots=1」跑；只有「属性没设」「显式设 0」两行改成读缺省 / 读 0
    rp_shader_reset(); s2a_source_clear(); rp_state_reset(); nvn_unimpl_reset();
    gate_alpha0 = rp_S.color.alpha_func; gate_src0 = rp_S.blend[0].src_rgb;
    uint8_t k[3][8]; for (unsigned st = 0; st < 3; st++) gs_source(st, 0, k[st]);
    memcpy(gate_key_hit, k[0], 8); gs_source(0, 1, gate_key_miss);   // 库里没有的 VS 键：变体 1 的顶点来源
    memcpy(gate_key_fs, k[1], 8); gs_source(1, 1, gate_key_fsjson);   // 库里只有 json 的 FS 键：变体 1 的片元来源
    gs_source(0, 2, gate_key_vssb); gs_source(1, 2, gate_key_fssb);   // 库里有、真用到存储缓冲槽的 VS / FS 键：变体 2（顶点槽 3 / 片元槽 0）
    gs_source(1, 3, gate_key_fspsb);   // 库里有、_b.spv 带按地址访问内存的能力的 FS 键：变体 3
    memset(&gs_program, 0, sizeof gs_program); gs_program.magic = MAGIC('P','R','O','G'); gs_program.serial = 101; gs_program.nstage = 3;
    memset(gs_stages, 0xff, sizeof gs_stages); memset(gs_keys, 0, sizeof gs_keys);
    for (unsigned i = 0; i < 3; i++) { gs_stages[i] = (uint8_t)i; memcpy(gs_keys[i], k[i], 8); }
    gs_keys_ok = 7; p_bind_program_id bind = {&gs_program, gs_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "gate fixture binds program");
    rp_S.bser[2] = 0;   // 去掉几何段：VS + FS 两段
    CHECK(rp_shader_graphics_stages() == 3, "gate fixture stage mask is 0x3");
    bind_pools(7);
    char root[256], dir[600];
    snprintf(root, sizeof root, "%s/nvk_lp_gate.XXXXXX", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { CHECK(0, "gate temp root"); return 1; }
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    static const uint32_t spv0[5] = {0x07230203u, 0x00010300u, 0, 1, 0};
    static const uint32_t spv_psb[7] = {0x07230203u, 0x00010300u, 0, 1, 0, 0x00020011u, 5347u};   // 头 + OpCapability PhysicalStorageBufferAddresses
    char hx[2][17]; for (int st = 0; st < 2; st++) for (int b = 0; b < 8; b++) snprintf(hx[st] + 2 * b, 3, "%02x", k[st][b]);
    char hxj[17]; for (int b = 0; b < 8; b++) snprintf(hxj + 2 * b, 3, "%02x", gate_key_fsjson[b]);
    char hxvsb[17], hxfsb[17], hxfpsb[17];
    for (int b = 0; b < 8; b++) { snprintf(hxvsb + 2 * b, 3, "%02x", gate_key_vssb[b]); snprintf(hxfsb + 2 * b, 3, "%02x", gate_key_fssb[b]); snprintf(hxfpsb + 2 * b, 3, "%02x", gate_key_fspsb[b]); }
    char jvs[160], jfs[160], jfj[160], jvsb[320], jfsb[320], jfpsb[160];
    snprintf(jfpsb, sizeof jfpsb, "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hxfpsb);   // 绑定表里一个存储槽也没有；存储访问全靠地址
    snprintf(jvs, sizeof jvs, "{\"stage\":\"vs\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hx[0]);
    snprintf(jfs, sizeof jfs, "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hx[1]);
    snprintf(jfj, sizeof jfj, "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hxj);
    snprintf(jvsb, sizeof jvsb, "{\"stage\":\"vs\",\"dm_ctl\":\"%s\",\"bindings\":[{\"msl_name\":\"vp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":3,\"rw\":true,\"arg_id\":0}]}", hxvsb);   // 顶点段用存储槽 3
    snprintf(jfsb, sizeof jfsb, "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[{\"msl_name\":\"fp_s0\",\"kind\":\"sbuf\",\"index\":0,\"reg\":0,\"rw\":true,\"arg_id\":0}]}", hxfsb);   // 片元段用存储槽 0
    struct { uint64_t key; uint8_t kind; const uint8_t *d; size_t n; } rec[11] = {
        {lp_key(k[0]), 1, (const uint8_t *)jvs, strlen(jvs)}, {lp_key(k[0]), 52, (const uint8_t *)spv0, sizeof spv0},
        {lp_key(k[1]), 1, (const uint8_t *)jfs, strlen(jfs)}, {lp_key(k[1]), 52, (const uint8_t *)spv0, sizeof spv0},
        {lp_key(gate_key_fsjson), 1, (const uint8_t *)jfj, strlen(jfj)},   // 只有 json、没有 spv：探测要两样都在才算「库里有」
        {lp_key(gate_key_vssb), 1, (const uint8_t *)jvsb, strlen(jvsb)}, {lp_key(gate_key_vssb), 52, (const uint8_t *)spv0, sizeof spv0},
        {lp_key(gate_key_fssb), 1, (const uint8_t *)jfsb, strlen(jfsb)}, {lp_key(gate_key_fssb), 52, (const uint8_t *)spv0, sizeof spv0},
        {lp_key(gate_key_fspsb), 1, (const uint8_t *)jfpsb, strlen(jfpsb)}, {lp_key(gate_key_fspsb), 52, (const uint8_t *)spv_psb, sizeof spv_psb},   // 绑定表里没有存储槽、SPIR-V 带 PhysicalStorageBufferAddresses
    };
    for (int i = 0; i < 11; i++) for (int j = i + 1; j < 11; j++)   // (键, 种类) 升序
        if (rec[j].key < rec[i].key || (rec[j].key == rec[i].key && rec[j].kind < rec[i].kind)) { __typeof__(rec[0]) t = rec[i]; rec[i] = rec[j]; rec[j] = t; }
    uint64_t keys[11]; uint8_t kinds[11]; const uint8_t *data[11]; size_t lens[11];
    for (int i = 0; i < 11; i++) { keys[i] = rec[i].key; kinds[i] = rec[i].kind; data[i] = rec[i].d; lens[i] = rec[i].n; }
    CHECK(lp_write(dir, keys, kinds, data, lens, 11) == 0, "gate container written");
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
    static const char prof_on[] = "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16,\"supportsGeometryShader\":true}}";
    snprintf(external_profile, sizeof external_profile, "%s", prof_on);
    fake_gs_success = 1; fake_gs_result_mode = 1;   // 现编结果只含 vs + fs，与两段程序对得上
    // 每行：状态（设置 / 还原）、份数、期望是否从库里取、这一行是否开只读探测（nvk_gate_probe）、现编时请求里 hasUnalignedStorageBuffer 的期望值（-1 = 命中库、没有现编请求，不核）。
    //   份数是手算的，直方图的期望从这张表算出（见下面 want）。
    static const struct { const char *name; void (*on)(void); void (*off)(void); int n, hit, probe, un; } cases[] = {
        {"allowed, in library",   gc_none,         gc_none,          1, 1, 1, -1},
        {"allowed, not in library", gc_miss_on,    gc_miss_off,      2, 0, 1, 0},
        {"alpha test, probe off", gc_alpha_on,     gc_alpha_off,     1, 0, 0, 0},   // 探测缺省关：被挡下、照样现编，但不探
        {"depth mode",            gc_depth_on,     gc_depth_off,     2, 0, 1, 0},
        {"origin lower-left",     gc_origin_on,    gc_origin_off,    1, 0, 1, 0},
        {"alpha test",            gc_alpha_on,     gc_alpha_off,     2, 0, 1, 0},
        {"alpha test, not in library", gc_alpha_miss_on, gc_alpha_miss_off, 1, 0, 1, 0},   // 被挡下、库里本来也没有
        {"alpha test, library has only the json", gc_alpha_fsjson_on, gc_alpha_fsjson_off, 1, 0, 1, 0},   // 被挡下、片元键在库里只有 _b.json
        {"alpha to coverage",     gc_a2c_on,       gc_a2c_off,       1, 0, 1, 0},
        {"dual source blend",     gc_dual_on,      gc_dual_off,      1, 0, 1, 0},
        // 存储缓冲非对齐：只有「这个程序真用到的槽」非对齐才算（口径见 nvk_shader.c gfx_ssbo_align）。绑定在没用到的槽上，不影响程序的代码：库照取，现编也不当非对齐编
        {"unaligned storage, slot the program does not use", gc_ssbo_on, gc_ssbo_off, 2, 1, 1, -1},   // 基准程序一个存储槽也不用：以前就被挡下（旧口径），现在放行、命中
        {"alpha test + coverage", gc_alpha_a2c_on, gc_alpha_a2c_off, 1, 0, 1, 0},
        {"unaligned storage, slot the program uses (fs)", gc_fssb_un_on, gc_fssb_un_off, 2, 0, 1, 1},   // 真用到的槽非对齐：照旧挡下、现编（按非对齐编）；库里有这个程序，探测算「有」
        {"aligned storage, slot the program uses (fs)", gc_fssb_al_on, gc_fssb_un_off, 1, 1, 1, -1},   // 真用到的槽、但对齐：放行、命中（旧口径下别处没有非对齐也一样放行）
        {"unaligned storage, only in another stage", gc_fssb_stage_on, gc_fssb_stage_off, 1, 1, 1, -1},   // 非对齐的槽在顶点段，顶点程序不用存储；片元程序用槽 0 且对齐 → 放行（两段各算各的）
        {"unaligned storage, slot the program uses (vs)", gc_vssb_un_on, gc_vssb_un_off, 1, 0, 1, 1},
        {"unaligned storage, another slot of the same stage", gc_vssb_slot_on, gc_vssb_slot_off, 1, 1, 1, -1},   // 顶点程序用槽 3（对齐），同阶段槽 0 非对齐但没用 → 放行（逐槽算，不是整阶段）
        {"unaligned storage, crossed between stages", gc_cross_on, gc_cross_off, 1, 1, 1, -1},   // 顶点用槽 3、片元用槽 0；非对齐的是顶点槽 0 与片元槽 3：两段各算各的，没有一段碰到自己的非对齐槽 → 放行
        {"unaligned storage, program not in library", gc_miss_vsun_on, gc_miss_vsun_off, 1, 0, 1, 1},   // 非对齐绑定在顶点段、顶点键库里没有 → 读不出用到哪几个槽 → 不猜，照旧按非对齐：挡下，现编也按非对齐编（探测：库里本来也没有）
        {"unaligned storage, switch off", gc_flagoff_ssbo_on, gc_flagoff_ssbo_off, 1, 0, 1, 1},   // debug.s2a.nvk_lp_used_slots=0：退回旧口径，基准程序也被挡下、按非对齐编
        {"unaligned storage, property not set (production default)", gc_default_ssbo_on, gc_default_ssbo_off, 1, 0, 1, 1},   // 属性没设：生产缺省是关 = 旧口径，基准程序也被挡下、按非对齐编（缺省改回开会放行命中库 → 这一行红）
        {"alpha test, unaligned slot the program does not use", gc_alpha_ssbo_on, gc_alpha_ssbo_off, 1, 0, 1, 0},   // 被别的闸挡下、现编；没用到的槽非对齐不改变现编结果（同一份现编结果不再因旧绑定分成两份）
        {"unaligned storage, stage without unaligned binding is not in library", gc_miss_ssbo_on, gc_miss_ssbo_off, 1, 0, 1, 0},   // 顶点键库里没有，但顶点段的绑定全对齐（只有片元段有非对齐、且片元程序没用）→ 不必读顶点段：按对齐，现编也按对齐编
        {"unaligned storage, program declares physical addresses", gc_fspsb_un_on, gc_fspsb_un_off, 1, 0, 1, 1},   // 片元 SPIR-V 带 PhysicalStorageBufferAddresses：绑定表不代表它碰得到的全部存储 → 退回旧口径：挡下，现编按非对齐编（探测：库里有）
        {"aligned storage, program declares physical addresses", gc_fspsb_al_on, gc_fspsb_un_off, 1, 1, 1, -1},   // 同一份程序、绑定全对齐：没有非对齐可言，照常取库（负对照：这条能力本身不挡库）
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) for (int i = 0; i < cases[c].n; i++) {
        rp_shader_reset(); compiler_calls = 0; captured[0] = 0;
        cases[c].on(); fixture_gate_probe = cases[c].probe;
        struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
        int rc = rp_shader_resolve(&q, &p);
        cases[c].off(); fixture_gate_probe = 0;
        CHECK(rc == 0 && p, "gate case '%s' #%d: program resolves (rc=%d)", cases[c].name, i, rc);
        if (cases[c].hit) CHECK(compiler_calls == 0 && p && p->lib_sourced, "gate case '%s': hit from the library, compiler not called (calls=%d)", cases[c].name, compiler_calls);
        else CHECK(compiler_calls == 1 && p && !p->lib_sourced, "gate case '%s' #%d: library not used, live compile runs once (calls=%d)", cases[c].name, i, compiler_calls);
        if (!cases[c].hit && cases[c].un >= 0)
            CHECK(strstr(captured, cases[c].un ? "\"hasUnalignedStorageBuffer\":true" : "\"hasUnalignedStorageBuffer\":false") != NULL,
                  "gate case '%s' #%d: live compile request carries hasUnalignedStorageBuffer=%s", cases[c].name, i, cases[c].un ? "true" : "false");
    }
    // 期望（手算）：新程序 放行 = 基准 1 + 库里没有 2 + 没用到的槽非对齐 2 + 用到的槽对齐 1 + 非对齐在别的阶段 1 + 同阶段别的槽非对齐 1
    //          + 两段的非对齐槽恰好各是对方用到的槽 1
    //          + 顶点键库里没有、但非对齐的绑定只在片元段且片元程序没用 1 + 带按地址访问内存能力的程序、绑定全对齐 1 = 11；
    //   挡下 = 透明测试 1+2+1+1 + 深度模式 2 + 窗口原点左下 1 + 透明转覆盖 1 + 双源混合 1 + 透明测试+透明转覆盖 1（= 11）
    //          + 存储缓冲非对齐：用到的槽 片元 2 + 顶点 1、顶点键库里没有且顶点段有非对齐 1、开关显式设 0 的 1、属性没设（生产缺省关）的 1、带按地址访问内存的能力且有非对齐 1（= 7）
    //          + 透明测试且没用到的槽非对齐 1 = 19；现场编 放行 = 3（库里没有的那两份 + 顶点键库里没有但顶点段绑定全对齐的那份），挡下 = 19。
    //   按「挡下」份数前五名：存储缓冲不对齐 7；透明测试 6（探测关的 1 + 开的 2 + 库里没有的 1 + 库里只有 json 的 1 + 没用到的槽非对齐的 1）；
    //   深度模式 2；窗口原点左下 1 与 透明转覆盖 1（并列，位集合小的在前；双源混合 1、透明测试+透明转覆盖 1 排在第六、第七，不列）。
    static const char want[] = "出包预编库状态闸：新程序 放行 11、挡下 19（最多的：存储缓冲不对齐 7、透明测试 6、深度模式 2、窗口原点左下 1、透明转覆盖 1）"
                               "；现场编 放行 3、挡下 19（最多的：存储缓冲不对齐 7、透明测试 6、深度模式 2、窗口原点左下 1、透明转覆盖 1）";
    char got[640]; nvk_shader_test_gate_line(got, sizeof got);
    CHECK(!strcmp(got, want), "gate readout line\n  got : %s\n  want: %s", got, want);
    // 探测读数（手算）：只算开了探测的被挡下份数 = 19 - 1（探测关的那一份）= 18；库里有这几段（_b.json 与 _b.spv 都在）的 = 18 - 3 = 15
    //   （透明测试：库里没有的 1、只有 json 的 1；存储缓冲不对齐：库里没有的 1；带按地址访问内存能力的那份两样都在，算「有」；属性没设的那份基准程序在库里，算「有」）。
    //   按闸：存储缓冲不对齐 探 7（片元 2 + 顶点 1 + 库里没有 1 + 开关显式设 0 的 1 + 属性没设的 1 + 带按地址访问内存能力 1）有 6；透明测试 探 5（开的 2 + 库里没有的 1 + 只有 json 的 1
    //   + 没用到的槽非对齐的 1）有 3（份数多的在前：存储缓冲不对齐在前）；深度模式 2/2；窗口原点左下 1/1、透明转覆盖 1/1（并列，位集合小的在前）。
    static const char wantp[] = "出包预编库放开状态闸会怎样（只读探测）：被挡下又现场编的 18 份里，库里有这几段记录的 15 份"
                                "（按闸 有/共：存储缓冲不对齐 6/7、透明测试 3/5、深度模式 2/2、窗口原点左下 1/1、透明转覆盖 1/1）";
    char gotp[640]; nvk_shader_test_gate_probe_line(gotp, sizeof gotp);
    CHECK(!strcmp(gotp, wantp), "gate probe line\n  got : %s\n  want: %s", gotp, wantp);
    // 存储缓冲非对齐读数（手算）：只有「旧口径说有非对齐」且开关开着的新程序才记（开关显式设 0 的那份、属性没设（缺省关）的那份都不记，也不读库）。
    //   没用到非对齐的槽、按对齐放行 7（基准程序的片元槽 0 非对齐 2 份 + 非对齐在别的阶段 1 + 同阶段别的槽 1 + 两段交叉 1 + 透明测试那份 1
    //   + 顶点键库里没有但顶点段绑定全对齐 1）；用到了、照旧按非对齐 3（片元 2 + 顶点 1）；
    //   库里说不准、退回旧口径 2（顶点键库里没有且顶点段有非对齐 1 + SPIR-V 带按地址访问内存的能力 1，后一份单列）；合计 12。
    //   判定读库（手算）：每份新程序前都清了缓存，所以每份新程序读一次它「真有非对齐绑定的阶段」的库——没有非对齐绑定的阶段不读：
    //   没用到的槽 2 + 用到的槽（片元）2 + 非对齐在别的阶段 1（只读顶点段）+ 用到的槽（顶点）1 + 同阶段别的槽 1 + 两段交叉 2（顶点段、片元段各读一次）
    //   + 顶点键库里没有 1 + 透明测试那份 1 + 顶点键库里没有但顶点段全对齐 1（只读片元段）+ 带按地址访问内存的能力 1 = 13 次。
    static const char wants[] = "存储缓冲非对齐（只看程序真用到的槽）：新程序里绑定有非对齐的 12 份：程序没用到非对齐的槽、按对齐放行 7、用到了、照旧按非对齐 3、"
                                "库里说不准、退回旧口径按非对齐 2（其中 SPIR-V 带按地址访问内存的能力 1）；判定读库 13 次、命中缓存 ";
    char gots[800]; nvk_shader_test_ssbo_line(gots, sizeof gots);
    CHECK(!strncmp(gots, wants, sizeof wants - 1), "ssbo readout line\n  got : %s\n  want: %s…", gots, wants);
    {   // 句子的后半：命中缓存几次（至少每份新程序 1 次——快路摘要先读库、程序身份再查缓存）、读库合计耗时要有、平均 / 最长是数
        unsigned long long hits = 0, ms = 0, us = 0, avg = 0, mx = 0;
        CHECK(!strncmp(gots, wants, sizeof wants - 1) &&
              sscanf(gots + sizeof wants - 1, "%llu 次，读库合计 %llu.%llu 毫秒、平均每份新程序 %llu 微秒、最长一次 %llu 微秒", &hits, &ms, &us, &avg, &mx) == 5,
              "ssbo readout line: cost part parses\n  got : %s", gots);
        CHECK(hits >= 12, "ssbo readout line: cache hits cover every new program (hits=%llu)", hits);
        CHECK(ms * 1000 + us > 0, "ssbo readout line: library read time is recorded (%llu.%03llu ms)", ms, us);
    }
    // 探测只读：库自己的命中 / 没有 / 坏三个计数只来自真正的取程序（命中 8 = 基准 1 + 没用到的槽非对齐 2 + 用到的槽对齐 1 + 别的阶段 1 + 同阶段别的槽 1
    //   + 两段交叉 1 + 带按地址访问内存的能力但绑定全对齐 1；没有 3 = 库里没有 VS 键的那两份 + 顶点键库里没有但顶点段全对齐的那份；被挡下的一份都没查过库，
    //   库里没有 VS 键又非对齐的那份、带按地址访问内存的能力又非对齐的那份也被挡在查库之前）。
    //   命中 8 里同一组阶段键的重取由库件结果备忘接住（nvk_shader.c lp_memo_*），不再读库：库自己的命中 + 备忘命中 = 8。
    {
        extern void nvk_shader_test_tally_line(char *out, size_t n);
        char tl[640]; nvk_shader_test_tally_line(tl, sizeof tl);
        const char *mh = strstr(tl, "库件备忘 命中 ");
        const unsigned long long memo = mh ? strtoull(mh + strlen("库件备忘 命中 "), NULL, 10) : 0;
        CHECK(mh && s2a_libpack_hits() + memo == 8 && s2a_libpack_misses() == 3 && s2a_libpack_rejects() == 0,
              "gate probe leaves the library counters alone (hits=%llu + memo=%llu, misses=%llu rejects=%llu)",
              (unsigned long long)s2a_libpack_hits(), memo, (unsigned long long)s2a_libpack_misses(), (unsigned long long)s2a_libpack_rejects());
    }
    // 基准状态再来一份新程序（放行、库里有 → 命中）：只有「新程序 放行」加一，现场编不动；同一份已发布的程序再解析一遍，读数不动。
    {
        struct rp_shader_req q = {0}; q.prim = 4; const struct rp_prog *p = NULL;
        rp_shader_reset(); compiler_calls = 0;
        CHECK(rp_shader_resolve(&q, &p) == 0 && p && compiler_calls == 0, "gate: one more baseline program comes from the library (calls=%d)", compiler_calls);
        char once[640]; nvk_shader_test_gate_line(once, sizeof once);
        CHECK(strstr(once, "新程序 放行 12、挡下 19（") && strstr(once, "；现场编 放行 3、挡下 19（"),
              "gate readout: one more allowed snapshot, no more live compiles\n  %s", once);
        CHECK(rp_shader_resolve(&q, &p) == 0 && p, "gate: a published program resolves again");
        char again[640]; nvk_shader_test_gate_line(again, sizeof again);
        CHECK(!strcmp(again, once), "gate readout does not move for an already published program\n  was: %s\n  now: %s", once, again);
        char againp[640]; nvk_shader_test_gate_probe_line(againp, sizeof againp);
        CHECK(!strcmp(againp, wantp), "gate probe readout does not move for allowed programs\n  now: %s", againp);
    }
    {   // 快路摘要（input_sig）与程序身份用同一个答案：同一份着色器、同一份库，绑定在「对齐 / 非对齐」之间来回翻，结果必须跟着「程序真用到的槽」走。
        //   快路若不看这个事实，翻转后会把「按对齐翻的」库件原样交给一个非对齐的绑定（画出来是错的）。
        struct rp_shader_req q = {0}; q.prim = 4;
        const struct rp_prog *p0 = NULL, *p1 = NULL, *pa = NULL, *pb = NULL, *pc = NULL, *pd = NULL;
        rp_shader_reset(); compiler_calls = 0;
        unsigned long long reads0 = 0, hits0 = 0; ssbo_cost_counts(&reads0, &hits0);
        CHECK(rp_shader_resolve(&q, &p0) == 0 && p0 && p0->lib_sourced && compiler_calls == 0, "quick path: baseline program comes from the library");
        gc_ssbo_on();   // 程序没用到的槽翻成非对齐：身份不变，不再编、也不换程序
        CHECK(rp_shader_resolve(&q, &p1) == 0 && p1 == p0 && compiler_calls == 0,
              "quick path: an unused slot turning unaligned keeps the same program and compiles nothing (calls=%d)", compiler_calls);
        gc_ssbo_off();
        gc_fssb_al_on();   // 片元程序换成用槽 0 的那份，槽 0 对齐：库件
        CHECK(rp_shader_resolve(&q, &pa) == 0 && pa && pa->lib_sourced && compiler_calls == 0, "quick path: used slot aligned -> library program (calls=%d)", compiler_calls);
        gc_ssbo_set(1, 0, 0x10001);   // 同一个用到的槽翻成非对齐：不能再给库件（库件假设对齐），要现编
        CHECK(rp_shader_resolve(&q, &pb) == 0 && pb && pb != pa && !pb->lib_sourced && compiler_calls == 1,
              "quick path: used slot turning unaligned -> a different, live-compiled program, not the stale library one (calls=%d)", compiler_calls);
        gc_ssbo_set(1, 0, 0x10000);   // 翻回对齐：回到原来的库件，不再编
        CHECK(rp_shader_resolve(&q, &pc) == 0 && pc == pa && compiler_calls == 1, "quick path: back to aligned -> the library program again, no new compile (calls=%d)", compiler_calls);
        gc_ssbo_set(1, 0, 0x10001);   // 再翻非对齐：回到现编那份，不再编
        CHECK(rp_shader_resolve(&q, &pd) == 0 && pd == pb && compiler_calls == 1, "quick path: unaligned again -> the live-compiled program again, no new compile (calls=%d)", compiler_calls);
        {   // 判定按阶段键缓存：到这里只读过两个阶段键（片元基准键、片元用存储槽 0 的键）的库，来回翻了好几次绑定都只是查缓存；
            //   再给同一个阶段键换几种绑定状态（程序身份与快路摘要各不相同），读库次数仍不动
            unsigned long long reads1 = 0, hits1 = 0; ssbo_cost_counts(&reads1, &hits1);
            CHECK(reads1 - reads0 == 2, "stage-key cache: two stage keys read the library once each, however often the binding flipped (reads +%llu)", reads1 - reads0);
            CHECK(hits1 > hits0, "stage-key cache: the flips were answered from the cache (hits +%llu)", hits1 - hits0);
            const unsigned sets[4][2] = { {0, 1}, {2, 3}, {1, 0}, {4, 5} };   // 片元段哪些槽非对齐：程序用到的槽 0 在不在、别的槽怎么摆，每次都不同
            for (int s2 = 0; s2 < 4; s2++) {
                for (unsigned sl = 0; sl < 8; sl++) gc_ssbo_set(1, sl, 0);
                gc_ssbo_set(1, sets[s2][0], 0x10001); gc_ssbo_set(1, sets[s2][1], 0x10003);
                const struct rp_prog *px = NULL;
                CHECK(rp_shader_resolve(&q, &px) == 0 && px, "stage-key cache: binding pattern %d resolves", s2);
            }
            for (unsigned sl = 0; sl < 8; sl++) gc_ssbo_set(1, sl, 0);
            unsigned long long reads2 = 0, hits2 = 0; ssbo_cost_counts(&reads2, &hits2);
            CHECK(reads2 == reads1, "stage-key cache: four more binding states on the same stage key read the library no more (reads +%llu)", reads2 - reads1);
            CHECK(hits2 >= hits1 + 4, "stage-key cache: each of the four resolves was answered from the cache (hits +%llu)", hits2 - hits1);
        }
        gc_fssb_un_off();
    }
    fake_gs_success = 0; fake_gs_result_mode = 0; external_profile[0] = 0; unsetenv("S2A_SHADER_REVISION_ROOT");
    fixture_used_slots = -1;
    char path[700];
    snprintf(path, sizeof path, "%s/lib.idx", dir); unlink(path); snprintf(path, sizeof path, "%s/lib.000.pak", dir); unlink(path);
    rmdir(dir); snprintf(dir, sizeof dir, "%s/shaders/direct", root); rmdir(dir); snprintf(dir, sizeof dir, "%s/shaders", root); rmdir(dir); rmdir(root);
    printf("nvk shader pool (libpack state gate readout): %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (s2a_program_cache_start()) return 2;
    if (argc == 2 && !strcmp(argv[1], "--lp-gate")) return lp_gate_test();
    if (argc == 4 && !strcmp(argv[1], "--submit-compile")) return submit_compile_test(atoi(argv[2]), atoi(argv[3]));
    if (argc == 2 && !strcmp(argv[1], "--live-compile-default")) return live_compile_thread_test(0);
    if (argc == 2 && !strcmp(argv[1], "--live-compile-thread")) return live_compile_thread_test(1);
    if (argc == 2 && !strcmp(argv[1], "--live-compile-pending")) return live_compile_pending_test(0);
    if (argc == 2 && !strcmp(argv[1], "--live-compile-stop")) return live_compile_pending_test(1);
    if (argc == 2 && !strcmp(argv[1], "--diag-compile")) return diag_compile_test();
    if (argc == 4 && !strcmp(argv[1], "--lp-output")) {
        output_fixture_path = argv[2];
        output_fixture_class = atoi(argv[3]);
        return gs_libpack_test(0, 0);
    }
    if (argc == 2 && !strcmp(argv[1], "--lp-numeric")) return gs_libpack_test(0, 1);
    if (argc == 2 && !strcmp(argv[1], "--gs-libpack")) return gs_libpack_test(0, 0);
    if (argc == 2 && !strcmp(argv[1], "--link-refused-file")) return gs_libpack_test(1, 0);
    if (argc == 2 && !strcmp(argv[1], "--cs-libpack")) return cs_libpack_test();
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
    if (argc == 6 && (!strcmp(argv[1], "--emit-high-slot") || !strcmp(argv[1], "--execute-high-slot")))
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], !strcmp(argv[1], "--emit-high-slot") ? 8 : 9);
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
    if (argc == 6 && (!strcmp(argv[1], "--emit-nowindow") || !strcmp(argv[1], "--execute-nowindow") || !strcmp(argv[1], "--nowindow-arms"))) {
        emit_nowindow_pair = !strcmp(argv[1], "--emit-nowindow");
        execute_nowindow_pair = !emit_nowindow_pair;
        char wd[320], root[400], manifest[400], cache[400];
        if (!strcmp(argv[1], "--nowindow-arms")) {   // 自带临时目录：种子树、程序清单、跨局缓存各一个，不碰调用方的目录
            snprintf(wd, sizeof wd, "%s/nowindow_arms.XXXXXX", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
            if (!mkdtemp(wd)) return 1;
            snprintf(root, sizeof root, "%s/root", wd); snprintf(manifest, sizeof manifest, "%s/manifest", wd); snprintf(cache, sizeof cache, "%s/cache", wd);
            char sh[420], direct[440];
            snprintf(sh, sizeof sh, "%s/shaders", root); snprintf(direct, sizeof direct, "%s/direct", sh);
            if (mkdir(root, 0700) || mkdir(sh, 0700) || mkdir(direct, 0700) || mkdir(manifest, 0700) || mkdir(cache, 0700)) return 1;
            setenv("S2A_PW_TEST_DIR", manifest, 1);
            setenv("S2A_SHADER_REVISION_ROOT", root, 1);   // 随包种子目录 = <这个根>/shaders/direct/warm-vk/<闭包>-<profile>（nvk_prog_warm 的 seed_dir_of）
            arms_root = root; arms_manifest = manifest;
        }
        return emit_real_request(argv[2], argv[3], argv[4], argv[5], 0);
    }
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
    if (argc == 2 && !strcmp(argv[1], "--fact-wide-off")) {
        // 事实复用放宽关闭（debug.s2a.nvk_fact_wide=0）：fact_wide() 进程内只读一次，关闭态单独起一个进程
        fixture_fact_wide = 0;
        graphics_fact_wide_off_test();
        printf("nvk shader pool (fact wide off): %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--fact-wide-default")) {
        // 生产缺省：不强制开关，fact_wide() 取 NVK_FACT_WIDE_DEFAULT；按缺省那一档跑对应的一组用例（改缺省只动宏，不用改这里）
        extern int nvk_shader_test_fact_wide(int *dflt);
        int dflt = -1;
        const int eff = nvk_shader_test_fact_wide(&dflt);
        CHECK(eff == dflt, "fact wide default: production default flows into fact_wide() (got %d, want %d)", eff, dflt);
        if (eff) graphics_fact_wide_test(); else graphics_fact_wide_off_test();
        printf("nvk shader pool (fact wide default=%d): %d checks, %d failures\n", dflt, checks, failures);
        return failures ? 1 : 0;
    }
    if (argc != 1) return 2;
    fixture_fact_wide = 1;   // 本进程其余用例都在「放宽开」下跑（开 / 关 / 缺省三档各有单独进程）
    graphics_fact_reuse_test();
    graphics_fact_array_length_test();
    graphics_fact_wide_test();
    graphics_high_slot_test();
    graphics_active_ubo_test();
    graphics_c1_full_test();
    graphics_alias_test(1);
    graphics_alias_fact_test(1);
    graphics_alias_fact_bg_test(1);
    graphics_ubo_handle_fact_test();
    graphics_reject_context_test();
    query_sampler_test();
    compute_pool_test();
    compute_ubo_const_sampler_test();
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
        // 编译器把被拒消息整条守在 245 个字符以内（DirectShaderCompiler.cs HostMessageMaxChars）：compile_request.c 把整段错误 JSON
        // 抄进 reason[320]（≤ 319 个字符），信封 {"schema":…,"code":"UNSUPPORTED_PROGRAM","message":"…"} 占 74 个，245 + 74 = 319 正好装下；
        // 再多一个字符 JSON 被截断、compiler_rejected 解析不出来，整条归成 unclassified。恰好 245 个字符的消息必须仍照常归类
        char edge[640], edge_msg[300];
        {
            size_t n = (size_t)snprintf(edge_msg, sizeof edge_msg,
                "fs: texture handle from cbuf slot 65538 is not implemented (handle=809517146, arrayLength=1, count=1, request=%064d)"
                " [kind=texture word=90 word2=257 type=3 cbuf=2/0 key=", 0);
            memset(edge_msg + n, 'k', 244 - n); edge_msg[244] = ']'; edge_msg[245] = 0;
            CHECK(strlen(edge_msg) == 245, "boundary message is exactly 245 characters: %zu", strlen(edge_msg));
            snprintf(edge, sizeof edge, "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"%s\"}", edge_msg);
            CHECK(strlen(edge) == 319, "boundary error JSON is exactly 319 characters: %zu", strlen(edge));
        }
        const char *errors[] = {
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"fs: texture handle from cbuf slot 0 is not implemented\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"fs: separate sampler slot is not implemented\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureArrayLengthFromBuffer has no source (slot=2)\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureFormat has no source (stage=fs,handle=3,cbufSlot=-1,format=Unknown)\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureCoordNormalized has no source (stage=fs,handle=3,cbufSlot=-1)\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"missing fact\"}",
            // #999：编译器在「texture handle from cbuf slot N」后面补的现场（绑定种类 / 字偏移 / 类型 / 两个常量缓冲号；用户 UBO 那一处另有 handle / 个数 / 数组长度 / 程序键）：归类不变
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"fs: texture handle from cbuf slot 65538 is not implemented"
                " (handle=809517146, arrayLength=1, count=1, request=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef)"
                " [kind=texture word=90 word2=257 type=3 cbuf=2/0]\"}",
            "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"UNSUPPORTED_PROGRAM\",\"message\":\"fs: texture handle from cbuf slot 4 is not implemented (offset outside 64 KiB)"
                " [kind=texture handle=-2147483648 word=0 word2=0 type=8 count=1 array=1 key=0123456789abcdef0123456789abcdef0123456789abcdef]\"}",
            edge,
            "invalid error"};
        const char *reasons[] = {"cbuf_handle_unsupported", "separate_sampler_unsupported",
            "MISSING_FACT.QueryTextureArrayLengthFromBuffer", "MISSING_FACT.QueryTextureFormat",
            "MISSING_FACT.QueryTextureCoordNormalized", "MISSING_FACT.unclassified",
            "cbuf_handle_unsupported", "cbuf_handle_unsupported", "cbuf_handle_unsupported", "unclassified"};
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
        {   // 诊断开关关着（玩家包口径）：拒绝原因照样数、第一次拒绝就打一行（全库普查按款汇总它）；整份程序统计不打
            const int d0 = diag_enabled; diag_enabled = 0; rp_shader_reset(); nvn_unimpl_reset(); last_reason[0] = last_stats[0] = 0;
            fake_compile_error = errors[3]; bind_pools(524288); compiler_calls = 0;
            CHECK(rp_shader_resolve(&q, &p) != 0 && compiler_calls == 1, "player build: compiler rejection still happens once");
            CHECK(strstr(last_reason, "reason=MISSING_FACT.QueryTextureFormat count=1") && !last_stats[0],
                  "player build: reject reason counted and logged without the diagnostic switch, program stats stay off: reason=%s stats=%s", last_reason, last_stats);
            rp_shader_reset(); diag_enabled = d0;
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
    compute_default_fact_test();
    compute_ubo_handle_fact_test();
    last_table_test();
    gs_request_test();
    rp_shader_reset(); s2a_source_clear();
    fprintf(stderr, "nvk shader pool: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
