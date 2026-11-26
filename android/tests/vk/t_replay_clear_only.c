// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define nvn_plat_flag backend_fixture_flag
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_state.h"
#include "nvn_tex.h"

int rp_tm_on;
static nvn_gpu_cmd active;
static int enabled, watching;
static nvn_gpu_pass_desc observed[2];
static unsigned passes;
static uint64_t submit_serial = 1;
static _Thread_local size_t fail_realloc_size, fail_calloc_size;
static unsigned rejected_upload_allocs, rejected_zero_allocs;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_clear_only_attachments")) return enabled ? 1 : dflt;
    return backend_fixture_flag(name, dflt);
}
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 0; }
uint64_t rp_seg_serial(void) { return submit_serial; }
uint64_t rp_sub_serial(void) { return submit_serial; }
int rp_occl_want(void) { return 0; }
void rp_occl_pass_opened(int declared) { CHECK(!declared, "unexpected occlusion"); }
void rp_occl_pass_closed(void) {}
void rp_occl_pause(nvn_gpu_cmd c) { (void)c; }
int rp_buf_gpu_pending(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub) {
    *last_sub = rp_sub_serial();
    return active;
}

extern __typeof__(realloc) __real_realloc;
void *__wrap_realloc(void *ptr, size_t size) {
    if (!ptr && fail_realloc_size && size == fail_realloc_size) {
        fail_realloc_size = 0;
        rejected_upload_allocs++;
        errno = ENOMEM;
        return NULL;
    }
    return __real_realloc(ptr, size);
}

extern __typeof__(calloc) __real_calloc;
void *__wrap_calloc(size_t count, size_t size) {
    if (count == 1 && fail_calloc_size && size == fail_calloc_size) {
        fail_calloc_size = 0;
        rejected_zero_allocs++;
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(count, size);
}

extern __typeof__(nvn_gpu_pass_begin) __real_nvn_gpu_pass_begin;
void __wrap_nvn_gpu_pass_begin(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d) {
    if (watching) {
        if (passes < sizeof observed / sizeof observed[0]) observed[passes] = *d;
        passes++;
    }
    __real_nvn_gpu_pass_begin(c, d);
}

enum { W = 64, H = 64, DS = NVN_FORMAT_DEPTH32F_STENCIL8 };
enum shape { TRIM, MIXED, ALIAS, ZERO, DEPTH, STENCIL };
static uint8_t storage[262144];
static pool_t pool;

static tex_t guest_texture(uint64_t serial, unsigned offset, unsigned w, uint32_t format) {
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = format,
                    .width = w, .height = H, .depth = 1, .levels = 1},
               .serial = serial, .pool = &pool, .off = offset, .mem = storage + offset};
    nvn_tex_layout(&t.d, &t.L);
    CHECK(t.L.size && offset + t.L.size <= sizeof storage, "guest storage bounds");
    return t;
}

static void fill_rgba(const tex_t *t, const uint8_t color[4]) {
    for (uint64_t i = 0; i < t->L.size; i += 4) memcpy(t->mem + i, color, 4);
}

static void check_rgba(nvn_gpu_buf buf, unsigned pixels, const uint8_t color[4], const char *label) {
    const uint8_t *data = buf_cpu(buf);
    unsigned good = 0;
    if (data) for (unsigned i = 0; i < pixels; i++) good += !memcmp(data + i * 4, color, 4);
    CHECK(good == pixels, "%s pixels=%u/%u", label, good, pixels);
}

static nvn_gpu_buf check_ds_pixels(nvn_gpu_view ds, const char *dir, enum shape shape,
                                  nvn_gpu_tex render, nvn_gpu_view view) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vsm = NULL, *fsm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) {
        CHECK(0, "depth/stencil validation shaders");
        return NULL;
    }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, DS);
    pd.sfmt = DS;
    pd.ds.dt = pd.ds.st = 1;
    pd.ds.df = NVN_DEPTH_FUNC_EQUAL;
    pd.ds.sf_f = pd.ds.sf_b = NVN_STENCIL_FUNC_EQUAL;
    pd.ds.fail_f = pd.ds.zfail_f = pd.ds.zpass_f = 1; // KEEP
    pd.ds.fail_b = pd.ds.zfail_b = pd.ds.zpass_b = 1;
    nvn_gpu_err err = {0};
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err)), "DS validation pipe: %s", err.why);
    const float z = shape == DEPTH ? 0.25f : 0.75f;
    const float verts[3][4] = {{-1, -1, z, 1}, {3, -1, z, 1}, {-1, 3, z, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_buf rb = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_pass_desc pass = {.w = W, .h = H, .layers = 1, .ncolor = 1};
    pass.color[0].view = view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1;
    pass.depth.view = pass.stencil.view = ds;
    pass.depth.load = pass.stencil.load = NVN_GPU_LOAD_LOAD;
    pass.depth.store = pass.stencil.store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(active, &pass);
    nvn_gpu_set_pipe(active, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xff;
    dyn.st_ref[0] = dyn.st_ref[1] = shape == DEPTH ? 17 : 7;
    nvn_gpu_set_dyn(active, &dyn);
    nvn_gpu_set_vbuf(active, 0, vb, 0);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(active, &draw);
    nvn_gpu_pass_end(active);
    nvn_gpu_copy_tex_buf(active, render, 0, 0, NULL, rb, 0, W * 4, W * H * 4);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    free(vsm);
    free(fsm);
    return rb;
}

static void run_clear(const char *dir, enum shape shape, int expect_trim) {
    const uint8_t blue[4] = {0, 0, 255, 255}, red[4] = {255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255}, zero[4] = {0};
    const int partial_ds = shape == DEPTH || shape == STENCIL;
    const unsigned bw = shape == MIXED ? 32 : W;
    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t a = guest_texture(1, 0, W, NVN_FORMAT_RGBA8);
    tex_t b = guest_texture(2, shape == ALIAS ? 4096 : 65536, bw, NVN_FORMAT_RGBA8);
    tex_t ds = guest_texture(3, 131072, W, DS);
    if (partial_ds) fill_rgba(&a, blue);
    if (shape != ZERO) fill_rgba(&b, blue);
    if (shape == ALIAS) CHECK((uintptr_t)b.mem - (uintptr_t)a.mem < a.L.size, "guest ranges must overlap");

    active = begin_cmd();
    nvn_gpu_view dv = NULL;
    if (partial_ds) {
        dv = rp_view_for(&ds, NULL, active);
        CHECK(dv, "depth/stencil view");
        nvn_gpu_pass_desc init = {.w = W, .h = H, .layers = 1, .clear_depth = 0.75f, .clear_stencil = 17};
        init.depth.view = init.stencil.view = dv;
        init.depth.load = init.stencil.load = NVN_GPU_LOAD_CLEAR;
        init.depth.store = init.stencil.store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(active, &init);
        nvn_gpu_pass_end(active);
        rp_tex_mark_gpu_written(active, &ds);
    }
    const p_set_rt rt = {.n = 2, .color = {&a, &b}, .depth = partial_ds ? &ds : NULL};
    rp_pass_set_rt(active, &rt);
    if (partial_ds) {
        const p_clear_ds clear = {.depth = 0.25f, .depth_mask = shape == DEPTH,
                                  .stencil = 7, .stencil_mask = shape == STENCIL ? 0xff : 0};
        rp_pass_clear_ds(active, &clear, NULL);
    } else {
        const p_clear_color clear = {.index = 0, .mask = 0xf, .rgba = {1, 0, 0, 1}};
        rp_pass_clear_color(active, &clear, NULL);
    }
    watching = 1;
    passes = 0;
    rp_pass_flush(active);
    watching = 0;
    const int trimmed = expect_trim && (shape == TRIM || partial_ds);
    CHECK(passes == (shape == MIXED ? 2u : 1u), "shape %d passes=%u", shape, passes);
    CHECK((observed[shape == MIXED].color[1].view == NULL) == trimmed,
          "clear-only attachment behavior shape=%d expected_trim=%d", shape, trimmed);

    nvn_gpu_tex at = rp_tex_resolve(&a, active), bt = rp_tex_resolve(&b, active);
    nvn_gpu_buf ra = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf rb = make_buf(bw * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_copy_tex_buf(active, at, 0, 0, NULL, ra, 0, W * 4, W * H * 4);
    nvn_gpu_copy_tex_buf(active, bt, 0, 0, NULL, rb, 0, bw * 4, bw * H * 4);
    nvn_gpu_tex probe = NULL;
    nvn_gpu_view pv = NULL;
    nvn_gpu_buf rd = NULL;
    if (partial_ds) {
        probe = make_tex(W, H, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
        pv = make_view(probe);
        rd = check_ds_pixels(dv, dir, shape, probe, pv);
    }
    static uint64_t signal;
    nvn_gpu_err err = {0};
    ++signal;
    const nvn_gpu_rc committed = nvn_gpu_cmd_commit(active, signal, on_done, NULL, &err);
    CHECK(OK(committed), "submit shape %d: %s", shape, err.why);
    const nvn_gpu_rc retired = OK(committed) ? nvn_gpu_fence_wait(signal, 5000000000ull, &err) : committed;
    CHECK(OK(retired), "retire shape %d: %s", shape, err.why);
    active = NULL;
    if (OK(retired)) {
        check_rgba(ra, W * H, partial_ds ? blue : red, "attachment A");
        check_rgba(rb, bw * H, shape == ZERO ? zero : blue, "attachment B");
        if (rd) check_rgba(rd, W * H, green, "clear and preserved DS aspect");
    }
    printf("clear-only Vulkan shape=%d expected_trim=%d passes=%u pixels_checked=%d\n",
           shape, trimmed, passes, OK(retired));
    nvn_gpu_buf_release(ra, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    if (rd) nvn_gpu_buf_release(rd, NULL, NULL);
    if (pv) nvn_gpu_view_release(pv);
    if (probe) nvn_gpu_tex_release(probe);
    rp_pass_reset();
    rp_res_reset();
}

static void check_init_retry(void) {
    const uint8_t color[4] = {13, 49, 97, 255}, zero[4] = {0}, red[4] = {255, 0, 0, 255};
    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t upload = guest_texture(10, 0, W, NVN_FORMAT_RGBA8);
    tex_t zeroed = guest_texture(11, 65536, W, NVN_FORMAT_RGBA8);
    tex_t overwrite = guest_texture(12, 131072, W, NVN_FORMAT_RGBA8);
    upload.d.flags = NVN_TEXTURE_FLAGS_LINEAR;  // Linear guest data necessarily uses CPU staging.
    overwrite.d.flags = NVN_TEXTURE_FLAGS_LINEAR;
    nvn_tex_layout(&upload.d, &upload.L);
    nvn_tex_layout(&overwrite.d, &overwrite.L);
    fill_rgba(&upload, color);
    fill_rgba(&overwrite, color);

    active = begin_cmd();
    const size_t bytes = W * H * 4;
    fail_realloc_size = bytes;
    nvn_gpu_tex rejected_source = rp_tex_resolve(&upload, active);
    CHECK(rejected_upload_allocs == 1, "CPU texture staging allocation fault was injected");
    CHECK(!rejected_source, "failed first upload exposed the texture to a reader");
    fail_realloc_size = 0;
    nvn_gpu_tex source = rp_tex_resolve(&upload, active);
    CHECK(source, "first upload is retryable");

    fail_calloc_size = bytes;
    nvn_gpu_tex rejected_zero = rp_tex_resolve(&zeroed, active);
    CHECK(rejected_zero_allocs == 1, "zero staging allocation fault was injected");
    CHECK(!rejected_zero, "failed zero initialization exposed the texture to a reader");
    fail_calloc_size = 0;
    nvn_gpu_tex cleared = rp_tex_resolve(&zeroed, active);
    CHECK(cleared, "zero initialization is retryable");

    fail_realloc_size = bytes;
    CHECK(!rp_tex_resolve(&overwrite, active), "the future clear target has a failed initial upload");
    const p_set_rt rt = {.n = 1, .color = {&overwrite}};
    const p_clear_color clear = {.index = 0, .mask = 0xf, .rgba = {1, 0, 0, 1}};
    fail_realloc_size = bytes;
    rp_pass_set_rt(active, &rt);
    rp_pass_clear_color(active, &clear, NULL);
    rp_pass_flush(active);
    CHECK(fail_realloc_size == bytes, "full clear renewed no source upload");
    fail_realloc_size = 0;
    nvn_gpu_tex rewritten = rp_tex_resolve(&overwrite, active);
    CHECK(rewritten && nvn_be_tex_gpu_owned(overwrite.serial), "full clear defined the rejected texture");

    const uint8_t poison[4] = {0xdd, 0xdd, 0xdd, 0xdd};
    uint8_t fill[W * H * 4];
    memset(fill, poison[0], sizeof fill);
    nvn_gpu_buf rsource = make_buf(bytes, NVN_GPU_MEM_SHARED, fill);
    nvn_gpu_buf rzero = make_buf(bytes, NVN_GPU_MEM_SHARED, fill);
    nvn_gpu_buf rclear = make_buf(bytes, NVN_GPU_MEM_SHARED, fill);
    if (source) nvn_gpu_copy_tex_buf(active, source, 0, 0, NULL, rsource, 0, W * 4, bytes);
    if (cleared && !rejected_zero) nvn_gpu_copy_tex_buf(active, cleared, 0, 0, NULL, rzero, 0, W * 4, bytes);
    if (rewritten) nvn_gpu_copy_tex_buf(active, rewritten, 0, 0, NULL, rclear, 0, W * 4, bytes);
    nvn_gpu_err err = {0};
    const uint64_t signal = 1024;
    const nvn_gpu_rc submitted = nvn_gpu_cmd_commit(active, signal, on_done, NULL, &err);
    CHECK(OK(submitted), "retry submit: %s", err.why);
    if (OK(submitted) && OK(nvn_gpu_fence_wait(signal, 5000000000ull, &err))) {
        check_rgba(rsource, W * H, color, "retried CPU upload");
        if (!rejected_zero) check_rgba(rzero, W * H, zero, "retried zero initialization");
        check_rgba(rclear, W * H, red, "full overwrite after failed initialization");
    } else CHECK(0, "retry segment must complete: %s", err.why);
    printf("resource init retry: upload_exposed=%d zero_exposed=%d upload_recovered=%d zero_recovered=%d\n",
           rejected_source != NULL, rejected_zero != NULL, source != NULL, cleared != NULL);
    nvn_gpu_buf_release(rsource, NULL, NULL);
    nvn_gpu_buf_release(rzero, NULL, NULL);
    nvn_gpu_buf_release(rclear, NULL, NULL);
    active = NULL;
    rp_pass_reset();
    rp_res_reset();
}

static void check_failed_segment_publication(void) {
    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t upload = guest_texture(20, 0, W, NVN_FORMAT_RGBA8);
    tex_t clear = guest_texture(21, 65536, W, NVN_FORMAT_RGBA8);
    tex_t preserved = guest_texture(22, 131072, W, NVN_FORMAT_RGBA8);
    const uint8_t color[4] = {27, 63, 101, 255};
    fill_rgba(&upload, color);

    submit_serial++;
    active = begin_cmd();
    const p_set_rt prior = {.n = 1, .color = {&preserved}};
    const p_clear_color prior_clear = {.index = 0, .mask = 0xf, .rgba = {0, 1, 0, 1}};
    rp_pass_set_rt(active, &prior);
    rp_pass_clear_color(active, &prior_clear, NULL);
    rp_pass_flush(active);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_cmd_commit(active, 1025, on_done, NULL, &err)), "preceding submit: %s", err.why);
    CHECK(OK(nvn_gpu_fence_wait(1025, 5000000000ull, &err)), "preceding clear completes: %s", err.why);

    submit_serial++;
    active = begin_cmd();
    CHECK(rp_tex_resolve(&upload, active), "same recording can consume its preceding texture upload");
    const p_set_rt rt = {.n = 2, .color = {&clear, &preserved}};
    const p_clear_color cmd = {.index = 0, .mask = 0xf, .rgba = {1, 0, 0, 1}};
    rp_pass_set_rt(active, &rt);
    rp_pass_clear_color(active, &cmd, NULL);
    const p_clear_color later = {.index = 1, .mask = 0xf, .rgba = {0, 0, 1, 1}};
    rp_pass_clear_color(active, &later, NULL);
    rp_pass_flush(active);
    CHECK((rp_tex_debug_state(upload.serial) & 2) != 0, "ordinary upload was recorded before segment failure");
    CHECK(nvn_be_tex_gpu_owned(clear.serial), "attachment clear was recorded before segment failure");

    const uint64_t signal = 1026;
    const int done_before = atomic_load(&g_ndone);
    nvn_gpu_vk_test_set_fault(1);
    const nvn_gpu_rc result = nvn_gpu_cmd_commit(active, signal, on_done, NULL, &err);
    CHECK(result.code == NVN_GPU_E_DEVICE, "Vulkan segment submit failure was injected: %d %s", result.code, err.why);
    const struct done_rec *done = wait_signal(signal, done_before, 5000);
    CHECK(done && done->status == NVN_GPU_DONE_FAILED, "failed segment retirement must report FAILED");
    submit_serial++;
    const int uploaded = (rp_tex_debug_state(upload.serial) & 2) != 0;
    const int owned = nvn_be_tex_gpu_owned(clear.serial);
    CHECK(!uploaded, "next segment still sees an ordinary upload from a failed segment");
    CHECK(!owned, "next segment still sees GPU ownership from a failed attachment clear");
    CHECK(nvn_be_tex_gpu_owned(preserved.serial), "earlier completed clear lost its GPU ownership");
    printf("failed resource segment: next_upload=%d next_clear_owner=%d preserved_clear_owner=%d\n",
           uploaded, owned, nvn_be_tex_gpu_owned(preserved.serial));
    active = NULL;
    rp_pass_reset();
    rp_res_reset();
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    enabled = atoi(argv[2]);
    const int expect_trim = atoi(argv[3]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-clear-only"};
    if (!open_backend()) return 2;
    for (enum shape s = TRIM; s <= STENCIL; s++) run_clear(argv[1], s, expect_trim);
    check_init_retry();
    check_failed_segment_publication();
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("clear-only Vulkan cases=6 failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
