// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_main
#include "t_feedback.c"
#undef main

void nvn_vk_barrier_ceiling_submit(uint64_t) __attribute__((weak));
void nvn_vk_barrier_ceiling_pipe(nvn_gpu_cmd, nvn_gpu_pipe_desc *) __attribute__((weak));

static int ceiling_mode = 3, ab_seconds = 30;

int __real_nvn_plat_flag(const char *, int);
int __wrap_nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_barrier_ceiling")) return ceiling_mode;
    if (!strcmp(name, "nvk_barrier_ceiling_ab")) return ab_seconds;
    return __real_nvn_plat_flag(name, fallback);
}

static VkRenderPass recorded_pass;
static unsigned no_sample_sets(void) { return 0; }
static void record_pass(const VkRenderPassBeginInfo *info) {
    recorded_pass = info->renderPass;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3 || argc == 4);
    setvbuf(stdout, NULL, _IOLBF, 0);
    dq_mode = atoi(argv[2]);
    struct s2a_vk_device device;
    char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    if (argc == 4) {
        ab_seconds = 0;
        ceiling_mode = atoi(argv[3]);
        REQUIRE(ceiling_mode == 0 || ceiling_mode == 3);
        observed_on_begin = on_begin;
        // case 0：前一通道 CLEAR/STORE → 另一通道直接取样。
        // case 9：先走反馈快照，切出写原图的通道后恢复原图取样。
        for (int which = 0; which < 12; which++) run_case(&device, argv[1], which);
        run_depth(&device, argv[1], NVN_FORMAT_DEPTH32F, 12);
        run_depth(&device, argv[1], NVN_FORMAT_DEPTH32F_STENCIL8, 13);
        REQUIRE(!s2a_vk_device_destroy(&device, 1));
        printf("barrier fixed dq=%d mode=%d cases=%u pixel-and-structure-failures=%u\n",
               dq_mode, ceiling_mode, ncases, failures);
        return failures != 0;
    }
    nvn_gpu_open_desc open = {.host_device = &device, .app_label = "barrier-ceiling"};
    GPU(nvn_gpu_open(&open, &err));

    nvn_gpu_shader vs = shader(argv[1], "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(argv[1], "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc describe = {0};
    describe.poly.mode = NVN_POLYGON_MODE_FILL;
    describe.color.logic_op = 3;
    describe.prim = 4;
    describe.samples = 1;
    describe.chmask.mask = 15;
    describe.cfmt[0] = NVN_FORMAT_RGBA8;
    describe.st[NVN_GPU_SH_VS].sh = vs;
    describe.st[NVN_GPU_SH_FS].sh = fs;
    nvn_gpu_tex_desc texture = {0};
    texture.d.target = NVN_TEXTURE_TARGET_2D;
    texture.d.format = NVN_FORMAT_RGBA8;
    texture.d.width = texture.d.height = 4;
    texture.d.depth = texture.d.levels = 1;
    texture.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex image = NULL;
    nvn_gpu_view view = NULL;
    GPU(nvn_gpu_tex_create(&texture, &image, &err));
    GPU(nvn_gpu_view_create(image, NULL, &view, &err));

    nvn_gpu_pipe pipes[4] = {0};
    VkRenderPass passes[4] = {0};
    observed_on_begin = record_pass;
    observed_sample_sets = no_sample_sets;
    for (unsigned i = 0; i < 4; i++) {
        if (nvn_vk_barrier_ceiling_submit)
            nvn_vk_barrier_ceiling_submit((1ull + 30ull * i) * 1000000000ull);
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_pipe_desc key = describe;
        if (nvn_vk_barrier_ceiling_pipe)
            nvn_vk_barrier_ceiling_pipe(cmd, &key);
        GPU(nvn_gpu_pipe_get(&key, NVN_GPU_PIPE_SYNC, &pipes[i], &err));
        begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4);
        nvn_gpu_set_pipe(cmd, pipes[i]);
        nvn_gpu_dyn dyn = {0};
        dyn.nvp = 1;
        dyn.vp[0][2] = dyn.vp[0][3] = 4;
        dyn.vp[0][5] = 1;
        dyn.has_sc = 1;
        dyn.sc[2] = dyn.sc[3] = 4;
        nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
        nvn_gpu_draw(cmd, &draw);
        nvn_gpu_pass_end(cmd);
        GPU(nvn_gpu_cmd_commit(cmd, i + 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(i + 1, 5000000000ull, &err));
        passes[i] = recorded_pass;
    }
    REQUIRE(observed_draws == 4);
    int pipe_split = pipes[0] != pipes[1];
    int pass_split = passes[0] != passes[1];
    int pipe_reuse = pipes[0] == pipes[2] && pipes[1] == pipes[3];
    int pass_reuse = passes[0] == passes[2] && passes[1] == passes[3];
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(image);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("barrier ABAB dq=%d draws=%u pipe-split=%d pass-split=%d pipe-reuse=%d pass-reuse=%d\n",
           dq_mode, observed_draws, pipe_split, pass_split, pipe_reuse, pass_reuse);
    return !(pipe_split && pass_split && pipe_reuse && pass_reuse);
}
