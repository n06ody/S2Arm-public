// C3：native Vulkan 设备 owner（Android 直连首版）。
// 只按设备实际 advertise/feature 准入；所需能力缺失时明确失败，不猜别家驱动或静默降级。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

// 等栅栏，按墙钟算时限。有的驱动在等待被信号打断时不接着等，直接返回 VK_TIMEOUT（#697：天玑 9000+ / Mali-G710 r32p1 上，
// 一次计算解码的提交之后 vkWaitForFences(5 秒) 不到 1 毫秒就返回超时、栅栏其实稍后才触发；等待期间屏蔽本线程的异步信号后正常）。
// 我们的进程里信号很多（写跟踪的缺页、游戏线程的挂起），所以超时返回不能当真：没到墙钟时限就接着等。
// 返回值同 vkWaitForFences：VK_SUCCESS / 真到时限的 VK_TIMEOUT / 设备错误原样。
VkResult s2a_vk_wait_fences(VkDevice device, uint32_t n, const VkFence *fences, uint64_t timeout_ns);

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
    int draw_indirect_count_enabled; // KHR extension actually enabled, both entry points present
    PFN_vkCmdDrawIndirectCountKHR draw_indirect_count;
    PFN_vkCmdDrawIndexedIndirectCountKHR draw_indexed_indirect_count;
    int shader_draw_parameters_enabled;
    // VK_KHR_sampler_mirror_clamp_to_edge 已启用：采样器可用 MIRROR_CLAMP_TO_EDGE（实例按 1.1 建，用不到 1.2 核心里的同名特性）。
    int sampler_mirror_clamp_enabled;
    // #976：VK_EXT_sampler_filter_minmax 已启用（采样器 min / max 归约）；minmax_image_mapping = filterMinmaxImageComponentMapping
    // （0 时做归约的视图 r 分量必须恒等映射）。
    int sampler_minmax_enabled;
    int minmax_image_mapping;
    // 0 = 没有校准扩展；1 = VK_KHR_calibrated_timestamps；2 = VK_EXT_calibrated_timestamps。
    // GPU 时间戳换算到游戏时钟需要用它求 GPU 域与 CLOCK_MONOTONIC 的偏移。
    int calibrated_timestamps;
    VkPhysicalDeviceFeatures core_features;
    uint32_t device_extension_count;
    // #1199：逐格式 BC 能力位（bit i 对应 NVN 格式 0x41+i）。
    // native=optimal-tiling 有 SAMPLED_IMAGE；decode=要走 compute 解码（native 补集，force 时全 1）。
    uint32_t bc_native_mask;
    uint32_t bc_decode_mask;
    // #398：深度格式替换位（NVN_VK_DEPTH_SUBST_*，nvn_vk_format.h）。0 = D24 原样可用。
    uint32_t depth_subst_mask;
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
