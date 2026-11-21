// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_main
#include "t_feedback.c"
#undef main

static unsigned split_passes, observed_passes, game_barriers, narrow_game_barriers;
static int nvnbar_mode = 1, ab_seconds, omit_incoming, omit_fragment, omit_transfer, omit_game_fragment;
static unsigned game_fragment_destinations;
static VkRenderPass last_pass;
void nvn_vk_nvnbar_submit(uint64_t now);
void nvn_vk_nvnbar_pass_barrier(nvn_gpu_cmd, uint32_t) __attribute__((weak));
void nvn_vk_nvnbar_pipe(nvn_gpu_cmd c, nvn_gpu_pipe_desc *d);

static void observe_pass(const VkRenderPassBeginInfo *info) { last_pass = info->renderPass; }
static const VkPipelineStageFlags fragment_stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
static unsigned no_sample_sets(void) { return 0; }

int __real_nvn_plat_flag(const char *, int);
int __wrap_nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_nvnbar")) return nvnbar_mode;
    if (!strcmp(name, "nvk_nvnbar_ab")) return ab_seconds;
    return __real_nvn_plat_flag(name, fallback);
}

extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice device,
        const VkRenderPassCreateInfo *info, const VkAllocationCallbacks *alloc, VkRenderPass *pass) {
    if (info->subpassCount != 1) return __real_vkCreateRenderPass(device, info, alloc, pass);
    observed_passes++;
    VkRenderPassCreateInfo adjusted = *info;
    VkSubpassDependency deps[3];
    REQUIRE(info->dependencyCount <= 3);
    unsigned incoming = 0, added = 0;
    for (uint32_t i = 0; i < info->dependencyCount; i++) {
        VkSubpassDependency dep = info->pDependencies[i];
        if (dep.srcSubpass == VK_SUBPASS_EXTERNAL && dep.dstSubpass == 0) {
            incoming++;
            if (omit_incoming && (dep.srcStageMask & VK_PIPELINE_STAGE_TRANSFER_BIT)) continue;
            if (omit_fragment && dep.srcStageMask == fragment_stages) {
                dep.dstStageMask &= ~VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                dep.dstAccessMask &= ~VK_ACCESS_SHADER_READ_BIT;
            }
        }
        deps[added++] = dep;
    }
    split_passes += incoming == 2;
    adjusted.dependencyCount = added;
    adjusted.pDependencies = deps;
    return __real_vkCreateRenderPass(device, &adjusted, alloc, pass);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass),
               "render-pass wrapper signature");

extern __typeof__(vkCmdPipelineBarrier) __real_vkCmdPipelineBarrier;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
        VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m,
        uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    if (nm == 1 && !nb && !ni && dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
        m->srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT &&
        m->dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) {
        game_barriers++;
        const VkPipelineStageFlags expected = fragment_stages | VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if (src == expected) {
            narrow_game_barriers++;
            if (omit_transfer) src &= ~VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
    }
    VkMemoryBarrier amended;
    const VkAccessFlags fragment_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    const VkPipelineStageFlags fragment_source = fragment_stages |
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (nm == 1 && !nb && !ni && src == fragment_source && dst == fragment_stages &&
        m->srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT && m->dstAccessMask == fragment_access) {
        game_fragment_destinations++;
        if (omit_game_fragment) {
            amended = *m;
            amended.dstAccessMask &= ~VK_ACCESS_SHADER_READ_BIT;
            m = &amended;
            dst &= ~VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
    }
    __real_vkCmdPipelineBarrier(cmd, src, dst, flags, nm, m, nb, b, ni, im);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdPipelineBarrier), PFN_vkCmdPipelineBarrier),
               "pipeline-barrier wrapper signature");

static void shape(struct s2a_vk_device *device, const char *dir, int alternate);
static void visibility(struct s2a_vk_device *device, const char *dir, const char *arm);

static PFN_vkVoidFunction command_hook(const char *name) {
    if (!strcmp(name, "vkCmdPipelineBarrier")) return (PFN_vkVoidFunction)__wrap_vkCmdPipelineBarrier;
    return NULL;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2 || argc == 5);
    const char *arm = argc == 5 ? argv[4] : "shape";
    nvnbar_mode = argc == 5 ? atoi(argv[2]) : 1;
    dq_mode = argc == 5 ? atoi(argv[3]) : 0;
    omit_incoming = !strcmp(arm, "no-incoming") || !strcmp(arm, "compute-no-incoming");
    omit_game_fragment = !strcmp(arm, "no-game-fragment");
    omit_fragment = !strcmp(arm, "no-fragment") || !strncmp(arm, "game-fragment", 13) || omit_game_fragment;
    omit_transfer = !strcmp(arm, "no-transfer");
    ab_seconds = !strcmp(arm, "abab") ? 30 : 0;
    observed_command = command_hook;
    struct s2a_vk_device device;
    char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    if (!strcmp(arm, "shape") || ab_seconds) {
        shape(&device, argv[1], ab_seconds != 0);
    } else if (!strcmp(arm, "sample")) {
        observed_on_begin = on_begin;
        run_case(&device, argv[1], 0);  // 另一通道 STORE → 片元直接取样
        run_case(&device, argv[1], 9);  // 反馈后跨通道恢复原图
        REQUIRE(failures == 0);
    } else {
        visibility(&device, argv[1], arm);
    }
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("nvnbar pass: arm=%s mode=%d dq=%d game-narrow=%u/%u split-passes=%u/%u\n",
           arm, nvnbar_mode, dq_mode, narrow_game_barriers, game_barriers, split_passes, observed_passes);
    return 0;
}

static nvn_gpu_pipe graphics_pipe(nvn_gpu_cmd cmd, nvn_gpu_shader vs, nvn_gpu_shader fs, int vertex) {
    nvn_gpu_pipe_desc describe = {0};
    describe.poly.mode = NVN_POLYGON_MODE_FILL;
    describe.color.logic_op = 3;
    describe.prim = 4;
    describe.samples = 1;
    describe.chmask.mask = 15;
    describe.cfmt[0] = NVN_FORMAT_RGBA8;
    describe.st[NVN_GPU_SH_VS].sh = vs;
    describe.st[NVN_GPU_SH_FS].sh = fs;
    if (vertex) {
        describe.attr[0].fmt = NVN_FORMAT_RGBA32F;
        describe.stream[0].stride = 16;
    }
    if (cmd) nvn_vk_nvnbar_pipe(cmd, &describe);
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&describe, NVN_GPU_PIPE_SYNC, &pipe, &err));
    return pipe;
}

static void color_target(nvn_gpu_tex *image, nvn_gpu_view *view) {
    nvn_gpu_tex_desc texture = {0};
    texture.d.target = NVN_TEXTURE_TARGET_2D;
    texture.d.format = NVN_FORMAT_RGBA8;
    texture.d.width = texture.d.height = 4;
    texture.d.depth = texture.d.levels = 1;
    texture.usage = NVN_GPU_TEX_RENDER;
    GPU(nvn_gpu_tex_create(&texture, image, &err));
    GPU(nvn_gpu_view_create(*image, NULL, view, &err));
}

static void draw_green(nvn_gpu_cmd cmd, nvn_gpu_view view, nvn_gpu_pipe pipe,
                       nvn_gpu_buf vbuf, nvn_gpu_buf indirect, const nvn_gpu_binding *ssbo,
                       int clear_rect) {
    begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4);
    if (clear_rect) {
        nvn_gpu_clear clear = {.color = 0, .color_mask = 15, .w = 2, .h = 2};
        clear.value.f[3] = 1.f;
        nvn_gpu_clear_rect(cmd, &clear);
    }
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {.nvp = 1, .has_sc = 1};
    dyn.vp[0][2] = dyn.vp[0][3] = 4;
    dyn.vp[0][5] = 1;
    dyn.sc[2] = dyn.sc[3] = 4;
    nvn_gpu_set_dyn(cmd, &dyn);
    if (vbuf) nvn_gpu_set_vbuf(cmd, 0, vbuf, 0);
    if (ssbo) nvn_gpu_bind(cmd, NVN_GPU_SH_FS, ssbo, 1);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1, .ind = indirect};
    nvn_gpu_draw(cmd, &draw);
    nvn_gpu_pass_end(cmd);
}

static void finish(nvn_gpu_cmd cmd, uint64_t fence) {
    GPU(nvn_gpu_cmd_commit(cmd, fence, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(fence, 5000000000ull, &err));
}

static void shape(struct s2a_vk_device *device, const char *dir, int alternate) {
    nvn_gpu_open_desc open = {.host_device = device, .app_label = "nvnbar-shape"};
    GPU(nvn_gpu_open(&open, &err));
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_tex image = NULL;
    nvn_gpu_view view = NULL;
    color_target(&image, &view);
    observed_sample_sets = no_sample_sets;
    observed_on_begin = observe_pass;
    nvn_gpu_pipe pipes[4] = {0};
    VkRenderPass passes[4] = {0};
    const unsigned rounds = alternate ? 4 : 1;
    for (unsigned i = 0; i < rounds; i++) {
        if (alternate) nvn_vk_nvnbar_submit((1ull + 30ull * i) * 1000000000ull);
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        pipes[i] = graphics_pipe(cmd, vs, fs, 0);
        nvn_gpu_barrier(cmd, NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
        draw_green(cmd, view, pipes[i], NULL, NULL, NULL, alternate);
        finish(cmd, i + 1);
        passes[i] = last_pass;
    }
    REQUIRE(observed_draws == rounds * (alternate ? 2u : 1u) && game_barriers == rounds);
    if (alternate) {
        REQUIRE(pipes[0] != pipes[1] && pipes[0] == pipes[2] && pipes[1] == pipes[3]);
        REQUIRE(passes[0] != passes[1] && passes[0] == passes[2] && passes[1] == passes[3]);
        REQUIRE(narrow_game_barriers == 2 && observed_passes == 6 && split_passes == 3);
    } else {
        REQUIRE(narrow_game_barriers == (unsigned)nvnbar_mode && observed_passes == 2 && split_passes == 2u * nvnbar_mode);
    }
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(image);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

static nvn_gpu_layout layout_for(nvn_gpu_layout_ent *entry, nvn_gpu_stage stage, int access) {
    *entry = (nvn_gpu_layout_ent){.cls = NVN_GPU_RES_SSBO, .count = 1, .access = access, .stages = 1u << stage};
    return (nvn_gpu_layout){.ent = entry, .nent = 1};
}

static void visibility(struct s2a_vk_device *device, const char *dir, const char *arm) {
    const int vertex = !strcmp(arm, "vertex") || !strcmp(arm, "no-incoming");
    const int indirect = !strcmp(arm, "indirect") || !strcmp(arm, "compute-no-incoming");
    const int dispatch = !strcmp(arm, "dispatch") || !strcmp(arm, "no-transfer");
    const int game_fragment = !strncmp(arm, "game-fragment", 13) || omit_game_fragment;
    const int fragment = game_fragment || !strcmp(arm, "fragment") || !strcmp(arm, "no-fragment");
    REQUIRE(vertex || indirect || dispatch || fragment);

    nvn_gpu_open_desc open = {.host_device = device, .app_label = arm};
    GPU(nvn_gpu_open(&open, &err));
    nvn_gpu_tex image = NULL;
    nvn_gpu_view view = NULL;
    if (!dispatch) color_target(&image, &view);
    nvn_gpu_buf buf = NULL, readback = NULL;
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, NULL, &buf, &err));
    if (!dispatch) GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, NULL, &readback, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    if (vertex || dispatch) {
        const float vertices[] = {-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
        const uint32_t data[] = {3,1,0,0};
        nvn_gpu_buf_write(cmd, buf, 0, vertex ? (const void *)vertices : (const void *)data,
                          vertex ? sizeof vertices : sizeof data);
    }
    nvn_gpu_binding ssbo = {.cls = NVN_GPU_RES_SSBO, .u.b = {.buf = buf, .len = 64}};
    nvn_gpu_shader cs = NULL;
    if (indirect || dispatch) {
        nvn_gpu_layout_ent ent;
        const nvn_gpu_layout layout = layout_for(&ent, NVN_GPU_SH_CS, dispatch ? NVN_GPU_ACC_R : NVN_GPU_ACC_W);
        cs = shader(dir, dispatch ? "compute_read_readonly.comp" : "compute_read_args.comp", NVN_GPU_SH_CS, &layout);
        const nvn_gpu_stage_desc desc = {.sh = cs};
        nvn_gpu_pipe cp = NULL;
        GPU(nvn_gpu_cpipe_get(&desc, NVN_GPU_PIPE_SYNC, &cp, &err));
        if (dispatch) nvn_gpu_barrier(cmd, NVN_BARRIER_ORDER_FRAGMENTS_BIT);
        nvn_gpu_set_pipe(cmd, cp);
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &ssbo, 1);
        nvn_gpu_dispatch(cmd, cp, 1, 1, 1, NULL, 0);
    }

    nvn_gpu_shader vs = NULL, fs = NULL, producer = NULL;
    if (!dispatch) {
        observed_sample_sets = no_sample_sets;
        vs = shader(dir, vertex ? "compute_read_vertex.vert" : "fullscreen.vert", NVN_GPU_SH_VS, NULL);
        nvn_gpu_layout_ent ent;
        if (fragment) {
            nvn_gpu_layout layout = layout_for(&ent, NVN_GPU_SH_FS, NVN_GPU_ACC_W);
            producer = shader(dir, "nvnbar_fragment_write.frag", NVN_GPU_SH_FS, &layout);
            draw_green(cmd, view, graphics_pipe(cmd, vs, producer, 0), NULL, NULL, &ssbo, 0);
            layout = layout_for(&ent, NVN_GPU_SH_FS, NVN_GPU_ACC_R);
            fs = shader(dir, "compute_read_buffer.frag", NVN_GPU_SH_FS, &layout);
        } else {
            fs = shader(dir, "solid_green.frag", NVN_GPU_SH_FS, NULL);
        }
        nvn_gpu_pipe pipe = graphics_pipe(cmd, vs, fs, vertex);
        if (game_fragment) {
            const uint32_t bits = NVN_BARRIER_ORDER_FRAGMENTS_BIT |
                (!strcmp(arm, "game-fragment-plain") ? 0 : NVN_BARRIER_INVALIDATE_ZCULL_BIT);
            if (nvn_vk_nvnbar_pass_barrier) nvn_vk_nvnbar_pass_barrier(cmd, bits);
            else nvn_gpu_barrier(cmd, bits);
        }
        draw_green(cmd, view, pipe, vertex ? buf : NULL, indirect ? buf : NULL, fragment ? &ssbo : NULL, 0);
        nvn_gpu_copy_tex_buf(cmd, image, 0, 0, NULL, readback, 0, 16, 64);
    }
    finish(cmd, 1);
    if (readback) {
        const unsigned char *pixels = nvn_gpu_buf_cpu(readback);
        static const unsigned char green[] = {0, 255, 0, 255};
        unsigned mismatches = 0;
        for (unsigned i = 0; i < 16; i++) mismatches += memcmp(pixels + i * sizeof green, green, sizeof green) != 0;
        printf("nvnbar readback: arm=%s green=%u/16\n", arm, 16 - mismatches);
        if (!omit_incoming && (!omit_fragment || (game_fragment && !omit_game_fragment))) REQUIRE(mismatches == 0);
        nvn_gpu_buf_release(readback, NULL, NULL);
        nvn_gpu_view_release(view);
        nvn_gpu_tex_release(image);
    }
    if (vs) nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    if (cs) nvn_gpu_shader_release(cs);
    if (producer) nvn_gpu_shader_release(producer);
    nvn_gpu_buf_release(buf, NULL, NULL);
    if (game_fragment) {
        printf("nvnbar game fragment destination=%u expected=1\n", game_fragment_destinations);
        REQUIRE(game_fragment_destinations == 1);
    }
    GPU(nvn_gpu_close(5000000000ull, &err));
}
