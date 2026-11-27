// 回放核心（graphics/nvn/replay）的宿主单测：纯 C，后端与平台件都是本文件里的替身，不需要 GPU。
// 期望值都由用例自己写下的输入直接得出，不经被测实现计算。
#include "nvn_obj.h"
#include "nvn2_cmd.h"
#include "nvn_backend.h"
#include "nvn_fmt_info.h"
#include "nvn_mtl.h"
#include "../replay/rp_state.h"
#include "../replay/rp_exec.h"
#include "../replay/rp_fps_seg.h"
#include "../replay/rp_res.h"
#include "../replay/rp_shader.h"
#include "nvn_refl.h"
#include "nvn_dirty.h"
#include "nvn_deswizzle.h"
#include "../replay/nvn_unimpl.h"
#include "../replay/nvn_plat.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static int g_fail, g_checks;
// 红行立刻刷出：dirty_span_memo_mutation_test.py 经管道读本进程的输出，管道上 stdio 整块缓冲；变异体放行池外访问后，
// 后面的用例可能读写池外而崩溃（崩不崩取决于平台的静态布局），没刷出的红行会随进程一起丢，脚本就看不到「红在哪条断言」。
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("【红】 %s:%d  %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

// ---------- 平台替身 ----------
static int g_red_lines; static char g_last_red[200];
static int g_pf_red; static char g_pf_last[640];   // 图形管线失败分类的红行（rp_pfail_note）
static int g_arena_truncated_lines;
static int g_arena_detail_lines; static char g_arena_detail[256]; static char g_arena697_lines[32][256];
static char g_ps_lines[8][320]; static int g_ps_n;   // 池统计行（cls "poolstats"）收下来给 t_pool_stats 核
static char g_tdiag_lines[32][320], g_tdiag_summary[320]; static int g_tdiag_n, g_tdiag_summaries, g_flag_tdiag;
static int g_csd_lines; static char g_csd_line[256];   // 类型 3 句柄的采样器号没登记、绑默认采样器：每局首次的那一行（#1594 片 6）
static int g_ubo_zero_lines; static char g_ubo_zero_line[256];   // 常量缓冲句柄指向没登记的号、绑零贴图：每局首次的那一行（#1594 片 4）
static int g_dl_unread, g_dl_lines;   // 逐笔日志（cls "draw"）：含「读不到」的行数 / 含「存」字段的贴图行数
static int g_mm_lines; static char g_mm_last[200];   // #1810：「贴图类型不符」行数与最近一行
static char g_spill_line[300]; static const char *g_cs_note_stub;   // spill 派发日志行（回放层，cls "replay"）/ 后端给的「没走出库」原因替身
const char *nvk_shader_cs_lib_note(const uint8_t key[8]) { (void)key; return g_cs_note_stub; }   // 弱符号的真实现在 nvk_shader.c；这里给回放核心用例一个可控替身
void nvn_plat_log(int level, const char *cls, const char *text) { if (cls && text && !strcmp(cls, "replay") && strstr(text, "spill 派发")) snprintf(g_spill_line, sizeof g_spill_line, "%s", text); if (cls && text && !strcmp(cls, "tex") && strstr(text, "贴图类型不符")) { g_mm_lines++; snprintf(g_mm_last, sizeof g_mm_last, "%s", text); } if (cls && text && !strcmp(cls, "draw")) { if (strstr(text, "读不到")) g_dl_unread++; if (strstr(text, " 存")) g_dl_lines++; } if (cls && !strcmp(cls, "tex-reupload-summary")) { snprintf(g_tdiag_summary, sizeof g_tdiag_summary, "%s", text); g_tdiag_summaries++; return; } if (cls && !strcmp(cls, "tex-reupload")) { if (g_tdiag_n < 32) snprintf(g_tdiag_lines[g_tdiag_n], 320, "%s", text); g_tdiag_n++; return; } if (level == NVN_PLAT_LOG_INFO && text && strstr(text, "类型 3 句柄的采样器号")) { g_csd_lines++; snprintf(g_csd_line, sizeof g_csd_line, "%s", text); } if (level == NVN_PLAT_LOG_INFO && text && strstr(text, "常量缓冲句柄指向没登记的贴图号")) { g_ubo_zero_lines++; snprintf(g_ubo_zero_line, sizeof g_ubo_zero_line, "%s", text); } if (cls && !strcmp(cls, "poolstats")) { if (g_ps_n < 8) snprintf(g_ps_lines[g_ps_n], 320, "%s", text); g_ps_n++; return; } if(level == NVN_PLAT_LOG_RED && strstr(text,"后续不同原因详情省略（最多8）"))g_arena_truncated_lines++; if (level == NVN_PLAT_LOG_RED && strstr(text,"arena_alloc_failed rc=")) { if(g_arena_detail_lines<32)snprintf(g_arena697_lines[g_arena_detail_lines],256,"%s",text); g_arena_detail_lines++; snprintf(g_arena_detail,sizeof g_arena_detail,"%s",text); } if (level == NVN_PLAT_LOG_RED && text && strstr(text, "图形管线建失败")) { g_pf_red++; snprintf(g_pf_last, sizeof g_pf_last, "%s", text); } if (level == NVN_PLAT_LOG_RED) { g_red_lines++; snprintf(g_last_red, sizeof g_last_red, "%s", text); } }
static int g_flag_finish_lag;   // 用例设：开关 nvk_finish_lag 的值
static int g_flag_spill = 1;    // 用例设：开关 nvk_spill（缺省同产品：开）
static int g_flag_sync_lag;     // 用例设：开关 nvk_sync_lag 的值（产品缺省 1＝允许一格在飞；用例固定 0，测的是「真等」这条契约）
static int g_flag_binst_hoist = -1;   // 用例设：开关 nvk_binst_hoist（-1 = 缺省）
static int g_flag_clear_only_attachments;
static int g_flag_mirror_disjoint = -1;
static int g_flag_state_keep = 1, g_flag_rt_cont = 1, g_flag_clear_park = 1, g_flag_pass_area = 1;   // 用例设：开关 nvk_state_keep / nvk_rt_cont / nvk_clear_park / nvk_pass_area 的值（缺省同产品：都开）
static int g_flag_pool_memo = 1;   // 用例设：开关 nvk_pool_memo 的值（缺省同产品：开）；备忘每 4096 次查找才重读一次，换值后要用 pm_flag() 推一下
static int g_flag_land_merge = 1;  // 用例设：开关 nvk_land_merge 的值（缺省同产品：开）；每 256 次登记才重读一次，换值后要用 rp_test_gw_merge_reload() 推一下
static int g_flag_buf_dirtylog = -1, g_dirtylog_dflt = -1;   // 用例设：开关 nvk_buf_dirtylog（-1 = 没设，取产品写的缺省；--dirtylog-off 整套以 0 跑一遍）。受跟踪的池都有替身日志，见「脏页追加日志替身」
static int g_flag_land_noresync = -1;
static int g_flag_vf_key = 1;      // 用例设：开关 nvk_vf_key 的值（缺省同产品：视图表按存储认视图）；每 256 次取视图重读一次，换值后要用 vf_push() 推一下
static int g_flag_ub_steal = -1;   // 用例设：开关 nvk_ub_fill_steal（-1 = 缺省）；rp_draw 缓存它，换值后要 pass_fresh（会话重开清缓存）
static int g_flag_split_pass = -1, g_flag_diag_load_dc, g_flag_diag_store_dc;   // 用例设：开关 nvk_split_pass（-1 = 缺省）、诊断开关 nvk_diag_load_dontcare / nvk_diag_store_dontcare
static int g_flag_pool_stats;   // 用例设：开关 pool_stats（缺省同产品：关）
static int g_diag_frames;
void s2a_direct_diag_frame(void) { g_diag_frames++; }
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_tex_reupload_diag")) return g_flag_tdiag;
    if (!strcmp(name, "nvk_buf_dirtylog")) return g_flag_buf_dirtylog < 0 ? dflt : g_flag_buf_dirtylog;
    if (!strcmp(name, "nvk_land_noresync")) return g_flag_land_noresync < 0 ? dflt : g_flag_land_noresync;
    if (!strcmp(name, "pool_stats")) return g_flag_pool_stats;
    if (!strcmp(name, "nvk_spill")) return g_flag_spill;
    if (!strcmp(name, "nvk_pool_memo")) return g_flag_pool_memo;
    if (!strcmp(name, "nvk_land_merge")) return g_flag_land_merge;
    if (!strcmp(name, "nvk_vf_key")) return g_flag_vf_key;
    if (!strcmp(name, "nvk_finish_lag")) return g_flag_finish_lag;
    if (!strcmp(name, "nvk_sync_lag")) return g_flag_sync_lag;
    if (!strcmp(name, "nvk_state_keep")) return g_flag_state_keep;
    if (!strcmp(name, "nvk_rt_cont")) return g_flag_rt_cont;
    if (!strcmp(name, "nvk_clear_park")) return g_flag_clear_park;
    if (!strcmp(name, "nvk_clear_only_attachments")) return g_flag_clear_only_attachments;
    if (!strcmp(name, "nvk_pass_area")) return g_flag_pass_area;
    if (!strcmp(name, "nvk_ub_fill_steal")) return g_flag_ub_steal < 0 ? dflt : g_flag_ub_steal;
    if (!strcmp(name, "nvk_split_pass")) return g_flag_split_pass < 0 ? dflt : g_flag_split_pass;
    if (!strcmp(name, "nvk_diag_load_dontcare")) return g_flag_diag_load_dc;
    if (!strcmp(name, "nvk_diag_store_dontcare")) return g_flag_diag_store_dc;
    if (!strcmp(name, "nvk_binst_hoist")) return g_flag_binst_hoist < 0 ? dflt : g_flag_binst_hoist;
    if (!strcmp(name, "nvk_mirror_disjoint")) return g_flag_mirror_disjoint < 0 ? dflt : g_flag_mirror_disjoint;
    return dflt;
}
const char *nvn_plat_dir(int which) { (void)which; return NULL; }
static int g_safepoints; void nvn_plat_safepoint(void) { g_safepoints++; }   // 平台安全点：数回放循环过了几次

// ---------- 前端替身 ----------
static int g_q_enter, g_piece;
void nvn_hle_queue_enter(void *q) { (void)q; g_q_enter++; }
void nvn_hle_piece_begun(void) { g_piece++; }
int nvn_dpool_base(const void *pool, int tex) { return (int)(uintptr_t)pool + (tex ? 0 : 1000); }
// 程序快照替身：宿主表默认没有快照；按阶段绑定的用例按需给出 stage_of / kok / nstage。
static const prog_t *g_snap_prog;
static uint8_t g_snap_ok;
static uint8_t g_snap_keys[6][8];
static uint8_t g_snap_stage[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    if (!g_snap_prog || g_snap_prog->serial != serial) return NULL;
    if (keys) memcpy(keys, g_snap_keys, sizeof g_snap_keys);
    if (ok) *ok = g_snap_ok;
    if (stage) memcpy(stage, g_snap_stage, 6);
    return g_snap_prog;
}
int dm_fps_setting(void) { return 30; }
int s2a_nvn_rast_flip = 1;   // 建队列时窗口原点在左下（NVN 缺省）
uint64_t nvn_ga2ca(uint64_t a) { return a + 0x1000; }   // 与恒等不同：证明延后绑定确实做了地址换算
static int g_ft_hit;   // 脏页追踪的替身（真的在 hle_nvn.c）：nvn_ft_written 此刻的答案，用例直接定
int nvn_ft_written(const tex_t *t) { if (g_ft_hit) nvn_be_tex_cpu_written(t); return g_ft_hit; }   // 与 hle_nvn.c 一致：命中时它自己记「CPU 写过」（会把 GPU 写过的归属清掉）

// ---------- 后端替身：段只记账；完成由用例手动触发 ----------
struct nvn_gpu_cmd_s { int live, is_pre; uint32_t nts; nvn_gpu_timestamp_result ts[64]; };
static struct nvn_gpu_cmd_s g_fake_cmd, g_fake_pre;   // 主段 / 前置段各一个替身
// 只供读写顺序夹具：编码时保存写入快照和绘制所用缓冲，最后按各段「前置、主段」顺序模拟 GPU 执行。
static int g_order_active, g_order_seg, g_order_n, g_order_overflow;
static nvn_gpu_buf g_order_vbuf; static uint64_t g_order_voff;
static struct { int seg, pre, draw; nvn_gpu_buf buf; uint64_t off, len; uint8_t data[4096]; } g_order_ev[16];

static int g_aborts;
static int g_begins, g_commits;
// ReadTexels 读 GPU 内容（rt_gpu）的替身：提交锁计数、提交值、「等的时候 GPU 正好做完」、copy_tex_buf 往共享缓冲里写可算的「GPU 内容」
static int g_rt_locks, g_rt_unlocks, g_auto_done, g_tb_fill; static uint64_t g_last_signal;
void nvn_hle_submit_lock(void) { g_rt_locks++; CHECK(g_rt_locks == g_rt_unlocks + 1); }      // 不重入：拿之前一定放过
void nvn_hle_submit_unlock(void) { g_rt_unlocks++; CHECK(g_rt_unlocks == g_rt_locks); }
static uint8_t gpu_byte(uint32_t layer, uint32_t level, int yo, int xo, uint64_t y, uint64_t i) { return (uint8_t)(0x40 + layer * 17 + level * 5 + ((uint64_t)yo + y) * 3 + ((uint64_t)xo + i)); }
static void fake_complete(uint32_t status);   // 定义在本文件后面
static struct { nvn_gpu_done_fn fn; void *user; uint64_t signal; uint32_t nts; nvn_gpu_timestamp_result ts[64]; } g_pend[72]; static int g_npend, g_main_commits;
static int g_gdraws, g_ncd, g_cd[16];   // g_cd：每个主段提交时累计画了几笔（超长通道中途先交的用例看每段的笔数）   // g_pend：一次提交最多 64 个中间值 + 主段；g_main_commits：不算前置段
static uint64_t g_fdone;
static uint32_t g_occ_slots_decl; static int g_occ_log[32], g_nocc_log; static int g_rcond;
int s2a_nvn_render_cond(void) { return g_rcond; }
static int g_ts_calls; static uint32_t g_ts_types[128];
#ifdef S2A_TEST_TS_ALLOC
int s2a_test_fail_ts_alloc;
#endif
void nvn_gpu_timestamp(nvn_gpu_cmd c, uint32_t slot, uint32_t type) {
    CHECK(slot < 64); if (slot >= 64) return;
    if (g_ts_calls < 128) g_ts_types[g_ts_calls] = type;
    g_ts_calls++; c->ts[slot] = (nvn_gpu_timestamp_result){1000 + 100 * (uint64_t)g_ts_calls, 1}; c->nts = slot + 1;
}
void nvn_gpu_occlusion(nvn_gpu_cmd c, int32_t slot) { (void)c; if (g_nocc_log < 32) g_occ_log[g_nocc_log] = slot; g_nocc_log++; }
static int g_begin_fail_at;   // 第几次开主段失败（1 = 下一次；0 = 都成功）
static int g_pre_fail_once;
nvn_gpu_rc nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *d, nvn_gpu_cmd *out, nvn_gpu_err *err) { (void)err; g_begins++;
    struct nvn_gpu_cmd_s *c = d && d->label && strstr(d->label, "pre") ? &g_fake_pre : &g_fake_cmd;
    if (c == &g_fake_cmd && g_begin_fail_at && !--g_begin_fail_at) { *out = NULL; if (err) err->why[0] = 0; return (nvn_gpu_rc){NVN_GPU_E_OOM}; }
    if (c == &g_fake_pre && g_pre_fail_once) { g_pre_fail_once = 0; *out = NULL; return (nvn_gpu_rc){NVN_GPU_E_OOM}; }
    c->nts = 0; memset(c->ts, 0, sizeof c->ts); c->live = 1; c->is_pre = c == &g_fake_pre; *out = c;
    if (g_order_active && !c->is_pre) g_order_seg++;
    if (!c->is_pre) g_occ_slots_decl = d ? d->occlusion_slots : 0;
    return (nvn_gpu_rc){NVN_GPU_OK}; }
// 暂存填充替身：g_fill_defer = 0 就地拷（缺省）；= 1 挂起不拷，到 fill_wait(票号) / 段提交 / 段放弃时才按交出顺序拷——模仿 Vulkan 后端交录制线程的时序，
// 让「改游戏内存前没等填完」在单测里看得见（拷出来的是改过之后的内容）。fill_take(票号) 只拷那一笔（代拷），之后按序轮到它时跳过、不再拷
enum { FILL_MAX = 64 };
static int g_fill_defer; static struct { uint8_t *d; const void *s; uint64_t n, t; int taken; } g_fill_q[FILL_MAX]; static int g_fill_nq, g_fill_head;
static uint64_t g_fill_seq, g_fill_done; static int g_fill_waits, g_fill_takes;   // g_fill_waits：带票号、真去等了还没填完的次数；g_fill_takes：真代拷了的笔数
static struct { uint8_t *d; uint64_t t; } g_fill_log[FILL_MAX]; static int g_fill_nlog;   // 交出过的填充（目标、票号），按交出顺序
static void fill_run_to(uint64_t t) { while (g_fill_head < g_fill_nq && g_fill_q[g_fill_head].t <= t) { if (!g_fill_q[g_fill_head].taken) memcpy(g_fill_q[g_fill_head].d, g_fill_q[g_fill_head].s, (size_t)g_fill_q[g_fill_head].n); g_fill_done = g_fill_q[g_fill_head].t; g_fill_head++; } }
static void fill_reset(int defer) { fill_run_to(UINT64_MAX); g_fill_defer = defer; g_fill_nq = g_fill_head = 0; g_fill_waits = g_fill_takes = 0; g_fill_nlog = 0; }
uint64_t nvn_gpu_arena_fill(nvn_gpu_cmd c, const nvn_gpu_slice *sl, const void *src, uint64_t bytes) { (void)c;
    if (!g_fill_defer || g_fill_nq == FILL_MAX) { fill_run_to(UINT64_MAX); memcpy(sl->cpu, src, (size_t)bytes); return 0; }
    const uint64_t t = ++g_fill_seq; g_fill_q[g_fill_nq].d = sl->cpu; g_fill_q[g_fill_nq].s = src; g_fill_q[g_fill_nq].n = bytes; g_fill_q[g_fill_nq].t = t; g_fill_q[g_fill_nq].taken = 0; g_fill_nq++;
    if (g_fill_nlog < FILL_MAX) { g_fill_log[g_fill_nlog].d = sl->cpu; g_fill_log[g_fill_nlog].t = t; g_fill_nlog++; }
    return t; }
uint64_t nvn_gpu_arena_fill_wait(uint64_t t) { if (t && g_fill_done < t) { g_fill_waits++; fill_run_to(t); } return g_fill_done; }
int nvn_gpu_arena_fill_take(uint64_t t) {
    if (!t || g_fill_done >= t) return 0;
    for (int i = g_fill_head; i < g_fill_nq; i++) if (g_fill_q[i].t == t) {
        if (g_fill_q[i].taken) return 0;
        memcpy(g_fill_q[i].d, g_fill_q[i].s, (size_t)g_fill_q[i].n); g_fill_q[i].taken = 1; g_fill_takes++; return 1;
    }
    return 0; }
void nvn_gpu_cmd_abort(nvn_gpu_cmd c) { fill_run_to(UINT64_MAX); c->live = 0; g_aborts++; }
nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd c, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    (void)err; fill_run_to(UINT64_MAX); c->live = 0; g_commits++; g_last_signal = signal; if (!c->is_pre) { g_main_commits++; if (g_ncd < 16) g_cd[g_ncd++] = g_gdraws; } g_pend[g_npend].fn = done; g_pend[g_npend].user = user; g_pend[g_npend].signal = signal; g_pend[g_npend].nts = c->nts; memcpy(g_pend[g_npend].ts, c->ts, sizeof c->ts); g_npend++; return (nvn_gpu_rc){NVN_GPU_OK}; }
nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t timeout_ns, nvn_gpu_err *err) { (void)timeout_ns; (void)err; if (g_auto_done && v > g_fdone && g_npend) fake_complete(NVN_GPU_DONE_OK); return (nvn_gpu_rc){v <= g_fdone ? NVN_GPU_OK : NVN_GPU_E_TIMEOUT}; }
// 贴图 / 视图 / 采样器 / 上传：只记账
struct nvn_gpu_tex_s { nvn_gpu_tex_desc d; uint64_t access_id; int ord; uint32_t px[4][4]; int views, released; }; struct nvn_gpu_view_s { nvn_gpu_tex of; int has_v; int rid; view_t v; nvn_gpu_buf buf; uint64_t off, bytes; uint32_t format; }; struct nvn_gpu_samp_s { int x; };   // ord：后端贴图的递增序号；px：像素替身（4 个 mip × 4 层各一个 32 位字）；v：建视图时收到的视图记录
nvn_gpu_tex nvn_gpu_view_texture(nvn_gpu_view v) { return v ? v->of : NULL; }
static int g_tex_creates, g_tex_releases, g_view_creates, g_view_releases, g_samp_creates, g_samp_releases, g_tex_create_fail, g_tex_ord;
// 像素替身（视图表按存储认视图的用例用）：每份后端存储有一小块「像素」——4 个 mip × 4 层各一个 32 位字，初值 0。「画」= 往视图指到的（mip，层）那个字里写
// 一个值；写进去的值要经过视图记录的签名（格式 / 通道映射不同的视图，同一次绘制写进去的字节不同，真 GPU 上也一样）；「读回」= 把存储的 16 个字原样取出。
// 视图的内容只取决于（后端贴图，视图记录），所以同样的画面序列走新旧两条取视图的路径，每帧读回的像素必须逐字节相同——
// 不同就说明某次画进了别的存储 / 别的层 / 别的 mip / 用了别种视图。
static uint32_t view_sig(const struct nvn_gpu_view_s *w) { uint32_t h = 2166136261u ^ (uint32_t)w->has_v; if (w->has_v) for (size_t i = 0; i < sizeof w->v; i++) h = (h ^ ((const uint8_t *)&w->v)[i]) * 16777619u; return h; }
static void fake_paint(const struct nvn_gpu_view_s *w, uint32_t val) {
    const uint32_t lvl = w->has_v ? w->v.base_level : 0, layer = w->has_v ? w->v.min_layer : 0;   // 没带视图记录 = 第 0 个 mip、第 0 层
    if (lvl < 4 && layer < 4) w->of->px[lvl][layer] = val ^ view_sig(w);
}
static int g_vid; static int g_released_id[8192]; static int g_nrel;   // 视图生命周期取证：每个视图一个递增 id，release 记下 id
static int was_view_released(int rid) { for (int i = 0; i < g_nrel; i++) if (g_released_id[i] == rid) return 1; return 0; }
static uint32_t g_last_usage; static uint16_t g_last_swz;
static view_t g_vw_last; static int g_vw_last_has;   // 最近一次 view_create 收到的视图记录（NULL = 没带）
static struct { uint32_t level, layer; uint64_t pitch, img; int y, w, h, pre; uint8_t first[64]; } g_up[16]; static int g_nup;
nvn_gpu_rc nvn_gpu_tex_create(const nvn_gpu_tex_desc *d, nvn_gpu_tex *out, nvn_gpu_err *err) {
    if (g_tex_create_fail) { snprintf(err->why, sizeof err->why, "替身：不支持"); *out = NULL; return (nvn_gpu_rc){NVN_GPU_E_UNSUPPORTED}; }
    // 同真后端（backend_vulkan.c nvn_gpu_tex_create）：建不出一维 / 一维数组目标，回放核心必须先折成二维 / 二维数组（#1810）
    if (d->d.target == NVN_TEXTURE_TARGET_1D || d->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) {
        snprintf(err->why, sizeof err->why, "替身（同真后端）：target %d 本段不支持", d->d.target); *out = NULL; return (nvn_gpu_rc){NVN_GPU_E_UNSUPPORTED}; }
    g_tex_creates++; g_last_usage = d->usage; g_last_swz = d->swz; *out = calloc(1, sizeof **out); (*out)->d = *d; (*out)->ord = ++g_tex_ord; return (nvn_gpu_rc){NVN_GPU_OK}; }
// 引用语义同真后端（backend_vulkan.c：建视图 t->refs++，release 时还有视图就只记 released，最后一个视图拆掉时才拆）：
// g_tex_held = 此刻「已 release、还被视图拿着」的后端贴图张数（真机显存账里的 dangling）。
static int g_tex_held;
void nvn_gpu_tex_release(nvn_gpu_tex t) { g_tex_releases++; if (t && t->views > 0) { t->released = 1; g_tex_held++; } else free(t); }
nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex t, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) { (void)err; g_view_creates++; g_vw_last_has = v != NULL; if (v) g_vw_last = *v; *out = calloc(1, sizeof **out); (*out)->of = t; if (t) t->views++; (*out)->has_v = v != NULL; if (v) (*out)->v = *v; (*out)->rid = ++g_vid; return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_view_release(nvn_gpu_view v) { g_view_releases++; if (v && g_nrel < 8192) g_released_id[g_nrel++] = v->rid;
    if (v && v->of && --v->of->views == 0 && v->of->released) { free(v->of); g_tex_held--; }
    free(v); }
// 环绕的替身语义同真后端（#976）：MIRROR_CLAMP / MIRROR_CLAMP_TO_BORDER 永远具名 UNSUPPORTED；g_samp_no_mce = 设备没有镜像夹到边的扩展。
static int g_samp_no_mce, g_samp_tries, g_samp_fail_nearest; static samp_par g_samp_last;
nvn_gpu_rc nvn_gpu_samp_create(const samp_par *p, nvn_gpu_samp *out, nvn_gpu_err *err) {
    g_samp_tries++; *out = NULL;
    if (g_samp_fail_nearest && p->magf == NVN_MAG_FILTER_NEAREST) { snprintf(err->why, sizeof err->why, "模拟内存不足"); return (nvn_gpu_rc){NVN_GPU_E_OOM}; }
    const uint32_t w[3] = {p->ws, p->wt, p->wr};
    for (int i = 0; i < 3; i++) {
        const char *why = w[i] == NVN_WRAP_MODE_MIRROR_CLAMP ? "sampler_wrap_mirror_clamp：替身" : w[i] == NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER ? "sampler_wrap_mirror_clamp_to_border：替身"
                        : w[i] == NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE && g_samp_no_mce ? "sampler_mirror_clamp_to_edge：替身设备没有扩展" : NULL;
        if (why) { snprintf(err->why, sizeof err->why, "%s", why); return (nvn_gpu_rc){NVN_GPU_E_UNSUPPORTED}; }
    }
    g_samp_creates++; g_samp_last = *p; *out = calloc(1, sizeof **out); (*out)->x = p->ws; return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_samp_release(nvn_gpu_samp s) { g_samp_releases++; free(s); }
static uint32_t g_caps_stor[4];   // 用例：这些格式在替身设备上也能当存储图像（默认只有 RGBA8）
static uint32_t g_caps_nosample = ~0u;   // 用例：这个格式在替身设备上不能直接采样（后端走计算解码的 BC 格式，能力位回 0）
static uint32_t g_caps_readback[4] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA_DXT1};   // 带 NVN_GPU_FMT_READBACK 的格式（用例可改）
static uint32_t g_caps_nofilter = ~0u, g_caps_noblend = ~0u;   // #1072：这个格式在替身设备上不能线性过滤 / 不能混合（FMT_FILTER / FMT_BLEND 去掉）
uint32_t nvn_gpu_format_caps(uint32_t f) { if (f == g_caps_nosample) return 0;
    uint32_t c = f == NVN_FORMAT_RGBA8 ? NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_RENDER | NVN_GPU_FMT_STORAGE | NVN_GPU_FMT_BLEND : f == NVN_FORMAT_R16 || f == NVN_FORMAT_RG32UI || f == NVN_FORMAT_RGBA32UI || f == NVN_FORMAT_RGBA32F || f == NVN_FORMAT_RGBA16UI ? NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_RENDER : NVN_GPU_FMT_SAMPLE;
    for (int k = 0; k < 4; k++) if (g_caps_stor[k] && f == g_caps_stor[k]) c = NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_STORAGE;   // 用例指定的存储图像格式
    if (f == NVN_FORMAT_DEPTH24_STENCIL8) c |= NVN_GPU_FMT_DEPTH;
    c |= NVN_GPU_FMT_FILTER;
    if (f == g_caps_nofilter) c &= ~(uint32_t)NVN_GPU_FMT_FILTER;
    if (f == g_caps_noblend) c &= ~(uint32_t)NVN_GPU_FMT_BLEND;
    for (int k = 0; k < 4; k++) if (g_caps_readback[k] && f == g_caps_readback[k]) c |= NVN_GPU_FMT_READBACK;
    return c; }
static int g_window_capture; static uint8_t g_window_linear[512], g_window_raw[512];
static uint64_t g_dirty_now = 100, g_dirty_written; static int g_protects, g_tdiag_race; static const void *g_prot_addr; static size_t g_prot_len;   // 最近一次保护的范围
static const void *g_prot_log[8]; static size_t g_prot_llen[8]; static int g_prot_n;   // 每次保护的范围（前 8 次），用例核「都只罩池内窗口」
void nvn_gpu_tex_upload(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t level, uint32_t layer, const NVNcopyRegion *r, const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    (void)t; if (g_nup == 16) return;
    g_up[g_nup].pre = c->is_pre; g_up[g_nup].level = level; g_up[g_nup].layer = layer; g_up[g_nup].pitch = row_bytes; g_up[g_nup].img = image_bytes; g_up[g_nup].y = r->yoffset; g_up[g_nup].w = r->width; g_up[g_nup].h = r->height;
    memcpy(g_up[g_nup].first, src, image_bytes < 64 ? image_bytes : 64);
    if (g_window_capture && image_bytes == sizeof g_window_linear) memcpy(g_window_linear, src, sizeof g_window_linear);
    g_nup++;
    if (g_tdiag_race) { g_dirty_written = ++g_dirty_now; g_tdiag_race = 0; } }
// 块线性上传替身：快照调用参数；能力开时回放必须传原始存储，不应先在线程解排。
static int g_block_uploads;
static struct { uint32_t level, layer; uint64_t bytes; const void *src; nvn_texlayout layout; } g_block_call[16];
static int g_block_rc;   // 用例设非 0：替身报该错误码（S2：后端校验失败回 rc，回放层走 CPU 具名回退）
nvn_gpu_rc nvn_gpu_tex_upload_blocklinear(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t level, uint32_t layer, const nvn_texlayout *L, const void *src, uint64_t bytes) {
    (void)c; (void)t;
    if (g_window_capture && bytes == sizeof g_window_raw) memcpy(g_window_raw, src, sizeof g_window_raw);   // 契约：入口返回前必须拍下源字节
    if (g_block_uploads < 16) { g_block_call[g_block_uploads].level = level; g_block_call[g_block_uploads].layer = layer; g_block_call[g_block_uploads].bytes = bytes; g_block_call[g_block_uploads].src = src; g_block_call[g_block_uploads].layout = *L; }
    g_block_uploads++;
    return (nvn_gpu_rc){ .code = g_block_rc };
}
// 脏页替身：代号由用例推进
void nvn_dirty_protect(const void *addr, size_t len) { g_prot_addr = addr; g_prot_len = len; if (g_prot_n < 8) { g_prot_log[g_prot_n] = addr; g_prot_llen[g_prot_n] = len; } g_prot_n++; g_protects++; }
void nvn_dirty_stats(uint64_t *f, uint64_t *p) { *f = 0; *p = 0; }
static uint64_t g_dirty_map = 1; uint64_t nvn_dirty_mapping_generation(void) { return g_dirty_map; }
static int g_block_unreliable;
int nvn_dirty_tracking_unreliable(void) { return g_block_unreliable; }
void nvn_dirty_mp_stats(uint64_t *c, uint64_t *g, uint64_t *n) { *c = 0; *g = 0; *n = 0; }
void nvn_dirty_span_stats(uint64_t *h, uint64_t *s, uint64_t *n) { *h = 0; *s = 0; *n = 0; }
void nvn_dirty_slot_stats(int *live, int *used, uint64_t *adds) { *live = 0; *used = 0; *adds = 0; }
uint64_t nvn_dirty_now(void) { return g_dirty_now; }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { (void)addr; (void)len; return g_dirty_written > stamp; }

// 缓冲替身：记每次按 GPU 顺序的写（偏移、长度、首 8 字节）
struct nvn_gpu_buf_s { uint64_t bytes, access_id; uint8_t *cpu; };
static int g_buf_cpu_auto;   // 1 = 新建的缓冲各带一块自己的 CPU 可见内存（两份池镜像要分得开的用例用）
static int g_buf_creates, g_buf_releases; static uint64_t g_buf_bytes;
static struct { uint64_t off, len; uint8_t head[8]; int pre; } g_bw[32]; static int g_nbw;
#ifdef S2A_GPU_LAND_TEST
static int g_land_capture;
static uint8_t g_land_staged[4096], g_land_mask[4096];
#endif
static uint64_t g_buf_fail_bytes;   // 非 0：建这个大小的缓冲时失败（池镜像建不出来的用例用）
nvn_gpu_rc nvn_gpu_buf_create(uint64_t bytes, int mem, const void *init, nvn_gpu_buf *out, nvn_gpu_err *err) { (void)mem; (void)init; (void)err; g_buf_creates++; g_buf_bytes = bytes; if (g_buf_fail_bytes && bytes == g_buf_fail_bytes) { *out = NULL; return (nvn_gpu_rc){NVN_GPU_E_OOM}; } *out = calloc(1, sizeof **out); (*out)->bytes = bytes; if (g_buf_cpu_auto) (*out)->cpu = calloc(1, bytes); return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_tex_info_get(nvn_gpu_tex t, nvn_gpu_tex_info *out) { memset(out, 0, sizeof *out); (void)t; }   // 调试读回用；用例不开那个开关
static struct { nvn_gpu_tex s, d; uint32_t sl, sy, dl, dy; NVNcopyRegion sr, dr; } g_ct[8]; static int g_nct;
void nvn_gpu_copy_tex(nvn_gpu_cmd c, nvn_gpu_tex src, uint32_t slevel, uint32_t slayer, const NVNcopyRegion *sr, nvn_gpu_tex dst, uint32_t dlevel, uint32_t dlayer, const NVNcopyRegion *dr) {
    (void)c; if (g_nct < 8) { g_ct[g_nct].s = src; g_ct[g_nct].d = dst; g_ct[g_nct].sl = slevel; g_ct[g_nct].sy = slayer; g_ct[g_nct].dl = dlevel; g_ct[g_nct].dy = dlayer; g_ct[g_nct].sr = *sr; g_ct[g_nct].dr = *dr; } g_nct++; }
static struct { nvn_gpu_view s, d; NVNcopyRegion sr, dr; int linear; } g_bl[4]; static int g_nbl;
void nvn_gpu_blit(nvn_gpu_cmd c, nvn_gpu_view s, const NVNcopyRegion *sr, nvn_gpu_view d, const NVNcopyRegion *dr, int linear) { (void)c; if (g_nbl < 4) { g_bl[g_nbl].s = s; g_bl[g_nbl].d = d; g_bl[g_nbl].sr = *sr; g_bl[g_nbl].dr = *dr; g_bl[g_nbl].linear = linear; } g_nbl++; }
// 贴图 ↔ 缓冲的拷贝：按发生的先后记一条（to_tex：1 = 缓冲到贴图；at_pass：这时已经开过几个通道 / 结束过几个）
static struct { int to_tex; nvn_gpu_tex t; uint32_t level, layer; int w, h; uint64_t row, img; int begun, ended; } g_tb[8]; static int g_ntb;
static int g_npd_now(void); static int g_ends_now(void);
static void tb_note(int to_tex, nvn_gpu_tex t, uint32_t level, uint32_t layer, const NVNcopyRegion *r, uint64_t row, uint64_t img) {
    if (g_ntb < 8) { g_tb[g_ntb].to_tex = to_tex; g_tb[g_ntb].t = t; g_tb[g_ntb].level = level; g_tb[g_ntb].layer = layer; g_tb[g_ntb].w = r ? r->width : 0; g_tb[g_ntb].h = r ? r->height : 0;
                     g_tb[g_ntb].row = row; g_tb[g_ntb].img = img; g_tb[g_ntb].begun = g_npd_now(); g_tb[g_ntb].ended = g_ends_now(); } g_ntb++; }
void nvn_gpu_copy_tex_buf(nvn_gpu_cmd c, nvn_gpu_tex src, uint32_t level, uint32_t layer, const NVNcopyRegion *r, nvn_gpu_buf dst, uint64_t off, uint64_t row_bytes, uint64_t image_bytes) {
    (void)c; tb_note(0, src, level, layer, r, row_bytes, image_bytes);
    if (g_tb_fill && dst && dst->cpu && r && row_bytes && image_bytes / row_bytes * row_bytes <= dst->bytes - off) {   // 「GPU 内容」= gpu_byte(层, 级, 区域原点, 行, 字节)
        for (uint64_t y = 0; y < image_bytes / row_bytes; y++) for (uint64_t i = 0; i < row_bytes; i++) dst->cpu[off + y * row_bytes + i] = gpu_byte(layer, level, r->yoffset, r->xoffset, y, i);
    } }
void nvn_gpu_copy_buf_tex(nvn_gpu_cmd c, nvn_gpu_buf src, uint64_t off, uint64_t row_bytes, uint64_t image_bytes, nvn_gpu_tex dst, uint32_t level, uint32_t layer, const NVNcopyRegion *r) {
    (void)c; (void)src; (void)off; tb_note(1, dst, level, layer, r, row_bytes, image_bytes); }
// 同存储不同视图共用代号；释放后再建不能因指针复用而沿用代号。
static uint64_t g_access_id;
int nvn_gpu_buf_access_span(nvn_gpu_buf b, uint64_t off, uint64_t bytes, nvn_gpu_access_span *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!b || !bytes || off > b->bytes || bytes > b->bytes - off) return 0;
    if (!b->access_id) b->access_id = ++g_access_id;
    out->storage = b->access_id; out->off = off; out->bytes = bytes; return 1;
}
int nvn_gpu_view_access_span(nvn_gpu_view v, nvn_gpu_access_span *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!v) return 0;
    if (v->buf) return nvn_gpu_buf_access_span(v->buf, v->off, v->bytes, out);
    if (!v->of) return 0;
    nvn_gpu_tex t = v->of;
    uint32_t levels = t->d.d.levels ? (uint32_t)t->d.d.levels : 1, layers = 1;
    if (t->d.d.target == NVN_TEXTURE_TARGET_1D_ARRAY || t->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY ||
        t->d.d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY || t->d.d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) layers = t->d.d.depth;
    else if (t->d.d.target == NVN_TEXTURE_TARGET_CUBEMAP) layers = 6;
    else if (t->d.d.target == NVN_TEXTURE_TARGET_3D) layers = t->d.d.depth;
    out->level = v->has_v && v->v.set_levels ? v->v.base_level : 0;
    out->levels = v->has_v && v->v.set_levels ? (v->v.nlevels ? v->v.nlevels : 1) : levels;
    out->layer = v->has_v && v->v.set_layers ? v->v.min_layer : 0;
    out->layers = v->has_v && v->v.set_layers ? (v->v.nlayers ? v->v.nlayers : 1) : layers;
    if (out->level >= levels || out->levels > levels - out->level || out->layer >= layers || out->layers > layers - out->layer) return 0;
    if (t->d.d.target == NVN_TEXTURE_TARGET_3D && !(v->has_v && v->v.set_target && v->v.target != NVN_TEXTURE_TARGET_3D)) {
        if (out->layer) return 0;
        uint32_t depth = out->level < 32 ? (uint32_t)t->d.d.depth >> out->level : 0;
        out->layer = 0; out->layers = depth ? depth : 1;
    }
    nvn_fmt_info fi;
    if (!nvn_fmt_info_of(t->d.d.format, &fi)) return 0;
    out->aspects = fi.depth ? 2u : fi.stencil ? 4u : 1u;
    if (fi.depth && fi.stencil && v->has_v && v->v.set_dsmode && v->v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL) out->aspects = 4;
    if (!t->access_id) t->access_id = ++g_access_id;
    out->storage = t->access_id; return 1;
}
static uint8_t *g_buf_cpu_fake;   // 用例要「GPU 写过镜像」时把它指到一块等大的内存；NULL = 这种缓冲 CPU 读不到
uint8_t *nvn_gpu_buf_cpu(nvn_gpu_buf b) { return b && b->cpu ? b->cpu : g_buf_cpu_fake; }
static const void *g_rel_log[64]; static int g_rel_n;   // 放过的缓冲指针（只比对、不解引用）：「放过的句柄又被录制」用例查它（用例开头清零，指针复用只在用例内比）
static int rel_log_has(const void *p) { for (int i = 0; i < g_rel_n; i++) if (g_rel_log[i] == p) return 1; return 0; }
void nvn_gpu_buf_release(nvn_gpu_buf b, nvn_gpu_retired_fn retired, void *user) { (void)retired; (void)user; g_buf_releases++; if (g_rel_n < 64) g_rel_log[g_rel_n++] = b; free(b->cpu); free(b); }
void nvn_gpu_buf_write(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off, const void *src, uint64_t bytes) {
    if (g_order_active) {
        if (g_order_n >= 16 || bytes > sizeof g_order_ev[0].data) g_order_overflow++;
        else { int i = g_order_n++; g_order_ev[i].seg = g_order_seg; g_order_ev[i].pre = c->is_pre; g_order_ev[i].draw = 0;
            g_order_ev[i].buf = dst; g_order_ev[i].off = off; g_order_ev[i].len = bytes; memcpy(g_order_ev[i].data, src, (size_t)bytes); }
    }
#ifdef S2A_GPU_LAND_TEST
    if (g_land_capture && off <= sizeof g_land_staged && bytes <= sizeof g_land_staged - off) {
        memcpy(g_land_staged + off, src, (size_t)bytes); memset(g_land_mask + off, 1, (size_t)bytes);
    }
#endif
    if (g_nbw == 32) return;
    g_bw[g_nbw].off = off; g_bw[g_nbw].len = bytes; g_bw[g_nbw].pre = c->is_pre; memcpy(g_bw[g_nbw].head, src, bytes < 8 ? bytes : 8); g_nbw++; }
// 页级脏账替身：4096 字节一页，g_pmem 这个池每页的写代号由用例设
enum { PG = 4096, NPG = 8 };
static _Alignas(4096) uint8_t g_pmem[PG * NPG]; static uint64_t g_pg_gen[NPG]; static int g_pg_tracked = 1;
enum { BLOCK_TEST_PAGES = 256, BLOCK_TEST_ALIGN = 64 * PG };
static _Alignas(4096) uint8_t g_block_arena[(BLOCK_TEST_PAGES + 64) * PG];
static uint8_t *g_block_mem;
static uint64_t g_block_gen[BLOCK_TEST_PAGES], g_block_pages_examined;
size_t nvn_dirty_page_size(void) { return PG; }
// 不受写跟踪的池（用例用 g_pg_tracked=0 设）没有跟踪区间：真实现里也是返回 NULL，靠它判「每次使用都整段同步」。
static size_t g_span_len = BLOCK_TEST_PAGES * PG;   // 用例收窄：登记的「池」比 g_block_mem 段短，越过池尾的查询没跟踪句柄
const void *nvn_dirty_span(const void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    if (!g_pg_tracked || e < a) return NULL;
    if (g_block_mem && a >= (uintptr_t)g_block_mem && e <= (uintptr_t)g_block_mem + g_span_len) return g_block_mem;
    return a >= (uintptr_t)g_pmem && e <= (uintptr_t)g_pmem + sizeof g_pmem ? g_pmem : NULL;
}
// 脏页位图替身：g_wp_on 开着时按 g_wp_dirty（g_block_mem 起算的页号）答，关着答 -1（「位图不可用」→ 调用方整张传，老用例行为不变）
static int g_wp_on; static uint8_t g_wp_dirty[BLOCK_TEST_PAGES];
long nvn_dirty_written_pages(const void *span, const void *addr, size_t len, uint64_t stamp, uint64_t *bits, size_t nbits) {
    (void)stamp;
    if (!g_wp_on || !g_block_mem || span != g_block_mem || !len) return -1;
    size_t p = (size_t)((const uint8_t *)addr - g_block_mem) / PG, q = (size_t)((const uint8_t *)addr + len - 1 - g_block_mem) / PG, n = q - p + 1;
    if (q >= BLOCK_TEST_PAGES || n > nbits) return -1;
    memset(bits, 0, (n + 63) / 64 * sizeof *bits);
    for (size_t i = 0; i < n; i++) if (g_wp_dirty[p + i]) bits[i >> 6] |= 1ull << (i & 63);
    return (long)n;
}
long nvn_dirty_page_gens(const void *span, const void *addr, size_t len, uint64_t *gens, size_t n) {
    if (!span || !g_pg_tracked || g_block_unreliable) return -1;
    if (span == g_block_mem && g_block_mem) {
        size_t p = (size_t)((const uint8_t *)addr - g_block_mem) / PG, q = (size_t)((const uint8_t *)addr + len - 1 - g_block_mem) / PG;
        if (q >= BLOCK_TEST_PAGES || q - p + 1 > n) return -1;
        g_block_pages_examined += q - p + 1;
        for (size_t i = p; i <= q; i++) gens[i - p] = g_block_gen[i];
        return (long)(q - p + 1);
    }
    size_t p = (size_t)((const uint8_t *)addr - g_pmem) / PG, q = (size_t)((const uint8_t *)addr + len - 1 - g_pmem) / PG;
    if (q - p + 1 > n) return -1;
    for (size_t i = p; i <= q; i++) gens[i - p] = g_pg_gen[i];
    return (long)(q - p + 1);
}

// 脏页追加日志替身，两种：
//   手动（g_log_fake_on = 1，只给 g_block_mem）：用例经 fake_log_write 改一页内容并追加事件；容量 FAKE_LOG_CAP，超了读者判环绕。测环绕、加保护没落地这些边角。
//   自动（其余情形，g_pmem 与 g_block_mem 都给）：用例照旧直接推页代号（g_pg_gen / g_block_gen），替身在开 / 读日志时把「代号变了」的页补成事件，
//     对应真实现「页由只读变可写之前先记一条」；跟踪的区间增减 / 换底（g_dirty_map 变）= 重新登记：新身份，旧游标对不上，读者按代号重建。
//   这样开关缺省（开）那一档，整套用例里受跟踪的池都走日志路；开关关的那一档（--dirtylog-off）整套另跑一遍，保住逐页退回路径。
enum { FAKE_LOG_CAP = 64, AUTO_LOG_CAP = 1024 };
static int g_log_fake_on, g_log_fake_bad; static uint64_t g_log_head, g_log_ident = 7;
static struct { uintptr_t a, b; } g_log_ev[FAKE_LOG_CAP];
static uint8_t g_log_unarmed[BLOCK_TEST_PAGES];   // 1 = 这一页加保护被并发解保护挡住（nvn_dirty_page_armed 答 0）
static uint64_t g_alog_events, g_alog_reads;   // 自动日志一共补了多少条事件、被读了几次（用例核「缺省开那一档真走了日志路」）
static int fake_log_manual(const void *span) { return g_log_fake_on && g_block_mem && span == g_block_mem; }
static int fake_log_ok(const void *span) { return fake_log_manual(span) && !g_log_fake_bad && !g_block_unreliable; }
static uint64_t *auto_gens(const void *span, size_t *n, uintptr_t *base) {
    if (span == g_pmem) { *n = NPG; *base = (uintptr_t)g_pmem; return g_pg_gen; }
    if (g_block_mem && span == g_block_mem) { *n = BLOCK_TEST_PAGES; *base = (uintptr_t)g_block_mem; return g_block_gen; }
    return NULL;
}
struct auto_log { const void *span; uint64_t map, ident, head; uint64_t seen[BLOCK_TEST_PAGES]; struct { uintptr_t a, b; } ev[AUTO_LOG_CAP]; };
static struct auto_log g_alog[2];
static struct auto_log *auto_log(const void *span) {
    size_t n; uintptr_t base; uint64_t *gen = auto_gens(span, &n, &base);
    if (!gen || !g_pg_tracked || g_block_unreliable) return NULL;
    struct auto_log *l = &g_alog[span == g_pmem ? 0 : 1];
    if (l->span != span || l->map != g_dirty_map) {   // 新登记：之前的写不在日志里（读者开日志之后会按代号重建 / 新镜像整份置旧）
        l->span = span; l->map = g_dirty_map; l->ident = ++g_log_ident; memcpy(l->seen, gen, n * sizeof *gen);
    }
    for (size_t k = 0; k < n; k++) if (gen[k] != l->seen[k]) {   // 用「变了」不用「变大」：用例换场时会把代号清零，宁可多报不能漏
        l->seen[k] = gen[k]; l->ev[l->head % AUTO_LOG_CAP].a = base + k * PG; l->ev[l->head % AUTO_LOG_CAP].b = base + (k + 1) * PG; l->head++; g_alog_events++;
    }
    return l;
}
int nvn_dirty_log_open(const void *span, uint64_t *ident, uint64_t *cursor) {
    if (fake_log_manual(span)) { if (!fake_log_ok(span)) return 0; *ident = g_log_ident; *cursor = g_log_head; return 1; }
    struct auto_log *l = auto_log(span); if (!l) return 0;
    *ident = l->ident; *cursor = l->head; return 1;
}
int nvn_dirty_log_read(const void *span, uint64_t ident, uint64_t *cursor, void (*fn)(void *, uintptr_t, uintptr_t), void *user) {
    if (fake_log_manual(span)) {
        if (!fake_log_ok(span) || ident != g_log_ident || g_log_head - *cursor > FAKE_LOG_CAP) return 0;
        for (uint64_t i = *cursor; i < g_log_head; i++) fn(user, g_log_ev[i % FAKE_LOG_CAP].a, g_log_ev[i % FAKE_LOG_CAP].b);
        *cursor = g_log_head; return 1;
    }
    struct auto_log *l = auto_log(span); g_alog_reads++;
    if (!l || ident != l->ident || l->head - *cursor > AUTO_LOG_CAP) return 0;
    for (uint64_t i = *cursor; i < l->head; i++) fn(user, l->ev[i % AUTO_LOG_CAP].a, l->ev[i % AUTO_LOG_CAP].b);
    *cursor = l->head; return 1;
}
int nvn_dirty_page_armed(const void *span, const void *addr) {
    if (fake_log_manual(span)) {
        size_t k = (size_t)((const uint8_t *)addr - g_block_mem) / PG;
        return k < BLOCK_TEST_PAGES && !g_log_unarmed[k];
    }
    size_t n; uintptr_t base; return auto_gens(span, &n, &base) && g_pg_tracked && !g_block_unreliable;   // 自动日志：代号一变就有事件，等价于「同步时页已重新只读」
}
static void fake_log_event(size_t first, size_t last) {   // 第 first..last 页（含）变可写
    g_log_ev[g_log_head % FAKE_LOG_CAP].a = (uintptr_t)g_block_mem + first * PG;
    g_log_ev[g_log_head % FAKE_LOG_CAP].b = (uintptr_t)g_block_mem + (last + 1) * PG; g_log_head++;
}
static void fake_log_write(size_t page, uint8_t v) {   // 游戏写一页：内容、逐页代号、块版本、日志事件一起变（与真实现同序：事件先于写）
    fake_log_event(page, page);
    memset(g_block_mem + page * PG, v, PG); g_block_gen[page] = ++g_dirty_now;
}

// 通道替身：记每次开通道的描述
static int g_pass_open, g_bind_outside, g_draw_outside;   // 通道开着没有；图形阶段的绑定在通道外到达的次数（真后端会置粘滞、整段作废）
static int g_tbuf_created, g_tbuf_in_pass, g_tbuf_binds[6];
static struct { nvn_gpu_buf buf; uint64_t off, bytes; uint32_t format; } g_tbuf_bound[6], g_itbuf_bound;   // g_itbuf_bound：最后一次绑的存储纹素缓冲（IMAGE_TBUF；视图编码后就释放，绑定时记下）
nvn_gpu_rc nvn_gpu_view_create_buf(nvn_gpu_buf b, uint64_t off, uint64_t bytes, uint32_t fmt, nvn_gpu_view *out, nvn_gpu_err *err) {
    (void)err; *out = NULL;
    // These two formats are the only ones exercised here, both four bytes.
    if (!b || (fmt != NVN_FORMAT_R32UI && fmt != NVN_FORMAT_RGBA8UI) || !bytes || (off & 3) || (bytes & 3) || off > b->bytes || bytes > b->bytes - off)
        return (nvn_gpu_rc){NVN_GPU_E_ARG};
    nvn_gpu_view v = calloc(1, sizeof *v); if (!v) return (nvn_gpu_rc){NVN_GPU_E_OOM};
    v->buf = b; v->off = off; v->bytes = bytes; v->format = fmt; v->rid = ++g_vid;
    g_tbuf_created++; g_tbuf_in_pass += g_pass_open; *out = v; return (nvn_gpu_rc){NVN_GPU_OK};
}
static nvn_gpu_pass_desc g_pd[8]; static int g_npd, g_pass_ends; static uint32_t g_barrier_bits; static int g_barrier_at;   // g_barrier_at：收到屏障时已开过几个通道
// 单像素 RGBA8 夹具：通道开始时执行 LOAD_CLEAR，绘制时按实际绑定复制一个像素。
static int g_color_sample_test, g_color_sample_index = -1;
static nvn_gpu_view g_color_target;
struct nvn_gpu_pool_table_s { uint32_t kind, count; nvn_gpu_view views[]; };
static nvn_gpu_pool_table g_color_image_table;
void nvn_gpu_pass_begin(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d) {
    (void)c; if (g_npd < 8) g_pd[g_npd] = *d; g_npd++; g_pass_open = 1;
    if (!g_color_sample_test) return;
    g_color_target = d->color[0].view;
    CHECK(d->ncolor == 1 && d->w == 1 && d->h == 1 && g_color_target && g_color_target->of->d.d.format == NVN_FORMAT_RGBA8);
    if (g_color_target && d->color[0].load == NVN_GPU_LOAD_CLEAR) {
        uint32_t rgba = 0;
        for (int i = 0; i < 4; i++) rgba |= (uint32_t)(d->color[0].clear.f[i] * 255.0f + 0.5f) << (8 * i);
        g_color_target->of->px[0][0] = rgba;
    }
}
void nvn_gpu_pass_end(nvn_gpu_cmd c) { (void)c; g_pass_ends++; g_pass_open = 0; }
static int g_npd_now(void) { return g_npd; } static int g_ends_now(void) { return g_pass_ends; }
static int g_ndisp_now(void);
static struct { uint32_t bits; int ndisp; } g_bar[8]; static int g_nbar;   // 每次屏障：交了哪些位、当时已派发几次
void nvn_gpu_barrier(nvn_gpu_cmd c, uint32_t bits) { (void)c; g_barrier_bits |= bits; g_barrier_at = g_npd; if (g_nbar < 8) { g_bar[g_nbar].bits = bits; g_bar[g_nbar].ndisp = g_ndisp_now(); } g_nbar++; }
static int g_compute_read_barriers;
void nvn_gpu_compute_to_graphics(nvn_gpu_cmd c) { (void)c; CHECK(!g_pass_open); g_compute_read_barriers++; }
// 帧日志替身（生产在 android/native/nvn/nvn_vk_frame_log.c，回放核心按弱引用调）：记次数与最后一次读数（#1501 D①）
static int g_fl_presents, g_fl_fences; static uint64_t g_fl_acq_ns, g_fl_pres_ns;
void s2a_frame_log_present_timed(uint64_t acquire_ns, uint64_t present_ns) { g_fl_presents++; g_fl_acq_ns = acquire_ns; g_fl_pres_ns = present_ns; }
// ADPF 帧负载上报替身（rp_present.c nvn_be_present）：记调用次数与最后一次的序号 / 目标
static int g_adpf_begin_n, g_adpf_end_n, g_adpf_invis_n; static uint64_t g_adpf_begin_seq, g_adpf_end_seq; static int64_t g_adpf_end_target;
void s2a_adpf_work_begin(uint64_t seq) { g_adpf_begin_n++; g_adpf_begin_seq = seq; }
void s2a_adpf_frame_end(int64_t target_ns, uint64_t seq) { g_adpf_end_n++; g_adpf_end_seq = seq; g_adpf_end_target = target_ns; }
void s2a_adpf_invisible(void) { g_adpf_invis_n++; }
void nvn_vk_frame_log_fence_wait(uint64_t wait_ns) { (void)wait_ns; g_fl_fences++; }
static void t_framelog_fence(void) {   // 帧日志 fence 字段：同步等待 / Finish 等待每次记一次（口径同 RP_PERF_WAIT_N + WAIT_FIN_N），栅栏 0 不算等
    uint64_t saved = g_fdone; g_fdone = 0; g_fl_fences = 0;
    nvn_be_wait(0, 0); nvn_be_finish_wait(0);
    CHECK(g_fl_fences == 0);
    nvn_be_finish_wait(1);                       // Finish：等不到也记（等过了）
    CHECK(g_fl_fences == 1);
    nvn_be_wait(nvn_be_last_fence() + 1000, 0);  // 真等（替身里等不到）
    CHECK(g_fl_fences == 2);
    g_fdone = saved;
}

static struct { nvn_gpu_clear d; int npd, ends; } g_cr[8]; static int g_ncr;   // 矩形清：记描述，以及收到时已开 / 已关过几个通道
void nvn_gpu_clear_rect(nvn_gpu_cmd c, const nvn_gpu_clear *d) { (void)c; if (g_ncr < 8) { g_cr[g_ncr].d = *d; g_cr[g_ncr].npd = g_npd; g_cr[g_ncr].ends = g_pass_ends; } g_ncr++; }
static struct { nvn_gpu_draw_tex_desc d; int in_pass, npd; } g_dt[8]; static int g_ndt;   // DrawTexture（#1452）：记描述，以及收到时通道开着没有、已开过几个通道
void nvn_gpu_draw_tex(nvn_gpu_cmd c, const nvn_gpu_draw_tex_desc *d) { (void)c; if (g_ndt < 8) { g_dt[g_ndt].d = *d; g_dt[g_ndt].in_pass = g_pass_open; g_dt[g_ndt].npd = g_npd; } g_ndt++; }
static uint64_t g_tick;   // 游戏时钟替身
uint64_t s2a_guest_tick(void) { return g_tick; }
static int g_ts_convert_fail;
int s2a_guest_tick_from_host_ns(uint64_t ns, uint64_t *out) { if (g_ts_convert_fail) return 0; *out = ns / 10; return 1; }

// 上屏替身：取图结果按脚本给，呈现记下描述并按「没有上屏时刻」回调
struct nvn_gpu_surface_s { int x; };
static struct nvn_gpu_surface_s g_fake_surf;
static int g_acq_script[4], g_acq_i, g_rebuilds, g_presents; static nvn_gpu_present_desc g_last_pd;   // 脚本：0 成功 / 其它 = 错误码
uint64_t plat_now_ns(void) { return 5000000000ull; }
nvn_gpu_rc nvn_gpu_drawable_acquire(nvn_gpu_surface s, uint64_t timeout_ns, void *plat_frame, nvn_gpu_drawable *out, nvn_gpu_err *err) {
    (void)s; (void)timeout_ns; (void)plat_frame; (void)err; int code = g_acq_i < 4 ? g_acq_script[g_acq_i] : 0; g_acq_i++;
    memset(out, 0, sizeof *out); if (code) return (nvn_gpu_rc){code};
    out->w = 1280; out->h = 720; out->gen = 1; out->token = 1; return (nvn_gpu_rc){NVN_GPU_OK}; }
nvn_gpu_rc nvn_gpu_surface_rebuild(nvn_gpu_surface s, nvn_gpu_err *err) { (void)s; (void)err; g_rebuilds++; return (nvn_gpu_rc){NVN_GPU_OK}; }
nvn_gpu_rc nvn_gpu_present(nvn_gpu_surface s, const nvn_gpu_drawable *dr, const nvn_gpu_present_desc *d, uint64_t ticket, nvn_gpu_presented_fn presented, void *user, nvn_gpu_err *err) {
    (void)s; (void)dr; (void)err; g_presents++; g_last_pd = *d; nvn_gpu_shown sh = {ticket, NVN_GPU_SHOWN_UNKNOWN, 0}; presented(user, &sh); return (nvn_gpu_rc){NVN_GPU_OK}; }

// 绘制替身：着色器来源给一份固定布局的程序；后端记下管线描述、绑定和绘制参数
struct nvn_gpu_shader_s { int x; }; struct nvn_gpu_pipe_s { int x; };
static struct nvn_gpu_shader_s g_fsh[3]; static struct nvn_gpu_pipe_s g_fpipe;
static struct rp_prog g_fprog, g_variant_prog; static struct nvn_gpu_shader_s g_variant_fs; static int g_rt_variant;
static int g_resolves, g_resolve_rc; static struct rp_shader_req g_last_req;
static struct rp_prog g_gprog2; static const struct rp_prog *g_gresolve_next;   // 图形出库回退用例：拒收后解析交「现编版」g_gprog2
static const struct rp_prog *g_resolve_seq[4]; static int g_resolve_seq_n, g_resolve_seq_i;   // 分层通道用例：逐次解析依次交这几份程序（最后一份一直用）
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    g_resolves++; g_last_req = *q;
    if (!g_resolve_rc && g_resolve_seq_n) { *out = g_resolve_seq[g_resolve_seq_i < g_resolve_seq_n ? g_resolve_seq_i++ : g_resolve_seq_n - 1]; return 0; }
    *out = g_resolve_rc ? NULL : g_gresolve_next ? g_gresolve_next : (g_rt_variant && q->rt_format[0] == NVN_FORMAT_RGBA32UI ? &g_variant_prog : &g_fprog);
    return g_resolve_rc;
}
// 计算：程序、管线、派发的替身
static struct rp_prog g_cprog; static int g_cresolves, g_cresolve_rc, g_cpipe_gets, g_ndisp; static uint32_t g_disp[3]; static struct nvn_gpu_pipe_s *g_disp_pipe; static nvn_gpu_shader g_cpipe_sh;
static nvn_gpu_buf g_disp_ind; static uint64_t g_disp_off;
static int g_ndisp_now(void) { return g_ndisp; }
static nvn_gpu_binding g_cb[16]; static uint32_t g_ncb; static int g_cbinds;
static struct rp_prog g_cprog2; static const struct rp_prog *g_cresolve_next; static int g_lib_reject_calls;
int rp_shader_resolve_compute(const struct rp_prog **out) { g_cresolves++; *out = g_cresolve_rc ? NULL : (g_cresolve_next ? g_cresolve_next : &g_cprog); return g_cresolve_rc; }
// rp_shader_lib_reject 的替身：出库版标坏并切到「现编版」（计算 g_cprog2 / 图形 g_gprog2）；非出库版（lib_sourced=0）原样报 0
int rp_shader_lib_reject(const struct rp_prog *p) {
    g_lib_reject_calls++;
    if (!p || !p->lib_sourced) return 0;
    ((struct rp_prog *)p)->lib_bad = 1;
    if (p->is_compute) g_cresolve_next = &g_cprog2; else g_gresolve_next = &g_gprog2;
    return 1;
}
void rp_shader_reset(void) {}
static int g_pipe_rc; static const char *g_pipe_why;   // 替身建管线的返回码与原文（原文给失败分类用例）
static int g_pipe_gets, g_set_pipes, g_set_dyns, g_gdraws; static nvn_gpu_pipe_desc g_last_pdesc; static nvn_gpu_dyn g_last_dyn; static nvn_gpu_draw_args g_last_da;
static struct { uint32_t stream; uint64_t off; } g_vb[4]; static int g_nvb;
static nvn_gpu_binding g_fb[3][16]; static uint32_t g_nfb[3];
static _Alignas(16) uint8_t g_arena[1 << 18]; static uint64_t g_arena_used; static struct nvn_gpu_buf_s g_arena_buf = { .bytes = sizeof g_arena, .cpu = g_arena };
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) { (void)mode; g_pipe_gets++; g_last_pdesc = *d; *out = g_pipe_rc ? NULL : &g_fpipe; if (err) { memset(err, 0, sizeof *err); err->rc.code = g_pipe_rc; if (g_pipe_rc && g_pipe_why) snprintf(err->why, sizeof err->why, "%s", g_pipe_why); } return (nvn_gpu_rc){g_pipe_rc}; }
void nvn_gpu_set_pipe(nvn_gpu_cmd c, nvn_gpu_pipe p) { (void)c; (void)p; g_set_pipes++; }
static struct nvn_gpu_pipe_s g_fcpipe;
nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) { (void)mode; (void)err; g_cpipe_gets++; g_cpipe_sh = cs->sh; *out = &g_fcpipe; return (nvn_gpu_rc){NVN_GPU_OK}; }
static int g_ncopy4;   // 4 字节的缓冲拷贝：间接绘制把 GPU 侧 base instance 拷进驱动段 c0 + 0x644
// 每次 4 字节拷贝的源 / 目标与录它时已发了几笔绘制（核「前移的拷贝排在生产者之后」）；每笔绘制发出时两阶段驱动段绑在哪（核「每笔一个切片、读到自己的记录」）
static struct { nvn_gpu_buf s, d; uint64_t soff, doff; int at_draw; } g_c4[64]; static int g_nc4;
static struct { nvn_gpu_buf b[2]; uint64_t off[2]; } g_dd[64]; static int g_ndd, g_dd_on;
static int g_ncopyv, g_ncopy1; static uint32_t g_copyv_stages;   // 多区域拷贝的调用次数、单段 4 字节拷贝次数、多区域拷贝声明的读阶段（并集）
static int g_hz_bump_at = -1;   // 第几笔绘制发出时把 rp_gpu_wr_gen 加一（替身：主段里录了一次写镜像的传输）
// 按 GPU 顺序做的缓冲拷贝 / 填充（#1022：改 / 拷游戏内存的命令碰到还没落地的 GPU 写时走它）：g_gx_rec 开着就记下来
static int g_gx_rec, g_ngx; static struct { char op; nvn_gpu_buf s, d; uint64_t soff, doff, n; uint32_t v; } g_gx[8];
void nvn_gpu_fill_buf(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off, uint64_t bytes, uint32_t value) { (void)c;
    CHECK(g_gx_rec); if (g_ngx < 8) { g_gx[g_ngx].op = 'f'; g_gx[g_ngx].d = dst; g_gx[g_ngx].doff = off; g_gx[g_ngx].n = bytes; g_gx[g_ngx].v = value; g_ngx++; } }
void nvn_gpu_copy_buf(nvn_gpu_cmd c, nvn_gpu_buf src, uint64_t soff, nvn_gpu_buf dst, uint64_t doff, uint64_t bytes) {
    if (bytes == 4) { (void)c; g_ncopy1++; if (g_nc4 < 64) { g_c4[g_nc4].s = src; g_c4[g_nc4].soff = soff; g_c4[g_nc4].d = dst; g_c4[g_nc4].doff = doff; g_c4[g_nc4].at_draw = g_ndd; g_nc4++; } g_ncopy4++; return; }
    if (g_gx_rec) { if (g_ngx < 8) { g_gx[g_ngx].op = 'c'; g_gx[g_ngx].s = src; g_gx[g_ngx].soff = soff; g_gx[g_ngx].d = dst; g_gx[g_ngx].doff = doff; g_gx[g_ngx].n = bytes; g_ngx++; } return; }
    (void)c; (void)src; CHECK(dst == &g_arena_buf && g_buf_cpu_fake && bytes == 12);
    if (dst == &g_arena_buf && g_buf_cpu_fake) memcpy(g_arena + doff, g_buf_cpu_fake + soff, (size_t)bytes);
}
void nvn_gpu_copy_bufs_uniform(nvn_gpu_cmd c, const nvn_gpu_buf_copy *v, uint32_t n, uint32_t stages) { (void)c; g_ncopyv++; g_copyv_stages |= stages;
    for (uint32_t i = 0; i < n; i++) { CHECK(v[i].bytes == 4); if (g_nc4 < 64) { g_c4[g_nc4].s = v[i].src; g_c4[g_nc4].soff = v[i].soff; g_c4[g_nc4].d = v[i].dst; g_c4[g_nc4].doff = v[i].doff; g_c4[g_nc4].at_draw = g_ndd; g_nc4++; } g_ncopy4++; } }
void nvn_gpu_dispatch(nvn_gpu_cmd c, nvn_gpu_pipe cp, uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf ind, uint64_t ind_off) { (void)c; g_disp_ind = ind; g_disp_off = ind_off; g_ndisp++; g_disp[0] = x; g_disp[1] = y; g_disp[2] = z; g_disp_pipe = cp; }
void nvn_gpu_set_dyn(nvn_gpu_cmd c, const nvn_gpu_dyn *d) { (void)c; g_set_dyns++; g_last_dyn = *d; }
void nvn_gpu_set_vbuf(nvn_gpu_cmd c, uint32_t stream, nvn_gpu_buf b, uint64_t off) { (void)c; if (g_order_active && stream == 0) { g_order_vbuf = b; g_order_voff = off; } if (g_nvb < 4) { g_vb[g_nvb].stream = stream; g_vb[g_nvb].off = off; } g_nvb++; }
static int g_bind_calls[3];   // 图形两个阶段各发了几次绑定
void nvn_gpu_bind(nvn_gpu_cmd c, nvn_gpu_stage st, const nvn_gpu_binding *b, uint32_t n) { (void)c;
    for (uint32_t i = 0; i < n; i++) if (b[i].cls == NVN_GPU_RES_TBUF && b[i].u.view) {
        nvn_gpu_view v = b[i].u.view; g_tbuf_binds[st]++;
        g_tbuf_bound[st].buf = v->buf; g_tbuf_bound[st].off = v->off;
        g_tbuf_bound[st].bytes = v->bytes; g_tbuf_bound[st].format = v->format;
    }
    for (uint32_t i = 0; i < n; i++) if (b[i].cls == NVN_GPU_RES_IMAGE_TBUF && b[i].u.view) {
        nvn_gpu_view v = b[i].u.view; g_itbuf_bound.buf = v->buf; g_itbuf_bound.off = v->off; g_itbuf_bound.bytes = v->bytes; g_itbuf_bound.format = v->format;
    }
    if (st == NVN_GPU_SH_CS) { g_cbinds++; g_ncb = n; memcpy(g_cb, b, (n < 16 ? n : 16) * sizeof *b); return; }
    CHECK(st >= NVN_GPU_SH_VS && st <= NVN_GPU_SH_GS);
    if (st < NVN_GPU_SH_VS || st > NVN_GPU_SH_GS) return;
    if (!g_pass_open) g_bind_outside++;
    int i = (int)st; g_bind_calls[i]++; g_nfb[i] = n; memcpy(g_fb[i], b, (n < 16 ? n : 16) * sizeof *b); }
void nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a) {
    if (g_order_active) {
        if (g_order_n >= 16 || !g_order_vbuf) g_order_overflow++;
        else { int i = g_order_n++; g_order_ev[i].seg = g_order_seg; g_order_ev[i].pre = c->is_pre; g_order_ev[i].draw = 1; g_order_ev[i].buf = g_order_vbuf; g_order_ev[i].off = g_order_voff; }
    }
    g_gdraws++; g_last_da = *a; if (!g_pass_open) g_draw_outside++;
    if (g_color_sample_test) {
        nvn_gpu_view src = NULL;
        if (g_color_sample_index >= 0) {
            if (g_color_image_table && (uint32_t)g_color_sample_index < g_color_image_table->count)
                src = g_color_image_table->views[g_color_sample_index];
        } else if (g_nfb[NVN_GPU_SH_FS] == 1 && g_fb[NVN_GPU_SH_FS][0].cls == NVN_GPU_RES_TEX_SAMP) {
            src = g_fb[NVN_GPU_SH_FS][0].u.ts.view;
        }
        CHECK(g_pass_open && g_color_target && src && src->of && src->of->d.d.format == NVN_FORMAT_RGBA8);
        if (g_pass_open && g_color_target && src && src->of) g_color_target->of->px[0][0] = src->of->px[0][0];
    }
    if (g_dd_on && g_ndd < 64) { for (int st = 0; st < 2; st++) { g_dd[g_ndd].b[st] = g_fb[st][0].u.b.buf; g_dd[g_ndd].off[st] = g_fb[st][0].u.b.off; } }
    if (g_dd_on) { g_ndd++; if (g_ndd - 1 == g_hz_bump_at) rp_gpu_wr_gen++; }
}
static uint32_t g_caps_remove;
static uint32_t g_caps_extra;   // 用例临时打开的能力位（如间接计数多绘）
static uint32_t g_caps_min_ssbo; static uint64_t g_caps_max_ssbo;   // 用例临时设的 minStorageBufferOffsetAlignment / maxStorageBufferRange（0 = 不报，其余用例的口径）
static uint32_t g_caps_min_texel, g_caps_max_texel; static uint64_t g_caps_max_ubo;   // g_caps_max_ubo：用例临时设的 maxUniformBufferRange（0 = 不报）   // 用例临时设的 minTexelBufferOffsetAlignment / maxTexelBufferElements（0 = 不报）
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); out->min_ubo_align = 64; out->caps = (NVN_GPU_CAP_BLIT_SCALED | g_caps_extra) & ~g_caps_remove; out->min_ssbo_align = g_caps_min_ssbo; out->max_ssbo_range = g_caps_max_ssbo;
    out->min_texel_align = g_caps_min_texel; out->max_texel_elems = g_caps_max_texel; out->max_ubo_range = g_caps_max_ubo; }
static int g_arena697_fail;
nvn_gpu_rc nvn_gpu_arena_alloc(nvn_gpu_cmd c, uint64_t bytes, uint32_t align, nvn_gpu_slice *out, nvn_gpu_err *err) { (void)c; (void)err;
    if(g_arena697_fail) { memset(out,0,sizeof *out);memset(err,0,sizeof *err);
        err->rc.code=g_arena697_fail==3?NVN_GPU_OK:NVN_GPU_E_ORDER;
        if(g_arena697_fail!=2 && g_arena697_fail!=3) {
            memset(err->why,'X',sizeof err->why);char prefix[48];snprintf(prefix,sizeof prefix,"first-bind-outside-pass-%d ",g_arena697_fail);
            memcpy(err->why,prefix,strlen(prefix));err->why[sizeof err->why-1]=0; }
        return err->rc; }
    g_arena_used = (g_arena_used + align - 1) / align * align;
    if (g_arena_used > sizeof g_arena || bytes > sizeof g_arena - g_arena_used) {
        memset(out, 0, sizeof *out);
        if (err) { memset(err, 0, sizeof *err); err->rc.code = NVN_GPU_E_OOM; }
        return (nvn_gpu_rc){NVN_GPU_E_OOM};
    }
    out->buf = &g_arena_buf; out->off = g_arena_used; out->cpu = g_arena + g_arena_used; out->va = 0; g_arena_used += bytes; return (nvn_gpu_rc){NVN_GPU_OK}; }

static const uint64_t *g_fake_occ; static uint32_t g_fake_nocc;   // 下一次完成时交给回调的各槽样本数
static void fake_complete(uint32_t status) {
    for (int i = 0; i < g_npend; i++) { nvn_gpu_done d = {0}; d.signal = g_pend[i].signal; d.status = status; d.why = "替身失败";
        if (status != NVN_GPU_DONE_FAILED) { d.occlusion = g_fake_occ; d.nocclusion = g_fake_nocc; d.timestamps = g_pend[i].ts; d.ntimestamps = g_pend[i].nts; }
        g_pend[i].fn(g_pend[i].user, &d); if (status != NVN_GPU_DONE_FAILED) g_fdone = g_pend[i].signal; }
    g_npend = 0; g_fake_occ = NULL; g_fake_nocc = 0;
}

// ---------- 录制替身：按 nvn_obj.h 的记录格式写 ----------
typedef struct { uint8_t b[4096]; size_t n; } stream;
static void *put(stream *s, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7); op_hdr h = {op, sz};
    memcpy(s->b + s->n, &h, 8); void *a = s->b + s->n + 8; memset(a, 0, sz - 8); s->n += sz; return a;
}
static void end(stream *s) { put(s, OP_END, 0); }
static void submit1(stream *s) { uint64_t h = (uint64_t)(uintptr_t)s->b; nvn_be_submit(NULL, &h, 1); }

static void t_state_persists(void) {   // 结论 B：状态跨命令缓冲、跨提交继承
    stream a = {0}, b = {0};
    p_blend_color *bc = put(&a, OP_BLEND_COLOR, sizeof *bc); bc->rgba[0] = 1; bc->rgba[1] = 2; bc->rgba[2] = 3; bc->rgba[3] = 4;
    p_viewports *vp = put(&a, OP_VIEWPORTS, sizeof *vp); vp->first = 1; vp->n = 1; vp->v[0][0] = 10; vp->v[0][1] = 20; vp->v[0][2] = 640; vp->v[0][3] = 360;
    p_scissor *sc = put(&a, OP_SCISSOR, sizeof *sc); sc->x = 5; sc->y = 6; sc->w = 7; sc->h = 8;
    end(&a); end(&b);
    submit1(&a); fake_complete(NVN_GPU_DONE_OK);
    submit1(&b); fake_complete(NVN_GPU_DONE_OK);
    CHECK(rp_S.blend_color[0] == 1 && rp_S.blend_color[3] == 4);
    CHECK(rp_S.has_vp == 1 && rp_S.vp[1][0] == 10 && rp_S.vp[1][2] == 640 && rp_S.vp[0][2] == 0);
    CHECK(rp_R.has_sc == 1 && rp_R.sc.x == 5 && rp_R.sc.h == 8);
    CHECK(rp_S.dr[1] == 1 && rp_S.patch_size == 3 && rp_S.ms.enable == 1);   // NVN 默认值没被提交清掉
}

static void t_unimpl_names(void) {   // 没接的命令按命令名报出并计数，回放继续走完
    nvn_unimpl_reset(); int red0 = g_red_lines;
    stream a = {0};
    put(&a, OP_DRAW_TEX, sizeof(p_draw_tex)); put(&a, OP_MULTI_DRAW, sizeof(p_multi_draw)); put(&a, OP_MULTI_DRAW, sizeof(p_multi_draw)); put(&a, OP_DLSS, sizeof(p_dlss));
    p_alpha_ref *ar = put(&a, OP_ALPHA_REF, sizeof *ar); ar->ref = 0.25f;   // 排在未接命令之后：证明没有因首个未接而停
    end(&a);
    submit1(&a); fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_unimpl_count("op", "OP_DRAW_TEX") == 1);
    CHECK(nvn_unimpl_count("op", "OP_MULTI_DRAW") == 0 && nvn_unimpl_count("draw", "multi_draw_invalid") == 2);   // 已接：空记录（max 0）按非法报名
    CHECK(nvn_unimpl_count("op", "OP_DLSS") == 1);
    CHECK(nvn_unimpl_count("op", "OP_ALPHA_REF") == 0);
    CHECK(nvn_unimpl_kinds() == 3);
    CHECK(g_red_lines - red0 == 3);   // 每种只在首次出现打一行
    char buf[256]; nvn_unimpl_dump(buf, sizeof buf);
    CHECK(!strcmp(buf, "op\tOP_DRAW_TEX\t1\ndraw\tmulti_draw_invalid\t2\nop\tOP_DLSS\t1\n"));
    CHECK(rp_S.alpha_ref == 0.25f);
}

static void t_one_commit_per_submit(void) {   // 结论 C：绘制攒着，一次提交一段
    int b0 = g_begins, c0 = g_commits, q0 = g_q_enter, p0 = g_piece;
    stream a = {0}; for (int i = 0; i < 5; i++) put(&a, OP_DISPATCH, sizeof(p_dispatch)); end(&a);
    submit1(&a);
    CHECK(g_begins - b0 == 1 && g_commits - c0 == 1);
    CHECK(g_q_enter - q0 == 1 && g_piece - p0 == 1);
    fake_complete(NVN_GPU_DONE_OK);
}

static void t_event_after_done(void) {   // 事件值只在段完成后可见；失败段不回写
    volatile uint32_t ev = 0; stream a = {0};
    uint8_t *r = put(&a, OP_SIGNAL_EVENT, 16); volatile uint32_t *pv = &ev; uint32_t m = 0, v = 7; memcpy(r, &pv, 8); memcpy(r + 8, &m, 4); memcpy(r + 12, &v, 4);
    r = put(&a, OP_SIGNAL_EVENT, 16); m = 1; v = 3; memcpy(r, &pv, 8); memcpy(r + 8, &m, 4); memcpy(r + 12, &v, 4);
    end(&a);
    uint64_t f0 = nvn_be_last_fence();
    submit1(&a);
    CHECK(nvn_be_last_fence() == f0 + 64);             // 每次提交：前置段占 +1（没用到就空着），主段 +2
    CHECK(ev == 0);                                   // 已提交、未完成
    CHECK(nvn_be_wait(f0 + 2, 0) == 0);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(ev == 10);                                  // 写 7 再加 3
    CHECK(nvn_be_wait(f0 + 2, 0) == 1);
    submit1(&a); int red0 = g_red_lines; fake_complete(NVN_GPU_DONE_FAILED);
    CHECK(ev == 10);                                  // 失败段：不回写
    CHECK(g_red_lines - red0 == 1);
}

static void t_fence_sync(void) {
    sync_t s; memset(&s, 0, sizeof s); stream a = {0};
    uint8_t *r = put(&a, OP_FENCE_SYNC, 8); sync_t *ps = &s; memcpy(r, &ps, 8); end(&a);
    uint64_t f0 = nvn_be_last_fence();
    submit1(&a); fake_complete(NVN_GPU_DONE_OK);
    uint64_t got = 0; CHECK(sync_published(&s, &got) == 1 && got == f0 + 64 && nvn_be_last_fence() == f0 + 64);
}

// CALL 目标核对替身（真身在 hle_nvn.c，本单测不链前端）：缺省都算活录制；用例把 g_call_reused 设成某个句柄，它就回 -1（块已被复用）
static uint64_t g_call_reused;
int nvn_cmd_call_resolve(uint64_t h, uint64_t *to) { *to = h; return g_call_reused && h == g_call_reused ? -1 : 0; }
// #768338 安全阀：CALL 的目标块已被别的录制复用 → 跳过这次调用（子列表的设置不生效）、记一行红；CALL 之后的记录照常执行。
//   反证：去掉 rp_exec.c 的 ck < 0 跳过，子列表照执行，alpha_ref 被写成 0.25
static void t_call_reused(void) {
    stream a = {0}, sub = {0};
    p_alpha_ref *ar = put(&sub, OP_ALPHA_REF, sizeof *ar); ar->ref = 0.25f; end(&sub);
    uint8_t *r = put(&a, OP_CALL, 8); uint64_t h = (uint64_t)(uintptr_t)sub.b; memcpy(r, &h, 8);
    p_set_patch_size *ps = put(&a, OP_SET_PATCH_SIZE, sizeof *ps); ps->size = 6;
    end(&a);
    rp_S.alpha_ref = 0; int red0 = g_red_lines; g_call_reused = h;
    submit1(&a); fake_complete(NVN_GPU_DONE_OK);
    g_call_reused = 0;
    if (rp_S.alpha_ref != 0 || rp_S.patch_size != 6 || g_red_lines - red0 != 1) printf("CALL 目标已复用：alpha_ref %.2f（期望 0）、patch_size %u（期望 6）、红行 +%d（期望 1）\n", rp_S.alpha_ref, (unsigned)rp_S.patch_size, g_red_lines - red0);
    CHECK(rp_S.alpha_ref == 0 && rp_S.patch_size == 6 && g_red_lines - red0 == 1);
}

static void t_jump_call(void) {
    stream a = {0}, b = {0}, sub = {0};
    p_alpha_ref *ar = put(&sub, OP_ALPHA_REF, sizeof *ar); ar->ref = 0.5f; end(&sub);
    p_depth_range *dr = put(&b, OP_DEPTH_RANGE, sizeof *dr); dr->n = 0.125f; dr->f = 0.75f; end(&b);
    uint8_t *r = put(&a, OP_CALL, 8); uint64_t h = (uint64_t)(uintptr_t)sub.b; memcpy(r, &h, 8);
    p_set_patch_size *ps = put(&a, OP_SET_PATCH_SIZE, sizeof *ps); ps->size = 4;   // CALL 返回后继续
    r = put(&a, OP_JUMP, 8); const uint8_t *t = b.b; memcpy(r, &t, 8);
    p_set_patch_size *dead = put(&a, OP_SET_PATCH_SIZE, sizeof *dead); dead->size = 9;   // JUMP 之后的不该执行
    end(&a);
    g_safepoints = 0;
    submit1(&a); fake_complete(NVN_GPU_DONE_OK);
    CHECK(rp_S.alpha_ref == 0.5f && rp_S.patch_size == 4 && rp_S.dr[0] == 0.125f && rp_S.dr[1] == 0.75f);
    // 平台安全点：每条执行到的记录前过一次。a 里 CALL、SET_PATCH_SIZE、JUMP 三条，sub 里 ALPHA_REF、END 两条，b 里 DEPTH_RANGE、END 两条；JUMP 之后那条不执行
    CHECK(g_safepoints == 7);
}

static void t_bad_record(void) {
    nvn_unimpl_reset(); stream a = {0};
    p_set_patch_size *ps = put(&a, OP_SET_PATCH_SIZE, sizeof *ps); ps->size = 5;
    op_hdr bad = {OP_DRAW, 12}; memcpy(a.b + a.n, &bad, 8); a.n += 16;   // 长度不是 8 的倍数
    ps = put(&a, OP_SET_PATCH_SIZE, sizeof *ps); ps->size = 6; end(&a);
    submit1(&a); fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_unimpl_count("rec", "bad_record") == 1);
    CHECK(rp_S.patch_size == 5);   // 坏记录之后不再执行
}

static void t_dirty_bits(void) {
    stream a = {0};
    p_bind_state *bs = put(&a, OP_BIND_STATE, sizeof *bs); bs->kind = 4; poly_st po = {NVN_FACE_BACK, NVN_FRONT_FACE_CW, NVN_POLYGON_MODE_FILL, 0}; memcpy(bs->bytes, &po, sizeof po);
    end(&a);
    nvn_be_exec(a.b); rp_pso_dirty = 0; rp_dss_dirty = 0;
    nvn_be_exec(a.b);                         // 同一状态再绑一次：不标脏
    CHECK(rp_pso_dirty == 0 && rp_S.poly.cull == NVN_FACE_BACK);
    stream b = {0}; bs = put(&b, OP_BIND_STATE, sizeof *bs); bs->kind = 4; po.cull = NVN_FACE_FRONT; memcpy(bs->bytes, &po, sizeof po);
    p_stencil *st = put(&b, OP_STENCIL, sizeof *st); st->face = 3; st->which = 2; st->ref = 9;   // 参考值是动态状态：不标深度模板脏
    end(&b);
    nvn_be_exec(b.b);
    CHECK(rp_pso_dirty == 1 && rp_dss_dirty == 0 && rp_S.st_ref[0] == 9 && rp_S.st_ref[1] == 9);
}

static void t_deferred_bind(void) {   // 延后绑定：回放到这条时才读游戏内存，且做 GPU 地址换算
    NVNbufferRange range = {0x4000, 64}; stream a = {0};
    p_bind_buf *b = put(&a, OP_BIND_UBO, sizeof *b); b->stage = 1 | BIND_BUF_DEFER; b->index = 3; b->addr = (uint64_t)(uintptr_t)&range;
    end(&a);
    range.address = 0x8000; range.size = 128;   // 录制之后、回放之前游戏改了
    nvn_be_exec(a.b);
    CHECK(rp_S.ubo[1][3].addr == 0x9000 && rp_S.ubo[1][3].size == 128 && rp_S.ubo[1][3].stage == 1);
    uint64_t sampler_handle = 0;
    p_bind_handles d = {0}; d.stage = 1; d.first = 11; d.n = 1; d.defer = 1; d.src = (uint64_t)(uintptr_t)&sampler_handle;
    p_bind_pool_bases bases = {0, 10}; memcpy(d.h + 1, &bases, sizeof bases);
    rp_state_op(OP_BIND_SAMP, &d, sizeof d);
    CHECK(rp_S.samp_bound[1][11] && rp_S.samp[1][11] == ((uint64_t)10 << 20)); // 句柄 0 属于录制时的池
}

static void t_program_stages(void) {
    prog_t pa, pb; memset(&pa, 0, sizeof pa); memset(&pb, 0, sizeof pb);
    pa.magic = pb.magic = MAGIC('P', 'R', 'O', 'G'); pa.serial = 11; pb.serial = 22;
    stream a = {0};
    p_bind_program_id *id = put(&a, OP_BIND_PROGRAM, sizeof *id); id->prog = &pa; id->serial = 11; id->stages = 0x3;
    id = put(&a, OP_BIND_PROGRAM, sizeof *id); id->prog = &pb; id->serial = 22; id->stages = 0x20;   // 计算阶段单独一份，不顶掉图形阶段
    end(&a);
    nvn_be_exec(a.b);
    CHECK(rp_S.bser[0] == 11 && rp_S.bser[1] == 11 && rp_S.bser[2] == 0 && rp_S.bst[0] == &pa);
    CHECK(rp_S.cser == 22 && rp_S.cprog == &pb);
    pa.serial = 99;   // 游戏复用了程序对象
    stream c = {0}; id = put(&c, OP_BIND_PROGRAM, sizeof *id); id->prog = &pb; id->serial = 22; id->stages = 0x20; end(&c);
    nvn_be_exec(c.b);
    CHECK(rp_S.bser[0] == 11 && rp_S.bst[0] == NULL);   // 仍按绑定当时的序号，不再读那个对象
}

static tex_t mk_tex(uint64_t serial, int64_t off, int fmt, uint32_t w, uint32_t h, uint32_t levels, int flags);   // 定义在本文件后面
static void res_fresh(void);   // 定义在本文件后面
// 视图取用窗口（#699）：一笔通道要同时挂多个附件，窗口内取到的缓存视图必须钉住到窗口结束；
// 窗口一结束它就该能按轮转淘汰（证明窗口只在窗口内起作用，不是永久驻留）。
static view_t vk_layer(int i) { view_t v; memset(&v, 0, sizeof v); v.set_layers = 1; v.min_layer = (uint16_t)i; v.nlayers = 1; return v; }   // 同一份存储上的第 i 种视图记录（替身不校验层号）
static void t_view_pin(void) {
    res_fresh();
    // 本用例要放掉成千上万个视图、又要查 first 有没有被放掉：「视图放掉」记录表（g_released_id，8192 条、整个程序不清）
    //   跑到这里已被前面的用例用掉一大半，记满后 first 被放掉也记不进去。同 t_view_storage_sweep：用例内从空表记起，结束时恢复
    const int saved_nrel = g_nrel; g_nrel = 0;
    // 视图表是 128 组 x 8 路（按存储、视图记录散列）：窗口里取 3000 种不同的（存储，视图记录），平均每组 23 个——first 那一组的 8 格必然被本轮钉满、
    // 多出来的走溢出表。窗口结束后 first 那一格要能被换掉（见下面的循环：插到它被放掉为止）。
    // 表按存储认视图：不同序号共用一份存储（同池同偏移同描述）时是同一项，所以这里在一份存储上取不同的视图记录来凑不同的项。
    enum { NWIN = 3000 };
    static view_t vp[NWIN];
    tex_t tp = mk_tex(3000, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&tp);
    for (int i = 0; i < NWIN; i++) vp[i] = vk_layer(i);
    nvn_gpu_view first = NULL;
    int first_id = 0;
    rp_view_pin_begin();
    first = rp_view_for(&tp, &vp[0], &g_fake_cmd);
    CHECK(first != NULL);
    if (first) first_id = first->rid;
    for (int i = 1; i < NWIN; i++) (void)rp_view_for(&tp, &vp[i], &g_fake_cmd);   // 每组都被挤满：钉住时不能放掉 first
    CHECK(first && !was_view_released(first_id));                                  // 修前：轮转把 first 放掉 → 红
    rp_view_pin_end();
    // 窗口结束后它可以被淘汰。淘汰哪一路取全局插入计数 % 8（rp_res.c rp_view_for），不是逐组轮转：first 那一组之后进几次插入、
    //   每次轮到哪一路随存储编号（全局累加）与散列而变，固定再插 3000 个时约 5% 的布局一次都轮不到 first（11-15 前面多一个用例就红）。
    //   所以一直插不同的视图记录，直到 first 被放掉；上限 60000 次（first 那组约进 470 次插入，轮不到的概率 (7/8)^470，可以不计）
    for (int i = NWIN; i < NWIN + 60000 && first && !was_view_released(first_id); i++) { view_t w = vk_layer(i); (void)rp_view_for(&tp, &w, &g_fake_cmd); }
    CHECK(first && was_view_released(first_id));                                   // 窗口结束：它可以被淘汰
    res_fresh();
    // 一帧里轮着用 300 个目标（真机一款 3D 游戏每帧一百多个通道）：第二遍取到的必须是第一遍那些视图，一个都不重建。
    // 300 个散进 128 组、每组 8 格；数量远小于 1024 格，但会不会有一组超过 8 个取决于散列，所以只要求「重建的不超过 300 个里的 5%」
    // （原先 64 格的环：第二遍 300 个全部重建）。
    { enum { M = 300 }; static tex_t tm[M]; static int rid[M]; int rebuilt = 0;
      for (int i = 0; i < M; i++) { tm[i] = mk_tex(9000 + (uint64_t)i * 7, (int64_t)i * 4096, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&tm[i]); }   // 各占一份存储（偏移错开）
      for (int i = 0; i < M; i++) { nvn_gpu_view v = rp_view_for(&tm[i], NULL, &g_fake_cmd); rid[i] = v ? v->rid : -1; }
      for (int i = 0; i < M; i++) { nvn_gpu_view v = rp_view_for(&tm[i], NULL, &g_fake_cmd); if (!v || v->rid != rid[i]) rebuilt++; }
      if (rebuilt > M / 20) fprintf(stderr, "视图表：300 个目标取第二遍，重建了 %d 个（期望 ≤ %d）\n", rebuilt, M / 20);
      CHECK(rebuilt <= M / 20); }
    res_fresh();   // 本用例建的存储不留给后面的用例（它们按 live 计数做断言）
    g_nrel = saved_nrel;
}


// 程序按阶段绑定：只绑程序实际有 shader 的阶段；VS/FS 不能被关（保留原绑定）；GS/TCS/TES 程序没有就解绑。
// 期望值直接由输入推出：绑 0x1f、程序只有 VS+FS → 有效掩码 3；分程序组合 → VS 保留、FS 换；
// 先绑带 GS 的程序再绑 VS+FS → GS 解绑；阶段认不出 → 旧规则（掩码照单全收）。
static void t_program_bind_stages(void) {
    prog_t pa, pb, pc;
    memset(&pa, 0, sizeof pa); memset(&pb, 0, sizeof pb); memset(&pc, 0, sizeof pc);
    pa.magic = pb.magic = pc.magic = MAGIC('P', 'R', 'O', 'G');
    pa.serial = 31; pa.nstage = 2;   // VS + FS
    pb.serial = 41; pb.nstage = 1;   // 只有 FS
    pc.serial = 51; pc.nstage = 3;   // VS + FS + GS
    static const uint8_t keyA[2][8] = {{1, 0, 0, 0, 0, 0, 0, 0}, {2, 0, 0, 0, 0, 0, 0, 0}};
    memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    g_snap_stage[0] = 0; g_snap_stage[1] = 1;
    memcpy(g_snap_keys, keyA, sizeof keyA); g_snap_ok = 0x3; g_snap_prog = &pa;
    // 1) VS+FS 程序用全部图形阶段掩码绑：只绑 0 / 1，不把 0x1f 记成有效掩码。
    stream a = {0};
    p_bind_program_id *id = put(&a, OP_BIND_PROGRAM, sizeof *id); id->prog = &pa; id->serial = 31; id->stages = 0x1f;
    end(&a); nvn_be_exec(a.b);
    CHECK(rp_S.bser[0] == 31 && rp_S.bser[1] == 31);
    CHECK(rp_S.bser[2] == 0 && rp_S.bser[3] == 0 && rp_S.bser[4] == 0);   // 修前：0x1f 全绑 → 这里红
    CHECK(rp_shader_graphics_stages() == 3u);
    CHECK(rp_shader_stage_key(0, NULL) == 1 && rp_shader_stage_key(1, NULL) == 1 && rp_shader_stage_key(2, NULL) == 0);
    // 2) 只有 FS 的程序用 0x1f 绑：顶点阶段保留上一次（不能被顶掉）。
    memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_stage[0] = 1;
    { uint8_t k[6][8]; memset(k, 0, sizeof k); memcpy(k[0], keyA[1], 8); memcpy(g_snap_keys, k, sizeof k); }
    g_snap_ok = 0x1; g_snap_prog = &pb;
    stream b = {0}; id = put(&b, OP_BIND_PROGRAM, sizeof *id); id->prog = &pb; id->serial = 41; id->stages = 0x1f;
    end(&b); nvn_be_exec(b.b);
    CHECK(rp_S.bser[0] == 31 && rp_S.bser[1] == 41);
    CHECK(rp_S.bser[2] == 0 && rp_S.bser[3] == 0 && rp_S.bser[4] == 0);
    CHECK(rp_shader_graphics_stages() == 3u);
    // 3) VS+FS+GS 程序绑上 GS。
    memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_stage[0] = 0; g_snap_stage[1] = 1; g_snap_stage[2] = 2;
    { uint8_t k[6][8]; memset(k, 0, sizeof k); memcpy(k[0], keyA[0], 8); memcpy(k[1], keyA[1], 8); k[2][0] = 3; memcpy(g_snap_keys, k, sizeof k); }
    g_snap_ok = 0x7; g_snap_prog = &pc;
    stream c = {0}; id = put(&c, OP_BIND_PROGRAM, sizeof *id); id->prog = &pc; id->serial = 51; id->stages = 0x1f;
    end(&c); nvn_be_exec(c.b);
    CHECK(rp_S.bser[0] == 51 && rp_S.bser[1] == 51 && rp_S.bser[2] == 51);
    CHECK(rp_shader_graphics_stages() == 7u);
    // 4) 再绑 VS+FS 程序：GS 必须被解绑（异度 3 的黑屏反例）。
    g_snap_prog = &pa; memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_stage[0] = 0; g_snap_stage[1] = 1;
    stream d = {0}; id = put(&d, OP_BIND_PROGRAM, sizeof *id); id->prog = &pa; id->serial = 31; id->stages = 0x1f;
    end(&d); nvn_be_exec(d.b);
    CHECK(rp_S.bser[2] == 0 && rp_shader_graphics_stages() == 3u);
    // 5) 显式解绑：空程序只覆盖 GS 位 → GS 解绑，其余不动（NVN 按阶段绑定）。
    g_snap_prog = &pc; memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_stage[0] = 0; g_snap_stage[1] = 1; g_snap_stage[2] = 2; g_snap_ok = 0x7;
    stream e2 = {0}; id = put(&e2, OP_BIND_PROGRAM, sizeof *id); id->prog = &pc; id->serial = 51; id->stages = 0x1f;
    end(&e2); nvn_be_exec(e2.b);
    CHECK(rp_S.bser[2] == 51);
    stream f = {0}; id = put(&f, OP_BIND_PROGRAM, sizeof *id); id->prog = NULL; id->serial = 0; id->stages = 0x4;
    end(&f); nvn_be_exec(f.b);
    CHECK(rp_S.bser[2] == 0 && rp_S.bser[0] == 51 && rp_S.bser[1] == 51);
    // 6) 只有 VS 的程序用 0x1f 绑：片元阶段保留上一次（FS 不能被顶掉）。
    g_snap_prog = &pc; memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_stage[0] = 0; g_snap_stage[1] = 1; g_snap_stage[2] = 2; g_snap_ok = 0x7;
    { stream r = {0}; id = put(&r, OP_BIND_PROGRAM, sizeof *id); id->prog = &pc; id->serial = 51; id->stages = 0x1f; end(&r); nvn_be_exec(r.b); }
    prog_t pd; memset(&pd, 0, sizeof pd); pd.magic = MAGIC('P', 'R', 'O', 'G'); pd.serial = 61; pd.nstage = 1;
    memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_stage[0] = 0;
    { uint8_t k[6][8]; memset(k, 0, sizeof k); k[0][0] = 4; memcpy(g_snap_keys, k, sizeof k); }
    g_snap_ok = 0x1; g_snap_prog = &pd;
    { stream s = {0}; id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pd; id->serial = 61; id->stages = 0x1f; end(&s); nvn_be_exec(s.b); }
    CHECK(rp_S.bser[0] == 61 && rp_S.bser[1] == 51);   // VS 换新，FS 保留
    CHECK(rp_shader_graphics_stages() == 3u);
    // 7) 显式解绑 FS：空程序只覆盖 FS 位 → FS 解绑，VS 不动。
    { stream v = {0}; id = put(&v, OP_BIND_PROGRAM, sizeof *id); id->prog = NULL; id->serial = 0; id->stages = 0x2; end(&v); nvn_be_exec(v.b); }
    CHECK(rp_S.bser[1] == 0 && rp_S.bser[0] == 61 && rp_shader_graphics_stages() == 1u);
    // 8) 阶段认不出（快照有项但 stage_of = 0xff）：旧规则，掩码照单全收，留给绘制时按名字报。
    memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_ok = 0x1; g_snap_prog = &pa;
    stream g = {0}; id = put(&g, OP_BIND_PROGRAM, sizeof *id); id->prog = &pa; id->serial = 31; id->stages = 0x1f;
    end(&g); nvn_be_exec(g.b);
    CHECK(rp_S.bser[2] == 31 && rp_S.bser[3] == 31 && rp_S.bser[4] == 31);
    g_snap_prog = NULL;
}

static void t_pools(void) {
    static uint8_t mem[0x1000]; uint64_t b = (uint64_t)(uintptr_t)mem; void *buf = NULL; uint64_t off = 0;
    int n0 = rp_pool_count();
    nvn_be_pool_add(mem, sizeof mem);
    CHECK(rp_pool_count() == n0 + 1);
    CHECK(nvn_be_addr(b + 0x800, 0x100, &buf, &off) == 1 && buf == (void *)mem && off == 0x800);
    CHECK(nvn_be_addr(b + 0xf80, 0x100, &buf, &off) == 0);    // 越过池尾
    CHECK(nvn_be_addr(b + 0xf00, 0x100, &buf, &off) == 1);    // 恰好到池尾
    nvn_be_pool_del(mem);
    CHECK(nvn_be_addr(b + 0x800, 0x100, &buf, &off) == 0 && rp_pool_count() == n0);
}

// ---------- 池查找的线程内备忘（rp_entry.c pool_find_memo，#974 问 3）----------
// 契约：答案与「候选 = 起点不超过地址的最后一个池、装得下才算」逐位一致（rp_pool_find 与 nvn_be_addr 两个入口都对）；
// 重复的查找真的命中备忘；池表一变（加池 / 删池 / 同起点换大小）旧记录整张作废；开关关着时不命中也不计数。
// 参考答案由用例自己登记的池直接线性扫出，不经被测实现。池区间允许嵌套：一个池的存储可能落在另一个池的区间里，
// 命中的是「起点不超过地址、且真正覆盖 [addr, addr+len) 的池里起点最大的那个」（更具体的池优先）。
int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c（rp_draw.c 也是就地声明）
typedef struct { uint64_t a, n; int live; } pm_ref_t;
enum { PM_SLOTS = 24 };
static pm_ref_t g_pmr[PM_SLOTS]; static uint8_t g_pm_arena[0x10000];
static int pm_ref_find(uint64_t addr, uint64_t len, uint64_t *a, uint64_t *n) {
    int cand = -1;
    for (int i = 0; i < PM_SLOTS; i++) {
        if (!g_pmr[i].live || g_pmr[i].a > addr) continue;
        if (addr - g_pmr[i].a > g_pmr[i].n || len > g_pmr[i].n - (addr - g_pmr[i].a)) continue;
        if (cand < 0 || g_pmr[i].a > g_pmr[cand].a) cand = i;   // 嵌套时更具体的池优先
    }
    if (cand < 0) return 0;
    *a = g_pmr[cand].a; *n = g_pmr[cand].n; return 1;
}
static void pm_add(int slot, uint64_t off, uint64_t size) { nvn_be_pool_add(g_pm_arena + off, size); g_pmr[slot] = (pm_ref_t){(uint64_t)(uintptr_t)(g_pm_arena + off), size, 1}; }
static void pm_del(int slot) { nvn_be_pool_del((const void *)(uintptr_t)g_pmr[slot].a); g_pmr[slot].live = 0; }
static uint64_t pm_hits(void) { uint64_t h, l; rp_pool_memo_stats(&h, &l); return h; }
static uint64_t pm_lookups(void) { uint64_t h, l; rp_pool_memo_stats(&h, &l); return l; }
static void pm_flag(int on) { g_flag_pool_memo = on; for (int i = 0; i < 4096; i++) rp_pool_find(1, 1, NULL, NULL); }   // 备忘每 4096 次查找重读一次开关
static int pm_check(uint64_t addr, uint64_t len) {   // 一次查找（两个入口各一次）与参考答案逐位对照：1 = 一致
    uint64_t ra = 0, rn = 0; int rh = pm_ref_find(addr, len, &ra, &rn);
    const uint8_t *m = NULL; uint64_t n = 0; int h = rp_pool_find(addr, len, &m, &n);
    if (h != rh || (h && ((uint64_t)(uintptr_t)m != ra || n != rn))) return 0;
    void *buf = NULL; uint64_t off = 0; int h2 = nvn_be_addr(addr, len, &buf, &off);
    return h2 == rh && (!h2 || ((uint64_t)(uintptr_t)buf == ra && off == addr - ra));
}
static uint32_t pm_rnd_state = 777;
static uint32_t pm_rnd(void) { pm_rnd_state = pm_rnd_state * 1664525u + 1013904223u; return pm_rnd_state >> 8; }
static void t_pool_memo(void) {
    const uint64_t B = (uint64_t)(uintptr_t)g_pm_arena; int n0 = rp_pool_count();
    memset(g_pmr, 0, sizeof g_pmr);
    pm_flag(1);
    // ⓪ 排满备忘（20 个池各问一次，环只有 16 项，别的用例留下的记录被全部挤掉），再把池整体删掉：旧记录一项都不能再用
    enum { NP = 20 };
    for (int i = 0; i < NP; i++) pm_add(i, 0x8000 + (uint64_t)i * 0x200, 0x100);
    for (int i = 0; i < NP; i++) CHECK(pm_check(B + 0x8000 + (uint64_t)i * 0x200 + 8, 0x10) && rp_pool_find(B + 0x8000 + (uint64_t)i * 0x200 + 8, 0x10, NULL, NULL) == 1);
    for (int i = 0; i < NP; i++) pm_del(i);
    for (int i = 0; i < NP; i++) CHECK(pm_check(B + 0x8000 + (uint64_t)i * 0x200 + 0x10, 8) && rp_pool_find(B + 0x8000 + (uint64_t)i * 0x200 + 0x10, 8, NULL, NULL) == 0);
    pm_add(0, 0x0000, 0x1000); pm_add(1, 0x2000, 0x1000); pm_add(2, 0x4000, 0x2000);
    // ① 命中：第一次走锁内二分并记下，同池另一处再问就命中备忘（两个入口各一次）
    CHECK(pm_check(B + 0x10, 0x20));
    uint64_t h0 = pm_hits();
    CHECK(pm_check(B + 0x800, 0x100));
    CHECK(pm_hits() == h0 + 2);
    CHECK(pm_check(B + 0x2100, 0x40));                       // 另一个池：第一问走锁内二分并记下
    uint64_t h1 = pm_hits(); CHECK(pm_check(B + 0x2200, 0x40) && pm_hits() == h1 + 2);
    // ② 装得下才算：越过池尾、池间空隙、恰好到池尾
    CHECK(pm_check(B + 0xf80, 0x100));                       // 越过 P0 尾：没有
    CHECK(pm_check(B + 0x1800, 0x10));                       // P0 与 P1 之间的空隙：候选是 P0、装不下：没有
    CHECK(pm_check(B + 0xf00, 0x100));                       // 恰好到 P0 尾：有
    CHECK(pm_check(B + 0x10, 0));                            // 零长度
    // ③ 删池之后旧记录作废：删掉再问是「没有」
    CHECK(pm_check(B + 0x4100, 0x40) && pm_check(B + 0x4200, 0x40));
    pm_del(2);
    CHECK(pm_check(B + 0x4100, 0x40) && pm_check(B + 0x4200, 0x40) && pm_check(B + 0x5ff0, 0x10));
    uint64_t ra = 0, rn = 0; CHECK(!pm_ref_find(B + 0x4100, 0x40, &ra, &rn) && !rp_pool_find(B + 0x4100, 0x40, NULL, NULL));   // 确实没有（参考与实现各自说）
    // ④ 同起点换大小：先 8 KB 后缩成 4 KB，后半必须变成「没有」，不能拿旧长度
    pm_add(2, 0x4000, 0x2000);
    CHECK(pm_check(B + 0x5800, 0x100) && pm_check(B + 0x5800, 0x100));
    CHECK(rp_pool_find(B + 0x5800, 0x100, NULL, NULL) == 1);
    pm_del(2); pm_add(2, 0x4000, 0x1000);
    CHECK(pm_check(B + 0x5800, 0x100) && rp_pool_find(B + 0x5800, 0x100, NULL, NULL) == 0);
    CHECK(pm_check(B + 0x4800, 0x100) && rp_pool_find(B + 0x4800, 0x100, NULL, NULL) == 1);
    // ⑤ 加池：新池的地址马上查得到（旧的「没有」不残留）
    CHECK(pm_check(B + 0x8100, 0x40) && rp_pool_find(B + 0x8100, 0x40, NULL, NULL) == 0);
    pm_add(3, 0x8000, 0x800);
    CHECK(pm_check(B + 0x8100, 0x40) && rp_pool_find(B + 0x8100, 0x40, NULL, NULL) == 1);
    // ⑥ 开关关：不命中、不计数，答案照旧
    pm_flag(0);
    uint64_t hz = pm_hits(), lz = pm_lookups();
    for (int i = 0; i < 64; i++) CHECK(pm_check(B + 0x10 + (uint64_t)i * 0x20, 0x20) && pm_check(B + 0x2100 + (uint64_t)i, 0x40));
    CHECK(pm_hits() == hz && pm_lookups() == lz);
    pm_flag(1);
    // ⑦ 随机：加池 / 删池 / 同起点换大小，每步 16 问（一半是刚问过的键）；先开着、再关着各跑一遍，答案都与参考逐位一致
    for (int i = 0; i < PM_SLOTS; i++) if (g_pmr[i].live) pm_del(i);
    for (int pass = 1; pass >= 0; pass--) {
        pm_flag(pass);
        uint64_t hs = pm_hits(); unsigned bad = 0, ops = 0; uint64_t ka[4] = {0}, kl[4] = {0};
        for (int step = 0; step < 3000; step++) {
            int i = (int)(pm_rnd() % PM_SLOTS); unsigned op = pm_rnd() % 8;
            if (op < 4 && !g_pmr[i].live) {                  // 加：起点是 0x400 的倍数，大小 0x100..0x1800，与现存的池不重叠
                uint64_t off = (pm_rnd() % 60) * 0x400, size = 0x100 + (pm_rnd() % 0x18) * 0x100; int ok = off + size <= sizeof g_pm_arena;
                for (int k = 0; k < PM_SLOTS && ok; k++) if (g_pmr[k].live) { uint64_t o = g_pmr[k].a - B; if (off < o + g_pmr[k].n && o < off + size) ok = 0; }
                if (ok) { pm_add(i, off, size); ops++; }
            } else if (op < 7 && g_pmr[i].live) { pm_del(i); ops++; }
            for (int q = 0; q < 16; q++) {
                uint64_t a, l;
                if (q & 1) { a = ka[(q >> 1) & 3]; l = kl[(q >> 1) & 3]; }
                else { a = B + pm_rnd() % sizeof g_pm_arena; l = pm_rnd() % 9 ? pm_rnd() % 0x1200 : 0; ka[(q >> 1) & 3] = a; kl[(q >> 1) & 3] = l; }
                if (!pm_check(a, l)) bad++;
            }
        }
        CHECK(bad == 0 && ops > 800);                        // 答案与参考不一致 0 次；表真的变了足够多次
        if (pass) CHECK(pm_hits() > hs + 1000);              // 开着：确实在命中
        else CHECK(pm_hits() == hs);                         // 关着：一次也不命中
    }
    for (int i = 0; i < PM_SLOTS; i++) if (g_pmr[i].live) pm_del(i);
    pm_flag(1);
    CHECK(rp_pool_count() == n0);
}

// ---------- 嵌套池：一个池的存储落在另一个池的区间里（#1345） ----------
// 契约：命中的是「起点不超过地址、且真正覆盖 [addr, addr+len) 的池里起点最大的那个」。
// 旧实现只取「起点不超过地址的最后一个池」，候选装不下就返回 0——大池会被区间内的小池遮蔽。
static uint8_t g_pn_arena[1 << 20] __attribute__((aligned(16)));
static void t_pool_nested(void) {
    const uint64_t B = (uint64_t)(uintptr_t)g_pn_arena;
    const uint64_t BIG = 1u << 20, SMALL = 0x2000, SMALL_OFF = 0x10000;
    const int n0 = rp_pool_count();
    const uint8_t *m = NULL; uint64_t n = 0; void *buf = NULL; uint64_t off = 0;
    // ① 先加 1 MiB 大池，再在其 +0x10000 处加 8 KB 小池
    nvn_be_pool_add(g_pn_arena, BIG);
    nvn_be_pool_add(g_pn_arena + SMALL_OFF, SMALL);
    CHECK(rp_pool_count() == n0 + 2);
    // ② 小池内地址 → 小池
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x10, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B + SMALL_OFF && n == SMALL);
    buf = NULL; off = 0;
    CHECK(nvn_be_addr(B + SMALL_OFF + 0x10, 0x20, &buf, &off) == 1 && (uint64_t)(uintptr_t)buf == B + SMALL_OFF && off == 0x10);
    // ③ 大池内、小池之外（小池之后的地址，旧实现会被小池遮蔽返回 0）→ 大池
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x4000, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    // ④ 起点在小池内、长度跨过小池尾但没出大池 → 大池
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x1ff0, 0x100, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    // ⑤ 先查大池内地址（让快路/备忘发布大池），再查小池内地址 → 仍必须是小池
    //    备忘先关掉：备忘里若已经有小池记录，会先于快路答出，就测不到快路的上界（lim）了
    pm_flag(0);
    CHECK(rp_pool_find(B + SMALL_OFF + 0x4000, 0x20, NULL, NULL) == 1);
    CHECK(rp_pool_find(B + SMALL_OFF + 0x20, 0x40, NULL, NULL) == 1);
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x20, 0x40, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B + SMALL_OFF && n == SMALL);
    pm_flag(1);
    // ⑥ 同一小池内地址连查 3 次（覆盖备忘与快路）：每次都要与锁内结论一致
    for (int i = 0; i < 3; i++) {
        m = NULL; n = 0;
        CHECK(rp_pool_find(B + SMALL_OFF + 0x30, 0x10, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B + SMALL_OFF && n == SMALL);
    }
    // ⑦ 删掉小池：小池内地址回落到大池
    nvn_be_pool_del(g_pn_arena + SMALL_OFF);
    CHECK(rp_pool_count() == n0 + 1);
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x10, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    // ⑧ 两个入口（rp_pool_find / nvn_be_addr）同一地址答案一致
    {
        static const uint64_t qo[5] = { 0x10, SMALL_OFF + 0x10, SMALL_OFF + 0x4000, BIG - 0x10, BIG };
        static const uint64_t ql[5] = { 0x20, 0x20, 0x40, 0x20, 0x10 };
        for (int i = 0; i < 5; i++) {
            const uint8_t *mm = NULL; uint64_t nn = 0;
            void *bb = NULL; uint64_t oo = 0;
            const int h1 = rp_pool_find(B + qo[i], ql[i], &mm, &nn);
            const int h2 = nvn_be_addr(B + qo[i], ql[i], &bb, &oo);
            CHECK(h1 == h2);
            if (h1 && h2) CHECK(mm == (const uint8_t *)bb && oo == B + qo[i] - (uint64_t)(uintptr_t)mm && nn == BIG);
        }
    }
    // ⑨ 快路：小池放回来，关掉备忘，查大池内、小池之后的地址两次——第二次必须走快路
    nvn_be_pool_add(g_pn_arena + SMALL_OFF, SMALL);
    CHECK(rp_pool_count() == n0 + 2);
    pm_flag(0);
    uint64_t fh0 = rp_pool_fast_hits();
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x4000, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    CHECK(rp_pool_fast_hits() == fh0);
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x4000, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    CHECK(rp_pool_fast_hits() == fh0 + 1);
    // ⑩ 备忘：开着时同地址连查 3 次，备忘命中数要增加（可答区间没被小池切断）
    pm_flag(1);
    uint64_t hz0 = pm_hits();
    // 先查一次小池内地址：把快路/备忘指向小池，随后的大池地址装不下，只能回落锁内并写入大池的记录
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + SMALL_OFF + 0x20, 0x40, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B + SMALL_OFF && n == SMALL);
    for (int i = 0; i < 3; i++) {
        m = NULL; n = 0;
        CHECK(rp_pool_find(B + SMALL_OFF + 0x4100, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    }
    CHECK(pm_hits() >= hz0 + 2);   // 后两次从备忘答出
    // ⑪ 左端：[大池起点, 小池起点) 的地址连查两次，第二次走快路
    pm_flag(0);
    uint64_t fh1 = rp_pool_fast_hits();
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + 0x800, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    CHECK(rp_pool_fast_hits() == fh1);
    m = NULL; n = 0;
    CHECK(rp_pool_find(B + 0x800, 0x20, &m, &n) == 1 && (uint64_t)(uintptr_t)m == B && n == BIG);
    CHECK(rp_pool_fast_hits() == fh1 + 1);
    pm_flag(1);
    nvn_be_pool_del(g_pn_arena + SMALL_OFF);
    CHECK(rp_pool_count() == n0 + 1);
    nvn_be_pool_del(g_pn_arena);
    CHECK(rp_pool_count() == n0);
}

static pool_t g_tp; static uint8_t g_tmem[4 << 20];   // #936 的 128x64x64 RGBA8 三维贴图要 2 MiB，池子得放得下（原来是 1 MiB，读到池外段错误）
static tex_t mk_tex(uint64_t serial, int64_t off, int fmt, uint32_t w, uint32_t h, uint32_t levels, int flags) {
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 'T'); t.serial = serial; t.pool = &g_tp; t.off = off; t.mem = g_tmem + off;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = fmt; t.d.width = w; t.d.height = h; t.d.depth = 1; t.d.levels = levels; t.d.flags = flags;
    nvn_tex_layout(&t.d, &t.L); return t;
}
static void res_fresh(void) { rp_res_reset(); memset(g_tmem, 0, sizeof g_tmem); memset(&g_tp, 0, sizeof g_tp); g_tp.mem = g_tmem; g_tp.size = sizeof g_tmem; g_tp.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    g_nup = 0; g_dirty_written = 0; }

// 线性单级二维 CPU 贴图只重传脏行（upload_linear_dirty）：一行 4096 字节 = 一页。第一次整张传；第 10、11、40 页脏 → 两段（y 10 共 2 行、y 40 共 1 行），
//   内容取新写的；脏页过半 → 整张；位图不可用（-1）→ 整张。
static void t_tex_linear_dirty_rows(void) {
    nvn_be_sub_begin();
    res_fresh();
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    memset(g_wp_dirty, 0, sizeof g_wp_dirty); g_wp_on = 0;
    tex_t t = mk_tex(7101, 0, NVN_FORMAT_R8, 4096, 64, 1, NVN_TEXTURE_FLAGS_LINEAR);
    t.mem = g_block_mem; g_tp.size = sizeof g_tmem;
    CHECK(t.L.size == 4096u * 64u);
    for (uint32_t y = 0; y < 64; y++) memset(g_block_mem + (size_t)y * 4096, (int)(y + 1), 4096);
    nvn_be_tex_init(&t);
    uint64_t first0, repeat0, first, repeat;
    nvn_be_pkg_stats(&first0, &repeat0);
    uint64_t u0 = rp_res_uploads();
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() - u0 == 1 && g_nup == 1 && g_up[0].y == 0 && g_up[0].h == 64);   // 第一次：整张
    g_wp_on = 1; g_wp_dirty[10] = g_wp_dirty[11] = g_wp_dirty[40] = 1;
    memset(g_block_mem + 10 * 4096, 0xA1, 2 * 4096); memset(g_block_mem + 40 * 4096, 0xB2, 4096);
    g_nup = 0; g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    g_block_gen[10] = g_block_gen[11] = g_block_gen[40] = g_dirty_written;   // 真写同时推进被写页的代号（脏页日志按它出事件）
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() - u0 == 2);
    nvn_be_pkg_stats(&first, &repeat);
    CHECK(first - first0 == 1 && repeat - repeat0 == 1);   // 两个脏行区间是同一次重传
    CHECK(g_nup == 2);
    CHECK(g_nup >= 1 && g_up[0].y == 10 && g_up[0].h == 2 && g_up[0].w == 4096 && g_up[0].pitch == 4096 && g_up[0].first[0] == 0xA1);
    CHECK(g_nup >= 2 && g_up[1].y == 40 && g_up[1].h == 1 && g_up[1].first[0] == 0xB2);
    memset(g_wp_dirty, 0, sizeof g_wp_dirty); for (int k = 0; k < 40; k++) g_wp_dirty[k] = 1;   // 脏 40 行（过半）→ 整张
    g_nup = 0; g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    for (int k = 0; k < 40; k++) g_block_gen[k] = g_dirty_written;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == 1 && g_up[0].y == 0 && g_up[0].h == 64);
    g_wp_on = 0; memset(g_wp_dirty, 0, sizeof g_wp_dirty);   // 位图不可用 → 整张
    g_nup = 0; g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    g_block_gen[5] = g_dirty_written;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == 1 && g_up[0].h == 64);
    nvn_be_tex_release(&t);
    g_block_mem = NULL;
    nvn_be_sub_commit();
}

// 结论 A：贴图初始化和登记都不建图；只有被用到的那一个建。
static void t_tex_init_registers_only(void) {
    nvn_be_sub_begin();
    res_fresh();
    enum { N = 11400 }; static tex_t t[N]; int n0 = rp_tex_live_n(), c0 = g_tex_creates;
    for (int i = 0; i < N; i++) { t[i] = mk_tex(1000 + (uint64_t)i, 0, NVN_FORMAT_RGBA8, 64u, 64u, 1, 0); nvn_be_tex_init(&t[i]); nvn_be_reg_tex(i, &t[i], NULL); }
    CHECK(rp_tex_live_n() == n0 + N);
    CHECK(g_tex_creates == c0 && rp_res_tex_built() == 0);          // 11400 个对象：0 张图
    CHECK(rp_view_by_id(7, &g_fake_cmd, NULL) != NULL);
    CHECK(g_tex_creates == c0 + 1 && rp_res_tex_built() == 1);      // 用到 1 个：1 张图
    for (int i = 0; i < 400; i++) nvn_be_tex_release(&t[i]);
    CHECK(rp_tex_live_n() == n0 + N - 400);
    tex_t z; memset(&z, 0, sizeof z); nvn_be_tex_init(&z);   // 序号 0：不登记
    CHECK(rp_tex_live_n() == n0 + N - 400);
    for (int i = 400; i < N; i++) nvn_be_tex_release(&t[i]);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// 共用存储被新贴图占了就出表（照 iOS alias_take_over）。期望的个数由「谁还被对象指着、谁闲置够了 16 次提交」数出来：
// 同一偏移上先后换 40 种宽高（带动态分辨率的游戏每帧换一种），旧宽高的对象都已 Finalize：表里不能越攒越多。
static void subs_pass(int n) { for (int i = 0; i < n; i++) { nvn_be_sub_begin(); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); } }
static void t_alias_take_over(void) {
    res_fresh(); int c0 = g_tex_creates, r0 = g_tex_releases;
    // ① 每换一种宽高之间隔 16 次提交：上一种闲置够了、又没有对象指着 → 被新的挤掉，表里始终只有 1 份
    for (int i = 0; i < 40; i++) {
        nvn_be_sub_begin();
        tex_t t = mk_tex(100 + (uint64_t)i, 0, NVN_FORMAT_RGBA8, 64 + i, 32, 1, 0);
        CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
        nvn_be_tex_release(&t);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        if (rp_res_tex_built() != 1) { CHECK(rp_res_tex_built() == 1); break; }
        subs_pass(16);
    }
    CHECK(g_tex_creates - c0 == 40 && g_tex_releases - r0 == 39 && rp_res_tex_built() == 1);
    nvn_be_alias_drop_pool(&g_tp); res_fresh(); c0 = g_tex_creates; r0 = g_tex_releases;
    // ② 闲置窗口：刚用过的不挤。同一次提交里先后两种宽高（对象都 Finalize 了）→ 两份都在；隔 16 次提交再来第三种 → 前两份都放
    nvn_be_sub_begin();
    tex_t a = mk_tex(200, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0), b = mk_tex(201, 0, NVN_FORMAT_RGBA8, 48, 48, 1, 0);
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) && rp_tex_resolve(&b, &g_fake_cmd));
    nvn_be_tex_release(&a); nvn_be_tex_release(&b);
    CHECK(rp_res_tex_built() == 2 && g_tex_releases == r0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    subs_pass(16);
    nvn_be_sub_begin();
    tex_t c = mk_tex(202, 0, NVN_FORMAT_RGBA8, 40, 40, 1, 0);
    CHECK(rp_tex_resolve(&c, &g_fake_cmd) != NULL);
    CHECK(rp_res_tex_built() == 1 && g_tex_releases - r0 == 2);
    // ③ 每帧 Finalize 再按同描述重建的目标：隔多少次提交都还是那一份（靠同描述查到，不算被占）
    nvn_gpu_tex gc = rp_tex_resolve(&c, &g_fake_cmd);
    nvn_be_tex_release(&c);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    subs_pass(40);
    nvn_be_sub_begin();
    tex_t c2 = mk_tex(203, 0, NVN_FORMAT_RGBA8, 40, 40, 1, 0);
    CHECK(rp_tex_resolve(&c2, &g_fake_cmd) == gc && rp_res_tex_built() == 1);
    // ④ 还有对象指着的不挤：c2 没 Finalize，闲置再久，同偏移来了别的宽高也留着
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    subs_pass(40);
    nvn_be_sub_begin();
    int r1 = g_tex_releases;
    tex_t d = mk_tex(204, 0, NVN_FORMAT_RGBA8, 36, 36, 1, 0);
    CHECK(rp_tex_resolve(&d, &g_fake_cmd) != NULL && rp_res_tex_built() == 2 && g_tex_releases == r1);
    // ⑤ 存储不重叠的不挤：别的偏移上闲置的、没对象指着的那份留着
    tex_t far = mk_tex(205, 0x40000, NVN_FORMAT_RGBA8, 32, 32, 1, 0);
    CHECK(rp_tex_resolve(&far, &g_fake_cmd) != NULL);
    nvn_be_tex_release(&far);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    subs_pass(40);
    nvn_be_sub_begin();
    r1 = g_tex_releases;
    nvn_be_tex_release(&d);
    tex_t e = mk_tex(206, 0x80000, NVN_FORMAT_RGBA8, 32, 32, 1, 0);
    CHECK(rp_tex_resolve(&e, &g_fake_cmd) != NULL && g_tex_releases == r1 && rp_res_tex_built() == 4);   // c2、d、far、e
    nvn_be_tex_release(&e); nvn_be_tex_release(&c2);
    nvn_be_alias_drop_pool(&g_tp);
    CHECK(rp_res_tex_built() == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // ⑥ 对象还活着、但存储被别的贴图写覆盖了（动态分辨率：一批并存的对象建在同一偏移上，各一个宽高，游戏不 Finalize 它们）：
    //    p 写过后闲置；q 建在同一偏移上、之后一直在被写；再来 r 时：p 闲置满 16 次提交、且 q 在 p 最后一次写之后写过 → p 放；q 刚写过 → 留
    res_fresh(); c0 = g_tex_creates; r0 = g_tex_releases;
    nvn_be_sub_begin();
    tex_t p = mk_tex(300, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0);
    nvn_gpu_tex gp = rp_tex_resolve(&p, &g_fake_cmd); CHECK(gp != NULL);
    rp_tex_mark_gpu_written(&p);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    subs_pass(20);
    nvn_be_sub_begin();
    tex_t q = mk_tex(301, 0, NVN_FORMAT_RGBA8, 60, 60, 1, 0);
    CHECK(rp_tex_resolve(&q, &g_fake_cmd) != NULL);
    CHECK(rp_res_tex_built() == 2 && g_tex_releases == r0);          // q 还没写：p 没被盖，留着
    rp_tex_mark_gpu_written(&q);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    subs_pass(20);
    nvn_be_sub_begin();
    rp_tex_mark_gpu_written(&q);                                      // q 一直在用
    tex_t r = mk_tex(302, 0, NVN_FORMAT_RGBA8, 56, 56, 1, 0);
    CHECK(rp_tex_resolve(&r, &g_fake_cmd) != NULL);
    CHECK(rp_res_tex_built() == 2 && g_tex_releases - r0 == 1);      // p 放了（q、r 在）
    CHECK(rp_res_tex_serials() == 2);                                 // p 的序号也出表了
    nvn_gpu_tex gp2 = rp_tex_resolve(&p, &g_fake_cmd);                // p 的对象还活着：再用就重建
    CHECK(gp2 != NULL && g_tex_creates - c0 == 4 && rp_res_tex_serials() == 3);
    nvn_be_tex_release(&p); nvn_be_tex_release(&q); nvn_be_tex_release(&r);
    nvn_be_alias_drop_pool(&g_tp);
    CHECK(rp_res_tex_built() == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// 视图表按存储认视图（rp_res.c g_vf）：游戏每帧把瞬态渲染目标 Finalize 再按同描述重建——新序号、同一份存储（共用表）。视图只由（存储，视图记录）决定，
// 所以新序号取到的必须是上一个序号建的那个视图，一个都不重建（后端按视图记的帧缓冲也就跟着沿用）；换视图记录 / 换存储才是新项；存储放掉，名下的视图一并放。
// 期望值都由用例自己建的输入直接得出：创建 / 放掉次数来自替身计数，视图身份来自替身给的 rid。
// 两臂：arm 1 = 缺省（按存储）；arm 0 = nvk_vf_key=0（旧：按序号）——同样的操作每个新序号都重建，是这条改动的负对照（两臂的期望写在同一处，一臂变红即说明开关没起作用或改动没生效）。
static void vf_push(int on) {   // 开关每 256 次取视图重读一次：换值后用一张探针贴图推一下
    g_flag_vf_key = on;
    tex_t pr = mk_tex(83999, 0x7F000, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&pr);
    for (int i = 0; i < 300; i++) (void)rp_view_for(&pr, NULL, &g_fake_cmd);
    nvn_be_tex_release(&pr);
}
static void t_view_storage_key(void) {
    for (int arm = 1; arm >= 0; arm--) {
        res_fresh();
        const int c0 = g_view_creates, r0 = g_view_releases;   // 整臂的账：结束时创建数 = 放掉数（不漏、不多放）
        vf_push(arm);
        const int ca = g_view_creates;
        tex_t a = mk_tex(84001, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&a);
        nvn_gpu_view va = rp_view_for(&a, NULL, &g_fake_cmd);
        CHECK(va != NULL && g_view_creates == ca + 1);
        const int rid_a = va ? va->rid : -1;
        // ① 序号销毁、同描述的新序号接上来（瞬态渲染目标每帧 Finalize 再重建）：同一份存储
        nvn_be_tex_release(&a);
        tex_t b = mk_tex(84002, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&b);
        nvn_gpu_view vb = rp_view_for(&b, NULL, &g_fake_cmd);
        CHECK(vb != NULL);
        if (arm) CHECK(vb && vb->rid == rid_a && g_view_creates == ca + 1 && !was_view_released(rid_a));   // 视图沿用：没重建、没放
        else     CHECK(vb && vb->rid != rid_a && g_view_creates == ca + 2 && was_view_released(rid_a));   // 负对照：按序号认，旧序号一没就放、新序号重建
        // ② 两个活着的序号共用一份存储（同池同偏移同描述）：视图共用
        const int cb = g_view_creates;
        tex_t d = mk_tex(84003, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&d);
        nvn_gpu_view vd = rp_view_for(&d, NULL, &g_fake_cmd);
        if (arm) CHECK(vd && vb && vd->rid == vb->rid && g_view_creates == cb);
        else     CHECK(vd && vb && vd->rid != vb->rid && g_view_creates == cb + 1);
        // ③ 同一份存储上换视图记录：新项；再取：命中
        view_t l1 = vk_layer(1);
        const int cc = g_view_creates;
        nvn_gpu_view v1 = rp_view_for(&b, &l1, &g_fake_cmd);
        CHECK(v1 && vb && v1->rid != vb->rid && g_view_creates == cc + 1);
        CHECK(rp_view_for(&b, &l1, &g_fake_cmd) == v1 && g_view_creates == cc + 1);
        // ④ 另一份存储（偏移不同）：新项
        tex_t e = mk_tex(84004, 4096, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&e);
        const int cd = g_view_creates;
        nvn_gpu_view ve = rp_view_for(&e, NULL, &g_fake_cmd);
        CHECK(ve && vb && ve->rid != vb->rid && g_view_creates == cd + 1);
        // ⑤ 存储放掉（内存池 Finalize）：名下的视图一并放，一个不留
        nvn_be_tex_release(&b); nvn_be_tex_release(&d); nvn_be_tex_release(&e); nvn_be_alias_drop_pool(&g_tp);
        CHECK(g_view_creates - c0 == g_view_releases - r0);
        // ⑥ 同地址的新存储（calloc 常把刚放掉的块再给出来）：取到新视图，不是死存储的旧视图
        tex_t f = mk_tex(84005, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&f);
        const int ce = g_view_creates;
        nvn_gpu_view vf = rp_view_for(&f, NULL, &g_fake_cmd);
        CHECK(vf && vf->rid != rid_a && g_view_creates == ce + 1);
        nvn_be_tex_release(&f); nvn_be_alias_drop_pool(&g_tp);
        CHECK(g_view_creates - c0 == g_view_releases - r0);
        // ⑦ 不进共用表的存储（没有池）：序号销毁就是存储销毁，视图当场放（tex_unref 路径）
        tex_t p = mk_tex(84006, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); p.pool = NULL; p.mem = NULL; nvn_be_tex_init(&p);
        const int cf = g_view_creates, rf = g_view_releases;
        nvn_gpu_view vp0 = rp_view_for(&p, NULL, &g_fake_cmd);
        CHECK(vp0 != NULL && g_view_creates == cf + 1);
        const int rid_p = vp0 ? vp0->rid : -1;
        nvn_be_tex_release(&p);
        CHECK(g_view_releases == rf + 1 && was_view_released(rid_p));
        CHECK(g_view_creates - c0 == g_view_releases - r0);
        // ⑧ 同偏移来了别的描述、旧存储闲置够久被挤掉（alias_take_over，序号早已销毁）：名下的视图一并放
        nvn_be_sub_begin();
        tex_t h = mk_tex(84007, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&h);
        nvn_gpu_view vh = rp_view_for(&h, NULL, &g_fake_cmd);
        CHECK(vh != NULL);
        const int rid_h = vh ? vh->rid : -1;
        nvn_be_tex_release(&h);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        subs_pass(16);
        nvn_be_sub_begin();
        tex_t h2 = mk_tex(84008, 0, NVN_FORMAT_RGBA8, 40, 40, 1, 0); nvn_be_tex_init(&h2);
        CHECK(rp_tex_resolve(&h2, &g_fake_cmd) != NULL);
        CHECK(was_view_released(rid_h));
        nvn_be_tex_release(&h2); nvn_be_alias_drop_pool(&g_tp);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_view_creates - c0 == g_view_releases - r0);
    }
    vf_push(1);   // 开关回缺省，并让备忘重读
    res_fresh();
}

// 视图记录的每个字节都在键里（键是（存储 uid，视图记录整份 16 字节 + 有没有记录））：同一份存储上，把记录逐字节各改一次、再加一个「没带记录」，每个变体各是一个独立的视图；
// 换序号后，按存储认的一臂每个变体沿用自己的视图、一个不错认；按序号认的一臂（负对照）每个变体都重建。身份用替身给的 rid 比（放掉的视图地址会被复用，不能比指针）。
static void t_view_storage_fields(void) {
    enum { NV = 18 };   // 0 = 基础记录；1..16 = 逐字节改一个字节；17 = 不带记录（NULL）
    for (int arm = 1; arm >= 0; arm--) {
        res_fresh();
        const int c0 = g_view_creates, r0 = g_view_releases;   // 整臂的账（含 vf_push 的探针视图）
        vf_push(arm);
        const int c1 = g_view_creates;                         // 本用例自己的创建数从这里数
        view_t var[NV]; var[0] = vk_layer(2); for (int k = 1; k < 17; k++) { var[k] = var[0]; ((uint8_t *)&var[k])[k - 1] ^= 0x5A; }
        memset(&var[17], 0, sizeof var[17]);
        tex_t a = mk_tex(86500, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&a);
        int rid[NV];
        for (int k = 0; k < NV; k++) {
            nvn_gpu_view w = rp_view_for(&a, k == 17 ? NULL : &var[k], &g_fake_cmd);
            CHECK(w != NULL); rid[k] = w ? w->rid : -1;
            for (int m = 0; m < k; m++) CHECK(rid[m] != rid[k]);                                     // 改任何一个字节都不是同一个视图
        }
        CHECK(g_view_creates - c1 == NV);
        nvn_be_tex_release(&a);                                                                         // 序号销毁；同描述的新序号接上来
        tex_t b = mk_tex(86501, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&b);
        for (int k = NV - 1; k >= 0; k--) {
            nvn_gpu_view w = rp_view_for(&b, k == 17 ? NULL : &var[k], &g_fake_cmd);
            CHECK(w != NULL);
            if (arm) CHECK(w && w->rid == rid[k]); else CHECK(w && w->rid != rid[k]);                 // 新：各沿用各的；旧：全部重建
        }
        CHECK(g_view_creates - c1 == (arm ? NV : 2 * NV));
        nvn_be_tex_release(&b); nvn_be_alias_drop_pool(&g_tp);
        CHECK(g_view_creates - c0 == g_view_releases - r0);
    }
    vf_push(1);
    res_fresh();
}

// 视图记录的每个字节都在键里，且不靠散列碰巧分开：上面逐字节的用例里各变体散列到不同的组，比较函数漏比一个字节时，被漏的那个变体多半碰不上同组的兄弟，
// 照样全绿。这里把同一个字节的 256 个取值逐个取一遍（一个字节 256 个变体挤进 128 组，同组争位的有几百对），再取一次「不带记录」：取回的视图自己记着建它时
// 收到的记录，必须与这次要的逐字节相同、有没有记录也相同——键里漏了哪个字节（或漏比有没有记录），总会有一对同组变体取回别人的视图。
static void t_view_storage_sweep(void) {
    const int saved_nrel = g_nrel;   // 本用例要放掉几千个视图：别把后面用例用的「视图放掉」记录挤满
    for (int arm = 1; arm >= 0; arm--) {
        int reqs = 0, bad = 0;
        for (int k = 0; k < (int)sizeof(view_t); k++) {
            res_fresh();
            const int c0 = g_view_creates, r0 = g_view_releases;   // 这一个字节的账（含 vf_push 的探针视图）
            vf_push(arm);
            const int c1 = g_view_creates;
            tex_t a = mk_tex(87000 + k, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&a);
            const view_t base = vk_layer(2);
            for (int val = 0; val < 256; val++) {
                view_t r = base; ((uint8_t *)&r)[k] ^= (uint8_t)val;   // val = 0 → 基础记录
                nvn_gpu_view w = rp_view_for(&a, &r, &g_fake_cmd);
                reqs++; if (!w || !w->has_v || memcmp(&w->v, &r, sizeof r)) bad++;
            }
            { nvn_gpu_view w = rp_view_for(&a, NULL, &g_fake_cmd); reqs++; if (!w || w->has_v) bad++; }
            { view_t r = base; nvn_gpu_view w = rp_view_for(&a, &r, &g_fake_cmd); reqs++; if (!w || !w->has_v || memcmp(&w->v, &r, sizeof r)) bad++; }   // 不带记录之后再取带记录的
            CHECK(g_view_creates - c1 >= 256);                         // 256 个不同的记录各建过自己的视图（用例没有空转）
            nvn_be_tex_release(&a); nvn_be_alias_drop_pool(&g_tp);
            CHECK(g_view_creates - c0 == g_view_releases - r0);        // 建了几个、放了几个：不漏、不多放
        }
        CHECK(reqs == 16 * 258);
        CHECK(bad == 0);   // 取回的视图没有一个是为别的记录建的
    }
    vf_push(1);
    res_fresh();
    g_nrel = saved_nrel;
}

// 同一存储换序号后，读回的像素与旧路径逐字节相同，且等于「该画的内容」。60 帧、每帧一个新序号（Finalize 再按同描述重建）、每帧在 6 种视图记录上各画一次（带 / 不带记录、
// 不同层、不同 mip、不同格式、不同通道映射；6 个视图各写自己的那个像素字，互不覆盖）；中间经历：存储不变（0–14）→ 内存池 Finalize、存储换成新对象（15–29）→ 同池另一偏移（30–44）
// → 回到前一份（45–59）。每帧画完读回这一帧用的那份存储的全部像素（16 个字），两臂（缺省按存储 / nvk_vf_key=0 按序号）各跑一遍：两臂每帧读回逐字节相同，
// 且每帧读回恰好等于「6 个视图各写进自己那个字的值」（没有画丢、没有画进别的存储）；同时核两臂确实走了不同机制：新路径只建 18 个视图（存储 / 偏移每换一次 6 个），旧路径每帧 6 个共 360 个。
static void t_view_storage_pixels(void) {
    static uint32_t snap[2][60][16]; int creates[2] = {0, 0};
    // 6 个视图各自写的那个像素字（mip，层）：0 → [0][3]；1 → [0][1]；2 → [0][2]；3 → [1][0]；4 → [2][1]；5（不带记录）→ [0][0]
    static const int cell[6] = {0 * 4 + 3, 0 * 4 + 1, 0 * 4 + 2, 1 * 4 + 0, 2 * 4 + 1, 0 * 4 + 0};
    for (int arm = 1; arm >= 0; arm--) {
        res_fresh();
        const int c0 = g_view_creates, r0 = g_view_releases;   // 整臂的账（含 vf_push 的探针视图）
        vf_push(arm);
        const int c1 = g_view_creates;                         // 本用例自己的创建数从这里数
        for (int f = 0; f < 60; f++) {
            const int64_t off = f >= 30 && f < 45 ? 4096 : 0;
            tex_t t = mk_tex(86600 + f, off, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&t);
            nvn_gpu_tex st = NULL; uint32_t want_px[16]; memset(want_px, 0, sizeof want_px);
            for (int j = 0; j < 6; j++) {
                view_t vj = vk_layer(j == 0 ? 3 : j % 3);
                if (j == 3) vj.base_level = 1;
                if (j == 4) { vj.base_level = 2; vj.format = 7; }
                if (j == 2) vj.swz[1] = 3;
                nvn_gpu_view w = rp_view_for_attach(&t, j == 5 ? NULL : &vj, &g_fake_cmd);
                CHECK(w != NULL);
                const uint32_t val = (uint32_t)(f * 8 + j + 1);
                struct nvn_gpu_view_s want = {0}; want.has_v = j != 5; if (j != 5) want.v = vj;
                want_px[cell[j]] = val ^ view_sig(&want);
                if (w) { fake_paint(w, val); st = w->of; }
            }
            CHECK(st != NULL);
            if (st) { memcpy(snap[arm][f], st->px, sizeof snap[arm][f]); CHECK(!memcmp(snap[arm][f], want_px, sizeof want_px)); }   // 读回 = 该画的内容
            nvn_be_tex_release(&t);
            if (f == 14) nvn_be_alias_drop_pool(&g_tp);   // 内存池 Finalize：存储死亡，下一帧起是新的存储对象
        }
        creates[arm] = g_view_creates - c1;
        nvn_be_alias_drop_pool(&g_tp);
        CHECK(g_view_creates - c0 == g_view_releases - r0);
    }
    CHECK(!memcmp(snap[1], snap[0], sizeof snap[0]));                 // 两臂每帧读回的像素逐字节相同
    CHECK(creates[1] == 18 && creates[0] == 360);                     // 且两臂走的确实是两条路（新路径少建 95%）
    CHECK(memcmp(snap[1][0], snap[1][1], sizeof snap[1][0]) != 0);    // 画面确实在变（每帧写进去的值不同）
    vf_push(1);
    res_fresh();
}

static void t_alias(void) {   // 同池同偏移同描述的对象共用一份存储；只差用途标志也算同一份；偏移不同是两份
    nvn_be_sub_begin();
    res_fresh(); int c0 = g_tex_creates, r0 = g_tex_releases;
    tex_t a = mk_tex(1, 0, NVN_FORMAT_RGBA8, 32, 32, 1, 0), b = mk_tex(2, 0, NVN_FORMAT_RGBA8, 32, 32, 1, NVN_TEXTURE_FLAGS_IMAGE), c = mk_tex(3, 0x4000, NVN_FORMAT_RGBA8, 32, 32, 1, 0);
    nvn_gpu_tex ga = rp_tex_resolve(&a, &g_fake_cmd), gb = rp_tex_resolve(&b, &g_fake_cmd), gc = rp_tex_resolve(&c, &g_fake_cmd);
    CHECK(ga && ga == gb && gc && gc != ga);
    CHECK(g_tex_creates - c0 == 2 && rp_res_tex_built() == 2 && rp_res_tex_serials() == 3);
    CHECK(rp_res_uploads() == 0);                                   // 存储全 0：不是 CPU 内容，不当 CPU 贴图传
    { static const uint8_t zero[64]; CHECK(g_nup == 2 && g_up[0].img == 32 * 32 * 4 && g_up[1].img == 32 * 32 * 4 && !memcmp(g_up[0].first, zero, 64) && !memcmp(g_up[1].first, zero, 64)); }   // 新建的两份存储各清零一次（共用的那份只清一次）
    CHECK(g_last_usage & NVN_GPU_TEX_RENDER);                       // 能当附件的格式建图时就带附件用途
    tex_t sw = mk_tex(4, 0x8000, NVN_FORMAT_RGBA8, 32, 32, 1, 0); sw.set_swz = 1; sw.swz = 2 | 3 << 3 | 4 << 6 | 5 << 9;
    CHECK(rp_tex_resolve(&sw, &g_fake_cmd) != NULL && g_last_swz == 0);   // 游戏设了恒等通道映射：按「没设」交给后端
    sw.serial = 6; sw.off = 0xC000; sw.mem = g_tmem + 0xC000; sw.swz = 5 | 4 << 3 | 3 << 6 | 2 << 9;
    CHECK(rp_tex_resolve(&sw, &g_fake_cmd) != NULL && g_last_swz == (5 | 4 << 3 | 3 << 6 | 2 << 9));   // 非恒等：原样交
    nvn_be_tex_release(&sw); sw.serial = 4; nvn_be_tex_release(&sw);   // 两个对象都 Finalize；存储仍由共用表持有
    c0 += 2;
    nvn_be_tex_release(&a); nvn_be_tex_release(&b);
    CHECK(g_tex_releases == r0);                                    // 共用表持有：对象 Finalize 不放存储
    tex_t a2 = mk_tex(9, 0, NVN_FORMAT_RGBA8, 32, 32, 1, 0);
    CHECK(rp_tex_resolve(&a2, &g_fake_cmd) == ga && g_tex_creates - c0 == 2);   // 每帧在新对象上重建同一张：还是那份
    nvn_be_tex_release(&a2); nvn_be_tex_release(&c);
    nvn_be_alias_drop_pool(&g_tp);
    CHECK(g_tex_releases - r0 == 4 && rp_res_tex_built() == 0);     // 池销毁：出表并放掉（a/b 共用的一份、c、两张通道映射的）
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- 同步读贴图（ReadTexels）：内容在游戏内存里的那一条 ----------
// 像素函数由用例定：(种子, x, y, 通道) → 字节。先用 nvn_swizzle 摆进游戏内存，再经 nvn_be_read_texels 读回，
// 期望值就是摆进去的线性数据里对应的字节，不经被测实现算。dst 先填哨兵，读后核对没写的地方仍是哨兵。
static uint8_t px_fn(unsigned seed, unsigned x, unsigned y, unsigned c) { return (uint8_t)(seed + x * 7 + y * 31 + c * 3); }
static void px_fill(uint8_t *lin, unsigned seed, unsigned w, unsigned h, unsigned bpp) {   // 紧排：行距 w * bpp
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) for (unsigned c = 0; c < bpp; c++) lin[(y * w + x) * bpp + c] = px_fn(seed, x, y, c);
}
enum { SENT = 0xEE };
static void sent_fill(uint8_t *p, size_t n) { memset(p, SENT, n); }
static int all_sent(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) if (p[i] != SENT) return 0; return 1; }
static uint64_t un_cnt(const char *name) { return nvn_unimpl_count("be", name); }

static void t_read_texels_guest(void) {
    res_fresh(); nvn_unimpl_reset(); g_ft_hit = 0;
    uint8_t dst[512];
    // 8×4 RGBA8，随包（has_layout）。整块读：行距紧挨
    tex_t a = mk_tex(41, 0, NVN_FORMAT_RGBA8, 8, 4, 1, 0); a.has_layout = 1;
    uint8_t la[8 * 4 * 4]; px_fill(la, 1, 8, 4, 4);
    CHECK(nvn_swizzle(&a.d, 0, 0, la, 32, a.mem) == 0 && memcmp(a.mem, la, 32) != 0);   // 摆进去了，且存储不是线性的（否则「不反交织」查不出来）
    NVNcopyRegion all = {.width = 8, .height = 4, .depth = 1};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &all, dst, 32, 128);
    CHECK(!memcmp(dst, la, 128) && all_sent(dst + 128, sizeof dst - 128));
    CHECK(un_cnt("read_texels") == 0);                                                   // 走游戏内存的不记账
    // 子区域 + 带 pad 的行距：x 2..4、y 1..2，每行 12 字节内容 + 4 字节 pad，pad 保持哨兵
    NVNcopyRegion sub = {.xoffset = 2, .yoffset = 1, .width = 3, .height = 2, .depth = 1};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &sub, dst, 16, 32);
    for (unsigned y = 0; y < 2; y++) CHECK(!memcmp(dst + y * 16, la + (1 + y) * 32 + 2 * 4, 12) && all_sent(dst + y * 16 + 12, 4));
    CHECK(all_sent(dst + 32, sizeof dst - 32));
    // 两层数组的第 1 层：起始层参数 1；区域 zoffset 1 也是层偏移；两层一次读，片距 80（每片 64 + 16 pad）
    tex_t b = mk_tex(42, 0x10000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); b.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; b.d.depth = 2; nvn_tex_layout(&b.d, &b.L); b.has_layout = 1;
    uint8_t lb0[64], lb1[64]; px_fill(lb0, 10, 4, 4, 4); px_fill(lb1, 90, 4, 4, 4);
    CHECK(b.L.layers == 2 && nvn_swizzle(&b.d, 0, 0, lb0, 16, b.mem) == 0 && nvn_swizzle(&b.d, 0, 1, lb1, 16, b.mem) == 0);
    NVNcopyRegion one = {.width = 4, .height = 4, .depth = 1}, one_z1 = {.zoffset = 1, .width = 4, .height = 4, .depth = 1}, two = {.width = 4, .height = 4, .depth = 2};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&b, 0, 1, &one, dst, 16, 64);
    CHECK(!memcmp(dst, lb1, 64) && all_sent(dst + 64, sizeof dst - 64));
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&b, 0, 0, &one_z1, dst, 16, 64);
    CHECK(!memcmp(dst, lb1, 64));
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&b, 0, 0, &two, dst, 16, 80);
    CHECK(!memcmp(dst, lb0, 64) && all_sent(dst + 64, 16) && !memcmp(dst + 80, lb1, 64) && all_sent(dst + 144, sizeof dst - 144));
    // 第 1 级（4×4）的子区域 x 1..3、y 1..2；第 0 级整读不受影响
    tex_t c = mk_tex(43, 0x20000, NVN_FORMAT_RGBA8, 8, 8, 2, 0); c.has_layout = 1;
    uint8_t lc0[8 * 8 * 4], lc1[4 * 4 * 4]; px_fill(lc0, 3, 8, 8, 4); px_fill(lc1, 150, 4, 4, 4);
    CHECK(nvn_swizzle(&c.d, 0, 0, lc0, 32, c.mem) == 0 && nvn_swizzle(&c.d, 1, 0, lc1, 16, c.mem) == 0);
    NVNcopyRegion l1 = {.xoffset = 1, .yoffset = 1, .width = 3, .height = 2, .depth = 1}, l0 = {.width = 8, .height = 8, .depth = 1};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&c, 1, 0, &l1, dst, 12, 24);
    for (unsigned y = 0; y < 2; y++) CHECK(!memcmp(dst + y * 12, lc1 + (1 + y) * 16 + 1 * 4, 12));
    CHECK(all_sent(dst + 24, sizeof dst - 24));
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&c, 0, 0, &l0, dst, 32, 256);
    CHECK(!memcmp(dst, lc0, 256));
    // BC1（4×4 块、每块 8 字节）16×8 = 4×2 个块：区域 x 4 宽 8 = 块列 1..2，y 0 / 4 = 块行 0 / 1；x 要按块宽折算
    tex_t d = mk_tex(44, 0x30000, NVN_FORMAT_RGBA_DXT1, 16, 8, 1, 0); d.has_layout = 1;
    uint8_t ld[64]; for (unsigned i = 0; i < sizeof ld; i++) ld[i] = (uint8_t)(0x20 + i);   // 块 (列, 行) 在 行 * 32 + 列 * 8
    CHECK(nvn_swizzle(&d.d, 0, 0, ld, 32, d.mem) == 0);
    NVNcopyRegion bc0 = {.xoffset = 4, .width = 8, .height = 4, .depth = 1}, bc1 = {.xoffset = 4, .yoffset = 4, .width = 8, .height = 4, .depth = 1};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&d, 0, 0, &bc0, dst, 16, 16);
    CHECK(!memcmp(dst, ld + 8, 16) && all_sent(dst + 16, sizeof dst - 16));
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&d, 0, 0, &bc1, dst, 16, 16);
    CHECK(!memcmp(dst, ld + 32 + 8, 16) && all_sent(dst + 16, sizeof dst - 16));
    // 三维 4×4×4：第 1、2 片，片距 80（每片 64 + 16 pad）
    tex_t e = mk_tex(45, 0x40000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); e.d.target = NVN_TEXTURE_TARGET_3D; e.d.depth = 4; nvn_tex_layout(&e.d, &e.L); e.has_layout = 1;
    uint8_t vol[4 * 64]; for (unsigned k = 0; k < 4; k++) px_fill(vol + k * 64, 20 * k + 5, 4, 4, 4);   // 片 k 在 k * 64
    CHECK(nvn_swizzle(&e.d, 0, 0, vol, 16, e.mem) == 0);
    NVNcopyRegion s12 = {.zoffset = 1, .width = 4, .height = 4, .depth = 2};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&e, 0, 0, &s12, dst, 16, 80);
    CHECK(!memcmp(dst, vol + 64, 64) && all_sent(dst + 64, 16) && !memcmp(dst + 80, vol + 128, 64) && all_sent(dst + 144, sizeof dst - 144));
    // 一维数组：height 是层数（每层一行 8 个）。读第 1 层；高 2 的区域越界（每层只有一行）
    tex_t f = mk_tex(46, 0x50000, NVN_FORMAT_RGBA8, 8, 2, 1, 0); f.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&f.d, &f.L); f.has_layout = 1;
    uint8_t lf0[32], lf1[32]; px_fill(lf0, 7, 8, 1, 4); px_fill(lf1, 77, 8, 1, 4);
    CHECK(f.L.layers == 2 && nvn_swizzle(&f.d, 0, 0, lf0, 32, f.mem) == 0 && nvn_swizzle(&f.d, 0, 1, lf1, 32, f.mem) == 0);
    NVNcopyRegion row1 = {.width = 8, .height = 1, .depth = 1}, tall = {.width = 8, .height = 2, .depth = 1};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&f, 0, 1, &row1, dst, 32, 32);
    CHECK(!memcmp(dst, lf1, 32) && all_sent(dst + 32, sizeof dst - 32));
    uint64_t br = un_cnt("read_texels_bad_region");
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&f, 0, 0, &tall, dst, 32, 64);
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels_bad_region") == br + 1);

    // 内容在不在游戏内存里：随包 / CPU 写过 / 脏页追踪命中，各自单独为真都读；三条都假不读（dst 不动、记账 +1）
    tex_t g = mk_tex(47, 0x60000, NVN_FORMAT_RGBA8, 4, 4, 1, 0), h = mk_tex(48, 0x61000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    CHECK(nvn_swizzle(&g.d, 0, 0, lb0, 16, g.mem) == 0 && nvn_swizzle(&h.d, 0, 0, lb0, 16, h.mem) == 0);
    uint64_t un0 = un_cnt("read_texels");
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&g, 0, 0, &one, dst, 16, 64);
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels") == un0 + 1);                // 三条都假
    nvn_be_tex_cpu_written(&g);                                                          // ② WriteTexels / 刷新写过
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&g, 0, 0, &one, dst, 16, 64);
    CHECK(!memcmp(dst, lb0, 64) && un_cnt("read_texels") == un0 + 1);
    g_ft_hit = 1;                                                                        // ③ 脏页追踪发现注册后被 CPU 写过（h 没有 ① ②）
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&h, 0, 0, &one, dst, 16, 64);
    CHECK(!memcmp(dst, lb0, 64) && un_cnt("read_texels") == un0 + 1);
    g_ft_hit = 0;

    // 存储不是块线性的（线性 / 多重采样）：反交织管不了，不动 dst、记 be read_texels
    tex_t m = mk_tex(49, 0x62000, NVN_FORMAT_RGBA8, 4, 4, 1, NVN_TEXTURE_FLAGS_LINEAR); m.has_layout = 1;
    tex_t s = mk_tex(50, 0x63000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); s.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; s.d.samples = 4; nvn_tex_layout(&s.d, &s.L); s.has_layout = 1;
    un0 = un_cnt("read_texels");
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&m, 0, 0, &one, dst, 16, 64); nvn_be_read_texels(&s, 0, 0, &one, dst, 16, 64);
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels") == un0 + 2);
    // 格式不认识：不猜布局，不动 dst
    tex_t u = mk_tex(51, 0x64000, 0, 4, 4, 1, 0); u.has_layout = 1;
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&u, 0, 0, &one, dst, 16, 64);
    CHECK(all_sent(dst, sizeof dst) && nvn_unimpl_count("fmt", "read_texels_unknown_format") == 1);

    // 请求不成立：级 / 层 / 片 / 区域越界、零面积、行距 / 片距容不下内容，都拒绝：不动 dst、按名字记账
    br = un_cnt("read_texels_bad_region"); uint64_t bs = un_cnt("read_texels_bad_stride");
    NVNcopyRegion past_x = {.xoffset = 6, .width = 4, .height = 4, .depth = 1}, past_z = {.zoffset = 3, .width = 4, .height = 4, .depth = 2}, empty = {.width = 0, .height = 4, .depth = 1};
    NVNcopyRegion huge = {.width = 0x7fffffff, .height = 0x7fffffff, .depth = 1};        // 宽高给到 INT_MAX：块数换算不能回绕 / 有符号溢出，当越界拒绝
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &past_x, dst, 16, 64);      // x 6..9 越过 8 宽
    nvn_be_read_texels(&a, 1, 0, &all, dst, 32, 128);                                    // a 只有 1 级
    nvn_be_read_texels(&b, 0, 2, &one, dst, 16, 64);                                     // b 只有 2 层，起始层 2
    nvn_be_read_texels(&e, 0, 0, &past_z, dst, 16, 80);                                  // 三维 4 片，片 3..4
    nvn_be_read_texels(&a, 0, 0, &empty, dst, 16, 64);                                   // 宽 0
    nvn_be_read_texels(&a, 0, 0, &huge, dst, 16, 64);
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels_bad_region") == br + 6);
    nvn_be_read_texels(&a, 0, 0, &sub, dst, 8, 32);                                      // 一行 12 字节内容，行距 8：行会互相覆盖
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels_bad_stride") == bs + 1);
    nvn_be_read_texels(&b, 0, 0, &two, dst, 16, 48);                                     // 一片占 64，片距 48：片会互相覆盖
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels_bad_stride") == bs + 2 && un_cnt("read_texels_bad_region") == br + 6);
    nvn_be_read_texels(NULL, 0, 0, &one, dst, 16, 64); nvn_be_read_texels(&a, 0, 0, NULL, dst, 16, 64); nvn_be_read_texels(&a, 0, 0, &one, NULL, 16, 64);
    CHECK(un_cnt("read_texels_bad_args") == 3);
}

// 已被 GPU 写过的贴图：即使随包（① 为真）、脏页追踪也说被写过，也不读游戏内存里更旧的字节；游戏之后又 WriteTexels，内容回到游戏内存
static void t_read_texels_gpu_owned(void) {
    nvn_be_sub_begin();
    res_fresh(); nvn_unimpl_reset(); g_ft_hit = 0;
    uint8_t dst[128], lin[64]; px_fill(lin, 33, 4, 4, 4);
    tex_t k = mk_tex(52, 0, NVN_FORMAT_RGBA8, 4, 4, 1, 0); k.has_layout = 1;
    CHECK(nvn_swizzle(&k.d, 0, 0, lin, 16, k.mem) == 0);
    NVNcopyRegion one = {.width = 4, .height = 4, .depth = 1};
    CHECK(rp_tex_resolve(&k, &g_fake_cmd) != NULL);
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&k, 0, 0, &one, dst, 16, 64);         // 建好、没被 GPU 写：随包，读游戏内存
    CHECK(!memcmp(dst, lin, 64) && un_cnt("read_texels") == 0);
    rp_tex_mark_gpu_written(&k);
    CHECK(nvn_be_tex_gpu_owned(52) == 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);   // 读回在提交之外（旁路段要拿提交锁）
    // GPU 写过：不读游戏内存里更旧的字节，读的是 GPU 的内容（替身 copy_tex_buf 写的 gpu_byte）
    uint8_t gpu[64]; for (unsigned y = 0; y < 4; y++) for (unsigned i = 0; i < 16; i++) gpu[y * 16 + i] = gpu_byte(0, 0, 0, 0, y, i);
    const int cpu_auto = g_buf_cpu_auto; g_buf_cpu_auto = 1; g_auto_done = 1; g_tb_fill = 1;
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&k, 0, 0, &one, dst, 16, 64);
    CHECK(!memcmp(dst, gpu, 64) && memcmp(dst, lin, 64) && un_cnt("read_texels") == 0);
    g_ft_hit = 1; sent_fill(dst, sizeof dst); nvn_be_read_texels(&k, 0, 0, &one, dst, 16, 64); g_ft_hit = 0;
    CHECK(!memcmp(dst, gpu, 64) && un_cnt("read_texels") == 0 && nvn_be_tex_gpu_owned(52) == 1);   // 脏页追踪的答案不能把归属翻回去
    g_buf_cpu_auto = cpu_auto; g_auto_done = 0; g_tb_fill = 0;
    nvn_be_tex_cpu_written(&k);                                                          // 游戏又 WriteTexels 了
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&k, 0, 0, &one, dst, 16, 64);
    CHECK(nvn_be_tex_gpu_owned(52) == 0 && !memcmp(dst, lin, 64) && un_cnt("read_texels") == 0);
    nvn_be_tex_release(&k);
}

// ReadTexels 读 GPU 内容（rp_entry.c rt_gpu）。期望值都来自替身对「GPU 内容」的定义（gpu_byte）与用例给的区域，不跑被测实现：
//   一次读回 = 拿提交锁 → 等之前的活 → 一段旁路段（开 1 交 1，提交值 = 上一个 + 64，之后的提交接着递增）→ 每层 / 片一次 copy_tex_buf → 按行距 / 片距写 dst；
//   读不了的（格式没有 READBACK、块没对齐、在提交里、之前的活 / 读回等不到）：一段都不开（或开了也交掉）、不动 dst、具名记账，提交锁一定放回。
static tex_t mk_tex3d(uint64_t serial, int fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels);   // 定义在本文件后面
static tex_t mk_tex_tgt(uint64_t serial, int target, int fmt, uint32_t w, uint32_t h, uint32_t depth);
static void rt_gpu_tex(tex_t *t) { nvn_be_sub_begin(); CHECK(rp_tex_resolve(t, &g_fake_cmd) != NULL); rp_tex_mark_gpu_written(t); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); }
static int rt_rows_are(const uint8_t *dst, size_t row, uint32_t layer, uint32_t level, const NVNcopyRegion *r, uint64_t wb, uint64_t hb) {   // 每行 wb 字节是 GPU 内容
    for (uint64_t y = 0; y < hb; y++) for (uint64_t i = 0; i < wb; i++) if (dst[y * row + i] != gpu_byte(layer, level, r->yoffset, r->xoffset, y, i)) return 0;
    return 1;
}
static void t_read_texels_gpu(void) {
    res_fresh(); nvn_unimpl_reset(); g_ft_hit = 0;
    const int cpu_auto = g_buf_cpu_auto; g_buf_cpu_auto = 1; g_auto_done = 1; g_tb_fill = 1;
    uint8_t dst[1024];
    uint64_t st0[4]; nvn_be_read_texels_gpu_stats(st0);
    // ① 8×4 RGBA8 整块：一段旁路段、一次拷贝（层 0、宽 8 高 4、行距 32、一片 128）、提交值 = 上一个 + 64
    tex_t a = mk_tex(61, 0, NVN_FORMAT_RGBA8, 8, 4, 1, 0); rt_gpu_tex(&a);
    const uint64_t f0 = nvn_be_last_fence(); const int b0 = g_begins, c0 = g_commits, l0 = g_rt_locks; g_ntb = 0;
    NVNcopyRegion all = {.width = 8, .height = 4, .depth = 1};
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &all, dst, 32, 128);
    CHECK(g_begins == b0 + 1 && g_commits == c0 + 1 && g_last_signal == f0 + 64 && nvn_be_last_fence() == f0 + 64);
    CHECK(g_ntb == 1 && g_tb[0].to_tex == 0 && g_tb[0].layer == 0 && g_tb[0].level == 0 && g_tb[0].w == 8 && g_tb[0].h == 4 && g_tb[0].row == 32 && g_tb[0].img == 128);
    CHECK(rt_rows_are(dst, 32, 0, 0, &all, 32, 4) && all_sent(dst + 128, sizeof dst - 128));
    CHECK(g_rt_locks == l0 + 1 && g_rt_unlocks == g_rt_locks && un_cnt("read_texels") == 0);
    { uint64_t st[4]; nvn_be_read_texels_gpu_stats(st); CHECK(st[0] == st0[0] + 1); }
    // 之后的正常提交：提交值接着往后取（不与旁路段撞）
    nvn_be_sub_begin(); nvn_be_sub_commit(); CHECK(g_last_signal == f0 + 128); fake_complete(NVN_GPU_DONE_OK);
    // ② 子区域 + 带 pad 的行距：x 2..4、y 1..2，每行 12 字节内容 + 4 字节 pad
    NVNcopyRegion sub = {.xoffset = 2, .yoffset = 1, .width = 3, .height = 2, .depth = 1}; g_ntb = 0;
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &sub, dst, 16, 32);
    CHECK(g_ntb == 1 && g_tb[0].w == 3 && g_tb[0].h == 2 && g_tb[0].row == 12);
    CHECK(rt_rows_are(dst, 16, 0, 0, &sub, 12, 2) && all_sent(dst + 12, 4) && all_sent(dst + 28, sizeof dst - 28));
    // ③ 二维数组 4×4 共 3 层：起始层 1、深度 2 → 拷第 1、2 层；片距 80（64 + 16 pad）
    tex_t b = mk_tex_tgt(62, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 4, 4, 3); rt_gpu_tex(&b);
    NVNcopyRegion two = {.width = 4, .height = 4, .depth = 2}; g_ntb = 0;
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&b, 0, 1, &two, dst, 16, 80);
    CHECK(g_ntb == 2 && g_tb[0].layer == 1 && g_tb[1].layer == 2);
    CHECK(rt_rows_are(dst, 16, 1, 0, &two, 16, 4) && all_sent(dst + 64, 16) && rt_rows_are(dst + 80, 16, 2, 0, &two, 16, 4) && all_sent(dst + 144, sizeof dst - 144));
    // ④ 三维 4×4×4：第 1、2 片（copy_tex_buf 的 layer 在三维上是片号）
    tex_t e = mk_tex3d(63, NVN_FORMAT_RGBA8, 4, 4, 4, 1); rt_gpu_tex(&e);
    NVNcopyRegion s12 = {.zoffset = 1, .width = 4, .height = 4, .depth = 2}; g_ntb = 0;
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&e, 0, 0, &s12, dst, 16, 64);
    CHECK(g_ntb == 2 && g_tb[0].layer == 1 && g_tb[1].layer == 2 && rt_rows_are(dst, 16, 1, 0, &s12, 16, 4) && rt_rows_are(dst + 64, 16, 2, 0, &s12, 16, 4));
    // ⑤ BC1 16×8：块列 1..2、块行 0..1 → 一行 16 字节（2 块 × 8）、2 块行
    tex_t d = mk_tex(64, 0x30000, NVN_FORMAT_RGBA_DXT1, 16, 8, 1, 0); rt_gpu_tex(&d);
    NVNcopyRegion bc = {.xoffset = 4, .width = 8, .height = 8, .depth = 1}; g_ntb = 0;
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&d, 0, 0, &bc, dst, 16, 32);
    CHECK(g_ntb == 1 && g_tb[0].row == 16 && g_tb[0].img == 32 && rt_rows_are(dst, 16, 0, 0, &bc, 16, 2) && all_sent(dst + 32, sizeof dst - 32));
    // ⑥ 读不了的：一段都不开、不动 dst、具名记账、锁平衡
    {   // 格式没有 READBACK（后端会拒 / 拷不出 NVN 字节）：开段前挡住
        const uint32_t saved = g_caps_readback[0]; g_caps_readback[0] = 0;
        const int bb = g_begins, cc = g_commits, ll = g_rt_locks;
        sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &all, dst, 32, 128);
        CHECK(all_sent(dst, sizeof dst) && g_begins == bb && g_commits == cc && g_rt_locks == ll && un_cnt("read_texels_gpu_format") == 1);
        g_caps_readback[0] = saved;
    }
    {   // 块没对齐（BC1 x 从 2 起）
        NVNcopyRegion un = {.xoffset = 2, .width = 4, .height = 4, .depth = 1}; const int bb = g_begins;
        sent_fill(dst, sizeof dst); nvn_be_read_texels(&d, 0, 0, &un, dst, 16, 16);
        CHECK(all_sent(dst, sizeof dst) && g_begins == bb && un_cnt("read_texels_gpu_unaligned") == 1);
    }
    {   // 在一次提交的回放里（持着提交锁的那条路上）：旁路段开不出来，锁放回
        nvn_be_sub_begin(); const int bb = g_begins;
        sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &all, dst, 32, 128);
        CHECK(all_sent(dst, sizeof dst) && g_begins == bb && un_cnt("side_segment_inside_submit") == 1 && g_rt_unlocks == g_rt_locks);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    }
    {   // 之前交的活没做完、等不到：不开段
        nvn_be_sub_begin(); nvn_be_sub_commit(); g_auto_done = 0; const int bb = g_begins;
        sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &all, dst, 32, 128);
        CHECK(all_sent(dst, sizeof dst) && g_begins == bb && un_cnt("read_texels_gpu_wait") == 1 && g_rt_unlocks == g_rt_locks);
        fake_complete(NVN_GPU_DONE_OK);
        // 读回那一段交了、等不到：不动 dst；时间线照样往后走，下一次提交的值更大
        const uint64_t fx = nvn_be_last_fence();
        sent_fill(dst, sizeof dst); nvn_be_read_texels(&a, 0, 0, &all, dst, 32, 128);
        CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels_gpu_wait") == 2 && nvn_be_last_fence() == fx + 64 && g_rt_unlocks == g_rt_locks);
        fake_complete(NVN_GPU_DONE_OK); g_auto_done = 1;
        nvn_be_sub_begin(); nvn_be_sub_commit(); CHECK(g_last_signal == fx + 128); fake_complete(NVN_GPU_DONE_OK);
    }
    tex_t one = mk_tex(65, 0x40000, NVN_FORMAT_RGBA8, 8, 0, 2, 0);
    one.d.target = NVN_TEXTURE_TARGET_1D;
    nvn_tex_layout(&one.d, &one.L);
    rt_gpu_tex(&one);
    tex_t array = mk_tex(66, 0x50000, NVN_FORMAT_RGBA8, 8, 4, 2, 0);
    array.d.target = NVN_TEXTURE_TARGET_1D_ARRAY;
    nvn_tex_layout(&array.d, &array.L);
    rt_gpu_tex(&array);
    NVNcopyRegion row = {.xoffset = 1, .width = 3, .height = 1, .depth = 1};
    g_ntb = 0;
    sent_fill(dst, sizeof dst);
    nvn_be_read_texels(&one, 1, 0, &row, dst, 16, 32);
    CHECK(g_ntb == 1 && g_tb[0].level == 1 && g_tb[0].layer == 0 &&
          rt_rows_are(dst, 16, 0, 1, &row, 12, 1) && all_sent(dst + 12, sizeof dst - 12));
    row.zoffset = 1;
    row.depth = 2;
    g_ntb = 0;
    sent_fill(dst, sizeof dst);
    nvn_be_read_texels(&array, 1, 1, &row, dst, 16, 32);
    printf("ReadTexels 1D array transfers=%d target_rejected=%llu\n", g_ntb,
           (unsigned long long)un_cnt("read_texels_gpu_target"));
    CHECK(g_ntb == 2 && g_tb[0].layer == 2 && g_tb[1].layer == 3 &&
          rt_rows_are(dst, 16, 2, 1, &row, 12, 1) && rt_rows_are(dst + 32, 16, 3, 1, &row, 12, 1) &&
          all_sent(dst + 12, 20) && all_sent(dst + 44, sizeof dst - 44));
    const int bb = g_begins;
    row.yoffset = 1;
    sent_fill(dst, sizeof dst);
    nvn_be_read_texels(&array, 1, 1, &row, dst, 16, 32);
    CHECK(all_sent(dst, sizeof dst) && g_begins == bb && un_cnt("read_texels_bad_region") == 1);
    { uint64_t st[4]; nvn_be_read_texels_gpu_stats(st); CHECK(st[0] == st0[0] + 7); }   // 只数成功读回
    nvn_be_tex_release(&one); nvn_be_tex_release(&array);
    nvn_be_tex_release(&a); nvn_be_tex_release(&b); nvn_be_tex_release(&e); nvn_be_tex_release(&d);
    g_buf_cpu_auto = cpu_auto; g_auto_done = 0; g_tb_fill = 0;
}

// 成帧时读取累计数取差；同一帧会有多个读者，查询不能清零。
static uint64_t fake_compiled, fake_created;
uint64_t rp_shader_compiled_programs(void) { return fake_compiled; }
uint64_t rp_sync_pipelines_created(void) { return fake_created; }

static void t_create_stats(void) {
    uint64_t programs, pipelines;
    fake_compiled = 3; fake_created = 7;
    nvn_be_create_stats(&programs, &pipelines);
    CHECK(programs == 3 && pipelines == 7);
    fake_created++;
    nvn_be_create_stats(&programs, NULL);
    nvn_be_create_stats(NULL, &pipelines);
    CHECK(programs == 3 && pipelines == 8);
}

static void t_cpu_upload(void) {   // 存储里是 CPU 写的数据：首次使用上传，之后只在脏了时重传；内容交 GPU 后不再传
    nvn_be_sub_begin();
    res_fresh();
    tex_t t = mk_tex(5, 0, NVN_FORMAT_RGBA8, 8, 8, 2, 0);
    uint8_t lin0[8 * 8 * 4], lin1[4 * 4 * 4]; for (unsigned i = 0; i < sizeof lin0; i++) lin0[i] = (uint8_t)(i * 7 + 1); for (unsigned i = 0; i < sizeof lin1; i++) lin1[i] = (uint8_t)(200 - i);
    CHECK(nvn_swizzle(&t.d, 0, 0, lin0, 32, t.mem) == 0 && nvn_swizzle(&t.d, 1, 0, lin1, 16, t.mem) == 0);
    int c0 = g_tex_creates, r0 = g_tex_releases, p0 = g_protects; uint64_t u0 = rp_res_uploads();
    uint64_t first0, repeat0, first, repeat;
    nvn_be_pkg_stats(&first0, &repeat0);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
    CHECK(g_tex_creates - c0 == 1 && (g_last_usage & NVN_GPU_TEX_CPU_UPLOAD));
    CHECK(rp_res_uploads() - u0 == 1 && g_nup == 2 && g_protects - p0 == 1 && g_up[0].pre == 1);   // 上传是传输：录进前置段
    CHECK(g_up[0].level == 0 && g_up[0].pitch == 32 && g_up[0].img == 256 && g_up[0].w == 8 && g_up[0].h == 8 && !memcmp(g_up[0].first, lin0, 64));
    CHECK(g_up[1].level == 1 && g_up[1].pitch == 16 && g_up[1].img == 64 && g_up[1].w == 4 && !memcmp(g_up[1].first, lin1, 64));
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() - u0 == 1);   // 没脏：不重传
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() - u0 == 2);   // 游戏写过：重传
    CHECK(nvn_be_tex_gpu_owned(5) == 0);
    rp_tex_mark_gpu_written(&t); g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() - u0 == 2);   // 内容已归 GPU：不再从游戏内存传
    CHECK(nvn_be_tex_gpu_owned(5) == 1);
    nvn_be_tex_cpu_written(&t);                                                     // 游戏又 WriteTexels 了
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() - u0 == 3 && nvn_be_tex_gpu_owned(5) == 0);
    nvn_be_pkg_stats(&first, &repeat);
    CHECK(first - first0 == 1 && repeat - repeat0 == 2);   // 两级首传一次，脏写与 WriteTexels 后各重传一次
    nvn_be_tex_release(&t);
    CHECK(g_tex_releases - r0 == 1 && rp_res_tex_built() == 0);                     // CPU 内容贴图不进共用表：对象 Finalize 即放
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// 能力位开启时原始块线性数据逐级交给后端；线性贴图仍走 CPU 行拷。
static void t_gpu_deswizzle_route(void) {
    nvn_be_sub_begin(); res_fresh(); g_caps_extra |= NVN_GPU_CAP_GPU_DESWIZZLE; g_block_uploads = 0;
    tex_t t = mk_tex(60001, 0, NVN_FORMAT_RGBA8, 8, 8, 2, 0);
    memset(t.mem, 0x5a, t.L.size);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
    CHECK(g_block_uploads == 2 && g_nup == 0);
    CHECK(g_block_call[0].level == 0 && g_block_call[1].level == 1 && g_block_call[0].layer == 0);
    CHECK(g_block_call[0].src == t.mem && g_block_call[0].bytes == t.L.size && g_block_call[0].layout.layer_size == t.L.layer_size);
    nvn_be_tex_release(&t); res_fresh(); g_block_uploads = 0;
    tex_t lin = mk_tex(60002, 0, NVN_FORMAT_R8, 8, 8, 1, NVN_TEXTURE_FLAGS_LINEAR);
    memset(lin.mem, 0x33, lin.L.size);
    CHECK(rp_tex_resolve(&lin, &g_fake_cmd) != NULL && g_block_uploads == 0 && g_nup == 1);
    nvn_be_tex_release(&lin); res_fresh(); g_caps_extra &= ~NVN_GPU_CAP_GPU_DESWIZZLE; g_block_uploads = 0;
    tex_t off = mk_tex(60003, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); memset(off.mem, 0x37, off.L.size);
    CHECK(rp_tex_resolve(&off, &g_fake_cmd) != NULL && g_block_uploads == 0 && g_nup == 1);
    nvn_be_tex_release(&off); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// 在途 GPU 写的池镜像比游戏内存更新；不能从旧 CPU 字节建图或当成零。
static void t_gpu_deswizzle_pending(void) {
    extern uint64_t rp_buf_gpu_write_note(uint64_t, uint64_t, uint64_t *);
    extern void rp_buf_gpu_write_land(uint64_t, uint64_t, int);
    nvn_be_shutdown_session(); res_fresh(); nvn_unimpl_reset(); g_buf_cpu_auto = 1; g_pg_tracked = 0;
    g_caps_extra |= NVN_GPU_CAP_GPU_DESWIZZLE; g_block_uploads = 0;
    nvn_be_pool_add(g_tmem, sizeof g_tmem); nvn_be_sub_begin();
    uint64_t base = (uint64_t)(uintptr_t)g_tmem, key = 0;
    nvn_gpu_buf mirror = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, sizeof g_tmem, &mirror, NULL) && mirror && mirror->cpu);
    uint64_t id = rp_buf_gpu_write_note(base, 16, &key); CHECK(id && key == base);
    tex_t a = mk_tex(60011, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); a.has_layout = 1; memset(a.mem, 0x5a, a.L.size);
    tex_t b = mk_tex(60012, 0, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    tex_t other = mk_tex(60013, 8192, NVN_FORMAT_RGBA8, 8, 8, 1, 0); other.has_layout = 1; memset(other.mem, 0x62, other.L.size);
    int before = g_block_uploads, linear_before = g_nup;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) == NULL && g_block_uploads == before);
    CHECK(rp_tex_resolve(&b, &g_fake_cmd) == NULL && g_block_uploads == before && g_nup == linear_before);
    CHECK(nvn_unimpl_count("tex", "gpu_deswizzle_pending_pool_write") == 2);
    CHECK(rp_tex_resolve(&other, &g_fake_cmd) != NULL && g_block_uploads == before + 1);
    if (mirror && mirror->cpu) memset(mirror->cpu, 0x7b, 16);
    if (id) rp_buf_gpu_write_land(key, id, 1);
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_block_uploads == before + 2 && a.mem[0] == 0x7b);
    CHECK(rp_tex_resolve(&b, &g_fake_cmd) != NULL && g_block_uploads == before + 3);
    nvn_be_tex_release(&a); nvn_be_tex_release(&b); nvn_be_tex_release(&other);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_pool_del(g_tmem);
    g_caps_extra &= ~NVN_GPU_CAP_GPU_DESWIZZLE; g_buf_cpu_auto = 0; g_pg_tracked = 1; nvn_be_shutdown_session(); res_fresh();
}

// 后端块线性上传报 rc（校验 / 暂存失败不粘滞命令缓冲）：回放具名回退 CPU，贴图照传且 uploaded 置位（rp_res_uploads 只在置位时 +1）。
// 负对照：upload_all 不看 rc 直接 goto done → g_nup 不涨但 resolve 照样成，断言红。
static void t_gpu_deswizzle_backend_fail(void) {
    extern uint64_t rp_res_uploads(void);
    nvn_be_sub_begin(); res_fresh(); nvn_unimpl_reset();
    g_caps_extra |= NVN_GPU_CAP_GPU_DESWIZZLE; g_block_uploads = 0; g_nup = 0; g_block_rc = NVN_GPU_E_ARG;
    tex_t a = mk_tex(60020, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); a.has_layout = 1; memset(a.mem, 0x5a, a.L.size);
    uint64_t up0 = rp_res_uploads();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_block_uploads == 1 && g_nup == 1);
    CHECK(nvn_unimpl_count("tex", "gpu_deswizzle_backend_fail") == 1 && rp_res_uploads() == up0 + 1);
    nvn_be_tex_release(&a); g_block_rc = 0; g_caps_extra &= ~NVN_GPU_CAP_GPU_DESWIZZLE;
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// 诊断只认完整、稳定的原始内容；未跟踪或上传期间有写不能算内容未变。
static void t_tex_reupload_diag(void) {
    nvn_be_sub_begin(); res_fresh(); g_flag_tdiag = 1; g_tdiag_n = g_tdiag_summaries = 0; g_pg_tracked = 1;
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    tex_t t = mk_tex(60101, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); t.mem = g_block_mem; memset(t.mem, 0x55, t.L.size);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_tdiag_n == 1 && strstr(g_tdiag_lines[0], "kind=first state=initial"));
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[0] = g_dirty_written;   // 真写同时推进被写页的代号（脏页日志按它出事件）
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_tdiag_n == 2 && strstr(g_tdiag_lines[1], "kind=repeat state=same"));
    t.mem[3] ^= 0x7f; g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[0] = g_dirty_written;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_tdiag_n == 3 && strstr(g_tdiag_lines[2], "kind=repeat state=changed"));
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[0] = g_dirty_written; g_tdiag_race = 1;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_tdiag_n == 4 && strstr(g_tdiag_lines[3], "state=unknown_race"));
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[0] = g_dirty_written;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_tdiag_n == 5 && strstr(g_tdiag_lines[4], "state=initial"));
    g_pg_tracked = 0; g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[0] = g_dirty_written;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_tdiag_n == 6 && strstr(g_tdiag_lines[5], "state=unknown_untracked"));
    g_pg_tracked = 1; g_log_fake_on = 1; g_log_unarmed[0] = 1; g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; fake_log_write(0, 0x66);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_tdiag_n == 7 && strstr(g_tdiag_lines[6], "state=unknown_race"));
    g_log_unarmed[0] = 0; g_log_fake_on = 0;
    for (int i = 0; i < 7; i++) { char seq[24]; snprintf(seq, sizeof seq, "seq=%d ", i + 1); CHECK(!strncmp(g_tdiag_lines[i], seq, strlen(seq))); }
    nvn_be_tex_release(&t); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    res_fresh(); g_flag_tdiag = 0; g_pg_tracked = 1; nvn_be_sub_begin();
    CHECK(g_tdiag_summaries == 1 && strstr(g_tdiag_summary, "events=7 first=1 repeat=6 initial=2 same=1 changed=1 unknown=3"));
    tex_t off = mk_tex(60102, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); off.mem = g_block_mem; memset(off.mem, 0x44, off.L.size);
    CHECK(rp_tex_resolve(&off, &g_fake_cmd) != NULL && g_tdiag_n == 7 && g_tdiag_summaries == 1);
    nvn_be_tex_release(&off); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_block_mem = NULL; res_fresh();
}

static void t_tex_reupload_diag_gpu_pending(void) {
    extern uint64_t rp_buf_gpu_write_note(uint64_t, uint64_t, uint64_t *);
    extern void rp_buf_gpu_write_land(uint64_t, uint64_t, int);
    nvn_be_shutdown_session(); res_fresh(); g_flag_tdiag = 1; g_tdiag_n = 0; g_pg_tracked = 1; g_buf_cpu_auto = 1;
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    g_tp.mem = g_block_mem; g_tp.size = BLOCK_TEST_PAGES * PG; memset(g_block_mem, 0x32, g_tp.size);
    nvn_be_pool_add(g_block_mem, g_tp.size); nvn_be_sub_begin();
    uint64_t base = (uint64_t)(uintptr_t)g_block_mem, key = 0; nvn_gpu_buf mirror = NULL;
    CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &mirror, NULL) && mirror && mirror->cpu);
    tex_t t = mk_tex(60103, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); t.mem = g_block_mem; t.has_layout = 1;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) && g_tdiag_n == 1 && strstr(g_tdiag_lines[0], "state=initial"));
    uint64_t id = rp_buf_gpu_write_note(base, 16, &key); CHECK(id && key == base);
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[0] = g_dirty_written;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) && g_tdiag_n == 2 && strstr(g_tdiag_lines[1], "state=unknown_gpu_pending"));
    if (id) rp_buf_gpu_write_land(key, id, 0);
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[0] = g_dirty_written;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) && g_tdiag_n == 3 && strstr(g_tdiag_lines[2], "state=initial"));
    nvn_be_tex_release(&t); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_pool_del(g_block_mem);
    g_flag_tdiag = 0; g_buf_cpu_auto = 0; g_block_mem = NULL; nvn_be_shutdown_session(); res_fresh();
}

static void t_cpu_rules(void) {   // 首用判定的三条分支
    nvn_be_sub_begin();
    res_fresh();
    tex_t a = mk_tex(21, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); memset(a.mem, 0x55, a.L.size);
    nvn_be_pool_gpu_written(&g_tp);                                 // GPU 写过的池里的未压缩贴图：当渲染目标，不当 CPU 贴图；建图时存储里现有的内容只取这一次
    uint64_t first0, repeat0, first, repeat;
    nvn_be_pkg_stats(&first0, &repeat0);
    { uint64_t u0 = rp_res_uploads(); int p0 = g_protects;
      CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == 1 && g_up[0].first[0] == 0x55 && rp_res_uploads() == u0 && g_protects == p0);
      memset(a.mem, 0x56, a.L.size); g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
      CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == 1); }   // 之后内存再变也不重传（不跟踪写）
    nvn_be_pkg_stats(&first, &repeat);
    CHECK(first - first0 == 1 && repeat == repeat0);   // 随存储初始化实际上传一次，随后读同一张不重复记
    tex_t b = mk_tex(22, 0x2000, NVN_FORMAT_RGBA8, 8, 8, 1, 0); memset(b.mem, 0x66, b.L.size);
    nvn_be_tex_cpu_written(&b);                                     // 但游戏明确 WriteTexels 写过的：取存储
    CHECK(rp_tex_resolve(&b, &g_fake_cmd) != NULL && g_nup == 2 && g_up[1].first[0] == 0x66);
    nvn_be_tex_release(&a); nvn_be_tex_release(&b); nvn_be_alias_drop_pool(&g_tp);
    res_fresh();
    tex_t d = mk_tex(23, 0, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_COMPRESSIBLE); memset(d.mem, 0x77, d.L.size);
    { uint64_t u0 = rp_res_uploads();
      CHECK(rp_tex_resolve(&d, &g_fake_cmd) != NULL && rp_res_uploads() == u0);   // COMPRESSIBLE（渲染目标的标志）：不当 CPU 贴图
      CHECK(g_nup == 1 && g_up[0].first[0] == 0x77); }                           // CPU 能写的池：压缩提示不是数据格式，建图时照取一次
    nvn_be_tex_release(&d); nvn_be_alias_drop_pool(&g_tp);
    // CPU 不能访问的池（游戏把文件读进去再在上面建贴图，之后没有任何通知）：建图时取一次存储
    res_fresh(); g_tp.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    { static const uint8_t blk[8] = {0x10, 0x84, 0x00, 0x00, 0xe4, 0xe4, 0xe4, 0xe4}, zero[64];
      tex_t e = mk_tex(24, 0, NVN_FORMAT_RGBA_DXT1, 4, 4, 1, NVN_TEXTURE_FLAGS_IMAGE); memcpy(e.mem, blk, 8);   // 4x4 的 DXT1 只有一个块：存储里就是这 8 字节
      uint64_t u0 = rp_res_uploads();
      CHECK(rp_tex_resolve(&e, &g_fake_cmd) != NULL && g_nup == 1 && g_up[0].img == 8 && g_up[0].pre == 1 && !memcmp(g_up[0].first, blk, 8) && rp_res_uploads() == u0);
      tex_t f = mk_tex(25, 0x2000, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_COMPRESSIBLE); memset(f.mem, 0x33, f.L.size);
      CHECK(rp_tex_resolve(&f, &g_fake_cmd) != NULL && g_nup == 2 && !memcmp(g_up[1].first, zero, 64));   // 这种池里带压缩提示的：内存对 CPU 没有意义，不取，清零
      tex_t h = mk_tex(26, 0x4000, NVN_FORMAT_R11G11B10F, 8, 8, 1, 0); { uint32_t v = 0x3f000000u | 31u << 6; memcpy(h.mem, &v, 4); memset(h.mem + 4, 0x11, 16); }   // 第一个像素的 R 是 Inf
      CHECK(rp_tex_resolve(&h, &g_fake_cmd) != NULL && g_nup == 3 && !memcmp(g_up[2].first, zero, 64));   // 这种池里浮点格式带 Inf / NaN：是别人留下的，不取，清零
      tex_t k = mk_tex(27, 0x6000, NVN_FORMAT_DEPTH24_STENCIL8, 8, 8, 1, 0); memset(k.mem, 0x22, k.L.size);
      CHECK(rp_tex_resolve(&k, &g_fake_cmd) != NULL && g_nup == 3);   // 深度模板：不取也不清（通道自己清）
      nvn_be_tex_release(&e); nvn_be_tex_release(&f); nvn_be_tex_release(&h); nvn_be_tex_release(&k); nvn_be_alias_drop_pool(&g_tp); }
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// LINEAR 图集（R8 / IMAGE|IMAGE... 字体与 UI 由 CPU 写）：首次按线性行距逐行上传，之后 CPU 更新能到下一次采样，
// 没变化不重传；换映射代际要重传；同提交里的第二次更新排在主段。iOS 同名语义：nvn_mtl.m tex_linear @3756 / tex_linear_needs_upload @3777。
static void t_linear_upload(void) {
    nvn_be_sub_begin();
    res_fresh();
    // 3x2 R8、行距 8：每行 3 个有效字节 + 5 字节 padding（毒值）。首用前全是 0（字体引擎可以建图之后才填字形）。
    tex_t t = mk_tex(41, 0x100, NVN_FORMAT_R8, 3, 2, 1, NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_IMAGE);
    t.d.stride = 8; nvn_tex_layout(&t.d, &t.L); t.mem = g_tmem + t.off;
    int n0 = g_nup, p0 = g_protects; uint64_t u0 = rp_res_uploads();
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
    CHECK(g_nup == n0 + 1 && rp_res_uploads() - u0 == 1 && g_protects - p0 == 1 && g_up[n0].pre == 1);
    CHECK(g_up[n0].level == 0 && g_up[n0].layer == 0 && g_up[n0].pitch == 3 && g_up[n0].img == 6 && g_up[n0].w == 3 && g_up[n0].h == 2);
    { static const uint8_t zero[6]; CHECK(!memcmp(g_up[n0].first, zero, 6)); }      // 初始全零：也要有一次定义内容的上传
    CHECK(g_last_usage & NVN_GPU_TEX_CPU_UPLOAD);
    // 填字形：行距 8、padding 是毒值，上传缓冲只能是 3x2 的紧凑行宽
    for (int y = 0; y < 2; y++) { t.mem[y * 8 + 0] = (uint8_t)(0x11 + y); t.mem[y * 8 + 1] = 0x22; t.mem[y * 8 + 2] = 0x33; memset(t.mem + y * 8 + 3, 0xEE, 5); }
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == n0 + 2 && g_up[n0 + 1].pitch == 3 && g_up[n0 + 1].img == 6);
    CHECK(g_up[n0 + 1].first[0] == 0x11 && g_up[n0 + 1].first[1] == 0x22 && g_up[n0 + 1].first[2] == 0x33 &&
          g_up[n0 + 1].first[3] == 0x12 && g_up[n0 + 1].first[4] == 0x22 && g_up[n0 + 1].first[5] == 0x33);   // 逐行拷：padding 不进缓冲
    CHECK(g_up[n0 + 1].pre == 0);                                                   // 同一提交里的第二次同步：排在主段
    CHECK(g_protects - p0 == 2);                                                    // 每次重传前都重新保护
    { int p1 = g_protects; uint64_t u1 = rp_res_uploads(); int ok = 1;
      for (int i = 0; i < 100; i++) ok &= rp_tex_resolve(&t, &g_fake_cmd) != NULL;
      CHECK(ok && g_protects == p1 && rp_res_uploads() == u1); }                    // 稳定内容：不传、不重新保护
    // 映射换底后的失效判定（全局代际 / 范围句柄）尚未接入：换底时 nvn_dirty_replace_range 会把该范围的页代际抬到旧 stamp 之后，
    // 常规换底已由 nvn_dirty_since 判脏；remove+add 路径的漏判留作后续（见 docs 对照表）。
    rp_tex_mark_gpu_written(&t); g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == n0 + 2);              // 内容归 GPU：不再从游戏内存传
    nvn_be_tex_release(&t);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// 线性族里第一批不收的组合：准确报红（不猜内容），初始化仍按“取不了就清零”走。
static void t_linear_reject(void) {
    nvn_be_sub_begin();
    nvn_unimpl_reset();
    res_fresh();
    { tex_t a = mk_tex(51, 0, NVN_FORMAT_R8, 4, 4, 2, NVN_TEXTURE_FLAGS_LINEAR);   // 多级：公共布局只给单级的行距
      int n0 = g_nup;
      CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && nvn_unimpl_count("tex", "linear_mips_unsupported") == 1);
      CHECK(g_nup == n0 + 2 && g_up[n0].pitch == 4 && g_up[n0].img == 16);          // 报红之后仍是真的清零初始化（两级各一次），不是静默错内容
      nvn_be_tex_release(&a); }
    res_fresh();
    { tex_t b = mk_tex(52, 0, NVN_FORMAT_R8, 4, 4, 1, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);   // 只有 LINEAR_RT：内容归 GPU（可能由 copy_b2t 写入），不按 CPU 内容传、不报红
      int n0 = g_nup, u0 = g_nup; (void)u0;
      CHECK(rp_tex_resolve(&b, &g_fake_cmd) != NULL && nvn_unimpl_count("tex", "linear_render_target_unsupported") == 0);
      CHECK(g_nup == n0 + 1 && g_up[n0].img == 4 * 4 * 1 && g_up[n0].first[0] == 0);   // 新建存储清零一次
      nvn_be_tex_release(&b); }
    res_fresh();
    { // 只有 LINEAR_RT、游戏用 WriteTexels 写了它（CPU 贴图）：按线性排布逐行拷进来，不报红——同苹果端 nvn_mtl.m 上传口径。
      //   原先 upload_all 里拒掉、报 linear_render_target_unsupported，内容一次都传不上去（11-21 真机每帧约 2 张、整屏黑）。
      //   行距 8、每行只有 4 字节有效，padding 填毒值：上传缓冲只能是 4x4 的紧凑行
      tex_t b2 = mk_tex(54, 0x1000, NVN_FORMAT_R8, 4, 4, 1, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);
      b2.d.stride = 8; nvn_tex_layout(&b2.d, &b2.L); b2.mem = g_tmem + b2.off;
      for (int y = 0; y < 4; y++) { for (int x = 0; x < 4; x++) b2.mem[y * 8 + x] = (uint8_t)(0x40 + y * 4 + x); memset(b2.mem + y * 8 + 4, 0xEE, 4); }
      nvn_be_tex_cpu_written(&b2);
      int n0 = g_nup;
      CHECK(rp_tex_resolve(&b2, &g_fake_cmd) != NULL && nvn_unimpl_count("tex", "linear_render_target_unsupported") == 0);
      CHECK(g_nup == n0 + 1 && g_up[n0].pitch == 4 && g_up[n0].img == 16);
      CHECK(g_up[n0].first[0] == 0x40 && g_up[n0].first[3] == 0x43 && g_up[n0].first[4] == 0x44 && g_up[n0].first[15] == 0x4F);   // 逐行拷、padding 不进缓冲
      nvn_be_tex_release(&b2); }
    res_fresh();
    { // 线性渲染目标再带 SPARSE（普通池，不是虚拟池）：SPARSE 只说驻留、不改排布，照线性逐行拷，不报「线性加稀疏」——同苹果端 lin 口径
      tex_t b3 = mk_tex(55, 0x2000, NVN_FORMAT_R8, 4, 4, 1, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE);
      b3.d.stride = 8; nvn_tex_layout(&b3.d, &b3.L); b3.mem = g_tmem + b3.off;
      for (int y = 0; y < 4; y++) { for (int x = 0; x < 4; x++) b3.mem[y * 8 + x] = (uint8_t)(0x60 + y * 4 + x); memset(b3.mem + y * 8 + 4, 0xEE, 4); }
      nvn_be_tex_cpu_written(&b3);
      int n0 = g_nup;
      CHECK(rp_tex_resolve(&b3, &g_fake_cmd) != NULL && nvn_unimpl_count("tex", "linear_sparse_unsupported") == 0);
      CHECK(g_nup == n0 + 1 && g_up[n0].pitch == 4 && g_up[n0].img == 16);
      CHECK(g_up[n0].first[0] == 0x60 && g_up[n0].first[4] == 0x64 && g_up[n0].first[15] == 0x6F);
      nvn_be_tex_release(&b3); }
    res_fresh();
    { tex_t c = mk_tex(53, (int64_t)sizeof g_tmem - 0x10, NVN_FORMAT_R8, 64, 64, 1, NVN_TEXTURE_FLAGS_LINEAR);   // 源范围出池（池大小跟着 g_tmem 走）
      CHECK(rp_tex_resolve(&c, &g_fake_cmd) != NULL && nvn_unimpl_count("tex", "linear_range_outside_pool") == 1);
      nvn_be_tex_release(&c); }
    res_fresh();
    { tex_t d = mk_tex(54, 0, NVN_FORMAT_R8, 2, 2, 1, NVN_TEXTURE_FLAGS_LINEAR); memset(d.mem, 0x5A, 4);
      nvn_be_tex_cpu_written(&d);   // WriteTexels 写过的线性贴图：以前打 upload_linear_or_sparse 红行，现在能传
      int n0 = g_nup;
      CHECK(rp_tex_resolve(&d, &g_fake_cmd) != NULL && g_nup == n0 + 1 && g_up[n0].first[0] == 0x5A);
      nvn_be_tex_release(&d); }
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// #976：后端没有的两种镜像夹取 → 核心记具名近似、按 MIRROR_CLAMP_TO_EDGE 重建；扩展也没有 → 不给采样器，按后端的名字记未接、红行带原文。
static void t_samp_mirror_clamp(void) {
    res_fresh(); nvn_unimpl_reset();
    samp_par sp; memset(&sp, 0, sizeof sp);
    sp.ws = NVN_WRAP_MODE_MIRROR_CLAMP; sp.wt = NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER; sp.wr = NVN_WRAP_MODE_REPEAT; sp.border[1] = 0.5f;
    nvn_be_reg_samp(2, &sp);
    int c0 = g_samp_creates, t0 = g_samp_tries;
    nvn_gpu_samp q = rp_samp_by_id(2);
    CHECK(q && g_samp_creates - c0 == 1 && g_samp_tries - t0 == 2);   // 先按原样问一次（后端拒），再按近似建成
    CHECK(g_samp_last.ws == NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE && g_samp_last.wt == NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE && g_samp_last.wr == NVN_WRAP_MODE_REPEAT);
    CHECK(g_samp_last.border[1] == 0.5f);                              // 其余参数原样
    CHECK(nvn_unimpl_count("gpu", "samp_wrap_mirror_clamp_approx") == 1 && nvn_unimpl_kinds() == 1);
    CHECK(rp_samp_by_id(2) == q && g_samp_tries - t0 == 2);            // 建成后不再重问，近似只记一次
    sp.ws = NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE; sp.wt = NVN_WRAP_MODE_CLAMP_TO_EDGE; nvn_be_reg_samp(3, &sp);
    CHECK(rp_samp_by_id(3) && g_samp_last.ws == NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE && nvn_unimpl_count("gpu", "samp_wrap_mirror_clamp_approx") == 1);   // 原生有的不算近似
    g_samp_no_mce = 1; int red0 = g_red_lines;
    sp.ws = NVN_WRAP_MODE_MIRROR_CLAMP; nvn_be_reg_samp(4, &sp);
    CHECK(rp_samp_by_id(4) == NULL);                                   // 近似也建不了：不给采样器（这笔绘制按槽位未解析跳过），不画别的
    CHECK(nvn_unimpl_count("gpu", "samp_wrap_mirror_clamp_approx") == 2 && nvn_unimpl_count("gpu", "sampler_mirror_clamp_to_edge") == 1);
    CHECK(g_red_lines - red0 == 2 && strstr(g_last_red, "sampler_mirror_clamp_to_edge：替身设备没有扩展"));   // 未接首行 + 后端原文
    sp.ws = NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE; nvn_be_reg_samp(5, &sp);
    CHECK(rp_samp_by_id(5) == NULL && nvn_unimpl_count("gpu", "sampler_mirror_clamp_to_edge") == 2 && nvn_unimpl_count("gpu", "samp_create") == 0);
    g_samp_no_mce = 0;
    res_fresh(); nvn_unimpl_reset();
}

// 整池数组的后端替身：记最近一次建表的项（状态与资源）、建表 / 放表 / 绑表次数
static int g_pool_creates, g_pool_releases, g_pool_binds; static nvn_gpu_pool_item g_pool_last[16]; static uint32_t g_pool_last_n, g_pool_last_kind;
nvn_gpu_rc nvn_gpu_pool_create(const nvn_gpu_pool_desc *d, nvn_gpu_pool_table *out, nvn_gpu_err *err) {
    (void)err; g_pool_creates++; g_pool_last_n = d->count; g_pool_last_kind = d->kind;
    for (uint32_t i = 0; i < d->count && i < 16; i++) g_pool_last[i] = d->items[i];
    size_t nv = d->kind == NVN_GPU_POOL_IMAGE ? d->count : 0;
    *out = calloc(1, sizeof **out + nv * sizeof(nvn_gpu_view)); (*out)->kind = d->kind; (*out)->count = d->count;
    for (size_t i = 0; i < nv; i++)
        if (d->items[i].state == NVN_GPU_POOL_READY) (*out)->views[i] = d->items[i].resource.view;
    return (nvn_gpu_rc){NVN_GPU_OK};
}
// 每次绑表的记录（阶段、布局项号、表指针与它的种类 / 项数）：用例把 g_pool_bind_n 清零后读
static struct { int st; uint32_t ent, kind, count; nvn_gpu_pool_table t; } g_pool_bind_log[16]; static int g_pool_bind_n;
nvn_gpu_rc nvn_gpu_pool_bind(nvn_gpu_cmd c, nvn_gpu_stage st, uint32_t ent, nvn_gpu_pool_table t, nvn_gpu_err *err) {
    (void)c; (void)err; g_pool_binds++;
    if (g_color_sample_test && st == NVN_GPU_SH_FS && ent == 0 && t && t->kind == NVN_GPU_POOL_IMAGE)
        g_color_image_table = t;
    if (g_pool_bind_n < 16) {
        g_pool_bind_log[g_pool_bind_n].st = (int)st; g_pool_bind_log[g_pool_bind_n].ent = ent; g_pool_bind_log[g_pool_bind_n].t = t;
        g_pool_bind_log[g_pool_bind_n].kind = t ? t->kind : 0; g_pool_bind_log[g_pool_bind_n].count = t ? t->count : 0; g_pool_bind_n++;
    }
    return (nvn_gpu_rc){NVN_GPU_OK};
}
void nvn_gpu_pool_release(nvn_gpu_pool_table t) { g_pool_releases++; free(t); }
int nvn_gpu_pool_has_texture(nvn_gpu_pool_table t, nvn_gpu_tex tex) {
    if (!t || t->kind != NVN_GPU_POOL_IMAGE || !tex) return 0;
    for (uint32_t i = 0; i < t->count; i++)
        if (nvn_gpu_view_texture(t->views[i]) == tex) return 1;
    return 0;
}

// 整池数组的池快照（rp_pool_table）：
//   期望（手算）：贴图池起点 0、池长 8，号 2 登记了一张 2D 贴图、其余没登记 → 8 项全 READY（没登记的给声明类型零贴图），号 2 是它自己的视图；
//   同键同纪元再取：不重建（建表次数不变）；登记表一变（重登记号 5）：重建一次、旧表放掉；采样器池：登记了的 READY、没登记的 EMPTY
static void t_pool_table(void) {
    nvn_be_sub_begin(); res_fresh(); nvn_unimpl_reset();
    int c0 = g_pool_creates, r0 = g_pool_releases;
    tex_t a = mk_tex(41, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(42, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    nvn_be_set_tex_pool_n(8); nvn_be_reg_tex(2, &a, NULL);
    const char *why = NULL;
    nvn_gpu_pool_table t1 = rp_pool_table(&g_fake_cmd, 0, 0, 8, NVN_TEXTURE_TARGET_2D + 1, 0, &why);
    CHECK(t1 && !why && g_pool_creates - c0 == 1 && g_pool_last_kind == NVN_GPU_POOL_IMAGE && g_pool_last_n == 8);
    int ready = 0; for (int i = 0; i < 8; i++) ready += g_pool_last[i].state == NVN_GPU_POOL_READY;
    CHECK(ready == 8 && g_pool_last[2].resource.view && g_pool_last[2].resource.view->of && g_pool_last[2].resource.view != g_pool_last[3].resource.view);
    CHECK(rp_pool_table(&g_fake_cmd, 0, 0, 8, NVN_TEXTURE_TARGET_2D + 1, 0, &why) == t1 && g_pool_creates - c0 == 1);   // 同键同纪元：沿用
    nvn_be_reg_tex(5, &b, NULL);
    nvn_gpu_pool_table t2 = rp_pool_table(&g_fake_cmd, 0, 0, 8, NVN_TEXTURE_TARGET_2D + 1, 0, &why);
    CHECK(t2 && t2 != t1 && g_pool_creates - c0 == 2 && g_pool_releases - r0 == 1);   // 登记变了：重建、旧表放掉
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_samp(1, &sp);
    nvn_gpu_pool_table ts1 = rp_pool_table(&g_fake_cmd, 1, 0, 4, 0, 0, &why);
    CHECK(ts1 && g_pool_last_kind == NVN_GPU_POOL_SAMPLER && g_pool_last_n == 4 && g_pool_last[1].state == NVN_GPU_POOL_READY &&
          g_pool_last[0].state == NVN_GPU_POOL_EMPTY && g_pool_last[3].state == NVN_GPU_POOL_EMPTY);
    CHECK(!rp_pool_table(&g_fake_cmd, 0, 0, 0xffff, NVN_TEXTURE_TARGET_2D + 1, 0, &why) && why);   // 池长表达不了：具名原因
    nvn_be_unreg_tex_range(0, 8);
}

static void t_registry(void) {
    nvn_be_sub_begin();
    res_fresh(); nvn_unimpl_reset(); int v0 = g_view_creates, vr0 = g_view_releases, s0 = g_samp_creates, sr0 = g_samp_releases;
    tex_t a = mk_tex(31, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(32, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    view_t vw; memset(&vw, 0, sizeof vw); vw.set_levels = 1; vw.nlevels = 1;
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == NULL);             // 没登记
    { tex_t miss; memset(&miss, 0x5A, sizeof miss); CHECK(rp_view_by_id(3, &g_fake_cmd, &miss) == NULL && miss.serial == 0); }   // 没取到：调用方的快照清零
    nvn_be_reg_tex(3, &a, NULL); nvn_be_set_tex_pool_n(8);
    tex_t tt; nvn_gpu_view v1 = rp_view_by_id(3, &g_fake_cmd, &tt);
    CHECK(v1 && tt.serial == 31 && v1->has_v == 0);
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == v1 && g_view_creates - v0 == 1);   // 第二次取：同一个视图
    nvn_be_reg_tex(3, &a, NULL);
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == v1 && g_view_creates - v0 == 1);   // 同内容重登记：视图还在
    nvn_be_reg_tex(3, &b, &vw);
    nvn_gpu_view v2 = rp_view_by_id(3, &g_fake_cmd, &tt);
    CHECK(v2 && tt.serial == 32 && v2->has_v == 1 && g_view_creates - v0 == 2 && g_view_releases - vr0 == 1);   // 换了内容：旧视图放掉
    nvn_be_reg_tex(9, &a, NULL);
    CHECK(rp_view_by_id(9, &g_fake_cmd, NULL) == NULL);             // 池容量 8：号 9 在池外，当空槽
    nvn_be_unreg_tex_range(0, 8);
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == NULL && g_view_releases - vr0 == 2);
    samp_par sp; memset(&sp, 0, sizeof sp);
    CHECK(rp_samp_by_id(4) == NULL);
    nvn_be_reg_samp(4, &sp); CHECK(g_samp_creates == s0);            // 登记不建
    nvn_gpu_samp q = rp_samp_by_id(4);
    CHECK(q && rp_samp_by_id(4) == q && g_samp_creates - s0 == 1);
    nvn_be_reg_samp(4, &sp);
    CHECK(g_samp_releases - sr0 == 1 && rp_samp_by_id(4) != NULL && g_samp_creates - s0 == 2);
    g_tex_create_fail = 1; tex_t f = mk_tex(40, 0x8000, 0x38, 16, 16, 1, 0); nvn_be_set_tex_pool_n(0); nvn_be_reg_tex(5, &f, NULL);
    CHECK(rp_view_by_id(5, &g_fake_cmd, NULL) == NULL && nvn_unimpl_count("gpu", "tex_create_fmt_0x38_target_1") == 1);   // 后端不支持：按格式号记账，不给假图
    g_tex_create_fail = 0;
    res_fresh();
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

#ifdef S2A_TEST_TREG_SCRUB
// #1581 C：回放线程取到贴图号的视图和登记快照后，客体线程再登记一个更大的号使登记表扩容（换地址），
//   回放线程之后再读快照，读到的必须仍是登记时的字段。旧表由宿主钩子写满 0xA5 并留着，读旧表读到标记值而不是崩溃。
//   同一个二进制里登记一个表内的号（不扩容）作对照：两轮只差扩不扩容。
int s2a_test_treg_grows;
static int g_grow_id;
static void *treg_grow_thread(void *arg) { nvn_be_reg_tex(g_grow_id, (const tex_t *)arg, NULL); return NULL; }
static void t_view_snap_grow(void) {
    for (int grow = 0; grow < 2; grow++) {
        nvn_be_sub_begin(); res_fresh(); nvn_unimpl_reset();
        tex_t a = mk_tex(501, 0, NVN_FORMAT_RGBA8, 16, 8, 1, 0), b = mk_tex(502, 0x4000, NVN_FORMAT_RGBA8, 32, 32, 1, 0);
        nvn_be_reg_tex(3, &a, NULL);   // 登记表容量 3 + 1024
        rp_view_pin_begin();
        tex_t tt, td;
        nvn_gpu_view v = rp_view_by_id(3, &g_fake_cmd, &tt);
        nvn_gpu_view vd = rp_view_by_id_decl(3, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, &td);   // 按声明改目标的那条路
        int g0 = s2a_test_treg_grows;
        g_grow_id = grow ? 4000 : 100;
        pthread_t th; CHECK(pthread_create(&th, NULL, treg_grow_thread, &b) == 0); pthread_join(th, NULL);
        CHECK(s2a_test_treg_grows - g0 == grow);   // 扩容轮确实换了表，对照轮没换
        CHECK(v && tt.serial == 501 && tt.d.width == 16 && tt.d.height == 8);
        CHECK(vd && td.serial == 501 && td.d.width == 16 && td.d.height == 8);
        rp_view_pin_end();
        res_fresh(); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    }
}
#endif

// #1810：一维 / 一维数组贴图。后端建不出目标 0 / 3（替身同真后端），翻译后的着色器一律把一维当二维采样（声明二维 / 二维数组）：
//   回放核心要按 iOS 同口径折成二维 / 二维数组（一维数组的层数在 NVN 描述的 height 里）再建、再比较声明，上传 / 清零每层一行。
//   期望值全部直接写常量（层数 4、宽 8、每层 32 字节），不从实现里推。
static void t_decl_view_1d(void) {
    nvn_be_sub_begin(); res_fresh(); nvn_unimpl_reset(); nvn_be_set_tex_pool_n(0);
    rp_view_pin_begin();
    const uint64_t mm0 = nvn_unimpl_count("tex", "decl_type_mismatch"); const int ml0 = g_mm_lines;   // 前面的用例也打过这类行：按本用例开始时的行数取差
    // ① 一维数组（宽 8、4 层、RGBA8，每层写不同内容）被声明成二维数组：折成二维数组 8×1、4 层建图，默认视图，不记不符，上传 4 层各 1 行
    tex_t a = mk_tex(8101, 0x10000, NVN_FORMAT_RGBA8, 8, 4, 1, 0); a.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&a.d, &a.L);
    CHECK(a.L.layers == 4);
    uint8_t lay[4][32];
    for (unsigned k = 0; k < 4; k++) { px_fill(lay[k], 10 + 20 * k, 8, 1, 4); CHECK(nvn_swizzle(&a.d, 0, k, lay[k], 32, a.mem) == 0); }
    nvn_be_reg_tex(30, &a, NULL);
    int creates = g_tex_creates; g_nup = 0; tex_t snap;
    nvn_gpu_view v = rp_view_by_id_decl(30, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, &snap);
    CHECK(v && !v->has_v && v->of->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->of->d.d.width == 8 && v->of->d.d.height == 1 && v->of->d.d.depth == 4);
    CHECK(g_tex_creates == creates + 1 && nvn_unimpl_count("tex", "decl_type_mismatch") == mm0);
    CHECK(snap.serial == 8101 && snap.d.target == NVN_TEXTURE_TARGET_1D_ARRAY && snap.d.height == 4 && snap.L.layers == 4);   // 登记快照与客体布局不动
    CHECK(g_nup == 4);
    for (unsigned k = 0; k < 4 && g_nup == 4; k++)
        CHECK(g_up[k].level == 0 && g_up[k].layer == k && g_up[k].w == 8 && g_up[k].h == 1 && g_up[k].pitch == 32 && g_up[k].img == 32 && !memcmp(g_up[k].first, lay[k], 32));
    CHECK(rp_view_by_id_decl(30, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL) == v && g_tex_creates == creates + 1);
    // ② 声明也是一维数组（目标 3）：两边折完相等，同一个默认视图
    CHECK(rp_view_by_id_decl(30, NVN_TEXTURE_TARGET_1D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL) == v && nvn_unimpl_count("tex", "decl_type_mismatch") == mm0);
    // ③ 一维（宽 16）：折成 16×1 的二维；声明二维、声明一维都给默认视图；声明二维数组按单层二维数组视图
    tex_t b = mk_tex(8102, 0x20000, NVN_FORMAT_RGBA8, 16, 1, 1, 0); b.d.target = NVN_TEXTURE_TARGET_1D; nvn_tex_layout(&b.d, &b.L);
    uint8_t row[64]; px_fill(row, 99, 16, 1, 4); CHECK(nvn_swizzle(&b.d, 0, 0, row, 64, b.mem) == 0);
    nvn_be_reg_tex(31, &b, NULL); creates = g_tex_creates; g_nup = 0;
    nvn_gpu_view v1 = rp_view_by_id_decl(31, NVN_TEXTURE_TARGET_2D + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(v1 && !v1->has_v && v1->of->d.d.target == NVN_TEXTURE_TARGET_2D && v1->of->d.d.width == 16 && v1->of->d.d.height == 1 && v1->of->d.d.depth == 1 && g_tex_creates == creates + 1);
    CHECK(g_nup == 1 && g_up[0].w == 16 && g_up[0].h == 1 && g_up[0].pitch == 64 && g_up[0].img == 64 && !memcmp(g_up[0].first, row, 64));
    CHECK(rp_view_by_id_decl(31, NVN_TEXTURE_TARGET_1D + 1, 0, 0, &g_fake_cmd, NULL) == v1);
    nvn_gpu_view v1a = rp_view_by_id_decl(31, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(v1a && v1a->of == v1->of && g_vw_last.set_target && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.set_layers && g_vw_last.nlayers == 1);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 && g_tex_creates == creates + 1);
    // ④ 全零的一维数组（高 4 = 4 层）：清零每层一行（不是把层数当行数）
    tex_t z = mk_tex(8103, 0x30000, NVN_FORMAT_RGBA8, 8, 4, 1, 0); z.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&z.d, &z.L);
    nvn_be_reg_tex(32, &z, NULL); g_nup = 0;
    nvn_gpu_view vz = rp_view_by_id_decl(32, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(vz && vz->of->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && vz->of->d.d.depth == 4 && vz->of->d.d.height == 1 && g_nup == 4);
    for (unsigned k = 0; k < 4 && g_nup == 4; k++) {
        CHECK(g_up[k].layer == k && g_up[k].w == 8 && g_up[k].h == 1 && g_up[k].pitch == 32 && g_up[k].img == 32);
        uint8_t zeros[32] = {0}; CHECK(!memcmp(g_up[k].first, zeros, 32));
    }
    // ⑤ 负对照：一维数组被声明成立方体 / 三维仍是真不符，记账、绑声明类型的零贴图，不把一维数组的视图交下去
    const uint64_t cube_unimpl = nvn_unimpl_count("tex", "cube_view_of_2d_array");
    nvn_gpu_view zc = rp_view_by_id_decl(30, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(zc && zc != v && zc->of->d.d.target == NVN_TEXTURE_TARGET_CUBEMAP && zc->of->d.d.width == 1 && zc->of->d.d.depth == 6);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 + 1 && nvn_unimpl_count("tex", "cube_view_of_2d_array") == cube_unimpl);
    CHECK(g_mm_lines - ml0 == 1 && strstr(g_mm_last, "声明目标 8，贴图是 3（8x4x1）"));   // 日志按 NVN 原目标打：贴图是 3（一维数组），不是折后的 4
    CHECK(rp_view_by_id_decl(30, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 0, &g_fake_cmd, NULL) == zc && nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 + 2 && g_mm_lines - ml0 == 1);   // 同一组合：计数 +1、不再打行
    nvn_gpu_view z3 = rp_view_by_id_decl(30, NVN_TEXTURE_TARGET_3D + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(z3 && z3 != v && z3->of->d.d.target == NVN_TEXTURE_TARGET_3D && z3->of->d.d.width == 1 && z3->of->d.d.depth == 1);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 + 3 && g_mm_lines - ml0 == 2 && strstr(g_mm_last, "声明目标 2，贴图是 3（8x4x1）"));   // 新组合：再打一行
    // ⑥ 三维贴图被声明成一维数组：折成二维数组，三维存储接不上 → 真不符，零贴图是二维数组（不是后端建不出的一维数组）
    tex_t c3 = mk_tex(8104, 0x40000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); c3.d.target = NVN_TEXTURE_TARGET_3D; c3.d.depth = 4; nvn_tex_layout(&c3.d, &c3.L);
    nvn_be_reg_tex(33, &c3, NULL);
    nvn_gpu_view za = rp_view_by_id_decl(33, NVN_TEXTURE_TARGET_1D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(za && za->of->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && za->of->d.d.width == 1 && za->of->d.d.depth == 1 && nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 + 4);
    CHECK(g_mm_lines - ml0 == 3 && strstr(g_mm_last, "声明目标 3，贴图是 2（4x4x4）"));
    CHECK(nvn_unimpl_count("tex", "decl_zero_create") == 0);
    // ⑦ 登记视图记录把一维数组（4 层）指成二维：按二维数组读时层数取贴图自身的 4 层（一维数组的层数在 height）
    tex_t e = mk_tex(8105, 0x50000, NVN_FORMAT_RGBA8, 8, 4, 1, 0); e.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&e.d, &e.L);
    view_t as2d = {0}; as2d.set_target = 1; as2d.target = NVN_TEXTURE_TARGET_2D;
    nvn_be_reg_tex(34, &e, &as2d);
    CHECK(rp_view_by_id_decl(34, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL) != NULL);
    CHECK(g_vw_last_has && g_vw_last.set_target && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.set_layers && g_vw_last.nlayers == 4 && g_vw_last.min_layer == 0);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 + 4);
    // ⑧ 每种组合首次各打一行，最多 16 行：再来 20 种宽度不同的一维数组被声明成三维（前面已有 3 种）→ 总共 16 行，计数照涨 20
    for (unsigned i = 0; i < 20; i++) {
        tex_t w = mk_tex(8200 + i, 0x60000, NVN_FORMAT_RGBA8, 9 + i, 2, 1, 0); w.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&w.d, &w.L);
        nvn_be_reg_tex(40 + (int)i, &w, NULL);
        CHECK(rp_view_by_id_decl(40 + (int)i, NVN_TEXTURE_TARGET_3D + 1, 0, 0, &g_fake_cmd, NULL) == z3);
    }
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 + 24 && g_mm_lines - ml0 == 16);
    // ⑨（评审补充 1）登记视图记录自己带一维数组目标（第 1 层起 2 层），贴图是一维数组（4 层）：视图的目标也按同一口径折。
    //   声明二维数组 → 折后相等，不记不符；视图记录的层范围 1+2 原样交给后端，后端存储是 8×1×4 的二维数组（层数从 height 来）
    const uint64_t mm9 = nvn_unimpl_count("tex", "decl_type_mismatch");
    tex_t xa = mk_tex(8301, 0x70000, NVN_FORMAT_RGBA8, 8, 4, 1, 0); xa.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&xa.d, &xa.L);
    view_t ra = {0}; ra.set_target = 1; ra.target = NVN_TEXTURE_TARGET_1D_ARRAY; ra.set_layers = 1; ra.min_layer = 1; ra.nlayers = 2;
    nvn_be_reg_tex(60, &xa, &ra);
    nvn_gpu_view va = rp_view_by_id_decl(60, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(va && va->has_v && va->v.set_layers && va->v.min_layer == 1 && va->v.nlayers == 2);
    CHECK(va && va->of->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && va->of->d.d.width == 8 && va->of->d.d.height == 1 && va->of->d.d.depth == 4);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm9);
    // ⑩ 同一张贴图 + 同一条视图记录，声明二维（单层）：折后是二维数组对二维，要改记录：目标二维、层数 1，起点仍是记录里的第 1 层
    nvn_gpu_view vb = rp_view_by_id_decl(60, NVN_TEXTURE_TARGET_2D + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(vb && va && vb != va && vb->of == va->of);
    CHECK(g_vw_last_has && g_vw_last.set_target && g_vw_last.target == NVN_TEXTURE_TARGET_2D && g_vw_last.set_layers && g_vw_last.min_layer == 1 && g_vw_last.nlayers == 1);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm9);
    // ⑪ 登记视图记录自己带一维目标（0），贴图是一维（宽 16）：声明二维 → 16×1 的二维存储；声明二维数组 → 单层二维数组视图
    tex_t xd = mk_tex(8302, 0x80000, NVN_FORMAT_RGBA8, 16, 1, 1, 0); xd.d.target = NVN_TEXTURE_TARGET_1D; nvn_tex_layout(&xd.d, &xd.L);
    view_t rd = {0}; rd.set_target = 1; rd.target = NVN_TEXTURE_TARGET_1D;
    nvn_be_reg_tex(61, &xd, &rd);
    nvn_gpu_view vc = rp_view_by_id_decl(61, NVN_TEXTURE_TARGET_2D + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(vc && vc->has_v && vc->of->d.d.target == NVN_TEXTURE_TARGET_2D && vc->of->d.d.width == 16 && vc->of->d.d.height == 1 && vc->of->d.d.depth == 1);
    nvn_gpu_view vd = rp_view_by_id_decl(61, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(vd && vc && vd != vc && vd->of == vc->of);
    CHECK(g_vw_last_has && g_vw_last.set_target && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.set_layers && g_vw_last.nlayers == 1);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm9);
    // 负对照：视图记录是一维数组、声明三维 → 仍是真不符：记账、绑声明类型的零贴图
    CHECK(rp_view_by_id_decl(60, NVN_TEXTURE_TARGET_3D + 1, 0, 0, &g_fake_cmd, NULL) == z3 && nvn_unimpl_count("tex", "decl_type_mismatch") == mm9 + 1);
    // ⑫（评审补充 3）别名表的键仍是登记描述：同池同偏移、登记描述相同的两个一维数组对象共用一份后端存储，
    //   解析两次、换个对象再解析，拿到的都是同一个后端对象（后端那份是折后的二维数组）；登记描述不同的（二维数组 8×1×4，正是一维数组折后的样子）不共用
    const int c12 = g_tex_creates, r12 = g_tex_releases;
    tex_t xe = mk_tex(8401, 0x90000, NVN_FORMAT_RGBA8, 8, 4, 1, 0); xe.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&xe.d, &xe.L);
    tex_t xf = xe; xf.serial = 8402;
    tex_t xg = mk_tex(8403, 0x90000, NVN_FORMAT_RGBA8, 8, 1, 1, 0); xg.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; xg.d.depth = 4; nvn_tex_layout(&xg.d, &xg.L);
    nvn_gpu_tex ge1 = rp_tex_resolve(&xe, &g_fake_cmd), ge2 = rp_tex_resolve(&xe, &g_fake_cmd), ge3 = rp_tex_resolve(&xf, &g_fake_cmd);
    CHECK(ge1 && ge1 == ge2 && ge1 == ge3 && g_tex_creates == c12 + 1);
    CHECK(ge1 && ge1->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && ge1->d.d.width == 8 && ge1->d.d.height == 1 && ge1->d.d.depth == 4);
    nvn_gpu_tex ge4 = rp_tex_resolve(&xg, &g_fake_cmd);
    CHECK(ge4 && ge4 != ge1 && g_tex_creates == c12 + 2 && g_tex_releases == r12);
    rp_view_pin_end();
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    res_fresh();
}

// 声明类型决定视图；期望目标、层数均直接写常量。
static void t_decl_view(void) {
    nvn_be_sub_begin(); res_fresh(); nvn_unimpl_reset();
    const struct { int target; uint32_t depth; uint8_t dtt; int modified, want; uint16_t layers; } cases[] = {
        {NVN_TEXTURE_TARGET_2D, 1, NVN_TEXTURE_TARGET_2D + 1, 0, 1, 1},
        {NVN_TEXTURE_TARGET_2D_ARRAY, 12, NVN_TEXTURE_TARGET_2D + 1, 1, 1, 1},
        {NVN_TEXTURE_TARGET_CUBEMAP, 6, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 1, 4, 6},
        {NVN_TEXTURE_TARGET_CUBEMAP, 6, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY + 1, 1, 9, 6},
        {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 8, 6},
        {NVN_TEXTURE_TARGET_2D_ARRAY, 12, 0, 0, 4, 12},
        // P0-6：方形、≥6 层的二维数组当立方体 / 立方体数组读：照 iOS 建立方体视图（后端给这种图建立方体兼容），不再绑零贴图
        {NVN_TEXTURE_TARGET_2D_ARRAY, 12, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 8, 6},
        {NVN_TEXTURE_TARGET_2D_ARRAY, 12, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY + 1, 1, 9, 12},
    };
    rp_view_pin_begin();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tex_t t = mk_tex(7000 + i, (int64_t)i * 0x1000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
        t.d.target = cases[i].target; t.d.depth = cases[i].depth; nvn_tex_layout(&t.d, &t.L);
        nvn_be_reg_tex((int)i, &t, NULL);
        tex_t snap;
        nvn_gpu_view v = rp_view_by_id_decl((int)i, cases[i].dtt, 0, 0, &g_fake_cmd, &snap);
        CHECK(v && snap.serial == 7000 + i);
        CHECK(g_vw_last_has == cases[i].modified);
        if (cases[i].modified) CHECK(g_vw_last.set_target && g_vw_last.target == cases[i].want &&
            g_vw_last.set_layers && g_vw_last.nlayers == cases[i].layers && g_vw_last.min_layer == 0);
        else CHECK(v == rp_view_by_id((int)i, &g_fake_cmd, NULL));
    }
    CHECK(nvn_unimpl_count("tex", "cube_view_of_2d_array") == 0);
    // 负对照：非方形（4x2）的 6 层二维数组不能立方体兼容，仍记 cube_view_of_2d_array、绑零贴图
    { tex_t ns = mk_tex(7020, 0x30000, NVN_FORMAT_RGBA8, 4, 2, 1, 0); ns.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; ns.d.depth = 6; nvn_tex_layout(&ns.d, &ns.L);
      nvn_be_reg_tex(20, &ns, NULL);
      nvn_gpu_view zn = rp_view_by_id_decl(20, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 0, &g_fake_cmd, NULL);
      CHECK(zn && zn->of->d.d.width == 1 && nvn_unimpl_count("tex", "cube_view_of_2d_array") == 1); }
    // 从已有子视图继续适配：保留 mip、起始层、格式与通道映射。
    tex_t t = mk_tex(7010, 0x10000, NVN_FORMAT_RGBA8, 8, 8, 3, 0);
    t.d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; t.d.depth = 12; nvn_tex_layout(&t.d, &t.L);
    view_t sub = {0}; sub.set_layers = 1; sub.min_layer = 6; sub.nlayers = 6;
    sub.set_levels = 1; sub.base_level = 1; sub.nlevels = 2;
    sub.set_format = 1; sub.format = NVN_FORMAT_RGBA8; sub.set_swizzle = 1; sub.swz[0] = 3;
    nvn_be_reg_tex(10, &t, &sub);
    CHECK(rp_view_by_id_decl(10, NVN_TEXTURE_TARGET_2D + 1, 0, 0, &g_fake_cmd, NULL) != NULL);
    CHECK(g_vw_last.target == 1 && g_vw_last.nlayers == 1 && g_vw_last.min_layer == 6 &&
        g_vw_last.base_level == 1 && g_vw_last.nlevels == 2 && g_vw_last.set_format &&
        g_vw_last.format == NVN_FORMAT_RGBA8 && g_vw_last.set_swizzle && g_vw_last.swz[0] == 3);
    // 单层二维占位绑定 uint 零立方体；同类及数组元素共用，不报红。
    t = mk_tex(7011, 0x14000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); nvn_be_reg_tex(11, &t, NULL);
    int creates = g_tex_creates, red = g_red_lines; g_nup = 0;
    nvn_gpu_view z = rp_view_by_id_decl(11, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL);
    CHECK(z && z->of->d.d.width == 1 && z->of->d.d.height == 1 && z->of->d.d.depth == 6 &&
        z->of->d.d.target == 8 && z->of->d.d.format == NVN_FORMAT_R32UI && z->has_v &&
        g_vw_last.set_target && g_vw_last.target == 8 && g_vw_last.set_layers && g_vw_last.nlayers == 6);
    CHECK(g_tex_creates == creates + 1 && g_nup == 6 && g_red_lines == red);
    for (int layer = 0; layer < 6 && g_nup == 6; layer++) CHECK(g_up[layer].layer == (uint32_t)layer &&
        g_up[layer].pitch == 4 && g_up[layer].img == 4 && g_up[layer].first[0] == 0 &&
        g_up[layer].first[1] == 0 && g_up[layer].first[2] == 0 && g_up[layer].first[3] == 0);
    CHECK(rp_view_by_id_decl(11, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL) == z);
    CHECK(rp_view_by_id_decl(11, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 1, &g_fake_cmd, NULL) == z);
    CHECK(rp_view_by_id_decl(-1, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 1, &g_fake_cmd, NULL) == z);
    CHECK(g_tex_creates == creates + 1 && g_nup == 6 && g_red_lines == red);
    // #1147 空槽：单槽取池容量内一个没登记的号（句柄 0 → tex_base + 0 落在驱动保留区、TexturePoolInitialize 不写）→ 绑同一张声明类型零贴图、
    //   不跳整笔、不报红，只记信息计数；号越过池容量、负号、池容量未知（没登记池）仍取不到 → 调用方照旧报 texture_slot_unresolved
    nvn_be_set_tex_pool_n(64);
    uint64_t ez0 = rp_empty_slot_zero_count();
    CHECK(rp_view_by_id_decl(40, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL) == z);
    CHECK(rp_view_by_id_decl(41, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL) == z);
    CHECK(rp_empty_slot_zero_count() == ez0 + 2 && g_tex_creates == creates + 1 && g_red_lines == red);
    CHECK(rp_view_by_id_decl(64, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL) == NULL);
    CHECK(rp_view_by_id_decl(-1, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL) == NULL);
    nvn_be_set_tex_pool_n(0);
    CHECK(rp_view_by_id_decl(40, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL) == NULL);
    CHECK(rp_empty_slot_zero_count() == ez0 + 2);
    // 不能桥接的不同维度组合（#936）：记账，绑声明类型的零贴图（同 iOS / Ryujinx），不把原视图交下去。
    nvn_gpu_view original = rp_view_by_id(11, &g_fake_cmd, NULL);
    creates = g_tex_creates;
    nvn_gpu_view z3 = rp_view_by_id_decl(11, NVN_TEXTURE_TARGET_3D + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(z3 && z3 != original && z3->of->d.d.target == NVN_TEXTURE_TARGET_3D && z3->of->d.d.format == NVN_FORMAT_RGBA8 &&
        z3->of->d.d.width == 1 && z3->of->d.d.height == 1 && z3->of->d.d.depth == 1 && g_tex_creates == creates + 1);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == 1);
    // #936 那个组合：128×64×64 的 3D 贴图被声明成二维数组读 → 二维数组零贴图；第二次取用复用同一张、不再建
    tex_t t3 = mk_tex(7014, 0x20000, NVN_FORMAT_RGBA8, 128, 64, 1, 0);
    t3.d.target = NVN_TEXTURE_TARGET_3D; t3.d.depth = 64; nvn_tex_layout(&t3.d, &t3.L); nvn_be_reg_tex(14, &t3, NULL);
    nvn_gpu_view orig3 = rp_view_by_id(14, &g_fake_cmd, NULL);
    creates = g_tex_creates;
    nvn_gpu_view za = rp_view_by_id_decl(14, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(za && za != orig3 && za->of->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && za->of->d.d.depth == 1 && g_tex_creates == creates + 1);
    CHECK(rp_view_by_id_decl(14, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL) == za && g_tex_creates == creates + 1);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == 3);
    // 12 层方形二维数组当立方体读（P0-6）：后端给这种图建立方体兼容，照 iOS 建 6 层立方体视图，不绑零贴图。
    // cube_view_of_2d_array 只有上面那个非方形负对照记过 1 次。
    t = mk_tex(7012, 0x18000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.depth = 12; nvn_tex_layout(&t.d, &t.L); nvn_be_reg_tex(12, &t, NULL);
    int vc = g_view_creates;
    nvn_gpu_view cv = rp_view_by_id_decl(12, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL);
    CHECK(cv && cv != z && g_view_creates == vc + 1 && g_vw_last.set_target && g_vw_last.target == NVN_TEXTURE_TARGET_CUBEMAP && g_vw_last.nlayers == 6);
    CHECK(nvn_unimpl_count("tex", "cube_view_of_2d_array") == 1);
    // 登记视图「层数 0 = 从起点取到底」：12 层立方体数组从第 6 层起，当二维数组读取 6 层
    t = mk_tex(7013, 0x1c000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    t.d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; t.d.depth = 12; nvn_tex_layout(&t.d, &t.L);
    view_t rest = {0}; rest.set_layers = 1; rest.min_layer = 6; rest.nlayers = 0; nvn_be_reg_tex(13, &t, &rest);
    CHECK(rp_view_by_id_decl(13, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, &g_fake_cmd, NULL) != NULL);
    CHECK(g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.min_layer == 6 && g_vw_last.nlayers == 6);
    rp_view_pin_end();
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    int releases = g_tex_releases, view_releases = g_view_releases;
    rp_res_reset();
    // 贴图：8 例 + 零浮点立方体（7020 负对照，7020 本身不建）+ 7010 + 7011 + 零 uint 立方体 + 零 3D + 7014 + 零二维数组 + 7012 + 7013 = 17；
    // 视图：未改造 2 + 改造 6 + 零浮点立方体 + 7010 + 7011 原样 + 零 uint 立方体 + 零 3D + 7014 原样 + 零二维数组 + 7012 立方体 + 7013 = 17
    CHECK(g_tex_releases == releases + 17 && g_view_releases == view_releases + 17);
}

// 数组各元素共享着色器声明：整数视图不能作为浮点元素交给后端。不匹配的元素绑声明类型的零贴图（整笔照画），具名记账；类型相符的元素照常取登记的视图。
static void t_decl_array_numeric(void) {
    nvn_be_sub_begin(); res_fresh(); nvn_unimpl_reset(); nvn_be_set_tex_pool_n(8); rp_view_pin_begin();
    tex_t u = mk_tex(7091, 0, NVN_FORMAT_RG16UI, 4, 4, 1, 0);
    tex_t f = mk_tex(7092, 0x1000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    view_t uv = {0}; uv.set_format = 1; uv.format = NVN_FORMAT_RGBA8UI;
    nvn_be_reg_tex(1, &u, NULL); nvn_be_reg_tex(2, &f, &uv); nvn_be_reg_tex(3, &f, NULL);
    uint8_t dtt = NVN_TEXTURE_TARGET_2D + 1;
    nvn_gpu_view zf = rp_view_by_id_decl(1, dtt, 0, 1, &g_fake_cmd, NULL);   // uint 贴图进了声明成浮点的数组
    CHECK(zf && zf->of->d.d.width == 1 && zf->of->d.d.height == 1 && zf->of->d.d.format == NVN_FORMAT_RGBA8);   // 拿到的是浮点的 1x1 零贴图，不是登记的 4x4 整数贴图
    tex_t snap; memset(&snap, 0x5A, sizeof snap);
    CHECK(rp_view_by_id_decl(2, dtt, 0, 1, &g_fake_cmd, &snap) == zf && snap.serial == 0);   // 视图把格式改成整数的同理：同一张零贴图；调用方的快照清零，不留下脏字节
    CHECK(nvn_unimpl_count("tex", "sampled_array_numeric_mismatch") == 2);   // 每个不匹配的元素记一次
    nvn_gpu_view zs = rp_view_by_id_decl(1, dtt, 2, 1, &g_fake_cmd, NULL);   // uint 贴图进了声明成 sint 的数组：零贴图跟声明走（R32I）
    CHECK(zs && zs != zf && zs->of->d.d.width == 1 && zs->of->d.d.format == NVN_FORMAT_R32I);
    CHECK(nvn_unimpl_count("tex", "sampled_array_numeric_mismatch") == 3);
    // 负对照：类型相符的元素拿登记的那张，不记账
    nvn_gpu_view real = rp_view_by_id_decl(1, dtt, 1, 1, &g_fake_cmd, &snap);
    CHECK(real && real != zf && real != zs && real->of->d.d.width == 4 && snap.serial == 7091);
    nvn_gpu_view rf = rp_view_by_id_decl(3, dtt, 0, 1, &g_fake_cmd, NULL);
    CHECK(rf && rf != zf && rf->of->d.d.width == 4);
    CHECK(nvn_unimpl_count("tex", "sampled_array_numeric_mismatch") == 3);
    // 负对照：没有命令段时零贴图建不出来，返回空（调用方照旧按 texture_slot_unresolved 跳笔），仍记账
    CHECK(!rp_view_by_id_decl(1, NVN_TEXTURE_TARGET_3D + 1, 0, 1, NULL, NULL));
    CHECK(nvn_unimpl_count("tex", "sampled_array_numeric_mismatch") == 4);
    rp_view_pin_end(); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); res_fresh();
}

// 深度模板贴图按哪个面采样（#1679 斑马纹的根因）：建贴图时设了模板模式（TextureBuilderSetDepthStencilMode）、视图没设模式时，
// 交给后端的采样视图记录要带模板模式；视图自己设了模式就取视图的（同 iOS 层 nvn_mtl.m tex_by_id 的规则）。
// 同一块存储上的深度模式贴图与模板模式贴图在同存储共用表里是同一份后端存储，区别只在视图记录。
static void t_stencil_sample_mode(void) {
    nvn_be_sub_begin(); res_fresh(); nvn_unimpl_reset(); nvn_be_set_tex_pool_n(8); rp_view_pin_begin();
    const int ds = NVN_FORMAT_DEPTH24_STENCIL8;
    tex_t dep = mk_tex(8101, 0, ds, 16, 16, 1, 0);                    // 深度缓冲本体：深度模式
    tex_t stn = mk_tex(8102, 0, ds, 16, 16, 1, 0); stn.stencil = 1;   // 同池同偏移同描述的第二个对象：建贴图时设了模板模式
    nvn_be_reg_tex(1, &dep, NULL); nvn_be_reg_tex(2, &stn, NULL);
    int c0 = g_tex_creates;
    nvn_gpu_view vd = rp_view_by_id(1, &g_fake_cmd, NULL), vs = rp_view_by_id(2, &g_fake_cmd, NULL);
    CHECK(vd && vs && vd != vs && vd->of == vs->of && g_tex_creates - c0 == 1);   // 一份后端存储、两个视图
    CHECK(!vd->has_v);                                                           // 深度模式：不带视图记录，后端取深度面
    CHECK(vs->has_v && vs->v.set_dsmode && vs->v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);   // 模板模式：记录带模板
    // 模板模式的贴图是存储的第一个使用者：存储照常建，建存储时不带模板标志（取哪个面由视图记录决定；描述里原 stencil_view 那一字节现在是保留位，恒 0）
    tex_t first = mk_tex(8103, 0x4000, ds, 16, 16, 1, 0); first.stencil = 1; nvn_be_reg_tex(3, &first, NULL);
    nvn_gpu_view vf = rp_view_by_id(3, &g_fake_cmd, NULL);
    CHECK(vf && vf->of != vs->of && vf->of->d._rsv[0] == 0 && vf->of->d._rsv[1] == 0 && vf->has_v && vf->v.set_dsmode && vf->v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    // 视图自己设了模式：取视图的（贴图是模板模式、视图设深度 → 深度；贴图是深度模式、视图设模板 → 模板）
    view_t vdep = {0}; vdep.set_dsmode = 1; vdep.dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    view_t vstn = {0}; vstn.set_dsmode = 1; vstn.dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
    tex_t sd = mk_tex(8104, 0x8000, ds, 16, 16, 1, 0); sd.stencil = 1; nvn_be_reg_tex(4, &sd, &vdep);
    tex_t ds2 = mk_tex(8105, 0xC000, ds, 16, 16, 1, 0); nvn_be_reg_tex(5, &ds2, &vstn);
    nvn_gpu_view v4 = rp_view_by_id(4, &g_fake_cmd, NULL), v5 = rp_view_by_id(5, &g_fake_cmd, NULL);
    CHECK(v4 && v4->has_v && v4->v.set_dsmode && v4->v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH);
    CHECK(v5 && v5->has_v && v5->v.set_dsmode && v5->v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    // 视图记录里别的设定（只取第 0 级）原样保留，只补模式
    view_t lv = {0}; lv.set_levels = 1; lv.base_level = 0; lv.nlevels = 1;
    tex_t sl = mk_tex(8106, 0x10000, ds, 16, 16, 1, 0); sl.stencil = 1; nvn_be_reg_tex(6, &sl, &lv);
    nvn_gpu_view v6 = rp_view_by_id(6, &g_fake_cmd, NULL);
    CHECK(v6 && v6->has_v && v6->v.set_levels && v6->v.nlevels == 1 && v6->v.set_dsmode && v6->v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    // 声明目标与贴图不同、要改写视图的路径（12 层二维数组按二维读）：模板模式照样带上
    tex_t arr = mk_tex(8107, 0x14000, ds, 4, 4, 1, 0); arr.stencil = 1;
    arr.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; arr.d.depth = 12; nvn_tex_layout(&arr.d, &arr.L); nvn_be_reg_tex(7, &arr, NULL);
    CHECK(rp_view_by_id_decl(7, NVN_TEXTURE_TARGET_2D + 1, 0, 0, &g_fake_cmd, NULL) != NULL);
    CHECK(g_vw_last_has && g_vw_last.set_target && g_vw_last.target == NVN_TEXTURE_TARGET_2D && g_vw_last.set_layers && g_vw_last.nlayers == 1 &&
          g_vw_last.set_dsmode && g_vw_last.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    // 描述符数组元素：模板的数值型是整数（uint），进了声明成浮点的数组按「数值型不符」处理；声明成 uint 的拿登记的
    uint8_t dtt = NVN_TEXTURE_TARGET_2D + 1;
    nvn_gpu_view z = rp_view_by_id_decl(2, dtt, 0, 1, &g_fake_cmd, NULL);
    CHECK(z && z->of->d.d.width == 1 && z->of->d.d.height == 1 && nvn_unimpl_count("tex", "sampled_array_numeric_mismatch") == 1);
    CHECK(rp_view_by_id_decl(2, dtt, 1, 1, &g_fake_cmd, NULL) == vs && nvn_unimpl_count("tex", "sampled_array_numeric_mismatch") == 1);
    // 负对照：同一块存储上深度模式的贴图声明成浮点，拿登记的、不记账；颜色贴图不带视图记录
    CHECK(rp_view_by_id_decl(1, dtt, 0, 1, &g_fake_cmd, NULL) == vd && nvn_unimpl_count("tex", "sampled_array_numeric_mismatch") == 1);
    tex_t col = mk_tex(8108, 0x18000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); nvn_be_reg_tex(0, &col, NULL);
    nvn_gpu_view vc = rp_view_by_id(0, &g_fake_cmd, NULL);
    CHECK(vc && !vc->has_v);
    rp_view_pin_end(); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); res_fresh();
}

static void t_buf_mirror_fail_once(void) {   // 池镜像建不出来：同一个池只尝试一次，之后直接按未接返回；池释放再登记会重试
    nvn_be_sub_begin();
    nvn_be_shutdown_session(); nvn_unimpl_reset(); g_pg_tracked = 1; g_nbw = 0;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; int c0 = g_buf_creates;
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    g_buf_fail_bytes = sizeof g_pmem;
    nvn_gpu_buf b = NULL; uint64_t o = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 64, 16, &b, &o) == 0);
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 128, 16, &b, &o) == 0);
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG, 32, &b, &o) == 0);
    CHECK(g_buf_creates - c0 == 1);                                         // 三次解析只试建一次
    CHECK(nvn_unimpl_count("gpu", "buf_create_pool_mirror") == 3);         // 每次照样记未接（计数），不再重建
    nvn_be_pool_del(g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);         // 池释放再登记：失败记录随之删掉
    g_buf_fail_bytes = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 64, 16, &b, &o) == 1 && b && o == 64);   // 同一地址重新使用：重试并建成
    CHECK(g_buf_creates - c0 == 2);
    nvn_be_shutdown_session();
}
static void t_buf_mirror(void) {   // 池镜像：首次使用建一份、按页同步；没写过的页不重传；按使用点的先后保证 GPU 顺序
    nvn_be_sub_begin();
    nvn_be_shutdown_session(); nvn_unimpl_reset(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_nbw = 0; g_dirty_now = 100;
    for (unsigned i = 0; i < sizeof g_pmem; i++) g_pmem[i] = (uint8_t)(i / PG + 1);   // 第 k 页全是 k+1
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; int c0 = g_buf_creates, r0 = g_buf_releases;
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    CHECK(g_buf_creates == c0);                                     // 登记池不建镜像
    nvn_gpu_buf b1 = NULL, b2 = NULL; uint64_t o1 = 0, o2 = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG + 100, 200, &b1, &o1) == 1);
    CHECK(g_buf_creates - c0 == 1 && g_buf_bytes == sizeof g_pmem && o1 == PG + 100);
    CHECK(g_nbw == 1 && g_bw[0].off == PG && g_bw[0].len == PG && g_bw[0].head[0] == 2 && g_bw[0].pre == 1);   // 只同步用到的那一页（整页）；传输录进前置段
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG + 300, 50, &b2, &o2) == 1 && b2 == b1 && g_nbw == 1);   // 同页、没写过：不重传
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG - 8, 2 * PG, NULL, NULL) == 1);             // 跨第 0–2 页：第 1 页已同步，补第 0 页和第 2 页
    CHECK(g_nbw == 3 && g_bw[1].off == 0 && g_bw[1].len == PG && g_bw[2].off == 2 * PG && g_bw[2].len == PG);
    g_pg_gen[1] = 101; g_dirty_now = 102;                           // 游戏写了第 1 页
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 4 && g_bw[3].off == PG && g_bw[3].len == PG && g_bw[3].pre == 0);   // 本次提交里的第二次同步：主段
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 4);       // 再用：干净
    CHECK(rp_buf_resolve(&g_fake_cmd, base + sizeof g_pmem - 4, 8, NULL, NULL) == 0 && nvn_unimpl_count("buf", "address_outside_pools") == 1);   // 越过池尾
    g_pg_tracked = 0; g_dirty_map++;                                // 没被写跟踪（跟踪的区间变了，代际跟着变）：每次使用都同步
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 100, NULL, NULL) == 1 && rp_buf_resolve(&g_fake_cmd, base, 100, NULL, NULL) == 1 && g_nbw == 6);
    g_pg_tracked = 1; g_dirty_map++;
    // 快路径：写代号没动时，同步过的页直接用；代号一动（游戏写过任何被跟踪的页）就回到逐页核对
    { int n0 = g_nbw; CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1); int n1 = g_nbw;   // 代际变过：逐页核对一遍（第 0 页在没跟踪时传过，代号比页上的新，不重传）
      CHECK(rp_buf_resolve(&g_fake_cmd, base + 10, 2 * PG, NULL, NULL) == 1 && g_nbw == n1);
      g_pg_gen[2] = g_dirty_now + 1; g_dirty_now += 2;
      CHECK(rp_buf_resolve(&g_fake_cmd, base + 2 * PG, 16, NULL, NULL) == 1 && g_nbw == n1 + 1 && g_bw[(n1 < 32 ? n1 : 31)].off == 2 * PG); (void)n0; }
    // 反例（真机上镜头一动几何全碎的那个次序）：第 2 页写了之后，先用了别的页，再用第 2 页——不能因为「刚核对过、代号没动」就放过它
    { g_pg_gen[2] = g_dirty_now + 1; g_dirty_now += 2; int n2 = g_nbw;
      CHECK(rp_buf_resolve(&g_fake_cmd, base, 16, NULL, NULL) == 1 && g_nbw == n2);                       // 第 0 页：干净
      CHECK(rp_buf_resolve(&g_fake_cmd, base + 2 * PG + 8, 16, NULL, NULL) == 1 && g_nbw == n2 + 1);     // 第 2 页：脏，必须传
      CHECK(rp_buf_resolve(&g_fake_cmd, base + 2 * PG + 8, 16, NULL, NULL) == 1 && g_nbw == n2 + 1); }   // 再用：干净
    CHECK(rp_buf_mirrors() == 1);
    nvn_be_pool_del(g_pmem);
    CHECK(g_buf_releases - r0 == 1 && rp_buf_mirrors() == 0);       // 池销毁：镜像放掉
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

static void t_buf_multi_tail(void) {
    nvn_be_sub_begin(); nvn_be_shutdown_session();
    memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_nbw = 0;
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    const int counts[] = {0, 1, 16, 17, 64, 65};
    for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        uint64_t addr[66], len[66], off[66]; nvn_gpu_buf buf[66]; unsigned char ok[66];
        memset(ok, 0xa5, sizeof ok); memset(buf, 0, sizeof buf); memset(off, 0xff, sizeof off);
        for (int i = 0; i < 66; i++) { addr[i] = base + (uint64_t)i * 16; len[i] = 8; }
        // Valid, zero-length and pool-tail failures on both sides of a chunk boundary.
        len[15] = len[31] = 0;
        addr[16] = addr[48] = base + sizeof g_pmem - 4;
        CHECK(rp_buf_resolve_multi(counts[c], addr, len, buf, off, ok) == 1);
        for (int i = 0; i < counts[c]; i++) {
            int valid = i != 15 && i != 16 && i != 31 && i != 48;
            CHECK(ok[i] == valid);
            if (valid) CHECK(buf[i] != NULL && off[i] == (uint64_t)i * 16);
        }
        CHECK(ok[counts[c]] == 0xa5);
    }
    nvn_be_pool_del(g_pmem); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// 脏页追加日志（nvk_buf_dirtylog，#1501 A②）：解析不按范围逐页比代号，只灌日志事件、在旧页位图里求交。
// 修前（没有这条路）：开关开着也走逐页路径，「全池已同步、只有别的池写了」时整段 256 页逐页查。
static uint64_t sync_bytes_now(void) { extern void rp_buf_stats(uint64_t *, uint64_t *); uint64_t b = 0, r = 0; rp_buf_stats(&b, &r); return b; }
static void t_buf_dirtylog(void) {
    extern uint64_t rp_buf_gpu_write_note(uint64_t, uint64_t, uint64_t *);
    extern void rp_buf_gpu_write_land(uint64_t, uint64_t, int);
    extern void rp_buf_frame_flip(void);
    nvn_be_shutdown_session(); nvn_unimpl_reset(); g_pg_tracked = 1; g_buf_cpu_auto = 1;
    g_flag_buf_dirtylog = 1; g_log_fake_on = 1; g_log_fake_bad = 0; g_log_head = 0; g_log_ident++; memset(g_log_unarmed, 0, sizeof g_log_unarmed);
    g_dirty_now = 100; g_nbw = 0;
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    memset(g_block_mem, 0x21, BLOCK_TEST_PAGES * PG); memset(g_block_gen, 0, sizeof g_block_gen);
    ++g_dirty_map; g_block_unreliable = 0;
    const uint64_t base = (uint64_t)(uintptr_t)g_block_mem, all = BLOCK_TEST_PAGES * PG;
    nvn_be_pool_add(g_block_mem, all);
    nvn_gpu_buf b = NULL;
    // 新镜像：整份置旧，第一次解析整段同步一次
    nvn_be_sub_begin(); g_block_pages_examined = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1 && b && b->cpu);
    if (b && b->cpu) CHECK(!memcmp(b->cpu, g_block_mem, all));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_gpu_buf m0 = b;
    // 只有别的池写了：本池一页都不查、一个字节都不传（修前逐页 256 页）
    nvn_be_sub_begin(); ++g_dirty_now; g_block_pages_examined = 0; uint64_t sb0 = sync_bytes_now();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1);
    printf("dirtylog unrelated: examined=%llu synced=%llu expected 0/0\n", (unsigned long long)g_block_pages_examined, (unsigned long long)(sync_bytes_now() - sb0));
    CHECK(g_block_pages_examined == 0 && sync_bytes_now() == sb0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 写一页：只同步这一页，内容对
    fake_log_write(70, 0x7b);
    nvn_be_sub_begin(); g_block_pages_examined = 0; sb0 = sync_bytes_now();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1);
    CHECK(g_block_pages_examined == 0 && sync_bytes_now() - sb0 == PG);
    if (b && b->cpu) CHECK(!memcmp(b->cpu, g_block_mem, all));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 写两页、只解析前一页：后一页的旧位留着，之后解析它照样同步
    fake_log_write(80, 0x38); fake_log_write(81, 0x49);
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 80 * PG, PG, &b, NULL) == 1);
    if (b && b->cpu) CHECK(b->cpu[80 * PG] == 0x38 && b->cpu[81 * PG] == 0x21);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 81 * PG, PG, &b, NULL) == 1);
    if (b && b->cpu) CHECK(b->cpu[81 * PG] == 0x49);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 两份镜像各有游标与位图：第 0 份同步了第 90 页之后换到第 1 份写第 91 页，换回来时第 0 份必须补上第 91 页
    fake_log_write(90, 0x5a);
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1 && b == m0); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    rp_buf_frame_flip();
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1 && b && b != m0);
    if (b && b->cpu) CHECK(!memcmp(b->cpu, g_block_mem, all));   // 新的第 1 份：整份同步
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    fake_log_write(91, 0x5b);
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1); if (b && b->cpu) CHECK(b->cpu[91 * PG] == 0x5b); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    rp_buf_frame_flip();
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1 && b == m0);
    if (b && b->cpu) CHECK(b->cpu[90 * PG] == 0x5a && b->cpu[91 * PG] == 0x5b);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 加保护被并发解保护挡住（此刻不是只读）：同步了也不能清位——之后那次没缺页的写下一次解析照样要同步
    g_log_unarmed[100] = 1; fake_log_write(100, 0x64);
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1); if (b && b->cpu) CHECK(b->cpu[100 * PG] == 0x64); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    memset(g_block_mem + 100 * PG, 0x65, PG); g_block_gen[100] = ++g_dirty_now;   // 页还可写：这次写不缺页、不进日志
    g_log_unarmed[100] = 0;
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1); if (b && b->cpu) CHECK(b->cpu[100 * PG] == 0x65); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 环绕：第 130 页的事件被后面 70 条覆盖，读者判不可用 → 按逐页代号重建，第 130 页不能漏
    fake_log_write(130, 0x13);
    for (int i = 0; i < FAKE_LOG_CAP + 6; i++) fake_log_write(120, (uint8_t)(0x40 + i));
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1);
    if (b && b->cpu) CHECK(b->cpu[130 * PG] == 0x13 && b->cpu[120 * PG] == g_block_mem[120 * PG] && !memcmp(b->cpu, g_block_mem, all));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 重建之后照常只按事件走
    nvn_be_sub_begin(); ++g_dirty_now; g_block_pages_examined = 0; sb0 = sync_bytes_now();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1 && g_block_pages_examined == 0 && sync_bytes_now() == sb0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 待落地的 GPU 写：CPU 同步不得覆盖 GPU 写进镜像、还没落地的字节
    nvn_gpu_buf written = b; uint64_t key = 0;
    uint64_t id = rp_buf_gpu_write_note(base + 200 * PG, PG, &key);
    CHECK(id && key == base && written && written->cpu);
    if (written && written->cpu) memset(written->cpu + 200 * PG, 0xee, PG);
    fake_log_event(200, 200); g_block_gen[200] = ++g_dirty_now;
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1 && b == written);
    if (b && b->cpu) CHECK(b->cpu[200 * PG] == 0xee);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    if (id) rp_buf_gpu_write_land(key, id, 1);
    CHECK(g_block_mem[200 * PG] == 0xee);
    // 跟踪不可靠：日志与逐页代号都不可用 → 这份镜像退回逐页路径，不受跟踪就整段同步
    g_block_unreliable = 1; g_block_mem[150 * PG] = 0x71;
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, all, &b, NULL) == 1); if (b && b->cpu) CHECK(b->cpu[150 * PG] == 0x71); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_block_unreliable = 0;
    nvn_be_pool_del(g_block_mem); nvn_be_shutdown_session();
    g_block_mem = NULL; g_buf_cpu_auto = 0; g_flag_buf_dirtylog = g_dirtylog_dflt; g_log_fake_on = 0;
}

// 不受写跟踪的池（安卓虚拟池的映射段）：同一次提交里同步过的页不再重传；回放自己按序改了页、显卡写回落地才让它作废。
// 修前：不受跟踪的池每次使用都整段重传，每次再因「同一次提交里的第二次同步」把当前通道切开
// （真机重场景一帧 1678 次这类解析、池镜像同步 3.09 GB、约 936 次切通道）。
static void t_buf_untracked_once(void) {
    extern uint64_t rp_buf_gpu_write_note(uint64_t, uint64_t, uint64_t *);
    extern void rp_buf_gpu_write_land(uint64_t, uint64_t, int);
    extern void rp_buf_frame_flip(void);
    nvn_be_shutdown_session(); nvn_unimpl_reset(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_nbw = 0; g_dirty_now = 100; g_buf_cpu_auto = 0;
    for (unsigned i = 0; i < sizeof g_pmem; i++) g_pmem[i] = (uint8_t)(i / PG + 1);   // 第 k 页全是 k+1
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    const int tracked = g_pg_tracked; g_pg_tracked = 0; g_dirty_map++;               // 池登记、建镜像时就不在写跟踪里
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    nvn_be_sub_begin();
    uint64_t sb0 = sync_bytes_now();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 100, 200, NULL, NULL) == 1 && g_nbw == 1 && g_bw[0].off == 0 && g_bw[0].len == PG && g_bw[0].head[0] == 1 && g_bw[0].pre == 1);   // 第一次：同步第 0 页，录前置段
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 300, 50, NULL, NULL) == 1 && g_nbw == 1);                     // 同页再用：免传（修前再录一笔主段传输、切通道）
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 2 && g_bw[1].off == PG && g_bw[1].len == 2 * PG && g_bw[1].pre == 1);   // 跨 0–2 页：只补第 1、2 页，仍在前置段
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 2);                       // 再用：全是新的
    CHECK(sync_bytes_now() - sb0 == 3 * PG);                                                               // 一次提交里这三页一共只传一遍
    g_pmem[3] = 0x77;                                                                                      // 游戏之后改了第 0 页（回放不知道）：这次提交里的命令按提交时的内容读，不重传
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 100, NULL, NULL) == 1 && g_nbw == 2);
    CHECK(rp_buf_mirrors() == 1 && nvn_unimpl_kinds() == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 下一次提交：每页重新同步一次，拿到游戏改过的内容；这次提交里再用不重传
    nvn_be_sub_begin(); g_nbw = 0; sb0 = sync_bytes_now();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 100, NULL, NULL) == 1 && g_nbw == 1 && g_bw[0].len == PG && g_bw[0].head[3] == 0x77 && g_bw[0].pre == 1);
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 2 && g_bw[1].off == PG && g_bw[1].len == 2 * PG);   // 第 1、2 页本次提交里第一次同步
    CHECK(sync_bytes_now() - sb0 == 3 * PG);
    // 回放自己按序改了第 0 页（OP_UPDATE_UB 的 CPU 路径）：命令之后的使用点必须看到新内容，只重传被写的页，本次提交里的第二次同步 → 主段
    { uint8_t rec[16 + 4]; const uint64_t d = base + 2, n = 4; memcpy(rec, &d, 8); memcpy(rec + 8, &n, 8); memset(rec + 16, 0x5a, 4); rp_op_update_ub(rec, NULL); }
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 3 && g_bw[2].off == 0 && g_bw[2].len == PG && g_bw[2].head[2] == 0x5a && g_bw[2].pre == 0);
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 3);                       // 再用：干净
    // 写跨两页：两页都作废（相邻的两页合成一串传）
    { uint8_t rec[16 + 4]; const uint64_t d = base + PG - 2, n = 4; memcpy(rec, &d, 8); memcpy(rec + 8, &n, 8); memset(rec + 16, 0x6b, 4); rp_op_update_ub(rec, NULL); }
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 4 && g_bw[3].off == 0 && g_bw[3].len == 2 * PG && g_bw[3].pre == 0);
    // 写落在这个池里没被用到的页（第 5 页）：用第 0–2 页的不受连累
    { uint8_t rec[16 + 4]; const uint64_t d = base + 5 * PG, n = 4; memcpy(rec, &d, 8); memcpy(rec + 8, &n, 8); memset(rec + 16, 0x6c, 4); rp_op_update_ub(rec, NULL); }
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 3 * PG, NULL, NULL) == 1 && g_nbw == 4);                       // 写的是第 5 页，用的是第 0–2 页：不重传
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 5 * PG, 8, NULL, NULL) == 1 && g_nbw == 5 && g_bw[4].off == 5 * PG && g_bw[4].len == PG && g_bw[4].head[0] == 0x6c);   // 第 5 页：本提交没同步过，第一次就是新内容
    CHECK(nvn_unimpl_kinds() == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_pool_del(g_pmem);
#ifdef S2A_GPU_LAND_TEST
    // 显卡写回落地：另一份镜像此前在本次提交里同步这页时，按登记跳过了写回区间，它没有落地的内容——落地之后这几页必须重新同步（受跟踪的池靠落地那次 memcpy 自己变脏）
    g_buf_cpu_auto = 1; memset(g_pmem, 0x11, sizeof g_pmem);
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    uint64_t key = 0; nvn_gpu_buf wr = NULL, other = NULL, again = NULL;
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &wr, NULL) == 1 && wr && wr->cpu);                         // 当前这一份（写它的那份）：同步第 0 页
    const uint64_t id = rp_buf_gpu_write_note(base + 128, 16, &key); CHECK(id && key == base);              // 显卡写这 16 字节（登记在这一份上）
    if (wr && wr->cpu) memset(wr->cpu + 128, 0xee, 16);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    rp_buf_frame_flip();                                                                                   // 下一帧：换成另一份
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 64, &other, NULL) == 1 && other && other != wr);               // 与登记不相交的一段 → 另一份：整页同步，按登记跳过 128..143
    rp_buf_gpu_write_land(key, id, 1);                                                                     // 落地：16 字节拷回游戏内存、撤登记
    CHECK(g_pmem[128] == 0xee && g_pmem[143] == 0xee);
    memset(g_land_mask, 0, sizeof g_land_mask); g_land_capture = 1; g_nbw = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 128, 16, &again, NULL) == 1 && again == other);
    g_land_capture = 0;
    CHECK(g_land_mask[128] && g_land_mask[143] && g_land_staged[128] == 0xee && g_land_staged[143] == 0xee);   // 另一份里这页作废、重新同步，落地的内容进来了
    // 落地发生在写入那一份自己同步过这页的同一次提交里：那一份里本来就是落地的内容（与落地后的游戏内存一致），不作废、不重传
    uint64_t key2 = 0; const uint64_t id2 = rp_buf_gpu_write_note(base + 256, 16, &key2); CHECK(id2 && key2 == base);   // 显卡写当前这一份（other）里这页的 16 字节
    if (other && other->cpu) memset(other->cpu + 256, 0xdd, 16);
    rp_buf_gpu_write_land(key2, id2, 1);
    g_nbw = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 256, 16, NULL, NULL) == 1 && g_nbw == 0);                       // 写入那一份：免传（另一份 wr 才要补）
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_pool_del(g_pmem); rp_buf_frame_flip();
#endif
    nvn_be_shutdown_session(); g_pg_tracked = tracked; g_dirty_map++; g_buf_cpu_auto = 0;
}

static void t_buf_ops(void) {   // 缓冲类命令只改游戏内存；GPU 顺序由使用点同步保证：先用的看到旧内容，后用的看到新内容
    nvn_be_shutdown_session(); nvn_unimpl_reset(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_nbw = 0; g_dirty_now = 100; memset(g_pmem, 0, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(g_pmem, 0xA1, 16);
    stream a = {0};
    uint8_t *r = put(&a, OP_UPDATE_UB, 16 + 8); uint64_t d = base, n = 8; memcpy(r, &d, 8); memcpy(r + 8, &n, 8); memset(r + 16, 0xB2, 8);
    r = put(&a, OP_COPY_B2B, 24); uint64_t sv = base, dv = base + PG; n = 8; memcpy(r, &sv, 8); memcpy(r + 8, &dv, 8); memcpy(r + 16, &n, 8);
    p_clear_buf *cb = put(&a, OP_CLEAR_BUF, sizeof *cb); cb->addr = base + 2 * PG; cb->size = 10; cb->value = 0x11223344;
    r = put(&a, OP_UPDATE_UB, 16 + 8); d = base + sizeof g_pmem - 4; n = 8; memcpy(r, &d, 8); memcpy(r + 8, &n, 8);   // 越过池尾：拒绝
    end(&a);
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 8, NULL, NULL) == 1 && g_nbw == 1 && g_bw[0].head[0] == 0xA1 && g_bw[0].pre == 1);   // 命令之前的使用点：旧内容，录进前置段
    nvn_be_exec(a.b);
    CHECK(g_pmem[0] == 0xB2 && g_pmem[7] == 0xB2 && g_pmem[8] == 0xA1);        // UPDATE_UB
    CHECK(g_pmem[PG] == 0xB2 && g_pmem[PG + 7] == 0xB2 && g_pmem[PG + 8] == 0);  // COPY_B2B 拷的是更新后的内容
    uint8_t want[10] = {0x44, 0x33, 0x22, 0x11, 0x44, 0x33, 0x22, 0x11, 0x44, 0x33};
    CHECK(!memcmp(g_pmem + 2 * PG, want, 10) && g_pmem[2 * PG + 10] == 0);     // CLEAR_BUF：32 位样式，不是按字节
    CHECK(nvn_unimpl_count("buf", "update_outside_pools") == 1);
    CHECK(nvn_unimpl_count("op", "OP_UPDATE_UB") == 0 && nvn_unimpl_count("op", "OP_COPY_B2B") == 0 && nvn_unimpl_count("op", "OP_CLEAR_BUF") == 0);
    g_pg_gen[0] = 101; g_dirty_now = 102;                                       // 写跟踪看到了第 0 页被写
    CHECK(rp_buf_resolve(&g_fake_cmd, base, 8, NULL, NULL) == 1 && g_nbw == 2 && g_bw[1].head[0] == 0xB2 && g_bw[1].pre == 0);   // 命令之后的使用点：新内容；同一页本次提交第二次同步，录进主段（排在已录的绘制之后）
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_pool_del(g_pmem);
}

static void t_copy_b2t_linear(void);
static void t_copy_b2t(void) {   // 缓冲 → 贴图：线性行直接传；级 / 层取自视图；行距可由游戏设；之后内容归 GPU
    nvn_be_shutdown_session(); res_fresh(); nvn_unimpl_reset(); memset(g_pmem, 0, sizeof g_pmem);
    for (int i = 0; i < 64; i++) g_pmem[i] = (uint8_t)(i + 1);
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    tex_t t = mk_tex(51, 0, NVN_FORMAT_RGBA8, 16, 16, 3, 0);
    stream a = {0};
    p_copy_b2t *o = put(&a, OP_COPY_B2T, sizeof *o + sizeof t); tex_t *snap = (tex_t *)(o + 1); *snap = t;
    o->src = (uint64_t)(uintptr_t)g_pmem; o->tex = snap; o->has_view = 1; o->view.set_levels = 1; o->view.base_level = 2;
    o->r = (NVNcopyRegion){1, 0, 0, 2, 2, 1};
    o = put(&a, OP_COPY_B2T, sizeof *o + sizeof t); snap = (tex_t *)(o + 1); *snap = t;
    o->src = (uint64_t)(uintptr_t)g_pmem + 16; o->tex = snap; o->r = (NVNcopyRegion){0, 0, 0, 4, 2, 1}; o->row_stride = 32;
    end(&a);
    nvn_be_sub_begin(); nvn_be_exec(a.b);
    CHECK(g_nup == 5 && nvn_unimpl_count("op", "OP_COPY_B2T") == 0);   // 头 3 次是新建存储的清零（三级各一次），后 2 次是两条拷贝
    CHECK(g_up[0].level == 0 && g_up[0].img == 16 * 16 * 4 && g_up[1].level == 1 && g_up[2].level == 2 && g_up[2].img == 4 * 4 * 4 && g_up[0].first[0] == 0);
    CHECK(g_up[3].level == 2 && g_up[3].layer == 0 && g_up[3].w == 2 && g_up[3].h == 2 && g_up[3].pitch == 8 && g_up[3].img == 16 && g_up[3].first[0] == 1);
    CHECK(g_up[4].level == 0 && g_up[4].pitch == 32 && g_up[4].img == 64 && g_up[4].first[0] == 17);
    CHECK(nvn_be_tex_gpu_owned(51) == 1);
    // 三维贴图一次传多片：从第 1 片起传 3 片，每片 2×2（16 字节），源按片顺排
    tex_t t3 = mk_tex(52, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); t3.d.target = NVN_TEXTURE_TARGET_3D; t3.d.depth = 4; nvn_tex_layout(&t3.d, &t3.L);
    stream a3 = {0}; o = put(&a3, OP_COPY_B2T, sizeof *o + sizeof t3); snap = (tex_t *)(o + 1); *snap = t3;
    o->src = (uint64_t)(uintptr_t)g_pmem; o->tex = snap; o->r = (NVNcopyRegion){0, 0, 1, 2, 2, 3}; end(&a3);
    nvn_be_exec(a3.b);
    CHECK(g_nup == 9 && nvn_unimpl_kinds() == 0 && g_up[5].img == 4 * 4 * 4 * 4 && g_up[5].first[0] == 0);   // 新建的三维存储先清零一次（4 片一起），再是 3 片
    for (int z = 0; z < 3 && g_nup == 9; z++) CHECK(g_up[6 + z].layer == (uint32_t)(1 + z) && g_up[6 + z].img == 16 && g_up[6 + z].w == 2 && g_up[6 + z].first[0] == (uint8_t)(1 + 16 * z));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&t3); nvn_be_tex_release(&t); nvn_be_alias_drop_pool(&g_tp); nvn_be_pool_del(g_pmem);
    t_copy_b2t_linear();   // 线性贴图的同一类拷贝（#734）
}

static void t_copy_b2t_linear(void) {   // 缓冲 → 线性贴图（#734）：宿主贴图不按游戏的内存布局建，拷贝这一侧和块线性的同一条路（iOS copy_b2t 不分线性）
    nvn_be_shutdown_session(); res_fresh(); nvn_unimpl_reset(); memset(g_pmem, 0, sizeof g_pmem);
    for (int i = 0; i < 64; i++) g_pmem[i] = (uint8_t)(i + 1);
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    tex_t t = mk_tex(61, 0, NVN_FORMAT_RGBA8, 8, 4, 1, NVN_TEXTURE_FLAGS_LINEAR);
    tex_t u = mk_tex(62, 0x1000, NVN_FORMAT_RGBA8, 4, 4, 1, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);
    stream a = {0};
    p_copy_b2t *o = put(&a, OP_COPY_B2T, sizeof *o + sizeof t); tex_t *snap = (tex_t *)(o + 1); *snap = t;
    o->src = (uint64_t)(uintptr_t)g_pmem; o->tex = snap; o->r = (NVNcopyRegion){1, 1, 0, 3, 2, 1};   // 紧排：行 3×4 = 12 字节，2 行 24 字节
    o = put(&a, OP_COPY_B2T, sizeof *o + sizeof u); snap = (tex_t *)(o + 1); *snap = u;
    o->src = (uint64_t)(uintptr_t)g_pmem + 16; o->tex = snap; o->r = (NVNcopyRegion){0, 0, 0, 4, 4, 1}; o->row_stride = 64;   // 游戏设的行距 64，4 行 256 字节
    end(&a);
    nvn_be_sub_begin(); nvn_be_exec(a.b);
    CHECK(nvn_unimpl_count("tex", "b2t_linear") == 0 && nvn_unimpl_kinds() == 0);
    CHECK(g_nup == 4);   // 每张新建存储先清零一次（线性的不从存储取初始内容），各一条拷贝
    CHECK(g_nup == 4 && g_up[0].img == 8 * 4 * 4 && g_up[0].first[0] == 0 && g_up[2].img == 4 * 4 * 4 && g_up[2].first[0] == 0);
    CHECK(g_nup == 4 && g_up[1].level == 0 && g_up[1].layer == 0 && g_up[1].w == 3 && g_up[1].h == 2 && g_up[1].pitch == 12 && g_up[1].img == 24 && g_up[1].first[0] == 1);
    CHECK(g_nup == 4 && g_up[3].level == 0 && g_up[3].layer == 0 && g_up[3].w == 4 && g_up[3].h == 4 && g_up[3].pitch == 64 && g_up[3].img == 256 && g_up[3].first[0] == 17);
    CHECK(nvn_be_tex_gpu_owned(61) == 1 && nvn_be_tex_gpu_owned(62) == 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&u); nvn_be_tex_release(&t); nvn_be_alias_drop_pool(&g_tp); nvn_be_pool_del(g_pmem);
}

static void put_set_rt(stream *s, const tex_t *color, const tex_t *depth) {   // 紧凑记录：头、指针、（无视图）、快照
    p_set_rt r; memset(&r, 0, sizeof r); r.n = color ? 1 : 0; r.color[0] = color; r.depth = depth;
    uint8_t *a = put(s, OP_SET_RT, set_rt_size(&r)); p_set_rt_c *h = set_rt_put(a, &r);
    const tex_t **pt = (const tex_t **)(h + 1); tex_t *snap = (tex_t *)(a + set_rt_snap_off(h)); uint32_t k = 0;
    if (color) { snap[k] = *color; pt[k] = &snap[k]; k++; }
    if (depth) { snap[k] = *depth; pt[k] = &snap[k]; }
}
static void pass_fresh(void) { nvn_be_shutdown_session(); res_fresh(); nvn_unimpl_reset(); g_npd = g_pass_ends = g_ncr = 0; g_barrier_bits = 0; g_barrier_at = -1; g_nbar = 0; }

static void t_pass_clear_fold(void) {   // 整屏清除不单独编码：折进通道的 load；没有绘制时提交末为它开一个只清除的通道
    pass_fresh();
    tex_t a = mk_tex(61, 0, NVN_FORMAT_RGBA8, 64, 32, 1, 0), z = mk_tex(62, 0x4000, NVN_FORMAT_DEPTH24_STENCIL8, 64, 32, 1, 0);
    stream s = {0}; put_set_rt(&s, &a, &z);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.25f; cc->rgba[3] = 1;
    p_clear_ds *cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth = 0.5f; cd->depth_mask = 1; cd->stencil = 3; cd->stencil_mask = 0xff;
    uint8_t *b = put(&s, OP_BARRIER, 8); int32_t bits = 5; memcpy(b, &bits, 4);
    end(&s);
    nvn_be_sub_begin(); nvn_be_exec(s.b);
    CHECK(g_npd == 0);                                   // 录到这里什么都没编码
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pass_ends == 1);
    CHECK(g_pd[0].ncolor == 1 && g_pd[0].w == 64 && g_pd[0].h == 32 && g_pd[0].color[0].view != NULL);
    CHECK(g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].store == NVN_GPU_STORE_STORE && g_pd[0].color[0].clear.f[0] == 0.25f && g_pd[0].color[0].clear.f[3] == 1);
    CHECK(g_pd[0].depth.view && g_pd[0].depth.load == NVN_GPU_LOAD_CLEAR && g_pd[0].clear_depth == 0.5f);
    CHECK(g_pd[0].stencil.view == g_pd[0].depth.view && g_pd[0].stencil.load == NVN_GPU_LOAD_CLEAR && g_pd[0].clear_stencil == 3);
    CHECK(g_barrier_bits == 5 && g_barrier_at == 0);     // 屏障在开这个通道之前交给后端
    CHECK(nvn_be_tex_gpu_owned(61) == 1 && nvn_be_tex_gpu_owned(62) == 1);
    CHECK(nvn_unimpl_kinds() == 0);
    // 第二次提交：没有新的清除 → 目标还绑着，但不为它开通道
    stream e2 = {0}; end(&e2); submit1(&e2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1);
}

static int pd_zero_clear(const nvn_gpu_pass_desc *d) { static const nvn_gpu_pass_desc z; return d->color[0].load == NVN_GPU_LOAD_CLEAR && !memcmp(&d->color[0].clear, &z.color[0].clear, sizeof z.color[0].clear); }   // 刚建的目标：载入时清成零（不是折进来的清除值）
// 刚建的单级单层颜色目标：初始的全零不在 CPU 上整张上传，由第一个通道载入时清零；之后的通道照常载入。
// 多级的不行（一个附件视图盖不住整张图）：照旧逐级填零。已有整屏清除的：按清除值清，不被零盖掉。
// DrawTexture（#1452，rp_draw_tex.c）：期望值都按 NVN 语义手算（iOS nvn_mtl.m draw_tex / Ryujinx HelperShader.DrawTexture），不跑被测实现。
//   负对照：后端没置 NVN_GPU_CAP_DRAW_TEX 时照旧记「未接 op OP_DRAW_TEX」、一笔不画、不开通道；同一段命令置位后才画。
static void put_draw_tex(stream *s, int tid, int sid, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1) {
    p_draw_tex *d = put(s, OP_DRAW_TEX, sizeof *d); d->handle = (uint64_t)tid | (uint64_t)sid << 20;
    d->dst[0] = x0; d->dst[1] = y0; d->dst[2] = x1; d->dst[3] = y1; d->src[0] = s0; d->src[1] = t0; d->src[2] = s1; d->src[3] = t1;
}
static void put_state(stream *s, int kind, const void *v, size_t n) { p_bind_state *b = put(s, OP_BIND_STATE, sizeof *b); b->kind = kind; memcpy(b->bytes, v, n); }
static void t_draw_tex(void) {
    tex_t rt = mk_tex(71, 0, NVN_FORMAT_RGBA8, 64, 32, 1, 0), src = mk_tex(72, 0x4000, NVN_FORMAT_RGBA8, 8, 4, 1, 0);
    tex_t isrc = mk_tex(73, 0x8000, NVN_FORMAT_R32UI, 8, 4, 1, 0), irt = mk_tex(74, 0xC000, NVN_FORMAT_RGBA8UI, 16, 16, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp);
#define DT_FRESH() do { pass_fresh(); g_ndt = 0; rp_state_reset(); nvn_be_tex_init(&src); nvn_be_tex_init(&isrc); \
        nvn_be_set_tex_pool_n(16); nvn_be_reg_tex(7, &src, NULL); nvn_be_reg_tex(8, &isrc, NULL); nvn_be_reg_tex(9, &rt, NULL); nvn_be_reg_samp(3, &sp); } while (0)
    g_caps_extra = 0; DT_FRESH();
    // ① 负对照：没有能力位
    { stream a = {0}; put_set_rt(&a, &rt, NULL); put_draw_tex(&a, 7, 3, 10, 20, 42, 4, 0, 0, 8, 4); end(&a);
      submit1(&a); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_ndt == 0 && g_npd == 0 && nvn_unimpl_count("op", "OP_DRAW_TEX") == 1 && nvn_unimpl_kinds() == 1); }
    // ② 有能力位：同一段命令画一笔。目的 (10,20)→(42,4) 上下反向：视口 = 10,4 32x16，源 v 端点交换；源矩形按 8x4 归一化
    g_caps_extra = NVN_GPU_CAP_DRAW_TEX; DT_FRESH();
    { stream a = {0}; put_set_rt(&a, &rt, NULL);
      p_scissor *sc = put(&a, OP_SCISSOR, sizeof *sc); sc->x = 5; sc->y = 6; sc->w = 100; sc->h = 100;   // 夹到 64x32：5,6 59x26
      put_draw_tex(&a, 7, 3, 10, 20, 42, 4, 2, 1, 6, 3); end(&a);
      submit1(&a); fake_complete(NVN_GPU_DONE_OK); }
    CHECK(g_ndt == 1 && nvn_unimpl_kinds() == 0);
    if (g_ndt == 1) {
        const nvn_gpu_draw_tex_desc *d = &g_dt[0].d;
        CHECK(g_dt[0].in_pass == 1 && g_dt[0].npd == 1 && g_pd[0].ncolor >= 1 && g_pd[0].color[0].view != NULL);   // 在开着的通道里、颜色 0 挂着目标
        CHECK(d->dst[0] == 10 && d->dst[1] == 4 && d->dst[2] == 32 && d->dst[3] == 16);
        CHECK(d->uv[0] == 0.25f && d->uv[2] == 0.75f && d->uv[1] == 0.75f && d->uv[3] == 0.25f);   // u 2/8→6/8；v 1/4→3/4 交换成 3/4→1/4
        CHECK(d->has_sc == 1 && d->sc[0] == 5 && d->sc[1] == 6 && d->sc[2] == 59 && d->sc[3] == 26);
        CHECK(d->src != NULL && d->samp != NULL && d->color_mask == 0xf && d->blend_en == 0);   // NVN 缺省：四通道全写、不混合
        CHECK(d->origin_ll == (uint8_t)(rp_origin_lower_left() != 0));
    }
    CHECK(rp_pso_dirty == 1);   // 后端换了内置管线：下一笔绘制前全部重发
    // ③ 混合与写掩码照游戏当前状态，只取 color(0) 那一份
    DT_FRESH();
    { stream a = {0}; put_set_rt(&a, &rt, NULL);
      color_st cs; memset(&cs, 0, sizeof cs); cs.blend_en = 1; cs.logic_op = NVN_LOGIC_OP_COPY; put_state(&a, 1, &cs, sizeof cs);
      chmask_st cm = {0x5u | 0xf0u}; put_state(&a, 2, &cm, sizeof cm);   // color(0) 只写 r、b；color(1) 的位不进
      blend_st bs; memset(&bs, 0, sizeof bs); bs.target = 0; bs.src_rgb = NVN_BLEND_FUNC_SRC_ALPHA; bs.dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA; bs.src_a = NVN_BLEND_FUNC_ONE; bs.dst_a = NVN_BLEND_FUNC_ZERO;
      bs.eq_rgb = NVN_BLEND_EQUATION_ADD; bs.eq_a = NVN_BLEND_EQUATION_ADD; put_state(&a, 0, &bs, sizeof bs);
      p_blend_color *bc = put(&a, OP_BLEND_COLOR, sizeof *bc); bc->rgba[2] = 0.5f;
      put_draw_tex(&a, 7, 3, 0, 0, 8, 4, 0, 0, 8, 4); end(&a);
      submit1(&a); fake_complete(NVN_GPU_DONE_OK); }
    CHECK(g_ndt == 1 && nvn_unimpl_kinds() == 0);
    if (g_ndt == 1) {
        const nvn_gpu_draw_tex_desc *d = &g_dt[0].d;
        CHECK(d->color_mask == 0x5 && d->blend_en == 1 && d->blend.src_rgb == NVN_BLEND_FUNC_SRC_ALPHA && d->blend.dst_rgb == NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA);
        CHECK(d->blend_color[2] == 0.5f && d->has_sc == 0);
        CHECK(d->dst[0] == 0 && d->dst[2] == 8 && d->uv[0] == 0 && d->uv[1] == 0 && d->uv[2] == 1 && d->uv[3] == 1);
    }
    // ④ 各种拒绝：都具名记账、一笔不画；空矩形 / 写掩码为 0 什么都不画也不算未接
    struct { const char *name; int tid; int rt; int kind; } rj[] = {
        {"source_format", 8, 0, 0},        // 整数源
        {"texture_unregistered", 11, 0, 0},
        {"target_integer", 7, 1, 0},       // 整数目标
        {"source_is_target", 9, 0, 0},     // 源就是当前目标（反馈）
        {"logic_op", 7, 0, 1},
        {"dual_source_blend", 7, 0, 2},
        {NULL, 7, 0, 3},                   // 空矩形
        {NULL, 7, 0, 4},                   // color(0) 写掩码为 0
    };
    for (size_t i = 0; i < sizeof rj / sizeof rj[0]; i++) {
        DT_FRESH();
        stream a = {0}; put_set_rt(&a, rj[i].rt ? &irt : &rt, NULL);
        if (rj[i].kind == 1) { color_st cs; memset(&cs, 0, sizeof cs); cs.logic_op = NVN_LOGIC_OP_XOR; put_state(&a, 1, &cs, sizeof cs); }
        if (rj[i].kind == 2) { color_st cs; memset(&cs, 0, sizeof cs); cs.blend_en = 1; cs.logic_op = NVN_LOGIC_OP_COPY; put_state(&a, 1, &cs, sizeof cs);
                               blend_st bs; memset(&bs, 0, sizeof bs); bs.src_rgb = 0x10; bs.dst_rgb = NVN_BLEND_FUNC_ZERO; bs.src_a = NVN_BLEND_FUNC_ONE; bs.dst_a = NVN_BLEND_FUNC_ZERO;
                               bs.eq_rgb = bs.eq_a = NVN_BLEND_EQUATION_ADD; put_state(&a, 0, &bs, sizeof bs); }
        if (rj[i].kind == 4) { chmask_st cm = {0xf0u}; put_state(&a, 2, &cm, sizeof cm); }
        if (rj[i].kind == 3) put_draw_tex(&a, rj[i].tid, 3, 4, 4, 4, 9, 0, 0, 8, 4);
        else put_draw_tex(&a, rj[i].tid, 3, 0, 0, 8, 4, 0, 0, 8, 4);
        end(&a); submit1(&a); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_ndt == 0);
        if (rj[i].name) CHECK(nvn_unimpl_count("draw_tex", rj[i].name) == 1 && nvn_unimpl_kinds() == 1);
        else CHECK(nvn_unimpl_kinds() == 0);
    }
    g_caps_extra = 0; pass_fresh(); rp_state_reset();
#undef DT_FRESH
}

static void t_pass_fresh_zero(void) {
    pass_fresh();
    tex_t a = mk_tex(64, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    for (int k = 0; k < 2; k++) {
        stream s = {0}; if (!k) put_set_rt(&s, &a, NULL);
        p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 8; sc->h = 8;
        p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.5f; end(&s);   // 裁剪不满：开通道、通道里按矩形清
        g_npd = 0; submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        if (!k) CHECK(g_npd == 1 && pd_zero_clear(&g_pd[0]) && g_nup == 0);                        // 第一次：通道清零，没有一次上传
        else CHECK(g_npd == 1 && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD && g_nup == 0);       // 第二次：原内容留着
    }
    pass_fresh();
    tex_t m = mk_tex(65, 0, NVN_FORMAT_RGBA8, 16, 16, 3, 0);
    { stream s = {0}; put_set_rt(&s, &m, NULL);
      p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 8; sc->h = 8;
      p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s);
      submit1(&s); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_npd == 1 && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD && g_nup == 3); }             // 三级：逐级填零，通道照常载入
    pass_fresh();
    tex_t f = mk_tex(66, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    { stream s = {0}; put_set_rt(&s, &f, NULL);
      p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[1] = 0.75f; end(&s);
      submit1(&s); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_npd == 1 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].clear.f[1] == 0.75f && g_nup == 0); }   // 整屏清除：按它的值
}
static void t_pass_target_persists(void) {   // 目标跨提交保留，且不依赖已经被复用的命令内存
    pass_fresh();
    tex_t a = mk_tex(63, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    stream s = {0}; put_set_rt(&s, &a, NULL); end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 0);
    memset(s.b, 0xEE, sizeof s.b);                       // 游戏复用了那块命令内存
    stream s2 = {0}; p_clear_color *cc = put(&s2, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[1] = 0.75f; end(&s2);
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pd[0].w == 16 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].clear.f[1] == 0.75f);
    CHECK(rp_pass_target() && rp_pass_target()->color[0]->serial == 63);
}

static void t_pass_switch_and_partial(void) {
    pass_fresh();
    tex_t a = mk_tex(64, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(65, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; put_set_rt(&s, &a, NULL);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[2] = 1;
    put_set_rt(&s, &b, NULL);                            // 换目标：a 的待折清除先落地
    cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0x7;        // 部分通道：折不进
    cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 3; cc->mask = 0xf;        // 没有这个槽
    p_clear_ds *cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth_mask = 1;          // 没有深度目标
    p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 4; sc->h = 4;
    cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[1] = 0.125f;   // 裁剪不满：开通道（LOAD），通道里按矩形清
    sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 8; sc->h = 8;
    cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.5f;   // 裁剪盖满目标：可以折
    end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 3 && g_pass_ends == 3);
    // a 的整屏清除挂起（换目标不再为它开纯清除通道），所以第一个通道是 b 的矩形清；提交收尾按挂起先后落地 a、b 的清除
    CHECK(g_pd[0].w == 8 && pd_zero_clear(&g_pd[0]));                               // 为矩形清开的通道：不折清除值（这张图刚建，载入时清成零）
    CHECK(g_pd[1].w == 16 && g_pd[1].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[1].color[0].clear.f[2] == 1);   // a 的整屏清除
    CHECK(g_ncr == 2 && g_cr[0].npd == 1 && g_cr[0].ends == 0 && g_cr[1].npd == 1 && g_cr[1].ends == 0);   // 两次矩形清都发在第一个通道开着的时候
    CHECK(g_cr[0].d.color == 0 && g_cr[0].d.color_mask == 0x7 && g_cr[0].d.x == 0 && g_cr[0].d.y == 0 && g_cr[0].d.w == 8 && g_cr[0].d.h == 8);   // 只写三个通道：整张目标、掩码原样交给后端
    CHECK(g_cr[1].d.color == 0 && g_cr[1].d.color_mask == 0xf && !g_cr[1].d.depth && !g_cr[1].d.stencil && g_cr[1].d.x == 0 && g_cr[1].d.y == 0 && g_cr[1].d.w == 4 && g_cr[1].d.h == 4 && g_cr[1].d.value.f[1] == 0.125f);
    CHECK(g_pd[2].w == 8 && g_pd[2].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[2].color[0].clear.f[0] == 0.5f);   // 之后的整屏清除：结束那个通道，折进下一个
    CHECK(nvn_unimpl_count("pass", "clear_color_no_target") == 1 && nvn_unimpl_count("pass", "clear_ds_no_target") == 1);
    CHECK(nvn_unimpl_kinds() == 2);
    pass_fresh();
    tex_t z = mk_tex(66, 0, NVN_FORMAT_DEPTH24_STENCIL8, 16, 16, 1, 0);
    stream d = {0}; put_set_rt(&d, NULL, &z); p_clear_ds *c2 = put(&d, OP_CLEAR_DS, sizeof *c2); c2->depth_mask = 1; c2->depth = 1; end(&d);
    submit1(&d); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pd[0].ncolor == 0 && g_pd[0].w == 16 && g_pd[0].h == 16);  // 只有深度的通道：尺寸取深度目标的
    CHECK(g_pd[0].depth.view && g_pd[0].depth.load == NVN_GPU_LOAD_CLEAR && g_pd[0].clear_depth == 1 && g_pd[0].stencil.view == g_pd[0].depth.view && g_pd[0].stencil.load == NVN_GPU_LOAD_LOAD);
    CHECK(nvn_unimpl_kinds() == 0);
    pass_fresh();
    stream n0 = {0}; p_set_rt r0; memset(&r0, 0, sizeof r0); { uint8_t *ra = put(&n0, OP_SET_RT, set_rt_size(&r0)); set_rt_put(ra, &r0); }   // 一个附件都没有的目标
    p_draw *dn0 = put(&n0, OP_DRAW, sizeof *dn0); dn0->prim = 4; dn0->count = 3; end(&n0);
    submit1(&n0); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 0 && g_pass_ends == 0);                                           // 不发没有附件的通道
    pass_fresh();
    // 颜色槽里有空位：槽 0 空、槽 1 有图。描述里槽 0 留空，尺寸取槽 1 的
    tex_t s1 = mk_tex(69, 0, NVN_FORMAT_RGBA8, 32, 8, 1, 0);
    stream ns = {0}; p_set_rt r2; memset(&r2, 0, sizeof r2); r2.n = 2; r2.color[1] = &s1; { uint8_t *ra = put(&ns, OP_SET_RT, set_rt_size(&r2)); set_rt_put(ra, &r2); }
    p_clear_color *c1 = put(&ns, OP_CLEAR_COLOR, sizeof *c1); c1->index = 1; c1->mask = 0xf; c1->rgba[0] = 0.25f;
    c1 = put(&ns, OP_CLEAR_COLOR, sizeof *c1); c1->index = 0; c1->mask = 0xf;        // 清空位：没有目标
    end(&ns);
    submit1(&ns); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pd[0].ncolor == 2 && g_pd[0].color[0].view == NULL && g_pd[0].color[1].view != NULL && g_pd[0].w == 32 && g_pd[0].h == 8);
    CHECK(g_pd[0].color[1].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[1].clear.f[0] == 0.25f);
    CHECK(nvn_unimpl_count("pass", "clear_color_no_target") == 1 && nvn_unimpl_kinds() == 1);
    pass_fresh();
}

static void t_clear_rect(void) {   // 带裁剪的清除：矩形与目标求交；整数格式换算；深度 / 模板各自按目标有的面与写掩码取舍
    pass_fresh();
    tex_t a = mk_tex(67, 0, NVN_FORMAT_RGBA8UI, 16, 16, 1, 0), z = mk_tex(68, 0x4000, NVN_FORMAT_DEPTH24_STENCIL8, 16, 16, 1, 0);
    stream s = {0}; put_set_rt(&s, &a, &z);
    p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 4; sc->y = -2; sc->w = 20; sc->h = 8;      // 与 16×16 的交集：x 4..16、y 0..6
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 3.75f; cc->rgba[1] = -1; cc->rgba[2] = 5e9f; cc->rgba[3] = 255;
    p_clear_ds *cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth = 0.5f; cd->depth_mask = 1; cd->stencil = 0x105; cd->stencil_mask = 0xff;
    cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth = 2; cd->depth_mask = 1; cd->stencil = 9; cd->stencil_mask = 0x0f;   // 只写低四位模板：掩码交给后端；超界的深度值收到 1
    sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 100; sc->y = 0; sc->w = 4; sc->h = 4;                 // 整个在目标外：什么都不动
    cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf;
    cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth_mask = 1; cd->stencil_mask = 0xff;
    end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pass_ends == 1 && pd_zero_clear(&g_pd[0]) && g_pd[0].depth.load == NVN_GPU_LOAD_LOAD);   // 三次矩形清在同一个通道里
    CHECK(g_ncr == 3);
    for (int i = 0; i < 3 && i < g_ncr; i++) CHECK(g_cr[i].npd == 1 && g_cr[i].ends == 0 && g_cr[i].d.x == 4 && g_cr[i].d.y == 0 && g_cr[i].d.w == 12 && g_cr[i].d.h == 6);
    CHECK(g_cr[0].d.color == 0 && g_cr[0].d.value.u[0] == 3 && g_cr[0].d.value.u[1] == 0 && g_cr[0].d.value.u[2] == 0xffffffffu && g_cr[0].d.value.u[3] == 255);
    CHECK(g_cr[1].d.color == -1 && g_cr[1].d.depth == 1 && g_cr[1].d.stencil == 1 && g_cr[1].d.depth_value == 0.5f && g_cr[1].d.stencil_value == 5);
    CHECK(g_cr[0].d.color_mask == 0xf && g_cr[1].d.stencil_mask == 0xff);
    CHECK(g_cr[2].d.color == -1 && g_cr[2].d.depth == 1 && g_cr[2].d.stencil == 1 && g_cr[2].d.stencil_mask == 0x0f && g_cr[2].d.stencil_value == 9 && g_cr[2].d.depth_value == 1);
    CHECK(nvn_unimpl_kinds() == 0);
    pass_fresh();
}

static void t_report_counter(void) {   // 计数器报告：段完成后才写；时间戳类两格都是时间，统计类计数值为 0；遮挡计数写累计值（没复位没画过 = 0）；失败段不写
    nvn_be_shutdown_session(); nvn_unimpl_reset(); g_rcond = 0;
    uint64_t rec[4][2]; memset(rec, 0xAB, sizeof rec);
    const uint64_t types[4] = {NVN_COUNTER_TYPE_TIMESTAMP, NVN_COUNTER_TYPE_INPUT_VERTICES, NVN_COUNTER_TYPE_TIMESTAMP_TOP, NVN_COUNTER_TYPE_SAMPLES_PASSED};
    stream a = {0};
    for (int i = 0; i < 4; i++) { uint8_t *r = put(&a, OP_REPORT_COUNTER, 16); uint64_t ad = (uint64_t)(uintptr_t)rec[i]; memcpy(r, &types[i], 8); memcpy(r + 8, &ad, 8); }
    end(&a);
    g_tick = 111; submit1(&a);
    CHECK(rec[0][0] == 0xABABABABABABABABull);        // 已提交、未完成：没写
    g_tick = 7777; fake_complete(NVN_GPU_DONE_OK);    // 时间取完成这一刻的游戏时钟
    CHECK(rec[0][0] == 7777 && rec[0][1] == 7777);
    CHECK(rec[1][0] == 0 && rec[1][1] == 7777);
    CHECK(rec[2][0] == 7777 && rec[2][1] == 7777);
    CHECK(rec[3][0] == 0 && rec[3][1] == 7777);
    CHECK(nvn_unimpl_kinds() == 0);
    g_tick = 9999; submit1(&a); fake_complete(NVN_GPU_DONE_FAILED);
    CHECK(rec[0][0] == 7777 && rec[1][1] == 7777 && rec[3][1] == 7777);    // 失败段：不回写
    // 条件渲染开着的款：时间戳类的计数值写 0（同真机），时间那一格照写
    g_rcond = 1; g_tick = 8888; submit1(&a); fake_complete(NVN_GPU_DONE_OK);
    CHECK(rec[0][0] == 0 && rec[0][1] == 8888 && rec[2][0] == 0 && rec[2][1] == 8888);
    g_rcond = 0; nvn_be_shutdown_session(); nvn_unimpl_reset();
}

static void ts_report(stream *s, uint64_t *rec) {
    uint64_t type = NVN_COUNTER_TYPE_TIMESTAMP, addr = (uint64_t)(uintptr_t)rec;
    uint8_t *p = put(s, OP_REPORT_COUNTER, 16); memcpy(p, &type, 8); memcpy(p + 8, &addr, 8);
}
static void t_report_position(void) {
    nvn_be_shutdown_session(); g_caps_extra = NVN_GPU_CAP_REPORT_TIMESTAMP; g_ts_calls = 0;
    uint64_t rec[2][2] = {{99, 99}, {88, 88}}; stream a = {0};
    for (int i = 0; i < 2; i++) { uint64_t t = i ? NVN_COUNTER_TYPE_TIMESTAMP_TOP : NVN_COUNTER_TYPE_TIMESTAMP;
        uint64_t ad = (uint64_t)(uintptr_t)rec[i]; uint8_t *r = put(&a, OP_REPORT_COUNTER, 16); memcpy(r, &t, 8); memcpy(r + 8, &ad, 8); }
    end(&a); g_tick = 999999; submit1(&a);
    CHECK(rec[0][0] == 99 && rec[1][0] == 88); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ts_calls == 2); CHECK(g_ts_types[0] == NVN_COUNTER_TYPE_TIMESTAMP && g_ts_types[1] == NVN_COUNTER_TYPE_TIMESTAMP_TOP);
    CHECK(rec[0][0] == 110 && rec[0][1] == 110); CHECK(rec[1][0] == 120 && rec[1][1] == 120);
    // Zero remains a valid sample; an unavailable result falls back and records the reason.
    uint64_t unavail0 = nvn_unimpl_count("timestamp", "result_unavailable");
    rec[0][0] = rec[0][1] = 99; rec[1][0] = rec[1][1] = 88; submit1(&a);
    g_pend[0].ts[0] = (nvn_gpu_timestamp_result){.host_ns=0, .valid=1};
    g_pend[0].ts[1].valid = 0; fake_complete(NVN_GPU_DONE_OK);
    CHECK(rec[0][0] == 0 && rec[0][1] == 0);
    CHECK(rec[1][0] == g_tick && rec[1][1] == g_tick);
    CHECK(nvn_unimpl_count("timestamp", "result_unavailable") == unavail0 + 1);
    rec[0][0] = rec[0][1] = 99; rec[1][0] = rec[1][1] = 88;
    g_ts_convert_fail = 1; submit1(&a); fake_complete(NVN_GPU_DONE_OK); g_ts_convert_fail = 0;
    CHECK(rec[0][0] == g_tick && rec[0][1] == g_tick);
    CHECK(rec[1][0] == g_tick && rec[1][1] == g_tick);
    CHECK(nvn_unimpl_count("timestamp", "result_unavailable") == unavail0 + 3);
    uint64_t fault0 = nvn_unimpl_count("timestamp", "fault");
    rec[0][0] = rec[0][1] = 99; rec[1][0] = rec[1][1] = 88; submit1(&a);
    fake_complete(NVN_GPU_DONE_FAULT);
    CHECK(rec[0][0] == g_tick && rec[0][1] == g_tick);
    CHECK(rec[1][0] == g_tick && rec[1][1] == g_tick);
    CHECK(nvn_unimpl_count("timestamp", "fault") == fault0 + 2);
#ifdef S2A_TEST_TS_ALLOC
    nvn_be_shutdown_session(); uint64_t oom0 = nvn_unimpl_count("timestamp", "slot_oom");
    rec[0][0] = rec[0][1] = 99; rec[1][0] = rec[1][1] = 88;
    s2a_test_fail_ts_alloc = 1; submit1(&a); s2a_test_fail_ts_alloc = 0; fake_complete(NVN_GPU_DONE_OK);
    CHECK(rec[0][0] == g_tick && rec[0][1] == g_tick);
    CHECK(rec[1][0] == g_tick && rec[1][1] == g_tick);
    CHECK(nvn_unimpl_count("timestamp", "slot_oom") == oom0 + 2);
#endif
    rec[0][0] = 99; submit1(&a); fake_complete(NVN_GPU_DONE_FAILED); CHECK(rec[0][0] == 99);
    // Slot budget is per actual segment, never a reason to drop the submission.
    stream many = {0}; uint64_t records[65][2]; memset(records, 0, sizeof records);
    for (int i = 0; i < 65; i++) { ts_report(&many, records[i]); }
    end(&many);
    rp_timestamp_fallback_take(); g_ts_calls = 0; submit1(&many); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ts_calls == 64 && records[63][0] == 740 && records[64][0] == g_tick);
    CHECK(rp_timestamp_fallback_take() == 1);
    records[64][0] = 54321; g_ts_calls = 0; submit1(&many); fake_complete(NVN_GPU_DONE_FAULT);
    CHECK(records[0][0] == g_tick && records[64][0] == g_tick);
    // CALL then JUMP preserves execution order and address-to-slot mapping.
    stream child = {0}, tail = {0}, root = {0}; ts_report(&child, rec[0]); end(&child);
    ts_report(&tail, rec[1]); end(&tail); uint64_t ptr = (uint64_t)(uintptr_t)child.b;
    memcpy(put(&root, OP_CALL, 8), &ptr, 8); ptr = (uint64_t)(uintptr_t)tail.b;
    memcpy(put(&root, OP_JUMP, 8), &ptr, 8); end(&root); g_ts_calls = 0;
    submit1(&root); fake_complete(NVN_GPU_DONE_OK); CHECK(rec[0][0] == 110 && rec[1][0] == 120);
    // The first occlusion reset cuts the segment between the two reports.
    nvn_be_shutdown_session(); stream split = {0}; ts_report(&split, rec[0]);
    uint64_t reset = NVN_COUNTER_TYPE_SAMPLES_PASSED; memcpy(put(&split, OP_RESET_COUNTER, 8), &reset, 8);
    ts_report(&split, rec[1]); end(&split); g_ts_calls = 0; submit1(&split);
    CHECK(g_npend == 2 && g_pend[0].nts == 1 && g_pend[1].nts == 1);
    fake_complete(NVN_GPU_DONE_OK); CHECK(rec[0][0] == 110 && rec[1][0] == 120);
    g_caps_extra = 0; nvn_be_shutdown_session(); nvn_unimpl_reset();
}

// 最小的可画环境：一个不带属性、不带绑定的程序，一张 64x36 的颜色目标
static tex_t g_qrt;
static void q_env(stream *s) {
    memset(&g_fprog, 0, sizeof g_fprog); for (int st = 0; st < RP_PROG_STAGES; ++st) for (int i = 0; i < RP_PROG_ENT_MAX; ++i) g_fprog.ent[st][i].src_reg2 = UINT16_MAX; g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.color_out_mask = 0xffffffffu;
    g_qrt = mk_tex(91, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    put_set_rt(s, &g_qrt, NULL);
    p_viewports *vp = put(s, OP_VIEWPORTS, sizeof *vp); vp->n = 1; vp->v[0][2] = 64; vp->v[0][3] = 36;
}
static void q_draw(stream *s) { p_draw *d = put(s, OP_DRAW, sizeof *d); d->prim = 4; d->count = 3; d->instances = 1; }

static unsigned g_scan_limit, g_scan_draws;
static unsigned g_scan_prims[4];
static int g_scan_start_draws, g_scan_untouched;

unsigned rp_shader_lookahead_budget(void) { return g_scan_limit; }

int rp_shader_lookahead(const struct rp_state *state, const struct rp_shader_req *q, const p_set_rt *targets) {
    CHECK(targets->color[0] && targets->color[0]->serial == g_qrt.serial);
    if (g_scan_draws < 4) g_scan_prims[g_scan_draws] = q->prim;
    g_scan_draws++;
    g_scan_untouched &= state != &rp_S && g_gdraws == g_scan_start_draws;
    CHECK(q->rt_count == 1 && q->rt_format[0] == NVN_FORMAT_RGBA8);
    return 1;
}

static void t_submit_compile_lookahead(void) {
    pass_fresh();
    g_resolve_rc = 0;
    stream s = {0}; q_env(&s);
    g_scan_limit = 4;
    g_scan_draws = 0;
    g_scan_start_draws = g_gdraws;
    g_scan_untouched = 1;
    q_draw(&s);
    p_draw *line = put(&s, OP_DRAW, sizeof *line);
    line->prim = 1; line->count = 2;
    *(uint32_t *)put(&s, OP_BARRIER, sizeof(uint32_t)) = NVN_BARRIER_ORDER_PRIMITIVES_BIT;
    q_draw(&s);
    uint64_t counters[4] = {0};
    p_render_cond *dynamic = put(&s, OP_RENDER_COND, sizeof *dynamic);
    *dynamic = (p_render_cond){.mode = 0, .addr = (uintptr_t)counters};
    q_draw(&s);
    end(&s);
    submit1(&s);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_scan_draws == 3 && g_scan_prims[0] == 4 && g_scan_prims[1] == 1 && g_scan_prims[2] == 4);
    CHECK(g_scan_untouched);
    CHECK(g_gdraws == g_scan_start_draws + 4);
    g_scan_limit = 0;
    pass_fresh();
}


// #1581 H：合成输入，只验替身后端调用轨迹，不代表 GPU 像素/真机通过。
static void t_pass_shared_extent(void) {
    int old_area = g_flag_pass_area;
    for (int area = 0; area < 2; area++) for (int mip = 0; mip < 2; mip++) {
        pass_fresh(); g_flag_pass_area = area; g_resolve_rc = 0;
        tex_t a = mk_tex(781, 0, NVN_FORMAT_RGBA8, 64 << mip, 16 << mip, 1 + mip, 0);
        tex_t z = mk_tex(782, 0x10000, NVN_FORMAT_DEPTH24_STENCIL8, 16, 64, 1, 0);
        stream s = {0}; q_env(&s);
        p_set_rt r; memset(&r, 0, sizeof r); r.n = 2; r.color[1] = &a; r.depth = &z;
        if (mip) { r.has_cview[1] = 1; r.cview[1].set_levels = 1; r.cview[1].base_level = 1; r.cview[1].nlevels = 1; }
        uint8_t *rt = put(&s, OP_SET_RT, set_rt_size(&r)); set_rt_put(rt, &r);
        q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        int found = 0;
        for (int i = 0; i < g_npd && i < 8; i++) {
            const nvn_gpu_pass_desc *d = &g_pd[i];
            if (d->ncolor == 2 && d->color[1].view && d->depth.view) {
                found++;
                CHECK(!d->color[0].view && d->w == 16 && d->h == 16);
                CHECK(d->color[1].load == NVN_GPU_LOAD_LOAD); // fresh 大附件的全清不能只覆盖交集
            }
        }
        CHECK(found == 1 && g_npd <= 8);
        CHECK(rp_S.vp[0][2] == 64 && rp_S.vp[0][3] == 36);
        CHECK(g_last_dyn.nvp == 1 && g_last_dyn.vp[0][2] == 64); // 保留逻辑视口
        CHECK(nvn_unimpl_kinds() == 0);
    }
    g_flag_pass_area = old_area; pass_fresh();
}

static void t_pass_shared_pending_clear(void) {
    // draw=0 走提交末 flush；draw=1 在绘制前落地。depth_only 不清模板，不能误清另一面。
    for (int draw = 0; draw < 2; draw++) for (int depth_only = 0; depth_only < 2; depth_only++) {
        pass_fresh(); g_resolve_rc = 0;
        tex_t a = mk_tex(783, 0, NVN_FORMAT_RGBA8, 64, 16, 1, 0);
        tex_t z = mk_tex(784, 0x10000, NVN_FORMAT_DEPTH24_STENCIL8, 16, 64, 1, 0);
        stream s = {0}; q_env(&s); put_set_rt(&s, &a, &z);
        p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.25f;
        p_clear_ds *cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth_mask = 1; cd->depth = 0.5f; cd->stencil_mask = depth_only ? 0 : 0xff; cd->stencil = 7;
        if (draw) q_draw(&s);
        end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        int color_clear = -1, depth_clear = -1, shared = -1;
        for (int i = 0; i < g_npd && i < 8; i++) {
            const nvn_gpu_pass_desc *d = &g_pd[i];
            if (d->color[0].view && d->color[0].load == NVN_GPU_LOAD_CLEAR && d->w == 64 && d->h == 16) {
                color_clear = i; CHECK(!d->depth.view && d->color[0].clear.f[0] == 0.25f);
            }
            if (d->depth.view && d->depth.load == NVN_GPU_LOAD_CLEAR && d->w == 16 && d->h == 64) {
                depth_clear = i; CHECK(!d->color[0].view && d->clear_depth == 0.5f);
                CHECK(d->stencil.load == (depth_only ? NVN_GPU_LOAD_LOAD : NVN_GPU_LOAD_CLEAR));
                if (!depth_only) CHECK(d->clear_stencil == 7);
            }
            if (d->color[0].view && d->depth.view) {
                shared = i; CHECK(d->w == 16 && d->h == 16);
                CHECK(d->color[0].load == NVN_GPU_LOAD_LOAD && d->depth.load == NVN_GPU_LOAD_LOAD && d->stencil.load == NVN_GPU_LOAD_LOAD);
            }
        }
        CHECK(g_npd <= 8 && color_clear >= 0 && depth_clear >= 0);
        if (draw) CHECK(shared > color_clear && shared > depth_clear);
        CHECK(nvn_unimpl_kinds() == 0);
    }
    pass_fresh();
}

static void t_pass_shared_explicit_rect(void) {
    for (int ds = 0; ds < 2; ds++) {
    pass_fresh(); g_resolve_rc = 0;
    tex_t a = mk_tex(785, 0, NVN_FORMAT_RGBA8, 64, 16, 1, 0);
    tex_t z = mk_tex(786, 0x10000, NVN_FORMAT_DEPTH24_STENCIL8, 16, 64, 1, 0);
    stream s = {0}; q_env(&s); put_set_rt(&s, &a, &z); q_draw(&s);
    p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = ds ? 0 : 8; sc->y = ds ? 8 : 0; sc->w = ds ? 12 : 40; sc->h = ds ? 40 : 12;
    if (ds) {
        p_clear_ds *cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth_mask = 1; cd->depth = 0.5f; cd->stencil_mask = 0x0f; cd->stencil = 7;
    } else {
        p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0x7; cc->rgba[1] = 0.75f;
    }
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    // 逐点计数仅代表发出的矩形并集；保留掩码，不能漏共享区外 32×12 或重清。
    unsigned char seen[12][40]; memset(seen, 0, sizeof seen);
    CHECK(g_ncr > 0 && g_ncr <= 8 && g_npd <= 8);
    for (int i = 0; i < g_ncr && i < 8; i++) {
        const nvn_gpu_clear *c = &g_cr[i].d;
        if (ds) CHECK(c->color == -1 && c->depth && c->stencil && c->depth_value == 0.5f && c->stencil_mask == 0x0f && c->stencil_value == 7);
        else CHECK(c->color == 0 && c->color_mask == 0x7 && c->value.f[1] == 0.75f && !c->depth && !c->stencil);
        CHECK(g_cr[i].npd > 0 && g_cr[i].npd <= 8 && g_cr[i].ends < g_cr[i].npd);
        if (g_cr[i].npd > 0 && g_cr[i].npd <= 8) {
            const nvn_gpu_pass_desc *d = &g_pd[g_cr[i].npd - 1];
            CHECK(c->x >= 0 && c->y >= 0 && (uint64_t)c->x + c->w <= d->w && (uint64_t)c->y + c->h <= d->h);
        }
        int32_t cx = ds ? c->y : c->x, cy = ds ? c->x : c->y;
        uint32_t cw = ds ? c->h : c->w, ch = ds ? c->w : c->h;
        CHECK(cx >= 8 && cy >= 0 && (uint64_t)cx + cw <= 48 && (uint64_t)cy + ch <= 12);
        for (int y = 0; y < 12; y++) for (int x = 8; x < 48; x++)
            if (x >= cx && y >= cy && (uint64_t)x < (uint64_t)cx + cw && (uint64_t)y < (uint64_t)cy + ch) seen[y][x - 8]++;
    }
    for (int y = 0; y < 12; y++) for (int x = 0; x < 40; x++) CHECK(seen[y][x] == 1);
    CHECK(nvn_unimpl_kinds() == 0); pass_fresh();
    }
}

static void area_scissor(stream *s, int x, int y, int w, int h) {
    p_scissor *sc = put(s, OP_SCISSOR, sizeof *sc);
    *sc = (p_scissor){.x = x, .y = y, .w = w, .h = h};
}

static void t_pass_area_guards(void) {
    enum { WITHIN, GROW, NO_SCISSOR, CLEAR_CROSSES, ZERO, ALIAS, MSAA, OFF };
    const int old_area = g_flag_pass_area;
    for (int kind = WITHIN; kind <= OFF; kind++) {
        pass_fresh();
        g_flag_pass_area = kind != OFF;
        g_resolve_rc = 0;
        nvn_be_sub_begin();
        stream first = {0};
        q_env(&first);
        const int before = g_gdraws;
        tex_t alias = mk_tex(792, 4096, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
        if (kind == ALIAS) {
            const p_set_rt rt = {.n = 2, .color = {&g_qrt, &alias}};
            uint8_t *a = put(&first, OP_SET_RT, set_rt_size(&rt));
            set_rt_put(a, &rt);
            CHECK(alias.mem < g_qrt.mem + g_qrt.L.size);
            memset(alias.mem, 0x55, alias.L.size);
        }
        if (kind == MSAA) {
            g_qrt.d.samples = 4;
            nvn_tex_layout(&g_qrt.d, &g_qrt.L);
            put_set_rt(&first, &g_qrt, NULL);
        }
        if (kind != ZERO) memset(g_qrt.mem, 0x66, g_qrt.L.size);
        area_scissor(&first, 3, 5, 16, 12);
        q_draw(&first); end(&first); nvn_be_exec(first.b);
        const int local = kind <= CLEAR_CROSSES;
        printf("PASS_AREA start=%d width=%u\n", kind, g_pd[0].area_w);
        CHECK(g_npd == 1 && g_gdraws == before + 1);
        CHECK(g_pd[0].area_w == (local ? 16u : 0u));
        CHECK(!local || (g_pd[0].area_x == 3 && g_pd[0].area_y == 5 && g_pd[0].area_h == 12));
        if (kind == ZERO) CHECK(g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR);
        if (kind <= CLEAR_CROSSES) {
            stream second = {0};
            if (kind == WITHIN) area_scissor(&second, 6, 7, 8, 4);
            if (kind == GROW) area_scissor(&second, 15, 10, 24, 14);
            if (kind == NO_SCISSOR) rp_R.has_sc = 0; // 下一笔恢复帧缓冲默认裁剪
            if (kind == CLEAR_CROSSES) {
                area_scissor(&second, 15, 10, 24, 14);
                p_clear_color *clear = put(&second, OP_CLEAR_COLOR, sizeof *clear);
                clear->mask = 1; clear->rgba[0] = 1;
            } else {
                q_draw(&second);
            }
            end(&second); nvn_be_exec(second.b);
            printf("PASS_AREA case=%d passes=%d first=%u next=%u clears=%d\n",
                   kind, g_npd, g_pd[0].area_w, g_npd > 1 ? g_pd[1].area_w : 0, g_ncr);
            CHECK(g_npd == (kind == WITHIN ? 1 : 2));
            if (kind != WITHIN) CHECK(g_pd[1].area_w == 0 && g_pd[1].color[0].load == NVN_GPU_LOAD_LOAD);
            CHECK(g_gdraws == before + (kind == CLEAR_CROSSES ? 1 : 2));
            if (kind == CLEAR_CROSSES) CHECK(g_ncr == 1 && g_cr[0].npd == 2 && g_cr[0].d.w == 24);
        }
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    }
    g_flag_pass_area = old_area;
    pass_fresh();
}

static void put_set_rt_v(stream *s, const tex_t *color, const view_t *cv);
// 连续绘制只改一项状态：后端必须拿到该次绘制的管线描述；不变的命令不额外重取。
static void dirty_strip(stream *s, uint64_t base) {
    p_draw_idx *d = put(s, OP_DRAW_IDX, sizeof *d); d->prim = 5; d->itype = 1; d->count = 4; d->instances = 1; d->ibuf = base;
}
static void dirty_exec(stream *s) { end(s); nvn_be_exec(s->b); }
static void t_pipeline_dirty_changes(void) {
    pass_fresh(); g_resolve_rc = 0; g_pg_tracked = 0; memset(g_pmem, 0, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    stream first = {0}; q_env(&first); dirty_strip(&first, base); dirty_exec(&first);
    CHECK(g_last_pdesc.prim_restart == 0 && g_last_pdesc.rast_discard == 0 && g_last_pdesc.patch_size == 3);
    int gets = g_pipe_gets, draws = g_gdraws;
    stream restart = {0}; p_prim_restart *r = put(&restart, OP_PRIM_RESTART, sizeof *r); r->on = 1; r->index = 0xffff; dirty_strip(&restart, base); dirty_exec(&restart);
    printf("PIPE_DIRTY restart: gets %d -> %d, value=%u\n", gets, g_pipe_gets, g_last_pdesc.prim_restart);
    CHECK(g_pipe_gets == gets + 1 && g_last_pdesc.prim_restart == 1 && g_gdraws == draws + 1);
    gets = g_pipe_gets;
    stream same = {0}; r = put(&same, OP_PRIM_RESTART, sizeof *r); r->on = 1; r->index = 0xffff; dirty_strip(&same, base); dirty_exec(&same);
    CHECK(g_pipe_gets == gets);
    gets = g_pipe_gets;
    stream discard = {0}; p_rast_discard *d = put(&discard, OP_RAST_DISCARD, sizeof *d); d->on = 1; dirty_strip(&discard, base); dirty_exec(&discard);
    printf("PIPE_DIRTY discard: gets %d -> %d, value=%u\n", gets, g_pipe_gets, g_last_pdesc.rast_discard);
    CHECK(g_pipe_gets == gets + 1 && g_last_pdesc.rast_discard == 1);
    gets = g_pipe_gets;
    stream patch = {0}; p_set_patch_size *p = put(&patch, OP_SET_PATCH_SIZE, sizeof *p); p->size = 7; dirty_strip(&patch, base); dirty_exec(&patch);
    printf("PIPE_DIRTY patch: gets %d -> %d, value=%u\n", gets, g_pipe_gets, g_last_pdesc.patch_size);
    CHECK(g_pipe_gets == gets + 1 && g_last_pdesc.patch_size == 7);
    gets = g_pipe_gets;
    stream repeat = {0}; p = put(&repeat, OP_SET_PATCH_SIZE, sizeof *p); p->size = 7; dirty_strip(&repeat, base); dirty_exec(&repeat);
    CHECK(g_pipe_gets == gets);
    stream invalid = {0}; p = put(&invalid, OP_SET_PATCH_SIZE, sizeof *p); p->size = 33; dirty_strip(&invalid, base); dirty_exec(&invalid);
    CHECK(g_pipe_gets == gets && g_last_pdesc.patch_size == 7);
    stream off_restart = {0}; r = put(&off_restart, OP_PRIM_RESTART, sizeof *r); r->on = 0; r->index = 0xffff; dirty_strip(&off_restart, base); dirty_exec(&off_restart);
    CHECK(g_pipe_gets == gets + 1 && g_last_pdesc.prim_restart == 0); gets = g_pipe_gets;
    stream off_discard = {0}; d = put(&off_discard, OP_RAST_DISCARD, sizeof *d); d->on = 0; dirty_strip(&off_discard, base); dirty_exec(&off_discard);
    CHECK(g_pipe_gets == gets + 1 && g_last_pdesc.rast_discard == 0); gets = g_pipe_gets;
    stream old_patch = {0}; p = put(&old_patch, OP_SET_PATCH_SIZE, sizeof *p); p->size = 3; dirty_strip(&old_patch, base); dirty_exec(&old_patch);
    CHECK(g_pipe_gets == gets + 1 && g_last_pdesc.patch_size == 3);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_pool_del(g_pmem); g_pg_tracked = 1; pass_fresh();
}
static void t_target_format_variant(void) {
    pass_fresh(); g_resolve_rc = 0; nvn_be_sub_begin();
    stream first = {0}; q_env(&first); g_variant_prog = g_fprog; g_variant_prog.sh[1] = &g_variant_fs; g_rt_variant = 1;
    q_draw(&first); dirty_exec(&first);
    CHECK(g_last_req.rt_format[0] == NVN_FORMAT_RGBA8 && g_last_pdesc.st[1].sh == &g_fsh[1]);
    tex_t other = mk_tex(92, 0x4000, NVN_FORMAT_RGBA32UI, 64, 36, 1, 0);
    int resolves = g_resolves;
    stream second = {0}; put_set_rt(&second, &other, NULL); q_draw(&second); dirty_exec(&second);
    printf("PIPE_DIRTY target: resolves %d -> %d, request=%u, shader_variant=%d\n", resolves, g_resolves, g_last_req.rt_format[0], g_last_pdesc.st[1].sh == &g_variant_fs);
    CHECK(g_resolves == resolves + 1 && g_last_req.rt_format[0] == NVN_FORMAT_RGBA32UI && g_last_pdesc.st[1].sh == &g_variant_fs);
    resolves = g_resolves;
    stream again = {0}; q_draw(&again); dirty_exec(&again);
    CHECK(g_resolves == resolves && g_last_pdesc.st[1].sh == &g_variant_fs);
    tex_t same_fmt = mk_tex(93, 0x8000, NVN_FORMAT_RGBA32UI, 64, 36, 1, 0);
    stream same_target_fmt = {0}; put_set_rt(&same_target_fmt, &same_fmt, NULL); q_draw(&same_target_fmt); dirty_exec(&same_target_fmt);
    CHECK(g_resolves == resolves && g_last_pdesc.st[1].sh == &g_variant_fs);
    view_t view = {0}; view.set_format = 1; view.format = NVN_FORMAT_RGBA32F;
    stream with_view = {0}; put_set_rt_v(&with_view, &same_fmt, &view); q_draw(&with_view); dirty_exec(&with_view);
    CHECK(g_resolves == resolves + 1 && g_last_req.rt_format[0] == NVN_FORMAT_RGBA32F && g_last_pdesc.st[1].sh == &g_fsh[1]);
    resolves = g_resolves;
    stream back = {0}; put_set_rt(&back, &g_qrt, NULL); q_draw(&back); dirty_exec(&back);
    CHECK(g_resolves == resolves + 1 && g_last_req.rt_format[0] == NVN_FORMAT_RGBA8 && g_last_pdesc.st[1].sh == &g_fsh[1]);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); g_rt_variant = 0; pass_fresh();
}

#include "t_pass_zero_mask.inc"

// ---------- 通道边界（同目标续接 / 状态保持 / 待折清除）----------
// 对齐 nvn_mtl.m:10955 rt_same 与 :15060 的续接：通道边界不再等于状态边界。这些用例都要真的画出东西，所以放在绘制替身之后。
static void put_set_rt_v(stream *s, const tex_t *color, const view_t *cv);   // 定义在本文件后面（带视图记录的目标）
static void t_pass_rt_continue(void) {   // 同目标续接：通道开着、没有待折清除、目标与当前一致 → 只换逻辑目标，不结束 / 不重开通道
    pass_fresh(); g_flag_rt_cont = 1; g_resolve_rc = g_cresolve_rc = 0;
    int d0 = g_gdraws, npd0 = g_npd, ends0 = g_pass_ends;
    stream s = {0}; q_env(&s);                        // 目标 91（64×36 RGBA8）
    q_draw(&s);                                       // 第一笔：开通道
    put_set_rt(&s, &g_qrt, NULL);                     // 同一组目标再设一次（游戏每帧反复重设同一组目标）
    q_draw(&s);                                       // 第二笔：还在同一个通道里
    end(&s);
    uint64_t p0[RP_PERF_N]; rp_perf_take(p0);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    uint64_t p1[RP_PERF_N]; rp_perf_take(p1);
    CHECK(g_gdraws - d0 == 2);
    CHECK(g_npd - npd0 == 1 && g_pass_ends - ends0 == 1);       // 只开一个通道、只结束一次（修前：两个通道、两次结束）
    CHECK(p1[RP_PERF_PASSES] == 1 && p1[RP_PERF_P_CONT] == 1);  // 续接命中一次
    // 负对照：同一个开关关掉 → 每次重设目标都是一次通道边界
    g_flag_rt_cont = 0;
    int npd1 = g_npd;
    stream s2 = {0}; q_env(&s2); q_draw(&s2); put_set_rt(&s2, &g_qrt, NULL); q_draw(&s2); end(&s2);
    uint64_t p2[RP_PERF_N]; rp_perf_take(p2);
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    uint64_t p3[RP_PERF_N]; rp_perf_take(p3);
    CHECK(g_gdraws - d0 == 4 && g_npd - npd1 == 2 && p3[RP_PERF_P_CONT] == 0);
    g_flag_rt_cont = 1; pass_fresh();
}

static void t_pass_state_keep(void) {   // 通道边界不再等于状态边界（开关 nvk_state_keep）：跨两个不同目标的通道，新通道第一笔不重发管线与绑定
    int second[2] = {-1, -1}, first = -1;
    for (int keep = 0; keep <= 1; keep++) {
        pass_fresh(); g_flag_state_keep = keep; g_resolve_rc = g_cresolve_rc = 0;
        tex_t b = mk_tex(92, 0x4000, NVN_FORMAT_RGBA8, 64, 36, 1, 0);   // 与 91 同格式同尺寸：管线形状没变，只有目标换了
        stream s = {0}; q_env(&s);                        // 目标 91
        q_draw(&s);                                        // 第一笔：通道 1
        size_t off = s.n;                                  // 第二个通道从这里开始
        put_set_rt(&s, &b, NULL); q_draw(&s);              // 换目标 92：通道 2 的第一笔
        end(&s);
        memset(g_bind_calls, 0, sizeof g_bind_calls);
        uint8_t save[8]; memcpy(save, s.b + off, 8); op_hdr eh = {OP_END, 8}; memcpy(s.b + off, &eh, 8);
        int pg0 = g_pipe_gets;
        nvn_be_sub_begin(); nvn_be_exec(s.b);              // 回放到通道 1 结束
        first = g_bind_calls[0] + g_bind_calls[1] + g_bind_calls[2];
        memcpy(s.b + off, save, 8);
        nvn_be_exec(s.b + off);                            // 再回放通道 2 的第一笔
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        second[keep] = g_bind_calls[0] + g_bind_calls[1] + g_bind_calls[2] - first;
        CHECK(g_npd == 2);                                 // 换目标照旧是通道边界
        CHECK(g_pipe_gets - pg0 == (keep ? 1 : 2));        // 形状没变：开关 1 不重取管线，开关 0 每个通道都重取
    }
    CHECK(first == 3);                                     // 第一笔：三个图形阶段各发一次（程序只有顶点 + 片元，几何阶段发零条）
    CHECK(second[1] == 0 && second[0] == first);           // 开关 1：新通道第一笔一次绑定都不发；开关 0：与第一笔同样多（各阶段重发）
    // 负对照：换目标时附件格式变了（管线对象绑着渲染通道的格式）→ 新通道第一笔必须重取管线、重发绑定
    pass_fresh(); g_flag_state_keep = 1; g_resolve_rc = g_cresolve_rc = 0;
    tex_t c16 = mk_tex(93, 0x8000, NVN_FORMAT_RGBA16UI, 64, 36, 1, 0);   // 与 91 同尺寸、不同格式
    { stream s = {0}; q_env(&s); q_draw(&s); put_set_rt(&s, &c16, NULL); q_draw(&s); end(&s);
      int pg0 = g_pipe_gets; memset(g_bind_calls, 0, sizeof g_bind_calls);
      submit1(&s); fake_complete(NVN_GPU_DONE_OK);
      // 两笔各取一次管线（附件格式进管线描述）；绑定布局没变：描述符组在命令缓冲里仍然有效
      //（Vulkan：描述符绑定不随管线对象、也不随开/收通道失效，只有布局不兼容才必须重发），所以不重发绑定。
      CHECK(g_npd == 2 && g_pipe_gets - pg0 == 2 && g_bind_calls[0] == 1); }
    g_flag_state_keep = 1; pass_fresh();
}

static void t_pass_clear_park(void) {   // 待折清除跨目标挂起（开关 nvk_clear_park）：清除不马上落地，等下一个把它当附件的通道吃掉
    pass_fresh(); g_flag_clear_park = 1; g_flag_rt_cont = 1; g_resolve_rc = g_cresolve_rc = 0;
    int g0 = g_gdraws;
    tex_t a = mk_tex(74, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(75, 0x4000, NVN_FORMAT_RGBA8, 32, 8, 1, 0);
    stream s = {0};
    put_set_rt(&s, &a, NULL);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.25f;   // 清 A：挂起
    put_set_rt(&s, &b, NULL); q_draw(&s);              // 换目标 B 画一笔：不为 A 开纯清除通道
    put_set_rt(&s, &a, NULL); q_draw(&s);              // 回到 A 画一笔：A 的清除被这个通道吃掉（load = CLEAR）
    end(&s);
    uint64_t p0[RP_PERF_N]; rp_perf_take(p0);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    uint64_t p1[RP_PERF_N]; rp_perf_take(p1);
    CHECK(g_npd == 2 && g_pass_ends == 2);             // 两个通道：B、A（修前：A 的纯清除 + B + A 三个）
    CHECK(g_pd[0].w == 32 && g_pd[0].h == 8 && pd_zero_clear(&g_pd[0]));   // B 刚建：载入时清成零
    CHECK(g_pd[1].w == 16 && g_pd[1].h == 16 && g_pd[1].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[1].color[0].clear.f[0] == 0.25f);
    CHECK(g_gdraws - g0 == 2 && p1[RP_PERF_P_PARK] == 1 && p1[RP_PERF_P_CLEARONLY] == 0);   // 一个纯清除通道都没有
    // 负对照：开关关掉 → 换目标时先为 A 落地（纯清除通道），再 B、再 A：三个通道
    g_flag_clear_park = 0;
    int npd0 = g_npd; g_gdraws = 0;
    stream s2 = {0};
    put_set_rt(&s2, &a, NULL);
    cc = put(&s2, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.25f;
    put_set_rt(&s2, &b, NULL); q_draw(&s2);
    put_set_rt(&s2, &a, NULL); q_draw(&s2);
    end(&s2);
    uint64_t p2[RP_PERF_N]; rp_perf_take(p2);
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    uint64_t p3[RP_PERF_N]; rp_perf_take(p3);
    CHECK(g_npd - npd0 == 3 && g_gdraws == 2 && p3[RP_PERF_P_CLEARONLY] == 1 && p3[RP_PERF_P_PARK] == 1);   // 关着时清除照样进表，只是换目标就落地
    g_flag_clear_park = 1; pass_fresh();
}

static void t_pass_clear_park_tail(void) {   // 挂起到提交收尾：清 A 后再不碰 A → 收尾一个纯清除通道（颜色 + 深度模板一起）
    pass_fresh(); g_flag_clear_park = 1; g_resolve_rc = g_cresolve_rc = 0;
    tex_t a = mk_tex(76, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), z = mk_tex(77, 0x4000, NVN_FORMAT_DEPTH24_STENCIL8, 16, 16, 1, 0);
    stream s = {0}; put_set_rt(&s, &a, &z);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[2] = 1;
    p_clear_ds *cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth = 0.5f; cd->depth_mask = 1;                      // 只清深度
    cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->stencil = 7; cd->stencil_mask = 0xff;                             // 再清模板：同一张图两面合成一项
    end(&s);
    uint64_t p0[RP_PERF_N]; rp_perf_take(p0);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    uint64_t p1[RP_PERF_N]; rp_perf_take(p1);
    CHECK(g_npd == 1 && g_pass_ends == 1);             // 收尾一个纯清除通道：同一次 SET_RT 的清除合成一组
    CHECK(g_pd[0].w == 16 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].clear.f[2] == 1);
    CHECK(g_pd[0].depth.view && g_pd[0].depth.load == NVN_GPU_LOAD_CLEAR && g_pd[0].clear_depth == 0.5f &&
          g_pd[0].stencil.view && g_pd[0].stencil.load == NVN_GPU_LOAD_CLEAR && g_pd[0].clear_stencil == 7);
    CHECK(p1[RP_PERF_P_CLEARONLY] == 1 && p1[RP_PERF_P_PARK] == 3);   // 颜色一条 + 深度 / 模板两条（后一条不能把前一条的面清掉）
    // 同一张图的不同级是两个子资源（身份 = 序号 + 级 + 层 + 类别）：各挂一项、不会互相覆盖；被当附件的那一级先吃掉，另一级收尾落地
    pass_fresh(); g_flag_clear_park = 1;
    tex_t m = mk_tex(84, 0, NVN_FORMAT_RGBA8, 16, 16, 2, 0);   // 两级：第 1 级 8×8
    view_t v0; memset(&v0, 0, sizeof v0); v0.set_levels = 1; v0.base_level = 0; v0.nlevels = 1;
    view_t v1 = v0; v1.base_level = 1;
    stream s2 = {0};
    put_set_rt_v(&s2, &m, &v1); cc = put(&s2, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.25f;   // 清第 1 级
    put_set_rt_v(&s2, &m, &v0); cc = put(&s2, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[1] = 0.75f;   // 清第 0 级
    q_draw(&s2); end(&s2);                                     // 画第 0 级：只吃掉第 0 级那一项
    uint64_t q0[RP_PERF_N]; rp_perf_take(q0);
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    uint64_t q1[RP_PERF_N]; rp_perf_take(q1);
    CHECK(g_npd == 2);                                         // 绘制通道（第 0 级，清成 0.75）+ 收尾落地第 1 级
    CHECK(g_pd[0].w == 16 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].clear.f[1] == 0.75f);
    CHECK(g_pd[1].w == 8 && g_pd[1].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[1].color[0].clear.f[0] == 0.25f);
    CHECK(q1[RP_PERF_P_PARK] == 2 && q1[RP_PERF_P_CLEARONLY] == 1);
    pass_fresh();
}

// #1072：格式不能线性过滤 / 不能混合时照常用是未定义行为（G710 的 RGBA16 不能过滤、R32F 不能混合等）。
// 期望：线性采样器配这种格式时改用同参数的最近点采样器，记 tex / filter_unsupported_fmt_0x..；这种格式当颜色目标时关混合，记 draw / blend_unsupported_fmt_0x..。
// 负对照：能过滤 / 能混合时不改；整数格式本来就走最近点，不记。
static void t_fmt_filter_blend(void) {
    pass_fresh(); g_resolve_rc = g_cresolve_rc = 0; int g0 = g_gdraws;
    tex_t a = mk_tex(151, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(152, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp); sp.minf = NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR; sp.magf = NVN_MAG_FILTER_LINEAR; sp.ws = NVN_WRAP_MODE_REPEAT; sp.aniso = 4;
    nvn_be_reg_tex(7, &a, NULL); nvn_be_reg_samp(3, &sp);
    blend_st bl; memset(&bl, 0, sizeof bl); bl.target = 0;
    for (int k = 0; k < 2; k++) {   // k = 0：替身设备说 RGBA8 不能过滤、不能混合；k = 1：负对照，能
        g_caps_nofilter = g_caps_noblend = k ? ~0u : NVN_FORMAT_RGBA8;
        stream s = {0}; q_env(&s); put_set_rt(&s, &b, NULL);
        g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5; g_fprog.ent[1][0].src_reg2 = UINT16_MAX;
        g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
        p_bind_state *by = put(&s, OP_BIND_STATE, sizeof *by); by->kind = 0; memcpy(by->bytes, &bl, sizeof bl);
        color_st cs; memset(&cs, 0, sizeof cs); cs.blend_en = 1; cs.logic_op = NVN_LOGIC_OP_COPY;
        by = put(&s, OP_BIND_STATE, sizeof *by); by->kind = 1; memcpy(by->bytes, &cs, sizeof cs);
        p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
        q_draw(&s); end(&s);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_gdraws == g0 + 1 + k && g_nfb[1] == 1 && g_fb[1][0].u.ts.samp != NULL);
        if (!k) {
            CHECK((g_last_pdesc.color.blend_en & 1) == 0 && nvn_unimpl_count("draw", "blend_unsupported_fmt_0x25") == 1);
            CHECK(g_samp_last.magf == NVN_MAG_FILTER_NEAREST && g_samp_last.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST && g_samp_last.ws == NVN_WRAP_MODE_REPEAT &&
                  g_samp_last.aniso <= 1 && nvn_unimpl_count("tex", "filter_unsupported_fmt_0x25") == 1);
        } else {
            CHECK((g_last_pdesc.color.blend_en & 1) == 1 && nvn_unimpl_count("draw", "blend_unsupported_fmt_0x25") == 0 && nvn_unimpl_count("tex", "filter_unsupported_fmt_0x25") == 0);
        }
        pass_fresh(); nvn_be_reg_tex(7, &a, NULL); nvn_be_reg_samp(3, &sp);
    }
    g_fprog.nent[1] = 0;
    pass_fresh(); g_caps_nofilter = NVN_FORMAT_RGBA8;
    nvn_be_reg_tex(7, &a, NULL);
    sp.ws = NVN_WRAP_MODE_MIRROR_CLAMP; nvn_be_reg_samp(3, &sp);
    nvn_gpu_samp near = rp_samp_for_tex(3, 7);
    CHECK(near && g_samp_last.ws == NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE && g_samp_last.magf == NVN_MAG_FILTER_NEAREST);
    pass_fresh(); g_caps_nofilter = NVN_FORMAT_RGBA8;
    nvn_be_reg_tex(7, &a, NULL); sp.ws = NVN_WRAP_MODE_REPEAT; nvn_be_reg_samp(3, &sp);
    g_samp_fail_nearest = 1;
    CHECK(rp_samp_for_tex(3, 7) == NULL);  // 最近点采样器分配失败，不能交原来的线性采样器
    g_samp_fail_nearest = 0; g_caps_nofilter = g_caps_noblend = ~0u;
    pass_fresh();
}

static void t_draw_tex_lib_fallback(int target, int format, int lib_cls, int actual_cls, int fallback_missing) {
    pass_fresh(); g_resolve_rc = g_cresolve_rc = 0; g_gresolve_next = NULL; g_lib_reject_calls = 0;
    int g0 = g_gdraws; uint64_t mm0 = nvn_unimpl_count("tex", "decl_type_mismatch");
    tex_t a = mk_tex(673, 0, format, 16, 16, target == NVN_TEXTURE_TARGET_3D ? 4 : 1, 0); a.d.target = target;
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_tex(7, &a, NULL); nvn_be_reg_samp(3, &sp);
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1; g_fprog.lib_sourced = 1; g_fprog.dcls[1][0] = lib_cls;
    memcpy(&g_gprog2, &g_fprog, sizeof g_fprog); g_gprog2.lib_sourced = 0; g_gprog2.dtt[1][0] = target + 1; g_gprog2.dcls[1][0] = actual_cls;
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    q_draw(&s); end(&s);
    if (fallback_missing) {
        g_resolve_seq[0] = &g_fprog; g_resolve_seq[1] = NULL;
        g_resolve_seq_i = 0; g_resolve_seq_n = 2;
    }
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    g_resolve_seq_n = 0;
    const int retry = target != NVN_TEXTURE_TARGET_2D || lib_cls != actual_cls;
    CHECK(g_lib_reject_calls == retry && g_fprog.lib_bad == retry && g_gdraws == g0 + !fallback_missing);
    CHECK(nvn_unimpl_count("tex", "sampled_single_numeric_mismatch") == (uint64_t)(lib_cls != actual_cls));
    CHECK(nvn_unimpl_count("draw", "sampled_numeric_fallback_failed") == (uint64_t)fallback_missing);
    if (!fallback_missing) CHECK(g_nfb[1] == 1 && g_fb[1][0].u.ts.view && ((const struct nvn_gpu_view_s *)g_fb[1][0].u.ts.view)->of->d.d.target == target &&
          ((const struct nvn_gpu_view_s *)g_fb[1][0].u.ts.view)->of->d.d.width == 16);
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm0);
    g_gresolve_next = NULL; g_lib_reject_calls = 0; g_fprog.lib_sourced = 0; g_fprog.lib_bad = 0; g_fprog.dcls[1][0] = 0; pass_fresh();
}

static void t_pass_clear_park_read(void) {   // 被读之前必须落地（正确性红线）：清 A 后把 A 当采样贴图用，绘制路径先落地再开通道
    pass_fresh(); g_flag_clear_park = 1; g_flag_rt_cont = 1; g_resolve_rc = g_cresolve_rc = 0;
    int g0 = g_gdraws;
    tex_t a = mk_tex(78, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(79, 0x4000, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_tex(7, &a, NULL); nvn_be_reg_samp(3, &sp);
    stream s = {0}; q_env(&s);                        // 程序（顶点 + 片元）与视口；目标 91
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5;   // 片元采样句柄表第 5 项
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
    put_set_rt(&s, &a, NULL);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.5f;   // 清 A：挂起
    put_set_rt(&s, &b, NULL);                          // 换目标 B
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;   // 片元采样 A（登记序号 7）
    q_draw(&s);                                        // 画到 B、采样 A：读之前 A 的清除必须先落地（rp_draw 在开通道之前调 rp_pass_land_reads）
    q_draw(&s);                                        // 第二笔：挂起表已空，不再拆通道
    end(&s);
    uint64_t p0[RP_PERF_N]; rp_perf_take(p0);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    uint64_t p1[RP_PERF_N]; rp_perf_take(p1);
    CHECK(g_gdraws - g0 == 2 && nvn_unimpl_kinds() == 0);
    CHECK(g_npd == 2 && g_pass_ends == 2);             // ① A 的纯清除（读之前落地）② B 的两笔绘制
    CHECK(g_pd[0].w == 16 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].clear.f[0] == 0.5f);   // 落地通道在绘制通道之前
    CHECK(g_pd[1].w == 64 && g_pd[1].h == 36 && pd_zero_clear(&g_pd[1]) && g_pd[1].color[0].view != NULL);
    CHECK(p1[RP_PERF_P_PARK] == 1 && p1[RP_PERF_P_CLEARONLY] == 1);
    pass_fresh();
}

static void put_clear_test_color(stream *s, float r, float g, float b) {
    p_clear_color *cc = put(s, OP_CLEAR_COLOR, sizeof *cc);
    cc->mask = 15; cc->rgba[0] = r; cc->rgba[1] = g; cc->rgba[2] = b; cc->rgba[3] = 1;
}

static void t_pass_clear_park_sample(void) {
    enum { DIRECT, ALIAS, COLD_TARGET, ALIAS_REPEATED_CLEAR };
    static const struct { const char *name; int slot, pool, alias, park; } cases[] = {
        {"low", 5, 0, 0, 1},
        {"separate-128", 128, 0, 0, 1},
        {"pool", 0, 1, 0, 1},
        {"alias", 5, 0, ALIAS, 1},
        {"alias-unresolved-clear-target", 5, 0, COLD_TARGET, 1},
        {"alias-repeated-clear", 5, 0, ALIAS_REPEATED_CLEAR, 1},
        {"separate-128-park-off", 128, 0, 0, 0},
        {"pool-park-off", 0, 1, 0, 0},
        {"alias-park-off", 5, 0, ALIAS, 0},
        {"alias-repeated-clear-park-off", 5, 0, ALIAS_REPEATED_CLEAR, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pass_fresh();
        g_flag_clear_park = cases[i].park;
        g_resolve_rc = 0;
        g_color_sample_test = 1;
        g_color_sample_index = cases[i].pool ? 2 : -1;
        g_color_image_table = NULL;
        tex_t src = mk_tex(1001, 0x4000, NVN_FORMAT_RGBA8, 1, 1, 1, 0);
        tex_t alias = src; alias.serial = 1002;
        tex_t dst = mk_tex(1003, 0x8000, NVN_FORMAT_RGBA8, 1, 1, 1, 0);
        nvn_be_reg_tex(2, cases[i].alias != DIRECT ? &alias : &src, NULL);
        samp_par sp = {0}; sp.minf = NVN_MIN_FILTER_NEAREST; sp.magf = NVN_MAG_FILTER_NEAREST;
        nvn_be_reg_samp(1, &sp);
        nvn_be_set_tex_pool_n(4);

        stream init = {0};
        const tex_t *sentinel = cases[i].alias == COLD_TARGET ? &alias : &src;
        put_set_rt(&init, sentinel, NULL);
        put_clear_test_color(&init, 1, 0, 0);
        end(&init); submit1(&init); fake_complete(NVN_GPU_DONE_OK);
        nvn_gpu_tex storage = rp_tex_backend_by_serial(sentinel->serial);
        CHECK(storage && storage->px[0][0] == 0xff0000ffu);
        if (cases[i].alias == COLD_TARGET) CHECK(!rp_tex_backend_by_serial(src.serial));

        stream s = {0}; q_env(&s);
        p_viewports *vp = put(&s, OP_VIEWPORTS, sizeof *vp);
        vp->n = 1; vp->v[0][2] = vp->v[0][3] = 1;
        nvn_gpu_layout_ent *e = &g_fprog.ent[NVN_GPU_SH_FS][0];
        e->count = 1; e->access = NVN_GPU_ACC_R;
        g_fprog.nent[NVN_GPU_SH_FS] = 1;
        g_fprog.dtt[NVN_GPU_SH_FS][0] = NVN_TEXTURE_TARGET_2D + 1;
        if (cases[i].pool) {
            rp_S.tex_pool_n = 4;
            rp_S.samp_pool_n = 2;
            e->cls = NVN_GPU_RES_TEX; e->src_kind = NVN_RB_TEXP; e->count = 5;
            nvn_gpu_layout_ent *se = e + 1;
            se->cls = NVN_GPU_RES_SAMP; se->src_kind = NVN_RB_SAMPP; se->count = 3; se->binding = 1;
            g_fprog.nent[NVN_GPU_SH_FS] = 2;
        } else {
            e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = (uint16_t)cases[i].slot;
            p_bind_handles *tex = put(&s, OP_BIND_TEX, sizeof *tex);
            tex->stage = NVN_GPU_SH_FS; tex->first = cases[i].slot; tex->n = 1; tex->h[0] = 2;
            if (cases[i].slot >= 128) {
                e->src_reg2 = 0;
                p_bind_handles *sam = put(&s, OP_BIND_SAMP, sizeof *sam);
                sam->stage = NVN_GPU_SH_FS; sam->n = 1; sam->h[0] = 1u << 20;
            } else tex->h[0] |= 1u << 20;
        }
        put_set_rt(&s, &src, NULL);
        if (cases[i].alias == ALIAS_REPEATED_CLEAR) {
            put_clear_test_color(&s, 1, 1, 0);
            put_set_rt(&s, &alias, NULL);
            put_clear_test_color(&s, 0, 0, 1);
            put_set_rt(&s, &src, NULL);
        }
        put_clear_test_color(&s, 0, 1, 51.0f / 255.0f);
        put_set_rt(&s, &dst, NULL); q_draw(&s); end(&s);
        const int draws = g_gdraws;
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_gdraws == draws + 1 && !nvn_unimpl_kinds());
        if (cases[i].alias) CHECK(rp_tex_backend_by_serial(alias.serial) == storage);
        nvn_gpu_tex output = rp_tex_backend_by_serial(dst.serial);
        uint32_t pixel = output ? output->px[0][0] : 0;
        printf("clear-park sample=%s rgba=%08x expected=ff33ff00\n", cases[i].name, (unsigned)pixel);
        CHECK(pixel == 0xff33ff00u);
        g_color_sample_test = 0;
    }
    g_flag_clear_park = 1;
    pass_fresh();
}
// 逐笔日志读贴图首 8 字节必须经内核读（rp_draw.c dbg_peek）：采样的贴图在「CPU 不可访问 + 虚拟」池里（真机池标 0x2a1 = 虚拟 | 可压缩 |
// GPU 缓存 | CPU 不可访问），存储页没有读权限；回放自己不读这种池（cpu_content 见标志就返回 0），日志直接解引用就吃 SIGSEGV
// （设备读数：开逐笔日志后重场景第一帧崩在这一读，SEGV_ACCERR）。日志该写「读不到」、整笔照画。
// 贴图 A 先被 GPU 清除，再被下一笔绘制采样。
static void drawlog_sample_a(uint8_t *amem, int lines) {
    pass_fresh(); g_flag_clear_park = 1; g_flag_rt_cont = 1; g_resolve_rc = g_cresolve_rc = 0;
    g_tp.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE | NVN_MEMORY_POOL_FLAGS_VIRTUAL;
    int g0 = g_gdraws; g_dl_unread = g_dl_lines = 0;
    tex_t a = mk_tex(78, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(79, 0x4000, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    a.mem = amem;
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_tex(7, &a, NULL); nvn_be_reg_samp(3, &sp);
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
    put_set_rt(&s, &a, NULL);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.5f;
    put_set_rt(&s, &b, NULL);
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    q_draw(&s);
    end(&s);
    rp_draw_log_arm(lines);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    rp_draw_log_arm(0);
    CHECK(g_gdraws - g0 == 1 && nvn_unimpl_kinds() == 0);   // 整笔照画
    pass_fresh();
}
static void t_drawlog_unreadable_tex_mem(void) {
    static uint8_t ok_mem[4096];
    drawlog_sample_a(ok_mem, 2);
    CHECK(g_dl_lines >= 1 && g_dl_unread == 0);   // 正对照：读得到的内存照常写首 8 字节，不写「读不到」
    uint8_t *bad = mmap(NULL, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(bad != MAP_FAILED);
    if (bad == MAP_FAILED) return;
    drawlog_sample_a(bad, 2);
    CHECK(g_dl_lines >= 1 && g_dl_unread >= 1);   // 无读权限：写「读不到」，没崩
    drawlog_sample_a(bad, 0);
    CHECK(g_dl_lines == 0 && g_dl_unread == 0);   // 日志关着：不读、不写
    munmap(bad, 8192);
}
// GS 资源变化必须重新取变体；带 GS 的各种拓扑都照画（三角带按 GL 序展开）。
static void t_gs_dirty_and_topology(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0;
    prog_t pg; memset(&pg, 0, sizeof pg); pg.magic = MAGIC('P','R','O','G'); pg.serial = 996; pg.nstage = 3;
    g_snap_prog = &pg; g_snap_ok = 7; memset(g_snap_keys, 0, sizeof g_snap_keys);
    memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    for (int i = 0; i < 3; i++) { g_snap_stage[i] = (uint8_t)i; g_snap_keys[i][0] = (uint8_t)(i + 1); }
    stream s = {0}; q_env(&s);
    p_bind_program_id *id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pg; id->serial = pg.serial; id->stages = 0x1f;
    q_draw(&s);
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 2; ub->index = 0; ub->addr = 0x4000; ub->size = 64;
    q_draw(&s);
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_resolves == 2);
    // 带 GS 的拓扑一律照画（同 Ryujinx：原生能收的直接交给 GS，收不了的换成三角形列表）。期望按定义手算，不跑被测的展开函数。
    //   四边形（不带索引、4 个顶点）→ 三角形列表 (0,1,2)(0,2,3)
    stream t = {0}; p_draw *quad = put(&t, OP_DRAW, sizeof *quad); quad->prim = 7; quad->count = 4; quad->instances = 1;
    end(&t); submit1(&t); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_last_da.prim == 4 && g_last_da.itype == 2);
    { const uint32_t e[6] = {0, 1, 2, 0, 2, 3}; CHECK(g_last_da.count == 6 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); }
    // #696：GS + 不带索引、非间接的三角带，按 GL 序展开成三角形列表再交给原生 GS。期望按 GL 4.6 §10.1.6 手算（不跑被测的展开函数）：
    //   第 i 个三角形偶 i 为 (i, i+1, i+2)、奇 i 为 (i+1, i, i+2)；Vulkan 原生三角带的奇三角形是 (i, i+2, i+1)。首顶点进 base vertex，实例数与首实例不变
    { stream u = {0}; p_draw *ts = put(&u, OP_DRAW, sizeof *ts); ts->prim = 5; ts->first = 7; ts->count = 4; ts->instances = 32; ts->base_instance = 3;
      end(&u); submit1(&u); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == 4);
      CHECK(g_last_da.prim == 4 && g_last_pdesc.prim == 4 && g_last_req.prim == 4 && g_last_pdesc.prim_restart == 0);
      CHECK(g_last_da.itype == 2 && g_last_da.ibuf == &g_arena_buf && g_last_da.count == 6 && g_last_da.base_vertex == 7 && g_last_da.first == 0);
      CHECK(g_last_da.instances == 32 && g_last_da.base_instance == 3);
      { const uint32_t e[6] = {0, 1, 2, 2, 1, 3}; CHECK(g_last_da.count == 6 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); } }
    { stream u = {0}; p_draw *ts = put(&u, OP_DRAW, sizeof *ts); ts->prim = 5; ts->count = 5; ts->instances = 1;   // 奇数个顶点：3 个三角形
      end(&u); submit1(&u); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == 5 && g_last_da.prim == 4 && g_last_da.base_vertex == 0);
      { const uint32_t e[9] = {0, 1, 2, 2, 1, 3, 2, 3, 4}; CHECK(g_last_da.count == 9 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); } }
    { stream u = {0}; p_draw *ts = put(&u, OP_DRAW, sizeof *ts); ts->prim = 5; ts->count = 2; ts->instances = 1;   // 不足 3 个顶点：不画、不报红
      end(&u); submit1(&u); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == 5); }
    // 带索引 + 重启的三角带：读游戏索引按 GL 序展开，重启后从第 0 个三角形重新算。u16 索引 {10,11,12, 重启, 20,21,22,23}：
    //   第一段 (10,11,12)；第二段 i=0 (20,21,22)、i=1 (22,21,23)。索引原值进列表，base vertex 照游戏给的 4
    { nvn_be_pool_add(g_pmem, sizeof g_pmem); const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
      const uint16_t ix[8] = {10, 11, 12, 0xffff, 20, 21, 22, 23}; memcpy(g_pmem + 3 * PG + 2048, ix, sizeof ix);
      stream u = {0}; p_prim_restart *pr = put(&u, OP_PRIM_RESTART, sizeof *pr); pr->on = 1; pr->index = 0xffff;
      p_draw_idx *di = put(&u, OP_DRAW_IDX, sizeof *di); di->prim = 5; di->itype = 1; di->count = 8; di->base_vertex = 4; di->instances = 1; di->ibuf = base + 3 * PG + 2048;
      pr = put(&u, OP_PRIM_RESTART, sizeof *pr); pr->on = 0;
      end(&u); submit1(&u); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == 6 && g_last_da.prim == 4 && g_last_da.itype == 2 && g_last_da.base_vertex == 4 && g_last_pdesc.prim_restart == 0);
      { const uint32_t e[9] = {10, 11, 12, 20, 21, 22, 22, 21, 23}; CHECK(g_last_da.count == 9 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); }
      // 间接的三角带：参数在 GPU 上、CPU 展开不了，按原生三角带交给 GS（不再整笔跳）
      memset(g_pmem + 3 * PG + 2560, 0, 16);
      stream v = {0}; p_draw *ts = put(&v, OP_DRAW, sizeof *ts); ts->prim = 5; ts->ind = base + 3 * PG + 2560;
      end(&v); submit1(&v); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == 7 && g_last_da.prim == 5 && g_last_pdesc.prim == 5); }
    // 三角扇：原生交给 GS
    { stream u = {0}; p_draw *ts = put(&u, OP_DRAW, sizeof *ts); ts->prim = 6; ts->count = 4; ts->instances = 1;
      end(&u); submit1(&u); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == 8 && g_last_da.prim == 6 && g_last_pdesc.prim == 6); }
    // 邻接三角形（0xC）：设备开了 geometryShader 时原生交给后端（后端映射 *_WITH_ADJACENCY）；没开时具名跳过（Vulkan 不许）
    { stream u = {0}; p_draw *ts = put(&u, OP_DRAW, sizeof *ts); ts->prim = 0xC; ts->count = 6; ts->instances = 1;
      end(&u); submit1(&u); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == 8 && nvn_unimpl_count("draw", "adjacency_without_geometry_feature") == 1);
      g_caps_extra = NVN_GPU_CAP_GEOMETRY;
      submit1(&u); fake_complete(NVN_GPU_DONE_OK);
      g_caps_extra = 0;
      CHECK(g_gdraws == 9 && g_last_da.prim == 0xC && g_last_pdesc.prim == 0xC && nvn_unimpl_count("draw", "adjacency_without_geometry_feature") == 1); }
    CHECK(nvn_unimpl_count("draw", "geometry_topology_unimplemented") == 0);
    g_snap_prog = NULL; pass_fresh();
}
// 不同阶段同槽的三个 UBO 有不同哨兵；只重绑 GS 后检查对应阶段与管线。
static void t_gs_bind(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0;
    memset(g_bind_calls, 0, sizeof g_bind_calls); memset(g_nfb, 0, sizeof g_nfb);
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    for (int i = 0; i < 4; i++) memset(g_pmem + PG + i * 64, 0x11 * (i + 1), 64);
    prog_t pg; memset(&pg, 0, sizeof pg); pg.magic = MAGIC('P','R','O','G'); pg.serial = 997; pg.nstage = 3;
    g_snap_prog = &pg; g_snap_ok = 7; memset(g_snap_keys, 0, sizeof g_snap_keys);
    memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    for (int i = 0; i < 3; i++) { g_snap_stage[i] = (uint8_t)i; g_snap_keys[i][0] = (uint8_t)(i + 1); }
    stream s = {0}; q_env(&s); g_fprog.sh[2] = &g_fsh[2];
    for (int i = 0; i < 3; i++) { g_fprog.nent[i] = 1; g_fprog.ent[i][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[i][0].src_kind = NVN_RB_CBUF; }
    p_bind_program_id *id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pg; id->serial = pg.serial; id->stages = 0x1f;
    for (int i = 0; i < 3; i++) { p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = i; ub->index = 0; ub->addr = base + PG + i * 64; ub->size = 64; }
    q_draw(&s);
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 2; ub->index = 0; ub->addr = base + PG + 3 * 64; ub->size = 64;
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_last_pdesc.st[NVN_GPU_SH_GS].sh == &g_fsh[2]);
    CHECK(g_bind_calls[NVN_GPU_SH_GS] == 2 && g_nfb[NVN_GPU_SH_GS] == 1);
    for (int st = 0; st < 3; st++) if (g_nfb[st] == 1) {
        unsigned char expected = st == 2 ? 0x44 : (unsigned char)(0x11 * (st + 1));
        CHECK(g_fb[st][0].u.b.len == 64 && g_arena[g_fb[st][0].u.b.off] == expected);
    } else CHECK(0);
    g_snap_prog = NULL; pass_fresh();
}
// 常量缓冲暂存交后端异步填（rp_draw.c alloc_fill）：替身挂起填充、到等 / 代拷 / 提交才拷。
// 第一笔绑 A（512 字节，全 0x11）→ OP_UPDATE_UB 改 A 头 8 字节为 0xB2：改之前第一笔的填充必须已做完，所以第一片拷到的是 0x11；
// 第二笔绑 A → 片头是 0xB2；再改一段与 A 不相交的内存（池里 A 之后 2 KB）：第二笔的填充还挂着也不用管。
// steal = 0（nvk_ub_fill_steal 关，原做法）：等 1 次、代拷 0 笔；steal = 1（缺省）：等 0 次、代拷恰好 1 笔 512 字节（读数行计数同此）。
// 期望全靠手算：两片头字节 0x11 / 0xB2、提交后挂起的全部拷完、最后一次绑定指向第二片；改 A 之后第一片仍是 0x11（写前来源 = 暂存，写后来源变了暂存不变）。
static void ub_fill_async_run(int steal) {
    g_flag_ub_steal = steal;
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0;
    memset(g_bind_calls, 0, sizeof g_bind_calls); memset(g_nfb, 0, sizeof g_nfb);
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem, a = base + PG; memset(g_pmem + PG, 0x11, 512);
    prog_t pg; memset(&pg, 0, sizeof pg); pg.magic = MAGIC('P','R','O','G'); pg.serial = 1993; pg.nstage = 2;
    g_snap_prog = &pg; g_snap_ok = 3; memset(g_snap_keys, 0, sizeof g_snap_keys); memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    for (int i = 0; i < 2; i++) { g_snap_stage[i] = (uint8_t)i; g_snap_keys[i][0] = (uint8_t)(i + 0x61); }
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_CBUF;
    p_bind_program_id *id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pg; id->serial = pg.serial; id->stages = 0x1f;
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 0; ub->addr = a; ub->size = 512;
    q_draw(&s);
    uint8_t *r = put(&s, OP_UPDATE_UB, 16 + 8); uint64_t d = a, n = 8; memcpy(r, &d, 8); memcpy(r + 8, &n, 8); memset(r + 16, 0xB2, 8);
    q_draw(&s);
    r = put(&s, OP_UPDATE_UB, 16 + 8); d = a + 2048; memcpy(r, &d, 8); memcpy(r + 8, &n, 8); memset(r + 16, 0xC3, 8);   // 与 A [a, a+512) 不相交
    end(&s);
    uint64_t pf[RP_PERF_N]; rp_perf_take(pf);
    fill_reset(1); submit1(&s);
    const int waits = g_fill_waits, takes = g_fill_takes, nlog = g_fill_nlog, pending = g_fill_nq - g_fill_head;
    fake_complete(NVN_GPU_DONE_OK); fill_reset(0);
    rp_perf_take(pf);
    CHECK(g_gdraws == 2 && nlog == 2);
    CHECK(waits == (steal ? 0 : 1));    // 只有改 A 的那一次要管；关着是等，开着是代拷、不等
    CHECK(takes == (steal ? 1 : 0));
    CHECK(pf[RP_PERF_UBO_FILL_N] == 2 && pf[RP_PERF_UBO_FILL_WAIT_N] == 1);
    CHECK(pf[RP_PERF_UBO_STAGED_B] == 1024);   // 两片各 512 字节真落暂存（第二笔前 A 被改过，切片表认不回）；手算 = 2 × 512
    CHECK(pf[RP_PERF_UBO_FILL_STEAL_N] == (steal ? 1u : 0u) && pf[RP_PERF_UBO_FILL_STEAL_B] == (steal ? 512u : 0u) && pf[RP_PERF_UBO_FILL_BUSY_N] == 0);
    CHECK(pending == 0);       // 段提交前全部填完
    if (nlog == 2) {
        CHECK(g_fill_log[0].d[0] == 0x11 && g_fill_log[0].d[511] == 0x11);   // 第一笔：改之前的内容（来源此刻已是 0xB2，暂存不跟着变）
        CHECK(g_pmem[PG] == 0xB2);
        CHECK(g_fill_log[1].d[0] == 0xB2 && g_fill_log[1].d[7] == 0xB2 && g_fill_log[1].d[8] == 0x11);   // 第二笔：改之后
        CHECK(g_nfb[1] == 1 && g_fb[1][0].u.b.len == 512 && g_arena + g_fb[1][0].u.b.off == g_fill_log[1].d);
    }
    g_snap_prog = NULL; g_flag_ub_steal = -1; pass_fresh();
}
static void t_ub_fill_async(void) { ub_fill_async_run(0); ub_fill_async_run(1); }
// 代拷要管每一笔相交的，不只最新那笔：三笔挂着的填充 F1 = [A, A+512)、F2 = [A+256, A+768)、F3 = [B, B+512)（B = A + 2 KB，不相交），
// 然后改 [A+300, A+308)——与 F1、F2 都相交。期望（手算）：代拷恰好 2 笔（F1、F2）共 1024 字节、等 0 次；F1 片第 300 字节、F2 片第 44 字节仍是 0x11
// （改之前的内容），F3 片是 0x22；改 F3 之外的内存不碰 F3。只代拷最新一笔（F2）的话，F1 到提交才拷，拷到的是 0xB2。
static void t_ub_fill_steal_all(void) {
    g_flag_ub_steal = 1;
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0;
    memset(g_bind_calls, 0, sizeof g_bind_calls); memset(g_nfb, 0, sizeof g_nfb);
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem, a = base + PG, b = a + 2048; memset(g_pmem + PG, 0x11, 768); memset(g_pmem + PG + 2048, 0x22, 512);
    prog_t pg; memset(&pg, 0, sizeof pg); pg.magic = MAGIC('P','R','O','G'); pg.serial = 1994; pg.nstage = 2;
    g_snap_prog = &pg; g_snap_ok = 3; memset(g_snap_keys, 0, sizeof g_snap_keys); memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    for (int i = 0; i < 2; i++) { g_snap_stage[i] = (uint8_t)i; g_snap_keys[i][0] = (uint8_t)(i + 0x61); }
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_CBUF;
    p_bind_program_id *id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pg; id->serial = pg.serial; id->stages = 0x1f;
    const uint64_t at[3] = { a, a + 256, b };
    for (int k = 0; k < 3; k++) { p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 0; ub->addr = at[k]; ub->size = 512; q_draw(&s); }
    uint8_t *r = put(&s, OP_UPDATE_UB, 16 + 8); uint64_t d = a + 300, n = 8; memcpy(r, &d, 8); memcpy(r + 8, &n, 8); memset(r + 16, 0xB2, 8);
    end(&s);
    uint64_t pf[RP_PERF_N]; rp_perf_take(pf);
    fill_reset(1); submit1(&s);
    const int waits = g_fill_waits, takes = g_fill_takes, nlog = g_fill_nlog;
    fake_complete(NVN_GPU_DONE_OK); fill_reset(0);
    rp_perf_take(pf);
    CHECK(g_gdraws == 3 && nlog == 3);
    CHECK(takes == 2 && waits == 0);
    CHECK(pf[RP_PERF_UBO_FILL_STEAL_N] == 2 && pf[RP_PERF_UBO_FILL_STEAL_B] == 1024 && pf[RP_PERF_UBO_FILL_WAIT_N] == 1);
    if (nlog == 3) {
        CHECK(g_fill_log[0].d[300] == 0x11 && g_fill_log[0].d[307] == 0x11);   // F1：改之前
        CHECK(g_fill_log[1].d[44] == 0x11 && g_fill_log[1].d[51] == 0x11);     // F2：改之前
        CHECK(g_fill_log[2].d[0] == 0x22 && g_fill_log[2].d[511] == 0x22);     // F3
    }
    CHECK(g_pmem[PG + 300] == 0xB2);
    g_snap_prog = NULL; g_flag_ub_steal = -1; pass_fresh();
}
// 上一笔的程序有片元阶段并带一条绑定，这一笔换成只有顶点阶段的程序：片元阶段要收到一次零条的绑定（清掉旧的），
// 否则后端拿旧的片元绑定去配没有片元布局的管线
static void t_stage_dropped_clears_binds(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0;
    memset(g_bind_calls, 0, sizeof g_bind_calls); memset(g_nfb, 0, sizeof g_nfb);
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    prog_t pa, pb; memset(&pa, 0, sizeof pa); pa.magic = MAGIC('P','R','O','G'); pa.serial = 994; pa.nstage = 2; pb = pa; pb.serial = 995; pb.nstage = 1;
    stream s = {0}; q_env(&s);                                   // q_env 会把假程序复位成「顶点 + 片元、没有布局项」
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_CBUF;
    g_snap_prog = &pa; g_snap_ok = 3; memset(g_snap_keys, 0, sizeof g_snap_keys); memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    for (int i = 0; i < 2; i++) { g_snap_stage[i] = (uint8_t)i; g_snap_keys[i][0] = (uint8_t)(i + 0x21); }
    p_bind_program_id *id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pa; id->serial = pa.serial; id->stages = 0x1f;
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 0; ub->addr = base + PG; ub->size = 64;
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_nfb[NVN_GPU_SH_FS] == 1);
    int fs_calls = g_bind_calls[NVN_GPU_SH_FS];
    g_fprog.sh[1] = NULL; g_fprog.nent[1] = 0;                   // 第二个程序：只有顶点阶段
    g_snap_prog = &pb; g_snap_ok = 1; g_snap_stage[1] = 0xff; g_snap_keys[0][0] = 0x31;
    stream t = {0};
    id = put(&t, OP_BIND_PROGRAM, sizeof *id); id->prog = &pb; id->serial = pb.serial; id->stages = 0x1f;
    q_draw(&t); end(&t); submit1(&t); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_last_pdesc.st[NVN_GPU_SH_FS].sh == NULL);
    CHECK(g_bind_calls[NVN_GPU_SH_FS] == fs_calls + 1 && g_nfb[NVN_GPU_SH_FS] == 0);
    g_fprog.sh[1] = &g_fsh[1];
    g_snap_prog = NULL; pass_fresh();
}
// 绘制要绑的存储缓冲，同一次提交里第二次同步时要录成传输、结束通道：必须发生在开通道之前，
// 不能在设了管线之后、发绑定的途中把通道结束掉（真后端随后收到通道外的绑定，置粘滞、整段作废；#697 Mali 真机）。
// 池不受写跟踪 → 每次用都整段同步；同一段里两笔都绑同一段存储缓冲，第二笔那次同步一定走传输。
// 合法 NVN 存储槽尚未绑定时，Ryujinx 为描述符保留 64 KiB 物理空缓冲，不能跳过整笔绘制。
// 不把真实地址解析失败或槽号越界当空槽；内部资源不登记到 guest 池。
static void t_storage_null_binding(void) {
    pass_fresh(); g_gdraws = g_resolve_rc = 0; memset(g_nfb, 0, sizeof g_nfb);
    int created = g_buf_creates, released = g_buf_releases;
    stream s = {0}; q_env(&s);
    g_fprog.nent[0] = 1;
    nvn_gpu_layout_ent *e = &g_fprog.ent[0][0];
    e->cls = NVN_GPU_RES_SSBO; e->src_kind = NVN_RB_SBUFP; e->src_reg = 0; e->access = NVN_GPU_ACC_R;
    q_draw(&s); q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && nvn_unimpl_kinds() == 0);
    nvn_gpu_buf empty = g_nfb[0] == 1 ? g_fb[0][0].u.b.buf : NULL;
    CHECK(empty && g_fb[0][0].u.b.off == 0 && g_fb[0][0].u.b.len == 0x10000);
    CHECK(g_buf_creates == created + 1); // 两笔复用同一对象
    e->src_reg = 16;
    stream bad = {0}; q_draw(&bad); end(&bad); submit1(&bad); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && nvn_unimpl_count("draw", "storage_buffer_unbound") == 1);
    e->src_reg = 0;
    stream missing = {0};
    p_bind_buf *b = put(&missing, OP_BIND_SSBO, sizeof *b); b->stage = 0; b->index = 0; b->addr = 0x12340; b->size = 32;
    q_draw(&missing); end(&missing); submit1(&missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && nvn_unimpl_count("draw", "storage_buffer_unresolved") == 1);
    // 没有可绑真实内存的存储缓冲槽（地址 0；或地址不在任何池里，k = 0 / 3 的 0x12340 不在池里）：
    //   照画，绑同一份 64 KiB 空缓冲、不另建缓冲、不登记回写（k = 3：可写的也一样）。UBO 指针来源（k = 2）没绑仍具名跳过，不被这条路接走。
    //   地址在池里、长度 0 的槽绑真实内存，在 t_sbuf_window 里测。
    for (int k = 0; k < 4; k++) {
        stream invalid = {0}; q_env(&invalid);
        g_fprog.nent[0] = 1; e = &g_fprog.ent[0][0];
        e->cls = NVN_GPU_RES_SSBO; e->src_kind = k == 2 ? NVN_RB_UBOP : NVN_RB_SBUFP; e->src_reg = 0; e->access = k == 3 ? NVN_GPU_ACC_R | NVN_GPU_ACC_W : NVN_GPU_ACC_R;
        if (k != 2) {
            b = put(&invalid, OP_BIND_SSBO, sizeof *b); b->stage = 0; b->index = 0;
            b->addr = k == 1 ? 0 : 0x12340; b->size = k == 1 ? 32 : 0;
        }
        int draws = g_gdraws, allocated = g_buf_creates; memset(g_nfb, 0, sizeof g_nfb);
        uint64_t unbound = nvn_unimpl_count("draw", "storage_buffer_unbound"); int kinds = nvn_unimpl_kinds();
        q_draw(&invalid); end(&invalid); submit1(&invalid); fake_complete(NVN_GPU_DONE_OK);
        if (k == 2) CHECK(g_gdraws == draws && nvn_unimpl_count("draw", "storage_buffer_unbound") == unbound + 1 && g_buf_creates == allocated);
        else {
            CHECK(g_gdraws == draws + 1 && nvn_unimpl_count("draw", "storage_buffer_unbound") == unbound && nvn_unimpl_kinds() == kinds && g_buf_creates == allocated);
            CHECK(g_nfb[0] == 1 && g_fb[0][0].u.b.buf == empty && g_fb[0][0].u.b.off == 0 && g_fb[0][0].u.b.len == 0x10000);
        }
    }
    pass_fresh();
    CHECK(g_buf_releases == released + 1);
}
static void t_storage_sync_before_pass(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0; g_bind_outside = 0; g_draw_outside = 0; g_pass_open = 0; g_nbw = 0;
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    int tracked = g_pg_tracked; g_pg_tracked = 0;
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_SSBO; g_fprog.ent[1][0].src_kind = NVN_RB_SBUFP;
    g_fprog.ent[1][0].src_reg = 0; g_fprog.ent[1][0].access = NVN_GPU_ACC_R;
    { p_bind_buf *sb = put(&s, OP_BIND_SSBO, sizeof *sb); sb->stage = 1; sb->index = 0; sb->addr = base + 2 * PG; sb->size = 64; }
    q_draw(&s);
    // 两笔之间回放自己按序改了 SSBO 所在的页：不受写跟踪的池只靠这个知道页变了（不再「每次使用都重传」，见 t_buf_untracked_once）
    { uint8_t *r = put(&s, OP_UPDATE_UB, 16 + 8); uint64_t d = base + 2 * PG + 8, n = 8; memcpy(r, &d, 8); memcpy(r + 8, &n, 8); memset(r + 16, 0xB2, 8); }
    q_draw(&s);
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2);
    CHECK(g_nbw == 2 && g_bw[0].pre == 1 && g_bw[1].pre == 0);  // 第二次同步确实录成了主段里的传输
    CHECK(g_bind_outside == 0 && g_draw_outside == 0);            // 每一次图形绑定、每一笔绘制都在通道里
    CHECK(nvn_unimpl_count("draw", "storage_buffer_unresolved") == 0);
    g_pg_tracked = tracked; pass_fresh();
}
// A buffer texture reads the registered pool range directly, with no sampler.
// A second untracked use must synchronize before its render pass is reopened.
// A sampled c2 image-table word must resolve the same handle as the driver table.
static void t_graphics_high_slots(void) {
    for (int st = 0; st < 3; st++) for (int slot = 0; slot < 8; slot++) for (int image = 0; image < 2; image++) {
        pass_fresh(); res_fresh(); g_arena_used = 0; g_resolve_rc = 0;
        stream s = {0}; q_env(&s);
        if (st == 2) g_fprog.sh[2] = &g_fsh[2];
        tex_t a = mk_tex(88901, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
        tex_t b = mk_tex(88902, 0x1000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
        samp_par sp; memset(&sp, 0, sizeof sp);
        nvn_be_reg_tex(3, &a, NULL); nvn_be_reg_tex(4, &b, NULL);
        nvn_be_reg_samp(0, &sp); nvn_be_set_tex_pool_n(16);
        nvn_gpu_layout_ent *e = &g_fprog.ent[st][0]; g_fprog.nent[st] = 1;
        e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 32 + slot;
        e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_R;
        rp_S.tex[st][32 + slot] = 3; rp_S.img[st][slot] = image ? 4 : 0;
        int before = g_gdraws;
        q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_gdraws == before + 1 && g_nfb[st] == 1 && g_fb[st][0].u.ts.view &&
              g_fb[st][0].u.ts.view->of->d.d.width == (image ? 16 : 8));
        rp_S.tex[st][32 + slot] = 0; rp_S.img[st][slot] = 0;
    }
    pass_fresh(); res_fresh();
}

// 图形阶段的整池数组（取表函数 pool_table_resolve 与计算派发共用，#1594 片 8 抽出）：行为不变的钉子。
//   期望（手算）：片元阶段 = 阶段 1；池长 8 / 4 → 布局项 count 9 / 5；一笔绘制 = 建 2 张表（IMAGE 8 项 / SAMPLER 4 项）、绑 2 次（阶段 1，项号 0 / 1）；
//   池长对不上 = 具名跳过这一笔、不绑表；池长 0 = 具名原因；恢复后又能画
static void t_graphics_pool(void) {
    for (int k = 0; k < 4; k++) {   // 0 = 正常；1 / 2 = 贴图池 / 采样器池的池长对不上；3 = 池长 0
        pass_fresh(); res_fresh(); g_arena_used = 0; g_resolve_rc = 0;
        stream s = {0}; q_env(&s);
        tex_t a = mk_tex(88911, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
        samp_par sp; memset(&sp, 0, sizeof sp);
        nvn_be_reg_tex(2, &a, NULL); nvn_be_reg_samp(1, &sp); nvn_be_set_tex_pool_n(8);
        rp_S.tex_base = 0; rp_S.samp_base = 0; rp_S.tex_pool_n = k == 3 ? 0 : 8; rp_S.samp_pool_n = 4;
        nvn_gpu_layout_ent *te = &g_fprog.ent[1][0], *se = &g_fprog.ent[1][1]; g_fprog.nent[1] = 2;
        te->cls = NVN_GPU_RES_TEX; te->src_kind = NVN_RB_TEXP; te->src_reg2 = UINT16_MAX; te->count = k == 1 ? 10 : k == 3 ? 1 : 9; te->access = NVN_GPU_ACC_R;
        se->cls = NVN_GPU_RES_SAMP; se->src_kind = NVN_RB_SAMPP; se->src_reg2 = UINT16_MAX; se->count = k == 2 ? 4 : 5; se->access = NVN_GPU_ACC_R;
        g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1; g_fprog.dcls[1][0] = 0;
        int g0 = g_gdraws, cr = g_pool_creates, pb = g_pool_binds; g_pool_bind_n = 0;
        q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        if (k == 0) {
            CHECK(g_gdraws == g0 + 1 && g_pool_creates - cr == 2 && g_pool_binds - pb == 2 && g_pool_bind_n == 2);
            CHECK(g_pool_bind_log[0].st == NVN_GPU_SH_FS && g_pool_bind_log[0].ent == 0 && g_pool_bind_log[0].kind == NVN_GPU_POOL_IMAGE && g_pool_bind_log[0].count == 8);
            CHECK(g_pool_bind_log[1].st == NVN_GPU_SH_FS && g_pool_bind_log[1].ent == 1 && g_pool_bind_log[1].kind == NVN_GPU_POOL_SAMPLER && g_pool_bind_log[1].count == 4);
        } else {
            CHECK(g_gdraws == g0 && g_pool_binds == pb);
            CHECK(nvn_unimpl_count("draw", k == 3 ? "pool_length_unsupported" : "pool_length_mismatch") == 1);
        }
        rp_S.tex_pool_n = rp_S.samp_pool_n = 0;
    }
    pass_fresh(); res_fresh();
}

static void t_tbuf_replay(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0;
    g_bind_outside = g_draw_outside = g_pass_open = g_nbw = 0;
    g_tbuf_created = g_tbuf_in_pass = 0; memset(g_tbuf_binds, 0, sizeof g_tbuf_binds);
    memset(g_tbuf_bound, 0, sizeof g_tbuf_bound); memset(g_pmem, 0, sizeof g_pmem);
    nvn_be_pool_add(g_pmem, sizeof g_pmem); int tracked = g_pg_tracked; g_pg_tracked = 0;
    const uint32_t values[4] = {11, 22, 33, 44}; memcpy(g_pmem + 2 * PG + 256, values, sizeof values);
    tex_t t = mk_tex(88801, 0, NVN_FORMAT_R32UI, 4, 1, 1, 0);
    t.d.target = NVN_TEXTURE_TARGET_BUFFER; t.mem = g_pmem + 2 * PG + 256; t.off = 2 * PG + 256;
    static pool_t pool; memset(&pool, 0, sizeof pool); pool.mem = g_pmem; pool.size = sizeof g_pmem; t.pool = &pool;
    nvn_tex_layout(&t.d, &t.L); nvn_be_set_tex_pool_n(8); nvn_be_reg_tex(3, &t, NULL);
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TBUF; e->src_kind = NVN_RB_TBUF; e->src_reg = 130; e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_R;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_BUFFER + 1; g_fprog.dcls[1][0] = 1;
    p_bind_handles *b = put(&s, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 3;
    q_draw(&s);
    // 两笔之间回放自己按序改了缓冲贴图的那 16 字节（内容同原值）：不受写跟踪的池由此知道页变了，第二笔要先重传再开通道
    { uint8_t *r = put(&s, OP_UPDATE_UB, 16 + sizeof values); uint64_t d = (uint64_t)(uintptr_t)g_pmem + 2 * PG + 256, n = sizeof values; memcpy(r, &d, 8); memcpy(r + 8, &n, 8); memcpy(r + 16, values, sizeof values); }
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_tbuf_binds[NVN_GPU_SH_FS] == 2);
    CHECK(g_tbuf_bound[NVN_GPU_SH_FS].buf && g_tbuf_bound[NVN_GPU_SH_FS].off == 2 * PG + 256 &&
          g_tbuf_bound[NVN_GPU_SH_FS].bytes == 16 && g_tbuf_bound[NVN_GPU_SH_FS].format == NVN_FORMAT_R32UI);
    CHECK(g_tbuf_created == 2 && !g_tbuf_in_pass && g_nbw == 2 && g_bw[0].pre == 1 && g_bw[1].pre == 0);
    CHECK(g_bind_outside == 0 && g_draw_outside == 0);
    // Tracked and unchanged bytes keep the pass open: the binding fast path
    // still must not reuse the previous draw's released temporary view.
    g_pg_tracked = 1; g_pg_gen[2] = ++g_dirty_now;
    int old_binds = g_tbuf_binds[NVN_GPU_SH_FS], old_draws = g_gdraws, old_passes = g_npd;
    stream stable = {0}; q_env(&stable);
    g_fprog.nent[1] = 1; e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TBUF; e->src_kind = NVN_RB_TBUF; e->src_reg = 130; e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_R;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_BUFFER + 1; g_fprog.dcls[1][0] = 1;
    b = put(&stable, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 3;
    q_draw(&stable); q_draw(&stable); end(&stable); submit1(&stable); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == old_draws + 2 && g_npd == old_passes + 1);
    CHECK(g_tbuf_binds[NVN_GPU_SH_FS] == old_binds + 2);
    g_pg_tracked = 0;
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1;
    e = &g_cprog.ent[0][0]; e->cls = NVN_GPU_RES_TBUF; e->src_kind = NVN_RB_TBUF; e->src_reg = 0; e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_R;
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_BUFFER + 1; g_cprog.dcls[0][0] = 1;
    rp_S.tex[5][0] = 3; int n = g_ndisp; p_dispatch dp = {.x=1, .y=1, .z=1};
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == n + 1 && g_tbuf_binds[NVN_GPU_SH_CS] == 1);
    CHECK(g_tbuf_bound[NVN_GPU_SH_CS].buf && g_tbuf_bound[NVN_GPU_SH_CS].off == 2 * PG + 256 &&
          g_tbuf_bound[NVN_GPU_SH_CS].bytes == 16 && g_tbuf_bound[NVN_GPU_SH_CS].format == NVN_FORMAT_R32UI);
    nvn_be_sub_begin(); g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_BUFFER + 1; g_cprog.dcls[0][0] = 1;
    view_t v = {0}; v.set_format = 1; v.format = NVN_FORMAT_RGBA8UI; nvn_be_reg_tex(3, &t, &v);
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && g_tbuf_bound[NVN_GPU_SH_CS].format == NVN_FORMAT_RGBA8UI && g_tbuf_bound[NVN_GPU_SH_CS].bytes == 16);
    t.d.width = 3; nvn_tex_layout(&t.d, &t.L); nvn_be_reg_tex(3, &t, &v); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && g_tbuf_bound[NVN_GPU_SH_CS].bytes == 12);
    v.format = NVN_FORMAT_R16UI; nvn_be_reg_tex(3, &t, &v); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_format_unsupported") == 1);
    v.format = NVN_FORMAT_RGBA8; nvn_be_reg_tex(3, &t, &v); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_format_mismatch") == 1);
    v.format = NVN_FORMAT_RGBA8UI; nvn_be_reg_tex(3, &t, &v); g_cprog.dtt[0][0] = 0;
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_format_mismatch") == 2);
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_BUFFER + 1;
    t.d.samples = 2; nvn_be_reg_tex(3, &t, &v); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_texture_shape") == 1);
    t.d.samples = 0;
    v.format = NVN_FORMAT_RGBA8UI; v.set_swizzle = 1; v.swz[0] = 4; v.swz[1] = 3; v.swz[2] = 2; v.swz[3] = 5;
    nvn_be_reg_tex(3, &t, &v); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_swizzle_unsupported") == 1);
    v.set_swizzle = 0; t.mem = g_pmem + sizeof g_pmem - 4; t.off = sizeof g_pmem - 4;
    nvn_be_reg_tex(3, &t, &v); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_buffer_unresolved") == 1);
    t.mem = g_pmem + 2 * PG + 258; t.off = 2 * PG + 258;
    nvn_be_reg_tex(3, &t, &v); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_view_create_failed") == 1);
    rp_S.tex[5][0] = 7; rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 3 && nvn_unimpl_count("draw", "tbuf_texture_unresolved") == 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_pg_tracked = tracked; nvn_be_pool_del(g_pmem); pass_fresh();
}
// 存储缓冲的绑定窗口（同 Ryujinx AlignDown(基址, 宿主对齐)）：基址不对齐时窗口从 基址 & −对齐 起、长度补余数（着色器按这个起点取数）；
// 长度夹到 maxStorageBufferRange，被夹短时具名记账、照画。图形（开通道前解析）与计算（发绑定时解析）两条路；UBOP 来源起点不动。
static void sbuf_case(int stage, int kind, uint64_t addr, uint64_t size, uint32_t align, uint64_t range) {
    g_caps_min_ssbo = align; g_caps_max_ssbo = range; rp_draw_reset(); g_arena_used = 0;   // 能力表是缓存的：改了替身值就重开一次
    stream s = {0};
    if (stage == 5) {
        g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0;
        memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
        g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][0].src_kind = (uint8_t)kind; g_cprog.ent[0][0].src_reg = 1; g_cprog.ent[0][0].access = NVN_GPU_ACC_R;
        p_bind_buf *b = put(&s, kind == NVN_RB_UBOP ? OP_BIND_UBO : OP_BIND_SSBO, sizeof *b); b->stage = 5; b->index = 1; b->addr = addr; b->size = size;
        p_dispatch *dp = put(&s, OP_DISPATCH, sizeof *dp); dp->x = dp->y = dp->z = 1;
    } else {
        q_env(&s);
        g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_SSBO; g_fprog.ent[1][0].src_kind = NVN_RB_SBUFP; g_fprog.ent[1][0].src_reg = 0; g_fprog.ent[1][0].access = NVN_GPU_ACC_R;
        p_bind_buf *b = put(&s, OP_BIND_SSBO, sizeof *b); b->stage = 1; b->index = 0; b->addr = addr; b->size = size;
        q_draw(&s);
    }
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
}
static void t_sbuf_window(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0; memset(g_nfb, 0, sizeof g_nfb);
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    const uint64_t cl0 = nvn_unimpl_count("draw", "storage_buffer_window_clamped");
    struct { uint64_t off, size; uint32_t align; uint64_t range; uint64_t want_off, want_len; int clamped; } g[] = {
        {2 * PG + 0x40, 64,     256, 0,      2 * PG, 0x80,   0},   // 不对齐：起点下取整、长度补余数
        {2 * PG,        64,     256, 0,      2 * PG, 64,     0},   // 负对照：已对齐，原样
        {2 * PG + 0x40, 64,     0,   0,      2 * PG + 0x40, 64, 0}, // 负对照：设备没报对齐，原样
        {PG,            0x3000, 0,   0x1000, PG,     0x1000, 1},   // 超范围上限：夹短、具名、照画
        {PG + 0x40,     0x3000, 256, 0x1000, PG,     0x1000, 1},   // 两样都有
        {PG,            0x1000, 0,   0x1000, PG,     0x1000, 0},   // 负对照：正好等于上限，不记账
        // 地址有效、长度 0：硬件按地址裸访存（LDG / STG 不拿槽长度判界），绑真实内存，长度 = 所在池剩下的部分（不是空缓冲）
        {3 * PG,        0,      0,   0,      3 * PG, (NPG - 3) * PG, 0},
        {3 * PG + 0x40, 0,      256, 0,      3 * PG, (NPG - 3) * PG, 0},   // 不对齐：起点下取整、长度补余数，正好到池尾
        {PG,            0,      0,   0x1000, PG,     0x1000, 1},           // 池剩余超范围上限：夹短、具名、照画
    };
    uint64_t want_cl = cl0;
    for (unsigned k = 0; k < sizeof g / sizeof g[0]; k++) {
        int d0 = g_gdraws;
        sbuf_case(1, NVN_RB_SBUFP, base + g[k].off, g[k].size, g[k].align, g[k].range);
        want_cl += (uint64_t)g[k].clamped;
        CHECK(g_gdraws == d0 + 1 && g_nfb[1] == 1 && g_fb[1][0].cls == NVN_GPU_RES_SSBO);
        CHECK(g_fb[1][0].u.b.off == g[k].want_off && g_fb[1][0].u.b.len == g[k].want_len);
        CHECK(nvn_unimpl_count("draw", "storage_buffer_window_clamped") == want_cl);
    }
    // 计算阶段：发绑定时自己解析，同一个窗口
    sbuf_case(5, NVN_RB_SBUFP, base + 2 * PG + 0x40, 64, 256, 0);
    CHECK(g_ndisp == 1 && g_ncb == 1 && g_cb[0].u.b.off == 2 * PG && g_cb[0].u.b.len == 0x80);
    sbuf_case(5, NVN_RB_SBUFP, base + PG, 0, 0, 0);   // 计算阶段同口径：长度 0 绑到池尾
    CHECK(g_ndisp == 1 && g_ncb == 1 && g_cb[0].u.b.off == PG && g_cb[0].u.b.len == (NPG - 1) * PG);
    {   // 可写、长度 0：显卡写回按实际绑定长度登记（不是槽里的 0），完成后落回游戏内存
        static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0, sizeof mirror); memset(g_pmem, 0, sizeof g_pmem);
        g_caps_min_ssbo = 0; g_caps_max_ssbo = 0; rp_draw_reset(); g_arena_used = 0;
        g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0;
        memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
        g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][0].src_kind = NVN_RB_SBUFP; g_cprog.ent[0][0].src_reg = 1;
        g_cprog.ent[0][0].access = NVN_GPU_ACC_R | NVN_GPU_ACC_W;
        g_buf_cpu_fake = mirror; nvn_be_sub_begin();
        rp_S.ssbo[5][1].addr = base + 3 * PG; rp_S.ssbo[5][1].size = 0;
        p_dispatch dp; memset(&dp, 0, sizeof dp); dp.x = dp.y = dp.z = 1;
        rp_dispatch(&g_fake_cmd, &dp, NULL);
        CHECK(g_ndisp == 1 && g_ncb == 1 && g_cb[0].u.b.len == (NPG - 3) * PG);
        memset(mirror + 3 * PG + PG, 0x5a, 16);   // 显卡写在槽起点后一页（槽长度 0 时只有按池剩余登记才够得着）
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); g_buf_cpu_fake = NULL;
        CHECK(g_pmem[4 * PG] == 0x5a && g_pmem[4 * PG + 15] == 0x5a);
        rp_S.ssbo[5][1].addr = 0;
    }
    {   // 统一缓冲来源（UBOP）长度 0 不按存储槽补长度：仍算没绑、具名跳过
        const uint64_t ub0 = nvn_unimpl_count("draw", "storage_buffer_unbound");
        sbuf_case(5, NVN_RB_UBOP, base + PG, 0, 0, 0);
        CHECK(g_ndisp == 0 && nvn_unimpl_count("draw", "storage_buffer_unbound") == ub0 + 1);
    }
    // UBOP 着色器按原基址取数：无法合法移动不对齐的窗口，应具名拒绝，而不是向 Vulkan 交非法偏移。
    const uint64_t unaligned0 = nvn_unimpl_count("draw", "storage_buffer_offset_unaligned");
    sbuf_case(5, NVN_RB_UBOP, base + 2 * PG + 0x40, 64, 256, 0);
    CHECK(g_ndisp == 0 && nvn_unimpl_count("draw", "storage_buffer_offset_unaligned") == unaligned0 + 1);
    // 已对齐的 UBOP 仍按原基址绑定。
    sbuf_case(5, NVN_RB_UBOP, base + 2 * PG, 64, 256, 0);
    CHECK(g_ndisp == 1 && g_ncb == 1 && g_cb[0].u.b.off == 2 * PG && g_cb[0].u.b.len == 64);
    sbuf_case(5, NVN_RB_UBOP, base + PG, 0x3000, 0, 0x1000);
    CHECK(g_ndisp == 1 && g_ncb == 1 && g_cb[0].u.b.off == PG && g_cb[0].u.b.len == 0x1000);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_window_clamped") == want_cl + 1);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_unresolved") == 0 && nvn_unimpl_count("draw", "storage_buffer_offset_unaligned") == unaligned0 + 1);
    g_caps_min_ssbo = 0; g_caps_max_ssbo = 0; rp_draw_reset(); pass_fresh();
}
// 存储缓冲的长度伸出所在池的池尾（真机：4096 字节的池里偏移 2560 处绑 1872 字节）：窗口夹到池尾照画，具名记账
//   storage_buffer_past_pool_end_clamped；不是范围上限那一条（storage_buffer_window_clamped 不动）。
// 负对照：正好绑到池尾的不记账；起点不在任何池里的仍然整笔拒绝（storage_buffer_unresolved），不被这条路接走。
static void t_sbuf_past_pool_end(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0; memset(g_nfb, 0, sizeof g_nfb);
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    const uint64_t tail = (NPG - 1) * PG + 2560, rem = PG - 2560;   // 最后一页偏移 2560：到池尾还剩 1536 字节
    const uint64_t pe0 = nvn_unimpl_count("draw", "storage_buffer_past_pool_end_clamped");
    const uint64_t cl0 = nvn_unimpl_count("draw", "storage_buffer_window_clamped");
    const uint64_t un0 = nvn_unimpl_count("draw", "storage_buffer_unresolved");
    int d0 = g_gdraws;
    sbuf_case(1, NVN_RB_SBUFP, base + tail, 1872, 0, 0);   // 伸出池尾 336 字节：照画，绑到池尾为止
    CHECK(g_gdraws == d0 + 1 && g_nfb[1] == 1 && g_fb[1][0].cls == NVN_GPU_RES_SSBO);
    CHECK(g_fb[1][0].u.b.off == tail && g_fb[1][0].u.b.len == rem);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_past_pool_end_clamped") == pe0 + 1);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_window_clamped") == cl0 && nvn_unimpl_count("draw", "storage_buffer_unresolved") == un0);
    d0 = g_gdraws;
    sbuf_case(1, NVN_RB_SBUFP, base + tail + 0x40, 1872, 256, 0);   // 不对齐：起点先下取整，再夹到池尾
    CHECK(g_gdraws == d0 + 1 && g_nfb[1] == 1 && g_fb[1][0].u.b.off == tail && g_fb[1][0].u.b.len == rem);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_past_pool_end_clamped") == pe0 + 2);
    d0 = g_gdraws;
    sbuf_case(1, NVN_RB_SBUFP, base + tail, rem, 0, 0);   // 负对照：正好绑到池尾，原样、不记账
    CHECK(g_gdraws == d0 + 1 && g_nfb[1] == 1 && g_fb[1][0].u.b.off == tail && g_fb[1][0].u.b.len == rem);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_past_pool_end_clamped") == pe0 + 2);
    sbuf_case(5, NVN_RB_SBUFP, base + tail, 1872, 0, 0);   // 计算阶段同一个窗口
    CHECK(g_ndisp == 1 && g_ncb == 1 && g_cb[0].u.b.off == tail && g_cb[0].u.b.len == rem);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_past_pool_end_clamped") == pe0 + 3);
    d0 = g_gdraws;
    sbuf_case(1, NVN_RB_SBUFP, 0x12340, 32, 0, 0);   // 负对照：起点不在任何池里，没有可夹的池尾，整笔拒绝
    CHECK(g_gdraws == d0 && nvn_unimpl_count("draw", "storage_buffer_unresolved") == un0 + 1);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_past_pool_end_clamped") == pe0 + 3 && nvn_unimpl_count("draw", "storage_buffer_window_clamped") == cl0);
    g_caps_min_ssbo = 0; g_caps_max_ssbo = 0; rp_draw_reset(); pass_fresh();
}
static void q_counter(stream *s, uint32_t op, uint64_t type, uint64_t *dst) { uint8_t *r = put(s, op, 16); uint64_t ad = (uint64_t)(uintptr_t)dst; memcpy(r, &type, 8); memcpy(r + 8, &ad, 8); }

static void put_set_rt_v(stream *s, const tex_t *color, const view_t *cv) {   // 一个颜色槽，带视图
    p_set_rt r; memset(&r, 0, sizeof r); r.n = 1; r.color[0] = color; if (cv) { r.has_cview[0] = 1; r.cview[0] = *cv; }
    uint8_t *a = put(s, OP_SET_RT, set_rt_size(&r)); p_set_rt_c *h = set_rt_put(a, &r);
    const tex_t **pt = (const tex_t **)(h + 1); tex_t *snap = (tex_t *)(a + set_rt_snap_off(h)); snap[0] = *color; pt[0] = &snap[0];
}
// 深度贴图经颜色视图当渲染目标：画进一张视图格式的影子，通道前拷进（整屏清除时不拷）、通道后拷回；走不了影子的当空槽。
// 期望值手算：64x32 的 D16，一个像素 2 字节 → 行 128 字节、整张 4096 字节。
static void t_pass_depth_shadow(void) {
    pass_fresh(); g_ntb = 0; g_gdraws = 0;
    tex_t z = mk_tex(66, 0, NVN_FORMAT_DEPTH16, 64, 32, 1, 0);
    view_t v; memset(&v, 0, sizeof v); v.set_format = 1; v.format = NVN_FORMAT_R16;
    int tc = g_tex_creates;
    stream s = {0}; put_set_rt_v(&s, &z, &v);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[0] = 0.5f; end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pass_ends == 1 && g_pd[0].w == 64 && g_pd[0].h == 32 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR);
    nvn_gpu_tex sh = g_pd[0].color[0].view ? g_pd[0].color[0].view->of : NULL;
    CHECK(sh && sh->d.d.format == NVN_FORMAT_R16 && sh->d.d.width == 64 && sh->d.d.height == 32 && (sh->d.usage & NVN_GPU_TEX_RENDER));
    CHECK(g_tex_creates == tc + 2);                                       // 基底 + 影子
    CHECK(g_ntb == 2 && g_tb[0].to_tex == 0 && g_tb[0].t == sh && g_tb[0].ended == 1);   // 整屏清除：通道前不拷；通道后影子 → 缓冲 → 基底
    CHECK(g_tb[1].to_tex == 1 && g_tb[1].t != sh && g_tb[1].t && g_tb[1].t->d.d.format == NVN_FORMAT_DEPTH16 && g_tb[1].row == 128 && g_tb[1].img == 4096 && g_tb[1].w == 64 && g_tb[1].h == 32);
    CHECK(nvn_unimpl_kinds() == 0);
    // 第二次提交：不清除直接画 → 通道前先把基底的内容拷进影子；影子复用，不新建
    nvn_gpu_tex base = g_tb[1].t;
    stream s2 = {0}; q_env(&s2); put_set_rt_v(&s2, &z, &v); q_draw(&s2); end(&s2);
    int tc2 = g_tex_creates;
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_npd == 2 && g_pd[1].color[0].load == NVN_GPU_LOAD_LOAD && g_pd[1].color[0].view && g_pd[1].color[0].view->of == sh && g_tex_creates == tc2);
    CHECK(g_ntb == 6 && g_tb[2].to_tex == 0 && g_tb[2].t == base && g_tb[2].begun == 1 && g_tb[3].to_tex == 1 && g_tb[3].t == sh && g_tb[3].begun == 1);
    CHECK(g_tb[4].to_tex == 0 && g_tb[4].t == sh && g_tb[4].ended == 2 && g_tb[5].to_tex == 1 && g_tb[5].t == base);
    // 走不了影子的（D24S8：存储字节不是游戏的字节；没有换格式的视图）：当空槽并记账；通道里一个附件都没有 → 不开通道
    pass_fresh(); g_ntb = 0;
    tex_t d24 = mk_tex(67, 0, NVN_FORMAT_DEPTH24_STENCIL8, 16, 16, 1, 0);
    stream s3 = {0}; put_set_rt_v(&s3, &d24, NULL);
    cc = put(&s3, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s3);
    submit1(&s3); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 0 && g_ntb == 0 && nvn_unimpl_count("pass", "color_target_depth") == 1 && nvn_unimpl_count("pass", "no_attachment") == 1);
    pass_fresh();
}

// 不挂任何目标的绘制（NVN 允许：片元只写存储 / 只为遮挡计数；安卓真机一款大作每帧约 4 笔被跳过）。
// 期望值手算：后端没有 NVN_GPU_CAP_NO_ATTACH_PASS → 不开通道、记 no_attachment 一笔、绘制 0 笔；
//   有 → 通道 0 个颜色槽、无深度，尺寸 = 视口右下角（24×20）；下一笔视口 40×30 超出 → 收通道按 40×30 重开；
//   再一笔视口 10×10 不超出 → 留在同一通道，裁剪按 40×30 夹（不夹成缺省的 1×1）= (0, 0, 10, 10)。
static void vp_sc(stream *s, float w, float h) {
    p_viewports *vp = put(s, OP_VIEWPORTS, sizeof *vp); vp->n = 1; vp->v[0][2] = w; vp->v[0][3] = h;
    p_scissor *sc = put(s, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = (int32_t)w; sc->h = (int32_t)h;
}
static void t_pass_no_attachment(void) {
    pass_fresh(); g_caps_extra = 0;
    int d0 = g_gdraws;
    stream s = {0}; q_env(&s); put_set_rt(&s, NULL, NULL); vp_sc(&s, 24, 20); q_draw(&s); end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 && g_npd == 0 && nvn_unimpl_count("pass", "no_attachment") == 1);
    pass_fresh(); g_caps_extra = NVN_GPU_CAP_NO_ATTACH_PASS;
    d0 = g_gdraws;
    stream s2 = {0}; q_env(&s2); put_set_rt(&s2, NULL, NULL);
    vp_sc(&s2, 24, 20); q_draw(&s2);
    vp_sc(&s2, 40, 30); q_draw(&s2);
    vp_sc(&s2, 10, 10); q_draw(&s2);
    end(&s2);
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 3 && g_npd == 2 && g_pass_ends == 2);
    CHECK(g_pd[0].ncolor == 0 && !g_pd[0].depth.view && !g_pd[0].stencil.view && g_pd[0].w == 24 && g_pd[0].h == 20);
    CHECK(g_pd[1].ncolor == 0 && g_pd[1].w == 40 && g_pd[1].h == 30);
    CHECK(g_last_dyn.has_sc && g_last_dyn.sc[0] == 0 && g_last_dyn.sc[1] == 0 && g_last_dyn.sc[2] == 10 && g_last_dyn.sc[3] == 10);
    CHECK(nvn_unimpl_kinds() == 0);
    g_caps_extra = 0; pass_fresh();
}

// 块压缩贴图经「一块当一个像素」的非压缩视图当渲染目标（#709，对照 nvn_mtl.m bcrt_attach）：同深度影子一套，影子按块数建。
// 期望值手算：64x32 的 BC1（RGBA_DXT1，一块 4x4、8 字节）配 RG32UI 视图 → 影子 16x8、一个像素 8 字节 → 行 128 字节、整张 1024 字节；
// 基底那边的拷贝区域按像素给整级 64x32。BC3（RGBA_DXT5，一块 16 字节）16x16 配 RGBA32UI → 影子 4x4、行 64、整张 256。
static void t_pass_bc_shadow(void) {
    pass_fresh(); g_ntb = 0; g_gdraws = 0;
    tex_t b1 = mk_tex(68, 0, NVN_FORMAT_RGBA_DXT1, 64, 32, 1, 0);
    view_t v; memset(&v, 0, sizeof v); v.set_format = 1; v.format = NVN_FORMAT_RG32UI;
    int tc = g_tex_creates;
    // ① 裁剪矩形按块数正好盖满（16x8）→ 算整屏清除，折成通道的 CLEAR，不走矩形清
    stream s = {0}; put_set_rt_v(&s, &b1, &v);
    p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 16; sc->h = 8;
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_ncr == 0 && g_pd[0].w == 16 && g_pd[0].h == 8 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR);
    nvn_gpu_tex sh = g_pd[0].color[0].view ? g_pd[0].color[0].view->of : NULL;
    CHECK(sh && sh->d.d.format == NVN_FORMAT_RG32UI && sh->d.d.width == 16 && sh->d.d.height == 8 && (sh->d.usage & NVN_GPU_TEX_RENDER));
    CHECK(g_tex_creates == tc + 2);                                       // 基底 + 影子
    CHECK(g_ntb == 2 && g_tb[0].to_tex == 0 && g_tb[0].t == sh && g_tb[0].w == 16 && g_tb[0].h == 8 && g_tb[0].row == 128 && g_tb[0].img == 1024);   // 整屏清除：通道前不拷；通道后影子 → 缓冲
    CHECK(g_tb[1].to_tex == 1 && g_tb[1].t && g_tb[1].t != sh && g_tb[1].t->d.d.format == NVN_FORMAT_RGBA_DXT1 && g_tb[1].w == 64 && g_tb[1].h == 32 && g_tb[1].row == 128 && g_tb[1].img == 1024);   // 缓冲 → 基底：区域按像素
    CHECK(nvn_unimpl_kinds() == 0);
    // ② 不清除直接画：通道前基底 → 缓冲 → 影子；绘制的裁剪按块数夹（给的 100x100 夹成 16x8）；按块数没盖满的清除走矩形清，矩形也是块数
    nvn_gpu_tex base = g_tb[1].t;
    stream s2 = {0}; q_env(&s2); put_set_rt_v(&s2, &b1, &v);
    sc = put(&s2, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 100; sc->h = 100; q_draw(&s2);
    sc = put(&s2, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 8; sc->h = 8;
    cc = put(&s2, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s2);
    int tc2 = g_tex_creates;
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_npd == 2 && g_pd[1].w == 16 && g_pd[1].h == 8 && g_pd[1].color[0].load == NVN_GPU_LOAD_LOAD && g_pd[1].color[0].view && g_pd[1].color[0].view->of == sh && g_tex_creates == tc2);
    CHECK(g_last_dyn.has_sc && g_last_dyn.sc[0] == 0 && g_last_dyn.sc[1] == 0 && g_last_dyn.sc[2] == 16 && g_last_dyn.sc[3] == 8);
    CHECK(g_ncr == 1 && g_cr[0].d.x == 0 && g_cr[0].d.y == 0 && g_cr[0].d.w == 8 && g_cr[0].d.h == 8);
    CHECK(g_ntb == 6 && g_tb[2].to_tex == 0 && g_tb[2].t == base && g_tb[2].w == 64 && g_tb[2].h == 32 && g_tb[2].begun == 1 && g_tb[3].to_tex == 1 && g_tb[3].t == sh && g_tb[3].w == 16 && g_tb[3].h == 8 && g_tb[3].begun == 1);
    CHECK(g_tb[4].to_tex == 0 && g_tb[4].t == sh && g_tb[4].ended == 2 && g_tb[5].to_tex == 1 && g_tb[5].t == base && g_tb[5].w == 64 && g_tb[5].h == 32);
    // ③ BC3 配 RGBA32UI（一块 16 字节）
    pass_fresh(); g_ntb = 0;
    tex_t b3 = mk_tex(69, 0, NVN_FORMAT_RGBA_DXT5, 16, 16, 1, 0); view_t v3; memset(&v3, 0, sizeof v3); v3.set_format = 1; v3.format = NVN_FORMAT_RGBA32UI;
    stream s3 = {0}; put_set_rt_v(&s3, &b3, &v3); cc = put(&s3, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s3);
    submit1(&s3); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_pd[0].w == 4 && g_pd[0].h == 4 && g_pd[0].color[0].view && g_pd[0].color[0].view->of->d.d.format == NVN_FORMAT_RGBA32UI);
    CHECK(g_ntb == 2 && g_tb[0].row == 64 && g_tb[0].img == 256 && g_tb[1].w == 16 && g_tb[1].h == 16 && nvn_unimpl_kinds() == 0);
    // ④ 走不了影子的，照旧当空槽记账：一个像素的字节对不上一块的字节、没有换格式的视图、基底不能直接采样（后端走计算解码）
    pass_fresh(); g_ntb = 0;
    stream s4 = {0}; view_t vbad = v3;                                                 // BC1 一块 8 字节，RGBA32UI 一个像素 16 字节
    put_set_rt_v(&s4, &b1, &vbad); cc = put(&s4, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf;
    put_set_rt_v(&s4, &b1, NULL); cc = put(&s4, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s4);
    submit1(&s4); fake_complete(NVN_GPU_DONE_OK);
    // 两条清除是同一个子资源（序号 + 级 + 层 + 类别相同，只是视图记录不同）：挂起表按这个身份合成一项，所以只记账一次
    CHECK(g_npd == 0 && g_ntb == 0 && nvn_unimpl_count("pass", "color_target_compressed") == 1);
    pass_fresh(); g_ntb = 0; g_caps_nosample = NVN_FORMAT_RGBA_DXT1;   // 每种情况各自从干净的会话起（待折清除落地时开不了通道就丢掉，见 rp_pass_flush）
    stream s5 = {0}; put_set_rt_v(&s5, &b1, &v); cc = put(&s5, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s5);
    submit1(&s5); fake_complete(NVN_GPU_DONE_OK);
    g_caps_nosample = ~0u;
    CHECK(g_npd == 0 && g_ntb == 0 && nvn_unimpl_count("pass", "color_target_compressed") == 1);
    pass_fresh();
}

// 三维的块压缩基底当渲染目标（#773；对照 nvn_mtl.m bcrt_attach 的 z = depthPlane）：一个深度切片一个通道，同一套影子，影子仍是二维。
// 视图的层号在三维贴图上是深度切片，原样当基底拷贝的 layer（后端在三维贴图上把它转成 zoffset、depth 1，见 android/tests/vk 的 C86c）。
// 期望值手算：64x32x4 的 BC1（一块 4x4、8 字节）配 RG32UI → 影子 16x8、一个像素 8 字节 → 行 128、整张 1024；视图切片 2、第 0 级 →
// 通道前后的基底拷贝都是 level 0 / layer 2、区域 64x32；换成切片 3：影子复用不新建、基底拷贝换成 layer 3。
// 两级的同一张图、视图第 1 级切片 1：该级 32x16 → 影子 8x4、行 64、整张 256，基底拷贝 level 1 / layer 1、区域 32x16（按像素，不按块取整）。
static tex_t mk_tex3d(uint64_t serial, int fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels) {
    tex_t t = mk_tex(serial, 0, fmt, w, h, levels, 0); t.d.target = NVN_TEXTURE_TARGET_3D; t.d.depth = depth; nvn_tex_layout(&t.d, &t.L); return t;
}
static void t_pass_bc_shadow_3d(void) {
    pass_fresh(); g_ntb = 0; g_gdraws = 0;
    tex_t b3 = mk_tex3d(70, NVN_FORMAT_RGBA_DXT1, 64, 32, 4, 1);
    view_t v; memset(&v, 0, sizeof v); v.set_format = 1; v.format = NVN_FORMAT_RG32UI;
    view_t v2 = v; v2.set_layers = 1; v2.min_layer = 2; v2.nlayers = 1;
    // ① 不清除直接画切片 2：通道前基底的切片 2 → 缓冲 → 影子；通道后影子 → 缓冲 → 基底的同一个切片
    stream s = {0}; q_env(&s); put_set_rt_v(&s, &b3, &v2); q_draw(&s); end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_npd == 1 && g_pd[0].ncolor == 1 && g_pd[0].w == 16 && g_pd[0].h == 8 && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD);
    nvn_gpu_tex sh = g_npd >= 1 && g_pd[0].color[0].view ? g_pd[0].color[0].view->of : NULL;   // g_pd 不随 pass_fresh 清：没开通道时里面是上一个用例留下的
    CHECK(sh && sh->d.d.target == NVN_TEXTURE_TARGET_2D && sh->d.d.format == NVN_FORMAT_RG32UI && sh->d.d.width == 16 && sh->d.d.height == 8 && (sh->d.usage & NVN_GPU_TEX_RENDER));
    nvn_gpu_tex base = g_ntb > 0 ? g_tb[0].t : NULL;
    CHECK(base && base != sh && base->d.d.target == NVN_TEXTURE_TARGET_3D && base->d.d.format == NVN_FORMAT_RGBA_DXT1 && base->d.d.depth == 4);
    CHECK(g_ntb == 4 && g_tb[0].to_tex == 0 && g_tb[0].level == 0 && g_tb[0].layer == 2 && g_tb[0].w == 64 && g_tb[0].h == 32 && g_tb[0].row == 128 && g_tb[0].img == 1024);
    CHECK(g_tb[1].to_tex == 1 && g_tb[1].t == sh && g_tb[1].layer == 0 && g_tb[1].w == 16 && g_tb[1].h == 8 && g_tb[1].row == 128 && g_tb[1].img == 1024);   // 影子是二维的，层 0
    CHECK(g_tb[2].to_tex == 0 && g_tb[2].t == sh && g_tb[2].layer == 0 && g_tb[2].w == 16 && g_tb[2].h == 8 && g_tb[2].ended == 1);
    CHECK(g_tb[3].to_tex == 1 && g_tb[3].t == base && g_tb[3].level == 0 && g_tb[3].layer == 2 && g_tb[3].w == 64 && g_tb[3].h == 32 && g_tb[3].row == 128 && g_tb[3].img == 1024);
    CHECK(nvn_unimpl_count("pass", "color_target_compressed_3d") == 0 && nvn_unimpl_count("pass", "color_target_compressed") == 0 && nvn_unimpl_count("pass", "no_attachment") == 0);
    // ② 同一张图的切片 3（另一次提交）：影子复用不新建，基底拷贝取自己的切片
    g_ntb = 0;
    view_t v3 = v2; v3.min_layer = 3;
    stream s2 = {0}; q_env(&s2); put_set_rt_v(&s2, &b3, &v3); q_draw(&s2); end(&s2);
    int tc = g_tex_creates;
    submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_npd == 2 && g_pd[1].w == 16 && g_pd[1].h == 8 && g_pd[1].color[0].view && g_pd[1].color[0].view->of == sh && g_tex_creates == tc);
    CHECK(g_ntb == 4 && g_tb[0].to_tex == 0 && g_tb[0].t == base && g_tb[0].layer == 3 && g_tb[3].to_tex == 1 && g_tb[3].t == base && g_tb[3].layer == 3);
    // ③ 不设层号（视图只换格式）：取切片 0
    g_ntb = 0;
    stream s3 = {0}; q_env(&s3); put_set_rt_v(&s3, &b3, &v); q_draw(&s3); end(&s3);
    submit1(&s3); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 3 && g_ntb == 4 && g_tb[0].layer == 0 && g_tb[3].layer == 0 && g_tb[3].t == base);
    // ④ 同一条命令流里连着两个切片（游戏一个深度切片一个通道地循环）：各自拷进拷出，第二个通道的拷进在第一个通道的拷回之后，用的是同一个影子
    pass_fresh(); g_ntb = 0; g_gdraws = 0;
    stream s6 = {0}; q_env(&s6); put_set_rt_v(&s6, &b3, &v2); q_draw(&s6); put_set_rt_v(&s6, &b3, &v3); q_draw(&s6); end(&s6);
    submit1(&s6); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_npd == 2 && g_ntb == 8);
    CHECK(g_tb[0].to_tex == 0 && g_tb[0].layer == 2 && g_tb[3].to_tex == 1 && g_tb[3].layer == 2 && g_tb[3].ended == 1);                                   // 第一个通道：切片 2 拷进、拷回
    CHECK(g_tb[4].to_tex == 0 && g_tb[4].layer == 3 && g_tb[4].begun == 1 && g_tb[7].to_tex == 1 && g_tb[7].layer == 3 && g_tb[7].t == g_tb[0].t);          // 第二个通道：切片 3，拷进时第二个通道还没开
    CHECK(g_tb[1].t == g_tb[5].t && g_tb[2].t == g_tb[1].t && g_tb[6].t == g_tb[1].t && g_tb[1].layer == 0 && g_tb[5].layer == 0);                         // 两个通道共用一个二维影子
    // ⑤ 两级的三维贴图，视图第 1 级切片 1（该级 32x16x2）
    pass_fresh(); g_ntb = 0; g_gdraws = 0;
    tex_t bm = mk_tex3d(71, NVN_FORMAT_RGBA_DXT1, 64, 32, 4, 2);
    view_t vm = v; vm.set_levels = 1; vm.base_level = 1; vm.nlevels = 1; vm.set_layers = 1; vm.min_layer = 1; vm.nlayers = 1;
    stream s4 = {0}; q_env(&s4); put_set_rt_v(&s4, &bm, &vm); q_draw(&s4); end(&s4);
    submit1(&s4); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_npd == 1 && g_pd[0].w == 8 && g_pd[0].h == 4 && g_pd[0].color[0].view && g_pd[0].color[0].view->of->d.d.width == 8 && g_pd[0].color[0].view->of->d.d.height == 4);
    CHECK(g_ntb == 4 && g_tb[0].to_tex == 0 && g_tb[0].level == 1 && g_tb[0].layer == 1 && g_tb[0].w == 32 && g_tb[0].h == 16 && g_tb[0].row == 64 && g_tb[0].img == 256);
    CHECK(g_tb[3].to_tex == 1 && g_tb[3].t == g_tb[0].t && g_tb[3].level == 1 && g_tb[3].layer == 1 && g_tb[3].w == 32 && g_tb[3].h == 16);
    // ⑥ 整屏清除（裁剪矩形按块数盖满 16x8）：折成通道的 CLEAR，通道前不拷；通道后只写回切片 2
    pass_fresh(); g_ntb = 0;
    stream s5 = {0}; put_set_rt_v(&s5, &b3, &v2);
    p_scissor *sc = put(&s5, OP_SCISSOR, sizeof *sc); sc->x = 0; sc->y = 0; sc->w = 16; sc->h = 8;
    p_clear_color *cc = put(&s5, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s5);
    submit1(&s5); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && g_ncr == 0 && g_pd[0].w == 16 && g_pd[0].h == 8 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR);
    CHECK(g_ntb == 2 && g_tb[0].to_tex == 0 && g_tb[0].layer == 0 && g_tb[1].to_tex == 1 && g_tb[1].layer == 2 && g_tb[1].w == 64 && g_tb[1].h == 32);
    CHECK(nvn_unimpl_kinds() == 0);
    // ⑦ 程序里的真实配置（#773 认领帖的验收）：32x32x32 的 BC4（RGTC1，一块 8 字节）配 RGBA16UI（一个像素 8 字节）→ 影子 8x8、行 64、整张 512；
    //    视图切片 5 → 通道前后两次基底拷贝的 layer 都是 5、区域 32x32
    pass_fresh(); g_ntb = 0; g_gdraws = 0;
    tex_t b4 = mk_tex3d(72, NVN_FORMAT_RGTC1_UNORM, 32, 32, 32, 1);
    view_t v5; memset(&v5, 0, sizeof v5); v5.set_format = 1; v5.format = NVN_FORMAT_RGBA16UI; v5.set_layers = 1; v5.min_layer = 5; v5.nlayers = 1;
    stream s7 = {0}; q_env(&s7); put_set_rt_v(&s7, &b4, &v5); q_draw(&s7); end(&s7);
    submit1(&s7); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_npd == 1 && g_pd[0].w == 8 && g_pd[0].h == 8 && g_pd[0].color[0].view && g_pd[0].color[0].view->of->d.d.target == NVN_TEXTURE_TARGET_2D);
    CHECK(g_npd == 1 && g_pd[0].color[0].view->of->d.d.format == NVN_FORMAT_RGBA16UI && g_pd[0].color[0].view->of->d.d.width == 8 && g_pd[0].color[0].view->of->d.d.height == 8);
    CHECK(g_ntb == 4 && g_tb[0].to_tex == 0 && g_tb[0].level == 0 && g_tb[0].layer == 5 && g_tb[0].w == 32 && g_tb[0].h == 32 && g_tb[0].row == 64 && g_tb[0].img == 512);
    CHECK(g_tb[1].to_tex == 1 && g_tb[1].layer == 0 && g_tb[1].w == 8 && g_tb[1].h == 8 && g_tb[1].row == 64 && g_tb[1].img == 512 && g_tb[2].to_tex == 0 && g_tb[2].layer == 0);
    CHECK(g_tb[3].to_tex == 1 && g_tb[3].t == g_tb[0].t && g_tb[3].level == 0 && g_tb[3].layer == 5 && g_tb[3].w == 32 && g_tb[3].h == 32 && g_tb[3].row == 64 && g_tb[3].img == 512);
    CHECK(nvn_unimpl_count("pass", "color_target_compressed_3d") == 0 && nvn_unimpl_count("pass", "color_target_compressed") == 0 && nvn_unimpl_count("pass", "no_attachment") == 0);
    pass_fresh();
}

static void put_set_rt_n(stream *s, const tex_t *const *color, int n, const tex_t *depth) {   // 多个颜色槽（可以有空位）+ 深度，都不带视图
    p_set_rt r; memset(&r, 0, sizeof r); r.n = n; for (int i = 0; i < n; i++) r.color[i] = color[i]; r.depth = depth;
    uint8_t *a = put(s, OP_SET_RT, set_rt_size(&r)); p_set_rt_c *h = set_rt_put(a, &r);
    const tex_t **pt = (const tex_t **)(h + 1); tex_t *snap = (tex_t *)(a + set_rt_snap_off(h)); uint32_t k = 0;
    for (int i = 0; i < n; i++) if (color[i]) { snap[k] = *color[i]; pt[k] = &snap[k]; k++; }
    if (depth) { snap[k] = *depth; pt[k] = &snap[k]; }
}
static void t_pass_clear_only_attachments(void) {
    enum { OFF, TRIM, MIXED, ALIAS, ZERO, DEPTH, STENCIL, CASES };
    for (int mode = OFF; mode < CASES; mode++) {
        pass_fresh(); g_flag_clear_only_attachments = mode != OFF;
        tex_t a = mk_tex(941, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
        tex_t b = mk_tex(942, mode == ALIAS ? 0 : 0x4000, NVN_FORMAT_RGBA8, mode == MIXED ? 8 : 16, 16, 1, 0);
        tex_t ds = mk_tex(943, 0x8000, NVN_FORMAT_DEPTH24_STENCIL8, 16, 16, 1, 0);
        const int aspect = mode == DEPTH || mode == STENCIL;
        if (aspect) memset(a.mem, 0x7e, (size_t)a.L.size);
        if (mode != ZERO) memset(b.mem, 0x7e, (size_t)b.L.size);
        if (mode == ALIAS) {
            // Both registrations deliberately resolve the same uninitialized GPU storage.
            memset(b.mem, 0, (size_t)b.L.size);
        }
        stream s = {0};
        const tex_t *colors[] = {&a, &b};
        put_set_rt_n(&s, colors, 2, aspect ? &ds : NULL);
        if (aspect) {
            p_clear_ds *cl = put(&s, OP_CLEAR_DS, sizeof *cl);
            cl->depth_mask = mode == DEPTH; cl->depth = 0.25f;
            cl->stencil_mask = mode == STENCIL ? 0xff : 0; cl->stencil = 7;
        } else {
            p_clear_color *cl = put(&s, OP_CLEAR_COLOR, sizeof *cl);
            cl->index = 0; cl->mask = 0xf; cl->rgba[2] = 1;
        }
        end(&s);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_npd == (mode == MIXED ? 2 : 1) && g_pass_ends == g_npd);
        const nvn_gpu_pass_desc *p = &g_pd[g_npd - 1];
        const int trim = mode == TRIM || aspect;
        CHECK((p->color[1].view == NULL) == trim); // A preserving slot is unnecessary only in the guarded shape.
        CHECK((nvn_be_tex_gpu_owned(b.serial) == 0) == trim);
        if (aspect) {
            CHECK(!p->color[0].view && nvn_be_tex_gpu_owned(a.serial) == 0);
            CHECK(p->depth.view && p->stencil.view == p->depth.view);
            CHECK(p->depth.load == (mode == DEPTH ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD));
            CHECK(p->stencil.load == (mode == STENCIL ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD));
            CHECK(p->depth.store == NVN_GPU_STORE_STORE && p->stencil.store == NVN_GPU_STORE_STORE);
        } else {
            CHECK(p->color[0].view && nvn_be_tex_gpu_owned(a.serial) == 1);
            if (mode == ZERO) CHECK(p->color[1].view && p->color[1].load == NVN_GPU_LOAD_CLEAR && !p->color[1].clear.u[0]);
            if (mode == MIXED) CHECK(g_pd[0].w == 16 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && p->w == 8);
        }
        printf("clear-only attachments case=%d expected_trim=%d attached_b=%d gpu_owned_b=%d\n",
               mode, trim, p->color[1].view != NULL, nvn_be_tex_gpu_owned(b.serial));
    }
    g_flag_clear_only_attachments = 0;
    pass_fresh();
}

static void t_pass_area_then_clear_only(void) {
    const int old_area = g_flag_pass_area;
    for (int area = 0; area <= 1; area++) for (int trim = 0; trim <= 1; trim++) {
        pass_fresh();
        g_flag_pass_area = area;
        g_flag_clear_only_attachments = trim;
        g_resolve_rc = 0;
        tex_t a = mk_tex(944, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
        tex_t b = mk_tex(945, 0x8000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
        memset(a.mem, 0x7e, a.L.size);
        memset(b.mem, 0x7e, b.L.size);
        stream s = {0};
        q_env(&s);
        const tex_t *colors[] = {&a, &b};
        put_set_rt_n(&s, colors, 2, NULL);
        p_scissor *crop = put(&s, OP_SCISSOR, sizeof *crop);
        *crop = (p_scissor){2, 2, 4, 4};
        q_draw(&s);
        p_scissor *full = put(&s, OP_SCISSOR, sizeof *full);
        *full = (p_scissor){0, 0, 16, 16};
        p_clear_color *clear = put(&s, OP_CLEAR_COLOR, sizeof *clear);
        clear->index = 0; clear->mask = 0xf; clear->rgba[2] = 1;
        end(&s);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_npd == 2 && g_pass_ends == 2);
        CHECK(g_pd[0].area_w == (area ? 4u : 0u) && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD);
        CHECK(g_pd[1].w == 16 && g_pd[1].h == 16 && g_pd[1].area_w == 0 && g_pd[1].area_h == 0);
        CHECK(g_pd[1].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[1].color[0].clear.f[2] == 1);
        CHECK((g_pd[1].color[1].view == NULL) == trim);
        printf("clear-only after draw: area=%d trim=%d draw_area=%u clear_area=%u attached_b=%d\n",
               area, trim, g_pd[0].area_w, g_pd[1].area_w, g_pd[1].color[1].view != NULL);
    }
    g_flag_pass_area = old_area;
    g_flag_clear_only_attachments = 0;
    pass_fresh();
}

// 中途交段：换渲染目标时，自上次交段起画够了 300 笔，就把已经录下的命令先交给后端，回放接着录进新的一段；
// 回写、同步对象的栅栏值都在最后一段上。用例把每段的笔数改成 12（手工拼的命令流放不下三百笔）：13 笔 → 交一次（主段 2 个）；11 笔 → 不交。
static int af_run(int draws, uint64_t *last_sig, uint64_t *f0) {
    pass_fresh(); g_gdraws = 0; *f0 = nvn_be_last_fence();   // 会话重开后栅栏值重新起算
    tex_t A = mk_tex(111, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), B = mk_tex(112, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    const tex_t *a_[1] = {&A}, *b_[1] = {&B};
    static stream s; memset(&s, 0, sizeof s); q_env(&s); put_set_rt_n(&s, a_, 1, NULL);
    for (int i = 0; i < draws; i++) q_draw(&s);
    put_set_rt_n(&s, b_, 1, NULL); q_draw(&s); end(&s);
    int c0 = g_main_commits; g_npend = 0;
    nvn_be_sub_begin(); nvn_be_exec(s.b); nvn_be_sub_commit();
    int n = g_main_commits - c0; *last_sig = g_npend ? g_pend[g_npend - 1].signal : 0;
    fake_complete(NVN_GPU_DONE_OK);
    return n;
}
static void t_autoflush(void) {
    uint64_t f0, b; int keep = rp_split_draws; rp_split_draws = 12;
    // 返回的是主段数（目标贴图第一次用要清零，各带一个前置段，不算在内）；中间的段依次取 +1、+2…，最后一段 +64
    CHECK(af_run(13, &b, &f0) == 2 && b == f0 + 64 && nvn_be_last_fence() == f0 + 64 && g_gdraws == 14 && g_npd == 2);   // 中途一段 + 最后一段；两个通道各在一段里
    CHECK(af_run(11, &b, &f0) == 1 && b == f0 + 64 && g_gdraws == 12);      // 不足一段的笔数：整次提交一段
    rp_split_draws = keep; pass_fresh();
}

// 超长通道中途先交（rp_exec.c pass_split）：同一个通道里自开（或上次切）以来画满 N 笔，下一笔之前结束通道、交出这一段，下一笔按 LOAD 续开。
// 期望值手算（N = 12，用例把 rp_split_pass_draws 改小）：40 笔 → 在第 13、25、37 笔之前各切一次，主段 4 个，各段提交时累计 12 / 24 / 36 / 40 笔；
// 第一个通道吃掉整屏清除（CLEAR），续开的三个都是 LOAD，四个通道存储都是 STORE、挂的是同一张图。12 笔正好满阈值但没有第 13 笔：不切。
// 缺省值 600 原样跑一遍 1300 笔：在第 601、1201 笔之前切，主段 3 个，累计 600 / 1200 / 1300。每次提交最多切 2 次时 60 笔：累计 12 / 24 / 60。
// 不切的形态（各 40 笔，主段都只有 1 个）：开关 0、多重采样目标、影子附件（深度图经颜色视图当目标）、会话用过遮挡计数、诊断开关把载入或存储换成 DONTCARE。
enum { SP_PLAIN, SP_MSAA, SP_SHADOW, SP_OCC, SP_DIAG_STORE, SP_DIAG_LOAD };
static int sp_run(int kind, int draws) {
    pass_fresh(); g_gdraws = 0; g_draw_outside = 0;
    if (kind == SP_OCC) { uint64_t rep_[2] = {0, 0}; stream w = {0}; q_env(&w); q_counter(&w, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL); q_draw(&w); q_counter(&w, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, rep_); end(&w); submit1(&w); fake_complete(NVN_GPU_DONE_OK); g_gdraws = 0; }
    g_flag_diag_store_dc = kind == SP_DIAG_STORE; g_flag_diag_load_dc = kind == SP_DIAG_LOAD;
    tex_t A = mk_tex(161, 0, kind == SP_SHADOW ? NVN_FORMAT_DEPTH16 : NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    if (kind == SP_MSAA) { A.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; A.d.samples = 4; nvn_tex_layout(&A.d, &A.L); A.has_layout = 1; }
    view_t v; memset(&v, 0, sizeof v); v.set_format = 1; v.format = NVN_FORMAT_R16;
    static stream h, b; memset(&h, 0, sizeof h); q_env(&h);
    if (kind == SP_SHADOW) put_set_rt_v(&h, &A, &v); else { const tex_t *a_[1] = {&A}; put_set_rt_n(&h, a_, 1, NULL); }
    if (kind == SP_OCC) q_counter(&h, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL);
    p_clear_color *cc = put(&h, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&h);
    const int chunk = 40; memset(&b, 0, sizeof b); for (int i = 0; i < chunk; i++) q_draw(&b); end(&b);   // 一条列表 40 笔（4096 字节放得下），整次提交里按需回放几遍
    CHECK(b.n <= sizeof b.b);
    static stream t; memset(&t, 0, sizeof t); for (int i = 0; i < draws % chunk; i++) q_draw(&t); end(&t);
    g_ncd = 0; g_npend = 0; const uint64_t f0 = nvn_be_last_fence();
    nvn_be_sub_begin(); nvn_be_exec(h.b);
    for (int i = 0; i < draws / chunk; i++) nvn_be_exec(b.b);
    nvn_be_exec(t.b); nvn_be_sub_commit();
    CHECK(g_npend && g_pend[g_npend - 1].signal == f0 + 64);   // 回写都在最后一段（+64），切出去的段依次取 +1、+2…（前置段也占）
    for (int i = 1; i < g_npend; i++) CHECK(g_pend[i].signal > g_pend[i - 1].signal);
    fake_complete(NVN_GPU_DONE_OK);
    g_flag_diag_store_dc = g_flag_diag_load_dc = 0;
    CHECK(g_gdraws == draws && g_draw_outside == 0);   // 切与不切画的笔数一样，每笔都在通道里
    return g_ncd;
}
static void t_split_long_pass(void) {
    const int keep = rp_split_pass_draws, keep_max = rp_split_pass_max;
    CHECK(keep == 600 && keep_max == 8);   // 缺省：每段 600 笔、每次提交最多切 8 次
    rp_split_pass_draws = 12;
    CHECK(sp_run(SP_PLAIN, 40) == 4 && g_cd[0] == 12 && g_cd[1] == 24 && g_cd[2] == 36 && g_cd[3] == 40);
    CHECK(g_npd == 4 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR);
    for (int k = 0; k < 4 && k < g_npd; k++) {
        CHECK(g_pd[k].color[0].store == NVN_GPU_STORE_STORE && g_pd[k].color[0].view && g_pd[0].color[0].view && g_pd[k].color[0].view->of == g_pd[0].color[0].view->of);
        if (k) CHECK(g_pd[k].color[0].load == NVN_GPU_LOAD_LOAD);
    }
    CHECK(sp_run(SP_PLAIN, 11) == 1 && g_cd[0] == 11 && g_npd == 1);
    CHECK(sp_run(SP_PLAIN, 12) == 1 && g_cd[0] == 12 && g_npd == 1);   // 满阈值但后面没有下一笔：不切
    CHECK(sp_run(SP_PLAIN, 13) == 2 && g_cd[0] == 12 && g_cd[1] == 13 && g_npd == 2 && g_pd[1].color[0].load == NVN_GPU_LOAD_LOAD);
    rp_split_pass_max = 2;   // 上限：60 笔本该切 4 次，只切前 2 次
    CHECK(sp_run(SP_PLAIN, 60) == 3 && g_cd[0] == 12 && g_cd[1] == 24 && g_cd[2] == 60);
    rp_split_pass_max = keep_max;
    g_flag_split_pass = 0; CHECK(sp_run(SP_PLAIN, 40) == 1 && g_npd == 1); g_flag_split_pass = -1;   // 开关 0 = 关
    CHECK(sp_run(SP_MSAA, 40) == 1 && g_npd == 1);
    CHECK(sp_run(SP_SHADOW, 40) == 1 && g_npd == 1);
    CHECK(sp_run(SP_OCC, 40) == 1);
    CHECK(sp_run(SP_DIAG_STORE, 40) == 1 && g_npd == 1);
    CHECK(sp_run(SP_DIAG_LOAD, 40) == 1 && g_npd == 1);
    rp_split_pass_draws = keep;   // 缺省值原样：1300 笔
    CHECK(sp_run(SP_PLAIN, 1300) == 3 && g_cd[0] == 600 && g_cd[1] == 1200 && g_cd[2] == 1300);
    CHECK(g_npd == 3 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[1].color[0].load == NVN_GPU_LOAD_LOAD && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
    CHECK(nvn_unimpl_kinds() == 0);
    pass_fresh();
}

// 3D 贴图的深度切片当渲染目标：NVN 视图的层号在 3D 贴图上是深度切片（nvn_mtl.m attach() 落到 depthPlane）；Vulkan 的 3D 视图起点只能是 0，
// 子范围只能用 2D（阵列）视图（nvn_vk_texview.h 文件头与 plan_make 的错误信息都这么说）。所以 3D 附件带层号时核心向后端请求 2D 阵列目标；
// 游戏自己设了 3D 目标的同样请求 2D 阵列（Vulkan 的 3D 视图本身不能当附件，不管起点是不是 0；真机上有游戏对 3D 贴图的第 k 片显式设「3D 目标 + 层号 k」，#724）；
// 游戏设的是 2D / 2D 阵列等附件合法目标的、没带层号的（首片）、不是 3D 的贴图，视图记录原样递下去。
// 期望值手算：32 层的 3D 取第 5 层 → set_target = 1、target = 2D_ARRAY、min_layer = 5、nlayers = 1，其余字段与游戏给的一致。
// 颜色槽、深度槽各一例；请求出来的视图只当附件用（采样 3D 贴图的视图不经这里，记录原样）。
static tex_t mk_tex_tgt(uint64_t serial, int target, int fmt, uint32_t w, uint32_t h, uint32_t depth) {
    tex_t t = mk_tex(serial, 0, fmt, w, h, 1, 0); t.d.target = target; t.d.depth = (int)depth; nvn_tex_layout(&t.d, &t.L); return t;
}
static void slice_pass(const tex_t *t, const view_t *v) {   // 一个颜色槽（可带视图）整屏清除 → 开一个通道；记下 view_create 收到的视图记录
    pass_fresh(); g_vw_last_has = -1; memset(&g_vw_last, 0xA5, sizeof g_vw_last);
    stream s = {0}; put_set_rt_v(&s, t, v);
    p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
}
static void put_set_rt_dv(stream *s, const tex_t *depth, const view_t *dv) {   // 只有深度，可带视图
    p_set_rt r; memset(&r, 0, sizeof r); r.depth = depth; if (dv) { r.has_dview = 1; r.dview = *dv; }
    uint8_t *a = put(s, OP_SET_RT, set_rt_size(&r)); p_set_rt_c *h = set_rt_put(a, &r);
    const tex_t **pt = (const tex_t **)(h + 1); tex_t *snap = (tex_t *)(a + set_rt_snap_off(h)); snap[0] = *depth; pt[0] = &snap[0];
}
static void slice_pass_depth(const tex_t *z, const view_t *v) {   // 只有深度目标（可带视图）整屏深度清除 → 开一个通道；记下 view_create 收到的视图记录
    pass_fresh(); g_vw_last_has = -1; memset(&g_vw_last, 0xA5, sizeof g_vw_last);
    stream s = {0}; put_set_rt_dv(&s, z, v);
    p_clear_ds *cd = put(&s, OP_CLEAR_DS, sizeof *cd); cd->depth = 0.5f; cd->depth_mask = 1; end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
}
static void t_pass_3d_slice_view(void) {
    tex_t v3 = mk_tex_tgt(72, NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, 32, 32, 32);
    view_t v; memset(&v, 0, sizeof v); v.set_layers = 1; v.min_layer = 5; v.nlayers = 1; v.set_levels = 1; v.base_level = 0; v.nlevels = 1;
    // ① 3D 的第 5 层
    slice_pass(&v3, &v);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY);
    CHECK(g_vw_last.set_layers == 1 && g_vw_last.min_layer == 5 && g_vw_last.nlayers == 1 && g_vw_last.set_levels == 1 && g_vw_last.base_level == 0 && g_vw_last.nlevels == 1);
    // ② 带层号的第 0 层：规则一致，同样请求 2D 阵列目标（不特判 0）
    v.min_layer = 0; slice_pass(&v3, &v);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.min_layer == 0);
    // ③ 没带视图 / 视图没带层号（首片）：原样（没带视图就是没带）
    slice_pass(&v3, NULL);
    CHECK(g_npd == 1 && g_vw_last_has == 0);
    view_t nl; memset(&nl, 0, sizeof nl); nl.set_levels = 1;
    slice_pass(&v3, &nl);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && !memcmp(&g_vw_last, &nl, sizeof nl));
    // ④ 游戏自己设了附件合法的目标（2D / 2D 阵列）：原样
    view_t own = v; own.min_layer = 7; own.set_target = 1; own.target = NVN_TEXTURE_TARGET_2D;
    slice_pass(&v3, &own);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && !memcmp(&g_vw_last, &own, sizeof own));
    view_t own_arr = own; own_arr.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    slice_pass(&v3, &own_arr);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && !memcmp(&g_vw_last, &own_arr, sizeof own_arr));
    // ④b 游戏自己设了 3D 目标 + 层号（#724 真机：16 层 3D 贴图的第 1…15 片，视图记录 set_target = 1、target = 3D、set_layers = 1、nlayers = 1、set_format = 1、没设级别）：
    //    3D 视图不能当附件 → 同样请求 2D 阵列；层号 / 层数 / 格式 / 通道交换等其余字段与游戏给的一致（只有 target 改）
    view_t real3; memset(&real3, 0, sizeof real3);
    real3.set_target = 1; real3.target = NVN_TEXTURE_TARGET_3D; real3.set_layers = 1; real3.min_layer = 7; real3.nlayers = 1;
    real3.set_format = 1; real3.format = NVN_FORMAT_RGBA8; real3.set_swizzle = 1; real3.swz[0] = 2; real3.swz[1] = 3; real3.swz[2] = 4; real3.swz[3] = 5;
    slice_pass(&v3, &real3);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY);
    CHECK(g_vw_last.set_layers == 1 && g_vw_last.min_layer == 7 && g_vw_last.nlayers == 1 && g_vw_last.set_levels == 0 && g_vw_last.base_level == 0 && g_vw_last.nlevels == 0);
    CHECK(g_vw_last.set_format == 1 && g_vw_last.format == NVN_FORMAT_RGBA8 && g_vw_last.set_swizzle == 1 && !memcmp(g_vw_last.swz, real3.swz, sizeof real3.swz));
    real3.min_layer = 0; slice_pass(&v3, &real3);   // 第 0 片：规则一致，不特判 0
    CHECK(g_npd == 1 && g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.min_layer == 0);
    view_t real3_nl = real3; real3_nl.set_layers = 0; real3_nl.min_layer = 0; real3_nl.nlayers = 0;   // 显式 3D 目标但没带层号（首片）：原样，本条不动
    slice_pass(&v3, &real3_nl);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && !memcmp(&g_vw_last, &real3_nl, sizeof real3_nl));
    tex_t a2_ = mk_tex_tgt(75, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 32, 32, 8);   // 不是 3D 的贴图上游戏设 3D 目标：不归这里管，原样
    real3.min_layer = 3; slice_pass(&a2_, &real3);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && !memcmp(&g_vw_last, &real3, sizeof real3));
    // ⑤ 2D 阵列贴图的第 3 层：视图记录与游戏给的逐字节相同（回归：只有 3D 才改）
    tex_t a2 = mk_tex_tgt(73, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 32, 32, 8);
    view_t va = v; va.min_layer = 3;
    slice_pass(&a2, &va);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && !memcmp(&g_vw_last, &va, sizeof va));
    // ⑥ 深度目标：深度槽的视图记录走同一个小函数。3D 贴图的第 3 片当深度目标 → 同样请求 2D 阵列目标，层号 / 级别原样
    tex_t z3 = mk_tex_tgt(74, NVN_TEXTURE_TARGET_3D, NVN_FORMAT_DEPTH16, 32, 32, 8);
    view_t vz = v; vz.min_layer = 3;
    slice_pass_depth(&z3, &vz);
    CHECK(g_npd == 1 && g_pd[0].depth.view && g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY);
    CHECK(g_vw_last.set_layers == 1 && g_vw_last.min_layer == 3 && g_vw_last.nlayers == 1 && g_vw_last.set_levels == 1 && g_vw_last.base_level == 0 && g_vw_last.nlevels == 1);
    view_t zown = vz; zown.set_target = 1; zown.target = NVN_TEXTURE_TARGET_2D;   // 游戏自己设了附件合法的目标：原样
    slice_pass_depth(&z3, &zown);
    CHECK(g_npd == 1 && g_pd[0].depth.view && g_vw_last_has == 1 && !memcmp(&g_vw_last, &zown, sizeof zown));
    view_t zown3 = vz; zown3.set_target = 1; zown3.target = NVN_TEXTURE_TARGET_3D;   // 游戏自己设了 3D 目标 + 层号：同样请求 2D 阵列，层号 / 级别原样
    slice_pass_depth(&z3, &zown3);
    CHECK(g_npd == 1 && g_pd[0].depth.view && g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY);
    CHECK(g_vw_last.set_layers == 1 && g_vw_last.min_layer == 3 && g_vw_last.nlayers == 1 && g_vw_last.set_levels == 1 && g_vw_last.base_level == 0 && g_vw_last.nlevels == 1);
    // ⑦ 请求出来的视图只当附件用：同一张 3D 贴图被采样走另一条取法（登记表 rp_view_by_id，缓存在登记项里，不进 rp_view_for 的环），
    //    视图记录是游戏登记的原样（不带我们为附件请求的 2D 阵列目标），建出来的也不是附件那次的对象；采样的 3D 视图仍是 3D 视图。
    view_t at; memset(&at, 0, sizeof at); at.set_layers = 1; at.min_layer = 5; at.nlayers = 1;
    slice_pass(&v3, &at);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.min_layer == 5);
    int att_rid = g_vid;   // 附件那次建的视图（通道里只建了这一个）
    view_t sv; memset(&sv, 0, sizeof sv); sv.set_levels = 1; sv.nlevels = 1;
    nvn_be_sub_begin(); nvn_be_reg_tex(3, &v3, &sv); nvn_be_set_tex_pool_n(8);
    nvn_gpu_view sm = rp_view_by_id(3, &g_fake_cmd, NULL);
    CHECK(sm && sm->rid != att_rid && g_vw_last_has == 1 && !memcmp(&g_vw_last, &sv, sizeof sv) && g_vw_last.set_target == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_unimpl_kinds() == 0);
    pass_fresh();
}

// 分层通道（八方旅人 010057D006492000 读档后整幅黑的根因）：调色 LUT #146 是 32x32x32 的 3D 贴图，游戏用一笔 6 顶点 × 32 实例的绘制、
// 顶点着色器按实例号写层号（BuiltIn Layer）一次画满 32 片；通道写死 1 层时只有第 0 片能落下，调色查表全 0，整幅画面黑。
// 规则照 iOS nvn_mtl.m（g_rp_layered → renderTargetArrayLength）：程序写层号（rp_prog.layer_out）时通道层数 = 各附件从挂接层起能给的层数取最小，
// 附件改成「起始层起这么多层、只取一级」的二维阵列视图；不写层号的照旧 1 层、视图记录原样；两种程序交替画同一目标时拆通道。
// 期望值手算：① 32 层 3D、不带视图 → 32 层，视图 2D 阵列 0+32、级 0+1；② 同一张、不写层号 → 1 层、不带视图；
// ③ 写层号 → 不写层号连画同一目标 → 两个通道，32 层、1 层（③b 两笔都写层号 → 一个通道）；④ 8 层 2D 阵列、视图起点 3 层数 5 → 5 层（8−3 = 5），层数 2 → 2；
// ⑤ 二维贴图 → 1 层、不带视图；⑥ 3D 视图取第 1 级 → 32>>1 = 16 层、级 1+1；⑦ 3D 32 层颜色 + 4 层二维阵列深度 → 取小 4。
static void lay_run(const tex_t *ct, const view_t *cv, const tex_t *zt, const struct rp_prog *const *progs, int ndraw) {
    pass_fresh(); g_resolve_rc = 0; g_vw_last_has = -1; memset(&g_vw_last, 0xA5, sizeof g_vw_last);
    g_resolve_seq_n = ndraw; g_resolve_seq_i = 0; for (int i = 0; i < ndraw && i < 4; i++) g_resolve_seq[i] = progs[i];
    stream s = {0}; q_env(&s);
    p_set_rt r; memset(&r, 0, sizeof r); r.n = 1; r.color[0] = ct; if (cv) { r.has_cview[0] = 1; r.cview[0] = *cv; } r.depth = zt;
    uint8_t *a = put(&s, OP_SET_RT, set_rt_size(&r)); p_set_rt_c *h = set_rt_put(a, &r);
    const tex_t **pt = (const tex_t **)(h + 1); tex_t *snap = (tex_t *)(a + set_rt_snap_off(h)); snap[0] = *ct; pt[0] = &snap[0]; if (zt) { snap[1] = *zt; pt[1] = &snap[1]; }
    for (int i = 0; i < ndraw; i++) { p_draw *d = put(&s, OP_DRAW, sizeof *d); d->prim = i & 1 ? 5 : 4; d->count = 6; d->instances = 32; }   // 换图元 = 换管线，逐笔重新解析程序
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    g_resolve_seq_n = 0;
}
static void t_pass_layered(void) {
    q_env(&(stream){0});   // 只为把 g_fprog 复位成固定布局（流丢弃）
    static struct rp_prog lay, flat; lay = g_fprog; lay.layer_out = 1; flat = g_fprog; flat.layer_out = 0;
    const struct rp_prog *L1[] = {&lay}, *F1[] = {&flat}, *LF[] = {&lay, &flat};
    tex_t lut = mk_tex_tgt(146, NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGB10A2, 32, 32, 32);
    // ①
    lay_run(&lut, NULL, NULL, L1, 1);
    CHECK(g_npd == 1 && g_pd[0].layers == 32 && g_pd[0].color[0].view);
    CHECK(g_vw_last_has == 1 && g_vw_last.set_target == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.set_layers == 1 && g_vw_last.min_layer == 0 && g_vw_last.nlayers == 32);
    CHECK(g_vw_last.set_levels == 1 && g_vw_last.base_level == 0 && g_vw_last.nlevels == 1);
    // ②
    lay_run(&lut, NULL, NULL, F1, 1);
    CHECK(g_npd == 1 && g_pd[0].layers == 1 && g_vw_last_has == 0);
    // ③
    lay_run(&lut, NULL, NULL, LF, 2);
    CHECK(g_npd == 2 && g_pd[0].layers == 32 && g_pd[1].layers == 1 && g_resolves >= 2);
    { const struct rp_prog *LL[] = {&lay, &lay}; lay_run(&lut, NULL, NULL, LL, 2); CHECK(g_npd == 1 && g_pd[0].layers == 32); }   // ③b 对照：两笔都写层号不拆（③ 的两个通道只来自分层与否不同）
    // ④
    tex_t arr = mk_tex_tgt(147, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 32, 32, 8);
    view_t va; memset(&va, 0, sizeof va); va.set_layers = 1; va.min_layer = 3; va.nlayers = 5;
    lay_run(&arr, &va, NULL, L1, 1);
    CHECK(g_npd == 1 && g_pd[0].layers == 5 && g_vw_last_has == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.min_layer == 3 && g_vw_last.nlayers == 5);
    va.nlayers = 2; lay_run(&arr, &va, NULL, L1, 1);
    CHECK(g_npd == 1 && g_pd[0].layers == 2 && g_vw_last_has == 1 && g_vw_last.min_layer == 3 && g_vw_last.nlayers == 2);
    // ⑤
    tex_t flat2d = mk_tex(148, 0, NVN_FORMAT_RGBA8, 32, 32, 1, 0);
    lay_run(&flat2d, NULL, NULL, L1, 1);
    CHECK(g_npd == 1 && g_pd[0].layers == 1 && g_vw_last_has == 0);
    // ⑥
    tex_t lut2 = lut; lut2.d.levels = 2; nvn_tex_layout(&lut2.d, &lut2.L);
    view_t v1; memset(&v1, 0, sizeof v1); v1.set_levels = 1; v1.base_level = 1; v1.nlevels = 1;
    lay_run(&lut2, &v1, NULL, L1, 1);
    CHECK(g_npd == 1 && g_pd[0].layers == 16 && g_vw_last_has == 1 && g_vw_last.base_level == 1 && g_vw_last.nlevels == 1 && g_vw_last.min_layer == 0 && g_vw_last.nlayers == 16);
    // ⑦
    tex_t za = mk_tex_tgt(149, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_DEPTH16, 32, 32, 4);
    lay_run(&lut, NULL, &za, L1, 1);
    CHECK(g_npd == 1 && g_pd[0].layers == 4 && g_pd[0].depth.view && g_vw_last_has == 1 && g_vw_last.target == NVN_TEXTURE_TARGET_2D_ARRAY && g_vw_last.nlayers == 4);
    CHECK(nvn_unimpl_kinds() == 0);
    pass_fresh();
}

// 游戏的顺序屏障落在「通道里已有着色器写」之后：屏障后的第一笔绘制另开通道（rp_pass.c rp_pass_barrier / rp_pass_prepare_draw）。
// 真机形态（Title ID 010074F013262000，Adreno 840）：一帧里顶点着色器写存储缓冲（蒙皮结果）的绘制与随后读它的绘制夹在同一个通道里，游戏在写段之后发屏障
// （ORDER_PRIMITIVES | INVALIDATE_SHADER = 0x21），回放层原先只记位、不拆通道；分块渲染的 GPU 先给整个通道的绘制做分块归属，读到写之前的旧数据，天空出现黑色块。
// 对齐：iOS nvn_mtl.m OP_BARRIER（通道里有着色器写 + ORDER_* 位 → 结束通道）；Ryujinx BarrierBatch.Flush（高通私有驱动不支持通道内屏障 → 结束通道）。
// 期望值手算（W = 程序对存储缓冲有写访问位的一笔，R = 只读的一笔，bXX = 游戏屏障、位 0xXX）：
//   W b21 R → 2 个通道，屏障位 0x21 在第二个通道开始前交后端（此前已开 1 个），通道被屏障结束 1 次；
//   负对照：W R → 1；R b21 R → 1（读不算写）；R b21 W → 1（屏障前通道里没有写）；b21 W R → 1（屏障在所有绘制之前）；
//   W b20 R / W b10 R / W bC0 R → 1（只有缓存失效位、没有顺序位：INVALIDATE_SHADER 0x20、INVALIDATE_TEXTURE 0x10、INVALIDATE_TEXTURE_DESCRIPTOR | INVALIDATE_ZCULL 0xC0）；
//   W bX R → 2，X = ORDER_PRIMITIVES 1 / ORDER_FRAGMENTS 2 / ORDER_FRAGMENTS_TILED 4 / ORDER_INDIRECT_DATA 8；
//   W b1 b20 R、W b20 b1 R → 2（两次屏障合起来只拆一次，位并起来 0x21，顺序位在前在后都算）；W b1 R b1 R → 2（拆开后新通道里没有写，第二次屏障不再拆）；W b1 W b1 R → 3（每个写后面的屏障各拆一次）。
//   同一张表跑两种「写」：img = 0 顶点阶段的存储缓冲（NVN_RB_SBUFP，槽没绑 → 64 KiB 空缓冲照画，见 t_storage_null_binding）；img = 1 片元阶段的存储图像（NVN_RB_IMG，见 t_graphics_image）。
struct sw_op { char k; int32_t bits; };   // k：'W' / 'R' 一笔绘制，'b' 一次游戏屏障
static void sw_run(int img, const struct sw_op *ops, int n, int *passes, uint64_t *cuts, uint32_t *bar_bits, int *bar_at, int *draws) {
    static struct rp_prog wp, rd; static tex_t it;
    q_env(&(stream){0});   // 只为把 g_fprog 复位成固定布局（流丢弃）
    wp = g_fprog;
    if (img) { wp.nent[1] = 1; wp.ent[1][0].cls = NVN_GPU_RES_IMAGE; wp.ent[1][0].src_kind = NVN_RB_IMG; wp.ent[1][0].src_reg = 2; wp.ent[1][0].count = 1; wp.ent[1][0].access = NVN_GPU_ACC_RW; }
    else     { wp.nent[0] = 1; wp.ent[0][0].cls = NVN_GPU_RES_SSBO; wp.ent[0][0].src_kind = NVN_RB_SBUFP; wp.ent[0][0].src_reg = 0; wp.ent[0][0].access = NVN_GPU_ACC_RW; }
    rd = wp; rd.ent[img][0].access = NVN_GPU_ACC_R;   // 同一布局、只读：两种程序除了写访问位一模一样
    pass_fresh(); g_resolve_rc = 0; g_gdraws = 0;
    if (img) { it = mk_tex(669, 0, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_IMAGE); nvn_be_reg_tex(3, &it, NULL); nvn_be_set_tex_pool_n(8); memset(rp_S.img[1], 0, sizeof rp_S.img[1]); rp_S.img[1][2] = 3; }   // pass_fresh 会清登记：在它之后登
    int nd = 0; for (int i = 0; i < n; i++) if (ops[i].k != 'b') { CHECK(nd < 4); g_resolve_seq[nd++] = ops[i].k == 'W' ? &wp : &rd; }
    g_resolve_seq_n = nd; g_resolve_seq_i = 0;
    uint64_t pf[RP_PERF_N]; rp_perf_take(pf);
    stream s = {0}; q_env(&s);
    nd = 0;
    for (int i = 0; i < n; i++) {
        if (ops[i].k == 'b') { uint8_t *b = put(&s, OP_BARRIER, 8); memcpy(b, &ops[i].bits, 4); continue; }
        p_draw *d = put(&s, OP_DRAW, sizeof *d); d->prim = nd++ & 1 ? 5 : 4; d->count = 3; d->instances = 1;   // 换图元 = 换管线，逐笔重新解析程序（同 lay_run）
    }
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    rp_perf_take(pf);
    g_resolve_seq_n = 0;
    *passes = g_npd; *cuts = pf[RP_PERF_P_CUT_BAR]; *bar_bits = g_barrier_bits; *bar_at = g_barrier_at; *draws = g_gdraws;
    CHECK(nvn_unimpl_kinds() == 0);
}
static void t_shader_write_barrier_split(void) {
    enum { BB_P = NVN_BARRIER_ORDER_PRIMITIVES_BIT, BB_F = NVN_BARRIER_ORDER_FRAGMENTS_BIT, BB_FT = NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT, BB_IND = NVN_BARRIER_ORDER_INDIRECT_DATA_BIT,
           BB_ITEX = NVN_BARRIER_INVALIDATE_TEXTURE_BIT, BB_ISH = NVN_BARRIER_INVALIDATE_SHADER_BIT, BB_ITD = NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT, BB_IZ = NVN_BARRIER_INVALIDATE_ZCULL_BIT };
    struct { const char *name; struct sw_op ops[6]; int n, passes; uint64_t cuts; } t[] = {
        { "W b21 R",       { {'W', 0}, {'b', BB_P | BB_ISH}, {'R', 0} },                           3, 2, 1 },
        { "W R",           { {'W', 0}, {'R', 0} },                                                 2, 1, 0 },
        { "R b21 R",       { {'R', 0}, {'b', BB_P | BB_ISH}, {'R', 0} },                           3, 1, 0 },
        { "R b21 W",       { {'R', 0}, {'b', BB_P | BB_ISH}, {'W', 0} },                           3, 1, 0 },
        { "b21 W R",       { {'b', BB_P | BB_ISH}, {'W', 0}, {'R', 0} },                           3, 1, 0 },
        { "W b20 R",       { {'W', 0}, {'b', BB_ISH}, {'R', 0} },                                  3, 1, 0 },
        { "W b10 R",       { {'W', 0}, {'b', BB_ITEX}, {'R', 0} },                                 3, 1, 0 },
        { "W bC0 R",       { {'W', 0}, {'b', BB_ITD | BB_IZ}, {'R', 0} },                          3, 1, 0 },
        { "W b01 R",       { {'W', 0}, {'b', BB_P}, {'R', 0} },                                    3, 2, 1 },
        { "W b02 R",       { {'W', 0}, {'b', BB_F}, {'R', 0} },                                    3, 2, 1 },
        { "W b04 R",       { {'W', 0}, {'b', BB_FT}, {'R', 0} },                                   3, 2, 1 },
        { "W b08 R",       { {'W', 0}, {'b', BB_IND}, {'R', 0} },                                  3, 2, 1 },
        { "W b01 b20 R",   { {'W', 0}, {'b', BB_P}, {'b', BB_ISH}, {'R', 0} },                     4, 2, 1 },
        { "W b20 b01 R",   { {'W', 0}, {'b', BB_ISH}, {'b', BB_P}, {'R', 0} },                     4, 2, 1 },
        { "W b01 R b01 R", { {'W', 0}, {'b', BB_P}, {'R', 0}, {'b', BB_P}, {'R', 0} },             5, 2, 1 },
        { "W b01 W b01 R", { {'W', 0}, {'b', BB_P}, {'W', 0}, {'b', BB_P}, {'R', 0} },             5, 3, 2 },
    };
    for (int img = 0; img < 2; img++) {
        for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
            int passes, bar_at, draws; uint64_t cuts; uint32_t bar_bits; int nd = 0;
            for (int k = 0; k < t[i].n; k++) if (t[i].ops[k].k != 'b') nd++;
            sw_run(img, t[i].ops, t[i].n, &passes, &cuts, &bar_bits, &bar_at, &draws);
            if (passes != t[i].passes || cuts != t[i].cuts || draws != nd) printf("t_shader_write_barrier_split img=%d %s：通道 %d（期望 %d）被屏障结束 %llu（期望 %llu）画 %d（期望 %d）\n", img, t[i].name, passes, t[i].passes, (unsigned long long)cuts, (unsigned long long)t[i].cuts, draws, nd);
            CHECK(passes == t[i].passes && cuts == t[i].cuts && draws == nd);
        }
        // 拆开的那一次：屏障位在第二个通道开始前交后端（此前只开过 1 个），合并后的位 0x21
        { int passes, bar_at, draws; uint64_t cuts; uint32_t bar_bits;
          struct sw_op ops[] = { {'W', 0}, {'b', BB_P}, {'b', BB_ISH}, {'R', 0} };
          sw_run(img, ops, 4, &passes, &cuts, &bar_bits, &bar_at, &draws);
          CHECK(passes == 2 && bar_bits == (BB_P | BB_ISH) && bar_at == 1); }
    }
    memset(rp_S.img[1], 0, sizeof rp_S.img[1]);
    pass_fresh();
}

// 程序自带常量段 c1 整段绑定（八方旅人调色 LUT 的片元读 c1 到第 2304 字节；原先只绑前 0x100，后段读 0 → LUT 全 0 → 场景整幅黑）。
// 期望值手算：① 2304 字节的 c1 → 绑定长 2304，暂存区内容与 c1 逐字节相同；② 2300 字节 → 按 16 字节补齐成 2304，前 2300 字节相同、末 4 字节 0；
// ③ 设备 maxUniformBufferRange = 2048 < 2304 → 这一笔具名跳过 c1_segment_over_ubo_range 一次、不画。
static void t_draw_c1_full(void) {
    static _Alignas(16) uint8_t c1[2304]; for (int i = 0; i < 2304; i++) c1[i] = (uint8_t)(i * 7 + 3);
    for (int cs = 0; cs < 3; cs++) {
        pass_fresh(); g_gdraws = g_resolve_rc = 0; g_caps_max_ubo = cs == 2 ? 2048 : 0;
        stream s = {0}; q_env(&s);
        g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_C1;
        static _Alignas(16) uint8_t c1b[2304]; memcpy(c1b, c1, sizeof c1b); if (cs == 1) memset(c1b + 2300, 0, 4);   // 程序对象的存储按 16 字节补齐、补齐部分为 0（nvk_shader.c prog_c1_take）
        g_fprog.c1[1] = c1b; g_fprog.c1_len[1] = cs == 1 ? 2300 : 2304; g_fprog.c1_ok = 2;
        q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        if (cs < 2) {
            CHECK(g_gdraws == 1 && g_nfb[1] == 1 && g_fb[1][0].u.b.len == 2304);
            CHECK(g_fb[1][0].u.b.off + 2304 <= sizeof g_arena && !memcmp(&g_arena[g_fb[1][0].u.b.off], c1, cs == 1 ? 2300 : 2304));
            if (cs == 1) { static const uint8_t z4[4]; CHECK(!memcmp(&g_arena[g_fb[1][0].u.b.off + 2300], z4, 4)); }
            CHECK(nvn_unimpl_kinds() == 0);
        } else CHECK(g_gdraws == 0 && nvn_unimpl_count("draw", "c1_segment_over_ubo_range") == 1);
    }
    g_caps_max_ubo = 0; g_fprog.c1[1] = NULL; g_fprog.c1_len[1] = 0; g_fprog.c1_ok = 0; pass_fresh();
}

// 遮挡计数：通过的样本数是队列上的累计值——复位清零、之后的绘制往上加、报告写出当时的累计值。
// 期望值手算：各槽的样本数由替身在完成时给出，报告值 = 复位之后、报告之前开过的各槽之和（跨段接着加）。
// 开段失败（审计 P1-174）：时间线照走一格，不回放出任何绘制；中途交段之后开新段失败时，已经交出去的段完成回调不能碰到放掉的提交。
static void t_begin_fail(void) {
    // ① 一上来就开不了段：这次提交一笔都不画，返回的栅栏值 = 原值 + 64；下一次正常提交用的是再 + 64，两次的值不重。
    //    这一格要有人点亮（P1-174）：补交一个空段、值就是这一格，做完后等这一格的线程不靠下一次提交就放行
    pass_fresh(); g_gdraws = 0; uint64_t f0 = nvn_be_last_fence(); int c0 = g_main_commits; g_npend = 0;
    static stream s; memset(&s, 0, sizeof s); q_env(&s); q_draw(&s); end(&s);
    g_begin_fail_at = 1; nvn_be_sub_begin(); nvn_be_exec(s.b); uint64_t f1 = nvn_be_sub_commit();
    CHECK(f1 == f0 + 64 && nvn_be_last_fence() == f0 + 64 && g_gdraws == 0 && g_begin_fail_at == 0);
    CHECK(g_main_commits == c0 + 1 && g_npend == 1 && g_pend[0].signal == f0 + 64);   // 补点亮的空段：恰好一个、值就是这一格
    CHECK(nvn_gpu_fence_wait(f1, 0, NULL).code == NVN_GPU_E_TIMEOUT);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_gpu_fence_wait(f1, 0, NULL).code == NVN_GPU_OK);   // 没有下一次提交，这一格也已点亮
    nvn_be_sub_begin(); nvn_be_exec(s.b); uint64_t f2 = nvn_be_sub_commit();
    CHECK(f2 == f0 + 128 && g_main_commits == c0 + 2 && g_npend && g_pend[g_npend - 1].signal == f0 + 128);
    fake_complete(NVN_GPU_DONE_OK);
    // ② 带遮挡计数、画 13 笔（每段 12 笔）：中途交出一段之后开新段失败。主段只交了那一个中途段（值 = 原值 + 1 或 + 2：前面可能有一个前置段），
    //    提交返回原值 + 64；随后那个中途段完成，回调要往所属提交里攒结果——这时那份提交必须还活着（用地址消毒器跑这条用例看得出来）。
    int keep = rp_split_draws; rp_split_draws = 12; pass_fresh();
    { uint64_t rep_[2] = {0, 0}; stream w = {0}; q_env(&w); q_counter(&w, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL); q_draw(&w); q_counter(&w, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, rep_); end(&w); submit1(&w); fake_complete(NVN_GPU_DONE_OK); }   // 先让计数器被用过一次：之后的段才声明遮挡槽
    g_gdraws = 0; f0 = nvn_be_last_fence(); c0 = g_main_commits; g_npend = 0;
    tex_t A = mk_tex(121, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), B = mk_tex(122, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0); const tex_t *a_[1] = {&A}, *b_[1] = {&B};
    static stream t; memset(&t, 0, sizeof t); q_env(&t); q_counter(&t, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL); put_set_rt_n(&t, a_, 1, NULL);
    for (int i = 0; i < 13; i++) q_draw(&t);
    put_set_rt_n(&t, b_, 1, NULL); q_draw(&t); end(&t);
    nvn_be_sub_begin(); CHECK(g_occ_slots_decl != 0); g_begin_fail_at = 1; nvn_be_exec(t.b); uint64_t f3 = nvn_be_sub_commit();
    CHECK(g_begin_fail_at == 0 && g_main_commits == c0 + 2 && f3 == f0 + 64 && g_gdraws == 13);   // 第 14 笔在开不出来的新段里：不画；主段 = 中途段 + 补点亮这一格的空段
    CHECK(g_npend >= 2 && g_pend[g_npend - 1].signal == f0 + 64 && g_pend[g_npend - 2].signal < f0 + 64);   // 空段在中途段之后、值是这一格
    static uint64_t occ[4] = {7, 0, 0, 0}; g_fake_occ = occ; g_fake_nocc = 1;
    fake_complete(NVN_GPU_DONE_OK); g_fake_occ = NULL; g_fake_nocc = 0;
    nvn_be_sub_begin(); nvn_be_exec(s.b); CHECK(nvn_be_sub_commit() == f0 + 128); fake_complete(NVN_GPU_DONE_OK);   // 之后照常
    rp_split_draws = keep; pass_fresh();
}

// #1015：多重采样目标上游戏关了多重采样（ms.enable = 0）：安卓照多重采样画，但要具名记 draw / multisample_off_on_ms_target，不静默。
// 负对照：单样本目标上关、多重采样目标上开，都不记。
static void t_msoff(void) {
    pass_fresh(); g_gdraws = 0;
    tex_t M = mk_tex(141, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); M.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; M.d.samples = 4; nvn_tex_layout(&M.d, &M.L); M.has_layout = 1;
    tex_t S1 = mk_tex(142, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0); const tex_t *m_[1] = {&M}, *s_[1] = {&S1};
    ms_st on = {0}, off = {0}; on.enable = 1; on.samples = 4; off.enable = 0; off.samples = 4;
    static const struct { int ms_target; int enable; unsigned want; } k[3] = {{1, 0, 1}, {0, 0, 0}, {1, 1, 0}};
    for (int i = 0; i < 3; i++) {
        stream q = {0}; q_env(&q); put_set_rt_n(&q, k[i].ms_target ? m_ : s_, 1, NULL);
        p_bind_state *by = put(&q, OP_BIND_STATE, sizeof *by); by->kind = 3; memcpy(by->bytes, k[i].enable ? &on : &off, sizeof on);
        q_draw(&q); end(&q);
        int g0 = g_gdraws; unsigned n0 = nvn_unimpl_count("draw", "multisample_off_on_ms_target");
        submit1(&q); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_gdraws == g0 + 1 && nvn_unimpl_count("draw", "multisample_off_on_ms_target") == n0 + k[i].want);
    }
    pass_fresh();
}

static void t_occlusion(void) {
    pass_fresh(); g_rcond = 0; g_gdraws = 0; g_nocc_log = 0; g_tick = 5;
    uint64_t ra[2] = {~0ull, ~0ull}, rb[2] = {~0ull, ~0ull}, rcx[2] = {~0ull, ~0ull};
    // 段 1：游戏第一次碰这个计数器。开段时还不知道、没声明槽：复位时就地交段，后半段带槽（#1015）。
    // 复位前那笔在前半段、不计；复位后那笔在后半段的槽 0，报告 R0 = 槽 0 的值。
    uint64_t r0[2] = {~0ull, ~0ull}; int mc0 = g_main_commits;
    stream s1 = {0}; q_env(&s1); q_draw(&s1); q_counter(&s1, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL); q_draw(&s1);
    q_counter(&s1, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, r0); end(&s1);
    submit1(&s1);
    CHECK(g_occ_slots_decl > 0 && g_main_commits == mc0 + 2 && g_gdraws == 2 && g_npd == 2 && g_pd[0].occlusion == 0 && g_pd[1].occlusion == 1);
    { static const int want[2] = {0, -1}; CHECK(g_nocc_log == 2 && !memcmp(g_occ_log, want, sizeof want)); }
    CHECK(nvn_unimpl_count("occl", "first_reset_not_counted") == 0);
    { static const uint64_t occ[1] = {9}; g_fake_occ = occ; g_fake_nocc = 1; fake_complete(NVN_GPU_DONE_OK); }
    CHECK(r0[0] == 9);
    g_nocc_log = 0;
    // 段 2：画（槽 0）、报告 A、画（槽 1）、带裁剪的清除（清除不计：先停）、画（槽 2）、复位、画（槽 3）、报告 B
    int npd0 = g_npd;
    stream s2 = {0}; q_draw(&s2); q_counter(&s2, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, ra); q_draw(&s2);
    p_scissor *sc = put(&s2, OP_SCISSOR, sizeof *sc); sc->x = 4; sc->y = 4; sc->w = 8; sc->h = 8;
    p_clear_color *cc = put(&s2, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf;
    q_draw(&s2); q_counter(&s2, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL); q_draw(&s2); q_counter(&s2, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, rb); end(&s2);
    submit1(&s2);
    CHECK(g_occ_slots_decl > 0 && g_npd == npd0 + 1 && g_pd[npd0 < 8 ? npd0 : 7].occlusion == 1 && g_ncr == 1);
    { static const int want[8] = {0, -1, 1, -1, 2, -1, 3, -1}; CHECK(g_nocc_log == 8 && !memcmp(g_occ_log, want, sizeof want)); }
    CHECK(ra[0] == ~0ull);   // 没完成：没写
    { static const uint64_t occ[4] = {10, 20, 30, 7}; g_fake_occ = occ; g_fake_nocc = 4; g_tick = 600; fake_complete(NVN_GPU_DONE_OK); }
    CHECK(ra[0] == 19 && ra[1] == 600);   // 复位在段 1（段 1 复位后那笔 9）；本段报告 A 之前只开过槽 0（10）
    CHECK(rb[0] == 7 && rb[1] == 600);    // 段内又复位过：只算槽 3
    // 段 3：接着画（这一段的槽 0）再报告：跨段累计 7 + 5
    g_nocc_log = 0;
    stream s3 = {0}; q_draw(&s3); q_counter(&s3, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, rcx); end(&s3);
    submit1(&s3);
    { static const uint64_t occ[1] = {5}; g_fake_occ = occ; g_fake_nocc = 1; fake_complete(NVN_GPU_DONE_OK); }
    CHECK(rcx[0] == 12 && g_nocc_log == 2 && g_occ_log[0] == 0 && g_occ_log[1] == -1);
    // 失败的段：报告不写，槽值不进累计
    rcx[0] = ~0ull; submit1(&s3); { static const uint64_t occ[1] = {100}; g_fake_occ = occ; g_fake_nocc = 1; fake_complete(NVN_GPU_DONE_FAILED); }
    CHECK(rcx[0] == ~0ull);
    submit1(&s3); { static const uint64_t occ[1] = {1}; g_fake_occ = occ; g_fake_nocc = 1; fake_complete(NVN_GPU_DONE_OK); }
    CHECK(rcx[0] == 13 && nvn_unimpl_kinds() == 0);
    pass_fresh();
}

// #1015：第一次复位要就地交段，但这次提交的中间值已经用完（每段 1 笔、换 61 次渲染目标先交满）：交不了，具名记 first_reset_not_counted，不静默。
static void t_occl_first_reset_cap(void) {
    pass_fresh(); g_gdraws = 0; g_npend = 0; int keep = rp_split_draws; rp_split_draws = 1;
    tex_t A = mk_tex(131, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), B = mk_tex(132, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0); const tex_t *a_[1] = {&A}, *b_[1] = {&B};
    static stream t; memset(&t, 0, sizeof t); q_env(&t); end(&t);
    nvn_be_sub_begin(); nvn_be_exec(t.b);   // 同一次提交里分块回放（一个流装不下 61 次换目标）
    for (int i = 0; i < 61; i++) { memset(&t, 0, sizeof t); put_set_rt_n(&t, (i & 1) ? b_ : a_, 1, NULL); q_draw(&t); end(&t); nvn_be_exec(t.b); }
    memset(&t, 0, sizeof t); q_counter(&t, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL); q_draw(&t); end(&t); nvn_be_exec(t.b);
    nvn_be_sub_commit();
    CHECK(g_gdraws == 62 && g_occ_slots_decl == 0 && nvn_unimpl_count("occl", "first_reset_not_counted") == 1);
    fake_complete(NVN_GPU_DONE_OK);
    rp_split_draws = keep; pass_fresh();
}

// 计算派发：取计算程序 → 结束通道 → 绑 NVN 阶段 5 的资源（驱动段里有组数与两张表）→ 派发；
// 可写的存储缓冲是 GPU 往池里写：完成后结果回到游戏内存，完成前镜像同步绕开那一段。期望值手算。
static void t_dispatch(void) {
    pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_arena_used = 0; g_nbw = 0;
    g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0;
    for (unsigned i = 0; i < sizeof g_pmem; i++) g_pmem[i] = 0x11;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 3;
    g_cprog.ent[0][0].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][0].src_kind = NVN_RB_SBUFP; g_cprog.ent[0][0].src_reg = 0; g_cprog.ent[0][0].access = NVN_GPU_ACC_RW;
    g_cprog.ent[0][1].cls = NVN_GPU_RES_UBO; g_cprog.ent[0][1].src_kind = NVN_RB_DRV0;
    g_cprog.ent[0][2].cls = NVN_GPU_RES_UBO; g_cprog.ent[0][2].src_kind = NVN_RB_CBUF; g_cprog.ent[0][2].src_reg = 1;
    stream s = {0}; q_env(&s); q_draw(&s);   // 先画一笔：派发要把开着的通道结束掉
    p_bind_buf *sb = put(&s, OP_BIND_SSBO, sizeof *sb); sb->stage = 5; sb->index = 0; sb->addr = base + PG + 64; sb->size = 128;
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 5; ub->index = 1; ub->addr = base + 3 * PG; ub->size = 32;
    p_dispatch *dp = put(&s, OP_DISPATCH, sizeof *dp); dp->x = 2; dp->y = 3; dp->z = 1;
    dp = put(&s, OP_DISPATCH, sizeof *dp); dp->x = 0; dp->y = 3; dp->z = 1;                        // 0 组：空操作
    dp = put(&s, OP_DISPATCH, sizeof *dp); dp->x = 1; dp->y = 1; dp->z = 1; dp->ind = base + sizeof g_pmem - 8;   // 间接参数越过池尾，记账
    q_draw(&s);                                                                                    // 派发之后再画：管线全部重发
    end(&s);
    int ends0 = g_pass_ends, sp0 = g_set_pipes, pg0 = g_pipe_gets;
    submit1(&s);
    CHECK(g_cresolves == 2 && g_cpipe_gets == 1 && g_cpipe_sh == &g_fsh[0] && g_ndisp == 1 && g_disp[0] == 2 && g_disp[1] == 3 && g_disp[2] == 1 && g_disp_pipe == &g_fcpipe);
    CHECK(g_pass_ends - ends0 >= 1 && g_npd == 2);                         // 派发前通道结束；之后那一笔另开一个通道
    CHECK(g_set_pipes - sp0 == 3 && g_pipe_gets - pg0 == 2);               // 图形、计算、再图形：派发之后图形管线重取重设
    CHECK(nvn_unimpl_count("draw", "dispatch_indirect_unresolved") == 1);
    CHECK(g_cbinds == 1 && g_ncb == 3 && g_cb[0].cls == NVN_GPU_RES_SSBO && g_cb[0].u.b.off == PG + 64 && g_cb[0].u.b.len == 128 && g_cb[0].u.b.buf != &g_arena_buf);   // 存储缓冲绑的是池镜像里的那一段
    { const uint8_t *c0 = g_arena + g_cb[1].u.b.off; uint32_t dim[3]; uint64_t a; uint32_t n; memcpy(dim, c0 + 0x200, 12);
      CHECK(g_cb[1].u.b.buf == &g_arena_buf && dim[0] == 2 && dim[1] == 3 && dim[2] == 1);
      memcpy(&a, c0 + 0x210 + 16, 8); memcpy(&n, c0 + 0x210 + 16 + 8, 4); CHECK(a == base + 3 * PG && n == 32);   // 常量缓冲表第 1 项
      memcpy(&a, c0 + 0x310, 8); memcpy(&n, c0 + 0x310 + 8, 4); CHECK(a == base + PG + 64 && n == 128); }           // 存储缓冲表第 0 项
    CHECK(g_cb[2].u.b.len == 32 && g_arena[g_cb[2].u.b.off] == 0x11);     // 常量缓冲：这一刻的内容拷进暂存区
    // GPU 写了镜像里的那一段（替身：整块镜像内存 = 0xEE）。完成之前：游戏内存不动；同页别处脏了重传时绕开那一段
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0xEE, sizeof mirror); g_buf_cpu_fake = mirror;
    CHECK(g_pmem[PG + 64] == 0x11);
    g_pg_gen[1] = 101; g_dirty_now = 102; g_nbw = 0;                       // 游戏写了第 1 页的别处
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG, PG, NULL, NULL) == 1);
    nvn_be_sub_commit();
    // 上一次提交（那次派发）显卡还没做完：镜像里的旧内容它可能还没读，这次同步不能直接写镜像，按序录成 GPU 上的拷贝；
    // [PG+64, PG+192) 是 GPU 写过还没落地的，绕开 → 前后两段（64 字节、PG−192 字节），镜像内存一个字节都没被 CPU 动
    CHECK(g_nbw == 2 && mirror[PG] == 0xEE && mirror[PG + 64] == 0xEE && mirror[PG + 192] == 0xEE);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_pmem[PG + 63] == 0x11 && g_pmem[PG + 64] == 0xEE && g_pmem[PG + 191] == 0xEE && g_pmem[PG + 192] == 0x11);   // 完成：只有那 128 字节回到游戏内存
    g_pg_gen[1] = 103; g_dirty_now = 104; g_nbw = 0;                       // 回写落地之后再脏：整页照传，不再绕
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG, PG, NULL, NULL) == 1 && g_nbw == 0 && !memcmp(mirror + PG, g_pmem + PG, PG));
    g_pmem[PG + 8] ^= 0x40; g_pg_gen[1] = 105; g_dirty_now = 106;          // 同一次提交里同步过又变了：这一次要排在已录的绘制之后，录成 GPU 上的拷贝
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG, PG, NULL, NULL) == 1 && g_nbw == 1 && g_bw[0].off == PG && g_bw[0].len == PG);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 失败的段：不回写，登记照撤
    memset(g_pmem + PG, 0x22, PG); memset(mirror, 0x77, sizeof mirror);
    stream s2 = {0}; dp = put(&s2, OP_DISPATCH, sizeof *dp); dp->x = 1; dp->y = 1; dp->z = 1; end(&s2);
    submit1(&s2); fake_complete(NVN_GPU_DONE_FAILED);
    CHECK(g_ndisp == 2 && g_cpipe_gets == 1 && g_pmem[PG + 64] == 0x22);   // 同一个程序不重建管线；失败不回写
    // 终态不推进已完成值。直接查撤登记，不在失败时间线上模拟下一次成功的镜像同步。
    CHECK(rp_buf_gpu_write_pending(base + PG, PG) == 0);
    // 取不到程序：记账跳过，不派发
    g_cresolve_rc = -1; submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == 2 && nvn_unimpl_count("draw", "compute_program_unresolved") == 1);
    g_cresolve_rc = 0; g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem); pass_fresh();
}

// 游戏屏障对通道外的计算派发也生效（#978；iOS 在下一个编码器开头消费游戏屏障，计算编码器也算）：
// 派发 A → 屏障 → 派发 B：屏障在 A 之后、B 之前交给后端，只交一次（之后开的绘制通道不再重发）；
// 间接派发自带的那道屏障和待发位合成一次；负控：没有游戏屏障就一道都不插。
static void t_dispatch_barrier(void) {
    const int32_t bits = NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT;
    for (int mode = 0; mode < 3; mode++) {   // 0 有屏障；1 有屏障、B 是间接派发；2 负控：没有屏障
        pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_arena_used = 0; g_nbw = 0;
        g_cresolve_rc = 0; g_ndisp = 0;
        uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem); memset(g_pmem, 0, sizeof g_pmem);
        uint32_t ind[3] = {1, 1, 1}; memcpy(g_pmem + 2 * PG, ind, 12);
        memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1;
        g_cprog.ent[0][0].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][0].src_kind = NVN_RB_SBUFP; g_cprog.ent[0][0].src_reg = 0; g_cprog.ent[0][0].access = NVN_GPU_ACC_RW;
        stream s = {0};
        p_bind_buf *sb = put(&s, OP_BIND_SSBO, sizeof *sb); sb->stage = 5; sb->index = 0; sb->addr = base + PG; sb->size = 256;
        p_dispatch *dp = put(&s, OP_DISPATCH, sizeof *dp); dp->x = 4; dp->y = 1; dp->z = 1;   // A：写
        if (mode != 2) { uint8_t *b = put(&s, OP_BARRIER, 8); memcpy(b, &bits, 4); }
        dp = put(&s, OP_DISPATCH, sizeof *dp);                                                 // B：读 A 写的内容
        if (mode == 1) dp->ind = base + 2 * PG; else { dp->x = 4; dp->y = 1; dp->z = 1; }
        q_env(&s); q_draw(&s);   // 派发之后开一个绘制通道：已经交过的屏障不在它开头重发
        end(&s);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_ndisp == 2 && g_npd == 1 && nvn_unimpl_kinds() == 0);
        if (mode == 0) CHECK(g_nbar == 1 && g_bar[0].ndisp == 1 && g_bar[0].bits == (uint32_t)bits);
        if (mode == 1) CHECK(g_nbar == 1 && g_bar[0].ndisp == 1 && g_bar[0].bits == ((uint32_t)bits | NVN_BARRIER_ORDER_INDIRECT_DATA_BIT));
        if (mode == 2) CHECK(g_nbar == 0);
        nvn_be_pool_del(g_pmem);
    }
    pass_fresh();
}

// 着色器来源快路径用的两个代号（分离贴图槽摘要的缓存靠它们判失效）：
// 分离槽（128 起）被改写才动 rp_tex_sep_gen[阶段]，组合槽不动；贴图登记 / 注销各动一次 rp_tex_reg_gen。
uint64_t rp_tex_reg_gen(void); extern uint64_t rp_tex_sep_gen[6];
static void t_tex_fact_gens(void) {
    pass_fresh();
    uint64_t s1 = rp_tex_sep_gen[1], s0 = rp_tex_sep_gen[0];
    { stream a = {0}; p_bind_handles *b = put(&a, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 3; b->n = 1; b->h[0] = 0x1234; end(&a); submit1(&a); fake_complete(NVN_GPU_DONE_OK); }
    CHECK(rp_tex_sep_gen[1] == s1 && rp_tex_sep_gen[0] == s0);                 // 组合槽：不动
    { stream a = {0}; p_bind_handles *b = put(&a, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 2; b->h[0] = 0x55; b->h[1] = 0x66; end(&a); submit1(&a); fake_complete(NVN_GPU_DONE_OK); }
    CHECK(rp_tex_sep_gen[1] == s1 + 1 && rp_tex_sep_gen[0] == s0);             // 分离槽：只动这个阶段
    { stream a = {0}; p_bind_handles *b = put(&a, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 2; b->h[0] = 0x55; b->h[1] = 0x66; end(&a); submit1(&a); fake_complete(NVN_GPU_DONE_OK); }
    CHECK(rp_tex_sep_gen[1] == s1 + 1);                                        // 写进去的和原来一样：不动
    uint64_t r0 = rp_tex_reg_gen();
    tex_t t = mk_tex(777, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    nvn_be_reg_tex(9, &t, NULL); CHECK(rp_tex_reg_gen() == r0 + 1);
    nvn_be_unreg_tex_range(9, 1); CHECK(rp_tex_reg_gen() == r0 + 2);
    samp_par sp = {0};
    nvn_be_reg_samp(4, &sp); CHECK(rp_tex_reg_gen() == r0 + 3); // 登记采样器使绑定缓存失效
    nvn_be_reg_samp(4, &sp); CHECK(rp_tex_reg_gen() == r0 + 4); // 原位重登记同样失效
    nvn_be_tex_release(&t); pass_fresh();
}

// 帧重叠（开关 nvk_finish_lag）：游戏的 Finish 不等刚交的那一帧，下一帧的回放和它的显卡工作重叠。池镜像每放行一帧换一份：
// 这一帧第一次同步的页直接写进「显卡已经不读」的那一份，上一帧还在读的那一份一个字节不动；两份各记各的页代号。
// 池 g_pmem，4096 字节一页。期望值全部手算：页里写的是用例自己放的常数。
uint64_t nvn_be_finish_publish(void); uint64_t nvn_be_last_fence(void);
uint64_t rp_buf_gpu_write_note(uint64_t addr, uint64_t len, uint64_t *pool_key); void rp_buf_gpu_write_land(uint64_t pool_key, uint64_t id, int ok);
#ifdef S2A_GPU_LAND_TEST
void rp_buf_frame_flip(void);
static pthread_mutex_t g_land_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_land_cv = PTHREAD_COND_INITIALIZER;
static int g_land_pause, g_land_half, g_land_resume;
static int g_destroy_test, g_destroy_enter, g_destroy_done;
void rp_test_pool_gone(void) {
    pthread_mutex_lock(&g_land_mu);
    if (g_destroy_test) { g_destroy_enter = 1; pthread_cond_broadcast(&g_land_cv); }
    pthread_mutex_unlock(&g_land_mu);
}
static void *pool_del_thread(void *p) {
    nvn_be_pool_del(p);
    pthread_mutex_lock(&g_land_mu); g_destroy_done = 1; pthread_cond_broadcast(&g_land_cv); pthread_mutex_unlock(&g_land_mu);
    return NULL;
}
static int g_land_copies; static uint64_t g_land_copied;   // 落地拷贝统计：次数 / 累计字节（同段合并用例核对「合并后只拷一遍」）
void rp_test_land_copy(void *dst, const void *src, size_t len) {
    g_land_copies++; g_land_copied += len;
    if (!g_land_pause) { memcpy(dst, src, len); return; }
    size_t half = len / 2; memcpy(dst, src, half);
    pthread_mutex_lock(&g_land_mu); g_land_half = 1; pthread_cond_broadcast(&g_land_cv);
    while (!g_land_resume) pthread_cond_wait(&g_land_cv, &g_land_mu);
    pthread_mutex_unlock(&g_land_mu);
    memcpy((uint8_t *)dst + half, (const uint8_t *)src + half, len - half);
}
struct land_args { uint64_t key, id; };
static void *land_thread(void *p) { struct land_args *a = p; rp_buf_gpu_write_land(a->key, a->id, 1); return NULL; }
static void t_gpu_land_concurrent(void) {
    pass_fresh(); g_buf_cpu_auto = 1; g_pg_tracked = 0; g_nbw = 0; memset(g_pmem, 0x11, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    nvn_gpu_buf b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b && b->cpu);
    if (!b || !b->cpu) return;
    memset(b->cpu + 128, 0xee, 16);
    struct land_args a = {0}; a.id = rp_buf_gpu_write_note(base + 128, 16, &a.key); CHECK(a.id && a.key == base);
    pthread_t th; g_land_pause = 1; g_land_half = g_land_resume = 0;
    int started = pthread_create(&th, NULL, land_thread, &a); CHECK(started == 0);
    if (started) return;
    pthread_mutex_lock(&g_land_mu); while (!g_land_half) pthread_cond_wait(&g_land_cv, &g_land_mu); pthread_mutex_unlock(&g_land_mu);
    CHECK(rp_buf_gpu_pending(base + 128, 16));    // 锁外拷贝时仍是 GPU 待落地，bufops 不得走 CPU
    CHECK(!rp_buf_gpu_pending(base + 144, 16));   // 不相交的区间不被扩大成待落地
    rp_write_note(base + 200, 4);   // 回放自己按序改了这一页别处的字节：不受写跟踪的池由此知道页要重同步（同步照样不许覆盖落地区间）
    memset(g_land_mask, 0, sizeof g_land_mask); g_land_capture = 1; g_nbw = 0;
    nvn_gpu_buf again = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &again, NULL) == 1 && again == b);
    g_land_capture = 0;
    CHECK(g_land_mask[0] && g_land_mask[144] && !g_land_mask[128] && !g_land_mask[143]);   // 只避写回区间，邻接字节仍同步
    rp_buf_frame_flip();
    nvn_gpu_buf flipped = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base + 128, 16, &flipped, NULL) == 1 && flipped == b);
    rp_buf_frame_flip();   // 拷回期间换帧：仍选 GPU 写入的那一份
    rp_buf_gpu_write_land(a.key, a.id, 1);   // 同一登记的重复回调不能再次摘账或拷贝
    pthread_mutex_lock(&g_land_mu); g_land_resume = 1; pthread_cond_broadcast(&g_land_cv); pthread_mutex_unlock(&g_land_mu);
    CHECK(pthread_join(th, NULL) == 0); g_land_pause = 0;
    CHECK(!rp_buf_gpu_pending(base + 128, 16));   // 拷回结束才撤账
    // 后端写在录制时保存快照，随后 GPU 按序执行；因此先落地、再应用录好的传输。
    for (unsigned i = 0; i < PG; i++) if (g_land_mask[i]) b->cpu[i] = g_land_staged[i];
    unsigned bad = 0; for (unsigned i = 128; i < 144; i++) bad += b->cpu[i] != 0xee;
    printf("GPU 落地并发：16 字节 GPU 结果被后续同步覆盖 %u 字节\n", bad);
    CHECK(bad == 0); CHECK(g_pmem[128] == 0xee && g_pmem[143] == 0xee);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_pool_del(g_pmem); g_pg_tracked = 1; g_buf_cpu_auto = 0; pass_fresh();
}
// 在开着的通道里解析两页。单段从锁外 memcpy 拷到一半时解析，另测两段
// 在同一页内首尾拼接，以及一页未完全归 GPU 时必须同步的 CPU 字节。
static void t_land_noresync(void) {
    for (int log = 0; log < 2; log++) for (int scene = 0; scene < 4; scene++) {
        pass_fresh(); g_buf_cpu_auto = g_pg_tracked = 1; g_flag_buf_dirtylog = log;
        g_flag_land_noresync = scene == 0 ? 0 : -1;
        g_log_fake_on = 1; g_log_fake_bad = 0; g_log_head = 0; g_log_ident++;
        memset(g_log_unarmed, 0, sizeof g_log_unarmed);
        g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
        memset(g_block_mem, 0x11, BLOCK_TEST_PAGES * PG); memset(g_block_gen, 0, sizeof g_block_gen);
        g_dirty_now = 100; ++g_dirty_map; g_block_unreliable = 0;
        const uint64_t base = (uint64_t)(uintptr_t)g_block_mem;
        const size_t offset = 63 * PG, gpu_offset = offset + (scene == 3 ? PG / 2 : 0);
        const size_t gpu_bytes = offset + 2 * PG - gpu_offset;
        nvn_be_pool_add(g_block_mem, BLOCK_TEST_PAGES * PG);
        nvn_be_sub_begin();
        nvn_gpu_buf mirror = NULL;
        CHECK(rp_buf_resolve(&g_fake_cmd, base, BLOCK_TEST_PAGES * PG, &mirror, NULL) && mirror && mirror->cpu);
        if (!mirror || !mirror->cpu) return;
        memset(mirror->cpu + gpu_offset, 0xd4, gpu_bytes);
        struct land_args a = {0}; uint64_t id2 = 0;
        a.id = rp_buf_gpu_write_note(base + gpu_offset, scene == 2 ? PG / 2 : gpu_bytes, &a.key);
        CHECK(a.id && a.key == base);
        if (scene == 2) {
            id2 = rp_buf_gpu_write_note(base + gpu_offset + PG / 2, gpu_bytes - PG / 2, &a.key);
            CHECK(id2 && id2 != a.id);
        }
        stream first = {0}; q_env(&first); q_draw(&first); end(&first); nvn_be_exec(first.b);
        CHECK(g_pass_open);
        // 落地前这两页已经可写，此次锁外 memcpy 不会另发新的脏页事件。
        fake_log_write(63, 0x77); fake_log_write(64, 0x88);
        g_log_unarmed[63] = g_log_unarmed[64] = 1;
        pthread_t thread;
        if (scene == 1) {
            g_land_pause = 1; g_land_half = g_land_resume = 0;
            int started = pthread_create(&thread, NULL, land_thread, &a);
            CHECK(started == 0);
            if (started) return;
            pthread_mutex_lock(&g_land_mu);
            while (!g_land_half) pthread_cond_wait(&g_land_cv, &g_land_mu);
            pthread_mutex_unlock(&g_land_mu);
            CHECK(g_block_mem[offset] == 0xd4 && g_block_mem[offset + PG] == 0x88);
        }
        const uint64_t sb = sync_bytes_now(); const int prot = g_protects;
        uint64_t pf[RP_PERF_N]; rp_perf_take(pf);
        g_nbw = 0;
        nvn_gpu_buf actual = NULL;
        CHECK(rp_buf_resolve(&g_fake_cmd, base + offset, 2 * PG, &actual, NULL) && actual == mirror);
        rp_perf_take(pf);
        const uint64_t moved = sync_bytes_now() - sb, cuts = pf[RP_PERF_P_CUT_XFER];
        const uint64_t expected_bytes = scene == 0 ? 2 * PG : scene == 3 ? PG : 0;
        const uint64_t expected_cuts = scene == 0 || scene == 3;
        printf("land in-flight %s scene=%d moved=%llu cuts=%llu protects=%d expected=%llu/%llu\n",
               log ? "log" : "plain", scene, (unsigned long long)moved, (unsigned long long)cuts,
               g_protects - prot, (unsigned long long)expected_bytes, (unsigned long long)expected_cuts);
        CHECK(moved == expected_bytes && cuts == expected_cuts);
        CHECK(pf[RP_PERF_BUF_SYNC_GW_KB] == (2 * PG - expected_bytes) / 1024);
        CHECK(expected_bytes || g_protects == prot);
        CHECK(mirror->cpu[gpu_offset] == 0xd4 && mirror->cpu[offset + PG] == 0xd4);
        if (log && scene == 1) {
            CHECK(rp_buf_resolve(&g_fake_cmd, base + offset, 2 * PG, &actual, NULL));
            rp_perf_take(pf);
            CHECK(sync_bytes_now() == sb && pf[RP_PERF_BUF_SYNC_GW_KB] == 0);
        }
        if (scene == 3) {
            CHECK(g_nbw && g_bw[g_nbw - 1].off == offset && g_bw[g_nbw - 1].len == PG / 2 &&
                  g_bw[g_nbw - 1].head[0] == 0x77);
        }
        if (scene == 1) {
            pthread_mutex_lock(&g_land_mu); g_land_resume = 1; pthread_cond_broadcast(&g_land_cv); pthread_mutex_unlock(&g_land_mu);
            CHECK(pthread_join(thread, NULL) == 0); g_land_pause = 0;
        } else {
            rp_buf_gpu_write_land(a.key, a.id, 1);
            if (id2) rp_buf_gpu_write_land(a.key, id2, 1);
        }
        CHECK(g_block_mem[gpu_offset] == 0xd4 && g_block_mem[offset + 2 * PG - 1] == 0xd4);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        nvn_be_sub_begin();
        const uint64_t after = sync_bytes_now();
        CHECK(rp_buf_resolve(&g_fake_cmd, base + offset, 2 * PG, &actual, NULL));
        if (scene == 1 || scene == 2 || (log && scene == 3)) CHECK(sync_bytes_now() - after == 2 * PG);
        fake_log_write(64, 0x6e);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        nvn_be_sub_begin();
        CHECK(rp_buf_resolve(&g_fake_cmd, base + 64 * PG, PG, &actual, NULL) && actual->cpu[64 * PG] == 0x6e);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        nvn_be_pool_del(g_block_mem); g_block_mem = NULL;
        g_flag_buf_dirtylog = g_dirtylog_dflt; g_flag_land_noresync = -1;
        g_log_fake_on = g_buf_cpu_auto = 0; pass_fresh();
    }
}
extern int rp_test_gw_live(void);
static void t_gpu_land_count_cleanup(void) {
    pass_fresh(); g_buf_cpu_auto = 1; g_pg_tracked = 0;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem, key = 0;
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    nvn_gpu_buf b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b);
    CHECK(rp_test_gw_live() == 0);
    uint64_t a = rp_buf_gpu_write_note(base + 64, 16, &key);
    uint64_t b_id = rp_buf_gpu_write_note(base + 128, 16, &key);
    CHECK(a && b_id && key == base && rp_test_gw_live() == 2);
    nvn_be_pool_del(g_pmem);   // 两笔还未回调，随池撤账；迟到回调不能再减
    CHECK(rp_test_gw_live() == 0);
    rp_buf_gpu_write_land(key, a, 1); rp_buf_gpu_write_land(key, b_id, 1);
    CHECK(rp_test_gw_live() == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); g_pg_tracked = 1; g_buf_cpu_auto = 0; pass_fresh();
}
// 同段合并（开关 nvk_land_merge）：同一次提交里与已登记的显卡写相交 / 首尾相接的登记并成一条，落地只拷一遍（真机 3D 那款标题每帧 ~170 条、共 ~920 MB 在落地线程上拷）。
// 期望值全部手算：池 g_pmem 镜像初值 0x11，「显卡写」的内容用例自己放（0xCC）。
//   ① 同一段再登记 / 被包含 / 向后重叠 / 首尾相接 → 并成同一条（返回同一登记号，*merged = 1），区间 [64,104) → [64,112)；
//   ② 有空隙（[120,128)）→ 另起一条（空隙里的镜像字节可能比游戏内存旧，不桥接）；一段同时接上两条 → 三条并成 [64,128)；
//   ③ 落地：只拷 1 次共 64 字节，游戏内存 [64,128) 变 0xCC、邻接字节 63 / 128 不动；被吞掉那条的登记号迟到落地：不拷、不减账；
//   ④ 负对照：不同提交 / 不同镜像 / 开关关 / 老入口（不带 merged）→ 都不并；
//   ⑤ 走真提交路径：同一次提交里三笔登记（[600,616)+[608,624)+[616,640) 重叠）→ 落地 1 次共 40 字节；开关关 → 3 次共 56 字节（16+16+24，落地总字节随条数走）；
//   ⑥ 已开始落地（拷贝在锁外、拷到一半）的登记不再扩：首尾相接的新登记另起一条，落地的那条撤账后新的一条原样还在。
static void t_gpu_land_merge(void) {
    extern void rp_test_gw_merge_reload(void); extern int rp_test_gw_get(uint64_t, int, uint64_t *, uint64_t *);
    extern uint64_t rp_buf_gpu_write_note2(uint64_t, uint64_t, uint64_t *, int *);
    pass_fresh(); g_buf_cpu_auto = 1; g_pg_tracked = 0; g_nbw = 0; memset(g_pmem, 0x11, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem, key = 0, o = 0, l = 0; int m = 0;
    g_flag_land_merge = 1; rp_test_gw_merge_reload();
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    nvn_gpu_buf b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b && b->cpu);
    if (!b || !b->cpu) return;
    CHECK(rp_test_gw_live() == 0);
    const uint64_t id1 = rp_buf_gpu_write_note2(base + 64, 16, &key, &m);
    CHECK(id1 && key == base && m == 0 && rp_test_gw_live() == 1);
    CHECK(rp_buf_gpu_write_note2(base + 64, 16, &key, &m) == id1 && m == 1 && rp_test_gw_live() == 1);   // ① 同一段再登记
    CHECK(rp_buf_gpu_write_note2(base + 70, 4, &key, &m) == id1 && m == 1);                                 // 被包含
    CHECK(rp_buf_gpu_write_note2(base + 72, 32, &key, &m) == id1 && m == 1);                                // 向后重叠 → [64,104)
    CHECK(rp_test_gw_get(key, 0, &o, &l) == 1 && o == 64 && l == 40);
    CHECK(rp_buf_gpu_write_note2(base + 104, 8, &key, &m) == id1 && m == 1);                                // 首尾相接 → [64,112)
    CHECK(rp_test_gw_get(key, 0, &o, &l) == 1 && o == 64 && l == 48);
    const uint64_t id5 = rp_buf_gpu_write_note2(base + 120, 8, &key, &m);                                   // ② 有空隙：另起一条
    CHECK(id5 && id5 != id1 && m == 0 && rp_test_gw_live() == 2 && rp_test_gw_get(key, 0, &o, &l) == 2);
    CHECK(rp_buf_gpu_write_note2(base + 112, 8, &key, &m) == id1 && m == 1);                                // 一段同时接上两条：三条并一条 [64,128)
    CHECK(rp_test_gw_get(key, 0, &o, &l) == 1 && o == 64 && l == 64 && rp_test_gw_live() == 1);
    memset(b->cpu + 64, 0xCC, 64); g_land_copies = 0; g_land_copied = 0;                                   // ③ 落地
    rp_buf_gpu_write_land(key, id1, 1);
    CHECK(g_land_copies == 1 && g_land_copied == 64 && rp_test_gw_live() == 0);
    int bad = 0; for (unsigned i = 64; i < 128; i++) bad += g_pmem[i] != 0xCC;
    CHECK(bad == 0 && g_pmem[63] == 0x11 && g_pmem[128] == 0x11);
    rp_buf_gpu_write_land(key, id5, 1);                                                                     // 被吞掉那条的登记号迟到落地
    CHECK(g_land_copies == 1 && g_land_copied == 64 && rp_test_gw_live() == 0);
    // 先登记右侧写，再从左侧并入：镜像的原区间保留，两次写的有效前缀都要落地。
    uint64_t partial = rp_buf_gpu_write_note_prefix(base + 160, 48, 8, &key, &m);
    CHECK(partial && !m);
    CHECK(rp_buf_gpu_write_note_prefix(base + 144, 32, 4, &key, &m) == partial && m);
    CHECK(rp_test_gw_get(key, 0, &o, &l) == 1 && o == 144 && l == 64);
    struct rp_gpu_write_ref ref = {.pool_key=key, .id=partial, .index=0};
    nvn_gpu_readback_span staged = {0};
    rp_buf_gpu_write_snapshot(&ref, 1, &staged);
    CHECK(staged.src == b && staged.offset == 144 && staged.bytes == 24);
    memset(b->cpu + 144, 0xcd, 64);
    g_land_copied = 0;
    rp_buf_gpu_write_land(key, partial, 1);
    CHECK(g_land_copied == 24 && g_pmem[167] == 0xcd && g_pmem[168] == 0x11);
    // ④ 负对照 a：不同提交的登记不并（上一次提交的登记还没落地——显卡还在做——这一次的相接也不能并进去）
    uint64_t ia = rp_buf_gpu_write_note2(base + 200, 16, &key, &m); CHECK(ia && m == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_sub_begin();
    uint64_t ib = rp_buf_gpu_write_note2(base + 216, 16, &key, &m);
    CHECK(ib && ib != ia && m == 0 && rp_test_gw_get(key, 0, &o, &l) == 2 && rp_test_gw_live() == 2);
    // 负对照 b：不同镜像的登记不并（帧重叠换了一份：新那份还没建出来、当前份与已有那条不是同一份）
    rp_buf_frame_flip();
    uint64_t ic = rp_buf_gpu_write_note2(base + 232, 8, &key, &m); CHECK(ic && ic != ib && m == 0);
    rp_buf_frame_flip();
    uint64_t id = rp_buf_gpu_write_note2(base + 240, 8, &key, &m); CHECK(id && id != ic && m == 0 && rp_test_gw_get(key, 0, &o, &l) == 4);
    // 负对照 c：开关关 → 同一段登记两次也是两条；老入口（不带 merged）从不并
    g_flag_land_merge = 0; rp_test_gw_merge_reload();
    uint64_t j1 = rp_buf_gpu_write_note2(base + 300, 16, &key, &m), j2 = rp_buf_gpu_write_note2(base + 300, 16, &key, &m);
    CHECK(j1 && j2 && j1 != j2 && m == 0 && rp_test_gw_get(key, 0, &o, &l) == 6);
    g_flag_land_merge = 1; rp_test_gw_merge_reload();
    uint64_t k1 = rp_buf_gpu_write_note(base + 400, 16, &key), k2 = rp_buf_gpu_write_note(base + 400, 16, &key);
    CHECK(k1 && k2 && k1 != k2 && rp_test_gw_get(key, 0, &o, &l) == 8);
    uint64_t all[] = {ia, ib, ic, id, j1, j2, k1, k2};   // 清账：这些都是没落过地的登记，按失败撤掉（不拷）
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++) rp_buf_gpu_write_land(key, all[i], 0);
    CHECK(rp_test_gw_live() == 0 && rp_test_gw_get(key, 0, &o, &l) == 0);
    // ⑤ 真提交路径（rp_post_gpu_write → 提交表 → sub_done 落地）：一次提交里三笔重叠登记，只落地一次
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_sub_begin();   // 当前份已回到第 0 份（上面翻过两次）
    memset(b->cpu + 600, 0xDD, 64); g_land_copies = 0; g_land_copied = 0;
    CHECK(rp_post_gpu_write(base + 600, 16) && rp_post_gpu_write(base + 608, 16) && rp_post_gpu_write(base + 616, 24));   // [600,616) + [608,624) + [616,640)
    CHECK(rp_test_gw_live() == 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_land_copies == 1 && g_land_copied == 40 && rp_test_gw_live() == 0);
    bad = 0; for (unsigned i = 600; i < 640; i++) bad += g_pmem[i] != 0xDD;
    CHECK(bad == 0 && g_pmem[599] == 0x11 && g_pmem[640] == 0x11);
    // 多行登记（读回 / 图像写，一次登记几行）不并：失败回滚只能撤掉「这一次新记的」，已有那条被扩大后缩不回。两行 [800,816) 与 [808,824) 相交也各记一条
    nvn_be_sub_begin();
    CHECK(rp_post_gpu_write_rows(base + 800, 16, 8, 0, 2, 1) && rp_test_gw_live() == 2);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(rp_test_gw_live() == 0);
    g_flag_land_merge = 0; rp_test_gw_merge_reload();                                                       // 同样三笔，开关关：三条、落地 3 次共 56 字节
    nvn_be_sub_begin(); memset(b->cpu + 600, 0xEE, 64); g_land_copies = 0; g_land_copied = 0;
    CHECK(rp_post_gpu_write(base + 600, 16) && rp_post_gpu_write(base + 608, 16) && rp_post_gpu_write(base + 616, 24));
    CHECK(rp_test_gw_live() == 3);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_land_copies == 3 && g_land_copied == 56 && rp_test_gw_live() == 0);
    g_flag_land_merge = 1; rp_test_gw_merge_reload();
    // ⑥ 已开始落地的登记不能再扩：拷贝在锁外按开始时的区间做，扩出去的字节既不会被这次拷到、又会被当成已落地撤账
    memset(b->cpu + 700, 0x77, 16);
    struct land_args la = {0}; int lm = 0; la.id = rp_buf_gpu_write_note2(base + 700, 16, &la.key, &lm); CHECK(la.id && lm == 0);
    pthread_t th; g_land_pause = 1; g_land_half = g_land_resume = 0;
    int started = pthread_create(&th, NULL, land_thread, &la); CHECK(started == 0);
    if (!started) {
        pthread_mutex_lock(&g_land_mu); while (!g_land_half) pthread_cond_wait(&g_land_cv, &g_land_mu); pthread_mutex_unlock(&g_land_mu);   // 拷到一半停住
        uint64_t nid = rp_buf_gpu_write_note2(base + 716, 16, &key, &lm);                                  // 与正在落地的那条首尾相接
        CHECK(nid && nid != la.id && lm == 0 && rp_test_gw_get(key, 0, &o, &l) == 2 && rp_test_gw_live() == 2);
        pthread_mutex_lock(&g_land_mu); g_land_resume = 1; pthread_cond_broadcast(&g_land_cv); pthread_mutex_unlock(&g_land_mu);
        CHECK(pthread_join(th, NULL) == 0); g_land_pause = 0;
        CHECK(rp_test_gw_live() == 1 && rp_test_gw_get(key, 0, &o, &l) == 1 && o == 716 && l == 16);       // 落地的那条撤了，新的一条原样还在
        rp_buf_gpu_write_land(key, nid, 0);
    }
    CHECK(rp_test_gw_live() == 0);
    printf("GPU 落地合并：三笔重叠登记并成一条后落地 1 次 / 40 字节；开关关 3 次 / 56 字节\n");   // 读数行：期望 → 真结果见上面的 CHECK
    nvn_be_pool_del(g_pmem); g_pg_tracked = 1; g_buf_cpu_auto = 0; pass_fresh();
}
static void t_gpu_land_destroy(void) {
    pass_fresh(); g_buf_cpu_auto = 1; g_pg_tracked = 0; memset(g_pmem, 0x11, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    nvn_gpu_buf b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b && b->cpu);
    if (!b || !b->cpu) return;
    memset(b->cpu + 128, 0xee, 16);
    struct land_args a = {0}; a.id = rp_buf_gpu_write_note(base + 128, 16, &a.key); CHECK(a.id && a.key == base);
    int releases = g_buf_releases; pthread_t land, del;
    g_land_pause = 1; g_land_half = g_land_resume = 0; g_destroy_test = 1; g_destroy_enter = g_destroy_done = 0;
    int r = pthread_create(&land, NULL, land_thread, &a); CHECK(r == 0); if (r) return;
    pthread_mutex_lock(&g_land_mu); while (!g_land_half) pthread_cond_wait(&g_land_cv, &g_land_mu); pthread_mutex_unlock(&g_land_mu);
    r = pthread_create(&del, NULL, pool_del_thread, g_pmem); CHECK(r == 0);
    if (r) { pthread_mutex_lock(&g_land_mu); g_land_resume = 1; pthread_cond_broadcast(&g_land_cv); pthread_mutex_unlock(&g_land_mu); pthread_join(land, NULL); return; }
    pthread_mutex_lock(&g_land_mu);
    while (!g_destroy_enter) pthread_cond_wait(&g_land_cv, &g_land_mu);
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_nsec += 200000000;
    if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; until.tv_sec++; }
    while (!g_destroy_done && pthread_cond_timedwait(&g_land_cv, &g_land_mu, &until) == 0) {}
    int early = g_destroy_done;
    g_land_resume = 1; pthread_cond_broadcast(&g_land_cv); pthread_mutex_unlock(&g_land_mu);
    CHECK(pthread_join(land, NULL) == 0); CHECK(pthread_join(del, NULL) == 0);
    g_destroy_test = 0; g_land_pause = 0;
    printf("GPU 落地期间销毁：拷回完成前 pool_del 已返回=%d，镜像释放=%d（回放窗口开着：别的线程销毁的镜像缓冲延到提交收尾再放）\n", early, g_buf_releases - releases);
    CHECK(early == 0); CHECK(g_buf_releases - releases == 0); CHECK(g_pmem[128] == 0xee && g_pmem[143] == 0xee);   // 落地等完才返回；回放线程手里的 b 此刻还不能放
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); CHECK(g_buf_releases - releases == 1); g_pg_tracked = 1; g_buf_cpu_auto = 0; pass_fresh();
}
#endif

// 回放窗口：别的线程在提交回放中途销毁池。回放线程用 rp_buf_resolve 取到镜像缓冲句柄之后、后端 cmd_ref 登记它之前，游戏的别的线程销毁了这个池
// （MapVirtual 合并 / 拆分 run 时先 pool_del 再 pool_add；MemoryPoolFinalize）：当场放缓冲 = 回放线程随后录制一个放过的句柄 =
// 后端「release 之后仍在使用句柄」契约违规，整段提交失败、时间线进失败终态（真机 3D 那款标题的长局里出现过，读数见 PR）。
// 期望值手算：一份镜像 1 次释放，帧重叠换过一份的池 2 次；窗口开着时别的线程销毁 0 次，窗口收尾才放；窗口外、同线程销毁照旧当场放。
void rp_buf_frame_flip(void); void rp_buf_reset(void);
static void *pool_del_plain_thread(void *p) { nvn_be_pool_del(p); return NULL; }
static void pool_del_other_thread(const void *mem) {
    pthread_t th; int r = pthread_create(&th, NULL, pool_del_plain_thread, (void *)mem); CHECK(r == 0);
    if (r == 0) pthread_join(th, NULL);
}
static void t_pool_gone_hold(void) {
    pass_fresh(); g_buf_cpu_auto = 1; g_pg_tracked = 0; memset(g_pmem, 0x11, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_gpu_buf b = NULL; int r0;
    // ① 窗口开着，别的线程销毁：回放线程手里已取到的缓冲此刻还活着（录制它合法），池的影子照撤，窗口收尾才放
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b);
    g_rel_n = 0; r0 = g_buf_releases;
    pool_del_other_thread(g_pmem);
    CHECK(g_buf_releases == r0 && !rel_log_has(b));
    CHECK(rp_buf_mirrors() == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_buf_releases == r0 + 1 && rel_log_has(b));
    // ② 同线程销毁（回放线程自己不会在回放中途销毁池）：窗口开着也照旧当场放，收尾不多放
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b);
    r0 = g_buf_releases; nvn_be_pool_del(g_pmem);
    CHECK(g_buf_releases == r0 + 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_buf_releases == r0 + 1);
    // ③ 窗口已关（提交收尾之后），别的线程销毁：当场放
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    r0 = g_buf_releases; pool_del_other_thread(g_pmem);
    CHECK(g_buf_releases == r0 + 1);
    // ④ 帧重叠换过一份镜像的池：两份都延到收尾
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b);
    rp_buf_frame_flip(); nvn_gpu_buf b1 = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b1, NULL) == 1 && b1 && b1 != b); rp_buf_frame_flip();
    r0 = g_buf_releases; pool_del_other_thread(g_pmem);
    CHECK(g_buf_releases == r0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_buf_releases == r0 + 2);
    // ⑤ 窗口没配上收尾（失败路径 / 会话收尾）：rp_buf_reset 也把延后的缓冲放掉，不漏
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    b = NULL; CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL) == 1 && b);
    r0 = g_buf_releases; pool_del_other_thread(g_pmem);
    CHECK(g_buf_releases == r0);
    rp_buf_reset();
    CHECK(g_buf_releases == r0 + 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_buf_releases == r0 + 1);
    g_pg_tracked = 1; g_buf_cpu_auto = 0; pass_fresh();
}

static void fake_complete_first(int n) {   // 只做完最早交的 n 段
    for (int i = 0; i < n && i < g_npend; i++) { nvn_gpu_done d = {0}; d.signal = g_pend[i].signal; d.status = NVN_GPU_DONE_OK; g_pend[i].fn(g_pend[i].user, &d); g_fdone = g_pend[i].signal; }
    int k = n < g_npend ? n : g_npend; memmove(&g_pend[0], &g_pend[k], (size_t)(g_npend - k) * sizeof g_pend[0]); g_npend -= k;
}
static void t_frame_overlap(void) {
    pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_nbw = 0; g_buf_cpu_fake = NULL;
    g_buf_cpu_auto = 1; g_flag_finish_lag = 1;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem);
    nvn_gpu_buf b0 = NULL, b1 = NULL, b2 = NULL, bx = NULL;
    // 第 1 帧：第 0 页 = 0xA1（每帧都改的动态数据），第 1 页 = 0x51（只写这一次的静态数据）
    memset(g_pmem, 0xA1, PG); memset(g_pmem + PG, 0x51, PG); g_pg_gen[0] = g_pg_gen[1] = 101; g_dirty_now = 102;
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, 2 * PG, &b0, NULL) == 1); nvn_be_sub_commit();
    CHECK(g_nbw == 0 && b0 && b0->cpu[0] == 0xA1 && b0->cpu[PG] == 0x51);              // 头一次用：直接写
    uint64_t f1 = nvn_be_last_fence();
    CHECK(nvn_be_finish_publish() < f1);                                               // Finish 放行：不等第 1 帧（它还没做完）
    // 第 2 帧：游戏把第 0 页改成 0xB2。第 1 帧显卡还在做，读的是第一份镜像
    memset(g_pmem, 0xB2, PG); g_pg_gen[0] = 103; g_dirty_now = 104;
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, 2 * PG, &b1, NULL) == 1); nvn_be_sub_commit();
    CHECK(b1 && b1 != b0 && g_nbw == 0);                                               // 换了一份，照样直接写，没有退回按序拷
    CHECK(b1->cpu[0] == 0xB2 && b1->cpu[PG] == 0x51);                                  // 第二份里：这一帧的动态数据 + 它从没同步过的静态页
    CHECK(b0->cpu[0] == 0xA1 && b0->cpu[PG - 1] == 0xA1);                              // 第 1 帧还在读的那一份没被动
    uint64_t f2 = nvn_be_last_fence();
    CHECK(nvn_be_finish_publish() == f1);                                              // 这次 Finish 等到第 1 帧（上一次 Finish 时的最后一次提交）
    fake_complete_first(1);                                                            // 第 1 帧做完；第 2 帧还没
    // 第 3 帧：第 0 页 = 0xC3。换回第一份：用过它的第 1 帧已经做完 → 直接写；它自己记的代号认得出这页两帧里变过
    memset(g_pmem, 0xC3, PG); g_pg_gen[0] = 105; g_dirty_now = 106;
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, 2 * PG, &b2, NULL) == 1);
    CHECK(b2 == b0 && g_nbw == 0 && b0->cpu[0] == 0xC3 && b1->cpu[0] == 0xB2);         // 第 2 帧还在读的第二份没被动
    // 显卡在这一帧往第 2 页写了 64 字节（登记在第一份上）
    uint64_t key = 0, id = rp_buf_gpu_write_note(base + 2 * PG, 64, &key); CHECK(id && key == base);
    nvn_be_sub_commit();
    (void)f2;
    CHECK(nvn_be_finish_publish() < nvn_be_last_fence());                              // 放行、换到第二份（那 64 字节的登记是用例直接做的，Finish 不知道）
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 2 * PG, 64, &bx, NULL) == 1 && bx == b0); // 显卡写过还没落地的段：只在第一份里有
    CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &bx, NULL) == 1 && bx == b1);          // 别的段照常用当前这一份
    nvn_be_sub_commit();
    memset(b0->cpu + 2 * PG, 0xEE, 64); fake_complete(NVN_GPU_DONE_OK); rp_buf_gpu_write_land(key, id, 1);
    CHECK(g_pmem[2 * PG] == 0xEE && g_pmem[2 * PG + 63] == 0xEE && g_pmem[2 * PG + 64] != 0xEE);   // 落地：从写它的那一份回到游戏内存
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base + 2 * PG, 64, &bx, NULL) == 1 && bx == b1); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 这一帧做完时有东西要写回游戏内存（发一个事件）：Finish 等到它，回写一样不少；诊断档 2 连它也不等
    { static volatile uint32_t ev; stream e1 = {0}; uint8_t *r = put(&e1, OP_SIGNAL_EVENT, 16); volatile uint32_t *pv = &ev; uint32_t m = 0, v = 7;
      memcpy(r, &pv, 8); memcpy(r + 8, &m, 4); memcpy(r + 12, &v, 4); end(&e1); submit1(&e1);
      CHECK(nvn_be_finish_publish() == nvn_be_last_fence());
      submit1(&e1); g_flag_finish_lag = 2; CHECK(nvn_be_finish_publish() < nvn_be_last_fence()); g_flag_finish_lag = 1;
      fake_complete(NVN_GPU_DONE_OK); CHECK(ev == 7); }
    // 只有时间戳报告的提交不算「有东西要写回」：1 档照样放行；别的计数器（这里用图元数）要等
    { static uint64_t ts[2], pr[2]; stream c1 = {0}; q_counter(&c1, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_TIMESTAMP, ts); end(&c1); submit1(&c1);
      CHECK(nvn_be_finish_publish() < nvn_be_last_fence());
      stream c2 = {0}; q_counter(&c2, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_PRIMITIVES_GENERATED, pr); end(&c2); submit1(&c2);
      CHECK(nvn_be_finish_publish() == nvn_be_last_fence());
      fake_complete(NVN_GPU_DONE_OK); }
    // 关掉开关：Finish 照旧等到最后一次提交，镜像不再换
    g_flag_finish_lag = 0; CHECK(nvn_be_finish_publish() == nvn_be_last_fence());
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &bx, NULL) == 1 && bx == b1); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);   // 前面一共放行 5 次：停在第二份
    int rel0 = g_buf_releases; nvn_be_pool_del(g_pmem); CHECK(g_buf_releases - rel0 == 2);   // 池销毁：两份都放
    g_buf_cpu_auto = 0; pass_fresh();
}

// 计算阶段「用户 UBO 当存储缓冲」（反射映射 NVK_STORAGE_UBO_PTR → 核心来源 NVN_RB_UBOP，#725）：
// 取址的表是统一缓冲槽表 rp_S.ubo[5][r]，不是存储缓冲槽表 rp_S.ssbo[5][r]；其余同 SBUFP——绑池镜像里的那一段。
// 这一项被着色器写不写由布局项的 access 定（来自反射的 rw）：写的登记一次 GPU 写（完成后回写游戏内存），只读的不登记。期望值手算。
// 池 g_pmem 起手全 0x11；「GPU 写了镜像」的替身是整块镜像 = 0xEE；诱饵 = 同槽号、绑在存储缓冲槽表里的另一段（取错表就会取到它）。
static void ubop_fresh(void) {
    pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_arena_used = 0; g_nbw = 0; g_buf_cpu_fake = NULL;
    g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0;
    memset(g_pmem, 0x11, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
}
static unsigned pmem_diff(uint8_t v) { unsigned n = 0; for (unsigned i = 0; i < sizeof g_pmem; i++) n += g_pmem[i] != v; return n; }   // 池里不等于 v 的字节数
static int pmem_all(unsigned from, unsigned to, uint8_t v) { for (unsigned i = from; i < to; i++) if (g_pmem[i] != v) return 0; return 1; }   // [from, to) 全是 v
static void ubop_ent(int i, uint8_t src_kind, uint16_t reg, uint8_t access) {
    g_cprog.ent[0][i].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][i].src_kind = src_kind; g_cprog.ent[0][i].src_reg = reg; g_cprog.ent[0][i].access = access;
}
static void put_bind(stream *s, uint32_t op, int stage, int index, uint64_t addr, uint64_t size) {
    p_bind_buf *b = put(s, op, sizeof *b); b->stage = stage; b->index = index; b->addr = addr; b->size = size;
}
static void put_dispatch1(stream *s) { p_dispatch *dp = put(s, OP_DISPATCH, sizeof *dp); dp->x = 1; dp->y = 1; dp->z = 1; }

// CBUF 声明长 64 KiB，设备上限更小时只拒绝超限的那笔。截取后的长度也用于池范围和镜像解析。
static _Alignas(4096) uint8_t g_cbig[0x20000];
static void t_cbuf_device_range(void) {
    pass_fresh();
    for (unsigned i = 0; i < sizeof g_cbig; i++) g_cbig[i] = (uint8_t)(i * 7u + (i >> 12));
    nvn_be_pool_add(g_cbig, sizeof g_cbig);
    const uint64_t base = (uint64_t)(uintptr_t)g_cbig;
    const char *oversized = "uniform_buffer_exceeds_device_range";
    const struct { uint64_t off, size, max_ubo, want; } cases[] = {
        {0,       0x18000, 0x10000, 0x10000},   // 96 KiB → 64 KiB
        {0x1000,  0x8000,  0x10000, 0x8000},    // 32 KiB 原样
        {0x2000,  0x10000, 0x10000, 0x10000},   // 恰好 64 KiB
        {0x10000, 0x18000, 0x10000, 0x10000},   // 只读的前 64 KiB 正好在池里
        {0x3000,  0x18000, 0,       0x10000},   // 后端没有报告上限
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        g_caps_max_ubo = cases[i].max_ubo;
        rp_draw_reset();
        g_arena_used = 0;
        stream s = {0};
        q_env(&s);
        g_fprog.nent[0] = 1;
        g_fprog.ent[0][0].cls = NVN_GPU_RES_UBO;
        g_fprog.ent[0][0].src_kind = NVN_RB_CBUF;
        g_fprog.ent[0][0].src_reg = 2;
        put_bind(&s, OP_BIND_UBO, 0, 2, base + cases[i].off, cases[i].size);
        q_draw(&s); end(&s);
        const int draws = g_gdraws;
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_gdraws == draws + 1 && g_nfb[0] == 1 && g_fb[0][0].u.b.buf == &g_arena_buf);
        CHECK(g_fb[0][0].u.b.len == cases[i].want);
        CHECK(!memcmp(g_arena + g_fb[0][0].u.b.off, g_cbig + cases[i].off, (size_t)cases[i].want));
    }
    CHECK(nvn_unimpl_count("draw", oversized) == 0);
    CHECK(nvn_unimpl_count("draw", "uniform_buffer_outside_pools") == 0);

    g_caps_max_ubo = 0x4000;
    rp_draw_reset();
    g_arena_used = 0;
    stream low = {0};
    q_env(&low);
    g_fprog.nent[0] = 1;
    g_fprog.ent[0][0].cls = NVN_GPU_RES_UBO;
    g_fprog.ent[0][0].src_kind = NVN_RB_CBUF;
    g_fprog.ent[0][0].src_reg = 2;
    put_bind(&low, OP_BIND_UBO, 0, 2, base + 0x4000, 0x18000);
    q_draw(&low);
    put_bind(&low, OP_BIND_UBO, 0, 2, base + 0x5000, 0x2000);
    q_draw(&low); end(&low);
    const int draws = g_gdraws;
    submit1(&low); fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_unimpl_count("draw", oversized) == 1);
    CHECK(g_gdraws == draws + 1 && g_fb[0][0].u.b.len == 0x2000);
    CHECK(!memcmp(g_arena + g_fb[0][0].u.b.off, g_cbig + 0x5000, 0x2000));

    // 首次派发通过 SSBO 写前段，第二次派发的超长 CBUF 应读仍在途的镜像。
    g_caps_max_ubo = 0x10000;
    rp_draw_reset();
    g_arena_used = 0;
    g_cresolves = g_cresolve_rc = g_ndisp = 0;
    memset(&g_cprog, 0, sizeof g_cprog);
    g_cprog.is_compute = 1;
    g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 2;
    ubop_ent(0, NVN_RB_UBOP, 2, NVN_GPU_ACC_RW);
    g_cprog.ent[0][1] = (nvn_gpu_layout_ent){.cls = NVN_GPU_RES_UBO, .src_kind = NVN_RB_CBUF, .src_reg = 3};
    stream compute = {0};
    put_bind(&compute, OP_BIND_UBO, 5, 2, base + 0x4000, 0x10000);
    put_bind(&compute, OP_BIND_UBO, 5, 3, base, 0x2000);
    put_dispatch1(&compute);
    put_bind(&compute, OP_BIND_UBO, 5, 3, base + 0x4000, 0x18000);
    put_dispatch1(&compute);
    end(&compute);
    submit1(&compute);
    CHECK(g_ndisp == 2 && g_ncb == 2);
    CHECK(g_cb[1].u.b.buf && g_cb[1].u.b.buf == g_cb[0].u.b.buf);
    CHECK(g_cb[1].u.b.off == 0x4000 && g_cb[1].u.b.len == 0x10000);
    fake_complete(NVN_GPU_DONE_OK);
    g_caps_max_ubo = 0;
    g_arena_used = 0;
    pass_fresh();
}

// #1022 / #697：计算派发写了池里一段（还没落地）之后，同一次提交里 CPU 改 / 拷游戏内存的三条命令和常量缓冲读取要按 GPU 顺序看到它。
// 池 g_pmem 起手全 0x11；派发把统一缓冲槽 2 那段 W = [2*PG+32, +96) 当可写存储缓冲（登记一次 GPU 写）。之后同一次提交：
//   ① OP_COPY_B2B 源在 W 里 → 走 GPU 拷贝（镜像里 W 头 64 字节 → 4*PG），游戏内存当场不动，目标登记成 GPU 写、完成后落地；
//   ② OP_CLEAR_BUF 目标与 W 相交 → 走 GPU 填充；③ OP_UPDATE_UB 打在不相交的 5*PG → 照旧 CPU 当场写；
//   ④ 计算阶段把 4*PG（①的目标，还没落地）当常量缓冲读 → 直接绑镜像，不从游戏内存暂存旧内容；把 6*PG（没有 GPU 写）当常量缓冲读 → 照旧暂存。
// 完成时替身镜像整块 0xEE（「GPU 的结果」），期望全靠手算：落地的是 W、①的目标、②的目标三段，5*PG 是 CPU 写的 0xB2，别处一字节不动。
static void t_bufops_gpu_order(void) {
    pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_arena_used = 0; g_nbw = 0; g_buf_cpu_fake = NULL;
    g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0; g_ngx = 0; g_gx_rec = 1;
    memset(g_pmem, 0x11, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][0].src_kind = NVN_RB_UBOP; g_cprog.ent[0][0].src_reg = 2; g_cprog.ent[0][0].access = NVN_GPU_ACC_RW;
    stream s = {0};
    p_bind_buf *b = put(&s, OP_BIND_UBO, sizeof *b); b->stage = 5; b->index = 2; b->addr = base + 2 * PG + 32; b->size = 96;
    p_dispatch *dp = put(&s, OP_DISPATCH, sizeof *dp); dp->x = dp->y = dp->z = 1;
    uint8_t *r = put(&s, OP_COPY_B2B, 24); uint64_t cs = base + 2 * PG + 32, cd = base + 4 * PG, cn = 64; memcpy(r, &cs, 8); memcpy(r + 8, &cd, 8); memcpy(r + 16, &cn, 8);
    p_clear_buf *cl = put(&s, OP_CLEAR_BUF, sizeof *cl); cl->addr = base + 2 * PG + 64; cl->size = 128; cl->value = 0x55667788;
    r = put(&s, OP_UPDATE_UB, 16 + 8); uint64_t ud = base + 5 * PG, un = 8; memcpy(r, &ud, 8); memcpy(r + 8, &un, 8); memset(r + 16, 0xB2, 8);
    end(&s);
    submit1(&s);
    CHECK(g_ndisp == 1 && g_ngx == 2);
    if (g_ngx == 2) {
        CHECK(g_gx[0].op == 'c' && g_gx[0].soff == 2 * PG + 32 && g_gx[0].doff == 4 * PG && g_gx[0].n == 64 && g_gx[0].s == g_gx[0].d && g_gx[0].s != &g_arena_buf);   // ① 镜像内拷贝
        CHECK(g_gx[1].op == 'f' && g_gx[1].doff == 2 * PG + 64 && g_gx[1].n == 128 && g_gx[1].v == 0x55667788);   // ② 镜像内填充
    }
    CHECK(pmem_all(4 * PG, 4 * PG + 64, 0x11) && pmem_all(2 * PG, 3 * PG, 0x11));   // ①② 当场不动游戏内存（要等落地）
    CHECK(pmem_all(5 * PG, 5 * PG + 8, 0xB2));                                      // ③ 不相交：CPU 当场写
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0xEE, sizeof mirror); g_buf_cpu_fake = mirror;
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(pmem_all(2 * PG + 32, 2 * PG + 192, 0xEE) && pmem_all(4 * PG, 4 * PG + 64, 0xEE));   // 落地：W ∪ ② = [2*PG+32, 2*PG+192)，① 的目标
    CHECK(pmem_diff(0x11) == 160 + 64 + 8);                                                     // 别处一字节不动（8 = ③ 的 0xB2）
    // ④ 再来一次提交：先派发写 4*PG 那段（还没落地）再当常量缓冲读；6*PG 没有 GPU 写
    g_buf_cpu_fake = NULL; g_ncb = 0; g_arena_used = 0;
    g_cprog.nent[0] = 3; g_cprog.ent[0][0].src_reg = 2;
    g_cprog.ent[0][1].cls = NVN_GPU_RES_UBO; g_cprog.ent[0][1].src_kind = NVN_RB_CBUF; g_cprog.ent[0][1].src_reg = 3;
    g_cprog.ent[0][2].cls = NVN_GPU_RES_UBO; g_cprog.ent[0][2].src_kind = NVN_RB_CBUF; g_cprog.ent[0][2].src_reg = 4;
    stream s2 = {0};
    b = put(&s2, OP_BIND_UBO, sizeof *b); b->stage = 5; b->index = 2; b->addr = base + 4 * PG; b->size = 256;   // 可写那一项：先把 4*PG 登记成 GPU 写
    b = put(&s2, OP_BIND_UBO, sizeof *b); b->stage = 5; b->index = 3; b->addr = base + 7 * PG; b->size = 256;   // 第一次派发时两个常量缓冲槽先指无关的一段
    b = put(&s2, OP_BIND_UBO, sizeof *b); b->stage = 5; b->index = 4; b->addr = base + 7 * PG; b->size = 256;
    dp = put(&s2, OP_DISPATCH, sizeof *dp); dp->x = dp->y = dp->z = 1;
    b = put(&s2, OP_BIND_UBO, sizeof *b); b->stage = 5; b->index = 3; b->addr = base + 4 * PG; b->size = 256;   // 常量缓冲：读还没落地的那段
    b = put(&s2, OP_BIND_UBO, sizeof *b); b->stage = 5; b->index = 4; b->addr = base + 6 * PG; b->size = 256;   // 常量缓冲：没有 GPU 写
    dp = put(&s2, OP_DISPATCH, sizeof *dp); dp->x = dp->y = dp->z = 1;
    end(&s2); submit1(&s2);
    CHECK(g_ndisp == 3 && g_ncb == 3);
    if (g_ncb == 3) {
        CHECK(g_cb[1].ent == 1 && g_cb[1].u.b.buf != &g_arena_buf && g_cb[1].u.b.off == 4 * PG && g_cb[1].u.b.len == 256);   // 还没落地：绑镜像里的那一段
        CHECK(g_cb[2].ent == 2 && g_cb[2].u.b.buf == &g_arena_buf && g_cb[2].u.b.len == 256);                               // 没有 GPU 写：照旧暂存
    }
    fake_complete(NVN_GPU_DONE_OK);
    g_gx_rec = 0; nvn_be_pool_del(g_pmem); pass_fresh();
}

// 缓冲贴图做计算存储图像（imageBuffer，布局类别 IMAGE_TBUF）与采样纹素缓冲的偏移对齐。期望值按 Vulkan 规范手算，不跑被测实现：
//   设 minTexelBufferOffsetAlignment = 16（VUID-VkBufferViewCreateInfo-offset-02749：视图偏移须是它的倍数）。
//   ① R32UI × 4 在池偏移 2*PG+256（16 的倍数）：原地视图 = 镜像、偏移 2*PG+256、16 字节、格式 R32UI；没有拷贝；可写 → 只有这 16 字节登记 GPU 写。
//   ② 同一张挪到 2*PG+260（4 的倍数、不是 16 的倍数）：镜像 [2*PG+260, +16) 先拷进暂存区 16 对齐的一块，视图建在那块上；
//      派发后那块拷回镜像 2*PG+260，再一道 INVALIDATE_SHADER 屏障；落地只覆盖 [2*PG+260, 2*PG+276)。
//   ③ R16UI × 3 = 6 字节在 2*PG+258：要副本但长度不是 4 的倍数（缓冲拷贝按 4 字节，多拷会盖相邻字节）→ 具名跳过、不派发。
//   ④ maxTexelBufferElements = 3、贴图 4 个元素 → 具名跳过。⑤ 槽里是 2D 贴图 → 具名跳过。
//   ⑥ 采样纹素缓冲（TBUF）在 2*PG+260：只拷进副本、不拷回。
static void t_image_tbuf_replay(void) {
    pass_fresh(); g_arena_used = 0; g_buf_cpu_fake = NULL; g_caps_min_texel = 16; g_caps_max_texel = 0; rp_draw_reset(); memset(&g_itbuf_bound, 0, sizeof g_itbuf_bound);
    g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0; g_ngx = 0; g_gx_rec = 1; g_nbar = 0;
    memset(g_pmem, 0x11, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem); int tracked = g_pg_tracked; g_pg_tracked = 0;
    static pool_t pool; memset(&pool, 0, sizeof pool); pool.mem = g_pmem; pool.size = sizeof g_pmem;
    tex_t t = mk_tex(88802, 0, NVN_FORMAT_R32UI, 4, 1, 1, 0);
    t.d.target = NVN_TEXTURE_TARGET_BUFFER; t.mem = g_pmem + 2 * PG + 256; t.off = 2 * PG + 256; t.pool = &pool;
    nvn_tex_layout(&t.d, &t.L); nvn_be_set_tex_pool_n(8); nvn_be_reg_tex(3, &t, NULL);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1;
    nvn_gpu_layout_ent *e = &g_cprog.ent[0][0];
    e->cls = NVN_GPU_RES_IMAGE_TBUF; e->src_kind = NVN_RB_IMG; e->src_reg = 2; e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_RW;
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_BUFFER + 1; g_cprog.dcls[0][0] = 1; g_cprog.difmt[0][0] = 33;   // SPIR-V Image Format 33 = R32ui
    rp_S.img[5][2] = 3; p_dispatch dp = {.x = 1, .y = 1, .z = 1};
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0xEE, sizeof mirror);
    // ①
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL); nvn_be_sub_commit(); g_buf_cpu_fake = mirror; fake_complete(NVN_GPU_DONE_OK); g_buf_cpu_fake = NULL;
    CHECK(g_ndisp == 1 && g_ncb == 1 && g_ngx == 0);
    CHECK(g_ncb == 1 && g_cb[0].cls == NVN_GPU_RES_IMAGE_TBUF && g_itbuf_bound.buf && g_itbuf_bound.buf != &g_arena_buf && g_itbuf_bound.off == 2 * PG + 256 &&
          g_itbuf_bound.bytes == 16 && g_itbuf_bound.format == NVN_FORMAT_R32UI);
    CHECK(pmem_all(2 * PG + 256, 2 * PG + 272, 0xEE) && pmem_diff(0x11) == 16);
    // ②
    memset(g_pmem, 0x11, sizeof g_pmem); g_ngx = 0; g_ncb = 0; g_nbar = 0; memset(&g_itbuf_bound, 0, sizeof g_itbuf_bound);
    t.mem = g_pmem + 2 * PG + 260; t.off = 2 * PG + 260; nvn_be_reg_tex(3, &t, NULL);
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL); nvn_be_sub_commit(); g_buf_cpu_fake = mirror; fake_complete(NVN_GPU_DONE_OK); g_buf_cpu_fake = NULL;
    CHECK(g_ndisp == 2 && g_ncb == 1 && g_ngx == 2);
    if (g_ngx == 2 && g_ncb == 1) {
        CHECK(g_gx[0].op == 'c' && g_gx[0].s != &g_arena_buf && g_gx[0].soff == 2 * PG + 260 && g_gx[0].d == &g_arena_buf && g_gx[0].doff % 16 == 0 && g_gx[0].n == 16);
        CHECK(g_itbuf_bound.buf == &g_arena_buf && g_itbuf_bound.off == g_gx[0].doff && g_itbuf_bound.bytes == 16 && g_itbuf_bound.format == NVN_FORMAT_R32UI);
        CHECK(g_gx[1].op == 'c' && g_gx[1].s == &g_arena_buf && g_gx[1].soff == g_gx[0].doff && g_gx[1].d == g_gx[0].s && g_gx[1].doff == 2 * PG + 260 && g_gx[1].n == 16); }
    { int hit = 0; for (int k = 0; k < g_nbar && k < 8; k++) if ((g_bar[k].bits & NVN_BARRIER_INVALIDATE_SHADER_BIT) && g_bar[k].ndisp == 2) hit = 1; CHECK(hit); }
    CHECK(pmem_all(2 * PG + 260, 2 * PG + 276, 0xEE) && pmem_diff(0x11) == 16);
    // ③
    g_ngx = 0;
    tex_t t16 = mk_tex(88803, 0, NVN_FORMAT_R16UI, 3, 1, 1, 0); t16.d.target = NVN_TEXTURE_TARGET_BUFFER; t16.mem = g_pmem + 2 * PG + 258; t16.off = 2 * PG + 258; t16.pool = &pool;
    nvn_tex_layout(&t16.d, &t16.L); nvn_be_reg_tex(4, &t16, NULL); rp_S.img[5][2] = 4; g_cprog.difmt[0][0] = 38;   // 38 = R16ui
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == 2 && g_ngx == 0 && nvn_unimpl_count("draw", "compute_image_tbuf_unaligned_partial_word") == 1);
    // ④
    g_caps_max_texel = 3; rp_draw_reset(); rp_S.img[5][2] = 3; g_cprog.difmt[0][0] = 33;
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == 2 && nvn_unimpl_count("draw", "compute_image_tbuf_too_many_elements") == 1);
    g_caps_max_texel = 0; rp_draw_reset();
    // ⑤
    tex_t t2 = mk_tex(88804, 0, NVN_FORMAT_R32UI, 4, 1, 1, 0); t2.mem = g_pmem + 3 * PG; t2.off = 3 * PG; t2.pool = &pool; nvn_be_reg_tex(5, &t2, NULL); rp_S.img[5][2] = 5;
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == 2 && nvn_unimpl_count("draw", "compute_image_tbuf_texture_shape") == 1);
    // ⑥
    g_ngx = 0; g_ncb = 0; memset(g_tbuf_bound, 0, sizeof g_tbuf_bound);
    e->cls = NVN_GPU_RES_TBUF; e->src_kind = NVN_RB_TBUF; e->src_reg = 0; e->access = NVN_GPU_ACC_R; rp_S.tex[5][0] = 3;
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == 3 && g_ngx == 1);
    if (g_ngx == 1) CHECK(g_gx[0].soff == 2 * PG + 260 && g_gx[0].d == &g_arena_buf && g_gx[0].doff % 16 == 0 && g_gx[0].n == 16 &&
                          g_tbuf_bound[NVN_GPU_SH_CS].buf == &g_arena_buf && g_tbuf_bound[NVN_GPU_SH_CS].off == g_gx[0].doff);
    g_caps_min_texel = 0; rp_draw_reset(); rp_S.img[5][2] = 0; rp_S.tex[5][0] = 0;
    g_gx_rec = 0; g_pg_tracked = tracked; nvn_be_pool_del(g_pmem); pass_fresh();
}

// 可写：绑统一缓冲槽 2 的那一段；完成后只有登记的那些字节回到游戏内存；失败的段不回写、登记照撤。同一程序里还有一项 SBUFP（两种来源并存，各取各的表）
static void t_dispatch_ubop_rw(void) {
    ubop_fresh();
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    g_cprog.nent[0] = 2; ubop_ent(0, NVN_RB_UBOP, 2, NVN_GPU_ACC_RW); ubop_ent(1, NVN_RB_SBUFP, 0, NVN_GPU_ACC_RW);
    stream s = {0};
    put_bind(&s, OP_BIND_UBO, 5, 2, base + 2 * PG + 32, 96);    // 统一缓冲槽 2：项 0 真正的来源
    put_bind(&s, OP_BIND_SSBO, 5, 2, base + PG, 64);            // 存储缓冲槽 2：诱饵
    put_bind(&s, OP_BIND_SSBO, 5, 0, base + 3 * PG + 16, 48);   // 存储缓冲槽 0：项 1 的来源
    put_dispatch1(&s); end(&s);
    submit1(&s);
    CHECK(g_cresolves == 1 && g_ndisp == 1 && g_cbinds == 1 && g_ncb == 2 && nvn_unimpl_kinds() == 0);
    CHECK(g_cb[0].ent == 0 && g_cb[0].cls == NVN_GPU_RES_SSBO && g_cb[0].u.b.off == 2 * PG + 32 && g_cb[0].u.b.len == 96 && g_cb[0].u.b.buf != &g_arena_buf);   // 项 0：池镜像里的 [2*PG+32, +96)，不是诱饵的 PG / 64
    CHECK(g_cb[1].ent == 1 && g_cb[1].cls == NVN_GPU_RES_SSBO && g_cb[1].u.b.off == 3 * PG + 16 && g_cb[1].u.b.len == 48 && g_cb[1].u.b.buf == g_cb[0].u.b.buf);   // 项 1：同一个池、同一份镜像里的 [3*PG+16, +48)
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0xEE, sizeof mirror); g_buf_cpu_fake = mirror;
    CHECK(pmem_diff(0x11) == 0);                                                                                   // 完成之前：游戏内存不动
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(pmem_all(2 * PG + 32, 2 * PG + 128, 0xEE) && pmem_all(3 * PG + 16, 3 * PG + 64, 0xEE));                 // 登记的两段（96 + 48 字节）回到游戏内存
    CHECK(g_pmem[2 * PG + 31] == 0x11 && g_pmem[2 * PG + 128] == 0x11 && g_pmem[3 * PG + 15] == 0x11 && g_pmem[3 * PG + 64] == 0x11);   // 两段紧邻的字节不动
    CHECK(pmem_all(PG, PG + 64, 0x11) && pmem_diff(0x11) == 96 + 48);                                              // 诱饵段不动；池里别处一字节没变
    // 失败的段：不回写，登记照撤（撤了，下一次镜像同步才不会绕开那两段）
    memset(g_pmem + 2 * PG, 0x22, 2 * PG); memset(mirror, 0x77, sizeof mirror);
    stream f = {0}; put_dispatch1(&f); end(&f);
    submit1(&f); fake_complete(NVN_GPU_DONE_FAILED);
    CHECK(g_ndisp == 2 && pmem_all(2 * PG, 4 * PG, 0x22));                                                         // 派发了、这一段失败：游戏内存还是游戏自己的
    CHECK(rp_buf_gpu_write_pending(base + 2 * PG, 2 * PG) == 0);   // 两段登记均已撤，失败值仍未完成
    g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem); pass_fresh();
}

// 只读不登记；没绑 / 槽号越界 / 地址不在池里：各记一笔账、整笔不派发（沿用存储缓冲的原因名，不拆开未接清单的计数）
static void t_dispatch_ubop_ro_skip(void) {
    ubop_fresh();
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    g_cprog.nent[0] = 2; ubop_ent(0, NVN_RB_UBOP, 2, NVN_GPU_ACC_R); ubop_ent(1, NVN_RB_SBUFP, 0, NVN_GPU_ACC_RW);   // 只读的统一缓冲 + 可写的存储缓冲
    stream s = {0};
    put_bind(&s, OP_BIND_UBO, 5, 2, base + 2 * PG + 32, 96);
    put_bind(&s, OP_BIND_SSBO, 5, 2, base + PG, 64);            // 诱饵
    put_bind(&s, OP_BIND_SSBO, 5, 0, base + 3 * PG + 16, 48);
    put_dispatch1(&s); end(&s);
    submit1(&s);
    CHECK(g_ndisp == 1 && g_ncb == 2 && nvn_unimpl_kinds() == 0 && g_cb[0].cls == NVN_GPU_RES_SSBO && g_cb[0].u.b.off == 2 * PG + 32 && g_cb[0].u.b.len == 96 && g_cb[1].u.b.off == 3 * PG + 16 && g_cb[1].u.b.len == 48);
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0xEE, sizeof mirror); g_buf_cpu_fake = mirror;
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(pmem_all(3 * PG + 16, 3 * PG + 64, 0xEE) && pmem_diff(0x11) == 48);                                      // 只有可写的那一项回写；只读的统一缓冲段（镜像里同样是 0xEE）一字节没动
    CHECK(pmem_all(2 * PG + 32, 2 * PG + 128, 0x11));
    // 跳过的三种：统一缓冲槽没绑 / 槽号越界 / 绑的地址越过池尾
    int n0 = g_ndisp, b0 = g_cbinds;
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].src_reg = 5;         // ① 统一缓冲槽 5 没绑：同号的存储缓冲槽 5 绑着，也不能拿来顶
    stream u = {0}; put_bind(&u, OP_BIND_SSBO, 5, 5, base + PG, 64);
    put_bind(&u, OP_BIND_UBO, 5, 0, base + 3 * PG, 64); put_bind(&u, OP_BIND_SSBO, 0, 0, base + 3 * PG, 64);   // 给 ② 备的有效段：槽 16 回绕成槽 0、或越界读到 ubo 数组后面紧跟的 ssbo[0][0]，都会取到它们
    put_dispatch1(&u); end(&u);
    submit1(&u); fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_unbound") == 1 && g_ndisp == n0 && g_cbinds == b0);
    g_cprog.ent[0][0].src_reg = 16;                             // ② 槽号越界：统一缓冲只有 0..15（槽 0 与 ssbo[0][0] 都绑着有效段：回绕或越界读都会派发出去）
    submit1(&u); fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_unbound") == 2 && g_ndisp == n0 && g_cbinds == b0);
    g_cprog.ent[0][0].src_reg = 2;                              // ③ 统一缓冲槽 2 换成一段越过池尾的地址
    stream v = {0}; put_bind(&v, OP_BIND_UBO, 5, 2, base + sizeof g_pmem - 4, 8); put_dispatch1(&v); end(&v);
    submit1(&v); fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_unresolved") == 1 && nvn_unimpl_count("buf", "address_outside_pools") == 1 && g_ndisp == n0 && g_cbinds == b0);
    CHECK(nvn_unimpl_count("draw", "binding_source_unknown") == 0 && nvn_unimpl_kinds() == 3);                      // UBOP 是认得的来源：不落到「来源不明」
    g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem); pass_fresh();
}

// 计算存储图像按着色器声明选视图格式（对齐 Ryujinx 按绑定格式取视图、iOS img_fit）：
//   ① 声明 Unknown + uint，贴图 RG32F → 同存储的 RG32UI 视图（SULD.B 按位读，原始位不变）；② 声明 Unknown + float → 贴图原格式；
//   ③ 声明 R32ui、贴图 RG32F（字节数不同）→ 报名跳过；④ 声明 R32ui、贴图 R32F → R32UI 视图；⑤ 声明 Rg32f、贴图 RG32F → 原格式；
//   ⑥ 声明没解析出来（dtt 0）→ 照旧原格式。
static const struct nvn_gpu_view_s *img_class_view(int tex_id, uint8_t dcls, uint8_t difmt, uint8_t dtt, int *dispatched) {
    g_cprog.dtt[0][0] = dtt; g_cprog.dcls[0][0] = dcls; g_cprog.difmt[0][0] = difmt; rp_S.img[5][2] = (uint64_t)tex_id;
    int n = g_ndisp; g_ncb = 0; rp_dispatch(&g_fake_cmd, &(p_dispatch){.x = 1, .y = 1, .z = 1}, NULL);
    *dispatched = g_ndisp == n + 1;
    return *dispatched && g_ncb == 1 ? (const struct nvn_gpu_view_s *)g_cb[0].u.view : NULL;
}
static void t_compute_image_class(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0; g_cresolve_rc = 0;
    g_caps_stor[0] = NVN_FORMAT_RG32F; g_caps_stor[1] = NVN_FORMAT_RG32UI; g_caps_stor[2] = NVN_FORMAT_R32F; g_caps_stor[3] = NVN_FORMAT_R32UI;
    tex_t rg = mk_tex(671, 0, NVN_FORMAT_RG32F, 1, 1, 1, NVN_TEXTURE_FLAGS_IMAGE), r = mk_tex(672, 0x4000, NVN_FORMAT_R32F, 1, 1, 1, NVN_TEXTURE_FLAGS_IMAGE);
    nvn_be_reg_tex(3, &rg, NULL); nvn_be_reg_tex(4, &r, NULL); nvn_be_set_tex_pool_n(8);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1;
    nvn_gpu_layout_ent *e = &g_cprog.ent[0][0]; e->cls = NVN_GPU_RES_IMAGE; e->src_kind = NVN_RB_IMG; e->src_reg = 2; e->count = 1; e->access = NVN_GPU_ACC_R;
    const uint8_t t2d = NVN_TEXTURE_TARGET_2D + 1; int ok = 0; const struct nvn_gpu_view_s *v;
    nvn_be_sub_begin();
    v = img_class_view(3, 1, 0, t2d, &ok); CHECK(ok && v && v->has_v && v->v.set_format && v->v.format == NVN_FORMAT_RG32UI);   // ①
    v = img_class_view(3, 0, 0, t2d, &ok); CHECK(ok && v && !(v->has_v && v->v.set_format));                                  // ②
    uint64_t mis0 = nvn_unimpl_count("draw", "compute_image_decl_format_size_mismatch");
    v = img_class_view(3, 1, 33, t2d, &ok); CHECK(!ok && nvn_unimpl_count("draw", "compute_image_decl_format_size_mismatch") == mis0 + 1);   // ③
    v = img_class_view(4, 1, 33, t2d, &ok); CHECK(ok && v && v->has_v && v->v.set_format && v->v.format == NVN_FORMAT_R32UI);  // ④
    v = img_class_view(3, 0, 6, t2d, &ok); CHECK(ok && v && !(v->has_v && v->v.set_format));                                  // ⑤
    v = img_class_view(3, 1, 0, 0, &ok); CHECK(ok && v && !(v->has_v && v->v.set_format));                                    // ⑥
    // 计算图像是立方体存储，着色器按二维数组访问：六个面与格式重解释必须落在同一视图。
    tex_t cube = mk_tex(673, 0x8000, NVN_FORMAT_RG32F, 4, 4, 1, NVN_TEXTURE_FLAGS_IMAGE);
    cube.d.target = NVN_TEXTURE_TARGET_CUBEMAP; cube.d.depth = 6; nvn_tex_layout(&cube.d, &cube.L);
    nvn_be_reg_tex(5, &cube, NULL);
    const uint8_t tarr = NVN_TEXTURE_TARGET_2D_ARRAY + 1;
    v = img_class_view(5, 0, 0, tarr, &ok);
    CHECK(ok && v && v->has_v && v->v.set_target && v->v.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->v.set_layers && v->v.nlayers == 6);
    v = img_class_view(5, 1, 0, tarr, &ok);
    CHECK(ok && v && v->has_v && v->v.set_target && v->v.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->v.set_layers && v->v.nlayers == 6 && v->v.set_format && v->v.format == NVN_FORMAT_RG32UI);
    tex_t cubes = cube; cubes.serial = 674; cubes.d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; cubes.d.depth = 12; nvn_tex_layout(&cubes.d, &cubes.L);
    view_t sub = {0}; sub.set_layers = 1; sub.min_layer = 6; sub.nlayers = 6; sub.set_levels = 1; sub.base_level = 0; sub.nlevels = 1;
    nvn_be_reg_tex(6, &cubes, &sub);
    v = img_class_view(6, 1, 0, tarr, &ok);
    CHECK(ok && v && v->v.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->v.min_layer == 6 && v->v.nlayers == 6 && v->v.set_levels && v->v.nlevels == 1 && v->v.format == NVN_FORMAT_RG32UI);
    tex_t volume = cube; volume.serial = 675; volume.d.target = NVN_TEXTURE_TARGET_3D; volume.d.depth = 4; nvn_tex_layout(&volume.d, &volume.L);
    nvn_be_reg_tex(7, &volume, NULL);
    uint64_t bad_type = nvn_unimpl_count("draw", "compute_image_decl_type_mismatch");
    v = img_class_view(7, 0, 0, tarr, &ok);
    CHECK(!ok && !v && nvn_unimpl_count("draw", "compute_image_decl_type_mismatch") == bad_type + 1);
    // #1810：一维数组图像（宽 8、4 层、R32F）、着色器按二维数组声明：折成二维数组后同类型，照常派发、默认视图；声明成三维仍是真不符（负对照）
    nvn_be_set_tex_pool_n(16);
    tex_t one = mk_tex(676, 0xc000, NVN_FORMAT_R32F, 8, 4, 1, NVN_TEXTURE_FLAGS_IMAGE);
    one.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&one.d, &one.L);
    nvn_be_reg_tex(8, &one, NULL);
    v = img_class_view(8, 0, 0, tarr, &ok);
    CHECK(ok && v && !v->has_v && v->of->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->of->d.d.depth == 4 && v->of->d.d.height == 1 &&
          nvn_unimpl_count("draw", "compute_image_decl_type_mismatch") == bad_type + 1);
    v = img_class_view(8, 0, 0, NVN_TEXTURE_TARGET_3D + 1, &ok);
    CHECK(!ok && !v && nvn_unimpl_count("draw", "compute_image_decl_type_mismatch") == bad_type + 2);
    // 同一条路径里（评审补充 1 / 2：rp_draw.c image_resolve 调 rp_decl_view_target，不走 rp_view_by_id_decl）：登记视图记录自己带一维数组目标（第 1 层起 2 层）
    const uint64_t bad1 = nvn_unimpl_count("draw", "compute_image_decl_type_mismatch");
    tex_t one2 = mk_tex(677, 0x10000, NVN_FORMAT_R32F, 8, 4, 1, NVN_TEXTURE_FLAGS_IMAGE);
    one2.d.target = NVN_TEXTURE_TARGET_1D_ARRAY; nvn_tex_layout(&one2.d, &one2.L);
    view_t ra = {0}; ra.set_target = 1; ra.target = NVN_TEXTURE_TARGET_1D_ARRAY; ra.set_layers = 1; ra.min_layer = 1; ra.nlayers = 2;
    nvn_be_reg_tex(9, &one2, &ra);
    v = img_class_view(9, 0, 0, tarr, &ok);   // 声明二维数组：折后同类型，登记记录原样（层范围 1+2），后端存储是 8×1×4 的二维数组
    CHECK(ok && v && v->has_v && v->v.set_layers && v->v.min_layer == 1 && v->v.nlayers == 2 &&
          v->of->d.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->of->d.d.depth == 4 && v->of->d.d.height == 1);
    v = img_class_view(9, 0, 0, NVN_TEXTURE_TARGET_2D + 1, &ok);   // 声明二维：折后二维数组 ≠ 二维，要改记录（target_change = 1）：目标二维、层数 1、起点仍是第 1 层
    CHECK(ok && v && v->has_v && v->v.set_target && v->v.target == NVN_TEXTURE_TARGET_2D && v->v.set_layers && v->v.min_layer == 1 && v->v.nlayers == 1 && v->of->d.d.depth == 4);
    tex_t oned = mk_tex(678, 0x14000, NVN_FORMAT_R32F, 16, 1, 1, NVN_TEXTURE_FLAGS_IMAGE);   // 一维图像（宽 16）
    oned.d.target = NVN_TEXTURE_TARGET_1D; nvn_tex_layout(&oned.d, &oned.L);
    nvn_be_reg_tex(10, &oned, NULL);
    v = img_class_view(10, 0, 0, NVN_TEXTURE_TARGET_2D + 1, &ok);
    CHECK(ok && v && !v->has_v && v->of->d.d.target == NVN_TEXTURE_TARGET_2D && v->of->d.d.width == 16 && v->of->d.d.height == 1 && v->of->d.d.depth == 1);
    v = img_class_view(10, 0, 0, tarr, &ok);   // 声明二维数组：单层二维数组视图
    CHECK(ok && v && v->has_v && v->v.set_target && v->v.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->v.set_layers && v->v.nlayers == 1);
    CHECK(nvn_unimpl_count("draw", "compute_image_decl_type_mismatch") == bad1);
    v = img_class_view(9, 0, 0, NVN_TEXTURE_TARGET_3D + 1, &ok);   // 负对照：视图记录是一维数组、声明三维 → 真不符，整笔跳过
    CHECK(!ok && !v && nvn_unimpl_count("draw", "compute_image_decl_type_mismatch") == bad1 + 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    memset(g_caps_stor, 0, sizeof g_caps_stor); rp_S.img[5][2] = 0; pass_fresh();
}

// 出库版绑定核对失败的同一笔回退：库版声明 R32ui、实绑 RG32F（字节不符）→ 身份标坏、换现编版（Unknown）重绑，同一笔派发出去；
// 现编版再不符（lib_sourced=0）不再回退，照旧具名跳过——回退只走一次。
static void t_compute_image_lib_fallback(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0; g_cresolve_rc = 0; g_cresolve_next = NULL; g_lib_reject_calls = 0;
    g_caps_stor[0] = NVN_FORMAT_RG32F; g_caps_stor[1] = NVN_FORMAT_RG32UI;
    tex_t rg = mk_tex(671, 0, NVN_FORMAT_RG32F, 1, 1, 1, NVN_TEXTURE_FLAGS_IMAGE);
    nvn_be_reg_tex(3, &rg, NULL); nvn_be_set_tex_pool_n(8);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1; g_cprog.lib_sourced = 1;
    nvn_gpu_layout_ent *e = &g_cprog.ent[0][0]; e->cls = NVN_GPU_RES_IMAGE; e->src_kind = NVN_RB_IMG; e->src_reg = 2; e->count = 1; e->access = NVN_GPU_ACC_R;
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_2D + 1; g_cprog.dcls[0][0] = 1; g_cprog.difmt[0][0] = 33;   // 库版声明 R32ui（4B）vs 贴图 RG32F（8B）
    memcpy(&g_cprog2, &g_cprog, sizeof g_cprog); g_cprog2.lib_sourced = 0; g_cprog2.difmt[0][0] = 0;   // 现编版声明 Unknown → 同位类 RG32UI 视图
    int n = g_ndisp, res = g_cresolves;
    rp_S.img[5][2] = 3;
    nvn_be_sub_begin();
    rp_dispatch(&g_fake_cmd, &(p_dispatch){.x = 1, .y = 1, .z = 1}, NULL);
    CHECK(g_lib_reject_calls == 1 && g_cprog.lib_bad && g_ndisp == n + 1 && g_cresolves >= res + 2);   // 库版不符 → 换现编版同一笔派发
    CHECK(g_ncb == 1 && g_cb[0].u.view && ((const struct nvn_gpu_view_s *)g_cb[0].u.view)->v.format == NVN_FORMAT_RG32UI);
    // 现编版再声明不符：lib_sourced=0 不再回退（reject 不再被调），照旧具名跳过——回退只走一次
    g_cprog2.difmt[0][0] = 33; g_lib_reject_calls = 0;   // 现编版也声明 R32ui：resolve 仍交 g_cprog2（g_cresolve_next 未动）
    uint64_t m0 = nvn_unimpl_count("draw", "compute_image_decl_format_size_mismatch"); n = g_ndisp;
    rp_dispatch(&g_fake_cmd, &(p_dispatch){.x = 1, .y = 1, .z = 1}, NULL);
    CHECK(g_ndisp == n && g_lib_reject_calls == 0 && nvn_unimpl_count("draw", "compute_image_decl_format_size_mismatch") == m0 + 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_cresolve_next = NULL; g_lib_reject_calls = 0;
    memset(g_caps_stor, 0, sizeof g_caps_stor); rp_S.img[5][2] = 0; pass_fresh();
}

// 出库版采样贴图声明与实绑桥接不了：出包预编库按苹果端约定翻译（缺类型答案时一维取数译成二维行读），声明不随这一刻绑的贴图变。
//   库版声明二维、实绑三维 → 身份标坏、换现编版（声明三维）重绑，同一笔派发出去，不绑零贴图、不记「类型不符」；
//   非出库版同样不符 → 不回退（reject 不被调），照旧绑声明类型的零贴图、记一次「类型不符」（Ryujinx 语义）。
static void t_compute_tex_lib_fallback(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0; g_cresolve_rc = 0; g_cresolve_next = NULL; g_lib_reject_calls = 0;
    tex_t t = mk_tex(672, 0, NVN_FORMAT_RGBA8, 8, 8, 4, 0); t.d.target = NVN_TEXTURE_TARGET_3D;
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_be_reg_tex(3, &t, NULL); nvn_be_reg_samp(0, &sp); nvn_be_set_tex_pool_n(16);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1; g_cprog.lib_sourced = 1;
    nvn_gpu_layout_ent *e = &g_cprog.ent[0][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 2; e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_R;
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_2D + 1;                                              // 库版声明二维
    memcpy(&g_cprog2, &g_cprog, sizeof g_cprog); g_cprog2.lib_sourced = 0; g_cprog2.dtt[0][0] = NVN_TEXTURE_TARGET_3D + 1;   // 现编版按事实声明三维
    rp_S.tex[5][2] = 3;
    p_dispatch dp = {.x = 1, .y = 1, .z = 1};
    int n = g_ndisp; uint64_t mm0 = nvn_unimpl_count("tex", "decl_type_mismatch");
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_lib_reject_calls == 1 && g_cprog.lib_bad && g_ndisp == n + 1);                     // 库版不符 → 换现编版同一笔派发
    CHECK(g_ncb == 1 && g_cb[0].u.ts.view && ((const struct nvn_gpu_view_s *)g_cb[0].u.ts.view)->of->d.d.target == NVN_TEXTURE_TARGET_3D && ((const struct nvn_gpu_view_s *)g_cb[0].u.ts.view)->of->d.d.width == 8);   // 绑的是这张 8 宽的三维贴图，不是 1x1 零贴图
    CHECK(nvn_unimpl_count("tex", "decl_type_mismatch") == mm0);                               // 回退了就不是「类型不符」
    // 非出库版同样不符：不回退，绑零贴图照派发，记一次「类型不符」
    g_cprog2.dtt[0][0] = NVN_TEXTURE_TARGET_2D + 1; g_lib_reject_calls = 0; n = g_ndisp;
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_lib_reject_calls == 0 && g_ndisp == n + 1 && nvn_unimpl_count("tex", "decl_type_mismatch") == mm0 + 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_cresolve_next = NULL; g_lib_reject_calls = 0; rp_S.tex[5][2] = 0; pass_fresh();
}

// #659：计算阶段独立图像槽，读写属性决定 GPU 所有权；不涉及间接参数同步。
static void t_compute_image(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0;
    tex_t t = mk_tex(659, 0, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_IMAGE);
    nvn_be_reg_tex(3, &t, NULL); nvn_be_set_tex_pool_n(8);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1;
    nvn_gpu_layout_ent *e = &g_cprog.ent[0][0];
    e->cls = NVN_GPU_RES_IMAGE; e->src_kind = NVN_RB_IMG; e->src_reg = 2; e->count = 1; e->access = NVN_GPU_ACC_R;
    rp_S.img[5][2] = 3; rp_S.tex[5][2] = 7; // 不得误用采样贴图槽，也不需要采样器。
    p_dispatch dp = {.x = 2, .y = 1, .z = 1};
    int n = g_ndisp;
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 1 && g_ncb == 1 && g_cb[0].cls == NVN_GPU_RES_IMAGE && g_cb[0].u.view);
    CHECK(!(rp_tex_debug_state(t.serial) & 4));
    e->access = NVN_GPU_ACC_RW; rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && (rp_tex_debug_state(t.serial) & 4));
    e->src_reg = 32; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2);
    e->src_reg = 2; e->count = 2; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2);
    e->count = 1; rp_S.img[5][2] = 7; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2);
    rp_S.img[5][2] = 3; e->cls = NVN_GPU_RES_TEX; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2);
    e->cls = NVN_GPU_RES_IMAGE;
    tex_t bad = mk_tex(660, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    nvn_be_reg_tex(4, &bad, NULL); rp_S.img[5][2] = 4;
    rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2 && !(rp_tex_debug_state(bad.serial) & 4));
    // 图像已解析但后续绑定失败：不能把没派发的写登记为 GPU 所有。
    g_cprog.nent[0] = 2; g_cprog.ent[0][1].src_kind = NVN_RB_CBUF; g_cprog.ent[0][1].src_reg = 15;
    tex_t fresh = mk_tex(661, 0x8000, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_IMAGE);
    nvn_be_reg_tex(5, &fresh, NULL); rp_S.img[5][2] = 5; rp_S.ubo[5][15].addr = 0;
    rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2 && !(rp_tex_debug_state(fresh.serial) & 4));
    g_cprog.nent[0] = 1; e->access = NVN_GPU_ACC_W;
    dp.x = 0; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2 && !(rp_tex_debug_state(fresh.serial) & 4));
    dp.x = 2; dp.ind = 1; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2 && !(rp_tex_debug_state(fresh.serial) & 4));
    dp.ind = 0; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 3 && (rp_tex_debug_state(fresh.serial) & 4));
    view_t v; memset(&v, 0, sizeof v); v.set_format = 1; v.format = NVN_FORMAT_R16;
    nvn_be_reg_tex(6, &fresh, &v); rp_S.img[5][2] = 6;
    rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 3); // 原图支持storage，重解释视图不支持。
    v.set_format = 0; v.set_swizzle = 1; v.swz[0] = 4; v.swz[1] = 3; v.swz[2] = 2; v.swz[3] = 5;
    nvn_be_reg_tex(6, &fresh, &v); rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 3);
    fresh.serial = 662; fresh.d.levels = 2; nvn_tex_layout(&fresh.d, &fresh.L);
    nvn_be_reg_tex(6, &fresh, NULL); rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 3);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); pass_fresh();
}

// 计算核按贴图采样 c2 图像句柄表的字（贴图槽 32..39，同 iOS nvn_mtl.m tex_slot_h）：取图像槽 k 绑着的句柄，不读组合 / 分离贴图表的同号项。
//   图像槽 0 绑贴图 3（已登记），贴图表第 32 项放一个没登记的号 9：取对了就能派发、视图非空；读错表就取不到视图、报名跳过。
static void t_compute_tex_from_image(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0;
    tex_t t = mk_tex(663, 0, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_IMAGE);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_be_reg_tex(3, &t, NULL); nvn_be_reg_samp(0, &sp); nvn_be_set_tex_pool_n(16);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1;
    nvn_gpu_layout_ent *e = &g_cprog.ent[0][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 32; e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_R;
    memset(rp_S.img[5], 0, sizeof rp_S.img[5]); rp_S.img[5][0] = 3; rp_S.tex[5][32] = 9;
    p_dispatch dp = {.x = 1, .y = 1, .z = 1};
    int n = g_ndisp; uint64_t un0 = nvn_unimpl_count("draw", "texture_slot_unresolved");
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 1 && g_ncb == 1 && g_cb[0].cls == NVN_GPU_RES_TEX_SAMP && g_cb[0].u.ts.view && g_cb[0].u.ts.samp);
    CHECK(nvn_unimpl_count("draw", "texture_slot_unresolved") == un0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); rp_S.img[5][0] = 0; rp_S.tex[5][32] = 0; pass_fresh();
}

// 着色器把驱动句柄表 c2 当常量缓冲读（NVN_RB_DRV2）：按完整驱动表 0xA00 填（同 iOS nvn_mtl.m DRV2）——
//   组合槽 r → 0x20 + 8r；r ≥ 32 与图像表同址，只在绑了该组合槽、同址图像槽没绑时写；图像槽 r → 0x120 + 8r；
//   分离贴图槽 r → 0x168 + 8r；分离采样器槽 r → 0x568 + 8r；句柄只占每项低 4 字节，其余为 0。图形（片元）与计算各验一次。
static void drv2_fill(int st) {
    memset(rp_S.tex[st], 0, sizeof rp_S.tex[st]); memset(rp_S.img[st], 0, sizeof rp_S.img[st]); memset(rp_S.samp[st], 0, sizeof rp_S.samp[st]);
    rp_S.tex[st][3] = 0x00300007; rp_S.tex[st][33] = 0x55; rp_S.tex[st][34] = 0x66; rp_S.img[st][2] = 0x77; rp_S.img[st][0] = 0x88;
    rp_S.tex[st][128 + 5] = 0x99; rp_S.samp[st][4] = 6u << 20;
}
static int drv2_ok(const uint8_t *d, uint64_t len, char *why, size_t n) {
    static const struct { uint32_t off, want; } w[] = {
        {0x20 + 8 * 3, 0x00300007}, {0x20 + 8 * 33, 0x55}, {0x20 + 8 * 34, 0x77},   // 槽 34 与图像槽 2 同址：图像绑着 → 图像的句柄
        {0x120, 0x88}, {0x168 + 8 * 5, 0x99}, {0x568 + 8 * 4, 6u << 20}, {0x24 + 8 * 3, 0}, {0x568 + 8 * 127, 0}, {0x9FC, 0} };
    if (len != 0xA00) { snprintf(why, n, "len %llu", (unsigned long long)len); return 0; }
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) { uint32_t v; memcpy(&v, d + w[i].off, 4); if (v != w[i].want) { snprintf(why, n, "0x%x = 0x%x, want 0x%x", w[i].off, v, w[i].want); return 0; } }
    return 1;
}
static void t_drv2_table(void) {
    char why[96] = "";
    // 计算
    pass_fresh(); res_fresh(); g_arena_used = 0; g_cresolve_rc = 0;
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1;
    g_cprog.ent[0][0].cls = NVN_GPU_RES_UBO; g_cprog.ent[0][0].src_kind = NVN_RB_DRV2; g_cprog.ent[0][0].src_reg2 = UINT16_MAX;
    drv2_fill(5);
    p_dispatch dp = {.x = 1, .y = 1, .z = 1}; int n = g_ndisp;
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL);
    int cok = g_ndisp == n + 1 && g_ncb == 1 && g_cb[0].u.b.buf == &g_arena_buf && drv2_ok(g_arena + g_cb[0].u.b.off, g_cb[0].u.b.len, why, sizeof why);
    if (!cok) printf("  compute DRV2: %s\n", why);
    CHECK(cok);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 图形（片元阶段）
    pass_fresh(); g_arena_used = 0; g_resolve_rc = 0;
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_DRV2;
    q_draw(&s); end(&s);
    drv2_fill(1); memset(g_nfb, 0, sizeof g_nfb);
    submit1(&s);
    why[0] = 0;
    int gok = g_nfb[1] == 1 && g_fb[1][0].u.b.buf == &g_arena_buf && drv2_ok(g_arena + g_fb[1][0].u.b.off, g_fb[1][0].u.b.len, why, sizeof why);
    if (!gok) printf("  graphics DRV2: nfb=%u %s\n", g_nfb[1], why);
    CHECK(gok);
    fake_complete(NVN_GPU_DONE_OK);
    memset(rp_S.tex[1], 0, sizeof rp_S.tex[1]); memset(rp_S.img[1], 0, sizeof rp_S.img[1]); memset(rp_S.samp[1], 0, sizeof rp_S.samp[1]);
    memset(rp_S.tex[5], 0, sizeof rp_S.tex[5]); memset(rp_S.img[5], 0, sizeof rp_S.img[5]); memset(rp_S.samp[5], 0, sizeof rp_S.samp[5]);
    pass_fresh();
}

// 图形阶段（片元）存储图像：与计算派发同一解析，句柄取片元阶段 BindImage 的槽（rp_S.img[1]），不读采样贴图槽。
//   ① 只读：画成、片元绑一项存储图像视图、不登记 GPU 写；② 读写：画成后贴图归 GPU；③ 贴图没带图像标志：具名跳过（图形那组名字，计算计数不动）；
//   ④ 槽里的号没登记：具名跳过。撤掉绘制路径的图像解析，① 会落到 graphics_image_unresolved / 不画。
static void t_graphics_image(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0; g_resolve_rc = 0;
    tex_t img = mk_tex(667, 0, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_IMAGE), noimg = mk_tex(668, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    for (int k = 0; k < 4; k++) {
        nvn_be_reg_tex(3, &img, NULL); nvn_be_reg_tex(4, &noimg, NULL); nvn_be_set_tex_pool_n(8);   // pass_fresh 会清登记：每轮重登
        uint64_t un0 = nvn_unimpl_count("draw", "graphics_image_unresolved"), su0 = nvn_unimpl_count("draw", "graphics_image_storage_unsupported");
        uint64_t csu0 = nvn_unimpl_count("draw", "compute_image_storage_unsupported"), csh0 = nvn_unimpl_count("draw", "compute_image_shape");
        stream s = {0}; q_env(&s);
        nvn_gpu_layout_ent *e = &g_fprog.ent[1][0]; g_fprog.nent[1] = 1;
        e->cls = NVN_GPU_RES_IMAGE; e->src_kind = NVN_RB_IMG; e->src_reg = 2; e->count = 1; e->access = k == 1 ? NVN_GPU_ACC_RW : NVN_GPU_ACC_R;
        q_draw(&s); end(&s);
        memset(rp_S.img[1], 0, sizeof rp_S.img[1]); memset(rp_S.tex[1], 0, sizeof rp_S.tex[1]);
        rp_S.img[1][2] = k == 2 ? 4 : k == 3 ? 7 : 3; rp_S.tex[1][2] = 4;   // 采样贴图槽 2 放一张不可写的图：误读它 ① 就画不成
        int g0 = g_gdraws; memset(g_nfb, 0, sizeof g_nfb);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        if (k == 0) CHECK(g_gdraws == g0 + 1 && g_nfb[1] == 1 && g_fb[1][0].cls == NVN_GPU_RES_IMAGE && g_fb[1][0].u.view && !(rp_tex_debug_state(img.serial) & 4));
        if (k == 1) CHECK(g_gdraws == g0 + 1 && (rp_tex_debug_state(img.serial) & 4));
        if (k == 2) CHECK(g_gdraws == g0 && nvn_unimpl_count("draw", "graphics_image_storage_unsupported") == su0 + 1 && !(rp_tex_debug_state(noimg.serial) & 4));
        if (k == 3) CHECK(g_gdraws == g0 && nvn_unimpl_count("draw", "graphics_image_unresolved") == un0 + 1);
        CHECK(nvn_unimpl_count("draw", "compute_image_storage_unsupported") == csu0 && nvn_unimpl_count("draw", "compute_image_shape") == csh0);
        pass_fresh();
    }
    memset(rp_S.img[1], 0, sizeof rp_S.img[1]); memset(rp_S.tex[1], 0, sizeof rp_S.tex[1]);
}

static void t_compute_query_sampler(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0;
    tex_t image = mk_tex(665, 0, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_IMAGE);
    nvn_be_reg_tex(3, &image, NULL); nvn_be_set_tex_pool_n(8);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1;
    nvn_gpu_layout_ent *e = &g_cprog.ent[0][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 32;
    e->src_reg2 = UINT16_MAX; e->count = 1; e->access = NVN_GPU_ACC_R;
    g_cprog.sampler_unused[0][0] = 1; rp_S.img[5][0] = 3;
    p_dispatch dp = {.x = 1, .y = 1, .z = 1}; int n = g_ndisp, creates = g_samp_creates, releases = g_samp_releases;
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 1 && g_ncb == 1 && g_cb[0].u.ts.samp && g_samp_creates == creates + 1);
    nvn_gpu_samp query = g_cb[0].u.ts.samp;
    CHECK(query && query->x == NVN_WRAP_MODE_REPEAT && !rp_samp_by_id(0)); // 内部对象不占用 NVN 0 号槽。
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && g_cb[0].u.ts.samp == query && g_samp_creates == creates + 1);
    g_cprog.sampler_unused[0][0] = 0;
    rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 2); // 真实采样/未证用途仍要求绑定采样器。
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); pass_fresh(); res_fresh();
    CHECK(g_samp_releases == releases + 1);
}

// #1594 片 8：计算阶段的整池数组。布局里的 NVN_RB_TEXP / SAMPP 项（编译结果 pool:"texture" / "sampler"，count = 池长 + 1 表尾 dummy）
//   在派发时各取一张池快照、按计算阶段绑上整表（与绘制同一个取表函数 pool_table_resolve）。
//   期望（手算）：池长 8 / 4 → 布局项 count 9 / 5；贴图池起点 0、采样器池起点 2（两个起点故意错开：号 2 登记贴图、号 3 登记采样器）；
//   ① 只有贴图池项：派发 1 次 = 建 1 张表（8 项 IMAGE，号 2 是它自己的视图、与号 3 的零贴图视图不同）、绑 1 次（阶段 = 计算，项号 0）；
//   ② 加上采样器池项：派发 1 次 = 贴图表沿用、再建 1 张采样器表（4 项 SAMPLER，起点 2 → 项 1 = 号 3 READY、项 0 / 项 3 = 号 2 / 号 5 EMPTY）、
//      绑 2 次（项号 0 / 1）；登记没变再派发 = 不建新表、仍绑 2 次；重登记一个贴图号 = 贴图表重建、旧表一张不留（建表数 − 放表数 = 2 张在用的）；
//   ③ 池长对不上（贴图池 / 采样器池各试一次）、池长 0 = 具名跳过：不派发、不绑表；恢复后又能派发
static void t_compute_pool(void) {
    pass_fresh(); res_fresh(); g_arena_used = 0; nvn_unimpl_reset();
    tex_t a = mk_tex(667, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0), b = mk_tex(668, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_be_set_tex_pool_n(8); nvn_be_reg_tex(2, &a, NULL); nvn_be_reg_samp(3, &sp);
    rp_S.tex_base = 0; rp_S.samp_base = 2; rp_S.tex_pool_n = 8; rp_S.samp_pool_n = 4;
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.nent[0] = 1;
    nvn_gpu_layout_ent *te = &g_cprog.ent[0][0], *se = &g_cprog.ent[0][1];
    te->cls = NVN_GPU_RES_TEX; te->src_kind = NVN_RB_TEXP; te->src_reg2 = UINT16_MAX; te->count = 9; te->access = NVN_GPU_ACC_R;
    se->cls = NVN_GPU_RES_SAMP; se->src_kind = NVN_RB_SAMPP; se->src_reg2 = UINT16_MAX; se->count = 5; se->access = NVN_GPU_ACC_R;
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_2D + 1; g_cprog.dcls[0][0] = 0;   // 声明二维：登记的二维贴图取它自己的视图（声明立方体时非立方体项都解析成同一张零立方体，认不出起点）
    p_dispatch dp = {.x = 1, .y = 1, .z = 1};
    int n = g_ndisp, cr = g_pool_creates, cl = g_pool_releases, pb = g_pool_binds;
    g_pool_bind_n = 0;
    nvn_be_sub_begin(); rp_dispatch(&g_fake_cmd, &dp, NULL);   // ① 只有贴图池项
    CHECK(g_ndisp == n + 1 && g_pool_creates - cr == 1 && g_pool_binds - pb == 1 && g_pool_bind_n == 1);
    CHECK(g_pool_bind_log[0].st == NVN_GPU_SH_CS && g_pool_bind_log[0].ent == 0 && g_pool_bind_log[0].kind == NVN_GPU_POOL_IMAGE && g_pool_bind_log[0].count == 8);
    CHECK(g_pool_last_kind == NVN_GPU_POOL_IMAGE && g_pool_last_n == 8 && g_pool_last[2].resource.view && g_pool_last[3].resource.view &&
          g_pool_last[2].resource.view != g_pool_last[3].resource.view);   // 贴图池按贴图池起点 0 取：号 2 是登记的那张
    g_cprog.nent[0] = 2; g_pool_bind_n = 0; rp_dispatch(&g_fake_cmd, &dp, NULL);   // ② 再加采样器池项：贴图表沿用
    CHECK(g_ndisp == n + 2 && g_pool_creates - cr == 2 && g_pool_binds - pb == 3 && g_pool_bind_n == 2);
    CHECK(g_pool_bind_log[0].st == NVN_GPU_SH_CS && g_pool_bind_log[0].ent == 0 && g_pool_bind_log[0].kind == NVN_GPU_POOL_IMAGE && g_pool_bind_log[0].count == 8);
    CHECK(g_pool_bind_log[1].st == NVN_GPU_SH_CS && g_pool_bind_log[1].ent == 1 && g_pool_bind_log[1].kind == NVN_GPU_POOL_SAMPLER && g_pool_bind_log[1].count == 4);
    CHECK(g_pool_last_kind == NVN_GPU_POOL_SAMPLER && g_pool_last_n == 4 && g_pool_last[1].state == NVN_GPU_POOL_READY &&
          g_pool_last[0].state == NVN_GPU_POOL_EMPTY && g_pool_last[3].state == NVN_GPU_POOL_EMPTY);   // 采样器池按采样器池起点 2 取：号 3 是登记的那个
    g_pool_bind_n = 0; rp_dispatch(&g_fake_cmd, &dp, NULL);   // 登记没变：表沿用
    CHECK(g_ndisp == n + 3 && g_pool_creates - cr == 2 && g_pool_releases - cl == 0 && g_pool_bind_n == 2 && g_pool_bind_log[0].ent == 0 && g_pool_bind_log[1].ent == 1);
    nvn_be_reg_tex(5, &b, NULL); g_pool_bind_n = 0; rp_dispatch(&g_fake_cmd, &dp, NULL);   // 重登记一个贴图号：贴图表重建，每重建一张就放掉一张旧表（缓存键与图形同一套，采样器表是否同时重建随序号纪元，不钉）
    CHECK(g_ndisp == n + 4 && g_pool_creates - cr >= 3 && g_pool_releases - cl == g_pool_creates - cr - 2 && g_pool_bind_n == 2 && g_pool_bind_log[0].kind == NVN_GPU_POOL_IMAGE);
    n = g_ndisp; pb = g_pool_binds;
    te->count = 10; rp_dispatch(&g_fake_cmd, &dp, NULL);   // 贴图池项的池长（9 − 1）与当前池长 8 对不上
    CHECK(g_ndisp == n && g_pool_binds == pb && nvn_unimpl_count("draw", "pool_length_mismatch") == 1);
    te->count = 9; rp_S.samp_pool_n = 3; rp_dispatch(&g_fake_cmd, &dp, NULL);   // 采样器池项（5 − 1）与当前池长 3 对不上
    CHECK(g_ndisp == n && g_pool_binds == pb && nvn_unimpl_count("draw", "pool_length_mismatch") == 2);
    rp_S.samp_pool_n = 4; te->count = 1; rp_S.tex_pool_n = 0; rp_dispatch(&g_fake_cmd, &dp, NULL);   // 池长 0：建不出表，具名原因
    CHECK(g_ndisp == n && g_pool_binds == pb && nvn_unimpl_count("draw", "pool_length_unsupported") == 1);
    te->count = 9; rp_S.tex_pool_n = 8; g_pool_bind_n = 0; rp_dispatch(&g_fake_cmd, &dp, NULL);   // 恢复：又能派发
    CHECK(g_ndisp == n + 1 && g_pool_binds == pb + 2 && g_pool_bind_n == 2);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    rp_S.tex_pool_n = rp_S.samp_pool_n = 0; rp_S.samp_base = 0; nvn_be_unreg_tex_range(0, 8); nvn_be_set_tex_pool_n(0); pass_fresh(); res_fresh(); nvn_unimpl_reset();
}
// 贴图 → 贴图的拷贝：同尺寸同格式的逐层直接拷，各按自己视图的级与起始层；缩放 / 换格式 / 镜像 / 多重采样解析分开记账；目标从此以 GPU 为准。
static p_copy_t2t *put_t2t(stream *s, const tex_t *src, const tex_t *dst) {
    p_copy_t2t *o = put(s, OP_COPY_T2T, sizeof *o + 2 * sizeof(tex_t)); tex_t *sn = (tex_t *)(o + 1); sn[0] = *src; sn[1] = *dst; o->src = &sn[0]; o->dst = &sn[1]; return o;
}
// P2：计算存储缓冲的两种非 NVN 槽来源——map 2 别名「用户 UBO 槽的本体」，map 3 读「UBO 内 8 字节 guest 裸指针」。
// 两条都要能在同一提交里绑成 SSBO；指针字节按镜像（不是可能过时的 guest 内存）取，目标长度按池余量。
static void t_compute_sources(void) {
    pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_arena_used = 0; g_nbw = 0;
    g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0;
    for (unsigned i = 0; i < sizeof g_pmem; i++) g_pmem[i] = 0x33;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0x33, sizeof mirror);
    uint64_t target = base + PG + 64, ptr = target - 0x1000;   // nvn_ga2ca 替身 +0x1000
    memcpy(g_pmem + 3 * PG + 8, &ptr, 8); memcpy(mirror + 3 * PG + 8, &ptr, 8);
    g_buf_cpu_fake = mirror;
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 2;
    g_cprog.ent[0][0].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][0].src_kind = NVN_RB_UBOP; g_cprog.ent[0][0].src_reg = 0; g_cprog.ent[0][0].access = NVN_GPU_ACC_RW;
    g_cprog.ent[0][1].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][1].src_kind = NVN_RB_UBOA; g_cprog.ent[0][1].src_reg = 0; g_cprog.ent[0][1].src_off = 8; g_cprog.ent[0][1].access = NVN_GPU_ACC_R;
    stream s = {0};
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 5; ub->index = 0; ub->addr = base + 3 * PG; ub->size = 32;
    p_dispatch *dp = put(&s, OP_DISPATCH, sizeof *dp); dp->x = 1; dp->y = 1; dp->z = 1;
    end(&s); submit1(&s);
    CHECK(g_cbinds == 1 && g_ncb == 2 && g_ndisp == 1);
    CHECK(g_cb[0].cls == NVN_GPU_RES_SSBO && g_cb[0].u.b.off == 3 * PG && g_cb[0].u.b.len == 32);   // map 2：绑 UBO 槽的地址与长度
    CHECK(g_cb[1].cls == NVN_GPU_RES_SSBO && g_cb[1].u.b.off == PG + 64 && g_cb[1].u.b.len == sizeof g_pmem - (PG + 64));   // map 3：读指针后按池余量绑
    // 负例 1：UBO 内偏移越界 → 准确跳过，不绑错内容
    g_cprog.ent[0][1].src_off = 32;
    { stream t = {0}; p_bind_buf *u2 = put(&t, OP_BIND_UBO, sizeof *u2); *u2 = *ub;
      p_dispatch *d2 = put(&t, OP_DISPATCH, sizeof *d2); d2->x = 1; d2->y = 1; d2->z = 1; end(&t); submit1(&t); }
    CHECK(g_ndisp == 1 && nvn_unimpl_count("draw", "uniform_buffer_indirect_offset_outside") == 1);
    // 负例 2：长度未知的可写间接来源 → 准确拒绝（回写不能扩到相邻分配）
    g_cprog.ent[0][1].src_off = 8; g_cprog.ent[0][1].access = NVN_GPU_ACC_RW;
    { stream t = {0}; p_bind_buf *u3 = put(&t, OP_BIND_UBO, sizeof *u3); *u3 = *ub;
      p_dispatch *d3 = put(&t, OP_DISPATCH, sizeof *d3); d3->x = 1; d3->y = 1; d3->z = 1; end(&t); submit1(&t); }
    CHECK(g_cresolves == 3 && g_cpipe_gets == 1);
    CHECK(nvn_unimpl_count("draw", "uniform_buffer_unbound") == 0);
    CHECK(nvn_unimpl_count("draw", "uniform_buffer_unresolved") == 0);
    CHECK(nvn_unimpl_count("draw", "uniform_buffer_not_host_readable") == 0);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_outside_pools") == 0);
    CHECK(nvn_unimpl_count("draw", "storage_buffer_indirect_empty") == 0);
    CHECK(g_ndisp == 1 && nvn_unimpl_count("draw", "compute_storage_indirect_writable_unknown") == 1);
    // 间接来源的真实指针相对池偏移 64：设备要求 256 时不能交非法描述符。
    const uint64_t unaligned0 = nvn_unimpl_count("draw", "storage_buffer_offset_unaligned");
    g_caps_min_ssbo = 256; rp_draw_reset(); g_cprog.ent[0][1].access = NVN_GPU_ACC_R;
    { stream t = {0}; p_bind_buf *u4 = put(&t, OP_BIND_UBO, sizeof *u4); *u4 = *ub;
      p_dispatch *d4 = put(&t, OP_DISPATCH, sizeof *d4); d4->x = 1; d4->y = 1; d4->z = 1; end(&t); submit1(&t); }
    CHECK(g_ndisp == 1 && nvn_unimpl_count("draw", "storage_buffer_offset_unaligned") == unaligned0 + 1);
    g_caps_min_ssbo = 0; rp_draw_reset();
    g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem);
}

// 共享内存溢出缓冲（NVN_RB_SPILL，同 iOS SpillFix）：每次派发按真正的组数 × 每组字节从暂存区分一片绑上。
//   直接派发 4×2×1 组 × 1024 = 8192 字节；间接派发读游戏内存里的组数 3×1×2 → 6144 字节；
//   间接参数还有待落地的 GPU 写：读不到实值，报名跳过、不派发（不猜上限）；超过 512 MiB 报名跳过。
static void t_dispatch_spill(void) {
    pass_fresh(); g_arena_used = 0; g_cresolve_rc = 0;
    g_caps_max_ssbo = 512ull << 20; rp_draw_reset();
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0]; g_cprog.spill_bytes = 1024;
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_SSBO; g_cprog.ent[0][0].src_kind = NVN_RB_SPILL; g_cprog.ent[0][0].access = NVN_GPU_ACC_RW;
    g_cprog.ent[0][0].src_reg2 = UINT16_MAX;
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0, sizeof mirror); memset(g_pmem, 0, sizeof g_pmem); g_buf_cpu_fake = mirror;
    nvn_be_sub_begin();
    int n = g_ndisp;
    p_dispatch dp = {0}; dp.x = 4; dp.y = 2; dp.z = 1;
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 1 && g_ncb == 1 && g_cb[0].cls == NVN_GPU_RES_SSBO && g_cb[0].u.b.buf == &g_arena_buf && g_cb[0].u.b.len == 8 * 1024);
    uint32_t args[3] = {3, 1, 2}; memcpy(g_pmem + 64, args, sizeof args);
    dp = (p_dispatch){.ind = base + 64};
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && g_ncb == 1 && g_cb[0].u.b.len == 6 * 1024);
    CHECK(g_disp_ind == &g_arena_buf && !memcmp(g_arena + g_disp_off, args, sizeof args));
    memset(g_pmem + 64, 0, sizeof args);
    CHECK(!memcmp(g_arena + g_disp_off, args, sizeof args));   // 派发参数和分配尺寸来自不可变的同一快照
    nvn_gpu_buf ab; uint64_t ao; CHECK(rp_buf_resolve(&g_fake_cmd, base + 128, 12, &ab, &ao)); CHECK(rp_post_gpu_write(base + 128, 12));
    memcpy(g_pmem + 128, args, sizeof args);   // 游戏内存里有个看起来合法的旧值：也不许拿来用
    uint64_t gw0 = nvn_unimpl_count("draw", "compute_spill_indirect_gpu_written");
    dp = (p_dispatch){.ind = base + 128};
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && nvn_unimpl_count("draw", "compute_spill_indirect_gpu_written") == gw0 + 1);
    uint64_t big0 = nvn_unimpl_count("draw", "compute_spill_too_large");
    dp = (p_dispatch){0}; dp.x = 1 << 20; dp.y = 1; dp.z = 1;   // 2^20 组 × 1024 = 1 GiB
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && nvn_unimpl_count("draw", "compute_spill_too_large") == big0 + 1);
    // 组数三维相乘和再乘每组字节各自溢出时，不得回绕成小分配。
    uint64_t overflow0 = nvn_unimpl_count("draw", "compute_spill_size_overflow");
    uint32_t over[3] = {0x80000000u, 0x80000000u, 4}; memcpy(g_pmem + 192, over, sizeof over);
    dp = (p_dispatch){.ind = base + 192}; rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && nvn_unimpl_count("draw", "compute_spill_size_overflow") == overflow0 + 1);
    over[0] = over[1] = over[2] = 1u << 20; memcpy(g_pmem + 192, over, sizeof over);
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && nvn_unimpl_count("draw", "compute_spill_size_overflow") == overflow0 + 2);
    // 平台开关 nvk_spill = 0（真机 A/B）：spill 程序的派发报名跳过，不分配、不派发
    uint64_t off0 = nvn_unimpl_count("draw", "compute_spill_disabled");
    g_flag_spill = 0; rp_draw_reset();
    dp = (p_dispatch){0}; dp.x = 4; dp.y = 2; dp.z = 1; rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && nvn_unimpl_count("draw", "compute_spill_disabled") == off0 + 1);
    g_flag_spill = 1; rp_draw_reset();
    rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 3 && g_cb[0].u.b.len == 8 * 1024);
    // spill 派发日志行带「这份程序没走出库」的原因：后端没有记录 → 「没有出库记录」；有记录 → 原样带上；程序本身就是出库版 → 「出库版」
    CHECK(strstr(g_spill_line, "程序键") && strstr(g_spill_line, "；没有出库记录"));   // 没有备忘
    g_cs_note_stub = "库闸：测试原因"; rp_draw_reset(); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(strstr(g_spill_line, "；库闸：测试原因") != NULL);   // 带上后端备忘的原因
    g_cprog.lib_sourced = 1; rp_draw_reset(); rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(strstr(g_spill_line, "；出库版") != NULL && !strstr(g_spill_line, "测试原因"));   // 出库版优先
    g_cprog.lib_sourced = 0; g_cs_note_stub = NULL; rp_draw_reset();
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem); g_caps_max_ssbo = 0; rp_draw_reset(); pass_fresh();
}

// 同段GPU写参数：guest保持旧值，间接派发必须引用权威镜像，不读guest组数。
static void t_dispatch_indirect_gpu(void) {
    pass_fresh(); g_arena_used = 0; g_cresolve_rc = 0;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_UBO; g_cprog.ent[0][0].src_kind = NVN_RB_DRV0;
    static uint8_t mirror[sizeof g_pmem]; memset(mirror, 0, sizeof mirror); memset(g_pmem, 0, sizeof g_pmem);
    uint32_t old[3] = {99, 99, 99}, gpu[3] = {2, 3, 4}; memcpy(g_pmem + 64, old, 12);
    g_buf_cpu_fake = mirror; nvn_be_sub_begin(); nvn_gpu_buf args; uint64_t off;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 64, 12, &args, &off));
    rp_post_gpu_write(base + 64, 12); memcpy(mirror + 64, gpu, 12);
    g_dirty_written = ++g_dirty_now; // 旧guest所在页即使被轮询为脏，也不得盖GPU写区间。
    p_dispatch dp = {.ind = base + 64}; int n = g_ndisp;
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 1 && g_disp_ind == args && g_disp_off == off);
    CHECK(g_disp[0] == 0 && g_disp[1] == 0 && g_disp[2] == 0);
    CHECK(!memcmp(mirror + 64, gpu, 12) && !memcmp(g_pmem + 64, old, 12));
    CHECK(g_ncb == 1 && !memcmp(g_arena + g_cb[0].u.b.off + 0x200, gpu, 12));
    dp.ind = base + sizeof g_pmem - 8; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 1);
    dp.ind = base + 65; rp_dispatch(&g_fake_cmd, &dp, NULL); CHECK(g_ndisp == n + 1);
    dp.ind = 0; dp.x = 5; dp.y = 2; dp.z = 1;
    rp_dispatch(&g_fake_cmd, &dp, NULL);
    CHECK(g_ndisp == n + 2 && !g_disp_ind && g_disp_off == 0);
    CHECK(g_disp[0] == 5 && g_disp[1] == 2 && g_disp[2] == 1);
    uint32_t direct[3] = {5, 2, 1};
    CHECK(!memcmp(g_arena + g_cb[0].u.b.off + 0x200, direct, 12));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem); pass_fresh();
}

static void t_depth_copy_scaled(void) {
    pass_fresh(); g_nbl = 0; g_caps_extra = NVN_GPU_CAP_BLIT_DEPTH_STENCIL;
    tex_t a = mk_tex(176, 0, NVN_FORMAT_DEPTH24_STENCIL8, 16, 16, 1, 0), b = mk_tex(177, 0x4000, NVN_FORMAT_DEPTH24_STENCIL8, 8, 8, 1, 0);
    stream s = {0}; p_copy_t2t *o = put_t2t(&s, &a, &b);
    o->sr = (NVNcopyRegion){2, 2, 0, 12, 12, 1}; o->dr = (NVNcopyRegion){1, 1, 0, 6, 6, 1}; end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_nbl == 1 && nvn_unimpl_kinds() == 0);
    CHECK(g_bl[0].s && g_bl[0].d && g_bl[0].sr.xoffset == 2 && g_bl[0].dr.xoffset == 1 && g_bl[0].dr.width == 6 && !g_bl[0].linear);
    o->flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_nbl == 1 && nvn_unimpl_count("tex", "t2t_scaled") == 1);
    o->flags = 0; g_caps_extra = 0;
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_nbl == 1 && nvn_unimpl_count("tex", "t2t_scaled") == 2);
    pass_fresh();
}
// Ryujinx StateUpdater 与 EnumConversion 合成的 VkFrontFace 仅由 API 值和队列 flip 决定；
// 后端 C80 的 NDC 合同另含窗口原点转换。下面是两步规范换算的真值表，不调用被测归一化。
static void t_front_origin_matrix(void) {
    static const unsigned canonical[2][2][2] = {{{0,1},{1,0}},{{1,0},{0,1}}};
    pass_fresh(); g_gdraws = g_resolve_rc = 0;
    for (int api = 0; api < 2; api++) for (int flip = 0; flip < 2; flip++) for (int ll = 0; ll < 2; ll++) {
        nvn_be_set_origin_lower_left(ll); s2a_nvn_rast_flip = flip;
        stream s = {0}; q_env(&s);
        p_bind_state *state = put(&s, OP_BIND_STATE, sizeof *state); state->kind = 4;
        poly_st poly = {NVN_FACE_BACK, (unsigned)api, NVN_POLYGON_MODE_FILL, 0}; memcpy(state->bytes, &poly, sizeof poly);
        q_draw(&s); end(&s); int before = g_pipe_gets; submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_last_pdesc.poly.front == canonical[api][flip][ll] && g_last_pdesc.poly.cull == NVN_FACE_BACK && g_last_pdesc.origin_ll == ll && g_pipe_gets == before + 1);
    }
    nvn_be_set_origin_lower_left(1); s2a_nvn_rast_flip = 1; pass_fresh();
}
// 源反向端点的期望由矩形两端手算，不调用后端转换。
static void t_color_copy_mirror(void) {
    for (int axes = 1; axes <= 3; axes++) for (int variant = 0; variant < 4; variant++) {
        pass_fresh(); g_nbl = g_nct = 0;
        tex_t a = mk_tex(300, 0, variant >= 2 ? NVN_FORMAT_R11G11B10F : NVN_FORMAT_RGBA8, 64, 32, 3, 0);
        tex_t b = mk_tex(301, 0x10000, NVN_FORMAT_RGBA8, 64, 32, 3, 0);
        a.d.target = b.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; a.d.depth = b.d.depth = 4;
        nvn_tex_layout(&a.d, &a.L); nvn_tex_layout(&b.d, &b.L);
        stream s = {0}; p_copy_t2t *o = put_t2t(&s, &a, &b);
        o->has_sv = o->has_dv = 1; o->sv.set_levels = o->dv.set_levels = 1; o->sv.base_level = o->dv.base_level = 1;
        o->sv.set_layers = o->dv.set_layers = 1; o->sv.min_layer = o->dv.min_layer = 1;
        o->sr = (NVNcopyRegion){4, 2, 1, 8, 4, 1}; o->dr = (NVNcopyRegion){2, 3, 1, variant & 1 ? 4 : 8, 4, 1};
        o->flags = (axes & 1 ? NVN_COPY_FLAGS_MIRROR_X_BIT : 0) | (axes & 2 ? NVN_COPY_FLAGS_MIRROR_Y_BIT : 0) |
            (variant == 1 ? NVN_COPY_FLAGS_LINEAR_FILTER_BIT : 0); end(&s);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_nbl == 1 && g_nct == 0 && nvn_unimpl_kinds() == 0);
        if (g_nbl) {
            CHECK(g_bl[0].sr.xoffset == (axes & 1 ? 12 : 4) && g_bl[0].sr.width == (axes & 1 ? -8 : 8));
            CHECK(g_bl[0].sr.yoffset == (axes & 2 ? 6 : 2) && g_bl[0].sr.height == (axes & 2 ? -4 : 4));
            CHECK(g_bl[0].sr.zoffset == 0 && g_bl[0].sr.depth == 1 && g_bl[0].dr.xoffset == 2 && g_bl[0].dr.yoffset == 3);
            CHECK(g_bl[0].dr.width == (variant & 1 ? 4 : 8) && g_bl[0].linear == (variant == 1));
            CHECK(nvn_be_tex_gpu_owned(301) == 1 && nvn_be_tex_gpu_owned(300) == 0);
        }
    }
    for (int bad = 0; bad < 11; bad++) {
        pass_fresh(); g_nbl = g_nct = 0;
        tex_t a = mk_tex(302, 0, bad == 2 ? NVN_FORMAT_DEPTH24_STENCIL8 : bad == 3 ? NVN_FORMAT_R32UI : NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = a;
        b.serial = bad == 10 ? a.serial : 303;
        if (bad == 4) a.d.target = b.d.target = NVN_TEXTURE_TARGET_3D;
        if (bad == 6) a.d.samples = b.d.samples = 4;
        stream s = {0}; p_copy_t2t *o = put_t2t(&s, &a, &b); o->sr = (NVNcopyRegion){0, 0, 0, 8, 8, 1}; o->dr = o->sr;
        o->flags = NVN_COPY_FLAGS_MIRROR_Y_BIT;
        if (bad == 0) g_caps_remove = NVN_GPU_CAP_BLIT_SCALED;
        if (bad == 1) o->flags |= NVN_COPY_FLAGS_MIRROR_Z_BIT;
        if (bad == 5) o->sr.depth = o->dr.depth = 2;
        if (bad == 7) o->sr.width = 0;
        if (bad == 8) o->sr.xoffset = INT32_MAX;
        if (bad == 9) o->sr.yoffset = -1;
        end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_nbl == 0 && g_nct == 0 && nvn_unimpl_count("tex", "t2t_mirror") == 1);
        g_caps_remove = 0;
    }
    pass_fresh();
}
static void t_copy_t2t(void) {
    pass_fresh(); g_nct = 0; g_nbl = 0;
    tex_t a = mk_tex(71, 0, NVN_FORMAT_RGBA8, 64, 32, 3, 0), b = mk_tex(72, 0x8000, NVN_FORMAT_RGBA8, 32, 16, 1, 0), f = mk_tex(73, 0xC000, NVN_FORMAT_R11G11B10F, 32, 16, 1, 0);
    a.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; a.d.depth = 4; nvn_tex_layout(&a.d, &a.L);
    stream s = {0};
    // 源：数组贴图的第 1 级、视图从第 1 层起，区域里再从第 1 层起拷 2 层；目标：单张的第 0 级
    p_copy_t2t *o = put_t2t(&s, &a, &b); o->has_sv = 1; o->sv.set_levels = 1; o->sv.base_level = 1; o->sv.set_layers = 1; o->sv.min_layer = 1;
    o->sr = (NVNcopyRegion){4, 2, 1, 16, 8, 2}; o->dr = (NVNcopyRegion){8, 4, 0, 16, 8, 2};
    o = put_t2t(&s, &a, &b); o->sr = (NVNcopyRegion){0, 0, 0, 32, 16, 1}; o->dr = (NVNcopyRegion){0, 0, 0, 16, 8, 1}; o->flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;   // 缩放：后端的缩放拷贝，线性过滤
    o = put_t2t(&s, &a, &f); o->sr = (NVNcopyRegion){0, 0, 0, 16, 8, 1}; o->dr = o->sr;                                                             // 换格式：同上，最近点
    { tex_t u = mk_tex(74, 0x10000, NVN_FORMAT_R32UI, 32, 16, 1, 0); o = put_t2t(&s, &u, &b); o->sr = (NVNcopyRegion){0, 0, 0, 32, 16, 1}; o->dr = (NVNcopyRegion){0, 0, 0, 16, 8, 1}; }   // 整数格式换格式：没接
    o = put_t2t(&s, &a, &b); o->sr = (NVNcopyRegion){0, 0, 0, 16, 8, 1}; o->dr = o->sr; o->flags = NVN_COPY_FLAGS_MIRROR_Z_BIT;                       // Z 镜像：没接
    o = put_t2t(&s, &b, &b); o->sr = (NVNcopyRegion){0, 0, 0, 32, 16, 1}; o->dr = o->sr; o->flags = (int32_t)DM_COPY_RESOLVE;                         // 降采样记录、两边采样数相同：整张拷贝
    { tex_t ms = mk_tex(75, 0, NVN_FORMAT_RGBA8, 32, 16, 1, 0); ms.d.samples = 4; ms.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
      o = put_t2t(&s, &ms, &b); o->flags = DM_COPY_RESOLVE; }
    // 整数、深度、缩放、反向采样转换仍记账，不能误走解析。
    for (int k = 0; k < 4; k++) {
        tex_t ms = mk_tex(76 + k * 2, 0, k == 0 ? NVN_FORMAT_R32UI : k == 1 ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8, 32, 16, 1, 0);
        tex_t dst = ms; dst.serial++; ms.d.samples = 4; ms.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
        if (k == 2) dst.d.width = 16;
        if (k == 3) { ms.d.samples = 1; dst.d.samples = 4; }
        o = put_t2t(&s, &ms, &dst); o->flags = DM_COPY_RESOLVE;
    }
    end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_nct == 3);
    CHECK(g_ct[0].sl == 1 && g_ct[0].sy == 2 && g_ct[0].dl == 0 && g_ct[0].dy == 0 && g_ct[0].sr.xoffset == 4 && g_ct[0].sr.width == 16 && g_ct[0].sr.depth == 1 && g_ct[0].sr.zoffset == 0 && g_ct[0].dr.xoffset == 8 && g_ct[0].dr.yoffset == 4);
    CHECK(g_ct[1].sy == 3 && g_ct[1].dy == 1 && g_ct[0].s != g_ct[0].d);
    CHECK(g_ct[2].sr.width == 32 && g_ct[2].sr.height == 16 && g_ct[2].s == g_ct[2].d);
    CHECK(g_nbl == 3 && g_bl[0].linear == 1 && g_bl[0].sr.width == 32 && g_bl[0].dr.width == 16 && g_bl[0].dr.height == 8 && g_bl[0].s && g_bl[0].d && g_bl[0].s != g_bl[0].d);
    CHECK(g_bl[1].linear == 0 && g_bl[1].sr.width == 16 && g_bl[1].dr.width == 16);
    CHECK(g_bl[2].linear == 0 && g_bl[2].sr.width == 32 && g_bl[2].sr.height == 16 && g_bl[2].dr.width == 32 && g_bl[2].dr.depth == 1);
    CHECK(nvn_unimpl_count("tex", "t2t_format_change") == 1 && nvn_unimpl_count("tex", "t2t_mirror") == 1 && nvn_unimpl_kinds() == 4);
    CHECK(nvn_unimpl_count("tex", "t2t_multisample_resolve") == 3 && nvn_unimpl_count("tex", "t2t_downsample_case") == 1);
    CHECK(nvn_be_tex_gpu_owned(72) == 1 && nvn_be_tex_gpu_owned(73) == 1 && nvn_be_tex_gpu_owned(71) == 0);
    pass_fresh();
}

// 条件渲染：判成不画的那一段里绘制和清除都丢掉；时间戳报告的计数值在回放时按 0 算；遮挡报告写过的地址当不知道、照画；新提交从「画」开始。
static void t_render_cond(void) {
    pass_fresh(); g_rcond = 1; g_gdraws = 0; g_tick = 9;
    uint64_t mem[6] = {1, 0, 2, 0, 0, 0};   // 两条报告各 16 字节：计数值现在是 CPU 留下的 1 和 2（不相等）
    stream s = {0}; q_env(&s);
    p_render_cond *rc = put(&s, OP_RENDER_COND, sizeof *rc); rc->mode = 3;                                      // SetRenderEnable(false)
    q_draw(&s); p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf;      // 都丢
    rc = put(&s, OP_RENDER_COND, sizeof *rc); rc->mode = 2; q_draw(&s);                                         // 恢复：画（第 1 笔）
    rc = put(&s, OP_RENDER_COND, sizeof *rc); rc->mode = 0; rc->addr = (uint64_t)(uintptr_t)mem; q_draw(&s);    // 相等才画：内存里 1 ≠ 2 → 不画
    q_counter(&s, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_TIMESTAMP, &mem[0]); q_counter(&s, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_TIMESTAMP, &mem[2]);
    rc = put(&s, OP_RENDER_COND, sizeof *rc); rc->mode = 0; rc->addr = (uint64_t)(uintptr_t)mem; q_draw(&s);    // 两条时间戳报告在这次提交里：都按 0 → 相等 → 画（第 2 笔）
    rc = put(&s, OP_RENDER_COND, sizeof *rc); rc->mode = 1; rc->addr = (uint64_t)(uintptr_t)mem; q_draw(&s);    // 不等才画 → 不画
    end(&s);
    submit1(&s);
    CHECK(g_gdraws == 2 && g_ncr == 0 && pd_zero_clear(&g_pd[0]));   // 被丢的清除没有折进通道
    // 已提交、没完成：下一次提交里这两个地址仍按 0 算（内存里还是 1 和 2）
    stream s2 = {0}; q_draw(&s2);                                                                                 // 新提交从「画」开始（上一次提交末尾是不画）
    rc = put(&s2, OP_RENDER_COND, sizeof *rc); rc->mode = 1; rc->addr = (uint64_t)(uintptr_t)mem; q_draw(&s2);  // 0 == 0 → 不等才画 → 不画
    end(&s2);
    submit1(&s2);
    CHECK(g_gdraws == 3);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(mem[0] == 0 && mem[2] == 0);   // 完成：计数值写成 0
    mem[0] = 5;                          // 完成之后 CPU 又写了：以内存为准
    stream s3 = {0}; rc = put(&s3, OP_RENDER_COND, sizeof *rc); rc->mode = 1; rc->addr = (uint64_t)(uintptr_t)mem; q_draw(&s3);   // 5 ≠ 0 → 画
    q_counter(&s3, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, &mem[4]);                                                 // 遮挡报告写过的地址：从此当不知道
    rc = put(&s3, OP_RENDER_COND, sizeof *rc); rc->mode = 0; rc->addr = (uint64_t)(uintptr_t)&mem[2]; q_draw(&s3);              // 比的是 mem[2] 与 mem[4]：后者不知道 → 照画
    end(&s3);
    submit1(&s3); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 5 && nvn_unimpl_kinds() == 0);
    g_rcond = 0; pass_fresh();
}

// 图形管线失败表（rp_draw.c rp_pfail_*，对齐 iOS g_pso_fail / g_pso_retry）：确定性失败同一份描述只交后端一次、整局不再建；
// 暂时性（OOM）按 nvn_pso_retry.h 退避 30 / 120 / 480 帧重试、连续 4 次进失败表、建成一次清零；会话重置后重建；分类按「返回码 + 原文数字归一 + 粗签名」分开计数。
static tex_t g_pf_rt;
static void pf_setup(void) {
    pass_fresh();
    memset(&g_fprog, 0, sizeof g_fprog);
    g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1];
    g_fprog.color_out_mask = 0xffffffffu;
    g_resolve_rc = 0;
    g_pf_rt = mk_tex(2101, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
}
// 一段：设目标、（可选）多边形状态、n 笔同样的绘制
static void pf_draws(unsigned n, unsigned prim, int cull) {
    stream s = {0}; put_set_rt(&s, &g_pf_rt, NULL);
    if (cull >= 0) { p_bind_state *bs = put(&s, OP_BIND_STATE, sizeof *bs); bs->kind = 4; poly_st py = {(unsigned)cull, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; memcpy(bs->bytes, &py, sizeof py); }
    for (unsigned i = 0; i < n; i++) { p_draw *d = put(&s, OP_DRAW, sizeof *d); d->prim = prim; d->count = 3; }
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
}
static void t_pipe_fail_table(void) {
    const char *reason; uint64_t fails, cached; char sum[1024];
    // 1. 确定性（COMPILE）：一段 5 笔只交后端 1 次，其余 4 笔失败表挡下；再来一段 5 笔 0 次；后端改成能建也不再试（整局不重建）
    pf_setup(); g_pipe_why = "vkCreateGraphicsPipelines 失败（VK_ERROR_UNKNOWN -13）";
    int gets = g_pipe_gets, draws = g_gdraws, red = g_pf_red;
    g_pipe_rc = NVN_GPU_E_COMPILE; pf_draws(5, 4, -1);
    printf("PIPE_FAIL fixed: backend calls %d (expect 1), cached %llu (expect 4)\n", g_pipe_gets - gets, (unsigned long long)nvn_unimpl_count("gpu", "pipe_get_cached_fail"));
    CHECK(g_pipe_gets == gets + 1 && g_gdraws == draws);
    CHECK(nvn_unimpl_count("gpu", "pipe_get") == 1 && nvn_unimpl_count("gpu", "pipe_get_cached_fail") == 4);
    CHECK(rp_pipe_fail_class(0, &reason, &fails, &cached) && fails == 1 && cached == 4);
    CHECK(!strcmp(reason, "COMPILE:vkCreateGraphicsPipelines 失败（VK_ERROR_UNKNOWN -#）"));
    CHECK(g_pf_red == red + 1 && strstr(g_pf_last, "第 1 类（确定性") && strstr(g_pf_last, "阶段 vs+fs、拓扑 4、颜色附件 1、采样 0、深度 无、变换反馈 无、顶点属性 0"));
    printf("PIPE_FAIL red: %s\n", g_pf_last);
    pf_draws(5, 4, -1);
    CHECK(g_pipe_gets == gets + 1 && nvn_unimpl_count("gpu", "pipe_get_cached_fail") == 9 && g_pf_red == red + 1);
    g_pipe_rc = 0; pf_draws(1, 4, -1);
    CHECK(g_pipe_gets == gets + 1 && g_gdraws == draws);
    CHECK(rp_pipe_fail_summary(sum, sizeof sum) == 1 && strstr(sum, "1 类，后端建失败 1 次、失败表挡下 10 笔") && strstr(sum, "#1 COMPILE:"));
    printf("PIPE_FAIL summary: %s\n", sum);
    CHECK(rp_pipe_fail_summary(sum, sizeof sum) == 0);   // 这之后没有新失败 / 新挡下：不再打
    // 2. 会话重置后重建：同一份描述再交后端，这回建成
    pf_setup(); gets = g_pipe_gets; draws = g_gdraws;
    pf_draws(5, 4, -1);
    CHECK(g_pipe_gets == gets + 1 && g_gdraws == draws + 5 && nvn_unimpl_count("gpu", "pipe_get_cached_fail") == 0 && !rp_pipe_fail_class(0, NULL, NULL, NULL));
    // 3. 暂时性（OOM）：帧 100 失败 → 退避到 130；130 再试失败 → 250；250 → 730；730 第 4 次失败进失败表，之后不再试
    pf_setup(); g_pipe_why = "vkCreateGraphicsPipelines 失败（VK_ERROR_OUT_OF_DEVICE_MEMORY -2）"; gets = g_pipe_gets;
    g_pipe_rc = NVN_GPU_E_OOM; nvn_be_set_frame(100); pf_draws(3, 4, -1);
    CHECK(g_pipe_gets == gets + 1 && nvn_unimpl_count("gpu", "pipe_get_cached_fail") == 2);
    nvn_be_set_frame(129); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 1);
    nvn_be_set_frame(130); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 2);
    nvn_be_set_frame(249); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 2);
    nvn_be_set_frame(250); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 3);
    nvn_be_set_frame(729); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 3);
    nvn_be_set_frame(730); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 4);
    nvn_be_set_frame(100000); pf_draws(1, 4, -1);
    printf("PIPE_FAIL transient: backend calls %d over frames 100..100000 (expect 4)\n", g_pipe_gets - gets);
    CHECK(g_pipe_gets == gets + 4);
    CHECK(rp_pipe_fail_class(0, &reason, &fails, &cached) && fails == 4 && !strcmp(reason, "OOM:vkCreateGraphicsPipelines 失败（VK_ERROR_OUT_OF_DEVICE_MEMORY -#）"));
    CHECK(strstr(g_pf_last, "暂时性，退避重试"));
    // 3b. 暂时性失败后建成一次，连续次数清零：再失败从 30 帧退避算起（没清零会是 120 帧）
    pf_setup(); gets = g_pipe_gets;
    g_pipe_rc = NVN_GPU_E_OOM; nvn_be_set_frame(0); pf_draws(1, 4, -1);
    g_pipe_rc = 0; nvn_be_set_frame(30); pf_draws(1, 4, -1);
    CHECK(g_pipe_gets == gets + 2);
    g_pipe_rc = NVN_GPU_E_OOM; nvn_be_set_frame(31); pf_draws(1, 4, -1);   // 新的一段：g_pipe 换 cmd 重取
    CHECK(g_pipe_gets == gets + 3);
    nvn_be_set_frame(60); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 3);
    nvn_be_set_frame(61); pf_draws(1, 4, -1); CHECK(g_pipe_gets == gets + 4);
    // 4. 不记表：会话关闭中（STOPPED）每笔照旧交后端
    pf_setup(); gets = g_pipe_gets; g_pipe_rc = NVN_GPU_E_STOPPED; nvn_be_set_frame(0); pf_draws(3, 4, -1);
    CHECK(g_pipe_gets == gets + 3 && nvn_unimpl_count("gpu", "pipe_get_cached_fail") == 0 && !rp_pipe_fail_class(0, NULL, NULL, NULL));
    // 5. 分类：原文里的数字归一（set 4 / set 5 同一类），签名不同（拓扑 4 / 5）分开，原因不同分开；每类只打一行红
    pf_setup(); red = g_pf_red;
    g_pipe_rc = NVN_GPU_E_LAYOUT; g_pipe_why = "set 4 超过四 set 上限"; pf_draws(2, 4, -1);
    g_pipe_why = "set 5 超过四 set 上限"; pf_draws(2, 4, NVN_FACE_BACK);    // 描述不同（剔除面）、签名相同 → 同一类第二次失败
    g_pipe_rc = NVN_GPU_E_COMPILE; g_pipe_why = "vkCreateGraphicsPipelines 失败（VK_ERROR_UNKNOWN -13）"; pf_draws(2, 5, -1);
    g_pipe_rc = NVN_GPU_E_UNSUPPORTED; g_pipe_why = "设备未启用无格式存储图像所需特性"; pf_draws(2, 6, -1);
    CHECK(rp_pipe_fail_class(0, &reason, &fails, &cached) && !strcmp(reason, "LAYOUT:set # 超过四 set 上限") && fails == 2 && cached == 2);
    CHECK(rp_pipe_fail_class(1, &reason, &fails, &cached) && !strncmp(reason, "COMPILE:", 8) && fails == 1 && cached == 1);
    CHECK(rp_pipe_fail_class(2, &reason, &fails, &cached) && !strcmp(reason, "UNSUPPORTED:设备未启用无格式存储图像所需特性") && fails == 1 && cached == 1);
    CHECK(!rp_pipe_fail_class(3, NULL, NULL, NULL) && g_pf_red == red + 3);
    CHECK(rp_pipe_fail_summary(sum, sizeof sum) == 1 && strstr(sum, "3 类，后端建失败 4 次、失败表挡下 4 笔") && strstr(sum, "#1 LAYOUT:set # 超过四 set 上限"));
    printf("PIPE_FAIL classes: %s\n", sum);
    g_pipe_rc = 0; g_pipe_why = NULL; nvn_be_set_frame(0); pass_fresh();
}

// #863: a rejecting backend attributes only unsupported non-COPY operations by op number.
static void t_logic_op_accounting(void) {
    pass_fresh();
    memset(&g_fprog, 0, sizeof g_fprog);
    g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1];
    g_fprog.color_out_mask = 0xffffffffu;
    g_resolve_rc = 0;
    tex_t rt = mk_tex(863, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    for (unsigned i = 0; i < 5; i++) {
        unsigned op = i == 1 ? NVN_LOGIC_OP_COPY : i == 3 ? NVN_LOGIC_OP_SET : NVN_LOGIC_OP_XOR;
        int rc = i == 2 ? NVN_GPU_E_ARG : i == 4 ? 0 : NVN_GPU_E_UNSUPPORTED;
        nvn_unimpl_reset(); rp_draw_reset();   // 替身每轮对同一份描述换返回码：上一轮的失败会进图形管线失败表，每轮先清（同会话重置）
        stream s = {0}; put_set_rt(&s, &rt, NULL);
        p_bind_state *bs = put(&s, OP_BIND_STATE, sizeof *bs); bs->kind = 1;
        color_st color = {0}; color.logic_op = op; memcpy(bs->bytes, &color, sizeof color);
        p_draw *d = put(&s, OP_DRAW, sizeof *d); d->prim = 4; d->count = 3;
        end(&s);
        int before = g_gdraws, gets = g_pipe_gets;
        g_pipe_rc = rc; submit1(&s); fake_complete(NVN_GPU_DONE_OK); g_pipe_rc = 0;
        CHECK(g_pipe_gets > gets && g_last_pdesc.color.logic_op == op);
        CHECK(g_gdraws == before + (rc == 0));
        CHECK(nvn_unimpl_count("gpu", "logic_op_6") == (i == 0 ? 1 : 0));
        CHECK(nvn_unimpl_count("gpu", "pipe_get") == (i == 1 || i == 2 ? 1 : 0));
        CHECK(nvn_unimpl_count("gpu", "logic_op_3") == 0);
        CHECK(nvn_unimpl_count("gpu", "logic_op_15") == (i == 3 ? 1 : 0));
    }
    pass_fresh();
}

// #697 B: support-copy and direct quad-index arena failures keep counters and bounded first detail.
static void t_arena697_detail(void) {
    for(int direct=0;direct<2;direct++) {
        pass_fresh();memset(&g_fprog,0,sizeof g_fprog);
        g_fprog.sh[0]=&g_fsh[0];g_fprog.sh[1]=&g_fsh[1];g_fprog.color_out_mask=0xffffffffu;
        if(!direct){g_fprog.support_bytes=2608;g_fprog.nent[0]=1;
            g_fprog.ent[0][0].cls=NVN_GPU_RES_UBO;g_fprog.ent[0][0].src_kind=NVN_RB_SUPPORT;}
        g_resolve_rc=0;nvn_unimpl_reset();int logs=g_arena_detail_lines,draws=g_gdraws,truncated=g_arena_truncated_lines;
        tex_t rt=mk_tex(697+direct,0,NVN_FORMAT_RGBA8,16,16,1,0);
        stream s={0};put_set_rt(&s,&rt,NULL);p_draw *d=put(&s,OP_DRAW,sizeof *d);
        d->prim=direct?7:4;d->count=direct?4:3;end(&s);
        for(int cause=1;cause<=12;cause++) {
            g_arena697_fail=cause;
            submit1(&s);fake_complete(NVN_GPU_DONE_OK);submit1(&s);fake_complete(NVN_GPU_DONE_OK);
        }
        g_arena697_fail=0;
        CHECK(g_gdraws==draws && nvn_unimpl_count("draw","arena_alloc_failed")==24);
        CHECK(g_arena_detail_lines==logs+8 && g_arena_truncated_lines==truncated+1); // distinct reasons once each; fixed table bounds logging
        char rc_text[32];snprintf(rc_text,sizeof rc_text,"rc=%d",NVN_GPU_E_ORDER);
        CHECK(strstr(g_arena697_lines[logs],rc_text) && strstr(g_arena697_lines[logs],"first-bind-outside-pass-1"));
        CHECK(strstr(g_arena697_lines[logs+1],"missing backend detail"));
        CHECK(strstr(g_arena697_lines[logs+2],"rc=0 ") && strstr(g_arena697_lines[logs+2],"CPU mapping unavailable"));
        CHECK(strstr(g_arena_detail,direct?"bytes=24 ":"bytes=2608 "));
        CHECK(strlen(g_arena_detail)<sizeof g_arena_detail-1);
    }
    pass_fresh();
}

// 间接计数多绘：同组记录（同计数地址、序号连续、记录地址等步长）在第 0 条整组交一次，GPU 读计数；后面同组的不再报名。
//   设备没开 drawIndirectCount 时整组报 indirect_count_no_device_support 跳过；组断了（序号跳号）只画能证实的前半组、断处报名；
//   OP_MULTI_DRAW（超过 1,024 条整段一条）直接按记录给的条数与步长交。期望值按记录手算：3 条 → max_draws 3、步长 20。紧跟 t_draw，沿用它绑好的状态。
static void mdi_prelude(stream *s, tex_t *rt) {   // 每段自己设渲染目标、常量缓冲 2（替身程序要它），不靠前面用例留下的状态
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; put_set_rt(s, rt, NULL);
    p_bind_buf *ub = put(s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 128; ub->size = 256;
}
static void t_draw_mdi(void) {
    uint64_t base = (uint64_t)(uintptr_t)g_pmem, cnt = base + 5 * PG, ind = base + 4 * PG;   // 绘制记录里的地址录制时已换成 CPU 地址（hle_nvn.c GA）
    nvn_be_pool_add(g_pmem, sizeof g_pmem);   // 间接记录、计数都在这个池里
    int d0 = g_gdraws; uint64_t gu0 = nvn_unimpl_count("draw", "indirect_count_group_unavailable");
    g_caps_extra = NVN_GPU_CAP_INDIRECT_COUNT;
    tex_t rtt = mk_tex(91, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);   // 渲染目标不跨提交保留：每段自己设
    stream s = {0}; mdi_prelude(&s, &rtt);
    for (int i = 0; i < 3; i++) {
        p_draw_idx *di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->instances = 1; di->ibuf = base + 3 * PG;
        di->ind = ind + (uint64_t)i * 20; di->mdi_cnt = cnt; di->mdi_i = i;
    }
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 1 && g_last_da.max_draws == 3 && g_last_da.stride == 20 && g_last_da.cnt != NULL && g_last_da.ind != NULL);
    CHECK(g_last_da.cnt_off == 5 * PG && g_last_da.ind_off == 4 * PG);
    CHECK(nvn_unimpl_count("draw", "indirect_count_group_unavailable") == gu0);   // 同组后两条认出来、不报名
    // 组断了：序号 0 后直接是 2 → 第 0 条单独一组（max 1），序号 2 报名跳过
    uint64_t broken0 = nvn_unimpl_count("draw", "indirect_count_group_broken");
    stream t = {0}; mdi_prelude(&t, &rtt);
    for (int i = 0; i < 3; i += 2) {
        p_draw_idx *di = put(&t, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->instances = 1; di->ibuf = base + 3 * PG;
        di->ind = ind + (uint64_t)i * 20; di->mdi_cnt = cnt; di->mdi_i = i;
    }
    end(&t); submit1(&t); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 2 && g_last_da.max_draws == 1 && nvn_unimpl_count("draw", "indirect_count_group_broken") == broken0 + 1 && nvn_unimpl_count("draw", "indirect_count_group_unavailable") == gu0);
    // 超过 1,024 条的一次调用：一条 OP_MULTI_DRAW
    stream m = {0}; mdi_prelude(&m, &rtt); p_multi_draw *md = put(&m, OP_MULTI_DRAW, sizeof *md);
    md->ind = ind; md->cnt = cnt; md->ibuf = base + 3 * PG; md->stride = 20; md->max = 100; md->prim = 4; md->itype = 1;
    end(&m); submit1(&m); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 3 && g_last_da.max_draws == 100 && g_last_da.stride == 20 && g_last_da.cnt != NULL);
    // 设备没开：整组跳过、报名一次，不交给后端（交了后端整段判失败）
    g_caps_extra = 0;
    uint64_t nd0 = nvn_unimpl_count("draw", "indirect_count_no_device_support");
    uint64_t unsupported0 = nvn_unimpl_count("draw", "indirect_count_unsupported");
    stream n = {0}; mdi_prelude(&n, &rtt);
    for (int i = 0; i < 3; i++) {
        p_draw_idx *di = put(&n, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->instances = 1; di->ibuf = base + 3 * PG;
        di->ind = ind + (uint64_t)i * 20; di->mdi_cnt = cnt; di->mdi_i = i;
    }
    end(&n); submit1(&n); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 3 && nvn_unimpl_count("draw", "indirect_count_unsupported") == unsupported0 + 1 && nvn_unimpl_count("draw", "indirect_count_no_device_support") == nd0);
}

// 间接绘制的 base instance 拷贝只给读 c0 + 0x644 的阶段（rp_prog.binst_unread 位 = 编译器 c0Reads 证明不读）。
//   逐笔拷（nvk_binst_hoist=0）：每笔拷贝都要先收通道（拷贝得排在 GPU 生产者之后），同一通道里连着 3 笔间接绘制：要拷 = 3 个通道；
//   通道级批量拷（缺省）：开通道前前瞻，3 笔的 firstInstance 一次拷进各自切片 = 1 个通道。都已证不读 = 1 个通道、0 次拷贝。
//   真机读数（9000 空轨 2nd 进 3D）：每帧收通道 884 次里 844 次是这一处。紧跟 t_draw_mdi，沿用它的池与渲染目标。
// 每笔发出时两阶段绑的驱动段切片：need 里的阶段必须是一次 4 字节拷贝的目标（目标 = 切片 + 0x644），那次拷贝的源是这一笔自己的记录 + 16，
//   且录在 min_draw 笔绘制之后；不同笔的切片互不相同。返回 1 = 都对。
static int binst_slices_ok(int n, uint8_t need, const uint64_t *rec_off, const int *min_draw, const char *what) {
    int ok = g_ndd == n;
    for (int d = 0; d < n && d < g_ndd; d++) for (int st = 0; st < 2; st++) {
        if (!(need >> st & 1)) continue;
        int hit = -1;
        for (int k = 0; k < g_nc4; k++) if (g_c4[k].d == g_dd[d].b[st] && g_c4[k].doff == g_dd[d].off[st] + 0x644) hit = k;
        if (hit < 0 || g_c4[hit].soff != rec_off[d] + 16 || g_c4[hit].at_draw < min_draw[d]) {
            printf("  %s: draw %d stage %d slice %#llx copy %d src %#llx (expect %#llx) recorded after %d draws (expect >= %d)\n", what, d, st,
                   (unsigned long long)g_dd[d].off[st], hit, hit < 0 ? 0ull : (unsigned long long)g_c4[hit].soff, (unsigned long long)(rec_off[d] + 16),
                   hit < 0 ? -1 : g_c4[hit].at_draw, min_draw[d]);
            ok = 0;
        }
        for (int e = 0; e < d; e++) if (g_dd[e].b[st] == g_dd[d].b[st] && g_dd[e].off[st] == g_dd[d].off[st]) { printf("  %s: draws %d and %d share the stage %d slice\n", what, e, d, st); ok = 0; }
    }
    return ok;
}
static void t_draw_ind_binst(void) {
    uint64_t base = (uint64_t)(uintptr_t)g_pmem, ind = base + 4 * PG;
    struct rp_prog saved = g_fprog;
    tex_t rtt = mk_tex(92, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    // hoist：开关值；calls：多区域拷贝调用次数（开关 2 时一次前瞻一次，单段拷贝为 0）
    static const struct { uint8_t unread; int hoist, copies, passes, calls; const char *what; } cases[] = {
        { 0, 0, 6, 3, 0, "per-draw, unknown / read: both stages copy, every indirect draw cuts the pass" },
        { 1, 0, 3, 3, 0, "per-draw, vs proven, fs still reads: only fs copies, the cut stays" },
        { 3, 0, 0, 1, 0, "per-draw, both stages proven: no copy, one pass for all three draws" },
        { 0, 1, 6, 1, 0, "hoisted one copy each, unknown / read: three draws' copies before the pass, one pass" },
        { 1, 1, 3, 1, 0, "hoisted one copy each, fs reads: fs copies before the pass, one pass" },
        { 3, 1, 0, 1, 0, "hoisted one copy each, both stages proven: no copy, one pass" },
        { 0, 2, 6, 1, 1, "hoisted multi-region, unknown / read: one call with 6 regions for vs+fs, one pass" },
        { 1, 2, 3, 1, 1, "hoisted multi-region, fs reads: one call with 3 regions for fs, one pass" },
        { 3, 2, 0, 1, 0, "hoisted multi-region, both stages proven: no copy, one pass" },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        g_flag_binst_hoist = cases[k].hoist; rp_draw_reset();   // 开关会话打开时读定
        for (int st = 0; st < 2; st++) { g_fprog.nent[st] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[st][0]; memset(e, 0, sizeof *e); e->src_reg2 = UINT16_MAX; e->cls = NVN_GPU_RES_UBO; e->src_kind = NVN_RB_DRV0; e->count = 1; }
        g_fprog.binst_unread = cases[k].unread; g_arena_used = 0;   // 替身暂存区 64 KB 不查越界：每段从头用（上一段已完成）
        int c0 = g_ncopy4, p0 = g_npd, d0 = g_gdraws, v0 = g_ncopyv, s0 = g_ncopy1; g_nc4 = g_ndd = 0; g_dd_on = 1; g_copyv_stages = 0;
        stream s = {0}; mdi_prelude(&s, &rtt);
        for (int i = 0; i < 3; i++) { p_draw_idx *di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->instances = 1; di->ibuf = base + 3 * PG; di->ind = ind + (uint64_t)i * 20; }
        end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK); g_dd_on = 0;
        const int singles = cases[k].hoist == 2 ? 0 : cases[k].copies;
        const uint32_t want_st = cases[k].calls ? (3u & ~cases[k].unread) : 0;
        if (g_gdraws - d0 != 3 || g_ncopy4 - c0 != cases[k].copies || g_npd - p0 != cases[k].passes || g_ncopyv - v0 != cases[k].calls || g_ncopy1 - s0 != singles || g_copyv_stages != want_st) {
            printf("  %s: draws %d copies %d passes %d multi-region calls %d single copies %d barrier stages %#x (expect 3 / %d / %d / %d / %d / %#x)\n", cases[k].what, g_gdraws - d0, g_ncopy4 - c0, g_npd - p0,
                   g_ncopyv - v0, g_ncopy1 - s0, g_copyv_stages, cases[k].copies, cases[k].passes, cases[k].calls, singles, want_st);
            CHECK(0);
        }
        const uint64_t ro[3] = {4 * PG, 4 * PG + 20, 4 * PG + 40}; const int md[3] = {0, 0, 0};
        const uint8_t need = (uint8_t)(3u & ~cases[k].unread);
        if (!binst_slices_ok(3, need, ro, md, cases[k].what)) CHECK(0);
    }
    // 批量拷的边界（开关缺省）：每段 3 笔间接绘制的记录 i 在 ind + 20 i，两阶段都要拷
    static const struct { int kind, passes, copies, calls; int min_draw[3]; const char *what; } edge[] = {
        { 0, 1, 6, 1, {0, 0, 0}, "a direct draw opens the pass: the indirect draws after it are hoisted with it, no cut" },
        { 1, 2, 6, 2, {0, 0, 2}, "UPDATE_UB writes between draw 1 and 2: the scan stops there, draw 2 cuts and copies after the write" },
        { 2, 2, 10, 2, {0, 1, 1}, "a main-stream mirror write while draw 0 records: the hoisted table is dropped, draw 1 cuts and copies draws 1-2 again" },
    };
    for (size_t k = 0; k < sizeof edge / sizeof edge[0]; k++) {
        g_flag_binst_hoist = -1; rp_draw_reset();
        g_fprog.binst_unread = 0; g_arena_used = 0;
        int c0 = g_ncopy4, p0 = g_npd, d0 = g_gdraws, v0 = g_ncopyv; g_nc4 = g_ndd = 0; g_dd_on = 1; g_hz_bump_at = edge[k].kind == 2 ? 0 : -1;
        stream s = {0}; mdi_prelude(&s, &rtt);
        if (edge[k].kind == 0) {   // 新会话先学阶段掩码（一段里一笔要拷的间接绘制），再让一笔普通绘制开通道：它开之前前瞻
            stream l = {0}; mdi_prelude(&l, &rtt); p_draw_idx *li = put(&l, OP_DRAW_IDX, sizeof *li); li->prim = 4; li->itype = 1; li->instances = 1; li->ibuf = base + 3 * PG; li->ind = ind; end(&l);
            submit1(&l); fake_complete(NVN_GPU_DONE_OK);
            c0 = g_ncopy4; p0 = g_npd; d0 = g_gdraws; v0 = g_ncopyv; g_nc4 = g_ndd = 0;
            p_draw_idx *pre = put(&s, OP_DRAW_IDX, sizeof *pre); pre->prim = 4; pre->itype = 1; pre->instances = 1; pre->count = 3; pre->ibuf = base + 3 * PG;
        }
        for (int i = 0; i < 3; i++) {
            if (edge[k].kind == 1 && i == 2) {   // 改第 2 笔记录的 firstInstance（CPU 路：游戏内存此刻改）
                uint8_t *u = put(&s, OP_UPDATE_UB, 16 + 8); uint64_t d = ind + 2 * 20 + 16, n = 4; uint32_t v = 77;
                memcpy(u, &d, 8); memcpy(u + 8, &n, 8); memcpy(u + 16, &v, 4);
            }
            p_draw_idx *di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->instances = 1; di->ibuf = base + 3 * PG; di->ind = ind + (uint64_t)i * 20;
        }
        end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK); g_dd_on = 0; g_hz_bump_at = -1;
        const int pre = edge[k].kind == 0;
        if (g_gdraws - d0 != 3 + pre || g_ncopy4 - c0 != edge[k].copies || g_npd - p0 != edge[k].passes || g_ncopyv - v0 != edge[k].calls) {
            printf("  %s: draws %d copies %d passes %d multi-region calls %d (expect %d / %d / %d / %d)\n", edge[k].what, g_gdraws - d0, g_ncopy4 - c0, g_npd - p0, g_ncopyv - v0, 3 + pre, edge[k].copies, edge[k].passes, edge[k].calls);
            CHECK(0);
        }
        if (pre) { memmove(g_dd, g_dd + 1, sizeof g_dd[0] * 63); g_ndd--; }
        const uint64_t ro[3] = {4 * PG, 4 * PG + 20, 4 * PG + 40};
        if (!binst_slices_ok(3, 3, ro, edge[k].min_draw, edge[k].what)) CHECK(0);
    }
    g_flag_binst_hoist = -1; rp_draw_reset();
    g_fprog = saved;
}

// 已解码 count 组与 base instance（#1787 第二处）：组大小 1 只有一条命令，语义与单条间接绘制相同——绑 DRV0 且没证明不读 c0 + 0x644 的程序照画，
//   firstInstance 取这一条记录的（4 字节拷贝：源 = 记录 + 16，目标 = 驱动段切片 + 0x644）；组大小 ≥ 2 照旧具名跳 indirect_count_driver_base_instance
//   （每条命令各有 firstInstance，驱动段那一个字只拷得了第一条的）；两阶段都证明不读的，大小任意都照画、不拷；OP_MULTI_DRAW 保持原路径（照画、拷第一条的）。
//   紧跟 t_draw_ind_binst，沿用它的池与渲染目标；每段自己设两阶段各一项 DRV0 布局项。
static void t_draw_mdi_binst(void) {
    uint64_t base = (uint64_t)(uintptr_t)g_pmem, ind = base + 4 * PG, cnt = base + 5 * PG;
    struct rp_prog saved = g_fprog;
    tex_t rtt = mk_tex(94, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    g_caps_extra = NVN_GPU_CAP_INDIRECT_COUNT;
    g_flag_binst_hoist = 0; rp_draw_reset();   // 逐笔拷（count 组本来就不进批量拷）
    // kind 0 = 已解码 count 组（n 条记录，序号 0..n-1）；kind 1 = OP_MULTI_DRAW（max = n）
    static const struct { int kind, n; uint8_t unread; int draws, copies, passes; uint8_t need; const char *what; } cases[] = {
        { 0, 1, 0, 1, 2, 1, 3, "group of 1, both stages read: drawn, vs + fs each copy the record's firstInstance" },
        { 0, 1, 1, 1, 1, 1, 2, "group of 1, vs proven unread: only fs copies" },
        { 0, 1, 2, 1, 1, 1, 1, "group of 1, fs proven unread: only vs copies" },
        { 0, 1, 3, 1, 0, 1, 0, "group of 1, both proven unread: drawn, no copy" },
        { 0, 2, 0, 0, 0, 0, 0, "group of 2, both stages read: skipped by name (the copy only carries the first command's firstInstance)" },
        { 0, 3, 0, 0, 0, 0, 0, "group of 3, both stages read: skipped by name" },
        { 0, 3, 1, 0, 0, 0, 0, "group of 3, fs still reads: skipped by name" },
        { 0, 2, 3, 1, 0, 1, 0, "group of 2, both proven unread: drawn with max_draws 2, no copy" },
        { 0, 3, 3, 1, 0, 1, 0, "group of 3, both proven unread: drawn with max_draws 3, no copy" },
        { 1, 2, 0, 1, 2, 1, 3, "OP_MULTI_DRAW of 2, both read: unchanged original path, drawn, copies the first record's firstInstance" },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        rp_draw_reset();
        for (int st = 0; st < 2; st++) { g_fprog.nent[st] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[st][0]; memset(e, 0, sizeof *e); e->src_reg2 = UINT16_MAX; e->cls = NVN_GPU_RES_UBO; e->src_kind = NVN_RB_DRV0; e->count = 1; }
        g_fprog.binst_unread = cases[k].unread; g_arena_used = 0;
        int c0 = g_ncopy4, p0 = g_npd, d0 = g_gdraws; g_nc4 = g_ndd = 0; g_dd_on = 1;
        uint64_t sk0 = nvn_unimpl_count("draw", "indirect_count_driver_base_instance"), br0 = nvn_unimpl_count("draw", "indirect_count_group_broken");
        stream s = {0}; mdi_prelude(&s, &rtt);
        if (cases[k].kind == 0) {
            for (int i = 0; i < cases[k].n; i++) {
                p_draw_idx *di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->instances = 1; di->ibuf = base + 3 * PG;
                di->ind = ind + (uint64_t)i * 20; di->mdi_cnt = cnt; di->mdi_i = i;
            }
        } else {
            p_multi_draw *md = put(&s, OP_MULTI_DRAW, sizeof *md);
            md->ind = ind; md->cnt = cnt; md->ibuf = base + 3 * PG; md->stride = 20; md->max = cases[k].n; md->prim = 4; md->itype = 1;
        }
        end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK); g_dd_on = 0;
        const uint64_t skipped = nvn_unimpl_count("draw", "indirect_count_driver_base_instance") - sk0;
        if (g_gdraws - d0 != cases[k].draws || g_ncopy4 - c0 != cases[k].copies || g_npd - p0 != cases[k].passes || skipped != (cases[k].draws ? 0u : 1u)
            || nvn_unimpl_count("draw", "indirect_count_group_broken") != br0 || (cases[k].draws && g_last_da.max_draws != (uint32_t)cases[k].n)) {
            printf("  %s: draws %d copies %d passes %d named skips %llu max_draws %u (expect %d / %d / %d / %d / %d)\n", cases[k].what, g_gdraws - d0, g_ncopy4 - c0, g_npd - p0,
                   (unsigned long long)skipped, g_last_da.max_draws, cases[k].draws, cases[k].copies, cases[k].passes, cases[k].draws ? 0 : 1, cases[k].n);
            CHECK(0);
        }
        const uint64_t ro[1] = {4 * PG}; const int md0[1] = {0};
        if (!binst_slices_ok(cases[k].draws, cases[k].need, ro, md0, cases[k].what)) CHECK(0);
    }
    g_flag_binst_hoist = -1; rp_draw_reset();
    g_fprog = saved;
}

extern int rp_split_draws;
static void t_clean_read_order(void) {
    const int old_split = rp_split_draws;
    for (int mode = 0; mode < 2; mode++) for (int split = 0; split < 2; split++) {
        pass_fresh(); g_pg_tracked = 1; g_buf_cpu_auto = 1; g_flag_buf_dirtylog = mode == 1;
        g_log_fake_on = mode == 1; g_log_fake_bad = 0; g_log_head = 0; g_log_ident++; memset(g_log_unarmed, 0, sizeof g_log_unarmed);
        g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
        memset(g_block_mem, 0x11, BLOCK_TEST_PAGES * PG); memset(g_block_mem + 100 * PG, 0x22, PG);
        memset(g_block_gen, 0, sizeof g_block_gen);
        g_dirty_now = 100; ++g_dirty_map; g_block_unreliable = 0; g_nbw = 0; rp_split_draws = 0;
        uint64_t base = (uint64_t)(uintptr_t)g_block_mem; nvn_be_pool_add(g_block_mem, BLOCK_TEST_PAGES * PG);
        nvn_be_sub_begin(); stream warm = {0}; q_env(&warm); g_fprog.attr_mask = 1;
        p_bind_vattr *va = put(&warm, OP_BIND_VATTR, sizeof *va); va->n = 1; va_st at = {.fmt = 0x2a, .stream = 0}; memcpy(&va->st[0], &at, 4);
        p_bind_vstream *vs = put(&warm, OP_BIND_VSTREAM, sizeof *vs); vs->n = 1; vs->st[0].stride = 16;
        p_bind_vbuf_single *vb = put(&warm, OP_BIND_VBUF, sizeof *vb); vb->first = 0; vb->size = 65 * PG; vb->addr = base - 0x1000;
        q_draw(&warm); end(&warm); nvn_be_exec(warm.b); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        g_order_active = 1; g_order_seg = g_order_n = g_order_overflow = 0; g_order_vbuf = NULL; rp_split_draws = split ? 1 : 0;
        nvn_be_sub_begin(); stream first = {0}; q_draw(&first); end(&first); nvn_be_exec(first.b);
        if (split) { tex_t rt = mk_tex(701, 0x4000, NVN_FORMAT_RGBA8, 64, 36, 1, 0); stream cut = {0}; put_set_rt(&cut, &rt, NULL); end(&cut); nvn_be_exec(cut.b); }
        // 脏页替身按真实 fault 顺序：先记事件 / 页代号，再执行 COPY_B2B 写目标。
        fake_log_event(0, 0); g_block_gen[0] = ++g_dirty_now;
        stream copy = {0}; uint8_t *cp = put(&copy, OP_COPY_B2B, 24); uint64_t src = base + 100 * PG, dst = base, size = 4;
        memcpy(cp, &src, 8); memcpy(cp + 8, &dst, 8); memcpy(cp + 16, &size, 8); end(&copy); nvn_be_exec(copy.b);
        stream second = {0}; q_draw(&second); end(&second); nvn_be_exec(second.b); nvn_be_sub_commit();
        uint8_t got[2] = {0, 0}; int ndraw = 0;
        for (int seg = 1; seg <= g_order_seg; seg++) for (int pre = 1; pre >= 0; pre--) for (int i = 0; i < g_order_n; i++) {
            if (g_order_ev[i].seg != seg || g_order_ev[i].pre != pre) continue;
            nvn_gpu_buf b = g_order_ev[i].buf; uint64_t off = g_order_ev[i].off;
            if (!b || !b->cpu || off >= b->bytes) { g_order_overflow++; continue; }
            if (g_order_ev[i].draw) { if (ndraw < 2) got[ndraw] = b->cpu[off]; ndraw++; }
            else if (g_order_ev[i].len <= b->bytes - off) memcpy(b->cpu + off, g_order_ev[i].data, (size_t)g_order_ev[i].len);
            else g_order_overflow++;
        }
        printf("C1581E mode=%d split=%d GPU draw bytes=%02x/%02x events=%d seg=%d errors=%d\n", mode, split, got[0], got[1], g_order_n, g_order_seg, g_order_overflow);
        CHECK(ndraw == 2 && got[0] == 0x11 && got[1] == 0x22 && g_order_overflow == 0);
        CHECK(!split || g_order_seg >= 2);
        g_order_active = 0; fake_complete(NVN_GPU_DONE_OK); nvn_be_pool_del(g_block_mem);
        g_block_mem = NULL; g_flag_buf_dirtylog = g_dirtylog_dflt; g_log_fake_on = 0; g_buf_cpu_auto = 0; pass_fresh();
    }
    rp_split_draws = old_split;
}

// 区间不相交、提交结束和桶碰撞都不能改变读写语义；传输替身检查命令段与快照。
static void t_clean_read_scopes(void) {
    pass_fresh(); g_pg_tracked = 1; g_buf_cpu_auto = 1; g_flag_buf_dirtylog = 0;
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    memset(g_block_mem, 0x11, BLOCK_TEST_PAGES * PG); memset(g_block_gen, 0, sizeof g_block_gen);
    g_dirty_now = 100; ++g_dirty_map; g_block_unreliable = 0;
    uint64_t base = (uint64_t)(uintptr_t)g_block_mem; nvn_be_pool_add(g_block_mem, BLOCK_TEST_PAGES * PG);
    nvn_gpu_buf b = NULL; nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, 8 * PG, &b, NULL) && b && b->cpu);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL));
    fake_log_write(1, 0x77); g_nbw = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG, PG, &b, NULL));
    CHECK(g_nbw == 0 && b && b->cpu[PG] == 0x77);   // 与已读页不相交，仍可直接同步
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_sub_begin(); fake_log_write(0, 0x33); g_nbw = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL));
    CHECK(g_nbw == 0 && b && b->cpu[0] == 0x33);   // 之前的提交已完成，新提交第一次使用可直写
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL));
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 6 * PG, PG, &b, NULL));   // 这两页在当前读区间表里碰撞
    fake_log_write(0, 0x44); fake_log_write(6, 0x55); g_nbw = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, base, PG, &b, NULL));
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 6 * PG, PG, &b, NULL));
    printf("C1581E scopes: collision writes=%d first=%llu/%02x second=%llu/%02x\n", g_nbw,
           (unsigned long long)g_bw[0].off, g_bw[0].head[0], (unsigned long long)g_bw[1].off, g_bw[1].head[0]);
    CHECK(g_nbw == 2 && g_bw[0].off == 0 && g_bw[0].head[0] == 0x44 && !g_bw[0].pre &&
          g_bw[1].off == 6 * PG && g_bw[1].head[0] == 0x55 && !g_bw[1].pre);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_pool_del(g_block_mem);
    g_block_mem = NULL; g_buf_cpu_auto = 0; g_flag_buf_dirtylog = g_dirtylog_dflt; pass_fresh();
}

static void mirror_draw_at(stream *s, uint64_t addr) {
    p_bind_vbuf_single *vb = put(s, OP_BIND_VBUF, sizeof *vb);
    vb->first = 0; vb->size = 64; vb->addr = addr - 0x1000;
    q_draw(s);
}

static void t_mirror_disjoint(void) {
    enum { DEFAULT_OFF, GAP, OVERLAP, PENDING, UNKNOWN, ALIAS, OVERFLOW, REMAP, PRE_OOM, NCASE };
    static const char *const names[NCASE] = {"default", "gap", "overlap", "pending", "unknown", "alias", "overflow", "remap", "pre-oom"};
    const int old_split = rp_split_draws;
    for (int log = 0; log <= (g_dirtylog_dflt != 0); log++) for (int kind = DEFAULT_OFF; kind < NCASE; kind++) {
        pass_fresh(); g_pg_tracked = 1; g_buf_cpu_auto = 1; g_flag_buf_dirtylog = log;
        g_log_fake_on = log; g_log_fake_bad = 0; g_log_head = 0; g_log_ident++;
        memset(g_log_unarmed, 0, sizeof g_log_unarmed);
        g_flag_mirror_disjoint = kind == DEFAULT_OFF ? -1 : 1; rp_split_draws = 0;
        g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
        memset(g_block_mem, 0x11, BLOCK_TEST_PAGES * PG); memset(g_block_gen, 0, sizeof g_block_gen);
        g_dirty_now = 100; ++g_dirty_map; g_block_unreliable = 0;
        const uint64_t base = (uint64_t)(uintptr_t)g_block_mem;
        const uint64_t target = base + 3 * PG;
        nvn_be_pool_add(g_block_mem, BLOCK_TEST_PAGES * PG);
        // The small pool overlaps the upload, but cannot satisfy the draw's entire binding.
        if (kind == ALIAS) nvn_be_pool_add(g_block_mem + 3 * PG + 160, 16);
        nvn_gpu_buf b = NULL;
        nvn_be_sub_begin(); CHECK(rp_buf_resolve(&g_fake_cmd, base, 8 * PG, &b, NULL) && b && b->cpu);
        stream warm = {0}; q_env(&warm); q_draw(&warm); end(&warm); nvn_be_exec(warm.b);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);

        g_order_active = 1; g_order_seg = g_order_n = g_order_overflow = 0; g_order_vbuf = NULL;
        nvn_be_sub_begin(); stream first = {0}; q_env(&first); g_fprog.attr_mask = 1;
        p_bind_vattr *va = put(&first, OP_BIND_VATTR, sizeof *va);
        va->n = 1; va_st at = {.fmt = 0x2a, .stream = 0}; memcpy(&va->st[0], &at, 4);
        p_bind_vstream *vs = put(&first, OP_BIND_VSTREAM, sizeof *vs); vs->n = 1; vs->st[0].stride = 16;
        mirror_draw_at(&first, base);
        mirror_draw_at(&first, base + 6 * PG);   // 页桶里的包络覆盖第 3 页，实际两段读取都不接触它
        if (kind == OVERLAP) mirror_draw_at(&first, target);   // 同一页的另一段，仍被随整页上传覆盖
        end(&first); nvn_be_exec(first.b);
        CHECK(rp_pass_is_open());
        uint64_t key = 0, pending = 0;
        if (kind == PENDING) {
            pending = rp_buf_gpu_write_note(base + 5 * PG, 16, &key);
            CHECK(pending != 0);
        }
        if (kind == UNKNOWN) CHECK(!rp_buf_resolve(&g_fake_cmd, 1, 16, &b, NULL));
        if (kind == OVERFLOW) {
            for (int i = 0; i < 80; i++) CHECK(rp_buf_resolve(&g_fake_cmd, base + PG + 2 * (uint64_t)i, 1, &b, NULL));
        }
        if (kind == REMAP) ++g_dirty_map;
        g_pre_fail_once = kind == PRE_OOM;
        fake_log_write(3, 0x22);
        g_nbw = 0;
        uint64_t perf[RP_PERF_N]; rp_perf_take(perf);
        const int before = g_npd;
        stream next = {0}; mirror_draw_at(&next, target + 128); end(&next); nvn_be_exec(next.b);
        rp_perf_take(perf);
        const int passes = g_npd - before;
        const int pre = g_nbw == 1 && g_bw[0].pre;
        CHECK(g_nbw == 1 && g_bw[0].off == 3 * PG && g_bw[0].len == PG);
        CHECK(perf[RP_PERF_MIRROR_HOIST] == (kind == GAP));
        CHECK(perf[RP_PERF_MIRROR_OVERLAP] == (kind == OVERLAP));
        CHECK(perf[RP_PERF_MIRROR_FALLBACK] == (kind >= PENDING));
        CHECK(perf[RP_PERF_P_CUT_XFER] == (kind != GAP));
        if (kind == GAP) CHECK((perf[RP_PERF_BUF_LOG_EV] != 0) == log);
        CHECK(!g_pre_fail_once); g_pre_fail_once = 0;
        nvn_be_sub_commit();
        int draws = 0;
        uint8_t observed[4] = {0};
        for (int pre_pass = 1; pre_pass >= 0; pre_pass--) for (int i = 0; i < g_order_n; i++) {
            if (g_order_ev[i].pre != pre_pass) continue;
            nvn_gpu_buf buf = g_order_ev[i].buf;
            const uint64_t off = g_order_ev[i].off;
            if (!buf || !buf->cpu || off >= buf->bytes) { g_order_overflow++; continue; }
            if (g_order_ev[i].draw) {
                if (draws < 4) observed[draws] = buf->cpu[off];
                draws++;
            } else if (g_order_ev[i].len <= buf->bytes - off) {
                memcpy(buf->cpu + off, g_order_ev[i].data, (size_t)g_order_ev[i].len);
            } else g_order_overflow++;
        }
        const int overlap = kind == OVERLAP;
        printf("mirror log=%d %s: pre=%d reopened=%d draw bytes=%02x/%02x/%02x/%02x\n",
               log, names[kind], pre, passes, observed[0], observed[1], observed[2], observed[3]);
        CHECK(pre == (kind == GAP) && passes == (kind != GAP));
        CHECK(g_order_seg == 1 && g_order_overflow == 0 && draws == 3 + overlap);
        CHECK(observed[0] == 0x11 && observed[1] == 0x11 && observed[2 + overlap] == 0x22);
        CHECK(!overlap || observed[2] == 0x11);
        g_order_active = 0; fake_complete(NVN_GPU_DONE_OK);
        if (pending) rp_buf_gpu_write_land(key, pending, 0);   // 用例只登记在途状态，收尾撤掉
        if (kind == ALIAS) nvn_be_pool_del(g_block_mem + 3 * PG + 160);
        nvn_be_pool_del(g_block_mem); g_block_mem = NULL; pass_fresh();
    }
    g_buf_cpu_auto = 0; g_log_fake_on = 0; g_flag_buf_dirtylog = g_dirtylog_dflt; g_flag_mirror_disjoint = -1;
    rp_split_draws = old_split;
}

static void t_draw(void) {   // 绘制：状态在绘制时才变成后端的管线 / 动态状态 / 顶点流 / 绑定；没变的管线不重取；取不到的资源跳过并记账
    pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; memset(g_pmem, 0, sizeof g_pmem); g_arena_used = 0;
    g_pipe_gets = g_set_pipes = g_set_dyns = g_gdraws = g_nvb = g_resolves = g_resolve_rc = 0; memset(g_nfb, 0, sizeof g_nfb);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(&g_fprog, 0, sizeof g_fprog); for (int st = 0; st < RP_PROG_STAGES; ++st) for (int i = 0; i < RP_PROG_ENT_MAX; ++i) g_fprog.ent[st][i].src_reg2 = UINT16_MAX; g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.support_bytes = 2608; g_fprog.attr_mask = 1; g_fprog.color_out_mask = 0xffffffffu;
    g_fprog.nent[0] = 2; g_fprog.ent[0][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][0].src_kind = NVN_RB_SUPPORT;
    g_fprog.ent[0][1].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][1].src_kind = NVN_RB_CBUF; g_fprog.ent[0][1].src_reg = 2;
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
    tex_t rtt = mk_tex(81, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0), tx = mk_tex(82, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_tex(7, &tx, NULL); nvn_be_reg_samp(3, &sp);
    for (int i = 0; i < 256; i++) g_pmem[PG + 128 + i] = (uint8_t)(i ^ 0x5a);
    g_nbw = 0;
    stream s = {0}; put_set_rt(&s, &rtt, NULL);
    // 属性 1 游戏设了格式、挂在流 2，但着色器不读（attr_mask = 1）：它和流 2 都不该进管线描述（管线键不烘不读的属性）
    p_bind_vattr *va = put(&s, OP_BIND_VATTR, sizeof *va); va->n = 2; va_st a0 = {.fmt = 0x2A, .offset = 0, .stream = 1}, a1 = {.fmt = 0x2A, .offset = 4, .stream = 2}; memcpy(&va->st[0], &a0, 4); memcpy(&va->st[1], &a1, 4);
    p_bind_vstream *vs = put(&s, OP_BIND_VSTREAM, sizeof *vs); vs->n = 3; vs->st[1].stride = 16; vs->st[2].stride = 32;
    p_bind_vbuf_single *vb = put(&s, OP_BIND_VBUF, sizeof *vb); vb->first = 1; vb->size = 64; vb->addr = base + 2 * PG + 32 - 0x1000;   // 记录里是 GPU 地址（替身的换算是 +0x1000）
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 128; ub->size = 256;
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    p_viewports *vp = put(&s, OP_VIEWPORTS, sizeof *vp); vp->n = 1; vp->v[0][2] = 64; vp->v[0][3] = 36;
    p_draw_idx *di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 6; di->base_vertex = 3; di->instances = 0; di->ibuf = base + 3 * PG + 8;
    di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG + 8;   // 第二笔：什么都没变
    di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 0xa; di->itype = 1; di->count = 4; di->ibuf = base + 3 * PG;      // 邻接线：设备没开 geometryShader（替身缺省）→ 具名跳过
    p_draw *dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 5; dn->first = 2; dn->count = 4; dn->instances = 3;            // 不带索引的条带
    end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_npd == 1 && pd_zero_clear(&g_pd[0]));   // 三笔画在同一个通道里；没有清除 → 不带清除值（刚建的目标载入时清成零）
    CHECK(nvn_unimpl_count("draw", "adjacency_without_geometry_feature") == 1 && nvn_unimpl_count("draw", "prim_10") == 0);
    CHECK(g_resolves == 2 && g_pipe_gets == 2);          // 第一笔取一次；第二笔没变不取；条带换了图元再取一次
    CHECK(g_last_req.prim == 5 && g_last_req.rt_count == 1 && g_last_req.rt_format[0] == NVN_FORMAT_RGBA8);
    CHECK(g_last_pdesc.st[0].sh == &g_fsh[0] && g_last_pdesc.st[1].sh == &g_fsh[1] && g_last_pdesc.cfmt[0] == NVN_FORMAT_RGBA8 && g_last_pdesc.dfmt == 0);
    CHECK(g_last_pdesc.attr[0].fmt == 0x2A && g_last_pdesc.attr[0].stream == 1 && g_last_pdesc.stream[1].stride == 16);
    CHECK(g_last_pdesc.attr[1].fmt == 0 && g_last_pdesc.attr[1].stream == 0 && g_last_pdesc.stream[2].stride == 0);   // 不读的属性与它独占的流不进管线
    { blend_st z3; memset(&z3, 0, sizeof z3); CHECK(g_last_pdesc.chmask.mask == 0xf && !memcmp(&g_last_pdesc.blend[3], &z3, sizeof z3)); }   // 只有槽 0 有图：其余槽的写掩码与混合状态清掉
    CHECK(g_last_dyn.nvp == 1 && g_last_dyn.vp[0][2] == 64 && g_last_dyn.vp[0][5] == 1 && g_last_dyn.origin_ll == 1 && g_last_dyn.st_vmask[0] == 0xff);
    CHECK(g_nvb == 3 && g_vb[0].stream == 1 && g_vb[0].off == 2 * PG + 32);              // 顶点缓冲地址做了 GPU → CPU 换算
    CHECK(g_nbw == 2 && g_bw[0].pre == 1 && g_bw[1].pre == 1 && g_pass_ends == 1);       // 顶点页、索引页各同步一次，都在前置段；通道没被打断
    CHECK(g_nfb[0] == 2 && g_fb[0][0].u.b.buf == &g_arena_buf && g_fb[0][0].u.b.len == 2608);
    CHECK(g_fb[0][1].ent == 1 && g_fb[0][1].u.b.buf == &g_arena_buf && g_fb[0][1].u.b.len == 256 && !memcmp(g_arena + g_fb[0][1].u.b.off, g_pmem + PG + 128, 256));   // 用户常量缓冲 2：编码这一刻的内容拷进暂存区
    CHECK(g_nfb[1] == 1 && g_fb[1][0].u.ts.view != NULL && g_fb[1][0].u.ts.samp != NULL);
    uint32_t flags; memcpy(&flags, g_arena + g_fb[0][0].u.b.off + 1392, 4); CHECK(flags == 3);   // 深度 −1..1（NVN 默认）+ 内建绘制参数
    CHECK(g_last_da.prim == 5 && g_last_da.first == 2 && g_last_da.count == 4 && g_last_da.instances == 3 && g_last_da.ibuf == NULL);
    // 取不到资源的三种情况：各自跳过并记账，不发半截绘制
    int d0 = g_gdraws;
    stream u = {0}; ub = put(&u, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2;   // 解绑常量缓冲
    di = put(&u, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&u);
    submit1(&u); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 && nvn_unimpl_count("draw", "uniform_buffer_unbound") == 1);
    stream w = {0}; ub = put(&w, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 100; ub->size = 64;   // 偏移 100 不合 64 对齐：拷进暂存区
    di = put(&w, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; di->base_instance = 9; end(&w);
    submit1(&w); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 1 && g_fb[0][1].u.b.buf == &g_arena_buf && g_fb[0][1].u.b.len == 64 && g_last_da.base_instance == 9 && g_last_da.ioff == 3 * PG && g_last_da.itype == 1);
    // 深度偏移：开关进管线描述，三个量进动态状态；点 / 线成功不误记账，明确不支持时记账并跳过
    g_fprog.color_out_mask = 0x5;
    stream y = {0}; p_bind_state *by = put(&y, OP_BIND_STATE, sizeof *by); by->kind = 4; poly_st py = {NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, NVN_POLYGON_OFFSET_ENABLE_FILL}; memcpy(by->bytes, &py, sizeof py);
    p_poly_offset *pf = put(&y, OP_POLY_OFFSET, sizeof *pf); pf->factor = 1.5f; pf->units = 2; pf->clamp = 0.25f;
    di = put(&y, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&y);
    submit1(&y); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_last_pdesc.chmask.mask == 0x5 && g_last_pdesc.color.blend_en == 0);   // 着色器只写了目标 0 的红、蓝：写掩码与输出映射相与；见下面把 color_out_mask 设成 0x5
    g_fprog.color_out_mask = 0xffffffffu;
    CHECK(g_gdraws == d0 + 2 && g_last_pdesc.poly.offset_en == NVN_POLYGON_OFFSET_ENABLE_FILL && g_last_dyn.po.factor == 1.5f && g_last_dyn.po.units == 2 && g_last_dyn.po.clamp == 0.25f);
    CHECK(nvn_unimpl_count("gpu", "polygon_mode_not_fill") == 0);
    stream y2 = {0}; by = put(&y2, OP_BIND_STATE, sizeof *by); by->kind = 4; py.mode = NVN_POLYGON_MODE_LINE; py.offset_en = NVN_POLYGON_OFFSET_ENABLE_LINE; memcpy(by->bytes, &py, sizeof py);
    di = put(&y2, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&y2);
    submit1(&y2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 3 && g_last_pdesc.poly.mode == NVN_POLYGON_MODE_LINE && g_last_pdesc.poly.offset_en == NVN_POLYGON_OFFSET_ENABLE_LINE && nvn_unimpl_count("gpu", "polygon_mode_not_fill") == 0);
    stream yp = {0}; by = put(&yp, OP_BIND_STATE, sizeof *by); by->kind = 4; py.mode = NVN_POLYGON_MODE_POINT; py.offset_en = NVN_POLYGON_OFFSET_ENABLE_POINT; memcpy(by->bytes, &py, sizeof py);
    di = put(&yp, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&yp);
    submit1(&yp); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_last_pdesc.poly.mode == NVN_POLYGON_MODE_POINT && g_last_pdesc.poly.offset_en == NVN_POLYGON_OFFSET_ENABLE_POINT && g_gdraws == d0 + 4 && nvn_unimpl_count("gpu", "polygon_mode_not_fill") == 0);
    g_pipe_rc = NVN_GPU_E_UNSUPPORTED; submit1(&y2); fake_complete(NVN_GPU_DONE_OK); g_pipe_rc = 0;
    CHECK(g_gdraws == d0 + 4 && nvn_unimpl_count("gpu", "polygon_mode_not_fill") == 1);
    // 四边形：展开成三角形列表的 32 位索引（每个四边形 0,1,2 / 0,2,3），按带索引的三角形画
    { py.mode = NVN_POLYGON_MODE_FILL; stream z = {0}; by = put(&z, OP_BIND_STATE, sizeof *by); by->kind = 4; memcpy(by->bytes, &py, sizeof py);
      dn = put(&z, OP_DRAW, sizeof *dn); dn->prim = 7; dn->first = 10; dn->count = 9;                   // 不带索引、9 个顶点：两个四边形，零头 1 个丢掉
      uint16_t qi[9] = {5, 6, 7, 8, 0xffff, 20, 21, 22, 23}; memcpy(g_pmem + 3 * PG + 512, qi, sizeof qi);
      p_prim_restart *pr = put(&z, OP_PRIM_RESTART, sizeof *pr); pr->on = 1; pr->index = 0xffff;
      di = put(&z, OP_DRAW_IDX, sizeof *di); di->prim = 7; di->itype = 1; di->count = 9; di->base_vertex = 2; di->ibuf = base + 3 * PG + 512;   // 带索引 + 重启：重启前后各一个四边形
      pr = put(&z, OP_PRIM_RESTART, sizeof *pr); pr->on = 0;
      dn = put(&z, OP_DRAW, sizeof *dn); dn->prim = 7; dn->count = 3; end(&z);                         // 不足一个四边形：不画
      int g0 = g_gdraws; nvn_be_sub_begin(); const uint8_t *zp = z.b;
      // 逐条回放，好在每笔之后核暂存区里的索引
      op_hdr zh; memcpy(&zh, zp, 8); size_t off1 = zh.size; memcpy(&zh, zp + off1, 8); size_t off2 = off1 + zh.size;   // 状态、第一笔
      uint8_t save[16]; memcpy(save, z.b + off2, 8); op_hdr eh = {OP_END, 8}; memcpy(z.b + off2, &eh, 8);
      nvn_be_exec(z.b);
      CHECK(g_gdraws == g0 + 1 && g_last_da.prim == 4 && g_last_da.count == 12 && g_last_da.itype == 2 && g_last_da.base_vertex == 10 && g_last_da.ibuf == &g_arena_buf && g_last_pdesc.prim == 4);
      { const uint32_t exp1[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7}; CHECK(!memcmp(g_arena + g_last_da.ioff, exp1, sizeof exp1)); }
      memcpy(z.b + off2, save, 8); nvn_be_exec(z.b + off2);
      CHECK(g_gdraws == g0 + 2 && g_last_da.count == 12 && g_last_da.itype == 2 && g_last_da.base_vertex == 2 && g_last_pdesc.prim_restart == 0);
      { const uint32_t exp2[12] = {5, 6, 7, 5, 7, 8, 20, 21, 22, 20, 22, 23}; CHECK(!memcmp(g_arena + g_last_da.ioff, exp2, sizeof exp2)); }
      nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); }
    // #977 重启值不是全 1：带索引的直接条带绘制改写成 32 位索引（等于重启值的 → 0xFFFFFFFF，原来的全 1 保持原值、不再触发重启）。
    // 期望序列按定义手算，不跑被测函数。
    { int g0 = g_gdraws; stream q = {0}; p_prim_restart *pr;
      uint16_t ri[7] = {0, 1, 2, 0x1234, 3, 4, 5}; memcpy(g_pmem + 3 * PG + 1536, ri, sizeof ri);
      uint16_t rk[8] = {0, 1, 2, 0x1234, 3, 4, 0xffff, 5}; memcpy(g_pmem + 3 * PG + 1600, rk, sizeof rk);
      // ① 重启值 0x1234、u16 三角带
      pr = put(&q, OP_PRIM_RESTART, sizeof *pr); pr->on = 1; pr->index = 0x1234;
      di = put(&q, OP_DRAW_IDX, sizeof *di); di->prim = 5; di->itype = 1; di->count = 7; di->base_vertex = 3; di->ibuf = base + 3 * PG + 1536; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 1 && g_last_da.prim == 5 && g_last_pdesc.prim_restart == 1 && g_last_da.itype == 2 && g_last_da.ibuf == &g_arena_buf && g_last_da.base_vertex == 3 && g_last_da.count == 7);
      { const uint32_t e[7] = {0, 1, 2, 0xFFFFFFFFu, 3, 4, 5}; CHECK(g_last_da.itype == 2 && g_last_da.count == 7 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); }
      // ② 同一重启值，索引里本来有 0xFFFF：保持原值（u32 的 0x0000FFFF，不再当重启）
      q.n = 0; di = put(&q, OP_DRAW_IDX, sizeof *di); di->prim = 5; di->itype = 1; di->count = 8; di->ibuf = base + 3 * PG + 1600; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      { const uint32_t e[8] = {0, 1, 2, 0xFFFFFFFFu, 3, 4, 0xFFFFu, 5}; CHECK(g_gdraws == g0 + 2 && g_last_da.itype == 2 && g_last_da.count == 8 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); }
      // ③ 间接绘制碰到这种重启值：具名记账、不画
      q.n = 0; di = put(&q, OP_DRAW_IDX, sizeof *di); di->prim = 5; di->itype = 1; di->ibuf = base + 3 * PG + 1536; di->ind = base + 3 * PG + 2048; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 2 && nvn_unimpl_count("draw", "indirect_restart_index_not_all_ones") == 1);
      // ④ 负对照：重启值本来就是全 1（u16 0xFFFF）不改写，索引与重启原样交给后端
      q.n = 0; pr = put(&q, OP_PRIM_RESTART, sizeof *pr); pr->on = 1; pr->index = 0xffff;
      di = put(&q, OP_DRAW_IDX, sizeof *di); di->prim = 5; di->itype = 1; di->count = 8; di->ibuf = base + 3 * PG + 1600; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 3 && g_last_da.itype == 1 && g_last_da.ibuf != &g_arena_buf && g_last_da.ioff == 3 * PG + 1600 && g_last_da.count == 8 && g_last_pdesc.prim_restart == 1);
      q.n = 0; pr = put(&q, OP_PRIM_RESTART, sizeof *pr); pr->on = 0; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK); }
    // 线环（2）、四边形带（8）、多边形（9）（#710）：期望的图元号与索引序列都是按定义手算的，不跑被测的展开函数
    { int g0 = g_gdraws; stream q = {0}; p_prim_restart *pr;
      // ① 线环、不带索引、3 个点：线段列表 (0,1)(1,2)(2,0)，首点进 base vertex
      dn = put(&q, OP_DRAW, sizeof *dn); dn->prim = 2; dn->first = 4; dn->count = 3; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 1 && g_last_da.prim == 1 && g_last_pdesc.prim == 1 && g_last_pdesc.prim_restart == 0 && g_last_da.itype == 2 && g_last_da.ibuf == &g_arena_buf && g_last_da.base_vertex == 4 && g_last_da.count == 6);
      { const uint32_t e[6] = {0, 1, 1, 2, 2, 0}; CHECK(g_last_da.count == 6 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); }
      // ② 线环、带索引 + 重启：三段 [3,4,5] [7] [9,10]——第一段闭合成 3 条线，单点段不画，两点段来回各一条
      uint16_t li[8] = {3, 4, 5, 0xffff, 7, 0xffff, 9, 10}; memcpy(g_pmem + 3 * PG + 1024, li, sizeof li);
      q.n = 0; pr = put(&q, OP_PRIM_RESTART, sizeof *pr); pr->on = 1; pr->index = 0xffff;
      di = put(&q, OP_DRAW_IDX, sizeof *di); di->prim = 2; di->itype = 1; di->count = 8; di->base_vertex = 1; di->ibuf = base + 3 * PG + 1024; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 2 && g_last_da.prim == 1 && g_last_pdesc.prim_restart == 0 && g_last_da.itype == 2 && g_last_da.base_vertex == 1);
      { const uint32_t e[10] = {3, 4, 4, 5, 5, 3, 9, 10, 10, 9}; CHECK(g_last_da.count == 10 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); }
      // ③ 四边形带、带索引 + 重启：两段 [0..5]（两个四边形）与 [10..14]（一个四边形，零头 1 个丢掉），按三角带的顶点序展开成三角形列表
      uint16_t si[12] = {0, 1, 2, 3, 4, 5, 0xffff, 10, 11, 12, 13, 14}; memcpy(g_pmem + 3 * PG + 1280, si, sizeof si);
      q.n = 0; di = put(&q, OP_DRAW_IDX, sizeof *di); di->prim = 8; di->itype = 1; di->count = 12; di->ibuf = base + 3 * PG + 1280; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 3 && g_last_da.prim == 4 && g_last_pdesc.prim == 4 && g_last_pdesc.prim_restart == 0 && g_last_da.itype == 2);
      { const uint32_t e[18] = {0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4, 10, 11, 12, 11, 13, 12}; CHECK(g_last_da.count == 18 && !memcmp(g_arena + g_last_da.ioff, e, sizeof e)); }
      // ④ 多边形、带索引 + 重启：当三角扇画，索引与重启原样交给后端
      q.n = 0; di = put(&q, OP_DRAW_IDX, sizeof *di); di->prim = 9; di->itype = 1; di->count = 5; di->ibuf = base + 3 * PG + 1280; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 4 && g_last_da.prim == 6 && g_last_pdesc.prim == 6 && g_last_pdesc.prim_restart == 1 && g_last_da.itype == 1 && g_last_da.count == 5 && g_last_da.ioff == 3 * PG + 1280);
      // ⑤ 关掉重启：四边形带不带索引、7 个点 → 三角带 6 个点（零头 1 个丢掉，不多画一个三角形），不展开
      q.n = 0; pr = put(&q, OP_PRIM_RESTART, sizeof *pr); pr->on = 0;
      dn = put(&q, OP_DRAW, sizeof *dn); dn->prim = 8; dn->first = 2; dn->count = 7; dn->instances = 2; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 5 && g_last_da.prim == 5 && g_last_pdesc.prim == 5 && g_last_da.ibuf == NULL && g_last_da.first == 2 && g_last_da.count == 6 && g_last_da.instances == 2);
      // ⑥ 多边形不带索引：三角扇，首点与实例原样传
      q.n = 0; dn = put(&q, OP_DRAW, sizeof *dn); dn->prim = 9; dn->first = 3; dn->count = 5; dn->instances = 4; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 6 && g_last_da.prim == 6 && g_last_da.ibuf == NULL && g_last_da.first == 3 && g_last_da.count == 5 && g_last_da.instances == 4);
      // ⑦ 画不出东西的：四边形带不足 4 个点、线环只有 1 个点——不发绘制，也不记账
      q.n = 0; dn = put(&q, OP_DRAW, sizeof *dn); dn->prim = 8; dn->count = 3;
      dn = put(&q, OP_DRAW, sizeof *dn); dn->prim = 2; dn->count = 1; end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g0 + 6 && nvn_unimpl_count("draw", "prim_2") == 0 && nvn_unimpl_count("draw", "prim_8") == 0 && nvn_unimpl_count("draw", "prim_9") == 0); }
    // 贴图数组（着色器按运行时下标取句柄）：数组第 k 项 = 驱动句柄表第 k 个 8 字节项 = 贴图槽 k − 4；前 4 项与没绑的槽读作池里的第 0 项
    { tex_t tb = mk_tex(83, 0x8000, NVN_FORMAT_RGBA8, 4, 4, 1, 0); nvn_be_reg_tex(0, &tb, NULL); nvn_be_reg_samp(0, &sp);   // 池基号在这份用例里是 0：池的第 0 项 = 贴图 0 / 采样器 0
      g_fprog.ent[1][0].count = 10; g_fprog.ent[1][0].src_reg = 0;       // 片元阶段那一项改成从句柄表第 0 项起、10 项的数组
      stream ar = {0};
      bh = put(&ar, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 1; bh->n = 1; bh->h[0] = 7 | 3 << 20;          // 只绑贴图槽 1 = 贴图 7
      di = put(&ar, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&ar);
      int g1 = g_gdraws; submit1(&ar); fake_complete(NVN_GPU_DONE_OK);
      nvn_gpu_view v7 = rp_view_by_id(7, &g_fake_cmd, NULL), v9 = rp_view_by_id(0, &g_fake_cmd, NULL);
      CHECK(g_gdraws == g1 + 1 && g_nfb[1] == 10 && v7 && v9 && v7 != v9);
      // 槽 1（这里绑的）→ 第 5 项，槽 5（本用例开头绑的）→ 第 9 项，都是贴图 7；其余读作池里第 0 项
      for (uint32_t k = 0; k < 10 && g_nfb[1] == 10; k++) CHECK(g_fb[1][k].ent == 0 && g_fb[1][k].elem == k && g_fb[1][k].u.ts.samp != NULL && g_fb[1][k].u.ts.view == (k == 5 || k == 9 ? v7 : v9));
      g_fprog.ent[1][0].count = 0; g_fprog.ent[1][0].src_reg = 5; nvn_be_tex_release(&tb); }
    // 正面绕序：接口值 + 建队列时定的光栅翻转位 → y 朝上的规范化设备坐标里的绕序；窗口原点一并进管线描述
    { stream w1 = {0}; by = put(&w1, OP_BIND_STATE, sizeof *by); by->kind = 4; poly_st pw = {NVN_FACE_BACK, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; memcpy(by->bytes, &pw, sizeof pw);
      di = put(&w1, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&w1);
      submit1(&w1); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_last_pdesc.poly.front == NVN_FRONT_FACE_CCW && g_last_pdesc.poly.cull == NVN_FACE_BACK && g_last_pdesc.origin_ll == 1);   // 翻转与当前左下原点共同归一化：这里抵消
      int pg0 = g_pipe_gets; s2a_nvn_rast_flip = 0;                                  // 换一条翻转关着的队列：同样的状态，管线要重取
      stream w2 = {0}; di = put(&w2, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&w2);
      submit1(&w2); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_pipe_gets == pg0 + 1 && g_last_pdesc.poly.front == NVN_FRONT_FACE_CW);
      s2a_nvn_rast_flip = 1; }
    // 裁剪矩形夹到目标（64×36）内；整个在目标外的不发绘制；深度夹取进管线描述；「y 取反」的视口重排 = 视口上下翻
    { stream c1 = {0}; p_scissor *sc1 = put(&c1, OP_SCISSOR, sizeof *sc1); sc1->x = -5; sc1->y = -6; sc1->w = 20; sc1->h = 100;
      p_depth_clamp *dc = put(&c1, OP_DEPTH_CLAMP, sizeof *dc); dc->on = 1;
      p_vp_swizzle *vsw = put(&c1, OP_VP_SWIZZLE, sizeof *vsw); vsw->first = 0; vsw->n = 1; vsw->sw[0][1] = 1;      // 四个字节 0,1,0,0 = 0x100
      di = put(&c1, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&c1);
      int g2 = g_gdraws; submit1(&c1); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g2 + 1 && g_last_dyn.has_sc && g_last_dyn.sc[0] == 0 && g_last_dyn.sc[1] == 0 && g_last_dyn.sc[2] == 15 && g_last_dyn.sc[3] == 36);
      CHECK(g_last_pdesc.depth_clamp == 1 && g_last_dyn.depth_clamp == 1);
      CHECK(g_last_dyn.vp[0][1] == 36 && g_last_dyn.vp[0][3] == -36 && nvn_unimpl_count("gpu", "viewport_swizzle") == 0);   // 视口原是 0,0 64×36
      stream c2 = {0}; sc1 = put(&c2, OP_SCISSOR, sizeof *sc1); sc1->x = 64; sc1->y = 0; sc1->w = 10; sc1->h = 10;
      di = put(&c2, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG;
      sc1 = put(&c2, OP_SCISSOR, sizeof *sc1); sc1->x = 0; sc1->y = 0; sc1->w = 64; sc1->h = 36;
      dc = put(&c2, OP_DEPTH_CLAMP, sizeof *dc); dc->on = 0; vsw = put(&c2, OP_VP_SWIZZLE, sizeof *vsw); vsw->first = 0; vsw->n = 1; end(&c2);
      submit1(&c2); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_gdraws == g2 + 1); }
    g_resolve_rc = -1;
    stream x = {0}; p_bind_state *bs = put(&x, OP_BIND_STATE, sizeof *bs); bs->kind = 4; poly_st po = {NVN_FACE_FRONT, NVN_FRONT_FACE_CW, NVN_POLYGON_MODE_FILL, 0}; memcpy(bs->bytes, &po, sizeof po);
    di = put(&x, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&x);
    submit1(&x); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == d0 + 19);   // 新拓扑6笔 + 点模式1笔成功 + #977 重启值改写 3 笔；不支持的线模式不绘制
    uint64_t dr_n = 0, sk_n = 0; rp_draw_stats(&dr_n, &sk_n); CHECK(dr_n == 22 && sk_n == 5);   // #977 间接 + 非全 1 重启值跳过 1 笔
    g_resolve_rc = 0; t_draw_mdi(); t_draw_ind_binst(); t_draw_mdi_binst();   // 间接计数多绘：放在 t_draw 末尾、池和贴图还没释放时，沿用同一套绑定（它多画的几笔不扰动上面按笔数的断言）
    g_resolve_rc = 0; nvn_be_tex_release(&rtt); nvn_be_tex_release(&tx); nvn_be_pool_del(g_pmem); pass_fresh();
}

static void t_bind_reuse(void) {   // 按脏位少发绑定：没变的阶段不发；同一段里同一区间的常量缓冲只落一份暂存；回放时在 CPU 上改过缓冲内容就必须重取
    pass_fresh(); memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; memset(g_pmem, 0, sizeof g_pmem); g_arena_used = 0;
    g_gdraws = g_resolve_rc = 0; memset(g_nfb, 0, sizeof g_nfb);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_pool_add(g_pmem, sizeof g_pmem);
    memset(&g_fprog, 0, sizeof g_fprog); for (int st = 0; st < RP_PROG_STAGES; ++st) for (int i = 0; i < RP_PROG_ENT_MAX; ++i) g_fprog.ent[st][i].src_reg2 = UINT16_MAX; g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.support_bytes = 2608; g_fprog.attr_mask = 1; g_fprog.color_out_mask = 0xffffffffu;
    g_fprog.nent[0] = 2; g_fprog.ent[0][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][0].src_kind = NVN_RB_SUPPORT;
    g_fprog.ent[0][1].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][1].src_kind = NVN_RB_CBUF; g_fprog.ent[0][1].src_reg = 2;
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5;
    tex_t rtt = mk_tex(91, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0), tx = mk_tex(92, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_tex(7, &tx, NULL); nvn_be_reg_samp(3, &sp);
    memset(g_pmem + PG + 128, 0x11, 64); memset(g_pmem + PG + 512, 0x22, 64);   // 两块常量缓冲：甲全 0x11、乙全 0x22
    stream s = {0}; put_set_rt(&s, &rtt, NULL);
    p_bind_vattr *va = put(&s, OP_BIND_VATTR, sizeof *va); va->n = 1; va_st a0 = {.fmt = 0x2A, .offset = 0, .stream = 1}; memcpy(&va->st[0], &a0, 4);
    p_bind_vstream *vs = put(&s, OP_BIND_VSTREAM, sizeof *vs); vs->n = 2; vs->st[1].stride = 16;
    p_bind_vbuf_single *vb = put(&s, OP_BIND_VBUF, sizeof *vb); vb->first = 1; vb->size = 64; vb->addr = base + 2 * PG + 32 - 0x1000;
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 128; ub->size = 64;   // 甲
    p_draw *dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3;   // ① 首笔
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3;             // ② 什么都没变
    ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 512; ub->size = 64;   // 换成乙
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3;             // ③
    ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 128; ub->size = 64;   // 换回甲
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3;             // ④
    uint8_t *r = put(&s, OP_UPDATE_UB, 16 + 64); uint64_t d = base + PG + 128, n = 64; memcpy(r, &d, 8); memcpy(r + 8, &n, 8); memset(r + 16, 0x33, 64);   // 回放时把甲改成全 0x33
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3;             // ⑤ 绑定状态没动，内容变了
    end(&s);
    // 逐笔看：把整条流拆成五次看不到中间状态，所以在替身的绘制回调之外用「每笔之后的累计」推——这里按最终累计核，期望逐项手算：
    //   ① 顶点、片元各发 1 次（1,1）；② 都不发（1,1）；③ 顶点换了常量缓冲发 1 次，片元不发（2,1）；④ 同上（3,1）；⑤ 回放时改过缓冲内容：两个阶段都保守重取重发（4,2）——片元那组内容其实一样，由后端按「缓冲 + 偏移 + 长度」判相等。
    memset(g_bind_calls, 0, sizeof g_bind_calls); uint64_t a_before = g_arena_used;
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 5);
    CHECK(g_bind_calls[0] == 4 && g_bind_calls[1] == 2);
    // 暂存区：对齐 64。支持块 2608 → 占到 2624（41 个 64）；甲 64、乙 64、改后的甲 64。④ 回到甲时用的是 ① 那一片，不再落新的：共 2608 + 3 × 64，按 64 对齐后 2624 + 192 = 2816。
    CHECK(g_arena_used - a_before == 2816);
    { uint8_t want[64]; memset(want, 0x33, 64); CHECK(g_fb[0][1].u.b.len == 64 && !memcmp(g_arena + g_fb[0][1].u.b.off, want, 64)); }   // ⑤ 绑的是改过之后的内容
    // 换一次提交：上一段的暂存不能带过来，全部重取重发
    stream t = {0}; dn = put(&t, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; end(&t);
    memset(g_bind_calls, 0, sizeof g_bind_calls);
    submit1(&t); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 6 && g_bind_calls[0] == 1 && g_bind_calls[1] == 1);
}

// ADPF 帧负载上报接在新层出图点：出图前对本帧报 frame_end（目标 = 游戏周期），出图后对下一帧开 work_begin；没有显示面时关会话、不报。
//   期望（手算）：无显示面出图 → invisible 1 次、frame_end / work_begin 0 次；有显示面、周期 1/30 s：
//   第 1 次 → frame_end(33333333, 1)、work_begin(2)；第 2 次 → frame_end(33333333, 2)、work_begin(3)。
static double g_land_period;
void nvn_gpu_land_auto_present(uint64_t now, uint64_t acquire, uint64_t present, uint64_t draws, int target, double period_s) {
    (void)now; (void)acquire; (void)present; (void)draws; (void)target;
    g_land_period = period_s;
}
static void t_present_adpf(void) {
    pass_fresh(); rp_present_reset(); memset(g_acq_script, 0, sizeof g_acq_script); g_acq_i = 0;
    g_adpf_begin_n = g_adpf_end_n = g_adpf_invis_n = 0; g_adpf_begin_seq = g_adpf_end_seq = 0; g_adpf_end_target = 0;
    tex_t t = mk_tex(72, 0, NVN_FORMAT_RGBA8, 640, 360, 1, 0);
    rp_present_set_surface(NULL, 0);
    nvn_be_present(&t, 1.0 / 30, NULL);
    CHECK(g_adpf_invis_n == 1 && g_adpf_end_n == 0 && g_adpf_begin_n == 0);
    rp_present_set_surface(&g_fake_surf, 60);
    nvn_be_present(&t, 1.0 / 30, NULL);
    CHECK(g_land_period == 1.0 / 30);   // 60 Hz 显示面上的实际 30 fps 呈现上限
    CHECK(g_adpf_end_n == 1 && g_adpf_end_seq == 1 && g_adpf_end_target == 33333333 && g_adpf_begin_n == 1 && g_adpf_begin_seq == 2);
    nvn_be_present(&t, 1.0 / 30, NULL);
    CHECK(g_adpf_end_n == 2 && g_adpf_end_seq == 2 && g_adpf_begin_n == 2 && g_adpf_begin_seq == 3 && g_adpf_invis_n == 1);
    rp_present_set_surface(NULL, 0);
}

// 上屏线程 CPU（rp_present_tc_note / _take）：只把同一线程相邻两次上屏配对；第一次没有起点不计；跨线程、时间倒退的记成不计样本；
// 取走即清零、起点保留；会话 reset 清起点。修前（没有这项）：新层没有任何「墙钟 vs 线程 CPU」读数（#1501 差距 9）。
static void *tc_other_thread(void *p) { (void)p; rp_present_tc_note(500, 7); return NULL; }
static void t_present_thread_cpu(void) {
    uint64_t tc[4];
    rp_present_reset(); rp_present_tc_take(tc);
    rp_present_tc_note(100, 10);                                   // 第一次：只记起点
    rp_present_tc_take(tc); CHECK(tc[2] == 0 && tc[3] == 0 && tc[0] == 0);
    rp_present_tc_note(300, 60);                                   // 同线程：墙钟 200、CPU 50
    pthread_t th; pthread_create(&th, NULL, tc_other_thread, NULL); pthread_join(th, NULL);   // 别的线程上屏：跨线程，不计
    rp_present_tc_note(700, 80);                                   // 起点是别的线程：跨线程，不计
    rp_present_tc_note(900, 100);                                  // 同线程：墙钟 200、CPU 20
    rp_present_tc_note(800, 120);                                  // 墙钟倒退：不计
    rp_present_tc_take(tc);
    printf("present thread cpu: wall=%llu cpu=%llu n=%llu cross=%llu expected 400/70/2/3\n", (unsigned long long)tc[0], (unsigned long long)tc[1], (unsigned long long)tc[2], (unsigned long long)tc[3]);
    CHECK(tc[0] == 400 && tc[1] == 70 && tc[2] == 2 && tc[3] == 3);
    rp_present_tc_take(tc); CHECK(tc[0] == 0 && tc[2] == 0 && tc[3] == 0);   // 取走即清零
    rp_present_tc_note(1000, 130);                                 // 起点保留：接着上一次（800, 120）算
    rp_present_tc_take(tc); CHECK(tc[2] == 1 && tc[0] == 200 && tc[1] == 10);
    rp_present_reset(); rp_present_tc_note(2000, 200);             // 新会话：不与上一局最后一次配对
    rp_present_tc_take(tc); CHECK(tc[2] == 0 && tc[3] == 0);
    rp_present_reset();
}
static void t_present(void) {   // 上屏：没有窗口按丢帧报；有窗口时取图、按比例贴合、呈现；交换链过期先重建
    g_fl_presents = 0;
    pass_fresh(); rp_present_reset(); g_diag_frames = 0; memset(g_acq_script, 0, sizeof g_acq_script); g_acq_i = g_rebuilds = g_presents = 0;
    tex_t t = mk_tex(71, 0, NVN_FORMAT_RGBA8, 640, 360, 1, 0);
    CHECK(nvn_be_has_window() == 0 && nvn_be_display_hz() == 0);
    uint64_t k1 = nvn_be_present(&t, 0, NULL);
    CHECK(k1 == 1 && nvn_be_presented_time(1) == -1 && g_presents == 0 && nvn_unimpl_count("present", "no_window") == 1);
    CHECK(g_diag_frames == 0);
    CHECK(nvn_be_presented_time(2) == 0);                           // 还没发出的票：没结果
    rp_present_set_surface(&g_fake_surf, 120);
    CHECK(nvn_be_has_window() == 1 && nvn_be_display_hz() == 120);
    CHECK(nvn_be_present(&t, 0, NULL) == 2 && g_presents == 1);
    CHECK(g_diag_frames == 1);   // 真正成功送出后才留下逐帧诊断
    CHECK(g_last_pd.src != NULL && g_last_pd.crop[2] == 0 && g_last_pd.dst[0] == 0 && g_last_pd.dst[1] == 0 && g_last_pd.dst[2] == 1280 && g_last_pd.dst[3] == 720);   // 16:9 贴满 16:9
    CHECK(nvn_be_presented_time(2) == 5.0);                         // 后端没给上屏时刻：按交给呈现引擎的时刻记
    int crop[4] = {0, 0, 320, 300};                                 // 16:15 的裁剪放进 1280x720：高贴满，宽 768，左右各 256 黑边
    CHECK(nvn_be_present(&t, 0, crop) == 3 && g_presents == 2);
    CHECK(g_last_pd.flip_y == 1 && g_last_pd.crop[1] == 0);           // 原点在左下（NVN 默认）：呈现时上下翻；裁剪按存储行号原样给
    CHECK(g_last_pd.crop[0] == 0 && g_last_pd.crop[2] == 320 && g_last_pd.crop[3] == 300 && g_last_pd.dst[0] == 256 && g_last_pd.dst[1] == 0 && g_last_pd.dst[2] == 768 && g_last_pd.dst[3] == 720);
    nvn_be_set_origin_lower_left(0); nvn_be_present(&t, 0, NULL); CHECK(g_last_pd.flip_y == 0); nvn_be_set_origin_lower_left(1);   // 原点在左上：不翻
    // 上屏翻转按被上屏窗口建时的原点（hle_nvn.c win_t.present_ll → nvn_be_set_present_lower_left）：窗口建在左下、之后游戏改左上（01002EF01A316000），
    //   绘制按左上、上屏仍翻；窗口建在左上、之后改回左下的反过来。期望按定义手写；收尾还原成「没有窗口说过」（-1，跟当前原点）
    //   （直接查 rp_present.c 取翻转用的 rp_present_lower_left，不多上屏，免得挪动后面用例的序号与计数）
    nvn_be_set_present_lower_left(1); nvn_be_set_origin_lower_left(0); CHECK(rp_present_lower_left() == 1 && rp_origin_lower_left() == 0);
    nvn_be_set_present_lower_left(0); nvn_be_set_origin_lower_left(1); CHECK(rp_present_lower_left() == 0 && rp_origin_lower_left() == 1);
    nvn_be_set_present_lower_left(-1); CHECK(rp_present_lower_left() == 1);   // 没有窗口说过：跟当前原点（左下）
    g_acq_i = 0; g_acq_script[0] = NVN_GPU_E_OUTDATED;              // 交换链过期：重建一次再取
    CHECK(nvn_be_present(&t, 0, NULL) == 5 && g_rebuilds == 1 && g_presents == 4 && nvn_be_presented_time(5) == 5.0);
    g_acq_i = 0; g_acq_script[0] = NVN_GPU_E_TIMEOUT;               // 没有空闲图：丢这一帧，不算故障
    CHECK(nvn_be_present(&t, 0, NULL) == 6 && g_presents == 4 && nvn_be_presented_time(6) == -1 && nvn_unimpl_count("present", "acquire_failed") == 0);
    CHECK(g_diag_frames == 4);   // 包括重建后成功；取图超时不计
    uint64_t sh = 0, dp = 0; rp_present_stats(&sh, &dp); CHECK(sh == 4 && dp == 2);
    CHECK(g_fl_presents == 4);                                      // 帧日志：成功上屏才记一条（没有窗口 / 取不到图的两帧不记）
    CHECK(g_fl_acq_ns != UINT64_MAX && g_fl_pres_ns != UINT64_MAX);  // 带取图 / 交上屏的 CPU 耗时，不是「未采集」
    rp_present_set_surface(NULL, 0);
    CHECK(nvn_be_has_window() == 0);
    nvn_be_tex_release(&t); pass_fresh(); rp_present_reset();
    CHECK(nvn_be_fps_target() == 30);   // 替身宿主设的是 30
}

static int t_table(const char *tsv) {   // 接线表与名字表一致：值、命令名逐行对
    FILE *f = fopen(tsv, "r"); if (!f) { printf("【红】 打不开接线表 %s\n", tsv); return 1; }
    char line[512]; int rows = 0; if (!fgets(line, sizeof line, f)) { fclose(f); return 1; }
    while (fgets(line, sizeof line, f)) { unsigned v; char name[64]; if (sscanf(line, "%u\t%63s", &v, name) != 2) continue;
        CHECK(v == (unsigned)rows); CHECK(!strcmp(rp_op_name(v), name)); rows++; }
    fclose(f);
    CHECK(rows == OP_NVN2_END_);
    CHECK(!strcmp(rp_op_name(OP_NVN2_END_), "OP_?"));
    return 0;
}

// 三条采样绑定路径都消费程序声明，不以默认的未知声明掩盖漏接线。
static void t_decl_sampling(void) {
    pass_fresh(); g_arena_used = 0; g_resolve_rc = g_cresolve_rc = 0;
    stream s = {0}; q_env(&s);
    tex_t t = mk_tex(7020, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    samp_par sp = {0}; nvn_be_reg_tex(7, &t, NULL); nvn_be_reg_samp(3, &sp);
    g_fprog.nent[1] = 1;
    g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX;
    g_fprog.ent[1][0].count = 1;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_CUBEMAP + 1; g_fprog.dcls[1][0] = 1;
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh);
    bh->stage = 1; bh->first = 0; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_nfb[1] == 1 && g_fb[1][0].u.ts.view != NULL);
    nvn_gpu_view z = g_nfb[1] ? g_fb[1][0].u.ts.view : NULL;
    CHECK(z && z->of->d.d.target == 8 && z->of->d.d.width == 1 && z->of->d.d.depth == 6 &&
        z->of->d.d.format == NVN_FORMAT_R32UI);
    g_fprog.ent[1][0].count = 8; rp_dirty_all();
    stream ar = {0}; q_draw(&ar); end(&ar); submit1(&ar); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_nfb[1] == 8);
    for (uint32_t k = 0; k < 8 && g_nfb[1] == 8; k++) CHECK(g_fb[1][k].u.ts.view == z && g_fb[1][k].u.ts.samp != NULL);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_TEX_SAMP;
    g_cprog.ent[0][0].src_kind = NVN_RB_TEX; g_cprog.ent[0][0].src_reg2 = UINT16_MAX; g_cprog.ent[0][0].count = 1;
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_CUBEMAP + 1; g_cprog.dcls[0][0] = 1;
    stream cs = {0}; bh = put(&cs, OP_BIND_TEX, sizeof *bh);
    bh->stage = 5; bh->first = 0; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    put_dispatch1(&cs); end(&cs); submit1(&cs); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ncb == 1 && g_cb[0].u.ts.view == z);
    CHECK(nvn_unimpl_kinds() == 0);
    pass_fresh();
}

// Two independent slots feed one sampled descriptor; slot 0 is a valid sampler handle.
static void t_separate_sampler(void) {
    pass_fresh(); g_resolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset();
    stream init = {0}; q_env(&init);
    tex_t t = mk_tex(7030, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    samp_par sp = {0}; sp.ws = 1; nvn_be_reg_tex(7, &t, NULL); nvn_be_reg_samp(3, &sp);
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 130; e->src_reg2 = 4; e->count = 1;
    p_bind_handles *b = put(&init, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7;
    q_draw(&init); end(&init); submit1(&init); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 0 && nvn_unimpl_count("draw", "separate_sampler_unbound") == 1);
    stream draw = {0}; b = put(&draw, OP_BIND_SAMP, sizeof *b); b->stage = 1; b->first = 4; b->n = 1; b->h[0] = (uint64_t)3 << 20;
    q_draw(&draw); end(&draw); submit1(&draw); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);
    sp.ws = 6; nvn_be_reg_samp(3, &sp);
    stream again = {0}; q_draw(&again); end(&again); submit1(&again); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6);
    stream missing = {0}; b = put(&missing, OP_BIND_SAMP, sizeof *b); b->stage = 1; b->first = 4; b->n = 1; b->h[0] = (uint64_t)31 << 20;
    q_draw(&missing); end(&missing); submit1(&missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && nvn_unimpl_count("draw", "separate_sampler_unresolved") == 1);
    sp.ws = 7; nvn_be_reg_samp(0, &sp);
    stream zero = {0}; b = put(&zero, OP_BIND_SAMP, sizeof *b); b->stage = 1; b->first = 4; b->n = 1; b->h[0] = 0;
    q_draw(&zero); end(&zero); submit1(&zero); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 7);
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_TEX_SAMP;
    g_cprog.ent[0][0].src_kind = NVN_RB_TEX; g_cprog.ent[0][0].src_reg = 130;
    g_cprog.ent[0][0].src_reg2 = 4; g_cprog.ent[0][0].count = 1;
    int dispatch_before = g_ndisp;
    stream compute = {0}; b = put(&compute, OP_BIND_TEX, sizeof *b); b->stage = 5; b->first = 130; b->n = 1; b->h[0] = 7;
    b = put(&compute, OP_BIND_SAMP, sizeof *b); b->stage = 5; b->first = 4; b->n = 1; b->h[0] = (uint64_t)3 << 20;
    put_dispatch1(&compute); end(&compute); submit1(&compute); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == dispatch_before + 1 && g_ncb == 1 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 6);
    int released = g_samp_releases;
    rp_view_pin_begin();
    nvn_gpu_samp old = rp_samp_by_id(0); CHECK(old != NULL);
    sp.ws = 1; nvn_be_reg_samp(0, &sp);
    CHECK(g_samp_releases == released && old->x == 7); // 已取出者在编码窗口内仍活着
    CHECK(rp_samp_by_id(0) && rp_samp_by_id(0)->x == 1);
    rp_view_pin_end(); CHECK(g_samp_releases == released + 1);
    nvn_be_tex_release(&t); pass_fresh();
}

// #1594：类型 3 句柄（常量采样器）。贴图在 c2 分离贴图表那一槽（槽 130，句柄 7），采样器是着色器里的立即数采样器池下标，
//   布局项 src_reg2 = NVN_GPU_SAMP2_CONST | 池下标：采样器号 = 贴图字高 12 位 | 池下标（Ryujinx ReadPackedId：samplerHandle = 立即数 << 20，
//   handle |= samplerHandle，采样器号取 handle >> 20；字高位为 0 时就是池下标），登记号 = 采样器号 + 当前采样器池起点，
//   不经 NVN 采样器槽；下标 0 是真下标；没登记的号（片 6）绑默认采样器、整笔照画，不拿别的采样器（含只取立即数 / 只取字高位的那个）顶替。
//   句柄的高 12 位取 9（0b1001），立即数取 3（0b0011）：或 = 11，与「只取立即数」3、「只取字高位」9、「相加」12、「异或」10 都不同。
static void t_const_sampler(void) {
    pass_fresh(); g_resolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset();
    stream init = {0}; q_env(&init);
    tex_t t = mk_tex(7033, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    samp_par sp = {0}; nvn_be_reg_tex(7, &t, NULL);
    // 标记 = 采样器的 S 向环绕模式（替身把它原样记在 x 里）；只用替身能原样保留的 1 / 5 / 6 / 7（2..4 里有会被换成近似模式的）
    sp.ws = 1; nvn_be_reg_samp(3, &sp);      // 号 3：标记 1（只取立即数的老口径会取到它）
    sp.ws = 6; nvn_be_reg_samp(0, &sp);      // 号 0：标记 6（真下标）
    sp.ws = 7; nvn_be_reg_samp(9, &sp);      // 号 9：标记 7（只取字高位会取到它）
    sp.ws = 5; nvn_be_reg_samp(11, &sp);     // 号 11 = 9 | 3：标记 5（要取到的）
    sp.ws = 5; nvn_be_reg_samp(4095, &sp);   // 最大的号：标记 5
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 130; e->count = 1; e->src_reg2 = NVN_GPU_SAMP2_CONST | 3;
    p_bind_handles *b = put(&init, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7 | (uint64_t)9 << 20;
    q_draw(&init); end(&init); submit1(&init); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 4);   // 贴图是 c2 分离表那一槽绑的 7
    CHECK(g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);                                // 采样器 = 字高位 9 | 立即数 3 = 11（标记 5），不是 3（标记 1）、9（标记 7）
    CHECK(nvn_unimpl_kinds() == 0);
    // 下标 0 是真下标：不当「没有」；字高位 9 | 0 = 9（标记 7，不是下标 0 的标记 6）
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 0; rp_dirty_all();
    stream zero = {0}; q_draw(&zero); end(&zero); submit1(&zero); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 7);
    // 最大池下标（12 位全 1）：9 | 4095 = 4095
    e->src_reg2 = NVN_GPU_SAMP2_CONST | NVN_GPU_SAMP2_ID_MASK; rp_dirty_all();
    stream top = {0}; q_draw(&top); end(&top); submit1(&top); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);
    // 没登记的采样器号（片 6）：同 Ryujinx 的空采样器，绑默认采样器（= rp_sampler_for_query，最近点 / Repeat）、整笔照画，不拿别的已登记采样器顶替
    //   （9 | 31 = 31 没登记；字高位的 9 登记着，不许退回去取它）；信息计数 + 首行（号 31、登记号 31、池起点 0），不记 const_sampler_unresolved
    g_csd_lines = 0; g_csd_line[0] = 0; const int red_before = g_red_lines;
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 31; rp_dirty_all();
    stream missing = {0}; q_draw(&missing); end(&missing); submit1(&missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 4 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 4);
    CHECK(g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp == rp_sampler_for_query() && g_fb[1][0].u.ts.samp != rp_samp_by_id(9) && g_fb[1][0].u.ts.samp != rp_samp_by_id(3));
    CHECK(nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && nvn_unimpl_kinds() == 0 && g_red_lines == red_before);
    CHECK(rp_const_samp_default_count() == 1 && g_csd_lines == 1 && strstr(g_csd_line, "采样器号 31（登记号 31，采样器池起点 0"));
    // 当前采样器池起点：登记号 = 采样器号 + 起点（池 1 起点 5：采样器号 11 → 登记号 16；只取立即数的 3 + 5 = 8、只取字高位的 9 + 5 = 14、原始号 11 都不是它）
    rp_S.samp_base = 5; sp.ws = 6; nvn_be_reg_samp(16, &sp); sp.ws = 1; nvn_be_reg_samp(8, &sp); sp.ws = 7; nvn_be_reg_samp(14, &sp);
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 3; rp_dirty_all();
    stream based = {0}; q_draw(&based); end(&based); submit1(&based); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 5 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6);
    rp_S.samp_base = 0;
    // 字高位为 0：采样器号就是立即数（老口径的结果，片 2 之后一直如此）。重绑一个只有纹理号 7 的句柄
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 3; rp_dirty_all();
    stream hi0 = {0}; b = put(&hi0, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7;
    q_draw(&hi0); end(&hi0); submit1(&hi0); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 6 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);   // 0 | 3 = 3（标记 1）
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 0; rp_dirty_all();
    stream hi0z = {0}; q_draw(&hi0z); end(&hi0z); submit1(&hi0z); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 7 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6);   // 0 | 0 = 0（标记 6）：下标 0 仍是真下标
    // 字高 12 位全 1、立即数 0：采样器号 = 4095（整 12 位都取，不只低几位）
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 0; rp_dirty_all();
    stream hif = {0}; b = put(&hif, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7 | (uint64_t)NVN_GPU_SAMP2_ID_MASK << 20;
    q_draw(&hif); end(&hif); submit1(&hif); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 8 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);   // 4095 | 0（标记 5）
    CHECK(nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && rp_const_samp_default_count() == 1 && g_csd_lines == 1);   // 已登记的号不记默认采样器；首行每局一次
    // 认不出的形状：具名跳过（separate_texture_shape），不画、不绑任何采样器——不当采样器槽、也不当池下标去猜。
    //   标记位之外还有别的位 / 标记位加各种多余位 / 没有标记位却 >= 128（= 不认标记位的消费方碰到常量采样器编码时落进的同一条
    //   「src_reg2 >= 128」具名跳过：老回放遇到 NVN_GPU_SAMP2_CONST | 池下标，也是这样跳过，不会把它当 NVN 采样器槽静默绑错）
    static const uint16_t odd_shapes[] = { NVN_GPU_SAMP2_CONST | 0x1000u | 3, 0xC003u, 0xFFFEu, 0x0080u, 0x7FFFu };
    const unsigned n_odd = sizeof odd_shapes / sizeof odd_shapes[0];
    for (unsigned k = 0; k < n_odd; k++) {
        e->src_reg2 = odd_shapes[k]; rp_dirty_all();
        stream shape = {0}; q_draw(&shape); end(&shape); submit1(&shape); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_gdraws == 8 && nvn_unimpl_count("draw", "separate_texture_shape") == k + 1);
    }
    // 数组：和别的分离来源一样不接，具名跳过
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 3; e->count = 2; rp_dirty_all();
    stream arr = {0}; q_draw(&arr); end(&arr); submit1(&arr); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 8 && nvn_unimpl_count("draw", "separate_texture_shape") == n_odd + 1);
    e->count = 1;
    // 老来源不变：采样器槽 4 照旧从 NVN 采样器槽取（句柄高 12 位 = 3），没绑照旧具名跳过（贴图字高位此时是 4095，槽路径不受影响）
    e->src_reg2 = 4; rp_dirty_all();
    stream slot_unbound = {0}; q_draw(&slot_unbound); end(&slot_unbound); submit1(&slot_unbound); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 8 && nvn_unimpl_count("draw", "separate_sampler_unbound") == 1);
    stream slot_bound = {0}; b = put(&slot_bound, OP_BIND_SAMP, sizeof *b); b->stage = 1; b->first = 4; b->n = 1; b->h[0] = (uint64_t)3 << 20;
    q_draw(&slot_bound); end(&slot_bound); submit1(&slot_bound); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 9 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);
    // 登记了、但后端建不出（模拟内存不足）：不是「没登记」，不拿默认采样器顶替，照旧具名跳笔（const_sampler_unresolved），不记默认采样器计数；
    //   后端恢复后同一笔照画，取到登记的那个（标记 5），不是默认采样器
    sp.ws = 5; nvn_be_reg_samp(40, &sp); g_samp_fail_nearest = 1;
    e->src_reg2 = NVN_GPU_SAMP2_CONST | 40; rp_dirty_all();
    const int fail_draws0 = g_gdraws; const uint64_t fail_csd0 = rp_const_samp_default_count();
    stream failed = {0}; b = put(&failed, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7;   // 字高位 0：采样器号 = 池下标 40
    q_draw(&failed); end(&failed); submit1(&failed); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == fail_draws0 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 1 && rp_const_samp_default_count() == fail_csd0);
    g_samp_fail_nearest = 0;
    stream recovered = {0}; q_draw(&recovered); end(&recovered); submit1(&recovered); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == fail_draws0 + 1 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5 && g_fb[1][0].u.ts.samp != rp_sampler_for_query() && rp_const_samp_default_count() == fail_csd0);
    // 计算阶段（片 7）：同一份合同。带标记位的计算项：采样器号 = 贴图字高 12 位 | 池下标，登记号 = 采样器号 + 池起点，照常派发（不再具名跳过）。
    //   此刻登记着的号：3（标记 1）、0（标记 6）、9（标记 7）、11（标记 5）、4095（标记 5）、40（标记 5）；池起点 5 时 16（标记 6）、8（标记 1）、14（标记 7）。
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_TEX_SAMP;
    g_cprog.ent[0][0].src_kind = NVN_RB_TEX; g_cprog.ent[0][0].src_reg = 130; g_cprog.ent[0][0].count = 1;
    int dispatch_before = g_ndisp;
    const uint64_t cs_csd0 = rp_const_samp_default_count(); const int cs_red0 = g_red_lines;
    static const struct { uint16_t reg2; uint64_t handle; int mark; } cs_ok[] = {
        { NVN_GPU_SAMP2_CONST | 3, 7 | (uint64_t)9 << 20, 5 },                                 // 9 | 3 = 11（标记 5）：不是只取立即数的 3（标记 1）、只取字高位的 9（标记 7）
        { NVN_GPU_SAMP2_CONST | 0, 7 | (uint64_t)9 << 20, 7 },                                 // 9 | 0 = 9（标记 7）
        { NVN_GPU_SAMP2_CONST | 0, 7, 6 },                                                      // 下标 0 是真下标（标记 6），不当「没有」
        { NVN_GPU_SAMP2_CONST | 3, 7, 1 },                                                      // 字高位 0：采样器号就是立即数 3（标记 1）
        { NVN_GPU_SAMP2_CONST | NVN_GPU_SAMP2_ID_MASK, 7 | (uint64_t)9 << 20, 5 },             // 9 | 4095 = 4095（标记 5）
        { NVN_GPU_SAMP2_CONST | 0, 7 | (uint64_t)NVN_GPU_SAMP2_ID_MASK << 20, 5 },             // 字高 12 位全 1、立即数 0 = 4095（整 12 位都取）
    };
    for (unsigned k = 0; k < sizeof cs_ok / sizeof cs_ok[0]; k++) {
        g_cprog.ent[0][0].src_reg2 = cs_ok[k].reg2; rp_dirty_all();
        stream compute = {0}; b = put(&compute, OP_BIND_TEX, sizeof *b); b->stage = 5; b->first = 130; b->n = 1; b->h[0] = cs_ok[k].handle;
        put_dispatch1(&compute); end(&compute); submit1(&compute); fake_complete(NVN_GPU_DONE_OK);
        CHECK(g_ndisp == dispatch_before + 1 && g_ncb == 1 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 4 &&   // 贴图仍是 c2 分离表那一槽绑的 7
              g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == cs_ok[k].mark);
        dispatch_before = g_ndisp;
    }
    CHECK(nvn_unimpl_count("draw", "compute_texture_array") == 0);
    // 池起点 5：采样器号 11 → 登记号 16（标记 6）；只取立即数的 3 + 5 = 8（标记 1）、只取字高位的 9 + 5 = 14（标记 7）、原始号 11（标记 5）都不是它
    rp_S.samp_base = 5; g_cprog.ent[0][0].src_reg2 = NVN_GPU_SAMP2_CONST | 3; rp_dirty_all();
    stream cbase = {0}; b = put(&cbase, OP_BIND_TEX, sizeof *b); b->stage = 5; b->first = 130; b->n = 1; b->h[0] = 7 | (uint64_t)9 << 20;
    put_dispatch1(&cbase); end(&cbase); submit1(&cbase); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == dispatch_before + 1 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 6);
    rp_S.samp_base = 0; dispatch_before = g_ndisp;
    // 没登记的采样器号（片 6 的口径同样用在计算阶段）：9 | 31 = 31 没登记 → 绑默认采样器（rp_sampler_for_query）照派发，不拿字高位的 9 / 只取立即数的 3 顶替；
    //   信息计数 + 1、首行已出过（每局一次），不记 const_sampler_unresolved（图形阶段的建不出那一笔已记 1 次）、不报红
    g_cprog.ent[0][0].src_reg2 = NVN_GPU_SAMP2_CONST | 31; rp_dirty_all();
    stream cmiss = {0}; b = put(&cmiss, OP_BIND_TEX, sizeof *b); b->stage = 5; b->first = 130; b->n = 1; b->h[0] = 7 | (uint64_t)9 << 20;
    put_dispatch1(&cmiss); end(&cmiss); submit1(&cmiss); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == dispatch_before + 1 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 4);
    CHECK(g_cb[0].u.ts.samp && g_cb[0].u.ts.samp == rp_sampler_for_query() && g_cb[0].u.ts.samp != rp_samp_by_id(9) && g_cb[0].u.ts.samp != rp_samp_by_id(3));
    CHECK(nvn_unimpl_count("draw", "const_sampler_unresolved") == 1 && rp_const_samp_default_count() == cs_csd0 + 1 && g_csd_lines == 1 && g_red_lines == cs_red0);
    dispatch_before = g_ndisp;
    // 登记了、但后端建不出（模拟内存不足）：不是「没登记」，不拿默认采样器顶替，具名跳过（const_sampler_unresolved）、不派发、不记默认采样器计数；
    //   后端恢复后同一笔照派发，取到登记的那个（标记 5）。号 41 是新登记的，还没建过对象
    sp.ws = 5; nvn_be_reg_samp(41, &sp); g_samp_fail_nearest = 1;
    g_cprog.ent[0][0].src_reg2 = NVN_GPU_SAMP2_CONST | 41; rp_dirty_all();
    stream cfail = {0}; b = put(&cfail, OP_BIND_TEX, sizeof *b); b->stage = 5; b->first = 130; b->n = 1; b->h[0] = 7;   // 字高位 0：采样器号 = 池下标 41
    put_dispatch1(&cfail); end(&cfail); submit1(&cfail); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == dispatch_before && nvn_unimpl_count("draw", "const_sampler_unresolved") == 2 && rp_const_samp_default_count() == cs_csd0 + 1);
    g_samp_fail_nearest = 0;
    stream crec = {0}; put_dispatch1(&crec); end(&crec); submit1(&crec); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == dispatch_before + 1 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 5 && g_cb[0].u.ts.samp != rp_sampler_for_query() && rp_const_samp_default_count() == cs_csd0 + 1);
    dispatch_before = g_ndisp;
    // 认不出的形状仍具名跳过（compute_texture_array），不派发、不绑任何采样器：标记位之外还有别的位 / 没有标记位却 >= 128（同图形阶段的 odd_shapes）、数组
    unsigned cs_skips = 0;
    for (unsigned k = 0; k < n_odd; k++) {
        g_cprog.ent[0][0].src_reg2 = odd_shapes[k]; rp_dirty_all();
        stream shape = {0}; b = put(&shape, OP_BIND_TEX, sizeof *b); b->stage = 5; b->first = 130; b->n = 1; b->h[0] = 7;
        put_dispatch1(&shape); end(&shape); submit1(&shape); fake_complete(NVN_GPU_DONE_OK);
        cs_skips++;
        CHECK(g_ndisp == dispatch_before && nvn_unimpl_count("draw", "compute_texture_array") == cs_skips);
    }
    g_cprog.ent[0][0].src_reg2 = NVN_GPU_SAMP2_CONST | 3; g_cprog.ent[0][0].count = 2; rp_dirty_all();
    stream carr = {0}; put_dispatch1(&carr); end(&carr); submit1(&carr); fake_complete(NVN_GPU_DONE_OK);
    cs_skips++;
    CHECK(g_ndisp == dispatch_before && nvn_unimpl_count("draw", "compute_texture_array") == cs_skips);
    nvn_be_tex_release(&t); pass_fresh();
}

// 句柄在用户常量缓冲里的贴图（NVN_RB_TEXU，iOS nvn_mtl.m ubo_packed 语义）：纹理字在常量槽 2 +0x10（= 7），同一句柄的另一半是
//   常量槽 3 +0x8 的采样器号字（sid，左移 20 = 3 << 20）→ 打包句柄 7 | 3 << 20：纹理号 7（4x4 那张）、采样器号 3（ws = 5）。
//   游戏改了常量缓冲里的纹理字（= 8，2x2 那张），下一笔就读到新的：句柄在编码这一刻从游戏内存读
static void t_ubo_texture(void) {
    pass_fresh(); g_resolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset();
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    stream s = {0}; q_env(&s);
    tex_t t7 = mk_tex(7040, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0), t8 = mk_tex(7041, 0x8000, NVN_FORMAT_RGBA8, 2, 2, 1, 0);
    samp_par sp = {0}; sp.ws = 5; nvn_be_reg_tex(7, &t7, NULL); nvn_be_reg_tex(8, &t8, NULL); nvn_be_reg_samp(3, &sp);
    uint32_t tw = 7, sw = 3; memcpy(g_pmem + PG + 0x10, &tw, 4); memcpy(g_pmem + 2 * PG + 0x8, &sw, 4);
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEXU; e->src_reg = 2; e->src_off = 0x10; e->src_reg2 = 0; e->count = 1;
    g_fprog.nusrc = 1;
    g_fprog.usrc[0] = (struct rp_usrc){ .tex = { .reg = 2, .off = 0x10, .pair = 1, .pair_reg = 3, .pair_off = 0x8, .pair_sid = 1 },
                                        .samp_kind = NVN_RB_SAMPU, .samp = { .reg = 3, .off = 0x8, .sid = 1, .pair = 1, .pair_reg = 2, .pair_off = 0x10 } };
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 2; ub->addr = base + PG; ub->size = 64;
    ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 3; ub->addr = base + 2 * PG; ub->size = 64;
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 4 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);
    tw = 8; memcpy(g_pmem + PG + 0x10, &tw, 4);
    stream again = {0}; q_draw(&again); end(&again); submit1(&again); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 2 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);
    CHECK(nvn_unimpl_kinds() == 0);
    // 常量缓冲句柄走同一格式能力检查：RGBA8 无线性过滤时必须交最近点并具名记账。
    g_caps_nofilter = NVN_FORMAT_RGBA8; sp.magf = NVN_MAG_FILTER_LINEAR; sp.minf = NVN_MIN_FILTER_LINEAR;
    nvn_be_reg_samp(3, &sp);
    stream filter = {0}; q_draw(&filter); end(&filter); submit1(&filter); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_samp_last.magf == NVN_MAG_FILTER_NEAREST && g_samp_last.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST &&
          nvn_unimpl_count("tex", "filter_unsupported_fmt_0x25") == 1);
    g_caps_nofilter = ~0u;
    nvn_be_tex_release(&t7); nvn_be_tex_release(&t8); pass_fresh();
}

// #1594 片 3：类型 3 句柄、贴图字在游戏常量缓冲里（NVN_RB_TEXU）+ 采样器是着色器里的立即数池下标（usrc 表项 SAMPI + samp_pool = 1）。
//   贴图字在编码这一刻从游戏内存读（常量槽 2 +0x10，同 t_ubo_texture）；采样器号 = 贴图字高 12 位 | 池下标（同 c2 常量采样器、Ryujinx ReadPackedId，
//   字高位为 0 时就是池下标），登记号 = 采样器号 + 当前采样器池起点；下标 0 是真下标；没登记的号（片 6）绑默认采样器、整笔照画，
//   不拿别的采样器（含只取池下标 / 只取字高位的那个）顶替；
//   samp_pool = 0 的 SAMPI（离线库写法，登记号原值）口径不变：不或字高位、不加池起点，没登记照旧走 sampler_slot_unresolved。
//   字高 12 位取 9（0b1001）、池下标取 3（0b0011）：或 = 11，与「只取池下标」3、「只取字高位」9、「相加」12、「异或」10 都不同。
static void t_ubo_const_sampler(void) {
    pass_fresh(); g_resolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset();
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    stream s = {0}; q_env(&s);
    tex_t t7 = mk_tex(7044, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0), t8 = mk_tex(7045, 0x8000, NVN_FORMAT_RGBA8, 2, 2, 1, 0);
    samp_par sp = {0}; nvn_be_reg_tex(7, &t7, NULL); nvn_be_reg_tex(8, &t8, NULL);
    // 标记 = 采样器的 S 向环绕模式（替身把它原样记在 x 里）；只用替身能原样保留的 1 / 5 / 6 / 7
    sp.ws = 1; nvn_be_reg_samp(3, &sp);      // 号 3：标记 1（字高位为 0 时要取到的；字高位非 0 时只取池下标的老口径会取到它）
    sp.ws = 6; nvn_be_reg_samp(0, &sp);      // 号 0：标记 6（真下标）
    sp.ws = 7; nvn_be_reg_samp(9, &sp);      // 号 9：标记 7（只取字高位会取到它）
    sp.ws = 5; nvn_be_reg_samp(11, &sp);     // 号 11 = 9 | 3：标记 5（字高位 9 时要取到的）
    sp.ws = 5; nvn_be_reg_samp(4095, &sp);   // 最大的号：标记 5
    uint32_t tw = 7 | (uint32_t)9 << 20; memcpy(g_pmem + PG + 0x10, &tw, 4);   // 贴图字：纹理号 7；高 12 位 9 要与立即数相或
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEXU; e->src_reg = 2; e->src_off = 0x10; e->src_reg2 = 0; e->count = 1;
    g_fprog.nusrc = 1;
    g_fprog.usrc[0] = (struct rp_usrc){ .tex = { .reg = 2, .off = 0x10 }, .samp_kind = NVN_RB_SAMPI, .samp_reg = 3, .samp_pool = 1 };
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 2; ub->addr = base + PG; ub->size = 64;
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 4);   // 贴图 = 游戏常量缓冲里的字 7（4x4 那张）
    CHECK(g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);                                // 采样器 = 字高位 9 | 池下标 3 = 11（标记 5），不是 3（标记 1）、9（标记 7）
    CHECK(nvn_unimpl_kinds() == 0);
    // 游戏改了常量缓冲里的贴图字（= 8，字高位为 0）：下一笔读到新的（贴图跟着换，采样器 = 0 | 3 = 池下标 3）
    tw = 8; memcpy(g_pmem + PG + 0x10, &tw, 4);
    stream again = {0}; q_draw(&again); end(&again); submit1(&again); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 2 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);
    // 下标 0 是真下标：不当「没有」（字高位为 0 时，0 | 0 = 号 0）
    g_fprog.usrc[0].samp_reg = 0; rp_dirty_all();
    stream zero = {0}; q_draw(&zero); end(&zero); submit1(&zero); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6);
    // 最大池下标（12 位全 1）
    g_fprog.usrc[0].samp_reg = 4095; rp_dirty_all();
    stream top = {0}; q_draw(&top); end(&top); submit1(&top); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 4 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);
    // 没登记的池下标（片 6）：同 Ryujinx 的空采样器，绑默认采样器（= rp_sampler_for_query）、整笔照画，不拿别的已登记采样器顶替；
    //   信息计数 + 首行（号 31、登记号 31、池起点 0），不记 const_sampler_unresolved、不报红
    g_csd_lines = 0; g_csd_line[0] = 0; const int red_before = g_red_lines; const uint64_t csd0 = rp_const_samp_default_count();
    g_fprog.usrc[0].samp_reg = 31; rp_dirty_all();
    stream missing = {0}; q_draw(&missing); end(&missing); submit1(&missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 5 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 2);   // 贴图字 8（2x2 那张）照常取
    CHECK(g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp == rp_sampler_for_query() && g_fb[1][0].u.ts.samp != rp_samp_by_id(3) && g_fb[1][0].u.ts.samp != rp_samp_by_id(11));
    CHECK(nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && nvn_unimpl_kinds() == 0 && g_red_lines == red_before);
    CHECK(rp_const_samp_default_count() == csd0 + 1 && g_csd_lines == 1 && strstr(g_csd_line, "采样器号 31（登记号 31，采样器池起点 0"));
    // 当前采样器池起点：登记号 = 采样器号 + 起点（池 1 起点 5：字高位为 0、池下标 3 → 登记号 8；原始号 3 不再是它）
    rp_S.samp_base = 5; sp.ws = 6; nvn_be_reg_samp(8, &sp);   // 登记号 8：标记 6（原始号 3 是标记 1）
    g_fprog.usrc[0].samp_reg = 3; rp_dirty_all();
    stream based = {0}; q_draw(&based); end(&based); submit1(&based); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 6 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6 && rp_const_samp_default_count() == csd0 + 1);   // 已登记：不记默认采样器
    // 池起点非零时，下标 0 也是真下标：登记号 = 0 + 起点（不退回原始号 0 = 标记 6）
    sp.ws = 5; nvn_be_reg_samp(5, &sp);   // 登记号 5：标记 5
    g_fprog.usrc[0].samp_reg = 0; rp_dirty_all();
    stream based0 = {0}; q_draw(&based0); end(&based0); submit1(&based0); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 7 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);
    g_fprog.usrc[0].samp_reg = 3;
    // 对照：samp_pool = 0 的 SAMPI（离线库写法）是登记号原值，不加池起点（加了会取到登记号 8 = 标记 6）
    g_fprog.usrc[0].samp_pool = 0; rp_dirty_all();
    stream raw = {0}; q_draw(&raw); end(&raw); submit1(&raw); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 8 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);
    // 对照：samp_pool = 0 且没登记：老口径的具名跳过（sampler_slot_unresolved）——不绑默认采样器（片 6 只管池下标形态）、不记成常量采样器的跳过 / 默认采样器计数
    g_fprog.usrc[0].samp_reg = 31; rp_dirty_all();
    stream raw_missing = {0}; q_draw(&raw_missing); end(&raw_missing); submit1(&raw_missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 8 && nvn_unimpl_count("draw", "sampler_slot_unresolved") == 1 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && rp_const_samp_default_count() == csd0 + 1);
    // 字高 12 位非零（片 5，Ryujinx ReadPackedId：采样器号 = 字高位 | 池下标）。贴图字写回高 12 位 = 9，样式回到 samp_pool = 1
    g_fprog.usrc[0].samp_pool = 1; g_fprog.usrc[0].samp_reg = 3;
    tw = 7 | (uint32_t)9 << 20; memcpy(g_pmem + PG + 0x10, &tw, 4);
    // 池起点 5：采样器号 9 | 3 = 11 → 登记号 16（标记 1）；只取池下标的 3 + 5 = 8（标记 6）、只取字高位的 9 + 5 = 14（标记 7）、不加起点的 11（标记 5）都不是它
    sp.ws = 7; nvn_be_reg_samp(14, &sp); sp.ws = 1; nvn_be_reg_samp(16, &sp);
    rp_dirty_all();
    stream hi_based = {0}; q_draw(&hi_based); end(&hi_based); submit1(&hi_based); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 9 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);
    // 池起点 0、池下标 0：9 | 0 = 9（标记 7）——下标 0 或上字高位仍是 9，不是号 0（标记 6）
    rp_S.samp_base = 0; g_fprog.usrc[0].samp_reg = 0; rp_dirty_all();
    stream hi_zero = {0}; q_draw(&hi_zero); end(&hi_zero); submit1(&hi_zero); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 10 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 7);
    // 字高 12 位全 1、池下标 0：号 4095（整 12 位都取，不只低几位）
    tw = 7 | (uint32_t)NVN_GPU_SAMP2_ID_MASK << 20; memcpy(g_pmem + PG + 0x10, &tw, 4);
    rp_dirty_all();
    stream hi_top = {0}; q_draw(&hi_top); end(&hi_top); submit1(&hi_top); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 11 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5);
    // 或出来的号没登记（片 6）：绑默认采样器、整笔照画（9 | 31 = 31 没登记；号 9 登记着，只取字高位的会取到它，不许顶替）；第二次计数 +1、首行不再打
    tw = 7 | (uint32_t)9 << 20; memcpy(g_pmem + PG + 0x10, &tw, 4);
    g_fprog.usrc[0].samp_reg = 31; rp_dirty_all();
    stream hi_missing = {0}; q_draw(&hi_missing); end(&hi_missing); submit1(&hi_missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 12 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp == rp_sampler_for_query() && g_fb[1][0].u.ts.samp != rp_samp_by_id(9));
    CHECK(nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && rp_const_samp_default_count() == csd0 + 2 && g_csd_lines == 1);
    // 对照：samp_pool = 0 的 SAMPI 不或字高位（登记号原值 3 = 标记 1；或了字高位 9 会取到 11 = 标记 5）
    g_fprog.usrc[0].samp_pool = 0; g_fprog.usrc[0].samp_reg = 3; rp_dirty_all();
    stream hi_raw = {0}; q_draw(&hi_raw); end(&hi_raw); submit1(&hi_raw); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 13 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);
    CHECK(nvn_unimpl_count("draw", "sampler_slot_unresolved") == 1 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && rp_const_samp_default_count() == csd0 + 2);
    // 登记了、但后端建不出（模拟内存不足）：不是「没登记」，不拿默认采样器顶替，照旧具名跳笔（const_sampler_unresolved），不记默认采样器计数；
    //   后端恢复后同一笔照画，取到登记的那个（标记 5），不是默认采样器
    sp.ws = 5; nvn_be_reg_samp(40, &sp); g_samp_fail_nearest = 1;
    tw = 7; memcpy(g_pmem + PG + 0x10, &tw, 4);   // 字高位 0：采样器号 = 池下标 40
    g_fprog.usrc[0].samp_pool = 1; g_fprog.usrc[0].samp_reg = 40; rp_dirty_all();
    const int fail_draws0 = g_gdraws; const uint64_t fail_csd0 = rp_const_samp_default_count();
    stream failed = {0}; q_draw(&failed); end(&failed); submit1(&failed); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == fail_draws0 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 1 && rp_const_samp_default_count() == fail_csd0);
    g_samp_fail_nearest = 0;
    stream recovered = {0}; q_draw(&recovered); end(&recovered); submit1(&recovered); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == fail_draws0 + 1 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 5 && g_fb[1][0].u.ts.samp != rp_sampler_for_query() && rp_const_samp_default_count() == fail_csd0);
    rp_S.samp_base = 0;
    nvn_be_tex_release(&t7); nvn_be_tex_release(&t8); pass_fresh();
}

// #1594 片 10：计算阶段、贴图字在游戏常量缓冲里的类型 3 句柄（NVN_RB_TEXU + usrc 表项 SAMPI / samp_pool = 1）。合同同 t_ubo_const_sampler（图形阶段）：
//   贴图字在派发编码这一刻从游戏内存读；采样器号 = 贴图字高 12 位 | 池下标，登记号 = 采样器号 + 池起点，下标 0 是真下标；没登记的号绑默认采样器照派发，
//   登记了但后端建不出的具名跳过（const_sampler_unresolved）；samp_pool = 0 的 SAMPI 口径不变。差别只有一处：贴图字从计算阶段（5）绑的常量缓冲读，
//   不是图形阶段绑的那一份。字高 12 位取 9、池下标取 3：或 = 11，与「只取池下标」3、「只取字高位」9 都不同。
static void t_cs_ubo_const_sampler(void) {
    pass_fresh(); g_resolve_rc = 0; g_cresolves = g_cresolve_rc = g_cpipe_gets = g_ndisp = g_cbinds = 0; g_ncb = 0; g_cresolve_next = NULL; nvn_unimpl_reset();
    g_csd_lines = 0; g_csd_line[0] = 0;
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    tex_t t7 = mk_tex(7046, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0), t8 = mk_tex(7047, 0x8000, NVN_FORMAT_RGBA8, 2, 2, 1, 0);
    samp_par sp = {0}; nvn_be_reg_tex(7, &t7, NULL); nvn_be_reg_tex(8, &t8, NULL);
    // 标记 = 采样器的 S 向环绕模式（替身把它原样记在 x 里）；只用替身能原样保留的 1 / 5 / 6 / 7
    sp.ws = 1; nvn_be_reg_samp(3, &sp);      // 号 3：标记 1（字高位为 0 时要取到的；字高位非 0 时只取池下标的老口径会取到它）
    sp.ws = 6; nvn_be_reg_samp(0, &sp);      // 号 0：标记 6（真下标）
    sp.ws = 7; nvn_be_reg_samp(9, &sp);      // 号 9：标记 7（只取字高位会取到它）
    sp.ws = 5; nvn_be_reg_samp(11, &sp);     // 号 11 = 9 | 3：标记 5（字高位 9 时要取到的）
    sp.ws = 5; nvn_be_reg_samp(4095, &sp);   // 最大的号：标记 5
    uint32_t tw = 7 | (uint32_t)9 << 20; memcpy(g_pmem + PG + 0x10, &tw, 4);   // 贴图字：纹理号 7；高 12 位 9 要与立即数相或
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1; nvn_gpu_layout_ent *e = &g_cprog.ent[0][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEXU; e->src_reg = 2; e->src_off = 0x10; e->src_reg2 = 0; e->count = 1;
    g_cprog.nusrc = 1;
    g_cprog.usrc[0] = (struct rp_usrc){ .tex = { .reg = 2, .off = 0x10 }, .samp_kind = NVN_RB_SAMPI, .samp_reg = 3, .samp_pool = 1 };
    int d0 = g_ndisp;
    stream s = {0}; p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 5; ub->index = 2; ub->addr = base + PG; ub->size = 64;
    put_dispatch1(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 1 && g_ncb == 1 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 4);   // 贴图 = 游戏常量缓冲里的字 7（4x4 那张）
    CHECK(g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 5);                                                  // 采样器 = 字高位 9 | 池下标 3 = 11（标记 5），不是 3（标记 1）、9（标记 7）
    CHECK(nvn_unimpl_kinds() == 0);
    // 游戏改了常量缓冲里的贴图字（= 8，字高位为 0）：下一次派发读到新的（贴图跟着换，采样器 = 0 | 3 = 池下标 3）
    tw = 8; memcpy(g_pmem + PG + 0x10, &tw, 4);
    stream again = {0}; put_dispatch1(&again); end(&again); submit1(&again); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 2 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 2 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 1);
    // 下标 0 是真下标：不当「没有」（字高位为 0 时，0 | 0 = 号 0）；最大池下标（12 位全 1）
    g_cprog.usrc[0].samp_reg = 0; rp_dirty_all();
    stream zero = {0}; put_dispatch1(&zero); end(&zero); submit1(&zero); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 3 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 6);
    g_cprog.usrc[0].samp_reg = 4095; rp_dirty_all();
    stream top = {0}; put_dispatch1(&top); end(&top); submit1(&top); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 4 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 5);
    // 没登记的池下标（片 6 的口径同样用在计算阶段）：绑默认采样器（= rp_sampler_for_query）、整次照派发，不拿别的已登记采样器顶替；
    //   信息计数 + 1，不记 const_sampler_unresolved、不报红
    const int red_before = g_red_lines; const uint64_t csd0 = rp_const_samp_default_count();
    g_cprog.usrc[0].samp_reg = 31; rp_dirty_all();
    stream missing = {0}; put_dispatch1(&missing); end(&missing); submit1(&missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 5 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 2);   // 贴图字 8（2x2 那张）照常取
    CHECK(g_cb[0].u.ts.samp && g_cb[0].u.ts.samp == rp_sampler_for_query() && g_cb[0].u.ts.samp != rp_samp_by_id(3) && g_cb[0].u.ts.samp != rp_samp_by_id(11));
    CHECK(nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && nvn_unimpl_kinds() == 0 && g_red_lines == red_before && rp_const_samp_default_count() == csd0 + 1);
    // 当前采样器池起点：登记号 = 采样器号 + 起点（池 1 起点 5：字高位为 0、池下标 3 → 登记号 8；原始号 3 不再是它）
    rp_S.samp_base = 5; sp.ws = 6; nvn_be_reg_samp(8, &sp);   // 登记号 8：标记 6（原始号 3 是标记 1）
    g_cprog.usrc[0].samp_reg = 3; rp_dirty_all();
    stream based = {0}; put_dispatch1(&based); end(&based); submit1(&based); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 6 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 6 && rp_const_samp_default_count() == csd0 + 1);
    // 对照：samp_pool = 0 的 SAMPI（离线库写法）是登记号原值，不加池起点（加了会取到登记号 8 = 标记 6）
    g_cprog.usrc[0].samp_pool = 0; g_cprog.usrc[0].samp_reg = 3; rp_dirty_all();
    stream raw = {0}; put_dispatch1(&raw); end(&raw); submit1(&raw); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 7 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 1);
    // 对照：samp_pool = 0 且没登记：老口径的具名跳过（sampler_slot_unresolved）——不绑默认采样器（片 6 只管池下标形态）、不记成常量采样器的跳过 / 默认采样器计数
    g_cprog.usrc[0].samp_reg = 31; rp_dirty_all();
    stream raw_missing = {0}; put_dispatch1(&raw_missing); end(&raw_missing); submit1(&raw_missing); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 7 && nvn_unimpl_count("draw", "sampler_slot_unresolved") == 1 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 0 && rp_const_samp_default_count() == csd0 + 1);
    rp_S.samp_base = 0; g_cprog.usrc[0].samp_pool = 1; g_cprog.usrc[0].samp_reg = 3;
    // 字高 12 位非零：贴图字写回 7 | 9 << 20，池下标 3 → 号 11（标记 5）；字高 12 位全 1、池下标 0 → 号 4095（整 12 位都取）
    tw = 7 | (uint32_t)9 << 20; memcpy(g_pmem + PG + 0x10, &tw, 4); rp_dirty_all();
    stream hi = {0}; put_dispatch1(&hi); end(&hi); submit1(&hi); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 8 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 4 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 5);
    tw = 7 | (uint32_t)NVN_GPU_SAMP2_ID_MASK << 20; memcpy(g_pmem + PG + 0x10, &tw, 4); g_cprog.usrc[0].samp_reg = 0; rp_dirty_all();
    stream hif = {0}; put_dispatch1(&hif); end(&hif); submit1(&hif); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 9 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 5);
    // 登记了、但后端建不出（模拟内存不足）：不是「没登记」，不拿默认采样器顶替，具名跳过（const_sampler_unresolved）、不派发、不记默认采样器计数；
    //   后端恢复后同一次照派发，取到登记的那个（标记 5）。号 40 是新登记的，还没建过对象
    sp.ws = 5; nvn_be_reg_samp(40, &sp); g_samp_fail_nearest = 1;
    tw = 7; memcpy(g_pmem + PG + 0x10, &tw, 4);   // 字高位 0：采样器号 = 池下标 40
    g_cprog.usrc[0].samp_reg = 40; rp_dirty_all();
    const uint64_t fail_csd0 = rp_const_samp_default_count();
    stream failed = {0}; put_dispatch1(&failed); end(&failed); submit1(&failed); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 9 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 1 && rp_const_samp_default_count() == fail_csd0);
    g_samp_fail_nearest = 0;
    stream recovered = {0}; put_dispatch1(&recovered); end(&recovered); submit1(&recovered); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 10 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 5 && g_cb[0].u.ts.samp != rp_sampler_for_query() && rp_const_samp_default_count() == fail_csd0);
    // 形状不对：数组 / 句柄来源表下标越界 → 具名跳过（uniform_handle_shape），不派发
    e->count = 2; rp_dirty_all();
    stream arr = {0}; put_dispatch1(&arr); end(&arr); submit1(&arr); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 10 && nvn_unimpl_count("draw", "uniform_handle_shape") == 1);
    e->count = 1; e->src_reg2 = 5; rp_dirty_all();
    stream oob = {0}; put_dispatch1(&oob); end(&oob); submit1(&oob); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 10 && nvn_unimpl_count("draw", "uniform_handle_shape") == 2);
    e->src_reg2 = 0;
    // 贴图字取计算阶段（5）绑的常量缓冲，不是图形阶段（1）绑的那一份：同一个槽号，图形阶段指向写着 7 的那块（4x4），计算阶段指向写着 8 的另一块（2x2）→ 取 2x2
    g_cprog.usrc[0].samp_reg = 3; tw = 7; memcpy(g_pmem + PG + 0x10, &tw, 4); tw = 8; memcpy(g_pmem + PG + 0x110, &tw, 4); rp_dirty_all();
    stream split = {0}; ub = put(&split, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 2; ub->addr = base + PG; ub->size = 64;
    ub = put(&split, OP_BIND_UBO, sizeof *ub); ub->stage = 5; ub->index = 2; ub->addr = base + PG + 0x100; ub->size = 64;
    put_dispatch1(&split); end(&split); submit1(&split); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == d0 + 11 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 2 && g_cb[0].u.ts.samp && g_cb[0].u.ts.samp->x == 1);
    // 计算阶段这个槽解绑、只剩图形阶段（1）绑着写着 7 的那块：计算读到的是空槽（字 0），不是图形阶段那份的 7——4x4 那张不许被绑上；
    //   号 0 没登记、着色器又没声明目标（建不出零贴图）→ 具名跳过（texture_slot_unresolved），不派发
    const int nd_unb = g_ndisp;
    stream unbind = {0}; ub = put(&unbind, OP_BIND_UBO, sizeof *ub); ub->stage = 5; ub->index = 2; ub->addr = 0; ub->size = 0;
    put_dispatch1(&unbind); end(&unbind); submit1(&unbind); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == nd_unb && nvn_unimpl_count("draw", "texture_slot_unresolved") == 1);
    // 空槽：计算阶段那块里的字是越池号（20 位全 1），着色器声明了二维浮点 → 没登记的号绑声明类型的 1x1 零贴图、整次照派发，不跳（同图形阶段 t_ubo_texture_unregistered）；
    //   声明有没有传下去由这一例认出（目标 0 建不出零贴图，就是上面那一例的具名跳过）
    g_cprog.dtt[0][0] = NVN_TEXTURE_TARGET_2D + 1; g_cprog.dcls[0][0] = 0; nvn_be_set_tex_pool_n(64);
    tw = 0xFFFFF; memcpy(g_pmem + PG + 0x110, &tw, 4);
    const uint64_t uz0 = rp_ubo_zero_count();
    stream zerotex = {0}; ub = put(&zerotex, OP_BIND_UBO, sizeof *ub); ub->stage = 5; ub->index = 2; ub->addr = base + PG + 0x100; ub->size = 64;
    put_dispatch1(&zerotex); end(&zerotex); submit1(&zerotex); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == nd_unb + 1 && g_cb[0].u.ts.view && g_cb[0].u.ts.view->of->d.d.width == 1 && g_cb[0].u.ts.view->of->d.d.format == NVN_FORMAT_RGBA8 && rp_ubo_zero_count() == uz0 + 1);
    nvn_be_set_tex_pool_n(0);
    nvn_be_tex_release(&t7); nvn_be_tex_release(&t8); pass_fresh();
}

// #1594 片 6：类型 3 句柄的采样器号没登记 = Ryujinx 的空采样器（SamplerPool.Get 对 (uint)id >= Items.Length 给 null、池内没写过的号读出全零描述符；
//   Vulkan 后端把 null 采样器换成最近点 / Repeat 的 dummy 采样器，draw 照常提交）：rp_samp_for_const_handle 给默认采样器（= rp_sampler_for_query：
//   最近点 / Repeat / 无比较 / 各向异性 1），调用方整笔照画；信息计数 rp_const_samp_default_count + 每局首次一行（带采样器号、登记号、池起点和容量）。
//   范围：已登记的号原样返回、不记计数；登记了但后端建不出的、默认采样器也建不出的返回 NULL（调用方具名跳笔 const_sampler_unresolved，draw 级的跳笔
//   在 t_const_sampler / t_ubo_const_sampler 末尾）；换局计数清零、首行重来。
static void t_const_sampler_default(void) {
    pass_fresh(); nvn_unimpl_reset(); g_csd_lines = 0; g_csd_line[0] = 0; g_samp_fail_nearest = 0;
    samp_par sp = {0}; sp.ws = 6; sp.magf = NVN_MAG_FILTER_LINEAR; nvn_be_reg_samp(2, &sp);   // 登记号 2：线性、标记 6
    int red0 = g_red_lines;
    // 已登记：原样返回登记的采样器，不记计数、不打首行、不建默认采样器
    int c0 = g_samp_creates;
    nvn_gpu_samp reg = rp_samp_for_const_handle(2, 0, 0, 64);
    CHECK(reg && reg->x == 6 && reg == rp_samp_by_id(2) && g_samp_creates - c0 == 1);
    CHECK(rp_const_samp_default_count() == 0 && g_csd_lines == 0 && g_red_lines == red0);
    // 没登记：同一个默认采样器——最近点 / Repeat / 无比较 / 各向异性 1（同 Ryujinx 的 dummy 采样器），不是登记表里的任何一个
    c0 = g_samp_creates;
    nvn_gpu_samp d = rp_samp_for_const_handle(7, 0, 5, 64);   // 登记号 7 = 采样器号 2 + 池起点 5：越过登记表（只登记了号 2）
    CHECK(d && d == rp_sampler_for_query() && d != reg && g_samp_creates - c0 == 1);
    CHECK(g_samp_last.magf == NVN_MAG_FILTER_NEAREST && g_samp_last.minf == NVN_MIN_FILTER_NEAREST && g_samp_last.ws == NVN_WRAP_MODE_REPEAT &&
          g_samp_last.wt == NVN_WRAP_MODE_REPEAT && g_samp_last.wr == NVN_WRAP_MODE_REPEAT && g_samp_last.cmp_mode == NVN_COMPARE_MODE_NONE && g_samp_last.aniso == 1.0f);
    CHECK(rp_const_samp_default_count() == 1 && g_csd_lines == 1 && strstr(g_csd_line, "采样器号 2（登记号 7，采样器池起点 5、容量 64）"));
    CHECK(nvn_unimpl_kinds() == 0 && g_red_lines == red0);   // 信息，不是未接、不是红行
    // 再来：登记表内但没登记的号 1、越过登记表很远的号、负号——同一个默认采样器，计数跟着加，首行不再打，不再建采样器
    c0 = g_samp_creates;
    CHECK(rp_samp_for_const_handle(1, 0, 0, 64) == d && rp_samp_for_const_handle(100000, 0, 0, 64) == d && rp_samp_for_const_handle(-1, 0, 0, 64) == d);
    CHECK(rp_const_samp_default_count() == 4 && g_csd_lines == 1 && g_samp_creates == c0);
    // 贴图号不影响：采样器没登记就是没登记（tid 登记或没登记都一样）
    CHECK(rp_samp_for_const_handle(9, 12345, 0, 64) == d && rp_const_samp_default_count() == 5);
    // 已登记的号照常，计数不动
    CHECK(rp_samp_for_const_handle(2, 0, 0, 64) == reg && rp_const_samp_default_count() == 5);
    // 登记了但后端建不出（模拟内存不足）：不是「没登记」，不拿默认采样器顶替，返回 NULL（调用方具名跳笔）；计数不动、首行不打；后端的失败照旧记红
    sp.magf = NVN_MAG_FILTER_NEAREST; sp.ws = 5; nvn_be_reg_samp(5, &sp); g_samp_fail_nearest = 1;
    red0 = g_red_lines;
    CHECK(rp_samp_for_const_handle(5, 0, 0, 64) == NULL && rp_const_samp_default_count() == 5 && g_csd_lines == 1);
    CHECK(nvn_unimpl_count("gpu", "samp_create") == 1 && g_red_lines > red0);
    // 默认采样器已建好时，后端之后建采样器失败不影响没登记的号
    CHECK(rp_samp_for_const_handle(9, 0, 0, 64) == d && rp_const_samp_default_count() == 6);
    g_samp_fail_nearest = 0;
    nvn_gpu_samp r5 = rp_samp_for_const_handle(5, 0, 0, 64);   // 后端恢复：登记的号 5 建成，取到的是它、不是默认采样器
    CHECK(r5 && r5->x == 5 && r5 != d && rp_const_samp_default_count() == 6);
    // 默认采样器也建不出：返回 NULL（调用方具名跳笔），不绑别的采样器、不记计数、不打首行；按 query_sampler_create 记未接
    res_fresh(); nvn_unimpl_reset(); sp.magf = NVN_MAG_FILTER_LINEAR; nvn_be_reg_samp(2, &sp);
    CHECK(rp_const_samp_default_count() == 0);
    g_samp_fail_nearest = 1; red0 = g_red_lines; int lines0 = g_csd_lines;
    CHECK(rp_samp_for_const_handle(7, 0, 5, 64) == NULL && rp_const_samp_default_count() == 0 && g_csd_lines == lines0);
    CHECK(nvn_unimpl_count("gpu", "query_sampler_create") == 1 && nvn_unimpl_count("gpu", "samp_create") == 0 && g_red_lines > red0);
    // 后端恢复：同一个号这回取到默认采样器，并按新一局重新打首行
    g_samp_fail_nearest = 0;
    nvn_gpu_samp d2 = rp_samp_for_const_handle(7, 0, 5, 64);
    CHECK(d2 && d2 == rp_sampler_for_query() && rp_const_samp_default_count() == 1 && g_csd_lines == lines0 + 1 && strstr(g_csd_line, "采样器号 2（登记号 7"));
    // 换局：计数清零，首行重来
    res_fresh();
    CHECK(rp_const_samp_default_count() == 0);
    lines0 = g_csd_lines;
    CHECK(rp_samp_for_const_handle(7, 0, 5, 64) != NULL && rp_const_samp_default_count() == 1 && g_csd_lines == lines0 + 1);
    res_fresh(); pass_fresh();
}

// 片 6 的两个调用点（c2 分离表 / 游戏常量缓冲）各自把当前采样器池起点和容量交给首行日志：先画一笔登记着的（不记默认采样器），
//   再把池起点改成 5、容量 64：池下标 3 的登记号变成 8、没登记 → 默认采样器，首行写「采样器号 3（登记号 8，采样器池起点 5、容量 64）」。
static void t_const_sampler_default_sites(void) {
    // c2 形态
    pass_fresh(); g_resolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset(); g_csd_lines = 0; g_csd_line[0] = 0;
    stream init = {0}; q_env(&init);
    tex_t t = mk_tex(7051, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    samp_par sp = {0}; sp.ws = 6; nvn_be_reg_tex(7, &t, NULL); nvn_be_reg_samp(3, &sp);   // 池起点 0 时登记号 3 登记着
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 130; e->count = 1; e->src_reg2 = NVN_GPU_SAMP2_CONST | 3;
    p_bind_handles *b = put(&init, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7;
    q_draw(&init); end(&init); submit1(&init); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6 && rp_const_samp_default_count() == 0 && g_csd_lines == 0);
    rp_S.samp_base = 5; rp_S.samp_pool_n = 64; rp_dirty_all();
    stream moved = {0}; q_draw(&moved); end(&moved); submit1(&moved); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_fb[1][0].u.ts.samp == rp_sampler_for_query() && rp_const_samp_default_count() == 1 && nvn_unimpl_kinds() == 0);
    CHECK(g_csd_lines == 1 && strstr(g_csd_line, "采样器号 3（登记号 8，采样器池起点 5、容量 64）"));
    rp_S.samp_base = 0; rp_S.samp_pool_n = 0;
    nvn_be_tex_release(&t); pass_fresh();
    // 游戏常量缓冲形态
    g_resolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset(); g_csd_lines = 0; g_csd_line[0] = 0;
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    stream s = {0}; q_env(&s);
    tex_t t7 = mk_tex(7052, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    samp_par sp2 = {0}; sp2.ws = 6; nvn_be_reg_tex(7, &t7, NULL); nvn_be_reg_samp(3, &sp2);
    uint32_t tw = 7; memcpy(g_pmem + PG + 0x10, &tw, 4);
    g_fprog.nent[1] = 1; e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEXU; e->src_reg = 2; e->src_off = 0x10; e->src_reg2 = 0; e->count = 1;
    g_fprog.nusrc = 1;
    g_fprog.usrc[0] = (struct rp_usrc){ .tex = { .reg = 2, .off = 0x10 }, .samp_kind = NVN_RB_SAMPI, .samp_reg = 3, .samp_pool = 1 };
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 2; ub->addr = base + PG; ub->size = 64;
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6 && rp_const_samp_default_count() == 0 && g_csd_lines == 0);
    rp_S.samp_base = 5; rp_S.samp_pool_n = 64; rp_dirty_all();
    stream moved2 = {0}; q_draw(&moved2); end(&moved2); submit1(&moved2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_fb[1][0].u.ts.samp == rp_sampler_for_query() && rp_const_samp_default_count() == 1 && nvn_unimpl_kinds() == 0);
    CHECK(g_csd_lines == 1 && strstr(g_csd_line, "采样器号 3（登记号 8，采样器池起点 5、容量 64）"));
    rp_S.samp_base = 0; rp_S.samp_pool_n = 0;
    nvn_be_tex_release(&t7); pass_fresh();
}

// 片 6 的射程对照：Ryujinx 的「空采样器」口径这一片只给类型 3 句柄；别的采样器来源的号没登记，照旧具名跳笔，不绑默认采样器、不记默认采样器计数——
//   组合句柄（句柄高 12 位 = 采样器号，图形）→ sampler_slot_unresolved；纹理数组（图形）→ sampler_slot_unresolved；
//   NVN 采样器槽的分离采样器（计算）→ separate_sampler_unresolved。每一种都先画一笔「采样器号登记着」的当对照（照常画）。
static void t_const_sampler_scope(void) {
    // 图形：组合句柄
    pass_fresh(); g_resolve_rc = g_cresolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset();
    stream init = {0}; q_env(&init);
    tex_t t = mk_tex(7053, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    samp_par sp = {0}; sp.ws = 6; nvn_be_reg_tex(7, &t, NULL); nvn_be_reg_samp(3, &sp);
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 130; e->count = 1; e->src_reg2 = UINT16_MAX;
    p_bind_handles *b = put(&init, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7 | (uint64_t)3 << 20;
    q_draw(&init); end(&init); submit1(&init); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 6);   // 采样器号 3 登记着：照常画
    stream comb = {0}; b = put(&comb, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 130; b->n = 1; b->h[0] = 7 | (uint64_t)777 << 20;
    q_draw(&comb); end(&comb); submit1(&comb); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && nvn_unimpl_count("draw", "sampler_slot_unresolved") == 1);   // 号 777 没登记：具名跳笔
    CHECK(rp_const_samp_default_count() == 0 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 0);
    // 图形：纹理数组（两项，元素 k 取句柄槽 src_reg + k - 4）
    e->src_reg = 4; e->count = 2; rp_dirty_all();
    stream arr_ok = {0}; b = put(&arr_ok, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 0; b->n = 2; b->h[0] = 7 | (uint64_t)3 << 20; b->h[1] = 7 | (uint64_t)3 << 20;
    q_draw(&arr_ok); end(&arr_ok); submit1(&arr_ok); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2);   // 两项的采样器号都登记着：照常画
    stream arr_bad = {0}; b = put(&arr_bad, OP_BIND_TEX, sizeof *b); b->stage = 1; b->first = 0; b->n = 2; b->h[0] = 7 | (uint64_t)3 << 20; b->h[1] = 7 | (uint64_t)777 << 20;
    q_draw(&arr_bad); end(&arr_bad); submit1(&arr_bad); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && nvn_unimpl_count("draw", "sampler_slot_unresolved") == 2);   // 第二项的号 777 没登记：具名跳笔
    CHECK(rp_const_samp_default_count() == 0 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 0);
    // 计算：NVN 采样器槽的分离采样器
    memset(&g_cprog, 0, sizeof g_cprog); g_cprog.is_compute = 1; g_cprog.sh[0] = &g_fsh[0];
    g_cprog.nent[0] = 1; g_cprog.ent[0][0].cls = NVN_GPU_RES_TEX_SAMP;
    g_cprog.ent[0][0].src_kind = NVN_RB_TEX; g_cprog.ent[0][0].src_reg = 130; g_cprog.ent[0][0].count = 1; g_cprog.ent[0][0].src_reg2 = 4;
    int dispatch_before = g_ndisp;
    stream cs_ok = {0}; b = put(&cs_ok, OP_BIND_TEX, sizeof *b); b->stage = 5; b->first = 130; b->n = 1; b->h[0] = 7;
    b = put(&cs_ok, OP_BIND_SAMP, sizeof *b); b->stage = 5; b->first = 4; b->n = 1; b->h[0] = (uint64_t)3 << 20;
    put_dispatch1(&cs_ok); end(&cs_ok); submit1(&cs_ok); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == dispatch_before + 1);   // 采样器号 3 登记着：照常分发
    stream cs_bad = {0}; b = put(&cs_bad, OP_BIND_SAMP, sizeof *b); b->stage = 5; b->first = 4; b->n = 1; b->h[0] = (uint64_t)777 << 20;
    put_dispatch1(&cs_bad); end(&cs_bad); submit1(&cs_bad); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == dispatch_before + 1 && nvn_unimpl_count("draw", "separate_sampler_unresolved") == 1);   // 号 777 没登记：具名跳笔
    CHECK(rp_const_samp_default_count() == 0 && nvn_unimpl_count("draw", "const_sampler_unresolved") == 0);
    nvn_be_tex_release(&t); pass_fresh();
}

// #1594 片 4：TEXU（句柄在游戏常量缓冲里）的贴图号没登记 = Ryujinx TexturePool.GetForBinding 的 null 贴图（(uint)id >= Items.Length 时
//   texture = null，draw 照常提交）：绑声明类型的零贴图、整笔照画，不再 texture_slot_unresolved 跳笔、不报红；信息计数 rp_ubo_zero_count，
//   每局首次一行（带贴图号和池容量）。池内没登记的号走既有的空槽路径（#1147，计数在 rp_empty_slot_zero_count，不进 rp_ubo_zero_count）；
//   已登记的号不受影响；范围只在 TEXU：同一个号经组合槽的取法（rp_view_by_id_decl）越池仍取不到、照旧跳笔。
static void t_ubo_texture_unregistered(void) {
    pass_fresh(); res_fresh(); g_resolve_rc = 0; g_gdraws = 0; nvn_unimpl_reset(); g_ubo_zero_lines = 0; g_ubo_zero_line[0] = 0;
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    stream s = {0}; q_env(&s);
    tex_t t7 = mk_tex(7046, 0x4000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    samp_par sp = {0}; sp.ws = 1; nvn_be_reg_tex(7, &t7, NULL); nvn_be_reg_samp(3, &sp);
    nvn_be_set_tex_pool_n(64);   // 池容量 64：号 7 已登记，号 40 池内没登记，号 0xFFFFF（1048575）越池
    uint32_t tw = 0xFFFFF; memcpy(g_pmem + PG + 0x10, &tw, 4);   // 贴图字：20 位全 1（游戏常拿它当「这一路不用贴图」的哨兵）
    g_fprog.nent[1] = 1; nvn_gpu_layout_ent *e = &g_fprog.ent[1][0];
    e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEXU; e->src_reg = 2; e->src_off = 0x10; e->src_reg2 = 0; e->count = 1;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1; g_fprog.dcls[1][0] = 0;   // 着色器声明二维浮点：零贴图建得出
    g_fprog.nusrc = 1;
    g_fprog.usrc[0] = (struct rp_usrc){ .tex = { .reg = 2, .off = 0x10 }, .samp_kind = NVN_RB_SAMPI, .samp_reg = 3, .samp_pool = 1 };
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 1; ub->index = 2; ub->addr = base + PG; ub->size = 64;
    uint64_t ez0 = rp_empty_slot_zero_count(); int red0 = g_red_lines;
    q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 1 && g_fb[1][0].u.ts.view->of->d.d.format == NVN_FORMAT_RGBA8);   // 越池号：声明类型的零贴图，整笔照画
    CHECK(g_fb[1][0].u.ts.samp && g_fb[1][0].u.ts.samp->x == 1);   // 采样器照常按池下标 3 取
    CHECK(nvn_unimpl_count("draw", "texture_slot_unresolved") == 0 && nvn_unimpl_kinds() == 0 && g_red_lines == red0);
    CHECK(rp_ubo_zero_count() == 1 && rp_empty_slot_zero_count() == ez0);
    CHECK(g_ubo_zero_lines == 1 && strstr(g_ubo_zero_line, "贴图号 1048575") && strstr(g_ubo_zero_line, "容量 64"));
    nvn_gpu_view zero = g_fb[1][0].u.ts.view;
    // 第二笔同一个号：计数 +1，首行不再打，同一张零贴图
    stream again = {0}; q_draw(&again); end(&again); submit1(&again); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2 && g_fb[1][0].u.ts.view == zero && rp_ubo_zero_count() == 2 && g_ubo_zero_lines == 1);
    // 池内没登记的号 40：既有的空槽路径（#1147），同一张零贴图，计数进 rp_empty_slot_zero_count，不进 rp_ubo_zero_count
    tw = 40; memcpy(g_pmem + PG + 0x10, &tw, 4);
    stream inpool = {0}; q_draw(&inpool); end(&inpool); submit1(&inpool); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_fb[1][0].u.ts.view == zero && rp_empty_slot_zero_count() == ez0 + 1 && rp_ubo_zero_count() == 2 && g_ubo_zero_lines == 1);
    // 反例：已登记的号 7 不受影响，不记任何一种零贴图
    tw = 7; memcpy(g_pmem + PG + 0x10, &tw, 4);
    stream reg = {0}; q_draw(&reg); end(&reg); submit1(&reg); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 4 && g_fb[1][0].u.ts.view && g_fb[1][0].u.ts.view->of->d.d.width == 4 && rp_ubo_zero_count() == 2 && rp_empty_slot_zero_count() == ez0 + 1);
    // 池容量未知（没登记池，Items.Length = 0，任何号都越界）：同样是 null 贴图
    nvn_be_set_tex_pool_n(0); tw = 40; memcpy(g_pmem + PG + 0x10, &tw, 4);
    stream nocap = {0}; q_draw(&nocap); end(&nocap); submit1(&nocap); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 5 && g_fb[1][0].u.ts.view == zero && rp_ubo_zero_count() == 3);
    nvn_be_set_tex_pool_n(64);
    // 反例：着色器声明的类型认不出（dtt = 0）造不出声明类型的零贴图：照旧具名跳笔，不乱绑别的贴图
    g_fprog.dtt[1][0] = 0; tw = 0xFFFFF; memcpy(g_pmem + PG + 0x10, &tw, 4); rp_dirty_all();
    stream untyped = {0}; q_draw(&untyped); end(&untyped); submit1(&untyped); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 5 && nvn_unimpl_count("draw", "texture_slot_unresolved") == 1 && rp_ubo_zero_count() == 3);
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
    // 范围对照：同一个号经组合槽的取法（rp_view_by_id_decl，elem = 0）越池仍取不到，照旧由调用方报 texture_slot_unresolved 跳笔；TEXU 的取法拿得到零贴图
    CHECK(rp_view_by_id_decl(1048575, NVN_TEXTURE_TARGET_2D + 1, 0, 0, &g_fake_cmd, NULL) == NULL && rp_ubo_zero_count() == 3);
    CHECK(rp_view_by_id_decl(-1, NVN_TEXTURE_TARGET_2D + 1, 0, 0, &g_fake_cmd, NULL) == NULL);
    CHECK(rp_view_by_ubo_handle_decl(1048575, NVN_TEXTURE_TARGET_2D + 1, 0, &g_fake_cmd, NULL) == zero && rp_ubo_zero_count() == 4);
    CHECK(rp_view_by_ubo_handle_decl(-1, NVN_TEXTURE_TARGET_2D + 1, 0, &g_fake_cmd, NULL) == zero && rp_ubo_zero_count() == 5);   // 负号同样是没登记
    // 换局：计数清零，首行重来（零贴图要在一次提交里建，同 t_decl_view）
    nvn_be_tex_release(&t7);
    nvn_be_sub_begin(); res_fresh();
    CHECK(rp_ubo_zero_count() == 0);
    nvn_be_set_tex_pool_n(64); g_ubo_zero_lines = 0;
    CHECK(rp_view_by_ubo_handle_decl(1048575, NVN_TEXTURE_TARGET_2D + 1, 0, &g_fake_cmd, NULL) != NULL && rp_ubo_zero_count() == 1 && g_ubo_zero_lines == 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_set_tex_pool_n(0); res_fresh(); pass_fresh();
}

// Registered views must obey the same encode-window lifetime as cached attachment
// views and samplers, including re-registration and nested windows.
static void t_registered_view_pin(void) {
    res_fresh();
    tex_t a = mk_tex(81001, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    tex_t b = mk_tex(81002, 4096, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    nvn_be_tex_init(&a); nvn_be_tex_init(&b);
    nvn_be_reg_tex(3, &a, NULL);
    rp_view_pin_begin();
    nvn_gpu_view first = rp_view_by_id(3, &g_fake_cmd, NULL);
    CHECK(first != NULL); int first_id = first ? first->rid : 0;
    rp_view_pin_begin();
    nvn_be_reg_tex(3, &b, NULL);
    CHECK(!was_view_released(first_id));
    nvn_gpu_view second = rp_view_by_id(3, &g_fake_cmd, NULL);
    CHECK(second != NULL); int second_id = second ? second->rid : 0;
    nvn_be_unreg_tex_range(3, 1);
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == NULL);
    CHECK(!was_view_released(second_id));
    rp_view_pin_end();
    CHECK(!was_view_released(first_id) && !was_view_released(second_id));
    rp_view_pin_end();
    CHECK(was_view_released(first_id) && was_view_released(second_id));
    res_fresh();
}

// 视图表按存储认视图：序号销毁时视图跟着存储留在表里（存储还在共用表里，下一个同描述的新序号接着用，见 t_view_storage_key）；
// 存储真放掉（内存池 Finalize）时名下的视图一并放，窗口内已交出的要等窗口结束。
static void t_invalidated_attachment_pin(void) {
    res_fresh();
    tex_t t = mk_tex(82001, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    nvn_be_tex_init(&t);
    rp_view_pin_begin();
    nvn_gpu_view v = rp_view_for(&t, NULL, &g_fake_cmd);
    CHECK(v != NULL); int id = v ? v->rid : 0;
    rp_view_pin_begin();
    nvn_be_tex_release(&t);
    CHECK(!was_view_released(id));
    nvn_be_alias_drop_pool(&g_tp);   // 存储放掉：视图要放，但窗口内已交出的钉到窗口结束
    CHECK(!was_view_released(id));
    rp_view_pin_end();
    CHECK(!was_view_released(id));
    rp_view_pin_end();
    CHECK(was_view_released(id));
    v = rp_view_for(&t, NULL, &g_fake_cmd);
    CHECK(v != NULL); id = v ? v->rid : 0;
    nvn_be_tex_release(&t);
    nvn_be_alias_drop_pool(&g_tp);   // 窗口外：当场放
    CHECK(was_view_released(id));
    res_fresh();
}

// 贴图号登记表（g_treg）上的视图随存储一起放（rp_res.c tex_free）。真机（01007EF00011E000 读档）：方形 8 层二维数组（1024x1024x8 R16）
// 带上 CUBE_COMPATIBLE 后立方体视图建得出来，登记号上的视图就建在这些存储上；对象 Finalize、内存池 Finalize 放掉存储之后，
// 登记表那一项的视图还拿着后端贴图（后端只记 released），110 张、2.36 GB 拆不掉，进程被系统按 GPU 显存上限杀掉。
// 后端替身按真后端的引用语义（还有视图就只记 released，g_tex_held 计数）；期望值由用例自己的调用序列直接得出。
static tex_t mk_cube_arr(uint64_t serial, int64_t off) {
    tex_t t = mk_tex(serial, off, NVN_FORMAT_R16, 64, 64, 4, 0); t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.depth = 8; nvn_tex_layout(&t.d, &t.L); return t;
}
static void t_registered_view_freed_with_storage(void) {
    res_fresh();
    const int saved_nrel = g_nrel; g_nrel = 0;   // 「视图放掉」记录表从空记起（同 t_view_pin），结束时恢复
    const int h0 = g_tex_held;
    view_t cube; memset(&cube, 0, sizeof cube); cube.set_target = 1; cube.target = NVN_TEXTURE_TARGET_CUBEMAP; cube.set_layers = 1; cube.nlayers = 6;
    // ① 共用存储（GPU 内容）：号 5 登记立方体视图、号 6 登记整图；两号各取一次，号 6 再按立方体声明取一次（按目标取视图的表 g_vf）
    tex_t a = mk_cube_arr(83001, 0); nvn_be_tex_init(&a);
    nvn_be_reg_tex(5, &a, &cube); nvn_be_reg_tex(6, &a, NULL);
    nvn_gpu_view v5 = rp_view_by_id(5, &g_fake_cmd, NULL), v6 = rp_view_by_id(6, &g_fake_cmd, NULL);
    nvn_gpu_view vd = rp_view_by_id_decl(6, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 0, &g_fake_cmd, NULL);
    CHECK(v5 && v6 && vd && v5->of == v6->of && vd->of == v5->of && v5->of->views == 3);
    const int id5 = v5 ? v5->rid : 0, id6 = v6 ? v6->rid : 0, idd = vd ? vd->rid : 0, r0 = g_tex_releases;
    nvn_be_tex_release(&a);   // 对照：对象 Finalize，存储还在共用表里（内存池没放）→ 后端贴图不放、视图都不放
    CHECK(g_tex_releases == r0 && !was_view_released(id5) && !was_view_released(id6) && !was_view_released(idd));
    nvn_be_alias_drop_pool(&g_tp);   // 内存池 Finalize：存储放掉，登记号 5 / 6 不重登记（游戏不清池里的描述符）
    CHECK(g_tex_releases == r0 + 1);
    CHECK(was_view_released(idd));                               // g_vf 的项原来就放（对照：这条修之前也绿）
    CHECK(was_view_released(id5) && was_view_released(id6));     // 登记表的项也要放（修之前红）
    CHECK(g_tex_held == h0);                                     // 后端贴图真拆了（修之前 h0 + 1）
    // ② 窗口内已交出的登记视图：存储放掉时钉到窗口结束（不能当场放，后端还没拿到引用），窗口一结束后端贴图跟着拆
    tex_t b = mk_cube_arr(83002, 0); nvn_be_tex_init(&b);
    nvn_be_reg_tex(5, &b, &cube);
    rp_view_pin_begin();
    nvn_gpu_view w = rp_view_by_id(5, &g_fake_cmd, NULL); CHECK(w != NULL); const int idw = w ? w->rid : 0;
    nvn_be_tex_release(&b); nvn_be_alias_drop_pool(&g_tp);
    CHECK(!was_view_released(idw) && g_tex_held == h0 + 1);     // 对照：窗口内拿着 → 悬着是对的
    rp_view_pin_end();
    CHECK(was_view_released(idw) && g_tex_held == h0);
    // ③ CPU 内容的存储（不进共用表）：对象 Finalize 就放存储（tex_unref），登记视图一并放
    tex_t c = mk_cube_arr(83003, 0x10000); nvn_be_tex_init(&c); nvn_be_tex_cpu_written(&c);
    nvn_be_reg_tex(7, &c, &cube);
    nvn_gpu_view u = rp_view_by_id(7, &g_fake_cmd, NULL); CHECK(u != NULL); const int idu = u ? u->rid : 0;
    const int r1 = g_tex_releases;
    nvn_be_tex_release(&c);
    CHECK(g_tex_releases == r1 + 1 && was_view_released(idu) && g_tex_held == h0);
    // ④ 存储放掉后同一个号重登记一张新图：取到的是新图上的视图（不是旧视图），旧的不再被放第二次
    tex_t d = mk_cube_arr(83004, 0); nvn_be_tex_init(&d);
    nvn_be_reg_tex(6, &d, NULL);
    const int vr0 = g_view_releases;
    nvn_gpu_view v6b = rp_view_by_id(6, &g_fake_cmd, NULL);
    CHECK(v6b != NULL && v6b->rid != id6 && g_view_releases == vr0);
    nvn_be_tex_release(&d); nvn_be_alias_drop_pool(&g_tp);
    CHECK(g_tex_held == h0);
    nvn_be_unreg_tex_range(0, 16);
    g_nrel = saved_nrel;
    res_fresh();
}

static void t_sync_reuse_after_gpu_write(void) {
    g_flag_sync_lag = 1; g_buf_cpu_auto = 1;
    uint64_t pf[RP_PERF_N]; rp_perf_take(pf);   // 清掉前面用例的计数：下面每次真等都按回写类别核一遍（RP_PERF_WAIT_EFX_*）
#define EFX_IS(ts, fx, gw, later) (rp_perf_take(pf), pf[RP_PERF_WAIT_EFX_TS] == (ts) && pf[RP_PERF_WAIT_EFX_FX] == (fx) && pf[RP_PERF_WAIT_EFX_GW] == (gw) && pf[RP_PERF_WAIT_EFX_LATER] == (later))
    memset(g_pmem, 0x11, sizeof g_pmem);
    uint64_t addr = (uint64_t)(uintptr_t)g_pmem;
    nvn_be_pool_add(g_pmem, sizeof g_pmem); nvn_be_sub_begin();
    nvn_gpu_buf b = NULL; uint64_t off = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, addr, 16, &b, &off));
    CHECK(rp_post_gpu_write(addr, 16)); memset(b->cpu + off, 0x22, 16);
    nvn_be_sub_commit(); uint64_t f = nvn_be_last_fence();
    // A successful wait grants CPU reuse. GPU writeback must have landed first.
    CHECK(nvn_be_wait(f, 0) == 0);
    CHECK(EFX_IS(0, 0, 1, 0));   // 挡住这次放行的是显卡写缓冲
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(nvn_be_wait(f, 0) == 1 && g_pmem[0] == 0x22);
    memset(g_pmem, 0x33, 16);
    // A later read-only submission can retain the existing overlap path.
    nvn_be_sub_begin(); nvn_be_sub_commit();
    CHECK(nvn_be_wait(nvn_be_last_fence(), 0) == 1);
    fake_complete(NVN_GPU_DONE_OK); CHECK(g_pmem[0] == 0x33);
    // A queued event is also observable CPU memory, not a read-only submission.
    volatile uint32_t ev = 0; stream a = {0};
    uint8_t *r = put(&a, OP_SIGNAL_EVENT, 16); volatile uint32_t *pv = &ev; uint32_t mode = 0, value = 7;
    memcpy(r, &pv, 8); memcpy(r+8, &mode, 4); memcpy(r+12, &value, 4); end(&a);
    rp_perf_take(pf); submit1(&a); CHECK(nvn_be_wait(nvn_be_last_fence(), 0) == 0);
    CHECK(EFX_IS(0, 1, 0, 0));   // 事件
    fake_complete(NVN_GPU_DONE_OK); CHECK(ev == 7);
    uint64_t ts[2] = {0, 0}; stream t = {0}; g_tick = 7777;
    q_counter(&t, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_TIMESTAMP, ts); end(&t);
    rp_perf_take(pf); submit1(&t); CHECK(nvn_be_wait(nvn_be_last_fence(), 0) == 0);
    CHECK(EFX_IS(1, 0, 0, 0));   // 只有时间戳
    fake_complete(NVN_GPU_DONE_OK); CHECK(ts[1] == 7777);
    // #653 capability path: report-position samples still write only before wait publishes success.
    g_caps_extra = NVN_GPU_CAP_REPORT_TIMESTAMP; g_ts_calls = 0; ts[0] = ts[1] = 999;
    submit1(&t); CHECK(nvn_be_wait(nvn_be_last_fence(), 0) == 0 && ts[1] == 999);
    fake_complete(NVN_GPU_DONE_OK); CHECK(nvn_be_wait(nvn_be_last_fence(), 0) == 1 && ts[1] == 110);
    g_caps_extra = 0;
    // 所等的是只读提交，之后又交了一次带时间戳的：照旧真等（放行判定不变），记「只有之后的提交带回写」
    nvn_be_sub_begin(); nvn_be_sub_commit(); const uint64_t fr = nvn_be_last_fence();
    rp_perf_take(pf); submit1(&t); CHECK(nvn_be_wait(fr, 0) == 0);
    CHECK(EFX_IS(0, 0, 0, 1));
    fake_complete(NVN_GPU_DONE_OK);
#undef EFX_IS
    nvn_be_pool_del(g_pmem); g_buf_cpu_auto = 0;
}

// ---------- 「分时」按游戏申请的帧率分段（rp_fps_seg.h）----------
// 一个窗口里 60 → 30 → 60：各段的目标、帧数、实际帧率、达标比例、慢帧计数都按手算的对；空窗口 0 值也打。时间都是整纳秒，期望值手算：
//   段 1 间隔 1：100.000 秒起每 1/60 秒一帧共 100 帧，再隔 120 毫秒一帧（101 帧；慢帧 1.5 倍 1、100 毫秒 1）；101.800 秒游戏设间隔 2
//   段 2 间隔 2：101.800 起每 1/30 秒一帧 30 帧（第一帧离上一帧 63 毫秒 > 50），再隔 60 毫秒一帧（31 帧；1.5 倍 2、100 毫秒 0）；102.900 秒设回 1
//   段 3 间隔 1：102.900 起每 1/60 秒一帧 60 帧（第一帧离上一帧 57 毫秒 > 25；1.5 倍 1），收尾在 103.900 秒
//   实际帧率：101 / 1.8 = 56.1（达标 94%），31 / 1.1 = 28.2（94%），60 / 1.0 = 60.0（100%）
static char g_fseg_lines[6][400]; static int g_fseg_n;
static void fseg_cap(const char *l) { if (g_fseg_n < 6) snprintf(g_fseg_lines[g_fseg_n], 400, "%s", l); g_fseg_n++; }
static void t_fps_target_segments(void) {
    struct fseg_win w; memset(&w, 0, sizeof w); g_fseg_n = 0;
    const uint64_t S = 1000000000ull, T0 = 100 * S;
    uint64_t t = 0;
    for (uint64_t k = 0; k < 100; k++) { t = T0 + k * S / 60; fseg_frame(&w, t, 1, 0); }
    t += 120000000ull; fseg_frame(&w, t, 1, 0);
    const uint64_t set2 = T0 + 1800000000ull, set3 = T0 + 2900000000ull;
    for (uint64_t j = 1; j <= 30; j++) { t = set2 + j * S / 30; fseg_frame(&w, t, 2, set2); }
    t += 60000000ull; fseg_frame(&w, t, 2, set2);
    for (uint64_t m = 1; m <= 60; m++) { t = set3 + m * S / 60; fseg_frame(&w, t, 1, set3); }
    CHECK(w.n == 3 && w.s[0].frames == 101 && w.s[1].frames == 31 && w.s[2].frames == 60);
    CHECK(w.s[1].t0 == set2 && w.s[2].t0 == set3);                       // 断段在游戏设间隔的时刻，不在下一帧
    fseg_emit(&w, t, fseg_cap);
    CHECK(g_fseg_n == 3);
    CHECK(strstr(g_fseg_lines[0], "第 1/3 段 100.000–101.800 秒（1.80 秒）：游戏申请交换间隔 1（目标 60.0 帧/秒、帧间隔 16.7 毫秒）；上屏 101 帧、实际 56.1 帧/秒、达标 94%；超过目标间隔 1.5 倍 1 帧、超过 100 毫秒 1 帧") != NULL);
    CHECK(strstr(g_fseg_lines[1], "第 2/3 段 101.800–102.900 秒（1.10 秒）：游戏申请交换间隔 2（目标 30.0 帧/秒、帧间隔 33.3 毫秒）；上屏 31 帧、实际 28.2 帧/秒、达标 94%；超过目标间隔 1.5 倍 2 帧、超过 100 毫秒 0 帧") != NULL);
    CHECK(strstr(g_fseg_lines[2], "第 3/3 段 102.900–103.900 秒（1.00 秒）：游戏申请交换间隔 1（目标 60.0 帧/秒、帧间隔 16.7 毫秒）；上屏 60 帧、实际 60.0 帧/秒、达标 100%；超过目标间隔 1.5 倍 1 帧、超过 100 毫秒 0 帧") != NULL);
    // 下一窗口沿用最后的间隔；一帧没上屏也打一行（0 值）
    CHECK(w.n == 1 && w.s[0].itv == 1 && w.s[0].frames == 0 && w.s[0].t0 == t);
    fseg_emit(&w, t + 5 * S, fseg_cap);
    CHECK(g_fseg_n == 4 && strstr(g_fseg_lines[3], "第 1/1 段") && strstr(g_fseg_lines[3], "上屏 0 帧、实际 0.0 帧/秒、达标 0%；超过目标间隔 1.5 倍 0 帧、超过 100 毫秒 0 帧"));
    // 设的时刻不在当前段里（早于段起点）：断在这一帧
    memset(&w, 0, sizeof w); fseg_frame(&w, T0, 1, 0); fseg_frame(&w, T0 + S, 2, T0 - S);
    CHECK(w.n == 2 && w.s[1].t0 == T0 + S);
    g_fseg_n = 0; fseg_emit(&w, T0 + 2 * S, fseg_cap);
    CHECK(g_fseg_n == 2 && w.n == 1 && w.s[0].itv == 2);             // 窗口收在间隔 2：下一窗口从间隔 2 起（不回到 1）
    nvn_be_present_target(1, 0);                                         // 回放核心接了前端的钩子（链得上即可；上屏节拍不受它影响）
}

// ---------- 池查找统计（rp_present.c rp_pool_stats_*，开关 pool_stats；#1368 读数）----------
// 契约：关着一行不打；开着每 1024 帧一行（0 值也打）、会话收尾一行 why=end。各字段等于对应的累计读数：
// fast_hits = rp_pool_fast_hits()，memo_hits / lookups = rp_pool_memo_stats，span_index_hits = nvn_dirty_span_stats，
// submits = 开着以来的队列提交次数。fast_hits 与 lookups 是两个数：夹具先关备忘查一批（只走快路，不计 lookups）再开备忘查一批，让两者不等。
static uint64_t ps_field(const char *line, const char *k) { const char *q = strstr(line, k); return q ? strtoull(q + strlen(k), NULL, 10) : ~0ull; }
static void t_pool_stats(void) {
    static uint8_t mem[0x1000]; const uint64_t b = (uint64_t)(uintptr_t)mem;
    g_ps_n = 0; g_flag_pool_stats = 0;
    for (int i = 0; i < 2048; i++) nvn_be_present(NULL, 0, NULL);
    CHECK(g_ps_n == 0);                                    // 关着：0 行
    nvn_be_pool_add(mem, sizeof mem);
    pm_flag(0); for (int i = 0; i < 64; i++) rp_pool_find(b + 0x10, 0x10, NULL, NULL);
    pm_flag(1); for (int i = 0; i < 8; i++) rp_pool_find(b + 0x20, 0x10, NULL, NULL);
    g_flag_pool_stats = 1;
    for (int i = 0; i < 1024; i++) nvn_be_present(NULL, 0, NULL);
    CHECK(g_ps_n == 1);                                    // 开着：第 1024 帧一行，这时还没有回放，0 值也打
    const char *l = g_ps_lines[0];
    CHECK(strncmp(l, "S2A_POOLSTATS v=1 why=frame frames=1024 ", 40) == 0);
    CHECK(ps_field(l, " submits=") == 0 && ps_field(l, " exec_ns=") == 0);
    uint64_t mh = 0, ml = 0, xh = 0, xs = 0, xn = 0; rp_pool_memo_stats(&mh, &ml); nvn_dirty_span_stats(&xh, &xs, &xn);
    CHECK(ps_field(l, " memo_hits=") == mh && ps_field(l, " lookups=") == ml && ps_field(l, " span_index_hits=") == xh);
    CHECK(rp_pool_fast_hits() != ml);                      // 前提：两个数确实不同（不然下面那条分不出混没混）
    CHECK(ps_field(l, " fast_hits=") == rp_pool_fast_hits());
    stream a = {0}; end(&a); submit1(&a); submit1(&a);
    rp_pool_stats_end();
    CHECK(g_ps_n == 2 && strncmp(g_ps_lines[1], "S2A_POOLSTATS v=1 why=end frames=1024 ", 38) == 0 && ps_field(g_ps_lines[1], " submits=") == 2);
    g_flag_pool_stats = 0;
    for (int i = 0; i < 1024; i++) nvn_be_present(NULL, 0, NULL);
    CHECK(g_ps_n == 2);                                    // 关回去：不再打
    nvn_be_pool_del(mem);
}

// 长帧日志的帧级账（rp_frame_take）：累计计数改成「与上次取时相减」后，取走的仍是本帧的量（取完再取为 0，不跨帧累加）。
static void t_frame_take_delta(void) {
    uint64_t ns = 0, dr = 0, sb = 0, tn = 0, ts = 0;
    rp_frame_take(&ns, &dr, &sb);                          // 先清掉之前用例留下的账
    stream a = {0}; end(&a); submit1(&a); submit1(&a);
    rp_frame_take(&ns, &dr, &sb); CHECK(sb == 2 && dr == 0);
    rp_frame_take(&ns, &dr, &sb); CHECK(sb == 0 && dr == 0 && ns == 0);   // 取过就是 0：不再把上一帧算进来
    submit1(&a);
    rp_frame_take(&ns, &dr, &sb); CHECK(sb == 1);
    rp_exec_totals(&tn, &ts); CHECK(ts >= 3);              // 累计只增：至少有本例这 3 次
}

// 会话收尾那一行走真实收尾路径（nvn_be_shutdown_session），不直接调 rp_pool_stats_end：收尾里漏接这一调用时本例必红。
// 收尾会把回放核心整份复位，所以放在 main 的最后、所有别的用例之后。
static void t_pool_stats_shutdown(void) {
    g_ps_n = 0; g_flag_pool_stats = 1;
    nvn_be_present(NULL, 0, NULL);                         // 帧边界：打开
    stream a = {0}; end(&a); submit1(&a);
    CHECK(g_ps_n == 0);                                    // 还没到 1024 帧：没有行
    CHECK(nvn_be_shutdown_session() == 0);
    CHECK(g_ps_n == 1 && strncmp(g_ps_lines[0], "S2A_POOLSTATS v=1 why=end frames=1 ", 35) == 0 && ps_field(g_ps_lines[0], " submits=") == 1);
    g_flag_pool_stats = 0;
    CHECK(nvn_be_shutdown_session() == 0 && g_ps_n == 1);  // 收尾后已关：再收尾不重复打
}

static int g_block_buf_resolve;   // 用例替身：非 0 时让 rp_buf_resolve 一律失败（构造「准备阶段给不出镜像」）
int rp_test_buf_resolve_block(void) { return g_block_buf_resolve; }

static void t_graphics_cbuf_prep_before_pass(void) {
    // #2100：图形阶段常量缓冲「有 GPU 待写回」的镜像解析必须落在准备阶段（开通道之前）。
    // 构造真机那条路：上一段提交用过镜像、GPU 还没跑完；这一段在通道里换常量缓冲再画一笔。
    // 解析若发生在发绑定之后，同一次提交里的第二次同步要录成传输（rp_xfer_cmd 结束当前通道），
    // 绑定与绘制就落在通道外——真机表现是整屏黑、每帧 2 笔重复帧。
    pass_fresh(); memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    const uint64_t base = (uint64_t)(uintptr_t)g_pmem, ub = base + PG;
    memset(g_pmem + PG, 0x11, 512);
    prog_t pg; memset(&pg, 0, sizeof pg); pg.magic = MAGIC('P','R','O','G'); pg.serial = 2201; pg.nstage = 2;
    g_snap_prog = &pg; g_snap_ok = 3; memset(g_snap_keys, 0, sizeof g_snap_keys); memset(g_snap_stage, 0xff, sizeof g_snap_stage);
    for (int i = 0; i < 2; i++) { g_snap_stage[i] = (uint8_t)i; g_snap_keys[i][0] = (uint8_t)(i + 0x41); }
    uint64_t key = 0;
    CHECK(rp_buf_gpu_write_note(ub, 256, &key) != 0);
    CHECK(rp_buf_gpu_pending(ub, 256));
    {   stream a = {0}; q_env(&a);   // 第一段：镜像被用上，GPU 还没跑完
        g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_CBUF;
        p_bind_program_id *id = put(&a, OP_BIND_PROGRAM, sizeof *id); id->prog = &pg; id->serial = pg.serial; id->stages = 0x1f;
        p_bind_buf *b = put(&a, OP_BIND_UBO, sizeof *b); b->stage = 1; b->index = 0; b->addr = ub; b->size = 256;
        q_draw(&a); end(&a); submit1(&a); }
    g_gdraws = 0; g_bind_outside = 0; g_draw_outside = 0; g_pass_ends = 0; g_arena_used = 0;
    {   stream s = {0}; q_env(&s);   // 第二段：通道里换常量缓冲
        g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_CBUF;
        p_bind_program_id *id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pg; id->serial = pg.serial; id->stages = 0x1f;
        q_draw(&s);
        p_bind_buf *b = put(&s, OP_BIND_UBO, sizeof *b); b->stage = 1; b->index = 0; b->addr = ub; b->size = 256;
        q_draw(&s); end(&s);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK); }
    CHECK(g_bind_outside == 0);                     // 没有任何绑定落在通道外（修前这里是 1）
    CHECK(g_draw_outside == 0);
    CHECK(g_gdraws == 2);
    CHECK(nvn_unimpl_count("draw", "uniform_gpu_pending_unprepared") == 0);   // 准备阶段给了镜像

    // 兜底：准备阶段解析失败（替身钩子）时，图形阶段不许在通道内解析，只能暂存 + 具名记账。
    // 修前的写法会在这里现场解析，兜底场景就没有这笔账了。
    {   g_gdraws = 0; g_bind_outside = 0; g_draw_outside = 0; g_pass_ends = 0; g_arena_used = 0;
        pass_fresh(); memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
        memset(g_pmem + PG, 0x11, 512);
        uint64_t k2 = 0; CHECK(rp_buf_gpu_write_note(ub, 256, &k2) != 0);
        stream s = {0}; q_env(&s);
        g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][0].src_kind = NVN_RB_CBUF;
        p_bind_program_id *id = put(&s, OP_BIND_PROGRAM, sizeof *id); id->prog = &pg; id->serial = pg.serial; id->stages = 0x1f;
        q_draw(&s);   // 让通道先开着
        p_bind_buf *b = put(&s, OP_BIND_UBO, sizeof *b); b->stage = 1; b->index = 0; b->addr = ub; b->size = 256;
        q_draw(&s); end(&s);
        g_block_buf_resolve = 1;
        submit1(&s);
        g_block_buf_resolve = 0;
        fake_complete(NVN_GPU_DONE_OK);
        CHECK(nvn_unimpl_count("draw", "uniform_gpu_pending_unprepared") == 1);   // 具名记账：图形阶段不现场解析
        CHECK(g_bind_outside == 0 && g_draw_outside == 0); }

    g_snap_prog = NULL; pass_fresh();
}


int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--dirtylog-off")) { g_dirtylog_dflt = g_flag_buf_dirtylog = 0; argc--; argv++; }   // 关那一档：整套用例走逐页退回路径
#ifdef S2A_GPU_LAND_TEST
    if (argc > 1 && !strcmp(argv[1], "--land-noresync")) { t_land_noresync(); printf("land noresync: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
    if (argc > 1 && !strcmp(argv[1], "--gpu-land-count")) { t_gpu_land_count_cleanup(); printf("gpu land count: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
    if (argc > 1 && !strcmp(argv[1], "--gpu-land-destroy")) { t_gpu_land_destroy(); printf("gpu land destroy: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
    if (argc > 1 && !strcmp(argv[1], "--gpu-land")) { t_gpu_land_concurrent(); printf("gpu land: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
    if (argc > 1 && !strcmp(argv[1], "--gpu-land-merge")) { t_gpu_land_merge(); printf("gpu land merge: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
#endif
    if (argc > 1 && !strcmp(argv[1], "--cbuf-device-range")) { t_cbuf_device_range(); printf("CBUF range: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
    if (argc > 1 && !strcmp(argv[1], "--sync-reuse")) { t_sync_reuse_after_gpu_write(); printf("sync reuse: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
    if (argc > 1 && !strcmp(argv[1], "--mirror-disjoint")) { t_mirror_disjoint(); printf("mirror disjoint: %d checks, %d failures\n", g_checks, g_fail); return g_fail != 0; }
    t_invalidated_attachment_pin();
    t_registered_view_pin();
    t_registered_view_freed_with_storage();
    t_state_persists();
    t_unimpl_names();
    t_one_commit_per_submit();
    t_submit_compile_lookahead();
    t_event_after_done();
    t_fence_sync();
    t_jump_call(); t_call_reused();
    t_bad_record();
    t_dirty_bits();
    t_deferred_bind();
    t_program_stages();
    t_program_bind_stages();
    // 一行一个调用：外部各支都往这里加用例，挤在一行时每两支必冲突
    t_pools();
    t_pool_memo();
    t_pool_nested();
    t_tex_init_registers_only();
    t_alias();
    t_alias_take_over();
    t_read_texels_guest();
    t_read_texels_gpu_owned();
    t_read_texels_gpu();
    t_create_stats();
    t_cpu_upload();
    t_gpu_deswizzle_route();
    t_gpu_deswizzle_pending();
    t_gpu_deswizzle_backend_fail();
    t_tex_reupload_diag();
    t_tex_reupload_diag_gpu_pending();
    t_cpu_rules();
    t_linear_upload();
    t_linear_reject();
    t_registry();
    t_pool_table();
#ifdef S2A_TEST_TREG_SCRUB
    t_view_snap_grow();
#endif
    t_samp_mirror_clamp();
    t_decl_view();
    t_decl_view_1d();
    t_decl_array_numeric();
    t_stencil_sample_mode();
#ifdef S2A_GPU_LAND_TEST
    t_gpu_land_concurrent();
    t_gpu_land_destroy();
    t_land_noresync();
    t_gpu_land_count_cleanup();
    t_gpu_land_merge();
#endif
    t_pool_gone_hold();
    t_buf_mirror();
    t_buf_dirtylog();
    t_buf_untracked_once();
    t_buf_mirror_fail_once();
    t_buf_multi_tail();
    t_buf_ops();
    t_copy_b2t();
    t_pass_shared_extent();
    t_pass_shared_pending_clear();
    t_pass_shared_explicit_rect();
    t_pass_area_guards();
    t_pass_clear_fold();
    t_draw_tex();
    t_pass_depth_shadow();
    t_pass_no_attachment();
    t_pass_bc_shadow();
    t_pass_bc_shadow_3d();
    t_autoflush();
    t_split_long_pass();
    t_pass_fresh_zero();
    t_frame_overlap();
    t_tex_fact_gens();
    t_pass_target_persists();
    t_pass_switch_and_partial();
    t_pass_rt_continue();
    t_pass_zero_mask();
    t_pass_state_keep();
    t_pass_clear_park();
    t_pass_clear_park_tail();
    t_pass_clear_only_attachments();
    t_pass_area_then_clear_only();
    t_pass_clear_park_read();
    t_pass_clear_park_sample();
    t_drawlog_unreadable_tex_mem();
    t_fmt_filter_blend();
    t_clear_rect();
    t_report_counter();
    t_report_position();
    t_occlusion();
    t_occl_first_reset_cap();
    t_msoff();
    t_render_cond();
    t_dispatch();
    t_dispatch_barrier();
    t_dispatch_indirect_gpu();
    t_dispatch_ubop_rw(); t_bufops_gpu_order();
    t_cbuf_device_range();
    t_dispatch_spill();
    t_dispatch_ubop_ro_skip();
    t_compute_image();
    t_compute_image_class();
    t_compute_image_lib_fallback();
    t_compute_tex_lib_fallback();
    t_draw_tex_lib_fallback(NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, 0, 0, 0);
    t_draw_tex_lib_fallback(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8UI, 0, 1, 0);
    t_draw_tex_lib_fallback(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8I, 1, 2, 0);
    t_draw_tex_lib_fallback(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 2, 0, 0);
    t_draw_tex_lib_fallback(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8UI, 1, 1, 0);
    t_draw_tex_lib_fallback(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, 3, 3, 0);
    t_draw_tex_lib_fallback(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8UI, 0, 1, 1);
    t_graphics_image();
    t_compute_tex_from_image();
    t_drv2_table();
    t_compute_query_sampler();
    t_compute_pool();
    t_compute_sources();
    t_depth_copy_scaled(); t_front_origin_matrix(); t_color_copy_mirror(); t_copy_t2t();
    t_clean_read_order();
    t_clean_read_scopes();
    t_mirror_disjoint();
    t_pipeline_dirty_changes();
    t_target_format_variant();
    t_draw();
    t_arena697_detail();
    t_logic_op_accounting();
    t_pipe_fail_table();
    t_gs_dirty_and_topology();
    t_gs_bind(); t_ub_fill_async(); t_ub_fill_steal_all(); t_storage_null_binding(); t_storage_sync_before_pass(); t_stage_dropped_clears_binds(); t_sbuf_window(); t_sbuf_past_pool_end();
    t_graphics_high_slots();
    t_graphics_pool();
    t_tbuf_replay();
    t_image_tbuf_replay();
    t_decl_sampling();
    t_separate_sampler();
    t_const_sampler();
    t_ubo_texture();
    t_ubo_const_sampler();
    t_cs_ubo_const_sampler();
    t_const_sampler_default();
    t_const_sampler_default_sites();
    t_const_sampler_scope();
    t_ubo_texture_unregistered();
    t_present_thread_cpu();
    t_present(); t_present_adpf();
    { uint64_t tc[4]; rp_present_tc_take(tc); CHECK(tc[2] + tc[3] >= 1); }   // 真上屏路径接上了：t_present 里多次成功上屏至少配出一个样本
    t_framelog_fence();
    t_view_pin();
    t_view_storage_key();
    t_view_storage_fields();
    t_view_storage_sweep();
    t_view_storage_pixels();
    t_pass_3d_slice_view();
    t_pass_layered();
    t_shader_write_barrier_split();
    t_draw_c1_full();
    t_bind_reuse();
    t_begin_fail();
    t_fps_target_segments();
    t_pool_stats();
    t_frame_take_delta();
    t_graphics_cbuf_prep_before_pass();   // 图形阶段常量缓冲的 GPU 待写回必须在开通道前解析（本 PR）
    if (argc > 1 && t_table(argv[1])) g_fail++;
    t_tex_linear_dirty_rows();   // 放在倒数第二：它推高全局上传计数，前面有用例按「从 0 起」断言
    t_pool_stats_shutdown();   // 会话收尾会复位整个回放核心：必须是最后一个用例（不看上传计数）
    CHECK(g_dirtylog_dflt != 0 ? g_alog_events > 0 && g_alog_reads > 0 : g_alog_events == 0 && g_alog_reads == 0);   // 两档各走各的路：开那一档受跟踪的池真读了日志，关那一档一次也没开过
    printf("t_nvn_replay 脏页日志档：%s，自动日志读 %llu 次、事件 %llu 条\n", g_dirtylog_dflt != 0 ? "缺省（开）" : "关", (unsigned long long)g_alog_reads, (unsigned long long)g_alog_events);
    printf("t_nvn_replay：%d 项检查，%d 项红\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
