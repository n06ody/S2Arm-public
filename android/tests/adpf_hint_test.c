// Host contract: only Android's ADPF API, thread identity, and monotonic clock are faked.
#include "adpf_hint.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failed++; } } while (0)
static int64_t clock_ns;
static int32_t thread_id;
static int creates, reports, closes, updates, fail_report, fail_create, fail_update;
static int active_during_report;
static int enabled = 1;
static int real_cpu_enabled = 1;
int s2a_adpf_test_enabled(void) { return enabled; }
int s2a_adpf_test_cpu_work_enabled(void) { return real_cpu_enabled; }
static int32_t created_tid;
static int32_t recording_tid, created_recorder;
static size_t created_threads;
static size_t updated_threads;
static int set_threads_calls, set_threads_result;
static int adpf_supported = 1;
int s2a_adpf_test_supported(void) { return adpf_supported; }
static int64_t created_target, actual, updated_target;
uint64_t s2a_adpf_test_now(void) { return (uint64_t)clock_ns; }
int32_t s2a_adpf_test_tid(void) { return thread_id; }
int32_t s2a_adpf_recorder_tid(void) { return recording_tid; }
void *s2a_adpf_test_create(const int32_t *tids, size_t count, int64_t target) {
    creates++; created_tid=tids[0]; created_target=target;
    created_threads = count;
    created_recorder = count == 2 ? tids[1] : 0;
    return fail_create ? NULL : (void *)(uintptr_t)1;
}
int s2a_adpf_test_set_threads(void *s, const int32_t *tids, size_t count) {
    (void)s;
    CHECK(tids[0] == thread_id);
    set_threads_calls++;
    updated_threads = count;
    return set_threads_result;
}
int s2a_adpf_test_update(void *s, int64_t target) { (void)s; updates++; updated_target=target; return fail_update; }
int s2a_adpf_test_report(void *s, int64_t ns) {
    (void)s;
    active_during_report = s2a_adpf_session_active();
    reports++; actual=ns; return fail_report;
}
void s2a_adpf_test_close(void *s) { (void)s; closes++; }
int main(void) {
    s2a_adpf_test_reset();
    CHECK(!s2a_adpf_session_active());
    s2a_adpf_set_foreground(1); s2a_adpf_set_surface(1);
    // First frame: session is created for the worker tid at the present period;
    // the reported span is measured begin->end, never the interval itself.
    thread_id=42; clock_ns=1000000000; s2a_adpf_work_begin(1);
    clock_ns=1005000000; s2a_adpf_work_begin(1);           // same frame: first begin wins
    clock_ns=1012000000; s2a_adpf_frame_end(16666667, 1);
    CHECK(creates==1 && created_tid==42 && created_target==16666667);
    CHECK(reports==1 && actual==12000000);
    CHECK(!active_during_report && s2a_adpf_session_active());
    // Same target is not re-updated; a real target change is.
    clock_ns=2000000000; s2a_adpf_work_begin(2); clock_ns=2020000000; s2a_adpf_frame_end(16666667, 2);
    CHECK(updates==0 && reports==2 && actual==20000000);
    clock_ns=2030000000; s2a_adpf_work_begin(3); clock_ns=2040000000; s2a_adpf_frame_end(33333334, 3);
    CHECK(updates==1 && updated_target==33333334 && reports==3 && actual==10000000);
    // A failed report closes the session and rate-limits a retry for a second.
    fail_report=1; clock_ns=2050000000; s2a_adpf_work_begin(4); clock_ns=2060000000; s2a_adpf_frame_end(33333334, 4);
    CHECK(reports==4 && closes==1);
    CHECK(!s2a_adpf_session_active());
    fail_report=0; clock_ns=2500000000; s2a_adpf_work_begin(5); clock_ns=2510000000; s2a_adpf_frame_end(33333334, 5);
    CHECK(creates==1 && reports==4);
    clock_ns=3061000000; s2a_adpf_work_begin(6); clock_ns=3071000000; s2a_adpf_frame_end(33333334, 6);
    CHECK(creates==2 && reports==5);
    CHECK(s2a_adpf_session_active());
    // Backgrounding closes the session; work while backgrounded is not recorded.
    s2a_adpf_set_foreground(0); CHECK(closes==2);
    CHECK(!s2a_adpf_session_active());
    clock_ns=3200000000; s2a_adpf_work_begin(7); clock_ns=3210000000; s2a_adpf_frame_end(33333334, 7);
    CHECK(creates==2 && reports==5);
    s2a_adpf_set_foreground(1);
    clock_ns=3300000000; s2a_adpf_work_begin(8); clock_ns=3310000000; s2a_adpf_frame_end(33333334, 8);
    CHECK(creates==3 && reports==6);
    // A span whose present never ran (seq mismatch) is dropped, not charged on.
    clock_ns=3400000000; s2a_adpf_work_begin(9);
    clock_ns=3450000000; s2a_adpf_frame_end(33333334, 10);   // present skipped frame 9
    CHECK(reports==6);
    // ...and a later frame's first job replaces the stale record.
    clock_ns=3500000000; s2a_adpf_work_begin(11);
    clock_ns=3510000000; s2a_adpf_frame_end(33333334, 11);
    CHECK(reports==7 && actual==10000000);
    // A stale pending record from before the mismatch is likewise discarded.
    clock_ns=3600000000; s2a_adpf_work_begin(12);          // frame 12 work begins
    clock_ns=3610000000; s2a_adpf_work_begin(13);          // rollover: 12 never presented
    clock_ns=3620000000; s2a_adpf_frame_end(33333334, 13); // reports 13's span, not 12's
    CHECK(reports==8 && actual==10000000);
    // invisible() closes without touching host lifecycle; next visible frame recreates.
    s2a_adpf_invisible(); CHECK(closes==3);
    clock_ns=3700000000; s2a_adpf_work_begin(14); clock_ns=3710000000; s2a_adpf_frame_end(33333334, 14);
    CHECK(creates==4 && reports==9);
    // Surface loss closes and gates recording; restore resumes.
    s2a_adpf_set_surface(0); CHECK(closes==4);
    CHECK(!s2a_adpf_session_active());
    clock_ns=3800000000; s2a_adpf_work_begin(15); clock_ns=3810000000; s2a_adpf_frame_end(33333334, 15);
    CHECK(creates==4 && reports==9);
    s2a_adpf_set_surface(1);
    clock_ns=3900000000; s2a_adpf_work_begin(16); clock_ns=3910000000; s2a_adpf_frame_end(33333334, 16);
    CHECK(creates==5 && reports==10);
    // debug.s2a.adpf=0 (faked): nothing is recorded or reported while off, and
    // the open session is closed. The property is re-polled only ~1/s, so the
    // re-enable frame must sit more than a second later.
    enabled=0; clock_ns=4100000000; s2a_adpf_work_begin(17); clock_ns=4110000000; s2a_adpf_frame_end(33333334, 17);
    CHECK(creates==5 && reports==10 && closes==5);
    CHECK(!s2a_adpf_session_active());
    enabled=1; clock_ns=5200000000; s2a_adpf_work_begin(18); clock_ns=5210000000; s2a_adpf_frame_end(33333334, 18);
    CHECK(creates==6 && reports==11);
    // A changed owner replaces the session; no work from different threads is mixed.
    thread_id=43; clock_ns=5300000000; s2a_adpf_work_begin(19); clock_ns=5310000000; s2a_adpf_frame_end(33333334, 19);
    CHECK(creates==7 && created_tid==43 && closes==6 && reports==12);
    // update failure also closes + retries after a second.
    fail_update=1; clock_ns=5400000000; s2a_adpf_work_begin(20); clock_ns=5410000000; s2a_adpf_frame_end(16666667, 20);
    CHECK(updates==2 && closes==7 && reports==12);
    fail_update=0; clock_ns=5500000000; s2a_adpf_work_begin(21); clock_ns=5510000000; s2a_adpf_frame_end(16666667, 21);
    CHECK(creates==7 && reports==12);
    clock_ns=6420000000; s2a_adpf_work_begin(22); clock_ns=6430000000; s2a_adpf_frame_end(16666667, 22);
    CHECK(creates==8 && reports==13);
    // create failure retries after a second too (close first: a live session
    // would just report instead of creating).
    s2a_adpf_invisible(); CHECK(closes==8);
    fail_create=1; clock_ns=6500000000; s2a_adpf_work_begin(23); clock_ns=6510000000; s2a_adpf_frame_end(16666667, 23);
    CHECK(creates==9 && reports==13);
    CHECK(!s2a_adpf_session_active());
    fail_create=0; clock_ns=6600000000; s2a_adpf_work_begin(24); clock_ns=6610000000; s2a_adpf_frame_end(16666667, 24);
    CHECK(creates==9 && reports==13);
    clock_ns=7520000000; s2a_adpf_work_begin(25); clock_ns=7530000000; s2a_adpf_frame_end(16666667, 25);
    CHECK(creates==10 && reports==14);
    // A present without work reports nothing; a zero/negative target reports nothing.
    clock_ns=7600000000; s2a_adpf_frame_end(16666667, 26);
    CHECK(reports==14);
    clock_ns=7610000000; s2a_adpf_work_begin(27); clock_ns=7620000000; s2a_adpf_frame_end(0, 27);
    CHECK(reports==14);
    // Stop latches: closes the session and blocks further work.
    s2a_adpf_stop(); CHECK(closes==9);
    CHECK(!s2a_adpf_session_active());
    clock_ns=7700000000; s2a_adpf_work_begin(28); clock_ns=7710000000; s2a_adpf_frame_end(16666667, 28);
    CHECK(creates==10 && reports==14);

    s2a_adpf_test_reset();
    s2a_adpf_set_foreground(1);
    s2a_adpf_set_surface(1);
    thread_id = 42;
    recording_tid = 71;
    clock_ns = 8000000000;
    s2a_adpf_work_begin(1);
    clock_ns = 8009000000;
    s2a_adpf_exclude_gpu_wait(8002000000, clock_ns);
    clock_ns = 8012000000;
    s2a_adpf_frame_end(16666667, 1);
    CHECK(created_threads == 2 && created_tid == 42 && created_recorder == 71);
    CHECK(created_target == 16666667);
    CHECK(actual == 5000000); // 2 ms of work, 7 ms fence wait, 3 ms of work.

    // Another thread's wait and a late wait from the last frame stay out.
    clock_ns = 8020000000;
    s2a_adpf_work_begin(2);
    thread_id = 43;
    s2a_adpf_exclude_gpu_wait(8020000000, 8022000000);
    thread_id = 42;
    s2a_adpf_exclude_gpu_wait(8011000000, 8022000000);
    clock_ns = 8030000000;
    s2a_adpf_frame_end(16666667, 2);
    CHECK(actual == 10000000 && set_threads_calls == 0);

    // API 34+: disabling two-level recording removes the recorder in place.
    int previous_creates = creates;
    recording_tid = 0;
    clock_ns = 8040000000;
    s2a_adpf_work_begin(3);
    clock_ns = 8050000000;
    s2a_adpf_frame_end(16666667, 3);
    CHECK(set_threads_calls == 1 && updated_threads == 1 && creates == previous_creates);

    // API 33: an absent setThreads entry point requires a fresh session.
    const int previous_closes = closes;
    set_threads_result = ENOSYS;
    recording_tid = 72;
    clock_ns = 8060000000;
    s2a_adpf_work_begin(4);
    clock_ns = 8070000000;
    s2a_adpf_frame_end(16666667, 4);
    CHECK(creates == previous_creates + 1 && closes == previous_closes + 1);
    CHECK(created_threads == 2 && created_recorder == 72 && actual == 10000000);

    // Changing presenter midway cannot complete the previous thread's span.
    int previous_reports = reports;
    clock_ns = 8080000000;
    s2a_adpf_work_begin(5);
    clock_ns = 8090000000;
    thread_id = 43;
    s2a_adpf_frame_end(16666667, 5);
    CHECK(reports == previous_reports);
    thread_id = 42;

    // The diagnostic rollback retains the old single-TID, full-span report.
    real_cpu_enabled = 0;
    clock_ns = 9100000000;
    s2a_adpf_work_begin(6);
    s2a_adpf_exclude_gpu_wait(9102000000, 9109000000);
    clock_ns = 9112000000;
    s2a_adpf_frame_end(16666667, 6);
    CHECK(created_threads == 1 && actual == 12000000);

    // Without base NDK entry points no platform session is attempted.
    s2a_adpf_test_reset();
    s2a_adpf_set_foreground(1);
    s2a_adpf_set_surface(1);
    real_cpu_enabled = 1;
    adpf_supported = 0;
    previous_creates = creates;
    previous_reports = reports;
    clock_ns = 9200000000;
    s2a_adpf_work_begin(7);
    clock_ns = 9210000000;
    s2a_adpf_frame_end(16666667, 7);
    CHECK(creates == previous_creates && reports == previous_reports);

    if (failed) return 1;
    printf("adpf host lifecycle: passed\n"); return 0;
}
