// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define nvn_plat_flag backend_fixture_flag
#define nvn_plat_log backend_fixture_log
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag
#undef nvn_plat_log
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_state.h"
#include "nvn_tex.h"

int rp_tm_on;
static nvn_gpu_cmd active;
static int enabled, watching, color_case;
static unsigned vk_color_clears;
static nvn_gpu_pass_desc observed[2];
static unsigned passes;
static int co_diag;
static uint64_t frame, segment = 1;
static unsigned co_trims, co_uploads;
static unsigned long long co_uid;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_clear_only_attachments")) return enabled ? 1 : dflt;
    if (!strcmp(name, "nvk_pass_color_rect_clear")) return enabled;
    if (!strcmp(name, "nvk_co_reupload_diag")) return co_diag;
    if (color_case && !strcmp(name, "nvk_pass_cache")) return 0;
    return backend_fixture_flag(name, dflt);
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    backend_fixture_log(level, cls, text);
    if (!cls || strcmp(cls, "co-reupload")) return;
    unsigned long long serial, uid, trim_serial, trim_frame, trim_seg, event_frame, event_seg, bytes;
    int recorded;
    char mode[16], trigger[16];
    if (sscanf(text, "event=trim serial=%llu uid=%llu frame=%llu seg=%llu",
               &serial, &uid, &event_frame, &event_seg) == 4) {
        co_trims++;
        co_uid = uid;
        CHECK(serial == 2 && uid && event_frame == 10 && event_seg == 1, "CO trim identity: %s", text);
    } else if (sscanf(text, "event=upload serial=%llu uid=%llu trim_serial=%llu trim_frame=%llu trim_seg=%llu"
                            " frame=%llu seg=%llu trigger=%15s mode=%15s recorded=%d raw_bytes=%llu",
                      &serial, &uid, &trim_serial, &trim_frame, &trim_seg, &event_frame, &event_seg,
                      trigger, mode, &recorded, &bytes) == 11) {
        co_uploads++;
        CHECK(serial == 2 && uid == co_uid && trim_serial == 2 && trim_frame == 10 && trim_seg == 1 &&
              event_frame == 11 && event_seg == 2 && !strcmp(trigger, "cpu") && !strcmp(mode, "whole") &&
              recorded == 1 && bytes == 64 * 64 * 4, "CO reupload identity and result: %s", text);
    }
}
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return frame; }
uint64_t rp_seg_serial(void) { return segment; }
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
        if (passes < sizeof observed / sizeof observed[0]) observed[passes] = *d;
        passes++;
    }
    __real_nvn_gpu_pass_begin(c, d);
}

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo *,
                                                       const VkAllocationCallbacks *, VkRenderPass *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *d,
                                                        const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    if (watching && color_case)
        for (uint32_t i = 0; i < d->attachmentCount; i++)
            vk_color_clears += d->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
    return __real_vkCreateRenderPass(dev, d, alloc, out);
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
    co_diag = shape == TRIM && !getenv("S2A_TEST_CO_DIAG_OFF");
    co_trims = co_uploads = 0;
    co_uid = 0;
    frame = 10;
    segment = 1;
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

    if (shape == TRIM && enabled) {
        CHECK(!nvn_be_tex_gpu_owned(b.serial), "trimmed attachment must keep CPU ownership");
        frame = 11;
        segment = 2;
        fill_rgba(&b, green);
        nvn_be_tex_cpu_written(&b);
    }

    nvn_gpu_tex at = rp_tex_resolve(&a, active), bt = rp_tex_resolve(&b, active);
    const unsigned co_expected = enabled && co_diag;
    CHECK(co_trims == co_expected && co_uploads == co_expected, "CO trim/reupload events=%u/%u expected=%u/%u",
          co_trims, co_uploads, co_expected, co_expected);
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
        check_rgba(rb, bw * H, shape == ZERO ? zero : shape == TRIM && enabled ? green : blue, "attachment B");
        if (rd) check_rgba(rd, W * H, green, "clear and preserved DS aspect");
    }
    printf("clear-only Vulkan shape=%d expected_trim=%d passes=%u pixels_checked=%d\n",
           shape, trimmed, passes, OK(retired));
    if (shape == TRIM) printf("CO trim/reupload events=%u/%u\n", co_trims, co_uploads);
    nvn_gpu_buf_release(ra, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    if (rd) nvn_gpu_buf_release(rd, NULL, NULL);
    if (pv) nvn_gpu_view_release(pv);
    if (probe) nvn_gpu_tex_release(probe);
    rp_pass_reset();
    rp_res_reset();
}

enum rect_case { CROPPED, OFFSET, MASK, RECT_ALIAS, CONTINUE, OPEN_PASS, FRESH };

static void run_color_rect(enum rect_case shape, int expect) {
    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t a = guest_texture(7, 0, W, NVN_FORMAT_RGBA8);
    tex_t b = guest_texture(8, 4096, W, NVN_FORMAT_RGBA8);
    static const uint8_t blue[4] = {0,0,255,255}, red[4] = {255,0,0,255}, magenta[4] = {255,0,255,255};
    fill_rgba(&a, blue);
    const p_set_rt rt = {.n = shape == RECT_ALIAS ? 2 : 1, .color = {&a, shape == RECT_ALIAS ? &b : NULL}};
    const p_set_rt next = shape == CONTINUE ? rt : (p_set_rt){0};
    const p_clear_color clear = {.index = 0, .mask = shape == MASK ? 1 : 0xf, .rgba = {1,0,0,1}};
    const op_hdr clear_hdr = {.op = OP_CLEAR_COLOR, .size = 8 + sizeof clear};
    const op_hdr next_hdr = {.op = OP_SET_RT, .size = (uint32_t)(8 + set_rt_size(&next))};
    _Alignas(8) uint8_t record[512] = {0};
    memcpy(record, &clear_hdr, 8);
    memcpy(record + 8, &clear, sizeof clear);
    memcpy(record + clear_hdr.size, &next_hdr, 8);
    set_rt_put(record + clear_hdr.size + 8, &next);
    active = begin_cmd();
    if (shape != FRESH) {
        nvn_gpu_view av = rp_view_for(&a, NULL, active);
        CHECK(av, "color view for initialization");
        nvn_gpu_pass_desc init = {.w = W, .h = H, .layers = 1, .ncolor = 1};
        init.color[0] = (nvn_gpu_att){.view = av, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
        init.color[0].clear.f[2] = init.color[0].clear.f[3] = 1;
        nvn_gpu_pass_begin(active, &init);
        nvn_gpu_pass_end(active);
        rp_tex_mark_gpu_written(&a);
    }
    rp_pass_set_rt(active, &rt);
    rp_R.has_sc = 1;
    const unsigned x = shape == OFFSET ? 16 : 0;
    rp_R.sc = (p_scissor){(int32_t)x, 0, 32, H / 2};
    if (shape == OPEN_PASS) {
        rp_pass_prepare_draw(active, 0xf);
        CHECK(rp_pass_begin(active), "prepare an open pass");
    }
    passes = vk_color_clears = 0;
    watching = 1;
    rp_pass_clear_color(active, &clear, record);
    watching = 0;
    if (shape == CROPPED)
        CHECK(vk_color_clears == (unsigned)expect && passes == 1 && observed[0].w == (expect ? 32 : W) &&
              observed[0].h == (expect ? H / 2 : H), "cropped clear expected=%d got=%u", expect, vk_color_clears);
    else CHECK(vk_color_clears == 0, "fallback shape=%d retains the prior LOAD", shape);
    rp_pass_set_rt(active, &next);
    rp_pass_flush(active);

    nvn_gpu_buf out = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_copy_tex_buf(active, rp_tex_resolve(&a, active), 0, 0, NULL, out, 0, W * 4, W * H * 4);
    static uint64_t signal;
    nvn_gpu_err err = {0};
    ++signal;
    const nvn_gpu_rc submitted = nvn_gpu_cmd_commit(active, signal, on_done, NULL, &err);
    CHECK(OK(submitted), "color rect submit: %s", err.why);
    const nvn_gpu_rc retired = OK(submitted) ? nvn_gpu_fence_wait(signal, 5000000000ull, &err) : submitted;
    CHECK(OK(retired), "color rect retire: %s", err.why);
    if (OK(retired)) {
        const uint8_t *pixels = buf_cpu(out);
        unsigned bad = 0;
        for (unsigned y = 0; y < H; y++) for (unsigned col = 0; col < W; col++) {
            const uint8_t *want = y < H / 2 && col >= x && col < x + 32
                ? (shape == MASK ? magenta : red) : blue;
            bad += memcmp(pixels + (y * W + col) * 4, want, 4) != 0;
        }
        CHECK(bad == 0, "color rect shape=%d bad pixels=%u", shape, bad);
    }
    nvn_gpu_buf_release(out, NULL, NULL);
    rp_res_reset();
    rp_pass_reset();
    rp_state_reset();
}

int main(int argc, char **argv) {
    color_case = argc == 5 && !strcmp(argv[4], "--color-rect");
    if (argc != 4 && !color_case) return 2;
    enabled = atoi(argv[2]);
    const int expect_trim = atoi(argv[3]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-clear-only"};
    if (!open_backend()) return 2;
    if (color_case) {
        for (enum rect_case s = CROPPED; s <= FRESH; s++) run_color_rect(s, expect_trim);
    } else {
        for (enum shape s = TRIM; s <= STENCIL; s++) run_clear(argv[1], s, expect_trim);
    }
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("%s Vulkan cases=%d failures=%d\n", color_case ? "color-rect" : "clear-only", color_case ? 7 : 6, g_fail);
    return g_fail ? 1 : 0;
}
