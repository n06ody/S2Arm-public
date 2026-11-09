#define VK_USE_PLATFORM_ANDROID_KHR 1
#include "vk_device.h"
#include "vk_membuf.h"
#include "gs_path.h"
#include "../capprobe/capprobe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <android/log.h>
#define S2A_DLOG(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#else
#define S2A_DLOG(...) fprintf(stderr, __VA_ARGS__)
#endif

// 诊断开关读取：Android 走 debug.s2a.* 属性，宿主探针走环境变量（同一个名字）。
static int prop_int(const char *name, int def) {
#if defined(__ANDROID__)
    char b[PROP_VALUE_MAX] = {0};
    return __system_property_get(name, b) > 0 ? atoi(b) : def;
#else
    const char *e = getenv(name);
    return e ? atoi(e) : def;
#endif
}

static int fail(char *why, size_t n, const char *text) {
    if (why && n) snprintf(why, n, "%s", text);
    return -1;
}

static int has_instance_extension(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *ext = calloc(count ? count : 1, sizeof *ext);
    if (!ext) return 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, ext);
    int found = 0;
    for (uint32_t i = 0; i < count; i++) if (!strcmp(ext[i].extensionName, name)) { found = 1; break; }
    free(ext);
    return found;
}

static int has_device_extension(VkPhysicalDevice physical, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *ext = calloc(count ? count : 1, sizeof *ext);
    if (!ext) return 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &count, ext);
    int found = 0;
    for (uint32_t i = 0; i < count; i++) if (!strcmp(ext[i].extensionName, name)) { found = 1; break; }
    free(ext);
    return found;
}

int s2a_vk_device_create(struct s2a_vk_device *out, char *why, size_t why_size) {
    if (!out) return fail(why, why_size, "null device");
    memset(out, 0, sizeof *out);
    uint32_t api = VK_API_VERSION_1_1;
    vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_1) return fail(why, why_size, "Vulkan loader below 1.1");

    const char *wanted[] = {
        "VK_KHR_get_physical_device_properties2", "VK_KHR_surface", "VK_KHR_android_surface",
        "VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1" };
    const char *enabled[sizeof wanted / sizeof *wanted];
    uint32_t enabled_count = 0;
    for (unsigned i = 0; i < sizeof wanted / sizeof *wanted; i++)
        if (has_instance_extension(wanted[i])) enabled[enabled_count++] = wanted[i];
    out->has_android_surface = has_instance_extension("VK_KHR_android_surface");

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "s2a-direct-vk";
    app.applicationVersion = 1;
    app.pEngineName = "s2a";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = enabled_count;
    info.ppEnabledExtensionNames = enabled;
    if (vkCreateInstance(&info, NULL, &out->instance) != VK_SUCCESS)
        return fail(why, why_size, "vkCreateInstance failed");

    uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(out->instance, &physical_count, NULL) != VK_SUCCESS || !physical_count)
        return fail(why, why_size, "no Vulkan physical device");
    VkPhysicalDevice *physicals = calloc(physical_count, sizeof *physicals);
    if (!physicals) return fail(why, why_size, "out of memory");
    vkEnumeratePhysicalDevices(out->instance, &physical_count, physicals);
    out->physical = physicals[0];   // 首个＝系统主 GPU；多 GPU 选择是 C8，不在首版猜。
    free(physicals);
    vkGetPhysicalDeviceProperties(out->physical, &out->properties);
    vkGetPhysicalDeviceMemoryProperties(out->physical, &out->memory);
    out->has_2d_array_compatible = out->properties.apiVersion >= VK_API_VERSION_1_1 ||
                                   has_device_extension(out->physical, "VK_KHR_maintenance1");

    VkPhysicalDeviceDriverProperties driver = {0};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props2 = {0};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(out->physical, &props2);
    snprintf(out->driver_name, sizeof out->driver_name, "%.120s", driver.driverName);
    snprintf(out->driver_info, sizeof out->driver_info, "%.120s", driver.driverInfo);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(out->physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count ? family_count : 1, sizeof *families);
    if (!families) return fail(why, why_size, "out of memory");
    vkGetPhysicalDeviceQueueFamilyProperties(out->physical, &family_count, families);
    int found = 0;
    for (uint32_t i = 0; i < family_count; i++)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            families[i].queueCount > 0) { out->queue_family = i; found = 1; break; }
    free(families);
    if (!found) return fail(why, why_size, "no graphics+compute queue family");

    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(out->physical, &features);
    const char *required[] = {
        "independentBlend", "fragmentStoresAndAtomics",
#ifndef S2A_ANDROID_VK_BACKEND_NVN_VK
        // The older backend does not use the per-program guard.
        "vertexPipelineStoresAndAtomics",
#endif
        "shaderStorageImageWriteWithoutFormat", "shaderInt16",
        "occlusionQueryPrecise", "shaderImageGatherExtended", "samplerAnisotropy", "depthClamp",
    };
    for (unsigned i = 0; i < sizeof required / sizeof *required; i++) {
        const char *name = required[i];
        VkBool32 value = VK_FALSE;
        if (!strcmp(name, "independentBlend")) value = features.independentBlend;
        else if (!strcmp(name, "fragmentStoresAndAtomics")) value = features.fragmentStoresAndAtomics;
#ifndef S2A_ANDROID_VK_BACKEND_NVN_VK
        else if (!strcmp(name, "vertexPipelineStoresAndAtomics")) value = features.vertexPipelineStoresAndAtomics;
#endif
        else if (!strcmp(name, "shaderStorageImageWriteWithoutFormat")) value = features.shaderStorageImageWriteWithoutFormat;
        else if (!strcmp(name, "shaderInt16")) value = features.shaderInt16;
        else if (!strcmp(name, "occlusionQueryPrecise")) value = features.occlusionQueryPrecise;
        else if (!strcmp(name, "shaderImageGatherExtended")) value = features.shaderImageGatherExtended;
        else if (!strcmp(name, "samplerAnisotropy")) value = features.samplerAnisotropy;
        else if (!strcmp(name, "depthClamp")) value = features.depthClamp;
        if (!value) {
            char text[96];
            snprintf(text, sizeof text, "required feature missing: %.64s", name);
            return fail(why, why_size, text);
        }
    }
    VkPhysicalDeviceFeatures enabled_features = features;
    // 不启用声明外的能力：只开首版执行层实际需要的字段。
    enabled_features.shaderFloat64 = VK_FALSE;
    enabled_features.shaderInt64 = VK_FALSE;
    enabled_features.sparseBinding = VK_FALSE;
    enabled_features.sparseResidencyBuffer = VK_FALSE;
    enabled_features.sparseResidencyImage2D = VK_FALSE;
    // #1456：与通用侧同一个能力位——capprobe 结果文件判 gs_occlusion 可用才启用 GS；
    // 名单/探测/运行时回写说不行（或无结果文件）就关，走已验证的转计算。
    // 直连执行层首版不建 GS 管线，此位只决定设备特性是否启用并如实进入 caps JSON。
    // #1482：s2a_vk_gs_path 内正向落 "graphics GS path: ..." 一行并内置
    // debug.s2a.gs_path 双门强制开关（native/direct/gs_path.c）。符号弱引用：
    // 只链 vk_device.c 的宿主探针照旧能链接（NULL→关 GS，与无判定文件同向安全）。
    enabled_features.geometryShader =
        s2a_vk_gs_path && s2a_vk_gs_path(features.geometryShader, NULL) ? VK_TRUE : VK_FALSE;
    enabled_features.tessellationShader = VK_FALSE;
    // C2 生成的 SPIR-V 会声明这些能力；只在实际支持时启用，不支持则留给 shader 校验显式失败。
    enabled_features.shaderImageGatherExtended = features.shaderImageGatherExtended;
    enabled_features.shaderStorageImageExtendedFormats = features.shaderStorageImageExtendedFormats;
    enabled_features.shaderStorageImageReadWithoutFormat = features.shaderStorageImageReadWithoutFormat;
    enabled_features.shaderStorageImageWriteWithoutFormat = features.shaderStorageImageWriteWithoutFormat;
    enabled_features.shaderInt16 = features.shaderInt16;
    enabled_features.shaderClipDistance = features.shaderClipDistance;
    enabled_features.shaderCullDistance = features.shaderCullDistance;
    enabled_features.sampleRateShading = features.sampleRateShading;
    out->core_features = enabled_features;

    // #1199：逐格式 BC 探测（optimal-tiling 的 SAMPLED_IMAGE）。bit i ↔ NVN 格式 0x41+i；
    // 缺的格式走 compute 解码，不再按 textureCompressionBC 一刀切拒绝整机。
    static const VkFormat bc_vk[16] = {
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
        VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
        VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK };
    for (int i = 0; i < 16; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(out->physical, bc_vk[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            out->bc_native_mask |= 1u << i;
    }
#if defined(S2A_ANDROID_DEBUG_ENV) || defined(S2A_BCDEC_HOST_TEST)
    {
        const char *env = getenv("S2A_VK_BC_DECODE");
        out->bc_decode_forced = env && !strcmp(env, "force");
    }
#else
    if (getenv("S2A_VK_BC_DECODE"))
        fprintf(stderr, "[s2a] S2A_VK_BC_DECODE ignored (not a debug build)\n");
#endif
    out->bc_decode_mask = out->bc_decode_forced ? 0xffffu : (~out->bc_native_mask & 0xffffu);
    fprintf(stderr, "[s2a] vk BC route: native=0x%04x compute=0x%04x%s\n",
            out->bc_native_mask, out->bc_decode_mask, out->bc_decode_forced ? " (force)" : "");

    out->has_swapchain = has_device_extension(out->physical, "VK_KHR_swapchain");
    out->has_swapchain_maintenance1 = has_device_extension(out->physical, "VK_EXT_swapchain_maintenance1");
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance = {0};
    maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {0};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters = {0};
    draw_parameters.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT index_u8 = {0};
    index_u8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT;
    {
        VkPhysicalDeviceFeatures2 query = {0};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        draw_parameters.pNext = &timeline;
        timeline.pNext = &index_u8;
        query.pNext = &draw_parameters;
        if (out->has_swapchain_maintenance1) {
            maintenance.pNext = &draw_parameters;
            query.pNext = &maintenance;
        }
        vkGetPhysicalDeviceFeatures2(out->physical, &query);
        out->swapchain_maintenance1_enabled = out->has_swapchain_maintenance1 && maintenance.swapchainMaintenance1 == VK_TRUE;
        out->timeline_semaphore_enabled = timeline.timelineSemaphore == VK_TRUE;
        out->shader_draw_parameters_enabled = draw_parameters.shaderDrawParameters == VK_TRUE;
    }
    // C2 顶点 SPIR-V 声明 DrawParameters（gl_VertexIndex / gl_InstanceIndex）；没有该特征不能合法消费模块。
    if (!out->shader_draw_parameters_enabled)
        return fail(why, why_size, "required feature missing: shaderDrawParameters");

    // 容量取两边并集：main 的 index_type_uint8 + 本 PR 的 host import / AHB（其余是 swapchain /
    // maintenance1 / 校准扩展），共 6 项，留到 10 不至于以后再加一项就越界。
    const char *device_ext[10];
    uint32_t device_ext_count = 0;
    if (out->has_swapchain) device_ext[device_ext_count++] = "VK_KHR_swapchain";
    if (out->swapchain_maintenance1_enabled) device_ext[device_ext_count++] = "VK_EXT_swapchain_maintenance1";
    // 时间戳换算成游戏时钟要用校准扩展求 GPU 域与 CLOCK_MONOTONIC 的偏移。两种扩展同名同签名，
    // 优先 KHR；都没有时时间戳走“完成点取 guest_tick”的兜底（不是伪造 GPU 时刻）。
    out->calibrated_timestamps = 0;
    if (has_device_extension(out->physical, "VK_KHR_calibrated_timestamps")) {
        out->calibrated_timestamps = 1;
        device_ext[device_ext_count++] = "VK_KHR_calibrated_timestamps";
    } else if (has_device_extension(out->physical, "VK_EXT_calibrated_timestamps")) {
        out->calibrated_timestamps = 2;
        device_ext[device_ext_count++] = "VK_EXT_calibrated_timestamps";
    }
    out->index_type_uint8_enabled = has_device_extension(out->physical, "VK_EXT_index_type_uint8") &&
                                    index_u8.indexTypeUint8 == VK_TRUE;
    if (out->index_type_uint8_enabled) device_ext[device_ext_count++] = "VK_EXT_index_type_uint8";
    // B2：display_timing 只在驱动 advertise 时启用；是否挂 presentID 由 nvn_vk_display_timing 的门控。
    out->has_display_timing = has_device_extension(out->physical, "VK_GOOGLE_display_timing");
    if (out->has_display_timing) device_ext[device_ext_count++] = "VK_GOOGLE_display_timing";
    // 零拷贝可行性探测（只读，不改行为）：VK_EXT_external_memory_host 允许把一个 host 指针导入成 VkBuffer
    // 内存——这是把 guest 内存直接交给 GPU、免掉每笔 memcpy 的唯一合法路径（Metal 侧靠
    // newBufferWithBytesNoCopy 就做到了）。这里只查 advertise + 对齐要求；是否启用由调用方按属性决定。
    out->has_external_memory_host = has_device_extension(out->physical, "VK_EXT_external_memory_host");
    if (out->has_external_memory_host) {
        PFN_vkGetPhysicalDeviceProperties2 get2 = (PFN_vkGetPhysicalDeviceProperties2)
            vkGetInstanceProcAddr(out->instance, "vkGetPhysicalDeviceProperties2");
        if (get2) {
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {0};
            hp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 p2 = {0};
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &hp;
            get2(out->physical, &p2);
            out->host_pointer_alignment = hp.minImportedHostPointerAlignment;
        }
    }
    {
        int want = prop_int("debug.s2a.host_import", 0);
        out->external_memory_host_enabled = out->has_external_memory_host && want;
        S2A_DLOG( "[s2a] host import: ext=%d want=%d enabled=%d align=%llu\n",
                out->has_external_memory_host, want, out->external_memory_host_enabled,
                (unsigned long long)out->host_pointer_alignment);
    }
    // Android 共享后备页（AHB）：与 host import 同一件事的另一种实现，按设备实际支持二选一。
    out->has_ahb_import = has_device_extension(out->physical, "VK_ANDROID_external_memory_android_hardware_buffer");
    out->ahb_import_enabled = out->has_ahb_import && prop_int("debug.s2a.mem_shared", 1);   // 本片起默认开：设备不支持 AHB 时 has_ahb_import=0，等价于关闭；debug.s2a.mem_shared=0 可关
    // 按规范：导入外部内存后当前队列族要能使用它，必须同时启用 VK_EXT_queue_family_foreign。
    // 设备不提供这条扩展时不能只启用 AHB（会违反 spec），整条零拷贝路径关掉、回退上传。
    out->has_queue_family_foreign = has_device_extension(out->physical, "VK_EXT_queue_family_foreign");
    if (out->ahb_import_enabled && !out->has_queue_family_foreign) {
        out->ahb_import_enabled = 0;
        S2A_DLOG( "[s2a] ahb import disabled: VK_EXT_queue_family_foreign missing\n");
    }
    S2A_DLOG( "[s2a] ahb import: ext=%d qff=%d enabled=%d\n", out->has_ahb_import,
            out->has_queue_family_foreign, out->ahb_import_enabled);

    // host_import 仍默认关（本机驱动不支持）；AHB 默认开（第 ② 片接线，debug.s2a.mem_shared=0 可关）：
    // 设备不支持 AHB 时 has_ahb_import=0，等价于关闭，绘制层继续走正确的上传路径。
    if (out->external_memory_host_enabled) device_ext[device_ext_count++] = "VK_EXT_external_memory_host";
    if (out->ahb_import_enabled) {
        device_ext[device_ext_count++] = "VK_ANDROID_external_memory_android_hardware_buffer";
        device_ext[device_ext_count++] = "VK_EXT_queue_family_foreign";   // 与 AHB 成对（VUID-vkCreateDevice / spec 外部内存要求）
    }
    out->device_extension_count = device_ext_count;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {0};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = out->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {0};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &enabled_features;
    device_info.enabledExtensionCount = device_ext_count;
    device_info.ppEnabledExtensionNames = device_ext;
    draw_parameters.shaderDrawParameters = VK_TRUE;
    draw_parameters.pNext = out->timeline_semaphore_enabled ? (VkBaseInStructure *)&timeline
                          : out->index_type_uint8_enabled ? (VkBaseInStructure *)&index_u8 : NULL;
    timeline.timelineSemaphore = out->timeline_semaphore_enabled ? VK_TRUE : VK_FALSE;
    timeline.pNext = out->index_type_uint8_enabled ? (VkBaseInStructure *)&index_u8 : NULL;
    index_u8.indexTypeUint8 = out->index_type_uint8_enabled ? VK_TRUE : VK_FALSE;
    if (out->swapchain_maintenance1_enabled) {
        maintenance.swapchainMaintenance1 = VK_TRUE;
        maintenance.pNext = &draw_parameters;
        device_info.pNext = &maintenance;
    } else {
        device_info.pNext = &draw_parameters;
    }
    if (vkCreateDevice(out->physical, &device_info, NULL, &out->device) != VK_SUCCESS)
        return fail(why, why_size, "vkCreateDevice failed");
    vkGetDeviceQueue(out->device, out->queue_family, 0, &out->queue);
    VkCommandPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = out->queue_family;
    if (vkCreateCommandPool(out->device, &pool, NULL, &out->command_pool) != VK_SUCCESS)
        return fail(why, why_size, "vkCreateCommandPool failed");
    // 内存后端自检（诊断，debug.s2a.mem_probe=1）：扩展 advertise 不等于能用，逐后端实测一遍
    // 「CPU 写→GPU 读」「GPU 写→CPU 读」。只在要求时跑，不影响默认路径。符号弱引用，
    // 让只链 vk_device.c 的宿主探针照旧能链接。
    if (prop_int("debug.s2a.mem_probe", 0)) {
        extern int s2a_gpu_mem_probe(struct s2a_vk_device *, struct s2a_gpu_mem_caps *, char *, size_t)
            __attribute__((weak));
        extern int s2a_gpu_mem_selftest(struct s2a_vk_device *, int, char *, size_t) __attribute__((weak));
        if (s2a_gpu_mem_probe && s2a_gpu_mem_selftest) {
            const struct { int kind; int enabled; const char *name; } try[] = {
                { S2A_GPU_MEM_HOST_IMPORT, out->external_memory_host_enabled, "host_import" },
                { S2A_GPU_MEM_AHB, out->ahb_import_enabled, "ahb" },
                { S2A_GPU_MEM_UPLOAD, 1, "upload" },
            };
            struct s2a_gpu_mem_caps caps;
            char wy[256] = "";
            if (s2a_gpu_mem_probe(out, &caps, wy, sizeof wy) == 0) {
                S2A_DLOG( "[s2a] mem caps: host_import=%d align=%llu ahb=%d dmabuf=%d shared_enabled=%d\n",
                        caps.host_import, (unsigned long long)caps.host_align, caps.ahb, caps.dmabuf,
                        caps.shared_enabled);
                extern double s2a_gpu_mem_bench(struct s2a_vk_device *, int, size_t, char *, size_t)
                    __attribute__((weak));
                for (unsigned i = 0; i < sizeof try / sizeof try[0]; i++) {
                    if (!try[i].enabled) {
                        S2A_DLOG("[s2a] mem selftest %s: skipped (not enabled)\n", try[i].name);
                        continue;
                    }
                    wy[0] = 0;
                    int rc = s2a_gpu_mem_selftest(out, try[i].kind, wy, sizeof wy);
                    S2A_DLOG("[s2a] mem selftest %s: rc=%d %s\n", try[i].name, rc, wy);
                    // 扩展存在、自检通过，还要看“当 guest 内存用”时 CPU 写入带宽：共享映射常不可缓存，
                    // 写池子会变慢，慢到一定程度就不该换过去。
                    if (rc == 0 && i < 2) {
                        extern int s2a_gpu_mem_selftest_alias(struct s2a_vk_device *, char *, size_t)
                            __attribute__((weak));
                        if (s2a_gpu_mem_selftest_alias) {
                            wy[0] = 0;
                            int arc = s2a_gpu_mem_selftest_alias(out, wy, sizeof wy);
                            S2A_DLOG("[s2a] mem alias %s: rc=%d %s\n", try[i].name, arc, wy);
                        }
                    }
                    {
                        extern double s2a_gpu_mem_bench_scatter(struct s2a_vk_device *, int, size_t, char *, size_t)
                            __attribute__((weak));
                        if (rc == 0 && s2a_gpu_mem_bench_scatter) {
                            wy[0] = 0;
                            double ns = s2a_gpu_mem_bench_scatter(out, try[i].kind, 4u << 20, wy, sizeof wy);
                            S2A_DLOG("[s2a] mem scatter %s: %.1f ns/写 (%s)\n", try[i].name, ns, wy);
                        }
                    }
                    if (rc == 0 && s2a_gpu_mem_bench) {
                        wy[0] = 0;
                        double mbs = s2a_gpu_mem_bench(out, try[i].kind, 32u << 20, wy, sizeof wy);
                        S2A_DLOG("[s2a] mem bench %s: 32MB cpu-write %.0f MB/s (%s)\n",
                                 try[i].name, mbs, wy);
                    }
                }
            } else {
                S2A_DLOG( "[s2a] mem probe failed: %s\n", wy);
            }
        }
    }
    return 0;
}

int s2a_vk_device_wait_idle(struct s2a_vk_device *device) {
    if (!device || !device->device) return VK_SUCCESS;
    return (int)vkDeviceWaitIdle(device->device);
}

int s2a_vk_device_destroy(struct s2a_vk_device *device, int idle_proven) {
    if (!device) return 0;
    if (device->device) {
        if (!idle_proven && s2a_vk_device_wait_idle(device) != VK_SUCCESS) return -1;
        if (device->command_pool) vkDestroyCommandPool(device->device, device->command_pool, NULL);
        vkDestroyDevice(device->device, NULL);
    }
    if (device->instance) vkDestroyInstance(device->instance, NULL);
    memset(device, 0, sizeof *device);
    return 0;
}

uint32_t s2a_vk_memory_type(const struct s2a_vk_device *device, uint32_t type_bits,
                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags *actual) {
    for (uint32_t i = 0; i < device->memory.memoryTypeCount && i < 32; i++) {
        VkMemoryPropertyFlags flags = device->memory.memoryTypes[i].propertyFlags;
        if ((type_bits & (1u << i)) && (flags & required) == required) {
            if (actual) *actual = flags;
            return i;
        }
    }
    return UINT32_MAX;
}

// 零拷贝探针：把一个真实 host 指针（guest 内存池的直映射地址）导入成 VkBuffer 内存，走完
// 「查 host 指针属性 → 建 buffer → 带 import 信息分配 → 绑定 → 映射 → 与 guest 内存对读」全流程，
// 再自己销毁。全程不改运行状态；任一步失败都写清是第几步、哪个 VkResult。
int s2a_vk_probe_host_import(struct s2a_vk_device *device, const void *host, size_t len,
                             char *why, size_t why_size) {
    if (!device || !device->device) return fail(why, why_size, "no device");
    if (!device->external_memory_host_enabled)
        return fail(why, why_size, "host import not enabled (needs VK_EXT_external_memory_host + debug.s2a.host_import=1)");
    if (!host || !len) return fail(why, why_size, "no host range");
    PFN_vkGetMemoryHostPointerPropertiesEXT get_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)
        vkGetDeviceProcAddr(device->device, "vkGetMemoryHostPointerPropertiesEXT");
    if (!get_props) return fail(why, why_size, "vkGetMemoryHostPointerPropertiesEXT missing");

    // 1) 这个 host 指针能不能当 external memory 的来源？驱动在这里给出可用内存类型位。
    VkMemoryHostPointerPropertiesEXT hp = {0};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    VkResult r = get_props(device->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (r != VK_SUCCESS) {
        char t[96];
        snprintf(t, sizeof t, "step1 vkGetMemoryHostPointerPropertiesEXT rc=%d", (int)r);
        return fail(why, why_size, t);
    }
    size_t align = (size_t)(device->host_pointer_alignment ? device->host_pointer_alignment : 1);
    if ((uintptr_t)host % align) {
        char t[128];
        snprintf(t, sizeof t, "step1 host pointer %p not %zu-aligned", host, align);
        return fail(why, why_size, t);
    }

    // 2) 建一个和真实用途同形的 buffer（VB/IB/UBO 都要能当）。
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = len;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    r = vkCreateBuffer(device->device, &bi, NULL, &buf);
    if (r != VK_SUCCESS) {
        char t[96];
        snprintf(t, sizeof t, "step2 vkCreateBuffer rc=%d", (int)r);
        return fail(why, why_size, t);
    }
    int rc = -1;
    char text[160] = "";
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device->device, buf, &mr);
    uint32_t bits = mr.memoryTypeBits & hp.memoryTypeBits;
    VkMemoryPropertyFlags flags = 0;
    uint32_t mt = s2a_vk_memory_type(device, bits, 0, &flags);
    if (mt == UINT32_MAX) {
        snprintf(text, sizeof text, "step2 no memory type shared by buffer(%#x)/host ptr(%#x)",
                 mr.memoryTypeBits, hp.memoryTypeBits);
        goto done;
    }
    // 3) 带 import 信息分配：这步成功才说明“驱动同意把这段 guest 内存当显存缓冲用”。
    {
        VkImportMemoryHostPointerInfoEXT imp = {0};
        imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        imp.pHostPointer = (void *)host;
        VkMemoryAllocateInfo mai = {0};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        mai.pNext = &imp;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        r = vkAllocateMemory(device->device, &mai, NULL, &mem);
        if (r != VK_SUCCESS) {
            snprintf(text, sizeof text, "step3 vkAllocateMemory(import) rc=%d allocSize=%llu mt=%u flags=%#x",
                     (int)r, (unsigned long long)mr.size, mt, flags);
            goto done;
        }
        // 4) 绑定 + 映射 + 对读：零拷贝要求 GPU 看到的就是 guest 内存本身。
        r = vkBindBufferMemory(device->device, buf, mem, 0);
        if (r != VK_SUCCESS) {
            snprintf(text, sizeof text, "step4 vkBindBufferMemory rc=%d", (int)r);
            vkFreeMemory(device->device, mem, NULL);
            goto done;
        }
        void *map = NULL;
        r = vkMapMemory(device->device, mem, 0, len, 0, &map);
        if (r != VK_SUCCESS) {
            snprintf(text, sizeof text, "step5 vkMapMemory rc=%d", (int)r);
            vkFreeMemory(device->device, mem, NULL);
            goto done;
        }
        int same = (map && memcmp(map, host, len) == 0);
        snprintf(text, sizeof text, "ok align=%zu mt=%u flags=%#x map=%p same_content=%d",
                 align, mt, flags, map, same);
        vkUnmapMemory(device->device, mem);
        vkFreeMemory(device->device, mem, NULL);
        rc = 0;
    }
done:
    vkDestroyBuffer(device->device, buf, NULL);
    if (why && why_size) snprintf(why, why_size, "%s", text);
    return rc;
}
