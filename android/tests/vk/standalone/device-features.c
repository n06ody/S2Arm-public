// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "vk_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *name;
static int scenario, failures, validation_errors, captured, query_seen, instance_caps, instance_maintenance;
static VkDebugUtilsMessengerEXT messenger;
static VkBool32 debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                        VkDebugUtilsMessageTypeFlagsEXT type,
                        const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        validation_errors++;
        fprintf(stderr, "VVL: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
static void check(int ok, const char *message) {
    if (!ok) { failures++; fprintf(stderr, "FAIL %s: %s\n", name, message); }
}
static int contains(uint32_t n, const char *const *names, const char *wanted) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(names[i], wanted)) return 1;
    return 0;
}
static int has_extension(uint32_t n, const VkExtensionProperties *p, const char *wanted) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, wanted)) return 1;
    return 0;
}
static void add_extension(uint32_t *n, VkExtensionProperties *p, const char *wanted) {
    if (has_extension(*n, p, wanted)) return;
    if (*n == 512) abort();
    memset(&p[*n], 0, sizeof p[*n]);
    snprintf(p[*n].extensionName, sizeof p[*n].extensionName, "%s", wanted);
    p[(*n)++].specVersion = 1;
}
static VkResult copy_extensions(uint32_t n, const VkExtensionProperties *p, uint32_t *count, VkExtensionProperties *out) {
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t written = *count < n ? *count : n;
    memcpy(out, p, written * sizeof *out); *count = written;
    return written < n ? VK_INCOMPLETE : VK_SUCCESS;
}
VkResult __real_vkEnumerateInstanceExtensionProperties(const char *, uint32_t *, VkExtensionProperties *);
VkResult __wrap_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *out) {
    if (layer) return __real_vkEnumerateInstanceExtensionProperties(layer, count, out);
    VkExtensionProperties p[512]; uint32_t n = 512;
    VkResult r = __real_vkEnumerateInstanceExtensionProperties(NULL, &n, p);
    if (r != VK_SUCCESS) return r;
    if (scenario) {
        add_extension(&n, p, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        add_extension(&n, p, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        for (uint32_t i = 0; i < n;) {
            if ((scenario == 2 && !strcmp(p[i].extensionName, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) ||
                (scenario == 3 && !strcmp(p[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)))
                p[i] = p[--n];
            else i++;
        }
    }
    return copy_extensions(n, p, count, out);
}
VkResult __real_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
VkResult __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer,
                                                   uint32_t *count, VkExtensionProperties *out) {
    if (layer) return __real_vkEnumerateDeviceExtensionProperties(physical, layer, count, out);
    if (scenario == 7) { *count = 0; return VK_SUCCESS; }
    VkExtensionProperties p[512]; uint32_t n = 512;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, p);
    if (r != VK_SUCCESS) return r;
    if (scenario) {
        // Jammy lavapipe 没有 maintenance1；模拟能力只用于捕获，绝不送进真实 vkCreateDevice。
        add_extension(&n, p, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        add_extension(&n, p, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        for (uint32_t i = 0; i < n;) {
            if ((scenario == 1 || scenario == 7) && !strcmp(p[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
                p[i] = p[--n];
            else i++;
        }
    }
    return copy_extensions(n, p, count, out);
}
VkResult __real_vkCreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
VkResult __wrap_vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc, VkInstance *instance) {
    check(info->pApplicationInfo->apiVersion == VK_API_VERSION_1_1, "application must remain Vulkan 1.1");
    instance_caps = contains(info->enabledExtensionCount, info->ppEnabledExtensionNames, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    instance_maintenance = contains(info->enabledExtensionCount, info->ppEnabledExtensionNames, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    const char *ext[64]; uint32_t n = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char *e = info->ppEnabledExtensionNames[i];
        if (scenario && (!strcmp(e, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) ||
                         !strcmp(e, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))) continue;
        if (n == 63) abort();
        ext[n++] = e;
    }
    ext[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug = {0};
    debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debug_cb; debug.pNext = info->pNext;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ci = *info; ci.pNext = &debug;
    ci.enabledExtensionCount = n; ci.ppEnabledExtensionNames = ext;
    ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = layers;
    VkResult r = __real_vkCreateInstance(&ci, alloc, instance);
    if (r == VK_SUCCESS) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(*instance, "vkCreateDebugUtilsMessengerEXT");
        debug.pNext = NULL;
        check(create && create(*instance, &debug, NULL, &messenger) == VK_SUCCESS, "validation messenger created");
    }
    return r;
}
void __real_vkDestroyInstance(VkInstance, const VkAllocationCallbacks *);
void __wrap_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy(instance, messenger, NULL); messenger = VK_NULL_HANDLE;
    }
    __real_vkDestroyInstance(instance, alloc);
}
enum { SPARSE_IMAGE_2D_OFF = 8, SPARSE_ALIASED_OFF, WIDE_LINES_HIDDEN };

static void mask_device_features(VkPhysicalDeviceFeatures *features) {
    if (scenario == SPARSE_IMAGE_2D_OFF) features->sparseResidencyImage2D = VK_FALSE;
    if (scenario == SPARSE_ALIASED_OFF) features->sparseResidencyAliased = VK_FALSE;
    if (scenario == WIDE_LINES_HIDDEN) features->wideLines = VK_FALSE;
}

void __real_vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *);
void __wrap_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *features) {
    __real_vkGetPhysicalDeviceFeatures(physical, features);
    mask_device_features(features);
}

void __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
void __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features) {
    VkBaseOutStructure **link = (VkBaseOutStructure **)&features->pNext;
    VkBaseOutStructure **removed_at = NULL, *removed = NULL;
    int timeline = 0;
    for (VkBaseOutStructure *p = *link; p; p = *link) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) timeline = 1;
        if (scenario && p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) {
            removed_at = link; removed = p; *link = p->pNext;
        } else link = &p->pNext;
    }
    if (scenario == 1 || scenario == 7) check(!timeline, "timeline query struct present with extension hidden");
    query_seen++;
    __real_vkGetPhysicalDeviceFeatures2(physical, features);
    mask_device_features(&features->features);
    if (removed) {
        removed->pNext = *removed_at; *removed_at = removed;
        ((VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT *)removed)->swapchainMaintenance1 = scenario != 5;
    }
    if (scenario == 6) {
        for (VkBaseOutStructure *p = features->pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
                ((VkPhysicalDeviceTimelineSemaphoreFeatures *)p)->timelineSemaphore = VK_FALSE;
    }
}

// 稀疏纹理必须既有物理能力，又实际申请了可提交绑定的队列。
static void expect_sparse_features(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                   const VkPhysicalDeviceFeatures *supported, VkPhysicalDeviceFeatures *expected) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof *families);
    if (!families) abort();
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    int available = 0, requested = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!families[i].queueCount || !(families[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) continue;
        available = 1;
        for (uint32_t j = 0; j < info->queueCreateInfoCount; j++) {
            const VkDeviceQueueCreateInfo *queue = &info->pQueueCreateInfos[j];
            if (queue->queueFamilyIndex == i && queue->queueCount) requested = 1;
        }
    }
    free(families);
    expected->sparseBinding = available && supported->sparseBinding && supported->sparseResidencyImage2D;
    expected->sparseResidencyImage2D = expected->sparseBinding;
    expected->sparseResidencyAliased = expected->sparseBinding && supported->sparseResidencyAliased;
    check(!expected->sparseBinding || requested, "sparse residency requires a requested sparse queue");
}

// 独立硬编码的允许集合；robustBufferAccess 有意保留（#1581 P 的 OOB 审计）。
#define ALLOWED(F) \
    F(robustBufferAccess) F(fullDrawIndexUint32) F(imageCubeArray) F(independentBlend) \
    F(geometryShader) F(tessellationShader) F(sampleRateShading) F(dualSrcBlend) F(logicOp) F(multiDrawIndirect) \
    F(drawIndirectFirstInstance) F(depthClamp) F(depthBiasClamp) F(fillModeNonSolid) \
    F(wideLines) F(largePoints) F(multiViewport) F(samplerAnisotropy) F(textureCompressionETC2) \
    F(textureCompressionASTC_LDR) F(textureCompressionBC) F(occlusionQueryPrecise) \
    F(vertexPipelineStoresAndAtomics) F(fragmentStoresAndAtomics) F(shaderTessellationAndGeometryPointSize) \
    F(shaderImageGatherExtended) F(shaderStorageImageExtendedFormats) F(shaderStorageImageMultisample) \
    F(shaderStorageImageReadWithoutFormat) F(shaderStorageImageWriteWithoutFormat) \
    F(shaderUniformBufferArrayDynamicIndexing) F(shaderSampledImageArrayDynamicIndexing) \
    F(shaderStorageBufferArrayDynamicIndexing) F(shaderStorageImageArrayDynamicIndexing) \
    F(shaderClipDistance) F(shaderCullDistance) F(shaderInt16) F(shaderResourceMinLod) F(variableMultisampleRate)

VkResult __real_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
VkResult __wrap_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                             const VkAllocationCallbacks *alloc, VkDevice *device) {
    captured++;
    const VkPhysicalDeviceFeatures *features = info->pEnabledFeatures;
    int timeline = 0, maintenance = 0;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            features = &((const VkPhysicalDeviceFeatures2 *)p)->features;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            timeline = 1;
            if (scenario == 6) check(!((const VkPhysicalDeviceTimelineSemaphoreFeatures *)p)->timelineSemaphore, "unsupported timeline bit enabled");
        }
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) maintenance = 1;
    }
    check(features != NULL, "core features provided");
    if (features) {
        VkPhysicalDeviceFeatures extra = *features, supported;
        __real_vkGetPhysicalDeviceFeatures(physical, &supported);
#define CLEAR(f) extra.f = VK_FALSE;
        ALLOWED(CLEAR)
#undef CLEAR
        extra.sparseBinding = extra.sparseResidencyImage2D = extra.sparseResidencyAliased = VK_FALSE;
        const unsigned char zero[sizeof extra] = {0};
        check(!memcmp(&extra, zero, sizeof extra), "whitelist: undeclared core feature enabled");
        check(supported.wideLines, "lavapipe must expose wideLines for the positive control");
        mask_device_features(&supported);
        check(features->wideLines == supported.wideLines, "device wideLines enable must match advertised capability");
        check(features->robustBufferAccess == supported.robustBufferAccess, "whitelist: robustness must be retained");
        VkPhysicalDeviceFeatures expected = {0};
#define COPY(f) expected.f = supported.f;
        ALLOWED(COPY)
#undef COPY
        expected.geometryShader = VK_FALSE; // 未链接 GS 探针，与生产弱引用缺省路径相同。
        expect_sparse_features(physical, info, &supported, &expected);
        check(!memcmp(features, &expected, sizeof expected), "whitelist: supported required/optional feature lost");
    }
    int timeline_ext = contains(info->enabledExtensionCount, info->ppEnabledExtensionNames, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    int maintenance_ext = contains(info->enabledExtensionCount, info->ppEnabledExtensionNames, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    check(timeline == timeline_ext, "timeline create chain/extension mismatch");
    if (scenario == 1 || scenario == 7) check(!timeline_ext && !timeline, "timeline hidden must stay off");
    else if (scenario) check(timeline && timeline_ext, "advertised timeline must be enabled/chained");
    check(!maintenance_ext || (instance_caps && instance_maintenance), "maintenance instance dependencies missing");
    check(maintenance == maintenance_ext, "maintenance chain/extension mismatch");
    if (scenario) {
        int expected = scenario != 2 && scenario != 3 && scenario != 5 && scenario != 7;
        check(maintenance_ext == expected, "maintenance dependency/feature gate");
        *device = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED; // 捕获模拟能力，不把它交给真实驱动。
    }
    return __real_vkCreateDevice(physical, info, alloc, device);
}
int main(void) {
    const char *names[] = {"default", "timeline-hidden", "surface-hidden", "capabilities-hidden",
                           "dependencies-present", "maintenance-feature-off", "timeline-feature-off", "device-extensions-empty",
                           [SPARSE_IMAGE_2D_OFF] = "sparse-image-2d-off", [SPARSE_ALIASED_OFF] = "sparse-aliased-off",
                           [WIDE_LINES_HIDDEN] = "wide-lines-hidden"};
    int cases = (int)(sizeof names / sizeof names[0]);
    for (scenario = 0; scenario < cases; scenario++) {
        name = names[scenario]; captured = query_seen = 0;
        struct s2a_vk_device device; char why[256] = "";
        int before = failures, rc = s2a_vk_device_create(&device, why, sizeof why);
        check(captured == 1 && query_seen > 0, "production reached query and vkCreateDevice");
        check(scenario ? rc != 0 : rc == 0, "real create or capture-stop result");
        if (scenario == 1 || scenario == 6 || scenario == 7)
            check(!device.timeline_semaphore_enabled, "timeline unavailable must stay off");
        check(!s2a_vk_device_destroy(&device, 0), "device/instance cleanup");
        printf("device-features case=%s failures=%d\n", name, failures - before);
    }
    printf("device-features cases=%d failures=%d validation-errors=%d\n", cases, failures, validation_errors);
    return failures || validation_errors ? 1 : 0;
}
