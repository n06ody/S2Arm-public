// 目标区域清除（NVN ClearColor / ClearDepthStencil 的 Vulkan 实现）。
// NVN 的清除只作用于“命令发生时的附件视图 + 当前 scissor 区域”，深度/模板两个平面分别按掩码，
// 模板掩码是 8 位平面掩码（0x01 只清 bit0，不是整平面）。vkCmdClear*Image 只能整幅清，表达不了
// scissor 与位掩码，所以这里统一用“一次 render pass”：
//   - 整平面：vkCmdClearAttachments（带 rect）
//   - 部分模板位：受 VkStencilWriteMask 约束的全屏三角形绘制（模板 REPLACE，颜色/深度不写）
// 每次清除同步提交并等完成，与现有 GPU 立即操作（copy/fill/clear-tex）保持同一种完成语义。
#include "vk_clear.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include "nvn_vk_format.h"
#include "maskclear_spv.h"
#include "colorclear_spv.h"


#define CLR_PASS_CACHE 16
#define CLR_PIPE_CACHE 16
struct clr_cache {
    struct s2a_vk_device *dev;
    struct { VkFormat fmt; VkImageAspectFlags aspect; VkImageAspectFlags clear_aspect; int do_clear; VkRenderPass pass; } passes[CLR_PASS_CACHE];
    int npass;
    struct { VkFormat fmt; uint32_t stencil, mask; int color; VkPipeline pipe; VkPipelineLayout layout; } pipes[CLR_PIPE_CACHE];
    int npipe;
};
static struct clr_cache g_clr;

// ---- 提交结果分类 + 退休记账：clear 每次提交都分“未提交/完成/完成未知”，
// 完成未知时命令缓冲、fence、framebuffer、view 全部留在设备 owner 名下，不释放、不复用。
static uint64_t g_cmd_created, g_cmd_freed;
static uint64_t g_fence_created, g_fence_freed;
static uint64_t g_fb_created, g_fb_freed;
static uint64_t g_view_created, g_view_freed;
static uint64_t g_submitted, g_completed, g_unsubmitted, g_uncompleted;
static int g_submit_fault;   // 诊断注入：1 = 提交后按完成未知；2 = 假装提交失败

#define CLR_RETAINED_MAX 32
struct clr_retained {
    VkFence fence;
    VkCommandBuffer cmd;
    VkFramebuffer fb;
    VkImageView view;
};
static struct clr_retained g_clr_retained[CLR_RETAINED_MAX];
static int g_clr_retained_n;
static VkDevice g_clr_retained_dev;
static uint64_t g_clr_retained_overflow;

void s2a_vk_clear_set_submit_fault(int mode) { g_submit_fault = mode; }

void s2a_vk_clear_stats(struct s2a_vk_clear_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->commands_created = g_cmd_created; out->commands_freed = g_cmd_freed;
    out->fences_created = g_fence_created; out->fences_freed = g_fence_freed;
    out->framebuffers_created = g_fb_created; out->framebuffers_freed = g_fb_freed;
    out->views_created = g_view_created; out->views_freed = g_view_freed;
    out->submitted = g_submitted; out->completed = g_completed;
    out->unsubmitted = g_unsubmitted; out->uncompleted = g_uncompleted;
    out->retained = (uint32_t)g_clr_retained_n;
    out->retained_overflow = g_clr_retained_overflow;
}

int s2a_vk_clear_release_retained(struct s2a_vk_device *d, uint64_t timeout_ns, uint32_t *left_out) {
    if (left_out) *left_out = 0;
    if (!d || !d->device || !g_clr_retained_n) return 0;
    if (g_clr_retained_dev && g_clr_retained_dev != d->device) {
        fprintf(stderr, "[vk-clear] %d retained object set(s) belong to a previous device; cleared without destroy\n",
                g_clr_retained_n);
        memset(g_clr_retained, 0, sizeof g_clr_retained);
        g_clr_retained_n = 0;
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec +
                       (int64_t)(timeout_ns ? timeout_ns : 0);
    int reclaimed = 0, left = 0;
    for (int i = 0; i < g_clr_retained_n; i++) {
        struct clr_retained e = g_clr_retained[i];
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
        int64_t left_ns = deadline - now;
        VkResult wr = VK_TIMEOUT;
        if (left_ns > 0) wr = vkWaitForFences(d->device, 1, &e.fence, VK_TRUE, (uint64_t)left_ns);
        if (wr != VK_SUCCESS) { g_clr_retained[left++] = e; continue; }
        vkDestroyFence(d->device, e.fence, NULL); g_fence_freed++;
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &e.cmd); g_cmd_freed++;
        if (e.fb) { vkDestroyFramebuffer(d->device, e.fb, NULL); g_fb_freed++; }
        if (e.view) { vkDestroyImageView(d->device, e.view, NULL); g_view_freed++; }
        reclaimed++;
    }
    g_clr_retained_n = left;
    if (left)
        fprintf(stderr, "[vk-clear] %d retained clear set(s) have no completion proof before device destroy; "
                        "kept and reclaimed by device destruction (no fake completion)\n", left);
    if (left_out) *left_out = (uint32_t)left;
    return reclaimed;
}

static void clr_retain(struct s2a_vk_device *d, VkFence fence, VkCommandBuffer cmd,
                       VkFramebuffer fb, VkImageView view) {
    if (g_clr_retained_dev && g_clr_retained_dev != d->device) {
        // 旧设备的对象已随设备消失：只清账，不能在新设备上 destroy。
        memset(g_clr_retained, 0, sizeof g_clr_retained);
        g_clr_retained_n = 0;
    }
    g_clr_retained_dev = d->device;
    if (g_clr_retained_n >= CLR_RETAINED_MAX) {
        static int noted;
        if (!noted++)
            fprintf(stderr, "[vk-clear] retained-object table full (%d); in-flight clear objects are kept "
                            "but no longer tracked centrally\n", CLR_RETAINED_MAX);
        g_clr_retained_overflow++;
        return;
    }
    struct clr_retained *e = &g_clr_retained[g_clr_retained_n++];
    e->fence = fence; e->cmd = cmd; e->fb = fb; e->view = view;
}

static char g_clear_error[256];
static int fail(char *why, size_t n, const char *msg) {
    if (why && n) snprintf(why, n, "%s", msg);
    return -1;
}
static void note_clear_error(VkResult rc) {
    if (g_clear_error[0]) return;
    snprintf(g_clear_error, sizeof g_clear_error, "clear submission rc=%d", (int)rc);
}
const char *s2a_vk_clear_first_error(void) { return g_clear_error; }

static void clr_reset_device(struct s2a_vk_device *d) {
    if (g_clr.dev && g_clr.dev->device != (d ? d->device : VK_NULL_HANDLE)) memset(&g_clr, 0, sizeof g_clr);
}

// do_clear=1：整幅覆盖的清除。既然每个像素都会被写，上一帧内容就不需要被读进来——
// 用 loadOp=CLEAR 让驱动在装载阶段完成清除，省掉一次整幅 LOAD（2712x1220x4 ≈ 13 MB）
// 和一条显式 vkCmdClearAttachments。do_clear=0（部分区域/部分通道掩码）仍必须 LOAD 保留其余像素。
static VkRenderPass clr_pass(struct s2a_vk_device *d, VkFormat fmt, VkImageAspectFlags aspect,
                             VkImageAspectFlags clear_aspect, int do_clear, char *why, size_t why_size) {
    for (int i = 0; i < g_clr.npass; i++)
        if (g_clr.passes[i].fmt == fmt && g_clr.passes[i].aspect == aspect &&
            g_clr.passes[i].clear_aspect == clear_aspect && g_clr.passes[i].do_clear == do_clear)
            return g_clr.passes[i].pass;
    if (g_clr.npass >= CLR_PASS_CACHE) return VK_NULL_HANDLE;
    // 只对“这次真的要清的 aspect”设 loadOp=CLEAR：深度-only 清除不能把模板清成 0，
    // 模板-only 也不能覆盖深度。旧实现按 do_clear 同时清两个平面（真机 C3 反例：
    // depth-only clear touched stencil 0x00 want 0x5A）。
    const int clear_color = do_clear && (clear_aspect & VK_IMAGE_ASPECT_COLOR_BIT);
    const int clear_depth = do_clear && (clear_aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    const int clear_stencil = do_clear && (clear_aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    VkAttachmentDescription att = {0};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = (clear_color || clear_depth) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref = { 0, aspect & VK_IMAGE_ASPECT_COLOR_BIT
        ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    } else {
        sub.pDepthStencilAttachment = &ref;
    }
    VkRenderPassCreateInfo rpi = {0};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(d->device, &rpi, NULL, &pass) != VK_SUCCESS)
        return fail(why, why_size, "clear: vkCreateRenderPass failed"), VK_NULL_HANDLE;
    g_clr.dev = d;
    g_clr.passes[g_clr.npass].fmt = fmt;
    g_clr.passes[g_clr.npass].aspect = aspect;
    g_clr.passes[g_clr.npass].clear_aspect = clear_aspect;
    g_clr.passes[g_clr.npass].do_clear = do_clear;
    g_clr.passes[g_clr.npass].pass = pass;
    g_clr.npass++;
    return pass;
}

// 部分模板位清除用的管线：无顶点输入、无颜色附件、不写深度，模板比较恒真、命中即 REPLACE，
// VkStencilWriteMask 限制到请求的位。reference 与掩码一起进缓存键（静态状态，不依赖动态状态扩展）。
static VkPipeline clr_mask_pipeline(struct s2a_vk_device *d, VkRenderPass pass, VkFormat fmt,
                                    uint32_t stencil, uint32_t mask, char *why, size_t why_size) {
    for (int i = 0; i < g_clr.npipe; i++)
        if (!g_clr.pipes[i].color && g_clr.pipes[i].fmt == fmt && g_clr.pipes[i].stencil == stencil &&
            g_clr.pipes[i].mask == mask)
            return g_clr.pipes[i].pipe;
    if (g_clr.npipe >= CLR_PIPE_CACHE) return VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci = {0};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof stages);
    smci.codeSize = sizeof maskclear_vert; smci.pCode = maskclear_vert;
    if (vkCreateShaderModule(d->device, &smci, NULL, &vs) != VK_SUCCESS)
        return fail(why, why_size, "clear: mask-clear vertex module failed"), VK_NULL_HANDLE;
    smci.codeSize = sizeof maskclear_frag; smci.pCode = maskclear_frag;
    if (vkCreateShaderModule(d->device, &smci, NULL, &fs) != VK_SUCCESS) {
        vkDestroyShaderModule(d->device, vs, NULL);
        return fail(why, why_size, "clear: mask-clear fragment module failed"), VK_NULL_HANDLE;
    }
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
    ds.stencilTestEnable = VK_TRUE;
    ds.front.failOp = VK_STENCIL_OP_KEEP; ds.front.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.front.passOp = VK_STENCIL_OP_REPLACE; ds.front.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.front.writeMask = mask; ds.front.reference = stencil;
    ds.back = ds.front;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn_state = {0};
    dyn_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_state.dynamicStateCount = 2; dyn_state.pDynamicStates = dyn;
    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 0;
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(d->device, &plci, NULL, &layout) != VK_SUCCESS) {
        vkDestroyShaderModule(d->device, vs, NULL); vkDestroyShaderModule(d->device, fs, NULL);
        return fail(why, why_size, "clear: mask-clear pipeline layout failed"), VK_NULL_HANDLE;
    }
    VkGraphicsPipelineCreateInfo gpi = {0};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dyn_state;
    gpi.layout = layout; gpi.renderPass = pass;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult pr = vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe);
    vkDestroyPipelineLayout(d->device, layout, NULL);   // 管线创建后不再需要（无描述符）
    vkDestroyShaderModule(d->device, vs, NULL);
    vkDestroyShaderModule(d->device, fs, NULL);
    if (pr != VK_SUCCESS) return fail(why, why_size, "clear: mask-clear pipeline creation failed"), VK_NULL_HANDLE;
    g_clr.pipes[g_clr.npipe].fmt = fmt; g_clr.pipes[g_clr.npipe].stencil = stencil;
    g_clr.pipes[g_clr.npipe].mask = mask; g_clr.pipes[g_clr.npipe].color = 0;
    g_clr.pipes[g_clr.npipe].layout = VK_NULL_HANDLE;
    g_clr.pipes[g_clr.npipe].pipe = pipe;
    g_clr.npipe++;
    return pipe;
}

// 整数附件的清除值走整数通道；部分通道掩码用浮点片元输出无法表达整数附件，明确拒绝。
static int clr_format_is_integer(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return 1;
    default: return 0;
    }
}

// 部分颜色通道：全屏三角形输出清除色，VkColorWriteMask 限制到请求通道；深度/模板/混合都不参与。
// 管线布局要留在缓存里（vkCmdPushConstants 在录制时还需要它），不随管线创建销毁。
static VkPipeline clr_color_pipeline(struct s2a_vk_device *d, VkRenderPass pass, VkFormat fmt,
                                     uint32_t mask, VkPipelineLayout *layout_out,
                                     char *why, size_t why_size) {
    for (int i = 0; i < g_clr.npipe; i++)
        if (g_clr.pipes[i].color && g_clr.pipes[i].fmt == fmt && g_clr.pipes[i].mask == mask) {
            *layout_out = g_clr.pipes[i].layout;
            return g_clr.pipes[i].pipe;
        }
    if (g_clr.npipe >= CLR_PIPE_CACHE) return fail(why, why_size, "clear: colour-mask pipeline cache full"), VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci = {0};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof maskclear_vert; smci.pCode = maskclear_vert;
    if (vkCreateShaderModule(d->device, &smci, NULL, &vs) != VK_SUCCESS)
        return fail(why, why_size, "clear: colour-mask vertex module failed"), VK_NULL_HANDLE;
    smci.codeSize = sizeof colorclear_frag; smci.pCode = colorclear_frag;
    if (vkCreateShaderModule(d->device, &smci, NULL, &fs) != VK_SUCCESS) {
        vkDestroyShaderModule(d->device, vs, NULL);
        return fail(why, why_size, "clear: colour-mask fragment module failed"), VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(d->device, &plci, NULL, &layout) != VK_SUCCESS) {
        vkDestroyShaderModule(d->device, vs, NULL); vkDestroyShaderModule(d->device, fs, NULL);
        return fail(why, why_size, "clear: colour-mask pipeline layout failed"), VK_NULL_HANDLE;
    }
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = ((mask & 1u) ? VK_COLOR_COMPONENT_R_BIT : 0) |
                         ((mask & 2u) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                         ((mask & 4u) ? VK_COLOR_COMPONENT_B_BIT : 0) |
                         ((mask & 8u) ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn_state = {0};
    dyn_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_state.dynamicStateCount = 2; dyn_state.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gpi = {0};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb; gpi.pDynamicState = &dyn_state;
    gpi.layout = layout; gpi.renderPass = pass;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult pr = vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe);
    vkDestroyShaderModule(d->device, vs, NULL);
    vkDestroyShaderModule(d->device, fs, NULL);
    if (pr != VK_SUCCESS) {
        vkDestroyPipelineLayout(d->device, layout, NULL);
        return fail(why, why_size, "clear: colour-mask pipeline creation failed"), VK_NULL_HANDLE;
    }
    struct clr_cache *cache = &g_clr;
    cache->pipes[cache->npipe].fmt = fmt; cache->pipes[cache->npipe].stencil = 0;
    cache->pipes[cache->npipe].mask = mask; cache->pipes[cache->npipe].color = 1;
    cache->pipes[cache->npipe].layout = layout; cache->pipes[cache->npipe].pipe = pipe;
    cache->npipe++;
    *layout_out = layout;
    return pipe;
}

// 提交一次清除并等完成。cmd/fb/view 的所有权归本函数：
//   OK          —— 已提交且完成：fence/命令缓冲/framebuffer/view 全部回收。
//   UNSUBMITTED —— 没有提交成功：命令从未执行，全部回收（含 fb/view）。
//   UNCOMPLETED —— 已提交但没有完成证明：fence/命令缓冲/framebuffer/view 转入保留账本。
static int clr_submit(struct s2a_vk_device *d, VkCommandBuffer cmd, VkFramebuffer fb, VkImageView view,
                      char *why, size_t why_size) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;
        if (fb) { vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++; }
        if (view) { vkDestroyImageView(d->device, view, NULL); g_view_freed++; }
        return fail(why, why_size, "clear: vkEndCommandBuffer failed"), S2A_VK_CLEAR_UNSUBMITTED;
    }
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(d->device, &fci, NULL, &fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;
        if (fb) { vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++; }
        if (view) { vkDestroyImageView(d->device, view, NULL); g_view_freed++; }
        return fail(why, why_size, "clear: vkCreateFence failed"), S2A_VK_CLEAR_UNSUBMITTED;
    }
    g_fence_created++;
    if (g_submit_fault == 2) {
        // 负例：不提交。命令从未执行，fence/命令缓冲/fb/view 可以回收。
        vkDestroyFence(d->device, fence, NULL); g_fence_freed++;
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;
        if (fb) { vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++; }
        if (view) { vkDestroyImageView(d->device, view, NULL); g_view_freed++; }
        g_unsubmitted++;
        return fail(why, why_size, "clear: injected unsubmitted submit failure"), S2A_VK_CLEAR_UNSUBMITTED;
    }
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkResult rc = vkQueueSubmit(d->queue, 1, &si, fence);
    if (rc != VK_SUCCESS) {
        note_clear_error(rc);
        vkDestroyFence(d->device, fence, NULL); g_fence_freed++;
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;
        if (fb) { vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++; }
        if (view) { vkDestroyImageView(d->device, view, NULL); g_view_freed++; }
        g_unsubmitted++;
        return fail(why, why_size, "clear: GPU submission failed"), S2A_VK_CLEAR_UNSUBMITTED;
    }
    g_submitted++;
    if (g_submit_fault == 1) {
        rc = VK_TIMEOUT;   // 注入超时不是真实等待，不计入逐帧 fence 账。
    } else {
        // t-direct-frametimes：本次 fence 等待计入逐帧帧时间记录。先判 on()——
        // 模块不在或开关关都跳过一次分支判断，不计时也不记账（量的成本不默认付）。
        extern void nvn_vk_frame_log_fence_wait(uint64_t) __attribute__((weak));
        extern int nvn_vk_frame_log_on(void) __attribute__((weak));
        extern void nvn_vk_frame_tsv_fence_wait(uint64_t) __attribute__((weak));
        extern int nvn_vk_frame_tsv_on(void) __attribute__((weak));
        const int fl = nvn_vk_frame_log_on && nvn_vk_frame_log_on();
        const int ft = nvn_vk_frame_tsv_on && nvn_vk_frame_tsv_on();
        struct timespec wt;
        uint64_t wt0 = 0;
        if (fl || ft) {
            clock_gettime(CLOCK_MONOTONIC, &wt);
            wt0 = (uint64_t)wt.tv_sec * 1000000000ull + (uint64_t)wt.tv_nsec;
        }
        rc = vkWaitForFences(d->device, 1, &fence, VK_TRUE, 5000000000ull);
        if (fl || ft) {
            clock_gettime(CLOCK_MONOTONIC, &wt);
            uint64_t wt_ns = (uint64_t)wt.tv_sec * 1000000000ull + (uint64_t)wt.tv_nsec - wt0;
            if (fl) nvn_vk_frame_log_fence_wait(wt_ns);
            if (ft) nvn_vk_frame_tsv_fence_wait(wt_ns);
        }
    }
    if (rc != VK_SUCCESS) {
        // 提交成功但没拿到完成证明：命令缓冲/fence/framebuffer/view 都还在 GPU 上，禁止释放/复用。
        note_clear_error(rc);
        g_uncompleted++;
        clr_retain(d, fence, cmd, fb, view);
        return fail(why, why_size, "clear: GPU submission/completion unknown; objects retained"),
               S2A_VK_CLEAR_UNCOMPLETED;
    }
    vkDestroyFence(d->device, fence, NULL); g_fence_freed++;
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;
    if (fb) { vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++; }
    if (view) { vkDestroyImageView(d->device, view, NULL); g_view_freed++; }
    g_completed++;
    return S2A_VK_CLEAR_OK;
}

static VkImageView clr_view(struct s2a_vk_device *d, const s2a_clear_target *t, char *why, size_t why_size) {
    VkImageViewCreateInfo vci = {0};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = t->format;
    vci.subresourceRange = (VkImageSubresourceRange){ t->aspect, t->base_level, 1, t->base_layer, 1 };
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(d->device, &vci, NULL, &v) != VK_SUCCESS) return fail(why, why_size, "clear: image view failed"), VK_NULL_HANDLE;
    g_view_created++;
    return v;
}

static int clr_begin(struct s2a_vk_device *d, const s2a_clear_target *t, VkCommandBuffer *cmd_out,
                     VkRenderPass *pass_out, VkFramebuffer *fb_out, VkImageView *view_out,
                     const VkRect2D *scissor, VkRect2D *rect, VkImageAspectFlags clear_aspect, int do_clear,
                     const VkClearValue *clear_values, char *why, size_t why_size) {
    if (!d || !d->device || !t || !t->image) return fail(why, why_size, "clear: bad target");
    VkRect2D r = { {0, 0}, { t->width, t->height } };
    if (scissor) {
        int x0 = r.offset.x > scissor->offset.x ? r.offset.x : scissor->offset.x;
        int y0 = r.offset.y > scissor->offset.y ? r.offset.y : scissor->offset.y;
        int x1 = (int)(r.offset.x + r.extent.width), xe = scissor->offset.x + (int)scissor->extent.width;
        int y1 = (int)(r.offset.y + r.extent.height), ye = scissor->offset.y + (int)scissor->extent.height;
        if (xe < x1) x1 = xe;
        if (ye < y1) y1 = ye;
        if (x1 <= x0 || y1 <= y0) { *rect = r; return 1; }   // 空区域：调用方按“无需清除”处理
        r.offset.x = x0; r.offset.y = y0;
        r.extent.width = (uint32_t)(x1 - x0); r.extent.height = (uint32_t)(y1 - y0);
    }
    *rect = r;
    VkRenderPass pass = clr_pass(d, t->format, t->aspect, clear_aspect, do_clear, why, why_size);
    if (!pass) return -1;
    VkImageView view = clr_view(d, t, why, why_size);
    if (!view) return -1;
    VkFramebufferCreateInfo fbi = {0};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = pass; fbi.attachmentCount = 1; fbi.pAttachments = &view;
    fbi.width = t->width; fbi.height = t->height; fbi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(d->device, &fbi, NULL, &fb) != VK_SUCCESS) {
        vkDestroyImageView(d->device, view, NULL); g_view_freed++;
        return fail(why, why_size, "clear: framebuffer failed");
    }
    g_fb_created++;
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = d->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d->device, &cai, &cmd) != VK_SUCCESS) {
        vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++;
        vkDestroyImageView(d->device, view, NULL); g_view_freed++;
        return fail(why, why_size, "clear: command buffer allocation failed");
    }
    g_cmd_created++;
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;
        vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++;
        vkDestroyImageView(d->device, view, NULL); g_view_freed++;
        return fail(why, why_size, "clear: vkBeginCommandBuffer failed");
    }
    VkRenderPassBeginInfo rbi = {0};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = pass; rbi.framebuffer = fb;
    rbi.renderArea = (VkRect2D){ {0, 0}, { t->width, t->height } };
    rbi.clearValueCount = clear_values ? 1u : 0u;
    rbi.pClearValues = clear_values;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    *cmd_out = cmd; *pass_out = pass; *fb_out = fb; *view_out = view;
    return 0;
}

// 只结束 render pass。framebuffer/view/命令缓冲的生命周期全部交给 clr_submit：
// 提交成功但完成未知时它们不能被销毁（Vulkan 禁止），由保留账本持有到设备销毁。
static void clr_end(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

// 录制中途失败但命令从未提交：命令缓冲/framebuffer/view 都可立即回收（不进入保留账本）。
static int clr_abort_unsubmitted(struct s2a_vk_device *d, VkCommandBuffer cmd, VkFramebuffer fb,
                                 VkImageView view, char *why, size_t why_size, const char *msg) {
    clr_end(cmd);
    vkEndCommandBuffer(cmd);
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;
    vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++;
    vkDestroyImageView(d->device, view, NULL); g_view_freed++;
    return fail(why, why_size, msg);
}
// C1 诊断：为什么这笔清除没走队列。每个原因只打一次，避免每帧刷屏。
enum { CLR_Q_NO_HOOK, CLR_Q_BAD_TARGET, CLR_Q_MASK, CLR_Q_ASPECT, CLR_Q_RECT,
       CLR_Q_FV, CLR_Q_PASS, CLR_Q_ALLOC, CLR_Q_ENQUEUE, CLR_Q_STENCIL, CLR_Q_REASONS };
static void clr_q_note(int reason, const char *what) {
    static unsigned char noted[CLR_Q_REASONS];
    if (reason < 0 || reason >= CLR_Q_REASONS || noted[reason]) return;
    noted[reason] = 1;
    fprintf(stderr, "[s2a] clear queue: %s -> 同步路径\n", what);
}

// C1：帧队列入口由 nvn_vk_first_draw 提供（弱符号）。返回 0 = 已接手，命令缓冲所有权移交；
// 非 0 = 没接手，调用方必须自己回收那段“从未提交”的命令缓冲。
extern int nvn_vk_first_draw_clear_enqueue(struct s2a_vk_device *d, VkCommandBuffer cmd) __attribute__((weak));

// 队列化清除要复用 framebuffer/view：同步路径在拿到完成证明后才销毁它们（Vulkan 禁止更早），
// 而队列化没有“立刻完成”这个时点。这里按目标子资源缓存，活到设备切换为止；缓存满就退回同步路径，
// 不去猜一个能安全销毁的时刻。
#define CLR_FV_MAX 24
struct clr_fv {
    int used;
    VkImage image;
    VkFormat fmt;
    VkImageAspectFlags aspect;
    uint32_t level, layer, w, h;
    int do_clear;               // framebuffer 绑定到具体 render pass，loadOp 不同就是不同 pass
    VkImageAspectFlags clear_aspect;   // 这次真正要清的平面（深度-only 不能连模板一起清）
    VkFramebuffer fb;
    VkImageView view;
};
static struct clr_fv g_fv[CLR_FV_MAX];
static VkDevice g_fv_dev;

static void clr_fv_check_device(VkDevice dev) {
    if (g_fv_dev && dev && g_fv_dev != dev) {
        // 设备换了：旧设备正在销毁，这里只是别再拿旧句柄去用（不在这里销毁，避免命中已毁设备）。
        memset(g_fv, 0, sizeof g_fv);
    }
    g_fv_dev = dev;
}

static int clr_fv_get(struct s2a_vk_device *d, const s2a_clear_target *t, int do_clear,
                      VkImageAspectFlags clear_aspect, VkFramebuffer *fb_out, VkImageView *view_out,
                      char *why, size_t why_size) {
    clr_fv_check_device(d->device);
    struct clr_fv *slot = NULL;
    for (int i = 0; i < CLR_FV_MAX; i++) {
        struct clr_fv *e = &g_fv[i];
        if (!e->used) { if (!slot) slot = e; continue; }
        if (e->image == t->image && e->fmt == t->format && e->aspect == t->aspect &&
            e->level == t->base_level && e->layer == t->base_layer &&
            e->w == t->width && e->h == t->height && e->do_clear == do_clear &&
            e->clear_aspect == clear_aspect) {
            *fb_out = e->fb; *view_out = e->view;
            return 0;
        }
    }
    if (!slot) return fail(why, why_size, "clear: 队列化目标缓存已满"), -1;
    VkImageView view = clr_view(d, t, why, why_size);
    if (!view) return -1;
    VkRenderPass pass = clr_pass(d, t->format, t->aspect, clear_aspect, do_clear, why, why_size);
    if (!pass) { vkDestroyImageView(d->device, view, NULL); g_view_freed++; return -1; }
    VkFramebufferCreateInfo fbi = {0};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = pass; fbi.attachmentCount = 1; fbi.pAttachments = &view;
    fbi.width = t->width; fbi.height = t->height; fbi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(d->device, &fbi, NULL, &fb) != VK_SUCCESS) {
        vkDestroyImageView(d->device, view, NULL); g_view_freed++;
        return fail(why, why_size, "clear: 队列化 framebuffer 创建失败"), -1;
    }
    g_fb_created++; g_view_created++;
    slot->used = 1; slot->image = t->image; slot->fmt = t->format; slot->aspect = t->aspect;
    slot->level = t->base_level; slot->layer = t->base_layer; slot->w = t->width; slot->h = t->height;
    slot->do_clear = do_clear; slot->clear_aspect = clear_aspect;
    slot->fb = fb; slot->view = view;
    *fb_out = fb; *view_out = view;
    return 0;
}

// 录一段“只含这笔清除”的 render pass 并交给帧队列。整段在录制失败时从未提交，可直接回收。
static int clr_record_queued(struct s2a_vk_device *d, const s2a_clear_target *t,
                             const VkClearAttachment *att, const VkRect2D *rect,
                             int do_clear, const VkClearValue *clear_values,
                             char *why, size_t why_size) {
    if (!nvn_vk_first_draw_clear_enqueue) { clr_q_note(CLR_Q_NO_HOOK, "没有帧队列入口"); return S2A_VK_CLEAR_UNHANDLED; }
    if (!d || !d->device || !t || !t->image || !rect) { clr_q_note(CLR_Q_BAD_TARGET, "目标参数不完整"); return S2A_VK_CLEAR_UNHANDLED; }
    clr_reset_device(d);
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    if (clr_fv_get(d, t, do_clear, att ? att->aspectMask : 0, &fb, &view, why, why_size) != 0) {
        why[0] = 0;   // 缓存满/建失败不是清除错误：退回同步路径，不报错
        clr_q_note(CLR_Q_FV, "framebuffer/view 缓存不可用或已满");
        return S2A_VK_CLEAR_UNHANDLED;
    }
    VkRenderPass pass = clr_pass(d, t->format, t->aspect, att ? att->aspectMask : 0, do_clear, why, why_size);
    if (!pass) { why[0] = 0; clr_q_note(CLR_Q_PASS, "render pass 缓存不可用"); return S2A_VK_CLEAR_UNHANDLED; }
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = d->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d->device, &cai, &cmd) != VK_SUCCESS) {
        why[0] = 0;
        clr_q_note(CLR_Q_ALLOC, "命令缓冲分配失败");
        return S2A_VK_CLEAR_UNHANDLED;
    }
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        why[0] = 0;
        return S2A_VK_CLEAR_UNHANDLED;
    }
    VkRenderPassBeginInfo rbi = {0};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = pass; rbi.framebuffer = fb;
    rbi.renderArea = (VkRect2D){ {0, 0}, { t->width, t->height } };
    rbi.clearValueCount = clear_values ? 1u : 0u;
    rbi.pClearValues = clear_values;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (!do_clear) {
        VkClearRect cr = { *rect, 0, 1 };
        vkCmdClearAttachments(cmd, 1, att, 1, &cr);
    }
    vkCmdEndRenderPass(cmd);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        why[0] = 0;
        return S2A_VK_CLEAR_UNHANDLED;
    }
    if (nvn_vk_first_draw_clear_enqueue(d, cmd) != 0) {
        // 帧队列没接手：命令从未提交，可安全释放，退回同步路径。
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        why[0] = 0;
        clr_q_note(CLR_Q_ENQUEUE, "帧队列没接手（已满/未就绪/已毒化）");
        return S2A_VK_CLEAR_UNHANDLED;
    }
    return S2A_VK_CLEAR_QUEUED;
}

// 只有覆盖整幅的清除才能这样表达：部分矩形用 rect 已经表达，但空/越界的判定统一走原路径，
// 免得两条路对“空区域”给出不同结论。
static int clr_full_rect(const s2a_clear_target *t, const VkRect2D *scissor, VkRect2D *out) {
    VkRect2D r = { {0, 0}, { t->width, t->height } };
    if (!t->width || !t->height) return 0;
    if (scissor) {
        int x0 = r.offset.x > scissor->offset.x ? r.offset.x : scissor->offset.x;
        int y0 = r.offset.y > scissor->offset.y ? r.offset.y : scissor->offset.y;
        int x1 = (int)(r.offset.x + r.extent.width), xe = scissor->offset.x + (int)scissor->extent.width;
        int y1 = (int)(r.offset.y + r.extent.height), ye = scissor->offset.y + (int)scissor->extent.height;
        if (xe < x1) x1 = xe;
        if (ye < y1) y1 = ye;
        if (x1 <= x0 || y1 <= y0) return 0;      // 空区域交给原路径给出 NOOP
        r.offset.x = x0; r.offset.y = y0;
        r.extent.width = (uint32_t)(x1 - x0); r.extent.height = (uint32_t)(y1 - y0);
    }
    *out = r;
    return 1;
}

int s2a_vk_clear_color_queued(struct s2a_vk_device *d, const s2a_clear_target *t,
                              const VkClearColorValue *value, uint32_t color_mask,
                              const VkRect2D *scissor, char *why, size_t why_size) {
    (void)why; (void)why_size;
    if (!value || !t) return S2A_VK_CLEAR_UNHANDLED;
    color_mask &= 0xfu;
    if (!color_mask) return S2A_VK_CLEAR_NOOP;
    if (color_mask != 0xfu) { clr_q_note(CLR_Q_MASK, "颜色掩码不是 0xF"); return S2A_VK_CLEAR_UNHANDLED; }
    if (!(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) { clr_q_note(CLR_Q_ASPECT, "aspect 不是颜色"); return S2A_VK_CLEAR_UNHANDLED; }
    VkRect2D rect;
    if (!clr_full_rect(t, scissor, &rect)) { clr_q_note(CLR_Q_RECT, "空区域/尺寸为 0"); return S2A_VK_CLEAR_UNHANDLED; }
    VkClearAttachment att = {0};
    att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    att.colorAttachment = 0;
    att.clearValue.color = *value;
    // 覆盖整个附件时用 loadOp=CLEAR：省掉整幅 LOAD 和一条显式 clear；否则必须 LOAD 保留其余像素。
    const int full = rect.offset.x == 0 && rect.offset.y == 0 &&
                     rect.extent.width == t->width && rect.extent.height == t->height;
    VkClearValue cv; memset(&cv, 0, sizeof cv); cv.color = *value;
    return clr_record_queued(d, t, &att, &rect, full, full ? &cv : NULL, why, why_size);
}

int s2a_vk_clear_ds_queued(struct s2a_vk_device *d, const s2a_clear_target *t,
                           float depth, uint32_t stencil, int depth_mask, uint32_t stencil_bit_mask,
                           const VkRect2D *scissor, char *why, size_t why_size) {
    (void)why; (void)why_size;
    if (!t) return S2A_VK_CLEAR_UNHANDLED;
    if (!depth_mask && !stencil_bit_mask) return S2A_VK_CLEAR_NOOP;
    if (!(t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return S2A_VK_CLEAR_UNHANDLED;
    // 模板位掩码不是整平面时要逐位表达，只有 mask_cs pipeline 能做。
    int want_stencil = stencil_bit_mask != 0;
    if (want_stencil && stencil_bit_mask != 0xFFu) { clr_q_note(CLR_Q_STENCIL, "模板位掩码不是整平面"); return S2A_VK_CLEAR_UNHANDLED; }
    if (!(t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) want_stencil = 0;
    VkRect2D rect;
    if (!clr_full_rect(t, scissor, &rect)) return S2A_VK_CLEAR_UNHANDLED;
    VkClearAttachment att = {0};
    att.aspectMask = depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
    if (want_stencil) att.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!att.aspectMask) return S2A_VK_CLEAR_NOOP;
    att.colorAttachment = 0;
    att.clearValue.depthStencil.depth = depth;
    att.clearValue.depthStencil.stencil = stencil;
    const int full = rect.offset.x == 0 && rect.offset.y == 0 &&
                     rect.extent.width == t->width && rect.extent.height == t->height;
    VkClearValue cv; memset(&cv, 0, sizeof cv);
    cv.depthStencil.depth = depth;
    cv.depthStencil.stencil = stencil;
    return clr_record_queued(d, t, &att, &rect, full, full ? &cv : NULL, why, why_size);
}

int s2a_vk_clear_color(struct s2a_vk_device *d, const s2a_clear_target *t,
                       const VkClearColorValue *value, uint32_t color_mask, const VkRect2D *scissor,
                       char *why, size_t why_size) {
    if (!value) return fail(why, why_size, "clear: null colour value");
    color_mask &= 0xfu;
    if (!color_mask) return S2A_VK_CLEAR_NOOP;   // 零掩码：NVN 语义就是不写任何通道
    clr_reset_device(d);
    VkRect2D rect;
    // 必须先算 rect 再判断“整幅”：以前这里读未初始化的 rect 决定 cfull（UB，结果依赖栈内容），
    // 会让整幅清除随机走 loadOp=CLEAR 或 clearAttachments 分支。空区域按无清除处理。
    if (!clr_full_rect(t, scissor, &rect)) return S2A_VK_CLEAR_NOOP;
    VkCommandBuffer cmd = VK_NULL_HANDLE; VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    const int cfull = color_mask == 0xfu && rect.offset.x == 0 && rect.offset.y == 0 &&
                      rect.extent.width == t->width && rect.extent.height == t->height;
    VkClearValue ccv; memset(&ccv, 0, sizeof ccv); ccv.color = *value;
    int rc = clr_begin(d, t, &cmd, &pass, &fb, &view, scissor, &rect, VK_IMAGE_ASPECT_COLOR_BIT, cfull,
                       cfull ? &ccv : NULL, why, why_size);
    if (rc != 0) return rc;   // 1 = 空区域（无清除），-1 = 失败
    if (cfull) {
        // 整幅：loadOp=CLEAR 已在装载阶段做完，不再发显式清除。
    } else if (color_mask == 0xfu) {
        VkClearAttachment att = {0};
        att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att.colorAttachment = 0;
        att.clearValue.color = *value;
        VkClearRect cr = { rect, 0, 1 };
        vkCmdClearAttachments(cmd, 1, &att, 1, &cr);
    } else {
        if (clr_format_is_integer(t->format))
            return clr_abort_unsubmitted(d, cmd, fb, view, why, why_size,
                                         "clear: partial colour mask on an integer attachment has no path");
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipe = clr_color_pipeline(d, pass, t->format, color_mask, &layout, why, why_size);
        if (!pipe)
            return clr_abort_unsubmitted(d, cmd, fb, view, why, why_size,
                                         "clear: colour-mask pipeline unavailable");
        VkViewport full = { 0.0f, 0.0f, (float)t->width, (float)t->height, 0.0f, 1.0f };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, value->float32);
        vkCmdSetViewport(cmd, 0, 1, &full);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    clr_end(cmd);
    return clr_submit(d, cmd, fb, view, why, why_size);
}

int s2a_vk_clear_depth_stencil(struct s2a_vk_device *d, const s2a_clear_target *t,
                               float depth, uint32_t stencil, int depth_mask, uint32_t stencil_bit_mask,
                               const VkRect2D *scissor, char *why, size_t why_size) {
    clr_reset_device(d);
    uint32_t sm = stencil_bit_mask & 0xffu;
    int dm = depth_mask ? 1 : 0;
    if (!dm && !sm) return 0;
    VkRect2D rect;
    if (!clr_full_rect(t, scissor, &rect)) return 0;   // 空区域：无需清除
    VkCommandBuffer cmd = VK_NULL_HANDLE; VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    const int stencil_full = (sm == 0xffu);
    // 整幅深度/模板：loadOp=CLEAR。部分模板位仍走写掩码管线，必须 LOAD。
    // 同颜色：dfull 必须在 clr_full_rect 之后算，不能读未初始化 rect（旧实现 UB）。
    const int dfull = rect.offset.x == 0 && rect.offset.y == 0 &&
                      rect.extent.width == t->width && rect.extent.height == t->height &&
                      (sm == 0 || stencil_full);
    VkClearValue dcv; memset(&dcv, 0, sizeof dcv);
    dcv.depthStencil.depth = depth;
    dcv.depthStencil.stencil = stencil & 0xffu;
    VkClearAttachment att = {0};
    att.aspectMask = (dm ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                     (sm && stencil_full ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    int rc = clr_begin(d, t, &cmd, &pass, &fb, &view, scissor, &rect, att.aspectMask, dfull,
                       dfull ? &dcv : NULL, why, why_size);
    if (rc != 0) return rc;
    att.clearValue.depthStencil.depth = depth;
    att.clearValue.depthStencil.stencil = stencil & 0xffu;
    VkClearRect cr = { rect, 0, 1 };
    if (att.aspectMask && !dfull) vkCmdClearAttachments(cmd, 1, &att, 1, &cr);
    if (sm && !stencil_full) {
        // 部分模板位：写掩码限制到请求位；深度/颜色都不写，比较恒真。
        VkPipeline pipe = clr_mask_pipeline(d, pass, t->format, stencil & 0xffu, sm, why, why_size);
        if (!pipe) {
            clr_end(cmd);
            vkEndCommandBuffer(cmd);
            vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_cmd_freed++;   // 从未提交，可释放
            vkDestroyFramebuffer(d->device, fb, NULL); g_fb_freed++;
            vkDestroyImageView(d->device, view, NULL); g_view_freed++;
            return fail(why, why_size, "clear: mask-clear pipeline unavailable"), S2A_VK_CLEAR_UNSUBMITTED;
        }
        VkViewport vp = { (float)rect.offset.x, (float)rect.offset.y,
                          (float)rect.extent.width, (float)rect.extent.height, 0.0f, 1.0f };
        // 全屏三角形：顶点在裁剪空间铺满目标，scissor/视口把清除限制到真实区域。
        VkViewport full = { 0.0f, 0.0f, (float)t->width, (float)t->height, 0.0f, 1.0f };
        (void)vp;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdSetViewport(cmd, 0, 1, &full);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    clr_end(cmd);
    return clr_submit(d, cmd, fb, view, why, why_size);
}
