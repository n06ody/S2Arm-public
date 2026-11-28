# native-test-nvn_prog：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += nvn_prog
ifeq ($(S2A_UNAME),Darwin)
# Program 扩展入口（#270 N12）：子程序链接显式失败、逐样本着色模式、子块尺寸 0。
$(BUILD)/tests/t_nvn_prog: graphics/nvn/tests/t_nvn_prog.c graphics/nvn/gfx/nvn/nvn_protos.h graphics/nvn/gfx/nvn/nvn_obj.h $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) $(NATIVE_INC) -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include \
	  -o $@ graphics/nvn/tests/t_nvn_prog.c $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_prog
native-test-nvn_prog: $(BUILD)/tests/t_nvn_prog
	timeout 60 $(BUILD)/tests/t_nvn_prog
TEST_TARGETS += native-test-nvn_prog
endif
