// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Compute-only immutable REQUEST and S2AI1 identity, backed by one source pin.
#include "nvn_vk_compute_request.h"
#include "direct/sha256.h"
#include "direct/json.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

struct sb {
    char *p;
    size_t n, cap;
    int oom;
};

static void sb_raw(struct sb *b, const char *s, size_t n) {
    if (b->oom) return;
    if (n > (2u << 20) || b->n > (2u << 20) - n) { b->oom = 1; return; }
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = (char *)realloc(b->p, cap);
        if (!p) { b->oom = 1; return; }
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_puts(struct sb *b, const char *s) { sb_raw(b, s, strlen(s)); }

static void sb_u64(struct sb *b, unsigned long long v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%llu", v);
    sb_raw(b, t, (size_t)n);
}

static void sb_u32(struct sb *b, unsigned v) {
    char t[24];
    int n = snprintf(t, sizeof t, "%u", v);
    sb_raw(b, t, (size_t)n);
}

// 只用于我们自己的短 ASCII 字符串（requestId/closure/stage/topology）；仍按 JSON 规则转义。
static void sb_json_string(struct sb *b, const char *s) {
    sb_puts(b, "\"");
    for (const unsigned char *c = (const unsigned char *)s; c && *c; c++) {
        char t[8];
        switch (*c) {
            case '"': sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n"); break;
            case '\r': sb_puts(b, "\\r"); break;
            case '\t': sb_puts(b, "\\t"); break;
            default:
                if (*c < 0x20) { snprintf(t, sizeof t, "\\u%04x", *c); sb_puts(b, t); }
                else sb_raw(b, (const char *)c, 1);
                break;
        }
    }
    sb_puts(b, "\"");
}

static void sb_key_string(struct sb *b, const char *k, const char *v) {
    sb_json_string(b, k);
    sb_puts(b, ":");
    sb_json_string(b, v);
}

static void sb_key_u64(struct sb *b, const char *k, unsigned long long v) {
    sb_json_string(b, k);
    sb_puts(b, ":");
    sb_u64(b, v);
}

static void sb_key_u32(struct sb *b, const char *k, unsigned v) {
    sb_json_string(b, k);
    sb_puts(b, ":");
    sb_u32(b, v);
}

static void sb_key_i32(struct sb *b, const char *k, int v) {
    sb_json_string(b, k);
    sb_puts(b, ":");
    if (v < 0) { sb_puts(b, "-"); sb_u32(b, (unsigned)(-(long long)v)); }
    else sb_u32(b, (unsigned)v);
}

static void sb_base64(struct sb *b, const uint8_t *data, size_t len) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sb_puts(b, "\"");
    if (data && len) {
        for (size_t i = 0; i < len; i += 3) {
            uint32_t v = (uint32_t)data[i] << 16;
            int have = 1;
            if (i + 1 < len) { v |= (uint32_t)data[i + 1] << 8; have = 2; }
            if (i + 2 < len) { v |= (uint32_t)data[i + 2]; have = 3; }
            char o[4] = { t[(v >> 18) & 63], t[(v >> 12) & 63], have >= 2 ? t[(v >> 6) & 63] : '=',
                          have >= 3 ? t[v & 63] : '=' };
            sb_raw(b, o, 4);
        }
    }
    sb_puts(b, "\"");
}


static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void hash(const void *p, size_t n, uint8_t out[32]) {
    s2a_sha256 h;
    s2a_sha256_init(&h);
    s2a_sha256_update(&h, p, n);
    s2a_sha256_final(&h, out);
}
void nvk_compute_request_release(struct nvk_compute_request *r) {
    if (!r) return;
    if (r->source.ref) s2a_source_release(r->source.ref);
    free(r->json);
    memset(r, 0, sizeof *r);
}
static int fail(struct nvk_compute_request *r, char *why, size_t cap, const char *message) {
    nvk_compute_request_release(r);
    if (why && cap) snprintf(why, cap, "%s", message);
    return -1;
}

int nvk_compute_profile_parse(const struct s2a_direct_compile_context *c,
    struct nvk_compute_profile_facts *out, char *why, size_t cap) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
#define PROFILE_REJECT(message) do { if (why && cap) snprintf(why, cap, "%s", message); memset(out, 0, sizeof *out); return -1; } while (0)
    if (!c || !c->profile_json || !c->profile_bytes || c->profile_bytes > S2A_JSON_MAX_BYTES)
        PROFILE_REJECT("invalid profile context");
    s2a_json_object profile;
    if (s2a_json_parse_object((const uint8_t *)c->profile_json, c->profile_bytes, &profile, NULL, 0))
        { PROFILE_REJECT("invalid profile JSON"); }
    const s2a_json_field *field = s2a_json_get(&profile, "schema");
    if (!field || field->kind != S2A_JSON_STRING || field->text_bytes != strlen("s2a-direct-profile-v1") ||
        strcmp(field->text, "s2a-direct-profile-v1"))
        { PROFILE_REJECT("compute profile schema invalid"); }
    field = s2a_json_get(&profile, "profileHash");
    if (!field || field->kind != S2A_JSON_STRING || field->text_bytes != 71 ||
        strncmp(field->text, "sha256-", 7) || !c->profile_hash || strcmp(field->text, c->profile_hash))
        { PROFILE_REJECT("compute profile hash differs from provider"); }
    for (unsigned i = 7; i < 71; ++i)
        if (!((field->text[i] >= '0' && field->text[i] <= '9') ||
              (field->text[i] >= 'a' && field->text[i] <= 'f')))
            { PROFILE_REJECT("compute profile hash invalid"); }
    memcpy(out->hash, field->text, sizeof out->hash);
    field = s2a_json_get(&profile, "capabilities");
    if (!field || field->kind != S2A_JSON_OBJECT)
        { PROFILE_REJECT("compute profile capabilities missing"); }
    const uint8_t *caps_json = field->raw; size_t caps_bytes = field->raw_bytes;
    if (s2a_json_parse_object(caps_json, caps_bytes, &profile, NULL, 0))
        { PROFILE_REJECT("compute profile capabilities invalid"); }
    field = s2a_json_get(&profile, "storageBufferOffsetAlignment");
    if (!field || field->kind != S2A_JSON_NUMBER || !field->is_unsigned ||
        !field->unsigned_value || field->unsigned_value > 65536 ||
        (field->unsigned_value & (field->unsigned_value - 1)))
        { PROFILE_REJECT("compute profile storage alignment missing or invalid"); }
    out->storage_alignment = (uint32_t)field->unsigned_value;
    return 0;
#undef PROFILE_REJECT
}

int nvk_compute_request_prepare(const struct s2a_record_dispatch_state *s,
    const struct s2a_direct_compile_context *c,
    const struct nvk_compute_texture_facts *textures, int has_unaligned_ssbo, int unaligned_ssbo,
    const char *request_id,
    struct nvk_compute_request *out, char *why, size_t cap) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (why && cap) why[0] = 0;
#define REJECT(message) return fail(out, why, cap, message)
    if (!s || !c || !textures || !request_id || !*request_id || strlen(request_id) > 128 ||
        !c->session || !c->generation || !c->direct_abi || c->direct_abi > INT_MAX ||
        !c->translator_closure || !*c->translator_closure || strlen(c->translator_closure) > 256 ||
        !c->profile_json || !c->profile_bytes || c->profile_bytes > S2A_JSON_MAX_BYTES)
        { REJECT("invalid compile context"); }
    if (has_unaligned_ssbo != 1 || (unaligned_ssbo != 0 && unaligned_ssbo != 1))
        { REJECT("missing or invalid CS alignment fact"); }
    if (s->stages != 32 || s->ubo_use_overflow || (s->ubo_use[5] & ~0x7FFFFu)) {
        // constantBufferUse 是「cbuf 号掩码」：位 j = NVN cbuf j。旧 5 位窗口（cbuf 0..4 =
        // 用户槽 0..1）保持原语义；compute 现在把 16 个用户槽（cbuf 3..18）全部表达出来。
        // 超出 cbuf 0..18 或记录层已判不可表达（overflow）时仍 fail-closed，并报出实际值。
        char message[160];
        snprintf(message, sizeof message,
            "unsupported compute stage or constant-buffer mask: stages=0x%x ubo_use=0x%x overflow=%d",
            s->stages, s->ubo_use[5], s->ubo_use_overflow ? 1 : 0);
        REJECT(message);
    }
    if (!s->tex_pool_seen || !s->samp_pool_seen || !s->tex_pool_n || !s->samp_pool_n ||
        s->tex_pool_n > 1048576 || s->samp_pool_n > 1048576)
        { REJECT("missing or out-of-range descriptor-pool facts"); }
    if (!textures->complete || textures->count < 0 || textures->count > S2A_COMPILE_TEXTURE_MAX ||
        (textures->count && !textures->items)) { REJECT("missing complete CS texture facts"); }
    for (int i = 0; i < textures->count; i++) {
        const struct s2a_compile_texture_src *t = &textures->items[i];
        if (!t->stage || strcmp(t->stage, "cs") || !t->format || !t->sampler_type ||
            t->handle > (1u << 20) || t->cbuf_slot < -1 || t->cbuf_slot > (1 << 20) ||
            !t->array_length || t->array_length > 65536 ||
            (t->is_buffer != 0 && t->is_buffer != 1) || (t->normalized != 0 && t->normalized != 1) ||
            (t->separate != 0 && t->separate != 1)) { REJECT("invalid CS texture fact"); }
        for (int j = 0; j < i; j++)
            if (textures->items[j].handle == t->handle && textures->items[j].cbuf_slot == t->cbuf_slot)
                { REJECT("duplicate CS texture query"); }
    }
    if (nvk_compute_profile_parse(c, &out->profile, why, cap)) return -1;
    if (!s2a_source_acquire(s->keys[0], &out->source) || !out->source.ref || !out->source.complete ||
        !out->source.code || !out->source.code_bytes || (out->source.code_bytes & 3u) ||
        !out->source.control || out->source.control_bytes < 0x75cu)
        { REJECT("missing complete pinned CS source"); }
    if (s2a_control_parse_cs(out->source.control, out->source.control_bytes, &out->cs))
        { REJECT("invalid CS local size or shared memory"); }
    // 计算源的指令流偏移，与桌面 dsh（graphics/tools/direct/dsh/Dsh.cs:149-155）和
    // translator/shader_split.py 同一口径：
    //   · 图形 code area：0x12345678（78 56 34 12）头 0x30 字节，指令从 +0x30 起；
    //   · 计算 code area：0x12345679（79 56 34 12）头，+0x30..+0x100 必须是零填充，指令从 +0x100 起；
    //   · 计算源也可能完全无头（SDK 10 的游戏）：指令从 +0 起，不剥。
    // 真机 0100F2C0115B6000 的 compute 源正是「79 头 + 0x30..0x100 零 + 指令」：整段扫描第一个非零字节
    // 在 0x100（ncode 0x1100 → 指令 0x1000），控制段 +0x700/+0x704/+0x708 = c1 起点/长度/头长
    // （0x1000/0x1100/0x100），图形对照是 0xa00/0xb00/0x30。此前把 0x30 字节的零填充当指令交给
    // 翻译器，报 `Invalid instruction at 0x000008`。任何不符合上述布局的源都明确拒绝，不猜偏移。
    const uint8_t *cs_code = out->source.code;
    uint32_t cs_code_bytes = out->source.code_bytes;
    int cs_off = 0;
    if (cs_code_bytes >= 4 && cs_code[0] == 0x79 && cs_code[1] == 0x56 &&
        cs_code[2] == 0x34 && cs_code[3] == 0x12) {
        if (cs_code_bytes <= 0x100) {
            REJECT("compute code area carries the 0x12345679 header but is too short for the "
                   "documented 0x100-byte prefix");
        }
        for (uint32_t i = 0x30; i < 0x100; i++) {
            if (cs_code[i]) {
                REJECT("compute code area 0x30..0x100 is not the documented zero padding "
                       "(magic=0x12345679): the instruction offset is unknown");
            }
        }
        cs_off = 0x100;
    } else if (cs_code_bytes >= 4 && cs_code[0] == 0x78 && cs_code[1] == 0x56 &&
               cs_code[2] == 0x34 && cs_code[3] == 0x12) {
        REJECT("compute source carries the graphics code-area header (0x12345678): "
               "a compute source must use 0x12345679 or no header");
    }
    cs_code += cs_off;
    cs_code_bytes -= (uint32_t)cs_off;
    if (!cs_code_bytes || (cs_code_bytes & 3u)) {
        REJECT("compute instruction stream is empty or not 4-byte aligned after the code-area prefix");
    }
#if defined(__ANDROID__)
    {
        static int logged;   // 每进程一行：真机核对计算源的头/指令偏移
        if (!logged) {
            logged = 1;
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                "compute cs source: bytes=%u head=%02x%02x%02x%02x ins_off=%d compiled_bytes=%u ctl=%u content_sha=%02x%02x%02x%02x",
                out->source.code_bytes, out->source.code[0], out->source.code[1],
                out->source.code[2], out->source.code[3], cs_off, cs_code_bytes,
                out->source.control_bytes, out->source.content_sha[0], out->source.content_sha[1],
                out->source.content_sha[2], out->source.content_sha[3]);
        }
    }
#endif
    // Same pinned control as the source identity. ShaderBake.ComputeStateOf documents
    // +0x758 as the raw local-memory byte count; unlike offline heuristics, no fallback.
    out->local_memory_bytes = le32(out->source.control + 0x758);
    if (out->local_memory_bytes > 0x100000u ||
        (s->local_memory_seen && s->local_memory_bytes != out->local_memory_bytes))
        { REJECT("invalid or conflicting CS local-memory fact"); }
    struct s2a_compile_graphics_src neutral = {0};
    struct s2a_input_identity id = {0};
    id.direct_abi = (int)c->direct_abi;
    id.translator_closure = c->translator_closure;
    hash(c->profile_json, c->profile_bytes, id.profile_sha);
    id.stage_mask = 32;
    id.compute_has_unaligned_ssbo = has_unaligned_ssbo;
    id.compute_unaligned_ssbo = unaligned_ssbo;
    memcpy(id.keys[0], out->source.key, 8);
    id.source_present = 1;
    memcpy(id.source_sha[0], out->source.content_sha, 32);
    id.source_code_bytes[0] = out->source.code_bytes;
    id.source_control_bytes[0] = out->source.control_bytes;
    id.ubo_use[5] = s->ubo_use[5];
    id.texture_array_length = s->tex_pool_n;
    id.sampler_array_length = s->samp_pool_n;
    // The S2AI1 fixed graphics segment is neutral for compute, not inherited draw state.
    id.gfx = &neutral;
    id.textures = textures->items;
    id.texture_count = textures->count;
    if (s2a_input_identity_encode(&id, out->canonical, sizeof out->canonical, &out->canonical_bytes))
        { REJECT("CS canonical identity rejected"); }
    hash(out->canonical, out->canonical_bytes, out->identity);
    out->session = c->session;
    out->generation = c->generation;
    struct sb b = {0};
    sb_puts(&b, "{"); sb_key_string(&b, "schema", "s2a-direct-compile-v1");
    sb_puts(&b, ","); sb_key_u64(&b, "session", c->session);
    sb_puts(&b, ","); sb_key_u64(&b, "generation", c->generation);
    sb_puts(&b, ","); sb_key_string(&b, "requestId", request_id);
    sb_puts(&b, ","); sb_key_u32(&b, "directABI", c->direct_abi);
    sb_puts(&b, ","); sb_key_string(&b, "translatorClosure", c->translator_closure);
    sb_puts(&b, ","); sb_key_string(&b, "runtimeCompilerABI", "s2a-direct-compile-v1");
    sb_puts(&b, ",\"sourceParserVersion\":1,\"profile\":");
    sb_raw(&b, c->profile_json, c->profile_bytes);
    sb_puts(&b, ",\"program\":{\"kind\":\"compute\",\"stages\":[{\"stage\":\"cs\",\"code\":");
    sb_base64(&b, cs_code, cs_code_bytes);
    sb_puts(&b, ",\"control\":"); sb_base64(&b, out->source.control, out->source.control_bytes);
    sb_puts(&b, ",\"c1\":"); sb_base64(&b, out->source.c1, out->source.c1_bytes);
    sb_puts(&b, ","); sb_key_u32(&b, "constantBufferUse", s->ubo_use[5]);
    sb_puts(&b, "}],\"computeState\":{\"localSize\":[");
    for (int i = 0; i < 3; i++) { if (i) sb_puts(&b, ","); sb_u32(&b, out->cs.local[i]); }
    sb_puts(&b, "],"); sb_key_u32(&b, "sharedMemoryBytes", out->cs.shared_bytes);
    sb_puts(&b, ","); sb_key_u32(&b, "localMemoryBytes", out->local_memory_bytes);
    sb_puts(&b, ",\"hasUnalignedStorageBuffer\":"); sb_puts(&b, unaligned_ssbo ? "true" : "false");
    sb_puts(&b, "},\"textures\":[");
    for (int i = 0; i < textures->count; i++) {
        const struct s2a_compile_texture_src *t = &textures->items[i];
        if (i) sb_puts(&b, ",");
        sb_puts(&b, "{"); sb_key_string(&b, "stage", "cs");
        sb_puts(&b, ","); sb_key_u32(&b, "handle", t->handle);
        sb_puts(&b, ","); sb_key_i32(&b, "cbufSlot", t->cbuf_slot);
        sb_puts(&b, ","); sb_key_string(&b, "samplerType", t->sampler_type);
        sb_puts(&b, ","); sb_key_string(&b, "format", t->format);
        sb_puts(&b, ",\"isBuffer\":"); sb_puts(&b, t->is_buffer ? "true" : "false");
        sb_puts(&b, ",\"normalized\":"); sb_puts(&b, t->normalized ? "true" : "false");
        sb_puts(&b, ",\"separate\":"); sb_puts(&b, t->separate ? "true" : "false");
        sb_puts(&b, ","); sb_key_u32(&b, "arrayLength", t->array_length);
        sb_puts(&b, "}");
    }
    sb_puts(&b, "],"); sb_key_u32(&b, "textureArrayLength", s->tex_pool_n);
    sb_puts(&b, ","); sb_key_u32(&b, "samplerArrayLength", s->samp_pool_n);
    sb_puts(&b, ",\"transformFeedbackEnabled\":false}}");
    if (b.oom) { free(b.p); REJECT("compute REQUEST allocation limit"); }
    out->json = b.p;
    out->json_bytes = b.n;
    return 0;
#undef REJECT
}
