// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once

#include <stdint.h>

#if defined(__ANDROID__) || defined(S2A_WAIT_TRACE_TEST)
enum { NVN_WAIT_TRACE_READY, NVN_WAIT_TRACE_ALLOWED };
void s2a_nvk_wait_trace_publish(const void *sync, uint64_t fence) __attribute__((weak));
uint64_t s2a_nvk_wait_trace_begin(const void *sync, uint64_t timeout, uintptr_t site) __attribute__((weak));
void s2a_nvk_wait_trace_end(uint64_t call, uint64_t fence, int result) __attribute__((weak));
void s2a_nvk_wait_trace_fast(uint64_t fence, uint64_t completed, uint64_t timeout, int path) __attribute__((weak));
#endif
