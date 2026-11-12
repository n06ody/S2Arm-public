// #1403：图形 storage buffer 描述符的编译结果 provenance 宿主回归。
//
// 这里原样链接生产 parser（nvn_vk_program_result.c），只验证：
//   * 编译结果的 nvnSlot / uboSlot+uboOffset / uboPtr / rw 被读成明确的运行时来源；
//   * 越界 nvnSlot、非布尔 rw 准确拒绝；
//   * 没有映射的 storage binding 保持 kind=0，运行时必须报红而不是猜槽位。
#include <stdio.h>
#include <string.h>

#include "nvn/nvn_vk_program_result.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static const char *k_spirv = "AwIjBwABAwAAAAAAAQAAAAAAAAA=";

static int parse_json(const char *bindings, struct fd_program *p, char *why, size_t cap) {
    char json[4096];
    int n = snprintf(json, sizeof json,
        "{\"schema\":\"s2a-direct-program-v1\",\"content\":{"
        "\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"},{\"stage\":\"fs\",\"spirv\":\"%s\"}],"
        "\"bindings\":[%s]}}", k_spirv, k_spirv, bindings);
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

    if (fails) { printf("direct nvn program result test: %d FAILED\n", fails); return 1; }
    printf("direct nvn program result test: ok\n");
    return 0;
}
