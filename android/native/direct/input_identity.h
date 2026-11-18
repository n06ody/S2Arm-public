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
#include "texture_facts.h"   // 解码存储要按纹理事实的名字上限开定长数组（S2A_TEX_*_MAX）

#ifdef __cplusplus
extern "C" {
#endif

// 480 个纹理事实 × 17 字节，再加源、图形状态和字符串；实际溢出仍明确失败。
#define S2A_INPUT_IDENTITY_MAX 16384
// 解码存储上限（见 struct s2a_input_identity_decoded）：编码端只会产生这几类短名。
//   · 名字类（topology / attributeTypes / fragmentOutputTypes / alphaTestCompare / 纹理 stage 与
//     sampler / format 名）：表内最长 "TrianglesAdjacency" / "R16G16B16A16Unorm" = 18 字节；
//   · translator closure：provider 的 closure 字段是 char[128]（android/native/direct/graphics.h），
//     所以 blob 里最多 127 字节 + NUL。
// 超过上限的字符串一律解码失败（返回 -1），绝不截断成“看起来相同”的输入。
#define S2A_INPUT_IDENTITY_NAME_MAX 48
#define S2A_INPUT_IDENTITY_CLOSURE_MAX 128

struct s2a_input_identity {
    int direct_abi;
    const char *translator_closure;
    uint8_t profile_sha[32];
    uint32_t stage_mask;
    int requires_linked_varying_interface; // explicit request policy; zero preserves legacy identity
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

// 解码的存储区：字符串与 textures[] 都内嵌在这里，id/gfx 的指针指向本结构内部，
// 生命周期随调用方给的这块结构（≈ 66 KB，别放栈上：用静态量或堆）。
// gfx/textures 的字符串域一律指到下面的定长存储，不需要调用方再补。
struct s2a_input_identity_decoded {
    struct s2a_input_identity id;
    struct s2a_compile_graphics_src gfx;
    struct s2a_compile_texture_src tex[S2A_COMPILE_TEXTURE_MAX];
    char closure[S2A_INPUT_IDENTITY_CLOSURE_MAX];
    char topology[S2A_INPUT_IDENTITY_NAME_MAX];
    char fragment_output_types[8][S2A_INPUT_IDENTITY_NAME_MAX];
    char attribute_types[32][S2A_INPUT_IDENTITY_NAME_MAX];
    char alpha_test_compare[S2A_INPUT_IDENTITY_NAME_MAX];
    char tex_stage[S2A_COMPILE_TEXTURE_MAX][4];                       // "vs"/"fs"/"cs"/"gs"
    char tex_sampler_type[S2A_COMPILE_TEXTURE_MAX][S2A_TEX_SAMPLER_MAX];
    char tex_format[S2A_COMPILE_TEXTURE_MAX][S2A_TEX_FORMAT_MAX];
};

// 编码到 buf（容量 cap）。返回 0 成功并写 *len；-1 = 容量不够/参数缺失（调用方准确失败，不截断）。
int s2a_input_identity_encode(const struct s2a_input_identity *in, uint8_t *buf, size_t cap, size_t *len);

// 解码一版规范输入（诊断、清单随记、宿主回归用）。语义：**只接受能原样重编码的 blob**——
// 解析完成后按同一编码器重新编码并与输入逐字节比较，不同就返回 -1。因此
//   s2a_input_identity_decode(blob) 成功  ⇔  s2a_input_identity_encode(decoded) 逐字节等于 blob。
// 明确失败（返回 -1 并写 why，why 可为 NULL）的情形：参数错 / 超长（> S2A_INPUT_IDENTITY_MAX）/
// 截断 / magic 不符 / 未知版本 / 版本与内容不一致（v3 内容却标 v4 或反之）/ 阶段掩码非法 /
// source_present 与阶段数不符 / 真实源长度为 0 / 纹理数量超限 / 纹理 stage·target·format 索引非法或
// 非规范（同一名字有多个索引时只接受编码器会产出的那个）/ 未知标志位（非 0/1）/ cbufSlot < -1 /
// 字符串超存储上限 / 末尾多余字节 / 重编码不一致。线程：[任意]（无全局状态）。
int s2a_input_identity_decode(const uint8_t *buf, size_t len, struct s2a_input_identity_decoded *out,
                              char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
