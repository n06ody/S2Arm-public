#include "nvk_session.h"
#include "nvn_backend.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "rp_exec.h"
#include "direct/vk_device.h"
#include "nvn/nvn_warm.h"   // 预热调度：会话 open 后起、Stop 前收（nvn_warm.h 合同）
#include "nvn/nvn_vk_pipe_demand.h"   // 首用管线后台构建：会话 open 后起、Stop 前收（nvn_vk_pipe_demand.h 合同）
#include <android/log.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "S2Arm"
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;
static _Atomic uint64_t g_stop_started;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void nvk_session_request_stop(void) {
    uint64_t expected = 0, now = mono_ns();
    atomic_compare_exchange_strong_explicit(&g_stop_started, &expected, now, memory_order_acq_rel, memory_order_acquire);
    // 首用管线后台构建先收（有界 join：丢弃未开工队列、只等当前一条建完），且必须在 nvk_shader_stop 之前：
    // 该线程要借用程序、调 nvn_gpu_pipe_get，着色器准入还开着才借得到。
    nvn_vk_pipe_demand_stop();
    nvk_shader_stop();
    // 预热线程在着色器准入关闭之后收：放在 nvk_shader_stop 之后是硬要求——准入关了新工作才会被
    // 各 owner 拒绝；迟到的结果由 owner 丢弃。
    // nvk_warm_stop 有界（≤500 毫秒统一截止）且**不做文件 IO**：清单落盘归预热线程自己（没有预热线程时
    // 归一次性 saver），这里只等「静默 + 落盘完成」。它占的是本会话 700 毫秒硬界限里的那一段，所以
    // 非 0 必须在日志里点名，收尾超时的读数才归得了因。
    if (nvk_warm_stop() != 0)
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "【红】nvk stop: warm 预热的静默/清单落盘没在 500 毫秒统一截止内完成"
                            "（本项超时；清单没写盘、旧文件保留；占的是会话 700 毫秒硬界限）");
}
uint64_t nvk_session_stop_deadline(void) {
    uint64_t start = atomic_load_explicit(&g_stop_started, memory_order_acquire);
    return start ? start + NVK_SHUTDOWN_BUDGET_NS : 0;
}
// Called while preparing an unpublished context, never concurrently with Stop.
int nvk_session_prepare(void) {
    if (nvk_shader_open()) return -1;
    atomic_store_explicit(&g_stop_started, 0, memory_order_release);
    return 0;
}
int nvk_session_wait_compiles(void) {
    uint64_t deadline = nvk_session_stop_deadline();
    uint64_t start = deadline - NVK_SHUTDOWN_BUDGET_NS;
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk stop enter shader wait: elapsed=%llums in_flight=%u", (unsigned long long)((mono_ns() - start) / 1000000ull), nvk_shader_inflight());
    while (nvk_shader_inflight()) {
        uint64_t n = mono_ns();
        if (n >= deadline) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk stop shader wait timeout: elapsed=%llums in_flight=%u (session retained)", (unsigned long long)((n - start) / 1000000ull), nvk_shader_inflight());
            return -1;
        }
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk stop exit shader wait: elapsed=%llums in_flight=0", (unsigned long long)((mono_ns() - start) / 1000000ull));
    return 0;
}
static void *g_window; static uint64_t g_window_gen; static int32_t g_win_w, g_win_h;
static char g_cache_dir[512];
static nvn_gpu_surface g_surface;

double s2a_direct_vk_display_refresh(void);   // provider_nvk.c：窗口租约公布的当前刷新率

const char *nvk_session_cache_dir(void) { return g_cache_dir[0] ? g_cache_dir : NULL; }

int nvk_session_open(struct s2a_vk_device *dev, const char *cache_dir, char *why, size_t why_size) {
    pthread_mutex_lock(&g_mu);
    if (g_ready) { pthread_mutex_unlock(&g_mu); return 0; }
    snprintf(g_cache_dir, sizeof g_cache_dir, "%s", cache_dir ? cache_dir : "");
    nvn_gpu_open_desc d = {0}; d.host_device = dev; d.cache_dir = nvk_session_cache_dir(); d.app_label = "s2arm";
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_open(&d, &err);
    if (!nvn_gpu_ok(rc)) {
        if (why && why_size) snprintf(why, why_size, "nvn_gpu_open %d: %s", rc.code, err.why);
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    nvn_unimpl_reset();
    // 写跟踪的保护分块：64 页（256 KB）。3D 那款场景里（同一手机，11-11 量，每帧约 2000 笔）：
    //   逐 4 KB 页  每帧缺页 3240 次、改保护 145 毫秒、回放 83 毫秒，7.7 帧
    //   4 页        819 次、31 毫秒、70 毫秒，8.4 帧
    //   16 页       207 次、12 毫秒、37 毫秒，10.9 帧
    //   64 页       54 次、2 毫秒、34 毫秒，11.1 帧
    // 块越大一次缺页后要重传的字节越多，但一次缺页加两次改保护的代价抵得上拷两百多 KB。debug.s2a.nvk_chunk_pages 可改（0 = 逐页）。
    { extern void nvn_dirty_set_chunk_pages(uint32_t); extern int nvn_dirty_log_configure(int); extern int nvn_plat_flag(const char *, int);
      int cp = nvn_plat_flag("nvk_chunk_pages", 64);
      nvn_dirty_set_chunk_pages(cp > 0 ? (uint32_t)cp : 0); __android_log_print(ANDROID_LOG_INFO, TAG, "nvk session: 写跟踪保护分块 %d 页", cp);
      // 脏页追加日志（#1501 A②，缺省开，debug.s2a.nvk_buf_dirtylog=0 关）：必须在登记任何池之前配置；回放核心按同一开关决定用不用
      if (nvn_plat_flag("nvk_buf_dirtylog", 1) && !nvn_dirty_log_configure(1))
          __android_log_print(ANDROID_LOG_WARN, TAG, "【黄】脏页追加日志未启用：已有登记池或所需原子操作非无锁，继续逐页核对"); }
    g_ready = 1;
    nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
    const char *drv_name = dev && dev->driver_name[0] ? dev->driver_name : "?";
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk session: backend open on %s (api %u, caps %#x, 单次分配上限 %llu, SSBO 范围上限 %llu, driver=%s driverID=%d)",
                        caps.name, caps.api_version, caps.caps,
                        (unsigned long long)caps.max_buf_bytes, (unsigned long long)caps.max_ssbo_range,
                        drv_name, dev ? dev->driver_id : -1);
    pthread_mutex_unlock(&g_mu);
    // 开局预热（iOS 蓝本：nvn_mtl.m warm_start 在 shader map 读完就起，后台把上一局清单建出来）。
    // 放在解锁之后：预热线程会取会话缓存目录 / 后端锁，不在 g_mu 里起线程。
    nvk_warm_start();
    nvn_vk_pipe_demand_start();
    return 0;
}

// 会话保留到进程退出时的管线缓存落盘（#860 / #907）：取数据在本线程，写盘在后端的后台线程，不拖停局。
void nvk_session_flush_pipe_cache(void) {
    extern void nvn_gpu_vk_pipe_cache_flush(void);
    if (!nvk_session_ready()) return;
    nvn_gpu_vk_pipe_cache_flush();
}

int nvk_session_ready(void) { pthread_mutex_lock(&g_mu); int r = g_ready; pthread_mutex_unlock(&g_mu); return r; }

// 收局汇总：这一局撞到的全部「未接 / 不支持」，一项一行。普查工具按 "nvk unimpl" 前缀收。
static void dump_unimpl(void) {
    static char buf[16384];
    size_t n = nvn_unimpl_dump(buf, sizeof buf);
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk unimpl summary: %d kinds", nvn_unimpl_kinds());
    for (char *p = buf, *e; n && (e = strchr(p, '\n')); p = e + 1) { *e = 0; __android_log_print(ANDROID_LOG_INFO, TAG, "nvk unimpl\t%s", p); }
}

int nvk_session_close(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_ready) { pthread_mutex_unlock(&g_mu); return 0; }
    dump_unimpl();
    rp_present_set_surface(NULL, 0); g_surface = NULL;   // 还接着的面由 nvn_gpu_close 在在途段清空后收掉
    { uint64_t sh = 0, dp = 0; rp_present_stats(&sh, &dp); __android_log_print(ANDROID_LOG_INFO, TAG, "nvk session: presents shown=%llu dropped=%llu", (unsigned long long)sh, (unsigned long long)dp); }
    nvn_vk_pipe_demand_stop();   // 幂等：确保后台建管线线程在设备关闭前已 join
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_close(2000000000ull, &err);
    if (!nvn_gpu_ok(rc)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk session: backend close %d: %s", rc.code, err.why);
        pthread_mutex_unlock(&g_mu);
        return 1;
    }
    g_ready = 0; g_window = NULL; g_window_gen = 0; g_win_w = g_win_h = 0;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int nvk_session_attach_window(void *window, uint64_t generation, char *why, size_t why_size) {
    if (!window) { if (why && why_size) snprintf(why, why_size, "null window"); return -1; }
    pthread_mutex_lock(&g_mu);
    if (!g_ready) { pthread_mutex_unlock(&g_mu); if (why && why_size) snprintf(why, why_size, "session not open"); return -1; }
    nvn_gpu_err err = {0}; nvn_gpu_surface sf = NULL;
    nvn_gpu_rc rc = nvn_gpu_surface_attach(window, &sf, &err);
    if (!nvn_gpu_ok(rc)) {
        pthread_mutex_unlock(&g_mu);
        if (why && why_size) snprintf(why, why_size, "surface attach %d: %s", rc.code, err.why);
        return -1;
    }
    g_surface = sf; g_window = window; g_window_gen = generation;
    g_win_w = ANativeWindow_getWidth((ANativeWindow *)window); g_win_h = ANativeWindow_getHeight((ANativeWindow *)window);
    double hz = s2a_direct_vk_display_refresh();
    rp_present_set_surface(sf, (int)(hz + 0.5));
    __android_log_print(ANDROID_LOG_INFO, TAG, "nvk session: surface attached gen=%llu window=%dx%d hz=%.1f", (unsigned long long)generation, g_win_w, g_win_h, hz);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

// 0 = 已摘且最后一帧的呈现段有完成证明；非 0 = 没证明（调用方保留窗口租约）
int nvk_session_detach_window(void) {
    pthread_mutex_lock(&g_mu);
    nvn_gpu_surface sf = g_surface;
    if (!sf) { g_window = NULL; g_window_gen = 0; g_win_w = g_win_h = 0; pthread_mutex_unlock(&g_mu); return 0; }
    rp_present_set_surface(NULL, 0);   // 返回后核心不再用这个面
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_surface_detach(sf, 1000000000ull, &err);
    if (!nvn_gpu_ok(rc)) {
        rp_present_set_surface(sf, 0);
        __android_log_print(ANDROID_LOG_ERROR, TAG, "【红】nvk session: surface detach %d: %s", rc.code, err.why);
        pthread_mutex_unlock(&g_mu);
        return 1;
    }
    g_surface = NULL; g_window = NULL; g_window_gen = 0; g_win_w = g_win_h = 0;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

void nvk_session_window_size(int32_t *w, int32_t *h) {
    pthread_mutex_lock(&g_mu);
    if (w) *w = g_win_w;
    if (h) *h = g_win_h;
    pthread_mutex_unlock(&g_mu);
}
