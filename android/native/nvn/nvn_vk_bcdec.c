// 见 nvn_vk_bcdec.h。路由表覆盖全部 16 个 NVN BC 格式；着色器为 Granite decode 组
// （s3tc=BC1/2/3，rgtc=BC4/5，bc6=BC6H，bc7=BC7），变体经 specialization constant 选定。
#include "nvn_vk_bcdec.h"
#include "nvn_bc_spv.h"
#include "vk_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 管线变体：shader 索引 + 两个 specialization constant 值（无第二常量时 c1 未用）。
struct variant { int shader; uint32_t c0, c1; };
enum { SH_S3TC, SH_RGTC, SH_BC6, SH_BC7 };

// 每格式：变体、dst 存储视图格式、解码后采样格式、族名（失败上报用）、块字节数。
struct route { struct variant v; VkFormat view_fmt; VkFormat decoded_fmt; VkFormat image_fmt; const char *family; int block_bytes; };
static const struct route ROUTES[0x51] = {
    [0x41] = {{SH_S3TC, 0, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 8},
    [0x42] = {{SH_S3TC, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 8},
    [0x43] = {{SH_S3TC, 1, 2}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 16},
    [0x44] = {{SH_S3TC, 1, 3}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 16},
    [0x45] = {{SH_S3TC, 0, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 8},
    [0x46] = {{SH_S3TC, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 8},
    [0x47] = {{SH_S3TC, 1, 2}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 16},
    [0x48] = {{SH_S3TC, 1, 3}, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, "s3tc", 16},
    [0x49] = {{SH_RGTC, 0, 0}, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, "rgtc", 8},
    [0x4a] = {{SH_RGTC, 0, 1}, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_SNORM, "rgtc", 8},
    [0x4b] = {{SH_RGTC, 1, 0}, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, "rgtc", 16},
    [0x4c] = {{SH_RGTC, 1, 1}, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_SNORM, "rgtc", 16},
    [0x4d] = {{SH_BC7, 0, 0}, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, "bc7", 16},
    [0x4e] = {{SH_BC7, 0, 0}, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, "bc7", 16},
    [0x4f] = {{SH_BC6, 1, 0}, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, "bc6", 16},
    [0x50] = {{SH_BC6, 0, 0}, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, "bc6", 16},
};
#define N_VARIANTS 11

int nvn_vk_bcdec_route(uint32_t f, VkFormat *view_fmt, VkFormat *decoded_fmt, VkFormat *image_fmt,
                       const char **family, int *block_bytes) {
    if (f >= 0x41 && f <= 0x50 && ROUTES[f].family) {
        const struct route *r = &ROUTES[f];
        if (view_fmt) *view_fmt = r->view_fmt;
        if (decoded_fmt) *decoded_fmt = r->decoded_fmt;
        if (image_fmt) *image_fmt = r->image_fmt;
        if (family) *family = r->family;
        if (block_bytes) *block_bytes = r->block_bytes;
        return 1;
    }
    return 0;
}

struct nvn_vk_bcdec {
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkPipelineLayout pl;
    VkShaderModule mod[4];
    VkPipeline pipe[N_VARIANTS];
    // 提交未证明完成的保留资源（与 texcache 账本同级：等 fence 后再放）。
    struct retained { VkFence fence; VkCommandBuffer cmd; VkBuffer buf; VkDeviceMemory mem;
                      VkImage img; VkDeviceMemory img_mem; VkImageView view, out_view; VkDescriptorSet ds;
                      struct retained *next; } *kept;
};

static const uint32_t *SPV[4];
static size_t SPV_LEN[4];

static void spv_init(void) {
    SPV[SH_S3TC] = nvn_bc_s3tc_comp; SPV_LEN[SH_S3TC] = sizeof(nvn_bc_s3tc_comp);
    SPV[SH_RGTC] = nvn_bc_rgtc_comp; SPV_LEN[SH_RGTC] = sizeof(nvn_bc_rgtc_comp);
    SPV[SH_BC6] = nvn_bc_bc6_comp; SPV_LEN[SH_BC6] = sizeof(nvn_bc_bc6_comp);
    SPV[SH_BC7] = nvn_bc_bc7_comp; SPV_LEN[SH_BC7] = sizeof(nvn_bc_bc7_comp);
}

// ROUTES 的 11 个去重变体，按管线数组下标。
static const struct variant VARIANTS[N_VARIANTS] = {
    {SH_S3TC, 0, 1}, {SH_S3TC, 1, 1}, {SH_S3TC, 1, 2}, {SH_S3TC, 1, 3},   // BC1 RGB/RGBA, BC2, BC3（UNORM/SRGB 同管线）
    {SH_RGTC, 0, 0}, {SH_RGTC, 0, 1}, {SH_RGTC, 1, 0}, {SH_RGTC, 1, 1},
    {SH_BC6, 0, 0}, {SH_BC6, 1, 0},
    {SH_BC7, 0, 0},
};

static int variant_index(const struct variant *v) {
    for (int i = 0; i < N_VARIANTS; i++)
        if (VARIANTS[i].shader == v->shader && VARIANTS[i].c0 == v->c0 && VARIANTS[i].c1 == v->c1) return i;
    return -1;
}

int nvn_vk_bcdec_create(struct s2a_vk_device *d, struct nvn_vk_bcdec **out, char *why, size_t why_size) {
    spv_init();
    struct nvn_vk_bcdec *bc = calloc(1, sizeof *bc);
    if (!bc) { snprintf(why, why_size, "bcdec: out of memory"); return -1; }

    VkDescriptorSetLayoutBinding bind[2] = {0};
    bind[0].binding = 0; bind[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bind[0].descriptorCount = 1; bind[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bind[1].binding = 1; bind[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bind[1].descriptorCount = 1; bind[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = 2; dli.pBindings = bind;
    if (vkCreateDescriptorSetLayout(d->device, &dli, NULL, &bc->dsl) != VK_SUCCESS) {
        snprintf(why, why_size, "bcdec: descriptor set layout failed"); goto fail;
    }
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 8; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->device, &dpi, NULL, &bc->pool) != VK_SUCCESS) {
        snprintf(why, why_size, "bcdec: descriptor pool failed"); goto fail;
    }
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &bc->dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(d->device, &pli, NULL, &bc->pl) != VK_SUCCESS) {
        snprintf(why, why_size, "bcdec: pipeline layout failed"); goto fail;
    }
    static const char *SH_NAME[4] = { "s3tc", "rgtc", "bc6", "bc7" };
    for (int s = 0; s < 4; s++) {
        VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = SPV_LEN[s]; mi.pCode = SPV[s];
        if (vkCreateShaderModule(d->device, &mi, NULL, &bc->mod[s]) != VK_SUCCESS) {
            snprintf(why, why_size, "bcdec: %s shader module failed", SH_NAME[s]); goto fail;
        }
    }
    // 建管线即验证族可用性：任一变体失败 → 报族名，整局不进直连（owner 裁定）。
    for (int i = 0; i < N_VARIANTS; i++) {
        const struct variant *v = &VARIANTS[i];
        VkSpecializationMapEntry me[2] = {
            { 0, 0, 4 }, { 1, 4, 4 } };
        uint32_t cv[2] = { v->c0, v->c1 };
        VkSpecializationInfo si = { 2, me, 8, cv };
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, bc->mod[v->shader], "main", &si };
        ci.layout = bc->pl;
        if (vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &ci, NULL, &bc->pipe[i]) != VK_SUCCESS) {
            static const char *FAM[N_VARIANTS] = { "s3tc","s3tc","s3tc","s3tc","rgtc","rgtc","rgtc","rgtc","bc6","bc6","bc7" };
            snprintf(why, why_size, "bcdec: %s compute pipeline failed (variant %u/%u)",
                     FAM[i], v->c0, v->c1); goto fail;
        }
    }
    *out = bc;
    return 0;
fail:
    nvn_vk_bcdec_destroy(d, bc);
    return -1;
}

int nvn_vk_bcdec_wait(struct s2a_vk_device *d, struct nvn_vk_bcdec *bc, uint64_t timeout_ns) {
    if (!bc) return 0;
    uint32_t n = 0;
    for (struct retained *e = bc->kept; e; e = e->next) n++;
    if (n) {
        VkFence *fences = malloc((size_t)n * sizeof *fences);
        if (!fences) return -1;
        uint32_t i = 0; for (struct retained *e = bc->kept; e; e = e->next) fences[i++] = e->fence;
        VkResult result = s2a_vk_wait_fences(d->device, n, fences, timeout_ns);
        free(fences);
        if (result != VK_SUCCESS) return -1;
    }
    while (bc->kept) {
        struct retained *e = bc->kept;
        bc->kept = e->next;
        if (e->fence) vkDestroyFence(d->device, e->fence, NULL);
        if (e->cmd) vkFreeCommandBuffers(d->device, d->command_pool, 1, &e->cmd);
        if (e->ds) vkFreeDescriptorSets(d->device, bc->pool, 1, &e->ds);
        if (e->buf) vkDestroyBuffer(d->device, e->buf, NULL);
        if (e->mem) vkFreeMemory(d->device, e->mem, NULL);
        if (e->out_view) vkDestroyImageView(d->device, e->out_view, NULL);
        if (e->view) vkDestroyImageView(d->device, e->view, NULL);
        if (e->img) vkDestroyImage(d->device, e->img, NULL);
        if (e->img_mem) vkFreeMemory(d->device, e->img_mem, NULL);
        free(e);
    }
    return 0;
}

void nvn_vk_bcdec_destroy(struct s2a_vk_device *d, struct nvn_vk_bcdec *bc) {
    if (!bc || nvn_vk_bcdec_wait(d, bc, 1000000000ull) != 0) return;
    for (int i = 0; i < N_VARIANTS; i++) if (bc->pipe[i]) vkDestroyPipeline(d->device, bc->pipe[i], NULL);
    for (int i = 0; i < 4; i++) if (bc->mod[i]) vkDestroyShaderModule(d->device, bc->mod[i], NULL);
    if (bc->pl) vkDestroyPipelineLayout(d->device, bc->pl, NULL);
    if (bc->pool) vkDestroyDescriptorPool(d->device, bc->pool, NULL);
    if (bc->dsl) vkDestroyDescriptorSetLayout(d->device, bc->dsl, NULL);
    free(bc);
}

int nvn_vk_bcdec_decode(struct nvn_vk_bcdec *bc, struct s2a_vk_device *d,
                        VkImage dst, uint32_t nvn_format, uint32_t w, uint32_t h,
                        uint32_t mip, uint32_t layer, const void *blocks, size_t bytes,
                        char *why, size_t why_size) {
    VkFormat view_fmt; const char *family; int bb;
    if (!nvn_vk_bcdec_route(nvn_format, &view_fmt, NULL, NULL, &family, &bb)) {
        snprintf(why, why_size, "bcdec: no route for nvn format 0x%02x", nvn_format); return -1;
    }
    int vi = variant_index(&ROUTES[nvn_format].v);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t nblocks = (size_t)bw * bh;
    if (bytes < nblocks * (size_t)bb) {
        snprintf(why, why_size, "bcdec: %s input %zu < %zu blocks", family, bytes, nblocks * (size_t)bb); return -1;
    }
    // 必须在提交前保留账本节点：提交后不能因分配失败丢失在途资源。
    struct retained *keep = calloc(1, sizeof *keep);
    if (!keep) { snprintf(why, why_size, "bcdec: retention allocation failed"); return -1; }

    // 临时资源一次建齐，任一失败跳 cleanup 全回收（未提交语义）。
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage in = VK_NULL_HANDLE; VkDeviceMemory in_mem = VK_NULL_HANDLE;
    VkImageView in_view = VK_NULL_HANDLE, out_view = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    int submitted = 0;
    char fail_why[128] = "bcdec: resource setup failed";

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = nblocks * 16; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(d->device, &bci, NULL, &buf) != VK_SUCCESS) goto cleanup;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (vkAllocateMemory(d->device, &mai, NULL, &mem) != VK_SUCCESS) goto cleanup;
    vkBindBufferMemory(d->device, buf, mem, 0);
    void *p;
    if (vkMapMemory(d->device, mem, 0, bci.size, 0, &p) != VK_SUCCESS) goto cleanup;
    // 8 字节块打包进 16 字节 texel（输入恒 RGBA32_UINT）；16 字节块原样拷。
    if (bb == 8) {
        const uint8_t *s = blocks; uint8_t *t = p;
        for (size_t i = 0; i < nblocks; i++) { memcpy(t, s, 8); memset(t + 8, 0, 8); s += 8; t += 16; }
    } else memcpy(p, blocks, nblocks * 16);
    vkUnmapMemory(d->device, mem);

    {
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R32G32B32A32_UINT;
        ici.extent = (VkExtent3D){bw, bh, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(d->device, &ici, NULL, &in) != VK_SUCCESS) goto cleanup;
        VkMemoryRequirements imr; vkGetImageMemoryRequirements(d->device, in, &imr);
        VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        imai.allocationSize = imr.size; imai.memoryTypeIndex = s2a_vk_memory_type(d, imr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
        if (vkAllocateMemory(d->device, &imai, NULL, &in_mem) != VK_SUCCESS) goto cleanup;
        vkBindImageMemory(d->device, in, in_mem, 0);
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = in; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ici.format;
        vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(d->device, &vci, NULL, &in_view) != VK_SUCCESS) goto cleanup;
        VkImageViewCreateInfo ovci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ovci.image = dst; ovci.viewType = VK_IMAGE_VIEW_TYPE_2D; ovci.format = view_fmt;
        ovci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
        if (vkCreateImageView(d->device, &ovci, NULL, &out_view) != VK_SUCCESS) {
            snprintf(fail_why, sizeof fail_why, "bcdec: %s output view failed", family); goto cleanup;
        }
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = bc->pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &bc->dsl;
        if (vkAllocateDescriptorSets(d->device, &dai, &ds) != VK_SUCCESS) goto cleanup;
        VkDescriptorImageInfo oi = { VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo ii = { VK_NULL_HANDLE, in_view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr[2] = {0};
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = ds; wr[0].dstBinding = 0;
        wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[0].pImageInfo = &oi;
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = ds; wr[1].dstBinding = 1;
        wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(d->device, 2, wr, 0, NULL);

        VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbi.commandPool = d->command_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(d->device, &cbi, &cmd) != VK_SUCCESS) goto cleanup;
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) goto cleanup;
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = in; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy cp = {0};
        cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = (VkExtent3D){bw, bh, 1};
        vkCmdCopyBufferToImage(cmd, buf, in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        // dst：整个子资源被覆写 → UNDEFINED 恒合法（保留语义与 upload() 一致：停 GENERAL）。
        VkImageMemoryBarrier db = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = dst; db.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        db.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc->pipe[vi]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc->pl, 0, 1, &ds, 0, NULL);
        int32_t res[2] = { (int32_t)w, (int32_t)h };
        vkCmdPushConstants(cmd, bc->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, res);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        db.oldLayout = VK_IMAGE_LAYOUT_GENERAL; db.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        db.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) goto cleanup;
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(d->device, &fci, NULL, &fence) != VK_SUCCESS) goto cleanup;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(d->queue, 1, &si, fence) != VK_SUCCESS) {
            snprintf(fail_why, sizeof fail_why, "bcdec: %s dispatch submit failed", family); goto cleanup;
        }
        submitted = 1;
        if (s2a_vk_wait_fences(d->device, 1, &fence, 5000000000ull) != VK_SUCCESS) {
            // 提交成功但没有完成证明：转入保留账本，资源不得释放/复用。
            keep->fence = fence; keep->cmd = cmd; keep->buf = buf; keep->mem = mem;
            keep->img = in; keep->img_mem = in_mem; keep->view = in_view; keep->out_view = out_view; keep->ds = ds;
            keep->next = bc->kept; bc->kept = keep;
            snprintf(why, why_size, "bcdec: %s dispatch completion unproven", family);
            return -2;
        }
    }
cleanup:
    free(keep);
    if (!submitted) {
        if (cmd) vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        if (fence) vkDestroyFence(d->device, fence, NULL);
        if (ds) vkFreeDescriptorSets(d->device, bc->pool, 1, &ds);
        if (out_view) vkDestroyImageView(d->device, out_view, NULL);
        if (in_view) vkDestroyImageView(d->device, in_view, NULL);
        if (in) vkDestroyImage(d->device, in, NULL);
        if (in_mem) vkFreeMemory(d->device, in_mem, NULL);
        if (buf) vkDestroyBuffer(d->device, buf, NULL);
        if (mem) vkFreeMemory(d->device, mem, NULL);
        snprintf(why, why_size, "%s", fail_why);
        return -1;
    }
    // 完成路径：临时资源全回收。
    vkDestroyFence(d->device, fence, NULL);
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
    vkFreeDescriptorSets(d->device, bc->pool, 1, &ds);
    vkDestroyImageView(d->device, out_view, NULL);
    vkDestroyImageView(d->device, in_view, NULL);
    vkDestroyImage(d->device, in, NULL);
    vkFreeMemory(d->device, in_mem, NULL);
    vkDestroyBuffer(d->device, buf, NULL);
    vkFreeMemory(d->device, mem, NULL);
    return 0;
}

void nvn_vk_bcdec_rec_free(struct s2a_vk_device *d, struct nvn_vk_bcdec_rec *rec) {
    if (!rec) return;
    if (rec->pool) vkDestroyDescriptorPool(d->device, rec->pool, NULL);   // 连带释放其中的描述符集
    if (rec->out_view) vkDestroyImageView(d->device, rec->out_view, NULL);
    if (rec->in_view) vkDestroyImageView(d->device, rec->in_view, NULL);
    if (rec->in) vkDestroyImage(d->device, rec->in, NULL);
    if (rec->in_mem) vkFreeMemory(d->device, rec->in_mem, NULL);
    memset(rec, 0, sizeof *rec);
}

// 描述符池每次一个：段完成前集不能回收，模块共用的 8 个集的池会被同一段里的多次拷贝用光。
int nvn_vk_bcdec_prepare(struct nvn_vk_bcdec *bc, struct s2a_vk_device *d, VkImage dst, uint32_t nvn_format,
                         uint32_t w, uint32_t h, uint32_t mip, uint32_t layer, struct nvn_vk_bcdec_rec *rec, char *why, size_t why_size) {
    memset(rec, 0, sizeof *rec);
    VkFormat view_fmt; const char *family; int bb;
    if (!bc || !nvn_vk_bcdec_route(nvn_format, &view_fmt, NULL, NULL, &family, &bb)) {
        snprintf(why, why_size, "bcdec: no route for nvn format 0x%02x", nvn_format); return -1;
    }
    if (!w || !h) { snprintf(why, why_size, "bcdec: %s prepare: empty extent", family); return -1; }
    int vi = variant_index(&ROUTES[nvn_format].v);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    VkFormat in_fmt = bb == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;

    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = in_fmt; ici.extent = (VkExtent3D){bw, bh, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(d->device, &ici, NULL, &rec->in) != VK_SUCCESS) goto fail;
    VkMemoryRequirements imr; vkGetImageMemoryRequirements(d->device, rec->in, &imr);
    VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex = s2a_vk_memory_type(d, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (imai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &imai, NULL, &rec->in_mem) != VK_SUCCESS ||
        vkBindImageMemory(d->device, rec->in, rec->in_mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = rec->in; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = in_fmt;
    vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d->device, &vci, NULL, &rec->in_view) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo ovci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ovci.image = dst; ovci.viewType = VK_IMAGE_VIEW_TYPE_2D; ovci.format = view_fmt;
    ovci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
    if (vkCreateImageView(d->device, &ovci, NULL, &rec->out_view) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->device, &dpi, NULL, &rec->pool) != VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = rec->pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &bc->dsl;
    if (vkAllocateDescriptorSets(d->device, &dai, &rec->ds) != VK_SUCCESS) goto fail;
    VkDescriptorImageInfo oi = { VK_NULL_HANDLE, rec->out_view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, rec->in_view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr[2] = {0};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = rec->ds; wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[0].pImageInfo = &oi;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = rec->ds; wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(d->device, 2, wr, 0, NULL);

    rec->pipe = bc->pipe[vi]; rec->pl = bc->pl; rec->bw = bw; rec->bh = bh; rec->in_format = in_fmt;
    return 0;
fail:
    snprintf(why, why_size, "bcdec: %s prepare resource setup failed", family);
    nvn_vk_bcdec_rec_free(d, rec);
    return -1;
}
