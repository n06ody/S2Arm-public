# native-test-nvn_replay：Vulkan 回放核心（graphics/nvn/replay，docs/设计_Vulkan回放层重搭.md）的宿主单测。
# 纯 C：后端（nvn_gpu_*）与平台件（nvn_plat_*）在单测里用替身，不需要 GPU。
#   1. C11 -Werror 编译并运行；接线表 docs/直连_Vulkan回放接线表.tsv 的值与命令名逐行对名字表；
#   2. 分层扫描：回放核心（rp_*.c / rp_*.h / nvn_unimpl.* / nvn_plat.h）注释外不许出现 Vulkan / Metal / 安卓 / 鸿蒙的头文件与类型，
#      也不许直接读系统属性或环境变量（开关只走 nvn_plat_flag）。nvn_plat_vk.h 是给后端的，不在扫描范围。
NVN_REPLAY_DIR := graphics/nvn/replay
NVN_REPLAY_CORE := $(NVN_REPLAY_DIR)/rp_exec.c $(NVN_REPLAY_DIR)/rp_clear_tex.c $(NVN_REPLAY_DIR)/rp_state.c $(NVN_REPLAY_DIR)/rp_entry.c $(NVN_REPLAY_DIR)/rp_res.c $(NVN_REPLAY_DIR)/rp_buf.c $(NVN_REPLAY_DIR)/rp_pass.c $(NVN_REPLAY_DIR)/rp_present.c $(NVN_REPLAY_DIR)/rp_draw.c $(NVN_REPLAY_DIR)/nvn_unimpl.c \
  graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/tex/nvn_deswizzle.c
NVN_REPLAY_SCAN = perl -0pe 's\#/\*.*?\*/\#\#gs; s\#//[^\n]*\#\#g' $(NVN_REPLAY_DIR)/rp_*.c $(NVN_REPLAY_DIR)/rp_*.h $(NVN_REPLAY_DIR)/nvn_unimpl.c $(NVN_REPLAY_DIR)/nvn_unimpl.h $(NVN_REPLAY_DIR)/nvn_plat.h
.PHONY: native-test-nvn_replay
native-test-nvn_replay:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_replay graphics/nvn/tests/t_nvn_replay.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) -lm
	@bad=$$($(NVN_REPLAY_SCAN) | grep -nE '\b(MTL[A-Za-z0-9_]*|CAMetal[A-Za-z]*|Vk[A-Z][A-Za-z0-9]*|VK_[A-Z0-9_]+|vk[A-Z][A-Za-z]*|ANativeWindow|AHardwareBuffer|OHNativeWindow|__system_property_get|getenv)\b|#(include|import) *[<"](Metal|QuartzCore|vulkan|android|jni|hilog)' || true); \
	  if [ -n "$$bad" ]; then echo "【红】 回放核心出现图形 API / 平台专有内容（注释外）：$$bad"; exit 1; fi; \
	  echo "分层扫描：回放核心注释外图形 API / 平台专有 0 处"
	timeout 30 $(BUILD)/tests/t_nvn_replay docs/直连_Vulkan回放接线表.tsv
	@echo '✓ t_nvn_replay：回放核心状态继承 / 未接命令报名 / 一次提交一段 / 完成后回写 / 接线表与名字表一致 / 分层扫描'
TEST_TARGETS += native-test-nvn_replay

.PHONY: native-test-nvn_wait_sync
native-test-nvn_wait_sync:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_wait_sync graphics/nvn/tests/t_nvn_wait_sync.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) -lm
	timeout 30 $(BUILD)/tests/t_nvn_wait_sync
TEST_TARGETS += native-test-nvn_wait_sync

.PHONY: native-test-nvn_clear_tex
native-test-nvn_clear_tex:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_clear_tex graphics/nvn/tests/t_nvn_clear_tex.c $(NVN_REPLAY_CORE) -lm
	timeout 30 $(BUILD)/tests/t_nvn_clear_tex
TEST_TARGETS += native-test-nvn_clear_tex
