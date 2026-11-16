// 严格 JSON 子集读取器实现，见 json.h。
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size, pos;
    s2a_json_object *out;
    char *err;
    size_t err_size;
    int failed;
} parser;

static int fail(parser *p, int code, const char *why) {
    if (!p->failed && p->err && p->err_size)
        snprintf(p->err, p->err_size, "%s at byte %zu", why, p->pos);
    p->failed = 1;
    return code;
}

static void skip_ws(parser *p) {
    while (p->pos < p->size) {
        uint8_t c = p->data[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

// 严格 UTF-8：拒绝 overlong、代理区、> U+10FFFF、截断。返回序列长度，0 = 非法。
static unsigned utf8_seq(const uint8_t *s, size_t left, uint32_t *cp) {
    if (!left) return 0;
    uint8_t c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) {
        if (left < 2 || (s[1] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (left < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;                 // overlong
        if (c == 0xED && s[1] >= 0xA0) return 0;                // UTF-16 代理区
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (left < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;                 // overlong
        if (c == 0xF4 && s[1] > 0x8F) return 0;                 // > U+10FFFF
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
            | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 0;
}

static int put_utf8(char *dst, size_t cap, size_t *used, uint32_t cp) {
    uint8_t bytes[4];
    unsigned n;
    if (cp < 0x80) { bytes[0] = (uint8_t)cp; n = 1; }
    else if (cp < 0x800) { bytes[0] = (uint8_t)(0xC0 | (cp >> 6)); bytes[1] = (uint8_t)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        bytes[0] = (uint8_t)(0xE0 | (cp >> 12)); bytes[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (uint8_t)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        bytes[0] = (uint8_t)(0xF0 | (cp >> 18)); bytes[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); bytes[3] = (uint8_t)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (!dst) return 0;
    if (*used + n > cap) return -1;
    memcpy(dst + *used, bytes, n);
    *used += n;
    return 0;
}

// 解析字符串（p->pos 指向开引号），验证并可选解码到 dst/cap。
static int parse_string(parser *p, char *dst, size_t cap, size_t *out_bytes) {
    if (p->pos >= p->size || p->data[p->pos] != '"') return fail(p, S2A_JSON_ERROR_STRING, "expected string");
    p->pos++;
    size_t used = 0;
    while (p->pos < p->size) {
        uint8_t c = p->data[p->pos++];
        if (c == '"') {
            if (dst) {
                if (used >= cap) return fail(p, S2A_JSON_ERROR_STRING, "string too long");
                dst[used] = 0;
            }
            if (out_bytes) *out_bytes = (uint32_t)used;
            return S2A_JSON_OK;
        }
        if (c < 0x20) return fail(p, S2A_JSON_ERROR_STRING, "raw control character");
        if (c == '\\') {
            if (p->pos >= p->size) return fail(p, S2A_JSON_ERROR_STRING, "truncated escape");
            uint8_t e = p->data[p->pos++];
            uint32_t cp;
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = 8; break;
            case 'f': cp = 12; break;
            case 'n': cp = 10; break;
            case 'r': cp = 13; break;
            case 't': cp = 9; break;
            case 'u': {
                if (p->pos + 4 > p->size) return fail(p, S2A_JSON_ERROR_STRING, "truncated unicode escape");
                uint32_t unit = 0;
                for (int i = 0; i < 4; i++) {
                    uint8_t h = p->data[p->pos++];
                    unit <<= 4;
                    if (h >= '0' && h <= '9') unit |= (uint32_t)(h - '0');
                    else if (h >= 'a' && h <= 'f') unit |= (uint32_t)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') unit |= (uint32_t)(h - 'A' + 10);
                    else return fail(p, S2A_JSON_ERROR_STRING, "bad unicode escape");
                }
                if (unit >= 0xD800 && unit <= 0xDBFF) {
                    if (p->pos + 6 > p->size || p->data[p->pos] != '\\' || p->data[p->pos + 1] != 'u')
                        return fail(p, S2A_JSON_ERROR_STRING, "unpaired surrogate");
                    p->pos += 2;
                    uint32_t low = 0;
                    for (int i = 0; i < 4; i++) {
                        uint8_t h = p->data[p->pos++];
                        low <<= 4;
                        if (h >= '0' && h <= '9') low |= (uint32_t)(h - '0');
                        else if (h >= 'a' && h <= 'f') low |= (uint32_t)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') low |= (uint32_t)(h - 'A' + 10);
                        else return fail(p, S2A_JSON_ERROR_STRING, "bad low surrogate");
                    }
                    if (low < 0xDC00 || low > 0xDFFF) return fail(p, S2A_JSON_ERROR_STRING, "unpaired surrogate");
                    cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                    return fail(p, S2A_JSON_ERROR_STRING, "unpaired surrogate");
                } else cp = unit;
                break;
            }
            default: return fail(p, S2A_JSON_ERROR_STRING, "unknown escape");
            }
            if (put_utf8(dst, cap, &used, cp)) return fail(p, S2A_JSON_ERROR_STRING, "string too long");
            continue;
        }
        if (c < 0x80) {
            if (dst) {
                if (used + 1 > cap) return fail(p, S2A_JSON_ERROR_STRING, "string too long");
                dst[used] = (char)c;
            }
            used++;
            continue;
        }
        // 多字节序列：回到序列起点做严格校验。
        size_t start = --p->pos;
        uint32_t cp;
        unsigned n = utf8_seq(p->data + start, p->size - start, &cp);
        if (!n) return fail(p, S2A_JSON_ERROR_UTF8, "invalid UTF-8");
        p->pos = start + n;
        if (put_utf8(dst, cap, &used, cp)) return fail(p, S2A_JSON_ERROR_STRING, "string too long");
    }
    return fail(p, S2A_JSON_ERROR_STRING, "unterminated string");
}

static int parse_number(parser *p, double *value, uint32_t *is_unsigned, uint64_t *uvalue) {
    size_t start = p->pos;
    int negative = 0;
    if (p->pos < p->size && p->data[p->pos] == '-') { negative = 1; p->pos++; }
    if (p->pos >= p->size) return fail(p, S2A_JSON_ERROR_NUMBER, "number");
    if (p->data[p->pos] == '0') {
        p->pos++;
        if (p->pos < p->size && p->data[p->pos] >= '0' && p->data[p->pos] <= '9')
            return fail(p, S2A_JSON_ERROR_NUMBER, "leading zero");
    } else if (p->data[p->pos] >= '1' && p->data[p->pos] <= '9') {
        while (p->pos < p->size && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') p->pos++;
    } else return fail(p, S2A_JSON_ERROR_NUMBER, "number");
    int integer = 1;
    if (p->pos < p->size && p->data[p->pos] == '.') {
        integer = 0;
        p->pos++;
        if (p->pos >= p->size || p->data[p->pos] < '0' || p->data[p->pos] > '9')
            return fail(p, S2A_JSON_ERROR_NUMBER, "fraction");
        while (p->pos < p->size && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->size && (p->data[p->pos] == 'e' || p->data[p->pos] == 'E')) {
        integer = 0;
        p->pos++;
        if (p->pos < p->size && (p->data[p->pos] == '+' || p->data[p->pos] == '-')) p->pos++;
        if (p->pos >= p->size || p->data[p->pos] < '0' || p->data[p->pos] > '9')
            return fail(p, S2A_JSON_ERROR_NUMBER, "exponent");
        while (p->pos < p->size && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') p->pos++;
    }
    size_t length = p->pos - start;
    char buffer[64];
    if (length >= sizeof buffer) return fail(p, S2A_JSON_ERROR_NUMBER, "number too long");
    memcpy(buffer, p->data + start, length);
    buffer[length] = 0;
    char *end = NULL;
    double parsed = strtod(buffer, &end);
    if (!end || *end || !isfinite(parsed)) return fail(p, S2A_JSON_ERROR_NUMBER, "non-finite number");
    *value = parsed;
    *is_unsigned = integer && !negative;
    if (*is_unsigned) {
        uint64_t number = strtoull(buffer, &end, 10);
        if (!end || *end) return fail(p, S2A_JSON_ERROR_NUMBER, "integer");
        *uvalue = number;
    } else *uvalue = 0;
    return S2A_JSON_OK;
}

static int parse_value(parser *p, int depth, s2a_json_field *field);

static int parse_object_nested(parser *p, int depth) {
    if (depth >= S2A_JSON_MAX_DEPTH) return fail(p, S2A_JSON_ERROR_SYNTAX, "nesting too deep");
    p->pos++; // {
    skip_ws(p);
    if (p->pos < p->size && p->data[p->pos] == '}') { p->pos++; return S2A_JSON_OK; }
    for (;;) {
        skip_ws(p);
        if (parse_string(p, NULL, 0, NULL)) return S2A_JSON_ERROR_STRING;
        skip_ws(p);
        if (p->pos >= p->size || p->data[p->pos] != ':') return fail(p, S2A_JSON_ERROR_SYNTAX, "expected colon");
        p->pos++;
        skip_ws(p);
        if (parse_value(p, depth + 1, NULL)) return S2A_JSON_ERROR_SYNTAX;
        skip_ws(p);
        if (p->pos >= p->size) return fail(p, S2A_JSON_ERROR_SYNTAX, "unterminated object");
        if (p->data[p->pos] == ',') { p->pos++; continue; }
        if (p->data[p->pos] == '}') { p->pos++; return S2A_JSON_OK; }
        return fail(p, S2A_JSON_ERROR_SYNTAX, "expected comma");
    }
}

static int parse_array_nested(parser *p, int depth) {
    if (depth >= S2A_JSON_MAX_DEPTH) return fail(p, S2A_JSON_ERROR_SYNTAX, "nesting too deep");
    p->pos++; // [
    skip_ws(p);
    if (p->pos < p->size && p->data[p->pos] == ']') { p->pos++; return S2A_JSON_OK; }
    for (;;) {
        skip_ws(p);
        if (parse_value(p, depth + 1, NULL)) return S2A_JSON_ERROR_SYNTAX;
        skip_ws(p);
        if (p->pos >= p->size) return fail(p, S2A_JSON_ERROR_SYNTAX, "unterminated array");
        if (p->data[p->pos] == ',') { p->pos++; continue; }
        if (p->data[p->pos] == ']') { p->pos++; return S2A_JSON_OK; }
        return fail(p, S2A_JSON_ERROR_SYNTAX, "expected comma");
    }
}

static int parse_value(parser *p, int depth, s2a_json_field *field) {
    if (depth >= S2A_JSON_MAX_DEPTH) return fail(p, S2A_JSON_ERROR_SYNTAX, "nesting too deep");
    if (p->pos >= p->size) return fail(p, S2A_JSON_ERROR_SYNTAX, "unexpected end");
    size_t start = p->pos;
    uint8_t c = p->data[p->pos];
    int kind;
    if (c == '"') {
        size_t used = 0;
        if (parse_string(p, field ? field->text : NULL,
                         field ? S2A_JSON_MAX_SHORT : 0, &used)) return S2A_JSON_ERROR_STRING;
        kind = S2A_JSON_STRING;
        if (field) field->text_bytes = (uint32_t)used;
    } else if (c == '{') {
        if (parse_object_nested(p, depth)) return S2A_JSON_ERROR_SYNTAX;
        kind = S2A_JSON_OBJECT;
    } else if (c == '[') {
        if (parse_array_nested(p, depth)) return S2A_JSON_ERROR_SYNTAX;
        kind = S2A_JSON_ARRAY;
    } else if (c == 't' && p->size - p->pos >= 4 && !memcmp(p->data + p->pos, "true", 4)) {
        p->pos += 4; kind = S2A_JSON_TRUE;
    } else if (c == 'f' && p->size - p->pos >= 5 && !memcmp(p->data + p->pos, "false", 5)) {
        p->pos += 5; kind = S2A_JSON_FALSE;
    } else if (c == 'n' && p->size - p->pos >= 4 && !memcmp(p->data + p->pos, "null", 4)) {
        p->pos += 4; kind = S2A_JSON_NULL;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        // parse_number 成功时三项都会写；先初始化是为了在 -O1 -Werror=maybe-uninitialized
        // 的宿主编译器（GCC 15）下不误报——调用方只读返回值 0 之后的字段。
        double number = 0;
        uint32_t is_unsigned = 0;
        uint64_t uvalue = 0;
        if (parse_number(p, &number, &is_unsigned, &uvalue)) return S2A_JSON_ERROR_NUMBER;
        kind = S2A_JSON_NUMBER;
        if (field) {
            field->number = number;
            field->is_unsigned = is_unsigned;
            field->unsigned_value = uvalue;
        }
    } else return fail(p, S2A_JSON_ERROR_SYNTAX, "unexpected value");
    if (field) {
        field->kind = (uint32_t)kind;
        field->raw = p->data + start;
        field->raw_bytes = (uint32_t)(p->pos - start);
    }
    return S2A_JSON_OK;
}

int s2a_json_parse_object(const uint8_t *data, size_t bytes, s2a_json_object *out,
                          char *err, size_t err_size) {
    return s2a_json_parse_object_max(data, bytes, S2A_JSON_MAX_BYTES, out, err, err_size);
}
int s2a_json_parse_object_max(const uint8_t *data, size_t bytes, size_t max_bytes, s2a_json_object *out,
                              char *err, size_t err_size) {
    if (err && err_size) err[0] = 0;
    if (!data || !out) {
        if (err && err_size) snprintf(err, err_size, "null input");
        return S2A_JSON_ERROR_EMPTY;
    }
    if (!bytes) {
        if (err && err_size) snprintf(err, err_size, "empty input");
        return S2A_JSON_ERROR_EMPTY;
    }
    if (bytes > max_bytes) {
        if (err && err_size) snprintf(err, err_size, "document of %zu bytes larger than %zu", bytes, max_bytes);
        return S2A_JSON_ERROR_TOO_LARGE;
    }
    parser p = { data, bytes, 0, out, err, err_size, 0 };
    memset(out, 0, sizeof *out);
    skip_ws(&p);
    if (p.pos >= p.size || p.data[p.pos] != '{') {
        fail(&p, S2A_JSON_ERROR_SYNTAX, "top-level object required");
        return S2A_JSON_ERROR_SYNTAX;
    }
    p.pos++;
    skip_ws(&p);
    if (p.pos < p.size && p.data[p.pos] == '}') p.pos++;
    else {
        for (;;) {
            skip_ws(&p);
            if (out->count == S2A_JSON_MAX_FIELDS) {
                fail(&p, S2A_JSON_ERROR_TOKEN_LIMIT, "too many top-level fields");
                return S2A_JSON_ERROR_TOKEN_LIMIT;
            }
            s2a_json_field *field = &out->fields[out->count];
            memset(field, 0, sizeof *field);
            size_t key_bytes = 0;
            if (parse_string(&p, field->key, sizeof field->key, &key_bytes)) return p.failed ? S2A_JSON_ERROR_STRING : S2A_JSON_ERROR_STRING;
            skip_ws(&p);
            if (p.pos >= p.size || p.data[p.pos] != ':') {
                fail(&p, S2A_JSON_ERROR_SYNTAX, "expected colon");
                return S2A_JSON_ERROR_SYNTAX;
            }
            p.pos++;
            skip_ws(&p);
            if (parse_value(&p, 1, field)) return S2A_JSON_ERROR_SYNTAX;
            for (uint32_t i = 0; i < out->count; i++) {
                if (!strcmp(out->fields[i].key, field->key)) {
                    fail(&p, S2A_JSON_ERROR_DUPLICATE_KEY, "duplicate key");
                    return S2A_JSON_ERROR_DUPLICATE_KEY;
                }
            }
            out->count++;
            skip_ws(&p);
            if (p.pos >= p.size) { fail(&p, S2A_JSON_ERROR_SYNTAX, "unterminated object"); return S2A_JSON_ERROR_SYNTAX; }
            if (p.data[p.pos] == ',') { p.pos++; continue; }
            if (p.data[p.pos] == '}') { p.pos++; break; }
            fail(&p, S2A_JSON_ERROR_SYNTAX, "expected comma");
            return S2A_JSON_ERROR_SYNTAX;
        }
    }
    skip_ws(&p);
    if (p.pos != p.size) {
        fail(&p, S2A_JSON_ERROR_TRAILING, "trailing bytes");
        return S2A_JSON_ERROR_TRAILING;
    }
    return S2A_JSON_OK;
}

const s2a_json_field *s2a_json_get(const s2a_json_object *object, const char *key) {
    if (!object || !key) return NULL;
    for (uint32_t i = 0; i < object->count; i++)
        if (!strcmp(object->fields[i].key, key)) return &object->fields[i];
    return NULL;
}

uint32_t s2a_json_array_count(const s2a_json_field *field) {
    if (!field || field->kind != S2A_JSON_ARRAY || field->raw_bytes < 2) return 0;
    uint32_t count = 0;
    int depth = 0, in_string = 0, escape = 0;
    for (uint32_t i = 1; i + 1 < field->raw_bytes; i++) {
        uint8_t c = field->raw[i];
        if (in_string) {
            if (escape) escape = 0;
            else if (c == '\\') escape = 1;
            else if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == '[' || c == '{') { depth++; continue; }
        if (c == ']' || c == '}') { if (depth) depth--; continue; }
        if (c == ',' && depth == 0) { count++; continue; }
    }
    // 空数组返回 0；否则逗号数 + 1。逐项确认至少有一个非空白 token。
    int any = 0;
    for (uint32_t i = 1; i + 1 < field->raw_bytes && !any; i++)
        if (field->raw[i] != ' ' && field->raw[i] != '\t' && field->raw[i] != '\n' && field->raw[i] != '\r') any = 1;
    return any ? count + 1 : 0;
}

int s2a_json_array_next(const s2a_json_field *field, uint32_t *cursor,
                        const uint8_t **data, uint32_t *bytes) {
    if (!field || field->kind != S2A_JSON_ARRAY || field->raw_bytes < 2 ||
        !cursor || !data || !bytes || *cursor < 1 || *cursor >= field->raw_bytes) return -1;
    uint32_t end = field->raw_bytes - 1, i = *cursor;
    while (i < end && (field->raw[i] == ' ' || field->raw[i] == '\t' ||
                       field->raw[i] == '\n' || field->raw[i] == '\r')) i++;
    if (i == end) { *cursor = end; return 0; }
    uint32_t start = i; int depth = 0, in_string = 0, escape = 0;
    for (; i < end; i++) {
        uint8_t c = field->raw[i];
        if (in_string) {
            if (escape) escape = 0;
            else if (c == '\\') escape = 1;
            else if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == '[' || c == '{') { depth++; continue; }
        if (c == ']' || c == '}') { depth--; continue; }
        if (c == ',' && depth == 0) break;
    }
    uint32_t last = i;
    while (last > start && (field->raw[last - 1] == ' ' || field->raw[last - 1] == '\t' ||
                            field->raw[last - 1] == '\n' || field->raw[last - 1] == '\r')) last--;
    if (last == start) return -1;
    *data = field->raw + start; *bytes = last - start;
    *cursor = i < end ? i + 1 : end;
    return 1;
}

int s2a_json_array_span(const s2a_json_field *field, uint32_t index,
                        const uint8_t **data, uint32_t *bytes) {
    if (!field || field->kind != S2A_JSON_ARRAY || !data || !bytes) return -1;
    uint32_t count = s2a_json_array_count(field);
    if (index >= count) return -1;
    uint32_t current = 0, start = UINT32_MAX;
    int depth = 0, in_string = 0, escape = 0;
    for (uint32_t i = 1; i + 1 < field->raw_bytes; i++) {
        uint8_t c = field->raw[i];
        if (in_string) {
            if (escape) escape = 0;
            else if (c == '\\') escape = 1;
            else if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; if (start == UINT32_MAX) start = i; continue; }
        if (c == '[' || c == '{') { if (depth == 0 && start == UINT32_MAX) start = i; depth++; continue; }
        if (c == ']' || c == '}') { if (depth) depth--; continue; }
        if (c == ',' && depth == 0) {
            if (current == index) {
                uint32_t end = i;
                while (end > start && (field->raw[end - 1] == ' ' || field->raw[end - 1] == '\t' ||
                                       field->raw[end - 1] == '\n' || field->raw[end - 1] == '\r')) end--;
                *data = field->raw + start;
                *bytes = end - start;
                return 0;
            }
            current++;
            start = UINT32_MAX;
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && start == UINT32_MAX) start = i;
    }
    if (current == index && start != UINT32_MAX) {
        uint32_t end = field->raw_bytes - 1;
        while (end > start && (field->raw[end - 1] == ' ' || field->raw[end - 1] == '\t' ||
                               field->raw[end - 1] == '\n' || field->raw[end - 1] == '\r')) end--;
        *data = field->raw + start;
        *bytes = end - start;
        return 0;
    }
    return -1;
}
