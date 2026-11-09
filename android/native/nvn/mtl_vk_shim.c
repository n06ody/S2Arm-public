// 前端 mtl_* → S2Arm 自己的 Vulkan 后端（nvn_vk_* / mtl_vk_adapter）的整表 shim。
//
// 设计口径（与 docs/handoff-direct-mode.md 的下一步一致）：
//   - 核心入口真的接后端：init/finish/drain/pool_*/addr/exec/sub_*/tex_*/present，以及地址与
//     上屏的通路；执行语义是**同步**的：mtl_exec 里就把每条记录提交并等 GPU 完成，
//     commit 只是收尾（事件/栅栏在提交前已经可见，符合“事件值只在 GPU 完成后可见”的约定）。
//   - 没有读数、或对画面没有可观察影响的遥测入口返回“无读数”（0 / 空），这是诚实语义，不是成功兜底：
//     它们不改变任何 GPU 状态。
//   - 还没接的通路（间接绘制参数、纹理读回、外部 producer）**显式红**：LOG 出具体原因后 abort，
//     不静默丢掉一条会影响画面的命令。
//
// 旧 d2r 回放引擎副本已删除（2026-10-30）：前端 mtl_* 现在只有这一份后端实现，链本文件。
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvn_vk_first_draw.h"   // 绘制路径管线创建计数（longframe 归因用）
#include <string.h>
#include <time.h>

#include "mtl_vk_adapter.h"
#include "nvn_obj.h"
#include "redlog.h"
#pragma weak nvn_ga2ca
#include "nvn_mtl.h"
#include "vk_record.h"
#include "record_flat.h"   // A2/A4：拍平记录并重定位记录内引用（职责在 direct/，本文件不再自己做字节复制）

#if defined(__ANDROID__)
#include <android/log.h>
#define SHIM_TAG "S2Arm"
#endif

static void shim_log(const char *level, const char *fmt, va_list ap) {
#if defined(__ANDROID__)
    __android_log_vprint(!strcmp(level, "RED") ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, SHIM_TAG, fmt, ap);
#else
    fprintf(stderr, "[nvk-shim][%s] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
}

// 窗口统计打印开关（默认关）：每 16 次 present 会刷十几行，其中内存账要读 /proc，
// 属于“量”的成本——不能默认付。
static int shim_win_stats_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.win_stats", b);
        v = (b[0] && atoi(b) != 0) ? 1 : 0;
    }
    return v;
}

// 时间线打点开关（默认关，属性 debug.s2a.timeline）：exec_chain 对每条命令都要取一次墙钟
// （世界帧 3800-4200 次/帧，报告 §6.4/§7）。打开才付这笔成本；关闭时 frame_stats 的
// timeline 各段如实输出「未测」，不写 0。首读定值，改属性要重启进程。
static int shim_timeline_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.timeline", b);
        v = (b[0] && atoi(b) != 0) ? 1 : 0;
    }
    return v;
}

// draw 判定：NVN 的索引绘制（OP_DRAW_IDX）和非索引绘制（OP_DRAW）是两种记录，枚举在
// nvn_obj.h。只认其中一种会把另一种静默算进「非draw」桶，并让 FIRST_DRAW 时间线在非索引流里
// 永远不打点（报告 §6.1）。用枚举名，不写死 25/26。
static int shim_is_draw_op(uint32_t op) { return op == OP_DRAW || op == OP_DRAW_IDX; }

static void info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); shim_log("INFO", fmt, ap); va_end(ap);
}

// 显式红：具体原因写进日志后终止本进程。丢掉一条会影响画面的命令不是可以“继续跑”的状态。
static void red(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); shim_log("RED", fmt, ap); va_end(ap);
    // 红日志同时落盘：进程 abort 后 logcat 里那一行可能来不及刷出。落盘目录由 Java 侧按当前
    // 应用注入（getExternalFilesDir() / getFilesDir()），这里只拼 red.log；同一行也写宿主日志
    // fd，abort 前两边 fsync（#1357）。
    va_list aq; va_start(aq, fmt); s2a_android_red_v(fmt, aq);   // 不返回
}

static void note_once(int *flag, const char *fmt, ...) {
    if (*flag) return;
    *flag = 1;
    va_list ap; va_start(ap, fmt); shim_log("INFO", fmt, ap); va_end(ap);
}

// ---------------- 状态 ----------------
static int g_sub_active;          // mtl_sub_begin..commit 之间
static uint64_t g_fence;          // 同步后端：每提交一次 +1
// 拍平失败（OOM/格式不合法）时不能“丢命令还发 fence”：本次提交不交给翻译线程，记下第一笔
// 丢失的提交 fence；之后 mtl_wait / present / finish 对它准确失败，不发布完成。
static int g_clone_lost;
static uint64_t g_lost_from_fence;
// 已经拿到完成证明的提交上界（guest fence 号）。只按调用方传入的 fence 推进，不拿 g_fence 反推：
// 多线程下别的线程可能已经发布了更大的 fence 号但作业还没回放。
static _Atomic uint64_t g_completed_fence;
static uint64_t g_present_n;
static double g_present_at[256];
static uint64_t g_submits, g_commands, g_draws_skipped;
static int g_depth_zero, g_origin_lower_left, g_tex_pool_n, g_frame_set;
static uint64_t g_frame;
static int g_notes[8];
// 正常模式帧账：把 guest 逻辑、录制执行、present 分开，避免把游戏自身 CPU 误算到后端。
static uint64_t g_frame_exec_ns /*ticks*/, g_frame_cmds;
static int g_frame_first_draw_marked;
// 逐 opcode 的耗时账：NVN 前端每帧上千条状态命令，必须先看清是哪一类在吃时间。
static uint64_t g_op_ns[256], g_op_n[256];
static uint64_t shim_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
// 逐命令计时用的是“读一次虚拟计数器”，不是 clock_gettime：x86/arm64 上 clock_gettime 走 vDSO
// 也要几十 ns，3800 条/帧 × 2 次就是 0.5 ms 级的固定开销。CNTVCT 只要一条 mrs，且全核单调；
// 采集时累加原始 tick，只在每帧/每窗口汇报时换算成 ns。
static uint64_t g_ticks_freq;
static inline uint64_t shim_ticks(void) {
#if defined(__aarch64__)
    uint64_t v; __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    return shim_now_ns();
#endif
}
static double shim_ticks_ns(uint64_t t) {
#if defined(__aarch64__)
    if (!g_ticks_freq) { uint64_t f; __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f)); g_ticks_freq = f ? f : 1; }
    return (double)t * 1e9 / (double)g_ticks_freq;
#else
    return (double)t;
#endif
}

// ---------------- 图形翻译线程（照 d2r-mobile 的形状）----------------
// 游戏线程只录制并把“执行这段命令”入队，翻译+提交在独立线程按序做，两者流水。
// 关键不是多开线程，而是入队后游戏能安全继续跑——所以要点是：
//   * 单线程串行队列：顺序天然保证，后端状态只被一个线程碰；
//   * 有界队列：满时有界等待，不把风险换成无界内存；
//   * 明确的 drain 点：只在“游戏会读结果 / 会复用命令内存”之前跑空队列；
//   * Stop 能唤醒退出。
// debug.s2a.gfx_async: 0=关闭（默认，行为与之前完全一致）、
//                      1=保守（每次 sub_begin 前 drain）、
//                      2=激进（只在 present / wait 前 drain）。
enum { SHIM_ASYNC_OFF = 0, SHIM_ASYNC_CONSERVATIVE = 1, SHIM_ASYNC_AGGRESSIVE = 2 };
#define SHIM_JOB_MAX 4096
struct shim_job { void (*fn)(void *); void *arg; };
static int g_async_mode = -1;
static struct shim_job g_jobs[SHIM_JOB_MAX];
static int g_job_head, g_job_tail, g_job_n;
static pthread_t g_gfx_thread;
static pthread_t g_gfx_tid;
static int g_gfx_tid_valid, g_gfx_stop, g_busy;
static pthread_mutex_t g_job_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_idle_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_async_enqueued, g_async_executed, g_async_block_ns;
static int shim_async_mode(void) {
    if (g_async_mode < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.gfx_async", b);
        g_async_mode = b[0] ? atoi(b) : SHIM_ASYNC_OFF;
        if (g_async_mode < 0 || g_async_mode > SHIM_ASYNC_AGGRESSIVE) g_async_mode = SHIM_ASYNC_OFF;
        // 模式 2 曾经会让进程消失（游戏侧命令内存被 fl_drop 回收后翻译线程仍在读）。
        // 现在 mtl_exec 已经把命令链拍平成本进程自有快照，游戏侧内存可以立刻回收，
        // 这条前提已经满足；仍保留提示，方便一眼看出跑的是哪一档。
        if (g_async_mode == SHIM_ASYNC_AGGRESSIVE)
            info("gfx_async=2（激进）：命令已拍平为自有快照，只在 present/wait 前 drain");
    }
    return g_async_mode;
}
// 把“已证明完成”的 guest fence 上界推进到 through（只增不减）。
static void shim_note_completed(uint64_t through) {
    uint64_t cur = atomic_load(&g_completed_fence);
    while (through > cur && !atomic_compare_exchange_weak(&g_completed_fence, &cur, through)) {}
}
static int shim_on_gfx_thread(void) {
    return g_gfx_tid_valid && pthread_equal(pthread_self(), g_gfx_tid);
}
// ADPF 帧跨距：排队作业按执行时所在的帧序号记账（序号只在 shim_frame_settle 翻转，
// 即每条 present 出口的末尾；所以「两次 settle 之间执行的作业」就是这一帧的工作）。
// 独立 NVN 探针可能不链接宿主提示模块，弱符号缺失时整块不启用。
extern void s2a_adpf_work_begin(uint64_t seq) __attribute__((weak));
extern void s2a_adpf_frame_end(int64_t target_ns, uint64_t seq) __attribute__((weak));
extern void s2a_adpf_discard(void) __attribute__((weak));
extern void s2a_adpf_invisible(void) __attribute__((weak));
static _Atomic uint64_t g_frame_seq;
static void adpf_work_tick(void) {
    if (s2a_adpf_work_begin) s2a_adpf_work_begin(atomic_load(&g_frame_seq) + 1);
}
static void *shim_gfx_main(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "s2a-gfx");
    for (;;) {
        struct shim_job job;
        pthread_mutex_lock(&g_job_mu);
        while (!g_gfx_stop && g_job_n == 0) pthread_cond_wait(&g_job_cv, &g_job_mu);
        if (g_gfx_stop && g_job_n == 0) { pthread_mutex_unlock(&g_job_mu); return NULL; }
        job = g_jobs[g_job_head];
        g_job_head = (g_job_head + 1) % SHIM_JOB_MAX;
        g_job_n--;
        g_busy = 1;
        pthread_cond_broadcast(&g_idle_cv);   // 腾出槽位，阻塞的入队方可以继续
        pthread_mutex_unlock(&g_job_mu);
        adpf_work_tick();
        job.fn(job.arg);
        pthread_mutex_lock(&g_job_mu);
        g_busy = 0;
        atomic_fetch_add(&g_async_executed, 1);
        pthread_cond_broadcast(&g_idle_cv);
        pthread_mutex_unlock(&g_job_mu);
    }
}
static void shim_async_start(void) {
    if (shim_async_mode() == SHIM_ASYNC_OFF || g_gfx_tid_valid) return;
    if (pthread_create(&g_gfx_thread, NULL, shim_gfx_main, NULL) != 0) {
        g_async_mode = SHIM_ASYNC_OFF;
        red("gfx_async: worker thread creation failed; falling back to synchronous translation");
        return;
    }
    // 由创建方立刻置位：pthread_t 就是 worker 的线程句柄。若等 worker 自己置位，存在
    // “作业已入队、worker 还没被调度”的窗口，这期间 drain 会误以为没有 worker 而直接跳过。
    g_gfx_tid = g_gfx_thread;
    g_gfx_tid_valid = 1;
    info("gfx_async: translation thread started (mode=%d)", shim_async_mode());
}
static int exec_chain(const uint8_t *pc, int depth);

// ---- 命令链拍平（照 d2r-mobile 的 xbg_copy_commands / xbg_command_clone）----
// 游戏在提交之后会立刻销毁/复用命令内存（hle_nvn 的 fl_drop / mtl_pool_add），
// 所以翻译线程不能持有游戏侧指针。这里在 mtl_exec 时就把整条链**拍平**成我们自有的
// 线性记录：OP_JUMP 直接跟进目标、OP_CALL 就地内联被调链、OP_END 丢弃（末尾统一补一个）。
// 拍平只花一次 memcpy，翻译仍留在 worker 上。
struct shim_flat { uint8_t *buf; size_t len; uint64_t fence; };
// 已经回放完成的提交上界（按 guest fence 号）。只有“已回放”的提交才可能被一次成功 flush
// 证明完成；fence 在入队前分配，所以不能用 g_fence 反推（并发提交时它可能超前于回放）。
static _Atomic uint64_t g_replayed_fence;
// A2/A4：命令流拍平与缓冲记账都在 direct/record_flat.c（它自有缓冲、定稿时才落记录内地址，
// 因为缓冲会随追加增长而搬移，地址早写就会悬垂）。这里只保留“追加一条记录”这一个调用。
static int clone_append(const uint8_t *p, size_t n) {
    return s2a_record_flat_append(p, n, NULL, 0) ? -1 : 0;
}
static int clone_chain(const uint8_t *pc, int depth) {
    if (depth > 8) return -1;
    for (int steps = 0; steps < 1000000; steps++) {
        if (!pc) return -1;
        op_hdr h;
        memcpy(&h, pc, sizeof h);
        if (h.size < sizeof h || (h.size & 7u) || h.size > (1u << 20)) return -1;
        if (h.op == OP_END) return 0;
        if (h.op == OP_JUMP) {
            uint64_t target = 0;
            if (h.size < 16) return -1;
            memcpy(&target, pc + sizeof h, sizeof target);
            pc = (const uint8_t *)(uintptr_t)target;
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t target = 0;
            if (h.size < 16) return -1;
            memcpy(&target, pc + sizeof h, sizeof target);
            if (clone_chain((const uint8_t *)(uintptr_t)target, depth + 1)) return -1;
            pc += h.size;
            continue;
        }
        if (clone_append(pc, h.size)) return -1;
        pc += h.size;
    }
    return -1;
}
static void job_flat(void *arg) {
    struct shim_flat *f = (struct shim_flat *)arg;
    if (!f) return;
    if (f->buf) (void)exec_chain(f->buf, 0);
    // 命令已经全部进入帧队列/当前 pass（不代表已提交或完成）。推进“已回放上界”。
    if (f->fence) {
        uint64_t cur = atomic_load(&g_replayed_fence);
        while (f->fence > cur && !atomic_compare_exchange_weak(&g_replayed_fence, &cur, f->fence)) {}
    }
    free(f->buf);
    free(f);
}
static void job_reset_state(void *arg) { (void)arg; s2a_record_reset_state(); }

// 入队：有界；队列满时有界等待（等到有空位或超时），绝不无界增长。
static int shim_enqueue(void (*fn)(void *), void *arg) {
    if (shim_async_mode() == SHIM_ASYNC_OFF || shim_on_gfx_thread()) { adpf_work_tick(); fn(arg); return 0; }
    shim_async_start();
    if (shim_async_mode() == SHIM_ASYNC_OFF) { adpf_work_tick(); fn(arg); return 0; }
    const uint64_t t0 = shim_now_ns();
    pthread_mutex_lock(&g_job_mu);
    while (g_job_n == SHIM_JOB_MAX && !g_gfx_stop) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 200000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&g_idle_cv, &g_job_mu, &dl) == ETIMEDOUT) break;
    }
    if (g_job_n == SHIM_JOB_MAX) { pthread_mutex_unlock(&g_job_mu); red("gfx_async: job queue stayed full; refusing to grow it unbounded"); return -1; }
    g_jobs[g_job_tail] = (struct shim_job){ fn, arg };
    g_job_tail = (g_job_tail + 1) % SHIM_JOB_MAX;
    g_job_n++;
    atomic_fetch_add(&g_async_enqueued, 1);
    pthread_cond_signal(&g_job_cv);
    pthread_mutex_unlock(&g_job_mu);
    atomic_fetch_add(&g_async_block_ns, shim_now_ns() - t0);
    return 0;
}
// 有界 drain（guest fence 的等待预算）：等到队列空并返回 0；预算用尽返回 1。
// 不改变 unbounded 版本；timeout_ns<0 = 无限等。
static int shim_drain_budget(int64_t budget_ns) {
    if (shim_async_mode() == SHIM_ASYNC_OFF || shim_on_gfx_thread() || !g_gfx_tid_valid) return 0;
    int64_t end = budget_ns < 0 ? -1 : (int64_t)shim_now_ns() + budget_ns;
    pthread_mutex_lock(&g_job_mu);
    while (g_job_n > 0 || g_busy) {
        if (end < 0) { pthread_cond_wait(&g_idle_cv, &g_job_mu); continue; }
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        int64_t left = end - (int64_t)shim_now_ns();
        if (left <= 0) { pthread_mutex_unlock(&g_job_mu); return 1; }
        dl.tv_sec += (time_t)(left / 1000000000ll);
        dl.tv_nsec += (long)(left % 1000000000ll);
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&g_idle_cv, &g_job_mu, &dl) == ETIMEDOUT) {
            if (g_job_n > 0 || g_busy) { pthread_mutex_unlock(&g_job_mu); return 1; }
        }
    }
    pthread_mutex_unlock(&g_job_mu);
    return 0;
}
// drain：唯一同步点。等到队列空且没有在跑的作业。
static void shim_drain(void) {
    if (shim_async_mode() == SHIM_ASYNC_OFF || shim_on_gfx_thread() || !g_gfx_tid_valid) return;
    pthread_mutex_lock(&g_job_mu);
    while (g_job_n > 0 || g_busy) pthread_cond_wait(&g_idle_cv, &g_job_mu);
    pthread_mutex_unlock(&g_job_mu);
}
// 帧边界结算（报告 §6.5）：本帧的 exec 账/命令数只属于本帧。present 因无窗口、present 中被撤销、
// acquire 超时被跳过时，如果不清账，下一帧样本的 exec 会是两帧以上的累加，而 gap 只有单帧，
// 于是 exec>gap、other 为负、FIRST_CMD 时间线也不再打点。任何“这一帧结束”的出口都走这里，
// 跳过的帧不产生样本（不把它的 exec 摊到下一帧）。
// 异步翻译档下 worker 可能还在回放本帧命令：先 drain 再清零，否则清零后的增量会混进下一帧，
// 且与游戏线程的清零构成数据竞争。同步档（默认）shim_drain 直接返回，不产生等待。
static void shim_frame_settle(void) {
    shim_drain();
    g_frame_first_draw_marked = 0;
    g_frame_exec_ns = 0;
    g_frame_cmds = 0;
    // 帧边界：drain 之后的作业记到下一帧。必须在 drain 之后翻，否则在 drain 里执行完
    // 的本帧作业会拿错序号；也不清 pending——晚于 end 登记的作业带旧序号，下一帧的
    // end 按序号对不上自然丢弃。
    atomic_fetch_add(&g_frame_seq, 1);
}
static void shim_async_stop(void) {
    if (!g_gfx_tid_valid) return;
    pthread_mutex_lock(&g_job_mu);
    g_gfx_stop = 1;
    pthread_cond_broadcast(&g_job_cv);
    pthread_mutex_unlock(&g_job_mu);
    pthread_join(g_gfx_thread, NULL);
    g_gfx_tid_valid = 0;
    g_gfx_stop = 0;
}

// 供 provider stop 调用：先把队列跑空，再唤醒并 join 翻译线程。
void s2a_mtl_async_stop(void) {
    if (shim_async_mode() == SHIM_ASYNC_OFF || !g_gfx_tid_valid) return;
    shim_drain();
    shim_async_stop();
}

static void nvk_require(void) {
    if (s2a_vk_adapter_ready()) return;
    char why[192] = "";
    if (s2a_vk_adapter_init(why, sizeof why)) red("mtl_init: Vulkan session init failed: %s", why[0] ? why : "unknown");
    info("mtl_init: Vulkan session ready (S2Arm nvn_vk backend)");
}

// 一条记录链的单步执行：OP_CALL 递归、OP_JUMP 跳转、OP_END 结束，其余交给回放器。
static int exec_chain(const uint8_t *pc, int depth) {
    if (depth > 8) red("mtl_exec: CALL depth exceeds 8 at %p", (const void *)pc);
    for (int steps = 0; steps < 1000000; steps++) {
        if (!pc) red("mtl_exec: null command handle");
        op_hdr h;
        memcpy(&h, pc, sizeof h);
        if (h.size < sizeof h || (h.size & 7u) || h.size > (1u << 20))
            red("mtl_exec: bad record header op=%u size=%u at %p", h.op, h.size, (const void *)pc);
        if (h.op == OP_END) return 0;
        if (h.op == OP_JUMP) {
            uint64_t target = 0;
            if (h.size < 16) red("mtl_exec: OP_JUMP payload too small");
            memcpy(&target, pc + sizeof h, sizeof target);
            pc = (const uint8_t *)(uintptr_t)target;
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t target = 0;
            if (h.size < 16) red("mtl_exec: OP_CALL payload too small");
            memcpy(&target, pc + sizeof h, sizeof target);
            int rc = exec_chain((const uint8_t *)(uintptr_t)target, depth + 1);
            if (rc) return rc;
            pc += h.size;
            continue;
        }
        struct s2a_record_result r;
        if (shim_timeline_on()) {
            // 时间线：本帧首条 replay 命令（不是首笔 draw）与首笔 draw 分开打点；
            // frame id 用"正在录制的这一帧"= g_present_n + 1，与 present 侧的 id 一致。
            // 只有打开 debug.s2a.timeline 才会走到这里：关闭时每条命令都不取墙钟。
            extern void nvn_vk_frame_stats_tl_mark(uint64_t, int, double) __attribute__((weak));
            if (nvn_vk_frame_stats_tl_mark) {
                uint64_t fid = g_present_n + 1;
                double now = shim_now_ns() / 1e9;
                if (g_frame_cmds == 0) nvn_vk_frame_stats_tl_mark(fid, 0 /* FIRST_CMD */, now);
                if (shim_is_draw_op(h.op) && !g_frame_first_draw_marked) {
                    g_frame_first_draw_marked = 1;
                    nvn_vk_frame_stats_tl_mark(fid, 1 /* FIRST_DRAW */, now);
                }
            }
        }
        uint64_t t0 = shim_ticks();
        int rc = s2a_vk_adapter_exec_guest(pc, h.size, &r);
        uint64_t dt = shim_ticks() - t0;
        g_frame_exec_ns += dt; g_frame_cmds++;
        if (h.op < 256) { g_op_ns[h.op] += dt; g_op_n[h.op]++; }
        g_commands++;
        if (rc != S2A_RECORD_OK) {
            const char *kinds[] = { "ok", "argument", "invalid", "unsupported", "gpu", "memory", "depth", "limit" };
            int k = -rc; if (k < 0 || k > 7) k = 0;
            red("mtl_exec: NVN record op=%u failed (%s): %s", r.opcode ? r.opcode : h.op, kinds[k], r.why);
        }
        pc += h.size;
    }
    red("mtl_exec: command chain did not terminate within 1000000 records");
    return -1;
}

// ---------------- 生命周期 ----------------
void mtl_init(void) {
    nvk_require();
    // 新会话 = 新设备/新队列：上一会话的“丢失提交”不继承，但仍不伪造旧完成。
    g_clone_lost = 0;
    g_lost_from_fence = 0;
    atomic_store(&g_completed_fence, 0);
    atomic_store(&g_replayed_fence, 0);
    // 启动首读一次并把模式/进程绑定写进日志：模式 0 不启动 worker，如果只在启动 worker 时
    // 打日志，事后无法从截图/日志判断这一局到底跑的是哪一档。
    static int logged;
    if (!logged) {
        logged = 1;
        info("mtl_init: pid=%d gfx_async=%d (0=off/1=conservative/2=aggressive) backend=nvn_vk",
             (int)getpid(), shim_async_mode());
        // 开关名写进日志：默认关时 timeline 各段是「未测」，读日志的人必须能一眼看到口径。
        info("mtl_init: timeline=%d（开关 debug.s2a.timeline；0 时每条命令不取墙钟，timeline 段=未测）",
             shim_timeline_on());
    }
}

// nvnQueueFinish 的语义是“等队列完成”，不是“退出会话”。这里只提交+等待，
// 设备和缓存保持存活；真正退出走 provider stop → s2a_vk_adapter_shutdown()。
void mtl_finish(void) {
    if (!s2a_vk_adapter_ready()) return;
    shim_drain();
    if (g_lost_from_fence) {
        red("mtl_finish: submit #%llu lost its command chain; queue is not complete",
            (unsigned long long)g_lost_from_fence);
        return;
    }
    info("mtl_finish: submits=%llu commands=%llu presents=%llu (queue finish, session kept)",
         (unsigned long long)g_submits, (unsigned long long)g_commands, (unsigned long long)g_present_n);
    if (s2a_vk_adapter_finish()) red("mtl_finish: queue did not complete; resources retained");
    else shim_note_completed(atomic_load(&g_replayed_fence));
}

void mtl_drain(void) { nvk_require(); shim_drain(); }

// ---------------- 内存池 / GPU 地址 ----------------
void mtl_pool_add(const void *mem, uint64_t size) {
    nvk_require();
    if (!mem || !size) red("mtl_pool_add: invalid pool");
    if (s2a_vk_adapter_pool_add(mem, size)) red("mtl_pool_add: pool table full (4096)");
}

void mtl_pool_del(const void *mem) { s2a_vk_adapter_pool_del(mem); }

int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    if (nvn_ga2ca) addr = nvn_ga2ca(addr);   // 兜底：同苹果端 nvn_be_addr（hle_nvn.c GPU 地址空间）
    return s2a_vk_adapter_pool_lookup(addr, len, buf, off);
}

void mtl_alias_drop_pool(const void *pool) { (void)pool; }   // 没有跨池别名表：纹理按 (地址, serial) 缓存，见 nvn_vk_texcache

// ---------------- 提交 ----------------
void mtl_sub_begin(void) {
    nvk_require();
    if (g_sub_active) red("mtl_sub_begin: submit re-entered");
    g_sub_active = 1;
    // 保守档：在开始下一次提交之前把队列跑空。这保证 game 复用/释放上一提交的命令内存时，
    // 翻译线程已经不再读它——代价是只在「上一次 commit 到这一次 begin」之间能重叠。
    if (shim_async_mode() == SHIM_ASYNC_CONSERVATIVE) shim_drain();
    // 复位必须排在本次提交的 exec 作业之前，所以也走队列（不能就地做，会和 worker 上的状态打架）。
    shim_enqueue(job_reset_state, NULL);
    s2a_record_flat_reset();
}

void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }      // 前瞻索引只服务 Metal 的 pass 合并
int nvn_frec_on(void) { return 0; }      // 帧录放只在 Metal 后端（graphics/nvn/gfx/nvn/nvn_frame_rec.inc）
void nvn_frec_submit(const uint64_t *h, int n) { (void)h; (void)n; }
void nvn_frec_present(const tex_t *t, double period, const int crop[4]) { (void)t; (void)period; (void)crop; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }      // 同上

void mtl_exec(const uint8_t *handle) {
    nvk_require();
    if (!g_sub_active) red("mtl_exec: command outside submit");
    // 立刻拍平成自有快照：游戏随后销毁/复用命令内存也不会影响翻译线程。
    if (clone_chain(handle, 0)) {
        g_clone_lost = 1;
        red("mtl_exec: command chain could not be flattened (malformed record/OOM); "
            "this submit will not publish a completion fence");
    }
}

// 同步后端里的“GPU 顺序”就是调用顺序：记录之间的可见副作用用 CPU 直接做，
// 与“提交完成后再做”等价（每条记录在 mtl_exec 里已经等过 GPU 完成）。
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!src || !dst || !n) red("mtl_sub_copy: invalid range");
    memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
}

void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!dst || !n) red("mtl_sub_fill: invalid range");
    memset((void *)(uintptr_t)dst, (int)(value & 0xFF), (size_t)n);
}

void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) {
    if (!val) red("mtl_sub_event: null event");
    *val = mode ? (*val + v) : v;
}

void mtl_sub_counter(uint64_t addr, uint32_t type) {
    (void)addr; (void)type;
    note_once(&g_notes[0], "mtl_sub_counter: GPU counter writes have no reading on the nvn_vk backend yet (commands are not fused)");
}

uint64_t mtl_sub_commit(void) {
    if (!g_sub_active) red("mtl_sub_commit: commit without begin");
    g_sub_active = 0;
    g_submits++;
    // fence 在入队前分配：回放进度用它记录，不能事后从 g_fence 反推（并发提交时 g_fence
    // 可能已经大于实际回放到的提交）。
    const uint64_t f = ++g_fence;
    // 末尾补一个 OP_END，然后把这段自有的拍平记录交给翻译线程；游戏侧内存不再被引用。
    // 任何一步失败（拍平已经丢了命令、补 END 失败、包装对象 OOM、入队失败）都走同一个出口：
    // 这条提交不发布完成证明。不能“丢命令还发一个能等成功的 fence”。
    int queued = 0;
    if (!g_clone_lost) {
        op_hdr end = { OP_END, (uint32_t)sizeof(op_hdr) };
        if (clone_append((const uint8_t *)&end, sizeof end) == 0) {
            struct shim_flat *sh = (struct shim_flat *)malloc(sizeof *sh);
            if (sh) {
                sh->buf = NULL; sh->len = 0;
                sh->buf = s2a_record_flat_take(&sh->len);   // 定稿：落记录内地址并把所有权交出来
                if (sh->buf && shim_enqueue(job_flat, sh) == 0) {
                    queued = 1;
                } else {
                    free(sh->buf); free(sh);
                    g_clone_lost = 1;
                }
            } else {
                g_clone_lost = 1;
            }
        } else {
            g_clone_lost = 1;
        }
    }
    if (queued) return f;
    // 失败出口：丢弃这条不完整快照（缓冲仍归 record_flat，下一次提交开始时会重置），
    // 登记“从这笔起没有完成证明”。
    g_clone_lost = 0;
    s2a_record_flat_reset();
    if (!g_lost_from_fence) g_lost_from_fence = f;
    {
        static int noted;
        if (noted++ < 8)
            red("mtl_sub_commit: submit #%llu was not queued in full (flatten/END/OOM/queue); "
                "no completion proof (later waits/present/finish will fail accurately)",
                (unsigned long long)f);
    }
    return f;
}

uint64_t mtl_last_fence(void) { return g_fence; }

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (fence > g_fence) red("mtl_wait: fence %llu never submitted (last %llu)",
                             (unsigned long long)fence, (unsigned long long)g_fence);
    if (g_lost_from_fence && fence >= g_lost_from_fence) {
        static int noted;
        if (noted++ < 8)
            red("mtl_wait: fence %llu belongs to an incomplete submit (#%llu); no completion proof",
                (unsigned long long)fence, (unsigned long long)g_lost_from_fence);
        return 0;
    }
    // 这个 fence 之前的提交已经证明完成：不再 flush 后面更新、尚未完成的提交。
    // 否则“提交 A → 等 A 成功 → 提交 B（慢） → 轮询 A”会把已完成的 A 报成超时。
    if (fence && fence <= atomic_load(&g_completed_fence)) return 1;
    const int64_t budget = timeout_ns == 0 ? 0
                          : (int64_t)(timeout_ns > 5000000000ull ? 5000000000ull : timeout_ns);
    // 纯轮询（timeout=0）不制造副作用：不关 pass、不提交、不推进完成上界。
    // 不能证明就如实返回 0（guest 看到 TIMEOUT_EXPIRED）。
    if (budget == 0) return 0;
    const int64_t t0 = (int64_t)shim_now_ns();
    // 先让翻译队列把目标提交回放出去；回放没跑完就没有完成证明。
    int drc = shim_drain_budget(budget);
    if (drc) return 0;
    const uint64_t replayed = atomic_load(&g_replayed_fence);
    int64_t left = budget - ((int64_t)shim_now_ns() - t0);
    if (left < 0) left = 0;
    int rc = s2a_vk_adapter_flush_budget(left);
    if (rc == 0) {
        // 一次成功 flush 证明“已回放”的提交都完成了；但只把调用方要的 fence 记为已证明。
        uint64_t through = fence < replayed ? fence : replayed;
        shim_note_completed(through);
        return fence <= atomic_load(&g_completed_fence);
    }
    if (rc == 1 || rc == 2) return 0;   // 未完成/没拿到 owner 锁：guest 看到 TIMEOUT_EXPIRED
    {
        extern const char *nvn_vk_first_draw_first_error(void) __attribute__((weak));
        red("mtl_wait: fence %llu has no completion proof: %s", (unsigned long long)fence,
            nvn_vk_first_draw_first_error && nvn_vk_first_draw_first_error()
                ? nvn_vk_first_draw_first_error() : "queue did not complete");
        return 0;
    }
}

// ---------------- 纹理 ----------------
void mtl_tex_init(const tex_t *t) {
    nvk_require();
    char why[192] = "";
    if (!s2a_vk_adapter_tex(t, why, sizeof why))
        note_once(&g_notes[1], "mtl_tex_init: texture not bridged yet: %s", why[0] ? why : "unknown");
}

void mtl_tex_cpu_written(const tex_t *t) { s2a_vk_adapter_tex_cpu_written(t); }
void mtl_tex_release(tex_t *t) { s2a_vk_adapter_tex_release(t); }
int mtl_tex_gpu_owned(uint64_t serial) { return s2a_vk_adapter_tex_gpu_owned(serial); }
// 共享前端每 300 帧的纹理生命周期汇总（DM_NVN_ALLOCPROF）。本后端的账由 nvn_vk_session 的
// texcache usage 行给（live/pinned/retired/dirty），这里保持空实现，不另造第二套计数。
// nvn_be_tex_tick 由 nvn_be_android.c 提供（main 的纹理生命周期摘要）；shim 不再放空桩。

void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { s2a_vk_adapter_reg_tex(id, t, v); }
// 贴图池销毁时清编号段（hle_nvn.c dpool_finalize）：安卓登记表按编号哈希、不按最大编号开表，复用的编号由新登记覆盖，这里不必清
void mtl_unreg_tex_range(int base, int n) { (void)base; (void)n; }
void mtl_reg_samp(int id, const samp_par *p) { s2a_vk_adapter_reg_samp(id, p); }
void mtl_set_tex_pool_n(int n) { g_tex_pool_n = n; }
void mtl_prog_release(prog_t *p) { (void)p; }   // 还没有按 prog 的管线缓存：没有东西要释放

// NVN ReadTexels（nvnTextureReadTexels / ReadTexelsStrided）：CPU 可见读回。
// 两条事实来源由后端按内容归属选：CPU 填过的纹理按 guest 池里的块线性存储 deswizzle；
// GPU 写过的纹理先拿帧队列完成证明，再从图像按区域拷回。失败必须报红（带 serial/尺寸/格式/
// level/layer/区域），不能返回未读回的内存当成功。
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                     uint8_t *dst, size_t row, size_t img) {
    nvk_require();
    char why[768] = "";
    if (s2a_vk_adapter_read_texels(t, level, layer0, r, dst, row, img, why, sizeof why) != 0)
        red("mtl_read_texels: %s", why[0] ? why : "texture readback failed without a reason");
}

// ---------------- 上屏 ----------------
// provider_vk.c 提供：会话激活早于窗口就绪时，这里再向 WSI lease 要一次窗口；窗口换代/
// 尺寸变化（旋转）也在这里拆旧交换链后重建。返回值：0 = 已有可用窗口；1 = 当前没有窗口
// （正常跳过 present）；-1 = 上一代呈现退休没有证明（必须失败，不能继续用旧 surface）。
// 采样剖析（诊断，debug.s2a.pcprof=1）：挂在绘制线程的 present 上，保证取样线程就是录制线程；
// 未开启时只多一个 static int 判断，不进每笔路径。
void nvn_vk_pcprof_tick(void) __attribute__((weak));
__attribute__((weak)) int s2a_direct_vk_sync_window(void);
// 兼容只有“晚挂”入口的旧构建/探针：没有 sync_window 时退回只补挂。
__attribute__((weak)) int s2a_direct_vk_try_attach_window(void);

uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    nvk_require();
    if (g_lost_from_fence) {
        if (s2a_adpf_discard) s2a_adpf_discard();   // 本帧不完整：挂着的跨距不上报
        static int noted;
        if (noted++ < 8)
            red("mtl_present: an earlier submit (%llu) lost its command chain; refusing to present incomplete content",
                (unsigned long long)g_lost_from_fence);
        return 0;
    }
    if (s2a_direct_vk_sync_window) {
        int wrc = s2a_direct_vk_sync_window();
        if (wrc < 0)
            red("mtl_present: window lifecycle failed; a previous surface/presentation has no retirement "
                "proof, refusing to present (see direct present log)");
    } else if (!mtl_has_window() && s2a_direct_vk_try_attach_window) {
        s2a_direct_vk_try_attach_window();
    }
    if (!mtl_has_window()) {
        if (s2a_adpf_invisible) s2a_adpf_invisible();   // 不可见即关会话，下一可见帧重建
        // 窗口已被 surfaceDestroyed 撤销、新 generation 还没到：这帧没有可上屏的 surface，
        // 跳过 present（不是“丢命令继续跑”，也不需要 abort）。真机从后台返回/旋转后由
        // 晚挂路径拿到新 generation 再继续；之前这里会 red("no game window attached") 把进程打崩。
        static int noted;
        if (noted++ < 8) info("mtl_present: no surface (window revoked); frame drained and present skipped");
        { extern void nvn_vk_first_draw_gpu_cancel(void) __attribute__((weak));
          if (nvn_vk_first_draw_gpu_cancel) nvn_vk_first_draw_gpu_cancel(); }
        // 帧边界结算必须排在 skip_present 之前：异步档下 skip 先 flush 帧队列，而翻译线程可能还有
        // 未回放的作业；先 drain 能把本帧命令全部放进帧队列，再 flush 才是完整的一帧。
        shim_frame_settle();
        // 只跳过上屏不够：纹理 pin 和帧队列要按 present 的收尾释放，否则后台几帧后
        // pin list 满，下一笔 draw 失败并 abort（真机 HOME 复现）。但“跳过上屏”不是完成证明：
        // flush 没有完成就保留 pin/队列资源，按真实失败退出，不能把仍在 GPU 读取的资源标成可回收。
        if (s2a_vk_adapter_skip_present()) {
            extern const char *nvn_vk_first_draw_first_error(void) __attribute__((weak));
            red("mtl_present: no surface and the frame did not complete; pins/queue retained: %s",
                nvn_vk_first_draw_first_error && nvn_vk_first_draw_first_error()[0]
                    ? nvn_vk_first_draw_first_error() : "no completion proof");
        }
        return 0;
    }
    // 激进度：只在 present 前 drain。此时本帧所有提交都已入队，翻译线程可与游戏线程
    // “录制本帧”并行；代价是依赖“游戏不会在本帧内改写上一帧的动态缓冲”这个前提（需真机验画面）。
    shim_drain();
    // ADPF：报「本帧第一批翻译工作 → drain 完成」这段真实关键路径墙钟；目标用共用前端
    // 给的生效上屏间隔。drain 之后的 acquire/上屏等待是 pacing，不是工时；序号只对
    // 本帧 pending，跳帧/丢帧留下的记录在这里自然丢弃。
    if (s2a_adpf_frame_end) {
        double ns = period_s * 1e9;
        s2a_adpf_frame_end(ns > 0 && ns < 9.223372036854776e18 ? (int64_t)ns : 0,
                           atomic_load(&g_frame_seq) + 1);
    }
    char why[192] = "";
    uint64_t t_present = shim_now_ns();
    {
        // 时间线：本帧命令执行结束（进入上屏调用之前）。这一打点与是否延迟 present 无关。
        extern void nvn_vk_frame_stats_tl_mark(uint64_t, int, double) __attribute__((weak));
        if (nvn_vk_frame_stats_tl_mark)
            nvn_vk_frame_stats_tl_mark(g_present_n + 1, 2 /* EXEC_END */, (double)t_present / 1e9);
    }
    const uint64_t replayed_before_present = atomic_load(&g_replayed_fence);
    int prc = s2a_vk_adapter_present_tex(t, crop, why, sizeof why);
    if (prc < 0) {
        // B3：窗口可能在“检查有窗口”和真正 present 之间被 surfaceDestroyed 撤销，
        // present 会报 no game window attached。这是正常的 BACK/HOME/旋转竞态，
        // 按“本帧无窗口、完成证明收尾、跳过上屏”处理，不能当成致命错误 abort。
        if (!mtl_has_window()) {
            if (s2a_adpf_invisible) s2a_adpf_invisible();
            static int noted;
            if (noted++ < 8) info("mtl_present: window revoked during present (%s); frame drained, present skipped",
                                  why[0] ? why : "no window");
            { extern void nvn_vk_first_draw_gpu_cancel(void) __attribute__((weak));
              if (nvn_vk_first_draw_gpu_cancel) nvn_vk_first_draw_gpu_cancel(); }
            // 同 no-surface 路径：先 drain 再 skip，异步档才不会把本帧命令留在 flush 之后。
            shim_frame_settle();
            if (s2a_vk_adapter_skip_present()) {
                extern const char *nvn_vk_first_draw_first_error(void) __attribute__((weak));
                red("mtl_present: window revoked during present and the frame did not complete; "
                    "pins/queue retained: %s",
                    nvn_vk_first_draw_first_error && nvn_vk_first_draw_first_error()[0]
                        ? nvn_vk_first_draw_first_error() : "no completion proof");
            }
            return 0;
        }
        red("mtl_present: %s", why[0] ? why : "present failed");
    } else if (prc > 0) {
        // 本帧绘制已完成，只是 acquire 在有限超时内没拿到 swapchain 图像：不上屏、不记 present 样本。
        static int noted;
        if (noted++ < 8) info("mtl_present: swapchain image unavailable (acquire timeout); present skipped");
        shim_note_completed(replayed_before_present);
        shim_frame_settle();   // 帧边界：acquire 超时也结清本帧
        return 0;
    } else {
        // 上屏路径内部已提交并等到本帧完成：此刻所有“已回放”的提交都有完成证据。
        shim_note_completed(replayed_before_present);
    }
    uint64_t present_ns = shim_now_ns() - t_present;
    uint64_t n = ++g_present_n;
    if (n == 1) info("mtl_present: first frame presented (nvn_vk backend, tex %ux%u)",
                     t ? (unsigned)t->d.width : 0u, t ? (unsigned)t->d.height : 0u);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    g_present_at[n % 256] = now;
    if (nvn_vk_pcprof_tick) nvn_vk_pcprof_tick();
    // 帧时统计与 longframe 归因归 Vulkan 侧模块（nvn_vk_frame_stats）：shim 只把本帧量到的数交进去，
    // 不持有窗口状态、不做阈值判断。没有该模块的构建（例如旧 engine 后端）整块跳过。
    {
        extern void nvn_vk_frame_stats_present(uint64_t, double, double, double, uint64_t)
            __attribute__((weak));
        if (nvn_vk_frame_stats_present)
            nvn_vk_frame_stats_present(n, now, shim_ticks_ns(g_frame_exec_ns) / 1e6,
                                       (double)present_ns / 1e6, g_frame_cmds);
    }
    // 逐帧帧时间（诊断 debug.s2a.frame_log=1，默认关）：与 stats 同一成功上屏点，
    // 只记 CLOCK_BOOTTIME u64 戳进环形缓冲，局末由 adapter shutdown 分片转储。
    {
        extern void nvn_vk_frame_log_present(void) __attribute__((weak));
        if (nvn_vk_frame_log_present) nvn_vk_frame_log_present();
    }
    // 出账依赖（维护方审 11-04）：w6_account.py 解析 `draw window: draws=` 与
    // `render pass window: hit=...` 两行，这两行不随 debug.s2a.win_stats 开关；
    // 其余窗口诊断（读 /proc、逐项 ns 账）仍在开关后面——量的成本不能默认付。
    if (n % 16 == 0) {
        extern void nvn_vk_first_draw_take_counts(uint64_t *, uint64_t *) __attribute__((weak));
        extern void nvn_vk_first_draw_take_pass_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *)
            __attribute__((weak));
        if (nvn_vk_first_draw_take_counts) {
            uint64_t draws = 0, depth_only = 0;
            nvn_vk_first_draw_take_counts(&draws, &depth_only);
            if (draws) info("draw window: draws=%llu depthOnly=%llu", (unsigned long long)draws,
                            (unsigned long long)depth_only);
        }
        if (nvn_vk_first_draw_take_pass_stats) {
            uint64_t hit = 0, miss = 0, create_ns = 0, fb_ns = 0, fb_n = 0;
            nvn_vk_first_draw_take_pass_stats(&hit, &miss, &create_ns, &fb_ns, &fb_n);
            if (hit || miss)
                info("render pass window: hit=%llu miss=%llu create=%.1f ms fb=%.1f ms (n=%llu)",
                     (unsigned long long)hit, (unsigned long long)miss, (double)create_ns / 1e6,
                     (double)fb_ns / 1e6, (unsigned long long)fb_n);
        }
        // 后段残差：pass close / 批量提交 / fence（开关 debug.s2a.pass_close=1，默认关）。
        // 报告 §3.3：这部分原先没有任何计时，被算在触发它的非 draw opcode 名下。
        {
            extern void nvn_vk_first_draw_take_pass_close_stats(uint64_t *, uint64_t *, uint64_t *,
                                                                uint64_t *, uint64_t *, uint64_t *)
                __attribute__((weak));
            uint64_t close_ns = 0, close_n = 0, sub_ns = 0, sub_n = 0, fence_ns = 0, fence_n = 0;
            if (nvn_vk_first_draw_take_pass_close_stats)
                nvn_vk_first_draw_take_pass_close_stats(&close_ns, &close_n, &sub_ns, &sub_n,
                                                        &fence_ns, &fence_n);
            if (close_n || sub_n)
                info("submit window(开关 debug.s2a.pass_close=1, 16帧窗口): close=%llu 次 %.2f ms"
                     "(%.1f us/次) | submit=%llu 批 %.2f ms (%.1f us/批, 含 fence) fence=%.2f ms | "
                     "submit 是独立窗口账（clear 入队/开 pass 也会触发），与 close 不相加",
                     (unsigned long long)close_n, (double)close_ns / 1e6,
                     close_n ? (double)close_ns / 1e3 / (double)close_n : 0.0,
                     (unsigned long long)sub_n, (double)sub_ns / 1e6,
                     sub_n ? (double)sub_ns / 1e3 / (double)sub_n : 0.0,
                     (double)fence_ns / 1e6);
        }
        // 后段残差：整幅 queued clear（开关 debug.s2a.clear_timing=1，默认关）。报告 §3.3/§4.4：
        // 当前 clear 的 CPU 成本 = pass 关闭 + fb/view/pass 取用 + 命令缓冲分配 + 录制 + enqueue，
        // 原先无打点；退回同步次数单独列，因为那是不同的（更贵的）路径。
        {
            extern void vk_record_take_clear_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *)
                __attribute__((weak));
            uint64_t clear_ns = 0, clear_n = 0, queued = 0, fallback = 0;
            if (vk_record_take_clear_stats)
                vk_record_take_clear_stats(&clear_ns, &clear_n, &queued, &fallback);
            if (clear_n)
                info("clear window(开关 debug.s2a.clear_timing=1, 16帧窗口): n=%llu (%.2f/帧) %.2f ms"
                     "(%.1f us/笔) | 整幅入队=%llu 退回同步=%llu",
                     (unsigned long long)clear_n, (double)clear_n / 16.0, (double)clear_ns / 1e6,
                     clear_n ? (double)clear_ns / 1e3 / (double)clear_n : 0.0,
                     (unsigned long long)queued, (unsigned long long)fallback);
        }
        // 出账依赖（同 draw window）：w6_account.py 的 LOCK 正则解析这一行。读取即清零、不读 /proc，成本很小。
        {
            uint64_t wait_ns = 0, calls = 0;
            s2a_vk_adapter_exec_lock_stats(&wait_ns, &calls);
            if (calls) info("adapter exec: lockWait=%.1f us/笔 (n=%llu) 本窗合计 %.1f ms",
                            (double)wait_ns / (double)calls / 1000.0, (unsigned long long)calls,
                            (double)wait_ns / 1e6);
        }
    }
    if (n % 16 == 0 && shim_win_stats_on()) {
        // 管线创建/预热与哈希窗口：诊断读数，留在开关内（量的成本不默认付）。
        {
            extern void nvn_vk_first_draw_pipe_create_stats(uint64_t *, uint64_t *) __attribute__((weak));
            extern void nvn_vk_first_draw_preheat_stats(uint64_t *, uint64_t *, uint64_t *) __attribute__((weak));
            extern void s2a_sha256_stats(uint64_t *, uint64_t *) __attribute__((weak));
            extern void s2a_vk_adapter_take_regen_bumps(uint64_t *) __attribute__((weak));
            if (nvn_vk_first_draw_pipe_create_stats) {
                uint64_t pn = 0, pns = 0;
                nvn_vk_first_draw_pipe_create_stats(&pn, &pns);
                if (pn) info("draw window: pipeCreateDraw=%llu total=%llu us avg=%llu us",
                             (unsigned long long)pn, (unsigned long long)(pns / 1000),
                             (unsigned long long)(pns / pn / 1000));
            }
            if (nvn_vk_first_draw_preheat_stats) {
                uint64_t hn = 0, hns = 0, hsk = 0;
                nvn_vk_first_draw_preheat_stats(&hn, &hns, &hsk);
                if (hn || hsk) info("draw window: pipePreheat created=%llu total=%llu us skipped=%llu",
                                    (unsigned long long)hn, (unsigned long long)(hns / 1000),
                                    (unsigned long long)hsk);
            }
            if (s2a_vk_adapter_take_regen_bumps) {
                uint64_t bumps = 0;
                s2a_vk_adapter_take_regen_bumps(&bumps);
                info("registry window: generation bumps=%llu (%.1f/帧)", (unsigned long long)bumps,
                     (double)bumps / 16.0);
            }
            if (s2a_sha256_stats) {
                uint64_t sb = 0, sc = 0;
                s2a_sha256_stats(&sb, &sc);
                if (sc) info("sha256 window: calls=%llu bytes=%llu (%.1f KB/帧, %.1f 次/帧)",
                             (unsigned long long)sc, (unsigned long long)sb,
                             (double)sb / 16.0 / 1024.0, (double)sc / 16.0);
            }
        }
        {
            // 每笔 draw 真正拷进 arena 的字节（VB/IB/UBO 分开）——判断零拷贝值不值得继续投入的依据。
            extern void nvn_vk_first_draw_take_copy_stats(uint64_t *, uint64_t *, uint64_t *,
                                                          uint64_t *, uint64_t *, uint64_t *)
                __attribute__((weak));
            if (nvn_vk_first_draw_take_copy_stats) {
                uint64_t vb = 0, ib = 0, ubo = 0, vbn = 0, ibn = 0, ubon = 0;
                nvn_vk_first_draw_take_copy_stats(&vb, &ib, &ubo, &vbn, &ibn, &ubon);
                info("copy window: VB=%.0fKB/帧(%llu笔) IB=%.0fKB/帧(%llu笔) UBO=%.0fKB/帧(%llu笔) 合计=%.0fKB/帧",
                     (double)vb / 16.0 / 1024.0, (unsigned long long)vbn,
                     (double)ib / 16.0 / 1024.0, (unsigned long long)ibn,
                     (double)ubo / 16.0 / 1024.0, (unsigned long long)ubon,
                     (double)(vb + ib + ubo) / 16.0 / 1024.0);
            }
        }
        {
            // 池 AHB 后备（零拷贝）采用率：命中=直接引用池缓冲，未命中=走 arena 上传。
            extern void s2a_poolahb_stats_get(void *) __attribute__((weak));
            extern int s2a_poolahb_enabled(void) __attribute__((weak));
            if (s2a_poolahb_enabled && s2a_poolahb_enabled()) {
                struct {
                    uint64_t established, refused, lookups, misses, bytes;
                    uint64_t miss_no_pool, miss_cold, miss_pool_failed, miss_range_unregistered,
                             miss_ref_full, miss_table_full, hit_bytes;
                } st = {0};
                if (s2a_poolahb_stats_get) s2a_poolahb_stats_get(&st);
                info("pool window: 命中=%llu(累计%.0fKB) miss=%llu[无池=%llu 未迁=%llu 迁失败=%llu 范围未登记=%llu 引用满=%llu 表满=%llu] 已迁=%llu 共=%lluMB",
                     (unsigned long long)st.lookups, (double)st.hit_bytes / 16.0 / 1024.0,
                     (unsigned long long)st.misses, (unsigned long long)st.miss_no_pool,
                     (unsigned long long)st.miss_cold, (unsigned long long)st.miss_pool_failed,
                     (unsigned long long)st.miss_range_unregistered,
                     (unsigned long long)st.miss_ref_full, (unsigned long long)st.miss_table_full,
                     (unsigned long long)st.established, (unsigned long long)(st.bytes >> 20));
            }
        }
        // 本窗 render pass 对象成本：命中/新建分开，创建耗时另列。命中率高但创建仍然频繁时，
        // 说明缓存键没有覆盖真实组合，不能拿命中率当成本已降的证据。
        {
            {
                // 统一窗口账：每次新开 / 每笔 / 每帧三个分母分别标明，禁止互除。
                struct nvn_vk_open_window ow;
                memset(&ow, 0, sizeof ow);
                nvn_vk_first_draw_take_open_window(&ow);
                if (ow.passes || ow.draws) {
                    // 帧数用 present 计数（本块每 16 次 present 打一次），不能用 g_frame_id：
                    // 内部同步 flush 也会让它自增，两者不是同一个量。
                    uint64_t frames = 16;
                    extern void nvn_vk_first_draw_take_cmd_stats(uint64_t *, uint64_t *);
                    uint64_t c_reset = 0, c_alloc = 0;
                    nvn_vk_first_draw_take_cmd_stats(&c_reset, &c_alloc);
                    double per_open = ow.passes ? (double)(ow.cget_ns + ow.bcb_ns + ow.rest_ns) / 1e3 / (double)ow.passes : 0.0;
                    double per_draw = ow.draws ? (double)ow.seg_total_ns / 1e3 / (double)ow.draws : 0.0;
                    info("open window: %llu 帧 | 新开 pass=%llu(%.2f/帧) 单笔=%llu(%.2f/帧)",
                         (unsigned long long)frames, (unsigned long long)ow.passes,
                         frames ? (double)ow.passes / (double)frames : 0.0,
                         (unsigned long long)ow.draws, frames ? (double)ow.draws / (double)frames : 0.0);
                    info("  · 每次新开(分母=%llu): cget=%.1f bcb=%.1f rest=%.1f 合计=%.1f us | 独立测调用=%.1f us(n=%llu) | "
                         "cmd reset=%.1f alloc=%.1f us",
                         (unsigned long long)ow.passes,
                         ow.passes ? (double)ow.cget_ns / 1e3 / (double)ow.passes : 0.0,
                         ow.passes ? (double)ow.bcb_ns / 1e3 / (double)ow.passes : 0.0,
                         ow.passes ? (double)ow.rest_ns / 1e3 / (double)ow.passes : 0.0, per_open,
                         ow.open_call_n ? (double)ow.open_call_ns / 1e3 / (double)ow.open_call_n : 0.0,
                         (unsigned long long)ow.open_call_n,
                         ow.passes ? (double)c_reset / 1e3 / (double)ow.passes : 0.0,
                         ow.passes ? (double)c_alloc / 1e3 / (double)ow.passes : 0.0);
                    if (ow.rest_sub_n) {
                        double g = (double)ow.rest_gpuarm_ns / 1e3 / (double)ow.rest_sub_n;
                        double q = (double)ow.rest_query_ns / 1e3 / (double)ow.rest_sub_n;
                        double pc = (double)ow.rest_precopy_ns / 1e3 / (double)ow.rest_sub_n;
                        double br = (double)ow.rest_barrier_ns / 1e3 / (double)ow.rest_sub_n;
                        double bp = (double)ow.rest_beginrp_ns / 1e3 / (double)ow.rest_sub_n;
                        double rest = ow.passes ? (double)ow.rest_ns / 1e3 / (double)ow.passes : 0.0;
                        info("  · rest 细分(分母=%llu 同批): GPU诊断=%.1f 查询准备=%.1f 预拷贝=%.1f barrier=%.1f "
                             "开pass=%.1f 子项和=%.1f 父项 rest=%.1f 未归因=%.1f us",
                             (unsigned long long)ow.rest_sub_n, g, q, pc, br, bp, g + q + pc + br + bp, rest,
                             rest - (g + q + pc + br + bp), rest);
                    }
                    if (ow.draws) {
                        double d = (double)ow.draws;
                        info("  · 每笔(分母=%llu): 入口+模块=%.2f pass=%.2f 管线=%.2f 资源=%.2f"
                             "(显存=%.2f/次×%llu 纹理=%.2f/次×%llu) vbuf=%.2f 描述符+命令=%.2f 合计=%.2f us",
                             (unsigned long long)ow.draws,
                             (double)ow.seg_ns[S2A_OW_SEG_ENTRY_MODULES] / 1e3 / d,
                             (double)ow.seg_ns[S2A_OW_SEG_PASS] / 1e3 / d,
                             (double)ow.seg_ns[S2A_OW_SEG_PIPE] / 1e3 / d,
                             (double)ow.seg_ns[S2A_OW_SEG_RES] / 1e3 / d,
                             ow.res_ub_n ? (double)ow.res_ub_ns / 1e3 / (double)ow.res_ub_n : 0.0,
                             (unsigned long long)ow.res_ub_n,
                             ow.res_tex_n ? (double)ow.res_tex_ns / 1e3 / (double)ow.res_tex_n : 0.0,
                             (unsigned long long)ow.res_tex_n,
                             (double)ow.seg_ns[S2A_OW_SEG_VBUF] / 1e3 / d,
                             (double)ow.seg_ns[S2A_OW_SEG_DESC_CMD] / 1e3 / d,
                             per_draw);
                        info("  · 每帧: 新开 pass=%.2f ms 单笔合计=%.2f ms | 描述符细分(分母=各自次数): "
                             "分配=%.2f(n=%llu) 循环=%.2f(n=%llu) 写入=%.2f(n=%llu) 池=%.2f(n=%llu) us",
                             frames ? (double)ow.passes * per_open / 1000.0 / (double)frames : 0.0,
                             frames ? (double)ow.draws * per_draw / 1000.0 / (double)frames : 0.0,
                             ow.desc_alloc_n ? (double)ow.desc_alloc_ns / 1e3 / (double)ow.desc_alloc_n : 0.0,
                             (unsigned long long)ow.desc_alloc_n,
                             ow.desc_loop_n ? (double)ow.desc_loop_ns / 1e3 / (double)ow.desc_loop_n : 0.0,
                             (unsigned long long)ow.desc_loop_n,
                             ow.desc_write_n ? (double)ow.desc_write_ns / 1e3 / (double)ow.desc_write_n : 0.0,
                             (unsigned long long)ow.desc_write_n,
                             ow.desc_pool_n ? (double)ow.desc_pool_ns / 1e3 / (double)ow.desc_pool_n : 0.0,
                             (unsigned long long)ow.desc_pool_n);
                        info("  · 描述符调用/帧: alloc=%.1f update=%.1f 写入=%.1f 条 | 帧内复用 命中=%llu 未命中=%llu",
                             frames ? (double)ow.desc_alloc_calls / (double)frames : 0.0,
                             frames ? (double)ow.desc_update_calls / (double)frames : 0.0,
                             frames ? (double)ow.desc_writes / (double)frames : 0.0,
                             (unsigned long long)ow.desc_cache_hits, (unsigned long long)ow.desc_cache_miss);
                    }
                }
            }
        }
        // opcode 全量账（本轮 16 帧窗口）：draw(OP_DRAW/OP_DRAW_IDX) 总时长/笔数、非 draw 总时长、
        // 非 draw TopN。两种 draw 记录必须一起算：只认 OP_DRAW_IDX 会把非索引 draw 静默扔进
        // 「非draw」桶（报告 §6.1）。全量输出（不隐藏小项、不按单帧）；g_op_ns 是 tick，统一换算。
        // 包含关系：g_op_ns[op] = adapter_exec_guest 的耗时，pass open/提交/arena 已含在对应 opcode
        // 内，不在此重复计；draw 的“程序查找/收尾”也在这两条 draw opcode 内（内层 first_draw 计时只是子集）。
        {
            uint64_t total_ticks = 0;
            for (int op = 0; op < 256; op++) total_ticks += g_op_ns[op];
            uint64_t draw_ticks = g_op_ns[OP_DRAW] + g_op_ns[OP_DRAW_IDX];
            uint64_t draw_n = g_op_n[OP_DRAW] + g_op_n[OP_DRAW_IDX];
            uint64_t nondraw_ticks = total_ticks >= draw_ticks ? total_ticks - draw_ticks : 0;
            info("opcode window(16帧窗口, 每帧均): total=%.2fms draw(op25/26)=%.2fms(%.1fus/笔 n=%llu/窗) 非draw=%.2fms",
                 shim_ticks_ns(total_ticks) / 1e6 / 16.0, shim_ticks_ns(draw_ticks) / 1e6 / 16.0,
                 draw_n ? shim_ticks_ns(draw_ticks) / (double)draw_n / 1000.0 : 0.0,
                 (unsigned long long)draw_n, shim_ticks_ns(nondraw_ticks) / 1e6 / 16.0);
            for (int k = 0; k < 8; k++) {
                int best = -1; uint64_t bt = 0;
                for (int op = 0; op < 256; op++) {
                    if (shim_is_draw_op((uint32_t)op)) continue;   // 两种 draw 已合并单独列出
                    if (g_op_ns[op] > bt) { bt = g_op_ns[op]; best = op; }
                }
                if (best < 0 || bt == 0) break;
                double bns = shim_ticks_ns(bt);
                info("    非draw op=%d count=%llu/窗 avg=%.1fus/笔 每帧%.2fms", best,
                     (unsigned long long)g_op_n[best],
                     g_op_n[best] ? bns / (double)g_op_n[best] / 1000.0 : 0.0, bns / 1e6 / 16.0);
                g_op_ns[best] = 0;   // 打印过的置零，下一轮不重复选
            }
        }
        {
            extern void vk_record_take_rt_stats(uint64_t *, uint64_t *) __attribute__((weak));
            uint64_t rs = 0, rd = 0;
            if (vk_record_take_rt_stats) vk_record_take_rt_stats(&rs, &rd);
            if (rs + rd) info("rt window: SET_RT same=%llu diff=%llu (冗余 %.0f%%)",
                              (unsigned long long)rs, (unsigned long long)rd, 100.0 * (double)rs / (double)(rs + rd));
            extern void vk_record_take_rt_sem_stats(uint64_t *, uint64_t *) __attribute__((weak));
            uint64_t ss = 0, sd = 0;
            if (vk_record_take_rt_sem_stats) vk_record_take_rt_sem_stats(&ss, &sd);
            if (ss + sd) info("rt 语义窗口: SET_RT same=%llu diff=%llu (冗余 %.0f%%，只比附件身份/代际/尺寸/mip/layer)",
                              (unsigned long long)ss, (unsigned long long)sd, 100.0 * (double)ss / (double)(ss + sd));
        }
        {
            // pass 关闭的触发源（只在「确实关掉了一个 pass」时记一次），用来判断 state_break 里到底是谁在关 pass。
            extern void vk_record_take_break_stats(uint64_t *) __attribute__((weak));
            uint64_t b[64];
            if (vk_record_take_break_stats) {
                memset(b, 0, sizeof b);
                vk_record_take_break_stats(b);
                uint64_t total = 0;
                for (int i = 0; i < 64; i++) total += b[i];
                const uint64_t named = b[OP_SET_RT] + b[OP_CLEAR_COLOR] + b[OP_CLEAR_DS] + b[OP_CLEAR_TEX] +
                                       b[OP_CLEAR_BUF] + b[OP_COPY_B2B] + b[OP_COPY_T2B] + b[OP_COPY_B2T] +
                                       b[OP_COPY_T2T] + b[OP_DISPATCH] + b[OP_BARRIER] + b[OP_FENCE_SYNC] +
                                       b[OP_SIGNAL_EVENT] + b[OP_REPORT_COUNTER];
                if (total) info("pass break 触发源(16帧): SET_RT=%llu 清色=%llu 清深=%llu 清纹理=%llu 清缓冲=%llu "
                                "拷贝B2B=%llu T2B=%llu B2T=%llu T2T=%llu DISPATCH=%llu BARRIER=%llu FENCE=%llu "
                                "SIGNAL=%llu REPORT=%llu 其它=%llu（合计 %llu）",
                                (unsigned long long)b[OP_SET_RT], (unsigned long long)b[OP_CLEAR_COLOR],
                                (unsigned long long)b[OP_CLEAR_DS], (unsigned long long)b[OP_CLEAR_TEX],
                                (unsigned long long)b[OP_CLEAR_BUF], (unsigned long long)b[OP_COPY_B2B],
                                (unsigned long long)b[OP_COPY_T2B], (unsigned long long)b[OP_COPY_B2T],
                                (unsigned long long)b[OP_COPY_T2T], (unsigned long long)b[OP_DISPATCH],
                                (unsigned long long)b[OP_BARRIER], (unsigned long long)b[OP_FENCE_SYNC],
                                (unsigned long long)b[OP_SIGNAL_EVENT], (unsigned long long)b[OP_REPORT_COUNTER],
                                (unsigned long long)(total - named), (unsigned long long)total);
            }
        }
        // 16 帧窗口收尾清零：g_op_ns/g_op_n 跨这 16 帧累加，打印完再清（不再每帧清，否则只是单帧抽样）。
        memset(g_op_ns, 0, sizeof g_op_ns); memset(g_op_n, 0, sizeof g_op_n);
    }
    // 每帧复位“本帧首笔 draw 已打点”闸门与本帧 exec 账：只置 1 不复位会让进程第一帧之后的
    // FIRST_DRAW 永远缺失；exec/命令数不清会让下一帧跨帧累加。跳过 present 的帧走同一个结算点。
    shim_frame_settle();
    // 注意：g_op_ns/g_op_n 是 16 帧窗口账，只在上面的窗口收尾处清零。这里再清一次会让窗口最后
    // 只剩一帧的量、却被除以 16（真机曾表现为「同窗 8561 笔 draw，opcode 账只有 536 笔」）。
    return n;
}

void mtl_present_wait(uint64_t ticket) {
    if (ticket > g_present_n) red("mtl_present_wait: ticket %llu was never issued", (unsigned long long)ticket);
}

void mtl_present_flush(void) { nvk_require(); }
void mtl_present_test_stall(uint64_t ns) { (void)ns; }
double mtl_presented_time(uint64_t n) { return n && n <= g_present_n && g_present_n - n < 256 ? g_present_at[n % 256] : 0; }
uint64_t mtl_present_count(void) { return g_present_n; }

void *mtl_device(void) { return NULL; }   // 没有共享 Metal device：本后端是 Vulkan
uint64_t mtl_ext_tex(void *tex) { (void)tex; return 0; }
uint64_t mtl_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) {
    (void)serial; (void)period; (void)ev; (void)ev_val; (void)crop;
    red("mtl_present_ext: external producers (dm/gfx/vk) are not bridged to the nvn_vk backend");
    return 0;
}

int mtl_has_window(void) { return s2a_vk_adapter_ready() && s2a_vk_adapter_session() && s2a_vk_adapter_session()->has_window; }

// Direct provider is optional in standalone graphics probes. A missing provider
// or window means unknown, rather than an invented 60 Hz display.
extern double s2a_direct_vk_display_refresh(void) __attribute__((weak));
double mtl_display_refresh(void) {
    if (!mtl_has_window() || !s2a_direct_vk_display_refresh) return 0;
    double hz = s2a_direct_vk_display_refresh();
    return hz > 0 && hz < 2147483646.0 ? hz : 0;
}
int mtl_display_hz(void) { return (int)(mtl_display_refresh() + 0.5); }
double dm_present_period(void) { return 0; }
double dm_pace_hz(double screen_hz, double floor_hz) { (void)floor_hz; return screen_hz; }
__attribute__((weak)) int dm_fps_setting(void) { return 60; }
int mtl_fps_target(void) { return dm_fps_setting() == 30 ? 30 : 60; }

// ---------------- 提示 / 调试入口（与引擎同语义） ----------------
void bf_want_note(const char *key) { (void)key; }
uint64_t mtl_host_latch(void) { return 0; }
void mtl_host_apply(uint64_t value) { (void)value; }
void mtl_set_depth_zero(int on) { g_depth_zero = on; }
void mtl_set_origin_lower_left(int on) { g_origin_lower_left = on; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_set_frame(uint64_t f) { g_frame = f; g_frame_set = 1; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
void mtl_bad_draw_feat(const char *why) { red("mtl_bad_draw_feat: %s", why ? why : "unsupported draw feature"); }
int mtl_ui_ghost_rect(float r[4]) { (void)r; return 0; }
int dm_nvn_ruler_truth(void) { return 0; }
void dm_gfx_shot_at(long k, const char *path) {
    (void)k;
    note_once(&g_notes[2], "dm_gfx_shot_at: debug screenshots are not wired on the nvn_vk backend: %s", path ? path : "");
}

// ---------------- 读数（没有读数就报 0，不改 GPU 状态） ----------------
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip) {
    if (resolve) *resolve = 0; if (reuse) *reuse = 0; if (set) *set = 0; if (skip) *skip = 0;
}
void mtl_res_counts(uint32_t tick, const char *why) {
    info("资源 拍 %u（%s）：提交 %llu，命令 %llu，present %llu，上游 draw 未接 %llu",
         tick, why ? why : "", (unsigned long long)g_submits, (unsigned long long)g_commands,
         (unsigned long long)g_present_n, (unsigned long long)g_draws_skipped);
}
void mtl_res_snapshot(mtl_res_count *out) { if (out) memset(out, 0, sizeof *out); }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { if (passes) *passes = 0; if (draws) *draws = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { if (enc) *enc = 0; if (reuse) *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { if (call) *call = 0; if (skip) *skip = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { if (load) *load = 0; if (store) *store = 0; if (full) *full = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { if (uploads) *uploads = 0; if (reuploads) *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { if (lib) *lib = 0; if (pso) *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = 0; }
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { if (waits) *waits = 0; if (grown) *grown = 0; if (n) *n = 0; }
void mtl_counter_stats(uint64_t rep[32], uint64_t rst[32]) { if (rep) memset(rep, 0, 32 * sizeof *rep); if (rst) memset(rst, 0, 32 * sizeof *rst); }
