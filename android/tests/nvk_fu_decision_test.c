// C4a 首用等待的决策环宿主测试：真实 compile_worker（单槽/去重/完成环）+ 真实
// nvn_first_use 判定表 + 内联的生产 nvk_fu_compile。假编译器可控延迟/失败。
// CPU 合成输入，不证明设备/GPU 行为。
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include "direct/s2a_direct.h"

/* ---- 属性桩：debug.s2a.* 读表（生产侧静态缓存，首轮读定后不改） ---- */
static struct { const char *name; char value[16]; } g_props[8];
int __system_property_get(const char *name, char *value) {
    value[0] = 0;
    for (unsigned i = 0; i < sizeof g_props / sizeof g_props[0]; i++)
        if (g_props[i].name && !strcmp(g_props[i].name, name)) {
            snprintf(value, PROP_VALUE_MAX, "%s", g_props[i].value);
            return (int)strlen(value);
        }
    return 0;
}
static void prop_set(const char *name, const char *value) {
    for (unsigned i = 0; i < sizeof g_props / sizeof g_props[0]; i++)
        if (!g_props[i].name) { g_props[i].name = name; snprintf(g_props[i].value, sizeof g_props[i].value, "%s", value); return; }
    assert(0 && "prop table full");
}
int __android_log_print(int level, const char *tag, const char *fmt, ...) {
    (void)level; (void)tag; (void)fmt; return 0;
}
int __android_log_vprint(int level, const char *tag, const char *fmt, va_list ap) {
    (void)level; (void)tag; (void)fmt; (void)ap; return 0;
}

/* 录制帧序号（生产定义在 nvn_vk_first_draw.c，宿主侧给一个可设的答案）。
   weak 声明必须先于定义，否则 TU 内联的生产 extern weak 声明与之冲突。 */
extern uint64_t nvn_vk_first_draw_frame_id(void) __attribute__((weak));
static uint64_t g_frame_no = 42;
uint64_t nvn_vk_first_draw_frame_id(void) { return g_frame_no; }

#include "nvn/nvn_vk_draw.c"

/* ---- 边界替身：与 w2_observation_draw_test.c 同一套口径——只挡 GPU/provider/存储
   边界；compile_worker/nvn_first_use/请求构建全部用生产实现。 ---- */
void s2a_source_log_line(const char *line) { (void)line; }
int s2a_nvn_depth_mode = 1, s2a_nvn_window_origin_mode = 1;
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) { (void)out; return -1; }
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return NULL; }
int s2a_offline_library_fetch(const char *root, const struct s2a_offline_request *q,
    struct s2a_offline_program *out, char *why, size_t cap) {
    (void)root; (void)q; (void)out; snprintf(why, cap, "host stub"); return S2A_OFFLINE_MISS;
}
int s2a_program_cache_load(const char *root, const uint8_t identity[32],
    uint8_t **bytes, size_t *len, uint8_t sha[32], char *why, size_t cap) {
    (void)root; (void)identity; (void)bytes; (void)len; (void)sha;
    snprintf(why, cap, "host stub"); return -1;
}
int s2a_program_cache_store_async(const char *root, const uint8_t id[32],
    const uint8_t *bytes, size_t len) { (void)root; (void)id; (void)bytes; (void)len; return 0; }
uint64_t s2a_program_cache_stores(void) { return 0; }
uint64_t s2a_program_cache_failures(void) { return 0; }
int s2a_direct_diag_write(const char *root, const char *name, const void *bytes,
    size_t len, char *why, size_t cap) {
    (void)root; (void)name; (void)bytes; (void)len; (void)why; (void)cap; return -1;
}
/* pipe_worker 只被 longframe 归因统计引用；桩掉它就不必链 vkCreate* 族。 */
void nvk_pipeline_long_stats(uint64_t *n, uint64_t *fail, uint64_t *ns, uint64_t *max) {
    *n = *fail = *ns = *max = 0;
}
/* first_draw 整条 GPU 消费路径不进本测试；桩掉避免拉进 first_draw.c。 */
int nvn_vk_first_draw(struct s2a_vk_device *device, const struct s2a_record_draw_state *state,
    const uint8_t *program, size_t len, const uint8_t sha[32], const uint8_t identity[32],
    uint64_t session, uint64_t generation, const char *request_id, char *why, size_t cap) {
    (void)device; (void)state; (void)program; (void)len; (void)sha; (void)identity;
    (void)session; (void)generation; (void)request_id;
    snprintf(why, cap, "host stub"); return -1;
}
int nvn_vk_first_draw_program_valid(const uint8_t *p, size_t n, char *why, size_t cap) {
    (void)p; (void)n; (void)why; (void)cap; return 0;
}
/* W2 观察钩子：nvk_draw_builder 的外层才用，本测试直驱 nvk_fu_compile。 */
void s2a_w2_shader_begin(struct s2a_w2_shader *o, enum s2a_w2_kind kind) { (void)o; (void)kind; }
void s2a_w2_shader_end(struct s2a_w2_shader *o, int ok) { (void)o; (void)ok; }
void s2a_w2_shader_lookup(const struct s2a_direct_compile_context *c, const uint8_t id[32],
    enum s2a_w2_source source, int ok) { (void)c; (void)id; (void)source; (void)ok; }

/* ---- 可控假编译器 ---- */
static int g_delay_ms = 600, g_fail = 0;
static _Atomic int g_calls;
static int fake_fn(void *ctx, uint64_t session, uint64_t generation, const char *json, size_t len,
                   uint8_t **bytes, size_t *bytes_len, char *why, size_t why_size) {
    (void)ctx; (void)session; (void)generation;
    atomic_fetch_add(&g_calls, 1);
    usleep((unsigned)g_delay_ms * 1000);
    if (g_fail) { snprintf(why, why_size, "fake compiler: refused"); return -1; }
    *bytes = (uint8_t *)malloc(len ? len : 1);
    if (*bytes) { memcpy(*bytes, json, len); *bytes_len = len; return 0; }
    return -1;
}

static int fails, checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void sha_of(const char *s, uint8_t out[32]) {
    memset(out, 0, 32);
    memcpy(out, s, strlen(s) < 32 ? strlen(s) : 32);
}

/* draw 快照：一个可跳过（无 storage 写、目标 fresh+steady）或不可跳过的最小事实集。 */
static void mk_draw_state(struct s2a_record_draw_state *ds, int safe) {
    memset(ds, 0, sizeof *ds);
    ds->rt_count = 1;
    ds->rt[0].obj_serial = 7;
    ds->rt[0].storage = 0xAAAA;
    ds->rt[0].nvn_format = 0x2d;
    if (!safe) ds->ssbo_bound[0] = 1;
}

static uint64_t mono_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void) {
    alarm(30);
    /* iOS 同默认值的缩微档：item 30 / frame 100 / unsafe 60（ms）。 */
    prop_set("debug.s2a.fu_wait", "1");
    prop_set("debug.s2a.fu_item_ms", "30");
    prop_set("debug.s2a.fu_frame_ms", "100");
    prop_set("debug.s2a.fu_unsafe_ms", "60");

    /* rt 证据：serial 7 在帧 40/41 画过（近 2 帧 steady），帧 42 清过（fresh）。 */
    s2a_fu_rt_reset();
    s2a_fu_rt_mark_seen(7, 0xAAAA, 0x2d, 40);
    s2a_fu_rt_mark_seen(7, 0xAAAA, 0x2d, 41);
    s2a_fu_rt_mark_fresh(7, 42);

    char why[512] = "";
    CHECK(s2a_compile_worker_start(fake_fn, NULL, why, sizeof why) == 0, "worker starts");

    struct s2a_record_draw_state safe_ds, unsafe_ds;
    mk_draw_state(&safe_ds, 1);
    mk_draw_state(&unsafe_ds, 0);
    CHECK(nvk_fu_skip_safe(&safe_ds, 42) == 1, "no-storage draw on fresh+steady target is skippable");
    CHECK(nvk_fu_skip_safe(&unsafe_ds, 42) == 0, "an SSBO-bound draw is never skippable");

    uint8_t *bytes = NULL; size_t blen = 0; int skipped = 0;
    uint8_t sha[10][32];
    const char *names[10] = { "shaA", "shaB", "shaC", "shaD", "shaE", "shaF", "shaG", "shaH", "shaI", "shaJ" };
    for (int i = 0; i < 10; i++) sha_of(names[i], sha[i]);
    const char json[] = "{\"k\":1}";

    /* 排水助手：让 600ms 假编译落地、槽位空出（也验证在途结果真实到达）。 */
    #define DRAIN(s_) do { \
        int drc = s2a_compile_worker_wait_async((s_), 3000, &bytes, &blen, why, sizeof why); \
        CHECK(drc == 0, "in-flight compile actually lands after the skipped draw"); \
        free(bytes); bytes = NULL; } while (0)

    /* c1：可跳绘制 + 在途 → 按 item 上限等 30ms → LATE(1)，写 late 项标。 */
    uint64_t t0 = mono_ns();
    int rc = nvk_fu_compile(&safe_ds, 1, 1, json, sizeof json - 1, sha[0], &bytes, &blen, why, sizeof why, &skipped);
    uint64_t dt = mono_ns() - t0;
    CHECK(rc == 0 && skipped == S2A_FU_T_LATE, "safe draw waits item cap then skips (late)");
    CHECK(dt > 20ull * 1000000 && dt < 500ull * 1000000, "the wait was bounded near the item cap");

    /* c2：同 sha 仍是同一在途项——late 标记让可跳绘制立即跳过，不再等。 */
    t0 = mono_ns();
    rc = nvk_fu_compile(&safe_ds, 1, 1, json, sizeof json - 1, sha[0], &bytes, &blen, why, sizeof why, &skipped);
    dt = mono_ns() - t0;
    CHECK(rc == 0 && skipped == S2A_FU_T_LATE, "late mark skips the next safe draw at once");
    CHECK(dt < 20ull * 1000000, "the late-marked verdict does not wait again");
    DRAIN(sha[0]);

    /* c3..c5：帧预算逐笔消耗（每笔 ~30ms + 抖动，item=30/frame=100 → 3~5 笔耗尽）。
       排水笔只断言「late 或 budget」，分类由预算余量当时决定，不钉死。 */
    int saw_budget = 0;
    for (int i = 1; i <= 5 && !saw_budget; i++) {
        rc = nvk_fu_compile(&safe_ds, 1, 1, json, sizeof json - 1, sha[i], &bytes, &blen, why, sizeof why, &skipped);
        CHECK(rc == 0 && (skipped == S2A_FU_T_LATE || skipped == S2A_FU_T_BUDGET),
              "draining draws end as late or budget-clipped");
        if (skipped == S2A_FU_T_BUDGET) saw_budget = 1;
        DRAIN(sha[i]);
    }
    CHECK(saw_budget, "frame budget eventually exhausts within five item waits");

    /* c6：预算已空 → 新 sha 立即 BUDGET，不再等待，也不打 late 项标。 */
    t0 = mono_ns();
    rc = nvk_fu_compile(&safe_ds, 1, 1, json, sizeof json - 1, sha[6], &bytes, &blen, why, sizeof why, &skipped);
    dt = mono_ns() - t0;
    CHECK(rc == 0 && skipped == S2A_FU_T_BUDGET && dt < 20ull * 1000000,
          "empty frame budget skips at once (budget)");
    DRAIN(sha[6]);

    /* c7/c8：不可跳绘制按 unsafe 上限等 → STUCK(3)；之后同项一切绘制立即跳过。 */
    t0 = mono_ns();
    rc = nvk_fu_compile(&unsafe_ds, 1, 1, json, sizeof json - 1, sha[7], &bytes, &blen, why, sizeof why, &skipped);
    dt = mono_ns() - t0;
    CHECK(rc == 0 && skipped == S2A_FU_T_STUCK, "unskippable draw waits the unsafe cap then gives up (stuck)");
    CHECK(dt > 50ull * 1000000 && dt < 500ull * 1000000, "unsafe wait was bounded near its cap");
    t0 = mono_ns();
    rc = nvk_fu_compile(&unsafe_ds, 1, 1, json, sizeof json - 1, sha[7], &bytes, &blen, why, sizeof why, &skipped);
    dt = mono_ns() - t0;
    CHECK(rc == 0 && skipped == S2A_FU_T_STUCK, "stuck mark skips every later draw at once");
    CHECK(dt < 20ull * 1000000, "the stuck-marked verdict does not wait again");
    DRAIN(sha[7]);

    /* 帧 42 的预算已耗尽：adopt/fail 用例挪到帧 43（预算自动复位），重铺 fresh 证据。 */
    g_frame_no = 43;
    s2a_fu_rt_mark_fresh(7, 43);

    /* c9：新 sha、编译快 → 在途取到结果（bytes 回传），不跳过。 */
    g_delay_ms = 5;
    skipped = -1;
    rc = nvk_fu_compile(&safe_ds, 1, 1, json, sizeof json - 1, sha[8], &bytes, &blen, why, sizeof why, &skipped);
    CHECK(rc == 0 && skipped == 0 && bytes && blen == sizeof json - 1 && !memcmp(bytes, json, blen),
          "ready in-flight result is adopted for the draw");
    free(bytes); bytes = NULL;

    /* c10：编译失败如实返回 -1，why 带编译器原因；不算跳过。 */
    g_fail = 1;
    skipped = -1;
    rc = nvk_fu_compile(&safe_ds, 1, 1, json, sizeof json - 1, sha[9], &bytes, &blen, why, sizeof why, &skipped);
    CHECK(rc != 0 && skipped == 0 && strstr(why, "refused"), "compile failure returns with its reason, never a skip");
    g_fail = 0;

    s2a_compile_worker_stop();

    /* 默认关时字节级等价性由 w2_observation_draw_test 的 16 用例 A/B 覆盖（fu_wait=0
       走旧同步通道，决策环不进场）。 */

    if (fails) { printf("nvk fu decision: %d FAILURES\n", fails); return 1; }
    printf("nvk fu decision: %d checks ok (skip_safe, late/budget/stuck classes, marks, adopt, failure)\n", checks);
    return 0;
}
