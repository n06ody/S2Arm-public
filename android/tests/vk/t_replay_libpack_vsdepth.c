// 图形 LibPack 的只有顶点阶段的深度程序（NVN 阶段位 0x1）：出包预编库的真实容器 → 生产取库合成（libpack_vk.c）→ 生产解析
// （allow_vs_only）/ 布局（nvk_shader.c）→ 回放绘制核心（rp_draw.c）→ Vulkan（lavapipe + 校验层）。
// 同一条命令里先画一笔 vs+fs（片元带支撑缓冲绑定，写颜色），再解绑片元、换成只有深度附件的通道画一笔只有 vs 的（z = 0.5，深度测试 ALWAYS、写深度），
// 读回深度。第二笔取到的必须是库里的真 vs（不造 fs），且前一笔留在后端的片元绑定不能带过来（坑记录「程序里没有的阶段不发绑定」）。
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
static int test_outputs;
static const char *case_dir;
static prog_t guest_program; static uint8_t guest_keys[6][8], guest_stages[6];
static p_set_rt target_color, target_depth; static tex_t color_tex, depth_tex; static nvn_gpu_view color_view, depth_view;
static int pass_mode;   // 0 = 颜色通道（vs+fs 那一笔），1 = 只有深度附件的通道（只有 vs 那一笔）
int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t rp_cpu_write_gen;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    if (serial != guest_program.serial) return NULL;
    memcpy(keys, guest_keys, sizeof guest_keys); memcpy(stage, guest_stages, sizeof guest_stages); *ok = 3; return &guest_program;
}
static int guest_depth_zero = 1;
int rp_depth_zero(void) { return guest_depth_zero; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 1; }   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身）
uint64_t rp_seg_serial(void) { return 1; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) { (void)a; (void)n; (void)m; (void)s; return 0; }
// 本夹具没有存储缓冲声明；空存储语义由真实 rp_buf GPU 用例验证。
nvn_gpu_buf rp_buf_for_null_storage(void) { return NULL; }
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t a, uint64_t n, nvn_gpu_buf *b, uint64_t *o) { (void)c; (void)a; (void)n; (void)b; (void)o; return 0; }
int rp_buf_gpu_pending(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }   // 本夹具没有 GPU 写回：恒无还没落地的写
// #958 批量解析：逐段转给上面的单段替身（口径一致）
int rp_buf_resolve_multi(int n, const uint64_t *a, const uint64_t *l, nvn_gpu_buf *b, uint64_t *o, unsigned char *ok) {
    for (int i = 0; i < n; i++) ok[i] = l[i] && rp_buf_resolve(NULL, a[i], l[i], &b[i], &o[i]);
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
const p_set_rt *rp_pass_target(void) { return pass_mode ? &target_depth : &target_color; }
int rp_pass_slot_attached(int i) { return !pass_mode && i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = *h = 4; }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { *w = *h = 4; }   // 只有深度的通道没有颜色槽：帧缓冲按 4×4
void rp_pass_note_draw(int d) { (void)d; }
// #950 新增的回放核心依赖：替身取保守中性值（写日志恒报「写过」= 不让暂存命中；没有待折清除、不缩渲染区域）
uint64_t rp_write_mark(void) { return 0; }
int rp_write_touched(uint64_t mark, uint64_t addr, uint64_t len) { (void)mark; (void)addr; (void)len; return 1; }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask) { (void)c; (void)color_write_mask; }
void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }   // main efc48c8b2：rp_draw 每笔绘制都调，直接链 rp_draw.c 的用例要补空替身
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *ts, int n) { (void)c; (void)ts; (void)n; }
int rp_pass_parked(void) { return 0; }
static int pass_began;   // 开过几次通道
int rp_pass_begin(nvn_gpu_cmd c) {
    pass_began++;
    nvn_gpu_pass_desc pass = {0};
    if (!pass_mode) {
        pass.ncolor = 1; pass.color[0].view = color_view;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    } else {
        pass.depth.view = depth_view; pass.depth.load = NVN_GPU_LOAD_CLEAR; pass.depth.store = NVN_GPU_STORE_STORE; pass.clear_depth = 1.0f;
    }
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
    (void)session; (void)generation; (void)req; compile_calls++;
    if (!test_outputs) return -1;
    char path[1100]; size_t n=0;
    snprintf(path,sizeof path,"%s/mrt-ordinary.json",case_dir);
    uint8_t *data=read_file(path,&n);
    if (!data) return -1;
    int rc=emit(sink,(s2a_direct_bytes){data,n}); free(data); return rc;
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


// 容器里的库：vs 一条、fs 一条（_b.json + _b.spv 各一）；fs 不带 _b.json 绑定，.spv 声明的 support_buffer 由合成按 .spv 补出
static void make_library(const char *tag, const uint8_t *vs_spv, size_t vs_n, const uint8_t *fs_spv, size_t fs_n) {
    char root[1024], dir[1100];
    snprintf(root, sizeof root, "%s/lp-%s", case_dir, tag);
    mkdir(root, 0700);
    snprintf(dir, sizeof dir, "%s/shaders", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct", root); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN); mkdir(dir, 0700);
    char hx[2][17], jvs[200], jfs[200];
    for (int s = 0; s < 2; s++) for (int b = 0; b < 8; b++) snprintf(hx[s] + 2 * b, 3, "%02x", guest_keys[s][b]);
    snprintf(jvs, sizeof jvs, "{\"stage\":\"vs\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hx[0]);
    snprintf(jfs, sizeof jfs, "{\"stage\":\"ps\",\"dm_ctl\":\"%s\",\"bindings\":[]}", hx[1]);
    struct rec r[4] = {
        {key64(guest_keys[0]), 1, (const uint8_t *)jvs, strlen(jvs)}, {key64(guest_keys[0]), 52, vs_spv, vs_n},
        {key64(guest_keys[1]), 1, (const uint8_t *)jfs, strlen(jfs)}, {key64(guest_keys[1]), 52, fs_spv, fs_n},
    };
    CHECK(write_container(dir, r, 4) == 0, "%s: library container written", tag);
    setenv("S2A_SHADER_REVISION_ROOT", root, 1); unsetenv("S2A_DIRECT_CACHE_DIR");
}

static void clip_control_draws(nvn_gpu_tex depth, uint64_t signal, int clamp_only) {
    const char *tag = clamp_only ? "depthClamp control" : "clipControl";
    const float expected[] = {1.0f, clamp_only ? 0.0f : 0.25f, 1.0f};
    nvn_gpu_buf readback = make_buf(3 * 64, NVN_GPU_MEM_SHARED, NULL);
    memset(buf_cpu(readback), 0xee, 3 * 64);
    nvn_gpu_cmd cmd = begin_cmd();
    const p_draw draw = {.prim = 4, .count = 3, .instances = 1};
    p_depth_clamp clamp = {0};
    guest_depth_zero = 1;
    CHECK(rp_state_op(OP_DEPTH_CLAMP, &clamp, sizeof clamp), "%s: disable depth clamp", tag);
    rp_dirty_all();   // 与 rp_exec.c 开新段同口径。段内接下来的笔保持回放状态可复用。
    for (unsigned i = 0; i < 3; i++) {
        // 三笔共用同一命令缓冲和程序，每遍只清同一张深度图。测试臂只切近裁模式。
        if (clamp_only) {
            clamp.on = i == 1;
            CHECK(rp_state_op(OP_DEPTH_CLAMP, &clamp, sizeof clamp), "%s: switch depth clamp", tag);
        } else guest_depth_zero = i != 1;
        pass_began = 0;
        rp_draw(cmd, &draw, NULL, &draw);
        CHECK(pass_began == 1, "%s pass %u opens", tag, i);
        if (pass_began) {
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, depth, 0, 0, NULL, readback, i * 64, 16, 64);
        }
    }
    nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &e)), "%s commit: %s", tag, e.why);
    CHECK(OK(nvn_gpu_fence_wait(signal, 5000000000ull, &e)), "%s wait: %s", tag, e.why);
    for (unsigned pass = 0; pass < 3; pass++) {
        float got[16];
        memcpy(got, buf_cpu(readback) + pass * 64, sizeof got);
        unsigned matched = 0;
        for (unsigned i = 0; i < 16; i++) matched += got[i] == expected[pass];
        printf("%s pass %u depth=%g expected=%g pixels=%u/16\n", tag, pass,
               (double)got[10], (double)expected[pass], matched);
        CHECK(matched == 16, "%s pass %u depth pixels differ", tag, pass);
    }
    nvn_gpu_buf_release(readback, NULL, NULL);
}

#include "t_libpack_mrt.inc"

int main(int argc, char **argv) {
    const int clip_control = argc == 3 && !strcmp(argv[2], "--clip-control");
    test_outputs = argc == 3 && !clip_control;
    if (argc != 2 && !clip_control && !test_outputs) return 2;
    case_dir = argv[1];
    char path[1100];
    size_t vs_n = 0, fs_n = 0;
    snprintf(path, sizeof path, "%s/%s.vert.spv", argv[1], clip_control ? "libpack_clip_control" : "libpack_depth"); uint8_t *vs_spv = read_file(path, &vs_n);
    snprintf(path, sizeof path, "%s/%s.spv", argv[1], test_outputs ? argv[2] : "libpack_support.frag"); uint8_t *fs_spv = read_file(path, &fs_n);
    if (!vs_spv || !fs_spv) return 2;
    memset(&guest_program, 0, sizeof guest_program); guest_program.magic = MAGIC('P','R','O','G'); guest_program.serial = 100; guest_program.nstage = 2;
    memset(guest_stages, 0xff, sizeof guest_stages);
    for (unsigned st = 0; st < 2; st++) {
        uint8_t code[16], ctl[0x760] = {0}, digest[32]; memset(code, (int)(0x10 + st), sizeof code); put32(ctl + 0x20, sizeof ctl);
        s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h, ctl, sizeof ctl); s2a_sha256_update(&h, code, sizeof code); s2a_sha256_final(&h, digest);
        if (s2a_source_keep(digest, st, code, sizeof code, ctl, sizeof ctl)) return 2;
        memcpy(guest_keys[st], digest, 8); guest_stages[st] = (uint8_t)st;
    }
    char why[256] = {0}; if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "libpack-vsdepth";
    if (!open_backend()) return 1;
    make_library(clip_control ? "clip-control" : "vsonly", vs_spv, vs_n, fs_spv, fs_n);
    rp_shader_reset(); rp_state_reset();
    p_bind_program_id bind = {&guest_program, guest_program.serial, clip_control ? 1u : 0x1fu, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "OP_BIND_PROGRAM accepted");
    dpool_t tp = {0}, sp = {0}; tp.n = 7; sp.n = 9; p_set_pool t = {&tp}, sm = {&sp};
    CHECK(rp_state_op(OP_SET_TEX_POOL, &t, sizeof t) && rp_state_op(OP_SET_SAMP_POOL, &sm, sizeof sm), "descriptor pools accepted");
    if (test_outputs) {
        free(vs_spv); free(fs_spv);
        return test_mrt(!strcmp(argv[2], "mrt-uint"));
    }
    nvn_gpu_tex ctex = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); color_view = make_view(ctex);
    nvn_gpu_tex dtex = make_tex(4, 4, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED); depth_view = make_view(dtex);
    memset(&color_tex, 0, sizeof color_tex); color_tex.d.target = NVN_TEXTURE_TARGET_2D; color_tex.d.format = NVN_FORMAT_RGBA8;
    color_tex.d.width = color_tex.d.height = 4; color_tex.d.depth = 1; color_tex.d.levels = 1; color_tex.serial = 7;
    memset(&depth_tex, 0, sizeof depth_tex); depth_tex.d.target = NVN_TEXTURE_TARGET_2D; depth_tex.d.format = NVN_FORMAT_DEPTH32F;
    depth_tex.d.width = depth_tex.d.height = 4; depth_tex.d.depth = 1; depth_tex.d.levels = 1; depth_tex.serial = 8;
    memset(&target_color, 0, sizeof target_color); target_color.n = 1; target_color.color[0] = &color_tex;
    memset(&target_depth, 0, sizeof target_depth); target_depth.depth = &depth_tex;

    ds_st ds = {0}; ds.dt = 1; ds.dw = 1; ds.df = 8;   // 深度测试开、写深度、ALWAYS（同后端 C76）
    p_bind_state bs = {5, {0}}; memcpy(bs.bytes, &ds, sizeof ds);
    CHECK(rp_state_op(OP_BIND_STATE, &bs, sizeof bs), "depth state accepted");

    if (clip_control) {
        nvn_gpu_caps caps;
        nvn_gpu_caps_get(&caps);
        CHECK(caps.caps & NVN_GPU_CAP_DEPTH_CLIP_CTL, "clip control capability");
        pass_mode = 1;
        const uint64_t hits = s2a_libpack_hits();
        clip_control_draws(dtex, 1, 0);
        clip_control_draws(dtex, 2, 1);
        CHECK(s2a_libpack_hits() > hits && compile_calls == 0, "clip control uses LibPack vertex shader");
        nvn_gpu_view_release(color_view); nvn_gpu_view_release(depth_view);
        nvn_gpu_tex_release(ctex); nvn_gpu_tex_release(dtex);
        nvn_gpu_err e = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close clip control session");
        s2a_vk_device_destroy(&g_dev, 1);
        free(vs_spv); free(fs_spv);
        printf("libpack clip control: failures=%d\n", g_fail);
        return g_fail ? 1 : 0;
    }

    const uint64_t h0 = s2a_libpack_hits(), j0 = s2a_libpack_rejects();
    nvn_gpu_buf rbc = make_buf(64, NVN_GPU_MEM_SHARED, NULL), rbd = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    memset(buf_cpu(rbc), 0xEE, 64); memset(buf_cpu(rbd), 0xEE, 64);   // 没画时读回仍是哨兵
    nvn_gpu_cmd cmd = begin_cmd(); p_draw draw = {0}; draw.prim = 4; draw.count = 3; draw.instances = 1;
    // 第一笔：vs+fs（片元带支撑缓冲绑定）→ 颜色通道
    pass_mode = 0; pass_began = 0;
    rp_draw(cmd, &draw, NULL, &draw);
    CHECK(pass_began == 1, "vs+fs draw: color pass opened");
    if (pass_began) { nvn_gpu_pass_end(cmd); nvn_gpu_copy_tex_buf(cmd, ctex, 0, 0, NULL, rbc, 0, 16, 64); }
    const uint64_t hits1 = s2a_libpack_hits() - h0;
    // 第二笔：解绑片元（绑 NULL 程序到片元阶段位）→ 只有 vs，只有深度附件的通道
    p_bind_program_id unbind = {NULL, 0, 0x2, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &unbind, sizeof unbind + 8), "fragment stage unbound");
    CHECK(rp_shader_graphics_stages() == 1, "only the vertex stage is bound (mask 0x1), got 0x%x", rp_shader_graphics_stages());
    pass_mode = 1; pass_began = 0;
    uint64_t d0 = 0, k0 = 0; rp_draw_stats(&d0, &k0);
    rp_draw(cmd, &draw, NULL, &draw);
    uint64_t d1 = 0, k1 = 0; rp_draw_stats(&d1, &k1);
    if (pass_began) { nvn_gpu_pass_end(cmd); nvn_gpu_copy_tex_buf(cmd, dtex, 0, 0, NULL, rbd, 0, 16, 64); }
    const uint64_t hits2 = s2a_libpack_hits() - h0 - hits1, rej = s2a_libpack_rejects() - j0;
    nvn_gpu_err e = {0};
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(cmd, 1, on_done, NULL, &e); CHECK(OK(rc), "commit %d %s", rc.code, e.why);
    rc = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(rc), "wait %d %s", rc.code, e.why);

    CHECK(hits1 == 1, "vs+fs: library hit (hits+%llu)", (unsigned long long)hits1);
    for (int i = 0; i < 16; i++) { const uint8_t red[4] = {255, 0, 0, 255}; const uint8_t *v = buf_cpu(rbc) + 4 * i;
        CHECK(!memcmp(v, red, 4), "vs+fs pixel %d got %u,%u,%u,%u", i, v[0], v[1], v[2], v[3]); }
    CHECK(hits2 == 1 && rej == 0 && compile_calls == 0, "vs-only: library hit (hits+%llu rejects+%llu, live compiles %d)", (unsigned long long)hits2, (unsigned long long)rej, compile_calls);
    CHECK(d1 - d0 == 1 && k1 - k0 == 0, "vs-only: one replay draw, none skipped");
    for (int i = 0; i < 16; i++) { float f; memcpy(&f, buf_cpu(rbd) + 4 * i, 4);
        CHECK(f == 0.5f, "vs-only depth %d got %g want 0.5", i, (double)f); }
    nvn_gpu_buf_release(rbc, NULL, NULL); nvn_gpu_buf_release(rbd, NULL, NULL);
    nvn_gpu_view_release(color_view); nvn_gpu_view_release(depth_view); nvn_gpu_tex_release(ctex); nvn_gpu_tex_release(dtex);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close session"); s2a_vk_device_destroy(&g_dev, 1);
    free(vs_spv); free(fs_spv);
    printf("libpack vs-only depth: failures=%d\n", g_fail); return g_fail ? 1 : 0;
}
