// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main backend_fixture_main
#define nvn_plat_flag backend_fixture_flag
#include "t_backend_vk.c"
#undef nvn_plat_flag
#undef main
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_state.h"
#include "nvn_tex.h"

enum { W = 64, H = 32, ZW = 32, DS = NVN_FORMAT_DEPTH32F_STENCIL8 };
enum shape { IDLE, DEPTH_TEST, DEPTH_WRITE, STENCIL_TEST, RECT_CLEAR, MASKED_CLEAR, QUERY, ALIAS, SAME_RT };
static uint8_t storage[262144];
static pool_t pool;
static nvn_gpu_cmd active;
static int enabled, watching;
static nvn_gpu_pass_desc outlined;
static nvn_gpu_pipe no_ds, with_ds, probe_ds;

// This optional call keeps the test runnable on the unmodified baseline.
extern void rp_pass_plan_ds(const void *rec) __attribute__((weak));
int rp_tm_on;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pass_io")) return 1;
    if (!strcmp(name, "nvk_pass_idle_ds_drop")) return enabled;
    return backend_fixture_flag(name, dflt);
}
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 0; }
uint64_t rp_seg_serial(void) { return 1; }
uint64_t rp_sub_serial(void) { return 1; }
int rp_occl_want(void) { return 0; }
void rp_occl_pass_opened(int declared) { CHECK(!declared, "unexpected occlusion pass"); }
void rp_occl_pass_closed(void) {}
void rp_occl_pause(nvn_gpu_cmd c) { (void)c; }
int rp_buf_gpu_pending(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub) {
    *last_sub = rp_sub_serial();
    return active;
}

extern __typeof__(nvn_gpu_pass_begin) __real_nvn_gpu_pass_begin;
void __wrap_nvn_gpu_pass_begin(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d) {
    if (watching) outlined = *d;
    __real_nvn_gpu_pass_begin(c, d);
}

static tex_t guest(uint64_t serial, unsigned offset, unsigned width, uint32_t fmt) {
    tex_t t = {.serial = serial, .pool = &pool, .off = offset, .mem = storage + offset,
        .d = {.target = NVN_TEXTURE_TARGET_2D, .format = fmt, .width = width,
              .height = H, .depth = 1, .levels = 1}};
    nvn_tex_layout(&t.d, &t.L);
    CHECK(t.L.size && offset + t.L.size <= sizeof storage, "guest range");
    return t;
}

static nvn_gpu_pipe pipe_for(const char *dir, int mode) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vs_mem = NULL, *fs_mem = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vs_mem, &fs_mem)) return NULL;
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, mode ? DS : 0);
    if (mode) pd.sfmt = DS;
    if (mode == 2) {
        pd.ds.dt = pd.ds.st = 1;
        pd.ds.df = NVN_DEPTH_FUNC_EQUAL;
        pd.ds.sf_f = pd.ds.sf_b = NVN_STENCIL_FUNC_EQUAL;
        pd.ds.fail_f = pd.ds.zfail_f = pd.ds.zpass_f = NVN_STENCIL_OP_KEEP;
        pd.ds.fail_b = pd.ds.zfail_b = pd.ds.zpass_b = NVN_STENCIL_OP_KEEP;
    }
    nvn_gpu_pipe p = NULL;
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &err)), "idle DS fixture pipeline: %s", err.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vs_mem); free(fs_mem);
    return p;
}

static void draw(nvn_gpu_pipe p, float z, unsigned stencil) {
    const float vertices[3][4] = {{-1,-1,z,1}, {3,-1,z,1}, {-1,3,z,1}};
    nvn_gpu_buf vb = make_buf(sizeof vertices, NVN_GPU_MEM_SHARED, vertices);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.vp[0][2] = W;
    dyn.vp[0][3] = H;
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xff;
    dyn.st_ref[0] = dyn.st_ref[1] = (int)stencil;
    nvn_gpu_set_pipe(active, p);
    nvn_gpu_set_dyn(active, &dyn);
    nvn_gpu_set_vbuf(active, 0, vb, 0);
    const nvn_gpu_draw_args args = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(active, &args);
    nvn_gpu_buf_release(vb, NULL, NULL);
}

static void run_case(enum shape shape, int expect) {
    static const uint8_t blue[4] = {0,0,255,255}, green[4] = {0,255,0,255};
    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t color = guest(1, 0, W, NVN_FORMAT_RGBA8);
    tex_t depth = guest(2, shape == ALIAS ? 1024 : 65536, ZW, DS);
    for (uint64_t i = 0; i < color.L.size; i += 4) memcpy(color.mem + i, blue, 4);
    const p_set_rt rt = {.n = 1, .color = {&color}, .depth = &depth};
    const p_set_rt next = shape == SAME_RT ? rt : (p_set_rt){0};
    active = begin_cmd();
    nvn_gpu_view depth_view = rp_view_for(&depth, NULL, active);
    CHECK(depth_view, "DS view");
    const nvn_gpu_pass_desc init = {.w = ZW, .h = H, .layers = 1,
        .depth = {.view = depth_view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE},
        .stencil = {.view = depth_view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE},
        .clear_depth = 0.75f, .clear_stencil = 17};
    nvn_gpu_pass_begin(active, &init); nvn_gpu_pass_end(active);
    rp_tex_mark_gpu_written(&depth);
    rp_state_reset();
    rp_S.ds.dt = shape == DEPTH_TEST;
    rp_S.ds.dw = shape == DEPTH_WRITE;
    rp_S.ds.st = shape == STENCIL_TEST;
    rp_pass_set_rt(active, &rt);

    struct { op_hdr h; p_draw draw; } first = {
        .h = {.op = OP_DRAW, .size = sizeof first}, .draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3}
    };
    _Alignas(8) uint8_t stream[1024] = {0};
    memcpy(stream, &first, sizeof first);
    op_hdr end = {.op = OP_SET_RT, .size = (uint32_t)(8 + set_rt_size(&next))};
    if (shape == RECT_CLEAR || shape == MASKED_CLEAR) end = (op_hdr){.op = OP_CLEAR_DS, .size = 8 + sizeof(p_clear_ds)};
    if (shape == QUERY) end = (op_hdr){.op = OP_REPORT_COUNTER, .size = 24};
    memcpy(stream + sizeof first, &end, sizeof end);
    if (end.op == OP_SET_RT) set_rt_put(stream + sizeof first + 8, &next);
    rp_pass_prepare_draw(active, 0xf);
    if (enabled && rp_pass_plan_ds) rp_pass_plan_ds(stream);
    watching = 1;
    CHECK(rp_pass_begin(active), "idle DS pass begin");
    watching = 0;
    CHECK(outlined.w == ZW && outlined.h == H && outlined.layers == 1, "DS constrains the extent after omission");
    const int omitted = outlined.depth.view == NULL && outlined.stencil.view == NULL;
    CHECK(omitted == (expect && shape == IDLE), "idle DS attachment behavior shape=%d omitted=%d", shape, omitted);
    draw(omitted ? no_ds : with_ds, 0.75f, 17);
    rp_pass_note_draw(shape == DEPTH_TEST || shape == DEPTH_WRITE || shape == STENCIL_TEST ? RP_NOTE_DS : 0);
    if (shape == RECT_CLEAR || shape == MASKED_CLEAR) {
        rp_R.has_sc = 1;
        rp_R.sc = (p_scissor){0,0,ZW,H};
        if (shape == RECT_CLEAR) rp_R.sc.w = ZW / 2;
        p_clear_ds cl = {.depth = 0.25f, .depth_mask = shape == RECT_CLEAR,
                         .stencil = 5, .stencil_mask = shape == MASKED_CLEAR ? 0xf : 0};
        rp_pass_clear_ds(active, &cl, NULL);
    }
    rp_pass_flush(active);

    nvn_gpu_tex probe = make_tex(ZW, H, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view pv = make_view(probe);
    nvn_gpu_pass_desc verify = init;
    verify.ncolor = 1;
    verify.color[0] = (nvn_gpu_att){.view = pv, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
    verify.color[0].clear.f[2] = verify.color[0].clear.f[3] = 1;
    verify.depth.load = verify.stencil.load = NVN_GPU_LOAD_LOAD;
    nvn_gpu_pass_begin(active, &verify);
    if (shape == RECT_CLEAR) draw(probe_ds, 0.25f, 17);
    draw(probe_ds, 0.75f, shape == MASKED_CLEAR ? 21u : 17u);
    nvn_gpu_pass_end(active);

    nvn_gpu_buf c = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf z = make_buf(ZW * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_copy_tex_buf(active, rp_tex_resolve(&color, active), 0, 0, NULL, c, 0, W * 4, W * H * 4);
    nvn_gpu_copy_tex_buf(active, probe, 0, 0, NULL, z, 0, ZW * 4, ZW * H * 4);
    static uint64_t signal;
    nvn_gpu_err err = {0};
    ++signal;
    const nvn_gpu_rc committed = nvn_gpu_cmd_commit(active, signal, on_done, NULL, &err);
    CHECK(OK(committed), "idle DS fixture submit: %s", err.why);
    const nvn_gpu_rc retired = OK(committed) ? nvn_gpu_fence_wait(signal, 5000000000ull, &err) : committed;
    CHECK(OK(retired), "idle DS fixture retire: %s", err.why);
    if (OK(retired)) {
        const uint8_t *pc = buf_cpu(c), *pz = buf_cpu(z);
        unsigned bad_color = 0, bad_depth = 0;
        for (unsigned i = 0; i < W * H; i++) bad_color += memcmp(pc + i*4, i % W < ZW ? green : blue, 4) != 0;
        for (unsigned i = 0; i < ZW * H; i++) bad_depth += memcmp(pz + i*4, green, 4) != 0;
        CHECK(!bad_color && !bad_depth, "idle DS pixels shape=%d color_bad=%u ds_bad=%u", shape, bad_color, bad_depth);
    }
    nvn_gpu_buf_release(c, NULL, NULL); nvn_gpu_buf_release(z, NULL, NULL);
    nvn_gpu_view_release(pv); nvn_gpu_tex_release(probe);
    rp_pass_reset(); rp_res_reset();
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    enabled = atoi(argv[2]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-idle-ds"};
    if (!open_backend()) return 2;
    no_ds = pipe_for(argv[1], 0); with_ds = pipe_for(argv[1], 1); probe_ds = pipe_for(argv[1], 2);
    if (!no_ds || !with_ds || !probe_ds) return 2;
    for (enum shape shape = IDLE; shape <= SAME_RT; shape++) run_case(shape, atoi(argv[3]));
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "idle DS close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("idle-ds Vulkan cases=9 failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
