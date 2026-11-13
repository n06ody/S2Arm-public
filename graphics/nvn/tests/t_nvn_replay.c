// 回放核心（graphics/nvn/replay）的宿主单测：纯 C，后端与平台件都是本文件里的替身，不需要 GPU。
// 期望值都由用例自己写下的输入直接得出，不经被测实现计算。
#include "nvn_obj.h"
#include "nvn2_cmd.h"
#include "nvn_backend.h"
#include "nvn_mtl.h"
#include "../replay/rp_state.h"
#include "../replay/rp_exec.h"
#include "../replay/rp_res.h"
#include "../replay/rp_shader.h"
#include "nvn_refl.h"
#include "nvn_dirty.h"
#include "nvn_deswizzle.h"
#include "../replay/nvn_unimpl.h"
#include "../replay/nvn_plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("【红】 %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------- 平台替身 ----------
static int g_red_lines; static char g_last_red[200];
static int g_arena_truncated_lines;
static int g_arena_detail_lines; static char g_arena_detail[256]; static char g_arena697_lines[32][256];
void nvn_plat_log(int level, const char *cls, const char *text) { (void)cls; if(level == NVN_PLAT_LOG_RED && strstr(text,"后续不同原因详情省略（最多8）"))g_arena_truncated_lines++; if (level == NVN_PLAT_LOG_RED && strstr(text,"arena_alloc_failed rc=")) { if(g_arena_detail_lines<32)snprintf(g_arena697_lines[g_arena_detail_lines],256,"%s",text); g_arena_detail_lines++; snprintf(g_arena_detail,sizeof g_arena_detail,"%s",text); } if (level == NVN_PLAT_LOG_RED) { g_red_lines++; snprintf(g_last_red, sizeof g_last_red, "%s", text); } }
static int g_flag_finish_lag;   // 用例设：开关 nvk_finish_lag 的值
static int g_flag_state_keep = 1, g_flag_rt_cont = 1, g_flag_clear_park = 1, g_flag_pass_area = 1;   // 用例设：开关 nvk_state_keep / nvk_rt_cont / nvk_clear_park / nvk_pass_area 的值（缺省同产品：都开）
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_finish_lag")) return g_flag_finish_lag;
    if (!strcmp(name, "nvk_state_keep")) return g_flag_state_keep;
    if (!strcmp(name, "nvk_rt_cont")) return g_flag_rt_cont;
    if (!strcmp(name, "nvk_clear_park")) return g_flag_clear_park;
    if (!strcmp(name, "nvk_pass_area")) return g_flag_pass_area;
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
struct nvn_gpu_cmd_s { int live, is_pre; };
static struct nvn_gpu_cmd_s g_fake_cmd, g_fake_pre;   // 主段 / 前置段各一个替身
static int g_aborts;
static int g_begins, g_commits;
static struct { nvn_gpu_done_fn fn; void *user; uint64_t signal; } g_pend[8]; static int g_npend, g_main_commits;   // g_main_commits：不算前置段
static uint64_t g_fdone;
static uint32_t g_occ_slots_decl; static int g_occ_log[32], g_nocc_log; static int g_rcond;
int s2a_nvn_render_cond(void) { return g_rcond; }
void nvn_gpu_occlusion(nvn_gpu_cmd c, int32_t slot) { (void)c; if (g_nocc_log < 32) g_occ_log[g_nocc_log] = slot; g_nocc_log++; }
static int g_begin_fail_at;   // 第几次开主段失败（1 = 下一次；0 = 都成功）
nvn_gpu_rc nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *d, nvn_gpu_cmd *out, nvn_gpu_err *err) { (void)err; g_begins++;
    struct nvn_gpu_cmd_s *c = d && d->label && strstr(d->label, "pre") ? &g_fake_pre : &g_fake_cmd;
    if (c == &g_fake_cmd && g_begin_fail_at && !--g_begin_fail_at) { *out = NULL; if (err) err->why[0] = 0; return (nvn_gpu_rc){NVN_GPU_E_OOM}; }
    c->live = 1; c->is_pre = c == &g_fake_pre; *out = c;
    if (!c->is_pre) g_occ_slots_decl = d ? d->occlusion_slots : 0;
    return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_cmd_abort(nvn_gpu_cmd c) { c->live = 0; g_aborts++; }
nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd c, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    (void)err; c->live = 0; g_commits++; if (!c->is_pre) g_main_commits++; g_pend[g_npend].fn = done; g_pend[g_npend].user = user; g_pend[g_npend].signal = signal; g_npend++; return (nvn_gpu_rc){NVN_GPU_OK}; }
nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t timeout_ns, nvn_gpu_err *err) { (void)timeout_ns; (void)err; return (nvn_gpu_rc){v <= g_fdone ? NVN_GPU_OK : NVN_GPU_E_TIMEOUT}; }
// 贴图 / 视图 / 采样器 / 上传：只记账
struct nvn_gpu_tex_s { nvn_gpu_tex_desc d; }; struct nvn_gpu_view_s { nvn_gpu_tex of; int has_v; int rid; nvn_gpu_buf buf; uint64_t off, bytes; uint32_t format; }; struct nvn_gpu_samp_s { int x; };
static int g_tex_creates, g_tex_releases, g_view_creates, g_view_releases, g_samp_creates, g_samp_releases, g_tex_create_fail;
static int g_vid; static int g_released_id[8192]; static int g_nrel;   // 视图生命周期取证：每个视图一个递增 id，release 记下 id
static int was_view_released(int rid) { for (int i = 0; i < g_nrel; i++) if (g_released_id[i] == rid) return 1; return 0; }
static uint32_t g_last_usage; static uint16_t g_last_swz;
static view_t g_vw_last; static int g_vw_last_has;   // 最近一次 view_create 收到的视图记录（NULL = 没带）
static struct { uint32_t level, layer; uint64_t pitch, img; int w, h, pre; uint8_t first[64]; } g_up[16]; static int g_nup;
nvn_gpu_rc nvn_gpu_tex_create(const nvn_gpu_tex_desc *d, nvn_gpu_tex *out, nvn_gpu_err *err) {
    if (g_tex_create_fail) { snprintf(err->why, sizeof err->why, "替身：不支持"); *out = NULL; return (nvn_gpu_rc){NVN_GPU_E_UNSUPPORTED}; }
    g_tex_creates++; g_last_usage = d->usage; g_last_swz = d->swz; *out = calloc(1, sizeof **out); (*out)->d = *d; return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_tex_release(nvn_gpu_tex t) { g_tex_releases++; free(t); }
nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex t, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) { (void)err; g_view_creates++; g_vw_last_has = v != NULL; if (v) g_vw_last = *v; *out = calloc(1, sizeof **out); (*out)->of = t; (*out)->has_v = v != NULL; (*out)->rid = ++g_vid; return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_view_release(nvn_gpu_view v) { g_view_releases++; if (v && g_nrel < 8192) g_released_id[g_nrel++] = v->rid; free(v); }
nvn_gpu_rc nvn_gpu_samp_create(const samp_par *p, nvn_gpu_samp *out, nvn_gpu_err *err) { (void)p; (void)err; g_samp_creates++; *out = calloc(1, sizeof **out); (*out)->x = p->ws; return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_samp_release(nvn_gpu_samp s) { g_samp_releases++; free(s); }
static uint32_t g_caps_nosample = ~0u;   // 用例：这个格式在替身设备上不能直接采样（后端走计算解码的 BC 格式，能力位回 0）
uint32_t nvn_gpu_format_caps(uint32_t f) { if (f == g_caps_nosample) return 0;
    return f == NVN_FORMAT_RGBA8 ? NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_RENDER | NVN_GPU_FMT_STORAGE : f == NVN_FORMAT_R16 || f == NVN_FORMAT_RG32UI || f == NVN_FORMAT_RGBA32UI || f == NVN_FORMAT_RGBA16UI ? NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_RENDER : NVN_GPU_FMT_SAMPLE; }
void nvn_gpu_tex_upload(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t level, uint32_t layer, const NVNcopyRegion *r, const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    (void)t; if (g_nup == 16) return;
    g_up[g_nup].pre = c->is_pre; g_up[g_nup].level = level; g_up[g_nup].layer = layer; g_up[g_nup].pitch = row_bytes; g_up[g_nup].img = image_bytes; g_up[g_nup].w = r->width; g_up[g_nup].h = r->height;
    memcpy(g_up[g_nup].first, src, image_bytes < 64 ? image_bytes : 64); g_nup++; }
// 脏页替身：代号由用例推进
static uint64_t g_dirty_now = 100, g_dirty_written; static int g_protects;
void nvn_dirty_protect(const void *addr, size_t len) { (void)addr; (void)len; g_protects++; }
void nvn_dirty_stats(uint64_t *f, uint64_t *p) { *f = 0; *p = 0; }
static uint64_t g_dirty_map = 1; uint64_t nvn_dirty_mapping_generation(void) { return g_dirty_map; }
int nvn_dirty_tracking_unreliable(void) { return 0; }
void nvn_dirty_mp_stats(uint64_t *c, uint64_t *g, uint64_t *n) { *c = 0; *g = 0; *n = 0; }
uint64_t nvn_dirty_now(void) { return g_dirty_now; }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { (void)addr; (void)len; return g_dirty_written > stamp; }

// 缓冲替身：记每次按 GPU 顺序的写（偏移、长度、首 8 字节）
struct nvn_gpu_buf_s { uint64_t bytes; uint8_t *cpu; };
static int g_buf_cpu_auto;   // 1 = 新建的缓冲各带一块自己的 CPU 可见内存（两份池镜像要分得开的用例用）
static int g_buf_creates, g_buf_releases; static uint64_t g_buf_bytes;
static struct { uint64_t off, len; uint8_t head[8]; int pre; } g_bw[32]; static int g_nbw;
nvn_gpu_rc nvn_gpu_buf_create(uint64_t bytes, int mem, const void *init, nvn_gpu_buf *out, nvn_gpu_err *err) { (void)mem; (void)init; (void)err; g_buf_creates++; g_buf_bytes = bytes; *out = calloc(1, sizeof **out); (*out)->bytes = bytes; if (g_buf_cpu_auto) (*out)->cpu = calloc(1, bytes); return (nvn_gpu_rc){NVN_GPU_OK}; }
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
    (void)c; (void)dst; (void)off; tb_note(0, src, level, layer, r, row_bytes, image_bytes); }
void nvn_gpu_copy_buf_tex(nvn_gpu_cmd c, nvn_gpu_buf src, uint64_t off, uint64_t row_bytes, uint64_t image_bytes, nvn_gpu_tex dst, uint32_t level, uint32_t layer, const NVNcopyRegion *r) {
    (void)c; (void)src; (void)off; tb_note(1, dst, level, layer, r, row_bytes, image_bytes); }
static uint8_t *g_buf_cpu_fake;   // 用例要「GPU 写过镜像」时把它指到一块等大的内存；NULL = 这种缓冲 CPU 读不到
uint8_t *nvn_gpu_buf_cpu(nvn_gpu_buf b) { return b && b->cpu ? b->cpu : g_buf_cpu_fake; }
void nvn_gpu_buf_release(nvn_gpu_buf b, nvn_gpu_retired_fn retired, void *user) { (void)retired; (void)user; g_buf_releases++; free(b->cpu); free(b); }
void nvn_gpu_buf_write(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off, const void *src, uint64_t bytes) { (void)dst; if (g_nbw == 32) return;
    g_bw[g_nbw].off = off; g_bw[g_nbw].len = bytes; g_bw[g_nbw].pre = c->is_pre; memcpy(g_bw[g_nbw].head, src, bytes < 8 ? bytes : 8); g_nbw++; }
// 页级脏账替身：4096 字节一页，g_pmem 这个池每页的写代号由用例设
enum { PG = 4096, NPG = 8 };
static _Alignas(4096) uint8_t g_pmem[PG * NPG]; static uint64_t g_pg_gen[NPG]; static int g_pg_tracked = 1;
size_t nvn_dirty_page_size(void) { return PG; }
// 不受写跟踪的池（用例用 g_pg_tracked=0 设）没有跟踪区间：真实现里也是返回 NULL，靠它判「每次使用都整段同步」。
const void *nvn_dirty_span(const void *addr, size_t len) { (void)len; if (!g_pg_tracked) return NULL; return (const uint8_t *)addr >= g_pmem && (const uint8_t *)addr < g_pmem + sizeof g_pmem ? g_pmem : NULL; }
long nvn_dirty_page_gens(const void *span, const void *addr, size_t len, uint64_t *gens, size_t n) {
    if (!span || !g_pg_tracked) return -1;
    size_t p = (size_t)((const uint8_t *)addr - g_pmem) / PG, q = (size_t)((const uint8_t *)addr + len - 1 - g_pmem) / PG;
    if (q - p + 1 > n) return -1;
    for (size_t i = p; i <= q; i++) gens[i - p] = g_pg_gen[i];
    return (long)(q - p + 1);
}

// 通道替身：记每次开通道的描述
static int g_pass_open, g_bind_outside, g_draw_outside;   // 通道开着没有；图形阶段的绑定在通道外到达的次数（真后端会置粘滞、整段作废）
static int g_tbuf_created, g_tbuf_in_pass, g_tbuf_binds[6];
static struct { nvn_gpu_buf buf; uint64_t off, bytes; uint32_t format; } g_tbuf_bound[6];
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
void nvn_gpu_pass_begin(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d) { (void)c; if (g_npd < 8) g_pd[g_npd] = *d; g_npd++; g_pass_open = 1; }
void nvn_gpu_pass_end(nvn_gpu_cmd c) { (void)c; g_pass_ends++; g_pass_open = 0; }
static int g_npd_now(void) { return g_npd; } static int g_ends_now(void) { return g_pass_ends; }
void nvn_gpu_barrier(nvn_gpu_cmd c, uint32_t bits) { (void)c; g_barrier_bits |= bits; g_barrier_at = g_npd; }
static struct { nvn_gpu_clear d; int npd, ends; } g_cr[8]; static int g_ncr;   // 矩形清：记描述，以及收到时已开 / 已关过几个通道
void nvn_gpu_clear_rect(nvn_gpu_cmd c, const nvn_gpu_clear *d) { (void)c; if (g_ncr < 8) { g_cr[g_ncr].d = *d; g_cr[g_ncr].npd = g_npd; g_cr[g_ncr].ends = g_pass_ends; } g_ncr++; }
static uint64_t g_tick;   // 游戏时钟替身
uint64_t s2a_guest_tick(void) { return g_tick; }

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
static struct rp_prog g_fprog; static int g_resolves, g_resolve_rc; static struct rp_shader_req g_last_req;
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) { g_resolves++; g_last_req = *q; *out = g_resolve_rc ? NULL : &g_fprog; return g_resolve_rc; }
// 计算：程序、管线、派发的替身
static struct rp_prog g_cprog; static int g_cresolves, g_cresolve_rc, g_cpipe_gets, g_ndisp; static uint32_t g_disp[3]; static struct nvn_gpu_pipe_s *g_disp_pipe; static nvn_gpu_shader g_cpipe_sh;
static nvn_gpu_buf g_disp_ind; static uint64_t g_disp_off;
static nvn_gpu_binding g_cb[16]; static uint32_t g_ncb; static int g_cbinds;
int rp_shader_resolve_compute(const struct rp_prog **out) { g_cresolves++; *out = g_cresolve_rc ? NULL : &g_cprog; return g_cresolve_rc; }
void rp_shader_reset(void) {}
static int g_pipe_rc;
static int g_pipe_gets, g_set_pipes, g_set_dyns, g_gdraws; static nvn_gpu_pipe_desc g_last_pdesc; static nvn_gpu_dyn g_last_dyn; static nvn_gpu_draw_args g_last_da;
static struct { uint32_t stream; uint64_t off; } g_vb[4]; static int g_nvb;
static nvn_gpu_binding g_fb[3][16]; static uint32_t g_nfb[3];
static _Alignas(16) uint8_t g_arena[1 << 16]; static uint64_t g_arena_used; static struct nvn_gpu_buf_s g_arena_buf;
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) { (void)mode; g_pipe_gets++; g_last_pdesc = *d; *out = g_pipe_rc ? NULL : &g_fpipe; if (err) { memset(err, 0, sizeof *err); err->rc.code = g_pipe_rc; } return (nvn_gpu_rc){g_pipe_rc}; }
void nvn_gpu_set_pipe(nvn_gpu_cmd c, nvn_gpu_pipe p) { (void)c; (void)p; g_set_pipes++; }
static struct nvn_gpu_pipe_s g_fcpipe;
nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) { (void)mode; (void)err; g_cpipe_gets++; g_cpipe_sh = cs->sh; *out = &g_fcpipe; return (nvn_gpu_rc){NVN_GPU_OK}; }
void nvn_gpu_copy_buf(nvn_gpu_cmd c, nvn_gpu_buf src, uint64_t soff, nvn_gpu_buf dst, uint64_t doff, uint64_t bytes) {
    (void)c; (void)src; CHECK(dst == &g_arena_buf && g_buf_cpu_fake && bytes == 12);
    if (dst == &g_arena_buf && g_buf_cpu_fake) memcpy(g_arena + doff, g_buf_cpu_fake + soff, (size_t)bytes);
}
void nvn_gpu_dispatch(nvn_gpu_cmd c, nvn_gpu_pipe cp, uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf ind, uint64_t ind_off) { (void)c; g_disp_ind = ind; g_disp_off = ind_off; g_ndisp++; g_disp[0] = x; g_disp[1] = y; g_disp[2] = z; g_disp_pipe = cp; }
void nvn_gpu_set_dyn(nvn_gpu_cmd c, const nvn_gpu_dyn *d) { (void)c; g_set_dyns++; g_last_dyn = *d; }
void nvn_gpu_set_vbuf(nvn_gpu_cmd c, uint32_t stream, nvn_gpu_buf b, uint64_t off) { (void)c; (void)b; if (g_nvb < 4) { g_vb[g_nvb].stream = stream; g_vb[g_nvb].off = off; } g_nvb++; }
static int g_bind_calls[3];   // 图形两个阶段各发了几次绑定
void nvn_gpu_bind(nvn_gpu_cmd c, nvn_gpu_stage st, const nvn_gpu_binding *b, uint32_t n) { (void)c;
    for (uint32_t i = 0; i < n; i++) if (b[i].cls == NVN_GPU_RES_TBUF && b[i].u.view) {
        nvn_gpu_view v = b[i].u.view; g_tbuf_binds[st]++;
        g_tbuf_bound[st].buf = v->buf; g_tbuf_bound[st].off = v->off;
        g_tbuf_bound[st].bytes = v->bytes; g_tbuf_bound[st].format = v->format;
    }
    if (st == NVN_GPU_SH_CS) { g_cbinds++; g_ncb = n; memcpy(g_cb, b, (n < 16 ? n : 16) * sizeof *b); return; }
    CHECK(st >= NVN_GPU_SH_VS && st <= NVN_GPU_SH_GS);
    if (st < NVN_GPU_SH_VS || st > NVN_GPU_SH_GS) return;
    if (!g_pass_open) g_bind_outside++;
    int i = (int)st; g_bind_calls[i]++; g_nfb[i] = n; memcpy(g_fb[i], b, (n < 16 ? n : 16) * sizeof *b); }
void nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a) { (void)c; g_gdraws++; g_last_da = *a; if (!g_pass_open) g_draw_outside++; }
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); out->min_ubo_align = 64; out->caps = NVN_GPU_CAP_BLIT_SCALED; }
static int g_arena697_fail;
nvn_gpu_rc nvn_gpu_arena_alloc(nvn_gpu_cmd c, uint64_t bytes, uint32_t align, nvn_gpu_slice *out, nvn_gpu_err *err) { (void)c; (void)err;
    if(g_arena697_fail) { memset(out,0,sizeof *out);memset(err,0,sizeof *err);
        err->rc.code=g_arena697_fail==3?NVN_GPU_OK:NVN_GPU_E_ORDER;
        if(g_arena697_fail!=2 && g_arena697_fail!=3) {
            memset(err->why,'X',sizeof err->why);char prefix[48];snprintf(prefix,sizeof prefix,"first-bind-outside-pass-%d ",g_arena697_fail);
            memcpy(err->why,prefix,strlen(prefix));err->why[sizeof err->why-1]=0; }
        return err->rc; }
    g_arena_used = (g_arena_used + align - 1) / align * align; out->buf = &g_arena_buf; out->off = g_arena_used; out->cpu = g_arena + g_arena_used; out->va = 0; g_arena_used += bytes; return (nvn_gpu_rc){NVN_GPU_OK}; }

static const uint64_t *g_fake_occ; static uint32_t g_fake_nocc;   // 下一次完成时交给回调的各槽样本数
static void fake_complete(uint32_t status) {
    for (int i = 0; i < g_npend; i++) { nvn_gpu_done d = {0}; d.signal = g_pend[i].signal; d.status = status; d.why = "替身失败";
        if (status != NVN_GPU_DONE_FAILED) { d.occlusion = g_fake_occ; d.nocclusion = g_fake_nocc; }
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
    CHECK(nvn_unimpl_count("op", "OP_MULTI_DRAW") == 2);
    CHECK(nvn_unimpl_count("op", "OP_DLSS") == 1);
    CHECK(nvn_unimpl_count("op", "OP_ALPHA_REF") == 0);
    CHECK(nvn_unimpl_kinds() == 3);
    CHECK(g_red_lines - red0 == 3);   // 每种只在首次出现打一行
    char buf[256]; nvn_unimpl_dump(buf, sizeof buf);
    CHECK(!strcmp(buf, "op\tOP_DRAW_TEX\t1\nop\tOP_MULTI_DRAW\t2\nop\tOP_DLSS\t1\n"));
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
static void t_view_pin(void) {
    res_fresh();
    // 视图表是 128 组 x 8 路（按贴图序号散列）：窗口里取 3000 个不同贴图，平均每组 23 个——first 那一组的 8 格必然被本轮钉满、
    // 多出来的走溢出表；窗口结束后再取 3000 个，first 那一格必然被换掉。数量由表的形状推出，不依赖具体散列到哪一组。
    enum { N = 6000, NWIN = 3000 };
    static tex_t tp[N];
    for (int i = 0; i < N; i++) { tp[i] = mk_tex(3000 + (uint64_t)i, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&tp[i]); }
    nvn_gpu_view first = NULL;
    int first_id = 0;
    rp_view_pin_begin();
    first = rp_view_for(&tp[0], NULL, &g_fake_cmd);
    CHECK(first != NULL);
    if (first) first_id = first->rid;
    for (int i = 1; i < NWIN; i++) (void)rp_view_for(&tp[i], NULL, &g_fake_cmd);   // 每组都被挤满：钉住时不能放掉 first
    CHECK(first && !was_view_released(first_id));                                  // 修前：轮转把 first 放掉 → 红
    rp_view_pin_end();
    for (int i = NWIN; i < N; i++) (void)rp_view_for(&tp[i], NULL, &g_fake_cmd);
    CHECK(first && was_view_released(first_id));                                   // 窗口结束：它可以被淘汰
    res_fresh();
    // 一帧里轮着用 300 个目标（真机一款 3D 游戏每帧一百多个通道）：第二遍取到的必须是第一遍那些视图，一个都不重建。
    // 300 个散进 128 组、每组 8 格；数量远小于 1024 格，但会不会有一组超过 8 个取决于散列，所以只要求「重建的不超过 300 个里的 5%」
    // （原先 64 格的环：第二遍 300 个全部重建）。
    { enum { M = 300 }; static tex_t tm[M]; static int rid[M]; int rebuilt = 0;
      for (int i = 0; i < M; i++) { tm[i] = mk_tex(9000 + (uint64_t)i * 7, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0); nvn_be_tex_init(&tm[i]); }
      for (int i = 0; i < M; i++) { nvn_gpu_view v = rp_view_for(&tm[i], NULL, &g_fake_cmd); rid[i] = v ? v->rid : -1; }
      for (int i = 0; i < M; i++) { nvn_gpu_view v = rp_view_for(&tm[i], NULL, &g_fake_cmd); if (!v || v->rid != rid[i]) rebuilt++; }
      if (rebuilt > M / 20) fprintf(stderr, "视图表：300 个目标取第二遍，重建了 %d 个（期望 ≤ %d）\n", rebuilt, M / 20);
      CHECK(rebuilt <= M / 20); }
    res_fresh();   // 本用例建的存储不留给后面的用例（它们按 live 计数做断言）
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

static pool_t g_tp; static uint8_t g_tmem[1 << 20];
static tex_t mk_tex(uint64_t serial, int64_t off, int fmt, uint32_t w, uint32_t h, uint32_t levels, int flags) {
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 'T'); t.serial = serial; t.pool = &g_tp; t.off = off; t.mem = g_tmem + off;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = fmt; t.d.width = w; t.d.height = h; t.d.depth = 1; t.d.levels = levels; t.d.flags = flags;
    nvn_tex_layout(&t.d, &t.L); return t;
}
static void res_fresh(void) { rp_res_reset(); memset(g_tmem, 0, sizeof g_tmem); memset(&g_tp, 0, sizeof g_tp); g_tp.mem = g_tmem; g_tp.size = sizeof g_tmem; g_tp.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    g_nup = 0; g_dirty_written = 0; }

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
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&k, 0, 0, &one, dst, 16, 64);
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels") == 1);                      // GPU 写过：不读游戏内存，记账
    g_ft_hit = 1; sent_fill(dst, sizeof dst); nvn_be_read_texels(&k, 0, 0, &one, dst, 16, 64); g_ft_hit = 0;
    CHECK(all_sent(dst, sizeof dst) && un_cnt("read_texels") == 2 && nvn_be_tex_gpu_owned(52) == 1);   // 脏页追踪的答案不能把归属翻回去
    nvn_be_tex_cpu_written(&k);                                                          // 游戏又 WriteTexels 了
    sent_fill(dst, sizeof dst); nvn_be_read_texels(&k, 0, 0, &one, dst, 16, 64);
    CHECK(nvn_be_tex_gpu_owned(52) == 0 && !memcmp(dst, lin, 64) && un_cnt("read_texels") == 2);
    nvn_be_tex_release(&k);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

static void t_cpu_upload(void) {   // 存储里是 CPU 写的数据：首次使用上传，之后只在脏了时重传；内容交 GPU 后不再传
    nvn_be_sub_begin();
    res_fresh();
    tex_t t = mk_tex(5, 0, NVN_FORMAT_RGBA8, 8, 8, 2, 0);
    uint8_t lin0[8 * 8 * 4], lin1[4 * 4 * 4]; for (unsigned i = 0; i < sizeof lin0; i++) lin0[i] = (uint8_t)(i * 7 + 1); for (unsigned i = 0; i < sizeof lin1; i++) lin1[i] = (uint8_t)(200 - i);
    CHECK(nvn_swizzle(&t.d, 0, 0, lin0, 32, t.mem) == 0 && nvn_swizzle(&t.d, 1, 0, lin1, 16, t.mem) == 0);
    int c0 = g_tex_creates, r0 = g_tex_releases, p0 = g_protects; uint64_t u0 = rp_res_uploads();
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
    nvn_be_tex_release(&t);
    CHECK(g_tex_releases - r0 == 1 && rp_res_tex_built() == 0);                     // CPU 内容贴图不进共用表：对象 Finalize 即放
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

static void t_cpu_rules(void) {   // 首用判定的三条分支
    nvn_be_sub_begin();
    res_fresh();
    tex_t a = mk_tex(21, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); memset(a.mem, 0x55, a.L.size);
    nvn_be_pool_gpu_written(&g_tp);                                 // GPU 写过的池里的未压缩贴图：当渲染目标，不当 CPU 贴图；建图时存储里现有的内容只取这一次
    { uint64_t u0 = rp_res_uploads(); int p0 = g_protects;
      CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == 1 && g_up[0].first[0] == 0x55 && rp_res_uploads() == u0 && g_protects == p0);
      memset(a.mem, 0x56, a.L.size); g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
      CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == 1); }   // 之后内存再变也不重传（不跟踪写）
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
    { tex_t c = mk_tex(53, (1 << 20) - 0x10, NVN_FORMAT_R8, 64, 64, 1, NVN_TEXTURE_FLAGS_LINEAR);   // 源范围出池
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

static void t_registry(void) {
    nvn_be_sub_begin();
    res_fresh(); nvn_unimpl_reset(); int v0 = g_view_creates, vr0 = g_view_releases, s0 = g_samp_creates, sr0 = g_samp_releases;
    tex_t a = mk_tex(31, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0), b = mk_tex(32, 0x4000, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    view_t vw; memset(&vw, 0, sizeof vw); vw.set_levels = 1; vw.nlevels = 1;
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == NULL);             // 没登记
    nvn_be_reg_tex(3, &a, NULL); nvn_be_set_tex_pool_n(8);
    const tex_t *tt = NULL; nvn_gpu_view v1 = rp_view_by_id(3, &g_fake_cmd, &tt);
    CHECK(v1 && tt && tt->serial == 31 && v1->has_v == 0);
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == v1 && g_view_creates - v0 == 1);   // 第二次取：同一个视图
    nvn_be_reg_tex(3, &a, NULL);
    CHECK(rp_view_by_id(3, &g_fake_cmd, NULL) == v1 && g_view_creates - v0 == 1);   // 同内容重登记：视图还在
    nvn_be_reg_tex(3, &b, &vw);
    nvn_gpu_view v2 = rp_view_by_id(3, &g_fake_cmd, &tt);
    CHECK(v2 && tt->serial == 32 && v2->has_v == 1 && g_view_creates - v0 == 2 && g_view_releases - vr0 == 1);   // 换了内容：旧视图放掉
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
    };
    rp_view_pin_begin();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tex_t t = mk_tex(7000 + i, (int64_t)i * 0x1000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
        t.d.target = cases[i].target; t.d.depth = cases[i].depth; nvn_tex_layout(&t.d, &t.L);
        nvn_be_reg_tex((int)i, &t, NULL);
        const tex_t *snap = NULL;
        nvn_gpu_view v = rp_view_by_id_decl((int)i, cases[i].dtt, 0, 0, &g_fake_cmd, &snap);
        CHECK(v && snap && snap->serial == 7000 + i);
        CHECK(g_vw_last_has == cases[i].modified);
        if (cases[i].modified) CHECK(g_vw_last.set_target && g_vw_last.target == cases[i].want &&
            g_vw_last.set_layers && g_vw_last.nlayers == cases[i].layers && g_vw_last.min_layer == 0);
        else CHECK(v == rp_view_by_id((int)i, &g_fake_cmd, NULL));
    }
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
    // 12 层二维数组当立方体读：后端建图没带 CUBE_COMPATIBLE，必被拒绝的视图不交下去；记账并绑零贴图。
    t = mk_tex(7012, 0x18000, NVN_FORMAT_RGBA8, 4, 4, 1, 0);
    t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.depth = 12; nvn_tex_layout(&t.d, &t.L); nvn_be_reg_tex(12, &t, NULL);
    int vc = g_view_creates;
    CHECK(rp_view_by_id_decl(12, NVN_TEXTURE_TARGET_CUBEMAP + 1, 1, 0, &g_fake_cmd, NULL) == z);
    CHECK(g_view_creates == vc && nvn_unimpl_count("tex", "cube_view_of_2d_array") == 1);
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
    // 贴图：6 例 + 7010 + 7011 + 零立方体 + 零 3D + 7014 + 零二维数组 + 7013 = 13（7012 不建视图也不解析）；
    // 视图：未改造 2 + 改造 4 + 7010 + 7011 原样 + 零立方体 + 零 3D + 7014 原样 + 零二维数组 + 7013 = 13
    CHECK(g_tex_releases == releases + 13 && g_view_releases == view_releases + 13);
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
static void pass_fresh(void) { nvn_be_shutdown_session(); res_fresh(); nvn_unimpl_reset(); g_npd = g_pass_ends = g_ncr = 0; g_barrier_bits = 0; g_barrier_at = -1; }

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

// 最小的可画环境：一个不带属性、不带绑定的程序，一张 64x36 的颜色目标
static tex_t g_qrt;
static void q_env(stream *s) {
    memset(&g_fprog, 0, sizeof g_fprog); for (int st = 0; st < RP_PROG_STAGES; ++st) for (int i = 0; i < RP_PROG_ENT_MAX; ++i) g_fprog.ent[st][i].src_reg2 = UINT16_MAX; g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.color_out_mask = 0xffffffffu;
    g_qrt = mk_tex(91, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    put_set_rt(s, &g_qrt, NULL);
    p_viewports *vp = put(s, OP_VIEWPORTS, sizeof *vp); vp->n = 1; vp->v[0][2] = 64; vp->v[0][3] = 36;
}
static void q_draw(stream *s) { p_draw *d = put(s, OP_DRAW, sizeof *d); d->prim = 4; d->count = 3; d->instances = 1; }

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
// GS 资源变化必须重新取变体；需要拆分的图元不能当原生 GS 输入。
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
    stream t = {0}; p_draw *quad = put(&t, OP_DRAW, sizeof *quad); quad->prim = 7; quad->count = 4; quad->instances = 1;
    end(&t); submit1(&t); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2);
    CHECK(nvn_unimpl_count("draw", "geometry_topology_unimplemented") == 1);
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
static void t_storage_sync_before_pass(void) {
    pass_fresh(); g_gdraws = g_resolves = g_resolve_rc = 0; g_arena_used = 0; g_bind_outside = 0; g_draw_outside = 0; g_pass_open = 0; g_nbw = 0;
    memset(g_pmem, 0, sizeof g_pmem); nvn_be_pool_add(g_pmem, sizeof g_pmem);
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    int tracked = g_pg_tracked; g_pg_tracked = 0;
    stream s = {0}; q_env(&s);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].cls = NVN_GPU_RES_SSBO; g_fprog.ent[1][0].src_kind = NVN_RB_SBUFP;
    g_fprog.ent[1][0].src_reg = 0; g_fprog.ent[1][0].access = NVN_GPU_ACC_R;
    { p_bind_buf *sb = put(&s, OP_BIND_SSBO, sizeof *sb); sb->stage = 1; sb->index = 0; sb->addr = base + 2 * PG; sb->size = 64; }
    q_draw(&s); q_draw(&s);
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 2);
    CHECK(g_nbw == 2 && g_bw[0].pre == 1 && g_bw[1].pre == 0);  // 第二次同步确实录成了主段里的传输
    CHECK(g_bind_outside == 0 && g_draw_outside == 0);            // 每一次图形绑定、每一笔绘制都在通道里
    CHECK(nvn_unimpl_count("draw", "storage_buffer_unresolved") == 0);
    g_pg_tracked = tracked; pass_fresh();
}

// A buffer texture reads the registered pool range directly, with no sampler.
// A second untracked use must synchronize before its render pass is reopened.
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
    q_draw(&s); q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
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

// 3D 贴图的深度切片当渲染目标：NVN 视图的层号在 3D 贴图上是深度切片（nvn_mtl.m attach() 落到 depthPlane）；Vulkan 的 3D 视图起点只能是 0，
// 子范围只能用 2D（阵列）视图（nvn_vk_texview.h 文件头与 plan_make 的错误信息都这么说）。所以 3D 附件带层号时核心向后端请求 2D 阵列目标；
// 游戏自己设了目标的、没带层号的（首片）、不是 3D 的贴图，视图记录原样递下去。
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
    // ④ 游戏自己设了目标：原样
    view_t own = v; own.min_layer = 7; own.set_target = 1; own.target = NVN_TEXTURE_TARGET_2D;
    slice_pass(&v3, &own);
    CHECK(g_npd == 1 && g_vw_last_has == 1 && !memcmp(&g_vw_last, &own, sizeof own));
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
    view_t zown = vz; zown.set_target = 1; zown.target = NVN_TEXTURE_TARGET_2D;   // 游戏自己设了目标：原样
    slice_pass_depth(&z3, &zown);
    CHECK(g_npd == 1 && g_pd[0].depth.view && g_vw_last_has == 1 && !memcmp(&g_vw_last, &zown, sizeof zown));
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

// 遮挡计数：通过的样本数是队列上的累计值——复位清零、之后的绘制往上加、报告写出当时的累计值。
// 期望值手算：各槽的样本数由替身在完成时给出，报告值 = 复位之后、报告之前开过的各槽之和（跨段接着加）。
// 开段失败（审计 P1-174）：时间线照走一格，不回放出任何绘制；中途交段之后开新段失败时，已经交出去的段完成回调不能碰到放掉的提交。
static void t_begin_fail(void) {
    // ① 一上来就开不了段：这次提交一笔都没交，返回的栅栏值 = 原值 + 64；下一次正常提交用的是再 + 64，两次的值不重
    pass_fresh(); g_gdraws = 0; uint64_t f0 = nvn_be_last_fence(); int c0 = g_main_commits; g_npend = 0;
    static stream s; memset(&s, 0, sizeof s); q_env(&s); q_draw(&s); end(&s);
    g_begin_fail_at = 1; nvn_be_sub_begin(); nvn_be_exec(s.b); uint64_t f1 = nvn_be_sub_commit();
    CHECK(f1 == f0 + 64 && nvn_be_last_fence() == f0 + 64 && g_main_commits == c0 && g_gdraws == 0 && g_begin_fail_at == 0);
    nvn_be_sub_begin(); nvn_be_exec(s.b); uint64_t f2 = nvn_be_sub_commit();
    CHECK(f2 == f0 + 128 && g_main_commits == c0 + 1 && g_npend && g_pend[g_npend - 1].signal == f0 + 128);
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
    CHECK(g_begin_fail_at == 0 && g_main_commits == c0 + 1 && f3 == f0 + 64 && g_gdraws == 13);   // 第 14 笔在开不出来的新段里：不画
    static uint64_t occ[4] = {7, 0, 0, 0}; g_fake_occ = occ; g_fake_nocc = 1;
    fake_complete(NVN_GPU_DONE_OK); g_fake_occ = NULL; g_fake_nocc = 0;
    nvn_be_sub_begin(); nvn_be_exec(s.b); CHECK(nvn_be_sub_commit() == f0 + 128); fake_complete(NVN_GPU_DONE_OK);   // 之后照常
    rp_split_draws = keep; pass_fresh();
}

static void t_occlusion(void) {
    pass_fresh(); g_rcond = 0; g_gdraws = 0; g_nocc_log = 0; g_tick = 5;
    uint64_t ra[2] = {~0ull, ~0ull}, rb[2] = {~0ull, ~0ull}, rcx[2] = {~0ull, ~0ull};
    // 段 1：游戏第一次碰这个计数器。开段时还不知道，没声明槽：这一段里不计
    stream s1 = {0}; q_env(&s1); q_counter(&s1, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL); q_draw(&s1); end(&s1);
    submit1(&s1);
    CHECK(g_occ_slots_decl == 0 && g_nocc_log == 0 && g_gdraws == 1 && g_pd[0].occlusion == 0);
    fake_complete(NVN_GPU_DONE_OK);
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
    CHECK(ra[0] == 10 && ra[1] == 600);   // 复位在段 1；报告 A 之前只开过槽 0
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
    g_pg_gen[1] = 107; g_dirty_now = 108; g_nbw = 0;
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + PG, PG, NULL, NULL) == 1 && g_nbw == 0 && mirror[PG] == 0x22 && mirror[2 * PG - 1] == 0x22);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 取不到程序：记账跳过，不派发
    g_cresolve_rc = -1; submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ndisp == 2 && nvn_unimpl_count("draw", "compute_program_unresolved") == 1);
    g_cresolve_rc = 0; g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem); pass_fresh();
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
    g_pg_gen[2] = g_pg_gen[3] = 101; g_dirty_now = 102; g_nbw = 0;                                                 // 游戏又写了这两页
    nvn_be_sub_begin();
    CHECK(rp_buf_resolve(&g_fake_cmd, base + 2 * PG, 2 * PG, NULL, NULL) == 1 && g_nbw == 0 && mirror[2 * PG + 32] == 0x22 && mirror[2 * PG + 127] == 0x22 && mirror[3 * PG + 16] == 0x22 && mirror[3 * PG + 63] == 0x22);   // 两段都重传了：登记已撤
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
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
    g_buf_cpu_fake = NULL; nvn_be_pool_del(g_pmem);
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
    o = put_t2t(&s, &a, &b); o->sr = (NVNcopyRegion){0, 0, 0, 16, 8, 1}; o->dr = o->sr; o->flags = NVN_COPY_FLAGS_MIRROR_Y_BIT;                       // 镜像：没接
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
        nvn_unimpl_reset();
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
    p_bind_vattr *va = put(&s, OP_BIND_VATTR, sizeof *va); va->n = 1; va_st a0 = {.fmt = 0x2A, .offset = 0, .stream = 1}; memcpy(&va->st[0], &a0, 4);
    p_bind_vstream *vs = put(&s, OP_BIND_VSTREAM, sizeof *vs); vs->n = 2; vs->st[1].stride = 16;
    p_bind_vbuf_single *vb = put(&s, OP_BIND_VBUF, sizeof *vb); vb->first = 1; vb->size = 64; vb->addr = base + 2 * PG + 32 - 0x1000;   // 记录里是 GPU 地址（替身的换算是 +0x1000）
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 128; ub->size = 256;
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    p_viewports *vp = put(&s, OP_VIEWPORTS, sizeof *vp); vp->n = 1; vp->v[0][2] = 64; vp->v[0][3] = 36;
    p_draw_idx *di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 6; di->base_vertex = 3; di->instances = 0; di->ibuf = base + 3 * PG + 8;
    di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG + 8;   // 第二笔：什么都没变
    di = put(&s, OP_DRAW_IDX, sizeof *di); di->prim = 0xa; di->itype = 1; di->count = 4; di->ibuf = base + 3 * PG;      // 邻接线：还没接
    p_draw *dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 5; dn->first = 2; dn->count = 4; dn->instances = 3;            // 不带索引的条带
    end(&s);
    submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 3 && g_npd == 1 && pd_zero_clear(&g_pd[0]));   // 三笔画在同一个通道里；没有清除 → 不带清除值（刚建的目标载入时清成零）
    CHECK(nvn_unimpl_count("draw", "prim_10") == 1);
    CHECK(g_resolves == 2 && g_pipe_gets == 2);          // 第一笔取一次；第二笔没变不取；条带换了图元再取一次
    CHECK(g_last_req.prim == 5 && g_last_req.rt_count == 1 && g_last_req.rt_format[0] == NVN_FORMAT_RGBA8);
    CHECK(g_last_pdesc.st[0].sh == &g_fsh[0] && g_last_pdesc.st[1].sh == &g_fsh[1] && g_last_pdesc.cfmt[0] == NVN_FORMAT_RGBA8 && g_last_pdesc.dfmt == 0);
    CHECK(g_last_pdesc.attr[0].fmt == 0x2A && g_last_pdesc.attr[0].stream == 1 && g_last_pdesc.stream[1].stride == 16);
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
      CHECK(g_last_pdesc.poly.front == NVN_FRONT_FACE_CW && g_last_pdesc.poly.cull == NVN_FACE_BACK && g_last_pdesc.origin_ll == 1);   // 翻转开着：接口说逆时针 → 规范化坐标里顺时针
      int pg0 = g_pipe_gets; s2a_nvn_rast_flip = 0;                                  // 换一条翻转关着的队列：同样的状态，管线要重取
      stream w2 = {0}; di = put(&w2, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->itype = 1; di->count = 3; di->ibuf = base + 3 * PG; end(&w2);
      submit1(&w2); fake_complete(NVN_GPU_DONE_OK);
      CHECK(g_pipe_gets == pg0 + 1 && g_last_pdesc.poly.front == NVN_FRONT_FACE_CCW);
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
    CHECK(g_gdraws == d0 + 16);   // 新拓扑6笔 + 点模式1笔成功；不支持的线模式不绘制
    uint64_t dr_n = 0, sk_n = 0; rp_draw_stats(&dr_n, &sk_n); CHECK(dr_n == 19 && sk_n == 4);
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

static void t_present(void) {   // 上屏：没有窗口按丢帧报；有窗口时取图、按比例贴合、呈现；交换链过期先重建
    pass_fresh(); rp_present_reset(); memset(g_acq_script, 0, sizeof g_acq_script); g_acq_i = g_rebuilds = g_presents = 0;
    tex_t t = mk_tex(71, 0, NVN_FORMAT_RGBA8, 640, 360, 1, 0);
    CHECK(nvn_be_has_window() == 0 && nvn_be_display_hz() == 0);
    uint64_t k1 = nvn_be_present(&t, 0, NULL);
    CHECK(k1 == 1 && nvn_be_presented_time(1) == -1 && g_presents == 0 && nvn_unimpl_count("present", "no_window") == 1);
    CHECK(nvn_be_presented_time(2) == 0);                           // 还没发出的票：没结果
    rp_present_set_surface(&g_fake_surf, 120);
    CHECK(nvn_be_has_window() == 1 && nvn_be_display_hz() == 120);
    CHECK(nvn_be_present(&t, 0, NULL) == 2 && g_presents == 1);
    CHECK(g_last_pd.src != NULL && g_last_pd.crop[2] == 0 && g_last_pd.dst[0] == 0 && g_last_pd.dst[1] == 0 && g_last_pd.dst[2] == 1280 && g_last_pd.dst[3] == 720);   // 16:9 贴满 16:9
    CHECK(nvn_be_presented_time(2) == 5.0);                         // 后端没给上屏时刻：按交给呈现引擎的时刻记
    int crop[4] = {0, 0, 320, 300};                                 // 16:15 的裁剪放进 1280x720：高贴满，宽 768，左右各 256 黑边
    CHECK(nvn_be_present(&t, 0, crop) == 3 && g_presents == 2);
    CHECK(g_last_pd.flip_y == 1 && g_last_pd.crop[1] == 0);           // 原点在左下（NVN 默认）：呈现时上下翻；裁剪按存储行号原样给
    CHECK(g_last_pd.crop[0] == 0 && g_last_pd.crop[2] == 320 && g_last_pd.crop[3] == 300 && g_last_pd.dst[0] == 256 && g_last_pd.dst[1] == 0 && g_last_pd.dst[2] == 768 && g_last_pd.dst[3] == 720);
    nvn_be_set_origin_lower_left(0); nvn_be_present(&t, 0, NULL); CHECK(g_last_pd.flip_y == 0); nvn_be_set_origin_lower_left(1);   // 原点在左上：不翻
    g_acq_i = 0; g_acq_script[0] = NVN_GPU_E_OUTDATED;              // 交换链过期：重建一次再取
    CHECK(nvn_be_present(&t, 0, NULL) == 5 && g_rebuilds == 1 && g_presents == 4 && nvn_be_presented_time(5) == 5.0);
    g_acq_i = 0; g_acq_script[0] = NVN_GPU_E_TIMEOUT;               // 没有空闲图：丢这一帧，不算故障
    CHECK(nvn_be_present(&t, 0, NULL) == 6 && g_presents == 4 && nvn_be_presented_time(6) == -1 && nvn_unimpl_count("present", "acquire_failed") == 0);
    uint64_t sh = 0, dp = 0; rp_present_stats(&sh, &dp); CHECK(sh == 4 && dp == 2);
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

int main(int argc, char **argv) {
    t_state_persists();
    t_unimpl_names();
    t_one_commit_per_submit();
    t_event_after_done();
    t_fence_sync();
    t_jump_call();
    t_bad_record();
    t_dirty_bits();
    t_deferred_bind();
    t_program_stages();
    t_program_bind_stages();
    // 一行一个调用：外部各支都往这里加用例，挤在一行时每两支必冲突
    t_pools();
    t_tex_init_registers_only();
    t_alias();
    t_alias_take_over();
    t_read_texels_guest();
    t_read_texels_gpu_owned();
    t_cpu_upload();
    t_cpu_rules();
    t_linear_upload();
    t_linear_reject();
    t_registry();
    t_decl_view();
    t_buf_mirror();
    t_buf_ops();
    t_copy_b2t();
    t_pass_clear_fold();
    t_pass_depth_shadow();
    t_pass_bc_shadow();
    t_pass_bc_shadow_3d();
    t_autoflush();
    t_pass_fresh_zero();
    t_frame_overlap();
    t_tex_fact_gens();
    t_pass_target_persists();
    t_pass_switch_and_partial();
    t_pass_rt_continue();
    t_pass_state_keep();
    t_pass_clear_park();
    t_pass_clear_park_tail();
    t_pass_clear_park_read();
    t_clear_rect();
    t_report_counter();
    t_occlusion();
    t_render_cond();
    t_dispatch();
    t_dispatch_indirect_gpu();
    t_dispatch_ubop_rw();
    t_dispatch_ubop_ro_skip();
    t_compute_image();
    t_compute_sources();
    t_copy_t2t();
    t_draw();
    t_arena697_detail();
    t_logic_op_accounting();
    t_gs_dirty_and_topology();
    t_gs_bind(); t_storage_sync_before_pass(); t_stage_dropped_clears_binds();
    t_tbuf_replay();
    t_decl_sampling();
    t_separate_sampler();
    t_present();
    t_view_pin();
    t_pass_3d_slice_view();
    t_bind_reuse();
    t_begin_fail();
    if (argc > 1 && t_table(argv[1])) g_fail++;
    printf("t_nvn_replay：%d 项检查，%d 项红\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
