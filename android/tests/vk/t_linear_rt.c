// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实渲染目标与 CopyT2B 回放。只替代资源登记和池回写账。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_exec.h"
#include "rp_res.h"

enum { DST_ADDR = 0x18000, DST_ROW = 16, DST_IMG = 40, DST_OFF = 4, DST_SIZE = 56 };
static nvn_gpu_tex linear_tex;
static nvn_gpu_buf linear_buf;
static unsigned linear_unimpl, linear_registered;

nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c) { (void)t; (void)c; return linear_tex; }
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t address, uint64_t bytes, nvn_gpu_buf *b, uint64_t *off) {
    (void)c;
    if (address != DST_ADDR || bytes != DST_IMG) return 0;
    *b = linear_buf; *off = DST_OFF;
    return 1;
}
int rp_post_gpu_write_rows(uint64_t address, uint64_t bytes, uint64_t row, uint64_t image, uint32_t height, uint32_t slices) {
    if (address != DST_ADDR || bytes != 8 || row != DST_ROW || image != DST_IMG || height != 2 || slices != 1) return 0;
    linear_registered++;
    return 1;
}
uint64_t nvn_unimpl_count(const char *cls, const char *name) { (void)cls; (void)name; return 0; }
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) {
    (void)site; printf("unimplemented %s/%s\n", cls, name);
    return ++linear_unimpl;
}

static void linear_copy(uint32_t flags, uint64_t signal) {
    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                                .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = flags, .stride = 24},
                           .usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD};
    nvn_gpu_err err = {0};
    const nvn_gpu_rc made = nvn_gpu_tex_create(&td, &linear_tex, &err);
    CHECK(OK(made), "linear RT create: %d %s", made.code, err.why);
    if (!OK(made)) return;
    nvn_gpu_view v = make_view(linear_tex);
    uint8_t seed[DST_SIZE], cpu[4][24], want[DST_SIZE];
    memset(seed, 0xdd, sizeof seed); memcpy(want, seed, sizeof want); memset(cpu, 0xee, sizeof cpu);
    for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++) {
        uint8_t *p = &cpu[y][x * 4];
        p[0] = (uint8_t)(y * 16 + x); p[1] = 70; p[2] = 90; p[3] = 255;
    }
    for (unsigned y = 0; y < 2; y++) memcpy(want + DST_OFF + y * DST_ROW, &cpu[y + 1][4], 8);
    const uint8_t blue[] = {0, 0, 255, 255};
    memcpy(want + DST_OFF + 4, blue, sizeof blue);
    linear_buf = make_buf(DST_SIZE, NVN_GPU_MEM_SHARED, seed);
    nvn_gpu_buf consumer = make_buf(DST_SIZE, NVN_GPU_MEM_SHARED, NULL);
    if (!v || !linear_buf || !consumer) goto done;
    tex_t tex = {.d = td.d, .serial = signal};
    nvn_tex_layout(&tex.d, &tex.L);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, linear_tex, 0, 0, NULL, cpu, sizeof cpu[0], sizeof cpu);
    const nvn_gpu_pass_desc pass = {.color = {{.view = v, .load = NVN_GPU_LOAD_LOAD, .store = NVN_GPU_STORE_STORE}},
                                    .ncolor = 1, .w = 4, .h = 4, .layers = 1};
    nvn_gpu_pass_begin(c, &pass);
    const nvn_gpu_clear cl = {.color = 0, .color_mask = 15, .value.f = {0, 0, 1, 1}, .x = 2, .y = 1, .w = 1, .h = 1};
    nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_pass_end(c);
    linear_unimpl = linear_registered = 0;
    const p_copy_b2t op = {.tex = &tex, .src = DST_ADDR, .r = {1, 1, 0, 2, 2, 1}, .row_stride = DST_ROW, .image_stride = DST_IMG};
    rp_op_copy_t2b(c, &op, &op);
    nvn_gpu_copy_buf(c, linear_buf, 0, consumer, 0, DST_SIZE);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, signal, on_done, NULL, &err);
    if (OK(rc)) rc = nvn_gpu_fence_wait(signal, 5000000000ull, &err);
    CHECK(OK(rc), "linear RT commit: %d %s", rc.code, err.why);
    if (OK(rc)) {
        const uint8_t *got = buf_cpu(consumer);
        unsigned matched = 0;
        for (unsigned i = 0; i < DST_SIZE; i++) matched += got[i] == want[i];
        printf("LINEAR_RT flags=0x%x CopyT2B bytes=%u/%d unimpl=%u\n", flags, matched, DST_SIZE, linear_unimpl);
        CHECK(matched == DST_SIZE && !linear_unimpl && linear_registered == 1, "LINEAR_RT CopyT2B");
    }
done:
    nvn_gpu_buf_release(consumer, NULL, NULL); nvn_gpu_buf_release(linear_buf, NULL, NULL);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(linear_tex);
}

int main(void) {
    char why[192] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 2; }
    printf("device: %s\n", g_dev.properties.deviceName);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "linear-rt-copy-t2b";
    if (!open_backend()) return 1;
    linear_copy(NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, 1);
    linear_copy(NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_LINEAR, 2);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "linear RT close: %s", err.why);
    s2a_vk_device_destroy(&g_dev, 1);
    return g_fail ? 1 : 0;
}
