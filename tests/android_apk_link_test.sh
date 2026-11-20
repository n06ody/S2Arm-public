#!/usr/bin/env bash
# 门禁只看：android/CMakeLists.txt android/native/* android/core-link/* android/ctest-link/* android/tools/ndk_toolchain.sh runtime/src/* runtime/include/* graphics/nvn/* graphics/dsx/*
# 门禁本机：要 NDK 29 与 CMake / ninja（gate-host 没有）；维护方本机名单配了 S2A_ANDROID_LINK_REMOTE 时整步送安卓专用 Mac跑
# 安卓 APK 单库链接核对（android/ctest-link/ctest_link.py --parts apk：NDK 工具链按 android/CMakeLists.txt 七种组合真编真链 libs2arm_android.so，含游戏 / 诊断 nvk，
#   每个未定义符号打【红】并给「文件:行」；不出整包、不装机）。起因 11-13：#871 在共用的 graphics 前端 hle_nvn.c 调了只有苹果宿主才定义的
#   dm_shader_mesh_hold / _key，安卓 APK 链接不过，列车不编 APK 没拦住，合入后所有安卓出包都失败。改到共用 C 的支路由命中就跑。
#   缺 NDK / CMake 的机器（贡献者）退 75「环境未就绪」，不判红。
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
. "$H/tools/machine.sh" 2> /dev/null || true
if [ -n "${S2A_ANDROID_LINK_REMOTE:-}" ] && [ -z "${S2A_REMOTE_NAME:-}" ] && [ -f "${S2A_PRIVATE_TOOLS:-}/remote.sh" ] && [ -z "$(git -C "$H" status --porcelain 2> /dev/null)" ]; then
  cd "$H"
  # shellcheck disable=SC2086  # S2A_ANDROID_LINK_ENV 是「K=V K=V」一串，按空格拆给 env
  rc=0; env S2A_REMOTE_PRIO=2 bash "$S2A_PRIVATE_TOOLS/remote.sh" --host "$S2A_ANDROID_LINK_REMOTE" --name gate-android-link --rev HEAD --timeout 900 -- \
    bash -c '( while sleep 60; do echo "安卓 APK 链接核对在远端跑着（$(date +%T)）"; done ) & hb=$!; trap "kill $hb 2> /dev/null" EXIT; env "$@" bash tests/android_apk_link_test.sh' _ ${S2A_ANDROID_LINK_ENV:-} || rc=$?
  # 远端自己没跑成（remote.sh 退 125：盘不足 / 连不上 / 排队超时）≠ 用例红：退 75 环境未就绪，列车等环境、不归责分支（11-14：远端机盘只剩 16G，contrib/pr1013 被当真红退回，同批 9 支逐支核、叠在上面的 10 支推测批作废，约 40 分钟）
  [ $rc = 125 ] && { echo "安卓 APK 链接核对：环境未就绪（远端没跑成：remote 退出 125，原因见上面的【红】行）"; exit 75; }; exit $rc
fi
. "$H/android/tools/ndk_toolchain.sh" 2> /dev/null && s2a_ndk_resolve > /dev/null 2>&1 || { echo "安卓 APK 链接核对：环境未就绪（找不到 NDK 29，设 ANDROID_NDK_HOME）"; exit 75; }
command -v cmake > /dev/null && command -v ninja > /dev/null || { echo "安卓 APK 链接核对：环境未就绪（PATH 上没有 cmake / ninja）"; exit 75; }
exec python3 "$H/android/ctest-link/ctest_link.py" --parts apk
