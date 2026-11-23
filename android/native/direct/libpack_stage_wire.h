// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../nvn/nvn_vk_program_result.h"
#include "graphics.h"
#include "s2a_direct.h"

// S2SB/1. Integers on disk are little endian. Never persist pointers or ABI padding.
// Each indexed stage carries the legacy outcome and the outcome when another stage
// promotes the program to v2. Additional v2 support bindings are appended program-wide.
enum {
  SB_HEADER = 64,
  SB_SECTION = 32,
  SB_META = 1,
  SB_BASE,
  SB_STRICT,
  SB_DATA,
  SB_SECTIONS = 4,
  SB_META_SIZE = 64,
  SB_META_WINDOW_SIZE = SB_META_SIZE + 8,  // S2SB/2 appends the SSBO window mode and SPIR-V id.
  SB_CHOICE = 96,
  SB_BINDING = 104
};
static inline uint32_t sb_u32(const uint8_t* p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t sb_u64(const uint8_t* p) {
  return sb_u32(p) | (uint64_t)sb_u32(p + 4) << 32;
}
static inline void sb_w32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}
static inline void sb_w64(uint8_t* p, uint64_t v) {
  sb_w32(p, (uint32_t)v);
  sb_w32(p + 4, (uint32_t)(v >> 32));
}
static inline void sb_word_write(uint8_t* d, const struct fd_ubo_word* w) {
  d[0] = w->slot;
  d[1] = w->sid;
  d[2] = w->pair;
  d[3] = w->pair_slot;
  d[4] = w->pair_sid;
  d[5] = (uint8_t)w->offset;
  d[6] = (uint8_t)(w->offset >> 8);
  d[7] = (uint8_t)w->pair_offset;
  d[8] = (uint8_t)(w->pair_offset >> 8);
}
static inline void sb_word_read(const uint8_t* d, struct fd_ubo_word* w) {
  w->slot = d[0];
  w->sid = d[1];
  w->pair = d[2];
  w->pair_slot = d[3];
  w->pair_sid = d[4];
  w->offset = (uint16_t)(d[5] + 256u * d[6]);
  w->pair_offset = (uint16_t)(d[7] + 256u * d[8]);
}
static inline void sb_put16(uint8_t* d, uint16_t v) {
  d[0] = (uint8_t)v;
  d[1] = (uint8_t)(v >> 8);
}
static inline uint16_t sb_get16(const uint8_t* d) {
  return (uint16_t)(d[0] + 256u * d[1]);
}

static inline void sb_binding_write(uint8_t* d, const struct fd_binding* b,
                                    const struct nvn_vk_compute_storage* s) {
  sb_w32(d, b->set);
  sb_w32(d + 4, b->binding);
  sb_w32(d + 8, b->kind);
  sb_w32(d + 12, b->slot);
  sb_put16(d + 16, b->count);
  memcpy(d + 18, b->stage, 16);
  d[34] = (uint8_t)b->stage_idx;
  d[35] = b->texel_buffer;
  sb_put16(d + 36, b->tex_slot);
  sb_put16(d + 38, b->samp_slot);
  d[40] = b->pool;
  d[41] = b->tex_ubo;
  d[42] = b->samp_src;
  d[43] = b->samp_pool;
  sb_put16(d + 44, b->samp_val);
  sb_put16(d + 46, b->ubo_slot);
  sb_word_write(d + 48, &b->tex_u);
  sb_word_write(d + 57, &b->samp_u);
  d[66] = b->is_support;
  d[67] = (uint8_t)b->storage_kind;
  d[68] = (uint8_t)b->storage_slot;
  d[69] = b->storage_writable;
  sb_w32(d + 72, b->storage_cb_slot);
  sb_w32(d + 76, b->storage_cb_offset);
  if (s) {
    sb_w32(d + 80, s->cb_slot);
    sb_w32(d + 84, s->cb_offset);
    sb_w32(d + 88, s->writable);
    sb_w32(d + 92, s->mapping);
    sb_w32(d + 96, s->index);
    sb_w32(d + 100, s->offset);
  }
}
static inline void sb_binding_read(const uint8_t* d, struct fd_binding* b,
                                   struct nvn_vk_compute_storage* s) {
  b->set = (int32_t)sb_u32(d);
  b->binding = (int32_t)sb_u32(d + 4);
  b->kind = (int32_t)sb_u32(d + 8);
  b->slot = (int32_t)sb_u32(d + 12);
  b->count = sb_get16(d + 16);
  memcpy(b->stage, d + 18, 16);
  b->stage_idx = (int8_t)d[34];
  b->texel_buffer = d[35];
  b->tex_slot = (int16_t)sb_get16(d + 36);
  b->samp_slot = (int16_t)sb_get16(d + 38);
  b->pool = d[40];
  b->tex_ubo = d[41];
  b->samp_src = d[42];
  b->samp_pool = d[43];
  b->samp_val = sb_get16(d + 44);
  b->ubo_slot = (int16_t)sb_get16(d + 46);
  sb_word_read(d + 48, &b->tex_u);
  sb_word_read(d + 57, &b->samp_u);
  b->is_support = d[66];
  b->storage_kind = (int8_t)d[67];
  b->storage_slot = (int8_t)d[68];
  b->storage_writable = d[69];
  b->storage_cb_slot = (int32_t)sb_u32(d + 72);
  b->storage_cb_offset = (int32_t)sb_u32(d + 76);
  if (s) {
    s->cb_slot = sb_u32(d + 80);
    s->cb_offset = sb_u32(d + 84);
    s->writable = (int)sb_u32(d + 88);
    s->mapping = (enum nvn_vk_storage_mapping)sb_u32(d + 92);
    s->index = sb_u32(d + 96);
    s->offset = sb_u32(d + 100);
  }
}
