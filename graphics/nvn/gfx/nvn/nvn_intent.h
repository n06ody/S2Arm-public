// 游戏在 NVN 层显式给出的意图（诊断探针，批 0）：Discard 调用、内存池创建标志、贴图创建标志。
// 目的：数清有多少「不用猜」的意图回放层现在没用上（前端 Discard 是空函数；CPU 不访问的池仍进写跟踪）。
// 前端 hle_nvn.c 记，回放 rp_present.c 每 5 秒读一次（弱链接：没有前端的单测构建里读不到就不打）。
#pragma once
#include <stdint.h>
struct nvn_intent {
    uint64_t discard[9];                 // 本窗口：DiscardColor 第 0..7 槽、[8] = DiscardDepthStencil（读取即清零）
    uint64_t pool_n[16], pool_b[16];     // 累计：按 (CPU 访问 0 无 / 1 不缓存 / 2 缓存 / 3 没给) × 4 + (GPU 访问同口径) 分桶的池个数 / 字节
    uint64_t pool_comp_b, pool_shader_b, pool_virt_b, pool_phys_b;   // 累计：带 COMPRESSIBLE / SHADER_CODE / VIRTUAL / PHYSICAL 的池字节
    uint64_t pool_noaccess_tracked_b;    // 累计：CPU_NO_ACCESS 却有存储、进了写跟踪的池字节
    uint64_t tex_n, tex_bit[11];         // 累计：建过的贴图数、各贴图标志位（DISPLAY / VIDEO_DECODE / IMAGE / COMPRESSIBLE / LINEAR / SPARSE / LINEAR_RT / ADAPTIVE_ZCULL / D16_FAST_CLEAR / MINIMAL_LAYOUT / ZCULL_STENCIL）的个数
};
void nvn_intent_take(struct nvn_intent *out);
