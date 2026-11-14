// 回放主循环与提交（对齐 nvn_mtl.m：nvn_be_exec @14491、nvn_be_sub_begin @1171、nvn_be_sub_commit @2210、nvn_xqueue.inc xq_piece @106）。
// 一次游戏提交 = 一段后端命令 = 一个栅栏值；绘制攒在段里，不逐笔提交，CPU 不等完成。
// 「GPU 完成后才可见」的 CPU 副作用（事件值、计数器）挂在本段的完成回调里做。
#include "rp_exec.h"
#include "rp_state.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_backend.h"
#include "nvn_mtl.h"   // 上沿声明（nvn_be_* 与 NVN_EV_WAIT；文件名是历史名，内容与 Metal 无关）
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void nvn_hle_queue_enter(void *q);   // hle_nvn.c
void nvn_hle_piece_begun(void);
uint64_t s2a_guest_tick(void);       // 运行时：游戏的系统计数器（与游戏自己读到的同一个钟）

#define OPN(x) [x] = #x
static const char *const k_op_name[OP_NVN2_END_] = {
    OPN(OP_END), OPN(OP_JUMP), OPN(OP_BEGIN), OPN(OP_CALL), OPN(OP_SIGNAL_EVENT), OPN(OP_REPORT_COUNTER), OPN(OP_COPY_B2B), OPN(OP_UPDATE_UB),
    OPN(OP_SET_RT), OPN(OP_CLEAR_COLOR), OPN(OP_CLEAR_DS), OPN(OP_VIEWPORTS), OPN(OP_SCISSOR), OPN(OP_DEPTH_RANGE),
    OPN(OP_BIND_PROGRAM), OPN(OP_BIND_STATE), OPN(OP_BIND_VATTR), OPN(OP_BIND_VSTREAM), OPN(OP_BIND_VBUF),
    OPN(OP_BIND_UBO), OPN(OP_BIND_SSBO), OPN(OP_BIND_TEX), OPN(OP_BIND_SAMP),
    OPN(OP_STENCIL), OPN(OP_POLY_OFFSET), OPN(OP_DRAW), OPN(OP_DRAW_IDX), OPN(OP_DISPATCH), OPN(OP_BARRIER),
    OPN(OP_SET_TEX_POOL), OPN(OP_SET_SAMP_POOL), OPN(OP_COPY_B2T), OPN(OP_BLEND_COLOR), OPN(OP_COPY_T2B), OPN(OP_BIND_IMG), OPN(OP_COPY_T2T),
    OPN(OP_RESET_COUNTER), OPN(OP_CLEAR_TEX), OPN(OP_CLEAR_BUF), OPN(OP_DEPTH_CLAMP), OPN(OP_RENDER_COND), OPN(OP_FENCE_SYNC),
    OPN(OP_TF_BIND), OPN(OP_TF_STATE), OPN(OP_RAST_DISCARD), OPN(OP_VP_SWIZZLE), OPN(OP_BIND_TEX1), OPN(OP_PRIM_RESTART), OPN(OP_SET_PATCH_SIZE),
    OPN(OP_DRAW_TEX), OPN(OP_WAIT_SYNC), OPN(OP_ALPHA_REF),
    OPN(OP_PUSH_CONST), OPN(OP_DLSS), OPN(OP_PUSH_CONST_PTR), OPN(OP_MULTI_DRAW),
};
#undef OPN
// 加了新命令这里就红：名字表、接线表（docs/直连_Vulkan回放接线表.tsv）、nvn_be_exec 三处同批补。
_Static_assert(OP_COUNT_ == 52 && OP_NVN2_END_ == 56, "录制命令个数变了：补 k_op_name、接线表和 nvn_be_exec");
const char *rp_op_name(uint32_t op) { return op < OP_NVN2_END_ && k_op_name[op] ? k_op_name[op] : "OP_?"; }

// ---------- 提交 ----------
struct post { volatile uint32_t *val; uint32_t mode, v; uint64_t report; uint32_t type; };   // 完成后写事件值（mode 0 写 / 1 加）；report 非 0 = 计数器报告的写回地址
struct occ_ev { uint32_t pos; uint32_t report; uint64_t addr; };   // 遮挡计数的事件：发生时本段已经开过 pos 个槽；report 0 = 复位、1 = 报告（写回 addr）
struct gw_ev { uint64_t pool_key, id; };   // 本段里 GPU 写进池的一段（rp_buf_gpu_write_note 的登记）
struct sub { uint32_t mids, freed; uint8_t orphan;   /* mids：还没完成的中途段里有几个指着这份提交；orphan：最后一段没交成，由最后一个中途段的完成回调来放（见 sub_orphan） */
             struct post *p; int n, cap, nfx; /* nfx：p 里时间戳报告以外的条数 */ uint64_t t_commit; struct occ_ev *oe; int noe, oecap; uint64_t fence; struct gw_ev *gw; int ngw, gwcap;
    uint64_t *oc; uint32_t noc, occ_base; };   // oc：中途交过段的提交，各段的遮挡槽结果按整次提交的槽号攒在这里（没拆过为 NULL）；occ_base：最后一段的第一个槽号
// 分时读数（上屏那边每 5 秒取走一次清零）：一次队列提交里回放花的、交给后端花的、游戏等围栏花的、段从提交到完成隔了多久
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static uint64_t g_pf[RP_PERF_N];
void rp_perf_take(uint64_t out[RP_PERF_N]) { for (int i = 0; i < RP_PERF_N; i++) out[i] = __atomic_exchange_n(&g_pf[i], 0, __ATOMIC_ACQ_REL); }
static void pf_add(int i, uint64_t v) { __atomic_fetch_add(&g_pf[i], v, __ATOMIC_RELAXED); }
void rp_perf_add(int i, uint64_t v) { pf_add(i, v); }
// GPU 忙时估算：每段完成时按完成顺序累计「显卡真正在跑」的区间（判 GPU 到底忙多久、是不是墙）。
// busy += done_i - max(commit_i, done_{i-1})（第一段：done_0 - commit_0）。commit 在提交前取，done 在完成回调里取；
// 完成回调线程与提交线程都碰 g_busy_last_done，用一把锁串起来（每段一次，量级可忽略）。
static pthread_mutex_t g_busy_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_busy_last_done;   // 上一段的完成时刻（按完成顺序；只在 g_busy_mu 里动）
static void busy_done(uint64_t commit, uint64_t done) {
    if (!commit || done <= commit) return;   // 没记到提交时刻 / 时钟异常：这一段不计
    pthread_mutex_lock(&g_busy_mu);
    uint64_t start = commit > g_busy_last_done ? commit : g_busy_last_done, busy = done > start ? done - start : 0;
    if (done > g_busy_last_done) g_busy_last_done = done;
    pthread_mutex_unlock(&g_busy_mu);
    if (busy) pf_add(RP_PERF_GPU_BUSY_NS, busy);
}
// 前置段的提交时刻：提交前分配，由完成回调 pre_done 带上、在回调里释放（失败段也恰好回调一次，见 nvn_backend.h 的提交契约）。
struct pre_note { uint64_t t_commit; };
static struct pre_note *pre_note_make(void) { struct pre_note *p = malloc(sizeof *p); if (p) p->t_commit = now_ns(); return p; }
static uint64_t g_done_fence;  // 已完成的最大围栏值（完成回调写，提交线程读）
static uint64_t g_occ_acc;     // 遮挡计数：自上次复位以来已完成的各槽之和（只在完成回调里动）
int s2a_nvn_render_cond(void);  // 宿主：这一款的条件渲染开没开（每款参数 render_cond）
static struct sub *g_sub;      // 当前段挂着的完成后副作用
static nvn_gpu_cmd g_cmd;      // 当前段（NULL = 后端没给出段：本次提交只走状态，不编码）
static nvn_gpu_cmd g_pre;      // 前置段：本次提交里资源同步用的传输（池镜像、贴图上传），先于 g_cmd 提交。用到才开
static uint64_t g_sub_serial;  // 第几次提交（资源按它判断「本次提交里已经用过」）
uint64_t rp_sub_serial(void) { return g_sub_serial; }
static uint64_t g_seg_serial;  // 第几个后端段（一次提交中途交过段就不止一个）。段里的暂存内容随段回收，按它判能不能复用
uint64_t rp_seg_serial(void) { return g_seg_serial; }
static uint64_t g_fence;       // 最近一次提交的栅栏值
static uint64_t g_effect_fence, g_finish_prev;   // 见 nvn_be_finish_publish
enum { SUB_STEP = 64 };       // 一次提交在时间线上占 64 个值：最后一段（带回写）取 g_fence + 64，它前面的前置段 / 中途先交的段依次取 +1、+2…
static uint32_t g_mid;         // 这次提交已经用掉几个中间值
static uint64_t g_split_base;  // 上次中途交段时的累计绘制笔数
static int g_ret_n;            // OP_CALL 嵌套深度

static void red(const char *fmt, const char *why) { char b[256]; snprintf(b, sizeof b, fmt, why); nvn_plat_log(NVN_PLAT_LOG_RED, "replay", b); }
static void pre_done(void *user, const nvn_gpu_done *d) {
    struct pre_note *p = user;
    if (d->status == NVN_GPU_DONE_FAILED) red("前置段失败：%s", d->why ? d->why : "?");
    else if (p) busy_done(p->t_commit, now_ns());
    free(p);
}

// 资源同步该录进哪一段。传输不能出现在渲染通道里，所以同步一律录进前置段，它在 GPU 上先于本次提交的全部通道执行。
// 前提是「同步的内容对本次提交里每个使用点都成立」：回放发生在游戏提交之后，游戏对池内存的写在提交前已经做完，这个前提成立；
// 例外是同一资源在本次提交里已经同步过又变了（命令流里按序的写、别的线程正在写下一帧）：那一次必须排在已录的绘制之后，
// 于是结束当前通道、录进主段（通道在下一笔绘制时重开）。last_sub：该资源上次同步所在的提交序号，由调用方持有。
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub) {
    if (!g_cmd) return NULL;
    if (*last_sub == g_sub_serial) { if (rp_pass_is_open()) pf_add(RP_PERF_P_CUT_XFER, 1); rp_pass_end(g_cmd); return g_cmd; }
    *last_sub = g_sub_serial;
    if (!g_pre) { nvn_gpu_err err = {0}; nvn_gpu_cmd_desc d = {0, "nvn submit pre"};
        if (!nvn_gpu_ok(nvn_gpu_cmd_begin(&d, &g_pre, &err))) { g_pre = NULL; rp_pass_end(g_cmd); return g_cmd; } }
    return g_pre;
}

static void sub_done(void *user, const nvn_gpu_done *d) {
    struct sub *s = user;
    uint64_t t_done = now_ns();
    if (s->t_commit) { pf_add(RP_PERF_DONE_NS, t_done - s->t_commit); pf_add(RP_PERF_DONE_N, 1); }
    if (d->status != NVN_GPU_DONE_FAILED) busy_done(s->t_commit, t_done);   // 失败段没执行：不算显卡忙
    // 先落 GPU 写，再发布事件和完成序号：等待事件的 CPU 不能先看见旧内存。
    for (int i = 0; i < s->ngw; i++) rp_buf_gpu_write_land(s->gw[i].pool_key, s->gw[i].id, d->status != NVN_GPU_DONE_FAILED);
    if (d->status != NVN_GPU_DONE_FAILED)   // 没执行的段不做任何成功回写
        for (int i = 0; i < s->n; i++) {
            if (s->p[i].report) {   // 计数器记录 = {计数值, 时间}（各 8 字节）。时间取本段完成这一刻的游戏时钟；时间戳类的计数值就是时间，
                // 其余管线统计类计数值写 0（同 nvn_mtl.m 提交完成回调里的写法）。同一段里的多条报告现在拿到同一个时间：按报告所在位置给 GPU 时间要后端出段内时间戳，没接。
                // 条件渲染开着时时间戳类的计数值也写 0，同真机（游戏拿两条报告的计数值做条件，见 rc_value）
                uint64_t t = s2a_guest_tick(), rec[2] = {(s->p[i].type == NVN_COUNTER_TYPE_TIMESTAMP || s->p[i].type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) && !s2a_nvn_render_cond() ? t : 0, t};
                memcpy((void *)(uintptr_t)s->p[i].report, rec, sizeof rec);
                continue;
            }
            if (s->p[i].mode & 1) __atomic_fetch_add((uint32_t *)s->p[i].val, s->p[i].v, __ATOMIC_SEQ_CST);
            else __atomic_store_n((uint32_t *)s->p[i].val, s->p[i].v, __ATOMIC_SEQ_CST);
        }
    else red("提交段失败，本段的事件 / 计数器不回写：%s", d->why ? d->why : "?");
    // 遮挡计数：自上次复位以来通过的样本数是队列上的累计值。按槽号顺序把本段各槽加进累计；复位 / 报告事件落在「它发生时已经开过的槽」之后。
    // 段按提交顺序完成，累计值只在这里（完成回调）动。失败的段：槽值当 0，报告不写。
    { uint64_t acc = g_occ_acc; uint32_t n = d->status != NVN_GPU_DONE_FAILED && d->occlusion ? d->nocclusion : 0; int e = 0;
      const uint64_t *occ = d->occlusion;
      if (s->oc && d->status != NVN_GPU_DONE_FAILED) {   // 拆过的提交：把最后一段的结果并进去，按整次提交的槽号走
          for (uint32_t i = 0; i < n && s->occ_base + i < 2048; i++) s->oc[s->occ_base + i] = d->occlusion[i];
          if (s->occ_base + n > s->noc) s->noc = s->occ_base + n < 2048 ? s->occ_base + n : 2048;
          occ = s->oc; n = s->noc; }
      for (uint32_t i = 0; i <= n || e < s->noe; i++) {
          for (; e < s->noe && s->oe[e].pos <= i; e++) {
              if (!s->oe[e].report) acc = 0;
              else if (d->status != NVN_GPU_DONE_FAILED) { uint64_t rec[2] = {acc, s2a_guest_tick()}; memcpy((void *)(uintptr_t)s->oe[e].addr, rec, sizeof rec); }
          }
          if (i < n) acc += occ[i];
          if (i >= n && e >= s->noe) break;
      }
      g_occ_acc = acc; }
    if (s->fence) { uint64_t f = __atomic_load_n(&g_done_fence, __ATOMIC_RELAXED); if (s->fence > f) __atomic_store_n(&g_done_fence, s->fence, __ATOMIC_RELEASE); }
    free(s->gw); free(s->oe); free(s->p); free(s->oc); free(s);
}
// 最后一段没交成的提交（开段失败）：它的回写、事件、计数器都不做（同「提交段失败」），登记过的 GPU 写按失败落地。
// 中途交出去的段还指着它（mid_done 要往它的 oc 里攒遮挡结果），所以不能当场放：谁最后一个松手谁放，两边可能同时到，用 freed 抢一次。
static void sub_release(struct sub *s) {
    if (__atomic_exchange_n(&s->freed, 1, __ATOMIC_ACQ_REL)) return;
    for (int i = 0; i < s->ngw; i++) rp_buf_gpu_write_land(s->gw[i].pool_key, s->gw[i].id, 0);
    free(s->gw); free(s->oe); free(s->p); free(s->oc); free(s);
}
static void sub_orphan(struct sub *s) {
    __atomic_store_n(&s->orphan, 1, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&s->mids, __ATOMIC_ACQUIRE)) sub_release(s);
}

// ---------- 遮挡计数（NVN_COUNTER_TYPE_SAMPLES_PASSED；nvn_mtl.m @950 一带） ----------
// NVN 语义：通过的样本数是队列上的累计值——ResetCounter 清零，之后每笔绘制往上加，ReportCounter 在 GPU 做到那里时把当时的累计值写出去。
// 做法同 iOS：计数期间每个通道用一个新槽（不指望跨通道累加），报告之后换新槽；各槽的值在段完成时按顺序加起来（sub_done）。
// 游戏第一次碰这个计数器的那一段没有声明槽（开段时还不知道）：复位时就地交段，后半段带槽，复位之后的绘制照计（occ_first_reset_cut）。
enum { OCC_SLOTS = 2048 };
static int g_occ_used, g_occ_counting, g_occ_seg, g_occ_cur = -1, g_occ_pass; static uint32_t g_occ_next, g_occ_base;   // g_occ_base：当前这一段的第一个槽号（中途交段之后不是 0）
static void occ_event(int report, uint64_t addr) {
    struct sub *s = g_sub; if (!s) return;
    if (s->noe == s->oecap) { int cap = s->oecap ? s->oecap * 2 : 16; struct occ_ev *n = realloc(s->oe, (size_t)cap * sizeof *n); if (!n) { red("%s", "遮挡计数：事件表扩不了，这条复位 / 报告丢了"); return; } s->oe = n; s->oecap = cap; }
    s->oe[s->noe++] = (struct occ_ev){g_occ_next, (uint32_t)report, addr};
}
static void occ_stop(nvn_gpu_cmd c) { if (g_occ_cur >= 0 && c && rp_pass_is_open()) nvn_gpu_occlusion(c, -1); g_occ_cur = -1; }
int rp_occl_want(void) { return g_occ_counting && g_occ_seg; }                       // 开通道时：这个通道要不要声明遮挡计数
void rp_occl_pass_opened(int declared) { g_occ_pass = declared; g_occ_cur = -1; }
void rp_occl_pass_closed(void) { g_occ_pass = 0; g_occ_cur = -1; }                   // 后端在通道结束时自己收掉开着的槽
void rp_occl_pause(nvn_gpu_cmd c) { occ_stop(c); }                                   // 通道里按矩形清之前：清除不是绘制，不计进去
void rp_occl_draw(nvn_gpu_cmd c) {                                                   // 每笔绘制之前
    if (g_occ_cur >= 0 || !g_occ_pass || !g_occ_counting) return;
    if (g_occ_next >= OCC_SLOTS) { nvn_unimpl("occl", "slots_exhausted", NULL); return; }
    g_occ_cur = (int)g_occ_next++; nvn_gpu_occlusion(c, g_occ_cur - (int)g_occ_base);
}
static void seg_cut(int next_occ);
// 第一次复位落在没声明槽的段里（开段时还不知道游戏用这个计数器）：就地交段，后半段开成带槽的新段，复位之后的绘制照计
// （同 iOS nvn_mtl.m 第一次复位就地拆通道）。前半段没有槽，不进 mids。中间值用完交不了时具名记账，这一段复位后不计。
static nvn_gpu_cmd occ_first_reset_cut(nvn_gpu_cmd c) {
    if (g_occ_seg || !g_sub || !c || c != g_cmd) return c;
    if (g_mid >= SUB_STEP - 4) { nvn_unimpl("occl", "first_reset_not_counted", NULL); return c; }
    rp_pass_flush(c);
    seg_cut(1);
    return g_cmd;
}
static void occ_reset(nvn_gpu_cmd c) { g_occ_used = 1; c = occ_first_reset_cut(c); occ_stop(c); g_occ_counting = 1; occ_event(0, 0); }
static void occ_report(nvn_gpu_cmd c, uint64_t addr) { g_occ_used = 1; occ_stop(c); occ_event(1, addr); }

// ---------- 条件渲染（nvn_mtl.m @893–@945 rc_*） ----------
// 游戏拿两条计数器报告的计数值做条件（相等 / 不等才画）。回放时这些报告还没被 GPU 做到，内存里是旧值：
// 时间戳类的计数值真机上是 0（本次提交里录到的、已提交还没完成的，都按 0 算）；别的计数器（遮挡查询等）写过的地址当「不知道」，照画——
// 遮挡剔除只是省工作量。判成不画的那一段里，绘制和清除都丢掉。两张表都不许丢项（丢了条件会读到 CPU 写的旧值）。
static int g_rc_skip;
static uint64_t *g_rc_pend; static uint32_t g_rc_npend, g_rc_pcap;
typedef struct { uint64_t addr, fence; } rc_sub;
static rc_sub *g_rc_sub; static uint32_t g_rc_nsub, g_rc_scap;
static uint64_t *g_rc_occ; static uint32_t g_rc_ocap, g_rc_on;
int rp_rc_skip(void) { return g_rc_skip; }
static void rc_retire(void) { uint64_t done = __atomic_load_n(&g_done_fence, __ATOMIC_ACQUIRE); uint32_t n = 0; for (uint32_t i = 0; i < g_rc_nsub; i++) if (g_rc_sub[i].fence > done) g_rc_sub[n++] = g_rc_sub[i]; g_rc_nsub = n; }
static void rc_commit(uint64_t fence) {
    rc_retire();
    uint32_t need = g_rc_nsub + g_rc_npend;
    if (need > g_rc_scap) { uint32_t cap = g_rc_scap ? g_rc_scap : 256; while (cap < need) cap *= 2; rc_sub *p = realloc(g_rc_sub, (size_t)cap * sizeof *p); if (!p) { red("%s", "条件渲染：已提交的时间戳报告表扩不了"); return; } g_rc_sub = p; g_rc_scap = cap; }
    for (uint32_t i = 0; i < g_rc_npend; i++) g_rc_sub[g_rc_nsub++] = (rc_sub){g_rc_pend[i], fence};
}
static int rc_occ_has(uint64_t a) { if (!g_rc_ocap) return 0; for (uint32_t i = (uint32_t)(a >> 4) & (g_rc_ocap - 1);; i = (i + 1) & (g_rc_ocap - 1)) { if (g_rc_occ[i] == a) return 1; if (!g_rc_occ[i]) return 0; } }
static void rc_occ_add(uint64_t a) {
    if (!a || rc_occ_has(a)) return;
    if ((g_rc_on + 1) * 2 > g_rc_ocap) { uint32_t oc = g_rc_ocap, nc = oc ? oc * 2 : 4096; uint64_t *old = g_rc_occ, *t = calloc(nc, 8); if (!t) { red("%s", "条件渲染：查询地址表扩不了"); return; }
        for (uint32_t k = 0; k < oc; k++) if (old[k]) { uint32_t i = (uint32_t)(old[k] >> 4) & (nc - 1); while (t[i]) i = (i + 1) & (nc - 1); t[i] = old[k]; }
        g_rc_occ = t; g_rc_ocap = nc; free(old); }
    uint32_t i = (uint32_t)(a >> 4) & (g_rc_ocap - 1); while (g_rc_occ[i]) i = (i + 1) & (g_rc_ocap - 1);
    g_rc_occ[i] = a; g_rc_on++;
}
static void rc_note(uint64_t a, int timestamp) {
    if (!timestamp) { rc_occ_add(a); return; }
    for (uint32_t i = 0; i < g_rc_npend; i++) if (g_rc_pend[i] == a) return;
    if (g_rc_npend == g_rc_pcap) { uint32_t nc = g_rc_pcap ? g_rc_pcap * 2 : 256; uint64_t *t = realloc(g_rc_pend, (size_t)nc * 8); if (!t) { red("%s", "条件渲染：时间戳报告表扩不了"); return; } g_rc_pend = t; g_rc_pcap = nc; }
    g_rc_pend[g_rc_npend++] = a;
}
static int rc_value(uint64_t a, uint64_t *v) {   // 0 = 回放这一刻不知道
    if (rc_occ_has(a)) return 0;
    for (uint32_t i = g_rc_npend; i-- > 0;) if (g_rc_pend[i] == a) { *v = 0; return 1; }
    for (uint32_t i = g_rc_nsub; i-- > 0;) if (g_rc_sub[i].addr == a) { if (g_rc_sub[i].fence > __atomic_load_n(&g_done_fence, __ATOMIC_ACQUIRE)) { *v = 0; return 1; } break; }   // 已完成：之后 CPU 写的算数
    memcpy(v, (const void *)(uintptr_t)a, 8); return 1;
}
static void rc_cond(const p_render_cond *rc) {
    if (rc->mode >= 2) { g_rc_skip = rc->mode == 3; return; }
    uint64_t x = 0, y = 0; int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y);
    g_rc_skip = known && (rc->mode ? x == y : x != y);
}

void rp_exec_reset(void) {   // 会话收尾：遮挡计数与条件渲染的账清掉
    g_occ_used = g_occ_counting = g_occ_seg = g_occ_pass = 0; g_occ_cur = -1; g_occ_next = 0; g_occ_acc = 0;
    g_rc_skip = 0; g_rc_npend = g_rc_nsub = 0; if (g_rc_occ) memset(g_rc_occ, 0, (size_t)g_rc_ocap * 8); g_rc_on = 0;
}

void nvn_be_sub_begin(void) {
    g_sub = calloc(1, sizeof *g_sub);
    rc_retire(); g_rc_npend = 0; g_rc_skip = 0;
    g_occ_seg = g_occ_used; g_occ_next = 0; g_occ_base = 0; g_occ_cur = -1; g_occ_pass = 0;
    nvn_gpu_err err = {0}; nvn_gpu_cmd_desc d = {g_occ_seg ? OCC_SLOTS : 0, "nvn submit"};
    g_cmd = NULL; g_pre = NULL; g_sub_serial++; g_seg_serial++; g_mid = 0; { uint64_t nd = 0, ns = 0; rp_draw_stats(&nd, &ns); g_split_base = nd; }
    if (!nvn_gpu_ok(nvn_gpu_cmd_begin(&d, &g_cmd, &err))) { g_cmd = NULL; static int said; if (!said++) red("开提交段失败（之后同样的失败不再逐条报）：%s", err.why); }
    rp_dirty_all();   // 新段：已发给后端的状态全部重发；rp_S / rp_R 不动
}

struct mid { struct sub *s; uint32_t base; uint8_t counted; uint64_t t_commit; };   // counted：这次中途段记进了 s->mids（完成回调里才减）；t_commit：提交时刻（算 GPU 忙）
static void mid_done(void *user, const nvn_gpu_done *d) {   // 中途交的段完成：遮挡槽的结果先攒进所属提交（它的最后一段还没完成：段按提交顺序完成）
    struct mid *m = user;
    if (d->status == NVN_GPU_DONE_FAILED) red("中途交的段失败：%s", d->why ? d->why : "?");
    else { if (m) busy_done(m->t_commit, now_ns());
        if (m && m->s->oc && d->occlusion) { for (uint32_t i = 0; i < d->nocclusion && m->base + i < 2048; i++) m->s->oc[m->base + i] = d->occlusion[i];
            uint32_t end = m->base + d->nocclusion < 2048 ? m->base + d->nocclusion : 2048; if (end > m->s->noc) m->s->noc = end; } }
    if (m && m->counted && !__atomic_sub_fetch(&m->s->mids, 1, __ATOMIC_ACQ_REL) && __atomic_load_n(&m->s->orphan, __ATOMIC_ACQUIRE)) sub_release(m->s);
    free(m);
}
// 中途先交一段：这次提交回放到这里为止录下的命令先交给 GPU 去做，回放接着往下录进新的一段。
// 一整次提交回放完才交的话，CPU 回放时 GPU 闲着、GPU 做的时候游戏线程（多半接着就等这次提交做完）闲着，两头串着干。
// 只在通道外、换渲染目标之后交；用了遮挡计数的提交不拆（查询池按段建）。回写、事件、同步对象都挂在最后一段上，语义不变。
// 什么时候交：换渲染目标时，自上次交段起画够了 SPLIT_DRAWS 笔。
// 做法来自安卓模拟器 Vulkan 后端的自动提交（compat/tree/src/Ryujinx.Graphics.Vulkan/AutoFlushCounter.cs：换帧缓冲时按时间与笔数交）。
// 它的阈值（10 笔、1 毫秒，以及「快交模式」下通道中间也交）照搬过来在真机上不行：这边交一段要把状态全部重发、另开一段，
// 固定成本比它高，标题画面每帧交到 10–15 段、回放一度每帧 178 毫秒，进了「快交模式」每帧一百多段。
// 按笔数、只在换渲染目标处交，是真机上量过有效的形态（3D 那款场景每帧约 2000 笔、交 7 段：12.6 → 20–23 帧）。
// 调试开关 nvk_split 改每段的笔数，0 = 不交（整次提交只交一段）。
int rp_split_draws = 300;   // 每段的笔数（用例改小了测）
static void sub_split(void) {
    int thr = nvn_plat_flag("nvk_split", rp_split_draws);
    if (thr <= 0 || !g_cmd || !g_sub || g_mid >= SUB_STEP - 4 || rp_pass_is_open()) return;
    uint64_t nd = 0, ns = 0; rp_draw_stats(&nd, &ns);
    if (nd - g_split_base < (uint64_t)thr) return;
    g_split_base = nd;
    seg_cut(g_occ_seg);
}
// 中途交段本体（sub_split 与第一次遮挡复位共用）：先交当前段，再开新段。交出去的这一段进不进 mids 看它自己声明没声明槽（g_occ_seg），
// next_occ 是新段要不要声明槽。调用方保证 g_cmd / g_sub 在、通道已收、中间值没用完。
static void seg_cut(int next_occ) {
    nvn_gpu_err err = {0};
    if (g_pre) { nvn_gpu_cmd pc = g_pre; g_pre = NULL; if (!nvn_gpu_ok(nvn_gpu_cmd_commit(pc, g_fence + ++g_mid, pre_done, pre_note_make(), &err))) red("前置段提交失败：%s", err.why); }
    nvn_gpu_cmd c = g_cmd; g_cmd = NULL;
    struct mid *m = calloc(1, sizeof *m);   // 每个中途段都带一份提交时刻（算 GPU 忙用）；分配失败只是这一段不计，语义不变
    if (m) { m->s = g_sub; m->base = g_occ_base; m->t_commit = now_ns(); }
    if (g_occ_seg) { if (!g_sub->oc) g_sub->oc = calloc(2048, sizeof *g_sub->oc); if (m) { __atomic_add_fetch(&g_sub->mids, 1, __ATOMIC_ACQ_REL); m->counted = 1; } }
    if (!nvn_gpu_ok(nvn_gpu_cmd_commit(c, g_fence + ++g_mid, mid_done, m, &err))) red("中途交段失败：%s", err.why);
    pf_add(RP_PERF_SUBS, 1);
    g_occ_base = g_occ_next; g_occ_cur = -1; g_seg_serial++;
    g_occ_seg = next_occ;
    nvn_gpu_cmd_desc d = {g_occ_seg ? OCC_SLOTS : 0, "nvn submit"};
    if (!nvn_gpu_ok(nvn_gpu_cmd_begin(&d, &g_cmd, &err))) { g_cmd = NULL; red("中途交段后开新段失败：%s", err.why); }
    rp_dirty_all();
}

uint64_t nvn_be_sub_commit(void) {
    struct sub *s = g_sub; g_sub = NULL;
    rp_pass_flush(g_cmd);   // 打开的通道与待折清除不跨提交
    if (!g_cmd) {   // 没有段（开段失败，或中途交段之后开新段失败）：副作用无处挂，按失败丢弃（不写假成功）
        if (s) sub_orphan(s);
        if (g_pre) { nvn_gpu_cmd_abort(g_pre); g_pre = NULL; }
        // 时间线照样往前走一格：这次提交里的同步对象已经按 g_fence + SUB_STEP 发布（OP_FENCE_SYNC），中途交出去的段也已经用掉了这一格里的中间值。
        // 不走的话，下一次提交会把同一批值再用一遍（后端要求严格递增），而等这一格的游戏线程要到下一次提交做完才被放行。
        g_fence += SUB_STEP;
        return g_fence;
    }
    nvn_gpu_err err = {0}; uint64_t f = g_fence + SUB_STEP;
    if (g_pre) { nvn_gpu_cmd pc = g_pre; g_pre = NULL; if (!nvn_gpu_ok(nvn_gpu_cmd_commit(pc, g_fence + ++g_mid, pre_done, pre_note_make(), &err))) red("前置段提交失败：%s", err.why); }
    nvn_gpu_cmd c = g_cmd; g_cmd = NULL;
    // 契约：无论成败 done 恰好调一次（失败以 FAILED 调），所以 s 一定由 sub_done 释放
    uint64_t t0 = now_ns(); if (s) { s->t_commit = t0; s->fence = f; s->occ_base = g_occ_base; }
    if (s && (s->nfx || s->noe || s->ngw)) g_effect_fence = f;   // 这次提交做完时有东西要写回游戏内存（事件、时间戳以外的计数器报告、遮挡结果、GPU 写的缓冲）
    if (s && (s->n || s->noe || s->ngw)) { pf_add(RP_PERF_FX_SUBS, 1); pf_add(RP_PERF_FX_OCC, (uint64_t)s->noe); pf_add(RP_PERF_FX_GW, (uint64_t)s->ngw); }   // 这次提交做完时有东西要写回游戏内存（事件、计数器报告、遮挡结果、GPU 写的缓冲）
    rc_commit(f);
    if (!nvn_gpu_ok(nvn_gpu_cmd_commit(c, f, sub_done, s, &err))) red("提交失败：%s", err.why);
    pf_add(RP_PERF_COMMIT_NS, now_ns() - t0); pf_add(RP_PERF_SUBS, 1);
    g_fence = f;
    rp_debug_dump_poll(&g_fence);   // 调试开关 nvk_dump：有登记才做事
    return f;
}

// nvk_sync_lag=3：每帧只放行一次同步等待的额度。上屏时补一次，之后第一次 SyncWait 不真等，其余照旧。
// 用意：让下一帧的 CPU 回放与这一帧的显卡收尾重叠，同时保住帧内的新鲜度（帧内那几次等待仍然真等，
// 游戏的遮挡计数/条件判定拿到的是本帧已做完的结果），比「一律不等」轻得多。
static int g_lag_budget;
void rp_sync_lag_present(void) { static int on = -1; if (on < 0) on = nvn_plat_flag("nvk_sync_lag", 0) == 3; if (on) __atomic_store_n(&g_lag_budget, 1, __ATOMIC_RELEASE); }
uint64_t nvn_be_last_fence(void) { return g_fence; }
uint64_t rp_sub_fence(void) { return g_fence + SUB_STEP; }   // 正在回放的这次提交最后一段的栅栏值（它中途先交的段取的值都比这个小）
int rp_fence_done(uint64_t f) { return __atomic_load_n(&g_done_fence, __ATOMIC_ACQUIRE) >= f; }
// 游戏调 QueueFinish 时要等到哪个栅栏值（前端在提交锁里取值，出了锁再等）。
// 真机上 Finish 等的是「显卡把已提交的都做完」。游戏能观察到的只有两样：做完时写回它内存的东西，和它之后再提交的活排在后面。
// 有的游戏每帧呈现前都调一次：回放完一帧之后在这里再等显卡把这一帧做完，显卡这一帧的活和下一帧的回放完全串着。
// 开关 nvk_finish_lag（缺省 1；0 = 照旧等到最后一次提交）：
//   缺省开的依据：一款每帧都调 Finish 的 3D 游戏真机同局对照，稳定场景 28.2 → 29.2–29.6 帧，画面一致、别的回写一样不少；
//   项目定的顺序是先把目标帧率做到、再压能耗。哪一款因为时间戳晚一帧出问题，用这个开关关掉来确认。
//   1 = 只等到「上一次 Finish 时的最后一次提交」和「最近一次有东西要写回的提交」里靠后的那个，显卡上最多压着一帧的活，
//       下一帧的回放与这一帧的显卡工作重叠。「有东西要写回」不算时间戳报告：它是测量值，游戏 Finish 之后读到的是上一帧的时间戳
//       （用它调动态分辨率的游戏晚一帧得到反馈）；事件、遮挡结果、别的计数器、显卡写的缓冲照等，一样不少；
//   2 = 诊断档：什么回写都不等。回写照常在提交做完时落地，游戏最多晚一帧看到。只用来量上限，不是产品行为。
// 配套在 rp_buf.c：每次这样放行一帧，池镜像换到另一份（rp_buf_frame_flip），这一帧直接写的那份显卡已经不读了。
void rp_buf_frame_flip(void);
uint64_t nvn_be_finish_publish(void) {
    uint64_t last = g_fence; int lag = nvn_plat_flag("nvk_finish_lag", 1);
    if (!lag) { g_finish_prev = last; return last; }
    uint64_t t = lag == 2 || g_finish_prev > g_effect_fence ? g_finish_prev : g_effect_fence;
    g_finish_prev = last;
    if (t < last) { rp_buf_frame_flip(); pf_add(RP_PERF_FIN_SKIP, (last - t) / SUB_STEP); }   // 有提交没等：它可能还在读当前这份镜像
    return t;
}
// Finish 的等待单独记账：WAIT_* 记的是游戏主动等同步对象（nvnSyncWait 等），FINISH 记 QueueFinish。
// 分开才知道每帧 27–38 毫秒的等围栏里各占多少——两条路的根治法不一样（见 nvn_be_finish_publish 的开关说明）。
void nvn_be_finish_wait(uint64_t fence) {
    if (!fence) return;
    uint64_t t0 = now_ns();
    nvn_gpu_rc r = nvn_gpu_fence_wait(fence, 5000000000ull, NULL);
    pf_add(RP_PERF_WAIT_FIN_N, 1); pf_add(RP_PERF_WAIT_FIN_NS, now_ns() - t0);
    (void)r;   // 时间线已失败时这里也等不到：失败本身已在 sub_done 报红，Finish 不因此卡游戏线程
}

// WAIT_* 记的是游戏主动等同步对象的次数 / 耗时。入口先看这段活是不是早就做完（rp_fence_done）：是的话不必再下去等，
// 但仍照旧记一次 WAIT_N / WAIT_NS（耗时只含这次判定），另记一个 WAIT_ALREADY_N——它不为 0 说明我们把已完成的等待也当成了阻塞在等。
static int g_wrote_since_wait;   // 本轮等待之后有没有「显卡写游戏缓冲」的活（定义在 rp_post_gpu_write_rows 上面）
int nvn_be_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!fence) return 1;
    uint64_t t0 = now_ns();
    if (rp_fence_done(fence)) { pf_add(RP_PERF_WAIT_NS, now_ns() - t0); pf_add(RP_PERF_WAIT_N, 1); pf_add(RP_PERF_WAIT_ALREADY_N, 1); return 1; }
    {   // 开关 debug.s2a.nvk_sync_lag（缺省 0 = 旧行为：真等）：
        //   1 = 允许「最多一次提交还在显卡上飞」：等的时候若这一栅栏只领先已完成值一格就不真等（返回已满足），
        //       下一次提交前换一份池镜像（照 nvk_finish_lag 的做法，见 rp_buf_frame_flip），让 CPU 回放与显卡执行重叠。
        //       代价：游戏在这之后读 GPU 回写的内存最多晚一次提交看到。两份镜像决定了这里只能放行一格。
        //   2 = 诊断档：一律不等（量重叠上限；池镜像只有两份，可能读错内容，不用于出货）。
        // 缺省 1（允许「最多一次提交还在显卡上飞」）：真机上这一档把帧从 70 压到 40–47 毫秒、14 → 20–24 帧
        // （读数见坑记录续 25）；代价是游戏在这一帧读到的是上一次提交为止的显卡回写，剔除会偏保守、多记约 46% 绘制，
        // 但不会漏画（保守方向）。画面按同一取景逐帧核对通过。要回到「每笔都真等」：debug.s2a.nvk_sync_lag=0。
        static int lag = -1; if (lag < 0) lag = nvn_plat_flag("nvk_sync_lag", 1);
        if (lag > 0) {
            uint64_t done = __atomic_load_n(&g_done_fence, __ATOMIC_ACQUIRE);
            uint64_t ahead = fence > done ? (fence - done + SUB_STEP - 1) / SUB_STEP : 0;
            int allow = (uint64_t)lag >= ahead || lag >= 2;
            if (lag == 3) {   // 每帧一次：从上屏之后补的额度里扣，扣完就真等
                if (__atomic_load_n(&g_lag_budget, __ATOMIC_ACQUIRE) > 0 && __atomic_sub_fetch(&g_lag_budget, 1, __ATOMIC_ACQ_REL) >= 0) allow = 1;
                else allow = 0;
            }
            if (lag == 4) {   // 数据相关：显卡这一栅栏之前有没有写过游戏缓冲
                allow = (ahead == 0) || !__atomic_load_n(&g_wrote_since_wait, __ATOMIC_ACQUIRE);
            }
            if (allow) { rp_buf_frame_flip(); pf_add(RP_PERF_WAIT_NS, now_ns() - t0); pf_add(RP_PERF_WAIT_N, 1); pf_add(RP_PERF_WAIT_FAKED_N, 1); return 1; }
        } }
    nvn_gpu_rc r = nvn_gpu_fence_wait(fence, timeout_ns, NULL);
    __atomic_store_n(&g_wrote_since_wait, 0, __ATOMIC_RELEASE);   // 真等过：显卡写的缓冲已经落地
    pf_add(RP_PERF_WAIT_NS, now_ns() - t0); pf_add(RP_PERF_WAIT_N, 1);
    // 时间线已失败：再等也不会完成。按「已结束」放行，让游戏线程不死等；失败本身已在 sub_done 报红
    return nvn_gpu_ok(r) || r.code == NVN_GPU_E_FAILED || r.code == NVN_GPU_E_DEVICE;
}

static uint64_t g_fx_types;
uint64_t rp_fx_report_types(void) { return __atomic_exchange_n(&g_fx_types, 0, __ATOMIC_RELAXED); }
static void post_event(volatile uint32_t *val, uint32_t mode, uint32_t v) {
    struct sub *s = g_sub; if (!s || !val) return;
    if (s->n == s->cap) { int cap = s->cap ? s->cap * 2 : 8; struct post *np = realloc(s->p, (size_t)cap * sizeof *np); if (!np) { red("%s", "回写表扩不了，这条事件 / 计数器回写丢了"); return; } s->p = np; s->cap = cap; }
    s->p[s->n++] = (struct post){val, mode, v, 0, 0}; s->nfx++; pf_add(RP_PERF_FX_EVENT, 1);
}
// 一笔读回可有多行与多个层：先全登记，再交给后端；失败撤销本笔全部登记。
// 仅登记真正写到的行，不把行间 / 片间填充覆盖回游戏内存。
// 数据相关重叠（nvk_sync_lag=4）：这一栅栏之后是否有「显卡会写游戏缓冲」的活。有 → 真等（游戏接下来要读它）；
// 没有（只画不写）→ 放行，让 CPU 回放与显卡执行重叠。计算派发写存储缓冲 / 存储图像时走 rp_post_gpu_write* 登记，这里就是信号源。
int rp_post_gpu_write_rows(uint64_t addr, uint64_t bytes, uint64_t row, uint64_t image, uint32_t rows, uint32_t images) {
    __atomic_store_n(&g_wrote_since_wait, 1, __ATOMIC_RELEASE);
    struct sub *s = g_sub; uint64_t count = (uint64_t)rows * images;
    if (!s || !bytes || !count || count > (uint64_t)(INT_MAX - s->ngw)) return 0;
    int start = s->ngw, need = start + (int)count;
    if (need > s->gwcap) {
        int cap = s->gwcap ? s->gwcap : 4;
        while (cap < need && cap <= INT_MAX / 2) cap *= 2;
        if (cap < need) cap = need;
        if ((size_t)cap > SIZE_MAX / sizeof *s->gw) return 0;
        struct gw_ev *n = realloc(s->gw, (size_t)cap * sizeof *n);
        if (!n) return 0;
        s->gw = n; s->gwcap = cap;
    }
    for (uint32_t z = 0; z < images; z++) for (uint32_t y = 0; y < rows; y++) {
        uint64_t key = 0, id = rp_buf_gpu_write_note(addr + (uint64_t)z * image + (uint64_t)y * row, bytes, &key);
        if (!id) {
            while (s->ngw > start) { struct gw_ev v = s->gw[--s->ngw]; rp_buf_gpu_write_land(v.pool_key, v.id, 0); }
            return 0;
        }
        s->gw[s->ngw++] = (struct gw_ev){key, id};
    }
    return 1;
}
int rp_post_gpu_write(uint64_t addr, uint64_t len) { return rp_post_gpu_write_rows(addr, len, len, len, 1, 1); }

static void post_report(uint64_t addr, uint32_t type) {
    struct sub *s = g_sub; if (!s || !addr) return;
    if (s->n == s->cap) { int cap = s->cap ? s->cap * 2 : 8; struct post *np = realloc(s->p, (size_t)cap * sizeof *np); if (!np) { red("%s", "回写表扩不了，这条事件 / 计数器回写丢了"); return; } s->p = np; s->cap = cap; }
    s->p[s->n++] = (struct post){NULL, 0, 0, addr, type}; if (type != NVN_COUNTER_TYPE_TIMESTAMP && type != NVN_COUNTER_TYPE_TIMESTAMP_TOP) s->nfx++; pf_add(RP_PERF_FX_REPORT, 1); __atomic_fetch_or(&g_fx_types, 1ull << (type & 63), __ATOMIC_RELAXED);
}

void nvn_be_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }     // 存储前瞻索引：通道规划接上时再建
void nvn_be_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }

// ---------- 主循环 ----------
static int rec_bad(op_hdr h) { return h.size < 8 || (h.size & 7) || h.op >= OP_NVN2_END_ || h.op == OP_BEGIN; }

// 逐命令类型的耗时（开关 nvk_exec_tm 打开时才记）：窗口末尾由 rp_exec_dump_ops 取前几名，看「其余」那部分到底是哪些命令占的。
static uint64_t g_op_ns[64];
void rp_exec_dump_ops(char *b, size_t n) {
    if (!n || !b) return;
    size_t o = 0; b[0] = 0;
    for (int r = 0; r < 6; r++) {
        int best = -1;
        for (int i = 0; i < 64; i++) if (g_op_ns[i] && (best < 0 || g_op_ns[i] > g_op_ns[best])) best = i;
        if (best < 0) break;
        o += (size_t)snprintf(b + o, n - o, "%s%s %.1f 毫秒", r ? "、" : "", rp_op_name((uint32_t)best), (double)g_op_ns[best] / 1e6);
        g_op_ns[best] = 0;
        if (o >= n) break;
    }
}
// 帧级账（给「长帧」日志用）：本帧回放花了多久、画了多少笔、提交几次。上屏那边发现帧间隔异常时取走打印。
static uint64_t g_fr_exec_ns, g_fr_draws, g_fr_subs;
void rp_frame_take(uint64_t *exec_ns, uint64_t *draws, uint64_t *subs) {
    *exec_ns = __atomic_exchange_n(&g_fr_exec_ns, 0, __ATOMIC_RELAXED);
    *draws = __atomic_exchange_n(&g_fr_draws, 0, __ATOMIC_RELAXED);
    *subs = __atomic_exchange_n(&g_fr_subs, 0, __ATOMIC_RELAXED);
}
void nvn_be_exec(const uint8_t *p) {
    static int inited; if (!inited) { inited = 1; rp_state_reset(); }
    if (!p) return;
    const uint64_t fr_t0 = now_ns();
    // 回放耗时按命令类别分（开关 debug.s2a.nvk_exec_tm，缺省 0）：绘制类 vs 其余（传输 / 整屏清除 / 派发 / 状态），
    // 用来判「回放总时长里有多少不在 rp_draw 的各步上」。取时刻本身有开销，只在量的时候开。
    static int tm = -1;
    if (tm < 0) tm = nvn_plat_flag("nvk_exec_tm", 0) != 0;
    for (;;) {
        nvn_plat_safepoint();   // 一次提交可连续回放数秒：让平台的停世界事务在命令之间越过本线程（nvn_plat.h）
        op_hdr h; memcpy(&h, p, sizeof h);
        if (rec_bad(h)) { nvn_unimpl("rec", "bad_record", p); return; }   // 按这条列表的 END 处理
        uint64_t t_cmd = tm ? now_ns() : 0;
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: __atomic_fetch_add(&g_fr_exec_ns, now_ns() - fr_t0, __ATOMIC_RELAXED); return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (g_ret_n >= 64) { nvn_unimpl("rec", "call_depth_over_64", p); break; }
                g_ret_n++; nvn_be_exec((const uint8_t *)(uintptr_t)t); g_ret_n--;
                break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                if (m & NVN_EV_WAIT) nvn_unimpl("op", "OP_SIGNAL_EVENT+WAIT", p);   // GPU 侧等事件：跨队列顺序接上时做
                else post_event(val, m, v);
                break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8);
                if (s2a_nvn_render_cond()) rc_note(ad, t == NVN_COUNTER_TYPE_TIMESTAMP || t == NVN_COUNTER_TYPE_TIMESTAMP_TOP);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) occ_report(g_cmd, ad);
                else post_report(ad, (uint32_t)t);
                break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) occ_reset(g_cmd); break; }   // 其余计数器的报告值本来就写 0，复位无事可做
            case OP_RENDER_COND: rc_cond(a); break;
            case OP_DRAW: { if (g_rc_skip) break; p_draw t; rp_draw(g_cmd, draw_rec(a, h.size, &t), NULL, p); break; }
            case OP_DRAW_IDX: { if (g_rc_skip) break; p_draw_idx t; rp_draw(g_cmd, NULL, draw_idx_rec(a, h.size, &t), p); break; }
            case OP_MULTI_DRAW: { if (g_rc_skip) break; p_multi_draw m; if (h.size - 8 < sizeof m) { nvn_unimpl("rec", "multi_draw_short", p); break; } memcpy(&m, a, sizeof m); rp_multi_draw(g_cmd, &m, p); break; }
            case OP_SET_RT: { p_set_rt t; rp_pass_set_rt(g_cmd, set_rt_rec(a, h.size, &t)); sub_split(); break; }
            case OP_CLEAR_COLOR: if (!g_rc_skip) rp_pass_clear_color(g_cmd, a, p); break;
            case OP_CLEAR_DS: if (!g_rc_skip) rp_pass_clear_ds(g_cmd, a, p); break;
            case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4); rp_pass_barrier((uint32_t)bits); break; }
            case OP_COPY_T2B: rp_pass_flush(g_cmd); rp_op_copy_t2b(g_cmd, a, p); break;
            case OP_CLEAR_TEX: rp_pass_flush(g_cmd); rp_op_clear_tex(g_cmd, a, p); break;
            case OP_COPY_B2T: rp_pass_flush(g_cmd); rp_op_copy_b2t(g_cmd, a, p); break;   // 传输在通道外
            case OP_COPY_T2T: rp_pass_flush(g_cmd); rp_op_copy_t2t(g_cmd, a, p); break;   // 先落地待折清除：拷贝可能读写那些目标
            case OP_UPDATE_UB: rp_op_update_ub(a, p); break;
            case OP_COPY_B2B: rp_op_copy_b2b(a, p); break;
            case OP_CLEAR_BUF: rp_op_clear_buf(a, p); break;
            case OP_WAIT_SYNC: {
                const sync_t *s; void *ev; uint64_t v;
                memcpy(&s, a, sizeof s);
                if (!s || !sync_published(s, NULL)) nvn_unimpl("sync", "wait_sync_unpublished", p);
                else if (sync_ext(s, &ev, &v)) nvn_unimpl("sync", "wait_sync_external", p);
                // 已发布的 NVN 栅栏在同一后端队列上，之前的工作按提交顺序执行；无需 CPU 等待。
                break;
            }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); if (s) sync_publish(s, g_fence + SUB_STEP); break; }   // 本次提交主段的栅栏值
            case OP_DISPATCH: {
                const p_dispatch *dp = a; static int said;   // 调试开关 nvk_drawlog：头几次把它的输入列出来
                if (said < 6 && nvn_plat_flag("nvk_drawlog", 0)) { said++; char b[700]; int o = snprintf(b, sizeof b, "计算派发 %dx%dx%d 间接 %#llx 着色器键 %02x%02x%02x%02x 有效 %d；", dp->x, dp->y, dp->z, (unsigned long long)dp->ind,
                        rp_S.skey[5][0], rp_S.skey[5][1], rp_S.skey[5][2], rp_S.skey[5][3], rp_S.skey_ok >> 5 & 1);
                    for (int i = 0; i < 16; i++) if (rp_S.ubo[5][i].addr) o += snprintf(b + o, sizeof b - (size_t)o, " 常量%d(%llu)", i, (unsigned long long)rp_S.ubo[5][i].size);
                    for (int i = 0; i < 16; i++) if (rp_S.ssbo[5][i].addr) o += snprintf(b + o, sizeof b - (size_t)o, " 存储%d(%llu)", i, (unsigned long long)rp_S.ssbo[5][i].size);
                    for (int i = 0; i < 32; i++) if (rp_S.img[5][i]) o += snprintf(b + o, sizeof b - (size_t)o, " 图像%d(#%llu)", i, (unsigned long long)(rp_S.img[5][i] & 0xFFFFF));
                    for (int i = 0; i < 32; i++) if (rp_S.tex[5][i]) o += snprintf(b + o, sizeof b - (size_t)o, " 贴图%d(#%llu)", i, (unsigned long long)(rp_S.tex[5][i] & 0xFFFFF));
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "dispatch", b); }
                rp_dispatch(g_cmd, dp, p);
                break; }
            default:
                if (!rp_state_op(h.op, a, h.size)) nvn_unimpl("op", rp_op_name(h.op), p);
                break;
        }
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) __atomic_fetch_add(&g_fr_draws, 1, __ATOMIC_RELAXED);   // 帧级账：这一帧画了多少笔
        // OP_CALL 不记：它里面那批记录各自会记（记了就把同一段算两遍）
        if (tm && h.op != OP_CALL) {
            uint64_t dns = now_ns() - t_cmd;
            if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) pf_add(RP_PERF_EXEC_DRAW_NS, dns); else { pf_add(RP_PERF_EXEC_OTHER_NS, dns); if (h.op < 64) __atomic_fetch_add(&g_op_ns[h.op], dns, __ATOMIC_RELAXED); }
        }
        p += h.size;
    }
}

// 一次 NVN 队列提交（hle_nvn.c nvnQueueSubmitCommands）。跨队列停靠（等别的队列还没回放的信号）未接：见 OP_SIGNAL_EVENT+WAIT。
void nvn_be_submit(void *q, const uint64_t *h, int n) {
    __atomic_fetch_add(&g_fr_subs, 1, __ATOMIC_RELAXED);   // 帧级账：这一帧提交了几次
    nvn_hle_queue_enter(q);
    uint64_t t0 = now_ns();
    // 一次提交拆三段记账（SUBMIT_NS 仍是含三段的整段）：开段（cmd_begin + 状态重发） / 主回放循环 / 收尾交后端
    // （收尾里含 rp_pass_flush、前置段与主段的 nvn_gpu_cmd_commit）。三段加起来与 SUBMIT_NS 的差额是取时刻本身的开销。
    nvn_be_sub_begin(); nvn_hle_piece_begun();
    uint64_t t1 = now_ns();
    nvn_be_sub_index(h, n);
    for (int i = 0; i < n; i++) { nvn_be_exec_rest(h + i + 1, n - i - 1); nvn_be_exec((const uint8_t *)(uintptr_t)h[i]); }
    nvn_be_exec_rest(NULL, 0);
    uint64_t t2 = now_ns();
    nvn_be_sub_commit();
    uint64_t t3 = now_ns();
    pf_add(RP_PERF_SUB_PRE_NS, t1 - t0); pf_add(RP_PERF_SUB_EXEC_NS, t2 - t1); pf_add(RP_PERF_SUB_POST_NS, t3 - t2);
    pf_add(RP_PERF_SUBMIT_NS, t3 - t0);
}
