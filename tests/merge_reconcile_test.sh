#!/bin/bash
# 列车合入对账（tools/merge_reconcile.py）：用 10-30 两个真实例子做正反对照：bash tests/merge_reconcile_test.sh
# 门禁覆盖：tools/merge_reconcile.py
#   用合成历史复现 10-30 两种情形（11-09：原夹具是私仓历史上的 bundle，公开仓是重新起的干净历史、前置提交不存在，从首次公开快照起一直红）：
#   A（同 #1000）：列车合入 c1，之后作者只把 main 合进 PR（不改内容）→ merged
#   B（同 #1002）：列车合入 d1，之后作者又推 d2 d3 → partial，没合的恰好是 d2 d3
#   期望值取自造历史时记下的提交号，不取自被测工具
set -u
H=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/merge_reconcile_test.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
g() { git -C "$T/r" -c user.name=t -c user.email=t@t -c commit.gpgsign=false "$@"; }
c() { printf '%s\n' "$2" > "$T/r/$1" && g add "$1" && g commit -qm "$3" && g rev-parse HEAD; }   # c <文件> <内容> <说明> → 新提交号
git init -q -b main "$T/r" || { echo "不通 建临时仓库"; exit 1; }
m0=$(c a.txt a base)
g checkout -qb pr1000; c1=$(c f1.txt one 'pr1000: c1')
g checkout -q main; m1=$(c b.txt b 'main: m1')
g merge -q --no-ff -m "Merge branch 'contrib/pr1000' into HEAD" "$c1"; M1=$(g rev-parse HEAD)        # 列车合入 c1
g checkout -q pr1000; g merge -q --no-ff -m 'Merge main into pr1000' "$M1"; h1000=$(g rev-parse HEAD)   # 作者只合 main
g checkout -q -b pr1002 "$M1"; d1=$(c f2.txt two 'pr1002: d1')
g checkout -q main; m2=$(c c.txt c 'main: m2')
g merge -q --no-ff -m "Merge branch 'contrib/pr1002' into HEAD" "$d1"; M=$(g rev-parse HEAD)          # 列车合入 d1（旧版）
g checkout -q pr1002; d2=$(c f3.txt three 'pr1002: d2'); d3=$(c f4.txt four 'pr1002: d3'); h1002=$d3   # 之后又推两个
R() { python3 "$H/tools/merge_reconcile.py" --repo "$T/r" "$@"; }
F() { python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d[sys.argv[2]] if sys.argv[2] != "outstanding" else " ".join(sorted(d["outstanding"])))' "$1" "$2"; }

o=$(R 1000 "$h1000" --main $M)
ck "① A：头只是合 main 的合并提交 → merged（实得 $(F "$o" state)）" '[ "$(F "$o" state)" = merged ]'
ck "① A：合入的提交 = c1、main 合并提交 = M1" '[ "$(F "$o" merged)" = "$c1" ] && [ "$(F "$o" main_merge)" = "$M1" ]'

o=$(R 1002 "$h1002" --main $M)
ck "② B：之后还有提交没合 → partial（实得 $(F "$o" state)）" '[ "$(F "$o" state)" = partial ]'
ck "② B：合入的提交 = d1" '[ "$(F "$o" merged)" = "$d1" ]'
want=$(printf '%s\n' "$d2" "$d3" | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')
ck "② B：没合的恰好是 d2 d3" '[ "$(F "$o" outstanding)" = "$want" ]'

# 反证：两支合入之前的 main，都判「没合过」
o=$(R 1000 "$h1000" --main "$m1")
ck "③ A 在合入前的 main 上 → none（实得 $(F "$o" state)）" '[ "$(F "$o" state)" = none ]'
o=$(R 1002 "$h1002" --main "$m2")
ck "③ B 在合入前的 main 上 → none（实得 $(F "$o" state)）" '[ "$(F "$o" state)" = none ]'

# 正对照：在 A 头上再叠一个不改内容的合并提交（树 = 按仓库规则自动合并的结果）→ 仍是 merged
auto=$(g -c attr.tree=$M merge-tree --write-tree "$h1000" $M | head -1)
honest=$(g commit-tree "$auto" -p "$h1000" -p $M -m 'plain merge')
o=$(R 1000 "$honest" --main $M)
ck "④ 再叠一个不改内容的合并提交 → 仍 merged（实得 $(F "$o" state)）" '[ "$(F "$o" state)" = merged ]'
# 反证：合并提交里手改过内容就不算「只是合 main」：同一位置叠一个多了文件的合并提交 → partial
blob=$(printf 'evil\n' | g hash-object -w --stdin)
g read-tree "$auto" && g update-index --add --cacheinfo 100644,"$blob",evil_merge.txt
t2=$(g write-tree)
evil=$(g commit-tree "$t2" -p "$h1000" -p $M -m 'merge with hand edit')
o=$(R 1000 "$evil" --main $M)
ck "⑤ 合并提交手改了内容（多一个文件）→ partial（实得 $(F "$o" state)）" '[ "$(F "$o" state)" = partial ]'

[ $bad = 0 ] && echo "merge_reconcile 单测全过" || { echo "merge_reconcile 单测有不通"; exit 1; }
