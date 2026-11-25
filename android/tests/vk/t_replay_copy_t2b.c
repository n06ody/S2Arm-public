// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实 T2B 回放函数接 lavapipe；资源查找与完成写回以受控夹具替代。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_exec.h"
#include "rp_res.h"

static nvn_gpu_tex g_src; static nvn_gpu_buf g_dst; static uint8_t g_guest[256];
static struct { uint64_t addr, bytes, row, image; uint32_t rows, images; } g_write;
static int g_write_n, g_unimpl;
nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c) { (void)t; (void)c; return g_src; }
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t addr, uint64_t len, nvn_gpu_buf *buf, uint64_t *off) {
    (void)c; uint64_t base = (uint64_t)(uintptr_t)g_guest;
    if (addr < base || len > sizeof g_guest || addr - base > sizeof g_guest - len) return 0;
    *buf = g_dst; *off = addr - base; memcpy(nvn_gpu_buf_cpu(g_dst), g_guest, sizeof g_guest); return 1;
}
int rp_post_gpu_write_rows(uint64_t addr, uint64_t bytes, uint64_t row, uint64_t image, uint32_t rows, uint32_t images, struct rp_write_source source) {
    if (source.op != RP_W_T2B) g_fail++;
    g_write.addr = addr; g_write.bytes = bytes; g_write.row = row; g_write.image = image; g_write.rows = rows; g_write.images = images; g_write_n++; return 1;
}
uint64_t nvn_unimpl_count(const char *cls, const char *name) { (void)cls; (void)name; return 0; }
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) {
    (void)site; printf("unimplemented %s/%s\n", cls, name); return ++g_unimpl;
}
static void copy_done(void *user, const nvn_gpu_done *d) {
    (void)user;
    if (d->status != NVN_GPU_DONE_OK) { g_fail++; return; }
    uint8_t *dst = (uint8_t *)(uintptr_t)g_write.addr;
    const uint8_t *src = nvn_gpu_buf_cpu(g_dst) + (dst - g_guest);
    for (uint32_t z = 0; z < g_write.images; z++) for (uint32_t y = 0; y < g_write.rows; y++) {
        uint64_t off = (uint64_t)z * g_write.image + (uint64_t)y * g_write.row;
        memcpy(dst + off, src + off, (size_t)g_write.bytes);
    }
}
static void run_rgba(void) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4; td.d.depth = 3; td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e);
    if (!OK(rc)) { CHECK(0, "RGBA image: %d %s", rc.code, e.why); return; }
    memset(g_guest, 0x11, sizeof g_guest); g_dst = make_buf(sizeof g_guest, NVN_GPU_MEM_SHARED, g_guest);
    nvn_gpu_cmd c = begin_cmd(); uint8_t src[2][64];
    for (int z = 0; z < 2; z++) for (int i = 0; i < 64; i++) src[z][i] = (uint8_t)(0x20 + z * 0x40 + i);
    NVNcopyRegion upload = {0, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(c, g_src, 0, 1, &upload, src[0], 16, 64);
    nvn_gpu_tex_upload(c, g_src, 0, 2, &upload, src[1], 16, 64);
    tex_t t = {0}; t.serial = 10; t.d = td.d;
    p_copy_b2t p = {0}; p.tex = &t; p.src = (uint64_t)(uintptr_t)(g_guest + 16);
    p.has_view = 1; p.view.set_layers = 1; p.view.min_layer = 1;
    p.r = (NVNcopyRegion){1, 1, 0, 2, 2, 2}; p.row_stride = 16; p.image_stride = 48;
    rp_op_copy_t2b(c, &p, &p);
    CHECK(g_write_n == 1 && g_unimpl == 0, "RGBA registration=%d unsupported=%d", g_write_n, g_unimpl);
    CHECK(g_guest[16] == 0x11, "guest changed before GPU completion");
    g_guest[24] = 0x42;  // 提交中的填充并非 GPU 输出。
    rc = nvn_gpu_cmd_commit(c, 1, copy_done, NULL, &e); CHECK(OK(rc), "RGBA commit: %d %s", rc.code, e.why);
    rc = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(rc), "RGBA wait: %d %s", rc.code, e.why);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++) {
        uint8_t got = g_guest[16 + z * 48 + y * 16 + x]; uint8_t want = src[z][(y + 1) * 16 + 4 + x];
        CHECK(got == want, "RGBA z=%d y=%d byte=%d: %u != %u", z, y, x, got, want);
    }
    CHECK(g_guest[24] == 0x42 && g_guest[15] == 0x11 && g_guest[40] == 0x11 && g_guest[112] == 0x11, "RGBA padding changed");
    nvn_gpu_buf_release(g_dst, NULL, NULL); nvn_gpu_tex_release(g_src);
}
static void run_3d(void) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = td.d.depth = 4; td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e);
    if (!OK(rc)) { CHECK(0, "3D image: %d %s", rc.code, e.why); return; }
    memset(g_guest, 0x11, sizeof g_guest); g_dst = make_buf(sizeof g_guest, NVN_GPU_MEM_SHARED, g_guest);
    nvn_gpu_cmd c = begin_cmd(); uint8_t src[2][64];
    for (int z = 0; z < 2; z++) for (int i = 0; i < 64; i++) src[z][i] = (uint8_t)(0x30 + z * 0x40 + i);
    NVNcopyRegion upload = {0, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(c, g_src, 0, 1, &upload, src[0], 16, 64);
    nvn_gpu_tex_upload(c, g_src, 0, 2, &upload, src[1], 16, 64);
    tex_t t = {0}; t.serial = 12; t.d = td.d;
    p_copy_b2t p = {0}; p.tex = &t; p.src = (uint64_t)(uintptr_t)(g_guest + 16);
    p.r = (NVNcopyRegion){1, 1, 1, 2, 2, 2}; p.row_stride = 12; p.image_stride = 32;
    int prev = g_write_n; rp_op_copy_t2b(c, &p, &p);
    CHECK(g_write_n == prev + 1 && g_unimpl == 0, "3D registration=%d unsupported=%d", g_write_n, g_unimpl);
    rc = nvn_gpu_cmd_commit(c, 2, copy_done, NULL, &e); CHECK(OK(rc), "3D commit: %d %s", rc.code, e.why);
    rc = nvn_gpu_fence_wait(2, 5000000000ull, &e); CHECK(OK(rc), "3D wait: %d %s", rc.code, e.why);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++) {
        uint8_t got = g_guest[16 + z * 32 + y * 12 + x]; uint8_t want = src[z][(y + 1) * 16 + 4 + x];
        CHECK(got == want, "3D z=%d y=%d byte=%d: %u != %u", z, y, x, got, want);
    }
    CHECK(g_guest[15] == 0x11 && g_guest[24] == 0x11 && g_guest[80] == 0x11, "3D padding changed");
    nvn_gpu_buf_release(g_dst, NULL, NULL); nvn_gpu_tex_release(g_src);
}
static void run_bc1(void) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA_DXT1;
    td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e);
    if (!OK(rc)) { CHECK(0, "BC1 image: %d %s", rc.code, e.why); return; }
    memset(g_guest, 0x11, sizeof g_guest); g_dst = make_buf(sizeof g_guest, NVN_GPU_MEM_SHARED, g_guest);
    nvn_gpu_cmd c = begin_cmd(); uint8_t block[8] = {0x3f, 0xf8, 0, 0, 0xe4, 0xe4, 0xe4, 0xe4};
    NVNcopyRegion r = {0, 0, 0, 4, 4, 1}; nvn_gpu_tex_upload(c, g_src, 0, 0, &r, block, 8, 8);
    tex_t t = {0}; t.serial = 11; t.d = td.d;
    p_copy_b2t p = {0}; p.tex = &t; p.src = (uint64_t)(uintptr_t)(g_guest + 32); p.r = r;
    int prev = g_write_n; rp_op_copy_t2b(c, &p, &p);
    CHECK(g_write_n == prev + 1 && g_unimpl == 0, "BC1 registration=%d unsupported=%d", g_write_n, g_unimpl);
    rc = nvn_gpu_cmd_commit(c, 3, copy_done, NULL, &e); CHECK(OK(rc), "BC1 commit: %d %s", rc.code, e.why);
    rc = nvn_gpu_fence_wait(3, 5000000000ull, &e); CHECK(OK(rc), "BC1 wait: %d %s", rc.code, e.why);
    CHECK(!memcmp(g_guest + 32, block, sizeof block) && g_guest[31] == 0x11 && g_guest[40] == 0x11, "BC1 raw block mismatch / overwritten neighbour");
    nvn_gpu_buf_release(g_dst, NULL, NULL); nvn_gpu_tex_release(g_src);
}
int main(void) {
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { printf("device failed: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-replay-t2b-test";
    if (!open_backend()) return 1;
    run_rgba(); run_3d(); run_bc1(); nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close: %d %s", e.rc.code, e.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("%s replay T2B: 64 RGBA bytes and 8 BC1 bytes, failures=%d\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
