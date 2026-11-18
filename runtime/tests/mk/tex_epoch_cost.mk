# 贴图内容纪元在「无关写」下的真实代价（真 nvn_dirty.c）：现状每笔每张整张逐页比；脏页日志游标原型只读事件。耗时只打印。
NATIVE_TEST_OWN += tex_epoch_cost
.PHONY: native-test-tex_epoch_cost
native-test-tex_epoch_cost:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Iruntime/include -o $(BUILD)/tests/t_tex_epoch_cost graphics/nvn/tests/t_tex_epoch_cost.c graphics/nvn/gfx/nvn/nvn_dirty.c -lpthread
	timeout 60 $(BUILD)/tests/t_tex_epoch_cost
TEST_TARGETS += native-test-tex_epoch_cost
