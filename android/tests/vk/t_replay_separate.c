// Production replay binds independent slots to one combined Vulkan descriptor.
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "nvn_unimpl.h"
static p_set_rt target; static tex_t target_tex; static nvn_gpu_view target_view;
int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t rp_cpu_write_gen;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) { (void)serial; (void)keys; (void)ok; (void)stage; return NULL; }
static struct rp_prog prog;
static nvn_gpu_view sample_view;
static nvn_gpu_samp sample_sampler, decoy_sampler;
static uint64_t generation = 1, segment;
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) { (void)q; *out = &prog; return 0; }
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 1; }   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身）
uint64_t rp_seg_serial(void) { return segment; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) { (void)a; (void)n; (void)m; (void)s; return 0; }
// 本夹具没有存储缓冲声明；空存储语义由真实 rp_buf GPU 用例验证。
nvn_gpu_buf rp_buf_for_null_storage(void) { return NULL; }
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t a, uint64_t n, nvn_gpu_buf *b, uint64_t *o) { (void)c; (void)a; (void)n; (void)b; (void)o; return 0; }
// #958 批量解析：逐段转给上面的单段替身（口径一致）
int rp_buf_resolve_multi(int n, const uint64_t *a, const uint64_t *l, nvn_gpu_buf *b, uint64_t *o, unsigned char *ok) {
    for (int i = 0; i < n; i++) ok[i] = l[i] && rp_buf_resolve(NULL, a[i], l[i], &b[i], &o[i]);
    return 1;
}
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, const tex_t **t) { (void)id; (void)c; (void)t; return NULL; }
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t tt, uint8_t cls, int elem, nvn_gpu_cmd c, const tex_t **t) { (void)tt; (void)cls; (void)elem; (void)c; (void)t; return id == 7 ? sample_view : NULL; }
nvn_gpu_samp rp_samp_by_id(int id) { return id == 3 ? sample_sampler : id == 0 ? decoy_sampler : NULL; }
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }   // #1072：替身不判过滤能力
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) { (void)id; (void)t; (void)v; (void)h; return 0; }
uint64_t rp_tex_reg_gen(void) { return generation; }
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
void rp_pass_land_reads(nvn_gpu_cmd c, const tex_t *const *ts, int n) { (void)c; (void)ts; (void)n; }
int rp_pass_parked(void) { return 0; }
int rp_pass_begin(nvn_gpu_cmd c) {
    nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = target_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4; pass.layers = 1; nvn_gpu_pass_begin(c, &pass); return 1;
}
void rp_pass_flush(nvn_gpu_cmd c) { nvn_gpu_pass_end(c); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char why[256] = {0}; if (s2a_vk_device_create(&g_dev, why, sizeof why)) return 2;
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "separate-core";
    if (!open_backend()) return 1;
    nvn_gpu_layout_ent *ent = &prog.ent[1][0];
    ent->set = 0; ent->binding = 0; ent->cls = NVN_GPU_RES_TEX_SAMP; ent->src_kind = NVN_RB_TEX;
    ent->src_reg = 130; ent->src_reg2 = 4; ent->count = 1; ent->access = NVN_GPU_ACC_R; ent->stages = 1u << NVN_GPU_SH_FS;
    prog.nent[1] = 1; prog.color_out_mask = 0xffffffffu;
    nvn_gpu_layout fs_layout = {0}; fs_layout.ent = ent; fs_layout.nent = 1;
    void *vsm = NULL, *fsm = NULL;
    CHECK(load_shader_pair(argv[1], "separate.vert", "separate.frag", NULL, &fs_layout, &prog.sh[0], &prog.sh[1], &vsm, &fsm), "load separate-source shaders");
    nvn_gpu_tex input = make_tex(2, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    sample_view = make_view(input);
    nvn_gpu_tex output = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); target_view = make_view(output);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    memset(&target_tex, 0, sizeof target_tex); target_tex.d.target = NVN_TEXTURE_TARGET_2D; target_tex.d.format = NVN_FORMAT_RGBA8;
    target_tex.d.width = target_tex.d.height = 4; target_tex.d.depth = target_tex.d.levels = 1; target_tex.serial = 7;
    memset(&target, 0, sizeof target); target.n = 1; target.color[0] = &target_tex;
    samp_par sp = {0}; sp.ws = NVN_WRAP_MODE_REPEAT; nvn_gpu_samp repeat = NULL, clamp = NULL; nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_samp_create(&sp, &repeat, &e)), "create repeat sampler");
    sp.ws = NVN_WRAP_MODE_CLAMP_TO_EDGE; CHECK(OK(nvn_gpu_samp_create(&sp, &clamp, &e)), "create clamp sampler");
    sample_sampler = repeat; decoy_sampler = clamp; rp_state_reset();
    p_bind_handles bh = {0}; bh.stage = 1; bh.first = 130; bh.n = 1; bh.h[0] = 7; rp_state_op(OP_BIND_TEX, &bh, sizeof bh);
    bh.first = 4; bh.h[0] = (uint64_t)3 << 20; rp_state_op(OP_BIND_SAMP, &bh, sizeof bh);
    const uint8_t pixels[8] = {255,0,0,255, 0,255,0,255};
    for (int round = 0; round < 2; ++round) {
        nvn_gpu_cmd cmd = begin_cmd();
        // Production nvn_be_sub_begin advances the segment and dirties all
        // backend state, even when a new command reuses the same address.
        segment++; rp_dirty_all();
        if (!round) nvn_gpu_tex_upload(cmd, input, 0, 0, NULL, pixels, 8, 8);
        else { sample_sampler = clamp; generation++; }
        p_draw draw = {0}; draw.prim = 4; draw.count = 3; draw.instances = 1;
        rp_draw(cmd, &draw, NULL, &draw); nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, output, 0, 0, NULL, rb, 0, 16, 64);
        nvn_gpu_rc rc = nvn_gpu_cmd_commit(cmd, (uint64_t)round + 1, on_done, NULL, &e);
        CHECK(OK(rc), "commit round %d: code=%d %s", round, rc.code, e.why);
        if (!OK(rc)) break;
        rc = nvn_gpu_fence_wait((uint64_t)round + 1, 5000000000ull, &e);
        CHECK(OK(rc), "wait round %d: code=%d %s", round, rc.code, e.why);
        if (!OK(rc)) break; // 未完成时读回是旧数据，不能当本轮像素结果
        for (int i = 0; i < 16; ++i) {
            const uint8_t *p = buf_cpu(rb) + i * 4;
            CHECK(p[0] == (round ? 0 : 255) && p[1] == (round ? 255 : 0) && p[2] == 0 && p[3] == 255,
                  "round %d pixel %d: %u,%u,%u,%u", round, i, p[0], p[1], p[2], p[3]);
        }
    }
    uint64_t drawn = 0, skipped = 0; rp_draw_stats(&drawn, &skipped);
    CHECK(drawn == 2 && skipped == 0, "two draws and no skips");
    nvn_gpu_shader_release(prog.sh[0]); nvn_gpu_shader_release(prog.sh[1]); free(vsm); free(fsm);
    nvn_gpu_samp_release(repeat); nvn_gpu_samp_release(clamp); nvn_gpu_view_release(sample_view); nvn_gpu_tex_release(input);
    nvn_gpu_view_release(target_view); nvn_gpu_tex_release(output); nvn_gpu_buf_release(rb, NULL, NULL);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close session"); s2a_vk_device_destroy(&g_dev, 1);
    printf("separate core: repeat red, clamp green, failures=%d\n", g_fail); return g_fail ? 1 : 0;
}
