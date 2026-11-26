// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Cross-queue ordering through the replay submission entry point.
#define main replay_other_tests_main
#define nvn_hle_submit_lock unused_fixture_submit_lock
#define nvn_hle_submit_unlock unused_fixture_submit_unlock
#include "t_nvn_replay.c"
#undef nvn_hle_submit_unlock
#undef nvn_hle_submit_lock
#undef main

static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;
void nvn_hle_submit_lock(void) { pthread_mutex_lock(&submit_lock); }
void nvn_hle_submit_unlock(void) { pthread_mutex_unlock(&submit_lock); }
static unsigned holds;
uintptr_t nvn_cmd_replay_hold(uint64_t address) { holds++; return (uintptr_t)address; }
void nvn_cmd_replay_release(uintptr_t hold) { if (hold) holds--; }

static const uint64_t wait_ns = 5000000000ull;

static void record_event(stream *s, volatile uint32_t *event, uint32_t mode, uint32_t value) {
    uint8_t *op = put(s, OP_SIGNAL_EVENT, 16);
    memcpy(op, &event, sizeof event);
    memcpy(op + 8, &mode, sizeof mode);
    memcpy(op + 12, &value, sizeof value);
}

static void copy_word(stream *s, const void *src, void *dst) {
    const uint64_t args[] = {(uintptr_t)src, (uintptr_t)dst, sizeof(uint32_t)};
    memcpy(put(s, OP_COPY_B2B, sizeof args), args, sizeof args);
}

static void fill_word(stream *s, void *dst, uint32_t value) {
    const p_clear_buf args = {.addr = (uintptr_t)dst, .size = sizeof value, .value = value};
    memcpy(put(s, OP_CLEAR_BUF, sizeof args), &args, sizeof args);
}

static void submit_queue(void *queue, stream *s) {
    const uint64_t handle = (uintptr_t)s->b;
    nvn_be_submit(queue, &handle, 1);
}

static uint32_t read_word(const void *address) {
    uint32_t value;
    memcpy(&value, address, sizeof value);
    return value;
}

static void wait_eq(stream *s, volatile uint32_t *event, uint32_t value) {
    record_event(s, event, NVN_EV_WAIT | NVN_EVENT_WAIT_MODE_EQUAL, value);
}

static void test_producer_submission_behind_consumer(void) {
    queue_t consumer = {0}, producer = {0};
    stream wait = {0}, signal = {0};
    volatile uint32_t event = 0;
    uint8_t *src = g_pmem, *dst = g_pmem + PG;
    const uint32_t old_value = 0x11111111, new_value = 0x22222222;
    uint32_t result = 0;
    memcpy(src, &old_value, sizeof old_value);
    memcpy(dst, &result, sizeof result);
    nvn_be_pool_add(g_pmem, sizeof g_pmem);

    wait_eq(&wait, &event, 1);
    copy_word(&wait, src, dst);
    end(&wait);
    fill_word(&signal, src, new_value);
    record_event(&signal, &event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    end(&signal);

    nvn_be_event_gpu_recorded(&event);
    nvn_hle_submit_lock();
    submit_queue(&consumer, &wait);
    CHECK(read_word(dst) == 0);
    submit_queue(&producer, &signal);
    CHECK(read_word(src) == new_value && event == 0 && read_word(dst) == 0);
    fake_complete(NVN_GPU_DONE_OK);
    nvn_hle_submit_unlock();
    CHECK(nvn_be_xq_wait_idle(&consumer, wait_ns));
    nvn_hle_submit_lock();
    fake_complete(NVN_GPU_DONE_OK);

    memcpy(&result, dst, sizeof result);
    printf("cross-queue producer event=%u consumer=%#010x expected=%#010x\n",
           event, result, new_value);
    CHECK(event == 1);
    CHECK(result == new_value);
    CHECK(!holds);
    nvn_hle_submit_unlock();
}

static void test_cpu_signal_nested_and_owed_fence(void) {
    queue_t consumer = {0}, producer = {0}, dependent = {0};
    stream inner = {0}, outer = {0}, tail = {0}, later = {0}, unrelated = {0}, ordered = {0};
    uint8_t *x = g_pmem, *y = x + 4, *z = y + 4, *w = z + 4, *last = w + 4;
    volatile uint32_t event = 0, other = 0;
    const uint32_t value = 0x33333333;
    memset(x, 0, 5 * sizeof value);
    sync_t fence = {0};
    wait_eq(&inner, &event, 1);
    fill_word(&inner, x, value);
    end(&inner);
    p_alpha_ref alpha = {.ref = 0.25f};
    memcpy(put(&outer, OP_ALPHA_REF, sizeof alpha), &alpha, sizeof alpha);
    const uint64_t inner_handle = (uintptr_t)inner.b;
    memcpy(put(&outer, OP_CALL, sizeof inner_handle), &inner_handle, sizeof inner_handle);
    copy_word(&outer, x, y);
    end(&outer);
    copy_word(&tail, y, z);
    end(&tail);
    copy_word(&later, z, w);
    end(&later);
    copy_word(&ordered, w, last);
    end(&ordered);
    alpha.ref = 0.75f;
    memcpy(put(&unrelated, OP_ALPHA_REF, sizeof alpha), &alpha, sizeof alpha);
    record_event(&unrelated, &other, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    end(&unrelated);

    nvn_hle_submit_lock();
    const uint64_t handles[] = {(uintptr_t)outer.b, (uintptr_t)tail.b};
    nvn_be_submit(&consumer, handles, 2);
    submit_queue(&consumer, &later);
    CHECK(nvn_be_xq_fence(&consumer, &fence));
    CHECK(nvn_be_xq_wait_queue(&dependent, &consumer, &fence));
    submit_queue(&dependent, &ordered);
    submit_queue(&producer, &unrelated);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(other == 1 && rp_S.alpha_ref == 0.75f && holds == 5);
    nvn_hle_submit_unlock();

    CHECK(!nvn_be_xq_owed_wait(&fence, 0));
    nvn_be_event_signal(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 2);
    CHECK(!nvn_be_xq_wait_idle(&consumer, 50000000));
    CHECK(!nvn_be_xq_owed_wait(&fence, 0));
    nvn_hle_submit_lock();
    CHECK(!read_word(x) && !read_word(last));
    nvn_hle_submit_unlock();

    // ADD wraps 2 + UINT32_MAX to the exact value 1.
    nvn_be_event_signal(&event, NVN_EVENT_SIGNAL_MODE_ADD, UINT32_MAX);
    CHECK(nvn_be_xq_owed_wait(&fence, wait_ns));
    CHECK(nvn_be_xq_wait_idle(&dependent, wait_ns));
    nvn_hle_submit_lock();
    fake_complete(NVN_GPU_DONE_OK);
    uint64_t published = 0;
    CHECK(sync_published(&fence, &published) && rp_fence_done(published));
    CHECK(read_word(x) == value && read_word(y) == value && read_word(z) == value &&
          read_word(w) == value && read_word(last) == value);
    CHECK(rp_S.alpha_ref == 0.25f && !holds);
    nvn_hle_submit_unlock();
}

static void test_satisfied_and_wrapping_comparisons(void) {
    queue_t queue = {0};
    stream ready = {0}, blocked = {0};
    volatile uint32_t equal = 7, wrapped = 2, half = UINT32_C(0x80000000);
    uint8_t *out = g_pmem;
    memset(out, 0, sizeof(uint32_t));
    wait_eq(&ready, &equal, 7);
    record_event(&ready, &wrapped, NVN_EV_WAIT | NVN_EVENT_WAIT_MODE_GEQUAL_WRAP, UINT32_MAX);
    fill_word(&ready, out, 1234);
    end(&ready);
    record_event(&blocked, &half, NVN_EV_WAIT | NVN_EVENT_WAIT_MODE_GEQUAL_WRAP, 0);
    fill_word(&blocked, out, 9876);
    end(&blocked);
    nvn_hle_submit_lock();
    submit_queue(&queue, &ready);
    CHECK(read_word(out) == 1234 && !holds);
    submit_queue(&queue, &blocked);
    CHECK(read_word(out) == 1234 && holds == 1);
    fake_complete(NVN_GPU_DONE_OK);
    nvn_hle_submit_unlock();
    nvn_be_event_signal(&half, NVN_EVENT_SIGNAL_MODE_WRITE, UINT32_MAX);
    CHECK(!nvn_be_xq_wait_idle(&queue, 50000000));
    nvn_be_event_signal(&half, NVN_EVENT_SIGNAL_MODE_WRITE, 0);
    CHECK(nvn_be_xq_wait_idle(&queue, wait_ns));
    nvn_hle_submit_lock();
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(read_word(out) == 9876 && !holds);
    nvn_hle_submit_unlock();
}

static void test_destroy_and_reuse_parked_queue(void) {
    queue_t queue = {0};
    stream old = {0}, queued = {0}, reuse = {0};
    volatile uint32_t event = 0;
    sync_t fence = {0};
    uint8_t *out = g_pmem;
    memset(out, 0, 3 * sizeof(uint32_t));
    wait_eq(&old, &event, 1);
    fill_word(&old, out, 1);
    end(&old);
    fill_word(&queued, out + 4, 2);
    end(&queued);
    fill_word(&reuse, out + 8, 3);
    end(&reuse);
    nvn_hle_submit_lock();
    submit_queue(&queue, &old);
    submit_queue(&queue, &queued);
    CHECK(nvn_be_xq_fence(&queue, &fence) && holds == 2);
    nvn_be_xq_discard(&queue);
    CHECK(!holds && !nvn_be_xq_owed(&fence, NULL) && !sync_published(&fence, NULL));
    nvn_be_event_signal(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    submit_queue(&queue, &reuse);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(!read_word(out) && !read_word(out + 4) && read_word(out + 8) == 3);
    nvn_hle_submit_unlock();
}

int main(void) {
    test_producer_submission_behind_consumer();
    test_cpu_signal_nested_and_owed_fence();
    test_satisfied_and_wrapping_comparisons();
    test_destroy_and_reuse_parked_queue();
    CHECK(!nvn_unimpl_kinds());
    nvn_be_pool_del(g_pmem);
    nvn_be_shutdown_session();
    printf("t_nvn_replay_wait_event checks=%d failures=%d\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
