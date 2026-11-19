# native-test-nvn_replay：Vulkan 回放核心（graphics/nvn/replay，docs/设计_Vulkan回放层重搭.md）的宿主单测。
# 纯 C：后端（nvn_gpu_*）与平台件（nvn_plat_*）在单测里用替身，不需要 GPU。
#   1. C11 -Werror 编译并运行；接线表 docs/直连_Vulkan回放接线表.tsv 的值与命令名逐行对名字表；
#   2. 分层扫描：回放核心（rp_*.c / rp_*.h / nvn_unimpl.* / nvn_plat.h）注释外不许出现 Vulkan / Metal / 安卓 / 鸿蒙的头文件与类型，
#      也不许直接读系统属性或环境变量（开关只走 nvn_plat_flag）。nvn_plat_vk.h 是给后端的，不在扫描范围。
NVN_REPLAY_DIR := graphics/nvn/replay
NVN_REPLAY_CORE := $(NVN_REPLAY_DIR)/rp_exec.c $(NVN_REPLAY_DIR)/rp_copy_t2b.c $(NVN_REPLAY_DIR)/rp_clear_tex.c $(NVN_REPLAY_DIR)/rp_state.c $(NVN_REPLAY_DIR)/rp_entry.c $(NVN_REPLAY_DIR)/rp_res.c $(NVN_REPLAY_DIR)/rp_buf.c $(NVN_REPLAY_DIR)/rp_pass.c $(NVN_REPLAY_DIR)/rp_present.c $(NVN_REPLAY_DIR)/rp_draw.c $(NVN_REPLAY_DIR)/nvn_unimpl.c \
  graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/tex/nvn_deswizzle.c
# 回放核心对平台 / 后端的可选件写的是弱引用（缺实现 = 地址为空、走不到调用）。ELF 链接器认未定义弱符号，苹果 ld 默认不认：
#   逐个列名允许缺，不用 dynamic_lookup 一刀切——真漏掉的强符号照样链接红。放行清单只从源码登记生成（「// 弱链接[replay]：符号」，见 tools/weak_link_decl.py）
ifeq ($(shell uname -s),Darwin)
NVN_REPLAY_WEAK = $(shell python3 tools/weak_link_decl.py flags replay)   # 用到才求值（= 不是 :=）：本文件随 runtime 总 make 引入，iOS 出壳也会读到这行；立即求值会让出壳去扫安卓源码（壳输入表外 112 个文件）
endif
NVN_REPLAY_SCAN = perl -0pe 's\#/\*.*?\*/\#\#gs; s\#//[^\n]*\#\#g' $(NVN_REPLAY_DIR)/rp_*.c $(NVN_REPLAY_DIR)/rp_*.h $(NVN_REPLAY_DIR)/nvn_unimpl.c $(NVN_REPLAY_DIR)/nvn_unimpl.h $(NVN_REPLAY_DIR)/nvn_plat.h
.PHONY: native-test-nvn_replay
native-test-nvn_replay:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -DS2A_TEST_TS_ALLOC -DS2A_GPU_LAND_TEST -DS2A_TEST_TREG_SCRUB -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_replay graphics/nvn/tests/t_nvn_replay.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) $(NVN_REPLAY_WEAK) -lm
	@bad=$$($(NVN_REPLAY_SCAN) | grep -nE '\b(MTL[A-Za-z0-9_]*|CAMetal[A-Za-z]*|Vk[A-Z][A-Za-z0-9]*|VK_[A-Z0-9_]+|vk[A-Z][A-Za-z]*|ANativeWindow|AHardwareBuffer|OHNativeWindow|__system_property_get|getenv)\b|#(include|import) *[<"](Metal|QuartzCore|vulkan|android|jni|hilog)' || true); \
	  if [ -n "$$bad" ]; then echo "【红】 回放核心出现图形 API / 平台专有内容（注释外）：$$bad"; exit 1; fi; \
	  echo "分层扫描：回放核心注释外图形 API / 平台专有 0 处"
	timeout 30 $(BUILD)/tests/t_nvn_replay docs/直连_Vulkan回放接线表.tsv
	timeout 30 $(BUILD)/tests/t_nvn_replay --sync-reuse
	timeout 30 $(BUILD)/tests/t_nvn_replay --dirtylog-off docs/直连_Vulkan回放接线表.tsv   # 开关 nvk_buf_dirtylog 关那一档整套再跑一遍：逐页退回路径
	@echo '✓ t_nvn_replay：回放核心状态继承 / 未接命令报名 / 一次提交一段 / 完成后回写 / 接线表与名字表一致 / 分层扫描'
TEST_TARGETS += native-test-nvn_replay

# native-test-nvn_res：资源池与绑定构造的省重（贴图内容纪元 / 整组绑定沿用 / 驱动段暂存按内容命中 / 换管线按布局立脏）。
# 与 t_nvn_replay 共用同一份替身（重命名它的 main 后 include）。
.PHONY: native-test-nvn_res
native-test-nvn_res:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_res graphics/nvn/tests/t_nvn_res.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) $(NVN_REPLAY_WEAK) -lm
	timeout 30 $(BUILD)/tests/t_nvn_res
	timeout 30 $(BUILD)/tests/t_nvn_res --dirtylog-off   # 开关 nvk_buf_dirtylog 关：贴图核对走整张逐页比（同修前）
	@echo '✓ t_nvn_res：贴图内容按纪元只查一次（换段重查 / 写后重传 / WriteTexels 强制作废）/ 连续同参数绘制第二笔整组沿用 / 驱动段暂存按内容命中 / 换管线按布局立脏'
TEST_TARGETS += native-test-nvn_res

# native-test-nvn_tm：分时读数抽样（绘制 / 开通道 / 贴图解析的逐步计时每 N 帧只完整计 1 帧，读数按计时过的帧数求每帧值；#1103）。
# 与 t_nvn_replay 共用同一份替身（重命名它的 main 后 include）；Linux 上另用假时钟数取时钟次数、核对抽样与全量打出的读数逐字相同。
.PHONY: native-test-nvn_tm
native-test-nvn_tm:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_tm graphics/nvn/tests/t_nvn_tm.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) $(NVN_REPLAY_WEAK) -lm
	timeout 30 $(BUILD)/tests/t_nvn_tm
	@echo '✓ t_nvn_tm：逐步计时按帧抽样（开关缺省每 32 帧计 1 帧 / 夹取 / nvk_tm_full 压过 / 会话重开重读；采样帧数数得对；非采样帧不取时钟；抽样与全量打出的三行读数逐字相同）'
TEST_TARGETS += native-test-nvn_tm

.PHONY: native-test-nvn_wait_sync
native-test-nvn_wait_sync:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_wait_sync graphics/nvn/tests/t_nvn_wait_sync.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) $(NVN_REPLAY_WEAK) -lm
	timeout 30 $(BUILD)/tests/t_nvn_wait_sync
TEST_TARGETS += native-test-nvn_wait_sync

.PHONY: native-test-nvn_copy_t2b
native-test-nvn_copy_t2b:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_copy_t2b graphics/nvn/tests/t_nvn_copy_t2b.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) $(NVN_REPLAY_WEAK) -lm
	timeout 30 $(BUILD)/tests/t_nvn_copy_t2b
TEST_TARGETS += native-test-nvn_copy_t2b

.PHONY: native-test-nvn_clear_tex
native-test-nvn_clear_tex:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_clear_tex graphics/nvn/tests/t_nvn_clear_tex.c $(NVN_REPLAY_CORE) $(NVN_REPLAY_WEAK) -lm
	timeout 30 $(BUILD)/tests/t_nvn_clear_tex
TEST_TARGETS += native-test-nvn_clear_tex
