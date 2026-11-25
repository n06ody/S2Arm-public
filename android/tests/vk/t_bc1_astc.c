// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真 Vulkan 计算输出压缩块，Arm 解码器和宿主 BC 解码器逐纹素对拍。
#define main backend_fixture_main
#define nvn_plat_flag backend_fixture_flag
#define nvn_plat_log backend_fixture_log
#include "t_backend_vk.c"
#undef nvn_plat_log
#undef nvn_plat_flag
#undef main
#include "astcenc.h"
#include "backend_vulkan_bc1_astc_spv.h"
#include "backend_vulkan_bc7_astc_spv.h"
#include "backend_vulkan_bc7_hybrid_spv.h"
#include "bc7_astc_shaders/inspect.h"
#include "nvn_deswizzle.h"
#include <math.h>
#include "nvn_bcdec.h"

static unsigned bc_timed_dispatches, bc_missing_dispatches;
static struct bc7_counts {
    unsigned long long snapshots, rgba_bytes, bc_bytes, blocks;
} bc7_modes[NVK_BC7_MODES], bc7_only56, bc7_only456;

void nvn_plat_log(int level, const char *category, const char *message) {
    if (!strcmp(category, "bc-upload")) {
        struct bc7_counts b = {0}, *counts = NULL;
        char bucket[24];
        unsigned mode;
        if (sscanf(message, "bc7-images eligible=%23s snapshots=%llu rgba_bytes=%llu bc_bytes=%llu",
                   bucket, &b.snapshots, &b.rgba_bytes, &b.bc_bytes) == 4) {
            if (!strcmp(bucket, "only5_6")) counts = &bc7_only56;
            if (!strcmp(bucket, "only4_5_6")) counts = &bc7_only456;
        } else if (sscanf(message, "bc7-mode mode=%u blocks=%llu snapshots=%llu rgba_bytes=%llu bc_bytes=%llu",
                          &mode, &b.blocks, &b.snapshots, &b.rgba_bytes, &b.bc_bytes) == 5 && mode < NVK_BC7_MODES) {
            counts = &bc7_modes[mode];
        }
        if (counts) {
            counts->snapshots += b.snapshots;
            counts->bc_bytes += b.bc_bytes;
            counts->rgba_bytes += b.rgba_bytes;
            counts->blocks += b.blocks;
        }
        if (strstr(message, "dispatch path=rgba8 format=0x42 blocks=1 ") == message) {
            if (strstr(message, "gpu_ns=missing")) bc_missing_dispatches++;
            else if (strstr(message, "gpu_ns=")) bc_timed_dispatches++;
        }
    }
    backend_fixture_log(level, category, message);
}

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_bc_upload_time")) return 1;
    if (!strcmp(name, "nvk_bc1_astc") || !strcmp(name, "nvk_bc7_astc")) return 1;
    return backend_fixture_flag(name, dflt);
}

enum { BLOCK_X = 64, BLOCK_Y = 64, BLOCKS = BLOCK_X * BLOCK_Y,
       WIDTH = BLOCK_X * 4, HEIGHT = BLOCK_Y * 4, PIXELS = WIDTH * HEIGHT,
       MAX_COLOR_ERROR = 2 };
static uint32_t source[BLOCKS][2];
static uint8_t expected[PIXELS * 4], pixels[PIXELS * 4];

static uint32_t random_word(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void fill_opaque(void) {
    uint32_t state = 0x12345678u;
    for (unsigned i = 0; i < BLOCKS; i++) {
        const uint32_t endpoints = random_word(&state);
        uint32_t c0 = endpoints & 0xffffu, c1 = endpoints >> 16;
        if ((i & 1) && c0 < c1) { const uint32_t tmp = c0; c0 = c1; c1 = tmp; }
        else if (!(i & 1) && c0 > c1) { const uint32_t tmp = c0; c0 = c1; c1 = tmp; }
        uint32_t selector = random_word(&state);
        if (c0 <= c1) selector &= ~((selector & (selector >> 1)) & 0x55555555u);
        source[i][0] = c0 | (c1 << 16);
        source[i][1] = selector;
    }
    source[0][0] = 0x12341234u;
    source[0][1] = 0xa4a4a4a4u;   // 相同端点加中间色
    source[1][0] = 0x07e0f800u;
    source[1][1] = 0xe4e4e4e4u;   // 四色各索引依次出现
}

static void sample_comparison(const uint8_t *astc, enum astcenc_profile profile) {
    struct astcenc_config config;
    struct astcenc_context *ctx = NULL;
    enum astcenc_error e = astcenc_config_init(profile, 4, 4, 1, ASTCENC_PRE_FASTEST,
                                                ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (e == ASTCENC_SUCCESS) e = astcenc_context_alloc(&config, 1, &ctx, NULL);
    CHECK(e == ASTCENC_SUCCESS, "BC1 ASTC reference decoder init: %s", astcenc_get_error_string(e));
    if (!ctx) return;
    void *planes[] = {pixels};
    const struct astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    struct astcenc_image image = {.dim_x = WIDTH, .dim_y = HEIGHT, .dim_z = 1, .data_type = ASTCENC_TYPE_U8, .data = planes};
    e = astcenc_decompress_image(ctx, astc, BLOCKS * 16, &image, &swizzle, 0);
    CHECK(e == ASTCENC_SUCCESS, "BC1 ASTC reference decoder: %s", astcenc_get_error_string(e));
    unsigned invalid = 0;
    for (unsigned i = 0; i < BLOCKS; i++) {
        struct astcenc_block_info info;
        if (astcenc_get_block_info(ctx, astc + i * 16, &info) != ASTCENC_SUCCESS || info.is_error_block) invalid++;
    }
    CHECK(invalid == 0, "BC1 ASTC opaque invalid blocks=%u", invalid);
    unsigned max_delta = 0, rgb_bad = 0, alpha_bad = 0;
    if (e == ASTCENC_SUCCESS) for (unsigned p = 0; p < PIXELS; p++) {
        for (unsigned c = 0; c < 3; c++) {
            unsigned delta = (unsigned)abs((int)pixels[p * 4 + c] - (int)expected[p * 4 + c]);
            if (delta > max_delta) max_delta = delta;
            if (delta > MAX_COLOR_ERROR) rgb_bad++;
        }
        if (pixels[p * 4 + 3] != expected[p * 4 + 3]) alpha_bad++;
    }
    printf("BC1 ASTC profile=%d blocks=%u pixels=%u max_rgb_error=%u rgb_bad=%u alpha_bad=%u\n",
           (int)profile, BLOCKS, PIXELS, max_delta, rgb_bad, alpha_bad);
    CHECK(rgb_bad == 0 && alpha_bad == 0, "BC1 ASTC decoded opaque texels must match BC1");
    astcenc_context_free(ctx);
}

static int encode(nvn_gpu_pipe pipe, unsigned nblocks, size_t source_bytes, nvn_gpu_buf in, nvn_gpu_buf out, uint64_t serial) {
    nvn_gpu_cmd cmd = begin_cmd();
    nvn_gpu_binding b[2] = {
        {.ent = 0, .cls = NVN_GPU_RES_SSBO, .u.b = {in, 0, source_bytes}},
        {.ent = 1, .cls = NVN_GPU_RES_SSBO, .u.b = {out, 0, (BLOCKS + 1) * 16}},
    };
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, b, 2);
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &nblocks, sizeof nblocks);
    nvn_gpu_dispatch(cmd, pipe, (nblocks + 63) / 64, 1, 1, NULL, 0);
    nvn_gpu_err e = {0};
    const int ok = OK(nvn_gpu_cmd_commit(cmd, serial, on_done, NULL, &e)) &&
                   OK(nvn_gpu_fence_wait(serial, 5000000000ull, &e));
    CHECK(ok, "BC1 ASTC fixture submit: %s", e.why);
    return ok;
}

// 同版宿主无 ASTC，仍可通过实际纹理入口核 RGBA8 回落路径与原压缩字节。
static void transparent_fallback(void) {
    static const uint8_t opaque[8] = {0x00, 0xf8, 0x1f, 0, 0, 0, 0, 0};
    static const uint8_t transparent[8] = {0xe0, 0x07, 0x00, 0xf8, 0xff, 0xff, 0xff, 0xff};
    const uint32_t bit = 1u << (NVN_FORMAT_RGBA_DXT1 - NVN_FORMAT_RGB_DXT1);
    const uint32_t old_mask = g_dev.bc_decode_mask;
    const VkBool32 old_astc = g_dev.core_features.textureCompressionASTC_LDR;
    const VkBool32 old_bc = g_dev.core_features.textureCompressionBC;
    g_dev.bc_decode_mask |= bit;
    // 能力负控只删设备事实。打开开关、给不透明提示，缺 ASTC 时仍须走 RGBA8。
    g_dev.core_features.textureCompressionASTC_LDR = VK_FALSE;
    g_dev.core_features.textureCompressionBC = VK_FALSE;
    nvn_gpu_tex tex = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1,
        NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_BC1_OPAQUE);
    g_dev.core_features.textureCompressionASTC_LDR = old_astc;
    g_dev.core_features.textureCompressionBC = old_bc;
    nvn_gpu_tex rgba = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf update = make_buf(8, NVN_GPU_MEM_SHARED, transparent);
    nvn_gpu_buf result = make_buf(72, NVN_GPU_MEM_SHARED, NULL);
    if (tex && rgba && update && result) {
        nvn_gpu_cmd cmd = begin_cmd();
        nvn_gpu_tex_upload(cmd, tex, 0, 0, NULL, opaque, 8, 8);
        nvn_gpu_copy_tex(cmd, tex, 0, 0, NULL, rgba, 0, 0, NULL);
        nvn_gpu_copy_tex_buf(cmd, rgba, 0, 0, NULL, result, 0, 16, 64);
        nvn_gpu_copy_buf_tex(cmd, update, 0, 8, 8, tex, 0, 0, NULL);
        nvn_gpu_copy_tex(cmd, tex, 0, 0, NULL, rgba, 0, 0, NULL);
        nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, result, 64, 8, 8);
        nvn_gpu_buf pixels_after = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
        if (pixels_after) nvn_gpu_copy_tex_buf(cmd, rgba, 0, 0, NULL, pixels_after, 0, 16, 64);
        nvn_gpu_err e = {0};
        int ok = OK(nvn_gpu_cmd_commit(cmd, 3, on_done, NULL, &e)) &&
                 OK(nvn_gpu_fence_wait(3, 5000000000ull, &e));
        CHECK(ok, "BC1 transparent fallback submit: %s", e.why);
        if (ok && pixels_after) {
            const uint8_t red[4] = {255, 0, 0, 255}, black[4] = {0};
            unsigned bad = 0;
            for (unsigned i = 0; i < 16; i++) {
                bad += memcmp(buf_cpu(result) + i * 4, red, 4) != 0;
                bad += memcmp(buf_cpu(pixels_after) + i * 4, black, 4) != 0;
            }
            CHECK(bad == 0, "BC1 opaque to transparent fallback mismatch=%u", bad);
            CHECK(!memcmp(buf_cpu(result) + 64, transparent, 8), "BC1 compressed copy must preserve original block");
        }
        if (pixels_after) nvn_gpu_buf_release(pixels_after, NULL, NULL);
        nvn_gpu_cmd aborted = begin_cmd();
        nvn_gpu_tex_upload(aborted, tex, 0, 0, NULL, opaque, 8, 8);
        nvn_gpu_cmd_abort(aborted);
        CHECK(bc_timed_dispatches == 2 && bc_missing_dispatches == 1,
              "BC1 RGBA8 per-dispatch timestamps expected=2 and missing=1 got=%u missing=%u",
              bc_timed_dispatches, bc_missing_dispatches);
    }
    if (tex) nvn_gpu_tex_release(tex);
    if (rgba) nvn_gpu_tex_release(rgba);
    if (update) nvn_gpu_buf_release(update, NULL, NULL);
    if (result) nvn_gpu_buf_release(result, NULL, NULL);
    g_dev.bc_decode_mask = old_mask;
}

#include "t_bc7_astc.inc"
#include "t_bc7_hybrid.inc"

int main(int argc, char **argv) {
    const int hybrid = argc >= 2 && !strcmp(argv[1], "--bc7-hybrid");
    const int bc7 = hybrid || (argc >= 2 && !strcmp(argv[1], "--bc7"));
    if (argc > 2 + bc7) return 2;
    const char *shader_dir = argc == 2 + bc7 ? argv[1 + bc7] : NULL;
    char why[256];
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) {
        fprintf(stderr, "BC1 ASTC fixture device: %s\n", why);
        return 2;
    }
    printf("device: %s ASTC_LDR=%d\n", g_dev.properties.deviceName, g_dev.core_features.textureCompressionASTC_LDR);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "bc1-astc-encoder";
    if (!open_backend()) return 2;

    const uint32_t cs = 1u << NVN_GPU_SH_CS;
    const nvn_gpu_layout_ent entries[] = {
        {.binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = cs},
        {.binding = 1, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_RW, .count = 1, .stages = cs},
    };
    const nvn_gpu_layout layout = {.ent = entries, .nent = 2, .push_bytes = 4};
    nvn_gpu_err pipe_err = {0};
    nvn_gpu_shader shader = NULL;
    nvn_gpu_shader_src src = {
        .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .entry = "main",
        .blob = bc7 ? nvk_bc7_astc_spv : nvk_bc1_astc_spv,
        .bytes = bc7 ? sizeof nvk_bc7_astc_spv : sizeof nvk_bc1_astc_spv, .layout = &layout,
    };
    if (hybrid) {
        src.blob = nvk_bc7_hybrid_spv;
        src.bytes = sizeof nvk_bc7_hybrid_spv;
    }
    if (shader_dir) {
        shader = load_one_shader(shader_dir, "encode.comp", NVN_GPU_SH_CS, &layout);
    } else {
        CHECK(OK(nvn_gpu_shader_load(&src, &shader, &pipe_err)), "BC1 shader: %s", pipe_err.why);
    }
    if (!shader) return 2;
    nvn_gpu_pipe pipe = NULL;
    const nvn_gpu_stage_desc stage = {.sh = shader};
    CHECK(OK(nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &pipe, &pipe_err)), "BC1 pipeline: %s", pipe_err.why);
    if (!pipe) return 2;

    if (bc7) {
        if (hybrid) hybrid_codec(pipe);
        else {
            bc7_codec(pipe);
            if (!shader_dir) bc7_storage();
        }
        goto close;
    }

    fill_opaque();
    CHECK(nvn_bc_decode(NVN_FORMAT_RGBA_DXT1, (const uint8_t *)source, BLOCK_X * 8,
                        WIDTH, HEIGHT, expected, WIDTH * 4) == 0, "BC1 reference decoding");
    nvn_gpu_buf in = make_buf(sizeof source, NVN_GPU_MEM_SHARED, source);
    nvn_gpu_buf out = make_buf((BLOCKS + 1) * 16, NVN_GPU_MEM_SHARED, NULL);
    if (!in || !out) return 2;
    memset(buf_cpu(out), 0, (BLOCKS + 1) * 16);
    if (!encode(pipe, BLOCKS, sizeof source, in, out, 1)) return 2;
    const uint8_t *data = buf_cpu(out);
    CHECK(*(const uint32_t *)data == 0, "Opaque aggregate must allow ASTC publication");
    sample_comparison(data + 16, ASTCENC_PRF_LDR);
    sample_comparison(data + 16, ASTCENC_PRF_LDR_SRGB);

    // 同一图像的最后一个块才含透明索引。检测位须汇总全部工作组。
    uint32_t *words = (uint32_t *)buf_cpu(in);
    words[2 * BLOCKS - 2] = 0xf80007e0u;
    words[2 * BLOCKS - 1] = 0xc0000000u;
    memset(buf_cpu(out), 0, (BLOCKS + 1) * 16);
    if (!encode(pipe, BLOCKS, sizeof source, in, out, 2)) return 2;
    CHECK(*(const uint32_t *)data == 1, "BC1 transparent block must mark the texture for RGBA8");

    transparent_fallback();

    nvn_gpu_buf_release(out, NULL, NULL);
    nvn_gpu_buf_release(in, NULL, NULL);
close:
    nvn_gpu_shader_release(shader);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "BC1 ASTC fixture close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "BC1 ASTC fixture device destroy");
    if (bc7 && !hybrid && !shader_dir) {
        CHECK(bc7_only56.snapshots == 2 && bc7_only56.rgba_bytes == 320 && bc7_only56.bc_bytes == 96,
              "BC7 diagnosis must count compatible complete snapshots and logical bytes");
        CHECK(bc7_only456.snapshots == 3 && bc7_only456.rgba_bytes == 480 && bc7_only456.bc_bytes == 144,
              "BC7 diagnosis must include the mode 4 mip but exclude truncated storage");
        // L0 含 mode 6 和 5。L1 在第一次快照是 mode 4，随后两次是 mode 6。
        CHECK(bc7_modes[4].blocks == 1 && bc7_modes[4].snapshots == 1 &&
              bc7_modes[5].blocks == 3 && bc7_modes[5].snapshots == 3 &&
              bc7_modes[6].blocks == 5 && bc7_modes[6].snapshots == 3,
              "BC7 mode histogram: mode4=%llu/%llu mode5=%llu/%llu mode6=%llu/%llu (blocks/snapshots)",
              bc7_modes[4].blocks, bc7_modes[4].snapshots, bc7_modes[5].blocks, bc7_modes[5].snapshots,
              bc7_modes[6].blocks, bc7_modes[6].snapshots);
    }
    printf("BC%d ASTC shader failures=%d\n", bc7 ? 7 : 1, g_fail);
    return g_fail ? 1 : 0;
}
