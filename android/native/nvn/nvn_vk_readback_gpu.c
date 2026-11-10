// NVN ReadTexels 的 GPU 图像路径：图像 → host-visible staging（VkBufferImageCopy）→ 按 plan 打包进 dst。
//
// 前置条件由调用方（nvn_vk_session_read_texels）保证：内容归 GPU，且已经通过帧队列
// pass_break(1) 拿到“先前 GPU 工作全部完成”的证明。本文件只负责：
//   · 区域/子资源原样落成 VkBufferImageCopy（imageOffset / imageExtent / imageSubresource）；
//   · 一次提交 + fence 等完成（CPU 可见读回不能读到半成品）；
//   · staging 缓冲的有界复用（读回是罕见路径，但截图/流式路径可能连续读，不每笔重建）。
// 图像存储格式与 NVN 格式不同（BC compute 解码、上传展开）时明确失败：无法从解码后的像素
// 还原 NVN 的压缩块字节布局，不能把 RGBA8 当 NVN 原始格式交出去。
#include "nvn_vk_readback.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <android/log.h>

#include "nvn_vk_format.h"
#include "nvn_vk_texcache.h"
#include "vk_device.h"

// 有界复用的读回 staging：一次读回一份，够用就复用，不够才重建。只在 adapter 执行包内调用
// （g_mu 串行），因此不需要第二把锁。提交后没等到 fence 的缓冲 poisoned：不再复用也不释放。
static struct {
    VkDevice dev;
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    size_t cap;
    int coherent;
    int poisoned;
} g_rb;

// 逐帧 TSV 的 rb_us 源：本路径 5 s 有界等待的实际耗时累计（不清零；只在 frame_tsv 开关
//   打开时计时，关态不付取钟成本）。读回是罕见路径，差分由读方自己算。
static _Atomic uint64_t g_rb_wait_ns_total;
uint64_t nvn_vk_readback_wait_ns_total(void) {
    return atomic_load_explicit(&g_rb_wait_ns_total, memory_order_relaxed);
}

static int gpu_fail(char *why, size_t why_size, const char *fmt, ...) {
    if (why && why_size) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, why_size, fmt, ap);
        va_end(ap);
    }
    return -1;
}

void nvn_vk_readback_gpu_release(struct s2a_vk_device *d) {
    (void)d;   // 缓冲属于创建它的设备；会话销毁前已拿到该设备的空闲证明。
    if (!g_rb.buf) return;
    if (g_rb.poisoned) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
            "readback staging retained: a copy submission never proved completion; not reusing/freeing it");
        return;
    }
    if (g_rb.map) vkUnmapMemory(g_rb.dev, g_rb.mem);
    vkDestroyBuffer(g_rb.dev, g_rb.buf, NULL);
    vkFreeMemory(g_rb.dev, g_rb.mem, NULL);
    memset(&g_rb, 0, sizeof g_rb);
}

static int gpu_staging(struct s2a_vk_device *d, size_t need, char *why, size_t why_size) {
    if (g_rb.buf && g_rb.dev == d->device && g_rb.cap >= need) return 0;
    if (g_rb.buf) {
        // 上一次读回已经等到 fence（poisoned 的不会走到这里）：可以安全重建/扩容。
        nvn_vk_readback_gpu_release(d);
        if (g_rb.buf) return gpu_fail(why, why_size, "readback staging buffer could not be retired");
    }
    size_t size = need < (64u << 10) ? (64u << 10) : need;
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult vr = vkCreateBuffer(d->device, &bi, NULL, &buf);
    if (vr != VK_SUCCESS) return gpu_fail(why, why_size, "vkCreateBuffer(%zu B) failed: %d", size, (int)vr);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, buf, &req);
    VkMemoryPropertyFlags actual = 0;
    uint32_t idx = s2a_vk_memory_type(d, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      &actual);
    if (idx == UINT32_MAX)
        idx = s2a_vk_memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &actual);
    if (idx == UINT32_MAX) {
        vkDestroyBuffer(d->device, buf, NULL);
        return gpu_fail(why, why_size, "no HOST_VISIBLE memory type for the %zu B readback staging buffer", size);
    }
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = idx;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(d->device, &ai, NULL, &mem) != VK_SUCCESS || vkBindBufferMemory(d->device, buf, mem, 0) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buf, NULL);
        if (mem) vkFreeMemory(d->device, mem, NULL);
        return gpu_fail(why, why_size, "readback staging vkAllocateMemory/vkBindBufferMemory failed");
    }
    void *map = NULL;
    if (vkMapMemory(d->device, mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS || !map) {
        vkDestroyBuffer(d->device, buf, NULL);
        vkFreeMemory(d->device, mem, NULL);
        return gpu_fail(why, why_size, "vkMapMemory for the readback staging buffer failed");
    }
    g_rb.dev = d->device; g_rb.buf = buf; g_rb.mem = mem; g_rb.map = map;
    g_rb.cap = size; g_rb.coherent = (actual & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0; g_rb.poisoned = 0;
    if (!g_rb.coherent)
        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
            "readback staging: no HOST_COHERENT memory type; invalidating the mapped range after each fence");
    return 0;
}

int nvn_vk_readback_gpu(struct s2a_vk_device *d, const struct nvn_vk_tex *tex,
                        const struct nvn_vk_rb_plan *p, uint8_t *dst,
                        char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!d || !d->device || !d->command_pool) return gpu_fail(why, why_size, "no Vulkan device");
    if (!tex || !tex->image) return gpu_fail(why, why_size, "texture has no GPU image");
    if (!p || !p->nz || !dst) return gpu_fail(why, why_size, "invalid readback plan/destination");

    // 图像必须按 NVN 格式原样存储：否则 GPU 内存里的字节不是 NVN 语义要求的那份。
    const VkFormat nvn_fmt = nvn_vk_format(tex->nvn_format);
    if (nvn_fmt == VK_FORMAT_UNDEFINED)
        return gpu_fail(why, why_size, "NVN format 0x%x has no Vulkan mapping; cannot verify the readback layout",
                        tex->nvn_format);
    if (tex->vk_format != nvn_fmt)
        return gpu_fail(why, why_size,
                        "GPU-written texture serial=%llu was stored in VkFormat %d, but NVN format 0x%x needs %d "
                        "(BC compute decode / expanded storage); the decoded pixels are not the NVN byte layout "
                        "and cannot be re-encoded losslessly", (unsigned long long)tex->serial,
                        (int)tex->vk_format, tex->nvn_format, (int)nvn_fmt);

    VkImageAspectFlags aspect;
    if (nvn_vk_format_is_depth(tex->vk_format))
        aspect = tex->stencil_mode ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    else
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // 子资源范围：这些层/切片同一 level。3D 的 image_layer 恒 0（plan 已保证）。
    // 3D 图像只有一个 array layer（切片走 imageOffset.z）；2D 数组/立方体的层号是 layer0+z。
    uint32_t layer_lo = p->is3d ? 0u : p->layer0 + p->z;
    uint32_t layer_n = p->is3d ? 1u : p->nz;

    if (gpu_staging(d, p->staging_bytes, why, why_size)) return -1;
    void *map = g_rb.map;

    VkCommandBufferAllocateInfo ca = {0};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = d->command_pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult vr = vkAllocateCommandBuffers(d->device, &ca, &cmd);
    if (vr != VK_SUCCESS) return gpu_fail(why, why_size, "vkAllocateCommandBuffers failed: %d", (int)vr);

    VkCommandBufferBeginInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = vkBeginCommandBuffer(cmd, &cb);
    if (vr != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        return gpu_fail(why, why_size, "vkBeginCommandBuffer failed: %d", (int)vr);
    }
    // 先前 GPU 工作已完成（调用方拿过证明），这里仍按 GENERAL→GENERAL 声明依赖：
    // 拷贝读图像必须与之前的颜色/深度/传输写有序，validation 与真驱动都按它判。
    VkImageMemoryBarrier ib = {0};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = tex->image;
    ib.subresourceRange = (VkImageSubresourceRange){aspect, p->level, 1, layer_lo, layer_n};
    ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &ib);
    for (uint32_t z = 0; z < p->nz; z++) {
        struct nvn_vk_rb_slice sl;
        nvn_vk_readback_slice(p, z, &sl);
        VkBufferImageCopy region = {0};
        region.bufferOffset = (VkDeviceSize)sl.staging_offset;
        region.bufferRowLength = 0;      // 紧排：一行 = ceil(width/blockW) 个块
        region.bufferImageHeight = 0;    // 紧排：块行直接相邻
        region.imageSubresource.aspectMask = aspect;
        region.imageSubresource.mipLevel = p->level;
        region.imageSubresource.baseArrayLayer = sl.image_layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = (VkOffset3D){(int32_t)p->x, (int32_t)p->y, (int32_t)sl.image_z};
        region.imageExtent = (VkExtent3D){p->w, p->h, 1};
        vkCmdCopyImageToBuffer(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, g_rb.buf, 1, &region);
    }
    VkBufferMemoryBarrier bb = {0};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = g_rb.buf;
    bb.offset = 0;
    bb.size = (VkDeviceSize)p->staging_bytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &bb, 0, NULL);
    vr = vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        return gpu_fail(why, why_size, "vkEndCommandBuffer failed: %d", (int)vr);
    }

    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vr = vkCreateFence(d->device, &fi, NULL, &fence);
    if (vr != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        return gpu_fail(why, why_size, "vkCreateFence failed: %d", (int)vr);
    }
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vr = vkQueueSubmit(d->queue, 1, &si, fence);
    if (vr != VK_SUCCESS) {
        // 提交从未成功 = 命令没执行；命令缓冲/缓冲可以安全回收。
        vkDestroyFence(d->device, fence, NULL);
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
        return gpu_fail(why, why_size, "vkQueueSubmit for the readback copy failed: %d", (int)vr);
    }
    // 5 s 有界等待：读回是 CPU 可见操作，没有完成证明就不能把字节交出去。
    // 逐帧 TSV：frame_tsv 开关开时记本次等待实测耗时（先判 on()，关态不取钟）。
    extern int nvn_vk_frame_tsv_on(void) __attribute__((weak));
    const int ft = nvn_vk_frame_tsv_on && nvn_vk_frame_tsv_on();
    struct timespec wt;
    uint64_t wt0 = 0;
    if (ft) { clock_gettime(CLOCK_MONOTONIC, &wt); wt0 = (uint64_t)wt.tv_sec * 1000000000ull + (uint64_t)wt.tv_nsec; }
    vr = vkWaitForFences(d->device, 1, &fence, VK_TRUE, 5000000000ull);
    if (ft) {
        clock_gettime(CLOCK_MONOTONIC, &wt);
        atomic_fetch_add_explicit(&g_rb_wait_ns_total,
            (uint64_t)wt.tv_sec * 1000000000ull + (uint64_t)wt.tv_nsec - wt0,
            memory_order_relaxed);
    }
    if (vr != VK_SUCCESS) {
        // 已经提交且可能仍在读 staging/图像：缓冲进 poisoned，不复用不释放；fence 与命令缓冲
        // 同理留给进程退出回收（销毁仍被提交引用的 fence/命令缓冲是非法用法）。
        g_rb.poisoned = 1;
        return gpu_fail(why, why_size,
                        "readback copy submitted but did not complete within 5 s (vkWaitForFences=%d); "
                        "staging buffer, fence and command buffer retained, returning no bytes", (int)vr);
    }
    vkDestroyFence(d->device, fence, NULL);
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
    if (!g_rb.coherent) {
        VkMappedMemoryRange mr = {0};
        mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mr.memory = g_rb.mem;
        mr.offset = 0;
        mr.size = VK_WHOLE_SIZE;
        vr = vkInvalidateMappedMemoryRanges(d->device, 1, &mr);
        if (vr != VK_SUCCESS)
            return gpu_fail(why, why_size, "vkInvalidateMappedMemoryRanges failed: %d; refusing to hand out "
                                          "possibly stale readback bytes", (int)vr);
    }
    // #398：D24 换成 D32 存储时深度面拷出来是 float，换回原 D24 深度面口径（低 24 位 UNORM）。
    const int depth_f32_to_24 = aspect == VK_IMAGE_ASPECT_DEPTH_BIT && nvn_vk_format_substituted(tex->nvn_format);
    // 打包：staging 每层/切片按 staging_stride 对齐，行间紧排；dst 按 NVN 的 row/img 距离写入。
    for (uint32_t z = 0; z < p->nz; z++) {
        const uint8_t *src = (const uint8_t *)map + (size_t)z * p->staging_stride;
        for (uint32_t y = 0; y < p->block_rows; y++) {
            uint8_t *row = dst + (size_t)z * p->dst_img + (size_t)y * p->dst_row;
            memcpy(row, src + (size_t)y * p->copy_row_bytes, p->copy_row_bytes);
            if (depth_f32_to_24) nvn_vk_depth_f32_to_24(row, p->copy_row_bytes / 4);
        }
    }
    return 0;
}
