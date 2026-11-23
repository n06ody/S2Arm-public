// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// ReadTexels 读 GPU 内容（片 1），被 rp_entry.c 收进来编；真后端夹具（android/tests/vk/t_replay_read_texels.c）单独收它、其余以替身代。
// 用到：rp_side_begin / rp_side_commit / rp_in_submit / nvn_be_last_fence（rp_exec.c）、rp_tex_resolve（rp_res.c）、后端 nvn_gpu_*、提交锁（hle_nvn.c，弱引用）。
// ② 内容归 GPU 的读回。开段 / 交段是 [提交] 角色、时间线只有一条：拿提交锁（hle_nvn.c；没有它的构建不读）、不在某次提交的回放里。
//    先等之前已交的活全部做完（同 iOS mtl_finish_gpu：游戏按 NVN 规则读前已等过，但新层的 Finish / SyncWait 有「晚一帧放行」，等过 ≠ 做完），
//    再开一段旁路段（rp_side_begin）：逐层 / 逐片 copy_tex_buf 到一块共享缓冲 → 交段 → 等完 → 按行距 / 片距拷进 dst。
//    能不能拷在开段之前就判完（NVN_GPU_FMT_READBACK 与后端 copy_tex_buf 是同一个判断；块对齐同 t2b）：带粘滞错误的段会让整条时间线进失败终态，
//    比读不到严重得多。任何一步不成：不动 dst、具名记账。
// （两个提交锁的弱链接登记在 rp_entry.c：登记工具只扫 replay/*.c）
#include "rp_idle.h"
void nvn_hle_submit_lock(void) __attribute__((weak));
void nvn_hle_submit_unlock(void) __attribute__((weak));
static uint64_t rt_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
// 读数（照 iOS 的口径：第 1、10、100 次及之后每 1000 次一行）：次数、等之前的活共多久 / 最长、读回本身共多久
static _Atomic uint64_t g_rt_n, g_rt_wait_ns, g_rt_wait_max, g_rt_copy_ns;
static void rt_note(uint64_t wait, uint64_t copy, int w, int h) {
    const uint64_t k = atomic_fetch_add(&g_rt_n, 1) + 1, tw = atomic_fetch_add(&g_rt_wait_ns, wait) + wait, tc = atomic_fetch_add(&g_rt_copy_ns, copy) + copy;
    uint64_t m = atomic_load(&g_rt_wait_max); while (wait > m && !atomic_compare_exchange_weak(&g_rt_wait_max, &m, wait)) {}
    if (k == 1 || k == 10 || k == 100 || k % 1000 == 0) {
        char b[320]; snprintf(b, sizeof b, "ReadTexels 读 GPU 写过的贴图：本局第 %llu 次；先等之前的活共 %.1f 毫秒、最长 %.2f 毫秒（这次 %.2f 毫秒）；读回共 %.1f 毫秒（这次 %.2f 毫秒，%dx%d）",
            (unsigned long long)k, tw / 1e6, (wait > m ? wait : m) / 1e6, wait / 1e6, tc / 1e6, copy / 1e6, w, h);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "be", b);
    }
}
void nvn_be_read_texels_gpu_stats(uint64_t out[4]) { out[0] = atomic_load(&g_rt_n); out[1] = atomic_load(&g_rt_wait_ns); out[2] = atomic_load(&g_rt_wait_max); out[3] = atomic_load(&g_rt_copy_ns); }   // 测试读数
// first：二维数组 / 立方体是起始层（layer0 + zoffset），三维是起始片（zoffset）；nz 层 / 片。wb / hb：一行块字节数 / 块行数。
static void rt_gpu(const tex_t *t, unsigned level, uint32_t first, uint32_t nz, const NVNcopyRegion *r, uint64_t wb, uint64_t hb,
                   int bw, int bh, uint32_t lw, uint32_t lh, uint8_t *dst, size_t row, size_t img) {
    const int target = t->d.target;
    if (target != NVN_TEXTURE_TARGET_2D && target != NVN_TEXTURE_TARGET_2D_ARRAY && target != NVN_TEXTURE_TARGET_3D &&
        target != NVN_TEXTURE_TARGET_CUBEMAP && target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) { nvn_unimpl("be", "read_texels_gpu_target", t); return; }
    if (!(nvn_gpu_format_caps((uint32_t)t->d.format) & NVN_GPU_FMT_READBACK)) { nvn_unimpl("be", "read_texels_gpu_format", t); return; }
    // 块对齐（同 rp_copy_t2b.c / 后端 copy_tex_buf）：起点落在块边上，宽高是块的整数倍或者一直到这一级的边
    if (r->xoffset % bw || r->yoffset % bh || ((uint32_t)r->width % (uint32_t)bw && (uint64_t)r->xoffset + (uint32_t)r->width != lw) ||
        ((uint32_t)r->height % (uint32_t)bh && (uint64_t)r->yoffset + (uint32_t)r->height != lh)) { nvn_unimpl("be", "read_texels_gpu_unaligned", t); return; }
    if (!nvn_hle_submit_lock || !nvn_hle_submit_unlock) { nvn_unimpl("be", "read_texels_gpu_no_submit_lock", t); return; }
    if (rp_in_submit()) { nvn_unimpl("be", "read_texels_inside_submit", t); return; }   // 同一线程持着提交锁：再拿会自锁
    // 共享缓冲：每层 / 片紧排（一行 wb 字节），片与片之间按 16 字节对齐（后端要求偏移 4 字节对齐且是块字节数的倍数）
    uint64_t slice = wb * hb, ss = (slice + 15) & ~(uint64_t)15, total = 0;
    if (!slice || __builtin_mul_overflow(ss, (uint64_t)nz, &total)) { nvn_unimpl("be", "read_texels_bad_stride", t); return; }
    nvn_hle_submit_lock();
    const uint64_t t0 = rt_now();
    nvn_gpu_err err = {0};
    const uint64_t prior = nvn_be_last_fence();
    if (prior && !nvn_gpu_ok(rp_fence_wait_at(prior, 5000000000ull, &err, NVN_GPU_WAIT_READ_PRIOR))) { nvn_hle_submit_unlock(); nvn_unimpl("be", "read_texels_gpu_wait", t); return; }
    const uint64_t t1 = rt_now();
    nvn_gpu_buf buf = NULL;
    if (!nvn_gpu_ok(nvn_gpu_buf_create(total, NVN_GPU_MEM_SHARED, NULL, &buf, &err)) || !buf || !nvn_gpu_buf_cpu(buf)) {
        nvn_hle_submit_unlock(); nvn_gpu_buf_release(buf, NULL, NULL); nvn_unimpl("limit", "read_texels_gpu_staging", t); return;
    }
    nvn_gpu_cmd c = rp_side_begin("ReadTexels");
    nvn_gpu_tex g = c ? rp_tex_resolve(t, c) : NULL;
    if (!g) {   // 开不了段（已记账）/ 宿主贴图建不出来（已记账）：开着的段交掉（空段也要占一格时间线，不能丢弃别人可能等的值）
        if (c) rp_side_commit(c);
        nvn_hle_submit_unlock(); nvn_gpu_buf_release(buf, NULL, NULL); nvn_unimpl("be", "read_texels_gpu_no_storage", t); return;
    }
    NVNcopyRegion rr = *r; rr.zoffset = 0; rr.depth = 1;
    for (uint32_t z = 0; z < nz; z++) nvn_gpu_copy_tex_buf(c, g, level, first + z, &rr, buf, (uint64_t)z * ss, wb, slice);
    const uint64_t f = rp_side_commit(c);
    const int ok = f && nvn_gpu_ok(rp_fence_wait_at(f, 5000000000ull, &err, NVN_GPU_WAIT_READ_COPY));
    nvn_hle_submit_unlock();
    if (!ok) { nvn_gpu_buf_release(buf, NULL, NULL); nvn_unimpl("be", "read_texels_gpu_wait", t); return; }
    const uint8_t *src = nvn_gpu_buf_cpu(buf);
    const struct nvn_gpu_idle_observer *o = rp_idle_observer();
    uint64_t begin = o ? o->clock_ns() : 0;
    for (uint32_t z = 0; z < nz; z++) for (uint64_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + (size_t)y * row, src + (size_t)z * ss + (size_t)(y * wb), (size_t)wb);
    if (o) o->note(NVN_IDLE_READ_TEXELS, buf, src, slice * nz, o->clock_ns() - begin);
    nvn_gpu_buf_release(buf, NULL, NULL);
    rt_note(t1 - t0, rt_now() - t1, r->width, r->height);
}
