// 宿主回归：nvn_vk_swapchain 的真实呈现退休路径（fake Vulkan，不需要设备）。
//
// 覆盖的反例/正例（都是生产代码路径，不是另写一份模型）：
//   1. present 成功 ≠ 呈现退休：present fence 与渲染 fence 必须是两个对象；绘制完成计数先到，
//      present fence 未 signal 时 pending 保留， retirement 不提前记账。
//   2. present fence signal 后才 retired；不能把渲染 fence 当呈现退休证明。
//   3. 无后续帧 Stop（窗口撤销）：present fence 未 signal 时 destroy 返回 -1，swapchain/surface/
//      fence 全部保留；signal 后第二次 destroy 才真正销毁。
//   4. 提交成功后 present 失败：命令缓冲/两把 fence 保留，不调 present_done，不伪造完成。
//   5. 渲染 fence 超时：即使 present 成功也不解锁 pin，不声称呈现退休。
//   6. present fence 能力缺失（maintenance1 未启用）：建链明确失败，不降级成渲染 fence 冒充。
//   7. debug.s2a.fail_present=2：故意不带 present fence，退休永远无法证明 → destroy 保留。
//   8. 旋转/resize：同一窗口尺寸变化 needs_rebuild=1；尺寸相同=0。
// 真机驱动行为（Adreno present fence 何时 signal）不在宿主覆盖范围内，报告中列为未验。
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_vk_swapchain.h"
#include "nvn_vk_lifecycle.h"
#include "game_touch.h"

// ---- 宿主 Android 桩 ----
static int32_t g_window_w = 100, g_window_h = 50;
int32_t ANativeWindow_getWidth(ANativeWindow *window) { (void)window; return g_window_w; }
int32_t ANativeWindow_getHeight(ANativeWindow *window) { (void)window; return g_window_h; }
// B3：记录每次缓冲尺寸请求；真实语义——之后 getWidth/getHeight 报告缓冲尺寸。
// g_geo_apply=0 模拟平台不兑现：调用返回 0 但缓冲尺寸不变。
static int g_geo_calls;
static int g_geo_apply = 1;
static int32_t g_geo_w, g_geo_h, g_geo_fmt;
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t w, int32_t h, int32_t format) {
    (void)window;
    g_geo_calls++; g_geo_w = w; g_geo_h = h; g_geo_fmt = format;
    if (g_geo_apply) { g_window_w = w; g_window_h = h; }
    return 0;
}

static struct { const char *name; const char *value; } g_props[16];
static int g_prop_count;
void fake_set_property(const char *name, const char *value) {
    for (int i = 0; i < g_prop_count; i++)
        if (!strcmp(g_props[i].name, name)) { g_props[i].value = value; return; }
    if (g_prop_count < 16) { g_props[g_prop_count].name = name; g_props[g_prop_count].value = value; g_prop_count++; }
}
int __system_property_get(const char *name, char *value) {
    for (int i = 0; i < g_prop_count; i++)
        if (!strcmp(g_props[i].name, name)) { strcpy(value, g_props[i].value); return (int)strlen(value); }
    value[0] = 0;
    return 0;
}
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag; (void)fmt;
    return 0;
}

// ---- fake Vulkan ----
struct fake_fence { int used, signaled, destroyed, is_present, never_signal; };
static struct fake_fence g_fences[64];
static int g_submit_calls, g_present_calls, g_acquire_calls;
static int g_submit_result = VK_SUCCESS;
static int g_present_result = VK_SUCCESS;
static int g_acquire_result = VK_SUCCESS;
static uint32_t g_acquire_index;
static VkResult g_alloc_cmd_result = VK_SUCCESS;
static VkSemaphore g_last_submit_signal, g_last_present_wait;
static int g_destroyed_semaphores;
static int g_render_fence_signal_on_submit = 1;
static int g_present_fence_signal_on_present = 0;
static VkFence g_last_submit_fence;
static VkFence g_captured_present_fence;
static int g_present_fence_info_seen;
static int g_present_fence_shared_with_submit;
static int g_present_fence_was_signaled;
static uint64_t g_max_wait_timeout_ns;
static uint64_t g_max_present_wait_timeout_ns;
static int g_acquire_after_fail;
static int g_destroyed_swapchains, g_destroyed_surfaces;
static int g_present_done_calls;
static int g_revoke_on_present;
static VkImageBlit g_last_blit;

static struct fake_fence *F(VkFence f) {
    for (int i = 0; i < 64; i++) if (g_fences[i].used && (VkFence)&g_fences[i] == f) return &g_fences[i];
    return NULL;
}
static VkFence new_fence(void) {
    for (int i = 0; i < 64; i++)
        if (!g_fences[i].used) {
            memset(&g_fences[i], 0, sizeof g_fences[i]);
            g_fences[i].used = 1;
            return (VkFence)&g_fences[i];
        }
    fprintf(stderr, "fake: fence table full\n");
    abort();
}
static void fake_reset(void) {
    memset(g_fences, 0, sizeof g_fences);
    g_submit_calls = g_present_calls = g_acquire_calls = 0;
    g_submit_result = g_present_result = g_acquire_result = g_alloc_cmd_result = VK_SUCCESS;
    g_acquire_index = 0;
    g_last_submit_signal = g_last_present_wait = VK_NULL_HANDLE;
    g_destroyed_semaphores = 0;
    g_render_fence_signal_on_submit = 1;
    g_present_fence_signal_on_present = 0;
    g_last_submit_fence = VK_NULL_HANDLE;
    g_captured_present_fence = VK_NULL_HANDLE;
    g_present_fence_info_seen = 0;
    g_present_fence_shared_with_submit = 0;
    g_present_fence_was_signaled = 0;
    g_max_wait_timeout_ns = 0;
    g_max_present_wait_timeout_ns = 0;
    g_acquire_after_fail = 0;
    g_destroyed_swapchains = g_destroyed_surfaces = 0;
    g_present_done_calls = 0;
    g_revoke_on_present = 0;
    memset(&g_last_blit, 0, sizeof g_last_blit);
    g_window_w = 100; g_window_h = 50;
    g_geo_calls = 0; g_geo_apply = 1; g_geo_w = g_geo_h = g_geo_fmt = 0;
    g_prop_count = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice device, const VkFenceCreateInfo *info,
                                             const VkAllocationCallbacks *alloc, VkFence *out) {
    (void)device; (void)alloc;
    VkFence f = new_fence();
    if (info && (info->flags & VK_FENCE_CREATE_SIGNALED_BIT)) F(f)->signaled = 1;
    *out = f;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *alloc) {
    (void)device; (void)alloc;
    struct fake_fence *f = F(fence);
    if (!f) { fprintf(stderr, "fake: destroy unknown fence\n"); abort(); }
    if (f->destroyed) { fprintf(stderr, "fake: double destroy fence\n"); abort(); }
    f->destroyed = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t count, const VkFence *fences) {
    (void)device;
    for (uint32_t i = 0; i < count; i++) if (F(fences[i])) F(fences[i])->signaled = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences,
                                               VkBool32 wait_all, uint64_t timeout) {
    (void)device; (void)wait_all;
    if (timeout > g_max_wait_timeout_ns) g_max_wait_timeout_ns = timeout;
    for (uint32_t i = 0; i < count; i++) {
        struct fake_fence *f = F(fences[i]);
        if (f && f->is_present && timeout > g_max_present_wait_timeout_ns)
            g_max_present_wait_timeout_ns = timeout;
        if (!f) { fprintf(stderr, "fake: wait unknown fence\n"); abort(); }
        if (f->never_signal) return VK_TIMEOUT;
        if (!f->signaled) return VK_TIMEOUT;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence) {
    (void)device;
    struct fake_fence *f = F(fence);
    if (!f) return VK_ERROR_UNKNOWN;
    return f->signaled ? VK_SUCCESS : VK_NOT_READY;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
                                             VkFence fence) {
    (void)queue; (void)count; (void)submits;
    g_submit_calls++;
    g_last_submit_fence = fence;
    if (submits && submits[0].pSignalSemaphores) g_last_submit_signal = submits[0].pSignalSemaphores[0];
    if (g_submit_result != VK_SUCCESS) return g_submit_result;
    if (g_render_fence_signal_on_submit && F(fence)) F(fence)->signaled = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) { (void)queue; return VK_SUCCESS; }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *i,
                                                 const VkAllocationCallbacks *a, VkSemaphore *s) {
    (void)d; (void)i; (void)a;
    static int dummy[8];
    static int n;
    *s = (VkSemaphore)&dummy[n++ % 8];
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) {
    (void)d; (void)s; (void)a;
    g_destroyed_semaphores++;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *i,
                                                        VkCommandBuffer *cbs) {
    (void)d; (void)i;
    if (g_alloc_cmd_result != VK_SUCCESS) return g_alloc_cmd_result;
    static int dummy[8];
    static int n;
    for (uint32_t k = 0; k < i->commandBufferCount; k++) cbs[k] = (VkCommandBuffer)&dummy[n++ % 8];
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t n,
                                                const VkCommandBuffer *cbs) {
    (void)d; (void)p; (void)n; (void)cbs;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *i) {
    (void)cb; (void)i; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) { (void)cb; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags a, VkPipelineStageFlags b,
                                                VkDependencyFlags c, uint32_t d, const VkMemoryBarrier *e,
                                                uint32_t f, const VkBufferMemoryBarrier *g, uint32_t h,
                                                const VkImageMemoryBarrier *i) {
    (void)cb; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
}
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l,
                                                const VkClearColorValue *c, uint32_t n,
                                                const VkImageSubresourceRange *r) {
    (void)cb; (void)img; (void)l; (void)c; (void)n; (void)r;
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                                                  VkBuffer dst, uint32_t count, const VkBufferImageCopy *regions) {
    (void)cb; (void)src; (void)layout; (void)dst; (void)count; (void)regions;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                          VkImageLayout dl, uint32_t n, const VkImageBlit *b, VkFilter f) {
    (void)cb; (void)s; (void)sl; (void)d; (void)dl; (void)f;
    if (n && b) g_last_blit = b[0];
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *i,
                                                 const VkAllocationCallbacks *a, VkImageView *v) {
    (void)d; (void)i; (void)a;
    static int dummy;
    *v = (VkImageView)&dummy;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) {
    (void)d; (void)v; (void)a;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *i,
                                              const VkAllocationCallbacks *a, VkBuffer *b) {
    (void)d; (void)i; (void)a;
    static int dummy;
    *b = (VkBuffer)&dummy;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void)d; (void)b;
    memset(r, 0, sizeof *r);
    r->size = 256; r->alignment = 16; r->memoryTypeBits = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i,
                                                const VkAllocationCallbacks *a, VkDeviceMemory *m) {
    (void)d; (void)i; (void)a;
    static int dummy;
    *m = (VkDeviceMemory)&dummy;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void)d; (void)m; (void)a;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
    (void)d; (void)b; (void)m; (void)o; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s,
                                           VkMemoryMapFlags f, void **p) {
    (void)d; (void)m; (void)o; (void)s; (void)f;
    static char page[4096];
    *p = page;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) {
    (void)d; (void)b; (void)a;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *a) {
    (void)d; (void)f; (void)a;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) {
    (void)i; (void)s; (void)a;
    g_destroyed_surfaces++;
}

static VkResult fake_create_surface(VkInstance i, const VkAndroidSurfaceCreateInfoKHR *ci,
                                    const VkAllocationCallbacks *a, VkSurfaceKHR *s) {
    (void)i; (void)ci; (void)a;
    static int dummy;
    *s = (VkSurfaceKHR)&dummy;
    return VK_SUCCESS;
}
static VkResult fake_support(VkPhysicalDevice p, uint32_t q, VkSurfaceKHR s, VkBool32 *out) {
    (void)p; (void)q; (void)s;
    *out = VK_TRUE;
    return VK_SUCCESS;
}
static void fake_caps(VkPhysicalDevice p, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *caps) {
    (void)p; (void)s;
    memset(caps, 0, sizeof *caps);
    caps->minImageCount = 2; caps->maxImageCount = 4;
    caps->currentExtent = (VkExtent2D){100, 50};
    caps->minImageExtent = (VkExtent2D){1, 1};
    caps->maxImageExtent = (VkExtent2D){4096, 4096};
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
static void fake_formats(VkPhysicalDevice p, VkSurfaceKHR s, uint32_t *count, VkSurfaceFormatKHR *out) {
    (void)p; (void)s;
    if (!out) { *count = 1; return; }
    *count = 1;
    out[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    out[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}
static void fake_modes(VkPhysicalDevice p, VkSurfaceKHR s, uint32_t *count, VkPresentModeKHR *out) {
    (void)p; (void)s;
    if (!out) { *count = 1; return; }
    *count = 1;
    out[0] = VK_PRESENT_MODE_FIFO_KHR;
}
static VkResult fake_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci,
                                      const VkAllocationCallbacks *a, VkSwapchainKHR *sc) {
    (void)d; (void)ci; (void)a;
    static int dummy;
    *sc = (VkSwapchainKHR)&dummy;
    return VK_SUCCESS;
}
static void fake_destroy_swapchain(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    (void)d; (void)sc; (void)a;
    g_destroyed_swapchains++;
}
static VkResult fake_get_images(VkDevice d, VkSwapchainKHR sc, uint32_t *count, VkImage *images) {
    (void)d; (void)sc;
    if (!images) { *count = 3; return VK_SUCCESS; }
    for (uint32_t i = 0; i < 3; i++) images[i] = (VkImage)(uintptr_t)(i + 1);
    *count = 3;
    return VK_SUCCESS;
}
static VkResult fake_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence,
                             uint32_t *index) {
    (void)d; (void)sc; (void)timeout; (void)sem; (void)fence;
    g_acquire_calls++;
    *index = g_acquire_index;
    return g_acquire_result;
}
static VkResult fake_present(VkQueue q, const VkPresentInfoKHR *info) {
    (void)q;
    g_present_calls++;
    if (g_revoke_on_present) s2a_android_game_touch_retire();
    g_last_present_wait = info && info->pWaitSemaphores ? info->pWaitSemaphores[0] : VK_NULL_HANDLE;
    if (info && info->pNext) {
        const VkBaseInStructure *base = (const VkBaseInStructure *)info->pNext;
        if (base->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT) {
            const VkSwapchainPresentFenceInfoEXT *fi = (const VkSwapchainPresentFenceInfoEXT *)info->pNext;
            g_present_fence_info_seen = 1;
            g_captured_present_fence = fi->pFences[0];
            if (fi->pFences[0] == g_last_submit_fence) g_present_fence_shared_with_submit = 1;
            struct fake_fence *f = F(fi->pFences[0]);
            if (f && f->signaled) g_present_fence_was_signaled = 1;
            if (f) f->is_present = 1;
        }
    }
    if (g_present_result != VK_SUCCESS) return g_present_result;
    if (g_present_fence_signal_on_present && g_captured_present_fence && F(g_captured_present_fence))
        F(g_captured_present_fence)->signaled = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (!strcmp(name, "vkCreateAndroidSurfaceKHR")) return (PFN_vkVoidFunction)fake_create_surface;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR")) return (PFN_vkVoidFunction)fake_support;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return (PFN_vkVoidFunction)fake_caps;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR")) return (PFN_vkVoidFunction)fake_formats;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR")) return (PFN_vkVoidFunction)fake_modes;
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    (void)device;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)fake_create_swapchain;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)fake_destroy_swapchain;
    if (!strcmp(name, "vkGetSwapchainImagesKHR")) return (PFN_vkVoidFunction)fake_get_images;
    if (!strcmp(name, "vkAcquireNextImageKHR")) return (PFN_vkVoidFunction)fake_acquire;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)fake_present;
    return NULL;
}

// ---- swapchain 依赖的最小桩 ----
int nvn_vk_pipeline_record(struct nvn_vk_pipeline_cache *pc, VkPipeline pipe, VkCommandBuffer cmd,
                           VkImageView view, uint32_t w, uint32_t h, VkBuffer vb, VkBuffer ubo,
                           VkDeviceSize ubo_size, VkFramebuffer *fb_out) {
    (void)pc; (void)pipe; (void)cmd; (void)view; (void)w; (void)h; (void)vb; (void)ubo; (void)ubo_size;
    if (fb_out) *fb_out = VK_NULL_HANDLE;
    return 0;
}
uint32_t s2a_vk_memory_type(const struct s2a_vk_device *d, uint32_t bits, VkMemoryPropertyFlags req,
                            VkMemoryPropertyFlags *actual) {
    (void)d; (void)bits; (void)req;
    if (actual) *actual = req;
    return 0;
}
int nvn_vk_first_draw_flush(void) { return 0; }
void nvn_vk_first_draw_present_done(void) { g_present_done_calls++; }
const char *nvn_vk_first_draw_first_error(void) { return ""; }
// #868 起 nvn_vk_swapchain.c 还调首帧诊断 / 延迟上屏这几支（定义在 nvn_vk_first_draw.c）：
//   替身按「不延迟、不记时间戳」实现，保持本测试原有路径（10-30 列车线：缺这几个替身，Linux CTest 链接失败）。
//   不直接链 nvn_vk_first_draw.c 的原因（10-30 实测）：它多出 76 个未定义符号——38 个 Vulkan 入口要另做假实现，
//   另 8 个模块里 mtl_vk_adapter.c 又拖进 nvn_vk_draw / lifecycle / exec_owner / direct 编译 worker / program_cache，
//   等于把整套安卓 NVN 运行时搬进一个交换链单测。改动首帧文件时，这几支的行为由真机 / 安卓门禁覆盖。
int nvn_vk_first_draw_flush_defer(void) { return 0; }
// W6.0 呈现账：宿主 fake Vulkan 不跑 GPU 查询/延迟帧，返回 unavailable，不伪造读数。
int nvn_vk_first_draw_gpu_last(double *ms, int *passes, uint64_t *frame) {
    (void)ms; (void)passes; (void)frame;
    return 0;
}
uint64_t nvn_vk_first_draw_deferred_retired(void) { return 0; }
void nvn_vk_callring_add(const char *fmt, ...) { (void)fmt; }   // 诊断调用环，宿主测试不记
int nvn_vk_first_draw_present_deferred(void) { return 0; }
void nvn_vk_first_draw_slot_blit(VkFence fence, VkCommandBuffer cmd) { (void)fence; (void)cmd; }
void nvn_vk_first_draw_gpu_end(VkCommandBuffer cmd) { (void)cmd; }
void nvn_vk_first_draw_gpu_read(struct s2a_vk_device *d, const char *where) { (void)d; (void)where; }

// ---- 断言 ----
static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static struct s2a_vk_device g_dev;
static struct nvn_vk_swapchain g_sc;
static ANativeWindow *g_window = (ANativeWindow *)(uintptr_t)0x1234;
static char g_create_why[192];

static int create_swapchain_keep(int maintenance1) {
    memset(&g_dev, 0, sizeof g_dev);
    memset(&g_sc, 0, sizeof g_sc);
    g_dev.instance = (VkInstance)(uintptr_t)0x1;
    g_dev.physical = (VkPhysicalDevice)(uintptr_t)0x2;
    g_dev.device = (VkDevice)(uintptr_t)0x3;
    g_dev.queue = (VkQueue)(uintptr_t)0x4;
    g_dev.command_pool = (VkCommandPool)(uintptr_t)0x5;
    g_dev.swapchain_maintenance1_enabled = maintenance1;
    g_create_why[0] = 0;
    int rc = nvn_vk_swapchain_create(&g_sc, &g_dev, g_window, g_create_why, sizeof g_create_why);
    if (rc) printf("create: %s\n", g_create_why);
    return rc;
}
static int create_swapchain(int maintenance1) {
    fake_reset();
    return create_swapchain_keep(maintenance1);
}

int main(void) {
    char why[192] = "";
    VkImage src = (VkImage)(uintptr_t)0x777;

    // Production present publishes the exact centered 16:9 blit for the held WSI generation.
    s2a_android_game_touch_revoke();
    if (create_swapchain(1) == 0) {
        g_sc.touch_generation = 42;
        s2a_android_game_touch_offer(42);
        g_present_fence_signal_on_present = 1;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        int64_t vp[8] = {0};
        CHECK(rc == 0, "touch fixture present rc=%d", rc);
        CHECK(g_last_blit.dstOffsets[0].x == 5 && g_last_blit.dstOffsets[0].y == 0 &&
              g_last_blit.dstOffsets[1].x == 94 && g_last_blit.dstOffsets[1].y == 50,
              "actual 16:9 destination is centered 89x50");
        CHECK(s2a_android_game_touch_viewport_read(vp) == 0 && vp[0] == 42 &&
              vp[2] == 5 && vp[3] == 0 && vp[4] == 89 && vp[5] == 50 && vp[6] == 100 && vp[7] == 50,
              "touch viewport must match the submitted blit and WSI generation");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "touch fixture cleanly retires");
    }
    s2a_android_game_touch_revoke();
    if (create_swapchain(1) == 0) {
        g_sc.touch_generation = 43; s2a_android_game_touch_offer(43);
        g_present_result = VK_ERROR_DEVICE_LOST;
        (void)nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        int64_t vp[8] = {0};
        CHECK(s2a_android_game_touch_viewport_read(vp) != 0, "failed present does not publish touch");
    }
    if (create_swapchain(1) == 0) {
        g_sc.touch_generation = 44; s2a_android_game_touch_offer(44);
        g_present_fence_signal_on_present = 1; g_revoke_on_present = 1;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        int64_t vp[8] = {0};
        CHECK(rc == 0 && s2a_android_game_touch_viewport_read(vp) != 0,
              "first frame finishing after generation retirement must not publish");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "revoked fixture cleanly retires");
    }
    if (create_swapchain(1) == 0) {
        g_sc.touch_generation = 45; s2a_android_game_touch_offer(45);
        s2a_android_game_touch_viewport(45, 0, 0, 100, 50, 100, 50);
        g_present_fence_signal_on_present = 1;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 100, NULL, why, sizeof why);
        int64_t vp[8] = {0};
        CHECK(rc == 0 && s2a_android_game_touch_viewport_read(vp) != 0,
              "non-16:9 source revokes an earlier touch viewport");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "nonstandard fixture cleanly retires");
    }

    // 6. maintenance1 未启用：建链必须失败（不能拿渲染 fence 冒充呈现退休）。
    CHECK(create_swapchain(0) != 0, "create without maintenance1 must fail");
    CHECK(strstr(g_create_why, "maintenance1") != NULL || strstr(g_create_why, "retirement") != NULL,
          "failure reason names presentation retirement");

    // 1/2. present 成功 ≠ 呈现退休；两把 fence 必须不同。
    if (create_swapchain(1) == 0) {
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc == 0, "present success rc=%d (%s)", rc, why);
        CHECK(g_present_calls == 1, "present called once");
        CHECK(g_present_fence_info_seen, "present carries VkSwapchainPresentFenceInfoEXT");
        CHECK(!g_present_fence_shared_with_submit, "present fence is distinct from the render submit fence");
        CHECK(!g_present_fence_was_signaled, "present fence is unsignaled when handed to the presentation engine");
        CHECK(g_present_done_calls == 1, "GPU draw completion releases texture pins");
        struct nvn_vk_swapchain_present_stats st;
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.submitted == 1 && st.completed == 1, "submitted+draw-completed accounted");
        CHECK(st.retired == 0 && st.pending == 1, "presentation retirement is NOT claimed when the fence is unsignaled");
        CHECK(nvn_vk_swapchain_retire_present(&g_sc, 0) == 1, "nonblocking retire keeps unsignaled present fence");
        CHECK(g_present_done_calls == 1, "deferred retirement does not unpin again");
        F(g_captured_present_fence)->signaled = 1;
        CHECK(nvn_vk_swapchain_retire_present(&g_sc, 0) == 0, "signaled present fence retires");
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.retired == 1 && st.pending == 0, "retired after present fence signals");
        CHECK(F(g_captured_present_fence)->destroyed, "retired present fence destroyed once");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "destroy after all retirements");
        CHECK(g_destroyed_swapchains == 1 && g_destroyed_surfaces == 1, "swapchain/surface destroyed once");
    }

    // 2b. 探针用的 clear-present 也走同一把 present fence，并同步观察退休。
    if (create_swapchain(1) == 0) {
        g_present_fence_signal_on_present = 1;   // 探针路径同步等 present fence；模拟正常退休
        int rc = nvn_vk_swapchain_clear_present(&g_sc, 0.1f, 0.2f, 0.3f, 1.0f, why, sizeof why);
        CHECK(rc == 0, "clear present succeeds rc=%d (%s)", rc, why);
        CHECK(g_present_fence_info_seen, "clear present attaches a present fence too");
        CHECK(g_captured_present_fence && F(g_captured_present_fence)->destroyed,
              "clear present retires and destroys its present fence");
        struct nvn_vk_swapchain_present_stats st;
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.retired == 1 && st.pending == 0, "clear present accounts a real retirement");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "destroy after clear present");
    }

    // 2c. per-image present 信号量：不同 image 用不同的 release，同一 image 才复用；
    //     共用一把会在 present fence 未 signal 时重复 signal（二值信号量违规）。
    if (create_swapchain(1) == 0) {
        g_present_fence_signal_on_present = 1;
        g_acquire_index = 0;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc == 0 && g_last_submit_signal == g_last_present_wait,
              "present waits the same per-image release semaphore it submitted");
        VkSemaphore rel0 = g_last_submit_signal;
        g_acquire_index = 1;
        rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc == 0 && g_last_submit_signal != rel0,
              "a different swapchain image uses a different release semaphore");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "destroy after per-image semaphore presents");
        CHECK(g_destroyed_semaphores == 1 + 3, "one acquire + one release per image destroyed once");
        g_present_fence_signal_on_present = 0;
    }

    // 2d. acquire 成功但提交前失败：没有提交去消费 acquire 信号量 → 不能销毁；Stop 保留。
    if (create_swapchain(1) == 0) {
        g_alloc_cmd_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc < 0, "command buffer failure after acquire is reported");
        CHECK(g_present_calls == 0, "no present was submitted");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == -1,
              "destroy refuses while an acquired image has no submit consuming its semaphore");
        CHECK(g_destroyed_swapchains == 0 && g_destroyed_semaphores == 0,
              "swapchain and semaphores kept without acquire-completion proof");
        g_alloc_cmd_result = VK_SUCCESS;
    }

    // 2e. 探针 clear-present 在 acquire 后失败：必须进入终态，下一次调用不得复用同一把 acquire
    //     信号量（否则重复 signal），destroy 也必须拒绝。
    if (create_swapchain(1) == 0) {
        g_alloc_cmd_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        int rc = nvn_vk_swapchain_clear_present(&g_sc, 0.0f, 0.0f, 0.0f, 1.0f, why, sizeof why);
        CHECK(rc < 0, "clear present failure after acquire is reported");
        CHECK(g_acquire_calls == 1, "one acquire happened");
        rc = nvn_vk_swapchain_clear_present(&g_sc, 0.0f, 0.0f, 0.0f, 1.0f, why, sizeof why);
        CHECK(rc < 0 && g_acquire_calls == 1, "retry does not acquire again with an unconsumed semaphore");
        rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc < 0 && g_acquire_calls == 1, "present image also refuses while acquire is unconsumed");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == -1, "destroy refuses with unconsumed acquire");
        g_alloc_cmd_result = VK_SUCCESS;
    }

    // 3. 无后续帧 Stop：present fence 未 signal 时 destroy 保留全部对象；观察到后第二次才销毁。
    if (create_swapchain(1) == 0) {
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc == 0, "present before stop rc=%d", rc);
        F(g_captured_present_fence)->never_signal = 1;
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == -1, "destroy without present retirement proof fails");
        CHECK(g_destroyed_swapchains == 0 && g_destroyed_surfaces == 0, "swapchain/surface kept");
        CHECK(!F(g_captured_present_fence)->destroyed, "present fence kept");
        CHECK(g_max_present_wait_timeout_ns > 0 &&
              g_max_present_wait_timeout_ns <= (uint64_t)NVK_PRESENT_RETIRE_BUDGET_NS,
              "present-fence wait is bounded (%llu ns)", (unsigned long long)g_max_present_wait_timeout_ns);
        F(g_captured_present_fence)->never_signal = 0;
        F(g_captured_present_fence)->signaled = 1;
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "destroy after the last present fence signals");
        CHECK(g_destroyed_swapchains == 1 && g_destroyed_surfaces == 1, "swapchain/surface destroyed after proof");
        CHECK(F(g_captured_present_fence)->destroyed, "present fence destroyed after proof");
    }

    // 4. 提交成功后 present 失败：命令/两把 fence 保留，不调 present_done。
    if (create_swapchain(1) == 0) {
        g_present_result = VK_ERROR_DEVICE_LOST;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc != 0, "present failure is reported");
        CHECK(g_present_done_calls == 0, "failed present does not release texture pins");
        struct nvn_vk_swapchain_present_stats st;
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.retained == 1 && st.present_failed == 1, "retained command set + present failure accounted");
        CHECK(!F(g_last_submit_fence)->destroyed, "render fence retained");
        CHECK(g_captured_present_fence == VK_NULL_HANDLE || !F(g_captured_present_fence)->destroyed,
              "present fence retained when present failed after submit");
        int before = g_acquire_calls;
        CHECK(nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why) != 0,
              "failed present path refuses new presents");
        CHECK(g_acquire_calls == before, "no new acquire on the failed swapchain");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == -1, "failed present path cannot claim retirement at destroy");
    }

    // 5. 渲染 fence 超时：即使 present 成功也不宣称呈现退休、不解锁 pin。
    if (create_swapchain(1) == 0) {
        g_render_fence_signal_on_submit = 0;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc != 0, "draw-completion timeout fails present");
        CHECK(g_present_done_calls == 0, "no pin release without GPU completion proof");
        struct nvn_vk_swapchain_present_stats st;
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.fence_timeout == 1 && st.completed == 0, "draw-completion timeout accounted separately");
        CHECK(F(g_last_submit_fence)->never_signal == 0, "fence retained, not fabricated");
    }

    // 9. acquire 有界超时：本帧绘制完成但没拿到图像 → 跳过上屏、pin 释放、不是失败终态。
    if (create_swapchain(1) == 0) {
        g_acquire_result = VK_TIMEOUT;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc == 1, "acquire timeout reports frame-completed/present-skipped rc=%d", rc);
        CHECK(g_present_calls == 0, "acquire timeout does not call present");
        CHECK(g_present_done_calls == 1, "acquire timeout releases this frame\'s pins");
        struct nvn_vk_swapchain_present_stats st;
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.acquire_timeout == 1 && st.present_failed_flag == 0, "acquire timeout is not a terminal failure");
        g_acquire_result = VK_ERROR_SURFACE_LOST_KHR;
        rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc < 0, "surface-lost acquire is a real failure");
    }

    // 10. acquire 直接 DEVICE_LOST：真实失败，不能当 acquire timeout 跳过。
    if (create_swapchain(1) == 0) {
        g_acquire_result = VK_ERROR_DEVICE_LOST;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc < 0, "device-lost acquire is a real failure");
        CHECK(g_present_done_calls == 0, "device-lost acquire does not release texture pins");
        struct nvn_vk_swapchain_present_stats st;
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.present_failed_flag == 1 && st.acquire_timeout == 0,
              "device-lost acquire is terminal, not an acquire timeout");
    }

    // 7. 诊断注入 2：present 成功但没有 present fence → 退休永远不能证明，Stop 保留。
    if (create_swapchain(1) == 0) {
        fake_set_property("debug.s2a.fail_present", "2");
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 100, 50, NULL, why, sizeof why);
        CHECK(rc == 0, "present without injected fence succeeds at submit level rc=%d", rc);
        CHECK(!g_present_fence_info_seen, "fault mode 2 deliberately omits the present fence");
        struct nvn_vk_swapchain_present_stats st;
        nvn_vk_swapchain_present_stats(&st);
        CHECK(st.pending == 1 && st.retired == 0, "unproven retirement is pending, not retired");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == -1, "destroy cannot claim retirement without a present fence");
        CHECK(g_destroyed_swapchains == 0, "swapchain kept when retirement is unproven");
    }

    // B3：debug.s2a.buffer_geometry=1 → 首个 present 按游戏输出尺寸改缓冲并退休旧交换链重建；
    //     之后 blit 退化成 1:1 拷贝；触摸按 view frame 像素发布整帧；同尺寸不重建；crop 变再重建。
    s2a_android_game_touch_revoke();
    fake_reset();
    fake_set_property("debug.s2a.buffer_geometry", "1");
    if (create_swapchain_keep(1) == 0) {
        CHECK(g_geo_calls == 0, "no geometry request before the first output size");
        CHECK(g_sc.extent.width == 100 && g_sc.extent.height == 50, "initial extent is the window size");
        g_sc.touch_generation = 60; s2a_android_game_touch_offer(60);
        g_present_fence_signal_on_present = 1;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        CHECK(rc == 0, "geometry present rc=%d (%s)", rc, why);
        CHECK(g_geo_calls == 1 && g_geo_w == 160 && g_geo_h == 90,
              "first present resizes the buffer to the game output 160x90");
        CHECK(g_destroyed_swapchains == 1 && g_destroyed_surfaces == 1,
              "previous swapchain retired for the resize");
        CHECK(g_sc.extent.width == 160 && g_sc.extent.height == 90, "new extent = game output size");
        CHECK(g_sc.touch_generation == 60, "WSI generation kept across the rebuild");
        CHECK(g_last_blit.dstOffsets[0].x == 0 && g_last_blit.dstOffsets[0].y == 0 &&
              g_last_blit.dstOffsets[1].x == 160 && g_last_blit.dstOffsets[1].y == 90,
              "blit degenerates to a 1:1 copy filling the whole game-size buffer");
        int64_t vp[8] = {0};
        CHECK(s2a_android_game_touch_viewport_read(vp) == 0 && vp[0] == 60 &&
              vp[2] == 0 && vp[3] == 0 && vp[4] == 100 && vp[5] == 50 && vp[6] == 100 && vp[7] == 50,
              "touch viewport publishes the whole view in frame pixels");
        int before_destroy = g_destroyed_swapchains, before_geo = g_geo_calls;
        rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        CHECK(rc == 0 && g_destroyed_swapchains == before_destroy && g_geo_calls == before_geo,
              "same output size does not rebuild");
        int crop4[4] = {0, 0, 80, 45};
        rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, crop4, why, sizeof why);
        CHECK(rc == 0 && g_geo_calls == before_geo + 1 && g_geo_w == 80 && g_geo_h == 45,
              "a different crop rebuilds the buffer to the new output size");
        CHECK(g_sc.extent.width == 80 && g_sc.extent.height == 45, "extent follows the crop");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "geometry fixture cleanly retires");
    }
    s2a_android_game_touch_revoke();

    // B3 降级：setBuffersGeometry 返回 0 但缓冲尺寸没动（平台不兑现）→ 不付销毁重建的代价、
    //     不重试，extent 留在窗口尺寸，之后按普通等比 blit 出图；触摸按缓冲像素发布。
    s2a_android_game_touch_revoke();
    fake_reset();
    fake_set_property("debug.s2a.buffer_geometry", "1");
    if (create_swapchain_keep(1) == 0) {
        g_present_fence_signal_on_present = 1;
        g_geo_apply = 0;
        g_sc.touch_generation = 61; s2a_android_game_touch_offer(61);
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        CHECK(rc == 0, "unhonored geometry still presents rc=%d (%s)", rc, why);
        CHECK(g_geo_calls == 1 && g_destroyed_swapchains == 0,
              "unhonored geometry: one request, no swapchain churn");
        CHECK(g_sc.extent.width == 100 && g_sc.extent.height == 50,
              "unhonored geometry keeps the frame-size extent");
        CHECK(g_sc.buf_geometry && g_sc.geometry_failed, "degrade is marked, not retried silently");
        rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        CHECK(rc == 0 && g_geo_calls == 1 && g_destroyed_swapchains == 0,
              "unhonored geometry does not retry rebuild per frame");
        CHECK(g_last_blit.dstOffsets[0].x == 5 && g_last_blit.dstOffsets[0].y == 0 &&
              g_last_blit.dstOffsets[1].x == 94 && g_last_blit.dstOffsets[1].y == 50,
              "unhonored geometry falls back to the aspect-fit blit");
        int64_t vp[8] = {0};
        CHECK(s2a_android_game_touch_viewport_read(vp) == 0 && vp[0] == 61 &&
              vp[2] == 5 && vp[3] == 0 && vp[4] == 89 && vp[5] == 50 && vp[6] == 100 && vp[7] == 50,
              "degraded touch viewport publishes the blit rect in buffer pixels");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "degraded fixture cleanly retires");
    }
    s2a_android_game_touch_revoke();

    // B3 负控：开关关 → present 全程不调 setBuffersGeometry、不重建，等比居中沿用窗口尺寸缓冲。
    if (create_swapchain(1) == 0) {
        g_present_fence_signal_on_present = 1;
        int rc = nvn_vk_swapchain_present_image(&g_sc, src, VK_FORMAT_R8G8B8A8_UNORM, 160, 90, NULL, why, sizeof why);
        CHECK(rc == 0 && g_geo_calls == 0 && g_destroyed_swapchains == 0,
              "flag off: no setBuffersGeometry call, no swapchain rebuild");
        CHECK(g_last_blit.dstOffsets[0].x == 5 && g_last_blit.dstOffsets[0].y == 0 &&
              g_last_blit.dstOffsets[1].x == 94 && g_last_blit.dstOffsets[1].y == 50,
              "flag off keeps the aspect-fit blit inside the window-size buffer");
        CHECK(nvn_vk_swapchain_destroy(&g_sc) == 0, "disabled fixture cleanly retires");
    }

    if (g_failures) {
        printf("nvn_vk_swapchain host test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("nvn_vk_swapchain host test: ok (present fence vs draw fence, pending retirement, bounded stop, failure retention, rotation rebuild)\n");
    return 0;
}
