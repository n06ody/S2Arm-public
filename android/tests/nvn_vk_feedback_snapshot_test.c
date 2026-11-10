// 反馈采样快照的源子资源拷贝：真 Vulkan（lavapipe / Adreno）字节级回归。
//
// 生产语义（nvn_vk_mrt.h）：附件在 3D 纹理上按深度切片、在 2D 数组/cube 上按层选一个子资源，
// 快照固定是单层 2D；拷贝区域由 nvn_vk_feedback_copy_make 生成——
//   · 2D / 2D 数组：层号走 srcSubresource.baseArrayLayer；
//   · 3D：Vulkan 的 3D 子资源 layerCount 恒 1，深度切片必须走 srcOffset.z（不能塞 baseArrayLayer）；
//   · extent 取 pass 与快照的交集（快照按本笔 framebuffer 尺寸建）。
// 本测试只调生产实现，不复制一份地址算法：填图 → 生产 helper 出 VkImageCopy → 拷贝 → 读回比字节。
//
// 反例（修前必红）：
//   ① 3D 切片：depth=4、切 1，拷贝必须读到第 1 片；旧实现按 array layer 选（3D 只有 1 层）→ 越界或读错片。
//   ② 3D + mip≠0：level=1、切 1，必须读到 mip1 的第 1 片。
//   ③ 2D 数组层：layer=2，必须读到第 2 层。
//   ④ 交集：pass 小于快照，只拷左上交集，其余保持原色（不是整幅覆盖）。
//   ⑤ 同构快照：多级×多层 2D 数组，nvn_vk_feedback_iso_copy 逐子资源拷贝，逐子资源读回比对。
// 打桩：只建 device/queue/命令池；不建 session、不链帧队列（与内容用例同一口径）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn/nvn_vk_mrt.h"
#include "nvn_vk_format.h"
#include "nvn_obj.h"
#include "vk_device.h"

int __android_log_print(int prio, const char *tag, const char *fmt, ...) { (void)prio; (void)tag; (void)fmt; return 0; }
int __system_property_get(const char *name, char *value) { (void)name; if (value) value[0] = 0; return 0; }

static int fails;
#define CK(c, ...) do { if (!(c)) { fails++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

typedef struct { uint8_t r, g, b, a; } rgba;
#define RED   ((rgba){255, 0, 0, 255})
#define BLUE  ((rgba){0, 0, 255, 255})
#define GREEN ((rgba){0, 255, 0, 255})
#define BLACK ((rgba){0, 0, 0, 255})
#define MAGENTA ((rgba){255, 0, 255, 255})

static struct s2a_vk_device D;
static uint32_t W = 32, H = 32, DEPTH = 4, LAYERS = 3;

static int mem_of(uint32_t bits, VkMemoryPropertyFlags want) {
    return s2a_vk_memory_type(&D, bits, want, NULL);
}

static int make_image(VkImageType type, uint32_t w, uint32_t h, uint32_t d3, uint32_t levels,
                      uint32_t layers, VkImageUsageFlags usage, VkImage *out, VkDeviceMemory *mem_out) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = type;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = (VkExtent3D){ w, h, d3 };
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(D.device, &ci, NULL, out) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(D.device, *out, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mem_of(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(D.device, &ai, NULL, mem_out) != VK_SUCCESS) return -1;
    if (vkBindImageMemory(D.device, *out, *mem_out, 0) != VK_SUCCESS) return -1;
    return 0;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *out, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(D.device, &ci, NULL, out) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(D.device, *out, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mem_of(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(D.device, &ai, NULL, mem) != VK_SUCCESS) return -1;
    if (vkBindBufferMemory(D.device, *out, *mem, 0) != VK_SUCCESS) return -1;
    if (map) { if (vkMapMemory(D.device, *mem, 0, size, 0, map) != VK_SUCCESS) return -1; }
    return 0;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    uint32_t level0, uint32_t levels, uint32_t layers) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, level0, levels, 0, layers };
    b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, b.srcAccessMask ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static int run_cb(VkCommandBuffer cmd) {
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f = VK_NULL_HANDLE;
    if (vkCreateFence(D.device, &fi, NULL, &f) != VK_SUCCESS) return -1;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    int rc = -1;
    if (vkQueueSubmit(D.queue, 1, &si, f) == VK_SUCCESS && vkWaitForFences(D.device, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS) rc = 0;
    vkDestroyFence(D.device, f, NULL);
    return rc;
}

static VkCommandBuffer begin_cb(void) {
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = D.command_pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(D.device, &ca, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
    return cmd;
}

// 用 staging 把一片/一层的纯色填进图像（3D 用 imageOffset.z，2D 数组用 subresource layer）。
static int fill_slice(VkImage img, int is_3d, uint32_t level, uint32_t w, uint32_t h,
                      uint32_t depth_or_layer, rgba c) {
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void *map = NULL;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    if (make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &buf, &mem, &map)) return -1;
    uint8_t *p = map;
    for (uint32_t i = 0; i < w * h; i++) { p[i*4+0] = c.r; p[i*4+1] = c.g; p[i*4+2] = c.b; p[i*4+3] = c.a; }
    VkCommandBuffer cmd = begin_cb();
    if (!cmd) return -1;
    VkBufferImageCopy bc = {0};
    bc.bufferOffset = 0;
    bc.bufferRowLength = 0; bc.bufferImageHeight = 0;
    bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, is_3d ? 0u : depth_or_layer, 1 };
    bc.imageOffset = (VkOffset3D){ 0, 0, is_3d ? (int32_t)depth_or_layer : 0 };
    bc.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    int rc = vkEndCommandBuffer(cmd) == VK_SUCCESS ? run_cb(cmd) : -1;
    vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
    vkUnmapMemory(D.device, mem);
    vkDestroyBuffer(D.device, buf, NULL);
    vkFreeMemory(D.device, mem, NULL);
    return rc;
}

static void clear_img(VkImage img, rgba c) {
    VkCommandBuffer cmd = begin_cb();
    if (!cmd) return;
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    VkClearColorValue cv; memset(&cv, 0, sizeof cv);
    cv.float32[0] = (float)c.r / 255.0f; cv.float32[1] = (float)c.g / 255.0f;
    cv.float32[2] = (float)c.b / 255.0f; cv.float32[3] = (float)c.a / 255.0f;
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
    if (vkEndCommandBuffer(cmd) == VK_SUCCESS) (void)run_cb(cmd);
    vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
}

// 生产拷贝：只调 nvn_vk_feedback_copy_make，再录 vkCmdCopyImage。
static int copy_snapshot(VkImage src, VkImage snap, const struct nvn_vk_feedback_src *s,
                         uint32_t pass_w, uint32_t pass_h, char *why, size_t why_size) {
    VkImageCopy rc;
    if (nvn_vk_feedback_copy_make(s, pass_w, pass_h, W, H, &rc, why, why_size)) return -1;
    VkCommandBuffer cmd = begin_cb();
    if (!cmd) return -1;
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, snap, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
    int ok = vkEndCommandBuffer(cmd) == VK_SUCCESS ? run_cb(cmd) : -1;
    vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
    return ok;
}

// 读 (level, layer) 子资源整幅（w×h 是该级实际尺寸）。
static int read_pixels_sub(VkImage snap, uint32_t level, uint32_t layer,
                           uint32_t w, uint32_t h, uint8_t *out) {
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void *map = NULL;
    if (make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map)) return -1;
    VkCommandBuffer cmd = begin_cb();
    if (!cmd) return -1;
    VkBufferImageCopy bc = {0};
    bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
    bc.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyImageToBuffer(cmd, snap, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bc);
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf; bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
    int rc = vkEndCommandBuffer(cmd) == VK_SUCCESS ? run_cb(cmd) : -1;
    if (rc == 0) memcpy(out, map, bytes);
    vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
    vkUnmapMemory(D.device, mem);
    vkDestroyBuffer(D.device, buf, NULL);
    vkFreeMemory(D.device, mem, NULL);
    return rc;
}

static int read_pixels(VkImage snap, uint8_t *out) {
    return read_pixels_sub(snap, 0, 0, W, H, out);
}

static int expect_px(const uint8_t *px, uint32_t w, uint32_t x, uint32_t y, rgba want, const char *what) {
    const uint8_t *p = px + ((size_t)y * w + x) * 4;
    if (p[0] != want.r || p[1] != want.g || p[2] != want.b || p[3] != want.a) {
        printf("  %s: (%u,%u) got=%u,%u,%u,%u want=%u,%u,%u,%u\n", what, x, y,
               p[0], p[1], p[2], p[3], want.r, want.g, want.b, want.a);
        return -1;
    }
    return 0;
}

int main(void) {
    char why[256] = "";
    if (s2a_vk_device_create(&D, why, sizeof why)) { printf("✗ device: %s\n", why); return 1; }
    uint8_t *px = malloc((size_t)W * H * 4);
    if (!px) { printf("✗ alloc\n"); return 1; }

    // ===== ① 3D 纹理：depth=4，拷贝深度切片 1（旧实现按 array layer 选 → 越界/读错片）=====
    {
        VkImage src = VK_NULL_HANDLE, snap = VK_NULL_HANDLE;
        VkDeviceMemory sm = VK_NULL_HANDLE, mm = VK_NULL_HANDLE;
        CK(make_image(VK_IMAGE_TYPE_3D, W, H, DEPTH, 1, 1,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &src, &sm) == 0, "① 建 3D 图像失败");
        CK(make_image(VK_IMAGE_TYPE_2D, W, H, 1, 1, 1,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &snap, &mm) == 0, "① 建快照失败");
        if (src && snap) {
            VkCommandBuffer cmd = begin_cb();
            barrier(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
            barrier(cmd, snap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
            CK(vkEndCommandBuffer(cmd) == VK_SUCCESS && run_cb(cmd) == 0, "① 初始屏障失败");
            vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
            clear_img(snap, BLACK);
            CK(fill_slice(src, 1, 0, W, H, 0, RED) == 0, "① 填切片 0 失败");
            CK(fill_slice(src, 1, 0, W, H, 1, BLUE) == 0, "① 填切片 1 失败");
            CK(fill_slice(src, 1, 0, W, H, 2, GREEN) == 0, "① 填切片 2 失败");
            struct nvn_vk_feedback_src s; memset(&s, 0, sizeof s);
            s.is_3d = 1; s.slice = 1; s.layers = 1;
            char cwhy[192] = "";
            CK(copy_snapshot(src, snap, &s, W, H, cwhy, sizeof cwhy) == 0, "① 生产拷贝失败：%s", cwhy);
            CK(read_pixels(snap, px) == 0, "① 读回失败");
            CK(expect_px(px, W, 0, 0, BLUE, "①切片1左上") == 0, "① 必须读到深度切片 1（蓝）");
            CK(expect_px(px, W, W-1, H-1, BLUE, "①切片1右下") == 0, "① 必须读到深度切片 1（右下也是蓝）");
        }
        if (snap) vkDestroyImage(D.device, snap, NULL);
        if (src) vkDestroyImage(D.device, src, NULL);
        if (mm) vkFreeMemory(D.device, mm, NULL);
        if (sm) vkFreeMemory(D.device, sm, NULL);
    }

    // ===== ② 3D + mip≠0：level=1、切片 1 =====
    {
        VkImage src = VK_NULL_HANDLE, snap = VK_NULL_HANDLE;
        VkDeviceMemory sm = VK_NULL_HANDLE, mm = VK_NULL_HANDLE;
        const uint32_t mw = W / 2, mh = H / 2;
        CK(make_image(VK_IMAGE_TYPE_3D, W, H, DEPTH, 2, 1,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &src, &sm) == 0, "② 建多级 3D 图像失败");
        CK(make_image(VK_IMAGE_TYPE_2D, W, H, 1, 1, 1,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &snap, &mm) == 0, "② 建快照失败");
        if (src && snap) {
            VkCommandBuffer cmd = begin_cb();
            barrier(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_REMAINING_MIP_LEVELS, 1);
            barrier(cmd, snap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
            CK(vkEndCommandBuffer(cmd) == VK_SUCCESS && run_cb(cmd) == 0, "② 初始屏障失败");
            vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
            clear_img(snap, BLACK);
            CK(fill_slice(src, 1, 1, mw, mh, 1, MAGENTA) == 0, "② 填 mip1 切片 1 失败");
            struct nvn_vk_feedback_src s; memset(&s, 0, sizeof s);
            s.is_3d = 1; s.slice = 1; s.level = 1; s.layers = 1;
            char cwhy[192] = "";
            CK(copy_snapshot(src, snap, &s, mw, mh, cwhy, sizeof cwhy) == 0, "② 生产拷贝失败：%s", cwhy);
            CK(read_pixels(snap, px) == 0, "② 读回失败");
            CK(expect_px(px, W, 0, 0, MAGENTA, "②mip1切片1") == 0, "② 必须读到 mip1 的深度切片 1");
            CK(expect_px(px, W, mw, mh, BLACK, "②交集外") == 0, "② 交集外必须保持快照原色");
        }
        if (snap) vkDestroyImage(D.device, snap, NULL);
        if (src) vkDestroyImage(D.device, src, NULL);
        if (mm) vkFreeMemory(D.device, mm, NULL);
        if (sm) vkFreeMemory(D.device, sm, NULL);
    }

    // ===== ③ 2D 数组：3 层，拷贝第 2 层 =====
    {
        VkImage src = VK_NULL_HANDLE, snap = VK_NULL_HANDLE;
        VkDeviceMemory sm = VK_NULL_HANDLE, mm = VK_NULL_HANDLE;
        CK(make_image(VK_IMAGE_TYPE_2D, W, H, 1, 1, LAYERS,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &src, &sm) == 0, "③ 建 2D 数组图像失败");
        CK(make_image(VK_IMAGE_TYPE_2D, W, H, 1, 1, 1,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &snap, &mm) == 0, "③ 建快照失败");
        if (src && snap) {
            VkCommandBuffer cmd = begin_cb();
            barrier(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, LAYERS);
            barrier(cmd, snap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
            CK(vkEndCommandBuffer(cmd) == VK_SUCCESS && run_cb(cmd) == 0, "③ 初始屏障失败");
            vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
            clear_img(snap, BLACK);
            CK(fill_slice(src, 0, 0, W, H, 0, RED) == 0, "③ 填层 0 失败");
            CK(fill_slice(src, 0, 0, W, H, 2, GREEN) == 0, "③ 填层 2 失败");
            struct nvn_vk_feedback_src s; memset(&s, 0, sizeof s);
            s.layer = 2; s.layers = 1;
            char cwhy[192] = "";
            CK(copy_snapshot(src, snap, &s, W, H, cwhy, sizeof cwhy) == 0, "③ 生产拷贝失败：%s", cwhy);
            CK(read_pixels(snap, px) == 0, "③ 读回失败");
            CK(expect_px(px, W, 0, 0, GREEN, "③层2") == 0, "③ 必须读到 2D 数组第 2 层（绿）");
        }
        if (snap) vkDestroyImage(D.device, snap, NULL);
        if (src) vkDestroyImage(D.device, src, NULL);
        if (mm) vkFreeMemory(D.device, mm, NULL);
        if (sm) vkFreeMemory(D.device, sm, NULL);
    }

    // ===== ④ 交集：pass 比快照小，只拷左上交集，其余保持原色 =====
    {
        VkImage src = VK_NULL_HANDLE, snap = VK_NULL_HANDLE;
        VkDeviceMemory sm = VK_NULL_HANDLE, mm = VK_NULL_HANDLE;
        CK(make_image(VK_IMAGE_TYPE_3D, W, H, DEPTH, 1, 1,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &src, &sm) == 0, "④ 建 3D 图像失败");
        CK(make_image(VK_IMAGE_TYPE_2D, W, H, 1, 1, 1,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &snap, &mm) == 0, "④ 建快照失败");
        if (src && snap) {
            VkCommandBuffer cmd = begin_cb();
            barrier(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
            barrier(cmd, snap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
            CK(vkEndCommandBuffer(cmd) == VK_SUCCESS && run_cb(cmd) == 0, "④ 初始屏障失败");
            vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
            clear_img(snap, MAGENTA);
            CK(fill_slice(src, 1, 0, W, H, 0, RED) == 0, "④ 填切片 0 失败");
            struct nvn_vk_feedback_src s; memset(&s, 0, sizeof s);
            s.is_3d = 1; s.slice = 0; s.layers = 1;
            char cwhy[192] = "";
            CK(copy_snapshot(src, snap, &s, W / 2, H / 2, cwhy, sizeof cwhy) == 0, "④ 生产拷贝失败：%s", cwhy);
            CK(read_pixels(snap, px) == 0, "④ 读回失败");
            CK(expect_px(px, W, W / 2 - 1, H / 2 - 1, RED, "④交集内") == 0, "④ 交集内必须是切片颜色");
            CK(expect_px(px, W, W / 2, H / 2, MAGENTA, "④交集外") == 0, "④ 交集外必须保持原色（不是整幅覆盖）");
        }
        if (snap) vkDestroyImage(D.device, snap, NULL);
        if (src) vkDestroyImage(D.device, src, NULL);
        if (mm) vkFreeMemory(D.device, mm, NULL);
        if (sm) vkFreeMemory(D.device, sm, NULL);
    }

    // ===== ⑤ 同构快照：32x32 三级 × 三层 2D 数组，逐子资源异色 → 逐子资源拷贝 → 逐子资源读回 =====
    // 多层/多级默认视图自采样的快照形态（8E 上 serial=5386 型缺陷的覆盖用例）：
    // 修前 bind_plan LEAVE 活绑定；修后 ISO 走整图同构拷贝——本用例按字节证明拷贝区域全对。
    {
        const uint32_t il = 3, ic = 3;   // levels × layers
        VkImage src = VK_NULL_HANDLE, snap = VK_NULL_HANDLE;
        VkDeviceMemory sm = VK_NULL_HANDLE, mm = VK_NULL_HANDLE;
        CK(make_image(VK_IMAGE_TYPE_2D, W, H, 1, il, ic,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &src, &sm) == 0, "⑤ 建多级多层源图失败");
        CK(make_image(VK_IMAGE_TYPE_2D, W, H, 1, il, ic,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &snap, &mm) == 0, "⑤ 建同构快照失败");
        if (src && snap) {
            VkCommandBuffer cmd = begin_cb();
            barrier(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    0, VK_REMAINING_MIP_LEVELS, ic);
            barrier(cmd, snap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    0, VK_REMAINING_MIP_LEVELS, ic);
            CK(vkEndCommandBuffer(cmd) == VK_SUCCESS && run_cb(cmd) == 0, "⑤ 初始屏障失败");
            vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
            clear_img(snap, BLACK);
            // 每个 (level, layer) 一个可区分颜色。
            const rgba cs[3] = { RED, GREEN, BLUE };
            for (uint32_t l = 0; l < il; l++) {
                const uint32_t mw = W >> l, mh = H >> l;
                for (uint32_t a = 0; a < ic; a++) {
                    rgba c = cs[(l + a) % 3];
                    c.a = (uint8_t)(16 + l * 32 + a);   // alpha 里编码 (level,layer)，查错一目了然
                    CK(fill_slice(src, 0, l, mw, mh, a, c) == 0, "⑤ 填 l%u a%u 失败", l, a);
                }
            }
            // 生产 iso 计划 + 逐份拷贝（与 first_draw 的 precopy 循环同口径）。
            struct nvn_vk_feedback_iso iso; memset(&iso, 0, sizeof iso);
            char cwhy[192] = "";
            CK(nvn_vk_feedback_iso_plan(0, W, H, 1, il, ic, 4, 64ull << 20,
                                        &iso, cwhy, sizeof cwhy) == 0, "⑤ iso 计划失败：%s", cwhy);
            CK(iso.n_copies == il * ic, "⑤ iso 份数应 = 级数×层数");
            cmd = begin_cb();
            for (uint32_t n = 0; cmd && n < iso.n_copies; n++) {
                VkImageCopy rc = {0};
                CK(nvn_vk_feedback_iso_copy(&iso, n, &rc, cwhy, sizeof cwhy) == 0,
                   "⑤ iso 拷贝 %u 出区失败：%s", n, cwhy);
                vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, snap, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
            }
            if (cmd) {
                CK(vkEndCommandBuffer(cmd) == VK_SUCCESS && run_cb(cmd) == 0, "⑤ 同构拷贝执行失败");
                vkFreeCommandBuffers(D.device, D.command_pool, 1, &cmd);
            }
            // 逐子资源读回验证：快照的每个 (level,layer) 必须与源同色。
            uint8_t *sub = malloc((size_t)W * H * 4);
            CK(sub != NULL, "⑤ 读回缓冲分配失败");
            if (sub) {
                for (uint32_t l = 0; l < il; l++) {
                    const uint32_t mw = W >> l, mh = H >> l;
                    for (uint32_t a = 0; a < ic; a++) {
                        rgba want = cs[(l + a) % 3];
                        want.a = (uint8_t)(16 + l * 32 + a);
                        memset(sub, 0, (size_t)W * H * 4);
                        CK(read_pixels_sub(snap, l, a, mw, mh, sub) == 0,
                           "⑤ 读回 l%u a%u 失败", l, a);
                        char what[64];
                        snprintf(what, sizeof what, "⑤l%ua%u", l, a);
                        CK(expect_px(sub, mw, 0, 0, want, what) == 0,
                           "⑤ 快照 l%u a%u 内容必须是源子资源颜色", l, a);
                        CK(expect_px(sub, mw, mw - 1, mh - 1, want, what) == 0,
                           "⑤ 快照 l%u a%u 右下也必须是源子资源颜色", l, a);
                    }
                }
                free(sub);
            }
        }
        if (snap) vkDestroyImage(D.device, snap, NULL);
        if (src) vkDestroyImage(D.device, src, NULL);
        if (mm) vkFreeMemory(D.device, mm, NULL);
        if (sm) vkFreeMemory(D.device, sm, NULL);
    }

    free(px);
    s2a_vk_device_destroy(&D, 1);
    if (fails) { printf("✗ nvn_vk_feedback_snapshot: %d 处不符\n", fails); return 1; }
    printf("✓ nvn_vk_feedback_snapshot: 3D 切片 / 3D+mip / 2D 数组层 / 交集 / 同构快照 全部通过（真 Vulkan）\n");
    return 0;
}
