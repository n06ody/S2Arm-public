// 渲染目标与通道（对齐 nvn_mtl.m：OP_SET_RT @14565、OP_CLEAR_COLOR @14578、OP_CLEAR_DS @14599、rp_begin_ex @5100 一带、rp_flush @5232）。
//   · OP_SET_RT 只换目标，不开通道；通道在下一笔绘制时才开。
//   · 整屏、全通道的清除不编码任何东西：记成「待折清除」，开通道时落成该附件的 load = CLEAR。
//     一直没有绘制：换目标或提交时为它开一个只做清除的通道。
//   · 目标绑定跨命令缓冲、跨提交保留（记录内的贴图快照拷一份自己留着）；打开的通道与待折清除不跨提交。
//   · 带裁剪或带写掩码的清除（裁剪矩形盖不满目标 / 只写部分颜色通道 / 只写部分模板位）：像绘制一样开通道（待折清除先落），
//     在通道里按矩形清（后端 nvn_gpu_clear_rect，写掩码一并交给它）。清完后端的管线与动态状态不保证，下一笔绘制前全部重发。
// 还没接：通道合并与存储前瞻（TBDR 上省带宽的手段，不影响画面）。
#include <time.h>
#include "rp_exec.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_fbtrace.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_fmt_info.h"
#include <math.h>
#include <string.h>

static struct {
    p_set_rt rt; tex_t kept[9]; int has_rt;
    uint8_t clear_c[8]; float cc[8][4];
    int clear_d, clear_s; float cd; int cs;
    int open;                 // 后端通道开着
    uint32_t barrier_bits;    // 游戏发过的屏障位：下一个通道前交给后端
    int has_ds, ds_used, ndraw;   // 当前通道：挂了深度模板附件、有绘制 / 清除用到它、绘制笔数（读数用）
} P;

// 视图看到的尺寸（像素）。块压缩基底配非压缩视图（一块当一个像素，见 bc_view）时按块数算：通道、整屏清除判断、清除矩形都用它
static int bc_view(const tex_t *t, const view_t *v, int has_v, uint32_t *bw, uint32_t *bh);
static void level_size(const tex_t *t, const view_t *v, int has_v, uint32_t *w, uint32_t *h) {
    uint32_t l = has_v && v->set_levels ? v->base_level : 0;
    *w = t->d.width >> l; *h = t->d.height >> l;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
    uint32_t bw, bh; if (bc_view(t, v, has_v, &bw, &bh)) { *w = (*w + bw - 1) / bw; *h = (*h + bh - 1) / bh; }
}
static int scissor_full(const tex_t *t, const view_t *v, int has_v) {
    if (!rp_R.has_sc) return 1;
    uint32_t w, h; level_size(t, v, has_v, &w, &h);
    return rp_R.sc.x <= 0 && rp_R.sc.y <= 0 && (int64_t)rp_R.sc.x + rp_R.sc.w >= (int64_t)w && (int64_t)rp_R.sc.y + rp_R.sc.h >= (int64_t)h;
}
// 裁剪矩形与目标（该级）求交。0 = 交集为空（这次清除什么都不动）
static int scissor_rect(const tex_t *t, const view_t *v, int has_v, nvn_gpu_clear *o) {
    uint32_t w, h; level_size(t, v, has_v, &w, &h);
    int64_t x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (rp_R.has_sc) {
        if (rp_R.sc.x > x0) x0 = rp_R.sc.x;
        if (rp_R.sc.y > y0) y0 = rp_R.sc.y;
        if ((int64_t)rp_R.sc.x + rp_R.sc.w < x1) x1 = (int64_t)rp_R.sc.x + rp_R.sc.w;
        if ((int64_t)rp_R.sc.y + rp_R.sc.h < y1) y1 = (int64_t)rp_R.sc.y + rp_R.sc.h;
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    o->x = (int32_t)x0; o->y = (int32_t)y0; o->w = (uint32_t)(x1 - x0); o->h = (uint32_t)(y1 - y0);
    return 1;
}
// 清除值按附件格式的数值类别换算（整数格式的转换同 iOS 的清除三角形，nvn_mtl.m:14426）。out 是 16 字节的 f / u / i 联合
static void color_value(int i, const float rgba[4], void *out) {
    const tex_t *t = P.rt.color[i];
    nvn_fmt_info fi; int kind = nvn_fmt_info_of((uint32_t)(P.rt.has_cview[i] && P.rt.cview[i].set_format ? P.rt.cview[i].format : t->d.format), &fi) ? fi.kind : NVN_FMT_UNORM;
    union { float f[4]; uint32_t u[4]; int32_t i[4]; } v;
    for (int k = 0; k < 4; k++) { float f = rgba[k];
        if (kind == NVN_FMT_UINT) v.u[k] = isnan(f) || f <= 0 ? 0u : f >= 4294967295.0f ? 0xffffffffu : (uint32_t)f;
        else if (kind == NVN_FMT_SINT) v.i[k] = isnan(f) ? 0 : f >= 2147483647.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
        else v.f[k] = f; }
    memcpy(out, &v, sizeof v);
}
static float depth_value(float d) { return isnan(d) ? 0 : d < 0 ? 0 : d > 1 ? 1 : d; }
// 颜色槽 i 挂的图（同一级、同一层）在更靠前的槽里已经挂过。记录里是贴图对象的快照，按序号比
static int dup_slot(int i) {
    const tex_t *t = P.rt.color[i]; if (!t) return 0;
    uint32_t lv = P.rt.has_cview[i] && P.rt.cview[i].set_levels ? P.rt.cview[i].base_level : 0, ly = P.rt.has_cview[i] && P.rt.cview[i].set_layers ? P.rt.cview[i].min_layer : 0;
    for (int j = 0; j < i; j++) { const tex_t *u = P.rt.color[j]; if (!u || u->serial != t->serial) continue;
        uint32_t lj = P.rt.has_cview[j] && P.rt.cview[j].set_levels ? P.rt.cview[j].base_level : 0, yj = P.rt.has_cview[j] && P.rt.cview[j].set_layers ? P.rt.cview[j].min_layer : 0;
        if (lj == lv && yj == ly) return 1; }
    return 0;
}

// ---------- 深度 / 块压缩贴图经颜色视图当渲染目标：影子 ----------
// 对照 nvn_mtl.m shrt_block @4762 / bcrt_view @4778 / bcrt_attach @4801 / bcrt_flush @4792：深度格式的图不能当颜色附件，也建不了
// 颜色格式的视图。画进一张视图格式的影子：通道前把基底那一级那一层的内容逐字节拷进影子，通道后拷回。只有视图一个像素的字节
// 等于基底一个像素的字节、且基底的存储字节就是游戏的字节的才走（D16、D32F；D24S8 不算）。影子按槽常驻复用。
// 块压缩格式同一套影子（#709，iOS bcrt_attach）：视图一个像素的字节等于基底一块的字节（BC1 / BC4 8 字节，其余 16），一块当一个像素，
// 影子按块数建；基底那边拷贝区域按像素给（整级），缓冲里一行就是一行块。基底在本设备上不能直接采样（后端走计算解码，存储里已经不是
// 游戏的字节）的不走，照旧当空槽记账。
// 三维的块压缩基底（#773，iOS bcrt_attach 的 z = depthPlane；一款程序的 32x32x32 三维压缩贴图一个深度切片一个通道）同一套影子：视图的
// 层号在三维贴图上是深度切片，原样当基底拷贝的 layer——后端 nvn_gpu_copy_tex_buf / copy_buf_tex 在三维贴图上把它转成 zoffset、depth 1
// （越界回 E_ARG）；影子仍是二维的块宽 x 块高，不建三维视图（附件视图见下面的 att_view，不经这条路）。
typedef struct { nvn_gpu_tex g; nvn_gpu_view v; uint32_t fmt, w, h; } shadow_t;
static shadow_t g_sh[8];
static struct { nvn_gpu_tex base; uint32_t level, layer, w, h, bytes, bw, bh; } g_shu[8];   // 当前通道里各槽用着的影子（base 为空 = 没用）；w、h 是影子的尺寸，bw、bh 是基底那一级的像素尺寸

// 块压缩基底配「一块当一个像素」的非压缩视图：回 1 并给块的宽高。只看格式，不看能不能挂（slot_kind 判）
static int bc_view(const tex_t *t, const view_t *v, int has_v, uint32_t *bw, uint32_t *bh) {
    nvn_fmt_info b, f;
    if (!has_v || !v->set_format || !nvn_fmt_info_of((uint32_t)t->d.format, &b) || (b.bw <= 1 && b.bh <= 1)) return 0;
    if (!nvn_fmt_info_of(v->format, &f) || f.bw != 1 || f.bh != 1 || f.depth || f.stencil || f.expand || f.bytes != b.bytes) return 0;
    *bw = b.bw; *bh = b.bh;
    return 1;
}
// 颜色槽 i：0 = 照常挂；1 = 走影子（*bytes 一个像素的字节）；-1 = 挂不了（*why 记账名）
static int slot_kind(int i, uint32_t *bytes, const char **why) {
    const tex_t *t = P.rt.color[i]; nvn_fmt_info b, v;
    if (!nvn_fmt_info_of((uint32_t)t->d.format, &b)) return 0;
    if (b.bw > 1 || b.bh > 1) {
        if (why) *why = "color_target_compressed";
        uint32_t bw, bh;
        if (!bc_view(t, &P.rt.cview[i], P.rt.has_cview[i], &bw, &bh)) return -1;
        if (!(nvn_gpu_format_caps((uint32_t)t->d.format) & NVN_GPU_FMT_SAMPLE)) return -1;   // 设备不能直接采样（后端走计算解码）：存储里不是游戏的字节，不能逐字节拷
        if (!(nvn_gpu_format_caps(P.rt.cview[i].format) & NVN_GPU_FMT_RENDER)) return -1;
        if (bytes) *bytes = b.bytes;
        return 1;
    }
    if (!b.depth && !b.stencil) return 0;
    if (why) *why = "color_target_depth";
    if (b.stencil || (b.bytes != 2 && !(b.bytes == 4 && b.kind == NVN_FMT_FLOAT)) || !P.rt.has_cview[i] || !P.rt.cview[i].set_format) return -1;
    if (!nvn_fmt_info_of(P.rt.cview[i].format, &v) || v.bw != 1 || v.bh != 1 || v.depth || v.stencil || v.expand || v.bytes != b.bytes) return -1;
    if (!(nvn_gpu_format_caps(P.rt.cview[i].format) & NVN_GPU_FMT_RENDER)) return -1;
    if (bytes) *bytes = b.bytes;
    return 1;
}
static void shadow_copy(nvn_gpu_cmd c, int i, int back) {
    uint64_t row = (uint64_t)g_shu[i].w * g_shu[i].bytes, img = row * g_shu[i].h;   // 缓冲里一行 = 影子一行 = 基底一行块
    nvn_gpu_slice sl; nvn_gpu_err e = {0};
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, img, 16, &sl, &e))) { nvn_unimpl("pass", "depth_target_shadow_staging", NULL); return; }
    NVNcopyRegion r = {0, 0, 0, (int)g_shu[i].w, (int)g_shu[i].h, 1};     // 影子那边（像素 = 块）
    NVNcopyRegion rb = {0, 0, 0, (int)g_shu[i].bw, (int)g_shu[i].bh, 1};  // 基底那边按像素：深度影子与 r 相同，块压缩是整级的像素尺寸
    if (!back) { nvn_gpu_copy_tex_buf(c, g_shu[i].base, g_shu[i].level, g_shu[i].layer, &rb, sl.buf, sl.off, row, img); nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, row, img, g_sh[i].g, 0, 0, &r); }
    else { nvn_gpu_copy_tex_buf(c, g_sh[i].g, 0, 0, &r, sl.buf, sl.off, row, img); nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, row, img, g_shu[i].base, g_shu[i].level, g_shu[i].layer, &rb); }
}
// 回影子的视图（当附件用）；开不了回 NULL（已记账）。cleared：这个槽开通道时整屏清除，不用先拷基底的内容
static nvn_gpu_view shadow_attach(nvn_gpu_cmd c, int i, const tex_t *t, uint32_t bytes, int cleared) {
    uint32_t w, h, fmt = P.rt.cview[i].format; level_size(t, &P.rt.cview[i], 1, &w, &h);
    shadow_t *s = &g_sh[i];
    if (!s->g || s->fmt != fmt || s->w != w || s->h != h) {
        if (s->v) rp_fbtrace_release(s->v, FT_SHADOW);
        if (s->g) nvn_gpu_tex_release(s->g);
        memset(s, 0, sizeof *s);
        nvn_gpu_tex_desc d; memset(&d, 0, sizeof d);
        d.d.target = NVN_TEXTURE_TARGET_2D; d.d.format = (int)fmt; d.d.width = w; d.d.height = h; d.d.depth = 1; d.d.levels = 1; d.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER;
        nvn_gpu_err e = {0};
        if (!nvn_gpu_ok(nvn_gpu_tex_create(&d, &s->g, &e)) || !nvn_gpu_ok(nvn_gpu_view_create(s->g, NULL, &s->v, &e))) {
            if (s->g) nvn_gpu_tex_release(s->g);
            memset(s, 0, sizeof *s);
            if (nvn_unimpl("pass", "depth_target_shadow_create", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", e.why);
            return NULL;
        }
        s->fmt = fmt; s->w = w; s->h = h;
    }
    nvn_gpu_tex base = rp_tex_resolve(t, c);
    if (!base) return NULL;
    g_shu[i].base = base; g_shu[i].level = P.rt.cview[i].set_levels ? P.rt.cview[i].base_level : 0; g_shu[i].layer = P.rt.cview[i].set_layers ? P.rt.cview[i].min_layer : 0;
    g_shu[i].w = w; g_shu[i].h = h; g_shu[i].bytes = bytes;
    { uint32_t l = g_shu[i].level; g_shu[i].bw = t->d.width >> l; g_shu[i].bh = t->d.height >> l;   // 基底那一级的像素尺寸（不按块取整：拷贝区域到级的边上）
      if (!g_shu[i].bw) g_shu[i].bw = 1;
      if (!g_shu[i].bh) g_shu[i].bh = 1; }
    if (!cleared) shadow_copy(c, i, 0);
    return s->v;
}
// ---------- 3D 贴图的深度切片当附件 ----------
// NVN 视图的层号在 3D 贴图上是深度切片（nvn_mtl.m attach() 落到 depthPlane）。Vulkan 的 3D 视图起点只能是 0，深度子范围只能用 2D（阵列）视图
//（nvn_vk_texview.h 文件头与 plan_make 的错误信息都这么说），所以 3D 附件带层号时请求 2D 阵列目标，层号、级别等其余字段原样。
// 游戏自己设了目标的、没带层号的（首片）、不是 3D 的贴图：视图记录原样。tmp 由调用方给，生命期同这次取视图。
static const view_t *att_view(const tex_t *t, int has_v, const view_t *v, view_t *tmp) {
    if (!has_v) return NULL;
    if (t->d.target != NVN_TEXTURE_TARGET_3D || !v->set_layers || v->set_target) return v;
    *tmp = *v; tmp->set_target = 1; tmp->target = NVN_TEXTURE_TARGET_2D_ARRAY;
    return tmp;
}
void rp_pass_note_draw(int uses_ds) { P.ndraw++; if (uses_ds) P.ds_used = 1; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { level_size(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i], w, h); }
int rp_pass_slot_attached(int i) { return P.has_rt && i >= 0 && i < P.rt.n && i < 8 && P.rt.color[i] && !dup_slot(i) && slot_kind(i, NULL, NULL) >= 0; }
static int pending(void) { if (P.clear_d || P.clear_s) return 1; for (int i = 0; i < 8; i++) if (P.clear_c[i]) return 1; return 0; }

void rp_pass_end(nvn_gpu_cmd c) {
    if (P.open && c) { nvn_gpu_pass_end(c); for (int i = 0; i < 8; i++) if (g_shu[i].base) shadow_copy(c, i, 1); }   // 画进影子的内容拷回各自的深度贴图
    if (P.open) { if (P.has_ds) { rp_perf_add(RP_PERF_P_DS, 1); if (!P.ds_used) rp_perf_add(RP_PERF_P_DS_IDLE, 1); } if (P.ndraw <= 1) rp_perf_add(RP_PERF_P_ONE, 1); }
    memset(g_shu, 0, sizeof g_shu);
    P.open = 0; rp_occl_pass_closed();
}

// 开通道（已开着直接回 1）。0 = 开不了（已记账）：调用方跳过这笔绘制。
static uint64_t pb_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#define PB(step) do { uint64_t n_ = pb_now(); rp_perf_add(step, n_ - pb_); pb_ = n_; } while (0)
int rp_pass_begin(nvn_gpu_cmd c) {
    if (P.open) return 1;
    if (!c || !P.has_rt) { if (c) nvn_unimpl("pass", "draw_without_target", NULL); return 0; }
    int any_color = 0; for (int i = 0; i < P.rt.n && i < 8; i++) if (P.rt.color[i]) any_color = 1;
    if (P.rt.n < 0 || P.rt.n > 8 || (!any_color && !P.rt.depth)) { nvn_unimpl("pass", "no_attachment", NULL); return 0; }
    rp_view_pin_begin();   // 下面取到的视图在本函数结束前不能被轮转放掉
    uint64_t pb_ = pb_now();   // 四段计时：取附件视图 / 屏障 / 后端开通道 / 记 GPU 写过
    nvn_gpu_pass_desc d; memset(&d, 0, sizeof d);
    int nattached = 0;
    for (int i = 0; i < P.rt.n; i++) {
        const tex_t *t = P.rt.color[i];
        if (!t) continue;   // 颜色槽里的空位：描述里留空（view = NULL），后端按「不用的槽」处理
        if (dup_slot(i)) continue;   // 同一张图的同一级同一层挂了两个槽：只挂前一个（nvn_mtl.m rt_dup_slot @4797）
        uint32_t sbytes = 0; const char *why = NULL; int kind = slot_kind(i, &sbytes, &why);
        if (kind < 0) { nvn_unimpl("pass", why, t); continue; }   // 深度 / 块压缩格式的图挂在颜色槽、又走不了影子：当空槽（画到这个槽的内容丢掉）
        view_t av;
        uint64_t sh0_ = kind ? pb_now() : 0;
        nvn_gpu_view v = kind ? shadow_attach(c, i, t, sbytes, P.clear_c[i]) : rp_view_for_attach(t, att_view(t, P.rt.has_cview[i], &P.rt.cview[i], &av), c);
        if (kind) rp_perf_add(RP_PERF_PB_SHADOW, pb_now() - sh0_);
        if (!v) { memset(g_shu, 0, sizeof g_shu); rp_view_pin_end(); return 0; }
        nattached++;
        d.color[i].view = v; d.color[i].store = NVN_GPU_STORE_STORE;
        d.color[i].load = P.clear_c[i] ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD;
        if (P.clear_c[i]) color_value(i, P.cc[i], &d.color[i].clear);
        if (!d.w) level_size(t, &P.rt.cview[i], P.rt.has_cview[i], &d.w, &d.h);   // 尺寸取第一个有图的槽
    }
    d.ncolor = (uint32_t)P.rt.n; d.layers = 1;
    if (!nattached && !P.rt.depth) { rp_view_pin_end(); nvn_unimpl("pass", "no_attachment", NULL); return 0; }   // 颜色槽全成了空槽、又没有深度：这个通道的绘制跳过（同 iOS）
    if (!d.w && P.rt.depth) level_size(P.rt.depth, &P.rt.dview, P.rt.has_dview, &d.w, &d.h);   // 只有深度的通道（阴影图、深度预通道）：尺寸取深度目标的
    if (P.rt.depth) {
        nvn_fmt_info fi; int ok = nvn_fmt_info_of((uint32_t)P.rt.depth->d.format, &fi);
        view_t av;
        nvn_gpu_view v = rp_view_for(P.rt.depth, att_view(P.rt.depth, P.rt.has_dview, &P.rt.dview, &av), c);
        if (!v) { memset(g_shu, 0, sizeof g_shu); rp_view_pin_end(); return 0; }
        if (!ok || fi.depth) { d.depth.view = v; d.depth.store = NVN_GPU_STORE_STORE; d.depth.load = P.clear_d ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD; d.clear_depth = P.cd; }
        if (ok && fi.stencil) { d.stencil.view = v; d.stencil.store = NVN_GPU_STORE_STORE; d.stencil.load = P.clear_s ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD; d.clear_stencil = (uint32_t)P.cs & 0xff; }
    }
    PB(RP_PERF_PB_VIEW);
    if (P.barrier_bits) { nvn_gpu_barrier(c, P.barrier_bits); P.barrier_bits = 0; }
    PB(RP_PERF_PB_BAR);
    d.occlusion = (uint8_t)rp_occl_want();
    P.has_ds = d.depth.view || d.stencil.view; P.ds_used = P.clear_d || P.clear_s; P.ndraw = 0;
    for (int i = 0; i < P.rt.n && i < 8; i++) if (d.color[i].view && d.color[i].load == NVN_GPU_LOAD_LOAD) rp_perf_add(RP_PERF_P_LOADC, 1);
    rp_perf_add(RP_PERF_P_KPIX, (uint64_t)d.w * d.h / 1000);
    for (int i = 0; i < P.rt.n && i < 8; i++)   // 刚建、该是全零的目标：不在 CPU 上搬一整张零，这个通道载入时清成零（已有整屏清除的照它的值清）
        if (d.color[i].view && rp_tex_take_zero(P.rt.color[i]) && d.color[i].load != NVN_GPU_LOAD_CLEAR) { d.color[i].load = NVN_GPU_LOAD_CLEAR; memset(&d.color[i].clear, 0, sizeof d.color[i].clear); }
    nvn_gpu_pass_begin(c, &d);
    PB(RP_PERF_PB_BE);
    rp_view_pin_end();
    rp_occl_pass_opened(d.occlusion);
    rp_perf_add(RP_PERF_PASSES, 1);
    for (int i = 0; i < P.rt.n; i++) if (P.rt.color[i]) rp_tex_mark_gpu_written(P.rt.color[i]);
    if (P.rt.depth) rp_tex_mark_gpu_written(P.rt.depth);
    PB(RP_PERF_PB_MARK);
    memset(P.clear_c, 0, sizeof P.clear_c); P.clear_d = P.clear_s = 0;
    P.open = 1;
    rp_dirty_all();   // 新通道：已发给后端的管线 / 动态状态 / 绑定全部重发
    return 1;
}

// 结束当前通道；有待折清除而没画过的，为它开一个只做清除的通道
void rp_pass_flush(nvn_gpu_cmd c) {
    if (!P.open && pending() && c && P.has_rt) rp_pass_begin(c);
    rp_pass_end(c);
    if (!c) { memset(P.clear_c, 0, sizeof P.clear_c); P.clear_d = P.clear_s = 0; }   // 没有段：清除无处落，丢弃（提交已按失败处理）
}

void rp_pass_set_rt(nvn_gpu_cmd c, const p_set_rt *r) {
    if (P.open) rp_perf_add(RP_PERF_P_CUT_RT, 1);
    rp_pass_flush(c);
    P.rt = *r; P.has_rt = 1;
    // 记录里的贴图指针指向记录内的快照，记录所在的命令内存这次提交之后可能被复用：自己留一份（nvn_mtl.m rt_keep_state @4453）
    for (int i = 0; i < 8; i++) if (P.rt.color[i]) { P.kept[i] = *P.rt.color[i]; P.rt.color[i] = &P.kept[i]; }
    if (P.rt.depth) { P.kept[8] = *P.rt.depth; P.rt.depth = &P.kept[8]; }
}

void rp_pass_clear_color(nvn_gpu_cmd c, const p_clear_color *cc, const void *rec) {
    if (!P.has_rt || cc->index < 0 || cc->index >= P.rt.n || cc->index >= 8 || !P.rt.color[cc->index]) { nvn_unimpl("pass", "clear_color_no_target", rec); return; }
    if (!(cc->mask & 0xf)) return;
    int i = cc->index;
    if ((cc->mask & 0xf) != 0xf || !scissor_full(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i])) {
        nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color = i; cl.color_mask = (uint8_t)(cc->mask & 0xf); cl.stencil_mask = 0xff;
        if (!scissor_rect(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i], &cl)) return;
        if (!c || !rp_pass_begin(c)) { if (c) nvn_unimpl("pass", "clear_rect_pass_begin_failed", rec); return; }
        color_value(i, cc->rgba, &cl.value);
        rp_occl_pause(c);
        nvn_gpu_clear_rect(c, &cl);
        rp_dirty_all();
        return;
    }
    if (P.open) rp_perf_add(RP_PERF_P_CUT_CLEAR, 1);
    if (P.open) rp_pass_end(c);   // 通道中的整屏清除：结束这一通道，清除挂到下一个通道开头（其余附件照常 LOAD）
    P.clear_c[i] = 1; memcpy(P.cc[i], cc->rgba, sizeof P.cc[i]);
}

void rp_pass_clear_ds(nvn_gpu_cmd c, const p_clear_ds *cd, const void *rec) {
    if (!P.has_rt || !P.rt.depth) { nvn_unimpl("pass", "clear_ds_no_target", rec); return; }
    int sm = cd->stencil_mask & 0xff;
    if (!cd->depth_mask && !sm) return;
    // 目标实际有哪些面：没有的面这次清除对它无事可做（同 rp_pass_begin 的附件判定）
    nvn_fmt_info fi; int ok = nvn_fmt_info_of((uint32_t)P.rt.depth->d.format, &fi);
    int do_d = cd->depth_mask && (!ok || fi.depth), do_s = sm && ok && fi.stencil;
    if (!do_d && !do_s) return;
    if (!scissor_full(P.rt.depth, &P.rt.dview, P.rt.has_dview) || (do_s && sm != 0xff)) {
        nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color = -1; cl.color_mask = 0xf; cl.stencil_mask = (uint8_t)(do_s ? sm : 0xff);
        if (!scissor_rect(P.rt.depth, &P.rt.dview, P.rt.has_dview, &cl)) return;
        if (!c || !rp_pass_begin(c)) { if (c) nvn_unimpl("pass", "clear_rect_pass_begin_failed", rec); return; }
        cl.depth = (uint8_t)do_d; cl.stencil = (uint8_t)do_s; cl.depth_value = depth_value(cd->depth); cl.stencil_value = (uint32_t)cd->stencil & 0xff;
        rp_occl_pause(c);
        nvn_gpu_clear_rect(c, &cl);
        rp_dirty_all();
        return;
    }
    if (P.open) rp_pass_end(c);
    if (do_d) { P.clear_d = 1; P.cd = depth_value(cd->depth); }
    if (do_s) { P.clear_s = 1; P.cs = cd->stencil; }
}

// 游戏的屏障：不拆通道，记下位，下一个通道前交给后端（nvn_mtl.m:14646 的缺省路径；通道内着色器写图像后要求片元顺序的拆通道随图像绑定一起接）
void rp_pass_barrier(uint32_t bits) { P.barrier_bits |= bits; }

void rp_pass_reset(void) {
    memset(&P, 0, sizeof P); memset(g_shu, 0, sizeof g_shu);
    for (int i = 0; i < 8; i++) { if (g_sh[i].v) rp_fbtrace_release(g_sh[i].v, FT_RESET); if (g_sh[i].g) nvn_gpu_tex_release(g_sh[i].g); }
    memset(g_sh, 0, sizeof g_sh);
}
int rp_pass_is_open(void) { return P.open; }
const p_set_rt *rp_pass_target(void) { return P.has_rt ? &P.rt : NULL; }
