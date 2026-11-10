#!/bin/bash
# 收集器「这局用的包须干净」单测（graphics/tools/shbake/shcollect.sh；S2A.ShBake guest-new 用桩：按缓存目录里 progs 文件的程序集合做差，输出「新 N 总 M」）：
#   基准包 {a b}。局 1：装干净包 {a b}、跑出 c → 收；局 2：装受污染包 {a b x}、跑出 d → 不收（包里有基准外的 x）；局 3：包快照已删、局目录也没有 pkg_shcache → 不收、报黄
#   局 4：包快照已删、局目录 pkg_shcache {a b}、跑出 f → 收（10-23 丝之歌 8 局 7 局因快照被 gc 删而不收）；局 5：快照已删、pkg_shcache {a b y}、跑出 g → 不收
#   期望：收集目录两份、来自局 1 与局 4；输出里局 2 / 局 5「不收…受污染包」、局 3「包快照找不到」
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0; trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
mkc() { local d=$1; shift; mkdir -p $d; for f in shared.toc shared.data guest.toc guest.data; do echo "$d $f" > $d/$f; done; printf '%s\n' "$@" > $d/progs; }
cat > $T/sb <<'SB'
#!/bin/bash
[ "$1" = guest-new ] || exit 2
[ "${2:-}" = --code ] && { echo "$*" >> "$(dirname "$0")/code_calls"; shift; }
[ -f "$2/shared.toc" ] || { echo "没有 shared.toc"; exit 1; }
c=$2; shift 2; have=$(for h in "$@"; do cat "$h/progs" 2>/dev/null; done | sort -u)
n=0; m=0; while read -r p; do [ -n "$p" ] || continue; m=$((m + 1)); grep -qx "$p" <<< "$have" || n=$((n + 1)); done < "$c/progs"; echo "新 $n 总 $m"
SB
chmod +x $T/sb
TID=0100aaaa00000000; mkc $T/base/shaders/shcache/$TID a b
job() { mkdir -p $T/sched/jobs/$1/out_1; printf 'GAME=g\nBUNDLE=io.s2arm.g\n' > $T/sched/jobs/$1/job.env; printf 'G=g\nPKG=%q\n' "$2" > $T/sched/jobs/$1.job; }
mkc $T/pk/clean/pack/shaders/shcache/$TID a b; job j1 $T/pk/clean; mkc $T/sched/jobs/j1/out_1/shcache_rt a b c
mkc $T/pk/dirty/pack/shaders/shcache/$TID a b x; job j2 $T/pk/dirty; mkc $T/sched/jobs/j2/out_1/shcache_rt a b x d
job j3 $T/pk/gone; mkc $T/sched/jobs/j3/out_1/shcache_rt a b e
job j4 $T/pk/gone4; mkc $T/sched/jobs/j4/out_1/shcache_rt a b f; mkc $T/sched/jobs/j4/out_1/pkg_shcache a b
job j5 $T/pk/gone5; mkc $T/sched/jobs/j5/out_1/shcache_rt a b y g; mkc $T/sched/jobs/j5/out_1/pkg_shcache a b y
export S2A_SCHED_DIR=$T/sched S2A_SHBAKE=$T/sb S2A_SHCOLLECT_BASE=$T/base S2A_SHCOLLECT=$T/col
# 按输出的「收回…来自」行判收了哪些局（桩靠 progs 文件认程序，收进目录只拷四个缓存文件，已收份在桩眼里是空的，去重会把先收的删掉，不能看目录残留）
o=$(bash $H/graphics/tools/shbake/shcollect.sh g 2>&1); got=$(sed -n 's/.* 收回 .*来自 \(.*\)）$/\1/p' <<< "$o" | LC_ALL=C sort | tr '\n' ' ')
ck "只收局 1、局 4（干净包跑出新代码）：$(echo $got | sed "s#$T/##g")" '[ "$got" = "$T/sched/jobs/j1/out_1/shcache_rt $T/sched/jobs/j4/out_1/shcache_rt " ]'
ck "局 2（受污染包 {a b x}）不收、说明原因" 'grep -q "不收 $T/sched/jobs/j2/out_1/shcache_rt：这局装的包里有现行包与已收各份之外的程序" <<< "$o"'
ck "核包用 --code（只比代码）：桩记到 4 次带 --code 的调用（局 1、2、4、5 的包）" '[ "$(wc -l < $T/code_calls | tr -d " ")" = 4 ]'
ck "局 5（快照已删、局目录包缓存受污染 {a b y}）不收" 'grep -q "不收 $T/sched/jobs/j5/out_1/shcache_rt：这局装的包里有" <<< "$o"'
ck "局 3（包快照已删）不收、报黄" 'grep -q "【黄】shcollect：$T/sched/jobs/j3/out_1/shcache_rt 的包快照找不到" <<< "$o"'
[ $bad = 0 ] && echo "收集器包检查单测全过" || { echo "收集器包检查单测有不通"; echo "$o" | sed "s#$T/##g"; exit 1; }
