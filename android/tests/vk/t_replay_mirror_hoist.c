// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真池表、脏页跟踪、镜像解析、提交顺序和 Vulkan 后端。
// 夹具只安排三笔绘制的位置和着色器；回放核心决定前置段和拆遍。
#define main backend_fixture_main
#define nvn_plat_flag backend_fixture_flag
#include "t_backend_vk.c"
#undef nvn_plat_flag
#undef main
__typeof__(backend_fixture_flag) nvn_plat_flag;
#include "rp_exec.c"
#include "nvn_dirty.h"
#include <signal.h>
#include <unistd.h>

static nvn_gpu_err test_err;
static int hoist, hoist_v2, pass_open, passes;
static void (*prepare_write)(void *, size_t);
static int (*dirty_fault)(void *, int, void *);

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "mirror fixture line %d: %s (%d %s)\n", __LINE__, #expr, test_err.rc.code, test_err.why); \
    exit(2); \
} } while (0)

int nvn_plat_flag(const char *key, int dflt) {
    if (!strcmp(key, "nvk_mirror_disjoint")) return hoist;
    if (!strcmp(key, "nvk_mirror_disjoint_v2")) return hoist_v2;
    return backend_fixture_flag(key, dflt);
}

void dm_fault_hook_register(int (*fn)(void *, int, void *)) { REQUIRE(fn != NULL); dirty_fault = fn; }

// On the overlap arm, recording the later upload protects the landing page
// again. Route its write fault through the real dirty-page tracker.
static void handle_dirty_fault(int sig, siginfo_t *info, void *context) {
    // The CI container can report MAPERR for a tracked, protected page.
    // Only the dirty tracker can claim its address and release the page.
    if (info->si_code > 0 && dirty_fault && dirty_fault(info->si_addr, 1, context))
        return;
    _exit(128 + sig);
}
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { prepare_write = fn; }

// 本夹具不录计数器报告、调试转储或贴图快照。若意外进入其失败/回落路径，直接报错。
void rp_debug_dump_poll(uint64_t *fence) { (void)fence; }
void rp_res_xfer_aborted(uint64_t sub) { (void)sub; REQUIRE(0); }
void rp_tex_async_step(nvn_gpu_cmd c) { (void)c; }
uint64_t rp_gpu_wr_gen;
void rp_draw_stats(uint64_t *draws, uint64_t *skips) {
    if (draws) *draws = 0;
    if (skips) *skips = 0;
}
int s2a_guest_tick_from_host_ns(uint64_t ns, uint64_t *out) { (void)ns; (void)out; REQUIRE(0); return 0; }
uint64_t s2a_guest_tick(void) { REQUIRE(0); return 0; }
int s2a_nvn_render_cond(void) { REQUIRE(0); return 0; }

// 只替代游戏的通道规划。切遍要真实结束 VkRenderPass，续遍用 LOAD 保住第一笔像素。
int rp_pass_is_open(void) { return pass_open; }
void rp_pass_end(nvn_gpu_cmd c) {
    if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; }
}
void rp_pass_flush(nvn_gpu_cmd c) { rp_pass_end(c); }

enum { WIDTH = 4, HEIGHT = 4 };
static void write_vertices(uint8_t *dst, int green) {
    const float vertices[][8] = {
        {-1, -1, 0, 1, !green, green, 0, 1},
        { 3, -1, 0, 1, !green, green, 0, 1},
        {-1,  3, 0, 1, !green, green, 0, 1},
    };
    prepare_write(dst, sizeof vertices);
    memcpy(dst, vertices, sizeof vertices);
}

static void draw_column(nvn_gpu_view view, nvn_gpu_pipe pipe, const void *guest, unsigned col) {
    nvn_gpu_buf mirror = NULL;
    uint64_t off = 0;
    REQUIRE(rp_buf_resolve(g_cmd, (uint64_t)(uintptr_t)guest, 3 * 8 * sizeof(float), &mirror, &off));
    if (!pass_open) {
        nvn_gpu_pass_desc pass = {0};
        pass.ncolor = 1;
        pass.color[0].view = view;
        pass.color[0].load = passes ? NVN_GPU_LOAD_LOAD : NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[3] = 1;
        pass.w = WIDTH; pass.h = HEIGHT; pass.layers = 1;
        nvn_gpu_pass_begin(g_cmd, &pass);
        pass_open = 1;
        passes++;
    }
    nvn_gpu_dyn dyn = {0};
    dyn.origin_ll = 1; dyn.nvp = 1; dyn.has_sc = 1;
    dyn.vp[0][2] = WIDTH; dyn.vp[0][3] = HEIGHT; dyn.vp[0][5] = 1;
    dyn.sc[0] = col; dyn.sc[2] = 1; dyn.sc[3] = HEIGHT;
    nvn_gpu_set_pipe(g_cmd, pipe);
    nvn_gpu_set_dyn(g_cmd, &dyn);
    nvn_gpu_set_vbuf(g_cmd, 0, mirror, off);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(g_cmd, &draw);
}

static void commit_mirror(void) {
    uint64_t fence = nvn_be_sub_commit();
    REQUIRE(OK(nvn_gpu_fence_wait(fence, 5000000000ull, &test_err)));
    REQUIRE(rp_fence_done(fence) && !nvn_gpu_fence_failed());
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    const char *arm = argv[2];
    const int overlap = !strcmp(arm, "overlap");
    const int pending_gap = !strcmp(arm, "v2-pending-gap");
    const int pending_hit = !strcmp(arm, "v2-pending-hit");
    const int grown_gap = !strcmp(arm, "v2-grown-gap");
    const int pending = !strcmp(arm, "pending") || pending_gap || pending_hit;
    const int off = !strcmp(arm, "off");
    REQUIRE(overlap || pending || grown_gap || off || !strcmp(arm, "gap"));
    hoist = !off;
    hoist_v2 = pending_gap || pending_hit || grown_gap;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&g_dev, why, sizeof why));
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "replay-mirror-hoist";
    REQUIRE(open_backend());

    const size_t page = nvn_dirty_page_size(), bytes = 8 * page;
    uint8_t *guest = NULL;
    struct sigaction fault = {.sa_sigaction = handle_dirty_fault, .sa_flags = SA_SIGINFO};
    REQUIRE(sigemptyset(&fault.sa_mask) == 0 && sigaction(SIGSEGV, &fault, NULL) == 0);
    REQUIRE(!posix_memalign((void **)&guest, page, bytes));
    memset(guest, 0, bytes);
    REQUIRE(nvn_dirty_log_configure(1));
    nvn_dirty_add_range(guest, bytes);
    nvn_be_pool_add(guest, bytes);
    REQUIRE(prepare_write && nvn_dirty_tracked(guest, bytes));
    uint8_t *const target = guest + 3 * page;
    write_vertices(guest, 0);
    write_vertices(guest + 6 * page, 0);
    write_vertices(target, 0);
    write_vertices(target + 128, 0);

    nvn_gpu_buf mirror = NULL;
    nvn_be_sub_begin();
    REQUIRE(rp_buf_resolve(g_cmd, (uint64_t)(uintptr_t)guest, bytes, &mirror, NULL));
    commit_mirror();

    nvn_gpu_pipe pipe = color_pipe(argv[1], 0);
    nvn_gpu_tex tex = make_tex(WIDTH, HEIGHT, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex);
    const size_t pixel_bytes = WIDTH * HEIGHT * 4;
    nvn_gpu_buf readback = make_buf(pixel_bytes, NVN_GPU_MEM_SHARED, NULL);
    REQUIRE(pipe && tex && view && readback);
    nvn_be_sub_begin();
    const uint64_t gpu_off = pending_hit ? 4 * page - sizeof(uint32_t) : 5 * page;
    if (pending) {
        const uint32_t marker = 0xdecafbad;
        // 完成回调在宿主写回；把目标页开放给它，回写数据仍必须来自 GPU。
        prepare_write(guest + gpu_off, sizeof marker);
        REQUIRE(rp_post_gpu_write((uint64_t)(uintptr_t)(guest + gpu_off), sizeof marker));
        nvn_gpu_buf_write(g_cmd, mirror, gpu_off, &marker, sizeof marker);
        REQUIRE(rp_buf_gpu_pending((uint64_t)(uintptr_t)(guest + gpu_off), sizeof marker));
    }
    // [第 0 页，第 6 页] 的老桶包络罩住第 3 页。overlap 臂再让第一笔真实读取第 3 页。
    draw_column(view, pipe, overlap ? target : guest, 0);
    draw_column(view, pipe, guest + 6 * page, 1);
    if (grown_gap) {
        for (unsigned i = 0; i < 40; i++) {
            REQUIRE(rp_buf_resolve(g_cmd, (uint64_t)(uintptr_t)(guest + page + 2 * i), 1, &mirror, NULL));
            REQUIRE(rp_buf_resolve(g_cmd, (uint64_t)(uintptr_t)(guest + 5 * page + 2 * i), 1, &mirror, NULL));
        }
    }
    if (overlap) write_vertices(target, 1);
    write_vertices(target + 128, 1);
    draw_column(view, pipe, target + 128, 2);
    const int pre = g_pre != NULL;
    rp_pass_end(g_cmd);
    nvn_gpu_copy_tex_buf(g_cmd, tex, 0, 0, NULL, readback, 0, WIDTH * 4, pixel_bytes);
    const void *landing_span = pending_hit ? nvn_dirty_span(guest + gpu_off, sizeof(uint32_t)) : NULL;
    REQUIRE(!pending_hit || (landing_span && nvn_dirty_page_armed(landing_span, guest + gpu_off)));
    const uint64_t before_landing = nvn_dirty_now();
    commit_mirror();

    static const uint8_t row[WIDTH][4] = {{255, 0, 0, 255}, {255, 0, 0, 255},
                                           {0, 255, 0, 255}, {0, 0, 0, 255}};
    const uint8_t *pixels = nvn_gpu_buf_cpu(readback);
    REQUIRE(pixels);
    unsigned bad = 0;
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) bad += memcmp(pixels + 4 * (y * WIDTH + x), row[x], 4) != 0;
    uint32_t landed = 0;
    memcpy(&landed, guest + gpu_off, sizeof landed);
    printf("MIRROR_HOIST arm=%s pre=%d passes=%d draw1=%02x%02x%02x%02x bad_pixels=%u landed=%08x\n",
           arm, pre, passes, pixels[0], pixels[1], pixels[2], pixels[3], bad, landed);
    const int want_pre = !off && !overlap && (!pending || pending_gap);
    CHECK(pre == want_pre && passes == 2 - want_pre, "%s: wrong pre-submit placement / pass count", arm);
    CHECK(!bad, "%s: rendered bytes changed", arm);
    CHECK(landed == (pending ? 0xdecafbadu : 0), "%s: GPU writeback changed", arm);
    CHECK(!pending_hit || nvn_dirty_since_span(landing_span, guest + gpu_off, sizeof landed, before_landing),
          "%s: landing did not dirty the protected page", arm);
    CHECK(!nvn_unimpl_kinds(), "%s: replay reported unsupported calls", arm);

    nvn_gpu_buf_release(readback, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    nvn_be_pool_del(guest);
    nvn_dirty_remove_range(guest);
    free(guest);
    CHECK(OK(nvn_gpu_close(5000000000ull, &test_err)), "backend close failed: %s", test_err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "device destroy failed");
    return g_fail ? 1 : 0;
}
