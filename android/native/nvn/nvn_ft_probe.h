// 逐帧时间探针（诊断，默认关；nvk 回放层专用，不进 main 的探针头）。
// 目的：把一帧的 33.3 ms+ 时间摊到具体段——每帧记 acquire / 每次 vkQueueSubmit /
// vkQueuePresentKHR 的精确起止与段键，写定长内存环，局末批量吐出（无每帧日志 I/O）。
//
// 开关：两道调试门（调试构建 + debug.s2a.debug_env=1）且 debug.s2a.ft_probe=1；
//       宿主探针用例走同名环境变量（与 nvn_vk_frame_log 的开关惯例一致）。
//
// 时钟域：全部 CLOCK_MONOTONIC 纳秒（now_ns 同一口径）——与 SurfaceFlinger 的
//   actualPresentTime（VK_GOOGLE_display_timing）、`dumpsys SurfaceFlinger --latency`
//   的 presentTime、Choreographer/VSYNC 时间戳同域，可直接逐值相减对位。
//   对 SF 的方法：本探针的 pre_e（vkQueuePresentKHR 返回点）与 SF 端该帧
//   actualPresentTime 之差 = 本端 CPU 提交→SF 合成上屏的全程间隔；逐帧对位用
//   ticket 顺序与 SF desiredPresentTime/present fence 序列按序对齐（两边都单调）。
//
// 记录口径（字段全是 ns，CLOCK_MONOTONIC）：
//   ticket   帧号（nvn_gpu_present 的 ticket 参数，rp_present g_ticket，单调）
//   acq_b/e  vkAcquireNextImageKHR 前后（含先等上一把取图信号量释放的 wsi_wait_acquire_free；
//            最近一次；一帧最多两次重试，记末次）
//   acq_rc   该次 vkAcquireNextImageKHR 的 VkResult（0=SUCCESS 4=SUBOPTIMAL …）
//   pre_b/e  vkQueuePresentKHR 调用前后（驱动调用精确起止，不是外层包装）
//   pre_rc   该次 vkQueuePresentKHR 的 VkResult
//   rep_b/e  本帧「提交窗」：窗口内最早一段 commit 入口 → 最晚一段 commit 出口
//            （commit 段含等提交锁 + vkEndCommandBuffer + vkCreateFence + vkQueueSubmit
//            + 入队，是回放工作交 GPU 的可测边界；解码 CPU 段在首个 commit 之前不计）
//   nsub     本帧窗口内 vkQueueSubmit 次数（含因 sub[] 满丢弃的）
//   nkept    sub[] 实际保留条数 ≤ S2A_FT_SUB_MAX
//   sub[i]   每次提交：seg_b=commit_impl 入口（含等锁）/ sub_b/e=vkQueueSubmit 精确起止 /
//            seg_e=commit 出口（段入队后；0=提交未成段，如提交失败/被拒）/ key=段键：
//              普通段 = signal（契约 v2 严格递增的段键，与 fence/publish_signal 同一值）；
//              呈现内嵌段 = S2A_FT_KEY_PRESENT|wsi_seq（wsi_seq 与 enqueue 同序）；
//              key=0 = 非段提交（设备初始化时的 GPU 时钟校准 submit）
//   归属模型：提交按「它发生在哪一帧窗口」归属——上一帧 vkQueuePresentKHR 到本帧
//            vkQueuePresentKHR 之间的提交计入本帧（提交与 present 同持 g_submit_mu，
//            严格有序；写者被该锁串行化，环内不加锁）。
//   边界情形：acquire 失败/掉帧不闭合记录——其 acq 写入待决槽，被下一成功帧携带
//            （最后那次 acq 可见）；全 CPU 零提交的帧 rep=0、nsub=0。
//
// 开销预算（开关 ON）：每帧 ~6–10 次 clock_gettime + 定长字段写，量级 ~1 µs，远低 0.1 ms。
//   宿主用例实测打印 ns/帧；开关 OFF：每次事件一次缓存标志读 + 分支（ns 级）。
//
// 转储：nvk_session_close 在 nvn_vk_frame_log_dump 旁弱引用调用（此刻 nvn_gpu_close
//   已收完在途段，写者全停，转储不加锁）；logcat 分片 base64（同 frame-log 例）：
//   meta 行（v=1、CLOCK_MONOTONIC、记录尺寸、cap、计数）+ chunks[i/n] + end 行（sha256）。
//   记录为定长 LE 字节序，宿主按结构体还原。
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2A_FT_CAP       8192u   /* 保留最近 8192 帧（30fps ≈ 4.5 分钟） */
#define S2A_FT_SUB_MAX   10u     /* 每帧保留的提交条数上限 */
#define S2A_FT_KEY_PRESENT  (1ull << 63)   /* sub.key 高位：呈现内嵌段，低 63 位=wsi_seq */

struct s2a_ft_sub {
    uint64_t seg_b;    /* commit_impl 入口（含提交锁等待） */
    uint64_t sub_b;    /* vkQueueSubmit 调用前 */
    uint64_t sub_e;    /* vkQueueSubmit 返回后 */
    uint64_t seg_e;    /* commit 出口（段入队后）；0 = 提交未成段 */
    uint64_t key;      /* 段键：signal，或 S2A_FT_KEY_PRESENT|wsi_seq，或 0 */
};                   /* 40B */

struct s2a_ft_rec {
    uint64_t ticket;   /* 帧号 */
    uint64_t acq_b, acq_e;
    uint64_t pre_b, pre_e;
    uint64_t rep_b, rep_e;
    int32_t  acq_rc, pre_rc;   /* 两次驱动调用的 VkResult */
    uint32_t nsub, nkept;
    struct s2a_ft_sub sub[S2A_FT_SUB_MAX];
};                   /* 72 + 400 = 472B */
_Static_assert(sizeof(struct s2a_ft_rec) == 472, "ft rec layout");

struct s2a_ft_ring {
    struct s2a_ft_rec rec[S2A_FT_CAP];
    uint64_t seq;         /* 记过的 present 总数 */
    uint64_t dropped;     /* 环覆盖丢弃的最旧帧数 */
    uint64_t sub_dropped; /* sub[] 满丢弃的提交条数 */
    uint32_t w, kept;
};

int  s2a_ft_on(void);      /* 缓存的开关（0=关） */
void s2a_ft_acquire(uint64_t b, uint64_t e, int32_t rc);          /* vkAcquireNextImageKHR 前后 */
void s2a_ft_submit(uint64_t seg_b, uint64_t sub_b, uint64_t sub_e, uint64_t key); /* 提交点 */
void s2a_ft_seg_end(uint64_t seg_e);          /* commit 出口：补写最近一条 sub 的 seg_e 与 rep_e */
void s2a_ft_present(uint64_t ticket, uint64_t pre_b, uint64_t pre_e, int32_t rc); /* present 点闭合记录 */

/* 转储（emit 每行回调，宿主测可注入捕获） */
void s2a_ft_dump_to(struct s2a_ft_ring *ft, void (*emit)(void *ctx, const char *line), void *ctx);
void s2a_ft_dump(void);                       /* 平台包装：logcat；nvk_session_close 弱引用调用 */

#ifdef __cplusplus
}
#endif
