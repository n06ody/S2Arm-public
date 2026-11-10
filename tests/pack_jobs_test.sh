#!/bin/bash
# 门禁覆盖：pack/jobs/*.py pack/jobs/tests/*.py
# 启动器任务层单测进门禁（10-27 维护者：pack/jobs/tests 原先只挂在 launchers/macos/gate.sh 钩子下，tool 档不跑钩子，
#   名单线 lists/save-zero 改 pack/jobs/saves.py 判 tool 档、test_saves.py 没进门禁）：bash tests/pack_jobs_test.sh
# 选测：GATE_CHANGED（门禁给的改动清单）里只有 pack/jobs/<模块>.py → 只跑 import / 点名了这些模块的 pack/jobs/tests/test_*.py；
#   改到 pack/jobs/tests/ 下的文件（含 jobcase.py）、或没给 GATE_CHANGED → 全跑（235 个约 55 秒）
set -u; cd "$(dirname "$0")/.." || exit 1
mods=(); all=; [ -n "${GATE_CHANGED:-}" ] || all=1
for f in ${GATE_CHANGED:-}; do case $f in
  pack/jobs/tests/*) all=1;;
  pack/jobs/*.py) m=${f#pack/jobs/}; mods+=("${m%.py}");; esac; done
if [ -n "$all" ] || [ ${#mods[@]} = 0 ]; then sel=(pack/jobs/tests/test_*.py)
else sel=($(for t in pack/jobs/tests/test_*.py; do for m in "${mods[@]}"; do grep -qE "(^|[^A-Za-z0-9_])${m}([^A-Za-z0-9_]|$)" "$t" && { echo "$t"; break; }; done; done))
  [ ${#sel[@]} -gt 0 ] || sel=(pack/jobs/tests/test_*.py); fi   # 改的模块没有单测点名它：全跑（宁多勿漏）
echo "pack/jobs 单测：跑 ${#sel[@]} 份（$(printf '%s ' "${sel[@]##*/}")）"
# 单测不许往工作树里写东西（11-09：mock 改全进程 os.name = "nt" 时新建的 Path 变成 WindowsPath，「/var/folders/…/job.json」写成工作树根的
#   「\var\folders…」杂文件，两次被 git add -A 带进提交）：每份跑完比一次工作树里未跟踪的文件，多出来的判这份不通、点名
untracked() { git ls-files --others --exclude-standard 2>/dev/null | LC_ALL=C sort; }
bad=0; for t in "${sel[@]}"; do
  u0=$(untracked)
  o=$(python3 -B -m unittest discover -s pack/jobs/tests -p "${t##*/}" 2>&1); rc=$?
  new=$(LC_ALL=C comm -13 <(echo "$u0") <(untracked))
  if [ $rc = 0 ] && [ -z "$new" ]; then echo "通 ${t##*/}：$(grep -E '^Ran ' <<< "$o")"
  elif [ $rc = 0 ]; then echo "不通 ${t##*/}：跑完工作树里多了文件（单测要写临时目录）"; sed 's/^/    | /' <<< "$new" | head -5; bad=1
  else echo "不通 ${t##*/}（rc=${rc}）"; tail -15 <<< "$o" | sed 's/^/    | /'; bad=1; fi
done
[ $bad = 0 ] && echo "pack/jobs 单测全过" || { echo "pack/jobs 单测有不通"; exit 1; }
