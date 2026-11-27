// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main zero_attr_main_unused
#include "t_replay_zero_attr.c"
#include "nvn_plat.h"

#undef main

static int use_dq;
int __real_nvn_plat_flag(const char *, int);
int __wrap_nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_compute_warm")) return 1;
    if (!strcmp(name, "nvk_dq")) return use_dq;
    return __real_nvn_plat_flag(name, fallback);
}
const char *nvn_plat_dir(int which) { (void)which; return NULL; }

static uint64_t fence;
static void finish(nvn_gpu_cmd cmd) {
    ++fence;
    GPU(nvn_gpu_cmd_commit(cmd, fence, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(fence, 5000000000ull, &err));
}

static unsigned check_pixel(const char *label, const uint8_t *base, unsigned x, unsigned y,
                            uint8_t red, uint8_t green) {
    const uint8_t *value = base + (y * 64 + x) * 4;
    int ok = value[0] == red && value[1] == green && value[2] == 0 && value[3] == 255;
    printf("%s (%u,%u): expected %u,%u,0,255 -> actual %u,%u,%u,%u%s\n",
           label, x, y, red, green, value[0], value[1], value[2], value[3], ok ? "" : " MISMATCH");
    return !ok;
}

static void draw_result(nvn_gpu_pipe pipe, nvn_gpu_tex tex, nvn_gpu_buf rb) {
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    ++serial;
    pass(active, 1);
    const nvn_gpu_draw_args draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    nvn_gpu_dyn dyn = {.nvp = 1, .origin_ll = 1};
    dyn.vp[0][2] = dyn.vp[0][3] = 64;
    dyn.vp[0][5] = 1;
    nvn_gpu_set_pipe(active, pipe);
    nvn_gpu_set_dyn(active, &dyn);
    nvn_gpu_draw(active, &draw);
    rp_pass_flush(active);
    nvn_gpu_copy_tex_buf(active, tex, 0, 0, NULL, rb, 0, 256, 16384);
    finish(active);
}

static unsigned check_viewports(const char *label, const nvn_texdesc *td, nvn_gpu_tex tex, nvn_gpu_buf rb,
                                nvn_gpu_shader vs, nvn_gpu_shader fs, nvn_gpu_shader gs) {
    unsigned bad = 0;
    rp_state_reset();
    rp_draw_reset();
    nvn_unimpl_reset();
    memset(&prog, 0, sizeof prog);
    prog.sh[0] = vs;
    prog.sh[1] = fs;
    prog.sh[2] = gs;
    prog.color_out_mask = 15;
    target_tex = (tex_t){.d = *td, .serial = 1};
    target = (p_set_rt){.n = 1, .color = {&target_tex}};
    // Change just the swizzle on one command segment; all other recipe
    // state is fixed. The Z/W exchange must clip the whole triangle.
    const uint8_t selectors[][4] = {{0}, {2,2,0,0}, {1,0,0,0}, {0,0,2,2}, {0}};
    const p_draw draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    ++serial;
    for (unsigned i = 0; i < sizeof selectors / sizeof selectors[0]; ++i) {
        p_vp_swizzle sw = {.n = 1};  // selectors XORed with identity
        memcpy(sw.sw[0], selectors[i], sizeof sw.sw[0]);
        REQUIRE(rp_state_op(OP_VP_SWIZZLE, &sw, sizeof sw));
        pass(active, 1);
        rp_draw(active, &draw, NULL, &sw);
        rp_pass_flush(active);
        nvn_gpu_copy_tex_buf(active, tex, 0, 0, NULL, rb, i * 16384, 256, 16384);
    }
    finish(active);
    for (unsigned i = 0; i < sizeof selectors / sizeof selectors[0]; ++i) {
        const uint8_t *pixels = (const uint8_t *)nvn_gpu_buf_cpu(rb) + i * 16384;
        bad += check_pixel(label, pixels, 10, 46, 0, i == 0 || i == 4 ? 255 : 0);
        bad += check_pixel(label, pixels, 46, 10, 0, i == 1 ? 255 : 0);
        bad += check_pixel(label, pixels, 53, 46, 0, i == 2 ? 255 : 0);
    }
    unsigned skipped_swizzle = (unsigned)nvn_unimpl_count("gpu", "viewport_swizzle");
    printf("%s: expected unimplemented=0 -> actual %u\n", label, skipped_swizzle);
    bad += skipped_swizzle != 0;
    rp_draw_reset();
    rp_buf_reset();
    return bad;
}

static unsigned check_graphics(const char *dir) {
    nvn_gpu_shader vs = shader(dir, "spec_pipe.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "spec_pipe.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc desc = {.samples = 1, .origin_ll = 1, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES};
    desc.poly.mode = NVN_POLYGON_MODE_FILL;
    desc.color.logic_op = NVN_LOGIC_OP_COPY;
    desc.chmask.mask = 15;
    desc.cfmt[0] = NVN_FORMAT_RGBA8;
    desc.st[NVN_GPU_SH_VS].sh = vs;
    desc.st[NVN_GPU_SH_FS].sh = fs;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 64;
    td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &target_view, &err));
    nvn_gpu_buf rb = buffer(5 * 16384, NULL);
    unsigned bad = 0;
    const uint8_t values[] = {19, 231, 19};
    nvn_gpu_pipe variants[3] = {0};
    nvn_gpu_stage_desc *frag = &desc.st[NVN_GPU_SH_FS];
    frag->nspec = 2;
    frag->spec[0] = (nvn_gpu_spec){.id = 3, .type = NVN_GPU_SPEC_UINT};
    frag->spec[1] = (nvn_gpu_spec){.id = 7, .type = NVN_GPU_SPEC_BOOL, .bits = 1};
    for (unsigned i = 0; i < 3; ++i) {
        frag->spec[0].bits = values[i];
        GPU(nvn_gpu_pipe_get(&desc, NVN_GPU_PIPE_SYNC, &variants[i], &err));
        draw_result(variants[i], tex, rb);
        bad += check_pixel("graphics spec", nvn_gpu_buf_cpu(rb), 10, 46, values[i], 255);
    }
    REQUIRE(variants[0] != variants[1] && variants[0] == variants[2]);

    bad += check_viewports("VS viewport replay", &td.d, tex, rb, vs, fs, NULL);
    nvn_gpu_shader gs = shader(dir, "spec_pipe.geom", NVN_GPU_SH_GS, NULL);
    bad += check_viewports("GS viewport replay", &td.d, tex, rb, vs, fs, gs);
    nvn_gpu_shader_release(gs);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(target_view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    return bad;
}

static unsigned check_compute(const char *dir) {
    const nvn_gpu_layout_ent ent = {
        .set = 0, .binding = 0, .count = 1, .stages = 1u << NVN_GPU_SH_CS,
        .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W,
    };
    const nvn_gpu_layout layout = {.ent = &ent, .nent = 1};
    nvn_gpu_shader cs = shader(dir, "spec_pipe.comp", NVN_GPU_SH_CS, &layout);
    nvn_gpu_stage_desc desc = {.sh = cs, .nspec = 3};
    desc.spec[0] = (nvn_gpu_spec){.id = 2, .type = NVN_GPU_SPEC_INT};
    desc.spec[1] = (nvn_gpu_spec){.id = 5, .type = NVN_GPU_SPEC_FLOAT};
    desc.spec[2] = (nvn_gpu_spec){.id = 9, .type = NVN_GPU_SPEC_UINT};
    const float delta = 7.0f;
    memcpy(&desc.spec[1].bits, &delta, sizeof delta);
    const int32_t base[] = {70, -34, 70};
    nvn_gpu_pipe variants[3] = {0};
    unsigned bad = 0;
    for (unsigned i = 0; i < 3; ++i) {
        memcpy(&desc.spec[0].bits, &base[i], sizeof base[i]);
        desc.spec[2].bits = i == 1 ? 2 : 1;
        const nvn_gpu_rc code = nvn_gpu_cpipe_get(&desc, NVN_GPU_PIPE_SYNC, &variants[i], &err);
        if (!nvn_gpu_ok(code)) {
            printf("compute spec: expected success -> actual %d (%s) MISMATCH\n", code.code, err.why);
            ++bad;
            continue;
        }
        const int32_t sentinel[] = {-999, 12345};
        nvn_gpu_buf rb = buffer(sizeof sentinel, sentinel);
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_set_pipe(cmd, variants[i]);
        const nvn_gpu_binding bind = {.cls = NVN_GPU_RES_SSBO, .u.b = {.buf = rb, .len = sizeof sentinel}};
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &bind, 1);
        nvn_gpu_dispatch(cmd, variants[i], 1, 1, 1, NULL, 0);
        finish(cmd);
        int32_t actual[2];
        memcpy(actual, nvn_gpu_buf_cpu(rb), sizeof actual);
        const int32_t expected = base[i] + 7 + (int32_t)desc.spec[2].bits - 1;
        printf("compute spec: expected %d/%d -> actual %d/%d\n", expected, sentinel[1], actual[0], actual[1]);
        bad += actual[0] != expected || actual[1] != sentinel[1];
        nvn_gpu_buf_release(rb, NULL, NULL);
    }
    if (variants[0] && variants[1] && variants[2])
        REQUIRE(variants[0] != variants[1] && variants[0] == variants[2]);
    nvn_gpu_pipe rejected = NULL;
    desc.spec[2].bits = UINT32_MAX;
    REQUIRE(nvn_gpu_cpipe_get(&desc, NVN_GPU_PIPE_SYNC, &rejected, &err).code == NVN_GPU_E_UNSUPPORTED && !rejected);
    REQUIRE(strstr(err.why, "maxComputeWorkGroupSize"));
    desc.spec[2].id = desc.spec[0].id;
    REQUIRE(nvn_gpu_cpipe_get(&desc, NVN_GPU_PIPE_SYNC, &rejected, &err).code == NVN_GPU_E_ARG && !rejected);
    desc.nspec = NVN_GPU_SPEC_MAX + 1;
    REQUIRE(nvn_gpu_cpipe_get(&desc, NVN_GPU_PIPE_SYNC, &rejected, &err).code == NVN_GPU_E_ARG && !rejected);
    nvn_gpu_shader_release(cs);
    return bad;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    use_dq = atoi(argv[2]);
    struct s2a_vk_device device;
    char why[256] = {0};
    if (s2a_vk_device_create(&device, why, sizeof why)) {
        fprintf(stderr, "device: %s\n", why);
        return 2;
    }
    nvn_gpu_open_desc open = {.host_device = &device, .app_label = "pipeline-specialization"};
    GPU(nvn_gpu_open(&open, &err));
    const unsigned graphics_bad = check_graphics(argv[1]);
    const unsigned compute_bad = check_compute(argv[1]);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("RESULT: %s pipeline specialization/swizzle (DQ=%d graphics/viewport=%u compute=%u)\n",
           graphics_bad || compute_bad ? "FAIL" : "PASS", use_dq, graphics_bad, compute_bad);
    return graphics_bad || compute_bad ? 1 : 0;
}
