// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "libpack_stage_wire.h"
#include "sha256.h"

struct sb_view {
  uint8_t *allocation, *section[SB_SECTIONS];
  size_t size[SB_SECTIONS];
  const uint8_t* choice;
  unsigned stage, regular, count;
  int strict;
};
int s2a_libpack_is_binary(const struct s2a_libpack* lp) {
  return lp && lp->p.idx && sb_u32(lp->p.idx + 44) != 0;
}
int s2a_libpack_same_producer(const struct s2a_libpack *a, const struct s2a_libpack *b) {
  return a && b && a->p.idx && b->p.idx && !memcmp(a->p.idx + 24, b->p.idx + 24, 20);
}
static int sb_view_open(struct s2a_libpack* lp, const uint8_t key[8], unsigned stage, int topology,
                        struct sb_view* v, char* why, size_t cap) {
  unsigned kind = stage == 2 ? 129u + (unsigned)topology : 128u;
  if (memcmp(lp->p.idx + 44, "S2SB", 4))
    return lp_fail(why, cap, "unsupported Vulkan stage library version");
  size_t n = 0;
  int rc = s2a_libpack_get(lp, key, kind, &v->allocation, &n, why, cap);
  if (rc) {
    if (rc == S2A_LIBPACK_MISS && why && cap)
      snprintf(why, cap, "binary stage kind %u missing", kind);
    return rc;
  }
  uint8_t* p = v->allocation;
  if (n < SB_HEADER + SB_SECTION * SB_SECTIONS || memcmp(p, "S2SB", 4) || (sb_u32(p + 4) != 1 && sb_u32(p + 4) != 2))
    return lp_fail(why, cap, "binary stage magic or version mismatch");
  if (sb_u32(p + 8) != SB_HEADER || sb_u32(p + 12) || sb_u32(p + 16) != SB_SECTIONS ||
      sb_u32(p + 20) != SB_HEADER || sb_u64(p + 24) != n)
    return lp_fail(why, cap, "invalid binary stage header");
  s2a_sha256 h;
  uint8_t digest[32];
  static const uint8_t zero[32] = {0};
  s2a_sha256_init(&h);
  s2a_sha256_update(&h, p, 32);
  s2a_sha256_update(&h, zero, 32);
  s2a_sha256_update(&h, p + 64, n - 64);
  s2a_sha256_final(&h, digest);
  if (memcmp(digest, p + 32, 32))
    return lp_fail(why, cap, "binary stage checksum mismatch");
  size_t end = SB_HEADER + SB_SECTION * SB_SECTIONS;
  for (unsigned i = 0; i < SB_SECTIONS; i++) {
    const uint8_t* d = p + SB_HEADER + SB_SECTION * i;
    uint64_t off = sb_u64(d + 16), len = sb_u64(d + 24);
    if (sb_u32(d) != i + 1 || sb_u32(d + 4) != 1 || sb_u32(d + 8) != 1 || sb_u32(d + 12) != len ||
        off != ((end + 3u) & ~(uint64_t)3) || off > n || len > n - off)
      return lp_fail(why, cap, "invalid binary stage section %u", i + 1);
    v->section[i] = p + off;
    v->size[i] = (size_t)len;
    end = (size_t)(off + len);
  }
  const uint8_t* m = v->section[0];
  v->stage = stage;
  if (end != n || v->size[0] != (sb_u32(p + 4) == 2 ? SB_META_WINDOW_SIZE : SB_META_SIZE) || v->size[1] < SB_CHOICE || v->size[2] < SB_CHOICE ||
      v->size[3] < 20 || (v->size[3] & 3) || v->size[3] > NVK_PROGRAM_RESULT_MAX_BYTES)
    return lp_fail(why, cap, "invalid binary stage section sizes");
  if (memcmp(m, key, 8) || memcmp(m + 8, lp->p.idx + 36, 8) || sb_u32(m + 16) != stage ||
      (int32_t)sb_u32(m + 20) != (stage == 2 ? topology : -1) || sb_u32(m + 24) > 1)
    return lp_fail(why, cap, "binary stage identity mismatch");
  const uint8_t compiler_abi[SB_META_SIZE - 40] = S2A_GRAPHICS_RUNTIME_COMPILER_ABI;
  if (sb_u32(m + 28) != S2A_DIRECT_ABI || memcmp(m + 40, compiler_abi, sizeof compiler_abi))
    return lp_fail(why, cap, "binary stage ABI mismatch");
  return S2A_LIBPACK_HIT;
}
// Compare the stored ownership facts with the SPIR-V used to create the module.
// Once any stage requests v2 this check covers every participating stage.
static int sb_check_v2(const struct sb_view* v, char* why, size_t cap) {
  struct lp_stage_contract contract = {0};
  if (lp_spv_read_bindings(v->section[3], v->size[3], &contract.declared))
    return lp_fail(why, cap, "invalid binary stage SPIR-V descriptors");
  for (unsigned i = 0; i < v->regular; i++) {
    const uint8_t* row = v->choice + SB_CHOICE + i * SB_BINDING;
    lp_record_binding(&contract.emitted, sb_u32(row + 4),
                      row[66] ? LP_BINDING_SUPPORT : LP_BINDING_GUEST);
  }
  for (unsigned i = 0; i < 8; i++)
    snprintf(contract.key + 2 * i, 3, "%02x", v->section[0][i]);
  snprintf(contract.spv_name, sizeof contract.spv_name, "S2SB SPIR-V");
  snprintf(contract.json_name, sizeof contract.json_name, "S2SB");
  int implicit_support = -1;
  int rc = lp_check_v2_bindings(&contract, &implicit_support, why, cap);
  if (rc)
    return rc;
  if ((implicit_support >= 0) != (v->count > v->regular))
    return lp_fail(why, cap, "binary deferred support count mismatch");
  if (implicit_support >= 0 &&
      sb_u32(v->choice + SB_CHOICE + v->regular * SB_BINDING + 4) != (uint32_t)implicit_support)
    return lp_fail(why, cap, "binary deferred support owner mismatch");
  return 0;
}

static int sb_choice(struct sb_view* v, int strict, char* why, size_t cap) {
  unsigned index = strict ? 2 : 1;
  const uint8_t* p = v->section[index];
  size_t n = v->size[index];
  if (sb_u32(p) == 1) {
    if (n <= SB_CHOICE + 1 || p[n - 1] || memchr(p + SB_CHOICE, 0, n - SB_CHOICE - 1))
      return lp_fail(why, cap, "invalid binary stage rejection record");
    return lp_fail(why, cap, "%s", (const char*)p + SB_CHOICE);
  }
  v->choice = p;
  v->strict = strict;
  v->regular = sb_u32(p + 4);
  v->count = sb_u32(p + 8);
  if (sb_u32(p) || v->count > FD_MAX_BINDINGS || v->regular > v->count ||
      v->count - v->regular > 1 || n != SB_CHOICE + (size_t)v->count * SB_BINDING ||
      sb_u32(p + 12) < SUPPORT_DEFAULT_BYTES)
    return lp_fail(why, cap, "invalid binary stage binding table");
  return strict ? sb_check_v2(v, why, cap) : 0;
}
static int sb_rows(struct sb_view* v, unsigned begin, unsigned end, struct fd_binding* bindings,
                   struct nvn_vk_compute_storage* storage, int* count, char* why, size_t cap) {
  for (unsigned i = begin; i < end; i++) {
    if (*count >= FD_MAX_BINDINGS)
      return lp_fail(why, cap, "binary program binding limit exceeded");
    struct fd_binding* b = &bindings[*count];
    sb_binding_read(v->choice + SB_CHOICE + i * SB_BINDING, b, storage ? storage + *count : NULL);
    if (!memchr(b->stage, 0, sizeof b->stage) || b->set != 0 || b->binding < 0 ||
        b->binding > UINT16_MAX || b->kind < 0 || b->kind > 3 || b->count != 1 ||
        b->is_support > 1 || b->samp_slot < -1 || b->samp_slot > 127 ||
        (b->samp_slot >= 0 && (b->kind != 2 || b->tex_slot < 128 || b->tex_slot > 255)))
      return lp_fail(why, cap, "invalid binary descriptor binding");
    const char* expected_stage =
        b->is_support ? "program" : lp_v1_stage_name(lp_nvn_stage_name(v->stage));
    if (strcmp(b->stage, expected_stage) || b->stage_idx != (b->is_support ? -1 : (int)v->stage) ||
        (b->is_support && b->kind != 0))
      return lp_fail(why, cap, "invalid binary descriptor stage owner");
    if (b->samp_slot >= 0 &&
        (b->slot < 90 || b->slot > 344 || (b->slot & 1) || b->tex_slot != 128 + (b->slot - 90) / 2))
      return lp_fail(why, cap, "invalid separate sampler source");
    if (b->samp_slot >= 0 &&
        (!v->strict || !lp_spv_single_combined(v->section[3], v->size[3], (uint32_t)b->binding)))
      return lp_fail(why, cap, "invalid binary separate sampler declaration");
    if (b->samp_pool) {
      const int c2 = b->slot >= 90 && b->slot <= 344 && !(b->slot & 1) && !b->tex_ubo &&
                     b->tex_slot == 128 + (b->slot - 90) / 2;
      const int flat = b->tex_ubo == 1 && b->slot == -1 && b->tex_slot == -1 &&
                       b->tex_u.slot < 16 && !(b->tex_u.offset & 3) && !b->tex_u.sid &&
                       !b->tex_u.pair;
      if (!v->strict || b->kind != 2 || b->pool || b->samp_slot != -1 ||
          b->samp_src != FD_SAMP_ID || b->samp_val > 4095 || b->samp_pool != 1 || !(c2 || flat))
        return lp_fail(why, cap, "invalid binary constant sampler source");
    }
    if (i >= v->regular && !b->is_support)
      return lp_fail(why, cap, "invalid deferred support binding");
    for (int j = 0; j < *count; j++)
      if (bindings[j].set == b->set && bindings[j].binding == b->binding)
        return lp_fail(why, cap, "duplicate descriptor binding");
    ++*count;
  }
  return 0;
}
static int sb_stage_take(struct sb_view* v, struct fd_stage* st, int geometry, const struct s2a_libpack_gfx *gfx, char* why,
                         size_t cap) {
  uint8_t* spv = v->section[3];
  size_t n = v->size[3];
  struct lp_window window = {0};
  if (v->size[0] == SB_META_WINDOW_SIZE) {
    const uint8_t *ext = v->section[0] + SB_META_SIZE;
    window.mode = sb_u32(ext);
    window.id = sb_u32(ext + 4);
    if (!window.mode) return lp_fail(why, cap, "invalid binary window metadata");
  }
  {
    int rc = lp_window_apply(window, gfx, spv, n, why, cap);
    if (rc) return rc;
  }
  if (s2a_libpack_spv_admit_caps(spv, &n, why, cap))
    return -1;
  if (geometry && lp_spv_viewport_builtin(spv, n))
    return lp_fail(why, cap,
                   "geometry program has unreadable instructions or writes viewport index/mask");
  st->present = 1;
  st->attr_mask = sb_u32(v->choice + 16);
  st->fragment_output_map = sb_u32(v->choice + 20);
  st->binst_unread = (int)sb_u32(v->choice + 24);
  st->binst_unread_static = (int)sb_u32(v->choice + 28);
  st->binst_static_present = (int)sb_u32(v->choice + 32);
  st->spirv = spv;
  st->spirv_bytes = n;
  st->spirv_owner = v->allocation;
  v->allocation = NULL;
  return 0;
}
int s2a_libpack_binary_program(struct s2a_libpack* lp, const uint8_t keys[6][8], uint32_t stages,
                               const struct s2a_libpack_gfx* gfx, uint32_t shared_limit,
                               struct fd_program* gp, struct nvn_vk_compute_program* cp, char* why,
                               size_t cap) {
  unsigned n = stages == 3 ? 2 : stages == 7 ? 3 : (stages == 1 || stages == 32) ? 1 : 0;
  const int cs = stages == 32;
  if (!lp || !keys || (cs ? !cp : !gp))
    return S2A_LIBPACK_INVALID;
  if (cs)
    memset(cp, 0, sizeof *cp);
  else
    memset(gp, 0, sizeof *gp);
  int topo = -1, rc = S2A_LIBPACK_HIT;
  if (!n)
    rc = S2A_LIBPACK_MISS;
  if (stages == 7 &&
      (!gfx || !s2a_libpack_profile_geometry_shader(gfx->profile_json, gfx->profile_bytes) ||
       (topo = lp_input_topology(gfx->topology)) < 0))
    rc = S2A_LIBPACK_MISS;
  struct sb_view v[3] = {0};
  if (rc)
    goto done;
  int strict = 0;
  for (unsigned i = 0; i < n; i++) {
    unsigned st = cs ? 5 : i;
    char note[256] = "";
    int r = sb_view_open(lp, keys[i], st, topo, &v[i], note, sizeof note);
    if (!r && sb_u32(v[i].section[0] + 24))
      strict = 1;
    if (r && (r < 0 || rc == 0)) {
      rc = r;
      if (why && cap)
        snprintf(why, cap, "%s", note);
    }
  }
  if (rc)
    goto done;
  for (unsigned i = 0; i < n; i++) {
    rc = sb_choice(&v[i], strict, why, cap);
    if (rc)
      goto done;
    const uint8_t* p = v[i].choice;
    if (cs) {
      if (sb_u64(v[i].section[0] + 32) > shared_limit) {
        rc = lp_fail(why, cap, "binary compute shared memory exceeds device limit");
        goto done;
      }
      if (sb_u32(p + 36) != 1 || sb_u32(p + 40) != 1 || sb_u32(p + 44) != 0 ||
          sb_u32(p + 48) != 0) {
        rc = lp_fail(why, cap, "invalid binary compute alignment or read facts");
        goto done;
      }
      cp->support_reads_unknown = 1;
      cp->c0_reads_unknown = 1;
      cp->support_bytes = sb_u32(p + 12);
      for (unsigned j = 0; j < 2; j++) {
        const uint8_t* d = p + 52 + 16 * j;
        cp->support_scales[j] =
            (struct nvn_vk_support_scale){sb_u32(d), sb_u32(d + 4), sb_u32(d + 8), sb_u32(d + 12)};
      }
    } else if (gp->support_bytes < sb_u32(p + 12))
      gp->support_bytes = sb_u32(p + 12);
  }
  for (unsigned pass = 0; pass < 2; pass++)
    for (unsigned i = 0; i < n; i++) {
      rc = sb_rows(&v[i], pass ? v[i].regular : 0, pass ? v[i].count : v[i].regular,
                   cs ? cp->bindings : gp->bindings, cs ? cp->storage : NULL,
                   cs ? &cp->binding_count : &gp->binding_count, why, cap);
      if (rc)
        goto done;
    }
  for (unsigned i = 0; i < n; i++) {
    rc = sb_stage_take(&v[i], cs ? &cp->stage : &gp->stage[i], stages == 7, gfx, why, cap);
    if (rc)
      goto done;
  }
done:
  for (unsigned i = 0; i < n; i++)
    free(v[i].allocation);
  if (rc) {
    if (cs)
      nvn_vk_program_free_compute(cp);
    else
      nvn_vk_program_free(gp);
  }
  if (rc == 0)
    atomic_fetch_add(&g_hits, 1);
  else if (rc > 0)
    atomic_fetch_add(&g_misses, 1);
  else
    atomic_fetch_add(&g_rejects, 1);
  return rc;
}
