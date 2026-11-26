// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 录制的贴图清除：整级整层走后端清图；区域或写掩码走临时附件通道。
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_state.h"
#include "nvn_unimpl.h"
#include "nvn_fmt_info.h"
#include "nvn_plat.h"
#include <math.h>
#include <string.h>

// 弱链接[replay]：rp_pass_park_tex_clear
#pragma weak rp_pass_park_tex_clear
// 弱链接[replay]：rp_pass_prepare_tex_clear
#pragma weak rp_pass_prepare_tex_clear
// 弱链接[replay]：rp_tex_fold_count
#pragma weak rp_tex_fold_count

static void clear_value(const p_clear_tex *o, int kind, nvn_gpu_clear *cl) {
    for (int k = 0; k < 4; k++) {
        double v = o->value_type == CLEAR_TEX_UINT ? (double)o->c.u[k] :
                   o->value_type == CLEAR_TEX_INT ? (double)o->c.i[k] : (double)o->c.f[k];
        if (kind == NVN_FMT_UINT) cl->value.u[k] = isnan(v) || v <= 0 ? 0u : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
        else if (kind == NVN_FMT_SINT) cl->value.i[k] = isnan(v) ? 0 : v <= -2147483648.0 ? INT32_MIN : v >= 2147483647.0 ? INT32_MAX : (int32_t)v;
        else cl->value.f[k] = (float)v;
    }
}

static int fold_clear_value(uint32_t fmt, const nvn_fmt_info *fi, const nvn_gpu_clear *cl) {
    if (fi->kind != NVN_FMT_UINT && fi->kind != NVN_FMT_SINT) {
        for (unsigned i = 0; i < fi->channels; i++) if (!isfinite(cl->value.f[i])) return 0;
        return 1;
    }
    unsigned bits = 8u * fi->bytes / fi->channels;
    if (fmt != NVN_FORMAT_RGB10A2UI && fmt != NVN_FORMAT_RGB10A2I &&
        (fi->bytes % fi->channels || (bits != 8 && bits != 16 && bits != 32))) return 0;
    for (unsigned i = 0; i < fi->channels; i++) {
        if (fmt == NVN_FORMAT_RGB10A2UI || fmt == NVN_FORMAT_RGB10A2I) bits = i == 3 ? 2 : 10;
        if (fi->kind == NVN_FMT_UINT) {
            if (cl->value.u[i] >= (UINT64_C(1) << bits)) return 0;
        } else {
            const int64_t limit = INT64_C(1) << (bits - 1);
            if (cl->value.i[i] < -limit || cl->value.i[i] >= limit) return 0;
        }
    }
    return 1;
}

void rp_op_clear_tex(nvn_gpu_cmd c, const p_clear_tex *o, const void *rec) {
    if (!(o->mask & 15) || !o->r.width || !o->r.height) return;
    const tex_t *t = o->tex;
    if (!t || !c) { if (c) nvn_unimpl("tex", "clear_no_texture", rec); return; }
    if (t->d.target != NVN_TEXTURE_TARGET_2D && t->d.target != NVN_TEXTURE_TARGET_2D_ARRAY) {
        nvn_unimpl("tex", "clear_target", rec); return;
    }
    if (t->d.samples > 1 || o->value_type > CLEAR_TEX_INT) { nvn_unimpl("tex", "clear_samples_or_value", rec); return; }
    uint32_t fmt = (uint32_t)t->d.format;
    if (o->has_view && o->view.set_format && o->view.format != fmt) { nvn_unimpl("tex", "clear_view_format", rec); return; }
    nvn_fmt_info fi;
    if (!nvn_fmt_info_of(fmt, &fi) || fi.depth || fi.stencil || fi.expand || fi.bw != 1 || fi.bh != 1) {
        nvn_unimpl("tex", "clear_format", rec); return;
    }
    uint32_t lv = o->has_view && o->view.set_levels ? o->view.base_level : 0;
    uint32_t levels = t->d.levels ? t->d.levels : 1;
    if (lv >= levels || lv >= 32 || !t->d.width || !t->d.height) { nvn_unimpl("tex", "clear_level", rec); return; }
    uint32_t w = t->d.width >> lv, h = t->d.height >> lv;
    if (!w) w = 1;
    if (!h) h = 1;
    uint32_t l0 = o->has_view && o->view.set_layers ? o->view.min_layer : 0;
    uint32_t layers = t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? t->d.depth : 1;
    int32_t nz = o->r.depth ? o->r.depth : 1;
    if (o->r.xoffset < 0 || o->r.yoffset < 0 || o->r.zoffset < 0 || o->r.width < 0 || o->r.height < 0 || nz < 0 ||
        (uint64_t)o->r.xoffset + o->r.width > w || (uint64_t)o->r.yoffset + o->r.height > h ||
        (uint64_t)l0 + o->r.zoffset + nz > layers || (uint64_t)l0 + o->r.zoffset + nz > 65536u) {
        nvn_unimpl("tex", "clear_bounds", rec); return;
    }
    unsigned active = (1u << fi.channels) - 1u, mask = (unsigned)o->mask & active;
    if (!mask) return;
    int whole = !o->r.xoffset && !o->r.yoffset && (uint32_t)o->r.width == w && (uint32_t)o->r.height == h && mask == active;
    if (!whole && !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_RENDER)) { nvn_unimpl("tex", "clear_not_renderable", rec); return; }
    nvn_gpu_clear cl = {0}; cl.color = 0; cl.color_mask = (uint8_t)mask; cl.stencil_mask = 0xff;
    cl.x = o->r.xoffset; cl.y = o->r.yoffset; cl.w = (uint32_t)o->r.width; cl.h = (uint32_t)o->r.height;
    clear_value(o, fi.kind, &cl);
    int is_int = fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT;
    enum rp_tex_fold_result result = RP_TF_N;
    if (!whole || nz != 1) result = RP_TF_SHAPE;
    else if (!rp_pass_park_tex_clear || !nvn_plat_flag("nvk_clear_tex_fold", 0)) result = RP_TF_DISABLED;
    else if (!t->serial || !t->mem || !t->L.size || !t->pool ||
             (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) ||
             t->off < 0 || (uint64_t)t->off > t->pool->size || t->L.size > t->pool->size - (uint64_t)t->off)
        result = RP_TF_STORAGE;
    else if (o->has_view && (o->view.set_swizzle || (o->view.set_target && o->view.target != t->d.target)))
        result = RP_TF_VIEW_POLICY;
    else if (!(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_CLEAR_FOLD) || !fold_clear_value(fmt, &fi, &cl))
        result = RP_TF_FORMAT_VALUE;
    const int can_park = result == RP_TF_N;
    const int diag = rp_pass_prepare_tex_clear && rp_pass_prepare_tex_clear(c, t, can_park);
    if (diag && !can_park) rp_tex_fold_count(result);
    // 后端附件只支持一级一层：每一层独立建视图和通道，避免把数组层数交给单层附件。
    for (int32_t z = 0; z < nz; z++) {
        view_t v = {0}; v.set_levels = 1; v.base_level = (uint8_t)lv; v.nlevels = 1;
        v.set_layers = 1; v.min_layer = (uint16_t)(l0 + (uint32_t)o->r.zoffset + (uint32_t)z); v.nlayers = 1;
        rp_view_pin_begin(); nvn_gpu_view gpu = rp_view_for(t, &v, c);
        if (!gpu) {
            if (diag && can_park) rp_tex_fold_count(RP_TF_VIEW_FAILED);
            rp_view_pin_end();
            return;
        }
        if (can_park) {
            result = rp_pass_park_tex_clear(c, t, &v, gpu, &cl, is_int);
            if (result == RP_TF_N) {
                // 与即时清除相同的资源所有权时刻；命令段已保活原视图。
                rp_view_pin_end(); rp_tex_mark_gpu_written(t);
                return;
            }
            if (diag) rp_tex_fold_count(result);
            if (rp_pass_prepare_tex_clear) rp_pass_prepare_tex_clear(c, NULL, 0);
        }
        if (whole) nvn_gpu_clear_view(c, gpu, cl.value.f, cl.value.u, is_int);
        else {
            nvn_gpu_pass_desc d = {0}; d.ncolor = 1; d.w = w; d.h = h; d.layers = 1;
            d.color[0].view = gpu; d.color[0].load = NVN_GPU_LOAD_LOAD; d.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(c, &d); nvn_gpu_clear_rect(c, &cl); nvn_gpu_pass_end(c); rp_dirty_all();
        }
        rp_view_pin_end(); rp_tex_mark_gpu_written(t);
    }
}
