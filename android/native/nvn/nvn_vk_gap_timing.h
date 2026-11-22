#ifndef S2A_NVNVK_GAP_TIMING_H
#define S2A_NVNVK_GAP_TIMING_H

#include <stdint.h>

enum nvk_gap_kind {
    NVK_GAP_NONE, NVK_GAP_BARRIER, NVK_GAP_LAYOUT, NVK_GAP_TRANSFER, NVK_GAP_CLEAR,
    NVK_GAP_COMPUTE, NVK_GAP_PASS_BEGIN, NVK_GAP_PASS_END, NVK_GAP_KIND_COUNT
};
enum { NVK_GAP_OFF, NVK_GAP_TIME, NVK_GAP_EMPTY };
enum { NVK_GAP_PAIRS = 128 };
struct nvk_gap_row { uint8_t kind; uint16_t window; };
struct nvk_gap_plan {
    uint8_t mode, active;
    uint16_t window, n, current;
    uint32_t dropped;
    struct nvk_gap_row rows[NVK_GAP_PAIRS];
};
typedef void (*nvk_gap_stamp_fn)(void *user, uint32_t index);

void nvk_gap_plan_step(struct nvk_gap_plan *p, enum nvk_gap_kind kind,
                        uint16_t window, nvk_gap_stamp_fn stamp, void *user);

#ifdef S2A_NVK_GAP_HOST_TEST
void nvk_gap_test_present(uint64_t seq, uint64_t elapsed_seconds);
#endif
#endif
