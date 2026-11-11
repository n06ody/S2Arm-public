#!/usr/bin/env bash
# 宿主回归：程序结果里 bindings 超出容量必须准确失败（F1：绝不静默截断）。
# 用法：bash android/tools/nvn_vk_program_binding_cap.sh；退出 0 = 全过。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cc=${CC:-cc}
py=${S2A_PY:-${PY:-python3}}
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-progbindcap.XXXXXX")
trap 'rm -rf "$out"' EXIT
inc=(-I "$repo/android/native" -I "$repo/android/native/nvn" -I "$repo/android/native/direct")
src=("$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c")
"$cc" -std=c11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers "${inc[@]}" \
    -o "$out/t" "$repo/android/tests/nvn_vk_program_binding_cap_test.c" "${src[@]}"
fail=0
printf '== cap ==\n'
"$out/t" cap || fail=1
# 反例说明：撤掉守卫并把循环条件恢复成改前写法（`i < n && count < cap`），17 条应被静默截成 16 条
# 且 rc=0——这条锁住守卫不能删。
mut="$out/mut.c"
"$py" "$repo/android/tools/nvn_vk_program_binding_cap_mutant.py" \
    "$repo/android/native/nvn/nvn_vk_program_result.c" "$mut"
"$cc" -std=c11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers "${inc[@]}" \
    -o "$out/mut" "$repo/android/tests/nvn_vk_program_binding_cap_test.c" "$mut" \
    "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c"
printf '== truncate（反例说明）==\n'
"$out/mut" truncate || fail=1
[ "$fail" = 0 ] && echo "nvn-vk-program-binding-cap: 通过" || { echo "nvn-vk-program-binding-cap: 失败"; exit 1; }
