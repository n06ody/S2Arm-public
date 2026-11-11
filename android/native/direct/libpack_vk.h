// #1434 安卓离线 Vulkan 直连着色器库：包内 <资源根>/shaders/direct/b-<链>-vk/ 的 LibPack 容器
//   （lib.idx + lib.NNN.pak，与 iOS 同一种容器——共用核在 graphics/nvn/lib_pack_core.inc，
//   POSIX 段在这里：键的 _b.json 种类 1 + _b.spv 种类 52 分片段）。
// 读法与 iOS 同键：NVN 每阶段 8 字节控制键（state->keys[idx]，文件名前 16 位 hex）→ lp_find。
//   _b.json 是逐键单阶段的反射（dsh 批翻链，Metal/Vulkan 共用同一份），_b.spv 是同键 Vulkan 产物
//   （绑定号已按 arg_id×6+阶段号烘焙：vs 0 / ps 4 / cs 5，DshVk.BindToArgIds）。
// 命中判据：键的 _b.json 与 _b.spv 都在、_b.json 的 dm_ctl == 键 hex、stage 是本段阶段、
//   _b.spv 的描述符和支撑缓冲布局可映射；合成的 s2a-direct-program-v1 字节再走 nvn_vk_first_draw 的
//   parse_program 同一套接受——命中不等于可绘制（与 offline_library.h 同一口径）。
// 计数口径：hit = 全部阶段都拿到并合成通过；miss = 包不在/键不在/缺 json 或 spv/
//   程序含 vk 库不出的阶段（gs/tcs/tes）；reject = 容器坏/段越界/json 损坏/
//   键不符/绑定翻译不出的 kind（不静默丢弃）。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    S2A_LIBPACK_HIT = 0,      // 全部阶段都有，v1 字节已合成
    S2A_LIBPACK_MISS = 1,     // 容器不在、某键某段不在、或阶段在库外出路（gs/tcs/tes）
    S2A_LIBPACK_REJECT = -1,  // 找到了但不可接受：容器坏/段坏/json 坏/键不符/绑定种类译不出
    S2A_LIBPACK_INVALID = -2, // 调用方参数错
};

struct s2a_libpack;   // 不透明句柄（lp_pack 本体在 .c 里，句柄由 open 分配/close 释放）

// 打开 <dir>（应形如 …/shaders/direct/b-<DMSH_CHAIN>-vk）：lib.idx 核头 + 链版本核（dmsh_chain.h）。
// 返回 1 = 容器开着；0 = 没有 lib.idx（不报红，由调用方决定）；-1 = 容器坏（报红）。
// 成功时 *out 为 malloc 的句柄，调用方 s2a_libpack_close 释放。
int s2a_libpack_open(struct s2a_libpack **out, const char *dir);
void s2a_libpack_close(struct s2a_libpack *lp);

// 单段读取：键 + 种类 → malloc 的字节副本（对齐安全，调用方 free）。POSIX 段：分片按页对齐
//   mmap 这一段、memcpy 出来、munmap——不像 Apple 段那样常驻映射（每段一份拷贝随取随放，
//   SPIR-V 建 module / json 解析都要求字节对齐，且安卓侧段小）。
// 返回 0 hit / 1 miss（键或种类不在）/ -1 reject（容器/分片/越界坏）。
int s2a_libpack_get(const struct s2a_libpack *lp, const uint8_t key[8], unsigned kind,
                    uint8_t **bytes, size_t *len, char *why, size_t why_size);

// 程序命中：keys[6][8] + stages 位图 → 合成的 s2a-direct-program-v1 字节（malloc，调用方 free）。
//   每阶段以 _b.spv 的 OpVariable/类型/描述符装饰为准，按名字联 _b.json 的来源事实，
//   只输出真实 Vulkan 描述符；Metal 独有的 uvx/lb/ubolen/sbsize 不凭空生成绑定。
//   support → program support、c0/c1/c3..c18 → 对应 uniform slot、已知 sbuf / 合并纹理
//   取实际来源；不认识的描述符或不支持的槽位明确 REJECT。supportLayout.requiredSize
//   由各阶段 SPIR-V 支撑缓冲的显式偏移/步长计算并取最大，不可计算时拒绝。
//   任一键缺段/坏段 → 整个程序 MISS/REJECT（半程序不命中）。identity 段由调用方给
//   （direct_abi/profile_hash/translator_closure——与现编 request 同源）。
int s2a_libpack_program_v1(const struct s2a_libpack *lp, const uint8_t keys[6][8], uint32_t stages,
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size);

uint64_t s2a_libpack_hits(void);
uint64_t s2a_libpack_misses(void);
uint64_t s2a_libpack_rejects(void);

#ifdef __cplusplus
}
#endif
