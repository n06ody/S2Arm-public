// Vulkan 后端（nvn_gpu_*）D8 第一段验收：直接调 D0 原语，不经过任何 OP 解码器。
// 覆盖设计文档 §8：C01（32 位填充）、C02/C03（失败终止、失败段不回写）、C04（发布顺序）、
// C05/C06（录制未提交保活）、C08（cmd_abort）、C23 的粘滞错误部分，以及 close 义务。
// 期望值全部是 CPU 手算 / 契约条文，不取自被测实现。
#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvn_backend.h"
#include "nvn_vk_format.h"
#include "nvn_vk_bcdec.h"
#include "nvn_fmt_info.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"

// 平台件替身：无头表面（lavapipe 没有窗口系统）。交换链、取图、呈现段、信号量与布局都是真的，校验层照查。
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) {
    static const char *const exts[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    *n = 2; return exts;
}
VkResult nvn_plat_vk_surface_create(VkInstance instance, void *native, VkSurfaceKHR *out) {
    (void)native;
    VkHeadlessSurfaceCreateInfoEXT ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    return vkCreateHeadlessSurfaceEXT(instance, &ci, NULL, out);
}
int nvn_plat_window_size(void *native, uint32_t *w, uint32_t *h) { (void)native; *w = 320; *h = 180; return 1; }
// S2A_TEST_NVK_DQ=1：整套用例在「两级回放开」下再跑一遍（run.sh 第二遍）
// S2A_TEST_NVK_DQ=1：整套用例在「两级回放开」下再跑一遍；S2A_TEST_DESC_MEMO=1：在「描述符组备忘开」下再跑一遍
// （备忘开着时内容相同的组不再分配，按「分配了几组」写的那几条期望在这一遍里不比，像素期望照比）。
static int g_memo_on;
static unsigned g_pipeprobe_seen;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pipeprobe")) return 1;
    if (!strcmp(name, "nvk_dq") && getenv("S2A_TEST_NVK_DQ")) return 1;
    if (!strcmp(name, "nvk_desc_memo") && getenv("S2A_TEST_DESC_MEMO")) { g_memo_on = 1; return 1; }
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level;
    if (text && strstr(text, "pipeprobe event=hit")) g_pipeprobe_seen |= 1;
    if (text && strstr(text, "pipeprobe event=miss")) g_pipeprobe_seen |= 2;
    if (text && strstr(text, "pipeprobe event=create")) g_pipeprobe_seen |= 4;
    if (text && strstr(text, "same_shaders=1 fields=poly")) g_pipeprobe_seen |= 8;
    printf("[%s] %s\n", cls ? cls : "", text ? text : "");
}
static int g_tri_origin_ul;   // 1 = 按 NVN 窗口原点在左上画（视口翻转）：期望上下对调。缺省 0 = 原点在左下（Vulkan 的自然方向）

// C66 only: model a strict driver enforcing maxSets=64; all other calls
// reach the real lavapipe entry. This is injected behaviour, not a claim about llvmpipe.
static int g_c66_inject_pool_limit;
static unsigned g_c66_sets_requested, g_c66_rejects;
static VkDescriptorPool g_c66_pool;
static unsigned g_desc_sets_total;
VKAPI_ATTR VkResult VKAPI_CALL __real_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *sets) {
    if (g_c66_inject_pool_limit) {
        if (g_c66_pool != info->descriptorPool) { g_c66_pool = info->descriptorPool; g_c66_sets_requested = 0; }
        g_c66_sets_requested += info->descriptorSetCount;
        if (g_c66_sets_requested > 64) { g_c66_rejects++; return VK_ERROR_OUT_OF_POOL_MEMORY; }
    }
    VkResult rc = __real_vkAllocateDescriptorSets(device, info, sets);
    if (rc == VK_SUCCESS) g_desc_sets_total += info->descriptorSetCount;
    return rc;
}

// 限定注入：真实 R32_UINT 支持不变，仅模拟驱动只报告单一 texel 能力。
static VkFormatFeatureFlags g_texel_feature_mask;
VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice device, VkFormat format, VkFormatProperties *props) {
    __real_vkGetPhysicalDeviceFormatProperties(device, format, props);
    if (g_texel_feature_mask && format == VK_FORMAT_R32_UINT)
        props->bufferFeatures &= ~g_texel_feature_mask;
}

extern uint32_t nvn_gpu_vk_test_push_capacity(nvn_gpu_cmd c, nvn_gpu_stage st);
extern void nvn_gpu_vk_test_emit_counts(nvn_gpu_cmd c, uint32_t out[9]);   // #708：管线、视口、裁剪、混合常量、模板、深度偏移、顶点缓冲、索引缓冲、push 各录了几次
extern void nvn_gpu_vk_test_set_fault(int mode);
extern void nvn_gpu_vk_test_fail_fence(uint64_t signal);
extern void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms);

static int g_fail;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            g_fail++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)
#define OK(r) ((r).code == NVN_GPU_OK)

struct done_rec { uint64_t signal; uint32_t status; int code; };
static struct done_rec g_done[64];
static atomic_int g_ndone;
static atomic_int g_order_bad;
static volatile int g_event;   // 普通变量：C04 靠 fence_wait 的同步边，TSan 才能真的检查
static atomic_int g_retired;
static atomic_int g_ndone_at_retire;
static atomic_int g_probe_order, g_order_code;

static void on_done(void *u, const nvn_gpu_done *d) {
    (void)u;
    int i = atomic_load_explicit(&g_ndone, memory_order_relaxed);   // done 回调由 worker 串行调用
    if (i < 64) { g_done[i].signal = d->signal; g_done[i].status = d->status; g_done[i].code = d->rc.code; }
    atomic_store_explicit(&g_ndone, i + 1, memory_order_release);
    // 发布顺序：成功段的 done 里，已完成值必须还没推进到本段 signal。
    if (d->status == NVN_GPU_DONE_OK && nvn_gpu_fence_done() >= d->signal) atomic_fetch_add(&g_order_bad, 1);
    if (d->status == NVN_GPU_DONE_OK) g_event = 9;
    if (atomic_load(&g_probe_order)) {
        nvn_gpu_err e;
        atomic_store(&g_order_code, nvn_gpu_fence_wait(d->signal, 0, &e).code);
    }
}

static void on_retire(void *u) {
    (void)u;
    atomic_store(&g_ndone_at_retire, atomic_load(&g_ndone));
    atomic_fetch_add(&g_retired, 1);
}

static void sleep_ms(int ms) { struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&ts, NULL); }

static int wait_until(atomic_int *v, int target, int ms) {
    for (int i = 0; i < ms; i++) {
        if (atomic_load(v) >= target) return 1;
        sleep_ms(1);
    }
    return atomic_load(v) >= target;
}

static void wait_done(int n) { CHECK(wait_until(&g_ndone, n, 5000), "等 done 回调超时：期望 %d，实到 %d", n, atomic_load(&g_ndone)); }

// 只认 base 之后的新记录：重开会话后 signal 会从头开始，旧记录不能冒充。
static const struct done_rec *wait_signal(uint64_t signal, int base, int ms) {
    for (int t = 0; t < ms; t++) {
        int n = atomic_load_explicit(&g_ndone, memory_order_acquire);
        if (n > 64) n = 64;
        for (int i = n - 1; i >= base; i--)
            if (g_done[i].signal == signal) return &g_done[i];
        sleep_ms(1);
    }
    return NULL;
}

static uint8_t *buf_cpu(nvn_gpu_buf b) { return nvn_gpu_buf_cpu(b); }

static struct s2a_vk_device g_dev;
static nvn_gpu_open_desc g_desc;

// 保守故障注入：不等 fence 就返回 TIMEOUT，记录完成证明前的视图销毁；其余调用走真实 Vulkan。
static int g_bc_inject_wait, g_bc_unproven, g_bc_early_destroy;
static VkFence g_bc_injected_fence;
static int g_bc_early_n, g_bc_early_seen;
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    PFN_vkWaitForFences real = (PFN_vkWaitForFences)vkGetDeviceProcAddr(d, "vkWaitForFences");
    if (g_bc_inject_wait && n == 1) {   // 真超时：睡满调用方给的时限再报超时（等栅栏的包装按墙钟判，不睡满它会接着等）
        g_bc_inject_wait = 0; g_bc_unproven = 1; g_bc_injected_fence = f[0];
        struct timespec ts = {(time_t)(timeout / 1000000000ull), (long)(timeout % 1000000000ull)}; nanosleep(&ts, NULL);
        return VK_TIMEOUT;
    }
    if (g_bc_early_n > 0 && n == 1) { g_bc_early_n--; g_bc_early_seen++; return VK_TIMEOUT; }   // 提前返回（#697）：不等、立刻报超时
    return real(d, n, f, all, timeout);
}
// #697: synthetic driver enumeration, keeping real headless images/semaphores and VVL.
// Virtual indices 8+ map to the driver's real indices; this is NOT a device result.
static uint32_t g_w697_count, g_w697_real_count, g_w697_base = 8;
static int g_w697_fail_sem;
static int g_w697_incomplete, g_w697_invalid;
static unsigned g_w697_queries, g_w697_fetches, g_w697_created, g_w697_destroyed, g_w697_sc_destroyed;
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *images) {
    PFN_vkGetSwapchainImagesKHR real = (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(d, "vkGetSwapchainImagesKHR");
    if (!g_w697_count) return real(d, sc, n, images);
    if (!images) { g_w697_queries++; *n = g_w697_count; return VK_SUCCESS; }
    g_w697_fetches++;
    VkImage actual[64]; uint32_t nr = 64;
    VkResult r = real(d, sc, &nr, actual);
    if (r != VK_SUCCESS || !nr || nr > 6) return VK_ERROR_INITIALIZATION_FAILED;
    g_w697_real_count = nr;
    uint32_t cap = *n, written = cap < g_w697_count ? cap : g_w697_count;
    for (uint32_t i = 0; i < written; i++) images[i] = actual[(i >= g_w697_base ? i - g_w697_base : i) % nr];
    *n = written;
    if (g_w697_incomplete) { if (g_w697_incomplete > 0) g_w697_incomplete--; return VK_INCOMPLETE; }
    return cap < g_w697_count ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *idx) {
    PFN_vkAcquireNextImageKHR real = (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(d, "vkAcquireNextImageKHR");
    VkResult r = real(d, sc, timeout, sem, fence, idx);
    if (g_w697_count && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) *idx = g_w697_invalid ? g_w697_count : *idx + g_w697_base;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *sem) {
    PFN_vkCreateSemaphore real = (PFN_vkCreateSemaphore)vkGetDeviceProcAddr(d, "vkCreateSemaphore");
    if (g_w697_count && g_w697_fail_sem > 0 && !--g_w697_fail_sem) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r = real(d, ci, a, sem);
    if (g_w697_count && r == VK_SUCCESS) g_w697_created++;
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice d, VkSemaphore sem, const VkAllocationCallbacks *a) {
    PFN_vkDestroySemaphore real = (PFN_vkDestroySemaphore)vkGetDeviceProcAddr(d, "vkDestroySemaphore");
    if (g_w697_count && sem) g_w697_destroyed++;
    real(d, sem, a);
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    PFN_vkDestroySwapchainKHR real = (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(d, "vkDestroySwapchainKHR");
    if (g_w697_count && sc) g_w697_sc_destroyed++;
    real(d, sc, a);
}

// 呈现「次优」注入（C199）：呈现照常做，结果改报 VK_SUBOPTIMAL_KHR；g_wsi_rotated 让表面的当前方向报成转了 90 度。
static int g_wsi_subopt, g_wsi_rotated;
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    PFN_vkQueuePresentKHR real = (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(g_dev.device, "vkQueuePresentKHR");
    VkPresentInfoKHR mapped = *pi; uint32_t idx;
    if (g_w697_count && pi->swapchainCount == 1) {
        idx = pi->pImageIndices[0];
        CHECK(idx >= g_w697_base && idx < g_w697_count, "C697 present virtual index %u outside range", idx);
        idx = (idx >= g_w697_base ? idx - g_w697_base : idx) % g_w697_real_count;
        mapped.pImageIndices = &idx;
    }
    VkResult r = real(q, &mapped);
    return r == VK_SUCCESS && g_wsi_subopt ? VK_SUBOPTIMAL_KHR : r;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR sf, VkSurfaceCapabilitiesKHR *caps) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR real = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)dlsym(RTLD_NEXT, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkResult r = real(pd, sf, caps);
    if (r == VK_SUCCESS && g_wsi_rotated) caps->currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView view, const VkAllocationCallbacks *a) {
    PFN_vkDestroyImageView real = (PFN_vkDestroyImageView)vkGetDeviceProcAddr(d, "vkDestroyImageView");
    if (g_bc_unproven && view) g_bc_early_destroy++;
    real(d, view, a);
}

static void bc_timeout_lifetime(void) {
    VkDevice d = g_dev.device; VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
    struct nvn_vk_bcdec *bc = NULL; char why[192] = {0};
    VkImageCreateInfo ii = {0}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = (VkExtent3D){4,4,1};
    ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult vr = vkCreateImage(d, &ii, NULL, &image); CHECK(vr == VK_SUCCESS, "BC timeout 建图：%d", vr); if (vr != VK_SUCCESS) goto done;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(d, image, &mr);
    VkMemoryAllocateInfo ma = {0}; ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize = mr.size;
    ma.memoryTypeIndex = s2a_vk_memory_type(&g_dev, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (ma.memoryTypeIndex == UINT32_MAX) { CHECK(0, "BC timeout 无显存类型"); goto done; }
    vr = vkAllocateMemory(d, &ma, NULL, &memory); CHECK(vr == VK_SUCCESS, "BC timeout 分配：%d", vr); if (vr != VK_SUCCESS) goto done;
    vr = vkBindImageMemory(d, image, memory, 0); CHECK(vr == VK_SUCCESS, "BC timeout 绑定：%d", vr); if (vr != VK_SUCCESS) goto done;
    int br = nvn_vk_bcdec_create(&g_dev, &bc, why, sizeof why); CHECK(br == 0, "BC timeout 解码器：%s", why); if (br) goto done;
    { static const uint8_t block[8] = {0,0xf8,0,0xf8,0,0,0,0};
      // #697：驱动的等待被信号打断、提前报超时（这里连着 3 次立刻返回 VK_TIMEOUT）→ 没到墙钟时限必须接着等，解码照常成功
      g_bc_early_n = 3; g_bc_early_seen = 0;
      br = nvn_vk_bcdec_decode(bc, &g_dev, image, NVN_FORMAT_RGBA_DXT1, 4, 4, 0, 0, block, sizeof block, why, sizeof why);
      CHECK(br == 0 && g_bc_early_seen == 3 && g_bc_early_n == 0, "BC 等待提前返回 3 次：期望接着等、解码成功（0）且 3 次都被消耗 → 返回 %d、消耗 %d 次 %s", br, g_bc_early_seen, why);
      g_bc_early_n = 0;
      g_bc_inject_wait = 1; g_bc_unproven = g_bc_early_destroy = 0; g_bc_injected_fence = VK_NULL_HANDLE;
      br = nvn_vk_bcdec_decode(bc, &g_dev, image, NVN_FORMAT_RGBA_DXT1, 4, 4, 0, 0, block, sizeof block, why, sizeof why);
      CHECK(br == -2 && g_bc_injected_fence, "BC timeout 应报告未证明完成，实得 %d %s", br, why);
      CHECK(g_bc_early_destroy == 0, "BC timeout 完成证明前销毁视图：%d", g_bc_early_destroy);
      g_bc_inject_wait = 1;
      nvn_vk_bcdec_destroy(&g_dev, bc);  // 第二次等待仍失败：解码器和视图也必须继续保活
      CHECK(g_bc_early_destroy == 0, "BC destroy 等待失败时提前销毁视图：%d", g_bc_early_destroy);
      if (g_bc_injected_fence) {
          vr = vkWaitForFences(d, 1, &g_bc_injected_fence, VK_TRUE, 5000000000ull);
          CHECK(vr == VK_SUCCESS, "BC timeout 后真实 fence 完成：%d", vr);
      }
      g_bc_unproven = 0;
      printf("BC timeout 注入用例已执行：rc=%d，提前销毁=%d\n", br, g_bc_early_destroy); }
done:
    g_bc_inject_wait = g_bc_unproven = 0;
    if (bc) nvn_vk_bcdec_destroy(&g_dev, bc);
    if (image) vkDestroyImage(d, image, NULL);
    if (memory) vkFreeMemory(d, memory, NULL);
}

static int open_backend(void) {
    nvn_gpu_err e;
    memset(&e, 0, sizeof e);
    nvn_gpu_rc r = nvn_gpu_open(&g_desc, &e);
    if (!OK(r)) printf("open 失败：%d %s\n", r.code, e.why);
    return OK(r);
}

static nvn_gpu_buf make_buf(uint64_t n, int mem, const void *init) {
    nvn_gpu_buf b = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_buf_create(n, mem, init, &b, &e);
    CHECK(OK(r), "buf_create(%llu) 失败：%d %s", (unsigned long long)n, r.code, e.why);
    return b;
}

static nvn_gpu_tex make_tex(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td;
    memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = (int)fmt;
    td.d.width = w;
    td.d.height = h;
    td.d.depth = 1;
    td.d.levels = 1;
    td.d.samples = 0;
    td.usage = usage;
    nvn_gpu_tex t = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "tex_create 失败：%d %s", r.code, e.why);
    return t;
}

static nvn_gpu_tex make_tex_target(uint32_t w, uint32_t h, uint32_t depth, uint32_t target, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td;
    memset(&td, 0, sizeof td);
    td.d.target = (int)target; td.d.format = (int)fmt; td.d.width = w; td.d.height = h;
    td.d.depth = (int)depth; td.d.levels = 1; td.d.samples = 0; td.usage = usage;
    nvn_gpu_tex t = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "tex_create target %u 失败：%d %s", target, r.code, e.why);
    return t;
}

static nvn_gpu_view make_view(nvn_gpu_tex t) {
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create(t, NULL, &v, &e);
    CHECK(OK(r), "view_create 失败：%d %s", r.code, e.why);
    return v;
}

static nvn_gpu_cmd begin_cmd(void) {
    nvn_gpu_cmd c = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_begin(NULL, &c, &e);
    CHECK(OK(r), "cmd_begin 失败：%d %s", r.code, e.why);
    return c;
}

// C01：fill 是 32 位样式，不是逐字节 memset；范围外不动。
static void t01_fill32(void) {
    uint8_t zeros[64];
    memset(zeros, 0, sizeof zeros);
    nvn_gpu_buf b = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 16, 16, 0x11223344u);
    nvn_gpu_err e;
    atomic_store(&g_probe_order, 1);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 2000000000ull, &e);
    CHECK(OK(r), "wait 1：%d %s", r.code, e.why);
    uint8_t *p = buf_cpu(b);
    CHECK(p != NULL, "SHARED 缓冲没有 CPU 地址");
    static const uint8_t want[4] = {0x44, 0x33, 0x22, 0x11};
    for (int i = 0; i < 16; i++) CHECK(p[16 + i] == want[i % 4], "fill 字节 %d = %02x，期望 %02x", 16 + i, p[16 + i], want[i % 4]);
    for (int i = 0; i < 16; i++) CHECK(p[i] == 0, "fill 前越界改了字节 %d", i);
    for (int i = 32; i < 64; i++) CHECK(p[i] == 0, "fill 后越界改了字节 %d", i);
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(1);
}

// 写入 + 拷贝：dst 与 src 全等；CPU 真值不来自 GPU。
static void t02_write_copy(void) {
    uint8_t init[256], zero[256];
    for (int i = 0; i < 256; i++) init[i] = (uint8_t)i;
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(256, NVN_GPU_MEM_SHARED, init);
    nvn_gpu_buf b = make_buf(256, NVN_GPU_MEM_SHARED, zero);
    uint8_t w[16];
    for (int i = 0; i < 16; i++) w[i] = (uint8_t)(0x80 + i);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf_write(c, a, 32, w, sizeof w);
    nvn_gpu_copy_buf(c, a, 0, b, 0, 256);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(2, 2000000000ull, &e);
    CHECK(OK(r), "wait 2：%d %s", r.code, e.why);
    CHECK(memcmp(buf_cpu(a), buf_cpu(b), 256) == 0, "copy 后 dst 与 src 不同");
    CHECK(memcmp(buf_cpu(a) + 32, w, sizeof w) == 0, "buf_write 内容不对");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(2);
}

// C05：录制进未提交 cmd 后立刻 release，真正销毁必须等提交段 done 之后。
static void t03_keepalive_unsubmitted(void) {
    uint8_t pat[64], zero[64];
    memset(pat, 0xAB, sizeof pat);
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf src = make_buf(64, NVN_GPU_MEM_SHARED, pat);
    nvn_gpu_buf dst = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    int retired_before = atomic_load(&g_retired), done_base = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_copy_buf(c, src, 0, dst, 0, 64);
    nvn_gpu_buf_release(src, on_retire, NULL);
    sleep_ms(20);
    CHECK(atomic_load(&g_retired) == retired_before, "录制未提交的 src 被提前销毁");
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(3, 2000000000ull, &e);
    CHECK(OK(r), "wait 3：%d %s", r.code, e.why);
    for (int i = 0; i < 64; i++) CHECK(buf_cpu(dst)[i] == 0xAB, "保活期间内容被改，字节 %d", i);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "commit 完成后 src 没有退休");
    CHECK(atomic_load(&g_ndone_at_retire) >= done_base + 1, "retired 在本段 done 之前就调了");
    nvn_gpu_buf_release(dst, NULL, NULL);
    wait_done(3);
}

// C06：经 GPU 地址间接引用（buf_use）也保活；retired 在该段 done 之后才调。
static void t03b_buf_use(void) {
    uint8_t zero[32];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(32, NVN_GPU_MEM_SHARED, zero);
    int retired_before = atomic_load(&g_retired), done_before = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf_use(c, b);
    nvn_gpu_buf_release(b, on_retire, NULL);
    sleep_ms(20);
    CHECK(atomic_load(&g_retired) == retired_before, "buf_use 录制后仍提前销毁");
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 4, on_done, NULL, &e);
    CHECK(OK(r), "commit 4：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(4, 2000000000ull, &e);
    CHECK(OK(r), "wait 4：%d %s", r.code, e.why);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "buf_use 段完成后没有退休");
    CHECK(atomic_load(&g_retired) == retired_before + 1, "retired 调了不止一次");
    CHECK(atomic_load(&g_ndone_at_retire) >= done_before + 1, "retired 在本段 done 之前就调了");
    wait_done(4);
}

// C08：abort 不执行、不回调、不推进时间线，引用随即解除。
static void t04_abort(void) {
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf keep = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf rel = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    int done_before = atomic_load(&g_ndone), retired_before = atomic_load(&g_retired);
    uint64_t fence_before = nvn_gpu_fence_done();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, keep, 0, 64, 0xFFFFFFFFu);
    nvn_gpu_copy_buf(c, keep, 0, rel, 0, 64);
    nvn_gpu_buf_release(rel, on_retire, NULL);
    nvn_gpu_cmd_abort(c);
    sleep_ms(50);
    for (int i = 0; i < 64; i++) CHECK(buf_cpu(keep)[i] == 0, "abort 后仍执行了 fill（字节 %d）", i);
    CHECK(atomic_load(&g_ndone) == done_before, "abort 后仍调了 done");
    CHECK(nvn_gpu_fence_done() == fence_before, "abort 推进了时间线");
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "abort 后引用没有解除 / 没有退休");
    nvn_gpu_buf_release(keep, NULL, NULL);
}

// C02/C03：失败段终止后续；失败段与后续段都不执行、各回调一次 FAILED。
static void t05_failure_terminal(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf c = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_err e;

    // 三段都先录好再依次提交：失败终止后 begin 会返回 E_FAILED，C02 的「段 3」属于已录制的段。
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, a, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, c, 0, 16, 0x33333333u);

    nvn_gpu_rc r = nvn_gpu_cmd_commit(c1, 5, on_done, NULL, &e);
    CHECK(OK(r), "段 1 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(5, 2000000000ull, &e);
    CHECK(OK(r), "wait(5)：%d %s", r.code, e.why);

    nvn_gpu_vk_test_set_fault(1);
    r = nvn_gpu_cmd_commit(c2, 6, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_DEVICE, "段 2 注入失败应返回 E_DEVICE，实为 %d", r.code);

    r = nvn_gpu_cmd_commit(c3, 7, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "段 3 应返回 E_FAILED，实为 %d", r.code);

    CHECK(nvn_gpu_fence_done() == 5, "fence_done 应为 5，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_failed() == 6, "fence_failed 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    r = nvn_gpu_fence_wait(6, 2000000000ull, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "wait(6) 应 FAILED，实为 %d", r.code);
    r = nvn_gpu_fence_wait(7, 2000000000ull, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "wait(7) 应 FAILED，实为 %d", r.code);

    const struct done_rec *d2 = wait_signal(6, 0, 5000), *d3 = wait_signal(7, 0, 5000);
    CHECK(d2 && d2->status == NVN_GPU_DONE_FAILED, "段 2 回调不是 FAILED");
    CHECK(d3 && d3->status == NVN_GPU_DONE_FAILED, "段 3 回调不是 FAILED");
    for (int i = 0; i < 16; i++) {
        CHECK(((uint32_t *)buf_cpu(b))[i / 4] == 0, "失败段写了 B（偏移 %d）", i);
        CHECK(((uint32_t *)buf_cpu(c))[i / 4] == 0, "失败段之后仍写了 C（偏移 %d）", i);
    }
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_FAILED, "失败终态后 cmd_begin 应 E_FAILED");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    nvn_gpu_buf_release(c, NULL, NULL);
}

// C04：done 里写的值，等 fence 的线程返回后一定读得到。
struct waiter_arg { uint64_t v; int ok; };
static void *waiter_main(void *p) {
    struct waiter_arg *a = p;
    nvn_gpu_err e;
    a->ok = OK(nvn_gpu_fence_wait(a->v, 5000000000ull, &e));
    return NULL;
}

static void t06_publish_order(void) {
    g_event = 0;
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x00000009u);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    struct waiter_arg a = {1, 0};
    pthread_t t;
    pthread_create(&t, NULL, waiter_main, &a);
    pthread_join(t, NULL);
    CHECK(a.ok, "wait(1) 失败");
    CHECK(atomic_load(&g_order_code) == NVN_GPU_E_ORDER, "done 里 fence_wait 应 E_ORDER，实为 %d", atomic_load(&g_order_code));
    atomic_store(&g_probe_order, 0);
    CHECK(g_event == 9, "done 的写没有在 fence 返回前可见（读到 %d）", g_event);
    CHECK(atomic_load(&g_order_bad) == 0, "done 被调时已完成值已推进（发布顺序错）");
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(4);
}

// 等待语义：从没提交过的值 E_ARG；纯查询不产生副作用。
static void t07_wait_args(void) {
    nvn_gpu_err e;
    CHECK(nvn_gpu_fence_wait(1, 0, &e).code == NVN_GPU_OK, "wait(1, 0) 应 OK");
    CHECK(nvn_gpu_fence_wait(999, 0, &e).code == NVN_GPU_E_ARG, "wait(999) 应 E_ARG");
    CHECK(nvn_gpu_fence_wait(0, 0, &e).code == NVN_GPU_E_ARG, "wait(0) 应 E_ARG");
}

// 粘滞错误：整段不提交，按失败段处理（F = 该段 signal）。
static void t08_sticky(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    int base = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 6, 0xDEADBEEFu);   // 非 4 字节长度
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "粘滞段 commit 应返回 E_ARG，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    const struct done_rec *d5 = wait_signal(2, base, 5000);
    CHECK(d5 && d5->status == NVN_GPU_DONE_FAILED, "粘滞段回调不是 FAILED");
    for (int i = 0; i < 16; i++) CHECK(buf_cpu(b)[i] == 0, "粘滞段仍执行了");
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 没有引用时 release 立即退休。
static void t09_release_only(void) {
    int retired_before = atomic_load(&g_retired);
    nvn_gpu_buf b = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf_release(b, on_retire, NULL);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "无引用 release 没有退休");
}

// 清屏：CPU 手算像素；先清屏再读回，布局屏障也一起走通。
static void t11_clear_color(void) {
    nvn_gpu_tex t = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[8 * 8 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    CHECK(nvn_gpu_buf_create(0, NVN_GPU_MEM_SHARED, NULL, &(nvn_gpu_buf){0}, NULL).code != 0, "空缓冲应被拒");
    nvn_gpu_cmd c = begin_cmd();
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    nvn_gpu_clear_view(c, v, red, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 32, 256);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(2, 2000000000ull, &e);
    CHECK(OK(r), "wait 2：%d %s", r.code, e.why);
    for (int i = 0; i < 64; i++) {
        const uint8_t want[4] = {0xFF, 0x00, 0x00, 0xFF};
        CHECK(buf_cpu(b)[i * 4 + 0] == want[0] && buf_cpu(b)[i * 4 + 1] == want[1] &&
              buf_cpu(b)[i * 4 + 2] == want[2] && buf_cpu(b)[i * 4 + 3] == want[3],
              "清屏像素 %d = %02x%02x%02x%02x", i, buf_cpu(b)[i * 4], buf_cpu(b)[i * 4 + 1], buf_cpu(b)[i * 4 + 2], buf_cpu(b)[i * 4 + 3]);
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 上传 + 贴图到贴图复制：读回字节与 CPU 真值逐字节相同。
static void t12_upload_copy_tex(void) {
    uint8_t pat[4 * 4 * 4];
    for (int i = 0; i < (int)sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + 3);
    nvn_gpu_tex src = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex dst = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, src, 0, 0, NULL, pat, 16, 64);
    nvn_gpu_copy_tex(c, src, 0, 0, NULL, dst, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(3, 2000000000ull, &e);
    CHECK(OK(r), "wait 3：%d %s", r.code, e.why);
    CHECK(memcmp(buf_cpu(b), pat, sizeof pat) == 0, "上传 + 复制后的像素与 CPU 真值不同");
    nvn_gpu_tex_release(src);
    nvn_gpu_tex_release(dst);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 深度清屏：0.5 的 D32_SFLOAT 是 0x3F000000，直接对字节。
static void t13_clear_depth(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    const float half[4] = {0.5f, 0.0f, 0.0f, 0.0f};
    nvn_gpu_clear_view(c, v, half, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 4, on_done, NULL, &e);
    CHECK(OK(r), "commit 4：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(4, 2000000000ull, &e);
    CHECK(OK(r), "wait 4：%d %s", r.code, e.why);
    for (int i = 0; i < 16; i++) {
        uint32_t word;
        memcpy(&word, buf_cpu(b) + i * 4, 4);
        CHECK(word == 0x3F000000u, "深度像素 %d = 0x%08x，期望 0x3f000000", i, word);
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}


static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    void *p = malloc((size_t)sz);
    if (p && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); p = NULL; }
    fclose(f);
    if (p) *n = (size_t)sz;
    return p;
}

// 原创纹理三角形：全屏三角形 + 2x2 纹理 + nearest 采样；逐像素用 CPU 手算真值。
// 通道映射用例的旋钮（缺省全 0 = 不设）：存储自带映射、视图映射（NVN 取值 0 零 / 1 一 / 2 R / 3 G / 4 B / 5 A）
static uint16_t g_tri_swz; static int g_tri_vsw_on; static uint8_t g_tri_vsw[4];
static const uint8_t *g_tri_expect;   // 非空：整张图期望都是这 4 个字节（压缩格式用，纹素不是按像素摆的）
static int g_tri_tol;
static uint64_t g_tri_signal = 5;
static void tri_impl(const char *shader_dir, uint32_t tex_fmt, const uint8_t *texels, int tbpp, int trow, int want_a) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", shader_dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri.frag.spv", shader_dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fsz, "读不到 SPIR-V（%s）", shader_dir);
    if (!vs || !fs) { free(vs); free(fs); return; }

    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fs_layout;
    memset(&fs_layout, 0, sizeof fs_layout);
    fs_layout.ent = ents;
    fs_layout.nent = 2;
    nvn_gpu_layout vs_layout;
    memset(&vs_layout, 0, sizeof vs_layout);

    nvn_gpu_shader vs_sh = NULL, fs_sh = NULL;
    nvn_gpu_err e;
    nvn_gpu_shader_src ssrc;
    memset(&ssrc, 0, sizeof ssrc);
    ssrc.stage = NVN_GPU_SH_VS; ssrc.kind = NVN_GPU_BLOB_SPIRV; ssrc.entry = "main";
    ssrc.blob = vs; ssrc.bytes = vsz; ssrc.layout = &vs_layout;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &vs_sh, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    ssrc.stage = NVN_GPU_SH_FS; ssrc.blob = fs; ssrc.bytes = fsz; ssrc.layout = &fs_layout;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &fs_sh, &e)), "FS 装载：%d %s", e.rc.code, e.why);

    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs_sh;
    pd.st[NVN_GPU_SH_FS].sh = fs_sh;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;   // NVN_DRAW_PRIMITIVE_TRIANGLES
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "pipe_get：%d %s", r.code, e.why);
    if (!OK(r)) { nvn_gpu_shader_release(vs_sh); nvn_gpu_shader_release(fs_sh); free(vs); free(fs); return; }

    const float verts[3][4] = {{-1.0f, -1.0f, 0.0f, 0.0f},
                               { 3.0f, -1.0f, 2.0f, 0.0f},
                               {-1.0f,  3.0f, 0.0f, 2.0f}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex tex = NULL;
    {
        nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = (int)tex_fmt; td.d.width = 2; td.d.height = 2; td.d.depth = 1; td.d.levels = 1;
        td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD; td.swz = g_tri_swz;
        r = nvn_gpu_tex_create(&td, &tex, &e);
        CHECK(OK(r), "tex_create（swz=%#x）：%d %s", g_tri_swz, r.code, e.why);
    }
    nvn_gpu_view tview = NULL;
    if (g_tri_vsw_on) {
        view_t vw; memset(&vw, 0, sizeof vw); vw.set_swizzle = 1; memcpy(vw.swz, g_tri_vsw, 4);
        r = nvn_gpu_view_create(tex, &vw, &tview, &e);
        CHECK(OK(r), "view_create（带通道映射）：%d %s", r.code, e.why);
    } else tview = make_view(tex);
    samp_par sp;
    memset(&sp, 0, sizeof sp);   // nearest / clamp
    nvn_gpu_samp samp = NULL;
    r = nvn_gpu_samp_create(&sp, &samp, &e);
    CHECK(OK(r), "samp_create：%d %s", r.code, e.why);

    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view oview = make_view(out);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);

    nvn_gpu_cmd c = begin_cmd();
    // r=NULL：整级上传，展开时按该级宽高取区域（C30 覆盖这条路径）
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, texels, (uint64_t)trow, g_tri_expect ? (uint64_t)trow : (uint64_t)trow * 2);   // 压缩：2x2 只有一排块

    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = 0.0f; pass.color[0].clear.f[1] = 0.0f;
    pass.color[0].clear.f[2] = 0.0f; pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[0] = 0; dyn.sc[1] = 0; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2];
    memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = tview;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    // 保活：录制完成后、提交前就 release 视图 / 贴图 / 采样器，段完成前不能真销毁。
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_samp_release(samp);
    r = nvn_gpu_cmd_commit(c, g_tri_signal, on_done, NULL, &e);
    CHECK(OK(r), "commit 5：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(g_tri_signal, 5000000000ull, &e);
    CHECK(OK(r), "wait 5：%d %s", r.code, e.why);

    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float u = (x + 0.5f) / 64.0f, v = (y + 0.5f) / 64.0f;
            int tx = (int)(u * 2.0f); if (tx > 1) tx = 1;
            int ty = (int)(v * 2.0f); if (ty > 1) ty = 1;
            if (g_tri_origin_ul) ty = 1 - ty;   // 原点在左上：同一个裁剪空间 y 落到相反的行
            const uint8_t *src = g_tri_expect ? g_tri_expect : texels + (ty * 2 + tx) * tbpp;
            // 期望：先取纹素（没有 alpha 的格式按 want_a），再按生效的通道映射手算（视图设了用视图的，否则用存储的）
            uint8_t px[4] = {src[0], src[1], src[2], (uint8_t)(tbpp == 4 ? src[3] : want_a)}, mapped[4], sw[4] = {2, 3, 4, 5};
            if (g_tri_vsw_on) memcpy(sw, g_tri_vsw, 4);
            else if (g_tri_swz) for (int ch = 0; ch < 4; ch++) sw[ch] = (uint8_t)(g_tri_swz >> (3 * ch) & 7);
            for (int ch = 0; ch < 4; ch++) mapped[ch] = sw[ch] == 0 ? 0 : sw[ch] == 1 ? 255 : px[sw[ch] - 2];
            const uint8_t *want = mapped;
            const uint8_t *got = buf_cpu(rb) + (y * 64 + x) * 4;
            if (abs(got[0] - want[0]) > g_tri_tol || abs(got[1] - want[1]) > g_tri_tol || abs(got[2] - want[2]) > g_tri_tol || abs(got[3] - (g_tri_vsw_on || g_tri_swz ? want[3] : want_a)) > g_tri_tol) {
                g_fail++;
                printf("FAIL %s:%d: 三角像素 (%d,%d) = %02x%02x%02x%02x，期望 %02x%02x%02x%02x\n",
                       __FILE__, __LINE__, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                y = 64;
                break;
            }
        }
    }
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs_sh);
    nvn_gpu_shader_release(fs_sh);
    free(vs);
    free(fs);
}


static void t14_textured_triangle(const char *dir) {
    static const uint8_t texels[16] = {255, 0, 0, 255,   0, 255, 0, 255,
                                       0, 0, 255, 255,   255, 255, 255, 255};
    tri_impl(dir, NVN_FORMAT_RGBA8, texels, 4, 8, 255);
}

// C70：通道映射。存储自带映射 B G R 一（红蓝对调、alpha 恒 1）；视图映射 零 R A G 取代存储的（不是叠加）。
// 纹素的四个通道互不相同，哪一路映射错了都读得出来。期望在 tri_impl 里按 NVN 取值手算。
static void c70_swizzle(const char *dir, int view_overrides) {
    static const uint8_t texels[16] = {10, 60, 110, 200,   20, 70, 120, 210,
                                       30, 80, 130, 220,   40, 90, 140, 230};
    g_tri_swz = 4 | 3 << 3 | 2 << 6 | 1 << 9;
    if (view_overrides) { g_tri_vsw_on = 1; g_tri_vsw[0] = 0; g_tri_vsw[1] = 2; g_tri_vsw[2] = 5; g_tri_vsw[3] = 3; }
    tri_impl(dir, NVN_FORMAT_RGBA8, texels, 4, 8, 255);
    g_tri_swz = 0; g_tri_vsw_on = 0;
}

// C71：上屏。接面 → 取图 → 把刚提交（还没等完成）的贴图贴上去呈现 → 取了不呈现再取 → 越界矩形 → 重建换代 → 撤面。
// 无头表面读不回屏上像素：本用例核的是所有权与换代语义、每个 ticket 恰好回调一次，以及校验层 0 条（布局、信号量、交换链用法）。
static int g_shown_n; static uint64_t g_shown_ticket; static uint32_t g_shown_status;
static void on_shown(void *user, const nvn_gpu_shown *s) { (void)user; g_shown_n++; g_shown_ticket = s->ticket; g_shown_status = s->status; }
static void c71_present(void) {
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_surface sf = NULL, sf2 = NULL;
    nvn_gpu_rc r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(OK(r) && sf, "C71 attach：%d %s", r.code, e.why);
    if (!sf) return;
    CHECK(nvn_gpu_surface_gen(sf) == 1, "C71 首个 generation 应为 1，得 %llu", (unsigned long long)nvn_gpu_surface_gen(sf));
    r = nvn_gpu_surface_attach((void *)1, &sf2, &e);
    CHECK(r.code == NVN_GPU_E_ORDER && !sf2, "C71 已接着一个面时再 attach 应 E_ORDER，得 %d", r.code);

    nvn_gpu_tex tex = make_tex(64, 36, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view view = make_view(tex);
    static uint8_t px[64 * 36 * 4]; memset(px, 0x80, sizeof px);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, px, 64 * 4, sizeof px);
    r = nvn_gpu_cmd_commit(c, 7, on_done, NULL, &e);
    CHECK(OK(r), "C71 commit：%d %s", r.code, e.why);

    nvn_gpu_drawable dr; memset(&dr, 0, sizeof dr);
    r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
    CHECK(OK(r) && dr.w == 320 && dr.h == 180 && dr.gen == 1, "C71 acquire：%d %s（%ux%u gen %llu）", r.code, e.why, dr.w, dr.h, (unsigned long long)dr.gen);
    nvn_gpu_present_desc pd; memset(&pd, 0, sizeof pd); pd.src = view; pd.filter = 1;
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &dr, &pd, 42, on_shown, NULL, &e);   // 上传那一段还没等完成：呈现段排在它后面
    CHECK(OK(r), "C71 present：%d %s", r.code, e.why);
    CHECK(g_shown_n == 1 && g_shown_ticket == 42 && g_shown_status == NVN_GPU_SHOWN_UNKNOWN, "C71 presented 应恰好一次、UNKNOWN（没有上屏时刻），得 n=%d status=%u", g_shown_n, g_shown_status);
    CHECK(OK(nvn_gpu_fence_wait(7, 5000000000ull, &e)), "C71 wait 7：%d %s", e.rc.code, e.why);

    // 取了不呈现：交回后下一次取到的是同一张
    nvn_gpu_drawable a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &a, &e)), "C71 acquire 2：%d %s", e.rc.code, e.why);
    nvn_gpu_drawable_release(sf, &a);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &b, &e)) && b.token == a.token, "C71 交回的 drawable 应作为下次取图的结果");
    // 目标矩形越界：拒绝、按丢帧回调；这张图仍留给下一次
    pd.dst[0] = 300; pd.dst[1] = 0; pd.dst[2] = 100; pd.dst[3] = 50;
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &b, &pd, 43, on_shown, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG && g_shown_n == 1 && g_shown_status == NVN_GPU_SHOWN_DROPPED, "C71 越界矩形应 E_ARG + DROPPED，得 %d n=%d status=%u", r.code, g_shown_n, g_shown_status);
    nvn_gpu_drawable d3; memset(&d3, 0, sizeof d3);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &d3, &e)) && d3.token == b.token, "C71 被拒的那张应留给下一次取图");
    memset(pd.dst, 0, sizeof pd.dst); pd.dst[0] = 40; pd.dst[1] = 0; pd.dst[2] = 240; pd.dst[3] = 180;   // 两侧留黑边
    pd.crop[0] = 0; pd.crop[1] = 0; pd.crop[2] = 32; pd.crop[3] = 18;
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &d3, &pd, 44, on_shown, NULL, &e);
    CHECK(OK(r) && g_shown_n == 1 && g_shown_ticket == 44, "C71 present（裁剪 + 黑边）：%d %s", r.code, e.why);

    // 重建换代：旧 generation 的 drawable 作废
    nvn_gpu_drawable old; memset(&old, 0, sizeof old);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &old, &e)), "C71 acquire 4：%d %s", e.rc.code, e.why);
    r = nvn_gpu_surface_rebuild(sf, &e);
    CHECK(OK(r) && nvn_gpu_surface_gen(sf) == 2, "C71 rebuild：%d %s gen=%llu", r.code, e.why, (unsigned long long)nvn_gpu_surface_gen(sf));
    memset(pd.dst, 0, sizeof pd.dst); memset(pd.crop, 0, sizeof pd.crop);
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &old, &pd, 45, on_shown, NULL, &e);
    CHECK(r.code == NVN_GPU_E_OUTDATED && g_shown_n == 1 && g_shown_status == NVN_GPU_SHOWN_DROPPED, "C71 旧 generation 的 drawable 应 E_OUTDATED + DROPPED，得 %d", r.code);
    nvn_gpu_drawable nd; memset(&nd, 0, sizeof nd);
    CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &nd, &e)) && nd.gen == 2, "C71 重建后 acquire：%d %s", e.rc.code, e.why);
    g_shown_n = 0;
    r = nvn_gpu_present(sf, &nd, &pd, 46, on_shown, NULL, &e);
    CHECK(OK(r) && g_shown_n == 1, "C71 重建后 present：%d %s", r.code, e.why);

    // C199：呈现回「次优」但表面的方向和尺寸没变（安卓横屏的常态）→ 不重建，下一帧照常取图；表面真转了 → 30 次以内标过期
    { uint64_t gen0 = nvn_gpu_surface_gen(sf); int bad = 0;
      g_wsi_subopt = 1;
      for (int i = 0; i < 40; i++) {
          nvn_gpu_drawable q; memset(&q, 0, sizeof q);
          nvn_gpu_rc ra = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &q, &e);
          if (!OK(ra)) { bad++; break; }
          if (!OK(nvn_gpu_present(sf, &q, &pd, 100 + (uint64_t)i, on_shown, NULL, &e))) bad++;
      }
      CHECK(!bad && nvn_gpu_surface_gen(sf) == gen0, "C199 表面没变、连着 40 帧「次优」：期望每帧取图和呈现都成、不换代 → 失败 %d 次、代 %llu（原 %llu）：%s", bad, (unsigned long long)nvn_gpu_surface_gen(sf), (unsigned long long)gen0, e.why);
      g_wsi_rotated = 1;
      int outdated_at = -1;
      for (int i = 0; i < 31 && outdated_at < 0; i++) {
          nvn_gpu_drawable q; memset(&q, 0, sizeof q);
          nvn_gpu_rc ra = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &q, &e);
          if (ra.code == NVN_GPU_E_OUTDATED) { outdated_at = i; break; }
          if (!OK(ra)) break;
          nvn_gpu_present(sf, &q, &pd, 200 + (uint64_t)i, on_shown, NULL, &e);
      }
      CHECK(outdated_at >= 0, "C199 表面转了 90 度：期望 31 帧以内取图报过期（要重建）→ 没报");
      g_wsi_subopt = g_wsi_rotated = 0;
      r = nvn_gpu_surface_rebuild(sf, &e);
      CHECK(OK(r), "C199 rebuild：%d %s", r.code, e.why);
    }

    nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    r = nvn_gpu_surface_detach(sf, 5000000000ull, &e);
    CHECK(OK(r), "C71 detach：%d %s", r.code, e.why);
    r = nvn_gpu_surface_attach((void *)1, &sf, &e);          // 撤掉以后可以再接
    CHECK(OK(r) && nvn_gpu_surface_gen(sf) == 1, "C71 再 attach：%d %s", r.code, e.why);
    // 不 detach 直接 close：close 负责收掉还接着的面（由 main 里紧跟的 close 验）
}

// C697: the fixed-eight implementation must fail before we submit an out-of-range image.
static void c697_swapchain_images(void) {
    nvn_gpu_err e = {0}; nvn_gpu_surface sf = NULL;
    g_w697_count = 14; g_w697_queries = g_w697_fetches = 0;
    g_w697_created = g_w697_destroyed = g_w697_sc_destroyed = 0;
    g_w697_incomplete = 1; // valid count query followed by incomplete fill: must re-enumerate.
    nvn_gpu_rc r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(OK(r) && sf, "C697 attach 14 images/retry: %d %s", r.code, e.why);
    CHECK(g_w697_queries >= 2 && g_w697_fetches >= 2, "C697 complete enumeration: queries=%u fetches=%u", g_w697_queries, g_w697_fetches);
    CHECK(g_w697_created == 29, "C697 14 rendered + 15 acquire semaphores: got %u", g_w697_created);
    // Do not let the negative control feed an out-of-bounds handle to the real driver.
    if (!sf || g_w697_created != 29 || g_w697_queries < 2 || g_w697_fetches < 2) goto finish;
    nvn_gpu_tex tex = make_tex(64, 36, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex);
    nvn_gpu_present_desc pd = {0}; pd.src = view;
    for (int k = 0; k < 16; k++) {
        nvn_gpu_drawable dr = {0};
        r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
        CHECK(OK(r), "C697 acquire virtual index >=8 frame %d: %d %s", k, r.code, e.why);
        if (!OK(r)) break;
        CHECK(OK(nvn_gpu_present(sf, &dr, &pd, 697 + (uint64_t)k, NULL, NULL, &e)), "C697 present frame %d: %s", k, e.why);
    }
    // Exercise arrays/pending state beyond the old uint32_t bit mask.
    g_w697_count = 40;
    r = nvn_gpu_surface_rebuild(sf, &e);
    CHECK(OK(r), "C697 rebuild with 40 images: %d %s", r.code, e.why);
    if (OK(r)) {
        for (int k = 0; k < 35; k++) {
            nvn_gpu_drawable dr = {0};
            r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
            CHECK(OK(r), "C697 >32 acquire %d: %s", k, e.why);
            if (!OK(r)) break;
            CHECK(OK(nvn_gpu_present(sf, &dr, &pd, 797 + (uint64_t)k, NULL, NULL, &e)), "C697 >32 present %d: %s", k, e.why);
        }
        // Driver has signalled acquire slot 35, but returns the first invalid image index.
        g_w697_invalid = 1;
        nvn_gpu_drawable bad = {0};
        r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &bad, &e);
        CHECK(!OK(r) && bad.token == 0, "C697 invalid index must fail with empty drawable: rc=%d token=%llu", r.code, (unsigned long long)bad.token);
        g_w697_invalid = 0;
    }
    // Image and semaphore slots beyond 255 must not alias the old 8-bit token fields.
    g_w697_count = 264; g_w697_base = 256;
    r = nvn_gpu_surface_rebuild(sf, &e);
    CHECK(OK(r), "C697 rebuild 264 images: %s", e.why);
    if (OK(r)) {
        for (int k = 0; k < 258; k++) {
            nvn_gpu_drawable dr = {0}, held = {0};
            r = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e);
            CHECK(OK(r), "C697 32-bit token acquire %d: %s", k, e.why);
            if (!OK(r)) break;
            nvn_gpu_drawable_release(sf, &dr);
            CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &held, &e)) && held.token == dr.token,
                  "C697 32-bit token release/reacquire %d", k);
            CHECK(OK(nvn_gpu_present(sf, &held, &pd, 897 + (uint64_t)k, NULL, NULL, &e)), "C697 32-bit token present %d: %s", k, e.why);
        }
    }
    nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
finish:
    if (sf) CHECK(OK(nvn_gpu_surface_detach(sf, 5000000000ull, &e)), "C697 detach: %s", e.why);
    CHECK(g_w697_created == g_w697_destroyed, "C697 semaphore cleanup: %u created / %u destroyed", g_w697_created, g_w697_destroyed);
    // Permanently incomplete enumeration must fail cleanly, never publish a partial swapchain.
    g_w697_count = 14; g_w697_incomplete = -1; sf = NULL;
    unsigned destroyed_before = g_w697_sc_destroyed;
    r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(!OK(r) && !sf, "C697 perpetual VK_INCOMPLETE must fail: %d", r.code);
    if (sf) nvn_gpu_surface_detach(sf, 5000000000ull, &e);
    CHECK(g_w697_sc_destroyed > destroyed_before, "C697 failed enumeration destroyed swapchain");
    CHECK(g_w697_created == g_w697_destroyed, "C697 failure semaphore cleanup");
    // Failure after some semaphores exist must free every successfully-created handle.
    g_w697_incomplete = 0; g_w697_fail_sem = 5; g_w697_base = 8; sf = NULL;
    r = nvn_gpu_surface_attach((void *)1, &sf, &e);
    CHECK(!OK(r) && !sf, "C697 partial semaphore creation must fail");
    if (sf) nvn_gpu_surface_detach(sf, 5000000000ull, &e);
    CHECK(g_w697_created == g_w697_destroyed, "C697 partial semaphore cleanup: %u / %u", g_w697_created, g_w697_destroyed);
    g_w697_fail_sem = 0;
    g_w697_count = 0; g_w697_incomplete = 0;
    printf("C697 synthetic swapchain enumeration/index/cleanup complete; failures=%d\n", g_fail);
}

// C73：NVN 窗口原点在左上时视口翻转。同一张 2x2 贴图、同一个三角形，读回的图应当上下对调（期望在 tri_impl 里按行号手算）。
static void c73_origin_upper_left(const char *dir) {
    static const uint8_t texels[16] = {255, 0, 0, 255,   0, 255, 0, 255,
                                       0, 0, 255, 255,   255, 255, 255, 255};
    g_tri_origin_ul = 1;
    tri_impl(dir, NVN_FORMAT_RGBA8, texels, 4, 8, 255);
    g_tri_origin_ul = 0;
}

// C72：块压缩贴图上传（一行是一排块）。BC1 一个 4x4 块：两个端点色都是纯红（565 的 0xF800）、索引全 0 → 解出来每个纹素都是不透明纯红。
// 2x2 的贴图只占这一个块的左上角。设备不支持 BC 直接采样时这条由 compute 解码路线负责，本用例只在原生支持时跑。
static void c72_bc1_upload(const char *dir, int required) {
    if (required) {
        nvn_gpu_tex_desc td = {0}; nvn_gpu_tex probe = NULL; nvn_gpu_err e = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA_DXT1;
        td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
        td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
        nvn_gpu_rc r = nvn_gpu_tex_create(&td, &probe, &e);
        CHECK(OK(r) && probe, "C72 compute：BC1 建图应成功，得到 %d %s", r.code, e.why);
        if (probe) nvn_gpu_tex_release(probe);
    }
    if (!(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE)) {
        CHECK(!required, "C72 compute：BC1 解码路线应可采样，实际未报告能力");
        if (!required) printf("C72 跳过：本设备档 BC1 不可采样\n");
        return;
    }
    printf("C72 执行：%s\n", required ? "compute" : "native");
    static const uint8_t block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    static const uint8_t red[4] = {255, 0, 0, 255};
    g_tri_expect = red;
    tri_impl(dir, NVN_FORMAT_RGBA_DXT1, block, 4, 8, 255);
    g_tri_expect = NULL;
}

// C192：压缩块行带 8 字节填充；两次上传之间抓一次快照，再检查 abort 不改变结果。
static void c192_bc_order(void) {
    static const uint8_t blocks[48] = {
        0x00,0xf8,0x00,0xf8,0,0,0,0, 0xe0,0x07,0xe0,0x07,0,0,0,0, 0,0,0,0,0,0,0,0,
        0x1f,0x00,0x1f,0x00,0,0,0,0, 0xff,0xff,0xff,0xff,0,0,0,0, 0,0,0,0,0,0,0,0
    };
    static const uint8_t blue[8] = {0x1f,0,0x1f,0,0,0,0,0};
    static const uint8_t colors[4][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
    nvn_gpu_tex bc = make_tex(8, 8, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex a = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex b = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(8 * 8 * 4 * 3, NVN_GPU_MEM_SHARED, NULL);
    if (!bc || !a || !b || !rb) return;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blocks, 24, sizeof blocks);
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, a, 0, 0, NULL);
    NVNcopyRegion region = {4,0,0,4,4,1};
    nvn_gpu_tex_upload(c, bc, 0, 0, &region, blue, 8, sizeof blue);
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, b, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, a, 0, 0, NULL, rb, 0, 32, 256);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 256, 32, 256);
    nvn_gpu_err e; nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(OK(rc), "C192 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(6, 5000000000ull, &e)), "C192 wait：%s", e.why);
    c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, &region, blocks, 8, 8);
    nvn_gpu_cmd_abort(c);
    c = begin_cmd();
    nvn_gpu_copy_tex(c, bc, 0, 0, NULL, b, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 512, 32, 256);
    rc = nvn_gpu_cmd_commit(c, 7, on_done, NULL, &e);
    CHECK(OK(rc), "C192 abort 后 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(7, 5000000000ull, &e)), "C192 abort 后 wait：%s", e.why);
    for (int image = 0; image < 3; image++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        int k = (y / 4) * 2 + x / 4; if (image && k == 1) k = 2;
        CHECK(memcmp(buf_cpu(rb) + image * 256 + (y * 8 + x) * 4, colors[k], 4) == 0,
              "C192 快照 %d 坐标 %d,%d 应为颜色 %d", image, x, y, k);
    }
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(a); nvn_gpu_tex_release(b); nvn_gpu_buf_release(rb, NULL, NULL);
}

static void c193_bc_readback_rejected(void) {
    nvn_gpu_tex bc = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    if (!bc || !rb) return;
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e;
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, NULL, rb, 0, 8, 8);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C193 解码贴图按压缩块回读应 UNSUPPORTED，实得 %d", r.code);
    nvn_gpu_tex_release(bc); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C196：上传中的解码未证明完成时，段失败/abort 和 close 超时都不能释放其资源；完成后 close 可重试。
static void c196_bc_close_timeout(void) {
    static const uint8_t red[8] = {0,0xf8,0,0xf8,0,0,0,0};
    nvn_gpu_tex bc = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    if (!bc) return;
    nvn_gpu_cmd c = begin_cmd();
    g_bc_inject_wait = 1; g_bc_unproven = g_bc_early_destroy = 0; g_bc_injected_fence = VK_NULL_HANDLE;
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, red, 8, 8);
    CHECK(g_bc_injected_fence && !g_bc_early_destroy, "C196 解码超时应保活，提前销毁=%d", g_bc_early_destroy);
    nvn_gpu_cmd_abort(c);
    g_bc_inject_wait = 1;
    nvn_gpu_err e; nvn_gpu_rc r = nvn_gpu_close(5000000000ull, &e);
    CHECK(r.code == NVN_GPU_E_TIMEOUT && !g_bc_early_destroy, "C196 close 未证明完成应超时并保活：%d，提前销毁=%d", r.code, g_bc_early_destroy);
    if (g_bc_injected_fence) {
        VkResult vr = vkWaitForFences(g_dev.device, 1, &g_bc_injected_fence, VK_TRUE, 5000000000ull);
        CHECK(vr == VK_SUCCESS, "C196 真实 fence 应完成：%d", vr);
    }
    g_bc_unproven = g_bc_inject_wait = 0;
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "C196 完成后 close 重试：%d %s", r.code, e.why);
    printf("C196 close 超时保活与重试已执行\n");
}

// C194：8×8 立方体的一级 mip 是 4×4，两个面分别为纯蓝、纯绿。
static void c194_bc_cube_mips(void) {
    static const uint8_t blue[8] = {0x1f,0,0x1f,0,0,0,0,0};
    static const uint8_t green[8] = {0xe0,7,0xe0,7,0,0,0,0};
    static const uint8_t expected[2][4] = {{0,0,255,255}, {0,255,0,255}};
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_CUBEMAP; td.d.format = NVN_FORMAT_RGBA_DXT1;
    td.d.width = td.d.height = 8; td.d.depth = 1; td.d.levels = 2; td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD | NVN_GPU_TEX_VIEW_FMT;
    nvn_gpu_tex bc = NULL; nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &bc, &e);
    CHECK(OK(rc) && bc, "C194 立方体建图：%d %s", rc.code, e.why); if (!bc) return;
    view_t same = {0}; same.set_format = 1; same.format = NVN_FORMAT_RGBA_DXT1;
    nvn_gpu_view v = NULL; rc = nvn_gpu_view_create(bc, &same, &v, &e);
    CHECK(OK(rc) && v, "C194 同格式显式视图：%d %s", rc.code, e.why);
    nvn_gpu_tex out = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(128, NVN_GPU_MEM_SHARED, NULL);
    if (!out || !rb) return;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 1, 2, NULL, blue, 8, 8);
    nvn_gpu_tex_upload(c, bc, 1, 4, NULL, green, 8, 8);
    nvn_gpu_copy_tex(c, bc, 1, 2, NULL, out, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_copy_tex(c, bc, 1, 4, NULL, out, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 64, 16, 64);
    rc = nvn_gpu_cmd_commit(c, 8, on_done, NULL, &e); CHECK(OK(rc), "C194 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(8, 5000000000ull, &e)), "C194 wait：%s", e.why);
    for (int i = 0; i < 32; i++) CHECK(!memcmp(buf_cpu(rb) + i * 4, expected[i / 16], 4), "C194 面 %d 像素 %d", i / 16, i % 16);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(bc); nvn_gpu_tex_release(out); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C30：RGB8 上传展开成 RGBA8 后经三角形采样读回；期望按 2x2 最近邻 + a=0xFF 手算。
static void t30_rgb8_sampled(const char *dir) {
    static const uint8_t texels[12] = {255, 0, 0,   0, 255, 0,
                                       0, 0, 255,   255, 255, 255};
    tri_impl(dir, NVN_FORMAT_RGB8, texels, 3, 6, 255);
}

// close 义务：在途段先 done 再返回；返回后零回调、新工作 STOPPED。
static void t10_close_inflight(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x77777777u);
    nvn_gpu_err e;
    int done_before = atomic_load(&g_ndone);   // 必须在 commit 前取：worker 可能先完成
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "close：%d %s", r.code, e.why);
    CHECK(atomic_load(&g_ndone) == done_before + 1, "close 返回前在途段没有恰好 done 一次");
    sleep_ms(50);
    CHECK(atomic_load(&g_ndone) == done_before + 1, "close 返回后仍有回调");
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_STOPPED, "close 后 cmd_begin 应 STOPPED");
}


// 真实 fence 失败路径：F 取最小、后续段 FAILED、完成值停在 F 之前。
static void t16_fence_failure(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf c = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, a, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, c, 0, 16, 0x33333333u);
    int base = atomic_load(&g_ndone);
    nvn_gpu_vk_test_fail_fence(2);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c1, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_cmd_commit(c2, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_cmd_commit(c3, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
    CHECK(OK(r), "wait(1)：%d %s", r.code, e.why);
    CHECK(nvn_gpu_fence_wait(2, 5000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(2) 应 FAILED");
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    CHECK(nvn_gpu_fence_done() == 1, "完成值应停在 1，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_wait(3, 2000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(3) 应 FAILED");
    const struct done_rec *d2 = wait_signal(2, base, 5000), *d3 = wait_signal(3, base, 5000);
    CHECK(d2 && d2->status == NVN_GPU_DONE_FAILED, "段 2 回调不是 FAILED");
    CHECK(d3 && d3->status == NVN_GPU_DONE_FAILED, "段 3 回调不是 FAILED");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    nvn_gpu_buf_release(c, NULL, NULL);
}

// C13 TIMEOUT：close(0) 在途段没完成 → TIMEOUT 且资源保留；再 close 完成，done 恰好一次。
static void t17_close_timeout(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x55555555u);
    int base = atomic_load(&g_ndone);
    nvn_gpu_vk_test_delay_segment(1, 300);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_close(0, &e);
    CHECK(r.code == NVN_GPU_E_TIMEOUT, "close(0) 应 TIMEOUT，实为 %d", r.code);
    CHECK(atomic_load(&g_ndone) == base, "TIMEOUT 前不应有 done");
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "第二次 close：%d %s", r.code, e.why);
    CHECK(atomic_load(&g_ndone) == base + 1, "close 完成后 done 应恰好 +1");
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_STOPPED, "close 后 cmd_begin 应 STOPPED");
}


// 组合采样管线的公共搭建（C25）：FS 用 sampler2D，布局里 cls = TEX_SAMP。
static nvn_gpu_pipe make_combined_pipe(const char *dir, nvn_gpu_shader *vs_out, nvn_gpu_shader *fs_out) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri_combined.frag.spv", dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fs, "读不到组合采样 SPIR-V");
    if (!vs || !fs) { free(vs); free(fs); return NULL; }
    nvn_gpu_layout_ent ent;
    memset(&ent, 0, sizeof ent);
    ent.set = 0; ent.cls = NVN_GPU_RES_TEX_SAMP; ent.access = NVN_GPU_ACC_R;
    ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = &ent; fl.nent = 1;
    nvn_gpu_layout vl;
    memset(&vl, 0, sizeof vl);
    nvn_gpu_err e;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.stage = NVN_GPU_SH_VS; src.blob = vs; src.bytes = vsz; src.layout = &vl;
    CHECK(OK(nvn_gpu_shader_load(&src, vs_out, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    src.stage = NVN_GPU_SH_FS; src.blob = fs; src.bytes = fsz; src.layout = &fl;
    CHECK(OK(nvn_gpu_shader_load(&src, fs_out, &e)), "FS 装载：%d %s", e.rc.code, e.why);
    free(vs);
    free(fs);
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = *vs_out;
    pd.st[NVN_GPU_SH_FS].sh = *fs_out;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "pipe_get：%d %s", e.rc.code, e.why);
    return pipe;
}

static void check_triangle_pixels(nvn_gpu_buf rb, const uint8_t texels[16]) {
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float u = (x + 0.5f) / 64.0f, v = (y + 0.5f) / 64.0f;
            int tx = (int)(u * 2.0f); if (tx > 1) tx = 1;
            int ty = (int)(v * 2.0f); if (ty > 1) ty = 1;
            const uint8_t *want = texels + (ty * 2 + tx) * 4;
            const uint8_t *got = buf_cpu(rb) + (y * 64 + x) * 4;
            if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2] || got[3] != want[3]) {
                g_fail++;
                printf("FAIL %s:%d: 三角像素 (%d,%d) = %02x%02x%02x%02x，期望 %02x%02x%02x%02x\n",
                       __FILE__, __LINE__, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                return;
            }
        }
    }
}

// C25 正例：cls = TEX_SAMP，u.ts 同时给 view 与 samp，采样结果与分开绑定一致。
static void t18_combined_sampler(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_combined_pipe(dir, &vs, &fs);
    if (!pipe) return;
    const float verts[3][4] = {{-1.0f, -1.0f, 0.0f, 0.0f}, {3.0f, -1.0f, 2.0f, 0.0f}, {-1.0f, 3.0f, 0.0f, 2.0f}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    nvn_gpu_tex tex = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view tview = make_view(tex);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "samp_create：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view oview = make_view(out);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, texels, 8, 16);
    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = tview; b.u.ts.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_samp_release(samp);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "wait 1：%d %s", e.rc.code, e.why);
    check_triangle_pixels(rb, texels);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
}

// C26：signal 违约（重复 5 / 0）→ E_ARG、F=6、各回调一次 FAILED；之后 7 也 FAILED。
static void t19_signal_contract(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_err e;
    nvn_gpu_cmd c0 = begin_cmd();
    nvn_gpu_fill_buf(c0, b, 0, 16, 0xAABBCCDDu);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c0, 5, on_done, NULL, &e);
    CHECK(OK(r), "合法 commit 5：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(5, 5000000000ull, &e)), "wait 5：%d %s", e.rc.code, e.why);
    int base = atomic_load(&g_ndone);
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, b, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, b, 0, 16, 0x33333333u);
    r = nvn_gpu_cmd_commit(c1, 5, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "重复 signal 5 应 E_ARG，实为 %d", r.code);
    r = nvn_gpu_cmd_commit(c2, 0, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "signal 0 应 E_ARG，实为 %d", r.code);
    r = nvn_gpu_cmd_commit(c3, 7, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "失败终态后 signal 7 应 E_FAILED，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 6, "F 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    CHECK(nvn_gpu_fence_done() == 5, "完成值应停在 5，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_wait(6, 2000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(6) 应 FAILED");
    CHECK(wait_until(&g_ndone, base + 3, 5000), "应有 3 次 done（三次被拒段）");
    int nfail = 0, zero_seen = 0;
    for (int i = base; i < base + 3 && i < 64; i++) {
        if (g_done[i].status == NVN_GPU_DONE_FAILED) nfail++;
        if (g_done[i].signal == 0 && g_done[i].status == NVN_GPU_DONE_FAILED) zero_seen = 1;
    }
    CHECK(nfail == 3, "FAILED 回调应有 3 次，实为 %d", nfail);
    CHECK(zero_seen, "signal 0 的被拒段没有 FAILED 回调");
    uint32_t word;
    memcpy(&word, buf_cpu(b), 4);
    CHECK(word == 0xAABBCCDDu, "被拒段改写了缓冲：0x%08x", word);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// C25 反例：TEX_SAMP 缺 samp → 粘滞 E_LAYOUT，提交返回 E_LAYOUT 并进入失败终态。
static void t20_combined_missing_samp(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_combined_pipe(dir, &vs, &fs);
    if (!pipe) return;
    nvn_gpu_tex out = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view oview = make_view(out);
    nvn_gpu_tex tex = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view tview = make_view(tex);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 8; pass.h = 8; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = tview; b.u.ts.samp = NULL;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_LAYOUT, "缺 samp 的 TEX_SAMP 应 E_LAYOUT，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
}



// C27：clear_view 的 is_int 必须与贴图格式一致（中立格式表驱动）。
static void t21_clear_int(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGBA8UI, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    const uint32_t vals[4] = {1, 2, 3, 4};
    nvn_gpu_clear_view(c, v, NULL, vals, 1);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "整数 clear commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 2000000000ull, &e)), "整数 clear wait：%d %s", e.rc.code, e.why);
    for (int i = 0; i < 16; i++) {
        uint32_t word;
        memcpy(&word, buf_cpu(b) + i * 4, 4);
        CHECK(word == 0x04030201u, "整数像素 %d = 0x%08x，期望 0x04030201", i, word);
    }
    // 反例：浮点颜色贴图却用 is_int=1，必须 E_ARG 且不执行
    nvn_gpu_tex ft = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view fv = make_view(ft);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_clear_view(c2, fv, NULL, vals, 1);
    r = nvn_gpu_cmd_commit(c2, 2, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "浮点贴图用 is_int 应 E_ARG，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(fv);
    nvn_gpu_tex_release(ft);
    nvn_gpu_buf_release(b, NULL, NULL);
}


// C27b：视图覆盖格式的 clear 明确不支持（Vulkan 按 image 格式解释 clear 值）。
static void t22_clear_view_fmt(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_VIEW_FMT);
    view_t vd;
    memset(&vd, 0, sizeof vd);
    vd.set_format = 1;
    vd.format = NVN_FORMAT_RGBA8UI;
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create(t, &vd, &v, &e);
    CHECK(OK(r), "覆盖格式视图创建：%d %s", r.code, e.why);
    nvn_gpu_cmd c = begin_cmd();
    const uint32_t vals[4] = {1, 2, 3, 4};
    nvn_gpu_clear_view(c, v, NULL, vals, 1);
    r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "覆盖格式 clear 应 E_UNSUPPORTED，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
}


// C76：单次分配上限必须来自 maxMemoryAllocationSize，不能与 maxStorageBufferRange 共用同一个值。
// 以前两者相等（都是 128 MiB）：回放核心把内存池整池镜像成一块 CPU 可见缓冲，任何大于 SSBO 范围上限的池
// 都建不出镜像，池里的顶点 / 索引取不到，真机大地图进游戏后几何大面积不画（只剩 UI 与少数小池物件）。
// 这里用一个「超过 SSBO 范围上限、仍在分配上限内」的缓冲做判据：修前 E_ARG，修后成功。
static void t76_buf_create_alloc_cap(void) {
    nvn_gpu_caps cp;
    nvn_gpu_caps_get(&cp);
    // 直接对设备真值：单次分配上限必须是 maxMemoryAllocationSize（没有该项时才退回 SSBO 范围上限）。
    // 这一条不依赖设备上两个上限是否相等，能直接抓住「拿 maxStorageBufferRange 当分配上限」的回归。
    VkPhysicalDeviceMaintenance3Properties m3; memset(&m3, 0, sizeof m3);
    m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
    VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof p2);
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &m3;
    vkGetPhysicalDeviceProperties2(g_dev.physical, &p2);
    uint64_t expect = m3.maxMemoryAllocationSize ? (uint64_t)m3.maxMemoryAllocationSize
                                                 : (uint64_t)g_dev.properties.limits.maxStorageBufferRange;
    CHECK(cp.max_buf_bytes == expect, "C76: caps.max_buf_bytes=%llu 应等于设备 maxMemoryAllocationSize %llu（SSBO 范围上限 %llu 是另一项）",
          (unsigned long long)cp.max_buf_bytes, (unsigned long long)expect, (unsigned long long)cp.max_ssbo_range);
    if (cp.max_buf_bytes <= cp.max_ssbo_range) {
        printf("C76 跳过：本设备 max_buf_bytes(%llu) <= max_ssbo_range(%llu)，没有可判别的区间\n",
               (unsigned long long)cp.max_buf_bytes, (unsigned long long)cp.max_ssbo_range);
        return;
    }
    uint64_t n = cp.max_ssbo_range + (1u << 20);
    if (n > cp.max_buf_bytes) n = cp.max_buf_bytes;
    nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_buf b = NULL;
    nvn_gpu_rc r = nvn_gpu_buf_create(n, NVN_GPU_MEM_SHARED, NULL, &b, &e);
    CHECK(nvn_gpu_ok(r) && b, "C76: 建 %llu 字节缓冲（> max_ssbo_range %llu，<= 分配上限 %llu）失败：%d %s",
          (unsigned long long)n, (unsigned long long)cp.max_ssbo_range, (unsigned long long)cp.max_buf_bytes, r.code, e.why);
    if (nvn_gpu_ok(r)) printf("C76: 分配上限 %llu > SSBO 范围上限 %llu，建 %llu 字节缓冲成功\n",
                              (unsigned long long)cp.max_buf_bytes, (unsigned long long)cp.max_ssbo_range, (unsigned long long)n);
    if (b) nvn_gpu_buf_release(b, NULL, NULL);
}

// C29：Vulkan 映射按中立表重定向：展开格式必须映射到目标格式的 VkFormat（纯格式检查，不用 GPU）。
static void t29_format_expand(void) {
    for (uint32_t f = 0; f <= 0xFF; f++) {
        nvn_fmt_info i;
        if (!nvn_fmt_info_of(f, &i) || !i.expand) continue;
        VkFormat a = nvn_vk_tex_format(f), b = nvn_vk_tex_format(i.expand);
        CHECK(a == b, "0x%02X 贴图映射 %d != 目标 0x%02X 的 %d", f, (int)a, i.expand, (int)b);
    }
    // 顶点 / 存储映射不能被展开改掉
    CHECK(nvn_vk_format(0x22) == VK_FORMAT_R32G32B32_SFLOAT, "RGB32F 顶点映射被改：%d", (int)nvn_vk_format(0x22));
    CHECK(nvn_vk_format(0x19) == VK_FORMAT_R8G8B8_UNORM, "RGB8 存储映射被改：%d", (int)nvn_vk_format(0x19));
    CHECK(nvn_vk_format(0x51) == VK_FORMAT_R8_USCALED, "R8_UI2F 顶点取样映射不是 USCALED：%d", (int)nvn_vk_format(0x51));
    CHECK(nvn_vk_tex_format(0x51) == VK_FORMAT_R16_SFLOAT, "R8_UI2F 贴图映射不是 R16F：%d", (int)nvn_vk_tex_format(0x51));
    CHECK(nvn_vk_tex_format(0x19) == VK_FORMAT_R8G8B8A8_UNORM, "RGB8 贴图映射不是 RGBA8：%d", (int)nvn_vk_tex_format(0x19));
}

// C28：展开格式的回读明确不支持（不做隐式收缩）。
static void t28_rgb8_upload(void) {
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGB8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    const uint8_t px[12] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    NVNcopyRegion r = {0, 0, 0, 2, 2, 1};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, &r, px, 6, 12);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 8, 16);
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_UNSUPPORTED, "展开格式回读应 E_UNSUPPORTED，实为 %d", rc.code);
    CHECK(nvn_gpu_fence_failed() == 6, "F 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}


// C31：顶点路径证据——RGB32F 位置属性不能被展开成 RGBA32F（w 会读成下一个属性的字节），
// RGBX8 也不能被展开成 RGBA8（多读 1 字节）。两者都画一个实色三角形并回读。
static void t31_vertex_formats(const char *dir) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri3.vert.spv", dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri_solid.frag.spv", dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fs, "读不到顶点格式用例 SPIR-V（%s）", dir);
    if (!vs || !fs) { free(vs); free(fs); return; }

    nvn_gpu_layout lay;
    memset(&lay, 0, sizeof lay);
    nvn_gpu_shader vs_sh = NULL, fs_sh = NULL;
    nvn_gpu_err e;
    nvn_gpu_shader_src ssrc;
    memset(&ssrc, 0, sizeof ssrc);
    ssrc.stage = NVN_GPU_SH_VS; ssrc.kind = NVN_GPU_BLOB_SPIRV; ssrc.entry = "main";
    ssrc.blob = vs; ssrc.bytes = vsz; ssrc.layout = &lay;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &vs_sh, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    ssrc.stage = NVN_GPU_SH_FS; ssrc.blob = fs; ssrc.bytes = fsz;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &fs_sh, &e)), "FS 装载：%d %s", e.rc.code, e.why);

    const float verts32[3][4] = {{-1.0f, -1.0f, 0.0f, 99.0f},
                                 { 3.0f, -1.0f, 0.0f, 99.0f},
                                 {-1.0f,  3.0f, 0.0f, 99.0f}};
    const uint8_t vertsx[3][4] = {{0, 0, 0, 0}, {255, 0, 0, 0}, {0, 255, 0, 0}};
    nvn_gpu_buf vb32 = make_buf(sizeof verts32, NVN_GPU_MEM_SHARED, verts32);
    nvn_gpu_buf vbx = make_buf(sizeof vertsx, NVN_GPU_MEM_SHARED, vertsx);
    uint8_t zero[8 * 8 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_tex out = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);

    for (int which = 0; which < 2; which++) {
        nvn_gpu_pipe_desc pd;
        memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
        pd.st[NVN_GPU_SH_VS].sh = vs_sh;
        pd.st[NVN_GPU_SH_FS].sh = fs_sh;
        pd.cfmt[0] = NVN_FORMAT_RGBA8;
        pd.samples = 1;
        pd.prim = 4;
        pd.chmask.mask = 0xF;
        pd.attr[0].fmt = which ? NVN_FORMAT_RGBX8 : NVN_FORMAT_RGB32F;
        pd.attr[0].offset = 0;
        pd.attr[0].stream = 0;
        pd.stream[0].stride = which ? 4 : 16;
        nvn_gpu_pipe pipe = NULL;
        nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
        CHECK(OK(r), "顶点格式 pipe_get：%d %s", r.code, e.why);
        if (!OK(r)) continue;
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc pass;
        memset(&pass, 0, sizeof pass);
        pass.ncolor = 1;
        pass.color[0].view = ov;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[3] = 1.0f;
        pass.w = 8; pass.h = 8; pass.layers = 1;
        nvn_gpu_pass_begin(c, &pass);
        nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_dyn dyn;
        memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
        dyn.nvp = 1;
        dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 8; dyn.vp[0][3] = 8; dyn.vp[0][5] = 1;
        dyn.has_sc = 1; dyn.sc[2] = 8; dyn.sc[3] = 8;
        nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, which ? vbx : vb32, 0);
        nvn_gpu_draw_args da;
        memset(&da, 0, sizeof da);
        da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 8 * 4, 8 * 8 * 4);
        r = nvn_gpu_cmd_commit(c, which + 1, on_done, NULL, &e);
        CHECK(OK(r), "顶点格式 commit %d：%d %s", which + 1, r.code, e.why);
        CHECK(OK(nvn_gpu_fence_wait(which + 1, 2000000000ull, &e)), "顶点格式 wait %d：%d %s", which + 1, e.rc.code, e.why);
        int red = 0;
        for (int i = 0; i < 8 * 8; i++) {
            const uint8_t *p = buf_cpu(rb) + i * 4;
            if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) red++;
        }
        if (which == 0) CHECK(red == 64, "RGB32F 顶点全屏红像素 %d / 64（w 被读成 pad）", red);
        else CHECK(red > 0, "RGBX8 顶点没有红像素（多读字节 / w 错）");
    }
    nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb32, NULL, NULL);
    nvn_gpu_buf_release(vbx, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs_sh);
    nvn_gpu_shader_release(fs_sh);
    free(vs);
    free(fs);
}


// C32：整级展开的源越界必须 E_ARG（RGB8 4×4 只给 6 字节行宽 / 12 字节总量）。
static void t32_expand_bounds(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGB8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    const uint8_t px[12] = {0};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, px, 6, 12);
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_ARG, "展开源越界应 E_ARG，实为 %d", rc.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_tex_release(t);
}


// ---- C33–C35 公共绘制助手：只在已开始的通道里画，不管理 pass ----
static int load_shader_pair(const char *dir, const char *vs_name, const char *fs_name,
                            const nvn_gpu_layout *vlay, const nvn_gpu_layout *flay,
                            nvn_gpu_shader *vs, nvn_gpu_shader *fs, void **vsmem, void **fsmem) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/%s.spv", dir, vs_name);
    *vsmem = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/%s.spv", dir, fs_name);
    *fsmem = read_file(path, &fsz);
    if (!*vsmem || !*fsmem) return 0;
    nvn_gpu_layout empty;
    memset(&empty, 0, sizeof empty);
    nvn_gpu_err e;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.stage = NVN_GPU_SH_VS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = *vsmem; src.bytes = vsz; src.layout = vlay ? vlay : &empty;
    if (!OK(nvn_gpu_shader_load(&src, vs, &e))) return 0;
    src.stage = NVN_GPU_SH_FS; src.blob = *fsmem; src.bytes = fsz; src.layout = flay ? flay : &empty;
    if (!OK(nvn_gpu_shader_load(&src, fs, &e))) return 0;
    return 1;
}

static void fs_draw_tex_frag(nvn_gpu_cmd c, nvn_gpu_view view, nvn_gpu_samp samp, const char *dir, const char *frag) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl;
    memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "tri.vert", frag, &vl, &fl, &vs, &fs, &vsm, &fsm)) { CHECK(0, "读不到 %s 着色器", frag); return; }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C33 pipe：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1, -1, 0, 0}, {3, -1, 2, 0}, {-1, 3, 0, 2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2];
    memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = view;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vsm); free(fsm);
}

static void fs_draw_solid(nvn_gpu_cmd c, const char *dir, const char *frag, const int sc[4]) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "tri3.vert", frag, NULL, NULL, &vs, &fs, &vsm, &fsm)) { CHECK(0, "读不到 %s 着色器", frag); return; }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C34 pipe：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1;
    memcpy(dyn.sc, sc, sizeof dyn.sc);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vsm); free(fsm);
}

static void expect_rect(const uint8_t *rb, int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b, const char *what) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            const uint8_t *p = rb + (y * 64 + x) * 4;
            if (p[0] != r || p[1] != g || p[2] != b || p[3] != 255) {
                CHECK(0, "%s 像素 (%d,%d)=%02x%02x%02x%02x，期望 %02x%02x%02xff", what, x, y, p[0], p[1], p[2], p[3], r, g, b);
                return;
            }
        }
    }
}


// 非恒等采样映射的图仍能做附件：先按原始颜色清两次（第二次 LOAD），
// 再从同一采样视图读进另一图；原始读回不换通道，采样读回按映射换通道。
// 各变体独立会话：修前 sticky 会令该会话的时间线失败，不能污染下一用例。
static atomic_int g_attach_done;
static char g_attach_why[192];
static void attach_swizzle_done(void *user, const nvn_gpu_done *done) {
    snprintf(g_attach_why, sizeof g_attach_why, "%s", done->why ? done->why : "");
    on_done(user, done);
    atomic_store_explicit(&g_attach_done, 1, memory_order_release);
}
static void attach_swizzle_case(const char *dir, int explicit_view) {
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 64; td.d.height = 64; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    td.swz = 4 | 3 << 3 | 2 << 6 | 1 << 9; // B,G,R,1
    nvn_gpu_tex src = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &src, &e);
    CHECK(OK(r), "attachment swizzle tex_create: %d %s", r.code, e.why);
    if (!OK(r)) return;
    view_t vd; memset(&vd, 0, sizeof vd);
    if (explicit_view) { vd.set_swizzle = 1; vd.swz[0] = 0; vd.swz[1] = 2; vd.swz[2] = 5; vd.swz[3] = 3; }
    nvn_gpu_view src_view = NULL;
    r = nvn_gpu_view_create(src, explicit_view ? &vd : NULL, &src_view, &e);
    CHECK(OK(r), "attachment swizzle view_create: %d %s", r.code, e.why);
    if (!OK(r)) { nvn_gpu_tex_release(src); return; }
    nvn_gpu_tex dst = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view dst_view = make_view(dst);
    uint8_t zero[64 * 64 * 4] = {0};
    nvn_gpu_buf raw = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf sampled = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_gpu_samp sampler = NULL;
    r = nvn_gpu_samp_create(&sp, &sampler, &e);
    CHECK(OK(r), "attachment swizzle sampler: %d %s", r.code, e.why);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = src_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = 0.2f; pass.color[0].clear.f[1] = 0.4f;
    pass.color[0].clear.f[2] = 0.6f; pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass); nvn_gpu_pass_end(c);
    pass.color[0].load = NVN_GPU_LOAD_LOAD;  // 第二次消费同一附件
    nvn_gpu_pass_begin(c, &pass); nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, src, 0, 0, NULL, raw, 0, 64 * 4, 64 * 64 * 4);
    memset(&pass, 0, sizeof pass); pass.ncolor = 1; pass.color[0].view = dst_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    fs_draw_tex_frag(c, src_view, sampler, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, sampled, 0, 64 * 4, 64 * 64 * 4);
    // 录制后立即 release：段完成前不得销毁附件或采样视图。
    nvn_gpu_view_release(src_view); nvn_gpu_tex_release(src);
    nvn_gpu_view_release(dst_view); nvn_gpu_tex_release(dst); nvn_gpu_samp_release(sampler);
    atomic_store(&g_attach_done, 0); g_attach_why[0] = 0;
    r = nvn_gpu_cmd_commit(c, 1, attach_swizzle_done, NULL, &e);
    if (!OK(r)) CHECK(wait_until(&g_attach_done, 1, 5000), "attachment swizzle failure callback missing");
    CHECK(OK(r), "attachment swizzle %s commit: %d %s", explicit_view ? "explicit" : "inherited", r.code, g_attach_why);
    if (OK(r)) {
        r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
        CHECK(OK(r), "attachment swizzle wait: %d %s", r.code, e.why);
        const uint8_t want_raw[4] = {51, 102, 153, 255};
        const uint8_t want_inherited[4] = {153, 102, 51, 255};
        const uint8_t want_explicit[4] = {0, 51, 255, 102};
        const uint8_t *want_sample = explicit_view ? want_explicit : want_inherited;
        const uint8_t *rawp = buf_cpu(raw), *samplep = buf_cpu(sampled);
        for (int i = 0; i < 64 * 64; i++) {
            if (memcmp(rawp + i * 4, want_raw, 4)) { CHECK(0, "attachment raw pixel %d differs", i); break; }
            if (memcmp(samplep + i * 4, want_sample, 4)) { CHECK(0, "attachment sampled pixel %d differs", i); break; }
        }
    }
    nvn_gpu_buf_release(raw, NULL, NULL); nvn_gpu_buf_release(sampled, NULL, NULL);
}

// 9000 的建视图崩溃参数：12 层 cube-array 图、只取第 0 层和第 0 级。
// 修前 vkCreateImageView 收到 CUBE_ARRAY + 1 层，VVL 报 viewType-02961。
static void cube_array_one_face(void) {
    CHECK(g_dev.core_features.imageCubeArray, "cube-array probe requires imageCubeArray");
    if (!g_dev.core_features.imageCubeArray) return;
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    td.d.format = NVN_FORMAT_R11G11B10F; td.d.width = 128; td.d.height = 128;
    td.d.depth = 12; td.d.levels = 8;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT;
    td.swz = 2 | 3 << 3 | 4 << 6 | 1 << 9; // R,G,B,ONE
    nvn_gpu_tex t = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "cube-array tex: %d %s", r.code, e.why);
    if (!OK(r)) return;
    const int layers[] = {1, 6, 12};
    for (size_t i = 0; i < sizeof layers / sizeof layers[0]; i++) {
        view_t vd = {0}; vd.set_levels = 1; vd.base_level = 0; vd.nlevels = 1;
        vd.set_layers = 1; vd.min_layer = 0; vd.nlayers = (uint16_t)layers[i];
        nvn_gpu_view v = NULL;
        r = nvn_gpu_view_create(t, &vd, &v, &e);
        CHECK(OK(r), "cube-array view %d layers: %d %s", layers[i], r.code, e.why);
        if (v) nvn_gpu_view_release(v);
    }
    nvn_gpu_tex_release(t);
}

// 数组第 1 层、多 mip 视图与 sRGB 重解释均保留精确的附件子资源与格式。
static void attach_swizzle_subresource_case(const char *dir, int srgb) {
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 128; td.d.height = 128; td.d.depth = 2; td.d.levels = 2;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT;
    td.swz = 4 | 3 << 3 | 2 << 6 | 1 << 9;
    nvn_gpu_tex src = NULL; nvn_gpu_view view = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &src, &e);
    CHECK(OK(r), "attachment subresource tex: %d %s", r.code, e.why);
    if (!OK(r)) return;
    view_t vd; memset(&vd, 0, sizeof vd);
    vd.set_layers = 1; vd.min_layer = 1; vd.nlayers = 1;
    vd.set_target = 1; vd.target = NVN_TEXTURE_TARGET_2D;
    if (srgb) { vd.set_levels = 1; vd.base_level = 1; vd.nlevels = 1;
                vd.set_format = 1; vd.format = NVN_FORMAT_RGBA8_SRGB; }
    r = nvn_gpu_view_create(src, &vd, &view, &e);
    CHECK(OK(r), "attachment subresource view: %d %s", r.code, e.why);
    if (!OK(r)) { nvn_gpu_tex_release(src); return; }
    uint8_t zero[64 * 64 * 4] = {0};
    nvn_gpu_buf raw = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf sampled = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_tex dst = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view dst_view = make_view(dst);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_gpu_samp sampler = NULL;
    r = nvn_gpu_samp_create(&sp, &sampler, &e);
    CHECK(OK(r), "attachment subresource sampler: %d %s", r.code, e.why);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = view; pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = srgb ? 0.25f : 0.2f;
    pass.color[0].clear.f[1] = srgb ? 0.5f : 0.4f;
    pass.color[0].clear.f[2] = srgb ? 0.75f : 0.6f;
    pass.color[0].clear.f[3] = 1.0f; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass); nvn_gpu_pass_end(c);
    NVNcopyRegion region = {0, 0, 0, 64, 64, 1};
    nvn_gpu_copy_tex_buf(c, src, srgb ? 1 : 0, 1, &region, raw, 0, 64 * 4, 64 * 64 * 4);
    if (srgb) {
        memset(&pass, 0, sizeof pass); pass.ncolor = 1; pass.color[0].view = dst_view;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.w = 64; pass.h = 64; pass.layers = 1;
        nvn_gpu_pass_begin(c, &pass); fs_draw_tex_frag(c, view, sampler, dir, "tri.frag"); nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, sampled, 0, 64 * 4, 64 * 64 * 4);
    }
    nvn_gpu_view_release(view); nvn_gpu_tex_release(src);
    nvn_gpu_view_release(dst_view); nvn_gpu_tex_release(dst); nvn_gpu_samp_release(sampler);
    atomic_store(&g_attach_done, 0); g_attach_why[0] = 0;
    r = nvn_gpu_cmd_commit(c, 1, attach_swizzle_done, NULL, &e);
    if (!OK(r)) CHECK(wait_until(&g_attach_done, 1, 5000), "attachment subresource callback missing");
    CHECK(OK(r), "attachment subresource %s commit: %d %s", srgb ? "sRGB" : "multi-mip", r.code, g_attach_why);
    if (OK(r)) {
        r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
        CHECK(OK(r), "attachment subresource wait: %d %s", r.code, e.why);
        const uint8_t rgba_linear[4] = {51, 102, 153, 255};
        const uint8_t rgba_srgb[4] = {137, 188, 225, 255};
        const uint8_t sample_srgb[4] = {191, 128, 64, 255};
        const uint8_t *got = buf_cpu(raw), *tex = buf_cpu(sampled);
        for (int i = 0; i < 64 * 64; i++) {
            for (int ch = 0; ch < 4; ch++) {
                int want = srgb ? rgba_srgb[ch] : rgba_linear[ch];
                if (abs((int)got[i * 4 + ch] - want) > (srgb ? 1 : 0)) { CHECK(0, "attachment subresource raw pixel %d channel %d: %u vs %d", i, ch, got[i * 4 + ch], want); goto pixels_done; }
                if (srgb && abs((int)tex[i * 4 + ch] - sample_srgb[ch]) > 1) { CHECK(0, "attachment subresource sample pixel %d channel %d: %u vs %u", i, ch, tex[i * 4 + ch], sample_srgb[ch]); goto pixels_done; }
            }
        }
    pixels_done: ;
    }
    nvn_gpu_buf_release(raw, NULL, NULL); nvn_gpu_buf_release(sampled, NULL, NULL);
}

// C33：一个 cmd 两个通道；通道 1 清 T 红，通道 2 采样 T 画进 U，U 读回全红。
static void t33_two_passes(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex U = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view vT = make_view(T), vU = make_view(U);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C33 samp：%d %s", e.rc.code, e.why);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vU; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vT, samp, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, U, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "C33 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C33 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 255, 0, 0, "C33 U");
    nvn_gpu_view_release(vT); nvn_gpu_view_release(vU);
    nvn_gpu_tex_release(T); nvn_gpu_tex_release(U);
    nvn_gpu_samp_release(samp); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C34：同一目标两通道；通道 1 左半绿，通道 2 Load 右半蓝。
static void t34_two_pass_load(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view vT = make_view(T);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    const int left[4] = {0, 0, 32, 64};
    fs_draw_solid(c, dir, "solid_green.frag", left);
    nvn_gpu_pass_end(c);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    const int right[4] = {32, 0, 32, 64};
    fs_draw_solid(c, dir, "solid_blue.frag", right);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "C34 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(2, 5000000000ull, &e)), "C34 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C34 左半");
    expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C34 右半");
    nvn_gpu_view_release(vT); nvn_gpu_tex_release(T); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C35：三通道链；每通道后各自回读：T 红 → U 采样 T 红 → T 采样 U 反色成青。
static void t35_three_passes(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex U = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view vT = make_view(T), vU = make_view(U);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C35 samp：%d %s", e.rc.code, e.why);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf r1 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf r2 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf r3 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, r1, 0, 64 * 4, 64 * 64 * 4);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vU; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vT, samp, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, U, 0, 0, NULL, r2, 0, 64 * 4, 64 * 64 * 4);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vU, samp, dir, "tri_invert.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, r3, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "C35 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(3, 5000000000ull, &e)), "C35 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(r1), 0, 0, 64, 64, 255, 0, 0, "C35 通道1 T");
    expect_rect(buf_cpu(r2), 0, 0, 64, 64, 255, 0, 0, "C35 通道2 U");
    expect_rect(buf_cpu(r3), 0, 0, 64, 64, 0, 255, 255, "C35 通道3 T反色");
    nvn_gpu_view_release(vT); nvn_gpu_view_release(vU);
    nvn_gpu_tex_release(T); nvn_gpu_tex_release(U);
    nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(r1, NULL, NULL); nvn_gpu_buf_release(r2, NULL, NULL); nvn_gpu_buf_release(r3, NULL, NULL);
}


// ---- C36–C40：索引 / 实例化 / 间接绘制 ----
static nvn_gpu_pipe color_pipe(const char *dir, int inst) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, inst ? "inst.vert" : "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return NULL;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C36 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return pipe;
}

static void run_quad(const char *dir, int inst, const void *vb, size_t vbytes,
                     nvn_gpu_buf ib, uint64_t ioff, uint32_t itype, uint32_t count, uint32_t first,
                     int32_t base_vertex, uint32_t instances, uint32_t base_instance,
                     nvn_gpu_buf ind, uint64_t ind_off, uint32_t max_draws, uint32_t stride,
                     uint8_t *rb, uint64_t sig) {
    nvn_gpu_pipe pipe = color_pipe(dir, inst);
    if (!pipe) return;
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf vbuf = make_buf(vbytes, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rbuf = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, rb);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vbuf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = count; a.first = first; a.instances = instances; a.base_instance = base_instance;
    a.ibuf = ib; a.ioff = ioff; a.itype = itype; a.base_vertex = base_vertex;
    a.ind = ind; a.ind_off = ind_off; a.max_draws = max_draws; a.stride = stride;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rbuf, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(r), "C36 commit %llu：%d %s", (unsigned long long)sig, r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "C36 wait %llu：%d %s", (unsigned long long)sig, e.rc.code, e.why);
    memcpy(rb, buf_cpu(rbuf), 64 * 64 * 4);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vbuf, NULL, NULL); nvn_gpu_buf_release(rbuf, NULL, NULL);
}

static int same_img(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 64 * 64 * 4) == 0; }
static const float quad4[4][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1},
    { 1, -1, 0, 1, 0, 1, 0, 1}, { 1, 1, 0, 1, 0, 1, 0, 1}};
static const float blue4[4][8] = {
    {-1, -1, 0, 1, 0, 0, 1, 1}, {-1, 1, 0, 1, 0, 0, 1, 1},
    { 1, -1, 0, 1, 0, 0, 1, 1}, { 1, 1, 0, 1, 0, 0, 1, 1}};
static const float left2[4][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1},
    { 0.2f, -1, 0, 1, 1, 0, 0, 1}, { 0.2f, 1, 0, 1, 1, 0, 0, 1}};
static const float six6[6][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 1, 0, 1},
    { 1, -1, 0, 1, 0, 1, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1}, {1, 1, 0, 1, 0, 1, 0, 1}};

static void c36_40(const char *dir) {
    static const uint16_t idx16[6] = {0, 1, 2, 2, 1, 3};
    static const uint16_t idxneg[10] = {0, 1, 2, 2, 1, 3, 0, 0, 0, 0};
    static const uint8_t idx8[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t ind16[5] = {6, 1, 0, 0, 0};
    static const uint32_t ind6[4] = {6, 1, 0, 0};
    nvn_gpu_buf ib16 = make_buf(sizeof idx16, NVN_GPU_MEM_SHARED, idx16);
    nvn_gpu_buf ibneg = make_buf(sizeof idxneg, NVN_GPU_MEM_SHARED, idxneg);
    nvn_gpu_buf ib8 = make_buf(sizeof idx8, NVN_GPU_MEM_SHARED, idx8);
    nvn_gpu_buf ibi = make_buf(sizeof ind16, NVN_GPU_MEM_SHARED, ind16);
    nvn_gpu_buf ini = make_buf(sizeof ind6, NVN_GPU_MEM_SHARED, ind6);
    uint8_t rb[64 * 64 * 4], ref[64 * 64 * 4], other[64 * 64 * 4];
    memset(rb, 0, sizeof rb);
    run_quad(dir, 0, quad4, sizeof quad4, ib16, 0, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, ref, 1);
    expect_rect(ref, 0, 0, 32, 64, 255, 0, 0, "C36 左半");
    expect_rect(ref, 32, 0, 32, 64, 0, 255, 0, "C36 右半");
    // C36 负控：ioff 偏一个 u16
    run_quad(dir, 0, quad4, sizeof quad4, ibneg, 2, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, other, 2);
    CHECK(!same_img(ref, other), "C36 负控：ioff=2 仍与期望相同（用例没在看索引）");
    // C37 base_vertex
    float vb8[8][8];
    memcpy(vb8, blue4, sizeof blue4);
    memcpy(vb8 + 4, quad4, sizeof quad4);
    run_quad(dir, 0, vb8, sizeof vb8, ib16, 0, 1, 6, 0, 4, 1, 0, NULL, 0, 0, 0, rb, 3);
    CHECK(same_img(ref, rb), "C37 base_vertex=4 与 C36 不同");
    run_quad(dir, 0, vb8, sizeof vb8, ib16, 0, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, other, 4);
    expect_rect(other, 0, 0, 64, 64, 0, 0, 255, "C37 base_vertex=0");
    // C37 负控：用 first=4 代替 base_vertex
    run_quad(dir, 0, vb8, sizeof vb8, ibneg, 0, 1, 6, 4, 0, 1, 0, NULL, 0, 0, 0, other, 5);
    CHECK(!same_img(ref, other), "C37 负控：first=4 仍与 base_vertex=4 相同（语义没区分）");
    // C38 u8：native 与强制展开
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    run_quad(dir, 0, quad4, sizeof quad4, ib8, 0, 0, 6, 0, 0, 1, 0, NULL, 0, 0, 0, rb, 6);
    CHECK(same_img(ref, rb), "C38 u8 原生与 C36 不同");
    setenv("S2A_VK_FORCE_INDEX_U8_EXPAND", "1", 1);
    run_quad(dir, 0, quad4, sizeof quad4, ib8, 0, 0, 6, 0, 0, 1, 0, NULL, 0, 0, 0, rb, 7);
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    CHECK(same_img(ref, rb), "C38 u8 展开与 C36 不同");
    // C39 实例化
    run_quad(dir, 1, left2, sizeof left2, ib16, 0, 1, 6, 0, 0, 2, 0, NULL, 0, 0, 0, rb, 8);
    expect_rect(rb, 0, 0, 64, 64, 255, 0, 0, "C39 两实例");
    run_quad(dir, 1, left2, sizeof left2, ib16, 0, 1, 6, 0, 0, 1, 1, NULL, 0, 0, 0, rb, 9);
    expect_rect(rb, 0, 0, 32, 64, 0, 0, 0, "C39 base_instance=1 左半清屏");
    expect_rect(rb, 32, 0, 32, 64, 255, 0, 0, "C39 base_instance=1 右半");
    // C40 间接
    run_quad(dir, 0, quad4, sizeof quad4, ib16, 0, 1, 6, 0, 0, 1, 0, ibi, 0, 1, 20, rb, 10);
    CHECK(same_img(ref, rb), "C40 索引间接与 C36 不同");
    run_quad(dir, 0, six6, sizeof six6, NULL, 0, 0, 6, 0, 0, 1, 0, ini, 0, 1, 16, rb, 11);
    CHECK(same_img(ref, rb), "C40 非索引间接与 C36 不同");
    nvn_gpu_buf_release(ib16, NULL, NULL); nvn_gpu_buf_release(ibneg, NULL, NULL); nvn_gpu_buf_release(ib8, NULL, NULL);
    nvn_gpu_buf_release(ibi, NULL, NULL); nvn_gpu_buf_release(ini, NULL, NULL);
}


// C41/C42：索引越界与 u8 间接的负例（各自独立会话，失败即终止）。
static void c41_bounds_neg(const char *dir) {
    static const uint8_t idx[6] = {0, 1, 2, 2, 1, 3};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, color_pipe(dir, 0));
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 6; a.ibuf = ib; a.ioff = 6; a.itype = 0;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C41 越界 u8 应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out); nvn_gpu_buf_release(ib, NULL, NULL);
}

static void c42_u8_indirect_neg(const char *dir) {
    static const uint8_t idx[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t cmd[5] = {6, 1, 0, 0, 0};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_buf ind = make_buf(sizeof cmd, NVN_GPU_MEM_SHARED, cmd);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    setenv("S2A_VK_FORCE_INDEX_U8_EXPAND", "1", 1);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, color_pipe(dir, 0));
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 6; a.ibuf = ib; a.itype = 0; a.ind = ind; a.max_draws = 1; a.stride = 20;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C42 u8 间接应 UNSUPPORTED，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(ind, NULL, NULL);
}


static nvn_gpu_pipe blend_pipe(const char *dir, const blend_st *b, int en, uint32_t mask) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return NULL;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = mask;
    pd.color.blend_en = en ? 1 : 0;
    if (b) pd.blend[0] = *b;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "blend pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return pipe;
}

static void blend_draw(nvn_gpu_pipe pipe, const float clear[4], const float vb[4][8], const float *bcolor, uint8_t *out, uint64_t sig) {
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf buf = make_buf(sizeof *vb * 4, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    for (int i = 0; i < 4; i++) p.color[0].clear.f[i] = clear[i];
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    if (bcolor) memcpy(dyn.blend_color, bcolor, sizeof dyn.blend_color);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, buf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 4; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(r), "blend commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "blend wait：%d %s", e.rc.code, e.why);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C69：双源混合因子（NVN 0x10–0x13 → SRC1_*）。缺 dualSrcBlend 的机型（多数 Mali / Xclipse 之外的低端档）
// 用到它的管线须按规定报 UNSUPPORTED、不崩、不建 Vulkan 管线；有特性时须正常建成。
// 先测缺特性（把已启用的位临时清掉），再测设备实况：成功的管线进后端缓存，顺序反了会命中缓存。
static nvn_gpu_rc c69_dual_src_get(const char *dir, nvn_gpu_pipe *pipe, nvn_gpu_err *e) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    *pipe = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) {
        nvn_gpu_rc bad = {NVN_GPU_E_ARG};
        return bad;
    }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.color.blend_en = 1;
    pd.blend[0].src_rgb = 0x10; pd.blend[0].dst_rgb = 0x11;   // SRC1_COLOR / ONE_MINUS_SRC1_COLOR
    pd.blend[0].src_a = 0x2; pd.blend[0].dst_a = 0x1; pd.blend[0].eq_rgb = 1; pd.blend[0].eq_a = 1;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, pipe, e);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return r;
}

static void c69_dual_src_blend(const char *dir) {
    VkBool32 has = g_dev.core_features.dualSrcBlend;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    memset(&e, 0, sizeof e);
    g_dev.core_features.dualSrcBlend = VK_FALSE;
    nvn_gpu_rc r = c69_dual_src_get(dir, &pipe, &e);
    g_dev.core_features.dualSrcBlend = has;
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !pipe, "C69 缺 dualSrcBlend 时双源混合管线应 UNSUPPORTED，实 %d", r.code);
    pipe = NULL;
    memset(&e, 0, sizeof e);
    r = c69_dual_src_get(dir, &pipe, &e);
    if (has) {
        CHECK(OK(r) && pipe, "C69 有 dualSrcBlend 时双源混合管线应建成：%d %s", r.code, e.why);
    } else {
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !pipe, "C69 设备无 dualSrcBlend，双源混合管线应 UNSUPPORTED，实 %d", r.code);
    }
    printf("C69 双源混合：设备 dualSrcBlend=%u，缺特性时 UNSUPPORTED%s\n", (unsigned)has,
           has ? "，有特性时建成" : "（本机实况即缺特性）");
}

static void c43_blend(const char *dir) {
    float red_half[4][8] = {{-1,-1,0,1, 1,0,0,0.5f},{-1,1,0,1, 1,0,0,0.5f},{1,-1,0,1, 1,0,0,0.5f},{1,1,0,1, 1,0,0,0.5f}};
    float red[4][8] = {{-1,-1,0,1, 1,0,0,1},{-1,1,0,1, 1,0,0,1},{1,-1,0,1, 1,0,0,1},{1,1,0,1, 1,0,0,1}};
    const float blue[4] = {0, 0, 1, 1};
    blend_st b;
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x5; b.dst_rgb = 0x6; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    uint8_t out[64 * 64 * 4];
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red_half, NULL, out, 1);
    const uint8_t *p = out;
    CHECK((p[0] == 127 || p[0] == 128) && p[1] == 0 && (p[2] == 127 || p[2] == 128) && p[3] == 255,
          "C43 alpha 混合 = %02x%02x%02x%02x，期望 7f/80 0 7f/80 ff", p[0], p[1], p[2], p[3]);
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x2; b.dst_rgb = 0x2; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red, NULL, out, 2);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "C43 加法 = %02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
    float yellow[4][8] = {{-1,-1,0,1, 1,1,0,1},{-1,1,0,1, 1,1,0,1},{1,-1,0,1, 1,1,0,1},{1,1,0,1, 1,1,0,1}};
    blend_draw(blend_pipe(dir, NULL, 0, 0x1), blue, yellow, NULL, out, 3);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "C43 写掩码 = %02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
    // 负控：把 alpha 混合换成加法，像素必须不同
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x2; b.dst_rgb = 0x2; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red_half, NULL, out, 4);
    CHECK(!((out[0] == 127 || out[0] == 128) && (out[2] == 127 || out[2] == 128)), "C43 负控：加法仍等于 alpha 期望（因子没生效）");
}


static nvn_gpu_rc prim_pipe(const char *dir, uint32_t prim, nvn_gpu_pipe *out) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, prim == 0 ? "point.vert" : "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return (nvn_gpu_rc){NVN_GPU_E_COMPILE};
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = prim; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, out, &e);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return rc;
}

static int prim_draw(const char *dir, uint32_t prim, uint32_t count, const float *vb, uint8_t *out, uint64_t sig) {
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc r = prim_pipe(dir, prim, &pipe);
    if (!OK(r)) return (int)r.code;
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf buf = make_buf((size_t)count * 32, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, buf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = prim; a.count = count; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    if (OK(r)) (void)nvn_gpu_fence_wait(sig, 5000000000ull, &e);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    return OK(r) ? 0 : (int)r.code;
}

static int count_px(const uint8_t *img, uint8_t r, uint8_t g, uint8_t b) {
    int n = 0;
    for (int i = 0; i < 64 * 64; i++) if (img[i*4] == r && img[i*4+1] == g && img[i*4+2] == b) n++;
    return n;
}

static void c44_prims(const char *dir) {
    uint8_t img[64 * 64 * 4];
    float pts[3][8] = {{-0.75f,-0.75f,0,1,1,0,0,1},{0,0,0,1,1,0,0,1},{0.75f,0.75f,0,1,1,0,0,1}};
    CHECK(prim_draw(dir, 0, 3, pts[0], img, 1) == 0, "C44 点绘制失败");
    CHECK(count_px(img, 255, 0, 0) >= 3, "C44 点像素不足：%d", count_px(img, 255, 0, 0));
    float line[2][8] = {{-1,0,0,1,1,0,0,1},{1,0,0,1,1,0,0,1}};
    CHECK(prim_draw(dir, 1, 2, line[0], img, 2) == 0, "C44 线绘制失败");
    CHECK(count_px(img, 255, 0, 0) > 10, "C44 线像素不足：%d", count_px(img, 255, 0, 0));
    float strip[3][8] = {{-0.75f,-0.75f,0,1,1,0,0,1},{0,0.5f,0,1,1,0,0,1},{0.75f,-0.75f,0,1,1,0,0,1}};
    uint8_t listimg[64 * 64 * 4];
    CHECK(prim_draw(dir, 1, 3, strip[0], listimg, 3) == 0, "C44 线列表失败");
    CHECK(prim_draw(dir, 3, 3, strip[0], img, 4) == 0, "C44 线带失败");
    CHECK(memcmp(img, listimg, sizeof img) != 0, "C44 线带与线列表结果相同（拓扑没生效）");
    int strc = prim_draw(dir, 5, 4, quad4[0], img, 5); CHECK(strc == 0, "C44 三角带失败：%d", strc);
    CHECK(count_px(img, 255, 0, 0) > 500 && count_px(img, 0, 255, 0) > 500, "C44 三角带两色不足");
    nvn_gpu_pipe dummy = NULL;
    nvn_gpu_rc frc = prim_pipe(dir, 6, &dummy);
    CHECK(OK(frc), "C44 三角扇在 lavapipe 上应可用，实为 %d", frc.code);
    CHECK(prim_pipe(dir, 2, &dummy).code == NVN_GPU_E_UNSUPPORTED, "C44 线环应 UNSUPPORTED");
}


static void blend2_draw(const char *dir, const blend_st b[2], uint8_t *out0, uint8_t *out1, uint64_t sig) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad2.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C45 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.cfmt[1] = NVN_FORMAT_RGBA8;
    pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xFF;
    pd.color.blend_en = 0x3;
    pd.blend[0] = b[0]; pd.blend[1] = b[1];
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C45 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex t0 = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_tex t1 = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v0 = make_view(t0), v1 = make_view(t1);
    float red_half[4][8] = {{-1,-1,0,1, 1,0,0,0.5f},{-1,1,0,1, 1,0,0,0.5f},{1,-1,0,1, 1,0,0,0.5f},{1,1,0,1, 1,0,0,0.5f}};
    nvn_gpu_buf vb = make_buf(sizeof red_half, NVN_GPU_MEM_SHARED, red_half);
    uint8_t z[64 * 64 * 4]; memset(z, 0, sizeof z);
    nvn_gpu_buf r0 = make_buf(sizeof z, NVN_GPU_MEM_SHARED, z), r1 = make_buf(sizeof z, NVN_GPU_MEM_SHARED, z);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 2; p.color[0].view = v0; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[1].view = v1; p.color[1].load = NVN_GPU_LOAD_CLEAR; p.color[1].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;
    p.color[1].clear.f[2] = 1; p.color[1].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 4; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t0, 0, 0, NULL, r0, 0, 256, 16384);
    nvn_gpu_copy_tex_buf(c, t1, 0, 0, NULL, r1, 0, 256, 16384);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(rc), "C45 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "C45 wait：%d %s", e.rc.code, e.why);
    memcpy(out0, buf_cpu(r0), 64 * 64 * 4); memcpy(out1, buf_cpu(r1), 64 * 64 * 4);
    nvn_gpu_view_release(v0); nvn_gpu_view_release(v1);
    nvn_gpu_tex_release(t0); nvn_gpu_tex_release(t1);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(r0, NULL, NULL); nvn_gpu_buf_release(r1, NULL, NULL);
}

static void c45_multi_blend(const char *dir) {
    blend_st b[2];
    memset(b, 0, sizeof b);
    for (int i = 0; i < 2; i++) { b[i].src_rgb = 0x5; b[i].dst_rgb = 0x6; b[i].src_a = 0x2; b[i].dst_a = 0x2; b[i].eq_rgb = 1; b[i].eq_a = 1; }
    b[1].src_rgb = 0x2; b[1].dst_rgb = 0x2; b[1].src_a = 0x2; b[1].dst_a = 0x2;
    uint8_t o0[64 * 64 * 4], o1[64 * 64 * 4];
    blend2_draw(dir, b, o0, o1, 1);
    CHECK((o0[0] == 127 || o0[0] == 128) && o0[1] == 0 && (o0[2] == 127 || o0[2] == 128),
          "C45 target0 alpha = %02x%02x%02x", o0[0], o0[1], o0[2]);
    CHECK(o1[0] == 0 && o1[1] == 255 && o1[2] == 255, "C45 target1 加法 = %02x%02x%02x", o1[0], o1[1], o1[2]);
    blend_st sw[2]; sw[0] = b[1]; sw[1] = b[0];
    uint8_t n0[64 * 64 * 4], n1[64 * 64 * 4];
    blend2_draw(dir, sw, n0, n1, 2);
    CHECK(n0[0] == 255 && n0[2] == 255, "C45 负控：交换后 target0 应为加法：%02x%02x%02x", n0[0], n0[1], n0[2]);
}


static void c46_blend_const(const char *dir) {
    blend_st b;
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x61; b.dst_rgb = 0x1; b.src_a = 0x61; b.dst_a = 0x1; b.eq_rgb = 1; b.eq_a = 1;
    float cb[4] = {0.25f, 0.5f, 0.75f, 0.25f};
    float clear[4] = {0, 0, 0, 1};
    float vb[4][8] = {{-1,-1,0,1,1,1,0,1},{-1,1,0,1,1,1,0,1},{1,-1,0,1,1,1,0,1},{1,1,0,1,1,1,0,1}};
    uint8_t out[64 * 64 * 4];
    blend_draw(blend_pipe(dir, &b, 1, 0xF), clear, vb, cb, out, 6);
    CHECK((out[0] == 63 || out[0] == 64) && out[1] == 0 && out[2] == 0 && (out[3] == 63 || out[3] == 64),
          "C46 混合常量 = %02x%02x%02x%02x，期望 3f/40 0 0 3f/40", out[0], out[1], out[2], out[3]);
}


static void c47_logic(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C47 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe p = NULL;
    nvn_gpu_err e;
    pd.color.logic_op = 0;   // NVN_LOGIC_OP_CLEAR
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C47 CLEAR 逻辑操作应 UNSUPPORTED，实为 %d", r.code);
    pd.color.logic_op = 3;   // 缺省 COPY
    r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &e);
    CHECK(OK(r), "C47 缺省 COPY 应可建管线：%d %s", r.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}


static void c48_fan(const char *dir) {
    uint8_t fan[64 * 64 * 4], strip[64 * 64 * 4];
    CHECK(prim_draw(dir, 6, 4, quad4[0], fan, 7) == 0, "C48 三角扇绘制失败");
    CHECK(prim_draw(dir, 5, 4, quad4[0], strip, 8) == 0, "C48 三角带绘制失败");
    CHECK(memcmp(fan, strip, sizeof fan) != 0, "C48 三角扇与三角带结果相同（拓扑映射没生效）");
    CHECK(count_px(fan, 255, 0, 0) > 500 && count_px(fan, 0, 255, 0) > 500, "C48 三角扇两色不足");
}

static void c48_mismatch(const char *dir) {
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(prim_pipe(dir, 6, &pipe)), "C48 扇管线创建失败");
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf vb = make_buf(sizeof quad4, NVN_GPU_MEM_SHARED, quad4);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 5; a.count = 4; a.instances = 1;   // 管线是扇，draw 写带 → 必须 E_ARG
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C48 prim 与管线不一致应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t); nvn_gpu_buf_release(vb, NULL, NULL);
}


static int restart_draw(const char *dir, int on, uint8_t *out, uint64_t sig) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return -1;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 5; pd.prim_restart = on ? 1 : 0; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    if (!OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e))) { nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm); return -1; }
    // 0x10000 个顶点，0xFFFF 是合法顶点；关重启时它会产生一条连接两段的三角形
    float *vb = calloc(0x10000 * 8, sizeof(float));
    if (!vb) { nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm); return -1; }
    const float V[7][8] = {
        {-1,-1,0,1, 1,0,0,1}, {-1,1,0,1, 1,0,0,1}, {-0.95f,-1,0,1, 1,0,0,1},
        {0.8f,1,0,1, 0,1,0,1}, {1,-1,0,1, 0,1,0,1}, {1,1,0,1, 0,1,0,1},
        {0,0.5f,0,1, 1,0,0,1}};
    for (int i = 0; i < 6; i++) memcpy(vb + i * 8, V[i], sizeof V[i]);
    memcpy(vb + 0xFFFF * 8, V[6], sizeof V[6]);
    static const uint16_t idx[7] = {0,1,2,0xFFFF,3,4,5};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_buf vbuf = make_buf(0x10000 * 32, NVN_GPU_MEM_SHARED, vb);
    free(vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vbuf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 5; a.count = 7; a.instances = 1; a.ibuf = ib; a.itype = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 256, 16384);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    if (OK(r)) (void)nvn_gpu_fence_wait(sig, 5000000000ull, &e);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t); nvn_gpu_buf_release(vbuf, NULL, NULL);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return OK(r) ? 0 : (int)r.code;
}

static void c49_restart(const char *dir) {
    uint8_t on[64 * 64 * 4], off[64 * 64 * 4];
    CHECK(restart_draw(dir, 1, on, 9) == 0, "C49 重启开启绘制失败");
    CHECK(restart_draw(dir, 0, off, 10) == 0, "C49 重启关闭绘制失败");
    const int px = (1 * 64 + 2) * 4;   // 关重启时 0xFFFF 顶点多出的连接三角形覆盖 (2,1)
    CHECK(on[px] == 0 && on[px + 1] == 0 && on[px + 2] == 0, "C49 开重启时指定像素应保持清屏色");
    CHECK(off[px] != 0 || off[px + 1] != 0 || off[px + 2] != 0, "C49 关重启时 0xFFFF 顶点应多画出连接三角形（覆盖指定像素）");
    CHECK(memcmp(on, off, sizeof on) != 0, "C49 重启关闭与开启结果相同（重启没生效）");
}

static void c50_list_restart(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C50 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.prim_restart = 1; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "C50 列表图元带 prim_restart 应可建管线（忽略重启）：%d %s", r.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}


static void sample_tex(const char *dir, const char *frag, uint32_t target, uint32_t depth,
                       const uint8_t *face_colors, uint32_t ncolors, uint32_t tw, uint32_t th,
                       const view_t *vv, uint8_t *img, uint64_t sig);

static void c51_array(const char *dir) {
    nvn_gpu_tex src = make_tex_target(4, 4, 5, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8,
                                      NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    static const uint8_t colors[5][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255},{255,255,0,255}};
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_view sv = make_view(src);
    view_t vd; memset(&vd, 0, sizeof vd);
    vd.set_layers = 1; vd.min_layer = 1; vd.nlayers = 4;
    nvn_gpu_err e;
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl; memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "tri.vert", "arr.frag", &vl, &fl, &vs, &fs, &vsm, &fsm), "C51 读着色器失败");
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul; pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C51 pipe：%d %s", e.rc.code, e.why);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C51 samp：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    uint8_t zero[64*64*4]; memset(zero,0,sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    for (uint32_t l = 0; l < 5; l++) {
        uint8_t px[4*4*4];
        for (int i = 0; i < 16; i++) memcpy(px + i*4, colors[l], 4);
        nvn_gpu_tex_upload(c, src, 0, l, NULL, px, 16, 64);
    }
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2]; memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = sv;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 256, 16384);
    CHECK(OK(nvn_gpu_cmd_commit(c, 11, on_done, NULL, &e)), "C51 commit：%d %s", e.rc.code, e.why);
    (void)0;
    CHECK(OK(nvn_gpu_fence_wait(11, 5000000000ull, &e)), "C51 wait：%d %s", e.rc.code, e.why);
    uint8_t img[64*64*4]; memcpy(img, buf_cpu(rb), sizeof img);
    const int pts[4][2] = {{16,16},{48,16},{16,48},{48,48}};
    const uint8_t *exp[4] = {colors[0], colors[1], colors[2], colors[3]};
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = img + (pts[i][1]*64 + pts[i][0])*4;
        CHECK(p[0]==exp[i][0] && p[1]==exp[i][1] && p[2]==exp[i][2], "C51 层 %d = %02x%02x%02x", i, p[0],p[1],p[2]);
    }
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    uint8_t img2[64*64*4];
    sample_tex(dir, "arr.frag", NVN_TEXTURE_TARGET_2D_ARRAY, 5, colors[0], 5, 4, 4, &vd, img2, 12);
    const uint8_t *q = img2 + (16*64 + 16)*4;
    CHECK(q[0]==colors[1][0] && q[1]==colors[1][1] && q[2]==colors[1][2],
          "C51 负控 起始层偏 1 后第一象限应为第二层颜色，实为 %02x%02x%02x", q[0],q[1],q[2]);
}


static void sample_tex(const char *dir, const char *frag, uint32_t target, uint32_t depth,
                       const uint8_t *face_colors, uint32_t ncolors, uint32_t tw, uint32_t th,
                       const view_t *vv, uint8_t *img, uint64_t sig) {
    nvn_gpu_tex src = make_tex_target(tw, th, depth, target, NVN_FORMAT_RGBA8,
                                      NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_view sv = NULL;
    if (vv) { nvn_gpu_err ve; CHECK(OK(nvn_gpu_view_create(src, vv, &sv, &ve)), "%s 视图：%d %s", frag, ve.rc.code, ve.why); }
    else sv = make_view(src);
    nvn_gpu_layout_ent ents[2]; memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl; memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "tri.vert", frag, &vl, &fl, &vs, &fs, &vsm, &fsm), "%s 读着色器失败", frag);
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul; pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "%s pipe：%d %s", frag, e.rc.code, e.why);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "%s samp：%d %s", frag, e.rc.code, e.why);
    const float verts[3][4] = {{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    uint8_t zero[64*64*4]; memset(zero,0,sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    for (uint32_t f = 0; f < ncolors; f++) {
        uint8_t px[64*4];
        for (uint32_t i = 0; i < tw * th; i++) memcpy(px + i*4, face_colors + f*4, 4);
        nvn_gpu_tex_upload(c, src, 0, f, NULL, px, tw*4, tw*4*th);
    }
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2]; memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = sv;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 256, 16384);
    CHECK(OK(nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e)), "%s commit：%d %s", frag, e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "%s wait：%d %s", frag, e.rc.code, e.why);
    memcpy(img, buf_cpu(rb), 64*64*4);
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}

static void check_cell(const char *tag, const uint8_t *img, int x, int y, const uint8_t *exp) {
    const uint8_t *p = img + (y*64 + x)*4;
    CHECK(p[0]==exp[0] && p[1]==exp[1] && p[2]==exp[2], "%s (%d,%d)=%02x%02x%02x 期望 %02x%02x%02x",
          tag, x, y, p[0],p[1],p[2], exp[0],exp[1],exp[2]);
}

static void c52_cube(const char *dir) {
    static const uint8_t c[6][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255},{255,0,255,255},{0,255,255,255}};
    uint8_t img[64*64*4];
    sample_tex(dir, "cube.frag", NVN_TEXTURE_TARGET_CUBEMAP, 6, c[0], 6, 1, 1, NULL, img, 13);
    for (int f = 0; f < 6; f++) check_cell("C52", img, 10 + (f%3)*22, 16 + (f/3)*32, c[f]);
    uint8_t sw[6][4];
    memcpy(sw, c, sizeof sw);
    memcpy(sw[0], c[1], 4); memcpy(sw[1], c[0], 4);
    uint8_t img2[64*64*4];
    sample_tex(dir, "cube.frag", NVN_TEXTURE_TARGET_CUBEMAP, 6, sw[0], 6, 1, 1, NULL, img2, 14);
    check_cell("C52 负控", img2, 10, 16, c[1]);
    CHECK(img2[0] != img[0] || img2[1] != img[1] || img2[2] != img[2], "C52 换面顺序后 (+X) 应不同");
}

static void c53_cube_array(const char *dir) {
    uint8_t c[12][4];
    memset(c, 0, sizeof c);
    for (int f = 0; f < 6; f++) { c[f][0] = 255; }          // 第 0 个立方体 +X 红、其余面随 f 变化
    for (int f = 0; f < 6; f++) { c[6+f][1] = 255; c[6+f][2] = 255; }   // 第 1 个立方体全青
    c[0][0] = 255; c[0][1] = 0; c[0][2] = 0;
    uint8_t img[64*64*4];
    sample_tex(dir, "cubearr.frag", NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, c[0], 12, 1, 1, NULL, img, 15);
    const uint8_t red[4] = {255,0,0,255}, cyan[4] = {0,255,255,255};
    check_cell("C53", img, 16, 32, red);
    check_cell("C53", img, 48, 32, cyan);
}

static void c54_3d(const char *dir) {
    static const uint8_t c[4][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
    uint8_t img[64*64*4];
    sample_tex(dir, "tex3d.frag", NVN_TEXTURE_TARGET_3D, 4, c[0], 4, 2, 2, NULL, img, 16);
    check_cell("C54", img, 16, 16, c[0]);
    check_cell("C54", img, 48, 16, c[1]);
    check_cell("C54", img, 16, 48, c[2]);
    check_cell("C54", img, 48, 48, c[3]);
}


static void c55_compute(const char *dir) {
    char path[512]; size_t csz = 0;
    snprintf(path, sizeof path, "%s/comp.comp.spv", dir);
    void *cm = read_file(path, &csz);
    CHECK(cm != NULL, "C55 读不到 comp.comp.spv");
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1; ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl; memset(&cl, 0, sizeof cl); cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader_src src; memset(&src, 0, sizeof src);
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main"; src.blob = cm; src.bytes = csz; src.layout = &cl;
    nvn_gpu_shader sh = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_shader_load(&src, &sh, &e)), "C55 shader：%d %s", e.rc.code, e.why);
    nvn_gpu_stage_desc cs; memset(&cs, 0, sizeof cs); cs.sh = sh;
    nvn_gpu_pipe cp = NULL;
    CHECK(OK(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e)), "C55 cpipe：%d %s", e.rc.code, e.why);
    uint32_t data[64]; memset(data, 0, sizeof data);
    nvn_gpu_buf buf = make_buf(sizeof data, NVN_GPU_MEM_SHARED, data);
    nvn_gpu_binding b; memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO;
    b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = sizeof data;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c, cp, 4, 1, 1, NULL, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c, 17, on_done, NULL, &e)), "C55 commit：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(17, 5000000000ull, &e)), "C55 wait：%d %s", e.rc.code, e.why);
    uint32_t rd[64]; memcpy(rd, buf_cpu(buf), sizeof rd);
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != i + 1) bad++;
    CHECK(bad == 0, "C55 dispatch 结果错 %d 项（首项 %u）", bad, rd[0]);
    // 间接 dispatch：先把 GPU 缓冲清 0，否则即使什么都不做也会读到上一次的 1..64
    memset(buf_cpu(buf), 0, sizeof rd);
    uint32_t indv[3] = {4, 1, 1};
    nvn_gpu_buf ind = make_buf(sizeof indv, NVN_GPU_MEM_SHARED, indv);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_set_pipe(c2, cp);
    nvn_gpu_bind(c2, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c2, cp, 0, 0, 0, ind, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c2, 18, on_done, NULL, &e)), "C55 间接 commit：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(18, 5000000000ull, &e)), "C55 间接 wait：%d %s", e.rc.code, e.why);
    memcpy(rd, buf_cpu(buf), sizeof rd);
    bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != i + 1) bad++;
    CHECK(bad == 0, "C55 间接 dispatch 结果错 %d 项", bad);
    // #659: GPU writes parameters, then native indirect dispatch consumes them
    // in the same submission. No CPU read/patch of generated parameters.
    memset(buf_cpu(buf), 0, sizeof rd);
    uint32_t zeros[64] = {0};
    uint32_t stale[4] = {99, 0, 0, 0};
    nvn_gpu_buf generated = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_buf gpu_ind = make_buf(sizeof stale, NVN_GPU_MEM_SHARED, stale);
    nvn_gpu_buf drv = make_buf(sizeof stale, NVN_GPU_MEM_SHARED, stale);
    nvn_gpu_binding producer = b; producer.u.b.buf = generated;
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_set_pipe(c3, cp);
    nvn_gpu_bind(c3, NVN_GPU_SH_CS, &producer, 1);
    nvn_gpu_dispatch(c3, cp, 1, 1, 1, NULL, 0);
    // Existing comp.comp produces 1..16. Assemble {2,1,1} on GPU at offset 4.
    nvn_gpu_copy_buf(c3, generated, 4, gpu_ind, 4, 4);
    nvn_gpu_copy_buf(c3, generated, 0, gpu_ind, 8, 4);
    nvn_gpu_copy_buf(c3, generated, 0, gpu_ind, 12, 4);
    // Model DRV0's group-count copy from the exact same authoritative buffer.
    nvn_gpu_copy_buf(c3, gpu_ind, 4, drv, 4, 12);
    nvn_gpu_barrier(c3, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);
    nvn_gpu_bind(c3, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c3, cp, 0, 0, 0, gpu_ind, 4);
    CHECK(OK(nvn_gpu_cmd_commit(c3, 19, on_done, NULL, &e)), "C55 GPU indirect commit: %d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(19, 5000000000ull, &e)), "C55 GPU indirect wait: %d %s", e.rc.code, e.why);
    memcpy(rd, buf_cpu(buf), sizeof rd);
    bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != (i < 32 ? i + 1 : 0)) bad++;
    CHECK(bad == 0, "C55 GPU generated indirect: %d wrong outputs", bad);
    uint32_t counts[4]; memcpy(counts, buf_cpu(drv), sizeof counts);
    CHECK(counts[0] == 99 && counts[1] == 2 && counts[2] == 1 && counts[3] == 1,
          "C55 GPU DRV0 copy: %u %u %u %u", counts[0], counts[1], counts[2], counts[3]);
    nvn_gpu_buf_release(drv, NULL, NULL);
    nvn_gpu_buf_release(gpu_ind, NULL, NULL);
    nvn_gpu_buf_release(generated, NULL, NULL);
    // 0 组 dispatch：空操作，命令缓冲仍应可提交
    nvn_gpu_cmd c4 = begin_cmd();
    nvn_gpu_set_pipe(c4, cp);
    nvn_gpu_bind(c4, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c4, cp, 0, 1, 1, NULL, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c4, 20, on_done, NULL, &e)), "C55 0 组 dispatch 后 commit 失败：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(20, 5000000000ull, &e)), "C55 0 组 wait：%d %s", e.rc.code, e.why);
    nvn_gpu_buf_release(ind, NULL, NULL);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh); free(cm);
}

// ---------- C56–C68：D8 补口（资源类别 + push 常量）契约用例 + D9 静默错复现 ----------
// 期望值全部按契约与 GLSL/SPIR-V 语义手算，不经过被测实现的路径。

static nvn_gpu_shader load_one_shader(const char *dir, const char *name, nvn_gpu_stage stage,
                                      const nvn_gpu_layout *lay) {
    char path[512];
    size_t sz = 0;
    snprintf(path, sizeof path, "%s/%s.spv", dir, name);
    void *mem = read_file(path, &sz);
    CHECK(mem != NULL, "读不到 %s.spv", name);
    if (!mem) return NULL;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.stage = stage; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = mem; src.bytes = sz; src.layout = lay;
    nvn_gpu_err e;
    nvn_gpu_shader sh = NULL;
    CHECK(OK(nvn_gpu_shader_load(&src, &sh, &e)), "%s shader：%d %s", name, e.rc.code, e.why);
    free(mem);
    return sh;
}

static nvn_gpu_pipe mk_cpipe(const char *dir, const char *name, const nvn_gpu_layout *lay, nvn_gpu_shader *sh_out) {
    *sh_out = NULL;
    nvn_gpu_shader sh = load_one_shader(dir, name, NVN_GPU_SH_CS, lay);
    if (!sh) return NULL;
    nvn_gpu_stage_desc cs;
    memset(&cs, 0, sizeof cs);
    cs.sh = sh;
    nvn_gpu_err e;
    nvn_gpu_pipe cp = NULL;
    CHECK(OK(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e)), "%s cpipe：%d %s", name, e.rc.code, e.why);
    *sh_out = sh;
    return cp;
}

static void fill_gpipe_desc(nvn_gpu_pipe_desc *pd, nvn_gpu_shader vs, nvn_gpu_shader fs, uint32_t dfmt) {
    memset(pd, 0, sizeof *pd); pd->poly.mode=NVN_POLYGON_MODE_FILL;
    pd->color.logic_op = 0x3;
    pd->st[NVN_GPU_SH_VS].sh = vs;
    pd->st[NVN_GPU_SH_FS].sh = fs;
    pd->cfmt[0] = NVN_FORMAT_RGBA8;
    pd->dfmt = dfmt;
    pd->samples = 1;
    pd->prim = 4;
    pd->chmask.mask = 0xF;
    pd->attr[0].fmt = NVN_FORMAT_RGBA32F; pd->attr[0].offset = 0; pd->attr[0].stream = 0;
    pd->stream[0].stride = 16;
    pd->origin_ll = 1;   // 与 full_dyn 的窗口原点一致（契约 v6：两处必须相同）
}

static void full_dyn(nvn_gpu_dyn *d) {
    memset(d, 0, sizeof *d); d->origin_ll = 1;   /* 原点在左下 = Vulkan 的自然方向 */
    d->nvp = 1; d->vp[0][2] = 64; d->vp[0][3] = 64; d->vp[0][5] = 1;
    d->has_sc = 1; d->sc[2] = 64; d->sc[3] = 64;
}

static nvn_gpu_buf full_vb(void) {
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    return make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
}

// C56：FS push 常量。两次 push 两个颜色，两笔各画一半屏。
// 手算：RGBA8 的 UNORM 量化 0.25*255=63.75→64、0.5*255=127.5→128、0.75*255=191.25→191。
static void c56_push_fs(const char *dir) {
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.push_off = 0; fl.push_bytes = 16;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 pushc 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C56 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.sc[3] = 32;   // 上半屏第一笔
    const float ca[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, ca, sizeof ca);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    const float cb[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, cb, sizeof cb);
    dyn.sc[1] = 32;   // 下半屏第二笔
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 100, on_done, NULL, &e);
    CHECK(OK(r), "C56 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(100, 5000000000ull, &e);
    CHECK(OK(r), "C56 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 64, 128, 191, "C56 上半");
    expect_rect(buf_cpu(rb), 0, 32, 64, 32, 255, 0, 0, "C56 下半");
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C57：CS push 常量 + SSBO 绑定共存。d[i] = 7 + i（i = 0..63 → 7..70）。
static void c57_push_cs(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1; cl.push_off = 0; cl.push_bytes = 4;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "pushc.comp", &cl, &sh);
    CHECK(cp != NULL, "C57 cpipe 为空");
    if (!cp) return;
    nvn_gpu_buf buf = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = 64 * 4;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    uint32_t v = 7;
    nvn_gpu_push(c, NVN_GPU_SH_CS, 0, &v, sizeof v);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 101, on_done, NULL, &e);
    CHECK(OK(r), "C57 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(101, 5000000000ull, &e);
    CHECK(OK(r), "C57 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(buf);
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != 7 + i) bad++;
    CHECK(bad == 0, "C57 push 结果错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C58d：同一个 cmd 切到更大 push 范围时，旧阶段值必须保留且写入不越界。
static void c58d_push_pipe_resize(const char *dir) {
    nvn_gpu_layout_ent ent = {0};
    ent.set = 0; ent.cls = NVN_GPU_RES_SSBO; ent.access = NVN_GPU_ACC_W;
    ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout small = {0}, large = {0};
    small.ent = large.ent = &ent; small.nent = large.nent = 1;
    small.push_bytes = 4; large.push_bytes = 16;
    nvn_gpu_shader a = NULL, b = NULL;
    nvn_gpu_pipe pa = mk_cpipe(dir, "pushc.comp", &small, &a);
    nvn_gpu_pipe pb = mk_cpipe(dir, "pushc.comp", &large, &b);
    CHECK(pa && pb, "C58d 两个计算管线必须可创建");
    if (!pa || !pb) { if (a) nvn_gpu_shader_release(a); if (b) nvn_gpu_shader_release(b); return; }
    nvn_gpu_buf out = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd();
    nvn_gpu_set_pipe(cmd, pa);
    uint32_t value = 7, last = 42;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &value, sizeof value);
    nvn_gpu_set_pipe(cmd, pb);
    uint32_t capacity = nvn_gpu_vk_test_push_capacity(cmd, NVN_GPU_SH_CS);
    CHECK(capacity >= large.push_bytes, "C58d 切换管线后的容量 %u，小于新范围 %u", capacity, large.push_bytes);
    if (capacity < large.push_bytes) {
        nvn_gpu_cmd_abort(cmd); nvn_gpu_buf_release(out, NULL, NULL);
        nvn_gpu_shader_release(a); nvn_gpu_shader_release(b); return;
    }
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 12, &last, sizeof last);
    nvn_gpu_binding binding = {0}; binding.ent = 0; binding.cls = NVN_GPU_RES_SSBO;
    binding.u.b.buf = out; binding.u.b.len = 64 * 4;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &binding, 1);
    nvn_gpu_dispatch(cmd, pb, 1, 1, 1, NULL, 0);
    nvn_gpu_err error; nvn_gpu_rc result = nvn_gpu_cmd_commit(cmd, 102, on_done, NULL, &error);
    CHECK(OK(result), "C58d commit %d %s", result.code, error.why);
    result = nvn_gpu_fence_wait(102, 5000000000ull, &error);
    CHECK(OK(result), "C58d wait %d %s", result.code, error.why);
    const uint32_t *values = (const uint32_t *)buf_cpu(out);
    int bad = 0; for (uint32_t i = 0; i < 64; i++) if (values[i] != 7 + i) bad++;
    CHECK(!bad, "C58d 切管线后 push 值错误：%d 项", bad);
    nvn_gpu_buf_release(out, NULL, NULL); nvn_gpu_shader_release(a); nvn_gpu_shader_release(b);
}

// C58a–c：push 越界 / 未声明阶段 → 粘滞 E_ARG（契约：只许写进该阶段声明范围）。
// 失败段让时间线进终态、恢复只能关会话重开，故三个子例各占独立会话（main 里关/开）。
static nvn_gpu_pipe c58_cpipe(const char *dir, nvn_gpu_shader *sh_out) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1; cl.push_off = 0; cl.push_bytes = 4;
    return mk_cpipe(dir, "pushc.comp", &cl, sh_out);
}

static void c58a_push_oob_cs(const char *dir) {
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = c58_cpipe(dir, &sh);
    CHECK(cp != NULL, "C58a cpipe 为空");
    if (!cp) { nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    uint32_t v = 1;
    nvn_gpu_push(c, NVN_GPU_SH_CS, 2, &v, 4);   // 声明 [0,4)：写 [2,4) → off+4=6 > 4
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 102, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58a 越界 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_shader_release(sh);
}

static void c58b_push_bad_stage(const char *dir) {
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = c58_cpipe(dir, &sh);
    CHECK(cp != NULL, "C58b cpipe 为空");
    if (!cp) { nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    uint32_t v = 1;
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, &v, 4);   // 计算管线无 FS 阶段 → 未声明范围
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 103, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58b 未声明阶段 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_shader_release(sh);
}

static void c58c_push_oob_fs(const char *dir) {
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.push_off = 0; fl.push_bytes = 16;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 pushc 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C58c pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_DONTCARE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    const float part[2] = {0.1f, 0.2f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 4, part, sizeof part);   // [4,8) ⊆ [0,16)：合法，不应置错
    nvn_gpu_push(c, NVN_GPU_SH_FS, 12, part, sizeof part);  // [12,8)：12+8=20 > 16 → E_ARG
    nvn_gpu_pass_end(c);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 104, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58c 越界 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C59a：缓冲视图显式携带格式，后端必须能创建并释放它。
static void c59a_buffer_view(void) {
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create_buf(b, 0, 16, NVN_FORMAT_R32UI, &v, &e);
    CHECK(OK(r) && v != NULL, "C59a R32UI 缓冲视图应创建成功：%d %s", r.code, e.why);
    if (v) nvn_gpu_view_release(v);
    for (int i = 0; i < 2; i++) {
        g_texel_feature_mask = i ? VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT
                                 : VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        v = NULL;
        r = nvn_gpu_view_create_buf(b, 0, 16, NVN_FORMAT_R32UI, &v, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !v,
              "C59a 模拟缺一种 texel 特性时不可创建双 usage 视图（%d）：%d", i, r.code);
        if (v) nvn_gpu_view_release(v);
    }
    g_texel_feature_mask = 0;
    const uint64_t offsets[] = {0, 0, 16};
    const uint64_t lengths[] = {0, 15, 4};
    for (int i = 0; i < 3; i++) {
        v = NULL;
        r = nvn_gpu_view_create_buf(b, offsets[i], lengths[i], NVN_FORMAT_R32UI, &v, &e);
        CHECK(r.code == NVN_GPU_E_ARG && !v, "C59a 非法范围 %d 应返回 ARG：%d", i, r.code);
        if (v) nvn_gpu_view_release(v);
    }
    nvn_gpu_buf_release(b, NULL, NULL);
}

// C59：Unknown 采样纹素缓冲的格式来自显式 view，读取结果须与输入一致。
static void c59_tbuf(const char *dir) {
    nvn_gpu_layout_ent ents[2] = {0};
    ents[0].cls = NVN_GPU_RES_TBUF; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_W;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ents; lay.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tbuf.comp", &lay, &sh);
    CHECK(cp != NULL, "C59 Unknown 采样 TBUF 管线必须创建成功");
    if (!cp) { if (sh) nvn_gpu_shader_release(sh); return; }
    const uint32_t src[4] = {11, 22, 33, 44};
    nvn_gpu_buf input = make_buf(sizeof src, NVN_GPU_MEM_SHARED, src);
    nvn_gpu_buf output = make_buf(sizeof src, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_view view = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create_buf(input, 0, sizeof src, NVN_FORMAT_R32UI, &view, &e);
    CHECK(OK(r) && view, "C59 视图创建失败：%d %s", r.code, e.why);
    if (!view) { nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL); nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b[2] = {0};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TBUF; b[0].u.view = view;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = output; b[1].u.b.len = sizeof src;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2);
    int retired_before = atomic_load(&g_retired);
    nvn_gpu_buf_release(input, on_retire, NULL);
    nvn_gpu_view_release(view);
    CHECK(atomic_load(&g_retired) == retired_before, "C59 录制中的视图未保活底层缓冲");
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    r = nvn_gpu_cmd_commit(c, 105, on_done, NULL, &e);
    CHECK(OK(r), "C59 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(105, 5000000000ull, &e);
    CHECK(OK(r), "C59 wait：%d %s", r.code, e.why);
    CHECK(!memcmp(buf_cpu(output), src, sizeof src), "C59 采样读回与输入不同");
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "C59 完成后底层缓冲未退休");
    CHECK(atomic_load(&g_retired) == retired_before + 1, "C59 底层缓冲退休次数不为一");
    nvn_gpu_buf_release(output, NULL, NULL);

    // 同一 Unknown 着色器用 RGBA8UI 时，读取每个 texel 的首分量。
    const uint8_t packed[16] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    const uint32_t expected[4] = {1,5,9,13};
    input = make_buf(sizeof packed, NVN_GPU_MEM_SHARED, packed);
    output = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
    view = NULL;
    r = nvn_gpu_view_create_buf(input, 0, sizeof packed, NVN_FORMAT_RGBA8UI, &view, &e);
    CHECK(OK(r) && view, "C59 RGBA8UI 视图创建失败：%d %s", r.code, e.why);
    if (view) {
        c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
        b[0].u.view = view; b[1].u.b.buf = output;
        nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2);
        nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
        r = nvn_gpu_cmd_commit(c, 106, on_done, NULL, &e);
        CHECK(OK(r), "C59 RGBA8UI commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(106, 5000000000ull, &e);
        CHECK(OK(r), "C59 RGBA8UI wait：%d %s", r.code, e.why);
        CHECK(!memcmp(buf_cpu(output), expected, sizeof expected), "C59 RGBA8UI 采样结果不符");
        nvn_gpu_view_release(view);
    }
    nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// 失败提交令时间线终止，旧绑定负测必须独占会话。
static void c59b_legacy_binding(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {0};
    ent[0].cls = NVN_GPU_RES_TBUF; ent[0].access = NVN_GPU_ACC_R;
    ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W;
    ent[1].binding = 1; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ent; lay.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tbuf.comp", &lay, &sh);
    CHECK(cp != NULL, "C59b 管线创建失败");
    if (!cp) { if (sh) nvn_gpu_shader_release(sh); return; }
    nvn_gpu_buf input = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding legacy = {0}; legacy.cls = NVN_GPU_RES_TBUF;
    legacy.u.b.buf = input; legacy.u.b.len = 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &legacy, 1);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 108, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C59b 旧 u.b 应返回 UNSUPPORTED：%d %s", r.code, e.why);
    nvn_gpu_buf_release(input, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C60：IMAGE_TBUF（storage texel buffer）。imageStore 写 src[i]+100 → {101,102,103,104}。
static void c60_image_tbuf(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_IMAGE_TBUF; ents[0].access = NVN_GPU_ACC_W;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = ents; cl.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "itbuf.comp", &cl, &sh);
    CHECK(cp != NULL, "C60 cpipe 为空");
    if (!cp) return;
    const uint32_t src[4] = {1, 2, 3, 4};
    nvn_gpu_buf ib = make_buf(sizeof src, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf sb = make_buf(sizeof src, NVN_GPU_MEM_SHARED, src);
    nvn_gpu_view iv = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc vr = nvn_gpu_view_create_buf(ib, 0, sizeof src, NVN_FORMAT_R32UI, &iv, &e);
    CHECK(OK(vr) && iv, "C60 视图创建失败：%d %s", vr.code, e.why);
    if (!iv) { nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(sb, NULL, NULL); nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[2];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_IMAGE_TBUF; bs[0].u.view = iv;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = sb; bs[1].u.b.off = 0; bs[1].u.b.len = sizeof src;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 2);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 107, on_done, NULL, &e);
    CHECK(OK(r), "C60 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(107, 5000000000ull, &e);
    CHECK(OK(r), "C60 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(ib);
    int bad = 0;
    for (int i = 0; i < 4; i++) if (rd[i] != src[i] + 100u) bad++;
    CHECK(bad == 0, "C60 imageStore 读回错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_view_release(iv);
    nvn_gpu_buf_release(ib, NULL, NULL);
    nvn_gpu_buf_release(sb, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C60b：Unknown 存储视图须按已启用的 without-format 写特性建管线。
static void c60b_unknown_image_tbuf(const char *dir) {
    VkBool32 enabled = g_dev.core_features.shaderStorageImageWriteWithoutFormat;
    CHECK(enabled, "C60b 宿主未启用 shaderStorageImageWriteWithoutFormat，无法验证正控");
    if (!enabled) return;
    nvn_gpu_layout_ent ent = {0};
    ent.cls = NVN_GPU_RES_IMAGE_TBUF; ent.access = NVN_GPU_ACC_W;
    ent.count = 1; ent.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = &ent; lay.nent = 1;
    char path[512]; size_t bytes = 0;
    snprintf(path, sizeof path, "%s/itbuf_unknown.comp.spv", dir);
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "C60b shader 文件缺失");
    if (!blob) return;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = blob; src.bytes = bytes; src.layout = &lay;
    nvn_gpu_err e; nvn_gpu_shader sh = NULL;
    g_dev.core_features.shaderStorageImageWriteWithoutFormat = VK_FALSE;
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60b 缺特性时 shader 仍须装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh;
        nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !cp, "C60b 缺写特性管线应 UNSUPPORTED，实 %d", r.code);
        nvn_gpu_shader_release(sh);
    }
    g_dev.core_features.shaderStorageImageWriteWithoutFormat = enabled;
    sh = NULL; r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60b 有特性时 shader 应装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(OK(r) && cp, "C60b 有写特性管线应成功：%d %s", r.code, e.why);
        if (cp) {
            const uint32_t expected[4] = {5, 6, 7, 8};
            nvn_gpu_buf b = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
            nvn_gpu_view v = NULL;
            r = nvn_gpu_view_create_buf(b, 0, sizeof expected, NVN_FORMAT_R32UI, &v, &e);
            CHECK(OK(r) && v, "C60b 视图创建失败：%d %s", r.code, e.why);
            if (v) {
                nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
                nvn_gpu_binding bind = {0}; bind.cls = NVN_GPU_RES_IMAGE_TBUF; bind.u.view = v;
                nvn_gpu_bind(c, NVN_GPU_SH_CS, &bind, 1);
                nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
                r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
                CHECK(OK(r), "C60b commit：%d %s", r.code, e.why);
                r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
                CHECK(OK(r), "C60b wait：%d %s", r.code, e.why);
                CHECK(!memcmp(buf_cpu(b), expected, sizeof expected), "C60b 写回不符");
                nvn_gpu_view_release(v);
            }
            nvn_gpu_buf_release(b, NULL, NULL);
        }
        nvn_gpu_shader_release(sh);
    }
    free(blob);
}

// C60c：Unknown 存储读取独立检查 read-without-format 特性并执行读回。
static void c60c_unknown_image_tbuf_read(const char *dir) {
    VkBool32 enabled = g_dev.core_features.shaderStorageImageReadWithoutFormat;
    CHECK(enabled, "C60c 宿主未启用 read-without-format，无法验证正控");
    if (!enabled) return;
    nvn_gpu_layout_ent ent[2] = {0};
    ent[0].cls = NVN_GPU_RES_IMAGE_TBUF; ent[0].access = NVN_GPU_ACC_R;
    ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W;
    ent[1].binding = 1; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ent; lay.nent = 2;
    char path[512]; size_t bytes = 0;
    snprintf(path, sizeof path, "%s/itbuf_unknown_read.comp.spv", dir);
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "C60c shader 文件缺失");
    if (!blob) return;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = blob; src.bytes = bytes; src.layout = &lay;
    nvn_gpu_err e; nvn_gpu_shader sh = NULL;
    g_dev.core_features.shaderStorageImageReadWithoutFormat = VK_FALSE;
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60c 缺特性时 shader 装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !cp, "C60c 缺读特性应 UNSUPPORTED：%d", r.code);
        nvn_gpu_shader_release(sh);
    }
    g_dev.core_features.shaderStorageImageReadWithoutFormat = enabled;
    sh = NULL; r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60c 有特性时 shader 装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(OK(r) && cp, "C60c 管线：%d %s", r.code, e.why);
        if (cp) {
            const uint32_t expected[4] = {19, 23, 29, 31};
            nvn_gpu_buf input = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, expected);
            nvn_gpu_buf output = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
            nvn_gpu_view v = NULL;
            r = nvn_gpu_view_create_buf(input, 0, sizeof expected, NVN_FORMAT_R32UI, &v, &e);
            CHECK(OK(r) && v, "C60c 视图：%d %s", r.code, e.why);
            if (v) {
                nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
                nvn_gpu_binding b[2] = {0};
                b[0].cls = NVN_GPU_RES_IMAGE_TBUF; b[0].u.view = v;
                b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = output; b[1].u.b.len = sizeof expected;
                nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2); nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
                r = nvn_gpu_cmd_commit(c, 110, on_done, NULL, &e);
                CHECK(OK(r), "C60c commit：%d %s", r.code, e.why);
                r = nvn_gpu_fence_wait(110, 5000000000ull, &e);
                CHECK(OK(r), "C60c wait：%d %s", r.code, e.why);
                CHECK(!memcmp(buf_cpu(output), expected, sizeof expected), "C60c 读回不符");
                nvn_gpu_view_release(v);
            }
            nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL);
        }
        nvn_gpu_shader_release(sh);
    }
    free(blob);
}

// C61：IMAGE（storage image）。uimage2D r32ui 4x4 imageStore(x,y)=x+4y+1 → 1..16。
static void c61_storage_image(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_IMAGE; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "img.comp", &cl, &sh);
    CHECK(cp != NULL, "C61 cpipe 为空");
    if (!cp) return;
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_R32UI, NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf rb = make_buf(16 * 4, NVN_GPU_MEM_SHARED, NULL);
    const uint32_t zeros[16] = {0};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, zeros, 16, 64);   // 上传后贴图落到 GENERAL，可进存储用途
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_IMAGE; b.u.view = v;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 108, on_done, NULL, &e);
    CHECK(OK(r), "C61 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(108, 5000000000ull, &e);
    CHECK(OK(r), "C61 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(rb);
    int bad = 0;
    for (int i = 0; i < 16; i++) if (rd[i] != (uint32_t)i + 1) bad++;
    CHECK(bad == 0, "C61 imageStore 读回错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C62：TEX_SAMP 组合采样项走计算路径。texelFetch(1,0) = 红 {1,0,0,1} → 位模式 {3F800000,0,0,3F800000}。
static void c62_tsamp_cs(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX_SAMP; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_W;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = ents; cl.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tsamp.comp", &cl, &sh);
    CHECK(cp != NULL, "C62 cpipe 为空");
    if (!cp) return;
    static const uint8_t texels[16] = {0, 0, 0, 255,  255, 0, 0, 255,
                                       0, 0, 0, 255,  0, 0, 0, 255};
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v = make_view(t);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_err e;
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C62 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_buf out = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 8, 16);
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[2];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX_SAMP; bs[0].u.ts.view = v; bs[0].u.ts.samp = samp;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = out; bs[1].u.b.off = 0; bs[1].u.b.len = 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 2);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 109, on_done, NULL, &e);
    CHECK(OK(r), "C62 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(109, 5000000000ull, &e);
    CHECK(OK(r), "C62 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(out);
    CHECK(rd[0] == 0x3F800000u && rd[1] == 0 && rd[2] == 0 && rd[3] == 0x3F800000u,
          "C62 texelFetch = %08x %08x %08x %08x，期望 3f800000 0 0 3f800000", rd[0], rd[1], rd[2], rd[3]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(out, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C63：描述符数组元素位。t[2] 绑定 elem0=红、elem1=绿，着色器取 t[1] → 全绿。
// 若后端把 elem 当 0 写，elem1 会盖到元素 0、t[1] 留空——本例必红。
static void c63_desc_array_elem(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 2; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "arr2.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 arr2 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C63 pipe：%d %s", e.rc.code, e.why);
    const uint8_t red[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const uint8_t grn[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    nvn_gpu_tex tr = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex tg = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view vr = make_view(tr), vg = make_view(tg);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C63 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tr, 0, 0, NULL, red, 8, 16);
    nvn_gpu_tex_upload(c, tg, 0, 0, NULL, grn, 8, 16);
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding bs[3];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX; bs[0].elem = 0; bs[0].u.view = vr;
    bs[1].ent = 0; bs[1].cls = NVN_GPU_RES_TEX; bs[1].elem = 1; bs[1].u.view = vg;
    bs[2].ent = 1; bs[2].cls = NVN_GPU_RES_SAMP; bs[2].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 3);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 110, on_done, NULL, &e);
    CHECK(OK(r), "C63 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(110, 5000000000ull, &e);
    CHECK(OK(r), "C63 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, "C63");
    nvn_gpu_view_release(vr); nvn_gpu_view_release(vg);
    nvn_gpu_tex_release(tr); nvn_gpu_tex_release(tg);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C64：VA 类别显式报红——契约未定非 BDA 消费方式，管线创建期必须 E_UNSUPPORTED，不许静默成功。
static void c64_va_red(const char *dir) {
    nvn_gpu_err e;
    // 计算管线
    {
        nvn_gpu_layout_ent ce;
        memset(&ce, 0, sizeof ce);
        ce.set = 0; ce.cls = NVN_GPU_RES_VA; ce.access = NVN_GPU_ACC_R; ce.binding = 0; ce.count = 1;
        ce.stages = 1u << NVN_GPU_SH_CS;
        nvn_gpu_layout cl;
        memset(&cl, 0, sizeof cl);
        cl.ent = &ce; cl.nent = 1;
        nvn_gpu_shader sh = load_one_shader(dir, "comp.comp", NVN_GPU_SH_CS, &cl);
        if (sh) {
            nvn_gpu_stage_desc cs;
            memset(&cs, 0, sizeof cs);
            cs.sh = sh;
            nvn_gpu_pipe cp = NULL;
            nvn_gpu_rc r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C64 计算 VA 应 E_UNSUPPORTED，实为 %d", r.code);
            nvn_gpu_shader_release(sh);
        }
    }
    // 图形管线
    {
        nvn_gpu_layout_ent fe;
        memset(&fe, 0, sizeof fe);
        fe.set = 0; fe.cls = NVN_GPU_RES_VA; fe.access = NVN_GPU_ACC_R; fe.binding = 0; fe.count = 1;
        fe.stages = 1u << NVN_GPU_SH_FS;
        nvn_gpu_layout fl;
        memset(&fl, 0, sizeof fl);
        fl.ent = &fe; fl.nent = 1;
        nvn_gpu_shader vs = NULL, fs = NULL;
        void *vm = NULL, *fm = NULL;
        if (load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, &fl, &vs, &fs, &vm, &fm)) {
            nvn_gpu_pipe_desc pd;
            fill_gpipe_desc(&pd, vs, fs, 0);
            nvn_gpu_pipe pipe = NULL;
            nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C64 图形 VA 应 E_UNSUPPORTED，实为 %d", r.code);
            nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
            free(vm); free(fm);
        }
    }
}

// ---- D9 静默错复现（只复现不修）：断言的是当前实际行为；修好对应项后这些用例要翻期望 ----

// C65：只有深度附件、没有颜色附件的通道合法（阴影图、深度预通道）。清成 0.5 后读回。
// 期望（手算）：提交成功；64×64 个 32 位浮点全是 0.5。
static void c65_depth_only_pass(void) {
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.depth.view = v; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 0.5f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 111, on_done, NULL, &e);
    CHECK(OK(r), "C65 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(111, 5000000000ull, &e);
    CHECK(OK(r), "C65 wait：%d %s", r.code, e.why);
    { const float *f = (const float *)buf_cpu(rb); int bad = -1; for (int i = 0; i < 64 * 64; i++) if (f[i] != 0.5f) { bad = i; break; }
      CHECK(bad < 0, "C65 深度读回第 %d 个 = %g，期望 0.5", bad, bad < 0 ? 0.0 : (double)f[bad]); }
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
}

// C66：一段命令里的描述符组数不设上限。测试的链接包装器模拟严格执行池上限的驱动：每个池第 65 组起返回池耗尽。
// 一段里连发 70 次派发（每次 1 组）：第 65 次在第一个池上被拒 → 后端换新池重分 → 新池上分第 65–70 次共 6 组。
// 期望（手算）：被拒 1 次；最后一个池上累计请求 6 组；提交成功；派发的结果正确（comp.comp 把 v[i] 写成 i + 1）。
static void c66_pool_sets(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "comp.comp", &cl, &sh);
    CHECK(cp != NULL, "C66 cpipe 为空");
    if (!cp) return;
    uint64_t stride = g_dev.properties.limits.minStorageBufferOffsetAlignment; if (stride < 64) stride = 64;
    nvn_gpu_buf buf = make_buf(stride * 70, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = 64;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    // 计算派发也复用；真实重复派发不得多分配，后面的压力段仍让每笔offset变化。
    b.u.b.off = 69 * stride; nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    unsigned before = g_desc_sets_total; nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    CHECK(g_desc_sets_total - before == 1, "C66 首派发分配1组");
    before = g_desc_sets_total; nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1); nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    CHECK(g_desc_sets_total == before, "C66 相同派发不分配");
    g_c66_sets_requested = g_c66_rejects = 0;
    g_c66_pool = VK_NULL_HANDLE;
    g_c66_inject_pool_limit = 1;
    for (int i = 0; i < 70; i++) {
        b.u.b.off = (uint64_t)i * stride; nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
        nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    }
    g_c66_inject_pool_limit = 0;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 112, on_done, NULL, &e);
    CHECK(g_memo_on || (g_c66_rejects == 1 && g_c66_sets_requested == 6), "C66 应被拒 1 次、新池上请求 6 组，实为 被拒 %u 次、%u 组", g_c66_rejects, g_c66_sets_requested);
    CHECK(OK(r), "C66 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(112, 5000000000ull, &e);
    CHECK(OK(r), "C66 wait：%d %s", r.code, e.why);
    for (int k = 0; k < 70; k++) { const uint32_t *v = (const uint32_t *)(buf_cpu(buf) + k * stride); int bad = -1;
        for (int i = 0; i < 16; i++) if (v[i] != (uint32_t)i + 1) { bad = i; break; }
        CHECK(bad < 0, "C66 第%d笔输出项%d不符", k, bad); }
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C67：origin_ll=1（NVN 窗口原点在左下）时后端不翻视口：这种约定下图是自下而上存的，行号 0 就是「下」，
// 视口 {0,0,64,32} 落在行 0–31；呈现时由 present_desc.flip_y 整张翻过来。原点在左上的翻转见 C73。
static void c67_viewport_yflip(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C67 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;   // 蓝底衬绿
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn); dyn.origin_ll = (uint8_t)!g_tri_origin_ul;   /* 原点在左下 = Vulkan 的自然方向 */
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 32; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.origin_ll = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 113, on_done, NULL, &e);
    CHECK(OK(r), "C67 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(113, 5000000000ull, &e);
    CHECK(OK(r), "C67 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 0, 255, 0, "C67 行 0–31（视口内，绿）");
    expect_rect(buf_cpu(rb), 0, 32, 64, 32, 0, 0, 255, "C67 行 32–63（视口外，保持蓝底）");
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C68：模板的比较函数 / 三种操作进管线，读写掩码与参考值走动态状态。两笔：
//   ① 只在左半屏（裁剪）画绿：函数 ALWAYS、通过时 REPLACE、参考值 1 → 左半屏模板变 1；
//   ② 整屏画蓝：函数 NOTEQUAL、参考值 1、操作全 KEEP → 只有模板还是 0 的右半屏画得上。
// 期望（手算）：左半绿、右半蓝。函数没进管线（全 0 = NEVER）时两笔都画不上，整屏是黑的清屏色。
static void c68_stencil_ops(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.sfmt = NVN_FORMAT_DEPTH24_STENCIL8; pd.ds.st = 1;
    pd.ds.sf_f = 8; pd.ds.sf_b = 8;            // NVN StencilFunc：8 = ALWAYS
    pd.ds.fail_f = 2; pd.ds.zfail_f = 2; pd.ds.zpass_f = 3;   // NVN op 值 = MTLStencilOperation+1：2=ZERO、3=REPLACE
    pd.ds.fail_b = 2; pd.ds.zfail_b = 2; pd.ds.zpass_b = 3;
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C68 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader vs2 = NULL, fs2 = NULL; void *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd2;
    fill_gpipe_desc(&pd2, vs2, fs2, NVN_FORMAT_DEPTH24_STENCIL8);
    pd2.sfmt = NVN_FORMAT_DEPTH24_STENCIL8; pd2.ds.st = 1;
    pd2.ds.sf_f = 6; pd2.ds.sf_b = 6;          // 6 = NOTEQUAL
    pd2.ds.fail_f = pd2.ds.zfail_f = pd2.ds.zpass_f = 1;   // 1 = KEEP
    pd2.ds.fail_b = pd2.ds.zfail_b = pd2.ds.zpass_b = 1;
    nvn_gpu_pipe pipe2 = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C68 pipe2：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f; p.clear_stencil = 0;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF;
    dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    dyn.st_ref[0] = dyn.st_ref[1] = 1;
    dyn.sc[2] = 32;   // ① 只画左半屏
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_set_pipe(c, pipe2);   // ② 整屏、NOTEQUAL 1
    dyn.sc[2] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 114, on_done, NULL, &e);
    CHECK(OK(r), "C68 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(114, 5000000000ull, &e);
    CHECK(OK(r), "C68 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C68 左半（①写了模板 1，②被 NOTEQUAL 挡住：绿）");
    expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C68 右半（模板还是 0，②画上：蓝）");
    nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2); free(vm2); free(fm2);
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// 平铺在 z = 0.5 的整屏三角形（C74 / C75 的深度测试用）
static nvn_gpu_buf half_depth_vb(void) {
    const float verts[3][4] = {{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}};
    return make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
}

// C76：只有顶点阶段的管线（没有片元着色器）在只有深度附件的通道里写深度。深度清成 1，整屏三角形平铺在 z = 0.5，深度函数 ALWAYS、写深度。
// 期望（手算）：提交成功；读回的 64×64 个 32 位浮点全是 0.5。管线没建成或没写深度则仍是 1。
static void c76_vs_only_depth(const char *dir) {
    nvn_gpu_shader vs = load_one_shader(dir, "tri3.vert", NVN_GPU_SH_VS, NULL);
    if (!vs) return;
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, NULL, NVN_FORMAT_DEPTH32F);
    pd.cfmt[0] = 0; pd.chmask.mask = 0;
    pd.ds.dt = 1; pd.ds.dw = 1; pd.ds.df = 8;   // 深度测试开、写深度、ALWAYS
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C76 pipe：%d %s", e.rc.code, e.why);
    if (!pipe) { nvn_gpu_shader_release(vs); return; }
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = half_depth_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.depth.view = v; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE; p.clear_depth = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn); nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 118, on_done, NULL, &e);
    CHECK(OK(r), "C76 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(118, 5000000000ull, &e);
    CHECK(OK(r), "C76 wait：%d %s", r.code, e.why);
    { const float *f = (const float *)buf_cpu(rb); int bad = -1; for (int i = 0; i < 64 * 64; i++) if (f[i] != 0.5f) { bad = i; break; }
      CHECK(bad < 0, "C76 深度读回第 %d 个 = %g，期望 0.5", bad, bad < 0 ? 0.0 : (double)f[bad]); }
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs);
}

// C77：颜色槽里有空位的通道（槽 0 没有附件、槽 1 有）。槽 1 清成蓝，片元着色器只写输出 1（绿），整屏画一笔。
// 期望（手算）：槽 1 的图整屏绿。空位没处理对的话：通道 / 管线建不出来（提交失败），或者写到别处、槽 1 仍是蓝。
// C78：颜色写掩码为 0 的绘制一个通道都不写。同一张图清成蓝后，用写掩码 0 的管线整屏画绿。期望：仍然整屏蓝。
// C197：通道对象缓存。同一组目标反复开通道，渲染通道对象和帧缓冲只建一次；视图销毁后帧缓冲作废、换新视图再开要重建一个，
// 渲染通道对象（只看附件描述）不重建。期望的次数按「开了几次、目标变没变」数出来，不看实现。
uint64_t nvn_vk_dbg_created(int which);
static void c197_pass_cache(void) {
    nvn_gpu_err e; nvn_gpu_rc r;
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1; p.color[0].clear.f[3] = 1; p.w = 64; p.h = 64; p.layers = 1;
    uint64_t rp0 = nvn_vk_dbg_created(0), fb0 = nvn_vk_dbg_created(1);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    r = nvn_gpu_cmd_commit(c, 197, on_done, NULL, &e); CHECK(OK(r), "C197 commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(197, 5000000000ull, &e); CHECK(OK(r), "C197 wait 1：%d %s", r.code, e.why);
    CHECK(nvn_vk_dbg_created(0) - rp0 <= 1 && nvn_vk_dbg_created(1) - fb0 == 1, "C197 同一段里同一目标开两次：期望新建渲染通道至多 1（前面的用例建过同样的附件描述就是 0）、帧缓冲 1 → %llu、%llu",
          (unsigned long long)(nvn_vk_dbg_created(0) - rp0), (unsigned long long)(nvn_vk_dbg_created(1) - fb0));
    uint64_t rp1 = nvn_vk_dbg_created(0);
    c = begin_cmd();
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    r = nvn_gpu_cmd_commit(c, 198, on_done, NULL, &e); CHECK(OK(r), "C197 commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(198, 5000000000ull, &e); CHECK(OK(r), "C197 wait 2：%d %s", r.code, e.why);
    CHECK(nvn_vk_dbg_created(0) == rp1 && nvn_vk_dbg_created(1) - fb0 == 1, "C197 下一段再开同一目标：期望渲染通道不再新建、帧缓冲仍是 1 → %llu、%llu",
          (unsigned long long)(nvn_vk_dbg_created(0) - rp0), (unsigned long long)(nvn_vk_dbg_created(1) - fb0));
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 255, 0, 0, "C197 沿用缓存的帧缓冲清成红");
    nvn_gpu_view_release(ov);
    nvn_gpu_view ov2 = make_view(out);
    p.color[0].view = ov2; p.color[0].clear.f[0] = 0; p.color[0].clear.f[2] = 1;
    c = begin_cmd();
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    r = nvn_gpu_cmd_commit(c, 199, on_done, NULL, &e); CHECK(OK(r), "C197 commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(199, 5000000000ull, &e); CHECK(OK(r), "C197 wait 3：%d %s", r.code, e.why);
    CHECK(nvn_vk_dbg_created(0) == rp1 && nvn_vk_dbg_created(1) - fb0 == 2, "C197 旧视图销毁、换新视图再开：期望渲染通道不再新建、帧缓冲 2 → %llu、%llu",
          (unsigned long long)(nvn_vk_dbg_created(0) - rp0), (unsigned long long)(nvn_vk_dbg_created(1) - fb0));
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 0, 255, "C197 新视图的帧缓冲清成蓝");
    nvn_gpu_view_release(ov2); nvn_gpu_tex_release(out);
}

static void c77_null_color_slot(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "out1_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 out1_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    pd.cfmt[0] = 0; pd.cfmt[1] = NVN_FORMAT_RGBA8; pd.chmask.mask = 0xF0;   // 槽 1 四个通道都写
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C77 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    if (pipe) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 2; p.color[1].view = ov; p.color[1].load = NVN_GPU_LOAD_CLEAR; p.color[1].store = NVN_GPU_STORE_STORE;
        p.color[1].clear.f[2] = 1; p.color[1].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_dyn dyn; full_dyn(&dyn); nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 119, on_done, NULL, &e);
        CHECK(OK(r), "C77 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(119, 5000000000ull, &e);
        CHECK(OK(r), "C77 wait：%d %s", r.code, e.why);
        expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, "C77 槽 1（空位在槽 0）整屏绿");
    }
    // C78
    nvn_gpu_shader vs2 = NULL, fs2 = NULL; void *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd2;
    fill_gpipe_desc(&pd2, vs2, fs2, 0);
    pd2.chmask.mask = 0;
    nvn_gpu_pipe pipe2 = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C78 pipe：%d %s", e.rc.code, e.why);
    if (pipe2) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_set_pipe(c, pipe2);
        nvn_gpu_dyn dyn; full_dyn(&dyn); nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 120, on_done, NULL, &e);
        CHECK(OK(r), "C78 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(120, 5000000000ull, &e);
        CHECK(OK(r), "C78 wait：%d %s", r.code, e.why);
        expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 0, 255, "C78 写掩码 0：仍是清屏的蓝");
    }
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2);
    free(vm); free(fm); free(vm2); free(fm2);
}

// C74：通道内按矩形清（契约 v4 nvn_gpu_clear_rect）。通道开头整屏清成黑 / 深度 1 / 模板 0，然后三次矩形清：
//   ① 颜色：x 8–31、y 16–47 清成红；② 只清模板：左半屏 x 0–31 写 1；③ 只清深度：右上 x 32–63、y 0–31 写 0。
//   再整屏画一笔蓝（z = 0.5）：深度 LESS、不写深度；模板 NOTEQUAL 1、操作全 KEEP。
// 期望（手算）：左半屏模板是 1，蓝画不上 → ① 的矩形是红，其余是黑；右上深度是 0，0.5 < 0 不成立 → 黑；右下深度 1、模板 0 → 蓝。
// ②③ 各只动一个面：② 若连深度一起清（清成 0）不影响左半结论，但 ③ 若连模板一起动、或 ② 没清到，右上 / 左半会出蓝。
static void c74_clear_rect(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.sfmt = NVN_FORMAT_DEPTH24_STENCIL8;
    pd.ds.dt = 1; pd.ds.dw = 0; pd.ds.df = 2;   // 深度 LESS，不写
    pd.ds.st = 1; pd.ds.sf_f = 6; pd.ds.sf_b = 6;   // 模板 NOTEQUAL
    pd.ds.fail_f = pd.ds.zfail_f = pd.ds.zpass_f = 1; pd.ds.fail_b = pd.ds.zfail_b = pd.ds.zpass_b = 1;   // KEEP
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C74 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = half_depth_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f; p.clear_stencil = 0; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff;
    cl.color = 0; cl.value.f[0] = 1; cl.value.f[3] = 1; cl.x = 8; cl.y = 16; cl.w = 24; cl.h = 32;
    nvn_gpu_clear_rect(c, &cl);
    memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.color = -1; cl.stencil = 1; cl.stencil_value = 1; cl.depth_value = 0.25f /* 不该被用到 */; cl.x = 0; cl.y = 0; cl.w = 32; cl.h = 64;
    nvn_gpu_clear_rect(c, &cl);
    memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.color = -1; cl.depth = 1; cl.depth_value = 0; cl.stencil_value = 1 /* 不该被用到 */; cl.x = 32; cl.y = 0; cl.w = 32; cl.h = 32;
    nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF; dyn.st_ref[0] = dyn.st_ref[1] = 1;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 115, on_done, NULL, &e);
    CHECK(OK(r), "C74 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(115, 5000000000ull, &e);
    CHECK(OK(r), "C74 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 8, 16, 24, 32, 255, 0, 0, "C74 颜色矩形（红）");
    expect_rect(buf_cpu(rb), 0, 0, 8, 64, 0, 0, 0, "C74 左半、矩形左侧（模板 1 挡住蓝：黑）");
    expect_rect(buf_cpu(rb), 8, 0, 24, 16, 0, 0, 0, "C74 左半、矩形上方（黑）");
    expect_rect(buf_cpu(rb), 8, 48, 24, 16, 0, 0, 0, "C74 左半、矩形下方（黑）");
    expect_rect(buf_cpu(rb), 32, 0, 32, 32, 0, 0, 0, "C74 右上（深度清成 0，LESS 不过：黑）");
    expect_rect(buf_cpu(rb), 32, 32, 32, 32, 0, 0, 255, "C74 右下（深度 1、模板 0：蓝）");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv); nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C79：带写掩码的清除（契约 v5）。通道开头颜色清成 (0.25, 0.5, 0.75, 1) = 64,128,191,255，模板清成 0x0F。
//   ① 颜色：左半屏只写绿通道，值 (1,1,1,0) → 左半 64,255,191,255（红蓝 alpha 不动）；
//   ② 模板：上半屏只写 0x30 两位，值 0xF0 → 上半模板 = 0x0F 保留位 | 0x30 = 0x3F，下半仍是 0x0F；
//   再整屏画一笔蓝，模板 EQUAL 0x3F、操作全 KEEP → 只有上半画得上。
// 期望（手算）：上半蓝；左下 64,255,191；右下 64,128,191。
//   掩码没生效的反例：① 四通道全写则左下是 255,255,255,0；② 八位全写则上半模板是 0xF0，蓝一笔都画不上。
static void c79_masked_clear(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.sfmt = NVN_FORMAT_DEPTH24_STENCIL8;
    pd.ds.st = 1; pd.ds.sf_f = 3; pd.ds.sf_b = 3;   // 模板 EQUAL
    pd.ds.fail_f = pd.ds.zfail_f = pd.ds.zpass_f = 1; pd.ds.fail_b = pd.ds.zfail_b = pd.ds.zpass_b = 1;   // KEEP
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C79 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 0.25f; p.color[0].clear.f[1] = 0.5f; p.color[0].clear.f[2] = 0.75f; p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f; p.clear_stencil = 0x0F; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl);
    cl.color = 0; cl.color_mask = 0x2; cl.stencil_mask = 0xff; cl.value.f[0] = 1; cl.value.f[1] = 1; cl.value.f[2] = 1; cl.value.f[3] = 0; cl.x = 0; cl.y = 0; cl.w = 32; cl.h = 64;
    nvn_gpu_clear_rect(c, &cl);
    memset(&cl, 0, sizeof cl);
    cl.color = -1; cl.color_mask = 0xf; cl.stencil = 1; cl.stencil_mask = 0x30; cl.stencil_value = 0xF0; cl.x = 0; cl.y = 0; cl.w = 64; cl.h = 32;
    nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF; dyn.st_ref[0] = dyn.st_ref[1] = 0x3F;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 121, on_done, NULL, &e);
    CHECK(OK(r), "C79 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(121, 5000000000ull, &e);
    CHECK(OK(r), "C79 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 0, 0, 255, "C79 上半（模板 0x3F：蓝画上）");
    expect_rect(buf_cpu(rb), 0, 32, 32, 32, 64, 255, 191, "C79 左下（只写了绿通道）");
    expect_rect(buf_cpu(rb), 32, 32, 32, 32, 64, 128, 191, "C79 右下（没动）");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv); nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C80：正面绕序（契约 v6）。管线描述里的 poly.front 是 y 朝上的规范化设备坐标里的绕序；整屏三角形 (-1,-1) (3,-1) (-1,3) 在那个坐标里是逆时针。
//   剔除背面时：正面 = 逆时针的管线画得上，正面 = 顺时针的画不上——窗口原点在左下（视口不翻）和在左上（视口翻）两种情况都一样。
// 期望（手算）：每种原点下，先用「正面逆时针」画绿 → 整屏绿；再用「正面顺时针」画蓝 → 被剔掉，仍然整屏绿。
static void c80_front_face(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL, vs2 = NULL, fs2 = NULL; void *vm = NULL, *fm = NULL, *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    for (int ll = 1; ll >= 0; ll--) {
        nvn_gpu_pipe_desc pd, pd2; nvn_gpu_err e; nvn_gpu_pipe pipe = NULL, pipe2 = NULL;
        fill_gpipe_desc(&pd, vs, fs, 0); pd.poly.cull = 2; pd.poly.front = 1; pd.origin_ll = (uint8_t)ll;      // 剔背面，正面逆时针
        fill_gpipe_desc(&pd2, vs2, fs2, 0); pd2.poly.cull = 2; pd2.poly.front = 0; pd2.origin_ll = (uint8_t)ll;  // 剔背面，正面顺时针
        CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C80 pipe：%d %s", e.rc.code, e.why);
        CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C80 pipe2：%d %s", e.rc.code, e.why);
        if (!pipe || !pipe2) break;
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.origin_ll = (uint8_t)ll;
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
        nvn_gpu_set_pipe(c, pipe2); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
        uint64_t sig = 122 + (uint64_t)(1 - ll);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
        CHECK(OK(r), "C80 commit（原点左下 = %d）：%d %s", ll, r.code, e.why);
        r = nvn_gpu_fence_wait(sig, 5000000000ull, &e);
        CHECK(OK(r), "C80 wait：%d %s", r.code, e.why);
        expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, ll ? "C80 原点左下：逆时针画上、顺时针被剔（整屏绿）" : "C80 原点左上：逆时针画上、顺时针被剔（整屏绿）");
    }
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2);
    free(vm); free(fm); free(vm2); free(fm2);
}

// C81：整数格式的贴图绑进采样槽。游戏的采样器是线性过滤也照绑（整数格式不能线性过滤：后端给它换最近点的那一个）。
// 期望（手算）：2x2 的 R32UI 贴图存四个位模式（含浮点里是非数、次正规数的那两种），画到 2x2 的 R32UI 目标，逐像素读回来与存进去的位相同；校验层 0 条。
static void c81_int_sampled(const char *dir) {
    static const uint32_t bits[4] = {0x12345678u, 0xFFFFFFFFu, 0x00000123u, 0x7FC00001u};
    nvn_gpu_layout_ent ent; memset(&ent, 0, sizeof ent);
    ent.set = 0; ent.cls = NVN_GPU_RES_TEX_SAMP; ent.access = NVN_GPU_ACC_R; ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = &ent; fl.nent = 1;
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "tex_u.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 tex_u"); return; }
    nvn_gpu_tex src = make_tex(2, 2, NVN_FORMAT_R32UI, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex out = make_tex(2, 2, NVN_FORMAT_R32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view sv = make_view(src), ov = make_view(out);
    nvn_gpu_buf rb = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_err e; samp_par sp; memset(&sp, 0, sizeof sp);
    sp.magf = NVN_MAG_FILTER_LINEAR; sp.minf = NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR; sp.lod_max = 1000.0f;
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C81 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_pipe_desc pd; nvn_gpu_pipe pipe = NULL;
    fill_gpipe_desc(&pd, vs, fs, 0); pd.cfmt[0] = NVN_FORMAT_R32UI;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C81 pipe：%d %s", e.rc.code, e.why);
    if (pipe && samp) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_tex_upload(c, src, 0, 0, NULL, bits, 8, 16);
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.w = 2; p.h = 2; p.layers = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.vp[0][2] = 2; dyn.vp[0][3] = 2; dyn.sc[2] = 2; dyn.sc[3] = 2;
        nvn_gpu_binding b; memset(&b, 0, sizeof b); b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = sv; b.u.ts.samp = samp;
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1); nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 8, 16);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 124, on_done, NULL, &e);
        CHECK(OK(r), "C81 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(124, 5000000000ull, &e);
        CHECK(OK(r), "C81 wait：%d %s", r.code, e.why);
        uint32_t got[4]; memcpy(got, buf_cpu(rb), 16);
        CHECK(!memcmp(got, bits, 16), "C81 整数贴图读回的位：%#x %#x %#x %#x（期望 %#x %#x %#x %#x）", got[0], got[1], got[2], got[3], bits[0], bits[1], bits[2], bits[3]);
    }
    if (samp) nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov); nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C82：遮挡查询。一段声明 4 个槽：槽 0 整屏三角形（64x64 目标）、槽 1 同一个三角形裁剪到 16x16、槽 2 开了不画、槽 3 不用。
// 期望（手算）：完成回调里各槽样本数 = 4096、256、0、0。
static uint64_t g_c82_occ[4]; static uint32_t g_c82_n;
static void c82_done(void *user, const nvn_gpu_done *d) { (void)user; g_c82_n = d->nocclusion; for (uint32_t i = 0; i < 4 && i < d->nocclusion && d->occlusion; i++) g_c82_occ[i] = d->occlusion[i]; on_done(NULL, d); }
static void c82_occlusion(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_pipe_desc pd; nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    fill_gpipe_desc(&pd, vs, fs, 0);
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C82 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_cmd c = NULL; nvn_gpu_cmd_desc cd = {4, "C82"};
    CHECK(OK(nvn_gpu_cmd_begin(&cd, &c, &e)), "C82 cmd_begin：%d %s", e.rc.code, e.why);
    if (pipe && c) {
        nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
        p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
        p.w = 64; p.h = 64; p.layers = 1; p.occlusion = 1;
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_dyn dyn; full_dyn(&dyn);
        nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_occlusion(c, 0); nvn_gpu_draw(c, &da);
        dyn.sc[0] = 8; dyn.sc[1] = 8; dyn.sc[2] = 16; dyn.sc[3] = 16; nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_occlusion(c, 1); nvn_gpu_draw(c, &da);
        nvn_gpu_occlusion(c, 2);
        nvn_gpu_occlusion(c, -1); nvn_gpu_draw(c, &da);   // 停了以后画的不计
        nvn_gpu_pass_end(c);
        g_c82_n = 0; memset(g_c82_occ, 0xff, sizeof g_c82_occ);
        nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 125, c82_done, NULL, &e);
        CHECK(OK(r), "C82 commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(125, 5000000000ull, &e);
        CHECK(OK(r), "C82 wait：%d %s", r.code, e.why);
        CHECK(g_c82_n == 4 && g_c82_occ[0] == 4096 && g_c82_occ[1] == 256 && g_c82_occ[2] == 0 && g_c82_occ[3] == 0, "C82 各槽样本数：%u 槽 %llu %llu %llu %llu（期望 4 槽 4096 256 0 0）", g_c82_n,
              (unsigned long long)g_c82_occ[0], (unsigned long long)g_c82_occ[1], (unsigned long long)g_c82_occ[2], (unsigned long long)g_c82_occ[3]);
    }
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out); nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C83：带缩放 / 换格式的贴图拷贝。
// 期望（手算）：① 8x8 的 RGBA8 左半红右半蓝，最近点缩到 4x4 → 左两列红、右两列蓝；
//   ② 2x2 的 R11G11B10F 全是 (0.5, 0.25, 1.0)（位模式 R 0x380、G 0x340、B 0x1E0），线性拷到 2x2 的 RGBA8 → (127 或 128, 64 ± 1, 255, 255)。
static void c83_blit(void) {
    nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
    CHECK(caps.caps & NVN_GPU_CAP_BLIT_SCALED, "C83 后端声明原生缩放拷贝");
    nvn_gpu_tex a = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED), b = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex f = make_tex(2, 2, NVN_FORMAT_R11G11B10F, NVN_GPU_TEX_SAMPLED), g2 = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view av = make_view(a), bv = make_view(b), fv = make_view(f), gv = make_view(g2);
    nvn_gpu_buf rb = make_buf(4 * 4 * 4 + 2 * 2 * 4, NVN_GPU_MEM_SHARED, NULL);
    uint8_t px[8 * 8 * 4]; for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t *p = px + (y * 8 + x) * 4; p[0] = x < 4 ? 255 : 0; p[1] = 0; p[2] = x < 4 ? 0 : 255; p[3] = 255; }
    uint32_t fw[4]; for (int i = 0; i < 4; i++) fw[i] = 0x380u | 0x340u << 11 | 0x1E0u << 22;
    nvn_gpu_err e; nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, a, 0, 0, NULL, px, 32, sizeof px);
    nvn_gpu_tex_upload(c, f, 0, 0, NULL, fw, 8, 16);
    NVNcopyRegion r8 = {0, 0, 0, 8, 8, 1}, r4 = {0, 0, 0, 4, 4, 1}, r2 = {0, 0, 0, 2, 2, 1};
    nvn_gpu_blit(c, av, &r8, bv, &r4, 0);
    nvn_gpu_blit(c, fv, &r2, gv, &r2, 1);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_copy_tex_buf(c, g2, 0, 0, NULL, rb, 64, 8, 16);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 126, on_done, NULL, &e);
    CHECK(OK(r), "C83 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(126, 5000000000ull, &e);
    CHECK(OK(r), "C83 wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb); int bad = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = o + (y * 4 + x) * 4; if (p[0] != (x < 2 ? 255 : 0) || p[1] != 0 || p[2] != (x < 2 ? 0 : 255) || p[3] != 255) bad++; }
    CHECK(!bad, "C83 缩到 4x4：%d 个像素不符（期望左两列红、右两列蓝）", bad);
    bad = 0; for (int i = 0; i < 4; i++) { const uint8_t *p = o + 64 + i * 4; if (p[0] < 127 || p[0] > 128 || p[1] < 63 || p[1] > 65 || p[2] != 255 || p[3] != 255) bad++; }
    CHECK(!bad, "C83 R11G11B10F → RGBA8：%d 个像素不符，首像素 %u %u %u %u（期望 127–128, 63–65, 255, 255）", bad, o[64], o[65], o[66], o[67]);
    nvn_gpu_view_release(av); nvn_gpu_view_release(bv); nvn_gpu_view_release(fv); nvn_gpu_view_release(gv);
    nvn_gpu_tex_release(a); nvn_gpu_tex_release(b); nvn_gpu_tex_release(f); nvn_gpu_tex_release(g2); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C84：带校验层在手机上报过的几类用法各走一遍（整套用例在校验层下跑，报一条就是红），内容按手算的期望读回。
//   ① 两级贴图的默认视图（取全部级）当颜色附件：按首级首层挂。两级先传成红，通道清成绿 → 第 0 级全绿、第 1 级还是红。
//   ② 深度模板贴图的默认视图当深度模板附件（采样形态只取深度面，附件形态两个面都在）。
//   ③ 暂存偏移是 4 不是 8 的时候上传 BC1（一块 8 字节）：偏移要对齐到块。
//   ④ D16 → 缓冲 → R16 逐字节拷（核心的影子就是这么拷的）：0x8000 原样落到 R16。
static void c84_validation_classes(void) {
    nvn_gpu_err e; nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = 8; td.d.height = 8; td.d.depth = 1; td.d.levels = 2; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex m = NULL; nvn_gpu_rc r = nvn_gpu_tex_create(&td, &m, &e);
    CHECK(OK(r), "C84 两级贴图：%d %s", r.code, e.why);
    nvn_gpu_view mv = make_view(m);
    int has_ds = (nvn_gpu_format_caps(NVN_FORMAT_DEPTH24_STENCIL8) & NVN_GPU_FMT_DEPTH) != 0, has_bc = (nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE) != 0;
    nvn_gpu_tex ds = has_ds ? make_tex(8, 8, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED) : NULL; nvn_gpu_view dv = ds ? make_view(ds) : NULL;
    nvn_gpu_tex one = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED), bc = has_bc ? make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED) : NULL;
    nvn_gpu_tex d16 = make_tex(4, 4, NVN_FORMAT_DEPTH16, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED), r16 = make_tex(4, 4, NVN_FORMAT_R16, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(256 + 64 + 32, NVN_GPU_MEM_SHARED, NULL);
    uint8_t red[8 * 8 * 4]; for (int i = 0; i < 64; i++) { red[i * 4] = 255; red[i * 4 + 1] = 0; red[i * 4 + 2] = 0; red[i * 4 + 3] = 255; }
    uint8_t px1[4] = {1, 2, 3, 4}, blk[8] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0}; uint16_t half[16]; for (int i = 0; i < 16; i++) half[i] = 0x8000;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, one, 0, 0, NULL, px1, 4, 4);                       // 暂存用掉 4 字节
    if (bc) nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blk, 8, 8);               // ③
    nvn_gpu_tex_upload(c, m, 0, 0, NULL, red, 32, 256);
    nvn_gpu_tex_upload(c, m, 1, 0, NULL, red, 16, 64);
    nvn_gpu_tex_upload(c, d16, 0, 0, NULL, half, 8, 32);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = mv; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[1] = 1; p.color[0].clear.f[3] = 1;
    if (dv) { p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE; p.stencil = p.depth; p.clear_depth = 0.5f; }
    p.w = 8; p.h = 8; p.layers = 1;
    nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);                          // ① ②
    nvn_gpu_slice sl; r = nvn_gpu_arena_alloc(c, 32, 16, &sl, &e);
    CHECK(OK(r), "C84 暂存：%d %s", r.code, e.why);
    NVNcopyRegion r4 = {0, 0, 0, 4, 4, 1};
    nvn_gpu_copy_tex_buf(c, d16, 0, 0, &r4, sl.buf, sl.off, 8, 32);          // ④
    nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, 8, 32, r16, 0, 0, &r4);
    nvn_gpu_copy_tex_buf(c, m, 0, 0, NULL, rb, 0, 32, 256);
    nvn_gpu_copy_tex_buf(c, m, 1, 0, NULL, rb, 256, 16, 64);
    nvn_gpu_copy_tex_buf(c, r16, 0, 0, NULL, rb, 320, 8, 32);
    r = nvn_gpu_cmd_commit(c, 127, on_done, NULL, &e);
    CHECK(OK(r), "C84 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(127, 5000000000ull, &e);
    CHECK(OK(r), "C84 wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb); int bad = 0;
    for (int i = 0; i < 64; i++) if (o[i * 4] != 0 || o[i * 4 + 1] != 255 || o[i * 4 + 2] != 0 || o[i * 4 + 3] != 255) bad++;
    CHECK(!bad, "C84 ① 第 0 级：%d 个像素不是绿（首像素 %u %u %u %u）", bad, o[0], o[1], o[2], o[3]);
    bad = 0; for (int i = 0; i < 16; i++) if (o[256 + i * 4] != 255 || o[256 + i * 4 + 1] != 0) bad++;
    CHECK(!bad, "C84 ① 第 1 级：%d 个像素不是红（通道只该写首级）", bad);
    bad = 0; for (int i = 0; i < 16; i++) { uint16_t v; memcpy(&v, o + 320 + i * 2, 2); if (v != 0x8000) bad++; }
    CHECK(!bad, "C84 ④ D16 → R16：%d 个像素不是 0x8000", bad);
    nvn_gpu_view_release(mv); if (dv) nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(m); if (ds) nvn_gpu_tex_release(ds); nvn_gpu_tex_release(one); if (bc) nvn_gpu_tex_release(bc); nvn_gpu_tex_release(d16); nvn_gpu_tex_release(r16); nvn_gpu_buf_release(rb, NULL, NULL);
}

// #690：原生 BC1 sRGB 的整级跨 mip 图像拷贝，按输入原始块字节核对结果。
static void c690_native_bc_image_copy(uint32_t fmt, uint32_t block_bytes, uint64_t signal) {
    if ((g_dev.bc_decode_mask & (1u << (fmt - 0x41))) ||
        !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_SAMPLE)) {
        printf("C690：格式 %#x 非原生存储，跳过原块图像拷贝\n", fmt);
        return;
    }
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = (int)fmt;
    td.d.width = 64; td.d.height = 64; td.d.depth = 1; td.d.levels = 7;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex src = NULL, dst = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &src, &e);
    CHECK(OK(r), "C690 源图创建：%d %s", r.code, e.why);
    td.d.width = 1024; td.d.height = 1024; td.d.levels = 11;
    r = nvn_gpu_tex_create(&td, &dst, &e);
    CHECK(OK(r), "C690 目标图创建：%d %s", r.code, e.why);
    uint32_t bytes = 256 * block_bytes, row = 16 * block_bytes;
    nvn_gpu_buf rb = make_buf(bytes, NVN_GPU_MEM_SHARED, NULL);
    if (src && dst && rb) {
        uint8_t blocks[4096];
        for (unsigned i = 0; i < bytes; i++) blocks[i] = (uint8_t)(i * 73u + i / block_bytes + 19u);
        NVNcopyRegion region = {0, 0, 0, 64, 64, 1};
        nvn_gpu_cmd c = begin_cmd();
        if (c) {
            nvn_gpu_tex_upload(c, src, 0, 0, &region, blocks, row, bytes);
            nvn_gpu_copy_tex(c, src, 0, 0, &region, dst, 4, 0, &region);
            nvn_gpu_copy_tex_buf(c, dst, 4, 0, &region, rb, 0, row, bytes);
            r = nvn_gpu_cmd_commit(c, signal, on_done, NULL, &e);
            CHECK(OK(r), "C690 原生 BC 格式 %#x 跨 mip 图像拷贝应成功：%d %s", fmt, r.code, e.why);
            if (OK(r)) {
                r = nvn_gpu_fence_wait(signal, 5000000000ull, &e);
                CHECK(OK(r), "C690 等待：%d %s", r.code, e.why);
                if (OK(r)) {
                    int same = !memcmp(buf_cpu(rb), blocks, bytes);
                    CHECK(same, "C690 格式 %#x 的 %u 字节原始 BC 块应逐字节相同", fmt, bytes);
                    if (same) printf("C690 跨 mip 原块一致：格式 %#x，%u 字节\n", fmt, bytes);
                }
            }
        }
    }
    if (src) nvn_gpu_tex_release(src);
    if (dst) nvn_gpu_tex_release(dst);
    if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
}

// 不满块的区域同时落在两张图的右下边缘，合法且只应替换最后一个块。
static void c690_edge_copy(void) {
    const uint32_t fmt = NVN_FORMAT_RGB_DXT1_SRGB;
    if ((g_dev.bc_decode_mask & (1u << (fmt - 0x41))) || !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_SAMPLE)) return;
    nvn_gpu_tex src = make_tex(6, 6, fmt, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex dst = make_tex(6, 6, fmt, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_buf rb = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    uint8_t a[32], b[32], want[32];
    for (unsigned i = 0; i < 32; i++) { a[i] = (uint8_t)(i + 1); b[i] = (uint8_t)(i + 101); }
    memcpy(want, b, 32); memcpy(want + 24, a + 24, 8);
    if (src && dst && rb) {
        NVNcopyRegion edge = {4, 4, 0, 2, 2, 1};
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e = {0};
        if (c) {
            nvn_gpu_tex_upload(c, src, 0, 0, NULL, a, 16, 32);
            nvn_gpu_tex_upload(c, dst, 0, 0, NULL, b, 16, 32);
            nvn_gpu_copy_tex(c, src, 0, 0, &edge, dst, 0, 0, &edge);
            nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, rb, 0, 16, 32);
            nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
            CHECK(OK(r), "C690 不满块的双图边缘应允许：%d %s", r.code, e.why);
            if (OK(r)) {
                r = nvn_gpu_fence_wait(3, 5000000000ull, &e);
                CHECK(OK(r), "C690 边缘等待：%d %s", r.code, e.why);
                if (OK(r)) {
                    int same = !memcmp(buf_cpu(rb), want, 32);
                    CHECK(same, "C690 边缘仅替换最后 8 字节块");
                    if (same) printf("C690 双图边缘：仅替换末块，32 字节一致\n");
                }
            }
        }
    }
    if (src) nvn_gpu_tex_release(src);
    if (dst) nvn_gpu_tex_release(dst);
    if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
}

// 每个非法命令都使时间线进入失败终态，调用方为每例另开会话。
static void c690_bad_region(int k) {
    const uint32_t fmt = NVN_FORMAT_RGB_DXT1_SRGB;
    if ((g_dev.bc_decode_mask & (1u << (fmt - 0x41))) || !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_SAMPLE)) return;
    uint32_t sw = k == 6 ? 6 : 8, dw = k == 7 ? 6 : 8;
    nvn_gpu_tex src = make_tex(sw, 8, fmt, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex dst = make_tex(dw, 8, fmt, NVN_GPU_TEX_SAMPLED);
    if (src && dst) {
        NVNcopyRegion sr = {0, 0, 0, 4, 4, 1}, dr = sr;
        switch (k) {
        case 0: sr.xoffset = 1; break;
        case 1: sr.yoffset = 1; break;
        case 2: dr.xoffset = 1; break;
        case 3: dr.yoffset = 1; break;
        case 4: sr.width = dr.width = 2; break;
        case 5: sr.height = dr.height = 2; break;
        case 6: case 7: sr.width = dr.width = 6; break; // 只到其中一张图的边缘
        }
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_err e = {0};
        if (c) {
            nvn_gpu_copy_tex(c, src, 0, 0, &sr, dst, 0, 0, &dr);
            nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
            CHECK(r.code == NVN_GPU_E_ARG, "C690 非法块区域 %d 应 E_ARG，实为 %d", k, r.code);
            if (r.code == NVN_GPU_E_ARG) printf("C690 非法块区域 %d：E_ARG\n", k);
        }
    }
    if (src) nvn_gpu_tex_release(src);
    if (dst) nvn_gpu_tex_release(dst);
}

// C84b（#709）：块压缩贴图与非压缩贴图之间经缓冲逐字节拷（核心的「一块当一个像素」影子就是这么拷的）。
//   BC1 4x4（一块 8 字节）传进去 → 区域按像素 4x4 拷到缓冲（一行块 8 字节）→ 拷进 1x1 的 RG32UI（一个像素 8 字节）→ 读回。
//   期望（手算）：读回的 8 个字节就是传进去的那一块。不能直接采样 BC 的设备上建不了这张贴图，跳过。
static void c84b_bc_block_copy(void) {
    if ((g_dev.bc_decode_mask & (1u << (NVN_FORMAT_RGBA_DXT1 - 0x41))) || !(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE)) { fprintf(stderr, "C84b：BC1 非原生压缩存储，跳过原块回读\n"); return; }
    nvn_gpu_tex bc = make_tex(4, 4, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED), px = make_tex(1, 1, NVN_FORMAT_RG32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    const uint8_t blk[8] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    nvn_gpu_err e; nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, bc, 0, 0, NULL, blk, 8, 8);
    nvn_gpu_slice sl; nvn_gpu_rc r = nvn_gpu_arena_alloc(c, 8, 16, &sl, &e);
    CHECK(OK(r), "C84b 暂存：%d %s", r.code, e.why);
    NVNcopyRegion rbc = {0, 0, 0, 4, 4, 1}, rpx = {0, 0, 0, 1, 1, 1};
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, &rbc, sl.buf, sl.off, 8, 8);
    nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, 8, 8, px, 0, 0, &rpx);
    nvn_gpu_copy_tex_buf(c, px, 0, 0, NULL, rb, 0, 8, 8);
    r = nvn_gpu_cmd_commit(c, 128, on_done, NULL, &e);
    CHECK(OK(r), "C84b commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(128, 5000000000ull, &e);
    CHECK(OK(r), "C84b wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb);
    CHECK(!memcmp(o, blk, 8), "C84b BC1 → 缓冲 → RG32UI：读回 %02x%02x%02x%02x%02x%02x%02x%02x，期望 123456789abcdef0", o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(px); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C85：两个实际使用的组；三笔分别留下红/红/蓝，分配增量 2/0/1。
static void c85_descriptor_reuse(const char *dir) {
    char path[1024]; size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", dir); void *vm = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri.frag.spv", dir); uint32_t *fm = read_file(path, &fsz);
    CHECK(vm && fm, "C85 shader"); if (!vm || !fm) { free(vm); free(fm); return; }
    // 原创仓内夹具的 sampler binding=1：只把 DescriptorSet decoration 改到组1。
    uint32_t sampler_id = 0; int changed = 0;
    for (size_t i = 5; i < fsz / 4; i += fm[i] >> 16) {
        if (!(fm[i] >> 16)) break;
        if ((fm[i] & 0xffff) == 71 && (fm[i] >> 16) == 4 && fm[i+2] == 33 && fm[i+3] == 1) sampler_id = fm[i+1];
    }
    for (size_t i = 5; i < fsz / 4; i += fm[i] >> 16) {
        if (!(fm[i] >> 16)) break;
        if ((fm[i] & 0xffff) == 71 && (fm[i] >> 16) == 4 && fm[i+1] == sampler_id && fm[i+2] == 34) { fm[i+3] = 1; changed++; }
    }
    CHECK(sampler_id && changed == 1, "C85 sampler装饰唯一");
    nvn_gpu_layout_ent ents[2] = {0};
    ents[0].set = 0; ents[0].binding = 0; ents[0].cls = NVN_GPU_RES_TEX;
    ents[1].set = 1; ents[1].binding = 1; ents[1].cls = NVN_GPU_RES_SAMP;
    for (int i=0;i<2;i++) { ents[i].count=1; ents[i].access=NVN_GPU_ACC_R; ents[i].stages=1u<<NVN_GPU_SH_FS; }
    nvn_gpu_layout fl = {0}, vl = {0}; fl.ent=ents; fl.nent=2;
    nvn_gpu_shader vs=NULL, fs=NULL, plain=NULL; nvn_gpu_err e;
    nvn_gpu_shader_src ss = {0}; ss.stage=NVN_GPU_SH_VS; ss.kind=NVN_GPU_BLOB_SPIRV; ss.entry="main"; ss.blob=vm; ss.bytes=vsz; ss.layout=&vl;
    CHECK(OK(nvn_gpu_shader_load(&ss,&vs,&e)), "C85 VS %s", e.why);
    ss.stage=NVN_GPU_SH_FS; ss.blob=fm; ss.bytes=fsz; ss.layout=&fl;
    CHECK(OK(nvn_gpu_shader_load(&ss,&fs,&e)), "C85 FS %s", e.why);
    size_t psz=0; snprintf(path,sizeof path,"%s/solid_green.frag.spv",dir); void *pm=read_file(path,&psz);
    CHECK(pm != NULL,"C85 no-set shader");
    ss.blob=pm; ss.bytes=psz; ss.layout=&vl;
    CHECK(OK(nvn_gpu_shader_load(&ss,&plain,&e)),"C85 plain FS %s",e.why); free(pm);
    nvn_gpu_pipe_desc pd={0}; pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.origin_ll=(uint8_t)!g_tri_origin_ul; pd.color.logic_op=3;
    pd.st[NVN_GPU_SH_VS].sh=vs; pd.st[NVN_GPU_SH_FS].sh=fs; pd.cfmt[0]=NVN_FORMAT_RGBA8; pd.samples=1; pd.prim=4; pd.chmask.mask=0xf;
    pd.attr[0].fmt=pd.attr[1].fmt=NVN_FORMAT_RG32F; pd.attr[1].offset=8; pd.stream[0].stride=16;
    nvn_gpu_pipe pipe=NULL, pipe2=NULL, no_sets=NULL; CHECK(OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e)),"C85 pipe %s",e.why);
    pd.prim=5; CHECK(OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe2,&e)),"C85 pipe2 %s",e.why);
    pd.st[NVN_GPU_SH_FS].sh=plain;
    CHECK(OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&no_sets,&e)),"C85 no-set pipe %s",e.why);
    const float verts[3][4]={{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb=make_buf(sizeof verts,NVN_GPU_MEM_SHARED,verts), rb=make_buf(12*4*4,NVN_GPU_MEM_SHARED,NULL);
    nvn_gpu_tex red=make_tex(1,1,NVN_FORMAT_RGBA8,NVN_GPU_TEX_SAMPLED), blue=make_tex(1,1,NVN_FORMAT_RGBA8,NVN_GPU_TEX_SAMPLED), out=make_tex(12,4,NVN_FORMAT_RGBA8,NVN_GPU_TEX_RENDER);
    nvn_gpu_view rv=make_view(red), bv=make_view(blue), ov=make_view(out);
    samp_par sp={0}; nvn_gpu_samp samp=NULL; CHECK(OK(nvn_gpu_samp_create(&sp,&samp,&e)),"C85 sampler %s",e.why);
    nvn_gpu_binding binds[2]={0}; binds[0].cls=NVN_GPU_RES_TEX; binds[0].u.view=rv; binds[1].ent=1; binds[1].cls=NVN_GPU_RES_SAMP; binds[1].u.samp=samp;
    for (int seg=0;seg<2;seg++) {
        nvn_gpu_cmd c=begin_cmd(); uint8_t r[4]={255,0,0,255}, b[4]={0,0,255,255};
        if (!seg) { nvn_gpu_tex_upload(c,red,0,0,NULL,r,4,4); nvn_gpu_tex_upload(c,blue,0,0,NULL,b,4,4); }
        nvn_gpu_pass_desc pass={0}; pass.ncolor=1; pass.w=12; pass.h=4; pass.layers=1; pass.color[0].view=ov; pass.color[0].load=NVN_GPU_LOAD_CLEAR; pass.color[0].store=NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(c,&pass); nvn_gpu_set_pipe(c,pipe); nvn_gpu_set_vbuf(c,0,vb,0);
        nvn_gpu_dyn dyn={0}; dyn.origin_ll=pd.origin_ll; dyn.nvp=1; dyn.vp[0][2]=12; dyn.vp[0][3]=4; dyn.vp[0][5]=1; dyn.has_sc=1; dyn.sc[2]=4; dyn.sc[3]=4;
        nvn_gpu_draw_args da={0}; da.prim=4; da.count=3; da.instances=1;
        for (int draw=0;draw<3;draw++) {
            dyn.sc[0]=draw*4; nvn_gpu_set_dyn(c,&dyn); binds[0].u.view=draw==2?bv:rv;
            // 无效union字节不同不应影响同一种绑定身份。
            binds[0]._rsv=(uint8_t)draw;
            nvn_gpu_bind(c,NVN_GPU_SH_FS,binds,2); unsigned before=g_desc_sets_total; nvn_gpu_draw(c,&da);
            CHECK(g_desc_sets_total-before==(draw==0?2u:draw==1?0u:1u),"C85 段%d笔%d分配%u",seg,draw,g_desc_sets_total-before);
        }
        // 内部清除后所有组失效；重新发同绑定也必须重建两组。
        nvn_gpu_clear cl={0}; cl.color=0; cl.color_mask=0xf; cl.stencil_mask=0xff; cl.x=8; cl.w=4; cl.h=4; cl.value.f[1]=1; cl.value.f[3]=1;
        nvn_gpu_clear_rect(c,&cl); nvn_gpu_set_pipe(c,pipe); nvn_gpu_set_dyn(c,&dyn); nvn_gpu_set_vbuf(c,0,vb,0); nvn_gpu_bind(c,NVN_GPU_SH_FS,binds,2);
        unsigned before=g_desc_sets_total; nvn_gpu_draw(c,&da); CHECK(g_memo_on || g_desc_sets_total-before==2,"C85 clear后重建两组");
        nvn_gpu_set_pipe(c,pipe2); nvn_gpu_bind(c,NVN_GPU_SH_FS,binds,2); da.prim=5; before=g_desc_sets_total; nvn_gpu_draw(c,&da);
        CHECK(g_desc_sets_total-before==2,"C85 layout换后重建两组");
        // 旧 stage 绑定仍保留：无组管线必须忽略它们，回来后重建全部组。
        nvn_gpu_set_pipe(c,no_sets); before=g_desc_sets_total; nvn_gpu_draw(c,&da);
        CHECK(g_desc_sets_total==before,"C85 无组管线不分配");
        nvn_gpu_set_pipe(c,pipe2); before=g_desc_sets_total; nvn_gpu_draw(c,&da);
        CHECK(g_memo_on || g_desc_sets_total-before==2,"C85 有组→无组→有组重建两组");
        nvn_gpu_pass_end(c); nvn_gpu_copy_tex_buf(c,out,0,0,NULL,rb,0,48,192);
        CHECK(OK(nvn_gpu_cmd_commit(c,130+seg,on_done,NULL,&e)),"C85 commit %s",e.why); CHECK(OK(nvn_gpu_fence_wait(130+seg,5000000000ull,&e)),"C85 wait %s",e.why);
        const uint8_t *px=buf_cpu(rb); int bad=0; for(int y=0;y<4;y++)for(int x=0;x<12;x++){const uint8_t *v=px+(y*12+x)*4;if(v[0]!=(x<8?255:0)||v[1]||v[2]!=(x<8?0:255)||v[3]!=255)bad++;}
        CHECK(!bad,"C85 段%d 48像素红/红/蓝，不符%d",seg,bad);
    }
    nvn_gpu_view_release(rv); nvn_gpu_view_release(bv); nvn_gpu_view_release(ov); nvn_gpu_tex_release(red); nvn_gpu_tex_release(blue); nvn_gpu_tex_release(out); nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(vb,NULL,NULL); nvn_gpu_buf_release(rb,NULL,NULL); nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(plain); free(vm);free(fm);
}

// C87：4×标准样本 x=1/8、3/8、5/8、7/8；半像素红矩形覆盖其中2个，另2个保留蓝。
// 解析后首列应为 (128,0,128,255)，其它列为蓝；不是只清同一种颜色。
static void c87_resolve(const char *dir) {
    CHECK(g_dev.properties.limits.standardSampleLocations, "C87 手算覆盖需要标准采样位置");
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
    td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1; td.d.samples = 4; td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex a = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_tex_create(&td, &a, &e)), "C87 target5 MSAA 创建：%s", e.why);
    if (!a) return;
    // target6 的 depth 是数组层数，不是 VkImage 的第三维。
    nvn_gpu_tex arr = NULL; td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY; td.d.depth = 3;
    CHECK(OK(nvn_gpu_tex_create(&td, &arr, &e)), "C87 target6 创建：%s", e.why);
    if (arr) { nvn_gpu_tex_info ti = {0}; nvn_gpu_tex_info_get(arr, &ti); CHECK(ti.layers == 3 && ti.samples == 4, "C87 target6 层数/样本 %u/%u", ti.layers, ti.samples); nvn_gpu_tex_release(arr); }
    td.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; td.d.depth = 1; td.d.samples = 3;
    nvn_gpu_tex bad = NULL;
    CHECK(nvn_gpu_tex_create(&td, &bad, &e).code == NVN_GPU_E_UNSUPPORTED && !bad, "C87 非法采样数应 UNSUPPORTED");
    td.d.samples = 64;
    VkImageFormatProperties ip = {0};
    VkResult ir = vkGetPhysicalDeviceImageFormatProperties(g_dev.physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ip);
    if (ir == VK_SUCCESS && !(ip.sampleCounts & VK_SAMPLE_COUNT_64_BIT))
        CHECK(nvn_gpu_tex_create(&td, &bad, &e).code == NVN_GPU_E_UNSUPPORTED && !bad, "C87 设备不支持64采样应 UNSUPPORTED");
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C87 shader");
    nvn_gpu_pipe_desc pd = {0}; pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.origin_ll = (uint8_t)!g_tri_origin_ul;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 4; pd.prim = 5; pd.chmask.mask = 0xf; pd.color.logic_op = 3;
    pd.attr[0].fmt = pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C87 MSAA管线：%s", e.why);
    float verts[4][8] = {{-1,-1,0,1,1,0,0,1},{-1,1,0,1,1,0,0,1},{1,-1,0,1,1,0,0,1},{1,1,0,1,1,0,0,1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex b = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view av = make_view(a), bv = make_view(b); nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_desc p = {0}; p.ncolor = 1; p.w = p.h = 4; p.layers = 1;
    p.color[0].view = av; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = p.color[0].clear.f[3] = 1;
    nvn_gpu_pass_begin(c, &p); nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn = {0}; dyn.origin_ll = pd.origin_ll; dyn.nvp = 1; dyn.vp[0][2] = 0.5f; dyn.vp[0][3] = 4; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da = {0}; da.prim = 5; da.count = 4; da.instances = 1; nvn_gpu_draw(c, &da); nvn_gpu_pass_end(c);
    NVNcopyRegion rr = {0, 0, 0, 4, 4, 1}; nvn_gpu_blit(c, av, &rr, bv, &rr, 0);
    nvn_gpu_copy_tex_buf(c, b, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 160, on_done, NULL, &e); CHECK(OK(rc), "C87 resolve commit：%d %s", rc.code, e.why);
    if (OK(rc)) {
        CHECK(OK(nvn_gpu_fence_wait(160, 5000000000ull, &e)), "C87 wait：%s", e.why);
        const uint8_t *px = buf_cpu(rb); int badpx = 0;
        for (int i = 0; i < 16; i++) { int red = i % 4 == 0 ? 128 : 0, blue = i % 4 == 0 ? 128 : 255;
            if (px[i*4] != red || px[i*4+1] || px[i*4+2] != blue || px[i*4+3] != 255) badpx++; }
        CHECK(!badpx, "C87 混合解析16像素不符 %d，首像素 %u,%u,%u,%u", badpx, px[0], px[1], px[2], px[3]);
    }
    nvn_gpu_view_release(av); nvn_gpu_view_release(bv); nvn_gpu_tex_release(a); nvn_gpu_tex_release(b);
    nvn_gpu_buf_release(rb, NULL, NULL); nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}

// 每个拒绝用例独占会话；必须在发 Vulkan 命令前拒绝。
static void c87_resolve_reject(int k) {
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = k == 2 ? NVN_FORMAT_R32UI : k == 3 ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
    td.d.samples = 4; td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex a = NULL, b = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_tex_create(&td, &a, &e)), "C87 拒绝源创建：%s", e.why);
    td.d.samples = k == 4 ? 4 : 1;
    if (k == 1) td.d.format = NVN_FORMAT_BGRA8;
    if (k == 7) td.d.width = 8;
    CHECK(OK(nvn_gpu_tex_create(&td, &b, &e)), "C87 拒绝目标创建：%s", e.why);
    nvn_gpu_view av = make_view(a), bv = make_view(b); nvn_gpu_cmd c = begin_cmd();
    NVNcopyRegion sr = {0, 0, 0, 4, 4, 1}, dr = sr; if (k == 0) dr.width = 2; if (k == 5) dr.width = 0;
    nvn_gpu_blit(c, av, &sr, bv, &dr, k == 6);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(rc.code == ((k == 0 || k == 5) ? NVN_GPU_E_ARG : NVN_GPU_E_UNSUPPORTED), "C87 拒绝 %d：%d %s", k, rc.code, e.why);
    nvn_gpu_view_release(av); nvn_gpu_view_release(bv); nvn_gpu_tex_release(a); nvn_gpu_tex_release(b);
}

// C74b：矩形超出通道范围 → 粘滞 E_ARG，整段不提交（原生的矩形清越界是未定义行为，不能放过去）。独占会话。
static void c74b_clear_rect_oob(void) {
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.color = 0; cl.x = 60; cl.y = 0; cl.w = 8; cl.h = 8;   // x 60–67 超出 64
    nvn_gpu_clear_rect(c, &cl);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 116, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C74b 越界矩形应 E_ARG，实为 %d %s", r.code, e.why);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
}

// C75：深度偏移。开关（NVN 偏移开关的填充位 = 4）进管线，factor / units / clamp 走动态状态。
//   深度整屏清成 0.5；两笔都画在 z = 0.5、深度 LESS，动态状态里都带 units = −4、factor = 1000：
//   ① 绿，左半屏，管线没开偏移 → 0.5 < 0.5 不成立，画不上；
//   ② 蓝，右半屏，管线开了偏移 → 平铺三角形斜率为 0，factor 那一项是 0，深度 = 0.5 − 4 个最小可分辨差 < 0.5，画上。
// 期望（手算）：左半黑、右半蓝。偏移没进管线则右半也黑；units 与 factor 接反则偏移是 +1000 个可分辨差，右半也黑；
// 没开偏移的管线若吃了动态量则左半出绿。
static void c75_depth_bias(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL, vs2 = NULL, fs2 = NULL; void *vm = NULL, *fm = NULL, *vm2 = NULL, *fm2 = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    if (!load_shader_pair(dir, "tri3.vert", "solid_blue.frag", NULL, NULL, &vs2, &fs2, &vm2, &fm2)) { CHECK(0, "读不到 solid_blue"); return; }
    nvn_gpu_pipe_desc pd, pd2;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8); pd.ds.dt = 1; pd.ds.dw = 0; pd.ds.df = 2;
    fill_gpipe_desc(&pd2, vs2, fs2, NVN_FORMAT_DEPTH24_STENCIL8); pd2.ds.dt = 1; pd2.ds.dw = 0; pd2.ds.df = 2; pd2.poly.offset_en = 4;
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL, pipe2 = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C75 pipe：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_pipe_get(&pd2, NVN_GPU_PIPE_SYNC, &pipe2, &e)), "C75 pipe2：%d %s", e.rc.code, e.why);
    CHECK(pipe != pipe2, "C75 偏移开关不同的两份描述拿到了同一条管线");
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = half_depth_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.stencil.view = dv; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 0.5f; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_dyn dyn; full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    dyn.po.units = -4; dyn.po.factor = 1000; dyn.po.clamp = 0;
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_set_pipe(c, pipe); dyn.sc[2] = 32; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    nvn_gpu_set_pipe(c, pipe2); dyn.sc[0] = 32; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 117, on_done, NULL, &e);
    CHECK(OK(r), "C75 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(117, 5000000000ull, &e);
    CHECK(OK(r), "C75 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 0, 0, "C75 左半（没开偏移，0.5 < 0.5 不过：黑）");
    expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C75 右半（偏移 −4 个可分辨差：蓝）");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv); nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs2); nvn_gpu_shader_release(fs2);
    free(vm); free(fm); free(vm2); free(fm2);
}

// C85（#708）：冗余状态消除——管线、动态状态、顶点 / 索引缓冲、push 只在和上次真正录进去的不同时才录。
//   计数（期望值手算，下标：管线、视口、裁剪、混合常量、模板、深度偏移、顶点缓冲、索引缓冲、push）：
//   ① 同一通道里两笔完全相同的带索引绘制 → 各录 1 次；② 第三笔只改裁剪 → 只有裁剪多 1 次；
//   ③ 第四笔只换顶点缓冲的偏移、索引偏移 → 只有顶点缓冲、索引缓冲各多 1 次；④ 矩形清除之后同样的一笔 → 全部再录一遍；
//   ⑤ 新通道里同样的一笔 → 全部再录一遍；⑥ push 常量：同样的字节再 push 不重发，内容变了重发。
//   读回（维护方在 #708 要求的）：通道清黑；第一笔绿、裁剪左半（x 0–31）；第二笔只改裁剪到 x 48–63。
//   期望：x 0–31 绿、32–47 黑、48–63 绿。第二笔的裁剪若被当成「没变」漏发，就会再画在左半，右边一条留黑。
static void c85_redundant_state(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd; fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e; nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C85 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    const float verts[4][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}, {-1, -1, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    const uint16_t idx[8] = {0, 1, 2, 0, 1, 2, 0, 0};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE; p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_dyn dyn; full_dyn(&dyn); dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1; da.ibuf = ib; da.itype = 1;
    uint32_t n[9];
#define C85_EXPECT(tag, a0, a1, a2, a3, a4, a5, a6, a7, a8) do { nvn_gpu_vk_test_emit_counts(c, n); const uint32_t x_[9] = {a0, a1, a2, a3, a4, a5, a6, a7, a8}; \
        CHECK(!memcmp(n, x_, sizeof x_), "C85 %s：录了 管线%u 视口%u 裁剪%u 混合%u 模板%u 偏移%u 顶点%u 索引%u push%u，期望 %u %u %u %u %u %u %u %u %u", tag, \
              n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], a0, a1, a2, a3, a4, a5, a6, a7, a8); } while (0)
    // 计数用的一段（不读回）
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    C85_EXPECT("① 两笔相同", 1, 1, 1, 1, 1, 1, 1, 1, 0);
    dyn.sc[0] = 16; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_draw(c, &da);
    C85_EXPECT("② 只改裁剪", 1, 1, 2, 1, 1, 1, 1, 1, 0);
    nvn_gpu_set_vbuf(c, 0, vb, 16); da.ioff = 6; nvn_gpu_draw(c, &da); da.ioff = 0;
    C85_EXPECT("③ 只换顶点 / 索引偏移", 1, 1, 2, 1, 1, 1, 2, 2, 0);
    nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color = 0; cl.color_mask = 0xf; cl.stencil_mask = 0xff; cl.x = 0; cl.y = 0; cl.w = 8; cl.h = 8;
    nvn_gpu_clear_rect(c, &cl); nvn_gpu_set_pipe(c, pipe); nvn_gpu_draw(c, &da);
    C85_EXPECT("④ 矩形清除之后", 2, 2, 3, 2, 2, 2, 3, 3, 0);
    nvn_gpu_pass_end(c);
    p.color[0].load = NVN_GPU_LOAD_LOAD; nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_draw(c, &da);
    C85_EXPECT("⑤ 新通道", 3, 3, 4, 3, 3, 3, 4, 4, 0);
    nvn_gpu_pass_end(c);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 140, on_done, NULL, &e);
    CHECK(OK(r), "C85 commit（计数段）：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(140, 5000000000ull, &e);
    CHECK(OK(r), "C85 wait（计数段）：%d %s", r.code, e.why);
    // 读回用的一段：两笔只差裁剪
    p.color[0].load = NVN_GPU_LOAD_CLEAR; full_dyn(&dyn); dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF; dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    c = begin_cmd();
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe); dyn.sc[2] = 32; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0); nvn_gpu_draw(c, &da);
    dyn.sc[0] = 48; dyn.sc[2] = 16; nvn_gpu_set_dyn(c, &dyn); nvn_gpu_draw(c, &da);
    C85_EXPECT("读回段", 1, 1, 2, 1, 1, 1, 1, 1, 0);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    r = nvn_gpu_cmd_commit(c, 141, on_done, NULL, &e);
    CHECK(OK(r), "C85 commit（读回段）：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(141, 5000000000ull, &e);
    CHECK(OK(r), "C85 wait（读回段）：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C85 第一笔（裁剪 x 0–31：绿）");
    expect_rect(buf_cpu(rb), 32, 0, 16, 64, 0, 0, 0, "C85 两笔都没画到的 x 32–47（黑）");
    expect_rect(buf_cpu(rb), 48, 0, 16, 64, 0, 255, 0, "C85 第二笔只改了裁剪（x 48–63：绿）");
    // ⑥ push 常量（FS push 的管线，同 C56）：同样的字节再 push 一遍 → 不重发；内容变了 → 重发
    { nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.push_off = 0; fl.push_bytes = 16;
      nvn_gpu_shader pvs = NULL, pfs = NULL; void *pvm = NULL, *pfm = NULL;
      if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &pvs, &pfs, &pvm, &pfm)) { CHECK(0, "读不到 pushc 着色器"); }
      else {
        nvn_gpu_pipe_desc ppd; fill_gpipe_desc(&ppd, pvs, pfs, 0); nvn_gpu_pipe ppipe = NULL;
        CHECK(OK(nvn_gpu_pipe_get(&ppd, NVN_GPU_PIPE_SYNC, &ppipe, &e)), "C85 push pipe：%d %s", e.rc.code, e.why);
        const float ca[4] = {0.25f, 0.5f, 0.75f, 1.0f}, cb[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        nvn_gpu_draw_args pa = da; pa.ibuf = NULL; pa.itype = 0;
        c = begin_cmd(); nvn_gpu_pass_begin(c, &p);
        nvn_gpu_set_pipe(c, ppipe); nvn_gpu_set_dyn(c, &dyn); nvn_gpu_set_vbuf(c, 0, vb, 0);
        nvn_gpu_push(c, NVN_GPU_SH_FS, 0, ca, sizeof ca); nvn_gpu_draw(c, &pa);
        nvn_gpu_push(c, NVN_GPU_SH_FS, 0, ca, sizeof ca); nvn_gpu_draw(c, &pa);
        C85_EXPECT("⑥ 同样的 push", 1, 1, 1, 1, 1, 1, 1, 0, 1);
        nvn_gpu_push(c, NVN_GPU_SH_FS, 0, cb, sizeof cb); nvn_gpu_draw(c, &pa);
        C85_EXPECT("⑥ push 内容变了", 1, 1, 1, 1, 1, 1, 1, 0, 2);
        nvn_gpu_pass_end(c);
        r = nvn_gpu_cmd_commit(c, 142, on_done, NULL, &e);
        CHECK(OK(r), "C85 commit（push 段）：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(142, 5000000000ull, &e);
        CHECK(OK(r), "C85 wait（push 段）：%d %s", r.code, e.why);
        nvn_gpu_shader_release(pvs); nvn_gpu_shader_release(pfs); free(pvm); free(pfm);
      } }
#undef C85_EXPECT
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

// C86：同一三角形的填充 / 线 / 点；期望来自顶点窗口坐标，不取后端输出当金样。
static void c86_polygon_modes(const char *dir) {
    nvn_gpu_shader vs=NULL,fs=NULL; void *vm=NULL,*fm=NULL;
    if (!load_shader_pair(dir,"point.vert","solid_green.frag",NULL,NULL,&vs,&fs,&vm,&fm)) { CHECK(0,"C86 shaders"); return; }
    nvn_gpu_pipe_desc pd; fill_gpipe_desc(&pd,vs,fs,NVN_FORMAT_DEPTH24_STENCIL8);
    pd.ds.df=2; pd.attr[1].fmt=NVN_FORMAT_RGBA32F; pd.attr[1].offset=16; pd.stream[0].stride=32;
    const float verts[3][8]={{-.625f,-.625f,.5f,1,0,1,0,1},{.625f,-.625f,.5f,1,0,1,0,1},{-.625f,.625f,.5f,1,0,1,0,1}};
    nvn_gpu_buf vb=make_buf(sizeof verts,NVN_GPU_MEM_SHARED,verts),rb=make_buf(8*8*4,NVN_GPU_MEM_SHARED,NULL);
    nvn_gpu_tex out=make_tex(8,8,NVN_FORMAT_RGBA8,NVN_GPU_TEX_RENDER),ds=make_tex(8,8,NVN_FORMAT_DEPTH24_STENCIL8,NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov=make_view(out),dv=make_view(ds);
    nvn_gpu_pipe modes[3]={0}; nvn_gpu_err e;
    // 这是拒绝路径注入：只覆盖后端能力检查，不冒充真实无能力GPU。
    VkBool32 supported=g_dev.core_features.fillModeNonSolid;
    g_dev.core_features.fillModeNonSolid=VK_FALSE;
    for(int mode=0;mode<2;mode++) { pd.poly.mode=(uint8_t)mode; nvn_gpu_pipe denied=NULL; nvn_gpu_rc r=nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&denied,&e); CHECK(r.code==NVN_GPU_E_UNSUPPORTED&&!denied,"C86 无能力mode%d应拒绝，rc%d",mode,r.code); }
    g_dev.core_features.fillModeNonSolid=supported;
    CHECK(supported,"C86 本机需支持非填充正控");
    if(supported) for(int mode=0;mode<3;mode++) { pd.poly.mode=(uint8_t)mode; nvn_gpu_rc r=nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&modes[mode],&e); CHECK(OK(r),"C86 mode%d pipe %s",mode,e.why); }
    CHECK(modes[0]!=modes[1]&&modes[1]!=modes[2]&&modes[0]!=modes[2],"C86 模式必须进管线键");
    // 4次形状（fill,line,point,fill回切）+ 3模式x3偏移位（关/别的模式/自身）。
    for(int test=0;supported&&test<13;test++) {
        int mode=test<4?(test==0||test==3?2:test==1?1:0):(test-4)/3;
        int bias=test<4?-1:(test-4)%3;
        pd.poly.mode=(uint8_t)mode; pd.ds.dt=bias>=0; pd.ds.dw=0; pd.ds.df=2;
        pd.poly.offset_en=bias<1?0:bias==1?(uint8_t)(1u<<((mode+1)%3)):(uint8_t)(1u<<mode);
        nvn_gpu_pipe pipe=NULL; nvn_gpu_rc r=nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e); CHECK(OK(r),"C86 test%d pipe %s",test,e.why);
        if(test<4) CHECK(pipe==modes[mode],"C86 mode%d cache身份",mode);
        nvn_gpu_cmd c=begin_cmd(); nvn_gpu_pass_desc p={0}; p.ncolor=1;p.w=p.h=8;p.layers=1;
        p.color[0].view=ov;p.color[0].load=NVN_GPU_LOAD_CLEAR;p.color[0].store=NVN_GPU_STORE_STORE;p.color[0].clear.f[3]=1;
        p.depth.view=p.stencil.view=dv;p.depth.load=p.stencil.load=NVN_GPU_LOAD_CLEAR;p.depth.store=p.stencil.store=NVN_GPU_STORE_STORE;p.clear_depth=.5f;
        nvn_gpu_pass_begin(c,&p); nvn_gpu_set_pipe(c,pipe); nvn_gpu_set_vbuf(c,0,vb,0);
        nvn_gpu_dyn dyn;full_dyn(&dyn);dyn.vp[0][2]=dyn.vp[0][3]=8;dyn.sc[2]=dyn.sc[3]=8;dyn.po.units=-4;dyn.po.factor=0;
        nvn_gpu_set_dyn(c,&dyn);nvn_gpu_draw_args da={0};da.prim=4;da.count=3;da.instances=1;nvn_gpu_draw(c,&da);nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c,out,0,0,NULL,rb,0,32,256);r=nvn_gpu_cmd_commit(c,140+test,on_done,NULL,&e);CHECK(OK(r),"C86 test%d commit %s",test,e.why);r=nvn_gpu_fence_wait(140+test,5000000000ull,&e);CHECK(OK(r),"C87 wait %s",e.why);
        const uint8_t *px=buf_cpu(rb); int x=mode==2?2:mode==1?3:1,y=mode==2?2:1; int lit=bias<0||bias==2;
        const uint8_t *v=px+(y*8+x)*4;CHECK(v[0]==0&&v[1]==(lit?255:0)&&v[2]==0&&v[3]==255,"C86 test%d mode%d bias%d anchor(%d,%d)=(%u,%u,%u,%u)",test,mode,bias,x,y,v[0],v[1],v[2],v[3]);
        CHECK(px[(7*8+7)*4+1]==0,"C86 外部应黑");
        if(mode!=2) CHECK(px[(2*8+2)*4+1]==0,"C86 非填充内部应黑");
        if(mode==0) CHECK(px[(1*8+3)*4+1]==0,"C86 点模式边中应黑");
    }
    nvn_gpu_view_release(ov);nvn_gpu_view_release(dv);nvn_gpu_tex_release(out);nvn_gpu_tex_release(ds);nvn_gpu_buf_release(vb,NULL,NULL);nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);free(vm);free(fm);
}

// C86b：3D 贴图的深度切片当渲染目标。核心为 3D 附件请求 2D 阵列目标（graphics/nvn/replay/rp_pass.c att_view）；后端的 3D 图像带
//   2D_ARRAY_COMPATIBLE，通用视图按「2D 阵列、base_layer = 切片号」建，开通道时另建附件视图按 att_layer 取那一片。
// 期望（手算）：16x16x4 的 RGBA8，第 k 片初值全是 (0x40+k, 0x80, 0xC0-k, 0xFF)；
//   ① 只把第 2 片清成绿 (0,255,0,255) → 读回：片 2 = 绿，片 0、1、3 还是各自的初值；
//   ② 再把第 0 片清成蓝 (0,0,255,255) → 读回：片 0 = 蓝，片 2 仍是绿，片 1、3 不动。
//   反证：同一片不请求 2D 阵列目标（视图没设 set_target）→ view_create 被拒，原因里有「depth slice 2 of 4」。
enum { C86b_W = 16, C86b_H = 16, C86b_D = 4, C86b_SLICE = C86b_W * C86b_H * 4 };
static void c86_expect(const uint8_t *px, const uint8_t want[4][4], const char *tag) {
    for (int k = 0; k < C86b_D; k++) {
        int nbad = 0; const uint8_t *o = px + k * C86b_SLICE;
        for (int i = 0; i < C86b_W * C86b_H; i++) if (memcmp(o + i * 4, want[k], 4)) nbad++;
        CHECK(!nbad, "C86b %s 第 %d 片：%d 个像素不符（首像素 %02x%02x%02x%02x，期望 %02x%02x%02x%02x）", tag, k, nbad, o[0], o[1], o[2], o[3], want[k][0], want[k][1], want[k][2], want[k][3]);
    }
}
static void c86b_3d_slice_attach(void) {
    nvn_gpu_tex t = make_tex_target(C86b_W, C86b_H, C86b_D, NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER);
    static uint8_t init[C86b_D * C86b_SLICE];
    for (int k = 0; k < C86b_D; k++) for (int i = 0; i < C86b_W * C86b_H; i++) { uint8_t *p = init + k * C86b_SLICE + i * 4; p[0] = (uint8_t)(0x40 + k); p[1] = 0x80; p[2] = (uint8_t)(0xC0 - k); p[3] = 0xFF; }
    nvn_gpu_buf ib = make_buf(sizeof init, NVN_GPU_MEM_SHARED, init), rb = make_buf(sizeof init, NVN_GPU_MEM_SHARED, NULL);
    view_t vs; memset(&vs, 0, sizeof vs); vs.set_layers = 1; vs.min_layer = 2; vs.nlayers = 1;
    nvn_gpu_view bad = NULL, v2 = NULL, v0 = NULL; nvn_gpu_err e; memset(&e, 0, sizeof e);
    nvn_gpu_rc r = nvn_gpu_view_create(t, &vs, &bad, &e);
    CHECK(!OK(r) && !bad && strstr(e.why, "depth slice 2 of 4"), "C86b 不请求 2D 阵列目标的第 2 片视图应被拒：%d %s", r.code, e.why);
    vs.set_target = 1; vs.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    r = nvn_gpu_view_create(t, &vs, &v2, &e);
    CHECK(OK(r) && v2, "C86b 第 2 片视图（2D 阵列目标）：%d %s", r.code, e.why);
    vs.min_layer = 0;
    r = nvn_gpu_view_create(t, &vs, &v0, &e);
    CHECK(OK(r) && v0, "C86b 第 0 片视图（2D 阵列目标）：%d %s", r.code, e.why);
    if (v2 && v0) {
        const float green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
        const uint8_t gr[4] = {0, 255, 0, 255}, bl[4] = {0, 0, 255, 255};
        uint8_t want[4][4] = {{0x40, 0x80, 0xC0, 0xFF}, {0x41, 0x80, 0xBF, 0xFF}, {0x42, 0x80, 0xBE, 0xFF}, {0x43, 0x80, 0xBD, 0xFF}};
        for (int round = 0; round < 2; round++) {
            nvn_gpu_cmd c = begin_cmd();
            if (round == 0) for (uint32_t k = 0; k < C86b_D; k++) nvn_gpu_copy_buf_tex(c, ib, k * C86b_SLICE, C86b_W * 4, C86b_SLICE, t, 0, k, NULL);
            nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
            p.ncolor = 1; p.color[0].view = round == 0 ? v2 : v0; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            memcpy(p.color[0].clear.f, round == 0 ? green : blue, sizeof green);
            p.w = C86b_W; p.h = C86b_H; p.layers = 1;
            nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c);
            for (uint32_t k = 0; k < C86b_D; k++) nvn_gpu_copy_tex_buf(c, t, 0, k, NULL, rb, k * C86b_SLICE, C86b_W * 4, C86b_SLICE);
            r = nvn_gpu_cmd_commit(c, 170 + round, on_done, NULL, &e);
            CHECK(OK(r), "C86b commit %d：%d %s", round, r.code, e.why);
            r = nvn_gpu_fence_wait(170 + round, 5000000000ull, &e);
            CHECK(OK(r), "C86b wait %d：%d %s", round, r.code, e.why);
            memcpy(want[round == 0 ? 2 : 0], round == 0 ? gr : bl, 4);
            c86_expect(buf_cpu(rb), want, round == 0 ? "只清第 2 片后" : "再清第 0 片后");
        }
    }
    if (v2) nvn_gpu_view_release(v2);
    if (v0) nvn_gpu_view_release(v0);
    nvn_gpu_tex_release(t); nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C86c（#773）：三维块压缩贴图的深度切片经缓冲与非压缩贴图之间逐字节拷。核心把三维块压缩目标的一个深度切片当影子拷就是这么拷的
//   （graphics/nvn/replay/rp_pass.c shadow_copy：通道前基底的切片 → 缓冲 → 二维影子，通道后影子 → 缓冲 → 基底的同一个切片；
//   层号在三维贴图上是深度切片，后端转成 zoffset、depth 1）。影子是 RG32UI，一个像素 8 字节 = BC1 一块。
// 期望（手算）：
//   ① BC1 8x4x2（两块宽、一块高、两个切片）：切片 0 = A0 A1、切片 1 = B0 B1（四块互不相同，A = 0x10..0x1f、B = 0x20..0x2f、C = 0x30..0x3f）。
//      切片 1 经 缓冲 → 2x1 的影子 → 缓冲 读回 = B0 B1（取错切片会读到 A）；
//   ② 往影子写 C0 C1，经缓冲拷回切片 1，再读两个切片：切片 1 = C0 C1，切片 0 仍是 A0 A1（写错切片会盖掉 A）；
//   ③ 三级的 BC1 8x8x2：第 2 级是 2x2 个纹素（一块，不是块高 4 的整数倍）。写 D（0x40..0x47）、经 1x1 的影子拷到切片 0 再读回 = D
//      （核心按「基底那一级的像素尺寸」给拷贝区域，不按块取整，深一级的区域就是这样的）。
//   不能直接采样 BC 的设备（后端走计算解码）建不了原生压缩的三维贴图，跳过。
static nvn_gpu_tex make_tex_3d_levels(uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td; memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = (int)fmt; td.d.width = w; td.d.height = h;
    td.d.depth = (int)depth; td.d.levels = (int)levels; td.d.samples = 0; td.usage = usage;
    nvn_gpu_tex t = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r) && t, "tex_create 三维 %ux%ux%u 级 %u 失败：%d %s", w, h, depth, levels, r.code, e.why);
    return t;
}
static void c86c_bc_3d_slice_copy(void) {
    if ((g_dev.bc_decode_mask & (1u << (NVN_FORMAT_RGBA_DXT1 - 0x41))) || !(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_SAMPLE)) { fprintf(stderr, "C86c：BC1 非原生压缩存储，跳过三维切片原块拷贝\n"); return; }
    nvn_gpu_tex bc = make_tex_3d_levels(8, 4, 2, 1, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex bm = make_tex_3d_levels(8, 8, 2, 3, NVN_FORMAT_RGBA_DXT1, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex px = make_tex(2, 1, NVN_FORMAT_RG32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED), p1 = make_tex(1, 1, NVN_FORMAT_RG32UI, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    uint8_t init[64]; for (int i = 0; i < 64; i++) init[i] = (uint8_t)(0x10 + i);   // A0 A1 = 0x10..0x1f、B = 0x20..0x2f、C = 0x30..0x3f、D（第 2 级的一块）= 0x40..0x47
    nvn_gpu_buf ib = make_buf(sizeof init, NVN_GPU_MEM_SHARED, init), rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    if (!bc || !bm || !px || !p1 || !ib || !rb) return;
    nvn_gpu_err e; nvn_gpu_cmd c = begin_cmd();
    NVNcopyRegion rbc = {0, 0, 0, 8, 4, 1}, rpx = {0, 0, 0, 2, 1, 1};   // 基底按像素（整级 8x4），影子按块（2x1）
    NVNcopyRegion rm = {0, 0, 0, 2, 2, 1}, r1 = {0, 0, 0, 1, 1, 1};     // 第 2 级：2x2 个纹素，影子 1x1
    nvn_gpu_slice s1, s2, s3, s4;
    nvn_gpu_rc r = nvn_gpu_arena_alloc(c, 16, 16, &s1, &e); CHECK(OK(r), "C86c 暂存 1：%d %s", r.code, e.why);
    r = nvn_gpu_arena_alloc(c, 16, 16, &s2, &e); CHECK(OK(r), "C86c 暂存 2：%d %s", r.code, e.why);
    r = nvn_gpu_arena_alloc(c, 8, 16, &s3, &e); CHECK(OK(r), "C86c 暂存 3：%d %s", r.code, e.why);
    r = nvn_gpu_arena_alloc(c, 8, 16, &s4, &e); CHECK(OK(r), "C86c 暂存 4：%d %s", r.code, e.why);
    nvn_gpu_copy_buf_tex(c, ib, 0, 16, 16, bc, 0, 0, &rbc);                 // 切片 0 ← A0 A1
    nvn_gpu_copy_buf_tex(c, ib, 16, 16, 16, bc, 0, 1, &rbc);                // 切片 1 ← B0 B1
    nvn_gpu_copy_tex_buf(c, bc, 0, 1, &rbc, s1.buf, s1.off, 16, 16);        // ① 通道前：基底的切片 1 → 缓冲
    nvn_gpu_copy_buf_tex(c, s1.buf, s1.off, 16, 16, px, 0, 0, &rpx);        //            缓冲 → 影子
    nvn_gpu_copy_tex_buf(c, px, 0, 0, &rpx, rb, 0, 16, 16);                 //            影子读回到 rb[0..16)
    nvn_gpu_copy_buf_tex(c, ib, 32, 16, 16, px, 0, 0, &rpx);                // ② 「通道」往影子写 C0 C1
    nvn_gpu_copy_tex_buf(c, px, 0, 0, &rpx, s2.buf, s2.off, 16, 16);        //    通道后：影子 → 缓冲
    nvn_gpu_copy_buf_tex(c, s2.buf, s2.off, 16, 16, bc, 0, 1, &rbc);        //            缓冲 → 基底的同一个切片 1
    nvn_gpu_copy_tex_buf(c, bc, 0, 0, &rbc, rb, 16, 16, 16);                //            读回切片 0 → rb[16..32)
    nvn_gpu_copy_tex_buf(c, bc, 0, 1, &rbc, rb, 32, 16, 16);                //            读回切片 1 → rb[32..48)
    nvn_gpu_copy_buf_tex(c, ib, 48, 8, 8, bm, 2, 0, &rm);                   // ③ 第 2 级切片 0 ← D（2x2 个纹素、一块）
    nvn_gpu_copy_tex_buf(c, bm, 2, 0, &rm, s3.buf, s3.off, 8, 8);           //    基底第 2 级 → 缓冲
    nvn_gpu_copy_buf_tex(c, s3.buf, s3.off, 8, 8, p1, 0, 0, &r1);           //    缓冲 → 1x1 的影子
    nvn_gpu_copy_tex_buf(c, p1, 0, 0, &r1, s4.buf, s4.off, 8, 8);           //    影子 → 缓冲
    nvn_gpu_copy_buf_tex(c, s4.buf, s4.off, 8, 8, bm, 2, 0, &rm);           //    缓冲 → 基底第 2 级（同一个切片）
    nvn_gpu_copy_tex_buf(c, bm, 2, 0, &rm, rb, 48, 8, 8);                   //    读回 → rb[48..56)
    r = nvn_gpu_cmd_commit(c, 191, on_done, NULL, &e);   // 围栏号同一会话里严格递增：取合入时的下一个空号（比前面所有用例的都大）
    CHECK(OK(r), "C86c commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(191, 5000000000ull, &e);
    CHECK(OK(r), "C86c wait：%d %s", r.code, e.why);
    const uint8_t *o = buf_cpu(rb);
    CHECK(!memcmp(o, init + 16, 16), "C86c ① 三维切片 1 经影子读回 %02x%02x%02x%02x…，期望 B0 B1 = 20212223…（读到 10111213 = 取成了切片 0）", o[0], o[1], o[2], o[3]);
    CHECK(!memcmp(o + 16, init, 16), "C86c ② 切片 0 %02x%02x%02x%02x…，期望仍是 A0 A1 = 10111213…（被盖掉 = 写回写错了切片）", o[16], o[17], o[18], o[19]);
    CHECK(!memcmp(o + 32, init + 32, 16), "C86c ② 切片 1 %02x%02x%02x%02x…，期望 C0 C1 = 30313233…（还是 B = 没写回）", o[32], o[33], o[34], o[35]);
    CHECK(!memcmp(o + 48, init + 48, 8), "C86c ③ 第 2 级 %02x%02x%02x%02x…，期望 D = 40414243…", o[48], o[49], o[50], o[51]);
    nvn_gpu_tex_release(bc); nvn_gpu_tex_release(bm); nvn_gpu_tex_release(px); nvn_gpu_tex_release(p1);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
}

// 用同一组真实着色器先建、再命中、再只改剔除模式；探针应指出 poly，而不是猜测其他字段。
static void pipeprobe_nearest_test(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL; void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) {
        CHECK(0, "pipeprobe 加载测试着色器"); free(vm); free(fm); return;
    }
    nvn_gpu_pipe_desc pd; nvn_gpu_pipe a = NULL, b = NULL, other = NULL; nvn_gpu_err e = {0};
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &a, &e);
    CHECK(OK(r) && a, "pipeprobe 首次创建：%d %s", r.code, e.why);
    r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &b, &e);
    CHECK(OK(r) && a == b, "pipeprobe 同一描述应命中：%d %s", r.code, e.why);
    sleep_ms(5100);   // 跨过 5 秒采样间隔，再只变一个已知键字段
    pd.poly.cull = 1;
    r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &other, &e);
    CHECK(OK(r) && other && other != a, "pipeprobe 改剔除模式应 miss：%d %s", r.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vm); free(fm);
}

int main(int argc, char **argv) {
    t29_format_expand();
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why) != 0) {
        printf("SKIP? 建 Vulkan 设备失败：%s\n", why);
        return 2;   // 不静默跳过：run.sh 把 2 当红
    }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    bc_timeout_lifetime();
    // 机型模拟档（android/tests/vk/sim_tiers.sh）按这行核对模拟层确实藏掉了对应能力。
    printf("sim caps: vertexPipelineStoresAndAtomics=%u dualSrcBlend=%u calibrated_timestamps=%d（0 = 无校准扩展，走完成点兜底）\n",
           (unsigned)g_dev.core_features.vertexPipelineStoresAndAtomics, (unsigned)g_dev.core_features.dualSrcBlend,
           g_dev.calibrated_timestamps);
    if (argc < 2) {
        printf("用法：%s <shader-dir>（run.sh 会编译 tri.vert/frag 到该目录）\n", argv[0]);
        return 2;
    }
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "nvn-backend-vk-test";
    if (!open_backend()) return 1;
    if (getenv("S2A_TEST_WSI697_ONLY")) {
        c697_swapchain_images();
        c71_present();
        nvn_gpu_err close_err = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &close_err)), "C697 close: %s", close_err.why);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    c697_swapchain_images();
    c690_native_bc_image_copy(NVN_FORMAT_RGB_DXT1_SRGB, 8, 1);
    c690_native_bc_image_copy(NVN_FORMAT_RGBA_DXT5, 16, 2);
    c690_edge_copy();
    { nvn_gpu_err e690; CHECK(OK(nvn_gpu_close(5000000000ull, &e690)), "C690 close：%d %s", e690.rc.code, e690.why); }
    for (int k = 0; k < 8; k++) {
        if (!open_backend()) return 1;
        c690_bad_region(k);
        nvn_gpu_err e690; CHECK(OK(nvn_gpu_close(5000000000ull, &e690)), "C690 负例 %d close：%d %s", k, e690.rc.code, e690.why);
    }
    if (!open_backend()) return 1;

    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    CHECK(caps.api == NVN_GPU_API_VULKAN, "caps.api 应为 VULKAN");
    CHECK(caps.max_sets > 0 && caps.max_buf_bytes > 0, "caps 限值没有填");
    CHECK(caps.caps_id[0] | caps.caps_id[1] | caps.caps_id[15], "caps_id 全 0");
    CHECK(nvn_gpu_format_caps(0x1) != 0, "format_caps 对 R8 无能力位");   // NVN_FORMAT_R8
    t76_buf_create_alloc_cap();

    t01_fill32();
    t02_write_copy();
    t03_keepalive_unsubmitted();
    t03b_buf_use();
    t04_abort();
    t05_failure_terminal();

    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "第一次 close 失败：%d %s", e.rc.code, e.why);
    if (!open_backend()) return 1;
    t06_publish_order();
    t07_wait_args();
    t08_sticky();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "第二次 close 失败：%d %s", e.rc.code, e.why);
    if (!open_backend()) return 1;
    t09_release_only();
    t11_clear_color();
    t12_upload_copy_tex();
    t13_clear_depth();
    t14_textured_triangle(argv[1]);
    t10_close_inflight();
    if (!open_backend()) return 1;
    t16_fence_failure();
    nvn_gpu_err e2;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e2)), "t16 close：%d %s", e2.rc.code, e2.why);
    if (!open_backend()) return 1;
    t17_close_timeout();
    if (!open_backend()) return 1;
    t18_combined_sampler(argv[1]);
    nvn_gpu_err e3;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e3)), "t18 close：%d %s", e3.rc.code, e3.why);
    if (!open_backend()) return 1;
    t19_signal_contract();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e3)), "t19 close：%d %s", e3.rc.code, e3.why);
    if (!open_backend()) return 1;
    t20_combined_missing_samp(argv[1]);
    nvn_gpu_err e4;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e4)), "t20 close：%d %s", e4.rc.code, e4.why);
    if (!open_backend()) return 1;
    t21_clear_int();
    nvn_gpu_err e5;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e5)), "t21 close：%d %s", e5.rc.code, e5.why);
    if (!open_backend()) return 1;
    t22_clear_view_fmt();
    nvn_gpu_err e6;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e6)), "t22 close：%d %s", e6.rc.code, e6.why);
    if (!open_backend()) return 1;
    t30_rgb8_sampled(argv[1]);
    t28_rgb8_upload();
    nvn_gpu_err e7;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e7)), "t30/t28 close：%d %s", e7.rc.code, e7.why);
    if (!open_backend()) return 1;
    c71_present();
    { nvn_gpu_err e71; CHECK(OK(nvn_gpu_close(5000000000ull, &e71)), "C71 close（面还接着）：%d %s", e71.rc.code, e71.why); }
    if (!open_backend()) return 1;
    c73_origin_upper_left(argv[1]);
    { nvn_gpu_err e73; CHECK(OK(nvn_gpu_close(5000000000ull, &e73)), "C73 close：%d %s", e73.rc.code, e73.why); }
    if (!open_backend()) return 1;
    c72_bc1_upload(argv[1], (g_dev.bc_decode_mask & (1u << (NVN_FORMAT_RGBA_DXT1 - 0x41))) != 0);
    { nvn_gpu_err e72; CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C72 close：%d %s", e72.rc.code, e72.why); }
    { uint32_t saved_mask = g_dev.bc_decode_mask;
      g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_RGBA_DXT1 - 0x41);
      if (!open_backend()) return 1;
      c72_bc1_upload(argv[1], 1);
      c192_bc_order();
      c194_bc_cube_mips();
      nvn_gpu_err e72; CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C72 compute close：%d %s", e72.rc.code, e72.why);
      if (!open_backend()) return 1;
      c193_bc_readback_rejected();
      CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C87 close：%d %s", e72.rc.code, e72.why);
      if (!open_backend()) return 1;
      c196_bc_close_timeout();
      {   // #682 的六个格式：每块按端点直接构造纯红，采样后都是 (255,0,0,255)。
          static const uint32_t fmt[6] = {0x41,0x44,0x45,0x48,0x49,0x4b};
          static const uint8_t bc1[8] = {0x00,0xf8,0x00,0xf8,0,0,0,0};
          static const uint8_t bc3[16] = {255,255,0,0,0,0,0,0, 0x00,0xf8,0x00,0xf8,0,0,0,0};
          static const uint8_t bc4[8] = {255,255,0,0,0,0,0,0};
          static const uint8_t bc5[16] = {255,255,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};
          static const uint8_t red[4] = {255,0,0,255};
          for (int i = 0; i < 6; i++) g_dev.bc_decode_mask |= 1u << (fmt[i] - 0x41);
          if (!open_backend()) return 1;
          g_tri_expect = red;
          for (int i = 0; i < 6; i++) {
              const uint8_t *block = i == 1 || i == 3 ? bc3 : i == 4 ? bc4 : i == 5 ? bc5 : bc1;
              int row = i == 1 || i == 3 || i == 5 ? 16 : 8;
              g_tri_signal = (uint64_t)i + 1;
              printf("C195 执行：BC 格式 %#x\n", fmt[i]);
              tri_impl(argv[1], fmt[i], block, 4, row, 255);
          }
          g_tri_expect = NULL; g_tri_signal = 5;
          CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "C195 close：%d %s", e72.rc.code, e72.why);
      }
      g_dev.bc_decode_mask |= 1u << (NVN_FORMAT_RGBA_DXT1_SRGB - 0x41);
      if (!open_backend()) return 1;
      {   // 565 红端点 16 → 字节 132，sRGB 采样变线性 ≈ 0.2307 → 59/255。
          static const uint8_t block[8] = {0x00,0x80,0x00,0x80,0,0,0,0};
          static const uint8_t linear[4] = {59,0,0,255};
          g_tri_expect = linear; g_tri_tol = 1;
          tri_impl(argv[1], NVN_FORMAT_RGBA_DXT1_SRGB, block, 4, 8, 255);
          g_tri_expect = NULL; g_tri_tol = 0;
      }
      CHECK(OK(nvn_gpu_close(5000000000ull, &e72)), "BC SRGB close：%d %s", e72.rc.code, e72.why);
      g_dev.bc_decode_mask = saved_mask; }
    for (int k = 0; k < 2; k++) {   // tri_impl 的提交序号固定：每个变体一个会话
        if (!open_backend()) return 1;
        c70_swizzle(argv[1], k);
        nvn_gpu_err e70;
        CHECK(OK(nvn_gpu_close(5000000000ull, &e70)), "C70 close：%d %s", e70.rc.code, e70.why);
    }
    if (!open_backend()) return 1;
    t31_vertex_formats(argv[1]);
    nvn_gpu_err e8;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e8)), "t31 close：%d %s", e8.rc.code, e8.why);
    if (!open_backend()) return 1;
    t32_expand_bounds();
    nvn_gpu_err e9;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e9)), "t32 close：%d %s", e9.rc.code, e9.why);
    if (!open_backend()) return 1;
    t33_two_passes(argv[1]);
    t34_two_pass_load(argv[1]);
    t35_three_passes(argv[1]);
    nvn_gpu_err e10;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e10)), "C33–C35 close：%d %s", e10.rc.code, e10.why);
    if (!open_backend()) return 1;
    c36_40(argv[1]);
    nvn_gpu_err e11;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e11)), "C36–C40 close：%d %s", e11.rc.code, e11.why);
    if (!open_backend()) return 1;
    c41_bounds_neg(argv[1]);
    nvn_gpu_err e12;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e12)), "C41 close：%d %s", e12.rc.code, e12.why);
    if (!open_backend()) return 1;
    c42_u8_indirect_neg(argv[1]);
    nvn_gpu_err e13;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e13)), "C42 close：%d %s", e13.rc.code, e13.why);
    if (!open_backend()) return 1;
    c43_blend(argv[1]);
    nvn_gpu_err e14;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e14)), "C43 close：%d %s", e14.rc.code, e14.why);
    if (!open_backend()) return 1;
    c44_prims(argv[1]);
    c46_blend_const(argv[1]);
    c47_logic(argv[1]);
    c48_fan(argv[1]);
    c49_restart(argv[1]);
    c50_list_restart(argv[1]);
    c51_array(argv[1]);
    c52_cube(argv[1]);
    c53_cube_array(argv[1]);
    c54_3d(argv[1]);
    c55_compute(argv[1]);
    nvn_gpu_err e15;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e15)), "C44 close：%d %s", e15.rc.code, e15.why);
    if (!open_backend()) return 1;
    c45_multi_blend(argv[1]);
    nvn_gpu_err e16;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e16)), "C45 close：%d %s", e16.rc.code, e16.why);
    if (!open_backend()) return 1;
    c48_mismatch(argv[1]);
    nvn_gpu_err e17;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e17)), "C48 mismatch close：%d %s", e17.rc.code, e17.why);
    if (!open_backend()) return 1;
    // 成功路径同会话；C58a–c 预期失败提交，每个独占会话（失败段进终态，关会话重开恢复）。
    c56_push_fs(argv[1]);
    c57_push_cs(argv[1]);
    c58d_push_pipe_resize(argv[1]);
    c59a_buffer_view();
    c59_tbuf(argv[1]);
    c60_image_tbuf(argv[1]);
    c61_storage_image(argv[1]);
    c62_tsamp_cs(argv[1]);
    c63_desc_array_elem(argv[1]);
    c64_va_red(argv[1]);
    c67_viewport_yflip(argv[1]);
    c68_stencil_ops(argv[1]);
    c74_clear_rect(argv[1]);
    c75_depth_bias(argv[1]);
    c76_vs_only_depth(argv[1]);
    c77_null_color_slot(argv[1]);
    c79_masked_clear(argv[1]);
    c80_front_face(argv[1]);
    c81_int_sampled(argv[1]);
    c82_occlusion(argv[1]);
    c83_blit();
    c84_validation_classes(); c84b_bc_block_copy();
    c85_descriptor_reuse(argv[1]);
    c86_polygon_modes(argv[1]);
    c87_resolve(argv[1]);
    c86b_3d_slice_attach();
    c86c_bc_3d_slice_copy();
    nvn_gpu_err e18;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C56–C68 / C74 / C75 成功路径 close：%d %s", e18.rc.code, e18.why);
    for (int k = 0; k < 8; k++) {
        if (!open_backend()) return 1;
        c87_resolve_reject(k);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C87 拒绝 close：%s", e18.why);
    }
    if (!open_backend()) return 1;
    c74b_clear_rect_oob();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C74b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c85_redundant_state(argv[1]);   // 独占会话：时间线信号不受前后用例的排列影响
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C85 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58a_push_oob_cs(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58a close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58b_push_bad_stage(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58c_push_oob_fs(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58c close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c65_depth_only_pass();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C65 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c66_pool_sets(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C66 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c60b_unknown_image_tbuf(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C60b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c60c_unknown_image_tbuf_read(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C60c close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c59b_legacy_binding(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C59b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c69_dual_src_blend(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C69 close：%d %s", e18.rc.code, e18.why);

    if (!open_backend()) return 1;
    cube_array_one_face();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "cube-array view close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;   // C197 自己一个会话：围栏号从这里重新算，和前后用例不相干
    c197_pass_cache();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C197 close：%d %s", e18.rc.code, e18.why);

    for (int sw = 0; sw < 2; sw++) {
        if (!open_backend()) return 1;
        attach_swizzle_case(argv[1], sw);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "attachment swizzle close：%d %s", e18.rc.code, e18.why);
    }

    for (int srgb = 0; srgb < 2; srgb++) {
        if (!open_backend()) return 1;
        attach_swizzle_subresource_case(argv[1], srgb);
        CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "attachment subresource close：%d %s", e18.rc.code, e18.why);
    }

    if (!open_backend()) return 1;
    pipeprobe_nearest_test(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "pipeprobe close：%d %s", e18.rc.code, e18.why);
    CHECK(g_pipeprobe_seen == 15, "管线探针应见真实 hit/miss/create 与 poly 差异，位图应为 15，实际 %u", g_pipeprobe_seen);
    printf("nvn_gpu Vulkan 后端 C01–C08/C23/C25/C26/C27–C55/C56–C69/C74–C75 + 清屏/复制/深度/纹理三角形用例：%s（%d 处不符，done %d 次，retired %d 次）\n",
           g_fail ? "红" : "绿", g_fail, atomic_load(&g_ndone), atomic_load(&g_retired));
    s2a_vk_device_destroy(&g_dev, 1);   // 最后一个会话已在 t20 close 里证明空闲
    { extern uint64_t nvn_vk_dbg_dq_recs(void); uint64_t n = nvn_vk_dbg_dq_recs();
      if (getenv("S2A_TEST_NVK_DQ")) { CHECK(n > 1000, "两级回放开着跑完整套：期望交给录制线程的调用远多于 1000 条 → %llu 条", (unsigned long long)n); printf("两级回放：交给录制线程 %llu 条调用\n", (unsigned long long)n); }
      else CHECK(n == 0, "两级回放关着：期望没有调用进队列 → %llu 条", (unsigned long long)n); }
    return g_fail ? 1 : 0;
}
