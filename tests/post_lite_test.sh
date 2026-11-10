#!/bin/bash
# 合入后刷新减负单测（10-25 维护者；约 20 秒）：bash tests/post_lite_test.sh
# 期望（按需求手写，不跑被测实现推）：
#   ① 同一款两份 refresh 同时起：后到的那份打「已有一份刷新在跑」，它的第一段在先到那份最后一段结束之后才开始（不重叠）
#   ② refresh 锁的持有者被 kill -9 → 接管、不等（内核文件锁随持锁者退出放，tools/plock.sh）
#   ②b 锁按「根目录 + 款」（10-27 维护者）：同一款、两个不同私有根同时起 → 都不打「已有一份刷新在跑」、两份的刷新区间相交（并行；D 停住等 E 先起，按事件证）
#   ②c 同一个私有根起两份 → 照样排队：后到的打「已有一份刷新在跑」、第一段在先到那份最后一段之后
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0; trap 'rm -rf "$T"' EXIT
# 临时仓库 / 临时提交的固定身份（#948）：全新环境（全新家目录、没设全局 git 身份、GIT_CONFIG_GLOBAL=/dev/null、useConfigOnly）下
#   不许依赖调用者的 git 配置自动推断；只作用于本单测进程与它拉起的子进程，绝不写调用者的全局 / 仓库配置。
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid

ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
now() { python3 -c 'import time;print(time.time())'; }
# —— ①② refresh 锁：各阶段换成桩（记起止时刻）；给了 S2A_STUB_HOLD 的那份停在直连库段、等放行文件出现（上限 60 秒只防挂）
#   排队按事件证（11-05 合入运维线根修 ②c，同法改 ①）：原先着色器段睡 2 秒、先到那份一落日志就起后到那份，后到那份拿锁前的前置
#   （xcrun ×2 等）在负载下跑得比先到那份剩下的 2 秒长 → 拿到空锁、不打「已有一份刷新在跑」（11-02 / 11-04 两次红）。
#   改成：先到那份停在直连库段（11-07 前是通用着色器段），等后到那份打出「已有一份刷新在跑」再放行；60 秒等不到（锁失效）或后到那份已跑完也放行，由判据报红
mkdir -p $T/repo/pack $T/roots/acnh@v1/ctl; cp "$H/pack/refresh.sh" "$H/pack/roots.sh" $T/repo/pack/; mkdir -p $T/repo/tools; cp "$H/tools/toolchain.sh" "$H/tools/toolchain.env" "$H/tools/cores.sh" "$H/tools/progress.sh" "$H/tools/plock.sh" "$H/tools/flock_hold.py" $T/repo/tools/; export S2A_DISK_FREE_G=999; touch $T/roots/acnh@v1/ctl/control.nacp; ln -s acnh@v1 $T/roots/acnh@现行
printf '#!/bin/bash\necho "原件核对桩 rc=${FAKE_SRCVER_RC:-0}"; exit ${FAKE_SRCVER_RC:-0}\n' > $T/repo/pack/srcver.sh   # 原件版本核对桩（pack/srcver.sh 自己的单测见 tests/srcver_test.sh）
cp "$H/pack/decoder_need.sh" $T/repo/pack/; mkdir -p $T/repo/translator $T/repo/graphics/nvn/tools; printf 'print("no")\n' > $T/repo/translator/glslc_offline.py; cp "$H/graphics/nvn/tools/romfs_list.py" $T/repo/graphics/nvn/tools/   # 直连链要的判据（同 tests/refresh_test.sh）
# 直连库桩出 shaders/direct/keys.tsv（refresh.sh 核「直连库阶段退 0 但产物根里没有 keys.tsv」）
for s in rewrite sysdata uilayout aoc caps romfsidx direct_profile extract direct direct_seed; do
  printf '#!/bin/bash\necho "$S2A_TAG %s 起 $(python3 -c "import time;print(time.time())")" >> %s/log\n[ -n "${S2A_STUB_HOLD:-}" ] && [ %s = direct ] && { n=0; while [ ! -e "$S2A_STUB_HOLD" ] && [ $n -lt 600 ]; do sleep 0.1; n=$((n + 1)); done; }\n[ %s = direct ] && { d=$(cd -P "$S2A_ROOTS/acnh@现行" && pwd)/shaders/direct; mkdir -p "$d" && echo k > "$d/keys.tsv"; }\necho "$S2A_TAG %s 止 $(python3 -c "import time;print(time.time())")" >> %s/log\necho "🟢 %s 桩"\n' $s $T $s $s $s $T $s > $T/repo/pack/stage_$s.sh
done
# 本单测测的是刷新锁的先后，不测并行度：跳过 tools/cores.sh 入口的忙核采样（11-04 起采 3 秒平均，B 在采样里等 3 秒时 A 的 2 秒桩已跑完，B 碰不到在跑的 A）
RF() { S2A_TAG=$1 S2A_ROOTS=$T/roots S2A_ROOTS_SHARED=$T/roots S2A_REFRESH_LOCKS=$T/locks S2A_REFRESH_POLL_S=0.2 bash $T/repo/pack/refresh.sh acnh > $T/$1.out 2>&1; }
# 等「后到那份打出排队那句话」再放行先到那份（或它已退出 / 60 秒防挂）：$1 后到那份的 .out，$2 它的 pid，$3 放行文件
wait_queued() { local i; for i in $(seq 1 600); do grep -q "已有一份刷新在跑" "$1" 2> /dev/null && break; kill -0 "$2" 2> /dev/null || break; sleep 0.1; done; : > "$3"; }
S2A_STUB_HOLD=$T/goA RF A & PA=$!; for i in $(seq 1 600); do grep -q "^A " $T/log 2> /dev/null && break; sleep 0.1; done   # 等 A 第一段落日志（= A 已持锁）再起 B（门禁 gate-host 满载时固定 0.5 秒不够，A 还没拿锁 B 先拿，两份角色对调）；60 秒只防挂（原先按 .holder 无上限等，锁失效时挂死）
RF B & PB=$!; wait_queued $T/B.out $PB $T/goA; wait $PA $PB
ta=$(LC_ALL=C awk '$1 == "A" && $3 == "止" { t = $4 } END { print t }' $T/log); tb=$(LC_ALL=C awk '$1 == "B" && $3 == "起" { print $4; exit }' $T/log)
ck "① 后到的 B 等 A：B 打「已有一份刷新在跑」；B 第一段起 ${tb} ≥ A 最后一段止 ${ta}" 'grep -q "已有一份刷新在跑" $T/B.out && python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) >= float(sys.argv[2]) else 1)" $tb $ta'
free() { python3 "$H/tools/flock_hold.py" "$1" $$ probe; }   # 内核锁已放（tools/plock.sh，审计 P1-27：锁文件常驻，判据是锁空闲不是文件没了）
ck "① 两份都绿、锁已放" 'grep -q "🟢 acnh 现行产物根各阶段与本树一致" $T/A.out && grep -q "🟢 acnh 现行产物根各阶段与本树一致" $T/B.out && free $T/locks/refresh_acnh.lock'
( source $T/repo/tools/plock.sh; plock_take $T/locks/refresh_acnh.lock 0 && echo held > $T/held; sleep 60 ) & dp=$!
until [ -s $T/held ]; do sleep 0.05; done; kill -9 $dp; wait $dp 2> /dev/null; rm -f $T/log   # 持锁者被 kill -9：内核随持锁小进程退出放锁
t0=$(now); RF C; t1=$(now)
ck "② 持有者已死 → 接管不等（$(python3 -c "print(round($t1 - $t0, 1))") 秒）：没打「已有一份刷新在跑」、跑完绿" '! grep -q "已有一份刷新在跑" $T/C.out && grep -q "🟢 acnh 现行产物根各阶段与本树一致" $T/C.out && python3 -c "import sys; sys.exit(0 if $t1 - $t0 < 60 else 1)"'
# —— ②bc 私有根的锁：两份私有根各自从同一份现行起（同 ①：各段桩，停住 / 放行按事件）
for d in p1 p2; do mkdir -p $T/$d/acnh@v1/ctl; touch $T/$d/acnh@v1/ctl/control.nacp; ln -s acnh@v1 $T/$d/acnh@现行; done
RP() { S2A_TAG=$1 S2A_ROOTS=$T/$2 S2A_ROOTS_SHARED=$T/roots S2A_REFRESH_LOCKS=$T/locks S2A_REFRESH_POLL_S=0.2 bash $T/repo/pack/refresh.sh acnh > $T/$1.out 2>&1; }
#   并行按事件证（11-05 维护者：不靠「着色器段睡 2 秒」碰重叠）：D 停在直连库段等放行文件，E 第一段落日志后才放行 D——按根加锁则 E 不被挡、E 起 < D 止；
#   锁没按根分则 E 被 D 挡住，30 秒等不到 E 的日志照样放行 D，E 起 > D 止 → 红
rm -f $T/log; S2A_STUB_HOLD=$T/goD RP D p1 & PD=$!; for i in $(seq 1 600); do ls $T/locks/refresh_acnh@*.lock.holder > /dev/null 2>&1 && break; sleep 0.05; done
RP E p2 & PE=$!; for i in $(seq 1 600); do grep -q "^E " $T/log 2> /dev/null && break; sleep 0.05; done; : > $T/goD; wait $PD $PE   # 私有根锁名带根哈希；30 秒等不到 = 没按根加锁，往下走由判据报红
dz=$(LC_ALL=C awk '$1 == "D" && $3 == "止" { t = $4 } END { print t }' $T/log); ea=$(LC_ALL=C awk '$1 == "E" && $3 == "起" { print $4; exit }' $T/log)
ck "②b 同款两个私有根：都没打「已有一份刷新在跑」、E 第一段起 ${ea} < D 最后一段止 ${dz}（并行）、两份都绿、锁都放" '! grep -q "已有一份刷新在跑" $T/D.out $T/E.out && python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) < float(sys.argv[2]) else 1)" $ea $dz && grep -q "🟢 acnh 现行产物根各阶段与本树一致" $T/D.out && grep -q "🟢 acnh 现行产物根各阶段与本树一致" $T/E.out && for l in $T/locks/refresh_acnh@*.lock; do free "$l" || exit 1; done'
rm -f $T/log; S2A_STUB_HOLD=$T/goF RP F p1 & PF=$!; for i in $(seq 1 600); do grep -q "^F " $T/log 2> /dev/null && break; sleep 0.05; done
RP G p1 & PG=$!; wait_queued $T/G.out $PG $T/goF; wait $PF $PG   # F 停在直连库段，G 打出「已有一份刷新在跑」才放行 F；等 F 第一段落日志（= F 已持锁）再起 G；不能按 .holder 判：②b 的 D 已在同一私有根锁留下 .holder（tools/plock.sh 放锁不删），F 没拿锁 G 先起、两份角色对调（11-02 a55b5c839 无缓存实跑红）
fz=$(LC_ALL=C awk '$1 == "F" && $3 == "止" { t = $4 } END { print t }' $T/log); ga=$(LC_ALL=C awk '$1 == "G" && $3 == "起" { print $4; exit }' $T/log)
ck "②c 同一个私有根两份：G 打「已有一份刷新在跑」、G 第一段起 ${ga} ≥ F 最后一段止 ${fz}" 'grep -q "已有一份刷新在跑" $T/G.out && python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) >= float(sys.argv[2]) else 1)" $ga $fz'
[ $bad = 0 ] && echo "post_lite 单测全过" || { echo "post_lite 单测有不通"; exit 1; }
