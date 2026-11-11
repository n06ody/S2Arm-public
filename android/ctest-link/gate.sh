#!/usr/bin/env bash
# 门禁路由：runtime/src/ runtime/include/ graphics/track/ platform/plat.h platform/plat_asm.h runtime/tests/t_crash_android_fatal.c
# 门禁路由：android/CMakeLists.txt android/native/ android/tests/ android/w4-tests/ android/core-link/ android/ctest-link/ android/codecs/ android/cs/savecommit/tests/native_host_test.c android/cs/savecommit/tests/open_read_host_test.c
# 门禁路由：graphics/nvn/gfx/nvn/ graphics/nvn/gfx/tex/ graphics/nvn/host/ graphics/nvn/third_party/vulkan-headers/ graphics/nvn/lib_pack_core.inc
# 门禁本机：* 要 cmake + ninja + NDK 29：gate-host 的 /opt/homebrew/bin 没有 cmake / ninja（10-30 实测），本机都有；秒级
# 安卓链接符号核对（合入前层）：NDK 交叉真链接 android/CMakeLists.txt 在 Linux aarch64 配置下的全部 CTest 可执行目标，
#   加安卓单库 libs2arm_android.so 五种配置（诊断 / 游戏 × 直连前端关 / nvn_vk，外加 Gradle 缺省的诊断构建 #557，真 -Werror、-z defs，C# 一侧用 link_symbols 探针；起因 #1086 nvn_be_tex_tick、#902 / 9d6d81f2f 游戏模式编不过，都只在出 APK 时暴露），
#   局限（10-30 维护者要求写在钩子头）：链的是 NDK 自带的 bionic libc，不是 CTest 实际用的 glibc——glibc 有而 bionic 没有的函数会误报红、反之漏报；去掉了 -Werror，告警不在本层核；以合入后层的真 Linux CTest（android/gate.sh linux 子项）为准
#   缺定义 / 缺替身的外部符号逐个打【红】（符号、引用文件:行、目标）。起因 #1003（svc_mem.c 新引用 s2a_android_guest_writers_* 无桩，41 项 CTest 全 Not Run）与 10-28 同类。
#   x86_64 编译检查（11-01）：同一份 CTest 配置换 x86_64 目标只编不链、-Werror 保留——门禁机器都是 arm64，x86_64 宿主才有的编译错（7360dfa77「定义未使用」）原先只有贡献者自检能发现。约 3 秒。
#   完整 CTest（编译 + 跑，colima）仍是 android/gate.sh 的 linux 子项、合入后层。路由由本核对自检：编译读到的仓库文件没被上面三行覆盖就报红。
set -uo pipefail
cd "$(cd "$(dirname "$0")/../.." && pwd)"   # 仓库根
if python3 android/ctest-link/ctest_link.py --parts ctest,apk,ctest-x86 --route-hook android/ctest-link/gate.sh; then
  echo "安卓链接符号核对（Linux CTest + 单库 + x86_64 编译）：期望 全部目标链过、x86_64 -Werror 编过、路由无漏项 → 真结果 通过"
else
  echo "安卓链接符号核对（Linux CTest + 单库 + x86_64 编译）：期望 全部目标链过、x86_64 -Werror 编过、路由无漏项 → 真结果 红（见上）"
  exit 1
fi
