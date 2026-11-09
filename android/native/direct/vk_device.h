// C3：native Vulkan 设备 owner（Android 直连首版）。
// 只按设备实际 advertise/feature 准入；所需能力缺失时明确失败，不猜别家驱动或静默降级。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct s2a_vk_device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    char driver_name[128];
    char driver_info[128];
    int has_android_surface;
    int has_swapchain;
    // VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT（Vulkan 1.1 / VK_KHR_maintenance1）：
    // 3D 纹理的 depth slice 只能通过该标志建 2D/2D_ARRAY 视图（附件/采样）。
    int has_2d_array_compatible;
    int has_swapchain_maintenance1;
    int swapchain_maintenance1_enabled;
    int timeline_semaphore_enabled;
    int index_type_uint8_enabled;
    int shader_draw_parameters_enabled;
    // 0 = 没有校准扩展；1 = VK_KHR_calibrated_timestamps；2 = VK_EXT_calibrated_timestamps。
    // GPU 时间戳换算到游戏时钟需要用它求 GPU 域与 CLOCK_MONOTONIC 的偏移。
    int calibrated_timestamps;
    VkPhysicalDeviceFeatures core_features;
    uint32_t device_extension_count;
    // #1199：逐格式 BC 能力位（bit i 对应 NVN 格式 0x41+i）。
    // native=optimal-tiling 有 SAMPLED_IMAGE；decode=要走 compute 解码（native 补集，force 时全 1）。
    uint32_t bc_native_mask;
    uint32_t bc_decode_mask;
    int bc_decode_forced;   // S2A_VK_BC_DECODE=force（仅 debug 构建生效）
    // VK_EXT_external_memory_host：把 host 指针（guest 内存池直映射地址）导入成 VkBuffer 内存的零拷贝路径。
    // 只有驱动 advertise 且 debug.s2a.host_import=1 时才启用；未启用时行为与之前完全一致。
    int has_external_memory_host;
    int external_memory_host_enabled;
    VkDeviceSize host_pointer_alignment;
    // 同一块的 Android 共享路径：AHB 导入（guest 与 Vulkan 共用后备页）。同样只在显式要求时启用。
    int has_ahb_import;
    int has_queue_family_foreign;   // 导入外部内存时当前队列族要能用，按规范与 AHB 成对启用
    int ahb_import_enabled;
    // B2：VK_GOOGLE_display_timing——present 带 presentID、回读每帧实际上屏时刻。
    int has_display_timing;
};

// 创建 instance/physical/device/queue/command pool。why 写 ASCII 原因。
int s2a_vk_device_create(struct s2a_vk_device *out, char *why, size_t why_size);
// 返回实际 VkResult；失败（包括 DEVICE_LOST）不证明退休。
int s2a_vk_device_wait_idle(struct s2a_vk_device *device);
// idle_proven=1 仅用于同一次销毁中已取得 VK_SUCCESS、且未再提交工作的调用者。
// 0 则自己等待一次；失败返回 -1，保留 device/command pool/instance。
int s2a_vk_device_destroy(struct s2a_vk_device *device, int idle_proven);
uint32_t s2a_vk_memory_type(const struct s2a_vk_device *device, uint32_t type_bits,
                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags *actual);

// 诊断：把一段 host 指针（guest 内存池的直映射地址）导入成 VkBuffer 内存，验证零拷贝是否可行。
// 自建并自毁临时 buffer/内存，不改动任何运行状态；host_import 未启用时返回 -1 并写 why。
// 成功写 0，why 里给出对齐/内存类型等实测值，供上层判断能否真的用它替换每笔 memcpy。
int s2a_vk_probe_host_import(struct s2a_vk_device *device, const void *host, size_t len,
                             char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
