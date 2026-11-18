// SPDX-License-Identifier: Apache-2.0
// sys/system_properties.h is copied from the real NDK by the runner; the stub
// implements its declared signature and the real return-length semantics.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "direct/vuid_context_gate.h"
static const char *env_value = "", *ctx_value = "";
static int env_reads, ctx_reads, bad;
int __system_property_get(const char *name, char *out) {
    const char *v;
    if (!strcmp(name, "debug.s2a.debug_env")) { env_reads++; v = env_value; }
    else if (!strcmp(name, "debug.s2a.vk_validation_ctx")) { ctx_reads++; v = ctx_value; }
    else abort();
    size_t n = strlen(v); if (n >= PROP_VALUE_MAX) abort();
    memcpy(out, v, n + 1); return (int)n;
}
#define CHECK(c) do { if (!(c)) { bad++; fprintf(stderr, "【红】gate line %d: %s\n", __LINE__, #c); } } while (0)
int main(void) {
    env_value = "1"; ctx_value = "1";
    CHECK(!s2a_vuid_gate_open(0) && env_reads == 0 && ctx_reads == 0);
#if defined(S2A_ANDROID_DEBUG_ENV)
    CHECK(s2a_vuid_gate_open(1));
    env_value = "0"; ctx_reads = 0; CHECK(!s2a_vuid_gate_open(1) && ctx_reads == 0);
    env_value = "10"; CHECK(!s2a_vuid_gate_open(1) && ctx_reads == 0);
    env_value = "1"; ctx_value = ""; CHECK(!s2a_vuid_gate_open(1));
    ctx_value = "10"; CHECK(!s2a_vuid_gate_open(1));
    ctx_value = "0"; CHECK(!s2a_vuid_gate_open(1));
#else
    CHECK(!s2a_vuid_gate_open(1) && env_reads == 0 && ctx_reads == 0);
#endif
    printf("vuid_context_gate: build=%s bad=%d\n",
#if defined(S2A_ANDROID_DEBUG_ENV)
        "debug",
#else
        "release",
#endif
        bad);
    return bad != 0;
}
