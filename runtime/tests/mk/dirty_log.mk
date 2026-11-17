# 脏页追加日志（#1501 A②）：真实写保护、缺页回调、宿主写前准备与「事件先于页变可写」的交错。
NATIVE_TEST_OWN += dirty_log
.PHONY: native-test-dirty_log
native-test-dirty_log:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Iruntime/include -o $(BUILD)/tests/t_dirty_log graphics/nvn/tests/t_dirty_log.c graphics/nvn/gfx/nvn/nvn_dirty.c -lpthread
	timeout 30 $(BUILD)/tests/t_dirty_log
TEST_TARGETS += native-test-dirty_log
