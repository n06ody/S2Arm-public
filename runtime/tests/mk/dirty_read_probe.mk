ifeq ($(S2A_UNAME),Darwin)
NATIVE_TEST_OWN += dirty_read_probe
.PHONY: native-test-dirty_read_probe
native-test-dirty_read_probe:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -Iruntime/include -o $(BUILD)/tests/t_dirty_read_probe graphics/nvn/tests/t_dirty_read_probe.c graphics/nvn/gfx/nvn/nvn_dirty.c -lpthread
	$(BUILD)/tests/t_dirty_read_probe
TEST_TARGETS += native-test-dirty_read_probe
endif
