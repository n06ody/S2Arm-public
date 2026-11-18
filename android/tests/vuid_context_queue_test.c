// SPDX-License-Identifier: Apache-2.0
// Run against the real two-stage queue and a recording Vulkan stub. The two
// producers leave their scopes before the recorder may inspect the first call.
#define main dq_original_main
#include "dq_queue_test.c"
#undef main
#include "nvn_vuid_diag.h"

static _Thread_local struct s2a_vuid_context fallback;
static _Atomic int released, failures, observed, reported;
static void exchange(const struct s2a_vuid_context *in, struct s2a_vuid_context *out) {
    if (s2a_vuid_exchange) s2a_vuid_exchange(in, out);
    else { if (out) *out = fallback; fallback = in ? *in : (struct s2a_vuid_context){0}; }
}
static const struct s2a_vuid_context *current(void) {
    return s2a_vuid_current ? s2a_vuid_current() : &fallback;
}
static void check_event(uint32_t vc, int32_t id, const char *vuid, const char *msg) {
    while (!atomic_load_explicit(&released, memory_order_acquire)) sched_yield();
    const struct s2a_vuid_context *c = current();
    int ok = vc == 3 ? !c || c->kind == S2A_VUID_NONE :
        c && c->kind == S2A_VUID_DRAW && c->stages == 1 && c->keys[0][0] == vc;
    if (!ok) {
        int n = atomic_fetch_add(&failures, 1);
        if (n < 4) fprintf(stderr, "【红】draw %u: expected key=%u, got kind=%u mask=%u key=%u\n", vc, vc,
            c ? c->kind : 0, c ? c->stages : 0, c ? c->keys[0][0] : 0);
    }
    if (s2a_vuid_observe) {
        char line[1200], key[32];
        int emitted = s2a_vuid_observe(id, vuid, msg, line, sizeof line);
        if (emitted) {
            snprintf(key, sizeof key, "st0=%02x00000000000000", vc);
            if (!(vc == 3 ? strstr(line, "unknown") != NULL : strstr(line, "exact") && strstr(line, key))) {
                atomic_fetch_add(&failures, 1); fprintf(stderr, "【红】错误关联行：%s\n", line);
            }
            atomic_fetch_add(&reported, 1);
        }
    }
    atomic_fetch_add(&observed, 1);
}
static VKAPI_ATTR void VKAPI_CALL check_draw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    (void)cb; (void)ic; (void)fv; (void)fi;
    check_event(vc, 7753, "VUID-vkCmdDraw-format-07753", "vkCmdDraw(): format mismatch");
}
static VKAPI_ATTR void VKAPI_CALL check_bindpipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    (void)cb; (void)bp;
    check_event((uint32_t)(uintptr_t)p, 328, "VUID-bind-test", "vkCmdBindPipeline(): test");
    if (s2a_vuid_observe) {
        char line[1200];
        // 回放的是 vkCmdBindPipeline 这条记录，消息却点名 vkCmdDraw：不能算精确
        if (s2a_vuid_observe(321, "VUID-mismatched-api", "vkCmdDraw(): unrelated callback", line, sizeof line) && (strstr(line, " exact ") || !strstr(line, "不是本范围的入口"))) {
            atomic_fetch_add(&failures, 1); fprintf(stderr, "【红】入口不匹配却归到程序：%s\n", line);
        }
    }
}
static void *producer(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    struct s2a_vuid_context c = {.kind = S2A_VUID_DRAW, .stages = 1, .bound = 1}, old;
    c.keys[0][0] = (uint8_t)id;
    exchange(&c, &old);
    vkCmdBindPipeline(cb_of(id), VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)id);
    for (unsigned i = 0; i < 200; ++i) vkCmdDraw(cb_of(id), id, 1, i, 0);
    exchange(&old, NULL);
    return NULL;
}
int main(void) {
    if (s2a_vuid_start) s2a_vuid_start(1);
    vk_dq_init((VkDevice)(uintptr_t)1);
    if (!g_dq_ring || !g_dq_on) { fprintf(stderr, "queue unavailable\n"); return 2; }
    r_vkCmdDraw = check_draw; r_vkCmdBindPipeline = check_bindpipe;
    pthread_t a, b;
    pthread_create(&a, NULL, producer, (void *)(uintptr_t)1);
    pthread_create(&b, NULL, producer, (void *)(uintptr_t)2);
    pthread_join(a, NULL); pthread_join(b, NULL);
    vkCmdDraw(cb_of(3), 3, 1, 0, 0); // no source scope: must remain unknown
    atomic_store_explicit(&released, 1, memory_order_release);
    vk_dq_drain();
    if (s2a_vuid_observe && atomic_load(&reported) != 19) {
        atomic_fetch_add(&failures, 1); fprintf(stderr, "【红】关联行数期望 19，实测 %d\n", atomic_load(&reported));
    }
    printf("vuid_context_queue: observed=%d expected=403 incorrect=%d reports=%d\n", atomic_load(&observed), atomic_load(&failures), atomic_load(&reported));
    return atomic_load(&observed) != 403 || atomic_load(&failures) != 0;
}
