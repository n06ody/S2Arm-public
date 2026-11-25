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

enum { W = 64, H = 64, DS = NVN_FORMAT_DEPTH32F_STENCIL8 };
enum scenario { WITHIN, GROW, DEFAULT_SC, CLEAR_CROSSES, DS_CROSSES, ZERO, ALIAS, MSAA, BACKEND_GROW };
static const p_scissor small = {11, 9, 11, 10}, inside = {15, 12, 5, 4}, larger = {18, 14, 27, 20};
static const uint8_t blue[4] = {0, 0, 255, 255}, green[4] = {0, 255, 0, 255}, zero[4];
static nvn_gpu_cmd active;
static nvn_gpu_pipe green_pipe, blue_pipe, ds_pipe, msaa_pipe, alias_pipe;
static nvn_gpu_buf vb_high, vb_low;
static int enabled;
static uint8_t storage[262144];
static pool_t pool;

int rp_tm_on;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pass_area")) return enabled;
    if (!strcmp(name, "nvk_pass_edge_trace") && getenv("S2A_TEST_AREA_TRACE")) return 1;
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

static pthread_mutex_t vk_capture_mu = PTHREAD_MUTEX_INITIALIZER;
static VkRect2D areas[5];
static unsigned num_areas, num_draws, gran_calls;
static VkExtent2D granularity;
static PFN_vkCmdBeginRenderPass original_begin;
static PFN_vkCmdDraw original_draw;
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
extern __typeof__(vkGetRenderAreaGranularity) __real_vkGetRenderAreaGranularity;

static VKAPI_ATTR void VKAPI_CALL observed_begin(VkCommandBuffer cmd, const VkRenderPassBeginInfo *info,
                                                 VkSubpassContents contents) {
    pthread_mutex_lock(&vk_capture_mu);
    if (num_areas < sizeof areas / sizeof areas[0]) areas[num_areas] = info->renderArea;
    num_areas++;
    pthread_mutex_unlock(&vk_capture_mu);
    original_begin(cmd, info, contents);
}
static VKAPI_ATTR void VKAPI_CALL observed_draw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances,
                                                uint32_t first_vertex, uint32_t first_instance) {
    pthread_mutex_lock(&vk_capture_mu);
    num_draws++;
    pthread_mutex_unlock(&vk_capture_mu);
    original_draw(cmd, vertices, instances, first_vertex, first_instance);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(dev, name);
    if (fn && !strcmp(name, "vkCmdBeginRenderPass")) {
        original_begin = (PFN_vkCmdBeginRenderPass)fn;
        return (PFN_vkVoidFunction)observed_begin;
    }
    if (fn && !strcmp(name, "vkCmdDraw")) {
        original_draw = (PFN_vkCmdDraw)fn;
        return (PFN_vkVoidFunction)observed_draw;
    }
    return fn;
}
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetRenderAreaGranularity(VkDevice dev, VkRenderPass pass, VkExtent2D *out) {
    __real_vkGetRenderAreaGranularity(dev, pass, out);
    // Keep the real driver granularity as a divisor. Use a different
    // X and Y factor to expose a swapped axis and inward rounding.
    CHECK(out->width && out->width <= 8 && out->height && out->height <= 8, "lavapipe area granularity");
    out->width *= 8;
    out->height *= 4;
    granularity = *out;
    gran_calls++;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&observed_begin), PFN_vkCmdBeginRenderPass), "NDK begin");
_Static_assert(__builtin_types_compatible_p(__typeof__(&observed_draw), PFN_vkCmdDraw), "NDK draw");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetRenderAreaGranularity), PFN_vkGetRenderAreaGranularity), "NDK granularity");

static void raw_fill(const tex_t *t, const uint8_t rgba[4]) {
    for (uint64_t i = 0; i < t->L.size; i += 4) memcpy(t->mem + i, rgba, 4);
}
static tex_t guest_texture(uint64_t serial, unsigned offset, uint32_t format) {
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = W,
                    .height = H, .depth = 1, .levels = 1},
               .serial = serial, .pool = &pool, .off = offset, .mem = storage + offset};
    nvn_tex_layout(&t.d, &t.L);
    CHECK(t.L.size && offset + t.L.size <= sizeof storage, "guest range bounds");
    return t;
}
static nvn_gpu_pass_desc load_desc(nvn_gpu_view view, nvn_gpu_view ds) {
    nvn_gpu_pass_desc d = {.w = W, .h = H, .layers = 1, .ncolor = view ? 1u : 0u};
    d.color[0].view = view;
    d.depth.view = d.stencil.view = ds;
    return d;
}
static nvn_gpu_pipe create_pipe(const char *dir, const char *color, unsigned samples, uint32_t ds, int mrt) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vsm = NULL, *fsm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", color, NULL, NULL, &vs, &fs, &vsm, &fsm)) {
        CHECK(0, "area fixture shaders"); return NULL;
    }
    nvn_gpu_pipe_desc d;
    fill_gpipe_desc(&d, vs, fs, ds);
    d.samples = samples;
    if (mrt) d.cfmt[1] = NVN_FORMAT_RGBA8;
    if (ds) {
        d.sfmt = ds;
        d.ds.dt = d.ds.st = 1;
        d.ds.df = NVN_DEPTH_FUNC_EQUAL;
        d.ds.sf_f = d.ds.sf_b = NVN_STENCIL_FUNC_EQUAL;
        d.ds.fail_f = d.ds.zfail_f = d.ds.zpass_f = NVN_STENCIL_OP_KEEP;
        d.ds.fail_b = d.ds.zfail_b = d.ds.zpass_b = NVN_STENCIL_OP_KEEP;
    }
    nvn_gpu_err e = {0};
    nvn_gpu_pipe p = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&d, NVN_GPU_PIPE_SYNC, &p, &e)), "area pipe: %s", e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return p;
}
static nvn_gpu_buf make_vertices(float z) {
    const float verts[3][4] = {{-1, -1, z, 1}, {3, -1, z, 1}, {-1, 3, z, 1}};
    return make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
}
static void draw(nvn_gpu_pipe pipe, nvn_gpu_buf vbuf, const p_scissor *sc, int replay) {
    if (replay) {
        rp_R.has_sc = sc != NULL;
        if (sc) rp_R.sc = *sc;
        rp_pass_prepare_draw(active, 0xf);
        CHECK(rp_pass_begin(active), "replay pass begin");
    }
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.has_sc = sc != NULL;
    if (sc) { dyn.sc[0] = sc->x; dyn.sc[1] = sc->y; dyn.sc[2] = sc->w; dyn.sc[3] = sc->h; }
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xff;
    dyn.st_ref[0] = dyn.st_ref[1] = 17;
    nvn_gpu_set_pipe(active, pipe);
    nvn_gpu_set_dyn(active, &dyn);
    nvn_gpu_set_vbuf(active, 0, vbuf, 0);
    const nvn_gpu_draw_args a = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(active, &a);
    if (replay) rp_pass_note_draw(0);
}
static int in_rect(const p_scissor *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}
static void check_pixels(nvn_gpu_buf readback, enum scenario kind) {
    const uint8_t *pixels = buf_cpu(readback);
    CHECK(pixels != NULL, "map area fixture pixels");
    if (!pixels) return;
    unsigned bad = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t expected[4];
        const uint8_t *base = kind == ZERO ? zero : blue;
        if (in_rect(&small, x, y)) base = green;
        if (kind == WITHIN && in_rect(&inside, x, y)) base = blue;
        if ((kind == GROW || kind == BACKEND_GROW) && in_rect(&larger, x, y)) base = blue;
        if (kind == DEFAULT_SC) base = blue;
        if (kind == DS_CROSSES && in_rect(&larger, x, y)) base = green;
        memcpy(expected, base, sizeof expected);
        if (kind == CLEAR_CROSSES && in_rect(&larger, x, y)) expected[0] = 255;
        bad += memcmp(pixels + ((size_t)y * W + (size_t)x) * 4, expected, sizeof expected) != 0;
    }
    CHECK(bad == 0, "case=%d entire surface bad pixels=%u", kind, bad);
}
static void check_area(enum scenario kind, unsigned index, int local) {
    VkRect2D expected = {{0, 0}, {W, H}};
    if (local) {
        CHECK(granularity.width && granularity.height, "granularity reported");
        if (!granularity.width || !granularity.height) return;
        const unsigned x = (unsigned)small.x, y = (unsigned)small.y;
        const unsigned gx = granularity.width, gy = granularity.height;
        expected.offset.x = (int32_t)(x - x % gx); expected.offset.y = (int32_t)(y - y % gy);
        unsigned right = x + (unsigned)small.w, bottom = y + (unsigned)small.h;
        while (right < W && right % gx) right++;
        while (bottom < H && bottom % gy) bottom++;
        expected.extent.width = right - (unsigned)expected.offset.x;
        expected.extent.height = bottom - (unsigned)expected.offset.y;
    }
    const VkRect2D a = areas[index];
    CHECK(a.offset.x == expected.offset.x && a.offset.y == expected.offset.y &&
          a.extent.width == expected.extent.width && a.extent.height == expected.extent.height,
          "area case=%d pass=%u actual=%d,%d,%u,%u expected=%d,%d,%u,%u", kind, index,
          a.offset.x, a.offset.y, a.extent.width, a.extent.height,
          expected.offset.x, expected.offset.y, expected.extent.width, expected.extent.height);
}

static void run_case(enum scenario kind, uint64_t signal) {
    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t a = guest_texture(1, 0, NVN_FORMAT_RGBA8), b = guest_texture(2, 4096, NVN_FORMAT_RGBA8);
    tex_t z = guest_texture(3, 131072, DS);
    if (kind != ZERO) raw_fill(&a, blue);
    if (kind == ALIAS) { CHECK(b.mem < a.mem + a.L.size, "guest ranges overlap"); raw_fill(&b, blue); }
    active = begin_cmd();
    num_areas = num_draws = 0;
    nvn_gpu_tex out = NULL, ms = NULL;
    nvn_gpu_view out_view = NULL, ms_view = NULL;
    if (kind == MSAA) {
        nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                                   .width = W, .height = H, .depth = 1, .levels = 1, .samples = 4},
                              .usage = NVN_GPU_TEX_RENDER};
        nvn_gpu_err e = {0};
        CHECK(OK(nvn_gpu_tex_create(&td, &ms, &e)), "four samples: %s", e.why);
        ms_view = make_view(ms);
        nvn_gpu_pass_desc d = load_desc(ms_view, NULL);
        d.color[0].load = NVN_GPU_LOAD_CLEAR; d.color[0].clear.f[2] = d.color[0].clear.f[3] = 1;
        nvn_gpu_pass_begin(active, &d); nvn_gpu_pass_end(active);
        d.color[0].load = NVN_GPU_LOAD_LOAD;
        if (enabled) { d.area_x = small.x; d.area_y = small.y; d.area_w = small.w; d.area_h = small.h; }
        nvn_gpu_pass_begin(active, &d);
        draw(msaa_pipe, vb_high, &small, 0);
        nvn_gpu_pass_end(active);
        out = make_tex(W, H, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
        out_view = make_view(out);
        NVNcopyRegion r = {0, 0, 0, W, H, 1};
        nvn_gpu_blit(active, ms_view, &r, out_view, &r, 0);
    } else {
        if (kind == DS_CROSSES) {
            nvn_gpu_view view = rp_view_for(&z, NULL, active);
            nvn_gpu_pass_desc d = load_desc(NULL, view);
            d.depth.load = d.stencil.load = NVN_GPU_LOAD_CLEAR;
            d.clear_depth = 0.75f; d.clear_stencil = 17;
            nvn_gpu_pass_begin(active, &d); nvn_gpu_pass_end(active);
            rp_tex_mark_gpu_written(&z);
        }
        const p_set_rt rt = {.n = kind == ALIAS ? 2 : 1, .color = {&a, kind == ALIAS ? &b : NULL},
                             .depth = kind == DS_CROSSES ? &z : NULL};
        rp_pass_set_rt(active, &rt);
        if (kind == ALIAS) {
            rp_R.has_sc = 1; rp_R.sc = small;
            rp_pass_prepare_draw(active, 0xff);
            CHECK(rp_pass_begin(active), "aliased MRT pass");
        }
        if (kind == BACKEND_GROW) {
            rp_R.has_sc = 1; rp_R.sc = small;
            rp_pass_prepare_draw(active, 0xf);
            CHECK(rp_pass_begin(active), "backend bounds pass");
        }
        nvn_gpu_pipe first = kind == DS_CROSSES ? ds_pipe : kind == ALIAS ? alias_pipe : green_pipe;
        draw(first, vb_high, &small, kind != ALIAS && kind != BACKEND_GROW);
        if (kind == WITHIN) draw(blue_pipe, vb_high, &inside, 1);
        if (kind == GROW || kind == BACKEND_GROW) draw(blue_pipe, vb_high, &larger, kind == GROW);
        if (kind == DEFAULT_SC) draw(blue_pipe, vb_high, NULL, 1);
        if (kind == CLEAR_CROSSES) {
            rp_R.sc = larger;
            const p_clear_color clear = {.mask = 1, .rgba = {1, 0, 0, 0}};
            rp_pass_clear_color(active, &clear, NULL);
        }
        if (kind == DS_CROSSES) {
            rp_R.sc = larger;
            const p_clear_ds clear = {.depth_mask = 1, .depth = 0.25f};
            rp_pass_clear_ds(active, &clear, NULL);
            draw(ds_pipe, vb_low, &larger, 1);
        }
        rp_pass_flush(active);
        out = rp_tex_resolve(&a, active);
    }
    nvn_gpu_buf readback = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_copy_tex_buf(active, out, 0, 0, NULL, readback, 0, W * 4, W * H * 4);
    nvn_gpu_err e = {0};
    const nvn_gpu_rc committed = nvn_gpu_cmd_commit(active, signal, on_done, NULL, &e);
    CHECK(OK(committed), "area submit case=%d: %s", kind, e.why);
    const nvn_gpu_rc retired = OK(committed) ? nvn_gpu_fence_wait(signal, 5000000000ull, &e) : committed;
    CHECK(OK(retired), "area retire case=%d: %s", kind, e.why);
    if (OK(retired)) check_pixels(readback, kind);
    const unsigned initial = kind == DS_CROSSES || kind == MSAA;
    const int local = enabled && kind != ZERO && kind != ALIAS && kind != MSAA;
    const unsigned split = local && kind != WITHIN;
    const unsigned expected_draws = kind == WITHIN || kind == GROW || kind == DEFAULT_SC ||
        kind == CLEAR_CROSSES || kind == BACKEND_GROW ? 2u : kind == DS_CROSSES ? 3u : 1u;
    CHECK(num_areas == initial + 1 + split, "case=%d pass count=%u", kind, num_areas);
    CHECK(num_draws == expected_draws, "case=%d draw count=%u", kind, num_draws);
    if (initial) check_area(kind, 0, 0);
    check_area(kind, initial, local);
    if (split) check_area(kind, initial + 1, 0);
    printf("pass-area case=%d passes=%u draws=%u\n", kind, num_areas, num_draws);
    nvn_gpu_buf_release(readback, NULL, NULL);
    if (out_view) nvn_gpu_view_release(out_view);
    if (ms_view) nvn_gpu_view_release(ms_view);
    if (ms) { nvn_gpu_tex_release(ms); nvn_gpu_tex_release(out); }
    active = NULL;
    rp_pass_reset(); rp_res_reset();
}
int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) return 2;
    enabled = atoi(argv[2]);
    const int only = argc == 4 ? atoi(argv[3]) : -1;
    if (only < -1 || only > BACKEND_GROW) return 2;
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-pass-area"};
    if (!open_backend()) return 2;
    green_pipe = create_pipe(argv[1], "solid_green.frag", 1, 0, 0);
    blue_pipe = create_pipe(argv[1], "solid_blue.frag", 1, 0, 0);
    ds_pipe = create_pipe(argv[1], "solid_green.frag", 1, DS, 0);
    msaa_pipe = create_pipe(argv[1], "solid_green.frag", 4, 0, 0);
    alias_pipe = create_pipe(argv[1], "solid_green.frag", 1, 0, 1);
    if (!green_pipe || !blue_pipe || !ds_pipe || !msaa_pipe || !alias_pipe) return 2;
    vb_high = make_vertices(0.75f);
    vb_low = make_vertices(0.25f);
    for (enum scenario s = WITHIN; s <= BACKEND_GROW; s++)
        if (only < 0 || only == (int)s) run_case(s, (uint64_t)s + 1);
    if (only < 0) CHECK(enabled ? gran_calls == 2 : gran_calls == 0, "granularity cached per render-pass object, calls=%u", gran_calls);
    nvn_gpu_buf_release(vb_high, NULL, NULL);
    nvn_gpu_buf_release(vb_low, NULL, NULL);
    nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close: %s", e.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("pass-area Vulkan cases=9 failures=%d granularity_calls=%u\n", g_fail, gran_calls);
    return g_fail ? 1 : 0;
}
