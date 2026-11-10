// 见 nvn_vk_mrt.h。纯逻辑，不创建 Vulkan 对象；宿主与真机走同一份实现。
#include "nvn_vk_mrt.h"

#include <stdio.h>
#include <string.h>

#include "nvn_obj.h"
#include "nvn_vk_format.h"

static int fmt_is_integer(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32:
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_A2R10G10B10_SINT_PACK32:
            return 1;
        default:
            return 0;
    }
}

int nvn_vk_mrt_format_is_color(VkFormat f) {
    if (f == VK_FORMAT_UNDEFINED) return 0;
    if (nvn_vk_format_is_depth(f)) return 0;
    if (nvn_vk_format_is_compressed(f)) return 0;
    return 1;
}

VkBlendFactor nvn_vk_mrt_blend_factor(uint32_t f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp nvn_vk_mrt_blend_op(uint32_t e) {
    switch (e) {
        case 2: return VK_BLEND_OP_SUBTRACT;
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;
        case 5: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

static void fail_why(char *why, size_t why_size, const char *text) {
    if (why && why_size) snprintf(why, why_size, "%s", text);
}

int nvn_vk_mrt_plan_build(const struct nvn_vk_mrt_slot slots[NVN_VK_MRT_MAX_COLOR], uint32_t rt_count,
                          int has_depth, uint32_t depth_width, uint32_t depth_height,
                          struct nvn_vk_mrt_plan *plan, char *why, size_t why_size) {
    if (!slots || !plan) { fail_why(why, why_size, "MRT plan: missing slot/plan storage"); return -1; }
    if (rt_count > NVN_VK_MRT_MAX_COLOR) { fail_why(why, why_size, "MRT plan: more than 8 colour slots"); return -1; }
    memset(plan, 0, sizeof *plan);
    for (uint32_t i = 0; i < NVN_VK_MRT_MAX_COLOR; i++) plan->color_att[i] = VK_ATTACHMENT_UNUSED;
    plan->depth_att = VK_ATTACHMENT_UNUSED;
    plan->first_slot = UINT32_MAX;

    uint8_t keep[NVN_VK_MRT_MAX_COLOR] = {0};
    uint32_t highest_bound = 0;
    int any_bound = 0;

    for (uint32_t i = 0; i < rt_count; i++) {
        if (!slots[i].bound) continue;
        any_bound = 1;
        highest_bound = i;
        // 重复槽：与 Metal rt_dup_slot 同口径（serial/image + level + layer）。
        // 写掩码为 0 的重复槽不产生写入，可以安全丢弃；非 0 则丢弃会真的丢写，准确报红。
        int dup = 0;
        for (uint32_t j = 0; j < i; j++) {
            if (!slots[j].bound || !keep[j]) continue;
            if (!slots[i].image_id || !slots[j].image_id) continue;
            if (slots[i].image_id == slots[j].image_id && slots[i].level == slots[j].level &&
                slots[i].layer == slots[j].layer) { dup = 1; break; }
        }
        if (dup) {
            if (slots[i].write_mask != 0) {
                char msg[200];
                snprintf(msg, sizeof msg,
                         "colour slot %u repeats an earlier image/level/layer with write mask 0x%x; "
                         "attaching it twice is not supported and dropping it would lose writes",
                         i, slots[i].write_mask);
                fail_why(why, why_size, msg);
                return -1;
            }
            plan->dropped_mask |= 1u << i;
            continue;
        }
        if (!nvn_vk_mrt_format_is_color(slots[i].format)) {
            char msg[160];
            snprintf(msg, sizeof msg, "colour slot %u format %d cannot be a colour attachment",
                     i, (int)slots[i].format);
            fail_why(why, why_size, msg);
            return -1;
        }
        if (!slots[i].width || !slots[i].height) {
            char msg[160];
            snprintf(msg, sizeof msg, "colour slot %u has a zero extent", i);
            fail_why(why, why_size, msg);
            return -1;
        }
        keep[i] = 1;
        plan->bound_mask |= 1u << i;
        if (plan->first_slot == UINT32_MAX) {
            plan->first_slot = i;
            plan->width = slots[i].width;
            plan->height = slots[i].height;
        }
    }

    if (!any_bound && !has_depth) {
        fail_why(why, why_size, "draw has neither a colour nor a depth attachment");
        return -1;
    }

    if (plan->first_slot == UINT32_MAX) {
        // 没有参与颜色槽：depth-only pass，尺寸由深度决定。
        if (!depth_width || !depth_height) {
            fail_why(why, why_size, "depth target has a zero extent");
            return -1;
        }
        plan->color_count = 0;
        plan->width = depth_width;
        plan->height = depth_height;
        plan->depth_used = 1;
        plan->depth_att = 0;
        plan->attachment_count = 1;
        return 0;
    }

    plan->color_count = highest_bound + 1;
    for (uint32_t i = 0; i < plan->color_count; i++) {
        if (!keep[i]) continue;
        if (slots[i].width != plan->width || slots[i].height != plan->height) {
            char msg[220];
            snprintf(msg, sizeof msg,
                     "colour slot %u is %ux%u but slot %u is %ux%u; one render pass cannot mix extents",
                     i, slots[i].width, slots[i].height, plan->first_slot, plan->width, plan->height);
            fail_why(why, why_size, msg);
            return -1;
        }
    }

    uint32_t natt = 0;
    for (uint32_t i = 0; i < plan->color_count; i++) {
        if (!keep[i]) { plan->color_att[i] = VK_ATTACHMENT_UNUSED; continue; }
        plan->color_att[i] = natt++;
    }
    if (has_depth) {
        // 深度尺寸不一致：沿用现有“本 pass 丢掉深度”的保守行为（Adreno 在非法 framebuffer
        // 的 vkCmdBeginRenderPass 里会直接 SIGSEGV）。调用方应打一行说明，不静默当成功。
        if (depth_width == plan->width && depth_height == plan->height) {
            plan->depth_used = 1;
            plan->depth_att = natt++;
        } else {
            plan->depth_used = 0;
            plan->depth_att = VK_ATTACHMENT_UNUSED;
        }
    }
    plan->attachment_count = natt;
    return 0;
}

void nvn_vk_mrt_color_blend(uint32_t slot, int slot_bound, VkFormat format,
                            const struct nvn_vk_mrt_blend_input *in,
                            VkPipelineColorBlendAttachmentState *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->colorWriteMask = 0;
    out->blendEnable = VK_FALSE;
    out->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    out->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    out->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    out->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    out->colorBlendOp = VK_BLEND_OP_ADD;
    out->alphaBlendOp = VK_BLEND_OP_ADD;
    if (!in || slot >= NVN_VK_MRT_MAX_COLOR) return;

    // 未绑状态按 NVN 新命令缓冲默认值：chmask 未绑时 mask=0xffffffff 全开
    // （nvnChannelMaskStateSetDefaults）；零值会让 colorWriteMask=0 整槽不写（#1409）。
    color_st color = { .blend_en = 0, .logic_op = NVN_LOGIC_OP_COPY,
                       .alpha_func = NVN_ALPHA_FUNC_ALWAYS };
    chmask_st chmask = { .mask = 0xffffffffu };
    if (in->state_seen & (1u << 1)) memcpy(&color, in->color_obj, sizeof color);
    if (in->state_seen & (1u << 2)) memcpy(&chmask, in->chmask_obj, sizeof chmask);

    // 未参与槽：附件是 VK_ATTACHMENT_UNUSED，写掩码 0、混合关。
    if (!slot_bound) return;

    uint32_t m = (chmask.mask >> (4u * slot)) & 0xfu;
    out->colorWriteMask = (m & 1u ? VK_COLOR_COMPONENT_R_BIT : 0) |
                          (m & 2u ? VK_COLOR_COMPONENT_G_BIT : 0) |
                          (m & 4u ? VK_COLOR_COMPONENT_B_BIT : 0) |
                          (m & 8u ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (!m) return;   // 整槽不写：不必开混合（结果同样被丢弃）

    blend_st blend;
    memset(&blend, 0, sizeof blend);
    blend.src_rgb = 2; blend.dst_rgb = 1; blend.src_a = 2; blend.dst_a = 1;   // ONE / ZERO
    blend.eq_rgb = 1; blend.eq_a = 1;                                          // ADD
    if (in->blend_seen & (1u << slot)) memcpy(&blend, in->blend_obj[slot], sizeof blend);

    out->srcColorBlendFactor = nvn_vk_mrt_blend_factor(blend.src_rgb);
    out->dstColorBlendFactor = nvn_vk_mrt_blend_factor(blend.dst_rgb);
    out->srcAlphaBlendFactor = nvn_vk_mrt_blend_factor(blend.src_a);
    out->dstAlphaBlendFactor = nvn_vk_mrt_blend_factor(blend.dst_a);
    out->colorBlendOp = nvn_vk_mrt_blend_op(blend.eq_rgb);
    out->alphaBlendOp = nvn_vk_mrt_blend_op(blend.eq_a);

    // NVNcolorState.blend_en 是按目标一位；整数目标的混合必须关（Vulkan 合同）。
    if (!(color.blend_en & (1u << slot)) || fmt_is_integer(format)) return;
    out->blendEnable = VK_TRUE;

    // 数学上等价于“不混合”的 ADD/ONE/ZERO 配置：结果逐像素相同，关掉省 dst 读+混合。
    if (blend.eq_rgb == 1u && blend.eq_a == 1u && blend.src_rgb == 2u && blend.dst_rgb == 1u &&
        blend.src_a == 2u && blend.dst_a == 1u)
        out->blendEnable = VK_FALSE;
}

// ---- NVN size=0（无边界）绑定：用 guest 池窗口求可证明区间 ----
// 前端把每个池映射进 GPU 地址窗口（hle_nvn.c），快照是 {GPU 起点, CPU 起点, 字节数} 三元组。
uint64_t nvn_vk_guest_buffer_extent(uint64_t guest_addr, uint64_t cap, int *exact) {
    // 第一优先：地址所在活 nvnBuffer 的精确长度（BufferInitialize 登记，Finalize 注销）。
    // 弱链接[androiddraw]：nvn_buf_extent（前端 graphics/nvn/gfx/nvn/hle_nvn.c 提供；宿主单测只链 mrt.c，不链前端）
    // 弱链接[androiddraw]：nvn_gva_pool_of（同上；GPU 地址 → 池的 CPU 范围）
    extern int nvn_buf_extent(uint64_t cpu_addr, uint64_t *base, uint64_t *size) __attribute__((weak));
    if (exact) *exact = 0;
    if (!guest_addr || !cap) return 0;
    uint64_t base = 0, bytes = 0;
    if (nvn_buf_extent && nvn_buf_extent(guest_addr, &base, &bytes) && bytes && guest_addr >= base) {
        uint64_t room = bytes - (guest_addr - base);
        if (exact) *exact = 1;
        return room > cap ? cap : room;
    }
    // 退路：所在池的池尾（可证明“已映射”，但不是分配边界）。
    // 弱引用：宿主单测没有前端时返回 0 → 调用方准确失败。必须用单点查询，不能用快照：
    // 快照有 cap 截断（0100F2C0115B6000/01007EF00011E000 活跃池可达上千），命中池排在 cap 之后会被误判成“不可证明”。
    extern int nvn_gva_pool_of(uint64_t cpu_addr, uint64_t *cpu_base, uint64_t *bytes) __attribute__((weak));
    if (!nvn_gva_pool_of || !nvn_gva_pool_of(guest_addr, &base, &bytes) || !bytes || guest_addr < base) return 0;
    uint64_t room = bytes - (guest_addr - base);
    return room > cap ? cap : room;
}

uint64_t nvn_vk_guest_pool_extent(uint64_t guest_addr, uint64_t cap) {
    return nvn_vk_guest_buffer_extent(guest_addr, cap, NULL);
}

// ---- 图形驱动常量段 c0 的合成（纯逻辑，宿主可测） ----
int nvn_vk_driver_c0_base(uint32_t stage_idx) {
    switch (stage_idx) {
        case 0: return 0x110;   // vs
        case 1: return 0x510;   // fs
        case 2: return 0x410;   // gs
        case 3: return 0x210;   // tcs
        case 4: return 0x310;   // tes
        default: return -1;
    }
}

int nvn_vk_driver_c0_fill_graphics(uint8_t *dst, size_t dst_size, uint32_t stage_idx,
                                   const uint64_t ssbo_addr[16], const uint32_t ssbo_size[16],
                                   uint32_t base_instance, char *why, size_t why_size) {
    if (!dst || dst_size < NVN_VK_DRIVER_C0_BYTES || !ssbo_addr || !ssbo_size) {
        fail_why(why, why_size, "driver c0: buffer smaller than the measured layout or missing tables");
        return -1;
    }
    const int base = nvn_vk_driver_c0_base(stage_idx);
    if (base < 0) {
        char msg[128];
        snprintf(msg, sizeof msg, "driver c0: stage %u has no measured storage-buffer table base", stage_idx);
        fail_why(why, why_size, msg);
        return -1;
    }
    memset(dst, 0, NVN_VK_DRIVER_C0_BYTES);
    for (int r = 0; r < 16; r++) {
        if (!ssbo_addr[r]) continue;
        const uint64_t a = ssbo_addr[r];
        const uint32_t n = ssbo_size[r];
        memcpy(dst + base + 16 * r, &a, sizeof a);
        memcpy(dst + base + 16 * r + 8, &n, sizeof n);
    }
    const float max_point = NVN_VK_DRIVER_C0_MAX_POINT;
    memcpy(dst + 0x610, &max_point, sizeof max_point);
    memcpy(dst + 0x644, &base_instance, sizeof base_instance);
    return 0;
}

int nvn_vk_feedback_src_plan(uint32_t target, uint32_t depth, uint32_t levels, uint32_t layers,
                             uint32_t view_level, uint32_t view_layer, uint32_t view_layers,
                             uint32_t w, uint32_t h,
                             struct nvn_vk_feedback_src *out, char *why, size_t why_size) {
    if (!out) { fail_why(why, why_size, "feedback snapshot: missing plan storage"); return -1; }
    memset(out, 0, sizeof *out);
    if (!w || !h) { fail_why(why, why_size, "feedback snapshot: zero extent"); return -1; }
    if (!levels || !layers) { fail_why(why, why_size, "feedback snapshot: image has no subresources"); return -1; }
    if (view_level >= levels) {
        char msg[192];
        snprintf(msg, sizeof msg, "feedback snapshot: level %u outside 0..%u", view_level, levels - 1);
        fail_why(why, why_size, msg);
        return -1;
    }
    // 3D 底图的 cview 层号是深度切片（nvn_vk_texview.h 的 attachment 口径）：层数上限是 depth，
    // 不是 arrayLayers（3D 的 arrayLayers 恒为 1）。这里不把它当越界，也不退回 layer 0。
    const int is_3d = target == NVN_TEXTURE_TARGET_3D && depth > 1;
    const uint32_t layer_cap = is_3d ? depth : (layers ? layers : 1);
    if (view_layer >= layer_cap) {
        char msg[224];
        snprintf(msg, sizeof msg, "feedback snapshot: %s %u outside 0..%u (target=%u depth=%u layers=%u)",
                 is_3d ? "depth slice" : "layer", view_layer, layer_cap - 1, target, depth, layers);
        fail_why(why, why_size, msg);
        return -1;
    }
    if (view_layers != 1) {
        char msg[192];
        snprintf(msg, sizeof msg, "feedback snapshot: view covers %u layers; only a single slice is supported",
                 view_layers);
        fail_why(why, why_size, msg);
        return -1;
    }
    out->level = view_level;
    out->is_3d = (uint8_t)is_3d;
    if (is_3d) { out->slice = view_layer; out->layer = 0; }
    else { out->layer = view_layer; out->slice = 0; }
    out->layers = 1;
    return 0;
}

// 单子资源 → 2D 快照的拷贝区域（纯构造，不调 Vulkan）。extent 取 pass 与快照的交集，
// 因为快照按“本笔 framebuffer 的尺寸”建、而源子资源可能更大（历史行为，已有一条诊断日志）。
int nvn_vk_feedback_copy_make(const struct nvn_vk_feedback_src *src, uint32_t pass_w, uint32_t pass_h,
                              uint32_t snap_w, uint32_t snap_h, VkImageCopy *out,
                              char *why, size_t why_size) {
    if (!src || !out) { fail_why(why, why_size, "feedback copy: missing src region"); return -1; }
    memset(out, 0, sizeof *out);
    if (!pass_w || !pass_h || !snap_w || !snap_h) { fail_why(why, why_size, "feedback copy: zero extent"); return -1; }
    const uint32_t cw = pass_w < snap_w ? pass_w : snap_w;
    const uint32_t ch = pass_h < snap_h ? pass_h : snap_h;
    if (!cw || !ch) { fail_why(why, why_size, "feedback copy: pass and snapshot do not intersect"); return -1; }
    if (src->is_3d && src->slice == UINT32_MAX) { fail_why(why, why_size, "feedback copy: 3D source has no depth slice"); return -1; }
    // 3D 源：Vulkan 要求 3D 子资源 layerCount=1，切片由 srcOffset.z 选；2D/数组源才用 baseArrayLayer。
    out->srcSubresource = (VkImageSubresourceLayers){
        VK_IMAGE_ASPECT_COLOR_BIT, src->level, src->is_3d ? 0u : src->layer, 1 };
    out->srcOffset = (VkOffset3D){ 0, 0, src->is_3d ? (int32_t)src->slice : 0 };
    out->dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    out->dstOffset = (VkOffset3D){ 0, 0, 0 };
    out->extent = (VkExtent3D){ cw, ch, 1 };
    return 0;
}

// 采样绑定替换判定。返回 NONE（不是本子资源的反馈：不替换、不报）、SUBSTITUTE（换单层快照）、
// ISO（换同构快照的等形视图：默认视图落在多级/多层/3D 底图，或注册视图覆盖多个子资源）、
// LEAVE（是反馈但快照路表达不了：保持原绑定，调用方记一次原因，不静默假装成功）。
int nvn_vk_feedback_bind_plan(const struct nvn_vk_feedback_bind *in, char *why, size_t why_size) {
    if (!in) return NVN_VK_FB_BIND_NONE;
    if (!in->view_override) {
        // 默认整幅视图覆盖底图全部子资源。单层 2D 快照只能代表“单级单层 2D 底图”；
        // 多级/多层/3D 走同构快照（换一张同形图 + 等形视图，不是换错视图形状）。
        if (in->fb_is_3d || in->tex_depth > 1 || in->tex_layers > 1 || in->tex_levels > 1)
            return NVN_VK_FB_BIND_ISO;
        return NVN_VK_FB_BIND_SUBSTITUTE;
    }
    if (!in->view_ok) {
        fail_why(why, why_size, "feedback snapshot: the sampled view cannot be created under the Vulkan view rules");
        return NVN_VK_FB_BIND_LEAVE;
    }
    if (!in->override_ok) {
        fail_why(why, why_size, "feedback snapshot: the sampled view has a format/swizzle/depth-stencil override; the snapshot default view does not express it");
        return NVN_VK_FB_BIND_LEAVE;
    }
    // 采样视图不含附件那个子资源（只读更早的 mip / 其它层/切片）：不是本子资源的反馈，
    // 合法，保持原绑定也不报。3D 视图类型覆盖全部深度切片；2D 类视图（含 3D 底图上的
    // 单切片形态）按层号范围查。
    const int covers_level = in->view_base_level <= in->fb_level &&
                             in->fb_level < in->view_base_level + in->view_nlevels;
    const int covers_layer = in->view_type == VK_IMAGE_VIEW_TYPE_3D ||
                             (in->view_base_layer <= in->fb_layer &&
                              in->fb_layer < in->view_base_layer + in->view_nlayers);
    if (!covers_level || !covers_layer) return NVN_VK_FB_BIND_NONE;
    // 视图恰好是附件那一个子资源：单层 2D 快照就能代表。
    if (in->view_type == VK_IMAGE_VIEW_TYPE_2D && in->view_nlevels == 1 && in->view_nlayers == 1)
        return NVN_VK_FB_BIND_SUBSTITUTE;
    // 覆盖更多子资源（多级/多层/非 2D 形）：同构快照按同一视图参数等形替换。
    return NVN_VK_FB_BIND_ISO;
}

int nvn_vk_feedback_view_multi(const struct nvn_vk_feedback_bind *in) {
    if (!in) return 0;
    if (!in->view_override)
        return in->fb_is_3d || in->tex_depth > 1 || in->tex_layers > 1 || in->tex_levels > 1;
    // 注册视图：建不出或覆盖非单子资源都按多算——快照替不了它，直通与旧 LEAVE 同结果。
    if (!in->view_ok) return 1;
    return !(in->view_type == VK_IMAGE_VIEW_TYPE_2D && in->view_nlevels == 1 && in->view_nlayers == 1);
}

static uint32_t iso_mip_extent(uint32_t v, uint32_t level) {
    uint32_t e = v >> level;
    return e ? e : 1;
}

int nvn_vk_feedback_iso_plan(uint32_t is_3d, uint32_t w, uint32_t h, uint32_t depth,
                             uint32_t levels, uint32_t layers, uint32_t bytes_per_texel,
                             uint64_t cap_bytes,
                             struct nvn_vk_feedback_iso *out, char *why, size_t why_size) {
    if (!out || !w || !h || !levels || !bytes_per_texel || (!is_3d && !layers) || (is_3d && !depth)) {
        fail_why(why, why_size, "iso snapshot plan: incomplete shape");
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->is_3d = (uint8_t)(is_3d ? 1 : 0);
    out->w = w; out->h = h; out->depth = is_3d ? depth : 1;
    out->levels = levels; out->layers = is_3d ? 1 : layers;
    uint64_t bytes = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint64_t a = (uint64_t)iso_mip_extent(w, l) * iso_mip_extent(h, l) *
                     (is_3d ? iso_mip_extent(depth, l) : 1);
        bytes += a * out->layers * bytes_per_texel;
    }
    out->est_bytes = bytes;
    out->n_copies = levels * (is_3d ? 1 : layers);
    if (cap_bytes && bytes > cap_bytes) {
        char msg[192];
        snprintf(msg, sizeof msg,
                 "feedback iso snapshot: sampled view is too large to copy whole (%.1f MB > %.1f MB)",
                 (double)bytes / 1048576.0, (double)cap_bytes / 1048576.0);
        fail_why(why, why_size, msg);
        return -1;
    }
    return 0;
}

int nvn_vk_feedback_iso_copy(const struct nvn_vk_feedback_iso *iso, uint32_t n,
                             VkImageCopy *out, char *why, size_t why_size) {
    if (!iso || !out || n >= iso->n_copies) {
        fail_why(why, why_size, "iso snapshot copy: bad index");
        return -1;
    }
    memset(out, 0, sizeof *out);
    const uint32_t level = iso->is_3d ? n : n / (iso->layers ? iso->layers : 1);
    const uint32_t layer = iso->is_3d ? 0 : n % (iso->layers ? iso->layers : 1);
    if (level >= iso->levels) {
        fail_why(why, why_size, "iso snapshot copy: level out of range");
        return -1;
    }
    out->srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
    out->dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
    out->extent = (VkExtent3D){ iso_mip_extent(iso->w, level), iso_mip_extent(iso->h, level),
                                iso->is_3d ? iso_mip_extent(iso->depth, level) : 1 };
    return 0;
}

// ---- BC/块压缩纹理作为渲染目标（块=像素的 UI 视图）的判定与拷贝区域 ----
// BC1/BC4 一块 8 字节，BC2/BC3/BC5/BC6H/BC7 一块 16 字节；块边长恒 4。
static uint32_t bcrt_block_bytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
            return 8;
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
            return 16;
        default:
            return 0;
    }
}
// 无压缩颜色视图的每像素字节数（只认 8 / 16 字节这两类块编码覆盖）。
static uint32_t bcrt_texel_bytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_USCALED: case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
            return (f == VK_FORMAT_R32G32B32A32_UINT || f == VK_FORMAT_R32G32B32A32_SINT ||
                    f == VK_FORMAT_R32G32B32A32_SFLOAT) ? 16 : 8;
        default:
            return 0;
    }
}

int nvn_vk_bcrt_view_format(VkFormat base_vk_format, VkFormat view_vk_format, VkFormat *out,
                            char *why, size_t why_size) {
    if (out) *out = VK_FORMAT_UNDEFINED;
    const uint32_t bb = bcrt_block_bytes(base_vk_format);
    if (!bb) {
        char msg[192];
        snprintf(msg, sizeof msg, "bcrt: base format %d is not a BC compressed format", (int)base_vk_format);
        fail_why(why, why_size, msg);
        return -1;
    }
    if (nvn_vk_format_is_compressed(view_vk_format) || nvn_vk_format_is_depth(view_vk_format)) {
        char msg[192];
        snprintf(msg, sizeof msg, "bcrt: view format %d is compressed or depth; a block-encoding view must be uncompressed colour",
                 (int)view_vk_format);
        fail_why(why, why_size, msg);
        return -1;
    }
    const uint32_t pb = bcrt_texel_bytes(view_vk_format);
    if (!pb) {
        char msg[192];
        snprintf(msg, sizeof msg, "bcrt: view format %d is not an 8/16-byte uncompressed colour format", (int)view_vk_format);
        fail_why(why, why_size, msg);
        return -1;
    }
    if (pb != bb) {
        char msg[192];
        snprintf(msg, sizeof msg, "bcrt: view pixel is %u bytes but a %d block is %u bytes; the view cannot encode blocks",
                 pb, (int)base_vk_format, bb);
        fail_why(why, why_size, msg);
        return -1;
    }
    if (out) *out = view_vk_format;
    return 0;
}

int nvn_vk_bcrt_plan_make(VkFormat base_vk_format, VkFormat view_vk_format, uint32_t base_width,
                          uint32_t base_height, uint32_t level, uint32_t layer, uint8_t is_3d,
                          struct nvn_vk_bcrt_plan *out, char *why, size_t why_size) {
    if (!out) { fail_why(why, why_size, "bcrt: missing plan storage"); return -1; }
    memset(out, 0, sizeof *out);
    if (nvn_vk_bcrt_view_format(base_vk_format, view_vk_format, NULL, why, why_size) != 0) return -1;
    const uint32_t bb = bcrt_block_bytes(base_vk_format);
    if (!base_width || !base_height) { fail_why(why, why_size, "bcrt: zero base extent"); return -1; }
    out->block_bytes = bb;
    out->side = 4;
    out->width = base_width;
    out->height = base_height;
    out->bw = (base_width + 3) / 4;
    out->bh = (base_height + 3) / 4;
    out->level = level;
    out->layer = layer;
    out->is_3d = is_3d ? 1 : 0;
    return 0;
}

static void bcrt_buf_copy(VkBufferImageCopy *out, uint32_t w, uint32_t h, VkImageSubresourceLayers sub) {
    memset(out, 0, sizeof *out);
    out->bufferOffset = 0;
    out->bufferRowLength = 0;      // 紧打包：块行字节 = bw*block_bytes = 影子行字节
    out->bufferImageHeight = 0;
    out->imageSubresource = sub;
    out->imageOffset = (VkOffset3D){ 0, 0, 0 };
    out->imageExtent = (VkExtent3D){ w, h, 1 };
}
void nvn_vk_bcrt_img_to_buf(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out) {
    if (!p || !out) return;
    // 压缩底图：整 mip/平面，3D 用 imageOffset.z 选深度切片（layerCount 恒 1）。
    VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, p->level, p->is_3d ? 0u : p->layer, 1 };
    bcrt_buf_copy(out, p->width, p->height, sub);
    out->imageOffset.z = p->is_3d ? (int32_t)p->layer : 0;
}
void nvn_vk_bcrt_buf_to_shadow(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out) {
    if (!p || !out) return;
    VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bcrt_buf_copy(out, p->bw, p->bh, sub);
}
void nvn_vk_bcrt_shadow_to_buf(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out) {
    if (!p || !out) return;
    VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bcrt_buf_copy(out, p->bw, p->bh, sub);
}
void nvn_vk_bcrt_buf_to_img(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out) {
    if (!p || !out) return;
    VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, p->level, p->is_3d ? 0u : p->layer, 1 };
    bcrt_buf_copy(out, p->width, p->height, sub);
    out->imageOffset.z = p->is_3d ? (int32_t)p->layer : 0;
}
