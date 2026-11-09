// F4.4 driver pipeline worker：需求编译 worker 创建 VkGraphicsPipeline，完成后转交 owner 安装。
//
// 合同（统一方案 §8.0.3 F4.4、实施契约 §4.3）：
//   * 使用同一 VkDevice / 同一 VkPipelineCache；worker 不提交 queue、不改 owner 的全局缓存；
//   * worker 只产结果：pipeline + 配方 + 设备代际。owner 取走前必须复核代际与配方；
//   * 同一配方在途/已产结果时按 ProgramKey 合并，不重复创建；
//   * 有界队列、有界结果环；容量满准确失败，不阻塞 owner；
//   * Stop 有界：未退出就如实返回 1（迟到结果在 worker 内销毁，不转交旧代际结果）；
//     未退出前拒绝重启；下一次 Stop 先回收再 join。
//
// 本模块不接热路径：真正的插入点在 nvn_vk_first_draw.c 的管线创建分支（维护者所有），
// 报告里给出精确签名/插入点。宿主回归用 fake Vulkan 直接驱动本模块的生产实现。
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "nvn_vk_pipe_recipe.h"

#ifdef __cplusplus
extern "C" {
#endif

// W2.7：longframe 归因读这一组计数（只含真实 vkCreateGraphicsPipelines 的次数与墙钟）。
void nvk_pipeline_long_stats(uint64_t *attempts, uint64_t *failures, uint64_t *ns, uint64_t *ns_max);

#define NVK_PW_QUEUE 4
#define NVK_PW_RESULTS 8
#define NVK_PW_MAX_BINDINGS 16
#define NVK_PW_MAX_ATTRS 32
#define NVK_PW_MAX_BLEND 8
#define NVK_PW_MAX_DYNAMIC 16
#define NVK_PW_MAX_STAGES 2
#define NVK_PW_MAX_ENTRY 32
// 单阶段 SPIR-V 上限（与请求合同的 16 MiB 一致；这里按 4 MiB 收紧，超限准确拒绝，不截断）。
#define NVK_PW_MAX_SPIRV_BYTES (4u << 20)

struct nvk_pipe_stage_spec {
    VkShaderStageFlagBits stage;   // 只接受 VERTEX/FRAGMENT（首版 graphics 合同 VS+FS）
    // 二选一：spirv 非空 = worker 自己建/销毁 VkShaderModule；module 非空 = 复用 owner 已建的
    // 程序对象模块（同一 device、只读、由 owner 保活到管线创建完成），worker 不销毁它。
    const uint32_t *spirv;         // 提交时复制
    size_t bytes;                  // spirv 模式：4 的倍数，>0
    VkShaderModule module;         // module 模式：owner 的程序对象模块；与 spirv 互斥
    char entry[NVK_PW_MAX_ENTRY];  // 入口名（"main"），提交时复制
};

// owner 提交的完整创建输入。所有指针指向的内容在 submit 内复制，返回后调用方可以释放。
struct nvk_pipe_job_spec {
    uint64_t device_generation;    // owner 当前设备代际；代际变化后结果一律不转交
    VkDevice device;               // 必须是 worker 登记的同一设备
    VkPipelineCache cache;         // 必须是 worker 登记的同一管线缓存
    VkPipelineLayout layout;       // owner 对象，生命周期由 owner 保活到结果被 take/丢弃
    VkRenderPass render_pass;      // 同上
    uint32_t subpass;
    VkPipelineCreateFlags flags;
    VkPipeline base_pipeline;
    int32_t base_pipeline_index;
    struct nvk_pipe_recipe recipe; // 完整规范配方（缓存判据/转交复核）

    uint32_t stage_count;
    struct nvk_pipe_stage_spec stages[NVK_PW_MAX_STAGES];

    uint32_t binding_count;
    VkVertexInputBindingDescription bindings[NVK_PW_MAX_BINDINGS];
    uint32_t attr_count;
    VkVertexInputAttributeDescription attrs[NVK_PW_MAX_ATTRS];

    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;   // viewportCount/scissorCount 必须 <=1（首版单视口）
    VkViewport viewport;                    // vp.pViewports 非空时复制到这里
    VkRect2D scissor;                       // vp.pScissors 非空时复制到这里
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendStateCreateInfo cb; // 附件数组单独给出
    uint32_t blend_count;
    VkPipelineColorBlendAttachmentState blend[NVK_PW_MAX_BLEND];
    VkPipelineDepthStencilStateCreateInfo ds;
    int has_depth;
    uint32_t dynamic_count;
    VkDynamicState dynamic[NVK_PW_MAX_DYNAMIC];
};

struct nvk_pipe_job_result {
    uint64_t job_id;
    uint64_t device_generation;
    struct nvk_pipe_recipe recipe;
    VkPipeline pipeline;   // ok=0 时为 VK_NULL_HANDLE
    VkDevice device;       // 释放/丢弃该 pipeline 用；owner 转交成功后不再使用
    int ok;
    char why[192];
};

struct nvn_vk_pipe_worker_stats {
    uint64_t submitted;
    uint64_t deduped;
    uint64_t created;
    uint64_t create_failed;
    uint64_t taken;
    uint64_t dropped;          // 结果环满/Stop/换代：worker 内销毁的 pipeline
    uint64_t rejected_stale;   // 设备代际不匹配，拒绝创建或丢弃
    uint64_t queue_full;
    uint64_t refused_restart;
};

// 幂等启动。device/cache 由 owner 保活到 worker stop；同一 process 只允许一个 worker。
// 返回 0 成功；-1 失败（why 写明）；未退出的旧线程存在时拒绝启动并写原因。
int nvn_vk_pipe_worker_start(VkDevice device, VkPipelineCache cache, uint64_t device_generation,
                             char *why, size_t why_size);
int nvn_vk_pipe_worker_active(void);
void nvn_vk_pipe_worker_set_generation(uint64_t device_generation);
uint64_t nvn_vk_pipe_worker_generation(void);

// 提交：0 = 已接收；1 = 同配方已在队列/在途/待取（去重，不重复创建）；-2 = 队列满；
// -3 = worker 未运行；-4 = device/cache/代际不匹配；-1 = 参数或容量错误（why 写明）。
int nvn_vk_pipe_worker_submit(const struct nvk_pipe_job_spec *spec, uint64_t *job_id,
                              char *why, size_t why_size);
// owner 取结果：1 = 有结果（out->ok 可能为 0）；0 = 暂无；-1 = 参数错误。
// 结果里的代际/配方仍须 owner 复核；不匹配时调 release 丢弃，不能装进自己的缓存。
int nvn_vk_pipe_worker_take(struct nvk_pipe_job_result *out);
// 按配方等待：1 = 取到该配方结果（同代际才转交；out->ok 可能为 0，why 写明原因）；
// 0 = 该配方仍在队列/在途且 deadline 到；-1 = 该配方在队列/在途/结果环都没有任何证据
// （提交被拒/结果已被挤掉/stop 清场：owner 不能空等，回同步创建或如实失败）。
// 命中时把该条从结果环中取出（其余结果保持 FIFO 不动）。
int nvn_vk_pipe_worker_wait(const struct nvk_pipe_recipe *recipe, uint64_t device_generation,
                            uint32_t deadline_ms, struct nvk_pipe_job_result *out);
// 丢弃/释放 owner 尚未安装的结果：销毁 pipeline 并清零。已安装的结果由 owner 自己管理。
void nvn_vk_pipe_worker_release(struct nvk_pipe_job_result *out);
// 有界 Stop：0 = worker 已退出并回收；1 = 仍在创建（未 join；迟到结果丢弃，下一次 Stop 回收）。
int nvn_vk_pipe_worker_stop(uint32_t budget_ms, char *why, size_t why_size);

// worker 创建与 owner 直接创建共用同一个 VkPipelineCache：owner 在自己的创建路径前后必须持锁。
// （热路径接线时由维护者在 nvn_vk_first_draw.c 的 vkCreateGraphicsPipelines 外包这对函数。）
void nvn_vk_pipe_worker_cache_lock(void);
void nvn_vk_pipe_worker_cache_unlock(void);

void nvn_vk_pipe_worker_stats(struct nvn_vk_pipe_worker_stats *out);

#ifdef __cplusplus
}
#endif
