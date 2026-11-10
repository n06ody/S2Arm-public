#!/bin/bash
# 出包·共享·选包 / 解包阶段（J07 #183）：pack/stages/unpack.sh
# 与目标无关：把「按内容认包 → 选中本款的包 → nx_unpack 解包」抽成共享阶段。产物只有游戏原始输入，
#   路径里不含 ios / android / ohos；目标插件只决定解出来的根之后怎么用（rewrite / bundle / …）。
#
#   pack/stages/unpack.sh inspect --src <目录> [--ids <缓存>] [--nx-unpack <程序>] [--keys <prod.keys>]
#       → stdout 每行「路径<TAB>字节<TAB>修改时间<TAB>程序号」；读不出程序号的更新 / DLC 包记「-」
#   pack/stages/unpack.sh extract --src <目录> --out <目录> [--title-id <16 位十六进制>] [--ids <缓存>]
#                              [--nx-unpack <程序>] [--keys <prod.keys>] [--log <文件>] [--force]
#       → 解包到 <out>（先写 <out>.tmp，成功才原子改名）；产物 exefs/ romfs/ ctl/ aoc/ unpack.tsv 与
#         阶段键 <out>/.stage_unpack。退出 0 = 产物齐全；非 0 = 没写成（<out> 不动、<out>.tmp 清掉）
#   Switch 2 NUP 容器（--src 是 .nup 文件，或目录下没有 Switch 1 原件、恰有一个 .nup；判法 pack/ns2_nup.py --detect）：不要 prod.keys，
#     认包行由 ns2_nup.py --inspect 出，解包走 ns2_nup.py --to-extract（展开 + pack/ns2_dump.py 整理，同一键出包 / 启动器作业层），
#     产物同形；键另含 ns2_nup.py / ns2_dump.py 内容；不进全机解包缓存（容器不加密、展开只是拷贝）
#
# 缓存键（写进 .stage_unpack，16 位）：
#   sha256( 选中包逐行「路径<TAB>字节<TAB>mtime<TAB>程序号」 + nx_unpack / nx_unpack.dll 内容 sha256 )[:16]
#   选中包 = 程序号等于 --title-id 的包 + 读不出程序号的包（更新 / DLC 在 --nacp-only 下没有控制分区，
#   由 nx_unpack 按内容清单自己排除别的游戏的包）。文件名不参与判定（10-20 维护者：按内容认包）。
#   目标名 / ABI / 工具链环境变量不进键：解包只由「原始包字节 + nx_unpack 程序」决定。
#   键里用字节数 + mtime（沿用 pack/srcs.sh 的口径）：原始包只读、不会被原地改；工具换了由工具哈希兜住。
# 为什么不复用 oneclick 的 .oneclick_key：那是另一套键（含 --aoc-index / --title-id 与 macOS stat 输出），
#   两套键互相独立；oneclick 尚未改调本阶段（本片只新建共享阶段，不动 oneclick / make_pack.py）。
set -u
H=$(cd "$(dirname "$0")/.." && pwd)   # pack/
R=$(cd "$H/.." && pwd)

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
CMD=${1:-}; [ $# -gt 0 ] && shift || true
SRC= OUT= TID= IDS= NXU= KEYS= LOG= FORCE=0
while [ $# -gt 0 ]; do
  case $1 in
    --src) SRC=${2:?--src 需要目录}; shift 2;;
    --out) OUT=${2:?--out 需要目录}; shift 2;;
    --title-id) TID=${2:?--title-id 需要 16 位十六进制}; shift 2;;
    --ids) IDS=${2:?--ids 需要缓存文件}; shift 2;;
    --nx-unpack) NXU=${2:?--nx-unpack 需要程序}; shift 2;;
    --keys) KEYS=${2:?--keys 需要文件}; shift 2;;
    --log) LOG=${2:?--log 需要文件}; shift 2;;
    --force) FORCE=1; shift;;
    -h|--help) usage;;
    *) echo "【红】认不得的参数：$1" >&2; exit 2;;
  esac
done
case $CMD in inspect|extract) ;; *) usage;; esac
[ -n "$SRC" ] || { echo "【红】要给 --src <源包目录>" >&2; exit 2; }
if [ -f "$SRC" ]; then SRC=$(cd "$(dirname "$SRC")" 2>/dev/null && pwd -P)/$(basename "$SRC")   # 单个 .nup 文件（下面判真 NUP；别的文件照旧找不到原件报红）
else SRC=$(cd "$SRC" 2>/dev/null && pwd -P) || { echo "【红】源包目录不在：$SRC" >&2; exit 1; }; fi
[ "$CMD" = inspect ] || [ -n "$OUT" ] || { echo "【红】extract 要给 --out <解包目录>" >&2; exit 2; }

# 认包 / 解包都用 pack/srcs.sh 的同一套（nxu_path / src_ids）：S2A_NX_UNPACK 未给时按源码哈希现编。
export S2A_GAME_SRC=$SRC
[ -z "$NXU" ] || export S2A_NX_UNPACK=$NXU
[ -z "$KEYS" ] || export S2A_KEYS=$KEYS
[ -z "$IDS" ] || export S2A_SRC_IDS=$IDS
source "$H/srcs.sh"
[ -x "$SRC_N" ] || { echo "【红】nx_unpack 不可执行：${SRC_N}（用 --nx-unpack 或 S2A_NX_UNPACK 指定；要 .NET 10 SDK 现编）" >&2; exit 1; }

# Switch 2 NUP：源里没有 Switch 1 原件（同 src_ids 的扩展名）且 ns2_nup.py --detect 认出恰一个 NUP 才走这支，混放时照旧按 Switch 1 原件解
NUPF=
if [ -z "$(find -L "$SRC" -maxdepth 4 \( -iname '*.xci' -o -iname '*.nsp' -o -iname '*.nsz' -o -iname '*.xcz' \) -type f -print 2> /dev/null | head -1)" ]; then
  NUPF=$(python3 "$R/pack/ns2_nup.py" --detect "$SRC") || NUPF=
fi
if [ "$CMD" = inspect ]; then
  if [ -n "$NUPF" ]; then python3 "$R/pack/ns2_nup.py" --inspect "$NUPF" || exit 1; else src_ids || exit 1; fi; exit 0; fi

OUT=$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$OUT") || exit 1
mkdir -p "$(dirname "$OUT")" || { echo "【红】建不了输出目录的上级：$(dirname "$OUT")" >&2; exit 1; }
if [ -n "$NUPF" ]; then IDSROWS=$(python3 "$R/pack/ns2_nup.py" --inspect "$NUPF") || { echo "【红】NUP 容器读不出：${NUPF}（原因见上）" >&2; exit 1; }
else IDSROWS=$(src_ids) || { echo "【红】列原始包失败（${SRC}；原件探测出错见上，机器原因，不判「没有这个程序号」）" >&2; exit 1; }; fi
[ -n "$IDSROWS" ] || { echo "【红】$SRC 下没有 .xci / .nsp / .nsz / .xcz" >&2; exit 1; }

if [ -z "$TID" ]; then
  # 不止一个本体（10-26 #456：游戏文件夹里还带同系列独立小程序，另一个程序号）：按各程序号的原始包字节数认主游戏——最大的至少是第二大的 4 倍才自动挑它、打黄字；
  #   判不出（差不多大）报红、请用 --title-id 指定（同 pack/oneclick/oneclick.sh 的认游戏规则）
  pick=$(printf '%s\n' "$IDSROWS" | python3 -c '
import sys
s = {}
for l in sys.stdin:
    f = l.rstrip("\n").split("\t")
    if len(f) >= 4 and f[3] != "-": s[f[3].upper()] = s.get(f[3].upper(), 0) + int(f[1] or 0)
top = sorted(s, key=lambda t: -s[t]); c = " ".join(f"{t}（{s[t] / 1e9:.2f} GB）" for t in top)
if len(top) == 1: print(top[0])
elif len(top) > 1 and s[top[0]] > 0 and s[top[0]] >= 4 * s[top[1]]: print(top[0]); print(f"🟡 有 {len(top)} 个本体，按大小认 {top[0]} 是主游戏，跳过 {" ".join(top[1:])}；要出别的用 --title-id", file=sys.stderr)
else: print(f"【红】本体应恰好 1 款，实际 {len(top)} 款（{c or "无"}），判不出哪个是主游戏：一个目录只放一款，或用 --title-id 指定", file=sys.stderr); sys.exit(1)') || exit 1
  TID=$pick
fi
TID=$(printf '%s' "$TID" | tr 'a-f' 'A-F')
case $TID in *[!0-9A-F]*|'') echo "【红】--title-id 要 16 位十六进制：${TID}" >&2; exit 2;; esac
[ ${#TID} = 16 ] || { echo "【红】--title-id 要 16 位十六进制：${TID}" >&2; exit 2; }

SEL=$(printf '%s\n' "$IDSROWS" | awk -F'\t' -v t="$TID" 'toupper($4) == t || $4 == "-"')
printf '%s\n' "$SEL" | awk -F'\t' -v t="$TID" 'toupper($4) == t {f=1} END {exit !f}' \
  || { echo "【红】$SRC 下没有程序号 ${TID} 的原始包（有：$(printf '%s\n' "$IDSROWS" | awk -F'\t' '{print $4}' | sort -u | tr '\n' ' ')）" >&2; exit 1; }

# 键：选中包的「路径 字节 mtime 程序号」+ nx_unpack 内容。只按内容，不看文件名 / 目标。
K=$( { printf '%s\n' "$SEL"; for t in "$SRC_N" "$SRC_N.dll" ${NUPF:+"$R/pack/ns2_nup.py" "$R/pack/ns2_dump.py"}; do [ -f "$t" ] && shasum -a 256 < "$t"; done; } | shasum -a 256 | cut -c1-16)

if [ -f "$OUT/.stage_unpack" ] && [ "$(cat "$OUT/.stage_unpack")" = "$K" ] && [ -f "$OUT/exefs/main" ]; then
  # 复用也要保证旁注在：键相同说明解包内容没变，但目录可能是旧版工具解的（那时还不写旁注）。
  #   已有旁注不动（调用方可能特意给了一份）；缺了按同一份算法补，写不出说明这份产物没有本体版本，照常报红不放过。
  python3 "$R/pack/direct_profile_input.py" "$OUT" || { echo "【红】复用 ${OUT} 但直连配置版本旁注补不出（解包产物太旧，用当前 nx_unpack 重解）" >&2; exit 1; }
  echo "🟢 解包输入没变，复用 ${OUT}（键 ${K}）"; exit 0
fi
# 全机解包缓存（11-02 维护者：「解包结果共用是正道，多出一份浪费时间」——普查解过的款，出包队列 / 一键出包再解一遍要 2–3 分钟读机械盘）：
#   按「程序号-内容键」放 S2A_UNPACK_CACHE（缺省 run/s2arm/cache/unpack/<程序号>-<内容键>/，设 off 关；整目录改名落地，在即完整），命中就克隆过来（同卷 APFS 克隆几秒、不占新空间），
#   没命中照常解，解完克隆一份进缓存（解到 .tmp.<pid> 再改名，并发后到的丢弃；旁边 <条目>.size 记 KB 数供 gc 算总量）。只读使用；gc 每个程序号留最新 1 份、总量设上限（tools/gc.sh）
#   缓存键只看内容、不看路径（中段同长度的位翻转认不出：原件只读，坏件都是截断 / 大小不同；上面的阶段键 K 含路径：普查把原件拷到暂存目录、出包队列软链原件，同一份原件 K 不同）：每个选中包「程序号 字节数 头 1 MB 哈希 尾 1 MB 哈希」+ nx_unpack 内容
UC=${S2A_UNPACK_CACHE:-${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/cache/unpack}; UCE=
if [ "$UC" != off ] && [ -z "$NUPF" ]; then
  UK=$( { while IFS=$'\t' read -r f sz _ id; do [ -n "$f" ] || continue; printf '%s %s %s %s\n' "$id" "$sz" "$(head -c 1048576 "$f" | shasum -a 256 | cut -c1-16)" "$(tail -c 1048576 "$f" | shasum -a 256 | cut -c1-16)"; done <<< "$SEL" | LC_ALL=C sort
          for t in "$SRC_N" "$SRC_N.dll"; do [ -f "$t" ] && shasum -a 256 < "$t"; done; } | shasum -a 256 | cut -c1-16) && UCE=$UC/$TID-$UK
fi
if [ -e "$OUT" ] && [ "$FORCE" != 1 ]; then
  echo "【红】${OUT} 已存在且不是本次输入的解包（旧键 $(cat "$OUT/.stage_unpack" 2> /dev/null || echo 无)）：确认可覆盖后加 --force" >&2; exit 1
fi
LOG=${LOG:-$OUT.unpack.log}; mkdir -p "$(dirname "$LOG")" || exit 1
TMP=$OUT.tmp.$$; rm -rf "$TMP"
trap 'rm -rf "$TMP"' EXIT
sel=(); while IFS=$'\t' read -r f _; do [ -n "$f" ] && sel+=("$f"); done <<< "$SEL"
t0=$SECONDS; HIT=0
if [ -n "$UCE" ] && [ -f "$UCE/exefs/main" ] && python3 "$R/tools/clonedir.py" "$UCE" "$TMP" > /dev/null 2>&1; then
  HIT=1; touch "$UCE"; echo "解包缓存命中：从 $UCE 克隆" > "$LOG"   # 改目录时刻：gc 按最近用过的留
else
  rm -rf "$TMP"
  # S2A_HDD_READ：本机读原件的锁前缀（我方机器设成 run/s2arm/bin/hdd_read.sh：机械盘只扛得住一个读者，普查拷贝在拷时先等）；没设为空、原样直跑
  if [ -n "$NUPF" ]; then S2A_NX_UNPACK=$SRC_N ${S2A_HDD_READ:-} python3 "$R/pack/ns2_nup.py" --to-extract "$NUPF" "$TMP" > "$LOG" 2>&1; rc=$?   # ZBIC 压缩的 NSO 由 ns2_dump 交 nx_unpack 解
  else ${S2A_HDD_READ:-} "$SRC_N" "$SRC_K" "$TMP" --aoc-index --title-id="$TID" "${sel[@]}" > "$LOG" 2>&1; rc=$?; fi
  if [ $rc != 0 ]; then
    echo "【红】解包失败（rc=${rc}）：$(tail -1 "$LOG" | cut -c1-200)（日志 ${LOG}）" >&2
    exit 1
  fi
fi
[ -f "$TMP/exefs/main" ] || { echo "【红】解包退出 0 但没有 exefs/main（日志 ${LOG}）" >&2; exit 1; }
[ -f "$TMP/unpack.tsv" ] || { echo "【红】解包退出 0 但没有 unpack.tsv（日志 ${LOG}）" >&2; exit 1; }
# 直连配置版本旁注：出包路径只按这里的产物走，任何一条新的出包路径都不必自己算版本（11-03 御天之剑HD 手测单漏写的根修）。
#   算法只一份（pack/direct_profile_input.py）：content_version = 最新更新的 CNMT 版本，没有更新取本体 CNMT 版本；
#   目录里已有旁注（旧解包目录 / 调用方给的）就保留，不覆盖。写不出（unpack.tsv 没有本体版本 = 工具太旧）报红：
#   直连是缺省，缺这份元数据刷新会停在「missing content version」，早报比晚报清楚。
python3 "$R/pack/direct_profile_input.py" "$TMP" || { echo "【红】直连配置版本旁注没写成（解包 ${TMP}）：$(tail -1 "$LOG" 2> /dev/null | cut -c1-160)" >&2; exit 1; }
echo "$K" > "$TMP/.stage_unpack"
OLD=
if [ -e "$OUT" ]; then OLD=$OUT.old.$$; mv "$OUT" "$OLD" || { echo "【红】挪开旧解包目录失败：${OUT}" >&2; exit 1; }; fi
if ! mv "$TMP" "$OUT"; then [ -z "$OLD" ] || mv "$OLD" "$OUT"; echo "【红】解包目录改名失败：${OUT}" >&2; exit 1; fi
trap - EXIT
[ -z "$OLD" ] || rm -rf "$OLD"
if [ $HIT = 1 ]; then echo "🟢 解包缓存命中：${OUT}（程序号 ${TID}，键 ${K}，从 ${UCE} 克隆，$((SECONDS - t0)) 秒）"; exit 0; fi
if [ -n "$UCE" ] && [ ! -e "$UCE" ]; then   # 存一份进缓存：失败只打黄（缓存是加速，不挡出包）
  CT=$UCE.tmp.$$; mkdir -p "$UC" && python3 "$R/tools/clonedir.py" "$OUT" "$CT" > /dev/null 2>&1 && { python3 -c 'import os, sys; os.rename(sys.argv[1], sys.argv[2])' "$CT" "$UCE" 2> /dev/null && du -sk "$UCE" | cut -f1 > "$UCE.size" || rm -rf "$CT"; } \
    || { rm -rf "$CT"; echo "🟡 解包结果没存进缓存 ${UC}（不影响本次）" >&2; }
fi
echo "🟢 解包完成：${OUT}（程序号 ${TID}，键 ${K}，$(printf '%s\n' "$SEL" | grep -c .) 个包，$(du -sh "$OUT" | cut -f1)，$((SECONDS - t0)) 秒）"
