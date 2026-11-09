#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <android/log.h>
#include <string.h>
#include <sys/system_properties.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "nvn_vk_swapchain.h"
#include "nvn_vk_surface.h"
#include "nvn_vk_pipeline.h"
#include "nvn_vk_first_draw.h"
#include "nvn_vk_lifecycle.h"

static PFN_vkCreateAndroidSurfaceKHR pCreateSurface;
static PFN_vkGetPhysicalDeviceSurfaceSupportKHR pSupport;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pCaps;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR pFormats;
static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pModes;
static PFN_vkCreateSwapchainKHR pCreateSwapchain;
static PFN_vkDestroySwapchainKHR pDestroySwapchain;
static PFN_vkGetSwapchainImagesKHR pGetImages;
static PFN_vkAcquireNextImageKHR pAcquire;
static PFN_vkQueuePresentKHR pPresent;

static int load(struct nvn_vk_swapchain *sc) {
    struct s2a_vk_device *d = sc->d;
    pCreateSurface = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(d->instance, "vkCreateAndroidSurfaceKHR");
    pSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkGetInstanceProcAddr(d->instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    pCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkGetInstanceProcAddr(d->instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    pFormats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkGetInstanceProcAddr(d->instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    pModes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(d->instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    pCreateSwapchain = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(d->device, "vkCreateSwapchainKHR");
    pDestroySwapchain = (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(d->device, "vkDestroySwapchainKHR");
    pGetImages = (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(d->device, "vkGetSwapchainImagesKHR");
    pAcquire = (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(d->device, "vkAcquireNextImageKHR");
    pPresent = (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(d->device, "vkQueuePresentKHR");
    return pCreateSurface && pSupport && pCaps && pFormats && pModes && pCreateSwapchain && pDestroySwapchain && pGetImages && pAcquire && pPresent;
}

static int64_t fd_now_ns_sc(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static int fail(char *why, size_t n, const char *text) { if (why && n) snprintf(why, n, "%s", text); return -1; }

// present 记账：同一时刻只有一个 swapchain（单窗口 owner），这里保留最近一次的快照供诊断/验收读取。
static struct nvn_vk_swapchain *g_present_sc;
void nvn_vk_swapchain_present_stats(struct nvn_vk_swapchain_present_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!g_present_sc) return;
    out->submitted = g_present_sc->submitted;
    out->completed = g_present_sc->completed;
    out->retired = g_present_sc->retired;
    out->submit_failed = g_present_sc->submit_failed;
    out->present_failed = g_present_sc->present_failed_count;
    out->fence_timeout = g_present_sc->fence_timeout;
    out->retire_timeout = g_present_sc->retire_timeout;
    out->acquire_timeout = g_present_sc->acquire_timeout;
    out->retained = g_present_sc->nretained;
    out->pending = g_present_sc->npresent_pending;
    out->present_failed_flag = g_present_sc->present_failed;
}

// W6.0/W6.1：低频呈现账。四态分开报：present 已提交、渲染 fence 完成（同步路径）、
// present fence 退休、仍在途/保留；失败计数不折叠。GPU 时间取最近一次已完成的帧，
// 没取到就写 unavailable，不写 0 ms。每 64 次成功 present 一行，采集器离线读。
static void present_account_tick(struct nvn_vk_swapchain *sc) {
    static uint64_t n;
    if (!sc || ++n % 64 != 0) return;
    double gpu_ms = 0.0;
    int passes = 0;
    uint64_t gpu_frame = 0;
    int gpu_ok = nvn_vk_first_draw_gpu_last(&gpu_ms, &passes, &gpu_frame);
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
        "present account: submitted=%llu draw_completed_sync=%llu presentation_retired=%llu "
        "pending=%d retained=%d deferred_retired=%llu | "
        "fail(submit=%llu present=%llu fence_timeout=%llu retire_timeout=%llu acquire_timeout=%llu) | "
        "gpu=%s",
        (unsigned long long)sc->submitted, (unsigned long long)sc->completed,
        (unsigned long long)sc->retired, sc->npresent_pending, sc->nretained,
        (unsigned long long)nvn_vk_first_draw_deferred_retired(),
        (unsigned long long)sc->submit_failed, (unsigned long long)sc->present_failed_count,
        (unsigned long long)sc->fence_timeout, (unsigned long long)sc->retire_timeout,
        (unsigned long long)sc->acquire_timeout,
        gpu_ok ? "valid" : "unavailable");
    if (gpu_ok)
        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
            "present account gpu: last=%.2f ms frame=%llu passes=%d (read after completion)",
            gpu_ms, (unsigned long long)gpu_frame, passes);
}

// 诊断注入（与 debug.s2a.fail_submit 同一类，只用于验收失败/退休路径）：
// 1 = 提交成功后假装 present 报错；2 = present 成功后假装等不到 fence。0 = 正常语义。
static int present_fault_mode(void) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.fail_present", v);
    return v[0] ? atoi(v) : 0;
}

// 已提交但没有完成/呈现证据：命令缓冲和 fence 保留（Vulkan 禁止释放/复用），present_done 不调用。
static void present_retain(struct nvn_vk_swapchain *sc, VkFence fence, VkFence present_fence, VkCommandBuffer cmd) {
    if (sc->nretained >= NVK_SC_RETAINED) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "present: retained-command table full (%d); in-flight present command kept but not tracked",
                            NVK_SC_RETAINED);
        return;
    }
    sc->retained[sc->nretained].fence = fence;
    sc->retained[sc->nretained].present_fence = present_fence;
    sc->retained[sc->nretained].cmd = cmd;
    sc->nretained++;
}

// 呈现退休：只对已经交给呈现引擎的 present fence 做观察。budget_ns==0 是纯轮询（不等待）；
// >0 是有界等待；<0 无限等（只给进程收尾的显式路径用，不作为生产帧路径的默认）。
// 返回 0 = 队列已清空；1 = 仍有未 signal 的 present fence（不是失败）；-1 = fence 状态不可用。
int nvn_vk_swapchain_retire_present(struct nvn_vk_swapchain *sc, int64_t budget_ns) {
    if (!sc || !sc->d || sc->npresent_pending <= 0) return 0;
    const int64_t deadline = budget_ns < 0 ? -1 : fd_now_ns_sc() + budget_ns;
    int i = 0;
    while (i < sc->npresent_pending) {
        VkFence fence = sc->present_pending[i];
        VkResult wr;
        if (fence == VK_NULL_HANDLE) {
            // 诊断注入（debug.s2a.fail_present=2）或创建失败后的占位：没有可观察的退休证明，
            // 不伪造 signal，也不释放。按“仍在途”保留。
            i++;
            continue;
        }
        if (budget_ns == 0) {
            wr = vkGetFenceStatus(sc->d->device, fence);
        } else {
            int64_t left = deadline < 0 ? (int64_t)UINT64_MAX : deadline - fd_now_ns_sc();
            if (left <= 0 && budget_ns > 0) wr = VK_TIMEOUT;
            else wr = vkWaitForFences(sc->d->device, 1, &fence, VK_TRUE, (uint64_t)left);
        }
        if (wr == VK_SUCCESS) {
            vkDestroyFence(sc->d->device, fence, NULL);
            for (int k = i + 1; k < sc->npresent_pending; k++)
                sc->present_pending[k - 1] = sc->present_pending[k];
            sc->npresent_pending--;
            sc->retired++;
            continue;
        }
        if (wr == VK_TIMEOUT || wr == VK_NOT_READY) { i++; continue; }
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "present retire: fence status=%d; presentation retirement proof unavailable "
                            "(%d present(s) kept unretired)", (int)wr, sc->npresent_pending);
        return -1;
    }
    return sc->npresent_pending ? 1 : 0;
}

// 失败终态：present 路径不能再用这份 swapchain（信号量状态已不保证），但对象不被销毁。
static int present_fail_state(struct nvn_vk_swapchain *sc, char *why, size_t why_size, const char *text) {
    if (!sc->present_failed) {
        sc->present_failed = 1;
        snprintf(sc->present_fail, sizeof sc->present_fail, "%s", text ? text : "present path failed");
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "present: %s; swapchain present path retired (window retirement required, "
                            "%d in-flight command set(s) retained)", text ? text : "failed", sc->nretained);
    }
    return fail(why, why_size, text);
}

int nvn_vk_swapchain_create(struct nvn_vk_swapchain *sc, struct s2a_vk_device *d, ANativeWindow *window, char *why, size_t why_size) {
    memset(sc, 0, sizeof *sc);
    sc->d = d;
    if (!d || !window) return fail(why, why_size, "null device/window");
    sc->window = window;
    // B3 输出尺寸出图：开关在建链时读一次；会话中途改开关只影响下一次窗口 attach。
    char bg[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.buffer_geometry", bg);
    sc->buf_geometry = bg[0] == '1';
    // 首版直连合同：present 必须有真实呈现退休证据（VK_EXT_swapchain_maintenance1 的 present fence）。
    // 只有渲染 fence 能证明“画完”，不能证明呈现引擎已释放图像；缺项在 prepare 阶段就拒绝 direct
    // （AUTO 在 commit 前退回 generic），不能拿渲染完成冒充呈现退休。
    if (!d->swapchain_maintenance1_enabled)
        return fail(why, why_size,
                    "presentation retirement proof unavailable: VK_EXT_swapchain_maintenance1/swapchainMaintenance1 is not enabled");
    if (!load(sc)) return fail(why, why_size, "WSI entry points missing");
    VkAndroidSurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    si.window = window;
    if (pCreateSurface(d->instance, &si, NULL, &sc->surface) != VK_SUCCESS) return fail(why, why_size, "vkCreateAndroidSurfaceKHR failed");
    VkBool32 supported = VK_FALSE;
    pSupport(d->physical, d->queue_family, sc->surface, &supported);
    if (!supported) return fail(why, why_size, "queue family cannot present this surface");
    VkSurfaceCapabilitiesKHR caps;
    pCaps(d->physical, sc->surface, &caps);
    uint32_t fc = 0, mc = 0;
    pFormats(d->physical, sc->surface, &fc, NULL);
    VkSurfaceFormatKHR fmts[32]; if (fc > 32) fc = 32;
    pFormats(d->physical, sc->surface, &fc, fmts);
    pModes(d->physical, sc->surface, &mc, NULL);
    VkPresentModeKHR modes[16]; if (mc > 16) mc = 16;
    pModes(d->physical, sc->surface, &mc, modes);
    VkSurfaceFormatKHR chosen = nvn_vk_pick_surface_format(fmts, fc, VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    if (chosen.format == VK_FORMAT_UNDEFINED) return fail(why, why_size, "surface has no usable format");
    if (chosen.format == VK_FORMAT_R8G8B8A8_SRGB || chosen.format == VK_FORMAT_B8G8R8A8_SRGB)
        __android_log_print(ANDROID_LOG_WARN, "S2Arm",
            "swapchain: surface offers no UNORM format; SRGB present may double-encode the UNORM game render target");

    VkPresentModeKHR pm = nvn_vk_pick_present_mode(modes, mc, VK_PRESENT_MODE_FIFO_KHR);
    uint32_t ww = (uint32_t)ANativeWindow_getWidth(window), wh = (uint32_t)ANativeWindow_getHeight(window);
    // 诊断：源图/窗口/swapchain 各自的坐标系与尺寸。
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
        "swapchain caps: window=%ux%u currentExtent=%ux%u min=%ux%u max=%ux%u currentTransform=0x%x supportedTransforms=0x%x",
        ww, wh, caps.currentExtent.width, caps.currentExtent.height,
        caps.minImageExtent.width, caps.minImageExtent.height,
        caps.maxImageExtent.width, caps.maxImageExtent.height,
        (unsigned)caps.currentTransform, (unsigned)caps.supportedTransforms);
    // Android 契约（graphics/vulkan/wsi/AndroidWindow.cs）：surface 支持 Identity 时把旋转交给
    // Android 合成器，swapchain 用发布窗口坐标；不能把 currentTransform 当 preTransform 又不转内容，
    // 那会让合成器再转一次（实测 2712x1220 窗口、ROTATE_90、源图横排 → 屏幕竖排）。
    if (!(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        return fail(why, why_size, "surface does not support identity preTransform; pre-rotation path not implemented");
    if (!ww || !wh) {
        if (caps.currentExtent.width == UINT32_MAX || !caps.currentExtent.width || !caps.currentExtent.height)
            return fail(why, why_size, "window and surface extent are both unavailable");
        ww = caps.currentExtent.width; wh = caps.currentExtent.height;
    }
    uint32_t w = ww, h = wh;
    if (w < caps.minImageExtent.width) w = caps.minImageExtent.width;
    if (w > caps.maxImageExtent.width) w = caps.maxImageExtent.width;
    if (h < caps.minImageExtent.height) h = caps.minImageExtent.height;
    if (h > caps.maxImageExtent.height) h = caps.maxImageExtent.height;
    if (!w || !h) return fail(why, why_size, "clamped swapchain extent is zero");
    VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = sc->surface; ci.minImageCount = caps.minImageCount + 1;
    ci.imageFormat = chosen.format; ci.imageColorSpace = chosen.colorSpace; ci.imageExtent = (VkExtent2D){w, h};
    ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
        "swapchain create: extent=%ux%u preTransform=0x%x fmt=%d colorspace=%d presentMode=%d presented-retirement=present-fence(EXT) visible-proof=none",
        w, h, (unsigned)ci.preTransform, (int)chosen.format, (int)chosen.colorSpace, (int)pm);
    ci.compositeAlpha = alpha; ci.presentMode = pm; ci.clipped = VK_TRUE;
    if (pCreateSwapchain(d->device, &ci, NULL, &sc->swapchain) != VK_SUCCESS) return fail(why, why_size, "vkCreateSwapchainKHR failed");
    pGetImages(d->device, sc->swapchain, &sc->image_count, NULL);
    if (sc->image_count > 8) sc->image_count = 8;
    pGetImages(d->device, sc->swapchain, &sc->image_count, sc->images);
    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateSemaphore(d->device, &sem, NULL, &sc->acquire) != VK_SUCCESS)
        return fail(why, why_size, "acquire semaphore create failed");
    for (uint32_t i = 0; i < sc->image_count; i++)
        if (vkCreateSemaphore(d->device, &sem, NULL, &sc->release[i]) != VK_SUCCESS)
            return fail(why, why_size, "per-image present semaphore create failed");
    if (vkCreateFence(d->device, &fci, NULL, &sc->fence) != VK_SUCCESS)
        return fail(why, why_size, "draw-completion fence create failed");
    sc->format = chosen.format; sc->extent = (VkExtent2D){w, h};
    sc->window_width = ww ? ww : w;
    sc->window_height = wh ? wh : h;
    // B3：此时缓冲还没被改成游戏输出尺寸，getWidth/getHeight 仍是 view 的像素尺寸；
    // 之后 buffer_geometry 重建会让这两个 API 报告缓冲尺寸，窗口像素只能用这里记下的值。
    sc->frame_width = sc->window_width;
    sc->frame_height = sc->window_height;
    sc->ready = 1;
    return 0;
}

int nvn_vk_swapchain_clear_present(struct nvn_vk_swapchain *sc, float r, float g, float b, float a, char *why, size_t why_size) {
    if (sc && sc->present_failed)
        return fail(why, why_size, "clear present: swapchain present path already failed (window retirement required)");
    if (!sc || !sc->ready) return fail(why, why_size, "swapchain not ready");
    if (sc->acquire_pending)
        return fail(why, why_size, "clear present: previous acquire has no consuming submit; swapchain unusable");
    g_present_sc = sc;
    uint32_t index = 0;
    VkResult ar = pAcquire(sc->d->device, sc->swapchain, (uint64_t)NVK_PRESENT_ACQUIRE_TIMEOUT_NS,
                           sc->acquire, VK_NULL_HANDLE, &index);
    if (ar == VK_TIMEOUT || ar == VK_NOT_READY)
        return fail(why, why_size, "clear present: acquire timed out; frame skipped (no infinite wait)");
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return fail(why, why_size, "vkAcquireNextImageKHR failed");
    if (index >= sc->image_count) return fail(why, why_size, "swapchain index out of range");
    sc->acquire_pending = 1;   // 提交成功前不能销毁/复用 acquire 信号量
    vkResetFences(sc->d->device, 1, &sc->fence);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = sc->d->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(sc->d->device, &cai, &cmd) != VK_SUCCESS)
        return present_fail_state(sc, why, why_size, "command buffer alloc failed after acquire; acquire state kept");
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = sc->images[index];
    to_dst.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
    VkClearColorValue clear = { { r, g, b, a } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, sc->images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    VkImageMemoryBarrier to_present = to_dst;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &sc->acquire; submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &sc->release[index];
    // 探针路径也必须区分“画完”与“呈现退休”：同步等 present fence（探针不追帧节拍）。
    VkFenceCreateInfo pfci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence present_fence = VK_NULL_HANDLE;
    if (vkCreateFence(sc->d->device, &pfci, NULL, &present_fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
        return present_fail_state(sc, why, why_size,
                                  "present-retirement fence create failed after acquire; acquire state kept");
    }
    VkSwapchainPresentFenceInfoEXT fence_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT };
    fence_info.swapchainCount = 1; fence_info.pFences = &present_fence;
    nvn_vk_callring_add("present submit cmd=%p fence=%p", (void *)cmd, (void *)sc->fence);
    if (vkQueueSubmit(sc->d->queue, 1, &submit, sc->fence) != VK_SUCCESS) {
        vkDestroyFence(sc->d->device, present_fence, NULL);
        return present_fail_state(sc, why, why_size,
                                  "vkQueueSubmit failed; acquired image/command kept, acquire state unproven");
    }
    sc->acquire_pending = 0;   // 提交已 wait acquire 信号量
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.pNext = &fence_info;
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sc->release[index];
    pi.swapchainCount = 1; pi.pSwapchains = &sc->swapchain; pi.pImageIndices = &index;
    VkResult pr = pPresent(sc->d->queue, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        present_retain(sc, sc->fence, present_fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        return present_fail_state(sc, why, why_size, "vkQueuePresentKHR failed after submit; fences/command retained");
    }
    int drew_ok = vkWaitForFences(sc->d->device, 1, &sc->fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
    int retired_ok = drew_ok &&
        vkWaitForFences(sc->d->device, 1, &present_fence, VK_TRUE, (uint64_t)NVK_PRESENT_RETIRE_BUDGET_NS) == VK_SUCCESS;
    if (!retired_ok) {
        present_retain(sc, sc->fence, present_fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        if (!drew_ok) sc->fence_timeout++; else sc->retire_timeout++;
        return present_fail_state(sc, why, why_size, "clear-present retirement not proven; command/fences retained");
    }
    vkDestroyFence(sc->d->device, present_fence, NULL);
    vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
    sc->completed++;
    sc->retired++;
    return 0;
}

int nvn_vk_swapchain_draw_present(struct nvn_vk_swapchain *sc, struct nvn_vk_pipeline_cache *pc, VkPipeline pipe,
                                  const void *verts, size_t vert_bytes, VkBuffer ubo, VkDeviceSize ubo_size,
                                  char *why, size_t why_size) {
    if (sc && sc->present_failed)
        return fail(why, why_size, "draw present: swapchain present path already failed (window retirement required)");
    if (!sc || !sc->ready || !pc || !pipe || !verts || vert_bytes < 16) return fail(why, why_size, "draw present: bad args");
    if (sc->acquire_pending)
        return fail(why, why_size, "draw present: previous acquire has no consuming submit; swapchain unusable");
    g_present_sc = sc;
    uint32_t index = 0;
    VkResult ar = pAcquire(sc->d->device, sc->swapchain, (uint64_t)NVK_PRESENT_ACQUIRE_TIMEOUT_NS,
                           sc->acquire, VK_NULL_HANDLE, &index);
    if (ar == VK_TIMEOUT || ar == VK_NOT_READY)
        return fail(why, why_size, "draw present: acquire timed out; frame skipped (no infinite wait)");
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return fail(why, why_size, "vkAcquireNextImageKHR failed");
    if (index >= sc->image_count) return fail(why, why_size, "swapchain index out of range");
    sc->acquire_pending = 1;   // 提交成功前不能销毁/复用 acquire 信号量
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = sc->images[index]; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = sc->format;
    vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(sc->d->device, &vci, NULL, &view) != VK_SUCCESS)
        return present_fail_state(sc, why, why_size, "swapchain view create failed after acquire; acquire state kept");
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = sc->d->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(sc->d->device, &cai, &cmd) != VK_SUCCESS) {
        vkDestroyImageView(sc->d->device, view, NULL);
        return present_fail_state(sc, why, why_size, "command buffer alloc failed after acquire; acquire state kept");
    }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier to_color = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = sc->images[index];
    to_color.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &to_color);
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = vert_bytes; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vb = VK_NULL_HANDLE; vkCreateBuffer(sc->d->device, &bci, NULL, &vb);
    VkMemoryRequirements vmr; vkGetBufferMemoryRequirements(sc->d->device, vb, &vmr);
    VkMemoryAllocateInfo vai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    vai.allocationSize = vmr.size;
    vai.memoryTypeIndex = s2a_vk_memory_type(sc->d, vmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkDeviceMemory vmem = VK_NULL_HANDLE; vkAllocateMemory(sc->d->device, &vai, NULL, &vmem); vkBindBufferMemory(sc->d->device, vb, vmem, 0);
    void *vp = NULL; if (vkMapMemory(sc->d->device, vmem, 0, vert_bytes, 0, &vp) == VK_SUCCESS) { memcpy(vp, verts, vert_bytes); vkUnmapMemory(sc->d->device, vmem); }
    VkFramebuffer fb = VK_NULL_HANDLE;
    int rc = nvn_vk_pipeline_record(pc, pipe, cmd, view, sc->extent.width, sc->extent.height, vb, ubo, ubo_size, &fb);
    VkImageMemoryBarrier to_present = to_color;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &sc->acquire; si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sc->release[index];
    vkResetFences(sc->d->device, 1, &sc->fence);
    VkFenceCreateInfo pfci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence present_fence = VK_NULL_HANDLE;
    if (vkCreateFence(sc->d->device, &pfci, NULL, &present_fence) != VK_SUCCESS) {
        if (fb) vkDestroyFramebuffer(sc->d->device, fb, NULL);
        vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
        vkDestroyImageView(sc->d->device, view, NULL);
        vkDestroyBuffer(sc->d->device, vb, NULL); vkFreeMemory(sc->d->device, vmem, NULL);
        return present_fail_state(sc, why, why_size,
                                  "retirement fence create failed after acquire; acquire state kept");
    }
    if (rc != 0 || vkQueueSubmit(sc->d->queue, 1, &si, sc->fence) != VK_SUCCESS) {
        // 命令从未提交：命令缓冲与它的 framebuffer/view/buffer 全部可回收。
        vkDestroyFence(sc->d->device, present_fence, NULL);
        if (fb) vkDestroyFramebuffer(sc->d->device, fb, NULL);
        vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
        vkDestroyImageView(sc->d->device, view, NULL);
        vkDestroyBuffer(sc->d->device, vb, NULL); vkFreeMemory(sc->d->device, vmem, NULL);
        return present_fail_state(sc, why, why_size,
                                  "draw present: submit failed; acquired image/command state kept");
    }
    sc->acquire_pending = 0;
    VkSwapchainPresentFenceInfoEXT fence_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT };
    fence_info.swapchainCount = 1; fence_info.pFences = &present_fence;
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.pNext = &fence_info;
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sc->release[index];
    pi.swapchainCount = 1; pi.pSwapchains = &sc->swapchain; pi.pImageIndices = &index;
    VkResult pr = pPresent(sc->d->queue, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        present_retain(sc, sc->fence, present_fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        // retained command 仍引用 fb/view/vb/vmem：不销毁它们（随设备/进程收尾）。
        sc->present_failed_count++;
        return present_fail_state(sc, why, why_size,
                                  "draw present: present failed after submit; command/fences retained");
    }
    int drew_ok = vkWaitForFences(sc->d->device, 1, &sc->fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
    int retired_ok = drew_ok &&
        vkWaitForFences(sc->d->device, 1, &present_fence, VK_TRUE, (uint64_t)NVK_PRESENT_RETIRE_BUDGET_NS) == VK_SUCCESS;
    if (!retired_ok) {
        present_retain(sc, sc->fence, present_fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        if (!drew_ok) sc->fence_timeout++; else sc->retire_timeout++;
        // 同 present 失败：命令引用的 fb/view/vb/vmem 保留，不在这里销毁。
        return present_fail_state(sc, why, why_size,
                                  "draw present: completion/retirement not proven; command/fences retained");
    }
    vkDestroyFence(sc->d->device, present_fence, NULL);
    if (fb) vkDestroyFramebuffer(sc->d->device, fb, NULL);
    vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
    vkDestroyImageView(sc->d->device, view, NULL);
    vkDestroyBuffer(sc->d->device, vb, NULL); vkFreeMemory(sc->d->device, vmem, NULL);
    sc->completed++;
    sc->retired++;
    return 0;
}

// 把一张已经上传完成的纹理（SHADER_READ_ONLY_OPTIMAL 布局）缩放 blit 到 swapchain 并 present。
// 源格式/尺寸由调用方给出（texcache 的 image 只带 NVN 格式，VkFormat 由调用方再算一次）。

// 一次性诊断：把上屏源图（真实游戏 RT）整幅读回，落成 raw + 采样日志。
// 只在 debug.s2a.dump_frame=1 时执行一次；不改绘制/呈现语义。
static void dump_present_src(struct nvn_vk_swapchain *sc, VkImage src, uint32_t w, uint32_t h,
                             VkFormat format, const char *dir, int continuous) {
    static int done;
    // 1 = 首次呈现抓一张；2 = 每次呈现都覆盖同一文件（用于和屏幕截图同一时刻对照）。
    if (!dir || !dir[0] || dir[0] != '/') return;
    if (done && !continuous) return;
    done = 1;
    uint32_t bpp = 0;
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_UNORM: bpp = 4; break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: bpp = 2; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: bpp = 16; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: bpp = 8; break;
        case VK_FORMAT_R8_UNORM: bpp = 1; break;
        default: break;
    }
    if (!bpp) { __android_log_print(ANDROID_LOG_INFO, "S2Arm", "frame dump: unsupported present format %d", format); return; }
    VkDeviceSize bytes = (VkDeviceSize)w * h * bpp;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkCreateBuffer(sc->d->device, &bi, NULL, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(sc->d->device, buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = s2a_vk_memory_type(sc->d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (vkAllocateMemory(sc->d->device, &mai, NULL, &mem) != VK_SUCCESS) { vkDestroyBuffer(sc->d->device, buf, NULL); return; }
    vkBindBufferMemory(sc->d->device, buf, mem, 0);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = sc->d->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(sc->d->device, &cai, &cmd) != VK_SUCCESS) goto cleanup;
    {
        VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbi);
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = src; b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy c = {0};
        c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageExtent = (VkExtent3D){ w, h, 1 };
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cmd);
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(sc->d->device, &fci, NULL, &fence);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(sc->d->queue, 1, &si, fence) == VK_SUCCESS &&
            vkWaitForFences(sc->d->device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS) {
            void *map = NULL;
            if (vkMapMemory(sc->d->device, mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
                char path[1200];
                snprintf(path, sizeof path, "%s/present-src.raw", dir);
                FILE *f = fopen(path, "wb");
                if (f) { size_t wr = fwrite(map, 1, (size_t)bytes, f); fclose(f);
                         __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                             "frame dump: %s %ux%u bpp=%u fmt=%d bytes=%zu written=%zu", path, w, h, bpp, format, (size_t)bytes, wr); }
                const uint8_t *p = (const uint8_t *)map;
                for (uint32_t gy = 0; gy < 4; gy++) for (uint32_t gx = 0; gx < 4; gx++) {
                    size_t off = ((size_t)(gy * (h - 1) / 3) * w + (gx * (w - 1) / 3)) * bpp;
                    uint32_t v0 = 0, v1 = 0;
                    memcpy(&v0, p + off, 4);
                    if (bpp >= 8) memcpy(&v1, p + off + 4, 4);
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "frame dump sample (%u,%u)=%08x %08x",
                                        gx * (w - 1) / 3, gy * (h - 1) / 3, v0, v1);
                }
                vkUnmapMemory(sc->d->device, mem);
            }
        }
        if (fence) vkDestroyFence(sc->d->device, fence, NULL);
    }
    if (cmd) vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
cleanup:
    vkDestroyBuffer(sc->d->device, buf, NULL);
    vkFreeMemory(sc->d->device, mem, NULL);
}

// present 内部四段计时：哪一段吃掉这 24ms 必须量出来，不能猜。
// 1=等本帧 GPU 画完(flush fence) 2=acquire 3=录clear+blit并提交 4=queuePresent+fence(含 vsync)
static uint64_t sw_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static uint64_t sw_st[4], sw_n;
static void sw_note(int i, uint64_t dt) { if (i >= 0 && i < 4) sw_st[i] += dt; }

// Touch transport is linked by the Android host; older standalone probes may omit it.
extern uint64_t s2a_android_game_touch_epoch(void) __attribute__((weak));
extern void s2a_android_game_touch_viewport_checked(uint64_t, uint64_t, int, int, int, int, int, int) __attribute__((weak));

static void publish_touch(const struct nvn_vk_swapchain *sc, uint64_t epoch,
                          uint32_t crop_w, uint32_t crop_h, int32_t x, int32_t y,
                          uint32_t width, uint32_t height) {
    if (!s2a_android_game_touch_viewport_checked) return;
    // Like the generic compositor, only the standard 16:9 Switch region is known.
    uint64_t a = (uint64_t)crop_w * 9, b = (uint64_t)crop_h * 16;
    uint64_t diff = a > b ? a - b : b - a;
    int standard = diff * 1000 < (uint64_t)crop_h * 9;
    int bw = (int)sc->extent.width, bh = (int)sc->extent.height;
    if (sc->buf_geometry && !sc->geometry_failed && sc->frame_width && sc->frame_height) {
        // B3：缓冲=游戏输出尺寸，整幅被合成器放大到 view frame；Java 把 view 布成同比例
        // 居中矩形，画面区 = 整个 view。触摸矩形与 buffer 尺寸都按 view frame 像素发布
        // （TouchPadView 被布成同一个矩形，两侧像素空间一致）。缓冲尺寸没兑现的降级
        // 交换链走下面缓冲像素路径，与 blit 同一坐标系。
        x = 0; y = 0;
        width = sc->frame_width; height = sc->frame_height;
        bw = (int)sc->frame_width; bh = (int)sc->frame_height;
    }
    s2a_android_game_touch_viewport_checked(epoch, standard ? sc->touch_generation : 0,
        x, y, (int)width, (int)height, bw, bh);
}

// B3：把窗口缓冲改成游戏输出尺寸，SurfaceFlinger/显示硬件把它放大到 view frame。
// 先改缓冲尺寸并核对 getWidth/getHeight 真变了再付销毁+重建的代价：返回 0 但缓冲尺寸没动
// （平台没兑现）时记 geometry_failed 退回普通等比 blit，不能每帧销毁重建。
// 旧交换链按正常退休合同销毁（present fence 未证明就失败），新交换链建在改过的缓冲上，
// extent=输出尺寸，present 里的等比 blit 退化成 1:1 拷贝。
static int geometry_resize(struct nvn_vk_swapchain *sc, uint32_t w, uint32_t h, char *why, size_t why_size) {
    ANativeWindow *window = sc->window;
    struct s2a_vk_device *d = sc->d;
    uint32_t fw = sc->frame_width, fh = sc->frame_height;
    uint64_t tg = sc->touch_generation;
    // format=0 保留现有像素格式；兑现后 ANativeWindow_getWidth/getHeight 报告的是缓冲尺寸。
    if (ANativeWindow_setBuffersGeometry(window, (int32_t)w, (int32_t)h, 0) != 0)
        return fail(why, why_size, "ANativeWindow_setBuffersGeometry failed");
    if (ANativeWindow_getWidth(window) != (int32_t)w || ANativeWindow_getHeight(window) != (int32_t)h) {
        sc->geometry_failed = 1;
        __android_log_print(ANDROID_LOG_WARN, "S2Arm",
            "buffer geometry: setBuffersGeometry accepted but buffer size is %dx%d, not %ux%u — "
            "platform did not honor it; staying on the aspect-fit blit for this swapchain",
            (int)ANativeWindow_getWidth(window), (int)ANativeWindow_getHeight(window), w, h);
        return 0;
    }
    if (nvn_vk_swapchain_destroy(sc))
        return fail(why, why_size, "buffer geometry resize: previous swapchain has no retirement proof");
    if (nvn_vk_swapchain_create(sc, d, window, why, why_size)) return -1;
    // create 重新 memset：窗口像素与 WSI generation 不属于某一条交换链，交还原值。
    sc->frame_width = fw; sc->frame_height = fh;
    sc->touch_generation = tg;
    if (sc->extent.width != w || sc->extent.height != h) {
        // 驱动按 surface min/max 夹取后 extent 不等于缓冲尺寸：缓冲已改但交换链到不了输出尺寸，
        // 同样降级（继续按 extent 等比 blit，不再重试）。
        sc->geometry_failed = 1;
        __android_log_print(ANDROID_LOG_WARN, "S2Arm",
            "buffer geometry: swapchain extent clamped to %ux%u, not %ux%u — "
            "staying on the aspect-fit blit for this swapchain",
            sc->extent.width, sc->extent.height, w, h);
        return 0;
    }
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
        "buffer geometry: buffer=%ux%u (game output) frame=%ux%u — compositor/display upscales",
        sc->extent.width, sc->extent.height, fw, fh);
    return 0;
}

int nvn_vk_swapchain_present_image(struct nvn_vk_swapchain *sc, VkImage src, VkFormat src_format,
                                   uint32_t src_w, uint32_t src_h, const int crop[4], char *why, size_t why_size) {
    const uint64_t sw0 = sw_now_ns();
    uint64_t touch_epoch = s2a_android_game_touch_epoch ? s2a_android_game_touch_epoch() : 0;
    // 先完成本帧挂起的 draw（一条 submit + 一次 fence），present 才能读 RT。
    // 失败/超时时命令缓冲与在途资源已保留，这里不能再呈现一张未完成的图。
    if (nvn_vk_first_draw_flush_defer() != 0)
        return fail(why, why_size, "present image: pending draws did not complete (see frame queue log)");
    uint64_t sw1 = sw_now_ns(); sw_note(0, sw1 - sw0);

    if (!sc || !sc->ready || !src) return fail(why, why_size, "present image: not ready");
    if (!src_w || !src_h || src_format == VK_FORMAT_UNDEFINED) return fail(why, why_size, "present image: bad source");
    if (sc->acquire_pending)
        return fail(why, why_size, "present image: previous acquire has no consuming submit; swapchain unusable");
    if (sc->present_failed)
        return fail(why, why_size, "present image: swapchain present path already failed (window retirement required)");
    // 有效裁剪区 = 游戏真实输出尺寸；crop 不合法时按整幅源图。acquire 之前就要拿到它：
    // buffer_geometry 用它决定要不要按输出尺寸重建交换链。
    uint32_t crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 &&
        (uint32_t)crop[0] + (uint32_t)crop[2] <= src_w && (uint32_t)crop[1] + (uint32_t)crop[3] <= src_h) {
        crop_x = (uint32_t)crop[0]; crop_y = (uint32_t)crop[1];
        crop_w = (uint32_t)crop[2]; crop_h = (uint32_t)crop[3];
    }
    if (sc->buf_geometry && !sc->geometry_failed && sc->window &&
        (sc->extent.width != crop_w || sc->extent.height != crop_h)) {
        int gr = geometry_resize(sc, crop_w, crop_h, why, why_size);
        if (gr) return gr;
    }
    g_present_sc = sc;
    // 先机会式回收上一帧已 signal 的 present fence；仍在途不是错误（FIFO 可能等下一个 vsync）。
    if (sc->npresent_pending && nvn_vk_swapchain_retire_present(sc, 0) < 0)
        return present_fail_state(sc, why, why_size, "present-retirement fence status is unusable");
    uint32_t index = 0;
    // acquire 有界：窗口被撤销/后台时呈现引擎可能不再归还图像，不能拿 UINT64_MAX 把它拖成
    // “surfaceDestroyed 等满 1 s 后 abort”。超时没有取到 image，本帧绘制已完成，按跳过上屏收尾。
    VkResult ar = pAcquire(sc->d->device, sc->swapchain, NVK_PRESENT_ACQUIRE_TIMEOUT_NS,
                           sc->acquire, VK_NULL_HANDLE, &index);
    if (ar == VK_TIMEOUT || ar == VK_NOT_READY) {
        sc->acquire_timeout++;
        if (sc->acquire_timeout % 8 == 1)
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                "present image: acquire timed out (%llu total); frame completed, present skipped",
                                (unsigned long long)sc->acquire_timeout);
        nvn_vk_first_draw_present_done();
        return 1;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR)
        return present_fail_state(sc, why, why_size, "vkAcquireNextImageKHR failed");
    if (index >= sc->image_count)
        return present_fail_state(sc, why, why_size, "swapchain image index out of range");
    sc->acquire_pending = 1;   // 提交成功前不能销毁/复用 acquire 信号量
    uint64_t sw2 = sw_now_ns(); sw_note(1, sw2 - sw1);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = sc->d->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(sc->d->device, &cai, &cmd) != VK_SUCCESS)
        return present_fail_state(sc, why, why_size, "command buffer allocation failed");
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier src_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_barrier.srcQueueFamilyIndex = src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.image = src;
    src_barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    src_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    // TRANSFER_WRITE 只由 TRANSFER 阶段支持；只写 FRAGMENT_SHADER 会触发
    // VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819（8E VVL 实测）。
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &src_barrier);
    // blit 完把源图放回 GENERAL，下一次清屏/拷贝/上屏继续在同一个布局上接力。
    VkImageMemoryBarrier src_back = src_barrier;
    src_back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    src_back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    VkImageMemoryBarrier dst_barrier = src_barrier;
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.image = sc->images[index];
    dst_barrier.srcAccessMask = 0;
    dst_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &dst_barrier);
    {
        // 目标先清黑：等比居中后两侧/上下的黑边是明确的，不是上一帧残留。
        VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, sc->images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    VkImageBlit blit = {0};
    blit.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = (VkOffset3D){(int32_t)crop_x, (int32_t)crop_y, 0};
    blit.srcOffsets[1] = (VkOffset3D){(int32_t)(crop_x + crop_w), (int32_t)(crop_y + crop_h), 1};
    blit.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    // 等比缩放并居中（同 iOS present_encode / Ryujinx Present 的 crop→fit→center）；目标尺寸是
    // Identity 契约下的发布窗口坐标，不能拿旋转前的宽高算比例。
    uint32_t dst_w = sc->extent.width, dst_h = sc->extent.height;
    uint32_t draw_w, draw_h;   // crop_w/h 已由上面的 src_w/src_h 校验保证非零
    if ((uint64_t)crop_w * dst_h >= (uint64_t)crop_h * dst_w) {
        draw_w = dst_w;
        draw_h = (uint32_t)(((uint64_t)crop_h * dst_w + crop_w / 2) / crop_w);
    } else {
        draw_h = dst_h;
        draw_w = (uint32_t)(((uint64_t)crop_w * dst_h + crop_h / 2) / crop_h);
    }
    if (draw_w > dst_w) draw_w = dst_w;
    if (draw_h > dst_h) draw_h = dst_h;
    int32_t dst_x = (int32_t)((dst_w - draw_w) / 2), dst_y = (int32_t)((dst_h - draw_h) / 2);
    blit.dstOffsets[0] = (VkOffset3D){ dst_x, dst_y, 0 };
    blit.dstOffsets[1] = (VkOffset3D){ dst_x + (int32_t)draw_w, dst_y + (int32_t)draw_h, 1 };
    {
        static int logged;
        if (!logged) {
            logged = 1;
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                "present image: src=%ux%u crop=(%d,%d,%d,%d)->(%u,%u,%u,%u) dst=%ux%u fit=%ux%u at=(%d,%d) preTransform=identity src_fmt=%d",
                src_w, src_h, crop ? crop[0] : 0, crop ? crop[1] : 0, crop ? crop[2] : 0, crop ? crop[3] : 0,
                crop_x, crop_y, crop_w, crop_h, dst_w, dst_h, draw_w, draw_h, dst_x, dst_y, (int)src_format);
        }
    }
    nvn_vk_callring_add("blit dst_index=%u dst=%p %ux%u", (unsigned)index, (void *)sc->images[index], (unsigned)sc->extent.width, (unsigned)sc->extent.height);
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->images[index],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &src_back);
    VkImageMemoryBarrier to_present = dst_barrier;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_present);
    nvn_vk_first_draw_gpu_end(cmd);   // 诊断：GPU 帧结束时间戳（present blit 末尾）
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &sc->acquire; submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &sc->release[index];
    if (sc->fence) {
        vkResetFences(sc->d->device, 1, &sc->fence);
    } else {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(sc->d->device, &fci, NULL, &sc->fence) != VK_SUCCESS)
            return present_fail_state(sc, why, why_size, "draw-completion fence creation failed");
    }
    const int fault = present_fault_mode();
    // 呈现退休 fence 必须与渲染 fence 是两个对象：Vulkan 要求 present fence 传入时未 signal、
    // 且没有被其它队列操作使用。用渲染 fence 冒充 present fence 会在提交完成时就 signal，
    // 既不能证明呈现退休，也违反 present fence 的有效性要求。
    VkFence present_fence = VK_NULL_HANDLE;
    if (fault != 2) {
        VkFenceCreateInfo pfci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(sc->d->device, &pfci, NULL, &present_fence) != VK_SUCCESS) {
            // 还没提交：命令缓冲可回收；acquire 已被消费，呈现路径仍按失败终态处理。
            vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
            sc->submit_failed++;
            return present_fail_state(sc, why, why_size,
                                      "present-retirement fence creation failed; command buffer reclaimed");
        }
    }
    if (vkQueueSubmit(sc->d->queue, 1, &submit, sc->fence) != VK_SUCCESS) {
        // 没有提交成功 = 命令从未执行：命令缓冲与 fence 可以回收。但 acquire 已被消费，
        // 这份 swapchain 的 acquire/release 信号量状态不再能保证，呈现路径按失败终态处理。
        if (present_fence) vkDestroyFence(sc->d->device, present_fence, NULL);
        vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
        sc->submit_failed++;
        return present_fail_state(sc, why, why_size,
                                  "vkQueueSubmit failed; command buffer/fences reclaimed, acquire state kept");
    }
    sc->acquire_pending = 0;   // 提交已 wait acquire 信号量
    { uint64_t sw3 = sw_now_ns(); sw_note(2, sw3 - sw2); sw2 = sw3; }
    if (fault == 1) {
        // 负例：真实提交成功，但 present 报错。命令与 fence 仍在队列上，保留且不调用 present_done。
        present_retain(sc, sc->fence, present_fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        sc->present_failed_count++;
        return present_fail_state(sc, why, why_size,
                                  "vkQueuePresentKHR failed after submit; command buffer/fences retained, "
                                  "present_done not called");
    }
    VkSwapchainPresentFenceInfoEXT fence_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT };
    fence_info.swapchainCount = 1;
    fence_info.pFences = &present_fence;
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    if (present_fence) pi.pNext = &fence_info;   // fault=2：故意不带 present fence，验证“退休未证明”保留
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sc->release[index];
    pi.swapchainCount = 1; pi.pSwapchains = &sc->swapchain; pi.pImageIndices = &index;
    VkResult pr = pPresent(sc->d->queue, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        // 提交成功后 present 报错：渲染命令可能仍在执行，命令缓冲/fence 都不能释放/复用。
        present_retain(sc, sc->fence, present_fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        sc->present_failed_count++;
        return present_fail_state(sc, why, why_size,
                                  "vkQueuePresentKHR failed after submit; command buffer/fences retained, "
                                  "present_done not called");
    }
    sc->submitted++;
    {
        // 整幅源图读回放在两条路径汇合处：延迟 present 在下面提前 return，
        // 留在末尾会让 dump_frame 在默认路径下永远够不着（cure3 实测零输出）。
        char frame_dump[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dump_frame", frame_dump);
        char dir[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.dump_fail", dir);
        if ((frame_dump[0] == '1' || frame_dump[0] == '2') && dir[0] == '/')
            dump_present_src(sc, src, src_w, src_h, src_format, dir, frame_dump[0] == '2');
    }
    if (nvn_vk_first_draw_present_deferred()) {
        // 延迟 present：不等渲染 fence。fence 与命令缓冲交给 first_draw 的延迟槽，
        // 下一帧 present 前统一等/释放；本帧的 pin 也留到那时再解。
        nvn_vk_first_draw_slot_blit(sc->fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        if (sc->npresent_pending >= NVK_SC_PRESENT_PENDING) (void)nvn_vk_swapchain_retire_present(sc, 0);
        if (sc->npresent_pending >= NVK_SC_PRESENT_PENDING)
            (void)nvn_vk_swapchain_retire_present(sc, NVK_PRESENT_RETIRE_BUDGET_NS);
        if (sc->npresent_pending >= NVK_SC_PRESENT_PENDING) {
            present_retain(sc, VK_NULL_HANDLE, present_fence, VK_NULL_HANDLE);
            sc->retire_timeout++;
            return present_fail_state(sc, why, why_size,
                                      "present-retirement tracking table is full (deferred present); "
                                      "present fence retained, presentation retirement not claimed");
        }
        sc->present_pending[sc->npresent_pending++] = present_fence;
        (void)nvn_vk_swapchain_retire_present(sc, 0);
        present_account_tick(sc);
        publish_touch(sc, touch_epoch, crop_w, crop_h, dst_x, dst_y, draw_w, draw_h);
        { uint64_t sw3 = sw_now_ns(); sw_note(3, sw3 - sw2);
          if (++sw_n % 32 == 0)
              __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                  "present 拆分(延迟)：等GPU画完=0 us | acquire=%llu us | 录blit+提交=%llu us | present+vsync=%llu us",
                  (unsigned long long)(sw_st[1] / sw_n / 1000), (unsigned long long)(sw_st[2] / sw_n / 1000),
                  (unsigned long long)(sw_st[3] / sw_n / 1000));
          if (sw_n % 32 == 0) { memset(sw_st, 0, sizeof sw_st); sw_n = 0; } }
        return 0;
    }
    // 1) 绘制完成：渲染 fence signal 只证明这批命令执行完。同步调度下等到它才解锁本帧纹理 pin
    //    并回收命令缓冲；这与“呈现退休”是两件事。
    int drew = vkWaitForFences(sc->d->device, 1, &sc->fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
    if (!drew) {
        present_retain(sc, sc->fence, present_fence, cmd);
        sc->fence = VK_NULL_HANDLE;
        sc->fence_timeout++;
        return present_fail_state(sc, why, why_size,
                                  "draw-completion fence timeout; command buffer/fences retained, "
                                  "presentation retirement not claimed");
    }
    sc->completed++;
    nvn_vk_first_draw_gpu_read(sc->d, "present");
    nvn_vk_first_draw_present_done();
    vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &cmd);
    { uint64_t sw3 = sw_now_ns(); sw_note(3, sw3 - sw2);
      if (++sw_n % 32 == 0)
          __android_log_print(ANDROID_LOG_INFO, "S2Arm",
              "present 拆分：等GPU画完=%llu us | acquire=%llu us | 录blit+提交=%llu us | present+vsync=%llu us",
              (unsigned long long)(sw_st[0] / sw_n / 1000), (unsigned long long)(sw_st[1] / sw_n / 1000),
              (unsigned long long)(sw_st[2] / sw_n / 1000), (unsigned long long)(sw_st[3] / sw_n / 1000));
      if (sw_n % 32 == 0) { memset(sw_st, 0, sizeof sw_st); sw_n = 0; } }
    // 2) 呈现退休：present fence 进入 pending 队列，由下一帧/撤销/Stop 观察。这里不能同步等：
    //    FIFO 的 present fence 可能在下一个 vsync 才 signal，逐帧同步等会把渲染节拍绑在呈现引擎上。
    if (sc->npresent_pending >= NVK_SC_PRESENT_PENDING) {
        (void)nvn_vk_swapchain_retire_present(sc, 0);   // 先摘掉已 signal 的
    }
    if (sc->npresent_pending >= NVK_SC_PRESENT_PENDING) {
        // 表满：先给最老的 present 一个有界机会退休（合成器/后台切换可能短暂压住）；
        // 仍腾不出位置就不能丢 present fence，进入失败终态并把它放进保留账本。
        (void)nvn_vk_swapchain_retire_present(sc, NVK_PRESENT_RETIRE_BUDGET_NS);
    }
    if (sc->npresent_pending >= NVK_SC_PRESENT_PENDING) {
        present_retain(sc, VK_NULL_HANDLE, present_fence, VK_NULL_HANDLE);
        sc->retire_timeout++;
        return present_fail_state(sc, why, why_size,
                                  "present-retirement tracking table is full and no bounded wait retired a frame; "
                                  "present fence retained, presentation retirement not claimed");
    }
    sc->present_pending[sc->npresent_pending++] = present_fence;
    (void)nvn_vk_swapchain_retire_present(sc, 0);   // 机会式回收已 signal 的旧 present fence
    present_account_tick(sc);
    publish_touch(sc, touch_epoch, crop_w, crop_h, dst_x, dst_y, draw_w, draw_h);
    return 0;
}

int nvn_vk_swapchain_destroy(struct nvn_vk_swapchain *sc) {
    if (!sc || !sc->d) return 0;
    if (!sc->swapchain && !sc->surface && !sc->fence && !sc->npresent_pending && !sc->nretained &&
        !sc->acquire_pending) return 0;
    if (sc->acquire_pending) {
        // acquire 已经成功、但没有任何提交去 wait 那把信号量：销毁信号量/交换链没有完成证明。
        // 保留全部对象与 WSI 租约，按失败终态结束；不伪造 acquire 已完成。
        sc->present_failed = 1;
        snprintf(sc->present_fail, sizeof sc->present_fail,
                 "acquired swapchain image has no submit consuming its acquire semaphore");
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "swapchain destroy: acquired image has no acquire-completion proof; "
                            "swapchain/surface/semaphores kept");
        sc->ready = 0;
        return -1;
    }
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "present lifecycle: submitted=%llu draw_completed=%llu presentation_retired=%llu "
                        "pending=%d retained=%d acquire_timeout=%llu (present fence proves retirement, not scanout)",
                        (unsigned long long)sc->submitted, (unsigned long long)sc->completed,
                        (unsigned long long)sc->retired, sc->npresent_pending, sc->nretained,
                        (unsigned long long)sc->acquire_timeout);
    // 最后一次观察呈现退休：没有后续帧的 Stop/窗口撤销/旋转都从这里收口。预算有界；
    // 拿不到证明就保留 swapchain/surface/present fence，不 ACK 窗口、不伪造“已退休”。
    int64_t retire_deadline = fd_now_ns_sc() + NVK_PRESENT_RETIRE_BUDGET_NS;
    if (sc->npresent_pending) {
        int64_t left = retire_deadline - fd_now_ns_sc();
        int rr = nvn_vk_swapchain_retire_present(sc, left > 0 ? left : 0);
        if (rr != 0) {
            sc->retire_timeout++;
            sc->present_failed = 1;
            snprintf(sc->present_fail, sizeof sc->present_fail,
                     "presentation retirement not proven at window retirement");
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "swapchain destroy: %d presentation(s) have no retirement proof within %lld ms "
                                "(%s); swapchain/surface kept",
                                sc->npresent_pending, (long long)(NVK_PRESENT_RETIRE_BUDGET_NS / 1000000),
                                rr < 0 ? "fence status unusable" : "present fence still unsignaled");
            sc->ready = 0;
            return -1;
        }
    }
    if (sc->nretained) {
        // 窗口退休前再给一次有界完成检查：拿到完成就显式回收命令缓冲/fence，再正常销毁 swapchain。
        int64_t deadline = retire_deadline;   // B7：与 pending 退休共用一个总预算，不各给 500 ms
        int settled = 1;
        for (int i = 0; i < sc->nretained && settled; i++) {
            VkFence fences[2] = { sc->retained[i].fence, sc->retained[i].present_fence };
            for (int k = 0; k < 2; k++) {
                if (!fences[k]) continue;   // 没有该类证明：不能当完成，也不能等一个空 fence
                int64_t left = deadline - fd_now_ns_sc();
                if (left <= 0) { settled = 0; break; }
                if (vkWaitForFences(sc->d->device, 1, &fences[k], VK_TRUE, (uint64_t)left) != VK_SUCCESS) {
                    settled = 0;
                    break;
                }
            }
        }
        if (settled) {
            for (int i = 0; i < sc->nretained; i++) {
                if (sc->retained[i].fence) vkDestroyFence(sc->d->device, sc->retained[i].fence, NULL);
                if (sc->retained[i].present_fence) vkDestroyFence(sc->d->device, sc->retained[i].present_fence, NULL);
                if (sc->retained[i].cmd) vkFreeCommandBuffers(sc->d->device, sc->d->command_pool, 1, &sc->retained[i].cmd);
            }
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                "swapchain destroy: %d retained present command set(s) completed; reclaimed explicitly",
                                sc->nretained);
            sc->nretained = 0;
        } else {
            // 已提交但没有完成/呈现证明：命令缓冲/fence（以及它们引用的 swapchain 图像）不能销毁。
            // 按窗口退休合同保留，随设备/实例销毁；不伪造 presentation 已退休。
            sc->present_failed = 1;
            snprintf(sc->present_fail, sizeof sc->present_fail,
                     "in-flight present command set has no completion proof at window retirement");
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "swapchain destroy: %d in-flight present command set(s) retained "
                                "(no completion/presentation proof); swapchain/surface kept",
                                sc->nretained);
            sc->ready = 0;
            return -1;
        }
    }
    // 初始 summary 在等待前打印；成功收口后再打一行终态，证明“无后续帧”也把 pending
    // 等到/退休到 0，而不是只凭初始 pending=1 猜结果。
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "present lifecycle settled: submitted=%llu presentation_retired=%llu "
                        "pending=%d retained=%d (no-next-frame retirement)",
                        (unsigned long long)sc->submitted, (unsigned long long)sc->retired,
                        sc->npresent_pending, sc->nretained);
    if (sc->acquire) vkDestroySemaphore(sc->d->device, sc->acquire, NULL);
    for (uint32_t i = 0; i < sc->image_count && i < 8; i++)
        if (sc->release[i]) vkDestroySemaphore(sc->d->device, sc->release[i], NULL);
    if (sc->fence) vkDestroyFence(sc->d->device, sc->fence, NULL);
    if (sc->swapchain && pDestroySwapchain) pDestroySwapchain(sc->d->device, sc->swapchain, NULL);
    if (sc->surface) vkDestroySurfaceKHR(sc->d->instance, sc->surface, NULL);
    if (g_present_sc == sc) g_present_sc = NULL;
    sc->swapchain = VK_NULL_HANDLE;
    sc->surface = VK_NULL_HANDLE;
    sc->fence = VK_NULL_HANDLE;
    sc->npresent_pending = 0;
    sc->ready = 0;
    return 0;
}
