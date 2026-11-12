// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实回放清除实现 + Vulkan 后端；仅资源查找换成受控纹理，读回全部像素。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_exec.h"
#include "rp_res.h"

static nvn_gpu_tex g_image;
static nvn_gpu_view g_views[8]; static int g_nviews, g_writes, g_unimpl;
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) {
    (void)t; (void)c; nvn_gpu_view out = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_view_create(g_image, v, &out, &e);
    CHECK(OK(r), "replay view：%d %s", r.code, e.why);
    if (out && g_nviews < 8) g_views[g_nviews++] = out;
    return out;
}
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; g_writes++; }
void rp_dirty_all(void) {}
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) {
    (void)site; printf("unimplemented %s/%s\n", cls, name); return ++g_unimpl;
}

int main(void) {
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { printf("device failed: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-replay-clear-test";
    if (!open_backend()) return 1;
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 8; td.d.depth = 3; td.d.levels = 2; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc r = nvn_gpu_tex_create(&td, &g_image, &e);
    if (!OK(r)) { printf("texture failed: %d %s\n", r.code, e.why); return 1; }
    nvn_gpu_view all = make_view(g_image); nvn_gpu_buf rb = make_buf(3 * 4 * 4 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd(); float bg[4] = {0.25f, 0.5f, 0.75f, 1};
    nvn_gpu_clear_view(c, all, bg, NULL, 0);
    tex_t t = {0}; t.d = td.d; t.serial = 1;
    p_clear_tex p = {0}; p.tex = &t; p.has_view = 1; p.view.set_levels = 1; p.view.base_level = 1;
    p.r.zoffset = 1; p.r.depth = 1; p.r.width = p.r.height = 4; p.mask = 15; p.value_type = CLEAR_TEX_FLOAT; p.c.f[0] = p.c.f[3] = 1;
    rp_op_clear_tex(c, &p, &p);
    p.r.zoffset = 2; p.r.xoffset = 1; p.r.yoffset = 1; p.r.width = 2; p.r.height = 2; p.mask = 2; p.c.f[1] = 1;
    rp_op_clear_tex(c, &p, &p);
    for (uint32_t layer = 0; layer < 3; layer++) nvn_gpu_copy_tex_buf(c, g_image, 1, layer, NULL, rb, 64 * layer, 16, 64);
    r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e); CHECK(OK(r), "commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(r), "wait：%d %s", r.code, e.why);
    uint8_t *pixels = buf_cpu(rb);
    for (int layer = 0; layer < 3; layer++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        const uint8_t *q = pixels + layer * 64 + (y * 4 + x) * 4;
        int green = layer == 2 && x >= 1 && x < 3 && y >= 1 && y < 3;
        uint8_t expected[4] = {layer == 1 ? 255 : 64, layer == 1 ? 0 : green ? 255 : 128, layer == 1 ? 0 : 191, 255};
        CHECK(memcmp(q, expected, 4) == 0, "layer %d xy %d,%d = %u,%u,%u,%u expected %u,%u,%u,%u", layer, x, y,
              q[0], q[1], q[2], q[3], expected[0], expected[1], expected[2], expected[3]);
    }
    CHECK(g_writes == 2 && g_unimpl == 0, "writes %d unimpl %d", g_writes, g_unimpl);
    for (int i = 0; i < g_nviews; i++) nvn_gpu_view_release(g_views[i]);
    nvn_gpu_view_release(all); nvn_gpu_tex_release(g_image); nvn_gpu_buf_release(rb, NULL, NULL);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close：%d %s", e.rc.code, e.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("%s replay CLEAR_TEX: 3 layers x 4 x 4 RGBA readback, failures=%d\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
