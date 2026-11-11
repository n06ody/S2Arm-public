// 跨度式 JSON 读取器（见 jv.h）。原实现内嵌在 nvn_vk_program_result.c，#1434 原样抽出共用。
#include "jv.h"
#include <stdlib.h>
#include <string.h>

const char *s2a_jv_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

int s2a_jv_scan(const char *p, const char *end, const char **out_end) {
    p = s2a_jv_ws(p, end);
    if (p >= end) return -1;
    if (*p == '"') {
        p++;
        while (p < end) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') { *out_end = p + 1; return 0; }
            p++;
        }
        return -1;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                p++;
                while (p < end && *p != '"') { if (*p == '\\') p++; p++; }
                if (p < end) p++;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) { depth--; if (!depth) { *out_end = p + 1; return 0; } }
            p++;
        }
        return -1;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    *out_end = p;
    return 0;
}

int s2a_jv_is_object(struct s2a_jv v) { return v.n >= 2 && v.p[0] == '{' && v.p[v.n - 1] == '}'; }
int s2a_jv_is_array(struct s2a_jv v) { return v.n >= 2 && v.p[0] == '[' && v.p[v.n - 1] == ']'; }
int s2a_jv_is_null(struct s2a_jv v) { return v.n == 4 && !memcmp(v.p, "null", 4); }
int s2a_jv_is_string(struct s2a_jv v) { return v.n >= 2 && v.p[0] == '"'; }

int s2a_jv_str(struct s2a_jv v, char *out, size_t cap) {
    if (!s2a_jv_is_string(v) || cap == 0) return -1;
    size_t n = 0;
    for (size_t i = 1; i + 1 < v.n; i++) {
        char c = v.p[i];
        if (c == '\\' && i + 2 < v.n) {
            char e = v.p[++i];
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
    out[n] = 0;
    return 0;
}

int s2a_jv_u32(struct s2a_jv v, uint32_t *out) {
    char buf[24];
    if (v.n == 0 || v.n >= sizeof buf) return -1;
    memcpy(buf, v.p, v.n); buf[v.n] = 0;
    char *end = NULL;
    unsigned long value = strtoul(buf, &end, 10);
    if (!end || *end || end == buf) return -1;
    *out = (uint32_t)value;
    return 0;
}

int s2a_jv_bool(struct s2a_jv v, int *out) {
    if (v.n == 4 && !memcmp(v.p, "true", 4)) { *out = 1; return 0; }
    if (v.n == 5 && !memcmp(v.p, "false", 5)) { *out = 0; return 0; }
    return -1;
}

int s2a_jv_obj_find(struct s2a_jv obj, const char *key, struct s2a_jv *out) {
    if (!s2a_jv_is_object(obj)) return -1;
    const char *p = obj.p + 1, *end = obj.p + obj.n - 1;
    while (p < end) {
        p = s2a_jv_ws(p, end);
        if (p >= end || *p == '}') break;
        struct s2a_jv k;
        const char *kend = NULL;
        if (*p != '"' || s2a_jv_scan(p, end, &kend)) return -1;
        k.p = p; k.n = (size_t)(kend - p);
        char name[64];
        if (s2a_jv_str(k, name, sizeof name)) return -1;
        p = s2a_jv_ws(kend, end);
        if (p >= end || *p != ':') return -1;
        const char *vstart = NULL, *vend = NULL;
        if (s2a_jv_scan(p + 1, end, &vend)) return -1;
        vstart = s2a_jv_ws(p + 1, end);
        if (!strcmp(name, key)) { out->p = vstart; out->n = (size_t)(vend - vstart); return 0; }
        p = s2a_jv_ws(vend, end);
        if (p < end && *p == ',') p++;
    }
    return -1;
}

int s2a_jv_arr_count(struct s2a_jv arr) {
    if (!s2a_jv_is_array(arr)) return -1;
    const char *p = arr.p + 1, *end = arr.p + arr.n - 1;
    int n = 0;
    while (p < end) {
        p = s2a_jv_ws(p, end);
        if (p >= end || *p == ']') break;
        const char *vend = NULL;
        if (s2a_jv_scan(p, end, &vend)) return -1;
        n++;
        p = s2a_jv_ws(vend, end);
        if (p < end && *p == ',') p++;
    }
    return n;
}

int s2a_jv_arr_item(struct s2a_jv arr, int index, struct s2a_jv *out) {
    if (!s2a_jv_is_array(arr) || index < 0) return -1;
    const char *p = arr.p + 1, *end = arr.p + arr.n - 1;
    int n = 0;
    while (p < end) {
        p = s2a_jv_ws(p, end);
        if (p >= end || *p == ']') break;
        const char *vend = NULL;
        if (s2a_jv_scan(p, end, &vend)) return -1;
        if (n++ == index) { out->p = p; out->n = (size_t)(vend - p); return 0; }
        p = s2a_jv_ws(vend, end);
        if (p < end && *p == ',') p++;
    }
    return -1;
}
