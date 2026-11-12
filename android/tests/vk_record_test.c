// C3：用真实 nvn_obj.h 记录布局构造命令流，经 Vulkan decoder 在真机执行并回读 guest buffer。
// 只覆盖控制流 + CLEAR_BUF/COPY_B2B/UPDATE_UB；其它 opcode 必须显式报不支持。
#include "../native/direct/vk_record.h"
#include "../native/direct/vk_clear.h"
#include "nvn_vk_query.h"
#include "nvn_obj.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// TIMESTAMP_TOP 的写回必须落在游戏时钟上。生产里 now_ns/guest_ns/guest_tick 来自 runtime/src/os.c；
// 微例不链接 os.c，这里给同语义的强符号：guest 时钟比单调钟固定慢 1 s（模拟切后台冻结过的时间），
// 用来验证查询模块真的做了 guest_ns()-now_ns() 的平移，而不只是换了 GPU 自己的计数器。
static const uint64_t test_guest_lost_ns = 1000000000ull;
uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint64_t guest_ns(void) {
    uint64_t n = now_ns();
    return n > test_guest_lost_ns ? n - test_guest_lost_ns : 0;
}
uint64_t guest_tick(void) { return guest_ns() * 12 / 625; }

struct guest { uint8_t bytes[4096]; };
static int failures;

// vk_record 通过弱符号请求结束 open pass。测试里提供实现，用来验证“立即提交 GPU 工作”的 opcode
// （CLEAR_DS/CLEAR_COLOR/COPY…）确实请求了 sync 提交，而只改 CPU 记录的 SetRenderTargets 没有。
// g_break_fail 模拟生产帧队列“没有拿到完成证明”（提交失败/超时）：sync 边界必须返回失败，
// 由 record 在该 opcode 位置准确失败并停止后续 clear/copy/fence/event。
static int g_break_sync_calls;
static int g_break_fail;
static struct s2a_vk_device *g_fixture_device;
static uint64_t g_fixture_query_batch;
static const char g_break_first_error[] = "frame queue: injected submit failure (test)";
int nvn_vk_first_draw_pass_break(int sync);
int nvn_vk_first_draw_pass_break(int sync) {
    if (sync) g_break_sync_calls++;
    if (sync && g_break_fail) return -1;
    if (sync && g_fixture_device) {
        // The standalone decoder submits draws itself. Mirror the production
        // frame queue's query ownership callbacks only after real queue idle.
        if (vkQueueWaitIdle(g_fixture_device->queue) != VK_SUCCESS) return -1;
        uint64_t batch = ++g_fixture_query_batch;
        nvn_vk_query_batch_submitted(batch);
        char why[200] = "";
        if (nvn_vk_query_batch_completed(batch, why, sizeof why) != 0) {
            fprintf(stderr, "fixture query retirement failed: %s\n", why);
            return -1;
        }
    }
    return 0;
}
const char *nvn_vk_first_draw_first_error(void);
const char *nvn_vk_first_draw_first_error(void) { return g_break_fail ? g_break_first_error : ""; }

static void put(uint8_t *stream, size_t *at, uint32_t op, uint32_t size);

// B2T / T2B 按生产记录写（hle_nvn.c REC_SNAP）：载荷后 SNAP_OFF(p_copy_b2t) 处一份 tex_t 快照，
// 回放只从快照取 NVN 格式 / 目标 / 层数。返回整条长度。
static uint32_t put_copy_b2t(uint8_t *buf, size_t *at, uint32_t op, const p_copy_b2t *cp, const tex_t *snap) {
    const size_t off = (sizeof(p_copy_b2t) + 7) & ~(size_t)7;
    uint32_t size = (uint32_t)((sizeof(op_hdr) + off + sizeof(tex_t) + 7) & ~7u);
    put(buf, at, op, size);
    uint8_t *payload = buf + *at - size + sizeof(op_hdr);
    p_copy_b2t record = *cp;
    record.tex = (const tex_t *)(payload + off);
    memcpy(payload, &record, sizeof record);
    memcpy(payload + off, snap, sizeof *snap);
    return size;
}
static tex_t rgba8_snap(uint32_t w, uint32_t h, uint32_t levels, int target, uint32_t layers) {
    tex_t t; memset(&t, 0, sizeof t);
    t.magic = 0x00584554; t.d.format = NVN_FORMAT_RGBA8; t.d.target = target;
    t.d.width = w; t.d.height = h; t.d.depth = layers; t.d.levels = levels;
    return t;
}

// Match REC_SNAP: the record points at the embedded copy, not a synthetic
// numeric key or the API object that may have been reused before replay.
static void put_clear_tex(uint8_t *buf, size_t *at, const p_clear_tex *clear, const tex_t *snap) {
    const size_t off = (sizeof(p_clear_tex) + 7) & ~(size_t)7;
    uint32_t size = (uint32_t)(sizeof(op_hdr) + off + sizeof *snap);
    put(buf, at, OP_CLEAR_TEX, size);
    uint8_t *payload = buf + *at - size + sizeof(op_hdr);
    p_clear_tex record = *clear;
    record.tex = (const tex_t *)(payload + off);
    memcpy(payload, &record, sizeof record);
    memcpy(payload + off, snap, sizeof *snap);
}

static void put_copy_t2t(uint8_t *buf, size_t *at, const p_copy_t2t *copy,
                         const tex_t *src, const tex_t *dst) {
    const size_t off = (sizeof(p_copy_t2t) + 7) & ~(size_t)7;
    uint32_t size = (uint32_t)(sizeof(op_hdr) + off + 2 * sizeof *src);
    put(buf, at, OP_COPY_T2T, size);
    uint8_t *payload = buf + *at - size + sizeof(op_hdr);
    p_copy_t2t record = *copy;
    record.src = (const tex_t *)(payload + off);
    record.dst = (const tex_t *)(payload + off + sizeof *src);
    memcpy(payload, &record, sizeof record);
    memcpy(payload + off, src, sizeof *src);
    memcpy(payload + off + sizeof *src, dst, sizeof *dst);
}

// SET_RT 按紧凑记录写（nvn_obj.h p_set_rt_c）：头 + 指针 + 视图 + 快照区；快照区留零，指针直接指向测试的对象
static void put_set_rt(uint8_t *buf, size_t *at, const p_set_rt *rt) {
    uint32_t size = (uint32_t)((sizeof(op_hdr) + set_rt_size(rt) + 7) & ~7u);
    put(buf, at, OP_SET_RT, size);
    set_rt_put(buf + *at - size + sizeof(op_hdr), rt);
}

static void put_set_rt_depth(uint8_t *buf, size_t *at, const tex_t *depth) {
    p_set_rt rt;
    memset(&rt, 0, sizeof rt);
    rt.n = 0;
    rt.depth = depth;
    put_set_rt(buf, at, &rt);
}

static void put_set_rt_depth_view(uint8_t *buf, size_t *at, const tex_t *depth,
                                  int base_level, int nlevels, int min_layer, int nlayers) {
    p_set_rt rt;
    memset(&rt, 0, sizeof rt);
    rt.n = 0;
    rt.depth = depth;
    rt.has_dview = 1;
    rt.dview.set_levels = 1; rt.dview.base_level = (uint8_t)base_level; rt.dview.nlevels = (uint8_t)nlevels;
    rt.dview.set_layers = 1; rt.dview.min_layer = (uint16_t)min_layer; rt.dview.nlayers = (uint16_t)nlayers;
    put_set_rt(buf, at, &rt);
}

static void put_scissor(uint8_t *buf, size_t *at, int x, int y, int w, int h) {
    uint32_t size = (uint32_t)((sizeof(op_hdr) + sizeof(p_scissor) + 7) & ~7u);
    put(buf, at, OP_SCISSOR, size);
    p_scissor sc = { x, y, w, h };
    memcpy(buf + *at - size + sizeof(op_hdr), &sc, sizeof sc);
}

static void put_clear_ds(uint8_t *buf, size_t *at, float depth, int dmask, int stencil, int smask) {
    uint32_t size = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_ds) + 7) & ~7u);
    put(buf, at, OP_CLEAR_DS, size);
    p_clear_ds v;
    memset(&v, 0, sizeof v);
    v.depth = depth; v.depth_mask = dmask; v.stencil = stencil; v.stencil_mask = smask;
    memcpy(buf + *at - size + sizeof(op_hdr), &v, sizeof v);
}

static void put_counter(uint8_t *buf, size_t *at, uint32_t op, uint64_t type, uint64_t addr) {
    uint32_t payload = op == OP_REPORT_COUNTER ? 16u : 8u;
    uint32_t size = (uint32_t)((sizeof(op_hdr) + payload + 7) & ~7u);
    put(buf, at, op, size);
    memcpy(buf + *at - size + sizeof(op_hdr), &type, 8);
    if (op == OP_REPORT_COUNTER) memcpy(buf + *at - size + sizeof(op_hdr) + 8, &addr, 8);
}

static int read_guest(void *opaque, uint64_t address, void *dst, size_t bytes) {
    struct guest *g = opaque;
    if (address > sizeof g->bytes || bytes > sizeof g->bytes - address) return -1;
    memcpy(dst, g->bytes + address, bytes);
    return 0;
}
static int write_guest(void *opaque, uint64_t address, const void *src, size_t bytes) {
    struct guest *g = opaque;
    if (address > sizeof g->bytes || bytes > sizeof g->bytes - address) return -1;
    memcpy(g->bytes + address, src, bytes);
    return 0;
}
struct test_image_info { VkImage image; VkImageAspectFlags aspects; };
static struct test_image_info g_test_images[256];
static size_t g_test_image_count;

static VkImageAspectFlags test_image_aspects(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// Production texture creation gives replay a GENERAL image. Establish the same
// starting contract for every mip/layer rather than handing it UNDEFINED images.
static int prepare_test_image(struct s2a_vk_device *d, VkImage image, VkFormat format,
                              uint32_t levels, uint32_t layers) {
    if (g_test_image_count == sizeof g_test_images / sizeof g_test_images[0]) return -1;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d->device, &ai, &cmd) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    int ok = vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
    VkImageAspectFlags aspects = test_image_aspects(format);
    if (ok) {
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = { aspects, 0, levels, 0, layers } };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
    }
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    int submitted = 0;
    if (ok) ok = vkCreateFence(d->device, &fi, NULL, &fence) == VK_SUCCESS;
    if (ok) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        submitted = vkQueueSubmit(d->queue, 1, &si, fence) == VK_SUCCESS;
        ok = submitted && vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }
    if (submitted && !ok) return -1; // No completion proof: do not recycle live objects.
    if (fence) vkDestroyFence(d->device, fence, NULL);
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
    if (!ok) return -1;
    g_test_images[g_test_image_count++] = (struct test_image_info){ image, aspects };
    return 0;
}

static VkImageAspectFlags test_image_barrier_aspects(VkImage image, VkImageAspectFlags aspect) {
    (void)aspect;
    for (size_t i = g_test_image_count; i > 0; i--)
        if (g_test_images[i - 1].image == image) return g_test_images[i - 1].aspects;
    return 0;
}

static VkImage make_image_usage(struct s2a_vk_device *d, VkFormat format, VkImageUsageFlags usage, VkDeviceMemory *memory) {
    VkImageCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = 8; info.extent.height = 8; info.extent.depth = 1;
    info.mipLevels = 1; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(d->device, &info, NULL, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d->device, image, &req);
    VkMemoryAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = s2a_vk_memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &alloc, NULL, memory) != VK_SUCCESS ||
        vkBindImageMemory(d->device, image, *memory, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (prepare_test_image(d, image, format, 1, 1)) return VK_NULL_HANDLE;
    return image;
}
static VkImage make_image_layers(struct s2a_vk_device *d, VkFormat format, uint32_t levels, uint32_t layers,
                                 VkImageUsageFlags usage, VkDeviceMemory *memory) {
    VkImageCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = 8; info.extent.height = 8; info.extent.depth = 1;
    info.mipLevels = levels; info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(d->device, &info, NULL, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d->device, image, &req);
    VkMemoryAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = s2a_vk_memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &alloc, NULL, memory) != VK_SUCCESS ||
        vkBindImageMemory(d->device, image, *memory, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (prepare_test_image(d, image, format, levels, layers)) return VK_NULL_HANDLE;
    return image;
}

static VkImage make_image(struct s2a_vk_device *d, VkFormat format, VkDeviceMemory *memory) {
    return make_image_usage(d, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, memory);
}

// 按 aspect 读回区域（深度/模板测试用）。bpp 必须是该 aspect 的 texel 字节数。
static int readback_aspect(struct s2a_vk_device *d, VkImage image, VkImageAspectFlags aspect,
                           uint32_t base_level, uint32_t base_layer,
                           uint32_t w, uint32_t h, uint32_t bpp, uint8_t *out, size_t bytes) {
    VkImageAspectFlags barrier_aspects = test_image_barrier_aspects(image, aspect);
    if (!barrier_aspects || !(barrier_aspects & aspect) || bytes < (uint64_t)w * h * bpp) return -1;
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(d->device, &bi, NULL, &buffer) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, buffer, &req);
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    int rc = -1;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &ai, NULL, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(d->device, buffer, memory, 0) != VK_SUCCESS ||
        vkMapMemory(d->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) goto done;
    VkCommandBufferAllocateInfo ca = {0};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = d->command_pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d->device, &ca, &cmd) != VK_SUCCESS) goto done;
    VkCommandBufferBeginInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cb);
    // The clear recorder returns attachments to GENERAL. Keep both aspects of
    // a combined depth/stencil image together when separate layouts are off.
    VkImageMemoryBarrier to_src = {0};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = (VkImageSubresourceRange){barrier_aspects, base_level, 1, base_layer, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = aspect;
    region.imageSubresource.mipLevel = base_level;
    region.imageSubresource.baseArrayLayer = base_layer;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = w; region.imageExtent.height = h; region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    VkImageMemoryBarrier restore = to_src;
    restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restore.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restore.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &restore);
    VkBufferMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(d->device, &fi, NULL, &fence);
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(d->queue, 1, &si, fence) == VK_SUCCESS &&
        vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS) {
        memcpy(out, mapped, bytes);
        rc = 0;
    }
    vkDestroyFence(d->device, fence, NULL);
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd);
done:
    if (mapped) vkUnmapMemory(d->device, memory);
    vkDestroyBuffer(d->device, buffer, NULL);
    if (memory) vkFreeMemory(d->device, memory, NULL);
    return rc;
}

static int readback_image(struct s2a_vk_device *d, VkImage image, VkImageLayout layout, uint8_t *out, size_t bytes) {
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(d->device, &bi, NULL, &buffer) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->device, buffer, &req);
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(d, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d->device, &ai, NULL, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(d->device, buffer, memory, 0) != VK_SUCCESS ||
        vkMapMemory(d->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo ca = {0};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = d->command_pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(d->device, &ca, &cmd);
    VkCommandBufferBeginInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cb);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = 8; region.imageExtent.height = 8; region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, image, layout, buffer, 1, &region);
    VkBufferMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(d->device, &fi, NULL, &fence);
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    int rc = vkQueueSubmit(d->queue, 1, &si, fence) == VK_SUCCESS &&
             vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS ? 0 : -1;
    if (rc == 0) memcpy(out, mapped, bytes);
    vkDestroyFence(d->device, fence, NULL);
    vkUnmapMemory(d->device, memory);
    vkDestroyBuffer(d->device, buffer, NULL);
    vkFreeMemory(d->device, memory, NULL);
    return rc;
}

static void put(uint8_t *stream, size_t *at, uint32_t op, uint32_t size) {
    op_hdr header = {op, size};
    memcpy(stream + *at, &header, sizeof header);
    memset(stream + *at + sizeof header, 0, size - sizeof header);
    *at += size;
}
// OP_WAIT_SYNC：命令缓冲里的 GPU 等待（主线 34a54ae83 起录制）。回放只需读 guest 镜像
// sync_t.fenced（addr+4）：本后端每笔记录提交后等到完成，本队列栅栏回放到这里时已成立。
static void wait_sync_record(uint8_t *stream, size_t *at, uint64_t sync_addr) {
    size_t start = *at;
    put(stream, at, OP_WAIT_SYNC, 16);
    memcpy(stream + start + 8, &sync_addr, 8);
}
static void clear_record(uint8_t *stream, size_t *at, uint64_t address, uint64_t bytes, uint32_t value) {
    size_t start = *at;
    put(stream, at, OP_CLEAR_BUF, 32);
    p_clear_buf payload = {address, bytes, value, 0};
    memcpy(stream + start + 8, &payload, sizeof payload);
}
static void copy_record(uint8_t *stream, size_t *at, uint64_t src, uint64_t dst, uint64_t bytes) {
    size_t start = *at;
    put(stream, at, OP_COPY_B2B, 32);
    memcpy(stream + start + 8, &src, 8); memcpy(stream + start + 16, &dst, 8);
    memcpy(stream + start + 24, &bytes, 8);
}
static void update_record(uint8_t *stream, size_t *at, uint64_t dst, const void *data, size_t bytes) {
    uint32_t size = (uint32_t)((24 + bytes + 7) & ~7u);
    size_t start = *at;
    put(stream, at, OP_UPDATE_UB, size);
    uint64_t n = bytes;
    memcpy(stream + start + 8, &dst, 8); memcpy(stream + start + 16, &n, 8);
    memcpy(stream + start + 24, data, bytes);
}
static VkPipeline make_draw_pipeline(struct s2a_vk_device *d, VkRenderPass pass, const char *vert, const char *frag) {
    FILE *fv = fopen(vert, "rb"), *ff = fopen(frag, "rb");
    if (!fv || !ff) return VK_NULL_HANDLE;
    fseek(fv, 0, SEEK_END); long nv = ftell(fv); fseek(fv, 0, SEEK_SET);
    fseek(ff, 0, SEEK_END); long nf = ftell(ff); fseek(ff, 0, SEEK_SET);
    uint32_t *cv = malloc((size_t)nv), *cf = malloc((size_t)nf);
    if (!cv || !cf || fread(cv, 1, (size_t)nv, fv) != (size_t)nv || fread(cf, 1, (size_t)nf, ff) != (size_t)nf) return VK_NULL_HANDLE;
    fclose(fv); fclose(ff);
    VkShaderModuleCreateInfo vi = {0}, fi = {0};
    vi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; vi.codeSize = (size_t)nv; vi.pCode = cv;
    fi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; fi.codeSize = (size_t)nf; fi.pCode = cf;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &vi, NULL, &vs) != VK_SUCCESS ||
        vkCreateShaderModule(d->device, &fi, NULL, &fs) != VK_SUCCESS) return VK_NULL_HANDLE;
    free(cv); free(cf);
    VkPipelineShaderStageCreateInfo stages[2] = {{0}};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = {0};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, 8, 8, 0, 1};
    VkRect2D sc = {{0, 0}, {8, 8}};
    VkPipelineViewportStateCreateInfo vsst = {0};
    vsst.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vsst.viewportCount = 1; vsst.pViewports = &vp; vsst.scissorCount = 1; vsst.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkPipelineLayoutCreateInfo li = {0};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(d->device, &li, NULL, &layout);
    VkGraphicsPipelineCreateInfo gp = {0};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vsst;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.layout = layout; gp.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline);
    vkDestroyShaderModule(d->device, vs, NULL); vkDestroyShaderModule(d->device, fs, NULL);
    vkDestroyPipelineLayout(d->device, layout, NULL);
    return pipeline;
}


static VkShaderModule module_from_file(struct s2a_vk_device *d, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return VK_NULL_HANDLE;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n);
    if (!code || fread(code, 1, (size_t)n, f) != (size_t)n) { free(code); fclose(f); return VK_NULL_HANDLE; }
    fclose(f);
    VkShaderModuleCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = (size_t)n; info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &info, NULL, &module) != VK_SUCCESS) module = VK_NULL_HANDLE;
    free(code);
    return module;
}

static int make_compute(struct s2a_vk_device *d, const char *path, VkBuffer buffer,
                        VkPipeline *pipeline_out, VkPipelineLayout *layout_out, VkDescriptorSet *set_out,
                        VkDescriptorPool *pool_out, VkDescriptorSetLayout *set_layout_out) {
    VkShaderModule module = module_from_file(d, path);
    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo sli = {0};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 1; sli.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(d->device, &sli, NULL, &set_layout);
    VkPipelineLayoutCreateInfo pli = {0};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(d->device, &pli, NULL, &layout);
    VkComputePipelineCreateInfo cpi = {0};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = module; cpi.stage.pName = "main";
    cpi.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult pr = vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {0};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(d->device, &dpi, NULL, &pool);
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(d->device, &ai, &set);
    VkDescriptorBufferInfo bi = {buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {0};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(d->device, 1, &w, 0, NULL);
    vkDestroyShaderModule(d->device, module, NULL);
    if (pr != VK_SUCCESS || !layout || !set) {
        if (pipeline) vkDestroyPipeline(d->device, pipeline, NULL);
        if (pool) vkDestroyDescriptorPool(d->device, pool, NULL);
        if (layout) vkDestroyPipelineLayout(d->device, layout, NULL);
        if (set_layout) vkDestroyDescriptorSetLayout(d->device, set_layout, NULL);
        return -1;
    }
    *pipeline_out = pipeline; *layout_out = layout; *set_out = set;
    *pool_out = pool; *set_layout_out = set_layout;
    return 0;
}

// F3.2：拼一条“可选 SetRT / BindProgram / Reset / Draw / 辅助清屏 / Report”的记录流。
// 所有 query 都是共享 NVN 前端的真实记录，不是测试自造的语义。
static size_t build_query_stream(uint8_t *buf, const tex_t *tex, const prog_t *prog, int do_set_rt,
                                 int do_draw, int draw_count, int do_reset, int do_aux_clear,
                                 uint64_t report_addr) {
    size_t at = 0;
    if (do_set_rt) {
        p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = tex;
        put_set_rt(buf, &at, &rt);
    }
    if (prog) {
        uint32_t bs = (uint32_t)((sizeof(op_hdr) + sizeof(p_bind_program_id) + 7) & ~7u);
        put(buf, &at, OP_BIND_PROGRAM, bs);
        p_bind_program_id bp; memset(&bp, 0, sizeof bp); bp.prog = prog;
        memcpy(buf + at - bs + sizeof(op_hdr), &bp, sizeof bp);
    }
    if (do_reset) put_counter(buf, &at, OP_RESET_COUNTER, 1, 0);
    if (do_draw) {
        uint32_t ds = (uint32_t)((sizeof(op_hdr) + sizeof(p_draw) + 7) & ~7u);
        put(buf, &at, OP_DRAW, ds);
        p_draw dr; memset(&dr, 0, sizeof dr);
        dr.prim = 0; dr.first = 0; dr.count = draw_count; dr.instances = 1;
        memcpy(buf + at - ds + sizeof(op_hdr), &dr, sizeof dr);
    }
    if (do_aux_clear) {
        // 部分颜色掩码清屏走辅助全屏三角形（vk_clear 的 mask 路径）。它不是游戏绘制，不能计数。
        uint32_t cs = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
        put(buf, &at, OP_CLEAR_COLOR, cs);
        p_clear_color cc; memset(&cc, 0, sizeof cc);
        cc.index = 0; cc.mask = 0x3; cc.rgba[0] = 0.0f; cc.rgba[1] = 1.0f; cc.rgba[2] = 0.0f; cc.rgba[3] = 1.0f;
        memcpy(buf + at - cs + sizeof(op_hdr), &cc, sizeof cc);
    }
    if (report_addr) put_counter(buf, &at, OP_REPORT_COUNTER, 1, report_addr);
    put(buf, &at, OP_END, 8);
    return at;
}

int main(int argc, char **argv) {
    struct s2a_vk_device device;
    char why[192] = "";
    if (s2a_vk_device_create(&device, why, sizeof why)) { printf("FAIL device: %s\n", why); return 1; }
    g_fixture_device = &device;
    printf("INFO device: shaderDrawParameters=%d maintenance1=%d timeline=%d\n",
           device.shader_draw_parameters_enabled, device.swapchain_maintenance1_enabled,
           device.timeline_semaphore_enabled);
    uint8_t stream[1024];
    memset(stream, 0, sizeof stream);
    size_t at = 0;
    clear_record(stream, &at, 0x100, 64, 0xA5A5A5A5u);          // 0
    copy_record(stream, &at, 0x100, 0x200, 64);                 // 32
    uint8_t pattern[16];
    for (int i = 0; i < 16; i++) pattern[i] = (uint8_t)(0x40 + i);
    update_record(stream, &at, 0x300, pattern, sizeof pattern); // 64
    wait_sync_record(stream, &at, 0x700);                       // 已围栏的 sync：应直接通过
    size_t call_at = at;                                        // 96
    put(stream, &at, OP_CALL, 16);
    clear_record(stream, &at, 0x500, 8, 0x01020304u);          // 120
    size_t jump_at = at;                                        // 152
    put(stream, &at, OP_JUMP, 16);
    clear_record(stream, &at, 0x600, 8, 0xDEADBEEFu);          // 176 (must be skipped)
    put(stream, &at, OP_END, 8);                                // 208
    size_t block_at = at;                                       // 216? align: puts are 32/32/32/16...
    clear_record(stream, &at, 0x400, 8, 0x11223344u);
    size_t end_at = at;
    put(stream, &at, OP_END, 8);
    uint64_t call_target = (uint64_t)(uintptr_t)(stream + block_at);
    uint64_t jump_target = (uint64_t)(uintptr_t)(stream + end_at);
    memcpy(stream + call_at + 8, &call_target, 8);
    memcpy(stream + jump_at + 8, &jump_target, 8);

    struct guest guest;
    memset(&guest, 0, sizeof guest);
    {   // sync_t { magic, fenced }：fenced=1 表示已围栏（本后端回放到这里时本队列栅栏已成立）
        uint32_t magic = 0x534e5953u /* 'SYNS' */, fenced = 1;
        memcpy(guest.bytes + 0x700, &magic, 4);
        memcpy(guest.bytes + 0x704, &fenced, 4);
    }
    struct s2a_record_memory memory = {&guest, read_guest, write_guest};
    struct s2a_record_result result;
    int rc = s2a_record_replay(&device, stream, stream + at, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
    if (rc) { printf("FAIL replay: %d op=%u %s\n", rc, result.opcode, result.why); failures++; }
    for (size_t i = 0; i < 64; i++)
        if (guest.bytes[0x100 + i] != 0xA5 || guest.bytes[0x200 + i] != 0xA5) { failures++; printf("FAIL fill/copy at %zu\n", i); break; }
    if (memcmp(guest.bytes + 0x300, pattern, sizeof pattern)) { failures++; printf("FAIL uniform update\n"); }
    uint32_t block = 0;
    memcpy(&block, guest.bytes + 0x400, 4);
    if (block != 0x11223344u) { failures++; printf("FAIL call block value %08x\n", block); }
    uint32_t after = 0;
    memcpy(&after, guest.bytes + 0x500, 4);
    if (after != 0x01020304u) { failures++; printf("FAIL post-call record %08x\n", after); }
    if (guest.bytes[0x600] != 0) { failures++; printf("FAIL jump did not skip record\n"); }
    printf("INFO replay: executed=%llu (%s)\n", (unsigned long long)result.executed, rc ? result.why : "ok");

    // 反例：镜像未围栏（fenced=0）→ 有界等待（1000 ms）后必须准确报 UNSUPPORTED，
    // 且其后的记录一条都不执行——不假装等到、不静默跳过。
    {
        struct guest cold;
        memset(&cold, 0, sizeof cold);
        uint32_t magic = 0x534e5953u, fenced = 0;
        memcpy(cold.bytes + 0x780, &magic, 4);
        memcpy(cold.bytes + 0x784, &fenced, 4);
        struct s2a_record_memory cold_memory = {&cold, read_guest, write_guest};
        uint8_t cold_stream[64];
        memset(cold_stream, 0, sizeof cold_stream);
        size_t cat = 0;
        wait_sync_record(cold_stream, &cat, 0x780);
        clear_record(cold_stream, &cat, 0x7C0, 8, 0x55AA55AAu);   // 这条不能执行
        put(cold_stream, &cat, OP_END, 8);
        struct s2a_record_result cold_result;
        int cold_rc = s2a_record_replay(&device, cold_stream, cold_stream + cat, &cold_memory,
                                        NULL, 0, NULL, 0, NULL, 0, &cold_result);
        uint32_t after_wait = 0;
        memcpy(&after_wait, cold.bytes + 0x7C0, 4);
        if (cold_rc == 0 || cold_result.code != S2A_RECORD_UNSUPPORTED || cold_result.opcode != OP_WAIT_SYNC) {
            failures++;
            printf("FAIL wait-sync unfenced: rc=%d code=%d op=%u %s (expected UNSUPPORTED at OP_WAIT_SYNC)\n",
                   cold_rc, (int)cold_result.code, cold_result.opcode, cold_result.why);
        }
        if (after_wait != 0) { failures++; printf("FAIL wait-sync unfenced: later record executed (%08x)\n", after_wait); }
        printf("INFO wait-sync unfenced: rc=%d code=%d %s\n", cold_rc, (int)cold_result.code, cold_result.why);
    }

    // CLEAR_TEX：同一记录格式的 float / signed 两态，经 Vulkan 真机清除后读回。
    VkDeviceMemory unorm_memory = VK_NULL_HANDLE, sint_memory = VK_NULL_HANDLE;
    VkImage unorm = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &unorm_memory);
    VkImage sint = make_image(&device, VK_FORMAT_R8G8B8A8_SINT, &sint_memory);
    if (!unorm || !sint) { failures++; printf("FAIL clear-texture image creation\n"); }
    else {
        _Alignas(tex_t) uint8_t texture_stream[1024];
        memset(texture_stream, 0, sizeof texture_stream);
        size_t tat = 0;
        tex_t unorm_snap = rgba8_snap(8, 8, 1, NVN_TEXTURE_TARGET_2D, 1);
        tex_t sint_snap = unorm_snap;
        unorm_snap.serial = 101;
        sint_snap.serial = 102; sint_snap.d.format = NVN_FORMAT_RGBA8I;
        p_clear_tex fc;
        memset(&fc, 0, sizeof fc);
        fc.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
        fc.mask = 0xF; fc.value_type = CLEAR_TEX_FLOAT;
        fc.c.f[0] = 0.25f; fc.c.f[1] = 0.5f; fc.c.f[2] = 0.75f; fc.c.f[3] = 1.0f;
        put_clear_tex(texture_stream, &tat, &fc, &unorm_snap);
        p_clear_tex ic;
        memset(&ic, 0, sizeof ic);
        ic.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
        ic.mask = 0xF; ic.value_type = CLEAR_TEX_INT;
        ic.c.i[0] = -128; ic.c.i[1] = -1; ic.c.i[2] = 0; ic.c.i[3] = 127;
        put_clear_tex(texture_stream, &tat, &ic, &sint_snap);
        put(texture_stream, &tat, OP_END, 8);
        struct s2a_replay_texture textures[2] = {
            {&unorm_snap, unorm, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 101, 0, 0},
            {&sint_snap, sint, VK_FORMAT_R8G8B8A8_SINT, 8, 8, 1, 102, 0, 0},
        };
        rc = s2a_record_replay(&device, texture_stream, texture_stream + tat, &memory, textures, 2, NULL, 0, NULL, 0, &result);
        if (rc) { failures++; printf("FAIL clear-texture replay: %d %s\n", rc, result.why); }
        else {
            uint8_t unorm_pixels[256], sint_pixels[256];
            if (readback_image(&device, unorm, VK_IMAGE_LAYOUT_GENERAL, unorm_pixels, sizeof unorm_pixels)) { failures++; printf("FAIL unorm readback\n"); }
            else {
                const uint8_t want[4] = {64, 128, 191, 255};
                if (memcmp(unorm_pixels, want, 4)) { failures++; printf("FAIL float clear pixel %u,%u,%u,%u\n",
                    unorm_pixels[0], unorm_pixels[1], unorm_pixels[2], unorm_pixels[3]); }
                printf("INFO clear-texture: unorm=%u,%u,%u,%u\n",
                    unorm_pixels[0], unorm_pixels[1], unorm_pixels[2], unorm_pixels[3]);
            }
            if (readback_image(&device, sint, VK_IMAGE_LAYOUT_GENERAL, sint_pixels, sizeof sint_pixels)) { failures++; printf("FAIL sint readback\n"); }
            else {
                const int8_t got[4] = {(int8_t)sint_pixels[0], (int8_t)sint_pixels[1], (int8_t)sint_pixels[2], (int8_t)sint_pixels[3]};
                const int8_t want[4] = {-128, -1, 0, 127};
                if (memcmp(got, want, 4)) { failures++; printf("FAIL signed clear pixel %d,%d,%d,%d\n", got[0], got[1], got[2], got[3]); }
                printf("INFO clear-texture: sint=%d,%d,%d,%d\n", got[0], got[1], got[2], got[3]);
            }
        }
    }
    vkDestroyImage(device.device, unorm, NULL); if (unorm_memory) vkFreeMemory(device.device, unorm_memory, NULL);
    vkDestroyImage(device.device, sint, NULL); if (sint_memory) vkFreeMemory(device.device, sint_memory, NULL);

    // SET_RT + CLEAR_COLOR：用真实记录结构清除当前渲染目标。
    VkDeviceMemory rt0_memory = VK_NULL_HANDLE, rt1_memory = VK_NULL_HANDLE;
    VkImage rt0 = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &rt0_memory);
    VkImage rt1 = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &rt1_memory);
    if (!rt0 || !rt1) { failures++; printf("FAIL clear-color image creation\n"); }
    else {
        uint8_t rt_stream[4096];
        memset(rt_stream, 0, sizeof rt_stream);
        size_t rat = 0;
        // SET_RT 会按记录里的 const tex_t* 复制 0x80 字节对象头（draw 时用它解析格式）；
        // 所以测试也必须给真实对象，不能拿 0x3 这类假地址当纹理。
        tex_t rt_tex0, rt_tex1;
        memset(&rt_tex0, 0, sizeof rt_tex0); memset(&rt_tex1, 0, sizeof rt_tex1);
        rt_tex0.magic = 0x00584554; rt_tex0.d.format = NVN_FORMAT_RGBA8; rt_tex0.d.width = rt_tex0.d.height = 8; rt_tex0.d.levels = 1; rt_tex0.serial = 31;
        rt_tex1.magic = 0x00584554; rt_tex1.d.format = NVN_FORMAT_RGBA8; rt_tex1.d.width = rt_tex1.d.height = 8; rt_tex1.d.levels = 1; rt_tex1.serial = 32;
        p_set_rt rt_record;
        memset(&rt_record, 0, sizeof rt_record);
        rt_record.n = 2;
        rt_record.color[0] = &rt_tex0;
        rt_record.color[1] = &rt_tex1;
        put_set_rt(rt_stream, &rat, &rt_record);
        uint32_t clear_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
        put(rt_stream, &rat, OP_CLEAR_COLOR, clear_size);
        p_clear_color cc;
        memset(&cc, 0, sizeof cc);
        cc.index = 0; cc.mask = 0xF;
        cc.rgba[0] = 0.125f; cc.rgba[1] = 0.375f; cc.rgba[2] = 0.625f; cc.rgba[3] = 1.0f;
        memcpy(rt_stream + rat - clear_size + 8, &cc, sizeof cc);
        put(rt_stream, &rat, OP_CLEAR_COLOR, clear_size);
        cc.index = 1;
        cc.rgba[0] = 0.25f; cc.rgba[1] = 0.625f; cc.rgba[2] = 0.875f; cc.rgba[3] = 1.0f;
        memcpy(rt_stream + rat - clear_size + 8, &cc, sizeof cc);
        put(rt_stream, &rat, OP_END, 8);
        struct s2a_replay_texture rt_textures[2] = {
            {&rt_tex0, rt0, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 31, 0, 0},
            {&rt_tex1, rt1, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 32, 0, 0},
        };
        rc = s2a_record_replay(&device, rt_stream, rt_stream + rat, &memory, rt_textures, 2, NULL, 0, NULL, 0, &result);
        if (rc) { failures++; printf("FAIL clear-color replay: %d %s\n", rc, result.why); }
        else {
            uint8_t p0[256], p1[256];
            if (readback_image(&device, rt0, VK_IMAGE_LAYOUT_GENERAL, p0, sizeof p0) || readback_image(&device, rt1, VK_IMAGE_LAYOUT_GENERAL, p1, sizeof p1)) {
                failures++; printf("FAIL clear-color readback\n");
            } else {
                const uint8_t w0[4] = {32, 96, 159, 255}, w1[4] = {64, 159, 223, 255};
                if (memcmp(p0, w0, 4) || memcmp(p1, w1, 4)) {
                    failures++; printf("FAIL clear-color pixels %u,%u,%u,%u / %u,%u,%u,%u\n",
                        p0[0], p0[1], p0[2], p0[3], p1[0], p1[1], p1[2], p1[3]);
                }
                printf("INFO clear-color: rt0=%u,%u,%u,%u rt1=%u,%u,%u,%u\n",
                    p0[0], p0[1], p0[2], p0[3], p1[0], p1[1], p1[2], p1[3]);
            }
        }
    }
    vkDestroyImage(device.device, rt0, NULL); if (rt0_memory) vkFreeMemory(device.device, rt0_memory, NULL);
    vkDestroyImage(device.device, rt1, NULL); if (rt1_memory) vkFreeMemory(device.device, rt1_memory, NULL);

    // BIND_PROGRAM + DRAW：真实记录驱动 draw（合成程序/管线；尚未解析 prog_t/资源）。
    // 负例：坏记录/空范围/未注册 RT/无限跳转必须明确失败，不越界、不伪造成功。
    {
        struct s2a_record_result neg;
        op_hdr bad = {OP_END, 12};
        rc = s2a_record_replay(&device, (const uint8_t *)&bad, (const uint8_t *)&bad + sizeof bad,
                               &memory, NULL, 0, NULL, 0, NULL, 0, &neg);
        if (rc != S2A_RECORD_INVALID) { failures++; printf("FAIL unaligned record rc=%d\n", rc); }
        rc = s2a_record_replay(&device, (const uint8_t *)&bad, (const uint8_t *)&bad,
                               &memory, NULL, 0, NULL, 0, NULL, 0, &neg);
        if (rc != S2A_RECORD_ARGUMENT) { failures++; printf("FAIL empty range rc=%d\n", rc); }

        uint8_t loop[16]; size_t latch = 0;
        put(loop, &latch, OP_JUMP, 16);   // target = itself
        uint64_t self = (uint64_t)(uintptr_t)loop;
        memcpy(loop + 8, &self, 8);
        rc = s2a_record_replay(&device, loop, loop + sizeof loop, &memory, NULL, 0, NULL, 0, NULL, 0, &neg);
        if (rc != S2A_RECORD_LIMIT) { failures++; printf("FAIL infinite jump rc=%d\n", rc); }

        uint8_t no_program[64]; size_t nat = 0;
        put(no_program, &nat, OP_DRAW, 32);
        p_draw nd; memset(&nd, 0, sizeof nd); nd.count = 3; nd.instances = 1;
        memcpy(no_program + 8, &nd, sizeof nd);
        put(no_program, &nat, OP_END, 8);
        rc = s2a_record_replay(&device, no_program, no_program + nat, &memory, NULL, 0, NULL, 0, NULL, 0, &neg);
        if (rc != S2A_RECORD_INVALID) { failures++; printf("FAIL draw without program rc=%d\n", rc); }
        uint8_t partial[512]; size_t pat = 0;
        tex_t partial_tex; memset(&partial_tex, 0, sizeof partial_tex);
        partial_tex.magic = 0x00584554; partial_tex.d.format = NVN_FORMAT_RGBA8;
        partial_tex.d.width = partial_tex.d.height = 8; partial_tex.d.levels = 1; partial_tex.serial = 33;
        p_set_rt pr; memset(&pr, 0, sizeof pr); pr.n = 2;
        pr.color[0] = &partial_tex;
        put_set_rt(partial, &pat, &pr);
        uint32_t clear_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
        put(partial, &pat, OP_CLEAR_COLOR, clear_size);
        p_clear_color pc; memset(&pc, 0, sizeof pc); pc.index = 2; pc.mask = 0xF;   // 越界索引
        memcpy(partial + pat - clear_size + 8, &pc, sizeof pc);
        put(partial, &pat, OP_END, 8);
        rc = s2a_record_replay(&device, partial, partial + pat, &memory, NULL, 0, NULL, 0, NULL, 0, &neg);
        if (rc != S2A_RECORD_INVALID) { failures++; printf("FAIL clear-color index outside RT rc=%d\n", rc); }

        size_t pat2 = 0;
        p_set_rt nine; memset(&nine, 0, sizeof nine); nine.n = 9;
        put_set_rt(partial, &pat2, &nine);
        rc = s2a_record_replay(&device, partial, partial + pat2, &memory, NULL, 0, NULL, 0, NULL, 0, &neg);
        if (rc != S2A_RECORD_INVALID) { failures++; printf("FAIL nine render targets rc=%d\n", rc); }
        printf("INFO negative: unaligned/empty/loop/no-program/clear-index/nine-RT rejected\n");
    }

    // 紧凑记录（main 的生产端规则）：单条顶点缓冲只写 {first, size, addr}，整条 24 B。
    // 旧解码把前 8 字节当 {first, n}，会把 size 当数量拒绝——这里正反两面都钉住。
    {
        struct s2a_record_result cr;
        uint8_t cb[512];
        size_t cat = 0;
        uint32_t vsz = (uint32_t)(sizeof(op_hdr) + sizeof(p_bind_vbuf_single));
        put(cb, &cat, OP_BIND_VBUF, vsz);
        p_bind_vbuf_single vb = {3, 4096u, 0x123456780ull};
        memcpy(cb + 8, &vb, sizeof vb);
        put(cb, &cat, OP_END, 8);
        if (vsz != 24) { failures++; printf("FAIL compact vbuf record size=%u (want 24)\n", vsz); }
        rc = s2a_record_replay(&device, cb, cb + cat, &memory, NULL, 0, NULL, 0, NULL, 0, &cr);
        if (rc != S2A_RECORD_OK) { failures++; printf("FAIL compact vbuf rejected rc=%d: %s\n", rc, cr.why); }

        // 完整记录（两条，含 >32 位 size）必须继续可用
        size_t fat = 0;
        uint32_t fsz = (uint32_t)(sizeof(op_hdr) + 8 + 2 * sizeof(NVNbufferRange));
        put(cb, &fat, OP_BIND_VBUF, fsz);
        p_bind_vbuf fb; memset(&fb, 0, sizeof fb); fb.first = 2; fb.n = 2;
        fb.r[0].address = (NVNbufferAddress)0x1000; fb.r[0].size = 16;
        fb.r[1].address = (NVNbufferAddress)0x2000; fb.r[1].size = (uint64_t)UINT32_MAX + 1;
        memcpy(cb + 8, &fb, sizeof fb);
        put(cb, &fat, OP_END, 8);
        rc = s2a_record_replay(&device, cb, cb + fat, &memory, NULL, 0, NULL, 0, NULL, 0, &cr);
        if (rc != S2A_RECORD_OK) { failures++; printf("FAIL full vbuf rejected rc=%d: %s\n", rc, cr.why); }
        printf("INFO compact records: vbuf %u B ok, full %u B ok\n", vsz, fsz);
    }

    if (argc >= 2) {
        VkDeviceMemory draw_memory = VK_NULL_HANDLE;
        VkImage draw_image = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &draw_memory);
        VkRenderPass pass = VK_NULL_HANDLE;
        VkAttachmentDescription color = {0};
        color.format = VK_FORMAT_R8G8B8A8_UNORM; color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {0};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency deps[2] = {{0}};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp = {0};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1; rp.pAttachments = &color; rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 2; rp.pDependencies = deps;
        vkCreateRenderPass(device.device, &rp, NULL, &pass);
        VkImageViewCreateInfo vci = {0};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = draw_image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device.device, &vci, NULL, &view);
        VkFramebufferCreateInfo fbi = {0};
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = pass; fbi.attachmentCount = 1; fbi.pAttachments = &view;
        fbi.width = 8; fbi.height = 8; fbi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(device.device, &fbi, NULL, &fb);
        char vert[512], frag[512];
        snprintf(vert, sizeof vert, "%s/tri.vert.spv", argv[1]);
        snprintf(frag, sizeof frag, "%s/tri.frag.spv", argv[1]);
        VkPipeline pipeline = make_draw_pipeline(&device, pass, vert, frag);
        if (!pass || !view || !fb || !pipeline) { failures++; printf("FAIL draw-record pipeline creation\n"); }
        else {
            uint8_t draw_stream[16384];
            memset(draw_stream, 0, sizeof draw_stream);
            size_t dat = 0;
            tex_t draw_tex; memset(&draw_tex, 0, sizeof draw_tex);
            draw_tex.magic = 0x00584554; draw_tex.d.format = NVN_FORMAT_RGBA8;
            draw_tex.d.width = draw_tex.d.height = 8; draw_tex.d.levels = 1; draw_tex.serial = 34;
            p_set_rt rt;
            memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &draw_tex;
            put_set_rt(draw_stream, &dat, &rt);
            uint32_t bp_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_bind_program_id) + 7) & ~7u);
            put(draw_stream, &dat, OP_BIND_PROGRAM, bp_size);
            p_bind_program_id bp;
            memset(&bp, 0, sizeof bp); bp.prog = (const prog_t *)(uintptr_t)0x5;
            memcpy(draw_stream + dat - bp_size + 8, &bp, sizeof bp);
            uint32_t dr_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_draw) + 7) & ~7u);
            put(draw_stream, &dat, OP_DRAW, dr_size);
            p_draw dr;
            memset(&dr, 0, sizeof dr); dr.prim = 0; dr.first = 0; dr.count = 3; dr.instances = 1;
            memcpy(draw_stream + dat - dr_size + 8, &dr, sizeof dr);
            put(draw_stream, &dat, OP_END, 8);
            struct s2a_replay_draw draws[1] = {
                {(const void *)(uintptr_t)0x5, &draw_tex, pipeline, pass, fb, {8, 8}, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0},
            };
            rc = s2a_record_replay(&device, draw_stream, draw_stream + dat, &memory, NULL, 0, draws, 1, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL draw-record replay: %d %s\n", rc, result.why); }
            else {
                uint8_t pixels[256];
                if (readback_image(&device, draw_image, VK_IMAGE_LAYOUT_GENERAL, pixels, sizeof pixels)) { failures++; printf("FAIL draw-record readback\n"); }
                else {
                    const uint8_t *center = pixels + (4 * 8 + 4) * 4;
                    if (abs(center[0] - 143) > 3 || abs(center[1] - 143) > 3 || center[2] != 0 || center[3] != 255) {
                        failures++; printf("FAIL draw-record center %u,%u,%u,%u\n", center[0], center[1], center[2], center[3]);
                    }
                    printf("INFO draw-record: center=%u,%u,%u,%u\n", center[0], center[1], center[2], center[3]);
                }
            }
        }
        // ---- F0.9 顺序联合用例 1：draw（红）→ ClearColor（蓝）→ 读回蓝 ----
        // 清除必须落在 draw 之后（命令位置），且 draw 的完成证明不能只靠“调用返回”。
        if (pass && fb && pipeline) {
            uint8_t joint[4096];
            memset(joint, 0, sizeof joint);
            size_t jat = 0;
            tex_t jtex; memset(&jtex, 0, sizeof jtex);
            jtex.magic = 0x00584554; jtex.d.format = NVN_FORMAT_RGBA8;
            jtex.d.width = jtex.d.height = 8; jtex.d.levels = 1; jtex.serial = 34;
            p_set_rt jrt; memset(&jrt, 0, sizeof jrt); jrt.n = 1; jrt.color[0] = &jtex;
            put_set_rt(joint, &jat, &jrt);
            uint32_t jbp = (uint32_t)((sizeof(op_hdr) + sizeof(p_bind_program_id) + 7) & ~7u);
            put(joint, &jat, OP_BIND_PROGRAM, jbp);
            p_bind_program_id jprog; memset(&jprog, 0, sizeof jprog); jprog.prog = (const prog_t *)(uintptr_t)0x5;
            memcpy(joint + jat - jbp + 8, &jprog, sizeof jprog);
            uint32_t jdr = (uint32_t)((sizeof(op_hdr) + sizeof(p_draw) + 7) & ~7u);
            put(joint, &jat, OP_DRAW, jdr);
            p_draw jdraw; memset(&jdraw, 0, sizeof jdraw); jdraw.prim = 0; jdraw.first = 0; jdraw.count = 3; jdraw.instances = 1;
            memcpy(joint + jat - jdr + 8, &jdraw, sizeof jdraw);
            uint32_t jclr = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
            put(joint, &jat, OP_CLEAR_COLOR, jclr);
            p_clear_color jc; memset(&jc, 0, sizeof jc);
            jc.index = 0; jc.mask = 0xF; jc.rgba[0] = 0.0f; jc.rgba[1] = 0.0f; jc.rgba[2] = 1.0f; jc.rgba[3] = 1.0f;
            memcpy(joint + jat - jclr + 8, &jc, sizeof jc);
            put(joint, &jat, OP_END, 8);
            struct s2a_replay_texture jtexs[1] = {
                {&jtex, draw_image, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 34, 0, 0},
            };
            struct s2a_replay_draw jdraws[1] = {
                {(const void *)(uintptr_t)0x5, &jtex, pipeline, pass, fb, {8, 8}, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0},
            };
            rc = s2a_record_replay(&device, joint, joint + jat, &memory, jtexs, 1, jdraws, 1, NULL, 0, &result);
            uint8_t px[256];
            if (rc) { failures++; printf("FAIL order draw->clear replay: %d %s\n", rc, result.why); }
            else if (readback_image(&device, draw_image, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL order draw->clear readback\n");
            } else {
                const uint8_t *c = px + (4 * 8 + 4) * 4;
                if (c[0] > 3 || c[1] > 3 || c[2] < 250 || c[3] != 255) {
                    failures++; printf("FAIL order draw->clear center=%u,%u,%u,%u want blue\n", c[0], c[1], c[2], c[3]);
                } else {
                    printf("INFO order draw->clear: center=%u,%u,%u,%u (blue after the draw)\n", c[0], c[1], c[2], c[3]);
                }
            }
        }

        // ---- F0.9 顺序联合用例 2：clear A（红）→ 绑 B → 只清 B（绿）：A 不被改动 ----
        {
            VkDeviceMemory bmem = VK_NULL_HANDLE;
            VkImage bimg = make_image_usage(&device, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT, &bmem);
            tex_t btex; memset(&btex, 0, sizeof btex);
            btex.magic = 0x00584554; btex.d.format = NVN_FORMAT_RGBA8;
            btex.d.width = btex.d.height = 8; btex.d.levels = 1; btex.serial = 35;
            if (!bimg) { failures++; printf("FAIL order clear-A/B image creation\n"); }
            else {
                uint8_t st[4096];
                memset(st, 0, sizeof st);
                size_t sat = 0;
                tex_t atex; memset(&atex, 0, sizeof atex);
                atex.magic = 0x00584554; atex.d.format = NVN_FORMAT_RGBA8;
                atex.d.width = atex.d.height = 8; atex.d.levels = 1; atex.serial = 34;
                for (int k = 0; k < 2; k++) {
                    p_set_rt srt; memset(&srt, 0, sizeof srt); srt.n = 1; srt.color[0] = k ? &btex : &atex;
                    put_set_rt(st, &sat, &srt);
                    uint32_t cls = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
                    put(st, &sat, OP_CLEAR_COLOR, cls);
                    p_clear_color cc; memset(&cc, 0, sizeof cc);
                    cc.index = 0; cc.mask = 0xF;
                    cc.rgba[0] = k ? 0.0f : 1.0f; cc.rgba[1] = k ? 1.0f : 0.0f; cc.rgba[2] = 0.0f; cc.rgba[3] = 1.0f;
                    memcpy(st + sat - cls + 8, &cc, sizeof cc);
                }
                put(st, &sat, OP_END, 8);
                struct s2a_replay_texture ste[2] = {
                    {&atex, draw_image, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 34, 0, 0},
                    {&btex, bimg, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 35, 0, 0},
                };
                rc = s2a_record_replay(&device, st, st + sat, &memory, ste, 2, NULL, 0, NULL, 0, &result);
                uint8_t pa[256], pb[256];
                if (rc) { failures++; printf("FAIL order clear-A/B replay: %d %s\n", rc, result.why); }
                else if (readback_image(&device, draw_image, VK_IMAGE_LAYOUT_GENERAL, pa, sizeof pa) ||
                         readback_image(&device, bimg, VK_IMAGE_LAYOUT_GENERAL, pb, sizeof pb)) {
                    failures++; printf("FAIL order clear-A/B readback\n");
                } else {
                    const uint8_t *ca = pa + (4 * 8 + 4) * 4, *cb = pb + (4 * 8 + 4) * 4;
                    if (!(ca[0] > 250 && ca[1] < 3 && ca[2] < 3 && ca[3] == 255) ||
                        !(cb[0] < 3 && cb[1] > 250 && cb[2] < 3 && cb[3] == 255)) {
                        failures++; printf("FAIL order clear-A/B A=%u,%u,%u,%u B=%u,%u,%u,%u\n",
                                          ca[0], ca[1], ca[2], ca[3], cb[0], cb[1], cb[2], cb[3]);
                    } else {
                        printf("INFO order clear-A/B: A=%u,%u,%u,%u B=%u,%u,%u,%u (only the retargeted one changed)\n",
                               ca[0], ca[1], ca[2], ca[3], cb[0], cb[1], cb[2], cb[3]);
                    }
                }
            }
            vkDestroyImage(device.device, bimg, NULL); if (bmem) vkFreeMemory(device.device, bmem, NULL);
        }

    // ---- F3.2：真实 samples-passed 遮挡查询 ----
    // 语义（共享 NVN 前端）：Reset 清零；绘制累加；Report 写当时的累计值且不清零。
    // 每个 render pass = 一个 query 段（Vulkan 的 occlusion query 不能跨 render pass），Report 汇总
    // 自上次 Reset 以来的全部段；结果在 fence 完成证明之后读取并写回 guest。
    if (pass && fb && pipeline) {
        uint8_t qb[8192];
        tex_t qtex;
        memset(&qtex, 0, sizeof qtex);
        qtex.magic = 0x00584554; qtex.d.format = NVN_FORMAT_RGBA8;
        qtex.d.width = qtex.d.height = 8; qtex.d.levels = 1; qtex.serial = 36;
        struct s2a_replay_texture qtx[1] = {
            {&qtex, draw_image, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 36, 0, 0},
        };
        struct s2a_replay_draw qdr[1] = {
            {(const void *)(uintptr_t)0x5, &qtex, pipeline, pass, fb, {8, 8}, VK_NULL_HANDLE, VK_NULL_HANDLE,
             VK_NULL_HANDLE, 0},
        };
        const prog_t *qprog = (const prog_t *)(uintptr_t)0x5;
        uint64_t qc = 0;
        size_t qn;

        // (0) 第一次 Reset 之前 Report：前端模型写 0（没有可反推的真实累计），但必须真的写进
        //     guest 并清掉待写队列；不能“返回成功、一个字节没写”，否则后面的 Reset 会被 pending 卡住。
        memset(guest.bytes + 0x8f0, 0x5A, 16);
        qn = build_query_stream(qb, &qtex, qprog, 0, 0, 0, 0, 0, 0x8f0);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x8f0, 8);
        int no_reset_ts = 1;
        for (int i = 8; i < 16; i++) if (guest.bytes[0x8f0 + i] != 0x5A) no_reset_ts = 0;
        if (rc || qc != 0 || !no_reset_ts) {
            failures++; printf("FAIL query report-before-reset: rc=%d value=%llu ts_sentinel=%d want 0/1 %s\n",
                              rc, (unsigned long long)qc, no_reset_ts, result.why);
        } else {
            printf("INFO query report-before-reset: wrote 0 before any Reset (frontend model)\n");
        }

        // (1) reset -> 全屏三角形（8x8、1 sample/pixel）-> report = 64
        memset(guest.bytes + 0x900, 0x5A, 16);   // +8..16 是 timestamp：本项不写，必须保持哨兵
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 1, 0, 0x900);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x900, 8);
        int ts_sentinel = 1;
        for (int i = 8; i < 16; i++) if (guest.bytes[0x900 + i] != 0x5A) ts_sentinel = 0;
        if (rc) {
            failures++; printf("FAIL query nonzero: rc=%d %s\n", rc, result.why);
        } else if (qc != 64) {
            failures++; printf("FAIL query nonzero: value=%llu want 64 (8x8 target, full-screen triangle)\n",
                              (unsigned long long)qc);
        } else if (!ts_sentinel) {
            failures++; printf("FAIL query nonzero: counterData.timestamp was written (no GPU clock contract)\n");
        } else {
            printf("INFO query nonzero: reset -> fullscreen draw -> report = %llu (timestamp untouched)\n",
                   (unsigned long long)qc);
        }

        // (2) 不再绘制、直接 report：累计值必须独立回来（不能等“下一次绘制/下一帧”）
        qn = build_query_stream(qb, &qtex, qprog, 0, 0, 0, 0, 0, 0x908);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x908, 8);
        if (rc || qc != 64) {
            failures++; printf("FAIL query report-without-draw: rc=%d value=%llu want 64 %s\n",
                              rc, (unsigned long long)qc, result.why);
        } else {
            printf("INFO query report-without-draw: %llu (completion advanced without a new draw)\n",
                   (unsigned long long)qc);
        }

        // (3) Report 不清零：再画一笔后 report = 128
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 0, 0, 0x910);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x910, 8);
        if (rc || qc != 128) {
            failures++; printf("FAIL query cumulative: rc=%d value=%llu want 128 %s\n",
                              rc, (unsigned long long)qc, result.why);
        } else {
            printf("INFO query cumulative: after another draw = %llu (Report does not clear)\n",
                   (unsigned long long)qc);
        }

        // (4) Reset 清零：reset -> report = 0（旧累计不能漏进新的一轮）
        qn = build_query_stream(qb, &qtex, qprog, 0, 0, 0, 1, 0, 0x918);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x918, 8);
        if (rc || qc != 0) {
            failures++; printf("FAIL query reset: rc=%d value=%llu want 0 %s\n",
                              rc, (unsigned long long)qc, result.why);
        } else {
            printf("INFO query reset: %llu (previous run cleared)\n", (unsigned long long)qc);
        }

        // (5) 零样本：reset -> count=0 的绘制 -> report = 0
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 0, 1, 0, 0x920);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x920, 8);
        if (rc || qc != 0) {
            failures++; printf("FAIL query zero: rc=%d value=%llu want 0 %s\n",
                              rc, (unsigned long long)qc, result.why);
        } else {
            printf("INFO query zero: %llu (empty draw)\n", (unsigned long long)qc);
        }

        // (6) 槽复用必须证明 Reset 已执行：64 -> reset -> 64 -> reset -> 0
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 1, 0, 0x928);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x928, 8);
        uint64_t qc2 = 0, qc3 = 1;
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 1, 0, 0x930);
        if (!rc) rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc2, guest.bytes + 0x930, 8);
        qn = build_query_stream(qb, &qtex, qprog, 0, 0, 0, 1, 0, 0x938);
        if (!rc) rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc3, guest.bytes + 0x938, 8);
        if (rc || qc != 64 || qc2 != 64 || qc3 != 0) {
            failures++; printf("FAIL query slot reuse: rc=%d values=%llu,%llu,%llu want 64,64,0 %s\n", rc,
                              (unsigned long long)qc, (unsigned long long)qc2, (unsigned long long)qc3, result.why);
        } else {
            printf("INFO query slot reuse: 64 -> reset -> 64 -> reset -> 0 (old value never leaks)\n");
        }

        // (7) 辅助清屏三角形不计数：reset -> 部分掩码清屏（mask 路径的全屏三角形）-> report = 0
        qn = build_query_stream(qb, &qtex, qprog, 1, 0, 0, 1, 1, 0x940);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x940, 8);
        if (rc || qc != 0) {
            failures++; printf("FAIL query auxiliary clear counted: rc=%d value=%llu want 0 %s\n",
                              rc, (unsigned long long)qc, result.why);
        } else {
            printf("INFO query auxiliary clear: %llu (helper triangle excluded from the game counter)\n",
                   (unsigned long long)qc);
        }

        // (8) 跨 pass 累计：reset -> draw（pass A）-> draw（pass B）-> report = 128
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 1, 0, 0);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 0, 0, 0x948);
        if (!rc) rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        memcpy(&qc, guest.bytes + 0x948, 8);
        if (rc || qc != 128) {
            failures++; printf("FAIL query cross-pass: rc=%d value=%llu want 128 %s\n",
                              rc, (unsigned long long)qc, result.why);
        } else {
            printf("INFO query cross-pass: %llu (two render passes summed)\n", (unsigned long long)qc);
        }

        // (9) 连续两次 Report：reset -> draw -> report=64 -> draw -> report=128
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 1, 0, 0x950);
        rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        qn = build_query_stream(qb, &qtex, qprog, 1, 1, 3, 0, 0, 0x958);
        if (!rc) rc = s2a_record_replay(&device, qb, qb + qn, &memory, qtx, 1, qdr, 1, NULL, 0, &result);
        uint64_t qa = 0, qb2 = 0;
        memcpy(&qa, guest.bytes + 0x950, 8);
        memcpy(&qb2, guest.bytes + 0x958, 8);
        if (rc || qa != 64 || qb2 != 128) {
            failures++; printf("FAIL query two reports: rc=%d values=%llu,%llu want 64,128 %s\n", rc,
                              (unsigned long long)qa, (unsigned long long)qb2, result.why);
        } else {
            printf("INFO query two reports: 64 then 128 (each report snapshots its own prefix)\n");
        }
    }

        vkDestroyFramebuffer(device.device, fb, NULL);
        vkDestroyImageView(device.device, view, NULL);
        vkDestroyPipeline(device.device, pipeline, NULL);
        vkDestroyRenderPass(device.device, pass, NULL);
        vkDestroyImage(device.device, draw_image, NULL);
        if (draw_memory) vkFreeMemory(device.device, draw_memory, NULL);
    } else {
        printf("INFO draw-record: SKIPPED (需要 shader 目录参数)\n");
    }

    // BIND_PROGRAM + DISPATCH：记录驱动 compute SSBO，读回 3i+1。
    if (argc >= 2) {
        VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory_dev = VK_NULL_HANDLE; void *mapped = NULL;
        VkBufferCreateInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = 64 * 4; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkCreateBuffer(device.device, &bi, NULL, &buffer);
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device.device, buffer, &req);
        VkMemoryAllocateInfo ai = {0};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size;
        ai.memoryTypeIndex = s2a_vk_memory_type(&device, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
        vkAllocateMemory(device.device, &ai, NULL, &memory_dev);
        vkBindBufferMemory(device.device, buffer, memory_dev, 0);
        vkMapMemory(device.device, memory_dev, 0, VK_WHOLE_SIZE, 0, &mapped);
        memset(mapped, 0, 64 * 4);
        char compute_path[512];
        snprintf(compute_path, sizeof compute_path, "%s/fill.comp.spv", argv[1]);
        VkPipeline cp = VK_NULL_HANDLE; VkPipelineLayout cl = VK_NULL_HANDLE; VkDescriptorSet cs = VK_NULL_HANDLE;
        VkDescriptorPool cp_pool = VK_NULL_HANDLE; VkDescriptorSetLayout cp_set_layout = VK_NULL_HANDLE;
        if (make_compute(&device, compute_path, buffer, &cp, &cl, &cs, &cp_pool, &cp_set_layout)) { failures++; printf("FAIL dispatch compute pipeline\n"); }
        else {
            uint8_t dispatch_stream[1024]; memset(dispatch_stream, 0, sizeof dispatch_stream); size_t dat = 0;
            uint32_t bp_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_bind_program_id) + 7) & ~7u);
            put(dispatch_stream, &dat, OP_BIND_PROGRAM, bp_size);
            p_bind_program_id bp; memset(&bp, 0, sizeof bp); bp.prog = (const prog_t *)(uintptr_t)0x6;
            memcpy(dispatch_stream + dat - bp_size + 8, &bp, sizeof bp);
            uint32_t dp_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_dispatch) + 7) & ~7u);
            put(dispatch_stream, &dat, OP_DISPATCH, dp_size);
            p_dispatch dp; memset(&dp, 0, sizeof dp); dp.x = 8; dp.y = 1; dp.z = 1;
            memcpy(dispatch_stream + dat - dp_size + 8, &dp, sizeof dp);
            put(dispatch_stream, &dat, OP_END, 8);
            struct s2a_replay_dispatch dispatch = {(const void *)(uintptr_t)0x6, cp, cl, cs};
            rc = s2a_record_replay(&device, dispatch_stream, dispatch_stream + dat, &memory,
                                   NULL, 0, NULL, 0, &dispatch, 1, &result);
            if (rc) { failures++; printf("FAIL dispatch replay: %d %s\n", rc, result.why); }
            else {
                VkBufferMemoryBarrier b = {0};
                b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.buffer = buffer; b.size = VK_WHOLE_SIZE;
                VkCommandBuffer barrier_cmd = VK_NULL_HANDLE;
                VkCommandBufferAllocateInfo cai = {0};
                cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cai.commandPool = device.command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
                vkAllocateCommandBuffers(device.device, &cai, &barrier_cmd);
                VkCommandBufferBeginInfo cbi = {0}; cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(barrier_cmd, &cbi);
                vkCmdPipelineBarrier(barrier_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &b, 0, NULL);
                vkEndCommandBuffer(barrier_cmd);
                VkFenceCreateInfo fi = {0}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                VkFence fence = VK_NULL_HANDLE; vkCreateFence(device.device, &fi, NULL, &fence);
                VkSubmitInfo si = {0}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1; si.pCommandBuffers = &barrier_cmd;
                vkQueueSubmit(device.queue, 1, &si, fence);
                vkWaitForFences(device.device, 1, &fence, VK_TRUE, UINT64_MAX);
                vkDestroyFence(device.device, fence, NULL);
                uint32_t *values = mapped; int bad = 0;
                for (uint32_t i = 0; i < 64; i++) if (values[i] != i * 3u + 1u) bad++;
                if (bad) { failures++; printf("FAIL dispatch values bad=%d\n", bad); }
                printf("INFO dispatch-record: first=%u last=%u bad=%d\n", values[0], values[63], bad);
            }
        }
        vkDestroyPipeline(device.device, cp, NULL);
        vkDestroyDescriptorPool(device.device, cp_pool, NULL);
        vkDestroyPipelineLayout(device.device, cl, NULL);
        vkDestroyDescriptorSetLayout(device.device, cp_set_layout, NULL);
        vkUnmapMemory(device.device, memory_dev);
        vkDestroyBuffer(device.device, buffer, NULL);
        vkFreeMemory(device.device, memory_dev, NULL);
    } else {
        printf("INFO dispatch-record: SKIPPED (需要 shader 目录参数)\n");
    }

    // CLEAR_TEX + COPY_T2T：记录驱动纹理拷贝。
    {
        VkDeviceMemory src_mem = VK_NULL_HANDLE, dst_mem = VK_NULL_HANDLE;
        VkImage src_img = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &src_mem);
        VkImage dst_img = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &dst_mem);
        if (!src_img || !dst_img) { failures++; printf("FAIL copy-t2t image creation\n"); }
        else {
            _Alignas(tex_t) uint8_t copy_stream[1024]; memset(copy_stream, 0, sizeof copy_stream); size_t cat = 0;
            tex_t src_snap = rgba8_snap(8, 8, 1, NVN_TEXTURE_TARGET_2D, 1), dst_snap = src_snap;
            src_snap.serial = 107; dst_snap.serial = 108;
            p_clear_tex clear; memset(&clear, 0, sizeof clear);
            clear.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
            clear.mask = 0xF; clear.value_type = CLEAR_TEX_FLOAT;
            clear.c.f[0] = 1.0f; clear.c.f[1] = 0.0f; clear.c.f[2] = 0.0f; clear.c.f[3] = 1.0f;
            put_clear_tex(copy_stream, &cat, &clear, &src_snap);
            p_copy_t2t copy; memset(&copy, 0, sizeof copy);
            copy.sr = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
            copy.dr = copy.sr;
            put_copy_t2t(copy_stream, &cat, &copy, &src_snap, &dst_snap);
            put(copy_stream, &cat, OP_END, 8);
            struct s2a_replay_texture textures[2] = {
                {&src_snap, src_img, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 107, 0, 0},
                {&dst_snap, dst_img, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 108, 0, 0},
            };
            rc = s2a_record_replay(&device, copy_stream, copy_stream + cat, &memory,
                                   textures, 2, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL copy-t2t replay: %d %s\n", rc, result.why); }
            else {
                uint8_t pixels[256];
                if (readback_image(&device, dst_img, VK_IMAGE_LAYOUT_GENERAL, pixels, sizeof pixels)) {
                    failures++; printf("FAIL copy-t2t readback\n");
                } else if (pixels[0] != 255 || pixels[1] != 0 || pixels[2] != 0 || pixels[3] != 255) {
                    failures++; printf("FAIL copy-t2t pixel %u,%u,%u,%u\n", pixels[0], pixels[1], pixels[2], pixels[3]);
                } else printf("INFO copy-t2t: pixel=%u,%u,%u,%u\n", pixels[0], pixels[1], pixels[2], pixels[3]);
            }
        }
        vkDestroyImage(device.device, src_img, NULL); if (src_mem) vkFreeMemory(device.device, src_mem, NULL);
        vkDestroyImage(device.device, dst_img, NULL); if (dst_mem) vkFreeMemory(device.device, dst_mem, NULL);
    }

    // CLEAR_TEX + COPY_T2B + COPY_B2T：纹理↔guest buffer。
    {
        VkDeviceMemory t_mem = VK_NULL_HANDLE;
        VkImage t_img = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &t_mem);
        if (!t_img) { failures++; printf("FAIL T2B image\n"); }
        else {
            _Alignas(tex_t) uint8_t stream[1024]; memset(stream, 0, sizeof stream); size_t at2 = 0;
            tex_t snap9 = rgba8_snap(8, 8, 1, NVN_TEXTURE_TARGET_2D, 1);
            snap9.serial = 109;
            p_clear_tex clear; memset(&clear, 0, sizeof clear);
            clear.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
            clear.mask = 0xF; clear.value_type = CLEAR_TEX_FLOAT;
            clear.c.f[0] = 1.0f; clear.c.f[1] = 0.0f; clear.c.f[2] = 0.0f; clear.c.f[3] = 1.0f;
            put_clear_tex(stream, &at2, &clear, &snap9);
            p_copy_b2t cp; memset(&cp, 0, sizeof cp);
            cp.src = 0x800;
            cp.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
            cp.row_stride = 64; cp.image_stride = 512;
            put_copy_b2t(stream, &at2, OP_COPY_T2B, &cp, &snap9);
            put(stream, &at2, OP_END, 8);
            struct s2a_replay_texture ttab[1] = {{&snap9, t_img, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 109, 0, 0}};
            rc = s2a_record_replay(&device, stream, stream + at2, &memory, ttab, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL T2B replay: %d %s\n", rc, result.why); }
            else {
                const uint8_t *g = guest.bytes + 0x800;
                if (!(g[0] == 255 && g[1] == 0 && g[2] == 0 && g[3] == 255)) {
                    failures++; printf("FAIL T2B guest red %u,%u,%u,%u\n", g[0], g[1], g[2], g[3]);
                }
                // 改 guest 为绿，B2T 回纹理。
                guest.bytes[0x800] = 0; guest.bytes[0x801] = 255;
                size_t at3 = 0; memset(stream, 0, sizeof stream);
                cp.src = 0x800;
                put_copy_b2t(stream, &at3, OP_COPY_B2T, &cp, &snap9);
                put(stream, &at3, OP_END, 8);
                rc = s2a_record_replay(&device, stream, stream + at3, &memory, ttab, 1, NULL, 0, NULL, 0, &result);
                uint8_t px[256];
                if (rc || readback_image(&device, t_img, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                    failures++; printf("FAIL B2T replay/readback rc=%d\n", rc);
                } else {
                    if (!(px[1] > 200 && px[0] < 40)) {
                        failures++; printf("FAIL B2T texture green %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
                    }
                    printf("INFO copy-b2t: pixel=%u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
                }
            }
        }
        vkDestroyImage(device.device, t_img, NULL); if (t_mem) vkFreeMemory(device.device, t_mem, NULL);
    }

    // 整数/带 stride 的 T2B：RGBA8I 清 -128,-1,0,127，row_stride 64、image_stride 512。
    {
        VkDeviceMemory im_mem = VK_NULL_HANDLE;
        VkImage im = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &im_mem);
        if (!im) { failures++; printf("FAIL int T2B image\n"); }
        else {
            _Alignas(tex_t) uint8_t stream[1024]; memset(stream, 0, sizeof stream); size_t at4 = 0;
            tex_t snapA = rgba8_snap(8, 8, 1, NVN_TEXTURE_TARGET_2D, 1);
            snapA.serial = 110;
            p_clear_tex clear; memset(&clear, 0, sizeof clear);
            clear.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
            clear.mask = 0xF; clear.value_type = CLEAR_TEX_FLOAT;
            clear.c.f[0] = 1.0f; clear.c.f[1] = 0.0f; clear.c.f[2] = 0.0f; clear.c.f[3] = 1.0f;
            put_clear_tex(stream, &at4, &clear, &snapA);
            p_copy_b2t cp; memset(&cp, 0, sizeof cp);
            cp.src = 0xC00;
            cp.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
            cp.row_stride = 64; cp.image_stride = 512;
            put_copy_b2t(stream, &at4, OP_COPY_T2B, &cp, &snapA);
            put(stream, &at4, OP_END, 8);
            struct s2a_replay_texture tab[1] = {{&snapA, im, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 110, 0, 0}};
            rc = s2a_record_replay(&device, stream, stream + at4, &memory, tab, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL int T2B replay: %d %s\n", rc, result.why); }
            else {
                const uint8_t *g = guest.bytes + 0xC00;
                if (!(g[0] == 255 && g[1] == 0 && g[2] == 0 && g[3] == 255)) {
                    failures++; printf("FAIL stride T2B pixel %u,%u,%u,%u\n", g[0], g[1], g[2], g[3]);
                }
                int pad = 0;
                for (int i = 32; i < 64; i++) if (guest.bytes[0xC00 + i] != 0) pad++;
                if (pad) { failures++; printf("FAIL stride padding=%d\n", pad); }
                printf("INFO copy-t2b-stride: pixel=%u,%u,%u,%u pad=%d\n", g[0], g[1], g[2], g[3], pad);
            }
        }
        vkDestroyImage(device.device, im, NULL); if (im_mem) vkFreeMemory(device.device, im_mem, NULL);
    }


    // 局部区域/通道掩码/有符号精度：RGBA8I 局部、RGBA8 通道掩码、RGBA32I 精度。
    {
        VkDeviceMemory p_mem = VK_NULL_HANDLE;
        VkImage p_img = make_image(&device, VK_FORMAT_R8G8B8A8_SINT, &p_mem);
        if (!p_img) { failures++; printf("FAIL partial image\n"); }
        else {
            _Alignas(tex_t) uint8_t stream[1024]; size_t at5 = 0; memset(stream, 0, sizeof stream);
            tex_t snap = rgba8_snap(8, 8, 1, NVN_TEXTURE_TARGET_2D, 1);
            snap.serial = 111; snap.d.format = NVN_FORMAT_RGBA8I;
            p_clear_tex c; memset(&c, 0, sizeof c);
            c.r = (NVNcopyRegion){0,0,0,8,8,1}; c.mask = 0xF; c.value_type = CLEAR_TEX_INT;
            c.c.i[0] = c.c.i[1] = c.c.i[2] = c.c.i[3] = 0;
            put_clear_tex(stream, &at5, &c, &snap);
            c.r = (NVNcopyRegion){2,2,0,4,4,1};
            c.c.i[0] = -128; c.c.i[1] = -1; c.c.i[2] = 0; c.c.i[3] = 127;
            put_clear_tex(stream, &at5, &c, &snap);
            put(stream, &at5, OP_END, 8);
            struct s2a_replay_texture tab[1] = {{&snap, p_img, VK_FORMAT_R8G8B8A8_SINT, 8, 8, 1, 111, 0, 0}};
            rc = s2a_record_replay(&device, stream, stream + at5, &memory, tab, 1, NULL, 0, NULL, 0, &result);
            uint8_t px[256];
            if (rc || readback_image(&device, p_img, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL partial replay/readback rc=%d\n", rc);
            } else {
                const int8_t *in = (const int8_t *)(px + (4 * 8 + 4) * 4);
                const int8_t *out = (const int8_t *)px;
                if (!(in[0] == -128 && in[1] == -1 && in[2] == 0 && in[3] == 127 &&
                      out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0)) {
                    failures++; printf("FAIL partial pixel in=%d,%d,%d,%d out=%d,%d,%d,%d\n",
                                       in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
                }
                printf("INFO clear-partial: in=%d,%d,%d,%d out=%d,%d,%d,%d\n",
                       in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
            }
        }
        vkDestroyImage(device.device, p_img, NULL); if (p_mem) vkFreeMemory(device.device, p_mem, NULL);
    }

    {
        VkDeviceMemory c_mem = VK_NULL_HANDLE;
        VkImage c_img = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &c_mem);
        if (!c_img) { failures++; printf("FAIL mask image\n"); }
        else {
            _Alignas(tex_t) uint8_t stream[1024]; size_t at6 = 0; memset(stream, 0, sizeof stream);
            tex_t snap = rgba8_snap(8, 8, 1, NVN_TEXTURE_TARGET_2D, 1);
            snap.serial = 112;
            p_clear_tex c; memset(&c, 0, sizeof c);
            c.r = (NVNcopyRegion){0,0,0,8,8,1}; c.mask = 0xF; c.value_type = CLEAR_TEX_FLOAT;
            c.c.f[0] = 1; c.c.f[1] = 0; c.c.f[2] = 0; c.c.f[3] = 1;
            put_clear_tex(stream, &at6, &c, &snap);
            c.mask = 0x2; c.c.f[1] = 1;
            put_clear_tex(stream, &at6, &c, &snap);
            put(stream, &at6, OP_END, 8);
            struct s2a_replay_texture tab[1] = {{&snap, c_img, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 112, 0, 0}};
            rc = s2a_record_replay(&device, stream, stream + at6, &memory, tab, 1, NULL, 0, NULL, 0, &result);
            uint8_t px[256];
            if (rc || readback_image(&device, c_img, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL mask replay/readback rc=%d\n", rc);
            } else if (!(px[0] == 255 && px[1] == 255 && px[2] == 0 && px[3] == 255)) {
                failures++; printf("FAIL mask pixel %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
            } else {
                printf("INFO clear-mask: pixel=%u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
            }
        }
        vkDestroyImage(device.device, c_img, NULL); if (c_mem) vkFreeMemory(device.device, c_mem, NULL);
    }


    {
        VkDeviceMemory q_mem = VK_NULL_HANDLE;
        VkImage q_img = make_image(&device, VK_FORMAT_R32G32B32A32_SINT, &q_mem);
        if (!q_img) { failures++; printf("FAIL int32 image\n"); }
        else {
            _Alignas(tex_t) uint8_t stream[1024]; size_t at7 = 0; memset(stream, 0, sizeof stream);
            tex_t snap = rgba8_snap(8, 8, 1, NVN_TEXTURE_TARGET_2D, 1);
            snap.serial = 113; snap.d.format = NVN_FORMAT_RGBA32I;
            p_clear_tex c; memset(&c, 0, sizeof c);
            c.r = (NVNcopyRegion){0,0,0,8,8,1}; c.mask = 0xF; c.value_type = CLEAR_TEX_INT;
            c.c.i[0] = 1; c.c.i[1] = 2; c.c.i[2] = 3; c.c.i[3] = 4;
            put_clear_tex(stream, &at7, &c, &snap);
            c.r = (NVNcopyRegion){2,2,0,4,4,1};
            c.c.i[0] = INT32_MIN; c.c.i[1] = INT32_MAX;
            c.c.i[2] = 16777217; c.c.i[3] = -16777217;
            put_clear_tex(stream, &at7, &c, &snap);
            put(stream, &at7, OP_END, 8);
            struct s2a_replay_texture tab[1] = {{&snap, q_img, VK_FORMAT_R32G32B32A32_SINT, 8, 8, 1, 113, 0, 0}};
            rc = s2a_record_replay(&device, stream, stream + at7, &memory, tab, 1, NULL, 0, NULL, 0, &result);
            uint8_t px[256 * 4];
            if (rc || readback_image(&device, q_img, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL int32 replay/readback rc=%d\n", rc);
            } else {
                const int32_t *in = (const int32_t *)(px + (size_t)(4 * 8 + 4) * 16);
                const int32_t *out = (const int32_t *)px;
                if (!(in[0] == INT32_MIN && in[1] == INT32_MAX && in[2] == 16777217 && in[3] == -16777217 &&
                      out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4)) {
                    failures++; printf("FAIL int32 pixel in=%d,%d,%d,%d out=%d,%d,%d,%d\n",
                                       in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
                }
                printf("INFO clear-partial-int32: in=%d,%d,%d,%d out=%d,%d,%d,%d\n",
                       in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
            }
        }
        vkDestroyImage(device.device, q_img, NULL); if (q_mem) vkFreeMemory(device.device, q_mem, NULL);
    }

    // ---- ClearDepthStencil：按命令位置立即清，深度/模板两个平面分别按掩码 ----
    // 旧实现把清除值/掩码押后成“下一笔 draw 的 loadOp”：清后没有 draw 会整个丢掉，
    // 清 A 之后再绑 B 会清到 B，而且用一个合并的 depth_cleared 同时决定 depth/stencil 的 loadOp。
    {
        VkDeviceMemory d0_mem = VK_NULL_HANDLE, d1_mem = VK_NULL_HANDLE;
        VkImage d0 = make_image_usage(&device, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, &d0_mem);
        VkImage d1 = make_image_usage(&device, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, &d1_mem);
        tex_t t0, t1;
        memset(&t0, 0, sizeof t0); memset(&t1, 0, sizeof t1);
        t0.magic = 0x00584554; t0.d.format = NVN_FORMAT_DEPTH32F; t0.d.width = t0.d.height = 8; t0.d.levels = 1; t0.serial = 71;
        t1.magic = 0x00584554; t1.d.format = NVN_FORMAT_DEPTH32F; t1.d.width = t1.d.height = 8; t1.d.levels = 1; t1.serial = 72;
        if (!d0 || !d1) { failures++; printf("FAIL depth image creation\n"); }
        else {
            struct s2a_replay_texture dtex[2] = {
                {&t0, d0, VK_FORMAT_D32_SFLOAT, 8, 8, 1, 71, 0, 0},
                {&t1, d1, VK_FORMAT_D32_SFLOAT, 8, 8, 1, 72, 0, 0},
            };
            uint8_t ds_stream[4096];
            size_t ds_at;
            float got[64];

            // 1) 清除之后没有任何 draw：仍必须真正清到目标上。
            memset(ds_stream, 0, sizeof ds_stream); ds_at = 0;
            g_break_sync_calls = 0;
            put_set_rt_depth(ds_stream, &ds_at, &t0);
            put_clear_ds(ds_stream, &ds_at, 0.25f, 1, 0, 0);
            put(ds_stream, &ds_at, OP_END, 8);
            rc = s2a_record_replay(&device, ds_stream, ds_stream + ds_at, &memory, dtex, 2, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL clear-ds(no draw) replay: %d %s\n", rc, result.why); }
            else if (readback_aspect(&device, d0, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, (uint8_t *)got, sizeof got)) {
                failures++; printf("FAIL clear-ds depth readback\n");
            } else if (fabsf(got[0] - 0.25f) > 0.001f) {
                failures++; printf("FAIL clear-ds(no draw) depth=%.4f want 0.25\n", got[0]);
            } else {
                printf("INFO clear-ds(no draw): depth=%.4f\n", got[0]);
            }
            if (g_break_sync_calls != 1) {
                failures++; printf("FAIL clear-ds requested %d sync pass breaks (want 1: clear submits GPU work)\n", g_break_sync_calls);
            }

            // 2) 清 A → 绑 B → 清 B：两条清除必须各自落在命令发生时的目标上。
            memset(ds_stream, 0, sizeof ds_stream); ds_at = 0;
            put_set_rt_depth(ds_stream, &ds_at, &t0);
            put_clear_ds(ds_stream, &ds_at, 0.5f, 1, 0, 0);
            put_set_rt_depth(ds_stream, &ds_at, &t1);
            put_clear_ds(ds_stream, &ds_at, 0.75f, 1, 0, 0);
            put(ds_stream, &ds_at, OP_END, 8);
            rc = s2a_record_replay(&device, ds_stream, ds_stream + ds_at, &memory, dtex, 2, NULL, 0, NULL, 0, &result);
            float got1[64];
            if (rc) { failures++; printf("FAIL clear-ds(retarget) replay: %d %s\n", rc, result.why); }
            else if (readback_aspect(&device, d0, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, (uint8_t *)got, sizeof got) ||
                     readback_aspect(&device, d1, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, (uint8_t *)got1, sizeof got1)) {
                failures++; printf("FAIL clear-ds(retarget) readback\n");
            } else if (fabsf(got[0] - 0.5f) > 0.001f || fabsf(got1[0] - 0.75f) > 0.001f) {
                failures++; printf("FAIL clear-ds(retarget) A=%.4f(want .5) B=%.4f(want .75)\n", got[0], got1[0]);
            } else {
                printf("INFO clear-ds(retarget): A=%.4f B=%.4f\n", got[0], got1[0]);
            }
        }
        vkDestroyImage(device.device, d0, NULL); if (d0_mem) vkFreeMemory(device.device, d0_mem, NULL);
        vkDestroyImage(device.device, d1, NULL); if (d1_mem) vkFreeMemory(device.device, d1_mem, NULL);
    }

    // 深度/模板双平面：只清深度必须保留模板，只清模板必须保留深度；部分模板掩码明确拒绝且不改图像。
    {
        VkDeviceMemory s_mem = VK_NULL_HANDLE;
        VkImage img = make_image_usage(&device, VK_FORMAT_D24_UNORM_S8_UINT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, &s_mem);
        tex_t t;
        memset(&t, 0, sizeof t);
        t.magic = 0x00584554; t.d.format = NVN_FORMAT_DEPTH24_STENCIL8; t.d.width = t.d.height = 8; t.d.levels = 1; t.serial = 73;
        if (!img) { failures++; printf("FAIL depth-stencil image creation\n"); }
        else {
            struct s2a_replay_texture tex[1] = { {&t, img, VK_FORMAT_D24_UNORM_S8_UINT, 8, 8, 1, 73, 0, 0} };
            uint8_t stream[4096];
            size_t at2;
            uint8_t depth_raw[256], stencil_raw[64];

            // 双平面整清：深度 0.5、模板 0x5A。
            memset(stream, 0, sizeof stream); at2 = 0;
            put_set_rt_depth(stream, &at2, &t);
            put_clear_ds(stream, &at2, 0.5f, 1, 0x5A, 0xFF);
            put(stream, &at2, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at2, &memory, tex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL clear-ds(both planes) replay: %d %s\n", rc, result.why); }

            // 只清深度：模板必须还是 0x5A。
            memset(stream, 0, sizeof stream); at2 = 0;
            put_set_rt_depth(stream, &at2, &t);
            put_clear_ds(stream, &at2, 0.25f, 1, 0, 0);
            put(stream, &at2, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at2, &memory, tex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL clear-ds(depth only) replay: %d %s\n", rc, result.why); }
            if (readback_aspect(&device, img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, depth_raw, sizeof depth_raw) ||
                readback_aspect(&device, img, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 8, 8, 1, stencil_raw, sizeof stencil_raw)) {
                failures++; printf("FAIL depth-only readback\n");
            } else {
                uint32_t raw = 0; memcpy(&raw, depth_raw, 4);
                double norm24 = (double)(raw & 0xFFFFFFu) / 16777215.0;
                double norm32 = raw > 0xFFFFFFu ? (double)(raw >> 8) / 16777215.0 : norm24;
                double best = fabs(norm24 - 0.25) < fabs(norm32 - 0.25) ? norm24 : norm32;
                if (fabs(best - 0.25) > 0.002) { failures++; printf("FAIL depth-only clear: depth=%.5f (raw=%08x) want 0.25\n", best, raw); }
                if (stencil_raw[0] != 0x5A) { failures++; printf("FAIL depth-only clear touched stencil: 0x%02x want 0x5A\n", stencil_raw[0]); }
                if (stencil_raw[0] == 0x5A) printf("INFO depth-only clear: depth=%.5f stencil=0x%02x (kept)\n", best, stencil_raw[0]);
            }

            // 只清模板：深度必须还是 0.25。
            memset(stream, 0, sizeof stream); at2 = 0;
            put_set_rt_depth(stream, &at2, &t);
            put_clear_ds(stream, &at2, 0.9f, 0, 0x33, 0xFF);
            put(stream, &at2, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at2, &memory, tex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL clear-ds(stencil only) replay: %d %s\n", rc, result.why); }
            if (readback_aspect(&device, img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, depth_raw, sizeof depth_raw) ||
                readback_aspect(&device, img, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 8, 8, 1, stencil_raw, sizeof stencil_raw)) {
                failures++; printf("FAIL stencil-only readback\n");
            } else {
                uint32_t raw = 0; memcpy(&raw, depth_raw, 4);
                double norm24 = (double)(raw & 0xFFFFFFu) / 16777215.0;
                double norm32 = raw > 0xFFFFFFu ? (double)(raw >> 8) / 16777215.0 : norm24;
                double best = fabs(norm24 - 0.25) < fabs(norm32 - 0.25) ? norm24 : norm32;
                if (fabs(best - 0.25) > 0.002) { failures++; printf("FAIL stencil-only clear touched depth: %.5f want 0.25\n", best); }
                if (stencil_raw[0] != 0x33) { failures++; printf("FAIL stencil-only clear: stencil=0x%02x want 0x33\n", stencil_raw[0]); }
                if (stencil_raw[0] == 0x33 && fabs(best - 0.25) <= 0.002) printf("INFO stencil-only clear: depth=%.5f (kept) stencil=0x%02x\n", best, stencil_raw[0]);
            }

            // 部分模板掩码（0x0F 只清低 4 位）：当前 stencil=0x33，写 0x7F 后应为 0x3F；深度不动。
            memset(stream, 0, sizeof stream); at2 = 0;
            put_set_rt_depth(stream, &at2, &t);
            put_clear_ds(stream, &at2, 0.1f, 0, 0x7F, 0x0F);
            put(stream, &at2, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at2, &memory, tex, 1, NULL, 0, NULL, 0, &result);
            if (rc) {
                failures++; printf("FAIL partial stencil mask rc=%d op=%u (%s)\n", rc, result.opcode, result.why);
            } else if (readback_aspect(&device, img, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 8, 8, 1, stencil_raw, sizeof stencil_raw) ||
                       readback_aspect(&device, img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, depth_raw, sizeof depth_raw)) {
                failures++; printf("FAIL partial stencil mask readback\n");
            } else {
                uint32_t raw = 0; memcpy(&raw, depth_raw, 4);
                double n24 = (double)(raw & 0xFFFFFFu) / 16777215.0;
                double n32 = raw > 0xFFFFFFu ? (double)(raw >> 8) / 16777215.0 : n24;
                double best = fabs(n24 - 0.25) < fabs(n32 - 0.25) ? n24 : n32;
                if (stencil_raw[0] != 0x3f) { failures++; printf("FAIL partial stencil mask: 0x%02x want 0x3F (mask 0x0F, value 0x7F)\n", stencil_raw[0]); }
                else if (fabs(best - 0.25) > 0.002) { failures++; printf("FAIL partial stencil mask touched depth: %.5f want 0.25\n", best); }
                else printf("INFO partial stencil mask: stencil=0x%02x depth=%.5f (kept)\n", stencil_raw[0], best);
            }

            // F2.1：模板起点 0xAA、清值 0x55；mask 0x01 → 0xAB，mask 0x0F → 0xA5，深度不动。
            {
                const uint32_t masks[2] = { 0x01, 0x0F };
                const uint8_t want[2] = { 0xAB, 0xA5 };
                for (int si = 0; si < 2; si++) {
                    memset(stream, 0, sizeof stream); at2 = 0;
                    put_set_rt_depth(stream, &at2, &t);
                    put_clear_ds(stream, &at2, 0.0f, 0, 0xAA, 0xFF);   // 模板整平面回 0xAA，深度不动
                    put_clear_ds(stream, &at2, 0.0f, 0, 0x55, masks[si]);
                    put(stream, &at2, OP_END, 8);
                    rc = s2a_record_replay(&device, stream, stream + at2, &memory, tex, 1, NULL, 0, NULL, 0, &result);
                    if (rc) { failures++; printf("FAIL stencil bit mask 0x%02x rc=%d %s\n", masks[si], rc, result.why); continue; }
                    if (readback_aspect(&device, img, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 8, 8, 1, stencil_raw, sizeof stencil_raw) ||
                        readback_aspect(&device, img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, depth_raw, sizeof depth_raw)) {
                        failures++; printf("FAIL stencil bit mask readback 0x%02x\n", masks[si]); continue;
                    }
                    uint32_t raw = 0; memcpy(&raw, depth_raw, 4);
                    double n24 = (double)(raw & 0xFFFFFFu) / 16777215.0;
                    double n32 = raw > 0xFFFFFFu ? (double)(raw >> 8) / 16777215.0 : n24;
                    double best = fabs(n24 - 0.25) < fabs(n32 - 0.25) ? n24 : n32;
                    if (stencil_raw[0] != want[si]) {
                        failures++; printf("FAIL stencil bit mask 0x%02x: got 0x%02x want 0x%02x\n",
                                           masks[si], stencil_raw[0], want[si]);
                    } else if (fabs(best - 0.25) > 0.002) {
                        failures++; printf("FAIL stencil bit mask 0x%02x touched depth: %.5f want 0.25\n", masks[si], best);
                    } else {
                        printf("INFO stencil bit mask 0x%02x: 0xAA + 0x55 -> 0x%02x, depth=%.5f (kept)\n",
                               masks[si], stencil_raw[0], best);
                    }
                }
            }
        }
        vkDestroyImage(device.device, img, NULL); if (s_mem) vkFreeMemory(device.device, s_mem, NULL);
    }

    // ---- scissor 区域清除：NVN 的 ClearColor 只清当前 scissor 内的像素 ----
    {
        VkDeviceMemory cmem = VK_NULL_HANDLE;
        VkImage cimg = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &cmem);
        tex_t ct;
        memset(&ct, 0, sizeof ct);
        ct.magic = 0x00584554; ct.d.format = NVN_FORMAT_RGBA8; ct.d.width = ct.d.height = 8; ct.d.levels = 1; ct.serial = 91;
        if (!cimg) { failures++; printf("FAIL scissor clear image creation\n"); }
        else {
            struct s2a_replay_texture ctex[1] = { {&ct, cimg, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 91, 0, 0} };
            uint8_t stream[4096];
            size_t at3;
            // 整幅清成 (0.2,0.4,0.6,1) = (51,102,153,255)
            memset(stream, 0, sizeof stream); at3 = 0;
            p_set_rt rt;
            memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &ct;
            put_set_rt(stream, &at3, &rt);
            uint32_t csz = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
            put(stream, &at3, OP_CLEAR_COLOR, csz);
            p_clear_color pc; memset(&pc, 0, sizeof pc); pc.index = 0; pc.mask = 0xF;
            pc.rgba[0] = 0.2f; pc.rgba[1] = 0.4f; pc.rgba[2] = 0.6f; pc.rgba[3] = 1.0f;
            memcpy(stream + at3 - csz + sizeof(op_hdr), &pc, sizeof pc);
            put(stream, &at3, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at3, &memory, ctex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL scissor clear(full) rc=%d %s\n", rc, result.why); }
            // 只清 scissor (2,2) 4x4 为红
            memset(stream, 0, sizeof stream); at3 = 0;
            put_set_rt(stream, &at3, &rt);
            put_scissor(stream, &at3, 2, 2, 4, 4);
            put(stream, &at3, OP_CLEAR_COLOR, csz);
            pc.rgba[0] = 1.0f; pc.rgba[1] = 0.0f; pc.rgba[2] = 0.0f; pc.rgba[3] = 1.0f;
            memcpy(stream + at3 - csz + sizeof(op_hdr), &pc, sizeof pc);
            put(stream, &at3, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at3, &memory, ctex, 1, NULL, 0, NULL, 0, &result);
            uint8_t px[256];
            if (rc) { failures++; printf("FAIL scissor clear(rect) rc=%d %s\n", rc, result.why); }
            else if (readback_image(&device, cimg, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) { failures++; printf("FAIL scissor clear readback\n"); }
            else {
                const uint8_t *inside = px + ((size_t)3 * 8 + 3) * 4;
                const uint8_t *outside = px + ((size_t)1 * 8 + 1) * 4;
                if (inside[0] != 255 || inside[1] != 0 || inside[2] != 0) { failures++; printf("FAIL scissor clear inside=%u,%u,%u want 255,0,0\n", inside[0], inside[1], inside[2]); }
                else if (outside[0] != 51 || outside[1] != 102 || outside[2] != 153) { failures++; printf("FAIL scissor clear touched outside=%u,%u,%u want 51,102,153\n", outside[0], outside[1], outside[2]); }
                else printf("INFO scissor clear: inside=255,0,0 outside=51,102,153 (preserved)\n");
            }
        }
        vkDestroyImage(device.device, cimg, NULL); if (cmem) vkFreeMemory(device.device, cmem, NULL);
    }

    // ---- F2.2 部分颜色通道：RGBA8 旧 (255,0,255,255)，清 (0,255,0,0) 仅 RG 掩码 → (0,255,255,255) ----
    {
        VkDeviceMemory fmem = VK_NULL_HANDLE;
        VkImage fimg = make_image(&device, VK_FORMAT_R8G8B8A8_UNORM, &fmem);
        tex_t ft;
        memset(&ft, 0, sizeof ft);
        ft.magic = 0x00584554; ft.d.format = NVN_FORMAT_RGBA8; ft.d.width = ft.d.height = 8;
        ft.d.levels = 1; ft.serial = 93;
        if (!fimg) { failures++; printf("FAIL colour-mask image creation\n"); }
        else {
            struct s2a_replay_texture ftex[1] = { {&ft, fimg, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 93, 0, 0} };
            uint8_t stream[4096], px[256];
            size_t at = 0;
            uint32_t clear_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
            // 整幅旧色。
            memset(stream, 0, sizeof stream); at = 0;
            p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &ft;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            p_clear_color cl; memset(&cl, 0, sizeof cl); cl.index = 0; cl.mask = 0xF;
            cl.rgba[0] = 1.0f; cl.rgba[1] = 0.0f; cl.rgba[2] = 1.0f; cl.rgba[3] = 1.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, ftex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL colour-mask seed rc=%d %s\n", rc, result.why); }

            // 部分 RG 掩码。
            memset(stream, 0, sizeof stream); at = 0;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            cl.mask = 0x3; cl.rgba[0] = 0.0f; cl.rgba[1] = 1.0f; cl.rgba[2] = 0.0f; cl.rgba[3] = 0.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, ftex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL partial colour mask rc=%d %s\n", rc, result.why); }
            else if (readback_image(&device, fimg, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL partial colour mask readback\n");
            } else if (memcmp(px, (const uint8_t[4]){0, 255, 255, 255}, 4)) {
                failures++; printf("FAIL partial colour mask: %u,%u,%u,%u want 0,255,255,255\n", px[0], px[1], px[2], px[3]);
            } else printf("INFO partial colour mask: RG written -> 0,255,255,255 (B,A preserved)\n");

            // 全掩码 / 零掩码：各自独立预期。
            memset(stream, 0, sizeof stream); at = 0;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            cl.mask = 0xF; cl.rgba[0] = 10.0f / 255.0f; cl.rgba[1] = 20.0f / 255.0f;
            cl.rgba[2] = 30.0f / 255.0f; cl.rgba[3] = 40.0f / 255.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, ftex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL full colour mask rc=%d %s\n", rc, result.why); }
            else if (readback_image(&device, fimg, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL full colour mask readback\n");
            } else if (memcmp(px, (const uint8_t[4]){10, 20, 30, 40}, 4)) {
                failures++; printf("FAIL full colour mask: %u,%u,%u,%u want 10,20,30,40\n", px[0], px[1], px[2], px[3]);
            } else printf("INFO full colour mask: 10,20,30,40\n");

            memset(stream, 0, sizeof stream); at = 0;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            cl.mask = 0x0; cl.rgba[0] = 1.0f; cl.rgba[1] = 2.0f / 255.0f; cl.rgba[2] = 3.0f / 255.0f; cl.rgba[3] = 4.0f / 255.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, ftex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL zero colour mask rc=%d %s\n", rc, result.why); }
            else if (readback_image(&device, fimg, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL zero colour mask readback\n");
            } else if (memcmp(px, (const uint8_t[4]){10, 20, 30, 40}, 4)) {
                failures++; printf("FAIL zero colour mask changed pixels: %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
            } else printf("INFO zero colour mask: unchanged 10,20,30,40\n");

            // 局部 scissor + 部分掩码：区域外保留，区域内只改 RG。
            memset(stream, 0, sizeof stream); at = 0;
            put_set_rt(stream, &at, &rt);
            put_scissor(stream, &at, 2, 2, 4, 4);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            cl.mask = 0x3; cl.rgba[0] = 1.0f; cl.rgba[1] = 0.0f; cl.rgba[2] = 0.0f; cl.rgba[3] = 0.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, ftex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL scissor partial colour mask rc=%d %s\n", rc, result.why); }
            else if (readback_image(&device, fimg, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
                failures++; printf("FAIL scissor partial colour mask readback\n");
            } else {
                const uint8_t *inside = px + ((size_t)3 * 8 + 3) * 4;
                const uint8_t *outside = px + ((size_t)1 * 8 + 1) * 4;
                if (inside[0] != 255 || inside[1] != 0 || inside[2] != 30 || inside[3] != 40)
                    { failures++; printf("FAIL scissor partial inside=%u,%u,%u,%u want 255,0,30,40\n", inside[0], inside[1], inside[2], inside[3]); }
                else if (memcmp(outside, (const uint8_t[4]){10, 20, 30, 40}, 4))
                    { failures++; printf("FAIL scissor partial outside=%u,%u,%u,%u want 10,20,30,40\n", outside[0], outside[1], outside[2], outside[3]); }
                else printf("INFO scissor partial colour mask: inside=255,0,30,40 outside preserved\n");
            }
        }
        vkDestroyImage(device.device, fimg, NULL); if (fmem) vkFreeMemory(device.device, fmem, NULL);
    }

    // ---- F2.3 附件格式覆盖：SRGB backing + UNORM 视图，清除值 0.5 必须按视图写成 128（不是 SRGB 的 188）----
    {
        VkImageCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_SRGB;
        ci.extent = (VkExtent3D){8, 8, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkCreateImage(device.device, &ci, NULL, &img) != VK_SUCCESS) { failures++; printf("FAIL override image creation\n"); }
        else {
            VkMemoryRequirements req; vkGetImageMemoryRequirements(device.device, img, &req);
            VkMemoryAllocateInfo ai = {0};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = s2a_vk_memory_type(&device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
            if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device.device, &ai, NULL, &mem) != VK_SUCCESS ||
                vkBindImageMemory(device.device, img, mem, 0) != VK_SUCCESS ||
                prepare_test_image(&device, img, VK_FORMAT_R8G8B8A8_SRGB, 1, 1) != 0) {
                failures++; printf("FAIL override image memory/layout\n");
            }
        }
        tex_t ot;
        memset(&ot, 0, sizeof ot);
        ot.magic = 0x00584554; ot.d.format = NVN_FORMAT_RGBA8_SRGB; ot.d.width = ot.d.height = 8;
        ot.d.levels = 1; ot.serial = 94;
        struct s2a_replay_texture otex[1] = { {&ot, img, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, 1, 94, 0, 0} };
        uint8_t stream[2048], px[256];
        size_t at = 0;
        uint32_t clear_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
        p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &ot;
        rt.has_cview[0] = 1; rt.cview[0].set_format = 1; rt.cview[0].format = NVN_FORMAT_RGBA8;
        put_set_rt(stream, &at, &rt);
        put(stream, &at, OP_CLEAR_COLOR, clear_size);
        p_clear_color cl; memset(&cl, 0, sizeof cl); cl.index = 0; cl.mask = 0xF;
        cl.rgba[0] = cl.rgba[1] = cl.rgba[2] = 0.5f; cl.rgba[3] = 1.0f;
        memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
        put(stream, &at, OP_END, 8);
        rc = s2a_record_replay(&device, stream, stream + at, &memory, otex, 1, NULL, 0, NULL, 0, &result);
        if (rc) { failures++; printf("FAIL override clear rc=%d %s\n", rc, result.why); }
        else if (readback_image(&device, img, VK_IMAGE_LAYOUT_GENERAL, px, sizeof px)) {
            failures++; printf("FAIL override clear readback\n");
        } else if (px[0] < 126 || px[0] > 130) {
            failures++; printf("FAIL override clear: %u,%u,%u,%u want ~128 (UNORM view), not 188 (base SRGB)\n",
                               px[0], px[1], px[2], px[3]);
        } else printf("INFO override clear via UNORM view: %u,%u,%u,%u (base SRGB would encode 0.5 to 188)\n",
                      px[0], px[1], px[2], px[3]);

        // 不同兼容类的覆盖必须准确拒绝。
        memset(stream, 0, sizeof stream); at = 0;
        memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &ot;
        rt.has_cview[0] = 1; rt.cview[0].set_format = 1; rt.cview[0].format = NVN_FORMAT_RGBA16F;
        put_set_rt(stream, &at, &rt);
        put(stream, &at, OP_CLEAR_COLOR, clear_size);
        memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
        put(stream, &at, OP_END, 8);
        rc = s2a_record_replay(&device, stream, stream + at, &memory, otex, 1, NULL, 0, NULL, 0, &result);
        if (rc != S2A_RECORD_UNSUPPORTED) { failures++; printf("FAIL wrong override class rc=%d (%s)\n", rc, result.why); }
        else printf("INFO wrong attachment override rejected: %s\n", result.why);
        if (img) vkDestroyImage(device.device, img, NULL); if (mem) vkFreeMemory(device.device, mem, NULL);
    }

    // ---- F1.3 颜色附件的视图子资源：SetRenderTargets 的 cview 选 mip1/layer1，其他 mip/层哨兵保留 ----
    {
        VkDeviceMemory vmem = VK_NULL_HANDLE;
        VkImage vimg = make_image_layers(&device, VK_FORMAT_R8G8B8A8_UNORM, 2, 2,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vmem);
        tex_t vt;
        memset(&vt, 0, sizeof vt);
        vt.magic = 0x00584554; vt.d.format = NVN_FORMAT_RGBA8; vt.d.width = vt.d.height = 8;
        vt.d.levels = 2; vt.d.target = 1; vt.serial = 95;
        if (!vimg) { failures++; printf("FAIL colour view image creation\n"); }
        else {
            struct s2a_replay_texture vtex[1] = { {&vt, vimg, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 95, 2, 2} };
            _Alignas(tex_t) uint8_t stream[4096];
            uint8_t p0[256], p1[64];
            size_t at = 0;
            uint32_t clear_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
            p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &vt;
            rt.has_cview[0] = 1;
            rt.cview[0].set_levels = 1; rt.cview[0].base_level = 0; rt.cview[0].nlevels = 1;
            rt.cview[0].set_layers = 1; rt.cview[0].min_layer = 0; rt.cview[0].nlayers = 1;
            p_clear_color cl; memset(&cl, 0, sizeof cl); cl.index = 0; cl.mask = 0xF;
            // mip0/layer0 哨兵绿。
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            cl.rgba[0] = 0.0f; cl.rgba[1] = 1.0f; cl.rgba[2] = 0.0f; cl.rgba[3] = 1.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, vtex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL colour view seed mip0/layer0 rc=%d %s\n", rc, result.why); }
            // mip1/layer1 哨兵蓝。
            memset(stream, 0, sizeof stream); at = 0;
            rt.cview[0].base_level = 1; rt.cview[0].min_layer = 1;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            cl.rgba[0] = 0.0f; cl.rgba[1] = 0.0f; cl.rgba[2] = 1.0f; cl.rgba[3] = 1.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, vtex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL colour view seed mip1/layer1 rc=%d %s\n", rc, result.why); }
            // 只把 mip1/layer1 清成红；mip0/layer0 必须保持绿。
            memset(stream, 0, sizeof stream); at = 0;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            cl.rgba[0] = 1.0f; cl.rgba[1] = 0.0f; cl.rgba[2] = 0.0f; cl.rgba[3] = 1.0f;
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, vtex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL colour view clear mip1/layer1 rc=%d %s\n", rc, result.why); }
            if (readback_aspect(&device, vimg, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 8, 8, 4, p0, sizeof p0) ||
                readback_aspect(&device, vimg, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 4, 4, 4, p1, sizeof p1)) {
                failures++; printf("FAIL colour view readback\n");
            } else if (memcmp(p0, (const uint8_t[4]){0, 255, 0, 255}, 4) ||
                       memcmp(p1, (const uint8_t[4]){255, 0, 0, 255}, 4)) {
                failures++; printf("FAIL colour view: mip0/layer0=%u,%u,%u,%u(want green) mip1/layer1=%u,%u,%u,%u(want red)\n",
                                   p0[0], p0[1], p0[2], p0[3], p1[0], p1[1], p1[2], p1[3]);
            } else {
                printf("INFO colour attachment view: mip1/layer1=red, mip0/layer0=green (sentinel kept)\n");
            }
        }
        vkDestroyImage(device.device, vimg, NULL); if (vmem) vkFreeMemory(device.device, vmem, NULL);
    }

    // ---- B2T / T2B 按视图子资源（#48）：视图 base_level 1 + region z 1 写进 mip1/layer1，带行尾填充；
    //      mip0/layer0 哨兵保留，T2B 读回同一子资源且不覆写 guest 行尾填充 ----
    {
        VkDeviceMemory vmem = VK_NULL_HANDLE;
        VkImage vimg = make_image_layers(&device, VK_FORMAT_R8G8B8A8_UNORM, 2, 2,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vmem);
        tex_t vt = rgba8_snap(8, 8, 2, NVN_TEXTURE_TARGET_2D_ARRAY, 2);
        vt.serial = 96;
        if (!vimg) { failures++; printf("FAIL b2t view image creation\n"); }
        else {
            struct s2a_replay_texture vtex[1] = { {&vt, vimg, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 96, 2, 2} };
            uint8_t stream[4096], p0[256], p1[64];
            size_t at = 0;
            // mip0/layer0 哨兵绿（同时把图像带进 GENERAL）。
            uint32_t clear_size = (uint32_t)((sizeof(op_hdr) + sizeof(p_clear_color) + 7) & ~7u);
            p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &vt; rt.has_cview[0] = 1;
            rt.cview[0].set_levels = 1; rt.cview[0].nlevels = 1; rt.cview[0].set_layers = 1; rt.cview[0].nlayers = 1;
            p_clear_color cl; memset(&cl, 0, sizeof cl); cl.index = 0; cl.mask = 0xF;
            cl.rgba[1] = 1.0f; cl.rgba[3] = 1.0f;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            // mip1/layer1 先清蓝：B2T 必须整块改写成 guest 的红。
            rt.cview[0].base_level = 1; rt.cview[0].min_layer = 1;
            cl.rgba[1] = 0.0f; cl.rgba[2] = 1.0f;
            put_set_rt(stream, &at, &rt);
            put(stream, &at, OP_CLEAR_COLOR, clear_size);
            memcpy(stream + at - clear_size + sizeof(op_hdr), &cl, sizeof cl);
            // guest 0xE00：4x4 红，行距 20（4 字节填充 0x5A）。
            for (int y = 0; y < 4; y++) for (int x = 0; x < 20; x++)
                guest.bytes[0xE00 + y * 20 + x] = x < 16 ? (uint8_t)((x & 3) == 0 || (x & 3) == 3 ? 255 : 0) : 0x5A;
            p_copy_b2t cp; memset(&cp, 0, sizeof cp);
            cp.src = 0xE00; cp.tex = &vt; cp.has_view = 1;
            cp.view.set_levels = 1; cp.view.base_level = 1; cp.view.nlevels = 0;   // 游戏写法：级数 0 = 到末级
            cp.r = (NVNcopyRegion){0, 0, 1, 4, 4, 1}; cp.row_stride = 20;
            put_copy_b2t(stream, &at, OP_COPY_B2T, &cp, &vt);
            // T2B 读回 mip1/layer1 到 0xF00，行距 20，填充预置 0x33 必须保留。
            memset(guest.bytes + 0xF00, 0x33, 80);
            cp.src = 0xF00;
            put_copy_b2t(stream, &at, OP_COPY_T2B, &cp, &vt);
            put(stream, &at, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at, &memory, vtex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL b2t view replay rc=%d %s\n", rc, result.why); }
            else if (readback_aspect(&device, vimg, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 8, 8, 4, p0, sizeof p0) ||
                     readback_aspect(&device, vimg, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 4, 4, 4, p1, sizeof p1)) {
                failures++; printf("FAIL b2t view readback\n");
            } else {
                int bad = memcmp(p0, (const uint8_t[4]){0, 255, 0, 255}, 4) != 0;
                for (int i = 0; i < 16; i++) bad |= memcmp(p1 + i * 4, (const uint8_t[4]){255, 0, 0, 255}, 4) != 0;
                int pad = 0, rows = 0;
                for (int y = 0; y < 4; y++) {
                    for (int x = 16; x < 20; x++) pad += guest.bytes[0xF00 + y * 20 + x] != 0x33;
                    rows += !memcmp(guest.bytes + 0xF00 + y * 20, guest.bytes + 0xE00 + y * 20, 16);
                }
                if (bad || pad || rows != 4) {
                    failures++; printf("FAIL b2t view: mip0/layer0=%u,%u,%u,%u mip1/layer1[0]=%u,%u,%u,%u pad=%d rows=%d\n",
                                       p0[0], p0[1], p0[2], p0[3], p1[0], p1[1], p1[2], p1[3], pad, rows);
                } else printf("INFO b2t/t2b view: mip1/layer1=red (16/16), mip0/layer0=green, t2b rows=4 pad kept\n");
            }
        }
        vkDestroyImage(device.device, vimg, NULL); if (vmem) vkFreeMemory(device.device, vmem, NULL);
    }

    // ---- 深度清除按视图子资源：清 mip1/layer1 不能动 mip0/layer0 ----
    {
        VkDeviceMemory vmem = VK_NULL_HANDLE;
        VkImage vimg = make_image_layers(&device, VK_FORMAT_D32_SFLOAT, 2, 2,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &vmem);
        tex_t vt;
        memset(&vt, 0, sizeof vt);
        vt.magic = 0x00584554; vt.d.format = NVN_FORMAT_DEPTH32F; vt.d.width = vt.d.height = 8;
        vt.d.levels = 2; vt.d.depth = 2; vt.serial = 92;
        if (!vimg) { failures++; printf("FAIL depth view image creation\n"); }
        else {
            struct s2a_replay_texture vtex[1] = { {&vt, vimg, VK_FORMAT_D32_SFLOAT, 8, 8, 2, 92, 2, 2} };
            uint8_t stream[4096];
            size_t at4;
            float f0[64], f1[64];
            // mip0/layer0 = 0.9
            memset(stream, 0, sizeof stream); at4 = 0;
            put_set_rt_depth(stream, &at4, &vt);
            put_clear_ds(stream, &at4, 0.9f, 1, 0, 0);
            put(stream, &at4, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at4, &memory, vtex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL depth view(base) rc=%d %s\n", rc, result.why); }
            // 视图只覆盖 mip1/layer1 = 0.25
            memset(stream, 0, sizeof stream); at4 = 0;
            put_set_rt_depth_view(stream, &at4, &vt, 1, 1, 1, 1);
            put_clear_ds(stream, &at4, 0.25f, 1, 0, 0);
            put(stream, &at4, OP_END, 8);
            rc = s2a_record_replay(&device, stream, stream + at4, &memory, vtex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL depth view(mip1/layer1) rc=%d %s\n", rc, result.why); }
            if (readback_aspect(&device, vimg, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, (uint8_t *)f0, sizeof f0) ||
                readback_aspect(&device, vimg, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 4, 4, 4, (uint8_t *)f1, sizeof f1)) {
                failures++; printf("FAIL depth view readback\n");
            } else if (fabsf(f0[0] - 0.9f) > 0.002f || fabsf(f1[0] - 0.25f) > 0.002f) {
                failures++; printf("FAIL depth view: mip0/layer0=%.4f(want .9) mip1/layer1=%.4f(want .25)\n", f0[0], f1[0]);
            } else {
                printf("INFO depth view clear: mip0/layer0=%.4f mip1/layer1=%.4f\n", f0[0], f1[0]);
            }
        }
        vkDestroyImage(device.device, vimg, NULL); if (vmem) vkFreeMemory(device.device, vmem, NULL);
    }

    // ---- F0.1 失败传播：sync 边界没有完成证明时，record 在该 opcode 位置准确失败，
    // 后续 clear/copy/fence/event 一个都不执行；成功路径仍按原指令序列执行。 ----
    {
        uint8_t fp_stream[2048];
        size_t fp_at = 0;
        // SET_RT(n=0)：只改 CPU 记录（sync=0）。
        p_set_rt none; memset(&none, 0, sizeof none);
        put_set_rt(fp_stream, &fp_at, &none);
        // CLEAR_DS：立即 GPU 工作，sync=1；测试桩在这里返回“没有完成证明”。
        put_clear_ds(fp_stream, &fp_at, 0.5f, 1, 0, 0);
        uint32_t ub_word = 0xCAFEBABEu;
        update_record(fp_stream, &fp_at, 0x10, &ub_word, sizeof ub_word);
        uint32_t src_word = 0x11223344u;
        memcpy(guest.bytes + 0x100, &src_word, sizeof src_word);
        copy_record(fp_stream, &fp_at, 0x100, 0x200, sizeof src_word);
        put(fp_stream, &fp_at, OP_FENCE_SYNC, 16);
        uint64_t sync_addr = 0x20;
        memcpy(fp_stream + fp_at - 8, &sync_addr, 8);
        put(fp_stream, &fp_at, OP_SIGNAL_EVENT, 24);
        uint64_t event_addr = 0x40;
        uint32_t event_mode = 0, event_value = 5;
        memcpy(fp_stream + fp_at - 16, &event_addr, 8);
        memcpy(fp_stream + fp_at - 8, &event_mode, 4);
        memcpy(fp_stream + fp_at - 4, &event_value, 4);
        put(fp_stream, &fp_at, OP_END, 8);

        memset(guest.bytes, 0, sizeof guest.bytes);
        g_break_sync_calls = 0;
        g_break_fail = 1;
        rc = s2a_record_replay(&device, fp_stream, fp_stream + fp_at, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        uint32_t ub_got = 0, copy_got = 0, fence_got = 0, event_got = 0;
        memcpy(&ub_got, guest.bytes + 0x10, 4);
        memcpy(&copy_got, guest.bytes + 0x200, 4);
        memcpy(&fence_got, guest.bytes + 0x24, 4);   // sync_t{magic,fenced,fence}
        memcpy(&event_got, guest.bytes + 0x40, 4);
        if (rc != S2A_RECORD_GPU || result.opcode != OP_COPY_B2B) {
            failures++; printf("FAIL pass-break failure rc=%d op=%u %s\n", rc, result.opcode, result.why);
        } else if (!strstr(result.why, "no completion proof") || !strstr(result.why, g_break_first_error)) {
            failures++; printf("FAIL pass-break failure lost the first error: %s\n", result.why);
        } else if (ub_got != ub_word || copy_got || fence_got || event_got) {
            // UPDATE_UB 在失败的 sync 边界（COPY_B2B）之前，必须已经执行；copy/fence/event
            // 在边界之后，一个都不能执行。
            failures++; printf("FAIL pass-break failure executed ops across the boundary: ub=%08x(want %08x) copy=%08x fence=%u event=%u\n",
                              ub_got, ub_word, copy_got, fence_got, event_got);
        } else if (g_break_sync_calls != 1) {
            failures++; printf("FAIL pass-break sync calls=%d (want 1)\n", g_break_sync_calls);
        } else {
            printf("INFO pass-break failure: op=%u sync=1 later-ops=0 why=%s\n", result.opcode, result.why);
        }

        // 同一命令流在成功边界下必须完整执行（证明失败不是 fixture 的默认答案）。
        memset(guest.bytes, 0, sizeof guest.bytes);
        memcpy(guest.bytes + 0x100, &src_word, sizeof src_word);
        g_break_sync_calls = 0;
        g_break_fail = 0;
        rc = s2a_record_replay(&device, fp_stream, fp_stream + fp_at, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        memcpy(&ub_got, guest.bytes + 0x10, 4);
        memcpy(&copy_got, guest.bytes + 0x200, 4);
        memcpy(&fence_got, guest.bytes + 0x24, 4);   // sync_t{magic,fenced,fence}
        memcpy(&event_got, guest.bytes + 0x40, 4);
        if (rc) {
            failures++; printf("FAIL pass-break success replay: %d %s\n", rc, result.why);
        } else if (ub_got != ub_word || copy_got != src_word || !fence_got || event_got != event_value) {
            failures++; printf("FAIL pass-break success sequence: ub=%08x copy=%08x fence=%u event=%u\n",
                              ub_got, copy_got, fence_got, event_got);
        } else {
            printf("INFO pass-break success: ub=%08x copy=%08x fence=%u event=%u sync-breaks=%d\n",
                   ub_got, copy_got, fence_got, event_got, g_break_sync_calls);
        }
        g_break_fail = 0;
    }

    // ---- F0.4 清屏 helper 的退休：提交成功但完成未知时命令缓冲/fence/framebuffer/view 全部保留，
    // 原清除准确失败；未提交失败则全部回收，目标内容不变。 ----
    {
        VkDeviceMemory cmem = VK_NULL_HANDLE;
        VkImage cimg = make_image_usage(&device, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, &cmem);
        tex_t ct;
        memset(&ct, 0, sizeof ct);
        ct.magic = 0x00584554; ct.d.format = NVN_FORMAT_DEPTH32F; ct.d.width = ct.d.height = 8;
        ct.d.levels = 1; ct.serial = 77;
        if (!cimg) { failures++; printf("FAIL clear-retire image creation\n"); }
        else {
            struct s2a_replay_texture ctex[1] = { {&ct, cimg, VK_FORMAT_D32_SFLOAT, 8, 8, 1, 77, 0, 0} };
            uint8_t cs[1024];
            size_t ca;
            float got[64] = {0};
            struct s2a_vk_clear_stats c0, c1;

            // 正常清除（基线 0.25）。
            memset(cs, 0, sizeof cs); ca = 0;
            put_set_rt_depth(cs, &ca, &ct);
            put_clear_ds(cs, &ca, 0.25f, 1, 0, 0);
            put(cs, &ca, OP_END, 8);
            rc = s2a_record_replay(&device, cs, cs + ca, &memory, ctex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL clear-retire base rc=%d %s\n", rc, result.why); }

            // 已提交但完成未知：不释放仍在 GPU 上的对象、不发布完成。
            memset(cs, 0, sizeof cs); ca = 0;
            put_set_rt_depth(cs, &ca, &ct);
            put_clear_ds(cs, &ca, 0.5f, 1, 0, 0);
            put(cs, &ca, OP_END, 8);
            s2a_vk_clear_stats(&c0);
            s2a_vk_clear_set_submit_fault(1);
            rc = s2a_record_replay(&device, cs, cs + ca, &memory, ctex, 1, NULL, 0, NULL, 0, &result);
            s2a_vk_clear_set_submit_fault(0);
            s2a_vk_clear_stats(&c1);
            if (!rc || result.opcode != OP_CLEAR_DS) {
                failures++; printf("FAIL clear-retire timeout should fail rc=%d op=%u %s\n", rc, result.opcode, result.why);
            } else if (c1.uncompleted - c0.uncompleted != 1 || c1.retained - c0.retained != 1 ||
                       c1.commands_freed != c0.commands_freed || c1.fences_freed != c0.fences_freed ||
                       c1.framebuffers_freed != c0.framebuffers_freed || c1.views_freed != c0.views_freed) {
                failures++; printf("FAIL clear-retire retention: uncompleted=%llu retained=%llu cmd_freed=%llu->%llu "
                                  "fb_freed=%llu->%llu view_freed=%llu->%llu\n",
                                  (unsigned long long)(c1.uncompleted - c0.uncompleted), (unsigned long long)c1.retained,
                                  (unsigned long long)c0.commands_freed, (unsigned long long)c1.commands_freed,
                                  (unsigned long long)c0.framebuffers_freed, (unsigned long long)c1.framebuffers_freed,
                                  (unsigned long long)c0.views_freed, (unsigned long long)c1.views_freed);
            } else {
                printf("INFO clear-retire timeout: retained cmd/fence/fb/view (uncompleted=%llu), failure returned\n",
                       (unsigned long long)(c1.uncompleted - c0.uncompleted));
            }

            // 未提交失败：对象全部回收，目标不被清除。
            memset(cs, 0, sizeof cs); ca = 0;
            put_set_rt_depth(cs, &ca, &ct);
            put_clear_ds(cs, &ca, 0.75f, 1, 0, 0);
            put(cs, &ca, OP_END, 8);
            s2a_vk_clear_stats(&c0);
            s2a_vk_clear_set_submit_fault(2);
            rc = s2a_record_replay(&device, cs, cs + ca, &memory, ctex, 1, NULL, 0, NULL, 0, &result);
            s2a_vk_clear_set_submit_fault(0);
            s2a_vk_clear_stats(&c1);
            if (!rc) {
                failures++; printf("FAIL clear-retire unsubmitted should fail\n");
            } else if (c1.unsubmitted - c0.unsubmitted != 1 || c1.retained != c0.retained ||
                       c1.commands_freed - c0.commands_freed != 1 || c1.fences_freed - c0.fences_freed != 1 ||
                       c1.framebuffers_freed - c0.framebuffers_freed != 1 || c1.views_freed - c0.views_freed != 1) {
                failures++; printf("FAIL clear-retire unsubmitted accounting: unsub=%llu retained=%u->%u cmd=%llu/%llu\n",
                                  (unsigned long long)(c1.unsubmitted - c0.unsubmitted), c0.retained, c1.retained,
                                  (unsigned long long)(c1.commands_created - c0.commands_created),
                                  (unsigned long long)(c1.commands_freed - c0.commands_freed));
            } else {
                printf("INFO clear-retire unsubmitted: cmd/fence/fb/view reclaimed, nothing retained\n");
            }

            // 之后正常清除仍然工作（账本不外溢到新操作）。
            memset(cs, 0, sizeof cs); ca = 0;
            put_set_rt_depth(cs, &ca, &ct);
            put_clear_ds(cs, &ca, 0.25f, 1, 0, 0);
            put(cs, &ca, OP_END, 8);
            rc = s2a_record_replay(&device, cs, cs + ca, &memory, ctex, 1, NULL, 0, NULL, 0, &result);
            if (rc) { failures++; printf("FAIL clear-retire recovery rc=%d %s\n", rc, result.why); }
            else if (readback_aspect(&device, cimg, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 8, 8, 4, (uint8_t *)got, sizeof got)) {
                failures++; printf("FAIL clear-retire depth readback\n");
            } else if (fabsf(got[0] - 0.25f) > 0.002f) {
                failures++; printf("FAIL clear-retire recovery depth=%.4f want 0.25\n", got[0]);
            } else {
                printf("INFO clear-retire recovery: depth=%.4f after injected failures\n", got[0]);
            }

            // 设备销毁前结算 clear 保留账本：真实提交的清除应能拿到完成证明并逐项回收。
            uint32_t cleft = 123;
            s2a_vk_clear_stats(&c0);
            int creclaimed = s2a_vk_clear_release_retained(&device, 5000000000ull, &cleft);
            s2a_vk_clear_stats(&c1);
            if (c0.retained == 0 || creclaimed != (int)c0.retained || cleft != 0 ||
                c1.commands_created != c1.commands_freed || c1.fences_created != c1.fences_freed ||
                c1.framebuffers_created != c1.framebuffers_freed || c1.views_created != c1.views_freed) {
                failures++; printf("FAIL clear settlement: retained=%u reclaimed=%d left=%u "
                                  "cmd=%llu/%llu fence=%llu/%llu fb=%llu/%llu view=%llu/%llu\n",
                                  c0.retained, creclaimed, cleft,
                                  (unsigned long long)c1.commands_created, (unsigned long long)c1.commands_freed,
                                  (unsigned long long)c1.fences_created, (unsigned long long)c1.fences_freed,
                                  (unsigned long long)c1.framebuffers_created, (unsigned long long)c1.framebuffers_freed,
                                  (unsigned long long)c1.views_created, (unsigned long long)c1.views_freed);
            } else {
                printf("INFO clear settlement: %d retained clear set(s) reclaimed explicitly; "
                       "cmd/fence/fb/view created==freed\n", creclaimed);
            }
        }
        vkDestroyImage(device.device, cimg, NULL); if (cmem) vkFreeMemory(device.device, cmem, NULL);
    }

    // ---- F0.3 一次性提交的三类结果与对象退休：正常完成恰好回收一次；提交失败不保留；
    // 提交成功但完成未知时 fence/命令缓冲/辅助 buffer-memory 全部保留且不写回 guest。 ----
    {
        struct s2a_record_gpu_stats s0, s1;
        uint8_t fs_stream[128];
        size_t fs_at = 0;
        uint32_t fs_value = 0x5A5A1234u;

        memset(guest.bytes, 0, sizeof guest.bytes);
        clear_record(fs_stream, &fs_at, 0x800, 16, fs_value);
        put(fs_stream, &fs_at, OP_END, 8);
        s2a_record_gpu_stats(&s0);
        rc = s2a_record_replay(&device, fs_stream, fs_stream + fs_at, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        s2a_record_gpu_stats(&s1);
        uint32_t filled = 0;
        memcpy(&filled, guest.bytes + 0x800, 4);
        if (rc || filled != fs_value) {
            failures++; printf("FAIL submit-classify normal run rc=%d filled=%08x %s\n", rc, filled, result.why);
        } else if (s1.cmd_freed - s0.cmd_freed != s1.cmd_created - s0.cmd_created ||
                   s1.fence_freed - s0.fence_freed != s1.fence_created - s0.fence_created ||
                   s1.buffer_freed - s0.buffer_freed != s1.buffer_created - s0.buffer_created ||
                   s1.memory_freed - s0.memory_freed != s1.memory_created - s0.memory_created) {
            failures++; printf("FAIL object accounting: cmd %llu/%llu fence %llu/%llu buf %llu/%llu mem %llu/%llu\n",
                              (unsigned long long)(s1.cmd_created - s0.cmd_created),
                              (unsigned long long)(s1.cmd_freed - s0.cmd_freed),
                              (unsigned long long)(s1.fence_created - s0.fence_created),
                              (unsigned long long)(s1.fence_freed - s0.fence_freed),
                              (unsigned long long)(s1.buffer_created - s0.buffer_created),
                              (unsigned long long)(s1.buffer_freed - s0.buffer_freed),
                              (unsigned long long)(s1.memory_created - s0.memory_created),
                              (unsigned long long)(s1.memory_freed - s0.memory_freed));
        } else {
            printf("INFO object accounting: cmd=%llu fence=%llu buf=%llu mem=%llu all reclaimed once\n",
                   (unsigned long long)(s1.cmd_created - s0.cmd_created),
                   (unsigned long long)(s1.fence_created - s0.fence_created),
                   (unsigned long long)(s1.buffer_created - s0.buffer_created),
                   (unsigned long long)(s1.memory_created - s0.memory_created));
        }

        // 真实提交后没有完成证明：对象保留、guest 不写、原操作准确失败。
        memset(guest.bytes, 0, sizeof guest.bytes);
        fs_at = 0;
        clear_record(fs_stream, &fs_at, 0x840, 16, fs_value);
        put(fs_stream, &fs_at, OP_END, 8);
        s2a_record_gpu_stats(&s0);
        s2a_record_set_submit_fault(1);
        rc = s2a_record_replay(&device, fs_stream, fs_stream + fs_at, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        s2a_record_set_submit_fault(0);
        s2a_record_gpu_stats(&s1);
        uint32_t unwritten = 0;
        memcpy(&unwritten, guest.bytes + 0x840, 4);
        if (!rc || result.opcode != OP_CLEAR_BUF) {
            failures++; printf("FAIL submit-incomplete should fail at clear-buffer rc=%d op=%u %s\n", rc, result.opcode, result.why);
        } else if (s1.submit_incomplete - s0.submit_incomplete != 1 ||
                   s1.retained_entries - s0.retained_entries != 1) {
            failures++; printf("FAIL submit-incomplete accounting: incomplete=%llu retained=%llu\n",
                              (unsigned long long)(s1.submit_incomplete - s0.submit_incomplete),
                              (unsigned long long)(s1.retained_entries - s0.retained_entries));
        } else if (s1.cmd_freed != s0.cmd_freed || s1.buffer_freed != s0.buffer_freed ||
                   s1.memory_freed != s0.memory_freed || s1.fence_freed != s0.fence_freed) {
            failures++; printf("FAIL submit-incomplete freed in-flight objects: cmd %llu->%llu fence %llu->%llu\n",
                              (unsigned long long)s0.cmd_freed, (unsigned long long)s1.cmd_freed,
                              (unsigned long long)s0.fence_freed, (unsigned long long)s1.fence_freed);
        } else if (unwritten != 0) {
            failures++; printf("FAIL submit-incomplete wrote guest memory %08x\n", unwritten);
        } else {
            printf("INFO submit-incomplete: retained fence/cmd/buffer/memory, guest untouched, op failed\n");
        }

        // 没有提交成功：命令从未执行，对象可立即回收（不能进保留账本）。
        memset(guest.bytes, 0, sizeof guest.bytes);
        fs_at = 0;
        clear_record(fs_stream, &fs_at, 0x880, 16, fs_value);
        put(fs_stream, &fs_at, OP_END, 8);
        s2a_record_gpu_stats(&s0);
        s2a_record_set_submit_fault(2);
        rc = s2a_record_replay(&device, fs_stream, fs_stream + fs_at, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        s2a_record_set_submit_fault(0);
        s2a_record_gpu_stats(&s1);
        memcpy(&unwritten, guest.bytes + 0x880, 4);
        if (!rc) {
            failures++; printf("FAIL submit-unsubmitted should fail\n");
        } else if (s1.submit_unsubmitted - s0.submit_unsubmitted != 1 ||
                   s1.retained_entries != s0.retained_entries ||
                   s1.cmd_freed - s0.cmd_freed != 1 ||
                   s1.buffer_freed - s0.buffer_freed != 1 ||
                   s1.fence_freed - s0.fence_freed != 1 ||
                   s1.memory_freed - s0.memory_freed != 1 || unwritten != 0) {
            failures++; printf("FAIL submit-unsubmitted accounting: unsub=%llu retained=%llu cmd=%llu buf=%llu fence=%llu mem=%llu guest=%08x\n",
                              (unsigned long long)(s1.submit_unsubmitted - s0.submit_unsubmitted),
                              (unsigned long long)(s1.retained_entries - s0.retained_entries),
                              (unsigned long long)(s1.cmd_freed - s0.cmd_freed),
                              (unsigned long long)(s1.buffer_freed - s0.buffer_freed),
                              (unsigned long long)(s1.fence_freed - s0.fence_freed),
                              (unsigned long long)(s1.memory_freed - s0.memory_freed), unwritten);
        } else {
            printf("INFO submit-unsubmitted: fence/cmd/buffer/memory reclaimed, nothing retained\n");
        }

        // 设备销毁前结算：真实提交后完成未知的条目此时应能拿到完成证明并显式回收。
        s2a_record_gpu_stats(&s0);
        uint32_t left = 123;
        s2a_record_gpu_stats(&s1);
        int reclaimed = s2a_record_release_retained(&device, 5000000000ull, &left);
        s2a_record_gpu_stats(&s1);
        if (s0.retained_entries == 0 || reclaimed != (int)s0.retained_entries || left != 0 ||
            s1.cmd_created != s1.cmd_freed || s1.fence_created != s1.fence_freed ||
            s1.buffer_created != s1.buffer_freed || s1.memory_created != s1.memory_freed) {
            failures++; printf("FAIL retained settlement: entries=%llu reclaimed=%d left=%u cmd=%llu/%llu fence=%llu/%llu buf=%llu/%llu mem=%llu/%llu\n",
                              (unsigned long long)s0.retained_entries, reclaimed, left,
                              (unsigned long long)s1.cmd_created, (unsigned long long)s1.cmd_freed,
                              (unsigned long long)s1.fence_created, (unsigned long long)s1.fence_freed,
                              (unsigned long long)s1.buffer_created, (unsigned long long)s1.buffer_freed,
                              (unsigned long long)s1.memory_created, (unsigned long long)s1.memory_freed);
        } else {
            printf("INFO retained settlement: %d entry set(s) completed and were reclaimed explicitly;\n"
                   "     all cmd/fence/buffer/memory created==freed\n", reclaimed);
        }
    }

    // #1218：TIMESTAMP_TOP（0x10）= 段顶时刻。与 Metal 端同语义：
    //   counter 域（+0）写 0（条件渲染靠它与未写过的邻槽比对）；
    //   timestamp 域（+8）写游戏时钟的 NVN 刻度（19.2 MHz），不拿 GPU 计数器冒充。
    // 连发两次断言单调；再和同一时刻的 guest_tick() 对位，守住“写在游戏时钟上”。
    {
        uint8_t ts[512];
        size_t tat = 0;
        for (int i = 0; i < 16; i++) guest.bytes[0x900 + i] = 0x5A;
        for (int i = 0; i < 16; i++) guest.bytes[0x920 + i] = 0x5A;
        put_counter(ts, &tat, OP_REPORT_COUNTER, 0x10 /* NVN_COUNTER_TYPE_TIMESTAMP_TOP */, 0x900);
        put_counter(ts, &tat, OP_REPORT_COUNTER, 0x10 /* NVN_COUNTER_TYPE_TIMESTAMP_TOP */, 0x920);
        put(ts, &tat, OP_END, 8);
        rc = s2a_record_replay(&device, ts, ts + tat, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        uint64_t c0 = 1, c1 = 1, s0 = 0, s1 = 0;
        memcpy(&c0, guest.bytes + 0x900, 8);
        memcpy(&s0, guest.bytes + 0x908, 8);
        memcpy(&c1, guest.bytes + 0x920, 8);
        memcpy(&s1, guest.bytes + 0x928, 8);
        // 两次时间戳之间只有一条命令缓冲提交+fence 等待；500 ms 足够覆盖调度/校准偏差，
        // 又能挡住“写的是 GPU 自带计数器、和游戏时钟差出任意偏移”的旧问题。
        const uint64_t bound = 500ull * 1000000ull * 12 / 625;
        uint64_t at = guest_tick();
        uint64_t d0 = s0 > at ? s0 - at : at - s0;
        uint64_t d1 = s1 > at ? s1 - at : at - s1;
        if (rc != 0 || c0 != 0 || c1 != 0 || s0 == 0 || s1 == 0) {
            failures++; printf("FAIL timestamp-top report: rc=%d op=%u counter=%llu/%llu stamp=%llu/%llu %s\n",
                              rc, result.opcode, (unsigned long long)c0, (unsigned long long)c1,
                              (unsigned long long)s0, (unsigned long long)s1, result.why);
        } else if (s1 < s0) {
            failures++; printf("FAIL timestamp-top monotonic: second=%llu < first=%llu\n",
                              (unsigned long long)s1, (unsigned long long)s0);
        } else if (d0 > bound || d1 > bound) {
            failures++; printf("FAIL timestamp-top game clock: stamp=%llu/%llu guest_tick=%llu "
                              "delta=%llu/%llu ticks (bound %llu)\n",
                              (unsigned long long)s0, (unsigned long long)s1, (unsigned long long)at,
                              (unsigned long long)d0, (unsigned long long)d1, (unsigned long long)bound);
        } else {
            printf("INFO timestamp-top: counter=0/0 timestamp=%llu/%llu nvn-ticks "
                   "(monotonic, guest_tick delta=%llu/%llu <= %llu)\n",
                   (unsigned long long)s0, (unsigned long long)s1, (unsigned long long)d0,
                   (unsigned long long)d1, (unsigned long long)bound);
        }
    }

    // 未实现的计数类型仍要准确失败、不写 guest：F3.1 的负例保留给真正没有实现的类型
    // （samples-passed 已经接了真实查询，不能再用它当负例）。
    {
        uint8_t qs[512];
        size_t qat = 0;
        for (int i = 0; i < 16; i++) guest.bytes[0x900 + i] = 0x5A;
        qat = 0;
        put_counter(qs, &qat, OP_RESET_COUNTER, 3 /* NVN_COUNTER_TYPE_INPUT_PRIMITIVES */, 0);
        put(qs, &qat, OP_END, 8);
        rc = s2a_record_replay(&device, qs, qs + qat, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        if (rc != S2A_RECORD_UNSUPPORTED || result.opcode != OP_RESET_COUNTER) {
            failures++; printf("FAIL unimplemented counter reset should fail accurately rc=%d op=%u %s\n",
                              rc, result.opcode, result.why);
        }
        qat = 0;
        put_counter(qs, &qat, OP_REPORT_COUNTER, 3, 0x900);
        clear_record(qs, &qat, 0x920, 16, 0xAABBCCDDu);
        put(qs, &qat, OP_END, 8);
        rc = s2a_record_replay(&device, qs, qs + qat, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
        int untouched = 1;
        for (int i = 0; i < 16; i++) if (guest.bytes[0x900 + i] != 0x5A) untouched = 0;
        uint32_t after_report = 0;
        memcpy(&after_report, guest.bytes + 0x920, 4);
        if (rc != S2A_RECORD_UNSUPPORTED || result.opcode != OP_REPORT_COUNTER || !untouched || after_report != 0) {
            failures++; printf("FAIL unimplemented counter report: rc=%d op=%u untouched=%d later=%08x %s\n",
                              rc, result.opcode, untouched, after_report, result.why);
        } else {
            printf("INFO unimplemented counter type: accurate failure, no guest write, later op not executed\n");
        }
    }

    // ---- GPU 等事件（NVN_EV_WAIT）不得被当成信号写值 ----
    // 主线把 nvnCommandBufferWaitEvent 录成 OP_SIGNAL_EVENT + 方式高位 NVN_EV_WAIT(0x80000000)
    // （graphics/nvn/gfx/nvn/nvn_mtl.h）。若照信号分支累加/替换，就会把 guest 事件值写成一个错的数：
    // 静默错值，不是报错。正控（方式 0）必须照写；负控（带 WAIT 位、替换/累加两种）必须原样不动。
    {
        uint8_t ev_stream[512];
        size_t ev_at;
        uint64_t ev_addr = 0x40;
        uint32_t seeded = 42, got = 0;
        struct { uint32_t mode, value, want; const char *what; } cases[] = {
            { 0u,          7u,  7u,  "signal (replace)" },          // 正控：照信号写
            { 1u,          5u, 47u,  "signal (accumulate)" },       // 正控：照信号累加（种子 42+5）
            { 0x80000000u, 99u, 42u, "wait (replace bit)" },        // 负控：等事件，值保持种子
            { 0x80000001u, 99u, 42u, "wait (accumulate bit)" },     // 负控：最容易被当信号累加的一档
        };
        for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            ev_at = 0;
            put(ev_stream, &ev_at, OP_SIGNAL_EVENT, 24);
            memcpy(ev_stream + ev_at - 16, &ev_addr, 8);
            memcpy(ev_stream + ev_at - 8, &cases[ci].mode, 4);
            memcpy(ev_stream + ev_at - 4, &cases[ci].value, 4);
            put(ev_stream, &ev_at, OP_END, 8);
            memcpy(guest.bytes + 0x40, &seeded, 4);
            rc = s2a_record_replay(&device, ev_stream, ev_stream + ev_at, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
            memcpy(&got, guest.bytes + 0x40, 4);
            if (rc != S2A_RECORD_OK || got != cases[ci].want) {
                failures++;
                printf("FAIL %s rc=%d guest value=%u(want %u)\n", cases[ci].what, rc, got, cases[ci].want);
            } else {
                printf("INFO %s: guest value %u -> %u\n", cases[ci].what, seeded, got);
            }
        }
    }

    // 未实现的 opcode 必须明确 Unsupported（用枚举外的新编号；OP_BIND_UBO 现在已经实现，
    // 不是负例；之前拿它当负例时实际被 payload 检查判成 INVALID）。
    uint8_t unsupported[8];
    op_hdr header = {OP_COUNT_ + 5, 8};
    memcpy(unsupported, &header, sizeof header);
    rc = s2a_record_replay(&device, unsupported, unsupported + sizeof unsupported, &memory, NULL, 0, NULL, 0, NULL, 0, &result);
    if (rc != S2A_RECORD_UNSUPPORTED || result.opcode != OP_COUNT_ + 5) {
        failures++; printf("FAIL unsupported handling rc=%d op=%u\n", rc, result.opcode);
    } else {
        printf("INFO unsupported: op=%u %s\n", result.opcode, result.why);
    }

    if (s2a_vk_device_wait_idle(&device) != VK_SUCCESS) {
        failures++; printf("FAIL device idle before query cleanup\n");
    } else {
        nvn_vk_query_release_device();
        s2a_vk_clear_fv_release_device(&device);
        s2a_vk_clear_fv_release_device(&device); // repeat must not destroy handles twice
    }
    s2a_vk_device_destroy(&device, 0);
    if (failures) { printf("vk-record: %d FAILURES\n", failures); return 1; }
    printf("vk-record: ok\n");
    return 0;
}
