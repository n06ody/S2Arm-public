// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

static uint32_t bc7_source[BLOCKS][4];
static float bc7_pixels[PIXELS * 4];

static void bc7_fill(void) {
    uint32_t rng = 0x69a75b05u;
    for (unsigned b = 0; b < BLOCKS; b++) {
        const unsigned mode = (b & 1) ? 5 : 6;
        const unsigned rotation = (b >> 1) & 3;
        uint8_t *block = (uint8_t *)bc7_source[b];
        do {
            for (unsigned w = 0; w < 4; w++) bc7_source[b][w] = random_word(&rng);
            block[0] = mode == 6 ? (block[0] & 0x80u) | 0x40u : (uint8_t)(0x20u | (rotation << 6));
        } while (!nvk_bc7_astc_block_eligible(block, mode));
    }
    // 两平面、所有旋转及首索引/末索引均由随机组覆盖。两个端值另用确定样本。
    memset(bc7_source[0], 0, sizeof bc7_source[0]);
    bc7_source[0][0] = 0x40; // 透明 Mode 6
    memset(bc7_source[1], 0, sizeof bc7_source[1]);
    bc7_source[1][0] = 0x20; // 不透明 Mode 5，两个 scalar 端点均是 255
    bc7_source[1][1] = 0xffffu << 18;
    bc7_source[1][2] = 3;
}

static void bc7_compare(const uint8_t *blocks) {
    struct astcenc_config config;
    struct astcenc_context *context = NULL;
    enum astcenc_error e = astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST,
                                               ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (e == ASTCENC_SUCCESS) e = astcenc_context_alloc(&config, 1, &context, NULL);
    CHECK(e == ASTCENC_SUCCESS, "BC7 astcenc setup: %s", astcenc_get_error_string(e));
    if (e != ASTCENC_SUCCESS) return;

    void *planes[] = {bc7_pixels};
    struct astcenc_image image = {.dim_x = WIDTH, .dim_y = HEIGHT, .dim_z = 1, .data_type = ASTCENC_TYPE_F32, .data = planes};
    const struct astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    e = astcenc_decompress_image(context, blocks, BLOCKS * 16, &image, &swizzle, 0);
    CHECK(e == ASTCENC_SUCCESS, "BC7 astcenc decode: %s", astcenc_get_error_string(e));
    unsigned invalid = 0;
    for (unsigned b = 0; b < BLOCKS; b++) {
        struct astcenc_block_info info;
        invalid += astcenc_get_block_info(context, blocks + b * 16, &info) != ASTCENC_SUCCESS || info.is_error_block;
    }
    CHECK(invalid == 0, "BC7 invalid ASTC blocks=%u", invalid);
    unsigned channel_bad[2] = {0}, alpha_bad = 0;
    float maximum[2] = {0};
    if (e == ASTCENC_SUCCESS) for (unsigned p = 0; p < PIXELS; p++) {
        const unsigned b = ((p / WIDTH) / 4) * BLOCK_X + ((p % WIDTH) / 4);
        const unsigned m = b & 1;
        for (unsigned c = 0; c < 4; c++) {
            const float decoded = bc7_pixels[p * 4 + c];
            const unsigned bc = expected[p * 4 + c];
            const float delta = fabsf(decoded * 255 - (float)bc);
            if (delta > maximum[m]) maximum[m] = delta;
            if (!isfinite(delta) || delta > (m ? 4.0f : 8.0f)) channel_bad[m]++;
            if (c == 3 && ((decoded == 0) != (bc == 0) || (decoded == 1) != (bc == 255) ||
                           (decoded < 0.5f) != (bc < 128))) alpha_bad++;
        }
    }
    printf("BC7 ASTC blocks=%u max_mode6=%.5f/255 max_mode5=%.5f/255 alpha_bad=%u\n",
           BLOCKS, maximum[0], maximum[1], alpha_bad);
    CHECK(channel_bad[0] == 0 && channel_bad[1] == 0, "BC7 channel error limit: mode6=%u mode5=%u",
          channel_bad[0], channel_bad[1]);
    CHECK(alpha_bad == 0, "BC7 alpha endpoint/coverage preservation: bad=%u", alpha_bad);
    astcenc_context_free(context);
}

static void bc7_codec(nvn_gpu_pipe pipe) {
    bc7_fill();
    CHECK(nvk_bc7_astc_block_eligible((const uint8_t *)bc7_source[1], 5), "BC7 opaque source must be eligible");
    CHECK(nvn_bc_decode(NVN_FORMAT_BPTC_UNORM, (const uint8_t *)bc7_source, BLOCK_X * 16,
                        WIDTH, HEIGHT, expected, WIDTH * 4) == 0, "BC7 reference decode");
    nvn_gpu_buf in = make_buf(sizeof bc7_source, NVN_GPU_MEM_SHARED, bc7_source);
    nvn_gpu_buf out = make_buf((BLOCKS + 1) * 16, NVN_GPU_MEM_SHARED, NULL);
    if (in && out) {
        memset(buf_cpu(out), 0, (BLOCKS + 1) * 16);
        if (encode(pipe, BLOCKS, sizeof bc7_source, in, out, 1)) {
            const uint8_t *data = buf_cpu(out);
            CHECK(*(const uint32_t *)data == 0, "BC7 supported modes must sum to zero unsupported flags");
            bc7_compare(data + 16);
            buf_cpu(in)[(BLOCKS - 1) * 16] = 0x10;
            if (encode(pipe, BLOCKS, sizeof bc7_source, in, out, 2))
                CHECK(*(const uint32_t *)data == 1, "BC7 last block in unsupported mode must reject whole image");
        }
    }
    if (out) nvn_gpu_buf_release(out, NULL, NULL);
    if (in) nvn_gpu_buf_release(in, NULL, NULL);
}

#ifdef S2A_BC7_ASYNC_TEST
static int bc7_commit_fixture(nvn_gpu_cmd c, uint64_t signal, nvn_gpu_err *e) {
    return OK(nvn_gpu_cmd_commit(c, signal, on_done, NULL, e)) &&
           OK(nvn_gpu_fence_wait(signal, 5000000000ull, e));
}
#endif

static void bc7_storage(void) {
    const uint32_t saved_mask = g_dev.bc_decode_mask;
    const VkBool32 saved_bc = g_dev.core_features.textureCompressionBC;
    const VkBool32 saved_astc = g_dev.core_features.textureCompressionASTC_LDR;
    // Lavapipe 的能力负控只移除特性。遇到有原生 ASTC、无 native BC 的真实设备另核 ASTC 物理存储。
    const int can_astc = saved_astc && !saved_bc;
    g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_BPTC_UNORM - NVN_FORMAT_RGB_DXT1);
    g_dev.core_features.textureCompressionBC = VK_FALSE;
    g_dev.core_features.textureCompressionASTC_LDR = can_astc ? saved_astc : VK_FALSE;
    const nvn_gpu_tex_desc desc = {
        .d = {.width = 8, .height = 4, .depth = 1, .target = NVN_TEXTURE_TARGET_2D,
              .format = NVN_FORMAT_BPTC_UNORM, .levels = 2},
        .usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD,
    };
    nvn_gpu_tex tex = NULL, compressed = NULL, unwrapped = NULL;
    nvn_gpu_view old_view = NULL;
    nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_tex_create(&desc, &tex, &e)), "BC7 storage texture: %s", e.why);
    nvn_texlayout layout;
    nvn_tex_layout(&desc.d, &layout);
    uint8_t *source_storage = calloc(1, (size_t)layout.size);
    nvn_gpu_buf result = make_buf(96, NVN_GPU_MEM_SHARED, NULL);
    if (tex && source_storage && result) {
        nvn_gpu_cmd cmd = begin_cmd();
        static const uint8_t mode4[16] = {0x10};
        uint8_t source[48];
        memcpy(source, bc7_source, sizeof source);
        memset(source + 32, 0, 16);
        source[32] = 0x40;
        CHECK(nvn_swizzle(&desc.d, 0, 0, source, 32, source_storage) == 0, "BC7 source level 0 swizzle");
        CHECK(nvn_swizzle(&desc.d, 1, 0, mode4, 16, source_storage) == 0, "BC7 source level 1 swizzle");
        CHECK(!nvn_gpu_tex_bc7_snapshot(cmd, tex, source_storage, layout.size, 1, 0, &compressed) && !compressed,
              "BC7 diagnostic mip must keep ordinary storage");
        CHECK(nvn_swizzle(&desc.d, 1, 0, source + 32, 16, source_storage) == 0, "BC7 compatible mip swizzle");
        CHECK(!nvn_gpu_tex_bc7_snapshot(cmd, tex, source_storage, layout.size - 1, 1, 1, &compressed) && !compressed,
              "BC7 truncated snapshot must keep ordinary storage");
        CHECK(!nvn_gpu_tex_bc7_snapshot(cmd, tex, source_storage, layout.size, 1, 0, &compressed) && !compressed,
              "BC7 diagnostics-only snapshot must keep ordinary storage");
        int rc = nvn_gpu_tex_bc7_snapshot(cmd, tex, source_storage, layout.size, 1, 1, &compressed);
        CHECK(rc == can_astc && !compressed, "BC7 first upload must remain RGBA8 rc=%d ASTC=%d", rc, can_astc);
        uint64_t signal = 3;
#ifdef S2A_BC7_ASYNC_TEST
        if (can_astc) {
            extern void nvn_vk_test_bc7_present(void);
            CHECK(bc7_commit_fixture(cmd, signal++, &e), "BC7 first RGBA8 upload: %s", e.why);
            // The next ordinary signal also guarantees upload retirement before the first candidate chunk.
            CHECK(bc7_commit_fixture(begin_cmd(), signal++, &e), "BC7 upload retirement: %s", e.why);
            CHECK(!nvn_gpu_tex_bc7_take(tex), "BC7 cannot publish before fitting");
            for (unsigned level = 0; level < 2; level++) {
                nvn_vk_test_bc7_present();
                cmd = begin_cmd();
                nvn_gpu_bc7_step(cmd);
                CHECK(bc7_commit_fixture(cmd, signal++, &e), "BC7 candidate chunk %u: %s", level, e.why);
                CHECK(bc7_commit_fixture(begin_cmd(), signal++, &e), "BC7 candidate retirement: %s", e.why);
                if (level == 0) CHECK(!nvn_gpu_tex_bc7_take(tex), "BC7 incomplete mip chain cannot publish");
            }
            compressed = nvn_gpu_tex_bc7_take(tex);
            CHECK(compressed != NULL, "BC7 complete ASTC image must publish after the natural fence");
            cmd = begin_cmd();
        }
#endif
        if (compressed) {
            CHECK(OK(nvn_gpu_view_create(compressed, NULL, &old_view, &e)), "BC7 old view creation: %s", e.why);
            nvn_gpu_copy_tex_buf(cmd, compressed, 0, 0, NULL, result, 0, 32, 32);
            nvn_gpu_copy_tex_buf(cmd, compressed, 1, 0, NULL, result, 32, 16, 16);
            CHECK(nvn_gpu_tex_bc7_unwrap(cmd, compressed, &unwrapped) == 1 && unwrapped,
                  "BC7 partial write must unwrap using original shadow");
            nvn_gpu_tex_release(compressed);
            compressed = NULL; // 旧 view 与已录命令都仍持有它。
        }
        nvn_gpu_tex target = unwrapped ? unwrapped : tex;
        if (!unwrapped) {
            nvn_gpu_tex_upload(cmd, target, 0, 0, NULL, source, 32, 32);
            nvn_gpu_tex_upload(cmd, target, 1, 0, NULL, source + 32, 16, 16);
            nvn_gpu_copy_tex_buf(cmd, target, 0, 0, NULL, result, 0, 32, 32);
            nvn_gpu_copy_tex_buf(cmd, target, 1, 0, NULL, result, 32, 16, 16);
        }
        const NVNcopyRegion update = {.xoffset = 4, .width = 4, .height = 4, .depth = 1};
        nvn_gpu_tex_upload(cmd, target, 0, 0, &update, mode4, 16, 16);
        nvn_gpu_copy_tex_buf(cmd, target, 0, 0, NULL, result, 48, 32, 32);
        nvn_gpu_copy_tex_buf(cmd, target, 1, 0, NULL, result, 80, 16, 16);
        int ok = OK(nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &e)) &&
                 OK(nvn_gpu_fence_wait(signal, 5000000000ull, &e));
        CHECK(ok, "BC7 storage submit: %s", e.why);
        if (ok) {
            const uint8_t *bytes = buf_cpu(result);
            CHECK(!memcmp(bytes, source, 48), "BC7 original mip blocks must read back byte-for-byte");
            CHECK(!memcmp(bytes + 48, source, 16) && !memcmp(bytes + 64, mode4, 16) &&
                  !memcmp(bytes + 80, source + 32, 16), "BC7 partial update must preserve other block and level");
        }
    }
    if (old_view) nvn_gpu_view_release(old_view);
    if (unwrapped) nvn_gpu_tex_release(unwrapped);
    if (compressed) nvn_gpu_tex_release(compressed);
    if (tex) nvn_gpu_tex_release(tex);
    if (result) nvn_gpu_buf_release(result, NULL, NULL);
    free(source_storage);
    g_dev.bc_decode_mask = saved_mask;
    g_dev.core_features.textureCompressionBC = saved_bc;
    g_dev.core_features.textureCompressionASTC_LDR = saved_astc;
}
