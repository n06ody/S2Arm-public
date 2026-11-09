/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 S2Arm contributors */
#include "system_tuning.h"
#include <stdatomic.h>

static _Atomic int battery_limit;
static _Atomic uint64_t present_ns;

void s2a_android_set_battery_limit(int enabled) { atomic_store(&battery_limit, !!enabled); }
int s2a_android_battery_limit(void) { return atomic_load(&battery_limit); }
uint64_t s2a_android_limit_frame_period(uint64_t period_ns) {
    const uint64_t min_ns = 33333334ull;  // ceil(1 second / 30), applied to the final period
    return s2a_android_battery_limit() && period_ns < min_ns ? min_ns : period_ns;
}

void s2a_android_present_note(uint64_t period_ns) { atomic_store(&present_ns, period_ns); }
uint64_t s2a_android_present_ns(void) { return atomic_load(&present_ns); }
