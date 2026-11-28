// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract fixture for command-buffer descriptor-set bind shadow,
// minimal dirty-range rebind, and compute pipeline/push shadow (debug.s2a.nvk_bind_dedup).
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err err;
static int g_dq_mode = 0;
static int g_memo_mode = 1;
static int g_dedup_flag = 1;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "bind-dedup fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

// ---- Vulkan command observers (covers both dq=0 direct symbols and dq=1 GetDeviceProcAddr) ----
static _Atomic uint64_t g_bds_calls = 0;
static _Atomic uint64_t g_bds_sets = 0;
static _Atomic uint64_t g_bds_dyn = 0;
static _Atomic uint64_t g_bds_first_gt0 = 0;
static _Atomic uint64_t g_cpipe_binds = 0;
static _Atomic uint64_t g_cpush_calls = 0;

static PFN_vkCmdBindDescriptorSets g_real_bds = NULL;
static PFN_vkCmdBindPipeline g_real_bp = NULL;
static PFN_vkCmdPushConstants g_real_pc = NULL;

static void obs_reset(void) {
    atomic_store(&g_bds_calls, 0);
    atomic_store(&g_bds_sets, 0);
    atomic_store(&g_bds_dyn, 0);
    atomic_store(&g_bds_first_gt0, 0);
    atomic_store(&g_cpipe_binds, 0);
    atomic_store(&g_cpush_calls, 0);
}

static void note_bds(uint32_t first, uint32_t count, uint32_t ndyn) {
    atomic_fetch_add(&g_bds_calls, 1);
    atomic_fetch_add(&g_bds_sets, count);
    atomic_fetch_add(&g_bds_dyn, ndyn);
    if (first > 0) atomic_fetch_add(&g_bds_first_gt0, 1);
}

static VKAPI_ATTR void VKAPI_CALL obs_bds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
                                          uint32_t first, uint32_t n, const VkDescriptorSet *s,
                                          uint32_t nd, const uint32_t *d) {
    note_bds(first, n, nd);
    g_real_bds(cb, bp, l, first, n, s, nd, d);
}

static VKAPI_ATTR void VKAPI_CALL obs_bp(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) atomic_fetch_add(&g_cpipe_binds, 1);
    g_real_bp(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL obs_pc(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags sf,
                                         uint32_t off, uint32_t sz, const void *v) {
    if (sf & VK_SHADER_STAGE_COMPUTE_BIT) atomic_fetch_add(&g_cpush_calls, 1);
    g_real_pc(cb, l, sf, off, sz, v);
}

PFN_vkVoidFunction __real_vkGetDeviceProcAddr(VkDevice, const char *);
PFN_vkVoidFunction __wrap_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(dev, name);
    if (!fn || !name) return fn;
    if (!strcmp(name, "vkCmdBindDescriptorSets")) {
        g_real_bds = (PFN_vkCmdBindDescriptorSets)fn;
        return (PFN_vkVoidFunction)obs_bds;
    }
    if (!strcmp(name, "vkCmdBindPipeline")) {
        g_real_bp = (PFN_vkCmdBindPipeline)fn;
        return (PFN_vkVoidFunction)obs_bp;
    }
    if (!strcmp(name, "vkCmdPushConstants")) {
        g_real_pc = (PFN_vkCmdPushConstants)fn;
        return (PFN_vkVoidFunction)obs_pc;
    }
    return fn;
}

void __real_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                    const VkDescriptorSet *, uint32_t, const uint32_t *);
void __wrap_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
                                    uint32_t first, uint32_t n, const VkDescriptorSet *s,
                                    uint32_t nd, const uint32_t *d) {
    note_bds(first, n, nd);
    __real_vkCmdBindDescriptorSets(cb, bp, l, first, n, s, nd, d);
}

void __real_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
void __wrap_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) atomic_fetch_add(&g_cpipe_binds, 1);
    __real_vkCmdBindPipeline(cb, bp, p);
}

void __real_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void *);
void __wrap_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags sf,
                               uint32_t off, uint32_t sz, const void *v) {
    if (sf & VK_SHADER_STAGE_COMPUTE_BIT) atomic_fetch_add(&g_cpush_calls, 1);
    __real_vkCmdPushConstants(cb, l, sf, off, sz, v);
}

// ---- Platform stubs ----
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_mode;
    if (!strcmp(n, "nvk_desc_memo")) return g_memo_mode;
    if (!strcmp(n, "nvk_bind_dedup")) return g_dedup_flag;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done(void *u, const nvn_gpu_done *d) { REQUIRE(d->status == NVN_GPU_DONE_OK); }

static void submit_and_wait(nvn_gpu_cmd cmd) {
    uint64_t s = ++g_sig;
    GPU(nvn_gpu_cmd_commit(cmd, s, on_done, NULL, &err));
    GPU(nvn_gpu_fence_wait(s, 10000000000ull, &err));
}

static nvn_gpu_shader load_shader(const char *dir, const char *name, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb");
    REQUIRE(f);
    REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    REQUIRE(n > 0);
    rewind(f);
    void *data = malloc((size_t)n);
    REQUIRE(data);
    REQUIRE(fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    nvn_gpu_shader_src src = {0};
    src.stage = stage;
    src.kind = NVN_GPU_BLOB_SPIRV;
    src.entry = "main";
    src.blob = data;
    src.bytes = (size_t)n;
    src.layout = layout;
    nvn_gpu_shader sh = NULL;
    GPU(nvn_gpu_shader_load(&src, &sh, &err));
    free(data);
    return sh;
}

static int near_u8(uint8_t a, uint8_t b, int tol) {
    int d = (int)a - (int)b;
    return d >= -tol && d <= tol;
}

static void check_rgba(const char *tag, const uint8_t *px, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tol) {
    if (!near_u8(px[0], r, tol) || !near_u8(px[1], g, tol) || !near_u8(px[2], b, tol) || !near_u8(px[3], a, tol)) {
        fprintf(stderr, "FAIL %s: got=(%u,%u,%u,%u) want=(%u,%u,%u,%u)+-%d\n",
                tag, px[0], px[1], px[2], px[3], r, g, b, a, tol);
        g_failures++;
    }
}

static void check_u64(const char *tag, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got=%llu want=%llu\n", tag, (unsigned long long)got, (unsigned long long)want);
        g_failures++;
    }
}

static void set_scissor_dyn(nvn_gpu_cmd cmd, uint32_t w, uint32_t h, int32_t sx, int32_t sy, uint32_t sw, uint32_t sh) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1;
    dyn.vp[0][2] = (float)w;
    dyn.vp[0][3] = (float)h;
    dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1;
    dyn.sc[0] = sx; dyn.sc[1] = sy; dyn.sc[2] = (int32_t)sw; dyn.sc[3] = (int32_t)sh;
    nvn_gpu_set_dyn(cmd, &dyn);
}

static nvn_gpu_pipe make_2set_ubo_pipe(const char *dir, const char *frag_name, uint32_t set1_binding,
                                       nvn_gpu_shader *vs_out, nvn_gpu_shader *fs_out) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].set = 1; ent[1].binding = set1_binding; ent[1].cls = NVN_GPU_RES_UBO; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    *vs_out = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    *fs_out = load_shader(dir, frag_name, NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = *vs_out;
    pd.st[NVN_GPU_SH_FS].sh = *fs_out;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    return pipe;
}

static nvn_gpu_buf make_ubo_buf(float r0, float g0, float r1, float g1) {
    uint8_t data[512] = {0};
    const float v0[4] = {r0, g0, 0.0f, 1.0f};
    const float v1[4] = {r1, g1, 0.0f, 1.0f};
    memcpy(data, v0, sizeof v0);
    memcpy(data + 256, v1, sizeof v1);
    nvn_gpu_buf b = NULL;
    GPU(nvn_gpu_buf_create(sizeof data, NVN_GPU_MEM_SHARED, data, &b, &err));
    return b;
}

static void make_rt(uint32_t w, uint32_t h, nvn_gpu_tex *t, nvn_gpu_view *v, nvn_gpu_buf *rb) {
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = w; td.d.height = h; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    GPU(nvn_gpu_tex_create(&td, t, &err));
    GPU(nvn_gpu_view_create(*t, NULL, v, &err));
    size_t bytes = (size_t)w * (size_t)h * 4;
    void *z = calloc(1, bytes);
    REQUIRE(z);
    GPU(nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, z, rb, &err));
    free(z);
}

// Case 1: Partial-set rebind (2 sets): change only set 1 -> firstSet=1, count=1; then change only set 0 -> firstSet=0, count=1
static void case1_partial_set_range(const char *dir, uint64_t *tot_calls, uint64_t *tot_sets) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_2set_ubo_pipe(dir, "ubo_dyn.frag", 3, &vs, &fs);
    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL; nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &rt, &rtv, &rb);
    nvn_gpu_buf ub_r1 = make_ubo_buf(1.0f, 0.0f, 0.0f, 0.0f);
    nvn_gpu_buf ub_r2 = make_ubo_buf(0.5f, 0.0f, 0.0f, 0.0f);
    nvn_gpu_buf ub_g1 = make_ubo_buf(0.0f, 0.25f, 0.0f, 0.0f);
    nvn_gpu_buf ub_g2 = make_ubo_buf(0.0f, 0.75f, 0.0f, 0.0f);

    obs_reset();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    // Draw 1 -> pixel (0,0): R=255, G=64
    set_scissor_dyn(cmd, 4, 4, 0, 0, 2, 2);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub_r1; b[0].u.b.off = 0; b[0].u.b.len = 16;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub_g1; b[1].u.b.off = 0; b[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 2 -> pixel (2,0): set 0 unchanged, ONLY set 1 changes -> R=255, G=191
    set_scissor_dyn(cmd, 4, 4, 2, 0, 2, 2);
    b[1].u.b.buf = ub_g2;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 3 -> pixel (0,2): ONLY set 0 changes, set 1 unchanged -> R=128, G=191
    set_scissor_dyn(cmd, 4, 4, 0, 2, 2, 2);
    b[0].u.b.buf = ub_r2;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c1-draw1", px + (0 * 4 + 0) * 4, 255, 64, 0, 255, 2);
    check_rgba("c1-draw2", px + (0 * 4 + 2) * 4, 255, 191, 0, 255, 2);
    check_rgba("c1-draw3", px + (2 * 4 + 0) * 4, 128, 191, 0, 255, 2);

    const uint64_t calls = atomic_load(&g_bds_calls);
    const uint64_t sets = atomic_load(&g_bds_sets);
    const uint64_t fgt0 = atomic_load(&g_bds_first_gt0);
    *tot_calls += calls; *tot_sets += sets;
    check_u64("c1-calls", calls, 3);
    if (g_dedup_flag) {
        check_u64("c1-sets-dedup", sets, 4);    // 2 + 1 + 1 instead of 2 + 2 + 2
        check_u64("c1-first-gt0-dedup", fgt0, 1);
    } else {
        check_u64("c1-sets-base", sets, 6);
        check_u64("c1-first-gt0-base", fgt0, 0);
    }

    nvn_gpu_buf_release(ub_g2, NULL, NULL);
    nvn_gpu_buf_release(ub_g1, NULL, NULL);
    nvn_gpu_buf_release(ub_r2, NULL, NULL);
    nvn_gpu_buf_release(ub_r1, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 2: Dynamic UBO partial offset update (only set 1 offset changes) + dynamic offset toggle-revert full skip
static void case2_dyn_offset_partial_and_skip(const char *dir, uint64_t *tot_calls, uint64_t *tot_sets) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_2set_ubo_pipe(dir, "ubo_dyn.frag", 3, &vs, &fs);
    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL; nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &rt, &rtv, &rb);
    // Offset 0: (1.0, 0.25), Offset 256: (0.5, 1.0)
    nvn_gpu_buf ub = make_ubo_buf(1.0f, 0.25f, 0.5f, 1.0f);

    obs_reset();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    // Draw 1: set 0 off=0 (R=1.0), set 1 off=0 (G=0.25) -> (255, 64, 0, 255)
    set_scissor_dyn(cmd, 4, 4, 0, 0, 2, 2);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub; b[0].u.b.off = 0; b[0].u.b.len = 16;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub; b[1].u.b.off = 0; b[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 2: only set 1 dynamic offset 0 -> 256 -> (255, 255, 0, 255)
    set_scissor_dyn(cmd, 4, 4, 2, 0, 2, 2);
    b[1].u.b.off = 256;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 3: toggle set 1 off 256->0->256 before draw -> dyn_dirty=1 yet bound state identical!
    set_scissor_dyn(cmd, 4, 4, 0, 2, 2, 2);
    b[1].u.b.off = 0;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    b[1].u.b.off = 256;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c2-draw1", px + (0 * 4 + 0) * 4, 255, 64, 0, 255, 2);
    check_rgba("c2-draw2", px + (0 * 4 + 2) * 4, 255, 255, 0, 255, 2);
    check_rgba("c2-draw3", px + (2 * 4 + 0) * 4, 255, 255, 0, 255, 2);

    const uint64_t calls = atomic_load(&g_bds_calls);
    const uint64_t sets = atomic_load(&g_bds_sets);
    const uint64_t dyn = atomic_load(&g_bds_dyn);
    const uint64_t fgt0 = atomic_load(&g_bds_first_gt0);
    *tot_calls += calls; *tot_sets += sets;
    if (g_dedup_flag && g_memo_mode) {
        check_u64("c2-calls-dedup", calls, 2);   // Draw 3 fully skipped!
        check_u64("c2-sets-dedup", sets, 3);     // 2 + 1 + 0
        check_u64("c2-dyn-dedup", dyn, 3);       // 2 + 1 + 0 dynamic offsets
        check_u64("c2-first-gt0-dedup", fgt0, 1);
    } else if (g_dedup_flag && !g_memo_mode) {
        // With desc_memo=0, UBOs are static (dyn=0); Draw 2 & Draw 3 allocate fresh set 1, both narrowed to firstSet=1, count=1
        check_u64("c2-calls-dedup-nomemo", calls, 3);
        check_u64("c2-sets-dedup-nomemo", sets, 4);   // 2 + 1 + 1 instead of 2 + 2 + 2
        check_u64("c2-dyn-dedup-nomemo", dyn, 0);
        check_u64("c2-first-gt0-dedup-nomemo", fgt0, 2);
    } else {
        check_u64("c2-calls-base", calls, 3);
        check_u64("c2-sets-base", sets, 6);
        check_u64("c2-dyn-base", dyn, g_memo_mode ? 6 : 0);
    }

    nvn_gpu_buf_release(ub, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 3: desc_memo hit returning the exact VkDescriptorSet already bound -> 0 redundant vkCmdBindDescriptorSets
static void case3_memo_hit_full_skip(const char *dir, uint64_t *tot_calls, uint64_t *tot_sets) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_2set_ubo_pipe(dir, "ubo_dyn.frag", 3, &vs, &fs);
    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL; nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &rt, &rtv, &rb);
    nvn_gpu_buf ub_r = make_ubo_buf(1.0f, 0.0f, 0.0f, 0.0f);
    nvn_gpu_buf ub_g1 = make_ubo_buf(0.0f, 0.5f, 0.0f, 0.0f);
    nvn_gpu_buf ub_g2 = make_ubo_buf(0.0f, 1.0f, 0.0f, 0.0f);

    obs_reset();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    set_scissor_dyn(cmd, 4, 4, 0, 0, 2, 2);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ub_r; b[0].u.b.off = 0; b[0].u.b.len = 16;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ub_g1; b[1].u.b.off = 0; b[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Dirty set 1 with ub_g2 then immediately revert to ub_g1 -> desc_dirty=0b10, desc_memo returns same VkDescriptorSet!
    set_scissor_dyn(cmd, 4, 4, 2, 0, 2, 2);
    b[1].u.b.buf = ub_g2;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    b[1].u.b.buf = ub_g1;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c3-draw1", px + (0 * 4 + 0) * 4, 255, 128, 0, 255, 2);
    check_rgba("c3-draw2", px + (0 * 4 + 2) * 4, 255, 128, 0, 255, 2);

    const uint64_t calls = atomic_load(&g_bds_calls);
    const uint64_t sets = atomic_load(&g_bds_sets);
    *tot_calls += calls; *tot_sets += sets;
    if (g_dedup_flag && g_memo_mode) {
        check_u64("c3-calls-dedup-memo", calls, 1);  // Draw 2 completely skipped!
        check_u64("c3-sets-dedup-memo", sets, 2);
    } else if (g_dedup_flag && !g_memo_mode) {
        check_u64("c3-calls-dedup-nomemo", calls, 2); // Fresh set allocated for set 1, narrowed to 1 set
        check_u64("c3-sets-dedup-nomemo", sets, 3);
    } else {
        check_u64("c3-calls-base", calls, 2);
        check_u64("c3-sets-base", sets, 4);
    }

    nvn_gpu_buf_release(ub_g2, NULL, NULL);
    nvn_gpu_buf_release(ub_g1, NULL, NULL);
    nvn_gpu_buf_release(ub_r, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 4: Incompatible VkPipelineLayout switch (ubo_dyn.frag binding 3 <-> ubo_dyn34.frag binding 16) forces full rebind
static void case4_layout_switch(const char *dir, uint64_t *tot_calls, uint64_t *tot_sets) {
    nvn_gpu_shader vsA = NULL, fsA = NULL, vsB = NULL, fsB = NULL;
    nvn_gpu_pipe pipeA = make_2set_ubo_pipe(dir, "ubo_dyn.frag", 3, &vsA, &fsA);
    nvn_gpu_pipe pipeB = make_2set_ubo_pipe(dir, "ubo_dyn34.frag", 16, &vsB, &fsB);
    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL; nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &rt, &rtv, &rb);
    nvn_gpu_buf ubA0 = make_ubo_buf(1.0f, 0.0f, 0.0f, 0.0f);
    nvn_gpu_buf ubA1 = make_ubo_buf(0.0f, 0.25f, 0.0f, 0.0f);
    nvn_gpu_buf ubB0 = make_ubo_buf(0.5f, 0.0f, 0.0f, 0.0f);
    nvn_gpu_buf ubB1 = make_ubo_buf(0.0f, 0.75f, 0.0f, 0.0f);

    obs_reset();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    // Draw 1: pipeA -> (255, 64, 0, 255)
    nvn_gpu_set_pipe(cmd, pipeA);
    set_scissor_dyn(cmd, 4, 4, 0, 0, 2, 2);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ubA0; b[0].u.b.off = 0; b[0].u.b.len = 16;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_UBO; b[1].u.b.buf = ubA1; b[1].u.b.off = 0; b[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 2: pipeB (different VkPipelineLayout!) -> (128, 191, 0, 255)
    nvn_gpu_set_pipe(cmd, pipeB);
    set_scissor_dyn(cmd, 4, 4, 2, 0, 2, 2);
    b[0].u.b.buf = ubB0; b[1].u.b.buf = ubB1;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 3: back to pipeA -> (255, 64, 0, 255)
    nvn_gpu_set_pipe(cmd, pipeA);
    set_scissor_dyn(cmd, 4, 4, 0, 2, 2, 2);
    b[0].u.b.buf = ubA0; b[1].u.b.buf = ubA1;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw(cmd, &da);

    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c4-draw1", px + (0 * 4 + 0) * 4, 255, 64, 0, 255, 2);
    check_rgba("c4-draw2", px + (0 * 4 + 2) * 4, 128, 191, 0, 255, 2);
    check_rgba("c4-draw3", px + (2 * 4 + 0) * 4, 255, 64, 0, 255, 2);

    const uint64_t calls = atomic_load(&g_bds_calls);
    const uint64_t sets = atomic_load(&g_bds_sets);
    *tot_calls += calls; *tot_sets += sets;
    check_u64("c4-calls", calls, 3);
    check_u64("c4-sets", sets, 6);

    nvn_gpu_buf_release(ubB1, NULL, NULL);
    nvn_gpu_buf_release(ubB0, NULL, NULL);
    nvn_gpu_buf_release(ubA1, NULL, NULL);
    nvn_gpu_buf_release(ubA0, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fsB);
    nvn_gpu_shader_release(vsB);
    nvn_gpu_shader_release(fsA);
    nvn_gpu_shader_release(vsA);
}

// Case 5: Compute pipeline + compute push constants + compute descriptor set dedup across 3 dispatches
static void case5_compute_dedup(const char *dir, uint64_t *tot_cpipe, uint64_t *tot_cpush, uint64_t *tot_calls, uint64_t *tot_sets) {
    nvn_gpu_layout_ent ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout lay = { .ent = &ent, .nent = 1, .push_bytes = 4 };
    nvn_gpu_shader cs = load_shader(dir, "pushc.comp", NVN_GPU_SH_CS, &lay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    uint32_t zero[64] = {0};
    nvn_gpu_buf b0 = NULL, b1 = NULL;
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &b0, &err));
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &b1, &err));

    obs_reset();
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, cpipe);

    // Dispatch 1: b0, pc=100
    nvn_gpu_binding bd = {0};
    bd.ent = 0; bd.cls = NVN_GPU_RES_SSBO; bd.u.b.buf = b0; bd.u.b.off = 0; bd.u.b.len = sizeof zero;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &bd, 1);
    uint32_t pv = 100;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &pv, sizeof pv);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);

    // Dispatch 2: b1, same cpipe, same pc=100 -> skips cpipe & cpush!
    bd.u.b.buf = b1;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &bd, 1);
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &pv, sizeof pv);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    nvn_gpu_barrier(cmd, NVN_BARRIER_INVALIDATE_SHADER_BIT);

    // Dispatch 3: toggle b0->b1 (same VkDescriptorSet when memo=1), new pc=200 -> emits cpush, skips cpipe (& desc set if memo=1)!
    bd.u.b.buf = b0;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &bd, 1);
    bd.u.b.buf = b1;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &bd, 1);
    pv = 200;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &pv, sizeof pv);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    submit_and_wait(cmd);

    const uint32_t *r0 = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(b0);
    const uint32_t *r1 = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(b1);
    for (uint32_t i = 0; i < 64; i++) {
        if (r0[i] != 100u + i) { fprintf(stderr, "FAIL c5-b0[%u]: got=%u want=%u\n", i, r0[i], 100u + i); g_failures++; break; }
        if (r1[i] != 200u + i) { fprintf(stderr, "FAIL c5-b1[%u]: got=%u want=%u\n", i, r1[i], 200u + i); g_failures++; break; }
    }

    const uint64_t calls = atomic_load(&g_bds_calls);
    const uint64_t sets = atomic_load(&g_bds_sets);
    const uint64_t cpipe_n = atomic_load(&g_cpipe_binds);
    const uint64_t cpush_n = atomic_load(&g_cpush_calls);
    *tot_calls += calls; *tot_sets += sets;
    *tot_cpipe += cpipe_n; *tot_cpush += cpush_n;
    if (g_dedup_flag) {
        check_u64("c5-cpipe-dedup", cpipe_n, 1);   // 3 -> 1
        check_u64("c5-cpush-dedup", cpush_n, 2);   // 3 -> 2 (pc=100, then pc=200)
        check_u64("c5-bds-dedup", calls, g_memo_mode ? 2 : 3);
    } else {
        check_u64("c5-cpipe-base", cpipe_n, 3);
        check_u64("c5-cpush-base", cpush_n, 3);
        check_u64("c5-bds-base", calls, 3);
    }

    nvn_gpu_buf_release(b1, NULL, NULL);
    nvn_gpu_buf_release(b0, NULL, NULL);
    nvn_gpu_shader_release(cs);
}

// Case 6: clear_rect & graphics push constants properly invalidate shadows (graphics + compute interleave)
static void case6_invalidation_interleave(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe ubo_pipe = make_2set_ubo_pipe(dir, "ubo_dyn.frag", 3, &vs, &fs);

    nvn_gpu_layout play = { .push_bytes = 16 };
    nvn_gpu_shader pvs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader pfs = load_shader(dir, "pushc.frag", NVN_GPU_SH_FS, &play);
    nvn_gpu_pipe_desc ppd = {0};
    ppd.poly.mode = NVN_POLYGON_MODE_FILL;
    ppd.color.logic_op = 3;
    ppd.prim = 4; ppd.samples = 1; ppd.chmask.mask = 15;
    ppd.st[NVN_GPU_SH_VS].sh = pvs; ppd.st[NVN_GPU_SH_FS].sh = pfs;
    ppd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe push_pipe = NULL;
    GPU(nvn_gpu_pipe_get(&ppd, NVN_GPU_PIPE_SYNC, &push_pipe, &err));

    nvn_gpu_layout_ent cent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout clay = { .ent = &cent, .nent = 1, .push_bytes = 4 };
    nvn_gpu_shader cs = load_shader(dir, "pushc.comp", NVN_GPU_SH_CS, &clay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    nvn_gpu_tex rt = NULL; nvn_gpu_view rtv = NULL; nvn_gpu_buf rb = NULL;
    make_rt(4, 4, &rt, &rtv, &rb);
    nvn_gpu_buf ub0 = make_ubo_buf(1.0f, 0.0f, 0.0f, 0.0f);
    nvn_gpu_buf ub1 = make_ubo_buf(0.0f, 0.5f, 0.0f, 0.0f);
    uint32_t zero[64] = {0};
    nvn_gpu_buf ssbo = NULL;
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &ssbo, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));

    // Compute dispatch 1: pc=77
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_binding cb = {0};
    cb.ent = 0; cb.cls = NVN_GPU_RES_SSBO; cb.u.b.buf = ssbo; cb.u.b.off = 0; cb.u.b.len = sizeof zero;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
    uint32_t cv = 77;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &cv, sizeof cv);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);

    // Render pass: Draw 1 (UBO) -> clear_rect (internal pipe/push!) -> Draw 2 (same UBO pipe!) -> Draw 3 (gfx push!)
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1; p.color[0].view = rtv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, ubo_pipe);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

    set_scissor_dyn(cmd, 4, 4, 0, 0, 2, 2);
    nvn_gpu_binding gb[2] = {{0}};
    gb[0].ent = 0; gb[0].cls = NVN_GPU_RES_UBO; gb[0].u.b.buf = ub0; gb[0].u.b.off = 0; gb[0].u.b.len = 16;
    gb[1].ent = 1; gb[1].cls = NVN_GPU_RES_UBO; gb[1].u.b.buf = ub1; gb[1].u.b.off = 0; gb[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, gb, 2);
    nvn_gpu_draw(cmd, &da);

    // clear_rect over entire RT to blue (0, 0, 1, 1) -> must invalidate graphics shadows!
    nvn_gpu_clear cl = {0};
    cl.color = 0; cl.color_mask = 0xf; cl.x = 0; cl.y = 0; cl.w = 4; cl.h = 4;
    cl.value.f[2] = 1.0f; cl.value.f[3] = 1.0f;
    nvn_gpu_clear_rect(cmd, &cl);

    // Draw 2 after clear_rect: same ubo_pipe & same bindings -> MUST re-bind properly to (255, 128, 0, 255)!
    set_scissor_dyn(cmd, 4, 4, 0, 0, 2, 2);
    nvn_gpu_draw(cmd, &da);

    // Draw 3: graphics push constants (0.25, 0.75, 0.5, 1.0) -> must invalidate compute push shadow!
    nvn_gpu_set_pipe(cmd, push_pipe);
    set_scissor_dyn(cmd, 4, 4, 2, 0, 2, 2);
    const float gcol[4] = {0.25f, 0.75f, 0.5f, 1.0f};
    nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, gcol, sizeof gcol);
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);

    // Compute dispatch 2 with SAME cv=77 after graphics pass + graphics push: must re-emit compute push cleanly!
    nvn_gpu_barrier(cmd, NVN_BARRIER_INVALIDATE_SHADER_BIT);
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &cv, sizeof cv);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);

    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    check_rgba("c6-after-clear", px + (0 * 4 + 0) * 4, 255, 128, 0, 255, 2);
    check_rgba("c6-gfx-push", px + (0 * 4 + 2) * 4, 64, 191, 128, 255, 2);
    check_rgba("c6-clear-bg", px + (2 * 4 + 2) * 4, 0, 0, 255, 255, 2);
    const uint32_t *out = (const uint32_t *)(const void *)nvn_gpu_buf_cpu(ssbo);
    for (uint32_t i = 0; i < 64; i++) {
        if (out[i] != 77u + i) { fprintf(stderr, "FAIL c6-cs[%u]: got=%u want=%u\n", i, out[i], 77u + i); g_failures++; break; }
    }

    nvn_gpu_buf_release(ssbo, NULL, NULL);
    nvn_gpu_buf_release(ub1, NULL, NULL);
    nvn_gpu_buf_release(ub0, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(cs);
    nvn_gpu_shader_release(pfs);
    nvn_gpu_shader_release(pvs);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 5);
    const char *dir = argv[1];
    g_dq_mode = atoi(argv[2]);
    g_memo_mode = atoi(argv[3]);
    g_dedup_flag = atoi(argv[4]);

    struct s2a_vk_device dev = {0};
    char why[256] = {0};
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = { .host_device = &dev };
    GPU(nvn_gpu_open(&od, &err));

    uint64_t tot_calls = 0, tot_sets = 0, tot_cpipe = 0, tot_cpush = 0;
    case1_partial_set_range(dir, &tot_calls, &tot_sets);
    case2_dyn_offset_partial_and_skip(dir, &tot_calls, &tot_sets);
    case3_memo_hit_full_skip(dir, &tot_calls, &tot_sets);
    case4_layout_switch(dir, &tot_calls, &tot_sets);
    case5_compute_dedup(dir, &tot_cpipe, &tot_cpush, &tot_calls, &tot_sets);
    case6_invalidation_interleave(dir);

    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);

    printf("bind-dedup dq=%d memo=%d flag=%d bds_calls=%llu bds_sets=%llu cpipe=%llu cpush=%llu cases=6 failures=%u\n",
           g_dq_mode, g_memo_mode, g_dedup_flag,
           (unsigned long long)tot_calls, (unsigned long long)tot_sets,
           (unsigned long long)tot_cpipe, (unsigned long long)tot_cpush, g_failures);
    return g_failures ? 1 : 0;
}
