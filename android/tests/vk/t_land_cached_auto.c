// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_vk_land_cached_auto.h"

#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv) {
    if (argc == 2) {
        // 同 P0 包 708df6d8 原局日志的活动窗量级。重叠项用已知合成区间独立测算。
        struct mirror_auto_window candidate = {
            .frame_ms = 25.063, .p99_ms = 38.0, .present_ms = 0.6, .acquire_ms = 0.2,
            .land_bytes = 29294272, .draws = 1050, .submits = 7, .sync_bytes = 3e6,
            .copy_ms = 15.860, .shadow_wait_ms = 9.134, .target_ns = 16666666, .thermal = 0,
        };
        if (!strcmp(argv[1], "--retire-overlap")) {
            struct mirror_auto_window gpu_wait = candidate;
            CHECK(mirror_auto_observe(&gpu_wait, 0, 0) != NULL);
            CHECK(mirror_auto_observe(&candidate, 0, 7.0) == NULL);
            CHECK(fabs(candidate.shadow_wait_ms - 5.634) < 0.001);
            CHECK(mirror_auto_overlap_ns(10, 25, 12, 20) == 5);
            CHECK(mirror_auto_overlap_ns(10, 25, 26, 20) == 0);
            CHECK(mirror_auto_overlap_ns(10, 25, 12, 25) == 0);
            struct mirror_auto_window expensive = candidate;
            expensive.copy_ms = 28.29;
            expensive.shadow_wait_ms = 32.926;
            CHECK(mirror_auto_observe(&expensive, 0, 28.29) != NULL);
        } else {
            CHECK(!strcmp(argv[1], "--direct-upload"));
            candidate.shadow_wait_ms = 5.634;
            struct mirror_auto_window heavy = candidate;
            CHECK(mirror_auto_observe(&heavy, 2600, 0) != NULL);
            CHECK(mirror_auto_observe(&candidate, 121.80, 0) == NULL);
            CHECK(fabs(candidate.shadow_wait_ms - 5.769) < 0.001);
            heavy = candidate;
            heavy.sync_bytes = 64 * 1024;
            CHECK(mirror_auto_observe(&heavy, 121.80, 0) != NULL);
        }
        puts("land-cached auto observe model: passed");
        return 0;
    }
    CHECK(argc == 1);
    const struct mirror_auto_window baseline = {
        .frame_ms = 29.3, .p99_ms = 47.2, .present_ms = 0.9, .acquire_ms = 0.2,
        .land_bytes = 21e6, .draws = 1050, .submits = 7, .sync_bytes = 3e6,
        .copy_ms = 12, .shadow_wait_ms = 2.5, .target_ns = 16666667, .thermal = 0,
    };
    CHECK(mirror_auto_screen(&baseline));
    struct mirror_auto_window negative = baseline;
    negative.shadow_wait_ms = 29;
    CHECK(!mirror_auto_screen(&negative));
    const uint8_t compatibility[32] = {1};
    struct mirror_auto_receipt receipt = {.version = MIRROR_AUTO_VERSION, .created = 600};
    memcpy(receipt.compatibility, compatibility, sizeof compatibility);
    receipt.baseline[0] = receipt.baseline[1] = baseline;
    CHECK(mirror_auto_receipt_valid(&receipt, compatibility, 900));
    receipt.version = 1;
    CHECK(!mirror_auto_receipt_valid(&receipt, compatibility, 900));
    receipt.version = MIRROR_AUTO_VERSION;
    CHECK(!mirror_auto_receipt_valid(&receipt, compatibility, 599));
    CHECK(!mirror_auto_receipt_valid(&receipt, compatibility, 600 + MIRROR_AUTO_RECEIPT_SEC + 1));
    receipt.compatibility[0] = 0;
    CHECK(!mirror_auto_receipt_valid(&receipt, compatibility, 900));
    receipt.compatibility[0] = 1;
    receipt.baseline[1].frame_ms = NAN;
    CHECK(!mirror_auto_receipt_valid(&receipt, compatibility, 900));
    negative = baseline;
    negative.frame_ms = 16.8;
    CHECK(!mirror_auto_screen(&negative));

    const struct mirror_auto_window off[2] = {baseline, baseline};
    struct mirror_auto_window on[2] = {baseline, baseline};
    for (unsigned i = 0; i < 2; i++) {
        on[i].frame_ms = 22.9;
        on[i].p99_ms = 33.9;
        on[i].copy_ms = 2.1;
        on[i].maintenance_ms = 0.8;
        on[i].wait_ms = 2.3;
    }
    CHECK(mirror_auto_accept(off, on));
    on[1].thermal = 1;
    CHECK(!mirror_auto_accept(off, on));
    on[1].thermal = 0;
    on[1].land_bytes *= 0.5;
    CHECK(!mirror_auto_accept(off, on));
    on[1].land_bytes = baseline.land_bytes;
    on[1].p99_ms = 70.0;
    CHECK(!mirror_auto_accept(off, on));
    on[1] = on[0];
    on[1].present_ms = 12.0;
    CHECK(!mirror_auto_accept(off, on));
    on[1] = on[0];
    on[1].wait_ms = 30.0;
    CHECK(!mirror_auto_accept(off, on));
    on[1] = on[0];
    on[1].frame_ms = 29.0;
    CHECK(!mirror_auto_accept(off, on));
    puts("land-cached auto policy: candidate, capped, pressure, paired gain, load and thermal controls passed");
    return 0;
}
