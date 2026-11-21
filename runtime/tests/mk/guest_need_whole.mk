# native-test-guest_need_whole：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += guest_need_whole
ifeq ($(S2A_UNAME),Darwin)
# 整段进文件时心跳记需求只扫游戏放开过的块（vawin.c g_gtouch，#1842 后续）
.PHONY: native-test-guest_need_whole
native-test-guest_need_whole: $(TEST_VAGUARD)
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) $(NATIVE_INC) -DS2A_UNIT_TEST -o $(BUILD)/tests/t_guest_need_whole runtime/tests/t_guest_need_whole.c runtime/src/vawin.c -Wl,-weak_library,$(TEST_VAGUARD)
	$(BUILD)/tests/t_guest_need_whole
TEST_TARGETS += native-test-guest_need_whole
endif
