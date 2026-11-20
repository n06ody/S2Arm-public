// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// #674 (11-15): a 2D / 2D array texture in a 3D slot (graphics/nvn/gfx/nvn/nvn_tex3d_bridge.h).
//   Expectations are worked out by hand, not by running the implementation under test.
//   The case behind it: lighting shaders declare 3D at slots where the game binds a 4x4 2D placeholder (format
//   RGBA8) or a 4x4 one-layer 2D array (format R32F). No view bridged them, the slots read 0 and the lit
//   surfaces went black for the frames the placeholder was bound; with the 3D copy bound they read its texels.
#include <stdio.h>
#include "nvn_tex3d_bridge.h"

static int bad, checks;
#define CHECK(c, ...) do { checks++; int ok_ = !!(c); bad += !ok_; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    // ---- which bindings get a copy, and how deep ----
    CHECK(t3b_depth(T3B_TYPE_2D, T3B_TYPE_3D, 1, 1, 1) == 1, "1 a 2D texture read as 3D: one plane (the 4x4 RGBA8 placeholder)");
    CHECK(t3b_depth(T3B_TYPE_2D_ARRAY, T3B_TYPE_3D, 1, 1, 1) == 1, "2 a one-layer 2D array read as 3D: one plane (the 4x4 R32F placeholder)");
    CHECK(t3b_depth(T3B_TYPE_2D_ARRAY, T3B_TYPE_3D, 1, 6, 1) == 6, "3 a six-layer 2D array read as 3D: one plane per layer");
    CHECK(t3b_depth(T3B_TYPE_2D_ARRAY, T3B_TYPE_3D, 1, 0, 1) == 1, "4 a 2D array reporting no layers still gets its one plane");
    CHECK(t3b_depth(T3B_TYPE_2D, T3B_TYPE_2D_ARRAY, 1, 1, 1) == 0, "5 a 2D texture read as a 2D array is not this bridge's (a view does it)");
    CHECK(t3b_depth(T3B_TYPE_3D, T3B_TYPE_3D, 1, 1, 1) == 0, "6 a 3D texture in a 3D slot needs nothing");
    CHECK(t3b_depth(5 /* cube */, T3B_TYPE_3D, 1, 1, 1) == 0, "7 a cube read as 3D: no bridge, the zero texture and the report stay");
    CHECK(t3b_depth(T3B_TYPE_2D, T3B_TYPE_3D, 4, 1, 1) == 0, "8 a multisample 2D texture: no bridge");
    CHECK(t3b_depth(T3B_TYPE_2D, T3B_TYPE_3D, 1, 1, 0) == 0, "9 a compressed / depth format a 3D texture cannot take: no bridge");
    // ---- when the copy is made ----
    CHECK(t3b_action(0, 100, 0) == T3B_COPY, "10 never copied, no pass open: copy");
    CHECK(t3b_action(0, 100, 1) == T3B_COPY, "11 never copied, a pass is open: copy all the same (the draw would read 0)");
    CHECK(t3b_action(101, 100, 0) == T3B_USE, "12 copied this frame: use it");
    CHECK(t3b_action(101, 100, 1) == T3B_USE, "13 copied this frame, pass open: use it");
    CHECK(t3b_action(100, 100, 0) == T3B_COPY, "14 copied last frame, no pass open: refresh");
    CHECK(t3b_action(100, 100, 1) == T3B_USE, "15 copied last frame, a pass is open: keep it, no pass is ended to refresh");
    CHECK(t3b_action(1, 0, 0) == T3B_USE, "16 frame 0: a copy made in frame 0 (made = 1) counts as this frame's");
    printf("%d checks, %d failed\n", checks, bad);
    if (bad) printf("✗ t_tex3d_bridge：%d 项不通（共 %d 项）\n", bad, checks);
    else printf("✓ t_tex3d_bridge：%d 项全过\n", checks);   // 列车按结论行判（tests/t_result_lint）
    return bad != 0;
}
