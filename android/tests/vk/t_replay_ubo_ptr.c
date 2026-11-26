// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Actual replay submission, buffer mirrors and Vulkan draws. Only program and
// attachment selection are supplied by the fixture.
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_exec.c"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "rp_shader.h"

static nvn_gpu_err test_err;
static struct rp_prog gfx, compute;
static void (*prepare_write)(void *, size_t);
static nvn_gpu_view color_view;
static tex_t target_texture;
static p_set_rt target;
static int pass_open, pass_count;

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "UBOA fixture line %d: %s (%d %s)\n", __LINE__, #expr, test_err.rc.code, test_err.why); \
    exit(2); \
} } while (0)

void dm_fault_hook_register(int (*fn)(void *, int, void *)) { REQUIRE(fn != NULL); }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { prepare_write = fn; }
void rp_res_xfer_aborted(uint64_t sub) { (void)sub; REQUIRE(0); }
void rp_tex_async_step(nvn_gpu_cmd c) { (void)c; }
const char *nvn_plat_dir(int which) { (void)which; return NULL; }
int s2a_guest_tick_from_host_ns(uint64_t ns, uint64_t *out) { (void)ns; (void)out; REQUIRE(0); return 0; }
uint64_t s2a_guest_tick(void) { REQUIRE(0); return 0; }
int s2a_nvn_render_cond(void) { REQUIRE(0); return 0; }
int g_s2a_logfd = 2, s2a_nvn_rast_flip;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int t) { (void)p; (void)t; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t s, uint8_t k[6][8], uint8_t *o, uint8_t t[6]) {
    (void)s; (void)k; (void)o; (void)t; return NULL;
}
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    (void)q; *out = &gfx; return 0;
}
int rp_shader_resolve_compute(const struct rp_prog **out) { *out = &compute; return 0; }
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
nvn_gpu_view rp_view_by_id(int i, nvn_gpu_cmd c, tex_t *t) {
    (void)i; (void)c; if (t) memset(t, 0, sizeof *t); return NULL;
}
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) {
    (void)t; (void)v; (void)c; return NULL;
}
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int i, uint8_t t, uint8_t k, int e, nvn_gpu_cmd c, tex_t *o) {
    (void)t; (void)k; (void)e; return rp_view_by_id(i, c, o);
}
nvn_gpu_view rp_view_by_ubo_handle_decl(int i, uint8_t t, uint8_t k, nvn_gpu_cmd c, tex_t *o) {
    return rp_view_by_id_decl(i, t, k, 0, c, o);
}
void rp_decl_probe_begin(void) {}
int rp_decl_probe_end(void) { return 0; }
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c, int s, int b, uint32_t n, uint8_t t, uint8_t k, const char **why) {
    (void)c; (void)s; (void)b; (void)n; (void)t; (void)k; if (why) *why = "pool_table_unresolved"; return NULL;
}
nvn_gpu_samp rp_samp_by_id(int i) { (void)i; return NULL; }
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) {
    (void)pb; (void)pn; return rp_samp_for_tex(sid, tid);
}
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int i, tex_t *t, view_t *v, int *h) { (void)i; (void)t; (void)v; (void)h; return 0; }
uint64_t rp_tex_reg_gen(void) { return 1; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int c) { (void)h; (void)n; (void)b; (void)c; return 0; }
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; }
int rp_tex_debug_state(uint64_t s) { (void)s; return 0; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s) { (void)s; return NULL; }

// The fixture performs a real CLEAR on the first VkRenderPass and a real LOAD
// on each reopen, like the replay planner when the target has not changed.
enum { WIDTH = 8, HEIGHT = 8 };
const p_set_rt *rp_pass_target(void) { return &target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = WIDTH; *h = HEIGHT; }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { *w = WIDTH; *h = HEIGHT; }
void rp_pass_note_draw(int d) { (void)d; }
int rp_pass_parked(void) { return 0; }
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t mask) { (void)c; (void)mask; }
void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }
void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *reads, int n) { (void)c; (void)reads; (void)n; }
int rp_pass_is_open(void) { return pass_open; }
int rp_pass_begin(nvn_gpu_cmd c) {
    if (pass_open) return 1;
    nvn_gpu_pass_desc pass = {.ncolor = 1, .w = WIDTH, .h = HEIGHT, .layers = 1};
    pass.color[0] = (nvn_gpu_att){.view = color_view, .load = pass_count ? NVN_GPU_LOAD_LOAD : NVN_GPU_LOAD_CLEAR,
                                 .store = NVN_GPU_STORE_STORE};
    pass.color[0].clear.f[3] = 1;
    nvn_gpu_pass_begin(c, &pass);
    pass_open = 1;
    pass_count++;
    return 1;
}
void rp_pass_end(nvn_gpu_cmd c) {
    if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; }
}
void rp_pass_flush(nvn_gpu_cmd c) { rp_pass_end(c); }
uint32_t rp_pass_take_barrier(void) { return 0; }

static void commit_replay(void) {
    const uint64_t fence = nvn_be_sub_commit();
    REQUIRE(OK(nvn_gpu_fence_wait(fence, 5000000000ull, &test_err)));
    REQUIRE(rp_fence_done(fence) && !nvn_gpu_fence_failed());
}

static void producer(const char *dir, uint64_t location, uint64_t value) {
    const nvn_gpu_layout_ent entry = {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W,
                                      .count = 1, .stages = 1u << NVN_GPU_SH_CS};
    const nvn_gpu_layout layout = {.ent = &entry, .nent = 1, .push_bytes = sizeof value};
    nvn_gpu_shader cs = NULL;
    nvn_gpu_pipe pipe = mk_cpipe(dir, "producer.comp", &layout, &cs);
    REQUIRE(pipe && cs);
    nvn_gpu_buf mirror = NULL;
    uint64_t off = 0;
    REQUIRE(rp_buf_resolve(g_cmd, location, sizeof value, &mirror, &off));
    nvn_gpu_set_pipe(g_cmd, pipe);
    const nvn_gpu_binding slot = {.cls = NVN_GPU_RES_SSBO, .u.b = {mirror, off, sizeof value}};
    nvn_gpu_bind(g_cmd, NVN_GPU_SH_CS, &slot, 1);
    REQUIRE(rp_post_gpu_write(location, sizeof value));
    nvn_gpu_push(g_cmd, NVN_GPU_SH_CS, 0, &value, sizeof value);
    nvn_gpu_dispatch(g_cmd, pipe, 1, 1, 1, NULL, 0);
    nvn_gpu_shader_release(cs);
}

static void inline_pointer(uint64_t location, uint64_t value) {
    const uint64_t op[3] = {location, sizeof value, value};
    // The exact OP_UPDATE_UB routing: overlap with GPU writes is an ordered
    // write into the mirror, otherwise replay updates guest memory directly.
    if (!rp_op_update_ub_gpu(g_cmd, op, op)) rp_op_update_ub(op, op);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    const char *dir = argv[1], *arm = argv[2];
    const int inline_only = !strcmp(arm, "inline"), writable = !strcmp(arm, "writable");
    const int repeat = !strcmp(arm, "repeat-rewrite");
    const int cs_inline = !strcmp(arm, "compute-inline");
    const int inline_gpu = !strcmp(arm, "inline-gpu") || cs_inline;
    const int cs_read = !strcmp(arm, "compute") || cs_inline;
    const int gpu = !strcmp(arm, "gpu") || inline_gpu || cs_read || repeat;
    const int expect_new = inline_only || gpu;
    REQUIRE(expect_new || writable || !strcmp(arm, "cpu"));
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&g_dev, why, sizeof why));
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "replay-ubo-pointer";
    REQUIRE(open_backend());

    static const uint32_t old_color[4] = {17, 34, 51, 255}, new_color[4] = {71, 190, 93, 255};
    const size_t page = nvn_dirty_page_size(), bytes = 5 * page;
    uint8_t *guest = NULL;
    REQUIRE(!posix_memalign((void **)&guest, page, bytes));
    memset(guest, 0, bytes);
    REQUIRE(nvn_dirty_log_configure(1));
    nvn_dirty_add_range(guest, bytes);
    nvn_be_pool_add(guest, bytes);
    REQUIRE(prepare_write && nvn_dirty_tracked(guest, bytes));
    const uint64_t old_addr = (uint64_t)(uintptr_t)(guest + 2 * page);
    const uint64_t new_addr = (uint64_t)(uintptr_t)(guest + 3 * page);
    uint8_t *const ptr = guest + page;
    uint8_t *const output = guest + 4 * page;
    const uint64_t pointer_addr = (uint64_t)(uintptr_t)ptr;
    prepare_write(guest, bytes);
    // Full-screen triangle; vertex 1's offset is the UV expected by quad.vert.
    const float vertices[][8] = {{-1,-1,0,1,0,0,0,1}, {3,-1,0,1,1,0,0,1}, {-1,3,0,1,0,1,0,1}};
    memcpy(guest, vertices, sizeof vertices);
    memcpy(ptr, &old_addr, sizeof old_addr);
    memcpy(guest + 2 * page, old_color, sizeof old_color);
    memcpy(guest + 3 * page, new_color, sizeof new_color);
    memset(guest + 3 * page + sizeof new_color, 0xa5, 16);

    rp_state_reset();
    nvn_unimpl_reset();
    rp_S.nattr = 2;
    rp_S.nstream = 1;
    rp_S.attr[0].fmt = rp_S.attr[1].fmt = NVN_FORMAT_RGBA32F;
    rp_S.attr[1].offset = 16;
    rp_S.stream[0].stride = sizeof vertices[0];
    rp_S.vbuf[0].address = (uint64_t)(uintptr_t)guest;
    rp_S.vbuf[0].size = sizeof vertices;
    rp_S.color.logic_op = 3;
    rp_S.chmask.mask = 15;
    rp_S.poly.mode = NVN_POLYGON_MODE_FILL;
    rp_S.ubo[NVN_GPU_SH_FS][0] = (p_bind_buf){.addr = pointer_addr, .size = sizeof(uint64_t)};
    rp_S.ubo[NVN_GPU_SH_CS][0] = rp_S.ubo[NVN_GPU_SH_FS][0];
    rp_S.ssbo[NVN_GPU_SH_CS][0] = (p_bind_buf){.addr = (uintptr_t)output, .size = sizeof new_color};

    const nvn_gpu_layout_ent input = {.cls = NVN_GPU_RES_SSBO, .src_kind = NVN_RB_UBOA,
                                     .access = writable ? NVN_GPU_ACC_RW : NVN_GPU_ACC_R, .count = 1};
    gfx.ent[1][0] = input;
    gfx.ent[1][0].stages = 1u << NVN_GPU_SH_FS;
    gfx.nent[1] = 1;
    gfx.attr_mask = 3;
    gfx.color_out_mask = 15;
    const nvn_gpu_layout fs = {.ent = gfx.ent[1], .nent = 1};
    gfx.sh[0] = load_one_shader(dir, "quad.vert", NVN_GPU_SH_VS, NULL);
    gfx.sh[1] = load_one_shader(dir, writable ? "writable.frag" : "reader.frag", NVN_GPU_SH_FS, &fs);
    REQUIRE(gfx.sh[0] && gfx.sh[1]);
    if (cs_read) {
        compute.is_compute = 1;
        compute.ent[0][0] = input;
        compute.ent[0][0].stages = 1u << NVN_GPU_SH_CS;
        compute.ent[0][1] = (nvn_gpu_layout_ent){.cls = NVN_GPU_RES_SSBO, .src_kind = NVN_RB_SBUFP,
                                               .access = NVN_GPU_ACC_W, .binding = 1, .count = 1,
                                               .stages = 1u << NVN_GPU_SH_CS};
        compute.nent[0] = 2;
        const nvn_gpu_layout cs = {.ent = compute.ent[0], .nent = 2};
        compute.sh[0] = load_one_shader(dir, "reader.comp", NVN_GPU_SH_CS, &cs);
        REQUIRE(compute.sh[0]);
    }

    nvn_gpu_tex color = make_tex(WIDTH, HEIGHT, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    color_view = make_view(color);
    REQUIRE(color && color_view);
    target_texture = (tex_t){.serial = 1, .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                            .width = WIDTH, .height = HEIGHT, .depth = 1, .levels = 1}};
    target = (p_set_rt){.n = 1, .color = {&target_texture}};
    const uint64_t image_bytes = WIDTH * HEIGHT * 4;
    nvn_gpu_buf readback = make_buf(repeat ? 2 * image_bytes : image_bytes, NVN_GPU_MEM_SHARED, NULL);
    REQUIRE(readback);

    // Prime the mirror before recording the producer, so the stale value is A.
    nvn_be_sub_begin();
    REQUIRE(rp_buf_resolve(g_cmd, pointer_addr, sizeof old_addr, NULL, NULL));
    commit_replay();

    nvn_be_sub_begin();
    if (gpu) producer(dir, pointer_addr, inline_gpu ? old_addr : new_addr);
    if (inline_only || inline_gpu) {
        if (inline_only) prepare_write(ptr, sizeof old_addr);
        inline_pointer(pointer_addr, new_addr);
    }
    if (cs_read) {
        const p_dispatch dispatch = {.x = 1, .y = 1, .z = 1};
        rp_dispatch(g_cmd, &dispatch, &dispatch);
    } else {
        rp_pass_begin(g_cmd);
        rp_pass_flush(g_cmd);
        const p_draw draw = {.prim = 4, .count = 3, .instances = 1};
        for (unsigned i = 0; i < (repeat ? 70u : 1u); i++) rp_draw(g_cmd, &draw, NULL, &draw);
        rp_pass_flush(g_cmd);
        nvn_gpu_copy_tex_buf(g_cmd, color, 0, 0, NULL, readback, 0, WIDTH * 4, image_bytes);
        if (repeat) {
            inline_pointer(pointer_addr, old_addr);
            rp_draw(g_cmd, &draw, NULL, &draw);
            rp_pass_flush(g_cmd);
            nvn_gpu_copy_tex_buf(g_cmd, color, 0, 0, NULL, readback, image_bytes, WIDTH * 4, image_bytes);
        }
    }
    const uint32_t cuts = g_mid;
    // The standalone fixture registers the production dirty-write hook but
    // does not install the guest signal handler. Allow callback writes now.
    prepare_write(ptr, sizeof old_addr);
    prepare_write(output, sizeof new_color);
    commit_replay();

    const uint32_t *want = expect_new ? new_color : old_color;
    unsigned bad = 0;
    if (cs_read) {
        uint32_t actual[4];
        memcpy(actual, output, sizeof actual);
        bad = memcmp(actual, want, sizeof actual) != 0;
        printf("UBOA %s expected=%u,%u,%u,%u actual=%u,%u,%u,%u bad=%u\n",
               arm, want[0], want[1], want[2], want[3], actual[0], actual[1], actual[2], actual[3], bad);
    } else {
        const uint8_t desired[4] = {writable ? 0 : want[0], writable ? 0 : want[1], writable ? 0 : want[2], 255};
        const uint8_t *pixels = nvn_gpu_buf_cpu(readback);
        for (unsigned i = 0; i < WIDTH * HEIGHT; i++) bad += memcmp(pixels + i * 4, desired, 4) != 0;
        printf("UBOA %s expected=%u,%u,%u,255 actual=%u,%u,%u,%u pixels=%u bad=%u\n",
               arm, desired[0], desired[1], desired[2], pixels[0], pixels[1], pixels[2], pixels[3], WIDTH * HEIGHT, bad);
    }
    const uint64_t rejected = nvn_unimpl_count("draw", "compute_storage_indirect_writable_unknown");
    REQUIRE(rejected == (unsigned)writable && nvn_unimpl_kinds() == writable);
    REQUIRE(!memcmp(guest + 2 * page, old_color, sizeof old_color));
    REQUIRE(!memcmp(guest + 3 * page, new_color, sizeof new_color));
    for (unsigned i = 0; i < 16; i++) REQUIRE(guest[3 * page + sizeof new_color + i] == 0xa5);
    const uint64_t final_addr = expect_new && !repeat ? new_addr : old_addr;
    REQUIRE(!memcmp(ptr, &final_addr, sizeof final_addr));
    printf("UBOA %s writable_reject=%llu target_and_neighbor_intact=1\n", arm, (unsigned long long)rejected);
    if (repeat) {
        const uint8_t desired[4] = {old_color[0], old_color[1], old_color[2], old_color[3]};
        const uint8_t *pixels = nvn_gpu_buf_cpu(readback) + image_bytes;
        unsigned rewrite_bad = 0;
        for (unsigned i = 0; i < WIDTH * HEIGHT; i++) rewrite_bad += memcmp(pixels + i * 4, desired, 4) != 0;
        printf("UBOA repeat-rewrite second=%u,%u,%u,%u cuts=%u bad=%u\n",
               pixels[0], pixels[1], pixels[2], pixels[3], cuts, rewrite_bad);
        bad += rewrite_bad;
    }
    REQUIRE(cuts == (unsigned)(repeat ? 2 : gpu));

    rp_draw_reset();
    nvn_be_pool_del(guest);
    nvn_dirty_remove_range(guest);
    free(guest);
    for (unsigned i = 0; i < 2; i++) nvn_gpu_shader_release(gfx.sh[i]);
    if (compute.sh[0]) nvn_gpu_shader_release(compute.sh[0]);
    nvn_gpu_view_release(color_view);
    nvn_gpu_tex_release(color);
    nvn_gpu_buf_release(readback, NULL, NULL);
    REQUIRE(OK(nvn_gpu_close(5000000000ull, &test_err)));
    REQUIRE(!s2a_vk_device_destroy(&g_dev, 1));
    return bad || g_fail ? 1 : 0;
}
