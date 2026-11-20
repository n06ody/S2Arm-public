// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "../native/nvn/compat/CoreFoundation/CoreFoundation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "CFDictionary: %s (line %d)\n", #e, __LINE__); exit(1); } } while (0)
extern size_t s2a_test_cf_dict_comparisons;
static int use_hash = 1;
int nvn_plat_flag(const char *name, int fallback) {
    return !strcmp(name, "nvk_replay_cpu_cuts") ? use_hash : fallback;
}

enum { KEYS = 4096, DELETED = 1024 };
static _Alignas(64) unsigned char keys[KEYS * 2][64];
static unsigned char values[2][KEYS];
static const void *expect_key[KEYS + 1], *expect_value[KEYS + 1];
static unsigned enumerated;

static void visit(const void *key, const void *value, void *context) {
    CHECK(context == &enumerated);
    CHECK(enumerated <= KEYS && key == expect_key[enumerated] && value == expect_value[enumerated]);
    enumerated++;
}

int main(int argc, char **argv) {
    int expect_fast = argc == 1 || strcmp(argv[1], "off");
    use_hash = argc == 1;
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 3, NULL, NULL);
    CHECK(dict && CFDictionaryGetCount(dict) == 0);
    for (unsigned i = 0; i < KEYS; i++) {
        expect_key[i] = keys[i];
        expect_value[i] = &values[0][i];
        CFDictionarySetValue(dict, expect_key[i], expect_value[i]);
    }
    // The shim has always supported pointer equality, including NULL key/value.
    CFDictionarySetValue(dict, NULL, NULL);
    CHECK(CFDictionaryGetCount(dict) == KEYS + 1);
    expect_value[KEYS] = &values[1][0];
    CFDictionarySetValue(dict, NULL, expect_value[KEYS]);
    CHECK(CFDictionaryGetValue(dict, NULL) == expect_value[KEYS]);

    s2a_test_cf_dict_comparisons = 0;
    for (unsigned i = 0; i < KEYS; i++) {
        CHECK(CFDictionaryGetValue(dict, keys[i]) == &values[0][i]);
        CHECK(CFDictionaryGetValue(dict, keys[i + KEYS]) == NULL);
    }
    size_t probes = s2a_test_cf_dict_comparisons;

    unsigned live = KEYS + 1;
    for (unsigned i = 0; i < DELETED; i++) {
        CFDictionaryRemoveValue(dict, expect_key[i]);
        CHECK(CFDictionaryGetValue(dict, expect_key[i]) == NULL);
        expect_key[i] = expect_key[--live];
        expect_value[i] = expect_value[live];
    }
    CFDictionaryRemoveValue(dict, keys[KEYS + 1]);
    for (unsigned i = 0; i < live; i++) CHECK(CFDictionaryGetValue(dict, expect_key[i]) == expect_value[i]);
    for (unsigned i = 0; i < DELETED; i++) {
        expect_key[live] = keys[KEYS + i];
        expect_value[live] = &values[1][i];
        CFDictionarySetValue(dict, expect_key[live], expect_value[live]);
        live++;
    }
    CFDictionaryApplyFunction(dict, visit, &enumerated);
    CHECK(enumerated == live && CFDictionaryGetCount(dict) == (long)live);
    CFRelease(dict);
    CFRelease(values);   // 异源对象沿用空操作
    printf("CFDictionary: hits/misses=%d comparisons=%zu live=%u iteration=ok\n", KEYS * 2, probes, live);
    if (expect_fast && probes > KEYS * 16u) {
        fprintf(stderr, "CFDictionary: lookup exceeded eight comparisons per key on average\n");
        return 1;
    }
    if (!expect_fast) CHECK(probes > KEYS * 16u);
    return 0;
}
