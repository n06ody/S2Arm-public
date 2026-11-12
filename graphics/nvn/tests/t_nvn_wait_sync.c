// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 复用回放测试的真实录制格式和受控后端；不把已提交误当 GPU 已完成。
#define main replay_other_tests_main
#define nvn_gpu_fence_wait fixture_fence_wait
#include "t_nvn_replay.c"
#undef nvn_gpu_fence_wait
#undef main

static int wait_calls;
nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t ns, nvn_gpu_err *err) {
    wait_calls++;
    return fixture_fence_wait(v, ns, err);
}
static void put_sync(stream *s, uint32_t op, const sync_t *sync) {
    memcpy(put(s, op, sizeof sync), &sync, sizeof sync);
}
static void after_marker(stream *s, float x) {
    p_alpha_ref *a = put(s, OP_ALPHA_REF, sizeof *a); a->ref = x; end(s);
}
static void submit_queue(stream *s, void *queue) {
    uint64_t h = (uint64_t)(uintptr_t)s->b;
    nvn_be_submit(queue, &h, 1);
}
static void wait_prior_submission(void) {
    sync_t sync = {0}; stream producer = {0}, consumer = {0};
    int queue_a, queue_b;
    nvn_unimpl_reset(); wait_calls = 0;
    put_sync(&producer, OP_FENCE_SYNC, &sync); end(&producer); submit_queue(&producer, &queue_a);
    uint64_t fence = 0; CHECK(sync_published(&sync, &fence));
    CHECK(g_npend == 1 && g_fdone < fence); // 已发布但没有完成证明
    put_sync(&consumer, OP_WAIT_SYNC, &sync); after_marker(&consumer, 0.25f); submit_queue(&consumer, &queue_b);
    CHECK(g_npend == 2 && g_pend[0].signal == fence && g_pend[1].signal > fence);
    CHECK(wait_calls == 0 && rp_S.alpha_ref == 0.25f);
    CHECK(nvn_unimpl_count("op", "OP_WAIT_SYNC") == 0 && nvn_unimpl_kinds() == 0);
    fake_complete(NVN_GPU_DONE_OK);
}
static void wait_same_submission(void) {
    sync_t sync = {0}; stream s = {0}; nvn_unimpl_reset(); wait_calls = 0;
    put_sync(&s, OP_FENCE_SYNC, &sync); put_sync(&s, OP_WAIT_SYNC, &sync); after_marker(&s, 0.5f);
    submit1(&s);
    uint64_t fence = 0; CHECK(sync_published(&sync, &fence));
    CHECK(g_npend == 1 && g_pend[0].signal == fence && g_fdone < fence);
    CHECK(wait_calls == 0 && rp_S.alpha_ref == 0.5f && nvn_unimpl_kinds() == 0);
    fake_complete(NVN_GPU_DONE_OK);
}
static void wait_invalid_or_external(void) {
    sync_t absent = {0}, external = {0}, already_signaled = {0}; stream s = {0};
    sync_publish_ext(&external, 0, (void *)(uintptr_t)1, 7);
    sync_publish(&already_signaled, 0); // 初始化自已完成的外部服务栅栏也有合法值 0
    nvn_unimpl_reset(); wait_calls = 0;
    put_sync(&s, OP_WAIT_SYNC, &absent); put_sync(&s, OP_WAIT_SYNC, NULL);
    put_sync(&s, OP_WAIT_SYNC, &external); put_sync(&s, OP_WAIT_SYNC, &already_signaled);
    after_marker(&s, 0.75f); submit1(&s);
    CHECK(nvn_unimpl_count("sync", "wait_sync_unpublished") == 2);
    CHECK(nvn_unimpl_count("sync", "wait_sync_external") == 1);
    CHECK(nvn_unimpl_count("op", "OP_WAIT_SYNC") == 0 && nvn_unimpl_kinds() == 2);
    CHECK(wait_calls == 0 && rp_S.alpha_ref == 0.75f);
    fake_complete(NVN_GPU_DONE_OK);
}
int main(void) {
    wait_prior_submission(); wait_same_submission(); wait_invalid_or_external();
    printf("wait sync: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
