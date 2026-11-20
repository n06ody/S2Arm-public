// 宿主单测：android/native/nvn/nvn_vk_border.h（没有 VK_EXT_custom_border_color 时边框色就近取内置三档）。
#include <stdio.h>
#include "nvn_vk_border.h"

static int fails;
#define CHECK(id, cond) do { if (!(cond)) { printf("FAIL %s: %s\n", id, #cond); fails++; } } while (0)

int main(void) {
    static const float measured[4] = {0.132375f, 0.5f, 0.5f, 0.0273442f};   // Pura X 实测（01007EF00011E000）的边框色
    CHECK("T1", nvk_border_nearest(measured, 0) == 0);                           // 浮点 → 透明黑
    CHECK("T2", nvk_border_nearest(measured, 1) == 0);                           // 整数 (0,1,1,0)：透明黑与不透明白等距 → 下标小的
    static const float exact[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    for (int k = 0; k < 3; k++) { CHECK("T3", nvk_border_nearest(exact[k], 0) == k); CHECK("T4", nvk_border_nearest(exact[k], 1) == k); }
    static const float grey_opaque[4] = {0.3f, 0.3f, 0.3f, 0.9f}, light[4] = {0.8f, 0.9f, 0.7f, 1.0f};
    CHECK("T5", nvk_border_nearest(grey_opaque, 0) == 1);                    // 暗、不透明 → 不透明黑
    CHECK("T6", nvk_border_nearest(light, 0) == 2);                          // 亮、不透明 → 不透明白
    static const float f_int[4] = {0.4f, 0.4f, 0.4f, 0.6f};                  // 浮点：不透明黑；整数四舍五入成 (0,0,0,1) 也是不透明黑
    CHECK("T7", nvk_border_nearest(f_int, 0) == 1 && nvk_border_nearest(f_int, 1) == 1);
    static const float int_white[4] = {0.6f, 0.6f, 0.6f, 0.4f};              // 整数四舍五入成 (1,1,1,0) → 不透明白
    CHECK("T8", nvk_border_nearest(int_white, 1) == 2);
    static const float big[4] = {255, 255, 255, 255}, neg[4] = {-5, -5, -5, -5};   // 整数大值 / 负值不溢出
    CHECK("T9", nvk_border_nearest(big, 1) == 2 && nvk_border_nearest(big, 0) == 2);
    CHECK("T10", nvk_border_nearest(neg, 1) == 0);
    static const float nan4[4] = {0.0f / 0.0f, 0, 0, 1};                     // NaN 按 0
    CHECK("T11", nvk_border_nearest(nan4, 0) == 1);
    static const float rounds[4] = {0.55f, 0.55f, 0.55f, 0};                 // 浮点最近透明黑；整数先取整成 (1,1,1,0) → 不透明白
    CHECK("T12", nvk_border_nearest(rounds, 0) == 0 && nvk_border_nearest(rounds, 1) == 2);
    if (fails) return 1;
    puts("PASS: nvk_border_nearest 12 条");
    return 0;
}
