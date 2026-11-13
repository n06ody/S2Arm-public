#include "input_identity.h"

#include <string.h>

#include "texture_facts.h"

struct w {
    uint8_t *p;
    size_t n, cap;
    int overflow;
};

static void wb(struct w *w, const void *d, size_t n) {
    if (w->overflow || n > w->cap - w->n) { w->overflow = 1; return; }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}

static void wu8(struct w *w, uint8_t v) { wb(w, &v, 1); }
static void wu32(struct w *w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    wb(w, b, sizeof b);
}
static void wu16(struct w *w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    wb(w, b, sizeof b);
}
static void wi32(struct w *w, int32_t v) { wu32(w, (uint32_t)v); }
static void wf32(struct w *w, float v) { uint32_t bits; memcpy(&bits, &v, sizeof bits); wu32(w, bits); }

// 0xFFFFFFFF = NULL；其余为显式长度 + 字节（空串是 0 + 无字节，与 NULL 不同）。
static void wstr(struct w *w, const char *s) {
    if (!s) { wu32(w, 0xFFFFFFFFu); return; }
    size_t n = strlen(s);
    wu32(w, (uint32_t)n);
    wb(w, s, n);
}

static void wflag(struct w *w, int v) { wu8(w, v ? 1 : 0); }

int s2a_input_identity_encode(const struct s2a_input_identity *in, uint8_t *buf, size_t cap, size_t *len) {
    if (!in || !in->gfx || !in->translator_closure || !buf || !len) return -1;
    if (in->texture_count < 0 || in->texture_count > S2A_COMPILE_TEXTURE_MAX) return -1;
    if (in->texture_count && !in->textures) return -1;
    // 阶段掩码只有 6 个 NVN 阶段位；每个已置位阶段都必须带完整源摘要与真实长度。
    if (!in->stage_mask || (in->stage_mask & ~0x3Fu)) return -1;
    unsigned stages = 0;
    for (uint32_t m = in->stage_mask; m; m >>= 1) stages += m & 1u;
    if (stages > 6) return -1;
    if (in->source_present != (uint8_t)((1u << stages) - 1u)) return -1;
    for (unsigned i = 0; i < stages; i++) {
        if (!in->source_code_bytes[i] || !in->source_control_bytes[i]) return -1;
    }
    // 只需要放得下固定段；真正不够由 w.overflow 判定后返回 -1。
    // 不强制调用方预分配 S2A_INPUT_IDENTITY_MAX，避免把“容量上限”变成 API 门槛。
    if (cap < 256) return -1;
    struct w w = { buf, 0, cap, 0 };
    wb(&w, "S2AI1", 5);
    // 3 = 纳入完整源摘要与真实长度；与 2/1 不通，跨版本不能互认。
    // 4 = constantBufferUse 出现旧 5 位窗口（cbuf 0..4）之外的位（cbuf 5..18）。计算与图形都得这样：
    // 图形填表同样扩展到完整掩码后，高位不再被截断，v3 与 v4 必须分开，否则旧缓存里的截断掩码程序
    // 会和新程序混判。只有真正用到高位的输入才用 4：掩码落在旧窗口内的输入逐字节保持 3。
    int wide_cbuf = 0;
    for (int i = 0; i < 6; i++) if (in->ubo_use[i] & ~0x1Fu) wide_cbuf = 1;
    wu8(&w, wide_cbuf ? 4 : 3);
    wi32(&w, in->direct_abi);
    wstr(&w, in->translator_closure);
    wb(&w, in->profile_sha, sizeof in->profile_sha);
    wu32(&w, in->stage_mask);
    wb(&w, in->keys, sizeof in->keys);
    // 完整源身份：短 key 只是摘要前缀，碰巧相同的 8 字节不能证明代码/控制段相同。
    wu8(&w, in->source_present);
    for (unsigned i = 0; i < stages; i++) {
        wb(&w, in->source_sha[i], 32);
        wu32(&w, in->source_code_bytes[i]);
        wu32(&w, in->source_control_bytes[i]);
    }
    for (int i = 0; i < 6; i++) wu32(&w, in->ubo_use[i]);
    wu32(&w, in->texture_array_length);
    wu32(&w, in->sampler_array_length);
    wflag(&w, in->transform_feedback_enabled);

    const struct s2a_compile_graphics_src *g = in->gfx;
    wi32(&w, g->rt_count);
    wstr(&w, g->topology);
    wflag(&w, g->has_tess_mode);
    wu32(&w, g->tess_mode);
    for (int i = 0; i < 8; i++) wstr(&w, g->fragment_output_types[i]);
    for (int i = 0; i < 32; i++) wstr(&w, g->attribute_types[i]);
    wflag(&w, g->constant_buffer_use_overflow);
    wflag(&w, g->has_alpha_to_coverage);
    wflag(&w, g->alpha_to_coverage_enable);
    wflag(&w, g->alpha_to_coverage_dither);
    wflag(&w, g->has_alpha_test);
    wstr(&w, g->alpha_test_compare);
    wflag(&w, g->has_blend);
    wflag(&w, g->dual_source_blend);
    wflag(&w, g->has_depth_mode);
    wi32(&w, g->depth_mode);
    wflag(&w, g->has_y_negate);
    wflag(&w, g->y_negate);
    wflag(&w, g->has_origin);
    wflag(&w, g->origin_upper_left);
    wflag(&w, g->has_halve_primitive_id);
    wflag(&w, g->halve_primitive_id);
    wflag(&w, g->has_cb_draw_params);
    wflag(&w, g->cb_draw_params);
    wflag(&w, g->has_point_size);
    wflag(&w, g->program_point_size_enable);
    wf32(&w, g->point_size);
    wflag(&w, g->has_alpha_test_reference);
    wf32(&w, g->alpha_test_reference);
    wflag(&w, g->has_unaligned_ssbo);
    wflag(&w, g->unaligned_ssbo);
    wflag(&w, g->has_geometry_shader);
    wflag(&w, g->host_supports_quads);
    wflag(&w, g->early_z_force);
    wflag(&w, g->viewport_transform_disable);
    // 纹理事实：漏一条就是“不同纹理、同一身份”的误命中（与 constantBufferUse 反例同类）。
    wu32(&w, (uint32_t)(in->texture_count < 0 ? 0 : in->texture_count));
    // 纹理事实用定长索引编码：每个条目固定 17 字节，名字只经 s2a_nvn_texture_*_index
    // 映射一次，容量不再随格式名长度漂移。未知 stage/目标/格式一律明确失败——
    // 宁可让这次查找不命中，也不能截断成“看起来相同”的输入（那是误命中的入口）。
    for (int i = 0; i < in->texture_count; i++) {
        const struct s2a_compile_texture_src *t = in->textures ? &in->textures[i] : NULL;
        if (!t) return -1;
        uint8_t stage_idx;
        if (t->stage && !strcmp(t->stage, "vs")) stage_idx = 0;
        else if (t->stage && !strcmp(t->stage, "fs")) stage_idx = 1;
        else if (t->stage && !strcmp(t->stage, "cs")) stage_idx = 2;
        else return -1;   // 缺 stage 不等同 vs：NULL 与 "vs" 不能编成同一个输入
        uint8_t target_idx = 0;
        uint16_t format_idx = 0;
        if (s2a_nvn_texture_target_index(t->sampler_type, &target_idx)) return -1;
        if (s2a_nvn_texture_format_index(t->format, &format_idx)) return -1;
        if (t->cbuf_slot < -1) return -1;
        wu8(&w, stage_idx);
        wu32(&w, t->handle);
        wi32(&w, t->cbuf_slot);
        wu8(&w, target_idx);
        wu16(&w, format_idx);
        wu8(&w, (uint8_t)((t->is_buffer ? 1u : 0u) | (t->normalized ? 2u : 0u) | (t->separate ? 4u : 0u)));
        wu32(&w, t->array_length);
    }
    if (in->stage_mask == 0x20) {
        if (in->compute_has_unaligned_ssbo != 1 ||
            (in->compute_unaligned_ssbo != 0 && in->compute_unaligned_ssbo != 1)) return -1;
        wflag(&w, in->compute_has_unaligned_ssbo);
        wflag(&w, in->compute_unaligned_ssbo);
    }
    if (w.overflow) return -1;
    *len = w.n;
    return 0;
}
