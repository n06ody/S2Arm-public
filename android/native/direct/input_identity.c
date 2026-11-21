#include "input_identity.h"

#include <stdio.h>
#include <stdlib.h>
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
    if (in->requires_linked_varying_interface != 0 && in->requires_linked_varying_interface != 1) return -1;
    if (in->requires_linked_varying_interface && in->stage_mask != 3) return -1;
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
    // 5 = directly paired VS->FS uses actual fragment inputs. Do not reuse v4:
    // that version already identifies high-cbuf inputs under the old policy.
    // Standalone VS, compute and VS->GS->FS retain their existing v3/v4 bytes.
    wu8(&w, in->requires_linked_varying_interface ? 5 : (wide_cbuf ? 4 : 3));
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
        else if (t->stage && !strcmp(t->stage, "gs")) stage_idx = 3;
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
// ---------------- 解码 ----------------
// 语义：解析出与 encode 完全一致的字段，再要求「用同一编码器重编码逐字节等于输入」。后半句是硬判据：
// 版本字节与内容是否一致、标志位是否被归一化、纹理名字索引是否规范、末尾有没有多余字节，全部由它兜住；
// 前面逐字段的检查只是为了把坏 blob 的失败原因写清楚，不是判据的全部。
struct rdr { const uint8_t *p; size_t len, off; const char *why; };
static int rd_fail(struct rdr *r, const char *why) { if (!r->why) r->why = why; return -1; }
static int rd_bytes(struct rdr *r, void *dst, size_t n) {
    if (n > r->len - r->off) return rd_fail(r, "blob truncated");
    if (dst) memcpy(dst, r->p + r->off, n);
    r->off += n;
    return 0;
}
static int rd_u8(struct rdr *r, uint8_t *v) { return rd_bytes(r, v, 1); }
static int rd_u16(struct rdr *r, uint16_t *v) {
    uint8_t b[2];
    if (rd_bytes(r, b, 2)) return -1;
    *v = (uint16_t)(b[0] | (uint16_t)b[1] << 8);
    return 0;
}
static int rd_u32(struct rdr *r, uint32_t *v) {
    uint8_t b[4];
    if (rd_bytes(r, b, 4)) return -1;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}
static int rd_i32(struct rdr *r, int32_t *v) { uint32_t u; if (rd_u32(r, &u)) return -1; *v = (int32_t)u; return 0; }
static int rd_f32(struct rdr *r, float *v) { uint32_t u; if (rd_u32(r, &u)) return -1; memcpy(v, &u, sizeof u); return 0; }
// 0xFFFFFFFF = NULL；否则显式长度 + 字节（空串是 0 + 无字节，与 NULL 不同）。超出存储上限就失败，不截断。
static int rd_str(struct rdr *r, char *dst, size_t cap, const char **out) {
    uint32_t n;
    if (rd_u32(r, &n)) return -1;
    if (n == 0xFFFFFFFFu) { if (out) *out = NULL; return 0; }
    if (n >= cap) return rd_fail(r, "string exceeds decode storage");
    if (n > r->len - r->off) return rd_fail(r, "string truncated");
    memcpy(dst, r->p + r->off, n); dst[n] = 0; r->off += n;
    if (out) *out = dst;
    return 0;
}
// 编码端只写 0/1：其它字节重编码会归一化，所以不接受（否则 blob 不能原样重编码）。
static int rd_flag(struct rdr *r, int *v) {
    uint8_t b;
    if (rd_u8(r, &b)) return -1;
    if (b > 1) return rd_fail(r, "flag byte is not 0/1");
    *v = b;
    return 0;
}
static void set_why(char *why, size_t n, const char *text) { if (why && n) snprintf(why, n, "%s", text); }

// 纹理 stage 索引 → 名字（与 encode 的 {"vs","fs","cs","gs"} 同源）。
static const char *const g_stage_names[4] = { "vs", "fs", "cs", "gs" };

int s2a_input_identity_decode(const uint8_t *buf, size_t len, struct s2a_input_identity_decoded *out,
                              char *why, size_t why_size) {
    if (!buf || !len || !out) { set_why(why, why_size, "missing buffer or output"); return -1; }
    if (len > S2A_INPUT_IDENTITY_MAX) { set_why(why, why_size, "blob larger than S2A_INPUT_IDENTITY_MAX"); return -1; }
    memset(out, 0, sizeof *out);
    struct rdr r = { buf, len, 0, NULL };
    unsigned version = 0;
    {
        static const uint8_t magic[5] = { 'S', '2', 'A', 'I', '1' };
        if (rd_bytes(&r, NULL, 0)) return -1;
        if (r.len < sizeof magic || memcmp(r.p, magic, sizeof magic)) { set_why(why, why_size, "identity magic mismatch"); return -1; }
        r.off = sizeof magic;
    }
    uint8_t vb = 0;
    if (rd_u8(&r, &vb)) goto bad;
    if (vb != 3 && vb != 4 && vb != 5) { set_why(why, why_size, "unknown identity version"); return -1; }
    version = vb;
    out->id.requires_linked_varying_interface = version == 5;
    if (rd_i32(&r, (int32_t *)&out->id.direct_abi)) goto bad;
    if (rd_str(&r, out->closure, sizeof out->closure, &out->id.translator_closure)) goto bad;
    if (!out->id.translator_closure) { set_why(why, why_size, "identity has no translator closure"); return -1; }
    if (rd_bytes(&r, out->id.profile_sha, sizeof out->id.profile_sha)) goto bad;
    if (rd_u32(&r, &out->id.stage_mask)) goto bad;
    if (version == 5 && out->id.stage_mask != 3u) { set_why(why, why_size, "linked identity requires exact VS/FS stages"); return -1; }
    if (rd_bytes(&r, out->id.keys, sizeof out->id.keys)) goto bad;
    {
        uint32_t sm = out->id.stage_mask;
        if (!sm || (sm & ~0x3Fu)) { set_why(why, why_size, "stage mask is empty or beyond the six NVN stages"); return -1; }
        unsigned stages = 0;
        for (uint32_t m = sm; m; m >>= 1) stages += m & 1u;
        if (rd_u8(&r, &out->id.source_present)) goto bad;
        if (out->id.source_present != (uint8_t)((1u << stages) - 1u)) { set_why(why, why_size, "source proof does not cover the stage mask"); return -1; }
        for (unsigned i = 0; i < stages; i++) {
            if (rd_bytes(&r, out->id.source_sha[i], 32)) goto bad;
            if (rd_u32(&r, &out->id.source_code_bytes[i])) goto bad;
            if (rd_u32(&r, &out->id.source_control_bytes[i])) goto bad;
            // 编码端拒绝 0 长度：解码必须同样拒绝，否则重编码必然失败（这里给出更准确的原因）。
            if (!out->id.source_code_bytes[i] || !out->id.source_control_bytes[i]) {
                set_why(why, why_size, "real source length is zero");
                return -1;
            }
        }
    }
    for (int i = 0; i < 6; i++) if (rd_u32(&r, &out->id.ubo_use[i])) goto bad;
    if (rd_u32(&r, &out->id.texture_array_length)) goto bad;
    if (rd_u32(&r, &out->id.sampler_array_length)) goto bad;
    if (rd_flag(&r, &out->id.transform_feedback_enabled)) goto bad;
    {
        int wide_cbuf = 0;
        for (int i = 0; i < 6; i++) if (out->id.ubo_use[i] & ~0x1Fu) wide_cbuf = 1;
        // 版本字节是内容的一部分：v4 只在出现旧 5 位窗口外的 constantBufferUse 位时才有。别的组合重编码
        // 会变成另一版（v3/v4 不能互认）；v5 明确表示 linked VS/FS，低/高 cbuf 都保留 v5。
        if (version != (unsigned)(out->id.requires_linked_varying_interface ? 5 : (wide_cbuf ? 4 : 3))) { set_why(why, why_size, "identity version does not match its content"); return -1; }
    }
    out->id.compute_has_unaligned_ssbo = 0;
    out->id.compute_unaligned_ssbo = 0;
    out->id.gfx = &out->gfx;
    out->id.textures = out->tex;
    out->id.texture_count = 0;
    {
        struct s2a_compile_graphics_src *g = &out->gfx;
        if (rd_i32(&r, &g->rt_count)) goto bad;
        if (rd_str(&r, out->topology, sizeof out->topology, &g->topology)) goto bad;
        if (rd_flag(&r, &g->has_tess_mode)) goto bad;
        if (rd_u32(&r, &g->tess_mode)) goto bad;
        for (int i = 0; i < 8; i++) if (rd_str(&r, out->fragment_output_types[i], sizeof out->fragment_output_types[i], &g->fragment_output_types[i])) goto bad;
        for (int i = 0; i < 32; i++) if (rd_str(&r, out->attribute_types[i], sizeof out->attribute_types[i], &g->attribute_types[i])) goto bad;
        if (rd_flag(&r, &g->constant_buffer_use_overflow)) goto bad;
        if (rd_flag(&r, &g->has_alpha_to_coverage)) goto bad;
        if (rd_flag(&r, &g->alpha_to_coverage_enable)) goto bad;
        if (rd_flag(&r, &g->alpha_to_coverage_dither)) goto bad;
        if (rd_flag(&r, &g->has_alpha_test)) goto bad;
        if (rd_str(&r, out->alpha_test_compare, sizeof out->alpha_test_compare, &g->alpha_test_compare)) goto bad;
        if (rd_flag(&r, &g->has_blend)) goto bad;
        if (rd_flag(&r, &g->dual_source_blend)) goto bad;
        if (rd_flag(&r, &g->has_depth_mode)) goto bad;
        if (rd_i32(&r, &g->depth_mode)) goto bad;
        if (rd_flag(&r, &g->has_y_negate)) goto bad;
        if (rd_flag(&r, &g->y_negate)) goto bad;
        if (rd_flag(&r, &g->has_origin)) goto bad;
        if (rd_flag(&r, &g->origin_upper_left)) goto bad;
        if (rd_flag(&r, &g->has_halve_primitive_id)) goto bad;
        if (rd_flag(&r, &g->halve_primitive_id)) goto bad;
        if (rd_flag(&r, &g->has_cb_draw_params)) goto bad;
        if (rd_flag(&r, &g->cb_draw_params)) goto bad;
        if (rd_flag(&r, &g->has_point_size)) goto bad;
        if (rd_flag(&r, &g->program_point_size_enable)) goto bad;
        if (rd_f32(&r, &g->point_size)) goto bad;
        if (rd_flag(&r, &g->has_alpha_test_reference)) goto bad;
        if (rd_f32(&r, &g->alpha_test_reference)) goto bad;
        if (rd_flag(&r, &g->has_unaligned_ssbo)) goto bad;
        if (rd_flag(&r, &g->unaligned_ssbo)) goto bad;
        if (rd_flag(&r, &g->has_geometry_shader)) goto bad;
        if (rd_flag(&r, &g->host_supports_quads)) goto bad;
        if (rd_flag(&r, &g->early_z_force)) goto bad;
        if (rd_flag(&r, &g->viewport_transform_disable)) goto bad;
    }
    {
        uint32_t ntex = 0;
        if (rd_u32(&r, &ntex)) goto bad;
        if (ntex > S2A_COMPILE_TEXTURE_MAX) { set_why(why, why_size, "texture fact count exceeds the compiler limit"); return -1; }
        out->id.texture_count = (int)ntex;
        for (uint32_t i = 0; i < ntex; i++) {
            uint8_t stage_idx = 0, target_idx = 0, packed = 0;
            uint16_t format_idx = 0;
            int32_t cbuf_slot = 0;
            uint32_t handle = 0, array_length = 0;
            const char *target_name = NULL, *format_name = NULL;
            if (rd_u8(&r, &stage_idx)) goto bad;
            if (stage_idx >= 4) { set_why(why, why_size, "texture stage index is unknown"); return -1; }
            if (rd_u32(&r, &handle)) goto bad;
            if (rd_i32(&r, &cbuf_slot)) goto bad;
            if (cbuf_slot < -1) { set_why(why, why_size, "texture cbuf slot is below -1"); return -1; }
            if (rd_u8(&r, &target_idx)) goto bad;
            if (rd_u16(&r, &format_idx)) goto bad;
            if (rd_u8(&r, &packed)) goto bad;
            if (packed & ~7u) { set_why(why, why_size, "texture flag byte has unknown bits"); return -1; }
            if (rd_u32(&r, &array_length)) goto bad;
            // 名字索引 → 名字：只接受编码器会产出的那个索引（"2d" 有两个目标索引，编码时取第一个）。
            if (s2a_nvn_texture_target_at(target_idx, &target_name)) { set_why(why, why_size, "texture target index is unknown"); return -1; }
            if (s2a_nvn_texture_format_at(format_idx, NULL, &format_name) || !format_name) { set_why(why, why_size, "texture format index is unknown"); return -1; }
            uint8_t ti = 0;
            uint16_t fi = 0;
            if (s2a_nvn_texture_target_index(target_name, &ti) || ti != target_idx) { set_why(why, why_size, "texture target index is not canonical"); return -1; }
            if (s2a_nvn_texture_format_index(format_name, &fi) || fi != format_idx) { set_why(why, why_size, "texture format index is not canonical"); return -1; }
            snprintf(out->tex_stage[i], sizeof out->tex_stage[i], "%s", g_stage_names[stage_idx]);
            snprintf(out->tex_sampler_type[i], sizeof out->tex_sampler_type[i], "%s", target_name);
            snprintf(out->tex_format[i], sizeof out->tex_format[i], "%s", format_name);
            out->tex[i].stage = out->tex_stage[i];
            out->tex[i].handle = handle;
            out->tex[i].cbuf_slot = (int)cbuf_slot;
            out->tex[i].sampler_type = out->tex_sampler_type[i];
            out->tex[i].format = out->tex_format[i];
            out->tex[i].is_buffer = packed & 1u;
            out->tex[i].normalized = (packed >> 1) & 1u;
            out->tex[i].separate = (packed >> 2) & 1u;
            out->tex[i].array_length = array_length;
        }
    }
    if (out->id.stage_mask == 0x20) {
        // 计算身份的对齐事实是必填的（编码端要求 has==1）：缺了重编码会失败，这里明确拒绝。
        int has = 0, value = 0;
        if (rd_flag(&r, &has)) goto bad;
        if (rd_flag(&r, &value)) goto bad;
        if (has != 1) { set_why(why, why_size, "compute identity is missing the unaligned-SSBO fact"); return -1; }
        out->id.compute_has_unaligned_ssbo = has;
        out->id.compute_unaligned_ssbo = value;
    }
    if (r.off != r.len) { set_why(why, why_size, "trailing bytes after the canonical input"); return -1; }
    {
        // 最终判据：重编码逐字节等于输入。任何解析时放过、编码器却会改写的规范性问题都在这里被拒。
        size_t cap = len < 256 ? 256 : len;
        uint8_t *again = (uint8_t *)malloc(cap);
        size_t again_len = 0;
        if (!again) { set_why(why, why_size, "out of memory for the round-trip check"); return -1; }
        int rc = s2a_input_identity_encode(&out->id, again, cap, &again_len);
        int same = rc == 0 && again_len == len && !memcmp(again, buf, len);
        free(again);
        if (!same) { set_why(why, why_size, "blob is not canonical: re-encoding differs"); return -1; }
    }
    return 0;
bad:
    set_why(why, why_size, r.why ? r.why : "malformed identity");
    return -1;
}
