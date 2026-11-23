// 上屏（对齐 nvn_mtl.m present_now @15500 一带的裁剪与等比贴合；nvn_present_fit.h 两端共用）。
// 现在的做法：游戏调上屏时在调用线程上直接取一张交换链图、把游戏的贴图按比例贴上去、呈现。
// 还没接：独立的上屏线程与显示节拍（按住 N 拍、选帧、虚拟拍）、实际上屏时刻（后端没有这项能力时按交给呈现引擎的时刻记）、后处理与抗锯齿缩放。
#include <time.h>
#include <stdatomic.h>
#include "rp_exec.h"
#include "rp_shader.h"
#include "rp_fps_seg.h"
#include "rp_res.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_dirty.h"
#include "nvn_present_fit.h"
#include "nvn_mtl.h"
#include "platform/plat.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// 弱链接登记（tools/weak_link_decl.py，场合 replay = 回放核心宿主单测）：后端 / 首用模块的可选件，缺实现时地址为空、走不到调用
// 弱链接[replay]：rp_shader_live_compile_take
#pragma weak rp_shader_live_compile_take
// 弱链接[replay]：s2a_direct_diag_frame
__attribute__((weak)) void s2a_direct_diag_frame(void);
// 弱链接[replay]：nvn_gpu_stats

enum { PT_RING = 256 };
// 上屏线程的线程 CPU 时间（#1501 差距 9，旧层 nvn_vk_frame_stats.c 的「墙钟 vs 线程 CPU」）：相邻两次成功上屏之间，
// 墙钟走了多少、这条线程自己用了多少 CPU。两者接近 = 线程一直在跑（游戏 CPU 模拟与回放都在这条线程上）；
// CPU 远小于墙钟 = 线程被换出、在等锁或在等 GPU。只比同一线程的两次上屏：跨线程相减没有意义（旧层实测出过 22839 毫秒的假数），记成跨线程样本、不计。
static pthread_mutex_t g_tc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_tc_tid; static int g_tc_have;
static uint64_t g_tc_wall0, g_tc_cpu0;                           // 上一次上屏时的墙钟 / 线程 CPU（纳秒）
static uint64_t g_tc_wall, g_tc_cpu, g_tc_n, g_tc_cross;          // 窗口累计：同线程样本的墙钟、CPU、样本数；跨线程样本数
static uint64_t tc_thread_cpu_ns(void) { struct timespec t; return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static nvn_gpu_surface g_surf; static int g_hz;
static uint64_t g_ticket;                     // 已发出的票号（从 1 起）
static double g_shown[PT_RING];               // 票号 → 上屏时刻（秒）；0 = 还没结果，-1 = 丢了
static uint64_t g_n_shown, g_n_dropped;
// 诊断读数：相邻两次交上屏隔了多久，按显示节拍分桶（30 帧的游戏在 120 Hz 屏上应当全落在 29–37.5 毫秒那一桶；
// 落到 37.5–46 毫秒 = 这一帧晚了一个刷新周期）。平均帧率看不出「多数帧准点、少数帧晚一拍」和「每帧都慢一点」的区别。
static uint64_t g_gap_last, g_gap_bucket[5];
// 帧间隔直方图（1 毫秒一格，最后一格 = 255 毫秒及以上）：窗口末尾算 p50 / p99 / 最长 / >33 毫秒帧数。
// 验收要「平均帧、p99、>33 ms 帧数」，分桶只能看个大概，直方图能直接出这三个数。
static uint32_t g_gap_hist[256];
static uint64_t g_gap_over33;   // 超过 33 毫秒的帧数（精确按纳秒判，不走直方图取整）
// 管线缓存局中写盘在触发线程上的耗时：后端 nvn_gpu_stats 报的进程累计纳秒（pipe_cache_save_trigger_ns），
// 帧边界取一次差分。nvn_gpu_stats 是后端契约里的诊断入口（线程 [任意]）；宿主回放单测不链后端，
// 这里按弱引用处理（同 rp_entry.c 的 nvn_ft_written）：没有实现时这一项显示 0，不改变既有日志与回放语义。
// 只在上屏线程、只在长帧日志打开时查（nvn_gpu_stats 要锁后端、走一遍统计表）。
extern void nvn_gpu_stats(nvn_gpu_stat_fn fn, void *user, int take) __attribute__((weak));
struct plc_ns_rd { uint64_t ns; int got; };
static uint64_t g_plc_ns_last;   // 上次取到的累计值（只在 [上屏] 动）
static void plc_ns_cb(void *user, const char *name, uint64_t value) {
    if (strcmp(name, "pipe_cache_save_trigger_ns")) return;
    ((struct plc_ns_rd *)user)->ns = value;
    ((struct plc_ns_rd *)user)->got = 1;
}
static uint64_t plc_save_ns_frame(void) {
    if (!nvn_gpu_stats) return 0;   // 没有后端读数（宿主单测 / 后端没开）：这一项显示 0
    struct plc_ns_rd rd = {0, 0};
    nvn_gpu_stats(plc_ns_cb, &rd, 0);   // take = 0：后端别的读数还有消费者，别在这里清零
    if (!rd.got) return 0;
    uint64_t d = rd.ns - g_plc_ns_last;   // 进程累计只增
    g_plc_ns_last = rd.ns;
    return d;
}
static void frame_gap_note(void) {
    uint64_t now = plat_now_ns(), d = g_gap_last ? now - g_gap_last : 0; g_gap_last = now;
    if (!d) return;
    int k = d < 29000000ull ? 0 : d < 37500000ull ? 1 : d < 46000000ull ? 2 : d < 54000000ull ? 3 : 4;
    __atomic_fetch_add(&g_gap_bucket[k], 1, __ATOMIC_RELAXED);
    uint32_t ms = (uint32_t)(d / 1000000ull); if (ms > 255) ms = 255;
    __atomic_fetch_add(&g_gap_hist[ms], 1, __ATOMIC_RELAXED);
    if (d > 33333333ull) __atomic_fetch_add(&g_gap_over33, 1, __ATOMIC_RELAXED);
    // 长帧日志（缺省关，排查顿卡时开）：一次超过 80 毫秒就把上一帧的账打出来——回放花了多久、画了多少笔、提交几次，
    // 以及本帧回放各分项花在哪（分项账见 rp_exec.c「逐帧分项账」；每帧只取一次、取即清零）。
    // 开关与逐帧分项账共用 rp_fr_perf_on() 缓存的那一个值（nvk_hitch_log）。
    static int hl = -1;
    uint64_t ex = 0, dr = 0, sb = 0; rp_frame_take(&ex, &dr, &sb);   // 每帧都取（不清账时相邻两帧会累加，读数会虚高）
    if (hl < 0) hl = rp_fr_perf_on();
    if (hl) {
        uint64_t fr[RP_FR_N]; rp_fr_perf_take(fr);   // 本帧分项：与回放账同一个帧边界，每帧只取一次
        fr[RP_FR_PLC] = plc_save_ns_frame();          // 管线缓存写盘：后端统计项的帧差分
        if (d > 80000000ull) {
            char lb[512];
            rp_hitch_line(lb, sizeof lb, d, ex, dr, sb, fr);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "present", lb);
        }
    }
}
static void rp_frame_gap_take(uint64_t out[5]) { for (int i = 0; i < 5; i++) out[i] = __atomic_exchange_n(&g_gap_bucket[i], 0, __ATOMIC_RELAXED); }
// 取走直方图并算 p50 / p99 / 最长 / >33 毫秒帧数（毫秒；空窗口返回 0）
static void rp_frame_hist_take(double *p50, double *p99, double *mx, uint64_t *over33) {
    uint64_t h[256], tot = 0; *p50 = *p99 = *mx = 0;
    *over33 = __atomic_exchange_n(&g_gap_over33, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < 256; i++) { h[i] = __atomic_exchange_n(&g_gap_hist[i], 0, __ATOMIC_RELAXED); tot += h[i]; }
    if (!tot) return;
    uint64_t acc = 0, n50 = (tot + 1) / 2, n99 = (tot * 99 + 99) / 100;
    for (int i = 0; i < 256; i++) {
        if (!h[i]) continue;
        acc += h[i];
        if (!*p50 && acc >= n50) *p50 = (double)i;
        if (!*p99 && acc >= n99) *p99 = (double)i;
        if (h[i]) *mx = (double)i;
    }
}

// 平台件在窗口挂上 / 摘下时调。摘下时保证返回后核心不再用旧的面。
void rp_present_set_surface(nvn_gpu_surface s, int display_hz) {
    pthread_mutex_lock(&g_pmu);
    g_surf = s; g_hz = s ? display_hz : 0;
    pthread_mutex_unlock(&g_pmu);
}
int nvn_be_has_window(void) { pthread_mutex_lock(&g_pmu); int r = g_surf != NULL; pthread_mutex_unlock(&g_pmu); return r; }
int nvn_be_display_hz(void) { pthread_mutex_lock(&g_pmu); int r = g_hz; pthread_mutex_unlock(&g_pmu); return r; }

static void on_shown(void *user, const nvn_gpu_shown *s) {   // 持 g_pmu 的 present 里被后端回调，或后端的回调线程：只写自己的槽
    (void)user;
    double at = s->status == NVN_GPU_SHOWN_AT ? s->at : s->status == NVN_GPU_SHOWN_UNKNOWN ? (double)plat_now_ns() / 1e9 : -1;
    __atomic_store(&g_shown[s->ticket % PT_RING], &at, __ATOMIC_RELEASE);
}

// This backend resolves initial storage synchronously in rp_present_view below;
// there is no handoff to a separate presentation thread to prepare for.
void nvn_be_present_prepare(const tex_t *t) { (void)t; }
// 按游戏申请的帧率分段（rp_fps_seg.h）：前端每次上屏前交来此刻的交换间隔与设它的时刻；只进「分时（目标帧率）」日志，不改上屏节拍。
static _Atomic int g_tgt_itv = 1;
static _Atomic uint64_t g_tgt_set_ns;
static struct fseg_win g_fseg;   // [上屏] 线程独占
void nvn_be_present_target(int interval, uint64_t set_ns) { atomic_store_explicit(&g_tgt_itv, interval, memory_order_relaxed); atomic_store_explicit(&g_tgt_set_ns, set_ns, memory_order_relaxed); }
static void fseg_out(const char *line) { nvn_plat_log(NVN_PLAT_LOG_INFO, "present", line); }

// ---------- 诊断：池查找统计（开关 debug.s2a.pool_stats=1，缺省关；#1368 三档读数）----------
// 每 1024 帧（上屏的帧边界，回放线程）打一行，会话收尾再打一行；开着时 0 值也打。行形态（nvn_plat_log：logcat + 宿主日志）：
//   S2A_POOLSTATS v=1 why=frame|end frames=<开着以来的帧数> fast_hits=<n> memo_hits=<n> lookups=<n> span_index_hits=<n> exec_ns=<n> submits=<n>
// 各项口径（都是累计值，差分由离线算）：
//   fast_hits       = rp_pool_fast_hits()：池查找「上次命中的池」快路命中（rp_entry.c 的计数，线程内）
//   memo_hits       = 线程内备忘命中（rp_pool_memo_stats 的 hits）
//   lookups         = 池查找总次数（rp_pool_memo_stats 的 lookups：备忘开着时每次查找都先进备忘计一次；备忘关着时为 0）
//   span_index_hits = 写跟踪取范围有序索引（#1133，nvn_dirty_span_stats）答的次数——这是另一张索引，不是池查找快路，单列不混
//   exec_ns / submits = 开着以来最外层回放（每条提交的命令列表）的累计用时，与队列提交次数（rp_exec_totals 的差：与长帧日志同源，回放侧不另记）
// 计数都是回放线程自己的（线程内）；收尾那一行用最后一帧的快照（收尾可能不在回放线程上跑）。
static struct { int on; unsigned frames; uint64_t fast, memo_h, memo_l, span_h, ns0, sb0; } g_ps;
static void ps_snap(void) {
    g_ps.fast = rp_pool_fast_hits();
    rp_pool_memo_stats(&g_ps.memo_h, &g_ps.memo_l);
    uint64_t xs = 0, xn = 0; nvn_dirty_span_stats(&g_ps.span_h, &xs, &xn);
}
static void ps_line(const char *why) {
    uint64_t ens = 0, en = 0; rp_exec_totals(&ens, &en); ens -= g_ps.ns0; en -= g_ps.sb0;
    char b[320]; snprintf(b, sizeof b, "S2A_POOLSTATS v=1 why=%s frames=%u fast_hits=%llu memo_hits=%llu lookups=%llu span_index_hits=%llu exec_ns=%llu submits=%llu",
                          why, g_ps.frames, (unsigned long long)g_ps.fast, (unsigned long long)g_ps.memo_h, (unsigned long long)g_ps.memo_l, (unsigned long long)g_ps.span_h,
                          (unsigned long long)ens, (unsigned long long)en);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "poolstats", b);
}
void rp_pool_stats_frame(void) {
    const int on = nvn_plat_flag("pool_stats", 0) == 1;   // 帧边界读一次开关
    if (on != g_ps.on) {
        if (on) { g_ps.frames = 0; rp_exec_totals(&g_ps.ns0, &g_ps.sb0); }
        g_ps.on = on;
    }
    if (!on) return;
    ps_snap();
    if (!(++g_ps.frames & 1023)) ps_line("frame");
}
void rp_pool_stats_end(void) {
    if (!g_ps.on) return;
    ps_line("end");
    g_ps.on = 0;
}
// 逐帧帧日志（android/native/nvn/nvn_vk_frame_log.c，debug.s2a.frame_log，缺省关；#1501 D①）：成功上屏记一条，带取图 / 交上屏的 CPU 耗时。
//   原先只有旧层（mtl_vk_shim.c）在上屏点调，新回放层这里不调，nvk 直连下帧日志整份是空的。
//   弱引用：回放核心宿主单测与鸿蒙不链帧日志，地址为空就不调；开关关着时函数里只判一次缓存标志。
// 弱链接[replay]：s2a_frame_log_present_timed
void s2a_frame_log_present_timed(uint64_t acquire_ns, uint64_t present_ns) __attribute__((weak));
// ADPF 帧负载上报（android/native/adpf_hint.c；原先只接在旧层 mtl_vk_shim.c，缺省的新回放层构建不编它，CPU 调度器一直拿不到帧负载）：
//   一帧的工作 = 上一次出图返回 → 这一次取图之前（游戏逻辑 + 录命令 + 回放），取图 / 呈现的阻塞是节拍不是工作。
//   每次出图前对本帧报 frame_end（目标 = 游戏登记的上屏周期），出图后对下一帧开 work_begin；没有显示面时关会话（invisible），不报。
//   序号独立计（不借票号：票号在出图锁里发，ADPF 不该在出图锁里调）。弱引用：回放核心宿主单测与鸿蒙不链 ADPF，地址为空就不调。
// 弱链接[replay]：s2a_adpf_work_begin
void s2a_adpf_work_begin(uint64_t seq) __attribute__((weak));
// 弱链接[replay]：s2a_adpf_frame_end
void s2a_adpf_frame_end(int64_t target_ns, uint64_t seq) __attribute__((weak));
// 弱链接[replay]：s2a_adpf_invisible
void s2a_adpf_invisible(void) __attribute__((weak));
static _Atomic uint64_t g_adpf_seq;
static uint64_t present_impl(const tex_t *t, double period_s, const int crop[4]);
uint64_t nvn_be_present(const tex_t *t, double period_s, const int crop[4]) {
    pthread_mutex_lock(&g_pmu); const int surf = g_surf != NULL; pthread_mutex_unlock(&g_pmu);
    const uint64_t seq = atomic_load_explicit(&g_adpf_seq, memory_order_relaxed);
    if (!surf) { if (s2a_adpf_invisible) s2a_adpf_invisible(); }
    else if (seq && s2a_adpf_frame_end) {
        const double ns = period_s * 1e9;
        s2a_adpf_frame_end(ns > 0 && ns < 9.0e18 ? (int64_t)ns : 0, seq);
    }
    const uint64_t ticket = present_impl(t, period_s, crop);
    const uint64_t next = atomic_fetch_add_explicit(&g_adpf_seq, 1, memory_order_relaxed) + 1;
    if (surf && s2a_adpf_work_begin) s2a_adpf_work_begin(next);
    return ticket;
}
static uint64_t present_impl(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    rp_pool_stats_frame();   // 池统计（开关 pool_stats；关着每帧只读一次开关，回放与池查找路径不动）
    rp_tm_frame_end();   // 一帧结束：刚结束的是采样帧就记一帧，再定下一帧要不要逐步计时（分时抽样，#1103）
    pthread_mutex_lock(&g_pmu);
    uint64_t ticket = ++g_ticket; double zero = 0;
    __atomic_store(&g_shown[ticket % PT_RING], &zero, __ATOMIC_RELEASE);
    nvn_gpu_shown drop = {ticket, NVN_GPU_SHOWN_DROPPED, 0};
    if (!g_surf || !t) { on_shown(NULL, &drop); g_n_dropped++; pthread_mutex_unlock(&g_pmu); if (t) nvn_unimpl("present", "no_window", t); return ticket; }
    nvn_gpu_view v = rp_present_view(t);   // 上屏视图不走可淘汰缓存：iOS present 直接拿贴图对象
    if (!v) { on_shown(NULL, &drop); g_n_dropped++; pthread_mutex_unlock(&g_pmu); return ticket; }
    nvn_gpu_err err = {0}; nvn_gpu_drawable dr; nvn_gpu_rc r = {NVN_GPU_OK};
    struct timespec a0, a1; clock_gettime(CLOCK_MONOTONIC, &a0);
    for (int attempt = 0; attempt < 2; attempt++) {
        r = nvn_gpu_drawable_acquire(g_surf, 100000000ull, NULL, &dr, &err);
        if (r.code != NVN_GPU_E_OUTDATED) break;
        nvn_gpu_rc rb = nvn_gpu_surface_rebuild(g_surf, &err);   // 窗口尺寸 / 旋转变了：换一代交换链再取
        if (!nvn_gpu_ok(rb)) { r = rb; break; }
    }
    clock_gettime(CLOCK_MONOTONIC, &a1);
    const uint64_t acquire_ns = (uint64_t)(a1.tv_sec - a0.tv_sec) * 1000000000ull + (uint64_t)a1.tv_nsec - (uint64_t)a0.tv_nsec;
    if (!nvn_gpu_ok(r)) {
        on_shown(NULL, &drop); g_n_dropped++;
        pthread_mutex_unlock(&g_pmu);
        if (r.code != NVN_GPU_E_TIMEOUT && nvn_unimpl("present", "acquire_failed", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "present", err.why);
        return ticket;
    }
    nvn_gpu_present_desc d; memset(&d, 0, sizeof d);
    d.src = v; d.filter = 1;
    // 窗口原点在左下（NVN 默认）时窗口图是自下而上存的：呈现时翻过来。裁剪按存储的行号给，不用换算
    d.flip_y = (uint8_t)(rp_present_lower_left() != 0);   // 被上屏窗口建时的原点（rp_entry.c）
    int c[4]; int fit = pf_crop_fit(crop, t->d.width, t->d.height, c);
    if (fit >= 0) memcpy(d.crop, c, sizeof c);   // 非法裁剪：按整张显示
    double cw = d.crop[2] > 0 ? (double)d.crop[2] : (double)t->d.width, ch = d.crop[3] > 0 ? (double)d.crop[3] : (double)t->d.height;
    pf_vp vp = pf_viewport(dr.w, dr.h, cw, ch);
    {   // 铺满方式（nvk_present_fill，缺省 0 = 等比加黑边，与改前相同）：1 = 拉伸到整屏（画面与屏的比例差 ≤ 15% 才拉）；
        //   2 = 等比放大、裁掉多出的边（窄屏裁左右、宽屏裁上下，角上的界面可能被裁；比例差 1%–30% 才裁）。
        //   这是显示层的选项：游戏自己没有按屏幕比例出图时用，不改游戏的出图尺寸。矩形怎么算见 nvn_present_fit.h pf_fill。
        static int fill = -1; static unsigned tick;
        if (fill < 0 || !(++tick & 255)) fill = nvn_plat_flag("nvk_present_fill", 0);   // 每 256 帧重读一次，局中可切
        if (fill > 0) {
            const int src[4] = {d.crop[2] > 0 ? d.crop[0] : 0, d.crop[3] > 0 ? d.crop[1] : 0, (int)cw, (int)ch}; int fc[4];
            if (pf_fill(fill, dr.w, dr.h, src, fc, &vp) == 2) { d.crop[0] = fc[0]; d.crop[1] = fc[1]; d.crop[2] = fc[2]; d.crop[3] = fc[3]; }
        }
    }
    d.dst[0] = (int32_t)(vp.x + 0.5); d.dst[1] = (int32_t)(vp.y + 0.5); d.dst[2] = (int32_t)(vp.w + 0.5); d.dst[3] = (int32_t)(vp.h + 0.5);
    if (d.dst[0] + d.dst[2] > (int32_t)dr.w) d.dst[2] = (int32_t)dr.w - d.dst[0];
    if (d.dst[1] + d.dst[3] > (int32_t)dr.h) d.dst[3] = (int32_t)dr.h - d.dst[1];
    uint64_t present_ns;
    { struct timespec p0, p1; clock_gettime(CLOCK_MONOTONIC, &p0);
      r = nvn_gpu_present(g_surf, &dr, &d, ticket, on_shown, NULL, &err);
      clock_gettime(CLOCK_MONOTONIC, &p1); present_ns = (uint64_t)(p1.tv_sec - p0.tv_sec) * 1000000000ull + (uint64_t)p1.tv_nsec - (uint64_t)p0.tv_nsec; rp_perf_add(RP_PERF_PRESENT_NS, present_ns); }
    nvn_gpu_view_release(v);   // 录完就放：后端段按引用保活到段完成
    if (nvn_gpu_ok(r)) {
        if (s2a_direct_diag_frame) s2a_direct_diag_frame();
        if (s2a_frame_log_present_timed) s2a_frame_log_present_timed(acquire_ns, present_ns);
        if (rp_shader_live_compile_take) {
            struct rp_shader_live_compile_stats live;
            rp_shader_live_compile_take(&live);
            if (live.enqueued || live.wait_ns || live.pending || live.fallback || live.timeout) {
                char b[272];
                snprintf(b, sizeof b, "现编帧 %llu：排队 %llu、提交等待 %.3f ms、暂缓 %llu、启动失败回退 %llu、无结果 %llu",
                         (unsigned long long)rp_frame(), (unsigned long long)live.enqueued, live.wait_ns / 1e6,
                         (unsigned long long)live.pending, (unsigned long long)live.fallback, (unsigned long long)live.timeout);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
            }
        }
        {   // 分时读数：每 5 秒一行（帧率低的时候「每 600 帧」等不到）
            static uint64_t t_last, n_last, d_last; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (!t_last) t_last = now;
            { const uint64_t cpu = tc_thread_cpu_ns(); if (cpu) rp_present_tc_note(now, cpu); }
            fseg_frame(&g_fseg, now, atomic_load_explicit(&g_tgt_itv, memory_order_relaxed), atomic_load_explicit(&g_tgt_set_ns, memory_order_relaxed));
            if (now - t_last >= 5000000000ull) {
                uint64_t pf[RP_PERF_N], nd = 0, nsk = 0; rp_perf_take(pf); rp_draw_stats(&nd, &nsk);
                { char pb[1024]; if (rp_pipe_fail_summary(pb, sizeof pb)) nvn_plat_log(NVN_PLAT_LOG_INFO, "present", pb); }   // 图形管线失败分类：有新失败 / 新挡下的窗口才打
                uint64_t fr = g_n_shown + 1 - n_last; double sec = (double)(now - t_last) / 1e9, f = fr ? (double)fr : 1.0;
                const double fs = pf[RP_PERF_TM_FRAMES] ? (double)pf[RP_PERF_TM_FRAMES] : 1.0;   // 逐步计时那几项只在采样帧里计：按计时过的帧数求每帧值（#1103）
                char b[768]; snprintf(b, sizeof b, "分时：%.1f 秒上屏 %llu 帧（每秒 %.1f）；每帧：绘制 %.0f 笔、队列提交 %.1f 次共 %.1f 毫秒（其中交后端 %.1f）、等围栏 %.1f 次共 %.1f 毫秒（其中等同步 %.1f 次 %.1f 毫秒、Finish %.1f 次 %.1f 毫秒）；段从提交到完成平均 %.1f 毫秒；回放分三段：开段 %.1f / 回放 %.1f / 收尾 %.1f 毫秒、等已完成 %.1f 次、没真等 %.1f 次；GPU 忙 %.1f 毫秒；通道 %.1f 个、交上屏 %.1f 毫秒",
                                      sec, (unsigned long long)fr, (double)fr / sec, (double)(nd - d_last) / f, (double)pf[RP_PERF_SUBS] / f, (double)pf[RP_PERF_SUBMIT_NS] / 1e6 / f, (double)pf[RP_PERF_COMMIT_NS] / 1e6 / f,
                                      (double)(pf[RP_PERF_WAIT_N] + pf[RP_PERF_WAIT_FIN_N]) / f, (double)(pf[RP_PERF_WAIT_NS] + pf[RP_PERF_WAIT_FIN_NS]) / 1e6 / f,
                                      (double)pf[RP_PERF_WAIT_N] / f, (double)pf[RP_PERF_WAIT_NS] / 1e6 / f,
                                      (double)pf[RP_PERF_WAIT_FIN_N] / f, (double)pf[RP_PERF_WAIT_FIN_NS] / 1e6 / f,
                                      pf[RP_PERF_DONE_N] ? (double)pf[RP_PERF_DONE_NS] / 1e6 / (double)pf[RP_PERF_DONE_N] : 0.0,
                                      (double)pf[RP_PERF_SUB_PRE_NS] / 1e6 / f, (double)pf[RP_PERF_SUB_EXEC_NS] / 1e6 / f, (double)pf[RP_PERF_SUB_POST_NS] / 1e6 / f,
                                      (double)pf[RP_PERF_WAIT_ALREADY_N] / f, (double)pf[RP_PERF_WAIT_FAKED_N] / f, (double)pf[RP_PERF_GPU_BUSY_NS] / 1e6 / f,
                                      (double)pf[RP_PERF_PASSES] / f, (double)pf[RP_PERF_PRESENT_NS] / 1e6 / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                {   // 上屏线程 CPU（见 rp_present_tc_note）：每样本 = 相邻两次上屏
                    uint64_t tc[4]; rp_present_tc_take(tc); const double tn = tc[2] ? (double)tc[2] : 1.0;
                    snprintf(b, sizeof b, "分时（上屏线程 CPU，每帧）：两次上屏之间墙钟 %.1f 毫秒、本线程 CPU %.1f 毫秒（占 %.0f%%）；同线程样本 %llu、跨线程样本 %llu（不计）",
                             (double)tc[0] / 1e6 / tn, (double)tc[1] / 1e6 / tn, tc[0] ? 100.0 * (double)tc[1] / (double)tc[0] : 0.0, (unsigned long long)tc[2], (unsigned long long)tc[3]);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b); }
                snprintf(b, sizeof b, "分时（绘制各步，每帧毫秒）：取程序 %.1f、顶点流与索引 %.1f、取贴图视图 %.1f、开通道与取管线 %.1f、动态状态与顶点缓冲 %.1f、绑定 %.1f、后端绘制 %.1f、收尾 %.1f；回放按命令分：绘制类 %.1f、其余 %.1f",
                         (double)pf[RP_PERF_D_PROG] / 1e6 / fs, (double)pf[RP_PERF_D_PREP] / 1e6 / fs, (double)pf[RP_PERF_D_TEX] / 1e6 / fs, (double)(pf[RP_PERF_D_PASS] + pf[RP_PERF_D_PIPE]) / 1e6 / fs, (double)pf[RP_PERF_D_DYN] / 1e6 / fs, (double)pf[RP_PERF_D_BIND] / 1e6 / fs, (double)pf[RP_PERF_D_DRAW] / 1e6 / fs, (double)pf[RP_PERF_D_TAIL] / 1e6 / fs,
                          (double)pf[RP_PERF_EXEC_DRAW_NS] / 1e6 / f, (double)pf[RP_PERF_EXEC_OTHER_NS] / 1e6 / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                { char ops[420]; rp_exec_dump_ops(ops, sizeof ops); char lb[1200];
                  snprintf(lb, sizeof lb, "分时（其余命令里最费时的）：%s；派发 %.1f 次共 %.1f 毫秒（取程序 %.1f、取资源 %.1f、间接携程 %.1f、收通道 %.1f、取管线与绑定与交后端 %.1f；每次扫 %.1f 项）、换目标 %.1f 次（其中收通道共 %.1f 毫秒）、收通道 %.1f 次",
                           ops[0] ? ops : "（开关 nvk_exec_tm 关着）", (double)pf[RP_PERF_DISP_N] / f, (double)pf[RP_PERF_DISP_TOT_NS] / 1e6 / f, (double)pf[RP_PERF_DISP_SHADER_NS] / 1e6 / f, (double)pf[RP_PERF_DISP_RESOLVE_NS] / 1e6 / f, (double)pf[RP_PERF_DISP_IND_NS] / 1e6 / f, (double)pf[RP_PERF_DISP_FLUSH_NS] / 1e6 / f, (double)pf[RP_PERF_DISP_BIND_NS] / 1e6 / f,
                           pf[RP_PERF_DISP_N] ? (double)pf[RP_PERF_DISP_ENTS] / (double)pf[RP_PERF_DISP_N] : 0.0, (double)pf[RP_PERF_SETRT_N] / f,
                           (double)pf[RP_PERF_SETRT_END_NS] / 1e6 / f, (double)pf[RP_PERF_PASSEND_N] / f);
                  nvn_plat_log(NVN_PLAT_LOG_INFO, "present", lb); }
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                {   // 抽样说明（另起一行、不改原有各行的格式）：绘制各步 / 开通道细分 / 贴图解析的等锁与持锁三处按计时过的帧数求每帧值（#1103）
                    char tb[260]; snprintf(tb, sizeof tb, "分时（计时抽样）：每 %u 帧完整计 1 帧，本窗口计了 %llu 帧；绘制各步、开通道细分、贴图解析的等锁与持锁按计时的帧数求每帧毫秒（nvk_tm_full=1 每帧都计）",
                                           rp_tm_stride(), (unsigned long long)pf[RP_PERF_TM_FRAMES]);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", tb); }
                snprintf(b, sizeof b, "分时（开通道与取管线细分，每帧毫秒）：开通道 %.2f、取管线 %.2f（其中后端取与设 %.2f）；全部通道开始：取附件视图 %.2f、屏障 %.2f、后端开通道 %.2f、记写过 %.2f",
                         (double)pf[RP_PERF_D_PASS] / 1e6 / fs, (double)pf[RP_PERF_D_PIPE] / 1e6 / fs, (double)pf[RP_PERF_D_PGET] / 1e6 / fs,
                         (double)pf[RP_PERF_PB_VIEW] / 1e6 / fs, (double)pf[RP_PERF_PB_BAR] / 1e6 / fs, (double)pf[RP_PERF_PB_BE] / 1e6 / fs, (double)pf[RP_PERF_PB_MARK] / 1e6 / fs);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（贴图解析，每帧）：等锁 %.2f 毫秒、持锁 %.2f 毫秒、没见过的序号 %.1f 个、新建视图 %.1f 个、内容检查按纪元跳过 %.0f 次；影子附件 %.2f 毫秒；新贴图：后端建 %.2f 毫秒、扫存储 %.2f 毫秒、上传 %.1f 张 %.2f 毫秒、填零 %.1f 张 %.2f 毫秒、改由通道清零 %.1f 张",
                         (double)pf[RP_PERF_RES_LOCK] / 1e6 / fs, (double)pf[RP_PERF_RES_INIT] / 1e6 / fs, (double)pf[RP_PERF_RES_NEW] / f, (double)pf[RP_PERF_RES_VIEWMAKE] / f, (double)pf[RP_PERF_TEX_EPOCH] / f, (double)pf[RP_PERF_PB_SHADOW] / 1e6 / f,
                         (double)pf[RP_PERF_RES_MAKE] / 1e6 / f, (double)pf[RP_PERF_RES_SCAN] / 1e6 / f, (double)pf[RP_PERF_RES_UPN] / f, (double)pf[RP_PERF_RES_UP] / 1e6 / f, (double)pf[RP_PERF_RES_ZERON] / f, (double)pf[RP_PERF_RES_ZERO] / 1e6 / f, (double)pf[RP_PERF_RES_ZDEFER] / f);
                snprintf(b + strlen(b), sizeof b - strlen(b), "；内容检查：纪元没命中 %.0f 次、逐页比对 %.0f 页", (double)pf[RP_PERF_TEX_EPOCH_MISS] / f, (double)pf[RP_PERF_TEX_CHECK_PAGES] / f);   // 缺省常开、只计数：纪元快路没跳成的次数与逐页比的页数（上限）
                if (pf[RP_PERF_TEX_LOG_N] || pf[RP_PERF_TEX_LOG_FALLBACK])   // 脏页日志（nvk_buf_dirtylog）开着才有
                    snprintf(b + strlen(b), sizeof b - strlen(b), "；按脏页日志核对 %.0f 次（读事件 %.0f 条）、日志不可用退回逐页 %.2f 次", (double)pf[RP_PERF_TEX_LOG_N] / f, (double)pf[RP_PERF_TEX_LOG_EV] / f, (double)pf[RP_PERF_TEX_LOG_FALLBACK] / f);
                if (pf[RP_PERF_TEX_UNARMED])   // 上传时加保护没落地（下一次必重传）
                    snprintf(b + strlen(b), sizeof b - strlen(b), "；上传时加保护没落地 %llu 次（窗口合计，下一次重传）", (unsigned long long)pf[RP_PERF_TEX_UNARMED]);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（通道，每帧）：%.1f 个，共 %.1f 百万像素；颜色附件载入 %.1f 个；挂深度模板的 %.1f 个，其中整个通道没用到它的 %.1f 个；只有 0–1 笔绘制的 %.1f 个（挂深度模板 %.1f / 不挂 %.1f，共 %.1f 百万像素；一笔都没画的 %.1f 个）；同目标续接 %.1f 次、挂起清除 %.1f 条、缩小区域 %.1f 个（共 %.1f 百万像素）、影子附件 %.1f 个；开着的通道被结束的原因：换目标 %.1f、通道中途的整屏清除 %.1f、同一提交里第二次同步 %.1f、超长通道中途先交 %.1f",
                         (double)pf[RP_PERF_PASSES] / f, (double)pf[RP_PERF_P_KPIX] / 1e3 / f, (double)pf[RP_PERF_P_LOADC] / f, (double)pf[RP_PERF_P_DS] / f, (double)pf[RP_PERF_P_DS_IDLE] / f, (double)pf[RP_PERF_P_ONE] / f, (double)pf[RP_PERF_P_TINY_DS] / f, (double)pf[RP_PERF_P_TINY_NODS] / f, (double)pf[RP_PERF_P_TINY_KPIX] / 1e3 / f, (double)pf[RP_PERF_P_CLEARONLY] / f, (double)pf[RP_PERF_P_CONT] / f, (double)pf[RP_PERF_P_PARK] / f, (double)pf[RP_PERF_P_AREA] / f, (double)pf[RP_PERF_P_AREA_KPIX] / 1e3 / f, (double)pf[RP_PERF_P_SHADOW] / f, (double)pf[RP_PERF_P_CUT_RT] / f, (double)pf[RP_PERF_P_CUT_CLEAR] / f, (double)pf[RP_PERF_P_CUT_XFER] / f, (double)pf[RP_PERF_P_CUT_LONG] / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                if (pf[RP_PERF_IND_INDEX_BOUNDED] || pf[RP_PERF_IND_INDEX_GPU] || pf[RP_PERF_IND_INDEX_GROUP] ||
                    pf[RP_PERF_IND_INDEX_UNKNOWN] || pf[RP_PERF_IND_INDEX_OFF]) {
                    snprintf(b, sizeof b, "分时（通道内间接索引，每帧）：有界 %.1f 笔（切通道 %.1f 次）；退池尾：GPU 参数 %.1f 笔、计数组 %.1f 笔、其它 %.1f 笔、开关关 %.1f 笔（共切通道 %.1f 次）",
                             (double)pf[RP_PERF_IND_INDEX_BOUNDED] / f, (double)pf[RP_PERF_IND_INDEX_BOUNDED_CUT] / f,
                             (double)pf[RP_PERF_IND_INDEX_GPU] / f, (double)pf[RP_PERF_IND_INDEX_GROUP] / f,
                             (double)pf[RP_PERF_IND_INDEX_UNKNOWN] / f, (double)pf[RP_PERF_IND_INDEX_OFF] / f,
                             (double)pf[RP_PERF_IND_INDEX_CUT] / f);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                }
                if (pf[RP_PERF_BINST_HZ] || pf[RP_PERF_BINST_ONE] || pf[RP_PERF_BINST_SCAN]) {
                    snprintf(b, sizeof b, "分时（间接绘制 base instance 拷贝，每帧）：批量前移拷贝 %.1f 笔、回退逐笔 %.1f 笔（其中为它收了开着的通道 %.1f 次）、前移扫描 %.1f 次、前移了没用上 %.1f 笔",
                             (double)pf[RP_PERF_BINST_HZ] / f, (double)pf[RP_PERF_BINST_ONE] / f, (double)pf[RP_PERF_BINST_CUT] / f, (double)pf[RP_PERF_BINST_SCAN] / f, (double)pf[RP_PERF_BINST_WASTE] / f);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                }
                snprintf(b, sizeof b, "分时（绑定，每帧）：整组没重建直接沿用 %.0f 个、整组重建后没发的阶段 %.0f 个、段内切片表命中 %.0f 块；可沿用却没用成：命令变 %.0f、段变 %.0f、全局写代号变 %.0f、贴图登记代号变 %.0f、程序变 %.0f、base_instance 变 %.0f、换过管线 %.0f、逐项校验不过 %.0f", (double)pf[RP_PERF_BIND_FAST] / f, (double)pf[RP_PERF_BIND_SKIP] / f, (double)pf[RP_PERF_BIND_SLICE_HIT] / f,
                         (double)pf[RP_PERF_KEEP_F_CMD] / f, (double)pf[RP_PERF_KEEP_F_SEG] / f, (double)pf[RP_PERF_KEEP_F_GEN] / f, (double)pf[RP_PERF_KEEP_F_REG] / f,
                         (double)pf[RP_PERF_KEEP_F_PG] / f, (double)pf[RP_PERF_KEEP_F_BI] / f, (double)pf[RP_PERF_KEEP_F_PIPESET] / f, (double)pf[RP_PERF_KEEP_F_ITEMS] / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                { uint64_t bt_tot = 0, bt_be = 0, bt_n = 0; rp_bind_tm_take(&bt_tot, &bt_be, &bt_n);
                  uint64_t bt_drv = 0, bt_buf = 0, bt_rest = 0; rp_bind_tm_take2(&bt_drv, &bt_buf, &bt_rest);
                  uint64_t bt_alloc = 0, bt_copy = 0; rp_bind_tm_take3(&bt_alloc, &bt_copy);
                  snprintf(b, sizeof b, "分时（绑定重建，每帧）：重建 %.0f 次共 %.2f 毫秒（交后端 %.2f；暂存与构造 %.2f，其中驱动段 %.2f、常量缓冲与支持块 %.2f、其它 %.2f；暂存里申请 %.2f、拷贝 %.2f）；常量缓冲暂存 %.0f 次共 %.0f KB（去重后真落暂存 %.0f KB；其中程序自带常量段 %.0f 次；交后端异步填 %.0f 次、改内存前等填完 %.0f 次（代拷 %.0f 笔共 %.0f KB、撞上录制线程正在拷 %.0f 次）；有还没落地的 GPU 写、直接绑镜像 %.0f 次）、段内切片表命中 %.0f 块；改 / 拷游戏内存的命令改在 GPU 上做 %.0f 次",
                           (double)bt_n / f, (double)bt_tot / 1e6 / f, (double)bt_be / 1e6 / f, (double)(bt_tot - bt_be) / 1e6 / f,
                           (double)bt_drv / 1e6 / f, (double)bt_buf / 1e6 / f, (double)bt_rest / 1e6 / f,
                           (double)bt_alloc / 1e6 / f, (double)bt_copy / 1e6 / f,
                           (double)pf[RP_PERF_UBO_N] / f, (double)pf[RP_PERF_UBO_KB] / f, (double)pf[RP_PERF_UBO_STAGED_B] / 1024.0 / f, (double)pf[RP_PERF_UBO_C1_N] / f,
                           (double)pf[RP_PERF_UBO_FILL_N] / f, (double)pf[RP_PERF_UBO_FILL_WAIT_N] / f, (double)pf[RP_PERF_UBO_FILL_STEAL_N] / f, (double)pf[RP_PERF_UBO_FILL_STEAL_B] / 1024.0 / f, (double)pf[RP_PERF_UBO_FILL_BUSY_N] / f,
                           (double)pf[RP_PERF_UBO_MIRROR_N] / f, (double)pf[RP_PERF_BIND_SLICE_HIT] / f, (double)pf[RP_PERF_BUF_GPU_ORDER_N] / f);
                  nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b); }
                { uint64_t hb[5]; rp_frame_gap_take(hb); uint64_t ht = hb[0] + hb[1] + hb[2] + hb[3] + hb[4]; if (!ht) ht = 1;
                  snprintf(b, sizeof b, "分时（上屏间隔分布）：不到 29 毫秒 %.0f%%、29–37.5 毫秒 %.0f%%、37.5–46 毫秒 %.0f%%、46–54 毫秒 %.0f%%、更长 %.0f%%",
                           100.0 * (double)hb[0] / (double)ht, 100.0 * (double)hb[1] / (double)ht, 100.0 * (double)hb[2] / (double)ht, 100.0 * (double)hb[3] / (double)ht, 100.0 * (double)hb[4] / (double)ht);
                  nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b); }
                {   // 验收口径：平均帧（上面的每秒帧数）、p50 / p99 / 最长、>33 毫秒帧数
                    double p50 = 0, p99 = 0, mx = 0; uint64_t ov = 0; rp_frame_hist_take(&p50, &p99, &mx, &ov);
                    snprintf(b, sizeof b, "分时（帧间隔）：p50 %.1f 毫秒、p99 %.1f 毫秒、最长 %.1f 毫秒、超过 33 毫秒 %.0f 帧（占 %.0f%%）",
                             p50, p99, mx, (double)ov, 100.0 * (double)ov / (double)f);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b); }
                snprintf(b, sizeof b, "分时（上传，每帧）：贴图整张上传 %.1f 次共 %.0f KB；池镜像同步 %.0f KB（其中直接写 %.0f KB）；缓冲解析：快路 %.0f 次、逐页慢路 %.0f 次（共查 %.0f 页）、不受写跟踪 %.0f 次（其中本次提交已同步免传 %.0f 次）", (double)pf[RP_PERF_TEX_UP_N] / f, (double)pf[RP_PERF_TEX_UP_KB] / f, (double)pf[RP_PERF_BUF_SYNC_KB] / f, (double)pf[RP_PERF_BUF_SYNC_DIRECT_KB] / f,
                         (double)pf[RP_PERF_BUF_FAST_N] / f, (double)pf[RP_PERF_BUF_SLOW_N] / f, (double)pf[RP_PERF_BUF_PAGES_N] / f, (double)pf[RP_PERF_BUF_NOSPAN_N] / f, (double)pf[RP_PERF_BUF_NOSPAN_FRESH_N] / f);
                if (pf[RP_PERF_BUF_LOG_EV] || pf[RP_PERF_BUF_LOG_WORDS] || pf[RP_PERF_BUF_LOG_REBUILD])   // 脏页追加日志（nvk_buf_dirtylog）开着才有
                    snprintf(b + strlen(b), sizeof b - strlen(b), "；脏页日志：事件 %.0f 条、求交看位图 %.0f 字、重建 %.2f 次", (double)pf[RP_PERF_BUF_LOG_EV] / f, (double)pf[RP_PERF_BUF_LOG_WORDS] / f, (double)pf[RP_PERF_BUF_LOG_REBUILD] / f);
                if (pf[RP_PERF_TEX_UP_PART])   // 线性 CPU 贴图只传脏行（upload_linear_dirty）
                    snprintf(b + strlen(b), sizeof b - strlen(b), "；线性贴图按脏行传 %.1f 次共 %.0f 行", (double)pf[RP_PERF_TEX_UP_PART] / f, (double)pf[RP_PERF_TEX_UP_ROWS] / f);
                if (pf[RP_PERF_RES_N])
                    snprintf(b + strlen(b), sizeof b - strlen(b), "；解析分段：取镜像与快检 %.1f、锁等待 %.1f、慢路尾 %.1f 毫秒（%.0f 段/帧）；落地 %.0f 次共 %.0f KB、新建镜像 %.0f 次",
                             (double)pf[RP_PERF_RES_FIND_NS] / 1e6 / f, (double)pf[RP_PERF_RES_LW_NS] / 1e6 / f, (double)pf[RP_PERF_RES_LA_NS] / 1e6 / f, (double)pf[RP_PERF_RES_N] / f,
                             (double)pf[RP_PERF_LAND_N] / f, (double)pf[RP_PERF_LAND_KB] / f, (double)pf[RP_PERF_MIRROR_N] / f);
                if (pf[RP_PERF_GW_REG_N])   // 显卡写登记：收到多少条、同段合并掉多少条（落地次数 = 登记 − 合并）
                    snprintf(b + strlen(b), sizeof b - strlen(b), "；显卡写登记 %.0f 条共 %.0f KB、同段合并 %.0f 条省 %.0f KB",
                             (double)pf[RP_PERF_GW_REG_N] / f, (double)pf[RP_PERF_GW_REG_KB] / f, (double)pf[RP_PERF_GW_MERGED_N] / f, (double)pf[RP_PERF_GW_MERGED_KB] / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（回写，每帧）：Finish 没等的提交 %.1f 次；做完要写回的提交 %.1f 次；事件 %.1f、计数器报告 %.1f（类型位图 %#llx）、遮挡事件 %.1f、显卡写缓冲 %.1f；这一窗口新建贴图的宽高 %d 种", (double)pf[RP_PERF_FIN_SKIP] / f, (double)pf[RP_PERF_FX_SUBS] / f, (double)pf[RP_PERF_FX_EVENT] / f, (double)pf[RP_PERF_FX_REPORT] / f, (unsigned long long)rp_fx_report_types(), (double)pf[RP_PERF_FX_OCC] / f, (double)pf[RP_PERF_FX_GW] / f, rp_res_new_sizes_take());
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                {   // 写跟踪的开销：游戏写到被保护的页要走一次缺页处理，改保护要进内核
                    static uint64_t f0, p0, c0, g0, n0; uint64_t fl = 0, pr = 0, mc = 0, mpg = 0, mns = 0; nvn_dirty_stats(&fl, &pr); nvn_dirty_mp_stats(&mc, &mpg, &mns);
                    snprintf(b, sizeof b, "分时（写跟踪）：每帧缺页 %.0f 次、宿主写前解保护 %.0f 页、改保护 %.0f 次共 %.0f 页 %.1f 毫秒", (double)(fl - f0) / f, (double)(pr - p0) / f, (double)(mc - c0) / f, (double)(mpg - g0) / f, (double)(mns - n0) / 1e6 / f);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                    f0 = fl; p0 = pr; c0 = mc; g0 = mpg; n0 = mns; }
                {   // 写跟踪取范围的有序索引（#1133）：索引答了多少次、每次访问几个节点、退回线性扫几次；池数与这一窗口的登记数。
                    // 计数是调用线程自己的，这里读到的是打印这一行的线程（回放线程）的；池数 / 登记数是全局的
                    static uint64_t xh0, xs0, xn0, ad0; uint64_t xh = 0, xs = 0, xn = 0; nvn_dirty_span_stats(&xh, &xs, &xn);
                    int dl = 0, du = 0; uint64_t da = 0; nvn_dirty_slot_stats(&dl, &du, &da);
                    snprintf(b, sizeof b, "分时（取范围索引，每帧）：索引答 %.0f 次（平均每次访问 %.1f 个节点）、退回线性扫 %.0f 次；池 %d 个（占用槽 %d）、本窗口登记 %.1f 次",
                             (double)(xh - xh0) / f, xh > xh0 ? (double)(xn - xn0) / (double)(xh - xh0) : 0.0, (double)(xs - xs0) / f, dl, du, (double)(da - ad0) / f);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                    xh0 = xh; xs0 = xs; xn0 = xn; ad0 = da; }
                {   // 线程内备忘（#974）：池查找命中多少；计数是调用线程自己的，这里读到的是打印这一行的线程（回放线程）的
                    static uint64_t ph0, pl0; uint64_t ph = 0, pl = 0; rp_pool_memo_stats(&ph, &pl);
                    snprintf(b, sizeof b, "分时（线程内备忘，每帧）：池查找 备忘命中 %.0f 次 / 共 %.0f 次", (double)(ph - ph0) / f, (double)(pl - pl0) / f);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                    ph0 = ph; pl0 = pl; }
                fseg_emit(&g_fseg, now, fseg_out);   // 本窗口按游戏申请的交换间隔分段：每段一行（目标 / 实际帧率、达标比例、慢帧），然后开下一窗口
                t_last = now; n_last = g_n_shown + 1; d_last = nd;
            }
        }
        rp_sync_lag_present();   // 开关 nvk_sync_lag=3：补一次「本帧只放行一次同步等待」的额度
        frame_gap_note();
        if (!g_n_shown++ || g_n_shown % 600 == 0) {   // 首帧一行，之后每 600 帧一行
            uint64_t nd = 0, ns = 0; rp_draw_stats(&nd, &ns);
            char b[300]; snprintf(b, sizeof b, "第 %llu 帧已交给呈现：源 #%llu（格式 %#x，%s）%ux%u 裁剪 %d,%d %dx%d → 屏 %ux%u 的 %d,%d %dx%d（丢 %llu）；累计绘制 %llu 笔、跳过 %llu 笔", (unsigned long long)g_n_shown,
                                  (unsigned long long)t->serial, (unsigned)t->d.format, nvn_be_tex_gpu_owned(t->serial) == 1 ? "GPU 写过" : "GPU 没写过", t->d.width, t->d.height, d.crop[0], d.crop[1], d.crop[2], d.crop[3], dr.w, dr.h, d.dst[0], d.dst[1], d.dst[2], d.dst[3], (unsigned long long)g_n_dropped,
                                  (unsigned long long)nd, (unsigned long long)ns);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
            if (nvn_unimpl_kinds()) {   // 未接项各出现了多少次（首次出现时只打一行，量级要看这里）
                char u[700], line[760]; size_t n = nvn_unimpl_dump(u, sizeof u);
                for (size_t i = 0; i < n; i++) if (u[i] == '\n') u[i] = ';'; else if (u[i] == '\t') u[i] = ' ';
                snprintf(line, sizeof line, "未接计数：%s", u); nvn_plat_log(NVN_PLAT_LOG_INFO, "present", line);
            }
            if (nvn_plat_flag("nvk_drawlog", 0)) rp_draw_log_arm(nvn_plat_flag("nvk_drawlog", 0) > 1 ? nvn_plat_flag("nvk_drawlog", 0) : 160);
            if (nvn_plat_flag("nvk_dump", 0)) rp_debug_dump_request(t->serial, g_n_shown);   // 调试开关：把这张图和它上游的贴图读回落盘   // 调试开关：接下来的 160 笔绘制逐笔出日志
        }
    } else g_n_dropped++;
    pthread_mutex_unlock(&g_pmu);
    if (!nvn_gpu_ok(r) && r.code != NVN_GPU_E_OUTDATED && nvn_unimpl("present", "present_failed", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "present", err.why);
    return ticket;
}
void nvn_be_present_wait(uint64_t ticket) { (void)ticket; }   // 上屏在调用线程上同步做完，返回时已交给后端
double nvn_be_presented_time(uint64_t n) {
    pthread_mutex_lock(&g_pmu); uint64_t last = g_ticket; pthread_mutex_unlock(&g_pmu);
    if (!n || n > last || last - n >= PT_RING) return n && n <= last ? -1 : 0;
    double at; __atomic_load(&g_shown[n % PT_RING], &at, __ATOMIC_ACQUIRE);
    return at;
}
// 记一次成功上屏：wall_ns = CLOCK_MONOTONIC，cpu_ns = 本线程 CLOCK_THREAD_CPUTIME_ID（调用线程即上屏线程）。
void rp_present_tc_note(uint64_t wall_ns, uint64_t cpu_ns) {
    const pthread_t me = pthread_self();
    pthread_mutex_lock(&g_tc_mu);
    if (g_tc_have) {
        if (pthread_equal(me, g_tc_tid) && wall_ns >= g_tc_wall0 && cpu_ns >= g_tc_cpu0) { g_tc_wall += wall_ns - g_tc_wall0; g_tc_cpu += cpu_ns - g_tc_cpu0; g_tc_n++; }
        else g_tc_cross++;
    }
    g_tc_tid = me; g_tc_wall0 = wall_ns; g_tc_cpu0 = cpu_ns; g_tc_have = 1;
    pthread_mutex_unlock(&g_tc_mu);
}
// 取走窗口累计（out：墙钟纳秒、CPU 纳秒、同线程样本数、跨线程样本数），取走即清零；起点保留（下一次上屏接着算）
void rp_present_tc_take(uint64_t out[4]) {
    pthread_mutex_lock(&g_tc_mu);
    out[0] = g_tc_wall; out[1] = g_tc_cpu; out[2] = g_tc_n; out[3] = g_tc_cross;
    g_tc_wall = g_tc_cpu = g_tc_n = g_tc_cross = 0;
    pthread_mutex_unlock(&g_tc_mu);
}
void rp_present_stats(uint64_t *shown, uint64_t *dropped) { pthread_mutex_lock(&g_pmu); if (shown) *shown = g_n_shown; if (dropped) *dropped = g_n_dropped; pthread_mutex_unlock(&g_pmu); }
void rp_present_reset(void) { atomic_store(&g_adpf_seq, 0);
    pthread_mutex_lock(&g_tc_mu); g_tc_have = 0; g_tc_wall = g_tc_cpu = g_tc_n = g_tc_cross = 0; pthread_mutex_unlock(&g_tc_mu);   // 新会话不与上一局的最后一次上屏配对
    pthread_mutex_lock(&g_pmu); g_surf = NULL; g_hz = 0; g_ticket = 0; g_n_shown = g_n_dropped = 0; memset(g_shown, 0, sizeof g_shown); memset(&g_fseg, 0, sizeof g_fseg); pthread_mutex_unlock(&g_pmu); }
