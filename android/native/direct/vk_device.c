#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   // clock_gettime / nanosleep（s2a_vk_wait_fences）
#endif
#if defined(__OHOS__)
#define VK_USE_PLATFORM_OHOS 1
#define S2A_VK_PLATFORM_SURFACE "VK_OHOS_surface"
#else
#define VK_USE_PLATFORM_ANDROID_KHR 1
#define S2A_VK_PLATFORM_SURFACE "VK_KHR_android_surface"
#endif
#include "vk_device.h"
#include "gs_path.h"
#include "../capprobe/capprobe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// 支持范围的下限：物理设备（GPU 驱动）至少 Vulkan 1.1。loader 是 1.1 不代表 GPU 是（老 Mali / Adreno 5xx 驱动报 1.0），
//   只查 loader 时这类设备会一路走到后面的特性检查或建管线才失败、原因看不懂。0 = 够；-1 = 不够，why 写明 GPU 报的版本。
//   原因串开头的英文短语是契约：外壳 PlayerError.stopText 按它认出来、给玩家换成看得懂的话（android/tests/vk_api_floor_test.sh 核两边一致）。
static int physical_api_floor(uint32_t api, char *why, size_t n) {
    if (api >= VK_API_VERSION_1_1) return 0;
    char text[160];
    snprintf(text, sizeof text, "GPU supports only Vulkan 1.0 (apiVersion=%u.%u.%u), outside the supported range (Vulkan 1.1+ required)",
             VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
    return fail(why, n, text);
}

// 扩展查询（physical 为空 = 实例扩展，否则 = 该物理设备的设备扩展）：1 = 有，0 = 确实没有（枚举完整成功且不在表里），
//   -1 = 能力未知（枚举失败；why 写原因）。第二次取数回 VK_INCOMPLETE（两次调用之间表变长）按新的数目重取，最多 4 轮；部分结果不拿来判「没有」
static int extension_state(VkPhysicalDevice physical, const char *name, char *why, size_t why_n) {
    for (int round = 1; round <= 4; round++) {
        uint32_t count = 0;
        VkResult r = physical ? vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) : vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
        if (r != VK_SUCCESS) { snprintf(why, why_n, "取数目回 VkResult %d（第 %d 轮）", (int)r, round); return -1; }
        VkExtensionProperties *ext = calloc(count ? count : 1, sizeof *ext);
        if (!ext) { snprintf(why, why_n, "分配 %u 项失败（第 %d 轮）", count, round); return -1; }
        uint32_t got = count;
        r = physical ? vkEnumerateDeviceExtensionProperties(physical, NULL, &got, ext) : vkEnumerateInstanceExtensionProperties(NULL, &got, ext);
        if (r == VK_INCOMPLETE) { free(ext); continue; }
        if (r != VK_SUCCESS || got > count) {
            free(ext); snprintf(why, why_n, "取表回 VkResult %d、%u / %u 项（第 %d 轮）", (int)r, got, count, round); return -1;
        }
        int found = 0;
        for (uint32_t i = 0; i < got; i++) if (!strcmp(ext[i].extensionName, name)) { found = 1; break; }
        free(ext);
        return found;
    }
    snprintf(why, why_n, "连续 4 轮取表都回 VK_INCOMPLETE");
    return -1;
}
// 启用判断只认「确实有」；能力未知时报红并按未启用处理（不当成「确实不支持」默默略过）
static int has_extension(VkPhysicalDevice physical, const char *name) {
    char why[96] = "";
    int st = extension_state(physical, name, why, sizeof why);
    if (st < 0) S2A_DLOG("【红】vk %s扩展 %s：枚举失败（%s），能力未知，按未启用处理\n", physical ? "设备" : "实例", name, why);
    return st > 0;
}
static int has_instance_extension(const char *name) { return has_extension(VK_NULL_HANDLE, name); }
static int has_device_extension(VkPhysicalDevice physical, const char *name) { return has_extension(physical, name); }

// 弱链接[capshape]：nvn_plat_vk_instance_exts（宿主能力形态档 android/tests/caps_shape 不链平台件，靠弱符号为空）
// 弱链接[capshape]：s2a_android_vk_custom_driver_prepare
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) __attribute__((weak));

// ---------- 调试：Khronos 校验层（开关 debug.s2a.vk_validation=1，默认关） ----------
// 调试包里带了 libVkLayer_khronos_validation.so 时，应用自己在建实例那一刻启用它，报出来的每种错打一行（之后按 2 的幂再报次数）。
// 不改手机的系统设置；包里没有这个库、或开关没开，就和平时完全一样。
#if defined(__ANDROID__)
static VkDebugUtilsMessengerEXT g_val_messenger;
static struct { int32_t id; uint64_t n; } g_val_seen[128]; static int g_val_nseen;
static VKAPI_ATTR VkBool32 VKAPI_CALL val_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type; (void)user;
    if (!d || sev < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return VK_FALSE;
    int k = 0; for (; k < g_val_nseen; k++) if (g_val_seen[k].id == d->messageIdNumber) break;
    if (k == g_val_nseen) { if (g_val_nseen == 128) return VK_FALSE; g_val_seen[g_val_nseen].id = d->messageIdNumber; g_val_seen[g_val_nseen].n = 0; g_val_nseen++; }
    uint64_t n = ++g_val_seen[k].n;
    if (n == 1) __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "【红】vk 校验（%s）%s：%.700s", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "错" : "警告", d->pMessageIdName ? d->pMessageIdName : "?", d->pMessage ? d->pMessage : "");
    else if (!(n & (n - 1))) __android_log_print(ANDROID_LOG_INFO, "S2Arm", "vk 校验：%s 累计 %llu 次", d->pMessageIdName ? d->pMessageIdName : "?", (unsigned long long)n);
    return VK_FALSE;
}
static int val_wanted(void) {
    char v[PROP_VALUE_MAX] = {0}, e[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.s2a.debug_env", e) <= 0 || e[0] != '1') return 0;
    if (__system_property_get("debug.s2a.vk_validation", v) <= 0 || v[0] != '1') return 0;
    uint32_t n = 0; if (vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS || !n) return 0;
    VkLayerProperties *lp = calloc(n, sizeof *lp); if (!lp) return 0;
    int found = 0; if (vkEnumerateInstanceLayerProperties(&n, lp) == VK_SUCCESS || n) for (uint32_t i = 0; i < n; i++) if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) found = 1;
    free(lp);
    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "vk 校验：开关开着，校验层%s", found ? "在包里，启用" : "不在包里，不启用");
    return found;
}
#endif

void s2a_vk_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *out) {
    memset(out, 0, sizeof *out);
    if (format == VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT) {
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                     VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        return;
    }
    vkGetPhysicalDeviceFormatProperties(physical, format, out);
}

int s2a_vk_device_create(struct s2a_vk_device *out, char *why, size_t why_size) {
    if (!out) return fail(why, why_size, "null device");
    memset(out, 0, sizeof *out);
    uint32_t api = VK_API_VERSION_1_1;
    vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_1) return fail(why, why_size, "Vulkan loader below 1.1");

    const char *wanted[] = {
        "VK_KHR_get_physical_device_properties2", "VK_KHR_surface", S2A_VK_PLATFORM_SURFACE,
        "VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1" };
    const char *enabled[sizeof wanted / sizeof *wanted + 9];
    uint32_t enabled_count = 0;
    for (unsigned i = 0; i < sizeof wanted / sizeof *wanted; i++)
        if (has_instance_extension(wanted[i])) enabled[enabled_count++] = wanted[i];
    // 平台件要的表面扩展（graphics/nvn/replay/nvn_plat_vk.h；鸿蒙是 VK_OHOS_surface，宿主测试是无头表面）。
    // 上面写死的安卓名字是旧层留下的，旧层删除后这张表只留与平台无关的几项。
    if (nvn_plat_vk_instance_exts) {
        uint32_t pn = 0; const char *const *pe = nvn_plat_vk_instance_exts(&pn);
        for (uint32_t i = 0; pe && i < pn && i < 8; i++) {
            int dup = 0; for (uint32_t k = 0; k < enabled_count; k++) if (!strcmp(enabled[k], pe[i])) dup = 1;
            if (!dup && has_instance_extension(pe[i])) enabled[enabled_count++] = pe[i];
        }
    }
    int instance_surface_maintenance1 = 0, instance_surface_capabilities2 = 0;
    for (uint32_t i = 0; i < enabled_count; i++) {
        if (!strcmp(enabled[i], "VK_EXT_surface_maintenance1")) instance_surface_maintenance1 = 1;
        if (!strcmp(enabled[i], "VK_KHR_get_surface_capabilities2")) instance_surface_capabilities2 = 1;
    }
    out->has_android_surface = has_instance_extension(S2A_VK_PLATFORM_SURFACE);

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "s2a-direct-vk";
    app.applicationVersion = 1;
    app.pEngineName = "s2a";
    app.apiVersion = S2A_VK_APP_API_VERSION;
    VkInstanceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
#if defined(__ANDROID__)
    static const char *const val_layers[] = {"VK_LAYER_KHRONOS_validation"};
    int validate = val_wanted();
    if (validate) { enabled[enabled_count++] = "VK_EXT_debug_utils"; info.enabledLayerCount = 1; info.ppEnabledLayerNames = val_layers; }
#endif
    info.enabledExtensionCount = enabled_count;
    info.ppEnabledExtensionNames = enabled;
    // nvk 路径也给自定义驱动一个机会：game 构建里 vkCreateInstance 被 --wrap 接管，
    // 这里先填窄 shim。没选驱动时返回 NULL，系统 loader 路径与改前完全相同。
    {
        extern void *s2a_android_vk_custom_driver_prepare(const char *) __attribute__((weak));
        if (s2a_android_vk_custom_driver_prepare) (void)s2a_android_vk_custom_driver_prepare(NULL);
    }
    if (vkCreateInstance(&info, NULL, &out->instance) != VK_SUCCESS)
        return fail(why, why_size, "vkCreateInstance failed");
#if defined(__ANDROID__)
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT mk = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(out->instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {0}; mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = val_cb;
        if (!mk || mk(out->instance, &mi, NULL, &g_val_messenger) != VK_SUCCESS) g_val_messenger = VK_NULL_HANDLE;
    }
#endif

    uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(out->instance, &physical_count, NULL) != VK_SUCCESS || !physical_count)
        return fail(why, why_size, "no Vulkan physical device");
    VkPhysicalDevice *physicals = calloc(physical_count, sizeof *physicals);
    if (!physicals) return fail(why, why_size, "out of memory");
    vkEnumeratePhysicalDevices(out->instance, &physical_count, physicals);
    out->physical = physicals[0];   // 首个＝系统主 GPU；多 GPU 选择是 C8，不在首版猜。
    free(physicals);
    vkGetPhysicalDeviceProperties(out->physical, &out->properties);
    // #1749 M0：设备事实现查一次、落盘成设备档案、打【事实】摘要（dev_facts.c，只查询不改启用项）。放在一切「缺能力就拒绝」之前（含下一行的 1.1 版本下限）：
    //   建不成设备的机器也留下档案（1.0 设备只记核心查询）。弱引用：只链 vk_device.c 的宿主探针照旧能链接（没有就不记）。
    {
        extern int s2a_dev_facts_on_device_create(VkPhysicalDevice) __attribute__((weak));
        if (s2a_dev_facts_on_device_create) (void)s2a_dev_facts_on_device_create(out->physical);
    }
    if (physical_api_floor(out->properties.apiVersion, why, why_size)) return -1;
    vkGetPhysicalDeviceMemoryProperties(out->physical, &out->memory);
    out->has_2d_array_compatible = out->properties.apiVersion >= VK_API_VERSION_1_1 ||
                                   has_device_extension(out->physical, "VK_KHR_maintenance1");

    VkPhysicalDeviceDriverProperties driver = {0};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    // 驱动属性是 1.2 核心 / VK_KHR_driver_properties：有效版本（应用申请 1.1 与设备取低）不到 1.2 时只在列了扩展才挂；
    //   都没有 = 驱动身份未知（名字空、ID 0），按名字识别的特殊处理都不命中
    if (s2a_vk_effective_api(out->properties.apiVersion) >= VK_API_VERSION_1_2 ||
        has_device_extension(out->physical, "VK_KHR_driver_properties")) {
        VkPhysicalDeviceProperties2 props2 = {0};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(out->physical, &props2);
    }
    snprintf(out->driver_name, sizeof out->driver_name, "%.120s", driver.driverName);
    snprintf(out->driver_info, sizeof out->driver_info, "%.120s", driver.driverInfo);
    out->driver_id = (int)driver.driverID;

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(out->physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count ? family_count : 1, sizeof *families);
    if (!families) return fail(why, why_size, "out of memory");
    vkGetPhysicalDeviceQueueFamilyProperties(out->physical, &family_count, families);
    int found = 0;
    for (uint32_t i = 0; i < family_count; i++)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            families[i].queueCount > 0) { out->queue_family = i; found = 1; break; }
    uint32_t sparse_family = UINT32_MAX;
    if (found && (families[out->queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT))
        sparse_family = out->queue_family;
    else for (uint32_t i = 0; i < family_count; i++) {
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) {
            sparse_family = i;
            break;
        }
    }
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
        // shaderInt16 不在这里（#1340）：生成器不声明 Int16，设备有就启用；声明了的程序在建程序时按程序拒收。
        "shaderStorageImageWriteWithoutFormat",
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
    VkPhysicalDeviceFeatures enabled_features = {0};
    // #1581 P OOB 审计：顶点窗口和动态 SSBO/UBO 访问尚未证明自限，保留核心 robustness。
    // 见 rp_draw.c 的顶点绑定、DirectGpuAccessor.QueryHostStorageWindowClamp 和 InstEmitMemory.Ldc。
    enabled_features.robustBufferAccess = features.robustBufferAccess;
    enabled_features.fullDrawIndexUint32 = features.fullDrawIndexUint32;
    enabled_features.imageCubeArray = features.imageCubeArray;
    enabled_features.independentBlend = features.independentBlend;
    enabled_features.dualSrcBlend = features.dualSrcBlend;
    enabled_features.logicOp = features.logicOp;
    enabled_features.multiDrawIndirect = features.multiDrawIndirect;
    enabled_features.drawIndirectFirstInstance = features.drawIndirectFirstInstance;
    enabled_features.depthClamp = features.depthClamp;
    enabled_features.depthBounds = features.depthBounds;
    enabled_features.depthBiasClamp = features.depthBiasClamp;
    enabled_features.fillModeNonSolid = features.fillModeNonSolid;
    enabled_features.wideLines = features.wideLines;
    enabled_features.largePoints = features.largePoints;
    enabled_features.multiViewport = features.multiViewport;
    enabled_features.samplerAnisotropy = features.samplerAnisotropy;
    enabled_features.textureCompressionETC2 = features.textureCompressionETC2;
    enabled_features.textureCompressionASTC_LDR = features.textureCompressionASTC_LDR;
    enabled_features.textureCompressionBC = features.textureCompressionBC;
    enabled_features.occlusionQueryPrecise = features.occlusionQueryPrecise;
    enabled_features.vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics;
    enabled_features.fragmentStoresAndAtomics = features.fragmentStoresAndAtomics;
    enabled_features.shaderTessellationAndGeometryPointSize = features.shaderTessellationAndGeometryPointSize;
    enabled_features.shaderStorageImageMultisample = features.shaderStorageImageMultisample;
    enabled_features.shaderUniformBufferArrayDynamicIndexing = features.shaderUniformBufferArrayDynamicIndexing;
    enabled_features.shaderSampledImageArrayDynamicIndexing = features.shaderSampledImageArrayDynamicIndexing;
    enabled_features.shaderStorageBufferArrayDynamicIndexing = features.shaderStorageBufferArrayDynamicIndexing;
    enabled_features.shaderStorageImageArrayDynamicIndexing = features.shaderStorageImageArrayDynamicIndexing;
    enabled_features.shaderResourceMinLod = features.shaderResourceMinLod;
    enabled_features.variableMultisampleRate = features.variableMultisampleRate;
    enabled_features.shaderFloat64 = VK_FALSE;
    enabled_features.shaderInt64 = VK_FALSE;
    enabled_features.sparseBinding = sparse_family != UINT32_MAX && features.sparseBinding && features.sparseResidencyImage2D;
    enabled_features.sparseResidencyImage2D = enabled_features.sparseBinding;
    enabled_features.sparseResidencyAliased = enabled_features.sparseBinding && features.sparseResidencyAliased;
    // #1456：与通用侧同一个能力位——capprobe 结果文件判 gs_occlusion 可用才启用 GS；
    // 名单/探测/运行时回写说不行（或无结果文件）就关，走已验证的转计算。
    // 直连执行层首版不建 GS 管线，此位只决定设备特性是否启用并如实进入 caps JSON。
    // #1482：s2a_vk_gs_path 内正向落 "graphics GS path: ..." 一行并内置
    // debug.s2a.gs_path 双门强制开关（native/direct/gs_path.c）。符号弱引用：
    // 只链 vk_device.c 的宿主探针照旧能链接（NULL→关 GS，与无判定文件同向安全）。
    enabled_features.geometryShader =
        s2a_vk_gs_path && s2a_vk_gs_path(features.geometryShader, NULL) ? VK_TRUE : VK_FALSE;
    enabled_features.tessellationShader =
        (features.tessellationShader && out->properties.limits.maxTessellationPatchSize > 0)
            ? VK_TRUE : VK_FALSE;
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

    // #398：D24 深度附件不是必备能力（三星 Xclipse 只有 D16S8 / D32S8）。缺了就让 NVN DEPTH24(_STENCIL8)
    // 按 D32 建，与 Mac 后端同一做法。位定义同 nvn_vk_format.h 的 NVN_VK_DEPTH_SUBST_*；格式映射在
    // nvn_vk_format.c，弱引用：只链 vk_device.c 的宿主探针照旧能链接（它们不走 NVN 格式映射）。
    {
        extern uint32_t nvn_vk_depth_subst_pick(VkFormatFeatureFlags, VkFormatFeatureFlags,
                                                VkFormatFeatureFlags, VkFormatFeatureFlags) __attribute__((weak));
        extern void nvn_vk_format_set_depth_subst(uint32_t) __attribute__((weak));
        const VkFormat dv[4] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                 VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT };
        VkFormatFeatureFlags df[4];
        for (int i = 0; i < 4; i++) {
            VkFormatProperties fp;
            memset(&fp, 0, sizeof fp);
            vkGetPhysicalDeviceFormatProperties(out->physical, dv[i], &fp);
            df[i] = fp.optimalTilingFeatures;
        }
        out->depth_subst_mask = nvn_vk_depth_subst_pick ? nvn_vk_depth_subst_pick(df[0], df[1], df[2], df[3]) : 0;
        if (nvn_vk_format_set_depth_subst) nvn_vk_format_set_depth_subst(out->depth_subst_mask);
        fprintf(stderr, "[s2a] vk depth route: D24S8->%s X8D24->%s\n",
                (out->depth_subst_mask & 1u) ? "D32S8" : "native", (out->depth_subst_mask & 2u) ? "D32" : "native");
    }

    out->has_swapchain = has_device_extension(out->physical, "VK_KHR_swapchain");
    out->has_swapchain_maintenance1 = has_device_extension(out->physical, "VK_EXT_swapchain_maintenance1");
    const int has_timeline = has_device_extension(out->physical, "VK_KHR_timeline_semaphore");
    const int want_eds_master = prop_int("debug.s2a.nvk_eds", 1) != 0;
    const int want_eds1 = want_eds_master && prop_int("debug.s2a.nvk_eds1", 1) != 0;
    const int want_eds2 = want_eds_master && prop_int("debug.s2a.nvk_eds2", 1) != 0;
    const int want_vi = want_eds_master && prop_int("debug.s2a.nvk_vertex_input", 1) != 0;
    const int query_eds1 = want_eds1 && has_device_extension(out->physical, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    const int query_eds2 = want_eds2 && has_device_extension(out->physical, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    const int query_vi = want_vi && has_device_extension(out->physical, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
    };
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
    };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vi = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
    };
    const int has_sync2 = has_device_extension(out->physical, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance = {0};
    maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {0};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2_feat = {0};
    sync2_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters = {0};
    draw_parameters.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
    VkPhysicalDevice16BitStorageFeatures storage_16bit = {0};
    storage_16bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    VkPhysicalDeviceVariablePointersFeatures var_ptrs = {0};
    var_ptrs.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES;
    VkPhysicalDeviceMultiviewFeatures multiview = {0};
    multiview.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT index_u8 = {0};
    index_u8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT;
    // #975：VK_EXT_4444_formats 给 NVN RGBA4 的精确格式 A4B4G4R4（可选；没有就走退回格式 + 通道补偿）。
    VkPhysicalDevice4444FormatsFeaturesEXT f4444 = {0};
    f4444.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT;
    const int has_4444 = has_device_extension(out->physical, "VK_EXT_4444_formats");
    // #1070：placed 映射探针（只查、不启用）。NDK 29 自带的头（275 版）还没有这个扩展：结构与类型号照 Vulkan 头原文补在本地
#ifndef VK_EXT_map_memory_placed
    typedef struct { VkStructureType sType; void *pNext; VkBool32 memoryMapPlaced, memoryMapRangePlaced, memoryUnmapReserve; } VkPhysicalDeviceMapMemoryPlacedFeaturesEXT;
    typedef struct { VkStructureType sType; void *pNext; VkDeviceSize minPlacedMemoryMapAlignment; } VkPhysicalDeviceMapMemoryPlacedPropertiesEXT;
#define S2A_STYPE_PLACED_FEATURES ((VkStructureType)1000272000)
#define S2A_STYPE_PLACED_PROPERTIES ((VkStructureType)1000272001)
#else
#define S2A_STYPE_PLACED_FEATURES VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT
#define S2A_STYPE_PLACED_PROPERTIES VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT
#endif
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {0};
    placed.sType = S2A_STYPE_PLACED_FEATURES;
    out->placed_ext = has_device_extension(out->physical, "VK_EXT_map_memory_placed");
    out->map_memory2_ext = has_device_extension(out->physical, "VK_KHR_map_memory2");
    // 出包预编库的顶点着色器不改写深度（near = 0 口径）；near = -w 的游戏靠管线把裁剪深度设成 [-1, 1] 得到同一结果
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc = {0};
    dcc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
    const int has_dcc = has_device_extension(out->physical, "VK_EXT_depth_clip_control");
    // #14：遍内自采样反馈环（VK_EXT_attachment_feedback_loop_layout，依赖 1.1 核心 properties2，不抬 Vulkan 1.1 底线）。
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fb_loop = {0};
    fb_loop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT;
    const int has_fb_loop = has_device_extension(out->physical, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
    const int has_provoking = has_device_extension(out->physical, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    VkPhysicalDeviceProvokingVertexFeaturesEXT provoking = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
    };
    const int has_sep_ds = has_device_extension(out->physical, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
    const int has_rp2 = has_sep_ds && has_device_extension(out->physical, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures sep_ds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES,
    };
    // VK_EXT_host_query_reset（三机 8E/9400/9000 与 lavapipe 全支持）：主机侧直接重置查询池，
    // 省去命令缓冲内 vkCmdResetQueryPool 及 report_ts 的独立前置缓冲 init_cb + ALL_COMMANDS 全管线屏障。
    VkPhysicalDeviceHostQueryResetFeaturesEXT hqr = {0};
    hqr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT;
    const int has_hqr = has_device_extension(out->physical, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
    // VK_EXT_primitive_topology_list_restart（8E / 9400 / lavapipe / 26-28 形态档支持）：
    // 允许列表图元启用 primitiveRestartEnable 对齐 NVN SetPrimitiveRestart 语义；9000 等无扩展设备静默回退条带/扇。
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT list_restart = {0};
    list_restart.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT;
    const int has_list_restart = has_device_extension(out->physical, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    // VK_EXT_image_robustness（Vulkan 1.3 转正；实例按 1.1 建，仅经扩展协商）：保护图像越界读/写不触发未定义行为。
    VkPhysicalDeviceImageRobustnessFeaturesEXT img_robust = {0};
    img_robust.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT;
    const int has_img_robust = has_device_extension(out->physical, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
    // VK_KHR_zero_initialize_workgroup_memory（Vulkan 1.3 转正；实例按 1.1 建，仅经扩展协商）：允许 Workgroup 共享内存零初始化器。
    VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeaturesKHR zero_init_wg = {0};
    zero_init_wg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES_KHR;
    const int has_zero_init_wg = has_device_extension(out->physical, VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME);
    // VK_EXT_scalar_block_layout（Vulkan 1.2 转正；实例按 1.1 建，仅经扩展协商）：允许标量块对齐（紧凑 vec3/数组）。
    VkPhysicalDeviceScalarBlockLayoutFeaturesEXT scalar_layout = {0};
    scalar_layout.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT;
    const int has_scalar_layout = has_device_extension(out->physical, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
    // VK_KHR_uniform_buffer_standard_layout（Vulkan 1.2 转正；实例按 1.1 建，仅经扩展协商）：允许 UBO 按 std430 紧凑布局。
    VkPhysicalDeviceUniformBufferStandardLayoutFeaturesKHR ubo_std = {0};
    ubo_std.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES_KHR;
    const int has_ubo_std = has_device_extension(out->physical, VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
    // VK_EXT_line_rasterization（精确矩形/Bresenham/平滑/虚线光栅，实例按 1.1 建，仅经扩展协商）
    VkPhysicalDeviceLineRasterizationFeaturesEXT line_rast = {0};
    line_rast.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
    const int has_line_rast = has_device_extension(out->physical, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME);
    VkPhysicalDevice8BitStorageFeaturesKHR storage_8bit = {0};
    storage_8bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR;
    const int has_8bit_storage = has_device_extension(out->physical, VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    // 设备按 Vulkan 1.1 创建：扩展的 properties2 / maintenance3 依赖由 1.1 满足。
    // 扩展不可枚举时保持未知；仅在列出且物理设备版本满足依赖时查询。
    char device_fault_why[96];
    out->device_fault_ext_state = extension_state(out->physical, VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
                                                   device_fault_why, sizeof device_fault_why);
    char descriptor_why[96] = "";
    out->descriptor_indexing_ext_state = extension_state(out->physical, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                                                          descriptor_why, sizeof descriptor_why);
    if (out->descriptor_indexing_ext_state < 0)
        S2A_DLOG("【红】vk 设备扩展 %s：枚举失败（%s），能力未知，按未启用处理\n",
                 VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, descriptor_why);
    int descriptor_query = out->descriptor_indexing_ext_state == 1 && out->properties.apiVersion >= VK_API_VERSION_1_1;
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptor_supported = {0};
    descriptor_supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptor_enabled = {0};
    descriptor_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {0};
    border.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
    const int has_cache_ctl = has_device_extension(out->physical, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT cache_ctl_supported = {0};
    cache_ctl_supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT;
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT cache_ctl_enabled = {0};
    cache_ctl_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT;
    const int has_imageless_ext = has_device_extension(out->physical, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
    VkPhysicalDeviceImagelessFramebufferFeaturesKHR imageless_feat = {0};
    imageless_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES_KHR;
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT view_2d_of_3d = {0};
    view_2d_of_3d.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT;
    VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT tba = {0};
    tba.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT;
    const int has_tba = has_device_extension(out->physical, VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME);
    // VK_KHR_maintenance4：Vulkan 1.1 基线下按扩展列出与开关（缺省 1）可选协商；只在列出扩展时挂查询链。
    VkPhysicalDeviceMaintenance4FeaturesKHR maint4 = {0};
    maint4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR;
    const int has_maint4 = has_device_extension(out->physical, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    const int want_maint4 = prop_int("debug.s2a.nvk_maintenance4", 1) != 0;
    // VK_KHR_shader_float16_int8：Vulkan 1.1 基线下按扩展列出与开关（缺省 1）可选协商；只在列出扩展时挂查询链。
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR f16i8 = {0};
    f16i8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR;
    const int has_f16i8 = has_device_extension(out->physical, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    const int want_f16i8 = prop_int("debug.s2a.nvk_shader_float16_int8", 1) != 0;
    {
        VkPhysicalDeviceFeatures2 query = {0};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        // 只属于扩展的特性结构只在设备列出该扩展时挂（规范：pNext 上的结构必须属于设备支持的版本或扩展；没列却挂，驱动多半忽略、位停在 0）。
        //   index_type_uint8 原先无条件挂进查询链（启用时才看扩展），#1879 的形态档假驱动按规范判红后发现
        const int has_u8 = has_device_extension(out->physical, "VK_EXT_index_type_uint8");
        const int has_2d_of_3d = has_device_extension(out->physical, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME);
        void **tail = &draw_parameters.pNext; *tail = NULL;
        *tail = &storage_16bit; tail = &storage_16bit.pNext; *tail = NULL;
        *tail = &var_ptrs; tail = &var_ptrs.pNext; *tail = NULL;
        *tail = &multiview; tail = &multiview.pNext; *tail = NULL;
        if (has_timeline) { *tail = &timeline; tail = &timeline.pNext; *tail = NULL; }
        if (has_sync2) { *tail = &sync2_feat; tail = &sync2_feat.pNext; *tail = NULL; }
        if (has_u8) { *tail = &index_u8; tail = &index_u8.pNext; *tail = NULL; }
        if (has_4444) { *tail = &f4444; tail = &f4444.pNext; *tail = NULL; }
        if (has_dcc) { *tail = &dcc; tail = &dcc.pNext; *tail = NULL; }
        if (has_zero_init_wg) { *tail = &zero_init_wg; tail = &zero_init_wg.pNext; *tail = NULL; }
        if (has_img_robust) { *tail = &img_robust; tail = &img_robust.pNext; *tail = NULL; }
        if (has_2d_of_3d) { *tail = &view_2d_of_3d; tail = &view_2d_of_3d.pNext; *tail = NULL; }
        if (has_fb_loop) { *tail = &fb_loop; tail = &fb_loop.pNext; *tail = NULL; }
        if (query_eds1) { *tail = &eds1; tail = &eds1.pNext; *tail = NULL; }
        if (query_eds2) { *tail = &eds2; tail = &eds2.pNext; *tail = NULL; }
        if (query_vi) { *tail = &vi; tail = &vi.pNext; *tail = NULL; }
        if (has_cache_ctl) { *tail = &cache_ctl_supported; tail = &cache_ctl_supported.pNext; *tail = NULL; }
        if (has_rp2) { *tail = &sep_ds; tail = &sep_ds.pNext; *tail = NULL; }
        if (has_imageless_ext) { *tail = &imageless_feat; tail = &imageless_feat.pNext; *tail = NULL; }
        if (has_hqr) { *tail = &hqr; tail = &hqr.pNext; *tail = NULL; }
        if (has_list_restart) { *tail = &list_restart; tail = &list_restart.pNext; *tail = NULL; }
        if (has_tba) { *tail = &tba; tail = &tba.pNext; *tail = NULL; }
        if (has_scalar_layout) { *tail = &scalar_layout; tail = &scalar_layout.pNext; *tail = NULL; }
        if (has_ubo_std) { *tail = &ubo_std; tail = &ubo_std.pNext; *tail = NULL; }
        if (has_line_rast) { *tail = &line_rast; tail = &line_rast.pNext; *tail = NULL; }
        if (has_maint4) { *tail = &maint4; tail = &maint4.pNext; *tail = NULL; }
        if (has_f16i8) { *tail = &f16i8; tail = &f16i8.pNext; *tail = NULL; }
        if (has_8bit_storage) { *tail = &storage_8bit; tail = &storage_8bit.pNext; *tail = NULL; }
        if (has_provoking) *tail = &provoking;
        query.pNext = &draw_parameters;
        if (out->has_swapchain_maintenance1) {
            maintenance.pNext = &draw_parameters;
            query.pNext = &maintenance;
        }
        if (out->placed_ext) { placed.pNext = query.pNext; query.pNext = &placed; }   // 只在列出扩展时挂查询结构（同下）
        if (has_device_extension(out->physical, "VK_EXT_custom_border_color")) {   // 只在 advertise 时挂查询结构
            border.pNext = query.pNext;
            query.pNext = &border;
        }
        if (descriptor_query) { descriptor_supported.pNext = query.pNext; query.pNext = &descriptor_supported; }
        vkGetPhysicalDeviceFeatures2(out->physical, &query);
        if (descriptor_query) {
            out->descriptor_indexing_runtime = descriptor_supported.runtimeDescriptorArray == VK_TRUE;
            out->descriptor_indexing_partial = descriptor_supported.descriptorBindingPartiallyBound == VK_TRUE;
            out->descriptor_indexing_sampled_nonuniform = descriptor_supported.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
            out->descriptor_indexing_storage_nonuniform = descriptor_supported.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
            out->descriptor_indexing_variable_count = descriptor_supported.descriptorBindingVariableDescriptorCount == VK_TRUE;
            out->descriptor_indexing_sampled_update_after_bind = descriptor_supported.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
        }
        out->swapchain_maintenance1_enabled = out->has_swapchain_maintenance1 &&
            instance_surface_maintenance1 && instance_surface_capabilities2 && maintenance.swapchainMaintenance1 == VK_TRUE;
        out->timeline_semaphore_enabled = has_timeline && timeline.timelineSemaphore == VK_TRUE;
        out->synchronization2_enabled = has_sync2 && sync2_feat.synchronization2 == VK_TRUE;
        out->shader_draw_parameters_enabled = draw_parameters.shaderDrawParameters == VK_TRUE;
        out->formats_4444_enabled = has_4444 && f4444.formatA4B4G4R4 == VK_TRUE;
        out->depth_clip_control_enabled = has_dcc && dcc.depthClipControl == VK_TRUE;
        out->attachment_feedback_loop_enabled = has_fb_loop && fb_loop.attachmentFeedbackLoopLayout == VK_TRUE;
        out->image_2d_view_of_3d_enabled = has_2d_of_3d && view_2d_of_3d.image2DViewOf3D == VK_TRUE;
        out->sampler_2d_view_of_3d_enabled = has_2d_of_3d && view_2d_of_3d.sampler2DViewOf3D == VK_TRUE;
        out->texel_buffer_alignment_enabled = has_tba && tba.texelBufferAlignment == VK_TRUE &&
                                              prop_int("debug.s2a.nvk_texel_buffer_alignment", 1) != 0;
        out->image_robustness_enabled = has_img_robust && img_robust.robustImageAccess == VK_TRUE &&
                                        prop_int("debug.s2a.nvk_image_robustness", 1) != 0;
        out->zero_init_workgroup_enabled = has_zero_init_wg &&
                                           zero_init_wg.shaderZeroInitializeWorkgroupMemory == VK_TRUE &&
                                           prop_int("debug.s2a.nvk_zero_init_workgroup", 1) != 0;
        out->scalar_block_layout_enabled = has_scalar_layout &&
                                           scalar_layout.scalarBlockLayout == VK_TRUE &&
                                           prop_int("debug.s2a.nvk_scalar_block_layout", 1) != 0;
        out->ubo_standard_layout_enabled = has_ubo_std &&
                                           ubo_std.uniformBufferStandardLayout == VK_TRUE &&
                                           prop_int("debug.s2a.nvk_ubo_std_layout", 1) != 0;
        {
            const int want_lr = has_line_rast && prop_int("debug.s2a.nvk_line_raster", 1) != 0;
            out->line_raster_rectangular = want_lr && line_rast.rectangularLines == VK_TRUE;
            out->line_raster_bresenham = want_lr && line_rast.bresenhamLines == VK_TRUE;
            out->line_raster_smooth = want_lr && line_rast.smoothLines == VK_TRUE;
            out->line_raster_stippled_rectangular = want_lr && line_rast.stippledRectangularLines == VK_TRUE;
            out->line_raster_stippled_bresenham = want_lr && line_rast.stippledBresenhamLines == VK_TRUE;
            out->line_raster_stippled_smooth = want_lr && line_rast.stippledSmoothLines == VK_TRUE;
            out->line_raster_enabled = out->line_raster_rectangular || out->line_raster_bresenham || out->line_raster_smooth ||
                                       out->line_raster_stippled_rectangular || out->line_raster_stippled_bresenham ||
                                       out->line_raster_stippled_smooth;
        }
        out->shader_float16_enabled = has_f16i8 && want_f16i8 && f16i8.shaderFloat16 == VK_TRUE;
        out->shader_int8_enabled = has_f16i8 && want_f16i8 && f16i8.shaderInt8 == VK_TRUE;
        out->placed_map = out->placed_ext && placed.memoryMapPlaced == VK_TRUE;
        out->placed_range = out->placed_ext && placed.memoryMapRangePlaced == VK_TRUE;
        out->placed_unmap_reserve = out->placed_ext && placed.memoryUnmapReserve == VK_TRUE;
        index_u8.pNext = NULL;   // 查询链拆掉；建设备的链在下面按启用情况重接
        fb_loop.pNext = NULL;
        img_robust.pNext = NULL;
        zero_init_wg.pNext = NULL;
        scalar_layout.pNext = NULL;
        ubo_std.pNext = NULL;
        line_rast.pNext = NULL;
    }
    VkPhysicalDeviceProvokingVertexPropertiesEXT provoking_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT,
    };
    if (has_provoking && provoking.provokingVertexLast) {
        VkPhysicalDeviceProperties2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                              .pNext = &provoking_props};
        vkGetPhysicalDeviceProperties2(out->physical, &query);
    }
    // 04881 也涵盖开遍时已经绑定的管线。前端可在遍内任意切模式，本路径要求逐管线支持。
    out->provoking_vertex_last_enabled = has_provoking && provoking.provokingVertexLast &&
                                          provoking_props.provokingVertexModePerPipeline;
    S2A_DLOG("[s2a] vk provoking vertex: ext=%d last=%u per_pipeline=%u enabled=%d\n",
             has_provoking, provoking.provokingVertexLast, provoking_props.provokingVertexModePerPipeline,
             out->provoking_vertex_last_enabled);
    const int enable_maint4 = has_maint4 && want_maint4 && maint4.maintenance4 == VK_TRUE;
    const int enable_f16i8 = out->shader_float16_enabled || out->shader_int8_enabled;
    // C2 顶点 SPIR-V 声明 DrawParameters（gl_VertexIndex / gl_InstanceIndex）；没有该特征不能合法消费模块。
    if (!out->shader_draw_parameters_enabled)
        return fail(why, why_size, "required feature missing: shaderDrawParameters");

    // 整组齐全才启用：缺任一项时可继续启动，但不承诺 pool/non-uniform 能力。
    // 存储图像非一致索引也必须一起启用：生成器（MarkNonUniform）一开就同时声明采样 + 存储两种能力，
    // 只启用采样会让 SPIR-V 的能力声明与设备不符（C1 的 profile 一致性核对按这条走）。
    const int descriptor_enable = descriptor_query && out->descriptor_indexing_runtime &&
                                  out->descriptor_indexing_partial && out->descriptor_indexing_sampled_nonuniform &&
                                  out->descriptor_indexing_storage_nonuniform;
    const int want_list_restart = prop_int("debug.s2a.nvk_list_restart", 1) != 0;
    const int list_restart_enable = has_list_restart && list_restart.primitiveTopologyListRestart == VK_TRUE && want_list_restart;
    const int want_hqr = prop_int("debug.s2a.nvk_host_query_reset", 1) != 0;
    const int hqr_enable = has_hqr && hqr.hostQueryReset == VK_TRUE && want_hqr;
    const int want_cache_ctl = prop_int("debug.s2a.nvk_pipe_cache_ctl", 1) != 0;
    const int cache_ctl_enable = want_cache_ctl && has_cache_ctl &&
                                 cache_ctl_supported.pipelineCreationCacheControl == VK_TRUE;
    const int want_8bit_storage = prop_int("debug.s2a.nvk_8bit_storage", 1) != 0;
    const int s8_ssbo = want_8bit_storage && has_8bit_storage && storage_8bit.storageBuffer8BitAccess == VK_TRUE;
    const int s8_ubo_ssbo = want_8bit_storage && has_8bit_storage && storage_8bit.uniformAndStorageBuffer8BitAccess == VK_TRUE;
    const int s8_push = want_8bit_storage && has_8bit_storage && storage_8bit.storagePushConstant8 == VK_TRUE;
    const int s8_enable = s8_ssbo || s8_ubo_ssbo || s8_push;
    // 按下面所有扩展追加点预留，含互斥的两个校准时间戳扩展。
    const char *device_ext[46];
    const int want_16bit_storage = prop_int("debug.s2a.nvk_16bit_storage", 1) != 0;
    const int s16_ssbo = want_16bit_storage && storage_16bit.storageBuffer16BitAccess == VK_TRUE;
    const int s16_ubo_ssbo = want_16bit_storage && storage_16bit.uniformAndStorageBuffer16BitAccess == VK_TRUE;
    const int s16_push = want_16bit_storage && storage_16bit.storagePushConstant16 == VK_TRUE;
    const int s16_io = want_16bit_storage && storage_16bit.storageInputOutput16 == VK_TRUE;
    const int s16_enable = s16_ssbo || s16_ubo_ssbo || s16_push || s16_io;
    // VUID-VkPhysicalDeviceVariablePointersFeatures-variablePointers-01431：启用 variablePointers 时必须同时启用 variablePointersStorageBuffer。
    const int want_var_ptrs = prop_int("debug.s2a.nvk_variable_pointers", 1) != 0;
    const int vp_sb = want_var_ptrs && var_ptrs.variablePointersStorageBuffer == VK_TRUE;
    const int vp_full = vp_sb && var_ptrs.variablePointers == VK_TRUE;
    // VUID-VkPhysicalDeviceMultiviewFeatures-multiviewGeometryShader-00580 / 00581：
    // multiviewGeometryShader 依赖 multiview && geometryShader；multiviewTessellationShader 依赖 multiview && tessellationShader。
    const int want_mv = prop_int("debug.s2a.nvk_multiview", 1) != 0;
    const int mv_en = want_mv && multiview.multiview == VK_TRUE;
    const int mv_gs = mv_en && enabled_features.geometryShader == VK_TRUE &&
                      multiview.multiviewGeometryShader == VK_TRUE;
    const int mv_ts = mv_en && enabled_features.tessellationShader == VK_TRUE &&
                      multiview.multiviewTessellationShader == VK_TRUE;
    uint32_t device_ext_count = 0;
    const int enable_eds1 = query_eds1 && eds1.extendedDynamicState == VK_TRUE;
    const int enable_eds2 = query_eds2 && eds2.extendedDynamicState2 == VK_TRUE;
    const int enable_vi = query_vi && vi.vertexInputDynamicState == VK_TRUE;
    if (enable_eds1) device_ext[device_ext_count++] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
    if (enable_eds2) device_ext[device_ext_count++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME;
    if (enable_vi) device_ext[device_ext_count++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    if (hqr_enable) device_ext[device_ext_count++] = VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME;
    if (list_restart_enable) device_ext[device_ext_count++] = VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME;
    if (s8_enable) device_ext[device_ext_count++] = VK_KHR_8BIT_STORAGE_EXTENSION_NAME;
    // 反馈没有独立的 feature 位。实例仍申请 1.1，物理设备报 1.3 也须实际启用 EXT。
    const int pipe_feedback_core = s2a_vk_effective_api(out->properties.apiVersion) >= VK_API_VERSION_1_3;
    const int pipe_feedback_ext = !pipe_feedback_core &&
        has_device_extension(out->physical, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    if (pipe_feedback_ext) device_ext[device_ext_count++] = VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME;
    if (cache_ctl_enable) device_ext[device_ext_count++] = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME;
    // apiVersion=1.1，不能拿物理设备支持 1.3 当作应用已启用核心 NONE。
    // 此扩展没有额外 feature 位；缺失时仍走独立快照，不能用 DONT_CARE 假代 NONE。
    out->load_store_op_none_enabled = has_device_extension(out->physical, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
    if (out->load_store_op_none_enabled) device_ext[device_ext_count++] = VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME;
    out->separate_depth_stencil_layouts_enabled = has_rp2 && sep_ds.separateDepthStencilLayouts;
    if (out->separate_depth_stencil_layouts_enabled) {
        device_ext[device_ext_count++] = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
        device_ext[device_ext_count++] = VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME;
    }
    S2A_DLOG("[s2a] vk read-only attachment: load_store_op_none=%d\n", out->load_store_op_none_enabled);
    if (out->attachment_feedback_loop_enabled) device_ext[device_ext_count++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
    S2A_DLOG("[s2a] vk attachment feedback loop: enabled=%d\n", out->attachment_feedback_loop_enabled);
    S2A_DLOG("[s2a] vk separate depth/stencil: ext=%d rp2=%d feature=%d enabled=%d\n",
             has_sep_ds, has_rp2, sep_ds.separateDepthStencilLayouts != VK_FALSE, out->separate_depth_stencil_layouts_enabled);
    if (out->has_swapchain) device_ext[device_ext_count++] = "VK_KHR_swapchain";
    // 格式列表（VkImageFormatListCreateInfo）：可改格式的颜色图按列表建，Adreno 才保 UBWC 帧缓冲压缩（backend_vulkan.c tex_fmt_allowed 一节）
    out->image_format_list_enabled = has_device_extension(out->physical, "VK_KHR_image_format_list");
    if (out->image_format_list_enabled) device_ext[device_ext_count++] = "VK_KHR_image_format_list";
    // 无图帧缓冲（VK_KHR_imageless_framebuffer）：Vulkan 1.1 下依赖 VK_KHR_image_format_list 与 imagelessFramebuffer 特性。
    out->imageless_framebuffer_enabled = has_imageless_ext && out->image_format_list_enabled &&
                                         imageless_feat.imagelessFramebuffer == VK_TRUE;
    if (out->imageless_framebuffer_enabled) device_ext[device_ext_count++] = VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME;
    S2A_DLOG("[s2a] vk imageless framebuffer: ext=%d fmt_list=%d feat=%d enabled=%d\n",
             has_imageless_ext, out->image_format_list_enabled,
             imageless_feat.imagelessFramebuffer == VK_TRUE, out->imageless_framebuffer_enabled);
    if (out->swapchain_maintenance1_enabled) device_ext[device_ext_count++] = "VK_EXT_swapchain_maintenance1";
    if (has_timeline) device_ext[device_ext_count++] = "VK_KHR_timeline_semaphore";
    if (out->synchronization2_enabled) device_ext[device_ext_count++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
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
    // B2：display_timing 在驱动 advertise 且 debug.s2a.display_timing=1 时才启用——默认关：
    // 默认路径不请求扩展、不挂 presentID、不轮询（同 frame_log/gpu_time 惯例）。
    // has_display_timing = advertise ∧ 本局已启用，cap 位与 attach 的 supported 都按可用性如实报。
    out->has_display_timing = has_device_extension(out->physical, "VK_GOOGLE_display_timing") &&
                              prop_int("debug.s2a.display_timing", 0) == 1;
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
    int indirect_count_ext = has_device_extension(out->physical, "VK_KHR_draw_indirect_count");
    if (indirect_count_ext) device_ext[device_ext_count++] = "VK_KHR_draw_indirect_count";
    // VK_KHR_push_descriptor（8E Adreno 830 / 9400 Mali-G925 支持；9000 不支持自动零退化回落）：
    // 仅在物理设备列出扩展时才挂 VkPhysicalDevicePushDescriptorPropertiesKHR 查上限，严守 Vulkan 1.1 底线。
    const int push_desc_ext = has_device_extension(out->physical, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (push_desc_ext) {
        VkPhysicalDevicePushDescriptorPropertiesKHR pdp = {0};
        pdp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &pdp;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        out->max_push_descriptors = pdp.maxPushDescriptors;
        device_ext[device_ext_count++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    }
    // Vulkan 1.1 同时满足 EXT 与 KHR 的依赖。优先保留可用的 EXT 路径；只列 KHR 的设备单独查询其属性。
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vdiv = {0};
    vdiv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
    const int has_vdiv_ext = has_device_extension(out->physical, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    const int has_vdiv_khr = has_device_extension(out->physical, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    const char *vdiv_name = NULL;
    if (has_vdiv_ext || has_vdiv_khr) {
        VkPhysicalDeviceFeatures2 q = {0};
        q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        q.pNext = &vdiv;
        vkGetPhysicalDeviceFeatures2(out->physical, &q);
    }
    if (has_vdiv_ext && vdiv.vertexAttributeInstanceRateDivisor) {
        VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT vdp = {0};
        vdp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &vdp};
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        if (vdp.maxVertexAttribDivisor > 1) {
            vdiv_name = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
            out->max_vertex_attrib_divisor = vdp.maxVertexAttribDivisor;
            out->vertex_divisor_nonzero_first_instance = 1;   // EXT 不限制 firstInstance
        }
    }
    if (!vdiv_name && has_vdiv_khr && vdiv.vertexAttributeInstanceRateDivisor) {
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR vdp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR,
        };
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &vdp};
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        if (vdp.maxVertexAttribDivisor > 1) {
            vdiv_name = VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
            out->max_vertex_attrib_divisor = vdp.maxVertexAttribDivisor;
            out->vertex_divisor_nonzero_first_instance = vdp.supportsNonZeroFirstInstance == VK_TRUE;
        }
    }
    out->vertex_divisor_enabled = vdiv_name != NULL;
    if (vdiv_name) device_ext[device_ext_count++] = vdiv_name;
    fprintf(stderr, "[s2a] vk vertex divisor: enabled=%d max=%u path=%s nonzero_first=%d\n", out->vertex_divisor_enabled,
            out->max_vertex_attrib_divisor, vdiv_name ? vdiv_name : "none", out->vertex_divisor_nonzero_first_instance);
    // #976：NVN 的 MIRROR_CLAMP_TO_EDGE 环绕。扩展没有特性结构，advertise 即可启用；没有时建这种采样器报 UNSUPPORTED。
    out->sampler_mirror_clamp_enabled = has_device_extension(out->physical, "VK_KHR_sampler_mirror_clamp_to_edge");
    if (out->sampler_mirror_clamp_enabled) device_ext[device_ext_count++] = "VK_KHR_sampler_mirror_clamp_to_edge";
    if (out->formats_4444_enabled) device_ext[device_ext_count++] = "VK_EXT_4444_formats";
    if (out->depth_clip_control_enabled) device_ext[device_ext_count++] = "VK_EXT_depth_clip_control";
    if (out->provoking_vertex_last_enabled) device_ext[device_ext_count++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
    if (out->image_2d_view_of_3d_enabled || out->sampler_2d_view_of_3d_enabled)
        device_ext[device_ext_count++] = VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME;
    S2A_DLOG("[s2a] vk image 2d view of 3d: image=%d sampler=%d\n",
             out->image_2d_view_of_3d_enabled, out->sampler_2d_view_of_3d_enabled);
    out->storage_texel_align_bytes = 0;
    out->storage_texel_single_align = 0;
    out->uniform_texel_align_bytes = 0;
    out->uniform_texel_single_align = 0;
    if (out->texel_buffer_alignment_enabled) {
        device_ext[device_ext_count++] = VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME;
        VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT tbp = {0};
        tbp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &tbp;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        out->storage_texel_align_bytes = tbp.storageTexelBufferOffsetAlignmentBytes;
        out->storage_texel_single_align = tbp.storageTexelBufferOffsetSingleTexelAlignment == VK_TRUE;
        out->uniform_texel_align_bytes = tbp.uniformTexelBufferOffsetAlignmentBytes;
        out->uniform_texel_single_align = tbp.uniformTexelBufferOffsetSingleTexelAlignment == VK_TRUE;
    }
    S2A_DLOG("[s2a] vk texel buffer alignment: enabled=%d storage=%llu(single=%d) uniform=%llu(single=%d)\n",
             out->texel_buffer_alignment_enabled,
             (unsigned long long)out->storage_texel_align_bytes, out->storage_texel_single_align,
             (unsigned long long)out->uniform_texel_align_bytes, out->uniform_texel_single_align);
    if (out->image_robustness_enabled) device_ext[device_ext_count++] = VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME;
    S2A_DLOG("[s2a] vk image robustness: enabled=%d\n", out->image_robustness_enabled);
    if (out->zero_init_workgroup_enabled) device_ext[device_ext_count++] = VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME;
    S2A_DLOG("[s2a] vk zero_init_workgroup: enabled=%d\n", out->zero_init_workgroup_enabled);
    if (out->scalar_block_layout_enabled) device_ext[device_ext_count++] = VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME;
    S2A_DLOG("[s2a] vk scalar_block_layout: enabled=%d\n", out->scalar_block_layout_enabled);
    if (out->ubo_standard_layout_enabled) device_ext[device_ext_count++] = VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME;
    S2A_DLOG("[s2a] vk ubo_std_layout: enabled=%d\n", out->ubo_standard_layout_enabled);
    if (out->line_raster_enabled) {
        VkPhysicalDeviceLineRasterizationPropertiesEXT lrp = {0};
        lrp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &lrp;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        out->line_subpixel_bits = lrp.lineSubPixelPrecisionBits;
        device_ext[device_ext_count++] = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
    }
    // #976：NVN 的任意边框色。要 customBorderColors 特性；customBorderColorWithoutFormat 按设备如实开（没有时，视图格式未知的采样器使用最近的内置边框色）。
    out->custom_border_enabled = border.customBorderColors == VK_TRUE;
    out->custom_border_without_format = out->custom_border_enabled && border.customBorderColorWithoutFormat == VK_TRUE;
    out->custom_border_max = 0;
    if (out->custom_border_enabled) {
        device_ext[device_ext_count++] = "VK_EXT_custom_border_color";
        VkPhysicalDeviceCustomBorderColorPropertiesEXT bp = {0};
        bp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &bp;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        out->custom_border_max = bp.maxCustomBorderColorSamplers;
    }
    if (descriptor_enable) device_ext[device_ext_count++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
    // #976：NVN 采样器的 min / max 归约。扩展没有特性结构；属性里的 filterMinmaxImageComponentMapping 决定归约结果的可用分量。
    out->sampler_minmax_enabled = has_device_extension(out->physical, "VK_EXT_sampler_filter_minmax");
    if (out->sampler_minmax_enabled) {
        VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT mp = {0};
        mp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &mp;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        out->minmax_image_mapping = mp.filterMinmaxImageComponentMapping == VK_TRUE;
        device_ext[device_ext_count++] = "VK_EXT_sampler_filter_minmax";
    }
    fprintf(stderr, "[s2a] vk sampler minmax: enabled=%d image_mapping=%d\n", out->sampler_minmax_enabled, out->minmax_image_mapping);
    if (enable_maint4) {
        device_ext[device_ext_count++] = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
        VkPhysicalDeviceMaintenance4PropertiesKHR m4p = {0};
        m4p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &m4p;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        out->max_buffer_size = m4p.maxBufferSize;
    }
    if (enable_f16i8) device_ext[device_ext_count++] = VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME;
    // VK_KHR_shader_float_controls：Vulkan 1.1 基线下按扩展列出与开关（缺省 1）可选协商；只在列出扩展时查询属性。
    const int has_fctrl = has_device_extension(out->physical, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    const int want_fctrl = prop_int("debug.s2a.nvk_shader_float_controls", 1) != 0;
    out->float_controls_enabled = has_fctrl && want_fctrl;
    out->float_controls_mask = 0;
    if (out->float_controls_enabled) {
        device_ext[device_ext_count++] = VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME;
        VkPhysicalDeviceFloatControlsPropertiesKHR fcp = {0};
        fcp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &fcp;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        uint32_t m = 0;
        if (fcp.shaderDenormPreserveFloat16) m |= 1u << 0;
        if (fcp.shaderDenormPreserveFloat32) m |= 1u << 1;
        if (fcp.shaderDenormPreserveFloat64) m |= 1u << 2;
        if (fcp.shaderDenormFlushToZeroFloat16) m |= 1u << 3;
        if (fcp.shaderDenormFlushToZeroFloat32) m |= 1u << 4;
        if (fcp.shaderDenormFlushToZeroFloat64) m |= 1u << 5;
        if (fcp.shaderSignedZeroInfNanPreserveFloat16) m |= 1u << 6;
        if (fcp.shaderSignedZeroInfNanPreserveFloat32) m |= 1u << 7;
        if (fcp.shaderSignedZeroInfNanPreserveFloat64) m |= 1u << 8;
        if (fcp.shaderRoundingModeRTEFloat16) m |= 1u << 9;
        if (fcp.shaderRoundingModeRTEFloat32) m |= 1u << 10;
        if (fcp.shaderRoundingModeRTEFloat64) m |= 1u << 11;
        if (fcp.shaderRoundingModeRTZFloat16) m |= 1u << 12;
        if (fcp.shaderRoundingModeRTZFloat32) m |= 1u << 13;
        if (fcp.shaderRoundingModeRTZFloat64) m |= 1u << 14;
        out->float_controls_mask = m;
    }
    S2A_DLOG("[s2a] vk shader_float_controls: ext=%d want=%d enabled=%d mask=0x%04x\n",
             has_fctrl, want_fctrl, out->float_controls_enabled, out->float_controls_mask);
    // VK_EXT_global_priority（开关 debug.s2a.nvk_global_priority 缺省 0，便于真机 A/B 测帧时抖动）：
    // 仅当设备列出扩展且开关非 0 时请求 HIGH 队列优先级；若驱动/内核拒绝（VK_ERROR_NOT_PERMITTED_EXT），
    // 下方建设备时自动撤回扩展与队列 pNext 重试默认队列，不因此启动失败。
    out->has_global_priority = has_device_extension(out->physical, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
    const int want_global_priority = out->has_global_priority && prop_int("debug.s2a.nvk_global_priority", 0) != 0;
    const uint32_t global_priority_ext_idx = device_ext_count;
    if (want_global_priority) device_ext[device_ext_count++] = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
    out->device_extension_count = device_ext_count;

    // #975：NVN 16 位打包格式的路由（格式表在 nvn_vk_format.c 的 k_p16，弱引用同 #398 深度替换）。
    // 精确格式要扩展的：A4B4G4R4 看上面 4444 是否启用；A1B5G5R5 要 VK_KHR_maintenance5，本设备按 Vulkan 1.1 建，
    // 它的依赖链（dynamic_rendering → depth_stencil_resolve → create_renderpass2）这里不开，按不可用处理（走退回格式）。
    {
        extern int nvn_vk_p16_entry(int, uint32_t *, VkFormat *, VkFormat *) __attribute__((weak));
        extern uint32_t nvn_vk_p16_fallback_pick(const VkFormatFeatureFlags *, const VkFormatFeatureFlags *) __attribute__((weak));
        extern void nvn_vk_format_set_p16_fallback(uint32_t) __attribute__((weak));
        VkFormatFeatureFlags ex[8] = {0}, fb[8] = {0};   // 8 = NVN_VK_P16_N
        VkFormat e = VK_FORMAT_UNDEFINED, f = VK_FORMAT_UNDEFINED;
        for (int i = 0; nvn_vk_p16_entry && i < 8 && nvn_vk_p16_entry(i, NULL, &e, &f); i++) {
            VkFormatProperties fp;
            int gated = (e == VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT && !out->formats_4444_enabled) || e == VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR;
            if (!gated) { s2a_vk_format_properties(out->physical, e, &fp); ex[i] = fp.optimalTilingFeatures; }
            if (f != VK_FORMAT_UNDEFINED) { s2a_vk_format_properties(out->physical, f, &fp); fb[i] = fp.optimalTilingFeatures; }
        }
        out->p16_fallback_mask = nvn_vk_p16_fallback_pick ? nvn_vk_p16_fallback_pick(ex, fb) : 0;
        if (nvn_vk_format_set_p16_fallback) nvn_vk_format_set_p16_fallback(out->p16_fallback_mask);
        fprintf(stderr, "[s2a] vk packed16 route: 4444=%d fallback=0x%02x\n", out->formats_4444_enabled, out->p16_fallback_mask);
    {   // #1070 placed 探针：对齐值与单次分配上限（属性链；只读，不改任何启用项）
        VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = {0}; pp.sType = S2A_STYPE_PLACED_PROPERTIES;
        VkPhysicalDeviceMaintenance3Properties m3 = {0}; m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
        if (out->placed_ext) m3.pNext = &pp;
        VkPhysicalDeviceProperties2 p2 = {0}; p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &m3;
        vkGetPhysicalDeviceProperties2(out->physical, &p2);
        out->placed_align = out->placed_ext ? (uint64_t)pp.minPlacedMemoryMapAlignment : 0;
        out->max_alloc = (uint64_t)m3.maxMemoryAllocationSize;
        fprintf(stderr, "[s2a] vk placed map: ext=%d map2=%d placed=%d range=%d unmap_reserve=%d align=%llu max_alloc=%llu（只查未启用）\n",
                out->placed_ext, out->map_memory2_ext, out->placed_map, out->placed_range, out->placed_unmap_reserve,
                (unsigned long long)out->placed_align, (unsigned long long)out->max_alloc);
    }
    }

    float priority = 1.0f;
    VkDeviceQueueGlobalPriorityCreateInfoEXT queue_prio = {0};
    queue_prio.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT;
    queue_prio.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
    VkDeviceQueueCreateInfo queues[2] = {
        { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = out->queue_family,
          .queueCount = 1, .pQueuePriorities = &priority },
    };
    uint32_t queue_count = 1;
    if (enabled_features.sparseBinding && sparse_family != out->queue_family) {
        queues[1] = queues[0];
        queues[1].queueFamilyIndex = sparse_family;
        queue_count++;
    }
    // 稀疏绑定的独立队列仍走默认优先级。
    queues[0].pNext = want_global_priority ? &queue_prio : NULL;
    VkDeviceCreateInfo device_info = {0};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = queue_count;
    device_info.pQueueCreateInfos = queues;
    device_info.pEnabledFeatures = &enabled_features;
    device_info.enabledExtensionCount = device_ext_count;
    device_info.ppEnabledExtensionNames = device_ext;
    draw_parameters.shaderDrawParameters = VK_TRUE;
    draw_parameters.pNext = has_timeline ? (VkBaseInStructure *)&timeline
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
    if (out->vertex_divisor_enabled) {   // 接在链尾：只开实例除数（零除数用不到：NVN 除数 0 = 逐顶点）
        vdiv.vertexAttributeInstanceRateDivisor = VK_TRUE;
        vdiv.vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
        vdiv.pNext = NULL;
        VkBaseOutStructure *tail = (VkBaseOutStructure *)&device_info;
        while (tail->pNext) tail = tail->pNext;
        tail->pNext = (VkBaseOutStructure *)&vdiv;
    }
    if (out->formats_4444_enabled) {   // 接在链尾：只开 A4B4G4R4（A4R4G4B4 用不到）
        f4444.formatA4B4G4R4 = VK_TRUE;
        f4444.formatA4R4G4B4 = VK_FALSE;
        f4444.pNext = NULL;
        VkBaseOutStructure *tail = (VkBaseOutStructure *)device_info.pNext;
        while (tail->pNext) tail = tail->pNext;
        tail->pNext = (VkBaseOutStructure *)&f4444;
    }
    if (out->depth_clip_control_enabled) {   // 同样接在链尾（查询时它挂在 index_u8 后面，这里重接）
        dcc.depthClipControl = VK_TRUE;
        dcc.pNext = NULL;
        VkBaseOutStructure *tail = (VkBaseOutStructure *)device_info.pNext;
        while (tail->pNext) tail = tail->pNext;
        tail->pNext = (VkBaseOutStructure *)&dcc;
    }
    if (out->attachment_feedback_loop_enabled) {
        fb_loop.attachmentFeedbackLoopLayout = VK_TRUE;
        fb_loop.pNext = NULL;
        VkBaseOutStructure *tail = (VkBaseOutStructure *)device_info.pNext;
        while (tail->pNext) tail = tail->pNext;
        tail->pNext = (VkBaseOutStructure *)&fb_loop;
    }
    if (out->image_2d_view_of_3d_enabled || out->sampler_2d_view_of_3d_enabled) {
        view_2d_of_3d.image2DViewOf3D = out->image_2d_view_of_3d_enabled ? VK_TRUE : VK_FALSE;
        view_2d_of_3d.sampler2DViewOf3D = out->sampler_2d_view_of_3d_enabled ? VK_TRUE : VK_FALSE;
        view_2d_of_3d.pNext = (void *)device_info.pNext;
        device_info.pNext = &view_2d_of_3d;
    }
    if (out->texel_buffer_alignment_enabled) {
        tba.texelBufferAlignment = VK_TRUE;
        tba.pNext = NULL;
        VkBaseOutStructure *tail = (VkBaseOutStructure *)device_info.pNext;
        while (tail->pNext) tail = tail->pNext;
        tail->pNext = (VkBaseOutStructure *)&tba;
    }
    if (out->image_robustness_enabled) {
        img_robust.robustImageAccess = VK_TRUE;
        img_robust.pNext = (void *)device_info.pNext;
        device_info.pNext = &img_robust;
    }
    if (out->zero_init_workgroup_enabled) {
        zero_init_wg.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
        zero_init_wg.pNext = (void *)device_info.pNext;
        device_info.pNext = &zero_init_wg;
    }
    if (out->scalar_block_layout_enabled) {
        scalar_layout.scalarBlockLayout = VK_TRUE;
        scalar_layout.pNext = (void *)device_info.pNext;
        device_info.pNext = &scalar_layout;
    }
    if (out->ubo_standard_layout_enabled) {
        ubo_std.uniformBufferStandardLayout = VK_TRUE;
        ubo_std.pNext = (void *)device_info.pNext;
        device_info.pNext = &ubo_std;
    }
    if (out->line_raster_enabled) {
        line_rast.rectangularLines = out->line_raster_rectangular ? VK_TRUE : VK_FALSE;
        line_rast.bresenhamLines = out->line_raster_bresenham ? VK_TRUE : VK_FALSE;
        line_rast.smoothLines = out->line_raster_smooth ? VK_TRUE : VK_FALSE;
        line_rast.stippledRectangularLines = out->line_raster_stippled_rectangular ? VK_TRUE : VK_FALSE;
        line_rast.stippledBresenhamLines = out->line_raster_stippled_bresenham ? VK_TRUE : VK_FALSE;
        line_rast.stippledSmoothLines = out->line_raster_stippled_smooth ? VK_TRUE : VK_FALSE;
        line_rast.pNext = (void *)device_info.pNext;
        device_info.pNext = &line_rast;
    }
    if (out->custom_border_enabled) {
        border.customBorderColors = VK_TRUE;
        border.customBorderColorWithoutFormat = out->custom_border_without_format ? VK_TRUE : VK_FALSE;
        border.pNext = (void *)device_info.pNext;
        device_info.pNext = &border;
    }
    if (descriptor_enable) {
        // 用全零的新结构，避免把查询结果中其他已支持但当前不用的位误传为启用。
        descriptor_enabled.runtimeDescriptorArray = VK_TRUE;
        descriptor_enabled.descriptorBindingPartiallyBound = VK_TRUE;
        descriptor_enabled.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        descriptor_enabled.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        descriptor_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &descriptor_enabled;
    }
    if (enable_eds1) {
        eds1.pNext = (void *)device_info.pNext;
        device_info.pNext = &eds1;
    }
    if (enable_eds2) {
        eds2.extendedDynamicState2LogicOp = VK_FALSE;
        eds2.extendedDynamicState2PatchControlPoints = VK_FALSE;
        eds2.pNext = (void *)device_info.pNext;
        device_info.pNext = &eds2;
    }
    if (enable_vi) {
        vi.pNext = (void *)device_info.pNext;
        device_info.pNext = &vi;
    }
    if (out->provoking_vertex_last_enabled) {
        provoking.provokingVertexLast = VK_TRUE;
        provoking.transformFeedbackPreservesProvokingVertex = VK_FALSE;
        provoking.pNext = (void *)device_info.pNext;
        device_info.pNext = &provoking;
    }
    if (cache_ctl_enable) {
        cache_ctl_enabled.pipelineCreationCacheControl = VK_TRUE;
        cache_ctl_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &cache_ctl_enabled;
    }
    if (out->separate_depth_stencil_layouts_enabled) {
        sep_ds.pNext = (void *)device_info.pNext;
        device_info.pNext = &sep_ds;
    }
    VkPhysicalDeviceImagelessFramebufferFeaturesKHR imageless_en = {0};
    if (out->imageless_framebuffer_enabled) {
        imageless_en.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES_KHR;
        imageless_en.imagelessFramebuffer = VK_TRUE;
        imageless_en.pNext = (void *)device_info.pNext;
        device_info.pNext = &imageless_en;
    }
    VkPhysicalDeviceHostQueryResetFeaturesEXT hqr_enabled = {0};
    hqr_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT;
    if (hqr_enable) {
        hqr_enabled.hostQueryReset = VK_TRUE;
        hqr_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &hqr_enabled;
    }
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT list_restart_enabled = {0};
    list_restart_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT;
    if (list_restart_enable) {
        list_restart_enabled.primitiveTopologyListRestart = VK_TRUE;
        list_restart_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &list_restart_enabled;
    }
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2_enable = {0};
    if (out->synchronization2_enabled) {
        sync2_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
        sync2_enable.synchronization2 = VK_TRUE;
        sync2_enable.pNext = (void *)device_info.pNext;
        device_info.pNext = &sync2_enable;
    }
    VkPhysicalDeviceMaintenance4FeaturesKHR maint4_enable = {0};
    if (enable_maint4) {
        maint4_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR;
        maint4_enable.maintenance4 = VK_TRUE;
        maint4_enable.pNext = (void *)device_info.pNext;
        device_info.pNext = &maint4_enable;
    }
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR f16i8_enable = {0};
    if (enable_f16i8) {
        f16i8_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR;
        f16i8_enable.shaderFloat16 = out->shader_float16_enabled ? VK_TRUE : VK_FALSE;
        f16i8_enable.shaderInt8 = out->shader_int8_enabled ? VK_TRUE : VK_FALSE;
        f16i8_enable.pNext = (void *)device_info.pNext;
        device_info.pNext = &f16i8_enable;
    }
    VkPhysicalDevice8BitStorageFeaturesKHR s8_enabled = {0};
    s8_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR;
    if (s8_enable) {
        s8_enabled.storageBuffer8BitAccess = s8_ssbo ? VK_TRUE : VK_FALSE;
        s8_enabled.uniformAndStorageBuffer8BitAccess = s8_ubo_ssbo ? VK_TRUE : VK_FALSE;
        s8_enabled.storagePushConstant8 = s8_push ? VK_TRUE : VK_FALSE;
        s8_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &s8_enabled;
    }
    VkPhysicalDevice16BitStorageFeatures s16_enabled = {0};
    s16_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    if (s16_enable) {
        s16_enabled.storageBuffer16BitAccess = s16_ssbo ? VK_TRUE : VK_FALSE;
        s16_enabled.uniformAndStorageBuffer16BitAccess = s16_ubo_ssbo ? VK_TRUE : VK_FALSE;
        s16_enabled.storagePushConstant16 = s16_push ? VK_TRUE : VK_FALSE;
        s16_enabled.storageInputOutput16 = s16_io ? VK_TRUE : VK_FALSE;
        s16_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &s16_enabled;
    }
    VkPhysicalDeviceVariablePointersFeatures vp_enabled = {0};
    vp_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES;
    if (vp_sb) {
        vp_enabled.variablePointersStorageBuffer = VK_TRUE;
        vp_enabled.variablePointers = vp_full ? VK_TRUE : VK_FALSE;
        vp_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &vp_enabled;
    }
    VkPhysicalDeviceMultiviewFeatures mv_enabled = {0};
    mv_enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    if (mv_en) {
        mv_enabled.multiview = VK_TRUE;
        mv_enabled.multiviewGeometryShader = mv_gs ? VK_TRUE : VK_FALSE;
        mv_enabled.multiviewTessellationShader = mv_ts ? VK_TRUE : VK_FALSE;
        mv_enabled.pNext = (void *)device_info.pNext;
        device_info.pNext = &mv_enabled;
    }
    VkResult dev_rc = vkCreateDevice(out->physical, &device_info, NULL, &out->device);
    if (dev_rc == VK_ERROR_NOT_PERMITTED_EXT && want_global_priority) {
        S2A_DLOG("[s2a] vk global priority HIGH rejected (VK_ERROR_NOT_PERMITTED_EXT), retrying default queue priority\n");
        queues[0].pNext = NULL;
        for (uint32_t i = global_priority_ext_idx; i + 1 < device_ext_count; i++)
            device_ext[i] = device_ext[i + 1];
        device_ext_count--;
        device_info.enabledExtensionCount = device_ext_count;
        out->device_extension_count = device_ext_count;
        out->global_priority_not_permitted = 1;
        dev_rc = vkCreateDevice(out->physical, &device_info, NULL, &out->device);
    }
    if (dev_rc != VK_SUCCESS)
        return fail(why, why_size, "vkCreateDevice failed");
    out->global_priority_enabled = want_global_priority && !out->global_priority_not_permitted;
    out->queue_global_priority = out->global_priority_enabled
        ? VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT
        : VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT;
    S2A_DLOG("[s2a] vk global priority: has_ext=%d want=%d enabled=%d not_permitted=%d prio=%d\n",
             out->has_global_priority, want_global_priority, out->global_priority_enabled,
             out->global_priority_not_permitted, (int)out->queue_global_priority);
    if (out->synchronization2_enabled) {
        out->cmd_pipeline_barrier2 = (PFN_vkCmdPipelineBarrier2KHR)vkGetDeviceProcAddr(out->device, "vkCmdPipelineBarrier2KHR");
        if (!out->cmd_pipeline_barrier2) out->synchronization2_enabled = 0;
    }
    S2A_DLOG("[s2a] vk synchronization2: ext=%d feat=%d enabled=%d fn=%d\n",
             has_sync2, sync2_feat.synchronization2 == VK_TRUE,
             out->synchronization2_enabled, out->cmd_pipeline_barrier2 != NULL);
    out->primitive_topology_list_restart_enabled = list_restart_enable;
    S2A_DLOG("[s2a] vk list restart: ext=%d feature=%d want=%d enabled=%d\n",
             has_list_restart, list_restart.primitiveTopologyListRestart == VK_TRUE, want_list_restart,
             out->primitive_topology_list_restart_enabled);
    if (hqr_enable) {
        out->reset_query_pool = (PFN_vkResetQueryPoolEXT)vkGetDeviceProcAddr(out->device, "vkResetQueryPoolEXT");
        out->host_query_reset_enabled = out->reset_query_pool != NULL;
    }
    S2A_DLOG("[s2a] vk host query reset: ext=%d feature=%d want=%d entry=%d enabled=%d\n",
             has_hqr, hqr.hostQueryReset == VK_TRUE, want_hqr, out->reset_query_pool != NULL, out->host_query_reset_enabled);
    out->eds1_enabled = enable_eds1;
    out->eds2_enabled = enable_eds2;
    out->vertex_input_dynamic_enabled = enable_vi;
#define LOAD_EDS1(name) \
    out->name = (PFN_##name)vkGetDeviceProcAddr(out->device, #name); \
    out->eds1_enabled &= out->name != NULL;
#define LOAD_EDS2(name) \
    out->name = (PFN_##name)vkGetDeviceProcAddr(out->device, #name); \
    out->eds2_enabled &= out->name != NULL;
    if (enable_eds1) { S2A_VK_EDS1_COMMANDS(LOAD_EDS1) }
    if (enable_eds2) { S2A_VK_EDS2_COMMANDS(LOAD_EDS2) }
#undef LOAD_EDS1
#undef LOAD_EDS2
    if (enable_vi) {
        out->vkCmdSetVertexInputEXT = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(out->device, "vkCmdSetVertexInputEXT");
        out->vertex_input_dynamic_enabled = out->vkCmdSetVertexInputEXT != NULL;
    }
    if (want_eds1 || want_eds2 || want_vi)
        S2A_DLOG("[s2a] vk pipeline dynamic state: eds1=%d/%d eds2=%d/%d vertex_input=%d/%d (enabled/requested)\n",
                 out->eds1_enabled, want_eds1, out->eds2_enabled, want_eds2, out->vertex_input_dynamic_enabled, want_vi);
    S2A_DLOG("[s2a] vk shader_float16_int8: ext=%d want=%d fp16=%d int8=%d\n",
             has_f16i8, want_f16i8, out->shader_float16_enabled, out->shader_int8_enabled);
    out->storage_8bit_ssbo_enabled = s8_ssbo;
    out->storage_8bit_ubo_ssbo_enabled = s8_ubo_ssbo;
    out->storage_8bit_push_enabled = s8_push;
    S2A_DLOG("[s2a] vk 8bit storage: ext=%d want=%d ssbo=%d ubo_ssbo=%d push=%d\n",
             has_8bit_storage, want_8bit_storage, out->storage_8bit_ssbo_enabled,
             out->storage_8bit_ubo_ssbo_enabled, out->storage_8bit_push_enabled);
    out->storage_16bit_ssbo_enabled = s16_ssbo;
    out->storage_16bit_ubo_ssbo_enabled = s16_ubo_ssbo;
    out->storage_16bit_push_enabled = s16_push;
    out->storage_16bit_io_enabled = s16_io;
    S2A_DLOG("[s2a] vk 16bit storage: want=%d ssbo=%d ubo_ssbo=%d push=%d io=%d\n",
             want_16bit_storage, out->storage_16bit_ssbo_enabled, out->storage_16bit_ubo_ssbo_enabled,
             out->storage_16bit_push_enabled, out->storage_16bit_io_enabled);
    out->variable_pointers_storage_buffer_enabled = vp_sb;
    out->variable_pointers_enabled = vp_full;
    S2A_DLOG("[s2a] vk variable pointers: want=%d storage_buffer=%d full=%d\n",
             want_var_ptrs, out->variable_pointers_storage_buffer_enabled, out->variable_pointers_enabled);
    out->multiview_enabled = mv_en;
    out->multiview_geometry_shader_enabled = mv_gs;
    out->multiview_tessellation_shader_enabled = mv_ts;
    S2A_DLOG("[s2a] vk multiview: want=%d mv=%d gs=%d ts=%d\n",
             want_mv, out->multiview_enabled, out->multiview_geometry_shader_enabled,
             out->multiview_tessellation_shader_enabled);
    out->pipeline_creation_feedback_enabled = pipe_feedback_core || pipe_feedback_ext;
    out->pipeline_creation_cache_control_enabled = cache_ctl_enable;
    S2A_DLOG("[s2a] vk pipeline cache control: ext=%d feature=%d want=%d enabled=%d\n",
             has_cache_ctl, cache_ctl_supported.pipelineCreationCacheControl == VK_TRUE,
             want_cache_ctl, out->pipeline_creation_cache_control_enabled);
    out->descriptor_indexing_enabled = descriptor_enable;
    S2A_DLOG("[s2a] vk descriptor indexing: ext_state=%d queried=%d runtime=%d partial=%d sampled_nonuniform=%d "
             "storage_nonuniform=%d variable_count=%d sampled_update_after_bind=%d enabled=%d "
             "stage_sampled=%u stage_samplers=%u set_sampled=%u set_samplers=%u\n",
             out->descriptor_indexing_ext_state, descriptor_query, out->descriptor_indexing_runtime,
             out->descriptor_indexing_partial, out->descriptor_indexing_sampled_nonuniform,
             out->descriptor_indexing_storage_nonuniform, out->descriptor_indexing_variable_count,
             out->descriptor_indexing_sampled_update_after_bind, out->descriptor_indexing_enabled,
             out->properties.limits.maxPerStageDescriptorSampledImages, out->properties.limits.maxPerStageDescriptorSamplers,
             out->properties.limits.maxDescriptorSetSampledImages, out->properties.limits.maxDescriptorSetSamplers);
    if (out->timeline_semaphore_enabled) {
        out->timeline_get_counter = (PFN_vkGetSemaphoreCounterValueKHR)vkGetDeviceProcAddr(out->device, "vkGetSemaphoreCounterValueKHR");
        out->timeline_wait = (PFN_vkWaitSemaphoresKHR)vkGetDeviceProcAddr(out->device, "vkWaitSemaphoresKHR");
    }
    S2A_DLOG("[s2a] vk timeline semaphore: ext=%d feat=%d enabled=%d fns=%d\n",
             has_timeline, timeline.timelineSemaphore == VK_TRUE, out->timeline_semaphore_enabled,
             out->timeline_get_counter != NULL && out->timeline_wait != NULL);
    if (indirect_count_ext) {
        out->draw_indirect_count = (PFN_vkCmdDrawIndirectCountKHR)vkGetDeviceProcAddr(out->device, "vkCmdDrawIndirectCountKHR");
        out->draw_indexed_indirect_count = (PFN_vkCmdDrawIndexedIndirectCountKHR)vkGetDeviceProcAddr(out->device, "vkCmdDrawIndexedIndirectCountKHR");
        out->draw_indirect_count_enabled = out->draw_indirect_count && out->draw_indexed_indirect_count;
    }
    if (push_desc_ext) {
        out->cmd_push_descriptor_set = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(out->device, "vkCmdPushDescriptorSetKHR");
        out->push_descriptor_enabled = out->cmd_push_descriptor_set && out->max_push_descriptors > 0;
    }
    S2A_DLOG("[s2a] vk push descriptor: ext=%d enabled=%d max=%u\n", push_desc_ext, out->push_descriptor_enabled, out->max_push_descriptors);
    if (s2a_vk_effective_api(out->properties.apiVersion) >= VK_API_VERSION_1_1) {
        out->create_desc_update_template = (PFN_vkCreateDescriptorUpdateTemplate)vkGetDeviceProcAddr(out->device, "vkCreateDescriptorUpdateTemplate");
        out->destroy_desc_update_template = (PFN_vkDestroyDescriptorUpdateTemplate)vkGetDeviceProcAddr(out->device, "vkDestroyDescriptorUpdateTemplate");
        out->update_desc_set_with_template = (PFN_vkUpdateDescriptorSetWithTemplate)vkGetDeviceProcAddr(out->device, "vkUpdateDescriptorSetWithTemplate");
        out->desc_update_template_enabled = out->create_desc_update_template &&
            out->destroy_desc_update_template && out->update_desc_set_with_template;
    }
    S2A_DLOG("[s2a] vk desc update template: enabled=%d\n", out->desc_update_template_enabled);
    if (out->line_raster_enabled) {
        out->cmd_set_line_stipple = (PFN_vkCmdSetLineStippleEXT)vkGetDeviceProcAddr(out->device, "vkCmdSetLineStippleEXT");
        if (!out->cmd_set_line_stipple) {
            out->line_raster_stippled_rectangular = 0;
            out->line_raster_stippled_bresenham = 0;
            out->line_raster_stippled_smooth = 0;
        }
    }
    S2A_DLOG("[s2a] vk line_raster: enabled=%d rect=%d bres=%d smooth=%d stipple=(%d,%d,%d) subpixel=%u fn=%d\n",
             out->line_raster_enabled, out->line_raster_rectangular, out->line_raster_bresenham, out->line_raster_smooth,
             out->line_raster_stippled_rectangular, out->line_raster_stippled_bresenham, out->line_raster_stippled_smooth,
             out->line_subpixel_bits, out->cmd_set_line_stipple != NULL);
    if (enable_maint4) {
        out->get_device_buffer_memory_requirements = (PFN_vkGetDeviceBufferMemoryRequirementsKHR)
            vkGetDeviceProcAddr(out->device, "vkGetDeviceBufferMemoryRequirementsKHR");
        out->get_device_image_memory_requirements = (PFN_vkGetDeviceImageMemoryRequirementsKHR)
            vkGetDeviceProcAddr(out->device, "vkGetDeviceImageMemoryRequirementsKHR");
        out->maintenance4_enabled = out->get_device_buffer_memory_requirements &&
                                    out->get_device_image_memory_requirements;
        if (!out->maintenance4_enabled) {
            out->max_buffer_size = 0;
            out->get_device_buffer_memory_requirements = NULL;
            out->get_device_image_memory_requirements = NULL;
        }
    }
    S2A_DLOG("[s2a] vk maintenance4: ext=%d want=%d feat=%d enabled=%d max_buf=%llu\n",
             has_maint4, want_maint4, maint4.maintenance4 == VK_TRUE, out->maintenance4_enabled,
             (unsigned long long)out->max_buffer_size);
    vkGetDeviceQueue(out->device, out->queue_family, 0, &out->queue);
    if (enabled_features.sparseBinding) vkGetDeviceQueue(out->device, sparse_family, 0, &out->sparse_queue);
    VkCommandPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = out->queue_family;
    if (vkCreateCommandPool(out->device, &pool, NULL, &out->command_pool) != VK_SUCCESS)
        return fail(why, why_size, "vkCreateCommandPool failed");
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
#if defined(__ANDROID__)
    if (device->instance && g_val_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT rm = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(device->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (rm) rm(device->instance, g_val_messenger, NULL);
        g_val_messenger = VK_NULL_HANDLE;
    }
#endif
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

VkResult s2a_vk_buffer_memory_requirements(const struct s2a_vk_device *device,
                                           const VkBufferCreateInfo *info,
                                           VkMemoryRequirements *out) {
    if (!device || !device->device || !info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out, 0, sizeof *out);
    if (device->maintenance4_enabled && device->get_device_buffer_memory_requirements) {
        VkDeviceBufferMemoryRequirementsKHR req = {0};
        req.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS_KHR;
        req.pCreateInfo = info;
        VkMemoryRequirements2 mr2 = {0};
        mr2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        device->get_device_buffer_memory_requirements(device->device, &req, &mr2);
        *out = mr2.memoryRequirements;
        return VK_SUCCESS;
    }
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(device->device, info, NULL, &buf);
    if (r != VK_SUCCESS) return r;
    vkGetBufferMemoryRequirements(device->device, buf, out);
    vkDestroyBuffer(device->device, buf, NULL);
    return VK_SUCCESS;
}

VkResult s2a_vk_image_memory_requirements(const struct s2a_vk_device *device,
                                          const VkImageCreateInfo *info,
                                          VkMemoryRequirements *out) {
    if (!device || !device->device || !info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out, 0, sizeof *out);
    if (device->maintenance4_enabled && device->get_device_image_memory_requirements) {
        VkDeviceImageMemoryRequirementsKHR req = {0};
        req.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS_KHR;
        req.pCreateInfo = info;
        VkMemoryRequirements2 mr2 = {0};
        mr2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        device->get_device_image_memory_requirements(device->device, &req, &mr2);
        *out = mr2.memoryRequirements;
        return VK_SUCCESS;
    }
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(device->device, info, NULL, &img);
    if (r != VK_SUCCESS) return r;
    vkGetImageMemoryRequirements(device->device, img, out);
    vkDestroyImage(device->device, img, NULL);
    return VK_SUCCESS;
}

VkResult s2a_vk_wait_fences(VkDevice device, uint32_t n, const VkFence *fences, uint64_t timeout_ns) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t el = 0;
    for (;;) {
        VkResult r = vkWaitForFences(device, n, fences, VK_TRUE, timeout_ns - el);
        if (r != VK_TIMEOUT) return r;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        el = (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)t.tv_nsec - (uint64_t)t0.tv_nsec;
        if (el >= timeout_ns) return VK_TIMEOUT;                    // 墙钟真到时限
        struct timespec nap = {0, 200000}; nanosleep(&nap, NULL);   // 提前返回：让出 0.2 毫秒再等，不空转
    }
}
