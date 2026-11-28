// SPDX-License-Identifier: Apache-2.0
// 实际 SPIR-V、直连派发、GPU fence 后的客体回写。测试钩子只计量真实 memcpy。
#define main barrier_fixture_main
#define nvn_plat_flag barrier_fixture_flag
#include "t_replay_dispatch_barrier.c"
#undef nvn_plat_flag
#undef main
#include <errno.h>

static int enable_bound, enable_accounting, enable_versioned;
static size_t copied_bytes;
int nvn_plat_flag(const char* name, int fallback) {
  if (!strcmp(name, "nvk_land_accounting"))
    return enable_accounting;
  if (!strcmp(name, "nvk_land_lazy_uma"))
    return enable_versioned;
  if (enable_bound && !strcmp(name, "nvk_land_bound"))
    return 1;
  return barrier_fixture_flag(name, fallback);
}
int rp_post_gpu_write_prefix(uint64_t addr, uint64_t len, uint64_t copy_len) {
  REQUIRE(nw < 4);
  wid[nw] = rp_buf_gpu_write_note_prefix(addr, len, copy_len, &wkey[nw], NULL);
  REQUIRE(wid[nw]);
  ++nw;
  return 1;
}
int rp_post_gpu_write_tagged(uint64_t addr, uint64_t len, uint64_t copy_len, uint64_t row,
                             uint64_t image, uint32_t rows, uint32_t images,
                             enum rp_land_origin origin, unsigned stage) {
  REQUIRE(nw < 4 && rows == 1 && images == 1 && row == len && image == len);
  wid[nw] = rp_buf_gpu_write_note_tagged(addr, len, copy_len, &wkey[nw], NULL, origin, stage);
  REQUIRE(wid[nw]);
  ++nw;
  return 1;
}
int rp_pool_snapshot_key(uint64_t addr, size_t len, uint64_t* key, uint64_t* epoch) {
  if (!rp_pool_find(addr, len, NULL, NULL)) return EAGAIN;
  *key = (uintptr_t)guest;
  *epoch = 1;
  return 0;
}
uint64_t rp_pool_mirror_epoch(const uint8_t* mem, uint64_t size) {
  return 1;
}
static void land_done(void* user, const nvn_gpu_done* completion) {
  rp_buf_gpu_write_land(wkey[0], wid[0], completion->status != NVN_GPU_DONE_FAILED);
}
void rp_test_pool_gone(void) {
}
void rp_test_land_copy(void* dst, const void* src, size_t bytes) {
  copied_bytes += bytes;
  memcpy(dst, src, bytes);
}

static void run_arm(const char* shaders, const char* arm) {
  nw = 0;
  copied_bytes = 0;
  enable_bound = strcmp(arm, "default") != 0;
  enable_accounting = !strcmp(arm, "accounting");
  enable_versioned = !strcmp(arm, "versioned");
  const int dynamic = !strcmp(arm, "dynamic"), atomic_write = !strcmp(arm, "atomic");
  const int translated = !strcmp(arm, "translated");
  const int indirect = !strcmp(arm, "indirect");
  struct s2a_vk_device dev;
  char why[256];
  REQUIRE(!s2a_vk_device_create(&dev, why, sizeof why));
  nvn_gpu_open_desc od = {0};
  od.host_device = &dev;
  GPU(nvn_gpu_open(&od, &err));
  prog.is_compute = 1;
  prog.nent[0] = 1;
  nvn_gpu_layout_ent* entry = &prog.ent[0][0];
  entry->cls = NVN_GPU_RES_SSBO;
  entry->src_kind = NVN_RB_SBUFP;
  entry->src_reg2 = UINT16_MAX;
  entry->access = NVN_GPU_ACC_RW;
  entry->count = 1;
  entry->stages = 1u << NVN_GPU_SH_CS;
  nvn_gpu_layout layout = {0};
  layout.ent = entry;
  if (translated) {
    prog.ent[0][1] = (nvn_gpu_layout_ent){.cls = NVN_GPU_RES_UBO, .src_kind = NVN_RB_DRV0,
        .access = NVN_GPU_ACC_R, .count = 1, .binding = 1, .stages = 1u << NVN_GPU_SH_CS};
    prog.nent[0]++;
  }
  layout.nent = prog.nent[0];
  prog.sh[0] = shader(shaders, dynamic || atomic_write || translated ? arm : "linear", &layout);

  uint32_t expected[sizeof guest / sizeof(uint32_t)];
  for (unsigned i = 0; i < sizeof expected / sizeof *expected; ++i)
    expected[i] = 0xa5a5a5a5u;
  for (unsigned i = 0; i < 32; ++i)
    expected[900 + i] = 700 + i;
  const uint32_t groups[3] = {2, 1, 1};
  memcpy(expected + 960, groups, sizeof groups);
  memcpy(guest, expected, sizeof guest);
  for (unsigned i = 0; i < 32; ++i) {
    if (dynamic || atomic_write)
      expected[640 + i] = i + 1;
    unsigned word = dynamic ? 700 + i : 20 * i + 15;
    expected[word] = (atomic_write ? expected[word] : 0u) + i + 1;
  }
  // 首个会话从进程静态初值开始；rp_draw_reset 是退出会话时的清理。
  rp_state_reset();
  nvn_unimpl_reset();
  rp_S.ssbo[5][0].addr = (uintptr_t)guest;
  // 间接参数在同池另一区间，客体绑定不包含它。
  rp_S.ssbo[5][0].size = 3840;
  GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
  p_dispatch dp = {.x = 2, .y = 1, .z = 1};
  if (indirect)
    dp.ind = (uintptr_t)(guest + 3840);
  rp_dispatch(active, &dp, NULL);
  REQUIRE(nw == 1 && nvn_unimpl_kinds() == 0);
  if (enable_accounting || enable_versioned) {
    uint32_t owned[sizeof expected / sizeof *expected];
    REQUIRE(rp_buf_guest_snapshot((uintptr_t)guest, owned, sizeof owned, RP_LAND_READ_HOST) == EAGAIN);
    const struct rp_gpu_write_ref ref = {wkey[0], wid[0], 0};
    rp_buf_land_seal(&ref, 1, 1);
    GPU(nvn_gpu_cmd_commit(active, 1, land_done, NULL, &err));
    REQUIRE(rp_buf_guest_snapshot((uintptr_t)guest, owned, sizeof owned, RP_LAND_READ_HOST) == 0);
    REQUIRE(!memcmp(owned, expected, sizeof owned));
    if (enable_versioned) {
      uint64_t producer = 0;
      REQUIRE(rp_buf_replay_snapshot((uintptr_t)guest + 60, owned, 16, 1, &producer) == 0);
      REQUIRE(producer == 1 && !memcmp(owned, (const uint8_t*)expected + 60, 16));
      REQUIRE(rp_buf_replay_snapshot((uintptr_t)guest + 2544, owned, 16, 1, &producer) == 0);
      REQUIRE(producer == 0 && !memcmp(owned, (const uint8_t*)expected + 2544, 16));
      guest[60] ^= 0xff;
      REQUIRE(rp_buf_replay_snapshot((uintptr_t)guest + 60, owned, 16, 1, &producer) == 0);
      REQUIRE(producer == 0 && ((const uint8_t*)owned)[0] == (uint8_t)guest[60]);
      guest[60] ^= 0xff;
      printf("land-versioned real fence=1 matched_prefix=1 untouched_suffix=1 cpu_overwrite=1\n");
    }
    rp_buf_land_diag_log(1);
    printf("land-accounting software fence=1 owned_bytes=%zu all_match=1\n", sizeof owned);
  } else {
    GPU(nvn_gpu_cmd_commit(active, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    rp_buf_gpu_write_land(wkey[0], wid[0], 1);
  }
  REQUIRE(!memcmp(guest, expected, sizeof guest));
  size_t want = sizeof(uint32_t) * 960;
  if (!strcmp(arm, "linear") || enable_accounting || enable_versioned) want = 2544;
  if (translated) want = 2604;  // 63 B worst-case descriptor bias, truncated to words
  printf("land-bound %s copied=%zu expected=%zu all_guest_bytes_match=1\n", arm, copied_bytes,
         want);
  rp_draw_reset();
  rp_buf_reset();
  nvn_gpu_shader_release(prog.sh[0]);
  GPU(nvn_gpu_close(5000000000ull, &err));
  REQUIRE(!s2a_vk_device_destroy(&dev, 1));
  REQUIRE(copied_bytes == want);
}

int main(int argc, char** argv) {
  REQUIRE(argc == 3);
  if (!strcmp(argv[2], "reopen")) {
    run_arm(argv[1], "default");
    run_arm(argv[1], "linear");
  } else {
    run_arm(argv[1], argv[2]);
  }
  return 0;
}
