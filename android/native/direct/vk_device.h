// C3：native Vulkan 设备 owner（Android 直连首版）。
// 只按设备实际 advertise/feature 准入；所需能力缺失时明确失败，不猜别家驱动或静默降级。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

// 直连实例申请的 Vulkan 版本（VkApplicationInfo.apiVersion）。用更高核心版本才有的物理设备功能，要应用申请的版本和设备版本都够：
//   有效版本 = 两者较低者；不够就只认对应扩展（Khronos：VkApplicationInfo 描述、初始化一章的 physical-device 扩展段）。
#define S2A_VK_APP_API_VERSION VK_API_VERSION_1_1
static inline uint32_t s2a_vk_effective_api(uint32_t device_api) { return device_api < S2A_VK_APP_API_VERSION ? device_api : S2A_VK_APP_API_VERSION; }

// 等栅栏，按墙钟算时限。有的驱动在等待被信号打断时不接着等，直接返回 VK_TIMEOUT（#697：天玑 9000+ / Mali-G710 r32p1 上，
// 一次计算解码的提交之后 vkWaitForFences(5 秒) 不到 1 毫秒就返回超时、栅栏其实稍后才触发；等待期间屏蔽本线程的异步信号后正常）。
// 我们的进程里信号很多（写跟踪的缺页、游戏线程的挂起），所以超时返回不能当真：没到墙钟时限就接着等。
// 返回值同 vkWaitForFences：VK_SUCCESS / 真到时限的 VK_TIMEOUT / 设备错误原样。
VkResult s2a_vk_wait_fences(VkDevice device, uint32_t n, const VkFence *fences, uint64_t timeout_ns);

#ifdef __cplusplus
extern "C" {
#endif

// EXT 入口按 Vulkan 1.1 显式协商，设备和录制队列共用同一份真头文件声明。
#define S2A_VK_EDS1_COMMANDS(X) \
    X(vkCmdSetCullModeEXT) X(vkCmdSetFrontFaceEXT) X(vkCmdSetPrimitiveTopologyEXT) \
    X(vkCmdSetDepthTestEnableEXT) X(vkCmdSetDepthWriteEnableEXT) X(vkCmdSetDepthCompareOpEXT) \
    X(vkCmdSetStencilTestEnableEXT) X(vkCmdSetStencilOpEXT) X(vkCmdBindVertexBuffers2EXT)
#define S2A_VK_EDS2_COMMANDS(X) \
    X(vkCmdSetRasterizerDiscardEnableEXT) X(vkCmdSetDepthBiasEnableEXT) X(vkCmdSetPrimitiveRestartEnableEXT)
#define S2A_VK_VERTEX_INPUT_COMMANDS(X) X(vkCmdSetVertexInputEXT)

struct s2a_vk_device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkQueue sparse_queue;
    VkCommandPool command_pool;
    char driver_name[128];
    char driver_info[128];
    int driver_id;   // VkDriverId（VkPhysicalDeviceDriverProperties.driverID）：区分厂商驱动与 Mesa 实现
    int has_android_surface;
    int has_swapchain;
    // VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT（Vulkan 1.1 / VK_KHR_maintenance1）：
    // 3D 纹理的 depth slice 只能通过该标志建 2D/2D_ARRAY 视图（附件/采样）。
    int has_2d_array_compatible;
    int has_swapchain_maintenance1;
    int swapchain_maintenance1_enabled;
    int timeline_semaphore_enabled;
    // VK_KHR_timeline_semaphore 运行时入口（经 vkGetDeviceProcAddr 取，严守 Vulkan 1.1 底线零静态 1.2 符号）
    PFN_vkGetSemaphoreCounterValueKHR timeline_get_counter;
    PFN_vkWaitSemaphoresKHR timeline_wait;
    // VK_KHR_synchronization2：细粒度管线阶段（COPY / CLEAR / BLIT / RESOLVE / INDEX_INPUT / VERTEX_ATTRIBUTE_INPUT / NONE）
    // 与分离存取掩码；Vulkan 1.1 下通过 KHR 扩展启用并按 vkGetDeviceProcAddr 取 vkCmdPipelineBarrier2KHR。
    int synchronization2_enabled;
    PFN_vkCmdPipelineBarrier2KHR cmd_pipeline_barrier2;
    int index_type_uint8_enabled;
    int draw_indirect_count_enabled; // KHR extension actually enabled, both entry points present
    // #977：EXT 或 KHR vertex_attribute_divisor 的 vertexAttributeInstanceRateDivisor 已启用；
    // max_vertex_attrib_divisor 是设备上限（未启用时为 0）。
    int vertex_divisor_enabled;
    int vertex_divisor_nonzero_first_instance;
    uint32_t max_vertex_attrib_divisor;
    int eds1_enabled, eds2_enabled, vertex_input_dynamic_enabled;
#define S2A_VK_PROC(name) PFN_##name name;
    S2A_VK_EDS1_COMMANDS(S2A_VK_PROC)
    S2A_VK_EDS2_COMMANDS(S2A_VK_PROC)
    S2A_VK_VERTEX_INPUT_COMMANDS(S2A_VK_PROC)
#undef S2A_VK_PROC
    PFN_vkCmdDrawIndirectCountKHR draw_indirect_count;
    PFN_vkCmdDrawIndexedIndirectCountKHR draw_indexed_indirect_count;
    // Vulkan 1.1 核心描述子更新模板（0 扩展依赖，9000 / 9400 / 8E 全覆盖）：
    // 运行时经 vkGetDeviceProcAddr 解析 3 个入口，避免低 API libvulkan.so stub 静态符号依赖。
    int desc_update_template_enabled;
    PFN_vkCreateDescriptorUpdateTemplate create_desc_update_template;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_desc_update_template;
    PFN_vkUpdateDescriptorSetWithTemplate update_desc_set_with_template;
    int shader_draw_parameters_enabled;
    // VK_KHR_sampler_mirror_clamp_to_edge 已启用：采样器可用 MIRROR_CLAMP_TO_EDGE（实例按 1.1 建，用不到 1.2 核心里的同名特性）。
    int sampler_mirror_clamp_enabled;
    // Vulkan 1.1 下显式启用 EXT；只读附件用 STORE_OP_NONE 避免隐式写。
    int load_store_op_none_enabled;
    // #14：VK_EXT_attachment_feedback_loop_layout 已启用且 attachmentFeedbackLoopLayout 为真（保持 Vulkan 1.1 底线，不支持时回落快照池）。
    int attachment_feedback_loop_enabled;
    // VK_KHR_push_descriptor 已启用且 maxPushDescriptors > 0、入口存在（8E / 9400 支持，9000 零退化回落）。
    int push_descriptor_enabled;
    uint32_t max_push_descriptors;
    PFN_vkCmdPushDescriptorSetKHR cmd_push_descriptor_set;
    int separate_depth_stencil_layouts_enabled;
    // VK_EXT_host_query_reset：主机侧重置查询池，省去命令缓冲内 vkCmdResetQueryPool 与 report_ts 的 init_cb 全管线屏障。
    // 入口仅由 vkGetDeviceProcAddr("vkResetQueryPoolEXT") 取，不引入静态 Vulkan 1.2 符号。
    int host_query_reset_enabled;
    PFN_vkResetQueryPoolEXT reset_query_pool;
    // VK_KHR_8bit_storage：允许 SSBO / UBO+SSBO / PushConstant 直接读写 8 位整数（Vulkan 1.1 靠扩展按位协商）。
    int storage_8bit_ssbo_enabled;
    int storage_8bit_ubo_ssbo_enabled;
    int storage_8bit_push_enabled;
    // Vulkan 1.1 核心特性 VkPhysicalDevice16BitStorageFeatures：允许 SSBO / UBO+SSBO / PushConstant / Stage IO 直接读写 16 位值。
    int storage_16bit_ssbo_enabled;
    int storage_16bit_ubo_ssbo_enabled;
    int storage_16bit_push_enabled;
    int storage_16bit_io_enabled;
    // Vulkan 1.1 核心特性 VkPhysicalDeviceVariablePointersFeatures：允许 StorageBuffer / Workgroup 可变指针。
    int variable_pointers_storage_buffer_enabled;
    int variable_pointers_enabled;
    // Vulkan 1.1 核心特性 VkPhysicalDeviceMultiviewFeatures：单次渲染通道多视角渲染（viewMask / gl_ViewIndex）。
    int multiview_enabled;
    int multiview_geometry_shader_enabled;
    int multiview_tessellation_shader_enabled;
    int pipeline_creation_feedback_enabled; // 逻辑设备已启用 EXT，或有效 Vulkan 版本已达 1.3
    int pipeline_creation_cache_control_enabled; // VK_EXT_pipeline_creation_cache_control + pipelineCreationCacheControl 已启用（非阻塞缓存命中探测）
    int image_format_list_enabled;   // 启用了 VK_KHR_image_format_list（应用 apiVersion 1.1，1.2 核心的格式列表结构要靠这条扩展；保 Adreno UBWC 用）
    // 启用了 VK_KHR_imageless_framebuffer（依赖 VK_KHR_image_format_list + imagelessFramebuffer 特性）：
    // 按兼容性签名复用单个无图 VkFramebuffer，开通道时经 VkRenderPassAttachmentBeginInfo 传入实绑视图，消除 RT/DS 换视图时的 FBO 缓存抖动。
    int imageless_framebuffer_enabled;
    // VK_EXT_custom_border_color 已启用且 customBorderColors 特性开着：CLAMP_TO_BORDER 的采样器可用任意边框色（#976）。
    // custom_border_without_format：customBorderColorWithoutFormat 也开着，建采样器时可以不给视图格式。
    // custom_border_max：maxCustomBorderColorSamplers，同时存活的自定义边框色 VkSampler 个数上限。
    int custom_border_enabled;
    int custom_border_without_format;
    uint32_t custom_border_max;
    // #976：VK_EXT_sampler_filter_minmax 已启用（采样器 min / max 归约）；minmax_image_mapping = filterMinmaxImageComponentMapping
    // （0 时仅 R 分量有定义，未证明分量使用前拒绝归约）。
    int sampler_minmax_enabled;
    int minmax_image_mapping;
    // 0 = 没有校准扩展；1 = VK_KHR_calibrated_timestamps；2 = VK_EXT_calibrated_timestamps。
    // GPU 时间戳换算到游戏时钟需要用它求 GPU 域与 CLOCK_MONOTONIC 的偏移。
    int calibrated_timestamps;
    VkPhysicalDeviceFeatures core_features;
    uint32_t device_extension_count;
    // descriptor indexing 仅建可选设备能力；编译器的 non-uniform profile 仍独立关闭。
    // ext_state: -1 枚举失败（未知），0 未列出，1 列出。后续位为物理设备支持，不等于启用。
    int descriptor_indexing_ext_state, descriptor_indexing_enabled;
    int device_fault_ext_state;  // 仅诊断：1 已列出，0 未列出，-1 枚举失败
    int descriptor_indexing_runtime, descriptor_indexing_partial, descriptor_indexing_sampled_nonuniform;
    int descriptor_indexing_storage_nonuniform, descriptor_indexing_variable_count, descriptor_indexing_sampled_update_after_bind;
    // #1199：逐格式 BC 能力位（bit i 对应 NVN 格式 0x41+i）。
    // native=optimal-tiling 有 SAMPLED_IMAGE；decode=要走 compute 解码（native 补集，force 时全 1）。
    uint32_t bc_native_mask;
    uint32_t bc_decode_mask;
    // #398：深度格式替换位（NVN_VK_DEPTH_SUBST_*，nvn_vk_format.h）。0 = D24 原样可用。
    uint32_t depth_subst_mask;
    // #975：VK_EXT_4444_formats 已启用（formatA4B4G4R4）；16 位打包格式走退回格式的位（nvn_vk_format.h k_p16 序）。
    int formats_4444_enabled;
    // VK_EXT_depth_clip_control 已启用（depthClipControl）：管线可把裁剪深度设成 [-1, 1]（nvn_backend.h NVN_GPU_CAP_DEPTH_CLIP_CTL）
    int depth_clip_control_enabled;
    int provoking_vertex_last_enabled;  // LAST 特性和遍内逐管线切换均可用
    // VK_EXT_primitive_topology_list_restart 已启用（primitiveTopologyListRestart）：
    // 列表图元（POINT/LINE/TRIANGLE_LIST 及邻接列表）支持 NVN SetPrimitiveRestart 哨兵重启。
    int primitive_topology_list_restart_enabled;
    // VK_EXT_image_2d_view_of_3d：允许 3D 贴图的单级单层 2D 视图用于存储图像（image2DViewOf3D）或采样（sampler2DViewOf3D）。
    int image_2d_view_of_3d_enabled;
    int sampler_2d_view_of_3d_enabled;
    // VK_EXT_texel_buffer_alignment 已启用（texelBufferAlignment）：纹素缓冲视图可按规范使用逐格式 / 单纹素对齐
    int texel_buffer_alignment_enabled;
    VkDeviceSize storage_texel_align_bytes;
    int storage_texel_single_align;
    VkDeviceSize uniform_texel_align_bytes;
    int uniform_texel_single_align;
    // VK_EXT_image_robustness 已启用（robustImageAccess=VK_TRUE，debug.s2a.nvk_image_robustness=0 可关）：
    // 采样与存储图像越界读返回规范零值、越界写安全丢弃（Vulkan 1.1 基线下经扩展协商）。
    int image_robustness_enabled;
    // VK_KHR_zero_initialize_workgroup_memory 已启用（shaderZeroInitializeWorkgroupMemory=VK_TRUE，
    // debug.s2a.nvk_zero_init_workgroup=0 可关）：允许 Workgroup 共享内存 OpVariable 带 OpConstantNull 零初始化器。
    int zero_init_workgroup_enabled;
    // VK_EXT_scalar_block_layout 已启用（scalarBlockLayout=VK_TRUE，debug.s2a.nvk_scalar_block_layout=0 可关）：
    // 允许 UBO/SSBO 成员与数组按标量对齐紧密排布（Vulkan 1.1 基线下经扩展协商，无静态 1.2 依赖）。
    int scalar_block_layout_enabled;
    // VK_KHR_uniform_buffer_standard_layout 已启用（uniformBufferStandardLayout=VK_TRUE，
    // debug.s2a.nvk_ubo_std_layout=0 可关）：允许 UBO 使用与 SSBO 相同的 std430 紧凑布局（Vulkan 1.1 基线下经扩展协商）。
    int ubo_standard_layout_enabled;
    // VK_EXT_line_rasterization 已启用（debug.s2a.nvk_line_raster=0 可关）：
    // 精确矩形/Bresenham/平滑线光栅化与动态虚线入口（vkGetDeviceProcAddr 解析，无静态 EXT 符号）。
    int line_raster_enabled;
    int line_raster_rectangular, line_raster_bresenham, line_raster_smooth;
    int line_raster_stippled_rectangular, line_raster_stippled_bresenham, line_raster_stippled_smooth;
    uint32_t line_subpixel_bits;
    PFN_vkCmdSetLineStippleEXT cmd_set_line_stipple;
    // #1070 第 2 步探针：VK_EXT_map_memory_placed（把设备内存映射到指定 CPU 地址，池镜像零拷贝要它）。只查不启用：
    //   placed_ext = 设备列出了扩展；placed_map / range / unmap_reserve = 三项特性；placed_align = minPlacedMemoryMapAlignment；
    //   map_memory2_ext = 列出 VK_KHR_map_memory2（placed 依赖它）；max_alloc = maxMemoryAllocationSize（大池要按它拆段）
    int placed_ext, placed_map, placed_range, placed_unmap_reserve, map_memory2_ext;
    uint64_t placed_align, max_alloc;
    uint32_t p16_fallback_mask;
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
    // VK_KHR_maintenance4（Vulkan 1.1 基线可选协商；开关 debug.s2a.nvk_maintenance4 缺省 1）：
    // 允许 SPIR-V 1.3 LocalSizeId、放宽着色器接口匹配、查询 maxBufferSize，并经 vkGetDeviceProcAddr 动态解析
    // vkGetDeviceBufferMemoryRequirementsKHR / vkGetDeviceImageMemoryRequirementsKHR（0 静态 1.3/KHR 符号）。
    int maintenance4_enabled;
    VkDeviceSize max_buffer_size;
    PFN_vkGetDeviceBufferMemoryRequirementsKHR get_device_buffer_memory_requirements;
    PFN_vkGetDeviceImageMemoryRequirementsKHR get_device_image_memory_requirements;
    // VK_KHR_shader_float16_int8（Vulkan 1.1 基线可选协商；开关 debug.s2a.nvk_shader_float16_int8 缺省 1）：
    // 按实际支持位分别启用 shaderFloat16 / shaderInt8，供 SPIR-V 合规预检（SPV_REQ_FLOAT16 / SPV_REQ_INT8）放行。
    int shader_float16_enabled;
    int shader_int8_enabled;
    // VK_KHR_shader_float_controls（Vulkan 1.1 基线可选协商；开关 debug.s2a.nvk_shader_float_controls 缺省 1）：
    // float_controls_enabled：扩展已启用；float_controls_mask：15 位浮点控制能力掩码
    // （bit = mode_idx * 3 + width_idx；mode_idx: 0=DenormPreserve, 1=DenormFlushToZero,
    //  2=SignedZeroInfNanPreserve, 3=RoundingModeRTE, 4=RoundingModeRTZ；width_idx: 0=16, 1=32, 2=64）。
    int float_controls_enabled;
    uint32_t float_controls_mask;
    // VK_EXT_global_priority（Vulkan 1.1 基线可选协商；开关 debug.s2a.nvk_global_priority 缺省 0 以便真机 A/B 测帧时抖动）：
    // has_global_priority：设备列出扩展；global_priority_enabled：成功以 HIGH 建队列；
    // global_priority_not_permitted：请求 HIGH 时驱动/内核返回 VK_ERROR_NOT_PERMITTED_EXT，已自动降级重试默认队列成功；
    // queue_global_priority：实际生效队列优先级（HIGH 或 MEDIUM）。
    int has_global_priority;
    int global_priority_enabled;
    int global_priority_not_permitted;
    VkQueueGlobalPriorityEXT queue_global_priority;
};

// 查询缓冲区 / 图像内存需求：启用 VK_KHR_maintenance4 时直走 vkGetDevice{Buffer,Image}MemoryRequirementsKHR
// 免建临时对象；未启用时回退 Vulkan 1.1 临时对象查询。返回 VK_SUCCESS 或底层创建失败的 VkResult。
VkResult s2a_vk_buffer_memory_requirements(const struct s2a_vk_device *device,
                                           const VkBufferCreateInfo *info,
                                           VkMemoryRequirements *out);
VkResult s2a_vk_image_memory_requirements(const struct s2a_vk_device *device,
                                          const VkImageCreateInfo *info,
                                          VkMemoryRequirements *out);

// #975：查格式特性（代替 vkGetPhysicalDeviceFormatProperties）。A4B4G4R4_EXT 不发查询，按 VK_EXT_4444_formats
// formatA4B4G4R4 特性保证的特性取值（格式表只在启用该特性时映射到它）：设备按 Vulkan 1.1 建，验证层把设备扩展加的
// 格式当非法枚举（VUID-vkGetPhysicalDeviceFormatProperties-format-parameter）。其余格式照常查询。
void s2a_vk_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *out);
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
