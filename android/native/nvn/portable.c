// Android 兼容实现：CommonCrypto SHA-256 与 CFDictionary 指针表，只服务共享 NVN 前端。
#include <stdlib.h>
#include <string.h>
#include "../../native/direct/sha256.h"
#include "../../native/nvn/compat/CommonCrypto/CommonDigest.h"
#include "../../native/nvn/compat/CoreFoundation/CoreFoundation.h"
#include "nvn_hot_cnt.h"

void CC_SHA256_Init(CC_SHA256_CTX *ctx) { s2a_sha256_init(ctx); }
void CC_SHA256_Update(CC_SHA256_CTX *ctx, const void *data, CC_LONG length) { s2a_sha256_update(ctx, data, length); }
void CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *ctx) { s2a_sha256_final(ctx, md); }
unsigned char *CC_SHA256(const void *data, CC_LONG length, unsigned char *md) {
    s2a_sha256 ctx; s2a_sha256_init(&ctx); s2a_sha256_update(&ctx, data, length); s2a_sha256_final(&ctx, md); return md;
}

struct pair { const void *key, *value; };
struct __CFDictionary { struct pair *items; size_t count, capacity; const void *last_key; int has_last; };

// 兼容层只创建过共享前端的程序键表；CFRelease 只释放本层生成的表，其余对象保持空操作，
// 避免 Apple 专有代码路径在 Android 上误放非本层指针。
static struct __CFDictionary *g_dicts[16];
static size_t g_dict_count;

CFMutableDictionaryRef CFDictionaryCreateMutable(const void *allocator, long capacity,
                                                 const void *keyCallBacks, const void *valueCallBacks) {
    (void)allocator; (void)keyCallBacks; (void)valueCallBacks;
    struct __CFDictionary *dict = calloc(1, sizeof *dict);
    if (!dict) return NULL;
    dict->capacity = capacity > 0 ? (size_t)capacity : 16;
    dict->items = calloc(dict->capacity, sizeof *dict->items);
    if (!dict->items) { free(dict); return NULL; }
    if (g_dict_count < sizeof g_dicts / sizeof g_dicts[0]) g_dicts[g_dict_count++] = dict;
    return dict;
}
// 热点计数 ②（nvn_hot_cnt.h，开关由后端每 300 帧传进来，缺省关）：每次查找比较了几次键、命中与否、是否与这张表上次查的键相同。
// 键在表里不会重复（SetValue 同键替换），所以「重复键率」量的是查找序列里连续同键的比例——一格备忘能接住的那部分。
// 调用方（hle_nvn.c 的 pk_get 等）查表时持着自己的锁；计数用原子加，上屏线程取走。
static int g_hot; static uint64_t g_hot_lookups, g_hot_cmp, g_hot_max, g_hot_hits, g_hot_same;
// 键数一直记（只在增删时加减，比查找少得多）：取读数时不去碰表本身——表可能正被 CFRelease 释放。g_keys_max = 本局单张表到过的最多键数。
static uint64_t g_keys, g_keys_max;
void s2a_cfdict_hot_enable(int on) { __atomic_store_n(&g_hot, on ? 1 : 0, __ATOMIC_RELAXED); }
static void hot_note(struct __CFDictionary *dict, const void *key, uint64_t cmp, int hit) {
    __atomic_fetch_add(&g_hot_lookups, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_hot_cmp, cmp, __ATOMIC_RELAXED);
    if (hit) __atomic_fetch_add(&g_hot_hits, 1, __ATOMIC_RELAXED);
    if (dict->has_last && dict->last_key == key) __atomic_fetch_add(&g_hot_same, 1, __ATOMIC_RELAXED);
    dict->last_key = key; dict->has_last = 1;
    uint64_t m = __atomic_load_n(&g_hot_max, __ATOMIC_RELAXED);
    while (cmp > m && !__atomic_compare_exchange_n(&g_hot_max, &m, cmp, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
const void *CFDictionaryGetValue(CFMutableDictionaryRef dict, const void *key) {
    if (!dict) return NULL;
    for (size_t i = 0; i < dict->count; i++) if (dict->items[i].key == key) {
        if (__atomic_load_n(&g_hot, __ATOMIC_RELAXED)) hot_note(dict, key, i + 1, 1);
        return dict->items[i].value;
    }
    if (__atomic_load_n(&g_hot, __ATOMIC_RELAXED)) hot_note(dict, key, dict->count, 0);
    return NULL;
}
void CFDictionarySetValue(CFMutableDictionaryRef dict, const void *key, const void *value) {
    if (!dict) return;
    for (size_t i = 0; i < dict->count; i++)
        if (dict->items[i].key == key) { dict->items[i].value = value; return; }
    if (dict->count == dict->capacity) {
        size_t next = dict->capacity * 2;
        struct pair *grown = realloc(dict->items, next * sizeof *grown);
        if (!grown) return;
        dict->items = grown; dict->capacity = next;
    }
    dict->items[dict->count++] = (struct pair){key, value};
    __atomic_fetch_add(&g_keys, 1, __ATOMIC_RELAXED);
    uint64_t m = __atomic_load_n(&g_keys_max, __ATOMIC_RELAXED);
    while (dict->count > m && !__atomic_compare_exchange_n(&g_keys_max, &m, (uint64_t)dict->count, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef dict, const void *key) {
    if (!dict) return;
    for (size_t i = 0; i < dict->count; i++) {
        if (dict->items[i].key == key) {
            dict->items[i] = dict->items[--dict->count];
            __atomic_fetch_sub(&g_keys, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

long CFDictionaryGetCount(CFMutableDictionaryRef dict) { return dict ? (long)dict->count : 0; }
void CFDictionaryApplyFunction(CFMutableDictionaryRef dict, CFDictionaryApplierFunction applier, void *context) {
    if (!dict || !applier) return;
    for (size_t i = 0; i < dict->count; i++) applier(dict->items[i].key, dict->items[i].value, context);
}
void CFRelease(const void *object) {
    if (!object) return;
    struct __CFDictionary *dict = (struct __CFDictionary *)object;
    for (size_t i = 0; i < g_dict_count; i++) {
        if (g_dicts[i] != dict) continue;
        g_dicts[i] = g_dicts[--g_dict_count];
        __atomic_fetch_sub(&g_keys, (uint64_t)dict->count, __ATOMIC_RELAXED);
        free(dict->items); free(dict);
        return;
    }
}
// 取走这一窗的查找计数（增量）；表数、键数取这一刻，键数最大值是本局到过的最大（不清零）。
void s2a_cfdict_hot_take(struct s2a_cfdict_hot *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->lookups = __atomic_exchange_n(&g_hot_lookups, 0, __ATOMIC_RELAXED); out->compares = __atomic_exchange_n(&g_hot_cmp, 0, __ATOMIC_RELAXED);
    out->max_cmp = __atomic_exchange_n(&g_hot_max, 0, __ATOMIC_RELAXED); out->hits = __atomic_exchange_n(&g_hot_hits, 0, __ATOMIC_RELAXED);
    out->same_key = __atomic_exchange_n(&g_hot_same, 0, __ATOMIC_RELAXED);
    out->tables = (uint32_t)__atomic_load_n(&g_dict_count, __ATOMIC_RELAXED);
    out->keys = (uint32_t)__atomic_load_n(&g_keys, __ATOMIC_RELAXED); out->keys_max = (uint32_t)__atomic_load_n(&g_keys_max, __ATOMIC_RELAXED);
}
