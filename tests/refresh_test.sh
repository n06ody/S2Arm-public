#!/bin/bash
# pack/refresh.sh 单测（各阶段换成桩，只验编排；约 10 秒）：bash tests/refresh_test.sh
# 期望（10-25 维护者「一次出完整包」，按编排规则手写）：
#   ① 只有直连（10-30 直连单轨；11-07 拆兼容模式、精简档与只服务通用模式的着色器阶段 / 客体代码表）：出直连库，不再跑 shaders / direct_gk；
#     管线种子表 direct_seed（审计 #1534 P1-54）在直连库之后、只跑一次
#   ② 直连库阶段退 0 却没出库（没有 shaders/direct/keys.tsv）→ 报红「直连单轨停在 直连库」、退出 1、不跑种子表（不退成通用包）
#   ②b 抽取缺口出的运行时现编空库（keys.tsv 空、有 runtime_only.txt，11-05 维护者：直连库编不出也照出包）→ 照常退 0
#   ④ 直连库段内容缺口（退 3）→ 报红「直连单轨停在 直连库：直连缺口：<类别>」、退出 1、不跑种子表；直连库段基础设施出错（退 1）→ 报红、退出 1、不跑种子表
#   ⑤ 零配置（10-25 直连优先）：跑配置推导与直连库，不看有没有手写配置档；环境里留着旧的 S2A_DUAL=0 / S2A_PACK_LITE=1 也照样只出直连（11-07 两个开关已删）
#   ⑥ 零配置推导不支持本款（direct_profile 退 3）→ 报红「直连单轨停在 直连配置推导」、退出 1、不跑直连库；推导出错（退 1）→ 报红、退出 1
#   ⑩ 开工判一次盘：可用 10 G < 要的 40 G → 退出 1、红行写清要多少 / 剩多少、一个阶段都不跑；可用 50 G ≥ 40 G → 照常刷
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0; trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
mkdir -p $T/repo/pack $T/roots/acnh@v1/ctl; cp "$H/pack/refresh.sh" "$H/pack/roots.sh" $T/repo/pack/; mkdir -p $T/repo/tools; cp "$H/tools/toolchain.sh" "$H/tools/toolchain.env" "$H/tools/cores.sh" "$H/tools/progress.sh" "$H/tools/plock.sh" "$H/tools/flock_hold.py" "$H/tools/mem_admit.sh" "$H/tools/mem_peak.tsv" "$H/tools/mem_peak_sample.py" $T/repo/tools/; export S2A_MEM_DIR=$T/mem; export S2A_DISK_FREE_G=999 S2A_REFRESH_LOCKS=$T/locks S2A_RUN_ROOT=$T/run; touch $T/roots/acnh@v1/ctl/control.nacp; ln -s acnh@v1 $T/roots/acnh@现行
mkdir -p $T/repo/translator; printf 'import os\nprint(os.environ.get("FAKE_GLSLC", "no"))\n' > $T/repo/translator/glslc_offline.py   # 运行时 GLSLC 款判据桩（⑬c）
cp "$H/pack/decoder_need.sh" $T/repo/pack/; mkdir -p $T/repo/graphics/nvn/tools; cp "$H/graphics/nvn/tools/romfs_list.py" $T/repo/graphics/nvn/tools/   # 解码器款判据（⑬e）：真判据读假根 RomFS
printf '#!/bin/bash\necho "原件核对桩 rc=${FAKE_SRCVER_RC:-0}"; exit ${FAKE_SRCVER_RC:-0}\n' > $T/repo/pack/srcver.sh   # 原件版本核对桩（pack/srcver.sh 自己的单测见 tests/srcver_test.sh）
# 桩：记「阶段 起/止 时刻 开关」；stage_direct 按 FAKE_NODIRECT 决定出不出 keys.tsv；FAKE_FAIL=<阶段> 让那段退出 1（基础设施出错）；
#   FAKE_GAP=<阶段> 让那段打「【黄】直连缺口：桩缺口 3 支」退出 3（内容缺口）；FAKE_DFAIL=<阶段> 让那段打「【红】直连库失败：…」退出 3（抽取：只有直连用的那两份抽不出）
for s in rewrite sysdata uilayout aoc caps romfsidx direct_profile extract direct_seed direct; do
  cat > $T/repo/pack/stage_$s.sh <<EOF
#!/bin/bash
echo "$s 起 \$(python3 -c 'import time;print(time.time())') \${S2A_SHADER_NO_PSO:-0}\${S2A_SHADER_NO_DL:-0} J=\${J:-无} DD=\${DEVELOPER_DIR:-无} LZ=\${S2A_SHADER_LAZY:-0}" >> $T/log
[ "$s" = direct ] && sleep \${FAKE_DIRECT_SLEEP:-1}
[ "$s" = rewrite ] && sleep \${FAKE_RW_SLEEP:-0}
# FAKE_RW_UNTIL=<阶段>：改写等那个阶段在日志里记下「起」才结束（最多 30 秒）——按事件判并行，不靠墙钟窗口（⑬a：满载时直连库可能晚于固定的 2 秒才起）
if [ "$s" = rewrite ] && [ -n "\${FAKE_RW_UNTIL:-}" ]; then for i in \$(seq 600); do grep -q "^\$FAKE_RW_UNTIL 起 " $T/log && break; sleep 0.05; done; fi
if [ "$s" = direct ]; then d=\$(cd -P $T/roots/acnh@现行 && pwd)/shaders/direct; rm -rf "\$d"; if [ -n "\${FAKE_RTONLY:-}" ]; then mkdir -p "\$d" && : > "\$d/keys.tsv" && echo 缺口 > "\$d/runtime_only.txt"; elif [ -n "\${FAKE_VKHDR:-}" ]; then mkdir -p "\$d/vk" && : > "\$d/keys.tsv" && printf "# key\tfile\n" > "\$d/vk/vk_shaders.tsv"; else [ -n "\${FAKE_NODIRECT:-}" ] || { mkdir -p "\$d" && echo k > "\$d/keys.tsv"; }; fi; fi
echo "$s 止 \$(python3 -c 'import time;print(time.time())')" >> $T/log
[ "\${FAKE_FAIL:-}" = "$s" ] && { echo "🔴 $s 桩失败"; exit 1; }
[ "\${FAKE_GAP:-}" = "$s" ] && { echo "【黄】acnh 直连缺口：桩缺口 3 支"; exit 3; }
[ "\${FAKE_DFAIL:-}" = "$s" ] && { echo "【红】acnh 直连库失败：直连抽取失败（桩：Unity 无类型树）"; exit 3; }
echo "🟢 $s 桩"
EOF
done
R() { rm -f $T/log; S2A_REFRESH_LOCKS=$T/locks S2A_ROOTS=$T/roots S2A_ROOTS_SHARED=$T/roots bash $T/repo/pack/refresh.sh acnh 2>&1; }
# macOS 自带 awk（version 20200816，UTF-8 locale）字符串比较有坑：$2 == "起" 连 "止" 都判相等——起 / 止一律先按字节 grep 出再交 awk
t() { grep " $2 " $T/log | awk -v s="$1" '$1 == s { print $3; exit }'; }
o=$(R); r=$?
ck "① 只有直连：rc=${r}；不跑通用着色器阶段 / 客体代码表" '[ $r = 0 ] && ! grep -q "^shaders \|^direct_gk " $T/log && grep -q "^direct 起" $T/log'
tl=$(grep '^⏱' <<< "$o"); miss=$(for s in rewrite sysdata uilayout aoc direct_profile romfsidx extract direct direct_seed; do [[ "$tl" == *"$s "[0-9]* ]] || printf '%s ' $s; done)
# 直连段只判「≥1 秒」（桩睡 1 秒 = 计时确实记上了）：不卡上界——10-26 #477 自检 gate-host 并行负载高时 direct 量到 3 秒判红，那是机器忙，不是计时错
ck "① 退出时一行分阶段用时，各段都在（缺：${miss:-无}）、直连 ≥1 秒（桩睡 1 秒）：${tl}" '[ "$(grep -c "^⏱" <<< "$o")" = 1 ] && [ -z "$miss" ] && [[ "$tl" =~ \ direct\ [1-9][0-9]*\  ]] && [[ "$tl" == *"刷新共 "* ]]'
jj() { grep ' 起 ' $T/log | awk -v s="$1" '$1 == s { sub("J=", "", $5); print $5; exit }'; }
o=$(J=18 R); r=$?
ck "① 调用方给 J=18 → 直连 J=$(jj direct) 开满" '[ $r = 0 ] && [ "$(jj direct)" = 18 ]'
# 阶段进度（10-25 维护者「能看到进度么」）：给 S2A_PROGRESS → 每完成一段追加一行「刷新各阶段 已完成 总数」
o=$(S2A_PROGRESS=$T/prog R); r=$?; pg=$(cut -f3,4 $T/prog | tr '\t\n' '/ '); rm -f $T/prog
#   11-06 依赖重排（快路）：追加内容 ‖ 界面布局 2 → 配置推导 + 抽取 4 → 直连库 5 → 种子表 6 → 汇合旁路（改写 / 系统数据 / 能力表 / RomFS 索引）10
ck "① 阶段进度：${pg}" '[ $r = 0 ] && [ "$pg" = "2/10 4/10 5/10 6/10 10/10 " ]'
o=$(R); r=$?
ck "① 管线种子表 direct_seed（审计 #1534 P1-54 B3）在直连库出完之后才跑、只跑一次" 'python3 -c "import sys; a,d=map(float,sys.argv[1:]); sys.exit(0 if a >= d else 1)" $(t direct_seed 起) $(t direct 止) && [ "$(grep -c "^direct_seed 起" $T/log)" = 1 ]'
o=$(FAKE_NODIRECT=1 R); r=$?
ck "② 直连库退 0 却没出库：rc=${r}（期望 1）、报红「直连单轨停在 直连库」、不跑种子表（不退成通用）" '[ $r = 1 ] && [[ "$o" == *"【红】acnh 直连单轨停在 直连库：直连库阶段退 0 但产物根里没有 shaders/direct/keys.tsv"* ]] && ! grep -q "^direct_seed" $T/log'
o=$(FAKE_RTONLY=1 R); r=$?
ck "②b 抽取缺口出的运行时现编空库（keys.tsv 空、有 runtime_only.txt，11-05）→ rc=${r}（期望 0）、不报直连单轨停在、照跑种子表" '[ $r = 0 ] && [[ "$o" != *直连单轨停在* ]] && grep -q "^direct_seed 起" $T/log'
o=$(FAKE_VKHDR=1 R); r=$?
ck "②c Vulkan 款只出表头（keys.tsv 空、有 vk/vk_shaders.tsv，11-05 r4 Minecraft）→ rc=${r}（期望 0）、不报直连单轨停在、照跑种子表" '[ $r = 0 ] && [[ "$o" != *直连单轨停在* ]] && grep -q "^direct_seed 起" $T/log'
o=$(FAKE_GAP=direct R); r=$?
ck "④ 直连库内容缺口（退 3）→ rc=${r}（期望 1）、报红带缺口类别、不跑种子表（不退成通用）" '[ $r = 1 ] && [[ "$o" == *"【红】acnh 直连单轨停在 直连库：直连缺口：桩缺口 3 支"* ]] && ! grep -q "^direct_seed" $T/log'
o=$(FAKE_FAIL=direct R); r=$?
ck "④ 直连库基础设施出错（退 1）→ rc=${r}（期望 1）、打【红】、不退成通用、不跑种子表" '[ $r = 1 ] && [[ "$o" == *"【红】acnh 刷新停在 direct 阶段"* ]] && ! grep -q "^direct_seed" $T/log'
ck "④ 失败退出也打分阶段用时（停在的 direct 段在里面）" '[[ "$(grep "^⏱" <<< "$o")" =~ \ direct\ [0-9]+ ]]'
o=$(S2A_DUAL=0 S2A_PACK_LITE=1 R); r=$?
ck "⑤ 环境里留着旧的 S2A_DUAL=0 / S2A_PACK_LITE=1：照样跑配置推导、抽取与直连库（rc=${r}）" '[ $r = 0 ] && grep -q "^direct_profile 起" $T/log && grep -q "^extract 起" $T/log && grep -q "^direct 起" $T/log && grep -q "^direct_seed 起" $T/log'
o=$(R); r=$?
ck "⑧ 抽着色器只做一次：抽取阶段在直连库之前、只跑一次" '[ $r = 0 ] && [ "$(grep -c "^extract 起" $T/log)" = 1 ] && python3 -c "import sys; e,d=map(float,sys.argv[1:]); sys.exit(0 if e <= d else 1)" $(t extract 止) $(t direct 起)'
o=$(FAKE_FAIL=extract R); r=$?
ck "⑧ 抽取出错 → rc=1、打【红】、不跑直连库（rc=${r}）" '[ $r = 1 ] && echo "$o" | grep -q "【红】.*刷新停在抽取阶段" && ! grep -q "^direct 起" $T/log'
# ⑧b 抽取退 3（10-26 空洞骑士：只给直连用的两份抽不出）→ 直连单轨报红「直连单轨停在 抽取阶段：直连库失败：<原因>」、退出 1、不跑直连库（不退成通用）
o=$(FAKE_DFAIL=extract R); r=$?
ck "⑧b 抽取只坏直连那两份（退 3）→ rc=${r}（期望 1）、红行带原因、不跑直连库" '[ $r = 1 ] && [[ "$o" == *"【红】acnh 直连单轨停在 抽取阶段：直连库失败：直连抽取失败（桩：Unity 无类型树）"* ]] && ! grep -q "^direct 起" $T/log'
o=$(FAKE_GAP=direct_profile R); r=$?
ck "⑥ 零配置推导不支持（退 3）→ rc=${r}（期望 1）、报红「直连单轨停在 直连配置推导」、不跑直连库" '[ $r = 1 ] && [[ "$o" == *"【红】acnh 直连单轨停在 直连配置推导：直连缺口：配置推导不支持本款"* ]] && ! grep -q "^direct " $T/log'
o=$(FAKE_FAIL=direct_profile R); r=$?
ck "⑥ 零配置推导出错（退 1）→ rc=${r}（期望 1）、打【红】、不跑直连库" '[ $r = 1 ] && [[ "$o" == *"【红】acnh 刷新停在直连配置推导"* ]] && ! grep -q "^direct " $T/log'
# ⑦ 工具链解析一次、全程共用（10-25：一键出包外层 refresh 与 ipa.sh 里的 refresh 各取各的 Metal 版本，直连库整份出两遍）：
#   两个假 Xcode（假 xcrun 按 DEVELOPER_DIR 回各自的 SDK / Metal 版本），启动器传哪个，各阶段看到的就都是哪个，不串版本
mkdir -p $T/fx/bin $T/fx/A $T/fx/B; echo 27.0 > $T/fx/A/sdk; echo "Apple metal version 32023.1" > $T/fx/A/metal; echo 26.5 > $T/fx/B/sdk; echo "Apple metal version 32023.2" > $T/fx/B/metal
printf '#!/bin/bash\ncase " $* " in *" --show-sdk-version "*) cat "$DEVELOPER_DIR/sdk";; *" metal --version "*) cat "$DEVELOPER_DIR/metal";; *) exit 1;; esac\n' > $T/fx/bin/xcrun; chmod +x $T/fx/bin/xcrun
for x in A B; do env -u DEVELOPER_DIR -u S2A_TC_RESOLVED -u S2A_TC_METAL -u S2A_PSO_OS "PATH=$T/fx/bin:$PATH" S2A_MACHINE_NO_LOCAL=1 S2A_DEVELOPER_DIR=$T/fx/$x bash -c "T=$T; $(declare -f R); R" > /dev/null
  eval "dd_$x=\$(grep ' 起 ' $T/log | cut -d' ' -f6 | sort -u | tr '\n' ' ')"; done
ck "⑦ 启动器传 A → 各阶段都是 A（${dd_A}）；传 B → 都是 B（${dd_B}）" '[ "$dd_A" = "DD=$T/fx/A " ] && [ "$dd_B" = "DD=$T/fx/B " ]'
# Android 使用同一刷新入口，但模块由 Android make_pack 负责；不运行 Apple 改写或 Metal 阶段。
#   #783：romfsidx 之后先跑抽取阶段（stage_extract，答 1 选 a：它的键与平台无关，兄弟根克隆来的 extract/ 键相同就跳过——那一条由 tests/stage_extract_test.sh
#   「同键 → 跳过、不再抽」核，这里只核安卓路线确实调它、在直连库之前），再进直连库段（stage_direct，出离线 Vulkan 库）。手机端还不读这份库（答 ③）：
#   两段任何非 0 都打黄、照常出包（抽取没成就不跑直连库），黄项（计数 + 原因）带进「Android 共享资源已刷新」那行（一键出包的汇总从这行取），输出副本进 <产物根目录>/.failed/
#   11-04 起这两段跑不跑由 pack/targets/android/target.json 的 direct 阶段声明定：enabled=false（缺省，安卓运行时未读 LibPack）→
#   逐段与末行标 skipped（…#1434 接上后恢复）、不跑抽取 / 直连库；enabled=true 或文件读不出 → 照旧跑。
# 夹具目标描述：ATE <enabled> 写 $T/repo/pack/targets/android/target.json（缺省 false）
mkdir -p $T/repo/pack/targets/android
ATE() { cat > $T/repo/pack/targets/android/target.json <<EOF
{"schema":"s2a.target.v1","id":"android","stages":[{"id":"direct","description":"x","implementation":"pack/stage_direct.sh","execution":"build","startsGame":false,"observation":"declared","enabled":${1:-false}}]}
EOF
}
ATE false
o=$(rm -f $T/android-prog; S2A_PACK_PLAT=android S2A_PROGRESS=$T/android-prog R); r=$?; ran=$(grep ' 起 ' $T/log | cut -d' ' -f1 | LC_ALL=C sort | tr '\n' ' '); pg=$(cut -f3,4 $T/android-prog 2> /dev/null | tr '\t\n' '/ ')
ck "⑫ Android enabled=false：跑系统数据 / 追加内容 / 能力清单 / 索引，抽取与直连库 skipped、不跑，rc=${r}，阶段 ${ran}，进度 ${pg}" \
  '[ $r = 0 ] && [ "$ran" = "aoc caps romfsidx sysdata " ] && [ "$pg" = "2/6 3/6 4/6 5/6 6/6 " ] && grep -q "^extract skipped（安卓运行时未读 LibPack，#1434 接上后恢复）$" <<< "$o" && grep -q "^direct skipped（安卓运行时未读 LibPack，#1434 接上后恢复）$" <<< "$o" && ! grep -q "^extract " $T/log && ! grep -q "^direct " $T/log'
ck "⑫ Android enabled=false：末行带 skipped 黄项（一键出包的 result warnings 从这行取，rc=${r}）" \
  'grep -q "^🟢 acnh Android 共享资源已刷新：.*；🟡 1 项：离线 Vulkan 直连库 skipped（安卓运行时未读 LibPack，#1434 接上后恢复）$" <<< "$o"'
ck "⑫ Android enabled=false：⏱ 分阶段用时里 extract / direct 记 0 秒" '[[ "$(grep "^⏱" <<< "$o")" =~ extract\ 0\ .*direct\ 0 ]]'
ATE true
o=$(rm -f $T/android-prog; S2A_PACK_PLAT=android S2A_PROGRESS=$T/android-prog R); r=$?; ran=$(grep ' 起 ' $T/log | cut -d' ' -f1 | LC_ALL=C sort | tr '\n' ' '); pg=$(cut -f3,4 $T/android-prog 2> /dev/null | tr '\t\n' '/ ')
ck "⑪ Android enabled=true：系统数据、追加内容、能力清单、索引、抽取、直连库，rc=${r}，阶段 ${ran}，进度 ${pg}" '[ $r = 0 ] && [ "$ran" = "aoc caps direct extract romfsidx sysdata " ] && [ "$pg" = "2/6 3/6 4/6 5/6 6/6 " ]'
ck "⑪ Android 抽取在 RomFS 索引之后、直连库在抽取之后，各跑一次" 'python3 -c "import sys; e,i,d,x=map(float,sys.argv[1:]); sys.exit(0 if e >= i and d >= x else 1)" $(t extract 起) $(t romfsidx 止) $(t direct 起) $(t extract 止) && [ "$(grep -c "^extract 起" $T/log)" = 1 ] && [ "$(grep -c "^direct 起" $T/log)" = 1 ]'
for stage in sysdata aoc caps romfsidx; do
  o=$(S2A_PACK_PLAT=android FAKE_FAIL=$stage R); r=$?
  ck "⑪ Android ${stage} 失败即红" '[ $r = 1 ] && [[ "$o" == *"刷新停在 $stage 阶段"* ]]'
done
ar() { grep '^🟢 acnh Android 共享资源已刷新：' <<< "$o" | tail -1; }
o=$(S2A_PACK_PLAT=android R); r=$?
ck "⑪ Android 直连库出成 → 那行不带黄项（rc=${r}）" '[ $r = 0 ] && [ -n "$(ar)" ] && [[ "$(ar)" != *🟡* ]]'
rm -f $T/roots/.failed/acnh_android_direct.out; o=$(S2A_PACK_PLAT=android FAKE_FAIL=direct R); r=$?
ck "⑪ Android 直连库工具出错（退 1）→ 打黄、照常出包、那行带黄项与原因、输出副本进 .failed/（rc=${r}；$(ar | sed 's/^.*；//' | cut -c1-70)）" '[ $r = 0 ] && [[ "$(ar)" == *"；🟡 1 项：安卓直连库没出（direct rc=1：🔴 direct 桩失败；运行时现编）" ]] && [ -s $T/roots/.failed/acnh_android_direct.out ] && [[ "$o" != *【红】* ]]'
o=$(S2A_PACK_PLAT=android FAKE_GAP=direct R); r=$?
ck "⑪ Android 直连库内容缺口（退 3）→ 打黄、照常出包、那行带缺口原文（rc=${r}）" '[ $r = 0 ] && [[ "$(ar)" == *"；🟡 1 项：安卓直连库没出（direct rc=3：acnh 直连缺口：桩缺口 3 支；运行时现编）" ]]'
rm -f $T/roots/.failed/acnh_android_extract.out; o=$(S2A_PACK_PLAT=android FAKE_FAIL=extract R); r=$?
ck "⑪ Android 抽取出错（退 1）→ 打黄、不跑直连库、照常出包、副本进 .failed/（rc=${r}）" '[ $r = 0 ] && [[ "$(ar)" == *"；🟡 1 项：安卓直连库没出（extract rc=1：🔴 extract 桩失败；运行时现编）" ]] && ! grep -q "^direct 起" $T/log && [ -s $T/roots/.failed/acnh_android_extract.out ]'
o=$(S2A_PACK_PLAT=android FAKE_DFAIL=extract R); r=$?
ck "⑪ Android 抽取只坏直连那两份（退 3）→ 打黄带原因、不跑直连库（rc=${r}）" '[ $r = 0 ] && [[ "$(ar)" == *"；🟡 1 项：安卓直连库没出（extract rc=3：acnh 直连库失败：直连抽取失败（桩：Unity 无类型树）；运行时现编）" ]] && ! grep -q "^direct 起" $T/log'
# ⑬ 依赖重排（11-06 出包速度线，快路）：直连链不等改写那一组；期望按依赖手写（桩记起 / 止时刻）
lt() { python3 -c "import sys; a,b=map(float,sys.argv[1:]); sys.exit(0 if a < b else 1)" "$1" "$2"; }
o=$(FAKE_RW_UNTIL=direct R); r=$?
ck "⑬a 改写等直连库开工才结束：直连库在改写结束前已开工（rc=${r}；直连库若排在改写之后，改写等满 30 秒才止，这条红）" '[ $r = 0 ] && lt $(t direct 起) $(t rewrite 止)'
ck "⑬a 能力表在改写之后；抽取 / RomFS 索引在界面布局之后；抽取在追加内容与配置推导之后" '! lt $(t caps 起) $(t rewrite 止) && ! lt $(t extract 起) $(t uilayout 止) && ! lt $(t romfsidx 起) $(t uilayout 止) && ! lt $(t extract 起) $(t aoc 止) && ! lt $(t extract 起) $(t direct_profile 止)'
# ⑬e（见下）解码器款同样等改写
# ⑬d 运行时 GLSLC 款（11-07 gridfix 新私有根）：抽取要改写后的模块 → 改写睡 2 秒时抽取在改写结束之后才开工；改写红 → 退 1、不跑抽取 / 直连库
o=$(FAKE_GLSLC=yes FAKE_RW_SLEEP=2 R); r=$?
ck "⑬d GLSLC 款：抽取等改写结束才开工（rc=${r}）" '[ $r = 0 ] && ! lt $(t extract 起) $(t rewrite 止)'
o=$(FAKE_GLSLC=yes FAKE_FAIL=rewrite R); r=$?
ck "⑬d GLSLC 款改写红 → 退 1、抽取与直连库都没跑（rc=${r}）" '[ $r = 1 ] && ! grep -q "^extract 起\|^direct 起" $T/log'
# ⑬e 要游戏自带解码器的款（11-07 DOOM Eternal：RomFS 有 id Tech .resources，抽取在 Mac 宿主工具模式里跑游戏的 Oodle，要改写后的模块）：
#   RomFS 放一个 .resources → 改写睡 2 秒时抽取在改写结束之后才开工；改写红 → 退 1、抽取与直连库都没跑。判据是真的 pack/decoder_need.sh + romfs_list.py
mkdir -p "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/base" && : > "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/base/gameresources.resources"
o=$(FAKE_RW_SLEEP=2 R); r=$?
ck "⑬e 解码器款（.resources）：抽取等改写结束才开工（rc=${r}）" '[ $r = 0 ] && ! lt $(t extract 起) $(t rewrite 止)'
mkdir -p "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/base" && : > "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/base/gameresources.resources"
o=$(FAKE_FAIL=rewrite R); r=$?
ck "⑬e 解码器款改写红 → 退 1、抽取与直连库都没跑（rc=${r}）" '[ $r = 1 ] && ! grep -q "^extract 起\|^direct 起" $T/log'
rm -f "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/base/gameresources.resources"
# ⑬f Trinity 包（#470 0100F43008C44000：arc/data.trpfs，Oodle）同样是解码器款：抽取等改写结束才开工（原先不在名单里，抽取抢在改写前、离线 0 支）
mkdir -p "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/arc" && : > "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/arc/data.trpfs"
o=$(FAKE_RW_SLEEP=2 R); r=$?
ck "⑬f 解码器款（.trpfs）：抽取等改写结束才开工（rc=${r}）" '[ $r = 0 ] && ! lt $(t extract 起) $(t rewrite 止)'
rm -f "$(cd -P $T/roots/acnh@现行 && pwd)/romfs/arc/data.trpfs"
o=$(FAKE_FAIL=rewrite R); r=$?
ck "⑬b 改写红 → rc=${r}（期望 1）、红行点名 rewrite、不跑能力表" '[ $r = 1 ] && grep -q "刷新停在 rewrite 阶段" <<< "$o" && [ -z "$(t caps 起)" ]'
o=$(FAKE_FAIL=romfsidx R); r=$?
ck "⑬c 旁路 RomFS 索引红 → rc=${r}（期望 1）、红行点名 romfsidx" '[ $r = 1 ] && grep -q "刷新停在 romfsidx 阶段" <<< "$o"'
# ⑭ 早装信号（11-06 出包与装机重叠）：设 S2A_EARLY_READY → 旁路 RomFS 索引后写出现行版本名，写出时刻早于直连库结束（这条用例桩直连库睡 3 秒）；不设 → 不写
rm -f $T/early; o=$(S2A_EARLY_READY=$T/early FAKE_DIRECT_SLEEP=3 R); r=$?   # 直连库桩睡 3 秒：旁路要起几个进程，余量拉开免得机器忙时赛跑
em=$(python3 -c 'import os,sys; print(os.path.getmtime(sys.argv[1]))' $T/early 2> /dev/null)
ck "⑭ 早装信号：rc=${r}、写了现行名「$(cat $T/early 2> /dev/null)」、写出 ${em} 早于直连库结束 $(t direct 止)" '[ $r = 0 ] && [ "$(cat $T/early)" = "$(basename "$(cd -P $T/roots/acnh@现行 && pwd)")" ] && lt "$em" $(t direct 止)'
rm -f $T/early; o=$(R); ck "⑭ 不设 S2A_EARLY_READY 不写" '[ ! -e $T/early ] && ! ls $T/early.tmp > /dev/null 2>&1'
[ $bad = 0 ] && echo "refresh 单测全过" || { echo "refresh 单测有不通"; exit 1; }
