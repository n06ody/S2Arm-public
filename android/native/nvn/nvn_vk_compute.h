// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include "direct/compile_request.h"
#include "direct/texture_facts.h"
#include "direct/vk_record.h"
#include "nvn_vk_compute_lookup.h"
#ifdef __cplusplus
extern "C" {
#endif
// 编译期事实：NVN CS stage 观察到的组合纹理绑定（slot → handle）→ REQUEST 的 program.textures[]
// 条目。坐标与图形路径逐字节一致：handle = 8 + 2*slot、cbufSlot = -1、stage = "cs"。
// 本结构只在 bridge 的一次调用里存活（items 指向同结构的 format/sampler），不要按值拷贝后继续用。
#define NVK_COMPUTE_TEXTURE_MAX 64
struct nvk_compute_texture_set {
    struct s2a_compile_texture_src items[NVK_COMPUTE_TEXTURE_MAX];
    char format[NVK_COMPUTE_TEXTURE_MAX][S2A_TEX_FORMAT_MAX];
    char sampler[NVK_COMPUTE_TEXTURE_MAX][S2A_TEX_SAMPLER_MAX];
    int count;
};
/* 一次 dispatch 在**消费点**拿全的纹理快照。每个观察到的 NVN 组合纹理槽对应一条：
 * 槽位、原始 handle、纹理/采样器 id、解析时的注册表代际、已解析并 pin 住的真实对象
 * （struct nvn_vk_tex*，带它自己的 serial/generation）、精确 VkImageView、VkSampler，
 * 以及同一对象导出的编译事实（格式/采样器类型/数组长度）。
 * 编译请求和运行期 descriptor 绑定吃的是**同一份**条目：编译等待期间发生对象 Finalize、
 * 同槽换对象或注册表换代，都不会让两侧看到不同对象；无法证明是同一份就整笔失败。 */
struct nvk_compute_tex_binding {
    unsigned slot;                 // NVN 组合纹理槽（0..31）
    uint64_t handle;               // state->tex_handle[slot] 原样（纹理 id | 采样器 id << 20）
    uint32_t tex_id, samp_id;
    uint64_t registry_generation;  // 解析时 s2a_vk_adapter_registry_generation()
    struct nvn_vk_tex *tex;        // 已 pin；finish/-release 之前不可销毁
    VkImageView view;              // 精确视图（可能是视图覆盖，也可能是默认整幅）
    VkSampler sampler;
    struct s2a_nvn_texture_fact fact;   // 同一对象的编译事实（无指针，可随结构拷贝）
};
struct nvk_compute_tex_snapshot {
    struct nvk_compute_tex_binding items[NVK_COMPUTE_TEXTURE_MAX];
    int count;
};
// 解析 + 保活 state 里观察到的全部纹理绑定，并导出编译事实。返回 0 = 快照完整（count 可为 0）；
// -1 = 有绑定描述不出来（id 未注册/目标未映射/解析失败/注册表在取快照期间换代/对象与事实不一致），
// 此时不会留下任何保活，why 带 slot/id/handle/原始格式/目标/注册代际。
int nvk_compute_texture_snapshot_acquire(struct s2a_vk_device *device,
    const struct s2a_record_dispatch_state *state, struct nvk_compute_tex_snapshot *out,
    char *why, size_t why_bytes);
// 解除快照登记的全部保活（幂等；成功后清零）。
void nvk_compute_texture_snapshot_release(struct nvk_compute_tex_snapshot *snapshot);
// 按 NVN 槽位查快照条目；没找到返回 NULL（反射有、快照没有 = 明确失败，不猜）。
const struct nvk_compute_tex_binding *nvk_compute_texture_snapshot_find(
    const struct nvk_compute_tex_snapshot *snapshot, unsigned slot);
// 由快照生成 REQUEST 的 program.textures[]（指针指向 set 自身，只在本函数返回后、set 存活期内有效）。
void nvk_compute_texture_facts(const struct nvk_compute_tex_snapshot *snapshot,
    struct nvk_compute_texture_set *set);
/* Build only: record owns submission and must keep output.owner reachable until
 * completion and COMPUTE_SHADER/SHADER_WRITE -> HOST/HOST_READ visibility are
 * proven by the command owner before the submitted fence. finish(-2) retains every object; finish(0) copies SSBOs
 * back then frees; finish(-1) frees a never-submitted build without copyback.
 * SupportBuffer reads are compiler-proven; only native no-scaling float1.0
 * elements are supplied. Proven-unused support descriptors are omitted. No
 * synthetic support, textures, images, writable aliases or unknown slots are substituted.
 * Reflected textures come from the caller's dispatch snapshot (handleIndex 8+2*slot): a texture
 * that reflection names but the snapshot does not contain is a hard failure, never a placeholder.
 * On success the owner takes ownership of the snapshot's keep-alive references and the caller's
 * snapshot is cleared; on failure the snapshot is left untouched for the caller to release.
 * program is the successful exact lookup result: its typed profile was extracted
 * from REQUEST bytes and matched against the provider and program identity.
 * Its alignment must equal the actual physical device capability.
 * True storage variants stage the full aligned window, including its prefix.
 * Storage provenance is resolved, never guessed: NVN c0 table entries carry an
 * explicit {addr,size}; UBO_PTR binds the dispatch state's recorded UBO window;
 * UBO_ADDRESS reads the 8-byte raw pointer from the bound user UBO (ga2ca when
 * the frontend supplies one) and binds the registered-pool mapped remainder,
 * truncated at upstream MaxUnknownStorageSize (1 MiB) with a per-shader-key
 * [s2a-direct-diag] line on first hit. Unbound source UBOs, out-of-UBO
 * descriptor offsets, unmapped pointers and absent pool seams are explicit
 * rejections, not substituted ranges.
 * The memory callbacks/opaque must remain valid until finish(0/-1). */
/* c1 常量段（cbuf 1）与 c0 一样是每笔命令都可能变的内容：它只能来自本次 dispatch 在消费点
 * pin 住的源快照（source->c1/c1_bytes），不允许按槽号事后回查、也不允许绑零缓冲顶替。 */
/* compute PSO 进程内缓存（debug.s2a.compute_pso_cache=1 才有内容）的整表释放：
 * session 在 s2a_vk_device_destroy 前调（幂等；表空/别的设备 = 无操作）。
 * 在途 owner 引用的对象由其 finish/destroy 收尾，不在此强制销毁。 */
void nvn_vk_compute_pso_cache_release(struct s2a_vk_device *device);
/* vkCreateComputePipelines 的进程累计：调用次数与总耗时（ns）。与缓存门无关、永不清零——
 * 逐帧 TSV cpso_n/cpso_us 对它差分；宿主替身与设备同口径。 */
void nvn_vk_compute_cpso_totals(uint64_t *creates, uint64_t *ns);
int nvn_vk_compute_pipeline_build(struct s2a_vk_device *device,
    const struct s2a_record_dispatch_state *state,
    const struct s2a_record_memory *memory,
    struct nvk_compute_tex_snapshot *snapshot,
    const struct s2a_source_entry_view *source,
    const struct nvk_compute_lookup_result *program,
    struct s2a_record_dispatch_output *out, char *why, size_t why_bytes);
#ifdef __cplusplus
}
#endif
