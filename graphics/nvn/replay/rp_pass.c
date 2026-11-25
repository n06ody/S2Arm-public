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
#include "nvn_first_use.h"   // 首用跳过判据的 fresh 标记（iOS rt_fresh_now）
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_fmt_info.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// 弱链接登记（tools/weak_link_decl.py，场合 replay = 回放核心宿主单测）：后端 / 首用模块的可选件，缺实现时地址为空、走不到调用
// 弱链接[replay]：s2a_fu_rt_mark_fresh

// 首用跳过开关（安卓 demand 层，弱引用）：iOS / 鸿蒙 / 宿主单测没有实现 → 不标记，行为同现状。
int nvn_vk_pipe_demand_on(void) __attribute__((weak));
#pragma weak s2a_fu_rt_mark_fresh   // 只编回放核的宿主门禁可能没链接 nvn_first_use.c；缺实现时 enabled 为假、不会调用
static int rp_pass_fu_enabled(void) { return nvn_vk_pipe_demand_on && nvn_vk_pipe_demand_on(); }

static struct {
    p_set_rt rt; tex_t kept[9]; int has_rt;
    int open;                 // 后端通道开着
    uint8_t colors, want_colors;   // 当前通道 / 下一笔绘制要挂的颜色槽；清除可独立恢复目标
    uint8_t layered, want_layered; // 当前通道 / 下一笔绘制是不是分层通道（程序的顶点 / 几何阶段写层号，见 rp_pass_want_layered）
    uint8_t na_from_rt;            // 全零掩码：无附件，但光栅范围仍受逻辑目标尺寸限制
    uint32_t barrier_bits;    // 游戏发过的屏障位：下一个通道前交给后端
    int sw_pend, sw_split;    // 当前通道里有绘制的着色器写了存储缓冲 / 存储图像；其后游戏发过带顺序位的屏障 = 下一笔绘制前要结束通道（见 rp_pass_barrier）
    int has_ds, ds_used, ndraw;   // 当前通道：挂了深度模板附件、有绘制 / 清除用到它、绘制笔数（读数用）
    uint32_t cur_kpix;            // 当前通道的千像素（读数用：0–1 笔通道各吃掉多少像素）
    // 渲染区域（开关 debug.s2a.nvk_pass_area）：附件只在区域内载入 / 保存。区域只按「裁剪矩形」收缩——后端把裁剪矩形原样交给
    // Vulkan，光栅出来的像素一定在它里面，所以收缩一定安全；视口带原点翻转，先不参与（要按窗口原点翻 y，等有真机验证再加）。
    int area_on; int32_t ax, ay; uint32_t aw, ah;   // 当前通道的区域（帧缓冲坐标）
    int area_want; int32_t wx, wy; uint32_t ww, wh; // 下一笔绘制要求的区域（rp_pass_prepare_draw 设，rp_pass_begin 用）
    // 上一个通道的管线形状（各颜色槽的附件格式、深度 / 模板格式、采样数）：Vulkan 的管线对象与渲染通道的格式绑在一起，
    // 形状变了必须重取管线（见 rp_pass_begin 末尾）；形状没变、目标只是换了张同格式的图，管线与绑定都不用重发
    uint32_t shape_c[8], shape_d, shape_s, shape_smp; int shape_ok;
    uint32_t na_w, na_h;          // 无附件通道（一个附件都没挂，只为片元副作用 / 遮挡计数）的帧缓冲尺寸；有附件的通道是 0
    uint32_t pass_w, pass_h;    // 实际公共通道尺寸；不跨视图 pin 窗口保存可重录的后端句柄
} P = {.want_colors = 0xff};

// 视图看到的尺寸（像素）。块压缩基底配非压缩视图（一块当一个像素，见 bc_view）时按块数算：通道、整屏清除判断、清除矩形都用它
static int bc_view(const tex_t *t, const view_t *v, int has_v, uint32_t *bw, uint32_t *bh);
static void level_size(const tex_t *t, const view_t *v, int has_v, uint32_t *w, uint32_t *h) {
    uint32_t l = has_v && v->set_levels ? v->base_level : 0;
    *w = t->d.width >> l; *h = t->d.height >> l;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
    uint32_t bw, bh; if (bc_view(t, v, has_v, &bw, &bh)) { *w = (*w + bw - 1) / bw; *h = (*h + bh - 1) / bh; }
}
static void extent_min(uint32_t *w, uint32_t *h, uint32_t aw, uint32_t ah) {
    if (!*w || aw < *w) *w = aw;
    if (!*h || ah < *h) *h = ah;
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
static int dup_slot(int i, uint8_t colors) {
    const tex_t *t = P.rt.color[i]; if (!t) return 0;
    uint32_t lv = P.rt.has_cview[i] && P.rt.cview[i].set_levels ? P.rt.cview[i].base_level : 0, ly = P.rt.has_cview[i] && P.rt.cview[i].set_layers ? P.rt.cview[i].min_layer : 0;
    for (int j = 0; j < i; j++) { const tex_t *u = P.rt.color[j]; if (!(colors >> j & 1) || !u || u->serial != t->serial) continue;
        uint32_t lj = P.rt.has_cview[j] && P.rt.cview[j].set_levels ? P.rt.cview[j].base_level : 0, yj = P.rt.has_cview[j] && P.rt.cview[j].set_layers ? P.rt.cview[j].min_layer : 0;
        if (lj == lv && yj == ly) return 1; }
    return 0;
}
// 先按这一笔的有效写掩码筛掉，再去重；早先的零掩码别名不能吃掉后面的可写槽。
static uint8_t select_colors(uint8_t wanted) {
    uint8_t selected = 0;
    for (int i = 0; i < P.rt.n && i < 8; i++)
        if ((wanted >> i & 1) && P.rt.color[i] && !dup_slot(i, selected)) selected |= (uint8_t)(1u << i);
    return selected;
}
static int pass_begin_colors(nvn_gpu_cmd c, uint8_t colors, int layered, int clear_only);

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
// 返回值只反映可见的 staging 分配结果；void 拷贝编码器的 sticky 错误仍由提交报告。
static int shadow_copy(nvn_gpu_cmd c, int i, int back) {
    uint64_t row = (uint64_t)g_shu[i].w * g_shu[i].bytes, img = row * g_shu[i].h;   // 缓冲里一行 = 影子一行 = 基底一行块
    nvn_gpu_slice sl; nvn_gpu_err e = {0};
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, img, 16, &sl, &e))) { nvn_unimpl("pass", back ? "shadow_copyback_staging" : "shadow_load_staging", P.rt.color[i]); return 0; }
    NVNcopyRegion r = {0, 0, 0, (int)g_shu[i].w, (int)g_shu[i].h, 1};     // 影子那边（像素 = 块）
    NVNcopyRegion rb = {0, 0, 0, (int)g_shu[i].bw, (int)g_shu[i].bh, 1};  // 基底那边按像素：深度影子与 r 相同，块压缩是整级的像素尺寸
    if (!back) { nvn_gpu_copy_tex_buf(c, g_shu[i].base, g_shu[i].level, g_shu[i].layer, &rb, sl.buf, sl.off, row, img); nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, row, img, g_sh[i].g, 0, 0, &r); }
    else { nvn_gpu_copy_tex_buf(c, g_sh[i].g, 0, 0, &r, sl.buf, sl.off, row, img); nvn_gpu_copy_buf_tex(c, sl.buf, sl.off, row, img, g_shu[i].base, g_shu[i].level, g_shu[i].layer, &rb); }
    return 1;
}
// 回影子的视图（当附件用）；开不了回 NULL（已记账）。cleared：这个槽开通道时整屏清除，不用先拷基底的内容
static nvn_gpu_view shadow_attach(nvn_gpu_cmd c, int i, const tex_t *t, uint32_t bytes, int cleared) {
    uint32_t w, h, fmt = P.rt.cview[i].format; level_size(t, &P.rt.cview[i], 1, &w, &h);
    shadow_t *s = &g_sh[i];
    if (!s->g || s->fmt != fmt || s->w != w || s->h != h) {
        if (s->v) nvn_gpu_view_release(s->v);
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
    if (!cleared && !shadow_copy(c, i, 0)) { memset(&g_shu[i], 0, sizeof g_shu[i]); return NULL; }
    return s->v;
}
// ---------- 3D 贴图的深度切片当附件 ----------
// NVN 视图的层号在 3D 贴图上是深度切片（nvn_mtl.m attach() 落到 depthPlane）。Vulkan 的 3D 视图起点只能是 0，深度子范围只能用 2D（阵列）视图
//（nvn_vk_texview.h 文件头与 plan_make 的错误信息都这么说），所以 3D 附件带层号时请求 2D 阵列目标，层号、级别等其余字段原样。
// 游戏自己把目标设成 3D 也一样：Vulkan 的 3D 视图本身就不能当附件（不管起点是不是 0），附件只认层号，视图上的 3D 目标对附件没有意义
//（真机实测有游戏对 3D 贴图的第 k 片显式设「3D 目标 + 层号 k」，原样递下去，后端就在通用视图那一步拒绝，#724）。
// 游戏设的是 2D / 2D 阵列等附件合法的目标、没带层号的（首片）、不是 3D 的贴图：视图记录原样。tmp 由调用方给，生命期同这次取视图。
static const view_t *att_view(const tex_t *t, int has_v, const view_t *v, view_t *tmp) {
    if (!has_v) return NULL;
    if (t->d.target != NVN_TEXTURE_TARGET_3D || !v->set_layers || (v->set_target && v->target != NVN_TEXTURE_TARGET_3D)) return v;
    *tmp = *v; tmp->set_target = 1; tmp->target = NVN_TEXTURE_TARGET_2D_ARRAY;
    return tmp;
}
// 分层通道（顶点 / 几何阶段写层号）里附件从挂接层起能给几层，同 iOS nvn_mtl.m 开分层通道时 renderTargetArrayLength 的取法：
//   3D = 该级的深度切片数 − 起始切片；2D 阵列 / 多重采样阵列 / 立方体阵列 = 层数 − 起始层；立方体 = 6 面 − 起始面；其余 1。
//   视图显式设了层数的不超过它（NVN 视图的层范围就是这张目标可写的层）。
static uint32_t att_layers_avail(const tex_t *t, int has_v, const view_t *v) {
    const uint32_t lv = has_v && v->set_levels ? v->base_level : 0, from = has_v && v->set_layers ? v->min_layer : 0;
    uint32_t all;
    switch (t->d.target) {
    case NVN_TEXTURE_TARGET_3D: all = (uint32_t)t->d.depth >> lv; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: all = (uint32_t)t->d.depth; break;
    case NVN_TEXTURE_TARGET_CUBEMAP: all = 6; break;
    default: return 1;
    }
    uint32_t n = all > from ? all - from : 1;
    if (has_v && v->set_layers && v->nlayers && v->nlayers < n) n = v->nlayers;
    return n;
}
// 分层通道的附件视图：起始层起 nl 层的二维阵列视图（3D 贴图的深度切片在 Vulkan 里只能经二维阵列视图当附件，见 att_view；
//   附件只取一级）。nl <= 1 照旧走 att_view。tmp 由调用方给，生命期同这次取视图。
static const view_t *att_view_n(const tex_t *t, int has_v, const view_t *v, view_t *tmp, uint32_t nl) {
    if (nl <= 1) return att_view(t, has_v, v, tmp);
    if (has_v) *tmp = *v; else memset(tmp, 0, sizeof *tmp);
    const uint32_t lv = has_v && v->set_levels ? v->base_level : 0, from = has_v && v->set_layers ? v->min_layer : 0;
    tmp->set_levels = 1; tmp->base_level = (uint8_t)lv; tmp->nlevels = 1;
    tmp->set_layers = 1; tmp->min_layer = (uint16_t)from; tmp->nlayers = (uint16_t)nl;
    tmp->set_target = 1; tmp->target = NVN_TEXTURE_TARGET_2D_ARRAY;
    return tmp;
}
void rp_pass_note_draw(int note) { P.ndraw++; if (note & RP_NOTE_DS) P.ds_used = 1; if (note & RP_NOTE_SHADER_WRITE) P.sw_pend = 1; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { level_size(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i], w, h); }
void rp_pass_noatt_size(uint32_t *w, uint32_t *h) { if (P.na_w && P.na_h) { *w = P.na_w; *h = P.na_h; } }

// 无附件通道的帧缓冲尺寸。NVN 允许不挂任何渲染目标照样绘制（片元着色器只写存储缓冲 / 图像、或只为遮挡计数），
// 光栅范围由视口与裁剪矩形定：片元落不出视口，所以帧缓冲盖住视口 0 的右下角就够；没设视口时用裁剪矩形；两样都没有返回 0。
// 夹到设备的最大目标尺寸（Vulkan maxFramebufferWidth / Height 与 maxImageDimension2D 同级，caps.max_tex_dim）。
static int noatt_extent(uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (rp_S.has_vp) {
        const float *v = rp_S.vp[0];
        x1 = v[2] >= 0 ? (double)v[0] + v[2] : v[0];
        y1 = v[3] >= 0 ? (double)v[1] + v[3] : v[1];   // 负高度（上下翻）的视口：右下角是起点本身
    } else if (rp_R.has_sc) {
        x1 = (double)rp_R.sc.x + rp_R.sc.w; y1 = (double)rp_R.sc.y + rp_R.sc.h;
    }
    if (!(x1 >= 1) || !(y1 >= 1)) return 0;
    nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
    double lim = cp.max_tex_dim ? (double)cp.max_tex_dim : 16384.0;
    *w = (uint32_t)ceil(x1 < lim ? x1 : lim); *h = (uint32_t)ceil(y1 < lim ? y1 : lim);
    return 1;
}
static int noatt_ok(void) { nvn_gpu_caps cp; nvn_gpu_caps_get(&cp); return (cp.caps & NVN_GPU_CAP_NO_ATTACH_PASS) != 0; }
int rp_pass_slot_attached(int i) { return P.has_rt && i >= 0 && i < P.rt.n && i < 8 && (P.colors >> i & 1) && P.rt.color[i] && slot_kind(i, NULL, NULL) >= 0; }
int rp_pass_slot_planned(int i) {
    return P.has_rt && i >= 0 && i < P.rt.n && i < 8 && (P.want_colors >> i & 1) &&
           P.rt.color[i] && slot_kind(i, NULL, NULL) >= 0;
}

// 同一组目标（对齐 nvn_mtl.m rt_same @10955）：颜色 / 深度个数、每张图的序号、视图记录逐字节相同就算同一组。
// 不比对象指针：游戏会按值拷贴图对象，指针不可信（同一个序号才是同一张图）。
// 这一遍可证的像素区域：裁剪矩形（设了才有）按目标尺寸裁。只收裁剪、不看视口：后端把裁剪原样交给 Vulkan，
// 像素一定落在它里面；视口带窗口原点翻转，要按原点翻 y 才能用，等真机验证过再接。返回 0 = 不能收缩（整张）。
static int pass_area(p_scissor *sc) {
    uint32_t tw = 0, th = 0;
    for (int i = 0; i < P.rt.n && i < 8; i++) if (P.rt.color[i]) { level_size(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i], &tw, &th); break; }
    if (!tw && P.rt.depth) level_size(P.rt.depth, &P.rt.dview, P.rt.has_dview, &tw, &th);
    return rp_R.has_sc && rp_clip_scissor(&rp_R.sc, tw, th, sc);
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n || (!a->depth) != (!b->depth) || (a->depth && a->depth->serial != b->depth->serial) || a->has_dview != b->has_dview) return 0;
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return 0;
    for (int k = 0; k < a->n && k < 8; k++) {
        if ((!a->color[k]) != (!b->color[k]) || (a->color[k] && a->color[k]->serial != b->color[k]->serial) || a->has_cview[k] != b->has_cview[k]) return 0;
        if (a->has_cview[k] && memcmp(&a->cview[k], &b->cview[k], sizeof a->cview[k])) return 0;
    }
    return 1;
}
// 当前目标的管线形状：各颜色槽的附件格式、深度 / 模板格式、采样数。取法与 rp_draw.c 取管线描述时逐项一致
// （那边按 rp_pass_slot_attached 判槽、用同一组格式与采样数填 cfmt / dfmt / sfmt / samples）——形状一样，管线对象就可以照用。
static void pipe_shape(uint32_t cf[8], uint32_t *dfmt, uint32_t *sfmt, uint32_t *samples) {
    memset(cf, 0, 8 * sizeof *cf); *dfmt = *sfmt = 0; *samples = 0;
    for (int i = 0; i < P.rt.n && i < 8; i++) {
        const tex_t *t = P.rt.color[i];
        if (!t || !rp_pass_slot_attached(i)) continue;
        cf[i] = (uint32_t)(P.rt.has_cview[i] && P.rt.cview[i].set_format ? P.rt.cview[i].format : t->d.format);
    }
    if (P.rt.depth) {
        nvn_fmt_info fi; uint32_t f = (uint32_t)P.rt.depth->d.format; int ok = nvn_fmt_info_of(f, &fi);
        if (!ok || fi.depth) *dfmt = f;
        if (ok && fi.stencil) *sfmt = f;
    }
    const tex_t *c0 = NULL; for (int i = 0; i < P.rt.n && i < 8 && !c0; i++) c0 = P.rt.color[i];   // 采样数取第一个有图的颜色槽，没有取深度目标（同 rp_draw.c）
    *samples = c0 ? (uint32_t)c0->d.samples : P.rt.depth ? (uint32_t)P.rt.depth->d.samples : 0;
}

// ---------- 待折清除的挂起（开关 debug.s2a.nvk_clear_park，缺省 1；0 = 旧行为）----------
// 整屏清除不编码任何东西，只表示「这个附件下一次被当附件时载入成这个值」。旧做法是换目标 / 提交收尾时为它开一个只做清除的通道
//（真机每帧 159 个通道里 132.7 个是 0–1 笔，换目标结束 135.4 次）。现在把清除挂起，等下一个把它当附件的通道吃掉（load = CLEAR）：
// 一个通道可以同时吃掉好几个目标的清除，纯清除通道就没了。
// 身份 = 贴图序号 + 级 + 层 + 类别（颜色 / 深度模板）：同一张图挂不同视图还是同一份存储，级 / 层取自视图记录。
// 绘制前按宿主底图落地采样和图像依赖（rp_pass_land_reads）；通道外操作通过 rp_pass_flush 兑现剩余项。
enum { PK_COLOR = 0, PK_DEPTH = 1 };   // 类别：颜色槽 / 深度模板
enum { PK_ENT = 16, PK_GRP = 8 };      // 挂起项 / 目标组（一组 = 一次 SET_RT 的附件表；落地时一组开一个只做清除的通道）
static struct {                        // 挂起项
    uint8_t used, kind, slot, grp;     // kind 见 PK_*；slot 颜色槽号；grp 属于哪一组目标
    uint8_t has_view; view_t view;     // 挂起时的视图记录（级 / 层 / 格式原样用）
    uint8_t cd, cs; float dv; uint32_t sv;   // 深度 / 模板两面与各自的清除值
    float rgba[4];                     // 颜色清除值（原始值，消费时按附件格式换算，见 color_value）
} g_pk[PK_ENT];
static struct {                        // 目标组：附件表 + 快照（照 P.kept 自己留一份：记录所在的命令内存在这次提交之后可能被复用）
    uint8_t used; uint64_t seq; p_set_rt rt; tex_t kept[9];
} g_pkg[PK_GRP];
static uint64_t g_pk_seq;
static int park_on(void) { return nvn_plat_flag("nvk_clear_park", 1) != 0; }

// 同段互不重叠的清除可等待同一次开遍。表内按录制顺序保存，原视图由本段保活。
enum { TEX_CLEAR_SLOTS = 8 };
typedef struct {
    tex_t tex;
    nvn_gpu_view view;
    nvn_gpu_tex image;
    nvn_gpu_clear value;
    uint32_t level, layer;
    int is_int;
} parked_tex_clear;
static parked_tex_clear g_tex_clear[TEX_CLEAR_SLOTS];
static nvn_gpu_cmd g_tex_clear_cmd;
int rp_tex_clear_active;   // 表中项数，仅回放线程读写

// 弱链接[replay]：nvn_gpu_cmd_keep_clear_view
#pragma weak nvn_gpu_cmd_keep_clear_view
// 弱链接[replay]：nvn_gpu_clear_kept_view
#pragma weak nvn_gpu_clear_kept_view

static void tex_clear_emit(nvn_gpu_cmd c, const parked_tex_clear *p) {
    const nvn_gpu_clear *cl = &p->value;
    nvn_gpu_clear_kept_view(c, p->view, cl->value.f, cl->value.u, p->is_int);
}

static void tex_clear_land(nvn_gpu_cmd c, unsigned mask) {
    if (!mask || !rp_tex_clear_active || c != g_tex_clear_cmd) return;
    int kept = 0;
    for (int i = 0; i < rp_tex_clear_active; i++) {
        if (mask & (1u << i)) tex_clear_emit(c, &g_tex_clear[i]);
        else {
            if (kept != i) g_tex_clear[kept] = g_tex_clear[i];
            kept++;
        }
    }
    rp_tex_clear_active = kept;
}

void rp_pass_land_tex_clear(nvn_gpu_cmd c) { tex_clear_land(c, ~0u); }

static int tex_clear_same_texture(const tex_t *t, const tex_t *s) {
    return t->serial == s->serial && t->mem == s->mem && t->pool == s->pool &&
           t->off == s->off && t->L.size == s->L.size && !memcmp(&t->d, &s->d, sizeof t->d);
}

// 同一存储的部分重叠、两个池映到重叠地址和未证明的范围都走原传输。
static int tex_clear_overlap(const tex_t *t, const tex_t *s) {
    if (!t) return 0;
    if (t->serial == s->serial || !t->mem || !t->L.size || !t->pool ||
        (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE)) return 1;
    uintptr_t a = (uintptr_t)t->mem, b = (uintptr_t)s->mem;
    return a <= b ? b - a < t->L.size : a - b < s->L.size;
}

void rp_pass_prepare_tex_clear(nvn_gpu_cmd c, const tex_t *t, int can_park) {
    if (!rp_tex_clear_active || c != g_tex_clear_cmd) return;
    if (!can_park) { rp_pass_land_tex_clear(c); return; }
    unsigned conflicts = 0;
    for (int i = 0; i < rp_tex_clear_active; i++)
        if (tex_clear_overlap(t, &g_tex_clear[i].tex)) conflicts |= 1u << i;
    if (!conflicts && rp_tex_clear_active == TEX_CLEAR_SLOTS) conflicts = 1u;   // 最早项先兑现
    tex_clear_land(c, conflicts);
}

int rp_pass_park_tex_clear(nvn_gpu_cmd c, const tex_t *t, const view_t *v, nvn_gpu_view gpu,
                          const nvn_gpu_clear *value, int is_int) {
    if (P.open || P.barrier_bits || rp_tex_clear_active >= TEX_CLEAR_SLOTS ||
        (rp_tex_clear_active && c != g_tex_clear_cmd) ||
        !nvn_gpu_cmd_keep_clear_view || !nvn_gpu_clear_kept_view) return 0;
    nvn_gpu_tex image = nvn_gpu_view_texture(gpu);
    if (!image || !nvn_gpu_cmd_keep_clear_view(c, gpu)) return 0;
    unsigned conflicts = 0;
    for (int i = 0; i < rp_tex_clear_active; i++)
        if (image == g_tex_clear[i].image) conflicts |= 1u << i;
    tex_clear_land(c, conflicts);
    g_tex_clear_cmd = c;
    g_tex_clear[rp_tex_clear_active++] = (parked_tex_clear){
        .tex = *t, .view = gpu, .image = image, .value = *value,
        .level = v->base_level, .layer = v->min_layer, .is_int = is_int,
    };
    return 1;
}

void rp_pass_tex_clear_before_tex(nvn_gpu_cmd c, const tex_t *t) {
    if (c != g_tex_clear_cmd) return;
    unsigned conflicts = 0;
    for (int i = 0; i < rp_tex_clear_active; i++) {
        const tex_t *s = &g_tex_clear[i].tex;
        if (tex_clear_overlap(t, s) && !tex_clear_same_texture(t, s)) conflicts |= 1u << i;
    }
    tex_clear_land(c, conflicts);
}

void rp_pass_tex_clear_before_buf(nvn_gpu_cmd c, uint64_t addr, uint64_t len) {
    if (!len || (c && c != g_tex_clear_cmd)) return;
    unsigned conflicts = 0;
    for (int i = 0; i < rp_tex_clear_active; i++) {
        const tex_t *s = &g_tex_clear[i].tex;
        const uint64_t base = (uintptr_t)s->mem;
        if (addr <= base ? base - addr < len : addr - base < s->L.size) conflicts |= 1u << i;
    }
    tex_clear_land(g_tex_clear_cmd, conflicts);
}

// 槽的子资源身份：贴图序号 + 级 + 层
static void slot_id(const tex_t *t, int has_v, const view_t *v, uint64_t *ser, uint32_t *lv, uint32_t *ly) {
    *ser = t ? t->serial : 0;
    *lv = has_v && v->set_levels ? (uint32_t)v->base_level : 0;
    *ly = has_v && v->set_layers ? (uint32_t)v->min_layer : 0;
}
static void pk_id(int e, uint64_t *ser, uint32_t *lv, uint32_t *ly) {   // 挂起项 e 的子资源身份（从它那一组的附件表取）
    const p_set_rt *rt = &g_pkg[g_pk[e].grp].rt;
    if (g_pk[e].kind == PK_DEPTH) slot_id(rt->depth, rt->has_dview, &rt->dview, ser, lv, ly);
    else slot_id(rt->color[g_pk[e].slot], rt->has_cview[g_pk[e].slot], &rt->cview[g_pk[e].slot], ser, lv, ly);
}
static int park_find(uint64_t ser, uint32_t lv, uint32_t ly, int kind) {   // 同一子资源的挂起项；-1 = 没有
    for (int e = 0; e < PK_ENT; e++) if (g_pk[e].used && g_pk[e].kind == kind) {
        uint64_t s2; uint32_t l2, y2; pk_id(e, &s2, &l2, &y2);
        if (s2 == ser && l2 == lv && y2 == ly) return e;
    }
    return -1;
}
static void park_land_group(nvn_gpu_cmd c, int g);
static void park_land_oldest(nvn_gpu_cmd c) {   // 表满：先把挂得最早的一组落地（等于把旧行为的一次提前落地搬到这里）
    int g = -1;
    for (int k = 0; k < PK_GRP; k++) if (g_pkg[k].used && (g < 0 || g_pkg[k].seq < g_pkg[g].seq)) g = k;
    if (g >= 0) park_land_group(c, g);
}
static int park_group(nvn_gpu_cmd c) {   // 当前目标那一组：按附件表比（rt_same）；没有就新建（组也满了先落地最早的一组）
    for (int g = 0; g < PK_GRP; g++) if (g_pkg[g].used && rt_same(&g_pkg[g].rt, &P.rt)) return g;
    int g = -1;
    for (int k = 0; k < PK_GRP && g < 0; k++) if (!g_pkg[k].used) g = k;
    if (g < 0) { park_land_oldest(c); for (int k = 0; k < PK_GRP && g < 0; k++) if (!g_pkg[k].used) g = k; }
    if (g < 0) return -1;
    g_pkg[g].used = 1; g_pkg[g].seq = ++g_pk_seq; g_pkg[g].rt = P.rt;
    for (int i = 0; i < 8; i++) if (g_pkg[g].rt.color[i]) { g_pkg[g].kept[i] = *g_pkg[g].rt.color[i]; g_pkg[g].rt.color[i] = &g_pkg[g].kept[i]; }
    if (g_pkg[g].rt.depth) { g_pkg[g].kept[8] = *g_pkg[g].rt.depth; g_pkg[g].rt.depth = &g_pkg[g].kept[8]; }
    return g;
}
static int park_slot(nvn_gpu_cmd c, int kind, int slot) {   // 取（必要时新建）一条挂起项：同子资源已挂起就改挂到当前这组并覆盖值。-1 = 记不下
    int g = park_group(c);
    if (g < 0) return -1;
    const p_set_rt *rt = &g_pkg[g].rt;
    int has_v = kind == PK_DEPTH ? rt->has_dview : rt->has_cview[slot];
    const view_t *v = kind == PK_DEPTH ? &rt->dview : &rt->cview[slot];
    uint64_t ser; uint32_t lv, ly;
    slot_id(kind == PK_DEPTH ? rt->depth : rt->color[slot], has_v, v, &ser, &lv, &ly);
    int e = park_find(ser, lv, ly, kind);
    if (e >= 0) {   // 同一子资源已经挂着：改挂到当前这组，值由调用方覆盖（深度 / 模板两面在这里合起来，不能整项清掉）
        g_pk[e].grp = (uint8_t)g; g_pk[e].slot = (uint8_t)slot;
        g_pk[e].has_view = (uint8_t)has_v; if (has_v) g_pk[e].view = *v;
        return e;
    }
    for (e = 0; e < PK_ENT && g_pk[e].used; e++) {}
    if (e == PK_ENT) { park_land_oldest(c); for (e = 0; e < PK_ENT && g_pk[e].used; e++) {} }
    if (e == PK_ENT) return -1;
    memset(&g_pk[e], 0, sizeof g_pk[e]);
    g_pk[e].used = 1; g_pk[e].kind = (uint8_t)kind; g_pk[e].slot = (uint8_t)slot; g_pk[e].grp = (uint8_t)g;
    g_pk[e].has_view = (uint8_t)has_v; if (has_v) g_pk[e].view = *v;
    return e;
}
static void park_put_color(nvn_gpu_cmd c, int i, const float rgba[4]) {
    int e = park_slot(c, PK_COLOR, i);
    if (e < 0) return;
    memcpy(g_pk[e].rgba, rgba, sizeof g_pk[e].rgba);
    rp_perf_add(RP_PERF_P_PARK, 1);
}
// 深度 / 模板：同一张图的两面合成一项（游戏常常先清深度、再清模板两条命令）
static void park_put_depth(nvn_gpu_cmd c, int do_d, float dv, int do_s, uint32_t sv) {
    int e = park_slot(c, PK_DEPTH, 0);
    if (e < 0) return;
    if (do_d) { g_pk[e].cd = 1; g_pk[e].dv = dv; }
    if (do_s) { g_pk[e].cs = 1; g_pk[e].sv = sv & 0xff; }
    rp_perf_add(RP_PERF_P_PARK, 1);
}
static int park_color(int i) { uint64_t s; uint32_t lv, ly; slot_id(P.rt.color[i], P.rt.has_cview[i], &P.rt.cview[i], &s, &lv, &ly); return park_find(s, lv, ly, PK_COLOR); }
static int park_depth(void) { if (!P.rt.depth) return -1; uint64_t s; uint32_t lv, ly; slot_id(P.rt.depth, P.rt.has_dview, &P.rt.dview, &s, &lv, &ly); return park_find(s, lv, ly, PK_DEPTH); }
static int park_here(void) {   // 当前目标上还有挂起的清除（续接前必须判）
    for (int i = 0; i < P.rt.n && i < 8; i++) if (P.rt.color[i] && park_color(i) >= 0) return 1;
    return park_depth() >= 0;
}
static void park_gc(void) {   // 空了的组放掉（组位有限）
    for (int g = 0; g < PK_GRP; g++) if (g_pkg[g].used) {
        int any = 0; for (int e = 0; e < PK_ENT; e++) if (g_pk[e].used && g_pk[e].grp == g) any = 1;
        if (!any) g_pkg[g].used = 0; }
}
// 落地一组：用这一组自己的附件表开一个只做清除的通道（开通道时把这一组的挂起项吃掉），开完就结束。
// 落地要开通道，所以当前通道先收掉（调用方随后会重开它：rp_draw 的贴图解析循环在开通道之前调，见 rp_pass_land_reads）。
// 管线形状与脏位是给「有绘制的通道」记的：落地通道一笔都不画，前后原样还原，不打扰下一笔。
static void park_land_group(nvn_gpu_cmd c, int g) {
    if (!g_pkg[g].used) return;
    int any = 0; for (int e = 0; e < PK_ENT; e++) if (g_pk[e].used && g_pk[e].grp == g) any = 1;
    if (c && any) {
        rp_pass_end(c);
        p_set_rt save_rt = P.rt; tex_t save_kept[9]; memcpy(save_kept, P.kept, sizeof save_kept);
        int save_has = P.has_rt;
        uint32_t sc[8], sd, ss, sm; memcpy(sc, P.shape_c, sizeof sc); sd = P.shape_d; ss = P.shape_s; sm = P.shape_smp;
        int sok = P.shape_ok; uint8_t pso = rp_pso_dirty, dss = rp_dss_dirty;
        P.rt = g_pkg[g].rt; P.has_rt = 1;
        if (pass_begin_colors(c, 0xff, 0, 1)) rp_pass_end(c);   // 显式清除不受绘制写掩码影响
        P.rt = save_rt; memcpy(P.kept, save_kept, sizeof save_kept); P.has_rt = save_has;
        memcpy(P.shape_c, sc, sizeof sc); P.shape_d = sd; P.shape_s = ss; P.shape_smp = sm; P.shape_ok = sok;
        rp_pso_dirty = pso; rp_dss_dirty = dss;
    }
    for (int e = 0; e < PK_ENT; e++) if (g_pk[e].used && g_pk[e].grp == g) g_pk[e].used = 0;
    g_pkg[g].used = 0;
}
static void park_land_selected(nvn_gpu_cmd c, unsigned groups) {
    for (;;) {
        int g = -1;
        for (int k = 0; k < PK_GRP; k++)
            if ((groups & (1u << k)) && g_pkg[k].used && (g < 0 || g_pkg[k].seq < g_pkg[g].seq)) g = k;
        if (g < 0) return;
        park_land_group(c, g);
    }
}
// 提交收尾 / 开关关着时换目标：所有还挂着的组按先后落地。
static void park_land_all(nvn_gpu_cmd c) { park_land_selected(c, (1u << PK_GRP) - 1); }
static void park_drop_all(void) { memset(g_pk, 0, sizeof g_pk); memset(g_pkg, 0, sizeof g_pkg); g_pk_seq = 0; }
static const tex_t *park_texture(int e) {
    const p_set_rt *rt = &g_pkg[g_pk[e].grp].rt;
    return g_pk[e].kind == PK_DEPTH ? rt->depth : rt->color[g_pk[e].slot];
}

static void park_land_alias_targets(nvn_gpu_cmd c, const p_set_rt *next) {
    if (!c) return;
    unsigned groups = 0;
    for (int e = 0; e < PK_ENT; e++) {
        if (!g_pk[e].used) continue;
        const unsigned group = 1u << g_pk[e].grp;
        if (groups & group) continue;
        const tex_t *target = park_texture(e);
        for (int i = 0; i < next->n && i < 8; i++) {
            if (rp_tex_storage_aliases(target, next->color[i])) {
                groups |= group;
                break;
            }
        }
        if (!(groups & group) && rp_tex_storage_aliases(target, next->depth)) groups |= group;
    }
    if (groups) park_land_selected(c, groups);
}

void rp_pass_land_reads(nvn_gpu_cmd c, const rp_pass_read *reads, int n) {
    if (!c || n <= 0) return;
    if (rp_tex_clear_active) {
        unsigned conflicts = 0;
        for (int i = 0; i < n; i++) {
            // 整池快照可能含按不同描述建立的别名，后端贴图身份不足以排除。
            if (reads[i].pool) { conflicts = ~0u; break; }
            for (int j = 0; j < rp_tex_clear_active; j++)
                if (reads[i].tex == g_tex_clear[j].image) conflicts |= 1u << j;
        }
        tex_clear_land(c, conflicts);
    }
    unsigned groups = 0;
    for (int e = 0; e < PK_ENT; e++) {
        if (!g_pk[e].used) continue;
        const unsigned group = 1u << g_pk[e].grp;
        if (groups & group) continue;
        if (rp_tex_is_read(park_texture(e), reads, n)) groups |= group;
    }
    if (groups) park_land_selected(c, groups);
}

// 仅内部余区重开路径使用可见拷回失败；不把此结果当作后端执行成功。
static int pass_end_checked(nvn_gpu_cmd c) {
    int copied = 1;
    if (P.open) rp_perf_add(RP_PERF_PASSEND_N, 1);
    if (P.open && c) { nvn_gpu_pass_end(c); for (int i = 0; i < 8; i++) if (g_shu[i].base && !shadow_copy(c, i, 1)) copied = 0; }   // 画进影子的内容拷回各自的深度贴图
    if (P.open) { if (P.has_ds) { rp_perf_add(RP_PERF_P_DS, 1); if (!P.ds_used) rp_perf_add(RP_PERF_P_DS_IDLE, 1); } if (P.ndraw <= 1) rp_perf_add(RP_PERF_P_ONE, 1);
                  if (!P.ndraw) rp_perf_add(RP_PERF_P_CLEARONLY, 1);
                  if (P.ndraw <= 1) {   // 0–1 笔通道的构成：挂不挂深度模板、各吃掉多少像素（碎片化到底是阴影/深度遍还是轻量目标）
                      if (P.has_ds) rp_perf_add(RP_PERF_P_TINY_DS, 1); else rp_perf_add(RP_PERF_P_TINY_NODS, 1);
                      rp_perf_add(RP_PERF_P_TINY_KPIX, (uint64_t)P.cur_kpix);
                  } }   // 一笔绘制都没有的通道：只为落地待折清除（或一开就被别的操作截断）
    memset(g_shu, 0, sizeof g_shu);
    P.open = 0; P.area_on = 0; P.area_want = 0; rp_occl_pass_closed();
    return copied;
}
void rp_pass_end(nvn_gpu_cmd c) { (void)pass_end_checked(c); }

// 开通道（已开着直接回 1）。0 = 开不了（已记账）：调用方跳过这笔绘制。
static uint64_t pb_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#define PB(step) do { if (tm_) { uint64_t n_ = pb_now(); rp_perf_add(step, n_ - pb_); pb_ = n_; } } while (0)
static int pass_area_contains(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    return x >= P.ax && y >= P.ay && (int64_t)x + w <= (int64_t)P.ax + P.aw &&
           (int64_t)y + h <= (int64_t)P.ay + P.ah;
}
// 局部遍只续接已知落在区域内的命令。扩大或失去范围证明时整面 LOAD 重开，
// 既保留已经画出的内容，也限制同一遍因连续扩大剪裁而反复重开。
void rp_pass_prepare_draw(nvn_gpu_cmd c, uint32_t color_write_mask) {
    if (P.open && P.sw_split) { rp_perf_add(RP_PERF_P_CUT_BAR, 1); rp_pass_end(c); }   // 通道里的着色器写之后游戏发过屏障：这一笔另开通道（见 rp_pass_barrier）
    uint8_t wanted = 0;
    for (int i = 0; i < 8; i++) if (color_write_mask >> (4 * i) & 0xf) wanted |= (uint8_t)(1u << i);
    wanted = select_colors(wanted);
    // 后端没声明无附件通道时，全零且无深度仍须保留原目标，不能把原本可执行的片元副作用 / 查询变成跳过。
    // 部分槽被筛掉、有深度、或后端明确支持无附件通道时，才真的移除全部零掩码颜色槽。
    if (!wanted && !P.rt.depth && !noatt_ok()) wanted = select_colors(0xff);
    if (P.open && P.colors != wanted) rp_pass_end(c);
    P.want_colors = wanted;
    P.area_want = 0;
    if (P.open && P.na_w && !P.na_from_rt) {   // 真无目标通道按视口定；零掩码留下的逻辑目标尺寸不能随视口扩大
        uint32_t w = 0, h = 0;
        if (noatt_extent(&w, &h) && (w > P.na_w || h > P.na_h)) rp_pass_end(c);
    }
    p_scissor sc;
    const int known = nvn_plat_flag("nvk_pass_area", 0) && !rp_occl_want() && pass_area(&sc);
    if (P.open) {
        if (P.area_on && (!known || !pass_area_contains(sc.x, sc.y, (uint32_t)sc.w, (uint32_t)sc.h))) rp_pass_end(c);
        return;
    }
    if (known) { P.area_want = 1; P.wx = sc.x; P.wy = sc.y; P.ww = (uint32_t)sc.w; P.wh = (uint32_t)sc.h; }
}
// 大附件的整张清除不能折进较小公共区域：先用原视图单独清整张，再让公共通道 LOAD。
// 不经 pass_begin_colors 递归，不换逻辑目标；影子附件沿用本次视图，最终由 rp_pass_end 拷回。
static void pass_land_full(nvn_gpu_cmd c, nvn_gpu_pass_desc *d, int color,
                           uint32_t w, uint32_t h, const tex_t *t) {
    nvn_gpu_pass_desc one; memset(&one, 0, sizeof one);
    one.w = w; one.h = h; one.layers = d->layers;
    if (color >= 0) {
        one.ncolor = (uint32_t)color + 1; one.color[color] = d->color[color];
    } else {
        one.depth = d->depth; one.stencil = d->stencil;
        one.clear_depth = d->clear_depth; one.clear_stencil = d->clear_stencil;
    }
    nvn_gpu_pass_begin(c, &one);
    nvn_gpu_pass_end(c);
    rp_perf_add(RP_PERF_PASSES, 1);
    rp_tex_mark_gpu_written(t);
    if (rp_pass_fu_enabled()) s2a_fu_rt_mark_fresh(t->serial, rp_frame());
    if (color >= 0) d->color[color].load = NVN_GPU_LOAD_LOAD;
    else { d->depth.load = NVN_GPU_LOAD_LOAD; d->stencil.load = NVN_GPU_LOAD_LOAD; }
    rp_dirty_all();
}
// 截去纯清除遍里不写的附件。已解析的描述仍是原路径的尺寸、视图与零初始化，
// 先确认删掉附件不会改变覆盖范围或可观察的别名内容，再改挂载表。
static int trim_clear_attachment(const tex_t *t, const view_t *view, const tex_t *const *seen, int count,
                                 uint32_t caps) {
    if (!t->serial || !t->mem || !t->L.size || t->d.target != NVN_TEXTURE_TARGET_2D ||
        t->d.depth != 1 || t->d.samples > 1 || (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) ||
        (t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) ||
        (view && view->set_format && view->format != (uint32_t)t->d.format) ||
        !(nvn_gpu_format_caps((uint32_t)t->d.format) & caps)) return 0;
    uintptr_t address = (uintptr_t)t->mem;
    for (int i = 0; i < count; i++) {
        const tex_t *p = seen[i];
        uintptr_t other = (uintptr_t)p->mem;
        if (t->serial == p->serial || (address <= other ? other - address < t->L.size : address - other < p->L.size))
            return 0;
    }
    return 1;
}

static int area_attachment_safe(const tex_t *t, const tex_t *const *others, int count) {
    if (!t->serial || !t->mem || !t->L.size || t->d.target != NVN_TEXTURE_TARGET_2D ||
        t->d.depth != 1 || t->d.samples > 1 || (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) ||
        !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return 0;
    const uintptr_t address = (uintptr_t)t->mem;
    for (int i = 0; i < count; i++) {
        const tex_t *p = others[i];
        const uintptr_t other = (uintptr_t)p->mem;
        if (p->serial == t->serial ||
            (address <= other ? other - address < t->L.size : address - other < p->L.size)) return 0;
    }
    return 1;
}

static int trim_park_clear(nvn_gpu_pass_desc *d, const uint32_t cw[8], const uint32_t ch[8], uint32_t dw, uint32_t dh) {
    if (d->layers != 1 || d->occlusion) return 0;
    const tex_t *seen[9];
    int count = 0;
    uint8_t clears = 0, attached = 0;
    for (uint32_t i = 0; i < d->ncolor; i++) {
        if (!d->color[i].view) continue;
        const tex_t *t = P.rt.color[i];
        if (g_shu[i].base || cw[i] != d->w || ch[i] != d->h ||
            !trim_clear_attachment(t, P.rt.has_cview[i] ? &P.rt.cview[i] : NULL, seen, count, NVN_GPU_FMT_RENDER))
            return 0;
        seen[count++] = t;
        attached |= (uint8_t)(1u << i);
        if (d->color[i].load == NVN_GPU_LOAD_CLEAR) clears |= (uint8_t)(1u << i);
    }
    const int has_ds = d->depth.view || d->stencil.view;
    const int clear_ds = (d->depth.view && d->depth.load == NVN_GPU_LOAD_CLEAR) ||
                         (d->stencil.view && d->stencil.load == NVN_GPU_LOAD_CLEAR);
    if (has_ds && (dw != d->w || dh != d->h ||
        !trim_clear_attachment(P.rt.depth, P.rt.has_dview ? &P.rt.dview : NULL, seen, count, NVN_GPU_FMT_DEPTH))) return 0;
    if ((!clears && !clear_ds) || (attached == clears && has_ds == clear_ds)) return 0;
    for (uint32_t i = 0; i < d->ncolor; i++)
        if (!(clears & (1u << i))) d->color[i].view = NULL;
    // 有一面待清即保留整个 DS；另一面按原来的 LOAD/STORE 保存。
    if (!clear_ds) d->depth.view = d->stencil.view = NULL;
    return 1;
}

static int pass_area_safe(const nvn_gpu_pass_desc *d, const uint32_t cw[8], const uint32_t ch[8],
                          uint32_t dw, uint32_t dh) {
    if (d->layers != 1 || d->occlusion) return 0;
    const tex_t *seen[8];
    int count = 0;
    for (uint32_t i = 0; i < d->ncolor; i++) {
        if (!d->color[i].view) continue;
        if (d->color[i].load != NVN_GPU_LOAD_LOAD || d->color[i].store != NVN_GPU_STORE_STORE ||
            g_shu[i].base || cw[i] != d->w || ch[i] != d->h ||
            !area_attachment_safe(P.rt.color[i], seen, count)) return 0;
        seen[count++] = P.rt.color[i];
    }
    if (d->depth.view || d->stencil.view) {
        return dw == d->w && dh == d->h && area_attachment_safe(P.rt.depth, seen, count) &&
            (!d->depth.view || (d->depth.load == NVN_GPU_LOAD_LOAD && d->depth.store == NVN_GPU_STORE_STORE)) &&
            (!d->stencil.view || (d->stencil.load == NVN_GPU_LOAD_LOAD && d->stencil.store == NVN_GPU_STORE_STORE));
    }
    return count != 0;
}

static int tex_clear_slot(const parked_tex_clear *p, const nvn_gpu_pass_desc *d, int layered) {
    if (layered || d->layers != 1 || d->occlusion ||
        d->w != p->value.w || d->h != p->value.h ||
        tex_clear_overlap(P.rt.depth, &p->tex)) return -1;
    int found = -1;
    for (uint32_t i = 0; i < d->ncolor; i++) {
        if (!d->color[i].view) continue;
        const tex_t *t = P.rt.color[i];
        const nvn_gpu_tex image = nvn_gpu_view_texture(d->color[i].view);
        if (!tex_clear_overlap(t, &p->tex) && image != p->image) continue;
        const int hv = P.rt.has_cview[i];
        const view_t *v = &P.rt.cview[i];
        const uint32_t level = hv && v->set_levels ? v->base_level : 0;
        const uint32_t layer = hv && v->set_layers ? v->min_layer : 0;
        if (found >= 0 || !tex_clear_same_texture(t, &p->tex) || d->color[i].load != NVN_GPU_LOAD_LOAD ||
            g_shu[i].base || image != p->image ||
            level != p->level || layer != p->layer ||
            (hv && (v->set_swizzle || (v->set_format && v->format != (uint32_t)t->d.format) ||
                    (v->set_target && v->target != t->d.target)))) return -1;
        found = (int)i;
    }
    return found;
}

static int pass_begin_colors(nvn_gpu_cmd c, uint8_t colors, int layered, int clear_only) {
    const int area_want = P.area_want;
    P.area_want = 0;   // 只给下一次开遍，解析附件失败也不能遗留给独立清除。
    colors = select_colors(colors);
    layered = layered != 0;
    if (P.open && P.colors == colors && P.layered == layered) return 1;
    if (P.open) rp_pass_end(c);
    if (!c || !P.has_rt) { if (c) nvn_unimpl("pass", "draw_without_target", NULL); return 0; }
    P.colors = colors;
    int any_color = 0; for (int i = 0; i < P.rt.n && i < 8; i++) if (P.rt.color[i]) any_color = 1;
    if (P.rt.n < 0 || P.rt.n > 8) { nvn_unimpl("pass", "no_attachment", NULL); return 0; }
    if (!any_color && !P.rt.depth && !noatt_ok()) { nvn_unimpl("pass", "no_attachment", NULL); return 0; }   // 后端开不了无附件通道（iOS 现在）：这笔跳过
    rp_view_pin_begin();   // 下面取到的视图在本函数结束前不能被轮转放掉
    const int tm_ = rp_tm();   // 分时抽样（#1103）：只在采样帧里计时
    uint64_t pb_ = tm_ ? pb_now() : 0;   // 四段计时：取附件视图 / 屏障 / 后端开通道 / 记 GPU 写过
    nvn_gpu_pass_desc d; memset(&d, 0, sizeof d);
    uint32_t cw[8] = {0}, ch[8] = {0}, dw = 0, dh = 0;
    int nattached = 0;
    int taken[9], ntake = 0;   // 这个通道吃掉的挂起项（开通道成功后才移出表：开不了要留着下次落地）
    // 分层通道的层数：各附件从挂接层起能给的层数取最小（iOS nvn_mtl.m 同取法）；影子附件（块压缩 / 深度当颜色）只有一层。
    //   只有一层可给时就是普通通道：写到更高层的图元按 Vulkan 规则丢弃，与 NVN 目标只有一层时一致。
    uint32_t lay_n = 1;
    if (layered) {
        uint32_t m = UINT32_MAX;
        for (int i = 0; i < P.rt.n && i < 8; i++) {
            const tex_t *t = P.rt.color[i];
            if (!t || !(colors >> i & 1)) continue;
            uint32_t sb = 0; const uint32_t a = slot_kind(i, &sb, NULL) ? 1 : att_layers_avail(t, P.rt.has_cview[i], &P.rt.cview[i]);
            if (a < m) m = a;
        }
        if (P.rt.depth) { const uint32_t a = att_layers_avail(P.rt.depth, P.rt.has_dview, &P.rt.dview); if (a < m) m = a; }
        if (m != UINT32_MAX && m > 1) lay_n = m;
    }
    for (int i = 0; i < P.rt.n; i++) {
        const tex_t *t = P.rt.color[i];
        if (!t) continue;   // 颜色槽里的空位：描述里留空（view = NULL），后端按「不用的槽」处理
        if (!(P.colors >> i & 1)) continue;   // 零写掩码 / 去重后的空槽，不能进入帧缓冲或被记成 GPU 写过
        uint32_t sbytes = 0; const char *why = NULL; int kind = slot_kind(i, &sbytes, &why);
        if (kind < 0) { nvn_unimpl("pass", why, t); continue; }   // 深度 / 块压缩格式的图挂在颜色槽、又走不了影子：当空槽（画到这个槽的内容丢掉）
        if (kind && rp_tex_clear_active) rp_pass_land_tex_clear(c);
        int pk = park_color(i);   // 这个槽挂着待折清除：载入时清成挂起值
        view_t av;
        uint64_t sh0_ = kind ? pb_now() : 0;
        nvn_gpu_view v = kind ? shadow_attach(c, i, t, sbytes, pk >= 0) : rp_view_for_attach(t, att_view_n(t, P.rt.has_cview[i], &P.rt.cview[i], &av, lay_n), c);
        if (kind) { rp_perf_add(RP_PERF_PB_SHADOW, pb_now() - sh0_); rp_perf_add(RP_PERF_P_SHADOW, 1); }   // 影子附件（深度/块压缩图经颜色视图当目标）：拷贝通道是我们加的，单独计数
        if (!v) { memset(g_shu, 0, sizeof g_shu); rp_view_pin_end(); return 0; }
        nattached++;
        d.color[i].view = v; d.color[i].store = NVN_GPU_STORE_STORE;
        d.color[i].load = pk >= 0 ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD;
        if (pk >= 0) { color_value(i, g_pk[pk].rgba, &d.color[i].clear); taken[ntake++] = pk; }
        level_size(t, &P.rt.cview[i], P.rt.has_cview[i], &cw[i], &ch[i]);
        extent_min(&d.w, &d.h, cw[i], ch[i]);   // 只统计掩码 / 去重 / 视图解析后真正挂入的附件
    }
    d.ncolor = (uint32_t)P.rt.n; d.layers = lay_n;
    P.na_w = P.na_h = 0; P.na_from_rt = 0;
    if (!nattached && !P.rt.depth) {   // 一个附件都没挂（没设目标，或颜色槽全成了空槽）：后端能开无附件通道就按视口尺寸开，片元副作用 / 遮挡计数照常
        if (!noatt_ok()) { rp_view_pin_end(); nvn_unimpl("pass", "no_attachment", NULL); return 0; }   // 开不了（iOS 现在）：这个通道的绘制跳过
        if (any_color && !P.colors) {
            for (int i = 0; i < P.rt.n; i++) if (P.rt.color[i]) { level_size(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i], &d.w, &d.h); break; }
            P.na_from_rt = 1;
        } else if (!noatt_extent(&d.w, &d.h)) { rp_view_pin_end(); nvn_unimpl("pass", "no_attachment_no_extent", NULL); return 0; }
        P.na_w = d.w; P.na_h = d.h;
    }
    int pdpk = park_depth();   // 深度模板挂着的待折清除（一项可以带两面）
    int pcd = pdpk >= 0 && g_pk[pdpk].cd, pcs = pdpk >= 0 && g_pk[pdpk].cs;
    if (P.rt.depth) {
        nvn_fmt_info fi = {0}; int ok = nvn_fmt_info_of((uint32_t)P.rt.depth->d.format, &fi);
        view_t av;
        nvn_gpu_view v = rp_view_for(P.rt.depth, att_view_n(P.rt.depth, P.rt.has_dview, &P.rt.dview, &av, lay_n), c);
        if (!v) { memset(g_shu, 0, sizeof g_shu); rp_view_pin_end(); return 0; }
        if (!ok || fi.depth) { d.depth.view = v; d.depth.store = NVN_GPU_STORE_STORE; d.depth.load = pcd ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD; if (pcd) d.clear_depth = g_pk[pdpk].dv; }
        if (ok && fi.stencil) { d.stencil.view = v; d.stencil.store = NVN_GPU_STORE_STORE; d.stencil.load = pcs ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD; if (pcs) d.clear_stencil = (uint32_t)g_pk[pdpk].sv & 0xff; }
        if (d.depth.view || d.stencil.view) {
            level_size(P.rt.depth, &P.rt.dview, P.rt.has_dview, &dw, &dh);
            extent_min(&d.w, &d.h, dw, dh);
        }
    }
    PB(RP_PERF_PB_VIEW);
    if (P.barrier_bits) { nvn_gpu_barrier(c, P.barrier_bits); P.barrier_bits = 0; }
    PB(RP_PERF_PB_BAR);
    d.occlusion = (uint8_t)rp_occl_want();
    rp_perf_add(RP_PERF_P_KPIX, (uint64_t)d.w * d.h / 1000);
    P.cur_kpix = (uint32_t)((uint64_t)d.w * d.h / 1000);
    uint32_t color_loads = 0;
    for (int i = 0; i < P.rt.n && i < 8; i++)
        color_loads += d.color[i].view && d.color[i].load == NVN_GPU_LOAD_LOAD;
    for (int i = 0; i < P.rt.n && i < 8; i++)   // 刚建、该是全零的目标：不在 CPU 上搬一整张零，这个通道载入时清成零（已有整屏清除的照它的值清）
        if (d.color[i].view && rp_tex_take_zero(P.rt.color[i]) && d.color[i].load != NVN_GPU_LOAD_CLEAR) { d.color[i].load = NVN_GPU_LOAD_CLEAR; memset(&d.color[i].clear, 0, sizeof d.color[i].clear); }
    const int diag_load = nvn_plat_flag("nvk_diag_load_dontcare", 0);
    const int diag_store = nvn_plat_flag("nvk_diag_store_dontcare", 0);
    // 先按原有载存选区域。局部遍外的像素也要被整图清掉，所以局部遍前用传输兑现整图清除。
    if (!clear_only && area_want && !diag_load && !diag_store &&
        (uint32_t)P.wx < d.w && (uint32_t)P.wy < d.h && pass_area_safe(&d, cw, ch, dw, dh)) {
        const uint32_t w = P.ww < d.w - (uint32_t)P.wx ? P.ww : d.w - (uint32_t)P.wx;
        const uint32_t h = P.wh < d.h - (uint32_t)P.wy ? P.wh : d.h - (uint32_t)P.wy;
        if (P.wx || P.wy || w != d.w || h != d.h) {
            d.area_x = P.wx; d.area_y = P.wy; d.area_w = w; d.area_h = h;
        }
    }
    unsigned folded_tex = 0;
    if (rp_tex_clear_active) {
        // 纯清除遍可能裁掉附件。在开遍前兑现所有无法折叠的清除，并清空表。
        const int fold = !clear_only && !diag_load && !diag_store && !d.area_w;
        for (int i = 0; i < rp_tex_clear_active; i++) {
            const parked_tex_clear *p = &g_tex_clear[i];
            const int slot = fold ? tex_clear_slot(p, &d, layered) : -1;
            if (slot < 0) { tex_clear_emit(c, p); continue; }
            d.color[slot].load = NVN_GPU_LOAD_CLEAR;
            memcpy(&d.color[slot].clear, &p->value.value, sizeof d.color[slot].clear);
            folded_tex |= 1u << slot;
            color_loads--;
        }
        rp_tex_clear_active = 0;
    }
    const int trimmed = clear_only && !diag_load && !diag_store &&
        nvn_plat_flag("nvk_clear_only_attachments", 0) && trim_park_clear(&d, cw, ch, dw, dh);
    P.has_ds = d.depth.view || d.stencil.view; P.ds_used = pcd || pcs; P.ndraw = 0; P.sw_pend = P.sw_split = 0;
    if (!trimmed && color_loads) rp_perf_add(RP_PERF_P_LOADC, color_loads);
    for (int i = 0; i < P.rt.n && i < 8; i++)
        if (d.color[i].view && d.color[i].load == NVN_GPU_LOAD_CLEAR && (cw[i] > d.w || ch[i] > d.h))
            pass_land_full(c, &d, i, cw[i], ch[i], P.rt.color[i]);
    if (((d.depth.view && d.depth.load == NVN_GPU_LOAD_CLEAR) || (d.stencil.view && d.stencil.load == NVN_GPU_LOAD_CLEAR)) && (dw > d.w || dh > d.h))
        pass_land_full(c, &d, -1, dw, dh, P.rt.depth);
    // 诊断开关（只在测量构建里开，不用于出货）：把载入 / 保存全换成 DONTCARE，量「附件载入保存」在 GPU 上到底值多少毫秒。
    // 开了画面会错（前一遍的内容不再载入、这一遍的内容不再保存），只读读数用。
    if (diag_load) {
        for (uint32_t i = 0; i < d.ncolor && i < 8; i++) if (d.color[i].view) d.color[i].load = NVN_GPU_LOAD_DONTCARE;
        if (d.depth.view) d.depth.load = NVN_GPU_LOAD_DONTCARE;
        if (d.stencil.view) d.stencil.load = NVN_GPU_LOAD_DONTCARE;
    }
    if (diag_store) {
        for (uint32_t i = 0; i < d.ncolor && i < 8; i++) if (d.color[i].view) d.color[i].store = NVN_GPU_STORE_DONTCARE;
        if (d.depth.view) d.depth.store = NVN_GPU_STORE_DONTCARE;
        if (d.stencil.view) d.stencil.store = NVN_GPU_STORE_DONTCARE;
    }
    P.area_on = d.area_w != 0;
    if (P.area_on) {
        P.ax = d.area_x; P.ay = d.area_y;
        P.aw = d.area_w; P.ah = d.area_h;
        rp_perf_add(RP_PERF_P_AREA, 1);
        rp_perf_add(RP_PERF_P_AREA_KPIX, (uint64_t)d.area_w * d.area_h / 1000);
    }
    P.pass_w = d.w; P.pass_h = d.h;
    nvn_gpu_pass_begin(c, &d);
    // 大附件已单独落地；剩下的 CLEAR 尺寸等于公共范围，开通道后才可标 fresh。
    if (rp_pass_fu_enabled()) {
        uint64_t ff = rp_frame();
        if (!P.area_on) {
            for (uint32_t i = 0; i < d.ncolor && i < 8; i++)
                if (!(folded_tex & (1u << i)) && d.color[i].view && d.color[i].load == NVN_GPU_LOAD_CLEAR && P.rt.color[i])
                    s2a_fu_rt_mark_fresh(P.rt.color[i]->serial, ff);
            if (d.depth.view && d.depth.load == NVN_GPU_LOAD_CLEAR && P.rt.depth) s2a_fu_rt_mark_fresh(P.rt.depth->serial, ff);
            if (d.stencil.view && d.stencil.load == NVN_GPU_LOAD_CLEAR && P.rt.depth) s2a_fu_rt_mark_fresh(P.rt.depth->serial, ff);
        }
    }
    PB(RP_PERF_PB_BE);
    rp_view_pin_end();
    rp_occl_pass_opened(d.occlusion);
    rp_perf_add(RP_PERF_PASSES, 1);
    for (int i = 0; i < P.rt.n; i++) if (d.color[i].view) rp_tex_mark_gpu_written(P.rt.color[i]);
    if (P.rt.depth && (!trimmed || P.has_ds)) rp_tex_mark_gpu_written(P.rt.depth);
    PB(RP_PERF_PB_MARK);
    for (int k = 0; k < ntake; k++) g_pk[taken[k]].used = 0;   // 开通道成功了：吃掉的挂起项移出表
    if (pdpk >= 0) g_pk[pdpk].used = 0;
    park_gc();
    P.open = 1; P.layered = (uint8_t)layered;
    // 通道边界不等于状态边界（开关 debug.s2a.nvk_state_keep，缺省 1 = 新行为，0 = 旧行为全部重发）。
    // Vulkan 的管线、描述符集、动态状态在 vkCmdBeginRenderPass 前后都保持，不必因为换了通道就重发；
    // 真要让后端状态失效的路径各自已经 rp_dirty_all()，这里不动它们：矩形清（本文件两处）、计算派发（rp_draw.c）、
    // 清贴图（rp_clear_tex.c）、新段与中途交段（rp_exec.c）、会话复位（rp_state.c）。
    // 通道本身只剩两件会让后端手里的东西作废：
    //   · 管线对象绑着渲染通道的附件格式：形状（颜色 / 深度 / 模板格式、采样数）变了必须重取管线；
    //   · 深度模板状态进了管线描述，绘制那边不消费 rp_dss_dirty，这里补一次重取。
    { uint32_t cf[8], df, sf, smp; pipe_shape(cf, &df, &sf, &smp);
      if (!P.shape_ok || P.shape_d != df || P.shape_s != sf || P.shape_smp != smp || memcmp(P.shape_c, cf, sizeof cf)) {
          memcpy(P.shape_c, cf, sizeof cf); P.shape_d = df; P.shape_s = sf; P.shape_smp = smp; P.shape_ok = 1; rp_pso_dirty = 1; } }
    if (rp_dss_dirty) rp_pso_dirty = 1;
    if (!nvn_plat_flag("nvk_state_keep", 1)) rp_dirty_all();   // 旧行为：通道边界 = 状态边界，已发给后端的管线 / 动态状态 / 绑定全部重发
    return 1;
}
int rp_pass_begin(nvn_gpu_cmd c) { return pass_begin_colors(c, P.want_colors, P.want_layered, 0); }
// 绘制前调（rp_pass_prepare_draw 之前）：这一笔的程序写不写层号。开着的通道分层与否和它不同就收掉重开
// （同 iOS nvn_mtl.m draw：g_pass.layered != g_rp_layered 时 rp_end）。不写层号的绘制不进分层通道：
// 分层通道里没写层号的图元一律落第 0 层，结果相同，但只为写层号的程序付多层帧缓冲的代价。
void rp_pass_want_layered(nvn_gpu_cmd c, int layered) {
    layered = layered != 0;
    if (P.open && P.layered != layered) rp_pass_end(c);
    P.want_layered = (uint8_t)layered;
}

// 结束当前通道；还挂着的待折清除各自落地（一组目标开一个只做清除的通道）
void rp_pass_flush(nvn_gpu_cmd c) {
    rp_pass_end(c);              // 先收当前通道：落地要另开通道
    if (rp_tex_clear_active) rp_pass_land_tex_clear(c);
    if (c) park_land_all(c);     // 挂起到这里还没被任何通道吃掉的：提交收尾落地（同旧行为的一次落地，只是可能多组一起）
    else park_drop_all();        // 没有段：清除无处落，丢弃（提交已按失败处理）
}

// 着色器变体请求使用目标槽数与每槽有效颜色格式（见 rp_draw）；在取程序之前判断，不能等开通道再置脏。
static int rt_shader_same(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        uint32_t af = a->color[i] ? (uint32_t)(a->has_cview[i] && a->cview[i].set_format ? a->cview[i].format : a->color[i]->d.format) : 0;
        uint32_t bf = b->color[i] ? (uint32_t)(b->has_cview[i] && b->cview[i].set_format ? b->cview[i].format : b->color[i]->d.format) : 0;
        if (af != bf) return 0;
    }
    return 1;
}
void rp_pass_set_rt(nvn_gpu_cmd c, const p_set_rt *r) {
    if (rp_tex_clear_active) {
        unsigned conflicts = 0;
        for (int i = 0; i < rp_tex_clear_active; i++) {
            const tex_t *s = &g_tex_clear[i].tex;
            int alias = tex_clear_overlap(r->depth, s);
            for (int j = 0; j < r->n && j < 8 && !alias; j++) {
                const tex_t *t = r->color[j];
                alias = tex_clear_overlap(t, s) && !tex_clear_same_texture(t, s);
            }
            if (alias) conflicts |= 1u << i;
        }
        tex_clear_land(c, conflicts);
    }
    if (!P.has_rt || !rt_shader_same(&P.rt, r)) rp_pso_dirty = 1;
    // 同目标续接（对齐 nvn_mtl.m:15060 的 g_enc && g_enc_mode != 1 && !pend && R.has_rt && rt_same → 只换逻辑目标）：
    // 通道开着、没有待折清除、这组目标与当前的一致 → 只更新目标快照，不结束 / 不重开通道。游戏常在一帧里反复重设同一组目标
    //（iOS 注释里那款每帧 180–285 次遮挡查询各重设一次），每次重开通道都要把整张附件重新载入一遍。
    // 开关 debug.s2a.nvk_rt_cont（缺省 1；0 = 旧行为：每次 SET_RT 都是通道边界）。
    // !park_here() 现在是恒成立的（整屏清除会先 rp_pass_end，见 rp_pass_clear_color / rp_pass_clear_ds），留着防以后改坏：
    // 一旦把带待折清除的通道接下去，那次清除就落不到附件上，或者落到下一个通道之后把刚画的内容盖掉。
    if (P.open && P.has_rt && !park_here() && nvn_plat_flag("nvk_rt_cont", 1) && rt_same(&P.rt, r)) {
        P.rt = *r;
        // 快照照旧留在自己这份（记录所在的命令内存在这次提交之后可能被复用，见下面的说明）
        for (int i = 0; i < 8; i++) if (P.rt.color[i]) { P.kept[i] = *P.rt.color[i]; P.rt.color[i] = &P.kept[i]; }
        if (P.rt.depth) { P.kept[8] = *P.rt.depth; P.rt.depth = &P.kept[8]; }
        rp_perf_add(RP_PERF_P_CONT, 1);
        return;
    }
    if (P.open) rp_perf_add(RP_PERF_P_CUT_RT, 1);
    rp_perf_add(RP_PERF_SETRT_N, 1);
    { struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0); uint64_t s0 = (uint64_t)ts0.tv_sec * 1000000000ull + (uint64_t)ts0.tv_nsec;
      // 开关开着（缺省）：只收当前通道，待折清除挂起（等下一个把它当附件的通道吃掉，见 park 段）；关着 = 旧行为：先落地再换目标
      if (park_on()) {
          rp_pass_end(c);
          // 换到共用底图的另一个序号：先用旧附件视图兑现清除。否则 A→B→A 的最后一次清色可能被 B 的旧组盖掉；
          // 直接把旧清色折进 B 的通道，也可能按 B 的其它视图格式换算。
          park_land_alias_targets(c, r);
      } else {
          rp_pass_end(c);
          if (c) park_land_all(c);
          else park_drop_all();
      }
      struct timespec ts1; clock_gettime(CLOCK_MONOTONIC, &ts1); uint64_t s1 = (uint64_t)ts1.tv_sec * 1000000000ull + (uint64_t)ts1.tv_nsec;
      rp_perf_add(RP_PERF_SETRT_END_NS, s1 - s0); }
    P.rt = *r; P.has_rt = 1;
    // 记录里的贴图指针指向记录内的快照，记录所在的命令内存这次提交之后可能被复用：自己留一份（nvn_mtl.m rt_keep_state @4453）
    for (int i = 0; i < 8; i++) if (P.rt.color[i]) { P.kept[i] = *P.rt.color[i]; P.rt.color[i] = &P.kept[i]; }
    if (P.rt.depth) { P.kept[8] = *P.rt.depth; P.rt.depth = &P.kept[8]; }
}

static void clear_piece(nvn_gpu_clear *out, int *n, const nvn_gpu_clear *src,
                        int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    if (x1 <= x0 || y1 <= y0) return;
    out[*n] = *src;
    out[*n].x = (int32_t)x0; out[*n].y = (int32_t)y0;
    out[*n].w = (uint32_t)(x1 - x0); out[*n].h = (uint32_t)(y1 - y0);
    (*n)++;
}
// 显式清除不能因公共通道较小而丢掉请求余区。先清公共区域内交集，再以同一目标视图
// 单独 LOAD / STORE 清最多四块互不重叠的余区；颜色通道 / DS 面和模板掩码逐块原样保留。
static void clear_remainder_failed(const char *why, const nvn_gpu_clear *cl) {
    if (nvn_unimpl("pass", why, cl) == 1) {
        char msg[240];
        snprintf(msg, sizeof msg, "clear remainder: slot=%d rect=%d,%d %u,%u color_mask=%u depth=%d stencil=%d stencil_mask=%u",
                 cl->color, cl->x, cl->y, cl->w, cl->h, cl->color_mask, cl->depth, cl->stencil, cl->stencil_mask);
        nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", msg);
    }
}
static void pass_clear_rect(nvn_gpu_cmd c, const nvn_gpu_clear *cl) {
    if (P.area_on && !pass_area_contains(cl->x, cl->y, cl->w, cl->h)) {
        const uint8_t colors = P.colors;
        const int layered = P.layered;
        rp_pass_end(c);
        if (!pass_begin_colors(c, colors, layered, 0)) {
            clear_remainder_failed("clear_rect_reopen_failed", cl);
            return;
        }
    }
    int64_t x0 = cl->x, y0 = cl->y, x1 = x0 + cl->w, y1 = y0 + cl->h;
    int64_t ax = P.area_on ? P.ax : 0, ay = P.area_on ? P.ay : 0;
    int64_t ax1 = ax + (P.area_on ? P.aw : P.pass_w), ay1 = ay + (P.area_on ? P.ah : P.pass_h);
    int64_t ix0 = x0 > ax ? x0 : ax, iy0 = y0 > ay ? y0 : ay;
    int64_t ix1 = x1 < ax1 ? x1 : ax1, iy1 = y1 < ay1 ? y1 : ay1;
    nvn_gpu_clear inside[1], outside[4]; int ni = 0, no = 0;
    clear_piece(inside, &ni, cl, ix0, iy0, ix1, iy1);
    if (!ni) outside[no++] = *cl;
    else {
        clear_piece(outside, &no, cl, x0, y0, x1, iy0);
        clear_piece(outside, &no, cl, x0, iy1, x1, y1);
        clear_piece(outside, &no, cl, x0, iy0, ix0, iy1);
        clear_piece(outside, &no, cl, ix1, iy0, x1, iy1);
    }
    rp_occl_pause(c);
    if (ni) nvn_gpu_clear_rect(c, &inside[0]);
    if (no) {
        // 先收公共通道（影子内容也先拷回）。旧附件视图可能已从缓存 release，不能将
        // 保存的 desc.view 再录进辅助通道；重新走有界 pin / 视图解析，且此时允许传输。
        if (!pass_end_checked(c)) {
            clear_remainder_failed("clear_remainder_copyback_failed", cl);
            rp_dirty_all();
            return;   // 不从未更新的基底重载影子覆盖刚清过的公共区域
        }
        p_set_rt saved = P.rt;
        uint8_t colors = 0;
        if (cl->color >= 0) {
            int i = cl->color;
            for (int j = 0; j < 8; j++) if (j != i) P.rt.color[j] = NULL;
            P.rt.n = i + 1; P.rt.depth = NULL; P.rt.has_dview = 0;
            colors = (uint8_t)(1u << i);
        } else {
            P.rt.n = 0;
            memset(P.rt.color, 0, sizeof P.rt.color);
        }
        if (pass_begin_colors(c, colors, 0, 0)) {
            rp_occl_pause(c);
            for (int k = 0; k < no; k++) nvn_gpu_clear_rect(c, &outside[k]);
            // 影子再次拷回：这次包含余区，公共区域由辅助通道的 LOAD 保留
            if (!pass_end_checked(c)) clear_remainder_failed("clear_remainder_copyback_failed", cl);
        } else clear_remainder_failed("clear_remainder_pass_begin_failed", cl);
        P.rt = saved;
    }
    rp_dirty_all();
}

void rp_pass_clear_color(nvn_gpu_cmd c, const p_clear_color *cc, const void *rec) {
    if (!P.has_rt || cc->index < 0 || cc->index >= P.rt.n || cc->index >= 8 || !P.rt.color[cc->index]) { nvn_unimpl("pass", "clear_color_no_target", rec); return; }
    if (!(cc->mask & 0xf)) return;
    int i = cc->index;
    if ((cc->mask & 0xf) != 0xf || !scissor_full(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i])) {
        nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color = i; cl.color_mask = (uint8_t)(cc->mask & 0xf); cl.stencil_mask = 0xff;
        if (!scissor_rect(P.rt.color[i], &P.rt.cview[i], P.rt.has_cview[i], &cl)) return;
        // 与 iOS clear_color_tri 一样：目标没挂进当前通道时，为显式清除恢复附件；下一笔绘制再按自己的掩码筛。
        if (!c || (!(P.open && rp_pass_slot_attached(i)) && !pass_begin_colors(c, (uint8_t)(1u << i), 0, 0))) { if (c) nvn_unimpl("pass", "clear_rect_pass_begin_failed", rec); return; }
        color_value(i, cc->rgba, &cl.value);
        pass_clear_rect(c, &cl);
        return;
    }
    if (P.open) rp_perf_add(RP_PERF_P_CUT_CLEAR, 1);
    if (P.open) rp_pass_end(c);   // 通道中的整屏清除：结束这一通道，清除挂到下一个通道开头（其余附件照常 LOAD）
    park_put_color(c, i, cc->rgba);   // 挂起：下一个把这个槽当附件的通道载入时清成这个值
}

void rp_pass_clear_ds(nvn_gpu_cmd c, const p_clear_ds *cd, const void *rec) {
    if (!P.has_rt || !P.rt.depth) { nvn_unimpl("pass", "clear_ds_no_target", rec); return; }
    int sm = cd->stencil_mask & 0xff;
    if (!cd->depth_mask && !sm) return;
    // 目标实际有哪些面：没有的面这次清除对它无事可做（同 rp_pass_begin 的附件判定）
    nvn_fmt_info fi = {0}; int ok = nvn_fmt_info_of((uint32_t)P.rt.depth->d.format, &fi);
    int do_d = cd->depth_mask && (!ok || fi.depth), do_s = sm && ok && fi.stencil;
    if (!do_d && !do_s) return;
    if (!scissor_full(P.rt.depth, &P.rt.dview, P.rt.has_dview) || (do_s && sm != 0xff)) {
        nvn_gpu_clear cl; memset(&cl, 0, sizeof cl); cl.color = -1; cl.color_mask = 0xf; cl.stencil_mask = (uint8_t)(do_s ? sm : 0xff);
        if (!scissor_rect(P.rt.depth, &P.rt.dview, P.rt.has_dview, &cl)) return;
        if (!c || (!P.open && !pass_begin_colors(c, 0xff, 0, 0))) { if (c) nvn_unimpl("pass", "clear_rect_pass_begin_failed", rec); return; }
        cl.depth = (uint8_t)do_d; cl.stencil = (uint8_t)do_s; cl.depth_value = depth_value(cd->depth); cl.stencil_value = (uint32_t)cd->stencil & 0xff;
        pass_clear_rect(c, &cl);
        return;
    }
    if (P.open) rp_pass_end(c);
    park_put_depth(c, do_d, depth_value(cd->depth), do_s, cd->stencil);   // 挂起：深度 / 模板两面合成一项
}

// 游戏的屏障：记下位，下一个通道前交给后端（nvn_mtl.m:14646 的缺省路径）。
// 例外：开着的通道里已经有绘制的着色器写了存储缓冲 / 存储图像（rp_pass_note_draw），游戏又发了带顺序位的屏障（ORDER_PRIMITIVES 管顶点 / 几何阶段的写，
// ORDER_FRAGMENTS / ORDER_FRAGMENTS_TILED 管片元阶段的写，ORDER_INDIRECT_DATA 管间接参数）：同一个通道里分块渲染的 GPU（Adreno / Mali / Apple）不保证
// 着色器写先于后面绘制的读——先给整个通道的全部绘制做一遍分块归属 / 低分辨率深度，再逐块重放；后面绘制的分块归属读到的是写之前的旧数据。
// 所以屏障之后的第一笔绘制另开通道（rp_pass_prepare_draw 结束这个通道），屏障位在新通道开头交给后端。
// 对齐：iOS nvn_mtl.m OP_BARRIER（g_pass_swrite + ORDER_FRAGMENTS*，rp_end）；Ryujinx BarrierBatch.Flush（高通私有驱动 SupportsRenderPassBarrier = false → endRenderPass）。
// 只有「屏障之前通道里已经有着色器写」才拆：没有写、或写在屏障之后的，屏障照旧只记位、不拆通道。
void rp_pass_barrier(uint32_t bits) {
    P.barrier_bits |= bits;
    if (P.open && P.sw_pend && (bits & (NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT | NVN_BARRIER_ORDER_INDIRECT_DATA_BIT))) P.sw_split = 1;
}
// 通道外的计算派发开工前取走待发的游戏屏障位（#978）：调用方交给后端，之后的通道开头不再重发
uint32_t rp_pass_take_barrier(void) { uint32_t b = P.barrier_bits; P.barrier_bits = 0; return b; }

// 现在挂着几条待折清除。绘制路径拿它做快路的门：0 时不必为「读之前落地」收集这一笔的贴图（稳态下几乎总是 0）。
int rp_pass_parked(void) {
    int n = rp_tex_clear_active;
    if (park_on()) for (int e = 0; e < PK_ENT; e++) if (g_pk[e].used) n++;
    return n;
}
void rp_pass_reset(void) {
    rp_tex_clear_active = 0;
    memset(&P, 0, sizeof P); memset(g_shu, 0, sizeof g_shu); park_drop_all();
    P.want_colors = 0xff;
    for (int i = 0; i < 8; i++) { if (g_sh[i].v) nvn_gpu_view_release(g_sh[i].v); if (g_sh[i].g) nvn_gpu_tex_release(g_sh[i].g); }
    memset(g_sh, 0, sizeof g_sh);
}
int rp_pass_is_open(void) { return P.open; }
int rp_pass_draws(void) { return P.open ? P.ndraw : 0; }
// 当前通道能不能在两笔之间切开、下一笔按 LOAD 续开（rp_exec.c pass_split）。存储恒为 STORE、续开时没有清除负载才算；
// 影子附件、多重采样目标即使正确也不划算，一并不切。
int rp_pass_split_ok(void) {
    if (!P.open || park_here() || P.shape_smp > 1) return 0;
    for (int i = 0; i < 8; i++) if (g_shu[i].base) return 0;
    if (nvn_plat_flag("nvk_diag_load_dontcare", 0) || nvn_plat_flag("nvk_diag_store_dontcare", 0)) return 0;
    return 1;
}
const p_set_rt *rp_pass_target(void) { return P.has_rt ? &P.rt : NULL; }
