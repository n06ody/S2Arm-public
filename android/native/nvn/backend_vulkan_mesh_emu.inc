// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Vulkan 1.1 任务 / 网格着色器计算模拟辅助核（nvn_gpu_mesh_emu_op，契约 v27）。
// 对照 iOS graphics/nvn/gfx/nvn/nvn_geometry.inc（mxe_tclamp / mxe_scan / mxe_monly）：
//   纯 Vulkan 1.1 核心（仅用 128 线程共享内存前缀和 + 4 字节 push 常量 + SSBO，无 Vulkan 1.2/1.3 静态符号与 subgroup 扩展依赖）。
//   间接参数地址任意 4 字节对齐均可：绑定向下对齐到 minStorageBufferOffsetAlignment，余量字偏移经 push 常量传入。
// 由 backend_vulkan.c include（共用 g、set_sticky、alloc_sets、cmd_ref、buf_range、mem_barrier、es_reset）。
#include "backend_vulkan_mesh_emu_spv.h"

static struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule mod[3];
    VkPipeline pipe[3];
} g_mxe_vk;

static int mxe_vk_ready(void) {
    return g_mxe_vk.pipe[0] && g_mxe_vk.pipe[1] && g_mxe_vk.pipe[2];
}

static void mxe_vk_close(void) {
    for (int i = 0; i < 3; i++) {
        if (g_mxe_vk.pipe[i]) vkDestroyPipeline(g.d, g_mxe_vk.pipe[i], NULL);
        if (g_mxe_vk.mod[i]) vkDestroyShaderModule(g.d, g_mxe_vk.mod[i], NULL);
    }
    if (g_mxe_vk.layout) vkDestroyPipelineLayout(g.d, g_mxe_vk.layout, NULL);
    if (g_mxe_vk.dsl) vkDestroyDescriptorSetLayout(g.d, g_mxe_vk.dsl, NULL);
    memset(&g_mxe_vk, 0, sizeof g_mxe_vk);
}

static int mxe_vk_open(void) {
    const VkPhysicalDeviceLimits *l = &g.dev->properties.limits;
    if (l->maxComputeWorkGroupInvocations < 128u || l->maxComputeWorkGroupSize[0] < 128u ||
        l->maxPushConstantsSize < 4u)
        return 0;
    VkDescriptorSetLayoutBinding b[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b };
    if (vkCreateDescriptorSetLayout(g.d, &di, NULL, &g_mxe_vk.dsl) != VK_SUCCESS) goto bad;
    VkPushConstantRange pr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 4 };
    VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &g_mxe_vk.dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pr,
    };
    if (vkCreatePipelineLayout(g.d, &li, NULL, &g_mxe_vk.layout) != VK_SUCCESS) goto bad;
    const uint32_t *const code[3] = { nvk_mesh_emu_tclamp_spv, nvk_mesh_emu_scan_spv, nvk_mesh_emu_monly_spv };
    const size_t bytes[3] = { sizeof nvk_mesh_emu_tclamp_spv, sizeof nvk_mesh_emu_scan_spv, sizeof nvk_mesh_emu_monly_spv };
    for (int i = 0; i < 3; i++) {
        VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes[i], .pCode = code[i] };
        if (vkCreateShaderModule(g.d, &mi, NULL, &g_mxe_vk.mod[i]) != VK_SUCCESS) goto bad;
        VkComputePipelineCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                       .module = g_mxe_vk.mod[i], .pName = "main" },
            .layout = g_mxe_vk.layout,
        };
        if (vkCreateComputePipelines(g.d, VK_NULL_HANDLE, 1, &pi, NULL, &g_mxe_vk.pipe[i]) != VK_SUCCESS) goto bad;
    }
    return 1;
bad:
    mxe_vk_close();
    return 0;
}

void nvn_gpu_mesh_emu_op(nvn_gpu_cmd ch, uint32_t op, nvn_gpu_buf exchange, uint64_t xoff, uint64_t xbytes,
                         nvn_gpu_buf ind, uint64_t ind_off) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "mesh_emu_op 在通道内"); return; }
    if (!(g.caps.caps & NVN_GPU_CAP_MESH_EMU)) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "mesh_emu_op：没置 NVN_GPU_CAP_MESH_EMU"); return; }
    if (op > NVN_GPU_MESH_EMU_MONLY) { set_sticky(c, NVN_GPU_E_ARG, "mesh_emu_op 未知操作码 %u", op); return; }
    if (!g_mxe_vk.pipe[op]) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "mesh_emu_op：辅助计算管线未就绪"); return; }
    uint64_t align = g.dev->properties.limits.minStorageBufferOffsetAlignment;
    if (align < 4u) align = 4u;
    if (!exchange || xbytes < (uint64_t)NVN_GPU_MESH_EMU_HEAD * 4u || (xbytes & 3u) ||
        (xoff % align) != 0 || !buf_range(exchange, xoff, xbytes) ||
        (g.caps.max_ssbo_range && xbytes > g.caps.max_ssbo_range)) {
        set_sticky(c, NVN_GPU_E_ARG, "mesh_emu_op 交换缓冲范围 / 对齐非法（off=%llu len=%llu）",
                   (unsigned long long)xoff, (unsigned long long)xbytes);
        return;
    }
    if ((op == NVN_GPU_MESH_EMU_TCLAMP || op == NVN_GPU_MESH_EMU_MONLY) && !ind) {
        set_sticky(c, NVN_GPU_E_ARG, "mesh_emu_op 操作 %u 需要间接参数缓冲", op);
        return;
    }
    uint64_t ib_off = xoff, ib_len = xbytes;
    uint32_t ind_word_off = 6u;   // 无间接缓冲（直接绘制的 SCAN）时指回 h[6]：即使 h[6]==0 也读作 0，不会误读 h[0]
    nvn_gpu_buf ibuf = exchange;
    if (ind) {
        if ((ind_off & 3u) || !buf_range(ind, ind_off, 8u)) {
            set_sticky(c, NVN_GPU_E_ARG, "mesh_emu_op 间接参数越界或未 4 字节对齐（off=%llu）", (unsigned long long)ind_off);
            return;
        }
        ib_off = (ind_off / align) * align;
        uint64_t delta = ind_off - ib_off;
        if (delta > UINT32_MAX - 8u || (delta & 3u)) {
            set_sticky(c, NVN_GPU_E_ARG, "mesh_emu_op 间接参数对齐偏移非法");
            return;
        }
        ind_word_off = (uint32_t)(delta >> 2);
        ib_len = delta + 8u;
        if (g.caps.max_ssbo_range && ib_len > g.caps.max_ssbo_range) {
            set_sticky(c, NVN_GPU_E_ARG, "mesh_emu_op 间接参数窗口超出 maxStorageBufferRange");
            return;
        }
        ibuf = ind;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!alloc_sets(c, &g_mxe_vk.dsl, 1, &set, NULL, 0, 0)) {
        if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_OOM, "mesh_emu_op 描述符组分配失败");
        return;
    }
    cmd_ref(c, exchange, REF_BUF);
    if (ind) cmd_ref(c, ind, REF_BUF);
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    VkDescriptorBufferInfo bi[2] = {
        { exchange->buffer, xoff, xbytes },
        { ibuf->buffer, ib_off, ib_len },
    };
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] },
    };
    vkUpdateDescriptorSets(g.d, 2, wr, 0, NULL);
    // 先保证此前主机写 / 传输写（如间接参数拷贝）/ 任务计算写对本次辅助核可见
    mem_barrier(c,
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mxe_vk.pipe[op]);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mxe_vk.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(c->cb, g_mxe_vk.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &ind_word_off);
    vkCmdDispatch(c->cb, 1, 1, 1);
    // 辅助核写出的间接派发参数、排表与索引绘制头对后续间接派发与计算阶段可见
    mem_barrier(c,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    es_reset(c);
    c->desc_layout = VK_NULL_HANDLE;
    c->desc_dirty = 0xfu;
}
