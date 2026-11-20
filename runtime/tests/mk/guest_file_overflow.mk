# native-test-guest_file_overflow：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += guest_file_overflow
ifeq ($(S2A_UNAME),Darwin)
# 客体内存文件后备溢出模式（vawin.c s2a_guest_commit）：预算内的块匿名、超出的块第一次放开时换文件；还页 / 挖区 / 窗口重置按块状态处理
.PHONY: native-test-guest_file_overflow
native-test-guest_file_overflow: $(TEST_VAGUARD)
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) $(NATIVE_INC) -DS2A_UNIT_TEST -o $(BUILD)/tests/t_guest_file_overflow runtime/tests/t_guest_file_overflow.c runtime/src/vawin.c -Wl,-weak_library,$(TEST_VAGUARD)
	$(BUILD)/tests/t_guest_file_overflow
TEST_TARGETS += native-test-guest_file_overflow
endif
