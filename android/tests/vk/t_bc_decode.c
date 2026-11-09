// BC1–BC7 compute 解码主机用例：每个 NVN BC 格式造一组原创小块（种子伪随机 + 关键端点样式），
// 走 nvn_vk_bcdec 的 GPU 解码 → 读回 → 与 graphics/nvn/gfx/tex/nvn_bcdec.c（bcdec，CPU 参考）逐像素比对。
// BC6H 按半精度位型比较：NaN 只认 NaN（载荷不比）、±Inf 精确比、其余位级相等。
// 另含反证：故意改一块数据，比对必须报不符（否则比对是哑的）。在 lavapipe + validation 上跑（run.sh）。
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_device.h"
#include "nvn_vk_bcdec.h"
#include "gfx/tex/nvn_bcdec.h"

static struct s2a_vk_device g_dev;
static struct nvn_vk_bcdec *g_bc;
static int g_bad;

static void note(int ok, const char *fmt, const char *a, const char *b, int x, int y, int c) {
    if (ok) return;
    g_bad++;
    if (g_bad <= 20) printf("  不符 %s：x=%d y=%d c=%d got=%s want=%s\n", fmt, x, y, c, a, b);
}

static uint64_t g_rng;
static uint8_t rnd8(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint8_t)g_rng; }

static int block_bytes(uint32_t f) { return (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4a) ? 8 : 16; }
static int out_bpp(uint32_t f) { return (f == 0x4f || f == 0x50) ? 8 : (f == 0x4b || f == 0x4c) ? 2 : (f == 0x49 || f == 0x4a) ? 1 : 4; }

// 造一组原创块：种子伪随机；对 BC1 再固定一块透明样式（c0<c1 + index3），对 BC4/BC5 固定一块 e0<=e1 样式。
static void gen_blocks(uint32_t f, uint32_t w, uint32_t h, uint8_t *out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, n = bw * bh, bb = block_bytes(f);
    g_rng = 0x9e3779b97f4a7c15ull ^ ((uint64_t)f << 40) ^ w ^ ((uint64_t)h << 20);
    for (uint32_t i = 0; i < n * bb; i++) out[i] = rnd8();
    if ((f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46) && n >= 2) {
        memset(out, 0, 8);                      // c0=0 < c1=0xffff：透明模式
        out[2] = 0xff; out[3] = 0xff;           // c1 端点全 1
        out[4] = out[5] = out[6] = out[7] = 0xff; // 全 index3 → alpha=0
    }
    if ((f >= 0x49 && f <= 0x4c) && n >= 2) {
        out[0] = 0x20; out[1] = 0x60;                 // e0 <= e1 走特殊区间（含 -1/+1 端）
    }
}

static int gpu_decode_to_buf(uint32_t f, uint32_t w, uint32_t h, const uint8_t *blocks, uint8_t *dst) {
    VkFormat view_fmt, dfmt, img_fmt;
    int bb;
    nvn_vk_bcdec_route(f, &view_fmt, &dfmt, &img_fmt, NULL, &bb);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bpp = out_bpp(f);
    struct s2a_vk_device *d = &g_dev;

    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = img_fmt; ici.extent = (VkExtent3D){w, h, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage img;
    if (vkCreateImage(d->device, &ici, NULL, &img) != VK_SUCCESS) { printf("  建 dst 图失败\n"); return -1; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(d->device, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    VkDeviceMemory imem;
    if (vkAllocateMemory(d->device, &mai, NULL, &imem) != VK_SUCCESS) { printf("  dst 显存失败\n"); return -1; }
    vkBindImageMemory(d->device, img, imem, 0);

    char why[160] = {0};
    int rc = nvn_vk_bcdec_decode(g_bc, d, img, f, w, h, 0, 0, blocks, (size_t)bw * bh * bb, why, sizeof why);
    if (rc != 0) { printf("  解码失败 rc=%d why=%s\n", rc, why); return -1; }

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (size_t)w * h * bpp; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (vkCreateBuffer(d->device, &bci, NULL, &buf) != VK_SUCCESS) return -1;
    vkGetBufferMemoryRequirements(d->device, buf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkDeviceMemory bmem;
    if (vkAllocateMemory(d->device, &mai, NULL, &bmem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(d->device, buf, bmem, 0);

    VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = d->command_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(d->device, &cbi, &cmd) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp = {0};
    cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; vkCreateFence(d->device, &fci, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(d->queue, 1, &si, fence);
    vkWaitForFences(d->device, 1, &fence, VK_TRUE, 5000000000ull);
    void *p; vkMapMemory(d->device, bmem, 0, bci.size, 0, &p);
    memcpy(dst, p, (size_t)w * h * bpp);
    vkUnmapMemory(d->device, bmem);
    vkDestroyFence(d->device, fence, NULL);
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
    vkDestroyBuffer(d->device, buf, NULL); vkFreeMemory(d->device, bmem, NULL);
    vkDestroyImage(d->device, img, NULL); vkFreeMemory(d->device, imem, NULL);
    return 0;
}

// BC6H 半精度位型：0 数值位相等；1 视为同型（NaN 载荷不比）。
static int h16_cmp(uint16_t a, uint16_t b) {
    int an = (a & 0x7c00) == 0x7c00 && (a & 0x03ff), bn = (b & 0x7c00) == 0x7c00 && (b & 0x03ff);
    if (an || bn) return an && bn ? 1 : 0;
    return a == b;
}

static int run_case(uint32_t f, uint32_t w, uint32_t h, int corrupt) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = block_bytes(f), bpp = out_bpp(f);
    size_t src_sz = (size_t)bw * bh * bb, dst_sz = (size_t)w * h * bpp;
    uint8_t *blocks = malloc(src_sz), *want = malloc(dst_sz), *got = malloc(dst_sz);
    gen_blocks(f, w, h, blocks);
    nvn_bc_decode(f, blocks, (size_t)bw * bb, w, h, want, (size_t)w * bpp);
    // 反证：CPU 吃干净块、GPU 吃坏块——比对必须报出差异，否则比对是哑的。
    if (corrupt) blocks[0] ^= 0x40;
    if (gpu_decode_to_buf(f, w, h, blocks, got) != 0) { g_bad++; free(blocks); free(want); free(got); return -1; }
    int diff = 0;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint8_t *g = got + ((size_t)y * w + x) * bpp, *e = want + ((size_t)y * w + x) * bpp;
        if (bpp == 8) {   // BC6H：四通道半精度位型
            for (int c = 0; c < 4; c++) {
                uint16_t gv, ev; memcpy(&gv, g + c * 2, 2); memcpy(&ev, e + c * 2, 2);
                if (!h16_cmp(gv, ev)) { char a[16], bb2[16]; snprintf(a, 16, "0x%04x", gv); snprintf(bb2, 16, "0x%04x", ev); if (!corrupt) note(0, "bc6", a, bb2, x, y, c); diff++; }
            }
        } else for (uint32_t c = 0; c < bpp; c++)
            if (g[c] != e[c]) { char a[8], bb2[8]; snprintf(a, 8, "%d", g[c]); snprintf(bb2, 8, "%d", e[c]); if (!corrupt) note(0, "u8", a, bb2, x, y, c); diff++; }
    }
    if (corrupt) {
        if (diff == 0) { g_bad++; printf("  反证失败：改块后比对仍全中\n"); }
        else printf("  反证 OK：坏块 diff=%d（预期 >0）\n", diff);
    }
    free(blocks); free(want); free(got);
    return diff;
}

int main(void) {
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why) != 0) { printf("建 Vulkan 设备失败：%s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    if (nvn_vk_bcdec_create(&g_dev, &g_bc, why, sizeof why) != 0) { printf("建 bcdec 失败：%s\n", why); return 2; }
    static const struct { uint32_t f; const char *n; } FMTS[16] = {
        {0x41,"BC1_RGB"},{0x42,"BC1_RGBA"},{0x43,"BC2"},{0x44,"BC3"},
        {0x45,"BC1_RGB_SRGB"},{0x46,"BC1_RGBA_SRGB"},{0x47,"BC2_SRGB"},{0x48,"BC3_SRGB"},
        {0x49,"BC4U"},{0x4a,"BC4S"},{0x4b,"BC5U"},{0x4c,"BC5S"},
        {0x4d,"BC7"},{0x4e,"BC7_SRGB"},{0x4f,"BC6H_S"},{0x50,"BC6H_U"},
    };
    for (int i = 0; i < 16; i++) {
        int d1 = run_case(FMTS[i].f, 8, 8, 0);
        int d2 = run_case(FMTS[i].f, 10, 6, 0);   // 非 4 倍数尾块
        printf("%s: 8x8 diff=%d, 10x6 diff=%d %s\n", FMTS[i].n, d1, d2, (d1 == 0 && d2 == 0) ? "OK" : "MISMATCH");
    }
    run_case(0x44, 8, 8, 1);   // 反证：坏块必须被比对出来
    // 反证：无路由格式必须显式失败（非零 + why），不得静默成功。
    {
        char w2[128] = {0};
        uint8_t dummy[16] = {0};
        int rc = nvn_vk_bcdec_decode(g_bc, &g_dev, VK_NULL_HANDLE, 0x40, 4, 4, 0, 0, dummy, 8, w2, sizeof w2);
        if (rc == 0 || w2[0] == 0) { g_bad++; printf("  反证失败：无路由格式 rc=%d why='%s'（预期非零+原因）\n", rc, w2); }
        else printf("  反证 OK：无路由格式显式失败（why=%s）\n", w2);
    }
    nvn_vk_bcdec_destroy(&g_dev, g_bc);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("bc-decode 逐像素：%s（%d 处不符/异常）\n", g_bad ? "红" : "绿", g_bad);
    return g_bad ? 1 : 0;
}
