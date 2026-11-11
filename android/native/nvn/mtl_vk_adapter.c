#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#include "mtl_vk_adapter.h"
#include "mutex_timedlock.h"
#include "nvn_vk_format.h"
#include "nvn_vk_compile_facts.h"
#include "nvn_vk_lifecycle.h"
#include "nvn_vk_exec_owner.h"
#include "nvn_vk_draw.h"
#include "nvn_vk_first_draw.h"
#include "nvn_vk_query.h"
#include "nvn_vk_poolahb.h"
#include "nvn_vk_readback.h"
#include <android/log.h>
#include "direct/compile_request.h"
#include "direct/compile_worker.h"
#include "direct/w2_observation.h"
#include "../../../graphics/nvn/host/game_profile.h"

static struct nvn_vk_session g_session;
static int g_ready;
// 收尾已经尝试过但没拿到完成证明：设备/对象可能已部分退休。之后任何 shutdown 都不能再
// 返回 0（那会把 Retirement 变成 STOPPED）；保留 Failed/Retirement 到进程退出或新会话 init。
static int g_retire_failed;
// Stop 已发布：迟到入口（nvk_require/mtl_present/poll）不得重新 init 会话；只有 provider 的
// 显式新会话（s2a_vk_adapter_new_session）才清除。与 g_retire_failed 分开：干净 Stop 后也拒绝重开。
static _Atomic int g_stopped;
static _Atomic uint64_t g_registry_generation;

// NVN 前端按款读 DM_GAME_* 宏（graphics/nvn/host/game_profile.h）。Apple 侧由 host_adapt.m 从 game.tsv 填；
// 安卓直连没有逐款档，按“NVN 真实语义的缺省”给：条件渲染照实求值（render_cond=1，回放已在
// vk_record.c 实现条件+清屏/绘制门控），其余缺省保持与旧行为一致的 0，不改动其它路径。
// 可用 debug.s2a.render_cond=0 关掉做 A/B。弱桩在 vk_engine_host_shim.c，这里是强定义。
static s2a_nvn_profile g_android_nvn_profile;
static _Atomic int g_android_nvn_profile_ready;
const s2a_nvn_profile *s2a_nvn_prof(void) {
    if (!atomic_load_explicit(&g_android_nvn_profile_ready, memory_order_acquire)) {
        s2a_nvn_profile p;
        memset(&p, 0, sizeof p);
        p.t.render_cond = 1;
        char v[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.s2a.render_cond", v) > 0 && v[0] == '0') p.t.render_cond = 0;
        g_android_nvn_profile = p;
        atomic_store_explicit(&g_android_nvn_profile_ready, 1, memory_order_release);
    }
    return &g_android_nvn_profile;
}
// 诊断：注册表代际推进次数（读取即清零）。内容完全相同的重复登记不再推进代际，
// 这个数用来证明“真的变了”还是“每帧重登记”。
static _Atomic uint64_t g_regen_bumps;
void s2a_vk_adapter_take_regen_bumps(uint64_t *out) {
    if (out) *out = atomic_exchange(&g_regen_bumps, 0);
}
// Vulkan queue 需要外部同步：guest 线程的 mtl_exec 与 present 线程的 mtl_present 会并发碰到
// 同一个 queue（上传/提交/呈现），这里用一把锁把会话操作串起来。
// 递归互斥：执行记录时已持锁，绘制消费者还要通过同一适配层取纹理/采样器（不能自锁死）。
static pthread_mutex_t g_mu;
__attribute__((constructor)) static void s2a_vk_adapter_mu_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mu, &attr);
    pthread_mutexattr_destroy(&attr);
    // 弱链接[androiddraw]：nvk_w2_diagnostic_init（宿主单测不链提供方；macOS ld64 按 tools/weak_link_decl.py 登记放行）
    extern void nvk_w2_diagnostic_init(void) __attribute__((weak));
    if (nvk_w2_diagnostic_init) nvk_w2_diagnostic_init(); // outside adapter/worker locks
}
// 设备内编译产物目录：provider 在 activate 时把 <revision>/shaders/vulkan/direct 导出到 S2A_SHADER_DIR。
static char g_shader_dir[512];

// F4.4：执行包独占 + 可挂起。draw 等待需求编译时交还 g_mu，让 owner（present/Stop/窗口）继续；
// 其他 guest 线程的 exec 在挂起期间排队，不能与挂起的执行包交错。判决规则抽到
// nvn_vk_exec_owner.c（纯状态机，宿主回归直接链接生产实现），这里只负责锁/条件变量。
static struct nvk_exec_owner g_exec;
static uint64_t g_exec_generation;
// owner poll 需要无锁读取“是否挂起”：adapter 在 g_mu 下改状态机，同时发布这个原子镜像。
static _Atomic int g_exec_suspended_mirror;
// 收尾屏障用：是否有执行包在途（含挂起等待编译）。shutdown 不能先拿 g_mu 再等它，
// 否则挂起的 draw 永远拿不到锁 resume；只能无锁读这个镜像。
static _Atomic int g_exec_busy_mirror;
// 挂起令牌是本线程的：resume 只接受本线程这次挂起发出的令牌。
static _Thread_local uint64_t g_suspend_token_tls;
static _Atomic unsigned long long g_suspend_total_ns;
static uint64_t g_suspend_started_ns;
static uint64_t adapter_thread_token(void) { return (uint64_t)(uintptr_t)pthread_self(); }
// op=26 的“派发成本”里有一块一直没单列：拿 g_mu + 等前一个执行包让出的时间。
// 只统计，不改变任何调度语义；由 present 窗口汇总（读取即清零），避免逐笔日志放大。
static _Atomic uint64_t g_exec_lock_wait_ns, g_exec_lock_calls;
static uint64_t adapter_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int g_cancel;
static pthread_cond_t g_exec_cv = PTHREAD_COND_INITIALIZER;
// 暂停窗口内 owner 是否真的还在 present（F4.4 的“owner 继续处理窗口”证据）。
static _Atomic int g_pause_depth;
static _Atomic unsigned long long g_present_during_pause;
static void note_present_during_pause(void) {
    if (atomic_load(&g_pause_depth) > 0) atomic_fetch_add(&g_present_during_pause, 1);
}

// F4.4：worker 的编译执行体（managed 统一编译器；不碰 Vulkan，暂停期间与 present 不共享状态）。
static int adapter_worker_compile(void *ctx, uint64_t session, uint64_t generation,
                                  const char *json, size_t len, uint8_t **bytes, size_t *bytes_len,
                                  char *why, size_t why_size) {
    (void)ctx;
    struct s2a_compile_outcome out;
    memset(&out, 0, sizeof out);
    int rc = s2a_compile_request_execute(session, generation, json, len, &out);
    if (rc || !out.bytes || !out.bytes_len) {
        snprintf(why, why_size, "%s", out.summary[0] ? out.summary : "compile_fn produced no result");
        s2a_compile_outcome_free(&out);
        return -1;
    }
    *bytes = out.bytes;
    *bytes_len = out.bytes_len;
    out.bytes = NULL;
    out.bytes_len = 0;
    s2a_compile_outcome_free(&out);
    return 0;
}

// draw 在 worker 等待期间调用：把当前执行包明确挂起，再交还 g_mu。
// 返回 0 = 已挂起并交还锁；-1 = 不是当前执行包线程/Stop 已发布（调用方不得把等待当成可暂停）。
int s2a_vk_adapter_exec_pause(void) {
    uint64_t me = adapter_thread_token();
    uint64_t token = nvk_exec_owner_suspend(&g_exec, me);
    if (!token) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "adapter exec: pause refused (not the current packet, already suspended, or stopping)");
        return -1;
    }
    g_suspend_token_tls = token;
    g_suspend_started_ns = adapter_now_ns();
    atomic_store(&g_exec_suspended_mirror, 1);
    atomic_fetch_add(&g_pause_depth, 1);
    return pthread_mutex_unlock(&g_mu);
}
void s2a_vk_adapter_exec_resume(void) {
    uint64_t me = adapter_thread_token();
    uint64_t token = g_suspend_token_tls;
    if (pthread_mutex_lock(&g_mu) != 0) return;
    int rc = nvk_exec_owner_resume(&g_exec, me, token);
    if (rc == NVK_EXEC_RESUME_OK) {
        g_suspend_token_tls = 0;
        uint64_t now = adapter_now_ns();
        atomic_fetch_add(&g_suspend_total_ns,
                         now > g_suspend_started_ns ? now - g_suspend_started_ns : 0);
        atomic_store(&g_exec_suspended_mirror, 0);
        atomic_fetch_sub(&g_pause_depth, 1);
    } else if (rc != NVK_EXEC_RESUME_NOT_SUSPENDED) {
        // Stop/换代抢先时这里如实报错；执行包随后按失败收尾，不假装恢复成功。
        g_suspend_token_tls = 0;
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "adapter exec: resume refused (%d); the suspended packet must fail, not continue",
                            rc);
        atomic_store(&g_exec_suspended_mirror, 0);
        atomic_store(&g_pause_depth, 0);
    }
}
int s2a_vk_adapter_exec_suspended(void) { return atomic_load(&g_exec_suspended_mirror) ? 1 : 0; }
int s2a_vk_adapter_stop_requested(void) { return g_cancel; }

void s2a_vk_adapter_owner_stats(struct s2a_vk_owner_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    struct nvk_exec_owner_stats st;
    nvk_exec_owner_get_stats(&g_exec, &st);
    out->enters = st.enters;
    out->wait_enters = st.wait_enters;
    out->suspends = st.suspends;
    out->resumes = st.resumes;
    out->refused_foreign = st.refused_foreign + st.refused_double_suspend + st.refused_double_resume + st.refused_stale;
    out->refused_after_stop = st.refused_after_stop;
    out->stop_during_suspend = st.stop_during_suspend;
    out->leaves_while_suspended = st.leaves_while_suspended;
    out->suspend_total_ns = atomic_load(&g_suspend_total_ns);
    out->presented_during_suspend = atomic_load(&g_present_during_pause);
}

// 会话初始化时一次性注册（见 exec_guest）：定义在文件后面，这里先声明。
static int record_resolve_tex(void *opaque, const void *key, struct s2a_replay_texture *out);

// resolver / draw builder 是 _Thread_local：必须在“真正执行回放的线程”上注册。
// 以前每条命令都设一次并清掉（4 次全局写/命令）；改成每线程按会话代注册一次。
static _Atomic unsigned g_reg_gen;
static _Thread_local unsigned tls_reg_gen;

int s2a_vk_adapter_init(char *why, size_t why_size) {
    if (atomic_load(&g_stopped)) {
        if (why && why_size) snprintf(why, why_size, "adapter stopped; refusing re-init until an explicit new session");
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    int rc = 0;
    if (g_retire_failed) {
        if (why && why_size) snprintf(why, why_size, "previous session retirement incomplete");
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    if (g_ready) { pthread_mutex_unlock(&g_mu); return 0; }
    g_cancel = 0;
    g_retire_failed = 0;
    g_exec_generation++;
    if (!g_exec_generation) g_exec_generation = 1;
    nvk_exec_owner_init(&g_exec, g_exec_generation);
    atomic_store(&g_exec_suspended_mirror, 0);
    atomic_store(&g_exec_busy_mirror, 0);
    atomic_store(&g_pause_depth, 0);
    if (nvn_vk_session_create(&g_session, why, why_size)) rc = -1;
    else {
        // 默认开（设备支持 AHB 且能力自检通过才真正启用；debug.s2a.pool_ahb=0 可关）。迁移只在 VM owner 窗口里做。
        (void)s2a_poolahb_init(&g_session.device);
        g_ready = 1;
        {
            char wwhy[160] = "";
            if (s2a_compile_worker_start(adapter_worker_compile, NULL, wwhy, sizeof wwhy))
                __android_log_print(ANDROID_LOG_WARN, "S2Arm",
                                    "adapter: compile worker unavailable (%s); draws fall back to synchronous compile",
                                    wwhy);
            else
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                    "adapter: compile worker started (one demand slot, dedupe by request sha)");
        }
        const char *dir = getenv("S2A_SHADER_DIR");
        if (dir) snprintf(g_shader_dir, sizeof g_shader_dir, "%s", dir);
        // 只推进代次；真正的 TLS 注册在各自执行线程的第一条命令上做（见 exec_guest）。
        atomic_fetch_add_explicit(&g_reg_gen, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// session 已结算并退休呈现，仍持有 device；此时才能拆 first_draw 子对象。
static void adapter_release_device_children(struct s2a_vk_device *d) {
    nvn_vk_first_draw_release_device(d);
    s2a_poolahb_shutdown();
}

int s2a_vk_adapter_shutdown(void) {
    // 诊断：debug.s2a.stop_delay_ms=N 在 shutdown 入口睡 N ms（上限 10 s），把 UI 的
    // release/revoke 排进 Stop 的收尾窗口，用来验证 B4（长收尾不持窗口锁 + stopping_engine 门）。
    {
        char v[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.s2a.stop_delay_ms", v) > 0) {
            int ms = atoi(v);
            if (ms > 0 && ms <= 10000) {
                __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                    "adapter shutdown: injecting %d ms stop delay (diagnostic)", ms);
                usleep((useconds_t)ms * 1000);
            }
        }
    }
    atomic_store(&g_stopped, 1);   // 先发布：迟到 present/require 不再重建会话
    extern void nvk_w2_diagnostic_cancel(void) __attribute__((weak));
    if (nvk_w2_diagnostic_cancel) nvk_w2_diagnostic_cancel(); // atomic-only revoke
    if (g_retire_failed) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "adapter shutdown: a previous retirement attempt failed; keeping Failed/Retirement "
                            "(process exit required; new session refused)");
        return -1;
    }
    int retire_ok = 0;
    // 先停图形翻译线程（跑空队列 + join），再拆设备和缓存：否则 worker 可能在设备销毁后碰句柄。
    { extern void s2a_mtl_async_stop(void) __attribute__((weak));
      if (s2a_mtl_async_stop) s2a_mtl_async_stop(); }
    pthread_mutex_lock(&g_mu);
    g_cancel = 1;
    nvk_exec_owner_request_stop(&g_exec);
    pthread_cond_broadcast(&g_exec_cv);
    pthread_mutex_unlock(&g_mu);
    // 先让等待编译的 draw 醒来失败，再按预算收 worker；join 不持 g_mu（等待者在 resume 时会拿锁）。
    // 有界：managed 编译不可中断，长编译/卡住时无界 join 会把 Stop 拖过收尾预算。未退出就如实报错；
    // worker 的编译体不碰 Vulkan（只用自有 JSON 副本 + 已登记编译入口），迟到结果按 Stop 丢弃。
    int worker_still_running = 0;
    {
        char wwhy[192] = "";
        int wrc = s2a_compile_worker_stop_bounded(1000, wwhy, sizeof wwhy);
        if (wrc > 0) {
            // 合同 §4.3：在途 worker 返回前保留 device/context。worker 的编译体是纯 CPU（不碰
            // Vulkan），但这里仍不拆 session/device：下一次 Stop（或进程退出）先回收 worker。
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "adapter shutdown: compile worker still running after 1000 ms (%s); "
                                "session/device/context retained, no normal-stop claim", wwhy);
            worker_still_running = 1;
            retire_ok = -1;
        } else if (wrc < 0) {
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "adapter shutdown: compile worker stop failed (%s)",
                                wwhy);
            retire_ok = -1;
        }
    }
    if (worker_still_running) {
        // 不拿 g_mu：挂起中的 draw 醒来后还要用它 resume/leave；保留会话让迟到结果与下一次
        // Stop 能回收。g_cancel 保持 1，新 exec 会被拒绝。
        return -1;
    }
    // 执行包屏障：worker 已退出不等于 draw 已离开回放。挂起的 draw 需要先拿 g_mu resume/leave，
    // 所以这里不能持 g_mu 等；只读无锁镜像，按预算等它自己退出。拿不到就保留 device/session。
    {
        int waited_ms = 0;
        while (atomic_load(&g_exec_busy_mirror) && waited_ms < 500) {
            usleep(1000);
            waited_ms++;
        }
        if (atomic_load(&g_exec_busy_mirror)) {
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "adapter shutdown: an execution packet is still in flight after 500 ms; "
                                "session/device/context retained, no normal-stop claim");
            return -1;
        }
    }
    pthread_mutex_lock(&g_mu);
    if (g_ready) {
        // 停止生产前先尝试拿到完成证明；失败时资源由 owner 记账保留（不伪造完成）。
        {
            // 执行包 owner 对账必须在 nvk_exec_owner_init 清状态之前读：Stop 时是否真的有挂起包、
            // 挂起期间 owner 是否在推进，这些是 F4.4 的独立事实。
            struct s2a_vk_owner_stats ost;
            memset(&ost, 0, sizeof ost);
            s2a_vk_adapter_owner_stats(&ost);
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                "adapter owner: slices not tracked here; enters=%llu wait=%llu suspends=%llu "
                                "resumes=%llu refused=%llu stopDuringSuspend=%llu leavesWhileSuspended=%llu "
                                "suspendMs=%llu presentedInSuspend=%llu",
                                (unsigned long long)ost.enters, (unsigned long long)ost.wait_enters,
                                (unsigned long long)ost.suspends, (unsigned long long)ost.resumes,
                                (unsigned long long)ost.refused_foreign,
                                (unsigned long long)ost.stop_during_suspend,
                                (unsigned long long)ost.leaves_while_suspended,
                                (unsigned long long)(ost.suspend_total_ns / 1000000ull),
                                (unsigned long long)ost.presented_during_suspend);
        }
        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                            "adapter shutdown: owner presented %llu times while a demand compile held the execution packet",
                            (unsigned long long)atomic_load(&g_present_during_pause));
        int frc = nvn_vk_first_draw_finish();
        // finish 可在当前队列为空时返回；延迟 present 帧槽仍须由其 owner 退休。
        if (!frc) frc = nvn_vk_first_draw_retire_all();
        // 未完成或失败的 query 账本不可清空，也不能销毁其 device。
        if (!frc && !nvn_vk_query_terminal_ready()) frc = -1;
        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                            "adapter shutdown: frame queue finish rc=%d; retiring session (device destruction "
                            "follows only after retained-object settlement)", frc);
        if (frc) {
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "adapter shutdown: frame queue did not complete (%d); resources retained, "
                                "no fake completion", frc);
            retire_ok = -1;
        }
        // 未完成的 first_draw 队列不得销毁 session 或其任何子对象。
        if (!frc) {
            // 逐帧帧时间转储（debug.s2a.frame_log=1 且有记录才打）：此刻帧队列已结算、
            // present 不再新增；模块不在的构建（无 NVN 前端）整块跳过。
            { extern void nvn_vk_frame_log_dump(void) __attribute__((weak));
              if (nvn_vk_frame_log_dump) nvn_vk_frame_log_dump(); }
            if (nvn_vk_session_destroy_with_callback(&g_session, adapter_release_device_children)) retire_ok = -1;
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                                "adapter shutdown: frame queue incomplete; session and children kept");
        }
        g_ready = 0;
        if (retire_ok) g_retire_failed = 1;   // 不能再假装第二次 Stop 成功
    }
    g_cancel = 0;
    atomic_store(&g_exec_suspended_mirror, 0);
    g_exec_generation++;
    if (!g_exec_generation) g_exec_generation = 1;
    nvk_exec_owner_init(&g_exec, g_exec_generation);
    pthread_mutex_unlock(&g_mu);
    return retire_ok;
}

// nvnQueueFinish：把积压的绘制提交并等完成，会话和设备保持存活（不再 shutdown）。
int s2a_vk_adapter_finish(void) {
    pthread_mutex_lock(&g_mu);
    int rc = g_ready ? nvn_vk_first_draw_finish() : 0;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// guest fence 的完成证明：把已录制的帧队列提交并等 GPU 完成。持 g_mu 与回放/呈现串行，
// 避免两个 guest 线程同时碰帧队列。
int s2a_vk_adapter_flush(void) {
    pthread_mutex_lock(&g_mu);
    int rc = g_ready ? nvn_vk_first_draw_flush() : 0;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// 带预算拿 owner 锁：budget_ns<0 = 无限等；0 = trylock；>0 = timedlock（本线程已持有递归锁
// 时直接成功）。拿不到锁就没有完成证明，返回 1；调用方不得把“没等到锁”当成设备错误。
static int adapter_lock_budget(int64_t budget_ns) {
    if (budget_ns < 0) return pthread_mutex_lock(&g_mu) == 0 ? 0 : -1;
    if (budget_ns == 0) return pthread_mutex_trylock(&g_mu) == 0 ? 0 : 1;
#if defined(__APPLE__) || defined(S2A_TEST_DARWIN_TIMEDLOCK)
    // macOS libSystem 没有 pthread_mutex_timedlock（编译期就报未声明）：直接走下面同一段
    // “预算内 trylock 轮询”回退——预算仍是 budget_ns、返回语义仍是 0 = 拿到锁 / 1 = 预算内
    // 没拿到（调用方 s2a_vk_adapter_flush_budget 把 1 当“没有完成证明、非设备错误”），
    // 与 Android/Linux 的 timedlock 路径等价；Android/Linux 默认路径保持原样（#else 分支）。
    // S2A_TEST_DARWIN_TIMEDLOCK 让 Linux 宿主单测也能编/跑这条 Darwin 分支：nvn_event_completion_test
    // 与 nvn_vk_adapter_release_order_test 已按这个宏在 Linux 上跑两遍（EINVAL 回退语义另有断言）。
#else
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += (time_t)(budget_ns / 1000000000ll);
    dl.tv_nsec += (long)(budget_ns % 1000000000ll);
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    int rc = s2a_mutex_timedlock(&g_mu, &dl);
    if (rc == 0) return 0;
    if (rc == ETIMEDOUT) return 1;
    // 某些实现对递归 mutex 的 timedlock 返回 EINVAL/ENOTSUP：退回有界 trylock 轮询。
#endif
    const int64_t end = (int64_t)adapter_now_ns() + budget_ns;
    while ((int64_t)adapter_now_ns() < end) {
        if (pthread_mutex_trylock(&g_mu) == 0) return 0;
        usleep(50);
    }
    return 1;
}

int s2a_vk_adapter_flush_budget(int64_t budget_ns) {
    const int64_t t0 = (int64_t)adapter_now_ns();
    if (adapter_lock_budget(budget_ns)) return 2;   // 没拿到锁：没有完成证明（非设备错误）
    int64_t left = -1;
    if (budget_ns >= 0) {
        left = budget_ns - ((int64_t)adapter_now_ns() - t0);
        if (left < 0) left = 0;
    }
    int rc = g_ready ? nvn_vk_first_draw_flush_ready(left) : 0;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// NVN 允许进程注册多个内存池（内存池 + 命令/控制/计算内存），注册表按地址区间查询。
// 每个槽 16 字节，4096 槽 = 64 KiB；容量按 ABI 的池 id 空间留足，不假设某个标题的池数量。
#define S2A_ADAPTER_POOLS 4096
static struct { uint64_t start, end; } g_pools[S2A_ADAPTER_POOLS];
static int g_pool_n;   // 已用槽数（表按 start 升序，查找走二分）

// 查地址区间落在哪个已注册的内存池里（池表是 NVN 的注册顺序，按区间匹配）。
// 表按 start 升序：每笔 draw 都要查，线性扫 4096 槽在这里是几毫秒级的开销。
int s2a_vk_adapter_pool_find(uint64_t addr, uint64_t len, uint64_t *base, uint64_t *size) {
    if (!len || !g_pool_n) return -1;
    int lo = 0, hi = g_pool_n - 1, best = -1;
    while (lo <= hi) {                       // 找到 start <= addr 的最大项
        int mid = (lo + hi) / 2;
        if (g_pools[mid].start <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return -1;
    uint64_t off = addr - g_pools[best].start;
    uint64_t sz = g_pools[best].end - g_pools[best].start;
    if (off > sz || len > sz - off) return -1;
    if (base) *base = g_pools[best].start;
    if (size) *size = sz;
    return 0;
}

int s2a_vk_adapter_pool_add(const void *mem, uint64_t size) {
    if (!mem || !size) return -1;
    uint64_t start = (uint64_t)(uintptr_t)mem, end = start + size;
    if (g_pool_n >= S2A_ADAPTER_POOLS) return -1;
    // 按 start 升序插入（池数量是几十个，插入排序的搬移可忽略）
    int pos = g_pool_n;
    while (pos > 0 && g_pools[pos - 1].start > start) { g_pools[pos] = g_pools[pos - 1]; pos--; }
    g_pools[pos].start = start;
    g_pools[pos].end = end;
    g_pool_n++;
    // 内存建立点：此时池是新的（没有纹理/GPU 消费者、没有在途引用）。
    // 前提不满足就什么都不做，绘制层继续走正确的上传路径。
    (void)s2a_poolahb_establish(start, size);
    return 0;
}

void s2a_vk_adapter_pool_del(const void *mem) {
    uint64_t start = (uint64_t)(uintptr_t)mem;
    for (int i = 0; i < g_pool_n; i++)
        if (g_pools[i].start == start) {
            // 池被删：账本立即停止新命中，后备等完成证明后释放。
            s2a_poolahb_pool_removed(g_pools[i].start, g_pools[i].end - g_pools[i].start);
            memmove(&g_pools[i], &g_pools[i + 1], (size_t)(g_pool_n - i - 1) * sizeof g_pools[0]);
            g_pool_n--;
            g_pools[g_pool_n].start = g_pools[g_pool_n].end = 0;
            return;
        }
}

// guest 写了池内存（FlushMappedRange 等）：推进内容版本。
void s2a_vk_adapter_pool_written(const void *mem, uint64_t off, uint64_t size) {
    if (!mem || !size) return;
    (void)s2a_poolahb_note_write((uint64_t)(uintptr_t)mem + off, size);
    // 内容归属：这是调用点（FlushMappedRange）给的精确范围，进纹理缓存的范围账本。
    pthread_mutex_lock(&g_mu);
    if (g_ready) nvn_vk_session_pool_written(&g_session, mem, off, size);
    pthread_mutex_unlock(&g_mu);
}

void s2a_vk_adapter_tex_written_rect(const tex_t *t, unsigned level, unsigned layer0,
                                     int x, int y, int w, int h, int z0, int nz) {
    pthread_mutex_lock(&g_mu);
    if (g_ready && t) nvn_vk_session_tex_written_rect(&g_session, t, level, layer0, x, y, w, h, z0, nz);
    pthread_mutex_unlock(&g_mu);
}
void s2a_vk_adapter_tex_gpu_written_subres(const tex_t *t, unsigned level, unsigned layer) {
    pthread_mutex_lock(&g_mu);
    if (g_ready && t) nvn_vk_session_tex_gpu_written_subres(&g_session, t, level, layer);
    pthread_mutex_unlock(&g_mu);
}

int s2a_vk_adapter_pool_contains(uint64_t addr, uint64_t len) {
    for (int i = 0; i < S2A_ADAPTER_POOLS; i++)
        if (g_pools[i].start && addr >= g_pools[i].start && addr + len <= g_pools[i].end) return 1;
    return 0;
}

// GPU 地址 → (池基址, 偏移)：mtl_addr 用。地址必须整个落在同一个池里。
int s2a_vk_adapter_pool_lookup(uint64_t addr, uint64_t len, void **base, uint64_t *off) {
    if (base) *base = NULL;
    if (off) *off = 0;
    for (int i = 0; i < S2A_ADAPTER_POOLS; i++) {
        if (!g_pools[i].start) continue;
        if (addr >= g_pools[i].start && len <= g_pools[i].end - addr) {
            if (base) *base = (void *)(uintptr_t)g_pools[i].start;
            if (off) *off = addr - g_pools[i].start;
            return 1;
        }
    }
    return 0;
}

struct nvn_vk_tex *s2a_vk_adapter_tex(const tex_t *t, char *why, size_t why_size) {
    pthread_mutex_lock(&g_mu);
    struct nvn_vk_tex *tex = NULL;
    if (!g_ready) { if (why && why_size) snprintf(why, why_size, "adapter not initialized"); }
    else tex = nvn_vk_session_tex(&g_session, t, why, why_size);
    pthread_mutex_unlock(&g_mu);
    return tex;
}

void s2a_vk_adapter_mark_written(VkImage image) {
    pthread_mutex_lock(&g_mu);
    if (g_ready) nvn_vk_session_mark_written(&g_session, image);
    pthread_mutex_unlock(&g_mu);
}

int s2a_vk_adapter_tex_gpu_owned(uint64_t serial) {
    pthread_mutex_lock(&g_mu);
    int r = g_ready ? nvn_vk_session_tex_gpu_owned(&g_session, serial) : 0;
    pthread_mutex_unlock(&g_mu);
    return r;
}

void s2a_vk_adapter_tex_cpu_written(const tex_t *t) {
    pthread_mutex_lock(&g_mu);
    if (g_ready && t) nvn_vk_session_tex_cpu_written(&g_session, t);
    pthread_mutex_unlock(&g_mu);
}

void s2a_vk_adapter_tex_release(const tex_t *t) {
    pthread_mutex_lock(&g_mu);
    if (g_ready && t) nvn_vk_session_tex_release(&g_session, t);
    atomic_fetch_add(&g_registry_generation, 1);
    pthread_mutex_unlock(&g_mu);
}

int s2a_vk_adapter_present_tex(const tex_t *t, const int crop[4], char *why, size_t why_size) {
    note_present_during_pause();
    pthread_mutex_lock(&g_mu);
    int rc = -1;
    if (!g_ready) { if (why && why_size) snprintf(why, why_size, "adapter not initialized"); }
    else rc = nvn_vk_session_present_tex(&g_session, t, crop, why, why_size);
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// 没有 surface 的 present：提交本帧并只在拿到 GPU 完成证明后解锁纹理 pin。返回 0 = 本帧
// 已完成且 pin 已释放；-1 = 没有完成证明（队列终态/预算耗尽），pin 与队列资源保留。
int s2a_vk_adapter_skip_present(void) {
    pthread_mutex_lock(&g_mu);
    int rc = 0;
    if (g_ready) {
        int frc = nvn_vk_first_draw_flush();
        if (nvn_vk_skip_present_unpin_allowed(frc)) nvn_vk_first_draw_present_done();
        else rc = -1;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

void s2a_vk_adapter_new_session(void) {
    pthread_mutex_lock(&g_mu);
    // 失败退休不是新会话许可；保留 stopped/retire_failed，禁止 init 覆盖旧设备。
    if (!g_retire_failed && !g_session.ready && !g_session.device.device && !g_session.device.instance) {
        // 弱链接[androiddraw]：nvk_w2_diagnostic_resume（宿主单测 device_retirement_test 摘本函数编译、不链 nvn_vk_compile_facts.c；macOS ld64 要按 tools/weak_link_decl.py 登记放行）
        extern void nvk_w2_diagnostic_resume(void) __attribute__((weak));
        if (nvk_w2_diagnostic_resume) nvk_w2_diagnostic_resume();
        atomic_store(&g_stopped, 0);
    }
    pthread_mutex_unlock(&g_mu);
}

int s2a_vk_adapter_detach_window(void) {
    // surfaceDestroyed 在 UI 线程、外壳只给 1 s：等 owner 锁必须有界。拿不到锁就没有退休证明，
    // 返回 -1 让调用方保留租约（随后 revoke 超时会带 tombstone，不会假装已释放 surface）。
    if (adapter_lock_budget(400000000ll)) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "adapter detach: owner lock busy for 400 ms; surface retirement not proven");
        return -1;
    }
    int rc = 0;
    if (g_ready) rc = nvn_vk_session_detach_window(&g_session);
    else if (g_retire_failed) {
        // B1：Stop 失败时 session/device 被保留、呈现退休仍未证明；detach 不能返回 0，
        // 否则 provider 会把仍有在途 present 的 WSI 租约 ACK 掉。
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "adapter detach: stopped with unretired presentation; window lease kept (no ACK)");
        rc = -1;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int s2a_vk_adapter_attach_window(ANativeWindow *window, uint64_t generation, char *why, size_t why_size) {
    pthread_mutex_lock(&g_mu);
    int rc = -1;
    if (!g_ready) { if (why && why_size) snprintf(why, why_size, "adapter not initialized"); }
    else {
        rc = nvn_vk_session_attach_window(&g_session, window, why, why_size);
        if (!rc) g_session.swapchain.touch_generation = generation;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// owner poll 的窗口推进：等 owner 锁必须有界（控制片 <=10 ms，不能把 managed owner 线程拖过
// Stop 预算）。返回 0 = 已接上；2 = 锁忙/未就绪（本轮不推进，下一轮重试，不是失败）；-1 = 真实失败。
int s2a_vk_adapter_attach_window_budget(ANativeWindow *window, uint64_t generation, int64_t budget_ns,
                                        char *why, size_t why_size) {
    if (!window) {
        if (why && why_size) snprintf(why, why_size, "no window");
        return -1;
    }
    if (adapter_lock_budget(budget_ns)) {
        if (why && why_size) snprintf(why, why_size, "owner lock busy; window attach deferred");
        return 2;
    }
    int rc = -1;
    if (!g_ready) {
        if (why && why_size) snprintf(why, why_size, "adapter not initialized");
        rc = 2;
    } else {
        rc = nvn_vk_session_attach_window(&g_session, window, why, why_size);
        if (!rc) g_session.swapchain.touch_generation = generation;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// B3：debug.s2a.buffer_geometry=1 时交换链 extent=渲染目标尺寸；Java 读它把 SurfaceView
// 的 frame 布成同比例居中（合成器拉伸保持宽高比）。未开/无窗口/交换链未就绪时返回 0。
int s2a_vk_adapter_buffer_geometry(uint32_t *w, uint32_t *h) {
    pthread_mutex_lock(&g_mu);
    int on = g_ready && g_session.has_window && g_session.swapchain.buf_geometry &&
             !g_session.swapchain.geometry_failed && g_session.swapchain.ready;
    uint32_t ew = on ? g_session.swapchain.extent.width : 0;
    uint32_t eh = on ? g_session.swapchain.extent.height : 0;
    pthread_mutex_unlock(&g_mu);
    if (w) *w = ew;
    if (h) *h = eh;
    return on;
}

int s2a_vk_adapter_present_clear(float r, float g, float b, float a, char *why, size_t why_size) {
    note_present_during_pause();
    pthread_mutex_lock(&g_mu);
    int rc = -1;
    if (!g_ready) { if (why && why_size) snprintf(why, why_size, "adapter not initialized"); }
    else rc = nvn_vk_session_present_clear(&g_session, r, g, b, a, why, why_size);
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int s2a_vk_adapter_draw_present(const char *blob_path, char *why, size_t why_size) {
    note_present_during_pause();
    if (!g_ready) { if (why && why_size) snprintf(why, why_size, "adapter not initialized"); return -1; }
    return nvn_vk_session_draw_present(&g_session, blob_path, why, why_size);
}

// ---- 纹理/采样器注册表：NVN 池 id → 注册当时的 tex_t/view_t/samp_par 快照 ----
// 与 Metal 回放的 mtl_reg_tex/mtl_reg_samp 同一来源（hle_nvn 在 RegisterTexture 时给出真实对象副本）。
#define NVK_TEXREG 4096   // 初始容量；按需增长，容量不是合法资源上限（审计 A2）
#define NVK_SAMPREG 1024
struct tex_reg_ent { int used; int id; tex_t t; view_t v; int has_view; };
struct samp_reg_ent { int used; int id; samp_par p; };
static struct tex_reg_ent *g_tex_reg;
static struct samp_reg_ent *g_samp_reg;
static int g_tex_cap, g_samp_cap;
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
#define RED_REG(msg) __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "【红】%s", msg)

// g_reg_mu 持有下调用：按需初始化/翻倍重建（开放寻址、无删除，探测在首个空槽停止）。
static int reg_grow_locked(int tex) {
    if (tex) {
        int ncap = g_tex_cap ? g_tex_cap * 2 : NVK_TEXREG;
        struct tex_reg_ent *nt = (struct tex_reg_ent *)calloc((size_t)ncap, sizeof *nt);
        if (!nt) return -1;
        for (int i = 0; i < g_tex_cap; i++) if (g_tex_reg[i].used) {
            unsigned h = (unsigned)g_tex_reg[i].id * 2654435761u;
            for (int pr = 0; pr < ncap; pr++) {
                int j = (int)((h + (unsigned)pr) % (unsigned)ncap);
                if (!nt[j].used) { nt[j] = g_tex_reg[i]; break; }
            }
        }
        free(g_tex_reg); g_tex_reg = nt; g_tex_cap = ncap;
        return 0;
    }
    int ncap = g_samp_cap ? g_samp_cap * 2 : NVK_SAMPREG;
    struct samp_reg_ent *nt = (struct samp_reg_ent *)calloc((size_t)ncap, sizeof *nt);
    if (!nt) return -1;
    for (int i = 0; i < g_samp_cap; i++) if (g_samp_reg[i].used) {
        unsigned h = (unsigned)g_samp_reg[i].id * 2654435761u;
        for (int pr = 0; pr < ncap; pr++) {
            int j = (int)((h + (unsigned)pr) % (unsigned)ncap);
            if (!nt[j].used) { nt[j] = g_samp_reg[i]; break; }
        }
    }
    free(g_samp_reg); g_samp_reg = nt; g_samp_cap = ncap;
    return 0;
}
static int reg_init_locked(int tex) {   // 0 = 可用，-1 = 分配失败（调用方准确失败）
    if (tex) return (g_tex_cap || reg_grow_locked(1) == 0) ? 0 : -1;
    return (g_samp_cap || reg_grow_locked(0) == 0) ? 0 : -1;
}

void s2a_vk_adapter_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (!t) return;
    pthread_mutex_lock(&g_reg_mu);
    if (reg_init_locked(1) != 0) { pthread_mutex_unlock(&g_reg_mu); RED_REG("texture registry init failed (out of memory)"); return; }
    int found = -1;
    for (;;) {
        unsigned h = (unsigned)id * 2654435761u;
        for (int probe = 0; probe < g_tex_cap; probe++) {
            int i = (int)((h + (unsigned)probe) % (unsigned)g_tex_cap);
            if (!g_tex_reg[i].used) { found = i; break; }
            if (g_tex_reg[i].id == id) { found = i; break; }
        }
        if (found >= 0) break;
        if (reg_grow_locked(1) != 0) { pthread_mutex_unlock(&g_reg_mu); RED_REG("texture registry growth failed (out of memory)"); return; }
    }
    if (found >= 0) {
        // 代际表示“注册表内容变了”。内容逐字节相同就不推进：内容身份键里带了代际，
        // 每帧把同一张纹理重新登记一遍会无谓地作废所有已经验证过的身份备忘
        // （实测备忘命中率被压到 ~50%，身份编码/哈希每笔重算）。
        const int same = g_tex_reg[found].used && g_tex_reg[found].id == id &&
                         g_tex_reg[found].has_view == (v != NULL) &&
                         !memcmp(&g_tex_reg[found].t, t, sizeof(tex_t)) &&
                         (!v || !memcmp(&g_tex_reg[found].v, v, sizeof(view_t)));
        g_tex_reg[found].used = 1;
        g_tex_reg[found].id = id;
        memcpy(&g_tex_reg[found].t, t, sizeof(tex_t));
        g_tex_reg[found].has_view = v != NULL;
        if (v) memcpy(&g_tex_reg[found].v, v, sizeof(view_t));
        if (!same) {
            atomic_fetch_add(&g_registry_generation, 1);   // 内容写完后才发布代际
            atomic_fetch_add(&g_regen_bumps, 1);
        }
    }
    pthread_mutex_unlock(&g_reg_mu);
}

void s2a_vk_adapter_reg_samp(int id, const samp_par *p) {
    if (!p) return;
    pthread_mutex_lock(&g_reg_mu);
    if (reg_init_locked(0) != 0) { pthread_mutex_unlock(&g_reg_mu); RED_REG("sampler registry init failed (out of memory)"); return; }
    int found = -1;
    for (;;) {
        unsigned h = (unsigned)id * 2654435761u;
        for (int probe = 0; probe < g_samp_cap; probe++) {
            int i = (int)((h + (unsigned)probe) % (unsigned)g_samp_cap);
            if (!g_samp_reg[i].used) { found = i; break; }
            if (g_samp_reg[i].id == id) { found = i; break; }
        }
        if (found >= 0) break;
        if (reg_grow_locked(0) != 0) { pthread_mutex_unlock(&g_reg_mu); RED_REG("sampler registry growth failed (out of memory)"); return; }
    }
    if (found >= 0) {
        const int same = g_samp_reg[found].used && g_samp_reg[found].id == id &&
                         !memcmp(&g_samp_reg[found].p, p, sizeof *p);
        g_samp_reg[found].used = 1; g_samp_reg[found].id = id; g_samp_reg[found].p = *p;
        if (!same) { atomic_fetch_add(&g_registry_generation, 1); atomic_fetch_add(&g_regen_bumps, 1); }
    }
    pthread_mutex_unlock(&g_reg_mu);
}

// 已注册纹素对象的通道 swizzle（TextureBuilderSetSwizzle）。0 = 未设置（恒等 R G B A）。
int s2a_vk_adapter_tex_swz_by_id(int id, uint32_t *swz, int *set) {
    if (!swz || !set) return -1;
    int rc = -1;
    pthread_mutex_lock(&g_reg_mu);
    if (reg_init_locked(1) != 0) { pthread_mutex_unlock(&g_reg_mu); return -1; }
    unsigned h = (unsigned)id * 2654435761u;
    for (int probe = 0; probe < g_tex_cap; probe++) {
        int i = (int)((h + (unsigned)probe) % (unsigned)g_tex_cap);
        if (!g_tex_reg[i].used) break;
        if (g_tex_reg[i].id == id) { *swz = g_tex_reg[i].t.swz; *set = g_tex_reg[i].t.set_swz; rc = 0; break; }
    }
    pthread_mutex_unlock(&g_reg_mu);
    return rc;
}

VkImageView s2a_vk_adapter_attachment_view(struct nvn_vk_tex *t, const view_t *v, VkFormat *format_out,
                                           char *why, size_t why_size) {
    return (g_ready && t) ? nvn_vk_texcache_attachment_view(&g_session.textures, t, v, format_out, why, why_size)
                          : VK_NULL_HANDLE;
}
VkImageView s2a_vk_adapter_tex_view(struct nvn_vk_tex *t, const view_t *v, char *why, size_t why_size) {
    pthread_mutex_lock(&g_mu);
    VkImageView iv = (g_ready && t) ? nvn_vk_texcache_view(&g_session.textures, t, v, why, why_size) : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    return iv;
}

int s2a_vk_adapter_tex_view_by_id(int id, view_t *out) {
    if (!out) return -1;
    int rc = -1;
    pthread_mutex_lock(&g_reg_mu);
    if (reg_init_locked(1) != 0) { pthread_mutex_unlock(&g_reg_mu); return -1; }
    unsigned h = (unsigned)id * 2654435761u;
    for (int probe = 0; probe < g_tex_cap; probe++) {
        int i = (int)((h + (unsigned)probe) % (unsigned)g_tex_cap);
        if (!g_tex_reg[i].used) break;
        if (g_tex_reg[i].id == id) {
            if (g_tex_reg[i].has_view) { *out = g_tex_reg[i].v; rc = 1; }
            else rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_reg_mu);
    return rc;
}

uint64_t s2a_vk_adapter_registry_generation(void) { return atomic_load(&g_registry_generation); }

int s2a_vk_adapter_samp_by_id(int id, samp_par *out) {
    if (!out) return -1;
    pthread_mutex_lock(&g_reg_mu);
    if (reg_init_locked(0) != 0) { pthread_mutex_unlock(&g_reg_mu); return -1; }
    unsigned h = (unsigned)id * 2654435761u;
    int rc = -1;
    // 探测半径必须是当前容量 g_samp_cap：登记/增长都按它取模（reg_grow_locked）。
    // 用初始常量 NVK_SAMPREG 时，表增长后哈希顺序整体改变，已登记项会变得查不到。
    for (int probe = 0; probe < g_samp_cap; probe++) {
        int i = (int)((h + (unsigned)probe) % (unsigned)g_samp_cap);
        if (!g_samp_reg[i].used) break;
        if (g_samp_reg[i].id == id) { *out = g_samp_reg[i].p; rc = 0; break; }
    }
    pthread_mutex_unlock(&g_reg_mu);
    return rc;
}

// F4.2B：真实纹理描述事实（读注册表快照；不建视图、不触发上传/同步）。
int s2a_vk_adapter_tex_facts(int id, struct s2a_vk_tex_facts *out, char *why, size_t why_size) {
    if (!out) return -1;
    tex_t t;
    int has = 0;
    pthread_mutex_lock(&g_reg_mu);
    if (reg_init_locked(1) != 0) { pthread_mutex_unlock(&g_reg_mu); if (why) snprintf(why, why_size, "texture registry init failed (out of memory)"); return -1; }
    unsigned h = (unsigned)id * 2654435761u;
    for (int probe = 0; probe < g_tex_cap; probe++) {
        int i = (int)((h + (unsigned)probe) % (unsigned)g_tex_cap);
        if (!g_tex_reg[i].used) break;
        if (g_tex_reg[i].id == id) { t = g_tex_reg[i].t; has = 1; break; }
    }
    pthread_mutex_unlock(&g_reg_mu);
    if (!has) { if (why) snprintf(why, why_size, "texture id %d is not registered", id); return -1; }
    out->nvn_format = (uint32_t)t.d.format;
    out->nvn_target = (uint32_t)t.d.target;
    out->layers = t.d.depth;
    out->levels = t.d.levels;
    out->samples = t.d.samples;
    return 0;
}

// Bounded diagnostic copy of what the existing facts lookup reads. This is not
// proof of live pool membership: the registry owns registration-time copies.
int s2a_vk_adapter_w2_capture_registered(const int *ids, size_t count,
    struct nvk_w2_raw_registration *out, uint64_t *generation) {
    if (!ids || !out || !generation || count > NVK_TEXTURE_SET_MAX) return -1;
    memset(out, 0, count * sizeof *out);
    pthread_mutex_lock(&g_reg_mu);
    for (size_t n = 0; n < count; n++) {
        if (ids[n] < 0) continue;
        unsigned h = (unsigned)ids[n] * 2654435761u;
        for (int probe = 0; probe < g_tex_cap; probe++) {
            int i = (int)((h + (unsigned)probe) % (unsigned)g_tex_cap);
            const struct tex_reg_ent *r = &g_tex_reg[i];
            if (!r->used) break;
            if (r->id != ids[n]) continue;
            out[n].present = 1;
            out[n].format = (uint32_t)r->t.d.format;
            out[n].target = (uint32_t)r->t.d.target;
            out[n].layers = r->t.d.depth;
            out[n].view_present = r->has_view;
            if (r->has_view) {
                out[n].view_complete = r->v.set_format && r->v.set_target && r->v.set_layers;
                out[n].view_format = r->v.format;
                out[n].view_target = r->v.target;
                out[n].view_layers = r->v.nlayers;
            }
            break;
        }
    }
    *generation = atomic_load(&g_registry_generation);
    pthread_mutex_unlock(&g_reg_mu);
    return 0;
}

// 注册的 tex_t 快照 → 会话纹理（视图）。找不到/格式不支持时写 why，不返回占位纹理。
int s2a_vk_adapter_tex_by_id(int id, struct nvn_vk_tex **out, char *why, size_t why_size) {
    if (!out) return -1;
    tex_t t;
    int has = 0;
    pthread_mutex_lock(&g_reg_mu);
    unsigned h = (unsigned)id * 2654435761u;
    // 探测半径必须是当前容量 g_tex_cap：登记/增长都按它取模（reg_grow_locked）。
    // 用初始常量 NVK_TEXREG 时，表增长后哈希顺序整体改变，已登记项会变得查不到。
    // 容量为 0（注册表还没建）时循环不执行，走下面“没有登记”的准确失败。
    for (int probe = 0; probe < g_tex_cap; probe++) {
        int i = (int)((h + (unsigned)probe) % (unsigned)g_tex_cap);
        if (!g_tex_reg[i].used) break;
        if (g_tex_reg[i].id == id) { t = g_tex_reg[i].t; has = 1; break; }
    }
    pthread_mutex_unlock(&g_reg_mu);
    if (!has) { if (why) snprintf(why, why_size, "texture id %d is not registered", id); return -1; }
    struct nvn_vk_tex *tex = s2a_vk_adapter_tex(&t, why, why_size);
    if (!tex || !tex->view) { if (why && !why[0]) snprintf(why, why_size, "texture id %d has no view", id); return -1; }
    *out = tex;
    return 0;
}

// The scope includes waiting/early rejection, not just the builder call. No
// payload is inspected: an intact opcode header only declares an expected shader.
static void observation_expect_shader(struct s2a_w2_scope *scope,
                                      const uint8_t *packet, size_t len) {
    if (scope->entered && packet && len >= sizeof(op_hdr)) {
        op_hdr header;
        memcpy(&header, packet, sizeof header);
        if (header.op == OP_DRAW || header.op == OP_DRAW_IDX || header.op == OP_DISPATCH)
            s2a_w2_scope_expect_shader(scope);
    }
}

// GPU 侧事件写（OP_SIGNAL_EVENT，且不带 NVN_EV_WAIT 的“等事件”位）的完成语义。
// NVN：nvnCommandBufferSignalEvent / nvnCommandBufferReportValue 由 GPU 执行到这一条时才写值，客户读到
// 新值即代表它之前的命令都已完成（Metal 侧在提交完成回调里落地，见 graphics/nvn/gfx/nvn/nvn_mtl.m mtl_sub_event）。
// 本后端回放时用 CPU 直接写 guest 值，那一刻之前录制的 draw 还只在帧队列里没提交：值会提前可见，客户据此
// 读回纹理/缓冲会读到旧内容。所以回放这条之前先要一份完成证明——用与“CPU 可见 clear/copy/计数报告”
// 相同的边界（关 open pass + 提交 + 等 fence；没有在途工作时立即返回，不加成本）。
// 拿不到证明就不回放这一条：guest 值一个字节都不写，错误原样返回调用方（fail-closed）。
// 等待点等价于 Metal 的“提交完成回调”（管线底部）；NVNeventSignalLocation 的 TOP/VERTEX_PIPE 更早的
// 位置差异按审计 P2 接受（nvn_mtl.m 也是这样近似的）。
#define S2A_EVENT_NV_WAIT_BIT 0x80000000u   // NVN_EV_WAIT：nvnCommandBufferWaitEvent，见 graphics/nvn/gfx/nvn/nvn_mtl.h

static int adapter_event_signal_needs_proof(const uint8_t *packet, size_t len) {
    if (!packet || len < sizeof(op_hdr)) return 0;
    op_hdr header;
    memcpy(&header, packet, sizeof header);
    if (header.op != OP_SIGNAL_EVENT || header.size < 24 || header.size > len) return 0;
    uint32_t mode = 0;
    memcpy(&mode, packet + sizeof(op_hdr) + 8, 4);
    return (mode & S2A_EVENT_NV_WAIT_BIT) == 0;
}

// 返回 0 = 可以回放（有完成证明，或本来就没有前序 GPU 工作）；-1 = 拿不到证明，已写 result，禁止回放。
static int adapter_event_completion_gate(const uint8_t *packet, size_t len, struct s2a_record_result *result) {
    if (!adapter_event_signal_needs_proof(packet, len)) return 0;
    if (nvn_vk_first_draw_pass_break(1) == 0) return 0;
    if (result) {
        const char *first = nvn_vk_first_draw_first_error();
        result->code = S2A_RECORD_GPU;
        result->opcode = OP_SIGNAL_EVENT;
        snprintf(result->why, sizeof result->why,
                 "event signal has no completion proof for earlier GPU work; the guest value is not written: %s",
                 first && first[0] ? first : "frame queue has no completion proof");
    }
    return -1;
}

static int s2a_vk_adapter_exec_impl(const uint8_t *packet, size_t len,
                        const struct s2a_record_memory *memory, struct s2a_record_result *result,
                        struct s2a_w2_scope *observation) {
    if (!g_ready) { if (result) { result->code = S2A_RECORD_ARGUMENT; snprintf(result->why, sizeof result->why, "adapter not initialized"); } return S2A_RECORD_ARGUMENT; }
    observation_expect_shader(observation, packet, len);
    if (adapter_event_completion_gate(packet, len, result) != 0) return S2A_RECORD_GPU;
    return s2a_record_exec_one(&g_session.device, packet, len, memory, result);
}

static int guest_rd(void *o, uint64_t addr, void *dst, size_t n) { (void)o; if (!addr || !dst) return -1; memcpy(dst, (const void *)(uintptr_t)addr, n); return 0; }
// 拍平期 guest 直读（mtl_exec 里把命令数据所有权转移到宿主时用）：延迟参数数组、录制时失效但提交时
// 可能已重建的 tex 对象都从这里取一次。地址口径与记录执行时 mtl_exec 的 guest_rd 相同（HLE 池直映射）。
// 返回非 0 = 该地址此刻不可读；调用方必须让整笔提交不发布完成证明，不能按 0 放行。
int s2a_vk_adapter_guest_read(uint64_t address, void *dst, size_t bytes) {
    return guest_rd(NULL, address, dst, bytes);
}
static int guest_wr(void *o, uint64_t addr, const void *src, size_t n) { (void)o; if (!addr || !src) return -1; memcpy((void *)(uintptr_t)addr, src, n); return 0; }

// 回放器在静态纹理表未命中时回调这里：记录的 const tex_t* → 会话纹理（按 CPU 内容建/上传）。
static int record_resolve_tex(void *opaque, const void *key, struct s2a_replay_texture *out) {
    struct nvn_vk_session *s = (struct nvn_vk_session *)opaque;
    if (!s || !key || !out) return 0;
    char why[160] = "";
    struct nvn_vk_tex *tex = nvn_vk_session_tex(s, (const tex_t *)key, why, sizeof why);
    if (!tex || !tex->image) return 0;
    memset(out, 0, sizeof *out);
    out->image = tex->image;
    out->serial = tex->serial;
    out->width = tex->width;
    out->height = tex->height;
    out->depth = tex->depth;
    out->levels = tex->levels;
    out->layers = tex->layers;
    VkFormat fmt = nvn_vk_format(tex->nvn_format);
    out->format = fmt;
    return fmt != VK_FORMAT_UNDEFINED;
}

// 仅在回放执行持有 adapter 的互斥锁时调用；不建纹理、不上传，身份与 image 双重核对。
void *s2a_vk_adapter_clear_tex(uint64_t serial, VkImage image) {
    if (!g_ready || !serial || !image) return NULL;
    struct nvn_vk_tex *tex = nvn_vk_texcache_find_serial(&g_session.textures, serial);
    return tex && tex->image == image ? tex : NULL;
}

// 诊断口径：逐命令的等锁统计默认关（每条命令两次 clock_gettime 只服务这一行日志）。
static int adapter_cmd_stats(void) {
    static int v = -1;
    if (v < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.cmd_stats", b); v = b[0] ? atoi(b) : 0; }
    return v;
}
static _Atomic unsigned g_exec_waiters;


static int s2a_vk_adapter_exec_guest_impl(const uint8_t *packet, size_t len,
                                           struct s2a_record_result *result,
                                           struct s2a_w2_scope *observation) {
    const int stats = adapter_cmd_stats();
    const uint64_t wait_t0 = stats ? adapter_now_ns() : 0;
    const uint64_t me = adapter_thread_token();
    pthread_mutex_lock(&g_mu);
    // 同一时刻只有一个执行包；挂起中的执行包在等需求编译时，其他 guest 线程在这里排队，
    // 不能越过这个 draw（§4.3“同 queue 后续工作不越过该 draw”）。
    int entered;
    for (;;) {
        entered = nvk_exec_owner_enter(&g_exec, me);
        if (entered != NVK_EXEC_ENTER_WAIT) break;
        if (g_cancel) break;
        // 只有真排队时才算等待者；离开路径只在有人等时广播（没人等时每次广播都是一次 futex 系统调用）。
        atomic_fetch_add_explicit(&g_exec_waiters, 1, memory_order_relaxed);
        pthread_cond_wait(&g_exec_cv, &g_mu);
        atomic_fetch_sub_explicit(&g_exec_waiters, 1, memory_order_relaxed);
    }
    if (!g_ready || g_cancel || entered != NVK_EXEC_ENTER_READY) {
        // 已经拿到独占但会话同时在收尾：必须归还执行包，不能让下一个会话看到永远 busy 的旧状态。
        // 只有本线程真的拿到 READY 时才清镜像；WAIT/STOPPED 不能清掉另一个在途包的标记。
        if (entered == NVK_EXEC_ENTER_READY) {
            (void)nvk_exec_owner_leave(&g_exec, me);
            atomic_store(&g_exec_busy_mirror, 0);
        }
        if (result) { result->code = S2A_RECORD_ARGUMENT; snprintf(result->why, sizeof result->why, "adapter not initialized or stopping"); }
        if (atomic_load_explicit(&g_exec_waiters, memory_order_relaxed)) pthread_cond_broadcast(&g_exec_cv);
        pthread_mutex_unlock(&g_mu);
        return S2A_RECORD_ARGUMENT;
    }
    if (stats) {
        atomic_fetch_add_explicit(&g_exec_lock_wait_ns, adapter_now_ns() - wait_t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_exec_lock_calls, 1, memory_order_relaxed);
    }
    atomic_store(&g_exec_busy_mirror, 1);
    {
        unsigned gen = atomic_load_explicit(&g_reg_gen, memory_order_relaxed);
        if (tls_reg_gen != gen) {
            s2a_record_set_tex_resolver(record_resolve_tex, &g_session);
            s2a_record_set_draw_builder(nvk_draw_builder, g_shader_dir);
            tls_reg_gen = gen;
        }
    }
    struct s2a_record_memory m = { NULL, guest_rd, guest_wr };
    observation_expect_shader(observation, packet, len);
    // 事件信号先要完成证明，拿不到就不回放（guest 值不写）——见 adapter_event_completion_gate。
    if (adapter_event_completion_gate(packet, len, result) != 0) {
        (void)nvk_exec_owner_leave(&g_exec, me);
        atomic_store(&g_exec_busy_mirror, 0);
        if (atomic_load_explicit(&g_exec_waiters, memory_order_relaxed)) pthread_cond_broadcast(&g_exec_cv);
        pthread_mutex_unlock(&g_mu);
        return S2A_RECORD_GPU;
    }
    int rc = s2a_record_exec_one(&g_session.device, packet, len, &m, result);
    (void)nvk_exec_owner_leave(&g_exec, me);
    atomic_store(&g_exec_busy_mirror, 0);
    if (atomic_load_explicit(&g_exec_waiters, memory_order_relaxed)) pthread_cond_broadcast(&g_exec_cv);
    pthread_mutex_unlock(&g_mu);
    return rc;
}

// NVN ReadTexels：与 mtl_exec 同一套执行包协议——读回是 CPU 可见操作，必须排在已经入队的
// GPU 工作之后，且不能与其他 guest 线程的回放交错。拿不到执行包/会话在收尾时如实失败。
int s2a_vk_adapter_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                               uint8_t *dst, size_t row, size_t img, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    const uint64_t me = adapter_thread_token();
    pthread_mutex_lock(&g_mu);
    int entered;
    for (;;) {
        entered = nvk_exec_owner_enter(&g_exec, me);
        if (entered != NVK_EXEC_ENTER_WAIT) break;
        if (g_cancel) break;
        atomic_fetch_add_explicit(&g_exec_waiters, 1, memory_order_relaxed);
        pthread_cond_wait(&g_exec_cv, &g_mu);
        atomic_fetch_sub_explicit(&g_exec_waiters, 1, memory_order_relaxed);
    }
    int rc = -1;
    if (!g_ready || g_cancel || entered != NVK_EXEC_ENTER_READY) {
        if (entered == NVK_EXEC_ENTER_READY) {
            (void)nvk_exec_owner_leave(&g_exec, me);
            atomic_store(&g_exec_busy_mirror, 0);
        }
        if (why && why_size) snprintf(why, why_size, "adapter not initialized or stopping");
        if (atomic_load_explicit(&g_exec_waiters, memory_order_relaxed)) pthread_cond_broadcast(&g_exec_cv);
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    atomic_store(&g_exec_busy_mirror, 1);
    rc = nvn_vk_session_read_texels(&g_session, t, level, layer0, r, dst, row, img, why, why_size);
    (void)nvk_exec_owner_leave(&g_exec, me);
    atomic_store(&g_exec_busy_mirror, 0);
    if (atomic_load_explicit(&g_exec_waiters, memory_order_relaxed)) pthread_cond_broadcast(&g_exec_cv);
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int s2a_vk_adapter_exec(const uint8_t *packet, size_t len,
                        const struct s2a_record_memory *memory, struct s2a_record_result *result) {
    struct s2a_w2_scope observation = {0};
    s2a_w2_scope_enter(&observation);
    int rc = s2a_vk_adapter_exec_impl(packet, len, memory, result, &observation);
    s2a_w2_scope_leave(&observation, rc == S2A_RECORD_OK);
    return rc;
}

int s2a_vk_adapter_exec_guest(const uint8_t *packet, size_t len, struct s2a_record_result *result) {
    struct s2a_w2_scope observation = {0};
    s2a_w2_scope_enter(&observation);
    int rc = s2a_vk_adapter_exec_guest_impl(packet, len, result, &observation);
    s2a_w2_scope_leave(&observation, rc == S2A_RECORD_OK);
    return rc;
}

// 自上次读取以来的“等 g_mu / 等执行包让出”总时间与次数（窗口汇总用；读取即清零）。
void s2a_vk_adapter_exec_lock_stats(uint64_t *wait_ns, uint64_t *calls) {
    if (wait_ns) *wait_ns = atomic_exchange(&g_exec_lock_wait_ns, 0);
    if (calls) *calls = atomic_exchange(&g_exec_lock_calls, 0);
}

int s2a_vk_adapter_ready(void) { return g_ready; }
struct nvn_vk_session *s2a_vk_adapter_session(void) { return g_ready ? &g_session : NULL; }
