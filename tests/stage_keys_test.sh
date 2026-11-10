#!/bin/bash
# 在两种 sed 上用临时文件完成同一替换。
sed_edit() { sed -i.s2a-bak "$1" "$2" && rm -f "$2.s2a-bak"; }
# 门禁闭包：--table pack/stage_inputs/direct.tsv pack/stage_direct.sh graphics/metal/gal_prog.m
# 门禁闭包：--table pack/stage_inputs/extract.tsv pack/stage_extract.sh
# 门禁闭包：--table pack/stage_inputs/direct_seed.tsv pack/stage_direct_seed.sh
# 门禁覆盖：pack/stage_*.sh pack/stage_keyfiles.sh pack/stage_inputs/* tools/stage_inputs.py graphics/tools/direct/* graphics/tools/shbake/*.py graphics/nvn/* graphics/metal/gal_*
# 阶段源码键单测（pack/stage_keyfiles.sh + pack/stage_inputs/*.tsv + tools/stage_inputs.py；维护者 10-26 20:3x 直连库 / 抽取键收窄）：bash tests/stage_keys_test.sh
# 期望按规则手写：
#   ① 本仓库：直连库 / 抽取 / 管线种子表（审计 #1534 P1-54）三个阶段从起点扫出的依赖闭包里每个文件都在各自声明表里归了类（入 / 外），表里每个文件都在 HEAD
#   ② 临时克隆里提交改动：宿主运行时代码（graphics/nvn/gfx/nvn/nvn_mtl.m，不在表里）改一行 → 两个键都不变（旧算法整树对象号会变：反证）；
#      「外」文件（tools/clonedir.py）改一行 → 键不变；「入」文件改一行（se_want.py → 直连库键变、nxsh_extract.py → 抽取键变）；
#      gal_prog.m 里 gal_spv_hash 改一处取值（摘要偏移 d → d + 8）→ 直连库键变（gal_prog.m 整份进键：直连出库经 libnxgal 调它的共享内存溢出改写）
#   ③ 出库脚本新 import 一个表里没列的模块 → 闭包核对报出它（漏声明报红）
#   ⑤ 表里删掉 gal_tessxc.c → 闭包核对报出它（S2A.Dsh 经 DllImport 调 libnxgal 导出，按 NXGAL_API 定义连到实现）；只改它一行注释 → 直连库键变
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
# 临时仓库 / 临时提交的固定身份（#948）：全新环境（全新家目录、没设全局 git 身份、GIT_CONFIG_GLOBAL=/dev/null、useConfigOnly）下
#   不许依赖调用者的 git 配置自动推断；只作用于本单测进程与它拉起的子进程，绝不写调用者的全局 / 仓库配置。
#   覆盖「②」里 4 处裸 `git commit -qam`（clonedir.py / se_want.py / gal_prog.m / nxsh_extract.py）；同一段里带 -c user.* 的那 3 处不受影响。
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
# 入口取自头部「门禁闭包」行（门禁路由 tools/gate_route.py 按同一份算闭包选本单测，两处不各写一份）
ENT_direct=$(sed -n 's#^\# 门禁闭包：--table pack/stage_inputs/direct.tsv ##p' "$0"); ENT_extract=$(sed -n 's#^\# 门禁闭包：--table pack/stage_inputs/extract.tsv ##p' "$0")
ENT_direct_seed=$(sed -n 's#^\# 门禁闭包：--table pack/stage_inputs/direct_seed.tsv ##p' "$0")
[ -n "$ENT_direct" ] && [ -n "$ENT_extract" ] && [ -n "$ENT_direct_seed" ] || { echo "不通 头部「门禁闭包」行读不到入口"; exit 1; }
miss() {   # miss <仓库> <阶段> → 闭包里有、声明表里没有的文件（一行一个）
  local r=$1 st=$2 e; e=ENT_$st
  # shellcheck disable=SC2086
  # 起点 = 头部入口 + 表里「入」的文件：手写进表的输入（S2A.Dsh 的 *.cs 等，静态扫描从入口走不到）自己的依赖也要归类（审计 #1565 P1-115：SpvCross.cs 调 nxgal_dmsh_xc_tess，gal_tessxc.m（11-08 起 graphics/dsx/gal_tessxc.c）漏登记）
  LC_ALL=C comm -23 <(cd "$r" && python3 tools/stage_inputs.py --table "pack/stage_inputs/$st.tsv" ${!e} $(LC_ALL=C awk -F'\t' '$1 == "入" { print $2 }' "$r/pack/stage_inputs/$st.tsv") | LC_ALL=C sort) <(awk -F'\t' '!/^#/ && NF >= 2 { print $2 }' "$r/pack/stage_inputs/$st.tsv" | LC_ALL=C sort)
}
key() { bash -c 'source "$1/pack/stage_keyfiles.sh"; stage_files_key "$2" "$1"' _ "$1" "$2"; }
# ①
for st in direct extract direct_seed; do
  m=$(miss "$H" $st); k=$(key "$H" $st 2> $T/err)
  ck "① ${st}：闭包都在声明表里（漏 $(grep -c . <<< "$m") 个：$(tr '\n' ' ' <<< "$m" | cut -c1-120)）、键算得出（${k}）" '[ -z "$m" ] && [ ${#k} = 16 ]'
done
# ②
git clone -q --shared "$H" $T/r 2> /dev/null || { echo "不通 建临时克隆"; exit 1; }
cp -R "$H"/pack/stage_inputs "$H"/pack/stage_keyfiles.sh $T/r/pack/ 2> /dev/null; cp "$H"/tools/stage_inputs.py $T/r/tools/
( cd $T/r && git add -A && git -c user.email=t@t -c user.name=t commit -qm base ) > /dev/null 2>&1 || true
d0=$(key $T/r direct); e0=$(key $T/r extract); o0=$(git -C $T/r rev-parse HEAD:graphics/nvn)
chg() { echo "// stage_keys_test $RANDOM" >> "$T/r/$1"; ( cd $T/r && git add "$1" && git -c user.email=t@t -c user.name=t commit -qm "$1" ); }
chg graphics/nvn/gfx/nvn/nvn_mtl.m; d1=$(key $T/r direct); e1=$(key $T/r extract); o1=$(git -C $T/r rev-parse HEAD:graphics/nvn)
ck "② 宿主运行时代码 nvn_mtl.m 改一行 → 直连库键 ${d0} → ${d1}、抽取键 ${e0} → ${e1} 都不变（旧算法 graphics/nvn 整树对象号 ${o0:0:9} → ${o1:0:9} 会变）" '[ "$d0" = "$d1" ] && [ "$e0" = "$e1" ] && [ "$o0" != "$o1" ]'
echo "# stage_keys_test" >> $T/r/tools/clonedir.py; ( cd $T/r && git commit -qam mb ); d2=$(key $T/r direct)
ck "② 「外」文件 tools/clonedir.py 改一行 → 直连库键不变（${d2}）" '[ "$d2" = "$d0" ]'
echo "# stage_keys_test" >> $T/r/graphics/tools/direct/se_want.py; ( cd $T/r && git commit -qam db ); d3=$(key $T/r direct); e3=$(key $T/r extract)
ck "② 「入」文件 se_want.py 改一行 → 直连库键变（${d0} → ${d3}）、抽取键不变（${e3}）" '[ "$d3" != "$d0" ] && [ "$e3" = "$e0" ]'
sed_edit 's/uint64_t h; memcpy(&h, d, 8); return h;/uint64_t h; memcpy(\&h, d + 8, 8); return h;/' $T/r/graphics/metal/gal_prog.m   # 种类键取 SHA256 摘要的另一段 8 字节
( cd $T/r && git commit -qam gp ) > /dev/null; d5=$(key $T/r direct)
ck "② gal_prog.m 里 gal_spv_hash 改取摘要第 8–15 字节（$(git -C $T/r diff HEAD~1 --stat | tail -1 | tr -s ' ')）→ 直连库键变（${d3} → ${d5}）" 'git -C $T/r show HEAD -- graphics/metal/gal_prog.m | grep -q "^+.*d + 8, 8" && [ "$d5" != "$d3" ]'
echo "# stage_keys_test" >> $T/r/graphics/tools/shbake/nxsh_extract.py; ( cd $T/r && git commit -qam nx ); e4=$(key $T/r extract)
ck "② 「入」文件 nxsh_extract.py 改一行 → 抽取键变（${e0} → ${e4}）" '[ "$e4" != "$e0" ]'
# ③
printf 'X = 1\n' > $T/r/graphics/tools/direct/stage_keys_newdep.py; printf 'import stage_keys_newdep  # noqa\n' >> $T/r/graphics/tools/direct/se_want.py
( cd $T/r && git add -A && git -c user.email=t@t -c user.name=t commit -qm dep ); m=$(miss $T/r direct)
ck "③ se_want.py 新 import 表外模块 → 闭包核对报出 graphics/tools/direct/stage_keys_newdep.py（${m}）" '[ "$m" = graphics/tools/direct/stage_keys_newdep.py ]'
# ⑤ libnxgal 导出连到实现（审计 #1565 P1-115）：表里删掉 gal_tessxc.c → 闭包核对报出它（SpvCross.cs 经 DllImport 调 nxgal_dmsh_xc_tess）；
#    放回后只改 gal_tessxc.c 一行注释 → 直连库键变（修前这一改键不变、出库打「现行已是直连库」跳过）
grep -v '^入	graphics/dsx/gal_tessxc.c	' $T/r/pack/stage_inputs/direct.tsv > $T/dt && cp $T/dt $T/r/pack/stage_inputs/direct.tsv
( cd $T/r && git -c user.email=t@t -c user.name=t commit -qam rmtess ); m=$(miss $T/r direct)
ck "⑤ 表里删掉 gal_tessxc.c → 闭包核对报出它（${m//$'\n'/ }）" 'grep -qx graphics/dsx/gal_tessxc.c <<< "$m"'
( cd $T/r && git checkout -q HEAD~1 -- pack/stage_inputs/direct.tsv && git -c user.email=t@t -c user.name=t commit -qm putback ); d6=$(key $T/r direct)
sed_edit '1s#$# （stage_keys_test 只改注释）#' $T/r/graphics/dsx/gal_tessxc.c; ( cd $T/r && git commit -qam tc ) > /dev/null; d7=$(key $T/r direct)
ck "⑤ 只改 gal_tessxc.c 一行注释 → 直连库键变（${d6} → ${d7}）" '[ -n "$d6" ] && [ -n "$d7" ] && [ "$d6" != "$d7" ]'
# ④ locale 合同：原创三阶段夹具，不改生产声明表；包装 awk 只记录环境，仍执行真实系统 awk。
mkdir -p "$T/locale/pack/stage_inputs" "$T/awk-bin"
git -C "$T/locale" init -q
printf 'input\n' > "$T/locale/input.txt"; printf 'outside\n' > "$T/locale/outside.txt"
for st in shader_pack direct_lib extract; do
  printf '入\tinput.txt\n外\toutside.txt\n' > "$T/locale/pack/stage_inputs/$st.tsv"
done
cp "$H/pack/stage_keyfiles.sh" "$T/locale/pack/"
git -C "$T/locale" add .
git -C "$T/locale" -c user.name=test -c user.email=test@example.invalid commit -qm fixture
fixture_key=$(LC_ALL=C key "$T/locale" shader_pack)
REAL_AWK=$(command -v awk); export REAL_AWK
cat > "$T/awk-bin/awk" <<'AWK'
#!/bin/sh
for arg do
  case "$arg" in *'入'*) printf '%s\n' "${LC_ALL-unset}" >> "$LOCALE_TRACE";; esac
done
exec "$REAL_AWK" "$@"
AWK
chmod +x "$T/awk-bin/awk"
export LOCALE_TRACE="$T/awk-locale.log"
for st in shader_pack direct_lib extract; do
  kc=$(LC_ALL=C key "$T/locale" "$st")
  ku=$(LC_ALL=en_US.UTF-8 key "$T/locale" "$st")
  ck "④ ${st}：C / UTF-8 阶段键逐字一致" '[ ${#kc} = 16 ] && [ "$kc" = "$ku" ]'
  : > "$LOCALE_TRACE"
  observed=$(PATH="$T/awk-bin:$PATH" LC_ALL=en_US.UTF-8 bash -c 'source "$1/pack/stage_keyfiles.sh"; stage_files_key "$2" "$1"; printf "%s\n" "$LC_ALL"' _ "$T/locale" "$st")
  ck "④ ${st}：中文筛选局部使用 C（真实 awk），调用者仍为 UTF-8" '[ "$(cat "$LOCALE_TRACE")" = C ] && [ "$(head -1 <<< "$observed")" = "$fixture_key" ] && [ "$(tail -1 <<< "$observed")" = en_US.UTF-8 ]'
done
printf 'changed\n' >> "$T/locale/outside.txt"
git -C "$T/locale" -c user.name=test -c user.email=test@example.invalid commit -qam outside
for st in shader_pack direct_lib extract; do
  kc=$(LC_ALL=C key "$T/locale" "$st"); ku=$(LC_ALL=en_US.UTF-8 key "$T/locale" "$st")
  ck "④ ${st}：外部文件变化后两种 locale 均保持原键" '[ "$kc" = "$ku" ] && [ "$kc" = "$fixture_key" ]'
done
[ $bad = 0 ] && echo "stage_keys 单测全过" || { echo "stage_keys 单测有不通"; exit 1; }
