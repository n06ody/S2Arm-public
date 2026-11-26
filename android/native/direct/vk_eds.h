// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <vulkan/vulkan.h>

struct s2a_vk_eds {
    PFN_vkCmdSetCullMode cull_mode;
    PFN_vkCmdSetFrontFace front_face;
    PFN_vkCmdSetPrimitiveTopology topology;
    PFN_vkCmdSetDepthTestEnable depth_test;
    PFN_vkCmdSetDepthWriteEnable depth_write;
    PFN_vkCmdSetDepthCompareOp depth_compare;
    PFN_vkCmdSetStencilTestEnable stencil_test;
    PFN_vkCmdSetStencilOp stencil_op;
    PFN_vkCmdSetDepthBiasEnable depth_bias;
    PFN_vkCmdSetPrimitiveRestartEnable restart;
    PFN_vkCmdSetRasterizerDiscardEnable discard;
};

struct nvk_eds_values {
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkPrimitiveTopology topology;
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_compare;
    VkBool32 stencil_test;
    VkStencilOpState stencil[2];
    VkBool32 depth_bias, restart, discard;
};

enum {
    EDS_CULL          = 1u << 0,
    EDS_FRONT         = 1u << 1,
    EDS_TOPOLOGY      = 1u << 2,
    EDS_DEPTH_TEST    = 1u << 3,
    EDS_DEPTH_WRITE   = 1u << 4,
    EDS_DEPTH_COMPARE = 1u << 5,
    EDS_STENCIL_TEST  = 1u << 6,
    EDS_STENCIL_FRONT = 1u << 7,
    EDS_STENCIL_BACK  = 1u << 8,
    EDS_BIAS          = 1u << 9,
    EDS_RESTART       = 1u << 10,
    EDS_DISCARD       = 1u << 11,
    EDS_ALL           = (1u << 12) - 1,
};
