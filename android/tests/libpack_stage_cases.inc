// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "direct/libpack_stage_wire.h"
#include "direct/sha256.h"

// Strip pointers before comparing the two independently allocated programs.
// Include the complete reachable SPIR-V buffers, all descriptor fields and compute facts.
static int stages_equal(struct fd_stage* legacy, struct fd_stage* binary) {
  int equal = legacy->spirv_bytes == binary->spirv_bytes &&
              (!legacy->spirv_bytes || !memcmp(legacy->spirv, binary->spirv, legacy->spirv_bytes));
  free(legacy->spirv);
  free(binary->spirv_owner);
  legacy->spirv = binary->spirv = legacy->spirv_owner = binary->spirv_owner = NULL;
  return equal;
}

static int binary_lookup(const char* dir, const uint8_t keys[6][8], uint32_t stages,
                         struct fd_program* graphics, struct nvn_vk_compute_program* compute,
                         char* reason, size_t capacity) {
  struct s2a_libpack* lib = NULL;
  if (s2a_libpack_open(&lib, dir) != 1 || !s2a_libpack_is_binary(lib)) {
    CHECK(0, "expected S2SB container");
    s2a_libpack_close(lib);
    return S2A_LIBPACK_INVALID;
  }
  int rc = s2a_libpack_binary_program(lib, keys, stages, NULL, 32768, graphics, compute, reason,
                                      capacity);
  s2a_libpack_close(lib);
  return rc;
}

static void seal_stage(uint8_t* record, size_t size) {
  memset(record + 32, 0, 32);
  s2a_sha256 hash;
  s2a_sha256_init(&hash);
  s2a_sha256_update(&hash, record, size);
  s2a_sha256_final(&hash, record + 32);
}

// Feed the #2033 live compiler contract into the wire codec. The R1 offline converter
// uses a different, nested sampler-id contract and preserves its previous rejection.
static void check_flat_compute(const char* dir, const uint8_t keys[6][8], const uint8_t* json,
                               uint8_t* record, size_t size, struct trec rec) {
  const char* field = "\"slot\":90,\"samp_slot\":0";
  const char* src = (const char*)json;
  const char* slot = strstr(src, field);
  CHECK(slot != NULL, "locate separate sampler pair in real assembled fixture");
  if (!slot)
    return;
  char* flat = malloc(strlen(src) + 80);
  if (!flat) {
    CHECK(0, "allocate flat compute fixture");
    return;
  }
  snprintf(flat, strlen(src) + 80, "%.*s\"uboSlot\":2,\"uboOffset\":160,\"constSampler\":260%s",
           (int)(slot - src), src, slot + strlen(field));
  static struct nvn_vk_compute_program expected, actual;
  char why[256] = "";
  int rc =
      nvn_vk_program_parse_compute((const uint8_t*)flat, strlen(flat), &expected, why, sizeof why);
  free(flat);
  CHECK(!rc, "production parser admits flat compute UBO and immediate sampler");
  if (rc)
    return;
  unsigned i = 0;
  while (i < (unsigned)expected.binding_count && expected.bindings[i].binding != 11)
    ++i;
  CHECK(i < (unsigned)expected.binding_count, "type-3 compute fixture has texture binding 11");
  if (i == (unsigned)expected.binding_count) {
    nvn_vk_program_free_compute(&expected);
    return;
  }
  struct fd_binding* b = &expected.bindings[i];
  CHECK(b->tex_ubo && b->tex_u.slot == 2 && b->tex_u.offset == 160 && b->samp_pool == 1 &&
            b->samp_src == FD_SAMP_ID && b->samp_val == 260,
        "#2033 source identity from live contract");
  uint8_t* strict = record + sb_u64(record + SB_HEADER + 2 * SB_SECTION + 16);
  uint8_t* row = strict + SB_CHOICE + i * SB_BINDING;
  sb_binding_write(row, b, &expected.storage[i]);
  const uint8_t* payload = record;
  for (unsigned attempt = 0; attempt < 2; attempt++) {
    if (attempt)
      sb_w32(row + 12, 90);  // retain UBO and also claim a c2 slot
    seal_stage(record, size);
    CHECK(!mk_pack_at(dir, 1, &rec, 1, &payload, &size, 1),
          "write checksummed compute type-3 facts");
    rc = binary_lookup(dir, keys, 32, NULL, &actual, why, sizeof why);
    if (!attempt)
      CHECK(!rc && !memcmp(&actual.bindings[i], b, sizeof *b),
            "binary preserves flat UBO and sampler pool identity");
    else
      CHECK(rc == S2A_LIBPACK_REJECT && strstr(why, "constant sampler source"),
            "binary refuses simultaneous c2 and UBO texture sources");
    nvn_vk_program_free_compute(&actual);
  }
  nvn_vk_program_free_compute(&expected);
}

static void check_binary_contract(const uint8_t keys[6][8], uint32_t stages, const uint8_t** input,
                                  size_t* sizes, int legacy_rc, const uint8_t* json,
                                  size_t json_len, const char* legacy_reason,
                                  int check_corruption) {
  const unsigned count = stages == 32 ? 1 : 2;
  uint8_t* encoded[2] = {0};
  size_t lengths[2] = {0};
  struct trec records[2] = {0};
  char dir[sizeof g_dir + 8];
  snprintf(dir, sizeof dir, "%s/binary", g_dir);
  CHECK(!mkdir(dir, 0700), "create binary fixture directory");
  const uint8_t* payloads[2];
  for (unsigned i = 0; i < count; i++) {
    unsigned stage = stages == 32 ? 5 : i;
    int rc = s2a_pack_stage(keys[i], stage, -1, 0, input[2 * i], sizes[2 * i], input[2 * i + 1],
                            sizes[2 * i + 1], &encoded[i], &lengths[i]);
    CHECK(!rc && encoded[i], "encode offline stage contract");
    if (rc || !encoded[i])
      goto cleanup;
    for (unsigned j = 0; j < 8; j++)
      records[i].key = (records[i].key << 8) | keys[i][j];
    records[i].kind = 128;
    records[i].len = (uint32_t)lengths[i];
    payloads[i] = encoded[i];
  }
  char expected_reason[256], reason[256] = "";
  snprintf(expected_reason, sizeof expected_reason, "%s", legacy_reason);
  static struct fd_program old_gfx, new_gfx;
  static struct nvn_vk_compute_program old_cs, new_cs;
  memset(&old_gfx, 0, sizeof old_gfx);
  memset(&old_cs, 0, sizeof old_cs);
  if (!legacy_rc) {
    legacy_rc = stages == 32
                    ? nvn_vk_program_parse_compute(json, json_len, &old_cs, expected_reason,
                                                   sizeof expected_reason)
                    : nvn_vk_program_parse_graphics(json, json_len, &old_gfx, expected_reason,
                                                    sizeof expected_reason);
  }
  CHECK(!mk_pack_at(dir, 1, records, count, payloads, lengths, 1), "write binary fixture");
  int rc = binary_lookup(dir, keys, stages, &new_gfx, &new_cs, reason, sizeof reason);
  if (rc != legacy_rc || (rc && strcmp(reason, expected_reason)))
    fprintf(stderr, "S2SB expected rc=%d %s, observed rc=%d %s\n", legacy_rc, expected_reason, rc,
            reason);
  CHECK(rc == legacy_rc && (!rc || !strcmp(reason, expected_reason)),
        "binary preserves acceptance and exact rejection reason");
  if (!legacy_rc && !rc) {
    if (stages == 32) {
      CHECK(stages_equal(&old_cs.stage, &new_cs.stage) && !memcmp(&old_cs, &new_cs, sizeof old_cs),
            "binary preserves complete compute contract");
    } else {
      for (unsigned i = 0; i < count; i++)
        CHECK(stages_equal(&old_gfx.stage[i], &new_gfx.stage[i]),
              "binary preserves graphics SPIR-V");
      CHECK(!memcmp(&old_gfx, &new_gfx, sizeof old_gfx),
            "binary preserves complete graphics contract");
    }
  }
  if (stages == 32) {
    nvn_vk_program_free_compute(&old_cs);
    nvn_vk_program_free_compute(&new_cs);
  } else {
    nvn_vk_program_free(&old_gfx);
    nvn_vk_program_free(&new_gfx);
  }
  if (stages == 32 && rc == 0 && !legacy_rc)
    check_flat_compute(dir, keys, json, encoded[0], lengths[0], records[0]);
  if (check_corruption) {
    for (unsigned i = 0; i < 6; i++) {
      uint8_t* record = encoded[0];
      size_t pos = i == 0   ? 4
                   : i == 1 ? lengths[0] - 1
                            : (size_t)sb_u64(record + SB_HEADER + 16) + (i == 2   ? 0
                                                                         : i == 3 ? 8
                                                                         : i == 4 ? 28
                                                                                  : 40);
      record[pos] ^= 1;
      uint8_t original_hash[32];
      memcpy(original_hash, record + 32, 32);
      if (i >= 2) {
        // A valid checksum cannot replace validation of the indexed key and translator identity.
        memset(record + 32, 0, 32);
        s2a_sha256 hash;
        s2a_sha256_init(&hash);
        s2a_sha256_update(&hash, record, lengths[0]);
        s2a_sha256_final(&hash, record + 32);
      }
      CHECK(!mk_pack_at(dir, 1, records, count, payloads, lengths, 1), "write damaged test record");
      rc = binary_lookup(dir, keys, stages, &new_gfx, &new_cs, reason, sizeof reason);
      const char* expected = i == 0   ? "version mismatch"
                             : i == 1 ? "checksum mismatch"
                             : i < 4  ? "identity mismatch"
                                      : "ABI mismatch";
      CHECK(rc == S2A_LIBPACK_REJECT && strstr(reason, expected) && !new_gfx.stage[0].spirv,
            "damaged binary is rejected by its own contract");
      nvn_vk_program_free(&new_gfx);
      record[pos] ^= 1;
      memcpy(record + 32, original_hash, 32);
    }
  }
cleanup:
  for (unsigned i = 0; i < count; i++)
    s2a_pack_stage_free(encoded[i]);
  char path[sizeof dir + 16];
  snprintf(path, sizeof path, "%s/lib.idx", dir);
  unlink(path);
  snprintf(path, sizeof path, "%s/lib.000.pak", dir);
  unlink(path);
  rmdir(dir);
}
