// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 同格式、单采样二维深度模板缩放。对齐 Ryujinx HelperShader 的绘制拷贝；
// 无需 BLIT_SRC/DST 或 shaderStencilExport：先写深度并清目标矩形模板，再按位写模板。
// stencil.frag 不声明 EarlyFragmentTests，discard 的片元不更新模板。
#include "backend_vulkan_depth_blit_spv.h"
static struct {
    VkShaderModule vs, fs[2], fs_ms;
    VkDescriptorSetLayout dsl, ms_dsl;
    VkPipelineLayout layout, ms_layout;
    VkSampler sampler;
    struct { VkPipeline depth, stencil; VkRenderPass pass; } p[2];
    struct { VkPipeline pipe; VkRenderPass pass; } ms[3];   // 0=D32F 1=D16 2=D24
} g_dsb;
static pthread_mutex_t g_dsb_mu = PTHREAD_MUTEX_INITIALIZER;

static void depth_blit_close(void) {
    pthread_mutex_lock(&g_dsb_mu);
    for (unsigned i = 0; i < 2; i++) {
        if (g_dsb.p[i].depth) vkDestroyPipeline(g.d, g_dsb.p[i].depth, NULL);
        if (g_dsb.p[i].stencil) vkDestroyPipeline(g.d, g_dsb.p[i].stencil, NULL);
        if (g_dsb.p[i].pass) vkDestroyRenderPass(g.d, g_dsb.p[i].pass, NULL);
        if (g_dsb.fs[i]) vkDestroyShaderModule(g.d, g_dsb.fs[i], NULL);
    }
    for (unsigned i = 0; i < 3; i++) {
        if (g_dsb.ms[i].pipe) vkDestroyPipeline(g.d, g_dsb.ms[i].pipe, NULL);
        if (g_dsb.ms[i].pass) vkDestroyRenderPass(g.d, g_dsb.ms[i].pass, NULL);
    }
    if (g_dsb.fs_ms) vkDestroyShaderModule(g.d, g_dsb.fs_ms, NULL);
    if (g_dsb.vs) vkDestroyShaderModule(g.d, g_dsb.vs, NULL);
    if (g_dsb.ms_layout) vkDestroyPipelineLayout(g.d, g_dsb.ms_layout, NULL);
    if (g_dsb.ms_dsl) vkDestroyDescriptorSetLayout(g.d, g_dsb.ms_dsl, NULL);
    if (g_dsb.layout) vkDestroyPipelineLayout(g.d, g_dsb.layout, NULL);
    if (g_dsb.dsl) vkDestroyDescriptorSetLayout(g.d, g_dsb.dsl, NULL);
    if (g_dsb.sampler) vkDestroySampler(g.d, g_dsb.sampler, NULL);
    memset(&g_dsb, 0, sizeof g_dsb);
    pthread_mutex_unlock(&g_dsb_mu);
}

static VkShaderModule depth_blit_module(const uint32_t *words, size_t bytes) {
    VkShaderModuleCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes; ci.pCode = words;
    VkShaderModule out = VK_NULL_HANDLE;
    return vkCreateShaderModule(g.d, &ci, NULL, &out) == VK_SUCCESS ? out : VK_NULL_HANDLE;
}

// 调用者持 g_dsb_mu；两个槽分别对应 D24S8 / D32FS8，不按设备或游戏分支。
static int depth_blit_pipelines(uint32_t format, unsigned slot) {
    if (g_dsb.p[slot].depth && g_dsb.p[slot].stencil) return 1;
    if (!g_dsb.vs) g_dsb.vs = depth_blit_module(k_dsb_vs, sizeof k_dsb_vs);
    if (!g_dsb.fs[0]) g_dsb.fs[0] = depth_blit_module(k_dsb_depth, sizeof k_dsb_depth);
    if (!g_dsb.fs[1]) g_dsb.fs[1] = depth_blit_module(k_dsb_stencil, sizeof k_dsb_stencil);
    if (!g_dsb.vs || !g_dsb.fs[0] || !g_dsb.fs[1]) return 0;
    if (!g_dsb.dsl) {
        VkDescriptorSetLayoutBinding b[2] = {{0}};
        for (unsigned i = 0; i < 2; i++) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
        VkDescriptorSetLayoutCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; ci.bindingCount = 2; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(g.d, &ci, NULL, &g_dsb.dsl) != VK_SUCCESS) return 0;
    }
    if (!g_dsb.layout) {
        VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36};
        VkPipelineLayoutCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1; ci.pSetLayouts = &g_dsb.dsl; ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(g.d, &ci, NULL, &g_dsb.layout) != VK_SUCCESS) return 0;
    }
    if (!g_dsb.sampler) {
        VkSamplerCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(g.d, &ci, NULL, &g_dsb.sampler) != VK_SUCCESS) return 0;
    }
    if (!g_dsb.p[slot].pass) {
        nvn_gpu_pipe_desc pd = {0}; pd.dfmt = format; pd.samples = 1;
        g_dsb.p[slot].pass = make_pipeline_pass(&pd);
        if (!g_dsb.p[slot].pass) return 0;
    }
    for (unsigned mode = 0; mode < 2; mode++) {
        VkPipeline *out = mode ? &g_dsb.p[slot].stencil : &g_dsb.p[slot].depth;
        if (*out) continue;
        VkPipelineShaderStageCreateInfo stages[2] = {{0}};
        for (unsigned i = 0; i < 2; i++) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].pName = "main"; }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = g_dsb.vs;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = g_dsb.fs[mode];
        VkPipelineVertexInputStateCreateInfo vi = {0}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia = {0}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = {0}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {0}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = {0}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {0}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = ds.depthWriteEnable = !mode; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS; ds.stencilTestEnable = VK_TRUE;
        ds.front.compareOp = VK_COMPARE_OP_ALWAYS; ds.front.passOp = VK_STENCIL_OP_REPLACE; ds.front.compareMask = 255; ds.back = ds.front;
        VkPipelineColorBlendStateCreateInfo cb = {0}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
        VkPipelineDynamicStateCreateInfo dyn = {0}; dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dyn.dynamicStateCount = 4; dyn.pDynamicStates = dynamic;
        VkGraphicsPipelineCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp; ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb; ci.pDynamicState = &dyn;
        ci.layout = g_dsb.layout; ci.renderPass = g_dsb.p[slot].pass;
        if (vkCreateGraphicsPipelines(g.d, g_plc, 1, &ci, NULL, out) != VK_SUCCESS) return 0;
        plc_created();
    }
    return 1;
}

static VkImageView depth_blit_view(struct nvn_gpu_view_s *v, unsigned stencil) {
    pthread_mutex_lock(&g_mu);
    VkImageView *out = &v->depth_blit[stencil];
    if (!*out) {
        VkImageViewCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = v->tex->image; ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = v->tex->format;
        ci.subresourceRange = (VkImageSubresourceRange){stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, v->base_level, 1, v->base_layer, 1};
        if (vkCreateImageView(g.d, &ci, NULL, out) != VK_SUCCESS) *out = VK_NULL_HANDLE;
    }
    VkImageView result = *out;
    pthread_mutex_unlock(&g_mu); return result;
}

static void depth_blit(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *sv, const NVNcopyRegion *sr, struct nvn_gpu_view_s *dv, const NVNcopyRegion *dr, int linear) {
    struct nvn_gpu_tex_s *s = sv->tex, *d = dv->tex;
    const VkImageAspectFlags ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (linear || s == d || s->samples != 1 || d->samples != 1 || s->aspect != ds || d->aspect != ds ||
        s->nvn_format != d->nvn_format || s->format != d->format || s->target == NVN_TEXTURE_TARGET_3D || d->target == NVN_TEXTURE_TARGET_3D ||
        (d->nvn_format != NVN_FORMAT_DEPTH24_STENCIL8 && d->nvn_format != NVN_FORMAT_DEPTH32F_STENCIL8)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度模板缩放：须不同图像、同 D24S8/D32FS8 格式、单采样二维及最近点过滤"); return;
    }
    VkOffset3D so, dof; VkExtent3D se, de;
    if (sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0 || sr->depth != 1 || dr->depth != 1 || sr->zoffset || dr->zoffset ||
        copy_region(s, sv->base_level, sr, &so, &se) || copy_region(d, dv->base_level, dr, &dof, &de) || sv->base_layer >= s->layers || dv->base_layer >= d->layers) {
        set_sticky(c, NVN_GPU_E_ARG, "深度模板缩放区域越界"); return;
    }
    VkFormatProperties features; vkGetPhysicalDeviceFormatProperties(g.phys, s->format, &features);
    if (!(s->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(d->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度模板缩放：源图缺采样用途或目标图缺附件用途"); return;
    }
    if (!(features.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || !(features.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度模板缩放：格式缺采样或附件能力"); return;
    }
    unsigned slot = d->nvn_format == NVN_FORMAT_DEPTH32F_STENCIL8;
    pthread_mutex_lock(&g_dsb_mu); int ready = depth_blit_pipelines(d->nvn_format, slot); pthread_mutex_unlock(&g_dsb_mu);
    if (!ready) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度模板缩放管线创建失败"); return; }
    VkDescriptorImageInfo images[2] = {{0}}; VkWriteDescriptorSet writes[2] = {{0}}; VkDescriptorSet set;
    if (!alloc_sets(c, &g_dsb.dsl, 1, &set, NULL, 0, 0)) return;
    for (unsigned i = 0; i < 2; i++) {
        images[i].sampler = g_dsb.sampler; images[i].imageView = depth_blit_view(sv, i); images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!images[i].imageView) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度模板缩放源视图创建失败"); return; }
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set; writes[i].dstBinding = i;
        writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(g.d, 2, writes, 0, NULL);
    full_barrier(c); image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL); cmd_ref(c, sv, REF_VIEW);
    nvn_gpu_pass_desc pass = {0}; pass.w = d->w >> dv->base_level; pass.h = d->h >> dv->base_level; if (!pass.w) pass.w = 1; if (!pass.h) pass.h = 1; pass.layers = 1;
    pass.depth.view = pass.stencil.view = dv; pass.depth.load = pass.stencil.load = NVN_GPU_LOAD_LOAD; pass.depth.store = pass.stencil.store = NVN_GPU_STORE_STORE;
    pass.trace_origin = NVN_GPU_PASS_TRACE_DEPTH_BLIT;
    nvn_gpu_pass_begin(c, &pass); if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    VkViewport vp = {(float)dr->xoffset, (float)dr->yoffset, (float)dr->width, (float)dr->height, 0, 1};
    VkRect2D sc = {{dr->xoffset, dr->yoffset}, {(uint32_t)dr->width, (uint32_t)dr->height}};
    struct { float origin_scale[4], destination[4]; uint32_t bit; } push = {{(float)sr->xoffset, (float)sr->yoffset, (float)sr->width / dr->width, (float)sr->height / dr->height}, {(float)dr->xoffset, (float)dr->yoffset, 0, 0}, 0};
    vkCmdSetViewport(c->cb, 0, 1, &vp); vkCmdSetScissor(c->cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dsb.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dsb.p[slot].depth);
    vkCmdSetStencilWriteMask(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, 255); vkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdPushConstants(c->cb, g_dsb.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36, &push); vkCmdDraw(c->cb, 3, 1, 0, 0);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dsb.p[slot].stencil);
    vkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
    for (unsigned bit = 0; bit < 8; bit++) {
        push.bit = 1u << bit; vkCmdSetStencilWriteMask(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, push.bit);
        vkCmdPushConstants(c->cb, g_dsb.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36, &push); vkCmdDraw(c->cb, 3, 1, 0, 0);
    }
    nvn_gpu_pass_end(c); image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL); full_barrier(c);
    // 抽象绑定/动态状态仍属于调用方；物理命令缓冲被辅助绘制改过，下一笔须全部重发。
    es_reset(c); memset(c->dsh_ok, 0, sizeof c->dsh_ok); c->desc_point = 0;
}

// 调用者持 g_dsb_mu；三个槽分别对应单平面 D32F / D16 / D24 的 MSAA -> 1x sample0 深度 resolve。
static int depth_resolve_pipeline(uint32_t format, unsigned slot) {
    if (g_dsb.ms[slot].pipe) return 1;
    if (!g_dsb.vs) g_dsb.vs = depth_blit_module(k_dsb_vs, sizeof k_dsb_vs);
    if (!g_dsb.fs_ms) g_dsb.fs_ms = depth_blit_module(k_dsb_depth_msaa, sizeof k_dsb_depth_msaa);
    if (!g_dsb.vs || !g_dsb.fs_ms) return 0;
    if (!g_dsb.ms_dsl) {
        VkDescriptorSetLayoutBinding b = {0};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1; ci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(g.d, &ci, NULL, &g_dsb.ms_dsl) != VK_SUCCESS) return 0;
    }
    if (!g_dsb.ms_layout) {
        VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36};
        VkPipelineLayoutCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1; ci.pSetLayouts = &g_dsb.ms_dsl; ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(g.d, &ci, NULL, &g_dsb.ms_layout) != VK_SUCCESS) return 0;
    }
    if (!g_dsb.sampler) {
        VkSamplerCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(g.d, &ci, NULL, &g_dsb.sampler) != VK_SUCCESS) return 0;
    }
    if (!g_dsb.ms[slot].pass) {
        nvn_gpu_pipe_desc pd = {0}; pd.dfmt = format; pd.samples = 1;
        g_dsb.ms[slot].pass = make_pipeline_pass(&pd);
        if (!g_dsb.ms[slot].pass) return 0;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{0}};
    for (unsigned i = 0; i < 2; i++) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = g_dsb.vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = g_dsb.fs_ms;
    VkPipelineVertexInputStateCreateInfo vi = {0}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {0}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {0}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {0}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {0}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS; ds.stencilTestEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb = {0}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {0}; dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb; ci.pDynamicState = &dyn;
    ci.layout = g_dsb.ms_layout; ci.renderPass = g_dsb.ms[slot].pass;
    if (vkCreateGraphicsPipelines(g.d, g_plc, 1, &ci, NULL, &g_dsb.ms[slot].pipe) != VK_SUCCESS) return 0;
    plc_created();
    return 1;
}

// Vulkan 1.1 纯核心 MSAA -> 1x 单平面深度 sample0 resolve：经 sampler2DMS texelFetch(..., 0) 写 gl_FragDepth，
// 零非 1.1 扩展依赖。合并深度模板 / 纯模板 / 跨格式 / 线性过滤继续明确报 UNSUPPORTED。
static void depth_resolve_msaa(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *sv, const NVNcopyRegion *sr,
                               struct nvn_gpu_view_s *dv, const NVNcopyRegion *dr, int linear) {
    struct nvn_gpu_tex_s *s = sv->tex, *d = dv->tex;
    if (linear || s == d || s->samples <= 1 || d->samples != 1 ||
        s->aspect != VK_IMAGE_ASPECT_DEPTH_BIT || d->aspect != VK_IMAGE_ASPECT_DEPTH_BIT ||
        s->nvn_format != d->nvn_format || s->format != d->format ||
        s->target == NVN_TEXTURE_TARGET_3D || d->target == NVN_TEXTURE_TARGET_3D ||
        (d->nvn_format != NVN_FORMAT_DEPTH32F && d->nvn_format != NVN_FORMAT_DEPTH16 && d->nvn_format != NVN_FORMAT_DEPTH24)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度 resolve：只支持同格式单平面深度（D32F/D16/D24）二维 MSAA -> 1x"); return;
    }
    VkOffset3D so, dof; VkExtent3D se, de;
    if (sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0 ||
        sr->depth != 1 || dr->depth != 1 || sr->zoffset || dr->zoffset ||
        copy_region(s, sv->base_level, sr, &so, &se) || copy_region(d, dv->base_level, dr, &dof, &de) ||
        sv->base_layer >= s->layers || dv->base_layer >= d->layers || se.depth != 1 || de.depth != 1 || so.z || dof.z) {
        set_sticky(c, NVN_GPU_E_ARG, "深度 resolve 区域越界"); return;
    }
    if (!(s->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(d->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度 resolve：源图缺采样用途或目标图缺深度附件用途"); return;
    }
    VkFormatProperties features; vkGetPhysicalDeviceFormatProperties(g.phys, s->format, &features);
    if (!(features.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !(features.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        !(g.dev->properties.limits.sampledImageDepthSampleCounts & (VkSampleCountFlags)s->samples)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度 resolve：设备不支持该格式/采样数的深度采样或附件能力"); return;
    }
    unsigned slot = d->nvn_format == NVN_FORMAT_DEPTH32F ? 0u : d->nvn_format == NVN_FORMAT_DEPTH16 ? 1u : 2u;
    pthread_mutex_lock(&g_dsb_mu); int ready = depth_resolve_pipeline(d->nvn_format, slot); pthread_mutex_unlock(&g_dsb_mu);
    if (!ready) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度 resolve 管线创建失败"); return; }
    VkDescriptorSet set;
    if (!alloc_sets(c, &g_dsb.ms_dsl, 1, &set, NULL, 0, 0)) return;
    VkDescriptorImageInfo image = { g_dsb.sampler, depth_blit_view(sv, 0), VK_IMAGE_LAYOUT_GENERAL };
    if (!image.imageView) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度 resolve 源视图创建失败"); return; }
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = set; write.dstBinding = 0;
    write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &image;
    vkUpdateDescriptorSets(g.d, 1, &write, 0, NULL);
    full_barrier(c); image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL); cmd_ref(c, sv, REF_VIEW);
    nvn_gpu_pass_desc pass = {0};
    pass.w = d->w >> dv->base_level; pass.h = d->h >> dv->base_level; if (!pass.w) pass.w = 1; if (!pass.h) pass.h = 1; pass.layers = 1;
    pass.depth.view = dv; pass.depth.load = NVN_GPU_LOAD_LOAD; pass.depth.store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(c, &pass); if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    VkViewport vp = {(float)dr->xoffset, (float)dr->yoffset, (float)dr->width, (float)dr->height, 0, 1};
    VkRect2D sc = {{dr->xoffset, dr->yoffset}, {(uint32_t)dr->width, (uint32_t)dr->height}};
    struct { float origin_scale[4], destination[4]; uint32_t bit; } push = {
        {(float)sr->xoffset, (float)sr->yoffset, (float)sr->width / dr->width, (float)sr->height / dr->height},
        {(float)dr->xoffset, (float)dr->yoffset, 0, 0}, 0};
    vkCmdSetViewport(c->cb, 0, 1, &vp); vkCmdSetScissor(c->cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dsb.ms_layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dsb.ms[slot].pipe);
    vkCmdPushConstants(c->cb, g_dsb.ms_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36, &push);
    vkCmdDraw(c->cb, 3, 1, 0, 0);
    nvn_gpu_pass_end(c); image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL); full_barrier(c);
    es_reset(c); memset(c->dsh_ok, 0, sizeof c->dsh_ok); c->desc_point = 0;
}
