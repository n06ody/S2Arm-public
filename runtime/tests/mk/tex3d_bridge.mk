# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# native-test-tex3d_bridge：#674 3D 槽里的 2D / 2D 数组贴图要不要做 3D 副本、何时拷的纯 C 判定（graphics/nvn/gfx/nvn/nvn_tex3d_bridge.h）
#   runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk
NATIVE_TEST_OWN += tex3d_bridge
.PHONY: native-test-tex3d_bridge
native-test-tex3d_bridge:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=gnu11 -Igraphics/nvn/gfx/nvn -o $(BUILD)/tests/t_tex3d_bridge graphics/nvn/tests/t_tex3d_bridge.c
	timeout 30 $(BUILD)/tests/t_tex3d_bridge
TEST_TARGETS += native-test-tex3d_bridge
