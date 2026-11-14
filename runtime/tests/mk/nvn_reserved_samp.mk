# native-test-nvn_reserved_samp：本单测的构建规则（采样器池保留号 0 / 1：照驱动在池初始化时登记）（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += nvn_reserved_samp
ifeq ($(shell uname -s),Darwin)
$(BUILD)/tests/t_nvn_reserved_samp: graphics/nvn/tests/t_nvn_reserved_samp.c graphics/nvn/gfx/nvn/nvn_protos.h graphics/nvn/gfx/nvn/include/nvn.h $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) $(NATIVE_INC) -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include \
	  -o $@ graphics/nvn/tests/t_nvn_reserved_samp.c $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_reserved_samp
native-test-nvn_reserved_samp: $(BUILD)/tests/t_nvn_reserved_samp
	$(BUILD)/tests/t_nvn_reserved_samp $(BUILD)/tests
TEST_TARGETS += native-test-nvn_reserved_samp
endif
