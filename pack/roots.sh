#!/bin/bash
# 版本化产物根（唯一写法；底本旧库 run/pkgline/pkg_daemon3.sh 的 roots 段）：source pack/roots.sh 后用
#   W=$(root_begin <款> <提交>)   新建 <根>/<款>@<提交>.tmp：从现行版本整目录 clonefile（不占新空间、秒级），出包只往 W 里写
#   root_commit <款> <提交>       W.tmp → <款>@<提交>，再原子切 <款>@现行 → 它（rename(2) 换软链，读方任何时刻看到的都是完整的某一版）
#                                 并发保护：提交时现行必须还是开工时克隆的那一版，否则报红中止、不切（10-19 两个出包阶段并发，后提交的把先提交的宽屏改写静默盖掉）
#                                 例外：阶段声明了自己的产物路径 ROOT_REPLAY="<相对路径或通配…>" 时，从新现行克隆一份、把这些路径换成本阶段的（W 里没有的 = 删），再提交
#                                 （!<路径> = 声明路径里套着的别的阶段产物，换完后恢复成新现行的：着色器阶段 'shaders !shaders/direct'）
#                                 ——产物互不重叠的阶段可以并行（pack/refresh.sh：改写 / 系统数据 / 界面布局）；提交本身持 <根>/.<款>.commit 锁串行
#   root_tag <款> <标签>          给标签找一个没用过的版本目录名（标签已占用就加 -2、-3…）
#   root_stage_done <现行目录> <阶段> <键>   现行版本里记的该阶段键 = <键> 时返回 0（阶段已做过、且就在现行这一版里）；各阶段提交前写 .stage_<阶段>
#   root_abort <款> <提交>        出包失败：删 W.tmp，现行不动
#   root_cur <款>                 打印现行版本的实际目录（读方一律先解析成实际目录再读，别跟着软链读到一半被切换）
# 每款只留现行 + 上一版（10-19 维护者：磁盘）。根目录 S2A_ROOTS，缺省 ~/.s2arm/run/s2arm/roots。
# RomFS 共享存储（11-03 维护者「火纹冷出包 30 分钟」：Switch 2 火纹 RomFS 27.4 万条目，每个阶段开工克隆整棵根、提交后删旧版，Spare-host 上各约 14 秒、7 个阶段累计 3–4 分钟）：
#   RomFS 是不可变的游戏输入，只有播种与界面布局阶段换它的内容。各版本的 romfs 是指向 $S2A_ROOTS/.romfs_store/<款>/<身份>/ 的相对软链（../.romfs_store/<款>/<身份>），
#   身份目录整棵只读（chmod a-w：阶段往 romfs 写 / 删 / 建都会 EACCES 响亮失败，改不到共享份）；root_begin 克隆时跳过 romfs、照抄这条软链（不再逐条目克隆），
#   提交时 W 里的 romfs 若是真目录（播种、ROOT_OWN_ROMFS=1 开工的阶段、旧格式根克隆来的）就整棵搬进存储成新身份（rename，不拷）；
#   修剪版本后删没有任何版本（含 .tmp / 重放中的 .tmp.rb）再指向的身份（root_romfs_sweep）。整根往别处复制的读方（tools/clonedir.py 的快照、私有根起步、
#   ipa.sh 只出程序、rsync 推远端）跟随这条软链拿到真目录，内容与原先逐字节相同。要改 RomFS 的阶段用 ROOT_OWN_ROMFS=1 root_begin 拿一份自己的真目录。
# 禁止原地覆盖：已提交的版本目录不许再写（root_begin 对已存在的 <款>@<提交> 报红）。
# 共享根只代表 origin/main（10-20 维护者拍板）：往共享根开新版本前核本树 HEAD 在 origin/main 上（不是分支）、阶段相关目录（runtime ios graphics translator services compat pack）干净、
#   且不比现行的来源提交（.root_src）旧，否则拒绝——分支树按各自代码算阶段键，会和别的树轮流刷共享现行（10-20 01006F8002326000现行被三棵树乒乓，在途出包反复被并发保护作废），分支版本还会冒充现行。
#   分支树用私有根：S2A_ROOTS=~/.s2arm/run/s2arm/roots_priv/<名>（约定放这里，gc 24 小时没动就清）；某款在私有根里还没有现行时，从共享现行 clonefile 起步，只重做键变了的阶段。
S2A_ROOTS_SHARED=${S2A_ROOTS_SHARED:-${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/roots}; S2A_ROOTS_PRIV=${S2A_ROOTS_PRIV:-${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/roots_priv}   # 单测可覆盖
S2A_ROOTS=${S2A_ROOTS:-$S2A_ROOTS_SHARED}
# macOS 保留 clonefile 复制；Linux 使用保留元数据的独立拷贝，错误直接上报。
_root_cp() {
  case $(uname -s) in
    Darwin) cp -c "$@" || return $? ;;
    Linux) cp -p "$@" || return $? ;;
    *) echo "【红】不支持的目录复制宿主" >&2; return 1 ;;
  esac
}
# 不跟随现行软链，保持同目录 rename 的原子切换。
_root_mv_link() {
  [ "$#" = 2 ] || return 1
  if [ -d "$2" ] && [ ! -L "$2" ]; then
    echo "【红】现行目标是目录，拒绝切换" >&2; return 1
  fi
  case $(uname -s) in
    Darwin) mv -fh "$@" || return $? ;;
    Linux) mv -fT "$@" || return $? ;;
    *) echo "【红】不支持的软链切换宿主" >&2; return 1 ;;
  esac
}
# Canonicalize existing and first-run paths without starting Python. Resolve
# symlink components before processing .., including links to absent targets.
_root_resolve_dir() (
  local pending=$1 resolved part candidate target links=0
  case $pending in /*) resolved=/;; *) resolved=$(pwd -P);; esac
  while [ -n "$pending" ]; do
    part=${pending%%/*}
    if [ "$part" = "$pending" ]; then pending=; else pending=${pending#*/}; fi
    case $part in ''|.) continue;; ..) resolved=${resolved%/*}; [ -n "$resolved" ] || resolved=/; continue;; esac
    candidate=${resolved%/}/$part
    if [ -L "$candidate" ]; then
      links=$((links + 1)); [ "$links" -le 40 ] || return 1
      target=$(readlink "$candidate") || return 1
      case $target in /*) resolved=/;; esac
      pending=$target${pending:+/$pending}
    else
      resolved=$candidate
    fi
  done
  printf '%s\n' "$resolved"
)
_root_shared() {
  local actual shared
  # Unresolvable links fail closed: retain the shared-root write guard.
  actual=$(_root_resolve_dir "$S2A_ROOTS") || return 0
  shared=$(_root_resolve_dir "$S2A_ROOTS_SHARED") || return 0
  [ "$actual" = "$shared" ]
}
# 包身份专用（pack/ipa.sh：真 = 正式 io.s2arm.<款>，假 = 实验 io.s2arm-exp.<款>）：安全方向与上面的写守卫相反——
#   两边都解析成功且是同一路径才算共享；解析失败（循环软链等）= 私有 = 实验身份，不让异常配置下的实验包冒充正式身份覆盖测试设备正式 App（10-21 出过）
_root_identity_shared() {
  local actual shared
  actual=$(_root_resolve_dir "$S2A_ROOTS") || return 1
  shared=$(_root_resolve_dir "$S2A_ROOTS_SHARED") || return 1
  [ "$actual" = "$shared" ]
}
_root_tree_ok() {   # 本树能不能改共享现行（$1 = 款）：HEAD 是 origin/main 上的提交（不是分支）、阶段相关目录干净、不比现行的来源提交旧（只许往前，不乒乓）
  local r why= src cur; r=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
  # 不联网：main 的每个提交都是本机列车合出来再推上去的，origin/main 本机已是最新（11-02 维护者「咱们是唯一的合并者」）
  git -C "$r" merge-base --is-ancestor HEAD origin/main 2> /dev/null || why="不在 origin/main 上（分支提交）"
  [ -z "$(git -C "$r" status --porcelain -- runtime ios graphics translator services compat pack 2>/dev/null)" ] || why="$why 阶段相关目录有未提交改动"
  cur=$(cd -P "$S2A_ROOTS/${1:-?}@现行" 2>/dev/null && pwd); src=$(cat "$cur/.root_src" 2>/dev/null)
  [ -z "$why" ] && [ -n "$src" ] && ! git -C "$r" merge-base --is-ancestor "$src" HEAD 2> /dev/null && why="比现行的来源提交 ${src:0:9} 旧（会把现行刷回去）"
  [ -z "$why" ] && return 0
  echo "【红】本树（$(git -C "$r" rev-parse --short HEAD)）${why}：共享产物根只代表 main，不许改共享现行。分支树用私有根：S2A_ROOTS=${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/roots_priv/<名> 再跑（从共享现行克隆起步，只重做变了的阶段）；旧 main 树先 git checkout origin/main" >&2
  return 1
}
# 克隆会带上源目录的只读权限（d2r romfs/data），先给自己写权限再删。
# 软链本身只删链接：GNU chmod -R 会顺着命令行上的软链改到 RomFS 共享存储，把只读保护打开
# 删的同时有程序往里写系统文件（#369：游戏库在外置盘，访达往正在删的目录写 .DS_Store，rm 报 Directory not empty；同样的文件放内置盘就成功）：
#   rm 没删干净就只清系统自动生成的文件（名单同 pack/jobs/layout.py OS_JUNK / ._ 前缀，tests/os_junk_sync_test.sh 核一致）再删，最多 5 次；
#   最后一次 rm 的报错照常输出（_root_rm_red 按它报原因）
_ROOT_OS_JUNK=(".DS_Store" "desktop.ini" "Thumbs.db")
_root_rm() {
  [ -L "$1" ] && { rm -f "$1"; return; }; [ -e "$1" ] || return 0; chmod -R u+w "$1" 2>/dev/null
  local i n a=(); for n in "${_ROOT_OS_JUNK[@]}"; do a+=(-name "$n" -o); done; a+=(-name '._*')
  for i in 1 2 3 4; do rm -rf "$1" 2>/dev/null; [ -e "$1" ] || return 0; find "$1" -type f \( "${a[@]}" \) -delete 2>/dev/null; sleep 0.2; done
  rm -rf "$1"
}
# _root_rm_red <路径> [谁]：同 _root_rm；删不掉只打一行【红】（路径 + rm 报错条数 + 首条原因），rm 的逐文件报错不外漏，返回 1。
#   删产物根 / 它的克隆 / 临时副本的收尾点都走它（11-04 只出程序 pack/ipa.sh 的临时克隆 romfs 只读、裸 rm -rf 删不掉，逐文件报 Permission denied）
# 被取代的整根 / 重放后丢弃的工作根：同卷改名挪进 $S2A_ROOTS/.trash/（瞬间完成），再由脱离本进程的后台删（不占出包时间、不在提交锁里删）。
#   11-05 一次成功线：0400486024DEC000资源包 18.7 万文件，出包收尾「重放 + 修剪」在提交锁里把整根逐文件删 / 拷了 2–3 遍，出包 331 秒里约 234 秒没计时。
#   后台删被打断（进程被杀）留下的残余由 tools/gc.sh 按名字里的 pid 已不在清掉。挪不动（跨卷等）退回当场删
_root_trash() {
  [ -L "$1" ] && { rm -f "$1"; return; }; [ -e "$1" ] || return 0
  local t="$S2A_ROOTS/.trash/$(basename "$1").$$.$RANDOM"
  mkdir -p "$S2A_ROOTS/.trash" 2> /dev/null && mv "$1" "$t" 2> /dev/null || { _root_rm "$1"; return; }
  ( ( _root_rm "$t" ) > /dev/null 2>&1 & )   # 两层子 shell：删的进程不是调用方的子进程，调用方的 wait 不等它
}
_root_rm_red() { local e; e=$(_root_rm "$1" 2>&1 > /dev/null | awk 'NR == 1 {f = $0} END {if (NR) print NR " 条报错，首条：" f}'); [ -e "$1" ] || [ -L "$1" ] || return 0; echo "【红】${2:+$2：}删不掉 ${1}（${e:-rm 没报原因}）"; return 1; }
_root_dir() { echo "$S2A_ROOTS/$1@$2"; }
_ROOT_STORE=.romfs_store
# _root_romfs_link <版本目录>：romfs 是合规的存储软链 → 打印链接原文（../.romfs_store/<本款>/<身份>）；是真目录 / 没有 → 不打印、返回 0；
#   其余一律【红】返回 1：指向别处、身份带 / 或以 . 开头、身份目录不在 / 本身是软链（只许指向本款存储里的一个真目录）
_root_romfs_link() {
  local v=$1 g t id d
  [ -L "$v/romfs" ] || return 0
  g=$(basename "$v"); g=${g%%@*}; t=$(readlink "$v/romfs") || return 1
  id=${t#"../$_ROOT_STORE/$g/"}
  case $id in ''|*/*|.*|"$t") echo "【红】$v/romfs 是指向 $t 的软链：只许指向本款 RomFS 存储 ../$_ROOT_STORE/$g/<身份>" >&2; return 1;; esac
  d=$(dirname "$v")/$_ROOT_STORE/$g/$id
  [ -d "$d" ] && [ ! -L "$d" ] || { echo "【红】$v/romfs 指向的 RomFS 存储 $d 不在" >&2; return 1; }
  printf '%s\n' "$t"
}
# _root_romfs_intern <W.tmp> <款>：W 里的 romfs 是真目录 → 整棵搬进存储成新身份（同卷 rename，不拷）、原处换成软链、身份目录整棵只读。
#   先建软链再搬进存储：任何时刻存储里出现的身份都已有版本指向，并发的 root_romfs_sweep 不会把它当无主删掉。
#   chmod 同权限不动 ctime：从存储克隆来的文件本来就只读，资源键缓存（pack/resource_input_key.py 按 ctime 判）不失效；只有新写的文件与目录真被改
_root_romfs_intern() {
  local w=$1 g=$2 st id n=1
  [ -d "$w/romfs" ] && [ ! -L "$w/romfs" ] || return 0
  st=$(dirname "$w")/$_ROOT_STORE/$g; mkdir -p "$st" || return 1
  id=$(basename "$w"); id=${id#*@}; id=${id%%.tmp*}-$(date +%s); while [ -e "$st/$id" ] || [ -L "$st/$id" ]; do n=$((n + 1)); id=${id%~*}~$n; done
  # 跨目录 rename 一个目录要它自己可写（改 ..）：开工时已只读的（_root_romfs_seal）先只给顶层加回写位，搬完再整棵只读（同权限不动文件 ctime）
  _root_rm "$w/.romfs.intern"; chmod u+w "$w/romfs" && mv "$w/romfs" "$w/.romfs.intern" && ln -s "../$_ROOT_STORE/$g/$id" "$w/romfs" && chmod u+w "$w/.romfs.intern" && mv "$w/.romfs.intern" "$st/$id" && chmod -R a-w "$st/$id" || return 1
}
# _root_romfs_seal <W.tmp>：从旧格式现行（romfs 是真目录）克隆来的 romfs 开工时就整棵只读（ROOT_OWN_ROMFS=1 的阶段除外）：
#   阶段照样写不进 RomFS；放在资源键克隆交接之前，交接记下的 ctime 就是只读之后的，提交时收进存储只是 rename、不再改文件，资源键缓存不失效
_root_romfs_seal() {
  [ -z "${ROOT_OWN_ROMFS:-}" ] && [ -d "$1/romfs" ] && [ ! -L "$1/romfs" ] || return 0
  chmod -R a-w "$1/romfs"
}
# root_romfs_sweep [款…]：删存储里没有任何版本（现行、旧版、.tmp、重放中的 .tmp.rb）再指向的身份；不给款 = 存储里全部款。
#   修剪 / gc 删版本之后调。钉住的旧版本没删，它指向的身份照样留着。
#   root_commit 在持提交锁时调（审计 #1508 P1-108：原先放锁后清扫，名单拍下之后并行阶段拿锁收纳的新身份不在名单里、被删，现行 romfs 断链）；
#   删每个身份前再读一次全部版本链接，名单之后新出现的指向照样算
_root_romfs_used() { local d; for d in "$S2A_ROOTS/$1"@*; do [ ! -L "$d/romfs" ] || readlink "$d/romfs"; done; }
#   一律持这一款的提交锁清（root_commit 里调时已持锁就不再拿；单独调的自己拿，最多等 600 秒，拿不到这一款跳过、一个不删）：
#   原先单独调的清扫不拿锁，只靠「删前再读一次链接」防并发的收纳——再读时按 g@* 列版本再逐个读链接，并发的提交恰好把 g@<标签>.tmp 改名成
#   g@<标签>，两个名字都没列到，刚收纳的新身份被当成无主删掉、现行 romfs 读不到（tests/roots_romfs_sweep_race_test.sh 满载下偶发红，
#   测试按事件把这一刻钉住后旧码必红）。与收纳、提交同一把锁串行就没有这个空当
root_romfs_sweep() {
  local base=$S2A_ROOTS/$_ROOT_STORE g d used held
  [ -d "$base" ] || return 0
  [ $# -gt 0 ] || set -- $(ls "$base")
  for g in "$@"; do [ -d "$base/$g" ] || continue
    held=
    if [ -z "$_ROOT_LOCK_PID" ] || [ "${_ROOT_LOCK_GAME:-}" != "$g" ]; then
      type plock_take > /dev/null 2>&1 || source "$(dirname "${BASH_SOURCE[0]}")/../tools/plock.sh" || return 1
      PLOCK_POLL_S=0.2 plock_take "$S2A_ROOTS/.$g.commit.lock" 600 || { echo "【黄】$g RomFS 清扫拿不到提交锁（持有者 pid $(plock_holder "$S2A_ROOTS/.$g.commit.lock")），这次不清" >&2; continue; }
      held=$PLOCK_PID
    fi
    used=$(_root_romfs_used "$g")
    for d in "$base/$g"/*; do [ -d "$d" ] && [ ! -L "$d" ] || continue
      grep -qxF -- "../$_ROOT_STORE/$g/${d##*/}" <<< "$used" && continue
      _root_rm "$d" && echo "删 RomFS 存储 ${d}（没有版本再指向它）" >&2; done
    rmdir "$base/$g" 2> /dev/null || :
    [ -z "$held" ] || plock_release "$held"; done
  rmdir "$base" 2> /dev/null || :; return 0
}
# 取现行时顺手「钉」住它（11-02 体验线 Tools Up! 一键出包：刷新的 aoc / sysdata 阶段连续 commit 两次，root_commit 的「只留现行 + 上一版」把改写阶段刚取到、正在读的
#   播种根 @oc 删了 → 改写读不出输入）：每个取了现行的进程在 $S2A_ROOTS/.pin.<根名>.<pid>（普通文件，不是目录）记「pid 进程起始时间」，修剪（root_commit / gc）见到有活着的钉就不删这个根；
#   钉在产物根目录之外（不进克隆 / 指纹），持有者进程死了钉自己失效、下次修剪顺手清掉。$$ 在 $(…) 里仍是调用脚本本身。
_root_pin() { local b; b=$(basename "$1"); [ -d "$S2A_ROOTS" ] && printf '%s %s\n' "$$" "$(_root_lstart $$)" > "$S2A_ROOTS/.pin.$b.$$" 2> /dev/null; return 0; }
_root_pinned() { local b f p l; b=$(basename "$1"); for f in "$S2A_ROOTS/.pin.$b".*; do [ -f "$f" ] || continue
    read -r p l < "$f"; [ "$p" = "$$" ] && continue   # 自己的钉不算：自己 commit 之后不会再读自己取过的旧根（并行的别的阶段才会）
    [ -n "$p" ] && [ "$(_root_lstart "$p")" = "$l" ] && return 0; rm -f "$f"; done; return 1; }
# 修剪被取代的版本：只留现行 + 上一版，被钉住（有进程正在把它当输入）的不删。$1 = 款，$2 = 现行目录名
_root_prune() { ls -dt "$S2A_ROOTS/$1"@* 2>/dev/null | grep -v -e '@现行$' -e '\.tmp$' | while read -r d; do [ "$(basename "$d")" = "$2" ] || echo "$d"; done | tail -n +2 | while read -r d; do _root_pinned "$d" || _root_trash "$d"; done; }
root_cur() { local r; r=$(_root_cur_raw "$@"); [ -n "$r" ] && [ -z "${S2A_ROOT_CUR:-}" ] && _root_pin "$r"; [ -z "$r" ] || echo "$r"; }
_root_cur_raw() {
  [ -n "${S2A_ROOT_CUR:-}" ] && { echo "$S2A_ROOT_CUR"; return; }   # 只算键（pack/stage_keys.sh）：各阶段对指定产物根算键，不碰现行
  [ -n "${S2A_ROOTS_NO_BOOT:-}" ] && { (cd -P "$S2A_ROOTS/$1@现行" 2>/dev/null && pwd); return; }   # 全新状态（tools/oneshot.sh）：不从共享现行起步
  case "$S2A_ROOTS" in "$S2A_ROOTS_PRIV/"*) ;; *) (cd -P "$S2A_ROOTS/$1@现行" 2>/dev/null && pwd); return;; esac   # 只有约定的私有根自动起步（单测的临时根不克隆真数据）
  if [ ! -e "$S2A_ROOTS/$1@现行" ] && [ -e "$S2A_ROOTS_SHARED/$1@现行" ]; then   # 私有根起步：克隆共享现行
    local s; s=$(cd -P "$S2A_ROOTS_SHARED/$1@现行" && pwd); mkdir -p "$S2A_ROOTS"
    python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/clonedir.py" "$s" "$S2A_ROOTS/$(basename "$s")" > /dev/null 2>&1 \
      && ln -sfn "$(basename "$s")" "$S2A_ROOTS/$1@现行" || echo "【红】私有根从共享现行 $s 克隆失败" >&2
  fi
  (cd -P "$S2A_ROOTS/$1@现行" 2>/dev/null && pwd); }
# 派生摘要可丢，真实克隆不可丢；所有阶段（含replay）在暴露新根前交接同一协议。
_root_resource_clone() {
  local helper="$(dirname "${BASH_SOURCE[0]}")/resource_input_key.py"
  [ -f "$helper" ] || return 0
  "${S2A_PYTHON:-${S2A_PY:-python3}}" "$helper" --root-clone "$1" "$2" > /dev/null 2>&1 || :
}
# 快路径只在提交后核全部输入身份才发布，不能把调用方写入前的KR给下一阶段。
_root_resource_publish() {
  [ -n "${S2A_RESOURCE_KEY_ONCE_FILE:-}" ] || return 0
  local helper="$(dirname "${BASH_SOURCE[0]}")/resource_input_key.py"
  [ -f "$helper" ] || return 0
  "${S2A_PYTHON:-${S2A_PY:-python3}}" "$helper" --root-publish "$1" > /dev/null 2>&1 || :
}
root_begin() {
  local g=${1:?款} c=${2:?提交} w cur rl sk=
  _root_shared && { _root_tree_ok "$g" || return 1; }
  w=$(_root_dir "$g" "$c"); mkdir -p "$S2A_ROOTS"
  [ -e "$w" ] && { echo "【红】产物根 $w 已存在：已提交的版本不许原地重写" >&2; return 1; }
  _root_rm "$w.tmp"; cur=$(root_cur "$g")
  if [ -n "$cur" ]; then
    # RomFS 是存储软链：克隆跳过它、照抄软链（ROOT_OWN_ROMFS=1 的阶段要改 RomFS：clonedir 跟随软链克隆出一份真目录，提交时收成新身份）
    rl=$(_root_romfs_link "$cur") || return 1
    [ -n "$rl" ] && [ -z "${ROOT_OWN_ROMFS:-}" ] && sk=romfs
    python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/clonedir.py" "$cur" "$w.tmp" $sk || { _root_rm "$w.tmp"; echo "【红】从现行 $cur 克隆失败" >&2; return 1; }
    [ -z "$sk" ] || ln -s "$rl" "$w.tmp/romfs" || { _root_rm "$w.tmp"; echo "【红】$w.tmp 建 RomFS 存储软链失败" >&2; return 1; }
    _root_romfs_seal "$w.tmp" || { _root_rm "$w.tmp"; echo "【红】$w.tmp 的 RomFS 设只读失败" >&2; return 1; }
    _root_resource_clone "$cur" "$w.tmp"
    _root_shards "$w.tmp" "克隆现行(root_begin)"
    # 安全阀：除 romfs 那一条合规的存储软链外不许有软链（克隆后写入会顺链改到别处，旧库 10-18 改坏过原版 romfs）
    { find "$w.tmp" -mindepth 1 -type l | grep -vxF -- "$w.tmp/romfs" | grep -q . || ! _root_romfs_link "$w.tmp" > /dev/null; } && { _root_rm "$w.tmp"; echo "【红】现行版本里有软链（romfs 指向本款 RomFS 存储的那一条除外）：克隆后写入会顺链改到别处（旧库 10-18 改坏过原版 romfs），先清掉软链" >&2; return 1; }
  else mkdir -p "$w.tmp"; fi
  ( cd "$w.tmp" && for f in .stage_*; do [ -f "$f" ] && printf '%s\t%s\n' "$f" "$(cat "$f")"; done ) > "$w.tmp/.root_base_stages" 2> /dev/null   # 开工时各阶段键（root_commit 判别的树动没动过同一阶段）
  _root_owner > "$w.tmp/.root_pid"   # 持有者（tools/gc.sh 只删持有者已死的 .tmp；10-23 排烘焙道 2 小时的 .tmp 被按时间删掉、出包中途失败）
  basename "${cur:-无}" > "$w.tmp/.root_base"; git -C "$(dirname "${BASH_SOURCE[0]}")/.." rev-parse HEAD > "$w.tmp/.root_src" 2> /dev/null   # 来源提交：护栏判「只许往前」
  echo "$w.tmp"
}
_root_shards() {   # _root_shards <目录> <标签>：记一行直连库分片清点（11-01 究极4 分片丢在出包之后某步，要能定位）；记不成不影响提交。日志 ${S2A_SHARD_LOG}（缺省 ${S2A_ROOTS}/.shard_inventory.log）
  python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/direct_shards.py" inventory "$1" --label "$2" --log "${S2A_SHARD_LOG:-${S2A_ROOTS:-/dev/null/x}/.shard_inventory.log}" > /dev/null 2>&1 || true; }
_ROOT_LOCK_PID=; _ROOT_LOCK_GAME=
_root_lock() {   # 同一款的提交串行：内核文件锁（tools/plock.sh，审计 P1-27：原先 mkdir + 判 pid 死就删，读 → 判 → 删之间别人可能已接手）；持锁者 = 本（子）shell，退出即放
  # 用到才 source（同本文件调 ../tools/clonedir.py）：调度器冻结的 sched/auto/tools_<提交>/ 里 roots.sh 与 tools 平铺，只用不提交产物根的函数
  type plock_take > /dev/null 2>&1 || source "$(dirname "${BASH_SOURCE[0]}")/../tools/plock.sh" || return 1
  PLOCK_POLL_S=0.2 plock_take "$S2A_ROOTS/.$1.commit.lock" 86400 || { echo "【红】$1 拿不到产物根提交锁（$S2A_ROOTS/.$1.commit.lock，持有者 pid $(plock_holder "$S2A_ROOTS/.$1.commit.lock")）" >&2; return 1; }
  _ROOT_LOCK_PID=$PLOCK_PID; _ROOT_LOCK_GAME=$1
}
_root_unlock() { plock_release "$_ROOT_LOCK_PID"; _ROOT_LOCK_PID=; _ROOT_LOCK_GAME=; }
_root_replay() {   # _root_replay <W.tmp> <新现行> <路径…>：新现行克隆一份，按本阶段声明的路径换成 W 里的，结果放回 W.tmp
  #   !<路径>：声明路径里套着的、别的阶段的产物（着色器阶段的 shaders/ 里有直连阶段的 shaders/direct）——换完之后这一块恢复成新现行的。
  #   10-25 测试设备1 0100152000022000：着色器阶段提交时把整个 shaders/ 换成开工时克隆的那份，烘的几分钟里直连阶段刚提交的新库被退回旧版
  #   （旧版顶层表少、b-<链>/ 同名还在，.stage_direct 戳却是新的、没人再出库），推包时机上 9 个顶层表被当旧文件清掉，直连起手即 abort
  local w=$1 now=$2 r=$1.rb p m rl sk= own=; shift 2; _root_rm "$r"
  # 本阶段声明的产物在 RomFS 里（romfs 或 romfs/<路径>）：新现行的 RomFS 克隆成自己的真目录再换（同 ROOT_OWN_ROMFS=1），提交时收成新身份；否则照抄存储软链
  for p in "$@"; do case ${p#!} in romfs|romfs/*) own=1;; esac; done
  rl=$(_root_romfs_link "$now") || return 1; [ -z "$rl" ] || [ -n "$own" ] || sk=romfs
  # 顶层的字面声明路径（不带 / 与通配，如 pkg）：克隆新现行时直接跳过，后面从 W 整个挪过去（W 随后就删）——原先先克隆新现行的那份、
  #   再逐文件删掉、再从 W 逐文件拷一份（0400486024DEC000 pkg/pack 18.7 万文件，各走一遍）
  local mvp=; for p in "$@"; do case $p in '!'*|*/*|*'*'*|*'?'*|*'['*|romfs) ;; *) mvp="$mvp $p";; esac; done
  python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/clonedir.py" "$now" "$r" $sk $mvp > /dev/null || { _root_rm "$r"; return 1; }
  [ -z "$sk" ] || ln -s "$rl" "$r/romfs" || { _root_rm "$r"; return 1; }
  [ -n "$own" ] || _root_romfs_seal "$r" || { _root_rm "$r"; return 1; }
  _root_resource_clone "$now" "$r"
  _root_shards "$r" "replay·克隆新现行"
  for p in "$@"; do case $p in '!'*) continue;; esac
    case " $mvp " in *" $p "*) [ -e "$w/$p" ] || [ -L "$w/$p" ] || continue; mv "$w/$p" "$r/$p" || { _root_rm "$r"; return 1; }; continue;; esac   # 上面克隆时跳过的：整个挪过来
    ( cd "$r" && shopt -s nullglob dotglob && for m in $p; do _root_rm "$m"; done ) || return 1   # 走 _root_rm：删的同时被写进系统文件也删得掉（#369）
    # 不带通配的字面路径 W 里没有 = 本阶段删了它；复制失败必须中止，不能提交缺失的产物。
    ( cd "$w" && shopt -s nullglob dotglob && for m in $p; do [ -e "$m" ] || continue; mkdir -p "$r/$(dirname "$m")" && _root_cp -R "$m" "$r/$m" || exit 1; done ) || { _root_rm "$r"; return 1; }
  done
  for p in "$@"; do case $p in '!'*) p=${p#!};; *) continue;; esac
    ( cd "$r" && _root_rm "$p"; [ -e "$now/$p" ] || exit 0; mkdir -p "$(dirname "$p")" && _root_cp -R "$now/$p" "$p" ) || { _root_rm "$r"; return 1; }
  done
  # 来源提交取两边较新的：新现行来自更新的 main 时留它的（旧树重放一个没被动过的阶段，不把共享现行的来源退回旧提交）
  local ns ms; ns=$(cat "$r/.root_src" 2> /dev/null); ms=$(cat "$w/.root_src" 2> /dev/null)
  { [ -z "$ns" ] || git -C "$(dirname "${BASH_SOURCE[0]}")/.." merge-base --is-ancestor "$ns" "$ms" 2> /dev/null; } && cp "$w/.root_src" "$r/.root_src" 2> /dev/null
  basename "$now" > "$r/.root_base"
  _root_shards "$r" "replay·换完路径后"
  _root_trash "$w" && mv "$r" "$w"
}
root_commit() {
  local g=${1:?款} c=${2:?提交} w; w=$(_root_dir "$g" "$c")
  [ -d "$w.tmp" ] || { echo "【红】没有 $w.tmp（先 root_begin）" >&2; return 1; }
  _root_lock "$g" || return 1
  local base now; base=$(cat "$w.tmp/.root_base" 2>/dev/null); now=$(basename "$(root_cur "$g")" 2>/dev/null); [ -n "$now" ] || now=无
  if [ "$base" != "$now" ]; then
    # 共享根只许往前（10-24 宽屏线报：各线工作树停在不同 main，轮流刷 010074F013262000 等共享现行，改写 / 着色器键来回变、反复重烘、烘焙槽长期满）：
    #   开工后现行已被别的树按更新的 main 刷过（新现行的来源提交不是本树来源的祖先）→ 本阶段作废、不重放——原先照样把旧树产物连同旧 .root_src
    #   重放到新现行上，等于把共享现行退回旧版本，下一个新树按键不符又重烘。旧树要用自己的产物走私有根（S2A_ROOTS=…/roots_priv/<名>）
    #   只在新树动过「本阶段」时作废（本阶段声明的 .stage_* 键与开工时不同）：新树只刷了别的阶段（如改写）就照常重放本阶段、来源提交留新的——
    #   10-24 首版不分阶段一律作废，合入几分钟一笔、烘焙 20 分钟，持槽烘完的着色器每次都被新树的改写提交顶掉，一款都烘不上
    if _root_shared && [ "$now" != 无 ]; then local ns ms st touched=; ns=$(cat "$(root_cur "$g")/.root_src" 2> /dev/null); ms=$(cat "$w.tmp/.root_src" 2> /dev/null)
      for st in ${ROOT_REPLAY:-}; do case $st in .stage_*) [ "$(cat "$(root_cur "$g")/$st" 2> /dev/null)" = "$(awk -F'\t' -v k="$st" '$1 == k { print $2 }' "$w.tmp/.root_base_stages" 2> /dev/null)" ] || touched=$st;; esac; done
      [ -z "${ROOT_REPLAY:-}" ] && touched=整份   # 不声明产物路径的阶段本来就不重放
      if [ -n "$touched" ] && [ -n "$ns" ] && [ -n "$ms" ] && ! git -C "$(dirname "${BASH_SOURCE[0]}")/.." merge-base --is-ancestor "$ns" "$ms" 2> /dev/null; then
        _root_unlock "$g"; _root_rm "$w.tmp"
        echo "【红】$g 现行在本阶段期间已由更新的 main ${ns:0:9} 刷过（本树 ${ms:0:9}）：共享现行只许往前，丢弃 $(basename "$w")、不重放；要用本树产物走私有根 S2A_ROOTS=${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/roots_priv/<名>" >&2; return 1; fi
    fi
    if [ -n "${ROOT_REPLAY:-}" ] && [ "$now" != 无 ]; then   # 产物互不重叠的并行阶段：重放到新现行（$ROOT_REPLAY 按空格分开，故意不加引号）
      _root_replay "$w.tmp" "$(root_cur "$g")" $ROOT_REPLAY || { _root_unlock "$g"; echo "【红】$g 重放 $(basename "$w") 到新现行 $now 失败" >&2; _root_rm "$w.tmp"; return 1; }
      echo "${g}：现行在本阶段期间变了（开工时 ${base}，现在 ${now}），已把本阶段产物（${ROOT_REPLAY}）重放到新现行" >&2
    else _root_unlock "$g"; echo "【红】$g 现行在出包期间变了（开工时 ${base}，现在 ${now}）：不切现行、丢弃 $(basename "$w")，基于新现行重做这一步" >&2; _root_rm "$w.tmp"; return 1; fi
  fi
  _root_romfs_intern "$w.tmp" "$g" || { _root_unlock "$g"; echo "【红】$g 把 $(basename "$w") 的 RomFS 收进共享存储失败" >&2; _root_rm "$w.tmp"; return 1; }
  _root_shards "$w.tmp" "提交前(root_commit)"
  rm -f "$w.tmp/.root_pid"; mv "$w.tmp" "$w" || { _root_unlock "$g"; return 1; }
  _root_resource_publish "$w"
  _root_shards "$w" "提交后(root_commit)"
  ln -sfn "$(basename "$w")" "$S2A_ROOTS/.$g@现行.new" && _root_mv_link "$S2A_ROOTS/.$g@现行.new" "$S2A_ROOTS/$g@现行" || { _root_unlock "$g"; echo "【红】切现行失败" >&2; return 1; }
  local cur; cur=$(basename "$w")
  # 只留现行 + 上一版（被进程钉住的不删），再清没有版本指向的 RomFS 身份；都在放锁之前（P1-108：清扫与下一笔收纳同一把提交锁串行）
  _root_prune "$g" "$cur"
  root_romfs_sweep "$g"
  _root_unlock "$g"
}
# .tmp 持有者：「pid 进程起始时间」。root_begin 在 $(…) 里调，$$ 仍是调用脚本本身（排道 + 烘焙全程活着）；带起始时间防 pid 复用把死掉的 .tmp 认成在用
_root_lstart() { ps -o lstart= -p "$1" 2> /dev/null | awk '{$1 = $1} 1'; }   # 进程起始时间，空白规整（ps 行尾带空格，read 读回会去掉）
_root_owner() { echo "$$ $(_root_lstart $$)"; }
# root_tmp_live <.tmp 目录>：0 = 有持有者记录且那个进程还活着（同一 pid、同一起始时间）；没有记录（旧格式）/ 持有者已死 = 1
root_tmp_live() {
  local p t; read -r p t < "$1/.root_pid" 2> /dev/null || return 1
  [ -n "$p" ] && kill -0 "$p" 2> /dev/null && [ "$(_root_lstart "$p")" = "$(awk '{$1 = $1} 1' <<< "$t")" ]
}
root_abort() { local w; w=$(_root_dir "${1:?款}" "${2:?提交}"); _root_rm "$w.tmp"; }
# 追加内容阶段记下的没解出的原始包（pack/stages/aoc.sh 写进 aoc.log 的「追加内容缺<TAB>文件名<TAB>原因」行）：
#   root_aoc_missing <产物根> → 文件名一行一个；root_aoc_note <产物根> → 出包完成行 / 登记说明后缀「；🟡 追加内容 <文件名> 未能解出，包内不含」（有几份写几份，没有则空）
root_aoc_missing() { LC_ALL=C awk -F'\t' '$1 == "追加内容缺" && $2 != "" {print $2}' "${1:?产物根}/aoc.log" 2> /dev/null; }
root_aoc_note() { root_aoc_missing "$1" | while IFS= read -r f; do printf '；🟡 追加内容 %s 未能解出，包内不含' "$f"; done; }
root_tag() { local t=${2:?标签} n=1 c; c=$t; while [ -e "$(_root_dir "${1:?款}" "$c")" ] || [ -e "$(_root_dir "$1" "$c").tmp" ]; do n=$((n + 1)); c=$t-$n; done; echo "$c"; }
root_stage_done() {   # S2A_STAGE_KEYS=<文件>：只算键模式——记「阶段<TAB>键」后当作已是该键（阶段脚本随即退出，不出任何产物）
  if [ -n "${S2A_STAGE_KEYS:-}" ]; then printf '%s\t%s\n' "${2:?阶段}" "${3:?键}" >> "$S2A_STAGE_KEYS"; return 0; fi
  [ -n "${1:-}" ] && [ "$(cat "$1/.stage_${2:?阶段}" 2>/dev/null)" = "${3:?键}" ]; }
# 管线归档 0 KB 判据（10-21 维护者：d2r / d3 / mhrise 管线归档全片失败、0 KB 照样当部分包成了现行并登记，玩家首遇的管线全部现编）：
#   root_pso0 <产物根或其 shaders 的上一级>：有管线分片（shader_pack.json 的 pso.chunks > 0）而归档 0 KB → 打印分片数、返回 0（= 是坏的）；
#   没有分片（程序 0）、没有 shader_pack.json、旧版没有这两个字段 → 返回 1（不算 0 KB）
#   直连款（包里有直连库，root_has_direct）→ 返回 1：双轨口径下通用段本来就不出管线归档（pack/refresh.sh S2A_SHADER_NO_PSO=1），
#   shader_pack.json 照记计划分片数、归档 0 KB 是有意的（10-25 01006F8002326000 3.0.3 正式包「4000 片全部失败」误红、没登记）；直连库没出成时通用走全量、照判
# root_has_direct <产物根>：现行有直连库——keys.tsv 非空，或离线 0 支的三种形态：OpenGL 只有 gl/gl_shaders.tsv 表头、Vulkan 只有 vk/vk_shaders.tsv 表头
#   （运行时生成 SPIR-V、出包不起局，11-05）、抽取缺口的运行时现编空库 runtime_only.txt。与 pack/stage_direct.sh「算出了库」同一口径
#   （11-05：原先只认 keys.tsv 非空，这三种都会被当成通用款去查管线归档 0 KB、登记闸门误红）
root_has_direct() { local d=${1:?根}/shaders/direct; [ -s "$d/keys.tsv" ] || [ -s "$d/gl/gl_shaders.tsv" ] || [ -s "$d/vk/vk_shaders.tsv" ] || [ -f "$d/runtime_only.txt" ]; }
root_pso0() { root_has_direct "${1:?根}" && return 1; local n; n=$(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));n=(d.get('pso') or {}).get('chunks') or 0;print(n if n > 0 and d.get('pso_archive_kb') == 0 else '')" "${1:?根}/shaders/shader_pack.json" 2> /dev/null); [ -n "$n" ] && echo "$n"; }
# 登记前闸门：root_pso0_gate <产物根> <款>：不是 0 KB → 返回 0（放行，不打印）；0 KB → 打【红】返回 1；
#   例外只能显式开：S2A_ALLOW_PSO0=<原因>（10-21 维护者：暗黑3 录按键包只录制、不评流畅度，例外登记一次）→ 打 🟡 一行、返回 0，调用方把原因写进登记说明
root_pso0_gate() { local n; n=$(root_pso0 "${1:?根}") || return 0
  if [ -n "${S2A_ALLOW_PSO0:-}" ]; then echo "🟡 ${2:-?} 管线归档 0 KB（${n} 片全部失败）：按 S2A_ALLOW_PSO0 例外登记——${S2A_ALLOW_PSO0}"; return 0; fi
  echo "【红】${2:-?} 管线归档 0 KB（${n} 片全部失败）：包已出、不登记（只录制等确需例外时显式设 S2A_ALLOW_PSO0=<原因>）"; return 1; }
