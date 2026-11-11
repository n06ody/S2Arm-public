// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Synthetic metadata only; no game input, Vulkan driver or compiler is used.
#define S2A_ANDROID_VK_ENGINE 1
#define S2A_ANDROID_VK_BACKEND_NVN_VK 1
#define S2A_ANDROID_DEBUG_ENV 1
#include "../native/direct/w2_observation.inc"
#include <assert.h>

static unsigned cases;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #c); exit(1); \
} } while (0)
static const char profile[] = "sha256-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static struct s2a_direct_compile_context context = {
    .session = 91, .generation = 7, .direct_abi = 4,
    .translator_closure = "synthetic-closure", .profile_hash = profile
};
static uint8_t identity[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                              17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
struct sink {
    struct s2a_w2_window *window;
    struct s2a_w2_scope *leave_scope;
    int fail, make_late, release_owner, calls;
    char text[65536];
};
static int write_snapshot(void *opaque, const char *text, size_t bytes) {
    struct sink *s = opaque;
    CHECK(bytes < sizeof s->text);
    memcpy(s->text, text, bytes); s->text[bytes] = '\0';
    ++s->calls;
    // Re-entering observer APIs proves output I/O does not hold its mutex.
    if (s->make_late) {
        struct s2a_w2_scope scope;
        s2a_w2_scope_enter(&scope);
        s2a_w2_scope_leave(&scope, 1);
    }
    if (s->leave_scope) {
        s2a_w2_scope_leave(s->leave_scope, 1);
        s->leave_scope = NULL;
    }
    if (s->release_owner) s2a_w2_release(s->window);
    errno = EIO;
    return s->fail ? -1 : 0;
}
static struct s2a_w2_snapshot snap(struct s2a_w2_window *w) {
    struct s2a_w2_snapshot d;
    CHECK(s2a_w2_snapshot(w, &d) == 0);
    CHECK(!d.request_set_proven && !d.strict_proven && !d.gpu_proven && !d.w2_acceptance_proven);
    return d;
}
static struct s2a_w2_window *open_window(void) {
    struct s2a_w2_window *w = s2a_w2_start(1, &context);
    CHECK(w != NULL); return w;
}
static void one_shader(const struct s2a_direct_compile_context *c, const uint8_t *id,
                       enum s2a_w2_kind kind, enum s2a_w2_source source, int accepted,
                       int success) {
    struct s2a_w2_shader shader;
    s2a_w2_shader_begin(&shader, kind);
    s2a_w2_shader_lookup(c, id, source, accepted);
    s2a_w2_shader_end(&shader, success);
}
static void one_packet(struct s2a_w2_window *w, const uint8_t *id,
                       enum s2a_w2_kind kind, enum s2a_w2_source source) {
    (void)w;
    struct s2a_w2_scope scope;
    s2a_w2_scope_enter(&scope); s2a_w2_scope_expect_shader(&scope);
    one_shader(&context, id, kind, source, 1, 1);
    s2a_w2_scope_leave(&scope, 1);
}
static void finish(struct s2a_w2_window *w) {
    s2a_w2_close(w); CHECK(s2a_w2_seal(w, 1, 1) == 0);
}
static void expect_incomplete(struct s2a_w2_window *w) {
    finish(w); struct s2a_w2_snapshot d = snap(w);
    CHECK(!d.metadata_complete && !d.window_complete);
    struct sink sink = {0}; CHECK(s2a_w2_export(w, write_snapshot, &sink) != 0);
    CHECK(sink.calls == 1); CHECK(strstr(sink.text, "\"window_complete\":false"));
    s2a_w2_release(w); ++cases;
}
static void test_disabled(void) {
    errno = ERANGE;
    CHECK(!s2a_w2_start(0, &context));
    struct s2a_w2_scope scope; struct s2a_w2_shader shader;
    s2a_w2_scope_enter(&scope); s2a_w2_scope_expect_shader(&scope);
    s2a_w2_shader_begin(&shader, S2A_W2_GRAPHICS);
    s2a_w2_shader_lookup(&context, identity, S2A_W2_LIVE, 1);
    s2a_w2_shader_end(&shader, 1); s2a_w2_scope_leave(&scope, 1);
    CHECK(errno == ERANGE && !w2_current); ++cases;
}
static void test_positive_sources_and_full_identity(void) {
    struct s2a_w2_window *w = open_window();
    for (int source = 0; source < S2A_W2_SOURCE_COUNT; ++source)
        one_packet(w, identity, S2A_W2_GRAPHICS, (enum s2a_w2_source)source);
    uint8_t other[32]; memcpy(other, identity, 32); other[31] ^= 1;
    one_packet(w, other, S2A_W2_GRAPHICS, S2A_W2_MEMO);
    one_packet(w, identity, S2A_W2_COMPUTE, S2A_W2_LIVE);
    finish(w); struct s2a_w2_snapshot d = snap(w);
    CHECK(d.metadata_complete && !d.window_complete);
    CHECK(d.entry_count == 3 && d.observations == 7 && d.accepted_builds == 7);
    CHECK(!memcmp(d.entries[0].identity, identity, 32));
    CHECK(!memcmp(d.entries[1].identity, other, 32));
    CHECK(d.entries[2].kind == S2A_W2_COMPUTE);
    for (int source = 0; source < S2A_W2_SOURCE_COUNT; ++source)
        CHECK(d.entries[0].source_counts[source] == 1);
    CHECK(d.entries[1].source_counts[S2A_W2_MEMO] == 1);
    struct sink sink = {0}; errno = ERANGE;
    CHECK(s2a_w2_export(w, write_snapshot, &sink) == 0 && errno == ERANGE);
    CHECK(snap(w).window_complete);
    CHECK(strstr(sink.text, "\"receipt_not_persisted\":true"));
    CHECK(strstr(sink.text, "\"metadata_eligible\":true"));
    CHECK(strstr(sink.text, "\"request_set_complete\":\"not_proven\""));
    CHECK(strstr(sink.text, "\"strict_acceptance\":\"not_proven\""));
    CHECK(strstr(sink.text, "\"gpu_execution\":\"not_proven\""));
    CHECK(strstr(sink.text, "\"w2_acceptance\":\"not_proven\""));
    CHECK(strstr(sink.text, "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"));
    CHECK(!strstr(sink.text, "payload") && !strstr(sink.text, "canonical"));
    s2a_w2_release(w); ++cases;
}
// 一笔 dispatch 为等待先前 GPU 工作而内部重试：观察事实只计最终构建，不伪造失败或成功。
static void test_compute_sync_retry(void) {
    struct s2a_w2_window *w=open_window();struct s2a_w2_scope scope;struct s2a_w2_shader shader;
    s2a_w2_scope_enter(&scope);s2a_w2_scope_expect_shader(&scope);
    s2a_w2_shader_begin(&shader,S2A_W2_COMPUTE);
    s2a_w2_shader_lookup(&context,identity,S2A_W2_MEMO,1);
    s2a_w2_shader_end(&shader,-1); // 内部重试，不是构建完成
    one_shader(&context,identity,S2A_W2_COMPUTE,S2A_W2_MEMO,1,1);
    s2a_w2_scope_leave(&scope,1);finish(w);struct s2a_w2_snapshot d=snap(w);
    CHECK(d.shaders==1 && d.observations==1 && d.accepted_builds==1 && d.build_failures==0);
    CHECK(d.metadata_complete && d.entry_count==1 && d.entries[0].source_counts[S2A_W2_MEMO]==1);
    s2a_w2_release(w);++cases;
    // 真实被拒的 lookup 即便被错误标记成 retry，也不能抹掉拒绝与 gap。
    w=open_window();s2a_w2_scope_enter(&scope);s2a_w2_scope_expect_shader(&scope);
    s2a_w2_shader_begin(&shader,S2A_W2_COMPUTE);
    s2a_w2_shader_lookup(&context,identity,S2A_W2_MEMO,0);
    s2a_w2_shader_end(&shader,-1);s2a_w2_scope_leave(&scope,0);
    d=snap(w);CHECK(d.rejects>0 && d.gaps>0 && d.build_failures==1);
    expect_incomplete(w);
}
static void test_zero_and_ordinary_packets(void) {
    struct s2a_w2_window *w = open_window(); expect_incomplete(w);
    w = open_window(); struct s2a_w2_scope scope;
    s2a_w2_scope_enter(&scope); s2a_w2_scope_leave(&scope, 1);
    CHECK(!snap(w).gaps && !snap(w).shaders); expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope); s2a_w2_scope_leave(&scope, 1);
    one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_MEMORY); finish(w);
    CHECK(snap(w).metadata_complete && !snap(w).gaps); s2a_w2_release(w); ++cases;
}
static void test_zero_hash(void) {
    struct s2a_w2_window *w = open_window(); uint8_t zero[32] = {0};
    one_packet(w, zero, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    CHECK(snap(w).metadata_complete && !snap(w).missing_identity);
    s2a_w2_release(w); ++cases;
}
static void test_close_and_lease(void) {
    struct s2a_w2_window *w = open_window(); struct s2a_w2_scope scope;
    s2a_w2_scope_enter(&scope); s2a_w2_scope_expect_shader(&scope); s2a_w2_close(w);
    CHECK(s2a_w2_seal(w, 1, 1) != 0 && !snap(w).metadata_complete);
    one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE, 1, 1);
    s2a_w2_scope_leave(&scope, 1); CHECK(s2a_w2_seal(w, 1, 1) == 0);
    CHECK(snap(w).metadata_complete); s2a_w2_release(w); ++cases;
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE);
    s2a_w2_close(w); s2a_w2_scope_enter(&scope);
    one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_MEMO, 1, 1);
    s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).observations == 1);
    expect_incomplete(w);
}
static void test_early_nested_and_unbound(void) {
    struct s2a_w2_window *w = open_window(); struct s2a_w2_scope scope, inner;
    struct s2a_w2_shader shader, nested;
    s2a_w2_scope_enter(&scope); s2a_w2_scope_expect_shader(&scope);
    s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).gaps); expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope);
    s2a_w2_shader_begin(&shader, S2A_W2_GRAPHICS); s2a_w2_shader_end(&shader, 0);
    s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).missing_identity); expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope);
    s2a_w2_shader_begin(&shader, S2A_W2_GRAPHICS);
    s2a_w2_shader_begin(&nested, S2A_W2_COMPUTE);
    s2a_w2_shader_lookup(&context, identity, S2A_W2_LIVE, 1);
    s2a_w2_shader_end(&nested, 1);
    s2a_w2_shader_lookup(&context, identity, S2A_W2_MEMORY, 1);
    s2a_w2_shader_end(&shader, 1); s2a_w2_scope_leave(&scope, 1);
    CHECK(snap(w).observations == 1 && snap(w).entries[0].source_counts[S2A_W2_MEMORY] == 1);
    expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope); s2a_w2_scope_enter(&inner);
    one_shader(&context, identity, S2A_W2_COMPUTE, S2A_W2_LIVE, 1, 1);
    s2a_w2_scope_leave(&inner, 1);
    one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_MEMO, 1, 1);
    s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).observations == 1); expect_incomplete(w);
    w = open_window(); one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE, 1, 1);
    CHECK(snap(w).unbound && !snap(w).observations); expect_incomplete(w);
    w = open_window(); s2a_w2_shader_lookup(&context, identity, S2A_W2_LIVE, 1);
    CHECK(snap(w).unbound); expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope); s2a_w2_scope_leave(&scope, 0);
    CHECK(snap(w).rejects); expect_incomplete(w);
}
static void test_cohort_missing_rejected_and_failed(void) {
    for (int change = 0; change < 5; ++change) {
        struct s2a_w2_window *w = open_window(); struct s2a_w2_scope scope;
        struct s2a_direct_compile_context different = context;
        if (change == 0) ++different.session;
        if (change == 1) ++different.generation;
        if (change == 2) ++different.direct_abi;
        if (change == 3) different.translator_closure = "different";
        if (change == 4) different.profile_hash =
            "sha256-1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        s2a_w2_scope_enter(&scope);
        one_shader(&different, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE, 1, 1);
        s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).cohort_mismatch == 1 && !snap(w).observations && !snap(w).entry_count);
        expect_incomplete(w);
    }
    struct s2a_w2_window *w = open_window(); struct s2a_w2_scope scope;
    s2a_w2_scope_enter(&scope);
    one_shader(&context, NULL, S2A_W2_GRAPHICS, S2A_W2_LIVE, 1, 1);
    s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).missing_identity); expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope);
    one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_MEMO, 0, 0);
    s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).rejects && snap(w).observations == 1);
    CHECK(!snap(w).entries[0].source_counts[S2A_W2_MEMO] &&
          snap(w).entries[0].rejected_source_counts[S2A_W2_MEMO] == 1);
    expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope);
    one_shader(&context, identity, S2A_W2_COMPUTE, S2A_W2_UNKNOWN, 0, 0);
    s2a_w2_scope_leave(&scope, 1); CHECK(snap(w).observations == 1); expect_incomplete(w);
    w = open_window(); s2a_w2_scope_enter(&scope);
    one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE, 1, 0);
    s2a_w2_scope_leave(&scope, 1);
    CHECK(snap(w).build_failures == 1 && snap(w).entries[0].source_counts[S2A_W2_LIVE] == 1);
    expect_incomplete(w);
}
static void test_overflow_and_stop_failure(void) {
    struct s2a_w2_window *w = open_window();
    for (unsigned i = 0; i <= S2A_W2_ENTRY_CAPACITY; ++i) {
        uint8_t id[32]; memcpy(id, identity, 32); id[30] = (uint8_t)i;
        one_packet(w, id, S2A_W2_GRAPHICS, S2A_W2_LIVE);
    }
    CHECK(snap(w).entry_count == S2A_W2_ENTRY_CAPACITY && snap(w).overflow == 1);
    expect_incomplete(w);
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE);
    s2a_w2_close(w); CHECK(s2a_w2_seal(w, 0, 1) != 0);
    CHECK(snap(w).state == S2A_W2_CLOSING && snap(w).stop_failures == 1);
    expect_incomplete(w); // A later successful stop never erases the failed attempt.
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE);
    s2a_w2_close(w); CHECK(s2a_w2_seal(w, 1, 0) != 0);
    CHECK(snap(w).drain_failures == 1); expect_incomplete(w);
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE);
    CHECK(s2a_w2_seal(w, 1, 1) != 0 && snap(w).protocol_errors); expect_incomplete(w);
}
static void test_receipts_and_output(void) {
    struct s2a_w2_window *w = open_window();
    one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE);
    struct s2a_w2_snapshot old = snap(w); finish(w);
    CHECK(snap(w).metadata_complete); CHECK(s2a_w2_ack(w, &old, 1) != 0);
    CHECK(!snap(w).window_complete && snap(w).output_failures); s2a_w2_release(w); ++cases;
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    old = snap(w); ++old.instance; CHECK(s2a_w2_ack(w, &old, 1) != 0);
    CHECK(!snap(w).window_complete); s2a_w2_release(w); ++cases;
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    struct sink sink = {.fail = 1}; CHECK(s2a_w2_export(w, write_snapshot, &sink) != 0);
    CHECK(snap(w).output_failures == 1 && !snap(w).metadata_complete);
    sink.fail = 0; CHECK(s2a_w2_export(w, write_snapshot, &sink) != 0);
    CHECK(!snap(w).window_complete); s2a_w2_release(w); ++cases;
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    memset(&sink, 0, sizeof sink); sink.make_late = 1;
    CHECK(s2a_w2_export(w, write_snapshot, &sink) != 0);
    CHECK(snap(w).late && !snap(w).window_complete);
    CHECK(strstr(sink.text, "\"window_complete\":false")); s2a_w2_release(w); ++cases;
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    memset(&sink, 0, sizeof sink); CHECK(s2a_w2_export(w, write_snapshot, &sink) == 0);
    struct s2a_w2_scope scope; s2a_w2_scope_enter(&scope); s2a_w2_scope_leave(&scope, 1);
    CHECK(!snap(w).window_complete && snap(w).late); s2a_w2_release(w); ++cases;
}
static void test_ineligible_snapshot_progress(void) {
    struct s2a_w2_window *w = open_window(); struct s2a_w2_scope scope;
    s2a_w2_scope_enter(&scope);
    one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE, 1, 1);
    s2a_w2_close(w);
    CHECK(s2a_w2_seal(w, 1, 1) != 0 && snap(w).inflight_scopes == 1);
    struct sink sink = {.leave_scope = &scope};
    CHECK(s2a_w2_export(w, write_snapshot, &sink) != 0 && sink.calls == 1);
    CHECK(strstr(sink.text, "\"metadata_eligible\":false"));
    CHECK(!snap(w).inflight_scopes && !snap(w).window_complete);
    CHECK(snap(w).output_failures == 0 && snap(w).gaps == 0);
    CHECK(s2a_w2_seal(w, 1, 1) == 0 && snap(w).metadata_complete);
    CHECK(s2a_w2_export(w, write_snapshot, &sink) == 0 && sink.calls == 2);
    CHECK(snap(w).window_complete && !snap(w).output_failures);
    s2a_w2_release(w); ++cases;
    w = open_window(); s2a_w2_scope_enter(&scope);
    one_shader(&context, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE, 1, 1);
    s2a_w2_close(w); CHECK(s2a_w2_seal(w, 1, 1) != 0);
    memset(&sink, 0, sizeof sink); sink.leave_scope = &scope; sink.fail = 1;
    CHECK(s2a_w2_export(w, write_snapshot, &sink) != 0);
    CHECK(snap(w).output_failures == 1 && snap(w).gaps > 0);
    CHECK(s2a_w2_seal(w, 1, 1) == 0 && !snap(w).metadata_complete);
    s2a_w2_release(w); ++cases;
}
static void test_serialization_bounds(void) {
    struct s2a_w2_snapshot d = {0};
#define MAXIMUM(name) d.name = UINT64_MAX
    MAXIMUM(instance); MAXIMUM(revision); MAXIMUM(session); MAXIMUM(generation);
    MAXIMUM(scopes); MAXIMUM(inflight_scopes); MAXIMUM(shaders); MAXIMUM(finished_shaders);
    MAXIMUM(inflight_shaders); MAXIMUM(observations); MAXIMUM(accepted_builds);
    MAXIMUM(gaps); MAXIMUM(rejects); MAXIMUM(overflow); MAXIMUM(late); MAXIMUM(unbound);
    MAXIMUM(missing_identity); MAXIMUM(cohort_mismatch); MAXIMUM(build_failures);
    MAXIMUM(protocol_errors); MAXIMUM(stop_failures); MAXIMUM(drain_failures);
    MAXIMUM(output_failures);
#undef MAXIMUM
    d.direct_abi = UINT32_MAX; d.state = UINT32_MAX;
    memset(d.closure, 1, sizeof d.closure - 1); // Every byte becomes a six-byte JSON escape.
    memcpy(d.profile_hash, profile, sizeof profile);
    d.entry_count = S2A_W2_ENTRY_CAPACITY;
    for (size_t i = 0; i < d.entry_count; ++i) {
        memset(d.entries[i].identity, 255, sizeof d.entries[i].identity);
        d.entries[i].kind = S2A_W2_GRAPHICS;
        for (int source = 0; source < S2A_W2_SOURCE_COUNT; ++source) {
            d.entries[i].source_counts[source] = UINT64_MAX;
            d.entries[i].rejected_source_counts[source] = UINT64_MAX;
        }
        d.entries[i].unknown_rejects = UINT64_MAX;
    }
    char text[65536]; struct w2_json full = {text, 0, sizeof text, 0};
    w2_json_snapshot(&full, &d);
    // Independent schema bound: 457 bytes/entry + 127 commas + 2005 bytes envelope.
    CHECK(!full.failed && full.used == 60628 && full.used + 1 <= sizeof text);
    CHECK(text[full.used - 1] == '\n' && text[full.used] == '\0');
    ++cases;
    char tiny[32]; memset(tiny, 0x5a, sizeof tiny);
    struct w2_json small = {tiny, 0, sizeof tiny - 1, 0};
    w2_json_snapshot(&small, &d);
    CHECK(small.failed && small.used < small.capacity && tiny[sizeof tiny - 1] == 0x5a);
    char saved[sizeof tiny]; memcpy(saved, tiny, sizeof saved);
    w2_json_add(&small, "must not append after serialization failure");
    CHECK(!memcmp(saved, tiny, sizeof saved));
    ++cases;
    printf("PASS W2 serialization bound: %zu / %zu bytes including newline; tiny buffer fails closed\n",
           full.used, sizeof text);
}
static void test_null_sink(void) {
    struct s2a_w2_window *w = open_window();
    one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    struct sink sink = {0}; CHECK(s2a_w2_export(w, write_snapshot, &sink) == 0);
    CHECK(s2a_w2_export(w, NULL, NULL) != 0);
    CHECK(snap(w).output_failures == 1 && !snap(w).window_complete);
    s2a_w2_release(w); ++cases;
}
static void test_repeat_seal(void) {
    struct s2a_w2_window *w = open_window();
    one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_OFFLINE); finish(w);
    struct sink sink = {0}; CHECK(s2a_w2_export(w, write_snapshot, &sink) == 0);
    struct s2a_w2_snapshot before = snap(w);
    CHECK(before.window_complete && before.metadata_complete);
    s2a_w2_close(w); CHECK(s2a_w2_seal(w, 1, 1) == 0);
    struct s2a_w2_snapshot after = snap(w);
    CHECK(after.instance == before.instance && after.revision == before.revision);
    CHECK(after.window_complete && after.metadata_complete); // Before another export can repair an ACK.
    CHECK(after.observations == before.observations && after.accepted_builds == before.accepted_builds);
    CHECK(after.gaps == 0 && after.stop_failures == 0 && after.drain_failures == 0);
    s2a_w2_release(w); ++cases;

    w = open_window(); one_packet(w, identity, S2A_W2_COMPUTE, S2A_W2_OFFLINE); finish(w);
    CHECK(s2a_w2_export(w, write_snapshot, &sink) == 0);
    CHECK(s2a_w2_seal(w, 0, 0) != 0); // A later failed destroy remains evidence, not a fresh window.
    before = snap(w);
    CHECK(before.stop_failures == 1 && before.drain_failures == 1 && before.gaps == 2);
    CHECK(!before.metadata_complete && !before.window_complete);
    CHECK(s2a_w2_seal(w, 1, 1) == 0);
    after = snap(w);
    CHECK(after.instance == before.instance && after.revision == before.revision);
    CHECK(after.stop_failures == 1 && after.drain_failures == 1 && after.gaps == 2);
    CHECK(!after.metadata_complete && !after.window_complete);
    s2a_w2_release(w); ++cases;
}
static void test_owner_and_misordered_lifetimes(void) {
    struct s2a_w2_window *w = open_window(); struct s2a_w2_scope scope;
    struct s2a_w2_shader shader;
    s2a_w2_scope_enter(&scope); s2a_w2_shader_begin(&shader, S2A_W2_GRAPHICS);
    s2a_w2_release(w); // Scope and shader each own a reference after provider destruction.
    s2a_w2_shader_lookup(&context, identity, S2A_W2_LIVE, 1);
    s2a_w2_shader_end(&shader, 1); s2a_w2_scope_leave(&scope, 1);
    CHECK(!w2_current); ++cases;
    w = open_window(); s2a_w2_scope_enter(&scope); s2a_w2_shader_begin(&shader, S2A_W2_GRAPHICS);
    s2a_w2_scope_leave(&scope, 1);
    CHECK(snap(w).protocol_errors && snap(w).inflight_shaders == 1);
    s2a_w2_shader_lookup(&context, identity, S2A_W2_LIVE, 1); s2a_w2_shader_end(&shader, 1);
    expect_incomplete(w);
    w = open_window(); one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    struct sink sink = {.window = w, .release_owner = 1};
    CHECK(s2a_w2_export(w, write_snapshot, &sink) == 0); // Export owns a temporary reference.
    CHECK(!w2_current); ++cases;
}
static void *unbound_thread(void *unused) {
    (void)unused; one_shader(&context, identity, S2A_W2_COMPUTE, S2A_W2_LIVE, 1, 1);
    return NULL;
}
static void test_thread_binding(void) {
    struct s2a_w2_window *w = open_window(); pthread_t thread;
    CHECK(pthread_create(&thread, NULL, unbound_thread, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(snap(w).unbound == 1 && snap(w).observations == 0); expect_incomplete(w);
}
static void test_tiny_export(void) {
    struct s2a_w2_window *w = open_window();
    one_packet(w, identity, S2A_W2_GRAPHICS, S2A_W2_LIVE); finish(w);
    struct sink sink = {0};
    CHECK(s2a_w2_export(w, write_snapshot, &sink) != 0);
    CHECK(sink.calls == 0 && snap(w).output_failures == 1 && !snap(w).window_complete);
    s2a_w2_release(w);
    puts("PASS W2 tiny export: serialization failure skips writer and cannot acknowledge completion");
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "repeat-seal")) { test_repeat_seal(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "tiny-export")) { test_tiny_export(); return 0; }
    test_disabled(); test_positive_sources_and_full_identity(); test_compute_sync_retry(); test_zero_and_ordinary_packets();
    test_zero_hash(); test_close_and_lease(); test_early_nested_and_unbound(); test_cohort_missing_rejected_and_failed();
    test_overflow_and_stop_failure(); test_receipts_and_output();
    test_ineligible_snapshot_progress(); test_serialization_bounds(); test_null_sink();
    test_repeat_seal(); test_owner_and_misordered_lifetimes();
    test_thread_binding();
    CHECK(!w2_current && !w2_scope && !w2_shader);
    printf("PASS W2 observation core: %u synthetic metadata cases; request set / strict / GPU / W2 acceptance not proven\n", cases);
    return 0;
}
