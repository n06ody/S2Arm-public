// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// 可选的上屏 EASU 放大（AMD FidelityFX FSR1，MIT；原文与被移植的函数见 nvn_vk_easu_license.txt）。
// 默认关：开关是 debug.s2a.nvk_upscale（nvn_plat_flag("nvk_upscale",0)），发布包恒取缺省 0。
// 这里只提供「把源图合成到一张颜色附件」的一段录制；交换链创建参数一个字都不动
// （ci.imageUsage 本来就带 COLOR_ATTACHMENT_BIT）。由 backend_vulkan_wsi.inc 的 present 决定开/关。
#include "nvn_vk_easu_spv.h"

// 打包校验用的标记：libs2arm_android.so 里带上这串字符串就要求 APK 带 assets/licenses/easu-fsr1-LICENSE.txt；
// 不带就不求（见 android/verify-apk.py 的 LICENSE_MARKERS）。
__attribute__((used, retain)) static const char k_easu_license_marker[] = "S2ARM_EASU_ffx_fsr1_mit";

typedef struct {
    float con[4][4];
    float dstOrigin[2];
    float dstSize[2];
    uint32_t swap_rb;
    uint32_t encode_srgb;   // 1 = 目标是无编码语义的 UNORM 交换链图，按 sRGB 写编码字节
} easu_push;
_Static_assert(sizeof(easu_push) == 88, "push constant layout must match easu.frag");

// 8 位四通道（UNORM / SRGB）算同一个族：EASU 逐通道独立，字节序只影响写出时的分量次序。
static int easu_is_8bit_rgba(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM ||
           f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
}
static int easu_is_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
}
static int easu_is_bgr(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

#define EASU_FB_N 8
static struct {
    VkShaderModule vs, fs;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkSampler sampler;
    VkRenderPass pass;
    VkPipeline pipe;
    VkFormat fmt;
    int failed;
    // 交换链图是按 句柄+代 复用的：换代（重建交换链）时整表作废。
    struct { VkImage img; VkImageView view; VkFramebuffer fb; VkExtent2D ext; uint32_t level; } fb[EASU_FB_N];
    unsigned nfb, nextfb;
    uint32_t fgen;
} g_easu;
static pthread_mutex_t g_easu_mu = PTHREAD_MUTEX_INITIALIZER;

// 上屏 pass 的 GPU 时间（诊断）：一对时间戳包住这一 pass，隔一帧回读（不等待，不卡呈现线程）。
// 口径：只含这一 pass 的命令；不含取图等待、不含缓存刷新、不含呈现。
#define EASU_QSLOTS 8u
static struct {
    VkQueryPool pool;
    double period_ns;      // 每个时间戳计数换算成纳秒
    int inited;
    uint32_t slot;
    uint64_t sum_ns, n;
} g_easu_q;

static void easu_q_init_locked(void) {
    if (g_easu_q.inited) return;
    g_easu_q.inited = 1;
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g.phys, &pp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nq, NULL);
    if (!nq) return;
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    if (!qf) return;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nq, qf);
    uint32_t bits = 0;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].timestampValidBits > bits) bits = qf[i].timestampValidBits;
    free(qf);
    if (!bits || pp.limits.timestampPeriod <= 0.0f) return;   // 这个设备不记，上屏照旧
    VkQueryPoolCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = EASU_QSLOTS * 2u;
    if (vkCreateQueryPool(g.d, &ci, NULL, &g_easu_q.pool) != VK_SUCCESS) { g_easu_q.pool = VK_NULL_HANDLE; return; }
    g_easu_q.period_ns = (double)pp.limits.timestampPeriod;
}

static uint64_t g_easu_hits;   // 诊断：真正走了 EASU 通道的上屏次数（测试与真机读数用）
uint64_t s2a_vk_easu_hits(void) { return __atomic_load_n(&g_easu_hits, __ATOMIC_RELAXED); }

static void easu_fb_clear_locked(void) {
    for (unsigned i = 0; i < g_easu.nfb; i++) {
        if (g_easu.fb[i].fb) vkDestroyFramebuffer(g.d, g_easu.fb[i].fb, NULL);
        if (g_easu.fb[i].view) vkDestroyImageView(g.d, g_easu.fb[i].view, NULL);
    }
    memset(g_easu.fb, 0, sizeof g_easu.fb);
    g_easu.nfb = g_easu.nextfb = 0;
}

static void easu_close(void) {
    pthread_mutex_lock(&g_easu_mu);
    easu_fb_clear_locked();
    if (g_easu_q.pool) vkDestroyQueryPool(g.d, g_easu_q.pool, NULL);
    memset(&g_easu_q, 0, sizeof g_easu_q);
    if (g_easu.pipe) vkDestroyPipeline(g.d, g_easu.pipe, NULL);
    if (g_easu.pass) vkDestroyRenderPass(g.d, g_easu.pass, NULL);
    if (g_easu.fs) vkDestroyShaderModule(g.d, g_easu.fs, NULL);
    if (g_easu.vs) vkDestroyShaderModule(g.d, g_easu.vs, NULL);
    if (g_easu.layout) vkDestroyPipelineLayout(g.d, g_easu.layout, NULL);
    if (g_easu.dsl) vkDestroyDescriptorSetLayout(g.d, g_easu.dsl, NULL);
    if (g_easu.sampler) vkDestroySampler(g.d, g_easu.sampler, NULL);
    memset(&g_easu, 0, sizeof g_easu);
    pthread_mutex_unlock(&g_easu_mu);
}

static int easu_module(const uint32_t *words, size_t bytes, VkShaderModule *out) {
    VkShaderModuleCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = bytes; ci.pCode = words;
    return vkCreateShaderModule(g.d, &ci, NULL, out) == VK_SUCCESS;
}

// 调用者持 g_easu_mu。渲染通道只带一个颜色附件、开时清黑。
// 注意 LOAD_OP_CLEAR 只作用于渲染区：渲染区之外的边框由调用方先整张清黑。
// 首末布局都用 COLOR_ATTACHMENT_OPTIMAL：present 自己负责 UNDEFINED→附件、附件→PRESENT。
static VkAttachmentDescription easu_attachment(VkFormat fmt) {
    return (VkAttachmentDescription){
        .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
}

static int easu_pipelines_locked(VkFormat fmt) {
    if (g_easu.failed) return 0;
    easu_q_init_locked();
    if (g_easu.pipe && g_easu.fmt == fmt) return 1;
    if (g_easu.pipe) { vkDestroyPipeline(g.d, g_easu.pipe, NULL); g_easu.pipe = VK_NULL_HANDLE; }
    if (g_easu.pass) { vkDestroyRenderPass(g.d, g_easu.pass, NULL); g_easu.pass = VK_NULL_HANDLE; }
    easu_fb_clear_locked();   // 帧缓冲绑在旧通道上
    if (!g_easu.vs && !easu_module(k_easu_vs, sizeof k_easu_vs, &g_easu.vs)) { g_easu.failed = 1; return 0; }
    if (!g_easu.fs && !easu_module(k_easu_fs, sizeof k_easu_fs, &g_easu.fs)) { g_easu.failed = 1; return 0; }
    if (!g_easu.dsl) {
        VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci; memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; ci.bindingCount = 1; ci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(g.d, &ci, NULL, &g_easu.dsl) != VK_SUCCESS) { g_easu.failed = 1; return 0; }
    }
    if (!g_easu.layout) {
        VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(easu_push)};
        VkPipelineLayoutCreateInfo ci; memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; ci.setLayoutCount = 1; ci.pSetLayouts = &g_easu.dsl;
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(g.d, &ci, NULL, &g_easu.layout) != VK_SUCCESS) { g_easu.failed = 1; return 0; }
    }
    if (!g_easu.sampler) {
        // textureGather 不看过滤方式；地址模式要 CLAMP_TO_EDGE，边界按边缘像素延拓。
        VkSamplerCreateInfo ci; memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(g.d, &ci, NULL, &g_easu.sampler) != VK_SUCCESS) { g_easu.failed = 1; return 0; }
    }
    if (!g_easu.pass) {
        VkAttachmentDescription a = easu_attachment(fmt);
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo ci; memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; ci.attachmentCount = 1; ci.pAttachments = &a;
        ci.subpassCount = 1; ci.pSubpasses = &sp;
        if (vkCreateRenderPass(g.d, &ci, NULL, &g_easu.pass) != VK_SUCCESS) { g_easu.failed = 1; return 0; }
    }
    VkPipelineShaderStageCreateInfo stages[2]; memset(stages, 0, sizeof stages);
    for (unsigned i = 0; i < 2; i++) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = g_easu.vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = g_easu.fs;
    VkPipelineVertexInputStateCreateInfo vi; memset(&vi, 0, sizeof vi); vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp); vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba; memset(&ba, 0, sizeof ba); ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn; memset(&dyn, 0, sizeof dyn); dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;
    VkGraphicsPipelineCreateInfo ci; memset(&ci, 0, sizeof ci); ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &dyn;
    ci.layout = g_easu.layout; ci.renderPass = g_easu.pass;
    if (vkCreateGraphicsPipelines(g.d, g_plc, 1, &ci, NULL, &g_easu.pipe) != VK_SUCCESS) { g_easu.failed = 1; return 0; }
    plc_created();
    g_easu.fmt = fmt;
    return 1;
}

// 调用者持 g_easu_mu。交换链图句柄+代相同就复用帧缓冲；换代时整表重建。
static VkFramebuffer easu_framebuffer_locked(VkImage img, VkExtent2D ext, uint32_t level, uint32_t gen) {
    if (gen != g_easu.fgen) { easu_fb_clear_locked(); g_easu.fgen = gen; }
    for (unsigned i = 0; i < g_easu.nfb; i++)
        if (g_easu.fb[i].img == img && g_easu.fb[i].level == level &&
            g_easu.fb[i].ext.width == ext.width && g_easu.fb[i].ext.height == ext.height) return g_easu.fb[i].fb;
    unsigned slot;
    if (g_easu.nfb < EASU_FB_N) slot = g_easu.nfb++;
    else { slot = g_easu.nextfb; g_easu.nextfb = (g_easu.nextfb + 1) % EASU_FB_N;
           if (g_easu.fb[slot].fb) vkDestroyFramebuffer(g.d, g_easu.fb[slot].fb, NULL);
           if (g_easu.fb[slot].view) vkDestroyImageView(g.d, g_easu.fb[slot].view, NULL);
           memset(&g_easu.fb[slot], 0, sizeof g_easu.fb[slot]); }
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi); vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = g_easu.fmt;
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    if (vkCreateImageView(g.d, &vi, NULL, &g_easu.fb[slot].view) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkImageView att = g_easu.fb[slot].view;
    VkFramebufferCreateInfo fi; memset(&fi, 0, sizeof fi); fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = g_easu.pass; fi.attachmentCount = 1; fi.pAttachments = &att;
    fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
    if (vkCreateFramebuffer(g.d, &fi, NULL, &g_easu.fb[slot].fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_easu.fb[slot].img = img; g_easu.fb[slot].ext = ext; g_easu.fb[slot].level = level;
    return g_easu.fb[slot].fb;
}

// 能走 EASU 的前提：源与目标同格式（两侧都是 8 位 RGBA/BGRA，通道序不会在采样与写出之间交叉）、
// 单采样、源可采样。不满足就返回 0，调用方回落 blit 并打日志。
static int easu_eligible(struct nvn_gpu_tex_s *st, VkFormat dstfmt) {
    if (!st || st->samples != 1) return 0;
    if (!easu_is_8bit_rgba(st->format) || !easu_is_8bit_rgba(dstfmt)) return 0;
    // sRGB 源 + UNORM 目标：采样自动线性化，写出时按 encode 谓词编码（见 easu_compose 的 encode_srgb），允许。
    // 反向（UNORM 源 + sRGB 目标）不做：那会把原始字节当线性再编码，颜色错。
    if (!easu_is_srgb(st->format) && easu_is_srgb(dstfmt)) return 0;
    if (!(st->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return 0;
    return 1;
}

// 与实际录制共用归一化矩形，WSI 可在任何临时 clear/barrier 前识别自然回退。
struct easu_rects {
    uint32_t lw, lh;
    int32_t sx, sy, sw, sh, dx, dy, dw, dh;
};
enum easu_rect_fit { EASU_RECT_INVALID, EASU_RECT_SKIP, EASU_RECT_ATTEMPT };
// 无效矩形仍按原失败路径诊断；同尺寸/双轴整数倍率自然跳过。
static int easu_rects_prepare(struct nvn_gpu_view_s *sv, VkExtent2D dstExtent,
                             const int32_t crop[4], const int32_t dstrect[4], struct easu_rects *out) {
    struct nvn_gpu_tex_s *st = sv->tex;
    uint32_t lw = st->w >> sv->base_level, lh = st->h >> sv->base_level;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    int32_t sx = crop[0], sy = crop[1], sw = crop[2], sh = crop[3];
    if (sw <= 0 || sh <= 0) { sx = 0; sy = 0; sw = (int32_t)lw; sh = (int32_t)lh; }
    int32_t dx = dstrect[0], dy = dstrect[1], dw = dstrect[2], dh = dstrect[3];
    if (dw <= 0 || dh <= 0) { dx = 0; dy = 0; dw = (int32_t)dstExtent.width; dh = (int32_t)dstExtent.height; }
    if (sx < 0 || sy < 0 || (uint32_t)(sx + sw) > lw || (uint32_t)(sy + sh) > lh ||
        dx < 0 || dy < 0 || (uint32_t)(dx + dw) > dstExtent.width || (uint32_t)(dy + dh) > dstExtent.height) return EASU_RECT_INVALID;
    *out = (struct easu_rects){lw, lh, sx, sy, sw, sh, dx, dy, dw, dh};
    // 整数倍或同尺寸没有上采样的余量，交给原来的 blit。
    if (sw == dw && sh == dh) return EASU_RECT_SKIP;
    if (dw % sw == 0 && dh % sh == 0) return EASU_RECT_SKIP;

    return EASU_RECT_ATTEMPT;
}

// 把 sv 按 crop→dst 放大画到 dstImg 的 dstrect 区域。返回 1 = 已录制；0 = 无法录制（调用方回落）。
static int easu_compose(struct nvn_gpu_cmd_s *c, VkImage dstImg, VkExtent2D dstExtent, uint32_t dstLevel,
                        VkFormat dstfmt, struct nvn_gpu_view_s *sv, const struct easu_rects *rect,
                        int flip_y, uint32_t surface_gen, int encode_srgb) {
    struct nvn_gpu_tex_s *st = sv->tex;
    uint32_t lw = rect->lw, lh = rect->lh;
    int32_t sx = rect->sx, sy = rect->sy, sw = rect->sw, sh = rect->sh;
    int32_t dx = rect->dx, dy = rect->dy, dw = rect->dw, dh = rect->dh;

    pthread_mutex_lock(&g_easu_mu);
    int ready = easu_pipelines_locked(dstfmt);   // 通道与管线按目标格式建（源可以是另一种字节序）
    VkFramebuffer fb = ready ? easu_framebuffer_locked(dstImg, dstExtent, dstLevel, surface_gen) : VK_NULL_HANDLE;
    VkQueryPool qp = g_easu_q.pool;
    pthread_mutex_unlock(&g_easu_mu);
    // 隔一帧回读上一槽：池里两帧前的命令多半已完成，取不到就跳过，绝不等待。
    if (qp && g_easu_q.slot > 0) {
        uint32_t prev = (g_easu_q.slot + EASU_QSLOTS - 1u) % EASU_QSLOTS;
        uint64_t r[2];
        if (vkGetQueryPoolResults(g.d, qp, prev * 2u, 2, sizeof r, r, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && r[1] > r[0]) {
            g_easu_q.sum_ns += (uint64_t)((double)(r[1] - r[0]) * g_easu_q.period_ns);
            if (++g_easu_q.n >= 300) {
                char b[220];
                snprintf(b, sizeof b, "EASU 上屏 pass GPU 时间（近 %llu 帧平均）：%.3f 毫秒（只含该 pass，不含取图等待与缓存刷新）",
                         (unsigned long long)g_easu_q.n, (double)g_easu_q.sum_ns / (double)g_easu_q.n / 1e6);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b);
                g_easu_q.sum_ns = 0; g_easu_q.n = 0;
            }
        }
    }
    if (!fb) return 0;

    VkDescriptorSet set;
    if (!alloc_sets(c, &g_easu.dsl, 1, &set, NULL, 0, 0)) return 0;   // 超分自己的描述符组不属于任何管线，不进回收（#1096）
    VkDescriptorImageInfo img; memset(&img, 0, sizeof img);
    img.sampler = g_easu.sampler; img.imageView = sv->view; img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wr; memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr.dstSet = set; wr.dstBinding = 0;
    wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &img;
    vkUpdateDescriptorSets(g.d, 1, &wr, 0, NULL);
    cmd_ref(c, sv, REF_VIEW);

    easu_push p; memset(&p, 0, sizeof p);
    const float fw = (float)lw, fh = (float)lh;
    const float sx_scale = (float)sw / (float)dw, sy_scale = (float)sh / (float)dh;
    // 输出像素中心 -> 源存储坐标（y 向下），裁剪偏移与垂直翻转都折进这一条仿射。
    p.con[0][0] = sx_scale;
    p.con[0][2] = (float)sx + 0.5f * sx_scale - 0.5f;
    p.con[0][1] = flip_y ? -sy_scale : sy_scale;
    p.con[0][3] = flip_y ? ((float)lh - 1.0f) - ((float)sy + 0.5f * sy_scale - 0.5f)
                         : ((float)sy + 0.5f * sy_scale - 0.5f);
    p.con[1][0] = 1.0f / fw; p.con[1][1] = 1.0f / fh;
    p.con[1][2] = 1.0f / fw; p.con[1][3] = -1.0f / fh;
    p.con[2][0] = -1.0f / fw; p.con[2][1] = 2.0f / fh;
    p.con[2][2] = 1.0f / fw;  p.con[2][3] = 2.0f / fh;
    p.con[3][0] = 0.0f; p.con[3][1] = 4.0f / fh; p.con[3][2] = 0.0f; p.con[3][3] = 0.0f;
    p.dstOrigin[0] = (float)dx; p.dstOrigin[1] = (float)dy;
    p.dstSize[0] = (float)dw; p.dstSize[1] = (float)dh;
    p.swap_rb = (easu_is_bgr(st->format) != easu_is_bgr(dstfmt)) ? 1u : 0u;
    // 与 backend_vulkan_wsi.inc 的 encode 谓词同一口径：sRGB 源 + 非线性 UNORM 交换链要写编码后的字节。
    p.encode_srgb = encode_srgb ? 1u : 0u;

    full_barrier(c);
    image_barrier(c, st, VK_IMAGE_LAYOUT_GENERAL);
    VkViewport vp = {(float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f};
    VkRect2D sc = {{dx, dy}, {(uint32_t)dw, (uint32_t)dh}};
    VkClearValue clear; memset(&clear, 0, sizeof clear); clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rp; memset(&rp, 0, sizeof rp); rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = g_easu.pass; rp.framebuffer = fb;
    rp.renderArea = sc; rp.clearValueCount = 1; rp.pClearValues = &clear;
    uint32_t qs = g_easu_q.slot % EASU_QSLOTS;
    if (qp) {
        vkCmdResetQueryPool(c->cb, qp, qs * 2u, 2);
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, qs * 2u);
    }
    if (g.pass_diag) {
        struct gpu_pass_meta *meta = gpu_diag_record_pass(c, sc.extent.width, sc.extent.height, 1);
        if (meta) {
            VkAttachmentDescription att = easu_attachment(dstfmt);
            gpu_diag_attachment(meta, 0, &att, dstExtent.width, dstExtent.height);
        }
        if (c->tsq && c->ntsp < TS_PASS_MAX)
            vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 2 + 2 * c->ntsp);
    }
    vkCmdBeginRenderPass(c->cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_easu.pipe);
    vkCmdSetViewport(c->cb, 0, 1, &vp);
    vkCmdSetScissor(c->cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_easu.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(c->cb, g_easu.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(c->cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(c->cb);
    if (g.pass_diag) gpu_diag_pass_end(c);
    if (qp) {
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, qs * 2u + 1u);
        g_easu_q.slot++;
    }
    // 抽象绑定/动态状态仍属于调用方；物理命令缓冲被辅助绘制改过，下一笔须全部重发。
    es_reset(c); memset(c->dsh_ok, 0, sizeof c->dsh_ok); c->desc_point = 0;
    __atomic_add_fetch(&g_easu_hits, 1, __ATOMIC_RELAXED);
    return 1;
}