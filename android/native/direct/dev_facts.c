// 设备事实与设备档案（#1749 M0），见 dev_facts.h。只查询、只记录：不建设备、不改任何启用项、不参与任何决策。
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   // clock_gettime
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE           // 宿主测试：macOS 上 _SC_NPROCESSORS_CONF 不在纯 POSIX 名字空间
#endif
#include "dev_facts.h"
#include "dev_policy.h"
#include "vk_device.h"
#include "sha256.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <android/log.h>
#define S2A_FLOG(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#else
#define S2A_FLOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

// NDK 29 自带的头（275 版）还没有 VK_EXT_map_memory_placed：结构与类型号照 Vulkan 头原文补在本地（同 vk_device.c）
#ifndef VK_EXT_map_memory_placed
typedef struct { VkStructureType sType; void *pNext; VkBool32 memoryMapPlaced, memoryMapRangePlaced, memoryUnmapReserve; } VkPhysicalDeviceMapMemoryPlacedFeaturesEXT;
typedef struct { VkStructureType sType; void *pNext; VkDeviceSize minPlacedMemoryMapAlignment; } VkPhysicalDeviceMapMemoryPlacedPropertiesEXT;
#define S2A_FSTYPE_PLACED_FEATURES ((VkStructureType)1000272000)
#define S2A_FSTYPE_PLACED_PROPERTIES ((VkStructureType)1000272001)
#else
#define S2A_FSTYPE_PLACED_FEATURES VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT
#define S2A_FSTYPE_PLACED_PROPERTIES VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT
#endif

// ---------------- 表 ----------------
// NVN 用到的格式：nvn_vk_format.c 的映射目标（含深度替换与 16 位打包的退回格式）∪ vk_device.c / device_caps.c 建设备时查的格式。
//   A1B5G5R5（VK_KHR_maintenance5）不在表里：vk_device.c 一律按不可用处理、从不查询。新映射 / 新查询漏进表，
//   android/tests/caps_shape 的覆盖核对当场红（生产映射逐个过一遍、假驱动记下建设备时的每次格式查询）。按名字字典序。
#define S2A_FACT_FORMATS(V) \
    V(VK_FORMAT_A1R5G5B5_UNORM_PACK16) \
    V(VK_FORMAT_A2B10G10R10_SINT_PACK32) \
    V(VK_FORMAT_A2B10G10R10_SNORM_PACK32) \
    V(VK_FORMAT_A2B10G10R10_SSCALED_PACK32) \
    V(VK_FORMAT_A2B10G10R10_UINT_PACK32) \
    V(VK_FORMAT_A2B10G10R10_UNORM_PACK32) \
    V(VK_FORMAT_A2B10G10R10_USCALED_PACK32) \
    V(VK_FORMAT_A4B4G4R4_UNORM_PACK16) \
    V(VK_FORMAT_A8B8G8R8_SRGB_PACK32) \
    V(VK_FORMAT_A8B8G8R8_UNORM_PACK32) \
    V(VK_FORMAT_ASTC_10x10_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_10x10_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_10x5_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_10x5_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_10x6_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_10x6_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_10x8_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_10x8_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_12x10_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_12x10_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_12x12_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_12x12_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_4x4_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_4x4_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_5x4_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_5x4_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_5x5_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_5x5_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_6x5_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_6x5_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_6x6_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_6x6_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_8x5_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_8x5_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_8x6_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_8x6_UNORM_BLOCK) \
    V(VK_FORMAT_ASTC_8x8_SRGB_BLOCK) \
    V(VK_FORMAT_ASTC_8x8_UNORM_BLOCK) \
    V(VK_FORMAT_B10G11R11_UFLOAT_PACK32) \
    V(VK_FORMAT_B4G4R4A4_UNORM_PACK16) \
    V(VK_FORMAT_B5G5R5A1_UNORM_PACK16) \
    V(VK_FORMAT_B5G6R5_UNORM_PACK16) \
    V(VK_FORMAT_B8G8R8A8_SRGB) \
    V(VK_FORMAT_B8G8R8A8_UNORM) \
    V(VK_FORMAT_BC1_RGBA_SRGB_BLOCK) \
    V(VK_FORMAT_BC1_RGBA_UNORM_BLOCK) \
    V(VK_FORMAT_BC2_SRGB_BLOCK) \
    V(VK_FORMAT_BC2_UNORM_BLOCK) \
    V(VK_FORMAT_BC3_SRGB_BLOCK) \
    V(VK_FORMAT_BC3_UNORM_BLOCK) \
    V(VK_FORMAT_BC4_SNORM_BLOCK) \
    V(VK_FORMAT_BC4_UNORM_BLOCK) \
    V(VK_FORMAT_BC5_SNORM_BLOCK) \
    V(VK_FORMAT_BC5_UNORM_BLOCK) \
    V(VK_FORMAT_BC6H_SFLOAT_BLOCK) \
    V(VK_FORMAT_BC6H_UFLOAT_BLOCK) \
    V(VK_FORMAT_BC7_SRGB_BLOCK) \
    V(VK_FORMAT_BC7_UNORM_BLOCK) \
    V(VK_FORMAT_D16_UNORM) \
    V(VK_FORMAT_D16_UNORM_S8_UINT) \
    V(VK_FORMAT_D24_UNORM_S8_UINT) \
    V(VK_FORMAT_D32_SFLOAT) \
    V(VK_FORMAT_D32_SFLOAT_S8_UINT) \
    V(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) \
    V(VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK) \
    V(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK) \
    V(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK) \
    V(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK) \
    V(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK) \
    V(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) \
    V(VK_FORMAT_R16G16B16A16_SFLOAT) \
    V(VK_FORMAT_R16G16B16A16_SINT) \
    V(VK_FORMAT_R16G16B16A16_SNORM) \
    V(VK_FORMAT_R16G16B16A16_SSCALED) \
    V(VK_FORMAT_R16G16B16A16_UINT) \
    V(VK_FORMAT_R16G16B16A16_UNORM) \
    V(VK_FORMAT_R16G16B16A16_USCALED) \
    V(VK_FORMAT_R16G16B16_SFLOAT) \
    V(VK_FORMAT_R16G16B16_SINT) \
    V(VK_FORMAT_R16G16B16_SNORM) \
    V(VK_FORMAT_R16G16B16_SSCALED) \
    V(VK_FORMAT_R16G16B16_UINT) \
    V(VK_FORMAT_R16G16B16_UNORM) \
    V(VK_FORMAT_R16G16B16_USCALED) \
    V(VK_FORMAT_R16G16_SFLOAT) \
    V(VK_FORMAT_R16G16_SINT) \
    V(VK_FORMAT_R16G16_SNORM) \
    V(VK_FORMAT_R16G16_SSCALED) \
    V(VK_FORMAT_R16G16_UINT) \
    V(VK_FORMAT_R16G16_UNORM) \
    V(VK_FORMAT_R16G16_USCALED) \
    V(VK_FORMAT_R16_SFLOAT) \
    V(VK_FORMAT_R16_SINT) \
    V(VK_FORMAT_R16_SNORM) \
    V(VK_FORMAT_R16_SSCALED) \
    V(VK_FORMAT_R16_UINT) \
    V(VK_FORMAT_R16_UNORM) \
    V(VK_FORMAT_R16_USCALED) \
    V(VK_FORMAT_R32G32B32A32_SFLOAT) \
    V(VK_FORMAT_R32G32B32A32_SINT) \
    V(VK_FORMAT_R32G32B32A32_UINT) \
    V(VK_FORMAT_R32G32B32_SFLOAT) \
    V(VK_FORMAT_R32G32B32_SINT) \
    V(VK_FORMAT_R32G32B32_UINT) \
    V(VK_FORMAT_R32G32_SFLOAT) \
    V(VK_FORMAT_R32G32_SINT) \
    V(VK_FORMAT_R32G32_UINT) \
    V(VK_FORMAT_R32_SFLOAT) \
    V(VK_FORMAT_R32_SINT) \
    V(VK_FORMAT_R32_UINT) \
    V(VK_FORMAT_R5G5B5A1_UNORM_PACK16) \
    V(VK_FORMAT_R5G6B5_UNORM_PACK16) \
    V(VK_FORMAT_R8G8B8A8_SINT) \
    V(VK_FORMAT_R8G8B8A8_SNORM) \
    V(VK_FORMAT_R8G8B8A8_SRGB) \
    V(VK_FORMAT_R8G8B8A8_SSCALED) \
    V(VK_FORMAT_R8G8B8A8_UINT) \
    V(VK_FORMAT_R8G8B8A8_UNORM) \
    V(VK_FORMAT_R8G8B8A8_USCALED) \
    V(VK_FORMAT_R8G8B8_SINT) \
    V(VK_FORMAT_R8G8B8_SNORM) \
    V(VK_FORMAT_R8G8B8_SSCALED) \
    V(VK_FORMAT_R8G8B8_UINT) \
    V(VK_FORMAT_R8G8B8_UNORM) \
    V(VK_FORMAT_R8G8B8_USCALED) \
    V(VK_FORMAT_R8G8_SINT) \
    V(VK_FORMAT_R8G8_SNORM) \
    V(VK_FORMAT_R8G8_SRGB) \
    V(VK_FORMAT_R8G8_SSCALED) \
    V(VK_FORMAT_R8G8_UINT) \
    V(VK_FORMAT_R8G8_UNORM) \
    V(VK_FORMAT_R8G8_USCALED) \
    V(VK_FORMAT_R8_SINT) \
    V(VK_FORMAT_R8_SNORM) \
    V(VK_FORMAT_R8_SRGB) \
    V(VK_FORMAT_R8_SSCALED) \
    V(VK_FORMAT_R8_UINT) \
    V(VK_FORMAT_R8_UNORM) \
    V(VK_FORMAT_R8_USCALED) \
    V(VK_FORMAT_S8_UINT) \
    V(VK_FORMAT_X8_D24_UNORM_PACK32)
static const struct { VkFormat f; const char *name; } k_formats[] = {
#define V(x) { x, #x },
    S2A_FACT_FORMATS(V)
#undef V
};
_Static_assert(sizeof k_formats / sizeof *k_formats == S2A_DEV_FACTS_FORMATS, "S2A_DEV_FACTS_FORMATS 与格式表项数不符");
const char *s2a_dev_facts_format_name(size_t i, VkFormat *format) {
    if (i >= S2A_DEV_FACTS_FORMATS) return NULL;
    if (format) *format = k_formats[i].f;
    return k_formats[i].name;
}

// VkPhysicalDeviceLimits 全部 106 项（Vulkan 头原文顺序换成字典序）。类型：U32 / I32 / U64 / F32，xN = 数组
#define S2A_FACT_LIMITS(L) \
    L(U64, bufferImageGranularity) \
    L(U32, discreteQueuePriorities) \
    L(U32, framebufferColorSampleCounts) \
    L(U32, framebufferDepthSampleCounts) \
    L(U32, framebufferNoAttachmentsSampleCounts) \
    L(U32, framebufferStencilSampleCounts) \
    L(F32, lineWidthGranularity) \
    L(F32x2, lineWidthRange) \
    L(U32, maxBoundDescriptorSets) \
    L(U32, maxClipDistances) \
    L(U32, maxColorAttachments) \
    L(U32, maxCombinedClipAndCullDistances) \
    L(U32, maxComputeSharedMemorySize) \
    L(U32x3, maxComputeWorkGroupCount) \
    L(U32, maxComputeWorkGroupInvocations) \
    L(U32x3, maxComputeWorkGroupSize) \
    L(U32, maxCullDistances) \
    L(U32, maxDescriptorSetInputAttachments) \
    L(U32, maxDescriptorSetSampledImages) \
    L(U32, maxDescriptorSetSamplers) \
    L(U32, maxDescriptorSetStorageBuffers) \
    L(U32, maxDescriptorSetStorageBuffersDynamic) \
    L(U32, maxDescriptorSetStorageImages) \
    L(U32, maxDescriptorSetUniformBuffers) \
    L(U32, maxDescriptorSetUniformBuffersDynamic) \
    L(U32, maxDrawIndexedIndexValue) \
    L(U32, maxDrawIndirectCount) \
    L(U32, maxFragmentCombinedOutputResources) \
    L(U32, maxFragmentDualSrcAttachments) \
    L(U32, maxFragmentInputComponents) \
    L(U32, maxFragmentOutputAttachments) \
    L(U32, maxFramebufferHeight) \
    L(U32, maxFramebufferLayers) \
    L(U32, maxFramebufferWidth) \
    L(U32, maxGeometryInputComponents) \
    L(U32, maxGeometryOutputComponents) \
    L(U32, maxGeometryOutputVertices) \
    L(U32, maxGeometryShaderInvocations) \
    L(U32, maxGeometryTotalOutputComponents) \
    L(U32, maxImageArrayLayers) \
    L(U32, maxImageDimension1D) \
    L(U32, maxImageDimension2D) \
    L(U32, maxImageDimension3D) \
    L(U32, maxImageDimensionCube) \
    L(F32, maxInterpolationOffset) \
    L(U32, maxMemoryAllocationCount) \
    L(U32, maxPerStageDescriptorInputAttachments) \
    L(U32, maxPerStageDescriptorSampledImages) \
    L(U32, maxPerStageDescriptorSamplers) \
    L(U32, maxPerStageDescriptorStorageBuffers) \
    L(U32, maxPerStageDescriptorStorageImages) \
    L(U32, maxPerStageDescriptorUniformBuffers) \
    L(U32, maxPerStageResources) \
    L(U32, maxPushConstantsSize) \
    L(U32, maxSampleMaskWords) \
    L(U32, maxSamplerAllocationCount) \
    L(F32, maxSamplerAnisotropy) \
    L(F32, maxSamplerLodBias) \
    L(U32, maxStorageBufferRange) \
    L(U32, maxTessellationControlPerPatchOutputComponents) \
    L(U32, maxTessellationControlPerVertexInputComponents) \
    L(U32, maxTessellationControlPerVertexOutputComponents) \
    L(U32, maxTessellationControlTotalOutputComponents) \
    L(U32, maxTessellationEvaluationInputComponents) \
    L(U32, maxTessellationEvaluationOutputComponents) \
    L(U32, maxTessellationGenerationLevel) \
    L(U32, maxTessellationPatchSize) \
    L(U32, maxTexelBufferElements) \
    L(U32, maxTexelGatherOffset) \
    L(U32, maxTexelOffset) \
    L(U32, maxUniformBufferRange) \
    L(U32, maxVertexInputAttributeOffset) \
    L(U32, maxVertexInputAttributes) \
    L(U32, maxVertexInputBindingStride) \
    L(U32, maxVertexInputBindings) \
    L(U32, maxVertexOutputComponents) \
    L(U32x2, maxViewportDimensions) \
    L(U32, maxViewports) \
    L(F32, minInterpolationOffset) \
    L(U64, minMemoryMapAlignment) \
    L(U64, minStorageBufferOffsetAlignment) \
    L(U64, minTexelBufferOffsetAlignment) \
    L(I32, minTexelGatherOffset) \
    L(I32, minTexelOffset) \
    L(U64, minUniformBufferOffsetAlignment) \
    L(U32, mipmapPrecisionBits) \
    L(U64, nonCoherentAtomSize) \
    L(U64, optimalBufferCopyOffsetAlignment) \
    L(U64, optimalBufferCopyRowPitchAlignment) \
    L(F32, pointSizeGranularity) \
    L(F32x2, pointSizeRange) \
    L(U32, sampledImageColorSampleCounts) \
    L(U32, sampledImageDepthSampleCounts) \
    L(U32, sampledImageIntegerSampleCounts) \
    L(U32, sampledImageStencilSampleCounts) \
    L(U64, sparseAddressSpaceSize) \
    L(U32, standardSampleLocations) \
    L(U32, storageImageSampleCounts) \
    L(U32, strictLines) \
    L(U32, subPixelInterpolationOffsetBits) \
    L(U32, subPixelPrecisionBits) \
    L(U32, subTexelPrecisionBits) \
    L(U32, timestampComputeAndGraphics) \
    L(F32, timestampPeriod) \
    L(F32x2, viewportBoundsRange) \
    L(U32, viewportSubPixelBits)
// VkPhysicalDeviceFeatures 全部 55 项（字典序）
#define S2A_FACT_FEATURES(F) \
    F(alphaToOne) \
    F(depthBiasClamp) \
    F(depthBounds) \
    F(depthClamp) \
    F(drawIndirectFirstInstance) \
    F(dualSrcBlend) \
    F(fillModeNonSolid) \
    F(fragmentStoresAndAtomics) \
    F(fullDrawIndexUint32) \
    F(geometryShader) \
    F(imageCubeArray) \
    F(independentBlend) \
    F(inheritedQueries) \
    F(largePoints) \
    F(logicOp) \
    F(multiDrawIndirect) \
    F(multiViewport) \
    F(occlusionQueryPrecise) \
    F(pipelineStatisticsQuery) \
    F(robustBufferAccess) \
    F(sampleRateShading) \
    F(samplerAnisotropy) \
    F(shaderClipDistance) \
    F(shaderCullDistance) \
    F(shaderFloat64) \
    F(shaderImageGatherExtended) \
    F(shaderInt16) \
    F(shaderInt64) \
    F(shaderResourceMinLod) \
    F(shaderResourceResidency) \
    F(shaderSampledImageArrayDynamicIndexing) \
    F(shaderStorageBufferArrayDynamicIndexing) \
    F(shaderStorageImageArrayDynamicIndexing) \
    F(shaderStorageImageExtendedFormats) \
    F(shaderStorageImageMultisample) \
    F(shaderStorageImageReadWithoutFormat) \
    F(shaderStorageImageWriteWithoutFormat) \
    F(shaderTessellationAndGeometryPointSize) \
    F(shaderUniformBufferArrayDynamicIndexing) \
    F(sparseBinding) \
    F(sparseResidency16Samples) \
    F(sparseResidency2Samples) \
    F(sparseResidency4Samples) \
    F(sparseResidency8Samples) \
    F(sparseResidencyAliased) \
    F(sparseResidencyBuffer) \
    F(sparseResidencyImage2D) \
    F(sparseResidencyImage3D) \
    F(tessellationShader) \
    F(textureCompressionASTC_LDR) \
    F(textureCompressionBC) \
    F(textureCompressionETC2) \
    F(variableMultisampleRate) \
    F(vertexPipelineStoresAndAtomics) \
    F(wideLines)

// ---------------- 查询 ----------------
// 设备扩展全表，同 vk_device.c extension_state 的口径：VK_INCOMPLETE（两次调用之间表变长）按新数目重取，最多 4 轮；
// 任何一轮失败 = 未知（不拿部分结果判「没有」）
static void enum_extensions(VkPhysicalDevice physical, struct s2a_dev_facts *f) {
    for (int round = 1; round <= 4; round++) {
        uint32_t count = 0;
        VkResult r = vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL);
        if (r != VK_SUCCESS) { snprintf(f->ext_why, sizeof f->ext_why, "取数目回 VkResult %d（第 %d 轮）", (int)r, round); f->ext_state = -1; return; }
        VkExtensionProperties *ext = calloc(count ? count : 1, sizeof *ext);
        if (!ext) { snprintf(f->ext_why, sizeof f->ext_why, "分配 %u 项失败（第 %d 轮）", count, round); f->ext_state = -1; return; }
        uint32_t got = count;
        r = vkEnumerateDeviceExtensionProperties(physical, NULL, &got, ext);
        if (r == VK_INCOMPLETE) { free(ext); continue; }
        if (r != VK_SUCCESS || got > count) {
            free(ext); snprintf(f->ext_why, sizeof f->ext_why, "取表回 VkResult %d、%u / %u 项（第 %d 轮）", (int)r, got, count, round);
            f->ext_state = -1; return;
        }
        if (got > S2A_DEV_FACTS_MAX_EXT) {
            free(ext); snprintf(f->ext_why, sizeof f->ext_why, "设备列出 %u 项，超过记录上限 %d", got, S2A_DEV_FACTS_MAX_EXT);
            f->ext_state = -1; return;
        }
        for (uint32_t i = 0; i < got; i++) snprintf(f->ext[i], sizeof f->ext[i], "%s", ext[i].extensionName);
        f->n_ext = got;
        f->ext_state = 1;
        free(ext);
        return;
    }
    snprintf(f->ext_why, sizeof f->ext_why, "连续 4 轮取表都回 VK_INCOMPLETE");
    f->ext_state = -1;
}
static int listed(const struct s2a_dev_facts *f, const char *name) {
    if (f->ext_state != 1) return 0;   // 未知：不挂依赖它的查询结构（规范不许），对应事实不记
    for (uint32_t i = 0; i < f->n_ext; i++) if (!strcmp(f->ext[i], name)) return 1;
    return 0;
}
#define LINK(head, s) do { (s)->pNext = (head); (head) = (s); } while (0)

static void query_vulkan(VkPhysicalDevice physical, struct s2a_dev_facts *f) {
    vkGetPhysicalDeviceProperties(physical, &f->props);
    vkGetPhysicalDeviceFeatures(physical, &f->features);
    vkGetPhysicalDeviceMemoryProperties(physical, &f->memory);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, NULL);
    if (nq > S2A_DEV_FACTS_MAX_QUEUE) nq = S2A_DEV_FACTS_MAX_QUEUE;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, f->queues);
    f->n_queue = nq;
    enum_extensions(physical, f);
    f->have_core11 = f->props.apiVersion >= VK_API_VERSION_1_1;

    // 特性：查询结构只挂设备列出的扩展（核心 1.1 的看设备版本），同 vk_device.c 的挂法
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = { .sType = S2A_FSTYPE_PLACED_FEATURES };
    void *chain = NULL;
    f->draw_parameters.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
    f->storage_16bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    f->var_ptrs.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES;
    f->multiview.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    f->timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    f->index_u8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT;
    f->f4444.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT;
    f->dcc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
    f->eds1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    f->eds2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    f->vi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
    f->img_robust.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT;
    f->zero_init_wg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES_KHR;
    f->scalar_layout.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT;
    f->ubo_std.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES_KHR;
    f->line_rast.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
    f->maint4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR;
    f->f16i8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR;
    f->border.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
    f->di.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    f->vdiv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
    f->swapchain_m1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    f->pipe_cache_ctl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT;
    f->hqr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT;
    f->list_restart.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT;
    f->view_2d_of_3d.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT;
    f->tba.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT;
    f->storage_8bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR;
    // 低于 1.1 的设备：本实例按 1.1 建，Features2 / Properties2 对它不成立（后端也建不了它）——整条扩展查询链都不挂，只记核心查询
    // 已转正的扩展：设备版本够（timeline / 描述符索引 1.2）就算有，不要求再列扩展名。4444 例外：1.3 只转正了格式枚举，
    //   VkPhysicalDevice4444FormatsFeaturesEXT 没有核心版，没列扩展时挂进链不合规（驱动多半忽略，位停在 0 被当成「实测不支持」），只在列了扩展时挂
    // 有效版本 = 应用申请的版本（S2A_VK_APP_API_VERSION = 1.1）与设备版本取低：转正进 1.2 / 1.3 的功能，有效版本够才按核心用，否则只认扩展
    const uint32_t api = s2a_vk_effective_api(f->props.apiVersion);
    f->have_4444_fmt = listed(f, "VK_EXT_4444_formats") || api >= VK_API_VERSION_1_3;
    if (f->have_core11) {
        LINK(chain, &f->draw_parameters);
        LINK(chain, &f->storage_16bit);
        LINK(chain, &f->var_ptrs);
        LINK(chain, &f->multiview);
        if ((f->have_timeline = listed(f, "VK_KHR_timeline_semaphore") || api >= VK_API_VERSION_1_2)) LINK(chain, &f->timeline);
        if ((f->have_index_u8 = listed(f, "VK_EXT_index_type_uint8") || listed(f, "VK_KHR_index_type_uint8"))) LINK(chain, &f->index_u8);   // KHR 版同一结构
        if ((f->have_4444 = listed(f, "VK_EXT_4444_formats"))) LINK(chain, &f->f4444);
        if ((f->have_dcc = listed(f, "VK_EXT_depth_clip_control"))) LINK(chain, &f->dcc);
        // 记录物理设备支持位，独立于本会话是否启用扩展。
        if ((f->have_eds1 = listed(f, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME))) LINK(chain, &f->eds1);
        if ((f->have_eds2 = listed(f, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME))) LINK(chain, &f->eds2);
        if ((f->have_vi = listed(f, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME))) LINK(chain, &f->vi);
        if ((f->have_img_robust = listed(f, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME) || api >= VK_API_VERSION_1_3)) LINK(chain, &f->img_robust);
        if ((f->have_zero_init_wg = listed(f, VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME) || api >= VK_API_VERSION_1_3)) LINK(chain, &f->zero_init_wg);
        if ((f->have_scalar_layout = listed(f, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME) || api >= VK_API_VERSION_1_2)) LINK(chain, &f->scalar_layout);
        if ((f->have_ubo_std = listed(f, VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME) || api >= VK_API_VERSION_1_2)) LINK(chain, &f->ubo_std);
        if ((f->have_line_rast = listed(f, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME) || listed(f, "VK_KHR_line_rasterization"))) LINK(chain, &f->line_rast);
        if ((f->have_maint4 = listed(f, VK_KHR_MAINTENANCE_4_EXTENSION_NAME) || api >= VK_API_VERSION_1_3)) LINK(chain, &f->maint4);
        if ((f->have_f16i8 = listed(f, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) || api >= VK_API_VERSION_1_2)) LINK(chain, &f->f16i8);
        if ((f->have_placed = listed(f, "VK_EXT_map_memory_placed"))) LINK(chain, &placed);
        if ((f->have_border = listed(f, "VK_EXT_custom_border_color"))) LINK(chain, &f->border);
        if ((f->have_di = listed(f, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) || api >= VK_API_VERSION_1_2)) LINK(chain, &f->di);
        if ((f->have_vdiv = listed(f, "VK_EXT_vertex_attribute_divisor"))) LINK(chain, &f->vdiv);   // 只认 EXT：下面属性链挂的是 EXT 的属性结构（KHR 版属性结构是另一个类型）
        if ((f->have_swapchain_m1 = listed(f, "VK_EXT_swapchain_maintenance1") || listed(f, "VK_KHR_swapchain_maintenance1"))) LINK(chain, &f->swapchain_m1);
        if ((f->have_pipe_cache_ctl = listed(f, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME))) LINK(chain, &f->pipe_cache_ctl);
        if ((f->have_hqr = listed(f, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) || api >= VK_API_VERSION_1_2)) LINK(chain, &f->hqr);
        if ((f->have_list_restart = listed(f, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME))) LINK(chain, &f->list_restart);
        if ((f->have_2d_of_3d = listed(f, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME))) LINK(chain, &f->view_2d_of_3d);
        if ((f->have_tba = listed(f, VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME))) LINK(chain, &f->tba);
        if ((f->have_8bit_storage = listed(f, VK_KHR_8BIT_STORAGE_EXTENSION_NAME) || api >= VK_API_VERSION_1_2)) LINK(chain, &f->storage_8bit);
    }
    if (chain) {
        VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = chain };
        vkGetPhysicalDeviceFeatures2(physical, &q);
    }
    f->placed_map = placed.memoryMapPlaced; f->placed_range = placed.memoryMapRangePlaced; f->placed_unmap_reserve = placed.memoryUnmapReserve;

    // 属性：驱动身份、单次分配上限、子组、各扩展的上限
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceMaintenance3Properties m3 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES };
    VkPhysicalDeviceSubgroupProperties sg = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceCustomBorderColorPropertiesEXT bp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT };
    VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT mp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES_EXT };
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT vp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT };
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceLineRasterizationPropertiesEXT lrp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT };
    VkPhysicalDeviceMaintenance4PropertiesKHR m4p = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES_KHR };
    VkPhysicalDeviceFloatControlsPropertiesKHR fcp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR };
    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = { .sType = S2A_FSTYPE_PLACED_PROPERTIES };
    chain = NULL;
    // driverProperties 是 1.2 核心 / VK_KHR_driver_properties：有效版本 ≥ 1.2 或列了扩展才挂（同 vk_device.c）；没有 = 驱动身份未知（null）
    f->have_driver = f->have_core11 && (api >= VK_API_VERSION_1_2 || listed(f, "VK_KHR_driver_properties"));
    if (f->have_core11) {
        if (f->have_driver) LINK(chain, &driver);
        LINK(chain, &m3); LINK(chain, &sg);
        if (f->have_border) LINK(chain, &bp);
        if ((f->have_minmax = listed(f, "VK_EXT_sampler_filter_minmax"))) LINK(chain, &mp);
        if (f->have_vdiv) LINK(chain, &vp);
        if ((f->have_host_import = listed(f, "VK_EXT_external_memory_host"))) LINK(chain, &hp);
        if (f->have_line_rast) LINK(chain, &lrp);
        if (f->have_maint4) LINK(chain, &m4p);
        if ((f->have_fctrl = listed(f, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME) || api >= VK_API_VERSION_1_2)) LINK(chain, &fcp);
        if (f->have_placed) LINK(chain, &pp);
    }
    if (chain) {
        VkPhysicalDeviceProperties2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = chain };
        vkGetPhysicalDeviceProperties2(physical, &q);
    }
    f->driver_id = (int)driver.driverID;
    snprintf(f->driver_name, sizeof f->driver_name, "%s", driver.driverName);
    f->max_alloc = m3.maxMemoryAllocationSize; f->max_per_set = m3.maxPerSetDescriptors;
    f->subgroup_size = sg.subgroupSize; f->subgroup_stages = sg.supportedStages; f->subgroup_ops = sg.supportedOperations;
    f->subgroup_quad_all = sg.quadOperationsInAllStages;
    f->border_max = bp.maxCustomBorderColorSamplers;
    f->minmax_single = mp.filterMinmaxSingleComponentFormats; f->minmax_mapping = mp.filterMinmaxImageComponentMapping;
    f->vdiv_max = vp.maxVertexAttribDivisor;
    f->host_align = hp.minImportedHostPointerAlignment;
    f->line_subpixel_bits = lrp.lineSubPixelPrecisionBits;
    f->max_buffer_size = m4p.maxBufferSize;
    f->fctrl_denorm_preserve_f32 = fcp.shaderDenormPreserveFloat32;
    f->fctrl_denorm_ftz_f32 = fcp.shaderDenormFlushToZeroFloat32;
    f->fctrl_sz_inf_nan_f32 = fcp.shaderSignedZeroInfNanPreserveFloat32;
    f->fctrl_rte_f32 = fcp.shaderRoundingModeRTEFloat32;
    f->fctrl_rtz_f32 = fcp.shaderRoundingModeRTZFloat32;
    f->placed_align = pp.minPlacedMemoryMapAlignment;

    // 逐格式：核心格式照常查；A4B4G4R4（VK_EXT_4444_formats 加的格式，1.3 转正）只在设备列出该扩展或设备 ≥ 1.3 时查，同 device_caps.c 的直查
    for (size_t i = 0; i < S2A_DEV_FACTS_FORMATS; i++) {
        if (k_formats[i].f == VK_FORMAT_A4B4G4R4_UNORM_PACK16 && !f->have_4444_fmt) continue;
        vkGetPhysicalDeviceFormatProperties(physical, k_formats[i].f, &f->fmt[i]);
        f->fmt_have[i] = 1;
    }
}

static uint32_t read_u32_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    unsigned v = 0;
    int ok = fscanf(fp, "%u", &v) == 1;
    fclose(fp);
    return ok ? v : 0;
}
#if defined(__ANDROID__)
static void prop(const char *name, char *out, size_t cap) {
    char b[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, b) <= 0) b[0] = 0;
    snprintf(out, cap, "%s", b);
}
#endif

// 系统段只记静态事实（全部进摘要）：SDK、页大小、核数、cpu_capacity、cpuinfo_max_freq（硬件上限）、KGSL 节点、ADPF / 节拍接口符号、机型。
//   会在运行中变的量一律不记：scaling_max_freq（温控 / 省电 / 厂商调频策略随时改）、在线核数、热状态、电量——记了指纹就抖、白白换代。
#ifdef S2A_DEV_FACTS_TEST
static const char *sysfs_root(void) { const char *r = getenv("S2A_TEST_SYSFS_ROOT"); return r ? r : ""; }   // 宿主测试：假 sysfs 根
#else
static const char *sysfs_root(void) { return ""; }
#endif

static void query_system(struct s2a_dev_facts *f) {
    f->page_size = sysconf(_SC_PAGESIZE);
    long n = sysconf(_SC_NPROCESSORS_CONF);
    f->ncpu = n > 0 && n <= S2A_DEV_FACTS_MAX_CPU ? (int)n : 0;
    char path[640];
    for (int i = 0; i < f->ncpu; i++) {
        snprintf(path, sizeof path, "%s/sys/devices/system/cpu/cpu%d/cpu_capacity", sysfs_root(), i);
        f->capacity[i] = read_u32_file(path);
        snprintf(path, sizeof path, "%s/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", sysfs_root(), i);
        f->max_khz[i] = read_u32_file(path);
    }
    struct stat st;
    f->kgsl = stat("/dev/kgsl-3d0", &st) == 0 ? 1 : errno == ENOENT ? 0 : -1;
    f->sdk = -1; f->adpf = -1; f->vsync_api = -1;
#if defined(__ANDROID__)
    char b[96];
    prop("ro.build.version.sdk", b, sizeof b);
    f->sdk = b[0] ? atoi(b) : -1;
    prop("ro.product.manufacturer", f->manufacturer, sizeof f->manufacturer);
    prop("ro.product.model", f->model, sizeof f->model);
    prop("ro.soc.model", f->soc_model, sizeof f->soc_model);
    prop("ro.build.version.release", f->android_release, sizeof f->android_release);
    f->have_model = 1;
    void *h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        // adpf_hint.c backend_init：五个必需符号齐才算有
        f->adpf = dlsym(h, "APerformanceHint_getManager") && dlsym(h, "APerformanceHint_createSession") &&
                  dlsym(h, "APerformanceHint_updateTargetWorkDuration") && dlsym(h, "APerformanceHint_reportActualWorkDuration") &&
                  dlsym(h, "APerformanceHint_closeSession");
        // nvn_vsync_android.c ops_resolve：API 33 富时间线四件套 > postFrameCallback64（29）> postFrameCallback（24）；ALooper 三件与 getInstance 必需
        int base = dlsym(h, "ALooper_prepare") && dlsym(h, "ALooper_pollOnce") && dlsym(h, "ALooper_wake") && dlsym(h, "AChoreographer_getInstance");
        int v33 = dlsym(h, "AChoreographer_postVsyncCallback") && dlsym(h, "AChoreographerFrameCallbackData_getFrameTimeNanos") &&
                  dlsym(h, "AChoreographerFrameCallbackData_getFrameTimelinesLength") &&
                  dlsym(h, "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex") &&
                  dlsym(h, "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos");
        f->vsync_api = !base ? 0 : v33 ? 33 : dlsym(h, "AChoreographer_postFrameCallback64") ? 29 : dlsym(h, "AChoreographer_postFrameCallback") ? 24 : 0;
        dlclose(h);
    } else {
        f->adpf = 0; f->vsync_api = 0;
    }
#endif
}

int s2a_dev_facts_collect(VkPhysicalDevice physical, struct s2a_dev_facts *out) {
    if (!physical || !out) return -1;
    memset(out, 0, sizeof *out);
    query_vulkan(physical, out);
    query_system(out);
    return 0;
}

// ---------------- 档案 JSON ----------------
// 规范写法 = Python json.dumps(obj, sort_keys=True, separators=(',', ':'), ensure_ascii=False)：键字典序、无空白；
// 字符串只转义 " \ 与控制字符（\b \f \n \r \t，其余 \u00XX）；非法 UTF-8 字节写成 '?'；整数十进制；
// 浮点：整值写成整数，否则 %.9g（float 的 9 位有效数字可往返；Python 读回再 repr 得到同一串）；非有限值写 null。
struct jw { char *p; size_t cap, n; int bad; };
static void jw_put(struct jw *w, const char *s, size_t len) {
    if (w->bad || w->n + len + 1 > w->cap) { w->bad = 1; return; }
    memcpy(w->p + w->n, s, len); w->n += len; w->p[w->n] = 0;
}
static void jw_s(struct jw *w, const char *s) { jw_put(w, s, strlen(s)); }
static void jw_f(struct jw *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jw_f(struct jw *w, const char *fmt, ...) {
    char b[128]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof b) { w->bad = 1; return; }
    jw_put(w, b, (size_t)n);
}
static int utf8_len(const unsigned char *s) {   // 合法 UTF-8 序列长度；0 = 非法
    if (s[0] < 0x80) return 1;
    int n = (s[0] & 0xe0) == 0xc0 ? 2 : (s[0] & 0xf0) == 0xe0 ? 3 : (s[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (!n || (n == 2 && s[0] < 0xc2) || (n == 4 && s[0] > 0xf4)) return 0;
    for (int i = 1; i < n; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    if (n == 3 && ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0))) return 0;   // 过长编码 / 代理区
    if (n == 4 && ((s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90))) return 0;
    return n;
}
static void jw_str(struct jw *w, const char *text) {
    const unsigned char *s = (const unsigned char *)text;
    jw_put(w, "\"", 1);
    while (*s) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; jw_put(w, e, 2); s++; continue; }
        if (c < 0x20) {
            const char *e = c == '\b' ? "\\b" : c == '\f' ? "\\f" : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : NULL;
            if (e) jw_s(w, e); else jw_f(w, "\\u%04x", c);
            s++; continue;
        }
        int n = utf8_len(s);
        if (!n) { jw_put(w, "?", 1); s++; continue; }
        jw_put(w, (const char *)s, (size_t)n); s += n;
    }
    jw_put(w, "\"", 1);
}
static void jw_flt(struct jw *w, double v) {
    if (!isfinite(v)) { jw_s(w, "null"); return; }
    if (v == floor(v) && fabs(v) < 9007199254740992.0) { jw_f(w, "%lld", (long long)v); return; }
    jw_f(w, "%.9g", v);
}
static void jw_key(struct jw *w, int *first, const char *k) {
    if (!*first) jw_put(w, ",", 1);
    *first = 0;
    jw_str(w, k);
    jw_put(w, ":", 1);
}
static void jw_text_or_null(struct jw *w, int known, const char *s) { if (known) jw_str(w, s); else jw_s(w, "null"); }

// 键按字典序输出：数值型的小对象先收进表、排序再写
struct kvn { const char *k; char v[80]; };
static int kvn_cmp(const void *a, const void *b) { return strcmp(((const struct kvn *)a)->k, ((const struct kvn *)b)->k); }
static void kv_add(struct kvn *t, size_t *n, const char *k, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void kv_add(struct kvn *t, size_t *n, const char *k, const char *fmt, ...) {
    t[*n].k = k; va_list ap; va_start(ap, fmt); vsnprintf(t[*n].v, sizeof t[*n].v, fmt, ap); va_end(ap); (*n)++;
}
static void kv_flt(struct kvn *t, size_t *n, const char *k, double v) {
    char b[80]; struct jw w = { b, sizeof b, 0, 0 }; jw_flt(&w, v); kv_add(t, n, k, "%s", b);
}
static void kv_emit(struct jw *w, struct kvn *t, size_t n) {
    qsort(t, n, sizeof *t, kvn_cmp);
    int first = 1;
    jw_put(w, "{", 1);
    for (size_t i = 0; i < n; i++) { jw_key(w, &first, t[i].k); jw_s(w, t[i].v); }
    jw_put(w, "}", 1);
}

static int strcmp_ptr(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void emit_limits(struct jw *w, const VkPhysicalDeviceLimits *l) {
    struct kvn t[128]; size_t n = 0;
#define L_U32(x) kv_add(t, &n, #x, "%u", (unsigned)l->x);
#define L_I32(x) kv_add(t, &n, #x, "%d", (int)l->x);
#define L_U64(x) kv_add(t, &n, #x, "%llu", (unsigned long long)l->x);
#define L_F32(x) kv_flt(t, &n, #x, l->x);
#define L_U32x2(x) kv_add(t, &n, #x, "[%u,%u]", (unsigned)l->x[0], (unsigned)l->x[1]);
#define L_U32x3(x) kv_add(t, &n, #x, "[%u,%u,%u]", (unsigned)l->x[0], (unsigned)l->x[1], (unsigned)l->x[2]);
#define L_F32x2(x) do { char a[40], b[40]; struct jw wa = { a, sizeof a, 0, 0 }, wb = { b, sizeof b, 0, 0 }; \
                        jw_flt(&wa, l->x[0]); jw_flt(&wb, l->x[1]); kv_add(t, &n, #x, "[%s,%s]", a, b); } while (0);
#define L(kind, x) L_##kind(x)
    S2A_FACT_LIMITS(L)
#undef L
    kv_emit(w, t, n);
}

static void emit_vulkan(struct jw *w, const struct s2a_dev_facts *f) {
    int first = 1;
    jw_put(w, "{", 1);
    // extensions：设备列出的每一项 = 1（不在表里 = 确实没有）；枚举失败时为空表并带 extensionsWhy（全部未知）
    jw_key(w, &first, "extensions");
    {
        const char *names[S2A_DEV_FACTS_MAX_EXT];
        for (uint32_t i = 0; i < f->n_ext; i++) names[i] = f->ext[i];
        qsort(names, f->n_ext, sizeof *names, strcmp_ptr);
        int ef = 1;
        jw_put(w, "{", 1);
        for (uint32_t i = 0; i < f->n_ext; i++) {
            if (i && !strcmp(names[i], names[i - 1])) continue;   // 驱动重复列同名扩展：JSON 键只出现一次
            jw_key(w, &ef, names[i]); jw_put(w, "1", 1);
        }
        jw_put(w, "}", 1);
    }
    if (f->ext_state != 1) { jw_key(w, &first, "extensionsWhy"); jw_str(w, f->ext_why); }
    // features：核心 55 项全写；扩展特性只写挂了查询结构的那几组
    jw_key(w, &first, "features");
    {
        struct kvn t[96]; size_t n = 0;
#define F(x) kv_add(t, &n, #x, "%d", f->features.x ? 1 : 0);
        S2A_FACT_FEATURES(F)
#undef F
#define B(k, v) kv_add(t, &n, k, "%d", (v) ? 1 : 0)
        if (f->have_core11) {
            B("shaderDrawParameters", f->draw_parameters.shaderDrawParameters);
            B("storageBuffer16BitAccess", f->storage_16bit.storageBuffer16BitAccess);
            B("uniformAndStorageBuffer16BitAccess", f->storage_16bit.uniformAndStorageBuffer16BitAccess);
            B("storagePushConstant16", f->storage_16bit.storagePushConstant16);
            B("storageInputOutput16", f->storage_16bit.storageInputOutput16);
            B("variablePointersStorageBuffer", f->var_ptrs.variablePointersStorageBuffer);
            B("variablePointers", f->var_ptrs.variablePointers);
            B("multiview", f->multiview.multiview);
            B("multiviewGeometryShader", f->multiview.multiviewGeometryShader);
            B("multiviewTessellationShader", f->multiview.multiviewTessellationShader);
        }
        if (f->have_timeline) B("timelineSemaphore", f->timeline.timelineSemaphore);
        if (f->have_index_u8) B("indexTypeUint8", f->index_u8.indexTypeUint8);
        if (f->have_4444) { B("formatA4B4G4R4", f->f4444.formatA4B4G4R4); B("formatA4R4G4B4", f->f4444.formatA4R4G4B4); }
        if (f->have_dcc) B("depthClipControl", f->dcc.depthClipControl);
        if (f->have_eds1) B("extendedDynamicState", f->eds1.extendedDynamicState == VK_TRUE);
        if (f->have_eds2) {
            B("extendedDynamicState2", f->eds2.extendedDynamicState2 == VK_TRUE);
            B("extendedDynamicState2LogicOp", f->eds2.extendedDynamicState2LogicOp == VK_TRUE);
            B("extendedDynamicState2PatchControlPoints", f->eds2.extendedDynamicState2PatchControlPoints == VK_TRUE);
        }
        if (f->have_vi) B("vertexInputDynamicState", f->vi.vertexInputDynamicState == VK_TRUE);
        if (f->have_img_robust) B("robustImageAccess", f->img_robust.robustImageAccess);
        if (f->have_zero_init_wg) B("shaderZeroInitializeWorkgroupMemory", f->zero_init_wg.shaderZeroInitializeWorkgroupMemory);
        if (f->have_scalar_layout) B("scalarBlockLayout", f->scalar_layout.scalarBlockLayout);
        if (f->have_ubo_std) B("uniformBufferStandardLayout", f->ubo_std.uniformBufferStandardLayout);
        if (f->have_line_rast) {
            B("rectangularLines", f->line_rast.rectangularLines);
            B("bresenhamLines", f->line_rast.bresenhamLines);
            B("smoothLines", f->line_rast.smoothLines);
            B("stippledRectangularLines", f->line_rast.stippledRectangularLines);
            B("stippledBresenhamLines", f->line_rast.stippledBresenhamLines);
            B("stippledSmoothLines", f->line_rast.stippledSmoothLines);
        }
        if (f->have_maint4) B("maintenance4", f->maint4.maintenance4);
        if (f->have_f16i8) { B("shaderFloat16", f->f16i8.shaderFloat16); B("shaderInt8", f->f16i8.shaderInt8); }
        if (f->have_placed) { B("memoryMapPlaced", f->placed_map); B("memoryMapRangePlaced", f->placed_range); B("memoryUnmapReserve", f->placed_unmap_reserve); }
        if (f->have_border) { B("customBorderColors", f->border.customBorderColors); B("customBorderColorWithoutFormat", f->border.customBorderColorWithoutFormat); }
        if (f->have_di) {
            B("runtimeDescriptorArray", f->di.runtimeDescriptorArray);
            B("descriptorBindingPartiallyBound", f->di.descriptorBindingPartiallyBound);
            B("shaderSampledImageArrayNonUniformIndexing", f->di.shaderSampledImageArrayNonUniformIndexing);
            B("shaderStorageImageArrayNonUniformIndexing", f->di.shaderStorageImageArrayNonUniformIndexing);
            B("descriptorBindingVariableDescriptorCount", f->di.descriptorBindingVariableDescriptorCount);
            B("descriptorBindingSampledImageUpdateAfterBind", f->di.descriptorBindingSampledImageUpdateAfterBind);
        }
        if (f->have_vdiv) {
            B("vertexAttributeInstanceRateDivisor", f->vdiv.vertexAttributeInstanceRateDivisor);
            B("vertexAttributeInstanceRateZeroDivisor", f->vdiv.vertexAttributeInstanceRateZeroDivisor);
        }
        if (f->have_swapchain_m1) B("swapchainMaintenance1", f->swapchain_m1.swapchainMaintenance1);
        if (f->have_pipe_cache_ctl) B("pipelineCreationCacheControl", f->pipe_cache_ctl.pipelineCreationCacheControl);
        if (f->have_hqr) B("hostQueryReset", f->hqr.hostQueryReset);
        if (f->have_list_restart) B("primitiveTopologyListRestart", f->list_restart.primitiveTopologyListRestart);
        if (f->have_2d_of_3d) {
            B("image2DViewOf3D", f->view_2d_of_3d.image2DViewOf3D);
            B("sampler2DViewOf3D", f->view_2d_of_3d.sampler2DViewOf3D);
        }
        if (f->have_tba) B("texelBufferAlignment", f->tba.texelBufferAlignment);
        if (f->have_8bit_storage) {
            B("storageBuffer8BitAccess", f->storage_8bit.storageBuffer8BitAccess);
            B("uniformAndStorageBuffer8BitAccess", f->storage_8bit.uniformAndStorageBuffer8BitAccess);
            B("storagePushConstant8", f->storage_8bit.storagePushConstant8);
        }
#undef B
        kv_emit(w, t, n);
    }
    // formats：VkFormat 名 → [linearTilingFeatures, optimalTilingFeatures, bufferFeatures]（表已按名字字典序）
    jw_key(w, &first, "formats");
    {
        int ff = 1;
        jw_put(w, "{", 1);
        for (size_t i = 0; i < S2A_DEV_FACTS_FORMATS; i++) {
            if (!f->fmt_have[i]) continue;
            jw_key(w, &ff, k_formats[i].name);
            jw_f(w, "[%u,%u,%u]", (unsigned)f->fmt[i].linearTilingFeatures, (unsigned)f->fmt[i].optimalTilingFeatures, (unsigned)f->fmt[i].bufferFeatures);
        }
        jw_put(w, "}", 1);
    }
    jw_key(w, &first, "memory");
    jw_s(w, "{\"heaps\":[");
    for (uint32_t i = 0; i < f->memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        jw_f(w, "%s{\"flags\":%u,\"size\":%llu}", i ? "," : "", (unsigned)f->memory.memoryHeaps[i].flags, (unsigned long long)f->memory.memoryHeaps[i].size);
    jw_s(w, "],\"types\":[");
    for (uint32_t i = 0; i < f->memory.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
        jw_f(w, "%s{\"flags\":%u,\"heap\":%u}", i ? "," : "", (unsigned)f->memory.memoryTypes[i].propertyFlags, (unsigned)f->memory.memoryTypes[i].heapIndex);
    jw_s(w, "]}");
    // properties：限值整组 + 扩展属性（只写查了的）
    jw_key(w, &first, "properties");
    {
        struct kvn t[32]; size_t n = 0;
        kv_add(t, &n, "deviceType", "%d", (int)f->props.deviceType);
        if (f->have_core11) {
            kv_add(t, &n, "maxMemoryAllocationSize", "%llu", (unsigned long long)f->max_alloc);
            kv_add(t, &n, "maxPerSetDescriptors", "%u", f->max_per_set);
            kv_add(t, &n, "subgroupSize", "%u", f->subgroup_size);
            kv_add(t, &n, "subgroupSupportedStages", "%u", (unsigned)f->subgroup_stages);
            kv_add(t, &n, "subgroupSupportedOperations", "%u", (unsigned)f->subgroup_ops);
            kv_add(t, &n, "subgroupQuadOperationsInAllStages", "%d", f->subgroup_quad_all ? 1 : 0);
        }
        if (f->have_border) kv_add(t, &n, "maxCustomBorderColorSamplers", "%u", f->border_max);
        if (f->have_minmax) {
            kv_add(t, &n, "filterMinmaxSingleComponentFormats", "%d", f->minmax_single ? 1 : 0);
            kv_add(t, &n, "filterMinmaxImageComponentMapping", "%d", f->minmax_mapping ? 1 : 0);
        }
        if (f->have_vdiv) kv_add(t, &n, "maxVertexAttribDivisor", "%u", f->vdiv_max);
        if (f->have_host_import) kv_add(t, &n, "minImportedHostPointerAlignment", "%llu", (unsigned long long)f->host_align);
        if (f->have_line_rast) kv_add(t, &n, "lineSubPixelPrecisionBits", "%u", f->line_subpixel_bits);
        if (f->have_maint4) kv_add(t, &n, "maxBufferSize", "%llu", (unsigned long long)f->max_buffer_size);
        if (f->have_fctrl) {
            kv_add(t, &n, "shaderDenormPreserveFloat32", "%d", f->fctrl_denorm_preserve_f32 ? 1 : 0);
            kv_add(t, &n, "shaderDenormFlushToZeroFloat32", "%d", f->fctrl_denorm_ftz_f32 ? 1 : 0);
            kv_add(t, &n, "shaderSignedZeroInfNanPreserveFloat32", "%d", f->fctrl_sz_inf_nan_f32 ? 1 : 0);
            kv_add(t, &n, "shaderRoundingModeRTEFloat32", "%d", f->fctrl_rte_f32 ? 1 : 0);
            kv_add(t, &n, "shaderRoundingModeRTZFloat32", "%d", f->fctrl_rtz_f32 ? 1 : 0);
        }
        if (f->have_placed) kv_add(t, &n, "minPlacedMemoryMapAlignment", "%llu", (unsigned long long)f->placed_align);
        kv_add(t, &n, "limits", "%s", "");   // 占位：排序后在这里写整组限值
        qsort(t, n, sizeof *t, kvn_cmp);
        int pf = 1;
        jw_put(w, "{", 1);
        for (size_t i = 0; i < n; i++) {
            jw_key(w, &pf, t[i].k);
            if (!strcmp(t[i].k, "limits")) emit_limits(w, &f->props.limits); else jw_s(w, t[i].v);
        }
        jw_put(w, "}", 1);
    }
    jw_key(w, &first, "queues");
    jw_put(w, "[", 1);
    for (uint32_t i = 0; i < f->n_queue; i++)
        jw_f(w, "%s{\"count\":%u,\"flags\":%u,\"timestampValidBits\":%u}", i ? "," : "", f->queues[i].queueCount,
             (unsigned)f->queues[i].queueFlags, f->queues[i].timestampValidBits);
    jw_put(w, "]", 1);
    jw_put(w, "}", 1);
}

static void emit_system(struct jw *w, const struct s2a_dev_facts *f) {
    jw_s(w, "{\"adpf\":");
    if (f->adpf < 0) jw_s(w, "null"); else jw_f(w, "%d", f->adpf);
    jw_s(w, ",\"cpu\":{\"capacity\":[");
    for (int i = 0; i < f->ncpu; i++) jw_f(w, "%s%u", i ? "," : "", f->capacity[i]);
    jw_s(w, "],\"maxKHz\":[");
    for (int i = 0; i < f->ncpu; i++) jw_f(w, "%s%u", i ? "," : "", f->max_khz[i]);
    jw_s(w, "]},\"kgsl\":");
    jw_f(w, "%d", f->kgsl);
    jw_s(w, ",\"pageSize\":");
    if (f->page_size > 0) jw_f(w, "%ld", f->page_size); else jw_s(w, "null");
    jw_s(w, ",\"sdk\":");
    if (f->sdk < 0) jw_s(w, "null"); else jw_f(w, "%d", f->sdk);
    jw_s(w, ",\"vsyncApi\":");
    if (f->vsync_api < 0) jw_s(w, "null"); else jw_f(w, "%d", f->vsync_api);
    jw_put(w, "}", 1);
}

// digest_hex 为 NULL：写摘要的输入（不带 factsDigest 与元数据 completeness / provenance）；否则全都带上。
//   provenance 恒为 probed（本机现查）；completeness：扩展表完整枚举 = complete，枚举失败（扩展、扩展特性、扩展属性全部未知）= partial
static void emit_profile(struct jw *w, const struct s2a_dev_facts *f, const char *digest_hex) {
    const uint32_t api = f->props.apiVersion;
    jw_f(w, "{\"api\":{\"kind\":\"vulkan\",\"version\":\"%u.%u.%u\"}", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
    if (digest_hex) {
        jw_s(w, f->ext_state == 1 ? ",\"completeness\":\"complete\"" : ",\"completeness\":\"partial\"");
        jw_s(w, ",\"factsDigest\":\"sha256-"); jw_s(w, digest_hex); jw_s(w, "\"");
    }
    jw_f(w, ",\"identity\":{\"deviceId\":\"0x%08x\",\"deviceName\":", f->props.deviceID);
    jw_str(w, f->props.deviceName);
    jw_s(w, ",\"driverId\":");
    if (f->have_driver) jw_f(w, "%d", f->driver_id); else jw_s(w, "null");
    jw_s(w, ",\"driverName\":");
    jw_text_or_null(w, f->have_driver, f->driver_name);
    jw_f(w, ",\"driverVersion\":%u,\"pipelineCacheUUID\":\"", f->props.driverVersion);
    for (unsigned i = 0; i < VK_UUID_SIZE; i++) jw_f(w, "%02x", f->props.pipelineCacheUUID[i]);
    jw_f(w, "\",\"vendorId\":\"0x%04x\"}", f->props.vendorID);
    jw_s(w, ",\"model\":{\"androidRelease\":"); jw_text_or_null(w, f->have_model && f->android_release[0], f->android_release);
    jw_s(w, ",\"manufacturer\":"); jw_text_or_null(w, f->have_model && f->manufacturer[0], f->manufacturer);
    jw_s(w, ",\"model\":"); jw_text_or_null(w, f->have_model && f->model[0], f->model);
    jw_s(w, ",\"socModel\":"); jw_text_or_null(w, f->have_model && f->soc_model[0], f->soc_model);
#if defined(__OHOS__)
    jw_s(w, "},\"platform\":\"ohos\"");
#else
    jw_s(w, "},\"platform\":\"android\"");
#endif
    if (digest_hex) jw_s(w, ",\"provenance\":\"probed\"");
    jw_s(w, ",\"schema\":\"s2a-device-profile-v1\",\"system\":");
    emit_system(w, f);
    jw_s(w, ",\"vulkan\":");
    emit_vulkan(w, f);
    jw_put(w, "}", 1);
}

int s2a_dev_profile_json(const struct s2a_dev_facts *f, char *out, size_t cap, size_t *len, char digest_hex[65]) {
    if (!f || !out || !cap) return -1;
    struct jw w = { out, cap, 0, 0 };
    emit_profile(&w, f, NULL);
    if (w.bad) return -1;
    s2a_sha256 ctx; uint8_t d[32];
    s2a_sha256_init(&ctx); s2a_sha256_update(&ctx, out, w.n); s2a_sha256_final(&ctx, d);
    char hex[65]; s2a_sha256_hex(d, hex);
    w.n = 0; w.bad = 0;
    emit_profile(&w, f, hex);
    if (w.bad) return -1;
    if (len) *len = w.n;
    if (digest_hex) memcpy(digest_hex, hex, 65);
    return 0;
}

// ---------------- 落盘 ----------------
#if defined(__APPLE__)
#define ST_MTIM st_mtimespec   // 宿主测试（macOS）的同一字段
#else
#define ST_MTIM st_mtim
#endif
static int is_generation_name(const char *n) {   // <16 位小写 hex>.json：只认这一种，别的文件不碰
    if (strlen(n) != 21 || strcmp(n + 16, ".json")) return 0;
    for (int i = 0; i < 16; i++) if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return 0;
    return 1;
}
int s2a_dev_profile_store(const char *files_dir, const char *json, size_t len, const char *digest_hex,
                          int *written, int *removed, char *why, size_t why_size) {
    if (written) *written = 0;
    if (removed) *removed = 0;
    if (!files_dir || files_dir[0] != '/' || !json || !digest_hex || strlen(digest_hex) < 16) {
        snprintf(why, why_size, "目录或摘要无效"); return EINVAL;
    }
    char dir[512], name[32], path[600], tmp[620];
    if ((size_t)snprintf(dir, sizeof dir, "%s/devprofile", files_dir) >= sizeof dir) { snprintf(why, why_size, "目录名过长"); return ENAMETOOLONG; }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) { int e = errno; snprintf(why, why_size, "建目录失败 errno=%d", e); return e; }
    snprintf(name, sizeof name, "%.16s.json", digest_hex);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == len + 1) {
        // 同一代已在（文件名就是内容摘要）：不重写，只把修改时间刷成现在——「最新一代」按修改时间认（导出、留代都用它）
        if (utimensat(AT_FDCWD, path, NULL, 0) != 0) { int e = errno; snprintf(why, why_size, "刷同一代档案修改时间失败 errno=%d（「最新一代」会按旧时间认）", e); return e; }
    } else {
        snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, name);
        FILE *fp = fopen(tmp, "w");
        if (!fp) { int e = errno; snprintf(why, why_size, "开临时文件失败 errno=%d", e); return e; }
        int ok = fwrite(json, 1, len, fp) == len && fputc('\n', fp) != EOF && fflush(fp) == 0 && fsync(fileno(fp)) == 0;   // 落到盘上再 rename
        if (fclose(fp) != 0) ok = 0;
        if (!ok || rename(tmp, path) != 0) { int e = errno ? errno : EIO; unlink(tmp); snprintf(why, why_size, "写档案失败 errno=%d", e); return e; }
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);   // 同仓库其他原子落盘：临时文件 fsync → rename → 目录 fsync
        if (dfd >= 0) { fsync(dfd); close(dfd); }
        if (written) *written = 1;
    }
    // 留最近 2 代（当前一代 + 修改时间最新的一份旧代，驱动升级后能前后对照），更旧的删掉；别的名字不碰
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char keep[32] = "";
        struct timespec keep_t = {0, 0};
        char doomed[16][32]; int n_doomed = 0;
        while ((e = readdir(d))) {
            if (!is_generation_name(e->d_name) || !strcmp(e->d_name, name)) continue;
            char old[600]; struct stat os;
            snprintf(old, sizeof old, "%s/%.21s", dir, e->d_name);   // 代名已核为 21 字符（is_generation_name）：精度写明上限，编译器可证不截断
            if (stat(old, &os) != 0) continue;
            int newer = !keep[0] || os.ST_MTIM.tv_sec > keep_t.tv_sec ||
                        (os.ST_MTIM.tv_sec == keep_t.tv_sec && os.ST_MTIM.tv_nsec > keep_t.tv_nsec);
            const char *drop = e->d_name;
            if (newer) { if (keep[0] && n_doomed < 16) snprintf(doomed[n_doomed++], 32, "%.21s", keep); snprintf(keep, sizeof keep, "%.21s", e->d_name); keep_t = os.ST_MTIM; drop = NULL; }
            if (drop && n_doomed < 16) snprintf(doomed[n_doomed++], 32, "%.21s", drop);
        }
        closedir(d);
        for (int i = 0; i < n_doomed; i++) {
            char old[600];
            snprintf(old, sizeof old, "%s/%.21s", dir, doomed[i]);
            if (unlink(old) == 0 && removed) (*removed)++;
        }
    }
    return 0;
}

// ---------------- 摘要行 ----------------
void s2a_dev_facts_summary(const struct s2a_dev_facts *f, const char *digest_hex, char *out, size_t cap) {
    int feat_on = 0, feat_n = 0;
#define F(x) feat_n++; feat_on += f->features.x ? 1 : 0;
    S2A_FACT_FEATURES(F)
#undef F
    int fmt_n = 0, fmt_rt = 0;
    for (size_t i = 0; i < S2A_DEV_FACTS_FORMATS; i++) {
        if (!f->fmt_have[i]) continue;
        fmt_n++;
        fmt_rt += (f->fmt[i].optimalTilingFeatures & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
    }
    int tiers = 0;   // capacity 有几档（0 = 读不到）
    for (int i = 0; i < f->ncpu; i++) {
        int seen = 0;
        for (int k = 0; k < i; k++) seen |= f->capacity[k] == f->capacity[i];
        tiers += f->capacity[i] && !seen;
    }
    const uint32_t api = f->props.apiVersion;
    char ext[48];
    if (f->ext_state == 1) snprintf(ext, sizeof ext, "%u", f->n_ext); else snprintf(ext, sizeof ext, "未知（%.24s）", f->ext_why);
    snprintf(out, cap, "【事实】v1 fp=%.16s GPU=\"%s\" 驱动=\"%s\"(id %d, 0x%08x) api=%u.%u.%u 扩展=%s 核心特性=%d/%d 格式=%d（可当附件 %d）"
             " 显存堆=%u 类型=%u 队列族=%u｜系统 sdk=%d 页=%ld 核=%d（capacity %d 档）kgsl=%d adpf=%d vsync=%d",
             digest_hex ? digest_hex : "?", f->props.deviceName, f->have_driver ? f->driver_name : "?", f->driver_id, f->props.driverVersion,
             VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api), ext, feat_on, feat_n, fmt_n, fmt_rt,
             f->memory.memoryHeapCount, f->memory.memoryTypeCount, f->n_queue,
             f->sdk, f->page_size, f->ncpu, tiers, f->kgsl, f->adpf, f->vsync_api);
}

// ---------------- vk_device.c 的挂点 ----------------
extern const char *s2a_android_gpu_driver_files_dir(void) __attribute__((weak));   // bridge.cpp：App 的 files 目录

static double ms_since(const struct timespec *a) {
    struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
    return (double)(b.tv_sec - a->tv_sec) * 1e3 + (double)(b.tv_nsec - a->tv_nsec) / 1e6;
}

static char g_logged_digest[17];   // 本进程已打过摘要行的那一代（设备丢失后重建同一台设备不再刷屏）

int s2a_dev_facts_on_device_create(VkPhysicalDevice physical) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    struct s2a_dev_facts *f = calloc(1, sizeof *f);
    enum { CAP = 64 << 10 };
    char *json = malloc(CAP);
    if (!f || !json) { S2A_FLOG("【红】【事实】分配失败，本次不记设备事实"); free(f); free(json); return 0; }
    s2a_dev_facts_collect(physical, f);
    const double collect_ms = ms_since(&t0);
    size_t len = 0; char digest[65] = "";
    char line[1536], where[160] = "";
    if (s2a_dev_profile_json(f, json, CAP, &len, digest)) {
        S2A_FLOG("【红】【事实】档案序列化失败（超过 %d 字节），本次不落盘", (int)CAP);
    } else {
        const char *files = s2a_android_gpu_driver_files_dir ? s2a_android_gpu_driver_files_dir() : NULL;
        if (!files || files[0] != '/') {
            snprintf(where, sizeof where, "档案=未落盘（App 目录未配置）");
        } else {
            int written = 0, removed = 0; char why[96] = "";
            int rc = s2a_dev_profile_store(files, json, len, digest, &written, &removed, why, sizeof why);
            if (rc) snprintf(where, sizeof where, "档案=写失败（%s）", why);
            else snprintf(where, sizeof where, "档案=devprofile/%.16s.json（%s，清旧代 %d）", digest, written ? "新写" : "同一代已在", removed);
        }
    }
    int logged = 0;
    if (!digest[0] || strncmp(g_logged_digest, digest, 16)) {
        s2a_dev_facts_summary(f, digest, line, sizeof line);
        S2A_FLOG("%s %s 采集 %.1f ms 合计 %.1f ms", line, where, collect_ms, ms_since(&t0));
        if (f->ext_state != 1) S2A_FLOG("【红】【事实】设备扩展表未知（%s）：档案 extensions 为空表、带 extensionsWhy", f->ext_why);
        struct s2a_dev_policy policy;
        const struct s2a_dev_policy_inputs inputs = {.facts = f};
        if (!s2a_dev_policy_derive(&inputs, &policy) && !s2a_dev_policy_summary(&policy, digest, line, sizeof line)) {
            S2A_FLOG("%s", line);
            for (enum s2a_policy_id id = 0; id < S2A_POLICY_COUNT; id++) {
                const struct s2a_policy_item *item = s2a_dev_policy_get(&policy, id);
                if (item->basis == S2A_POLICY_UNKNOWN)
                    S2A_FLOG("【红】【策略】%s 未证（%s）", s2a_dev_policy_meta(id)->name, item->evidence);
            }
        } else {
            S2A_FLOG("【红】【策略】现状兼容摘要生成失败");
        }
        snprintf(g_logged_digest, sizeof g_logged_digest, "%.16s", digest);
        logged = 1;
    }
    free(json);
    free(f);
    return logged;
}
