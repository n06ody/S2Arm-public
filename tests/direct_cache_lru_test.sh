#!/bin/bash
# 直连缓存按容量预算淘汰单测（tools/direct_cache_lru.py，tools/gc.sh 每轮调；约 1 秒）：bash tests/direct_cache_lru_test.sh
# 夹具：两款 A / B，仓条目各 1 MiB、修改时间手设（A1 最旧 … B2 最新），另有仓外文件（库目录 / 日志）各 1 MiB；合计 6 MiB。期望按规则手写：
#   ① 预算 10 MiB（不超）→ 一个不删
#   ② 预算 4.5 MiB（超 1.5 MiB）→ 按修改时间删最旧的 A1、A2（2 MiB），B 的条目与仓外文件都在
#   ③ 同样预算、A 在用 → A 整款不动，删 B 里最旧的 B1、B2（B 都删完仍超，打【黄】）
#   ④ 仓外文件永不删（预算 0：只删仓条目，库目录 / 日志在）
#   ⑤ --dry → 只打「会删」，一个文件不动
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
mk() {   # 重建夹具
  rm -rf "$T/c"; mkdir -p "$T/c/A/store/dshb/00" "$T/c/B/store/dshb/01" "$T/c/A/ios/b-x" "$T/c/B/logs"
  for e in A:1:202601010000 A:2:202601020000 B:1:202601030000 B:2:202601040000; do IFS=: read -r g i ts <<< "$e"
    d=$T/c/$g/store/dshb/0$([ $g = A ] && echo 0 || echo 1); head -c 1048576 /dev/zero > "$d/$g$i.e"; touch -t "$ts" "$d/$g$i.e"; done
  head -c 1048576 /dev/zero > "$T/c/A/ios/b-x/lib.metallib"; head -c 1048576 /dev/zero > "$T/c/B/logs/build.log"; }
L() { python3 "$H/tools/direct_cache_lru.py" "$T/c" "$@"; }
left() { (cd "$T/c" && find . -type f | sed 's|.*/||' | LC_ALL=C sort | tr '\n' ' '); }
mk; o=$(L --budget-gb 0.009765625); ck "① 不超预算 → 不删（${o}）" '[ "$(left)" = "A1.e A2.e B1.e B2.e build.log lib.metallib " ]'
mk; o=$(L --budget-gb 0.00439453125); ck "② 超 1.5 MiB → 删最旧 A1 A2（剩 $(left)）" '[ "$(left)" = "B1.e B2.e build.log lib.metallib " ] && grep -q "删 2 条" <<< "$o"'
mk; o=$(L --budget-gb 0.00146484375 --in-use A); ck "③ A 在用 → A 不动、删 B1 B2、仍超打【黄】（剩 $(left)）" '[ "$(left)" = "A1.e A2.e build.log lib.metallib " ] && grep -q "【黄】" <<< "$o"'
mk; o=$(L --budget-gb 0); ck "④ 预算 0 → 只删仓条目，库目录 / 日志在（剩 $(left)）" '[ "$(left)" = "build.log lib.metallib " ]'
mk; o=$(L --budget-gb 0 --dry); ck "⑤ --dry → 只打会删、一个不动（${o}）" '[ "$(left)" = "A1.e A2.e B1.e B2.e build.log lib.metallib " ] && grep -q "会删 4 条" <<< "$o"'
[ $bad = 0 ] && echo "direct_cache_lru 单测全过" || { echo "direct_cache_lru 单测有不通"; exit 1; }
