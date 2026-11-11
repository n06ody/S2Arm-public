#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "nvn_vk_texcache.h"
#include "nvn_vk_texview.h"
#if defined(__APPLE__)
extern void s2a_vk_clear_forget_image(struct s2a_vk_device *, VkImage, uint64_t) __attribute__((weak_import));
#else
extern void s2a_vk_clear_forget_image(struct s2a_vk_device *, VkImage, uint64_t) __attribute__((weak));
#endif
// first_draw.c：跨帧 framebuffer 缓存按 view 失效；视图销毁前必须调用。
void nvn_vk_first_draw_forget_view(VkImageView view);

#include <stdio.h>
#include "nvn_vk_format.h"
#include "nvn_vk_bcdec.h"
#include "nvn_tex.h"
#include "nvn_dirty.h"
#include "nvn_deswizzle.h"
#include <stdlib.h>
#include <android/log.h>
#include <sys/system_properties.h>

// ---- 上传提交/对象记账与保留账本 -------------------------------------------------
// 上传是“一次性提交 + 等完成”。提交失败 = 命令从未执行，staging/CB/fence 可回收；
// 完成未知 = 全部对象仍在 GPU 上，禁止释放/复用，也不能把图像当作可采样成功条目。
static uint64_t g_up_cmd_created, g_up_cmd_freed;
static uint64_t g_up_fence_created, g_up_fence_freed;
static uint64_t g_up_staging_created, g_up_staging_freed;
static uint64_t g_up_submitted, g_up_completed, g_up_unsubmitted, g_up_uncompleted;
static uint64_t g_up_images_retained;
static int g_up_fault;   // 诊断注入：1 = 提交后完成未知；2 = 假装提交失败
static uint64_t g_partial_skipped;   // 诊断：重传时因所在页都没脏而被跳过的子资源数
// A3：按 serial 的状态查询（回放层不持有 cache 指针）。唯一归属仍是本模块：这里只登记当前会话。
// 生产同一时刻只有一个直连会话；未知（未初始化/已销毁）按 UNKNOWN 回答，不推断“从未创建”。
static struct nvn_vk_texcache *g_tex_state_owner;

#define TEX_RETAINED_MAX 32
struct tex_retained {
    VkFence fence;
    VkCommandBuffer cmd;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    VkImage image;
    VkDeviceMemory image_mem;
    VkImageView view;
    VkImageView view_att;
};
static struct tex_retained g_tex_retained[TEX_RETAINED_MAX];
static int g_tex_retained_n;
static VkDevice g_tex_retained_dev;
static uint64_t g_tex_retained_overflow;      // 终态溢出次数（账本满且回收后仍放不下）
// P0-leak ①：账本满时不许“既不销毁也不记账”。三条出路，按代价从低到高：
//   1) 结算已有完成证明的条目（fence 已 signal → staging/CB/fence/图像可以安全销毁）；
//   2) 有界等待已有条目退休（只等已提交过的完成证明，不阻塞未提交工作，200 ms 上限）；
//   3) 仍满 → 终态：准确失败 + 【红】日志 + 计数；句柄归设备寿命（vkDestroyDevice 回收），
//      会话不再假装成功（release_retained 与上传统计都会报出溢出）。
#define TEX_RETAIN_WAIT_NS 200000000ll
static int g_tex_retained_terminal;          // 1 = 已进终态（还有未追踪的在途句柄）
static uint64_t g_tex_retained_overflow_untracked;   // 终态下交给设备寿命的句柄组数
static uint64_t g_tex_retained_reclaimed;    // 溢出时按需结算掉的条目数（诊断）
static uint64_t g_ledger_peak;               // ledger_bytes 的过程峰值（纯计数：真机长局看是否逼近上限）

void nvn_vk_texcache_set_upload_fault(int mode) { g_up_fault = mode; }

void nvn_vk_texcache_upload_stats(struct nvn_vk_tex_upload_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->commands_created = g_up_cmd_created; out->commands_freed = g_up_cmd_freed;
    out->fences_created = g_up_fence_created; out->fences_freed = g_up_fence_freed;
    out->staging_created = g_up_staging_created; out->staging_freed = g_up_staging_freed;
    out->submitted = g_up_submitted; out->completed = g_up_completed;
    out->unsubmitted = g_up_unsubmitted; out->uncompleted = g_up_uncompleted;
    out->images_retained = g_up_images_retained;
    out->retained_entries = (uint32_t)g_tex_retained_n;
    out->retained_overflow = g_tex_retained_overflow;
}

// 真实 VkDeviceMemory 存活数：分配 +1；释放 -1（保留账本回收也要递减）。
static int nvk_subres_ensure(struct nvn_vk_tex *t);          // 批 A：子资源版本表/账本（定义在页级脏位图之前）
static void nvk_subres_set_all(struct nvn_vk_tex *t, int st);
static void nvk_ranges_free(struct nvk_range **a, uint32_t *n, uint32_t *cap);   // 内容归属账本（定义在账本段）
static _Atomic uint64_t g_tex_devmem_live;

// 销毁一组保留句柄并记账：释放数必须与创建数对得上，否则“泄漏”在统计里看不出来。
static void tex_retained_destroy(struct s2a_vk_device *d, struct tex_retained *e) {
    if (e->fence) { vkDestroyFence(d->device, e->fence, NULL); g_up_fence_freed++; e->fence = VK_NULL_HANDLE; }
    if (e->cmd) { vkFreeCommandBuffers(d->device, d->command_pool, 1, &e->cmd); g_up_cmd_freed++; e->cmd = VK_NULL_HANDLE; }
    if (e->staging) { vkDestroyBuffer(d->device, e->staging, NULL); g_up_staging_freed++; e->staging = VK_NULL_HANDLE; }
    if (e->staging_mem) { vkFreeMemory(d->device, e->staging_mem, NULL); e->staging_mem = VK_NULL_HANDLE; }
    if (e->view_att && e->view_att != e->view) { nvn_vk_first_draw_forget_view(e->view_att); vkDestroyImageView(d->device, e->view_att, NULL); }
    if (e->view) { nvn_vk_first_draw_forget_view(e->view); vkDestroyImageView(d->device, e->view, NULL); }
    if (e->image) vkDestroyImage(d->device, e->image, NULL);
    if (e->image_mem) { vkFreeMemory(d->device, e->image_mem, NULL); atomic_fetch_sub(&g_tex_devmem_live, 1); }
    e->view = VK_NULL_HANDLE; e->view_att = VK_NULL_HANDLE; e->image = VK_NULL_HANDLE; e->image_mem = VK_NULL_HANDLE;
}


int nvn_vk_texcache_release_retained(struct s2a_vk_device *d, uint64_t timeout_ns, uint32_t *left_out) {
    if (left_out) *left_out = 0;
    if (!d || !d->device || !g_tex_retained_n) return 0;
    if (g_tex_retained_dev && g_tex_retained_dev != d->device) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "texture upload: %d retained object set(s) belong to a previous device; "
                            "cleared without destroy", g_tex_retained_n);
        memset(g_tex_retained, 0, sizeof g_tex_retained);
        g_tex_retained_n = 0;
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec +
                       (int64_t)(timeout_ns ? timeout_ns : 0);
    // 第一遍：所有带 fence 的条目必须有完成证明；有一个等不到就整体保留（图像没有独立证据）。
    int all_done = 1;
    for (int i = 0; i < g_tex_retained_n; i++) {
        if (!g_tex_retained[i].fence) continue;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
        int64_t left_ns = deadline - now;
        VkResult wr = VK_TIMEOUT;
        if (left_ns > 0)
            wr = vkWaitForFences(d->device, 1, &g_tex_retained[i].fence, VK_TRUE, (uint64_t)left_ns);
        if (wr != VK_SUCCESS) { all_done = 0; break; }
    }
    if (!all_done) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "texture upload: %d retained object set(s) have no completion proof before device "
                            "destroy; kept and reclaimed by device destruction (no fake completion)",
                            g_tex_retained_n);
        if (left_out) *left_out = (uint32_t)g_tex_retained_n;
        return 0;
    }
    int reclaimed = 0;
    for (int i = 0; i < g_tex_retained_n; i++) { tex_retained_destroy(d, &g_tex_retained[i]); reclaimed++; }
    memset(g_tex_retained, 0, sizeof g_tex_retained);
    g_tex_retained_n = 0;
    if (g_tex_retained_terminal) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "texture upload: retained ledger hit its terminal state this session (%llu set(s) were "
                            "beyond the ledger and are owned by the device lifetime; no completion proof was "
                            "dropped silently)", (unsigned long long)g_tex_retained_overflow_untracked);
        g_tex_retained_terminal = 0;
        g_tex_retained_overflow_untracked = 0;
    }
    return reclaimed;
}

// 结算所有 fence 已 signal 的保留项：销毁句柄、压缩表。返回结算条数。
static int tex_retained_reclaim_done(struct s2a_vk_device *d) {
    int w = 0, freed = 0;
    for (int i = 0; i < g_tex_retained_n; i++) {
        struct tex_retained *e = &g_tex_retained[i];
        int done = 1;
        if (e->fence) done = vkGetFenceStatus(d->device, e->fence) == VK_SUCCESS;
        if (!done) { if (w != i) g_tex_retained[w] = *e; w++; continue; }
        tex_retained_destroy(d, e);
        freed++;
    }
    memset(&g_tex_retained[w], 0, sizeof g_tex_retained[0] * (size_t)(g_tex_retained_n - w));
    g_tex_retained_n = w;
    return freed;
}

static struct tex_retained *tex_retain_slot(struct s2a_vk_device *d) {
    if (g_tex_retained_dev && g_tex_retained_dev != d->device) {
        // 旧设备的对象已随设备消失：只清账，不能在新设备上 destroy。
        memset(g_tex_retained, 0, sizeof g_tex_retained);
        g_tex_retained_n = 0;
    }
    g_tex_retained_dev = d->device;
    if (g_tex_retained_n >= TEX_RETAINED_MAX) {
        // 出路 1：先把已有完成证明的条目结算掉（fence 已 signal = 句柄可以安全销毁）。
        int freed = tex_retained_reclaim_done(d);
        // 出路 2：仍然满 → 有界等待已有条目退休（只等已提交过的完成证明；不持锁、不阻塞未提交工作）。
        if (g_tex_retained_n >= TEX_RETAINED_MAX) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t deadline = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec + TEX_RETAIN_WAIT_NS;
            for (int i = 0; i < g_tex_retained_n && g_tex_retained_n >= TEX_RETAINED_MAX; i++) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                int64_t left = deadline - ((int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec);
                if (left <= 0) break;
                if (g_tex_retained[i].fence)
                    (void)vkWaitForFences(d->device, 1, &g_tex_retained[i].fence, VK_TRUE, (uint64_t)left);
            }
            freed += tex_retained_reclaim_done(d);
        }
        if (g_tex_retained_n < TEX_RETAINED_MAX) {
            static int noted_ok;
            if (freed && !noted_ok++)
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                    "texture upload: retained ledger full; reclaimed %d completed set(s) on "
                                    "demand and took the slot", freed);
            g_tex_retained_reclaimed += (uint64_t)freed;
        } else {
            // 出路 3：终态。准确失败并记账；句柄归设备寿命（vkDestroyDevice 回收），
            // 会话不再假装成功：release_retained 与上传统计会报出这次溢出，不会“既不销毁也不记账”。
            static int noted;
            g_tex_retained_terminal = 1;
            g_tex_retained_overflow++;
            g_tex_retained_overflow_untracked++;
            if (!noted++)
                __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                    "【红】texture upload: retained ledger still full (%d) after reclaim + %lld ms "
                                    "bounded wait; %d in-flight set(s) are now owned by the device lifetime "
                                    "(destroyed with vkDestroyDevice) and the session is terminal", TEX_RETAINED_MAX,
                                    (long long)(TEX_RETAIN_WAIT_NS / 1000000), TEX_RETAINED_MAX);
            return NULL;
        }
    }
    struct tex_retained *e = &g_tex_retained[g_tex_retained_n++];
    memset(e, 0, sizeof *e);
    return e;
}

static void retired_release_if_idle(struct nvn_vk_tex *t);   // 定义在槽位管理之后（unpin 路径要用）
// 视图缓存按需增长：2D 默认 8 槽；3D 深度切片一次可能要 depth 个不同视图。
// 全在途时继续增长到 NVK_TEX_VIEWS_MAX（只增 CPU 槽位，VkImageView 仍按实际请求创建）。
static int tex_views_reserve(struct nvn_vk_tex *t, uint32_t cap) {
    if (!t) return -1;
    if (cap <= (uint32_t)t->views_cap) return 0;
    if (cap > NVK_TEX_VIEWS_MAX) cap = NVK_TEX_VIEWS_MAX;
    struct nvn_vk_tex_view *grown = realloc(t->views, (size_t)cap * sizeof *grown);
    if (!grown) return -1;
    memset(grown + t->views_cap, 0, (size_t)(cap - (uint32_t)t->views_cap) * sizeof *grown);
    t->views = grown; t->views_cap = (int)cap;
    return 0;
}
static int tex_has_pinned_views(const struct nvn_vk_tex *t) {
    for (int i = 0; i < t->views_cap; i++)
        if (t->views[i].valid && t->views[i].pins) return 1;
    return 0;
}
// 独立分配要求：Vulkan 允许实现要求某张图必须独占一块内存（VkMemoryDedicatedRequirements）。
// 本实现每张纹理都独立分配；这个判定只用于记账（独立分配次数），查不到就按“不要求”处理。
static int s2a_tex_needs_dedicated(struct s2a_vk_device *d, VkImage image, VkMemoryRequirements *mr) {
    VkImageMemoryRequirementsInfo2 info = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    info.image = image;
    VkMemoryDedicatedRequirements ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 mr2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    mr2.pNext = &ded;
    vkGetImageMemoryRequirements2(d->device, &info, &mr2);   // 该函数按规范返回 void
    if (mr) *mr = mr2.memoryRequirements;
    return ded.requiresDedicatedAllocation ? 1 : 0;
}
// 纹理内存账：世界加载时活纹理上千张，每张一份独立 vkAllocateMemory（= 一次设备内存映射）。
// 映射耗尽会让驱动 mmap 失败（errno 12），必须在报告里把这类分配单独列出来。
static _Atomic uint64_t g_tex_alloc_calls, g_tex_alloc_bytes, g_tex_live_bytes, g_tex_live_n;
static void s2a_tex_alloc_track(uint64_t bytes) {
    atomic_fetch_add(&g_tex_alloc_calls, 1);
    atomic_fetch_add(&g_tex_alloc_bytes, bytes);
    atomic_fetch_add(&g_tex_live_bytes, bytes);
    atomic_fetch_add(&g_tex_live_n, 1);
}
static void s2a_tex_free_track(uint64_t bytes) {
    atomic_fetch_sub(&g_tex_live_bytes, bytes);
    if (atomic_load(&g_tex_live_n)) atomic_fetch_sub(&g_tex_live_n, 1);
}
// 真实 VkDeviceMemory 分配次数（每张独立分配计一次）与独立分配次数。
static _Atomic uint64_t g_tex_devmem_calls, g_tex_dedicated_calls;
// 重传账：验收“分块保护（整块记脏）是否把上传收益吃回去”必须看字节数，不能只看“不崩”。
static _Atomic uint64_t g_tex_reup_calls, g_tex_reup_bytes_full, g_tex_reup_bytes_pages;
void nvn_vk_texcache_reupload(uint64_t *calls, uint64_t *full_bytes, uint64_t *page_bytes) {
    if (calls) *calls = atomic_load(&g_tex_reup_calls);
    if (full_bytes) *full_bytes = atomic_load(&g_tex_reup_bytes_full);
    if (page_bytes) *page_bytes = atomic_load(&g_tex_reup_bytes_pages);
}
void nvn_vk_texcache_devmem(uint64_t *calls, uint64_t *live, uint64_t *dedicated) {
    if (calls) *calls = atomic_load(&g_tex_devmem_calls);
    if (live) *live = atomic_load(&g_tex_devmem_live);
    if (dedicated) *dedicated = atomic_load(&g_tex_dedicated_calls);
}
void nvn_vk_texcache_mem(uint64_t *calls, uint64_t *bytes, uint64_t *live_bytes, uint64_t *live_n) {
    if (calls) *calls = atomic_load(&g_tex_alloc_calls);
    if (bytes) *bytes = atomic_load(&g_tex_alloc_bytes);
    if (live_bytes) *live_bytes = atomic_load(&g_tex_live_bytes);
    if (live_n) *live_n = atomic_load(&g_tex_live_n);
}

static void release(struct s2a_vk_device *d, struct nvn_vk_tex *t) {
    if (t->pins) {   // 在途命令仍引用：绝不在这里销毁（调用方先 unpin，flush 后才会到这一步）
        return;
    }
    // 生产路径每笔采样同时 pin 底图与 view；这里仍单独挡住“只 pin view”的调用，
    // 宁可保留到设备销毁，也不销毁在途 VkImageView。
    if (tex_has_pinned_views(t)) return;
    if (t->upload_incomplete) {
        // 初始上传没有完成证明：图像/内存/视图都可能在 GPU 上读过，销毁即违反 Vulkan。
        // 交给保留账本（持有句柄到设备销毁），槽本身清空、绝不作为可命中条目返回。
        struct tex_retained *e = tex_retain_slot(d);
        if (e) {
            e->image = t->image; e->image_mem = t->memory;
            e->view = t->view; e->view_att = t->view_att == t->view ? VK_NULL_HANDLE : t->view_att;
            // 槽里其它附加视图也引用同一图像：随账本一起保留到设备销毁（不做单独记账）。
            g_up_images_retained++;
        } else {
            // 终态：账本放不下这组 image/memory/view；它们归设备寿命（vkDestroyDevice 回收），
            // 溢出已在 tex_retain_slot 里计数并打【红】，这里只把同一笔记到图像账上。
            g_tex_retained_overflow_untracked++;
        }
        struct nvn_vk_texcache *owner = t->owner;
        s2a_tex_free_track(t->bytes);   // 对象已转入保留账本、不再由缓存槽记账；不扣会让 live_bytes 永远偏大
        free(t->views);            // 附加视图句柄已随账本保留到设备销毁，这里只释放槽位表
        free(t->subres); free(t->gobs);   // 批 A：内容版本表/账本不持有 GPU 资源，直接还
        nvk_ranges_free(&t->pend, &t->n_pend, &t->cap_pend);
        nvk_ranges_free(&t->sync_r, &t->n_sync, &t->cap_sync);
        memset(t, 0, sizeof *t);   // 条目管理信息（owner/槽位）必须保留：否则原槽重建后再退役会漏回收
        t->owner = owner;
        return;
    }
    if (s2a_vk_clear_forget_image) s2a_vk_clear_forget_image(d, t->image, t->generation);
    for (int i = 0; i < t->views_cap; i++)
        if (t->views[i].valid && t->views[i].view) { nvn_vk_first_draw_forget_view(t->views[i].view); vkDestroyImageView(d->device, t->views[i].view, NULL); }
    if (t->view_att && t->view_att != t->view) { nvn_vk_first_draw_forget_view(t->view_att); vkDestroyImageView(d->device, t->view_att, NULL); }
    if (t->image) vkDestroyImage(d->device, t->image, NULL);
    if (t->memory) {
        vkFreeMemory(d->device, t->memory, NULL); atomic_fetch_sub(&g_tex_devmem_live, 1);
        s2a_tex_free_track(t->bytes);
        t->memory = VK_NULL_HANDLE;
    }
    struct nvn_vk_texcache *owner = t->owner;
    free(t->views);
    if (t->gobs && owner && owner->ledger_bytes >= (uint64_t)t->ngobs * sizeof(uint64_t))
        owner->ledger_bytes -= (uint64_t)t->ngobs * sizeof(uint64_t);
    free(t->subres);   // 批 A：子资源内容版本表与 guest 内容账本随条目一起回收
    free(t->gobs);
    nvk_ranges_free(&t->pend, &t->n_pend, &t->cap_pend);   // 内容归属账本（声明写范围）
    nvk_ranges_free(&t->sync_r, &t->n_sync, &t->cap_sync);
    memset(t, 0, sizeof *t);
    t->owner = owner;   // 同上：条目可被原槽复用，owner 不能丢
}

// 上传失败的统一收尾：rc == -2（已提交但没有完成证明）时先标记，release 只保留不销毁。
static void release_upload_failure(struct s2a_vk_device *d, struct nvn_vk_tex *t, int rc) {
    if (rc == -2) t->upload_incomplete = 1;
    release(d, t);
}

void nvn_vk_texcache_pin(struct nvn_vk_tex *t) { if (t) t->pins++; }
void nvn_vk_texcache_unpin(struct nvn_vk_tex *t) { if (t && t->pins) { t->pins--; retired_release_if_idle(t); } }

// 视图级引用：LRU 只回收 pins==0 的条目。底图 pin 只保 image，不保 view。
static uint64_t g_view_created, g_view_reused, g_view_evicted, g_view_pinned_full;
static uint64_t g_bcrt_gpu_marks;   // 块编码影子把字节拷回压缩底图的次数（诊断）
void nvn_vk_texcache_view_stats(struct nvn_vk_tex_view_stats *out) {
    if (!out) return;
    out->created = g_view_created; out->reused = g_view_reused;
    out->evicted = g_view_evicted; out->pinned_full_rejects = g_view_pinned_full;
}
int nvn_vk_texcache_pin_view(struct nvn_vk_tex *t, VkImageView view) {
    if (!t || !view) return -1;
    for (int i = 0; i < t->views_cap; i++)
        if (t->views[i].valid && t->views[i].view == view) { t->views[i].pins++; return 0; }
    return -1;
}
void nvn_vk_texcache_unpin_view(struct nvn_vk_tex *t, VkImageView view) {
    if (!t || !view) return;
    for (int i = 0; i < t->views_cap; i++)
        if (t->views[i].valid && t->views[i].view == view) {
            if (t->views[i].pins) t->views[i].pins--;
            retired_release_if_idle(t);
            return;
        }
}
// 测试/诊断可把视图缓存容量调小以触达真实 LRU 与“全部在途”分支（只缩不扩）。
// 返回 0 = 无覆盖（使用每条纹理自己的 views_cap）。
static int texview_env_capacity(void) {
    static int cap = -1;
    if (cap < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.view_max", v);
        int n = v[0] ? atoi(v) : 0;
        cap = (n > 1) ? n : 0;
    }
    return cap;
}

static int64_t tex_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
// CPU 会在纹理存续期间改内容（视频帧/流式资源）。逐级上传路径已经存在，命中时用整块内容哈希
// 判断是否重传；不上页保护（Android 的 fault 跟踪不属于本模块），只按时间片复查。
// 内容变化检测：按 8 字节块做 FNV-1a（视频纹理单张 5–6 MB，逐字节版本单次哈希就要数毫秒）。
static uint64_t tex_hash(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, b + i, 8);
        h ^= v;
        h *= 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// 诊断：debug.s2a.dump_frame=3 时，把前 8 张上传的纹理（已按真实格式/行距整理后的紧凑数据）落成 raw，
// 便于核对“采样输入”本身是否就已经错了（格式/行距/反混淆/上传），不参与绘制语义。
static void dump_texture_input(const void *data, size_t bytes, uint32_t w, uint32_t h, uint32_t level, uint32_t layer, uint32_t bpp) {
    static int dumped;
    char mode[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.dump_frame", mode);
    char dir[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.dump_fail", dir);
    // 只落“非均匀”输入（前 256 字节不是同一个值），最多 16 张：世界流式纹理排在标题/菜单之后，
    // 全 0 的创建上传没有诊断价值；紫/绿帧要抓的正是“非均匀、可能是垃圾”的那几张。
    if (mode[0] != '3' || dir[0] != '/' || dumped >= 16) return;
    const uint8_t *p = data;
    int uniform = bytes > 0;
    for (size_t i = 1; i < bytes && i < 256; i++) if (p[i] != p[0]) { uniform = 0; break; }
    if (uniform) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/tex-%d-%ux%u-l%u-a%u-bpp%u.raw", dir, dumped, w, h, level, layer, bpp);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    size_t wr = fwrite(data, 1, bytes, f);
    fclose(f);
    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "tex dump: %s %ux%u bpp=%u bytes=%zu written=%zu", path, w, h, bpp, bytes, wr);
    dumped++;
}

// 上传一个 mip/层/切片。返回 0 = 完成；-1 = 没有提交成功（staging/CB/fence 已回收）；
// -2 = 已提交但没有完成证明（staging/CB/fence 转入保留账本，图像由调用方按 upload_incomplete 保留）。
static int upload(struct s2a_vk_device *d, VkImage img, uint32_t w, uint32_t h, uint32_t depth, const void *data, size_t bytes,
                  uint32_t row_bytes, uint32_t mip, uint32_t layer, VkImageAspectFlags aspect) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf; if (vkCreateBuffer(d->device, &bci, NULL, &buf) != VK_SUCCESS) return -1;
    g_up_staging_created++;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkDeviceMemory mem;
    if (vkAllocateMemory(d->device, &mai, NULL, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        return -1;
    }
    vkBindBufferMemory(d->device, buf, mem, 0);
    void *p;
    if (vkMapMemory(d->device, mem, 0, bytes, 0, &p) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        vkFreeMemory(d->device, mem, NULL);
        return -1;
    }
    memcpy(p, data, bytes); vkUnmapMemory(d->device, mem);
    dump_texture_input(data, bytes, w, h, mip, layer, w ? row_bytes / w : 0);

    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = d->command_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(d->device, &cbi, &cmd) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        vkFreeMemory(d->device, mem, NULL);
        return -1;
    }
    g_up_cmd_created++;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        vkFreeMemory(d->device, mem, NULL);
        return -1;
    }
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = (VkImageSubresourceRange){aspect, mip, 1, layer, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy bc = {0};
    // Vulkan 的 copy region 只允许一个 aspect；NVN 的 DEPTH|STENCIL 打包纹理先只传深度面
    // （stencil 拆分尚未实现），屏障仍按合并 aspect 过渡，布局合法。
    VkImageAspectFlags copy_aspect = aspect;
    if ((aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ==
        (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        copy_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    bc.imageSubresource = (VkImageSubresourceLayers){copy_aspect, mip, layer, 1};
    bc.imageExtent = (VkExtent3D){w, h, depth};
    (void)row_bytes;
    bc.bufferRowLength = 0;   // 紧凑数据：0 = 每行紧接；单位是纹素/块，不能拿字节数除以 4 蒙
    vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    // 停在 GENERAL：回放/清屏/上屏都在 GENERAL 上接力，避免各处 oldLayout 与实际不符。
    VkImageMemoryBarrier b2 = b;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    // TRANSFER_READ 只由 TRANSFER 阶段支持；只写 FRAGMENT_SHADER 会触发
    // VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02820（8E VVL 实测）。
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b2);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        vkFreeMemory(d->device, mem, NULL);
        return -1;
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f = VK_NULL_HANDLE;
    if (vkCreateFence(d->device, &fci, NULL, &f) != VK_SUCCESS) {
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        vkFreeMemory(d->device, mem, NULL);
        return -1;
    }
    g_up_fence_created++;
    if (g_up_fault == 2) {
        // 负例：不提交。命令从未执行，staging/CB/fence 可以回收，图像内容保持未定义但不会被消费。
        vkDestroyFence(d->device, f, NULL); g_up_fence_freed++;
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        vkFreeMemory(d->device, mem, NULL);
        g_up_unsubmitted++;
        return -1;
    }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(d->queue, 1, &si, f) != VK_SUCCESS) {
        vkDestroyFence(d->device, f, NULL); g_up_fence_freed++;
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
        vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
        vkFreeMemory(d->device, mem, NULL);
        g_up_unsubmitted++;
        return -1;
    }
    g_up_submitted++;
    VkResult wr = g_up_fault == 1 ? VK_TIMEOUT : vkWaitForFences(d->device, 1, &f, VK_TRUE, 5000000000ull);
    if (wr != VK_SUCCESS) {
        // 提交成功但没拿到完成证明：staging/CB/fence 与图像都不能释放/复用。
        struct tex_retained *e = tex_retain_slot(d);
        if (e) { e->fence = f; e->cmd = cmd; e->staging = buf; e->staging_mem = mem; }
        g_up_uncompleted++;
        return -2;
    }
    vkDestroyFence(d->device, f, NULL); g_up_fence_freed++;
    vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
    vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
    vkFreeMemory(d->device, mem, NULL);
    g_up_completed++;
    return 0;
}

// #1199：nvn 格式在本机要走 compute 解码（bc_decode_mask 对应位置位）时返回 1。
static int bc_needs_decode(const struct s2a_vk_device *d, uint32_t nvn_format) {
    return nvn_format >= 0x41 && nvn_format <= 0x50 && (d->bc_decode_mask >> (nvn_format - 0x41)) & 1u;
}

// 附件用途只在设备真正支持时加：压缩/BC 格式没有颜色附件能力，硬加会让
// vkCreateImage/vkCreateImageView 触发 VUID，并可能把无效图像交给 framebuffer。
static VkImageUsageFlags tex_attach_usage(struct s2a_vk_device *d, VkFormat fmt) {
    if (nvn_vk_format_is_depth(fmt)) return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(d->physical, fmt, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
        ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0;
}

// 一个 mip/层/切片的上传入口：原生支持 → upload()；缺原生 → compute 解码。返回约定同 upload()。
static int tex_upload_or_decode(struct nvn_vk_texcache *c, VkImage img, uint32_t nvn_format,
                                uint32_t w, uint32_t h, uint32_t depth_slices, const void *lin, size_t bytes,
                                uint32_t row_bytes, uint32_t mip, uint32_t layer, VkImageAspectFlags aspect,
                                char *why, size_t why_size) {
    if (nvn_vk_format_substituted(nvn_format) && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        // #398：D24 换成 D32 存储。upload() 只传深度面，每纹素 4 字节，原 D24 口径是低 24 位 UNORM；
        // D32 深度面要 float，按纹素换算后再传（字节数不变）。
        void *conv = malloc(bytes ? bytes : 1);
        if (!conv) {
            if (why && why_size) snprintf(why, why_size, "D24->D32 depth upload: staging alloc failed");
            return -1;
        }
        memcpy(conv, lin, bytes);
        nvn_vk_depth24_to_f32(conv, bytes / 4);
        int rc = upload(c->d, img, w, h, depth_slices, conv, bytes, row_bytes, mip, layer, aspect);
        free(conv);
        return rc;
    }
    if (!bc_needs_decode(c->d, nvn_format))
        return upload(c->d, img, w, h, depth_slices, lin, bytes, row_bytes, mip, layer, aspect);
    if (depth_slices != 1) {
        if (why && why_size) snprintf(why, why_size, "BC compute decode: 3D depth slices not bridged");
        return -1;
    }
    if (!c->bc) {
        if (why && why_size) snprintf(why, why_size, "BC compute decode: module unavailable");
        return -1;
    }
    return nvn_vk_bcdec_decode(c->bc, c->d, img, nvn_format, w, h, mip, layer, lin, bytes, why, why_size);
}

int nvn_vk_texcache_init(struct nvn_vk_texcache *c, struct s2a_vk_device *d, char *why, size_t why_size) {
    memset(c, 0, sizeof *c); c->d = d;
    // #1199：有需要 compute 解码的 BC 格式就在建缓存时一次性建好全部管线；任何一族建不起来
    // → why 写族名、返回 -1，整局不进直连（owner 裁定：不落 CPU 兜底）。
    if (d->bc_decode_mask) {
        if (nvn_vk_bcdec_create(d, &c->bc, why, why_size)) return -1;
    }
    g_tex_state_owner = c;   // A3：serial 状态/墓碑查询的唯一归属（同一时刻一个直连会话）
    return 0;
}

void nvn_vk_texcache_destroy(struct nvn_vk_texcache *c) {
    if (!c) return;
    if (g_tex_state_owner == c) g_tex_state_owner = NULL;
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
        "texcache destroy: created=%llu finalized=%llu retired=%llu retired_mb=%llu alias=%llu dirty(checks=%llu hits=%llu reupload_mb=%llu versioned=%llu version_full=%llu) arm_failed=%llu "
        "content(checks=%llu gobs=%llu gobs_changed=%llu patches=%llu full=%llu ledger_fail=%llu gpu_marks=%llu mixed=%llu reject=%llu ledger_mb=%llu ledger_peak_mb=%llu)",
        (unsigned long long)c->created, (unsigned long long)c->finalized,
        (unsigned long long)c->retired_n, (unsigned long long)(c->retired_bytes >> 20),
        (unsigned long long)c->alias_shared, (unsigned long long)c->dirty_checks,
        (unsigned long long)c->dirty_hits, (unsigned long long)(c->reupload_bytes >> 20),
        (unsigned long long)c->dirty_versioned, (unsigned long long)c->version_full,
        (unsigned long long)c->arm_failed,
        (unsigned long long)c->content_checks, (unsigned long long)c->gobs_scanned,
        (unsigned long long)c->gobs_changed, (unsigned long long)c->patches,
        (unsigned long long)c->full_covers, (unsigned long long)c->ledger_fail,
        (unsigned long long)c->gpu_marks, (unsigned long long)c->mixed,
        (unsigned long long)c->mixed_versioned_rejects, (unsigned long long)(c->ledger_bytes >> 20),
        (unsigned long long)(g_ledger_peak >> 20));
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *t = c->slots[i];
        if (!t) continue;
        release(c->d, t);
        free(t->views);   // release 持约提前返回时也不会漏掉视图表
        nvk_ranges_free(&t->pend, &t->n_pend, &t->cap_pend);   // 同上：早退时账本也不能漏
        nvk_ranges_free(&t->sync_r, &t->n_sync, &t->cap_sync);
        free(t);
    }
    free(c->slots);
    c->slots = NULL; c->nslots = 0; c->cap = 0;
    free(c->sidx_key); free(c->sidx_val);
    c->sidx_key = NULL; c->sidx_val = NULL; c->sidx_cap = 0; c->sidx_n = 0;
    nvn_vk_bcdec_destroy(c->d, c->bc);
}

// 新槽：优先复用空位；数组按需增长。条目单独 malloc，地址稳定（pin 列表保存的是裸指针），
// 数组增长不会让别处持有的指针失效。
static struct nvn_vk_tex *slot_new(struct nvn_vk_texcache *c) {
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *t = c->slots[i];
        if (t && t->valid) continue;
        if (!t) {
            if (!(t = calloc(1, sizeof *t))) return NULL;
            c->slots[i] = t;
        } else {
            free(t->views);            // 上一次 release 已释放；这里兜底提前返回留下的表
            memset(t, 0, sizeof *t);   // 复用已释放的条目：不 free，避免任何悬空指针
        }
        t->owner = c;
        c->created++;
        return t;
    }
    if (c->nslots == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        struct nvn_vk_tex **grown = realloc(c->slots, (size_t)cap * sizeof *grown);
        if (!grown) return NULL;
        memset(grown + c->nslots, 0, (size_t)(cap - c->nslots) * sizeof *grown);
        c->slots = grown; c->cap = cap;
    }
    struct nvn_vk_tex *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->owner = c;
    c->slots[c->nslots++] = t;
    c->created++;
    return t;
}

// 回收：退役条目在最后一个底图/视图引用释放后恰好销毁一次，清空槽位供复用。
// 计数只在这里与 slot_retire 触发，保证 retired_n/bytes 与 live 分开。
static void retired_release_if_idle(struct nvn_vk_tex *t) {
    if (!t || !t->owner || !t->retired) return;
    if (t->pins || tex_has_pinned_views(t)) return;
    struct nvn_vk_texcache *c = t->owner;
    uint64_t bytes = t->bytes;   // release 会清空条目字段，先记下
    release(c->d, t);   // 清 GPU 资源与内容字段，保留 owner
    c->finalized++;
    if (c->retired_n) c->retired_n--;
    if (c->retired_bytes >= bytes) c->retired_bytes -= bytes; else c->retired_bytes = 0;
    t->retired = 0;
}

// ================= A3：对象身份判据（纯函数，不读写缓存） =================
// 记录里的 tex_t 快照是产者写的副本：游戏可能在同一 serial 上重建对象、也可能刚 Finalize。
// 判据只回答“这份快照本身能不能当附件目标用”：未绑定(NULL) / 已绑定但描述无效 / 描述有效。
// 不依据零字段推断 guest 正在重建，也不在这里做资源创建或 no-op。
// 注意：这是附件/清除目标的判据（宽高、层、mip、格式、pool 必须在 NVN 合法域内且互相自洽）。
int nvn_vk_texcache_tex_identity(const tex_t *t, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
#define TEX_ID_INVALID(reason) do { if (why && why_size) snprintf(why, why_size, "%s", (reason)); return NVN_TEX_ID_DESC_INVALID; } while (0)
    if (!t) return NVN_TEX_ID_UNBOUND;
    if (!t->serial) TEX_ID_INVALID("identity serial is 0");
    if (t->magic != MAGIC('T', 'E', 'X', 0)) TEX_ID_INVALID("object magic is not NVNtexture");
    if (!t->mem) TEX_ID_INVALID("no CPU storage (pool memory is gone)");
    if (!t->pool) TEX_ID_INVALID("no memory pool");
    // 1D 的 height 不参与布局（shape() 强制为 1），其余目标的 height 是真实范围/层数。
    if (!t->d.width || (t->d.target != NVN_TEXTURE_TARGET_1D && !t->d.height)) TEX_ID_INVALID("extent is 0");
    if (t->d.target < NVN_TEXTURE_TARGET_1D || t->d.target > NVN_TEXTURE_TARGET_BUFFER)
        TEX_ID_INVALID("target outside the NVN domain");
    if (t->d.format <= 0) TEX_ID_INVALID("format is NONE");
    const uint32_t levels = t->d.levels ? t->d.levels : 1;
    uint32_t maxdim = t->d.width > t->d.height ? t->d.width : t->d.height;
    if (t->d.target == NVN_TEXTURE_TARGET_3D && t->d.depth > maxdim) maxdim = t->d.depth;
    if (levels > 16 || (levels > 1 && (1u << (levels - 1)) > maxdim))
        TEX_ID_INVALID("mip count cannot fit the extent");
    // 2D_ARRAY/MS_ARRAY/CUBE_ARRAY 的 depth 就是层数（shape() 语义），不能当“非 3D 就非法”；
    // 只有 3D 目标的 depth 是真实深度，必须 > 0。
    if (t->d.target == NVN_TEXTURE_TARGET_3D && !t->d.depth) TEX_ID_INVALID("3D target has depth 0");
    if (!t->L.layers) TEX_ID_INVALID("layer count is 0");
    if (t->d.target == NVN_TEXTURE_TARGET_3D && t->L.layers != 1)
        TEX_ID_INVALID("3D target is not layered");
    if (t->d.target == NVN_TEXTURE_TARGET_CUBEMAP && t->L.layers != 6)
        TEX_ID_INVALID("cubemap does not have 6 faces");
    if (t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && (t->L.layers % 6))
        TEX_ID_INVALID("cube array layer count is not a multiple of 6");
    if (t->d.samples < 0 || t->d.samples > 8) TEX_ID_INVALID("sample count outside the NVN domain");
    {
        const int ms = t->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE ||
                       t->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;
        if (t->d.samples > 1 && !ms) TEX_ID_INVALID("multisample count on a non-multisample target");
        if (ms && t->d.samples <= 1) TEX_ID_INVALID("multisample target declares a single sample");
    }
#undef TEX_ID_INVALID
    return NVN_TEX_ID_DESC_VALID;
}

// 状态判据：三个输入（活表命中 / 墓碑命中 / 墓碑表是否淘汰过）→ serial 状态。
// 同一 serial 活条目优先于任何墓碑记录：serial 复用后新对象是新的身份代际，不能被旧墓碑拦下。
static int tex_serial_state_from(int live_hit, int tomb_hit, int tomb_evicted) {
    if (live_hit) return NVN_TEX_SERIAL_LIVE;
    if (tomb_hit) return NVN_TEX_SERIAL_RETIRED;
    if (tomb_evicted) return NVN_TEX_SERIAL_UNKNOWN;   // 有界表丢过条目：不能断言“从未创建”
    return NVN_TEX_SERIAL_NEVER;
}
// 墓碑环：满后覆盖最旧；每写一条推进退休代际（generation 单调，调用方给的 generation 被覆盖）。
static void tex_tombstones_add(struct nvn_tex_tombstones *tt, struct nvn_tex_tombstone e) {
    if (!tt || !e.serial) return;   // 没有身份可锚定的条目不能当墓碑（serial 0 不是可查询身份）
    // 同一 serial 再次退休（同 serial 重建后再 Finalize）：更新原条目，避免查询报出更旧的描述/代际。
    for (uint32_t i = 0; i < tt->n; i++) {
        if (tt->e[i].serial == e.serial) {
            e.generation = ++tt->generation;
            tt->e[i] = e;
            return;
        }
    }
    uint32_t slot;
    if (tt->n < NVK_TEX_TOMB_MAX) {
        slot = tt->n++;
    } else {
        slot = tt->next;
        tt->evicted++;
    }
    tt->next = (slot + 1u < NVK_TEX_TOMB_MAX) ? slot + 1u : 0u;
    e.generation = ++tt->generation;
    tt->e[slot] = e;
}
static const struct nvn_tex_tombstone *tex_tombstones_find(const struct nvn_tex_tombstones *tt, uint64_t serial) {
    if (!tt || !serial) return NULL;
    for (uint32_t i = 0; i < tt->n; i++)
        if (tt->e[i].serial == serial) return &tt->e[i];
    return NULL;
}

// 退役：Finalize 到达。还有引用就挂账等最后一次释放；没有引用立即回收。
static void slot_retire(struct nvn_vk_texcache *c, struct nvn_vk_tex *t) {
    if (!c || !t || t->retired) return;   // 重复 Finalize：第二次进来什么都不做
    // A3：先留下身份与描述快照（serial/尺寸/格式/目标），再清活条目字段。墓碑只记事实，
    // 不参与回收决策；表满覆盖最旧并累计 evicted，之后“查不到”按未知回答。
    {
        struct nvn_tex_tombstone tomb;
        memset(&tomb, 0, sizeof tomb);
        tomb.serial = t->serial;
        tomb.width = t->width; tomb.height = t->height; tomb.depth = t->depth;
        tomb.levels = t->levels; tomb.layers = t->layers;
        tomb.nvn_format = t->nvn_format; tomb.nvn_target = t->nvn_target;
        tex_tombstones_add(&c->tombstones, tomb);
    }
    t->retired = 1;
    t->serial = 0;
    t->gpu_written = 0;
    if (t->pins || tex_has_pinned_views(t)) {   // 等最后一次 unpin
        c->retired_n++;
        c->retired_bytes += t->bytes;
        return;
    }
    release(c->d, t);   // 无引用：立即回收，条目保留在数组里（valid=0）供复用
    c->finalized++;
}

static struct nvn_vk_tex *get_impl(struct nvn_vk_texcache *c, uint64_t key, uint64_t serial,
                                  uint32_t nvn_format, uint32_t w, uint32_t h, const void *data, size_t bytes, uint32_t row_bytes) {
    struct nvn_vk_tex *t = NULL;
    // 身份优先 serial；serial==0（无身份调用）才退回地址。
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *s = c->slots[i];
        if (!s || !s->valid || s->retired) continue;
        if (serial ? s->serial == serial : s->key == key) { t = s; break; }
    }
    if (t && t->serial == serial && t->nvn_format == nvn_format && t->width == w && t->height == h) {
        t->last_use = ++c->clock;
        return t;   // 命中
    }
    if (t) {
        // 同址换了描述：旧条目在途时退役另开一条，绝不就地销毁仍在 GPU 上读的图像。
        if (t->pins || tex_has_pinned_views(t)) {
            // 反例 F5：这里原来手工把 retired/serial 清掉就另开一条，绕过了退役账本
            // （retired_n/retired_bytes 不记、旧图像与视图也没进“等最后一次 unpin 才回收”的路径），
            // 于是同一存储上的第十个 serial 在新建图像失败时会留下一条被改写过的旧条目。
            // 统一走 slot_retire：有引用就入账等最后一次释放，绝不就地改写旧图像。
            slot_retire(c, t);
            t = NULL;
        } else {
            release(c->d, t);
            c->reclaimed++;
        }
    }
    if (!t) t = slot_new(c);
    if (!t) return NULL;
    if (tex_views_reserve(t, NVK_TEX_VIEWS)) return NULL;
    // 直连贴图缓存保持存储映射（不消费 expand）：需要上传展开的格式走 nvn_gpu_tex_create 的贴图路径，
    // 这里不混用；三分量等格式在此路径仍按源字节直传。
    VkFormat fmt = nvn_vk_format(nvn_format);
    if (fmt == VK_FORMAT_UNDEFINED) return NULL;
    // #1199：compute 解码的 BC 格式建为解码后格式 + STORAGE + MUTABLE（同 get_tex 口径）。
    // sRGB 解码格式物理图建 UNORM（sRGB+STORAGE 移动端普遍不支持），SRGB 只放视图。
    VkFormat img_fmt = fmt;
    const int bc_dec = bc_needs_decode(c->d, nvn_format);
    if (bc_dec) {
        VkFormat dfmt = VK_FORMAT_UNDEFINED;
        if (!nvn_vk_bcdec_route(nvn_format, NULL, &dfmt, &img_fmt, NULL, NULL) || dfmt == VK_FORMAT_UNDEFINED) return NULL;
        fmt = dfmt;
    }
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = img_fmt; ici.extent = (VkExtent3D){w, h, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    // 深度/模板格式必须按 DEPTH_STENCIL_ATTACHMENT 建；用 COLOR_ATTACHMENT 建再当深度附件用会让 GPU 直接丢设备。
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                (bc_dec ? VK_IMAGE_USAGE_STORAGE_BIT : 0) |
                tex_attach_usage(c->d, img_fmt);
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (bc_dec) ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (vkCreateImage(c->d->device, &ici, NULL, &t->image) != VK_SUCCESS) { free(t->views); memset(t, 0, sizeof *t); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(c->d->device, t->image, &mr);
    const int dedicated = s2a_tex_needs_dedicated(c->d, t->image, &mr);
    t->bytes = mr.size;
    t->mem_type = s2a_vk_memory_type(c->d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    {
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = t->mem_type;
        if (vkAllocateMemory(c->d->device, &mai, NULL, &t->memory) != VK_SUCCESS) { release(c->d, t); return NULL; }
        atomic_fetch_add(&g_tex_devmem_calls, 1);
        atomic_fetch_add(&g_tex_devmem_live, 1);
        if (dedicated) atomic_fetch_add(&g_tex_dedicated_calls, 1);
        s2a_tex_alloc_track(mr.size);   // 先记账再 bind：bind 失败走 release() 的 free_track，不会下溢
    }
    if (vkBindImageMemory(c->d->device, t->image, t->memory, 0) != VK_SUCCESS) { release(c->d, t); return NULL; }
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = t->image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    VkImageAspectFlags impl_aspect = nvn_vk_format_is_depth(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange = (VkImageSubresourceRange){impl_aspect, 0, 1, 0, 1};
    if (vkCreateImageView(c->d->device, &vci, NULL, &t->view) != VK_SUCCESS) { release(c->d, t); return NULL; }
    t->views[0].view = t->view; t->views[0].nlevels = 1; t->views[0].nlayers = 1;
    t->views[0].format = fmt; t->views[0].type = VK_IMAGE_VIEW_TYPE_2D;
    t->views[0].comps = vci.components; t->views[0].valid = 1;
    t->view_att = t->view;
    if (impl_aspect == VK_IMAGE_ASPECT_DEPTH_BIT && nvn_vk_format_has_stencil(fmt)) {
        vci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(c->d->device, &vci, NULL, &t->view_att) != VK_SUCCESS) { release(c->d, t); return NULL; }
    }
    t->last_use = ++c->clock;
    t->key = key; t->serial = serial; t->nvn_format = nvn_format; t->vk_format = img_fmt; t->width = w; t->height = h; t->valid = 1; t->generation = ++c->next_generation;
    if (data && bytes) {
        int urc = tex_upload_or_decode(c, t->image, nvn_format, w, h, 1, data, bytes, row_bytes, 0, 0, impl_aspect, NULL, 0);
        if (urc) { release_upload_failure(c->d, t, urc); return NULL; }
    }
    return t;
}

void nvn_vk_texcache_sidx_stats(struct nvn_vk_texcache *c, int *n, int *cap) {
    if (n) *n = c ? c->sidx_n : 0;
    if (cap) *cap = c ? c->sidx_cap : 0;
}

void nvn_vk_texcache_usage(struct nvn_vk_texcache *c, int *live, int *pinned, int *gpu_written) {
    int l = 0, p = 0, g = 0;
    if (c) for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *t = c->slots[i];
        if (!t || !t->valid || t->retired) continue;
        l++;
        if (t->pins || tex_has_pinned_views(t)) p++;
        else if (t->gpu_written) g++;
    }
    if (live) *live = l; if (pinned) *pinned = p; if (gpu_written) *gpu_written = g;
}

void nvn_vk_texcache_retired(struct nvn_vk_texcache *c, uint64_t *n, uint64_t *bytes) {
    if (n) *n = c ? c->retired_n : 0;
    if (bytes) *bytes = c ? c->retired_bytes : 0;
}

void nvn_vk_texcache_dirty_stats(struct nvn_vk_texcache *c, uint64_t *checks, uint64_t *hits, uint64_t *bytes) {
    if (checks) *checks = c ? c->dirty_checks : 0;
    if (hits) *hits = c ? c->dirty_hits : 0;
    if (bytes) *bytes = c ? c->reupload_bytes : 0;
}
uint64_t nvn_vk_texcache_versioned(struct nvn_vk_texcache *c) { return c ? c->dirty_versioned : 0; }
uint64_t nvn_vk_texcache_version_full(struct nvn_vk_texcache *c) { return c ? c->version_full : 0; }
uint64_t nvn_vk_texcache_gpu_cpu_conflict(struct nvn_vk_texcache *c) { return c ? c->gpu_cpu_conflict : 0; }
uint64_t nvn_vk_texcache_partial_skipped(struct nvn_vk_texcache *c) { (void)c; return g_partial_skipped; }
uint64_t nvn_vk_texcache_arm_failed(struct nvn_vk_texcache *c) { return c ? c->arm_failed : 0; }

void nvn_vk_texcache_mark_written(struct nvn_vk_texcache *c, VkImage image) {
    if (!c || !image) return;
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *t = c->slots[i];
        if (!t || !t->valid || t->retired || t->image != image) continue;
        // 批 A：GPU 写入登记落到 (aspect, mip, layer)。调用点只给 VkImage、没有区域（见文件头限制），
        // 保守地把全部子资源置 GPU：图像成为事实来源，之后的 CPU 声明写范围会按精确范围覆盖进来。
        if (nvk_subres_ensure(t) == 0) nvk_subres_set_all(t, NVK_SUBRES_GPU);
        else t->gpu_written = 1;   // 版本表分配失败：退回整图标志（仍按“图像是事实来源”处理）
        // 新的 GPU 写代际：旧的“图像==guest”范围全部作废；此前声明但还没落地的 CPU 写范围也被
        // 这次 GPU 写覆盖（程序顺序：flush 在录制该 GPU 写之前），必须丢弃，否则会用旧 CPU 内容盖回。
        nvk_ranges_free(&t->sync_r, &t->n_sync, &t->cap_sync);
        t->sync_truncated = 0;
        nvk_ranges_free(&t->pend, &t->n_pend, &t->cap_pend);
        t->pend_truncated = 0;
        c->gpu_marks++;
        return;
    }
}

static int tex_has_serial(const struct nvn_vk_tex *t, uint64_t serial);   // 定义在本块之后
static void tex_sidx_put(struct nvn_vk_texcache *c, uint64_t serial, int idx);

// ---- serial → 活对象 的哈希提示 ---------------------------------------------------------
// 为什么需要：get_tex / gpu_owned 每笔纹理绑定都要按 serial 找活对象，原来是“遍历全部活条目
// 再逐个比 serial 列表”（3400 个活纹理时每笔数百微秒的扫描，采样剖析里占整帧 CPU 15%）。
// 为什么不维护删除：索引只是提示，命中后仍要验证对象是活的且仍持有该 serial；不成立就回退
// 一次全表扫描并刷新。于是槽复用、别名 serial 增减、退役都不需要在这里同步，只有“回退一次”。
static unsigned tex_sidx_hash(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return (unsigned)x;
}

static int tex_sidx_resize(struct nvn_vk_texcache *c, int ncap) {
    uint64_t *nk = (uint64_t *)calloc((size_t)ncap, sizeof *nk);
    int32_t *nv = (int32_t *)calloc((size_t)ncap, sizeof *nv);
    if (!nk || !nv) { free(nk); free(nv); return -1; }   // 失败保留旧表（提示失效即可，不影响正确性）
    if (c->sidx_cap) {
        const unsigned omask = (unsigned)c->sidx_cap - 1u;
        for (int i = 0; i < c->sidx_cap; i++) {
            if (!c->sidx_val[i]) continue;
            unsigned h = tex_sidx_hash(c->sidx_key[i]) & ((unsigned)ncap - 1u);
            for (int probe = 0; probe < ncap; probe++, h = (h + 1) & ((unsigned)ncap - 1u))
                if (!nv[h]) { nk[h] = c->sidx_key[i]; nv[h] = c->sidx_val[i]; break; }
        }
        (void)omask;
    }
    free(c->sidx_key); free(c->sidx_val);
    c->sidx_key = nk; c->sidx_val = nv; c->sidx_cap = ncap;
    return 0;
}

// 重建：只保留“当前活对象仍持有的 serial”。不重建的话索条目只增不减（每次新登记都塞一个），
// 负载率和内存都会一直长，探测反而比被替换掉的全表扫描更慢——真机上的表现就是“越玩越慢、
// 到换纹理最频繁的 NPC 处最明显”。
static void tex_sidx_rebuild(struct nvn_vk_texcache *c) {
    if (!c) return;
    int want = 0;
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *s = c->slots[i];
        if (!s || !s->valid || s->retired) continue;
        want += 1 + s->nref;
    }
    int ncap = 1024;
    while (ncap < want * 2 + 64 && ncap < (1 << 17)) ncap <<= 1;
    uint64_t *nk = (uint64_t *)calloc((size_t)ncap, sizeof *nk);
    int32_t *nv = (int32_t *)calloc((size_t)ncap, sizeof *nv);
    if (!nk || !nv) { free(nk); free(nv); return; }   // 失败保留旧表：只是提示变差，不影响正确性
    free(c->sidx_key); free(c->sidx_val);
    c->sidx_key = nk; c->sidx_val = nv; c->sidx_cap = ncap; c->sidx_n = 0;
    c->sidx_busy = 1;
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *s = c->slots[i];
        if (!s || !s->valid || s->retired) continue;
        tex_sidx_put(c, s->serial, i);
        for (int k = 0; k < s->nref; k++) tex_sidx_put(c, s->ref_serial[k], i);
    }
    c->sidx_busy = 0;
}

static void tex_sidx_put(struct nvn_vk_texcache *c, uint64_t serial, int idx) {
    if (!c || !serial || idx < 0) return;
    // 条目超过“活槽数的两倍 + 余量”就重建一次（活槽数是活对象数的上界）。
    if (!c->sidx_busy && c->sidx_n > 2 * c->nslots + 4096) tex_sidx_rebuild(c);
    if (c->sidx_cap == 0 || (c->sidx_n + 1) * 4 >= c->sidx_cap * 3) {
        int ncap = c->sidx_cap ? c->sidx_cap * 2 : 1024;
        if (ncap > (1 << 17)) ncap = 1 << 17;
        if (ncap <= c->sidx_cap) return;
        if (tex_sidx_resize(c, ncap) != 0) return;
    }
    const unsigned mask = (unsigned)c->sidx_cap - 1u;
    unsigned h = tex_sidx_hash(serial) & mask;
    for (int probe = 0; probe < c->sidx_cap; probe++, h = (h + 1) & mask) {
        if (c->sidx_val[h] && c->sidx_key[h] != serial) continue;
        if (!c->sidx_val[h]) c->sidx_n++;
        c->sidx_key[h] = serial; c->sidx_val[h] = idx + 1;
        return;
    }
}

static struct nvn_vk_tex *tex_find_serial(struct nvn_vk_texcache *c, uint64_t serial, int *slot_out) {
    if (slot_out) *slot_out = -1;
    if (!c || !serial) return NULL;
    if (c->sidx_cap) {
        const unsigned mask = (unsigned)c->sidx_cap - 1u;
        unsigned h = tex_sidx_hash(serial) & mask;
        for (int probe = 0; probe < c->sidx_cap; probe++, h = (h + 1) & mask) {
            int32_t v = c->sidx_val[h];
            if (!v) break;
            if (c->sidx_key[h] != serial) continue;
            int idx = (int)v - 1;
            if (idx >= 0 && idx < c->nslots) {
                struct nvn_vk_tex *s = c->slots[idx];
                if (s && s->valid && !s->retired && tex_has_serial(s, serial)) {
                    if (slot_out) *slot_out = idx;
                    return s;
                }
            }
            break;   // 提示过期：回退全表扫描并刷新
        }
    }
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *s = c->slots[i];
        if (!s || !s->valid || s->retired || !tex_has_serial(s, serial)) continue;
        tex_sidx_put(c, serial, i);
        if (slot_out) *slot_out = i;
        return s;
    }
    return NULL;
}

struct nvn_vk_tex *nvn_vk_texcache_find_serial(struct nvn_vk_texcache *c, uint64_t serial) {
    return tex_find_serial(c, serial, NULL);
}

// 真实“这张纹理的当前内容归 GPU”查询：按 serial 找活条目，返回其 gpu_written。
// 别名共享：一个对象可服务多个 serial（同一 backing）。主 serial + 引用列表，引用计数即两者之和。
static int tex_has_serial(const struct nvn_vk_tex *t, uint64_t serial) {
    if (!t || !serial) return 0;
    if (t->serial == serial) return 1;
    for (int i = 0; i < t->nref; i++) if (t->ref_serial[i] == serial) return 1;
    return 0;
}
static void tex_add_serial(struct nvn_vk_tex *t, uint64_t serial) {
    if (!t || !serial || tex_has_serial(t, serial) || t->nref >= 8) return;
    t->ref_serial[t->nref++] = serial;
}
// 摘除一个 serial；返回 1 = 这个对象已没有任何 serial（可以退役）。
static int tex_remove_serial(struct nvn_vk_tex *t, uint64_t serial) {
    if (!t) return 1;
    if (t->serial == serial) {
        if (t->nref > 0) { t->serial = t->ref_serial[--t->nref]; return 0; }
        return 1;
    }
    for (int i = 0; i < t->nref; i++) if (t->ref_serial[i] == serial) {
        t->ref_serial[i] = t->ref_serial[--t->nref];
        return 0;
    }
    return 0;
}
int nvn_vk_texcache_gpu_owned(struct nvn_vk_texcache *c, uint64_t serial) {
    if (!c || !serial) return 0;
    struct nvn_vk_tex *s = tex_find_serial(c, serial, NULL);
    return s && s->gpu_written ? 1 : 0;
}

// 批 A：本次 CPU 读回覆盖的子资源是否都能直接读 guest（1 = 全 CPU 内容；0 = 至少含 GPU/混合内容）。
// 只查不建：不创建图像、不上传、不推进任何状态（读回 dst ≠ guest 池已同步）。
int nvn_vk_texcache_read_guest_ok(struct nvn_vk_texcache *c, uint64_t serial, unsigned level,
                                 unsigned layer0, unsigned nlayers) {
    if (!c || !serial) return 1;
    struct nvn_vk_tex *s = tex_find_serial(c, serial, NULL);
    if (!s) return 1;   // 从没被 GPU 用过：guest 就是全部内容
    return nvn_vk_texcache_region_cpu(s, level, layer0, nlayers);
}

void nvn_vk_texcache_mark_cpu_dirty(struct nvn_vk_texcache *c, const tex_t *t) {
    if (!c || !t) return;
    if (t->serial) {
        struct nvn_vk_tex *s = tex_find_serial(c, t->serial, NULL);
        if (s) {
            s->content_check_ms = 0;
            s->content_force = 1;   // 批 A：CPU 写通知 → 下一次解析立刻按账本复查（脏页代数不一定变）
            if (s->gpu_written) c->gpu_cpu_conflict++;
        }
        return;
    }
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *s = c->slots[i];
        if (!s || !s->valid || s->retired) continue;
        if (s->key == (uint64_t)(uintptr_t)t->mem) {
            // 只标脏：下一次解析立刻按内容哈希/页代数复查并只重传变化的分级/层，不销毁图像。
            // 不能清整张 gpu_written：没有页级 GPU 写信息时清掉等于允许旧 CPU 副本覆盖 GPU 结果。
            s->content_check_ms = 0;
            s->content_force = 1;
            if (s->gpu_written) c->gpu_cpu_conflict++;
            return;
        }
    }
}

struct nvn_vk_tex *nvn_vk_texcache_get(struct nvn_vk_texcache *c, uint64_t key, uint64_t serial,
                                      uint32_t nvn_format, uint32_t w, uint32_t h, const void *data, size_t bytes) {
    uint32_t row_bytes = h && bytes ? (uint32_t)(bytes / h) : 0;
    return get_impl(c, key, serial, nvn_format, w, h, data, bytes, row_bytes);
}



static uint32_t mipdim(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// NVN texture swizzle（0 zero / 1 one / 2 R / 3 G / 4 B / 5 A）→ Vulkan 采样分量映射。
// 只在采样 view 上生效；R8 字形图集用 a=R 把覆盖率放到 .a，恒等映射会让 .a 恒为 1。
// 非法枚举必须准确失败，不能退回 identity（6/7 在 packed builder 字段里合法存在，但不是 NVN 合同）。
static int nvn_swz_component(uint32_t v, VkComponentSwizzle *out) {
    switch (v) {
        case 0: *out = VK_COMPONENT_SWIZZLE_ZERO; return 0;
        case 1: *out = VK_COMPONENT_SWIZZLE_ONE; return 0;
        case 2: *out = VK_COMPONENT_SWIZZLE_R; return 0;
        case 3: *out = VK_COMPONENT_SWIZZLE_G; return 0;
        case 4: *out = VK_COMPONENT_SWIZZLE_B; return 0;
        case 5: *out = VK_COMPONENT_SWIZZLE_A; return 0;
        default: return -1;
    }
}
static int nvn_swz_identity(VkComponentMapping *m) {
    *m = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    return 0;
}
// 建立一张精确子资源视图。format 与 aspect 分开给：stencil 面要 S8_UINT + STENCIL，深度面要 DEPTH。
static VkImageView tex_make_view(VkDevice dev, VkImage image,
                                 uint32_t base_level, uint32_t nlevels, uint32_t base_layer, uint32_t nlayers,
                                 VkFormat format, VkImageAspectFlags aspect, VkImageViewType type,
                                 VkComponentMapping comps, char *why, size_t why_size) {
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = image;
    vci.viewType = type;
    vci.format = format;
    vci.components = comps;
    vci.subresourceRange = (VkImageSubresourceRange){ aspect, base_level, nlevels, base_layer, nlayers };
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, NULL, &v) != VK_SUCCESS) {
        if (why && why_size) snprintf(why, why_size, "vkCreateImageView failed (fmt=%d type=%d mip=%u+%u layer=%u+%u)",
                                      (int)format, (int)type, base_level, nlevels, base_layer, nlayers);
        return VK_NULL_HANDLE;
    }
    return v;
}

static int comps_same(VkComponentMapping a, VkComponentMapping b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
static int view_same(const struct nvn_vk_tex_view *a, uint32_t bl, uint32_t nl, uint32_t by, uint32_t ny,
                     VkFormat fmt, VkImageViewType type, VkImageAspectFlags aspect, VkComponentMapping comps) {
    return a->valid && a->base_level == bl && a->nlevels == nl && a->base_layer == by && a->nlayers == ny &&
           a->format == fmt && a->type == type && a->aspect == aspect && comps_same(a->comps, comps);
}
// 视图类型：NVN 目标 → VkImageViewType 的默认映射由 nvn_vk_texview.h 的纯决策给出
// （3D 的层号 = 深度切片；array/cube 的单层附件视图落成 2D/2D_ARRAY）。

// 采样通道映射：NVN 纹理/视图的 swizzle（0 zero 1 one 2 R 3 G 4 B 5 A）→ Vulkan 分量映射。
// 字形图集用 a=R 把覆盖率放到 .a；恒等映射会让 .a 恒为 1（文字变成实心方块）。
static int tex_components_of(uint8_t set_swz, uint16_t swz, VkFormat fmt, VkComponentMapping *out,
                             char *why, size_t why_size) {
    // 深度/模板视图在 Vulkan 里必须恒等；这不是丢掉 NVN 请求，而是视图格式的硬约束。
    if (!set_swz || nvn_vk_format_is_depth(fmt)) return nvn_swz_identity(out);
    uint32_t part[4] = { swz & 7u, (swz >> 3) & 7u, (swz >> 6) & 7u, (swz >> 9) & 7u };
    VkComponentSwizzle c[4];
    for (int i = 0; i < 4; i++) {
        if (nvn_swz_component(part[i], &c[i])) {
            if (why && why_size) snprintf(why, why_size,
                "NVN texture swizzle component %d is %u (valid 0..5); refusing identity fallback", i, part[i]);
            return -1;
        }
    }
    *out = (VkComponentMapping){ c[0], c[1], c[2], c[3] };
    return 0;
}
// 默认视图：用 tex_t 自身的 swizzle（此时缓存槽的 set_swz/swz 还没写）。
static int tex_components_for_tex(const tex_t *t, VkFormat fmt, VkComponentMapping *out,
                                  char *why, size_t why_size) {
    return tex_components_of(t->set_swz, (uint16_t)t->swz, fmt, out, why, why_size);
}
// 视图请求：NVNtextureView 的 swizzle 是四个独立字节（0..5），不是纹理 builder 的 3-bit packed 字段；
// 设置了视图 swizzle 时它覆盖 builder 映射，再按实际格式（R8 缺省 G/B=0、A=1）由 Vulkan 组合。
static int tex_view_components(const struct nvn_vk_tex *t, const view_t *v, VkFormat fmt,
                               VkComponentMapping *out, char *why, size_t why_size) {
    if (!v || !v->set_swizzle)
        return tex_components_of(t->set_swz, (uint16_t)t->swz, fmt, out, why, why_size);
    if (nvn_vk_format_is_depth(fmt)) return nvn_swz_identity(out);
    VkComponentSwizzle c[4];
    for (int i = 0; i < 4; i++) {
        if (nvn_swz_component(v->swz[i], &c[i])) {
            if (why && why_size) snprintf(why, why_size,
                "NVN texture view swizzle component %d is %u (valid 0..5); refusing identity fallback",
                i, v->swz[i]);
            return -1;
        }
    }
    *out = (VkComponentMapping){ c[0], c[1], c[2], c[3] };
    return 0;
}

// NVNtextureTarget（nvn.h）：0 1D / 1 2D / 2 3D / 3 1D_ARRAY / 4 2D_ARRAY / 5 2D_MULTISAMPLE /
// 6 2D_MULTISAMPLE_ARRAY / 7 RECTANGLE / 8 CUBEMAP / 9 CUBEMAP_ARRAY / 10 BUFFER。
// 立方体贴图是 8/9，不是 5/6：5/6 是多采样目标，需要 sample count 与 resolve，当前没有通路。
// 层数由 nvn_tex_layout 给出：CUBEMAP 固定 6，CUBEMAP_ARRAY 的 depth 计的是层-面（6 的倍数）。
#define NVK_TARGET_SIZE_1D 0
#define NVK_TARGET_SIZE_2D 1
#define NVK_TARGET_SIZE_3D 2
#define NVK_TARGET_SIZE_1D_ARRAY 3
#define NVK_TARGET_SIZE_2D_ARRAY 4
#define NVK_TARGET_SIZE_2D_MS 5
#define NVK_TARGET_SIZE_2D_MS_ARRAY 6
#define NVK_TARGET_SIZE_RECT 7
#define NVK_TARGET_SIZE_CUBE 8
#define NVK_TARGET_SIZE_CUBE_ARRAY 9
#define NVK_TARGET_SIZE_BUFFER 10

// ================= 批 A：子资源级内容版本与 CPU/GPU 交接 =================
//
// 把“整图 gpu_written”换成 (aspect, mip, layer) 三态版本，并给出四条交接路径：
//   1) CPU 完整覆盖子资源：guest 的新字节覆盖该子资源的每一段 GOB → 图像整块重传，状态回 CPU；
//   2) CPU 局部更新：只把 guest 真正变了的 GOB 对应的纹素矩形合并进图像，GPU 写过的其余内容保留
//      → 状态 MIXED（全 CPU 子资源在局部更新后仍是 CPU）；
//   3) GPU 写入登记：mark_written 把该图像的全部子资源置 GPU（调用点只给 VkImage，见限制说明）；
//   4) CPU 读取：全部 CPU 的子资源直接读 guest；否则先把待合并的 CPU 更新落到图像，再读图像
//      （把字节交给 dst ≠ guest 纹理池已同步：读图像不改变任何状态与账本）。
//
// 变更定位靠 guest 内容账本（每 GOB 一个 64 位哈希），不靠 1 MiB 写保护块：安卓的 nvn_dirty 以
// 1 MiB 分块保护（避免 AHB 映射被逐页 mprotect 切碎、吃满 vm.max_map_count），块脏 ≠ 纹理被改，
// 更 ≠ 整块被 CPU 改写。账本只在“脏 1 MiB 块”里重算 GOB 哈希，与上次同步值不同才是 guest 真变了的
// 512 B 段。AHB 换底（映射代际变化）时账本全量重扫。
//
// 已知限制（本批不证明的部分，报告里如实列出）：
//   · GPU 写入登记只有 VkImage，没有区域：一次 clear/拷贝/附件写会把整图所有子资源置 GPU。
//     GOB(512 B) 内的 CPU/GPU 混写按 GOB 粒度合并（比 Metal 的 16 KiB 页细 32 倍），GOB 内混写以
//     guest 为准；深度+模板格式、3D 纹理、BC compute 解码图像、线性纹理的局部合并未接通 →
//     fail-closed 报红，绝不静默返回旧内容。
//   · 帧队列完成证明（in-flight 判定、dirty_defer 预算）归批 B：本文件只在已录制的 pin/预算条件下
//     走“退役旧条目 + 新建图像”，没有新增任何完成等待。
//   · nvn_be_tex_gpu_owned_since（脏页轮询的按页归属）仍在 nvn_be_android.c 按整图判定，不在本批范围。

#define NVK_GOB 512u
// 账本上限：超过这个大小的纹理不建 GOB 账本（内存 8 B / 512 B ≈ 1.6%）。真遇到时按子资源整块判定，
// 且只有“全是 CPU 子资源”才允许整块重传，否则 fail-closed。
#define NVK_LEDGER_MAX_BYTES (256ull << 20)

static uint32_t nvk_layers_of(const struct nvn_vk_tex *t) { return t && t->layers ? t->layers : 1u; }
static uint32_t nvk_levels_of(const struct nvn_vk_tex *t) { return t && t->levels ? t->levels : 1u; }

static uint8_t nvk_aspects_of(const struct nvn_vk_tex *t) {
    VkFormat f = t->vk_format ? t->vk_format : nvn_vk_format(t->nvn_format);
    return (uint8_t)((nvn_vk_format_is_depth(f) && nvn_vk_format_has_stencil(f)) ? 2 : 1);
}
static VkImageAspectFlags nvk_img_aspect(const struct nvn_vk_tex *o) {
    VkFormat f = o->vk_format ? o->vk_format : nvn_vk_format(o->nvn_format);
    if (!nvn_vk_format_is_depth(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return nvn_vk_format_has_stencil(f) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
}

static int nvk_subres_ensure(struct nvn_vk_tex *t) {
    if (t->subres) return 0;
    unsigned as = nvk_aspects_of(t), lv = nvk_levels_of(t), ly = nvk_layers_of(t);
    size_t n = (size_t)as * lv * ly;
    if (!n || n > (1u << 21)) return -1;
    uint8_t *p = calloc(n, 1);   // 0 = NVK_SUBRES_CPU
    if (!p) return -1;
    t->subres = p; t->subres_n = (uint32_t)n; t->subres_aspects = (uint8_t)as;
    return 0;
}
static uint32_t nvk_subres_idx(const struct nvn_vk_tex *t, unsigned aspect, unsigned level, unsigned layer) {
    return ((aspect * nvk_levels_of(t)) + level) * nvk_layers_of(t) + layer;
}
static int nvk_subres_get(const struct nvn_vk_tex *t, unsigned aspect, unsigned level, unsigned layer) {
    if (!t || !t->subres) return NVK_SUBRES_CPU;
    unsigned as = t->subres_aspects ? t->subres_aspects : 1;
    if (aspect >= as || level >= nvk_levels_of(t) || layer >= nvk_layers_of(t)) return NVK_SUBRES_CPU;
    return t->subres[nvk_subres_idx(t, aspect, level, layer)];
}
// 某一 (level, layer) 的任一 aspect 不是 CPU → 图像是事实来源（读回必须读图像）。
// 版本表分配失败时退回整图 gpu_written（保守：按 GPU 内容处理，不猜 CPU）。
static int nvk_subres_gpu_any(const struct nvn_vk_tex *t, unsigned level, unsigned layer) {
    if (t && !t->subres) return t->gpu_written ? 1 : 0;
    unsigned as = t && t->subres_aspects ? t->subres_aspects : 1;
    for (unsigned a = 0; a < as; a++) if (nvk_subres_get(t, a, level, layer) != NVK_SUBRES_CPU) return 1;
    return 0;
}
static void nvk_subres_set(struct nvn_vk_tex *t, unsigned level, unsigned layer, int st) {
    if (!t || !t->subres || level >= nvk_levels_of(t) || layer >= nvk_layers_of(t)) return;
    unsigned as = t->subres_aspects ? t->subres_aspects : 1;
    for (unsigned a = 0; a < as; a++) t->subres[nvk_subres_idx(t, a, level, layer)] = (uint8_t)st;
}
static void nvk_subres_set_all(struct nvn_vk_tex *t, int st) {
    if (!t) return;
    if (t->subres) memset(t->subres, st, t->subres_n);
    t->gpu_written = (uint8_t)(st != NVK_SUBRES_CPU);
}
static void nvk_recompute_gpu_flag(struct nvn_vk_tex *t) {
    if (!t || !t->subres) return;   // 没有版本表：保留 gpu_written（版本表分配失败时不能当成全 CPU）
    int g = 0;
    for (uint32_t i = 0; i < t->subres_n; i++) if (t->subres[i] != NVK_SUBRES_CPU) { g = 1; break; }
    t->gpu_written = (uint8_t)g;
}

// 账本哈希：8 字节混合 + 尾巴（只用于同源比较）。
static uint64_t nvk_hash_bytes(const uint8_t *m, size_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t)n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, m + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    for (; i < n; i++) { h ^= m[i]; h *= 0x100000001b3ull; h ^= h >> 29; }
    return h;
}
// 账本上限可被诊断注入覆盖（默认 NVK_LEDGER_MAX_BYTES；0 = 关闭）。
static uint64_t g_ledger_max_override;
void nvn_vk_texcache_set_ledger_max_bytes(uint64_t n) { g_ledger_max_override = n + 1; }   // 0 = 跟随默认
uint64_t nvn_vk_texcache_ledger_max_bytes(void) { return g_ledger_max_override ? g_ledger_max_override - 1 : NVK_LEDGER_MAX_BYTES; }
static uint64_t nvk_ledger_max(void) { return nvn_vk_texcache_ledger_max_bytes(); }

static void nvk_gob_span(const tex_t *t, uint32_t g, uint64_t *lo, uint64_t *n) {
    uint64_t a = (uint64_t)g * NVK_GOB, e = a + NVK_GOB;
    if (e > t->L.size) e = t->L.size;
    *lo = a; *n = e > a ? e - a : 0;
}
static int nvk_ledger_ensure(struct nvn_vk_tex *t, uint64_t size) {
    if (t->gobs) return 0;
    if (!size || size > nvk_ledger_max()) return -1;
    uint32_t n = (uint32_t)((size + NVK_GOB - 1) / NVK_GOB);
    if (!n) return -1;
    uint64_t *p = calloc(n, sizeof *p);
    if (!p) return -1;
    t->gobs = p; t->ngobs = n; t->gobs_ready = 0;
    return 0;
}
static void nvk_ledger_set(struct nvn_vk_tex *t, const uint8_t *mem, uint64_t lo, uint64_t hi) {
    if (!t->gobs || !mem || !t->ngobs) return;
    if (hi > t->dsize) hi = t->dsize;
    if (lo >= hi) return;
    for (uint64_t g = lo / NVK_GOB; g * NVK_GOB < hi && g < t->ngobs; g++) {
        uint64_t a = g * NVK_GOB, e = a + NVK_GOB; if (e > t->dsize) e = t->dsize;
        if (e <= a) break;
        t->gobs[g] = nvk_hash_bytes(mem + a, (size_t)(e - a));
    }
}
static void nvk_ledger_set_all(struct nvn_vk_tex *t, const uint8_t *mem) {
    if (!t->gobs || !mem) return;
    nvk_ledger_set(t, mem, 0, t->dsize);
    t->gobs_ready = 1;
}

struct nvk_chg { uint64_t *bits; uint32_t words, count; };
static int nvk_chg_init(struct nvk_chg *chg, uint32_t ngobs) {
    memset(chg, 0, sizeof *chg);
    if (!ngobs) return -1;
    chg->words = (ngobs + 63) / 64;
    chg->bits = calloc(chg->words, sizeof *chg->bits);
    return chg->bits ? 0 : -1;
}
static void nvk_chg_free(struct nvk_chg *chg) { free(chg->bits); memset(chg, 0, sizeof *chg); }
static int nvk_chg_test(const struct nvk_chg *chg, uint32_t g) {
    return chg->bits && (g >> 6) < chg->words && ((chg->bits[g >> 6] >> (g & 63)) & 1u);
}
static void nvk_chg_mark(struct nvk_chg *chg, uint32_t g) {
    if (!chg->bits || (g >> 6) >= chg->words) return;
    if (!((chg->bits[g >> 6] >> (g & 63)) & 1u)) { chg->bits[g >> 6] |= 1ull << (g & 63); chg->count++; }
}

// ================= 内容归属：CPU 声明写范围账本 =================
// 三份事实分工（不变量）：
//   · GPU 写登记（mark_written）只说明“图像可能被 GPU 改过”，新代际清空 sync_r；
//   · 声明写范围（pend）是 CPU 内容归属的唯一权威：只有它能把 guest 字节写进图像、覆盖 GPU 内容；
//   · 哈希（gobs）只用来 (a) 证明“已同步范围内的 guest 没变”从而少传，(b) 发现“没被声明范围解释的
//     guest 变化”——绝不把哈希相同当成“CPU 没写”，也不把“GOB 变了”当成“整个 GOB 被 CPU 覆盖”。
#define NVK_RANGES_MAX 64

static void nvk_ranges_free(struct nvk_range **a, uint32_t *n, uint32_t *cap) {
    free(*a); *a = NULL; *n = *cap = 0;
}
// 加入 [lo,hi) 并与相邻/重叠区间合并（按 lo 有序）。容量到上限/分配失败 → 置 truncated、返回 -1。
static int nvk_range_add(struct nvn_vk_tex *t, struct nvk_range **arr, uint32_t *n, uint32_t *cap,
                         uint8_t *truncated, uint64_t lo, uint64_t hi) {
    if (hi <= lo) return 0;
    struct nvk_range *a = *arr; uint32_t cnt = *n;
    uint32_t i = 0;
    while (i < cnt && a[i].lo < lo) i++;
    if (i > 0 && a[i - 1].hi >= lo) {
        i--;
        if (a[i].hi < hi) a[i].hi = hi;
    } else {
        if (cnt == *cap) {
            if (*cap >= NVK_RANGES_MAX) { *truncated = 1; return -1; }
            uint32_t nc = *cap ? *cap * 2 : 8;
            if (nc > NVK_RANGES_MAX) nc = NVK_RANGES_MAX;
            struct nvk_range *na = realloc(a, (size_t)nc * sizeof *na);
            if (!na) { *truncated = 1; return -1; }
            a = na; *arr = na; *cap = nc;
        }
        memmove(&a[i + 1], &a[i], (size_t)(cnt - i) * sizeof *a);
        a[i] = (struct nvk_range){ lo, hi };
        cnt++;
        *n = cnt;
    }
    while (i + 1 < cnt && a[i].hi >= a[i + 1].lo) {
        if (a[i + 1].hi > a[i].hi) a[i].hi = a[i + 1].hi;
        memmove(&a[i + 1], &a[i + 2], (size_t)(cnt - i - 2) * sizeof *a);
        cnt--;
        *n = cnt;
    }
    (void)t;
    return 0;
}
// [lo,hi) 是否完全落在区间并集里（列表已合并，不重叠）。
static int nvk_ranges_covered(const struct nvk_range *a, uint32_t n, uint64_t lo, uint64_t hi) {
    for (uint32_t i = 0; i < n && a[i].lo <= lo; i++)
        if (a[i].lo <= lo && a[i].hi >= hi) return 1;
    return 0;
}
static int nvk_ranges_hit(const struct nvk_range *a, uint32_t n, uint64_t lo, uint64_t hi) {
    for (uint32_t i = 0; i < n && a[i].lo < hi; i++)
        if (a[i].hi > lo) return 1;
    return 0;
}
// 一组待同步区间是否全部已被 sync_r 覆盖（含义：这些字节的图像内容已经等于 guest）。
static int nvk_hits_covered(const struct nvk_range *cov, uint32_t nc,
                            const struct nvk_range *hits, uint32_t nh) {
    for (uint32_t i = 0; i < nh; i++)
        if (!nvk_ranges_covered(cov, nc, hits[i].lo, hits[i].hi)) return 0;
    return 1;
}

// 在脏 1 MiB 块范围内重算 GOB 哈希，把与账本不同的 GOB 标进 chg。dpages <= 0 = 全量重扫。
static uint32_t nvk_scan_changed(struct nvn_vk_texcache *c, struct nvn_vk_tex *o, const tex_t *t,
                                 int dpages, const uint64_t *dbits, struct nvk_chg *chg) {
    const size_t pg = nvn_dirty_page_size();
    for (uint32_t g = 0; g < o->ngobs; g++) {
        uint64_t a, n; nvk_gob_span(t, g, &a, &n);
        if (!n) break;
        if (dpages > 0 && pg && dbits) {   // 只扫落在脏块里的 GOB：1 MiB 块脏只当“可能”，不是“整块被 CPU 改写”
            int dirty = 0;
            for (uint64_t p = a / pg; p <= (a + n - 1) / pg && !dirty; p++)
                if (p < (uint64_t)dpages && (dbits[p >> 6] & (1ull << (p & 63)))) dirty = 1;
            if (!dirty) continue;
        }
        c->gobs_scanned++;
        uint64_t h = nvk_hash_bytes(t->mem + a, (size_t)n);
        if (!o->gobs_ready || h != o->gobs[g]) nvk_chg_mark(chg, g);
    }
    return chg->count;
}

// 把存储字节区间 [lo, hi)（相对存储起点，落在第 layer 层内）换成精确的纹素矩形（16 B piece 粒度）。
// Tegra 块线性：一个块行（block row）是 gobs 个 GOB 列横排，每列竖着 2^tileH 个 GOB；
// 一个 GOB = 64 B 宽 × 8 行。GOB 内 16 B piece 才连续，piece 的排列由 nvn_gob_switch2 决定；
// 这里对每个与区间相交的 piece 出矩形（不足一个 piece 的边必须按块对齐，否则由调用方 fail-closed）。
// 3D / 深块（tileD）/ 多采样 / 无布局的级整级返回（这些情况的局部合并按 fail-closed 拒绝，整级返回只用于整块上传）。
typedef void (*nvk_rect_fn)(void *u, unsigned level, unsigned layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
// 返回 0 = 全部矩形已按块对齐生成；-1 = 边未按块对齐/布局不支持（调用方必须 fail-closed，不能只拷一部分）。
static int nvk_bytes_rects(const tex_t *t, unsigned layer, uint64_t lo, uint64_t hi, nvk_rect_fn fn, void *u) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || 64 % bpb) return -1;
    unsigned nl = t->d.levels ? t->d.levels : 1;
    uint64_t ls = t->L.layer_size ? t->L.layer_size : t->L.size;
    uint64_t la = (uint64_t)layer * ls;
    for (unsigned l = 0; l < nl; l++) {
        uint64_t a = la + nvn_tex_level_offset(&t->d, l);
        uint64_t e = l + 1 < nl ? la + nvn_tex_level_offset(&t->d, l + 1) : (uint64_t)(layer + 1) * ls;
        if (hi <= a || lo >= e) continue;
        uint32_t w = mipdim(t->d.width, l), h = mipdim(t->d.height, l);
        unsigned th = 0, td = 0; nvn_tex_level_tiles(&t->d, l, &th, &td); uint32_t gobs = nvn_tex_level_gobs(&t->d, l);
        if (td || t->d.samples > 1 || !gobs) return -1;   // 这些形态不做局部合并（只允许整级覆盖）
        const uint64_t blk = 512ull << th;          // 一个 GOB 列占的字节数
        const uint64_t row = (uint64_t)gobs * blk;  // 一个块行占的字节数
        const uint32_t cw = (uint32_t)(64 / bpb) * (uint32_t)bw;   // 一个 GOB 列横跨的 texel 列数
        const uint32_t rh = (8u << th) * (uint32_t)bh;            // 一个块行横跨的 texel 行数
        const uint64_t b0 = (lo > a ? lo : a) - a, b1 = (hi < e ? hi : e) - a;
        for (uint64_t r = b0 / row; r * row < b1; r++) {
            const uint64_t rs = r * row;
            const uint64_t seg0 = b0 > rs ? b0 : rs;
            const uint64_t seg1 = b1 < rs + row ? b1 : rs + row;
            if (seg1 <= seg0) continue;
            for (uint64_t c = (seg0 - rs) / blk; rs + c * blk < seg1; c++) {
                const uint64_t cs = rs + c * blk;                     // 该 GOB 列的起点（level 内偏移）
                const uint64_t c0 = seg0 > cs ? seg0 : cs;
                const uint64_t c1 = seg1 < cs + blk ? seg1 : cs + blk;
                if (c1 <= c0) continue;
                for (uint64_t k = (c0 - cs) / 16; (cs + k * 16) < c1; k++) {   // k = GOB 内 16 B piece
                    uint64_t ps = cs + k * 16, pe = ps + 16;
                    uint64_t s0 = c0 > ps ? c0 : ps, s1 = c1 < pe ? c1 : pe;
                    if (s1 <= s0) continue;
                    // piece 内偏移 → GOB 内 (x 字节, 行号)：两种 GOB 内顺序分别反解。
                    uint32_t gb = (uint32_t)(k / 32), idx = (uint32_t)(k % 32), xb, yr;
                    if (nvn_gob_switch2) {
                        xb = (idx / 16) * 32 + ((idx / 4) % 2) * 16;
                        yr = ((idx / 8) % 2) * 4 + ((idx / 2) % 2) * 2 + (idx % 2);
                    } else {
                        xb = (idx / 16) * 32 + ((idx / 2) % 2) * 16;
                        yr = ((idx / 4) % 4) * 2 + (idx % 2);
                    }
                    uint64_t in0 = s0 - ps, in1 = s1 - ps;   // piece 内字节偏移
                    if (in0 % (uint64_t)bpb || in1 % (uint64_t)bpb) return -1;   // 边不按块对齐：不能只拷一部分
                    uint32_t gx = (uint32_t)(c * cw) + (xb / (uint32_t)bpb) * (uint32_t)bw + (uint32_t)(in0 / (uint64_t)bpb) * (uint32_t)bw;
                    uint32_t gy = (uint32_t)(r * rh) + (gb * 8u + yr) * (uint32_t)bh;
                    uint32_t gw = (uint32_t)((in1 - in0) / (uint64_t)bpb) * (uint32_t)bw;
                    if (gy >= h || gx >= w || !gw) continue;
                    if (gw > w - gx) gw = w - gx;
                    uint32_t gh = (uint32_t)bh; if (gh > h - gy) gh = h - gy;
                    fn(u, l, layer, gx, gy, gw, gh);
                }
            }
        }
    }
    return 0;
}

// 一个子资源整块从 guest 上传（deswizzle 或线性行拷贝 → upload / BC 解码）。
// 返回 0 成功；-1 失败（why 写原因，未提交）；-2 已提交但没有完成证明（已按保留账本处理）。
static int nvk_upload_subres(struct nvn_vk_texcache *c, struct nvn_vk_tex *o, const tex_t *t,
                             unsigned level, unsigned layer, char *why, size_t why_size) {
    const VkImageAspectFlags aspect = nvk_img_aspect(o);
    const int is3d = t->d.target == NVK_TARGET_SIZE_3D;
    const unsigned lay = is3d ? 0 : layer;
    if (t->d.flags & 0x10) {   // NVN_TEXTURE_FLAGS_LINEAR：行主序，按 stride 拷成紧凑行
        int bw = 1, bh = 1;
        uint32_t bpb = (uint32_t)nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (!bpb) { if (why && why_size) snprintf(why, why_size, "linear subresource: NVN format 0x%x has no block size", t->d.format); return -1; }
        nvn_texlayout L; nvn_tex_layout(&t->d, &L);
        uint32_t w = mipdim(t->d.width, level), h = mipdim(t->d.height, level);
        uint32_t tight = (w + (uint32_t)bw - 1) / (uint32_t)bw * bpb;
        uint32_t pitch = t->d.stride ? t->d.stride : (tight + 31u) & ~31u;
        uint32_t rows = (h + (uint32_t)bh - 1) / (uint32_t)bh;
        uint8_t *b = malloc((size_t)tight * rows);
        if (!b) { if (why && why_size) snprintf(why, why_size, "linear subresource %u/%u alloc failed", level, layer); return -1; }
        const uint64_t base = nvn_tex_level_offset(&t->d, level) + (uint64_t)lay * (L.layer_size ? L.layer_size : L.size);
        for (uint32_t y = 0; y < rows; y++)
            memcpy(b + (size_t)y * tight, (const uint8_t *)t->mem + base + (uint64_t)y * pitch, tight);
        int rc = tex_upload_or_decode(c, o->image, o->nvn_format, w, h, 1, b, (size_t)tight * rows, tight, level, lay, aspect, why, why_size);
        free(b);
        if (rc == -2) release_upload_failure(c->d, o, rc);
        return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
    }
    uint32_t row_bytes = 0, rows = 0, slices = 0, th = 0, td = 0, bpb = 0; uint64_t base = 0;
    if (nvn_deswizzle_geom(&t->d, level, lay, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || !row_bytes || !rows) {
        if (why && why_size) snprintf(why, why_size, "level %u layer %u layout unsupported", level, layer);
        return -1;
    }
    if (slices != (is3d ? mipdim(t->d.depth, level) : 1u)) {
        if (why && why_size) snprintf(why, why_size, "level %u layer %u slice count %u unsupported", level, layer, slices);
        return -1;
    }
    const size_t bytes = (size_t)row_bytes * rows * slices;
    uint8_t *lin = malloc(bytes);
    if (!lin) { if (why && why_size) snprintf(why, why_size, "subresource %u/%u alloc failed", level, layer); return -1; }
    if (nvn_deswizzle(&t->d, level, lay, t->mem, lin, row_bytes)) {
        free(lin);
        if (why && why_size) snprintf(why, why_size, "level %u layer %u deswizzle failed (format %#x target %d)", level, layer, t->d.format, t->d.target);
        return -1;
    }
    int rc = tex_upload_or_decode(c, o->image, o->nvn_format, mipdim(t->d.width, level), mipdim(t->d.height, level),
                                  slices, lin, bytes, row_bytes, level, lay, aspect, why, why_size);
    free(lin);
    if (rc == -2) release_upload_failure(c->d, o, rc);
    return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
}

// 局部合并：只把“CPU 声明写的字节范围”对应的精确纹素矩形拷进图像，其余内容（GPU 写过的）原样保留。
// 一次反混淆到 staging + 每矩形一条 vkCmdCopyBufferToImage；不改其它子资源、不读回、不整块重传。
struct nvk_rect { uint32_t x, y, w, h; };
struct nvk_rects { struct nvk_rect *r; int n, cap; int oom; unsigned level, want_layer; };
static void nvk_rect_add(void *u, unsigned level, unsigned layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct nvk_rects *L = u;
    if (level != L->level || layer != L->want_layer) return;
    if (!w || !h) return;
    for (int i = 0; i < L->n; i++)   // 相邻 piece 会产生相邻/相同矩形：去重（只是省重复拷贝）
        if (L->r[i].x == x && L->r[i].y == y && L->r[i].w == w && L->r[i].h == h) return;
    if (L->n == L->cap) {
        int nc = L->cap ? 2 * L->cap : 16;
        struct nvk_rect *nr = realloc(L->r, (size_t)nc * sizeof *nr);
        if (!nr) { L->oom = 1; return; }
        L->r = nr; L->cap = nc;
    }
    L->r[L->n++] = (struct nvk_rect){ x, y, w, h };
}

// 精确补丁：把命中本子资源的声明写范围（hit[]）按 piece 粒度换成矩形并拷贝。
// 返回 0 成功；-1 fail-closed（why 写原因：3D/线性/深度模板/BC 解码/深块/边未按块对齐）。
static int nvk_patch_ranges(struct nvn_vk_texcache *c, struct nvn_vk_tex *o, const tex_t *t,
                            unsigned level, unsigned layer, const struct nvk_range *hit, uint32_t nh,
                            char *why, size_t why_size) {
    if (t->d.target == NVK_TARGET_SIZE_3D) {
        if (why && why_size) snprintf(why, why_size, "3D 纹理 level %u 的局部合并未接通（只允许整级覆盖）", level);
        return -1;
    }
    if (t->d.flags & 0x10) {
        if (why && why_size) snprintf(why, why_size, "线性纹理 level %u layer %u 的局部合并未接通", level, layer);
        return -1;
    }
    int bw = 1, bh = 1; uint32_t bpb = (uint32_t)nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { if (why && why_size) snprintf(why, why_size, "局部合并：NVN format 0x%x 没有块大小", t->d.format); return -1; }
    const VkFormat f = o->vk_format ? o->vk_format : nvn_vk_format(o->nvn_format);
    if (nvn_vk_format_has_stencil(f)) {
        if (why && why_size) snprintf(why, why_size, "深度+模板格式 level %u layer %u 的局部合并未接通（fail-closed，不猜 aspect）", level, layer);
        return -1;
    }
    if (bc_needs_decode(c->d, (uint32_t)t->d.format)) {
        if (why && why_size) snprintf(why, why_size, "BC 解码图像 level %u layer %u 的局部合并未接通（解码是整级 compute）", level, layer);
        return -1;
    }
    unsigned lth = 0, ltd = 0; nvn_tex_level_tiles(&t->d, level, &lth, &ltd);
    if (ltd) { if (why && why_size) snprintf(why, why_size, "深块（tileD）level %u 的局部合并未接通", level); return -1; }
    uint32_t row_bytes = 0, rows = 0, slices = 0, gth = 0, gtd = 0, gbpb = 0; uint64_t base = 0;
    if (nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &gth, &gtd, &gbpb) || !row_bytes || !rows || slices != 1) {
        if (why && why_size) snprintf(why, why_size, "level %u layer %u 布局不支持局部合并", level, layer);
        return -1;
    }
    struct nvk_rects L; memset(&L, 0, sizeof L); L.level = level; L.want_layer = layer;
    for (uint32_t i = 0; i < nh; i++) {
        if (nvk_bytes_rects(t, layer, hit[i].lo, hit[i].hi, nvk_rect_add, &L)) {
            free(L.r);
            if (why && why_size) snprintf(why, why_size,
                "声明写范围 [%llu,%llu) 的边未按块对齐（level %u layer %u）：不能只拷一部分，fail-closed",
                (unsigned long long)hit[i].lo, (unsigned long long)hit[i].hi, level, layer);
            return -1;
        }
    }
    if (L.oom) { free(L.r); if (why && why_size) snprintf(why, why_size, "局部合并矩形表分配失败（level %u layer %u）", level, layer); return -1; }
    if (!L.n) { free(L.r); return 0; }   // 范围内全是填充字节（没有纹素）：图像不用动
    const size_t bytes = (size_t)row_bytes * rows;
    struct s2a_vk_device *d = c->d;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(d->device, &bci, NULL, &buf) != VK_SUCCESS) { free(L.r); if (why && why_size) snprintf(why, why_size, "局部合并 staging buffer 创建失败（%zu B）", bytes); return -1; }
    g_up_staging_created++;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->device, buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = s2a_vk_memory_type(d, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *p = NULL;
    int ok = 0;
    if (mai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(d->device, &mai, NULL, &mem) == VK_SUCCESS &&
        vkBindBufferMemory(d->device, buf, mem, 0) == VK_SUCCESS &&
        vkMapMemory(d->device, mem, 0, bytes, 0, &p) == VK_SUCCESS)
        ok = 1;
    if (!ok) { if (why && why_size) snprintf(why, why_size, "局部合并 staging 分配失败（%zu B）", bytes); goto fail; }
    if (nvn_deswizzle(&t->d, level, layer, t->mem, (uint8_t *)p, row_bytes)) {
        if (why && why_size) snprintf(why, why_size, "局部合并 level %u layer %u deswizzle 失败", level, layer);
        goto fail;
    }
    vkUnmapMemory(d->device, mem);
    p = NULL;   // 已解映射：后面任何 goto fail 都不能再 unmapped 的指针（原实现会 double unmap）
    {
        VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbi.commandPool = d->command_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(d->device, &cbi, &cmd) != VK_SUCCESS) goto fail;
        g_up_cmd_created++;
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        const VkImageSubresourceRange range = { nvk_img_aspect(o), level, 1, layer, 1 };
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = o->image; b.subresourceRange = range;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        int bad = vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS;
        if (!bad) {
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
            for (int i = 0; i < L.n && !bad; i++) {
                VkBufferImageCopy bc = {0};
                bc.bufferOffset = (VkDeviceSize)(((size_t)(L.r[i].y / (uint32_t)bh) * row_bytes) +
                                                ((size_t)(L.r[i].x / (uint32_t)bw) * bpb));
                // Vulkan 要求 bufferOffset 对齐（保守按 4 B）：不能对齐就不猜，整笔 fail-closed。
                if (bc.bufferOffset % 4) {
                    if (why && why_size) snprintf(why, why_size,
                        "局部合并矩形偏移 %llu 未按 4 B 对齐（level %u layer %u）：fail-closed",
                        (unsigned long long)bc.bufferOffset, level, layer);
                    bad = 1;
                    break;
                }
                bc.bufferRowLength = (row_bytes / bpb) * (uint32_t)bw;   // 相邻两行之间的纹素数（块压缩格式必须按块宽计）
                bc.bufferImageHeight = 0;
                bc.imageSubresource = (VkImageSubresourceLayers){ nvk_img_aspect(o), level, layer, 1 };
                bc.imageOffset = (VkOffset3D){ (int32_t)L.r[i].x, (int32_t)L.r[i].y, 0 };
                bc.imageExtent = (VkExtent3D){ L.r[i].w, L.r[i].h, 1 };
                vkCmdCopyBufferToImage(cmd, buf, o->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
            }
            if (!bad) {
                VkImageMemoryBarrier b2 = b;
                b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
            }
            if (vkEndCommandBuffer(cmd) != VK_SUCCESS) bad = 1;   // 不能覆盖前面已置的 bad（否则漏拷却静默提交）
        }
        if (bad) {
            vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
            if (why && why_size) snprintf(why, why_size, "局部合并命令缓冲失败（level %u layer %u）", level, layer);
            goto fail;
        }
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence f = VK_NULL_HANDLE;
        if (vkCreateFence(d->device, &fci, NULL, &f) != VK_SUCCESS) {
            vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
            if (why && why_size) snprintf(why, why_size, "局部合并 fence 创建失败");
            goto fail;
        }
        g_up_fence_created++;
        if (g_up_fault == 2) {
            vkDestroyFence(d->device, f, NULL); g_up_fence_freed++;
            vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
            g_up_unsubmitted++;
            if (why && why_size) snprintf(why, why_size, "局部合并未提交（注入 mode=2）");
            goto fail;
        }
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(d->queue, 1, &si, f) != VK_SUCCESS) {
            vkDestroyFence(d->device, f, NULL); g_up_fence_freed++;
            vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
            g_up_unsubmitted++;
            if (why && why_size) snprintf(why, why_size, "局部合并提交失败");
            goto fail;
        }
        g_up_submitted++;
        VkResult wr = g_up_fault == 1 ? VK_TIMEOUT : vkWaitForFences(d->device, 1, &f, VK_TRUE, 5000000000ull);
        if (wr != VK_SUCCESS) {
            // 提交成功但没有完成证明：staging/CB/fence 转保留账本（图像内容未知，条目随后按失败处理）。
            struct tex_retained *e = tex_retain_slot(d);
            if (e) { e->fence = f; e->cmd = cmd; e->staging = buf; e->staging_mem = mem; }
            g_up_uncompleted++;
            free(L.r);
            if (why && why_size) snprintf(why, why_size, "局部合并没有完成证明（level %u layer %u）", level, layer);
            return -2;
        }
        vkDestroyFence(d->device, f, NULL); g_up_fence_freed++;
        vkFreeCommandBuffers(d->device, d->command_pool, 1, &cmd); g_up_cmd_freed++;
        g_up_completed++;
    }
    vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++;
    vkFreeMemory(d->device, mem, NULL);
    free(L.r);
    return 0;
fail:
    if (p) vkUnmapMemory(d->device, mem);
    if (mem) vkFreeMemory(d->device, mem, NULL);
    if (buf) { vkDestroyBuffer(d->device, buf, NULL); g_up_staging_freed++; }
    free(L.r);
    return -1;
}

static int nvk_any_gpu(const struct nvn_vk_tex *o) {
    if (!o) return 0;
    if (!o->subres) return o->gpu_written ? 1 : 0;   // 版本表缺失：按整图 GPU 标志保守处理
    for (uint32_t i = 0; i < o->subres_n; i++) if (o->subres[i] != NVK_SUBRES_CPU) return 1;
    return 0;
}

// 命中时定位 guest 变更：返回变更 GOB 数（>0 时 chg 有效，调用方负责 nvk_chg_free）。
// 有账本 → 按 GOB 哈希定位；没有账本（超上限/分配失败）→ 整张哈希兜底，命中时把所有 GOB 视为变更，
// 并计 ledger_fail（hint 路径据此按“无法定位”处理：CPU 子资源整块重传，GPU 内容 fail-closed）。
// 注意：哈希只回答“guest 是否变了”，不回答“哪些字节是 CPU 写的”——归属由声明写范围账本决定。
static uint32_t nvk_locate_changes(struct nvn_vk_texcache *c, struct nvn_vk_tex *o, const tex_t *t,
                                   int dirty_hint, int dpages, const uint64_t *dbits,
                                   struct nvk_chg *chg, int *ledger_missing) {
    *ledger_missing = 0;
    if (nvk_ledger_ensure(o, t->L.size) == 0 && o->gobs_ready) return nvk_scan_changed(c, o, t, dpages, dbits, chg);
    int changed = 0;
    if (o->content_tracked) {
        changed = (tex_hash(t->mem, t->L.size) != o->content_hash);
    } else if (dirty_hint) {
        changed = 1;
    }
    if (!changed) return 0;
    *ledger_missing = 1;
    c->ledger_fail++;
    for (uint32_t g = 0; g * NVK_GOB < t->L.size && g < chg->words * 64; g++) nvk_chg_mark(chg, g);
    return chg->count;
}

// 把声明的写范围落到图像。返回 0 成功（把 o->pend 里已成功应用的段标成 sync_r 并由调用方清空 pend）；
// -1 fail-closed（why，不返回旧内容）。整子资源被声明范围覆盖 → 整块重传；否则精确补丁；其余字节保持图像内容。
static int nvk_apply_pending(struct nvn_vk_texcache *c, struct nvn_vk_tex *o, const tex_t *t,
                             char *why, size_t why_size) {
    if (!o->n_pend) return 0;
    nvn_texlayout L; nvn_tex_layout(&t->d, &L);
    const uint32_t levels = nvk_levels_of(o), layers = nvk_layers_of(o);
    const uint64_t layer_size = L.layer_size ? L.layer_size : L.size;
    const int is3d = t->d.target == NVK_TARGET_SIZE_3D;
    if (o->pend_truncated) {
        // 范围账本溢出：不知道哪些字节被声明过。有 GPU 内容就不能猜；全 CPU 时整张从 guest 重建安全。
        if (nvk_any_gpu(o)) {
            c->range_truncated++;
            if (why && why_size) snprintf(why, why_size,
                "texture serial=%llu 的声明写范围账本溢出（>%d 段）：无法确定 CPU 改写了哪些字节，"
                "且图像含 GPU 内容（fail-closed，不返回旧内容）", (unsigned long long)t->serial, NVK_RANGES_MAX);
            return -1;
        }
        for (uint32_t l = 0; l < levels; l++) {
            const uint64_t lb0 = nvn_tex_level_offset(&t->d, l);
            const uint64_t lb1 = (l + 1 < levels) ? nvn_tex_level_offset(&t->d, l + 1) : layer_size;
            const uint32_t nl = is3d ? 1 : layers;
            for (uint32_t a = 0; a < nl; a++) {
                const uint64_t sbase = (uint64_t)a * layer_size + lb0, send = (uint64_t)a * layer_size + lb1;
                if (nvk_upload_subres(c, o, t, l, a, why, why_size)) return -1;
                nvk_subres_set(o, l, a, NVK_SUBRES_CPU);
                nvk_ledger_set(o, t->mem, sbase, send);
                nvk_range_add(o, &o->sync_r, &o->n_sync, &o->cap_sync, &o->sync_truncated, sbase, send);
                c->full_covers++;
            }
        }
        nvk_recompute_gpu_flag(o);
        return 0;
    }
    for (uint32_t l = 0; l < levels; l++) {
        const uint64_t lb0 = nvn_tex_level_offset(&t->d, l);
        const uint64_t lb1 = (l + 1 < levels) ? nvn_tex_level_offset(&t->d, l + 1) : layer_size;
        const uint32_t nl = is3d ? 1 : layers;
        for (uint32_t a = 0; a < nl; a++) {
            const uint64_t sbase = (uint64_t)a * layer_size + lb0, send = (uint64_t)a * layer_size + lb1;
            struct nvk_range hit[NVK_RANGES_MAX]; uint32_t nh = 0;
            for (uint32_t i = 0; i < o->n_pend && nh < NVK_RANGES_MAX; i++) {
                uint64_t lo = o->pend[i].lo > sbase ? o->pend[i].lo : sbase;
                uint64_t hi = o->pend[i].hi < send ? o->pend[i].hi : send;
                if (hi > lo) hit[nh++] = (struct nvk_range){ lo, hi };
            }
            if (!nh) continue;
            uint64_t cov = 0;
            for (uint32_t i = 0; i < nh; i++) cov += hit[i].hi - hit[i].lo;   // pend 已合并，不重叠
            // 已同步 && guest 未变（哈希证明）→ 跳过上传。哈希只在这里减少上传，不决定归属。
            int skip = 0;
            if (!o->sync_truncated && nvk_hits_covered(o->sync_r, o->n_sync, hit, nh)) {
                int unchanged = 1;
                if (o->gobs_ready) {
                    for (uint32_t i = 0; i < nh && unchanged; i++)
                        for (uint64_t g = hit[i].lo / NVK_GOB; g * NVK_GOB < hit[i].hi && g < o->ngobs; g++) {
                            uint64_t ga, gn; nvk_gob_span(t, (uint32_t)g, &ga, &gn);
                            if (!gn) continue;
                            if (nvk_hash_bytes(t->mem + ga, (size_t)gn) != o->gobs[g]) { unchanged = 0; break; }
                        }
                } else unchanged = 0;
                if (unchanged) { c->range_skips++; c->ranges_applied++; skip = 1; }
            }
            if (skip) continue;
            // 原本就是 CPU（图像==guest）：整块重传是安全的（未声明但已变了的字节也只是 CPU 内容），
            // 且避免在 BC 解码/深度模板/3D 等不支持局部合并的格式上退化成 fail-closed。
            const int cpu_before = !o->sync_truncated && nvk_ranges_covered(o->sync_r, o->n_sync, sbase, send);
            if (cpu_before || cov >= send - sbase) {
                // 整个子资源都是 CPU 内容：整块重传（GOB 只是上传粒度，这里由声明范围证明归属）。
                if (nvk_upload_subres(c, o, t, l, a, why, why_size)) return -1;
                nvk_subres_set(o, l, a, NVK_SUBRES_CPU);
                nvk_ledger_set(o, t->mem, sbase, send);
                nvk_range_add(o, &o->sync_r, &o->n_sync, &o->cap_sync, &o->sync_truncated, sbase, send);
                c->full_covers++;
                c->ranges_applied++;
                continue;
            }
            int rc = nvk_patch_ranges(c, o, t, l, a, hit, nh, why, why_size);
            if (rc) return -1;
            for (uint32_t i = 0; i < nh; i++) {
                nvk_range_add(o, &o->sync_r, &o->n_sync, &o->cap_sync, &o->sync_truncated, hit[i].lo, hit[i].hi);
                for (uint64_t g = hit[i].lo / NVK_GOB; g * NVK_GOB < hit[i].hi && g < o->ngobs; g++) {
                    uint64_t ga, gn; nvk_gob_span(t, (uint32_t)g, &ga, &gn);
                    if (gn) nvk_ledger_set(o, t->mem, ga, ga + gn);
                }
            }
            const int cpu = !o->sync_truncated && nvk_ranges_covered(o->sync_r, o->n_sync, sbase, send);
            nvk_subres_set(o, l, a, cpu ? NVK_SUBRES_CPU : NVK_SUBRES_MIXED);
            c->patches++;
            if (!cpu) c->mixed++;
            c->ranges_applied++;
        }
    }
    nvk_recompute_gpu_flag(o);
    return 0;
}

// hint（没有声明范围的通知）路径：哈希只用来发现“guest 变了”。变化 GOB 若已被本次声明范围解释
// （相交）就不再追究；解释不了的：全 CPU 子资源 → 整块重传（图像==guest，安全）；含 GPU 内容 →
// fail-closed（无法证明归属，绝不返回旧内容）。
static int nvk_sync_hint(struct nvn_vk_texcache *c, struct nvn_vk_tex *o, const tex_t *t,
                         const struct nvk_chg *chg, const struct nvk_range *applied, uint32_t napplied,
                         int ledger_missing, char *why, size_t why_size) {
    nvn_texlayout L; nvn_tex_layout(&t->d, &L);
    const uint32_t levels = nvk_levels_of(o), layers = nvk_layers_of(o);
    const uint64_t layer_size = L.layer_size ? L.layer_size : L.size;
    const int is3d = t->d.target == NVK_TARGET_SIZE_3D;
    for (uint32_t l = 0; l < levels; l++) {
        const uint64_t lb0 = nvn_tex_level_offset(&t->d, l);
        const uint64_t lb1 = (l + 1 < levels) ? nvn_tex_level_offset(&t->d, l + 1) : layer_size;
        const uint32_t nl = is3d ? 1 : layers;
        for (uint32_t a = 0; a < nl; a++) {
            const uint64_t sbase = (uint64_t)a * layer_size + lb0, send = (uint64_t)a * layer_size + lb1;
            int unexplained = 0;
            if (ledger_missing) {
                unexplained = 1;   // 没有账本：hint 说“可能变了”，但无法定位 → 按未解释处理
            } else {
                for (uint64_t g = sbase / NVK_GOB; g * NVK_GOB < send && g < o->ngobs; g++) {
                    if (!nvk_chg_test(chg, (uint32_t)g)) continue;
                    uint64_t ga, gn; nvk_gob_span(t, (uint32_t)g, &ga, &gn);
                    if (!gn) continue;
                    if (nvk_ranges_hit(applied, napplied, ga, ga + gn)) continue;   // 被声明范围解释
                    unexplained = 1;
                    break;
                }
            }
            if (!unexplained) continue;
            if (nvk_subres_gpu_any(o, l, a)) {
                c->hint_unexplained++;
                if (why && why_size) snprintf(why, why_size,
                    "texture serial=%llu 子资源 level=%u layer=%u 有未被 CPU 声明写范围解释的 guest 变化，"
                    "且图像含 GPU 内容：无法证明内容归属（fail-closed，不返回旧内容；调用点缺 WriteTexels/FlushMappedRange 范围?）",
                    (unsigned long long)t->serial, l, a);
                return -1;
            }
            // 全 CPU 子资源：上传前图像==guest，整块重传不会丢 GPU 内容。
            if (nvk_upload_subres(c, o, t, l, a, why, why_size)) return -1;
            nvk_subres_set(o, l, a, NVK_SUBRES_CPU);
            nvk_ledger_set(o, t->mem, sbase, send);
            nvk_range_add(o, &o->sync_r, &o->n_sync, &o->cap_sync, &o->sync_truncated, sbase, send);
            c->full_covers++;
        }
    }
    nvk_recompute_gpu_flag(o);
    return 0;
}

// 版本化重传（在途旧版本）能否“整张从 guest 上传”：全 CPU 内容，或声明范围覆盖整张纹理。
// 注意：sync_r 覆盖整张不构成证明——它只说明“当时图像==guest”，guest 之后可能被写过。
static int nvk_guest_whole(const struct nvn_vk_tex *o, const tex_t *t) {
    if (!nvk_any_gpu(o)) return 1;
    if (!o->n_pend || o->pend_truncated) return 0;
    nvn_texlayout L; nvn_tex_layout(&t->d, &L);
    const uint32_t levels = nvk_levels_of(o), layers = nvk_layers_of(o);
    const uint64_t layer_size = L.layer_size ? L.layer_size : L.size;
    const int is3d = t->d.target == NVK_TARGET_SIZE_3D;
    for (uint32_t l = 0; l < levels; l++) {
        const uint64_t lb0 = nvn_tex_level_offset(&t->d, l);
        const uint64_t lb1 = (l + 1 < levels) ? nvn_tex_level_offset(&t->d, l + 1) : layer_size;
        const uint32_t nl = is3d ? 1 : layers;
        for (uint32_t a = 0; a < nl; a++) {
            const uint64_t sbase = (uint64_t)a * layer_size + lb0, send = (uint64_t)a * layer_size + lb1;
            if (!nvk_ranges_covered(o->pend, o->n_pend, sbase, send)) return 0;
        }
    }
    return 1;
}

// ---- 内容归属账本：公开注册入口（调用点给精确范围）----
// 精确矩形 → 存储字节范围（16 B piece 粒度）。返回 0；-1 几何不支持（调用方退化为 hint）。
static int nvk_rect_ranges(struct nvn_vk_tex *o, const tex_t *t, unsigned level, unsigned layer_or_slice,
                           int is3d, int x, int y, int w, int h) {
    uint32_t row_bytes = 0, rows = 0, slices = 0, th = 0, td = 0, bpb = 0; uint64_t base = 0;
    int bw = 1, bh = 1;
    if (nvn_deswizzle_geom(&t->d, level, is3d ? 0u : layer_or_slice, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (!bpb || (64 % bpb) || !row_bytes || !rows) return -1;
    nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t lw = mipdim(t->d.width, level), lh = mipdim(t->d.height, level);
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return 0;
    if ((uint32_t)x >= lw || (uint32_t)y >= lh) return 0;
    if ((uint32_t)x + (uint32_t)w > lw) w = (int)(lw - (uint32_t)x);
    if ((uint32_t)y + (uint32_t)h > lh) h = (int)(lh - (uint32_t)y);
    uint64_t xb0 = (uint64_t)(x / bw) * (uint64_t)bpb, xb1 = (uint64_t)((x + w + bw - 1) / bw) * (uint64_t)bpb;
    uint64_t yb0 = (uint64_t)(y / bh), yb1 = (uint64_t)((y + h + bh - 1) / bh);
    if (xb1 > row_bytes) xb1 = row_bytes;
    if (yb1 > rows) yb1 = rows;
    if (xb1 <= xb0 || yb1 <= yb0) return 0;
    uint64_t lb = (uint64_t)nvn_tex_level_gobs(&t->d, level) * 64; if (lb < row_bytes) lb = row_bytes;
    uint32_t z = is3d ? layer_or_slice : 0;
    (void)slices; (void)td;
    for (uint64_t yb = yb0; yb < yb1; yb++) {
        for (uint64_t ps = xb0 / 16 * 16; ps < xb1; ps += 16) {
            uint64_t s0 = ps > xb0 ? ps : xb0, s1 = (ps + 16) < xb1 ? (ps + 16) : xb1;
            if (s1 <= s0) continue;
            uint64_t off = base + nvn_bl_offset((uint32_t)ps, (uint32_t)yb, z, (uint32_t)lb, (uint32_t)rows, th, td) + (s0 - ps);
            nvk_range_add(o, &o->pend, &o->n_pend, &o->cap_pend, &o->pend_truncated, off, off + (s1 - s0));
        }
    }
    return 0;
}

void nvn_vk_texcache_mark_cpu_written_range(struct nvn_vk_texcache *c, const void *addr, uint64_t len) {
    if (!c || !addr || !len) return;
    const uint64_t a = (uint64_t)(uintptr_t)addr, e = a + len;
    c->ranges_published++;
    for (int i = 0; i < c->nslots; i++) {
        struct nvn_vk_tex *s = c->slots[i];
        if (!s || !s->valid || s->retired || !s->key || !s->dsize) continue;
        const uint64_t kb = s->key, ke = s->key + s->dsize;
        if (a >= ke || e <= kb) continue;
        const uint64_t lo = a > kb ? a - kb : 0, hi = e < ke ? e - kb : s->dsize;
        nvk_range_add(s, &s->pend, &s->n_pend, &s->cap_pend, &s->pend_truncated, lo, hi);
    }
}

void nvn_vk_texcache_mark_written_rect(struct nvn_vk_texcache *c, const tex_t *t, unsigned level,
                                       unsigned layer0, int x, int y, int w, int h, int z0, int nz) {
    if (!c || !t || !t->mem || !t->serial || w <= 0 || h <= 0 || nz <= 0) return;
    struct nvn_vk_tex *o = tex_find_serial(c, t->serial, NULL);
    if (!o || !o->valid || o->retired) return;   // 还没有缓存条目：首次解析会整张从 guest 上传
    const int is3d = t->d.target == NVK_TARGET_SIZE_3D;
    const unsigned levels = t->d.levels ? t->d.levels : 1;
    if (level >= levels) return;
    uint32_t lw = mipdim(t->d.width, level), lh = mipdim(t->d.height, level);
    const int full_rect = x <= 0 && y <= 0 && w >= (int)lw && h >= (int)lh;
    nvn_texlayout L; nvn_tex_layout(&t->d, &L);
    uint64_t layer_size = L.layer_size ? L.layer_size : L.size;
    uint64_t lb0 = nvn_tex_level_offset(&t->d, level);
    uint64_t lb1 = (level + 1 < levels) ? nvn_tex_level_offset(&t->d, level + 1) : layer_size;
    c->ranges_published++;
    // 整级覆盖：直接记 level 的存储范围（含行尾填充；填充没有纹素，覆盖它无害），避免 piece 数超上限。
    if (full_rect && (is3d ? (nz >= (int)mipdim(t->d.depth, level) && z0 <= 0) : 1)) {
        if (is3d) {
            nvk_range_add(o, &o->pend, &o->n_pend, &o->cap_pend, &o->pend_truncated, lb0, lb1);
            return;
        }
        for (int k = 0; k < nz; k++) {
            unsigned layer = layer0 + (unsigned)(z0 + k);
            if (layer >= nvk_layers_of(o)) break;
            uint64_t base = (uint64_t)layer * layer_size;
            nvk_range_add(o, &o->pend, &o->n_pend, &o->cap_pend, &o->pend_truncated, base + lb0, base + lb1);
        }
        return;
    }
    for (int k = 0; k < nz; k++) {
        if (is3d) {
            if (!nvk_rect_ranges(o, t, level, (unsigned)(z0 + k), 1, x, y, w, h)) continue;
        } else {
            unsigned layer = layer0 + (unsigned)(z0 + k);
            if (layer >= nvk_layers_of(o)) break;
            if (!nvk_rect_ranges(o, t, level, layer, 0, x, y, w, h)) continue;
        }
    }
}

// 块编码影子（压缩纹理经“块=像素”视图渲染）把字节拷回底图后：该 (mip, 层/深度切片) 子资源
// 的内容由 GPU 决定，CPU 镜像不再可信。整张的内容哈希也不能再代表 GPU 内容（置 0 同时
// 关掉按 hash 的复查），否则下一笔命中会拿 guest 旧字节覆盖刚渲染的结果。
void nvn_vk_texcache_mark_gpu_written_subres(struct nvn_vk_texcache *c, const tex_t *t, unsigned level, unsigned layer) {
    if (!c || !t || !t->mem || !t->serial) return;
    struct nvn_vk_tex *o = tex_find_serial(c, t->serial, NULL);
    if (!o || !o->valid || o->retired) return;
    const unsigned levels = t->d.levels ? t->d.levels : 1;
    if (level >= levels) return;
    if (layer >= nvk_layers_of(o)) return;
    if (nvk_subres_ensure(o) == 0) nvk_subres_set(o, level, layer, NVK_SUBRES_GPU);
    o->gpu_written = 1;
    o->content_dynamic = 1;
    o->content_tracked = 0;
    o->content_hash = 0;
    g_bcrt_gpu_marks++;
}

// debug.s2a.tex_check_ms：把非动态纹理的复查间隔压到指定毫秒（验证用，默认 0 = 不覆盖）。
static int tex_check_ms_override(void) {
    static int v = -1;
    if (v >= 0) return v;
    char b[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.tex_check_ms", b);
    v = b[0] ? atoi(b) : 0;
    return v > 0 ? v : 0;
}

// 内容交接查询（只读）：读回/绘制据此选 guest 还是图像。
int nvn_vk_texcache_subres_state(const struct nvn_vk_tex *t, unsigned aspect, unsigned level, unsigned layer) {
    return nvk_subres_get(t, aspect, level, layer);
}
const char *nvn_vk_texcache_subres_state_name(int state) {
    switch (state) {
    case NVK_SUBRES_CPU: return "cpu";
    case NVK_SUBRES_GPU: return "gpu";
    case NVK_SUBRES_MIXED: return "mixed";
    default: return "?";
    }
}
int nvn_vk_texcache_region_cpu(const struct nvn_vk_tex *t, unsigned level, unsigned layer0, unsigned nlayers) {
    if (!t) return 1;   // 没有缓存条目 = 从没被 GPU 用过，guest 就是全部内容
    if (!nlayers) nlayers = 1;
    for (unsigned a = 0; a < nlayers; a++)
        if (nvk_subres_gpu_any(t, level, layer0 + a)) return 0;
    return 1;
}
void nvn_vk_texcache_content_stats(struct nvn_vk_texcache *c, struct nvn_vk_texcache_content_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!c) return;
    out->checks = c->content_checks; out->gobs_scanned = c->gobs_scanned; out->gobs_changed = c->gobs_changed;
    out->patches = c->patches; out->full_covers = c->full_covers; out->ledger_fail = c->ledger_fail;
    out->gpu_marks = c->gpu_marks; out->mixed = c->mixed;
    out->mixed_versioned_rejects = c->mixed_versioned_rejects;
    out->ledger_bytes = c->ledger_bytes;
    out->ranges_published = c->ranges_published; out->ranges_applied = c->ranges_applied;
    out->range_skips = c->range_skips; out->hint_unexplained = c->hint_unexplained;
    out->range_truncated = c->range_truncated;
}

struct nvn_vk_tex *nvn_vk_texcache_get_tex(struct nvn_vk_texcache *c, const tex_t *t, char *why, size_t why_size) {
    if (!t || !t->mem) { if (why && why_size) snprintf(why, why_size, "texture has no CPU storage"); return NULL; }
    uint32_t levels = t->d.levels ? t->d.levels : 1;
    const uint32_t target = (uint32_t)t->d.target;
    if (target == NVK_TARGET_SIZE_2D_MS || target == NVK_TARGET_SIZE_2D_MS_ARRAY) {
        if (why && why_size) snprintf(why, why_size,
            "multisampled texture target %u (samples=%d) is not bridged yet: no sample-count/resolve path",
            target, t->d.samples);
        return NULL;
    }
    if (target == NVK_TARGET_SIZE_BUFFER) {
        if (why && why_size) snprintf(why, why_size, "buffer texture target %u is not bridged yet (needs a texel buffer view)", target);
        return NULL;
    }
    if (target > NVK_TARGET_SIZE_BUFFER) {
        if (why && why_size) snprintf(why, why_size, "unknown NVN texture target %u", target);
        return NULL;
    }
    if (t->d.samples > 1) {
        // 单采样目标的 samples 只能是 0/1；大于 1 而目标又不是 5/6 时不能按单采样建图（会丢采样语义）。
        if (why && why_size) snprintf(why, why_size,
            "texture target %u declares samples=%d; multisample storage needs target 5/6 and is not bridged",
            target, t->d.samples);
        return NULL;
    }
    const int cube = target == NVK_TARGET_SIZE_CUBE;
    const int cube_array = target == NVK_TARGET_SIZE_CUBE_ARRAY;
    uint32_t depth = target == NVK_TARGET_SIZE_3D ? (t->d.depth ? t->d.depth : 1) : 1;
    nvn_texlayout L; nvn_tex_layout(&t->d, &L);
    uint32_t layers = target == NVK_TARGET_SIZE_3D ? 1 : (L.layers ? L.layers : 1);
    if (cube && layers != 6) {
        if (why && why_size) snprintf(why, why_size, "cubemap declares %u layers; NVN cubemaps always have 6 faces", layers);
        return NULL;
    }
    if (cube_array && (layers == 0 || layers % 6)) {
        if (why && why_size) snprintf(why, why_size,
            "cubemap array declares %u layer-faces; a cube array's depth must be a multiple of 6", layers);
        return NULL;
    }
    struct nvn_vk_tex *o = NULL;
    // 活对象主表按 serial 定位（对象身份）；地址只用于上传数据与别名识别。
    if (t->serial) o = tex_find_serial(c, t->serial, NULL);
    // 别名缺口（诊断）：另一张活对象也指着这块存储、serial 不同。两者现在各持一张图像，
    // 通过其中一个写的 GPU 内容不会出现在另一个里；接管/共享语义尚未实现，先如实计数与报一次。
    if (!o) {
        for (int i = 0; i < c->nslots; i++) {
            struct nvn_vk_tex *s = c->slots[i];
            if (!s || !s->valid || s->retired) continue;
            if (s->key != (uint64_t)(uintptr_t)t->mem) continue;
            c->alias_shared++;
            static int n;
            int compat = s->nvn_format == (uint32_t)t->d.format && s->width == t->d.width && s->height == t->d.height &&
                         s->layers == layers && s->levels == levels && s->depth == depth && s->nvn_target == target;
            if (compat) {
                // 同一 backing、同格式/尺寸/层级：共享同一张图像（内容互通），新 serial 进引用列表。
                tex_add_serial(s, t->serial);
                if (n++ < 30)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                        "tex alias: addr=%p share serial=%llu <-> serial=%llu (%ux%u fmt=0x%x tgt=%u)",
                        (void *)t->mem, (unsigned long long)s->serial, (unsigned long long)t->serial,
                        s->width, s->height, s->nvn_format, s->nvn_target);
                o = s;
            } else if (n++ < 30) {
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                    "tex alias: addr=%p incompatible serial=%llu(%ux%u fmt=0x%x tgt=%u) vs serial=%llu(%ux%u fmt=0x%x tgt=%u); separate images",
                    (void *)t->mem, (unsigned long long)s->serial, s->width, s->height, s->nvn_format, s->nvn_target,
                    (unsigned long long)t->serial, t->d.width, t->d.height, (unsigned)t->d.format, target);
            }
            break;
        }
    }
    if (o && (o->serial == t->serial || tex_has_serial(o, t->serial)) && o->nvn_format == (uint32_t)t->d.format && o->width == t->d.width &&
        o->height == t->d.height && o->levels == levels && o->layers == layers && o->depth == depth &&
        o->nvn_target == target &&
        o->set_swz == t->set_swz && o->swz == (uint16_t)t->swz) {
        // ---- 内容归属：GPU 写登记 / CPU 声明写范围 / 哈希基线三份事实分开 ----
        // 声明写范围（FlushMappedRange / WriteTexels 调用点给的精确范围）是内容归属的权威；
        // 哈希只用来 (a) 跳过“已同步且没变”的上传，(b) 发现没有声明范围解释的 guest 变化。
        int content_due = 0, dirty_force = 0, timer_due = 0, force_due = 0, dirty_defer = 0, force_full = 0, ledger_missing = 0;
        uint64_t sync_stamp = 0;
        static __thread int g_tex_reenter;   // 版本化重传：退役旧条目后递归走一次创建路径
        struct nvk_chg chg; memset(&chg, 0, sizeof chg);
        uint32_t nchanged = 0;
        const int have_pend = o->n_pend > 0;
        if (o->dirty_armed || o->content_tracked || o->content_force || have_pend) {
            const int64_t now_ms = (int64_t)tex_now_ms();
            if (o->dirty_armed) {
                c->dirty_checks++;
                // 映射代际变了 = 跟踪被重建（AHB 换底）：旧的“已保护/没变”判断全部作废，强制全量重扫。
                if (o->dmap_gen != nvn_dirty_mapping_generation()) {
                    o->dmap_gen = nvn_dirty_mapping_generation();
                    dirty_force = 1; force_full = 1;
                } else if (nvn_dirty_since_span(o->dspan, t->mem, o->dsize, o->dgen)) {
                    dirty_force = 1; c->dirty_hits++;
                }
            }
            if (o->content_tracked) {
                // 定时复查（与原行为一致）：动态 16 ms、其余 1 s；新建头 2 s 用 16 ms。
                uint64_t interval = o->content_dynamic ? 16 : (tex_check_ms_override() > 0 ? (uint64_t)tex_check_ms_override() : 1000);
                if (o->content_created_ms && now_ms - (int64_t)o->content_created_ms < 2000) interval = 16;
                if (now_ms - (int64_t)o->content_check_ms >= (int64_t)interval) timer_due = 1;
            }
            if (o->content_force) force_due = 1;   // CPU 写通知（无范围）：纯 hint，不构成归属证明
            o->content_force = 0;
            const int hint_due = dirty_force || timer_due || force_due;
            content_due = hint_due || have_pend;   // 声明范围优先：不等脏页代数/定时器
            if (content_due) {
                o->content_check_ms = (uint64_t)now_ms;
                // 扫描前取代数：本次扫描/同步期间的新写代数更大，留给下一次检查（与原上传路径同一口径）。
                const uint64_t scan_stamp = nvn_dirty_now();
                uint64_t dbits[256] = {0};   // 16K 页 × 64 = 1M 页；超过则按“全都可能脏”处理
                int dpages = -1;
                if (o->dirty_armed && o->dsize && dirty_force && !force_full)
                    dpages = (int)nvn_dirty_written_pages(o->dspan, t->mem, o->dsize, o->dgen, dbits, 256 * 64);
                else if (!dirty_force && o->dirty_armed && o->dsize) dpages = 0;   // 定时/通知触发：全量重扫 GOB
                const uint32_t want = o->ngobs ? o->ngobs : (uint32_t)((t->L.size + NVK_GOB - 1) / NVK_GOB);
                // 先按哈希定位“guest 变了什么”（仅 hint 触发时需要；声明范围不靠哈希证明归属）。
                if (hint_due && nvk_chg_init(&chg, want) == 0) {
                    nchanged = nvk_locate_changes(c, o, t, dirty_force, dpages, dbits, &chg, &ledger_missing);
                    if (nchanged) { c->content_checks++; c->gobs_changed += nchanged; }
                }
                const int pinned = (o->pins || tex_has_pinned_views(o)) && !g_tex_reenter;
                const int need = have_pend || nchanged;
                if (need && pinned) {
                    // 旧 draw 仍引用旧图像：只能“整张来自 guest”的版本化重传；否则 fail-closed。
                    if (!nvk_guest_whole(o, t)) {
                        c->mixed_versioned_rejects++;
                        if (why && why_size) snprintf(why, why_size,
                            "texture serial=%llu is in flight and only part of its content is CPU-authored "
                            "(pending ranges cover %u of the texture): versioned re-upload needs a whole-texture cover; "
                            "partial merge into a new image is not bridged (frame-queue completion proof: batch B)",
                            (unsigned long long)t->serial, o->n_pend);
                        nvk_chg_free(&chg);
                        return NULL;
                    }
                    int outstanding = 0;
                    for (int vi = 0; vi < c->nslots; vi++) {
                        struct nvn_vk_tex *vs = c->slots[vi];
                        if (vs && vs->valid && vs->retired && vs->key == o->key) outstanding++;
                    }
                    uint64_t vbytes = (uint64_t)t->L.size;
                    if (outstanding >= 8 || c->retired_bytes + vbytes > (64ull << 20)) {
                        // 背压：不就地重传（会覆盖仍被已录制/在途命令引用的图像）。本帧返回旧版本、
                        // 声明范围保留，下次引用释放后自动重试（预算策略归批 B，本批不改）。
                        c->version_full++;
                        dirty_defer = 1;
                    } else {
                        nvk_chg_free(&chg);
                        nvn_vk_texcache_drop_tex(c, t);
                        g_tex_reenter = 1;
                        struct nvn_vk_tex *fresh = nvn_vk_texcache_get_tex(c, t, why, why_size);
                        g_tex_reenter = 0;
                        c->dirty_versioned++;
                        return fresh;
                    }
                } else if (need) {
                    // 就地同步：先应用声明写范围（权威），再用哈希查“没被声明范围解释的 guest 变化”。
                    struct nvk_range applied[NVK_RANGES_MAX]; uint32_t napplied = 0;
                    if (have_pend) {
                        napplied = o->n_pend; if (napplied > NVK_RANGES_MAX) napplied = NVK_RANGES_MAX;
                        memcpy(applied, o->pend, (size_t)napplied * sizeof applied[0]);
                        if (nvk_apply_pending(c, o, t, why, why_size)) {
                            nvk_chg_free(&chg);
                            return NULL;   // fail-closed：声明范围无法安全落地，pending 保留供重试
                        }
                        o->n_pend = 0;     // 已全部处理（成功才消费）
                        o->pend_truncated = 0;
                        sync_stamp = nvn_dirty_now();
                        o->content_dynamic = 1;
                        {   // 诊断：debug.s2a.log_tex=1 时有界打印
                            static int logv = -1;
                            if (logv < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.log_tex", b); logv = (b[0] && atoi(b)) ? 1 : 0; }
                            if (logv) {
                                static int n;
                                if (n++ < 400)
                                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                        "tex content apply addr=%p serial=%llu %ux%u fmt=0x%x ranges=%u changed=%u size=%zu",
                                        (void *)t->mem, (unsigned long long)t->serial, t->d.width, t->d.height,
                                        t->d.format, napplied, nchanged, (size_t)t->L.size);
                            }
                        }
                    }
                    if (nchanged) {
                        if (nvk_sync_hint(c, o, t, &chg, applied, napplied, ledger_missing, why, why_size)) {
                            nvk_chg_free(&chg);
                            return NULL;   // hint 发现未解释变化且含 GPU 内容：fail-closed
                        }
                        o->content_dynamic = 1;
                        sync_stamp = nvn_dirty_now();
                    }
                    if (ledger_missing) o->content_hash = tex_hash(t->mem, t->L.size);
                    else if (o->content_tracked) o->content_hash = 0;   // 账本已接管变更判定
                    if (o->dirty_armed) {
                        o->dgen = sync_stamp;
                        nvn_dirty_protect((const void *)t->mem, o->dsize);
                        c->reupload_bytes += o->dsize;
                    }
                } else if (o->dirty_armed && o->dsize) {
                    // guest 真的没变（1 MiB 块脏是别的数据写出来的）：推进代数并重新加保护，
                    // 否则这块会一直“脏”，每帧都要重扫整张纹理。
                    o->dgen = scan_stamp;
                    nvn_dirty_protect((const void *)t->mem, o->dsize);
                }
                nvk_chg_free(&chg);
            }
        }
        if (dirty_defer) {
            // 旧版本还在途且额度已满：这一帧不更新内容（旧 draw 看旧版本），声明范围保留，下次再试。
            o->last_use = ++c->clock;
            return o;
        }
        o->last_use = ++c->clock;
        return o;
    }
    if (o) o->last_use = ++c->clock;
    if (!o) o = slot_new(c);
    if (!o) { if (why && why_size) snprintf(why, why_size, "texture allocation failed"); return NULL; }
    // 反例 F5：同址（同一 backing）来了一条承载新内容的新描述时，只能"退役 + 新建"，
    // 绝不能就地重建。旧条目仍有 pin / 在途视图引用时，release() 会直接返回，但下面
    // 的 vkCreateImage(&o->image) 仍会写进旧条目——旧 draw 看到的图像就被换掉了，
    // 而且新建失败时还会留下一个被改写过的旧对象。这里先入退役账本（等最后一次 unpin
    // 才真正回收），再另开一条承载新内容。
    if (o->valid && (o->pins || tex_has_pinned_views(o))) {
        slot_retire(c, o);
        o = slot_new(c);
        if (!o) { if (why && why_size) snprintf(why, why_size, "texture allocation failed (retired a live entry)"); return NULL; }
    }
    if (o->valid) { release(c->d, o); c->reclaimed++; }   // 同址新描述/尺寸格式变了：重建
    // 3D 的视图层号是深度切片：一个纹理最多会有 depth 个不同切片视图同时在途，
    // 先按 depth+1 留槽（全在途时仍可按需增长），2D 保持默认 8 槽。
    if (tex_views_reserve(o, depth > 1 ? depth + 1 : NVK_TEX_VIEWS)) {
        if (why && why_size) snprintf(why, why_size, "texture view table allocation failed");
        return NULL;
    }
    // 同 get_tex：直连缓存保持存储映射，上传展开只在 nvn_gpu_tex_create 路径做。
    VkFormat fmt = nvn_vk_format((uint32_t)t->d.format);
    if (fmt == VK_FORMAT_UNDEFINED) { if (why && why_size) snprintf(why, why_size, "unknown NVN format 0x%x", t->d.format); return NULL; }
    // #1199：走 compute 解码的 BC 格式，物理图像建为解码后的未压缩格式（采样/视图/复制
    // 全程按未压缩处理），上传时经 nvn_vk_bcdec 写入。3D BC 无 2D 切片视图可用 → 报红。
    // sRGB 解码格式物理图建 UNORM（sRGB+STORAGE 移动端普遍不支持），SRGB 只放采样视图。
    VkFormat img_fmt = fmt;
    const int bc_dec = bc_needs_decode(c->d, (uint32_t)t->d.format);
    if (bc_dec && depth > 1) { if (why && why_size) snprintf(why, why_size, "BC compute decode: 3D textures not bridged"); return NULL; }
    if (bc_dec) {
        VkFormat dfmt = VK_FORMAT_UNDEFINED;
        if (!nvn_vk_bcdec_route((uint32_t)t->d.format, NULL, &dfmt, &img_fmt, NULL, NULL) || dfmt == VK_FORMAT_UNDEFINED) {
            if (why && why_size) snprintf(why, why_size, "BC format 0x%x has no decode route", t->d.format); return NULL;
        }
        fmt = dfmt;
    }
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = img_fmt; ici.extent = (VkExtent3D){t->d.width, t->d.height, depth};
    ici.mipLevels = levels; ici.arrayLayers = layers; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    // 深度/模板格式必须按 DEPTH_STENCIL_ATTACHMENT 建；用 COLOR_ATTACHMENT 建再当深度附件用会让 GPU 直接丢设备。
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                (bc_dec ? VK_IMAGE_USAGE_STORAGE_BIT : 0) |
                tex_attach_usage(c->d, img_fmt);
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // 附件格式覆盖（NVNtextureView::SetFormat）要求图像创建时带 MUTABLE_FORMAT；只对能通过
    // vk_override_class 的 8 位颜色格式开启。合法性仍在建视图时按兼容类拒绝，不默认任意互换。
    {
        uint32_t ob = 0; int oc = 0;
        if (nvn_vk_format_override_class(fmt, &ob, &oc)) ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        // 解码路径的存储视图与图像格式不同类（s3tc 写 UNORM 视图、bc6/7 写 UINT 视图）→ 必须 MUTABLE。
        if (bc_dec) ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    // 立方体贴图必须带 CUBE_COMPATIBLE 才能建 CUBE / CUBE_ARRAY 视图；没有它只能建 2D_ARRAY 视图，
    // 而 samplerCube 采样要求 CUBE 视图（类型不匹配的采样是未定义读取，常见表现就是整片黑色）。
    if (cube || cube_array) ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // 3D 纹理的 NVN 视图层号是深度切片：带兼容标志后可以建 2D/2D_ARRAY 单切片视图
    // （framebuffer 附件必需）。设备不支持时仍按 3D 建图，切片视图请求处准确报红。
    if (depth > 1 && c->d->has_2d_array_compatible) ici.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    if (vkCreateImage(c->d->device, &ici, NULL, &o->image) != VK_SUCCESS) { free(o->views); memset(o, 0, sizeof *o); if (why && why_size) snprintf(why, why_size, "vkCreateImage failed"); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(c->d->device, o->image, &mr);
    const int dedicated = s2a_tex_needs_dedicated(c->d, o->image, &mr);
    o->bytes = mr.size;
    o->mem_type = s2a_vk_memory_type(c->d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    {
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = o->mem_type;
        if (vkAllocateMemory(c->d->device, &mai, NULL, &o->memory) != VK_SUCCESS) {
            release(c->d, o); if (why && why_size) snprintf(why, why_size, "vkAllocateMemory failed"); return NULL;
        }
        atomic_fetch_add(&g_tex_devmem_calls, 1);
        atomic_fetch_add(&g_tex_devmem_live, 1);
        if (dedicated) atomic_fetch_add(&g_tex_dedicated_calls, 1);
        s2a_tex_alloc_track(mr.size);   // 先记账再 bind：bind 失败走 release() 的 free_track，不会下溢
    }
    if (vkBindImageMemory(c->d->device, o->image, o->memory, 0) != VK_SUCCESS) {
        release(c->d, o); if (why && why_size) snprintf(why, why_size, "vkBindImageMemory failed"); return NULL;
    }
    VkImageViewType vt = depth > 1 ? VK_IMAGE_VIEW_TYPE_3D
                       : cube ? VK_IMAGE_VIEW_TYPE_CUBE
                       : cube_array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                       : layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    // 采样格式/aspect：深度只带 DEPTH；stencil 模式采样 stencil 面（S8_UINT + STENCIL），
    // 与附件视图（DEPTH|STENCIL）分开——Vulkan 不允许采样 DEPTH|STENCIL 合成视图。
    const int stencil_mode = t->stencil != 0;
    // 通道映射要在建视图之前就写进槽：tex_view_components 读的是槽上的 set_swz/swz（字形图集 a=R 等）。
    o->set_swz = t->set_swz; o->swz = (uint16_t)t->swz;
    o->nvn_target = target; o->view_type = vt;
    const VkFormat sample_fmt = (nvn_vk_format_is_depth(fmt) && stencil_mode) ? VK_FORMAT_S8_UINT : fmt;
    const VkImageAspectFlags sample_aspect = nvn_vk_format_is_depth(fmt)
        ? (stencil_mode ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkComponentMapping comps;
    if (tex_components_for_tex(t, fmt, &comps, why, why_size)) { release(c->d, o); return NULL; }
    o->view_att = VK_NULL_HANDLE;
    o->view = tex_make_view(c->d->device, o->image, 0, levels, 0, layers, sample_fmt, sample_aspect, vt, comps, why, why_size);
    if (!o->view) { release(c->d, o); return NULL; }
    o->views[0].view = o->view;
    o->views[0].base_level = 0; o->views[0].nlevels = levels;
    o->views[0].base_layer = 0; o->views[0].nlayers = layers;
    o->views[0].format = sample_fmt; o->views[0].type = vt;
    o->views[0].comps = comps;
    o->views[0].valid = 1;
    o->views[0].pins = 0;
    g_view_created++;
    o->view_att = o->view;
    if (nvn_vk_format_is_depth(fmt) && nvn_vk_format_has_stencil(fmt)) {
        o->view_att = tex_make_view(c->d->device, o->image, 0, levels, 0, layers, fmt,
                                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, vt,
                                    (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
                                    why, why_size);
        if (!o->view_att) { release(c->d, o); return NULL; }
    }
    o->last_use = ++c->clock;
    o->key = (uint64_t)(uintptr_t)t->mem; o->serial = t->serial; o->nvn_format = (uint32_t)t->d.format; o->vk_format = img_fmt;
    o->width = t->d.width; o->height = t->d.height; o->layers = layers; o->levels = levels; o->depth = depth;
    // 诊断：3D 纹理的建图事实（target/depth/layers/levels），用于核对视图层号是否是深度切片。
    if (depth > 1) {
        static int n3;
        int n = __atomic_fetch_add(&n3, 1, __ATOMIC_RELAXED);
        if (n < 8)
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                "tex 3D image: serial=%llu target=%u %ux%ux%u fmt=0x%x levels=%u layers=%u depth=%u img3d=1 compat=%d",
                (unsigned long long)t->serial, target, t->d.width, t->d.height, depth, t->d.format,
                levels, layers, depth, c->d->has_2d_array_compatible);
    }
    o->nvn_target = target; o->nvn_flags = (uint32_t)t->d.flags; o->nvn_stride = t->d.stride;
    o->nvn_pkg_block_h = t->d.pkg_block_h; o->view_type = vt; o->stencil_mode = (uint8_t)stencil_mode; o->valid = 1;
    o->set_swz = t->set_swz; o->swz = (uint16_t)t->swz;
    o->generation = ++c->next_generation;
    o->dsize = t->L.size;   // 批 A：内容账本/脏页查询都以存储字节数为准（未跟踪存储也保留）
    uint64_t upload_stamp = nvn_dirty_now();   // 本次上传的观察代数（上传期间的新写留给下一次检查）
    const int linear = (t->d.flags & 0x10) != 0;   // NVN_TEXTURE_FLAGS_LINEAR：行主序，不做块线性反混淆
    VkImageAspectFlags aspect = nvn_vk_format_is_depth(fmt)
        ? (nvn_vk_format_has_stencil(fmt) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT)
        : VK_IMAGE_ASPECT_COLOR_BIT;
    nvn_texlayout layout; nvn_tex_layout(&t->d, &layout);
    for (uint32_t l = 0; l < levels; l++) {
        if (linear) {
            int bw = 1, bh = 1;
            uint32_t bpb = (uint32_t)nvn_fmt_bpb(t->d.format, &bw, &bh);
            if (!bpb) { if (why && why_size) snprintf(why, why_size, "linear texture: NVN format 0x%x has no block size", t->d.format); release(c->d, o); return NULL; }
            uint32_t w = mipdim(t->d.width, l), h = mipdim(t->d.height, l);
            uint32_t tight = (w + (uint32_t)bw - 1) / (uint32_t)bw * bpb;
            // 行距按真实 stride（未给则按 32 字节对齐的紧凑行）；逐行拷成紧凑缓冲后上传。
            uint32_t pitch = t->d.stride ? t->d.stride : (tight + 31u) & ~31u;
            uint32_t rows = (h + (uint32_t)bh - 1) / (uint32_t)bh;
            uint64_t level_base = nvn_tex_level_offset(&t->d, l);
            uint8_t *tight_buf = malloc((size_t)tight * rows);
            if (!tight_buf) { if (why && why_size) snprintf(why, why_size, "linear level %u alloc failed", l); release(c->d, o); return NULL; }
            for (uint32_t a = 0; a < layers; a++) {
                uint64_t base = level_base + (uint64_t)a * layout.layer_size;
                for (uint32_t y = 0; y < rows; y++)
                    memcpy(tight_buf + (size_t)y * tight, (const uint8_t *)t->mem + base + (uint64_t)y * pitch, tight);
                int urc = tex_upload_or_decode(c, o->image, o->nvn_format, w, h, 1, tight_buf, (size_t)tight * rows, tight, l, a, aspect, why, why_size);
                if (urc) {
                    free(tight_buf);
                    if (why && why_size) snprintf(why, why_size, "linear level %u layer %u upload failed (rc=%d)", l, a, urc);
                    release_upload_failure(c->d, o, urc);
                    return NULL;
                }
            }
            free(tight_buf);
            continue;
        }
        if (depth > 1) {
            uint32_t row_bytes = 0, rows = 0, slices = 0, th = 0, td = 0, bpb = 0; uint64_t base = 0;
            if (nvn_deswizzle_geom(&t->d, l, 0, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || slices != mipdim(depth, l)) {
                if (why && why_size) snprintf(why, why_size, "3D level %u layout unsupported", l); release(c->d, o); return NULL;
            }
            uint8_t *lin = malloc((size_t)row_bytes * rows * slices);
            int urc = -1;
            if (lin && !nvn_deswizzle(&t->d, l, 0, t->mem, lin, row_bytes))
                urc = tex_upload_or_decode(c, o->image, o->nvn_format, mipdim(t->d.width, l), mipdim(t->d.height, l), slices, lin,
                             (size_t)row_bytes * rows * slices, row_bytes, l, 0, aspect, why, why_size);
            free(lin);
            if (urc) {
                if (why && why_size) snprintf(why, why_size, "3D level %u decode/upload failed (rc=%d)", l, urc);
                release_upload_failure(c->d, o, urc);
                return NULL;
            }
        } else {
            for (uint32_t a = 0; a < layers; a++) {
                uint32_t row_bytes = 0, rows = 0, slices = 0, th = 0, td = 0, bpb = 0; uint64_t base = 0;
                if (nvn_deswizzle_geom(&t->d, l, a, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || slices != 1) {
                    if (why && why_size) snprintf(why, why_size, "level %u layer %u layout unsupported", l, a); release(c->d, o); return NULL;
                }
                uint8_t *lin = malloc((size_t)row_bytes * rows);
                int urc = -1;
                if (lin && !nvn_deswizzle(&t->d, l, a, t->mem, lin, row_bytes))
                    urc = tex_upload_or_decode(c, o->image, o->nvn_format, mipdim(t->d.width, l), mipdim(t->d.height, l), 1, lin,
                                 (size_t)row_bytes * rows, row_bytes, l, a, aspect, why, why_size);
                free(lin);
                if (urc) {
                    if (why && why_size) snprintf(why, why_size, "level %u layer %u decode/upload failed (rc=%d)", l, a, urc);
                    release_upload_failure(c->d, o, urc);
                    return NULL;
                }
            }
        }
    }
    // 批 A：建 guest 内容账本并初始化子资源版本。刚上传完 = 图像与 guest 一致，全部 CPU。
    if (nvk_subres_ensure(o) == 0) nvk_subres_set_all(o, NVK_SUBRES_CPU);
    if (nvk_ledger_ensure(o, t->L.size) == 0) { nvk_ledger_set_all(o, t->mem); c->ledger_bytes += (uint64_t)o->ngobs * sizeof(uint64_t); if (c->ledger_bytes > g_ledger_peak) g_ledger_peak = c->ledger_bytes; }
    else c->ledger_fail++;
    // 刚上传完 = 图像与 guest 一致，整段存储都已在“图像==guest”范围里（之后 GPU 写会清空）。
    nvk_range_add(o, &o->sync_r, &o->n_sync, &o->cap_sync, &o->sync_truncated, 0, t->L.size);
    // 诊断：debug.s2a.log_tex=1 时，把“大但不在内容跟踪范围”的纹理打出来（查地面纹理为何不跟踪）。
    if (!(t->L.size && t->L.size <= (32u << 20)) && t->L.size >= (1u << 20)) {
        static int logv2 = -1;
        if (logv2 < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.log_tex", b); logv2 = (b[0] && atoi(b)) ? 1 : 0; }
        if (logv2) {
            static int n2;
            if (n2++ < 200)
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                    "tex untracked addr=%p %ux%u fmt=0x%x levels=%u layers=%u depth=%u target=%u flags=0x%x size=%zu",
                    (void *)t->mem, t->d.width, t->d.height, t->d.format, levels, layers, depth,
                    t->d.target, (unsigned)t->d.flags, (size_t)t->L.size);
        }
    }
    // 诊断：debug.s2a.log_tex=1 时有界打印创建信息（地址/尺寸/mip/层/格式/大小），用来查“谁该填这张图”。
    {
        static int logv = -1;
        if (logv < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.log_tex", b); logv = (b[0] && atoi(b)) ? 1 : 0; }
        if (logv && t->d.width >= 512 && t->d.height >= 512) {   // 只看大纹理，避免被启动期的小图刷满
            static int n;
            if (n++ < 500)
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                    "tex create addr=%p serial=%llu %ux%u nvn_fmt=0x%x levels=%u layers=%u depth=%u size=%zu target=%u",
                    (void *)t->mem, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format,
                    levels, layers, depth, (size_t)t->L.size, t->d.target);
        }
    }
    // 所有可解码 target 都跟踪（1D/2D/3D/数组/立方体/RECT）：不再靠“淘汰后重建”兜底之后，
    // 首次上传时内容还没就绪的纹理（角色/NPC 贴图就是）会永久停在空内容，表现为黑剪影。
    // 存储被页级写跟踪覆盖：保存范围句柄 + 上传代数，并按现有协议把覆盖页设只读；
    // 之后游戏/宿主的写入由 nvn_dirty 记账（缺页 → 记脏 → 放开 → 重执行），命中时只查代数。
    if (t->L.size && !o->upload_incomplete) {
        const void *span = nvn_dirty_span(t->mem, t->L.size);
        if (!span) {
            c->arm_failed++;
            static int n;
            if (n++ < 20)
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                    "tex dirty arm failed (storage not tracked): addr=%p serial=%llu size=%zu target=%u fmt=0x%x",
                    (void *)t->mem, (unsigned long long)t->serial, (size_t)t->L.size, target, t->d.format);
        }
        if (span) {
            o->dspan = span; o->dsize = t->L.size; o->dgen = upload_stamp; o->dirty_armed = 1;
            o->dmap_gen = nvn_dirty_mapping_generation();   // 换底会让这个值变化 → 强制重新验证/重传
            nvn_dirty_protect(t->mem, t->L.size);
        }
    }
    if (t->L.size && t->L.size <= (32u << 20)) {
        o->content_hash = tex_hash(t->mem, t->L.size);
        o->content_check_ms = (uint64_t)tex_now_ms();
        o->content_created_ms = o->content_check_ms;
        o->content_tracked = 1;
    }
    return o;
}

// 前端说这张纹理的 CPU 内容变了（mtl_tex_cpu_written）或纹理对象释放（mtl_tex_release）：
// 丢掉缓存槽，下一次 get_tex 会按新 serial/内容重建。没命中就什么也不做。
void nvn_vk_texcache_drop_tex(struct nvn_vk_texcache *c, const tex_t *t) {
    if (!c || !t) return;
    // 释放幂等（原生线 #1014）：Finalize 只清 magic 不清 serial，重复释放会再次进来。
    // serial 为 0 时已没有身份可锚定，再按地址匹配会误伤同一地址上新建立的纹理。
    if (!t->serial) return;
    {
        struct nvn_vk_tex *s = tex_find_serial(c, t->serial, NULL);
        if (!s) return;
        if (!tex_remove_serial(s, t->serial)) return;   // 还有别的 serial 共享这张图像
        slot_retire(c, s);
    }
}

// A3：按 serial 的资源状态查询（唯一归属是 texcache；不建资源、不改状态）。
// 活表命中优先于墓碑（serial 复用后新对象是新的身份代际）；墓碑表淘汰过条目后缺席只能报未知。
int nvn_vk_texcache_serial_state(uint64_t serial, struct nvn_tex_tombstone *tomb) {
    if (tomb) memset(tomb, 0, sizeof *tomb);
    if (!serial) return NVN_TEX_SERIAL_UNKNOWN;
    struct nvn_vk_texcache *c = g_tex_state_owner;
    if (!c) return NVN_TEX_SERIAL_UNKNOWN;   // 没有直连会话：不能断言“从未创建”
    const int live = tex_find_serial(c, serial, NULL) != NULL;
    const struct nvn_tex_tombstone *found = tex_tombstones_find(&c->tombstones, serial);
    const int state = tex_serial_state_from(live, found != NULL, c->tombstones.evicted != 0);
    if (state == NVN_TEX_SERIAL_RETIRED && tomb) *tomb = *found;
    return state;
}

void nvn_vk_texcache_tombstone_stats(uint32_t *n, uint32_t *cap, uint64_t *evicted) {
    struct nvn_vk_texcache *c = g_tex_state_owner;
    if (n) *n = c ? c->tombstones.n : 0;
    if (cap) *cap = NVK_TEX_TOMB_MAX;
    if (evicted) *evicted = c ? c->tombstones.evicted : 0;
}

// 按注册视图取（或建）精确视图。身份 = (mip 起点/数量, 层起点/数量, 格式, 视图类型, aspect, 通道映射)；
// 同一图像上换视图只建/换 VkImageView，不重建图像、不重传 CPU 内容。
// want_aspect：0 = 采样用途（按 dsmode/stencil 推导）；非 0 = 附件用途（COLOR 或 DEPTH|STENCIL）。
// 附件 view 不做通道 swizzle（附件写入不受采样分量映射影响），并返回实际使用的 VkFormat。
static VkImageView tex_view_core(struct nvn_vk_texcache *c, struct nvn_vk_tex *t, const view_t *v,
                                 VkImageAspectFlags want_aspect, VkFormat *fmt_out,
                                 char *why, size_t why_size) {
    if (!c || !c->d || !c->d->device) {
        if (why && why_size) snprintf(why, why_size, "texture cache has no device");
        return VK_NULL_HANDLE;
    }
    if (!t || !t->valid || !t->view || !t->image) {
        if (why && why_size) snprintf(why, why_size, "texture has no usable image/view");
        return VK_NULL_HANDLE;
    }
    // 实际图像格式（compute 解码的 BC 纹理是解码后格式，不是 nvn_vk_format(nvn)）。
    const VkFormat image_fmt = t->vk_format ? t->vk_format : nvn_vk_format(t->nvn_format);
    if (fmt_out) *fmt_out = image_fmt;
    const int attachment = want_aspect != 0;
    if (!v || !(v->set_format || v->set_levels || v->set_layers || v->set_target ||
                v->set_swizzle || v->set_dsmode)) {
        // 采样默认视图直接复用整幅视图。附件不能走这条：NVN 未显式设范围时落到
        // mip 0 / layer 0，3D 的 depth slice 必须建成 2D 视图才能挂 framebuffer。
        if (!attachment) {
            if (fmt_out) *fmt_out = t->views[0].format;
            return t->view;
        }
    }
    if (attachment && v && v->set_target) {
        if (why && why_size) snprintf(why, why_size, "attachment view cannot change the view target");
        return VK_NULL_HANDLE;
    }
    // 范围/类型统一由纯决策给出：3D 的层号是深度切片，array/cube 的单层视图
    // 落成 2D/2D_ARRAY；越界不钳位，准确失败。决策与宿主测试共用同一实现。
    struct nvn_vk_texview_plan plan;
    nvn_vk_texview_plan_make(t->nvn_target, t->depth, t->layers, t->levels, v, attachment,
                             c->d->has_2d_array_compatible, &plan);
    if (!plan.ok) {
        if (why && why_size) snprintf(why, why_size, "%s", plan.why);
        return VK_NULL_HANDLE;
    }
    const uint32_t bl = plan.base_level, nl = plan.nlevels, by = plan.base_layer, ny = plan.nlayers;
    if (t->depth > 1 && plan.type != VK_IMAGE_VIEW_TYPE_3D) {
        static int nv3;
        int n = __atomic_fetch_add(&nv3, 1, __ATOMIC_RELAXED);
        if (n < 8)
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                "tex 3D view: serial=%llu mip=%u+%u slice=%u+%u of %u -> viewType=%d attach=%d",
                (unsigned long long)t->serial, bl, nl, by, ny, t->depth, (int)plan.type, attachment);
    }
    const int depth_format = nvn_vk_format_is_depth(image_fmt);
    VkImageAspectFlags aspect;
    VkFormat fmt;
    if (attachment) {
        aspect = want_aspect;
        fmt = image_fmt;
        if (depth_format && v && v->set_format) {
            if (why && why_size) snprintf(why, why_size,
                "depth/stencil attachment format override has no mutable-format path");
            return VK_NULL_HANDLE;
        }
    } else {
        int stencil_face = t->stencil_mode ? 1 : 0;
        if (v && v->set_dsmode) stencil_face = (v->dsmode == 1);
        fmt = stencil_face ? VK_FORMAT_S8_UINT : t->views[0].format;
        if (stencil_face) {
            if (!depth_format || !nvn_vk_format_has_stencil(image_fmt)) {
                if (why && why_size) snprintf(why, why_size, "stencil-mode view of a texture without stencil bits");
                return VK_NULL_HANDLE;
            }
            if (v && v->set_format) { if (why && why_size) snprintf(why, why_size, "stencil-mode view cannot override the format"); return VK_NULL_HANDLE; }
        }
        aspect = depth_format ? (stencil_face ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT)
                              : VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if (v && v->set_format) {
        // 视图就是纹理自身 NVN 格式时沿用解码后的图像/采样格式（BC4 → R8_UNORM），不进兼容类表。
        if (nvn_vk_format_texview_ok(fmt, t->nvn_format, v->format, &fmt, why, why_size))
            return VK_NULL_HANDLE;
    }
    VkImageViewType type = plan.type;
    VkComponentMapping comps;
    if (attachment) {
        comps = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    } else if (tex_view_components(t, v, fmt, &comps, why, why_size)) {
        return VK_NULL_HANDLE;
    }
    for (int i = 0; i < t->views_cap; i++)
        if (view_same(&t->views[i], bl, nl, by, ny, fmt, type, aspect, comps)) {
            t->views[i].last_use = ++t->view_clock;
            g_view_reused++;
            if (fmt_out) *fmt_out = fmt;
            return t->views[i].view;
        }
    // 建新视图：优先空槽，其次回收 1..cap-1 里最久未用且不在途的（槽 0 是默认整幅视图，不回收）。
    // 底图 pin 只保 image；在途 view 自己 pins>0。全部在途时先按需扩槽（3D 深度切片一次
    // 可能同时用到几十个切片视图），到硬上限仍全在途才准确失败，绝不提前 destroy。
    const int envcap = texview_env_capacity();
    int cap = t->views_cap;
    if (envcap > 1 && envcap < cap) cap = envcap;
    int slot = -1;
    for (int i = 1; i < cap; i++) if (!t->views[i].valid) { slot = i; break; }
    if (slot < 0) {
        uint64_t oldest = 0; int victim = -1, pinned = 0;
        for (int i = 1; i < cap; i++) {
            if (!t->views[i].view) continue;
            if (t->views[i].pins) { pinned++; continue; }
            if (victim < 0 || t->views[i].last_use < oldest) { victim = i; oldest = t->views[i].last_use; }
        }
        if (victim < 0) {
            int hard = envcap > 1 ? (envcap < NVK_TEX_VIEWS_MAX ? envcap : NVK_TEX_VIEWS_MAX) : NVK_TEX_VIEWS_MAX;
            if (cap < hard) {
                uint32_t want = (uint32_t)cap * 2;
                if (want <= (uint32_t)cap) want = (uint32_t)cap + 1;
                if (want > (uint32_t)hard) want = (uint32_t)hard;
                if (tex_views_reserve(t, want) == 0) {
                    cap = t->views_cap;
                    if (envcap > 1 && envcap < cap) cap = envcap;
                    for (int i = 1; i < cap; i++) if (!t->views[i].valid) { slot = i; break; }
                }
            }
        }
        if (slot < 0 && victim >= 0) {
            slot = victim;
            nvn_vk_first_draw_forget_view(t->views[slot].view);
            vkDestroyImageView(c->d->device, t->views[slot].view, NULL);
            g_view_evicted++;
            memset(&t->views[slot], 0, sizeof t->views[slot]);
        }
        if (slot < 0) {
            g_view_pinned_full++;
            if (why && why_size) snprintf(why, why_size,
                "texture view cache full of in-flight views (serial=%llu target=%u depth=%u levels=%u "
                "request mip %u+%u layer %u+%u type=%d; %d pinned of %d slots); "
                "refusing to destroy a view still in use",
                (unsigned long long)t->serial, t->nvn_target, t->depth, t->levels,
                bl, nl, by, ny, (int)type, pinned, cap - 1);
            return VK_NULL_HANDLE;
        }
    }
    VkImageView nv = tex_make_view(c->d->device, t->image, bl, nl, by, ny, fmt, aspect, type, comps, why, why_size);
    if (!nv) return VK_NULL_HANDLE;
    t->views[slot].view = nv;
    t->views[slot].base_level = bl; t->views[slot].nlevels = nl;
    t->views[slot].base_layer = by; t->views[slot].nlayers = ny;
    t->views[slot].format = fmt; t->views[slot].type = type; t->views[slot].aspect = aspect;
    t->views[slot].comps = comps;
    t->views[slot].last_use = ++t->view_clock;
    t->views[slot].valid = 1;
    t->views[slot].pins = 0;
    g_view_created++;
    if (fmt_out) *fmt_out = fmt;
    return nv;
}
VkImageView nvn_vk_texcache_view(struct nvn_vk_texcache *c, struct nvn_vk_tex *t, const view_t *v,
                                 char *why, size_t why_size) {
    return tex_view_core(c, t, v, 0, NULL, why, why_size);
}
VkImageView nvn_vk_texcache_attachment_view(struct nvn_vk_texcache *c, struct nvn_vk_tex *t, const view_t *v,
                                            VkFormat *format_out, char *why, size_t why_size) {
    // 与 tex_view_core 同一口径：compute 解码的 BC 纹理图像格式是 t->vk_format，不是 nvn_vk_format(nvn)。
    const VkFormat image_fmt = t ? (t->vk_format ? t->vk_format : nvn_vk_format(t->nvn_format)) : VK_FORMAT_UNDEFINED;
    if (image_fmt != VK_FORMAT_UNDEFINED && !nvn_vk_format_is_depth(image_fmt)) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(c->d->physical, image_fmt, &p);
        if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            if (why && why_size) snprintf(why, why_size, "format %d has no colour attachment feature", (int)image_fmt);
            return VK_NULL_HANDLE;
        }
    }
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (nvn_vk_format_is_depth(image_fmt))
        aspect = nvn_vk_format_has_stencil(image_fmt)
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
    return tex_view_core(c, t, v, aspect, format_out, why, why_size);
}
