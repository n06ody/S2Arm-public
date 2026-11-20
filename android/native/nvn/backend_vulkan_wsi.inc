// nvn_gpu_* 上屏部分（由 backend_vulkan.c include）：交换链、取图、把源视图贴进取到的图并呈现。
// 不含任何平台调用：窗口 → VkSurfaceKHR 由平台件 nvn_plat_vk_surface_create 给，窗口尺寸由 nvn_plat_window_size 给。
// 呈现用后端自己的内部段：与游戏的段走同一把提交锁，所以在 GPU 上排在此前已提交的段之后；
// 内部段不占时间线上的值，完成后只做资源回收。
#include "nvn_plat_vk.h"
#include "nvn_plat.h"
#include "backend_vulkan_timing.h"

struct nvn_gpu_surface_s {
    void *native;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkImage encoded;                         // sRGB 缩放后按位复制到非线性 UNORM 交换链，避免丢掉编码
    VkDeviceMemory encoded_mem;
    VkExtent2D extent;
    uint32_t nimages;
    VkImage *images;
    VkSemaphore *acquire;   // 每次取图用下一把：一把信号量在被对应的呈现段等掉之前不能再交给取图
    VkSemaphore *rendered;      // 每张图一把：呈现段完成时发，vkQueuePresentKHR 等
    uint32_t acquire_next;
    unsigned char *acquire_pending;             // 每槽一字节：取图发过、还没被呈现段等掉；不受 32 位掩码限制
    uint64_t *acquire_waiter;                   // 每槽：最后一个等这把信号量的呈现段序号（g.wsi_seq）；0 = 没有
    uint64_t gen;                               // 0 = 已撤销
    int outdated;
    int64_t held;                                   // 取了没呈现、留作下次取图结果的那张（没有就是 -1）
    uint32_t held_sem;
    uint64_t presents;
    struct wsi_timing timing;
    VkSurfaceTransformFlagBitsKHR built_transform;   // 建链时表面方向；有效尺寸在 extent（见 wsi_suboptimal_stale）
    uint32_t subopt_n;
};

// 建链与次优检查必须用同一套几何：UINT32_MAX 是「由窗口定尺寸」，不能拿哨兵值判 resize。
// 固定 currentExtent 是 consumer 的首选尺寸；Android 的窗口查询可能已是 buffer geometry，不能反过来覆盖它。
static nvn_gpu_rc wsi_geometry(struct nvn_gpu_surface_s *s, const VkSurfaceCapabilitiesKHR *caps, VkExtent2D *out, nvn_gpu_err *err) {
    if (!(caps->supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "surface 不支持 identity preTransform，内容预旋转未实现");
    // 已接着的窗口暂时没有绘制面积，不等于 surface 已丢失；保留代际，尺寸恢复后还可 rebuild。
    int unavailable = s->gen ? NVN_GPU_E_OUTDATED : NVN_GPU_E_REVOKED;
    VkExtent2D ext = caps->currentExtent;
    if (ext.width == UINT32_MAX && ext.height == UINT32_MAX) {
        if (!nvn_plat_window_size(s->native, &ext.width, &ext.height) || !ext.width || !ext.height)
            return fail(err, unavailable, "窗口没有尺寸");
    }
    // 0 不是未定义尺寸，也不是无限上限（与 maxImageCount 的 0 不同）。最小化的 surface 可以报 max=(0,0)。
    if (!ext.width || !ext.height || !caps->maxImageExtent.width || !caps->maxImageExtent.height)
        return fail(err, unavailable, "表面没有可建交换链的非零尺寸");
    if (ext.width < caps->minImageExtent.width) ext.width = caps->minImageExtent.width;
    if (ext.height < caps->minImageExtent.height) ext.height = caps->minImageExtent.height;
    if (ext.width > caps->maxImageExtent.width) ext.width = caps->maxImageExtent.width;
    if (ext.height > caps->maxImageExtent.height) ext.height = caps->maxImageExtent.height;
    *out = ext;
    return rc(NVN_GPU_OK);
}

// 取图 / 呈现回「次优」（VK_SUBOPTIMAL_KHR）时要不要重建交换链。
// 安卓上交换链按不旋转建（preTransform = IDENTITY，由系统的显示引擎去转），横屏时表面的当前方向是转过的，于是**每一次**呈现都回「次优」。
// 原先见「次优」就标过期、下一帧整条重建——重建前还要等在途的段全做完——等于每帧排空一次显卡再建一次交换链
// （真机：一局 7800 帧里「建交换链」7579 次；游戏不等显卡的时候，等待原样出现在呈现里 11–14 毫秒）。
// 「次优」只在表面的方向或尺寸相对建链时真的变了才值得重建；没变就照用。每 30 次「次优」问一次表面（第一次就问）。
static int wsi_suboptimal_stale(struct nvn_gpu_surface_s *s) {
    if (s->subopt_n++ % 30) return 0;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, s->surface, &caps) != VK_SUCCESS) return 1;
    VkExtent2D ext;
    if (!nvn_gpu_ok(wsi_geometry(s, &caps, &ext, NULL))) return 1;
    return caps.currentTransform != s->built_transform || ext.width != s->extent.width || ext.height != s->extent.height;
}
static struct nvn_gpu_surface_s *g_surface;    // 一个后端同时只接一个面

// 取图发过而没被等掉的信号量不能直接销毁（VUID-vkDestroySemaphore-semaphore-05149）：提交一个只等它的空批次把它消费掉
static void wsi_consume_acquired(struct nvn_gpu_surface_s *s) {
    for (uint32_t i = 0; i < s->nimages + 1; i++) {
        if (!s->acquire_pending || !s->acquire || !s->acquire_pending[i] || !s->acquire[i]) continue;
        VkFenceCreateInfo fi; memset(&fi, 0, sizeof fi); fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence f = VK_NULL_HANDLE;
        if (vkCreateFence(g.d, &fi, NULL, &f) != VK_SUCCESS) continue;
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si; memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s->acquire[i]; si.pWaitDstStageMask = &st;
        pthread_mutex_lock(&g_submit_mu);
        VkResult r = vkQueueSubmit(g.queue, 1, &si, f);
        pthread_mutex_unlock(&g_submit_mu);
        if (r == VK_SUCCESS) s2a_vk_wait_fences(g.d, 1, &f, 2000000000ull);
        vkDestroyFence(g.d, f, NULL);
    }
    if (s->acquire_pending) memset(s->acquire_pending, 0, (size_t)s->nimages + 1);
    // 最后一次呈现对「呈现信号量」的等待要到队列空了才算结束：之后才能销毁那把信号量
    pthread_mutex_lock(&g_submit_mu);
    vkQueueWaitIdle(g.queue);
    pthread_mutex_unlock(&g_submit_mu);
}

static void wsi_destroy_swapchain(struct nvn_gpu_surface_s *s) {
    wsi_consume_acquired(s);
    // 所有呈现段已排空；独立显存不经过贴图对象的 g_mu（close 调用者已持这把锁）。
    if (s->encoded) vkDestroyImage(g.d, s->encoded, NULL);
    if (s->encoded_mem) vkFreeMemory(g.d, s->encoded_mem, NULL);
    s->encoded = VK_NULL_HANDLE; s->encoded_mem = VK_NULL_HANDLE;
    // 先销毁交换链再销毁信号量：取图发过、还没被呈现段等掉的信号量，要等它所属的交换链没了才算用完
    if (s->swapchain) { vkDestroySwapchainKHR(g.d, s->swapchain, NULL); s->swapchain = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < s->nimages + 1; i++) if (s->acquire && s->acquire[i]) { vkDestroySemaphore(g.d, s->acquire[i], NULL); s->acquire[i] = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < s->nimages; i++) if (s->rendered && s->rendered[i]) { vkDestroySemaphore(g.d, s->rendered[i], NULL); s->rendered[i] = VK_NULL_HANDLE; }
    free(s->images); free(s->acquire); free(s->rendered); free(s->acquire_pending); free(s->acquire_waiter);
    s->images = NULL; s->acquire = NULL; s->rendered = NULL; s->acquire_pending = NULL; s->acquire_waiter = NULL;
    s->nimages = 0; s->held = -1; s->acquire_next = 0;
}

// 取图前：这把信号量上一次被哪个呈现段等，那一段的栅栏要先被 worker 等到（g.wsi_done 追上它的序号）才能再交给取图。
// 「轮着用 nimages + 1 把」只保证 GPU 上大概率早做完了，不保证主机已经看到那次等待完成：worker 落后（CPU 忙、
// 前面有慢段）时第 nimages + 2 次取图就会把还挂着等待的信号量交给驱动（VUID-vkAcquireNextImageKHR-semaphore-01779，
// 规范上的未定义行为，不只是校验层的说法）。正常节拍下 worker 早就等完了，这里只是查一次数；落后时有界地等它。
// 0 = 可以用；-1 = 到时限还没等到（按「没有空闲的交换链图」处理，这一帧不取图）。
static int wsi_wait_acquire_free(uint64_t seq, uint64_t timeout_ns) {
    if (!seq) return 0;
    uint64_t deadline = add_sat(now_ns(), timeout_ns);
    int ok = 1;
    pthread_mutex_lock(&g_mu);
    while (g.wsi_done < seq && !g.device_lost && !g.stop) {
        uint64_t now = now_ns();
        if (now >= deadline) { ok = 0; break; }
        struct timespec ts; deadline_after(&ts, deadline - now);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    if (g.wsi_done < seq && (g.device_lost || g.stop)) ok = 0;
    pthread_mutex_unlock(&g_mu);
    return ok ? 0 : -1;
}

// 等内部段与游戏段都执行完：之后销毁交换链 / 信号量才合法。0 = 等到了。
static int wsi_wait_drained(uint64_t timeout_ns) {
    uint64_t deadline = add_sat(now_ns(), timeout_ns);
    int ok = 1;
    pthread_mutex_lock(&g_mu);
    while (g.qhead || g.busy) {
        uint64_t now = now_ns();
        if (now >= deadline) { ok = 0; break; }
        struct timespec ts; deadline_after(&ts, deadline - now);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    return ok ? 0 : -1;
}

static void wsi_close_locked(void) {   // nvn_gpu_close 在在途段清空后调（持 g_mu）
    struct nvn_gpu_surface_s *s = g_surface;
    if (!s) return;
    wsi_destroy_swapchain(s);
    if (s->surface) vkDestroySurfaceKHR(g.dev->instance, s->surface, NULL);
    g_surface = NULL;
    free(s);
}

static nvn_gpu_rc wsi_build(struct nvn_gpu_surface_s *s, nvn_gpu_err *err) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, s->surface, &caps) != VK_SUCCESS) return fail(err, NVN_GPU_E_REVOKED, "取不到表面能力（窗口已不在）");
    VkSurfaceFormatKHR fmts[64]; uint32_t nf = 64;
    VkResult fr = vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, s->surface, &nf, fmts);
    if ((fr != VK_SUCCESS && fr != VK_INCOMPLETE) || !nf) return fail(err, NVN_GPU_E_UNSUPPORTED, "表面没有可用格式");
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nf; i++) if (fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM)) { pick = fmts[i]; break; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g.phys, pick.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "交换链格式 %d 不能当拷贝目标", (int)pick.format);
    VkExtent2D ext;
    nvn_gpu_rc geometry = wsi_geometry(s, &caps, &ext, err);
    if (!nvn_gpu_ok(geometry)) return geometry;
    uint32_t n = caps.minImageCount == UINT32_MAX ? UINT32_MAX : caps.minImageCount + 1; if (n < 3) n = 3;
    { int extra = nvn_plat_flag("nvk_swap_extra", 0); if (extra > 0 && extra <= 4 && n <= UINT32_MAX - (uint32_t)extra) n += (uint32_t)extra;   // 诊断：多要几张窗口图，看取图的等待是不是交换链反压
      if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
      char b[160]; snprintf(b, sizeof b, "交换链：驱动最少 %u 张、最多 %u 张，要 %u 张", caps.minImageCount, caps.maxImageCount, n); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = s->surface; ci.minImageCount = n; ci.imageFormat = pick.format; ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = ext; ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // 预旋转固定用恒等：把当前旋转当预旋转又不转内容，合成器会再转一次（安卓真机实测过的坑）
    ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                      : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.clipped = VK_TRUE;
    ci.oldSwapchain = s->swapchain;
    VkSwapchainKHR nsc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(g.d, &ci, NULL, &nsc);
    // vkCreateSwapchainKHR 带 oldSwapchain 时旧链即退休（失败也一样）；此前已排空，连同旧数组一起回收。
    if (s->swapchain || s->nimages) wsi_destroy_swapchain(s);
    if (r != VK_SUCCESS) return fail(err, r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR ? NVN_GPU_E_REVOKED : NVN_GPU_E_UNSUPPORTED, "vkCreateSwapchainKHR 失败（%d）", (int)r);
    s->swapchain = nsc; s->format = pick.format; s->color_space = pick.colorSpace; s->extent = ext;
    // minImageCount 是下限，不是返回张数。先问实际张数；枚举期间变多则重新问，绝不接受部分句柄。
    VkResult ir = VK_INCOMPLETE;
    uint32_t ni = 0;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        ni = 0;
        ir = vkGetSwapchainImagesKHR(g.d, nsc, &ni, NULL);
        if (ir != VK_SUCCESS || !ni || ni == UINT32_MAX) break; // acquire 另需一槽
        VkImage *images = calloc(ni, sizeof *images);
        if (!images) { wsi_destroy_swapchain(s); return fail(err, NVN_GPU_E_OOM, "交换链图数组分配失败"); }
        uint32_t capacity = ni;
        ir = vkGetSwapchainImagesKHR(g.d, nsc, &ni, images);
        if (ir == VK_SUCCESS && ni && ni <= capacity) { s->images = images; break; }
        free(images);
        if (ir != VK_INCOMPLETE) break;
    }
    if (!s->images) { wsi_destroy_swapchain(s); return fail(err, NVN_GPU_E_UNSUPPORTED, "取不到完整交换链图（%d）", (int)ir); }
    s->nimages = ni;
    s->acquire = calloc((size_t)ni + 1, sizeof *s->acquire);
    s->rendered = calloc(ni, sizeof *s->rendered);
    s->acquire_pending = calloc((size_t)ni + 1, sizeof *s->acquire_pending);
    s->acquire_waiter = calloc((size_t)ni + 1, sizeof *s->acquire_waiter);
    if (!s->acquire || !s->rendered || !s->acquire_pending || !s->acquire_waiter) { wsi_destroy_swapchain(s); return fail(err, NVN_GPU_E_OOM, "交换链信号量数组分配失败"); }
    VkSemaphoreCreateInfo si; memset(&si, 0, sizeof si); si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < ni + 1; i++) if (!s->acquire[i] && vkCreateSemaphore(g.d, &si, NULL, &s->acquire[i]) != VK_SUCCESS) { wsi_destroy_swapchain(s); return fail(err, NVN_GPU_E_OOM, "信号量创建失败"); }
    for (uint32_t i = 0; i < ni; i++) if (!s->rendered[i] && vkCreateSemaphore(g.d, &si, NULL, &s->rendered[i]) != VK_SUCCESS) { wsi_destroy_swapchain(s); return fail(err, NVN_GPU_E_OOM, "信号量创建失败"); }
    s->held = -1; s->outdated = 0; s->acquire_next = 0;
    s->built_transform = caps.currentTransform; s->subopt_n = 0;
    wsi_timing_init(&s->timing, g.d, nvn_plat_flag("display_timing", 0) == 1, g.dev->has_display_timing);
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_surface_attach(void *native, nvn_gpu_surface *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open || !session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_surface) return fail(err, NVN_GPU_E_ORDER, "已经接着一个面");
    if (!native) return fail(err, NVN_GPU_E_ARG, "窗口为空");
    struct nvn_gpu_surface_s *s = calloc(1, sizeof *s);
    if (!s) return fail(err, NVN_GPU_E_OOM, "面结构分配失败");
    s->native = native; s->held = -1;
    VkResult r = nvn_plat_vk_surface_create(g.dev->instance, native, &s->surface);
    if (r != VK_SUCCESS || !s->surface) { free(s); return fail(err, NVN_GPU_E_UNSUPPORTED, "平台建表面失败（%d）", (int)r); }
    VkBool32 can = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.dev->queue_family, s->surface, &can) != VK_SUCCESS || !can) {
        vkDestroySurfaceKHR(g.dev->instance, s->surface, NULL); free(s);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "图形队列族不能向这个表面呈现");
    }
    nvn_gpu_rc b = wsi_build(s, err);
    if (!nvn_gpu_ok(b)) { vkDestroySurfaceKHR(g.dev->instance, s->surface, NULL); free(s); return b; }
    s->gen = 1;
    g_surface = s; *out = s;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_surface_rebuild(nvn_gpu_surface s, nvn_gpu_err *err) {
    if (!s || !s->gen) return fail(err, NVN_GPU_E_REVOKED, "面已撤销");
    if (wsi_wait_drained(2000000000ull)) return fail(err, NVN_GPU_E_TIMEOUT, "在途的段没有完成，不能换交换链");
    uint64_t gen = s->gen;
    // 先消费旧取图信号；wsi_build 建新链后回收旧链与数组，新链使用全新的信号量。
    wsi_consume_acquired(s);
    s->outdated = 1;
    nvn_gpu_rc b = wsi_build(s, err);
    if (!nvn_gpu_ok(b)) { if (b.code == NVN_GPU_E_REVOKED) s->gen = 0; return b; }
    s->gen = gen + 1;
    return rc(NVN_GPU_OK);
}

uint64_t nvn_gpu_surface_gen(nvn_gpu_surface s) { return s ? s->gen : 0; }

nvn_gpu_rc nvn_gpu_surface_detach(nvn_gpu_surface s, uint64_t timeout_ns, nvn_gpu_err *err) {
    if (!s) return rc(NVN_GPU_OK);
    if (wsi_wait_drained(timeout_ns)) return fail(err, NVN_GPU_E_TIMEOUT, "最后一帧的呈现段没有完成证明");
    wsi_destroy_swapchain(s);
    if (s->surface) vkDestroySurfaceKHR(g.dev->instance, s->surface, NULL);
    if (g_surface == s) g_surface = NULL;
    s->gen = 0;
    free(s);
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_drawable_acquire(nvn_gpu_surface s, uint64_t timeout_ns, void *plat_frame, nvn_gpu_drawable *out, nvn_gpu_err *err) {
    (void)plat_frame;
    if (out) memset(out, 0, sizeof *out);
    if (!s || !out) return fail(err, NVN_GPU_E_ARG, "参数为空");
    if (!s->gen) return fail(err, NVN_GPU_E_REVOKED, "面已撤销");
    if (s->outdated || !s->swapchain) return fail(err, NVN_GPU_E_OUTDATED, "交换链已过期，先重建");
    uint32_t idx, sem;
    if (s->held >= 0) { idx = (uint32_t)s->held; sem = s->held_sem; s->held = -1; }   // 上次取了没呈现的那张
    else {
        sem = s->acquire_next;
        if (sem > s->nimages) return fail(err, NVN_GPU_E_ORDER, "取图信号量下标越界");
        struct timespec q0, q1; clock_gettime(CLOCK_MONOTONIC, &q0);
        if (wsi_wait_acquire_free(s->acquire_waiter[sem], timeout_ns)) return fail(err, NVN_GPU_E_TIMEOUT, "上一次等这把取图信号量的呈现段还没确认完成");
        VkResult r = vkAcquireNextImageKHR(g.d, s->swapchain, timeout_ns, s->acquire[sem], VK_NULL_HANDLE, &idx);
        clock_gettime(CLOCK_MONOTONIC, &q1); __atomic_fetch_add(&g_acq_ns, (uint64_t)(q1.tv_sec - q0.tv_sec) * 1000000000ull + (uint64_t)q1.tv_nsec - (uint64_t)q0.tv_nsec, __ATOMIC_RELAXED);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) return fail(err, NVN_GPU_E_TIMEOUT, "没有空闲的交换链图");
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { s->outdated = 1; return fail(err, NVN_GPU_E_OUTDATED, "交换链已过期"); }
        if (r == VK_ERROR_SURFACE_LOST_KHR) { s->gen = 0; return fail(err, NVN_GPU_E_REVOKED, "表面已丢失"); }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return fail(err, r == VK_ERROR_DEVICE_LOST ? NVN_GPU_E_DEVICE : NVN_GPU_E_UNSUPPORTED, "vkAcquireNextImageKHR 失败（%d）", (int)r);
        if (r == VK_SUBOPTIMAL_KHR && wsi_suboptimal_stale(s)) s->outdated = 2;   // 表面真变了：这张还能用，呈现之后要重建
        s->acquire_pending[sem] = 1;
        s->acquire_next = (s->acquire_next + 1) % (s->nimages + 1);
    }
    if (idx >= s->nimages || sem > s->nimages) {
        // 成功取图已发信号，保留 pending 让重建 / 拆除消费；不把坏下标交给 GPU。
        s->outdated = 1;
        return fail(err, NVN_GPU_E_OUTDATED, "交换链取图下标越界（%u / %u）", idx, s->nimages);
    }
    out->view = NULL; out->w = s->extent.width; out->h = s->extent.height; out->nvn_format = 0; out->gen = s->gen;
    out->token = (uint64_t)idx | ((uint64_t)sem + 1) << 32;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_drawable_release(nvn_gpu_surface s, const nvn_gpu_drawable *dr) {
    if (!s || !dr || !(dr->token >> 32) || dr->gen != s->gen) return;
    uint32_t idx = (uint32_t)dr->token, sem = (uint32_t)(dr->token >> 32) - 1;
    if (idx >= s->nimages || sem > s->nimages) return;
    s->held = idx; s->held_sem = sem;   // 留作下次取图的结果
}

static void wsi_swap_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b; memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = sa; b.dstAccessMask = da;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

// vkCmdBlitImage 会解码 sRGB 源。非线性 UNORM 交换链要求编码后的字节，直接 blit 会把线性值交给显示器。
// 在相同通道布局的 sRGB 中间图上缩放（解码、过滤、重新编码），再 copy 原始字节；UNORM 源保持原路径。
static nvn_gpu_rc wsi_encoded_target(struct nvn_gpu_surface_s *s, nvn_gpu_err *err) {
    if (s->encoded) return rc(NVN_GPU_OK);
    VkFormat format = s->format == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_SRGB;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g.phys, format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "上屏编码中间格式 %d 不支持缩放目标", (int)format);
    VkImageCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
    ci.extent = (VkExtent3D){s->extent.width, s->extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(g.d, &ci, NULL, &image);
    if (r != VK_SUCCESS) return fail(err, NVN_GPU_E_OOM, "上屏编码中间图创建失败（%d）", (int)r);
    VkMemoryRequirements req; vkGetImageMemoryRequirements(g.d, image, &req);
    uint32_t type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, 0, NULL);
    VkMemoryAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    r = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(g.d, &ai, NULL, &memory);
    if (r == VK_SUCCESS) r = vkBindImageMemory(g.d, image, memory, 0);
    if (r != VK_SUCCESS) {
        vkDestroyImage(g.d, image, NULL); if (memory) vkFreeMemory(g.d, memory, NULL);
        return fail(err, NVN_GPU_E_OOM, "上屏编码中间显存绑定失败（%d）", (int)r);
    }
    s->encoded = image; s->encoded_mem = memory;
    return rc(NVN_GPU_OK);
}

// 合成一段：把源（crop 矩形）缩放到交换链图（dst 矩形，flip_y 可上下翻），末态转 PRESENT_SRC。
// 返回 0 = 矩形越界。encode 时 target 是 sRGB 中间图，末段逐字节拷回交换链图。
static int wsi_compose_blit(struct nvn_gpu_cmd_s *c, struct nvn_gpu_surface_s *s, uint32_t idx,
                            struct nvn_gpu_tex_s *st, const nvn_gpu_present_desc *d,
                            const VkFormatProperties *fp, int encode, VkImage target) {
    cmd_ref(c, d->src, REF_VIEW);   // 源视图活到呈现段完成（它同时让贴图活着）：后端记录用的是 image，调用方录完就 release 视图也不能把 image 提前拆掉
    image_barrier(c, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    wsi_swap_barrier(c->cb, s->images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // 下一帧覆盖中间图前，等此前呈现段的 copy 读完；UNDEFINED 仅丢内容，仍需读→写依赖。
    if (encode) wsi_swap_barrier(c->cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black; memset(&black, 0, sizeof black); black.float32[3] = 1.0f;
    { static int bars = -1; if (bars < 0) bars = nvn_plat_flag("nvk_bars", 0);   // 调试：黑边改成品红，截图上一眼分清「没呈现」和「呈现了一张黑图」
      if (bars) { black.float32[0] = 1.0f; black.float32[2] = 1.0f; } }
    VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c->cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);   // 目标矩形外是黑的
    wsi_swap_barrier(c->cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);   // 清与贴是对同一张图的两次写：之间要有屏障
    // 源：crop（宽高为 0 = 整个视图的首级）；目标：dst（宽高为 0 = 整张交换链图）。裁到各自范围内。
    uint32_t lw = st->w >> d->src->base_level, lh = st->h >> d->src->base_level; if (!lw) lw = 1; if (!lh) lh = 1;
    int32_t sx = d->crop[0], sy = d->crop[1], sw = d->crop[2], sh = d->crop[3];
    if (sw <= 0 || sh <= 0) { sx = 0; sy = 0; sw = (int32_t)lw; sh = (int32_t)lh; }
    int32_t dx = d->dst[0], dy = d->dst[1], dw = d->dst[2], dh = d->dst[3];
    if (dw <= 0 || dh <= 0) { dx = 0; dy = 0; dw = (int32_t)s->extent.width; dh = (int32_t)s->extent.height; }
    int ok = sx >= 0 && sy >= 0 && (uint32_t)(sx + sw) <= lw && (uint32_t)(sy + sh) <= lh && dx >= 0 && dy >= 0 && (uint32_t)(dx + dw) <= s->extent.width && (uint32_t)(dy + dh) <= s->extent.height;
    if (ok) {
        VkImageBlit bl; memset(&bl, 0, sizeof bl);
        bl.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, d->src->base_level, d->src->base_layer, 1};
        bl.srcOffsets[0] = (VkOffset3D){sx, d->flip_y ? sy + sh : sy, 0}; bl.srcOffsets[1] = (VkOffset3D){sx + sw, d->flip_y ? sy : sy + sh, 1};   // flip_y：两个 y 对调，贴出来上下翻转
        bl.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[0] = (VkOffset3D){dx, dy, 0}; bl.dstOffsets[1] = (VkOffset3D){dx + dw, dy + dh, 1};
        VkFilter filt = d->filter && (fp->optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        vkCmdBlitImage(c->cb, st->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, filt);
    }
    if (encode) {
        wsi_swap_barrier(c->cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy = {0};
        copy.srcSubresource = copy.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = (VkExtent3D){s->extent.width, s->extent.height, 1};
        vkCmdCopyImage(c->cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    wsi_swap_barrier(c->cb, s->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    image_barrier(c, st, VK_IMAGE_LAYOUT_GENERAL);   // 源图转回常驻布局（见 backend_vulkan.c「图像布局」：别的段一律按 GENERAL 用它）
    return ok;
}

// 可选的上屏 EASU 放大：直接画进交换链图（它本来就带 COLOR_ATTACHMENT 用法）。
// 返回 1 = 已录制；0 = 不适用（调用方回落 wsi_compose_blit；此处已下发的 UNDEFINED 屏障对后者无害）。
static int wsi_compose_easu(struct nvn_gpu_cmd_s *c, struct nvn_gpu_surface_s *s, uint32_t idx,
                            const nvn_gpu_present_desc *d, int encode) {
    // 整张图先清黑：渲染通道的 CLEAR 只作用于渲染区，dst 之外的边框得自己清（与 blit 路径同一个黑）。
    wsi_swap_barrier(c->cb, s->images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black; memset(&black, 0, sizeof black); black.float32[3] = 1.0f;
    if (nvn_plat_flag("nvk_bars", 0)) { black.float32[0] = 1.0f; black.float32[2] = 1.0f; }
    VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c->cb, s->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    wsi_swap_barrier(c->cb, s->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (!easu_compose(c, s->images[idx], s->extent, 0, s->format, d->src, d->crop, d->dst, d->flip_y, s->gen, encode))
        return 0;
    wsi_swap_barrier(c->cb, s->images[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    return 1;
}

nvn_gpu_rc nvn_gpu_present(nvn_gpu_surface s, const nvn_gpu_drawable *dr, const nvn_gpu_present_desc *d, uint64_t ticket,
                           nvn_gpu_presented_fn presented, void *user, nvn_gpu_err *err) {
    nvn_gpu_err local; if (!err) err = &local;
    g_att_probe = 0;   // 首帧附件体检（backend_vulkan.c pass_begin）：第一帧上屏后不再打
    nvn_gpu_rc out = rc(NVN_GPU_OK);
    nvn_gpu_shown shown = {ticket, NVN_GPU_SHOWN_DROPPED, 0.0};
    nvn_gpu_cmd ch = NULL;
    { static uint64_t np; if (np % 300 == 1) g_draw_tm = nvn_plat_flag("nvk_gputime", 0) != 0;
      if (++np % 300 == 0 && g_draw_tm) {   // 诊断读数：见 g_oc
        uint64_t v[OC_N]; for (int i = 0; i < OC_N; i++) v[i] = __atomic_exchange_n(&g_oc[i], 0, __ATOMIC_RELAXED);
        char b[400]; snprintf(b, sizeof b, "对象读数（每帧，近 300 帧）：新建渲染通道 %.1f、帧缓冲 %.1f、命令池 %.1f、描述符池 %.1f；分配描述符集 %.1f 次、写描述符 %.1f 次；遮挡查询 %.1f 个；新建视图 %.1f 个；取窗口图 %.1f 毫秒",
            v[OC_RP] / 300.0, v[OC_FB] / 300.0, v[OC_CPOOL] / 300.0, v[OC_DPOOL] / 300.0, v[OC_DALLOC] / 300.0, v[OC_DWRITE] / 300.0, v[OC_QUERY] / 300.0, v[OC_VIEW] / 300.0, (double)__atomic_exchange_n(&g_acq_ns, 0, __ATOMIC_RELAXED) / 300.0 / 1e6);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b);
        { static uint64_t last_feedback[3];
          uint64_t ro = diag_delta(&g_feedback_stats[0], &last_feedback[0]);
          uint64_t cp = diag_delta(&g_feedback_stats[1], &last_feedback[1]);
          uint64_t split = diag_delta(&g_feedback_stats[2], &last_feedback[2]);
          snprintf(b, sizeof b, "只读反馈（每帧，近 300 帧，录制计数）：只读采样 %.2f 笔、快照拷贝 %.2f 次、重开 %.2f 次；nvk_fb_read=%d none_ext=%d",
              ro / 300.0, cp / 300.0, split / 300.0, nvn_plat_flag("nvk_fb_read", 0), g.dev->load_store_op_none_enabled);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b);
        }
        // 留存读数（#1118）：视图表按存储认视图后，存储在共用表里留着时名下的视图与帧缓冲也留着；看它们在一局里是否有界（取这一刻的现存数，不是窗口平均）
        snprintf(b, sizeof b, "留存读数（取这一刻）：现存后端视图 %llu 个、帧缓冲缓存 %llu / %u 项", (unsigned long long)nvn_vk_dbg_view_stat(0), (unsigned long long)nvn_vk_dbg_view_stat(2), (unsigned)PC_FB_N);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b);
        // 驱动调用数（每帧）：经过两级回放包装的各类 vkCmd*，按次数从多到少列前 10 类和总数
        { uint64_t c[OP_N], tot = 0; int idx[OP_N];
          for (int i = 0; i < OP_N; i++) { c[i] = __atomic_exchange_n(&g_dq_cnt[i], 0, __ATOMIC_RELAXED); tot += c[i]; idx[i] = i; }
          for (int i = 1; i < OP_N; i++) { int k = idx[i], j = i - 1; while (j >= 0 && c[idx[j]] < c[k]) { idx[j + 1] = idx[j]; j--; } idx[j + 1] = k; }
          size_t o = (size_t)snprintf(b, sizeof b, "驱动调用（每帧，近 300 帧）：共 %.0f 次", tot / 300.0);
          for (int i = 0; i < 10 && i < OP_N && c[idx[i]] && o < sizeof b - 48; i++) if (idx[i] != OP_WRAP) o += (size_t)snprintf(b + o, sizeof b - o, "；%s %.1f", g_dq_names[idx[i]] + (strncmp(g_dq_names[idx[i]], "vkCmd", 5) ? 0 : 5), c[idx[i]] / 300.0);   // 去掉 vkCmd 前缀（暂存填充 fill 没有）
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        if (g_dq_ring && DQ_ON()) {   // 两级回放开着才有这一行：录制线程怎么等活、生产者叫了几次、清空等了多久（nvk_dq_spin / nvk_dq_poll_us / nvk_dq_wake_kb 的 A/B 读数）
          uint64_t s[DQS_N]; for (int i = 0; i < DQS_N; i++) s[i] = __atomic_exchange_n(&g_dq_st[i], 0, __ATOMIC_RELAXED);
          snprintf(b, sizeof b, "两级回放等活读数（每帧，近 300 帧）：录制线程进睡眠 %.1f 次（其中定时醒 %.1f 次、醒来有活 %.1f 次）；生产者真去叫醒 %.1f 次；清空要等 %.1f 次、共 %.0f 微秒",
              s[DQS_SLEEP] / 300.0, s[DQS_TMO] / 300.0, s[DQS_TMO_WORK] / 300.0, s[DQS_WAKE] / 300.0, s[DQS_DRAIN_WAIT] / 300.0, s[DQS_DRAIN_NS] / 300.0 / 1000.0);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b);
          dq_lat_line(b, sizeof b); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        { static uint64_t l_draw[3];   // 这个读者上次读到的累加器值（不清零共享累加器，见 diag_delta：每 5 秒的逐笔读数也读它们）
          uint64_t d0 = diag_delta(&g_draw_ns[0], &l_draw[0]), d1 = diag_delta(&g_draw_ns[1], &l_draw[1]), d2 = diag_delta(&g_draw_ns[2], &l_draw[2]);
          snprintf(b, sizeof b, "后端绘制细分（每帧毫秒，近 300 帧）：描述符 %.2f、推常量与状态 %.2f、顶点索引与绘制调用 %.2f", d0 / 300.0 / 1e6, d1 / 300.0 / 1e6, d2 / 300.0 / 1e6);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        { static uint64_t l_nw, l_sets, l_hit, l_ns[4];
          uint64_t v4[4], nwt = diag_delta(&g_desc_nw, &l_nw), nst = diag_delta(&g_desc_sets, &l_sets);
          for (int i = 0; i < 4; i++) v4[i] = diag_delta(&g_desc_ns[i], &l_ns[i]);
          snprintf(b, sizeof b, "描述符细分（每帧毫秒，近 300 帧）：分配 %.2f、构造写入项 %.2f、写入 %.2f、绑定 %.2f；每帧重写 %.0f 组（其中备忘命中 %.0f 组）、写 %.0f 条描述符",
              v4[0] / 300.0 / 1e6, v4[1] / 300.0 / 1e6, v4[2] / 300.0 / 1e6, v4[3] / 300.0 / 1e6, nst / 300.0, diag_delta(&g_desc_hit, &l_hit) / 300.0, nwt / 300.0);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        { static uint64_t l_rb; const double rb = (double)diag_delta(&g_dyn_rebind, &l_rb) / 300.0;   // 动态 UBO 偏移（#1233）读数
          snprintf(b, sizeof b, "动态 UBO 偏移（每帧，近 300 帧）：只换偏移、不重写组、只重绑 %.0f 次；用动态偏移的管线 %llu 条（建过带 UBO 的管线 %llu 条）；设备动态 UBO 上限 %u；nvk_desc_memo=%d",
              rb, (unsigned long long)__atomic_load_n(&g_dyn_pipes[0], __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_dyn_pipes[1], __ATOMIC_RELAXED), g_max_dyn_ubo, desc_memo_flag());
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        { static uint64_t lnew, lreuse, lput, lfreed, lcap, lbusy, lsig;   // 描述符组按布局回收（#1096）：计数开会话起累计，这里按两次打印之差算每帧
          pthread_mutex_lock(&g_dset_mu);
          const uint64_t n1 = g_dset_new, r1 = g_dset_reuse, p1 = g_dset_put, f1 = g_dset_freed, s1 = g_dset_sigmiss, c1 = g_dset_capped; const uint32_t np1 = g_dset_npools, nb1 = g_dset_nbins, cap1 = g_dset_cap;
          pthread_mutex_unlock(&g_dset_mu);
          const uint64_t b1 = __atomic_load_n(&g_dset_busy, __ATOMIC_RELAXED), idle1 = dset_idle_total();
          pthread_mutex_lock(&g_dlay_mu); const uint32_t nl1 = g_dlay_n; const uint64_t la1 = g_dlay_ask, lh1 = g_dlay_hit; pthread_mutex_unlock(&g_dlay_mu);
          if (n1 < lnew || r1 < lreuse || p1 < lput || f1 < lfreed || c1 < lcap || b1 < lbusy || s1 < lsig) lnew = lreuse = lput = lfreed = lcap = lbusy = lsig = 0;   // 新会话：计数从 0 重来
          snprintf(b, sizeof b, "描述符组回收（每帧，近 300 帧）：复用 %.1f、新分 %.1f、放回 %.1f、放弃还池 %.1f、碰锁退回 %.2f 次、写法不同退回 %.2f 次、到上限退回 %.2f 次；空闲 %llu 组、在用加空闲共 %llu 组（上限 %u）、增长池 %u 个、表项 %u 个；布局结构 %u 种（建管线取布局 %llu 次、命中 %llu 次）；nvk_dset_rec=%d",
              (double)(r1 - lreuse) / 300.0, (double)(n1 - lnew) / 300.0, (double)(p1 - lput) / 300.0, (double)(f1 - lfreed) / 300.0, (double)(b1 - lbusy) / 300.0, (double)(s1 - lsig) / 300.0, (double)(c1 - lcap) / 300.0,
              (unsigned long long)idle1, (unsigned long long)(n1 - f1), cap1, np1, nb1, nl1, (unsigned long long)la1, (unsigned long long)lh1, __atomic_load_n(&g_dset_on, __ATOMIC_RELAXED));
          lnew = n1; lreuse = r1; lput = p1; lfreed = f1; lcap = c1; lbusy = b1; lsig = s1;
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        { static const char *const nm[8] = {"建图", "要显存并绑定", "默认视图", "帧缓冲", "附件视图", "拆视图", "从帧缓冲表里摘", "拆贴图"}; int o = snprintf(b, sizeof b, "建拆细分（每帧毫秒 / 次，近 300 帧）：");
          for (int i = 0; i < 8 && o < (int)sizeof b - 40; i++) o += snprintf(b + o, sizeof b - (size_t)o, "%s%s %.2f / %.1f", i ? "、" : "", nm[i], __atomic_exchange_n(&g_life_ns[i], 0, __ATOMIC_RELAXED) / 300.0 / 1e6, __atomic_exchange_n(&g_life_n[i], 0, __ATOMIC_RELAXED) / 300.0);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b);
          pthread_mutex_lock(&g_texmem_mu); uint64_t th = g_texmem_hit, tm_ = g_texmem_miss; g_texmem_hit = g_texmem_miss = 0; uint32_t tn = g_texmem_n; uint64_t tb = g_texmem_bytes; pthread_mutex_unlock(&g_texmem_mu);
          snprintf(b, sizeof b, "贴图显存池（每帧，近 300 帧）：复用 %.1f 次、向驱动要 %.1f 次；池里现有 %u 块 %llu MB", th / 300.0, tm_ / 300.0, tn, (unsigned long long)(tb >> 20));
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        { uint64_t ns = __atomic_exchange_n(&g_pl_ns, 0, __ATOMIC_RELAXED), n = __atomic_exchange_n(&g_pl_n, 0, __ATOMIC_RELAXED), st = __atomic_exchange_n(&g_pl_steps, 0, __ATOMIC_RELAXED), cm = __atomic_exchange_n(&g_pl_cmp, 0, __ATOMIC_RELAXED);
          pthread_mutex_lock(&g_mu); const unsigned htn = g_pipe_ht_n, htc = g_pipe_cnt; pthread_mutex_unlock(&g_mu);
          snprintf(b, sizeof b, "取管线查找（每帧，近 300 帧）：%.1f 次、共 %.0f 微秒、每次走 %.1f 条（其中逐字段比 %.2f 条）；散列桶 %u 个、图形管线 %u 条",
              n / 300.0, ns / 300.0 / 1000.0, n ? (double)st / (double)n : 0.0, n ? (double)cm / (double)n : 0.0, htn, htc);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        { uint64_t dn = __atomic_exchange_n(&g_dv_n, 0, __ATOMIC_RELAXED), dt = __atomic_exchange_n(&g_dv_ns[0], 0, __ATOMIC_RELAXED), dl = __atomic_exchange_n(&g_dv_ns[1], 0, __ATOMIC_RELAXED);
          uint64_t gn = __atomic_exchange_n(&g_gmu_n, 0, __ATOMIC_RELAXED), gw = __atomic_exchange_n(&g_gmu_wait_n, 0, __ATOMIC_RELAXED), gwn = __atomic_exchange_n(&g_gmu_wait_ns, 0, __ATOMIC_RELAXED);
          uint64_t rn = __atomic_exchange_n(&g_ref_n, 0, __ATOMIC_RELAXED), rh = __atomic_exchange_n(&g_ref_hit, 0, __ATOMIC_RELAXED);
          snprintf(b, sizeof b, "视图销毁与锁（每帧，近 300 帧）：销毁 %.1f 个、共 %.2f 毫秒（其中摘链 %.3f 毫秒）；回放线程取 g_mu %.0f 次、被占着要等的 %.1f 次、共等 %.2f 毫秒；引用句柄 %.0f 次、其中备忘命中 %.0f 次；开关 nvk_ulist=%d nvk_fbdrop=%d",
              dn / 300.0, dt / 300.0 / 1e6, dl / 300.0 / 1e6, gn / 300.0, gw / 300.0, gwn / 300.0 / 1e6, rn / 300.0, rh / 300.0, view_ulist_on(), fbdrop_mode());
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); }
        // 后端现存对象（显存涨在哪）。悬着 = 上层已放、还被引用着没销毁
        { uint64_t nt = 0, ntd = 0, tb = 0, tdb = 0, nb = 0, nbd = 0, bb = 0, nvw = 0, nq = 0;
          pthread_mutex_lock(&g_mu);
          for (struct nvn_gpu_tex_s *t = g.texs; t; t = t->all_next) { uint64_t sz = t->mem_size; nt++; tb += sz; if (t->released) { ntd++; tdb += sz; } }   // 按向驱动要的显存大小算（原先按 宽×高×深×层×4 估：数组贴图的层数乘了两遍、压缩格式也按每像素 4 字节，外部一台机器上读出 60 GB）
          for (struct nvn_gpu_buf_s *x = g.bufs; x; x = x->all_next) { nb++; bb += x->bytes; if (x->released) nbd++; }
          for (struct nvn_gpu_view_s *x = g.views; x; x = x->all_next) nvw++;
          for (struct vk_segment *q = g.qhead; q; q = q->next) nq++;
          pthread_mutex_unlock(&g_mu);
          snprintf(b, sizeof b, "现存对象：贴图 %llu 张 %llu MB（其中悬着 %llu 张 %llu MB）；缓冲 %llu 个 %llu MB（悬着 %llu 个）；视图 %llu 个；在途段 %llu 个",
              (unsigned long long)nt, (unsigned long long)(tb >> 20), (unsigned long long)ntd, (unsigned long long)(tdb >> 20), (unsigned long long)nb, (unsigned long long)(bb >> 20), (unsigned long long)nbd, (unsigned long long)nvw, (unsigned long long)nq);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); } } }
    if (!s || !dr || !d || !d->src || !d->src->tex) { out = fail(err, NVN_GPU_E_ARG, "present 参数为空"); goto done; }
    if (!s->gen) { out = fail(err, NVN_GPU_E_REVOKED, "面已撤销"); goto done; }
    if (dr->gen != s->gen || !(dr->token >> 32)) { out = fail(err, NVN_GPU_E_OUTDATED, "drawable 不属于当前交换链"); goto done; }
    uint32_t idx = (uint32_t)dr->token, sem = (uint32_t)(dr->token >> 32) - 1;
    if (idx >= s->nimages || sem > s->nimages) { out = fail(err, NVN_GPU_E_ARG, "drawable 下标越界"); goto done; }
    struct nvn_gpu_tex_s *st = d->src->tex;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g.phys, st->format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || st->samples > 1) {
        s->held = idx; s->held_sem = sem;
        out = fail(err, NVN_GPU_E_UNSUPPORTED, "上屏源格式 0x%x 不能当缩放拷贝的源（或是多重采样）", st->nvn_format); goto done;
    }
    nvn_fmt_info info;
    int encode = nvn_fmt_info_of(st->nvn_format, &info) && info.srgb &&
                 s->color_space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                 (s->format == VK_FORMAT_R8G8B8A8_UNORM || s->format == VK_FORMAT_B8G8R8A8_UNORM);
    if (encode) {
        out = wsi_encoded_target(s, err);
        if (!nvn_gpu_ok(out)) { s->held = idx; s->held_sem = sem; goto done; }
    }
    VkImage target = encode ? s->encoded : s->images[idx];
    out = nvn_gpu_cmd_begin(NULL, &ch, err);
    if (!nvn_gpu_ok(out)) { s->held = idx; s->held_sem = sem; goto done; }
    struct nvn_gpu_cmd_s *c = ch;
    // 上屏 EASU 放大：玩家选「高画质」时开（S2A_HIGH_QUALITY=1，宿主创建前由外壳设，缺省更省电 = 关）；
    //   调试构建里 debug.s2a.nvk_upscale 设了就以它为准（A/B 用），发布包 nvn_plat_flag 恒取缺省 -1 → 按玩家选项
    int upscaled = 0;
    { static int hq = -1; if (hq < 0) { const char *e = getenv("S2A_HIGH_QUALITY"); hq = e && e[0] == '1'; }
      int up = nvn_plat_flag("nvk_upscale", -1); if (up < 0) up = hq;
      int elig = up ? easu_eligible(st, s->format) : 0;
      { static int seen = -1; int now = up * 2 + elig;   // 只在「开关 × 适用」变化时记一行
        if (now != seen) { seen = now; char b[160];
          snprintf(b, sizeof b, "上屏放大读数：开关=%d 适用=%d（源格式=%d 交换链=%d）", up, elig, (int)st->format, (int)s->format);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b); } }
      if (elig) { upscaled = wsi_compose_easu(c, s, idx, d, encode);
        if (!upscaled) nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", "上屏放大：这次不适用，回落缩放拷贝"); } }
    int ok = upscaled ? 1 : wsi_compose_blit(c, s, idx, st, d, &fp, encode, target);
    // 内部段：摘出活动表，自己提交（等取图信号量、发呈现信号量），交给完成线程只做回收
    pthread_mutex_lock(&g_mu);
    { struct nvn_gpu_cmd_s **pp = &g.live; while (*pp && *pp != c) pp = &(*pp)->live_next; if (*pp) *pp = c->live_next; }
    pthread_mutex_unlock(&g_mu);
    struct vk_segment *seg = calloc(1, sizeof *seg);
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi; memset(&fi, 0, sizeof fi); fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkResult r = !ok ? VK_ERROR_UNKNOWN : !seg ? VK_ERROR_OUT_OF_HOST_MEMORY : vkEndCommandBuffer(c->cb);
    if (r == VK_SUCCESS) r = vkCreateFence(g.d, &fi, NULL, &fence);
    pthread_mutex_lock(&g_submit_mu);
    if (r == VK_SUCCESS) {
        VkPipelineStageFlags wait_stage = upscaled
            ? (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)   // 先清场再画
            : VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si; memset(&si, 0, sizeof si);
        VkCommandBuffer cbs[2];
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = submit_cbs(c, cbs); si.pCommandBuffers = cbs;
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s->acquire[sem]; si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s->rendered[idx];
        r = si.commandBufferCount ? vkQueueSubmit(g.queue, 1, &si, fence) : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (r != VK_SUCCESS) {
        pthread_mutex_unlock(&g_submit_mu);
        if (fence) vkDestroyFence(g.d, fence, NULL);
        free(seg);
        finish_cmd(c, 0);   // 回滚这一段记下的布局变化
        s->held = idx; s->held_sem = sem;
        out = fail(err, !ok ? NVN_GPU_E_ARG : r == VK_ERROR_DEVICE_LOST ? NVN_GPU_E_DEVICE : NVN_GPU_E_OOM, !ok ? "上屏矩形越界" : "呈现段提交失败（%d）", (int)r);
        goto done;
    }
    s->acquire_pending[sem] = 0;   // 这把信号量已交给呈现段去等
    seg->cmd = c; seg->cb = c->cb; seg->fence = fence; seg->submitted = 1; seg->executed = 1; seg->internal = 1;
    seg->wsi_seq = s->acquire_waiter[sem] = ++g.wsi_seq;   // 持 g_submit_mu：序号与入队同序
    enqueue_segment(&g, seg);
    VkPresentInfoKHR pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s->rendered[idx];
    pi.swapchainCount = 1; pi.pSwapchains = &s->swapchain; pi.pImageIndices = &idx;
    uint32_t timing_id = wsi_timing_id(&s->timing);
    VkPresentTimeGOOGLE pt = { timing_id, 0 };
    VkPresentTimesInfoGOOGLE pts = { VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, pi.pNext, 1, &pt };
    if (timing_id) pi.pNext = &pts;
    r = vkQueuePresentKHR(g.queue, &pi);
    pthread_mutex_unlock(&g_submit_mu);
    s->presents++;
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        uint64_t tnow = now_ns();
        if (!s->timing.start) s->timing.start = tnow;
        wsi_timing_submitted(&s->timing, timing_id);
        wsi_timing_poll(&s->timing, g.d, s->swapchain);
        tnow = now_ns();
        wsi_timing_report(&s->timing, tnow);
        // 每上屏一帧调一次共用 ap_host 的上屏出口（游玩时间、直连安全网帧证据、停帧看门、HUD 与帧计数；同 iOS dm_guest_frame_tick、
        //   旧层 mtl_vk_shim.c mtl_present 的口径）。新旧两层不会编进同一个包（LinkSources.cmake 两张清单互斥），一帧只调一次；
        //   ap_host.c 只在游戏目标里，独立直连探针 / vk 宿主用例没链就跳过
        { extern void s2a_frame_presented(void) __attribute__((weak)); if (s2a_frame_presented) s2a_frame_presented(); }
        shown.status = NVN_GPU_SHOWN_UNKNOWN;   // 交给了呈现引擎，但没有实际上屏时刻（没有 CAP_SHOWN_TIME）
        if ((r == VK_SUBOPTIMAL_KHR && wsi_suboptimal_stale(s)) || s->outdated == 2) s->outdated = 1;
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR) { s->outdated = 1; out = fail(err, NVN_GPU_E_OUTDATED, "呈现时交换链已过期"); }
    else if (r == VK_ERROR_SURFACE_LOST_KHR) { s->gen = 0; out = fail(err, NVN_GPU_E_REVOKED, "表面已丢失"); }
    else out = fail(err, r == VK_ERROR_DEVICE_LOST ? NVN_GPU_E_DEVICE : NVN_GPU_E_UNSUPPORTED, "vkQueuePresentKHR 失败（%d）", (int)r);
done:
    if (presented) { g_in_callback++; presented(user, &shown); g_in_callback--; }
    return out;
}
