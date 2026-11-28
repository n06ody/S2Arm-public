// 整台设备能力形态档（shapes.py 从 android/tests/devprofiles/*.json 出的各档）在 C 侧的样子；假驱动 fake_vk.c 按 S2A_CAPS_SHAPE 选一档回答 Vulkan 查询。
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

struct cs_fmt { int32_t format; uint32_t linear, optimal, buffer; };
struct cs_shape {
    const char *name;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;
    // 扩展特性结构里的位（shapes.py EXT_FEATURES；di* = 描述符索引支持位）
    int shaderDrawParameters, timelineSemaphore, indexTypeUint8, customBorderColors, customBorderColorWithoutFormat,
        vertexAttributeInstanceRateDivisor, vertexAttributeInstanceRateZeroDivisor,
        formatA4B4G4R4, formatA4R4G4B4, depthClipControl, scalarBlockLayout, uniformBufferStandardLayout, maintenance4, shaderFloat16, shaderInt8, swapchainMaintenance1, pipelineCreationCacheControl, hostQueryReset, primitiveTopologyListRestart,
        image2DViewOf3D, sampler2DViewOf3D, texelBufferAlignment, robustImageAccess,
        shaderZeroInitializeWorkgroupMemory,
        rectangularLines, bresenhamLines, smoothLines,
        stippledRectangularLines, stippledBresenhamLines, stippledSmoothLines,
        storageBuffer8BitAccess, uniformAndStorageBuffer8BitAccess, storagePushConstant8,
        storageBuffer16BitAccess, uniformAndStorageBuffer16BitAccess, storagePushConstant16, storageInputOutput16,
        variablePointersStorageBuffer, variablePointers,
        multiview, multiviewGeometryShader, multiviewTessellationShader,
        diRuntimeArray, diPartiallyBound, diSampledNonUniform, diStorageNonUniform, diVariableCount, diSampledUpdateAfterBind;
    int extendedDynamicState, extendedDynamicState2, extendedDynamicState2LogicOp,
        extendedDynamicState2PatchControlPoints, vertexInputDynamicState;
    int driver_id;
    const char *driver_name;
    uint64_t max_alloc, host_align, max_buffer_size;
    uint32_t max_per_set, subgroup_size, subgroup_stages, subgroup_ops;
    uint32_t subgroup_quad_all, border_max, divisor_max, minmax_single, minmax_mapping, line_subpixel_bits;
    uint32_t fctrl_denorm_preserve_f32, fctrl_denorm_ftz_f32, fctrl_sz_inf_nan_f32, fctrl_rte_f32, fctrl_rtz_f32;
    uint32_t n_heap; struct { uint64_t size; uint32_t flags; } heaps[VK_MAX_MEMORY_HEAPS];
    uint32_t n_type; struct { uint32_t flags, heap; } types[VK_MAX_MEMORY_TYPES];
    uint32_t n_queue; struct { uint32_t flags, count, timestamp_bits; } queues[8];
    uint32_t n_ext; const char *const *ext;
    uint32_t n_fmt; const struct cs_fmt *fmt;
};

// fake_vk.c：本进程选中的档（S2A_CAPS_SHAPE 不认识就退出 2）
const struct cs_shape *cs_shape(void);
int cs_has_ext(const struct cs_shape *s, const char *name);
VkFormatProperties cs_format(const struct cs_shape *s, VkFormat f);
// 假驱动在 vkCreateDevice 时核「启用的 ⊆ 支持的」（真驱动在这里回 FEATURE_NOT_PRESENT / EXTENSION_NOT_PRESENT）；不符时这里写原因
extern char cs_create_device_why[256];
extern uint32_t cs_enabled_ext_count;
enum { CS_MAX_ENABLED_EXT = 64 };
extern const char *cs_enabled_ext[CS_MAX_ENABLED_EXT];
// 假驱动记下的每次 vkGetPhysicalDeviceFormatProperties 查询（去重；测试核对它们都在设备事实的格式表里）
// vkGetPhysicalDeviceFeatures2 / Properties2 被调了几次（测试清零后核）
extern uint32_t cs_features2_calls, cs_properties2_calls;
extern uint32_t cs_queried_fmt_count;
extern VkFormat cs_queried_fmt[256];
// VK_EXT_global_priority 测试钩子：cs_deny_high_priority=1 时对 HIGH 队列返回 VK_ERROR_NOT_PERMITTED_EXT
extern int cs_deny_high_priority;
extern uint32_t cs_create_device_calls;
extern VkQueueGlobalPriorityEXT cs_queue_global_priority;
