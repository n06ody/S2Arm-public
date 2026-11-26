// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 通道内的清除（nvn_gpu_clear_rect）一律用一笔整屏三角形画：裁剪 = 清除矩形，颜色值与深度值走 push 常量，
// 颜色写掩码 / 模板写掩码就是要清的那几位。不用 vkCmdClearAttachments，两条理由：它不看写掩码；
// 而且在高通的私有驱动上它的执行次序会乱（安卓模拟器的 Vulkan 后端因此完全不用它，见 compat/tree/src/Ryujinx.Graphics.Vulkan/PipelineFull.cs
// ClearRenderTargetColor 的注释）。这里不按厂商分支，所有设备走同一条路。
// 由 backend_vulkan.c include（要用它的 g、make_pipeline_pass、set_sticky）。管线按「通道的附件格式 + 槽 + 数值类别 + 掩码」缓存，会话关闭时销毁。
// 缓存满只影响复用：新键建段自有管线，随段完成 / 放弃回收，不能把合法清除变成整段失败。
// 下面四个数组是 android/native/nvn/clear_shaders/ 里四个 GLSL 用 glslangValidator -V --target-env vulkan1.1 编出来的 SPIR-V；
// 片元着色器的输出位置编的是 0，建模块时把那一个 Location 字改成目标颜色槽。
static const uint32_t k_clr_vs[323] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000030, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x0000001d, 0x00030003,
    0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009,
    0x00000071, 0x00060005, 0x0000000c, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00060005,
    0x0000001b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000001b, 0x00000000,
    0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001b, 0x00000001, 0x505f6c67, 0x746e696f,
    0x657a6953, 0x00000000, 0x00070006, 0x0000001b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x0000001b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
    0x00030005, 0x0000001d, 0x00000000, 0x00030005, 0x00000025, 0x00000050, 0x00050006, 0x00000025,
    0x00000000, 0x74706564, 0x00000068, 0x00030005, 0x00000027, 0x00000070, 0x00040047, 0x0000000c,
    0x0000000b, 0x0000002a, 0x00030047, 0x0000001b, 0x00000002, 0x00050048, 0x0000001b, 0x00000000,
    0x0000000b, 0x00000000, 0x00050048, 0x0000001b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048,
    0x0000001b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001b, 0x00000003, 0x0000000b,
    0x00000004, 0x00030047, 0x00000025, 0x00000002, 0x00050048, 0x00000025, 0x00000000, 0x00000023,
    0x00000010, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007,
    0x00000007, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001,
    0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e,
    0x00000001, 0x0004002b, 0x0000000a, 0x00000010, 0x00000002, 0x00040017, 0x00000017, 0x00000006,
    0x00000004, 0x00040015, 0x00000018, 0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019,
    0x00000001, 0x0004001c, 0x0000001a, 0x00000006, 0x00000019, 0x0006001e, 0x0000001b, 0x00000017,
    0x00000006, 0x0000001a, 0x0000001a, 0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b,
    0x0000001c, 0x0000001d, 0x00000003, 0x0004002b, 0x0000000a, 0x0000001e, 0x00000000, 0x0004002b,
    0x00000006, 0x00000020, 0x40000000, 0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x0003001e,
    0x00000025, 0x00000006, 0x00040020, 0x00000026, 0x00000009, 0x00000025, 0x0004003b, 0x00000026,
    0x00000027, 0x00000009, 0x00040020, 0x00000028, 0x00000009, 0x00000006, 0x00040020, 0x0000002e,
    0x00000003, 0x00000017, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003d, 0x0000000a, 0x0000000d,
    0x0000000c, 0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a,
    0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006, 0x00000012, 0x00000011, 0x0004003d,
    0x0000000a, 0x00000013, 0x0000000c, 0x000500c7, 0x0000000a, 0x00000014, 0x00000013, 0x00000010,
    0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012,
    0x00000015, 0x0003003e, 0x00000009, 0x00000016, 0x0004003d, 0x00000007, 0x0000001f, 0x00000009,
    0x0005008e, 0x00000007, 0x00000021, 0x0000001f, 0x00000020, 0x00050050, 0x00000007, 0x00000023,
    0x00000022, 0x00000022, 0x00050083, 0x00000007, 0x00000024, 0x00000021, 0x00000023, 0x00050041,
    0x00000028, 0x00000029, 0x00000027, 0x0000001e, 0x0004003d, 0x00000006, 0x0000002a, 0x00000029,
    0x00050051, 0x00000006, 0x0000002b, 0x00000024, 0x00000000, 0x00050051, 0x00000006, 0x0000002c,
    0x00000024, 0x00000001, 0x00070050, 0x00000017, 0x0000002d, 0x0000002b, 0x0000002c, 0x0000002a,
    0x00000022, 0x00050041, 0x0000002e, 0x0000002f, 0x0000001d, 0x0000001e, 0x0003003e, 0x0000002f,
    0x0000002d, 0x000100fd, 0x00010038,
};
static const uint32_t k_clr_fs_f[121] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00030005, 0x00000009, 0x0000006f, 0x00030005, 0x0000000a, 0x00000050, 0x00040006, 0x0000000a,
    0x00000000, 0x00000063, 0x00030005, 0x0000000c, 0x00000070, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00030047, 0x0000000a, 0x00000002, 0x00050048, 0x0000000a, 0x00000000, 0x00000023,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003,
    0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0003001e, 0x0000000a, 0x00000007,
    0x00040020, 0x0000000b, 0x00000009, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000009,
    0x00040015, 0x0000000d, 0x00000020, 0x00000001, 0x0004002b, 0x0000000d, 0x0000000e, 0x00000000,
    0x00040020, 0x0000000f, 0x00000009, 0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x0000000f, 0x00000010, 0x0000000c, 0x0000000e,
    0x0004003d, 0x00000007, 0x00000011, 0x00000010, 0x0003003e, 0x00000009, 0x00000011, 0x000100fd,
    0x00010038,
};
static const uint32_t k_clr_fs_u[122] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00030005, 0x00000009, 0x0000006f, 0x00030005, 0x0000000a, 0x00000050, 0x00040006, 0x0000000a,
    0x00000000, 0x00000063, 0x00030005, 0x0000000c, 0x00000070, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00030047, 0x0000000a, 0x00000002, 0x00050048, 0x0000000a, 0x00000000, 0x00000023,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008,
    0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0003001e, 0x0000000a,
    0x00000007, 0x00040020, 0x0000000b, 0x00000009, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c,
    0x00000009, 0x00040015, 0x0000000d, 0x00000020, 0x00000001, 0x0004002b, 0x0000000d, 0x0000000e,
    0x00000000, 0x00040020, 0x0000000f, 0x00000009, 0x00000007, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x0000000f, 0x00000010, 0x0000000c,
    0x0000000e, 0x0004003d, 0x00000007, 0x00000011, 0x00000010, 0x0003003e, 0x00000009, 0x00000011,
    0x000100fd, 0x00010038,
};
static const uint32_t k_clr_fs_i[118] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000011, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00030005, 0x00000009, 0x0000006f, 0x00030005, 0x0000000a, 0x00000050, 0x00040006, 0x0000000a,
    0x00000000, 0x00000063, 0x00030005, 0x0000000c, 0x00000070, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00030047, 0x0000000a, 0x00000002, 0x00050048, 0x0000000a, 0x00000000, 0x00000023,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
    0x00000020, 0x00000001, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008,
    0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0003001e, 0x0000000a,
    0x00000007, 0x00040020, 0x0000000b, 0x00000009, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c,
    0x00000009, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000, 0x00040020, 0x0000000e, 0x00000009,
    0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x00050041, 0x0000000e, 0x0000000f, 0x0000000c, 0x0000000d, 0x0004003d, 0x00000007, 0x00000010,
    0x0000000f, 0x0003003e, 0x00000009, 0x00000010, 0x000100fd, 0x00010038,
};

enum { CLR_MAX = 48 };
struct clr_key { uint32_t cfmt[8], dsfmt, samples; int32_t slot; uint32_t kind, cmask, depth, stencil; };   // slot = -1：不清颜色；depth / stencil：这一笔清不清深度 / 模板
struct clr_pipe_temp { struct clr_key k; VkPipeline pipe; VkRenderPass pass; struct clr_pipe_temp *next; };
static struct {
    VkShaderModule vs, fs[3][8];
    VkPipelineLayout layout;
    struct { struct clr_key k; VkPipeline pipe; VkRenderPass pass; } p[CLR_MAX];
    uint32_t n;
} g_clr;
static pthread_mutex_t g_clr_mu = PTHREAD_MUTEX_INITIALIZER;

// 与段的其它临时资源共用完成证明；不能驱逐或当场销毁仍被已录制 / 在途命令引用的管线。
static void clr_cmd_free(struct nvn_gpu_cmd_s *c) {
    while (c->clear_pipes) {
        struct clr_pipe_temp *p = c->clear_pipes; c->clear_pipes = p->next;
        vkDestroyPipeline(g.d, p->pipe, NULL);
        vkDestroyRenderPass(g.d, p->pass, NULL);
        free(p);
    }
}

static void clr_close(void) {   // 会话关闭：设备还在
    pthread_mutex_lock(&g_clr_mu);
    for (uint32_t i = 0; i < g_clr.n; i++) { vkDestroyPipeline(g.d, g_clr.p[i].pipe, NULL); vkDestroyRenderPass(g.d, g_clr.p[i].pass, NULL); }
    if (g_clr.vs) vkDestroyShaderModule(g.d, g_clr.vs, NULL);
    for (int k = 0; k < 3; k++) for (int s = 0; s < 8; s++) if (g_clr.fs[k][s]) vkDestroyShaderModule(g.d, g_clr.fs[k][s], NULL);
    if (g_clr.layout) vkDestroyPipelineLayout(g.d, g_clr.layout, NULL);
    memset(&g_clr, 0, sizeof g_clr);
    pthread_mutex_unlock(&g_clr_mu);
}

static VkShaderModule clr_module(const uint32_t *words, size_t n, int location) {
    uint32_t buf[512];
    if (n > sizeof buf / sizeof buf[0]) return VK_NULL_HANDLE;
    memcpy(buf, words, n * 4);
    if (location >= 0) {   // 改唯一的那条 OpDecorate … Location（操作码 71、修饰 30）
        int done = 0;
        for (size_t i = 5; i < n; i += buf[i] >> 16) {
            if (!(buf[i] >> 16)) return VK_NULL_HANDLE;
            if ((buf[i] & 0xffff) == 71 && (buf[i] >> 16) == 4 && buf[i + 2] == 30) { buf[i + 3] = (uint32_t)location; done++; }
        }
        if (done != 1) return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n * 4;
    ci.pCode = buf;
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(g.d, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// 取（或建）一条清除管线。调用方持 g_clr_mu。失败返回空。
static VkPipeline clr_pipe_get(struct nvn_gpu_cmd_s *c, const struct clr_key *k) {
    for (uint32_t i = 0; i < g_clr.n; i++) if (!memcmp(&g_clr.p[i].k, k, sizeof *k)) return g_clr.p[i].pipe;
    for (struct clr_pipe_temp *p = c->clear_pipes; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p->pipe;
    if (!g_clr.vs && !(g_clr.vs = clr_module(k_clr_vs, sizeof k_clr_vs / 4, -1))) return VK_NULL_HANDLE;
    if (!g_clr.layout) {
        VkPushConstantRange pr[2] = {{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16}, {VK_SHADER_STAGE_VERTEX_BIT, 16, 4}};   // 颜色 16 字节给片元，深度 4 字节给顶点
        VkPipelineLayoutCreateInfo li;
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.pushConstantRangeCount = 2;
        li.pPushConstantRanges = pr;
        if (vkCreatePipelineLayout(g.d, &li, NULL, &g_clr.layout) != VK_SUCCESS) { g_clr.layout = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    VkShaderModule fs = VK_NULL_HANDLE;
    if (k->slot >= 0) {
        static const uint32_t *const blob[3] = {k_clr_fs_f, k_clr_fs_u, k_clr_fs_i};
        static const size_t words[3] = {sizeof k_clr_fs_f / 4, sizeof k_clr_fs_u / 4, sizeof k_clr_fs_i / 4};
        if (!g_clr.fs[k->kind][k->slot] && !(g_clr.fs[k->kind][k->slot] = clr_module(blob[k->kind], words[k->kind], k->slot))) return VK_NULL_HANDLE;
        fs = g_clr.fs[k->kind][k->slot];
    }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    memcpy(pd.cfmt, k->cfmt, sizeof pd.cfmt);
    pd.dfmt = k->dsfmt;
    pd.samples = k->samples;
    VkRenderPass pass = make_pipeline_pass(&pd);
    if (!pass) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_clr.vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi; memset(&vi, 0, sizeof vi); vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp); vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples > 1 ? k->samples : 1);
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds); ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (k->depth) { ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS; }   // 清深度：恒通过、写成顶点给的值
    if (k->stencil) {   // 清模板：恒通过、三种结果都替换成参考值；写哪几位、写成什么走动态状态
        ds.stencilTestEnable = VK_TRUE;
        ds.front.failOp = ds.front.passOp = ds.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        ds.front.compareOp = VK_COMPARE_OP_ALWAYS;
        ds.front.compareMask = 0xff;
        ds.back = ds.front;
    }
    VkPipelineColorBlendAttachmentState cb[8];
    memset(cb, 0, sizeof cb);
    uint32_t nslot = 0;
    for (uint32_t i = 0; i < 8; i++) if (k->cfmt[i]) nslot = i + 1;
    if (k->slot >= 0)
        cb[k->slot].colorWriteMask = ((k->cmask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((k->cmask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                     ((k->cmask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((k->cmask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cbs; memset(&cbs, 0, sizeof cbs); cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = nslot; cbs.pAttachments = cb;
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dyns; memset(&dyns, 0, sizeof dyns); dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 4; dyns.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp); gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = k->slot >= 0 ? 2 : 1; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cbs; gp.pDynamicState = &dyns;
    gp.layout = g_clr.layout; gp.renderPass = pass;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g.d, g_plc, 1, &gp, NULL, &pipe) != VK_SUCCESS) { vkDestroyRenderPass(g.d, pass, NULL); return VK_NULL_HANDLE; }
    plc_created();
    if (g_clr.n < CLR_MAX) {
        g_clr.p[g_clr.n].k = *k; g_clr.p[g_clr.n].pipe = pipe; g_clr.p[g_clr.n].pass = pass; g_clr.n++;
    } else {
        struct clr_pipe_temp *p = malloc(sizeof *p);
        if (!p) {
            vkDestroyPipeline(g.d, pipe, NULL); vkDestroyRenderPass(g.d, pass, NULL);
            set_sticky(c, NVN_GPU_E_OOM, "清除临时管线记录分配失败");
            return VK_NULL_HANDLE;
        }
        *p = (struct clr_pipe_temp){*k, pipe, pass, c->clear_pipes}; c->clear_pipes = p;
    }
    return pipe;
}

// 在当前通道里画一笔清除。slot >= 0：颜色槽 slot 的 cmask 那几个通道写成 value；do_depth：深度写成 d->depth_value；
// smask != 0：模板的那几位写成 sval。三样可以同一笔做。
static void clr_draw(struct nvn_gpu_cmd_s *c, int slot, uint32_t cmask, const void *value16, int do_depth, uint32_t smask, uint32_t sval, const nvn_gpu_clear *d) {
    struct clr_key k;
    memset(&k, 0, sizeof k);
    memcpy(k.cfmt, c->pass_cfmt, sizeof k.cfmt);
    k.dsfmt = c->pass_dsfmt; k.samples = c->pass_samples; k.slot = slot; k.cmask = slot >= 0 ? cmask : 0; k.depth = do_depth != 0; k.stencil = smask != 0;
    if (slot >= 0) { nvn_fmt_info fi; k.kind = nvn_fmt_info_of(c->pass_cfmt[slot], &fi) ? (fi.kind == NVN_FMT_UINT ? 1u : fi.kind == NVN_FMT_SINT ? 2u : 0u) : 0u; }
    pthread_mutex_lock(&g_clr_mu);
    VkPipeline pipe = clr_pipe_get(c, &k);
    VkPipelineLayout layout = g_clr.layout;
    pthread_mutex_unlock(&g_clr_mu);
    if (!pipe) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "通道内清除：建不出清除管线（槽 %d 颜色掩码 %#x 深度 %d 模板掩码 %#x）", slot, cmask, do_depth, smask); return; }
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = {0, 0, (float)c->pass_w, (float)c->pass_h, 0.0f, 1.0f};
    float depth = do_depth ? d->depth_value : 0.0f;
    vkCmdPushConstants(c->cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 16, 4, &depth);
    VkRect2D sc = {{d->x, d->y}, {d->w, d->h}};
    vkCmdSetViewport(c->cb, 0, 1, &vp);
    vkCmdSetScissor(c->cb, 0, 1, &sc);
    vkCmdSetStencilWriteMask(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask & 0xffu);
    vkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval & 0xffu);
    if (slot >= 0) vkCmdPushConstants(c->cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, value16);
    gpu_pt_draw(c, NULL);
    vkCmdDraw(c->cb, 3, 1, 0, 0);
}
