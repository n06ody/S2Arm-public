#!/usr/bin/env bash
# 首次使用准备下沉：预热调度（nvn_warm.c）的时序合同（宿主，确定性，无 GPU）。
#   ① 先程序、后管线：程序清单（含在途重试）走完才取管线；
#   ② 在途重试：build 返回 1 的条目会被重试并最终发布，不丢；
#   ③ 计数：失败条目计入 failed、不阻塞其它条目；
#   ④ Stop（快落盘）：返回 0、线程退出、两个 owner 的清单都落盘、后台静默被调用；
#   ⑤ Stop（慢落盘 3 秒）：返回 1、记「未完成」、耗时远小于这次落盘——落盘归预热线程，交互线程绝不做文件 IO；
#      到点后后台继续写完（旧文件在此之前保留）；
#   ⑥ 没有预热线程（开关关 / 线程起不来）：落盘归一次性 saver，Stop 同样有界；
#   ⑦ 负对照：把持久化挪回 Stop 调用线程 → ⑤ 必须变红（复现复核的 900.1 毫秒）。
# 蓝本：graphics/nvn/gfx/nvn/nvn_mtl.m warm_start / pso_pend（后台清单预热、首帧不等）；合同见 nvn_warm.h。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
android="$repo/android"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
CFLAGS=(-std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE
  -I "$android/native" -I "$android/native/nvn"
  -I "$repo/graphics/nvn/replay" -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include")
cc_bin="${CC:-cc}"
"$cc_bin" "${CFLAGS[@]}" -o "$tmp/t" "$android/tests/nvn_warm_test.c" "$android/native/nvn/nvn_warm.c"
"$tmp/t"                                     # ① ② ③ ④（默认：快落盘）
NVK_WARM_TEST_REACTIVE=1 "$tmp/t"
NVK_WARM_OFF=1 "$tmp/t"                      # ⑦ 开关关 + 无预热线程时的快落盘（saver）
NVK_WARM_SLOW_SAVE_US=3000000 "$tmp/t"        # ⑤ 慢落盘：Stop 必须有界
NVK_WARM_OFF=1 NVK_WARM_SLOW_SAVE_US=3000000 "$tmp/t"   # ⑥ 没有预热线程 + 慢落盘

# ---- 负对照：持久化挪回 Stop 调用线程（复核报的形态）----
# 在 nvk_warm_stop 入口直接落盘并置完成标志：落盘就成了交互线程上的同步文件 IO，⑤ 必须变红。
python3 - "$android/native/nvn/nvn_warm.c" "$tmp/nvn_warm_neg.c" <<'PY'
import sys, pathlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = src.read_text()
# 锚点一：在 Stop 入口做同步落盘（复核报的形态）。
needle_stop = "int nvk_warm_stop(void) {\n"
assert s.count(needle_stop) == 1, "负对照锚点失效：nvk_warm_stop 的定义行不唯一（改结构后要同步这里）"
s = s.replace(needle_stop, needle_stop + '    { warm_persist(1, "负对照"); atomic_store(&g_saved, 1); }   /* 负对照：持久化挪回 Stop 线程 */\n', 1)
# 锚点二：预热线程退出路径不再落盘（否则它还会在后台再写一遍，把耗时叠成两倍，读数对不上复核的 900 毫秒）。
needle_exit = "    warm_progress(\"收工（Stop）\");\n    warm_persist_stop();\n"
assert s.count(needle_exit) == 1, "负对照锚点失效：warm_exit_stop 里的落盘调用不唯一（改结构后要同步这里）"
s = s.replace(needle_exit, "    warm_progress(\"收工（Stop）\");\n    atomic_store(&g_saved, 1);   /* 负对照：退出路径不落盘 */\n", 1)
dst.write_text(s)
PY
"$cc_bin" "${CFLAGS[@]}" -o "$tmp/neg" "$android/tests/nvn_warm_test.c" "$tmp/nvn_warm_neg.c"
ulimit -c 0 || true   # 负对照会 abort（断言失败），不要在门禁机上留 core
set +e
( NVK_WARM_SLOW_SAVE_US=3000000 "$tmp/neg" > "$tmp/neg.out" 2>&1 ) 2>/dev/null   # 外层 2>/dev/null：吞掉 shell 的 "Aborted" 行
neg_rc=$?
set -e
if [ "$neg_rc" -eq 0 ]; then
  cat "$tmp/neg.out"
  echo "【红】负对照没有变红：把持久化挪回 Stop 线程后，慢落盘用例仍然通过（用例没有守住 Stop 预算）"
  exit 1
fi
echo "负对照红（预期），慢落盘用例的断言落在了 Stop 预算上："
grep -E 'Stop 耗时|Assertion' "$tmp/neg.out" | head -3 || true
