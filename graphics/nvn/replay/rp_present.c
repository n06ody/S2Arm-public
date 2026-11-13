// 上屏（对齐 nvn_mtl.m present_now @15500 一带的裁剪与等比贴合；nvn_present_fit.h 两端共用）。
// 现在的做法：游戏调上屏时在调用线程上直接取一张交换链图、把游戏的贴图按比例贴上去、呈现。
// 还没接：独立的上屏线程与显示节拍（按住 N 拍、选帧、虚拟拍）、实际上屏时刻（后端没有这项能力时按交给呈现引擎的时刻记）、后处理与抗锯齿缩放。
#include <time.h>
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_fbtrace.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_dirty.h"
#include "nvn_present_fit.h"
#include "nvn_mtl.h"
#include "platform/plat.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { PT_RING = 256 };
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static nvn_gpu_surface g_surf; static int g_hz;
static uint64_t g_ticket;                     // 已发出的票号（从 1 起）
static double g_shown[PT_RING];               // 票号 → 上屏时刻（秒）；0 = 还没结果，-1 = 丢了
static uint64_t g_n_shown, g_n_dropped;
// 诊断读数：相邻两次交上屏隔了多久，按显示节拍分桶（30 帧的游戏在 120 Hz 屏上应当全落在 29–37.5 毫秒那一桶；
// 落到 37.5–46 毫秒 = 这一帧晚了一个刷新周期）。平均帧率看不出「多数帧准点、少数帧晚一拍」和「每帧都慢一点」的区别。
static uint64_t g_gap_last, g_gap_bucket[5];
static void frame_gap_note(void) {
    uint64_t now = plat_now_ns(), d = g_gap_last ? now - g_gap_last : 0; g_gap_last = now;
    if (!d) return;
    int k = d < 29000000ull ? 0 : d < 37500000ull ? 1 : d < 46000000ull ? 2 : d < 54000000ull ? 3 : 4;
    __atomic_fetch_add(&g_gap_bucket[k], 1, __ATOMIC_RELAXED);
}
static void rp_frame_gap_take(uint64_t out[5]) { for (int i = 0; i < 5; i++) out[i] = __atomic_exchange_n(&g_gap_bucket[i], 0, __ATOMIC_RELAXED); }

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

uint64_t nvn_be_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    pthread_mutex_lock(&g_pmu);
    uint64_t ticket = ++g_ticket; double zero = 0;
    __atomic_store(&g_shown[ticket % PT_RING], &zero, __ATOMIC_RELEASE);
    nvn_gpu_shown drop = {ticket, NVN_GPU_SHOWN_DROPPED, 0};
    if (!g_surf || !t) { on_shown(NULL, &drop); g_n_dropped++; pthread_mutex_unlock(&g_pmu); if (t) nvn_unimpl("present", "no_window", t); return ticket; }
    nvn_gpu_view v = rp_present_view(t);   // 上屏视图不走可淘汰缓存：iOS present 直接拿贴图对象
    if (!v) { on_shown(NULL, &drop); g_n_dropped++; pthread_mutex_unlock(&g_pmu); return ticket; }
    nvn_gpu_err err = {0}; nvn_gpu_drawable dr; nvn_gpu_rc r = {NVN_GPU_OK};
    for (int attempt = 0; attempt < 2; attempt++) {
        r = nvn_gpu_drawable_acquire(g_surf, 100000000ull, NULL, &dr, &err);
        if (r.code != NVN_GPU_E_OUTDATED) break;
        nvn_gpu_rc rb = nvn_gpu_surface_rebuild(g_surf, &err);   // 窗口尺寸 / 旋转变了：换一代交换链再取
        if (!nvn_gpu_ok(rb)) { r = rb; break; }
    }
    if (!nvn_gpu_ok(r)) {
        on_shown(NULL, &drop); g_n_dropped++;
        pthread_mutex_unlock(&g_pmu);
        if (r.code != NVN_GPU_E_TIMEOUT && nvn_unimpl("present", "acquire_failed", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "present", err.why);
        return ticket;
    }
    nvn_gpu_present_desc d; memset(&d, 0, sizeof d);
    d.src = v; d.filter = 1;
    // 窗口原点在左下（NVN 默认）时窗口图是自下而上存的：呈现时翻过来。裁剪按存储的行号给，不用换算
    d.flip_y = (uint8_t)(rp_origin_lower_left() != 0);
    int c[4]; int fit = pf_crop_fit(crop, t->d.width, t->d.height, c);
    if (fit >= 0) memcpy(d.crop, c, sizeof c);   // 非法裁剪：按整张显示
    double cw = d.crop[2] > 0 ? (double)d.crop[2] : (double)t->d.width, ch = d.crop[3] > 0 ? (double)d.crop[3] : (double)t->d.height;
    pf_vp vp = pf_viewport(dr.w, dr.h, cw, ch);
    d.dst[0] = (int32_t)(vp.x + 0.5); d.dst[1] = (int32_t)(vp.y + 0.5); d.dst[2] = (int32_t)(vp.w + 0.5); d.dst[3] = (int32_t)(vp.h + 0.5);
    if (d.dst[0] + d.dst[2] > (int32_t)dr.w) d.dst[2] = (int32_t)dr.w - d.dst[0];
    if (d.dst[1] + d.dst[3] > (int32_t)dr.h) d.dst[3] = (int32_t)dr.h - d.dst[1];
    { struct timespec p0, p1; clock_gettime(CLOCK_MONOTONIC, &p0);
      r = nvn_gpu_present(g_surf, &dr, &d, ticket, on_shown, NULL, &err);
      clock_gettime(CLOCK_MONOTONIC, &p1); rp_perf_add(RP_PERF_PRESENT_NS, (uint64_t)(p1.tv_sec - p0.tv_sec) * 1000000000ull + (uint64_t)p1.tv_nsec - (uint64_t)p0.tv_nsec); }
    rp_fbtrace_release(v, FT_PRESENT);   // 录完就放：后端段按引用保活到段完成
    if (nvn_gpu_ok(r)) {
        {   // 分时读数：每 5 秒一行（帧率低的时候「每 600 帧」等不到）
            static uint64_t t_last, n_last, d_last; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (!t_last) t_last = now;
            if (now - t_last >= 5000000000ull) {
                uint64_t pf[RP_PERF_N], nd = 0, nsk = 0; rp_perf_take(pf); rp_draw_stats(&nd, &nsk);
                uint64_t fr = g_n_shown + 1 - n_last; double sec = (double)(now - t_last) / 1e9, f = fr ? (double)fr : 1.0;
                char b[640]; snprintf(b, sizeof b, "分时：%.1f 秒上屏 %llu 帧（每秒 %.1f）；每帧：绘制 %.0f 笔、队列提交 %.1f 次共 %.1f 毫秒（其中交后端 %.1f）、等围栏 %.1f 次共 %.1f 毫秒；段从提交到完成平均 %.1f 毫秒；通道 %.1f 个、交上屏 %.1f 毫秒",
                                      sec, (unsigned long long)fr, (double)fr / sec, (double)(nd - d_last) / f, (double)pf[RP_PERF_SUBS] / f, (double)pf[RP_PERF_SUBMIT_NS] / 1e6 / f, (double)pf[RP_PERF_COMMIT_NS] / 1e6 / f,
                                      (double)pf[RP_PERF_WAIT_N] / f, (double)pf[RP_PERF_WAIT_NS] / 1e6 / f, pf[RP_PERF_DONE_N] ? (double)pf[RP_PERF_DONE_NS] / 1e6 / (double)pf[RP_PERF_DONE_N] : 0.0, (double)pf[RP_PERF_PASSES] / f, (double)pf[RP_PERF_PRESENT_NS] / 1e6 / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（绘制各步，每帧毫秒）：取程序 %.1f、顶点流与索引 %.1f、取贴图视图 %.1f、开通道与取管线 %.1f、动态状态与顶点缓冲 %.1f、绑定 %.1f、后端绘制 %.1f、收尾 %.1f",
                         (double)pf[RP_PERF_D_PROG] / 1e6 / f, (double)pf[RP_PERF_D_PREP] / 1e6 / f, (double)pf[RP_PERF_D_TEX] / 1e6 / f, (double)(pf[RP_PERF_D_PASS] + pf[RP_PERF_D_PIPE]) / 1e6 / f, (double)pf[RP_PERF_D_DYN] / 1e6 / f, (double)pf[RP_PERF_D_BIND] / 1e6 / f, (double)pf[RP_PERF_D_DRAW] / 1e6 / f, (double)pf[RP_PERF_D_TAIL] / 1e6 / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（开通道与取管线细分，每帧毫秒）：开通道 %.2f、取管线 %.2f（其中后端取与设 %.2f）；全部通道开始：取附件视图 %.2f、屏障 %.2f、后端开通道 %.2f、记写过 %.2f",
                         (double)pf[RP_PERF_D_PASS] / 1e6 / f, (double)pf[RP_PERF_D_PIPE] / 1e6 / f, (double)pf[RP_PERF_D_PGET] / 1e6 / f,
                         (double)pf[RP_PERF_PB_VIEW] / 1e6 / f, (double)pf[RP_PERF_PB_BAR] / 1e6 / f, (double)pf[RP_PERF_PB_BE] / 1e6 / f, (double)pf[RP_PERF_PB_MARK] / 1e6 / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（贴图解析，每帧）：等锁 %.2f 毫秒、持锁 %.2f 毫秒、没见过的序号 %.1f 个、新建视图 %.1f 个；影子附件 %.2f 毫秒；新贴图：后端建 %.2f 毫秒、扫存储 %.2f 毫秒、上传 %.1f 张 %.2f 毫秒、填零 %.1f 张 %.2f 毫秒、改由通道清零 %.1f 张",
                         (double)pf[RP_PERF_RES_LOCK] / 1e6 / f, (double)pf[RP_PERF_RES_INIT] / 1e6 / f, (double)pf[RP_PERF_RES_NEW] / f, (double)pf[RP_PERF_RES_VIEWMAKE] / f, (double)pf[RP_PERF_PB_SHADOW] / 1e6 / f,
                         (double)pf[RP_PERF_RES_MAKE] / 1e6 / f, (double)pf[RP_PERF_RES_SCAN] / 1e6 / f, (double)pf[RP_PERF_RES_UPN] / f, (double)pf[RP_PERF_RES_UP] / 1e6 / f, (double)pf[RP_PERF_RES_ZERON] / f, (double)pf[RP_PERF_RES_ZERO] / 1e6 / f, (double)pf[RP_PERF_RES_ZDEFER] / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（通道，每帧）：%.1f 个，共 %.1f 百万像素；颜色附件载入 %.1f 个；挂深度模板的 %.1f 个，其中整个通道没用到它的 %.1f 个；只有 0–1 笔绘制的 %.1f 个；开着的通道被结束的原因：换目标 %.1f、通道中途的整屏清除 %.1f、同一提交里第二次同步 %.1f",
                         (double)pf[RP_PERF_PASSES] / f, (double)pf[RP_PERF_P_KPIX] / 1e3 / f, (double)pf[RP_PERF_P_LOADC] / f, (double)pf[RP_PERF_P_DS] / f, (double)pf[RP_PERF_P_DS_IDLE] / f, (double)pf[RP_PERF_P_ONE] / f, (double)pf[RP_PERF_P_CUT_RT] / f, (double)pf[RP_PERF_P_CUT_CLEAR] / f, (double)pf[RP_PERF_P_CUT_XFER] / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（绑定，每帧）：整组沿用没发的阶段 %.0f 个、段内切片表命中 %.0f 块", (double)pf[RP_PERF_BIND_SKIP] / f, (double)pf[RP_PERF_BIND_SLICE_HIT] / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                { uint64_t hb[5]; rp_frame_gap_take(hb); uint64_t ht = hb[0] + hb[1] + hb[2] + hb[3] + hb[4]; if (!ht) ht = 1;
                  snprintf(b, sizeof b, "分时（上屏间隔分布）：不到 29 毫秒 %.0f%%、29–37.5 毫秒 %.0f%%、37.5–46 毫秒 %.0f%%、46–54 毫秒 %.0f%%、更长 %.0f%%",
                           100.0 * (double)hb[0] / (double)ht, 100.0 * (double)hb[1] / (double)ht, 100.0 * (double)hb[2] / (double)ht, 100.0 * (double)hb[3] / (double)ht, 100.0 * (double)hb[4] / (double)ht);
                  nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b); }
                snprintf(b, sizeof b, "分时（上传，每帧）：贴图整张上传 %.1f 次共 %.0f KB；池镜像同步 %.0f KB（其中直接写 %.0f KB）", (double)pf[RP_PERF_TEX_UP_N] / f, (double)pf[RP_PERF_TEX_UP_KB] / f, (double)pf[RP_PERF_BUF_SYNC_KB] / f, (double)pf[RP_PERF_BUF_SYNC_DIRECT_KB] / f);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                snprintf(b, sizeof b, "分时（回写，每帧）：Finish 没等的提交 %.1f 次；做完要写回的提交 %.1f 次；事件 %.1f、计数器报告 %.1f（类型位图 %#llx）、遮挡事件 %.1f、显卡写缓冲 %.1f；这一窗口新建贴图的宽高 %d 种", (double)pf[RP_PERF_FIN_SKIP] / f, (double)pf[RP_PERF_FX_SUBS] / f, (double)pf[RP_PERF_FX_EVENT] / f, (double)pf[RP_PERF_FX_REPORT] / f, (unsigned long long)rp_fx_report_types(), (double)pf[RP_PERF_FX_OCC] / f, (double)pf[RP_PERF_FX_GW] / f, rp_res_new_sizes_take());
                nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                {   // 写跟踪的开销：游戏写到被保护的页要走一次缺页处理，改保护要进内核
                    static uint64_t f0, p0, c0, g0, n0; uint64_t fl = 0, pr = 0, mc = 0, mpg = 0, mns = 0; nvn_dirty_stats(&fl, &pr); nvn_dirty_mp_stats(&mc, &mpg, &mns);
                    snprintf(b, sizeof b, "分时（写跟踪）：每帧缺页 %.0f 次、宿主写前解保护 %.0f 页、改保护 %.0f 次共 %.0f 页 %.1f 毫秒", (double)(fl - f0) / f, (double)(pr - p0) / f, (double)(mc - c0) / f, (double)(mpg - g0) / f, (double)(mns - n0) / 1e6 / f);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
                    f0 = fl; p0 = pr; c0 = mc; g0 = mpg; n0 = mns; }
                t_last = now; n_last = g_n_shown + 1; d_last = nd;
            }
        }
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
void rp_present_stats(uint64_t *shown, uint64_t *dropped) { pthread_mutex_lock(&g_pmu); if (shown) *shown = g_n_shown; if (dropped) *dropped = g_n_dropped; pthread_mutex_unlock(&g_pmu); }
void rp_present_reset(void) { pthread_mutex_lock(&g_pmu); g_surf = NULL; g_hz = 0; g_ticket = 0; g_n_shown = g_n_dropped = 0; memset(g_shown, 0, sizeof g_shown); pthread_mutex_unlock(&g_pmu); }
