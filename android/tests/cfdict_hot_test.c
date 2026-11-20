// 热点计数 ②：portable.c 的 CFDictionary 兼容表查找计数（nvn_hot_cnt.h）。
// 关着不记；开着时每次查找的比较次数（命中 = 下标 + 1，没中 = 表里键数）、命中、与同表上次同键、窗口内最多比较次数、
// 表数与键数（增删与释放都跟着变）各自对上；取走后清零（键数、表数取这一刻，键数最大值不清）。
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "compat/CoreFoundation/CoreFoundation.h"
#include "nvn_hot_cnt.h"
static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static void expect(const char *what, uint64_t lookups, uint64_t cmp, uint64_t maxc, uint64_t hits, uint64_t same, uint32_t tables, uint32_t keys, uint32_t kmax) {
    struct s2a_cfdict_hot h; s2a_cfdict_hot_take(&h);
    CHECK(h.lookups == lookups && h.compares == cmp && h.max_cmp == maxc && h.hits == hits && h.same_key == same &&
          h.tables == tables && h.keys == keys && h.keys_max == kmax,
          "%s: lookups %llu cmp %llu max %llu hits %llu same %llu tables %u keys %u kmax %u; want %llu %llu %llu %llu %llu %u %u %u", what,
          (unsigned long long)h.lookups, (unsigned long long)h.compares, (unsigned long long)h.max_cmp, (unsigned long long)h.hits, (unsigned long long)h.same_key,
          h.tables, h.keys, h.keys_max, (unsigned long long)lookups, (unsigned long long)cmp, (unsigned long long)maxc, (unsigned long long)hits,
          (unsigned long long)same, tables, keys, kmax);
}
int main(void) {
    static int k[8], v[8];
    CFMutableDictionaryRef a = CFDictionaryCreateMutable(NULL, 0, NULL, NULL), b = CFDictionaryCreateMutable(NULL, 2, NULL, NULL);
    for (int i = 0; i < 5; i++) CFDictionarySetValue(a, &k[i], &v[i]);
    CFDictionarySetValue(a, &k[2], &v[7]);   // 同键替换：键数不涨
    CFDictionarySetValue(b, &k[0], &v[0]);
    // 关着：查找照常，不记
    CHECK(CFDictionaryGetValue(a, &k[4]) == &v[4] && CFDictionaryGetValue(a, &k[2]) == &v[7], "lookup semantics unchanged while off");
    expect("off", 0, 0, 0, 0, 0, 2, 6, 5);
    s2a_cfdict_hot_enable(1);
    CHECK(CFDictionaryGetValue(a, &k[0]) == &v[0], "hit first");          // 比较 1
    CHECK(CFDictionaryGetValue(a, &k[0]) == &v[0], "hit first again");    // 比较 1，同键
    CHECK(CFDictionaryGetValue(a, &k[4]) == &v[4], "hit last");           // 比较 5
    CHECK(CFDictionaryGetValue(a, &k[6]) == NULL, "miss");                // 比较 5（键数）
    CHECK(CFDictionaryGetValue(b, &k[6]) == NULL, "miss other table");    // 比较 1；b 的上一次不是 a 的
    CHECK(CFDictionaryGetValue(b, &k[6]) == NULL, "miss same key");       // 比较 1，同键
    expect("on", 6, 14, 5, 3, 2, 2, 6, 5);
    expect("taken", 0, 0, 0, 0, 0, 2, 6, 5);
    CFDictionaryRemoveValue(a, &k[1]);
    CHECK(CFDictionaryGetValue(a, &k[3]) == &v[3], "after remove");       // 删 k1 时末项 k4 补到下标 1：k3 在下标 3，比较 4
    expect("remove", 1, 4, 4, 1, 0, 2, 5, 5);
    CFRelease(a);
    expect("release", 0, 0, 0, 0, 0, 1, 1, 5);
    for (int i = 1; i < 8; i++) CFDictionarySetValue(b, &k[i], &v[i]);   // b 长到 8 键，超过初始容量 2
    expect("grow", 0, 0, 0, 0, 0, 1, 8, 8);
    s2a_cfdict_hot_enable(0);
    CHECK(CFDictionaryGetValue(b, &k[7]) == &v[7], "lookup after disable");
    expect("off again", 0, 0, 0, 0, 0, 1, 8, 8);
    CFRelease(b);
    printf("cfdict hot: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
