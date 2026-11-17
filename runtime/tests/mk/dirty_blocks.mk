# 写跟踪摘要：真实写保护、Apple 逐页路径与并发发布。
NATIVE_TEST_OWN += dirty_blocks
.PHONY: native-test-dirty_blocks
native-test-dirty_blocks:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -DS2A_DIRTY_BLOCK_TEST=1 -Wall -Wextra -Werror -Iruntime/include -o $(BUILD)/tests/t_dirty_blocks graphics/nvn/tests/t_dirty_blocks.c graphics/nvn/gfx/nvn/nvn_dirty.c -lpthread
	timeout 30 $(BUILD)/tests/t_dirty_blocks
TEST_TARGETS += native-test-dirty_blocks
