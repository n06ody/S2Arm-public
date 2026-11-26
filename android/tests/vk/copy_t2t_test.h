// SPDX-License-Identifier: Apache-2.0
// Original data, real rp_op_copy_t2t and Vulkan. Resource lookup shares the
// existing mirror fixture, and each case uses an isolated backend session.
#include "nvn_tex.h"

enum copy_case {
    RGBA8_TO_HALF, HALF_TO_RGBA8, TO_VOLUME, FROM_VOLUME, TO_VOLUME_MIRROR, FROM_VOLUME_MIRROR,
    NO_BLIT_MIRROR, NO_BLIT_VOLUME, DOWNSAMPLE_SINGLE_FORMAT,
    SCALE_DEPTH, RESOLVE_UINT, RESOLVE_DEPTH, RESOLVE_UINT_SCALED, RESOLVE_DEPTH_SCALED, COPY_CASES
};

static int hide_color_blit;
#ifdef S2A_COPY_T2T_HIDE_BLIT
extern __typeof__(s2a_vk_format_properties) __real_s2a_vk_format_properties;
void __wrap_s2a_vk_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties) {
    __real_s2a_vk_format_properties(physical, format, properties);
    if (hide_color_blit && (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R16G16B16A16_SFLOAT))
        properties->optimalTilingFeatures &= ~(VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
}
#endif

static void copy_fixture_seed_msaa(nvn_gpu_cmd command, nvn_gpu_view view, int depth,
                                   nvn_gpu_shader vs, nvn_gpu_shader fs) {
    nvn_gpu_pass_desc p = {.w = 2, .h = 2, .layers = 1};
    if (depth) {
        p.depth.view = view;
        p.depth.load = NVN_GPU_LOAD_CLEAR;
        p.depth.store = NVN_GPU_STORE_STORE;
        p.clear_depth = .125f;
    } else {
        p.ncolor = 1;
        p.color[0].view = view;
        p.color[0].load = NVN_GPU_LOAD_CLEAR;
        p.color[0].store = NVN_GPU_STORE_STORE;
        p.color[0].clear.u[0] = 17;
    }
    nvn_gpu_pass_begin(command, &p);
    if (depth) {
        nvn_gpu_pipe_desc pd = {.dfmt = NVN_FORMAT_DEPTH32F, .samples = 4, .prim = 4, .origin_ll = 1};
        pd.poly.mode = NVN_POLYGON_MODE_FILL;
        pd.color.logic_op = 3;
        pd.st[NVN_GPU_SH_VS].sh = vs;
        pd.st[NVN_GPU_SH_FS].sh = fs;
        pd.ds.dt = pd.ds.dw = 1;
        pd.ds.df = NVN_DEPTH_FUNC_ALWAYS;
        nvn_gpu_pipe pipe = NULL;
        GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
        const nvn_gpu_dyn dyn = {.origin_ll = 1, .nvp = 1, .vp = {{0, 0, 2, 2, 0, 1}},
                                 .has_sc = 1, .sc = {0, 0, 2, 2}};
        const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
        nvn_gpu_set_pipe(command, pipe);
        nvn_gpu_set_dyn(command, &dyn);
        nvn_gpu_draw(command, &draw);
    } else for (unsigned i = 1; i < 4; i++) {
        nvn_gpu_clear clear = {.color = 0, .color_mask = 0xf, .stencil_mask = 0xff,
                               .x = i % 2, .y = i / 2, .w = 1, .h = 1};
        clear.value.u[0] = 17 + 19 * i;
        nvn_gpu_clear_rect(command, &clear);
    }
    nvn_gpu_pass_end(command);
}

static int copy_fixture_case(struct s2a_vk_device *device, const char *dir, enum copy_case shape) {
    static const char *const names[COPY_CASES] = {
        "RGBA8-to-RGBA16F", "RGBA16F-to-RGBA8", "2D-to-3D", "3D-to-2D",
        "2D-to-3D-scale-XY", "3D-to-2D-scale-XY", "RGBA8-to-RGBA16F-no-blit-XY", "3D-to-3D-no-blit-XY",
        "RGBA8-to-RGBA16F-downsample-1x", "D32F-scale", "R32UI-resolve", "D32F-resolve",
        "R32UI-resolve-scale", "D32F-resolve-scale",
    };
    const int mirror = shape >= TO_VOLUME_MIRROR && shape <= NO_BLIT_VOLUME;
    const int scaled = shape <= HALF_TO_RGBA8 || shape == SCALE_DEPTH || mirror || shape >= RESOLVE_UINT_SCALED;
    const int resolve = shape >= RESOLVE_UINT;
    const int depth = shape == SCALE_DEPTH || shape == RESOLVE_DEPTH || shape == RESOLVE_DEPTH_SCALED;
    const int integer = shape == RESOLVE_UINT || shape == RESOLVE_UINT_SCALED;
    const int from_volume = shape == FROM_VOLUME || shape == FROM_VOLUME_MIRROR || shape == NO_BLIT_VOLUME;
    const int to_volume = shape == TO_VOLUME || shape == TO_VOLUME_MIRROR || shape == NO_BLIT_VOLUME;
    const int to_half = shape == RGBA8_TO_HALF || shape == NO_BLIT_MIRROR || shape == DOWNSAMPLE_SINGLE_FORMAT;
    const unsigned edge = scaled ? 4 : 2;
    const unsigned dest_planes = to_volume ? 3 : 1;
    const unsigned source_z = from_volume ? 1 : 0;
    const unsigned dest_z = to_volume ? 1 : 0;
    nvn_gpu_open_desc od = {.host_device = device, .app_label = names[shape]};
    GPU(nvn_gpu_open(&od, &err));
    tex_t source = {0}, dest = {0};
    nvn_gpu_tex_desc td = {.d = {.target = resolve ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE :
                            from_volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D,
                            .format = depth ? NVN_FORMAT_DEPTH32F : integer ? NVN_FORMAT_R32UI :
                                shape == HALF_TO_RGBA8 ? NVN_FORMAT_RGBA16F : NVN_FORMAT_RGBA8,
                            .width = 2, .height = 2, .levels = 1, .depth = from_volume ? 3 : 1,
                            .samples = resolve ? 4 : 1}, .usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED};
    source.d = td.d;
    source.serial = 202;
    nvn_tex_layout(&source.d, &source.L);
    GPU(nvn_gpu_tex_create(&td, &mirror_images[0], &err));
    td.d.target = to_volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D;
    if (to_half) td.d.format = NVN_FORMAT_RGBA16F;
    if (shape == HALF_TO_RGBA8) td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = edge;
    td.d.depth = dest_planes;
    td.d.samples = 1;
    dest.d = td.d;
    dest.serial = 203;
    nvn_tex_layout(&dest.d, &dest.L);
    GPU(nvn_gpu_tex_create(&td, &mirror_images[1], &err));
    mirror_objects[0] = &source;
    mirror_objects[1] = &dest;
    mirror_view_count = 0;

    const float depths[4] = {.125f, .375f, .625f, .875f};
    const uint32_t uints[4] = {17, 36, 55, 74};
    const uint8_t rgba[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255},
                                {0, 0, 255, 255}, {255, 255, 0, 255}};
    const uint16_t half[4][4] = {{0x3c00, 0, 0, 0x3c00}, {0, 0x3c00, 0, 0x3c00},
                                 {0, 0, 0x3c00, 0x3c00}, {0x3c00, 0x3c00, 0, 0x3c00}};
    const unsigned bpp = to_half ? 8 : 4;
    const unsigned plane_bytes = bpp * edge * edge;
    uint8_t untouched[128] = {0};
    nvn_gpu_buf read = buffer(plane_bytes * dest_planes, NULL);
    nvn_gpu_view msaa = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (resolve && depth) {
        REQUIRE(device->core_features.sampleRateShading);
        vs = shader(dir, "resolve_sample_depth.vert", NVN_GPU_SH_VS, NULL);
        fs = shader(dir, "resolve_sample_depth.frag", NVN_GPU_SH_FS, NULL);
    }
    nvn_gpu_cmd command = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &command, &err));
    const NVNcopyRegion src_box = {0, 0, 0, 2, 2, 1}, dst_box = {0, 0, 0, edge, edge, 1};
    if (resolve) {
        GPU(nvn_gpu_view_create(mirror_images[0], NULL, &msaa, &err));
        copy_fixture_seed_msaa(command, msaa, depth, vs, fs);
    } else {
        const void *input = depth ? (const void *)depths : shape == HALF_TO_RGBA8 ? (const void *)half : (const void *)rgba;
        const unsigned stride = shape == HALF_TO_RGBA8 ? 16 : 8;
        nvn_gpu_tex_upload(command, mirror_images[0], 0, source_z, &src_box, input, stride, 2 * stride);
    }
    for (unsigned z = 0; z < dest_planes; z++)
        nvn_gpu_tex_upload(command, mirror_images[1], 0, z, &dst_box, untouched, bpp * edge, plane_bytes);
    p_copy_t2t op = {.src = &source, .dst = &dest, .sr = src_box, .dr = dst_box};
    op.sr.zoffset = source_z;
    op.dr.zoffset = dest_z;
    if (resolve || shape == DOWNSAMPLE_SINGLE_FORMAT) op.flags = DM_COPY_RESOLVE;
    if (mirror) op.flags = NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT;
    nvn_unimpl_reset();
    hide_color_blit = shape == NO_BLIT_MIRROR || shape == NO_BLIT_VOLUME;
    rp_op_copy_t2t(command, &op, &op);
    hide_color_blit = 0;
    for (unsigned z = 0; z < dest_planes; z++)
        nvn_gpu_copy_tex_buf(command, mirror_images[1], 0, z, &dst_box, read, z * plane_bytes, bpp * edge, plane_bytes);
    const nvn_gpu_rc submitted = nvn_gpu_cmd_commit(command, 1, done, NULL, &err);
    int passed = 0;
    if (!nvn_gpu_ok(submitted)) {
        fprintf(stderr, "【红】 copy-t2t %s FAIL submit code=%d why=%s\n", names[shape], submitted.code, err.why);
    } else {
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        const uint8_t *actual = nvn_gpu_buf_cpu(read);
        REQUIRE(actual);
        unsigned wrong = 0;
        for (unsigned z = 0; z < dest_planes; z++) for (unsigned y = 0; y < edge; y++) for (unsigned x = 0; x < edge; x++) {
            const unsigned sample = (scaled ? y / 2 : y) * 2 + (scaled ? x / 2 : x);
            const unsigned i = mirror ? 3 - sample : sample;
            const void *want = rgba[i];
            if (to_half) want = half[i];
            if (integer) want = &uints[i];
            if (depth) want = &depths[i];
            if (z != dest_z) want = untouched;
            wrong += memcmp(actual + z * plane_bytes + (y * edge + x) * bpp, want, bpp) != 0;
        }
        const unsigned unimpl = nvn_unimpl_kinds();
        passed = !wrong && !unimpl;
        printf("%scopy-t2t %s %s pixels=%u wrong=%u unimpl=%u\n", passed ? "" : "【红】 ", names[shape], passed ? "PASS" : "FAIL",
               edge * edge * dest_planes, wrong, unimpl);
    }
    for (unsigned j = 0; j < mirror_view_count; j++) nvn_gpu_view_release(mirror_views[j]);
    if (msaa) nvn_gpu_view_release(msaa);
    if (vs) nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    nvn_gpu_tex_release(mirror_images[0]);
    nvn_gpu_tex_release(mirror_images[1]);
    nvn_gpu_buf_release(read, NULL, NULL);
    GPU(nvn_gpu_close(5000000000ull, &err));
    return passed;
}

static int run_copy_t2t(struct s2a_vk_device *device, const char *dir) {
    unsigned passed = 0;
    for (enum copy_case shape = 0; shape < COPY_CASES; shape++)
        passed += copy_fixture_case(device, dir, shape);
    printf("copy-t2t %u/%d passed\n", passed, COPY_CASES);
    return passed == COPY_CASES ? 0 : 1;
}

static void check_copy_rejections(struct s2a_vk_device *device) {
    const nvn_gpu_open_desc od = {.host_device = device, .app_label = "copy-t2t-rejections"};
    const uint32_t formats[] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA_DXT1, NVN_FORMAT_RGB5A1};
    const char *const reasons[] = {"区域越界", "压缩格式", "16 位打包格式"};
    for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        GPU(nvn_gpu_open(&od, &err));
        nvn_gpu_tex_desc td = {.d = {.target = i ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_3D,
                               .format = formats[i], .width = 8, .height = 8,
                               .depth = i ? 1 : 2, .samples = 1, .levels = 1},
                               .usage = NVN_GPU_TEX_SAMPLED};
        nvn_gpu_tex src = NULL, dst = NULL;
        nvn_gpu_view sv = NULL, dv = NULL;
        GPU(nvn_gpu_tex_create(&td, &src, &err));
        td.d.target = NVN_TEXTURE_TARGET_2D;
        td.d.format = NVN_FORMAT_RGBA8;
        td.d.depth = 1;
        GPU(nvn_gpu_tex_create(&td, &dst, &err));
        GPU(nvn_gpu_view_create(src, NULL, &sv, &err));
        GPU(nvn_gpu_view_create(dst, NULL, &dv, &err));
        nvn_gpu_cmd command = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &command, &err));
        const NVNcopyRegion sr = {0, 0, i ? 0 : 2, 4, 4, 1}, dr = {0, 0, 0, 8, 8, 1};
        nvn_gpu_blit(command, sv, &sr, dv, &dr, 0);
        const nvn_gpu_rc rc = nvn_gpu_cmd_commit(command, 1, done, NULL, &err);
        REQUIRE(rc.code == (i ? NVN_GPU_E_UNSUPPORTED : NVN_GPU_E_ARG));
        REQUIRE(strstr(err.why, reasons[i]));
        nvn_gpu_view_release(sv);
        nvn_gpu_view_release(dv);
        nvn_gpu_tex_release(src);
        nvn_gpu_tex_release(dst);
        GPU(nvn_gpu_close(5000000000ull, &err));
    }
    puts("copy-t2t rejections: 3/3 (out-of-range slice, compressed transform, packed16 conversion)");
}
