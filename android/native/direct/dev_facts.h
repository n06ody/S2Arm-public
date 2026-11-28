// 设备事实（#1749 设计稿 v3 的 M0：F0 查询事实 + 设备档案）。
//   建设备时（vk_device.c 选定物理设备之后）现查一次：扩展表（三态）、核心与扩展特性、限值、逐格式能力、内存堆与类型、
//   队列族、驱动身份；系统段（SDK、页大小、CPU capacity / 最高频、KGSL、ADPF、显示节拍接口）一次读。
//   只记录、不决策：本批没有任何代码读这些事实去选路径（推导层是 M1）；身份字段（厂商号 / 驱动名 / 机型名）只作记录。
//   落盘成 s2a-device-profile-v1 档案（App 私有目录 files/devprofile/<事实摘要前 16 位>.json，字段白名单见 tools/devprofile_lint.py），
//   并打一行【事实】摘要（同一进程同一代只打一次）。档案按事实摘要分代：驱动 / 系统一变摘要就变；留最近 2 代。摘要输入只有静态事实。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2A_DEV_FACTS_MAX_EXT 512
#define S2A_DEV_FACTS_MAX_QUEUE 16
#define S2A_DEV_FACTS_MAX_CPU 64      // 同 cpu_floor.c read_caps 的上限
#define S2A_DEV_FACTS_FORMATS 141     // dev_facts.c 的格式表项数（编译期核对）

struct s2a_dev_facts {
    // ---- Vulkan：驱动身份与核心查询 ----
    VkPhysicalDeviceProperties props;          // 身份（apiVersion / driverVersion / vendorID / deviceID / deviceType / deviceName / pipelineCacheUUID）+ 限值
    VkPhysicalDeviceFeatures features;         // 物理设备支持的核心特性（不是已启用记录）
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t n_queue;
    VkQueueFamilyProperties queues[S2A_DEV_FACTS_MAX_QUEUE];
    int have_driver;                           // 设备是 1.1+（driverProperties 能查）
    int driver_id;
    char driver_name[VK_MAX_DRIVER_NAME_SIZE];
    // 设备扩展表：ext_state 1 = 完整枚举成功（不在表里 = 确实没有）；-1 = 枚举失败（全部未知，ext_why 写原因）
    int ext_state;
    char ext_why[96];
    uint32_t n_ext;
    char ext[S2A_DEV_FACTS_MAX_EXT][VK_MAX_EXTENSION_NAME_SIZE];
    // 扩展特性（只在对应扩展列出 / 核心版本够时才挂查询结构；have_* = 挂了）
    int have_core11;                           // apiVersion ≥ 1.1：shaderDrawParameters / 16bit_storage / variable_pointers / multiview / maintenance3 / subgroup
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters;
    VkPhysicalDevice16BitStorageFeatures storage_16bit;
    VkPhysicalDeviceVariablePointersFeatures var_ptrs;
    VkPhysicalDeviceMultiviewFeatures multiview;
    int have_timeline; VkPhysicalDeviceTimelineSemaphoreFeatures timeline;
    int have_index_u8; VkPhysicalDeviceIndexTypeUint8FeaturesEXT index_u8;
    int have_4444; VkPhysicalDevice4444FormatsFeaturesEXT f4444;   // 列了 VK_EXT_4444_formats：特性结构只在这时挂链（1.3 只转正了格式，特性结构没转正）
    int have_4444_fmt;                         // 列了扩展或设备 ≥ 1.3：A4B4G4R4 格式可以查
    int have_dcc; VkPhysicalDeviceDepthClipControlFeaturesEXT dcc;
    int have_eds1; VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1;
    int have_eds2; VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2;
    int have_vi; VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vi;
    int have_img_robust; VkPhysicalDeviceImageRobustnessFeaturesEXT img_robust;
    int have_zero_init_wg; VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeaturesKHR zero_init_wg;
    int have_scalar_layout; VkPhysicalDeviceScalarBlockLayoutFeaturesEXT scalar_layout;
    int have_ubo_std; VkPhysicalDeviceUniformBufferStandardLayoutFeaturesKHR ubo_std;
    int have_line_rast; VkPhysicalDeviceLineRasterizationFeaturesEXT line_rast; uint32_t line_subpixel_bits;
    int have_maint4; VkPhysicalDeviceMaintenance4FeaturesKHR maint4; VkDeviceSize max_buffer_size;
    int have_f16i8; VkPhysicalDeviceShaderFloat16Int8FeaturesKHR f16i8;
    int have_placed; VkBool32 placed_map, placed_range, placed_unmap_reserve; VkDeviceSize placed_align;
    int have_border; VkPhysicalDeviceCustomBorderColorFeaturesEXT border; uint32_t border_max;
    int have_di; VkPhysicalDeviceDescriptorIndexingFeaturesEXT di;
    int have_vdiv; VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vdiv; uint32_t vdiv_max;
    int have_swapchain_m1; VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_m1;
    int have_pipe_cache_ctl; VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT pipe_cache_ctl;
    int have_hqr; VkPhysicalDeviceHostQueryResetFeaturesEXT hqr;
    int have_list_restart; VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT list_restart;
    int have_2d_of_3d; VkPhysicalDeviceImage2DViewOf3DFeaturesEXT view_2d_of_3d;
    int have_tba; VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT tba;
    int have_8bit_storage; VkPhysicalDevice8BitStorageFeaturesKHR storage_8bit;
    int have_minmax; VkBool32 minmax_single, minmax_mapping;
    int have_host_import; VkDeviceSize host_align;
    int have_fctrl; VkBool32 fctrl_denorm_preserve_f32, fctrl_denorm_ftz_f32, fctrl_sz_inf_nan_f32, fctrl_rte_f32, fctrl_rtz_f32;
    VkDeviceSize max_alloc; uint32_t max_per_set;            // maintenance3（have_core11）
    uint32_t subgroup_size; VkShaderStageFlags subgroup_stages; VkSubgroupFeatureFlags subgroup_ops; VkBool32 subgroup_quad_all;
    // 逐格式能力：与 s2a_dev_facts_format_name(i) 同序；fmt_have[i] = 查了（扩展格式只在扩展列出时查）
    VkFormatProperties fmt[S2A_DEV_FACTS_FORMATS];
    uint8_t fmt_have[S2A_DEV_FACTS_FORMATS];
    // ---- 系统（附录 B 的现有读法）：未知写 -1 / 空 ----
    int sdk;                                   // ro.build.version.sdk（= Build.VERSION.SDK_INT）；-1 = 读不到 / 非安卓
    long page_size;                            // sysconf(_SC_PAGESIZE)（plat_android.c plat_page_size 同读法）；≤0 = 不可用
    int ncpu;                                  // _SC_NPROCESSORS_CONF，不合理（≤0 或 >64）= 0（cpu_floor.c read_caps 同规则）
    uint32_t capacity[S2A_DEV_FACTS_MAX_CPU];  // /sys/devices/system/cpu/cpu<i>/cpu_capacity，单核读不到 = 0
    uint32_t max_khz[S2A_DEV_FACTS_MAX_CPU];   // cpufreq/cpuinfo_max_freq（硬件上限；不读会被温控 / 省电改的 scaling_max_freq），读不到 = 0
    int kgsl;                                  // /dev/kgsl-3d0：1 有 / 0 没有 / -1 判不了（只 stat，不打开、不 ioctl）
    int adpf;                                  // APerformanceHint_* 五个符号（adpf_hint.c backend_init 同一组）：1 齐 / 0 不齐 / -1 非安卓
    int vsync_api;                             // nvn_vsync_android.c ops_resolve 同一组符号：33 / 29 / 24 / 0 没有 / -1 非安卓
    // 机型（只给人看，推导不读；Q4 保留）：ro.product.manufacturer / model、ro.soc.model、ro.build.version.release；空 = 读不到
    int have_model;
    char manufacturer[96], model[96], soc_model[96], android_release[96];
};

// 现查一台物理设备的全部事实（不建设备、不改任何状态）。0 = 成功；-1 = 参数错。
int s2a_dev_facts_collect(VkPhysicalDevice physical, struct s2a_dev_facts *out);
// 事实 → s2a-device-profile-v1（紧凑 JSON，键按字典序，与 tools/devprofile_lint.py 的规范化写法逐字节一致）。
//   digest_hex 写 factsDigest 的 64 位 hex（不含 "sha256-"）；返回 0 成功，-1 缓冲不够。
int s2a_dev_profile_json(const struct s2a_dev_facts *facts, char *out, size_t cap, size_t *len, char digest_hex[65]);
// 写 <files_dir>/devprofile/<digest 前 16 位>.json（临时文件 + fsync + rename；同一代已在则只刷修改时间），
//   再把同目录更旧的代删到只剩最近 2 代（<16 位 hex>.json，别的名字不碰）。*written = 1 新写 / 0 同一代已在；*removed = 删掉的旧代数。返回 0 成功，否则 errno（why 写原因）。
int s2a_dev_profile_store(const char *files_dir, const char *json, size_t len, const char *digest_hex,
                          int *written, int *removed, char *why, size_t why_size);
// 一行【事实】摘要（不带换行）。
void s2a_dev_facts_summary(const struct s2a_dev_facts *facts, const char *digest_hex, char *out, size_t cap);
// vk_device.c 建设备时调（弱引用）：现查 → 落盘 → 打摘要。档案目录取 App files 目录（bridge.cpp s2a_android_gpu_driver_files_dir）。
//   返回 1 = 本次打了【事实】行；0 = 本进程这一代已打过（或分配失败）。
int s2a_dev_facts_on_device_create(VkPhysicalDevice physical);
// 格式表（测试核对「生产会查 / 会映射的格式都在表里」用）
const char *s2a_dev_facts_format_name(size_t i, VkFormat *format);

#ifdef __cplusplus
}
#endif
