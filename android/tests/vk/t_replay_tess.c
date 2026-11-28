// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 端到端宿主 Vulkan 细分着色器（TCS / TES / PATCHES）夹具：
//   arm 0（正常设备）：探测并启用 tessellationShader + maxTessellationPatchSize，走真实
//     rp_state_op(OP_BIND_PROGRAM / OP_SET_PATCH_SIZE / OP_BIND_UBO) + rp_draw(PATCHES) +
//     backend_vulkan.c 绘制 3-CP 三角细分（验证 inner=1 vs inner=3 真实细分差异）与 4-CP 四边形细分，
//     同时验证缺阶段 / 非法拓扑 / patch_size 越界均显式返回错误或具名跳过（零静默兜底），VVL+syncval 0 错。
//   arm 1（缺特性负对照）：通过 --wrap=vkGetPhysicalDeviceFeatures 屏蔽 tessellationShader，
//     验证 vk_device 不启用、caps 明确无 NVN_GPU_CAP_TESSELLATION 与 max_patch_size=0、
//     TCS/TES 装载与 PATCHES 管线报 NVN_GPU_E_UNSUPPORTED、rp_draw 具名跳过 patches_without_tessellation_feature。
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <time.h>

static nvn_gpu_err err;
static atomic_int retired;
static unsigned failures;
static int g_hide_tess;

#define REQUIRE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL line %d: %s (rc=%d why=%s)\n", __LINE__, #x, err.rc.code, err.why); \
        exit(1); \
    } \
} while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    __real_vkGetPhysicalDeviceFeatures(pd, f);
    if (g_hide_tess && f) f->tessellationShader = VK_FALSE;
}
VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    __real_vkGetPhysicalDeviceFeatures2(pd, f);
    if (g_hide_tess && f) f->features.tessellationShader = VK_FALSE;
}

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) { (void)n; return d; }
void nvn_plat_log(int l, const char *c, const char *s) { (void)l; fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : ""); }
static void done(void *u, const nvn_gpu_done *d) { (void)u; (void)d; }
static void retirement(void *u) { (void)u; atomic_fetch_add(&retired, 1); }
void rp_buf_reset(void);

static uint8_t *load_file(const char *dir, const char *name, size_t *out_len) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb");
    REQUIRE(f != NULL);
    REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    REQUIRE(n > 0);
    rewind(f);
    uint8_t *data = malloc((size_t)n);
    REQUIRE(data != NULL);
    REQUIRE(fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *out_len = (size_t)n;
    return data;
}

static nvn_gpu_rc load_shader_rc(const char *dir, const char *name, nvn_gpu_stage stage,
                                 const nvn_gpu_layout *layout, nvn_gpu_shader *out) {
    size_t len = 0;
    uint8_t *data = load_file(dir, name, &len);
    nvn_gpu_shader_src s = {0};
    s.stage = stage;
    s.kind = NVN_GPU_BLOB_SPIRV;
    s.entry = "main";
    s.blob = data;
    s.bytes = len;
    s.layout = layout;
    nvn_gpu_rc rc = nvn_gpu_shader_load(&s, out, &err);
    free(data);
    return rc;
}

static nvn_gpu_shader load_shader(const char *dir, const char *name, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    nvn_gpu_shader h = NULL;
    GPU(load_shader_rc(dir, name, stage, layout, &h));
    return h;
}

static nvn_gpu_buf make_buf(uint64_t bytes, const void *data) {
    nvn_gpu_buf b = NULL;
    GPU(nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, data, &b, &err));
    return b;
}

// Replay stubs (same minimal harness as t_replay_ubo_range.c; rp_draw / rp_buf / rp_state / backend_vulkan are production code).
int g_s2a_logfd = 2, s2a_nvn_rast_flip;
enum { GUEST_BYTES = 0x20000, VBUF_TRI_OFF = 0x0000, VBUF_QUAD_OFF = 0x1000, TCS_UBO_OFF = 0x2000, TES_UBO_OFF = 0x3000 };
static alignas(4096) unsigned char guest[GUEST_BYTES];
static struct rp_prog prog;
static struct rp_tess_layout tess_layout[RP_PROG_STAGES - RP_PROG_INLINE_STAGES];
static p_set_rt target;
static tex_t target_tex;
static nvn_gpu_view target_view;
static nvn_gpu_cmd active;
static uint64_t serial = 1;
static unsigned emitted;
static int pass_open;
static uint64_t next_fence = 1;

void __real_nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a);
void __wrap_nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a) { emitted++; __real_nvn_gpu_draw(c, a); }
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int t) { (void)p; (void)t; return 0; }
static prog_t g_snap_prog;
static uint8_t g_snap_keys[6][8], g_snap_ok, g_snap_stage[6];
const prog_t *nvn_prog_snap_get(uint64_t s, uint8_t k[6][8], uint8_t *o, uint8_t t[6]) {
    if (!g_snap_prog.serial || g_snap_prog.serial != s) return NULL;
    if (k) memcpy(k, g_snap_keys, sizeof g_snap_keys);
    if (o) *o = g_snap_ok;
    if (t) memcpy(t, g_snap_stage, sizeof g_snap_stage);
    return &g_snap_prog;
}
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **o) { (void)q; *o = &prog; return 0; }
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 1; }
uint64_t rp_frame(void) { return 1; }
void rp_perf_add(int i, uint64_t n) { (void)i; (void)n; }
uint64_t rp_seg_serial(void) { return serial; }
uint64_t rp_sub_serial(void) { return serial; }
uint64_t rp_sub_fence(void) { return next_fence; }
int rp_fence_done(uint64_t f) { (void)f; return 1; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last) { rp_pass_flush(active); *last = serial; return active; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) {
    uint64_t base = (uint64_t)(uintptr_t)guest;
    if (a < base || n > sizeof guest || a - base > sizeof guest - n) return 0;
    if (m) *m = guest;
    if (s) *s = sizeof guest;
    return 1;
}
size_t nvn_dirty_page_size(void) { return 4096; }
uint64_t nvn_dirty_now(void) { return 1; }
uint64_t nvn_dirty_mapping_generation(void) { return 1; }
int nvn_dirty_tracking_unreliable(void) { return 1; }
const void *nvn_dirty_span(const void *a, size_t n) { (void)a; (void)n; return NULL; }
long nvn_dirty_page_gens(const void *s, const void *a, size_t l, uint64_t *g, size_t n) { (void)s; (void)a; (void)l; (void)g; (void)n; return -1; }
void nvn_dirty_protect(const void *a, size_t n) { (void)a; (void)n; }
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
nvn_gpu_view rp_view_by_id(int i, nvn_gpu_cmd c, tex_t *t) { (void)i; (void)c; if (t) memset(t, 0, sizeof *t); return NULL; }
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) { (void)t; (void)v; (void)c; return NULL; }
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int i, uint8_t t, uint8_t k, int e, nvn_gpu_cmd c, tex_t *o) { (void)i; (void)t; (void)k; (void)e; (void)c; if (o) memset(o, 0, sizeof *o); return NULL; }
nvn_gpu_view rp_view_by_ubo_handle_decl(int i, uint8_t t, uint8_t k, nvn_gpu_cmd c, tex_t *o) { return rp_view_by_id_decl(i, t, k, 0, c, o); }
void rp_decl_probe_begin(void) {}
int rp_decl_probe_end(void) { return 0; }
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c, int samp, int base, uint32_t n, uint8_t dtt, uint8_t dcls, const char **why) {
    (void)c; (void)samp; (void)base; (void)n; (void)dtt; (void)dcls;
    if (why) *why = "pool_table_unresolved";
    return NULL;
}
nvn_gpu_samp rp_samp_by_id(int i) { (void)i; return NULL; }
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) { (void)pb; (void)pn; return rp_samp_for_tex(sid, tid); }
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int i, tex_t *t, view_t *v, int *h) { (void)i; (void)t; (void)v; (void)h; return 0; }
uint64_t rp_tex_reg_gen(void) { return 1; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int c) { (void)h; (void)n; (void)b; (void)c; return 0; }
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; }
int rp_tex_debug_state(uint64_t s) { (void)s; return 0; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s) { (void)s; return NULL; }
const p_set_rt *rp_pass_target(void) { return &target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = *h = 64; }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { (void)w; (void)h; }
void rp_pass_note_draw(int d) { (void)d; }
int rp_pass_parked(void) { return 0; }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask) { (void)c; (void)color_write_mask; }
void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *ts, int n) { (void)c; (void)ts; (void)n; }
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }

static void pass(nvn_gpu_cmd c, int clear) {
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1;
    p.color[0].view = target_view;
    p.color[0].load = clear ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD;
    p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = p.h = 64;
    p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    pass_open = 1;
}
int rp_pass_begin(nvn_gpu_cmd c) { if (!pass_open) pass(c, 0); return 1; }
void rp_pass_flush(nvn_gpu_cmd c) { if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; } }

static void bind_stages_mask(uint32_t mask, uint64_t prog_serial) {
    memset(&g_snap_prog, 0, sizeof g_snap_prog);
    g_snap_prog.magic = MAGIC('P', 'R', 'O', 'G');
    g_snap_prog.serial = prog_serial;
    memset(g_snap_keys, 0, sizeof g_snap_keys);
    memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    g_snap_ok = 0;
    uint32_t n = 0;
    for (uint32_t st = 0; st < 5; st++) {
        if (!(mask & (1u << st))) continue;
        g_snap_stage[n] = (uint8_t)st;
        g_snap_ok |= (uint8_t)(1u << n);
        g_snap_keys[n][0] = (uint8_t)(st + 1);
        g_snap_keys[n][1] = (uint8_t)prog_serial;
        n++;
    }
    g_snap_prog.nstage = (int32_t)n;
    p_bind_program_id id = {.prog = &g_snap_prog, .serial = prog_serial, .stages = 0x1f};
    REQUIRE(rp_state_op(OP_BIND_PROGRAM, &id, (uint32_t)(8 + sizeof id)));
    REQUIRE(rp_shader_graphics_stages() == mask);
}

static void setup_common_state(uint64_t vbuf_off, uint64_t vbuf_size) {
    rp_state_reset();
    rp_draw_reset();
    nvn_unimpl_reset();
    rp_S.nattr = 1;
    rp_S.nstream = 1;
    rp_S.attr[0].fmt = NVN_FORMAT_RGBA32F;
    rp_S.attr[0].offset = 0;
    rp_S.attr[0].stream = 0;
    rp_S.stream[0].stride = 16;
    rp_S.vbuf[0].address = (uint64_t)(uintptr_t)(guest + vbuf_off);
    rp_S.vbuf[0].size = vbuf_size;
    rp_S.color.logic_op = NVN_LOGIC_OP_COPY;
    rp_S.chmask.mask = 15;
    rp_S.poly.mode = NVN_POLYGON_MODE_FILL;
    rp_S.poly.cull = NVN_FACE_NONE;
    p_bind_buf tcs_ub = {.stage = NVN_GPU_SH_TCS, .index = 0, .addr = (uint64_t)(uintptr_t)(guest + TCS_UBO_OFF), .size = 32};
    p_bind_buf tes_ub = {.stage = NVN_GPU_SH_TES, .index = 0, .addr = (uint64_t)(uintptr_t)(guest + TES_UBO_OFF), .size = 16};
    REQUIRE(rp_state_op(OP_BIND_UBO, &tcs_ub, (uint32_t)(8 + sizeof tcs_ub)));
    REQUIRE(rp_state_op(OP_BIND_UBO, &tes_ub, (uint32_t)(8 + sizeof tes_ub)));
}

static void run_draw_and_readback(nvn_gpu_tex tex, nvn_gpu_buf readback, uint32_t prim, uint32_t count) {
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    serial++;
    rp_dirty_all();
    nvn_gpu_compute_to_graphics(active);
    pass(active, 1);
    rp_pass_flush(active);
    p_draw draw = {.prim = (int32_t)prim, .count = (int32_t)count, .instances = 1};
    rp_draw(active, &draw, NULL, &draw);
    rp_pass_flush(active);
    nvn_gpu_copy_tex_buf(active, tex, 0, 0, NULL, readback, 0, 256, 16384);
    const uint64_t fence = next_fence++;
    GPU(nvn_gpu_cmd_commit(active, fence, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(fence, 5000000000ull, &err));
    active = NULL;
}

static void run_normal_arm(struct s2a_vk_device *device, const char *dir) {
    REQUIRE(device->core_features.tessellationShader == VK_TRUE);
    REQUIRE(device->properties.limits.maxTessellationPatchSize >= 4);

    nvn_gpu_open_desc open = {.host_device = device, .app_label = "replay-tess"};
    GPU(nvn_gpu_open(&open, &err));

    nvn_gpu_caps caps = {0};
    nvn_gpu_caps_get(&caps);
    REQUIRE((caps.caps & NVN_GPU_CAP_TESSELLATION) != 0);
    REQUIRE(caps.max_patch_size == device->properties.limits.maxTessellationPatchSize);
    if (device->core_features.vertexPipelineStoresAndAtomics) {
        REQUIRE((caps.storage_stages & (1u << NVN_GPU_SH_TCS)) != 0);
        REQUIRE((caps.storage_stages & (1u << NVN_GPU_SH_TES)) != 0);
        REQUIRE((caps.atomic_stages & (1u << NVN_GPU_SH_TCS)) != 0);
        REQUIRE((caps.atomic_stages & (1u << NVN_GPU_SH_TES)) != 0);
    }

    const nvn_gpu_layout_ent tcs_ent = {
        .set = 0, .binding = 0, .cls = NVN_GPU_RES_UBO, .src_kind = NVN_RB_CBUF,
        .src_reg = 0, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_TCS,
    };
    const nvn_gpu_layout_ent tes_ent = {
        .set = 0, .binding = 1, .cls = NVN_GPU_RES_UBO, .src_kind = NVN_RB_CBUF,
        .src_reg = 0, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_TES,
    };
    const nvn_gpu_layout tcs_layout = {.ent = &tcs_ent, .nent = 1};
    const nvn_gpu_layout tes_layout = {.ent = &tes_ent, .nent = 1};

    nvn_gpu_shader sh_vs = load_shader(dir, "tess_patch.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader sh_fs = load_shader(dir, "tess_patch.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_shader sh_tri_tcs = load_shader(dir, "tess_patch.tesc", NVN_GPU_SH_TCS, &tcs_layout);
    nvn_gpu_shader sh_tri_tes = load_shader(dir, "tess_patch.tese", NVN_GPU_SH_TES, &tes_layout);
    nvn_gpu_shader sh_quad_tcs = load_shader(dir, "tess_quad.tesc", NVN_GPU_SH_TCS, &tcs_layout);
    nvn_gpu_shader sh_quad_tes = load_shader(dir, "tess_quad.tese", NVN_GPU_SH_TES, &tes_layout);

    // 1) Direct backend contract checks: incomplete stages / wrong topology / patch_size bounds.
    {
        nvn_gpu_pipe_desc base = {0};
        base.st[NVN_GPU_SH_VS].sh = sh_vs;
        base.st[NVN_GPU_SH_FS].sh = sh_fs;
        base.cfmt[0] = NVN_FORMAT_RGBA8;
        base.lmap[0] = 0;
        base.color.logic_op = NVN_LOGIC_OP_COPY;
        base.chmask.mask = 15;
        base.poly.mode = NVN_POLYGON_MODE_FILL;
        base.poly.cull = NVN_FACE_NONE;
        base.prim = NVN_DRAW_PRIMITIVE_PATCHES;
        base.patch_size = 3;

        nvn_gpu_pipe bad = NULL;
        // PATCHES without TCS/TES -> UNSUPPORTED
        nvn_gpu_rc r = nvn_gpu_pipe_get(&base, NVN_GPU_PIPE_SYNC, &bad, &err);
        REQUIRE(r.code == NVN_GPU_E_UNSUPPORTED && bad == NULL);

        // TCS without TES -> UNSUPPORTED
        nvn_gpu_pipe_desc only_tcs = base;
        only_tcs.st[NVN_GPU_SH_TCS].sh = sh_tri_tcs;
        r = nvn_gpu_pipe_get(&only_tcs, NVN_GPU_PIPE_SYNC, &bad, &err);
        REQUIRE(r.code == NVN_GPU_E_UNSUPPORTED && bad == NULL);

        // TES without TCS -> UNSUPPORTED
        nvn_gpu_pipe_desc only_tes = base;
        only_tes.st[NVN_GPU_SH_TES].sh = sh_tri_tes;
        r = nvn_gpu_pipe_get(&only_tes, NVN_GPU_PIPE_SYNC, &bad, &err);
        REQUIRE(r.code == NVN_GPU_E_UNSUPPORTED && bad == NULL);

        // TCS+TES with TRIANGLES -> E_ARG
        nvn_gpu_pipe_desc wrong_prim = base;
        wrong_prim.st[NVN_GPU_SH_TCS].sh = sh_tri_tcs;
        wrong_prim.st[NVN_GPU_SH_TES].sh = sh_tri_tes;
        wrong_prim.prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
        r = nvn_gpu_pipe_get(&wrong_prim, NVN_GPU_PIPE_SYNC, &bad, &err);
        REQUIRE(r.code == NVN_GPU_E_ARG && bad == NULL);

        // patch_size == 0 -> E_ARG
        nvn_gpu_pipe_desc zero_ps = base;
        zero_ps.st[NVN_GPU_SH_TCS].sh = sh_tri_tcs;
        zero_ps.st[NVN_GPU_SH_TES].sh = sh_tri_tes;
        zero_ps.patch_size = 0;
        r = nvn_gpu_pipe_get(&zero_ps, NVN_GPU_PIPE_SYNC, &bad, &err);
        REQUIRE(r.code == NVN_GPU_E_ARG && bad == NULL);

        // patch_size > max_patch_size -> UNSUPPORTED
        nvn_gpu_pipe_desc over_ps = base;
        over_ps.st[NVN_GPU_SH_TCS].sh = sh_tri_tcs;
        over_ps.st[NVN_GPU_SH_TES].sh = sh_tri_tes;
        over_ps.patch_size = caps.max_patch_size + 1;
        r = nvn_gpu_pipe_get(&over_ps, NVN_GPU_PIPE_SYNC, &bad, &err);
        REQUIRE(r.code == NVN_GPU_E_UNSUPPORTED && bad == NULL);
    }

    // Prepare render target & guest buffers.
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 64;
    td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &target_view, &err));
    nvn_gpu_buf readback = make_buf(64 * 64 * 4, NULL);
    target_tex = (tex_t){.d = td.d, .serial = 1};
    target = (p_set_rt){.n = 1, .color = {&target_tex}};

    memset(guest, 0, sizeof guest);
    // 3-CP triangle covering center (0, 0): (-0.8, -0.8), (0.8, -0.8), (0.0, 0.8)
    const float tri_verts[3][4] = {
        {-0.8f, -0.8f, 0.0f, 1.0f},
        { 0.8f, -0.8f, 0.0f, 1.0f},
        { 0.0f,  0.8f, 0.0f, 1.0f},
    };
    memcpy(guest + VBUF_TRI_OFF, tri_verts, sizeof tri_verts);
    // 4-CP fullscreen quad: (-1,-1), (1,-1), (1,1), (-1,1)
    const float quad_verts[4][4] = {
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f},
        { 1.0f,  1.0f, 0.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 1.0f},
    };
    memcpy(guest + VBUF_QUAD_OFF, quad_verts, sizeof quad_verts);

    // 2) Draw A: 3-CP triangle patch with outer=1, inner=1 -> NO interior vertex -> center R == 0.
    memset(&prog, 0, sizeof prog);
    prog.sh[NVN_GPU_SH_VS] = sh_vs;
    prog.sh[NVN_GPU_SH_FS] = sh_fs;
    prog.sh[NVN_GPU_SH_TCS] = sh_tri_tcs;
    prog.sh[NVN_GPU_SH_TES] = sh_tri_tes;
    prog.tess = tess_layout;
    prog.nent[NVN_GPU_SH_TCS] = 1;
    tess_layout[NVN_GPU_SH_TCS - RP_PROG_INLINE_STAGES].ent[0] = tcs_ent;
    prog.nent[NVN_GPU_SH_TES] = 1;
    tess_layout[NVN_GPU_SH_TES - RP_PROG_INLINE_STAGES].ent[0] = tes_ent;
    prog.attr_mask = 1;
    prog.color_out_mask = 15;

    setup_common_state(VBUF_TRI_OFF, sizeof tri_verts);
    bind_stages_mask((1u << NVN_GPU_SH_VS) | (1u << NVN_GPU_SH_FS) | (1u << NVN_GPU_SH_TCS) | (1u << NVN_GPU_SH_TES), 1);
    p_set_patch_size ps3 = {.size = 3};
    REQUIRE(rp_state_op(OP_SET_PATCH_SIZE, &ps3, (uint32_t)(8 + sizeof ps3)));
    REQUIRE(rp_S.patch_size == 3);

    const float tcs_lvl1[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f};
    const float tes_tint1[4] = {0.0f, 128.0f / 255.0f, 64.0f / 255.0f, 1.0f};
    memcpy(guest + TCS_UBO_OFF, tcs_lvl1, sizeof tcs_lvl1);
    memcpy(guest + TES_UBO_OFF, tes_tint1, sizeof tes_tint1);

    unsigned before = emitted;
    run_draw_and_readback(tex, readback, NVN_DRAW_PRIMITIVE_PATCHES, 3);
    REQUIRE(emitted == before + 1);
    REQUIRE(nvn_unimpl_kinds() == 0);
    const uint8_t *px = nvn_gpu_buf_cpu(readback);
    const uint8_t *center1 = px + (32 * 64 + 32) * 4;
    const uint8_t *corner1 = px + (0 * 64 + 0) * 4;
    printf("tri-patch lvl=1 center=(%u,%u,%u,%u) corner=(%u,%u,%u,%u)\n",
           center1[0], center1[1], center1[2], center1[3],
           corner1[0], corner1[1], corner1[2], corner1[3]);
    REQUIRE(corner1[0] == 0 && corner1[1] == 0 && corner1[2] == 0 && corner1[3] == 255);
    REQUIRE(center1[0] == 0 && center1[1] == 128 && center1[2] == 64 && center1[3] == 255);

    // 3) Draw B: Same 3-CP triangle patch, update TCS UBO to outer=3, inner=3 -> interior vertex created -> center R == 255!
    const float tcs_lvl3[8] = {3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.0f, 0.0f};
    memcpy(guest + TCS_UBO_OFF, tcs_lvl3, sizeof tcs_lvl3);
    before = emitted;
    run_draw_and_readback(tex, readback, NVN_DRAW_PRIMITIVE_PATCHES, 3);
    REQUIRE(emitted == before + 1);
    REQUIRE(nvn_unimpl_kinds() == 0);
    const uint8_t *center3 = px + (32 * 64 + 32) * 4;
    printf("tri-patch lvl=3 center=(%u,%u,%u,%u)\n", center3[0], center3[1], center3[2], center3[3]);
    REQUIRE(center3[0] == 255 && center3[1] == 128 && center3[2] == 64 && center3[3] == 255);

    // 4) Draw C: Switch to 4-CP quad patch via OP_SET_PATCH_SIZE=4 -> fills all 4096 pixels with (32,200,96,255).
    prog.sh[NVN_GPU_SH_TCS] = sh_quad_tcs;
    prog.sh[NVN_GPU_SH_TES] = sh_quad_tes;
    bind_stages_mask((1u << NVN_GPU_SH_VS) | (1u << NVN_GPU_SH_FS) | (1u << NVN_GPU_SH_TCS) | (1u << NVN_GPU_SH_TES), 2);
    p_set_patch_size ps4 = {.size = 4};
    REQUIRE(rp_state_op(OP_SET_PATCH_SIZE, &ps4, (uint32_t)(8 + sizeof ps4)));
    REQUIRE(rp_S.patch_size == 4);
    rp_S.vbuf[0].address = (uint64_t)(uintptr_t)(guest + VBUF_QUAD_OFF);
    rp_S.vbuf[0].size = sizeof quad_verts;
    const float tcs_quad_lvl[8] = {4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 0.0f, 0.0f};
    const float tes_tint_quad[4] = {32.0f / 255.0f, 200.0f / 255.0f, 96.0f / 255.0f, 1.0f};
    memcpy(guest + TCS_UBO_OFF, tcs_quad_lvl, sizeof tcs_quad_lvl);
    memcpy(guest + TES_UBO_OFF, tes_tint_quad, sizeof tes_tint_quad);

    before = emitted;
    run_draw_and_readback(tex, readback, NVN_DRAW_PRIMITIVE_PATCHES, 4);
    REQUIRE(emitted == before + 1);
    REQUIRE(nvn_unimpl_kinds() == 0);
    unsigned quad_bad = 0;
    const uint8_t want_quad[4] = {32, 200, 96, 255};
    for (unsigned i = 0; i < 64 * 64; i++) {
        if (memcmp(px + i * 4, want_quad, 4)) quad_bad++;
    }
    printf("quad-patch cp=4 pixels=4096 bad=%u\n", quad_bad);
    REQUIRE(quad_bad == 0);

    // 5) Replay-layer validation skips (zero backend draws, zero VVL errors).
    nvn_unimpl_reset();
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    serial++;
    rp_dirty_all();
    before = emitted;
    // 5a: TCS+TES bound with non-PATCHES prim -> skipped by name
    {
        p_draw d = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
        rp_draw(active, &d, NULL, &d);
    }
    REQUIRE(emitted == before && nvn_unimpl_count("draw", "tessellation_without_patches_topology") == 1);

    // 5b: PATCHES with incomplete stages (only TCS bound) -> skipped by name
    bind_stages_mask((1u << NVN_GPU_SH_VS) | (1u << NVN_GPU_SH_FS) | (1u << NVN_GPU_SH_TCS), 3);
    {
        p_draw d = {.prim = NVN_DRAW_PRIMITIVE_PATCHES, .count = 4, .instances = 1};
        rp_draw(active, &d, NULL, &d);
    }
    REQUIRE(emitted == before && nvn_unimpl_count("draw", "incomplete_tessellation_stages") == 1);

    // 5c: PATCHES with patch_size > max_patch_size -> skipped by name
    bind_stages_mask((1u << NVN_GPU_SH_VS) | (1u << NVN_GPU_SH_FS) | (1u << NVN_GPU_SH_TCS) | (1u << NVN_GPU_SH_TES), 4);
    rp_S.patch_size = (int)caps.max_patch_size + 1;
    {
        p_draw d = {.prim = NVN_DRAW_PRIMITIVE_PATCHES, .count = 4, .instances = 1};
        rp_draw(active, &d, NULL, &d);
    }
    REQUIRE(emitted == before && nvn_unimpl_count("draw", "patch_size_exceeds_device_limit") == 1);

    rp_pass_flush(active);
    nvn_gpu_cmd_abort(active);
    active = NULL;

    // Clean shutdown.
    rp_draw_reset();
    rp_buf_reset();
    nvn_gpu_shader_release(sh_vs);
    nvn_gpu_shader_release(sh_fs);
    nvn_gpu_shader_release(sh_tri_tcs);
    nvn_gpu_shader_release(sh_tri_tes);
    nvn_gpu_shader_release(sh_quad_tcs);
    nvn_gpu_shader_release(sh_quad_tes);
    nvn_gpu_view_release(target_view);
    nvn_gpu_tex_release(tex);
    const int target_retired = atomic_load(&retired) + 1;
    nvn_gpu_buf_release(readback, retirement, NULL);
    const struct timespec pause = {.tv_nsec = 1000000};
    for (int i = 0; i < 5000 && atomic_load(&retired) < target_retired; i++) nanosleep(&pause, NULL);
    REQUIRE(atomic_load(&retired) == target_retired);
    GPU(nvn_gpu_close(5000000000ull, &err));
    printf("replay-tess arm=0 max_patch=%u tri_subdiv=ok quad_cp4=ok rejections=ok failures=%u\n",
           caps.max_patch_size, failures);
}

static void run_missing_feature_arm(struct s2a_vk_device *device, const char *dir) {
    REQUIRE(device->core_features.tessellationShader == VK_FALSE);

    nvn_gpu_open_desc open = {.host_device = device, .app_label = "replay-tess-no-feat"};
    GPU(nvn_gpu_open(&open, &err));

    nvn_gpu_caps caps = {0};
    nvn_gpu_caps_get(&caps);
    REQUIRE((caps.caps & NVN_GPU_CAP_TESSELLATION) == 0);
    REQUIRE(caps.max_patch_size == 0);
    REQUIRE((caps.storage_stages & ((1u << NVN_GPU_SH_TCS) | (1u << NVN_GPU_SH_TES))) == 0);
    REQUIRE((caps.atomic_stages & ((1u << NVN_GPU_SH_TCS) | (1u << NVN_GPU_SH_TES))) == 0);

    // Loading TCS or TES must fail explicitly with NVN_GPU_E_UNSUPPORTED.
    nvn_gpu_shader sh_tcs = NULL, sh_tes = NULL;
    nvn_gpu_rc r_tcs = load_shader_rc(dir, "tess_patch.tesc", NVN_GPU_SH_TCS, NULL, &sh_tcs);
    REQUIRE(r_tcs.code == NVN_GPU_E_UNSUPPORTED && sh_tcs == NULL);
    nvn_gpu_rc r_tes = load_shader_rc(dir, "tess_patch.tese", NVN_GPU_SH_TES, NULL, &sh_tes);
    REQUIRE(r_tes.code == NVN_GPU_E_UNSUPPORTED && sh_tes == NULL);

    // PATCHES pipeline creation must fail explicitly with NVN_GPU_E_UNSUPPORTED.
    nvn_gpu_shader sh_vs = load_shader(dir, "tess_patch.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader sh_fs = load_shader(dir, "tess_patch.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc pd = {0};
    pd.st[NVN_GPU_SH_VS].sh = sh_vs;
    pd.st[NVN_GPU_SH_FS].sh = sh_fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.color.logic_op = NVN_LOGIC_OP_COPY;
    pd.chmask.mask = 15;
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.prim = NVN_DRAW_PRIMITIVE_PATCHES;
    pd.patch_size = 3;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc r_pipe = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err);
    REQUIRE(r_pipe.code == NVN_GPU_E_UNSUPPORTED && pipe == NULL);

    // Replay rp_draw(PATCHES) must skip explicitly with patches_without_tessellation_feature (zero backend draws).
    rp_state_reset();
    rp_draw_reset();
    nvn_unimpl_reset();
    bind_stages_mask((1u << NVN_GPU_SH_VS) | (1u << NVN_GPU_SH_FS) | (1u << NVN_GPU_SH_TCS) | (1u << NVN_GPU_SH_TES), 10);
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    serial++;
    const unsigned before = emitted;
    p_draw draw = {.prim = NVN_DRAW_PRIMITIVE_PATCHES, .count = 3, .instances = 1};
    rp_draw(active, &draw, NULL, &draw);
    REQUIRE(emitted == before);
    REQUIRE(nvn_unimpl_count("draw", "patches_without_tessellation_feature") == 1);
    nvn_gpu_cmd_abort(active);
    active = NULL;

    rp_draw_reset();
    rp_buf_reset();
    nvn_gpu_shader_release(sh_vs);
    nvn_gpu_shader_release(sh_fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    printf("replay-tess arm=1 missing_feature_unsupported=ok failures=%u\n", failures);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const char *shader_dir = argv[1];
    const int arm = atoi(argv[2]);
    g_hide_tess = (arm == 1);

    struct s2a_vk_device device;
    char why[256] = {0};
    if (s2a_vk_device_create(&device, why, sizeof why)) {
        fprintf(stderr, "device create failed: %s\n", why);
        return 2;
    }
    printf("device: %s (%s) arm=%d tess=%u max_patch=%u\n",
           device.properties.deviceName, device.driver_name, arm,
           (unsigned)device.core_features.tessellationShader,
           (unsigned)device.properties.limits.maxTessellationPatchSize);

    if (arm == 0) run_normal_arm(&device, shader_dir);
    else if (arm == 1) run_missing_feature_arm(&device, shader_dir);
    else return 2;

    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    return failures ? 1 : 0;
}
