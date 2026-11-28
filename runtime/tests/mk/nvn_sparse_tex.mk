# native-test-nvn_sparse_tex：虚拟池中采样贴图的内容来源（graphics/nvn/replay/rp_res.c sparse_*）。
# 与 t_nvn_replay 共用同一份替身（重命名它的 main 后 include）；页表 / 物理页写代号 / 映射日志在单测里用替身。
# 期望值走独立路径：同样的字节放进普通池里的同形贴图，走原有的 CPU 贴图上传，两边逐级逐层字节逐字相同。
# 负对照写在目标里：环境变量 T_NVN_SPARSE_OFF=1 把开关 nvk_sparse_tex 关掉（走改动前的行为），再跑必须变红；仍全绿 = 用例没有真的在测它，本目标报红。
.PHONY: native-test-nvn_sparse_tex
native-test-nvn_sparse_tex:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -I$(NVN_REPLAY_DIR) \
	  -o $(BUILD)/tests/t_nvn_sparse_tex graphics/nvn/tests/t_nvn_sparse_tex.c graphics/nvn/tests/nvn_clear_view_unused.c $(NVN_REPLAY_CORE) $(NVN_REPLAY_WEAK) -lm
	timeout 60 $(BUILD)/tests/t_nvn_sparse_tex
	@! T_NVN_SPARSE_OFF=1 timeout 60 $(BUILD)/tests/t_nvn_sparse_tex >/dev/null 2>&1 || { echo "【红】 负对照：关掉 nvk_sparse_tex 后单测仍全绿，说明用例没有真的在测它"; exit 1; }
	@echo '✓ t_nvn_sparse_tex：虚拟池里的压缩和带 SPARSE 标志的采样贴图按页表读（没映射的页 0）/ 写或重映射才重传 / 其他普通路径保留 / 超限拒绝 / 关开关即红'
TEST_TARGETS += native-test-nvn_sparse_tex
