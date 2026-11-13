// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_vk_program_result.h"
#include "../direct/jv.h"   // #1434 共用跨度 JSON（原内嵌实现）
#include "../direct/spirv_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *why, size_t n, const char *text) {
    if (why && n) snprintf(why, n, "%s", text);
    return -1;
}
static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *text, size_t length, uint8_t **out, size_t *out_bytes) {
    while (length && text[length - 1] == '=') length--;
    if (!length || length % 4 == 1) return -1;
    uint8_t *bytes = malloc(length / 4 * 3 + 3);
    if (!bytes) return -1;
    size_t n = 0;
    uint32_t acc = 0, bits = 0;
    for (size_t i = 0; i < length; i++) {
        int v = b64_value(text[i]);
        if (v < 0) { free(bytes); return -1; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes[n++] = (uint8_t)(acc >> bits); }
    }
    *out = bytes;
    *out_bytes = n;
    return 0;
}

// ---- 结果 JSON 的跨度扫描器 ----
// 结果里的 SPIR-V base64 单条就有 7–15 KB；通用 s2a_json 解析器把每个字符串解码进 256 B，
// 会拒绝这种合法结果。这里只做跨度扫描（字符串不复制），长字符串按原样交给 base64 解码。
int nvn_vk_program_graphics_stage_index(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "vs")) return 0;
    if (!strcmp(name, "fs")) return 1;
    if (!strcmp(name, "gs")) return 2;
    if (!strcmp(name, "tcs")) return 3;
    if (!strcmp(name, "tes")) return 4;
    if (!strcmp(name, "cs")) return 5;
    return -1;
}

static int compute_unique_fields(struct s2a_jv object);

// Return 1 for a proven sampled Buffer, 0 for another proven sampled image,
// -1 when the set/binding and its type chain cannot be proved. Op/Dim values
// correspond to the repository's SPIR-V headers (spirv_cross/spirv.h).
static int texture_buffer_kind(const void *bytes, size_t size, uint32_t set, uint32_t binding) {
    const uint8_t *p = bytes;
    if (!p || size < 20 || (size & 3u) || s2a_spv_word(p) != 0x07230203u) return -1;
    uint32_t bound = s2a_spv_word(p + 12);
    if (!bound || bound > (1u << 20)) return -1;
    struct item { size_t pos; uint32_t set, binding; unsigned has_set:1, has_binding:1; };
    struct item *ids = calloc(bound, sizeof *ids);
    if (!ids) return -1;
    size_t words = size / 4;
    int result = -1, found = 0;
    for (size_t pos = 5; pos < words;) {
        uint32_t opword = s2a_spv_word(p + 4*pos), count = opword >> 16, op = opword & 65535u;
        if (!count || count > words - pos) goto done;
        uint32_t id = 0;
        if ((op == 25 && count >= 9) || (op == 27 && count >= 3) ||
            ((op == 28 || op == 29) && count >= 3) || (op == 32 && count >= 4))
            id = s2a_spv_word(p + 4*(pos + 1));
        else if (op == 59 && count >= 4) id = s2a_spv_word(p + 4*(pos + 2));
        else if (op == 71 && count >= 4) {
            id = s2a_spv_word(p + 4*(pos + 1));
            uint32_t dec = s2a_spv_word(p + 4*(pos + 2)), value = s2a_spv_word(p + 4*(pos + 3));
            if (!id || id >= bound) goto done;
            if (dec == 34) { if (ids[id].has_set) goto done; ids[id].has_set = 1; ids[id].set = value; }
            if (dec == 33) { if (ids[id].has_binding) goto done; ids[id].has_binding = 1; ids[id].binding = value; }
            id = 0;
        }
        if (id) { if (id >= bound || ids[id].pos) goto done; ids[id].pos = pos; }
        pos += count;
    }
    for (uint32_t v = 1; v < bound; v++) {
        if (!ids[v].has_set || !ids[v].has_binding || ids[v].set != set || ids[v].binding != binding) continue;
        if (++found != 1 || !ids[v].pos) goto done;
        size_t pos = ids[v].pos;
        if ((s2a_spv_word(p + 4*pos) & 65535u) != 59 || s2a_spv_word(p + 4*(pos + 3)) != 0) goto done;
        uint32_t id = s2a_spv_word(p + 4*(pos + 1));
        if (!id || id >= bound || !ids[id].pos) goto done;
        pos = ids[id].pos;
        if ((s2a_spv_word(p + 4*pos) & 65535u) != 32 || s2a_spv_word(p + 4*(pos + 2)) != 0) goto done;
        id = s2a_spv_word(p + 4*(pos + 3));
        for (uint32_t steps = 0; steps < bound; steps++) {
            if (!id || id >= bound || !ids[id].pos) goto done;
            pos = ids[id].pos;
            uint32_t op = s2a_spv_word(p + 4*pos) & 65535u;
            if (op == 27 || op == 28 || op == 29) { id = s2a_spv_word(p + 4*(pos + 2)); continue; }
            if (op != 25) goto done;
            uint32_t dim = s2a_spv_word(p + 4*(pos + 3));
            uint32_t arrayed = s2a_spv_word(p + 4*(pos + 5)), ms = s2a_spv_word(p + 4*(pos + 6));
            uint32_t sampled = s2a_spv_word(p + 4*(pos + 7));
            if (dim > 6 || arrayed > 1 || ms > 1 || sampled > 1) goto done;
            if (dim == 5 && (arrayed || ms)) goto done;
            result = dim == 5;
            break;
        }
    }
done:
    free(ids);
    return found == 1 ? result : -1;
}

static int texture_classify(struct s2a_jv binding, const struct fd_stage *stage,
                            struct fd_binding *out, char *why, size_t why_size) {
    struct s2a_jv source;
    char kind[32] = "";
    int has_source = !s2a_jv_obj_find(binding, "source", &source);
    if (has_source && s2a_jv_str(source, kind, sizeof kind)) return fail(why, why_size, "texture source invalid");
    if (has_source && strcmp(kind, "texture") && strcmp(kind, "buffer-texture"))
        return fail(why, why_size, "unknown texture source");
    int actual = stage && stage->present ? texture_buffer_kind(stage->spirv, stage->spirv_bytes, (uint32_t)out->set, (uint32_t)out->binding) : -1;
    if (actual < 0)
        return fail(why, why_size, "texture SPIR-V descriptor type unproven");
    if (has_source && !strcmp(kind, "buffer-texture") && actual != 1)
        return fail(why, why_size, "buffer-texture source disagrees with SPIR-V");
    if (has_source && !strcmp(kind, "texture") && actual == 1)
        return fail(why, why_size, "texture source disagrees with SPIR-V buffer");
    out->texel_buffer = actual == 1;
    if (out->texel_buffer && out->count != 1) return fail(why, why_size, "buffer texture arrays are unsupported");
    return 0;
}

static int parse_graphics(const uint8_t *bytes, size_t length, struct fd_program *out, int allow_vs_only, char *why, size_t why_size);
int nvn_vk_program_parse_graphics(const uint8_t *bytes, size_t length, struct fd_program *out, char *why, size_t why_size) {
    return parse_graphics(bytes, length, out, 0, why, why_size);
}
int nvn_vk_program_parse_graphics_stages(const uint8_t *bytes, size_t length, struct fd_program *out, int allow_vs_only, char *why, size_t why_size) {
    return parse_graphics(bytes, length, out, allow_vs_only, why, why_size);
}
static int parse_graphics(const uint8_t *bytes, size_t length, struct fd_program *out, int allow_vs_only, char *why, size_t why_size) {
    struct s2a_jv top = { (const char *)bytes, length };
    if (!s2a_jv_is_object(top)) return fail(why, why_size, "program result is not a JSON object");
    struct s2a_jv schema;
    if (s2a_jv_obj_find(top, "schema", &schema)) return fail(why, why_size, "result has no schema");
    char schema_text[32];
    if (compute_unique_fields(top) || s2a_jv_str(schema, schema_text, sizeof schema_text))
        return fail(why, why_size, "invalid result schema");
    int v2 = !strcmp(schema_text, "s2a-direct-program-v2");
    if (!v2 && strcmp(schema_text, "s2a-direct-program-v1"))
        return fail(why, why_size, "unknown result schema");
    struct s2a_jv content;
    if (s2a_jv_obj_find(top, "content", &content) || compute_unique_fields(content))
        return fail(why, why_size, "result content invalid or duplicate");
    if (v2) {
        struct s2a_jv contract; char text[40];
        if (s2a_jv_obj_find(content, "contract", &contract) || s2a_jv_str(contract, text, sizeof text) ||
            strcmp(text, "s2a-direct-program-content-v2")) return fail(why, why_size, "v2 content contract mismatch");
    }

    struct s2a_jv stages;
    if (s2a_jv_obj_find(content, "stages", &stages) || !s2a_jv_is_array(stages))
        return fail(why, why_size, "result has no stages array");
    int count = s2a_jv_arr_count(stages);
    if (count <= 0 || count > FD_MAX_STAGES) return fail(why, why_size, "result stage count outside 1..2");
    for (int i = 0; i < count; i++) {
        struct s2a_jv st;
        if (s2a_jv_arr_item(stages, i, &st) || compute_unique_fields(st)) return fail(why, why_size, "stage fields invalid or duplicate");
        struct s2a_jv name, spirv;
        char name_text[8];
        if (s2a_jv_obj_find(st, "stage", &name) || s2a_jv_str(name, name_text, sizeof name_text))
            return fail(why, why_size, "stage has no name");
        int idx = nvn_vk_program_graphics_stage_index(name_text);
        if (idx < 0) return fail(why, why_size, "stage name is not a supported NVN stage");
        if (out->stage[idx].present) return fail(why, why_size, "duplicate result stage");
        if (s2a_jv_obj_find(st, "spirv", &spirv) || !s2a_jv_is_string(spirv))
            return fail(why, why_size, "stage has no spirv");
        uint8_t *words = NULL; size_t word_bytes = 0;
        if (b64_decode(spirv.p + 1, spirv.n - 2, &words, &word_bytes))
            return fail(why, why_size, "spirv base64 is invalid");
        if (word_bytes < 20 || (word_bytes & 3u) || *(const uint32_t *)words != 0x07230203u) {
            free(words);
            return fail(why, why_size, "spirv module is invalid");
        }
        out->stage[idx].present = 1;
        out->stage[idx].spirv = words;
        out->stage[idx].spirv_bytes = word_bytes;
        struct s2a_jv mask;
        uint32_t mask_value = 0;
        if (!s2a_jv_obj_find(st, "attributeReadMask", &mask) && !s2a_jv_u32(mask, &mask_value))
            out->stage[idx].attr_mask = mask_value;
        // 片元着色器的输出映射（每个颜色目标 4 位、RGBA 各 1 位）：在阶段的 info 里；读不到 = 全 1（不屏蔽）
        out->stage[idx].fragment_output_map = 0xffffffffu;
        { struct s2a_jv info = { NULL, 0 }, om = { NULL, 0 }; uint32_t om_v = 0;
          if (!s2a_jv_obj_find(st, "info", &info) && s2a_jv_is_object(info) && !s2a_jv_obj_find(info, "fragmentOutputMap", &om) && !s2a_jv_u32(om, &om_v))
              out->stage[idx].fragment_output_map = om_v; }
    }

    struct s2a_jv bindings;
    if (!s2a_jv_obj_find(content, "bindings", &bindings) && s2a_jv_is_array(bindings)) {
        int n = s2a_jv_arr_count(bindings);
        for (int i = 0; i < n; i++) {
            struct s2a_jv b;
            if (s2a_jv_arr_item(bindings, i, &b) || compute_unique_fields(b)) return fail(why, why_size, "binding fields invalid or duplicate");
            // 所有跨度先初始化：查找失败时绝不读未初始化的 p/n（旧代码对 "slot" 忽略了 s2a_jv_obj_find
            // 的结果，没 slot 的 support binding 会拿栈垃圾当跨度，是可疑的 SIGSEGV 来源）。
            struct s2a_jv kind = { NULL, 0 }, stage = { NULL, 0 }, set = { NULL, 0 }, binding = { NULL, 0 },
                      slot = { NULL, 0 };
            char kind_text[16], stage_text[8];
            if (s2a_jv_obj_find(b, "kind", &kind) || s2a_jv_str(kind, kind_text, sizeof kind_text)) return fail(why, why_size, "binding kind missing");
            uint32_t set_v = 0, binding_v = 0, slot_v = 0;
            if (s2a_jv_obj_find(b, "set", &set) || s2a_jv_u32(set, &set_v) || set_v > 3) return fail(why, why_size, "binding set outside 0..3");
            if (s2a_jv_obj_find(b, "binding", &binding) || s2a_jv_u32(binding, &binding_v) || binding_v > UINT16_MAX)
                return fail(why, why_size, "binding without binding");
            // slot 可以合法省略（support/storage 等没有 guest 槽位）；省略是“没有”，不是槽 0。
            // 写了但不是无符号整数则明确拒绝，不静默当省略。
            int has_slot = 0;
            if (!s2a_jv_obj_find(b, "slot", &slot)) {
                if (s2a_jv_u32(slot, &slot_v) || slot_v > INT32_MAX) return fail(why, why_size, "binding slot is not an unsigned integer");
                has_slot = 1;
            }
            if (out->binding_count >= FD_MAX_BINDINGS) {
                // F1：超出容量**准确失败**，绝不静默截断。旧写法把循环条件写成
                // `i < n && binding_count < FD_MAX_BINDINGS`：第 17 条起不解析、不报错，画面上只表现为
                // 少绑一个资源（错阴影 / 缺贴图），没有红行，是这类问题里最难查的一种。
                // 守卫必须放在取 dst 之前——`dst = &out->bindings[count]` 在 count==cap 时已经越界，
                // 紧随其后的 memset 会改写结构体里 binding_count 本身（复现过：17 条 → count 变 1）。
                // FD_MAX_BINDINGS 现在是 producer 合同上限（support 1 + vs/fs/gs 各 18+16+64+16 = 343）：
                // 越过它就是结果损坏/合同被违反，不是“再把数组调大”能掩盖的事。
                char msg[224];
                snprintf(msg, sizeof msg,
                         "program declares %d bindings, cap is %d: refusing to drop bindings "
                         "(cap is the contract maximum: support 1 + 3x(18+16+64+16); more means a corrupt result)",
                         n, FD_MAX_BINDINGS);
                return fail(why, why_size, msg);
            }

            struct fd_binding *dst = &out->bindings[out->binding_count];
            memset(dst, 0, sizeof *dst);
            dst->set = (int)set_v; dst->binding = (int)binding_v;
            dst->slot = has_slot ? (int)slot_v : -1;   // -1：反射没有 guest 槽位
            if (!strcmp(kind_text, "support") || !strcmp(kind_text, "uniform")) dst->kind = 0;
            else if (!strcmp(kind_text, "storage")) dst->kind = 1;
            else if (!strcmp(kind_text, "texture")) dst->kind = 2;
            else if (!strcmp(kind_text, "image")) dst->kind = 3;
            else return fail(why, why_size, "unknown binding kind");
            for (int j = 0; j < out->binding_count; ++j)
                if (out->bindings[j].set == dst->set && out->bindings[j].binding == dst->binding)
                    return fail(why, why_size, "duplicate descriptor binding");
            if (!s2a_jv_obj_find(b, "stage", &stage) && !s2a_jv_str(stage, stage_text, sizeof stage_text))
                snprintf(dst->stage, sizeof dst->stage, "%s", stage_text);
            dst->count = 1;
            { struct s2a_jv cnt = { NULL, 0 }; uint32_t cnt_v = 0;
              if (!s2a_jv_obj_find(b, "count", &cnt)) {
                  if (s2a_jv_u32(cnt, &cnt_v) || !cnt_v) return fail(why, why_size, "binding count is not a positive unsigned integer");
                  if (cnt_v > FD_MAX_ARRAY_ELEMENTS) {
                      if (why && why_size) snprintf(why, why_size, "stage %s descriptor array length=%u exceeds %u", dst->stage[0] ? dst->stage : "unknown", cnt_v, FD_MAX_ARRAY_ELEMENTS);
                      return NVK_PROGRAM_ARRAY_UNSUPPORTED;
                  }
                  dst->count = (uint16_t)cnt_v;
              } }
            // 一次算好逐笔要用的派生值（原来每笔都要重做 strcmp 和槽位换算）。
            dst->stage_idx = (int8_t)nvn_vk_program_graphics_stage_index(dst->stage);
            dst->is_support = (uint8_t)(dst->kind == 0 && !strcmp(dst->stage, "program"));
            dst->ubo_slot = (int16_t)(dst->slot >= 3 ? dst->slot - 3 : 0);
            dst->tex_slot = -1; dst->samp_slot = -1;
            struct s2a_jv samp = { NULL, 0 }; uint32_t samp_v = 0;
            int has_samp = !s2a_jv_obj_find(b, "samp_slot", &samp);
            if (has_samp) {
                if (!v2 || dst->kind != 2 || s2a_jv_u32(samp, &samp_v) || samp_v >= 128 ||
                    dst->count != 1 || dst->slot < 90 || dst->slot >= 346 || (dst->slot & 1))
                    return fail(why, why_size, "invalid separate sampler source");
                dst->tex_slot = (int16_t)(128 + (dst->slot - 90) / 2);
                dst->samp_slot = (int16_t)samp_v;
            } else if (dst->kind == 2 && dst->slot >= 8 && dst->slot < 72 && !(dst->slot & 1)) {
                dst->tex_slot = (int16_t)((dst->slot - 8) / 2);
            } else if (v2 && dst->kind == 2 && !(dst->count > 1 && dst->slot == 0)) {
                return fail(why, why_size, "invalid texture source");
            }
            if (dst->kind == 2 && texture_classify(b,
                    dst->stage_idx >= 0 && dst->stage_idx < FD_MAX_STAGES ? &out->stage[dst->stage_idx] : NULL,
                    dst, why, why_size)) return -1;
            if (dst->kind == 1) {
                // Storage buffer address provenance. These fields are optional for
                // older results; a missing mapping stays explicit and the runtime
                // reports the unsupported combination instead of guessing a slot.
                struct s2a_jv f = { NULL, 0 };
                uint32_t cb_slot = 0, cb_off = 0, nvn = 0, ubo_ptr = 0, ubo_slot = 0, ubo_off = 0;
                int has_cb_slot = 0, has_cb_off = 0, has_nvn = 0, has_ubo_ptr = 0;
                int has_ubo_slot = 0, has_ubo_off = 0, rw = 0, has_rw = 0;
                if (!s2a_jv_obj_find(b, "cbSlot", &f)) {
                    if (s2a_jv_u32(f, &cb_slot)) return fail(why, why_size, "storage cbSlot is not an unsigned integer");
                    has_cb_slot = 1;
                }
                if (!s2a_jv_obj_find(b, "cbOffset", &f)) {
                    if (s2a_jv_u32(f, &cb_off)) return fail(why, why_size, "storage cbOffset is not an unsigned integer");
                    has_cb_off = 1;
                }
                if (!s2a_jv_obj_find(b, "nvnSlot", &f)) {
                    if (s2a_jv_u32(f, &nvn) || nvn >= 16) return fail(why, why_size, "storage nvnSlot is outside 0..15");
                    has_nvn = 1;
                }
                if (!s2a_jv_obj_find(b, "uboPtr", &f)) {
                    if (s2a_jv_u32(f, &ubo_ptr)) return fail(why, why_size, "storage uboPtr is not an unsigned integer");
                    has_ubo_ptr = 1;
                }
                if (!s2a_jv_obj_find(b, "uboSlot", &f)) {
                    if (s2a_jv_u32(f, &ubo_slot)) return fail(why, why_size, "storage uboSlot is not an unsigned integer");
                    has_ubo_slot = 1;
                }
                if (!s2a_jv_obj_find(b, "uboOffset", &f)) {
                    if (s2a_jv_u32(f, &ubo_off)) return fail(why, why_size, "storage uboOffset is not an unsigned integer");
                    has_ubo_off = 1;
                }
                if (!s2a_jv_obj_find(b, "rw", &f)) {
                    if (s2a_jv_bool(f, &rw)) return fail(why, why_size, "storage rw is not a boolean");
                    has_rw = 1;
                }
                if (!has_rw) return fail(why, why_size, "storage rw fact is missing");
                dst->storage_cb_slot = has_cb_slot ? (int32_t)cb_slot : -1;
                dst->storage_cb_offset = has_cb_off ? (int32_t)cb_off : -1;
                dst->storage_writable = (uint8_t)rw;
                if (has_nvn) {
                    dst->storage_kind = 1;
                    dst->storage_slot = (int8_t)nvn;
                } else if (has_ubo_slot) {
                    if (ubo_slot >= 16 || !has_ubo_off)
                        return fail(why, why_size, "storage UBO slot/offset mapping is incomplete");
                    dst->storage_kind = 2;
                    dst->storage_slot = (int8_t)ubo_slot;
                    dst->storage_cb_offset = (int32_t)ubo_off;
                } else if (has_ubo_ptr) {
                    dst->storage_kind = 3;
                    dst->storage_slot = ubo_ptr < 16 ? (int8_t)ubo_ptr : -1;
                }
            }
            out->binding_count++;
        }
    }

    out->support_bytes = SUPPORT_DEFAULT_BYTES;
    struct s2a_jv support;
    if (!s2a_jv_obj_find(content, "supportLayout", &support) && s2a_jv_is_object(support)) {
        struct s2a_jv required;
        uint32_t required_v = 0;
        if (!s2a_jv_obj_find(support, "requiredSize", &required) && !s2a_jv_u32(required, &required_v) && required_v)
            out->support_bytes = required_v;
    }
    if (out->support_bytes < SUPPORT_DEFAULT_BYTES) return fail(why, why_size, "support layout is smaller than the compiler layout");
    if (!out->stage[0].present) return fail(why, why_size, "program has no vertex stage");
    if (!out->stage[1].present && !allow_vs_only) return fail(why, why_size, "program is not a vs+fs program");
    return 0;
}

void nvn_vk_program_free(struct fd_program *p) {
    for (int i = 0; i < FD_MAX_STAGES; i++) { free(p->stage[i].spirv); p->stage[i].spirv = NULL; }
}

/* The graphics reader above is deliberately unchanged. Compute validates the
 * complete JSON grammar with the shared reader first; only long SPIR-V values
 * are then consumed as spans, avoiding the short-string field limit. */
#include "direct/json.h"
#include <limits.h>

static int compute_unique_fields(struct s2a_jv object) {
    if (!s2a_jv_is_object(object)) return -1;
    char keys[S2A_JSON_MAX_FIELDS][64];
    int count = 0;
    const char *p = object.p + 1, *end = object.p + object.n - 1;
    while ((p = s2a_jv_ws(p, end)) < end) {
        const char *key_end, *value_end;
        if (count == S2A_JSON_MAX_FIELDS || s2a_jv_scan(p, end, &key_end)) return -1;
        struct s2a_jv key = {p, (size_t)(key_end - p)};
        if (s2a_jv_str(key, keys[count], sizeof keys[count])) return -1;
        /* Escaped field names are not emitted by the compiler. Reject rather
         * than compare a partially decoded Unicode spelling as another key. */
        if (memchr(key.p, '\\', key.n)) return -1;
        for (int i = 0; i < count; ++i) if (!strcmp(keys[i], keys[count])) return -1;
        count++;
        p = s2a_jv_ws(key_end, end);
        if (p >= end || *p != ':' || s2a_jv_scan(p + 1, end, &value_end)) return -1;
        p = s2a_jv_ws(value_end, end);
        if (p < end && *p == ',') p++;
    }
    return 0;
}
static int compute_u32(struct s2a_jv v, uint32_t *out) {
    if (!v.n || (v.n > 1 && v.p[0] == '0')) return -1;
    uint64_t value = 0;
    for (size_t i = 0; i < v.n; ++i) {
        if (v.p[i] < '0' || v.p[i] > '9') return -1;
        value = value * 10u + (unsigned)(v.p[i] - '0');
        if (value > UINT32_MAX) return -1;
    }
    *out = (uint32_t)value;
    return 0;
}
static int compute_string_is(struct s2a_jv object, const char *key, const char *expected) {
    struct s2a_jv v;
    char text[64];
    return !s2a_jv_obj_find(object, key, &v) && !s2a_jv_str(v, text, sizeof text) && !strcmp(text, expected);
}
static int compute_spirv(struct s2a_jv v, struct fd_stage *stage) {
    if (!s2a_jv_is_string(v) || v.n < 6) return -1;
    const char *p = v.p + 1;
    size_t n = v.n - 2, pad = 0;
    if (n % 4) return -1;
    while (pad < n && p[n - pad - 1] == '=') ++pad;
    if (pad > 2 || n == pad) return -1;
    for (size_t i = 0; i < n - pad; ++i) if (b64_value(p[i]) < 0) return -1;
    int last = b64_value(p[n - pad - 1]);
    if ((pad == 1 && (last & 3)) || (pad == 2 && (last & 15))) return -1;
    if (b64_decode(p, n, &stage->spirv, &stage->spirv_bytes)) return -1;
    if (stage->spirv_bytes < 20 || (stage->spirv_bytes & 3u)) return -1;
    uint32_t magic;
    memcpy(&magic, stage->spirv, sizeof magic);
    if (magic != 0x07230203u) return -1;
    stage->present = 1;
    return 0;
}
void nvn_vk_program_free_compute(struct nvn_vk_compute_program *p) {
    if (!p) return;
    free(p->stage.spirv);
    memset(p, 0, sizeof *p);
}
/* Compute-only source mapping. Do not interpret translator allocation order as an NVN slot. */
static int compute_storage(struct s2a_jv b, struct nvn_vk_compute_storage *s) {
    struct s2a_jv v;
    uint32_t nvn = 0, ptr = 0, ubo = 0, off = 0;
    if (s2a_jv_obj_find(b, "rw", &v)) return -1;
    if (v.n == 4 && !memcmp(v.p, "true", 4)) s->writable = 1;
    else if (v.n != 5 || memcmp(v.p, "false", 5)) return -1;
    if (s2a_jv_obj_find(b, "cbSlot", &v) || compute_u32(v, &s->cb_slot) || s->cb_slot > 255 ||
        s2a_jv_obj_find(b, "cbOffset", &v) || compute_u32(v, &s->cb_offset) || s->cb_offset > 65535u * 4u)
        return -1;
    int hn = !s2a_jv_obj_find(b, "nvnSlot", &v); if (hn && compute_u32(v, &nvn)) return -1;
    int hp = !s2a_jv_obj_find(b, "uboPtr", &v); if (hp && compute_u32(v, &ptr)) return -1;
    int hu = !s2a_jv_obj_find(b, "uboSlot", &v); if (hu && compute_u32(v, &ubo)) return -1;
    int ho = !s2a_jv_obj_find(b, "uboOffset", &v); if (ho && compute_u32(v, &off)) return -1;
    if (hn + hp + hu != 1 || hu != ho || (s->cb_offset & 3u)) return -1;
    if (hn && s->cb_slot == 0 && s->cb_offset >= 0x310 && s->cb_offset < 0x410 &&
        !(s->cb_offset & 15u) && nvn == (s->cb_offset - 0x310) / 16u) {
        s->mapping = NVK_STORAGE_NVN; s->index = nvn; return 0;
    }
    if (hp && s->cb_slot == 0 && s->cb_offset >= 0x210 && s->cb_offset < 0x310 &&
        !(s->cb_offset & 15u) && ptr == (s->cb_offset - 0x210) / 16u) {
        s->mapping = NVK_STORAGE_UBO_PTR; s->index = ptr; return 0;
    }
    if (hu && s->cb_slot >= 3 && ubo == s->cb_slot - 3 && off == s->cb_offset && ubo < 16) {
        s->mapping = NVK_STORAGE_UBO_ADDRESS; s->index = ubo; s->offset = off; return 0;
    }
    return -1;
}
int nvn_vk_program_parse_compute(const uint8_t *bytes, size_t length,
    struct nvn_vk_compute_program *out, char *why, size_t why_size) {
    if (!bytes || !out) return fail(why, why_size, "compute result arguments are missing");
    memset(out, 0, sizeof *out);
    s2a_json_object *syntax = malloc(sizeof *syntax);
    if (!syntax) return fail(why, why_size, "compute JSON allocation failed");
    int rc = s2a_json_parse_object(bytes, length, syntax, why, why_size);
    free(syntax);
    if (rc) return -1;
    struct s2a_jv top = {(const char *)bytes, length}, content, stages, st, spirv, bindings, support;
#define CS_FAIL(text) do { nvn_vk_program_free_compute(out); return fail(why, why_size, text); } while (0)
    int v2 = compute_string_is(top, "schema", "s2a-direct-program-v2");
    if (!v2 && !compute_string_is(top, "schema", "s2a-direct-program-v1")) CS_FAIL("compute result schema mismatch");
    if (s2a_jv_obj_find(top, "content", &content) || compute_unique_fields(content)) CS_FAIL("compute content fields invalid");
    if (!compute_string_is(content, "contract", v2 ? "s2a-direct-program-content-v2" : "s2a-direct-program-content-v1") ||
        !compute_string_is(content, "kind", "compute")) CS_FAIL("result is not compute content");
    struct s2a_jv alignment;
    if (s2a_jv_obj_find(content, "hasUnalignedStorageBuffer", &alignment)) CS_FAIL("compute storage alignment fact missing");
    if (alignment.n == 4 && !memcmp(alignment.p, "true", 4)) out->has_unaligned_storage_buffer = 1;
    else if (alignment.n != 5 || memcmp(alignment.p, "false", 5)) CS_FAIL("compute storage alignment fact must be boolean");
    struct s2a_jv reads;
    if (s2a_jv_obj_find(content, "c0Reads", &reads)) CS_FAIL("compute c0 reads are missing");
    int read_count;
    if (s2a_jv_is_null(reads)) {
        // 分析器给不出读集：运行时按完整驱动 c0 布局填（dispatch 尺寸 + UBO 表 + storage 表），
        // 这些表的值全都来自本次 dispatch 的真实状态，不需要“猜”。
        out->c0_reads_unknown = 1;
        read_count = 0;
    } else {
        read_count = s2a_jv_arr_count(reads);
        if (read_count < 0 || read_count > NVK_COMPUTE_C0_MAX_READS)
            CS_FAIL("compute c0 reads are unknown or excessive");
    }
    for (int i = 0; i < read_count; ++i) {
        struct s2a_jv pair, off, size;
        struct nvn_vk_c0_read *r = &out->c0_reads[i];
        if (s2a_jv_arr_item(reads, i, &pair) || s2a_jv_arr_count(pair) != 2 ||
            s2a_jv_arr_item(pair, 0, &off) || s2a_jv_arr_item(pair, 1, &size) ||
            compute_u32(off, &r->offset) || compute_u32(size, &r->size) || !r->size)
            CS_FAIL("compute c0 read range is invalid");
        uint32_t begin = 0;
        if (r->offset >= 0x200 && r->offset < 0x20c) begin = 0x200;
        else if (r->offset >= 0x210 && r->offset < 0x410) begin = r->offset & ~15u;
        if (!begin || r->offset - begin >= 12 || r->size > 12 - (r->offset - begin)) {
            char message[128];
            snprintf(message, sizeof message, "compute c0 read unsupported: offset=0x%x size=%u", r->offset, r->size);
            CS_FAIL(message);
        }
    }
    out->c0_read_count = (uint32_t)read_count;
    if (s2a_jv_obj_find(content, "stages", &stages) || s2a_jv_arr_count(stages) != 1 ||
        s2a_jv_arr_item(stages, 0, &st) || compute_unique_fields(st) ||
        !compute_string_is(st, "stage", "cs")) CS_FAIL("compute requires exactly one CS stage");
    if (s2a_jv_obj_find(st, "spirv", &spirv) || compute_spirv(spirv, &out->stage)) CS_FAIL("compute SPIR-V is invalid");
    struct s2a_jv mask;
    if (!s2a_jv_obj_find(st, "attributeReadMask", &mask) &&
        (compute_u32(mask, &out->stage.attr_mask) || out->stage.attr_mask)) CS_FAIL("compute vertex attribute mask is invalid");
    if (s2a_jv_obj_find(content, "bindings", &bindings)) CS_FAIL("compute bindings are missing");
    int count = s2a_jv_arr_count(bindings);
    if (count < 0 || count > FD_MAX_BINDINGS) CS_FAIL("compute binding count exceeds limit");
    for (int i = 0; i < count; ++i) {
        struct s2a_jv b, value;
        if (s2a_jv_arr_item(bindings, i, &b) || compute_unique_fields(b)) CS_FAIL("compute binding fields invalid");
        struct fd_binding *dst = &out->bindings[i];
        uint32_t u;
        if (s2a_jv_obj_find(b, "set", &value) || compute_u32(value, &u) || u > 3) CS_FAIL("compute descriptor set invalid");
        dst->set = (int)u;
        if (s2a_jv_obj_find(b, "binding", &value) || compute_u32(value, &u) || u > INT_MAX) CS_FAIL("compute binding number invalid");
        dst->binding = (int)u;
        if (s2a_jv_obj_find(b, "count", &value) || compute_u32(value, &u) || !u) CS_FAIL("compute binding count is invalid");
        if (u > FD_MAX_ARRAY_ELEMENTS) {
            char message[128]; snprintf(message, sizeof message, "stage cs descriptor array length=%u exceeds %u", u, FD_MAX_ARRAY_ELEMENTS);
            nvn_vk_program_free_compute(out);
            fail(why, why_size, message);
            return NVK_PROGRAM_ARRAY_UNSUPPORTED;
        }
        if (u != 1) CS_FAIL("compute descriptor arrays are unsupported");
        dst->count = 1;
        for (int j = 0; j < i; ++j)
            if (out->bindings[j].set == dst->set && out->bindings[j].binding == dst->binding) CS_FAIL("duplicate compute binding");
        dst->is_support = (uint8_t)compute_string_is(b, "kind", "support");
        if (dst->is_support || compute_string_is(b, "kind", "uniform")) dst->kind = 0;
        else if (compute_string_is(b, "kind", "storage")) dst->kind = 1;
        else if (compute_string_is(b, "kind", "texture")) dst->kind = 2;
        else if (compute_string_is(b, "kind", "image")) dst->kind = 3;
        else CS_FAIL("unknown compute binding kind");
        // texture (kind 2) 是 compute 的一等资源：slot 就是 NVN c2 组合纹理表里的 handleIndex，
        // 下面按 8+2*slot 校验并换算出 NVN 纹理槽。存储图像（kind 3）还没接：显式拒绝并把
        // set/binding/handleIndex 写进原因（不绑定占位图像、不静默丢一条描述符）。
        // 只有 storage 描述符带 guest 地址来源字段；别的种类夹带这些字段就是拼错的绑定，
        // 不能“忽略多余字段”放行（否则编译期的地址映射和运行期绑定的对象会不是同一个）。
        if (dst->kind != 1) {
            static const char *const storage_only[] = {
                "rw", "cbSlot", "cbOffset", "nvnSlot", "uboPtr", "uboSlot", "uboOffset" };
            for (unsigned k = 0; k < sizeof storage_only / sizeof storage_only[0]; ++k) {
                struct s2a_jv extra;
                if (!s2a_jv_obj_find(b, storage_only[k], &extra))
                    CS_FAIL("compute non-storage binding carries a storage mapping field");
            }
        }
        const char *stage = dst->is_support ? "program" : "cs";
        if (!compute_string_is(b, "stage", stage)) CS_FAIL("compute binding stage mismatch");
        snprintf(dst->stage, sizeof dst->stage, "%s", stage);
        dst->stage_idx = dst->is_support ? -1 : 5; /* NVN CS index, not identity texture enum. */
        dst->slot = -1;
        if (!s2a_jv_obj_find(b, "slot", &value)) {
            if (compute_u32(value, &u) || u > INT_MAX) CS_FAIL("compute guest slot invalid");
            dst->slot = (int)u;
        }
        if (!dst->is_support && dst->kind != 1 && dst->slot < 0) CS_FAIL("compute binding has no reflected guest slot");
        if (dst->kind == 3) {
            char message[160];
            snprintf(message, sizeof message,
                "compute descriptor unsupported: set=%d binding=%d kind=image handleIndex=%d",
                dst->set, dst->binding, dst->slot);
            CS_FAIL(message);
        }
        dst->ubo_slot = -1;
        dst->tex_slot = -1; dst->samp_slot = -1;
        struct s2a_jv samp;
        int has_samp = !s2a_jv_obj_find(b, "samp_slot", &samp);
        if (has_samp) {
            uint32_t sv;
            if (!v2 || dst->kind != 2 || compute_u32(samp, &sv) || sv >= 128 ||
                dst->slot < 90 || dst->slot >= 346 || (dst->slot & 1)) CS_FAIL("compute separate sampler source invalid");
            dst->samp_slot = (int16_t)sv;
        }
        if (dst->kind == 0 && !dst->is_support) {
            // NVN cbuf 语义（与图形侧 nvn_vk_first_draw.c 同一套）：0 = c0/support、1 = c1 常量段、
            // 2 = 纹理句柄表（还没实现）、3..18 = 用户 uniform（NVN 槽 = cbuf−3）。
            if (dst->slot == 2) {
                char message[160];
                snprintf(message, sizeof message,
                    "compute c2 texture-handle bank used as a uniform buffer is not implemented: set=%d binding=%d",
                    dst->set, dst->binding);
                CS_FAIL(message);
            }
            if (dst->slot != 0 && dst->slot != 1 && (dst->slot < 3 || dst->slot > 18)) {
                // 越界必须能定位：槽号 + set/binding。
                char message[160];
                snprintf(message, sizeof message,
                    "compute uniform slot %d outside NVN cbuf range (set=%d binding=%d)",
                    dst->slot, dst->set, dst->binding);
                CS_FAIL(message);
            }
            dst->ubo_slot = (int16_t)(dst->slot - 3);
        }
        if (dst->kind == 1 && (dst->slot != -1 || compute_storage(b, &out->storage[i])))
            CS_FAIL("compute storage source mapping is unknown or inconsistent");
        if (dst->kind == 2) {
            if (has_samp) dst->tex_slot = (int16_t)(128 + (dst->slot - 90) / 2);
            else {
                if (dst->slot < 8 || dst->slot >= 72 || (dst->slot & 1)) CS_FAIL("compute texture slot outside recorded range");
                dst->tex_slot = (int16_t)((dst->slot - 8) / 2);
            }
            if (texture_classify(b, &out->stage, dst, why, why_size)) {
                nvn_vk_program_free_compute(out);
                return -1;
            }
        }
        out->binding_count++;
    }
    if (s2a_jv_obj_find(content, "supportLayout", &support) || compute_unique_fields(support)) CS_FAIL("compute support layout is missing");
    struct s2a_jv required;
    if (s2a_jv_obj_find(support, "requiredSize", &required) || compute_u32(required, &out->support_bytes) ||
        !out->support_bytes) CS_FAIL("compute support layout is empty");
    if (s2a_jv_obj_find(content, "supportReads", &reads)) CS_FAIL("compute support reads missing");
    // null = 编译器静态分析给不出读集（SPIR-V 模式超出分析器能力）。这不是“没有读”，
    // 运行时按恒等值填满整个 SupportBuffer（所有 render scale = 1.0，flags 位为 0），
    // 并把这个事实记进程序对象；不是数组也不是 null 仍然拒绝。
    if (s2a_jv_is_null(reads)) {
        out->support_reads_unknown = 1;
        read_count = 0;
    } else {
        read_count = s2a_jv_arr_count(reads);
        if (read_count < 0 || read_count > NVK_COMPUTE_C0_MAX_READS)
            CS_FAIL("compute support reads unknown or excessive");
    }
    if (!read_count) return 0; /* proven no actual load: no layout fact is needed */
    struct s2a_jv scales;
    if (s2a_jv_obj_find(content, "supportScaleRanges", &scales) || s2a_jv_arr_count(scales) != 2)
        CS_FAIL("compute support scale ranges are missing or invalid");
    unsigned scale_seen = 0;
    for (int i = 0; i < 2; ++i) {
        struct s2a_jv item, v;
        if (s2a_jv_arr_item(scales, i, &item) || compute_unique_fields(item)) CS_FAIL("compute support scale fields invalid");
        int index = compute_string_is(item, "name", "render_scale") ? 0 :
                    compute_string_is(item, "name", "render_scale_y") ? 1 : -1;
        if (index < 0 || (scale_seen & (1u << index))) CS_FAIL("compute support scale name unknown or duplicate");
        scale_seen |= 1u << index;
        struct nvn_vk_support_scale *r = &out->support_scales[index];
        if (s2a_jv_obj_find(item, "offset", &v) || compute_u32(v, &r->offset) ||
            s2a_jv_obj_find(item, "stride", &v) || compute_u32(v, &r->stride) || r->stride < 4 ||
            s2a_jv_obj_find(item, "count", &v) || compute_u32(v, &r->count) || !r->count ||
            s2a_jv_obj_find(item, "elementBytes", &v) || compute_u32(v, &r->element_bytes) || r->element_bytes != 4 ||
            (r->offset & 3u) || (r->stride & 3u) ||
            (uint64_t)r->offset + (uint64_t)(r->count - 1) * r->stride + r->element_bytes > out->support_bytes)
            CS_FAIL("compute support scale extent invalid");
    }
    const struct nvn_vk_support_scale *a = &out->support_scales[0], *b = &out->support_scales[1];
    uint64_t a_end = (uint64_t)a->offset + (uint64_t)(a->count - 1) * a->stride + a->element_bytes;
    uint64_t b_end = (uint64_t)b->offset + (uint64_t)(b->count - 1) * b->stride + b->element_bytes;
    if (a->offset < b_end && b->offset < a_end) CS_FAIL("compute support scale ranges overlap");
    for (int i = 0; i < read_count; ++i) {
        struct s2a_jv pair, off, size;
        struct nvn_vk_c0_read *r = &out->support_reads[i];
        if (s2a_jv_arr_item(reads, i, &pair) || s2a_jv_arr_count(pair) != 2 || s2a_jv_arr_item(pair, 0, &off) || s2a_jv_arr_item(pair, 1, &size) ||
            compute_u32(off, &r->offset) || compute_u32(size, &r->size) || !r->size)
            CS_FAIL("compute support read range invalid");
        int known = 0;
        for (unsigned j = 0; j < 2; ++j) {
            const struct nvn_vk_support_scale *scale = &out->support_scales[j];
            if (r->offset < scale->offset) continue;
            uint32_t delta = r->offset - scale->offset;
            if (delta / scale->stride < scale->count && delta % scale->stride < scale->element_bytes &&
                r->size <= scale->element_bytes - delta % scale->stride) known = 1;
        }
        if (!known) {
            char message[128];
            snprintf(message, sizeof message, "compute support read unsupported: offset=0x%x size=%u", r->offset, r->size);
            CS_FAIL(message);
        }
    }
    out->support_read_count = (uint32_t)read_count;
    int support_bindings = 0;
    for (int i = 0; i < out->binding_count; ++i) support_bindings += out->bindings[i].is_support != 0;
    if (support_bindings > 1 || (read_count && support_bindings != 1)) CS_FAIL("compute support read binding missing or duplicate");
#undef CS_FAIL
    return 0;
}

// SPIR-V ExecutionModel per stage slot; slots 0/1 feed the vs/fs draw paths,
// slots 2-4 only feed the pre-raster storage scan below.
static const uint32_t FD_STAGE_MODEL[FD_MAX_STAGES] = { 0, 4, 3, 1, 2, 5 };
static const char *const FD_STAGE_LABEL[FD_MAX_STAGES] = {
    "vertex", "fragment", "geometry", "tessellation control", "tessellation evaluation", "compute" };
// Pre-raster stages covered by the writeStages 0x1d admission mask: vs, gs, tcs, tes.
static const int FD_SCAN_STAGES[4] = { 0, 2, 3, 4 };
int nvn_vk_program_vertex_storage_safe(const uint8_t *bytes, size_t length, char *why, size_t why_size) {
    struct fd_program p;
    memset(&p, 0, sizeof p);
    int rc = nvn_vk_program_parse_graphics(bytes, length, &p, why, why_size);
    if (!rc) {
        for (int s = 0; s < 4 && !rc; s++) {
            int idx = FD_SCAN_STAGES[s];
            if (!p.stage[idx].present) continue;
            int need = s2a_spirv_storage_need(p.stage[idx].spirv, p.stage[idx].spirv_bytes,
                                              FD_STAGE_MODEL[idx]);
            if (need != 0) {
                char text[160];
                snprintf(text, sizeof text, need < 0 ? "%s SPIR-V storage check invalid" :
                         "%s program requires vertexPipelineStoresAndAtomics", FD_STAGE_LABEL[idx]);
                rc = fail(why, why_size, text);
            }
        }
    }
    nvn_vk_program_free(&p);
    return rc;
}
