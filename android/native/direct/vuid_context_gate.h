// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "../debug_gate.h"

static inline int s2a_vuid_gate_open(int validation_active) {
#if defined(__ANDROID__) && defined(S2A_ANDROID_DEBUG_ENV)
    char value[PROP_VALUE_MAX] = {0};
    return validation_active && s2a_debug_entries_open() &&
        __system_property_get("debug.s2a.vk_validation_ctx", value) == 1 && value[0] == '1';
#else
    (void)validation_active;
    return 0;
#endif
}
