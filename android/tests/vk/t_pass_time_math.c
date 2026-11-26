// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_vk_pass_time.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(nvk_pass_time_host_bound(500, 755, 8, 2.0));
    assert(!nvk_pass_time_host_bound(500, 756, 8, 2.0));
    assert(!nvk_pass_time_host_bound(500, 499, 8, 2.0));

    struct nvk_pass_time_interval gap = nvk_pass_time_between(253, 2, 7, 8);
    assert(gap.gap == 5 && gap.overlap == 0 && gap.advances_end);
    struct nvk_pass_time_interval overlap = nvk_pass_time_between(2, 253, 7, 8);
    assert(overlap.gap == 0 && overlap.overlap == 5 && overlap.advances_end);
    struct nvk_pass_time_interval contained = nvk_pass_time_between(2, 253, 3, 8);
    assert(contained.gap == 0 && contained.overlap == 3 && !contained.advances_end);

    uint64_t offset = 0, elapsed = 0;
    assert(nvk_pass_time_span(250, 254, 3, 12, offset, 8, &offset, &elapsed));
    assert(offset == 9 && elapsed == 5);
    assert(!nvk_pass_time_span(250, 1, 5, 12, offset, 8, &offset, &elapsed));
    assert(!nvk_pass_time_span(250, 4, 7, 12, offset, 8, &offset, &elapsed));
    assert(nvk_pass_time_span(250, 5, 5, 12, offset, 8, &offset, &elapsed) && elapsed == 0);
    puts("GPU pass time math: wrap, gap, overlap, order, and host bound PASS");
}
