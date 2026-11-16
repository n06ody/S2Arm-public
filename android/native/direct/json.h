// 严格 JSON 子集读取器（C1 图形描述 / C2 编译协议共用）。
// 首版只接受：UTF-8、RFC 8259 对象/数组/字符串/有限数值/true/false/null。
// 拒绝：非法 UTF-8、孤立代理对、控制字符、重复顶层键、尾随字节、注释、NaN/Inf、超深嵌套、超长输入。
// 嵌套对象只做完整语法校验；消费其字段的一方（描述解析/编译协议）必须另行检查该层的重复键。
// 解析结果只记录顶层字段的原始跨度与解码后的短字符串；不建动态 DOM、不分配堆内存。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2A_JSON_MAX_BYTES (1u << 20)   // 单个文档 1 MiB：与契约 §4.3 的限额分开，描述文件远小于此
#define S2A_JSON_MAX_DEPTH 16
#define S2A_JSON_MAX_FIELDS 64
#define S2A_JSON_MAX_SHORT 256

enum s2a_json_kind {
    S2A_JSON_STRING = 1,
    S2A_JSON_NUMBER = 2,
    S2A_JSON_OBJECT = 3,
    S2A_JSON_ARRAY = 4,
    S2A_JSON_TRUE = 5,
    S2A_JSON_FALSE = 6,
    S2A_JSON_NULL = 7,
};

typedef struct {
    char key[64];
    uint32_t kind;
    const uint8_t *raw;      // 值在原文中的跨度（含引号/括号）
    uint32_t raw_bytes;
    char text[S2A_JSON_MAX_SHORT]; // STRING 的解码结果；NUMBER 保留原文
    uint32_t text_bytes;
    double number;           // NUMBER 且有限
    uint32_t is_unsigned;    // NUMBER 是非负整数
    uint64_t unsigned_value;
} s2a_json_field;

typedef struct {
    s2a_json_field fields[S2A_JSON_MAX_FIELDS];
    uint32_t count;
} s2a_json_object;

enum s2a_json_error {
    S2A_JSON_OK = 0,
    S2A_JSON_ERROR_EMPTY = -1,
    S2A_JSON_ERROR_TOO_LARGE = -2,
    S2A_JSON_ERROR_SYNTAX = -3,
    S2A_JSON_ERROR_UTF8 = -4,
    S2A_JSON_ERROR_DUPLICATE_KEY = -5,
    S2A_JSON_ERROR_TOKEN_LIMIT = -6,
    S2A_JSON_ERROR_NUMBER = -7,
    S2A_JSON_ERROR_STRING = -8,
    S2A_JSON_ERROR_TRAILING = -9,
};

// 解析顶层对象。成功返回 S2A_JSON_OK；失败时 out 不可用、err 写入 ASCII 原因。
// 同 s2a_json_parse_object，只是文档上限由调用方给（编译结果这种「程序」级文档，不是描述文件）。
int s2a_json_parse_object_max(const uint8_t *data, size_t bytes, size_t max_bytes, s2a_json_object *out,
                              char *err, size_t err_size);
int s2a_json_parse_object(const uint8_t *data, size_t bytes, s2a_json_object *out,
                          char *err, size_t err_size);

// 取字段；缺失返回 NULL。重复键在解析阶段已拒绝。
const s2a_json_field *s2a_json_get(const s2a_json_object *object, const char *key);

// 顶层数组字段的元素个数与第 index 个元素的原始 JSON 跨度（不含首尾空白）。
// 元素内容仍要由消费方按自己的 schema 解析；这里只做边界切分，不改变“已完整验证语法”的前提。
uint32_t s2a_json_array_count(const s2a_json_field *field);
int s2a_json_array_span(const s2a_json_field *field, uint32_t index,
                        const uint8_t **data, uint32_t *bytes);

// Sequential iterator: initialize *cursor to 1; returns 1 for an element,
// 0 at end, -1 for invalid arguments. The original object is already validated.
int s2a_json_array_next(const s2a_json_field *field, uint32_t *cursor,
                        const uint8_t **data, uint32_t *bytes);

#ifdef __cplusplus
}
#endif
