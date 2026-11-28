// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define nvn_plat_flag backend_fixture_flag
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag

extern uint64_t nvn_vk_dbg_dq_recs(void);

static int keep_state;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pass_state_keep")) return keep_state;
    return backend_fixture_flag(name, dflt);
}

enum { W = 64, H = 64, PASSES = 5, STATE_COUNT = 9 };

static void check_emits(nvn_gpu_cmd cmd, int pass, int expect_keep) {
    // 管线、视口、裁剪、混合常量、模板、深度偏移、顶点缓冲、索引缓冲、push。
    static const uint32_t kept[PASSES][STATE_COUNT] = {
        {1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 2, 1, 1, 1, 1, 1, 2},
        {2, 2, 3, 2, 2, 2, 2, 2, 3},
        {1, 1, 1, 1, 1, 1, 1, 1, 1},
    };
    uint32_t n[STATE_COUNT];
    nvn_gpu_vk_test_emit_counts(cmd, n);
    int good = 1;
    for (unsigned i = 0; i < STATE_COUNT; ++i)
        good &= n[i] == (expect_keep ? kept[pass][i] : pass == PASSES - 1 ? 1u : (uint32_t)pass + 1);
    printf("pass-state emits pass=%d pipe=%u vp=%u sc=%u blend=%u stencil=%u depth=%u vb=%u ib=%u push=%u\n",
           pass + 1, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8]);
    CHECK(good, "pass-state count mismatch pass=%d expect_keep=%d", pass + 1, expect_keep);
}

static void check_pixels(nvn_gpu_buf readback, int pass) {
    const uint8_t green[4] = {0, 255, 0, 255}, red[4] = {255, 0, 0, 255};
    const uint8_t blue[4] = {0, 0, 255, 255}, black[4] = {0, 0, 0, 255};
    const uint8_t *data = buf_cpu(readback);
    unsigned good = 0;
    if (data) for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        const uint8_t *expected = green;
        if (pass >= 2) expected = x < W / 2 ? red : black;
        if (pass == 3 && x >= W / 2 && x < W / 2 + 8 && y < 8) expected = blue;
        good += !memcmp(data + (y * W + x) * 4, expected, 4);
    }
    printf("pass-state pixels pass=%d good=%u/%d\n", pass + 1, good, W * H);
    CHECK(good == W * H, "pass-state pixel mismatch pass=%d", pass + 1);
}

static void complete_cmd(nvn_gpu_cmd cmd, uint64_t signal) {
    nvn_gpu_err err = {0};
    nvn_gpu_rc r = nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &err);
    CHECK(OK(r), "pass-state commit: %s", err.why);
    if (OK(r)) {
        r = nvn_gpu_fence_wait(signal, 5000000000ull, &err);
        CHECK(OK(r), "pass-state retire: %s", err.why);
    }
}

static void run_passes(const char *dir, int expect_keep) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    nvn_gpu_layout frag = {.push_bytes = 16};
    if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &frag, &vs, &fs, &vm, &fm)) {
        CHECK(0, "pass-state shaders");
        return;
    }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err err = {0};
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err)), "pass-state pipe: %s", err.why);
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    const uint16_t indices[4] = {0, 1, 2, 0};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_buf ib = make_buf(sizeof indices, NVN_GPU_MEM_SHARED, indices);
    nvn_gpu_tex targets[PASSES];
    nvn_gpu_view views[PASSES];
    nvn_gpu_buf readbacks[PASSES];
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    nvn_gpu_pass_desc pdsc = {.ncolor = 1, .w = W, .h = H, .layers = 1};
    pdsc.color[0].load = NVN_GPU_LOAD_CLEAR;
    pdsc.color[0].store = NVN_GPU_STORE_STORE;
    pdsc.color[0].clear.f[3] = 1;
    nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1, .ibuf = ib, .itype = 1};
    const float green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1};

    nvn_gpu_cmd cmd = begin_cmd();
    for (int i = 0; i < PASSES; ++i) {
        if (i == PASSES - 1) {
            complete_cmd(cmd, 1);
            cmd = begin_cmd();
        }
        targets[i] = make_tex(W, H, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
        views[i] = make_view(targets[i]);
        pdsc.color[0].view = views[i];
        nvn_gpu_pass_begin(cmd, &pdsc);
        if (i == 2) dyn.sc[2] = W / 2;
        if (i == 3) {
            nvn_gpu_clear clear = {.color_mask = 0xf, .x = W / 2, .w = 8, .h = 8};
            clear.value.f[2] = clear.value.f[3] = 1;
            nvn_gpu_clear_rect(cmd, &clear);
        }
        nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_set_vbuf(cmd, 0, vb, 0);
        nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, i < 2 ? green : red, sizeof green);
        nvn_gpu_draw(cmd, &draw);
        check_emits(cmd, i, expect_keep);
        nvn_gpu_pass_end(cmd);
    }

    for (int i = 0; i < PASSES; ++i) {
        readbacks[i] = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_copy_tex_buf(cmd, targets[i], 0, 0, NULL, readbacks[i], 0, W * 4, W * H * 4);
    }
    complete_cmd(cmd, 2);
    for (int i = 0; i < PASSES; ++i) {
        check_pixels(readbacks[i], i);
        nvn_gpu_buf_release(readbacks[i], NULL, NULL);
        nvn_gpu_view_release(views[i]);
        nvn_gpu_tex_release(targets[i]);
    }
    nvn_gpu_buf_release(ib, NULL, NULL);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    free(vm);
    free(fm);
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    keep_state = atoi(argv[2]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) {
        fprintf(stderr, "device: %s\n", why);
        return 2;
    }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "pass-state-keep"};
    if (!open_backend()) return 2;
    run_passes(argv[1], atoi(argv[3]));
    const uint64_t records = nvn_vk_dbg_dq_recs();
    printf("pass-state recorded DQ calls=%llu\n", (unsigned long long)records);
    CHECK(!!records == !!getenv("S2A_TEST_NVK_DQ"), "pass-state DQ mode");
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("pass-state Vulkan failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
