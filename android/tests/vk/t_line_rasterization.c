// VK_EXT_line_rasterization 合同测试（Mesa lavapipe + VVL 1.4.341 + SyncVal）。
// 验证：
//  1) mode=on：Vulkan 1.1 基线下协商 VK_EXT_line_rasterization，动态取 vkCmdSetLineStippleEXT（0 静态 EXT 符号）；
//     NVN 线段管线挂接 VkPipelineRasterizationLineStateCreateInfoEXT 并精确绘制水平红线；
//     动态 vkCmdSetLineStippleEXT(factor=1, pattern=0xAAAA) 实测在 32x32 帧缓冲上产生交替点亮/熄灭的虚线像素；VVL+SyncVal 0 错。
//  2) mode=off / hide-ext / hide-feat：不请求扩展/特性，线段管线不挂扩展结构、照常绘制实线，VVL+SyncVal 0 错。
//  3) mode=withdraw：关开关按启用期望验收，必报红退非零（负对照）。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_backend.h"
#include "nvn_plat.h"
#include "vk_device.h"

int nvn_plat_flag(const char *name, int dflt) { (void)name; return dflt; }
void nvn_plat_log(int level, const char *cls, const char *text) { (void)level; (void)cls; (void)text; }

extern VkResult __real_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
extern void __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
extern VkResult __real_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
extern VkResult __real_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);

static int g_hide_ext, g_hide_feat;
static unsigned g_dev_ext_enabled, g_dev_feat_enabled, g_pipe_line_state;

VkResult __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice phys, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (!g_hide_ext || layer) return __real_vkEnumerateDeviceExtensionProperties(phys, layer, count, props);
    uint32_t total = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(phys, NULL, &total, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *tmp = total ? calloc(total, sizeof *tmp) : NULL;
    if (total && !tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = __real_vkEnumerateDeviceExtensionProperties(phys, NULL, &total, tmp);
    if (r != VK_SUCCESS) { free(tmp); return r; }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (!strcmp(tmp[i].extensionName, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME)) continue;
        tmp[kept++] = tmp[i];
    }
    if (!props) { *count = kept; free(tmp); return VK_SUCCESS; }
    uint32_t n = *count < kept ? *count : kept;
    memcpy(props, tmp, n * sizeof *tmp);
    *count = n;
    free(tmp);
    return n < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

void __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice phys, VkPhysicalDeviceFeatures2 *features) {
    __real_vkGetPhysicalDeviceFeatures2(phys, features);
    if (!g_hide_feat || !features) return;
    for (VkBaseOutStructure *b = features->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT) {
            VkPhysicalDeviceLineRasterizationFeaturesEXT *lr = (VkPhysicalDeviceLineRasterizationFeaturesEXT *)b;
            lr->rectangularLines = VK_FALSE;
            lr->bresenhamLines = VK_FALSE;
            lr->smoothLines = VK_FALSE;
            lr->stippledRectangularLines = VK_FALSE;
            lr->stippledBresenhamLines = VK_FALSE;
            lr->stippledSmoothLines = VK_FALSE;
        }
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT) {
                const VkPhysicalDeviceLineRasterizationFeaturesEXT *lr = (const VkPhysicalDeviceLineRasterizationFeaturesEXT *)b;
                if (lr->rectangularLines || lr->bresenhamLines || lr->smoothLines ||
                    lr->stippledRectangularLines || lr->stippledBresenhamLines || lr->stippledSmoothLines)
                    g_dev_feat_enabled++;
            }
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, out);
}

VkResult __wrap_vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t count,
                                          const VkGraphicsPipelineCreateInfo *cis, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    for (uint32_t i = 0; cis && i < count; i++) {
        const VkPipelineRasterizationStateCreateInfo *rs = cis[i].pRasterizationState;
        for (const VkBaseInStructure *b = rs ? rs->pNext : NULL; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT)
                g_pipe_line_state++;
        }
    }
    return __real_vkCreateGraphicsPipelines(dev, cache, count, cis, alloc, out);
}

#define REQUIRE(cond) do { \
    if (!(cond)) { fprintf(stderr, "【红】fixture failed line %d: %s\n", __LINE__, #cond); return 1; } \
} while (0)
#define GPU(call) do { \
    nvn_gpu_rc rc_ = (call); \
    if (!nvn_gpu_ok(rc_)) { fprintf(stderr, "【红】gpu failed line %d: rc=%d why=%s\n", __LINE__, rc_.code, err.why); return 1; } \
} while (0)

static int load_spv(const char *path, void **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    *buf = malloc((size_t)sz);
    if (!*buf || fread(*buf, 1, (size_t)sz, f) != (size_t)sz) { free(*buf); fclose(f); return 0; }
    fclose(f);
    *len = (size_t)sz;
    return 1;
}

// 直接用 vk_device 动态解析的 dev->cmd_set_line_stipple 实测虚线像素（pattern=0xAAAA 交替亮灭）
static int run_dynamic_stipple_probe(struct s2a_vk_device *dev, const void *vspv, size_t vlen, const void *fspv, size_t flen) {
    PFN_vkCmdSetLineStippleEXT set_stipple = dev->cmd_set_line_stipple;
    if (!set_stipple) return 0;

    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    smci.codeSize = vlen; smci.pCode = (const uint32_t *)vspv;
    if (vkCreateShaderModule(dev->device, &smci, NULL, &vs) != VK_SUCCESS) return 0;
    smci.codeSize = flen; smci.pCode = (const uint32_t *)fspv;
    if (vkCreateShaderModule(dev->device, &smci, NULL, &fs) != VK_SUCCESS) { vkDestroyShaderModule(dev->device, vs, NULL); return 0; }

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {32, 32, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img = VK_NULL_HANDLE;
    if (vkCreateImage(dev->device, &ici, NULL, &img) != VK_SUCCESS) return 0;
    VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev->device, img, &imr);
    uint32_t imt = s2a_vk_memory_type(dev, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = imr.size, .memoryTypeIndex = imt };
    VkDeviceMemory imem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev->device, &imai, NULL, &imem) != VK_SUCCESS) return 0;
    vkBindImageMemory(dev->device, img, imem, 0);

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView iv = VK_NULL_HANDLE;
    if (vkCreateImageView(dev->device, &ivci, NULL, &iv) != VK_SUCCESS) return 0;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 32 * 32 * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer rb = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev->device, &bci, NULL, &rb) != VK_SUCCESS) return 0;
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev->device, rb, &bmr);
    uint32_t bmt = s2a_vk_memory_type(dev, bmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size, .memoryTypeIndex = bmt };
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev->device, &bmai, NULL, &bmem) != VK_SUCCESS) return 0;
    vkBindBufferMemory(dev->device, rb, bmem, 0);

    VkAttachmentDescription att = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference aref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &aref };
    VkSubpassDependency dep = {
        .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep,
    };
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev->device, &rpci, NULL, &rp) != VK_SUCCESS) return 0;
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &iv, .width = 32, .height = 32, .layers = 1,
    };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev->device, &fbci, NULL, &fb) != VK_SUCCESS) return 0;

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev->device, &plci, NULL, &pl) != VK_SUCCESS) return 0;

    VkPipelineShaderStageCreateInfo stg[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST };
    VkViewport vport = { 0, 0, 32, 32, 0, 1 };
    VkRect2D scis = { {0, 0}, {32, 32} };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vport, .scissorCount = 1, .pScissors = &scis };
    VkPipelineRasterizationLineStateCreateInfoEXT linfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT,
        .lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT,
        .stippledLineEnable = VK_TRUE, .lineStippleFactor = 1, .lineStipplePattern = 0xFFFFu,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .pNext = &linfo,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn_st[1] = { VK_DYNAMIC_STATE_LINE_STIPPLE_EXT };
    VkPipelineDynamicStateCreateInfo dyns = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = dyn_st };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stg,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
        .pDynamicState = &dyns, .layout = pl, .renderPass = rp,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS) return 0;

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = dev->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev->device, &cbai, &cb);
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &cbbi);
    VkClearValue clr = { .color = { .float32 = {0, 0, 0, 1} } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = { {0, 0}, {32, 32} }, .clearValueCount = 1, .pClearValues = &clr,
    };
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    set_stipple(cb, 1, 0xAAAAu);
    vkCmdDraw(cb, 2, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy bic = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { 32, 32, 1 },
    };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bic);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(dev->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(dev->queue);

    void *map = NULL;
    vkMapMemory(dev->device, bmem, 0, 32 * 32 * 4, 0, &map);
    const uint8_t *px = (const uint8_t *)map;
    int lit = 0, unlit_between = 0, prev_lit = -1, transitions = 0;
    for (int x = 4; x < 28; x++) {
        int is_red = (px[(16 * 32 + x) * 4 + 0] == 255 && px[(16 * 32 + x) * 4 + 1] == 0);
        if (is_red) lit++;
        else unlit_between++;
        if (prev_lit != -1 && prev_lit != is_red) transitions++;
        prev_lit = is_red;
    }
    vkUnmapMemory(dev->device, bmem);

    vkFreeCommandBuffers(dev->device, dev->command_pool, 1, &cb);
    vkDestroyPipeline(dev->device, pipe, NULL);
    vkDestroyPipelineLayout(dev->device, pl, NULL);
    vkDestroyFramebuffer(dev->device, fb, NULL);
    vkDestroyRenderPass(dev->device, rp, NULL);
    vkDestroyBuffer(dev->device, rb, NULL);
    vkFreeMemory(dev->device, bmem, NULL);
    vkDestroyImageView(dev->device, iv, NULL);
    vkDestroyImage(dev->device, img, NULL);
    vkFreeMemory(dev->device, imem, NULL);
    vkDestroyShaderModule(dev->device, fs, NULL);
    vkDestroyShaderModule(dev->device, vs, NULL);
    return lit >= 8 && unlit_between >= 8 && transitions >= 12;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-ext|hide-feat|withdraw>\n", argv[0]);
        return 2;
    }
    const char *sdir = argv[1];
    const char *mode = argv[2];
    const int is_withdraw = !strcmp(mode, "withdraw");
    if (!strcmp(mode, "off") || is_withdraw) {
        setenv("debug.s2a.nvk_line_raster", "0", 1);
    } else {
        unsetenv("debug.s2a.nvk_line_raster");
        if (!strcmp(mode, "hide-ext")) g_hide_ext = 1;
        else if (!strcmp(mode, "hide-feat")) g_hide_feat = 1;
        else if (strcmp(mode, "on") != 0) return 2;
    }
    const int expect_on = !strcmp(mode, "on") || is_withdraw;

    char vp[512], fp[512];
    snprintf(vp, sizeof vp, "%s/line_rast.vert.spv", sdir);
    snprintf(fp, sizeof fp, "%s/line_rast.frag.spv", sdir);
    void *vspv = NULL, *fspv = NULL;
    size_t vlen = 0, flen = 0;
    REQUIRE(load_spv(vp, &vspv, &vlen));
    REQUIRE(load_spv(fp, &fspv, &flen));

    struct s2a_vk_device dev = {0};
    char why[256] = "";
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.width = 32; td.d.height = 32; td.d.depth = 1; td.d.levels = 1;
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex rt = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err));
    nvn_gpu_view v_rt = NULL;
    GPU(nvn_gpu_view_create(rt, NULL, &v_rt, &err));
    nvn_gpu_buf rb_buf = NULL;
    GPU(nvn_gpu_buf_create(32 * 32 * 4, NVN_GPU_MEM_SHARED, NULL, &rb_buf, &err));
    memset(nvn_gpu_buf_cpu(rb_buf), 0, 32 * 32 * 4);

    nvn_gpu_layout empty_lay = {0};
    nvn_gpu_shader vsh = NULL, fsh = NULL;
    nvn_gpu_shader_src vsrc = { .stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV, .blob = vspv, .bytes = vlen, .entry = "main", .layout = &empty_lay };
    nvn_gpu_shader_src fsrc = { .stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV, .blob = fspv, .bytes = flen, .entry = "main", .layout = &empty_lay };
    GPU(nvn_gpu_shader_load(&vsrc, &vsh, &err));
    GPU(nvn_gpu_shader_load(&fsrc, &fsh, &err));

    nvn_gpu_pipe_desc pd = {0};
    pd.st[NVN_GPU_SH_VS].sh = vsh;
    pd.st[NVN_GPU_SH_FS].sh = fsh;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = NVN_DRAW_PRIMITIVE_LINES;
    pd.color.logic_op = NVN_LOGIC_OP_COPY;
    pd.chmask.mask = 0xFu;
    pd.poly.mode = 2;
    pd.origin_ll = 1;
    nvn_gpu_pipe gp = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &gp, &err));
    const unsigned backend_pipe_line_state = g_pipe_line_state;

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc pass = {0};
    pass.ncolor = 1;
    pass.color[0].view = v_rt;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 32; pass.h = 32; pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, gp);
    nvn_gpu_dyn dyn = {0};
    dyn.origin_ll = 1;
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 32; dyn.vp[0][3] = 32; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[0] = 0; dyn.sc[1] = 0; dyn.sc[2] = 32; dyn.sc[3] = 32;
    dyn.poly.mode = 2;
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_draw_args da = { .prim = NVN_DRAW_PRIMITIVE_LINES, .count = 2, .instances = 1 };
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    NVNcopyRegion reg = {0, 0, 0, 32, 32, 1};
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, &reg, rb_buf, 0, 32 * 4, 32 * 32 * 4);
    GPU(nvn_gpu_cmd_commit(cmd, 1, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));

    const uint8_t *px = (const uint8_t *)nvn_gpu_buf_cpu(rb_buf);
    int best_row_lit = 0, red_rows = 0;
    for (int y = 0; y < 32; y++) {
        int row_lit = 0;
        for (int x = 0; x < 32; x++) {
            const uint8_t *c = px + (y * 32 + x) * 4;
            if (c[0] == 255 && c[1] == 0 && c[2] == 0 && c[3] == 255) row_lit++;
        }
        if (row_lit > 0) red_rows++;
        if (row_lit > best_row_lit) best_row_lit = row_lit;
    }
    int bg_ok = (px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 255);
    const int line_px_ok = (red_rows == 1 && best_row_lit >= 20 && bg_ok);

    nvn_gpu_buf_release(rb_buf, NULL, NULL);
    nvn_gpu_view_release(v_rt);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fsh);
    nvn_gpu_shader_release(vsh);
    GPU(nvn_gpu_close(5000000000ull, &err));

    int stipple_px_ok = 0;
    if (g_dev_ext_enabled == 1 && g_dev_feat_enabled == 1) {
        stipple_px_ok = run_dynamic_stipple_probe(&dev, vspv, vlen, fspv, flen);
    }

    printf("line-rasterization mode=%s dev_ext=%u dev_feat=%u pipe_line=%u line_px=%d stipple_px=%d\n",
           mode, g_dev_ext_enabled, g_dev_feat_enabled, backend_pipe_line_state, line_px_ok, stipple_px_ok);

    s2a_vk_device_destroy(&dev, 1);
    free(vspv);
    free(fspv);

    REQUIRE(line_px_ok);
    if (expect_on) {
        if (g_dev_ext_enabled != 1 || g_dev_feat_enabled != 1 || backend_pipe_line_state == 0 || !stipple_px_ok) {
            fprintf(stderr, "【红】line_rasterization expected active: dev_ext=%u dev_feat=%u pipe_line=%u stipple_px=%d\n",
                    g_dev_ext_enabled, g_dev_feat_enabled, backend_pipe_line_state, stipple_px_ok);
            return 1;
        }
    } else {
        REQUIRE(g_dev_ext_enabled == 0 && g_dev_feat_enabled == 0 && backend_pipe_line_state == 0);
    }
    return 0;
}
