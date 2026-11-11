#!/usr/bin/env bash
# nvn_vk_poolahb.c 的宿主单测（AHB 池准入 + 迁移事务 + 单调提交序号退休语义）。
#
# 把真实实现编进测试：nvn_vk_poolahb.c + vk_resid.c + poolahb_cover.c + poolahb_policy.c +
# poolahb_alias.c。替身只给外部依赖（Vulkan 缓冲、适配层池表、tracking 合同、脏页跟踪），
# 都在 tests/vk_poolahb_core_test.c 里定义。
#
# -D__ANDROID__：生产实现的 s2a_poolahb_enabled() 在非 Android 宿主上恒为 0（见其 #else 分支），
# 不开这个宏连 init 的准入路径都进不去。Android 系统头由 tests/hostvk 下的宿主桩提供。
# 池地址用真实匿名 mmap：poolahb_cover_check_path 读真实 /proc/self/maps。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
android="$repo/android"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cc_bin="${CC:-cc}"
# 独立池夹具不链接记录器；按源码登记放行 macOS 宿主可空导入。
weak_u=(); [ "$(uname -s)" = Darwin ] && read -r -a weak_u <<< "$(python3 "$repo/tools/weak_link_decl.py" flags poolahb_test)"
"$cc_bin" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -D__ANDROID__ \
  -I "$android/native" -I "$android/native/direct" -I "$android/native/nvn" \
  -I "$android/tests/vkhost" -I "$android/tests/hostvk" \
  -I "$repo/graphics/nvn/third_party/vulkan-headers/include" \
  -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" \
  -I "$repo/graphics/nvn" -I "$repo/graphics/nvn/host" \
  -o "$tmp/vk_poolahb_core_test" ${weak_u[@]+"${weak_u[@]}"} \
  "$android/tests/vk_poolahb_core_test.c" \
  "$android/native/nvn/nvn_vk_poolahb.c" \
  "$android/native/direct/vk_resid.c" \
  "$android/native/nvn/poolahb_cover.c" \
  "$android/native/nvn/poolahb_policy.c" \
  "$android/native/nvn/poolahb_alias.c"
# “quiesce 入口不存在”只可能出现在链接层面，所以单独出一份二进制模拟（弱符号解析为 NULL）。
# Mach-O 不许留未定义的弱符号：macOS 门禁机上的放行参数按源码「弱链接[poolahb_test]」登记生成（不手写）
"$cc_bin" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -D__ANDROID__ \
  -DVK_POOLAHB_TEST_NO_QUIESCE_ENTRY \
  -I "$android/native" -I "$android/native/direct" -I "$android/native/nvn" \
  -I "$android/tests/vkhost" -I "$android/tests/hostvk" \
  -I "$repo/graphics/nvn/third_party/vulkan-headers/include" \
  -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" \
  -I "$repo/graphics/nvn" -I "$repo/graphics/nvn/host" \
  -o "$tmp/vk_poolahb_core_test_noquiesce" ${weak_u[@]+"${weak_u[@]}"} \
  "$android/tests/vk_poolahb_core_test.c" \
  "$android/native/nvn/nvn_vk_poolahb.c" \
  "$android/native/direct/vk_resid.c" \
  "$android/native/nvn/poolahb_cover.c" \
  "$android/native/nvn/poolahb_policy.c" \
  "$android/native/nvn/poolahb_alias.c"

# 准入的禁用状态会被缓存进 g_pa_on，两个禁用分支必须各起一个进程。
"$tmp/vk_poolahb_core_test" core
"$tmp/vk_poolahb_core_test" admission-ok
"$tmp/vk_poolahb_core_test" admission-ahb0
"$tmp/vk_poolahb_core_test" admission-importoff
"$tmp/vk_poolahb_core_test_noquiesce" owner-window-no-quiesce
echo "vk-poolahb-core: 全部用例通过"
