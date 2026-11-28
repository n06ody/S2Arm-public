// #1403：图形 storage buffer 描述符的编译结果 provenance 宿主回归。
//
// 这里原样链接生产 parser（nvn_vk_program_result.c），只验证：
//   * 编译结果的 nvnSlot / uboSlot+uboOffset / uboPtr / rw 被读成明确的运行时来源；
//   * 越界 nvnSlot、非布尔 rw 准确拒绝；
//   * 没有映射的 storage binding 保持 kind=0，运行时必须报红而不是猜槽位。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvn/nvn_vk_program_result.h"
#include "nvn/nvk_result_view.h"
#include "tbuf_spirv_fixtures.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static const char *k_spirv = "AwIjBwABAwAAAAAAAQAAAAAAAAA=";

static int parse_json(const char *bindings, struct fd_program *p, char *why, size_t cap) {
    char json[4096];
    int texture = strstr(bindings, "\"kind\":\"texture\"") != NULL;
    const char *vs = texture ? (strstr(bindings, "\"count\":64") ? S2A_TEST_VERT_64 : S2A_TEST_VERT_1) : k_spirv;
    const char *fs = texture ? S2A_TEST_FRAG_1 : k_spirv;
    int n = snprintf(json, sizeof json,
        "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
        "\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"},{\"stage\":\"fs\",\"spirv\":\"%s\"}],"
        "\"bindings\":[%s]}}", vs, fs, bindings);
    if (n <= 0 || (size_t)n >= sizeof json) return -2;
    memset(p, 0, sizeof *p);
    return nvn_vk_program_parse_graphics((const uint8_t *)json, (size_t)n, p, why, cap);
}

static int parse_v2(const char *bindings, struct fd_program *p, char *why, size_t cap) {
    char json[4096];
    int texture = strstr(bindings, "\"kind\":\"texture\"") != NULL;
    int n = snprintf(json, sizeof json,
        "{\"schema\":\"s2a-direct-program-v2\",\"content\":{\"contract\":\"s2a-direct-program-content-v2\","
        "\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"},{\"stage\":\"fs\",\"spirv\":\"%s\"}],"
        "\"bindings\":[%s]}}", texture ? S2A_TEST_VERT_1 : k_spirv, texture ? S2A_TEST_FRAG_1 : k_spirv, bindings);
    if (n <= 0 || (size_t)n >= sizeof json) return -2;
    memset(p, 0, sizeof *p);
    return nvn_vk_program_parse_graphics((const uint8_t *)json, (size_t)n, p, why, cap);
}

// ---- 两段接口核对（nvn_vk_spirv_link_ok）用的最小 SPIR-V：只含核对要读的指令，不是可装载的模块 ----
struct spv { uint32_t w[256]; size_t n; };
static void spv_begin(struct spv *m) { m->n = 0; uint32_t h[5] = {0x07230203u, 0x00010300u, 0, 64, 0}; for (int i = 0; i < 5; i++) m->w[m->n++] = h[i]; }
static void spv_op(struct spv *m, uint32_t op, int nargs, const uint32_t *a) { m->w[m->n++] = ((uint32_t)(nargs + 1) << 16) | op; for (int i = 0; i < nargs; i++) m->w[m->n++] = a[i]; }
// 一个用户接口变量：Location loc、存储类 storage（1 Input / 3 Output）。id 由调用方给，类型指针 %2（指向标量 / 向量 %1）
static void spv_var(struct spv *m, uint32_t id, uint32_t storage, int loc) {
    if (loc >= 0) { uint32_t d[3] = {id, 30, (uint32_t)loc}; spv_op(m, 71, 3, d); }
    uint32_t v[3] = {storage == 3 ? 2u : 3u, id, storage}; spv_op(m, 59, 3, v);
}
static void spv_types(struct spv *m) {   // %2 = 指向 %1 的 Output 指针，%3 = 指向 %1 的 Input 指针
    uint32_t p2[3] = {2, 3, 1}, p3[3] = {3, 1, 1}; spv_op(m, 32, 3, p2); spv_op(m, 32, 3, p3);
}
static void spv_perv(struct spv *m, uint32_t storage) {   // gl_PerVertex：结构体 %20 成员 0 带 BuiltIn Position，变量 %22 不带 Location
    uint32_t md[4] = {20, 0, 11, 0}; spv_op(m, 72, 4, md);
    uint32_t st[2] = {20, 1}; spv_op(m, 30, 2, st);
    uint32_t pt[3] = {21, storage, 20}; spv_op(m, 32, 3, pt);
    uint32_t v[3] = {21, 22, storage}; spv_op(m, 59, 3, v);
}
static int link(const struct spv *pre, const struct spv *fs, uint64_t *miss) {
    return nvn_vk_spirv_link_ok((const uint8_t *)pre->w, pre->n * 4, (const uint8_t *)fs->w, fs->n * 4, miss);
}
static void t_link(void) {
    struct spv vs, fs; uint64_t miss;
    // 对得上：顶点写 0、3（另有 gl_PerVertex 与 BuiltIn 变量），片元读 0、3
    spv_begin(&vs); { uint32_t b[3] = {12, 11, 0}; spv_op(&vs, 71, 3, b); } spv_types(&vs); spv_perv(&vs, 3); spv_var(&vs, 10, 3, 0); spv_var(&vs, 11, 3, 3);
    { uint32_t v[3] = {2, 12, 3}; spv_op(&vs, 59, 3, v); }   // %12 BuiltIn 输出，不带 Location
    spv_begin(&fs); spv_types(&fs); spv_var(&fs, 10, 1, 0); spv_var(&fs, 11, 1, 3);
    CHECK(link(&vs, &fs, &miss) == 1 && miss == 0, "两段对得上（内建块 / 内建变量不算用户接口）");
    // 片元多读一个 5：缺 5
    spv_var(&fs, 13, 1, 5);
    CHECK(link(&vs, &fs, &miss) == 0 && miss == (1ull << 5), "片元读了顶点没写的位置 5");
    // #2008 那种：顶点没有任何用户输出（只有 gl_PerVertex），片元读 0..6
    struct spv v0, f7; spv_begin(&v0); spv_types(&v0); spv_perv(&v0, 3);
    spv_begin(&f7); spv_types(&f7); for (int l = 0; l < 7; l++) spv_var(&f7, 30 + (uint32_t)l, 1, l);
    CHECK(link(&v0, &f7, &miss) == 0 && miss == 0x7f, "顶点没写用户输出、片元读 0..6");
    // 片元不读任何用户输入：对得上
    struct spv fe; spv_begin(&fe); spv_types(&fe); spv_perv(&fe, 1);
    CHECK(link(&v0, &fe, &miss) == 1 && miss == 0, "片元不读用户输入");
    // 认不全不下结论：片元有不带 Location 的用户输入 / 顶点读不了
    struct spv fu; spv_begin(&fu); spv_types(&fu); spv_var(&fu, 40, 1, -1); spv_var(&fu, 41, 1, 4);
    CHECK(link(&v0, &fu, &miss) == 1 && miss == 0, "片元有不带 Location 的用户输入：不下结论");
    CHECK(link(&v0, &f7, NULL) == 0, "missing 可为空");
    uint32_t junk[4] = {1, 2, 3, 4};
    CHECK(nvn_vk_spirv_link_ok((const uint8_t *)junk, sizeof junk, (const uint8_t *)f7.w, f7.n * 4, &miss) == 1, "上一段读不了：不下结论");
}

// Encode a small independent wire fixture, without going through the C# writer.
static void fixture_u32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (i * 8));
}
struct fixture_frame { uint8_t bytes[1024]; size_t used; };
static size_t fixture_record(struct fixture_frame *f, unsigned type, const char *field,
                             const void *value, unsigned size) {
    unsigned id = 0;
    for (unsigned i = 1; i < sizeof rv_fields / sizeof *rv_fields; i++)
        if (!strcmp(rv_fields[i], field)) { id = i; break; }
    const size_t pos = f->used;
    f->used += 8 + ((size + 3u) & ~3u);
    fixture_u32(f->bytes + pos, size);
    f->bytes[pos + 4] = (uint8_t)type;
    f->bytes[pos + 5] = (uint8_t)id;
    if (value) memcpy(f->bytes + pos + 8, value, size);
    return pos;
}
static void fixture_end(struct fixture_frame *f, size_t pos) {
    fixture_u32(f->bytes + pos, (uint32_t)(f->used - pos - 8));
}
static void t_live_result(void) {
    struct fixture_frame f = {{'S','2','A','D','R','B','1',0}, 16};
    size_t root = fixture_record(&f, 1, "", NULL, 0);
    static const char schema[] = "s2a-direct-program-v1";
    fixture_record(&f, 3, "schema", schema, sizeof schema - 1);
    size_t content = fixture_record(&f, 1, "content", NULL, 0);
    size_t stages = fixture_record(&f, 2, "stages", NULL, 0);
    size_t vs = fixture_record(&f, 1, "", NULL, 0);
    fixture_record(&f, 3, "stage", "vs", 2);
    static const uint8_t words[20] = {3, 2, 35, 7, 0, 3, 1, 0, 0, 0, 0, 0, 1};
    size_t module = fixture_record(&f, 9, "spirv", words, sizeof words);
    fixture_record(&f, 2, "c0Reads", NULL, 0);
    fixture_record(&f, 8, "c0ReadsStatic", NULL, 0);
    fixture_end(&f, vs); fixture_end(&f, stages);
    fixture_record(&f, 2, "bindings", NULL, 0);
    fixture_end(&f, content); fixture_end(&f, root);
    fixture_u32(f.bytes + 8, (uint32_t)f.used);

    struct fd_program p = {0};
    char why[192] = {0};
    int rc = nvn_vk_program_parse_graphics_live(f.bytes, f.used, &p, 1, why, sizeof why);
    CHECK(!rc && p.stage[0].spirv_bytes == sizeof words && !memcmp(p.stage[0].spirv, words, sizeof words) &&
          nvn_vk_binst_unread(&p.stage[0], 1) && !nvn_vk_binst_unread(&p.stage[0], 0),
          "live binary keeps raw SPIR-V and empty/unknown C0 reads");
    nvn_vk_program_free(&p);
    CHECK(nvn_vk_program_parse_graphics_stages(f.bytes, f.used, &p, 1, why, sizeof why) != 0,
          "persisted operands still require JSON");
    CHECK(nvn_vk_program_parse_graphics_live(f.bytes, f.used - 4, &p, 1, why, sizeof why) != 0,
          "live binary rejects a truncated frame");
    fixture_u32(f.bytes + module, sizeof words - 1);
    CHECK(nvn_vk_program_parse_graphics_live(f.bytes, f.used, &p, 1, why, sizeof why) != 0,
          "live binary refuses an unaligned SPIR-V extent");
    fixture_u32(f.bytes + module, UINT32_MAX);
    CHECK(nvn_vk_program_parse_graphics_live(f.bytes, f.used, &p, 1, why, sizeof why) != 0,
          "live binary refuses an overrun");
    nvn_vk_program_free(&p);
}

int main(void) {
    char why[192] = "";
    struct fd_program p;
    // c2 image-table words 72..86 are also sampled through combined slots 32..39.
    for (int v = 1; v <= 2; v++) for (int word = 72; word <= 88; word++) {
        char binding[192], what[100];
        snprintf(binding, sizeof binding, "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":%d,\"count\":1}", word);
        int rc = (v == 1 ? parse_json : parse_v2)(binding, &p, why, sizeof why);
        snprintf(what, sizeof what, "high-slot v%d c2 word %d maps only even image-table words", v, word);
        int valid = word < 88 && !(word & 1);
        CHECK(valid ? (!rc && p.binding_count == 1 && p.bindings[0].tex_slot == 32 + (word - 72) / 2) :
              (v == 2 ? rc != 0 : (!rc && p.bindings[0].tex_slot == -1)), what);
        nvn_vk_program_free(&p);
    }
    const char *bindings =
        "{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,\"count\":1,"
        "\"rw\":false,\"cbSlot\":0,\"cbOffset\":272,\"nvnSlot\":0},"
        "{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":1,\"count\":1,"
        "\"rw\":true,\"cbSlot\":3,\"cbOffset\":56,\"uboSlot\":0,\"uboOffset\":56},"
        "{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":2,\"count\":1,"
        "\"rw\":false,\"cbSlot\":0,\"cbOffset\":544,\"uboPtr\":1}";
    int rc = parse_json(bindings, &p, why, sizeof why);
    if (rc) printf("parse why: %s\n", why);
    CHECK(rc == 0, "valid graphics storage provenance parses");
    if (p.binding_count == 3) {
        CHECK(p.bindings[0].kind == 1 && p.bindings[0].storage_kind == 1 &&
              p.bindings[0].storage_slot == 0 && !p.bindings[0].storage_writable &&
              p.bindings[0].storage_cb_slot == 0 && p.bindings[0].storage_cb_offset == 272,
              "nvnSlot provenance is explicit");
        CHECK(p.bindings[1].kind == 1 && p.bindings[1].storage_kind == 2 &&
              p.bindings[1].storage_slot == 0 && p.bindings[1].storage_writable &&
              p.bindings[1].storage_cb_slot == 3 && p.bindings[1].storage_cb_offset == 56,
              "uboSlot/uboOffset provenance is explicit and rw is carried");
        CHECK(p.bindings[2].kind == 1 && p.bindings[2].storage_kind == 3 &&
              p.bindings[2].storage_slot == 1,
              "indirect UBO pointer remains an explicit unsupported source");
    } else {
        CHECK(0, "three storage bindings parsed");
    }
    nvn_vk_program_free(&p);

    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":0,\"count\":524288}",
                     &p, why, sizeof why) != 0, "large texture array is rejected before narrowing");
    CHECK(strstr(why, "524288") && strstr(why, "fs"), "large texture array reports exact count and stage");
    nvn_vk_program_free(&p);
    {
        const unsigned lengths[] = {1, 64, 65, 65535, 65536, 1048576};
        for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
            char binding[192], number[24];
            snprintf(binding, sizeof binding, "{\"stage\":\"vs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":0,\"count\":%u}", lengths[i]);
            snprintf(number, sizeof number, "%u", lengths[i]); why[0] = 0;
            int parsed = parse_json(binding, &p, why, sizeof why);
            if (lengths[i] <= 64) CHECK(parsed == 0 && p.bindings[0].count == lengths[i], "array at or below limit retains count");
            else CHECK(parsed != 0 && strstr(why, number) && strstr(why, "vs"), "oversized array retains count and stage in rejection");
            nvn_vk_program_free(&p);
        }
        why[0] = 0;
        CHECK(parse_json("{\"stage\":\"vs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":0}", &p, why, sizeof why) != 0,
              "zero count is not silently treated as one");
        nvn_vk_program_free(&p);
    }

    // 分离贴图槽 128 与采样器槽 3：生产 parser 必须接受 v2 且保持宽槽位。
    why[0] = 0;
    int separate_rc = parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"samp_slot\":3,\"count\":1}", &p, why, sizeof why);
    CHECK(separate_rc == 0, "v2 separate texture and sampler parse");
    if (!separate_rc) CHECK(p.binding_count == 1 && p.bindings[0].tex_slot == 128 && p.bindings[0].samp_slot == 3, "separate sources preserved");
    nvn_vk_program_free(&p);
    why[0] = 0;
    CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"samp_slot\":128,\"count\":1}", &p, why, sizeof why) != 0, "out of range sampler slot is rejected");
    nvn_vk_program_free(&p);
    why[0] = 0;
    CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"samp_slot\":3,\"count\":2}", &p, why, sizeof why) != 0, "separate source array is rejected");
    nvn_vk_program_free(&p);

    why[0] = 0;
    CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"samp_slot\":3,\"samp_slot\":4,\"count\":1}", &p, why, sizeof why) != 0, "duplicate sampler field rejected");
    nvn_vk_program_free(&p);
    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"fs\",\"kind\":\"bogus\",\"set\":2,\"binding\":0}", &p, why, sizeof why) != 0, "unknown binding kind rejected");
    nvn_vk_program_free(&p);
    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":8},{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":10}", &p, why, sizeof why) != 0, "duplicate descriptor rejected");
    nvn_vk_program_free(&p);
    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":8}", &p, why, sizeof why) == 0 && p.bindings[0].samp_slot == -1, "v1 keeps combined sampler sentinel");
    nvn_vk_program_free(&p);

    // 立即句柄直接读 c2 分离贴图表的字（空轨 2nd 9000：fs handle 90 / 92 / 124，取尺寸 / texelFetch，无分离采样器）：
    //   slot 90..344 偶、count 1、无 samp_slot → 分离贴图槽 128 + k、采样器取同字（samp_slot −1）；v1 / v2 同。奇字、表外、数组照旧不接。
    for (int v = 1; v <= 2; ++v) {
        int (*parse)(const char *, struct fd_program *, char *, size_t) = v == 1 ? parse_json : parse_v2;
        static const struct { int slot, count, want; } sep[] = {{90, 1, 128}, {92, 1, 129}, {124, 1, 145}, {344, 1, 255}, {91, 1, -1}, {346, 1, -1}, {90, 2, -1}};
        for (unsigned k = 0; k < sizeof sep / sizeof sep[0]; k++) {
            char b[200]; why[0] = 0;
            snprintf(b, sizeof b, "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":%d,\"count\":%d}", sep[k].slot, sep[k].count);
            int rc = parse(b, &p, why, sizeof why);
            char what[160]; snprintf(what, sizeof what, "v%d direct separate texture slot %d count %d -> tex_slot %d", v, sep[k].slot, sep[k].count, sep[k].want);
            if (sep[k].want >= 0) CHECK(rc == 0 && p.binding_count == 1 && p.bindings[0].tex_slot == sep[k].want && p.bindings[0].samp_slot == -1, what);
            else CHECK(v == 2 ? rc != 0 : (rc == 0 && p.binding_count == 1 && p.bindings[0].tex_slot == -1), what);   // v2 拒；v1 保持无槽（layout 如实报红）
            nvn_vk_program_free(&p);
        }
    }

    // #1594：类型 3 句柄（贴图字在 c2 分离贴图表、采样器池下标是着色器里的立即数）。v2 结果的 constSampler = 池下标 0..4095，
    //   slot = 贴图字（90..344 偶）→ 分离贴图槽 128 + k；采样器来源 = 常量池下标（FD_SAMP_ID），不是 samp_slot，也不是「取同字高位」。
    //   下标 0 是真下标（不能当「没有」）；没有这个字段的条目一律保持「没有常量采样器」。
    {
        static const struct { int slot, sampler, want; } cs_ok[] = {{90, 257, 128}, {92, 0, 129}, {124, 1, 145}, {344, 4095, 255}};
        for (unsigned k = 0; k < sizeof cs_ok / sizeof cs_ok[0]; k++) {
            char b[240]; why[0] = 0;
            snprintf(b, sizeof b, "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":%d,\"constSampler\":%d,\"count\":1}", cs_ok[k].slot, cs_ok[k].sampler);
            int rc = parse_v2(b, &p, why, sizeof why);
            char what[160]; snprintf(what, sizeof what, "v2 constant sampler slot %d sampler %d parses", cs_ok[k].slot, cs_ok[k].sampler);
            CHECK(rc == 0 && p.binding_count == 1, what);
            if (rc == 0 && p.binding_count == 1) {
                const struct fd_binding *cb = &p.bindings[0];
                snprintf(what, sizeof what, "constant sampler slot %d sampler %d -> tex_slot %d + sampler id, no samp_slot / ubo", cs_ok[k].slot, cs_ok[k].sampler, cs_ok[k].want);
                CHECK(cb->kind == 2 && cb->count == 1 && cb->tex_slot == cs_ok[k].want && cb->samp_slot == -1 && !cb->tex_ubo &&
                      cb->samp_src == FD_SAMP_ID && cb->samp_val == cs_ok[k].sampler && cb->samp_pool == 1, what);
            }
            nvn_vk_program_free(&p);
        }
        // 没有 constSampler 的分离贴图 / 分离采样器条目：不带常量采样器（采样器来源保持 NONE）
        why[0] = 0;
        CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"samp_slot\":3,\"count\":1}", &p, why, sizeof why) == 0 &&
              p.binding_count == 1 && p.bindings[0].samp_slot == 3 && p.bindings[0].samp_src == FD_SAMP_NONE, "samp_slot form carries no constant sampler");
        nvn_vk_program_free(&p);
        // 两条常量采样器绑定：各取各的池下标
        why[0] = 0;
        CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":257,\"count\":1},"
                       "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":1,\"slot\":100,\"constSampler\":259,\"count\":1}", &p, why, sizeof why) == 0 &&
              p.binding_count == 2 && p.bindings[0].tex_slot == 128 && p.bindings[0].samp_val == 257 && p.bindings[1].tex_slot == 133 && p.bindings[1].samp_val == 259 &&
              p.bindings[0].samp_src == FD_SAMP_ID && p.bindings[1].samp_src == FD_SAMP_ID, "two constant samplers keep their own sampler ids");
        nvn_vk_program_free(&p);
        // 拒绝：任何一条都不能退化成「只填贴图槽、采样器取同字高位」
        static const char *const cs_bad[] = {
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":4096,\"count\":1}",                         // 池下标超 12 位
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":-1,\"count\":1}",                           // 负数
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":1.5,\"count\":1}",                          // 非整数
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":\"7\",\"count\":1}",                        // 不是数字
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":91,\"constSampler\":7,\"count\":1}",                            // 奇字
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":88,\"constSampler\":7,\"count\":1}",                            // 分离贴图表之前
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":346,\"constSampler\":7,\"count\":1}",                           // 分离贴图表之后
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"constSampler\":7,\"count\":1}",                                        // 没有 slot
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":7,\"count\":2}",                            // 数组
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"samp_slot\":3,\"constSampler\":7,\"count\":1}",             // 同时给 samp_slot
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":7,\"count\":1,\"uboSlot\":1,\"uboOffset\":1024}", // c2 的槽和 UBO 来源同时给（贴图字来源不唯一）
            "{\"stage\":\"fs\",\"kind\":\"uniform\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":7,\"count\":1}",                            // 不是贴图
            "{\"stage\":\"fs\",\"kind\":\"storage\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":7,\"count\":1,\"rw\":false,\"nvnSlot\":0}",  // 不是贴图
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":7,\"constSampler\":8,\"count\":1}",         // 字段重复
        };
        for (unsigned i = 0; i < sizeof cs_bad / sizeof cs_bad[0]; ++i) {
            why[0] = 0;
            char what[96]; snprintf(what, sizeof what, "constant sampler negative %u rejected by v2", i);
            CHECK(parse_v2(cs_bad[i], &p, why, sizeof why) != 0, what);
            nvn_vk_program_free(&p);
        }
        // v1 结果没有这个字段的合同：带了就拒（不能被当成「多余字段」忽略掉后绑成错的采样器）
        why[0] = 0;
        CHECK(parse_json("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":7,\"count\":1}", &p, why, sizeof why) != 0, "constant sampler in a v1 result is rejected");
        nvn_vk_program_free(&p);
    }

    // #1594 片 3：类型 3 句柄的贴图字在游戏 UBO（平铺的 uboSlot = cbuf − 3 + uboOffset = 字节偏移，没有 slot）+ constSampler = 立即数采样器池下标。
    //   贴图字来源 = 常量缓冲 tex_u 处的字；采样器来源 = FD_SAMP_ID / samp_val，samp_pool = 1（池下标，回放要加池起点），不是「取同字高位」（FD_SAMP_UBO）。
    {
        static const struct { int ubo, off, sampler; } ub_ok[] = {{2, 160, 260}, {0, 0, 0}, {15, 65532, 4095}, {1, 1024, 1}};
        for (unsigned k = 0; k < sizeof ub_ok / sizeof ub_ok[0]; k++) {
            char b[240]; why[0] = 0;
            snprintf(b, sizeof b, "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":%d,\"uboOffset\":%d,\"constSampler\":%d}", ub_ok[k].ubo, ub_ok[k].off, ub_ok[k].sampler);
            int rc = parse_v2(b, &p, why, sizeof why);
            char what[320]; snprintf(what, sizeof what, "v2 UBO %d +%d with constant sampler %d parses (why=%s)", ub_ok[k].ubo, ub_ok[k].off, ub_ok[k].sampler, why);
            CHECK(rc == 0 && p.binding_count == 1, what);
            if (rc == 0 && p.binding_count == 1) {
                const struct fd_binding *ub = &p.bindings[0];
                snprintf(what, sizeof what, "UBO %d +%d / constant sampler %d -> texture word at the UBO, sampler id (pool index), no slot / samp_slot", ub_ok[k].ubo, ub_ok[k].off, ub_ok[k].sampler);
                CHECK(ub->kind == 2 && ub->count == 1 && ub->slot < 0 && ub->tex_slot == -1 && ub->samp_slot == -1 && ub->tex_ubo == 1 &&
                      ub->tex_u.slot == ub_ok[k].ubo && ub->tex_u.offset == ub_ok[k].off && !ub->tex_u.sid && !ub->tex_u.pair &&
                      ub->samp_src == FD_SAMP_ID && ub->samp_val == ub_ok[k].sampler && ub->samp_pool == 1, what);
            }
            nvn_vk_program_free(&p);
        }
        // 两个口径分开：离线库写法（uboHandle + sampler 对象 {"source":"id"}）的 FD_SAMP_ID 是宿主登记号原值，samp_pool = 0；
        //   没有 constSampler 的平铺 UBO 贴图仍是「采样器取同字」（FD_SAMP_UBO），不带池下标标记
        why[0] = 0;
        CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboHandle\":{\"slot\":1,\"offset\":1024},\"sampler\":{\"source\":\"id\",\"id\":257}}", &p, why, sizeof why) == 0 &&
              p.binding_count == 1 && p.bindings[0].tex_ubo == 1 && p.bindings[0].samp_src == FD_SAMP_ID && p.bindings[0].samp_val == 257 && p.bindings[0].samp_pool == 0,
              "a library-style sampler id keeps the raw registered id (samp_pool 0)");
        nvn_vk_program_free(&p);
        why[0] = 0;
        CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":1,\"uboOffset\":1024}", &p, why, sizeof why) == 0 &&
              p.binding_count == 1 && p.bindings[0].samp_src == FD_SAMP_UBO && p.bindings[0].samp_pool == 0, "a flat UBO texture without constSampler keeps 'sampler from the same word'");
        nvn_vk_program_free(&p);
        // 同一个结果里 c2 形态与 UBO 形态各带各的常量采样器
        why[0] = 0;
        CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":90,\"constSampler\":257,\"count\":1},"
                       "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":1,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":260}", &p, why, sizeof why) == 0 &&
              p.binding_count == 2 && !p.bindings[0].tex_ubo && p.bindings[0].tex_slot == 128 && p.bindings[0].samp_val == 257 &&
              p.bindings[1].tex_ubo == 1 && p.bindings[1].tex_slot == -1 && p.bindings[1].tex_u.slot == 2 && p.bindings[1].samp_val == 260 &&
              p.bindings[0].samp_pool == 1 && p.bindings[1].samp_pool == 1, "a c2 constant sampler and a UBO constant sampler in one result keep their own sources");
        nvn_vk_program_free(&p);
        // 拒绝：任何一条都不能退化成「只填贴图来源、采样器取同字高位」
        static const char *const ub_bad[] = {
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":4096}",                                    // 池下标超 12 位
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":-1}",                                      // 负数
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":\"7\"}",                                   // 不是数字
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":1.5}",                                     // 非整数
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":2,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":7}",                                       // 数组
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"samp_slot\":3,\"constSampler\":7}",                     // 同时给 samp_slot
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"sampler\":{\"source\":\"id\",\"id\":3},\"constSampler\":7}", // 同时给 sampler 对象
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboHandle\":{\"slot\":2,\"offset\":160},\"constSampler\":7}",                          // uboHandle 写法不接
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboHandle\":{\"slot\":2,\"offset\":160},\"sampler\":{\"source\":\"id\",\"id\":3},\"constSampler\":7}", // uboHandle + sampler 对象
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"slot\":8,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":7}",                         // 同时给组合槽
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":16,\"uboOffset\":160,\"constSampler\":7}",                                     // UBO 号越界
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"constSampler\":7}",                                                         // 缺偏移
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":6,\"constSampler\":7}",                                        // 不是字偏移
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":7,\"constSampler\":8}",                  // 字段重复
            "{\"stage\":\"fs\",\"kind\":\"storage\",\"set\":2,\"binding\":0,\"count\":1,\"rw\":false,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":7}",                         // 不是贴图（存储绑定也带 uboSlot）
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"constSampler\":7}",                                                                       // 没有任何贴图字来源
        };
        for (unsigned i = 0; i < sizeof ub_bad / sizeof ub_bad[0]; ++i) {
            why[0] = 0;
            char what[96]; snprintf(what, sizeof what, "UBO constant sampler negative %u rejected by v2", i);
            CHECK(parse_v2(ub_bad[i], &p, why, sizeof why) != 0, what);
            nvn_vk_program_free(&p);
        }
        // v1 结果没有这个字段的合同：带了就拒
        why[0] = 0;
        CHECK(parse_json("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":7}", &p, why, sizeof why) != 0,
              "UBO constant sampler in a v1 result is rejected");
        nvn_vk_program_free(&p);
    }

    // 整池数组（安卓整池 N=0）：v2 结果的 pool = "texture" / "sampler"，count = 池长，没有任何槽位来源。
    //   期望（手算）：贴图池 count 64 → pool 1、count 64、tex_slot −1；采样器池 count 8 → pool 2、count 8；池长可以超 FD_MAX_ARRAY_ELEMENTS（4096）
    {
        CHECK(parse_v2("{\"stage\":\"fs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":0,\"count\":64,\"pool\":\"texture\"},"
                       "{\"stage\":\"fs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":64,\"count\":8,\"pool\":\"sampler\"},"
                       "{\"stage\":\"vs\",\"kind\":\"texture\",\"source\":\"texture\",\"set\":2,\"binding\":72,\"count\":4096,\"pool\":\"texture\"}", &p, why, sizeof why) == 0 &&
              p.binding_count == 3 && p.bindings[0].pool == 1 && p.bindings[0].count == 64 && p.bindings[0].tex_slot == -1 && p.bindings[0].stage_idx == 1 &&
              p.bindings[1].pool == 2 && p.bindings[1].count == 8 && p.bindings[1].samp_slot == -1 &&
              p.bindings[2].pool == 1 && p.bindings[2].count == 4096 && p.bindings[2].stage_idx == 0, "v2 pool arrays parse: texture 64 / sampler 8 / texture 4096");
        nvn_vk_program_free(&p);
        static const char *const pool_bad[] = {
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":64,\"pool\":\"image\"}",                      // 不认识的池
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":64,\"pool\":1}",                                // 不是字符串
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"slot\":0,\"count\":64,\"pool\":\"texture\"}",          // 同时带 slot
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"samp_slot\":3,\"count\":64,\"pool\":\"texture\"}",     // 同时带 samp_slot
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"constSampler\":3,\"count\":64,\"pool\":\"sampler\"}",  // 同时带 constSampler
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":64,\"pool\":\"texture\",\"uboSlot\":1,\"uboOffset\":0}", // 同时带 UBO 句柄
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"pool\":\"texture\"}",                       // 池长 < 2
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":65535,\"pool\":\"texture\"}",                   // 池长 ≥ 0xffff
            "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"pool\":\"texture\"}",                                    // 没有 count
            "{\"stage\":\"fs\",\"kind\":\"image\",\"set\":3,\"binding\":0,\"count\":64,\"pool\":\"texture\"}",                        // 不是贴图
            "{\"stage\":\"cs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":64,\"pool\":\"texture\"}",                      // 不是图形阶段
        };
        for (unsigned i = 0; i < sizeof pool_bad / sizeof pool_bad[0]; ++i) {
            why[0] = 0;
            char what[80]; snprintf(what, sizeof what, "pool array negative %u rejected by v2", i);
            CHECK(parse_v2(pool_bad[i], &p, why, sizeof why) != 0, what);
            nvn_vk_program_free(&p);
        }
        why[0] = 0;
        CHECK(parse_json("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":64,\"pool\":\"texture\"}", &p, why, sizeof why) != 0,
              "pool array in a v1 result is rejected");
        nvn_vk_program_free(&p);
    }

    // #999 / #1189：现编结果的平铺写法 uboSlot（cbuf − 3）+ uboOffset（句柄字字节偏移），没有 slot。
    //   组合句柄一个字：贴图号低 20 位、采样器号高 12 位 → 贴图与采样器都取同一个字（FD_SAMP_UBO，采样器字 sid=0）。
    for (int v = 1; v <= 2; ++v) {
        int (*parse)(const char *, struct fd_program *, char *, size_t) = v == 1 ? parse_json : parse_v2;
        why[0] = 0;
        int flat_rc = parse("{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":1,\"uboOffset\":1024}", &p, why, sizeof why);
        if (flat_rc) printf("flat v%d why: %s\n", v, why);
        CHECK(flat_rc == 0, "flat uboSlot/uboOffset texture parses");
        if (!flat_rc && p.binding_count == 1) {
            const struct fd_binding *b = &p.bindings[0];
            CHECK(b->tex_ubo == 1 && b->slot < 0 && b->tex_u.slot == 1 && b->tex_u.offset == 1024 && !b->tex_u.sid && !b->tex_u.pair,
                  "flat form: texture word = UBO 1 byte 1024, no sid / pair");
            CHECK(b->samp_src == FD_SAMP_UBO && b->samp_u.slot == 1 && b->samp_u.offset == 1024 && !b->samp_u.sid && !b->samp_u.pair,
                  "flat form: sampler read from the same combined word");
        }
        nvn_vk_program_free(&p);
    }
    static const char *const flat_bad[] = {
        "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":16,\"uboOffset\":0}",              // 槽越界
        "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":0}",                                // 缺偏移
        "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":0,\"uboOffset\":65536}",           // 偏移出 64 KiB
        "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"uboSlot\":0,\"uboOffset\":6}",               // 不是字偏移
        "{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"slot\":8,\"uboSlot\":0,\"uboOffset\":0}",  // 同时给组合槽
    };
    for (unsigned i = 0; i < sizeof flat_bad / sizeof flat_bad[0]; ++i) {
        why[0] = 0;
        char what[64]; snprintf(what, sizeof what, "flat form negative %u rejected", i);
        CHECK(parse_json(flat_bad[i], &p, why, sizeof why) != 0, what);
        nvn_vk_program_free(&p);
    }

    // 负控 1：NVN 槽位越界必须拒绝，不能截断成 0 绑到别的缓冲。
    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,"
                     "\"nvnSlot\":16}", &p, why, sizeof why) != 0,
          "nvnSlot outside 0..15 is rejected");
    CHECK(strstr(why, "nvnSlot") != NULL, "rejection names nvnSlot");
    nvn_vk_program_free(&p);

    // 负控 1b：rw 缺失不能静默当只读（旧结果/不完整结果必须拒绝）。
    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,"
                     "\"cbSlot\":0,\"cbOffset\":272,\"nvnSlot\":0}", &p, why, sizeof why) != 0,
          "storage without rw is rejected");
    CHECK(strstr(why, "rw") != NULL, "rejection names missing rw");
    nvn_vk_program_free(&p);

    // 负控 2：rw 必须是布尔；字符串不能静默当 false。
    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,"
                     "\"rw\":\"false\"}", &p, why, sizeof why) != 0,
          "non-boolean rw is rejected");
    CHECK(strstr(why, "rw") != NULL, "rejection names rw");
    nvn_vk_program_free(&p);

    // 负控 3：没有映射的 storage 保留 kind=0；运行时据此报红，不猜 NVN 槽。
    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,"
                     "\"rw\":false,\"cbSlot\":0,\"cbOffset\":0xFFFF}", &p, why, sizeof why) != 0,
          "non-decimal cbOffset is rejected");
    nvn_vk_program_free(&p);

    why[0] = 0;
    CHECK(parse_json("{\"stage\":\"vs\",\"kind\":\"storage\",\"set\":1,\"binding\":0,"
                     "\"rw\":false,\"cbSlot\":0,\"cbOffset\":1040}", &p, why, sizeof why) == 0,
          "storage without mapping still parses");
    if (p.binding_count == 1) {
        CHECK(p.bindings[0].storage_kind == 0 && p.bindings[0].storage_slot == 0,
              "missing mapping stays explicit (runtime red), no guessed slot");
    } else {
        CHECK(0, "one unmapped storage binding parsed");
    }
    nvn_vk_program_free(&p);

    // 图形阶段 c0Reads → binst_unread（回放据此决定间接绘制要不要拷 c0 + 0x644）。
    //   缺字段 / null = 未知 = 0；数组且无一段碰 [0x644, 0x648) = 1；碰到 = 0；形状不对 = 拒。
    {
        static const struct { const char *vs_extra, *fs_extra; int rc, vs, fs; const char *what; } cases[] = {
            { "", "", 0, 0, 0, "c0Reads missing: unknown, both stages keep the copy" },
            { ",\"c0Reads\":null", ",\"c0Reads\":null", 0, 0, 0, "c0Reads null: unknown" },
            { ",\"c0Reads\":[]", ",\"c0Reads\":[[0,4],[1552,4]]", 0, 1, 1, "empty and non-overlapping read sets prove no base-instance read" },
            { ",\"c0Reads\":[[1604,4]]", ",\"c0Reads\":[]", 0, 0, 1, "exact 0x644 read keeps the vs copy, fs proven" },
            { ",\"c0Reads\":[[1600,16]]", ",\"c0Reads\":[[1606,1]]", 0, 0, 0, "vec4 at 0x640 and a byte inside 0x644 both overlap" },
            { ",\"c0Reads\":[[1602,4]]", ",\"c0Reads\":[[1596,8],[1608,4]]", 0, 0, 1, "range ending inside 0x644 overlaps; ending at 0x644 or starting at 0x648 does not" },
            { ",\"c0Reads\":[[1601,4]]", ",\"c0Reads\":[[1600,4]]", 0, 0, 1, "4 bytes from 0x641 cover the first byte of 0x644; 0x640..0x643 does not" },
            { ",\"c0Reads\":[[1600,4]]", ",\"c0Reads\":[[1604,0]]", -1, 0, 0, "zero-size range is rejected" },
            { ",\"c0Reads\":7", "", -1, 0, 0, "non-array c0Reads is rejected" },
            { ",\"c0Reads\":[[1604]]", "", -1, 0, 0, "single-element range is rejected" },
            { ",\"c0Reads\":[],\"c0ReadsStatic\":7", "", -1, 0, 0, "invalid static c0 read metadata must be rejected" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char json[1024]; char msg[512];   // 断言说明要装下 why（最多 191 字节）
            int n = snprintf(json, sizeof json,
                "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
                "\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"%s},{\"stage\":\"fs\",\"spirv\":\"%s\"%s}],"
                "\"bindings\":[]}}", k_spirv, cases[i].vs_extra, k_spirv, cases[i].fs_extra);
            memset(&p, 0, sizeof p); why[0] = 0;
            int rc = nvn_vk_program_parse_graphics((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
            snprintf(msg, sizeof msg, "%s (rc=%d vs=%d fs=%d why=%s)", cases[i].what, rc, p.stage[0].binst_unread, p.stage[1].binst_unread, why);
            if (cases[i].rc) CHECK(rc != 0 && strstr(why, "c0 read") != NULL, msg);
            else CHECK(rc == 0 && p.stage[0].binst_unread == cases[i].vs && p.stage[1].binst_unread == cases[i].fs, msg);
            nvn_vk_program_free(&p);
        }
    }

    // 同一个结果对象按开关选读集：动态可证仅开档省拷；静态可证两档都省；旧结果关档保守。
    {
        static const struct { const char *fields; int on, off; const char *why; } cases[] = {
            { ",\"c0Reads\":[[0,1024]],\"c0ReadsStatic\":null", 1, 0, "bounded dynamic only proves on" },
            { ",\"c0Reads\":[[0,4]],\"c0ReadsStatic\":[[0,4]]", 1, 1, "static proof works in both modes" },
            { ",\"c0Reads\":[[1604,4]],\"c0ReadsStatic\":[[1604,4]]", 0, 0, "real base-instance read copied in both modes" },
            { ",\"c0Reads\":null,\"c0ReadsStatic\":null", 0, 0, "unknown dynamic copied in both modes" },
            { ",\"c0Reads\":[]", 1, 0, "legacy result without static proof is conservative when off" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char json[1024], msg[512];
            int n = snprintf(json, sizeof json, "{\"schema\":\"s2a-direct-program-v1\",\"content\":{\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"%s},{\"stage\":\"fs\",\"spirv\":\"%s\"}],\"bindings\":[]}}", k_spirv, cases[i].fields, k_spirv);
            memset(&p, 0, sizeof p); why[0] = 0;
            int rc = nvn_vk_program_parse_graphics((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
            snprintf(msg, sizeof msg, "%s (rc=%d why=%s)", cases[i].why, rc, why);
            CHECK(rc == 0 && nvn_vk_binst_unread(&p.stage[0], 1) == cases[i].on && nvn_vk_binst_unread(&p.stage[0], 0) == cases[i].off, msg);
            nvn_vk_program_free(&p);
        }
    }

    t_live_result();
    t_link();
    if (fails) { printf("direct nvn program result test: %d FAILED\n", fails); return 1; }
    printf("direct nvn program result test: ok\n");
    return 0;
}
