// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Guest replay to Vulkan pixel readback. Synthetic GS encodes its ordered inputs
// in one column per primitive so permutations are observable, even at equal winding.
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "nvn_unimpl.h"

enum { WIDTH = 8, HEIGHT = 4 };
static p_set_rt target;
static tex_t target_tex;
static nvn_gpu_view target_view;
static nvn_gpu_buf indices;
static struct {
    uint16_t index[16];
    uint32_t args[5];
} guest;
static int pass_open, pass_started;
static struct rp_prog program;

int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t rp_cpu_write_gen;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    (void)serial; (void)keys; (void)ok; (void)stage; return NULL;
}
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 1; }   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身）
uint64_t rp_seg_serial(void) { return 1; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) {
    const uint64_t base = (uintptr_t)&guest;
    if (a < base || n > sizeof guest || a - base > sizeof guest - n) return 0;
    if (m) *m = (const uint8_t *)&guest;
    if (s) *s = sizeof guest;
    return 1;
}
// 本夹具没有存储缓冲声明；空存储语义由真实 rp_buf GPU 用例验证。
nvn_gpu_buf rp_buf_for_null_storage(void) { return NULL; }
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t a, uint64_t n, nvn_gpu_buf *b, uint64_t *o) {
    (void)c;
    if (!indices || !rp_pool_find(a, n, NULL, NULL)) return 0;
    if (b) *b = indices;
    if (o) *o = a - (uintptr_t)&guest;
    return 1;
}
// 本独立夹具不使用 UBOA 间接指针读取。
int rp_buf_read_pointer(nvn_gpu_buf b, uint64_t off, uint64_t addr, uint64_t *value) {
    (void)b; (void)off; (void)addr; (void)value;
    return 0;
}
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
const p_set_rt *rp_pass_target(void) { return &target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = WIDTH; *h = HEIGHT; }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { (void)w; (void)h; }   // 有附件的通道：不动尺寸
void rp_pass_note_draw(int d) { (void)d; }
// #950 新增的回放核心依赖：替身取保守中性值（写日志恒报「写过」= 不让暂存命中；没有待折清除、不缩渲染区域）
uint64_t rp_write_mark(void) { return 0; }
int rp_write_touched(uint64_t mark, uint64_t addr, uint64_t len) { (void)mark; (void)addr; (void)len; return 1; }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask) { (void)c; (void)color_write_mask; } void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *ts, int n) { (void)c; (void)ts; (void)n; }
int rp_pass_parked(void) { return 0; }
int rp_pass_begin(nvn_gpu_cmd c) {
    if (pass_open) return 1;
    nvn_gpu_pass_desc pass = {.ncolor = 1, .w = WIDTH, .h = HEIGHT, .layers = 1};
    pass.color[0] = (nvn_gpu_att){.view = target_view, .load = pass_started ? NVN_GPU_LOAD_LOAD : NVN_GPU_LOAD_CLEAR,
                                 .store = NVN_GPU_STORE_STORE};
    nvn_gpu_pass_begin(c, &pass);
    pass_open = pass_started = 1;
    return 1;
}
int rp_pass_is_open(void) { return pass_open; }
void rp_pass_end(nvn_gpu_cmd c) {
    if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; }
}
void rp_pass_flush(nvn_gpu_cmd c) { rp_pass_end(c); }

int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    (void)q;
    *out = &program;
    return 0;
}

struct topology_case {
    const char *name;
    uint32_t prim, count, first;
    unsigned lines, indexed, indirect, has_last_expected;
    uint16_t input[16];
    uint8_t expected[WIDTH][4];
    uint8_t expected_last[WIDTH][4];
};

static const struct topology_case cases[] = {
    {.name = "quads", .prim = 7, .count = 8, .first = 2,
     .expected = {{2,3,4,255}, {2,4,5,255}, {6,7,8,255}, {6,8,9,255}}},
    {.name = "quad-strip", .prim = 8, .count = 7, .first = 2,
     .expected = {{2,3,4,255}, {4,3,5,255}, {4,5,6,255}, {6,5,7,255}}},
    {.name = "line-loop", .prim = 2, .count = 4, .first = 3, .lines = 1,
     .expected = {{3,4,0,255}, {4,5,0,255}, {5,6,0,255}, {6,3,0,255}}},
    {.name = "polygon", .prim = 9, .count = 5, .first = 4,
     .expected = {{4,5,6,255}, {4,6,7,255}, {4,7,8,255}}},
    {.name = "line-strip", .prim = 3, .count = 4, .first = 3, .lines = 1,
     .expected = {{3,4,0,255}, {4,5,0,255}, {5,6,0,255}}},
    {.name = "indexed-quad-strip", .prim = 8, .count = 10, .first = 2, .indexed = 1,
     .input = {8,4,3,7,6,0xbeef,11,10,9,12},
     .expected = {{10,6,5,255}, {5,6,9,255}, {13,12,11,255}, {11,12,14,255}}},
    {.name = "indexed-polygon", .prim = 9, .count = 10, .first = 2, .indexed = 1,
     .input = {8,4,3,7,6,0xbeef,11,10,9,12},
     .expected = {{10,6,5,255}, {10,5,9,255}, {10,9,8,255}, {13,12,11,255}, {13,11,14,255}}},
    // Native Vulkan inputs: FIRST_VERTEX reverses odd strips' 2nd/3rd vertices and places
    // the fan center last; LAST_VERTEX (Maxwell/NVN default when VK_EXT_provoking_vertex is
    // active) delivers (v2,v1,v3) for odd strips and (v0,v1,v2) for fans.
    {.name = "indirect-strip", .prim = 5, .count = 4, .first = 2, .indirect = 1, .has_last_expected = 1,
     .expected = {{2,3,4,255}, {3,5,4,255}},
     .expected_last = {{2,3,4,255}, {4,3,5,255}}},
    {.name = "indirect-fan", .prim = 6, .count = 4, .first = 2, .indirect = 1, .has_last_expected = 1,
     .expected = {{3,4,2,255}, {4,5,2,255}},
     .expected_last = {{2,3,4,255}, {2,4,5,255}}},
    {.name = "indexed-indirect-strip", .prim = 5, .count = 4, .first = 2, .indexed = 1, .indirect = 1, .has_last_expected = 1,
     .input = {10,4,6,7}, .expected = {{12,6,8,255}, {6,9,8,255}},
     .expected_last = {{12,6,8,255}, {8,6,9,255}}},
    {.name = "indexed-indirect-fan", .prim = 6, .count = 4, .first = 2, .indexed = 1, .indirect = 1, .has_last_expected = 1,
     .input = {10,4,6,7}, .expected = {{6,8,12,255}, {8,9,12,255}},
     .expected_last = {{12,6,8,255}, {12,8,9,255}}},
    // Two primitives per draw. Red sums the main vertex IDs, green the neighbors.
    {.name = "adj-line-list", .prim = 0xA, .count = 8, .first = 2, .lines = 1,
     .expected = {{7,7,4,255}, {15,15,4,255}}},
    {.name = "adj-line-strip", .prim = 0xB, .count = 5, .first = 2, .lines = 1,
     .expected = {{7,7,4,255}, {9,9,4,255}}},
    {.name = "adj-triangle-list", .prim = 0xC, .count = 12, .first = 2,
     .expected = {{12,15,6,255}, {30,33,6,255}}},
    {.name = "adj-triangle-strip", .prim = 0xD, .count = 8, .first = 2,
     .expected = {{12,16,6,255}, {18,18,6,255}}},
};

static void run_case(const struct topology_case *tc, nvn_gpu_shader gs, uint64_t signal) {
    nvn_gpu_err err = {0};
    rp_draw_reset();
    rp_state_reset();
    nvn_unimpl_reset();
    for (int st = 0; st < 3; st++) rp_S.bser[st] = 1;
    program.sh[NVN_GPU_SH_GS] = gs;
    memcpy(guest.index, tc->input, sizeof guest.index);
    if (tc->indirect) {
        const uint32_t args[] = {tc->count, 1, tc->indexed ? 0 : tc->first, tc->indexed ? tc->first : 0, 0};
        memcpy(guest.args, args, sizeof args);
    }
    if (tc->indexed || tc->indirect) indices = make_buf(sizeof guest, NVN_GPU_MEM_SHARED, &guest);
    nvn_gpu_tex tex = make_tex(WIDTH, HEIGHT, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    target_view = make_view(tex);
    nvn_gpu_buf rb = make_buf(WIDTH * HEIGHT * 4, NVN_GPU_MEM_SHARED, NULL);
    if (!tex || !target_view || !rb || ((tc->indexed || tc->indirect) && !indices)) {
        CHECK(0, "%s: fixture resources", tc->name);
        return;
    }

    pass_started = pass_open = 0;
    nvn_gpu_cmd cmd = begin_cmd();
    // Clear explicitly so an incorrectly skipped draw reads deterministic black.
    rp_pass_begin(cmd);
    rp_pass_end(cmd);
    if (tc->indexed) {
        rp_S.restart.on = !tc->indirect;
        rp_S.restart.index = 0xbeef;
        const p_draw_idx di = {.prim = tc->prim, .count = tc->indirect ? 0 : tc->count, .instances = 1, .itype = 1,
                               .ibuf = (uintptr_t)guest.index, .base_vertex = tc->indirect ? 0 : (int32_t)tc->first,
                               .ind = tc->indirect ? (uintptr_t)guest.args : 0};
        rp_draw(cmd, NULL, &di, &di);
    } else {
        const p_draw dr = {.prim = tc->prim, .count = tc->indirect ? 0 : tc->count,
                           .first = tc->indirect ? 0 : tc->first, .instances = 1,
                           .ind = tc->indirect ? (uintptr_t)guest.args : 0};
        rp_draw(cmd, &dr, NULL, &dr);
    }
    rp_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, WIDTH * 4, WIDTH * HEIGHT * 4);
    if (!OK(nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &err)) ||
        !OK(nvn_gpu_fence_wait(signal, 5000000000ull, &err))) {
        CHECK(0, "%s: commit or fence %d %s", tc->name, err.rc.code, err.why);
        goto release;
    }
    uint64_t drawn, skipped;
    rp_draw_stats(&drawn, &skipped);
    nvn_gpu_caps caps = {0};
    nvn_gpu_caps_get(&caps);
    const uint8_t (*exp)[4] = (tc->has_last_expected && (caps.caps & NVN_GPU_CAP_PROVOKING_LAST))
        ? tc->expected_last : tc->expected;
    const uint8_t *pixels = nvn_gpu_buf_cpu(rb);
    unsigned bad = 0;
    for (unsigned y = 0; y < HEIGHT; y++) for (unsigned x = 0; x < WIDTH; x++)
        bad += memcmp(pixels + 4 * (y * WIDTH + x), exp[x], 4) != 0;
    printf("GS topology %s: draws=%llu skips=%llu bad_pixels=%u/%u\n",
           tc->name, (unsigned long long)drawn, (unsigned long long)skipped, bad, WIDTH * HEIGHT);
    CHECK(drawn == 1 && skipped == 0 && !bad, "%s ordered primitives", tc->name);
release:
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(target_view);
    nvn_gpu_tex_release(tex);
    if (indices) { nvn_gpu_buf_release(indices, NULL, NULL); indices = NULL; }
}

// No GS module: force the pipeline to check topology independently of shader admission.
static void check_rejected_topology(nvn_gpu_shader vs, uint32_t prim, const char *reason) {
    nvn_gpu_pipe_desc pd = {0};
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = NVN_LOGIC_OP_COPY;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = prim;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err);
    CHECK(rc.code == NVN_GPU_E_UNSUPPORTED && !pipe && strstr(err.why, reason),
          "topology %u requires explicit rejection: %d %s", prim, rc.code, err.why);
    printf("GS topology rejection %u: rc=%d reason=%s\n", prim, rc.code, err.why);
}

static void check_no_geometry(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/triangles.geom.spv", dir);
    size_t size = 0;
    void *blob = read_file(path, &size);
    CHECK(blob, "no-cap control loads the GS fixture bytes");
    if (!blob) return;
    nvn_gpu_shader_src src = {.stage = NVN_GPU_SH_GS, .kind = NVN_GPU_BLOB_SPIRV, .entry = "main",
                              .blob = blob, .bytes = size};
    nvn_gpu_shader gs = NULL;
    nvn_gpu_err err = {0};
    const nvn_gpu_rc rc = nvn_gpu_shader_load(&src, &gs, &err);
    CHECK(rc.code == NVN_GPU_E_UNSUPPORTED && !gs, "unavailable GS is rejected before creating a module");
    if (gs) nvn_gpu_shader_release(gs);
    free(blob);
}

int main(int argc, char **argv) {
    const int no_cap = argc == 3 && !strcmp(argv[2], "--no-cap");
    if (argc != 2 && argc != 3) return 2;
    const struct topology_case *selected = NULL;
    if (argc == 3 && !no_cap) {
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
            if (!strcmp(argv[2], cases[i].name)) selected = &cases[i];
        if (!selected) return 2;
    }
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "GS-topology-replay";
    if (!open_backend()) return 2;
    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    if (no_cap) {
        CHECK(!g_dev.core_features.geometryShader && !(caps.caps & NVN_GPU_CAP_GEOMETRY),
              "missing probe leaves the hardware feature disabled");
        check_no_geometry(argv[1]);
        nvn_gpu_shader vs = load_one_shader(argv[1], "gs_topology.vert", NVN_GPU_SH_VS, NULL);
        CHECK(vs, "no-GS topology control loads vertex shader");
        if (vs) {
            for (uint32_t prim = 0xA; prim <= 0xD; prim++)
                check_rejected_topology(vs, prim, "geometryShader");
            nvn_gpu_shader_release(vs);
        }
        nvn_gpu_err err = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close no-GS session");
        CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy no-GS device");
        if (!g_fail) puts("PASS no GS capability: hardware shader rejected");
        return g_fail ? 1 : 0;
    }
    if (!(caps.caps & NVN_GPU_CAP_GEOMETRY)) { fprintf(stderr, "fixture requires enabled GS\n"); return 2; }
    program.sh[0] = load_one_shader(argv[1], "gs_topology.vert", NVN_GPU_SH_VS, NULL);
    program.sh[1] = load_one_shader(argv[1], "gs_topology.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_shader triangles = load_one_shader(argv[1], "triangles.geom", NVN_GPU_SH_GS, NULL);
    nvn_gpu_shader lines = load_one_shader(argv[1], "lines.geom", NVN_GPU_SH_GS, NULL);
    nvn_gpu_shader adj_triangles = load_one_shader(argv[1], "adj-triangles.geom", NVN_GPU_SH_GS, NULL);
    nvn_gpu_shader adj_lines = load_one_shader(argv[1], "adj-lines.geom", NVN_GPU_SH_GS, NULL);
    if (!program.sh[0] || !program.sh[1] || !triangles || !lines || !adj_triangles || !adj_lines) return 2;
    check_rejected_topology(program.sh[0], 255, "255");
    program.color_out_mask = 15u;
    target_tex = (tex_t){.serial = 1, .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                                  .width = WIDTH, .height = HEIGHT, .depth = 1, .levels = 1}};
    target = (p_set_rt){.n = 1, .color = {&target_tex}};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const struct topology_case *tc = &cases[i];
        if (selected ? tc != selected : tc->prim >= 0xA) continue;
        nvn_gpu_shader gs = tc->lines ? lines : triangles;
        if (tc->prim >= 0xA) gs = tc->lines ? adj_lines : adj_triangles;
        run_case(tc, gs, i + 1);
    }

    rp_draw_reset();
    nvn_gpu_shader_release(program.sh[0]);
    nvn_gpu_shader_release(program.sh[1]);
    nvn_gpu_shader_release(adj_lines);
    nvn_gpu_shader_release(adj_triangles);
    nvn_gpu_shader_release(triangles);
    nvn_gpu_shader_release(lines);
    nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close GS topology session");
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy GS topology device");
    return g_fail ? 1 : 0;
}
