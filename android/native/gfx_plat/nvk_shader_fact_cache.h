// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

/* Specialization reuse is restricted to complete, direct combined tables.
 * In this shape HasDirectTextureFact is invariant and TrySeparateC2 is false.
 * Separate tables, buffer facts, arrays and unknown dependency sources retain
 * the exact identity path. Query results select whole facts, not guessed fields. */

static int fact_cache_shape(const struct s2a_input_identity *id) {
    unsigned stages = id->stage_mask == 1 ? 1 : id->stage_mask == 3 ? 2 : id->stage_mask == 7 ? 3 : 0;
    if (!stages || id->texture_count != (int)(stages * 32)) return 0;
    for (unsigned st = 0; st < stages; st++) for (unsigned slot = 0; slot < 32; slot++) {
        const struct s2a_compile_texture_src *t = &id->textures[st * 32 + slot];
        if (!t->stage || strcmp(t->stage, (const char *const[]){"vs", "fs", "gs"}[st]) ||
            t->handle != 8 + 2 * slot || t->cbuf_slot != -1 || t->separate || t->is_buffer) return 0;
        /* array_length is not a shape condition: for 1d/2d-array targets it carries the texture's layer count
         * (texture_facts.c LAYERS_COUNT), and the compiler never reads it from a fact (DirectGpuAccessor reads
         * SamplerType / IsBuffer / Normalized / Format through recorded queries only). A queried slot's whole fact,
         * array_length included, is still part of the masked key. Rejecting it here turned reuse off for every
         * program of a title that keeps a 2d-array texture in any of the 64 slots. */
    }
    return 1;
}

static int fact_cache_hash(const struct s2a_input_identity *id, const uint32_t mask[4], uint8_t out[32]) {
    struct s2a_input_identity selected = *id;
    struct s2a_compile_texture_src tex[96];
    struct s2a_compile_graphics_src graphics = *id->gfx;
    /* rt_count is native bookkeeping; the compiler receives all eight output
     * types, which remain in the identity regardless of attachment count. */
    graphics.rt_count = 0; selected.gfx = &graphics;
    // Active-stage UBO masks remain strict even when no query is recorded.
    // fill_facts already omits stages not submitted to the compiler.
    selected.texture_count = 0; selected.textures = tex;
    if (mask) for (int i = 0; i < id->texture_count; i++)
        if (mask[i / 32] & (1u << (i % 32))) tex[selected.texture_count++] = id->textures[i];
    uint8_t encoded[S2A_INPUT_IDENTITY_MAX]; size_t n;
    if (s2a_input_identity_encode(&selected, encoded, sizeof encoded, &n)) return 0;
    sha256(encoded, n, out);
    return 1;
}

static int fact_cache_mask(const uint8_t *bytes, size_t len, const struct rp_prog *pg, uint32_t mask[4]) {
    struct s2a_jv doc = {(const char *)bytes, len}, content, deps, v;
    char text[128];
    memset(mask, 0, 4 * sizeof *mask);
    if (s2a_jv_obj_find(doc, "content", &content) || s2a_jv_obj_find(content, "contract", &v) ||
        s2a_jv_str(v, text, sizeof text) || strcmp(text, "s2a-direct-program-content-v1") ||
        s2a_jv_obj_find(content, "dependencies", &deps) || !s2a_jv_is_array(deps)) return 0;
    /* Reflected resource use is included even if a future translator elides a
     * redundant query. Indirect/array resources need a separate contract. */
    for (unsigned st = 0; st < 3; st++) for (unsigned i = 0; i < pg->nent[st]; i++) {
        const nvn_gpu_layout_ent *e = &pg->ent[st][i];
        if (e->src_kind == NVN_RB_TEX) {
            if (e->count != 1 || e->src_reg >= 32 || e->src_reg2 != UINT16_MAX) return 0;
            mask[st] |= 1u << e->src_reg;
        } else if (e->src_kind == NVN_RB_TBUF || e->cls == NVN_GPU_RES_IMAGE || e->cls == NVN_GPU_RES_IMAGE_TBUF) return 0;
    }
    /* Walk once: arr_item for every query would repeatedly scan long code-read
     * traces. No dependency string or game code is retained by this cache. */
    const char *p = s2a_jv_ws(deps.p + 1, deps.p + deps.n), *end = deps.p + deps.n;
    unsigned code_reads = 0;
    while (p < end && *p != ']') {
        const char *next;
        if (s2a_jv_scan(p, end, &next)) return 0;
        struct s2a_jv item = {p, (size_t)(next - p)};
        if (s2a_jv_obj_find(item, "method", &v) || s2a_jv_str(v, text, sizeof text)) return 0;
        if (!strcmp(text, "GetCode")) code_reads++;
        if (!strcmp(text, "QueryConstantBufferUse")) {
            if (s2a_jv_obj_find(item, "arguments", &v) || s2a_jv_str(v, text, sizeof text)) return 0;
            int st = !strcmp(text, "stage=vs") ? 0 : !strcmp(text, "stage=fs") ? 1 : !strcmp(text, "stage=gs") ? 2 : -1;
            if (st < 0) return 0;
            mask[3] |= 1u << st;
        }
        if (s2a_jv_obj_find(item, "source", &v) || s2a_jv_str(v, text, sizeof text)) return 0;
        if (!strncmp(text, "texture-fact:", 13)) {
            char stage[3]; unsigned handle; int cb, consumed = 0;
            if (sscanf(text + 13, "%2[^:]:%u:%d%n", stage, &handle, &cb, &consumed) != 3 ||
                text[13 + consumed] || (cb != -1 && cb != 2) || handle < 8 || handle > 70 || (handle & 1)) return 0;
            int st = !strcmp(stage, "vs") ? 0 : !strcmp(stage, "fs") ? 1 : !strcmp(stage, "gs") ? 2 : -1;
            if (st < 0) return 0;
            mask[st] |= 1u << ((handle - 8) / 2);
        } else if (strncmp(text, "code:sha256:", 12) && strncmp(text, "c1:sha256:", 10) &&
                   strncmp(text, "profile:", 8) && strncmp(text, "direct-v1:", 10) &&
                   strcmp(text, "graphicsState") && strcmp(text, "transformFeedbackEnabled") &&
                   strcmp(text, "binding-allocator") && strcmp(text, "translator") && strcmp(text, "unbound") &&
                   strcmp(text, "translator:phi-see-through (same as dmsh_xlate)") &&   /* QueryBindlessPhiSeeThrough: constant true */
                   strcmp(text, "texture-pool-capacity") && strcmp(text, "sampler-pool-capacity") &&
                   strcmp(text, "first-version-four-sets") && strcmp(text, "first-version-no-transform-feedback")) {
            /* A new source needs an explicit identity contract before reuse. */
            return 0;
        }
        p = s2a_jv_ws(next, end);
        if (p < end && *p == ',') p = s2a_jv_ws(p + 1, end);
        else if (p >= end || *p != ']') return 0;
    }
    return code_reads && p < end && *p == ']';
}

struct fact_variant {
    struct fact_variant *next;
    const struct rp_prog *prog;
    uint32_t mask[4];
    uint8_t key[32];
};
static nvn_hmap g_fact_variants = {.klen = 32};
/* Exact aliases are non-owning; only g_progs releases backend programs. */
static nvn_hmap g_fact_aliases = {.klen = 32};

static const struct rp_prog *fact_cache_find(const struct s2a_input_identity *id, const uint8_t base[32]) {
    struct rp_shader_hitch *h = g_fact_hitch;
    const uint64_t start = shader_hitch_start(h);
    for (struct fact_variant *v = nvn_hm_get(&g_fact_variants, base); v; v = v->next) {
        uint8_t key[32];
        if (fact_cache_hash(id, v->mask, key) && !memcmp(key, v->key, 32)) {
            if (h) {
                const int bad = v->prog->lib_bad;
                shader_hitch_lookup(h, start, !bad, bad);
            }
            return v->prog;
        }
    }
    return NULL;
}

/* Mask already computed from the result bytes (the caller may have freed them). */
static void fact_cache_keep_mask(const struct s2a_input_identity *id, const uint8_t base[32],
                                 const uint32_t mask[4], const struct rp_prog *pg) {
    struct fact_variant *v = calloc(1, sizeof *v);
    if (!v) return;
    if (!fact_cache_hash(id, mask, v->key)) { free(v); return; }
    memcpy(v->mask, mask, sizeof v->mask); v->prog = pg;
    v->next = nvn_hm_get(&g_fact_variants, base);
    nvn_hm_put(&g_fact_variants, base, v);
}

static void fact_cache_reset(void) {
    for (uint32_t i = 0; i < g_fact_variants.cap; i++) {
        struct fact_variant *v = g_fact_variants.vals[i];
        while (v) { struct fact_variant *next = v->next; free(v); v = next; }
    }
    free(g_fact_variants.keys); free(g_fact_variants.vals); nvn_hm_init(&g_fact_variants, 32);
    free(g_fact_aliases.keys); free(g_fact_aliases.vals); nvn_hm_init(&g_fact_aliases, 32);
}
