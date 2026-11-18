// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>

// A copied snapshot, never a borrowed replay state or a pipeline identity.
enum { S2A_VUID_NONE, S2A_VUID_COMPILE, S2A_VUID_DRAW, S2A_VUID_DISPATCH, S2A_VUID_PIPE };
struct s2a_vuid_context {
    uint64_t sequence;
    uint64_t frame;
    uint8_t kind, stages, bound;
    uint8_t keys[6][8];
    char api[40]; // recorder's exact VkCmd* entry point; empty on direct path
};
// Optional Android diagnostic. Other platforms and isolated host fixtures omit it.
int s2a_vuid_enabled(void) __attribute__((weak));
void s2a_vuid_start(int) __attribute__((weak));
void s2a_vuid_reset_report(void) __attribute__((weak));
uint64_t s2a_vuid_next(void) __attribute__((weak));
void s2a_vuid_exchange(const struct s2a_vuid_context *, struct s2a_vuid_context *) __attribute__((weak));
const struct s2a_vuid_context *s2a_vuid_current(void) __attribute__((weak));
int s2a_vuid_observe(int32_t, const char *, const char *, char *, size_t) __attribute__((weak));
// Shader handle -> stage key, noted at a successful load and dropped when the
// backend destroys the shader. A pipeline build on any thread scopes itself
// with the keys of the shaders it links (S2A_VUID_PIPE), so a layout or
// interface error at vkCreate*Pipelines names its program.
void s2a_vuid_note_shader(const void *, unsigned, const uint8_t *) __attribute__((weak));
void s2a_vuid_forget_shader(const void *) __attribute__((weak));
int s2a_vuid_pipe_begin(const void *const *, unsigned, struct s2a_vuid_context *) __attribute__((weak));
