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
// Pipeline kind: 1=guest graphics, 2=guest compute, 3=clear, 4=depth blit, 5=upscale, 6=BC decoder, 7=graphics wait, 8=compute wait.
uint64_t s2a_direct_diag_pipe_begin(unsigned kind, int warm);
// feedback: 0=unknown, 1=valid but no hit reported, 2=valid application-cache hit.
void s2a_direct_diag_pipe_end(uint64_t token, int success, unsigned feedback);
// LibPack lookup outcomes use stable integer reason codes, documented by the reader.
void s2a_direct_diag_pack(const uint8_t identity[32], unsigned reason, int warm);
// Texture upload is CPU preparation/recording, not GPU completion.
uint64_t s2a_direct_diag_upload_begin(unsigned kind, unsigned reason);
void s2a_direct_diag_upload_end(uint64_t token, int success, unsigned method, uint64_t bytes);
