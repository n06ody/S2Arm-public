// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Host Vulkan packer only. Use the same converter and parser as legacy Android
// so texture provenance, implicit support descriptors and rejection paths agree.
#include "libpack_stage_wire.h"
#include "sha256.h"
extern int nvn_vk_pack_parse_stage(const uint8_t*, size_t, struct fd_program*, unsigned, char*,
                                   size_t);

struct sb_export {
  struct fd_program gfx;
  struct nvn_vk_compute_program cs;
  char why[384];
  unsigned regular, count;
};
static void sb_export_parse(struct sb_export* out, unsigned stage, int version, unsigned support,
                            const struct jw* st, const struct jw* bd, const struct jw* extra, int unaligned_capable) {
  struct jw doc = {0};
  jw_fmt(&doc,
         "{\"schema\":\"s2a-direct-program-v%d\",\"content\":{\"contract\":\"s2a-direct-program-"
         "content-v%d\",",
         version, version);
  if (stage == 5)
    jw_fmt(&doc,
           "\"kind\":\"compute\",\"hasUnalignedStorageBuffer\":%s,\"c0Reads\":null,"
           "\"supportReads\":null,",
           unaligned_capable ? "true" : "false");
  jw_str(&doc, "\"stages\":[");
  jw_put(&doc, (const char*)st->b, st->n);
  jw_str(&doc, "],\"bindings\":[");
  if (bd->n)
    jw_put(&doc, (const char*)bd->b, bd->n);
  if (extra->n)
    jw_put(&doc, (const char*)extra->b, extra->n);
  jw_fmt(&doc, "],\"supportLayout\":{\"requiredSize\":%u}}}",
         support > SUPPORT_DEFAULT_BYTES ? support : SUPPORT_DEFAULT_BYTES);
  int rc;
  if (doc.oom)
    rc = lp_fail(out->why, sizeof out->why, "stage assembly out of memory");
  else if (stage == 5)
    rc = nvn_vk_program_parse_compute(doc.b, doc.n, &out->cs, out->why, sizeof out->why);
  else
    rc = nvn_vk_pack_parse_stage(doc.b, doc.n, &out->gfx, stage, out->why, sizeof out->why);
  if (rc && !out->why[0])
    snprintf(out->why, sizeof out->why, "stage parser rejected");
  if (!rc) {
    out->count = (unsigned)(stage == 5 ? out->cs.binding_count : out->gfx.binding_count);
    out->regular = out->count - (extra->n ? 1 : 0);
  }
  free(doc.b);
}
static size_t sb_choice_size(const struct sb_export* c) {
  return c->why[0] ? SB_CHOICE + strlen(c->why) + 1 : SB_CHOICE + (size_t)c->count * SB_BINDING;
}
static void sb_choice_write(uint8_t* p, const struct sb_export* c, unsigned nvn) {
  if (c->why[0]) {
    sb_w32(p, 1);
    memcpy(p + SB_CHOICE, c->why, strlen(c->why) + 1);
    return;
  }
  const struct fd_stage* st = nvn == 5 ? &c->cs.stage : &c->gfx.stage[nvn];
  sb_w32(p + 4, c->regular);
  sb_w32(p + 8, c->count);
  sb_w32(p + 12, nvn == 5 ? c->cs.support_bytes : c->gfx.support_bytes);
  sb_w32(p + 16, st->attr_mask);
  sb_w32(p + 20, st->fragment_output_map);
  sb_w32(p + 24, st->binst_unread);
  sb_w32(p + 28, st->binst_unread_static);
  sb_w32(p + 32, st->binst_static_present);
  if (nvn == 5) {
    sb_w32(p + 36, c->cs.support_reads_unknown);
    sb_w32(p + 40, c->cs.c0_reads_unknown);
    sb_w32(p + 44, c->cs.has_unaligned_storage_buffer);
    sb_w32(p + 48, c->cs.spill_bytes);
    for (unsigned i = 0; i < 2; i++) {
      const struct nvn_vk_support_scale* sc = &c->cs.support_scales[i];
      uint8_t* d = p + 52 + 16 * i;
      sb_w32(d, sc->offset);
      sb_w32(d + 4, sc->stride);
      sb_w32(d + 8, sc->count);
      sb_w32(d + 12, sc->element_bytes);
    }
  }
  for (unsigned i = 0; i < c->count; i++)
    sb_binding_write(p + SB_CHOICE + i * SB_BINDING,
                     nvn == 5 ? &c->cs.bindings[i] : &c->gfx.bindings[i],
                     nvn == 5 ? &c->cs.storage[i] : NULL);
}
int s2a_pack_stage(const uint8_t key[8], unsigned nvn, int topo, uint64_t fingerprint,
                   const uint8_t* json, size_t jn, const uint8_t* spv, size_t sn, uint8_t** result,
                   size_t* length) {
  if (!result || !length || !key || !json || !spv || !jn || !sn ||
      sn > NVK_PROGRAM_RESULT_MAX_BYTES || jn > NVK_PROGRAM_RESULT_MAX_BYTES ||
      !lp_nvn_stage_name(nvn))
    return -1;
  *result = NULL;
  *length = 0;
  struct sb_export* choice = calloc(2, sizeof *choice);
  uint8_t* copy = malloc(sn);
  if (!copy || !choice) {
    free(copy);
    free(choice);
    return -1;
  }
  memcpy(copy, spv, sn);
  struct lp_stage_contract contract = {0};
  struct s2a_libpack scratch = {0};
  struct jw stages = {0}, binds = {0}, extra = {0};
  uint32_t mask = 0, support = 0;
  int sep = 0, v2 = 0, rc = -1;
  int status = lp_stage_from_bytes(&scratch, key, nvn, nvn == 5, topo, UINT32_MAX, NULL, 1, json, jn, copy,
                                   sn, &stages, &binds, &sep, &mask, &support, &v2, &contract,
                                   choice[0].why, sizeof choice[0].why);
  if (status && !choice[0].why[0])
    snprintf(choice[0].why, sizeof choice[0].why, "stage unavailable");
  char strict_why[sizeof choice[0].why] = "";
  if (!status &&
      lp_finish_v2_bindings(&contract, &extra, &sep, nvn == 5, strict_why, sizeof strict_why) &&
      !strict_why[0])
    snprintf(strict_why, sizeof strict_why, "v2 binding contract rejected");
  if (stages.oom || binds.oom || extra.oom)
    goto done;
  for (unsigned i = 0; i < 2; i++) {
    struct sb_export* c = &choice[i];
    if (status) {
      if (i)
        memcpy(c->why, choice[0].why, sizeof c->why);
      continue;
    }
    int strict = v2 || i;
    if (strict && strict_why[0]) {
      snprintf(c->why, sizeof c->why, "%s", strict_why);
      continue;
    }
    const struct jw empty = {0};
    sb_export_parse(c, nvn, strict ? 2 : 1, support, &stages, &binds, strict ? &extra : &empty,
                    LP_WINDOW_KIND(contract.window.mode) == LP_WINDOW_MASK);
  }
  size_t sizes[] = {contract.window.mode ? SB_META_WINDOW_SIZE : SB_META_SIZE, sb_choice_size(choice), sb_choice_size(choice + 1), sn};
  size_t off = SB_HEADER + SB_SECTIONS * SB_SECTION, offsets[SB_SECTIONS];
  for (unsigned i = 0; i < SB_SECTIONS; i++) {
    off = (off + 3) & ~(size_t)3;
    offsets[i] = off;
    off += sizes[i];
  }
  uint8_t* record = calloc(1, off);
  if (!record)
    goto done;
  memcpy(record, "S2SB", 4);
  sb_w32(record + 4, contract.window.mode ? 2 : 1);
  sb_w32(record + 8, SB_HEADER);
  sb_w32(record + 16, SB_SECTIONS);
  sb_w32(record + 20, SB_HEADER);
  sb_w64(record + 24, off);
  for (unsigned i = 0; i < SB_SECTIONS; i++) {
    uint8_t* d = record + SB_HEADER + i * SB_SECTION;
    sb_w32(d, i + 1);
    sb_w32(d + 4, 1);
    sb_w32(d + 8, 1);
    sb_w32(d + 12, (uint32_t)sizes[i]);
    sb_w64(d + 16, offsets[i]);
    sb_w64(d + 24, sizes[i]);
  }
  uint8_t* m = record + offsets[0];
  memcpy(m, key, 8);
  sb_w64(m + 8, fingerprint);
  sb_w32(m + 16, nvn);
  sb_w32(m + 20, (uint32_t)topo);
  sb_w32(m + 24, (uint32_t)v2);
  sb_w32(m + 28, S2A_DIRECT_ABI);
  _Static_assert(sizeof(S2A_GRAPHICS_RUNTIME_COMPILER_ABI) <= SB_META_SIZE - 40,
                 "compiler ABI exceeds metadata");
  memcpy(m + 40, S2A_GRAPHICS_RUNTIME_COMPILER_ABI, sizeof(S2A_GRAPHICS_RUNTIME_COMPILER_ABI));
  struct s2a_jv jf = {(const char*)json, jn}, field;
  uint64_t shared = 0;
  if (nvn == 5 && !status && !s2a_jv_obj_find(jf, "spill", &field)) {
    if (s2a_spv_workgroup_bytes(spv, sn, &shared)) {
      free(record);
      goto done;
    }
  }
  sb_w64(m + 32, shared);
  if (contract.window.mode) {
    sb_w32(m + SB_META_SIZE, contract.window.mode);
    sb_w32(m + SB_META_SIZE + 4, contract.window.id);
  }
  sb_choice_write(record + offsets[1], choice, nvn);
  sb_choice_write(record + offsets[2], choice + 1, nvn);
  memcpy(record + offsets[3], spv, sn);
  s2a_sha256 hash;
  s2a_sha256_init(&hash);
  s2a_sha256_update(&hash, record, off);
  s2a_sha256_final(&hash, record + 32);
  *result = record;
  *length = off;
  rc = 0;
done:
  for (unsigned i = 0; i < 2; i++) {
    nvn_vk_program_free(&choice[i].gfx);
    nvn_vk_program_free_compute(&choice[i].cs);
  }
  free(choice);
  free(copy);
  free(stages.b);
  free(binds.b);
  free(extra.b);
  return rc;
}
void s2a_pack_stage_free(void* p) {
  free(p);
}
