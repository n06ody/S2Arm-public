// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// ReadTexels 读 GPU 内容（graphics/nvn/replay/rp_read_texels.inc 的 rt_gpu）接真后端 + lavapipe，始终开 validation 与同步校验。
// 旁路段的开 / 交、取贴图、提交锁、上一个栅栏以受控替身代（核心里的这几件由 t_nvn_replay 的替身后端用例覆盖）；拷贝、提交、等完、格式判断都是真的。
// 期望值：上传进去的字节 / 通道清成的颜色 / 上传的 BC1 原块，不经被测实现算。
#define main backend_fixture_main
#define nvn_plat_flag read_texels_fixture_flag
#include "t_backend_vk.c"
#undef nvn_plat_flag
#undef main
#include "rp_exec.h"
#include "rp_res.h"
#include "nvn_plat.h"

int nvn_plat_flag(const char *name, int def) {
    if (!strcmp(name, "nvk_land_readback")) return 1;
    return read_texels_fixture_flag(name, def);
}

int s2a_nvk_land_readback_enabled(void);
int s2a_nvk_land_readback_record(nvn_gpu_cmd cmd, nvn_gpu_buf_copy *copies, uint32_t count);
const uint8_t *s2a_nvk_land_readback_map(nvn_gpu_cmd cmd);

static nvn_gpu_tex g_src; static uint64_t g_sig, g_last; static int g_side_begins, g_side_commits, g_locks, g_unlocks, g_in_sub;
static char g_un_last[64]; static int g_un_n;
uint64_t nvn_be_last_fence(void) { return g_last; }
int rp_in_submit(void) { return g_in_sub; }
nvn_gpu_cmd rp_side_begin(const char *label) { (void)label; g_side_begins++; return begin_cmd(); }
static void side_done(void *u, const nvn_gpu_done *d) { (void)u; if (d->status != NVN_GPU_DONE_OK) g_fail++; }
uint64_t rp_side_commit(nvn_gpu_cmd c) {
    nvn_gpu_err e = {0}; const uint64_t f = ++g_sig; g_side_commits++;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, f, side_done, NULL, &e); CHECK(OK(rc), "side commit: %d %s", rc.code, e.why);
    g_last = f; return OK(rc) ? f : 0;
}
nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c) { (void)t; (void)c; return g_src; }
void nvn_hle_submit_lock(void) { g_locks++; }
void nvn_hle_submit_unlock(void) { g_unlocks++; }
uint64_t nvn_unimpl_count(const char *cls, const char *name) { (void)cls; (void)name; return 0; }
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) { (void)site; printf("unimplemented %s/%s\n", cls, name); snprintf(g_un_last, sizeof g_un_last, "%s", name); return ++g_un_n; }
#include "rp_read_texels.inc"

static uint64_t commit_wait(nvn_gpu_cmd c, const char *what) {   // 夹具自己准备内容的段（上传 / 通道清色）：与旁路段共用一条递增的提交值
    nvn_gpu_err e = {0}; const uint64_t f = ++g_sig;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, f, side_done, NULL, &e); CHECK(OK(rc), "%s commit: %d %s", what, rc.code, e.why);
    rc = nvn_gpu_fence_wait(f, 5000000000ull, &e); CHECK(OK(rc), "%s wait: %d %s", what, rc.code, e.why);
    g_last = f; return f;
}
static tex_t fake_tex(const nvn_gpu_tex_desc *td) { tex_t t; memset(&t, 0, sizeof t); t.serial = 90; t.d = td->d; nvn_tex_layout(&t.d, &t.L); return t; }
static void run_array_upload(void) {   // 二维数组 4×4×3：上传第 1、2 层 → 读第 1、2 层的 (1,1) 起 2×2，行距 16、片距 48（带 pad）
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4; td.d.depth = 3; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e); if (!OK(rc)) { CHECK(0, "array image: %d %s", rc.code, e.why); return; }
    uint8_t src[2][64]; for (int z = 0; z < 2; z++) for (int i = 0; i < 64; i++) src[z][i] = (uint8_t)(0x20 + z * 0x40 + i);
    nvn_gpu_cmd c = begin_cmd(); NVNcopyRegion up = {0, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(c, g_src, 0, 1, &up, src[0], 16, 64); nvn_gpu_tex_upload(c, g_src, 0, 2, &up, src[1], 16, 64); commit_wait(c, "array upload");
    tex_t t = fake_tex(&td); uint8_t dst[160]; memset(dst, 0xEE, sizeof dst);
    NVNcopyRegion r = {1, 1, 0, 2, 2, 2};
    const int b0 = g_side_begins, c0 = g_side_commits;
    rt_gpu(&t, 0, 1, 2, &r, 8, 2, 1, 1, 4, 4, dst, 16, 48);
    CHECK(g_side_begins == b0 + 1 && g_side_commits == c0 + 1 && g_locks == g_unlocks && g_un_n == 0, "array: one side segment, lock balanced, no unimpl (%s)", g_un_last);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++)
        CHECK(dst[z * 48 + y * 16 + x] == src[z][(y + 1) * 16 + 4 + x], "array z=%d y=%d byte=%d: %u != %u", z, y, x, dst[z * 48 + y * 16 + x], src[z][(y + 1) * 16 + 4 + x]);
    CHECK(dst[8] == 0xEE && dst[15] == 0xEE && dst[32] == 0xEE && dst[96] == 0xEE, "array: padding / slice gap untouched");
    nvn_gpu_tex_release(g_src);
}
static void run_clear(void) {   // GPU 写的内容：8×8 RGBA8 当渲染目标、通道清成 (1, 0, 51/255, 1) → 读回每个纹素 ff 00 33 ff
    g_src = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(g_src);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[1] = 0.0f; p.color[0].clear.f[2] = 51.0f / 255.0f; p.color[0].clear.f[3] = 1.0f; p.w = 8; p.h = 8; p.layers = 1;
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c); commit_wait(c, "clear");
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 8; td.d.depth = td.d.levels = 1;
    tex_t t = fake_tex(&td); uint8_t dst[256]; memset(dst, 0xEE, sizeof dst);
    NVNcopyRegion r = {0, 0, 0, 8, 8, 1};
    rt_gpu(&t, 0, 0, 1, &r, 32, 8, 1, 1, 8, 8, dst, 32, 256);
    int bad = 0; for (int i = 0; i < 64; i++) bad += dst[i * 4] != 0xff || dst[i * 4 + 1] != 0 || dst[i * 4 + 2] != 0x33 || dst[i * 4 + 3] != 0xff;
    CHECK(!bad && g_un_n == 0, "clear: %d texels differ from ff 00 33 ff (first %02x %02x %02x %02x)", bad, dst[0], dst[1], dst[2], dst[3]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(g_src);
}
static void run_3d(void) {   // 三维 4×4×4：上传第 1、2 片 → 读这两片整片（copy_tex_buf 的 layer 在三维上是片号）
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = td.d.depth = 4; td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e); if (!OK(rc)) { CHECK(0, "3D image: %d %s", rc.code, e.why); return; }
    uint8_t src[2][64]; for (int z = 0; z < 2; z++) for (int i = 0; i < 64; i++) src[z][i] = (uint8_t)(0x30 + z * 0x40 + i);
    nvn_gpu_cmd c = begin_cmd(); NVNcopyRegion up = {0, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(c, g_src, 0, 1, &up, src[0], 16, 64); nvn_gpu_tex_upload(c, g_src, 0, 2, &up, src[1], 16, 64); commit_wait(c, "3D upload");
    tex_t t = fake_tex(&td); uint8_t dst[128]; memset(dst, 0xEE, sizeof dst);
    NVNcopyRegion r = {0, 0, 1, 4, 4, 2};
    rt_gpu(&t, 0, 1, 2, &r, 16, 4, 1, 1, 4, 4, dst, 16, 64);
    CHECK(!memcmp(dst, src[0], 64) && !memcmp(dst + 64, src[1], 64) && g_un_n == 0, "3D slices 1..2 read back the uploaded bytes");
    nvn_gpu_tex_release(g_src);
}
static void run_bc1(void) {   // BC1 4×4：上传一个块 → 读回就是这个块的 8 字节（不是解码后的像素）
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA_DXT1; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e); if (!OK(rc)) { CHECK(0, "BC1 image: %d %s", rc.code, e.why); return; }
    uint8_t block[8] = {0x3f, 0xf8, 0, 0, 0xe4, 0xe4, 0xe4, 0xe4};
    nvn_gpu_cmd c = begin_cmd(); NVNcopyRegion r = {0, 0, 0, 4, 4, 1}; nvn_gpu_tex_upload(c, g_src, 0, 0, &r, block, 8, 8); commit_wait(c, "BC1 upload");
    tex_t t = fake_tex(&td); uint8_t dst[24]; memset(dst, 0xEE, sizeof dst);
    rt_gpu(&t, 0, 0, 1, &r, 8, 1, 4, 4, 4, 4, dst, 8, 8);
    CHECK(!memcmp(dst, block, 8) && dst[8] == 0xEE && g_un_n == 0, "BC1 raw block read back");
    nvn_gpu_tex_release(g_src);
}
static void run_formats(void) {   // 后端的 READBACK 位：拷得出 NVN 字节的有，深度模板合一 / D24 没有；没有的格式一段都不开，时间线照常
    CHECK(nvn_gpu_format_caps(NVN_FORMAT_RGBA8) & NVN_GPU_FMT_READBACK, "RGBA8 has READBACK");
    CHECK(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_READBACK, "BC1 has READBACK");
    CHECK(!(nvn_gpu_format_caps(NVN_FORMAT_DEPTH24_STENCIL8) & NVN_GPU_FMT_READBACK), "D24S8 has no READBACK (depth face only)");
    CHECK(!(nvn_gpu_format_caps(NVN_FORMAT_DEPTH24) & NVN_GPU_FMT_READBACK), "D24 has no READBACK (upper 8 bits undefined)");
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_DEPTH24_STENCIL8; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
    tex_t t = fake_tex(&td); uint8_t dst[64]; memset(dst, 0xEE, sizeof dst); NVNcopyRegion r = {0, 0, 0, 4, 4, 1};
    const int b0 = g_side_begins, n0 = g_un_n;
    rt_gpu(&t, 0, 0, 1, &r, 16, 4, 1, 1, 4, 4, dst, 16, 64);
    int untouched = 1; for (size_t i = 0; i < sizeof dst; i++) untouched &= dst[i] == 0xEE;
    CHECK(g_side_begins == b0 && g_un_n == n0 + 1 && !strcmp(g_un_last, "read_texels_gpu_format") && untouched && nvn_gpu_fence_failed() == 0,
          "D24S8 refused before any segment (%s), dst untouched, timeline healthy", g_un_last);
    g_un_n = n0;
}

struct land_case {
    nvn_gpu_cmd cmd;
    uint8_t bytes[72];
    int mapped;
};

static void land_done(void *user, const nvn_gpu_done *done) {
    struct land_case *test = user;
    if (done->status != NVN_GPU_DONE_OK) return;
    const uint8_t *mapped = s2a_nvk_land_readback_map(test->cmd);
    if (!mapped) return;
    memcpy(test->bytes, mapped, sizeof test->bytes);
    test->mapped = 1;
}

static void run_land_readback(void) {
    CHECK(s2a_nvk_land_readback_enabled(), "land readback cached type available");
    if (!s2a_nvk_land_readback_enabled()) return;
    nvn_gpu_err err = {0};
    nvn_gpu_buf mirror = NULL;
    CHECK(OK(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, NULL, &mirror, &err)), "land mirror: %s", err.why);
    if (!mirror) return;
    struct land_case cases[3] = {0};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        nvn_gpu_cmd c = cases[i].cmd = begin_cmd();
        nvn_gpu_fill_buf(c, mirror, 0, 128, (0x22 + i) * 0x01010101);
        nvn_gpu_fill_buf(c, mirror, 128, 128, (0x33 + i) * 0x01010101);
        nvn_gpu_buf_copy ranges[2] = {{.src=mirror, .soff=3, .bytes=65},
                                      {.src=mirror, .soff=129, .bytes=7}};
        CHECK(s2a_nvk_land_readback_record(c, ranges, 2) && ranges[0].doff == 0 && ranges[1].doff == 65,
              "land copy ranges recorded in distinct slots");
    }
    // Reserving occurs before submission and must outlive both recording
    // and fence retirement. A fourth command may not alias a reserved slot.
    nvn_gpu_cmd extra = begin_cmd();
    nvn_gpu_buf_copy range = {.src=mirror, .soff=3, .bytes=65};
    CHECK(!s2a_nvk_land_readback_record(extra, &range, 1), "land readback refuses a busy slot");
    nvn_gpu_cmd_abort(extra);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const uint64_t f = ++g_sig;
        CHECK(OK(nvn_gpu_cmd_commit(cases[i].cmd, f, land_done, &cases[i], &err)), "land submit: %s", err.why);
    }
    CHECK(OK(nvn_gpu_fence_wait(g_sig, 5000000000ull, &err)), "land wait: %s", err.why);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t expected[72];
        memset(expected, 0x22 + i, 65);
        memset(expected + 65, 0x33 + i, 7);
        CHECK(cases[i].mapped && !memcmp(cases[i].bytes, expected, sizeof expected),
              "land exact bytes from command %u", i);
    }
    extra = begin_cmd();
    CHECK(s2a_nvk_land_readback_record(extra, &range, 1), "land readback reuses a retired slot");
    nvn_gpu_cmd_abort(extra);
    nvn_gpu_buf_release(mirror, NULL, NULL);
}

int main(void) {
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { printf("device failed: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-replay-read-texels-test";
    if (!open_backend()) return 1;
    run_array_upload(); run_clear(); run_3d(); run_bc1(); run_formats();
    uint64_t st[4]; nvn_be_read_texels_gpu_stats(st);
    CHECK(st[0] == 4 && g_locks == g_unlocks && g_locks == 4, "4 reads counted (%llu), locks %d/%d (the D24S8 refusal happens before the lock)", (unsigned long long)st[0], g_locks, g_unlocks);
    run_land_readback();
    nvn_gpu_err e = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close: %d %s", e.rc.code, e.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("%s replay ReadTexels GPU: array / clear / 3D / BC1 / formats / land readback, failures=%d\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
