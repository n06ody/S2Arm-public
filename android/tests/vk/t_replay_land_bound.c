// SPDX-License-Identifier: Apache-2.0
// 实际 SPIR-V、直连派发、GPU fence 后的客体回写。测试钩子只计量真实 memcpy。
#define main barrier_fixture_main
#define nvn_plat_flag barrier_fixture_flag
#include "t_replay_dispatch_barrier.c"
#undef nvn_plat_flag
#undef main

static int enable_bound;
static size_t copied_bytes;
int nvn_plat_flag(const char* name, int fallback) {
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
  GPU(nvn_gpu_cmd_commit(active, 1, done, NULL, &err));
  GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
  rp_buf_gpu_write_land(wkey[0], wid[0], 1);
  REQUIRE(!memcmp(guest, expected, sizeof guest));
  size_t want = sizeof(uint32_t) * 960;
  if (!strcmp(arm, "linear")) want = 2544;
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
