#include "nvn_backend.h"
#include "nvn_vk_idle.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
    struct idle_account a = {.window_start = 10};
    struct idle_bucket b;
    idle_sample(&a, 8, 1, 220, 245, 10, 40, 1, 0);
    idle_sample(&a, 8, 1, 250, 3, 41, 55, 1, 1);
    idle_sample(&a, 8, 1, 9, 28, 56, 79, 1, 0);
    idle_sample(&a, 8, 1, 15, 20, 60, 81, 1, 0); // entirely overlaps prior segment
    idle_sample(&a, 8, 1, 30, 40, 82, 95, 1, 0);
    CHECK(a.bucket.frames == 1 && a.bucket.samples == 5);
    CHECK(a.bucket.gaps[IDLE_IN_FRAME] == 3 && a.bucket.ticks[IDLE_IN_FRAME] == 7);
    CHECK(a.bucket.gaps[IDLE_AFTER_PRESENT] == 1 && a.bucket.ticks[IDLE_AFTER_PRESENT] == 6);
    CHECK(a.bucket.overlap == 1);
    idle_sample(&a, 8, 1, 0, 0, 96, 100, 0, 0); // unavailable breaks the chain
    idle_sample(&a, 8, 1, 75, 100, 101, 127, 1, 0);
    CHECK(a.bucket.missing == 1 && a.bucket.gaps[0] == 3);
    idle_sample(&a, 8, 1, 120, 126, 257, 265, 1, 1); // half-cycle ambiguity
    CHECK(a.bucket.ambiguous == 1 && a.bucket.gaps[0] == 3);
    idle_sample(&a, 0, 1, 0, 0, 270, 280, 1, 0);
    CHECK(a.bucket.missing == 2 && !a.have_end);
    idle_sample(&a, 64, 1, 400, 410, 4999999980ull, 4999999990ull, 1, 1);
    idle_wait(&a.bucket, NVN_GPU_WAIT_SYNC, 9, 1);
    idle_wait(&a.bucket, NVN_GPU_WAIT_SYNC, 11, 0); // same wait, second wake
    idle_wait(&a.bucket, NVN_GPU_WAIT_FINISH, 37, 1);
    CHECK(!idle_take(&a, 5000000009ull, &b));
    CHECK(idle_take(&a, 5000000010ull, &b));
    CHECK(b.waits[NVN_GPU_WAIT_SYNC] == 1 && b.wait_ns[NVN_GPU_WAIT_SYNC] == 20);
    CHECK(b.waits[NVN_GPU_WAIT_FINISH] == 1 && b.wait_ns[NVN_GPU_WAIT_FINISH] == 37);
    CHECK(!a.bucket.samples && !a.bucket.waits[NVN_GPU_WAIT_SYNC]);
    idle_sample(&a, 64, 1, 415, 430, 5000000010ull, 5000000030ull, 1, 0);
    CHECK(a.bucket.gaps[IDLE_AFTER_PRESENT] == 1 && a.bucket.ticks[IDLE_AFTER_PRESENT] == 5);
    puts("PASS GPU idle: wrap, overlap, present boundary, lost sample, ambiguity, wait sites and bucket rollover");
    return 0;
}
