// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stdint.h>
// Diagnostic-only, enabled at session start in a debug package. Returns -1 on capture setup failure.
int s2a_direct_diag_start(const char *directory, uint64_t session);
int s2a_direct_diag_enabled(void);
void s2a_direct_diag_frame(void);
uint64_t s2a_direct_diag_compile_begin(const uint8_t identity[32], unsigned stages, int submission, int warm_hit);
void s2a_direct_diag_compile_end(uint64_t token, int success);
