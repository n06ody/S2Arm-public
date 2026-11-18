// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define _POSIX_C_SOURCE 200809L
/* Original host fixture. Facts boundary is synthetic; request, worker and the
 * extracted adapter callback are the actual production implementations. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "direct/compile_request.h"
#include "direct/compile_worker.h"
#include "direct/s2a_direct.h"
#include "direct/sha256.h"
#include "direct/w2_single_query.h"

static atomic_int failures;
#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "ASSERT %s:%d: %s\n", __FILE__, __LINE__, label); failures++; } } while (0)

struct nvk_w2_capsule { atomic_int claimed, abandoned, released, published; };
static struct nvk_w2_capsule capsules[32];
static atomic_int claims, publishes, abandons, releases, compiler_calls, compiler_entered;
static int export_open = 1, compiler_mode, block_compile;
static uint8_t code[2][80], control[2][0x880];
static const uint8_t *expected_request;
static size_t expected_request_len;
static uint8_t retained_error[8192];
static size_t retained_error_len;
static struct s2a_w2_query retained_query;
static _Thread_local struct nvk_w2_capsule *returned_capsule;
static enum s2a_w2_query_method expected_method = S2A_W2_QUERY_NORMALIZED;
static pthread_mutex_t event_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv = PTHREAD_COND_INITIALIZER;
static int entered, permit_return;
static char error_json[4096];
static size_t error_len;

int s2a_compile_export_open(void) { return export_open; }

/* Stub proves transport calls; it deliberately does NOT pretend to prove the
 * real facts owner's freeze, canonical/source matching or pool lifecycle. */
int nvk_w2_capsule_claim(const struct s2a_compile_request_input *in,
                         struct nvk_w2_capsule **out) {
    *out = NULL;
    if (!export_open || !in || in->session != 51 || in->generation != 7 ||
        in->stage_count != 2 || in->stages[0].code != code[0] ||
        in->stages[1].code != code[1] || in->stages[0].control != control[0] ||
        in->stages[1].control != control[1]) return -1;
    int index = atomic_fetch_add(&claims, 1);
    if (index >= 32) return -1;
    capsules[index].claimed = 1;
    *out = &capsules[index];
    return 0;
}

int nvk_w2_capsule_publish(struct nvk_w2_capsule *cap,
                           const struct s2a_w2_query *query,
                           const uint8_t *raw, size_t bytes) {
    CHECK(cap && cap->claimed && !cap->released, "publish owns live capsule");
    if (!cap || cap->abandoned || !query || !raw || bytes > sizeof retained_error) return -1;
    memcpy(retained_error, raw, bytes);
    retained_error_len = bytes;
    retained_query = *query;
    cap->published = 1;
    atomic_fetch_add(&publishes, 1);
    return 0;
}
void nvk_w2_capsule_abandon(struct nvk_w2_capsule *cap, int reason) {
    (void)reason;
    if (cap && !atomic_exchange(&cap->abandoned, 1)) atomic_fetch_add(&abandons, 1);
}
void nvk_w2_capsule_release(struct nvk_w2_capsule *cap) {
    if (!cap) return;
    CHECK(!atomic_exchange(&cap->released, 1), "capsule reference released once");
    atomic_fetch_add(&releases, 1);
}
void nvk_w2_capsule_returned(struct nvk_w2_capsule *cap) {
    if (cap) CHECK(cap->published && !cap->abandoned && !cap->released, "returned receipt belongs to current live attempt");
    /* Static fixture records model facts' separately retained TLS ownership. */
    returned_capsule = cap;
}
static int consume_returned(void) {
    struct nvk_w2_capsule *cap = returned_capsule;
    returned_capsule = NULL;
    return cap && cap->published && !cap->abandoned;
}

static void make_error(const char *message, int padding) {
    size_t n = padding ? 600u : 0u;
    memset(error_json, ' ', n);
    int count = snprintf(error_json + n, sizeof error_json - n,
        "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"%s\"}", message);
    CHECK(count > 0 && (size_t)count < sizeof error_json - n, "synthetic error fits");
    error_len = n + (size_t)count;
}

static int32_t compiler(uint64_t session, uint64_t generation, s2a_direct_bytes request,
                         s2a_direct_emit_fn emit, void *sink) {
    CHECK(session == 51 && generation == 7, "compiler receives unchanged transport");
    CHECK(request.bytes == expected_request_len &&
          !memcmp(request.data, expected_request, expected_request_len), "compiler receives exact original JSON bytes");
    atomic_fetch_add(&compiler_calls, 1);
    if (block_compile) {
        pthread_mutex_lock(&event_mu);
        entered = 1; atomic_store(&compiler_entered, 1);
        pthread_cond_broadcast(&event_cv);
        while (!permit_return) pthread_cond_wait(&event_cv, &event_mu);
        pthread_mutex_unlock(&event_mu);
    }
    if (compiler_mode == 1) return S2A_DIRECT_RES_ARGUMENT; /* no emit */
    if (compiler_mode == 3) {
        static const uint8_t ok[] = "{\"schema\":\"s2a-direct-program-v1\",\"contentHash\":\"ok\"}";
        s2a_direct_bytes result = {ok, sizeof ok - 1};
        return emit(sink, result);
    }
    s2a_direct_bytes result = {(const uint8_t *)error_json, error_len};
    CHECK(emit(sink, result) == 0, "compiler error emit accepted");
    if (compiler_mode == 2) CHECK(emit(sink, result) == 0, "second emit preserves existing compile channel behavior");
    return S2A_DIRECT_RES_ARGUMENT;
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return compiler; }

/* Generated by the runner from the actual adapter source, not hand copied. */
#include "adapter_wrapper.inc"

static char *build(size_t *bytes) {
    struct s2a_compile_request_input in = {0};
    static const char profile[] = "{\"synthetic\":true}";
    in.session = 51; in.generation = 7; in.request_id = "public-synthetic";
    in.profile_json = profile; in.profile_bytes = sizeof profile - 1;
    in.direct_abi = 1; in.translator_closure = "public-test-closure";
    in.stage_count = 2;
    for (unsigned i = 0; i < 2; i++) {
        in.stages[i].stage = i ? "fs" : "vs";
        in.stages[i].code = code[i]; in.stages[i].code_bytes = sizeof code[i];
        in.stages[i].control = control[i]; in.stages[i].control_bytes = sizeof control[i];
    }
    in.gfx.topology = "Triangles";
    char *json = NULL;
    CHECK(s2a_compile_request_build(&in, &json, bytes, NULL, 0) == 0, "actual request build succeeds");
    CHECK(json && *bytes, "actual request owns JSON");
    return json;
}

static void expect_published(int want) {
    CHECK(atomic_load(&publishes) == want, "expected exact diagnostic publication count");
    if (want) {
        CHECK(retained_error_len == error_len && !memcmp(retained_error, error_json, error_len), "raw error retained byte-for-byte beyond summary cap");
        CHECK(retained_query.method == expected_method &&
              retained_query.coordinate.stage == S2A_W2_QUERY_FS &&
              retained_query.coordinate.handle == 12 && retained_query.coordinate.cbuf_slot == 3,
              "query parsed without changing original coordinates");
    }
}
static void *exit_with_pending(void *unused) {
    (void)unused;
    size_t bytes = 0;
    char *json = build(&bytes);
    free(json); /* Simulated caller cancellation before run/execute. */
    return NULL; /* The owning diagnostic binding must be destroyed here. */
}

struct run_args { const char *json; size_t len; uint8_t sha[32]; int deadline, rc, build_here, receipt; uint8_t *bytes; size_t bytes_len; };
static void *run_worker(void *opaque) {
    struct run_args *a = opaque;
    char *local_json = NULL;
    if (a->build_here) { local_json = build(&a->len); a->json = local_json; }
    char why[512] = "";
    a->rc = s2a_compile_worker_run(51, 7, a->json, a->len, a->sha, a->deadline,
        NULL, NULL, NULL, &a->bytes, &a->bytes_len, why, sizeof why);
    a->receipt = consume_returned();
    CHECK(!consume_returned(), "worker returned receipt is consumed once");
    free(local_json);
    return NULL;
}
static void init_run(struct run_args *a, const char *json, size_t len, int deadline) {
    memset(a, 0, sizeof *a); a->json = json; a->len = len; a->deadline = deadline;
    s2a_sha256 s; s2a_sha256_init(&s); s2a_sha256_update(&s, (const uint8_t *)json, len); s2a_sha256_final(&s, a->sha);
}
static void wait_entered(void) {
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 3;
    pthread_mutex_lock(&event_mu);
    while (!entered) {
        if (pthread_cond_timedwait(&event_cv, &event_mu, &until)) break;
    }
    CHECK(entered, "actual compiler entered before adverse event");
    pthread_mutex_unlock(&event_mu);
}
static void unblock(void) {
    pthread_mutex_lock(&event_mu); permit_return = 1;
    pthread_cond_broadcast(&event_cv); pthread_mutex_unlock(&event_mu);
}
static int refuse_pause(void *p) { (*(int *)p)++; return -1; }
static void must_not_resume(void *p) { (*(int *)p) += 100; }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *name = argv[1];
    make_error("QueryTextureCoordNormalized has no source (stage=fs,handle=12,cbufSlot=3)", 1);
    if (!strcmp(name, "gate_closed")) export_open = 0;
    if (!strcmp(name, "no_emit")) compiler_mode = 1;
    if (!strcmp(name, "multi_emit")) compiler_mode = 2;
    if (!strcmp(name, "success")) compiler_mode = 3;
    if (!strcmp(name, "sampler_method")) { expected_method = S2A_W2_QUERY_SAMPLER_TYPE; make_error("QuerySamplerType has no source (stage=fs,handle=12,cbufSlot=3)", 0); }
    if (!strcmp(name, "buffer_method")) { expected_method = S2A_W2_QUERY_IS_BUFFER; make_error("QueryTextureIsBuffer has no source (stage=fs,handle=12,cbufSlot=3)", 0); }
    if (!strcmp(name, "format_method")) { expected_method = S2A_W2_QUERY_FORMAT; make_error("QueryTextureFormat has no source (stage=fs,handle=12,cbufSlot=3,format=Unknown)", 0); }
    if (!strcmp(name, "sampled_format_method")) { expected_method = S2A_W2_QUERY_SAMPLED_FORMAT; make_error("QueryS2ASampledTextureFormat has no source (stage=fs,handle=12,cbufSlot=3)", 0); }
    if (!strcmp(name, "escaped_message")) make_error("QueryTextureCoordNormalized has no source (stage=\\u0066s,handle=12,cbufSlot=3)", 0);
    if (!strcmp(name, "bad_method")) make_error("ConstantBuffer1Read has no source (offset=12)", 0);
    if (!strcmp(name, "bad_coordinate")) make_error("QueryTextureCoordNormalized has no source (stage=fs,handle=12,cbufSlot=65536)", 0);
    if (!strcmp(name, "trailing_message")) make_error("QueryTextureCoordNormalized has no source (stage=fs,handle=12,cbufSlot=3) ignored", 0);
    if (!strcmp(name, "duplicate_key")) {
        const char *bad = "{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureCoordNormalized has no source (stage=fs,handle=12,cbufSlot=3)\"}";
        strcpy(error_json, bad); error_len = strlen(error_json);
    }
    if (!strcmp(name, "malformed")) { strcpy(error_json, "{broken"); error_len = strlen(error_json); }
    if (!strcmp(name, "thread_exit")) {
        for (int i = 0; i < 12; i++) {
            pthread_t exited;
            CHECK(!pthread_create(&exited, NULL, exit_with_pending, NULL), "pending builder thread starts");
            CHECK(!pthread_join(exited, NULL), "pending builder exits");
            CHECK(atomic_load(&releases) == i + 1, "thread exit releases its pending binding and capacity");
        }
    }
    int background = !strcmp(name, "timeout") || !strcmp(name, "stop") || !strcmp(name, "dedup");
    int saved_gate = export_open;
    if (background) export_open = 0; /* Expected JSON only; real caller builds below. */
    size_t len = 0; char *json = build(&len);
    export_open = saved_gate;
    if (!json) return 1;
    uint8_t *expected_copy = malloc(len); CHECK(expected_copy != NULL, "expected bytes allocated");
    if (!expected_copy) { free(json); return 1; }
    memcpy(expected_copy, json, len); expected_request = expected_copy; expected_request_len = len;

    int is_worker = !strcmp(name, "worker") || !strcmp(name, "timeout") || !strcmp(name, "stop") || !strcmp(name, "dedup") || !strcmp(name, "pause_failure") || !strcmp(name, "not_running");
    if (!is_worker) {
        char *copy = NULL; const char *used = json;
        if (!strcmp(name, "copy")) { copy = malloc(len); CHECK(copy != NULL, "copy allocated"); if (!copy) return 1; memcpy(copy, json, len); used = copy; }
        struct s2a_compile_outcome out;
        int rc = s2a_compile_request_execute(51, 7, used, len, &out);
        CHECK((compiler_mode == 3) ? rc == 0 : rc == S2A_DIRECT_RES_ARGUMENT, "compile outcome unchanged");
        if (compiler_mode != 3) CHECK(!out.bytes && !out.bytes_len, "failure bytes never become successful program");
        s2a_compile_outcome_free(&out);
        if (!strcmp(name, "stale_return")) {
            char *other = malloc(len); CHECK(other != NULL, "new unrelated allocation exists");
            if (other) {
                memcpy(other, json, len);
                CHECK(s2a_compile_request_execute(51, 7, other, len, &out) == S2A_DIRECT_RES_ARGUMENT, "unattributed failure still compiles");
                s2a_compile_outcome_free(&out); free(other);
            }
        }
        if (!strcmp(name, "replay")) {
            CHECK(s2a_compile_request_execute(51, 7, json, len, &out) == S2A_DIRECT_RES_ARGUMENT, "replay compile unchanged");
            s2a_compile_outcome_free(&out);
        }
        int positive = !strcmp(name, "raw") || !strcmp(name, "replay") || !strcmp(name, "stale_return") || !strcmp(name, "thread_exit") ||
            !strcmp(name, "sampler_method") || !strcmp(name, "buffer_method") ||
            !strcmp(name, "format_method") || !strcmp(name, "sampled_format_method") || !strcmp(name, "escaped_message");
        expect_published(positive ? 1 : 0);
        int returned = consume_returned();
        CHECK(returned == (positive && strcmp(name, "replay") && strcmp(name, "stale_return")), "new unattributed invocation cannot reuse old published receipt");
        CHECK(!consume_returned(), "sync returned receipt is consumed once");
        free(copy);
    } else {
        if (strcmp(name, "not_running")) CHECK(!s2a_compile_worker_start(adapter_worker_compile, NULL, NULL, 0), "actual worker starts");
        struct run_args a; init_run(&a, json, len, !strcmp(name, "timeout") ? 30 : 3000);
        if (!strcmp(name, "worker")) {
            run_worker(&a); CHECK(a.rc == -1 && !a.bytes, "worker wrapper preserves compiler failure");
            CHECK(a.receipt == 1, "worker hands receipt to original caller thread");
            expect_published(1);
        } else if (!strcmp(name, "pause_failure") || !strcmp(name, "not_running")) {
            int callbacks = 0; char why[256];
            a.rc = s2a_compile_worker_run(51, 7, json, len, a.sha, 30,
                !strcmp(name, "pause_failure") ? refuse_pause : NULL,
                !strcmp(name, "pause_failure") ? must_not_resume : NULL,
                &callbacks, &a.bytes, &a.bytes_len, why, sizeof why);
            CHECK(a.rc == -1 && !a.bytes && !atomic_load(&compiler_calls), "early refusal never invokes compiler");
            CHECK(callbacks == (!strcmp(name, "pause_failure") ? 1 : 0), "pause/resume behavior unchanged");
            expect_published(0);
        } else {
            block_compile = 1;
            a.build_here = 1;
            pthread_t first, second; CHECK(!pthread_create(&first, NULL, run_worker, &a), "first waiter starts");
            wait_entered();
            if (!strcmp(name, "timeout")) {
                CHECK(!pthread_join(first, NULL), "timeout waiter returns before compiler release");
                CHECK(a.rc == -1, "timeout is failure"); unblock();
            } else if (!strcmp(name, "stop")) {
                CHECK(s2a_compile_worker_stop_bounded(1, NULL, 0) == 1, "Stop reports still-running actual compiler");
                CHECK(!pthread_join(first, NULL), "Stop wakes waiter"); CHECK(a.rc == -1, "stopped waiter fails"); unblock();
            } else {
                struct run_args b; init_run(&b, json, len, 3000);
                CHECK(!pthread_create(&second, NULL, run_worker, &b), "dedup waiter starts");
                int attached = 0;
                for (int i = 0; i < 2000; i++) {
                    struct s2a_compile_worker_stats st; s2a_compile_worker_stats(&st);
                    if (st.dedup) { attached = 1; break; }
                    struct timespec tick = {0, 1000000}; nanosleep(&tick, NULL);
                }
                CHECK(attached, "second waiter actually attached before compiler release"); unblock();
                CHECK(!pthread_join(first, NULL) && !pthread_join(second, NULL), "both waiters joined");
                CHECK(a.rc == -1 && b.rc == -1 && atomic_load(&compiler_calls) == 1, "ordinary dedup compile remains one failure");
                free(b.bytes);
            }
            CHECK(s2a_compile_worker_stop_bounded(1000, NULL, 0) == 0, "actual worker drained");
            expect_published(0);
            CHECK(!a.receipt, "adverse worker return has no receipt");
        }
        free(a.bytes);
        CHECK(s2a_compile_worker_stop_bounded(1000, NULL, 0) == 0, "worker cleanup completes");
    }
    if (!strcmp(name, "parser_missing")) CHECK(atomic_load(&claims) <= 1, "missing optional parser does not invent attempts");
    else if (export_open) CHECK(atomic_load(&claims) == (!strcmp(name, "thread_exit") ? 13 : 1), "diagnostic claim really armed before case");
    else CHECK(!atomic_load(&claims), "closed gate does not claim source");
    if (is_worker && strcmp(name, "worker")) CHECK(atomic_load(&abandons) == 1, "adverse worker path abandons armed capsule");
    free(expected_copy); free(json);
    printf("%s: %s claims=%d publishes=%d abandons=%d releases=%d compiler_calls=%d\n", name,
        failures ? "FAIL" : "PASS", atomic_load(&claims), atomic_load(&publishes), atomic_load(&abandons), atomic_load(&releases), atomic_load(&compiler_calls));
    return failures ? 1 : 0;
}
