// Production replay + pool mirror + Vulkan buffer view; GLSL test shader.
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_dirty.h"

static p_set_rt target; static tex_t target_tex, buffer_tex; static pool_t pool;
static nvn_gpu_view target_view;
static struct rp_prog prog;
static _Alignas(4096) uint8_t guest[4096];
static nvn_gpu_cmd active_cmd; static int pass_open, xfer_after_pass;
int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) { (void)serial; (void)keys; (void)ok; (void)stage; return NULL; }
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) { (void)q; *out = &prog; return 0; }
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
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
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, const tex_t **t) { (void)id; (void)c; (void)t; return NULL; }
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t tt, uint8_t cls, int elem, nvn_gpu_cmd c, const tex_t **t) { (void)id; (void)tt; (void)cls; (void)elem; (void)c; (void)t; return NULL; }
nvn_gpu_samp rp_samp_by_id(int id) { (void)id; return NULL; }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) {
    if (id != 7) return 0;
    if (t) *t = buffer_tex;
    if (v) memset(v, 0, sizeof *v);
    if (h) *h = 0;
    return 1;
}
uint64_t rp_tex_reg_gen(void) { return 1; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int count) { (void)h; (void)n; (void)b; return (uint64_t)count; }
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; }
int rp_tex_debug_state(uint64_t s) { (void)s; return 0; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s) { (void)s; return NULL; }
const p_set_rt *rp_pass_target(void) { return &target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = *h = 4; }
void rp_pass_note_draw(int d) { (void)d; }
int rp_pass_begin(nvn_gpu_cmd c) {
    if (pass_open) return 1;
    nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = target_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4; pass.layers = 1; nvn_gpu_pass_begin(c, &pass); pass_open = 1; rp_dirty_all(); return 1;
}
void rp_pass_flush(nvn_gpu_cmd c) { if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; } }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last) { if (pass_open) xfer_after_pass++; rp_pass_flush(active_cmd); if (last) *last = 1; return active_cmd; }
void rp_buf_reset(void);

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char why[256] = {0}; if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "tbuf-core";
    if (!open_backend()) return 1;
    nvn_gpu_layout_ent *ent = &prog.ent[1][0];
    ent->set = 0; ent->binding = 0; ent->cls = NVN_GPU_RES_TBUF; ent->src_kind = NVN_RB_TBUF;
    ent->src_reg = 130; ent->src_reg2 = UINT16_MAX; ent->count = 1; ent->access = NVN_GPU_ACC_R; ent->stages = 1u << NVN_GPU_SH_FS;
    prog.nent[1] = 1; prog.dtt[1][0] = NVN_TEXTURE_TARGET_BUFFER + 1; prog.dcls[1][0] = 1; prog.color_out_mask = 0xffffffffu;
    nvn_gpu_layout fs_layout = {0}; fs_layout.ent = ent; fs_layout.nent = 1;
    void *vsm = NULL, *fsm = NULL;
    CHECK(load_shader_pair(argv[1], "separate.vert", "tbuf_replay.frag", NULL, &fs_layout, &prog.sh[0], &prog.sh[1], &vsm, &fsm), "load TBUF shaders");
    nvn_gpu_tex output = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); target_view = make_view(output);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    memset(&target_tex, 0, sizeof target_tex); target_tex.d.target = NVN_TEXTURE_TARGET_2D; target_tex.d.format = NVN_FORMAT_RGBA8;
    target_tex.d.width = target_tex.d.height = 4; target_tex.d.depth = target_tex.d.levels = 1; target_tex.serial = 7;
    memset(&target, 0, sizeof target); target.n = 1; target.color[0] = &target_tex;
    const uint32_t values[4] = {0x02030405u, 0x06070809u, 0x0a0b0c0du, 0x0e0f1011u};
    memcpy(guest + 256, values, sizeof values); memset(&pool, 0, sizeof pool); pool.mem = guest; pool.size = sizeof guest;
    memset(&buffer_tex, 0, sizeof buffer_tex); buffer_tex.magic = MAGIC('T','E','X','T'); buffer_tex.serial = 88;
    buffer_tex.d.target = NVN_TEXTURE_TARGET_BUFFER; buffer_tex.d.format = NVN_FORMAT_R32UI; buffer_tex.d.width = 4;
    buffer_tex.pool = &pool; buffer_tex.off = 256; buffer_tex.mem = guest + 256;
    rp_state_reset(); p_bind_handles bh = {0}; bh.stage = 1; bh.first = 130; bh.n = 1; bh.h[0] = 7; rp_state_op(OP_BIND_TEX, &bh, sizeof bh);
    active_cmd = begin_cmd(); p_draw draw = {0}; draw.prim = 4; draw.count = 3; draw.instances = 1;
    rp_draw(active_cmd, &draw, NULL, &draw);
    const uint32_t updated[4] = {0x03040506u, 0x0708090au, 0x0b0c0d0eu, 0x0f101112u};
    memcpy(guest + 256, updated, sizeof updated);
    rp_draw(active_cmd, &draw, NULL, &draw);
    uint64_t drawn = 0, skipped = 0; rp_draw_stats(&drawn, &skipped);
    CHECK(drawn == 2 && skipped == 0, "two TBUF draws without skips");
    CHECK(xfer_after_pass == 1, "second draw sync ends the first pass");
    if (drawn == 2) {
        rp_pass_flush(active_cmd); nvn_gpu_copy_tex_buf(active_cmd, output, 0, 0, NULL, rb, 0, 16, 64);
        nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_cmd_commit(active_cmd, 1, on_done, NULL, &e);
        CHECK(OK(rc), "TBUF commit: %d %s", rc.code, e.why);
        if (OK(rc)) { rc = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(rc), "TBUF wait: %d %s", rc.code, e.why); }
        const uint8_t expected[4] = {9, 13, 4, 255};
        for (int i = 0; i < 16; i++) { const uint8_t *p = buf_cpu(rb) + i * 4;
            CHECK(!memcmp(p, expected, 4), "TBUF pixel %d: %u,%u,%u,%u expected 9,13,4,255", i, p[0], p[1], p[2], p[3]); }
    } else nvn_gpu_cmd_abort(active_cmd);
    rp_buf_reset(); nvn_gpu_shader_release(prog.sh[0]); nvn_gpu_shader_release(prog.sh[1]); free(vsm); free(fsm);
    nvn_gpu_view_release(target_view); nvn_gpu_tex_release(output); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_err err = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close session"); s2a_vk_device_destroy(&g_dev, 1);
    printf("TBUF replay: expected 9,13,4,255 on 16 pixels, failures=%d\n", g_fail); return g_fail ? 1 : 0;
}
