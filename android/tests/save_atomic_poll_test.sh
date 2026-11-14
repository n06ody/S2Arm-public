#!/usr/bin/env bash
# 安卓存档原子态轮询宿主单测（#961）：真实 direct_prompt.c（-D__ANDROID__）分「有出口 / 缺出口」两次链接，
# 再跑一个负对照（缺出口改回答 0＝当成已确认安全，缺出口用例必须重新变红）。不证明设备、NativeAOT 或 Java 提示。
# 用法：bash android/tests/save_atomic_poll_test.sh [输出目录]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../.." && pwd)
src=${S2A_SAVE_POLL_SRC:-$repo/runtime/src/direct_prompt.c}
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/s2a-save-poll.XXXXXX")}; mkdir -p "$out"
CC=${CC:-cc}
flags=(-std=gnu11 -D_GNU_SOURCE -D__ANDROID__ -Wall -Wextra -Werror -I "$repo/runtime/include" -I "$repo/runtime/src")
# 缺出口那次：ELF 的未定义弱引用天然为空；Mach-O ld64 对未定义弱引用要显式放行
weak=(); [ "$(uname -s)" = Darwin ] && weak=(-Wl,-U,_S2A_SaveAtomicState)
build() {   # build <输出名> <direct_prompt 源> [额外 -D…]
    local exe=$1 source=$2; shift 2
    "$CC" "${flags[@]}" "$@" "$here/save_atomic_poll_test.c" "$source" "${weak[@]}" -o "$out/$exe"
}
build provider "$src" -DS2A_TEST_PROVIDER
build missing "$src"
"$out/provider"
"$out/missing"
# 负对照：缺出口改回答 0（当成已确认安全），缺出口用例必须红
python3 - "$src" "$out/mutant.c" <<'PY'
import sys
text = open(sys.argv[1], encoding='utf-8').read()
old = '        return S2A_SAVE_ATOMIC_UNKNOWN;\n'
assert text.count(old) == 1, '原文必须恰好出现一次'
open(sys.argv[2], 'w', encoding='utf-8').write(text.replace(old, '        return 0;\n'))
PY
build mutant "$out/mutant.c" -I "$repo/runtime/src"
if "$out/mutant" > "$out/mutant.log" 2>&1; then
    cat "$out/mutant.log"; echo "✗ save_atomic_poll 负对照：缺出口答 0 仍全绿"; exit 1
fi
grep '不符' "$out/mutant.log" | head -3
echo "✓ save_atomic_poll 负对照：缺出口答 0 → 缺出口用例红"
