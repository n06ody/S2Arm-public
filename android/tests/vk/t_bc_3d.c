// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实 NVN Vulkan 后端建图、两条上传路线、shader 采样和原块读回。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "nvn_bcdec.h"

enum {
    BC_WIDTH = 7, BC_HEIGHT = 5, BC_DEPTH = 4, BC_LEVELS = 3,
    BC_PIXELS = 7 * 5 * 4 + 3 * 2 * 2 + 1, BC_SLICES = 4 + 2 + 1,
    BC_MAX_ROW = 3 * 16, BC_SHADOW_SLICE = 2 * BC_MAX_ROW,
    BC_GPU_OFFSET = 16, BC_STAGING_BYTES = BC_DEPTH * BC_SHADOW_SLICE,
    BC_SHADOW_BYTES = BC_SLICES * BC_SHADOW_SLICE, SRGB_CODES = 256
};

struct bc_level {
    unsigned w, h, d, cols, rows;
    _Alignas(16) uint8_t blocks[BC_DEPTH][2 * 2 * 16];
};

static void bc_fill(uint8_t *p, unsigned n, uint32_t rng) {
    for (unsigned i = 0; i < n; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        p[i] = (uint8_t)rng;
    }
}

static nvn_gpu_pipe bc_sample_pipe(const char *dir, nvn_gpu_shader *shader) {
    const nvn_gpu_layout_ent ents[] = {
        {.cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R, .binding = 0, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .binding = 1, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
    };
    const nvn_gpu_layout lay = {.ent = ents, .nent = 2};
    return mk_cpipe(dir, "bc_volume.comp", &lay, shader);
}

static float bc_half(uint16_t h) {
    const unsigned exponent = (h >> 10) & 31, fraction = h & 1023;
    float magnitude = exponent == 31 ? (fraction ? NAN : INFINITY)
                      : !exponent ? ldexpf((float)fraction, -24)
                      : ldexpf((float)(1024 + fraction), (int)exponent - 25);
    return (h & 0x8000u) ? -magnitude : magnitude;
}

static int bc_is_srgb(uint32_t fmt) {
    return (fmt >= NVN_FORMAT_RGB_DXT1_SRGB && fmt <= NVN_FORMAT_RGBA_DXT5_SRGB) || fmt == NVN_FORMAT_BPTC_UNORM_SRGB;
}

static float bc_channel(uint32_t fmt, const uint8_t *texel, unsigned channel, int bpp, const float srgb[SRGB_CODES][4]) {
    if (bpp == 8) {
        uint16_t bits;
        memcpy(&bits, texel + channel * 2, sizeof bits);
        return bc_half(bits);
    }
    if (channel >= (unsigned)bpp ||
        (channel == 3 && (fmt == NVN_FORMAT_RGB_DXT1 || fmt == NVN_FORMAT_RGB_DXT1_SRGB)))
        return channel == 3 ? 1.0f : 0.0f;
    if (fmt == NVN_FORMAT_RGTC1_SNORM || fmt == NVN_FORMAT_RGTC2_SNORM)
        return fmaxf(-1.0f, (float)(int8_t)texel[channel] / 127.0f);
    if (channel < 3 && bc_is_srgb(fmt)) return srgb[texel[channel]][channel];
    return (float)texel[channel] / 255.0f;
}

static void bc_sample(nvn_gpu_cmd c, nvn_gpu_view view, nvn_gpu_pipe pipe, nvn_gpu_samp samp, nvn_gpu_buf pixels, unsigned count) {
    const nvn_gpu_binding bindings[] = {
        {.ent = 0, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {.view = view, .samp = samp}},
        {.ent = 1, .cls = NVN_GPU_RES_SSBO, .u.b = {.buf = pixels, .len = count * 4 * sizeof(float)}},
    };
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bindings, 2);
    nvn_gpu_dispatch(c, pipe, (count + 31) / 32, 1, 1, NULL, 0);
}

static int bc_wait(nvn_gpu_cmd c, uint64_t signal) {
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, signal, on_done, NULL, &err);
    if (OK(rc)) rc = nvn_gpu_fence_wait(signal, 5000000000ull, &err);
    CHECK(OK(rc), "BC3D submit %llu: %d %s", (unsigned long long)signal, rc.code, err.why);
    return OK(rc);
}

// CPU 解码给出编码字节。用原生 sRGB 图采样已知的 256 个码值作参考，隔离驱动的 sRGB 近似，
// 保持解码像素的码值逐个可区分（放宽浮点容差会把较暗的相邻码值合并）。
static int bc_srgb_reference(nvn_gpu_pipe pipe, nvn_gpu_samp samp, float srgb[SRGB_CODES][4], uint64_t signal) {
    nvn_gpu_tex t = make_tex_3d_levels(16, 16, 1, 1, NVN_FORMAT_RGBA8_SRGB, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = NULL;
    nvn_gpu_buf pixels = NULL;
    int ok = 0;
    if (!t) return 0;
    const view_t view = {.set_target = 1, .target = NVN_TEXTURE_TARGET_3D};
    nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_view_create(t, &view, &v, &e)) && v, "BC3D native sRGB view: %s", e.why);
    if (!v) goto done;
    uint8_t ramp[SRGB_CODES][4];
    for (unsigned i = 0; i < SRGB_CODES; i++) memset(ramp[i], (int)i, sizeof ramp[i]);
    pixels = make_buf(SRGB_CODES * 4 * sizeof(float), NVN_GPU_MEM_SHARED, NULL);
    if (!pixels) goto done;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, ramp, 16 * 4, sizeof ramp);
    bc_sample(c, v, pipe, samp, pixels, SRGB_CODES);
    if (!bc_wait(c, signal)) goto done;
    memcpy(srgb, buf_cpu(pixels), SRGB_CODES * 4 * sizeof(float));
    ok = srgb[0][0] == 0.0f && srgb[255][0] == 1.0f && srgb[128][0] > 0.21f && srgb[128][0] < 0.22f &&
         fabsf(srgb[128][3] - 128.0f / 255.0f) < 0.000001f;
    for (unsigned i = 1; i < SRGB_CODES; i++)
        for (unsigned ch = 0; ch < 3; ch++) ok &= srgb[i][ch] > srgb[i - 1][ch];
    CHECK(ok, "BC3D native sRGB ramp endpoints, midpoint, alpha or ordering");
    printf("BC3D native sRGB ramp codes=%d code230=%g\n", SRGB_CODES, srgb[230][0]);

done:
    nvn_gpu_buf_release(pixels, NULL, NULL);
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    return ok;
}

static int bc_capture(nvn_gpu_cmd c, nvn_gpu_tex tex, nvn_gpu_view view, nvn_gpu_pipe pipe, nvn_gpu_samp samp,
                      nvn_gpu_buf pixels, nvn_gpu_buf shadow, const struct bc_level *mips, unsigned bb, uint64_t signal) {
    unsigned slice = 0;
    for (unsigned m = 0; m < BC_LEVELS; m++) {
        const struct bc_level *l = &mips[m];
        for (unsigned z = 0; z < l->d; z++, slice++)
            nvn_gpu_copy_tex_buf(c, tex, m, z, NULL, shadow, slice * BC_SHADOW_SLICE,
                                 (l->cols + 1) * bb, BC_SHADOW_SLICE);
    }
    bc_sample(c, view, pipe, samp, pixels, BC_PIXELS);
    return bc_wait(c, signal);
}

static int bc_compare(uint32_t fmt, const char *phase, const struct bc_level *mips, unsigned bb,
                      nvn_gpu_buf pixels, nvn_gpu_buf shadow, const float srgb[SRGB_CODES][4]) {
    float got[BC_PIXELS][4];
    memcpy(got, buf_cpu(pixels), sizeof got);
    const uint8_t *raw = buf_cpu(shadow);
    uint32_t pf;
    int bpp = 0;
    if (!nvn_bc_unpacked(fmt, &pf, &bpp)) { CHECK(0, "BC3D CPU reference format 0x%x", fmt); return 0; }
    unsigned slice = 0, texel = 0, pixel_bad = 0, shadow_bad = 0;
    for (unsigned m = 0; m < BC_LEVELS; m++) {
        const struct bc_level *l = &mips[m];
        unsigned packed = l->cols * bb, row = (l->cols + 1) * bb;
        for (unsigned z = 0; z < l->d; z++, slice++) {
            _Alignas(16) uint8_t decoded[BC_WIDTH * BC_HEIGHT * 8];
            if (nvn_bc_decode(fmt, l->blocks[z], packed, l->w, l->h, decoded, l->w * bpp)) {
                CHECK(0, "BC3D CPU decode 0x%x mip=%u z=%u", fmt, m, z);
                return 0;
            }
            for (unsigned i = 0; i < l->w * l->h; i++, texel++) {
                for (unsigned ch = 0; ch < 4; ch++) {
                    const float want = bc_channel(fmt, decoded + i * bpp, ch, bpp, srgb);
                    const float actual = got[texel][ch];
                    const int near = bpp == 8 ? (isnan(want) && isnan(actual))
                                   : (!bc_is_srgb(fmt) || ch == 3) && fabsf(actual - want) < 0.000001f;
                    if (actual == want || near) continue;
                    if (!pixel_bad) printf("BC3D first sample mismatch fmt=0x%x %s mip=%u z=%u pixel=%u ch=%u got=%g want=%g\n",
                                           fmt, phase, m, z, i, ch, actual, want);
                    pixel_bad++;
                    break;
                }
            }
            for (unsigned i = 0; i < BC_SHADOW_SLICE; i++) {
                const uint8_t want = i / row < l->rows && i % row < packed
                                   ? l->blocks[z][(i / row) * packed + i % row] : 0x5a;
                if (raw[slice * BC_SHADOW_SLICE + i] != want) shadow_bad++;
            }
        }
    }
    printf("BC3D fmt=0x%x %s pixels=%u/%u shadow=%u/%u\n",
           fmt, phase, texel - pixel_bad, texel, BC_SHADOW_BYTES - shadow_bad, BC_SHADOW_BYTES);
    CHECK(!pixel_bad && !shadow_bad, "BC3D fmt=0x%x %s sample mismatches=%u shadow mismatches=%u",
          fmt, phase, pixel_bad, shadow_bad);
    return !pixel_bad && !shadow_bad;
}

static int bc_run(uint32_t fmt, nvn_gpu_pipe pipe, nvn_gpu_samp sampler, const float srgb[SRGB_CODES][4], uint64_t *signal) {
    nvn_gpu_tex_desc desc = {.d = {.target = NVN_TEXTURE_TARGET_3D, .format = fmt, .width = BC_WIDTH,
                                  .height = BC_HEIGHT, .depth = BC_DEPTH, .levels = BC_LEVELS},
                            .usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD};
    nvn_gpu_tex tex = NULL;
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_tex_create(&desc, &tex, &err);
    CHECK(OK(rc) && tex, "BC3D fmt=0x%x texture create: %d %s", fmt, rc.code, err.why);
    if (!tex) return 0;
    nvn_gpu_view view = make_view(tex);
    uint8_t seed[BC_SHADOW_BYTES];
    float sample_seed[BC_PIXELS][4];
    memset(seed, 0x5a, sizeof seed);
    for (unsigned i = 0; i < BC_PIXELS; i++)
        for (unsigned ch = 0; ch < 4; ch++) sample_seed[i][ch] = -12345.0f;
    nvn_gpu_buf pixels = make_buf(sizeof sample_seed, NVN_GPU_MEM_SHARED, sample_seed);
    nvn_gpu_buf shadow = make_buf(sizeof seed, NVN_GPU_MEM_SHARED, seed);
    nvn_gpu_buf gpu_src = make_buf(BC_STAGING_BYTES + BC_GPU_OFFSET, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf upload[BC_LEVELS] = {0};
    int passed = 0;
    if (!view || !pixels || !shadow || !gpu_src) goto done;

    unsigned bb = fmt == NVN_FORMAT_RGB_DXT1 || fmt == NVN_FORMAT_RGBA_DXT1 ||
                  fmt == NVN_FORMAT_RGB_DXT1_SRGB || fmt == NVN_FORMAT_RGBA_DXT1_SRGB ||
                  fmt == NVN_FORMAT_RGTC1_UNORM || fmt == NVN_FORMAT_RGTC1_SNORM ? 8 : 16;
    struct bc_level mips[BC_LEVELS] = {{.w = 7, .h = 5, .d = 4, .cols = 2, .rows = 2},
                                     {.w = 3, .h = 2, .d = 2, .cols = 1, .rows = 1},
                                     {.w = 1, .h = 1, .d = 1, .cols = 1, .rows = 1}};
    nvn_gpu_cmd c = begin_cmd();
    for (unsigned m = 0; m < BC_LEVELS; m++) {
        struct bc_level *l = &mips[m];
        uint8_t src[BC_STAGING_BYTES];
        memset(src, 0xcc, sizeof src);
        const unsigned packed = l->cols * bb, pitch = packed + bb;
        for (unsigned z = 0; z < l->d; z++) {
            bc_fill(l->blocks[z], packed * l->rows, 0x7390a511u ^ (fmt << 8) ^ (m << 4) ^ z);
            for (unsigned y = 0; y < l->rows; y++)
                memcpy(src + (z * l->rows + y) * pitch, l->blocks[z] + y * packed, packed);
        }
        nvn_gpu_tex_upload(c, tex, m, 0, NULL, src, pitch, l->d * l->rows * pitch - bb);
    }
    if (!bc_capture(c, tex, view, pipe, sampler, pixels, shadow, mips, bb, ++*signal)) goto done;
    int cpu_ok = bc_compare(fmt, "CPU", mips, bb, pixels, shadow, srgb);
    c = begin_cmd();
    nvn_gpu_buf_write(c, pixels, 0, sample_seed, sizeof sample_seed);
    for (unsigned m = 0; m < BC_LEVELS; m++) {
        struct bc_level *l = &mips[m];
        const unsigned bx = l->cols - 1, z0 = l->d > 1 ? 1 : 0, n = l->d - z0, pitch = 2 * bb;
        uint8_t src[BC_STAGING_BYTES];
        memset(src, 0xdd, sizeof src);
        for (unsigned z = 0; z < n; z++) for (unsigned y = 0; y < l->rows; y++) {
            uint8_t *block = l->blocks[z + z0] + (y * l->cols + bx) * bb;
            bc_fill(block, bb, 0xfa31b704u ^ (fmt << 12) ^ (m << 8) ^ (z << 4) ^ y);
            memcpy(src + (z * l->rows + y) * pitch, block, bb);
        }
        const unsigned bytes = n * l->rows * pitch - bb;
        upload[m] = make_buf(bytes, NVN_GPU_MEM_SHARED, src);
        if (!upload[m]) { nvn_gpu_cmd_abort(c); goto done; }
        nvn_gpu_copy_buf(c, upload[m], 0, gpu_src, BC_GPU_OFFSET, bytes);
        // 缓冲拷贝的 layer 是 3D 起始切片，CPU 直传才用区域的 zoffset。
        const NVNcopyRegion region = {4 * bx, 0, 0, l->w - 4 * bx, l->h, n};
        nvn_gpu_copy_buf_tex(c, gpu_src, BC_GPU_OFFSET, pitch, bytes, tex, m, z0, &region);
    }
    if (bc_capture(c, tex, view, pipe, sampler, pixels, shadow, mips, bb, ++*signal))
        passed = bc_compare(fmt, "GPU", mips, bb, pixels, shadow, srgb) && cpu_ok;

done:
    for (unsigned m = 0; m < BC_LEVELS; m++) nvn_gpu_buf_release(upload[m], NULL, NULL);
    nvn_gpu_buf_release(gpu_src, NULL, NULL);
    nvn_gpu_buf_release(shadow, NULL, NULL);
    nvn_gpu_buf_release(pixels, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    return passed;
}

static void bc_reject_unsupported_uses(uint32_t fmt) {
    for (unsigned use = 0; use < 3; use++) {
        nvn_gpu_tex_desc desc = {.d = {.target = NVN_TEXTURE_TARGET_3D, .format = fmt,
                                      .width = 7, .height = 5, .depth = 4, .levels = 1},
                                .usage = NVN_GPU_TEX_SAMPLED};
        if (use == 0) desc.usage |= NVN_GPU_TEX_RENDER;
        if (use == 1) desc.usage |= NVN_GPU_TEX_STORAGE;
        if (use == 2) desc.d.samples = 2;
        nvn_gpu_tex tex = NULL;
        nvn_gpu_err e = {0};
        nvn_gpu_rc rc = nvn_gpu_tex_create(&desc, &tex, &e);
        CHECK(rc.code == NVN_GPU_E_UNSUPPORTED && !tex, "BC3D fmt=0x%x negative use=%u: %d %s", fmt, use, rc.code, e.why);
        nvn_gpu_tex_release(tex);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char why[192] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "BC3D device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    CHECK(g_dev.bc_decode_forced && g_dev.bc_decode_mask == 0xffffu, "BC3D needs forced 16-format decode");
    g_desc.host_device = &g_dev;
    g_desc.app_label = "nvn-bc-3d-test";
    if (!open_backend()) return 1;
    nvn_gpu_shader shader = NULL;
    nvn_gpu_pipe pipe = bc_sample_pipe(argv[1], &shader);
    nvn_gpu_samp sampler = NULL;
    samp_par sp = {0};
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_samp_create(&sp, &sampler, &err)) && sampler, "BC3D sampler: %s", err.why);
    unsigned passed = 0;
    uint64_t signal = 0;
    float srgb[SRGB_CODES][4];
    const int sample_ready = pipe && sampler && bc_srgb_reference(pipe, sampler, srgb, ++signal);
    for (uint32_t f = NVN_FORMAT_RGB_DXT1; f <= NVN_FORMAT_BPTC_UFLOAT; f++) {
        if (sample_ready) passed += bc_run(f, pipe, sampler, srgb, &signal);
        bc_reject_unsupported_uses(f);
    }
    nvn_gpu_samp_release(sampler);
    nvn_gpu_shader_release(shader);
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "BC3D close: %s", err.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("BC3D formats=%u/16 failures=%d\n", passed, g_fail);
    return passed != 16 || g_fail ? 1 : 0;
}
