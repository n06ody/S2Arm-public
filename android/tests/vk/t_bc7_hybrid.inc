// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

static void hybrid_field(uint32_t block[4], unsigned at, unsigned bits, unsigned value) {
    for (unsigned i = 0; i < bits; i++)
        if (value & (1u << i)) block[(at + i) / 32] |= 1u << ((at + i) % 32);
}

static void hybrid_fill(void) {
    uint32_t rng = 0x18e6b024u;
    memset(bc7_source, 0, sizeof bc7_source);
    for (unsigned b = 0; b < BLOCKS; b++) {
        const unsigned mode = b % 8;
        if (b >= 16)
            for (unsigned i = 0; i < 4; i++) bc7_source[b][i] = random_word(&rng);
        bc7_source[b][0] &= ~((1u << (mode + 1)) - 1);
        bc7_source[b][0] |= 1u << mode;
    }

    // Mode 3 partition zero has an exact ASTC shape. Distinct subset endpoints
    // and nonzero indices detect a seed error or endpoint reversal.
    uint32_t *m3 = bc7_source[11];
    for (unsigned channel = 0; channel < 3; channel++) {
        hybrid_field(m3, 10 + 28 * channel, 7, 103 - 8 * channel);
        hybrid_field(m3, 17 + 28 * channel, 7, 81 - 8 * channel);
        hybrid_field(m3, 24 + 28 * channel, 7, 16 + 13 * channel);
        hybrid_field(m3, 31 + 28 * channel, 7, 21 + 13 * channel);
    }
    hybrid_field(m3, 98, 30, 0x1ca4aaaau);
    memset(bc7_source[BLOCKS - 1], 0, sizeof bc7_source[0]); // reserved BC7 prefix
}

static void hybrid_compare(const uint8_t *encoded) {
    struct astcenc_config cfg;
    struct astcenc_context *ctx = NULL;
    enum astcenc_error err = astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST,
                                                 ASTCENC_FLG_DECOMPRESS_ONLY, &cfg);
    if (err == ASTCENC_SUCCESS) err = astcenc_context_alloc(&cfg, 1, &ctx, NULL);
    CHECK(err == ASTCENC_SUCCESS, "BC7 hybrid oracle setup: %s", astcenc_get_error_string(err));
    if (err != ASTCENC_SUCCESS) return;
    const struct astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    void *planes[] = {bc7_pixels};
    struct astcenc_image image = {.dim_x = WIDTH, .dim_y = HEIGHT, .dim_z = 1,
                                  .data_type = ASTCENC_TYPE_F32, .data = planes};
    err = astcenc_decompress_image(ctx, encoded, BLOCKS * 16, &image, &swizzle, 0);
    CHECK(err == ASTCENC_SUCCESS, "BC7 hybrid oracle decode: %s", astcenc_get_error_string(err));

    unsigned accepted[8] = {0}, rejected = 0, invalid = 0, max_bad = 0, rms_bad = 0, alpha_bad = 0;
    float linf = 0.f, rms = 0.f, alpha = 0.f;
    const uint8_t zero[16] = {0};
    for (unsigned b = 0; b < BLOCKS && err == ASTCENC_SUCCESS; b++) {
        const uint8_t *block = encoded + b * 16;
        if (!memcmp(block, zero, sizeof zero)) {
            if (b < 16) CHECK(0, "BC7 hybrid known fit rejected mode %u", b % 8);
            if (b >= 16 && b < BLOCKS - 1) rejected++;
            continue;
        }
        accepted[b % 8]++;
        struct astcenc_block_info info;
        if (astcenc_get_block_info(ctx, block, &info) != ASTCENC_SUCCESS || info.is_error_block) {
            invalid++;
            continue;
        }
        if (b == 11) CHECK(info.partition_count == 2 && info.weight_level_count == 4 && info.color_level_count == 40,
                           "BC7 hybrid exact Mode 3 did not use shared two-subset Q40");
        const unsigned base = (b / BLOCK_X) * 4 * WIDTH + (b % BLOCK_X) * 4;
        float squares = 0.f;
        for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++) for (unsigned c = 0; c < 4; c++) {
            const unsigned p = (base + y * WIDTH + x) * 4 + c;
            const float sample = bc7_pixels[p];
            const uint8_t ref = expected[p];
            const float delta = fabsf(sample * 255.f - (float)ref);
            if (c < 3) {
                squares += delta * delta;
                if (delta > linf) linf = delta;
                if (!isfinite(delta) || delta > 8.f) max_bad++;
            } else {
                if (delta > alpha) alpha = delta;
                if (!isfinite(delta) || delta > 4.f || ((sample == 0.f) != (ref == 0)) ||
                    ((sample == 1.f) != (ref == 255)) || ((sample < .5f) != (ref < 128))) alpha_bad++;
            }
        }
        const float block_rms = sqrtf(squares / 48.f);
        if (block_rms > rms) rms = block_rms;
        if (!isfinite(block_rms) || block_rms > 3.f) rms_bad++;
    }
    for (unsigned m = 0; m < 8; m++) CHECK(accepted[m] >= 2, "BC7 hybrid mode %u has too few accepted samples", m);
    CHECK(rejected > 0, "BC7 hybrid no complex blocks were rejected");
    CHECK(!memcmp(encoded + (BLOCKS - 1) * 16, zero, 16), "BC7 hybrid reserved prefix emitted a block");
    CHECK(invalid == 0, "BC7 hybrid invalid ASTC blocks=%u", invalid);
    CHECK(max_bad == 0, "BC7 hybrid RGB L-infinity outliers=%u", max_bad);
    CHECK(rms_bad == 0, "BC7 hybrid RGB RMS outliers=%u", rms_bad);
    CHECK(alpha_bad == 0, "BC7 hybrid alpha outliers=%u", alpha_bad);
    printf("BC7 hybrid accepted=%u/%u/%u/%u/%u/%u/%u/%u rejected=%u RGB max=%.4f RMS=%.4f alpha max=%.4f (units of 1/255)\n",
           accepted[0], accepted[1], accepted[2], accepted[3], accepted[4], accepted[5], accepted[6], accepted[7],
           rejected, linf, rms, alpha);
    astcenc_context_free(ctx);
}

static void hybrid_codec(nvn_gpu_pipe pipe) {
    hybrid_fill();
    CHECK(nvn_bc_decode(NVN_FORMAT_BPTC_UNORM, (const uint8_t *)bc7_source, BLOCK_X * 16,
                        WIDTH, HEIGHT, expected, WIDTH * 4) == 0, "BC7 hybrid reference decode");
    nvn_gpu_buf in = make_buf(sizeof bc7_source, NVN_GPU_MEM_SHARED, bc7_source);
    nvn_gpu_buf out = make_buf((BLOCKS + 1) * 16, NVN_GPU_MEM_SHARED, NULL);
    if (in && out) {
        memset(buf_cpu(out), 0, (BLOCKS + 1) * 16);
        if (encode(pipe, 16, sizeof bc7_source, in, out, 1))
            CHECK(*(const uint32_t *)buf_cpu(out) == 0, "BC7 hybrid known fits must leave fallback clear");
        if (encode(pipe, BLOCKS, sizeof bc7_source, in, out, 2)) {
            const uint8_t *data = buf_cpu(out);
            CHECK(*(const uint32_t *)data == 1, "BC7 hybrid rejected blocks must mark the entire image");
            hybrid_compare(data + 16);
        }
    }
    if (out) nvn_gpu_buf_release(out, NULL, NULL);
    if (in) nvn_gpu_buf_release(in, NULL, NULL);
#ifdef S2A_BC7_ASYNC_TEST
    extern int nvn_vk_test_bc7_completion(int, unsigned, int);
    CHECK(nvn_vk_test_bc7_completion(1, 0, 0) == 1, "BC7 hybrid natural completion must unlock ready storage");
    CHECK(nvn_vk_test_bc7_completion(0, 0, 0) == 0, "BC7 hybrid failed fence must retain RGBA8");
    CHECK(nvn_vk_test_bc7_completion(1, 1, 0) == 0, "BC7 hybrid rejected fit must retain RGBA8");
    CHECK(nvn_vk_test_bc7_completion(1, 0, 1) == 0, "BC7 hybrid stale generation must retain RGBA8");
#endif
}
