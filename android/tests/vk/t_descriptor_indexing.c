// Real Vulkan driver + validation. Only remove capabilities from what the driver reports;
// the device creation itself is forwarded to the driver. Vulkan types come from NDK 29.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_device.h"
#include "device_caps.h"

enum scenario { FULL, NO_EXTENSION, NO_RUNTIME, NO_PARTIAL, NO_NONUNIFORM, NO_STORAGE, CREATE_FAIL };
static enum scenario scenario;
static int seen_create, seen_query, enabled_extension, enabled_runtime, enabled_partial, enabled_nonuniform, enabled_storage, enabled_extra;
static int supported_runtime, supported_partial, supported_nonuniform, supported_storage;

VKAPI_ATTR VkResult VKAPI_CALL __real_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateDevice(
    VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (scenario != NO_EXTENSION || layer)
        return __real_vkEnumerateDeviceExtensionProperties(physical, layer, count, props);
    uint32_t n = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(physical, layer, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = __real_vkEnumerateDeviceExtensionProperties(physical, layer, &n, all);
    if (r != VK_SUCCESS) { free(all); return r; }
    uint32_t total = 0, written = 0, capacity = props ? *count : 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!strcmp(all[i].extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) continue;
        ++total;
        if (props && written < capacity) props[written++] = all[i];
    }
    free(all);
    *count = props ? written : total;
    return props && written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features) {
    __real_vkGetPhysicalDeviceFeatures2(physical, features);
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)features->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT) continue;
        VkPhysicalDeviceDescriptorIndexingFeaturesEXT *f = (void *)p;
        seen_query = 1;
        if (scenario == NO_RUNTIME) f->runtimeDescriptorArray = VK_FALSE;
        if (scenario == NO_PARTIAL) f->descriptorBindingPartiallyBound = VK_FALSE;
        if (scenario == NO_NONUNIFORM) f->shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
        if (scenario == NO_STORAGE) f->shaderStorageImageArrayNonUniformIndexing = VK_FALSE;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                     const VkAllocationCallbacks *allocator, VkDevice *device) {
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT supported = {0};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 query = {0};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    query.pNext = &supported;
    __real_vkGetPhysicalDeviceFeatures2(physical, &query);
    supported_runtime = supported.runtimeDescriptorArray == VK_TRUE;
    supported_partial = supported.descriptorBindingPartiallyBound == VK_TRUE;
    supported_nonuniform = supported.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    supported_storage = supported.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
    seen_create = 1;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        if (!strcmp(info->ppEnabledExtensionNames[i], VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) enabled_extension = 1;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT) continue;
        const VkPhysicalDeviceDescriptorIndexingFeaturesEXT *f = (const void *)p;
        enabled_runtime = f->runtimeDescriptorArray == VK_TRUE;
        enabled_partial = f->descriptorBindingPartiallyBound == VK_TRUE;
        enabled_nonuniform = f->shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
        enabled_storage = f->shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
        enabled_extra = f->shaderInputAttachmentArrayDynamicIndexing || f->shaderUniformTexelBufferArrayDynamicIndexing ||
            f->shaderStorageTexelBufferArrayDynamicIndexing || f->shaderUniformBufferArrayNonUniformIndexing ||
            f->shaderStorageBufferArrayNonUniformIndexing ||
            f->shaderInputAttachmentArrayNonUniformIndexing || f->shaderUniformTexelBufferArrayNonUniformIndexing ||
            f->shaderStorageTexelBufferArrayNonUniformIndexing || f->descriptorBindingUniformBufferUpdateAfterBind ||
            f->descriptorBindingSampledImageUpdateAfterBind || f->descriptorBindingStorageImageUpdateAfterBind ||
            f->descriptorBindingStorageBufferUpdateAfterBind || f->descriptorBindingUniformTexelBufferUpdateAfterBind ||
            f->descriptorBindingStorageTexelBufferUpdateAfterBind || f->descriptorBindingUpdateUnusedWhilePending ||
            f->descriptorBindingVariableDescriptorCount;
    }
    if (scenario == CREATE_FAIL) return VK_ERROR_INITIALIZATION_FAILED;
    return __real_vkCreateDevice(physical, info, allocator, device);
}

static int run_case(enum scenario which, const char *name) {
    scenario = which;
    seen_create = seen_query = enabled_extension = enabled_runtime = enabled_partial = enabled_nonuniform = enabled_storage = enabled_extra = 0;
    struct s2a_vk_device d;
    char why[256] = "", json[16384];
    int rc = s2a_vk_device_create(&d, why, sizeof why);
    if (which == CREATE_FAIL) {
        int failed = !rc || !seen_create || !enabled_extension || !enabled_runtime || !enabled_partial ||
                     !enabled_nonuniform || !enabled_storage || enabled_extra || d.descriptor_indexing_enabled;
        if (s2a_vk_device_destroy(&d, 0)) failed = 1;
        if (failed) fprintf(stderr, "FAIL %s: failed vkCreateDevice must not record enabled capability\n", name);
        else printf("PASS %s\n", name);
        return failed;
    }
    if (rc) {
        fprintf(stderr, "FAIL %s: device creation: %s\n", name, why);
        s2a_vk_device_destroy(&d, 0);
        return 1;
    }
    int failed = 0;
    if (!seen_create || !supported_runtime || !supported_partial || !supported_nonuniform || !supported_storage) {
        fprintf(stderr, "FAIL %s: reference driver lacks required test features\n", name);
        failed = 1;
    }
    int expected = which == FULL;
    if (seen_query != (which != NO_EXTENSION)) {
        fprintf(stderr, "FAIL %s: capability query presence is wrong (%d)\n", name, seen_query);
        failed = 1;
    }
    if (enabled_extra) {
        fprintf(stderr, "FAIL %s: unrelated descriptor indexing feature was enabled\n", name);
        failed = 1;
    }
    if (enabled_extension != expected || enabled_runtime != expected || enabled_partial != expected ||
        enabled_nonuniform != expected || enabled_storage != expected) {
        fprintf(stderr, "FAIL %s: expected extension/runtime/partial/nonuniform/storage=%d, got %d/%d/%d/%d/%d\n",
                name, expected, enabled_extension, enabled_runtime, enabled_partial, enabled_nonuniform, enabled_storage);
        failed = 1;
    }
    if (d.descriptor_indexing_ext_state != (which != NO_EXTENSION) || d.descriptor_indexing_enabled != expected ||
        d.descriptor_indexing_runtime != (which != NO_EXTENSION && which != NO_RUNTIME) ||
        d.descriptor_indexing_partial != (which != NO_EXTENSION && which != NO_PARTIAL) ||
        d.descriptor_indexing_sampled_nonuniform != (which != NO_EXTENSION && which != NO_NONUNIFORM)) {
        fprintf(stderr, "FAIL %s: recorded support/enabled state is wrong\n", name);
        failed = 1;
    }
    size_t len = 0;
    if (s2a_vk_device_caps_json(&d, json, sizeof json, &len) ||
        (!!strstr(json, "\"VK_EXT_descriptor_indexing\"")) != expected) {
        fprintf(stderr, "FAIL %s: enabledExtensions does not match the actual enabled device extension\n", name);
        failed = 1;
    } else {
        // C1：caps 必须把 A 片记录的支持位与启用位原样带走（编译器 profile 只认这里）。
        char object[320];
        snprintf(object, sizeof object,
                 "\"descriptorIndexing\":{\"enabled\":%s,\"runtimeArray\":%s,\"partiallyBound\":%s,"
                 "\"sampledImageArrayNonUniformIndexing\":%s,\"storageImageArrayNonUniformIndexing\":%s,"
                 "\"variableDescriptorCount\":%s,\"sampledImageUpdateAfterBind\":%s}",
                 d.descriptor_indexing_enabled ? "true" : "false",
                 d.descriptor_indexing_runtime ? "true" : "false",
                 d.descriptor_indexing_partial ? "true" : "false",
                 d.descriptor_indexing_sampled_nonuniform ? "true" : "false",
                 d.descriptor_indexing_storage_nonuniform ? "true" : "false",
                 d.descriptor_indexing_variable_count ? "true" : "false",
                 d.descriptor_indexing_sampled_update_after_bind ? "true" : "false");
        if (!strstr(json, object)) {
            fprintf(stderr, "FAIL %s: descriptorIndexing object does not match the recorded support/enabled bits\n", name);
            failed = 1;
        }
    }
    if (s2a_vk_device_destroy(&d, 0)) { fprintf(stderr, "FAIL %s: device destroy\n", name); failed = 1; }
    if (!failed) printf("PASS %s\n", name);
    return failed;
}

int main(void) {
    int failures = 0;
    failures += run_case(FULL, "full support");
    failures += run_case(NO_EXTENSION, "extension absent");
    failures += run_case(NO_RUNTIME, "runtimeDescriptorArray absent");
    failures += run_case(NO_PARTIAL, "descriptorBindingPartiallyBound absent");
    failures += run_case(NO_NONUNIFORM, "shaderSampledImageArrayNonUniformIndexing absent");
    failures += run_case(NO_STORAGE, "shaderStorageImageArrayNonUniformIndexing absent");
    failures += run_case(CREATE_FAIL, "vkCreateDevice failure");
    return failures ? 1 : 0;
}
