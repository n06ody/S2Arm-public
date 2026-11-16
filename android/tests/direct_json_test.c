// C1：严格 JSON 读取器的接受/拒绝向量。只用本仓实现，不读游戏数据。
#include "../native/direct/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); g_failures++; } } while (0)

static int rejects(const char *text, const char *what) {
    s2a_json_object object;
    char error[96];
    int rc = s2a_json_parse_object((const uint8_t *)text, strlen(text), &object, error, sizeof error);
    if (!rc) { printf("FAIL accepted: %s\n", what); g_failures++; return 0; }
    if (!error[0]) { printf("FAIL no error text: %s\n", what); g_failures++; }
    return 1;
}

int main(void) {
    const char *valid =
        "{\"schema\":\"s2a-graphics-v1\",\"directABI\":1,\"nested\":{\"a\":[1,2,{\"b\":null}]},"
        "\"escaped\":\"a\\\"b\\\\c\\u4e2d\\ud83d\\ude00\",\"zero\":0,\"neg\":-2,\"frac\":1.5e2,"
        "\"ok\":true,\"no\":false,\"nil\":null,\"空\":\"值\"}";
    s2a_json_object object;
    char error[96];
    CHECK(s2a_json_parse_object((const uint8_t *)valid, strlen(valid), &object, error, sizeof error) == 0,
          "valid document");
    CHECK(object.count == 11, "field count");
    const s2a_json_field *f = s2a_json_get(&object, "schema");
    CHECK(f && f->kind == S2A_JSON_STRING && !strcmp(f->text, "s2a-graphics-v1"), "schema string");
    f = s2a_json_get(&object, "directABI");
    CHECK(f && f->kind == S2A_JSON_NUMBER && f->is_unsigned && f->unsigned_value == 1 && f->number == 1.0, "integer");
    f = s2a_json_get(&object, "escaped");
    CHECK(f && !strcmp(f->text, "a\"b\\c中😀"), "escapes and unicode");
    CHECK(s2a_json_get(&object, "zero")->is_unsigned, "zero unsigned");
    CHECK(!s2a_json_get(&object, "neg")->is_unsigned && s2a_json_get(&object, "neg")->number == -2.0, "negative");
    CHECK(s2a_json_get(&object, "frac")->number == 150.0 && !s2a_json_get(&object, "frac")->is_unsigned, "fraction");
    CHECK(s2a_json_get(&object, "ok")->kind == S2A_JSON_TRUE, "true");
    CHECK(s2a_json_get(&object, "no")->kind == S2A_JSON_FALSE, "false");
    CHECK(s2a_json_get(&object, "nil")->kind == S2A_JSON_NULL, "null");
    CHECK(s2a_json_get(&object, "空") && !strcmp(s2a_json_get(&object, "空")->text, "值"), "utf-8 key");
    CHECK(!s2a_json_get(&object, "missing"), "missing key");

    {
        const char *text = "{\"a\": [ {\"x\":\"a,b\"}, [1,2], \"x\\\"y\", 42, null ], \"empty\": []}";
        CHECK(s2a_json_parse_object((const uint8_t *)text, strlen(text), &object, error, sizeof error) == 0, "iterator fixture parses");
        const s2a_json_field *array = s2a_json_get(&object, "a");
        uint32_t cursor = 1;
        for (uint32_t i = 0; i < 5; i++) {
            const uint8_t *next = NULL, *span = NULL; uint32_t n = 0, m = 0, before = cursor;
            CHECK(s2a_json_array_next(array, &cursor, &next, &n) == 1 && cursor > before, "iterator advances");
            CHECK(s2a_json_array_span(array, i, &span, &m) == 0 && n == m && !memcmp(next, span, n), "iterator matches indexed span");
        }
        const uint8_t *last = NULL; uint32_t bytes = 0;
        CHECK(s2a_json_array_next(array, &cursor, &last, &bytes) == 0, "iterator end");
        cursor = 1;
        CHECK(s2a_json_array_next(s2a_json_get(&object, "empty"), &cursor, &last, &bytes) == 0, "iterator empty");
    }

    rejects("", "empty");
    rejects("   ", "whitespace");
    rejects("[]", "top-level array");
    rejects("null", "top-level null");
    rejects("{} x", "trailing bytes");
    rejects("{\"a\":1,}", "trailing comma");
    rejects("{a:1}", "unquoted key");
    rejects("{\"a\":1 \"b\":2}", "missing comma");
    rejects("{\"a\":1,\"a\":2}", "duplicate top-level key");
    rejects("{\"a\":01}", "leading zero");
    rejects("{\"a\":1e999}", "overflow");
    rejects("{\"a\":NaN}", "NaN");
    rejects("{\"a\":\"\x01\"}", "raw control");
    rejects("{\"a\":\"\\ud800\"}", "lone high surrogate");
    rejects("{\"a\":\"\\udc00\"}", "lone low surrogate");
    rejects("{\"a\":\"\\ud800\\u0041\"}", "bad pair");
    rejects("{\"a\":\"\\x41\"}", "unknown escape");
    rejects("{\"a\":\"\xC0\x80\"}", "overlong utf8");
    rejects("{\"a\":\"\xED\xA0\x80\"}", "utf8 surrogate");
    rejects("{\"a\":\"\xF4\x90\x80\x80\"}", "utf8 above max");
    rejects("{\"a\":\"\xE4\xB8\"}", "truncated utf8");
    {
        char text[512];
        strcpy(text, "{\"a\":");
        for (int i = 0; i < 17; i++) strcat(text, "[");
        strcat(text, "1");
        for (int i = 0; i < 17; i++) strcat(text, "]");
        strcat(text, "}");
        rejects(text, "too deep");
    }
    {
        static char text[70000];
        size_t at = 0;
        at += (size_t)snprintf(text, sizeof text, "{");
        for (int i = 0; i < 64; i++) at += (size_t)snprintf(text + at, sizeof text - at, "%s\"k%d\":%d", i ? "," : "", i, i);
        at += (size_t)snprintf(text + at, sizeof text - at, "}");
        CHECK(s2a_json_parse_object((const uint8_t *)text, at, &object, error, sizeof error) == 0, "64 fields accepted");
        text[at - 1] = 0;
        at += (size_t)snprintf(text + at - 1, sizeof text - at + 1, ",\"k64\":64}");
        CHECK(s2a_json_parse_object((const uint8_t *)text, at, &object, error, sizeof error) != 0, "65 fields rejected");
    }
    {
        char text[600];
        snprintf(text, sizeof text, "{\"%.300s\":1}", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        rejects(text, "key too long");
    }
    // 文档上限：缺省 1 MiB（描述文件）不变；s2a_json_parse_object_max 按调用方给的上限，超了报出实际大小
    {
        size_t big = ((size_t)1 << 20) + 16; char *doc = malloc(big); s2a_json_object o2; char e2[128] = "";
        memset(doc, ' ', big); doc[0] = '{'; doc[big - 1] = '}';
        CHECK(s2a_json_parse_object((const uint8_t *)doc, big, &o2, e2, sizeof e2) == S2A_JSON_ERROR_TOO_LARGE, "default limit stays 1 MiB");
        e2[0] = 0;
        CHECK(s2a_json_parse_object_max((const uint8_t *)doc, big, (size_t)2 << 20, &o2, e2, sizeof e2) == 0 && o2.count == 0, "explicit 2 MiB limit accepts a 1 MiB + 16 document");
        CHECK(s2a_json_parse_object_max((const uint8_t *)doc, big, (size_t)1 << 20, &o2, e2, sizeof e2) == S2A_JSON_ERROR_TOO_LARGE &&
              strstr(e2, "1048592 bytes") && strstr(e2, "1048576"), "explicit limit reports actual size and limit");
        free(doc);
    }
    if (g_failures) { printf("direct-json: %d FAILURES\n", g_failures); return 1; }
    printf("direct-json: ok\n");
    return 0;
}
