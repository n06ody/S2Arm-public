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
const char *cs_enabled_ext[CS_MAX_ENABLED_EXT];
uint32_t cs_queried_fmt_count;
uint32_t cs_features2_calls, cs_properties2_calls;
VkFormat cs_queried_fmt[256];
int cs_deny_high_priority;
uint32_t cs_create_device_calls;
VkQueueGlobalPriorityEXT cs_queue_global_priority;

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
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
        return cs_has_ext(s, "VK_EXT_image_robustness") || eff >= VK_API_VERSION_1_3 ? NULL : "image_robustness 特性结构（1.3 核心 / VK_EXT_image_robustness）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES_KHR:
        return cs_has_ext(s, "VK_KHR_zero_initialize_workgroup_memory") || eff >= VK_API_VERSION_1_3 ? NULL : "zero_initialize_workgroup_memory 特性结构（1.3 核心 / VK_KHR_zero_initialize_workgroup_memory）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT:
        return cs_has_ext(s, "VK_EXT_scalar_block_layout") || eff >= VK_API_VERSION_1_2 ? NULL : "scalar_block_layout 特性结构（1.2 核心 / VK_EXT_scalar_block_layout）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES_KHR:
        return cs_has_ext(s, "VK_KHR_uniform_buffer_standard_layout") || eff >= VK_API_VERSION_1_2 ? NULL : "uniform_buffer_standard_layout 特性结构（1.2 核心 / VK_KHR_uniform_buffer_standard_layout）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT:
        return cs_any_ext(s, "VK_EXT_line_rasterization", "VK_KHR_line_rasterization") ? NULL : "line_rasterization 特性结构（VK_EXT/KHR_line_rasterization）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR:
        return cs_has_ext(s, VK_KHR_MAINTENANCE_4_EXTENSION_NAME) || eff >= VK_API_VERSION_1_3 ? NULL : "maintenance4 特性结构（1.3 核心 / VK_KHR_maintenance4）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR:
        return cs_has_ext(s, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) || eff >= VK_API_VERSION_1_2 ? NULL : "shader_float16_int8 特性结构（1.2 核心 / VK_KHR_shader_float16_int8）";
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
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME) ? NULL : "image_2d_view_of_3d 特性结构（VK_EXT_image_2d_view_of_3d）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME) ? NULL : "texel_buffer_alignment 特性结构（VK_EXT_texel_buffer_alignment）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
        return cs_has_ext(s, "VK_KHR_timeline_semaphore") || eff >= VK_API_VERSION_1_2 ? NULL : "timeline 特性结构（1.2 核心 / VK_KHR_timeline_semaphore）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        return cs_has_ext(s, "VK_EXT_descriptor_indexing") || eff >= VK_API_VERSION_1_2 ? NULL : "descriptor_indexing 特性结构（1.2 核心 / VK_EXT_descriptor_indexing）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME) || eff >= VK_API_VERSION_1_3
            ? NULL : "pipeline_creation_cache_control 特性结构（1.3 核心 / VK_EXT_pipeline_creation_cache_control）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) || eff >= VK_API_VERSION_1_2 ? NULL : "host_query_reset 特性结构（1.2 核心 / VK_EXT_host_query_reset）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT:
        return cs_has_ext(s, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME) ? NULL : "list_restart 特性结构（VK_EXT_primitive_topology_list_restart）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR:
        return cs_has_ext(s, VK_KHR_8BIT_STORAGE_EXTENSION_NAME) || eff >= VK_API_VERSION_1_2 ? NULL : "8bit_storage 特性结构（1.2 核心 / VK_KHR_8bit_storage）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
        return eff >= VK_API_VERSION_1_1 || cs_has_ext(s, "VK_KHR_16bit_storage") ? NULL : "16bit_storage 特性结构（1.1 核心 / VK_KHR_16bit_storage）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
        return eff >= VK_API_VERSION_1_1 || cs_has_ext(s, "VK_KHR_variable_pointers") ? NULL : "variable_pointers 特性结构（1.1 核心 / VK_KHR_variable_pointers）";
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
        return eff >= VK_API_VERSION_1_1 || cs_has_ext(s, "VK_KHR_multiview") ? NULL : "multiview 特性结构（1.1 核心 / VK_KHR_multiview）";
    default: return NULL;
    }
}
// 扩展特性结构：只有设备列出了扩展才答它的位。越过有效版本 / 扩展挂进链 = 规范不允许（真驱动多半忽略、位停在 0），直接判红，
//   不像真平台那样安静地答 0 把错误读数放过去（#1879 复审：4444 结构、index_uint8、应用 1.1 下按设备版本挂 1.2 结构）
static void features_chain(void *next, int for_create, const struct cs_shape *s, const VkPhysicalDeviceFeatures *en_core) {
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
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
            VkPhysicalDevice16BitStorageFeatures *s16 = (void *)b;
            const int ok11 = cs_eff_api(s) >= VK_API_VERSION_1_1 || cs_has_ext(s, "VK_KHR_16bit_storage");
            BIT(s16->storageBuffer16BitAccess, s->storageBuffer16BitAccess && ok11, "storageBuffer16BitAccess");
            BIT(s16->uniformAndStorageBuffer16BitAccess, s->uniformAndStorageBuffer16BitAccess && ok11, "uniformAndStorageBuffer16BitAccess");
            BIT(s16->storagePushConstant16, s->storagePushConstant16 && ok11, "storagePushConstant16");
            BIT(s16->storageInputOutput16, s->storageInputOutput16 && ok11, "storageInputOutput16");
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
            VkPhysicalDeviceVariablePointersFeatures *vp = (void *)b;
            const int ok11 = cs_eff_api(s) >= VK_API_VERSION_1_1 || cs_has_ext(s, "VK_KHR_variable_pointers");
            if (for_create && vp->variablePointers && !vp->variablePointersStorageBuffer) {
                ok = 0; what = "variablePointers(requires variablePointersStorageBuffer)";
            }
            BIT(vp->variablePointersStorageBuffer, s->variablePointersStorageBuffer && ok11, "variablePointersStorageBuffer");
            BIT(vp->variablePointers, s->variablePointers && ok11, "variablePointers");
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
            VkPhysicalDeviceMultiviewFeatures *mv = (void *)b;
            const int ok11 = cs_eff_api(s) >= VK_API_VERSION_1_1 || cs_has_ext(s, "VK_KHR_multiview");
            if (for_create && mv->multiviewGeometryShader && (!mv->multiview || !en_core || !en_core->geometryShader)) {
                ok = 0; what = "multiviewGeometryShader(requires multiview + geometryShader)";
            }
            if (for_create && mv->multiviewTessellationShader && (!mv->multiview || !en_core || !en_core->tessellationShader)) {
                ok = 0; what = "multiviewTessellationShader(requires multiview + tessellationShader)";
            }
            BIT(mv->multiview, s->multiview && ok11, "multiview");
            BIT(mv->multiviewGeometryShader, s->multiviewGeometryShader && ok11, "multiviewGeometryShader");
            BIT(mv->multiviewTessellationShader, s->multiviewTessellationShader && ok11, "multiviewTessellationShader");
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            BIT(((VkPhysicalDeviceTimelineSemaphoreFeatures *)b)->timelineSemaphore, s->timelineSemaphore && cs_has_ext(s, "VK_KHR_timeline_semaphore"), "timelineSemaphore"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT:
            BIT(((VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)b)->indexTypeUint8, s->indexTypeUint8 && cs_any_ext(s, "VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8"), "indexTypeUint8"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT:
            BIT(((VkPhysicalDevice4444FormatsFeaturesEXT *)b)->formatA4B4G4R4, s->formatA4B4G4R4 && cs_has_ext(s, "VK_EXT_4444_formats"), "formatA4B4G4R4");
            BIT(((VkPhysicalDevice4444FormatsFeaturesEXT *)b)->formatA4R4G4B4, s->formatA4R4G4B4 && cs_has_ext(s, "VK_EXT_4444_formats"), "formatA4R4G4B4"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT:
            BIT(((VkPhysicalDeviceDepthClipControlFeaturesEXT *)b)->depthClipControl, s->depthClipControl && cs_has_ext(s, "VK_EXT_depth_clip_control"), "depthClipControl"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
            BIT(((VkPhysicalDeviceImageRobustnessFeaturesEXT *)b)->robustImageAccess, s->robustImageAccess && cs_has_ext(s, "VK_EXT_image_robustness"), "robustImageAccess"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES_KHR:
            BIT(((VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeaturesKHR *)b)->shaderZeroInitializeWorkgroupMemory, s->shaderZeroInitializeWorkgroupMemory && cs_has_ext(s, "VK_KHR_zero_initialize_workgroup_memory"), "shaderZeroInitializeWorkgroupMemory"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT:
            BIT(((VkPhysicalDeviceScalarBlockLayoutFeaturesEXT *)b)->scalarBlockLayout, s->scalarBlockLayout && cs_has_ext(s, "VK_EXT_scalar_block_layout"), "scalarBlockLayout"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES_KHR:
            BIT(((VkPhysicalDeviceUniformBufferStandardLayoutFeaturesKHR *)b)->uniformBufferStandardLayout, s->uniformBufferStandardLayout && cs_has_ext(s, "VK_KHR_uniform_buffer_standard_layout"), "uniformBufferStandardLayout"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT: {
            VkPhysicalDeviceLineRasterizationFeaturesEXT *lr = (void *)b;
            const int e = cs_any_ext(s, "VK_EXT_line_rasterization", "VK_KHR_line_rasterization");
            BIT(lr->rectangularLines, s->rectangularLines && e, "rectangularLines");
            BIT(lr->bresenhamLines, s->bresenhamLines && e, "bresenhamLines");
            BIT(lr->smoothLines, s->smoothLines && e, "smoothLines");
            BIT(lr->stippledRectangularLines, s->stippledRectangularLines && e, "stippledRectangularLines");
            BIT(lr->stippledBresenhamLines, s->stippledBresenhamLines && e, "stippledBresenhamLines");
            BIT(lr->stippledSmoothLines, s->stippledSmoothLines && e, "stippledSmoothLines"); break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR:
            BIT(((VkPhysicalDeviceMaintenance4FeaturesKHR *)b)->maintenance4, s->maintenance4 && cs_has_ext(s, VK_KHR_MAINTENANCE_4_EXTENSION_NAME), "maintenance4"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR: {
            VkPhysicalDeviceShaderFloat16Int8FeaturesKHR *sf = (void *)b; const int e = cs_has_ext(s, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
            BIT(sf->shaderFloat16, s->shaderFloat16 && e, "shaderFloat16");
            BIT(sf->shaderInt8, s->shaderInt8 && e, "shaderInt8"); break; }
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
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT:
            BIT(((VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT *)b)->pipelineCreationCacheControl,
                s->pipelineCreationCacheControl && cs_has_ext(s, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME),
                "pipelineCreationCacheControl"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT:
            BIT(((VkPhysicalDeviceHostQueryResetFeaturesEXT *)b)->hostQueryReset, s->hostQueryReset && cs_has_ext(s, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME), "hostQueryReset"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT:
            BIT(((VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *)b)->primitiveTopologyListRestart, s->primitiveTopologyListRestart && cs_has_ext(s, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME), "primitiveTopologyListRestart"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT: {
            VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *v = (void *)b;
            const int e = cs_has_ext(s, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME);
            BIT(v->image2DViewOf3D, s->image2DViewOf3D && e, "image2DViewOf3D");
            BIT(v->sampler2DViewOf3D, s->sampler2DViewOf3D && e, "sampler2DViewOf3D"); break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT:
            BIT(((VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT *)b)->texelBufferAlignment, s->texelBufferAlignment && cs_has_ext(s, VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME), "texelBufferAlignment"); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR: {
            VkPhysicalDevice8BitStorageFeaturesKHR *s8 = (void *)b;
            const int e = cs_has_ext(s, VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
            BIT(s8->storageBuffer8BitAccess, s->storageBuffer8BitAccess && e, "storageBuffer8BitAccess");
            BIT(s8->uniformAndStorageBuffer8BitAccess, s->uniformAndStorageBuffer8BitAccess && e, "uniformAndStorageBuffer8BitAccess");
            BIT(s8->storagePushConstant8, s->storagePushConstant8 && e, "storagePushConstant8");
            break;
        }
        default: break;   // 别的结构（placed 映射等）：没有读数，保持调用方清零的 0（= 无）
        }
#undef BIT
        if (!ok) { snprintf(cs_create_device_why, sizeof cs_create_device_why, "启用了设备不支持的扩展特性 %s（形态档 %s）", what, s->name); }
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) {
    (void)p; cs_features2_calls++; f->features = cs_shape()->features; features_chain(f->pNext, 0, cs_shape(), NULL);
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
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT: {
            if (!cs_has_ext(s, VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME)) cs_reject(s, "texel_buffer_alignment 属性缺扩展");
            VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *tp = (void *)b;
            tp->storageTexelBufferOffsetAlignmentBytes = 16;
            tp->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            tp->uniformTexelBufferOffsetAlignmentBytes = 16;
            tp->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE; break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT:
            if (!cs_any_ext(s, "VK_EXT_line_rasterization", "VK_KHR_line_rasterization")) cs_reject(s, "线光栅化属性缺扩展");
            ((VkPhysicalDeviceLineRasterizationPropertiesEXT *)b)->lineSubPixelPrecisionBits = s->line_subpixel_bits; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES_KHR:
            if (!cs_has_ext(s, VK_KHR_MAINTENANCE_4_EXTENSION_NAME) && cs_eff_api(s) < VK_API_VERSION_1_3) cs_reject(s, "maintenance4 属性缺扩展");
            ((VkPhysicalDeviceMaintenance4PropertiesKHR *)b)->maxBufferSize = s->max_buffer_size; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR: {
            if (!cs_has_ext(s, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME) && cs_eff_api(s) < VK_API_VERSION_1_2)
                cs_reject(s, "float_controls 属性缺扩展（1.2 核心 / VK_KHR_shader_float_controls）");
            VkPhysicalDeviceFloatControlsPropertiesKHR *fc = (void *)b;
            fc->shaderDenormPreserveFloat32 = s->fctrl_denorm_preserve_f32;
            fc->shaderDenormFlushToZeroFloat32 = s->fctrl_denorm_ftz_f32;
            fc->shaderSignedZeroInfNanPreserveFloat32 = s->fctrl_sz_inf_nan_f32;
            fc->shaderRoundingModeRTEFloat32 = s->fctrl_rte_f32;
            fc->shaderRoundingModeRTZFloat32 = s->fctrl_rtz_f32; break; }
        default: break;   // 没有读数的结构（如 placed 对齐）保持调用方清零的 0。
        }
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)p; (void)a; const struct cs_shape *s = cs_shape();
    cs_create_device_calls++;
    cs_create_device_why[0] = 0; cs_enabled_ext_count = 0; cs_queue_global_priority = (VkQueueGlobalPriorityEXT)0;
    int enabled_gp_ext = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (cs_enabled_ext_count < CS_MAX_ENABLED_EXT) cs_enabled_ext[cs_enabled_ext_count++] = ci->ppEnabledExtensionNames[i];
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) enabled_gp_ext = 1;
        if (!cs_has_ext(s, ci->ppEnabledExtensionNames[i])) {
            snprintf(cs_create_device_why, sizeof cs_create_device_why, "启用了设备没列出的扩展 %s（形态档 %s）", ci->ppEnabledExtensionNames[i], s->name);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    for (uint32_t q = 0; q < ci->queueCreateInfoCount; q++) {
        for (const VkBaseInStructure *b = ci->pQueueCreateInfos[q].pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT) {
                if (q != 0) cs_reject(s, "global_priority 只能请求主队列");
                if (!cs_has_ext(s, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME) || !enabled_gp_ext)
                    cs_reject(s, "global_priority 队列结构缺扩展 VK_EXT_global_priority");
                cs_queue_global_priority = ((const VkDeviceQueueGlobalPriorityCreateInfoEXT *)b)->globalPriority;
                if (cs_deny_high_priority && cs_queue_global_priority == VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT)
                    return VK_ERROR_NOT_PERMITTED_EXT;
            }
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
    features_chain((void *)ci->pNext, 1, s, ci->pEnabledFeatures);
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
static VKAPI_ATTR void VKAPI_CALL fake_dyn_cmd(void) {}
static VKAPI_ATTR void VKAPI_CALL fake_reset_query_pool(VkDevice d, VkQueryPool qp, uint32_t first, uint32_t count) {
    (void)d; (void)qp; (void)first; (void)count;
}
static VKAPI_ATTR void VKAPI_CALL fake_set_line_stipple(VkCommandBuffer c, uint32_t f, uint16_t p) { (void)c; (void)f; (void)p; }
static VKAPI_ATTR void VKAPI_CALL fake_dev_buf_mr(VkDevice d, const VkDeviceBufferMemoryRequirements *i, VkMemoryRequirements2 *o) {
    (void)d; (void)i; (void)o;
}
static VKAPI_ATTR void VKAPI_CALL fake_dev_img_mr(VkDevice d, const VkDeviceImageMemoryRequirements *i, VkMemoryRequirements2 *o) {
    (void)d; (void)i; (void)o;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char *name) {
    (void)d;
    const struct cs_shape *s = cs_shape();
    if (!strcmp(name, "vkCmdDrawIndirectCountKHR") || !strcmp(name, "vkCmdDrawIndexedIndirectCountKHR")) return (PFN_vkVoidFunction)fake_draw_count;
    if (cs_has_ext(s, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) &&
        (!strcmp(name, "vkCmdSetCullModeEXT") || !strcmp(name, "vkCmdSetFrontFaceEXT") ||
         !strcmp(name, "vkCmdSetPrimitiveTopologyEXT") || !strcmp(name, "vkCmdSetDepthTestEnableEXT") ||
         !strcmp(name, "vkCmdSetDepthWriteEnableEXT") || !strcmp(name, "vkCmdSetDepthCompareOpEXT") ||
         !strcmp(name, "vkCmdSetStencilTestEnableEXT") || !strcmp(name, "vkCmdSetStencilOpEXT") ||
         !strcmp(name, "vkCmdBindVertexBuffers2EXT")))
        return (PFN_vkVoidFunction)fake_dyn_cmd;
    if (cs_has_ext(s, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) &&
        (!strcmp(name, "vkCmdSetRasterizerDiscardEnableEXT") || !strcmp(name, "vkCmdSetDepthBiasEnableEXT") ||
         !strcmp(name, "vkCmdSetPrimitiveRestartEnableEXT")))
        return (PFN_vkVoidFunction)fake_dyn_cmd;
    if (cs_has_ext(s, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) && !strcmp(name, "vkCmdSetVertexInputEXT"))
        return (PFN_vkVoidFunction)fake_dyn_cmd;
    if (!strcmp(name, "vkResetQueryPoolEXT")) return (PFN_vkVoidFunction)fake_reset_query_pool;
    if (!strcmp(name, "vkCmdSetLineStippleEXT")) return (PFN_vkVoidFunction)fake_set_line_stipple;
    if (!strcmp(name, "vkGetDeviceBufferMemoryRequirementsKHR")) return (PFN_vkVoidFunction)fake_dev_buf_mr;
    if (!strcmp(name, "vkGetDeviceImageMemoryRequirementsKHR")) return (PFN_vkVoidFunction)fake_dev_img_mr;
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
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d, const VkImageCreateInfo *c, const VkAllocationCallbacks *a, VkImage *i) { (void)d; (void)c; (void)a; (void)i; NOT_HERE("vkCreateImage"); }
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)i; (void)a; NOT_HERE("vkDestroyImage"); }
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice d, VkImage i, VkMemoryRequirements *r) { (void)d; (void)i; (void)r; NOT_HERE("vkGetImageMemoryRequirements"); }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *m) { (void)d; (void)i; (void)a; (void)m; NOT_HERE("vkAllocateMemory"); }
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; NOT_HERE("vkFreeMemory"); }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; NOT_HERE("vkBindBufferMemory"); }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void **p) { (void)d; (void)m; (void)o; (void)s; (void)f; (void)p; NOT_HERE("vkMapMemory"); }
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; NOT_HERE("vkUnmapMemory"); }
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) { (void)d; (void)n; (void)f; (void)all; (void)t; NOT_HERE("vkWaitForFences"); }
