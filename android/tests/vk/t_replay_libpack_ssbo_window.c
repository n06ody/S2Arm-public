// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 图形 LibPack 的存储缓冲窗口补偿：出包库的顶点着色器（翻译器重译、带窗口掩码，见 DshVk.cs StorageWindowContract）
//   → 生产取库合成（libpack_vk.c 按设备对齐 A 固化掩码）→ 生产解析 / 布局（nvk_shader.c）→ 回放绘制核心（rp_draw.c 把 c0 写原始基址 B、
//   存储缓冲窗口从 B & -A 起）→ 真实 rp_buf 镜像 → Vulkan（lavapipe + 校验层，含同步校验）→ 回写客体内存，逐字节比对。
// 着色器（原创合成 Maxwell 指令，见 graphics/tools/direct/tests/dsh_vk/Program.cs WindowFixture）：
//   R4 = [B+0x20]；[B+0x40] = R4；[B+0] = c3[0]。期望字节手算：[B+0] = 0x12345678、[B+0x40] = 初始 [B+0x20]，客体内存其余每个字节不变。
// 对齐档 A = 16 / 64 / 256：设备真对齐 16，其余档位把后端取到的能力改大（描述符偏移取 A 的倍数同样满足真设备的 16）。
//   基址 B = 池内 +0x4000 + A + 余数，余数 ∈ {0, 4, A/2, A-4}：B 的位 A 与位 A/2 都有取值，错的掩码（A/2、2A）总有一档读错地址。
#define nvn_plat_log backend_fixture_log
#define nvn_plat_flag backend_fixture_flag
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_log
#undef nvn_plat_flag
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "nvn_unimpl.h"
#include "nvn_dirty.h"
#include "direct/compile_request.h"
#include "direct/source_store.h"
#include "direct/s2a_direct.h"
#include "direct/sha256.h"
#include "direct/jv.h"
#include "dmsh_chain.h"
#include "direct/libpack_vk.h"
#include <stdalign.h>
#include <sys/stat.h>

void rp_buf_reset(void);

static int compile_calls;
static int pair_binst;
static unsigned binst_copies, paired_hits;
void nvn_plat_log(int level, const char *cls, const char *message) {
    if (message && !strncmp(message, "S2A_LIBPAIR ", 12) && strstr(message, "same_source=1 json_rc=0 s2sb_rc=0"))
        paired_hits++;
    backend_fixture_log(level, cls, message);
}
int nvn_plat_flag(const char *name, int dflt) {
    return !strcmp(name, "nvk_libpack_pair_timing") ? pair_binst : backend_fixture_flag(name, dflt);
}
const char *nvn_plat_dir(int which) { (void)which; return NULL; }

static const char *case_dir;
static prog_t guest_program; static uint8_t guest_keys[6][8], guest_stages[6];
static p_set_rt target; static tex_t target_tex; static nvn_gpu_view target_view; static nvn_gpu_tex target_tex_backend;
static nvn_gpu_cmd active; static uint64_t serial_no = 1;
static uint32_t profile_align = 16;   // 本局 profile 里的 storageBufferOffsetAlignment（与设备能力一致才放行；用例可故意错开）
int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    if (serial != guest_program.serial) return NULL;
    memcpy(keys, guest_keys, sizeof guest_keys); memcpy(stage, guest_stages, sizeof guest_stages); *ok = 3; return &guest_program;
}
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) {
    if (index == RP_PERF_BINST_ONE || index == RP_PERF_BINST_HZ) binst_copies += (unsigned)value;
}
uint64_t rp_frame(void) { return 1; }
uint64_t rp_seg_serial(void) { return serial_no; }
uint64_t rp_sub_serial(void) { return serial_no; }
uint64_t rp_sub_fence(void) { return 1; }
int rp_fence_done(uint64_t f) { (void)f; return 1; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
// 客体内存池：整块 guest[]（真实 rp_buf 据此建镜像、同步页、回写）
enum { GUEST_BYTES = 0x8000, UBO_OFF = 0x1000, SB_BASE = 0x4000, SB_BOUND = 0x100 };
static alignas(4096) unsigned char guest[GUEST_BYTES];
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) {
    const uint64_t base = (uint64_t)(uintptr_t)guest;
    if (a < base || n > sizeof guest || a - base > sizeof guest - n) return 0;
    if (m) *m = guest;
    if (s) *s = sizeof guest;
    return 1;
}
static uint64_t wkey[4], wid[4]; static int nw;
int rp_post_gpu_write(uint64_t a, uint64_t n) {
    if (nw >= 4) return 0;
    wid[nw] = rp_buf_gpu_write_note(a, n, &wkey[nw]);
    if (!wid[nw]) return 0;
    nw++; return 1;
}
// 不受写跟踪的池：每次解析都重新同步 CPU 页，真实 rp_buf 的「显卡写过未回写」排除负责保护
size_t nvn_dirty_page_size(void) { return 4096; }
uint64_t nvn_dirty_now(void) { return 1; }
uint64_t nvn_dirty_mapping_generation(void) { return 1; }
int nvn_dirty_tracking_unreliable(void) { return 1; }
const void *nvn_dirty_span(const void *a, size_t n) { (void)a; (void)n; return NULL; }
long nvn_dirty_page_gens(const void *s, const void *a, size_t l, uint64_t *g, size_t n) { (void)s; (void)a; (void)l; (void)g; (void)n; return -1; }
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
void nvn_dirty_protect(const void *a, size_t n) { (void)a; (void)n; }
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, tex_t *t) { (void)id; (void)c; if (t) memset(t, 0, sizeof *t); return NULL; }
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) { (void)t; (void)v; (void)c; return NULL; }
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t tt, uint8_t cls, int elem, nvn_gpu_cmd c, tex_t *t) { (void)id; (void)tt; (void)cls; (void)elem; (void)c; if (t) memset(t, 0, sizeof *t); return NULL; }
nvn_gpu_view rp_view_by_ubo_handle_decl(int i, uint8_t t, uint8_t k, nvn_gpu_cmd c, tex_t *o) { return rp_view_by_id_decl(i, t, k, 0, c, o); }
void rp_decl_probe_begin(void) {} int rp_decl_probe_end(void) { return 0; }
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c, int samp, int base, uint32_t n, uint8_t dtt, uint8_t dcls, const char **why) { (void)c; (void)samp; (void)base; (void)n; (void)dtt; (void)dcls; if (why) *why = "pool_table_unresolved"; return NULL; }
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
nvn_gpu_samp rp_samp_by_id(int id) { (void)id; return NULL; }
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) { (void)pb; (void)pn; return rp_samp_for_tex(sid, tid); }
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) { (void)id; (void)t; (void)v; (void)h; return 0; }
uint64_t rp_tex_reg_gen(void) { return 1; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int count) { (void)h; (void)n; (void)b; return (uint64_t)count; }
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; }
int rp_tex_debug_state(uint64_t s) { (void)s; return 0; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s) { (void)s; return NULL; }
const p_set_rt *rp_pass_target(void) { return &target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = *h = 4; }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { (void)w; (void)h; }
void rp_pass_note_draw(int d) { (void)d; }
void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask) { (void)c; (void)color_write_mask; }
void rp_pass_land_reads(nvn_gpu_cmd c, const tex_t *const *ts, int n) { (void)c; (void)ts; (void)n; }
int rp_pass_parked(void) { return 0; }
static int pass_open;
static void pass_begin_now(nvn_gpu_cmd c) {
    nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = target_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4; pass.layers = 1; nvn_gpu_pass_begin(c, &pass); pass_open = 1;
}
int rp_pass_begin(nvn_gpu_cmd c) { if (!pass_open) pass_begin_now(c); return 1; }
void rp_pass_flush(nvn_gpu_cmd c) { if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; } }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last) { rp_pass_flush(active); *last = serial_no; return active; }

static char profile_json[256];
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    memset(c, 0, sizeof *c); c->session = 4; c->generation = 6; c->direct_abi = 1;
    snprintf(profile_json, sizeof profile_json, "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":%u}}", profile_align);
    c->profile_json = profile_json; c->profile_bytes = strlen(profile_json);
    c->profile_hash = "sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";   // 同 profile_json 里的 profileHash（计算请求核二者一致）
    c->translator_closure = "libpack-ssbo-window-test"; c->storage_buffer_offset_alignment = (uint32_t)g_dev.properties.limits.minStorageBufferOffsetAlignment; return 0;
}
// 现编不该被走到：出库命中不会现编；出库拒后的退路调用次数就是证据。
static int32_t compiled_result(uint64_t session, uint64_t generation, s2a_direct_bytes req, s2a_direct_emit_fn emit, void *sink) {
    (void)session; (void)generation; (void)req; (void)emit; (void)sink; compile_calls++; return -1;
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return compiled_result; }

static void put32(uint8_t *p, uint32_t n) { p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24; }
static void put64(uint8_t *p, uint64_t n) { put32(p, (uint32_t)n); put32(p + 4, (uint32_t)(n >> 32)); }
struct rec { uint64_t key; uint8_t kind; const uint8_t *d; size_t n; };
static int rec_cmp(const void *a, const void *b) {
    const struct rec *x = a, *y = b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : (int)x->kind - (int)y->kind;
}
static int write_container(const char *dir, struct rec *r, int n) {
    qsort(r, (size_t)n, sizeof *r, rec_cmp);
    char path[1200]; snprintf(path, sizeof path, "%s/lib.000.pak", dir);
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    uint64_t off[8], at = 0;
    for (int i = 0; i < n; i++) { off[i] = at; if (fwrite(r[i].d, 1, r[i].n, f) != r[i].n) { fclose(f); return -1; } at += r[i].n; }
    if (fclose(f)) return -1;
    uint8_t idx[64 + 24 * 8]; memset(idx, 0, sizeof idx);
    memcpy(idx, "S2ALIB\0\0", 8); put32(idx + 8, 1); put32(idx + 12, (uint32_t)n); put32(idx + 16, 1);
    if (r[0].kind == 128) memcpy(idx + 44, "S2SB", 4);
    memcpy(idx + 24, DMSH_CHAIN, 12); put64(idx + 48, at);
    for (int i = 0; i < n; i++) {
        uint8_t *p = idx + 64 + 24 * i;
        put64(p, r[i].key); p[8] = r[i].kind; put32(p + 12, (uint32_t)r[i].n); put64(p + 16, off[i]);
    }
    snprintf(path, sizeof path, "%s/lib.idx", dir);
    f = fopen(path, "wb"); if (!f) return -1;
    if (fwrite(idx, 1, 64 + 24 * (size_t)n, f) != 64 + 24 * (size_t)n) { fclose(f); return -1; }
    return fclose(f) ? -1 : 0;
}
static uint64_t key64(const uint8_t k[8]) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | k[i]; return v; }

enum { V_WINDOW, V_OLD_ABI, V_BAD_MASK_ID, V_PAIR_UNUSED, V_PAIR_READ };   // 库里的顶点段：带窗口合同 / 旧库（无合同）/ 掩码 id 指不到常量
struct vsrc { char *json; uint8_t *spv; size_t spv_n; char key[17]; };   // 翻译器出的顶点段（_b.json / _b.spv）与它原来的程序键
static struct vsrc syn, real, psrc, csrc;   // csrc = 原创合成计算（dsh_vk WindowFixtureCs）   // psrc = 原创合成片元（同一份翻译器出料，见 dsh_vk WindowFixturePs）   // syn = 原创合成程序（仓内可复现）；real = 私有真实程序（可选，只在本机 build/ 下，不入仓）
static uint8_t *fs_spv, *fs_real_spv, *vs_triv_spv; static size_t fs_n, fs_real_n, vs_triv_n;
// 判据用的不变量：读回夹具里每个字都是 xorshift 出来的伪随机数，撞上期望值的概率 2^-32
static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void word_put(uint64_t off, uint32_t v) { memcpy(guest + off, &v, 4); }

static int load_vsrc(struct vsrc *v, const char *dir, const char *keyfile) {
    char path[1100]; size_t n = 0;
    snprintf(path, sizeof path, "%s/%s", dir, keyfile); uint8_t *k = read_file(path, &n);
    if (!k || n != 16) return -1;
    memcpy(v->key, k, 16); v->key[16] = 0; free(k);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, v->key); uint8_t *js = read_file(path, &n);
    if (!js) return -1;
    v->json = malloc(n + 1); memcpy(v->json, js, n); v->json[n] = 0; free(js);
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, v->key); v->spv = read_file(path, &v->spv_n);
    return v->spv ? 0 : -1;
}
// 写一份只含「顶点段 v + 片元段」的出包库，设成本局的资源根
static void make_library(const char *tag, const struct vsrc *v, int variant, const uint8_t *vs_spv_override, size_t vs_override_n, const uint8_t *fs, size_t fsn) {
    char root[1024], dir[1100];
    snprintf(root, sizeof root, "%s/lp-%s", case_dir, tag); mkdir(root, 0700);
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    char hx[2][17], jfs[256];
    for (int s = 0; s < 2; s++) for (int b = 0; b < 8; b++) snprintf(hx[s] + 2 * b, 3, "%02x", guest_keys[s][b]);
    char *jvs = malloc(strlen(v->json) + 128); strcpy(jvs, v->json);
    if (variant == V_PAIR_READ) {
        // Conservative bank-0 contract: the vec4 at 0x640 includes the base-instance word.
        const char from[] = "\"drv_reads\":{}";
        const char to[] = "\"drv_reads\":{\"0\":{\"vec4_offsets\":[1600],\"dynamic\":false}}";
        char *reads = strstr(jvs, from);
        CHECK(reads != NULL, "%s: compiler fixture has an empty driver-read set", tag);
        if (reads) {
            memmove(reads + sizeof to - 1, reads + sizeof from - 1, strlen(reads + sizeof from - 1) + 1);
            memcpy(reads, to, sizeof to - 1);
        }
    }
    for (char *p = jvs; (p = strstr(p, v->key)); p += 16) memcpy(p, hx[0], 16);   // 同长度：原键换成本夹具的程序键
    if (variant == V_OLD_ABI) { char *p = strstr(jvs, ",\"ssbo_window_abi\""); CHECK(p != NULL, "%s: window fields present in template", tag); if (p) strcpy(p, "}"); }
    if (variant == V_BAD_MASK_ID) { char *p = strstr(jvs, "\"ssbo_window_mask\":"); CHECK(p != NULL, "%s: mask field present in template", tag); if (p) strcpy(p, "\"ssbo_window_mask\":99999}"); }
    snprintf(jfs, sizeof jfs, "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[],\"ssbo_window_abi\":1}", hx[1]);
    struct rec r[4] = {
        {key64(guest_keys[0]), 1, (const uint8_t *)jvs, strlen(jvs)}, {key64(guest_keys[0]), 52, vs_spv_override ? vs_spv_override : v->spv, vs_spv_override ? vs_override_n : v->spv_n},
        {key64(guest_keys[1]), 1, (const uint8_t *)jfs, strlen(jfs)}, {key64(guest_keys[1]), 52, fs, fsn},
    };
    CHECK(write_container(dir, r, 4) == 0, "%s: library container written", tag);
    if (pair_binst) {
        snprintf(dir, sizeof dir, "%s/shaders/direct/qa-s2sb", root); mkdir(dir, 0700);
        snprintf(dir, sizeof dir, "%s/shaders/direct/qa-s2sb/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
        const uint8_t *reflection[] = { (const uint8_t *)jvs, (const uint8_t *)jfs };
        const uint8_t *spirv[] = { v->spv, fs };
        const size_t sizes[] = { v->spv_n, fsn };
        struct rec binary[2] = {{0}};
        uint8_t *records[2] = {0};
        size_t bytes[2] = {0};
        for (unsigned st = 0; st < 2; st++) {
            int rc = s2a_pack_stage(guest_keys[st], st, -1, 0, reflection[st], strlen((const char *)reflection[st]),
                                    spirv[st], sizes[st], &records[st], &bytes[st]);
            CHECK(rc == 0 && records[st], "%s: encode S2SB stage %u", tag, st);
            binary[st] = (struct rec){key64(guest_keys[st]), 128, records[st], bytes[st]};
        }
        CHECK(records[0] && records[1] && !write_container(dir, binary, 2), "%s: paired S2SB container written", tag);
        for (unsigned st = 0; st < 2; st++) s2a_pack_stage_free(records[st]);
    }
    free(jvs);
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
}
// 能力要在开后端之前改：fill_caps 在 open 时取一次
static int open_session(const char *tag, uint32_t dev_align, uint32_t prof_align) {
    g_dev.properties.limits.minStorageBufferOffsetAlignment = dev_align; profile_align = prof_align;
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "libpack-ssbo-window";
    const int opened = open_backend(); CHECK(opened, "%s: backend open", tag); if (!opened) return 0;
    nvn_gpu_tex tex = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); target_view = make_view(tex); target_tex_backend = tex;
    memset(&target_tex, 0, sizeof target_tex); target_tex.d.target = NVN_TEXTURE_TARGET_2D; target_tex.d.format = NVN_FORMAT_RGBA8;
    target_tex.d.width = target_tex.d.height = 4; target_tex.d.depth = 1; target_tex.d.levels = 1; target_tex.serial = 7;
    memset(&target, 0, sizeof target); target.n = 1; target.color[0] = &target_tex;
    return 1;
}
static void close_session(const char *tag) {
    nvn_gpu_err e = {0};
    rp_shader_reset();   // 本会话载入的着色器必须在关会话之前放掉：留到下一个用例再放就是在别的后端上销毁旧句柄
    nvn_gpu_view_release(target_view); nvn_gpu_tex_release(target_tex_backend);
    rp_buf_reset(); rp_draw_reset();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "%s: close session", tag);
}
static void bind_program(const char *tag) {
    rp_buf_reset(); rp_draw_reset(); rp_shader_reset(); rp_state_reset(); nw = 0;
    guest_program.serial++; serial_no++;
    p_bind_program_id bind = {&guest_program, guest_program.serial, 0x3, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "%s: OP_BIND_PROGRAM accepted", tag);
    dpool_t tp = {0}, sp = {0}; tp.n = 7; sp.n = 9; p_set_pool t = {&tp}, s = {&sp};
    CHECK(rp_state_op(OP_SET_TEX_POOL, &t, sizeof t) && rp_state_op(OP_SET_SAMP_POOL, &s, sizeof s), "%s: descriptor pools accepted", tag);
}
// 一笔绘制 + 提交 + 回写落地；pixels 非空时同时读回 4×4 颜色目标
static int draw_and_land(const char *tag, uint8_t *pixels, uint64_t *drawn) {
    uint64_t d0 = 0, k0 = 0; rp_draw_stats(&d0, &k0);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL); memset(buf_cpu(rb), 0xEE, 64);
    active = begin_cmd(); p_draw draw = {0}; draw.prim = 4; draw.count = 3; draw.instances = 1;
    if (pair_binst) draw.ind = (uint64_t)(uintptr_t)(guest + 0x2000);
    rp_draw(active, &draw, NULL, &draw);
    uint64_t d1 = 0, k1 = 0; rp_draw_stats(&d1, &k1);
    nvn_gpu_err e = {0};
    if (d1 - d0 == 1) {
        static uint64_t fence_no; rp_pass_flush(active);
        if (pixels) nvn_gpu_copy_tex_buf(active, target_tex_backend, 0, 0, NULL, rb, 0, 16, 64);
        nvn_gpu_rc rc = nvn_gpu_cmd_commit(active, ++fence_no, on_done, NULL, &e); CHECK(OK(rc), "%s: commit %d %s", tag, rc.code, e.why);
        rc = nvn_gpu_fence_wait(fence_no, 5000000000ull, &e); CHECK(OK(rc), "%s: wait %d %s", tag, rc.code, e.why);
        for (int k = 0; k < nw; k++) rp_buf_gpu_write_land(wkey[k], wid[k], 1);
        if (pixels) memcpy(pixels, buf_cpu(rb), 64);
    } else { rp_pass_flush(active); nvn_gpu_cmd_abort(active); }
    nvn_gpu_buf_release(rb, NULL, NULL);
    if (drawn) *drawn = d1 - d0;
    return d1 - d0 == 1 && k1 - k0 == 0;
}

struct result { int drew, hit, rej, mismatches; uint64_t window_off; };
// 一个用例：设备对齐 A（真设备 16，其余档改能力）、基址 B = SB_BASE + A + rem；variant 选库里顶点段的合同形态。
static struct result run_case(const char *tag, uint32_t dev_align, uint32_t prof_align, uint32_t rem, int variant) {
    struct result res = {0};
    if (!open_session(tag, dev_align, prof_align)) return res;
    make_library(tag, &syn, variant, NULL, 0, fs_spv, fs_n);
    // 客体内存：基址附近整片是伪随机，窗口前缀、目标字、邻居字互不相同
    const uint64_t B_off = SB_BASE + dev_align + rem, gbase = (uint64_t)(uintptr_t)guest, B = gbase + B_off;
    res.window_off = B_off - rem;
    for (unsigned i = 0; i < GUEST_BYTES; i += 4) word_put(i, rnd());
    const uint32_t src_val = 0xCAFEF00Du, ubo_val = 0x12345678u;
    word_put(B_off + 0x20, src_val); word_put(UBO_OFF, ubo_val);
    if (pair_binst) {
        const uint32_t args[] = {3, 1, 0, 0};
        memcpy(guest + 0x2000, args, sizeof args);
    }
    static uint8_t expect[GUEST_BYTES]; memcpy(expect, guest, sizeof expect);
    memcpy(expect + B_off + 0x40, &src_val, 4); memcpy(expect + B_off + 0, &ubo_val, 4);

    bind_program(tag);
    p_bind_buf ubo = {0, 0, gbase + UBO_OFF, 16}, ssbo = {0, 0, B, SB_BOUND};   // 顶点阶段（NVN 阶段 0）：用户常量缓冲 0、存储缓冲 0
    CHECK(rp_state_op(OP_BIND_UBO, &ubo, sizeof ubo) && rp_state_op(OP_BIND_SSBO, &ssbo, sizeof ssbo), "%s: VS UBO 0 / SSBO 0 bound", tag);
    const uint64_t h0 = s2a_libpack_hits(), j0 = s2a_libpack_rejects();
    res.drew = draw_and_land(tag, NULL, NULL);
    res.hit = (int)(s2a_libpack_hits() - h0); res.rej = (int)(s2a_libpack_rejects() - j0);
    if (res.drew) {
        // 回写只登记游戏绑的那一段 [B, B + 0x100)：段内逐字节比对，段外保持随机初值（不由 GPU 回写，也不该被改）
        for (unsigned i = 0; i < GUEST_BYTES; i++) if (guest[i] != expect[i]) {
            if (res.mismatches < 6) printf("  %s: byte %#x (B%+d) got %02x want %02x\n", tag, i, (int)i - (int)B_off, guest[i], expect[i]);
            res.mismatches++;
        }
    }
    close_session(tag);
    return res;
}

// 片元阶段的窗口读回：库 = 无输入的全屏三角形顶点段（abi 1、无存储缓冲）+ 合成片元段（两个存储缓冲槽，32 位 / 128 位读写）。
//   [B0+0x40] = [B0+0x20]；[B0+0x80..0x8f] = [B0+0x30..0x3f]；[B0+0] = c3[0]；[B1+0x20] = [B1+0x10]。两个缓冲各有各的基址余数。
static void make_library_ps(const char *tag, const struct vsrc *fsv) {
    char root[1024], dir[1100];
    snprintf(root, sizeof root, "%s/lp-%s", case_dir, tag); mkdir(root, 0700);
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    char hx[2][17], jvs[256];
    for (int s = 0; s < 2; s++) for (int b = 0; b < 8; b++) snprintf(hx[s] + 2 * b, 3, "%02x", guest_keys[s][b]);
    snprintf(jvs, sizeof jvs, "{\"stage\":\"vs\",\"dm_ctl\":\"%s\",\"bindings\":[],\"ssbo_window_abi\":1}", hx[0]);
    char *jfs = malloc(strlen(fsv->json) + 32); strcpy(jfs, fsv->json);
    for (char *p = jfs; (p = strstr(p, fsv->key)); p += 16) memcpy(p, hx[1], 16);
    struct rec r[4] = {
        {key64(guest_keys[0]), 1, (const uint8_t *)jvs, strlen(jvs)}, {key64(guest_keys[0]), 52, vs_triv_spv, vs_triv_n},
        {key64(guest_keys[1]), 1, (const uint8_t *)jfs, strlen(jfs)}, {key64(guest_keys[1]), 52, fsv->spv, fsv->spv_n},
    };
    CHECK(write_container(dir, r, 4) == 0, "%s: library container written", tag);
    free(jfs);
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
}
static struct result run_ps_case(const char *tag, uint32_t dev_align, uint32_t rem0, uint32_t rem1) {
    struct result res = {0};
    if (!open_session(tag, dev_align, dev_align)) return res;
    make_library_ps(tag, &psrc);
    const uint64_t B0_off = SB_BASE + dev_align + rem0, B1_off = SB_BASE + 0x1000 + dev_align + rem1, gbase = (uint64_t)(uintptr_t)guest;
    for (unsigned i = 0; i < GUEST_BYTES; i += 4) word_put(i, rnd());
    const uint32_t src0 = 0xCAFEF00Du, src1 = 0xFEEDBEEFu, ubo_val = 0x12345678u;
    word_put(B0_off + 0x20, src0); word_put(B1_off + 0x10, src1); word_put(UBO_OFF, ubo_val);
    static uint8_t expect[GUEST_BYTES]; memcpy(expect, guest, sizeof expect);
    memcpy(expect + B0_off + 0x40, &src0, 4); memcpy(expect + B0_off + 0, &ubo_val, 4);
    memcpy(expect + B0_off + 0x80, guest + B0_off + 0x30, 16);   // 128 位：四个相邻的随机字原样搬
    memcpy(expect + B1_off + 0x20, &src1, 4);

    bind_program(tag);
    p_bind_buf ubo = {1, 0, gbase + UBO_OFF, 16}, s0 = {1, 0, gbase + B0_off, SB_BOUND}, s1 = {1, 1, gbase + B1_off, SB_BOUND};   // 片元阶段（NVN 阶段 1）
    CHECK(rp_state_op(OP_BIND_UBO, &ubo, sizeof ubo) && rp_state_op(OP_BIND_SSBO, &s0, sizeof s0) && rp_state_op(OP_BIND_SSBO, &s1, sizeof s1), "%s: FS UBO 0 / SSBO 0, 1 bound", tag);
    rp_S.color.logic_op = 3; rp_S.chmask.mask = 15; rp_S.poly.mode = NVN_POLYGON_MODE_FILL;
    const uint64_t h0 = s2a_libpack_hits(), j0 = s2a_libpack_rejects();
    res.drew = draw_and_land(tag, NULL, NULL);
    res.hit = (int)(s2a_libpack_hits() - h0); res.rej = (int)(s2a_libpack_rejects() - j0);
    if (res.drew) {
        for (unsigned i = 0; i < GUEST_BYTES; i++) if (guest[i] != expect[i]) {
            if (res.mismatches < 6) printf("  %s: byte %#x (B0%+d B1%+d) got %02x want %02x\n", tag, i, (int)i - (int)B0_off, (int)i - (int)B1_off, guest[i], expect[i]);
            res.mismatches++;
        }
    }
    close_session(tag);
    return res;
}

// 计算阶段：库 = 单个计算段（abi 1 + 掩码）。[B+0x40] = [B+0x20]；[B+0x80..0x8f] = [B+0x30..0x3f]（128 位）；[B+0] = c3[0]。
static void make_library_cs(const char *tag, const struct vsrc *cs, int variant) {
    char root[1024], dir[1100];
    snprintf(root, sizeof root, "%s/lp-%s", case_dir, tag); mkdir(root, 0700);
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    char hx[17];
    for (int b = 0; b < 8; b++) snprintf(hx + 2 * b, 3, "%02x", guest_keys[0][b]);
    char *jcs = malloc(strlen(cs->json) + 32); strcpy(jcs, cs->json);
    for (char *p = jcs; (p = strstr(p, cs->key)); p += 16) memcpy(p, hx, 16);
    if (variant == V_OLD_ABI) { char *p = strstr(jcs, ",\"ssbo_window_abi\""); CHECK(p != NULL, "%s: window fields present in template", tag); if (p) strcpy(p, "}"); }
    struct rec r[2] = { {key64(guest_keys[0]), 1, (const uint8_t *)jcs, strlen(jcs)}, {key64(guest_keys[0]), 52, cs->spv, cs->spv_n} };
    CHECK(write_container(dir, r, 2) == 0, "%s: library container written", tag);
    free(jcs);
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
}
static struct result run_cs_case(const char *tag, uint32_t dev_align, uint32_t rem, int variant) {
    struct result res = {0};
    if (!open_session(tag, dev_align, dev_align)) return res;
    make_library_cs(tag, &csrc, variant);
    const uint64_t B_off = SB_BASE + dev_align + rem, gbase = (uint64_t)(uintptr_t)guest;
    for (unsigned i = 0; i < GUEST_BYTES; i += 4) word_put(i, rnd());
    const uint32_t src0 = 0xCAFEF00Du, ubo_val = 0x12345678u;
    word_put(B_off + 0x20, src0); word_put(UBO_OFF, ubo_val);
    static uint8_t expect[GUEST_BYTES]; memcpy(expect, guest, sizeof expect);
    memcpy(expect + B_off + 0x40, &src0, 4); memcpy(expect + B_off + 0, &ubo_val, 4);
    memcpy(expect + B_off + 0x80, guest + B_off + 0x30, 16);

    bind_program(tag);
    memcpy(rp_S.skey[5], guest_keys[0], 8); rp_S.skey_ok |= 1u << 5;   // 计算阶段的程序键（生产解析按它取库）
    p_bind_buf ubo = {5, 0, gbase + UBO_OFF, 16}, s0 = {5, 0, gbase + B_off, SB_BOUND};   // NVN 阶段 5 = 计算
    CHECK(rp_state_op(OP_BIND_UBO, &ubo, sizeof ubo) && rp_state_op(OP_BIND_SSBO, &s0, sizeof s0), "%s: CS UBO 0 / SSBO 0 bound", tag);
    const uint64_t h0 = s2a_libpack_hits(), j0 = s2a_libpack_rejects();
    active = begin_cmd();
    p_dispatch dp = {0}; dp.x = dp.y = dp.z = 1;
    rp_dispatch(active, &dp, NULL);
    nvn_gpu_err e = {0};
    {
        static uint64_t fence_no = 100000; rp_pass_flush(active);
        nvn_gpu_rc rc = nvn_gpu_cmd_commit(active, ++fence_no, on_done, NULL, &e); CHECK(OK(rc), "%s: commit %d %s", tag, rc.code, e.why);
        rc = nvn_gpu_fence_wait(fence_no, 5000000000ull, &e); CHECK(OK(rc), "%s: wait %d %s", tag, rc.code, e.why);
        for (int k = 0; k < nw; k++) rp_buf_gpu_write_land(wkey[k], wid[k], 1);
    }
    res.hit = (int)(s2a_libpack_hits() - h0); res.rej = (int)(s2a_libpack_rejects() - j0);
    for (unsigned i = 0; i < GUEST_BYTES; i++) if (guest[i] != expect[i]) {
        if (res.mismatches < 6) printf("  %s: byte %#x (B%+d) got %02x want %02x\n", tag, i, (int)i - (int)B_off, guest[i], expect[i]);
        res.mismatches++;
    }
    res.drew = res.mismatches == 0;
    close_session(tag);
    return res;
}

// 私有真实程序（一件角色布料蒙皮顶点着色器）：按实例号从存储缓冲 15 读实例数据，输出位置 / 法线 / 切线等。
//   判据是「同一份数据放在不同基址、不同对齐档下，画出的颜色相同」：数据相对基址的布局固定（B+0 起 0x200 字节、值取 0.05..0.95 的伪随机浮点），
//   C3 取单位矩阵、C4 / C8 取零；片元读 location 2 的输出（实例数据的函数）当颜色。基准 = 第一档（A=16 对齐基址）。
static uint8_t real_ref[4]; static int real_ref_set;
static int run_real(const char *tag, uint32_t dev_align, uint32_t rem, int *out_drew, int *out_mismatch, uint8_t first[4]) {
    *out_drew = 0; *out_mismatch = 0;
    if (!open_session(tag, dev_align, dev_align)) return -1;
    make_library(tag, &real, V_WINDOW, NULL, 0, fs_real_spv, fs_real_n);
    const uint64_t B_off = SB_BASE + dev_align + rem, gbase = (uint64_t)(uintptr_t)guest, B = gbase + B_off;
    uint32_t seed = 0x1234567u;
    for (unsigned i = 0; i < GUEST_BYTES; i += 4) word_put(i, rnd());   // 窗口前缀、窗口后面都是和数据无关的垃圾
    for (unsigned i = 0; i < 0x200; i += 4) { seed = seed * 1664525u + 1013904223u; const float f = 0.05f + 0.9f * (float)(seed >> 8) / (float)(1u << 24); memcpy(guest + B_off + i, &f, 4); }
    // 实例 0 的 3×4 变换（字节 0..47，行主序：R 行 [0..2]、[4..6]、[8..10]，平移在 [3]、[7]、[11]）：取近单位旋转 + 小扰动，让三角形盖满屏幕、
    //   输出颜色仍是这 12 个浮点的函数——任何一个读错位置颜色就变
    {
        float m[12] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0};
        for (int i = 0; i < 12; i++) {
            seed = seed * 1664525u + 1013904223u; const float u = (float)(seed >> 8) / (float)(1u << 24) - 0.5f;   // [-0.5, 0.5)
            m[i] += (i % 4 == 3) ? (i == 11 ? 0.5f + 0.1f * u : 0.1f * u) : (i % 5 == 0 ? 0.0f : 0.2f * u);
        }
        memcpy(guest + B_off, m, sizeof m);
    }
    static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for (unsigned r = 0; r < 0x200; r += 64) memcpy(guest + 0x1000 + r, ident, 64);   // c3：着色器读到第 19 行，行 k = 单位矩阵第 k%4 行
    memset(guest + 0x1400, 0, 0x40); memset(guest + 0x1500, 0, 0x40);                      // c4 / c8
    // 顶点流：3 个顶点盖满屏幕，4 个 float4 属性
    for (unsigned v = 0; v < 3; v++) {
        static const float xy[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
        float a[16] = {xy[v][0], xy[v][1], 0, 1,  0, 0, 1, 0,  1, 0, 0, 0,  0, 0, 0, 1};
        memcpy(guest + 0x2000 + 64 * v, a, 64);
    }
    bind_program(tag);
    rp_S.nattr = 4; rp_S.nstream = 1;
    for (int i = 0; i < 4; i++) { rp_S.attr[i].fmt = NVN_FORMAT_RGBA32F; rp_S.attr[i].offset = 16 * i; }
    rp_S.stream[0].stride = 64; rp_S.vbuf[0].address = gbase + 0x2000; rp_S.vbuf[0].size = 192;
    rp_S.color.logic_op = 3; rp_S.chmask.mask = 15; rp_S.poly.mode = NVN_POLYGON_MODE_FILL;
    p_bind_buf c3 = {0, 0, gbase + 0x1000, 0x200}, c4 = {0, 1, gbase + 0x1400, 0x40}, c8 = {0, 5, gbase + 0x1500, 0x40}, ssbo = {0, 15, B, 0x200};
    CHECK(rp_state_op(OP_BIND_UBO, &c3, sizeof c3) && rp_state_op(OP_BIND_UBO, &c4, sizeof c4) && rp_state_op(OP_BIND_UBO, &c8, sizeof c8) &&
          rp_state_op(OP_BIND_SSBO, &ssbo, sizeof ssbo), "%s: real VS cbufs / SSBO 15 bound", tag);
    const uint64_t h0 = s2a_libpack_hits(); uint8_t px[64];
    const int drew = draw_and_land(tag, px, NULL);
    CHECK(s2a_libpack_hits() - h0 == 1, "%s: real VS library hit", tag);
    *out_drew = drew;
    if (drew) {
        memcpy(first, px, 4);
        for (int i = 1; i < 16; i++) for (int c = 0; c < 4; c++) if (abs((int)px[4 * i + c] - (int)px[c]) > 1) *out_mismatch += 1;   // 整屏同色
        // 不许空转：片元 alpha 恒 1，没盖到的像素是 0。基准必须是真画出来的非黑颜色，否则「各档相同」没有意义
        CHECK(px[3] == 255 && (px[0] | px[1] | px[2]) != 0, "%s real VS draws visible pixels (got %02x %02x %02x %02x)", tag, px[0], px[1], px[2], px[3]);
        if (!real_ref_set) { memcpy(real_ref, px, 4); real_ref_set = 1; }
    }
    close_session(tag);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) return 2;
    case_dir = argv[1];
    char path[1100];
    if (load_vsrc(&syn, argv[2], "window_vs.key")) { fprintf(stderr, "stages: window_vs.key / _b.json / _b.spv missing in %s\n", argv[2]); return 2; }
    snprintf(path, sizeof path, "%s/libpack_window.frag.spv", case_dir); fs_spv = read_file(path, &fs_n);
    snprintf(path, sizeof path, "%s/libpack_window_real.frag.spv", case_dir); fs_real_spv = read_file(path, &fs_real_n);
    if (!fs_spv || !fs_real_spv) return 2;
    const char *real_dir = argc >= 4 ? argv[3] : NULL;   // 可选：私有真实程序的出料目录（同 window_vs.key 格式）
    if (load_vsrc(&csrc, argv[2], "window_cs.key")) { fprintf(stderr, "stages: window_cs.key missing in %s\n", argv[2]); return 2; }
    if (load_vsrc(&psrc, argv[2], "window_ps.key")) { fprintf(stderr, "stages: window_ps.key / _b.json / _b.spv missing in %s\n", argv[2]); return 2; }
    snprintf(path, sizeof path, "%s/libpack_full.vert.spv", case_dir); vs_triv_spv = read_file(path, &vs_triv_n);
    if (!vs_triv_spv) return 2;
    if (real_dir && load_vsrc(&real, real_dir, "window_vs.key")) { fprintf(stderr, "real stages missing in %s\n", real_dir); return 2; }
    memset(&guest_program, 0, sizeof guest_program); guest_program.magic = MAGIC('P','R','O','G'); guest_program.serial = 100; guest_program.nstage = 2;
    memset(guest_stages, 0xff, sizeof guest_stages);
    for (unsigned st = 0; st < 2; st++) {
        uint8_t code[16], ctl[0x760] = {0}, digest[32]; memset(code, (int)(0x30 + st), sizeof code); put32(ctl + 0x20, sizeof ctl);
        put32(ctl + 0x748, 8); put32(ctl + 0x74c, 2); put32(ctl + 0x750, 1); put32(ctl + 0x754, 17); put32(ctl + 0x758, 32);   // 作计算阶段用时的局部尺寸 8×2×1、共享内存等（同 nvk_shader_pool_test source()）
        s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h, ctl, sizeof ctl); s2a_sha256_update(&h, code, sizeof code); s2a_sha256_final(&h, digest);
        if (s2a_source_keep(digest, st, code, sizeof code, ctl, sizeof ctl)) return 2;
        memcpy(guest_keys[st], digest, 8); guest_stages[st] = (uint8_t)st;
    }
    char why[256] = {0}; if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    const uint32_t real_align = (uint32_t)g_dev.properties.limits.minStorageBufferOffsetAlignment;
    printf("real minStorageBufferOffsetAlignment=%u\n", real_align);
    if (real_align > 16) { fprintf(stderr, "FAIL: fixture expects the base device to report 16, got %u\n", real_align); return 2; }

    // 正：三档对齐 × 四种余数：库命中、不现编、画一笔、客体字节与手算期望逐字节相同
    static const uint32_t tiers[3] = {16, 64, 256};
    for (int t = 0; t < 3; t++) {
        const uint32_t A = tiers[t], rems[4] = {0, 4, A / 2, A - 4};
        for (int k = 0; k < 4; k++) {
            char tag[48]; snprintf(tag, sizeof tag, "A%u-rem%u", A, rems[k]);
            compile_calls = 0;
            struct result r = run_case(tag, A, A, rems[k], V_WINDOW);
            CHECK(r.hit == 1 && r.rej == 0 && compile_calls == 0, "%s library hit (hits+%d rejects+%d, live compiles %d)", tag, r.hit, r.rej, compile_calls);
            CHECK(r.drew, "%s one replay draw, none skipped", tag);
            CHECK(r.mismatches == 0, "%s guest bytes (read [B+0x20] -> [B+0x40], c3[0] -> [B+0]) mismatches=%d", tag, r.mismatches);
        }
    }
    // 片元阶段：32 位 + 128 位 + 第二个存储缓冲，两个缓冲各取不同的余数
    for (int t = 0; t < 3; t++) {
        const uint32_t A = tiers[t], rems[4] = {0, 4, A / 2, A - 4};
        for (int k = 0; k < 4; k++) {
            char tag[48]; snprintf(tag, sizeof tag, "ps-A%u-rem%u", A, rems[k]);
            compile_calls = 0;
            struct result r = run_ps_case(tag, A, rems[k], rems[(k + 1) % 4]);
            CHECK(r.hit == 1 && r.rej == 0 && compile_calls == 0, "%s library hit (hits+%d rejects+%d, live compiles %d)", tag, r.hit, r.rej, compile_calls);
            CHECK(r.drew, "%s one replay draw, none skipped", tag);
            CHECK(r.mismatches == 0, "%s guest bytes (FS: 32-bit, 128-bit, second SSBO) mismatches=%d", tag, r.mismatches);
        }
    }
    // 计算阶段：32 位 + 128 位读写，A × 余数同图形
    for (int t = 0; t < 3; t++) {
        const uint32_t A = tiers[t], rems[4] = {0, 4, A / 2, A - 4};
        for (int k = 0; k < 4; k++) {
            char tag[48]; snprintf(tag, sizeof tag, "cs-A%u-rem%u", A, rems[k]);
            compile_calls = 0;
            struct result r = run_cs_case(tag, A, rems[k], V_WINDOW);
            CHECK(r.hit == 1 && r.rej == 0 && compile_calls == 0, "%s library hit (hits+%d rejects+%d, live compiles %d)", tag, r.hit, r.rej, compile_calls);
            CHECK(r.mismatches == 0, "%s guest bytes (CS: 32-bit, 128-bit) mismatches=%d", tag, r.mismatches);
        }
    }
    // 计算：旧库（无窗口合同）+ 非对齐基址 → 具名 MISS，走现编（现编桩无结果 = 不派发，客体不变）；对齐基址照用
    {
        compile_calls = 0;
        struct result a = run_cs_case("cs-old-aligned", 64, 0, V_OLD_ABI);
        CHECK(a.hit == 1 && a.mismatches == 0 && compile_calls == 0, "old CS library, aligned base: still used (hit %d mismatches %d compiles %d)", a.hit, a.mismatches, compile_calls);
        compile_calls = 0;
        struct result b = run_cs_case("cs-old-unaligned", 64, 4, V_OLD_ABI);
        CHECK(b.hit == 0 && b.mismatches != 0 && compile_calls == 1, "old CS library, unaligned base: library refused, live compile requested (hit %d compiles %d)", b.hit, compile_calls);
    }
    // Same non-aligned window draw and S2SB delivery, with and without proof for c0+0x644.
    pair_binst = 1;
    for (unsigned reads = 0; reads <= 1; reads++) {
        const char *tag = reads ? "pair-binst-read" : "pair-binst-unread";
        compile_calls = 0; binst_copies = paired_hits = 0;
        struct result r = run_case(tag, 64, 64, 4, reads ? V_PAIR_READ : V_PAIR_UNUSED);
        CHECK(paired_hits == 1 && r.hit == 1 && r.rej == 0 && compile_calls == 0 && r.drew && !r.mismatches,
              "%s: S2SB window library hits, indirect draw runs and guest writes land", tag);
        printf("%s: hits=%d draws=%d base-instance copies=%u\n", tag, r.hit, r.drew, binst_copies);
        CHECK(binst_copies == reads, "%s: base-instance copies %u, expected %u", tag, binst_copies, reads);
    }
    pair_binst = 0;
    // 门：旧库（无窗口合同）。对齐基址照旧用库；非对齐基址必须拒库、走现编（现编桩无结果 = 不画），不返回成功兜底
    {
        compile_calls = 0;
        struct result a = run_case("old-aligned", 64, 64, 0, V_OLD_ABI);
        CHECK(a.hit == 1 && a.drew && a.mismatches == 0 && compile_calls == 0, "old library, aligned base: still used (hit %d drew %d mismatches %d compiles %d)", a.hit, a.drew, a.mismatches, compile_calls);
        compile_calls = 0;
        struct result b = run_case("old-unaligned", 64, 64, 4, V_OLD_ABI);
        CHECK(b.hit == 0 && !b.drew && compile_calls == 1, "old library, unaligned base: library refused, live compile requested (hit %d drew %d compiles %d)", b.hit, b.drew, compile_calls);
    }
    // 坏合同：掩码 id 指不到常量 / profile 与设备对齐不一致 → 具名拒绝、不画
    {
        compile_calls = 0;
        struct result a = run_case("bad-mask-id", 64, 64, 4, V_BAD_MASK_ID);
        CHECK(a.hit == 0 && !a.drew && a.rej == 1, "mask id not a constant: rejected by name (hit %d rejects %d drew %d)", a.hit, a.rej, a.drew);
        struct result b = run_case("profile-mismatch", 64, 256, 4, V_WINDOW);
        CHECK(b.hit == 0 && !b.drew && b.rej == 1, "profile alignment != device alignment: rejected by name (hit %d rejects %d drew %d)", b.hit, b.rej, b.drew);
    }
    if (real_dir) {
        // 私有真实程序：同样三档对齐 × 四种余数，整屏颜色与第一档一致；对齐 / 非对齐必须画出同一个颜色
        for (int t = 0; t < 3; t++) {
            const uint32_t A = tiers[t], rems[4] = {0, 4, A / 2, A - 4};
            for (int k = 0; k < 4; k++) {
                char tag[48]; snprintf(tag, sizeof tag, "real-A%u-rem%u", A, rems[k]);
                int drew = 0, bad = 0; uint8_t px[4] = {0};
                const int rc = run_real(tag, A, rems[k], &drew, &bad, px);
                const int same = real_ref_set && abs((int)px[0] - real_ref[0]) <= 1 && abs((int)px[1] - real_ref[1]) <= 1 && abs((int)px[2] - real_ref[2]) <= 1;
                printf("  %s pixel %02x %02x %02x %02x (ref %02x %02x %02x %02x) drew=%d\n", tag, px[0], px[1], px[2], px[3], real_ref[0], real_ref[1], real_ref[2], real_ref[3], drew);
                CHECK(rc == 0 && drew, "%s real VS: one replay draw", tag);
                CHECK(bad == 0 && same, "%s real VS pixel (uniform=%d same-as-aligned-reference=%d)", tag, bad == 0, same);
            }
        }
    }
    s2a_vk_device_destroy(&g_dev, 1);
    printf("libpack SSBO window: failures=%d\n", g_fail); return g_fail ? 1 : 0;
}
