// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// BC 解码贴图（t->bc_decoded）跨格式视图契约测试：
// 覆盖 BC1/2/3/7 UNORM <-> SRGB 同族切换、RGBA_DXT1 -> RGB_DXT1_SRGB alpha=1、
// 同存储字节类未压缩视图（RGBA8_SRGB / BGRA8 / R8 / RG8）真实采样像素读回，
// 以及跨族 BC / 改符号 BC / 异字节类 / 缺 VIEW_FMT 的显式拒绝（0 校验层错误）。
#define main backend_fixture_main
#define nvn_plat_flag backend_fixture_flag
#include "t_backend_vk.c"
#undef nvn_plat_flag
#undef main
#include <math.h>

static int g_wrong_view, g_probe_pack;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_bc7_astc_pack") || !strcmp(name, "nvk_bc7_workset_diag")) return g_probe_pack;
    return backend_fixture_flag(name, dflt);
}
static uint64_t g_sig = 100;

static float srgb_to_lin(double u) {
    if (u <= 0.04045) return (float)(u / 12.92);
    return (float)pow((u + 0.055) / 1.055, 2.4);
}

static void bc7_pack_bits(uint8_t dst[16], unsigned *pos, uint32_t val, unsigned nbits) {
    for (unsigned i = 0; i < nbits; i++) {
        if ((val >> i) & 1u) dst[*pos >> 3] |= (uint8_t)(1u << (*pos & 7u));
        (*pos)++;
    }
}

// BC7 Mode 6 单色块：7-bit 端点 + 1-bit p-bit（全 1）→ 精确输出 (133, 131, 133, 255)
static void make_bc7_midgray(uint8_t blk[16]) {
    memset(blk, 0, 16);
    unsigned pos = 0;
    bc7_pack_bits(blk, &pos, 0x40u, 7);   // mode 6
    bc7_pack_bits(blk, &pos, 66u, 7); bc7_pack_bits(blk, &pos, 66u, 7);   // R0, R1
    bc7_pack_bits(blk, &pos, 65u, 7); bc7_pack_bits(blk, &pos, 65u, 7);   // G0, G1
    bc7_pack_bits(blk, &pos, 66u, 7); bc7_pack_bits(blk, &pos, 66u, 7);   // B0, B1
    bc7_pack_bits(blk, &pos, 127u, 7); bc7_pack_bits(blk, &pos, 127u, 7); // A0, A1
    bc7_pack_bits(blk, &pos, 1u, 1); bc7_pack_bits(blk, &pos, 1u, 1);     // P0=1, P1=1
    // 63-bit indices all 0 -> every texel picks endpoint 0 = (133, 131, 133, 255)
}

static nvn_gpu_pipe make_sample_pipe(const char *dir, nvn_gpu_shader *out_sh) {
    nvn_gpu_layout_ent ents[3];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX_SAMP; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO;     ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    ents[2].set = 0; ents[2].cls = NVN_GPU_RES_SSBO;     ents[2].access = NVN_GPU_ACC_W; ents[2].binding = 2; ents[2].count = 1; ents[2].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl = { .ent = ents, .nent = 3 };
    return mk_cpipe(dir, "samp_at.comp", &cl, out_sh);
}

static int sample_view_once(nvn_gpu_pipe cp, nvn_gpu_view v, nvn_gpu_samp samp, float out_rgba[4]) {
    const float uv[2] = { 0.5f, 0.5f };
    nvn_gpu_buf in = make_buf(sizeof uv, NVN_GPU_MEM_SHARED, uv);
    nvn_gpu_buf out = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    if (!in || !out) return 0;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[3];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX_SAMP; bs[0].u.ts.view = v; bs[0].u.ts.samp = samp;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO;     bs[1].u.b.buf = in;  bs[1].u.b.len = sizeof uv;
    bs[2].ent = 2; bs[2].cls = NVN_GPU_RES_SSBO;     bs[2].u.b.buf = out; bs[2].u.b.len = 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 3);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_err e = {0};
    uint64_t sig = ++g_sig;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    int ok = OK(r) && OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e));
    CHECK(ok, "sample_view_once submit/wait: %s", e.why);
    if (ok) memcpy(out_rgba, buf_cpu(out), 16);
    nvn_gpu_buf_release(in, NULL, NULL);
    nvn_gpu_buf_release(out, NULL, NULL);
    return ok;
}

static int near_f(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static nvn_gpu_tex create_and_upload_bc(uint32_t fmt, uint32_t usage, const void *block, size_t block_bytes) {
    nvn_gpu_tex t = make_tex(4, 4, fmt, usage);
    if (!t) return NULL;
    nvn_gpu_cmd c = begin_cmd();
    const int probe_bc7 = g_probe_pack && (fmt == NVN_FORMAT_BPTC_UNORM || fmt == NVN_FORMAT_BPTC_UNORM_SRGB);
    nvn_gpu_buf shadow = NULL;
    int cached = 0;
    if (probe_bc7) {
        const nvn_texdesc desc = {.target = NVN_TEXTURE_TARGET_2D, .format = (int)fmt,
                                 .width = 4, .height = 4, .depth = 1, .levels = 1};
        nvn_texlayout layout;
        nvn_tex_layout(&desc, &layout);
        uint8_t *source = calloc(1, (size_t)layout.size);
        CHECK(source && layout.size >= block_bytes, "BC7 complete block-linear source");
        if (source && layout.size >= block_bytes) {
            memcpy(source, block, block_bytes);
            nvn_gpu_tex replacement = NULL;
            if (fmt == NVN_FORMAT_BPTC_UNORM) {
                CHECK(!nvn_gpu_tex_bc7_snapshot(c, t, NULL, 0, NVN_GPU_BC7_PARTIAL, &replacement) && !replacement,
                      "BC7 partial observation must not replace storage");
                CHECK(!nvn_gpu_tex_bc7_snapshot(c, t, NULL, 0, NVN_GPU_BC7_SPARSE, &replacement) && !replacement,
                      "BC7 sparse observation must not replace storage");
            }
            cached = nvn_gpu_tex_bc7_snapshot(c, t, source, layout.size, NVN_GPU_BC7_FULL, &replacement);
            const int can_use_astc = g_dev.core_features.textureCompressionASTC_LDR &&
                                    !g_dev.core_features.textureCompressionBC;
            CHECK(cached == can_use_astc && !!replacement == can_use_astc,
                  "BC7 pack promotion expected=%d actual=%d", can_use_astc, cached);
            if (replacement) {
                nvn_gpu_tex_release(t);
                t = replacement;
            }
        }
        free(source);
        shadow = make_buf(block_bytes, NVN_GPU_MEM_SHARED, NULL);
    }
    if (!cached) nvn_gpu_tex_upload(c, t, 0, 0, NULL, block, block_bytes, block_bytes);
    if (shadow) nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, shadow, 0, block_bytes, block_bytes);
    nvn_gpu_err e = {0};
    uint64_t sig = ++g_sig;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    int ready = OK(r) && OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e));
    CHECK(ready, "upload bc fmt=0x%x: %s", fmt, e.why);
    if (shadow) {
        if (ready) {
            const void *bytes = buf_cpu(shadow);
            CHECK(bytes && !memcmp(bytes, block, block_bytes), "BC7 original-block shadow preserved");
            printf("BC7-PACK format=%#x storage=%s\n", fmt, cached > 0 ? "ASTC" : "RGBA8");
        }
        nvn_gpu_buf_release(shadow, NULL, NULL);
    }
    return t;
}

static void check_pair(nvn_gpu_pipe cp, nvn_gpu_samp samp, const char *label,
                       uint32_t tex_fmt, uint32_t alt_fmt, int tex_is_srgb,
                       const void *block, size_t block_bytes,
                       uint8_t r8, uint8_t g8, uint8_t b8, uint8_t a8, int allow_wrong_view) {
    nvn_gpu_tex t = create_and_upload_bc(tex_fmt, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_VIEW_FMT,
                                         block, block_bytes);
    if (!t) return;
    nvn_gpu_view v_def = NULL, v_alt = NULL;
    nvn_gpu_err e = {0};
    nvn_gpu_rc r_def = nvn_gpu_view_create(t, NULL, &v_def, &e);
    CHECK(OK(r_def) && v_def, "%s default view: %d %s", label, r_def.code, e.why);
    view_t av = {0}; av.set_format = 1; av.format = (int)alt_fmt;
    nvn_gpu_rc r_alt = nvn_gpu_view_create(t, &av, &v_alt, &e);
    CHECK(OK(r_alt) && v_alt, "%s alt view (0x%x -> 0x%x): %d %s", label, tex_fmt, alt_fmt, r_alt.code, e.why);
    if (v_def && v_alt) {
        float got_def[4] = {0}, got_alt[4] = {0};
        const float exp_lin[4] = { r8 / 255.0f, g8 / 255.0f, b8 / 255.0f, a8 / 255.0f };
        const float exp_srgb[4] = { srgb_to_lin(r8 / 255.0), srgb_to_lin(g8 / 255.0), srgb_to_lin(b8 / 255.0), a8 / 255.0f };
        const float *want_def = tex_is_srgb ? exp_srgb : exp_lin;
        const float *want_alt = tex_is_srgb ? exp_lin : exp_srgb;
        if (sample_view_once(cp, v_def, samp, got_def)) {
            CHECK(near_f(got_def[0], want_def[0], 0.015f) && near_f(got_def[1], want_def[1], 0.015f) &&
                  near_f(got_def[2], want_def[2], 0.015f) && near_f(got_def[3], want_def[3], 0.015f),
                  "%s default sample got=(%.4f,%.4f,%.4f,%.4f) want=(%.4f,%.4f,%.4f,%.4f)",
                  label, got_def[0], got_def[1], got_def[2], got_def[3],
                  want_def[0], want_def[1], want_def[2], want_def[3]);
        }
        nvn_gpu_view probe_view = (allow_wrong_view && g_wrong_view) ? v_def : v_alt;
        if (sample_view_once(cp, probe_view, samp, got_alt)) {
            CHECK(near_f(got_alt[0], want_alt[0], 0.015f) && near_f(got_alt[1], want_alt[1], 0.015f) &&
                  near_f(got_alt[2], want_alt[2], 0.015f) && near_f(got_alt[3], want_alt[3], 0.015f),
                  "%s alt sample got=(%.4f,%.4f,%.4f,%.4f) want=(%.4f,%.4f,%.4f,%.4f)",
                  label, got_alt[0], got_alt[1], got_alt[2], got_alt[3],
                  want_alt[0], want_alt[1], want_alt[2], want_alt[3]);
            printf("BC-ALT-VIEW %s def=(%.3f,%.3f,%.3f,%.3f) alt=(%.3f,%.3f,%.3f,%.3f)\n",
                   label, got_def[0], got_def[1], got_def[2], got_def[3],
                   got_alt[0], got_alt[1], got_alt[2], got_alt[3]);
        }
    }
    if (v_alt) nvn_gpu_view_release(v_alt);
    if (v_def) nvn_gpu_view_release(v_def);
    nvn_gpu_tex_release(t);
}

static void expect_reject(nvn_gpu_tex t, uint32_t view_fmt, const char *why_case) {
    view_t av = {0}; av.set_format = 1; av.format = (int)view_fmt;
    nvn_gpu_view v = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_view_create(t, &av, &v, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED && v == NULL,
          "reject %s (view 0x%x): expected UNSUPPORTED, got rc=%d why=%s",
          why_case, view_fmt, r.code, e.why);
    if (v) nvn_gpu_view_release(v);
}

static void check_widened_bc7(void) {
    const nvn_gpu_tex_desc desc = {
        .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_BPTC_UNORM,
              .width = 4, .height = 4, .depth = 1, .levels = 1},
        .usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD,
        .host_w = 8,
    };
    nvn_gpu_err err = {0};
    nvn_gpu_tex tex = NULL;
    const nvn_gpu_rc rc = nvn_gpu_tex_create(&desc, &tex, &err);
    CHECK(rc.code == NVN_GPU_E_UNSUPPORTED && !tex &&
          strstr(err.why, "压缩格式") && strstr(err.why, "不支持宽屏宿主尺寸"),
          "widened BC7 must reject before snapshot: rc=%d why=%s", rc.code, err.why);
    if (tex) nvn_gpu_tex_release(tex);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[2], "--wrong-view")) g_wrong_view = 1;
    else if (argc == 3 && !strcmp(argv[2], "--packed")) {
        g_probe_pack = 1;
        if (setenv("S2A_SHADER_REVISION_ROOT", argv[1], 1)) return 2;
    } else if (argc != 2) return 64;

    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why) != 0) {
        fprintf(stderr, "create Vulkan device: %s\n", why);
        return 2;
    }
    printf("device: %s (%s) bc_decode_mask=0x%x\n",
           g_dev.properties.deviceName, g_dev.driver_name, g_dev.bc_decode_mask);
    CHECK(g_dev.bc_decode_mask == 0xffffu, "S2A_VK_BC_DECODE=force must enable all 16 BC decode bits");

    g_desc.host_device = &g_dev;
    g_desc.app_label = "bc-decoded-alt-view";
    if (!open_backend()) return 2;

    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = make_sample_pipe(argv[1], &sh);
    nvn_gpu_samp samp = NULL;
    {
        nvn_gpu_err e = {0};
        samp_par sp;
        memset(&sp, 0, sizeof sp);   // nearest / clamp
        CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)) && samp, "create sampler: %s", e.why);
    }

    if (cp && samp) {
        // 1. BC1/BC2/BC3/BC7 UNORM <-> SRGB 同族双向采样像素读回
        static const uint8_t bc1_mid[8] = { 0x10, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // (132,130,132,255)
        static const uint8_t bc2_mid[16] = {
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0x10, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        static const uint8_t bc3_mid[16] = {
            0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x10, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        uint8_t bc7_mid[16]; make_bc7_midgray(bc7_mid); // (133,131,133,255)
        if (g_probe_pack) check_widened_bc7();

        check_pair(cp, samp, "BC1_RGBA_UNORM->SRGB", NVN_FORMAT_RGBA_DXT1, NVN_FORMAT_RGBA_DXT1_SRGB, 0, bc1_mid, 8, 132, 130, 132, 255, 1);
        check_pair(cp, samp, "BC1_RGBA_SRGB->UNORM", NVN_FORMAT_RGBA_DXT1_SRGB, NVN_FORMAT_RGBA_DXT1, 1, bc1_mid, 8, 132, 130, 132, 255, 0);
        check_pair(cp, samp, "BC1_RGB_UNORM->SRGB",  NVN_FORMAT_RGB_DXT1, NVN_FORMAT_RGB_DXT1_SRGB, 0, bc1_mid, 8, 132, 130, 132, 255, 0);
        check_pair(cp, samp, "BC1_RGB_SRGB->UNORM",  NVN_FORMAT_RGB_DXT1_SRGB, NVN_FORMAT_RGB_DXT1, 1, bc1_mid, 8, 132, 130, 132, 255, 0);
        check_pair(cp, samp, "BC2_UNORM->SRGB",      NVN_FORMAT_RGBA_DXT3, NVN_FORMAT_RGBA_DXT3_SRGB, 0, bc2_mid, 16, 132, 130, 132, 255, 0);
        check_pair(cp, samp, "BC2_SRGB->UNORM",      NVN_FORMAT_RGBA_DXT3_SRGB, NVN_FORMAT_RGBA_DXT3, 1, bc2_mid, 16, 132, 130, 132, 255, 0);
        check_pair(cp, samp, "BC3_UNORM->SRGB",      NVN_FORMAT_RGBA_DXT5, NVN_FORMAT_RGBA_DXT5_SRGB, 0, bc3_mid, 16, 132, 130, 132, 255, 0);
        check_pair(cp, samp, "BC3_SRGB->UNORM",      NVN_FORMAT_RGBA_DXT5_SRGB, NVN_FORMAT_RGBA_DXT5, 1, bc3_mid, 16, 132, 130, 132, 255, 0);
        check_pair(cp, samp, "BC7_UNORM->SRGB",      NVN_FORMAT_BPTC_UNORM, NVN_FORMAT_BPTC_UNORM_SRGB, 0, bc7_mid, 16, 133, 131, 133, 255, 0);
        check_pair(cp, samp, "BC7_SRGB->UNORM",      NVN_FORMAT_BPTC_UNORM_SRGB, NVN_FORMAT_BPTC_UNORM, 1, bc7_mid, 16, 133, 131, 133, 255, 0);

        // 2. RGBA_DXT1 透明块经 RGB_DXT1_SRGB 视图采样：alpha 必须钉为 1
        {
            static const uint8_t bc1_trans[8] = { 0x00, 0x00, 0x00, 0xf8, 0xff, 0xff, 0xff, 0xff };
            nvn_gpu_tex t = create_and_upload_bc(NVN_FORMAT_RGBA_DXT1,
                NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_VIEW_FMT, bc1_trans, 8);
            if (t) {
                nvn_gpu_view v_def = NULL, v_rgb = NULL; nvn_gpu_err e = {0};
                view_t av = {0}; av.set_format = 1; av.format = NVN_FORMAT_RGB_DXT1_SRGB;
                CHECK(OK(nvn_gpu_view_create(t, NULL, &v_def, &e)) && v_def, "BC1_trans def: %s", e.why);
                CHECK(OK(nvn_gpu_view_create(t, &av, &v_rgb, &e)) && v_rgb, "BC1_trans -> RGB_DXT1_SRGB: %s", e.why);
                if (v_def && v_rgb) {
                    float a[4] = {0}, b[4] = {0};
                    if (sample_view_once(cp, v_def, samp, a) && sample_view_once(cp, v_rgb, samp, b)) {
                        CHECK(near_f(a[3], 0.0f, 0.01f), "RGBA_DXT1 punch-through alpha expected 0 got %.3f", a[3]);
                        CHECK(near_f(b[3], 1.0f, 0.01f), "RGB_DXT1_SRGB view alpha expected 1 got %.3f", b[3]);
                    }
                }
                if (v_rgb) nvn_gpu_view_release(v_rgb);
                if (v_def) nvn_gpu_view_release(v_def);
                nvn_gpu_tex_release(t);
            }
        }

        // 3. 兼容字节类未压缩视图（RGBA8_SRGB、BGRA8 通道互换、BC4->R8、BC5->RG8）
        {
            check_pair(cp, samp, "BC1_RGBA->RGBA8_SRGB", NVN_FORMAT_RGBA_DXT1, NVN_FORMAT_RGBA8_SRGB, 0, bc1_mid, 8, 132, 130, 132, 255, 0);

            static const uint8_t bc1_red[8] = { 0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // (255,0,0,255)
            nvn_gpu_tex tr = create_and_upload_bc(NVN_FORMAT_RGBA_DXT1,
                NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_VIEW_FMT, bc1_red, 8);
            if (tr) {
                view_t av = {0}; av.set_format = 1; av.format = NVN_FORMAT_BGRA8;
                nvn_gpu_view vb = NULL; nvn_gpu_err e = {0};
                CHECK(OK(nvn_gpu_view_create(tr, &av, &vb, &e)) && vb, "BC1_RGBA -> BGRA8: %s", e.why);
                if (vb) {
                    float g[4] = {0};
                    if (sample_view_once(cp, vb, samp, g))
                        CHECK(near_f(g[0], 0.0f, 0.01f) && near_f(g[1], 0.0f, 0.01f) &&
                              near_f(g[2], 1.0f, 0.01f) && near_f(g[3], 1.0f, 0.01f),
                              "BC1_RGBA red via BGRA8 view got=(%.3f,%.3f,%.3f,%.3f)", g[0], g[1], g[2], g[3]);
                    nvn_gpu_view_release(vb);
                }
                nvn_gpu_tex_release(tr);
            }

            static const uint8_t bc4_blk[8] = { 132, 132, 0, 0, 0, 0, 0, 0 };
            nvn_gpu_tex t4 = create_and_upload_bc(NVN_FORMAT_RGTC1_UNORM,
                NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_VIEW_FMT, bc4_blk, 8);
            if (t4) {
                view_t av = {0}; av.set_format = 1; av.format = NVN_FORMAT_R8;
                nvn_gpu_view vr = NULL; nvn_gpu_err e = {0};
                CHECK(OK(nvn_gpu_view_create(t4, &av, &vr, &e)) && vr, "BC4 -> R8: %s", e.why);
                if (vr) {
                    float g[4] = {0};
                    if (sample_view_once(cp, vr, samp, g))
                        CHECK(near_f(g[0], 132 / 255.0f, 0.015f), "BC4 via R8 view got=%.4f", g[0]);
                    nvn_gpu_view_release(vr);
                }
                // 负控：改符号 BC4 SNORM 与异字节类必须拒收
                expect_reject(t4, NVN_FORMAT_RGTC1_SNORM, "BC4_UNORM->BC4_SNORM");
                expect_reject(t4, NVN_FORMAT_RGTC2_UNORM, "BC4_UNORM->BC5_UNORM");
                expect_reject(t4, NVN_FORMAT_RGBA8,       "BC4_UNORM(1B)->RGBA8(4B)");
                expect_reject(t4, NVN_FORMAT_RG8,         "BC4_UNORM(1B)->RG8(2B)");
                nvn_gpu_tex_release(t4);
            }

            static const uint8_t bc5_blk[16] = { 132, 132, 0,0,0,0,0,0,  64, 64, 0,0,0,0,0,0 };
            nvn_gpu_tex t5 = create_and_upload_bc(NVN_FORMAT_RGTC2_UNORM,
                NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_VIEW_FMT, bc5_blk, 16);
            if (t5) {
                view_t av = {0}; av.set_format = 1; av.format = NVN_FORMAT_RG8;
                nvn_gpu_view vrg = NULL; nvn_gpu_err e = {0};
                CHECK(OK(nvn_gpu_view_create(t5, &av, &vrg, &e)) && vrg, "BC5 -> RG8: %s", e.why);
                if (vrg) {
                    float g[4] = {0};
                    if (sample_view_once(cp, vrg, samp, g))
                        CHECK(near_f(g[0], 132 / 255.0f, 0.015f) && near_f(g[1], 64 / 255.0f, 0.015f),
                              "BC5 via RG8 view got=(%.4f,%.4f)", g[0], g[1]);
                    nvn_gpu_view_release(vrg);
                }
                expect_reject(t5, NVN_FORMAT_RGTC2_SNORM, "BC5_UNORM->BC5_SNORM");
                expect_reject(t5, NVN_FORMAT_RGBA8,       "BC5_UNORM(2B)->RGBA8(4B)");
                expect_reject(t5, NVN_FORMAT_R8,          "BC5_UNORM(2B)->R8(1B)");
                nvn_gpu_tex_release(t5);
            }
        }

        // 4. 不兼容格式显式拒绝负控（跨族 BC / RGB_DXT1->RGBA_DXT1 / BC6H SFLOAT->UFLOAT / 异字节类 / 缺 VIEW_FMT）
        {
            nvn_gpu_tex t_bc1 = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT);
            if (t_bc1) {
                expect_reject(t_bc1, NVN_FORMAT_RGBA_DXT3,   "BC1->BC2");
                expect_reject(t_bc1, NVN_FORMAT_RGBA_DXT5,   "BC1->BC3");
                expect_reject(t_bc1, NVN_FORMAT_BPTC_UNORM,  "BC1->BC7");
                expect_reject(t_bc1, NVN_FORMAT_RGTC1_UNORM, "BC1->BC4");
                expect_reject(t_bc1, NVN_FORMAT_RGBA16F,     "BC1(4B)->RGBA16F(8B)");
                expect_reject(t_bc1, NVN_FORMAT_R8,          "BC1(4B)->R8(1B)");
                expect_reject(t_bc1, NVN_FORMAT_RG8,         "BC1(4B)->RG8(2B)");
                expect_reject(t_bc1, NVN_FORMAT_RGB8,        "BC1(4B)->RGB8(expand)");
                expect_reject(t_bc1, NVN_FORMAT_DEPTH32F,    "BC1->DEPTH32F");
                expect_reject(t_bc1, NVN_FORMAT_RGBA_ASTC_4x4, "BC1->ASTC4x4");
                nvn_gpu_tex_release(t_bc1);
            }
            nvn_gpu_tex t_rgb1 = make_tex(4, 4, NVN_FORMAT_RGB_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT);
            if (t_rgb1) {
                expect_reject(t_rgb1, NVN_FORMAT_RGBA_DXT1,      "BC1_RGB->BC1_RGBA");
                expect_reject(t_rgb1, NVN_FORMAT_RGBA_DXT1_SRGB, "BC1_RGB->BC1_RGBA_SRGB");
                nvn_gpu_tex_release(t_rgb1);
            }
            nvn_gpu_tex t_bc6 = make_tex(4, 4, NVN_FORMAT_BPTC_SFLOAT, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT);
            if (t_bc6) {
                expect_reject(t_bc6, NVN_FORMAT_BPTC_UFLOAT, "BC6H_SFLOAT->BC6H_UFLOAT");
                expect_reject(t_bc6, NVN_FORMAT_RGBA8,       "BC6H(8B)->RGBA8(4B)");
                nvn_gpu_tex_release(t_bc6);
            }
            // 未声明 NVN_GPU_TEX_VIEW_FMT 时，UNORM 建图切 SRGB 必须拒收（未设 MUTABLE_FORMAT_BIT）
            nvn_gpu_tex t_nomut = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED);
            if (t_nomut) {
                expect_reject(t_nomut, NVN_FORMAT_RGBA_DXT1_SRGB, "BC1_no_VIEW_FMT->SRGB");
                nvn_gpu_tex_release(t_nomut);
            }
        }
    }

    if (samp) nvn_gpu_samp_release(samp);
    if (sh) nvn_gpu_shader_release(sh);
    nvn_gpu_err e = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close: %s", e.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("bc-decoded-alt-view: failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
