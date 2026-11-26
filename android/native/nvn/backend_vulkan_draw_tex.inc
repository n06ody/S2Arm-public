// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// DrawTexture（nvn_gpu_draw_tex，契约 v20，#1452）：当前通道里用内置管线画一个贴图矩形到颜色附件 0。
// 对照 iOS nvn_mtl.m draw_tex 与 Ryujinx HelperShader.DrawTexture：只写 color(0)（其余槽写掩码 0），混合 / 写掩码按调用方给的，
// 深度 / 模板测试关、不剔除，视口 = 目的矩形（窗口原点的换算同 apply 普通绘制：原点在左上时负高度翻），裁剪照给的。
// 着色器是 blit_shaders/draw_tex.{vert,frag} 编出的 SPIR-V（backend_vulkan_draw_tex_spv.h，generate.py 生成）。
// 管线按「通道各槽格式 + 深度模板格式 + 采样数 + 混合 + 写掩码」缓存，满了建段自有管线（同 backend_vulkan_clear.inc：缓存满不能把合法绘制变成整段失败）。
// 由 backend_vulkan.c include（要用它的 g、make_pipeline_pass、set_sticky、alloc_sets、cmd_ref、布局跟踪）。
#include "backend_vulkan_draw_tex_spv.h"

enum { DTX_MAX = 32 };
struct dtx_key { uint32_t cfmt[8], dsfmt, samples, blend_en, cmask; uint32_t blend[4]; };   // blend：src_rgb dst_rgb src_a dst_a | eq 打包，见 dtx_key_fill
struct dtx_pipe_temp { struct dtx_key k; VkPipeline pipe; VkRenderPass pass; struct dtx_pipe_temp *next; };
static struct {
    VkShaderModule vs, fs;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    struct { struct dtx_key k; VkPipeline pipe; VkRenderPass pass; } p[DTX_MAX];
    uint32_t n;
} g_dtx;
static pthread_mutex_t g_dtx_mu = PTHREAD_MUTEX_INITIALIZER;

static void dtx_cmd_free(struct nvn_gpu_cmd_s *c) {
    while (c->dtx_pipes) {
        struct dtx_pipe_temp *p = c->dtx_pipes; c->dtx_pipes = p->next;
        vkDestroyPipeline(g.d, p->pipe, NULL);
        vkDestroyRenderPass(g.d, p->pass, NULL);
        free(p);
    }
}

static void dtx_close(void) {   // 会话关闭：设备还在
    pthread_mutex_lock(&g_dtx_mu);
    for (uint32_t i = 0; i < g_dtx.n; i++) { vkDestroyPipeline(g.d, g_dtx.p[i].pipe, NULL); vkDestroyRenderPass(g.d, g_dtx.p[i].pass, NULL); }
    if (g_dtx.vs) vkDestroyShaderModule(g.d, g_dtx.vs, NULL);
    if (g_dtx.fs) vkDestroyShaderModule(g.d, g_dtx.fs, NULL);
    if (g_dtx.layout) vkDestroyPipelineLayout(g.d, g_dtx.layout, NULL);
    if (g_dtx.dsl) vkDestroyDescriptorSetLayout(g.d, g_dtx.dsl, NULL);
    memset(&g_dtx, 0, sizeof g_dtx);
    pthread_mutex_unlock(&g_dtx_mu);
}

static VkShaderModule dtx_module(const uint32_t *words, size_t bytes) {
    VkShaderModuleCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = bytes; ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(g.d, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// 着色器、描述符组布局（组合采样贴图一项）、管线布局（顶点 16 字节 push：uv）。调用方持 g_dtx_mu。
static int dtx_common(void) {
    if (!g_dtx.vs && !(g_dtx.vs = dtx_module(k_dtx_vs, sizeof k_dtx_vs))) return 0;
    if (!g_dtx.fs && !(g_dtx.fs = dtx_module(k_dtx_fs, sizeof k_dtx_fs))) return 0;
    if (!g_dtx.dsl) {
        VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci; memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; ci.bindingCount = 1; ci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(g.d, &ci, NULL, &g_dtx.dsl) != VK_SUCCESS) { g_dtx.dsl = VK_NULL_HANDLE; return 0; }
    }
    if (!g_dtx.layout) {
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li; memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; li.setLayoutCount = 1; li.pSetLayouts = &g_dtx.dsl;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(g.d, &li, NULL, &g_dtx.layout) != VK_SUCCESS) { g_dtx.layout = VK_NULL_HANDLE; return 0; }
    }
    return 1;
}

// 取（或建）一条 DrawTexture 管线。调用方持 g_dtx_mu。失败返回空（why 写原因）。
static VkPipeline dtx_pipe_get(struct nvn_gpu_cmd_s *c, const struct dtx_key *k, const nvn_gpu_draw_tex_desc *d, const char **why) {
    for (uint32_t i = 0; i < g_dtx.n; i++) if (!memcmp(&g_dtx.p[i].k, k, sizeof *k)) return g_dtx.p[i].pipe;
    for (struct dtx_pipe_temp *p = c->dtx_pipes; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p->pipe;
    if (!dtx_common()) { *why = "着色器 / 布局建不出"; return VK_NULL_HANDLE; }
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd);
    memcpy(pd.cfmt, k->cfmt, sizeof pd.cfmt); pd.dfmt = k->dsfmt; pd.samples = k->samples;
    VkRenderPass pass = make_pipeline_pass(&pd);
    if (!pass) { *why = "兼容渲染通道建不出"; return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_dtx.vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_dtx.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi; memset(&vi, 0, sizeof vi); vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp); vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples > 1 ? k->samples : 1);
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds); ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;   // 深度 / 模板测试都关
    VkPipelineColorBlendAttachmentState cb[8]; memset(cb, 0, sizeof cb);
    uint32_t nslot = 0;
    for (uint32_t i = 0; i < 8; i++) if (k->cfmt[i]) nslot = i + 1;
    cb[0].colorWriteMask = ((k->cmask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((k->cmask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                           ((k->cmask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((k->cmask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (k->blend_en) {
        const blend_st *b = &d->blend;
        VkBlendFactor sf = vk_blend_factor(b->src_rgb), df = vk_blend_factor(b->dst_rgb), sa = vk_blend_factor(b->src_a), da = vk_blend_factor(b->dst_a);
        VkBlendOp er = vk_blend_op(b->eq_rgb), ea = vk_blend_op(b->eq_a);
        if (sf == VK_BLEND_FACTOR_MAX_ENUM || df == VK_BLEND_FACTOR_MAX_ENUM || sa == VK_BLEND_FACTOR_MAX_ENUM ||
            da == VK_BLEND_FACTOR_MAX_ENUM || er == VK_BLEND_OP_MAX_ENUM || ea == VK_BLEND_OP_MAX_ENUM) {
            vkDestroyRenderPass(g.d, pass, NULL); *why = "混合因子 / 运算未实现"; return VK_NULL_HANDLE;
        }
        cb[0].blendEnable = VK_TRUE;
        cb[0].srcColorBlendFactor = sf; cb[0].dstColorBlendFactor = df; cb[0].colorBlendOp = er;
        cb[0].srcAlphaBlendFactor = sa; cb[0].dstAlphaBlendFactor = da; cb[0].alphaBlendOp = ea;
    }
    VkPipelineColorBlendStateCreateInfo cbs; memset(&cbs, 0, sizeof cbs); cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = nslot; cbs.pAttachments = cb;
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyns; memset(&dyns, 0, sizeof dyns); dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 3; dyns.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp); gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cbs; gp.pDynamicState = &dyns;
    gp.layout = g_dtx.layout; gp.renderPass = pass;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g.d, g_plc, 1, &gp, NULL, &pipe) != VK_SUCCESS) { vkDestroyRenderPass(g.d, pass, NULL); *why = "vkCreateGraphicsPipelines 失败"; return VK_NULL_HANDLE; }
    plc_created();
    if (g_dtx.n < DTX_MAX) {
        g_dtx.p[g_dtx.n].k = *k; g_dtx.p[g_dtx.n].pipe = pipe; g_dtx.p[g_dtx.n].pass = pass; g_dtx.n++;
    } else {
        struct dtx_pipe_temp *p = malloc(sizeof *p);
        if (!p) { vkDestroyPipeline(g.d, pipe, NULL); vkDestroyRenderPass(g.d, pass, NULL); *why = "临时管线记录分配失败"; return VK_NULL_HANDLE; }
        *p = (struct dtx_pipe_temp){*k, pipe, pass, c->dtx_pipes}; c->dtx_pipes = p;
    }
    return pipe;
}

void nvn_gpu_draw_tex(nvn_gpu_cmd ch, const nvn_gpu_draw_tex_desc *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "draw_tex 在通道外"); return; }
    if (!d || !d->src || !d->samp) { set_sticky(c, NVN_GPU_E_ARG, "draw_tex 源视图 / 采样器为空"); return; }
    for (int i = 0; i < 4; i++)
        if (!isfinite(d->dst[i]) || !isfinite(d->uv[i])) { set_sticky(c, NVN_GPU_E_ARG, "draw_tex 矩形不是有限数"); return; }
    if (!(d->dst[2] > 0) || !(d->dst[3] > 0)) { set_sticky(c, NVN_GPU_E_ARG, "draw_tex 目的矩形为空（%gx%g）", (double)d->dst[2], (double)d->dst[3]); return; }
    if (!(d->color_mask & 0xfu)) { set_sticky(c, NVN_GPU_E_ARG, "draw_tex 写掩码为 0"); return; }
    if (d->has_sc && (d->sc[0] < 0 || d->sc[1] < 0 || d->sc[2] <= 0 || d->sc[3] <= 0 ||
                      (uint64_t)d->sc[0] + (uint32_t)d->sc[2] > c->pass_w || (uint64_t)d->sc[1] + (uint32_t)d->sc[3] > c->pass_h)) {
        set_sticky(c, NVN_GPU_E_ARG, "draw_tex 裁剪 %d,%d %dx%d 为空或超出通道 %ux%u", d->sc[0], d->sc[1], d->sc[2], d->sc[3], c->pass_w, c->pass_h); return;
    }
    if (!(g.caps.caps & NVN_GPU_CAP_DRAW_TEX)) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_tex：没置 NVN_GPU_CAP_DRAW_TEX"); return; }
    if (!c->pass_ncolor || !(c->pass_color_mask & 1u)) { set_sticky(c, NVN_GPU_E_ARG, "draw_tex 通道没有颜色附件 0"); return; }
    nvn_fmt_info fi;
    if (nvn_fmt_info_of(c->pass_cfmt[0], &fi) && (fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_tex 颜色附件 0 是整数格式 0x%x", c->pass_cfmt[0]); return;
    }
    struct nvn_gpu_view_s *v = d->src;
    struct nvn_gpu_tex_s *t = v->tex;
    if (!t || v->buf || v->type != VK_IMAGE_VIEW_TYPE_2D || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || t->samples > 1 ||
        (nvn_fmt_info_of(v->nvn_format, &fi) && (fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT))) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_tex 源不是二维单采样浮点颜色视图（格式 0x%x）", v->nvn_format); return;
    }
    if (pass_attached(c, t)) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_tex 源是本通道的附件（反馈）"); return; }
    if (d->blend_en) {
        if (d->blend.adv_mode) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_tex 高级混合（adv_mode）"); return; }
        const uint32_t f4[4] = {d->blend.src_rgb, d->blend.dst_rgb, d->blend.src_a, d->blend.dst_a};
        for (int k = 0; k < 4; k++)
            if (f4[k] >= 0x10 && f4[k] <= 0x13) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_tex 双源混合因子（内置片元只有一个输出）"); return; }
    }
    if (c->feedback_read_mask && !feedback_restart(c, 0, 0)) return;
    VkRect2D sc = {{0, 0}, {c->pass_w, c->pass_h}};
    if (d->has_sc) sc = (VkRect2D){{d->sc[0], d->sc[1]}, {(uint32_t)d->sc[2], (uint32_t)d->sc[3]}};
    if (!pass_ensure_area(c, sc)) return;
    // 源的布局：nvk_opt_layout=1 时本段里它可能还停在附件布局，Vulkan 通道内不能迁，按通道边界重开迁好（同 prepare_bind_layouts）
    if (c->opt) {
        const VkImageLayout cur = lay_cur(c, t), want = sample_layout_for(t);
        if (cur != want && cur != VK_IMAGE_LAYOUT_GENERAL) {
            struct reopen_fix fx = {t, want};
            if (!pass_reopen(c, &fx, 1)) {
                if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_ORDER, "draw_tex 源要改布局，通道又拆不开");
                return;
            }
        }
    }
    struct dtx_key k; memset(&k, 0, sizeof k);
    memcpy(k.cfmt, c->pass_cfmt, sizeof k.cfmt);
    k.dsfmt = c->pass_dsfmt; k.samples = c->pass_samples; k.cmask = d->color_mask & 0xfu; k.blend_en = d->blend_en != 0;
    if (k.blend_en) {
        k.blend[0] = d->blend.src_rgb; k.blend[1] = d->blend.dst_rgb; k.blend[2] = d->blend.src_a | (uint32_t)d->blend.dst_a << 8;
        k.blend[3] = d->blend.eq_rgb | (uint32_t)d->blend.eq_a << 8;
    }
    const char *why = "";
    pthread_mutex_lock(&g_dtx_mu);
    VkPipeline pipe = dtx_pipe_get(c, &k, d, &why);
    VkPipelineLayout layout = g_dtx.layout; VkDescriptorSetLayout dsl = g_dtx.dsl;
    pthread_mutex_unlock(&g_dtx_mu);
    if (!pipe) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_tex：建不出内置管线（%s；颜色 0 格式 0x%x 采样 %u）", why, c->pass_cfmt[0], c->pass_samples); return; }
    VkDescriptorSet set;
    if (!alloc_sets(c, &dsl, 1, &set, NULL, 0, 0)) { if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_OOM, "draw_tex 描述符组分配失败"); return; }
    cmd_ref(c, v, REF_VIEW); cmd_ref(c, d->samp, REF_SAMP);
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    VkDescriptorImageInfo ii = {d->samp->sampler, v->view, bind_image_layout(c, v)};
    VkWriteDescriptorSet w; memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g.d, 1, &w, 0, NULL);
    es_reset(c);   // 换了内置管线、视口裁剪与混合常量、描述符组：下一笔绘制全部重发（同 clear_rect，#708）
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    VkViewport vp = {d->dst[0], d->dst[1], d->dst[2], d->dst[3], 0.0f, 1.0f};
    if (!d->origin_ll) { vp.y += vp.height; vp.height = -vp.height; }   // 同普通绘制（apply 视口那段）：原点在左上时裁剪空间 +y 落在行号小的一边
    vkCmdSetViewport(c->cb, 0, 1, &vp);
    vkCmdSetScissor(c->cb, 0, 1, &sc);
    vkCmdSetBlendConstants(c->cb, d->blend_color);
    vkCmdPushConstants(c->cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, d->uv);
    vkCmdDraw(c->cb, 4, 1, 0, 0);
    if (ROI_ACTIVE(c)) {
        unsigned draw=++c->roi->draws;
        roi_message("event=draw_texture frame=%d pass=%u draw=%u image=%llu view=%llu type=%d format=%d mip=%u+%u layer=%u+%u zero=%u",
                    roi_target,c->roi->pass,draw,(unsigned long long)access_id(&v->tex->access_id),
                    (unsigned long long)(uintptr_t)ii.imageView,v->type,v->format,v->base_level,v->levels,v->base_layer,v->layers,v->roi_zero);
    }
    c->desc_layout = VK_NULL_HANDLE; c->desc_dirty = 0xfu;   // 改绑了描述符组：下一笔全部重建（同 clear_rect）
}
