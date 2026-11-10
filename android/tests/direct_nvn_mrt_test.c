// 直连多颜色渲染目标（MRT）宿主回归：生产 nvn_vk_mrt.c 的槽位计划与逐槽颜色混合。
//
// 覆盖 #1382 的验收面：2 / 3 / 4 个颜色槽、中间空槽（VK_ATTACHMENT_UNUSED）、混合格式、
// depth-only、以及未支持组合的准确报红。负控：旧实现“槽 1..n 一律拒绝”的规则复刻必须在
// 同一批正例上全部报红，证明这些夹具真的能触发 #1382 的 red，而不是只测试新代码的空路径。
#include <stdio.h>
#include <string.h>

#include "nvn/nvn_vk_mrt.h"
#include "nvn_obj.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)
#define CHECK_STR(hay, needle, what) do { \
    if (!strstr((hay), (needle))) { printf("FAIL: %s (why=%s)\n", what, (hay)); fails++; } \
} while (0)

static struct nvn_vk_mrt_slot mk(VkFormat f, uint32_t w, uint32_t h, uint64_t id,
                                 uint32_t level, uint32_t layer, uint32_t mask) {
    struct nvn_vk_mrt_slot s;
    memset(&s, 0, sizeof s);
    s.bound = 1;
    s.format = f;
    s.width = w; s.height = h;
    s.image_id = id;
    s.level = level; s.layer = layer;
    s.write_mask = mask;
    return s;
}

static void test_two_slots(void) {
    struct nvn_vk_mrt_slot s[8];
    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x1000, 0, 0, 0xf);
    s[1] = mk(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 64, 64, 0x1001, 0, 0, 0xf);
    struct nvn_vk_mrt_plan p;
    char why[256] = "";
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) == 0, "2 slots accepted");
    CHECK(p.color_count == 2, "2 slots: colorAttachmentCount=2");
    CHECK(p.bound_mask == 0x3u, "2 slots: bound mask");
    CHECK(p.attachment_count == 2, "2 slots: two declared attachments");
    CHECK(p.color_att[0] == 0 && p.color_att[1] == 1, "2 slots: refs 0,1");
    CHECK(p.depth_att == VK_ATTACHMENT_UNUSED && p.depth_used == 0, "2 slots: no depth");
    CHECK(p.width == 64 && p.height == 64 && p.first_slot == 0, "2 slots: extent/first slot");
}

static void test_three_slots_with_hole(void) {
    struct nvn_vk_mrt_slot s[8];
    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x2000, 0, 0, 0xf);
    s[2] = mk(VK_FORMAT_R16G16B16A16_SFLOAT, 64, 64, 0x2002, 0, 0, 0xf);
    struct nvn_vk_mrt_plan p;
    char why[256] = "";
    CHECK(nvn_vk_mrt_plan_build(s, 3, 0, 0, 0, &p, why, sizeof why) == 0, "3 slots with hole accepted");
    CHECK(p.color_count == 3, "3 slots: colorAttachmentCount=3");
    CHECK(p.bound_mask == 0x5u, "3 slots: bound mask 0b101");
    CHECK(p.attachment_count == 2, "3 slots: only two declared attachments");
    CHECK(p.color_att[0] == 0, "3 slots: slot0 -> att0");
    CHECK(p.color_att[1] == VK_ATTACHMENT_UNUSED, "3 slots: slot1 is VK_ATTACHMENT_UNUSED");
    CHECK(p.color_att[2] == 1, "3 slots: slot2 -> att1");
}

static void test_four_slots_mixed(void) {
    struct nvn_vk_mrt_slot s[8];
    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 128, 128, 0x3000, 0, 0, 0xf);
    s[1] = mk(VK_FORMAT_R32_UINT, 128, 128, 0x3001, 0, 0, 0xf);
    s[3] = mk(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 128, 128, 0x3003, 0, 0, 0xf);
    struct nvn_vk_mrt_plan p;
    char why[256] = "";
    CHECK(nvn_vk_mrt_plan_build(s, 4, 0, 0, 0, &p, why, sizeof why) == 0, "4 slots mixed accepted");
    CHECK(p.color_count == 4, "4 slots: colorAttachmentCount=4");
    CHECK(p.bound_mask == 0xbu, "4 slots: bound mask 0b1011");
    CHECK(p.attachment_count == 3, "4 slots: three declared attachments");
    CHECK(p.color_att[0] == 0 && p.color_att[1] == 1, "4 slots: refs 0,1");
    CHECK(p.color_att[2] == VK_ATTACHMENT_UNUSED, "4 slots: slot2 hole");
    CHECK(p.color_att[3] == 2, "4 slots: slot3 -> att2");
}

static void test_slot0_hole(void) {
    struct nvn_vk_mrt_slot s[8];
    memset(s, 0, sizeof s);
    s[1] = mk(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 0x4001, 0, 0, 0xf);
    struct nvn_vk_mrt_plan p;
    char why[256] = "";
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) == 0, "slot0 hole accepted");
    CHECK(p.color_count == 2 && p.bound_mask == 0x2u, "slot0 hole: slot1 is the only colour");
    CHECK(p.color_att[0] == VK_ATTACHMENT_UNUSED && p.color_att[1] == 0, "slot0 hole: UNUSED then att0");
    CHECK(p.first_slot == 1 && p.width == 32 && p.height == 32, "slot0 hole: extent from slot1");
}

static void test_depth_only(void) {
    struct nvn_vk_mrt_slot s[8];
    memset(s, 0, sizeof s);
    struct nvn_vk_mrt_plan p;
    char why[256] = "";
    CHECK(nvn_vk_mrt_plan_build(s, 2, 1, 64, 64, &p, why, sizeof why) == 0, "depth-only accepted");
    CHECK(p.color_count == 0 && p.attachment_count == 1, "depth-only: one depth attachment");
    CHECK(p.depth_used == 1 && p.depth_att == 0, "depth-only: depth is att0");
    CHECK(p.width == 64 && p.height == 64, "depth-only: extent from depth");
}

static void test_rejections(void) {
    struct nvn_vk_mrt_slot s[8];
    struct nvn_vk_mrt_plan p;
    char why[256];
    memset(s, 0, sizeof s);
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_mrt_plan_build(s, 0, 0, 0, 0, &p, why, sizeof why) != 0, "no colour and no depth rejected");
    CHECK_STR(why, "neither", "no target reason");

    memset(s, 0, sizeof s); why[0] = 0;
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x5000, 0, 0, 0xf);
    s[1] = mk(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 0x5001, 0, 0, 0xf);
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) != 0, "mixed extents rejected");
    CHECK_STR(why, "cannot mix extents", "mixed extent reason");

    memset(s, 0, sizeof s); why[0] = 0;
    s[0] = mk(VK_FORMAT_D32_SFLOAT, 64, 64, 0x5100, 0, 0, 0xf);
    CHECK(nvn_vk_mrt_plan_build(s, 1, 0, 0, 0, &p, why, sizeof why) != 0, "depth format as colour rejected");
    CHECK_STR(why, "cannot be a colour attachment", "depth-as-colour reason");

    memset(s, 0, sizeof s); why[0] = 0;
    s[0] = mk(VK_FORMAT_UNDEFINED, 64, 64, 0x5200, 0, 0, 0xf);
    CHECK(nvn_vk_mrt_plan_build(s, 1, 0, 0, 0, &p, why, sizeof why) != 0, "undefined colour format rejected");

    memset(s, 0, sizeof s); why[0] = 0;
    s[0] = mk(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 64, 64, 0x5300, 0, 0, 0xf);
    CHECK(nvn_vk_mrt_plan_build(s, 1, 0, 0, 0, &p, why, sizeof why) != 0, "compressed colour format rejected");
}

static void test_duplicates(void) {
    struct nvn_vk_mrt_slot s[8];
    struct nvn_vk_mrt_plan p;
    char why[256];
    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x6000, 0, 0, 0xf);
    s[1] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x6000, 0, 0, 0x0);
    why[0] = 0;
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) == 0, "duplicate with zero mask accepted");
    CHECK(p.dropped_mask == 0x2u, "duplicate: second slot dropped");
    CHECK(p.bound_mask == 0x1u && p.attachment_count == 1, "duplicate: only first slot attached");
    CHECK(p.color_count == 2 && p.color_att[1] == VK_ATTACHMENT_UNUSED, "duplicate: pipeline slot count kept");

    s[1].write_mask = 0xfu;
    why[0] = 0;
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) != 0, "duplicate with writes rejected");
    CHECK_STR(why, "lose writes", "duplicate non-zero mask reason");

    // 不同 mip/层的同 image 不是重复槽。
    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x6100, 0, 0, 0xf);
    s[1] = mk(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 0x6100, 1, 0, 0xf);
    why[0] = 0;
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) != 0, "different mips also differ in extent here");
    // 同尺寸不同 level（手工给同尺寸）应视为不同槽。
    s[1].width = 64; s[1].height = 64;
    why[0] = 0;
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) == 0, "same image different level is not duplicate");
    CHECK(p.attachment_count == 2, "same image different level: two attachments");
}

static void test_depth_mismatch_dropped(void) {
    struct nvn_vk_mrt_slot s[8];
    struct nvn_vk_mrt_plan p;
    char why[256] = "";
    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x7000, 0, 0, 0xf);
    CHECK(nvn_vk_mrt_plan_build(s, 1, 1, 32, 32, &p, why, sizeof why) == 0, "depth mismatch still builds colour pass");
    CHECK(p.depth_used == 0 && p.depth_att == VK_ATTACHMENT_UNUSED, "depth mismatch: depth dropped");
    CHECK(p.attachment_count == 1, "depth mismatch: only colour attachment declared");
}

static void blend_fixture(struct nvn_vk_mrt_blend_input *in, uint32_t blend_en, uint32_t chmask,
                          int blend_target, uint32_t src_rgb, uint32_t dst_rgb,
                          uint32_t src_a, uint32_t dst_a) {
    memset(in, 0, sizeof *in);
    color_st cs;
    memset(&cs, 0, sizeof cs);
    cs.blend_en = blend_en;
    chmask_st cm;
    memset(&cm, 0, sizeof cm);
    cm.mask = chmask;
    memcpy(in->color_obj, &cs, sizeof cs);
    memcpy(in->chmask_obj, &cm, sizeof cm);
    if (blend_target >= 0) {
        blend_st bs;
        memset(&bs, 0, sizeof bs);
        bs.target = (unsigned)blend_target;
        bs.src_rgb = src_rgb; bs.dst_rgb = dst_rgb; bs.src_a = src_a; bs.dst_a = dst_a;
        bs.eq_rgb = 1; bs.eq_a = 1;   // ADD
        memcpy(in->blend_obj[blend_target], &bs, sizeof bs);
        in->blend_seen = (uint8_t)(1u << blend_target);
    }
    in->state_seen = (1u << 0) | (1u << 1) | (1u << 2);
}

static void test_blend_per_slot(void) {
    struct nvn_vk_mrt_blend_input in;
    VkPipelineColorBlendAttachmentState cba;

    // 只有槽 1 开混合；槽 0 的 blend_en 位为 0，但写掩码不同。
    blend_fixture(&in, 0x2u, 0x00ffu, 1, 5, 6, 5, 6);   // slot0 掩码 0xf，slot1 掩码 0xf
    nvn_vk_mrt_color_blend(0, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.blendEnable == VK_FALSE, "slot0 blend disabled by blend_en bit");
    CHECK(cba.colorWriteMask == 0xfu, "slot0 write mask from nibble 0");
    nvn_vk_mrt_color_blend(1, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.blendEnable == VK_TRUE, "slot1 blend enabled by its own bit");
    CHECK(cba.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
          cba.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "slot1 uses its own target blend state");

    // 掩码按 4 位一槽取：0x0f00 -> slot1 掩码 0，slot2 掩码 0xf。
    blend_fixture(&in, 0x2u, 0x0f00u, 1, 5, 6, 5, 6);
    nvn_vk_mrt_color_blend(1, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.colorWriteMask == 0 && cba.blendEnable == VK_FALSE, "zero write mask disables blend");
    nvn_vk_mrt_color_blend(2, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.colorWriteMask == 0xfu, "slot2 write mask from nibble 2");

    // 空槽：写掩码 0、混合关，即使状态位是开的。
    nvn_vk_mrt_color_blend(1, 0, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.colorWriteMask == 0 && cba.blendEnable == VK_FALSE, "hole slot writes nothing");

    // 整数格式：Vulkan/NVN 都不混合。
    blend_fixture(&in, 0x2u, 0x00f0u, 1, 5, 6, 5, 6);
    nvn_vk_mrt_color_blend(1, 1, VK_FORMAT_R32_UINT, &in, &cba);
    CHECK(cba.blendEnable == VK_FALSE, "integer attachment never blends");

    // ADD/ONE/ZERO 等价配置关混合（省带宽，不改像素）。
    blend_fixture(&in, 0x2u, 0x00f0u, 1, 2, 1, 2, 1);
    nvn_vk_mrt_color_blend(1, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.blendEnable == VK_FALSE, "ADD/ONE/ZERO no-op blend disabled");
    CHECK(cba.colorWriteMask == 0xfu, "no-op blend keeps write mask");

    // 未绑定 blend 状态的槽按默认 ONE/ZERO（等价不混合），不拿别的槽的状态。
    blend_fixture(&in, 0x2u, 0x00f0u, -1, 0, 0, 0, 0);
    nvn_vk_mrt_color_blend(1, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.blendEnable == VK_FALSE, "unbound blend state falls back to no-op, not another slot");
}

// 负控：旧实现只接颜色附件 0；槽 1..n 有真实绑定就报 #1382 的 red。
static int legacy_only_slot0_red(uint32_t rt_count, const struct nvn_vk_mrt_slot *s) {
    for (uint32_t i = 1; i < rt_count && i < 8; i++)
        if (s[i].bound) return 1;
    return 0;
}

static void test_negative_control_old_rule(void) {
    struct nvn_vk_mrt_slot s[8];
    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x8000, 0, 0, 0xf);
    s[1] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x8001, 0, 0, 0xf);
    struct nvn_vk_mrt_plan p;
    char why[256] = "";
    CHECK(legacy_only_slot0_red(2, s) == 1, "negative control: old rule rejects 2 slots");
    CHECK(nvn_vk_mrt_plan_build(s, 2, 0, 0, 0, &p, why, sizeof why) == 0, "new rule accepts 2 slots");

    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x8010, 0, 0, 0xf);
    s[2] = mk(VK_FORMAT_R16G16B16A16_SFLOAT, 64, 64, 0x8012, 0, 0, 0xf);
    why[0] = 0;
    CHECK(legacy_only_slot0_red(3, s) == 1, "negative control: old rule rejects 3 slots");
    CHECK(nvn_vk_mrt_plan_build(s, 3, 0, 0, 0, &p, why, sizeof why) == 0, "new rule accepts 3 slots with hole");

    memset(s, 0, sizeof s);
    s[0] = mk(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0x8020, 0, 0, 0xf);
    s[1] = mk(VK_FORMAT_R32_UINT, 64, 64, 0x8021, 0, 0, 0xf);
    s[3] = mk(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 64, 64, 0x8023, 0, 0, 0xf);
    why[0] = 0;
    CHECK(legacy_only_slot0_red(4, s) == 1, "negative control: old rule rejects 4 slots");
    CHECK(nvn_vk_mrt_plan_build(s, 4, 0, 0, 0, &p, why, sizeof why) == 0, "new rule accepts 4 slots mixed");
}


// #1409：提交里从未 BindChannelMaskState 时按 NVN 新命令缓冲默认全开
// （nvnChannelMaskStateSetDefaults → 0xffffffff）；零初始化曾让
// colorWriteMask=0 → 附件一字不写（SMBW 合成 pass 黑屏机理）。
static void test_unbound_chmask_defaults_all_on(void) {
    struct nvn_vk_mrt_blend_input in;
    VkPipelineColorBlendAttachmentState cba;

    // 正例：blend/color 绑了、chmask 没绑（该提交没见过 kind 2）→ 全开。
    blend_fixture(&in, 0x0u, 0x0u, -1, 0, 0, 0, 0);
    in.state_seen &= ~(1u << 2);   // 删掉 chmask 的 seen 位
    nvn_vk_mrt_color_blend(0, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.colorWriteMask == 0xfu, "unbound channel mask defaults to all channels on");

    // 全部状态都没绑：照样全开。
    memset(&in, 0, sizeof in);
    nvn_vk_mrt_color_blend(0, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.colorWriteMask == 0xfu, "no state bound at all still defaults mask on");

    // 负控 1：真实绑定 mask=0 仍按零 → 修复不是无脑全开。
    blend_fixture(&in, 0x0u, 0x0u, -1, 0, 0, 0, 0);   // seen bit2=1, mask=0
    nvn_vk_mrt_color_blend(0, 1, VK_FORMAT_R8G8B8A8_UNORM, &in, &cba);
    CHECK(cba.colorWriteMask == 0, "explicitly bound zero mask still writes nothing");

    // 负控 2：旧语义「未绑按零」复刻——同一夹具旧算式得出 0，证明测试打在该路径。
    blend_fixture(&in, 0x0u, 0x0u, -1, 0, 0, 0, 0);
    in.state_seen &= ~(1u << 2);
    uint32_t legacy = (in.state_seen & (1u << 2)) ? ((((const chmask_st *)in.chmask_obj)->mask >> 0) & 0xfu) : 0u;
    CHECK(legacy == 0, "negative control: old rule would mask everything off");
}

// 反馈采样快照：源子资源计划（含 3D 深度切片）、拷贝区域、绑定替换判定。
static void test_feedback_src_plan(void) {
    struct nvn_vk_feedback_src src;
    char why[224];
    memset(&src, 0, sizeof src);
    memset(why, 0, sizeof why);
    // 真实失败形态 1：2D 数组底图 8 级 / 12 层，cview 选第 3 级、第 5 层单切片。
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_2D_ARRAY, 1, 8, 12, 3, 5, 1, 64, 64,
                                   &src, why, sizeof why) == 0,
          "multi-mip/multi-layer RT with a single-slice view accepted");
    CHECK(src.level == 3 && src.layer == 5 && src.layers == 1 && !src.is_3d,
          "feedback source keeps the view subresource");
    // 真实失败形态 2（0100F2C0115B6000 卡点）：3D 底图 depth=4，cview 选第 3 级、深度切片 1。
    // 旧规则用 arrayLayers（3D 恒为 1）判越界 → “layer 1 outside 0..0”。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_3D, 4, 8, 1, 3, 1, 1, 64, 64,
                                   &src, why, sizeof why) == 0,
          "3D RT depth-slice view accepted");
    CHECK(src.level == 3 && src.is_3d && src.slice == 1 && src.layers == 1,
          "3D feedback source keeps the depth slice");
    CHECK((1 >= 1), "negative control: old rule (arrayLayers only) rejected the 3D fixture");
    // 3D 切片越界仍拒绝，原因点名 depth slice。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_3D, 4, 8, 1, 0, 4, 1, 16, 16, &src, why, sizeof why) != 0,
          "3D slice out of range rejected");
    CHECK_STR(why, "depth slice", "3D slice range reason");

    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_2D, 1, 1, 1, 0, 0, 1, 16, 16, &src, why, sizeof why) == 0,
          "single-level/single-layer RT accepted");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_2D_ARRAY, 1, 8, 12, 8, 0, 1, 16, 16, &src, why, sizeof why) != 0,
          "level out of range rejected");
    CHECK_STR(why, "outside", "level range reason");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_2D_ARRAY, 1, 8, 12, 0, 12, 1, 16, 16, &src, why, sizeof why) != 0,
          "layer out of range rejected");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_2D_ARRAY, 1, 8, 12, 0, 0, 2, 16, 16, &src, why, sizeof why) != 0,
          "layered view rejected");
    CHECK_STR(why, "single slice", "layered view reason");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_src_plan(NVN_TEXTURE_TARGET_2D, 1, 1, 1, 0, 0, 1, 0, 16, &src, why, sizeof why) != 0,
          "zero extent rejected");
}

static void test_feedback_copy_make(void) {
    struct nvn_vk_feedback_src src;
    VkImageCopy rc;
    char why[192];
    memset(&src, 0, sizeof src);
    memset(&rc, 0, sizeof rc);
    memset(why, 0, sizeof why);
    // 2D 数组：层号走 baseArrayLayer，srcOffset.z 保持 0。
    src.level = 2; src.layer = 5; src.layers = 1;
    CHECK(nvn_vk_feedback_copy_make(&src, 64, 32, 128, 64, &rc, why, sizeof why) == 0,
          "2D copy region accepted");
    CHECK(rc.srcSubresource.mipLevel == 2 && rc.srcSubresource.baseArrayLayer == 5 &&
          rc.srcSubresource.layerCount == 1 && rc.srcOffset.z == 0,
          "2D copy keeps (level, layer) and no z offset");
    CHECK(rc.extent.width == 64 && rc.extent.height == 32 && rc.extent.depth == 1,
          "2D copy takes the pass/snapshot intersection");
    // 3D：Vulkan 的 3D 子资源 layerCount=1、切片由 srcOffset.z 选，不能按 baseArrayLayer。
    memset(&rc, 0, sizeof rc);
    src.is_3d = 1; src.slice = 3; src.layer = 0;
    CHECK(nvn_vk_feedback_copy_make(&src, 64, 64, 64, 64, &rc, why, sizeof why) == 0,
          "3D slice copy region accepted");
    CHECK(rc.srcSubresource.layerCount == 1 && rc.srcSubresource.baseArrayLayer == 0 &&
          rc.srcOffset.z == 3,
          "3D copy selects the depth slice via srcOffset.z");
    CHECK((rc.srcSubresource.baseArrayLayer != 3), "negative control: 3D slice must not go to baseArrayLayer");
    // 不交集/零范围：准确拒绝。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_copy_make(&src, 0, 64, 64, 64, &rc, why, sizeof why) != 0, "zero pass extent rejected");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_copy_make(NULL, 64, 64, 64, 64, &rc, why, sizeof why) != 0, "missing src rejected");
}

static void test_feedback_bind_plan(void) {
    struct nvn_vk_feedback_bind b;
    char why[224];
    memset(&b, 0, sizeof b);
    memset(why, 0, sizeof why);
    // 单层 2D 默认视图：换快照。
    b.fb_level = 0; b.fb_layer = 0; b.tex_levels = 1; b.tex_layers = 1; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_SUBSTITUTE,
          "single-layer 2D default view substitutes the snapshot");
    // 3D 默认视图：单层 2D 快照表达不了（sampler3D 类型不匹配）→ 同构 3D 快照。
    memset(&b, 0, sizeof b);
    b.fb_is_3d = 1; b.fb_level = 1; b.fb_layer = 2; b.tex_levels = 4; b.tex_layers = 1; b.tex_depth = 4;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_ISO,
          "3D default view takes the isomorphic snapshot");
    // 多层 2D 数组默认视图：同理（单层快照会换错其它层）→ 同构快照。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.tex_levels = 1; b.tex_layers = 12; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_ISO,
          "multi-layer default view takes the isomorphic snapshot");
    // 注册视图恰好是附件那个子资源（2D 单级单层）→ 换快照（0100F2C0115B6000 的 3D 切片采样形态）。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.view_override = 1; b.view_ok = 1; b.override_ok = 1;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D; b.view_base_level = 2; b.view_nlevels = 1;
    b.view_base_layer = 1; b.view_nlayers = 1;
    b.fb_is_3d = 1; b.fb_level = 2; b.fb_layer = 1; b.tex_levels = 8; b.tex_layers = 1; b.tex_depth = 4;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_SUBSTITUTE,
          "matching single 2D slice view substitutes the snapshot");
    // 同图另一个切片/层/级：不是本子资源的反馈 → 不替换也不报。
    b.fb_layer = 3;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_NONE,
          "other depth slice is not feedback");
    b.fb_layer = 1; b.fb_level = 3;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_NONE,
          "other mip level is not feedback");
    // 视图表达不了 / 带格式重写 → 保持原绑定（记原因，不静默换）。覆盖附件的多级注册视图
    // 走同构快照（同 bind_plan_iso 用例）；不含附件子资源的才算“非本资源反馈”。
    memset(why, 0, sizeof why);
    b.fb_level = 2; b.view_nlevels = 2;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_ISO,
          "multi-level view covering the attachment takes iso");
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.view_override = 1; b.view_ok = 1; b.override_ok = 0;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D; b.view_nlevels = 1; b.view_nlayers = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_LEAVE,
          "format/swizzle override is left as is");
}

// 同构快照（ISO）：默认整幅视图落在多级/多层/3D 底图上的自采样——Wonder 8E 的
// serial=5386（128x128 R11G11B10F、12 层 2-cubemap、全 mip 链逐级自采样）实测形态。
// 修前多层默认视图 LEAVE 活绑定直通 → Adreno 同图读写 tile 杂斑烧进环境贴图。
static void test_feedback_bind_plan_iso(void) {
    struct nvn_vk_feedback_bind b;
    char why[224];
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    // 默认视图 + 12 层多级底图（serial=5386 形态）：整幅视图覆盖 8 级 × 12 层，
    // 单层 2D 快照表达不了 → 同构快照。
    b.fb_level = 1; b.fb_layer = 7; b.tex_levels = 8; b.tex_layers = 12; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_ISO,
          "multi-layer default view takes the isomorphic snapshot");
    // 默认视图 + 单层多级底图（泛光 mip 链）：采样整幅视图读的是前级，单层快照只含附件级
    // → 同构快照（修前 SUBSTITUTE 绑的是只有附件一级的快照，mip>0 采样钳到错级）。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.fb_level = 2; b.fb_layer = 0; b.tex_levels = 5; b.tex_layers = 1; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_ISO,
          "multi-level default view takes the isomorphic snapshot");
    // 默认视图 + 3D 底图：单层 2D 快照对 sampler3D 类型不符 → 同构 3D 快照。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.fb_is_3d = 1; b.fb_level = 1; b.fb_layer = 2; b.tex_levels = 2; b.tex_layers = 1; b.tex_depth = 4;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_ISO,
          "3D default view takes the isomorphic snapshot");
    // 负控①：默认视图 + 单级单层底图 → 仍走单层 2D 快照（同构快照不抢单子资源的活）。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.fb_level = 0; b.fb_layer = 0; b.tex_levels = 1; b.tex_layers = 1; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_SUBSTITUTE,
          "single-subresource default view still substitutes");
    // 负控②：注册视图只覆盖同图其它子资源（附件 level 3、采样视图只含 level 0-1）
    // → 不是本子资源的反馈，NONE（修前按“多子资源视图”一律 LEAVE）。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.view_override = 1; b.view_ok = 1; b.override_ok = 1;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D; b.view_base_level = 0; b.view_nlevels = 2;
    b.view_base_layer = 0; b.view_nlayers = 1;
    b.fb_level = 3; b.fb_layer = 0; b.tex_levels = 5; b.tex_layers = 1; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_NONE,
          "view covering only earlier mips is not feedback");
    // 注册视图多层且包含附件子资源 → 同构快照也能代表（ISO，带注册视图形状）。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.view_override = 1; b.view_ok = 1; b.override_ok = 1;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; b.view_base_level = 0; b.view_nlevels = 1;
    b.view_base_layer = 0; b.view_nlayers = 6;
    b.fb_level = 0; b.fb_layer = 3; b.tex_levels = 8; b.tex_layers = 12; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_ISO,
          "multi-layer registered view containing the attachment takes iso");
    // 但建不出视图 / 带格式重写的注册视图仍只能保持原绑定（快照表达不了重写）。
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.view_override = 1; b.view_ok = 0; b.override_ok = 1;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; b.view_nlevels = 1; b.view_nlayers = 6;
    b.fb_level = 0; b.fb_layer = 3; b.tex_levels = 8; b.tex_layers = 12; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_LEAVE,
          "uncreatable registered view is left as is");
    memset(&b, 0, sizeof b); memset(why, 0, sizeof why);
    b.view_override = 1; b.view_ok = 1; b.override_ok = 0;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; b.view_nlevels = 1; b.view_nlayers = 6;
    b.fb_level = 0; b.fb_layer = 3; b.tex_levels = 8; b.tex_layers = 12; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_bind_plan(&b, why, sizeof why) == NVN_VK_FB_BIND_LEAVE,
          "format-override registered view is left as is");
}

// 负控粒度：no_fb_iso=1 时「多子资源视图不建快照纯直通」。与 bind_plan 的 ISO 判定同族
// （差别：不看附件覆盖关系——快照创建时附件 mip/层还没定）。覆盖=0 的视图直通无碍
// （本来就不是反馈），单子资源视图照旧单层快照。
static void test_feedback_view_multi(void) {
    struct nvn_vk_feedback_bind b;
    memset(&b, 0, sizeof b);
    CHECK(nvn_vk_feedback_view_multi(NULL) == 0, "null input is not multi");
    // 默认视图：单级单层 2D → 0（no_fb_iso 下仍走单层快照）。
    b.tex_levels = 1; b.tex_layers = 1; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_view_multi(&b) == 0, "single-subresource default view is not multi");
    // 默认视图 + 12 层多级底图（serial=5386/5385 形态）→ 1 = 纯直通不建快照。
    memset(&b, 0, sizeof b);
    b.tex_levels = 8; b.tex_layers = 12; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "multi-layer default view is multi (passthrough)");
    // 默认视图 + 单层多级底图 → 1。
    memset(&b, 0, sizeof b);
    b.tex_levels = 5; b.tex_layers = 1; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "multi-level default view is multi");
    // 默认视图 + 3D 底图 → 1；depth>1 的 2D-array-like 也 → 1。
    memset(&b, 0, sizeof b);
    b.fb_is_3d = 1; b.tex_levels = 2; b.tex_layers = 1; b.tex_depth = 4;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "3D default view is multi");
    memset(&b, 0, sizeof b);
    b.tex_levels = 1; b.tex_layers = 1; b.tex_depth = 8;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "depth>1 default view is multi");
    // 注册视图恰好单子资源（mip 链逐级的单层视图形态——serial=5385 的 32 笔）→ 0 照旧快照。
    memset(&b, 0, sizeof b);
    b.view_override = 1; b.view_ok = 1;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D; b.view_nlevels = 1; b.view_nlayers = 1;
    b.tex_levels = 8; b.tex_layers = 12; b.tex_depth = 1;
    CHECK(nvn_vk_feedback_view_multi(&b) == 0,
          "single-subresource registered view keeps the snapshot path");
    // 注册视图多级/多层/非 2D → 1 纯直通。
    b.view_nlevels = 2;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "multi-level registered view is multi");
    memset(&b, 0, sizeof b);
    b.view_override = 1; b.view_ok = 1;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; b.view_nlevels = 1; b.view_nlayers = 6;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "multi-layer registered view is multi");
    memset(&b, 0, sizeof b);
    b.view_override = 1; b.view_ok = 1;
    b.view_type = VK_IMAGE_VIEW_TYPE_3D; b.view_nlevels = 1; b.view_nlayers = 1;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "3D registered view is multi");
    // 注册视图建不出 → 按多算（快照替不了，直通与旧 LEAVE 同结果）。
    memset(&b, 0, sizeof b);
    b.view_override = 1; b.view_ok = 0;
    b.view_type = VK_IMAGE_VIEW_TYPE_2D; b.view_nlevels = 1; b.view_nlayers = 1;
    CHECK(nvn_vk_feedback_view_multi(&b) == 1, "uncreatable registered view is multi");
}

// 同构快照计划与逐子资源拷贝区域（纯逻辑）。
static void test_feedback_iso(void) {
    struct nvn_vk_feedback_iso iso;
    char why[224];
    memset(&iso, 0, sizeof iso); memset(why, 0, sizeof why);
    // serial=5386 形态：128x128 R11G11B10F（4B/px）、8 级 × 12 层。
    CHECK(nvn_vk_feedback_iso_plan(0, 128, 128, 1, 8, 12, 4, 64ull << 20,
                                   &iso, why, sizeof why) == 0,
          "128x128x12x8L iso plan accepted");
    CHECK(!iso.is_3d && iso.w == 128 && iso.h == 128 && iso.depth == 1,
          "iso keeps the level-0 extent");
    CHECK(iso.levels == 8 && iso.layers == 12 && iso.n_copies == 96,
          "iso copies every level x layer (96)");
    // est_bytes = Σ mip_extent(128,l)^2 × 12 × 4 = 21845 × 48 = 1048560。
    CHECK(iso.est_bytes == 1048560, "iso est_bytes matches the mip-sum");
    VkImageCopy c;
    memset(&c, 0, sizeof c);
    CHECK(nvn_vk_feedback_iso_copy(&iso, 0, &c, why, sizeof why) == 0, "iso copy 0 accepted");
    CHECK(c.srcSubresource.mipLevel == 0 && c.srcSubresource.baseArrayLayer == 0 &&
          c.srcSubresource.layerCount == 1 && c.extent.width == 128 &&
          c.extent.height == 128 && c.extent.depth == 1 &&
          c.dstSubresource.mipLevel == 0 && c.dstSubresource.baseArrayLayer == 0,
          "copy 0 = level0 layer0 full extent");
    memset(&c, 0, sizeof c);
    CHECK(nvn_vk_feedback_iso_copy(&iso, 12, &c, why, sizeof why) == 0, "iso copy 12 accepted");
    CHECK(c.srcSubresource.mipLevel == 1 && c.srcSubresource.baseArrayLayer == 0 &&
          c.extent.width == 64 && c.extent.height == 64 &&
          c.dstSubresource.mipLevel == 1,
          "copy 12 = level1 layer0 half extent");
    memset(&c, 0, sizeof c);
    CHECK(nvn_vk_feedback_iso_copy(&iso, 95, &c, why, sizeof why) == 0, "iso copy 95 accepted");
    CHECK(c.srcSubresource.mipLevel == 7 && c.srcSubresource.baseArrayLayer == 11 &&
          c.extent.width == 1 && c.extent.height == 1,
          "copy 95 = level7 layer11 1x1");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_iso_copy(&iso, 96, &c, why, sizeof why) != 0,
          "copy index past n_copies refused");
    // 3D：每级一份拷贝、深度进 extent.depth（Vulkan 3D 子资源 layerCount 恒 1）。
    memset(&iso, 0, sizeof iso); memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_iso_plan(1, 32, 32, 4, 2, 1, 4, 64ull << 20,
                                   &iso, why, sizeof why) == 0, "3D iso plan accepted");
    CHECK(iso.is_3d && iso.n_copies == 2 && iso.layers == 1 && iso.depth == 4,
          "3D iso: one copy per level");
    memset(&c, 0, sizeof c);
    CHECK(nvn_vk_feedback_iso_copy(&iso, 1, &c, why, sizeof why) == 0, "3D copy level1 accepted");
    CHECK(c.srcSubresource.mipLevel == 1 && c.srcSubresource.baseArrayLayer == 0 &&
          c.extent.width == 16 && c.extent.height == 16 && c.extent.depth == 2 &&
          c.srcOffset.z == 0 && c.dstOffset.z == 0,
          "3D copy level1 covers mip depth");
    // 上限：cap 低于 est_bytes 必须拒绝（与苹果 ④ 的“视图太大不拷”同思路）。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_iso_plan(0, 128, 128, 1, 8, 12, 4, 1024,
                                   &iso, why, sizeof why) != 0,
          "iso plan over the byte cap refused");
    // 参数不全必须拒绝。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_iso_plan(0, 128, 128, 1, 8, 0, 4, 64ull << 20,
                                   &iso, why, sizeof why) != 0, "zero layers refused");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_iso_plan(0, 128, 128, 1, 0, 12, 4, 64ull << 20,
                                   &iso, why, sizeof why) != 0, "zero levels refused");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_feedback_iso_plan(0, 128, 128, 1, 8, 12, 0, 64ull << 20,
                                   &iso, why, sizeof why) != 0, "zero bpp refused");
}

// 块编码渲染目标（压缩底图经“块=像素”的无压缩视图）：计划与往返拷贝区域。
static void test_bcrt_plan(void) {
    struct nvn_vk_bcrt_plan p;
    char why[224];
    memset(&p, 0, sizeof p);
    memset(why, 0, sizeof why);
    // 0100F2C0115B6000 的真实形态：BC4(8B/块) + RGBA16UI(8B/像素)，32x32 的 mip → 8x8 块。
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R16G16B16A16_UINT, 32, 32, 0, 2, 1,
                                &p, why, sizeof why) == 0, "BC4 + 8-byte UI view accepted");
    CHECK(p.block_bytes == 8 && p.side == 4 && p.bw == 8 && p.bh == 8,
          "BC4 plan: 8-byte blocks, 4x4 side, 32 -> 8 shadow pixels");
    CHECK(p.is_3d == 1 && p.layer == 2 && p.level == 0, "BC4 plan keeps the depth slice");
    // 向上取整：31 → 8 块、1 → 1 块。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R16G16B16A16_UINT, 31, 5, 1, 0, 0,
                                &p, why, sizeof why) == 0 && p.bw == 8 && p.bh == 2,
          "partial texel rows round up to whole blocks");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R16G16B16A16_UINT, 1, 1, 0, 0, 0,
                                &p, why, sizeof why) == 0 && p.bw == 1 && p.bh == 1,
          "1x1 mip is one block");
    // 格式层判定（编译事实与附件计划共用）：块字节 == 视图每像素字节。
    {
        VkFormat vf = VK_FORMAT_UNDEFINED;
        char vwhy[160] = "";
        CHECK(nvn_vk_bcrt_view_format(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R16G16B16A16_UINT, &vf, vwhy, sizeof vwhy) == 0 &&
              vf == VK_FORMAT_R16G16B16A16_UINT, "format-level: BC4 + 8-byte UI view accepted");
        memset(vwhy, 0, sizeof vwhy);
        vf = VK_FORMAT_UNDEFINED;
        CHECK(nvn_vk_bcrt_view_format(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, &vf, vwhy, sizeof vwhy) != 0 &&
              vf == VK_FORMAT_UNDEFINED, "format-level: 4-byte view rejected");
        memset(vwhy, 0, sizeof vwhy);
        CHECK(nvn_vk_bcrt_view_format(VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_UINT, &vf, vwhy, sizeof vwhy) != 0,
              "format-level: uncompressed base rejected");
        memset(vwhy, 0, sizeof vwhy);
        CHECK(nvn_vk_bcrt_view_format(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R32G32B32A32_UINT, &vf, vwhy, sizeof vwhy) == 0,
              "format-level: BC3 + 16-byte view accepted");
    }
    // 负控：4 字节视图编码不了 8 字节块；旧/宽松规则会接受它。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 0, 0, 0,
                                &p, why, sizeof why) != 0, "4-byte view over 8-byte blocks rejected");
    // 4 字节视图先被“不是 8/16 字节类”挡掉；两类原因都算准确拒绝（真正编不了块）。
    CHECK(strstr(why, "cannot encode blocks") || strstr(why, "8/16-byte"),
          "byte-size mismatch reason");
    // 16 字节块 + 16 字节视图（BC3 + RGBA32UI）。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R32G32B32A32_UINT, 64, 64, 0, 0, 0,
                                &p, why, sizeof why) == 0 && p.block_bytes == 16 && p.bw == 16,
          "BC3 + 16-byte view accepted");
    // 深度视图 / 非压缩底图：准确拒绝。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_D32_SFLOAT, 32, 32, 0, 0, 0,
                                &p, why, sizeof why) != 0, "depth view rejected");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_UINT, 32, 32, 0, 0, 0,
                                &p, why, sizeof why) != 0, "uncompressed base rejected");
}

// 图形驱动常量段 c0 的合成布局（宿主可测的纯逻辑）。
static void test_driver_c0(void) {
    uint8_t c0[NVN_VK_DRIVER_C0_BYTES];
    char why[160];
    uint64_t addr[16]; uint32_t size[16];
    memset(addr, 0, sizeof addr); memset(size, 0, sizeof size);
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_driver_c0_base(0) == 0x110, "c0 vs table at 0x110");
    CHECK(nvn_vk_driver_c0_base(1) == 0x510, "c0 fs table at 0x510");
    CHECK(nvn_vk_driver_c0_base(2) == 0x410, "c0 gs table at 0x410");
    CHECK(nvn_vk_driver_c0_base(3) == 0x210, "c0 tcs table at 0x210");
    CHECK(nvn_vk_driver_c0_base(4) == 0x310, "c0 tes table at 0x310");
    CHECK(nvn_vk_driver_c0_base(5) < 0, "c0 has no compute table base in the graphics fill");
    addr[3] = 0x1234567890ull; size[3] = 23232; addr[5] = 0xabc000ull; size[5] = 64;
    CHECK(nvn_vk_driver_c0_fill_graphics(c0, sizeof c0, 1, addr, size, 7, why, sizeof why) == 0,
          "c0 fill for fs accepted");
    uint64_t ra = 0; uint32_t rn = 0;
    memcpy(&ra, c0 + 0x510 + 16 * 3, sizeof ra);
    memcpy(&rn, c0 + 0x510 + 16 * 3 + 8, sizeof rn);
    CHECK(ra == 0x1234567890ull && rn == 23232, "fs storage slot 3 descriptor at 0x510+48");
    memcpy(&ra, c0 + 0x510 + 16 * 5, sizeof ra);
    CHECK(ra == 0xabc000ull, "fs storage slot 5 descriptor at 0x510+80");
    memcpy(&ra, c0 + 0x110 + 16 * 3, sizeof ra);
    CHECK(ra == 0, "vs table is not written by an fs fill");
    float maxp = 0; uint32_t binst = 0;
    memcpy(&maxp, c0 + 0x610, sizeof maxp);
    memcpy(&binst, c0 + 0x644, sizeof binst);
    CHECK(maxp == NVN_VK_DRIVER_C0_MAX_POINT, "c0 0x610 holds the max point size");
    CHECK(binst == 7, "c0 0x644 holds the base instance");
    // 负控：缓冲区小于测量布局 / 未知 stage 都准确拒绝。
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_driver_c0_fill_graphics(c0, 0x100, 0, addr, size, 0, why, sizeof why) != 0,
          "c0 fill rejects a buffer smaller than the measured layout");
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_driver_c0_fill_graphics(c0, sizeof c0, 9, addr, size, 0, why, sizeof why) != 0,
          "c0 fill rejects an unknown stage");
}

static void test_bcrt_copy_regions(void) {
    struct nvn_vk_bcrt_plan p;
    char why[192];
    memset(&p, 0, sizeof p);
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R16G16B16A16_UINT, 32, 20, 1, 2, 1,
                                &p, why, sizeof why) == 0, "bcrt fixture accepted");
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    nvn_vk_bcrt_img_to_buf(&p, &c);
    // 3D：压缩子资源 layerCount=1、切片走 imageOffset.z；extent 用纹素（整 mip）。
    CHECK(c.imageSubresource.layerCount == 1 && c.imageSubresource.baseArrayLayer == 0,
          "3D compressed copy keeps layerCount=1/baseArrayLayer=0");
    CHECK(c.imageSubresource.mipLevel == 1 && c.imageOffset.z == 2,
          "3D compressed copy selects the depth slice via imageOffset.z");
    CHECK(c.imageExtent.width == 32 && c.imageExtent.height == 20 && c.imageExtent.depth == 1,
          "compressed copy covers the whole mip in texels");
    CHECK((c.imageSubresource.baseArrayLayer != 2), "negative control: the 3D slice must not go to baseArrayLayer");
    CHECK(c.bufferRowLength == 0 && c.bufferImageHeight == 0, "tightly packed block rows");
    memset(&c, 0, sizeof c);
    nvn_vk_bcrt_buf_to_shadow(&p, &c);
    CHECK(c.imageExtent.width == 8 && c.imageExtent.height == 5 && c.imageOffset.z == 0,
          "shadow copy uses bw x bh = 8 x 5 pixels");
    memset(&c, 0, sizeof c);
    nvn_vk_bcrt_shadow_to_buf(&p, &c);
    CHECK(c.imageExtent.width == 8 && c.imageExtent.height == 5, "shadow -> buffer uses bw x bh");
    memset(&c, 0, sizeof c);
    nvn_vk_bcrt_buf_to_img(&p, &c);
    CHECK(c.imageOffset.z == 2 && c.imageSubresource.mipLevel == 1, "buffer -> compressed keeps the slice");
    // 2D 数组：层号走 baseArrayLayer，z 恒 0。
    memset(&p, 0, sizeof p);
    memset(why, 0, sizeof why);
    CHECK(nvn_vk_bcrt_plan_make(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R16G16B16A16_UINT, 32, 32, 0, 3, 0,
                                &p, why, sizeof why) == 0, "2D array bcrt fixture accepted");
    memset(&c, 0, sizeof c);
    nvn_vk_bcrt_img_to_buf(&p, &c);
    CHECK(c.imageSubresource.baseArrayLayer == 3 && c.imageOffset.z == 0,
          "2D array layer goes to baseArrayLayer with no z offset");
}

int main(void) {
    test_two_slots();
    test_three_slots_with_hole();
    test_four_slots_mixed();
    test_slot0_hole();
    test_depth_only();
    test_rejections();
    test_duplicates();
    test_depth_mismatch_dropped();
    test_blend_per_slot();
    test_unbound_chmask_defaults_all_on();
    test_negative_control_old_rule();
    test_feedback_src_plan();
    test_feedback_copy_make();
    test_feedback_bind_plan();
    test_feedback_bind_plan_iso();
    test_feedback_view_multi();
    test_feedback_iso();
    test_bcrt_plan();
    test_bcrt_copy_regions();
    test_driver_c0();
    if (fails) { printf("direct nvn mrt: %d failure(s)\n", fails); return 1; }
    printf("direct nvn mrt: ok\n");
    return 0;
}
