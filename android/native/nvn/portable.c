// Android 兼容实现：CommonCrypto SHA-256 与 CFDictionary 指针表，只服务共享 NVN 前端。
#include <stdlib.h>
#include <string.h>
#include "../../native/direct/sha256.h"
#include "../../native/nvn/compat/CommonCrypto/CommonDigest.h"
#include "../../native/nvn/compat/CoreFoundation/CoreFoundation.h"
#include "../../../graphics/nvn/gfx/nvn/nvn_hmap.h"   // 复用共用层的指针宽度键哈希
#if defined(S2A_ANDROID_VK_BACKEND_NVK)
#include "../../../graphics/nvn/replay/nvn_plat.h"
#endif

void CC_SHA256_Init(CC_SHA256_CTX *ctx) { s2a_sha256_init(ctx); }
void CC_SHA256_Update(CC_SHA256_CTX *ctx, const void *data, CC_LONG length) { s2a_sha256_update(ctx, data, length); }
void CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *ctx) { s2a_sha256_final(ctx, md); }
unsigned char *CC_SHA256(const void *data, CC_LONG length, unsigned char *md) {
    s2a_sha256 ctx; s2a_sha256_init(&ctx); s2a_sha256_update(&ctx, data, length); s2a_sha256_final(&ctx, md); return md;
}

// 宿主用例以键比较次数区别哈希探测与整表扫描，不引入计时或产物诊断开销。
#if defined(S2A_TEST_CF_DICT_COMPARE)
size_t s2a_test_cf_dict_comparisons;
#define CF_KEY_EQ(a, b) (s2a_test_cf_dict_comparisons++, (a) == (b))
#else
#define CF_KEY_EQ(a, b) ((a) == (b))
#endif

struct pair { const void *key, *value; };
// 保留紧凑项表和 swap-delete 的遍历顺序。哈希槽保存项下标 + 1，0 表示空槽；键和值本身可以是 NULL。
struct __CFDictionary {
    struct pair *items;
    size_t count, capacity;
    size_t *index;
    size_t index_cap;
};

// 兼容层只创建过共享前端的程序键表；前端用 g_pk_mu 串行访问，会话终止确认无调用者后释放。
// CFRelease 只释放本层生成的表，避免 Apple 专有代码路径在 Android 上误放非本层指针。
static struct __CFDictionary *g_dicts[16];
static size_t g_dict_count;

static size_t key_home(const void *key, size_t mask) {
    return (size_t)nvn_hm_hash(&key, sizeof key) & mask;
}

static size_t find_slot(const struct __CFDictionary *dict, const void *key) {
    size_t mask = dict->index_cap - 1;
    size_t slot = key_home(key, mask);
    while (dict->index[slot] && !CF_KEY_EQ(dict->items[dict->index[slot] - 1].key, key))
        slot = (slot + 1) & mask;
    return slot;
}

static int reserve(struct __CFDictionary *dict, size_t capacity, int indexed) {
    if (capacity > SIZE_MAX / sizeof *dict->items) return 0;
    size_t *new_index = NULL;
    size_t index_cap = 0;
    if (indexed) {
        index_cap = 32;
        while (index_cap / 2 < capacity) index_cap *= 2;
        if (index_cap > SIZE_MAX / sizeof *new_index) return 0;
        new_index = calloc(index_cap, sizeof *new_index);
        if (!new_index) return 0;
    }
    struct pair *items = realloc(dict->items, capacity * sizeof *items);
    if (!items) { free(new_index); return 0; }
    free(dict->index);
    dict->items = items;
    dict->capacity = capacity;
    dict->index = new_index;
    dict->index_cap = index_cap;
    if (new_index)
        for (size_t i = 0; i < dict->count; i++) new_index[find_slot(dict, items[i].key)] = i + 1;
    return 1;
}

CFMutableDictionaryRef CFDictionaryCreateMutable(const void *allocator, long capacity,
                                                 const void *keyCallBacks, const void *valueCallBacks) {
    (void)allocator; (void)keyCallBacks; (void)valueCallBacks;
    struct __CFDictionary *dict = calloc(1, sizeof *dict);
    if (!dict) return NULL;
    int indexed = 1;
#if defined(S2A_ANDROID_VK_BACKEND_NVK)
    indexed = nvn_plat_flag("nvk_replay_cpu_cuts", 1) != 0;
#endif
    if (!reserve(dict, capacity > 0 ? (size_t)capacity : 16, indexed)) { free(dict); return NULL; }
    if (g_dict_count < sizeof g_dicts / sizeof g_dicts[0]) g_dicts[g_dict_count++] = dict;
    return dict;
}

// Zero means absent. Return a 1-based item number so null keys and values remain distinct from an empty slot.
static size_t find_item(const struct __CFDictionary *dict, const void *key, size_t *slot) {
    if (dict->index) {
        *slot = find_slot(dict, key);
        return dict->index[*slot];
    }
    for (size_t i = 0; i < dict->count; i++) if (CF_KEY_EQ(dict->items[i].key, key)) return i + 1;
    return 0;
}

const void *CFDictionaryGetValue(CFMutableDictionaryRef dict, const void *key) {
    if (!dict) return NULL;
    size_t slot;
    size_t item = find_item(dict, key, &slot);
    return item ? dict->items[item - 1].value : NULL;
}

void CFDictionarySetValue(CFMutableDictionaryRef dict, const void *key, const void *value) {
    if (!dict) return;
    size_t slot = 0;
    size_t item = find_item(dict, key, &slot);
    if (item) { dict->items[item - 1].value = value; return; }
    if (dict->count == dict->capacity) {
        if (!reserve(dict, dict->capacity * 2, dict->index != NULL)) return;
        if (dict->index) slot = find_slot(dict, key);
    }
    dict->items[dict->count++] = (struct pair){key, value};
    if (dict->index) dict->index[slot] = dict->count;
}

// Fill a hole by shifting later entries whose probe path ran through it. Leave no tombstones behind.
static void erase_slot(struct __CFDictionary *dict, size_t hole) {
    size_t mask = dict->index_cap - 1;
    for (size_t slot = (hole + 1) & mask; dict->index[slot]; slot = (slot + 1) & mask) {
        size_t entry = dict->index[slot];
        size_t home = key_home(dict->items[entry - 1].key, mask);
        if (((hole - home) & mask) >= ((slot - home) & mask)) continue;
        dict->index[hole] = entry;
        hole = slot;
    }
    dict->index[hole] = 0;
}

void CFDictionaryRemoveValue(CFMutableDictionaryRef dict, const void *key) {
    if (!dict) return;
    size_t slot = 0;
    size_t item = find_item(dict, key, &slot);
    if (!item) return;
    if (dict->index) erase_slot(dict, slot);
    size_t last = --dict->count;
    if (item - 1 != last) {
        if (dict->index) dict->index[find_slot(dict, dict->items[last].key)] = item;
        dict->items[item - 1] = dict->items[last];
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
        free(dict->index); free(dict->items); free(dict);
        return;
    }
}
