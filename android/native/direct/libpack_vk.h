// #1434 安卓离线 Vulkan 直连着色器库：包内 <资源根>/shaders/direct/b-<链>-vk/ 的 LibPack 容器
//   （lib.idx + lib.NNN.pak，与 iOS 同一种容器——共用核在 graphics/nvn/lib_pack_core.inc，
//   POSIX 段在这里：键的 _b.json 种类 1 + _b.spv 种类 52 分片段；几何键按输入图元 _vg<t>.json 112+t + _vg<t>.spv 120+t）。
// 读法与 iOS 同键：NVN 每阶段 8 字节控制键（state->keys[idx]，文件名前 16 位 hex）→ lp_find。
//   _b.json 是逐键单阶段的反射（dsh 批翻链，Metal/Vulkan 共用同一份），_b.spv 是同键 Vulkan 产物
//   （绑定号已按 arg_id×6+阶段号烘焙：vs 0 / gs 3 / ps 4 / cs 5，DshVk.BindToArgIds）。
//   几何着色器没有 Metal 对应物：_vg<t>.json 是 DshVk.CompileGeometry 照 .spv 写的同格式反射，t = 输入图元（点 0 / 线 1 / 三角 3）。
// 命中判据：键的 _b.json 与 _b.spv 都在、_b.json 的 dm_ctl == 键 hex、stage 是本段阶段、
//   _b.spv 是合法 SPIR-V 头；合成的 s2a-direct-program-v1/v2 字节再走 nvn_vk_first_draw 的
//   parse_program 同一套接受——命中不等于可绘制（与 offline_library.h 同一口径）。
// 计数口径：hit = 全部阶段都拿到并合成通过；miss = 包不在/键不在/缺 json 或 spv/
//   程序含 vk 库不出的阶段（tcs/tes）/ 几何程序而设备没开 geometryShader 或输入图元没有那一份（邻接）；
//   reject = 容器坏/段越界/json 损坏/键不符/绑定翻译不出的 kind/几何程序写视口号（不静默丢弃）。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    S2A_LIBPACK_HIT = 0,      // 全部阶段都有，程序字节已合成
    S2A_LIBPACK_MISS = 1,     // 容器不在、某键某段不在、或阶段在库外出路（tcs/tes、没开 geometryShader 的几何）
    S2A_LIBPACK_REJECT = -1,  // 找到了但不可接受：容器坏/段坏/json 坏/键不符/绑定种类译不出
    S2A_LIBPACK_INVALID = -2, // 调用方参数错
};

// 没走出库 / 拒收的原因码（计算取库诊断按码分类，不再从报错文本里找字眼：改文案不会把原因悄悄归错类）。
//   UNALIGNED：存储缓冲非对齐被对齐闸拒（闸在调用方，库件只有对齐变体；库本身不会返回它）；
//   SHARED_LIMIT：spill 程序的 Workgroup 字节数超设备 maxComputeSharedMemorySize，出库拒；
//   REFLECT：SPIR-V 反射 / 能力核拒（不是 SPIR-V、用了设备不开的能力、描述符与声明对不上、装饰读不出）；
//   OTHER：其余拒收（_b.json 损坏 / 键不符 / 绑定种类译不出 / 内存不足等）。
enum s2a_libpack_why {
    S2A_LIBPACK_WHY_NONE = 0,
    S2A_LIBPACK_WHY_UNALIGNED = 1,
    S2A_LIBPACK_WHY_SHARED_LIMIT = 2,
    S2A_LIBPACK_WHY_REFLECT = 3,
    S2A_LIBPACK_WHY_OTHER = 4,
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

// 程序命中：keys[6][8] + stages 位图（只有 vs 0x1、vs+fs 0x3 或 vs+fs+gs 0x7）→ 合成的 s2a-direct-program-v1/v2 字节（malloc，调用方 free）。
//   每阶段取 _b.json + _b.spv：绑定号 = arg_id×6+阶段号（与 .spv 内烘焙的一致），kind 桶照
//   _b.json 的语义种类译：cbuf→uniform、tex/samp_tex→texture、img→image、sbuf 系→storage、
//   support→support(stage="program")；…Smplr / samp* 不另出描述符行。独立 c2 采样器按 v2 附在纹理的 samp_slot；
//   宿主辅助（uvx/lb/ubolen/sbsize/geometry/drvcb/c1）→ uniform/storage 无 guest 槽。
//   任一键缺段/坏段 → 整个程序 MISS/REJECT（半程序不命中）。identity 段由调用方给
//   （direct_abi/profile_hash/translator_closure——与现编 request 同源）。
/* 片元 .spv 的输出映射（同现编结果 info.fragmentOutputMap：每个颜色目标 4 位，第 c 分量 = 位 4×location + c）。
 * 0 = 推出；-1 = 读不懂（数组输出、location ≥ 8 等），调用方不出这一项（消费端按全写）。 */
struct s2a_frag_outputs {
    uint32_t components; // nibble per location
    uint8_t uint_locations, sint_locations;
};
int s2a_libpack_spv_frag_interface(const uint8_t *spv, size_t n, struct s2a_frag_outputs *out);
int s2a_libpack_spv_frag_outputs(const uint8_t *spv, size_t n, uint32_t *map);
// 几何程序（stages 0x7 = vs|fs|gs）要的两样绘制事实；vs+fs 程序不读，可传 NULL（传 NULL 时几何程序 miss）。
//   topology：绘制图元名，与现编请求 graphicsState.topology 同源（nvk_input_topology：Points / Lines / Triangles / …Adjacency）；
//   profile_json：本局 effective profile（s2a_direct_compile_context.profile_json），取 capabilities.supportsGeometryShader，
//   与现编 profile.Caps.SupportsGeometryShader 同一判据：没开 = miss。
struct s2a_libpack_gfx { const char *topology; const char *profile_json; size_t profile_bytes; };
int s2a_libpack_profile_geometry_shader(const char *profile_json, size_t profile_bytes);
/* 能力核（见 .c）：删掉模块用不上的 Float64 / Int64 能力声明（本后端设备从不启用 shaderFloat64 / shaderInt64），
 * 真用到 64 位浮点 / 整型或声明 Int64Atomics 就拒。0 = 可用（*n 可能变小）；-1 = 拒，why 以具名原因开头
 * （fp64_type_used / int64_type_used / int64_atomics_used）。 */
int s2a_libpack_spv_admit_caps(uint8_t *spv, size_t *n, char *why, size_t why_size);
int s2a_libpack_program_v1(const struct s2a_libpack *lp, const uint8_t keys[6][8], uint32_t stages,
                           const struct s2a_libpack_gfx *gfx,
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size);

// 一个图形阶段用到的存储缓冲槽（调用方据此把「存储缓冲绑定不对齐」收窄到程序真用到的那几个槽）。
//   nvn = NVN 阶段号（vs 0 / fs 1 / gs 2）；topology = 本笔绘制的输入图元名（Points / Lines / Triangles，几何段按它挑 _vg<t>.json，
//   口径同 s2a_libpack_program_v1；vs / fs 不看，可传 NULL）。
//   返回 0 = 读到了，*slots 的位 i = 这一段用 NVN 存储缓冲槽 i（0..15），0 位 = 一个存储缓冲都没用；
//   返回 1 = 说不准，*slots 清零、调用方不许把它当「一个槽都没用」：库里没有这一键 / 这一段读不出 / dm_ctl 或阶段对不上 /
//   有 sbuf 以外的存储种类（或库不认得的绑定种类）/ sbuf 槽号 ≥ 16 / 几何输入图元没有那一份 / 这一段的 .spv 取不到或读不懂。
//   返回 2 = 同样说不准（*slots 清零，调用方照 1 处理），原因单列：这一段的 .spv 声明了 PhysicalStorageBufferAddresses 能力
//   （着色器可以按 64 位地址直接读写内存，不经描述符，也就不经 rp_S.ssbo 的槽，_b.json 的绑定表不是它碰得到的全部存储）。
//   依据：图形库件只认 kind "sbuf"（libpack_vk.c lp_emit_bindings：别的存储种类图形侧没有消费端认的出处，整份拒收、走现编），
//   所以 sbuf 的 reg 集合 = 合成出的 storage 绑定的 nvnSlot 集合 = rp_draw 按 rp_S.ssbo[阶段][槽] 绑的那几个槽。
//   只读：不动 hits / misses / rejects 三个计数（它们只记真正的合成）。
int s2a_libpack_stage_sbuf_slots(const struct s2a_libpack *lp, const uint8_t key[8], unsigned nvn, const char *topology, uint32_t *slots);

// 计算程序命中：单个 cs 键（rp_S.skey[5]）→ kind "compute" 的 s2a-direct-program-v1/v2（malloc，调用方 free），
//   按 nvn_vk_program_parse_compute 的合同合成：绑定号 = arg_id×6+5、每条 count 1、storage 带 cbSlot/cbOffset
//   出处与 rw、c0 驱动常量段挂 slot 0；c0Reads / supportReads 写 null（_b.json 给不出，解析器认可的「未知」），
//   hasUnalignedStorageBuffer 写 false（批翻链按缺省 false 翻）。返回码与计数口径同 program_v1。
//   shared_limit = 本设备 maxComputeSharedMemorySize：_b.json 带 spill 标记（批翻链 MSL 侧 32K 阈值溢出事实）
//   时核 _b.spv 的 Workgroup 字节数，超出的程序出库即拒（其 .spv 未做溢出改写，建不出计算管线）。
//   reason（可空）：没取到 / 拒收的原因码（enum s2a_libpack_why）。诊断只按码分类，why 文本只给人看、改文案不影响归类；
//   命中（返回 S2A_LIBPACK_HIT）和「库里没有这一键」（MISS）时是 S2A_LIBPACK_WHY_NONE，只有拒收（REJECT）才带码。
int s2a_libpack_compute_v1(const struct s2a_libpack *lp, const uint8_t key[8],
                           uint32_t direct_abi, const char *profile_hash, const char *translator_closure,
                           uint32_t shared_limit,
                           uint8_t **json, size_t *json_len, char *why, size_t why_size, int *reason);

uint64_t s2a_libpack_hits(void);
uint64_t s2a_libpack_misses(void);
uint64_t s2a_libpack_rejects(void);
uint64_t s2a_libpack_caps_stripped(void);   // 删过用不上的 Float64 / Int64 能力声明的模块条数（libpack_caps_stripped）
uint64_t s2a_libpack_interlock_dropped(void);   // 真有互锁临界区、去掉互锁照常画的模块条数（libpack_interlock_dropped；本后端从不启用 VK_EXT_fragment_shader_interlock）

#ifdef __cplusplus
}
#endif
