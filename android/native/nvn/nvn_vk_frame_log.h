// 逐帧上屏间隔 + 每帧 fence 等待（诊断，默认关；direct/generic 同一份）。
// 开关：Android 要两道调试门（调试构建 + debug.s2a.debug_env=1）且 debug.s2a.frame_log=1，
//       宿主探针走同名环境变量。
//
// 背景：nvn_vk_frame_stats.c 的 `frame timing(近N帧)` 每 16 帧才输出一次、是窗口聚合；
// longframe 走 dm_longframe_note 的归因行，也不是完整逐帧流；generic 的 _s2aFrameMs
// 同样只出 600 帧聚合。验收口径要的整窗 p50 / p95 / p99 / 1% low / >50、>100ms 次数
// 必须靠全窗逐帧数据。t-direct-frametimes 定口径：两路只出一份——direct 在 mtl_present 成功点记、
// generic 在 AndroidWindow.Present 成功（QueuePresent 被接受）后记到同一个环形缓冲；
// t-direct-frametimes 的每帧 fence 等待（直连 vk_record.c submit / vk_clear.c clr_submit 的
// vkWaitForFences）挂进同一条记录。
//
// 口径：只在成功 present 返回后记一条，记的是 present 返回的 CPU 节拍（不是 GPU 执行
// 时间、不是扫描输出），时钟取 CLOCK_BOOTTIME 微秒（与 generic frames.tsv 的 boot_us
// 同域）。fence 字段 = 距上一条 present 之间累计的等待次数与等待时长（µs），present
// 时刻原子快照并清零——仍在飞的等待记进下一帧。
//
// 存储与转储：每帧一条 32B 记录进定长环形缓冲（32768 帧 × 32B = 1 MB 静态，
// 60fps 约 9.1 分钟；写满覆盖最旧的、丢弃数可见）；会话收尾同时写应用内部私有文件与 logcat 分片。文件按进程号命名，
// 使用 Context 注入的 filesDir；debuggable 包可经 run-as 只读取回。direct 在 adapter shutdown 转储，generic 在
// AndroidGameRuntime.Cleanup 的 StopGpu 后转储。宿主解析：android/direct_framelog.py。
//
// 开销：每帧一次 clock_gettime + 一条 24B 写；每次 fence 等待多两次 clock_gettime +
// 短临界区更新；开关默认关，关态每帧/每等只读一次已缓存标志（fence 点先判标志，不再计时）。
#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2A_FL_SITE_CAP 32u
#define S2A_FL_SITE_WINDOW 120u
struct s2a_fl_site { unsigned id; uint64_t ns, count; };

#define S2A_FL_CAP 32768u   /* 保留最近 32768 帧 */

// v5 记录 = 40B：u64 ts，u32 fence_wait_us/fence_n/acquire_us/present_us/gpu_us/gpu_seq/
// disp_us/disp_seq。acquire/present 的 UINT32_MAX 表示未采集或饱和，不得解释为零；
// gpu_us/gpu_seq 与 disp_us/disp_seq 的 UINT32_MAX 表示无新结算/确认（不写 0，
// 对齐 iOS gpu_us / disp_us 空口径）。
struct s2a_fl_rec {
    uint64_t ts;            /* 成功 present 返回点的 CLOCK_BOOTTIME 微秒戳 */
    uint32_t fence_wait_us; /* 距上一条 present 之间 fence 等待总时长（µs，封顶 UINT32_MAX） */
    uint32_t fence_n;       /* 同期 fence 等待次数（封顶 UINT32_MAX） */
    uint32_t acquire_us;     /* 本帧 AcquireNextImage CPU 墙钟 us */
    uint32_t present_us;     /* 本帧 QueuePresent CPU 墙钟 us */
    uint32_t gpu_us;         /* 最新结算帧 GPU TOP→BOTTOM 忙时 us（同一结算帧只报一次） */
    uint32_t gpu_seq;        /* 该读数对应的结算帧序号（gpu_read_frame 的 frame_seq） */
    uint32_t disp_us;        /* 最新确认上屏间隔 us（actualPresentTime 相邻帧差，同一对只报一次） */
    uint32_t disp_seq;       /* 该间隔所属的较新帧号（nvn_vk_dt_last_gap 的 frame） */
};
#define S2A_FL_REC_BYTES 40u

struct s2a_frame_log {
    struct s2a_fl_rec rec[S2A_FL_CAP]; /* 环形：最近 kept 个成功 present 的记录 */
    uint64_t seq;               /* 记过的成功 present 总数；到 UINT64_MAX 封顶不回绕 */
    uint64_t dropped;           /* 被环形覆盖的最旧帧数；到 UINT64_MAX 封顶不回绕 */
    uint32_t w;                 /* 下一次写入的槽位（独立游标，不依赖 seq % CAP） */
    uint32_t kept;              /* 环内现存帧数 ≤ S2A_FL_CAP */
    struct s2a_fl_site sites[S2A_FL_SITE_CAP];
    struct s2a_fl_site sites_ready[S2A_FL_SITE_CAP];
    unsigned site_frames;
    int sites_pending;
    _Atomic int snapshot_lock;
    _Atomic uint64_t fence_ns_acc;  /* 距上一条 present 累计的 fence 等待 ns */
    _Atomic uint64_t fence_n_acc;   /* 距上一条 present 累计的 fence 等待次数 */
};

void s2a_fl_reset(struct s2a_frame_log *fl);

// 记一次 fence 等待（wait_ns = 该次 vkWaitForFences 的墙钟 ns）。累计进帧内账，
// 下一条 note 拍快照进记录并清零。wait_ns==0 也计一次（等待确实发生了）。
void s2a_fl_fence(struct s2a_frame_log *fl, uint64_t wait_ns);

// 记一帧（boot_us = CLOCK_BOOTTIME 微秒）。把此刻的帧内 fence 账拍进本帧记录。
// 返回本次之后的累计丢弃数。
uint64_t s2a_fl_note(struct s2a_frame_log *fl, uint64_t boot_us);
uint64_t s2a_fl_note_timed(struct s2a_frame_log *fl, uint64_t boot_us, uint64_t acquire_ns, uint64_t present_ns);

// 现存首帧的 present 序号（0 基）。seq 封顶后该值不再推进——转储 meta 里的
// presents==UINT64_MAX 时宿主按「序号不可靠」处理，不拿 first 反推。
static inline uint64_t s2a_fl_first(const struct s2a_frame_log *fl) { return fl->seq - fl->kept; }

// 按现存顺序取第 i 帧的时间戳（i < kept）；越界返回 0（宿主单测用来核环形顺序）。
uint64_t s2a_fl_at(const struct s2a_frame_log *fl, uint64_t i);

// 按现存顺序取第 i 帧的 fence 字段；越界返回 0。
uint32_t s2a_fl_fence_us(const struct s2a_frame_log *fl, uint64_t i);
uint32_t s2a_fl_fence_n(const struct s2a_frame_log *fl, uint64_t i);

// 转储回调：emit 收到不含换行的完整单行。分片格式（与 direct device caps[i/n] 同例）：
//   direct frame-log meta: v=4 clock=<名> unit=us endian=le rec=32 cap=<CAP> recorded=<kept>
//                            presents=<seq> dropped=<dropped> first=<first> bytes=<kept*32> chunks=<n>
//   direct frame-log[<i>/<n>]:<base64，载荷 = kept 条 32B LE 记录，最旧→最新>
//   direct frame-log end: frames=<kept> dropped=<dropped> sha256=<载荷小写 hex>
void s2a_fl_dump(const struct s2a_frame_log *fl, void (*emit)(void *ctx, const char *line), void *ctx);

// 生产钩子：present 成功点 / shutdown 弱符号调用；fence 等待点弱符号调用。
// 开关关 / 没记过帧时什么都不打。emit 出口 Android 走 logcat(TAG=S2Arm)，宿主走 stderr（同 shim_log）。
// fence 调用点先判 nvn_vk_frame_log_on()（返回缓存的开关标志，0=关）再计时、再记账——
// 关态只剩一次分支判断。
int nvn_vk_frame_log_on(void);
void nvn_vk_frame_log_present(void);
void nvn_vk_frame_log_dump(void);
void nvn_vk_frame_log_fence_wait(uint64_t wait_ns);
// site: 0=其他, 1=vk_record submit, 2=vk_clear, 10000+first_draw 源码行号。
void nvn_vk_frame_log_fence_site(uint64_t wait_ns, unsigned site);

// 托管层入口（generic AndroidWindow.Present / AndroidGameRuntime.Cleanup 经
// DllImport("s2a_native") 直调）：与 direct 同一个环形缓冲、同一份转储格式。
int s2a_frame_log_on(void);
void s2a_frame_log_fence_wait(uint64_t wait_ns);
void s2a_frame_log_present(void);
void s2a_frame_log_present_timed(uint64_t acquire_ns, uint64_t present_ns);
void s2a_frame_log_dump(void);

#ifdef __cplusplus
}
#endif
