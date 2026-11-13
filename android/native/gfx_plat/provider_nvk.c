// 安卓直连 provider（新回放层版）。与 C# 的会话表、窗口租约、profile 合成、编译登记这些生命周期逻辑取自 direct/provider_vk.c，
// 只换掉对旧会话（mtl_vk_adapter）的 9 处调用，并去掉编译观测通道。旧层删除时 direct/provider_vk.c 一并删除，这里是唯一 provider。
// C1：生产 direct provider —— 用 App 自己的 Vulkan device 建候选（不写 guest 内存、不猜设备）。
// 关键约定：
//   - available() 只在"共享 NVN 前端 + Vulkan owner 都编进本 DSO"时为 1（本文件只在这些条件成立时参与编译）；
//   - prepare() 真的建 device/queue 并填表；失败返回负码并写准确原因，不伪造 table；
//   - activate() 用同一个 device/queue 初始化 nvn_vk 适配层会话（着色器目录取 <revision>/shaders/vulkan/direct）；
//     出图走 mtl_present → 适配层 swapchain，present 退休证明按 F4.4/F5.3 的判据；
//   - destroy()/request_stop() 关会话并释放 device 与上下文。
#include <android/log.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "direct/graphics.h"
#include "direct/selection.h"
#include "direct/s2a_direct.h"
#include "direct/vk_device.h"
#include "direct/vk_identity.h"
#include "direct/sha256.h"
#include "direct/compile_request.h"
#include "direct/compile_worker.h"
// direct 会话挂新回放层：回放核心 graphics/nvn/replay + Vulkan 后端 nvn_gpu_*，会话装配在 gfx_plat/nvk_session.c。
#include "gfx_plat/nvk_session.h"
#include "nvn/nvn_vk_lifecycle.h"
#include "wsi_lease.h"
#include "profile_compose.h"
#include <android/native_window.h>
#include "diag.h"
#include "program_cache.h"
#include "debug_gate.h"

#define TAG "S2Arm"

// 共享 NVN 前端的入口：用来证明前端真的链接进来（而不是只有 provider 一个空壳）。
extern void *h_nvnBootstrapLoader(const char *name);

// 外壳在调用 s2a_direct_select 之前填好的上下文（runtime.c 从 services 请求里取）。
// 只放"后端初始化必需"的字段，不把 runtime 的请求结构塞进 direct 层。
struct s2a_direct_vk_context {
    const char *game_data_dir;   // 已激活 revision 的根（沙盒屏幕内的只读内容）
    const char *state_dir;       // 会话状态目录（attempt 锁在同一棵树里）
    double display_hz;           // 0=未知
    const void *lease_api;       // 既有 s2a_wsi_api（直接连接窗口用，当前未使用）
};
static struct s2a_direct_vk_context g_context;
void s2a_direct_provider_vk_configure(const struct s2a_direct_vk_context *context) {
    if (context) g_context = *context;
    else memset(&g_context, 0, sizeof g_context);
}

// The lease publishes the current mode, not the panel's maximum supported rate.
double s2a_direct_vk_display_refresh(void) {
    const struct s2a_wsi_api *lease = (const struct s2a_wsi_api *)g_context.lease_api;
    if (!lease || lease->size < sizeof *lease || lease->abi_version != S2A_WSI_ABI_VERSION || !lease->lease)
        return 0;
    return s2a_wsi_refresh_hz(lease->lease);
}

struct provider_context {
    struct s2a_vk_device device;
    int device_ok, activation_started; // activation_started is protected by g_window_mu
    pthread_t idle_thread;
    int idle_started, idle_result;
    uint64_t idle_deadline;
    _Atomic int idle_done, idle_phase, destroying;
    uint64_t session;
    uint32_t state;      // s2a_direct_state
    uint32_t blockers;
    int32_t error;
    int engine_ok;       // 后端已开：适配层会话已建（destroy/Stop 靠它决定要不要做收尾）
    void *lease_window;  // nvn_vk 模式：从 WSI lease 持有的窗口（销毁时归还）
    uint64_t lease_generation;
    int has_window;
    int revoking;             // surfaceDestroyed/Stop 已发布撤销：present 线程不得再 take/attach
    int stopping_engine;      // Stop/Destroy 正在 g_window_mu 之外拆引擎：release 不得并发 detach
    char window_error[192];   // 最近一次窗口生命周期失败原因（surfaceDestroyed 收尾写进 tombstone）
    char reason[160];
    char shader_dir[512];
    char diag_dir[512];          // 本局私有诊断目录（F4.1：不写安装 revision）
    char cache_dir[512];         // 跨局编译结果缓存（F5.1：应用私有，跨局复用）
    // enabled profile（真实设备事实 + 编译器常量/哈希合成）：编译请求原样带上，身份取 profileHash。
    char profile_json[16384];
    size_t profile_bytes;
    char profile_hash[72];
    int offline_profile_available;
    // 编译请求的身份半（与 profile 一起原样进请求）：descriptor 的 closure/directABI + 本上下文的代际。
    char closure[128];
    uint32_t direct_abi;
    uint64_t generation;
};
// F4.4 owner 控制片：managed owner 循环调用 poll。没有在途编译/窗口事件时不能忙等，
// 用条件变量睡满一个控制片；activate/Stop/窗口 offer/revoke 都发布 epoch 唤醒。
// 这不改变任何生产语义，只把“owner 控制轮询”从空实现变成真实等待点。
static pthread_mutex_t g_ctl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ctl_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_ctl_epoch;
// Stop 已发布（request_stop 入口置位）：poll 必须立刻返回 STOPPED，不能再去驱动在拆的 worker。
static _Atomic int g_stop_published;
static _Atomic int g_session_active;
static _Atomic unsigned long long g_owner_slices, g_owner_stop_seen;

static int64_t ctl_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + (int64_t)t.tv_nsec;
}
static void ctl_wake(void) {
    pthread_mutex_lock(&g_ctl_mu);
    g_ctl_epoch++;
    pthread_cond_broadcast(&g_ctl_cv);
    pthread_mutex_unlock(&g_ctl_mu);
}
// 睡到 deadline（monotonic），或者被 ctl_wake 提前唤醒。返回 1 = 被唤醒，0 = 超时/已过期。
static int ctl_wait_until(int64_t deadline_ns) {
    int64_t left = deadline_ns - ctl_now_ns();
    if (left <= 0) return 0;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += (time_t)(left / 1000000000ll);
    dl.tv_nsec += (long)(left % 1000000000ll);
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_ctl_mu);
    uint64_t epoch = g_ctl_epoch;
    int woke = 0;
    while (g_ctl_epoch == epoch) {
        if (pthread_cond_timedwait(&g_ctl_cv, &g_ctl_mu, &dl) == ETIMEDOUT) break;
        woke = 1;
    }
    pthread_mutex_unlock(&g_ctl_mu);
    return woke;
}
// 当前 direct 会话上下文：mtl_present 的“迟到的窗口”回调要在 activate 之后找到它。
struct provider_context *g_active_context;
// 退休失败的 owner 保留到进程退出；不能被下一次 prepare 覆盖。
static struct provider_context *g_retained_context;
// 窗口生命周期串行化：game 线程的 present/attach 与 UI 线程的 surfaceDestroyed/Stop 不能同时
// 改 ctx->has_window/lease_window。锁顺序固定 window -> adapter(g_mu)，adapter 侧不回调 provider。
static pthread_mutex_t g_window_mu = PTHREAD_MUTEX_INITIALIZER;
// Stop poll checks the published atomic itself; when this mutex is busy the
// 10 ms control slice supplies the wakeup without an unbounded shutdown lock.
static void ctl_wake_stop(void) {
    if (!pthread_mutex_trylock(&g_ctl_mu)) { g_ctl_epoch++; pthread_cond_broadcast(&g_ctl_cv); pthread_mutex_unlock(&g_ctl_mu); }
}
static int stop_window_lock(void) {
    uint64_t deadline = nvk_session_stop_deadline();
    uint64_t start = deadline - NVK_SHUTDOWN_BUDGET_NS;
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk stop enter window lock: elapsed=%llums", (unsigned long long)(((uint64_t)ctl_now_ns() - start) / 1000000ull));
    for (;;) {
        int e = pthread_mutex_trylock(&g_window_mu);
        if (!e) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "nvk stop exit window lock: elapsed=%llums", (unsigned long long)(((uint64_t)ctl_now_ns() - start) / 1000000ull));
            return 0;
        }
        if (e != EBUSY || !deadline || (uint64_t)ctl_now_ns() >= deadline) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk stop window lock deadline/error: elapsed=%llums rc=%d (context retained)", (unsigned long long)(((uint64_t)ctl_now_ns() - start) / 1000000ull), e); return -1;
        }
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    }
}
// owner 控制片对账：适配层执行包挂起切片 / 窗口推进次数。
static _Atomic unsigned long long g_owner_suspended_slices, g_owner_window_actions;

// 统一编译请求的输入来源（Draw 接线用）：session/generation、enabled profile 原文与 identity 半。
// 只在 provider 存活期间有效；未激活或 profile 未合成时返回非 0。
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) {
    if (!out || !g_active_context) return -1;
    struct provider_context *ctx = g_active_context;
    if (!ctx->session || !ctx->generation || !ctx->profile_bytes) return -1;
    out->session = ctx->session;
    out->generation = ctx->generation;
    out->profile_json = ctx->profile_json;
    out->profile_bytes = ctx->profile_bytes;
    out->direct_abi = ctx->direct_abi;
    out->translator_closure = ctx->closure;
    out->profile_hash = ctx->profile_hash;
    out->offline_profile_available = ctx->offline_profile_available;
    out->shader_draw_parameters = ctx->device.shader_draw_parameters_enabled ? 1 : 0;
    // 编译事实的 SSBO 对齐来源只能取本局真实设备的 limits；没有设备时保持 0（缺口）。
    out->storage_buffer_offset_alignment =
        ctx->device_ok ? (uint32_t)ctx->device.properties.limits.minStorageBufferOffsetAlignment : 0;
    return 0;
}
int s2a_vk_device_caps_json(const struct s2a_vk_device *device, char *out, size_t cap, size_t *len);

static void release_lease_window(struct provider_context *ctx);

// context 可能已经被 destroy 释放（Stop 失败后进程仍可能收到 surfaceDestroyed）：
// 原因另留一份进程级副本，保证 tombstone 能读到真正的首错，而不是只有一句泛化超时。
static char g_window_error_fallback[192];
static void set_window_error(struct provider_context *ctx, const char *text) {
    snprintf(g_window_error_fallback, sizeof g_window_error_fallback, "%s", text ? text : "");
    if (ctx) snprintf(ctx->window_error, sizeof ctx->window_error, "%s", text ? text : "");
}
static void clear_window_error(struct provider_context *ctx) {
    g_window_error_fallback[0] = 0;
    if (ctx) ctx->window_error[0] = 0;
}

// surfaceDestroyed 收尾失败时，window.cpp 把这句写进 abort message/tombstone：
// 只有它非空才代表“租约没还、我也不能还”。
const char *s2a_direct_vk_window_error(void) {
    // 调用方在 release 返回后读；destroy 可能刚清掉 context。锁内复制一份快照，
    // 不在锁外返回可能被改写的缓冲区。
    static _Thread_local char snapshot[192];
    pthread_mutex_lock(&g_window_mu);
    const char *src = (g_active_context && g_active_context->window_error[0])
                          ? g_active_context->window_error : g_window_error_fallback;
    snprintf(snapshot, sizeof snapshot, "%s", src);
    pthread_mutex_unlock(&g_window_mu);
    return snapshot;
}

// 归还 take 到的窗口租约：先 release_window 再 released，顺序与 wsi_lease.h 的约定一致。
static void release_lease_window(struct provider_context *ctx) {
    const struct s2a_wsi_api *lease = (const struct s2a_wsi_api *)g_context.lease_api;
    if (!ctx || !lease || lease->size < sizeof *lease) return;
    if (ctx->lease_window && lease->release_window) lease->release_window(lease->lease, ctx->lease_window);
    if (ctx->lease_generation && lease->released) lease->released(lease->lease, ctx->lease_generation);
    ctx->lease_window = NULL;
    ctx->lease_generation = 0;
    ctx->has_window = 0;
}

// 从租约取当前窗口并挂到后端。返回 0 = 挂上；1 = 当前没有可挂窗口/owner 锁忙（正常状态，
// present 跳过或下一轮重试）；-1 = 有窗口但挂不上（真实失败，原因写 window_error）。
// attach_budget_ns<0 表示等锁不设上限（present 路径）；>=0 时交给 owner poll 做有界等锁。
// take 到的引用在这里结清：成功时由 ctx->lease_window 持有到窗口撤销，失败时立刻 release_window + released。
// 调用方必须持有 g_window_mu（窗口状态与租约引用一起改，不能只锁 adapter）。
static int take_and_attach_window_locked(struct provider_context *ctx, int64_t attach_budget_ns) {
    if (!ctx || !nvk_session_ready()) return 1;
    // surfaceDestroyed/Stop 已发布撤销：activate 晚到也不能在 revoke 完成前重新 take 本代窗口。
    if (ctx->revoking) return 1;
    const struct s2a_wsi_api *lease = (const struct s2a_wsi_api *)g_context.lease_api;
    if (!lease || lease->size < sizeof *lease || !lease->lease || !lease->take) {
        set_window_error(ctx, "no WSI lease API; presenting will fail loudly");
        __android_log_print(ANDROID_LOG_WARN, TAG, "direct activate: %s", ctx->window_error);
        return 1;
    }
    struct s2a_wsi_state st;
    memset(&st, 0, sizeof st);
    int trc = lease->take(lease->lease, &st);
    if (trc || !st.window) {
        if (trc) {
            // 没有窗口时不 retain；take 的 EBUSY 只可能是上一代窗口还没 released。
            char text[160];
            snprintf(text, sizeof text, "WSI take failed (%d): a previous window generation is still held", trc);
            set_window_error(ctx, text);
            __android_log_print(ANDROID_LOG_ERROR, TAG, "direct present: %s", text);
            return -1;
        }
        return 1;   // 还没有窗口：由 mtl_present 的无 surface 收尾处理
    }
    if (st.stopping) {
        lease->release_window(lease->lease, st.window);
        lease->released(lease->lease, st.generation);
        return 1;
    }
    char why[192] = "";
    (void)attach_budget_ns;
    int arc = nvk_session_attach_window(st.window, st.generation, why, sizeof why);
    if (arc == 2) {
        // owner 锁忙：本轮不推进，窗口引用立刻还掉；不是失败。
        lease->release_window(lease->lease, st.window);
        lease->released(lease->lease, st.generation);
        return 1;
    }
    if (arc) {
        char text[192];
        snprintf(text, sizeof text, "window attach failed: %s", why[0] ? why : "unknown");
        set_window_error(ctx, text);
        __android_log_print(ANDROID_LOG_ERROR, TAG, "direct present: %s", text);
        lease->release_window(lease->lease, st.window);
        lease->released(lease->lease, st.generation);
        return -1;
    }
    ctx->lease_window = st.window;
    ctx->lease_generation = st.generation;
    ctx->has_window = 1;
    clear_window_error(ctx);
    __android_log_print(ANDROID_LOG_INFO, TAG, "direct present: window attached gen=%llu, %dx%d",
                        (unsigned long long)st.generation, st.width, st.height);
    return 0;
}

// mtl_present / owner poll 的窗口统一入口：挂新代、换代撤销、旋转/resize 原地重建都在这里决定。
// 返回 0 = 当前有可用窗口；1 = 当前没有窗口/锁忙（正常跳过 present，下一轮重试）；
// -1 = 呈现退休/重建失败（旧 swapchain、present fence、租约全部保留，调用方必须失败退出）。
// attach_budget_ns 只用于“还没有窗口”的 take/attach；present 路径传 <0（不设上限），
// owner poll 传一个控制片内的预算，锁忙时按“本轮不推进”返回，不把控制轮询卡在 draw 上。
static int sync_window_locked(struct provider_context *ctx, int64_t attach_budget_ns) {
    if (!ctx || !nvk_session_ready()) return 1;
    // surfaceDestroyed/Stop 已经发布撤销：在 revoke 完成前不得再 take/attach，否则会在
    // surfaceDestroyed 与 s2a_wsi_revoke 之间把同一代窗口重新挂上，revoke 等不到 released 而超时。
    if (ctx->revoking) return 1;
    if (!ctx->has_window) return take_and_attach_window_locked(ctx, attach_budget_ns);
    const struct s2a_wsi_api *lease = (const struct s2a_wsi_api *)g_context.lease_api;
    if (!lease || lease->size < sizeof *lease || !lease->lease || !lease->take) return 0;

    // take 只用来读租约当前状态（临时 retain 一个引用，立即还掉，不改变 held）。
    struct s2a_wsi_state st;
    memset(&st, 0, sizeof st);
    int trc = lease->take(lease->lease, &st);
    int lease_has = trc == 0 && st.window != NULL;
    if (lease_has && lease->release_window) lease->release_window(lease->lease, st.window);

    int32_t win_w = ctx->lease_window ? ANativeWindow_getWidth((ANativeWindow *)ctx->lease_window) : 0;
    int32_t win_h = ctx->lease_window ? ANativeWindow_getHeight((ANativeWindow *)ctx->lease_window) : 0;
    // 挂窗口时记下的发布尺寸（不是被表面能力夹取后的 extent）
    int32_t attached_w = 0, attached_h = 0; nvk_session_window_size(&attached_w, &attached_h);
    int action = nvn_vk_window_action(
        1, ctx->lease_generation, attached_w, attached_h,
        lease_has, st.generation, st.stopping, win_w, win_h);
    if (action == NVK_WINDOW_REVOKED) {
        char text[192];
        snprintf(text, sizeof text,
                 "window generation revoked/changed (held gen %llu, lease gen %llu, window=%d, stopping=%d)",
                 (unsigned long long)ctx->lease_generation, (unsigned long long)st.generation,
                 lease_has, st.stopping);
        __android_log_print(ANDROID_LOG_INFO, TAG, "direct present: %s", text);
        if (nvk_session_detach_window()) {
            set_window_error(ctx, "surface revoked but the swapchain has no presentation-retirement proof; "
                                  "window lease kept (not ACKed)");
            __android_log_print(ANDROID_LOG_ERROR, TAG, "direct present: %s", ctx->window_error);
            return -1;
        }
        release_lease_window(ctx);
        clear_window_error(ctx);
        return 1;
    }
    if (action == NVK_WINDOW_REBUILD) {
        // 同一代窗口的发布尺寸变了（旋转/resize/分屏）：extent/crop 不能继续用旧值。
        if (nvk_session_detach_window()) {
            set_window_error(ctx, "window resized but the old swapchain has no presentation-retirement proof; "
                                  "swapchain/window lease kept");
            __android_log_print(ANDROID_LOG_ERROR, TAG, "direct present: %s", ctx->window_error);
            return -1;
        }
        char why[192] = "";
        if (nvk_session_attach_window(ctx->lease_window, ctx->lease_generation, why, sizeof why)) {
            char text[192];
            snprintf(text, sizeof text, "window resize: swapchain rebuild failed: %s", why[0] ? why : "unknown");
            set_window_error(ctx, text);
            __android_log_print(ANDROID_LOG_ERROR, TAG, "direct present: %s", text);
            release_lease_window(ctx);
            return -1;
        }
        int32_t nw = ANativeWindow_getWidth((ANativeWindow *)ctx->lease_window);
        int32_t nh = ANativeWindow_getHeight((ANativeWindow *)ctx->lease_window);
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "direct present: swapchain rebuilt for window %dx%d (gen %llu, rotation/resize)",
                            nw, nh, (unsigned long long)ctx->lease_generation);
        clear_window_error(ctx);
        return 0;
    }
    return 0;
}

int s2a_direct_vk_sync_window(void) {
    pthread_mutex_lock(&g_window_mu);
    int rc = sync_window_locked(g_active_context, -1);
    pthread_mutex_unlock(&g_window_mu);
    return rc;
}

// owner poll 的窗口推进：只在有执行包挂起时调用。这里只做“还没有窗口时把新 generation 接上”，
// 不在 poll 里撤销/重建已有交换链：撤销由 surfaceDestroyed 的 s2a_direct_vk_release_window 负责，
// 旋转/换代后的重建由 present 路径负责。poll 多拆一次交换链会与另一个 guest 线程的 present
// 形成“检查窗口”和“真正 present”之间的新竞态，不属于 F4.4 的收益。
// 返回 0 = 已有窗口；1 = 没有窗口/锁忙（下一片重试）；-1 = 真实 attach 失败（由调用方记录并重试）。
static int owner_window_control(struct provider_context *ctx, int64_t attach_budget_ns) {
    pthread_mutex_lock(&g_window_mu);
    int rc;
    if (!ctx || ctx->revoking) rc = 1;
    else if (ctx->has_window) rc = 0;
    else rc = take_and_attach_window_locked(ctx, attach_budget_ns);
    pthread_mutex_unlock(&g_window_mu);
    return rc;
}

// 外壳 surfaceCreated 交出新 generation 后调用：清除撤销标记，下一次 present 的 sync 会 take/attach。
// 必须在 offer 之后调用，不能提前清，否则旧 surfaceDestroyed 收尾期间又能重新挂旧代。
void s2a_direct_vk_on_window_offered(void) {
    pthread_mutex_lock(&g_window_mu);
    struct provider_context *ctx = g_active_context;
    if (ctx && ctx->stopping_engine) {
        // Stop 正在拆引擎：新 surface 先不接，等显式新会话（activate 的 new_session）。
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "direct present: new window generation offered while Stop is retiring; "
                            "attach deferred to the next session");
    } else if (ctx && ctx->revoking) {
        ctx->revoking = 0;
        clear_window_error(ctx);
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "direct present: new window generation offered; attach on next present");
    }
    pthread_mutex_unlock(&g_window_mu);
    ctl_wake();   // owner poll 醒来处理新窗口，不必等下一次 present
}

// 兼容只有“晚挂”入口的旧构建：返回 0 = 已挂上，非 0 = 没有窗口。
int s2a_direct_vk_try_attach_window(void) {
    return s2a_direct_vk_sync_window() == 0 ? 0 : -1;
}

// 窗口撤销（surfaceDestroyed）：surface 的合同要求 revoke 返回后没有线程再用它。
// 必须先在 g_mu 下拆后端交换链并观察最后一帧的 present fence，成功后才 release_window +
// released 归还租约；拿不到呈现退休证明时保留 swapchain/present fence 与租约（不 ACK），
// 由 s2a_wsi_revoke 的超时路径带着 tombstone 结束进程——不能在这里把租约还掉。
void s2a_direct_vk_release_window(void) {
    pthread_mutex_lock(&g_window_mu);
    struct provider_context *ctx = g_active_context;
    if (ctx && ctx->stopping_engine) {
        // Stop/Destroy 正在拆引擎（长收尾不持窗口锁）：这里不并发 detach，也不 ACK 租约；
        // 由 Stop/Destroy 在线程外收尾后统一按退休证明处理。
        static int noted;
        if (noted++ < 4)
            __android_log_print(ANDROID_LOG_INFO, TAG,
                                "direct present: surface revoke while Stop is retiring the engine; "
                                "lease handled by Stop (no concurrent detach)");
        pthread_mutex_unlock(&g_window_mu);
        ctl_wake();
        return;
    }
    // 先发布撤销，再拆交换链：present 线程的 sync 看到 revoking 后不会再 take/attach 本代窗口。
    if (ctx) ctx->revoking = 1;
    if (nvk_session_detach_window()) {
        const char *text = "surface revoked but the last presentation has no retirement proof; "
                           "swapchain/present fence/window lease kept (no ACK)";
        set_window_error(ctx, text);
        __android_log_print(ANDROID_LOG_ERROR, TAG, "direct present: %s", text);
        pthread_mutex_unlock(&g_window_mu);
        ctl_wake();
        return;
    }
    if (ctx && ctx->lease_window) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "direct present: releasing window generation %llu for surface revoke",
                            (unsigned long long)ctx->lease_generation);
        release_lease_window(ctx);
        clear_window_error(ctx);
    }
    pthread_mutex_unlock(&g_window_mu);
    ctl_wake();
}

// F4.1 验收辅助：debug.s2a.diag_manifest=1 时在 activate/destroy 各打一行 revision 清单摘要
// （按文件名排序的 name/size/SHA-256 再取 SHA-256）。两行相同即“启动/补编/停止前后逐文件不变”。
// 正常路径不读文件、不产生开销。
static void diag_log_manifest(const char *label, const char *dir) {
    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.s2a.diag_manifest", v) <= 0 || v[0] != '1') return;
    char digest[65] = "";
    uint32_t files = 0;
    uint64_t bytes = 0;
    char why[192] = "";
    if (s2a_direct_diag_manifest(dir, digest, sizeof digest, &files, &bytes, why, sizeof why) == 0)
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "direct revision manifest %s: files=%u bytes=%llu digest=%s dir=%s",
                            label, files, (unsigned long long)bytes, digest, dir);
    else
        __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】direct revision manifest %s failed: %s (%s)",
                            label, why, dir ? dir : "(null)");
}

// Debug build + existing adb gate + explicit observer opt-in. No environment override.
static int available(void) { return 1; }

static int32_t activate_impl(void *context) {
    struct provider_context *ctx = (struct provider_context *)context;
    if (!ctx) return S2A_DIRECT_RES_ARGUMENT;
    if (ctx->state != S2A_DIRECT_PREPARED) return S2A_DIRECT_RES_STOPPED;
    unsetenv("S2A_DIRECT_DIAG_DIR");  // a failed activation must not inherit the previous session
    if (!g_context.game_data_dir || !g_context.game_data_dir[0]) {
        snprintf(ctx->reason, sizeof ctx->reason, "direct activate needs the revision directory");
        ctx->state = S2A_DIRECT_FAILED;
        ctx->error = S2A_DIRECT_RES_ARGUMENT;
        __android_log_print(ANDROID_LOG_ERROR, TAG, "direct activate refused: %s", ctx->reason);
        return S2A_DIRECT_RES_ARGUMENT;
    }
    // 着色器目录取自本局已激活的 revision：离线库与现场编译请求都从这里读。
    int n = snprintf(ctx->shader_dir, sizeof ctx->shader_dir, "%s/shaders/vulkan/direct", g_context.game_data_dir);
    if (n < 0 || (size_t)n >= sizeof ctx->shader_dir) {
        snprintf(ctx->reason, sizeof ctx->reason, "shader directory path is too long");
        ctx->state = S2A_DIRECT_FAILED;
        ctx->error = S2A_DIRECT_RES_ARGUMENT;
        return S2A_DIRECT_RES_ARGUMENT;
    }
    // 运行时补编（native/nvn/shader_survey.c）用的目录：与本局引擎一致，不另建。
    setenv("S2A_SHADER_DIR", ctx->shader_dir, 1);
    // F5.3：离线库按 <revision>/shaders/vulkan/direct/<abi>/<profileHash>/<identity>.program 定位，
    // 消费端需要 revision 根（S2A_SHADER_DIR 是它的 shaders/vulkan/direct 子目录）。
    setenv("S2A_SHADER_REVISION_ROOT", g_context.game_data_dir, 1);
    // F4.1：现场诊断（编译请求/补编产物/剥离标记）写到本局私有目录，安装 revision 保持只读。
    // 派生失败就不导出该变量：诊断写入会被跳过，绝不回落到 revision。
    {
        char dwhy[192] = "";
        if (s2a_direct_diag_dir(g_context.state_dir, ctx->diag_dir, sizeof ctx->diag_dir, dwhy, sizeof dwhy) == 0) {
            setenv("S2A_DIRECT_DIAG_DIR", ctx->diag_dir, 1);
        } else {
            __android_log_print(ANDROID_LOG_WARN, TAG,
                                "direct activate: no private diagnostics directory (%s); compile diagnostics are not "
                                "written and the revision is never used as a fallback", dwhy);
        }
    }
    // F5.1：跨局编译结果缓存。与诊断目录分开（可复用 vs 可弃），派生失败就整体关闭缓存。
    {
        char cwhy[192] = "";
        if (s2a_program_cache_dir(g_context.state_dir, ctx->cache_dir, sizeof ctx->cache_dir,
                                  cwhy, sizeof cwhy) == 0) {
            setenv("S2A_DIRECT_CACHE_DIR", ctx->cache_dir, 1);
            __android_log_print(ANDROID_LOG_INFO, TAG, "direct activate: program cache directory %s", ctx->cache_dir);
        } else {
            __android_log_print(ANDROID_LOG_WARN, TAG,
                                "direct activate: no cross-session program cache (%s); every launch recompiles", cwhy);
        }
    }
    diag_log_manifest("activate", ctx->shader_dir);
    { char v[PROP_VALUE_MAX] = {0}; if (__system_property_get("debug.s2a.strip_interlock", v) > 0 && v[0] == '1') setenv("S2A_DIRECT_STRIP_INTERLOCK", "1", 1); }
    char why[192] = "";
    // 回放会话接管 prepare 建好的设备（契约 A2：后端不另建设备）
    if (nvk_session_open(&ctx->device, ctx->cache_dir[0] ? ctx->cache_dir : NULL, why, sizeof why)) {
        snprintf(ctx->reason, sizeof ctx->reason, "nvk session open failed: %s", why[0] ? why : "unknown");
        ctx->state = S2A_DIRECT_FAILED;
        ctx->error = S2A_DIRECT_RES_DRIVER;
        __android_log_print(ANDROID_LOG_ERROR, TAG, "direct activate refused: %s", ctx->reason);
        return S2A_DIRECT_RES_DRIVER;
    }
    // 窗口：从外壳的 WSI lease 取当前 generation。拿不到不算激活失败（会话可以无窗口跑），
    // mtl_present 的 sync 路径会继续要；租约持有到窗口撤销或 Stop，由同一套归还路径收尾。
    {
        pthread_mutex_lock(&g_window_mu);
        int wrc = atomic_load(&g_stop_published) ? 1 : take_and_attach_window_locked(ctx, -1);
        pthread_mutex_unlock(&g_window_mu);
        if (wrc > 0)
            __android_log_print(ANDROID_LOG_WARN, TAG, "direct activate: no window offered by the WSI lease yet");
        else if (wrc < 0)
            __android_log_print(ANDROID_LOG_ERROR, TAG, "direct activate: %s; present will retry the lease",
                                ctx->window_error[0] ? ctx->window_error : "window attach failed");
    }
    // Published by activate() under the same lock as Stop.
    // Activate 之后冻结登记：本局用哪个编译器从此不能再被换掉（W2.6 的身份前提）。
    // 没有登记时只告警：诊断构建（无 managed 服务）本来就绑不上，不能因此拒绝激活。
    {
        int32_t freeze = s2a_direct_freeze_compiler_v1(ctx);
        __android_log_print(ANDROID_LOG_INFO, TAG, "direct activate: unified compile fn %s (frozen=%d)",
                            s2a_direct_bound_compile(ctx) ? "bound" : "missing", (int)freeze);
        if (freeze != S2A_DIRECT_RES_OK)
            __android_log_print(ANDROID_LOG_WARN, TAG,
                                "direct activate: compiler binding not frozen (rc=%d); "
                                "compile paths will report the missing compiler", (int)freeze);
    }
    snprintf(ctx->reason, sizeof ctx->reason, "nvk backend active on %s (window=%d)", ctx->device.driver_name, ctx->has_window);
    __android_log_print(ANDROID_LOG_INFO, TAG, "direct activate: nvk backend ready, window=%d current_hz=%.3f",
                        ctx->has_window, ctx->has_window ? s2a_direct_vk_display_refresh() : 0.0);
    return S2A_DIRECT_RES_OK;
}


// Publish activation ownership before touching the device. Stop can retain it
// without waiting for a blocked activation or mistaking it for an idle device.
static int32_t activate(void *context) {
    struct provider_context *ctx = context;
    if (!ctx) return S2A_DIRECT_RES_ARGUMENT;
    pthread_mutex_lock(&g_window_mu);
    if (atomic_load(&g_stop_published) || ctx->activation_started || ctx->state != S2A_DIRECT_PREPARED) {
        pthread_mutex_unlock(&g_window_mu); return S2A_DIRECT_RES_STOPPED;
    }
    ctx->activation_started = 1;
    pthread_mutex_unlock(&g_window_mu);
    int32_t rc = activate_impl(context);
    pthread_mutex_lock(&g_window_mu);
    ctx->activation_started = 0;
    if (rc == S2A_DIRECT_RES_OK) ctx->engine_ok = 1;
    if (atomic_load(&g_stop_published)) {
        ctx->revoking = ctx->stopping_engine = 1;
        ctx->state = S2A_DIRECT_FAILED;
        ctx->error = S2A_DIRECT_RES_RETIREMENT;
        ctx->blockers = S2A_DIRECT_BLOCK_PRESENT;
        g_retained_context = ctx;
        pthread_mutex_unlock(&g_window_mu);
        return S2A_DIRECT_RES_RETIREMENT;
    }
    if (rc == S2A_DIRECT_RES_OK) {
        ctx->state = S2A_DIRECT_ACTIVE;
        ctx->blockers = 0;
        atomic_store(&g_session_active, 1);
    }
    pthread_mutex_unlock(&g_window_mu);
    ctl_wake();
    return rc;
}

static int32_t request_stop_impl(void *context) {
    struct provider_context *ctx = context;
    if (!ctx) return S2A_DIRECT_RES_ARGUMENT;
    // Close shader admission first; a resolver already admitted stays counted.
    nvk_session_request_stop();
    atomic_store(&g_stop_published, 1);
    atomic_store(&g_session_active, 0);
    ctl_wake_stop();
    if (stop_window_lock()) return S2A_DIRECT_RES_RETIREMENT;
    if (ctx->activation_started) {
        g_retained_context = ctx;
        pthread_mutex_unlock(&g_window_mu);
        (void)nvk_session_wait_compiles();
        __android_log_print(ANDROID_LOG_INFO, TAG, "direct stop: activation in flight; session/device retained");
        return S2A_DIRECT_RES_RETIREMENT;
    }
    if (ctx->engine_ok) {
        // No frontend/guest quiescence proof exists here. Keep the entire
        // session, command buffers, WSI lease and device alive until exit.
        ctx->revoking = 1;
        ctx->stopping_engine = 1;
        ctx->state = S2A_DIRECT_FAILED;
        ctx->error = S2A_DIRECT_RES_RETIREMENT;
        ctx->blockers = S2A_DIRECT_BLOCK_PRESENT;
        snprintf(ctx->reason, sizeof ctx->reason,
                 "frontend retirement unproven; session and window lease retained until process exit");
        set_window_error(ctx, ctx->reason);
        g_retained_context = ctx;
        pthread_mutex_unlock(&g_window_mu);
        (void)nvk_session_wait_compiles();
        __android_log_print(ANDROID_LOG_INFO, TAG, "direct stop: %s", ctx->reason);
        return S2A_DIRECT_RES_RETIREMENT;
    }
    if (ctx->state == S2A_DIRECT_FAILED) {
        int32_t error = ctx->error ? ctx->error : S2A_DIRECT_RES_INTERNAL;
        pthread_mutex_unlock(&g_window_mu);
        return error;
    }
    ctx->state = S2A_DIRECT_STOPPED;
    ctx->blockers = 0;
    pthread_mutex_unlock(&g_window_mu);
    return S2A_DIRECT_RES_OK;
}

static int32_t request_stop(void *context) {
    int32_t rc = request_stop_impl(context);
    if (rc == S2A_DIRECT_RES_OK) { int e = errno; unsetenv("S2A_DIRECT_DIAG_DIR"); errno = e; }
    return rc;
}

static int32_t query(void *context, s2a_direct_status_v1 *out) {
    struct provider_context *ctx = (struct provider_context *)context;
    if (!ctx || !out) return S2A_DIRECT_RES_ARGUMENT;
    if (out->size != S2A_DIRECT_STATUS_SIZE || out->version != S2A_DIRECT_ABI) return S2A_DIRECT_RES_VERSION;
    out->state = ctx->state;
    out->blockers = ctx->blockers;
    out->error = ctx->error;
    return S2A_DIRECT_RES_OK;
}

// owner 控制片（契约：主动等待最多一个 10 ms 片）。managed owner 循环的真实调用点：
//   * Stop 已发布：立即 STOPPED，不再驱动正在收尾的 worker/窗口；
//   * 需求编译在途：最多等一个控制片，返回 PENDING（不是失败）；
//   * 执行包正挂起等待编译：owner 用这个片推进窗口生命周期（撤销/换代/旋转重建），
//     这正是“挂起期间确实处理到 Stop/窗口事件”的生产接点；
//   * 空闲：在控制条件变量上睡满剩余预算，避免 managed 循环空转；activate/Stop/窗口
//     offer/revoke 都会唤醒。
// 不在这里等待编译 join，也不改变 draw 的等待语义（draw 仍自己等结果；真正的 park/resume
// 需要热路径接线，见报告）。
static int32_t poll(void *context, uint32_t budget_ms) {
    // 先看 Stop 标记再解引用 context：Destroy 之后即使有迟到 poll，也不能碰已释放的内存。
    if (atomic_load(&g_stop_published)) {
        atomic_fetch_add(&g_owner_stop_seen, 1);
        return S2A_DIRECT_RES_STOPPED;
    }
    struct provider_context *ctx = (struct provider_context *)context;
    if (!ctx) return S2A_DIRECT_RES_ARGUMENT;
    if (!atomic_load(&g_session_active)) return S2A_DIRECT_RES_STOPPED;
    switch (ctx->state) {
        case S2A_DIRECT_ACTIVE:
            break;
        case S2A_DIRECT_STOPPING:
        case S2A_DIRECT_STOPPED:
            atomic_fetch_add(&g_owner_stop_seen, 1);
            return S2A_DIRECT_RES_STOPPED;
        case S2A_DIRECT_FAILED:
            return ctx->error ? ctx->error : S2A_DIRECT_RES_INTERNAL;
        default:
            return S2A_DIRECT_RES_STOPPED;
    }
    if (budget_ms > 10) budget_ms = 10;   // 契约：单次主动等待最多 10 ms
    const int64_t deadline = ctl_now_ns() + (int64_t)budget_ms * 1000000ll;
    atomic_fetch_add(&g_owner_slices, 1);

    int pending = 0;
    // 1) 需求 worker：有在途任务时等到完成或控制片结束；没有 worker 不是连接失败。
    {
        int64_t left = deadline - ctl_now_ns();
        int ms = left > 0 ? (int)(left / 1000000ll) : 0;
        int p = s2a_compile_worker_pump((uint32_t)ms);
        if (p > 0) pending = 1;
    }
    // 2) 窗口生命周期：还没有窗口时由 owner 片去要（上屏路径不再兼做这件事）
    if (!ctx->has_window && !ctx->revoking) {
        int64_t wl = deadline - ctl_now_ns();
        if (owner_window_control(ctx, wl > 0 ? wl : 0) == 0) atomic_fetch_add(&g_owner_window_actions, 1);
    }
    // 3) 没有在途工作：睡满剩余控制片；有工作但窗口推进已用完预算也小睡，避免忙转。
    int64_t left = deadline - ctl_now_ns();
    if (left > 0) ctl_wait_until(deadline);
    if (atomic_load(&g_stop_published)) {
        atomic_fetch_add(&g_owner_stop_seen, 1);
        return S2A_DIRECT_RES_STOPPED;
    }
    return pending ? S2A_DIRECT_RES_PENDING : S2A_DIRECT_RES_OK;
}

// vkDeviceWaitIdle has no timeout parameter. The worker owns its device/context
// until it returns; the caller never frees either on deadline. No cancellation.
static void *device_idle_worker(void *arg) {
    struct provider_context *ctx = arg;
    // The binding registry also has a mutex; do not block the stop caller on it.
    uint64_t start = ctx->idle_deadline - NVK_SHUTDOWN_BUDGET_NS;
    atomic_store_explicit(&ctx->idle_phase, 1, memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk close enter compiler unbind: elapsed=%llums", (unsigned long long)(((uint64_t)ctl_now_ns() - start) / 1000000ull));
    int unbind = s2a_direct_unbind_compiler_v1(ctx);
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk close exit compiler unbind: elapsed=%llums rc=%d", (unsigned long long)(((uint64_t)ctl_now_ns() - start) / 1000000ull), unbind);
    atomic_store_explicit(&ctx->idle_phase, 2, memory_order_release);
    if (unbind != S2A_DIRECT_RES_OK) ctx->idle_result = VK_ERROR_UNKNOWN;
    else if ((uint64_t)ctl_now_ns() >= ctx->idle_deadline) ctx->idle_result = VK_TIMEOUT;
    else if (!ctx->device_ok) ctx->idle_result = VK_SUCCESS;
    else {
        __android_log_print(ANDROID_LOG_INFO, TAG, "nvk close enter device idle: elapsed=%llums", (unsigned long long)(((uint64_t)ctl_now_ns() - start) / 1000000ull));
        ctx->idle_result = s2a_vk_device_wait_idle(&ctx->device);
        __android_log_print(ANDROID_LOG_INFO, TAG, "nvk close exit device idle: elapsed=%llums rc=%d", (unsigned long long)(((uint64_t)ctl_now_ns() - start) / 1000000ull), ctx->idle_result);
    }
    atomic_store_explicit(&ctx->idle_done, 1, memory_order_release);
    return NULL;
}
static int device_idle_bounded(struct provider_context *ctx, uint64_t deadline) {
    uint64_t start = deadline - NVK_SHUTDOWN_BUDGET_NS;
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk close enter device retirement: elapsed=%llums", (unsigned long long)((ctl_now_ns() - (int64_t)start) / 1000000ll));
    if (!ctx->idle_started) {
        if ((uint64_t)ctl_now_ns() >= deadline) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk close device idle: deadline before start (device retained)"); return -1;
        }
        ctx->idle_deadline = deadline;
        atomic_store_explicit(&ctx->idle_done, 0, memory_order_relaxed);
        pthread_attr_t attr;
        int e = pthread_attr_init(&attr);
        if (e) { __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk close device idle: thread attr %d (device retained)", e); return -1; }
        e = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (!e) e = pthread_create(&ctx->idle_thread, &attr, device_idle_worker, ctx);
        pthread_attr_destroy(&attr);
        if (e) { __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk close device idle: thread create %d (device retained)", e); return -1; }
        ctx->idle_started = 1;
    }
    while (!atomic_load_explicit(&ctx->idle_done, memory_order_acquire)) {
        int64_t n = ctl_now_ns();
        if ((uint64_t)n >= deadline) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk close device retirement timeout: elapsed=%llums stage=%s (thread/device retained)", (unsigned long long)((n - (int64_t)start) / 1000000ll), atomic_load_explicit(&ctx->idle_phase, memory_order_acquire) == 0 ? "worker start" : atomic_load_explicit(&ctx->idle_phase, memory_order_acquire) == 1 ? "compiler unbind" : "device idle");
            return -1;
        }
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    }
    if ((uint64_t)ctl_now_ns() >= deadline) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk close device idle: completion after deadline (device retained)"); return -1;
    }
    // The detached worker performs no context access after publishing idle_done.
    ctx->idle_started = 0;
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk close exit device retirement: elapsed=%llums rc=%d", (unsigned long long)((ctl_now_ns() - (int64_t)start) / 1000000ll), ctx->idle_result);
    return ctx->idle_result == VK_SUCCESS ? 0 : -1;
}

static int32_t destroy_impl(void *context) {
    struct provider_context *ctx = (struct provider_context *)context;
    if (!ctx) return S2A_DIRECT_RES_ARGUMENT;
    nvk_session_request_stop();
    // Destroy 之后 context 释放：任何迟到的 owner poll 必须立刻看到 STOPPED，不能再碰 ctx。
    atomic_store(&g_stop_published, 1);
    atomic_store(&g_session_active, 0);
    ctl_wake_stop();
    if (stop_window_lock()) return S2A_DIRECT_RES_RETIREMENT;
    if (ctx->activation_started) {
        g_retained_context = ctx;
        pthread_mutex_unlock(&g_window_mu);
        (void)nvk_session_wait_compiles();
        __android_log_print(ANDROID_LOG_INFO, TAG, "direct destroy: activation in flight; session/device retained");
        return S2A_DIRECT_RES_RETIREMENT;
    }
    ctx->revoking = 1;
    if (ctx->engine_ok) {
        ctx->stopping_engine = 1;
        g_retained_context = ctx;
        pthread_mutex_unlock(&g_window_mu);
        (void)nvk_session_wait_compiles();
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "direct destroy: frontend retirement unproven; session/device retained until process exit");
        return S2A_DIRECT_RES_RETIREMENT;
    }
    // No active engine: no replay can still reference the compiler binding.
    g_retained_context = ctx; // 等待期间也不允许新 prepare 覆盖 owner。
    pthread_mutex_unlock(&g_window_mu);
    uint64_t deadline = nvk_session_stop_deadline();
    if (!deadline || device_idle_bounded(ctx, deadline)) return S2A_DIRECT_RES_RETIREMENT;
    if (ctx->device_ok) {
        if (s2a_vk_device_destroy(&ctx->device, 1)) return S2A_DIRECT_RES_RETIREMENT;
        ctx->device_ok = 0;
    }
    if (stop_window_lock()) return S2A_DIRECT_RES_RETIREMENT;
    if (g_active_context == ctx) g_active_context = NULL;
    if (g_retained_context == ctx) g_retained_context = NULL;
    pthread_mutex_unlock(&g_window_mu);
    free(ctx);
    return S2A_DIRECT_RES_OK;
}

static int32_t destroy(void *context) {
    struct provider_context *ctx = context;
    if (!ctx) return S2A_DIRECT_RES_ARGUMENT;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&ctx->destroying, &expected, 1)) return S2A_DIRECT_RES_RETIREMENT;
    int32_t rc = destroy_impl(ctx);
    if (rc != S2A_DIRECT_RES_OK) atomic_store(&ctx->destroying, 0); // successful destroy already freed ctx
    return rc;
}

static int32_t prepare(const struct s2a_graphics_descriptor_v1 *descriptor, uint64_t session,
                       const char *state_dir, void *user, struct s2a_direct_api_v1 *table,
                       char effective_profile_hash[72], struct s2a_direct_counts *counts,
                       char *why, size_t why_size) {
    (void)user;
    if (!descriptor || !session || !table || !effective_profile_hash || !counts || !state_dir)
        return S2A_DIRECT_RES_ARGUMENT;
    if (descriptor->direct_abi != S2A_DIRECT_ABI) return S2A_DIRECT_RES_VERSION;
#ifndef S2A_ANDROID_VK_BACKEND_NVN_VK
    // 旧引擎没有现场 profile 合成；空库不允许读取空数组。
    if (descriptor->profile_count == 0 || !descriptor->profile_hashes[0][0]) {
        snprintf(why, why_size, "descriptor has no profile");
        return S2A_DIRECT_RES_IDENTITY;
    }
#endif
    pthread_mutex_lock(&g_window_mu);
    int retained = g_retained_context != NULL;
    pthread_mutex_unlock(&g_window_mu);
    if (retained) {
        snprintf(why, why_size, "previous Vulkan device retirement incomplete; refusing new session");
        return S2A_DIRECT_RES_RETIREMENT;
    }
    struct provider_context *ctx = (struct provider_context *)calloc(1, sizeof *ctx);
    if (!ctx) {
        snprintf(why, why_size, "provider context allocation failed");
        return S2A_DIRECT_RES_INTERNAL;
    }
    char device_why[192] = "";
    if (s2a_vk_device_create(&ctx->device, device_why, sizeof device_why)) {
        snprintf(why, why_size, "Vulkan device: %s", device_why[0] ? device_why : "unknown");
        if (s2a_vk_device_destroy(&ctx->device, 0)) {
            pthread_mutex_lock(&g_window_mu);
            g_retained_context = ctx;
            pthread_mutex_unlock(&g_window_mu);
            return S2A_DIRECT_RES_RETIREMENT;
        }
        free(ctx);
        return S2A_DIRECT_RES_DRIVER;
    }
    ctx->device_ok = 1;
    if (!s2a_graphics_storage_admit(ctx->device.core_features.vertexPipelineStoresAndAtomics,
                                   descriptor->storage_write_stages)) {
        snprintf(why, why_size, "package writeStages=0x%x requires vertexPipelineStoresAndAtomics",
                 descriptor->storage_write_stages);
        if (s2a_vk_device_destroy(&ctx->device, 0)) {
            pthread_mutex_lock(&g_window_mu);
            g_retained_context = ctx;
            pthread_mutex_unlock(&g_window_mu);
            return S2A_DIRECT_RES_RETIREMENT;
        }
        free(ctx);
        return S2A_DIRECT_RES_UNSUPPORTED;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "direct storage requirements: writeStages=0x%x coverage=%s",
                        descriptor->storage_write_stages,
                        descriptor->storage_coverage_complete ? "complete" : "unknown");
    ctx->session = session;
    // No provider table has been published yet; Stop cannot race this reset.
    if (nvk_session_prepare()) {
        snprintf(why, why_size, "previous shader resolution is still in flight");
        if (s2a_vk_device_destroy(&ctx->device, 0)) {
            pthread_mutex_lock(&g_window_mu); g_retained_context = ctx; pthread_mutex_unlock(&g_window_mu);
            return S2A_DIRECT_RES_RETIREMENT;
        }
        free(ctx); return S2A_DIRECT_RES_RETIREMENT;
    }
    // 新会话：清零 owner 控制统计，Stop 标记回到未发布（activate 后才允许 poll 工作）。
    atomic_store(&g_stop_published, 0);
    atomic_store(&g_session_active, 0);
    atomic_store(&g_owner_slices, 0);
    atomic_store(&g_owner_stop_seen, 0);
    pthread_mutex_lock(&g_window_mu);
    g_active_context = ctx;
    pthread_mutex_unlock(&g_window_mu);
    atomic_store(&g_owner_suspended_slices, 0);
    atomic_store(&g_owner_window_actions, 0);
    // 呈现退休证明：有 VK_EXT_swapchain_maintenance1 用 present fence；没有（安卓 13 及以下的系统
    // libvulkan 不补，#426）由交换链改用同图像再 acquire 的 fence 证明（nvn_vk_swapchain.c）。
    // 两种都不拿渲染 fence 冒充呈现退休，所以这里不再拒绝 direct，只记下走的是哪种。
    __android_log_print(ANDROID_LOG_INFO, TAG, "direct presentation retirement: %s",
                        ctx->device.swapchain_maintenance1_enabled
                            ? "present fence (VK_EXT_swapchain_maintenance1)"
                            : "same-image reacquire (no VK_EXT_swapchain_maintenance1)");
    ctx->state = S2A_DIRECT_PREPARED;
    ctx->blockers = S2A_DIRECT_BLOCK_ALL;
    ctx->generation = 1;   // provider 上下文代际：Stop/重建才有新值；transport 身份，不参与内容键
    ctx->direct_abi = descriptor->direct_abi;
    snprintf(ctx->closure, sizeof ctx->closure, "%s", descriptor->translator_closure);
    snprintf(ctx->reason, sizeof ctx->reason, "prepared with %s", ctx->device.driver_name);
    // enabled profile 必须来自本设备实测能力：compiler 侧的 layout hash/ABI/profileHash 由 managed 合成。
    // 合成失败就是 prepare 失败（AUTO 在 commit 前转 generic，显式 direct 直接报红），
    // 不能拿 descriptor 里离线目标的 profileHash 冒充本机 effective profile。
    char profile_why[480] = "";
    {
        // 完整 caps 只在这里打一次：profile 合成的输入必须能被主机用同一条 CLI 复算，
        // 截断日志不能作为跨端一致性的证据。
        // logcat 单条 ~1 KiB 会截断，分片打；主机按 [i/n] 顺序拼回同一份 JSON 再复算 profile。
        static char caps_json[8192];
        size_t caps_len = 0;
        if (s2a_vk_device_caps_json(&ctx->device, caps_json, sizeof caps_json, &caps_len) == 0) {
            const size_t chunk = 700;
            unsigned parts = (unsigned)((caps_len + chunk - 1) / chunk);
            for (unsigned i = 0; i < parts; i++) {
                size_t off = (size_t)i * chunk;
                size_t n = caps_len - off;
                if (n > chunk) n = chunk;
                __android_log_print(ANDROID_LOG_INFO, TAG, "direct device caps[%u/%u]:%.*s",
                                    i + 1, parts, (int)n, caps_json + off);
            }
        } else {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】direct device caps: 生成失败");
        }
    }
    if (s2a_direct_build_profile(&ctx->device, descriptor->translator_closure,
                                 ctx->profile_json, sizeof ctx->profile_json, &ctx->profile_bytes,
                                 ctx->profile_hash, profile_why, sizeof profile_why)) {
        // 停服原因这条路会截断长文本（观测到 ~220 字节），而身份不匹配的取证需要两边完整
        // closure：这里按 logcat 单条上限分片直出，界面提示仍走 why。
        {
            const char *text = profile_why[0] ? profile_why : "composition failed";
            const size_t n = strlen(text);
            const size_t chunk = 700;
            unsigned parts = (unsigned)((n + chunk - 1) / chunk);
            if (parts == 0) parts = 1;
            for (unsigned i = 0; i < parts; i++) {
                size_t off = (size_t)i * chunk;
                size_t len = n - off;
                if (len > chunk) len = chunk;
                __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】direct enabled profile[%u/%u]:%.*s",
                                    i + 1, parts, (int)len, text + off);
            }
        }
        snprintf(why, why_size, "enabled profile: %s", profile_why[0] ? profile_why : "composition failed");
        pthread_mutex_lock(&g_window_mu);
        if (g_active_context == ctx) g_active_context = NULL;
        pthread_mutex_unlock(&g_window_mu);
        if (s2a_vk_device_destroy(&ctx->device, 0)) {
            pthread_mutex_lock(&g_window_mu);
            g_retained_context = ctx;
            pthread_mutex_unlock(&g_window_mu);
            return S2A_DIRECT_RES_RETIREMENT;
        }
        free(ctx);
        return S2A_DIRECT_RES_UNSUPPORTED;
    }
    snprintf(effective_profile_hash, 72, "%s", ctx->profile_hash);
    ctx->offline_profile_available = s2a_graphics_has_profile(descriptor, ctx->profile_hash);
    {
        // profile 字节的摘要：主机用同一 caps 跑 CLI 必须得到同一长度/摘要。
        uint8_t digest[32];
        char hex[65];
        s2a_sha256 ctx_hash;
        s2a_sha256_init(&ctx_hash);
        s2a_sha256_update(&ctx_hash, ctx->profile_json, ctx->profile_bytes);
        s2a_sha256_final(&ctx_hash, digest);
        s2a_sha256_hex(digest, hex);
        __android_log_print(ANDROID_LOG_INFO, TAG, "direct enabled profile: %zuB sha256=%s hash=%s closure=%s",
                            ctx->profile_bytes, hex, ctx->profile_hash, descriptor->translator_closure);
        // profile 字节也分片打：主机可逐字节比对"同一份 caps → 同一份 profile"，而不是只信两个摘要。
        unsigned parts = (unsigned)((ctx->profile_bytes + 700 - 1) / 700);
        for (unsigned i = 0; i < parts; i++) {
            size_t off = (size_t)i * 700;
            size_t n = ctx->profile_bytes - off;
            if (n > 700) n = 700;
            __android_log_print(ANDROID_LOG_INFO, TAG, "direct profile json[%u/%u]:%.*s",
                                i + 1, parts, (int)n, ctx->profile_json + off);
        }
    }
    memset(table, 0, sizeof *table);
    table->size = S2A_DIRECT_TABLE_SIZE;
    table->version = S2A_DIRECT_ABI;
    table->session = session;
    table->context = ctx;
    table->activate = activate;
    table->request_stop = request_stop;
    table->query = query;
    table->poll = poll;
    table->destroy = destroy;
    counts->devices = 1;
    counts->queues = 1;
    counts->workers = 0;
    counts->consumers = 0;
    __android_log_print(ANDROID_LOG_INFO, TAG, "direct provider prepared: driver=%s api=%u.%u.%u profile=%.19s",
                        ctx->device.driver_name, VK_API_VERSION_MAJOR(ctx->device.properties.apiVersion),
                        VK_API_VERSION_MINOR(ctx->device.properties.apiVersion),
                        VK_API_VERSION_PATCH(ctx->device.properties.apiVersion), effective_profile_hash);
    // READY admission 的 GPU 身份门找的是 generic VulkanRenderer 的 PrintGpuInformation 行。
    // 这行本身必须证明直连 prepare 走完：只能打在上面两道拒绝闸（storage 能力 /
    // profile 合成）全部通过之后、返回 OK 之前；否则被拒时日志里
    // 仍有身份行，AUTO 转 generic 再失败会被这行误判 READY 通过（假绿）。
    // 值只来自本机 VkPhysicalDeviceProperties / driverProperties。
    {
        char identity[512];
        if (s2a_vk_identity_line(ctx->device.properties.deviceName,
                                 ctx->device.properties.apiVersion,
                                 s2a_vk_vendor_name(ctx->device.properties.vendorID),
                                 ctx->device.driver_info,
                                 identity, sizeof identity) == 0)
            fprintf(stderr, "[s2a] %s\n", identity);
    }
    return S2A_DIRECT_RES_OK;
}

static const struct s2a_direct_provider g_provider = {
    "android-vulkan-direct", available, prepare, NULL,
};

const struct s2a_direct_provider *s2a_direct_provider_default(void) { return &g_provider; }

// runtime.c 的弱符号由这里补齐：只有共享 NVN 前端真的链接进来时才算 ready（不做编译期假设）。
int s2a_direct_frontend_ready(void) {
    return h_nvnBootstrapLoader("nvnDeviceGetInteger") != NULL;
}
