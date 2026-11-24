// 假 Vulkan 驱动（宿主）：按 S2A_CAPS_SHAPE 选中的整台设备形态档回答 android/native/direct/vk_device.c / device_caps.c / dev_facts.c 用到的全部查询。
//   读数全部来自 shapes.py 从设备档案（android/tests/devprofiles/*.json）出的头，这里不写任何厂商分支。
//   vkCreateDevice 像真驱动一样核「启用的扩展 / 特性 ⊆ 设备支持的」：后端没看能力就启用，就在这里红（原因写进 cs_create_device_why）。
//   档案里没有读数（null / 没这一项）的扩展特性 / 属性一律按「无」回答（shapes.py 文件头）。
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "caps_shape.h"
#include "caps_shapes_gen.h"

char cs_create_device_why[256];
uint32_t cs_enabled_ext_count;
const char *cs_enabled_ext[32];
uint32_t cs_queried_fmt_count;
uint32_t cs_features2_calls, cs_properties2_calls;
VkFormat cs_queried_fmt[256];

const struct cs_shape *cs_shape(void) {
    static const struct cs_shape *picked;
    if (picked) return picked;
    const char *want = getenv("S2A_CAPS_SHAPE");
    for (size_t i = 0; want && i < sizeof cs_all / sizeof *cs_all; i++)
        if (!strcmp(cs_all[i]->name, want)) return picked = cs_all[i];
    fprintf(stderr, "S2A_CAPS_SHAPE=%s 不是已知形态档（shapes.py names）\n", want ? want : "(未设)");
    exit(2);
}
int cs_has_ext(const struct cs_shape *s, const char *name) {
    for (uint32_t i = 0; i < s->n_ext; i++) if (!strcmp(s->ext[i], name)) return 1;
    return 0;
}
VkFormatProperties cs_format(const struct cs_shape *s, VkFormat f) {
    VkFormatProperties p = {0};
    for (uint32_t i = 0; i < s->n_fmt; i++)
        if (s->fmt[i].format == (int32_t)f) { p.linearTilingFeatures = s->fmt[i].linear; p.optimalTilingFeatures = s->fmt[i].optimal; p.bufferFeatures = s->fmt[i].buffer; }
    return p;
}

#define H(x) ((void *)(uintptr_t)(x))
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *v) { *v = VK_API_VERSION_1_3; return VK_SUCCESS; }
static VkResult fill_ext(const char *const *names, uint32_t n, uint32_t *count, VkExtensionProperties *out) {
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t k = *count < n ? *count : n;
    for (uint32_t i = 0; i < k; i++) { memset(&out[i], 0, sizeof out[i]); snprintf(out[i].extensionName, sizeof out[i].extensionName, "%s", names[i]); out[i].specVersion = 1; }
    *count = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *out) {
    (void)layer; return fill_ext(cs_instance_ext, sizeof cs_instance_ext / sizeof *cs_instance_ext, count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *out) { (void)out; *count = 0; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    (void)p; (void)layer; const struct cs_shape *s = cs_shape(); return fill_ext(s->ext, s->n_ext, count, out);
}
static uint32_t cs_app_api = VK_API_VERSION_1_0;   // 被测代码建实例时申请的版本（VkApplicationInfo.apiVersion）
// 有效版本 = 应用申请的版本与设备版本取低：更高核心版本才有的入口，两者都够才合法（否则只能靠扩展提供）
static uint32_t cs_eff_api(const struct cs_shape *s) { return s->props.apiVersion < cs_app_api ? s->props.apiVersion : cs_app_api; }
static int cs_any_ext(const struct cs_shape *s, const char *a, const char *b) { return cs_has_ext(s, a) || (b && cs_has_ext(s, b)); }
static void cs_reject(const struct cs_shape *s, const char *what) {
    fprintf(stderr, "FAIL: %s 超出有效版本 / 扩展（应用 1.%u、设备 1.%u，没列能提供它的扩展）—— 规范不允许\n", what,
            VK_API_VERSION_MINOR(cs_app_api), VK_API_VERSION_MINOR(s->props.apiVersion));
    abort();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)a;
    cs_app_api = ci->pApplicationInfo && ci->pApplicationInfo->apiVersion ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        int ok = 0;
        for (size_t k = 0; k < sizeof cs_instance_ext / sizeof *cs_instance_ext; k++) ok |= !strcmp(cs_instance_ext[k], ci->ppEnabledExtensionNames[i]);
        if (!ok) return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    *out = (VkInstance)H(0x1000); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance i, uint32_t *count, VkPhysicalDevice *out) {
    (void)i; if (out && *count) out[0] = (VkPhysicalDevice)H(0x2000); *count = 1; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p, VkPhysicalDeviceProperties *out) { (void)p; *out = cs_shape()->props; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice p, VkPhysicalDeviceFeatures *out) { (void)p; *out = cs_shape()->features; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties *out) {
    (void)p; const struct cs_shape *s = cs_shape(); memset(out, 0, sizeof *out);
    out->memoryHeapCount = s->n_heap; out->memoryTypeCount = s->n_type;
    for (uint32_t i = 0; i < s->n_heap; i++) { out->memoryHeaps[i].size = s->heaps[i].size; out->memoryHeaps[i].flags = s->heaps[i].flags; }
    for (uint32_t i = 0; i < s->n_type; i++) { out->memoryTypes[i].propertyFlags = s->types[i].flags; out->memoryTypes[i].heapIndex = s->types[i].heap; }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice p, uint32_t *count, VkQueueFamilyProperties *out) {
    (void)p; const struct cs_shape *s = cs_shape();
    if (!out) { *count = s->n_queue; return; }
    uint32_t k = *count < s->n_queue ? *count : s->n_queue;
    for (uint32_t i = 0; i < k; i++) { memset(&out[i], 0, sizeof out[i]); out[i].queueFlags = s->queues[i].flags; out[i].queueCount = s->queues[i].count; out[i].timestampValidBits = s->queues[i].timestamp_bits; }
    *count = k;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *out) {
    if (f == VK_FORMAT_A4B4G4R4_UNORM_PACK16 && !cs_has_ext(cs_shape(), "VK_EXT_4444_formats") && cs_eff_api(cs_shape()) < VK_API_VERSION_1_3)
        cs_reject(cs_shape(), "A4B4G4R4 格式查询（1.3 核心 / VK_EXT_4444_formats）");
    (void)p; *out = cs_format(cs_shape(), f);
    uint32_t i = 0;
    while (i < cs_queried_fmt_count && cs_queried_fmt[i] != f) i++;
    if (i == cs_queried_fmt_count && i < 256) cs_queried_fmt[cs_queried_fmt_count++] = f;
}

// 特性结构准入：有效版本够（转正进核心的），或列了能提供它的扩展（EXT / KHR 同一结构）才能挂进链
static const char *feature_struct_bad(int st, const struct cs_shape *s) {
    const uint32_t eff = cs_eff_api(s);
    switch (st) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT:   // 1.3 只转正了格式枚举，特性结构没转正
        return cs_has_ext(s, "VK_EXT_4444_formats") ? NULL : "4444 特性结构（VK_EXT_4444_formats）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT:
        return cs_any_ext(s, "VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8") || eff >= VK_API_VERSION_1_4 ? NULL : "index_type_uint8 特性结构（VK_EXT/KHR_index_type_uint8）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT:
        return cs_has_ext(s, "VK_EXT_depth_clip_control") ? NULL : "depth_clip_control 特性结构（VK_EXT_depth_clip_control）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
        return cs_has_ext(s, "VK_EXT_custom_border_color") ? NULL : "custom_border_color 特性结构（VK_EXT_custom_border_color）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT:
        return cs_any_ext(s, "VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor") || eff >= VK_API_VERSION_1_4 ? NULL : "vertex_attribute_divisor 特性结构（VK_EXT/KHR_vertex_attribute_divisor）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT:
        return cs_any_ext(s, "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1") ? NULL : "swapchain_maintenance1 特性结构（VK_EXT/KHR_swapchain_maintenance1）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) ? NULL : "EDS1 特性结构缺扩展";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) ? NULL : "EDS2 特性结构缺扩展";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) ? NULL : "动态顶点输入特性结构缺扩展";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
        return cs_has_ext(s, "VK_KHR_timeline_semaphore") || eff >= VK_API_VERSION_1_2 ? NULL : "timeline 特性结构（1.2 核心 / VK_KHR_timeline_semaphore）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        return cs_has_ext(s, "VK_EXT_descriptor_indexing") || eff >= VK_API_VERSION_1_2 ? NULL : "descriptor_indexing 特性结构（1.2 核心 / VK_EXT_descriptor_indexing）";
    default: return NULL;
    }
}
// 扩展特性结构：只有设备列出了扩展才答它的位。越过有效版本 / 扩展挂进链 = 规范不允许（真驱动多半忽略、位停在 0），直接判红，
//   不像真平台那样安静地答 0 把错误读数放过去（#1879 复审：4444 结构、index_uint8、应用 1.1 下按设备版本挂 1.2 结构）
static void features_chain(void *next, int for_create, const struct cs_shape *s) {
    for (VkBaseOutStructure *b = next; b; b = b->pNext) {
        const char *bad = feature_struct_bad((int)b->sType, s);
        if (bad) cs_reject(s, bad);
        int ok = 1; const char *what = NULL;
#define BIT(field, have, name) do { if (for_create) { if ((field) && !(have)) { ok = 0; what = (name); } } else (field) = (have) ? VK_TRUE : VK_FALSE; } while (0)
        switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
            BIT(((VkPhysicalDeviceShaderDrawParametersFeatures *)b)->shaderDrawParameters, s->shaderDrawParameters, "shaderDrawParameters"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
            BIT(((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *)b)->extendedDynamicState,
                s->extendedDynamicState, "extendedDynamicState"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: {
            VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *eds2 = (void *)b;
            BIT(eds2->extendedDynamicState2, s->extendedDynamicState2, "extendedDynamicState2");
            BIT(eds2->extendedDynamicState2LogicOp, s->extendedDynamicState2LogicOp, "extendedDynamicState2LogicOp");
            BIT(eds2->extendedDynamicState2PatchControlPoints, s->extendedDynamicState2PatchControlPoints,
                "extendedDynamicState2PatchControlPoints"); break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT:
            BIT(((VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *)b)->vertexInputDynamicState,
                s->vertexInputDynamicState, "vertexInputDynamicState"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            BIT(((VkPhysicalDeviceTimelineSemaphoreFeatures *)b)->timelineSemaphore, s->timelineSemaphore && cs_has_ext(s, "VK_KHR_timeline_semaphore"), "timelineSemaphore"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT:
            BIT(((VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)b)->indexTypeUint8, s->indexTypeUint8 && cs_any_ext(s, "VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8"), "indexTypeUint8"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT:
            BIT(((VkPhysicalDevice4444FormatsFeaturesEXT *)b)->formatA4B4G4R4, s->formatA4B4G4R4 && cs_has_ext(s, "VK_EXT_4444_formats"), "formatA4B4G4R4");
            BIT(((VkPhysicalDevice4444FormatsFeaturesEXT *)b)->formatA4R4G4B4, s->formatA4R4G4B4 && cs_has_ext(s, "VK_EXT_4444_formats"), "formatA4R4G4B4"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT:
            BIT(((VkPhysicalDeviceDepthClipControlFeaturesEXT *)b)->depthClipControl, s->depthClipControl && cs_has_ext(s, "VK_EXT_depth_clip_control"), "depthClipControl"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT *c = (void *)b; const int e = cs_has_ext(s, "VK_EXT_custom_border_color");
            BIT(c->customBorderColors, s->customBorderColors && e, "customBorderColors");
            BIT(c->customBorderColorWithoutFormat, s->customBorderColorWithoutFormat && e, "customBorderColorWithoutFormat"); break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
            VkPhysicalDeviceDescriptorIndexingFeatures *d = (void *)b; const int e = cs_has_ext(s, "VK_EXT_descriptor_indexing");
            BIT(d->runtimeDescriptorArray, s->diRuntimeArray && e, "runtimeDescriptorArray");
            BIT(d->descriptorBindingPartiallyBound, s->diPartiallyBound && e, "descriptorBindingPartiallyBound");
            BIT(d->shaderSampledImageArrayNonUniformIndexing, s->diSampledNonUniform && e, "shaderSampledImageArrayNonUniformIndexing");
            BIT(d->shaderStorageImageArrayNonUniformIndexing, s->diStorageNonUniform && e, "shaderStorageImageArrayNonUniformIndexing");
            BIT(d->descriptorBindingVariableDescriptorCount, s->diVariableCount && e, "descriptorBindingVariableDescriptorCount");
            BIT(d->descriptorBindingSampledImageUpdateAfterBind, s->diSampledUpdateAfterBind && e, "descriptorBindingSampledImageUpdateAfterBind"); break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: {
            VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *v = (void *)b; const int e = cs_any_ext(s, "VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor");
            BIT(v->vertexAttributeInstanceRateDivisor, s->vertexAttributeInstanceRateDivisor && e, "vertexAttributeInstanceRateDivisor");
            BIT(v->vertexAttributeInstanceRateZeroDivisor, s->vertexAttributeInstanceRateZeroDivisor && e, "vertexAttributeInstanceRateZeroDivisor"); break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT:
            // 档案里的位（真机档：device_caps.c 只在特性也开着时把扩展写进 enabledExtensions，转换时据此记 1）
            BIT(((VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT *)b)->swapchainMaintenance1, s->swapchainMaintenance1 && cs_any_ext(s, "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1"), "swapchainMaintenance1"); break;
        default: break;   // 别的结构（placed 映射等）：没有读数，保持调用方清零的 0（= 无）
        }
#undef BIT
        if (!ok) { snprintf(cs_create_device_why, sizeof cs_create_device_why, "启用了设备不支持的扩展特性 %s（形态档 %s）", what, s->name); }
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) {
    (void)p; cs_features2_calls++; f->features = cs_shape()->features; features_chain(f->pNext, 0, cs_shape());
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *out) {
    (void)p; cs_properties2_calls++; const struct cs_shape *s = cs_shape();
    out->properties = s->props;
    for (VkBaseOutStructure *b = out->pNext; b; b = b->pNext) {
        switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
            if (!cs_has_ext(s, "VK_KHR_driver_properties") && cs_eff_api(s) < VK_API_VERSION_1_2) cs_reject(s, "driver properties 属性结构（1.2 核心 / VK_KHR_driver_properties）");
            VkPhysicalDeviceDriverProperties *d = (void *)b;
            d->driverID = (VkDriverId)s->driver_id;
            snprintf(d->driverName, sizeof d->driverName, "%s", s->driver_name);
            snprintf(d->driverInfo, sizeof d->driverInfo, "caps shape %s", s->name); break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:
            ((VkPhysicalDeviceMaintenance3Properties *)b)->maxMemoryAllocationSize = s->max_alloc;
            ((VkPhysicalDeviceMaintenance3Properties *)b)->maxPerSetDescriptors = s->max_per_set; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
            ((VkPhysicalDeviceSubgroupProperties *)b)->subgroupSize = s->subgroup_size;
            ((VkPhysicalDeviceSubgroupProperties *)b)->supportedStages = s->subgroup_stages;
            ((VkPhysicalDeviceSubgroupProperties *)b)->supportedOperations = s->subgroup_ops;
            ((VkPhysicalDeviceSubgroupProperties *)b)->quadOperationsInAllStages = s->subgroup_quad_all; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT:
            if (!cs_has_ext(s, "VK_EXT_custom_border_color")) cs_reject(s, "自定义边框颜色属性缺扩展");
            ((VkPhysicalDeviceCustomBorderColorPropertiesEXT *)b)->maxCustomBorderColorSamplers = s->border_max; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT:
            if (!cs_has_ext(s, "VK_EXT_vertex_attribute_divisor")) cs_reject(s, "EXT 实例除数属性缺扩展");
            ((VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)b)->maxVertexAttribDivisor = s->divisor_max; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES_EXT:
            if (!cs_has_ext(s, "VK_EXT_sampler_filter_minmax")) cs_reject(s, "采样器 minmax 属性缺扩展");
            ((VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT *)b)->filterMinmaxSingleComponentFormats = s->minmax_single;
            ((VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT *)b)->filterMinmaxImageComponentMapping = s->minmax_mapping; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT:
            if (!cs_has_ext(s, "VK_EXT_external_memory_host")) cs_reject(s, "主机内存导入属性缺扩展");
            ((VkPhysicalDeviceExternalMemoryHostPropertiesEXT *)b)->minImportedHostPointerAlignment = s->host_align; break;
        default: break;   // 没有读数的结构（如 placed 对齐）保持调用方清零的 0。
        }
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)p; (void)a; const struct cs_shape *s = cs_shape();
    cs_create_device_why[0] = 0; cs_enabled_ext_count = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (cs_enabled_ext_count < 32) cs_enabled_ext[cs_enabled_ext_count++] = ci->ppEnabledExtensionNames[i];
        if (!cs_has_ext(s, ci->ppEnabledExtensionNames[i])) {
            snprintf(cs_create_device_why, sizeof cs_create_device_why, "启用了设备没列出的扩展 %s（形态档 %s）", ci->ppEnabledExtensionNames[i], s->name);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (ci->pEnabledFeatures) {
        const VkBool32 *want = (const VkBool32 *)ci->pEnabledFeatures, *have = (const VkBool32 *)&s->features;
        for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
            if (want[i] && !have[i]) {
                snprintf(cs_create_device_why, sizeof cs_create_device_why, "启用了设备不支持的核心特性 %s（形态档 %s）", cs_feature_name(i), s->name);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
    }
    features_chain((void *)ci->pNext, 1, s);
    if (cs_create_device_why[0]) return VK_ERROR_FEATURE_NOT_PRESENT;
    *out = (VkDevice)H(0x3000); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice d, const VkAllocationCallbacks *a) { (void)d; (void)a; }
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice d) { (void)d; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice d, uint32_t f, uint32_t i, VkQueue *q) { (void)d; (void)f; (void)i; *q = (VkQueue)H(0x4000); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    (void)d; (void)ci; (void)a; *out = (VkCommandPool)(uintptr_t)0x5000; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice d, VkCommandPool c, const VkAllocationCallbacks *a) { (void)d; (void)c; (void)a; }
static VKAPI_ATTR void VKAPI_CALL fake_draw_count(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t m, uint32_t st) {
    (void)c; (void)b; (void)o; (void)cb; (void)co; (void)m; (void)st;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char *name) {
    (void)d;
    if (!strcmp(name, "vkCmdDrawIndirectCountKHR") || !strcmp(name, "vkCmdDrawIndexedIndirectCountKHR")) return (PFN_vkVoidFunction)fake_draw_count;
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char *name) {
    (void)i;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2;
    return NULL;
}
// 下面这些只为链接（主机内存导入探针 / 等栅栏）：本测试不走到，走到就是测试自己错了
#define NOT_HERE(name) do { fprintf(stderr, "fake_vk: %s 不该被调到\n", name); abort(); } while (0)
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *c, const VkAllocationCallbacks *a, VkBuffer *b) { (void)d; (void)c; (void)a; (void)b; NOT_HERE("vkCreateBuffer"); }
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)b; (void)a; NOT_HERE("vkDestroyBuffer"); }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r) { (void)d; (void)b; (void)r; NOT_HERE("vkGetBufferMemoryRequirements"); }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *m) { (void)d; (void)i; (void)a; (void)m; NOT_HERE("vkAllocateMemory"); }
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; NOT_HERE("vkFreeMemory"); }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; NOT_HERE("vkBindBufferMemory"); }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void **p) { (void)d; (void)m; (void)o; (void)s; (void)f; (void)p; NOT_HERE("vkMapMemory"); }
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; NOT_HERE("vkUnmapMemory"); }
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) { (void)d; (void)n; (void)f; (void)all; (void)t; NOT_HERE("vkWaitForFences"); }
