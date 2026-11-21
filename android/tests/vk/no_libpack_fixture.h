// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#ifndef S2A_NO_LIBPACK_FIXTURE_H
#define S2A_NO_LIBPACK_FIXTURE_H
#include "rp_shader.h"
#include <stdio.h>
#include <stdlib.h>

// These fixture resolvers supply only synthetic programs. A LibPack fall-back
// is outside their contract, so a test that reaches it must fail explicitly.
int rp_shader_lib_reject(const struct rp_prog *p) {
    if (p && p->lib_sourced) {
        fputs("fixture unexpectedly resolved a LibPack program\n", stderr);
        abort();
    }
    return 0;
}
#endif
