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
enum shape { IDLE, DEPTH_TEST, DEPTH_WRITE, STENCIL_TEST, RECT_CLEAR, MASKED_CLEAR, QUERY, ALIAS, SAME_RT, FULL_CLEAR, COPY, LIST_END, SAMPLE_BIND, ALIAS_CLEAR, PARTIAL_STENCIL, ASPECT_DEPTH, ASPECT_STENCIL, COLOR_CLEAR,
    HINT_COLOR_CLEAR, HINT_DS_CLEAR, HINT_ONLY, DEPTH_MIXED, DEPTH_PARTIAL,
    COLOR_PARTIAL, COLOR_DISPLAY, DEPTH_REOPEN, COLOR_CROSS_SUBMIT, COLOR_COPY_OBSERVER, COLOR_ALIAS, COLOR_MRT, IDLE_AREA, DEPTH_TEST_AREA };
static uint8_t storage[262144];
static pool_t pool;
static nvn_gpu_cmd active;
static int enabled, watching, area_case;
static nvn_gpu_pass_desc outlined;
static int vk_ds, vk_discard, vk_depth_only, vk_stencil_only, vk_color;
static int vk_clear_color, vk_clear_depth, vk_clear_stencil, vk_clear_kept_stencil, vk_clear_kept_depth;
static unsigned verified_pixels;
static nvn_gpu_pipe no_ds, with_ds, probe_ds, mrt_ds;

// This optional call keeps the test runnable on the unmodified baseline.
extern void rp_pass_plan_ds(const void *rec) __attribute__((weak));
extern __typeof__(rp_pass_plan_discard) rp_pass_plan_discard __attribute__((weak));
extern void rp_pass_discard_hint(int slot, const void *rec) __attribute__((weak));
int rp_tm_on;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pass_cache")) return 0;
    if (!strcmp(name, "nvk_pass_area")) return area_case;
    if (!strcmp(name, "nvk_pass_dont_care")) return !!(enabled & 8);
    if (!strcmp(name, "nvk_load_store_op_none")) return !!(enabled & 16);
    if (!strcmp(name, "nvk_clear_park") && (enabled & 4)) return 0;
    if (!strcmp(name, "nvk_pass_idle_ds_drop")) return !!(enabled & 1);
    if (!strcmp(name, "nvk_pass_ds_store_dontcare")) return !!(enabled & 2);
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
    if (watching == 1) outlined = *d;
    __real_nvn_gpu_pass_begin(c, d);
}

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo *,
                                                       const VkAllocationCallbacks *, VkRenderPass *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    if (watching) {
        for (uint32_t i = 0; i < ci->attachmentCount; i++) {
            const VkAttachmentDescription *a = &ci->pAttachments[i];
            if (watching == 2) {
                if (a->format == VK_FORMAT_R8G8B8A8_UNORM)
                    vk_clear_color += a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                                      a->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
                if (a->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
                    vk_clear_depth += a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                                      a->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
                    vk_clear_stencil += a->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                                        a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
                    vk_clear_kept_stencil += a->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                                             a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
                    vk_clear_kept_depth += a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                                           a->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
                }
                continue;
            }
            if (a->format == VK_FORMAT_R8G8B8A8_UNORM)
                vk_color += a->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE;
            if (a->format != VK_FORMAT_D32_SFLOAT_S8_UINT) continue;
            vk_ds++;
            vk_depth_only += a->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE &&
                             a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
            vk_stencil_only += a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_DONT_CARE &&
                               a->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
            vk_discard += a->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE &&
                          a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
    }
    return __real_vkCreateRenderPass(device, ci, alloc, out);
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
    if (mode == 3) {
        pd.cfmt[1] = NVN_FORMAT_RGBA8;
        pd.lmap[1] = 1;
    }
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

static void draw_span(nvn_gpu_pipe p, float z, unsigned stencil, int x, unsigned width) {
    const float vertices[3][4] = {{-1,-1,z,1}, {3,-1,z,1}, {-1,3,z,1}};
    nvn_gpu_buf vb = make_buf(sizeof vertices, NVN_GPU_MEM_SHARED, vertices);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.vp[0][2] = W;
    dyn.vp[0][3] = H;
    dyn.has_sc = 1;
    dyn.sc[0] = x; dyn.sc[1] = 0; dyn.sc[2] = (int)width; dyn.sc[3] = H;
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xff;
    dyn.st_ref[0] = dyn.st_ref[1] = (int)stencil;
    nvn_gpu_set_pipe(active, p);
    nvn_gpu_set_dyn(active, &dyn);
    nvn_gpu_set_vbuf(active, 0, vb, 0);
    const nvn_gpu_draw_args args = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(active, &args);
    nvn_gpu_buf_release(vb, NULL, NULL);
}

static void draw(nvn_gpu_pipe p, float z, unsigned stencil) {
    draw_span(p, z, stencil, 0, W);
}

static void submit_and_wait(void) {
    static uint64_t signal;
    nvn_gpu_err err = {0};
    ++signal;
    const nvn_gpu_rc committed = nvn_gpu_cmd_commit(active, signal, on_done, NULL, &err);
    CHECK(OK(committed), "TBDR fixture submit: %s", err.why);
    const nvn_gpu_rc retired = OK(committed) ? nvn_gpu_fence_wait(signal, 5000000000ull, &err) : committed;
    CHECK(OK(retired), "TBDR fixture retire: %s", err.why);
}

static void run_case(enum shape shape, int expect) {
    static const uint8_t blue[4] = {0,0,255,255}, green[4] = {0,255,0,255}, red[4] = {255,0,0,255};
    area_case = shape == IDLE_AREA || shape == DEPTH_TEST_AREA;
    memset(storage, 0, sizeof storage);
    pool = (pool_t){.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t color = guest(1, 0, W, NVN_FORMAT_RGBA8);
    const unsigned zw = shape >= ASPECT_DEPTH && shape != DEPTH_MIXED ? W : ZW;
    tex_t depth = guest(2, (shape == ALIAS || shape == ALIAS_CLEAR || shape == COLOR_ALIAS) ? 1024 : 65536, zw, DS);
    if (shape == COLOR_DISPLAY) color.d.flags |= NVN_TEXTURE_FLAGS_DISPLAY;
    for (uint64_t i = 0; i < color.L.size; i += 4) memcpy(color.mem + i, blue, 4);
    tex_t other = {0};
    p_set_rt rt = {.n = 1, .color = {&color}, .depth = &depth};
    if (shape == COLOR_MRT) {
        other = guest(3, 131072, W, NVN_FORMAT_RGBA8);
        for (uint64_t i = 0; i < other.L.size; i += 4) memcpy(other.mem + i, blue, 4);
        rt.n = 2;
        rt.color[1] = &other;
    }
    const p_set_rt next = shape == SAME_RT ? rt : (p_set_rt){0};
    active = begin_cmd();
    nvn_gpu_view depth_view = rp_view_for(&depth, NULL, active);
    CHECK(depth_view, "DS view");
    const nvn_gpu_pass_desc init = {.w = zw, .h = H, .layers = 1,
        .depth = {.view = depth_view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE},
        .stencil = {.view = depth_view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE},
        .clear_depth = 0.75f, .clear_stencil = 17};
    nvn_gpu_pass_begin(active, &init); nvn_gpu_pass_end(active);
    rp_tex_mark_gpu_written(&depth);
    rp_state_reset();
    rp_S.ds.dt = shape == DEPTH_TEST || shape == FULL_CLEAR || shape == DEPTH_TEST_AREA;
    rp_S.ds.dw = shape == DEPTH_WRITE;
    rp_S.ds.st = shape == STENCIL_TEST || shape == FULL_CLEAR;
    rp_pass_set_rt(active, &rt);
    if (area_case) {
        rp_R.has_sc = 1;
        rp_R.sc = (p_scissor){0, 0, W / 2, H};
    }

    const struct { op_hdr h; p_draw draw; } first = {
        .h = {.op = OP_DRAW, .size = sizeof first}, .draw = {.prim = 4, .count = 3}
    };
    _Alignas(8) uint8_t stream[1024] = {0};
    uint8_t *after = stream;
    memcpy(after, &first, sizeof first); after += sizeof first;
    if (shape == IDLE) { memcpy(after, &first, sizeof first); after += sizeof first; }
    op_hdr end = {.op = OP_SET_RT, .size = (uint32_t)(8 + set_rt_size(&next))};
    const int clear = shape == RECT_CLEAR || shape == MASKED_CLEAR || shape == FULL_CLEAR ||
        shape == ALIAS_CLEAR || shape == PARTIAL_STENCIL || shape == ASPECT_DEPTH ||
        shape == ASPECT_STENCIL || shape == HINT_DS_CLEAR || shape == DEPTH_MIXED ||
        shape == DEPTH_PARTIAL || shape == DEPTH_REOPEN;
    const int color_clear = shape == COLOR_CLEAR || shape == HINT_COLOR_CLEAR || shape == COLOR_PARTIAL ||
        shape == COLOR_DISPLAY || shape == COLOR_COPY_OBSERVER || shape == COLOR_ALIAS || shape == COLOR_MRT;
    const int color_hint = shape == HINT_COLOR_CLEAR || shape == HINT_ONLY;
    if (color_hint || shape == HINT_DS_CLEAR) {
        const op_hdr hint = color_hint ? (op_hdr){OP_DISCARD_COLOR, 16} : (op_hdr){OP_DISCARD_DS, 8};
        const p_discard_color dc = {0};
        memcpy(after, &hint, sizeof hint);
        if (color_hint) memcpy(after + sizeof hint, &dc, sizeof dc);
        after += hint.size;
    }
    if (shape == COLOR_COPY_OBSERVER) {
        const op_hdr reader = {OP_COPY_T2B, 8};
        memcpy(after, &reader, sizeof reader);
        after += reader.size;
    }
    if (shape == DEPTH_REOPEN) {
        memcpy(after, &first, sizeof first);
        after += sizeof first;
    }
    p_clear_color cc = {.index = 0, .mask = shape == COLOR_PARTIAL ? 1 : 15, .rgba = {1,0,0,1}};
    p_clear_ds cl = {.depth = 0.25f, .depth_mask = shape != MASKED_CLEAR && shape != ASPECT_STENCIL, .stencil = 5,
        .stencil_mask = (shape == MASKED_CLEAR || shape == PARTIAL_STENCIL || shape == DEPTH_PARTIAL) ? 0xf :
                       (shape == FULL_CLEAR || shape == ALIAS_CLEAR || shape == ASPECT_STENCIL) ? 0xff : 0};
    if (clear || color_clear) {
        end = clear ? (op_hdr){OP_CLEAR_DS, 8 + sizeof(p_clear_ds)} : (op_hdr){OP_CLEAR_COLOR, 8 + sizeof cc};
        rp_R.has_sc = 1;
        rp_R.sc = (p_scissor){0, 0, shape == RECT_CLEAR ? zw / 2 : zw, H};
    }
    if (shape == QUERY) end = (op_hdr){.op = OP_REPORT_COUNTER, .size = 24};
    if (shape == COPY) end = (op_hdr){.op = OP_COPY_T2B, .size = 8};
    if (shape == SAMPLE_BIND) end = (op_hdr){.op = OP_BIND_TEX, .size = 24};
    if (shape == LIST_END || shape == HINT_ONLY || shape == COLOR_CROSS_SUBMIT) end = (op_hdr){.op = OP_END, .size = 8};
    memcpy(after, &end, sizeof end);
    if (end.op == OP_SET_RT) set_rt_put(after + 8, &next);
    if (clear) memcpy(after + 8, &cl, sizeof cl);
    if (color_clear) memcpy(after + 8, &cc, sizeof cc);
    rp_pass_prepare_draw(active, shape == COLOR_MRT ? 0xff : 0xf);
    if (rp_pass_plan_ds) rp_pass_plan_ds(stream);
    if (rp_pass_plan_discard) rp_pass_plan_discard(stream, NULL, NULL);
    vk_ds = vk_discard = vk_depth_only = vk_stencil_only = vk_color = 0;
    watching = 1;
    CHECK(rp_pass_begin(active), "TBDR pass begin");
    const int omitted = outlined.depth.view == NULL && outlined.stencil.view == NULL;
    nvn_gpu_pipe pipe = omitted ? no_ds : (shape == FULL_CLEAR || shape == DEPTH_TEST_AREA) ? probe_ds : shape == COLOR_MRT ? mrt_ds : with_ds;
    if (shape == IDLE) draw_span(pipe, 0.75f, 17, 0, ZW / 2);
    else if (area_case) draw_span(pipe, 0.75f, 17, 0, W / 2);
    else draw(pipe, 0.75f, 17);
    watching = 0;
    CHECK(outlined.w == zw && outlined.h == H && outlined.layers == 1, "DS constrains the extent");
    if (area_case)
        CHECK(outlined.area_x == 0 && outlined.area_y == 0 && outlined.area_w == W / 2 && outlined.area_h == H,
              "idle DS render area shape=%d was %d,%d,%u,%u", shape, outlined.area_x, outlined.area_y, outlined.area_w, outlined.area_h);
    CHECK(omitted == ((expect & 1) && (shape == IDLE || shape == IDLE_AREA)) && vk_ds == !omitted,
          "idle DS attachment behavior shape=%d omitted=%d vk_ds=%d", shape, omitted, vk_ds);
    CHECK(vk_discard == ((expect & 2) && shape == FULL_CLEAR),
          "DS STORE behavior shape=%d discard=%d", shape, vk_discard);
    const int want_depth = (expect & 8) && (shape == ASPECT_DEPTH || shape == HINT_DS_CLEAR);
    const int want_stencil = (expect & 8) && shape == ASPECT_STENCIL;
    const int want_color = (expect & 8) && (shape == COLOR_CLEAR || shape == HINT_COLOR_CLEAR || shape == COLOR_MRT);
    CHECK(vk_depth_only == want_depth,
          "terminal DS aspect shape=%d depth_dontcare_stencil_store=%d", shape, vk_depth_only);
    CHECK(vk_stencil_only == want_stencil,
          "terminal DS aspect shape=%d stencil_dontcare_depth_store=%d", shape, vk_stencil_only);
    CHECK(vk_color == want_color, "terminal color shape=%d color_dontcare=%d", shape, vk_color);
    CHECK((outlined.terminal_discard != 0) == (want_depth || want_stencil || want_color),
          "terminal proof follows physical pass shape=%d", shape);
    if (shape == IDLE) {
        rp_pass_note_draw(0);
        rp_pass_prepare_draw(active, 0xf);
        if (rp_pass_plan_ds) rp_pass_plan_ds(stream + sizeof first);
        CHECK(rp_pass_begin(active), "second draw reuses the render pass");
        draw_span(pipe, 0.75f, 17, ZW / 2, ZW / 2);
    }
    rp_pass_note_draw(rp_S.ds.dt || rp_S.ds.dw || rp_S.ds.st ? RP_NOTE_DS : 0);
    if (shape == DEPTH_REOPEN) {
        rp_pass_end_xfer(active);
        rp_pass_prepare_draw(active, 0xf);
        CHECK(rp_pass_begin(active), "intermediate physical pass reopens");
        draw(with_ds, 0.75f, 17);
        rp_pass_note_draw(0);
    }
    if (rp_pass_discard_hint && (color_hint || shape == HINT_DS_CLEAR))
        rp_pass_discard_hint(color_hint ? 0 : -1, stream + sizeof first);

    nvn_gpu_buf observed = NULL;
    if (shape == COLOR_COPY_OBSERVER || shape == COLOR_CROSS_SUBMIT) {
        rp_pass_flush(active);
        if (shape == COLOR_CROSS_SUBMIT) {
            submit_and_wait();
            active = begin_cmd();
        }
        observed = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_copy_tex_buf(active, rp_tex_resolve(&color, active), 0, 0, NULL, observed, 0, W * 4, W * H * 4);
    }
    vk_clear_color = vk_clear_depth = vk_clear_stencil = vk_clear_kept_stencil = vk_clear_kept_depth = 0;
    watching = shape >= ASPECT_DEPTH && !(enabled & 16) ? 2 : 0;
    if (clear) rp_pass_clear_ds(active, &cl, after);
    if (color_clear) rp_pass_clear_color(active, &cc, after);
    rp_pass_flush(active);
    watching = 0;
    if (want_depth && !(enabled & 16))
        CHECK(vk_clear_depth == 1 && vk_clear_kept_stencil == 1, "terminal depth successor CLEAR/LOAD");
    if (want_stencil && !(enabled & 16))
        CHECK(vk_clear_stencil == 1 && vk_clear_kept_depth == 1, "terminal stencil successor CLEAR/LOAD");
    if (want_color && !(enabled & 16))
        CHECK(vk_clear_color == 1, "terminal color successor CLEAR/STORE");

    nvn_gpu_tex probe = make_tex(zw, H, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view pv = make_view(probe);
    nvn_gpu_pass_desc verify = init;
    verify.ncolor = 1;
    verify.color[0] = (nvn_gpu_att){.view = pv, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
    verify.color[0].clear.f[2] = verify.color[0].clear.f[3] = 1;
    verify.depth.load = verify.stencil.load = NVN_GPU_LOAD_LOAD;
    nvn_gpu_pass_begin(active, &verify);
    if (shape == RECT_CLEAR) draw(probe_ds, 0.25f, 17);
    if (shape == FULL_CLEAR || shape == ALIAS_CLEAR) draw(probe_ds, 0.25f, 5);
    else if (shape == ASPECT_DEPTH || shape == HINT_DS_CLEAR || shape == DEPTH_MIXED || shape == DEPTH_REOPEN)
        draw(probe_ds, 0.25f, 17);
    else if (shape == ASPECT_STENCIL) draw(probe_ds, 0.75f, 5);
    else if (shape == DEPTH_PARTIAL) draw(probe_ds, 0.25f, 21);
    else if (shape == PARTIAL_STENCIL) draw(probe_ds, 0.25f, 21);
    else draw(probe_ds, 0.75f, shape == MASKED_CLEAR ? 21u : 17u);
    nvn_gpu_pass_end(active);

    nvn_gpu_buf c = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf z = make_buf(zw * H * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_copy_tex_buf(active, rp_tex_resolve(&color, active), 0, 0, NULL, c, 0, W * 4, W * H * 4);
    nvn_gpu_copy_tex_buf(active, probe, 0, 0, NULL, z, 0, zw * 4, zw * H * 4);
    nvn_gpu_buf untouched = NULL;
    if (shape == COLOR_MRT) {
        untouched = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_copy_tex_buf(active, rp_tex_resolve(&other, active), 0, 0, NULL, untouched, 0, W * 4, W * H * 4);
    }
    const int before_submit = g_fail;
    submit_and_wait();
    if (g_fail == before_submit) {
        const uint8_t *pc = buf_cpu(c), *pz = buf_cpu(z);
        const uint8_t *po = observed ? buf_cpu(observed) : NULL;
        const uint8_t yellow[4] = {255,255,0,255};
        const uint8_t *pu = untouched ? buf_cpu(untouched) : NULL;
        unsigned bad_color = 0, bad_depth = 0, bad_observer = 0, bad_mrt = 0;
        for (unsigned i = 0; i < W * H; i++) {
            const uint8_t *expected = color_clear ? (shape == COLOR_PARTIAL ? yellow : red) :
                                                   (i % W < (area_case ? W / 2 : zw) ? green : blue);
            bad_color += memcmp(pc + i*4, expected, 4) != 0;
            if (po) bad_observer += memcmp(po + i*4, green, 4) != 0;
            if (pu) bad_mrt += memcmp(pu + i*4, blue, 4) != 0;
        }
        for (unsigned i = 0; i < zw * H; i++) bad_depth += memcmp(pz + i*4, green, 4) != 0;
        verified_pixels += W * H + zw * H + (po ? W * H : 0) + (pu ? W * H : 0);
        CHECK(!bad_color && !bad_depth && !bad_observer && !bad_mrt,
              "TBDR pixels shape=%d color_bad=%u ds_bad=%u observer_bad=%u mrt_bad=%u",
              shape, bad_color, bad_depth, bad_observer, bad_mrt);
    }
    if (untouched) nvn_gpu_buf_release(untouched, NULL, NULL);
    if (observed) nvn_gpu_buf_release(observed, NULL, NULL);
    nvn_gpu_buf_release(c, NULL, NULL); nvn_gpu_buf_release(z, NULL, NULL);
    nvn_gpu_view_release(pv); nvn_gpu_tex_release(probe);
    rp_pass_reset(); rp_res_reset();
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    enabled = atoi(argv[2]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-tbdr-ds"};
    if (!open_backend()) return 2;
    no_ds = pipe_for(argv[1], 0); with_ds = pipe_for(argv[1], 1); probe_ds = pipe_for(argv[1], 2);
    mrt_ds = pipe_for(argv[1], 3);
    if (!no_ds || !with_ds || !probe_ds || !mrt_ds) return 2;
    if (enabled & 16) CHECK(g_dev.load_store_op_none_enabled, "NONE extension fixture support");
    for (enum shape shape = IDLE; shape <= DEPTH_TEST_AREA; shape++) run_case(shape, atoi(argv[3]));
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "idle DS close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("tbdr-ds Vulkan cases=%d failures=%d pixels=%u\n", DEPTH_TEST_AREA + 1, g_fail, verified_pixels);
    return g_fail ? 1 : 0;
}
