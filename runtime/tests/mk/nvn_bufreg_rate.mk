# native-test-nvn_bufreg_rate：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += nvn_bufreg_rate
ifeq ($(S2A_UNAME),Darwin)
# 缓冲建删增长率（hle_nvn.c nvnBufferInitialize / Finalize 的活缓冲登记，11-07 时之笛 30 → 5 帧）：N 与 4N 个缓冲耗时比 ≤ 8，单线程 / 4 线程各一遍；不设绝对耗时上限；缺省 = 安卓路线（强制登记、查长度全对），metal = Metal 路线（零登记）
# 只替换真实前端的能力查询这一条边。导出 nvk 的真实定义，丢弃同编译单元的其他回放符号，避免和宿主的 Metal 后端冲突。
NVN_BUFREG_HOOK := nvk_test_buf_extent_consumer
$(NVN_TEST_DIR)/hle_nvn_bufreg_nvk.o: graphics/nvn/gfx/nvn/hle_nvn.c $(NVN_TEST_LIB)
	$(CC) $(CFLAGS) -std=gnu11 $(NATIVE_INC) -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/host \
	  -Dnvn_be_buf_extent_consumer=$(NVN_BUFREG_HOOK) -c $< -o $@
$(BUILD)/tests/nvk-test-consumer.dylib: graphics/nvn/replay/rp_entry.c $(wildcard graphics/nvn/replay/*.h) $(NVN_MTL_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex \
	  -Dnvn_be_buf_extent_consumer=$(NVN_BUFREG_HOOK) -dynamiclib -Wl,-dead_strip -Wl,-exported_symbol,_$(NVN_BUFREG_HOOK) \
	  -install_name @loader_path/$(notdir $@) $< -o $@
$(BUILD)/tests/t_nvn_bufreg_rate $(BUILD)/tests/t_nvn_bufreg_nvk: graphics/nvn/tests/t_nvn_bufreg_rate.m graphics/nvn/gfx/nvn/nvn_protos.h graphics/nvn/gfx/nvn/include/nvn.h $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) -fobjc-arc $(NATIVE_INC) -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include \
	  -o $@ $< $(filter %/hle_nvn_bufreg_nvk.o %/nvk-test-consumer.dylib,$^) $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
$(BUILD)/tests/t_nvn_bufreg_nvk: $(NVN_TEST_DIR)/hle_nvn_bufreg_nvk.o $(BUILD)/tests/nvk-test-consumer.dylib
.PHONY: native-test-nvn_bufreg_rate
native-test-nvn_bufreg_rate: $(BUILD)/tests/t_nvn_bufreg_rate $(BUILD)/tests/t_nvn_bufreg_nvk
	$(BUILD)/tests/t_nvn_bufreg_rate
	$(BUILD)/tests/t_nvn_bufreg_rate metal
	$(BUILD)/tests/t_nvn_bufreg_nvk nvk
	$(BUILD)/tests/t_nvn_bufreg_nvk nvk-forced
TEST_TARGETS += native-test-nvn_bufreg_rate
endif
