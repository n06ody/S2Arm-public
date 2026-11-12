// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 录制的贴图清除：整级整层走后端清图；区域或写掩码走临时附件通道。
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_state.h"
#include "nvn_unimpl.h"
#include "nvn_fmt_info.h"
#include <math.h>
#include <string.h>

static void clear_value(const p_clear_tex *o, int kind, nvn_gpu_clear *cl) {
    for (int k = 0; k < 4; k++) {
        double v = o->value_type == CLEAR_TEX_UINT ? (double)o->c.u[k] :
                   o->value_type == CLEAR_TEX_INT ? (double)o->c.i[k] : (double)o->c.f[k];
        if (kind == NVN_FMT_UINT) cl->value.u[k] = isnan(v) || v <= 0 ? 0u : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
        else if (kind == NVN_FMT_SINT) cl->value.i[k] = isnan(v) ? 0 : v <= -2147483648.0 ? INT32_MIN : v >= 2147483647.0 ? INT32_MAX : (int32_t)v;
        else cl->value.f[k] = (float)v;
    }
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
    // 后端附件只支持一级一层：每一层独立建视图和通道，避免把数组层数交给单层附件。
    for (int32_t z = 0; z < nz; z++) {
        view_t v = {0}; v.set_levels = 1; v.base_level = (uint8_t)lv; v.nlevels = 1;
        v.set_layers = 1; v.min_layer = (uint16_t)(l0 + (uint32_t)o->r.zoffset + (uint32_t)z); v.nlayers = 1;
        rp_view_pin_begin(); nvn_gpu_view gpu = rp_view_for(t, &v, c);
        if (!gpu) { rp_view_pin_end(); return; }
        if (whole) nvn_gpu_clear_view(c, gpu, cl.value.f, cl.value.u, is_int);
        else {
            nvn_gpu_pass_desc d = {0}; d.ncolor = 1; d.w = w; d.h = h; d.layers = 1;
            d.color[0].view = gpu; d.color[0].load = NVN_GPU_LOAD_LOAD; d.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(c, &d); nvn_gpu_clear_rect(c, &cl); nvn_gpu_pass_end(c); rp_dirty_all();
        }
        rp_view_pin_end(); rp_tex_mark_gpu_written(t);
    }
}
