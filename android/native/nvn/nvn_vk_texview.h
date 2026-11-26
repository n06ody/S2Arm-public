// NVNtextureView → Vulkan 子资源视图的纯决策（不调用 Vulkan，宿主可直接测）。
//
// NVN 的视图层号在 3D 纹理上表示深度切片（Apple 路线 nvn_mtl.m 的 attach()：
// target == 3D 时 min_layer 落到 MTLRenderPassAttachmentDescriptor.depthPlane）。
// Vulkan 的 3D 图像不能建 baseArrayLayer != 0 的 3D 视图，也不能把 3D 视图当
// framebuffer 附件；单切片用 2D 视图，多切片用 2D_ARRAY 视图，
// 都需要 VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT。切片范围按所选 mip 验证。
//
// 这里把“请求范围是否越界 / 视图类型 / 实际 base/n 范围”集中成一条可测规则：
//   - 越界仍准确失败（不钳位），错误里带目标、深度、层数；
//   - 2D array / cube 的单层视图给出 2D/2D_ARRAY 视图（cube 单面必须 2D）；
//   - 3D 的 3D 采样视图按 Vulkan 约束固定为 baseArrayLayer=0/layerCount=1，
//     只有覆盖全部深度时才允许（子范围没有等价的 3D 采样视图，明确失败）。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "nvn_obj.h"

struct nvn_vk_texview_plan {
    int ok;
    uint32_t base_level, nlevels, base_layer, nlayers;
    VkImageViewType type;
    char why[224];
};

// view 可为 NULL（未设视图 = 整幅）。attachment=1：framebuffer 附件视图。
// array_compatible=1：图像创建时带 VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT。
static inline void nvn_vk_texview_plan_make(uint32_t target, uint32_t depth, uint32_t layers, uint32_t levels,
                                            const view_t *v, int attachment, int array_compatible,
                                            struct nvn_vk_texview_plan *p) {
    memset(p, 0, sizeof *p);
    p->type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t bl = 0, nl = levels ? levels : 1, by = 0, ny = layers ? layers : 1;
    if (v && v->set_levels) { bl = v->base_level; nl = v->nlevels ? v->nlevels : 1; }
    if (v && v->set_layers) { by = v->min_layer; ny = v->nlayers ? v->nlayers : 1; }
    // 附件默认视图（未显式设置范围）在 NVN 里落到 mip 0 / layer 0（nvn_mtl.m 的 attach():
    // has_v && set_levels/set_layers 才写 level/slice，否则用描述符默认值）。
    if (attachment && !(v && v->set_levels)) { bl = 0; nl = 1; }
    if (attachment && !(v && v->set_layers)) { by = 0; ny = 1; }
    const int image3d = target == NVN_TEXTURE_TARGET_3D;
    const int slices = image3d && (attachment || (v && v->set_target && v->target != NVN_TEXTURE_TARGET_3D));
    const uint32_t slice_depth = bl < 32 ? depth >> bl : 0;
    const uint32_t layer_cap = slices ? (slice_depth ? slice_depth : 1) :
                               image3d ? (depth ? depth : 1) : (layers ? layers : 1);
    p->base_level = bl; p->nlevels = nl; p->base_layer = by; p->nlayers = ny;
    if (!nl || !ny || bl + nl > (levels ? levels : 1) || by + ny > layer_cap) {
        snprintf(p->why, sizeof p->why,
                 "texture view subresource outside the image: mip %u+%u of %u, layer %u+%u of %u "
                 "(target=%u depth=%u levels=%u)",
                 bl, nl, levels, by, ny, layer_cap, target, depth, levels);
        return;
    }
    if (attachment && nl != 1) {
        snprintf(p->why, sizeof p->why,
                 "attachment view must select exactly one mip (got mip %u+%u, layer %u+%u)", bl, nl, by, ny);
        return;
    }
    VkImageViewType type;
    if (v && v->set_target && !attachment) {
        uint32_t tgt = v->target;
        switch (tgt) {
        case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_VIEW_TYPE_3D; break;
        case NVN_TEXTURE_TARGET_1D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: type = VK_IMAGE_VIEW_TYPE_CUBE; break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
        default: type = ny > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; break;
        }
        if ((type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            !(target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)) {
            snprintf(p->why, sizeof p->why, "cube view requested for a non-cube-compatible image");
            return;
        }
    } else {
        // 默认视图（含 attachment 的默认整幅）：按图像目标给类型。
        if (image3d) type = VK_IMAGE_VIEW_TYPE_3D;
        else if (target == NVN_TEXTURE_TARGET_CUBEMAP) type = VK_IMAGE_VIEW_TYPE_CUBE;
        else if (target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        else if (target == NVN_TEXTURE_TARGET_1D_ARRAY || target == NVN_TEXTURE_TARGET_2D_ARRAY ||
                 target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY || ny > 1) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else type = VK_IMAGE_VIEW_TYPE_2D;
    }
    if (attachment) {
        // 附件只能是 2D/2D_ARRAY：cube 各面、3D 各切片都用相同的连续层编号。
        if (image3d) {
            if (!array_compatible) {
                snprintf(p->why, sizeof p->why,
                         "3D texture attachment selects slice %u+%u but the image was created without "
                         "VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT (device does not advertise it)", by, ny);
                return;
            }
        }
        type = ny > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    } else if (image3d) {
        if (type == VK_IMAGE_VIEW_TYPE_3D) {
            // Vulkan：3D 视图必须 baseArrayLayer=0、layerCount=1。NVN/苹果路线对 3D 采样视图的
            // 切片数直接取 1；只有起点不是 0（真正选深度子范围）才拒绝，0+任意层数按整幅 3D 接受。
            if (v && v->set_layers && by != 0) {
                snprintf(p->why, sizeof p->why,
                         "3D texture sampling view starts at depth slice %u of %u; Vulkan 3D views must "
                         "start at slice 0 (request a 2D/2D_ARRAY view target for a depth sub-range)",
                         by, depth);
                return;
            }
            by = 0; ny = 1;
        } else if (!array_compatible) {
            snprintf(p->why, sizeof p->why,
                     "3D texture view type %d needs VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, which the "
                     "device does not advertise", (int)type);
            return;
        }
    }
    // 立方体存储是一组面：显式目标合法时保留，否则按面数配对；不扩展层范围。
    if (!attachment && (target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)) {
        int explicit_ok = v && v->set_target &&
            ((v->target == NVN_TEXTURE_TARGET_2D && ny == 1) ||
             v->target == NVN_TEXTURE_TARGET_2D_ARRAY ||
             (v->target == NVN_TEXTURE_TARGET_CUBEMAP && ny == 6) ||
             (v->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && ny >= 6 && ny % 6 == 0));
        if (explicit_ok) {
            if (v->target == NVN_TEXTURE_TARGET_2D) type = VK_IMAGE_VIEW_TYPE_2D;
        } else if (ny == 1) type = VK_IMAGE_VIEW_TYPE_2D;
        else if (ny == 6) type = VK_IMAGE_VIEW_TYPE_CUBE;
        else if (ny > 6 && ny % 6 == 0) type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        else type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    p->base_layer = by; p->nlayers = ny; p->type = type;
    p->ok = 1;
}
