// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真回放 rp_dispatch + 池镜像 + 真 Vulkan 后端：图像槽里的缓冲贴图（imageBuffer，布局类别 IMAGE_TBUF）读写，lavapipe + VVL（含同步校验）。
// 期望值按 GLSL 语义手算：rmw 每元素 v → 3v + 1；atomic 每元素 v → v + 5。元素前后各 16 字节护栏不得被改（对齐副本拷回只写这 16 字节）。
// 臂：aligned（池偏移 256，满足 minTexelBufferOffsetAlignment，原地视图）/ unaligned（偏移 516 = 4 的倍数、非对齐，走对齐副本 + 拷回）/
//     atomic（偏移 772，非对齐，图像原子走同一副本；着色器装载时核 STORAGE_TEXEL_BUFFER_ATOMIC）。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "nvn_unimpl.h"
#include "nvn_dirty.h"

static p_set_rt target; static tex_t buffer_tex; static pool_t pool;
static nvn_gpu_view target_view;
static struct rp_prog progs[3], prog; static int cur_prog;   // 每臂一个程序对象：回放按程序对象缓存计算管线，同一对象换着色器会沿用旧管线
int rp_shader_resolve_compute(const struct rp_prog **out) { *out = &progs[cur_prog]; return 0; }
static _Alignas(4096) uint8_t guest[4096];
static nvn_gpu_cmd active_cmd; static int pass_open, xfer_after_pass; static uint64_t g_fence_no;
int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) { (void)serial; (void)keys; (void)ok; (void)stage; return NULL; }
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) { (void)q; *out = &prog; return 0; }
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_frame(void) { return 1; } // 两笔在同一帧。
uint64_t rp_seg_serial(void) { return 1; }
uint64_t rp_sub_serial(void) { return 1; }
uint64_t rp_sub_fence(void) { return 1; }
int rp_fence_done(uint64_t f) { return f == 0; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) {
    uint64_t start = (uint64_t)(uintptr_t)guest;
    if (a < start || n > sizeof guest || a - start > sizeof guest - n) return 0;
    if (m) *m = guest;
    if (s) *s = sizeof guest;
    return 1;
}
size_t nvn_dirty_page_size(void) { return 4096; }
uint64_t nvn_dirty_mapping_generation(void) { return 1; }
int nvn_dirty_tracking_unreliable(void) { return 0; }
uint64_t nvn_dirty_now(void) { return 1; }
const void *nvn_dirty_span(const void *a, size_t n) { (void)a; (void)n; return NULL; }
long nvn_dirty_page_gens(const void *s, const void *a, size_t n, uint64_t *g, size_t cap) { (void)s; (void)a; (void)n; (void)g; (void)cap; return -1; }
void nvn_dirty_protect(const void *a, size_t n) { (void)a; (void)n; }
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, tex_t *t) { (void)id; (void)c; if (t) memset(t, 0, sizeof *t); return NULL; }
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) { (void)t; (void)v; (void)c; return NULL; }
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t tt, uint8_t cls, int elem, nvn_gpu_cmd c, tex_t *t) { (void)id; (void)tt; (void)cls; (void)elem; (void)c; if (t) memset(t, 0, sizeof *t); return NULL; }
nvn_gpu_view rp_view_by_ubo_handle_decl(int i,uint8_t t,uint8_t k,nvn_gpu_cmd c,tex_t *o){return rp_view_by_id_decl(i,t,k,0,c,o);}   // 夹具不另判「没登记」：同 rp_view_by_id_decl 替身
void rp_decl_probe_begin(void){} int rp_decl_probe_end(void){return 0;}
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c,int samp,int base,uint32_t n,uint8_t dtt,uint8_t dcls,const char **why){(void)c;(void)samp;(void)base;(void)n;(void)dtt;(void)dcls;if(why)*why="pool_table_unresolved";return NULL;}   // 本夹具不出整池数组
nvn_gpu_samp rp_samp_by_id(int id) { (void)id; return NULL; }
// 这些隔离夹具只发 TBUF，不提供普通采样器；格式能力仍查真实后端。
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) { (void)pb; (void)pn; return rp_samp_for_tex(sid, tid); }   // 同 rp_samp_for_tex：替身不给默认采样器
// 夹具不提供查询采样器；请求该能力时返回不可用。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) {
    if (id != 7) return 0;
    if (t) *t = buffer_tex;
    if (v) memset(v, 0, sizeof *v);
    if (h) *h = 0;
    return 1;
}
uint64_t rp_tex_reg_gen(void) { return 1; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int count) { (void)h; (void)n; (void)b; return (uint64_t)count; }
void rp_tex_mark_gpu_written(nvn_gpu_cmd c, const tex_t *t) { (void)c; (void)t; }
int rp_tex_debug_state(uint64_t s) { (void)s; return 0; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s) { (void)s; return NULL; }
const p_set_rt *rp_pass_target(void) { return &target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = *h = 4; }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { (void)w; (void)h; } // 有颜色附件，不改尺寸。
void rp_pass_note_draw(int d) { (void)d; }
// These fixtures issue no deferred clears and leave pass-area optimization off.
int rp_pass_parked(void) { return 0; }
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *t, int n) { (void)c; (void)t; (void)n; abort(); }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask) { (void)c; (void)color_write_mask; } void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }
int rp_pass_begin(nvn_gpu_cmd c) {
    if (pass_open) return 1;
    nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = target_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4; pass.layers = 1; nvn_gpu_pass_begin(c, &pass); pass_open = 1; rp_dirty_all(); return 1;
}
void rp_pass_flush(nvn_gpu_cmd c) { if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; } }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last) { if (pass_open) xfer_after_pass++; rp_pass_flush(active_cmd); if (last) *last = 1; return active_cmd; }
void rp_buf_reset(void);
void rp_buf_pool_gone(const void *mem);
static void *pool_gone_thread(void *mem) { rp_buf_pool_gone(mem); return NULL; }   // 游戏的别的线程销毁池（MapVirtual 改 run / MemoryPoolFinalize 最终都走 nvn_be_pool_del → rp_buf_pool_gone）


static int run_arm(const char *dir, const char *shader, uint32_t off, int atomic) {
    struct rp_prog *pg = &progs[cur_prog];
    memset(pg, 0, sizeof *pg);
    nvn_gpu_layout_ent *ent = &pg->ent[0][0];
    ent->set = 0; ent->binding = 0; ent->cls = NVN_GPU_RES_IMAGE_TBUF; ent->src_kind = NVN_RB_IMG;
    ent->src_reg = 0; ent->src_reg2 = UINT16_MAX; ent->count = 1; ent->access = NVN_GPU_ACC_RW; ent->stages = 1u << NVN_GPU_SH_CS;
    pg->is_compute = 1; pg->nent[0] = 1; pg->dtt[0][0] = NVN_TEXTURE_TARGET_BUFFER + 1; pg->dcls[0][0] = 1; pg->difmt[0][0] = 33;   // r32ui
    nvn_gpu_layout lay = {0}; lay.ent = ent; lay.nent = 1;
    pg->sh[0] = load_one_shader(dir, shader, NVN_GPU_SH_CS, &lay);
    if (!pg->sh[0]) return 0;
    const uint32_t values[4] = {10, 20, 30, 40};
    memset(guest, 0xa5, sizeof guest); memcpy(guest + off, values, sizeof values);
    memset(&pool, 0, sizeof pool); pool.mem = guest; pool.size = sizeof guest;
    memset(&buffer_tex, 0, sizeof buffer_tex); buffer_tex.magic = MAGIC('T','E','X','T'); buffer_tex.serial = 88;
    buffer_tex.d.target = NVN_TEXTURE_TARGET_BUFFER; buffer_tex.d.format = NVN_FORMAT_R32UI; buffer_tex.d.width = 4; buffer_tex.d.flags = NVN_TEXTURE_FLAGS_IMAGE;
    buffer_tex.pool = &pool; buffer_tex.off = off; buffer_tex.mem = guest + off;
    rp_state_reset(); p_bind_handles bh = {0}; bh.stage = 5; bh.first = 0; bh.n = 1; bh.h[0] = 7; rp_state_op(OP_BIND_IMG, &bh, sizeof bh);
    nvn_gpu_buf rb = make_buf(48, NVN_GPU_MEM_SHARED, NULL);
    active_cmd = begin_cmd(); pass_open = 0;
    uint64_t d0 = 0, s0 = 0; rp_draw_stats(&d0, &s0);
    p_dispatch dp = {0}; dp.x = dp.y = dp.z = 1;
    rp_dispatch(active_cmd, &dp, NULL);
    uint64_t d1 = 0, s1 = 0; rp_draw_stats(&d1, &s1);
    CHECK(s1 == s0, "%s @%u: dispatch skipped (%llu)", shader, off, (unsigned long long)(s1 - s0));
    nvn_gpu_buf mb = NULL; uint64_t moff = 0;
    CHECK(rp_buf_resolve(active_cmd, (uint64_t)(uintptr_t)guest + off - 16, 48, &mb, &moff) && mb, "%s @%u: resolve the pool mirror", shader, off);
    if (mb) nvn_gpu_copy_buf(active_cmd, mb, moff, rb, 0, 48);
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_cmd_commit(active_cmd, ++g_fence_no, on_done, NULL, &e);
    CHECK(OK(rc), "%s @%u commit: %d %s", shader, off, rc.code, e.why);
    int bad = 0;
    if (OK(rc)) {
        rc = nvn_gpu_fence_wait(g_fence_no, 5000000000ull, &e); CHECK(OK(rc), "%s @%u wait: %d %s", shader, off, rc.code, e.why);
        const uint8_t *p = buf_cpu(rb);
        for (int i = 0; i < 16; i++) if (p[i] != 0xa5 || p[32 + i] != 0xa5) bad++;
        CHECK(bad == 0, "%s @%u: guard bytes changed (%d)", shader, off, bad);
        for (int i = 0; i < 4; i++) { uint32_t got; memcpy(&got, p + 16 + 4 * i, 4); uint32_t want = atomic ? values[i] + 5u : values[i] * 3u + 1u;
            CHECK(got == want, "%s @%u element %d: %u expected %u", shader, off, i, got, want); if (got != want) bad++; }
    }
    printf("IMAGE_TBUF arm=%s off=%u failures=%d\n", shader, off, bad);
    rp_buf_reset(); nvn_gpu_buf_release(rb, NULL, NULL); cur_prog++;   // 着色器在 main 收尾时释放
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char why[256] = {0}; if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "image-tbuf-core";
    if (!open_backend()) return 1;
    nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
    printf("minTexelBufferOffsetAlignment=%u maxTexelBufferElements=%u\n", cp.min_texel_align, cp.max_texel_elems);
    if (cp.min_texel_align <= 4) { fprintf(stderr, "【未证】设备纹素缓冲偏移对齐 %u ≤ 4，覆盖不到对齐副本路径\n", cp.min_texel_align); return 2; }
    run_arm(argv[1], "image_tbuf_rmw.comp", 256, 0);
    run_arm(argv[1], "image_tbuf_rmw.comp", 516, 0);
    run_arm(argv[1], "image_tbuf_atomic.comp", 772, 1);
    for (int k = 0; k < 3; k++) if (progs[k].sh[0]) nvn_gpu_shader_release(progs[k].sh[0]);
    nvn_gpu_err err = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close session"); s2a_vk_device_destroy(&g_dev, 1);
    printf("IMAGE_TBUF replay: 3 arms, failures=%d\n", g_fail); return g_fail ? 1 : 0;
}

// 本夹具不模拟真实块代号；返回未知，让回放沿用逐页保守路径。
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }   // 本夹具没有脏页追加日志：回放核心走原路径
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
