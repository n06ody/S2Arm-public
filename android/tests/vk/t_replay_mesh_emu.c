// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 端到端宿主 Vulkan 1.1 任务 / 网格计算模拟夹具（对齐 iOS graphics/nvn/tests/t_nvn_mesh.m 与 t_nvn_mesh_emu.m）：
//   真实回放 rp_draw.c（含 rp_mesh.inc）+ 真实缓冲解析 rp_buf.c + 真实状态 rp_state.c + 真实 Vulkan 后端 backend_vulkan.c（含 backend_vulkan_mesh_emu.inc）。
//   覆盖：
//     1) 直接 mesh-only（first=2, count=3 → 8x4 目标列掩码 0x1c）；
//     2) 直接 task+mesh（first=1, count=2，每任务派生 2 网格组 → 列掩码 0x3c）；
//     3) GPU 写出间接参数 mesh-only（非 256 对齐偏移 272，CPU 内存投毒 {7,0}，GPU 写 {3,4} → 列掩码 0x70）；
//     4) GPU 写出间接参数 task+mesh（偏移 276，CPU 投毒 {7,0}，GPU 写 {2,1} → 列掩码 0x3c）；
//     5) 间接 0 工作量 {0,1} → 列掩码 0x00、0 错误 0 跳过；
//     6) 后端辅助计算核直接校验：mxe_tclamp / mxe_monly 夹限 + mxe_scan n=0/1/127/128/129/257/4097 与 CPU 串行真值逐字对账；
//     7) 超限额与退化负对照（max_vertices / max_primitives / mesh_threads / payload / task_missing / own_shared_spill / rast_discard）各出 0 像素 + 精确具名记账 + 红日志；
//     8) 撤改负对照（CLI 传 neg，用 --wrap=nvn_gpu_caps_get 抹掉 NVN_GPU_CAP_MESH_EMU）→ 0 像素 + mesh_emu_unsupported。
#include "rp_exec.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdalign.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TW = 8, TH = 4 };
static nvn_gpu_err err;
static unsigned g_failures;
static int g_neg_strip_cap;
static int g_dq_flag;
static int g_red_logs;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "【红】断言失败 %s:%d: %s (rc=%d why=%s)\n", __FILE__, __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define CHECK(x, fmt, ...) do { if (!(x)) { g_failures++; fprintf(stderr, "【红】%s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

void __real_nvn_gpu_caps_get(nvn_gpu_caps *out);
void __wrap_nvn_gpu_caps_get(nvn_gpu_caps *out) {
    __real_nvn_gpu_caps_get(out);
    if (g_neg_strip_cap) out->caps &= ~(uint32_t)NVN_GPU_CAP_MESH_EMU;
}

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_flag;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    if (l == NVN_PLAT_LOG_RED) g_red_logs++;
    fprintf(stderr, "[%s:%d] %s\n", c ? c : "", l, s ? s : "");
}

static void on_done(void *u, const nvn_gpu_done *d) {
    (void)u;
    if (d->status == NVN_GPU_DONE_FAILED) {
        g_failures++;
        fprintf(stderr, "【红】GPU 段执行失败\n");
    }
}

void rp_buf_reset(void);

static nvn_gpu_buf make_buf(uint64_t bytes, const void *data) {
    nvn_gpu_buf b = NULL;
    GPU(nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, data, &b, &err));
    return b;
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
    nvn_gpu_shader_src s = {0};
    s.stage = stage; s.kind = NVN_GPU_BLOB_SPIRV; s.entry = "main";
    s.blob = data; s.bytes = (size_t)n; s.layout = layout;
    nvn_gpu_shader h = NULL;
    GPU(nvn_gpu_shader_load(&s, &h, &err));
    free(data);
    return h;
}

// 受控回放上下文替身（rp_draw / rp_mesh / rp_buf / rp_state / backend_vulkan 全部为生产真代码）
int g_s2a_logfd = 2, s2a_nvn_rast_flip = 0;
static alignas(4096) unsigned char g_guest[4096];
static struct rp_mesh_prog g_mprog;
static p_set_rt g_target;
static tex_t g_target_tex;
static nvn_gpu_view g_target_view;
static nvn_gpu_cmd g_active;
static uint64_t g_serial = 1;
static int g_pass_open;
static uint32_t g_pending_bar;

uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int t) { (void)p; (void)t; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t s, uint8_t k[6][8], uint8_t *o, uint8_t t[6]) { (void)s; (void)k; (void)o; (void)t; return NULL; }
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **o) { (void)q; *o = NULL; return -1; }
int rp_shader_resolve_compute(const struct rp_prog **o) { *o = NULL; return -1; }
int rp_shader_resolve_mesh(const struct rp_shader_req *q, const struct rp_mesh_prog **o) { (void)q; *o = &g_mprog; return 0; }
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
uint64_t rp_frame(void) { return 1; }
void rp_perf_add(int i, uint64_t n) { (void)i; (void)n; }
uint64_t rp_seg_serial(void) { return g_serial; }
uint64_t rp_sub_serial(void) { return g_serial; }
uint64_t rp_sub_fence(void) { return g_serial; }
int rp_fence_done(uint64_t f) { (void)f; return 1; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last) { rp_pass_flush(g_active); *last = g_serial; rp_gpu_wr_gen++; return g_active; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) {
    uint64_t base = (uint64_t)(uintptr_t)g_guest;
    if (a < base || n > sizeof g_guest || a - base > sizeof g_guest - n) return 0;
    if (m) *m = g_guest;
    if (s) *s = sizeof g_guest;
    return 1;
}
size_t nvn_dirty_page_size(void) { return 4096; }
uint64_t nvn_dirty_now(void) { return 1; }
uint64_t nvn_dirty_mapping_generation(void) { return 1; }
int nvn_dirty_tracking_unreliable(void) { return 1; }
const void *nvn_dirty_span(const void *a, size_t n) { (void)a; (void)n; return NULL; }
long nvn_dirty_page_gens(const void *s, const void *a, size_t l, uint64_t *g, size_t n) { (void)s; (void)a; (void)l; (void)g; (void)n; return -1; }
void nvn_dirty_protect(const void *a, size_t n) { (void)a; (void)n; }
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
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
    (void)c; (void)samp; (void)base; (void)n; (void)dtt; (void)dcls; if (why) *why = "pool_table_unresolved"; return NULL;
}
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
nvn_gpu_samp rp_samp_by_id(int i) { (void)i; return NULL; }
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)sid; (void)tid; return NULL; }
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) { (void)sid; (void)tid; (void)pb; (void)pn; return NULL; }
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int i, tex_t *t, view_t *v, int *h) { (void)i; (void)t; (void)v; (void)h; return 0; }
uint64_t rp_tex_reg_gen(void) { return 1; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int c) { (void)h; (void)n; (void)b; (void)c; return 0; }
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; }
int rp_tex_debug_state(uint64_t s) { (void)s; return 0; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s) { (void)s; return NULL; }
const p_set_rt *rp_pass_target(void) { return &g_target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = TW; *h = TH; }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { (void)w; (void)h; }
void rp_pass_note_draw(int d) { (void)d; }
int rp_pass_parked(void) { return 0; }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t m) { (void)c; (void)m; }
void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *ts, int n) { (void)c; (void)ts; (void)n; }
uint32_t rp_pass_take_barrier(void) { uint32_t b = g_pending_bar; g_pending_bar = 0; return b; }
int rp_pass_is_open(void) { return g_pass_open; }
int rp_pass_begin(nvn_gpu_cmd c) {
    if (g_pass_open) return 1;
    nvn_gpu_pass_desc p = {0};
    p.ncolor = 1;
    p.color[0].view = g_target_view;
    p.color[0].load = NVN_GPU_LOAD_LOAD;
    p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = TW; p.h = TH; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    g_pass_open = 1;
    return 1;
}
void rp_pass_flush(nvn_gpu_cmd c) {
    if (g_pass_open) { nvn_gpu_pass_end(c); g_pass_open = 0; }
}

static uint32_t col_mask_from_pixels(const uint8_t *px, int *uniform_ok) {
    uint32_t mask = 0;
    *uniform_ok = 1;
    for (int x = 0; x < TW; x++) {
        const uint8_t *p0 = px + x * 4;
        int white = (p0[0] >= 250 && p0[1] >= 250 && p0[2] >= 250 && p0[3] >= 250);
        int black = (p0[0] == 0 && p0[1] == 0 && p0[2] == 0 && p0[3] == 255);
        if (!white && !black) *uniform_ok = 0;
        for (int y = 1; y < TH; y++) {
            const uint8_t *py = px + (y * TW + x) * 4;
            if (memcmp(p0, py, 4)) *uniform_ok = 0;
        }
        if (white) mask |= 1u << x;
    }
    return mask;
}

static uint32_t run_draw_case(nvn_gpu_tex rt_tex, nvn_gpu_pipe ind_pipe, uint64_t *sig,
                              int indirect, uint32_t ind_byte_off, uint32_t first, uint32_t count) {
    nvn_gpu_buf rb = make_buf(TW * TH * 4, NULL);
    GPU(nvn_gpu_cmd_begin(NULL, &g_active, &err));
    g_serial++;
    g_pass_open = 0;
    float clear_black[4] = {0.f, 0.f, 0.f, 1.f};
    nvn_gpu_clear_view(g_active, g_target_view, clear_black, NULL, 0);

    p_draw dr = {0};
    dr.prim = DM_PRIM_MESH_TASKS;
    if (indirect) {
        // CPU 内存故意投毒 {7, 0}，验证间接路径真读的是 GPU 写出的 {count, first} 而非 CPU 旧值
        uint32_t *gwords = (uint32_t *)(void *)g_guest;
        gwords[ind_byte_off / 4u + 0u] = 7u;
        gwords[ind_byte_off / 4u + 1u] = 0u;
        nvn_gpu_buf mirror = NULL;
        uint64_t moff = 0;
        REQUIRE(rp_buf_resolve(g_active, (uint64_t)(uintptr_t)g_guest, 512u, &mirror, &moff));
        uint64_t pkey = 0;
        REQUIRE(rp_buf_gpu_write_note((uint64_t)(uintptr_t)g_guest, 512u, &pkey) != 0);
        nvn_gpu_binding sb = {0};
        sb.cls = NVN_GPU_RES_SSBO;
        sb.u.b.buf = mirror; sb.u.b.off = moff; sb.u.b.len = 512u;
        nvn_gpu_set_pipe(g_active, ind_pipe);
        nvn_gpu_bind(g_active, NVN_GPU_SH_CS, &sb, 1);
        uint32_t pc[3] = { ind_byte_off / 4u, count, first };
        nvn_gpu_push(g_active, NVN_GPU_SH_CS, 0, pc, sizeof pc);
        nvn_gpu_dispatch(g_active, ind_pipe, 1, 1, 1, NULL, 0);
        dr.ind = (uint64_t)(uintptr_t)(g_guest + ind_byte_off);
    } else {
        dr.first = (int32_t)first;
        dr.count = (int32_t)count;
    }

    rp_draw(g_active, &dr, NULL, &dr);
    rp_pass_flush(g_active);
    nvn_gpu_copy_tex_buf(g_active, rt_tex, 0, 0, NULL, rb, 0, TW * 4, TW * TH * 4);
    *sig += 1;
    GPU(nvn_gpu_cmd_commit(g_active, *sig, on_done, NULL, &err));
    GPU(nvn_gpu_fence_wait(*sig, 5000000000ull, &err));

    int uniform_ok = 0;
    uint32_t mask = col_mask_from_pixels(nvn_gpu_buf_cpu(rb), &uniform_ok);
    CHECK(uniform_ok, "列内像素不均匀或出现非黑非白残色 (mask=0x%02x)", mask);
    nvn_gpu_buf_release(rb, NULL, NULL);
    return mask;
}

// 后端辅助计算核直接对照（mxe_tclamp / mxe_monly / mxe_scan，逐字对比 CPU 串行真值，覆盖 128 线程边界与 4097 超限）
static void test_helper_kernels(uint64_t *sig) {
    // 1) mxe_tclamp：非 256 对齐间接偏移 260，{100000, 5} 夹到 capT=64
    {
        nvn_gpu_buf xb = make_buf(4096, NULL);
        nvn_gpu_buf ib = make_buf(512, NULL);
        uint32_t *h = (uint32_t *)(void *)nvn_gpu_buf_cpu(xb);
        memset(h, 0, 4096);
        h[3] = 64u;
        h[30] = 64u;   // first_aligned_off = 64 字 = 256 字节
        uint32_t *iw = (uint32_t *)(void *)nvn_gpu_buf_cpu(ib);
        memset(iw, 0, 512);
        iw[260u / 4u + 0u] = 100000u;
        iw[260u / 4u + 1u] = 5u;

        nvn_gpu_cmd c = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &c, &err));
        nvn_gpu_mesh_emu_op(c, NVN_GPU_MESH_EMU_TCLAMP, xb, 0, 4096, ib, 260u);
        *sig += 1;
        GPU(nvn_gpu_cmd_commit(c, *sig, on_done, NULL, &err));
        GPU(nvn_gpu_fence_wait(*sig, 5000000000ull, &err));
        CHECK(h[20] == 64u && h[21] == 1u && h[22] == 1u, "tclamp 夹限错: %u,%u,%u", h[20], h[21], h[22]);
        CHECK(h[29] == 5u && h[64] == 5u, "tclamp first 错: h[29]=%u h[64]=%u", h[29], h[64]);
        nvn_gpu_buf_release(xb, NULL, NULL);
        nvn_gpu_buf_release(ib, NULL, NULL);
    }

    // 2) mxe_monly：非 256 对齐偏移 268，{100000, 2} 夹到 capM=64
    {
        nvn_gpu_buf xb = make_buf(4096, NULL);
        nvn_gpu_buf ib = make_buf(512, NULL);
        uint32_t *h = (uint32_t *)(void *)nvn_gpu_buf_cpu(xb);
        memset(h, 0, 4096);
        h[4] = 64u;
        h[30] = 64u;
        uint32_t *iw = (uint32_t *)(void *)nvn_gpu_buf_cpu(ib);
        memset(iw, 0, 512);
        iw[268u / 4u + 0u] = 100000u;
        iw[268u / 4u + 1u] = 2u;

        nvn_gpu_cmd c = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &c, &err));
        nvn_gpu_mesh_emu_op(c, NVN_GPU_MESH_EMU_MONLY, xb, 0, 4096, ib, 268u);
        *sig += 1;
        GPU(nvn_gpu_cmd_commit(c, *sig, on_done, NULL, &err));
        GPU(nvn_gpu_fence_wait(*sig, 5000000000ull, &err));
        CHECK(h[8] == 64u && h[9] == 1u && h[10] == 1u, "monly 夹限错: %u,%u,%u", h[8], h[9], h[10]);
        CHECK(h[13] == 0u && h[14] == 1u && h[15] == 0u && h[16] == 0u && h[17] == 0u, "monly draw args 错");
        CHECK(h[18] == 100000u && h[19] == 0u && h[29] == 2u && h[64] == 2u, "monly raw/first 错");
        nvn_gpu_buf_release(xb, NULL, NULL);
        nvn_gpu_buf_release(ib, NULL, NULL);
    }

    // 3) mxe_scan：n = 0, 1, 127, 128, 129, 257, 4097（跨 128 线程工作组边界与超 capT=4096，含单项 >1024 夹限）
    const uint32_t ns[] = { 0u, 1u, 127u, 128u, 129u, 257u, 4097u };
    for (size_t ci = 0; ci < sizeof ns / sizeof ns[0]; ci++) {
        const uint32_t raw_n = ns[ci];
        const uint32_t capT = 4096u;
        const uint32_t capM = 1500u;
        const uint32_t counts_off = 32u;
        const uint32_t table_off = counts_off + capT;
        const uint64_t total_bytes = (uint64_t)(table_off + capM) * 4ull;
        nvn_gpu_buf xb = make_buf(total_bytes, NULL);
        uint32_t *h = (uint32_t *)(void *)nvn_gpu_buf_cpu(xb);
        memset(h, 0, (size_t)total_bytes);
        h[0] = counts_off;
        h[2] = table_off;
        h[3] = capT;
        h[4] = capM;
        h[6] = raw_n;

        // CPU 串行真值
        const uint32_t eff_n = raw_n < capT ? raw_n : capT;
        uint32_t *exp_tab = calloc(capM, sizeof(uint32_t));
        REQUIRE(exp_tab);
        uint32_t acc = 0u;
        for (uint32_t t = 0; t < eff_n; t++) {
            uint32_t raw_c = (t == 5u) ? 2000u : ((t * 7u + 3u) % 5u);
            h[counts_off + t] = raw_c;
            uint32_t c = raw_c > 1024u ? 1024u : raw_c;
            for (uint32_t j = 0; j < c && acc + j < capM; j++)
                exp_tab[acc + j] = (t << 10) | j;
            acc += c;
        }
        uint32_t exp_m = acc < capM ? acc : capM;

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_mesh_emu_op(cmd, NVN_GPU_MESH_EMU_SCAN, xb, 0, total_bytes, NULL, 0);
        *sig += 1;
        GPU(nvn_gpu_cmd_commit(cmd, *sig, on_done, NULL, &err));
        GPU(nvn_gpu_fence_wait(*sig, 5000000000ull, &err));

        CHECK(h[8] == exp_m && h[9] == 1u && h[10] == 1u, "scan n=%u h[8..10]=%u,%u,%u 期望 %u,1,1", raw_n, h[8], h[9], h[10], exp_m);
        CHECK(h[13] == 0u && h[14] == 1u && h[15] == 0u && h[16] == 0u && h[17] == 0u, "scan n=%u draw args 错", raw_n);
        CHECK(h[18] == acc && h[19] == raw_n, "scan n=%u raw=%u,%u 期望 %u,%u", raw_n, h[18], h[19], acc, raw_n);
        int mismatches = 0;
        for (uint32_t k = 0; k < exp_m; k++) {
            if (h[table_off + k] != exp_tab[k]) {
                if (mismatches++ < 3)
                    fprintf(stderr, "  scan n=%u tab[%u]=%#x 期望 %#x\n", raw_n, k, h[table_off + k], exp_tab[k]);
            }
        }
        CHECK(mismatches == 0, "scan n=%u 排表不符 %d 处", raw_n, mismatches);
        free(exp_tab);
        nvn_gpu_buf_release(xb, NULL, NULL);
    }
    printf("mesh-emu helper-kernels: ok (tclamp + monly + scan x7)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <spv_dir> [dq=0|1] [neg]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    g_dq_flag = argc > 2 && !strcmp(argv[2], "1");
    g_neg_strip_cap = argc > 3 && !strcmp(argv[3], "neg");

    struct s2a_vk_device dev;
    char why[256] = {0};
    if (s2a_vk_device_create(&dev, why, sizeof why)) {
        fprintf(stderr, "device create failed: %s\n", why);
        return 2;
    }
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "replay-mesh-emu";
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_caps caps = {0};
    nvn_gpu_caps_get(&caps);
    if (g_neg_strip_cap) {
        CHECK(!(caps.caps & NVN_GPU_CAP_MESH_EMU), "neg 模式应已抹掉 NVN_GPU_CAP_MESH_EMU");
    } else {
        CHECK((caps.caps & NVN_GPU_CAP_MESH_EMU) != 0, "Vulkan 1.1 后端未置 NVN_GPU_CAP_MESH_EMU");
    }

    // 加载 4 个模拟着色器 + 1 个 GPU 间接参数写出核
    nvn_gpu_layout_ent cs_ents[2] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .src_kind = NVN_RB_MESH_EMU, .count = 1, .access = NVN_GPU_ACC_RW, .stages = 1u << NVN_GPU_SH_CS },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_UBO,  .src_kind = NVN_RB_MESH_FIRST, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_CS },
    };
    nvn_gpu_layout cs_lay = { .ent = cs_ents, .nent = 2 };
    nvn_gpu_shader sh_task = load_shader(dir, "mesh_emu_task", NVN_GPU_SH_CS, &cs_lay);
    nvn_gpu_shader sh_mesh = load_shader(dir, "mesh_emu_mesh", NVN_GPU_SH_CS, &cs_lay);

    nvn_gpu_layout_ent mv_ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .src_kind = NVN_RB_MESH_VBO, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_VS };
    nvn_gpu_layout mv_lay = { .ent = &mv_ent, .nent = 1 };
    nvn_gpu_shader sh_mv = load_shader(dir, "mesh_emu_mv", NVN_GPU_SH_VS, &mv_lay);
    nvn_gpu_shader sh_fs = load_shader(dir, "mesh_emu_fs", NVN_GPU_SH_FS, NULL);

    nvn_gpu_layout_ent ind_ent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .count = 1, .access = NVN_GPU_ACC_RW, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout ind_lay = { .ent = &ind_ent, .nent = 1, .push_bytes = 12 };
    nvn_gpu_shader sh_ind = load_shader(dir, "mesh_emu_ind_write", NVN_GPU_SH_CS, &ind_lay);
    nvn_gpu_stage_desc ind_sd = { .sh = sh_ind };
    nvn_gpu_pipe ind_pipe = NULL;
    GPU(nvn_gpu_cpipe_get(&ind_sd, NVN_GPU_PIPE_SYNC, &ind_pipe, &err));

    // 8x4 RGBA8 渲染目标
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = TW; td.d.height = TH; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex rt_tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt_tex, &err));
    GPU(nvn_gpu_view_create(rt_tex, NULL, &g_target_view, &err));
    memset(&g_target_tex, 0, sizeof g_target_tex);
    g_target_tex.d = td.d; g_target_tex.serial = 1;
    memset(&g_target, 0, sizeof g_target);
    g_target.n = 1; g_target.color[0] = &g_target_tex;

    // 组装 rp_mesh_prog
    memset(&g_mprog, 0, sizeof g_mprog);
    g_mprog.task_cs.sh[0] = sh_task; g_mprog.task_cs.is_compute = 1; g_mprog.task_cs.nent[0] = 2;
    memcpy(g_mprog.task_cs.ent[0], cs_ents, sizeof cs_ents);
    g_mprog.mesh_cs.sh[0] = sh_mesh; g_mprog.mesh_cs.is_compute = 1; g_mprog.mesh_cs.nent[0] = 2;
    memcpy(g_mprog.mesh_cs.ent[0], cs_ents, sizeof cs_ents);
    g_mprog.raster.sh[0] = sh_mv; g_mprog.raster.sh[1] = sh_fs;
    g_mprog.raster.nent[0] = 1; g_mprog.raster.ent[0][0] = mv_ent;
    g_mprog.raster.color_out_mask = 0xfu;
    g_mprog.task_threads = 1; g_mprog.mesh_threads = 1;
    g_mprog.max_vertices = 4; g_mprog.max_primitives = 2;
    g_mprog.task_payload_bytes = 16; g_mprog.vertex_stride = 16;

    rp_state_reset(); rp_draw_reset(); nvn_unimpl_reset();
    rp_S.color.logic_op = NVN_LOGIC_OP_COPY;
    rp_S.chmask.mask = 0xfu;
    rp_S.poly.mode = NVN_POLYGON_MODE_FILL;
    rp_S.poly.cull = NVN_FACE_NONE;

    uint64_t sig = 0;
    if (g_neg_strip_cap) {
        g_mprog.mesh_wants_task = 0;
        g_red_logs = 0;
        uint32_t m = run_draw_case(rt_tex, ind_pipe, &sig, 0, 0, 2, 3);
        CHECK(m == 0x00u, "neg 模式不应画出像素，实得 0x%02x", m);
        CHECK(nvn_unimpl_count("mesh", "mesh_emu_unsupported") == 1, "neg 模式应精确记 mesh_emu_unsupported=1");
        CHECK(g_red_logs > 0, "neg 模式必须输出红日志");
        printf("mesh-emu neg(dq=%d): mask=0x%02x unimpl=%llu failures=%u\n",
               g_dq_flag, m, (unsigned long long)nvn_unimpl_count("mesh", "mesh_emu_unsupported"), g_failures);
    } else {
        test_helper_kernels(&sig);

        // Case 1: 直接 mesh-only first=2, count=3 → 0x1c
        nvn_unimpl_reset();
        g_mprog.mesh_wants_task = 0;
        uint32_t m1 = run_draw_case(rt_tex, ind_pipe, &sig, 0, 0, 2, 3);
        CHECK(m1 == 0x1cu, "Case 1 direct mesh-only: 实得 0x%02x 期望 0x1c", m1);

        // Case 2: 直接 task+mesh first=1, count=2 → 0x3c
        g_mprog.mesh_wants_task = 1;
        uint32_t m2 = run_draw_case(rt_tex, ind_pipe, &sig, 0, 0, 1, 2);
        CHECK(m2 == 0x3cu, "Case 2 direct task+mesh: 实得 0x%02x 期望 0x3c", m2);

        // Case 3: 间接 GPU 写出 mesh-only（非 256 对齐偏移 272，GPU 写 {3,4}）→ 0x70
        g_mprog.mesh_wants_task = 0;
        uint32_t m3 = run_draw_case(rt_tex, ind_pipe, &sig, 1, 272u, 4, 3);
        CHECK(m3 == 0x70u, "Case 3 indirect mesh-only: 实得 0x%02x 期望 0x70", m3);

        // Case 4: 间接 GPU 写出 task+mesh（非 256 对齐偏移 276，GPU 写 {2,1}）→ 0x3c
        g_mprog.mesh_wants_task = 1;
        uint32_t m4 = run_draw_case(rt_tex, ind_pipe, &sig, 1, 276u, 1, 2);
        CHECK(m4 == 0x3cu, "Case 4 indirect task+mesh: 实得 0x%02x 期望 0x3c", m4);

        // Case 5: 间接 0 工作量 {0,1} → 0x00
        uint32_t m5 = run_draw_case(rt_tex, ind_pipe, &sig, 1, 280u, 1, 0);
        CHECK(m5 == 0x00u, "Case 5 indirect zero: 实得 0x%02x 期望 0x00", m5);

        uint64_t draws_ok = 0, skips_ok = 0;
        rp_draw_stats(&draws_ok, &skips_ok);
        CHECK(draws_ok == 5 && skips_ok == 0, "正向 5 用例应画 5 跳过 0，实得 draws=%llu skips=%llu",
              (unsigned long long)draws_ok, (unsigned long long)skips_ok);

        // Case 7: 超限额与退化负对照（每项必须 0 像素 + 精确具名记账 + 红日志）
        const struct {
            const char *why;
            uint32_t mv, mp, mt, pay;
            uint8_t spill, want_t, rast_disc;
            int no_t;
        } negs[] = {
            { "mesh_max_vertices_over_limit",   300, 2,   1,    16,    0, 0, 0, 0 },
            { "mesh_max_primitives_over_limit", 4,   600, 1,    16,    0, 0, 0, 0 },
            { "mesh_threads_over_limit",        4,   2,   2048, 16,    0, 0, 0, 0 },
            { "mesh_payload_over_limit",        4,   2,   1,    20000, 0, 1, 0, 0 },
            { "mesh_task_shader_missing",       4,   2,   1,    16,    0, 1, 0, 1 },
            { "mesh_own_shared_unsupported",    4,   2,   1,    16,    1, 0, 0, 0 },
            { "mesh_rasterizer_discard",        4,   2,   1,    16,    0, 0, 1, 0 },
        };
        for (size_t i = 0; i < sizeof negs / sizeof negs[0]; i++) {
            nvn_unimpl_reset();
            g_red_logs = 0;
            g_mprog.max_vertices = negs[i].mv;
            g_mprog.max_primitives = negs[i].mp;
            g_mprog.mesh_threads = negs[i].mt;
            g_mprog.task_payload_bytes = negs[i].pay;
            g_mprog.own_shared_spill = negs[i].spill;
            g_mprog.mesh_wants_task = negs[i].want_t;
            g_mprog.task_cs.sh[0] = negs[i].no_t ? NULL : sh_task;
            rp_S.rast_discard = negs[i].rast_disc;
            uint32_t mn = run_draw_case(rt_tex, ind_pipe, &sig, 0, 0, 0, 2);
            CHECK(mn == 0x00u, "负对照 %s 不应画出像素，实得 0x%02x", negs[i].why, mn);
            CHECK(nvn_unimpl_count("mesh", negs[i].why) == 1, "负对照 %s 记账错 (%llu)",
                  negs[i].why, (unsigned long long)nvn_unimpl_count("mesh", negs[i].why));
            CHECK(g_red_logs > 0, "负对照 %s 必须输出红日志", negs[i].why);
        }
        rp_S.rast_discard = 0;
        printf("mesh-emu pass(dq=%d): direct_m=0x%02x direct_tm=0x%02x ind_m=0x%02x ind_tm=0x%02x ind_zero=0x%02x negs=%zu failures=%u\n",
               g_dq_flag, m1, m2, m3, m4, m5, sizeof negs / sizeof negs[0], g_failures);
    }

    rp_draw_reset();
    rp_buf_reset();
    nvn_gpu_view_release(g_target_view);
    nvn_gpu_tex_release(rt_tex);
    nvn_gpu_shader_release(sh_ind);
    nvn_gpu_shader_release(sh_fs);
    nvn_gpu_shader_release(sh_mv);
    nvn_gpu_shader_release(sh_mesh);
    nvn_gpu_shader_release(sh_task);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&dev, 1));
    return g_failures ? 1 : 0;
}
