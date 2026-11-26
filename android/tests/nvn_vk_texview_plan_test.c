// 宿主用例：NVNtextureView → Vulkan 子资源视图的纯决策。
// 覆盖 3D 深度切片（附件 2D 视图 / 越界）、2D array 单层视图、cube 单面视图、
// 默认附件落到 mip0/layer0，以及 3D 采样 3D 视图的 Vulkan 范围约束。
// 不含 Vulkan 调用，任何宿主都能跑；负控（撤修复）时这些断言会红。
#include <stdio.h>
#include <string.h>
#include "nvn_vk_texview.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static view_t view_levels_layers(uint32_t bl, uint32_t nl, uint32_t by, uint32_t ny) {
    view_t v; memset(&v, 0, sizeof v);
    v.set_levels = 1; v.base_level = (uint8_t)bl; v.nlevels = (uint8_t)nl;
    v.set_layers = 1; v.min_layer = (uint16_t)by; v.nlayers = (uint16_t)ny;
    return v;
}

static struct nvn_vk_texview_plan plan(uint32_t target, uint32_t depth, uint32_t layers, uint32_t levels,
                                       const view_t *v, int attachment, int compat) {
    struct nvn_vk_texview_plan p;
    nvn_vk_texview_plan_make(target, depth, layers, levels, v, attachment, compat, &p);
    return p;
}

int main(void) {
    // ---- 3D：层号 = 深度切片，附件必须落成 2D 视图 ----
    {
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, NULL, 1, 1);
        CHECK(p.ok, "3D default attachment rejected: %s", p.why);
        CHECK(p.type == VK_IMAGE_VIEW_TYPE_2D && p.base_layer == 0 && p.nlayers == 1 && p.nlevels == 1,
              "3D default attachment type=%d layer=%u+%u mip=%u+%u (want 2D 0+1 mip0+1)",
              (int)p.type, p.base_layer, p.nlayers, p.base_level, p.nlevels);
    }
    {
        view_t v = view_levels_layers(0, 1, 1, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, &v, 1, 1);
        CHECK(p.ok, "3D slice 1 attachment rejected: %s", p.why);
        CHECK(p.type == VK_IMAGE_VIEW_TYPE_2D && p.base_layer == 1 && p.nlayers == 1,
              "3D slice 1 attachment type=%d layer=%u+%u (want 2D layer 1+1)",
              (int)p.type, p.base_layer, p.nlayers);
    }
    {
        view_t v = view_levels_layers(0, 1, 2, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, &v, 1, 1);
        CHECK(!p.ok && strstr(p.why, "layer 2+1 of 2") && strstr(p.why, "target=2"),
              "3D layer out of range not rejected accurately: ok=%d why=%s", p.ok, p.why);
    }
    {
        view_t v = view_levels_layers(0, 1, 0, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, &v, 1, 0);
        CHECK(!p.ok && strstr(p.why, "2D_ARRAY_COMPATIBLE"),
              "3D slice attachment without compat flag not rejected: ok=%d why=%s", p.ok, p.why);
    }
    {
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, NULL, 0, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_3D && p.base_layer == 0 && p.nlayers == 1,
              "3D default sampling view type=%d layer=%u+%u (want 3D 0+1): %s",
              (int)p.type, p.base_layer, p.nlayers, p.why);
    }
    {
        // main 上 3D 的 layers 固定为 1，显式 0+1 采样过去能过；这里必须是正例（不新增回归）。
        view_t v = view_levels_layers(0, 1, 0, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, &v, 0, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_3D && p.base_layer == 0 && p.nlayers == 1,
              "3D sampling view 0+1 rejected: ok=%d why=%s", p.ok, p.why);
    }
    {
        // base_layer==0 时任意层数都接受（NVN/苹果路线对 3D 视图取 1 层）。
        view_t v = view_levels_layers(0, 1, 0, 2);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, &v, 0, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_3D && p.base_layer == 0 && p.nlayers == 1,
              "3D sampling view 0+2 rejected: ok=%d why=%s", p.ok, p.why);
    }
    {
        view_t v = view_levels_layers(0, 1, 1, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, &v, 0, 1);
        CHECK(!p.ok && strstr(p.why, "depth slice"),
              "3D sampling sub-slice 3D view not rejected: ok=%d why=%s", p.ok, p.why);
    }
    {
        view_t v = view_levels_layers(0, 1, 1, 1);
        v.set_target = 1; v.target = NVN_TEXTURE_TARGET_2D_ARRAY;
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 2, 1, 1, &v, 0, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_2D_ARRAY && p.base_layer == 1 && p.nlayers == 1,
              "3D slice sampling with 2D_ARRAY target type=%d layer=%u+%u: %s",
              (int)p.type, p.base_layer, p.nlayers, p.why);
    }
    {
        view_t v = view_levels_layers(1, 1, 2, 2);
        v.set_target = 1; v.target = NVN_TEXTURE_TARGET_2D_ARRAY;
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 8, 1, 2, &v, 1, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_2D_ARRAY && p.base_layer == 2 && p.nlayers == 2,
              "valid 3D mip 1 layered attachment rejected: %s", p.why);
        v.min_layer = 3;
        p = plan(NVN_TEXTURE_TARGET_3D, 8, 1, 2, &v, 0, 1);
        CHECK(!p.ok && strstr(p.why, "layer 3+2 of 4"),
              "3D slice view exceeded the selected mip: ok=%d why=%s", p.ok, p.why);
    }
    {
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_3D, 1, 1, 1, NULL, 0, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_3D, "depth-one 3D sampling type=%d: %s", (int)p.type, p.why);
    }

    // ---- 2D array：单层视图可建；越界仍拒 ----
    {
        view_t v = view_levels_layers(0, 1, 1, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_2D_ARRAY, 1, 2, 1, &v, 1, 1);
        CHECK(p.ok && p.base_layer == 1 && p.nlayers == 1 && p.nlevels == 1,
              "2D array single-layer attachment rejected: %s", p.why);
        CHECK(p.type == VK_IMAGE_VIEW_TYPE_2D_ARRAY || p.type == VK_IMAGE_VIEW_TYPE_2D,
              "2D array single-layer attachment type=%d", (int)p.type);
    }
    {
        view_t v = view_levels_layers(0, 1, 2, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_2D_ARRAY, 1, 2, 1, &v, 0, 1);
        CHECK(!p.ok && strstr(p.why, "layer 2+1 of 2"),
              "2D array layer out of range not rejected: ok=%d why=%s", p.ok, p.why);
    }

    // ---- cube：单面附件必须是 2D 视图；第 7 面越界 ----
    {
        view_t v = view_levels_layers(0, 1, 3, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_CUBEMAP, 1, 6, 1, &v, 1, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_2D && p.base_layer == 3 && p.nlayers == 1,
              "cube single-face attachment type=%d layer=%u+%u: %s",
              (int)p.type, p.base_layer, p.nlayers, p.why);
    }
    {
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_CUBEMAP, 1, 6, 1, NULL, 1, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_2D && p.base_layer == 0 && p.nlayers == 1,
              "cube default attachment type=%d layer=%u+%u (want 2D 0+1): %s",
              (int)p.type, p.base_layer, p.nlayers, p.why);
    }
    {
        view_t v = view_levels_layers(0, 1, 6, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_CUBEMAP, 1, 6, 1, &v, 0, 1);
        CHECK(!p.ok && strstr(p.why, "layer 6+1 of 6"),
              "cube face out of range not rejected: ok=%d why=%s", p.ok, p.why);
    }
    {
        view_t v = view_levels_layers(0, 1, 7, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 1, 12, 1, &v, 1, 1);
        CHECK(p.ok && p.type == VK_IMAGE_VIEW_TYPE_2D && p.base_layer == 7,
              "cube-array single-face attachment type=%d layer=%u: %s",
              (int)p.type, p.base_layer, p.why);
    }

    // 显式合法目标优先；其余按最终层数选类型，不改 mip / layer 范围。
    // 包含继承和显式目标，跨越 1、2–5、6、非 6 倍数以及多个 cube 的边界。
    {
        const struct { uint32_t target, layers, first, count; int explicit_target; uint32_t desired_target; VkImageViewType want; } cases[] = {
            {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0, 6, 1, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0, 6, 1, NVN_TEXTURE_TARGET_2D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0, 1, 1, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, VK_IMAGE_VIEW_TYPE_2D},
            {NVN_TEXTURE_TARGET_CUBEMAP, 6, 3, 1, 0, 0, VK_IMAGE_VIEW_TYPE_2D},
            {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0, 2, 0, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0, 5, 0, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0, 6, 0, 0, VK_IMAGE_VIEW_TYPE_CUBE},
            {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, 0, 1, 1, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, VK_IMAGE_VIEW_TYPE_2D},
            {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, 0, 2, 1, NVN_TEXTURE_TARGET_CUBEMAP, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, 1, 6, 1, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, 0, 7, 0, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, 0, 12, 0, 0, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY},
            {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 18, 0, 18, 1, NVN_TEXTURE_TARGET_2D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            view_t v = view_levels_layers(2, 1, cases[i].first, cases[i].count);
            v.set_target = cases[i].explicit_target; v.target = (uint8_t)cases[i].desired_target;
            struct nvn_vk_texview_plan p = plan(cases[i].target, cases[i].layers, cases[i].layers, 8, &v, 0, 1);
            CHECK(p.ok && p.type == cases[i].want && p.base_level == 2 && p.nlevels == 1 &&
                  p.base_layer == cases[i].first && p.nlayers == cases[i].count,
                  "cube sampling case %zu count=%u target=%d expected=%d actual=%d range=%u+%u mip=%u+%u: %s",
                  i, cases[i].count, cases[i].explicit_target ? (int)cases[i].desired_target : -1,
                  (int)cases[i].want, (int)p.type, p.base_layer, p.nlayers, p.base_level, p.nlevels, p.why);
        }
    }

    // ---- 单 mip 约束、默认附件范围 ----
    {
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_2D, 1, 1, 3, NULL, 1, 1);
        CHECK(p.ok && p.base_level == 0 && p.nlevels == 1 && p.type == VK_IMAGE_VIEW_TYPE_2D,
              "2D default attachment mip range=%u+%u type=%d (want 0+1 2D): %s",
              p.base_level, p.nlevels, (int)p.type, p.why);
    }
    {
        view_t v = view_levels_layers(0, 2, 0, 2);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_2D_ARRAY, 1, 2, 2, &v, 1, 1);
        CHECK(!p.ok && strstr(p.why, "exactly one mip"),
              "multi-mip attachment not rejected: ok=%d why=%s", p.ok, p.why);
    }
    {
        view_t v = view_levels_layers(2, 1, 0, 1);
        struct nvn_vk_texview_plan p = plan(NVN_TEXTURE_TARGET_2D, 1, 1, 2, &v, 0, 1);
        CHECK(!p.ok && strstr(p.why, "mip 2+1 of 2"),
              "mip out of range not rejected: ok=%d why=%s", p.ok, p.why);
    }

    if (!failures) printf("nvn-vk-texview-plan: ok (3D mip slices and array/cube attachment bounds)\n");
    return failures ? 1 : 0;
}
