#!/bin/bash
# 内存准入（11-04 维护者第 4 项；一个入口、一本共享账）：
#   bash tools/mem_admit.sh <类别> <款/键> [--cores N] -- <命令…>      （source 后也可直接调函数 mem_admit，参数同）
#   本机起 dsh / Metal 重活一律经它（AGENTS.md）；仓库内 dsh_batch build 在 translator/dsh_batch.py 里自动包上（谁起的都过同一本账，核数 = -j）
#   bash tools/mem_admit.sh --status                        看账：谁占着、峰值表
# 规则：本类别 / 本款的峰值（查表）+ 账内所有活着的预留 ≤ 物理内存 × S2A_MEM_PCT（缺省 70）%，且本作业核数 + 账内核数 ≤ 物理核数（hw.physicalcpu；S2A_MEM_CORES_MAX 可改），两样都够才放行；
#   核数 = --cores，缺省取调用方的 J / S2A_CPU_BAKE（tools/cores.sh 的分核口径），都没有记 0。11-04 维护者：准入只按内存放开后本机 CPU 打满（idle 0.2%、负载 41–45），每款都变慢。
#   上限取全部物理核、不扣普查跑局道的余量：各作业的 J 在入口已按「别人实际在用的核」扣过（tools/cores.sh），再扣一次会重复。不够就排队，
#   打一行【排队】写明等谁、差多少 GB（同写 admit.log）。队首放不下时后面放得下的可插空先走；前面有票排满 S2A_MEM_STARVE_S（缺省 900）秒、且它在本机当前「空闲 + 可回收」
#   内存下放得进，才给它预留、不再插空（防饿死，见 _ma_try）；放不进的票降回普通排队，不锁全机（11-04 晚：生键 dsh/fixture 估 50 GB 排满 900 秒后挡住后面 12 张票）。
#   单次预留本身 > 上限时只在账空且排在队首时放行（否则永远排不上），打一行【黄】；估值（非实测）超过本机物理内存 = 永远放不进，报【红】退出 125、不排队。
# 账本 = S2A_MEM_DIR（缺省 run 区 locks/mem，10-23 建的空目录，原先没人用）：use.<pid> 放行中的预留、q.<纳秒>.<pid> 排队票、peak.tsv 实测峰值表、admit.log。
#   单测不得记进这本共享账（11-04 两次：direct_profile_test 的 dsh/fixture 估 50 GB 排在真出库前面）：门禁步内（GATE_IN_STEP）或单测（S2A_TEST_ROOT）里
#   没显式给 S2A_MEM_DIR 时报【红】退出 125、不碰缺省目录（同 维护方工具 的单测约定；要真记共享账就显式给 S2A_MEM_DIR）。
#   互斥 = locks/mem.flock（plock.sh 内核文件锁，与出包锁同一份实现）；持有者进程不在了账自动作废（kill -0 判），不会有死账。
# 峰值表（11-04 维护者定）：① 本款在实测表 peak.tsv（$S2A_MEM_DIR 下，本机私有、可再生）有行就用本款实测；② 仓库估值表 tools/mem_peak.tsv 有本款的键就用它；
#   ③ 否则本类别在实测表的「实」行数 ≥ S2A_MEM_CLASS_MIN_N（缺省 10）时取本类别实测最大值 × S2A_MEM_CLASS_X（缺省 1.5，来源记「生键·同类实测最大×1.5」）；
#   ④ 本类别样本不够（生键、同类也没够数的实测）：按核数比例 = 上限 × 本作业核数（至少 1）÷ 物理核数，封顶上限的一半（来源记「生键·核数比例」）；
#      同类有 1–9 个样本时取它与「同类最大 × 1.5」中大的；估值表有本类别的下限行「<类别> *」（已量过的类别最大 × 1.5，审计 #1534 P1-113）时再与它取大。
#      下限行只在这一档用、不当缺省值：旧的 dsh * 50 是单进程无界时的量级，让单测生键一张票估 50 GB（11-04 晚事故），不恢复。生键一律打一行【黄】写明按什么估的。
#   来由：估值表 dsh * 50 是 dsh-mem2（9b306ec9f）之前单进程无界时的量级（见 tools/mem_peak.tsv 注释），11-04 本机实测表 52 款 dsh 实测中位 1.5 / p90 3.5 / 最大 5.2 GB；
#   没实测过的款按 50 GB 记账，上限 89.6 GB 下全机同时只放得进一个直连库，严格先来先放又让后面的小款一起等（一款第一方动作游戏直连库段本身 58 秒、排队 1692 秒，同时有款排 3132 秒）。
#   取最大值而非 p90、再乘 1.5：没实测过的款低估会多放、重演 11-02 内存吃光；最大值是已观测总体的上界，1.5 倍留给比样本更大的款，跑完即被本款实测覆盖。
#   每次跑完把本次实测峰值写回 peak.tsv（退出 0 = 覆盖；非 0 = 只升不降，被杀的局峰值不全）；下次按实测。取数 = 命令进程树的物理足迹合计（tools/mem_peak_sample.py，
#   与 iOS 判杀同口径，同 phys_footprint.py）。注意：Metal 真正编译在系统 MTLCompilerService（launchd 起、不在进程树里）不计入，metal 类的值靠估值表保守给。
# 开关：S2A_MEM_ADMIT=0 直接跑命令（不排队不记账，单测负控 / 应急）。等待上限 S2A_MEM_WAIT_S（缺省 14400）到点打【红】退出 125。
# 冷热：本文件与 tools/mem_peak.tsv、tools/mem_peak_sample.py 均不在 pack/stage_inputs/*.tsv 登记，不进任何阶段键。
set -u
MA_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MA_DIR=${S2A_MEM_DIR:-${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/locks/mem}
MA_LOCK=${S2A_MEM_LOCK:-$MA_DIR.flock}
MA_SEED=${S2A_MEM_SEED:-$MA_ROOT/tools/mem_peak.tsv}
MA_TAB=$MA_DIR/peak.tsv
MA_LOG=$MA_DIR/admit.log
MA_CHILD=; MA_USE=; MA_TK=; MA_WHY=; MA_SRC=
# 排队原因交给出包心跳（审计 #1534 P1-110）：调用方给了出包进度文件 S2A_PROGRESS（一键出包 / 启动器，tools/progress.sh）时，排队期间把最新一条【排队】行
#   写进 <进度文件>.wait（「进程号<TAB>排队行」），放行 / 不起时删掉；pack/oneclick/oneclick.sh 的心跳读它（进程不在了的不认）。
#   排队行原先只进 admit.log 与本阶段日志（如刷新里的 direct.out），心跳只看刷新日志末行，等内存准入时只打「刷新进行中」
MA_WAITF=${S2A_PROGRESS:+${S2A_PROGRESS}.wait}
_ma_wait_note() { [ -n "$MA_WAITF" ] || return 0; printf '%s\t%s\n' "$MA_ME" "$1" > "$MA_WAITF.$MA_ME" 2> /dev/null && mv -f "$MA_WAITF.$MA_ME" "$MA_WAITF" 2> /dev/null; return 0; }
_ma_wait_clear() { [ -n "$MA_WAITF" ] && [ -f "$MA_WAITF" ] && [ "$(cut -f1 "$MA_WAITF" 2> /dev/null)" = "${MA_ME:-}" ] && rm -f "$MA_WAITF"; return 0; }
# shellcheck source=plock.sh
source "$MA_ROOT/tools/plock.sh"

_ma_say() { echo "$*" >&2; echo "$(date '+%m-%d %H:%M:%S') $*" >> "$MA_LOG" 2> /dev/null; return 0; }
_ma_t10() { awk -v x="$1" 'BEGIN{printf "%d", int(x*10+0.99)}'; }   # GB → 十分之一 GB（向上取整）：bash 3.2 没浮点，账内全用整数
_ma_gb() { awk -v t="$1" 'BEGIN{printf "%.1f", t/10}'; }
_ma_ns() { python3 -c 'import time; print(time.time_ns())'; }
# 物理内存字节：S2A_MEM_PHYS_BYTES（单测）> macOS sysctl hw.memsize > /proc/meminfo 的 MemTotal（Linux、Windows Git Bash 都有；S2A_MEM_MEMINFO 单测给假文件）。
#   审计 #1534 P1-114：原先只读 sysctl，没有它的出包机按 0，凡估值都「超过本机物理内存 0.0 GB」退 125
_ma_physbytes() {
  [ -n "${S2A_MEM_PHYS_BYTES:-}" ] && { echo "$S2A_MEM_PHYS_BYTES"; return; }
  local b; b=$(sysctl -n hw.memsize 2> /dev/null)
  [ -n "$b" ] || b=$(awk '/^MemTotal:/{printf "%.0f", $2*1024}' "${S2A_MEM_MEMINFO:-/proc/meminfo}" 2> /dev/null)
  echo "${b:-0}"
}
_ma_total10() {
  if [ -n "${S2A_MEM_TOTAL_GB:-}" ]; then _ma_t10 "$S2A_MEM_TOTAL_GB"; return; fi
  awk -v b="$(_ma_physbytes)" -v p="${S2A_MEM_PCT:-70}" 'BEGIN{printf "%d", b/1073741824*p/100*10}'
}
_ma_find() { [ -f "$1" ] || return 0; awk -F'\t' -v c="$2" -v k="$3" '/^#/{next} $1==c&&$2==k{print $3, $4; exit}' "$1"; }
# 实测表里本类别「实」行：样本数 ≥ S2A_MEM_CLASS_MIN_N（缺省 10）→ 「最大值 × S2A_MEM_CLASS_X（缺省 1.5） 同类实测最大×1.5」；样本不够 → 空
_ma_cmax() { [ -f "$1" ] || return 0; LC_ALL=C awk -F'\t' -v c="$2" -v n0="${S2A_MEM_CLASS_MIN_N:-10}" -v x="${S2A_MEM_CLASS_X:-1.5}" '/^#/{next} $1==c&&$4=="实"{n++; if ($3+0>m) m=$3+0} END{if (n >= n0 && m > 0) printf "%.1f 同类实测最大×%s\n", m*x, x}' "$1"; }
_ma_need() {   # 类别 键 → 「十分之一GB 来源」（实 / 估 / 生键·同类实测最大×1.5 / 生键·核数比例…；实测表、估值表都没有本键 = 生键）；要先有 MA_TOTAL10 / MA_NCORES / MA_CORES
  local v src c n p10 f
  v=$(_ma_find "$MA_TAB" "$1" "$2"); [ -n "$v" ] || v=$(_ma_find "$MA_SEED" "$1" "$2")
  [ -n "$v" ] || { v=$(_ma_cmax "$MA_TAB" "$1"); [ -n "$v" ] && v="${v%% *} 生键·${v#* }"; }
  if [ -z "$v" ]; then   # ④ 生键：核数比例，封顶上限一半；同类有不够数的样本时取大
    c=$MA_CORES; [ "$c" -ge 1 ] || c=1; [ "$c" -le "$MA_NCORES" ] || c=$MA_NCORES
    p10=$((MA_TOTAL10 * c / MA_NCORES)); [ "$p10" -le $((MA_TOTAL10 / 2)) ] || p10=$((MA_TOTAL10 / 2))
    v="$(_ma_gb "$p10") 生键·核数比例（${c}/${MA_NCORES} 核 × 上限 $(_ma_gb "$MA_TOTAL10") GB，封顶一半）"
    n=$(S2A_MEM_CLASS_MIN_N=1 _ma_cmax "$MA_TAB" "$1")
    [ -n "$n" ] && [ "$(_ma_t10 "${n%% *}")" -gt "$p10" ] && { v="${n%% *} 生键·同类实测最大×${S2A_MEM_CLASS_X:-1.5}（样本不足 ${S2A_MEM_CLASS_MIN_N:-10}，大于核数比例 $(_ma_gb "$p10") GB）"; p10=$(_ma_t10 "${n%% *}"); }
    # 类别下限（审计 #1534 P1-113）：仓库估值表里「<类别><TAB>*」一行 = 这一类在已量过的机器上的实测上界（不是缺省值），只在这一档取大，
    #   让空实测表的机器不按核数比例少占账（8 GB / 8 核：核数比例封顶 2.8 GB，tools/cores.sh 给直连库按 1.7 + 1.0 + 7 × 0.42 ≈ 5.7 GB 开 8 路）
    f=$(_ma_find "$MA_SEED" "$1" '*')
    [ -n "$f" ] && [ "$(_ma_t10 "${f%% *}")" -gt "$p10" ] && v="${f%% *} 生键·类别下限（估值表 ${1} *，样本不足 ${S2A_MEM_CLASS_MIN_N:-10}，大于核数比例与同类样本）"
  fi
  src=${v#* }; [ "$src" != "$v" ] || src=估
  echo "$(_ma_t10 "${v%% *}") ${src:-估}"
}
_ma_phys10() { awk -v b="$(_ma_physbytes)" 'BEGIN{printf "%d", b/1073741824*10}'; }
# 本机当前「空闲 + 可回收」内存（十分之一 GB）：vm_stat 的 free + inactive + speculative 页（macOS 不换页即可收回的部分）；Linux 取 MemAvailable。
#   S2A_MEM_AVAIL_GB 可直接给（单测）。读不到时回物理内存（等同旧行为：防饿死不看实机内存）
_ma_avail10() {
  [ -n "${S2A_MEM_AVAIL_GB:-}" ] && { _ma_t10 "$S2A_MEM_AVAIL_GB"; return; }
  local v
  v=$(vm_stat 2> /dev/null | awk '/page size of/{for (i=1;i<=NF;i++) if ($i=="of") ps=$(i+1)} /^Pages (free|inactive|speculative):/{gsub(/\./,"",$NF); n+=$NF} END{if (ps>0 && n>0) printf "%d", n*ps/1073741824*10}')
  [ -n "$v" ] || v=$(awk '/^MemAvailable:/{printf "%d", $2/1048576*10}' /proc/meminfo 2> /dev/null)
  echo "${v:-$(_ma_phys10)}"
}
_ma_reap() {   # 清死账 / 死排队票（持有者进程不在了）
  local f p
  for f in "$MA_DIR"/use.* "$MA_DIR"/q.*; do [ -e "$f" ] || continue
    p=$(head -1 "$f" 2> /dev/null | cut -f1)
    { [ -n "$p" ] && kill -0 "$p" 2> /dev/null; } || rm -f "$f"
  done
}
_ma_used10() { cat "$MA_DIR"/use.* 2> /dev/null | awk -F'\t' '{s+=$2} END{printf "%d", s+0}'; }
_ma_usedc() { cat "$MA_DIR"/use.* 2> /dev/null | awk -F'\t' '{s+=$6} END{printf "%d", s+0}'; }   # 第 6 列 = 核数（旧版写的 5 列行记 0 核）
_ma_ncores() { local n=${S2A_MEM_CORES_MAX:-$(sysctl -n hw.physicalcpu 2> /dev/null || getconf _NPROCESSORS_ONLN 2> /dev/null)}; [ "${n:-0}" -ge 1 ] 2> /dev/null && echo "$n" || echo 1; }
_ma_list() {   # 参数：文件通配前缀（use / q）→ 「类别/键(pid,GB) …」
  local f; for f in "$MA_DIR"/$1.*; do [ -e "$f" ] || continue
    awk -F'\t' '{printf "%s/%s(pid %s,%s GB,%d 核) ", $3, $4, $1, $2/10, $6}' "$f"; done
}
# 在账锁内判一次：0 放行（已写账、票已撤）/ 1 内存不够 / 2 没轮到（排在前面、已排满 S2A_MEM_STARVE_S 的票给它预留）。MA_WHY 带原因（不含会变的墙钟，供去重）
#   插空（11-04 维护者）：队首放不下时，后面放得下的先走；但前面有票已排满 S2A_MEM_STARVE_S（缺省 900 秒）就给它预留，排在它后面的不再插空（防大件饿死）。
#   单次预留本身 > 上限的只在账空时放行，且只给队首（否则永远排不上）
_ma_try() {
  local head used usedc f now t0 n0 av= old=
  _ma_reap
  head=$(ls "$MA_DIR"/q.* 2> /dev/null | sort | head -1)
  used=$(_ma_used10); now=$(date +%s)
  if [ -z "$MA_PRIO" ]; then local pq; pq=$(ls "$MA_DIR"/q.0* 2> /dev/null | head -1)
    [ -n "$pq" ] && { MA_WHY="优先类别 $(awk -F'\t' '{printf "%s/%s(pid %s)", $3, $4, $1}' "$pq" 2> /dev/null) 在等，普通作业不插空（S2A_MEM_PRIO_CLASSES）"; return 2; }; fi
  # 通配直接遍历（按名排序，同 ls | sort）：原先 for f in $(ls …) 按空格切词，玩家机账本在「Application Support」下，每张票的路径被切成三段，
  #   别人的票一张也读不到、自己的票也认不出（#235 玩家 Mac 出包在这里报 unbound variable 停下）。
  #   票的发车时刻 / 预留量不是纯数字（别的版本写的、写了一半的）就跳过这张：set -u 下进算术会让整个出包停下
  for f in "$MA_DIR"/q.*; do [ -e "$f" ] || continue
    [ "$f" = "$MA_TK" ] && break
    t0=$(cut -f5 "$f" 2> /dev/null); case $t0 in ''|*[!0-9]*) continue;; esac
    [ $((now - t0)) -ge "${S2A_MEM_STARVE_S:-900}" ] || continue
    # 只给本机当前「空闲 + 可回收」放得进的票预留；放不进的降回普通排队（不占防饿死位），看下一张（11-04 晚：生键 50 GB 一直放不进、挡住后面 12 张票）
    n0=$(cut -f2 "$f" 2> /dev/null); case $n0 in ''|*[!0-9]*) continue;; esac; [ -n "$av" ] || av=$(_ma_avail10)
    [ "$n0" -le "$av" ] 2> /dev/null && { old=$f; break; }
  done
  if [ -n "$old" ]; then
    MA_WHY="前面 $(awk -F'\t' '{printf "%s/%s(pid %s)", $3, $4, $1}' "$old" 2> /dev/null) 已排满 ${S2A_MEM_STARVE_S:-900} 秒、本机空闲 + 可回收放得进，给它预留、不插空（账内已占 $(_ma_gb "$used") GB / 上限 $(_ma_gb "$MA_TOTAL10") GB）"
    return 2
  fi
  usedc=$(_ma_usedc)
  if { [ $((used + MA_NEED10)) -le "$MA_TOTAL10" ] && [ $((usedc + MA_CORES)) -le "$MA_NCORES" ]; } || { [ "$used" -eq 0 ] && [ "$usedc" -eq 0 ] && [ "$head" = "$MA_TK" ]; }; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$MA_ME" "$MA_NEED10" "$MA_CLS" "$MA_KEY" "$(date +%s)" "$MA_CORES" > "$MA_USE"
    rm -f "$MA_TK"; MA_USED10=$used; MA_USEDC=$usedc
    return 0
  fi
  if [ $((used + MA_NEED10)) -le "$MA_TOTAL10" ]; then
    MA_WHY="需 ${MA_CORES} 核，账内已占 ${usedc} 核 / 上限 ${MA_NCORES} 核（物理核数），差 $((usedc + MA_CORES - MA_NCORES)) 核；内存够（需 $(_ma_gb "$MA_NEED10") GB，已占 $(_ma_gb "$used") GB / 上限 $(_ma_gb "$MA_TOTAL10") GB）；等：$(_ma_list use)"
    return 1
  fi
  MA_WHY="需 $(_ma_gb "$MA_NEED10") GB（${MA_SRC}），账内已占 $(_ma_gb "$used") GB / 上限 $(_ma_gb "$MA_TOTAL10") GB，差 $(_ma_gb $((used + MA_NEED10 - MA_TOTAL10))) GB；等：$(_ma_list use)"
  return 1
}
# 命令签名 = 前 3 个参数的文件名部分（去目录：同一命令在不同工作树里路径不同），写在实测表第 6 列
_ma_sig() { local a s=; for a in "${@:1:3}"; do s="$s ${a##*/}"; done; s=${s# }; printf '%s' "${s//$'\t'/ }" | tr '\n' ' '; }
# 写回实测峰值表：类别 键 十分之一GB 退出码 命令签名。退出 0 覆盖、非 0 只升不降；
#   往低改只认同一命令（11-04 Switch2 线借 dsh/memrt_host0 的键跑别的命令，实测 0.4 把 5.7 覆盖了 → 该键下次少预留、可能吃光内存）：
#   表里本键签名与本次不同（或旧行没签名）时只升不降，把本次签名记上；签名相同才按实测往低改
_ma_writeback() {
  local row old=0 osig= new=$3 sig=${5:-} g
  row=$(awk -F'\t' -v c="$1" -v k="$2" '/^#/{next} $1==c&&$2==k{print $3 "\t" $6; exit}' "$MA_TAB" 2> /dev/null)
  [ -n "$row" ] && { old=$(_ma_t10 "${row%%$'\t'*}"); osig=${row#*$'\t'}; }
  if [ -n "$row" ] && [ "$new" -lt "$old" ] && { [ "$4" != 0 ] || [ "$osig" != "$sig" ]; }; then
    [ "$4" = 0 ] && [ -n "$osig" ] && _ma_say "【黄】mem_admit $1/${2}：本次命令「${sig}」与该键登记的「${osig}」不同，实测 $(_ma_gb "$new") GB 低于表里 $(_ma_gb "$old") GB，不往低改（借键不污染别人的实测）"
    [ "$4" = 0 ] && [ -z "$osig" ] || return 0
    new=$old
  fi
  [ "$4" != 0 ] && [ "$new" -le "$old" ] && return 0
  g=$(_ma_gb "$new")
  [ -f "$MA_TAB" ] || : > "$MA_TAB"
  awk -F'\t' -v OFS='\t' -v c="$1" -v k="$2" -v g="$g" -v d="$(date +%F)" -v sg="$sig" '$1==c&&$2==k{next} {print} END{print c, k, g, "实", d, sg}' "$MA_TAB" > "$MA_TAB.tmp.$$" && mv "$MA_TAB.tmp.$$" "$MA_TAB"
}
mem_admit() {
  local rc r lp t0=$SECONDS tsay=$SECONDS said= src smp pk new10 key cls
  cls=${1:?类别}; key=${2:?款/键}; shift 2
  MA_CORES=${J:-${S2A_CPU_BAKE:-0}}; [ "${1:-}" = --cores ] && { MA_CORES=${2:?核数}; shift 2; }   # 本作业占的核（缺省取调用方的 J / S2A_CPU_BAKE，同 tools/cores.sh 分核口径；都没有记 0 = 不占核账）
  [ "$MA_CORES" -ge 0 ] 2> /dev/null || MA_CORES=0
  [ "${1:-}" = -- ] && shift
  [ $# -gt 0 ] || { echo "用法：mem_admit <类别> <款/键> [--cores N] -- <命令…>" >&2; return 2; }
  [ "${S2A_MEM_ADMIT:-1}" = 0 ] && { "$@"; return $?; }
  if [ -z "${S2A_MEM_DIR:-}" ] && { [ -n "${GATE_IN_STEP:-}" ] || [ -n "${S2A_TEST_ROOT:-}" ]; }; then
    echo "【红】mem_admit $cls/${key}：在门禁步 / 单测里（GATE_IN_STEP / S2A_TEST_ROOT）却没给 S2A_MEM_DIR，拒绝记进共享账本 ${MA_DIR}；单测请设自己的 S2A_MEM_DIR（或 S2A_MEM_ADMIT=0）" >&2
    return 125
  fi
  MA_CLS=$cls; MA_KEY=$key; MA_ME=${BASHPID:-$$}; MA_USED10=0
  MA_TOTAL10=$(_ma_total10); MA_NCORES=$(_ma_ncores); MA_USEDC=0
  read -r MA_NEED10 src <<< "$(_ma_need "$cls" "$key")"; MA_SRC=$src   # 来源写进排队行（11-04：看得出等的是缺省估值还是实测）
  mkdir -p "$MA_DIR" || return 2
  case $src in 生键*) _ma_say "【黄】mem_admit $cls/${key}：生键（实测表、估值表都没有本键），按${src#生键·}估 $(_ma_gb "$MA_NEED10") GB";; esac
  if [ "$src" != 实 ] && [ "$MA_NEED10" -gt "$(_ma_phys10)" ]; then
    _ma_say "【红】mem_admit $cls/${key}：估值 $(_ma_gb "$MA_NEED10") GB（${src}）超过本机物理内存 $(_ma_gb "$(_ma_phys10)") GB，永远放不进，不排队、不起"
    return 125
  fi
  # 优先类别（11-07 维护者：上线在即启动器最高优先，它的 App 打包在排队等 CPU）：S2A_MEM_PRIO_CLASSES（缺省 appbuild）里的类别排队票名带 0 前缀、
  #   排在所有普通票前面；有活的优先票在等时，普通票不插空（_ma_try 返回 2）。不抢已放行的作业
  MA_PRIO=; case " ${S2A_MEM_PRIO_CLASSES-appbuild} " in *" $MA_CLS "*) MA_PRIO=0;; esac
  MA_USE=$MA_DIR/use.$MA_ME; MA_TK=$MA_DIR/q.$MA_PRIO$(_ma_ns).$MA_ME
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$MA_ME" "$MA_NEED10" "$cls" "$key" "$(date +%s)" "$MA_CORES" > "$MA_TK"
  while :; do
    plock_take "$MA_LOCK" 60; rc=$?
    [ $rc = 0 ] || { rm -f "$MA_TK"; _ma_wait_clear; _ma_say "【红】mem_admit：账锁 $MA_LOCK 取不到（rc=${rc}），$cls/$key 不起"; return 125; }
    lp=$PLOCK_PID; _ma_try; r=$?; plock_release "$lp"
    [ $r = 0 ] && break
    if [ "$MA_WHY" != "$said" ] || [ $((SECONDS - tsay)) -ge "${S2A_MEM_SAY_S:-300}" ]; then
      _ma_say "【排队】mem_admit $cls/${key}：$MA_WHY"; _ma_wait_note "【排队】mem_admit $cls/${key}：$MA_WHY"; said=$MA_WHY; tsay=$SECONDS
    fi
    if [ $((SECONDS - t0)) -ge "${S2A_MEM_WAIT_S:-14400}" ]; then
      rm -f "$MA_TK"; _ma_wait_clear; _ma_say "【红】mem_admit $cls/$key 排了 $((SECONDS - t0)) 秒仍没放行（${MA_WHY}），不起"; return 125
    fi
    sleep "${S2A_MEM_POLL:-2}"
  done
  _ma_wait_clear
  [ "$MA_NEED10" -le "$MA_TOTAL10" ] || _ma_say "【黄】mem_admit $cls/${key}：单次预留 $(_ma_gb "$MA_NEED10") GB 超过上限 $(_ma_gb "$MA_TOTAL10") GB，账空才放行，独占跑"
  _ma_say "mem_admit 放行 $cls/${key}：预留 $(_ma_gb "$MA_NEED10") GB（${src}），账内 $(_ma_gb $((MA_USED10 + MA_NEED10))) GB / 上限 $(_ma_gb "$MA_TOTAL10") GB、${MA_CORES} 核（账内 $((MA_USEDC + MA_CORES)) / ${MA_NCORES} 核），等了 $((SECONDS - t0)) 秒"
  pk=$MA_DIR/pk.$MA_ME; rm -f "$pk"
  "$@" <&0 & MA_CHILD=$!
  python3 -B "$MA_ROOT/tools/mem_peak_sample.py" "$MA_CHILD" "$pk" "${S2A_MEM_SAMPLE_S:-1}" 2> /dev/null & smp=$!
  wait "$MA_CHILD"; rc=$?
  while kill -0 "$MA_CHILD" 2> /dev/null; do wait "$MA_CHILD"; rc=$?; done   # 被转发的信号打断 wait 时，等命令真退出
  wait "$smp" 2> /dev/null
  new10=0; [ -s "$pk" ] && new10=$(awk '{printf "%d", int($1/1048576*10+0.99)}' "$pk"); rm -f "$pk"
  if [ "$new10" -gt 0 ]; then
    plock_take "$MA_LOCK" 60 && { lp=$PLOCK_PID; _ma_writeback "$cls" "$key" "$new10" "$rc" "$(_ma_sig "$@")"; plock_release "$lp"; }
    _ma_say "mem_admit 记 $cls/${key}：实测峰值 $(_ma_gb "$new10") GB（预留 $(_ma_gb "$MA_NEED10") GB，rc=${rc}）已写回 peak.tsv"
    [ "$new10" -le "$MA_NEED10" ] || _ma_say "【黄】mem_admit $cls/${key}：实测 $(_ma_gb "$new10") GB 超过预留 $(_ma_gb "$MA_NEED10") GB（${src}），下次按实测"
  fi
  rm -f "$MA_USE"; MA_CHILD=
  return "$rc"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  if [ "${1:-}" = --status ]; then
    _ma_reap; echo "上限 $(_ma_gb "$(_ma_total10)") GB、$(_ma_ncores) 核；占用 $(_ma_gb "$(_ma_used10)") GB、$(_ma_usedc) 核；在用：$(_ma_list use)；排队：$(_ma_list q)"
    echo '--- 实测峰值表 ---'; cat "$MA_TAB" 2> /dev/null; exit 0
  fi
  trap '[ -n "$MA_CHILD" ] && kill -TERM "$MA_CHILD" 2> /dev/null' TERM INT
  trap 'rm -f "$MA_USE" "$MA_TK" 2> /dev/null; _ma_wait_clear' EXIT
  mem_admit "$@"; exit $?
fi
