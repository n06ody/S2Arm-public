# nn::ro 懒绑定交接（#858）：完整原生生产对象 + 出包 C 改写器产物，合成模块按 SDK nn::ro 布局。
NATIVE_TEST_OWN += ro_lazy
ifeq ($(shell uname -s),Darwin)
$(BUILD)/tests/t_ro_lazy: runtime/tests/t_ro_lazy.c $(NVN_TEST_HOST_OBJS)
	$(CC) $(CFLAGS) $(NATIVE_INC) -o $@ $< $(NVN_TEST_HOST_OBJS) $(S2A_HOST_WEAK)
.PHONY: native-test-ro_lazy
native-test-ro_lazy: $(BUILD)/tests/t_ro_lazy
	python3 runtime/tests/ro_lazy/prepare.py $(BUILD)/tests/ro_lazy
	$(BUILD)/tests/t_ro_lazy $(BUILD)/tests/ro_lazy
	python3 runtime/tests/ro_lazy/prepare.py $(BUILD)/tests/ro_lazy_old old   # 老 SDK 形态（单数 RestoreFunction、+0xb0）
	$(BUILD)/tests/t_ro_lazy $(BUILD)/tests/ro_lazy_old
	python3 runtime/tests/ro_lazy/prepare.py $(BUILD)/tests/ro_lazy_early early   # 新 SDK 早退在前的块布局（RestoreFunctions 第 2 条就是 ret）
	$(BUILD)/tests/t_ro_lazy $(BUILD)/tests/ro_lazy_early
TEST_TARGETS += native-test-ro_lazy
endif
