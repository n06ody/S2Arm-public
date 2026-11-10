#!/bin/bash
# main 已知红失败项解析单测（tools/known_red.py；合成门禁日志，约 1 秒）：bash tests/known_red_test.sh
# 期望（按规则手写，10-30 今天实况的几种写法）：
#   ① 行尾的 ✗（整步红标记）不算最后那一项失败；②「｜」不切开：子项明细（①191/300 …｜有失败）整段是一项；
#   ③ 计数超基线「227（基线 ≤ 226）」算失败、「33 行（基线 ≤ 35）」不算；④ 临时目录只换随机那一级、后面路径（链号目录）保留；
#   ⑤ 父步绿、「↳ 本机另跑」子项红 → 失败项归父步；⑥ uncovered：表里同步同特征全等才算已知，错误原文差一个字就不算；取不出明细的红步算未覆盖
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
K="python3 $H/tools/known_red.py"
cat > "$T/a.log" <<'L'
🔴 钩子 h/gate.sh［gate-host］ 156 秒（rc=1）：期望 全过
    | 期望 全过 → 真结果 甲项：KeyError: ('/var/folders/02/abc123/T/t_x_9q8w/a/mac/b-9347db8fda99', 'k.metallib')（退出 1） / 乙项：t_y：10 项，失败 0（退出 0），用时 570 秒 ✗
🟢 钩子 g/gate.sh［gate-host］ 30 秒：好
    ↳ 🔴 本机另跑 dsh 106 秒（rc=1）：期望 本机子项全过
    | 期望 本机子项全过 → 真结果 对拍：①命令行191/300 ②导出入口28/40｜有失败（退出 1） / 直连 MTL4 出现处 227（基线 ≤ 226） / 直调 33 行（基线 ≤ 35）
🔴 单测 z_test 600 秒（rc=124，超 600 秒）：
L
s=$($K sig "$T/a.log")
ck "① 行尾 ✗ 不算乙项失败" '! grep -q "乙项" <<< "$s"'
ck "② 子项明细整段一项（含 191/300 与 有失败）" 'grep -q "钩子 g/gate.sh	对拍：①命令行191/300 ②导出入口28/40｜有失败（退出 1）" <<< "$s"'
ck "③ 超基线算失败、没超不算" 'grep -q "直连 MTL4 出现处 227（基线 ≤ 226）" <<< "$s" && ! grep -q "直调 33 行" <<< "$s"'
ck "④ 临时目录只换随机那一级，链号目录保留" 'grep -qF "甲项：KeyError: ('"'"'<tmp>/a/mac/b-9347db8fda99'"'"', '"'"'k.metallib'"'"')（退出 1）" <<< "$s"'
ck "⑤ 父步绿、子项红 → 归父步 g/gate.sh" 'grep -q "^钩子 g/gate.sh	" <<< "$s"'
grep "^钩子 h/gate.sh" <<< "$s" | awk -F'\t' '{ print $1 "\t" $2 "\tc\t0\to\t\t0\t0" }' > "$T/kr.tsv"
u=$($K uncovered "$T/kr.tsv" "$T/a.log" | LC_ALL=C sort | tr '\n' ' ')
ck "⑥ h 步全是已知红 → 不在未覆盖里；g 步有未知项、z_test 取不出明细 → 都在（${u}）" '[ "$u" = "单测 z_test 钩子 g/gate.sh " ]'
sed 's/KeyError: (/KeyError: [/' "$T/a.log" > "$T/b.log"
ck "⑥ 同一项错误原文差一个字 → 不算已知红" '$K uncovered "$T/kr.tsv" "$T/b.log" | grep -qx "钩子 h/gate.sh"'
ck "generic：取不出明细的红步只有 z_test" '[ "$($K generic "$T/a.log")" = "单测 z_test" ]'
# ⑦ 汇总行（去掉失败字样不足 10 字，如「排查手册自检有不通」）不算失败项：登记过的真失败项对上 → 已覆盖（10-30 #902 预检被汇总行判成本支红）
printf '🔴 单测 d_test 1 秒（rc=1）：手册自检有不通\n    |   ✗ docs/x.md：路径 tests/gone.tsv 不存在\n    | 手册自检有不通\n红步\t单测 d_test\n' > "$T/d.log"
printf '单测 d_test\tdocs/x.md：路径 tests/gone.tsv 不存在\tabc\t1\t甲\t\t1\t0\t已知红\n' > "$T/kd.tsv"
ck "⑦ 汇总行不算失败项：d_test 只有一项特征" '[ "$($K sig "$T/d.log" | grep -c .)" = 1 ]'
ck "⑦ 登记过的真失败项对上 → uncovered 为空" '[ -z "$($K uncovered "$T/kd.tsv" "$T/d.log")" ]'
# ⑨ 登记表写表头规矩（10-30 维护者）：保存后首行是 # 表头、写明不稳定 24 小时内要有根修分支；表头不影响读登记
cp "$T/kr.tsv" "$T/kr9.tsv"; n9=$(grep -c . "$T/kr9.tsv"); : > "$T/h9.tsv"; $K expire "$T/kr9.tsv" "$T/h9.tsv" 24 > /dev/null   # expire 读表再保存（已知红行不撤）
ck "⑨ 表头：保存后首行 #、含「24 小时内必须有根修分支在队」；登记 ${n9} 行照样读得出" 'head -1 "$T/kr9.tsv" | grep -q "^#" && grep -q "24 小时内必须有根修分支在队" "$T/kr9.tsv" && [ "$(grep -vc "^#" "$T/kr9.tsv")" = "$n9" ] && [ -z "$($K uncovered "$T/kr9.tsv" "$T/a.log" | grep -x "钩子 h/gate.sh")" ]'
# ⑧ 通过的用例行字面带「失败」「【红】」也不算失败项（10-30 维护方工具：「通 显卡门禁步骤试占 CPU 账失败：照跑…没有【红】」）
printf '🔴 单测 m_test 1 秒（rc=1）：m 单测有不通\n    | 不通 槽满时排队让满 4 秒后拿到槽（用时 秒）\n    | 通 显卡门禁步骤试占 CPU 账失败：照跑、没有【红】\n红步\t单测 m_test\n' > "$T/m.log"
ck "⑧ 通过行不算失败项：m_test 只有「不通」那一项" '[ "$($K sig "$T/m.log" | grep -c .)" = 1 ] && $K sig "$T/m.log" | grep -q "槽满时排队"'
# ⑭ 运行日志旁白不算失败项（11-05 能耗线：「[s2a] 【红】地址窗口：读不到本款 main.npdm」每个不占窗口的单测进程都打、单测照样过，
#   跟着 nvn_rec_cover 组红的日志尾巴带出来，被登记成「不稳定」；日志形状取自 11-05 16:09 那份红日志）：特征只有「组 nvn_rec_cover」与 make 两项，没有 [s2a] / [nvn] 行
printf '🔴 钩子 runtime/gate.sh［gate-host］ 326 秒（rc=1）：原生层单测 期望 204 组全绿 → 绿 203 组、红 1 组\n    | ✗ 组 nvn_rec_cover：异常退出（rc=2）\n    | [s2a] 【红】地址窗口：读不到本款 main.npdm（可执行文件同目录 / 模块目录），不知道要多大，不占窗口\n    | [nvn-mtl] 【红】某条运行诊断 Error 也不算\n    | make: *** [native-test-nvn_rec_cover] Error 1\n红步\t钩子 runtime/gate.sh\n' > "$T/r11.log"
s11=$($K sig "$T/r11.log")
ck "⑭ 运行日志旁白（[s2a] / [nvn-mtl] 开头）不进特征，判定行与 make 行照取（$(grep -c . <<< "$s11") 项）" '! grep -q "main.npdm\|某条运行诊断" <<< "$s11" && grep -q "组 nvn_rec_cover" <<< "$s11" && grep -q "make: \*\*\* \[native-test-nvn_rec_cover\] Error 1" <<< "$s11"'
# ⑩ 易变数字（进程号 / 地址 / 时刻）不进特征：同一项不同 pid 的两次红对得上同一条登记；判据里的计数不动（10-30 维护者）
printf '🔴 钩子 v/gate.sh 1 秒（rc=1）：x\n    | 判红：诊断：build_run.sh: line 26: 49158 Abort trap: 6  dotnet Runtime.Tests.dll（退出 134）at 0x7ffee1234567 19:12:03\n红步\t钩子 v/gate.sh\n' > "$T/v1.log"
sed -e 's/49158/51207/' -e 's/0x7ffee1234567/0x7ffee9abcdef/' -e 's/19:12:03/20:01:44/' "$T/v1.log" > "$T/v2.log"
$K sig "$T/v1.log" | awk -F'\t' '{ print $1 "\t" $2 "\tc\t0\to\t\t0\t0\t不稳定" }' > "$T/kv.tsv"
ck "⑩ pid / 地址 / 时刻不同的同一项 → 对得上登记（uncovered 空）" '[ -z "$($K uncovered "$T/kv.tsv" "$T/v2.log")" ]'
ck "⑩ 判据数字不动：「退出 134」「line 26」保留、进程号换成 <pid>" '$K sig "$T/v1.log" | grep -q "line 26: <pid> Abort trap" && $K sig "$T/v1.log" | grep -q "退出 134"'
printf '🔴 单测 n_test 1 秒（rc=1）：x\n    | 不通 计数 期望 0 → 真结果 1 ✗\n红步\t单测 n_test\n' > "$T/n1.log"; sed 's/真结果 1/真结果 2/' "$T/n1.log" > "$T/n2.log"
$K sig "$T/n1.log" | awk -F'\t' '{ print $1 "\t" $2 "\tc\t0\to\t\t0\t0" }' > "$T/kn.tsv"
ck "⑩ 判据值不同（真结果 1 → 2）仍算不同项，不被归一化吞掉" '$K uncovered "$T/kn.tsv" "$T/n2.log" | grep -qx "单测 n_test"'
# ⑪ 短而真的失败项（「B 项：坏了（退出 1）」）照算，汇总行（「x 单测有不通」）不算（10-30：按长度滤把短真项也滤了，真红按已知红放行）
printf '🔴 钩子 s/gate.sh 1 秒（rc=1）：x\n    | A 项：KeyError: boom（退出 1） / B 项：坏了（退出 1）\n    | s 单测有不通\n红步\t钩子 s/gate.sh\n' > "$T/s.log"
ck "⑪ 两项都算、汇总行不算：特征 2 条、含 B 项" '[ "$($K sig "$T/s.log" | grep -c .)" = 2 ] && $K sig "$T/s.log" | grep -q "B 项：坏了"'
# ⑫ 通过的结果段（带 ✓ 不带 ✗，如「过 47、不通 0 ✓」）不是失败项；「没跑完」的红步（红行不带秒数）也算未覆盖（10-30 回放真实红日志）
printf '🔴 钩子 g/gate.sh 1 秒（rc=1）：x\n    | 期望 … → 真结果 过 47、不通 0 ✓ / Q 项：坏了（退出 1）\n🔴 单测 u_test［本机］：没跑完（无退出码）\n红步\t钩子 g/gate.sh\t单测 u_test\n' > "$T/p.log"
ck "⑫ 「过 47、不通 0 ✓」不算失败项：g 只有 Q 项" '[ "$($K sig "$T/p.log" | grep -c "钩子 g/gate.sh")" = 1 ] && ! $K sig "$T/p.log" | grep -q "不通 0"'
$K sig "$T/p.log" | awk -F'\t' '{ print $1 "\t" $2 "\tc\t0\to\t\t0\t0" }' > "$T/kp.tsv"
ck "⑫ Q 项已登记、u_test 没跑完 → uncovered 仍列出 u_test" '$K uncovered "$T/kp.tsv" "$T/p.log" | grep -qx "单测 u_test"'
# ⑬ remind 只催已知红：挂满 12 小时的「已知红」项要催；同样挂满的「不稳定」项不催（由 expire 按 24 小时没再出现撤；11-02 pack_jobs_test 误催）
old=$(( $(date +%s) - 13 * 3600 ))
printf '单测 r_test\t甲：坏了（退出 1）\tc\t%s\t线甲\t\t0\t0\t已知红\n单测 s_test\t乙：偶发（退出 1）\tc\t%s\t线乙\t\t0\t0\t不稳定\n' $old $old > "$T/kr13.tsv"
r13=$($K remind "$T/kr13.tsv" 12 2)
ck "⑬ remind：已知红 r_test 催、不稳定 s_test 不催（${r13//$'\n'/ ／ }）" 'grep -q "^单测 r_test	" <<< "$r13" && ! grep -q "s_test" <<< "$r13"'
[ $bad = 0 ] && echo "known_red 单测全过"; exit $bad
