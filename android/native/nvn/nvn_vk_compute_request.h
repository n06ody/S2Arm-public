// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include "direct/compile_request.h"
#include "direct/control.h"
#include "direct/input_identity.h"
#include "direct/source_store.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 计算派发的绑定快照（原在 direct/vk_record.h；旧回放层删除时挪到这里，字段不变）。
// Compute 快照独立于 draw；keys 为 packed CS key + 5 个 inactive 零槽，ubo_use 按 NVN stage 编号（只有 [5] 活跃）。
// program 只作参考，serial 来自绑定记录。未观察到的 pool/local memory 不等于零；消费方必须按 seen 标志拒绝缺失事实。
struct s2a_record_dispatch_state {
    const void *program;
    uint64_t program_serial, cmd_seq;
    uint8_t keys[6][8];
    uint32_t stages, groups[3];
    uint32_t tex_pool_n, samp_pool_n;
    int32_t tex_pool_base;              // 已绑定纹理池的全局描述符编号基址；-1 = 未知/没绑
    uint8_t tex_pool_seen, samp_pool_seen;
    uint32_t ubo_use[6];
    int ubo_use_overflow;
    uint32_t ubo_seen, ssbo_seen; // bit per slot: an explicit bind/unbind was observed
    struct { uint64_t addr, size; } ubo[16], ssbo[16];
    uint64_t tex_handle[32], samp_handle[32], image_handle[32];
    uint8_t tex_present[32], samp_present[32], image_present[32];
    uint32_t local_memory_bytes;
    uint8_t local_memory_seen;
    // OP_DISPATCH 帧队列收编门（记录层 debug.s2a.dispatch_fq / 宿主 S2A_DISPATCH_FQ）：
    // =1 时 builder 可以对 guest-backed 绑定试 AHB 池后备直绑（全命中才置 output.fq=1）；
    // =0 时 builder 不做任何池查找，逐字节走 staging 旧路径。
    int fq_pool;
};


/* One immutable source pin backs both REQUEST and identity. Release only after the
 * consumer has copied C1 / finished using source pointers. No device operations. */
/* Extracted from the exact provider profile bytes embedded in REQUEST. */
struct nvk_compute_profile_facts {
    uint32_t storage_alignment;
    uint32_t max_shared;   // capabilities.maximumComputeSharedMemorySize；缺席按 Vulkan 下限 32768（只会多拒不会多放）
    char hash[72];
};
/* Same strict provider-profile parser used by REQUEST and the pre-identity bridge. */
int nvk_compute_profile_parse(const struct s2a_direct_compile_context *context,
    struct nvk_compute_profile_facts *out, char *why, size_t why_bytes);
struct nvk_compute_request {
    struct nvk_compute_profile_facts profile;
    struct s2a_source_entry_view source;
    struct s2a_cs_facts cs;
    uint32_t local_memory_bytes;
    uint64_t session, generation;
    char *json;
    size_t json_bytes;
    uint8_t canonical[S2A_INPUT_IDENTITY_MAX], identity[32];
    size_t canonical_bytes;
};

/* Facts must describe the CS stage, not graphics state inherited from a draw.
 * complete means the caller has proved the required texture queries; false is
 * unknown, not an empty set. Empty proven facts are valid for texture-free CS. */
struct nvk_compute_texture_facts {
    const struct s2a_compile_texture_src *items;
    int count;
    int complete;
};

/* out must be fresh or released; on failure it is empty. Profiles are the provider
 * byte representation; the same bytes are hashed and embedded without reformatting.
 * Alignment is explicit: has_unaligned_ssbo must be 1 and unaligned_ssbo 0/1;
 * unknown is rejected, never silently treated as aligned.
 * This builds input only: unsupported compiler queries still fail in the compiler. */
int nvk_compute_request_prepare(const struct s2a_record_dispatch_state *state,
    const struct s2a_direct_compile_context *context,
    const struct nvk_compute_texture_facts *textures,
    int has_unaligned_ssbo, int unaligned_ssbo, const char *request_id, struct nvk_compute_request *out,
    char *why, size_t why_bytes);
void nvk_compute_request_release(struct nvk_compute_request *request);

#ifdef __cplusplus
}
#endif
