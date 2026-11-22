// 配方级在途表（nvn_pipe_flight，纯逻辑）宿主单测：把外部复核 P1-2 的两条语义钉住。
//   ① 同配方并发需求合并成一份创建（建者只一个，等待者拿到同一对象）；
//   ② 不同配方互不排队（A 建到一半挂着时，B 必须能立刻成为建者）；
//   ③ 建者失败时等待者拿到同一错误码与原文（不重试、不返回假成功）；
//   ④ 表满（128 条在途）时第 129 个配方如实返回 0（这一次不合并、自己建）；
//   ⑤ 认领后复检命中计数（note_late_hit）。
// 负对照：把 nvk_pf_enter 的「同配方命中合并」去掉（永远新占一格）→ ① 必须变红。
//
// 编译：cc -std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE
//        -Iandroid/native/nvn android/tests/nvn_pipe_flight_test.c android/native/nvn/nvn_pipe_flight.c -o build/t
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvn_pipe_flight.h"

typedef struct { int key; } pkey_t;   // 不用 key_t：<sys/types.h> 已占用这个名字
static int key_eq(const void *a, const void *b) { return ((const pkey_t *)a)->key == ((const pkey_t *)b)->key; }
static int key_eq_big(const void *a, const void *b) { return !memcmp(a, b, 2360); }

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ① 同配方 4 线程：建者只一个，其余等待并拿到同一对象
static nvk_pf g_t;
static volatile int g_builds;
static void *same_worker(void *arg) {
    (void)arg;
    pkey_t k = {.key = 7};
    int h = -1;
    int role = nvk_pf_enter(&g_t, &k, key_eq, &h);
    if (role == 1) {
        __sync_fetch_and_add(&g_builds, 1);
        usleep(30000);                       // 模拟建管线耗时
        nvk_pf_publish(&g_t, h, (void *)0xabc0, 0, NULL);
        return NULL;
    }
    if (role == 2) {
        void *obj = NULL; char why[NVK_PF_WHY];
        int code = nvk_pf_wait(&g_t, h, &obj, why, sizeof why);
        if (code != 0 || obj != (void *)0xabc0) { fprintf(stderr, "waiter got code=%d obj=%p\n", code, obj); g_fail++; }
        return NULL;
    }
    fprintf(stderr, "unexpected role %d\n", role); g_fail++;
    return NULL;
}
static void case_merge(void) {
    nvk_pf_init(&g_t, sizeof(pkey_t));
    g_builds = 0;
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, same_worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(g_builds == 1, "same recipe: exactly one build, got %d", g_builds);
    uint64_t w = 0, f = 0, l = 0, p = 0, fl = 0;
    nvk_pf_stats(&g_t, &w, &f, &l, &p, &fl);
    CHECK(w == 3, "same recipe: three merged waits, got %llu", (unsigned long long)w);
    CHECK(p == 1 && fl == 0, "same recipe: one publish, no failure");
}

// ② 不同配方互不排队：A 建着（未发布）时，B enter 必须马上拿到「建者」身份
static volatile int g_a_in;
static void *a_owner(void *arg) {
    (void)arg;
    pkey_t k = {.key = 1};
    int h = -1;
    int role = nvk_pf_enter(&g_t, &k, key_eq, &h);
    if (role != 1) { fprintf(stderr, "A role=%d\n", role); g_fail++; return NULL; }
    g_a_in = 1;
    usleep(120000);                          // A 在建设备对象
    nvk_pf_publish(&g_t, h, (void *)0xa000, 0, NULL);
    return NULL;
}
static void case_independent(void) {
    nvk_pf_init(&g_t, sizeof(pkey_t));
    g_a_in = 0;
    pthread_t a; pthread_create(&a, NULL, a_owner, NULL);
    while (!g_a_in) usleep(1000);
    pkey_t kB = {.key = 2};
    int h = -1;
    int role = nvk_pf_enter(&g_t, &kB, key_eq, &h);
    CHECK(role == 1, "different recipe must not queue behind A (role=%d, want 1=owner)", role);
    if (role == 1) nvk_pf_publish(&g_t, h, (void *)0xb000, 0, NULL);
    pthread_join(a, NULL);
}

// ③ 失败传播
static void case_failure(void) {
    nvk_pf_init(&g_t, sizeof(pkey_t));
    g_a_in = 0;
    pkey_t k = {.key = 3};
    int h1 = -1, h2 = -1;
    CHECK(nvk_pf_enter(&g_t, &k, key_eq, &h1) == 1, "failure: first is owner");
    CHECK(nvk_pf_enter(&g_t, &k, key_eq, &h2) == 2, "failure: second waits");
    nvk_pf_publish(&g_t, h1, NULL, -7, "vkCreateGraphicsPipelines 失败");
    void *obj = (void *)1; char why[NVK_PF_WHY] = "";
    int code = nvk_pf_wait(&g_t, h2, &obj, why, sizeof why);
    CHECK(code == -7 && obj == NULL, "failure: waiter gets the same code (%d)", code);
    CHECK(!strcmp(why, "vkCreateGraphicsPipelines 失败"), "failure: waiter gets the same reason (%s)", why);
    uint64_t w, f, l, p, fl;
    nvk_pf_stats(&g_t, &w, &f, &l, &p, &fl);
    CHECK(fl == 1, "failure counted once");
}

// ⑦ 空对象不能当成功交付：建者错误发布 (NULL,0) → 等待者必须拿到具名失败而不是 NULL+OK
//   （#1774：等待侧拿 NULL 管线往下走，成员偏移解引即崩——返回码必须拦住空对象）
static void case_empty_publish(void) {
    nvk_pf_init(&g_t, sizeof(pkey_t));
    pkey_t k = {.key = 42};
    int h1 = -1, h2 = -1;
    CHECK(nvk_pf_enter(&g_t, &k, key_eq, &h1) == 1, "empty: owner");
    CHECK(nvk_pf_enter(&g_t, &k, key_eq, &h2) == 2, "empty: waiter");
    nvk_pf_publish(&g_t, h1, NULL, 0, NULL);   // 错误用法：成功码配空对象
    void *obj = (void *)1; char why[NVK_PF_WHY] = "";
    int code = nvk_pf_wait(&g_t, h2, &obj, why, sizeof why);
    CHECK(code != 0 && obj == NULL, "empty: (NULL,0) publish must not hand out a null object as success (code=%d)", code);
    CHECK(why[0], "empty: a named reason is attached");
}

// ④ 表满
static void case_full(void) {
    nvk_pf_init(&g_t, sizeof(pkey_t));
    int handles[NVK_PF_MAX];
    for (int i = 0; i < NVK_PF_MAX; i++) {
        pkey_t k = {.key = 1000 + i};
        CHECK(nvk_pf_enter(&g_t, &k, key_eq, &handles[i]) == 1, "full: recipe %d claims a slot", i);
    }
    pkey_t kx = {.key = 9999};
    int h = -1;
    CHECK(nvk_pf_enter(&g_t, &kx, key_eq, &h) == 0, "full: the 129th recipe must not merge (returns 0)");
    uint64_t w, f, l, p, fl;
    nvk_pf_stats(&g_t, &w, &f, &l, &p, &fl);
    CHECK(f == 1, "full: counted once, got %llu", (unsigned long long)f);
    for (int i = 0; i < NVK_PF_MAX; i++) nvk_pf_publish(&g_t, handles[i], (void *)0x1, 0, NULL);
}

// ⑤ late hit 计数
static void case_late_hit(void) {
    nvk_pf_init(&g_t, sizeof(pkey_t));
    nvk_pf_note_late_hit(&g_t);
    nvk_pf_note_late_hit(&g_t);
    uint64_t w, f, l, p, fl;
    nvk_pf_stats(&g_t, &w, &f, &l, &p, &fl);
    CHECK(l == 2, "late-hit counted, got %llu", (unsigned long long)l);
}

// ⑥ 键上限必须容得下真实描述（否则整表会静默停用，P1-2 静默失效）：用一个 2360 字节的键走一遍。
static void case_big_key(void) {
    static uint8_t big[NVK_PF_KEY_MAX];
    struct bigkey { uint8_t b[2360]; } k1, k2;
    memset(&k1, 0x5a, sizeof k1); memset(&k2, 0x5a, sizeof k2);
    (void)big;
    nvk_pf_init(&g_t, sizeof k1);
    int h1 = -1, h2 = -1;
    CHECK(nvk_pf_enter(&g_t, &k1, key_eq_big, &h1) == 1, "2360-byte key: owner claims a slot (key max must fit it)");
    CHECK(nvk_pf_enter(&g_t, &k2, key_eq_big, &h2) == 2, "2360-byte key: identical key merges instead of disabling the table");
    nvk_pf_publish(&g_t, h1, (void *)0x1, 0, NULL);
    void *obj = NULL; char why[NVK_PF_WHY];
    CHECK(nvk_pf_wait(&g_t, h2, &obj, why, sizeof why) == 0 && obj == (void *)0x1, "2360-byte key: waiter gets the object");
}

int main(void) {
    case_big_key();
    case_merge();
    case_independent();
    case_failure();
    case_empty_publish();
    case_full();
    case_late_hit();
    printf("%s：%d 项断言，%d 失败\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
