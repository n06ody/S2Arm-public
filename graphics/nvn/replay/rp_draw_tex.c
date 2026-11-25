// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 录制的 DrawTexture（OP_DRAW_TEX，#1452）：在当前目标的颜色 0 上画一个贴图矩形。
// 语义对照 iOS nvn_mtl.m draw_tex 与 Ryujinx HelperShader.DrawTexture：只写 color(0)，混合与写掩码沿用游戏当前状态，
// 深度 / 模板测试关、不剔除，视口 = 目的矩形，裁剪照旧；源按句柄取贴图与采样器，源矩形按游戏建图时的尺寸归一化。
// 游戏可以不先绑管线就调 DrawTexture，所以契约显式带全参数（nvn_gpu_draw_tex_desc），不依赖 set_pipe。
// 后端没置 NVN_GPU_CAP_DRAW_TEX 时照旧记「未接 op OP_DRAW_TEX」跳过；做不了的组合一律具名记账，不画近似结果。
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_state.h"
#include "nvn_unimpl.h"
#include "nvn_fmt_info.h"
#include <math.h>
#include <string.h>

static int fmt_int(uint32_t f) {
    nvn_fmt_info fi;
    return nvn_fmt_info_of(f, &fi) && (fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT);
}

void rp_op_draw_tex(nvn_gpu_cmd c, const p_draw_tex *o, const void *rec) {
    if (!c) return;
    nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
    if (!(cp.caps & NVN_GPU_CAP_DRAW_TEX)) { nvn_unimpl("op", "OP_DRAW_TEX", rec); return; }
    const float x0 = o->dst[0], y0 = o->dst[1], x1 = o->dst[2], y1 = o->dst[3];
    for (int k = 0; k < 4; k++)
        if (!isfinite(o->dst[k]) || !isfinite(o->src[k])) { nvn_unimpl("draw_tex", "rect_not_finite", rec); return; }
    if (x0 == x1 || y0 == y1) return;   // 空的目的矩形什么都不画（iOS 同）
    const unsigned cmask = rp_S.chmask.mask & 0xfu;
    if (!cmask) return;                 // color(0) 一个通道都不写：画了也看不见

    // 源：句柄低 20 位贴图号、高 12 位采样器号（同 iOS draw_tex 与 rp_draw 的组合句柄）
    const int tid = (int)(o->handle & 0xfffff), sid = (int)(o->handle >> 20);
    tex_t st; view_t sv; int has_sv = 0;
    if (!rp_tex_snap_by_id(tid, &st, &sv, &has_sv)) { nvn_unimpl("draw_tex", "texture_unregistered", rec); return; }
    const int starget = has_sv && sv.set_target ? sv.target : st.d.target;
    const uint32_t sfmt = has_sv && sv.set_format ? sv.format : (uint32_t)st.d.format;
    nvn_fmt_info sfi;
    if ((starget != NVN_TEXTURE_TARGET_2D && starget != NVN_TEXTURE_TARGET_2D_ARRAY) || st.d.samples > 1) {
        nvn_unimpl("draw_tex", "source_target", rec); return;   // iOS 同：只收二维（数组取第 0 层）单采样
    }
    if (!nvn_fmt_info_of(sfmt, &sfi) || sfi.depth || sfi.stencil || sfi.kind == NVN_FMT_UINT || sfi.kind == NVN_FMT_SINT) {
        nvn_unimpl("draw_tex", "source_format", rec); return;   // 内置片元按浮点读：整数、深度模板源不收（iOS fmt_int_or_ds 同）
    }

    // 目标：颜色 0 必须有图且不是整数格式
    const p_set_rt *rt = rp_pass_target();
    if (!rt || rt->n < 1 || !rt->color[0]) { nvn_unimpl("draw_tex", "no_color0_target", rec); return; }
    const uint32_t cfmt = rt->has_cview[0] && rt->cview[0].set_format ? rt->cview[0].format : (uint32_t)rt->color[0]->d.format;
    if (fmt_int(cfmt)) { nvn_unimpl("draw_tex", "target_integer", rec); return; }
    for (int i = 0; i < rt->n && i < 8; i++)
        if (rt->color[i] && rt->color[i]->serial == st.serial) { nvn_unimpl("draw_tex", "source_is_target", rec); return; }
    if (rt->depth && rt->depth->serial == st.serial) { nvn_unimpl("draw_tex", "source_is_target", rec); return; }

    // color(0) 的混合：照游戏当前状态。逻辑运算、高级混合、双源因子内置片元做不了，具名跳过；目标格式不能混合时关混合记账（同 rp_draw #1072）
    nvn_gpu_draw_tex_desc d; memset(&d, 0, sizeof d);
    d.color_mask = (uint8_t)cmask;
    d.blend_en = (uint8_t)(rp_S.color.blend_en & 1u);
    d.blend = rp_S.blend[0];
    memcpy(d.blend_color, rp_S.blend_color, sizeof d.blend_color);
    if (rp_S.color.logic_op != NVN_LOGIC_OP_COPY) { nvn_unimpl("draw_tex", "logic_op", rec); return; }
    if (d.blend_en) {
        if (d.blend.adv_mode) { nvn_unimpl("draw_tex", "advanced_blend", rec); return; }
        const uint32_t f4[4] = {d.blend.src_rgb, d.blend.dst_rgb, d.blend.src_a, d.blend.dst_a};
        for (int k = 0; k < 4; k++) if (f4[k] >= 0x10 && f4[k] <= 0x13) { nvn_unimpl("draw_tex", "dual_source_blend", rec); return; }
        if (!(rp_fmt_caps(cfmt) & NVN_GPU_FMT_BLEND)) { nvn_unimpl("draw_tex", "blend_unsupported_fmt", rec); d.blend_en = 0; }
    }
    if (!d.blend_en) memset(&d.blend, 0, sizeof d.blend);
    d.blend.adv_overlap = 0; d.blend.adv_premul = 0; d.blend.adv_normdst = 0;

    // 源矩形按游戏建图的尺寸归一化（宽屏放大过的图也按着色器的寻址）；目的矩形反向 = 源端点交换（Ryujinx / iOS 同）
    const float gw = (float)st.d.width, gh = (float)st.d.height;
    if (!(gw > 0) || !(gh > 0)) { nvn_unimpl("draw_tex", "source_size", rec); return; }
    float u0 = o->src[0] / gw, u1 = o->src[2] / gw, v0 = o->src[1] / gh, v1 = o->src[3] / gh;
    if (x1 < x0) { float q = u0; u0 = u1; u1 = q; }
    if (y1 < y0) { float q = v0; v0 = v1; v1 = q; }
    d.uv[0] = u0; d.uv[1] = v0; d.uv[2] = u1; d.uv[3] = v1;
    d.dst[0] = fminf(x0, x1); d.dst[1] = fminf(y0, y1); d.dst[2] = fabsf(x1 - x0); d.dst[3] = fabsf(y1 - y0);
    d.origin_ll = (uint8_t)(rp_origin_lower_left() != 0);

    rp_view_pin_begin();
    d.src = rp_view_by_id_decl(tid, NVN_TEXTURE_TARGET_2D + 1, 0, 0, c, NULL);   // 数组按声明的二维取第 0 层（iOS 同）
    d.samp = d.src ? rp_samp_for_tex(sid, tid) : NULL;
    if (!d.src || !d.samp) { rp_view_pin_end(); nvn_unimpl("draw_tex", d.src ? "sampler_unresolved" : "texture_view", rec); return; }
    if (rp_pass_parked()) {
        const rp_pass_read read = {.tex = nvn_gpu_view_texture(d.src)};
        rp_pass_land_reads(c, &read, 1);
    }

    rp_pass_want_layered(c, 0);   // 我们自己的拷贝程序不写层号：不进分层通道
    rp_pass_prepare_draw(c, cmask);   // 只写颜色 0：通道按这一槽开（同 rp_draw 的有效写掩码口径）
    if (!rp_pass_begin(c)) { rp_view_pin_end(); nvn_unimpl("draw_tex", "pass_begin_failed", rec); return; }
    if (!rp_pass_slot_attached(0)) { rp_view_pin_end(); nvn_unimpl("draw_tex", "no_color0_target", rec); return; }
    // 裁剪夹到颜色 0 的范围（同 fill_dyn）：交集为空就什么都画不出，不发
    uint32_t tw = 1, th = 1; rp_pass_slot_size(0, &tw, &th);
    if (rp_R.has_sc) {
        p_scissor sc;
        if (!rp_clip_scissor(&rp_R.sc, tw, th, &sc)) { rp_view_pin_end(); return; }
        d.has_sc = 1; d.sc[0] = sc.x; d.sc[1] = sc.y; d.sc[2] = sc.w; d.sc[3] = sc.h;
    }
    rp_occl_draw(c);
    nvn_gpu_draw_tex(c, &d);
    rp_pass_note_draw(0);
    rp_dirty_all();   // 后端画内置管线换了管线、动态状态与绑定：下一笔全部重发（同通道内的矩形清除）
    rp_view_pin_end();
}
