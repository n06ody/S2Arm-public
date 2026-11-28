// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

/* Specialization reuse is restricted to complete, direct combined tables.
 * In this shape HasDirectTextureFact is invariant and TrySeparateC2 is false.
 * Separate tables, buffer facts, arrays and unknown dependency sources retain
 * the exact identity path. Query results select whole facts, not guessed fields.
 *
 * fact_wide() (nvk_shader.c, debug.s2a.nvk_fact_wide, default NVK_FACT_WIDE_DEFAULT) adds two things to that contract:
 *  - The complete combined table may be followed by high-slot direct facts (c2 words 72..86, cbuf -1; fill_facts'
 *    high-slot loop). They never reach a masked key. The compiler reads the fact set only through keyed lookups
 *    (DirectGpuAccessor.FindTexture / HasDirectTextureFact / HasTextureFact, keyed by stage + handle + cbuf):
 *    a lookup of handle >= 72 is either a recorded query (QuerySamplerType ... RegisterTexture, found or "unbound";
 *    fact_cache_mask refuses to register such a program) or comes from a reflected binding of that handle
 *    (fact_cache_mask refuses reflected texture slots >= 32, images and buffer textures). So no registered program
 *    ever looked at a high-slot fact, and their presence, absence or format cannot change a reused result.
 *  - request:requiresLinkedVaryingInterface is an accepted dependency source: the request flag is part of the
 *    identity (version byte 5, input_identity.c), so the base key and every masked key encode it. */

static unsigned fact_cache_stages(const struct s2a_input_identity *id) {
    return id->stage_mask == 1 ? 1 : id->stage_mask == 3 ? 2 : id->stage_mask == 7 ? 3 : 0;
}

static int fact_cache_shape(const struct s2a_input_identity *id) {
    const unsigned stages = fact_cache_stages(id);
    if (!stages) return 0;
    const int combined = (int)(stages * 32);
    if (id->texture_count < combined || (id->texture_count != combined && !fact_wide())) return 0;
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
    /* After the complete table only high-slot direct facts are tolerated. Separate-table facts (cbuf 2), facts learned
     * from user constant buffers (cbuf >= 3) and buffer textures still send the whole identity down the exact path. */
    for (int i = combined; i < id->texture_count; i++) {
        const struct s2a_compile_texture_src *t = &id->textures[i];
        if (!t->stage || (strcmp(t->stage, "vs") && strcmp(t->stage, "fs") && strcmp(t->stage, "gs")) ||
            t->handle < 72 || t->handle > 86 || (t->handle & 1) || t->cbuf_slot != -1 || t->separate || t->is_buffer) return 0;
    }
    return 1;
}

/* A query whose arguments name a handle ("handle=N,cbufSlot=M") with N >= 72 reads outside the combined table. */
static int fact_args_high_handle(const char *args) {
    for (const char *p = args; (p = strstr(p, "handle=")) != NULL; p += 7)
        if ((p == args || p[-1] == ',') && strtoul(p + 7, NULL, 10) >= 72) return 1;
    return 0;
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
    /* Only the complete combined table (stages * 32 <= 96 entries) is selectable: trailing high-slot facts never reach
     * a key, and indexing mask[i / 32] past the table would alias mask[3] (the constant-buffer use bits). */
    const int combined = (int)(fact_cache_stages(id) * 32);
    if (mask) for (int i = 0; i < id->texture_count && i < combined; i++)
        if (mask[i / 32] & (1u << (i % 32))) tex[selected.texture_count++] = id->textures[i];
    uint8_t encoded[S2A_INPUT_IDENTITY_MAX]; size_t n;
    if (s2a_input_identity_encode(&selected, encoded, sizeof encoded, &n)) return 0;
    sha256(encoded, n, out);
    return 1;
}

static int fact_cache_mask(const uint8_t *bytes, size_t len, const struct rp_prog *pg, uint32_t mask[4]) {
    struct nvk_result_value doc = {(const char *)bytes, len, 0}, content, deps, v;
    char text[128];
    memset(mask, 0, 4 * sizeof *mask);
    // The program parser has already validated the successful live result.
    if ((rv_is_binary(bytes, len) && rv_frame(bytes, len, &doc)) ||
        rv_obj_find(doc, "content", &content) || rv_obj_find(content, "contract", &v) ||
        rv_str(v, text, sizeof text) || strcmp(text, "s2a-direct-program-content-v1") ||
        rv_obj_find(content, "dependencies", &deps) || !rv_is_array(deps)) return 0;
    /* Reflected resource use is included even if a future translator elides a
     * redundant query. Indirect/array resources need a separate contract. */
    for (unsigned st = 0; st < 3; st++) for (unsigned i = 0; i < pg->nent[st]; i++) {
        const nvn_gpu_layout_ent *e = &rp_prog_ent(pg, st)[i];
        if (e->src_kind == NVN_RB_TEX) {
            if (e->count != 1 || e->src_reg >= 32 || e->src_reg2 != UINT16_MAX) return 0;
            mask[st] |= 1u << e->src_reg;
        } else if (e->src_kind == NVN_RB_TBUF || e->cls == NVN_GPU_RES_IMAGE || e->cls == NVN_GPU_RES_IMAGE_TBUF) return 0;
    }
    /* Walk once: arr_item for every query would repeatedly scan long code-read
     * traces. No dependency string or game code is retained by this cache. */
    size_t offset = 0;
    unsigned code_reads = 0;
    int more;
    struct nvk_result_value item;
    while ((more = rv_array_next(deps, &offset, &item)) > 0) {
        if (rv_obj_find(item, "method", &v) || rv_str(v, text, sizeof text)) return 0;
        if (!strcmp(text, "GetCode")) code_reads++;
        if (!strcmp(text, "QueryConstantBufferUse")) {
            if (rv_obj_find(item, "arguments", &v) || rv_str(v, text, sizeof text)) return 0;
            int st = !strcmp(text, "stage=vs") ? 0 : !strcmp(text, "stage=fs") ? 1 : !strcmp(text, "stage=gs") ? 2 : -1;
            if (st < 0) return 0;
            mask[3] |= 1u << st;
        }
        /* The compile named a handle outside the combined table (found or unbound). High-slot facts are in no key, so such a
         * program must not be reused for identities that differ in them; it keeps the exact identity path. */
        if (fact_wide() && !rv_obj_find(item, "arguments", &v) && !rv_str(v, text, sizeof text) &&
            fact_args_high_handle(text)) return 0;
        if (rv_obj_find(item, "source", &v) || rv_str(v, text, sizeof text)) return 0;
        if (!strncmp(text, "texture-fact:", 13)) {
            char stage[3]; unsigned handle; int cb, consumed = 0;
            if (sscanf(text + 13, "%2[^:]:%u:%d%n", stage, &handle, &cb, &consumed) != 3 ||
                text[13 + consumed] || (cb != -1 && cb != 2) || handle < 8 || handle > 70 || (handle & 1)) return 0;
            int st = !strcmp(stage, "vs") ? 0 : !strcmp(stage, "fs") ? 1 : !strcmp(stage, "gs") ? 2 : -1;
            if (st < 0) return 0;
            mask[st] |= 1u << ((handle - 8) / 2);
        } else if (fact_wide() && !strcmp(text, "request:requiresLinkedVaryingInterface")) {
            /* QueryHostRequiresLinkedVaryingInterface: the request flag is part of the identity (version byte 5), hence of the
             * base key and every masked key, so a hit carries the same value. Other request:* sources need their own contract. */
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
    }
    return code_reads && more == 0;
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
    for (struct fact_variant *v = nvn_hm_get(&g_fact_variants, base); v; v = v->next) {
        uint8_t key[32];
        if (fact_cache_hash(id, v->mask, key) && !memcmp(key, v->key, 32)) {
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
