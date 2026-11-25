# native-test-dirty_chunk：分块保护反例（共块共享、跨块写入、保护失败保守路径、mprotect 区间块对齐）
NATIVE_TEST_OWN += dirty_chunk
.PHONY: native-test-dirty_chunk
native-test-dirty_chunk:   # 写跟踪分块保护：不需要 GPU / 游戏
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -Iruntime/include -o $(BUILD)/tests/t_dirty_chunk graphics/nvn/tests/t_dirty_chunk.c graphics/nvn/gfx/nvn/nvn_dirty.c
	$(BUILD)/tests/t_dirty_chunk
	$(CC) $(CFLAGS) -DS2A_LAND_PROBE_TEST -Iruntime/include -o $(BUILD)/tests/t_land_probe graphics/nvn/tests/t_land_probe.c graphics/nvn/gfx/nvn/nvn_dirty.c -lpthread
	$(BUILD)/tests/t_land_probe
TEST_TARGETS += native-test-dirty_chunk
