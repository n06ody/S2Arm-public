// 设备侧真实能力/limits（Vulkan physical device 实测值）→ 紧凑 JSON（s2a-device-caps-v1）。
// 编译请求里的 enabled profile 必须由此合成（编译器侧只提供 layout/ABI 哈希），不能填默认值。
// 只从"创建成功的 device"记录取 enabled 事实：features 用 s2a_vk_device.core_features（已按实际
// 启用字段裁剪），format support 用 vkGetPhysicalDeviceFormatProperties 实测，extensions 只列
// 本后端真的启用的设备扩展。
#include <stdio.h>
#include <string.h>
#include "device_caps.h"

#define PUT(...) do { \
    int n = snprintf(out + used, used < cap ? cap - used : 0, __VA_ARGS__); \
    if (n < 0) return -1; used += (size_t)n; if (used >= cap) return -1; } while (0)

// driver/device 名进 JSON：只保留可打印 ASCII，引号/反斜杠转义；非 ASCII 写成 '?'，
// 让 managed 的严格解析永远拿到合法 JSON（不因某个驱动的名字带引号而整份失败）。
static int put_text(char *out, size_t cap, size_t *used, const char *text) {
    if (*used + 2 > cap) return -1;
    out[(*used)++] = '"';
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            if (*used + 2 > cap) return -1;
            out[(*used)++] = '\\';
            out[(*used)++] = (char)c;
        } else {
            if (*used + 1 > cap) return -1;
            out[(*used)++] = (c < 0x20 || c >= 0x7f) ? '?' : (char)c;
        }
    }
    if (*used + 2 > cap) return -1;
    out[(*used)++] = '"';
    out[*used] = 0;
    return 0;
}

static const char *js_bool(int value) { return value ? "true" : "false"; }

// Ryujinx Vulkan 后端判定压缩格式用的同一组 feature 位：能采样、能线性过滤、能 blit、能传。
static int optimal_all(const struct s2a_vk_device *d, const VkFormat *formats, size_t count) {
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < count; i++) {
        VkFormatProperties p;
        memset(&p, 0, sizeof p);
        vkGetPhysicalDeviceFormatProperties(d->physical, formats[i], &p);
        if ((p.optimalTilingFeatures & need) != need) return 0;
    }
    return 1;
}

static int vertex_format_ok(const struct s2a_vk_device *d, VkFormat format) {
    VkFormatProperties p;
    memset(&p, 0, sizeof p);
    vkGetPhysicalDeviceFormatProperties(d->physical, format, &p);
    return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

// 与 Ryujinx FormatCapabilities.SupportsScaledVertexFormats 同口径：缩放格式不支持、但整数变体
// 支持时才算缺口（两者都不支持的格式本来就用不到，跳过）。
struct scaled_pair { VkFormat scaled, integer; };
static const struct scaled_pair k_scaled_pairs[] = {
    { VK_FORMAT_R8_USCALED, VK_FORMAT_R8_UINT },
    { VK_FORMAT_R8_SSCALED, VK_FORMAT_R8_SINT },
    { VK_FORMAT_R16_USCALED, VK_FORMAT_R16_UINT },
    { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_SINT },
    { VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_UINT },
    { VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8_SINT },
    { VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_UINT },
    { VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_SINT },
    { VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_UINT },
    { VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8_SINT },
    { VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_UINT },
    { VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16_SINT },
    { VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_UINT },
    { VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R8G8B8A8_SINT },
    { VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_UINT },
    { VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SINT },
    { VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32 },
    { VK_FORMAT_A2B10G10R10_SSCALED_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32 },
};

static int scaled_vertex_ok(const struct s2a_vk_device *d) {
    for (size_t i = 0; i < sizeof k_scaled_pairs / sizeof k_scaled_pairs[0]; i++) {
        if (!vertex_format_ok(d, k_scaled_pairs[i].scaled) &&
            vertex_format_ok(d, k_scaled_pairs[i].integer)) return 0;
    }
    return 1;
}

static const VkFormat k_bc123[] = {
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
    VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK,
    VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
};
static const VkFormat k_bc45[] = {
    VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK,
    VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
};
static const VkFormat k_bc67[] = {
    VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK,
    VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
};
static const VkFormat k_etc2[] = {
    VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
    VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
    VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
};
static const VkFormat k_astc[] = {
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
    VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_SRGB_BLOCK,
    VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
    VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_SRGB_BLOCK,
    VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
    VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_SRGB_BLOCK,
    VK_FORMAT_ASTC_12x12_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
};
static const VkFormat k_five_bit[] = {
    VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16,
    VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_FORMAT_B5G5R5A1_UNORM_PACK16,
    VK_FORMAT_A1R5G5B5_UNORM_PACK16,
};
static const VkFormat k_r4g4b4a4[] = { VK_FORMAT_A4B4G4R4_UNORM_PACK16 };

int s2a_vk_device_caps_json(const struct s2a_vk_device *d, char *out, size_t cap, size_t *len) {
    if (!d || !out || cap < 1024) return -1;
    size_t used = 0;
    const VkPhysicalDeviceProperties *p = &d->properties;
    const VkPhysicalDeviceLimits *l = &p->limits;
    const VkPhysicalDeviceFeatures *f = &d->core_features;
    VkPhysicalDeviceSubgroupProperties sg;
    memset(&sg, 0, sizeof sg);
    sg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof p2);
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(d->physical, &p2);

    uint64_t device_local = 0;
    for (uint32_t i = 0; i < d->memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        if (d->memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            device_local += (uint64_t)d->memory.memoryHeaps[i].size;
    int device_local_host_visible = 0;
    for (uint32_t i = 0; i < d->memory.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++) {
        const VkMemoryPropertyFlags flags = d->memory.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            device_local_host_visible = 1;
    }

    PUT("{\"schema\":\"s2a-device-caps-v1\",\"apiVersion\":\"%u.%u.%u\",\"driverVersion\":%u,"
        "\"vendorId\":%u,\"deviceId\":%u,\"deviceType\":%u,",
        VK_VERSION_MAJOR(p->apiVersion), VK_VERSION_MINOR(p->apiVersion), VK_VERSION_PATCH(p->apiVersion),
        p->driverVersion, p->vendorID, p->deviceID, (unsigned)p->deviceType);
    PUT("\"deviceName\":");
    if (put_text(out, cap, &used, p->deviceName)) return -1;
    PUT(",\"driverName\":");
    if (put_text(out, cap, &used, d->driver_name)) return -1;

    PUT(",\"limits\":{\"maxImageDimension2D\":%u,\"maxImageDimensionCube\":%u,\"maxImageArrayLayers\":%u,"
        "\"maxUniformBufferRange\":%u,\"maxStorageBufferRange\":%u,\"maxComputeSharedMemorySize\":%u,"
        "\"maxPerStageDescriptorSamplers\":%u,\"maxPerStageDescriptorSampledImages\":%u,"
        "\"maxPerStageDescriptorUniformBuffers\":%u,\"maxPerStageDescriptorStorageBuffers\":%u,"
        "\"maxPerStageDescriptorStorageImages\":%u,"
        "\"minUniformBufferOffsetAlignment\":%llu,\"minStorageBufferOffsetAlignment\":%llu,"
        "\"minTexelBufferOffsetAlignment\":%llu,\"subgroupSize\":%u,\"maxComputeWorkGroupInvocations\":%u,"
        "\"maxVertexOutputComponents\":%u,\"maxFragmentInputComponents\":%u,"
        "\"maxSamplerAnisotropy\":%.6f},",
        l->maxImageDimension2D, l->maxImageDimensionCube, l->maxImageArrayLayers,
        l->maxUniformBufferRange, l->maxStorageBufferRange, l->maxComputeSharedMemorySize,
        l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorSampledImages,
        l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers,
        l->maxPerStageDescriptorStorageImages,
        (unsigned long long)l->minUniformBufferOffsetAlignment, (unsigned long long)l->minStorageBufferOffsetAlignment,
        (unsigned long long)l->minTexelBufferOffsetAlignment, sg.subgroupSize, l->maxComputeWorkGroupInvocations,
        l->maxVertexOutputComponents, l->maxFragmentInputComponents,
        (double)l->maxSamplerAnisotropy);

    // geometryShaderOcclusionQuery 即通用侧 SupportsGeometryShaderOcclusionQuery 同一位：
    // vk_device.c 建设备时已由 s2a_vk_gs_path（capprobe 判定 + debug.s2a.gs_path
    // 双门强制开关）定 enabled_features.geometryShader，此处如实上报 enabled 事实。
    PUT("\"features\":{\"geometryShader\":%s,\"geometryShaderOcclusionQuery\":%s,"
        "\"tessellationShader\":%s,\"imageCubeArray\":%s,"
        "\"shaderFloat64\":%s,\"shaderInt64\":%s,\"fragmentStoresAndAtomics\":%s,"
        "\"vertexPipelineStoresAndAtomics\":%s,\"textureCompressionASTC_LDR\":%s,"
        "\"textureCompressionBC\":%s,\"textureCompressionETC2\":%s,\"shaderStorageImageExtendedFormats\":%s,"
        "\"independentBlend\":%s,\"depthClamp\":%s,\"multiViewport\":%s,\"samplerAnisotropy\":%s,"
        "\"shaderImageGatherExtended\":%s,\"shaderStorageImageReadWithoutFormat\":%s},",
        js_bool(f->geometryShader),
        js_bool(f->geometryShader),
        js_bool(f->tessellationShader), js_bool(f->imageCubeArray),
        js_bool(f->shaderFloat64), js_bool(f->shaderInt64),
        js_bool(f->fragmentStoresAndAtomics), js_bool(f->vertexPipelineStoresAndAtomics),
        js_bool(f->textureCompressionASTC_LDR), js_bool(f->textureCompressionBC),
        js_bool(f->textureCompressionETC2), js_bool(f->shaderStorageImageExtendedFormats),
        js_bool(f->independentBlend), js_bool(f->depthClamp), js_bool(f->multiViewport),
        js_bool(f->samplerAnisotropy), js_bool(f->shaderImageGatherExtended),
        js_bool(f->shaderStorageImageReadWithoutFormat));

    PUT("\"formats\":{\"bc123\":%s,\"bc45\":%s,\"bc67\":%s,\"etc2\":%s,\"astc\":%s,"
        "\"fiveBit\":%s,\"r4g4b4a4\":%s,\"scaledVertex\":%s},",
        js_bool(optimal_all(d, k_bc123, sizeof k_bc123 / sizeof k_bc123[0])),
        js_bool(optimal_all(d, k_bc45, sizeof k_bc45 / sizeof k_bc45[0])),
        js_bool(optimal_all(d, k_bc67, sizeof k_bc67 / sizeof k_bc67[0])),
        js_bool(optimal_all(d, k_etc2, sizeof k_etc2 / sizeof k_etc2[0])),
        js_bool(optimal_all(d, k_astc, sizeof k_astc / sizeof k_astc[0])),
        js_bool(optimal_all(d, k_five_bit, sizeof k_five_bit / sizeof k_five_bit[0])),
        js_bool(optimal_all(d, k_r4g4b4a4, sizeof k_r4g4b4a4 / sizeof k_r4g4b4a4[0])),
        js_bool(scaled_vertex_ok(d)));

    PUT("\"memory\":{\"deviceLocalBytes\":%llu,\"heapCount\":%u,\"memoryTypeCount\":%u,"
        "\"deviceLocalHostVisible\":%s},",
        (unsigned long long)device_local, d->memory.memoryHeapCount, d->memory.memoryTypeCount,
        js_bool(device_local_host_visible));

    PUT("\"enabledExtensions\":[");
    int first = 1;
    if (d->has_swapchain) {
        if (put_text(out, cap, &used, "VK_KHR_swapchain")) return -1;
        first = 0;
    }
    if (d->swapchain_maintenance1_enabled) {
        if (!first) PUT(",");
        if (put_text(out, cap, &used, "VK_EXT_swapchain_maintenance1")) return -1;
    }
    PUT("],\"subgroupSize\":%u}", sg.subgroupSize);
    if (len) *len = used;
    return 0;
}
