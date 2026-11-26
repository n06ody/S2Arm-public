// 图形 LibPack 的 FS c0 驱动常量缓冲（drvcb bank 0）：出包预编库的真实容器 → 生产取库合成（libpack_vk.c）→ 生产解析 /
// 布局（nvk_shader.c layout_ent：slot 0 → NVN_RB_DRV0）→ 回放绘制核心（rp_draw.c 按 0xA00 驱动段供数）→ Vulkan（lavapipe + 校验层）。
// 片元着色器读 c0 的 0x600（槽 15 存储地址的低字）/ 0x604（高字）当颜色：像素对上绑定的存储缓冲地址字节才算读对。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "nvn_unimpl.h"
#include "direct/compile_request.h"
#include "direct/source_store.h"
#include "direct/s2a_direct.h"
#include "direct/sha256.h"
#include "direct/jv.h"
#include "dmsh_chain.h"
#include "direct/libpack_vk.h"
#include <sys/stat.h>

static int compile_calls;
static const char *case_dir;
static prog_t guest_program; static uint8_t guest_keys[6][8], guest_stages[6];
static p_set_rt target; static tex_t target_tex; static nvn_gpu_view target_view; static nvn_gpu_tex target_tex_backend;
int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t rp_cpu_write_gen;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    if (serial != guest_program.serial) return NULL;
    memcpy(keys, guest_keys, sizeof guest_keys); memcpy(stage, guest_stages, sizeof guest_stages); *ok = 3; return &guest_program;
}
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 1; }   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身）
uint64_t rp_seg_serial(void) { return 1; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) { (void)a; (void)n; (void)m; (void)s; return 0; }
// 本夹具没有存储缓冲声明；空存储语义由真实 rp_buf GPU 用例验证。
nvn_gpu_buf rp_buf_for_null_storage(void) { return NULL; }
int rp_buf_resolve_binding(nvn_gpu_cmd c, uint64_t a, uint64_t n, nvn_gpu_buf *b, uint64_t *o, enum rp_buf_binding binding) {
    (void)binding; (void)c; (void)a; (void)n; (void)b; (void)o; return 0; }
int rp_buf_gpu_pending(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }   // 本夹具没有 GPU 写回：恒无还没落地的写
// #958 批量解析：逐段转给上面的单段替身（口径一致）
int rp_buf_resolve_multi_binding(int n, const uint64_t *a, const uint64_t *l, nvn_gpu_buf *b, uint64_t *o, unsigned char *ok, enum rp_buf_binding binding) {
    for (int i = 0; i < n; i++) ok[i] = l[i] && rp_buf_resolve_binding(NULL, a[i], l[i], &b[i], &o[i], binding);
    return 1;
}
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, tex_t *t) { (void)id; (void)c; if (t) memset(t, 0, sizeof *t); return NULL; }
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) { (void)t; (void)v; (void)c; return NULL; }
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t tt, uint8_t cls, int elem, nvn_gpu_cmd c, tex_t *t) { (void)id; (void)tt; (void)cls; (void)elem; (void)c; if (t) memset(t, 0, sizeof *t); return NULL; }
nvn_gpu_view rp_view_by_ubo_handle_decl(int i,uint8_t t,uint8_t k,nvn_gpu_cmd c,tex_t *o){return rp_view_by_id_decl(i,t,k,0,c,o);}   // 夹具不另判「没登记」：同 rp_view_by_id_decl 替身
void rp_decl_probe_begin(void){} int rp_decl_probe_end(void){return 0;}
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c,int samp,int base,uint32_t n,uint8_t dtt,uint8_t dcls,const char **why){(void)c;(void)samp;(void)base;(void)n;(void)dtt;(void)dcls;if(why)*why="pool_table_unresolved";return NULL;}   // 本夹具不出整池数组
// 此夹具未提供查询采样器证明，不使用内部查询对象。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
nvn_gpu_samp rp_samp_by_id(int id) { (void)id; return NULL; }
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }   // #1072：替身不判过滤能力
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) { (void)pb; (void)pn; return rp_samp_for_tex(sid, tid); }   // 同 rp_samp_for_tex：替身不给默认采样器
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
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { (void)w; (void)h; }   // 有附件的通道：不动尺寸
void rp_pass_note_draw(int d) { (void)d; }
// #950 新增的回放核心依赖：替身取保守中性值（写日志恒报「写过」= 不让暂存命中；没有待折清除、不缩渲染区域）
uint64_t rp_write_mark(void) { return 0; }
int rp_write_touched(uint64_t mark, uint64_t addr, uint64_t len) { (void)mark; (void)addr; (void)len; return 1; }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask) { (void)c; (void)color_write_mask; }
void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }   // main efc48c8b2：rp_draw 每笔绘制都调，直接链 rp_draw.c 的用例要补空替身
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *ts, int n) { (void)c; (void)ts; (void)n; }
int rp_pass_parked(void) { return 0; }
static int pass_began;   // 出库拒 → 没画 → 没开通道：读回与收通道只在开过通道时做
int rp_pass_begin(nvn_gpu_cmd c) {
    pass_began = 1;
    nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = target_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4; pass.layers = 1; nvn_gpu_pass_begin(c, &pass); return 1;
}
void rp_pass_flush(nvn_gpu_cmd c) { nvn_gpu_pass_end(c); }
static const char profile_json[] = "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    memset(c, 0, sizeof *c); c->session = 4; c->generation = 6; c->direct_abi = 1;
    c->profile_json = profile_json; c->profile_bytes = sizeof profile_json - 1; c->profile_hash = "";
    c->translator_closure = "libpack-c0-test"; c->storage_buffer_offset_alignment = 16; return 0;
}
// 现编不该被走到：出库命中（或出库拒后的退路）都不会有结果；调用次数就是证据。
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
// 容器：头 48 字节 + 分片表 + (键, 种类) 升序记录（格式同 libpack_vk_test.c / LibPack.cs）
static int write_container(const char *dir, struct rec *r, int n) {
    qsort(r, (size_t)n, sizeof *r, rec_cmp);
    char path[1200]; snprintf(path, sizeof path, "%s/lib.000.pak", dir);   // 调用方的 dir 最长 1099 字节：留够 12 字节后缀，否则 -Werror=format-truncation
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    uint64_t off[8], at = 0;
    for (int i = 0; i < n; i++) { off[i] = at; if (fwrite(r[i].d, 1, r[i].n, f) != r[i].n) { fclose(f); return -1; } at += r[i].n; }
    if (fclose(f)) return -1;
    uint8_t idx[64 + 24 * 8]; memset(idx, 0, sizeof idx);
    memcpy(idx, "S2ALIB\0\0", 8); put32(idx + 8, 1); put32(idx + 12, (uint32_t)n); put32(idx + 16, 1);
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

// 一个用例：用给定的 FS _b.json 现写一份库（独立资源根，库句柄按根缓存），绑定 FS 的存储缓冲槽 15 = addr，画一个全屏三角形，读回 4×4。
//   return 1 = 画了（pixels 有效），0 = 没画（出库拒 → 现编没有结果 → 回放按未解析跳过）
static int run_case(const char *tag, const char *fs_json_fmt, const uint8_t *vs_spv, size_t vs_n, const uint8_t *fs_spv, size_t fs_n,
                    uint64_t ssbo_addr, uint8_t pixels[64], uint64_t *rejects_delta, uint64_t *hits_delta) {
    char root[1024], dir[1100];
    snprintf(root, sizeof root, "%s/lp-%s", case_dir, tag);
    mkdir(root, 0700);
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    char hx[2][17], jvs[200], jfs[600];
    for (int s = 0; s < 2; s++) for (int b = 0; b < 8; b++) snprintf(hx[s] + 2 * b, 3, "%02x", guest_keys[s][b]);
    snprintf(jvs, sizeof jvs, "{\"stage\":\"vs\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hx[0]);
    snprintf(jfs, sizeof jfs, fs_json_fmt, hx[1]);
    struct rec r[4] = {
        {key64(guest_keys[0]), 1, (const uint8_t *)jvs, strlen(jvs)}, {key64(guest_keys[0]), 52, vs_spv, vs_n},
        {key64(guest_keys[1]), 1, (const uint8_t *)jfs, strlen(jfs)}, {key64(guest_keys[1]), 52, fs_spv, fs_n},
    };
    CHECK(write_container(dir, r, 4) == 0, "%s: library container written", tag);
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
    rp_shader_reset(); rp_state_reset();
    guest_program.serial++;
    p_bind_program_id bind = {&guest_program, guest_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "%s: OP_BIND_PROGRAM accepted", tag);
    dpool_t tp = {0}, sp = {0}; tp.n = 7; sp.n = 9; p_set_pool t = {&tp}, s = {&sp};
    CHECK(rp_state_op(OP_SET_TEX_POOL, &t, sizeof t) && rp_state_op(OP_SET_SAMP_POOL, &s, sizeof s), "%s: descriptor pools accepted", tag);
    p_bind_buf ssbo = {1, 15, ssbo_addr, 256};   // 片元阶段（NVN 阶段 1）的存储缓冲槽 15：DRV0 的 0x510 + 16×15 = 0x600 处放地址、0x608 处放长度
    CHECK(rp_state_op(OP_BIND_SSBO, &ssbo, sizeof ssbo), "%s: FS storage slot 15 bound", tag);
    const uint64_t h0 = s2a_libpack_hits(), j0 = s2a_libpack_rejects();
    uint64_t d0 = 0, k0 = 0; rp_draw_stats(&d0, &k0);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    memset(buf_cpu(rb), 0xEE, 64);   // 没画时读回仍是哨兵，不会被误当像素
    nvn_gpu_cmd cmd = begin_cmd(); p_draw draw = {0}; draw.prim = 4; draw.count = 3; draw.instances = 1;
    pass_began = 0;
    rp_draw(cmd, &draw, NULL, &draw);
    nvn_gpu_err e = {0};
    if (pass_began) {
        static uint64_t fence_no;   // 提交的信号值必须严格递增
        nvn_gpu_pass_end(cmd); nvn_gpu_copy_tex_buf(cmd, target_tex_backend, 0, 0, NULL, rb, 0, 16, 64);
        nvn_gpu_rc rc = nvn_gpu_cmd_commit(cmd, ++fence_no, on_done, NULL, &e); CHECK(OK(rc), "%s: commit %d %s", tag, rc.code, e.why);
        rc = nvn_gpu_fence_wait(fence_no, 5000000000ull, &e); CHECK(OK(rc), "%s: wait %d %s", tag, rc.code, e.why);
    } else nvn_gpu_cmd_abort(cmd);   // 没画：没有可提交的内容
    uint64_t d1 = 0, k1 = 0; rp_draw_stats(&d1, &k1);
    memcpy(pixels, buf_cpu(rb), 64); nvn_gpu_buf_release(rb, NULL, NULL);
    *hits_delta = s2a_libpack_hits() - h0; *rejects_delta = s2a_libpack_rejects() - j0;
    return d1 - d0 == 1 && k1 - k0 == 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    case_dir = argv[1];
    char path[1100];
    size_t vs_n = 0, fs_n = 0;
    snprintf(path, sizeof path, "%s/libpack_full.vert.spv", argv[1]); uint8_t *vs_spv = read_file(path, &vs_n);
    snprintf(path, sizeof path, "%s/libpack_c0.frag.spv", argv[1]); uint8_t *fs_spv = read_file(path, &fs_n);
    if (!vs_spv || !fs_spv) return 2;
    memset(&guest_program, 0, sizeof guest_program); guest_program.magic = MAGIC('P','R','O','G'); guest_program.serial = 100; guest_program.nstage = 2;
    memset(guest_stages, 0xff, sizeof guest_stages);
    const char *names[2] = {"vs", "fs"};
    for (unsigned st = 0; st < 2; st++) {
        uint8_t code[16], ctl[0x760] = {0}, digest[32]; memset(code, (int)(0x10 + st), sizeof code); put32(ctl + 0x20, sizeof ctl);
        s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h, ctl, sizeof ctl); s2a_sha256_update(&h, code, sizeof code); s2a_sha256_final(&h, digest);
        if (s2a_source_keep(digest, st, code, sizeof code, ctl, sizeof ctl)) return 2;
        memcpy(guest_keys[st], digest, 8); guest_stages[st] = (uint8_t)st; (void)names;
    }
    char why[256] = {0}; if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "libpack-c0";
    if (!open_backend()) return 1;
    nvn_gpu_tex tex = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); target_view = make_view(tex); target_tex_backend = tex;
    memset(&target_tex, 0, sizeof target_tex); target_tex.d.target = NVN_TEXTURE_TARGET_2D; target_tex.d.format = NVN_FORMAT_RGBA8;
    target_tex.d.width = target_tex.d.height = 4; target_tex.d.depth = 1; target_tex.d.levels = 1; target_tex.serial = 7;
    memset(&target, 0, sizeof target); target.n = 1; target.color[0] = &target_tex;

    static const char FS_BANK0[] = "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[{\"msl_name\":\"fp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":3}]}";
    static const char FS_BANK2[] = "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[{\"msl_name\":\"fp_c2\",\"kind\":\"drvcb\",\"bank\":2,\"arg_id\":3}]}";
    static const char FS_ARG4[]  = "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[{\"msl_name\":\"fp_c0\",\"kind\":\"drvcb\",\"bank\":0,\"arg_id\":4}]}";
    // 槽 15 的存储地址 = 0x80FF40C0_FF804020：0x600 = 0xFF804020（RGBA 字节 20 40 80 FF），0x604 = 0x80FF40C0（C0 40 FF 80）
    const uint64_t addr = 0x80FF40C0FF804020ull;
    uint8_t px[64]; uint64_t rej = 0, hit = 0;

    // 正：bank 0 + SPIR-V 在 binding 22（arg_id 3 × 6 + 阶段 4）声明 set 0 Uniform Block → 库命中，绘制读到槽 15 地址字
    int drew = run_case("bank0", FS_BANK0, vs_spv, vs_n, fs_spv, fs_n, addr, px, &rej, &hit);
    CHECK(hit == 1 && rej == 0 && compile_calls == 0, "bank 0 c0: library hit (hits+%llu rejects+%llu, live compiles %d)", (unsigned long long)hit, (unsigned long long)rej, compile_calls);
    CHECK(drew, "bank 0 c0: one replay draw, none skipped");
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        const uint8_t want[4] = {x < 2 ? 0x20 : 0xC0, 0x40, x < 2 ? 0x80 : 0xFF, x < 2 ? 0xFF : 0x80};
        const uint8_t *v = px + 4 * (4 * y + x);
        int ok = 1; for (int c = 0; c < 4; c++) ok &= abs((int)v[c] - (int)want[c]) <= 1;
        CHECK(ok, "bank 0 c0 pixel (%d,%d) got %02x %02x %02x %02x want %02x %02x %02x %02x", x, y, v[0], v[1], v[2], v[3], want[0], want[1], want[2], want[3]);
    }
    // 负：bank 2（句柄表）/ 错绑定号（arg_id 4 → 28，SPIR-V 没有）都必须出库拒、不画、不被现编兜底成成功
    struct { const char *tag, *fmt; } neg[2] = {{"bank2", FS_BANK2}, {"arg4", FS_ARG4}};
    for (int i = 0; i < 2; i++) {
        compile_calls = 0;
        drew = run_case(neg[i].tag, neg[i].fmt, vs_spv, vs_n, fs_spv, fs_n, addr, px, &rej, &hit);
        CHECK(!drew && hit == 0 && rej == 1, "%s: library rejects (hits+%llu rejects+%llu), draw not made", neg[i].tag, (unsigned long long)hit, (unsigned long long)rej);
    }
    nvn_gpu_err e = {0};
    nvn_gpu_view_release(target_view); nvn_gpu_tex_release(tex);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close session"); s2a_vk_device_destroy(&g_dev, 1);
    free(vs_spv); free(fs_spv);
    printf("libpack FS c0 drvcb: failures=%d\n", g_fail); return g_fail ? 1 : 0;
}
