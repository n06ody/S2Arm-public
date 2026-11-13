// F4.3：编译查找与编译本身共用的完整规范输入。
// 只编码“内容身份”：profile、ABI、translator closure、各阶段完整源摘要/长度与 constantBufferUse、
// 纹理/采样器池容量、TF、以及 graphicsState 的全部真实事实。session/generation/requestId
// 只是传输或本局有效性字段，不进内容身份。
// 固定顺序、显式长度、小端；字符串区分 NULL 与空串。任何未覆盖字段都是缓存误命中的入口。
// 版本：S2AI1 内第 3 版（2026-10-27 起纳入完整源摘要与真实 code/control 长度；短 stage key 只是摘要
// 前缀，不能单独证明内容相同）。旧版本的编码不再被接受，磁盘条目随之自然不命中。
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "compile_request.h"

#ifdef __cplusplus
extern "C" {
#endif

// 480 个纹理事实 × 17 字节，再加源、图形状态和字符串；实际溢出仍明确失败。
#define S2A_INPUT_IDENTITY_MAX 16384

struct s2a_input_identity {
    int direct_abi;
    const char *translator_closure;
    uint8_t profile_sha[32];
    uint32_t stage_mask;
    uint8_t keys[6][8];
    // 完整源身份（与 keys 同序：第 i 项对应第 i 个置位的 NVN 阶段）。
    // source_present 必须恰好覆盖 stage_mask 的全部阶段，缺一个就是“缺完整源证明”——编码失败，
    // 不能拿短 key 冒充内容相同。*_bytes 是真实快照长度：相同拼接摘要、不同切分也是不同输入。
    uint8_t source_present;
    uint8_t source_sha[6][32];
    uint32_t source_code_bytes[6];
    uint32_t source_control_bytes[6];
    uint32_t ubo_use[6];
    uint32_t texture_array_length;
    uint32_t sampler_array_length;
    int transform_feedback_enabled;
    // Compute-only alignment fact; appended only for stage_mask == 0x20.
    int compute_has_unaligned_ssbo, compute_unaligned_ssbo;
    const struct s2a_compile_graphics_src *gfx;
    const struct s2a_compile_texture_src *textures;   // program.textures[] 真实事实（F4.2B）
    int texture_count;
};

// 编码到 buf（容量 cap）。返回 0 成功并写 *len；-1 = 容量不够/参数缺失（调用方准确失败，不截断）。
int s2a_input_identity_encode(const struct s2a_input_identity *in, uint8_t *buf, size_t cap, size_t *len);

#ifdef __cplusplus
}
#endif
