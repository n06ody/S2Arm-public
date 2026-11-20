// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_main_unused
#include "t_feedback.c"
#undef main

static int ceiling_mode;
static unsigned dependency_count;
static unsigned dependency_errors;
static _Atomic unsigned full_barriers;
static PFN_vkCmdPipelineBarrier real_barrier;

int __real_nvn_plat_flag(const char *, int);
int __wrap_nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_barrier_ceiling")) return ceiling_mode;
    return __real_nvn_plat_flag(name, fallback);
}

extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc, VkRenderPass *pass) {
    REQUIRE(info->subpassCount == 1 && info->dependencyCount == 2);
    const VkSubpassDescription *sub = &info->pSubpasses[0];
    VkPipelineStageFlags stages = 0;
    VkAccessFlags writes = 0, reads = 0;
    for (uint32_t i = 0; i < sub->colorAttachmentCount; i++) {
        if (sub->pColorAttachments[i].attachment == VK_ATTACHMENT_UNUSED) continue;
        stages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        writes |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        reads |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    }
    if (sub->pDepthStencilAttachment && sub->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) {
        stages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        writes |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        reads |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    for (unsigned i = 0; i < 2; i++) {
        const VkSubpassDependency *d = &info->pDependencies[i];
        REQUIRE(d->srcSubpass == (i ? 0 : VK_SUBPASS_EXTERNAL));
        REQUIRE(d->dstSubpass == (i ? VK_SUBPASS_EXTERNAL : 0));
        VkPipelineStageFlags src = i ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                     : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags dst = i ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        VkAccessFlags from = i ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                               : VK_ACCESS_MEMORY_WRITE_BIT;
        VkAccessFlags to = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        if (ceiling_mode == 1 || ceiling_mode == 2) {
            src = dst = stages ? stages : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            from = writes;
            to = writes | reads;
        }
        dependency_count++;
        if (d->srcStageMask != src || d->dstStageMask != dst || d->srcAccessMask != from ||
            d->dstAccessMask != to || d->dependencyFlags) dependency_errors++;
    }
    return __real_vkCreateRenderPass(device, info, alloc, pass);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass),
               "vkCreateRenderPass signature");

static VKAPI_ATTR void VKAPI_CALL observe_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                                 VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *mem,
                                                 uint32_t nb, const VkBufferMemoryBarrier *buf,
                                                 uint32_t ni, const VkImageMemoryBarrier *img) {
    if (src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && nm == 1 && !nb && !ni)
        atomic_fetch_add(&full_barriers, 1);
    real_barrier(cmd, src, dst, flags, nm, mem, nb, buf, ni, img);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&observe_barrier), PFN_vkCmdPipelineBarrier),
               "vkCmdPipelineBarrier signature");

static PFN_vkVoidFunction observe_command(const char *name) {
    return !strcmp(name, "vkCmdPipelineBarrier") ? (PFN_vkVoidFunction)observe_barrier : NULL;
}

static unsigned no_sample_sets(void) { return 0; }

// 模板由 pipe_get 建，实际通道由 pass_begin 建，提交后确认真实 draw 和屏障。
static void run_shape(struct s2a_vk_device *device, const char *dir, int color, int depth) {
    observer_reset();
    observed_sample_sets = no_sample_sets;
    dependency_count = dependency_errors = 0;
    nvn_gpu_open_desc open = {.host_device = device, .app_label = "barrier-ceiling"};
    GPU(nvn_gpu_open(&open, &err));
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(dir, "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.cfmt[0] = color ? NVN_FORMAT_RGBA8 : 0;
    pd.dfmt = depth ? NVN_FORMAT_DEPTH32F : 0;
    if (depth) { pd.ds.dt = pd.ds.dw = 1; pd.ds.df = 8; }
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_pass_desc p = {.ncolor = color ? 1 : 2, .w = 4, .h = 4, .layers = 1, .clear_depth = .25f};
    nvn_gpu_tex tex[2] = {0};
    nvn_gpu_view view[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        if (!(i ? depth : color)) continue;
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D;
        td.d.format = i ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8;
        td.d.width = td.d.height = 4;
        td.d.depth = td.d.levels = 1;
        td.usage = NVN_GPU_TEX_RENDER;
        GPU(nvn_gpu_tex_create(&td, &tex[i], &err));
        GPU(nvn_gpu_view_create(tex[i], NULL, &view[i], &err));
    }
    if (color) { p.color[0].view = view[0]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; }
    if (depth) { p.depth.view = view[1]; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE; }
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    // 无依赖资源，独立测游戏显式屏障的实际发射次数。
    atomic_store(&full_barriers, 0);
    nvn_gpu_barrier(cmd, 0);
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {.nvp = 1, .has_sc = 1, .sc = {0, 0, 4, 4}};
    dyn.vp[0][2] = dyn.vp[0][3] = 4;
    dyn.vp[0][5] = 1;
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(cmd, &draw);
    nvn_gpu_pass_end(cmd);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    unsigned count = atomic_load(&full_barriers);
    printf("ceiling dq=%d mode=%d color=%d depth=%d dependencies=%u mismatches=%u full=%u draws=%u\n",
           dq_mode, ceiling_mode, color, depth, dependency_count, dependency_errors, count, observed_draws);
    REQUIRE(dependency_count >= 4 && dependency_errors == 0);
    REQUIRE(count == (ceiling_mode != 2) && observed_draws == 1);
    for (unsigned i = 0; i < 2; i++) if (view[i]) { nvn_gpu_view_release(view[i]); nvn_gpu_tex_release(tex[i]); }
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

int main(int argc, char **argv) {
    REQUIRE(argc == 4);
    dq_mode = atoi(argv[2]);
    ceiling_mode = atoi(argv[3]);
    struct s2a_vk_device device;
    char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    real_barrier = (PFN_vkCmdPipelineBarrier)__real_vkGetDeviceProcAddr(device.device, "vkCmdPipelineBarrier");
    REQUIRE(real_barrier);
    observed_command = observe_command;
    for (unsigned shape = 0; shape < 4; shape++) run_shape(&device, argv[1], shape & 1, !!(shape & 2));
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    return 0;
}
