// s2a-direct-compile-v1 请求生成与执行（native 侧唯一生产者）。
// 字段来源规则与 graphics/tools/direct/compiler/DirectProgram.cs / DirectShaderCompiler.cs 一一对应：
// profile/identity 来自 provider，code/control/c1 来自 SetShaders 源快照，GPU 查询事实来自 Draw 快照。
// 没有真实来源的字段不写默认值，直接记进 gaps；编译器因此会给出准确的 MISSING_FIELD。
#include "compile_request.h"
#include "kind_check.h"
#include "s2a_direct.h"
#include "sha256.h"
#include "json.h"
#include "w2_single_query.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Optional native-only producer seam. Standalone request/worker host links do
 * not need the registry or JSON parser; an absent dependency rejects diagnosis. */
struct nvk_w2_capsule;
extern int s2a_compile_export_open(void) __attribute__((weak));
extern int nvk_w2_capsule_claim(const struct s2a_compile_request_input *, struct nvk_w2_capsule **) __attribute__((weak));
extern int nvk_w2_capsule_publish(struct nvk_w2_capsule *, const struct s2a_w2_query *, const uint8_t *, size_t) __attribute__((weak));
extern void nvk_w2_capsule_abandon(struct nvk_w2_capsule *, int) __attribute__((weak));
extern void nvk_w2_capsule_returned(struct nvk_w2_capsule *) __attribute__((weak));
extern void nvk_w2_capsule_release(struct nvk_w2_capsule *) __attribute__((weak));
extern int s2a_json_parse_object(const uint8_t *, size_t, s2a_json_object *, char *, size_t) __attribute__((weak));
extern const s2a_json_field *s2a_json_get(const s2a_json_object *, const char *) __attribute__((weak));
// 弱链接[androiddraw]：s2a_compile_export_open（w2_request_worker_test「缺解析器」一例故意不链 json.c，靠弱符号为空；macOS ld64 要按 tools/weak_link_decl.py 登记放行）
// 弱链接[androiddraw]：nvk_w2_capsule_claim
// 弱链接[androiddraw]：nvk_w2_capsule_publish
// 弱链接[androiddraw]：nvk_w2_capsule_abandon
// 弱链接[androiddraw]：nvk_w2_capsule_returned
// 弱链接[androiddraw]：nvk_w2_capsule_release
// 弱链接[androiddraw]：s2a_json_parse_object
// 弱链接[androiddraw]：s2a_json_get

#define DIAG_ATTEMPTS_MAX 8u
#define DIAG_REQUEST_MAX (1u << 20)
#define DIAG_ERROR_MAX 4096u
struct s2a_compile_diag_attempt {
    atomic_uint refs, executions;
    atomic_int revoked, finished, published;
    struct nvk_w2_capsule *capsule;
    uint64_t session, generation;
    const char *original_json;
    uint8_t *json, *error;
    size_t json_len, error_len;
    int valid_error;
    struct s2a_w2_query query;
};
static atomic_uint g_diag_attempts;
/* Owning transaction, never a "most recent snapshot" selector: only the exact
 * allocation produced by this thread's build can consume it once. Production
 * draw always consumes build's result before free; arbitrary free/replay is not
 * this API's lifetime contract. Starting another build cancels the old binding. */
static pthread_key_t g_diag_pending_key;
static pthread_once_t g_diag_pending_once = PTHREAD_ONCE_INIT;
static int g_diag_pending_ready;
static _Thread_local struct {
    int active;
    struct s2a_compile_diag_attempt *attempt;
    uint64_t session, generation;
    const char *json;
    size_t len;
} g_diag_scope;

void s2a_compile_diag_retain(struct s2a_compile_diag_attempt *a) {
    if (a) atomic_fetch_add(&a->refs, 1);
}
void s2a_compile_diag_abandon(struct s2a_compile_diag_attempt *a, int reason) {
    if (!a) return;
    atomic_store(&a->revoked, 1);
    /* This producer operation is expressly atomic-only, safe under g_wmu. */
    nvk_w2_capsule_abandon(a->capsule, reason);
}
void s2a_compile_diag_release(struct s2a_compile_diag_attempt *a) {
    if (!a || atomic_fetch_sub(&a->refs, 1) != 1) return;
    nvk_w2_capsule_release(a->capsule);
    free(a->json); free(a->error); free(a);
    atomic_fetch_sub(&g_diag_attempts, 1);
}
void s2a_compile_diag_returned(struct s2a_compile_diag_attempt *a) {
    if (nvk_w2_capsule_returned)
        nvk_w2_capsule_returned(a && atomic_load(&a->published) && !atomic_load(&a->revoked) ? a->capsule : NULL);
}
static void diag_pending_destroy(void *value) {
    struct s2a_compile_diag_attempt *a = value;
    s2a_compile_diag_abandon(a, 1);
    s2a_compile_diag_release(a);
}
static void diag_pending_init(void) {
    g_diag_pending_ready = pthread_key_create(&g_diag_pending_key, diag_pending_destroy) == 0;
}
static struct s2a_compile_diag_attempt *diag_pending_detach(void) {
    if (pthread_once(&g_diag_pending_once, diag_pending_init) || !g_diag_pending_ready) return NULL;
    struct s2a_compile_diag_attempt *a = pthread_getspecific(g_diag_pending_key);
    if (a && pthread_setspecific(g_diag_pending_key, NULL)) {
        /* Keep the destructor's reference if detaching failed. Never free an
         * object while a TLS destructor can still observe its address. */
        s2a_compile_diag_abandon(a, 1); return NULL;
    }
    return a;
}
static void diag_pending_clear(void) {
    struct s2a_compile_diag_attempt *a = diag_pending_detach();
    s2a_compile_diag_returned(NULL);
    s2a_compile_diag_abandon(a, 1);
    s2a_compile_diag_release(a);
}
static void diag_bind(const struct s2a_compile_request_input *in, const char *json, size_t len) {
    if (!s2a_compile_export_open || !s2a_compile_export_open() ||
        !nvk_w2_capsule_claim || !nvk_w2_capsule_publish || !nvk_w2_capsule_abandon ||
        !nvk_w2_capsule_release || !nvk_w2_capsule_returned || !s2a_json_parse_object || !s2a_json_get ||
        !len || len > DIAG_REQUEST_MAX) return;
    if (pthread_once(&g_diag_pending_once, diag_pending_init) || !g_diag_pending_ready ||
        pthread_getspecific(g_diag_pending_key)) return;
    if (atomic_fetch_add(&g_diag_attempts, 1) >= DIAG_ATTEMPTS_MAX) {
        atomic_fetch_sub(&g_diag_attempts, 1); return;
    }
    struct s2a_compile_diag_attempt *a = calloc(1, sizeof *a);
    if (!a) { atomic_fetch_sub(&g_diag_attempts, 1); return; }
    a->json = malloc(len);
    if (!a->json) { free(a); atomic_fetch_sub(&g_diag_attempts, 1); return; }
    memcpy(a->json, json, len);
    if (nvk_w2_capsule_claim(in, &a->capsule) || !a->capsule) {
        if (a->capsule) nvk_w2_capsule_release(a->capsule);
        free(a->json); free(a); atomic_fetch_sub(&g_diag_attempts, 1); return;
    }
    atomic_init(&a->refs, 1);
    atomic_init(&a->executions, 0);
    atomic_init(&a->revoked, 0); atomic_init(&a->finished, 0); atomic_init(&a->published, 0);
    a->session = in->session; a->generation = in->generation;
    a->original_json = json; a->json_len = len;
    if (pthread_setspecific(g_diag_pending_key, a)) {
        s2a_compile_diag_abandon(a, 1); s2a_compile_diag_release(a);
    }
}
struct s2a_compile_diag_attempt *s2a_compile_diag_take(
    uint64_t session, uint64_t generation, const char *json, size_t len) {
    s2a_compile_diag_returned(NULL);
    struct s2a_compile_diag_attempt *a = diag_pending_detach();
    if (!a) return NULL;
    if (!json || json != a->original_json || len != a->json_len ||
        session != a->session || generation != a->generation ||
        !s2a_compile_export_open() || memcmp(json, a->json, len)) {
        s2a_compile_diag_abandon(a, 1); s2a_compile_diag_release(a); return NULL;
    }
    return a;
}
void s2a_compile_diag_enter(struct s2a_compile_diag_attempt *a,
    uint64_t session, uint64_t generation, const char *json, size_t len) {
    if (g_diag_scope.active) {
        s2a_compile_diag_abandon(g_diag_scope.attempt, 1);
        s2a_compile_diag_abandon(a, 1);
    }
    g_diag_scope.active = 1; g_diag_scope.attempt = a;
    g_diag_scope.session = session; g_diag_scope.generation = generation;
    g_diag_scope.json = json; g_diag_scope.len = len;
}
void s2a_compile_diag_leave(void) { memset(&g_diag_scope, 0, sizeof g_diag_scope); }

static int diag_literal(const char **p, const char *literal) {
    size_t n = strlen(literal);
    if (strncmp(*p, literal, n)) return 0;
    *p += n; return 1;
}
static int diag_number(const char **p, unsigned limit, unsigned *out) {
    const char *start = *p;
    unsigned value = 0;
    if (**p < '0' || **p > '9') return 0;
    do {
        unsigned digit = (unsigned)(**p - '0');
        if (digit > limit || value > (limit - digit) / 10) return 0;
        value = value * 10 + digit; (*p)++;
    } while (**p >= '0' && **p <= '9');
    if (*start == '0' && *p - start != 1) return 0;
    *out = value; return 1;
}
static int diag_query(const char *message, struct s2a_w2_query *q) {
    static const struct { const char *name; enum s2a_w2_query_method method; } methods[] = {
        {"QuerySamplerType", S2A_W2_QUERY_SAMPLER_TYPE},
        {"QueryTextureIsBuffer", S2A_W2_QUERY_IS_BUFFER},
        {"QueryTextureCoordNormalized", S2A_W2_QUERY_NORMALIZED},
        {"QueryTextureFormat", S2A_W2_QUERY_FORMAT}
    };
    const char *p = message;
    memset(q, 0, sizeof *q);
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        p = message;
        if (diag_literal(&p, methods[i].name)) { q->method = methods[i].method; break; }
    }
    if (!q->method || !diag_literal(&p, " has no source (stage=")) return -1;
    if (diag_literal(&p, "vs")) q->coordinate.stage = S2A_W2_QUERY_VS;
    else if (diag_literal(&p, "fs")) q->coordinate.stage = S2A_W2_QUERY_FS;
    else return -1; /* This producer is graphics-only. */
    unsigned handle, slot;
    if (!diag_literal(&p, ",handle=") || !diag_number(&p, INT_MAX, &handle) ||
        !diag_literal(&p, ",cbufSlot=")) return -1;
    if (diag_literal(&p, "-1")) q->coordinate.cbuf_slot = -1;
    else { if (!diag_number(&p, 31, &slot)) return -1; q->coordinate.cbuf_slot = (int32_t)slot; }
    q->coordinate.handle = handle;
    if (q->method == S2A_W2_QUERY_FORMAT && !strncmp(p, ",format=", 8) &&
        !diag_literal(&p, ",format=Unknown")) return -1;
    return diag_literal(&p, ")") && !*p ? 0 : -1;
}
static void diag_error(struct s2a_compile_diag_attempt *a, int32_t rc,
                       const uint8_t *bytes, size_t len, unsigned emits) {
    if (!a || atomic_load(&a->revoked) || !rc || !bytes || !len ||
        len > DIAG_ERROR_MAX || emits != 1 || a->error) return;
    a->error = malloc(len);
    if (!a->error) return;
    memcpy(a->error, bytes, len); a->error_len = len;
    s2a_json_object object;
    if (s2a_json_parse_object(a->error, len, &object, NULL, 0) || object.count != 3) return;
    const s2a_json_field *schema = s2a_json_get(&object, "schema");
    const s2a_json_field *code = s2a_json_get(&object, "code");
    const s2a_json_field *message = s2a_json_get(&object, "message");
    if (!schema || !code || !message || schema->kind != S2A_JSON_STRING ||
        code->kind != S2A_JSON_STRING || message->kind != S2A_JSON_STRING ||
        schema->text_bytes != sizeof "s2a-direct-error-v1" - 1 || strcmp(schema->text, "s2a-direct-error-v1") ||
        code->text_bytes != sizeof "MISSING_FACT" - 1 || strcmp(code->text, "MISSING_FACT") ||
        memchr(message->text, 0, message->text_bytes)) return;
    a->valid_error = diag_query(message->text, &a->query) == 0;
}
void s2a_compile_diag_finish(struct s2a_compile_diag_attempt *a, int accepted) {
    if (!a || atomic_exchange(&a->finished, 1)) return;
    if (accepted && !atomic_load(&a->revoked) && a->valid_error &&
        !nvk_w2_capsule_publish(a->capsule, &a->query, a->error, a->error_len))
        atomic_store(&a->published, 1);
    else s2a_compile_diag_abandon(a, 6);
}
static struct s2a_compile_diag_attempt *diag_execute_begin(
    uint64_t session, uint64_t generation, const char *json, size_t len, int *owned) {
    *owned = !g_diag_scope.active;
    if (*owned) {
        struct s2a_compile_diag_attempt *a = s2a_compile_diag_take(session, generation, json, len);
        if (a) atomic_fetch_add(&a->executions, 1);
        return a;
    }
    s2a_compile_diag_returned(NULL);
    struct s2a_compile_diag_attempt *a = g_diag_scope.attempt;
    if (a && (session != g_diag_scope.session || generation != g_diag_scope.generation ||
        json != g_diag_scope.json || len != g_diag_scope.len || len != a->json_len ||
        !json || session != a->session || generation != a->generation || memcmp(json, a->json, len) ||
        atomic_fetch_add(&a->executions, 1) != 0)) {
        s2a_compile_diag_abandon(a, 1); return NULL;
    }
    return a;
}
static void diag_execute_end(struct s2a_compile_diag_attempt *a, int owned, int failed) {
    if (!owned) return;
    s2a_compile_diag_finish(a, failed);
    s2a_compile_diag_returned(a);
    s2a_compile_diag_release(a);
}

struct sb {
    char *p;
    size_t n, cap;
    int oom;
};

static void sb_raw(struct sb *b, const char *s, size_t n) {
    if (b->oom) return;
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

#define GAPS_TRUNCATED "…(来源缺口清单被截断)"
static int append_gap(char *gaps, size_t cap, const char *field, const char *source) {
    if (!gaps || !cap) return 0;
    size_t used = strlen(gaps);
    if (used >= cap - 2) return -1;
    // 已截断：保留结尾标记，不再追加（避免把残缺清单当完整结果）。
    size_t mark = sizeof GAPS_TRUNCATED - 1;
    if (used >= mark && !strcmp(gaps + used - mark, GAPS_TRUNCATED)) return -1;
    if (used) { gaps[used] = ';'; gaps[used + 1] = ' '; gaps[used + 2] = 0; used += 2; }
    int n = snprintf(gaps + used, cap - used, "%s <- %s", field, source);
    if (n < 0 || (size_t)n >= cap - used) {
        size_t room = cap - used;
        if (room > mark + 1) {
            memcpy(gaps + used, GAPS_TRUNCATED, mark);
            gaps[used + mark] = 0;
        } else if (used >= 3) {
            memcpy(gaps + used - 3, "...", 3);
            gaps[used] = 0;
        }
        return -1;
    }
    return 0;
}

static void sha256_hex(const void *data, size_t len, char out[65]) {
    uint8_t digest[32];
    s2a_sha256 ctx;
    s2a_sha256_init(&ctx);
    s2a_sha256_update(&ctx, data, len);
    s2a_sha256_final(&ctx, digest);
    s2a_sha256_hex(digest, out);
}

int s2a_compile_request_build(const struct s2a_compile_request_input *in,
                              char **json, size_t *json_len, char *gaps, size_t gaps_size) {
    diag_pending_clear();
    if (!in || !json || !json_len) return -1;
    *json = NULL;
    *json_len = 0;
    if (gaps && gaps_size) gaps[0] = 0;
    if (!in->session || !in->generation) return -1;
    if (!in->profile_json || in->profile_bytes < 2 || in->profile_json[0] != '{' ||
        in->profile_json[in->profile_bytes - 1] != '}') return -1;
    if (!in->request_id || !*in->request_id || !in->translator_closure) return -1;
    if (in->stage_count < 1 || in->stage_count > 3) return -1;
    int v2 = in->stage_count == 3;
    if (v2 && (!in->stages[0].stage || strcmp(in->stages[0].stage, "vs") ||
               !in->stages[1].stage || strcmp(in->stages[1].stage, "fs") ||
               !in->stages[2].stage || strcmp(in->stages[2].stage, "gs"))) return -1;
    for (int i = 0; i < in->stage_count; i++) {
        const struct s2a_compile_stage_src *s = &in->stages[i];
        if (!s->stage || !s->code || !s->code_bytes || !s->control || !s->control_bytes) return -1;
    }

    struct sb b;
    memset(&b, 0, sizeof b);
    sb_puts(&b, "{");
    sb_key_string(&b, "schema", v2 ? "s2a-direct-compile-v2" : "s2a-direct-compile-v1");
    sb_puts(&b, ",");
    sb_key_u64(&b, "session", in->session);
    sb_puts(&b, ",");
    sb_key_u64(&b, "generation", in->generation);
    sb_puts(&b, ",");
    sb_json_string(&b, "requestId"); sb_puts(&b, ":"); sb_json_string(&b, in->request_id);
    sb_puts(&b, ",");
    sb_key_u32(&b, "directABI", (unsigned)in->direct_abi);
    sb_puts(&b, ",");
    sb_key_string(&b, "translatorClosure", in->translator_closure);
    sb_puts(&b, ",");
    // 与描述符/统一编译器常量同一值（graphics.h S2A_GRAPHICS_RUNTIME_COMPILER_ABI），不另写字面量。
    sb_key_string(&b, "runtimeCompilerABI", "s2a-direct-compile-v1");
    sb_puts(&b, ",");
    sb_key_u32(&b, "sourceParserVersion", 1);
    // 描述符种类校验的调试开关只在这里判一次（debug_gate.h 两道门 + debug.s2a.kind_check），
    // 判完随请求 JSON 带给编译器；缺省不带这个键 = 校验开着。
    // 消费端 graphics/tools/direct/compiler/DirectShaderCompiler.cs:39 只读请求 JSON 的 kindCheck
    // （JsonStrict.OptionalBool 缺省 true），不读进程环境变量——所以不能用 setenv 传这个开关。
    if (!s2a_direct_kind_check_enabled()) {
        sb_puts(&b, ",");
        sb_json_string(&b, "kindCheck"); sb_puts(&b, ":false");
    }
    sb_puts(&b, ",");
    sb_json_string(&b, "profile");
    sb_puts(&b, ":");
    // provider 合成的 enabled profile 原样带上，native 不改写、不补字段。
    sb_raw(&b, in->profile_json, in->profile_bytes);
    sb_puts(&b, ",");
    sb_json_string(&b, "program");
    sb_puts(&b, ":{\"kind\":\"graphics\",\"stages\":[");

    for (int i = 0; i < in->stage_count; i++) {
        const struct s2a_compile_stage_src *s = &in->stages[i];
        if (i) sb_puts(&b, ",");
        sb_puts(&b, "{");
        sb_key_string(&b, "stage", s->stage);
        sb_puts(&b, ",");
        sb_json_string(&b, "code"); sb_puts(&b, ":"); sb_base64(&b, s->code, s->code_bytes);
        sb_puts(&b, ",");
        sb_json_string(&b, "control"); sb_puts(&b, ":"); sb_base64(&b, s->control, s->control_bytes);
        sb_puts(&b, ",");
        sb_json_string(&b, "c1"); sb_puts(&b, ":"); sb_base64(&b, s->c1, s->c1_bytes);
        if (!s->c1 || !s->c1_bytes)
            (void)append_gap(gaps, gaps_size, "program.stages[].c1",
                             "SetShaders 控制段前 0x100 字节（仅 BRX 跳转表需要；本笔未捕获）");
        sb_puts(&b, ",");
        sb_key_u32(&b, "constantBufferUse", s->constant_buffer_use);
        sb_puts(&b, "}");
    }
    if (v2) sb_puts(&b, ",null,null");
    sb_puts(&b, "],");

    // graphicsState：只写有真实来源的字段；缺失的组逐项记进 gaps（编译器会按同一契约报 MISSING_FIELD）。
    sb_puts(&b, "\"graphicsState\":{");
    int wrote = 0;
#define GS_BOOL(name, cond, val) do { if (wrote++) sb_puts(&b, ","); sb_json_string(&b, name); sb_puts(&b, ":"); \
        sb_puts(&b, (cond) && (val) ? "true" : "false"); } while (0)
#define GS_STR(name, cond, val) do { if (wrote++) sb_puts(&b, ","); sb_key_string(&b, name, (val)); } while (0)
    // NVN 没有强制 early-Z 的入口、也没有视口变换开关：这两项由 API 面确定，不是猜的默认值。
    GS_BOOL("earlyZForce", 1, in->gfx.early_z_force);
    if (in->gfx.topology) {
        GS_STR("topology", 1, in->gfx.topology);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.topology", "Draw 快照的 NVN 绘制图元");
    }
    if (in->gfx.has_tess_mode && ((in->gfx.tess_mode & 3) <= 2) && (((in->gfx.tess_mode >> 4) & 3) <= 2)) {
        // 枚举名必须与编译器程序集的 TessPatchType / TessSpacing 一致（JsonStrict 按名字解析）。
        static const char *const patch[] = { "Isolines", "Triangles", "Quads" };
        static const char *const spacing[] = { "EqualSpacing", "FractionalEventSpacing", "FractionalOddSpacing" };
        unsigned tm = in->gfx.tess_mode;
        GS_BOOL("tessCw", 1, (tm & 0x100) != 0);
        GS_STR("tessPatchType", 1, patch[tm & 3]);
        GS_STR("tessSpacing", 1, spacing[(tm >> 4) & 3]);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.tessCw/tessPatchType/tessSpacing",
                         "控制段 +0x744 的 TessMode");
    }
    if (in->gfx.has_alpha_to_coverage) {
        GS_BOOL("alphaToCoverageEnable", 1, in->gfx.alpha_to_coverage_enable);
        GS_BOOL("alphaToCoverageDitherEnable", 1, in->gfx.alpha_to_coverage_dither);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.alphaToCoverageEnable/alphaToCoverageDitherEnable",
                         "已绑定的 NVNmultisampleState（OP_BIND_STATE kind 3）");
    }
    GS_BOOL("viewportTransformDisable", 1, in->gfx.viewport_transform_disable);
    if (in->gfx.has_depth_mode) {
        GS_BOOL("depthMode", 1, in->gfx.depth_mode);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.depthMode",
                         "nvnDeviceSetDepthMode（设备状态未同步到 Draw 快照）");
    }
    if (in->gfx.has_point_size) {
        GS_BOOL("programPointSizeEnable", 1, in->gfx.program_point_size_enable);
        if (wrote++) sb_puts(&b, ",");
        sb_json_string(&b, "pointSize"); sb_puts(&b, ":");
        { char t[32]; int n = snprintf(t, sizeof t, "%.9g", (double)in->gfx.point_size); sb_raw(&b, t, (size_t)n); }
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.programPointSizeEnable/pointSize",
                         "nvnCommandBufferSetPointSize 当前未记录");
    }
    if (in->gfx.has_alpha_test && in->gfx.alpha_test_compare) {
        GS_STR("alphaTestCompare", 1, in->gfx.alpha_test_compare);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.alphaTestCompare",
                         "已绑定的 NVNcolorState.alphaFunc（OP_BIND_STATE kind 1）");
    }
    if (in->gfx.has_alpha_test_reference) {
        if (wrote++) sb_puts(&b, ",");
        sb_json_string(&b, "alphaTestReference"); sb_puts(&b, ":");
        { char t[32]; int n = snprintf(t, sizeof t, "%.9g", (double)in->gfx.alpha_test_reference); sb_raw(&b, t, (size_t)n); }
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.alphaTestReference",
                         "未捕获 nvnCommandBufferSetAlphaRef 的真实调用（hle_nvn.c 目前只记日志）");
    }
    {
        int all = 1;
        for (int i = 0; i < 32; i++) if (!in->gfx.attribute_types[i]) all = 0;
        if (all) {
            if (wrote++) sb_puts(&b, ",");
            sb_json_string(&b, "attributeTypes"); sb_puts(&b, ":[");
            for (int i = 0; i < 32; i++) {
                if (i) sb_puts(&b, ",");
                sb_json_string(&b, in->gfx.attribute_types[i]);
            }
            sb_puts(&b, "]");
        } else {
            (void)append_gap(gaps, gaps_size, "graphicsState.attributeTypes[32]",
                             "OP_BIND_VATTR 顶点属性格式（本笔没有完整 32 槽来源）");
        }
    }
    if (in->gfx.has_cb_draw_params) {
        GS_BOOL("hasConstantBufferDrawParameters", 1, in->gfx.cb_draw_params);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.hasConstantBufferDrawParameters",
                         "设备 shaderDrawParameters 特性未进编译上下文");
    }
    {
        // 编译器（DirectProgram.cs）对图形程序要求 fragmentOutputTypes 恒为 8 项，与有没有颜色附件
        // 无关：rt_count==0 的 draw（只写 storage / depth-only）也必须给中性 Float。以前按 rt_count>0
        // 才写，导致这类程序的第一笔（常是写入 SSBO 的“编码”pass）被拒 MISSING_FIELD。
        int all = 1;
        for (int i = 0; i < 8; i++) if (!in->gfx.fragment_output_types[i]) all = 0;
        if (all) {
            if (wrote++) sb_puts(&b, ",");
            sb_json_string(&b, "fragmentOutputTypes"); sb_puts(&b, ":[");
            for (int i = 0; i < 8; i++) {
                if (i) sb_puts(&b, ",");
                sb_json_string(&b, in->gfx.fragment_output_types[i]);
            }
            sb_puts(&b, "]");
        } else {
            (void)append_gap(gaps, gaps_size, "graphicsState.fragmentOutputTypes[8]",
                             "Draw 快照颜色附件格式 → AttributeType");
        }
    }
    if (in->gfx.has_blend) {
        GS_BOOL("dualSourceBlendEnable", 1, in->gfx.dual_source_blend);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.dualSourceBlendEnable",
                         "已绑定的 NVNblendState（OP_BIND_STATE kind 0）");
    }
    if (in->gfx.has_y_negate) {
        GS_BOOL("yNegateEnabled", 1, in->gfx.y_negate);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.yNegateEnabled",
                         "nvnDeviceSetWindowOriginMode（设备状态未同步到 Draw 快照）");
    }
    if (in->gfx.has_origin) {
        GS_BOOL("originUpperLeft", 1, in->gfx.origin_upper_left);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.originUpperLeft", "目标=SPIR-V/Vulkan（可由此确定）");
    }
    if (in->gfx.has_halve_primitive_id) {
        GS_BOOL("halvePrimitiveId", 1, in->gfx.halve_primitive_id);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.halvePrimitiveId",
                         "输入拓扑 + 宿主 quad 能力 + 是否有 GS");
    }
    if (in->gfx.has_unaligned_ssbo) {
        GS_BOOL("hasUnalignedStorageBuffer", 1, in->gfx.unaligned_ssbo);
    } else {
        (void)append_gap(gaps, gaps_size, "graphicsState.hasUnalignedStorageBuffer",
                         "已绑定 SSBO 的地址对设备 minStorageBufferOffsetAlignment 的对齐（设备 limits 未提供）");
    }
    // 契约已扩到完整 32 位（此前只有 0x1F，>=5 号常量缓冲的信息会丢，导致着色器与 layout 不一致）。
    // overflow 仍上报，作为“曾经有超出旧 5 位范围”的观测项，不再当缺口。
    if (in->gfx.constant_buffer_use_overflow)
        (void)append_gap(gaps, gaps_size, "program.stages[].constantBufferUse",
                         "该阶段绑定了 >=5 号常量缓冲（契约已支持完整 32 位掩码；此项仅作观测）");
#undef GS_BOOL
#undef GS_STR
    sb_puts(&b, "},");
    sb_puts(&b, "\"textures\":[");
    if (in->texture_count < 0 || in->texture_count > S2A_COMPILE_TEXTURE_MAX) { free(b.p); return -1; }
    for (int i = 0; i < in->texture_count; i++) {
        const struct s2a_compile_texture_src *t = &in->textures[i];
        if (!t->stage || !t->format || !t->sampler_type || !t->array_length || t->cbuf_slot < -1) {
            free(b.p);
            return -1;
        }
        if (i) sb_puts(&b, ",");
        sb_puts(&b, "{");
        sb_key_string(&b, "stage", t->stage);
        sb_puts(&b, ",");
        sb_key_u32(&b, "handle", t->handle);
        sb_puts(&b, ",");
        sb_key_i32(&b, "cbufSlot", t->cbuf_slot);
        sb_puts(&b, ",");
        sb_key_string(&b, "samplerType", t->sampler_type);
        sb_puts(&b, ",");
        sb_key_string(&b, "format", t->format);
        sb_puts(&b, ",");
        sb_json_string(&b, "isBuffer"); sb_puts(&b, ":");
        sb_puts(&b, t->is_buffer ? "true" : "false");
        sb_puts(&b, ",");
        sb_json_string(&b, "normalized"); sb_puts(&b, ":");
        sb_puts(&b, t->normalized ? "true" : "false");
        sb_puts(&b, ",");
        sb_json_string(&b, "separate"); sb_puts(&b, ":");
        sb_puts(&b, t->separate ? "true" : "false");
        sb_puts(&b, ",");
        sb_key_u32(&b, "arrayLength", t->array_length);
        sb_puts(&b, "}");
    }
    if (!in->texture_count)
        (void)append_gap(gaps, gaps_size, "program.textures[]",
                         "各 stage 真实绑定 handle + texture/view 描述的 SamplerType/Format（本笔没有可映射来源）");
    sb_puts(&b, "]");
    if (in->sampler_array_length) {
        sb_puts(&b, ",");
        sb_key_u32(&b, "samplerArrayLength", in->sampler_array_length);
    } else {
        (void)append_gap(gaps, gaps_size, "program.samplerArrayLength", "NVN 采样器描述符池容量");
    }
    if (in->texture_array_length) {
        sb_puts(&b, ",");
        sb_key_u32(&b, "textureArrayLength", in->texture_array_length);
    } else {
        (void)append_gap(gaps, gaps_size, "program.textureArrayLength", "NVN 纹理描述符池容量");
    }
    if (in->handle_table_entries) {
        sb_puts(&b, ",");
        sb_json_string(&b, "textureBufferLengths");
        sb_puts(&b, ":{");
        sb_key_u32(&b, "2", in->handle_table_entries);
        sb_puts(&b, "}");
    }
    sb_puts(&b, ",");
    sb_json_string(&b, "transformFeedbackEnabled");
    sb_puts(&b, ":");
    sb_puts(&b, in->transform_feedback_enabled ? "true" : "false");
    sb_puts(&b, "}}");

    if (b.oom) { free(b.p); return -1; }
    *json = b.p;
    *json_len = b.n;
    diag_bind(in, b.p, b.n);
    return 0;
}

struct compile_capture {
    uint8_t *bytes;
    size_t len;
    unsigned emits;
};

static int32_t emit_copy(void *sink, s2a_direct_bytes result) {
    struct compile_capture *c = (struct compile_capture *)sink;
    if (c && c->emits < UINT_MAX) c->emits++;
    if (!c || !result.data || !result.bytes) return S2A_DIRECT_RES_ARGUMENT;
    if (result.bytes > (256ull << 20)) return S2A_DIRECT_RES_ARGUMENT;
    // +1：结果契约不要求 NUL，摘要扫描按自有长度用，但补一个终止符避免任何越界读。
    uint8_t *copy = (uint8_t *)malloc((size_t)result.bytes + 1);
    if (!copy) return S2A_DIRECT_RES_INTERNAL;
    memcpy(copy, result.data, (size_t)result.bytes);
    copy[result.bytes] = 0;
    free(c->bytes);
    c->bytes = copy;
    c->len = (size_t)result.bytes;
    return 0;
}

// 结果里只读身份/内容摘要；不解析、不改写编译器产物。
static void summarize(const uint8_t *bytes, size_t len, char *out, size_t cap) {
    out[0] = 0;
    if (!bytes || len < 2) return;
    const char *text = (const char *)bytes;
    const char *schema = strstr(text, "\"schema\":\"");
    const char *hash = strstr(text, "\"contentHash\":\"");
    int stages = 0;
    for (const char *p = text; (p = strstr(p, "\"stage\":\"")) != NULL; p += 9) stages++;
    const char *transport = strstr(text, "\"transport\":");
    const char *transport_end = transport ? strchr(transport, '}') : NULL;
    long tlen = transport && transport_end ? (long)(transport_end - transport) : 0;
    char schema_buf[32] = "?", hash_buf[80] = "?";
    if (schema) {
        const char *end = strchr(schema + 10, '"');
        size_t n = end ? (size_t)(end - (schema + 10)) : 0;
        if (n && n < sizeof schema_buf) memcpy(schema_buf, schema + 10, n);
    }
    if (hash) {
        const char *end = strchr(hash + 15, '"');
        size_t n = end ? (size_t)(end - (hash + 15)) : 0;
        if (n && n < sizeof hash_buf) memcpy(hash_buf, hash + 15, n);
    }
    snprintf(out, cap, "schema=%s contentHash=%s stages=%d transport=%.*s",
             schema_buf, hash_buf, stages,
             tlen > 0 && tlen < 160 ? (int)tlen : 0, transport ? transport : "");
}

int s2a_compile_request_execute(uint64_t session, uint64_t generation, const char *json, size_t json_len,
                                struct s2a_compile_outcome *out) {
    int diag_owned = 0;
    struct s2a_compile_diag_attempt *diag = diag_execute_begin(session, generation, json, json_len, &diag_owned);
    if (!out) { diag_execute_end(diag, diag_owned, 0); return -1; }
    memset(out, 0, sizeof *out);
    s2a_direct_compile_fn compile = s2a_direct_bound_compile_any();
    if (!compile) {
        out->rc = S2A_DIRECT_RES_UNSUPPORTED;
        snprintf(out->summary, sizeof out->summary, "统一编译入口未绑定（s2a_direct_bound_compile_any=NULL）");
        diag_execute_end(diag, diag_owned, 0);
        return -1;
    }
    char req_sha[65];
    sha256_hex(json, json_len, req_sha);
    struct compile_capture capture;
    memset(&capture, 0, sizeof capture);
    // 种类校验开关不再在这里读环境变量/属性：请求 JSON 的 kindCheck 字段由 s2a_compile_request_build
    // 按调试门判一次写进去（C# 从请求读），这里只负责原样把请求交给编译器。
    s2a_direct_bytes request = { (const uint8_t *)json, (uint64_t)json_len };
    out->rc = compile(session, generation, request, emit_copy, &capture);
    diag_error(diag, out->rc, capture.bytes, capture.len, capture.emits);
    out->bytes = capture.bytes;
    out->bytes_len = capture.len;
    if (out->rc != 0 || !capture.bytes || !capture.len) {
        // 编译失败时，C# 侧会用同一个 emit 通道回一段 s2a-direct-error-v1（ABI 只有状态码）。
        // 有就把 code/message 落进摘要：否则日志里只剩 rc=-1，无法判断是校验拦截还是翻译器问题。
        char reason[320] = "";
        if (out->rc != 0 && capture.bytes && capture.len) {
            const char *txt = (const char *)capture.bytes;
            size_t n = capture.len < sizeof reason - 1 ? capture.len : sizeof reason - 1;
            size_t k = 0;
            for (size_t i = 0; i < n && k < sizeof reason - 1; i++) {
                unsigned char ch = (unsigned char)txt[i];
                reason[k++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
            }
            reason[k] = 0;
        }
        snprintf(out->summary, sizeof out->summary,
                 "compile_fn rc=%d 无结果（请求 %zu B sha256=%s）%s%s", out->rc, json_len, req_sha,
                 reason[0] ? " 原因=" : "", reason);
        free(capture.bytes);
        out->bytes = NULL;
        out->bytes_len = 0;
        diag_execute_end(diag, diag_owned, 1);
        return out->rc ? out->rc : -1;
    }
    char summary[256];
    summarize(capture.bytes, capture.len, summary, sizeof summary);
    snprintf(out->summary, sizeof out->summary, "compile_fn ok 请求=%zuB sha256=%s 结果=%zuB %.200s",
             json_len, req_sha, capture.len, summary);
    diag_execute_end(diag, diag_owned, 0);
    return 0;
}

void s2a_compile_outcome_free(struct s2a_compile_outcome *out) {
    if (!out) return;
    free(out->bytes);
    out->bytes = NULL;
    out->bytes_len = 0;
}
