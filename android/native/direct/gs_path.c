/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 S2Arm contributors */
#include <stdbool.h>

#include "gs_path.h"
#include "../capprobe/capprobe.h"
#include "../debug_gate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <android/log.h>
#define S2A_GSLOG(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#else
#define S2A_GSLOG(...) fprintf(stderr, __VA_ARGS__)
#endif

/* debug.s2a.gs_path value read: a system property on Android, the
 * same-named environment variable on the host (prop_int's convention
 * in vk_device.c). The switch itself is behind s2a_debug_entries_open,
 * so on player builds this is never even consulted. */
static void prop_str(const char *name, char *out, size_t n) {
#if defined(__ANDROID__)
    char b[PROP_VALUE_MAX] = {0};
    __system_property_get(name, b);
    snprintf(out, n, "%s", b);
#else
    const char *e = getenv(name);
    snprintf(out, n, "%s", e ? e : "");
#endif
}

int s2a_caps_force_no_vertex_stores(void) {
    if (!s2a_debug_entries_open()) return 0;
    char v[8]; prop_str("debug.s2a.caps_no_vtx_stores", v, sizeof v);
    return v[0] == '1';
}
unsigned s2a_caps_force_subgroup(void) {
    if (!s2a_debug_entries_open()) return 0;
    char v[16]; prop_str("debug.s2a.caps_subgroup", v, sizeof v);
    unsigned n = (unsigned)strtoul(v, NULL, 10);
    return n >= 4 && n <= 128 && !(n & (n - 1)) ? n : 0;
}

/* 0 = verdict decides, 1 = force compute, 2 = force hardware. */
static int gs_force(void) {
    if (!s2a_debug_entries_open()) return 0;
    char v[24];
    prop_str("debug.s2a.gs_path", v, sizeof v);
    if (!strcmp(v, "compute")) return 1;
    if (!strcmp(v, "hardware")) return 2;
    return 0;
}

int s2a_vk_gs_path(int hw_geometry_shader, const char **reason) {
    int path = 0;
    const char *why = "hw-missing";
    if (hw_geometry_shader) {
        const char *state = s2a_capprobe_gs_occ_state
            ? s2a_capprobe_gs_occ_state() : "no-probe";
        const char *verdict = strrchr(state, ':');
        path = verdict && !strcmp(verdict + 1, "supported");
        why = state;
    }
    switch (gs_force()) {
    case 1:
        path = 0; why = "forced-compute";
        break;
    case 2:
        if (hw_geometry_shader) { path = 1; why = "forced-hardware"; }
        else { path = 0; why = "forced-hardware-hw-missing"; }
        break;
    }
    /* Positive line every device creation — the 9400 A/B readings grep
     * for the selected leg, not only a fallback warning. */
    S2A_GSLOG("graphics GS path: %s reason=%s\n",
              path ? "hardware" : "compute", why);
    if (reason) *reason = why;
    return path;
}
