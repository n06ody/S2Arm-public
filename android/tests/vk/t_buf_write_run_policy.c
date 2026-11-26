// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include <vulkan/vulkan.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "nvn_plat.h"

static int g_override = -1;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_buf_write_bar_coalesce")) return g_override < 0 ? dflt : g_override;
    return 1;
}
#include "backend_vulkan_buf_write_run.inc"

static int failures;
static void check(int yes, const char *label) {
    if (!yes) { fprintf(stderr, "FAIL write policy: %s\n", label); failures++; }
}

static int write_range(VkCommandBuffer cb, VkBuffer buf, VkDeviceMemory mem, uint64_t lo, uint64_t bytes) {
    const int skip = bw_before(cb, buf, mem, lo, bytes);
    bw_after(skip);
    return skip;
}

int main(void) {
    const VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)1, next = (VkCommandBuffer)(uintptr_t)2;
    const VkBuffer a = (VkBuffer)(uintptr_t)3, b = (VkBuffer)(uintptr_t)4;
    const VkDeviceMemory ma = (VkDeviceMemory)(uintptr_t)5, mb = (VkDeviceMemory)(uintptr_t)6;
    bw_open();
    check(!g_bw_enabled && !atomic_load(&g_bw_diagnostic), "default off");
    g_override = 1;
    bw_open();
    check(!write_range(cb, a, ma, 0, 4), "first write retains barrier");
    check(write_range(cb, a, ma, 4, 4), "adjacent half-open ranges");
    check(write_range(cb, b, mb, 0, 4), "distinct allocations");
    check(!write_range(cb, a, ma, 2, 4), "overlap with earlier range");
    check(!write_range(cb, b, ma, 12, 4), "unproved alias binding");
    check(!write_range(cb, a, ma, 20, 4), "unknown alias ends run");
    bw_end(BW_INTERVENING);
    check(!write_range(cb, a, ma, 24, 4), "intervening command ends run");
    check(!write_range(next, a, ma, 28, 4), "other command buffer");
    check(!write_range(next, a, VK_NULL_HANDLE, 32, 4), "unknown memory");
    check(!write_range(next, a, ma, UINT64_MAX - 3, 4), "overflowed end");
    check(!write_range(VK_NULL_HANDLE, a, ma, 0, 4), "unknown command buffer");
    bw_end(BW_SEGMENT);
    for (unsigned i = 0; i <= 16; i++)
        check(write_range(cb, a, ma, i * 4, 4) == (i > 0 && i < 16), "capacity boundary at 16");
    check(write_range(cb, a, ma, 68, 4), "capacity starts a new run");
    check(atomic_load(&g_bw_counts[BW_OVERLAP]) == 1, "overlap count");
    check(atomic_load(&g_bw_counts[BW_CAPACITY]) == 1, "capacity count");
    check(atomic_load(&g_bw_counts[BW_UNKNOWN]) == 4, "unknown count");
    check(atomic_load(&g_bw_counts[BW_INTERVENING]) == 1, "intervening count");
    check(sizeof g_bw_names / sizeof *g_bw_names == BW_N, "named counters");
    (void)g_bw_last_present;
    printf("write policy failures=%d\n", failures);
    return failures != 0;
}
