// #1403：图形 storage buffer 描述符的编译结果 provenance 宿主回归。
//
// 这里原样链接生产 parser（nvn_vk_program_result.c），只验证：
//   * 编译结果的 nvnSlot / uboSlot+uboOffset / uboPtr / rw 被读成明确的运行时来源；
//   * 越界 nvnSlot、非布尔 rw 准确拒绝；
//   * 没有映射的 storage binding 保持 kind=0，运行时必须报红而不是猜槽位。
#include <stdio.h>
#include <string.h>

#include "nvn/nvn_vk_program_result.h"
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

int main(void) {
    char why[192] = "";
    struct fd_program p;
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
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char json[1024]; char msg[192];
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

    if (fails) { printf("direct nvn program result test: %d FAILED\n", fails); return 1; }
    printf("direct nvn program result test: ok\n");
    return 0;
}
