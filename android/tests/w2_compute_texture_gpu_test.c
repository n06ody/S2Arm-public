// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 字节级 GPU 证据：真实纹理被 compute 采样后写进 SSBO，读回字节必须与纹理内容一致。
// 走的是生产构建器 nvn_vk_compute_pipeline_build（descriptor 布局/池/写入）与生产 record 的提交顺序；
// 只有 first_draw 的注册表解析/视图/采样器被替换成测试自己建的 VkImage/VkImageView/VkSampler，
// 因为那部分依赖 Android 会话与 NVN 前端。需要真实 Vulkan 设备（宿主 GPU 或设备），不是 device-free 门禁。
#include "nvn/nvn_vk_compute.h"
#include "nvn/nvn_vk_program_result.h"
#include "nvn/nvn_vk_texcache.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// glslc --target-env=vulkan1.1: layout(set=2,binding=3) uniform sampler2D; layout(set=1,binding=0) buffer{vec4}
static const uint32_t kSpirv[] = {
    0x07230203u, 0x00010300u, 0x000d000bu, 0x0000001du, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0005000fu, 0x00000005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060010u, 0x00000004u, 0x00000011u,
    0x00000001u, 0x00000001u, 0x00000001u, 0x00030047u, 0x00000008u, 0x00000002u, 0x00050048u, 0x00000008u,
    0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x0000000au, 0x00000021u, 0x00000000u, 0x00040047u,
    0x0000000au, 0x00000022u, 0x00000001u, 0x00040047u, 0x00000010u, 0x00000021u, 0x00000003u, 0x00040047u,
    0x00000010u, 0x00000022u, 0x00000002u, 0x00040047u, 0x0000001cu, 0x0000000bu, 0x00000019u, 0x00020013u,
    0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
    0x00000007u, 0x00000006u, 0x00000004u, 0x0003001eu, 0x00000008u, 0x00000007u, 0x00040020u, 0x00000009u,
    0x0000000cu, 0x00000008u, 0x0004003bu, 0x00000009u, 0x0000000au, 0x0000000cu, 0x00040015u, 0x0000000bu,
    0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000bu, 0x0000000cu, 0x00000000u, 0x00090019u, 0x0000000du,
    0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu,
    0x0000000eu, 0x0000000du, 0x00040020u, 0x0000000fu, 0x00000000u, 0x0000000eu, 0x0004003bu, 0x0000000fu,
    0x00000010u, 0x00000000u, 0x00040017u, 0x00000012u, 0x00000006u, 0x00000002u, 0x0004002bu, 0x00000006u,
    0x00000013u, 0x3f000000u, 0x0005002cu, 0x00000012u, 0x00000014u, 0x00000013u, 0x00000013u, 0x0004002bu,
    0x00000006u, 0x00000015u, 0x00000000u, 0x00040020u, 0x00000017u, 0x0000000cu, 0x00000007u, 0x00040015u,
    0x00000019u, 0x00000020u, 0x00000000u, 0x00040017u, 0x0000001au, 0x00000019u, 0x00000003u, 0x0004002bu,
    0x00000019u, 0x0000001bu, 0x00000001u, 0x0006002cu, 0x0000001au, 0x0000001cu, 0x0000001bu, 0x0000001bu,
    0x0000001bu, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x0004003du, 0x0000000eu, 0x00000011u, 0x00000010u, 0x00070058u, 0x00000007u, 0x00000016u, 0x00000011u,
    0x00000014u, 0x00000002u, 0x00000015u, 0x00050041u, 0x00000017u, 0x00000018u, 0x0000000au, 0x0000000cu,
    0x0003003eu, 0x00000018u, 0x00000016u, 0x000100fdu, 0x00010038u,
};

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

// ---- 生产代码的外部依赖（宿主替身） ----
// compute.c 的保活解除（真实实现与图形 kind==2 同源，需要 Android 会话/texcache）。
void s2a_vk_nvn_texture_release_for_compute(struct nvn_vk_tex *tex, VkImageView view) {
    (void)tex; (void)view;
}
// 本测试不走注册表解析（快照由测试直接构造），只需要符号在：解析入口不可用即明确失败。
int s2a_vk_nvn_texture_bind_for_compute(struct s2a_vk_device *d, uint64_t handle,
    struct nvn_vk_tex **tex_out, VkImageView *view_out, VkSampler *sampler_out, char *why, size_t cap) {
    (void)d; (void)handle; (void)tex_out; (void)view_out; (void)sampler_out;
    snprintf(why, cap, "host gpu test constructs the snapshot directly"); return -1;
}
// 宿主内存类型选择（生产实现按 s2a_vk_device 的能力表挑类型，这里等价地挑 HOST_VISIBLE|COHERENT）。
static VkPhysicalDeviceMemoryProperties g_memprops;
uint32_t s2a_vk_memory_type(const struct s2a_vk_device *d, uint32_t bits,
                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags *actual) {
    (void)d;
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & required) == required) {
            if (actual) *actual = g_memprops.memoryTypes[i].propertyFlags;
            return i;
        }
    return UINT32_MAX;
}

static uint64_t g_guest_addr;
static int g_reads, g_writes;
static int guest_read(void *opaque, uint64_t addr, void *out, size_t n) {
    (void)opaque;
    if (addr < g_guest_addr || addr + n > g_guest_addr + 64) return -1;
    memcpy(out, (const void *)(uintptr_t)addr, n); g_reads++; return 0;
}
static int guest_write(void *opaque, uint64_t addr, const void *in, size_t n) {
    (void)opaque;
    if (addr < g_guest_addr || addr + n > g_guest_addr + 64) return -1;
    memcpy((void *)(uintptr_t)addr, in, n); g_writes++; return 0;
}

// ---- 最小 Vulkan 环境 ----
static VkInstance g_inst; static VkPhysicalDevice g_phys; static VkDevice g_dev;
static VkQueue g_queue; static uint32_t g_family; static VkCommandPool g_pool;
static VkPhysicalDeviceProperties g_props;
static int vk_setup(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) return -1;
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(g_inst, &n, NULL) != VK_SUCCESS || !n) return -1;
    VkPhysicalDevice *phys = calloc(n, sizeof *phys);
    if (!phys) return -1;
    vkEnumeratePhysicalDevices(g_inst, &n, phys);
    int chosen = -1;
    for (uint32_t i = 0; i < n && chosen < 0; ++i) {
        uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, NULL);
        VkQueueFamilyProperties *q = calloc(qn ? qn : 1, sizeof *q);
        if (!q) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, q);
        for (uint32_t j = 0; j < qn; ++j) if (q[j].queueFlags & VK_QUEUE_COMPUTE_BIT) { chosen = (int)i; g_family = j; break; }
        free(q);
    }
    if (chosen < 0) { free(phys); return -1; }
    g_phys = phys[chosen];
    free(phys);
    vkGetPhysicalDeviceProperties(g_phys, &g_props);
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_family, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (vkCreateDevice(g_phys, &dci, NULL, &g_dev) != VK_SUCCESS) return -1;
    vkGetDeviceQueue(g_dev, g_family, 0, &g_queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_family };
    return vkCreateCommandPool(g_dev, &pci, NULL, &g_pool) == VK_SUCCESS ? 0 : -1;
}
static VkCommandBuffer cmd_begin(void) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_dev, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    return vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS ? cb : VK_NULL_HANDLE;
}
static int cmd_end_wait(VkCommandBuffer cb) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return -1;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(g_dev, &fci, NULL, &fence) != VK_SUCCESS) return -1;
    int rc = 0;
    if (vkQueueSubmit(g_queue, 1, &si, fence) != VK_SUCCESS ||
        vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) rc = -1;
    vkDestroyFence(g_dev, fence, NULL);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &cb);
    return rc;
}
// 2x2 RGBA8 纹理：4 个 texel 用同一个值（采样过滤方式不影响结果），上传后停在 GENERAL。
struct image {
    VkImage image; VkDeviceMemory mem; VkImageView view;
};
static int make_image(uint8_t r, uint8_t g, uint8_t b, uint8_t a, struct image *out) {
    memset(out, 0, sizeof *out);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 2, 2, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(g_dev, &ci, NULL, &out->image) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, out->image, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = s2a_vk_memory_type(NULL, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL) };
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = s2a_vk_memory_type(NULL, mr.memoryTypeBits, 0, NULL);
    if (vkAllocateMemory(g_dev, &ai, NULL, &out->mem) != VK_SUCCESS ||
        vkBindImageMemory(g_dev, out->image, out->mem, 0) != VK_SUCCESS) return -1;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(g_dev, &vci, NULL, &out->view) != VK_SUCCESS) return -1;
    // staging 上传 + 布局到 GENERAL
    uint8_t pixels[16]; for (int i = 0; i < 4; ++i) { pixels[i*4+0] = r; pixels[i*4+1] = g; pixels[i*4+2] = b; pixels[i*4+3] = a; }
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof pixels,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer staging = VK_NULL_HANDLE; VkDeviceMemory smem = VK_NULL_HANDLE; void *map = NULL;
    if (vkCreateBuffer(g_dev, &bci, NULL, &staging) != VK_SUCCESS) return -1;
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(g_dev, staging, &bmr);
    VkMemoryAllocateInfo smai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
        .memoryTypeIndex = s2a_vk_memory_type(NULL, bmr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL) };
    if (vkAllocateMemory(g_dev, &smai, NULL, &smem) != VK_SUCCESS ||
        vkBindBufferMemory(g_dev, staging, smem, 0) != VK_SUCCESS ||
        vkMapMemory(g_dev, smem, 0, sizeof pixels, 0, &map) != VK_SUCCESS) return -1;
    memcpy(map, pixels, sizeof pixels);
    VkCommandBuffer cb = cmd_begin();
    if (!cb) return -1;
    VkImageMemoryBarrier to_dst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = out->image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, NULL, 0, NULL, 1, &to_dst);
    VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { 2, 2, 1 } };
    vkCmdCopyBufferToImage(cb, staging, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier to_gen = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = out->image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        0, NULL, 0, NULL, 1, &to_gen);
    int rc = cmd_end_wait(cb);
    vkUnmapMemory(g_dev, smem);
    vkDestroyBuffer(g_dev, staging, NULL);
    vkFreeMemory(g_dev, smem, NULL);
    return rc;
}

// ---- 把 SPIR-V 塞进 s2a-direct-program-v1 的 compute 结果 JSON ----
static size_t b64(const uint8_t *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int have = 1;
        if (i + 1 < n) { v |= (uint32_t)in[i+1] << 8; have = 2; }
        if (i + 2 < n) { v |= in[i+2]; have = 3; }
        out[o++] = t[(v >> 18) & 63]; out[o++] = t[(v >> 12) & 63];
        out[o++] = have >= 2 ? t[(v >> 6) & 63] : '='; out[o++] = have >= 3 ? t[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}
static char *program_json(void) {
    static char spirv_b64[4096];
    b64((const uint8_t *)kSpirv, sizeof kSpirv, spirv_b64);
    static char json[8192];
    snprintf(json, sizeof json,
        "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
        "\"contract\":\"s2a-direct-program-content-v1\",\"kind\":\"compute\","
        "\"hasUnalignedStorageBuffer\":false,\"c0Reads\":[],"
        "\"stages\":[{\"stage\":\"cs\",\"spirv\":\"%s\",\"attributeReadMask\":0}],"
        "\"bindings\":["
        "{\"stage\":\"cs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,\"rw\":true,"
        "\"cbSlot\":0,\"cbOffset\":896,\"nvnSlot\":7,\"count\":1},"
        "{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":3,\"slot\":8,\"count\":1}],"
        "\"supportLayout\":{\"requiredSize\":2608},\"supportReads\":[]}}",
        spirv_b64);
    return json;
}

// 一次完整 dispatch：生产构建器 → 生产提交顺序 → 完成后 copyback → 返回 guest 字节。
static int run_dispatch(VkImageView view, VkSampler sampler, uint8_t *guest, size_t n,
                        char *why, size_t why_cap) {
    struct s2a_vk_device dev;
    memset(&dev, 0, sizeof dev);
    dev.device = g_dev; dev.physical = g_phys; dev.properties = g_props;
    struct s2a_record_memory memory = { .read = guest_read, .write = guest_write };
    struct s2a_record_dispatch_state state;
    memset(&state, 0, sizeof state);
    state.stages = 32;
    state.groups[0] = state.groups[1] = state.groups[2] = 1;
    state.ssbo_seen = 1u << 7;
    state.ssbo[7].addr = (uint64_t)(uintptr_t)guest;
    state.ssbo[7].size = n;
    struct nvn_vk_tex tex;
    memset(&tex, 0, sizeof tex);
    struct nvk_compute_tex_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.count = 1;
    snapshot.items[0] = (struct nvk_compute_tex_binding){
        .slot = 0, .handle = 5u | ((uint64_t)3 << 20), .tex_id = 5, .samp_id = 3,
        .tex = &tex, .view = view, .sampler = sampler };
    char *json = program_json();
    struct nvk_compute_lookup_result program;
    memset(&program, 0, sizeof program);
    program.bytes = (uint8_t *)json;
    program.length = strlen(json);
    program.profile.storage_alignment = g_props.limits.minStorageBufferOffsetAlignment;
    struct s2a_record_dispatch_output out;
    memset(&out, 0, sizeof out);
    int rc = nvn_vk_compute_pipeline_build(&dev, &state, &memory, &snapshot, NULL, &program, &out, why, why_cap);
    if (rc) return -1;
    CHECK(snapshot.count == 0, "successful build takes snapshot ownership");
    if (out.set_count != 3) { fprintf(stderr, "FAIL: expected 3 descriptor sets, got %u\n", out.set_count); return -1; }
    // 与记录层相同的提交顺序：仅一次 dispatch，完成后 SHADER_WRITE -> HOST_READ 屏障。
    VkCommandBuffer cb = cmd_begin();
    if (!cb) return -1;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, out.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, out.layout, 0, out.set_count, out.sets, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
        1, &bar, 0, NULL, 0, NULL);
    rc = cmd_end_wait(cb);
    int copyback = out.finish(&dev, out.owner, 0);
    if (rc || copyback) return -1;
    return 0;
}

static void dump(const char *label, const uint8_t *p, size_t n) {
    fprintf(stderr, "%s:", label);
    for (size_t i = 0; i < n; ++i) fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, "\n");
}

// 编译器静态分析给不出 supportReads（JSON null）时，运行时必须接受并把“读集未知”记进程序对象
// （消费端按恒等值填满 SupportBuffer），不能当成“没有读”把它静默变成空。
static void test_support_reads_unknown(void) {
    char *json = program_json();
    char *p = strstr(json, "\"supportReads\":[]");
    if (!p) { fprintf(stderr, "FAIL: fixture has no supportReads\n"); failures++; return; }
    static char buf[8192];
    size_t pre = (size_t)(p - json);
    memcpy(buf, json, pre);
    const char *null_reads = "\"supportReads\":null}}";
    memcpy(buf + pre, null_reads, strlen(null_reads) + 1);
    struct nvn_vk_compute_program prog;
    char why[256] = "";
    int rc = nvn_vk_program_parse_compute((const uint8_t *)buf, strlen(buf), &prog, why, sizeof why);
    CHECK(rc == 0, "supportReads=null is accepted as an unknown read set");
    CHECK(prog.support_reads_unknown == 1, "supportReads=null sets support_reads_unknown");
    CHECK(prog.support_read_count == 0, "supportReads=null leaves no proven ranges");
    if (rc == 0) nvn_vk_program_free_compute(&prog);
    // c0Reads=null 同理：运行时按完整驱动 c0 布局填（dispatch 尺寸/UBO 表/storage 表），
    // 每个值都来自本次 dispatch 的真实状态，不需要猜。
    {
        char *json2 = program_json();
        char *q = strstr(json2, "\"c0Reads\":[]");
        CHECK(q != NULL, "fixture has c0Reads");
        if (q) {
            // 只替换这一段、保留后面所有字段（c0Reads 在 JSON 中间，直接截断会把 stages/bindings 丢掉）。
            static char buf2[8192];
            size_t pre2 = (size_t)(q - json2);
            const char *null_c0 = "\"c0Reads\":null";
            const char *tail = q + strlen("\"c0Reads\":[]");
            memcpy(buf2, json2, pre2);
            memcpy(buf2 + pre2, null_c0, strlen(null_c0));
            memcpy(buf2 + pre2 + strlen(null_c0), tail, strlen(tail) + 1);
            struct nvn_vk_compute_program prog2;
            char why2[256] = "";
            int rc2 = nvn_vk_program_parse_compute((const uint8_t *)buf2, strlen(buf2), &prog2, why2, sizeof why2);
            CHECK(rc2 == 0, "c0Reads=null is accepted as an unknown read set");
            CHECK(prog2.c0_reads_unknown == 1, "c0Reads=null sets c0_reads_unknown");
            if (rc2 == 0) nvn_vk_program_free_compute(&prog2);
        }
    }
}

int main(void) {
    test_support_reads_unknown();
    if (vk_setup()) { fprintf(stderr, "FAIL: host Vulkan device unavailable\n"); return 2; }
    fprintf(stderr, "device: %s api=%u.%u.%u minStorageBufferOffsetAlignment=%llu\n", g_props.deviceName,
        VK_VERSION_MAJOR(g_props.apiVersion), VK_VERSION_MINOR(g_props.apiVersion), VK_VERSION_PATCH(g_props.apiVersion),
        (unsigned long long)g_props.limits.minStorageBufferOffsetAlignment);
    struct image white, black;
    CHECK(make_image(0xff, 0xff, 0xff, 0xff, &white) == 0, "white texture upload");
    CHECK(make_image(0x00, 0x00, 0x00, 0x00, &black) == 0, "black texture upload");
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler sampler = VK_NULL_HANDLE;
    CHECK(vkCreateSampler(g_dev, &sci, NULL, &sampler) == VK_SUCCESS, "sampler create");

    // 生产按设备 minStorageBufferOffsetAlignment 拒非对齐 guest 缓冲：栈数组不保证 16B 对齐，按设备限值分配。
    uint8_t *guest = NULL;
    size_t guest_align = (size_t)g_props.limits.minStorageBufferOffsetAlignment;
    if (guest_align < sizeof(void *)) guest_align = sizeof(void *);
    CHECK(posix_memalign((void **)&guest, guest_align, 64) == 0, "guest alloc");
    char why[256] = "";

    // 1) 白色纹理 → SSBO 必须是 4 个 float 1.0（0x3f800000），字节级核对。
    memset(guest, 0xa5, 64);
    g_guest_addr = (uint64_t)(uintptr_t)guest;
    CHECK(run_dispatch(white.view, sampler, guest, 16, why, sizeof why) == 0, why[0] ? why : "white dispatch");
    dump("white -> ssbo", guest, 16);
    static const uint8_t white_expected[16] = {
        0x00,0x00,0x80,0x3f, 0x00,0x00,0x80,0x3f, 0x00,0x00,0x80,0x3f, 0x00,0x00,0x80,0x3f };
    CHECK(!memcmp(guest, white_expected, 16), "白色纹理采样后 SSBO 字节 = 1.0f x4");
    CHECK(g_reads == 1 && g_writes == 1, "guest read/copyback exactly once");

    // 2) 同一条管线换成黑色纹理 → 字节必须变成 0（证明描述符真的指向被采样的那张纹理）。
    memset(guest, 0xa5, 64);
    g_reads = g_writes = 0;
    CHECK(run_dispatch(black.view, sampler, guest, 16, why, sizeof why) == 0, why[0] ? why : "black dispatch");
    dump("black -> ssbo", guest, 16);
    static const uint8_t black_expected[16] = { 0 };
    CHECK(!memcmp(guest, black_expected, 16), "黑色纹理采样后 SSBO 字节 = 0 x4");

    // 3) 反例：不绑定任何纹理的描述符集合（空快照 + 反射要求纹理）→ 必须明确失败，不绑占位。
    {
        struct s2a_vk_device dev; memset(&dev, 0, sizeof dev);
        dev.device = g_dev; dev.physical = g_phys; dev.properties = g_props;
        struct s2a_record_memory memory = { .read = guest_read, .write = guest_write };
        struct s2a_record_dispatch_state state; memset(&state, 0, sizeof state);
        state.stages = 32; state.groups[0] = 1;
        state.ssbo_seen = 1u << 7; state.ssbo[7].addr = g_guest_addr; state.ssbo[7].size = 16;
        struct nvk_compute_tex_snapshot empty; memset(&empty, 0, sizeof empty);
        char *json = program_json();
        struct nvk_compute_lookup_result program; memset(&program, 0, sizeof program);
        program.bytes = (uint8_t *)json; program.length = strlen(json);
        program.profile.storage_alignment = g_props.limits.minStorageBufferOffsetAlignment;
        struct s2a_record_dispatch_output out; memset(&out, 0, sizeof out);
        char neg_why[256] = "";
        int rc = nvn_vk_compute_pipeline_build(&dev, &state, &memory, &empty, NULL, &program, &out, neg_why, sizeof neg_why);
        CHECK(rc != 0 && strstr(neg_why, "is not in the dispatch snapshot") != NULL, "未绑定的反射纹理必须明确失败");
        CHECK(!out.owner && !out.pipeline, "失败不留 owner/pipeline");
        fprintf(stderr, "negative: %s\n", neg_why);
    }

    vkDestroySampler(g_dev, sampler, NULL);
    for (int i = 0; i < 2; ++i) {
        struct image *im = i ? &black : &white;
        vkDestroyImageView(g_dev, im->view, NULL);
        vkDestroyImage(g_dev, im->image, NULL);
        vkFreeMemory(g_dev, im->mem, NULL);
    }
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    free(guest);
    struct s2a_vk_device dev_rel; memset(&dev_rel, 0, sizeof dev_rel); dev_rel.device = g_dev;
    nvn_vk_compute_pso_cache_release(&dev_rel);   // 表内可能留存对象，同 session 序先放表再毁设备
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_inst, NULL);
    if (failures) { fprintf(stderr, "FAILED: %d checks\n", failures); return 1; }
    printf("PASS: compute 组合纹理采样字节正确（白=1.0f/黑=0.0f），未绑定纹理明确失败\n");
    return 0;
}
