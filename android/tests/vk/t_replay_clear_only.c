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
static int enabled, watching, fold_mode;
static nvn_gpu_pass_desc observed[2];
static unsigned passes, transfer_clears, transfers_at_first_pass;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_clear_only_attachments")) return !fold_mode && enabled ? 1 : dflt;
    if (!strcmp(name, "nvk_clear_tex_fold")) return fold_mode && enabled;
    if (!strcmp(name, "nvk_pass_area")) return fold_mode;
    return backend_fixture_flag(name, dflt);
}
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 0; }
uint64_t rp_seg_serial(void) { return 1; }
uint64_t rp_sub_serial(void) { return 1; }
int rp_occl_want(void) { return 0; }
void rp_occl_pass_opened(int declared) { CHECK(!declared, "unexpected occlusion"); }
void rp_occl_pass_closed(void) {}
void rp_occl_pause(nvn_gpu_cmd c) { (void)c; }
int rp_buf_gpu_pending(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub) {
    *last_sub = rp_sub_serial();
    return active;
}

extern __typeof__(nvn_gpu_pass_begin) __real_nvn_gpu_pass_begin;
void __wrap_nvn_gpu_pass_begin(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d) {
    if (watching) {
        if (!passes) transfers_at_first_pass = transfer_clears;
        if (passes < sizeof observed / sizeof observed[0]) observed[passes] = *d;
        passes++;
    }
    __real_nvn_gpu_pass_begin(c, d);
}

extern __typeof__(nvn_gpu_clear_view) __real_nvn_gpu_clear_view, __wrap_nvn_gpu_clear_view;
extern __typeof__(nvn_gpu_clear_kept_view) __real_nvn_gpu_clear_kept_view, __wrap_nvn_gpu_clear_kept_view;
void __wrap_nvn_gpu_clear_view(nvn_gpu_cmd c, nvn_gpu_view v, const float *f, const uint32_t *u, int is_int) {
    if (watching) transfer_clears++;
    __real_nvn_gpu_clear_view(c, v, f, u, is_int);
}
void __wrap_nvn_gpu_clear_kept_view(nvn_gpu_cmd c, nvn_gpu_view v, const float *f, const uint32_t *u, int is_int) {
    if (watching) transfer_clears++;
    __real_nvn_gpu_clear_kept_view(c, v, f, u, is_int);
}

static uint64_t case_signal;
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
        rp_tex_mark_gpu_written(&ds);
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
    nvn_gpu_err err = {0};
    ++case_signal;
    const nvn_gpu_rc committed = nvn_gpu_cmd_commit(active, case_signal, on_done, NULL, &err);
    CHECK(OK(committed), "submit shape %d: %s", shape, err.why);
    const nvn_gpu_rc retired = OK(committed) ? nvn_gpu_fence_wait(case_signal, 5000000000ull, &err) : committed;
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

enum fold_case {
    MRT, FOLD_ALIAS, RECT, SMALL_FB, ATTACH_FMT, FOLD_LOCAL,
    SOURCE_VIEW, ATTACH_IDENTITY, ATTACH_SWIZZLE, FOLD_CASES,
};

static void run_fold(enum fold_case scenario, int expect_fold) {
    static const uint8_t blue[4] = {0, 0, 255, 255};
    static const uint8_t colors[4][4] = {
        {64, 0, 0, 255}, {0, 255, 0, 255}, {255, 255, 0, 255}, {255, 255, 255, 255},
    };

    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS};
    tex_t tex[4];
    nvn_gpu_tex image[4];
    nvn_gpu_buf readback[4];
    active = begin_cmd();
    for (unsigned i = 0; i < 4; i++) {
        const unsigned width = scenario == SMALL_FB && i == 3 ? W / 2 : W;
        tex[i] = guest_texture(i + 1, i * 65536, width, NVN_FORMAT_RGBA8);
        if (scenario == SOURCE_VIEW && i == 0) {
            tex[i].d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
            tex[i].d.depth = 2;
            nvn_tex_layout(&tex[i].d, &tex[i].L);
            CHECK(tex[i].L.size && tex[i].L.size <= 65536, "source view array storage bounds");
        }
        fill_rgba(&tex[i], blue);
        image[i] = rp_tex_resolve(&tex[i], active);
        CHECK(image[i], "preload fold texture %u", i);
        readback[i] = make_buf(width * H * 4, NVN_GPU_MEM_SHARED, NULL);
    }
    p_set_rt rt = {.n = 4, .color = {&tex[0], &tex[1], &tex[2], &tex[3]}};
    tex_t alias = tex[0];
    if (scenario == FOLD_ALIAS) {
        alias.serial = 5;
        CHECK(rp_tex_resolve(&alias, active) == image[0], "alias must use the same real GPU image");
        rt.color[0] = &alias;
    }
    if (scenario == ATTACH_FMT) {
        rt.has_cview[0] = 1;
        rt.cview[0] = (view_t){.set_format=1, .format=NVN_FORMAT_RGBA8_SRGB};
    }
    if (scenario == SOURCE_VIEW) {
        rt.has_cview[0] = 1;
        rt.cview[0] = (view_t){.set_layers = 1, .min_layer = 1, .nlayers = 1};
    }
    if (scenario == ATTACH_IDENTITY || scenario == ATTACH_SWIZZLE) {
        rt.has_cview[0] = 1;
        rt.cview[0] = (view_t){.set_swizzle = 1, .swz = {
            NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A,
        }};
        if (scenario == ATTACH_SWIZZLE) {
            rt.cview[0].swz[0] = NVN_TEXTURE_SWIZZLE_B;
            rt.cview[0].swz[2] = NVN_TEXTURE_SWIZZLE_R;
        }
    }
    nvn_gpu_buf untouched = scenario == SOURCE_VIEW ? make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL) : NULL;

    watching = 1;
    passes = transfer_clears = transfers_at_first_pass = 0;
    memset(observed, 0, sizeof observed);
    for (unsigned i = 0; i < 4; i++) {
        const unsigned width = scenario == RECT && i == 0 ? W / 2 : tex[i].d.width;
        p_clear_tex clear = {
            .tex=&tex[i], .r={0, 0, 0, width, H, 1}, .mask=15, .value_type=CLEAR_TEX_FLOAT,
        };
        if (scenario == SOURCE_VIEW && i == 0) {
            clear.has_view = 1;
            clear.view = (view_t){.set_layers = 1, .min_layer = 1, .nlayers = 1,
                .set_target = 1, .target = NVN_TEXTURE_TARGET_2D, .set_swizzle = 1, .swz = {
                    NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_A,
                }};
        }
        for (unsigned j = 0; j < 4; j++) clear.c.f[j] = (float)colors[i][j] / 255.0f;
        rp_op_clear_tex(active, &clear, NULL);
    }
    rp_pass_set_rt(active, &rt);
    const int local = scenario == FOLD_LOCAL;
    rp_R.has_sc = 1;
    rp_R.sc = local ? (p_scissor){2, 2, 4, 4} : (p_scissor){0, 0, W, H};
    rp_pass_prepare_draw(active, 0xffffu);
    const int began = rp_pass_begin(active);
    CHECK(began, "open real MRT pass");
    if (began && local) {
        // Real in-pass access inside the crop; the readback must also see the pre-pass clear outside it.
        nvn_gpu_clear rect = {.color = 0, .color_mask = 15, .x = 2, .y = 2, .w = 4, .h = 4};
        rect.value.f[2] = rect.value.f[3] = 1.0f;
        nvn_gpu_clear_rect(active, &rect);
    }
    rp_pass_flush(active);
    watching = 0;

    const unsigned total_passes = scenario == RECT ? 2 : 1;
    const nvn_gpu_pass_desc *desc = &observed[total_passes - 1];
    unsigned attached = 0;
    unsigned mask = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (!desc->color[i].view) continue;
        attached |= 1u << i;
        if (desc->color[i].load == NVN_GPU_LOAD_CLEAR) mask |= 1u << i;
    }
    CHECK(attached == 0xf, "fold MRT attachment mask case=%d attached=%#x", scenario, attached);
    const int full_fold = scenario == MRT || scenario == SOURCE_VIEW || scenario == ATTACH_IDENTITY;
    const unsigned expected_mask = !expect_fold || local ? 0 : full_fold ? 0xf : scenario == SMALL_FB ? 0x8 : 0xe;
    const unsigned expected_transfers = !expect_fold || local ? (scenario == RECT ? 3 : 4) :
        full_fold || scenario == RECT ? 0 : scenario == SMALL_FB ? 3 : 1;
    CHECK(passes == total_passes && desc->ncolor == 4 &&
          mask == expected_mask && transfer_clears == expected_transfers,
          "fold routes case=%d passes=%u load_clear=%#x transfer=%u", scenario, passes, mask, transfer_clears);
    if (local) {
        CHECK(desc->area_x == 2 && desc->area_y == 2 && desc->area_w == 4 && desc->area_h == 4,
              "local area was expanded");
        CHECK(transfers_at_first_pass == 4, "local clears must all execute before the draw pass");
    } else if (mask) {
        CHECK(!desc->area_w && !desc->area_h, "fold must cover the whole framebuffer");
    }

    for (unsigned i = 0; i < 4; i++) {
        const uint32_t row_bytes = tex[i].d.width * 4;
        const unsigned layer = scenario == SOURCE_VIEW && i == 0 ? 1 : 0;
        nvn_gpu_copy_tex_buf(active, image[i], 0, layer, NULL, readback[i], 0, row_bytes, row_bytes * H);
    }
    if (untouched) nvn_gpu_copy_tex_buf(active, image[0], 0, 0, NULL, untouched, 0, W * 4, W * H * 4);
    nvn_gpu_err err = {0};
    ++case_signal;
    const nvn_gpu_rc committed = nvn_gpu_cmd_commit(active, case_signal, on_done, NULL, &err);
    CHECK(OK(committed), "submit fold case=%d: %s", scenario, err.why);
    const nvn_gpu_rc retired = OK(committed) ? nvn_gpu_fence_wait(case_signal, 5000000000ull, &err) : committed;
    CHECK(OK(retired), "retire fold case=%d: %s", scenario, err.why);
    active = NULL;
    if (OK(retired)) for (unsigned i = 0; i < 4; i++) {
        const uint8_t *pixels = buf_cpu(readback[i]);
        const unsigned width = tex[i].d.width;
        unsigned good = 0;
        if (pixels) for (unsigned j = 0; j < width * H; j++) {
            const unsigned x = j % width, y = j / width;
            const int blue_pixel = i == 0 && ((scenario == RECT && x >= W / 2) ||
                                               (local && x >= 2 && x < 6 && y >= 2 && y < 6));
            const uint8_t *need = blue_pixel ? blue : colors[i];
            good += !memcmp(pixels + j * 4, need, 4);
        }
        CHECK(good == width * H, "fold pixels case=%d slot=%u good=%u/%u", scenario, i, good, width * H);
    }
    if (OK(retired) && untouched) check_rgba(untouched, W * H, blue, "uncleared array layer");
    printf("clear-fold Vulkan case=%d passes=%u load_clear=%#x transfer=%u area=%ux%u before_first=%u\n",
           scenario, passes, mask, transfer_clears, desc->area_w, desc->area_h, transfers_at_first_pass);
    for (unsigned i = 0; i < 4; i++) nvn_gpu_buf_release(readback[i], NULL, NULL);
    if (untouched) nvn_gpu_buf_release(untouched, NULL, NULL);
    rp_pass_reset();
    rp_res_reset();
    rp_R.has_sc = 0;
}

int main(int argc, char **argv) {
    if (argc != 4 && (argc != 5 || strcmp(argv[4], "fold"))) return 2;
    fold_mode = argc == 5;
    enabled = atoi(argv[2]);
    const int expected = atoi(argv[3]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-clear-only"};
    if (!open_backend()) return 2;
    if (fold_mode) for (enum fold_case s = MRT; s < FOLD_CASES; s++) run_fold(s, expected);
    else for (enum shape s = TRIM; s <= STENCIL; s++) run_clear(argv[1], s, expected);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    if (fold_mode) {
        if (!open_backend()) return 2;
        t21b_clear_load_bits();
        CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close typed clears: %s", err.why);
    }
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    if (fold_mode) printf("clear-fold Vulkan cases=%d failures=%d\n", FOLD_CASES, g_fail);
    else printf("clear-only Vulkan cases=6 failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
