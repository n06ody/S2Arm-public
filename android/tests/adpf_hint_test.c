// Host contract: only Android's ADPF API, thread identity, and monotonic clock are faked.
#include "adpf_hint.h"
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failed++; } } while (0)
static int64_t clock_ns;
static int32_t thread_id;
static int creates, reports, closes, updates, fail_report, fail_create, fail_update;
static int enabled = 1;
int s2a_adpf_test_enabled(void) { return enabled; }
static int32_t created_tid;
static int64_t created_target, actual, updated_target;
uint64_t s2a_adpf_test_now(void) { return (uint64_t)clock_ns; }
int32_t s2a_adpf_test_tid(void) { return thread_id; }
void *s2a_adpf_test_create(int32_t tid, int64_t target) {
    creates++; created_tid=tid; created_target=target;
    return fail_create ? NULL : (void *)(uintptr_t)1;
}
int s2a_adpf_test_update(void *s, int64_t target) { (void)s; updates++; updated_target=target; return fail_update; }
int s2a_adpf_test_report(void *s, int64_t ns) { (void)s; reports++; actual=ns; return fail_report; }
void s2a_adpf_test_close(void *s) { (void)s; closes++; }
int main(void) {
    s2a_adpf_test_reset();
    s2a_adpf_set_foreground(1); s2a_adpf_set_surface(1);
    // First frame: session is created for the worker tid at the present period;
    // the reported span is measured begin->end, never the interval itself.
    thread_id=42; clock_ns=1000000000; s2a_adpf_work_begin(1);
    clock_ns=1005000000; s2a_adpf_work_begin(1);           // same frame: first begin wins
    clock_ns=1012000000; s2a_adpf_frame_end(16666667, 1);
    CHECK(creates==1 && created_tid==42 && created_target==16666667);
    CHECK(reports==1 && actual==12000000);
    // Same target is not re-updated; a real target change is.
    clock_ns=2000000000; s2a_adpf_work_begin(2); clock_ns=2020000000; s2a_adpf_frame_end(16666667, 2);
    CHECK(updates==0 && reports==2 && actual==20000000);
    clock_ns=2030000000; s2a_adpf_work_begin(3); clock_ns=2040000000; s2a_adpf_frame_end(33333334, 3);
    CHECK(updates==1 && updated_target==33333334 && reports==3 && actual==10000000);
    // A failed report closes the session and rate-limits a retry for a second.
    fail_report=1; clock_ns=2050000000; s2a_adpf_work_begin(4); clock_ns=2060000000; s2a_adpf_frame_end(33333334, 4);
    CHECK(reports==4 && closes==1);
    fail_report=0; clock_ns=2500000000; s2a_adpf_work_begin(5); clock_ns=2510000000; s2a_adpf_frame_end(33333334, 5);
    CHECK(creates==1 && reports==4);
    clock_ns=3061000000; s2a_adpf_work_begin(6); clock_ns=3071000000; s2a_adpf_frame_end(33333334, 6);
    CHECK(creates==2 && reports==5);
    // Backgrounding closes the session; work while backgrounded is not recorded.
    s2a_adpf_set_foreground(0); CHECK(closes==2);
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
    clock_ns=7700000000; s2a_adpf_work_begin(28); clock_ns=7710000000; s2a_adpf_frame_end(16666667, 28);
    CHECK(creates==10 && reports==14);
    if (failed) return 1;
    printf("adpf host lifecycle: passed\n"); return 0;
}
