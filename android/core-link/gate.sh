#!/usr/bin/env bash
# 门禁路由：services/ compat/tree/src/ android/cs/ graphics/metal/cs/ graphics/tools/direct/compiler/ graphics/tools/direct/dsh/ translator/dmsh_xlate/ graphics/widen/shader/ graphics/widen/SpirvCbufUseText.cs
# 门禁路由：runtime/src/ graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_dirty.c graphics/nvn/gfx/nvn/nvn_first_use.c graphics/nvn/gfx/tex/nvn_deswizzle.c
# 安卓单库链接符号核对（#530 第一层）的共用代码入口：改编进单库的共用 C#（services/、compat/tree/src/、graphics/metal/cs/ 等）
# 或共用原生源（runtime/src/ 等，定义侧）时安卓门禁本身不跑（安卓门禁自己的路由只看 android/ 等），
# #529 的 s2a_alt_phase / s2a_gfx_mode 就是这样 3 天没人发现。触发路径以本文件头的「# 门禁路由：」为准，t_link_symbols.py 核对两侧都不漏。
# 这里只跑源码级核对（core-link/link_symbols.py --no-link，诊断 + 游戏两种目标，秒级；要 Python 3 + CMake）。
# 真实链接探针和本工具的单测照旧在 android/gate.sh（test.sh）里跑；整包链接是 apk 子项（#779）。
set -euo pipefail
cd "$(cd "$(dirname "$0")/../.." && pwd)"   # 仓库根
command -v cmake >/dev/null 2>&1 || { echo "🔴 安卓链接符号核对要 CMake 读 core-link/LinkSources.cmake；不跳过、不自动下载" >&2; exit 1; }
if python3 android/core-link/link_symbols.py --no-link; then
  echo "安卓单库链接符号核对（源码级，诊断 + 游戏）：期望 缺失 0、重复 0 → 真结果 通过"
else
  echo "安卓单库链接符号核对（源码级，诊断 + 游戏）：期望 缺失 0、重复 0 → 真结果 红（见上）"
  exit 1
fi
