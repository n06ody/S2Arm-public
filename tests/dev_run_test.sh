#!/bin/bash
# 在两种 sed 上用临时文件完成同一替换。
sed_edit() { sed -i.s2a-bak "$1" "$2" && rm -f "$2.s2a-bak"; }
# 真机执行器单测（假 devicectl + 假 App，不碰手机，约 20 秒；轮询 0.2 秒、停顿判据 2 秒、采 1 秒）：bash tests/dev_run_test.sh
# 期望值按契约手写：首局装包 + 推全部文件 + 解包；同包再跑不重装、推 0 个；改 1 个文件只推 1 个；
# MARK 后回收 → 分配闸门红；出图停 10 秒 → 红；GPU Hang / PageFault 原文 → 当轮红即停；崩溃 → 红；没 MARK → 红；包名错 / 硬件 UDID 不符 / 日常手机 → 红且不起局
# 门禁覆盖：tools/gpu_hang.sh
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
export S2A_DISK_HELD=1   # 工作盘准入闸（tools/disk_admit_gate.sh）由 disk_admit_test 测；本测不读真盘、不写真账
export S2A_DEVICES_NO_LOCAL=1 S2A_MACHINE_NO_LOCAL=1 S2A_VISION_BRIDGE=0   # 驾驶员桥缺省关（它局中拉信箱文件）；只在专验它的用例里开
export S2A_TEST_ROOT=$T/lock S2A_DEVICE_NO_USB_CHECK=1 DEVBATTERY_FAKE="80 1 1 0" FAKE_DEV=$T/dev
export S2A_OWNPLAY_DIR=$T/ownplay S2A_BOARD=$T/board.tsv S2A_SCHED_DIR=$T/sched S2A_PSO_SEED_DIR=$T/pso_seed S2A_DEVICE_LEDGER=$T/ledger.tsv   # 非测试局日志抢救 / 首遇现编报 / 管线种子来源：只写临时目录，不碰真记分板、收件箱与 pso_seed/
# 停顿判据缺省 10 秒（同真机缺省）：本机负载高时假 App（python）会被饿住 2–3 秒不出帧，判据 2 秒会把别的用例误判「出图停在」（10-22 晚实测）；
#   只有专验停帧的用例（stall / livef / gpuslow）各自显式设短
export S2A_DEVICECTL="python3 $H/tests/fake_devicectl.py" S2A_POLL_S=0.2 S2A_POLL_APPLY_S=0.2 S2A_STALL_S=10 MARK_AT=5 SW_DEVLOCK_HIST=$T/h.tsv SW_DEVLOCK_WD_LOG=$T/wd.log
DV=$T/dev/00000000-0000-0000-0000-000000000001; mkdir -p $T/lock $T/dev $T/bin; for c in ideviceinfo idevicediagnostics; do printf '#!/bin/sh\nexit 1\n' > $T/bin/$c; chmod +x $T/bin/$c; done; export PATH=$T/bin:$PATH
mkipa() {   # $1 输出 ipa，$2 包名
  local w=$T/ipa_$$_$RANDOM; mkdir -p $w/Payload/X.app
  plutil -create xml1 $w/Payload/X.app/Info.plist; plutil -insert CFBundleIdentifier -string "$2" $w/Payload/X.app/Info.plist
  (cd $w && zip -qr "$1" Payload); rm -rf $w
}
mkpack() {   # $1 包目录：重算 index.tsv 与 manifest
  (cd $1 && find . -type f ! -name index.tsv ! -name manifest.json | sed 's#^\./##' | LC_ALL=C sort | while read -r f; do printf '%s\t%s\t%s\n' "$f" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"; done > index.tsv
   printf '{"version": 2, "index_sha256": "%s"}\n' "$(shasum -a 256 index.tsv | cut -d' ' -f1)" > manifest.json)
}
J=$T/job; mkdir -p $J/pack/romfs $J/save/device; for i in 1 2 3; do echo "file $i" > $J/pack/romfs/f$i.bin; done; echo s > $J/save/device/s.dat; echo 1600000000 > $J/system_time; echo Japanese > $J/language; mkpack $J/pack
mkipa $J/app.ipa com.s2arm.test
printf 'DEV=1\nBUNDLE=com.s2arm.test\nACT_S=1\nARRIVE_S=30\n' > $J/job.env   # 到场上限 30 秒（原 3 秒：满载门禁里假 App 被饿住、第 40 帧的 MARK 超 3 秒，stall2 判成「没到达代表场景」，10-23 列车红）；只验没 MARK 的 nomark 组自己设回 3 秒
# 用例按依赖分组、各组并行（10-20 回归提速）：每组一份独立的假设备目录 / 设备锁 / 作业目录，组内顺序跑
grp() {   # grp <组名>：建本组环境（作业目录从模板复制），之后组内 run / ck
  G=$T/g_$1; mkdir -p $G/lock $G/dev; cp -R $T/job $G/job; J=$G/job
  export S2A_TEST_ROOT=$G/lock FAKE_DEV=$G/dev SW_DEVLOCK_HIST=$G/h.tsv SW_DEVLOCK_WD_LOG=$G/wd.log; DV=$G/dev/00000000-0000-0000-0000-000000000001
}
run() { local o; o=$(timeout 200 bash $H/tools/dev_run.sh $J 2>&1); RES=$(printf '%s\n' "$o" | grep '^RESULT=' | tail -1); OUT=$(printf '%s' "$RES" | sed -n 's/.*out=\([^ ]*\).*/\1/p'); LOG="$o"; }
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; echo "    $RES"; printf '%s\n' "$LOG" | tail -8 | sed 's/^/    | /'; fi; }

g_incr() { grp incr   # 增量装包 / 推送：同一台假设备上依次跑
run; ck "首局 → done" '[[ $RES == RESULT=done* ]]'
ck "首局装包 1 次" '[ "$(wc -l < $DV/installs.log)" -eq 1 ]'
ck "首局推 3 个资源文件" 'grep -q "资源包：推 3 个文件" $OUT/run.log'
[ -z "${S2A_PRIVATE_TOOLS:-}" ] || ck "首局进统一账本：本局目录一行、设备列写设备名「测试设备1」（不是 CoreDevice 标识）、结果 done、窗口 MARK，run.log 记「统一账本」摘要（tools/device_ledger.py）" 'LC_ALL=C awk -F"\t" -v o=$OUT "\$NF==o && \$4==\"测试设备1\" && \$6==\"done\" && \$7==\"MARK\"{f=1} END{exit !f}" $S2A_DEVICE_LEDGER && grep -q "统一账本（MARK）" $OUT/run.log'
ck "首局 run.log 出五问帧率格（按 frames.tsv 同记分板口径；局只有几秒 → 未测）" 'grep -q "五问帧率格：未测（" $OUT/run.log'
ck "首局机上解包、.applied 对上" '[ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/.applied)" = "$(python3 -c "import json;print(json.load(open(\"$J/pack/manifest.json\"))[\"index_sha256\"])")" ] && [ -f $DV/apps/com.s2arm.test/Documents/资源包/romfs/f2.bin ]'
ck "存档推到每局独立目录、起局带 S2A_SAVE_DIR 与 S2A_SYSTEM_TIME" 'ls -d $DV/apps/com.s2arm.test/Documents/sv_*/device/s.dat > /dev/null && tail -1 $DV/launches.log | grep -q "\"S2A_SAVE_DIR\": \"sv_" && tail -1 $DV/launches.log | grep -q "\"S2A_SYSTEM_TIME\": \"1600000000\"" && tail -1 $DV/launches.log | grep -q "\"S2A_SYSTEM_LANGUAGE\": \"Japanese\""'
ck "起局显式带 S2A_AUTOPLAY=1（iPhone 上自动玩家缺省关，人手动打开 App 时不该自己按键）" 'tail -1 $DV/launches.log | grep -q "\"S2A_AUTOPLAY\": \"1\""'
ck "分配闸门绿" 'grep -q "^绿" $OUT/gate.txt'
ck "拉回 host.log / frames.tsv 非空" '[ -s $OUT/host.log ] && [ -s $OUT/frames.tsv ]'
ck "用时拆账 8 列" '[ "$(awk -F"\t" "{print NF}" $OUT/timing.tsv)" = 8 ]'
ck "锁已放" '[ ! -d $G/lock/device.lock ]'
run; ck "同包再跑：不重装" 'grep -q "不重装" $OUT/run.log && [ "$(wc -l < $DV/installs.log)" -eq 1 ]'
ck "同包再跑：推 0 个文件、不再解包" 'grep -q "资源包：推 0 个文件" $OUT/run.log && ! grep -q "机上解开推送包" $OUT/run.log'
echo changed > $J/pack/romfs/f2.bin; mkpack $J/pack
run; ck "改 1 个文件：只推 1 个并解包" 'grep -q "资源包：推 1 个文件" $OUT/run.log && grep -q "机上解开推送包" $OUT/run.log && [ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/romfs/f2.bin)" = changed ]'
# 机上实况与账不符（10-25 测试设备2 马车8：已应用索引记着、机上没有）：删掉机上一个文件、账不动 → 抽查发现；按机上实际清单核账，只补推缺的这 1 个（11-02 前整包重推）
rm -f $DV/apps/com.s2arm.test/Documents/资源包/romfs/f1.bin
run; ck "机上丢了文件、已应用索引还记着：抽查不符 → 按机上清单核账、只补推 1 个、文件补回、done" '[[ $RES == RESULT=done* ]] && grep -q "机上实况与已应用索引不符" $OUT/run.log && grep -q "按机上实际清单核账：2/3 行" $OUT/run.log && grep -q "资源包：推 1 个文件" $OUT/run.log && [ -f $DV/apps/com.s2arm.test/Documents/资源包/romfs/f1.bin ]'
rm -f $DV/apps/com.s2arm.test/Documents/资源包/romfs/f1.bin
FAKE_LIST_FAIL_SUB=Documents/资源包 run; ck "抽查不符、机上清单又拉不回 → 退回整包重推（推 3 个）、补回" '[[ $RES == RESULT=done* ]] && grep -q "机上清单拉不回，不信已应用索引，整包重推" $OUT/run.log && grep -q "资源包：推 3 个文件" $OUT/run.log && [ -f $DV/apps/com.s2arm.test/Documents/资源包/romfs/f1.bin ]'
run; ck "实况一致：抽查通过、推 0 个" 'grep -q "机上实况抽查 3 个文件，与已应用索引一致" $OUT/run.log && grep -q "资源包：推 0 个文件" $OUT/run.log'
S2A_PUSH_FULL=1 run; ck "S2A_PUSH_FULL=1：整包推、照样收推送" '[[ $RES == RESULT=done* ]] && grep -q "S2A_PUSH_FULL=1，整包推送" $OUT/run.log && grep -q "资源包：推 3 个文件" $OUT/run.log && grep -q "机上解开推送包" $OUT/run.log'
echo again > $J/pack/romfs/f3.bin; mkpack $J/pack
FAKE_APPLY=vanish run; ck "推了 1 批、App 记解开 0 个包 → 红（收推送包数不符），不当成功" '[[ $RES == RESULT=push* ]] && grep -q "推了 1 批，机上只解开 0 个包" $OUT/run.log'
run; ck "上一局账写成已应用但文件没到：这局抽查补推、done" '[[ $RES == RESULT=done* ]] && [ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/romfs/f3.bin)" = again ]'
# 包切换：新包删了某文件（真机应用时按新索引删掉旧文件），再换回带它的旧包 → 该文件要补推（累计 .pushed 不能当「机上已有」）
cp -R $J/pack $T/pack_keep; rm $J/pack/romfs/f2.bin; mkpack $J/pack; run; rm -f $DV/apps/com.s2arm.test/Documents/资源包/romfs/f2.bin
rm -rf $J/pack; cp -R $T/pack_keep $J/pack; run; ck "换回旧包：新包删掉的文件补推回机上（10-20 分片包后回退单库包丢 gal.metallib）" '[[ $RES == RESULT=done* ]] && [ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/romfs/f2.bin 2>/dev/null)" = changed ]'
printf '901 /private/var/containers/Bundle/Application/A/S2Arm.app/S2Arm\n' > $DV/processes.txt; rm -f $DV/terminated.log
nl=$(wc -l < $DV/launches.log); DEPLOY_ONLY=1 run; rm -f $DV/processes.txt; ck "只部署不清残留（人可能正在玩）" '[ ! -s $DV/terminated.log ]'
 ck "只部署：RESULT=deployed、不起局（launches 不增）" '[[ $RES == RESULT=deployed* ]] && [ "$(wc -l < $DV/launches.log)" -eq $nl ] && grep -q "只部署" $OUT/run.log'
}
g_wire() { grp wire   # 旧的直连失败标记（10-26 起运行中不切换）：执行器不再查 / 删，App 起局自己删掉、照包判直连
mkdir -p $J/pack/shaders/shcache/0100152000022000; echo c > $J/pack/shaders/shcache/0100152000022000/x; mkpack $J/pack
WD=$DV/apps/com.s2arm.test/Documents/wire/0100152000022000; mkdir -p $WD; echo "GPU 报错 超时" > $WD/fail
run; ck "旧 fail 在：执行器不报、App 起局删掉、本局图形模式直连" '[[ $RES == RESULT=done* ]] && ! grep -q "直连失败标记" $OUT/run.log && [ ! -e $WD/fail ] && grep -q "删掉旧的直连失败标记" $OUT/host.log && grep -q "本局图形模式：直连" $OUT/run.log'
run; ck "没有 fail：图形模式直连" '[[ $RES == RESULT=done* ]] && grep -q "本局图形模式：直连" $OUT/run.log'
}
g_shot() { grp shot   # 截图拉回 / 下局不串
FAKE_SHOT=mark run; ck "自动玩家截图：MARK 时写的 ap_mark.png 拉回局目录 shots_ap/" '[[ $RES == RESULT=done* ]] && [ -s $OUT/shots_ap/ap_mark.png ]'
run; ck "下一局没截图：上局的 ap_mark.png 起局前盖空、不被当本局拉回" '[[ $RES == RESULT=done* ]] && [ ! -e $OUT/shots_ap/ap_mark.png ]'
ENVJSON='"S2A_DIAG_X":"1"' run; ck "诊断局（带 ENVJSON）额外环境照传" '[[ $RES == RESULT=done* ]] && tail -1 $DV/launches.log | grep -q "\"S2A_DIAG_X\": \"1\""'
# 性能测量口径（11-07）：缺省起局环境带 S2A_DEV_FACILITIES=0（同发布包关开发设施）；诊断局在 ENVJSON 显式给 1 时以 ENVJSON 为准
run; ck "测量口径：缺省起局环境 S2A_DEV_FACILITIES=0" '[[ $RES == RESULT=done* ]] && tail -1 $DV/launches.log | grep -q "\"S2A_DEV_FACILITIES\": \"0\""'
ENVJSON='"S2A_DEV_FACILITIES":"1"' run; ck "诊断局 ENVJSON 给 S2A_DEV_FACILITIES=1 → 起局环境为 1" '[[ $RES == RESULT=done* ]] && tail -1 $DV/launches.log | grep -q "\"S2A_DEV_FACILITIES\": \"1\""'
}
g_hang() { grp hang   # 开局通道探针：第一次 copy from 挂死 → 超时后重试一次成功；连挂两次 → 判红收局（不白等 600 秒）
# 探针超时 5 秒：假 devicectl 是 Python，gate-host 门禁并行负载高时重试那次光起解释器就可能过 2 秒，被误判再挂一次（10-26 自检偶发红）
FAKE_HANG_FROM=1 S2A_PROBE_T=5 run; ck "开局第一次拉文件（抢救 维护者 实机局拉 host.log）挂一次 → 重试后照常跑完" '[[ $RES == RESULT=done* ]] && grep -q "开局拉 Documents/host.log 超 5 秒" $OUT/run.log && grep -q "重试一次" $OUT/run.log && ! grep -q "CoreDeviceService" $OUT/run.log'
rm -f $DV/hang_count; FAKE_HANG_FROM=2 S2A_PROBE_T=2 run; ck "连挂两次 → 红「设备通道挂死」" '[[ $RES == RESULT=red*设备通道挂死* ]]' && ck "机上没有系统更新进程 → 不报「疑似系统更新提示」" '[[ $RES != *疑似系统更新提示* ]]'
printf '63 /System/Library/PrivateFrameworks/MobileSoftwareUpdate.framework/Support/softwareupdated\n' > $DV/processes.txt
rm -f $DV/hang_count; FAKE_HANG_FROM=2 S2A_PROBE_T=2 run; ck "只有常驻守护进程 softwareupdated（一直在）→ 不报" '[[ $RES == RESULT=red*设备通道挂死* ]] && [[ $RES != *疑似系统更新提示* ]]'
printf '1102 /Applications/SoftwareUpdateUIService.app/SoftwareUpdateUIService\n' > $DV/processes.txt
rm -f $DV/hang_count; FAKE_HANG_FROM=2 S2A_PROBE_T=2 run; rm -f $DV/processes.txt
ck "连挂两次、机上开着 SoftwareUpdateUIService → 判「设备通道挂死」，不再报「更新提示挡住」（10-20 夜有反例，调度据它停机白丢设备时间）" '[[ $RES == RESULT=red*设备通道挂死* ]] && [[ $RES != *更新提示* ]]'
# 11-04 维护者定（能耗线巫师3 局清残留结束了 维护者 手测机上 20 个进程）：起局前只结束本局包名（MINE）的残留；别款本项目包（PROJ，io.s2arm.*，可能是人正在玩的）、
#   非本项目身份（OTHER）、系统 App 都不碰。真机 url 带 /private、进程表可能不带：本局进程写成不带 /private 的形式，验按安装目录 UUID 段认。
#   反证：10-29 写法（清全部本项目身份）会结束 904
mkdir -p $DV/apps/com.s2arm.test $DV/apps/io.s2arm.proj $DV/apps/com.example.player; echo 'file:///private/var/containers/Bundle/Application/MINE/S2Arm.app/' > $DV/apps/com.s2arm.test/url
echo 'file:///private/var/containers/Bundle/Application/PROJ/S2Arm.app/' > $DV/apps/io.s2arm.proj/url; echo 'file:///private/var/containers/Bundle/Application/OTHER/S2Arm.app/' > $DV/apps/com.example.player/url
printf '901 /var/containers/Bundle/Application/MINE/S2Arm.app/S2Arm\n902 /private/var/containers/Bundle/Application/OTHER/S2Arm.app/S2Arm\n903 /Applications/Weather.app/Weather\n904 /private/var/containers/Bundle/Application/PROJ/S2Arm.app/S2Arm\n' > $DV/processes.txt; rm -f $DV/terminated.log
run; rm -f $DV/processes.txt; rm -rf $DV/apps/io.s2arm.proj $DV/apps/com.example.player; ck "开局清残留：只结束本局包（901）；别款本项目包（904，可能是人正在玩的）、非本项目身份（902）、系统 App（903）不碰" '[[ $RES == RESULT=done* ]] && grep -q "清残留：结束本局包 com.s2arm.test 的残留进程 1 个" $OUT/run.log && grep -qx 901 $DV/terminated.log && ! grep -qx 904 $DV/terminated.log && ! grep -qx 902 $DV/terminated.log && ! grep -qx 903 $DV/terminated.log'
echo 'file:///private/var/containers/Bundle/Application/MINE/S2Arm.app/' > $DV/apps/com.s2arm.test/url   # 本局这个包的两个残留进程（上一局可能重装过，安装目录重写一次）
printf '901 /private/var/containers/Bundle/Application/MINE/S2Arm.app/S2Arm\n902 /private/var/containers/Bundle/Application/MINE/S2Arm.app/PlugIns/X.appex/X\n' > $DV/processes.txt; rm -f $DV/terminated.log $DV/signaled.log
FAKE_TERM_STUCK=901 run; rm -f $DV/processes.txt; ck "terminate 后仍在的残留（901）发 SIGKILL，已结束的（902）不发" '[[ $RES == RESULT=done* ]] && grep -qx "901 SIGKILL" $DV/signaled.log && ! grep -q "^902" $DV/signaled.log && grep -q "terminate 后仍在的 1 个发 SIGKILL" $OUT/run.log'
rm -f $DV/hang_src_count; FAKE_HANG_SRC=Documents/host.log FAKE_HANG_SRC_N=2 S2A_LIVE_PULL_T=2 run;   # 挂第 2 次：第 1 次后窗口才开（采 1 秒），第 2 次必在轮询内（挂第 3 次时窗口可能已收、落到收局拉回，用例偶发不成立）
 ck "起局后轮询拉 host.log 挂一次 → 2 秒超时丢一轮、照常跑完（不拖到截止）" '[[ $RES == RESULT=done* ]] && grep -q "拉文件失败" $OUT/run.log && ! grep -q "测试段" $OUT/run.log'
rm -f $DV/hang_count; FAKE_HANG_TO=1 S2A_CP_T0=2 run; ck "推文件拷完不退出（挂）→ 按大小超时、重试一次后照常跑完" '[[ $RES == RESULT=done* ]] && grep -q "挂：推 .* 超 2 秒（第 1 次）" $OUT/slow_devicectl.log'
}
g_gc() { grp gc; FAKE_SCENE=gc run; ck "MARK 后回收 → 红（分配闸门）" '[[ $RES == RESULT=red*分配闸门* ]]'; }
g_stall() { grp stall; sed_edit 's/^ACT_S=1$/ACT_S=6/' $J/job.env   # 窗口长过停顿判据（S2A_STALL_S=2）
S2A_STALL_S=2 FAKE_SCENE=stall run; ck "出图停住 S2A_STALL_S 秒 → 红（出图停在），结论带宿主最后红行（GPU 卡死）" '[[ $RES == RESULT=red*出图停在*宿主最后红行：GPU\ 卡死* ]]'; }
g_stall2() { grp stall2; S2A_STALL_S=999 FAKE_SCENE=stall MARK_AT=40 run;   # 只验窗口判据：停顿判据设到 999 秒、不可能抢先。原先 5 秒，
  #   负载高时一轮轮询（每轮拉文件起几个假 devicectl）超过 5 秒，同一轮里停顿判据先判（检查顺序在前）→「出图停在…GPU 卡死：假卡死」，10-22 晚挡多线门禁；轮询改 6 秒必现
  ck "窗口短于停顿判据、窗口内 0 帧 → 红" '[[ $RES == RESULT=red*没出新帧* ]]'; }
g_gpuhang() { grp gpuhang   # GPU 卡死即停（11-07 维护者：测试设备2 hang 之后局还跑了几分钟、整机卡死）：假 App 第 41 帧写一行系统原文后照常出帧。
#   窗口 60 秒：不认签名的执行器（负对照 = 改动前）会一直采到窗口满、收成 done；认签名 → 当轮红「GPU 卡死即停」，收局前不截屏
#   期望手算：两种原文（Metal 的 GPU Hang 描述、IOGPU 的 PageFault）都 → red 且原因带原文；没有 screen/end.png；起局到收局远小于 60 秒窗口
sed_edit 's/^ACT_S=1$/ACT_S=60/' $J/job.env; FAKE_SCENE=gpuhang MARK_AT=5 run
ck "GPU Hang → 当轮红「GPU 卡死即停」带原文、收局不截屏、不等满 60 秒窗口（实得：${RES:0:120}）" '[[ $RES == RESULT=red*GPU\ 卡死即停*kIOGPUCommandBufferCallbackErrorHang* ]] && [ ! -e $OUT/screen/end.png ] && ! grep -q "收局：窗口" $OUT/run.log'
FAKE_SCENE=gpuhang MARK_AT=5 FAKE_HANG_LINE='IOGPU: kIOGPUCommandBufferCallbackErrorPageFault (0x0e) at 0x1234' run
ck "PageFault → 同样当轮红（实得：${RES:0:120}）" '[[ $RES == RESULT=red*GPU\ 卡死即停*PageFault* ]]'
# 控制台流里真能看到的行（11-07 测试设备2 金刚现场原文：系统原文只在 stderr 的 [dm] 行，控制台流只有这一行 [s2a]）
FAKE_SCENE=gpuhang MARK_AT=5 FAKE_HANG_LINE='[s2a] 【红】直连异常（不切换，继续跑）：GPU 命令缓冲报错：未能完成操作。（MTL4CommandQueueErrorDomain错误1。）' run
ck "金刚现场控制台行「GPU 命令缓冲报错」→ 当轮红（实得：${RES:0:120}）" '[[ $RES == RESULT=red*GPU\ 卡死即停*GPU\ 命令缓冲报错* ]]'
FAKE_SCENE=gpuhang MARK_AT=5 FAKE_HANG_LINE='[s2a] 【红】GPU 挂死安全停：这局停在这里，不再往驱动提交（fence 4382）' run
ck "安全停行（App 停在错误页不退出）→ 当轮红（实得：${RES:0:120}）" '[[ $RES == RESULT=red*GPU\ 卡死即停*GPU\ 挂死安全停* ]]'
# 日志暴涨即停（11-07 性能线 A：挂死后 host.log 5 分钟涨到 120 MB，整份拉超时、局中永不判）：上限调到 1 KB，假 App 几行心跳就超过 → 不拉、当轮红、不截屏
S2A_LIVE_LOG_MAX_KB=1 MARK_AT=5 run
ck "机上 host.log 超上限 → 当轮红「日志暴涨即停」、不截屏、不等满窗口（实得：${RES:0:120}）" '[[ $RES == RESULT=red*日志暴涨即停* ]] && [ ! -e $OUT/screen/end.png ] && ! grep -q "收局：窗口" $OUT/run.log'; }
g_procs() { grp procs; export S2A_KILL_VERIFY_S=0.2   # 收局核实 + 自动化遗留（11-07 维护者：ab_pre3d2 收局 done 后在测试设备2 上又活了约 20 分钟，金刚局随后同机起，整机卡死）
#   夹具：本包安装路径 MINE（同真机格式）；FAKE_LINGER = 起局后进程表多一个 777（本包路径），被结束才消失。期望手算：
#   ① 777 收局后还在、terminate 能停 → 补停、done、run.log「收局核实…已补停」、terminated.log 有 777；登记撤掉
#   ② 777 terminate / SIGKILL 都停不掉 → red「收局后 App 进程停不掉（pid 777）」；登记留着
#   ③ 下一局起局前：登记里有别款 io.s2arm.proj（进程 904）→ 结束 904 并记名；没登记的别款 io.s2arm.hand（905，人自己打开的）不碰
#   负对照：改动前的执行器 ① ② 都收成 done、777 一次 terminate 都没收到
run   # 先装一次包，之后改安装路径为真机格式
MP=/private/var/containers/Bundle/Application/MINE/S2Arm.app; echo "file://$MP/" > $DV/apps/com.s2arm.test/url; rm -f $DV/terminated.log $DV/signaled.log $DV/killed.txt $DV/linger_on
FAKE_LINGER="777 $MP/S2Arm" run; AL=$(find $G -type d -name autolaunch | head -1)
ck "① 收局后进程还在 → 补停、核实没了、done、撤登记（实得：${RES:0:80}）" '[[ $RES == RESULT=done* ]] && grep -qx 777 $DV/terminated.log && grep -q "收局核实：App 进程 777 收局后还在，已补停" $OUT/run.log && [ -z "$(ls $AL/*/com.s2arm.test 2> /dev/null)" ]'
rm -f $DV/terminated.log $DV/signaled.log $DV/killed.txt $DV/linger_on
echo "file://$MP/" > $DV/apps/com.s2arm.test/url   # 夹具每次重装都会重写安装路径（上一局后可能重装过），这里写回真机格式
FAKE_LINGER="777 $MP/S2Arm" FAKE_TERM_STUCK=777 FAKE_KILL_STUCK=777 run
ck "② 收局后进程停不掉 → red 写明 pid、发过 SIGKILL、登记留着（实得：${RES:0:120}）" '[[ $RES == RESULT=red*收局后\ App\ 进程停不掉（pid\ 777）* ]] && grep -qx "777 SIGKILL" $DV/signaled.log && [ -n "$(ls $AL/*/com.s2arm.test 2> /dev/null)" ]'
rm -f $DV/terminated.log $DV/signaled.log $DV/killed.txt $DV/linger_on $AL/*/com.s2arm.test
mkdir -p $DV/apps/io.s2arm.proj $DV/apps/io.s2arm.hand; echo 'file:///private/var/containers/Bundle/Application/PROJ/S2Arm.app/' > $DV/apps/io.s2arm.proj/url; echo 'file:///private/var/containers/Bundle/Application/HAND/S2Arm.app/' > $DV/apps/io.s2arm.hand/url
printf '904 /private/var/containers/Bundle/Application/PROJ/S2Arm.app/S2Arm\n905 /private/var/containers/Bundle/Application/HAND/S2Arm.app/S2Arm\n' > $DV/processes.txt
for u in $AL/*; do echo "11-07 08:00:00 /x/out_上局" > $u/io.s2arm.proj; done
run; rm -f $DV/processes.txt
ck "③ 起局前结束登记过的别款遗留（904）并记名、没登记的别款（905）不碰、撤登记（实得：${RES:0:80}）" '[[ $RES == RESULT=done* ]] && grep -qx 904 $DV/terminated.log && ! grep -qx 905 $DV/terminated.log && grep -q "清自动化遗留：结束 io.s2arm.proj 的进程 904" $OUT/run.log && [ -z "$(ls $AL/*/io.s2arm.proj 2> /dev/null)" ]'; }
g_crash() { grp crash; FAKE_SCENE=crash MARK_AT=45 run; ck "崩溃 → 红" '[[ $RES == RESULT=red* ]] && grep -q CRASH $OUT/host.log'; }
g_restart() { grp restart   # 进程内重启（11-04 维护者转 Windows 出包线验收②：重启后帧号从 1 重数，执行器拿开窗时的帧号比到底判「窗口内没出新帧」）
  # 手算：MARK 第 60 帧开窗，第 63 帧重启归零；采 2 秒（30 帧/秒）收局时机上帧号约 50–60 < 开窗帧号 → 旧判据红。分段累计：重启前末个心跳 60 + 重启后帧号 > 60 → 绿
  echo ACT_S=2 >> $J/job.env; FAKE_SCENE=restart MARK_AT=60 run
  ck "MARK 后进程内重启、帧号归零 → done（按重启分段累计帧号，不判「窗口内没出新帧」）" '[[ $RES == RESULT=done* ]] && grep -q "收局：窗口" $OUT/run.log && ! grep -q "没出新帧" $OUT/run.log'
  [ -z "${S2A_PRIVATE_TOOLS:-}" ] || ck "重启局进统一账本：窗口列「重启后段」（MARK 在重启前，读数取重启后那段）" 'LC_ALL=C awk -F"\t" -v o=$OUT "\$NF==o && \$7==\"重启后段\"{f=1} END{exit !f}" $S2A_DEVICE_LEDGER'; }
g_nomark() { grp nomark; sed_edit 's/^ARRIVE_S=30$/ARRIVE_S=3/' $J/job.env; FAKE_SCENE=nomark run; ck "没 MARK → 红（没到达代表场景）" '[[ $RES == RESULT=red*没到达* ]]'
[ -z "${S2A_PRIVATE_TOOLS:-}" ] || ck "没 MARK 的红局也进统一账本：本局目录一行、结果 red、窗口「整局」（tools/device_ledger.py）" 'LC_ALL=C awk -F"\t" -v o=$OUT "\$NF==o && \$6==\"red\" && \$7==\"整局\"{f=1} END{exit !f}" $S2A_DEVICE_LEDGER'; }
g_guard() { grp guard   # 起局前的拦截：都不该起局
mkdir -p $DV; : >> $DV/launches.log; n0=$(wc -l < $DV/launches.log)
mkipa $T/wrong.ipa com.other; mv $J/app.ipa $T/right.ipa; cp $T/wrong.ipa $J/app.ipa
run; ck "包名不符 → 红 bundle 且不起局" '[[ $RES == RESULT=red*bundle* ]] && [ "$(wc -l < $DV/launches.log)" -eq $n0 ]'
cp $T/right.ipa $J/app.ipa
FAKE_HW_UDID=00000000-0000000000000000 run; ck "devicectl 报的硬件 UDID 不符 → 红 whitelist 且不起局" '[[ $RES == RESULT=red*whitelist* ]] && [ "$(wc -l < $DV/launches.log)" -eq $n0 ]'
sed_edit 's/^DEV=1/DEV=00000000-0000000000000000/' $J/job.env
run; ck "job 指定日常手机 → 设备不可用收 skip（未跑，不是游戏的红）、不取锁" '[[ $RES == RESULT=skip* ]] && ! grep -q 日常手机 $G/h.tsv 2>/dev/null && [ "$(wc -l < $DV/launches.log)" -eq $n0 ]'
}
g_apq() { grp apq   # 自动玩家自己收局（没 MARK、停帧）→ 报「没到 MARK」不报「出图停在」；心跳报前台失焦 → 一律 suspect（可疑），按截图文字分类写该点的按钮
printf '#!/bin/sh\necho "软件更新"; echo "“iOS 27.0.1”已经可以安装。"; echo "现在安装"; echo "稍后"\n' > $G/ocr_upd.sh; printf '#!/bin/sh\necho "允许“X”使用无线数据？"; echo "不允许"\n' > $G/ocr_wl.sh; chmod +x $G/ocr_*.sh
S2A_OCR=$G/ocr_upd.sh FAKE_SCENE=apquit MARK_AT=999 FAKE_UNFOCUS=1 run; ck "自动玩家优雅退出 + 前台失焦（软件更新框）→ suspect「可疑：系统弹框致前台失焦」，带原判红「没到 MARK」、类别软件更新、点「稍后」，局中截了 unfocus.png" '[[ $RES == RESULT=suspect*可疑：系统弹框致前台失焦*类别：软件更新*点「稍后」*执行器原判红：没到\ MARK* ]] && [ -s $OUT/screen/unfocus.png ] && grep -q "^软件更新" $OUT/sysdialog.tsv'
S2A_OCR=$G/ocr_wl.sh FAKE_UNFOCUS=1 run; ck "好局但前台失焦（无线数据框）→ suspect（不给绿）、选「仅限无线局域网」带包名、不提「软件更新」" '[[ $RES == RESULT=suspect*类别：无线数据*选「仅限无线局域网」*包名\ com.s2arm.test* ]] && [[ $RES != *软件更新* ]] && [[ $RES != *执行器原判红* ]]'
FAKE_UNFOCUS=1 FAKE_NO_SHOT=1 run; ck "失焦但截屏失败 → 仍 suspect、类别未识别、提示到测试设备上看屏幕" '[[ $RES == RESULT=suspect*类别：未识别*到测试设备上看屏幕* ]]'
run; ck "没有失焦 → 照旧 done、不截 unfocus.png" '[[ $RES == RESULT=done* ]] && [ ! -e $OUT/screen/unfocus.png ]'; }
g_trunc() { grp trunc   # 旧 App 撞截短包只删残包、后面待解包留着：.pushed 记着残包里的文件（没落地）→ 执行器不信 .pushed，按已应用索引补推
run; ck "截短组首局 → done" '[[ $RES == RESULT=done* ]]'
P=$DV/apps/com.s2arm.test/Documents/资源包; grep -v romfs/f1.bin $P/index.tsv > $P/index.applied.tsv; rm -f $P/romfs/f1.bin
mkdir -p $P/.incoming; (cd $J/pack && COPYFILE_DISABLE=1 tar -cf $P/.incoming/1_002.tar romfs/f3.bin); grep romfs/f1.bin $J/pack/index.tsv | cut -f1,3 > $P/.pushed
echo 上次没解完 > $P/.applied; echo "10-21 00:56:58 解包失败：解 1_001.tar（截短包，已删，下次部署补推） 返回 -4（已解 0 个包 2 个文件）" >> $P/../unpack.log
run; ck "上次解包撞截短包 → 不信 .pushed：补推残包里的 f1（推 1 个）、解包后机上有 f1" '[[ $RES == RESULT=done* ]] && grep -q "资源包：推 1 个文件" $OUT/run.log && grep -q "不信 .pushed" $OUT/run.log && [ "$(cat $P/romfs/f1.bin 2>/dev/null)" = "file 1" ]'
}
g_stalepush() { grp stalepush   # 10-26 运维线（测试设备2 巫师3）：重启打断推送，.incoming 留下截短批、.pushed 记着那批的文件 → 下一局先盖掉残批、不信 .pushed、补推缺的，App 不撞截短
run; ck "残批组首局 → done" '[[ $RES == RESULT=done* ]]'
P=$DV/apps/com.s2arm.test/Documents/资源包; grep -v romfs/f2.bin $P/index.tsv > $P/index.applied.tsv; rm -f $P/romfs/f2.bin
mkdir -p $P/.incoming; (cd $J/pack && COPYFILE_DISABLE=1 tar -cf $T/full.tar romfs/f2.bin) && head -c 700 $T/full.tar > $P/.incoming/1792980305_002.tar; grep romfs/f2.bin $J/pack/index.tsv | cut -f1,3 > $P/.pushed
run; ck "机上残留截短批 → 盖成空包、不信 .pushed、补推 f2（推 1 个）、局 done、机上 f2 在" '[[ $RES == RESULT=done* ]] && grep -q "已用空包盖掉" $OUT/run.log && grep -q "资源包：推 1 个文件" $OUT/run.log && [ "$(cat $P/romfs/f2.bin 2>/dev/null)" = "file 2" ] && ! grep -q "截短" $P/../unpack.log'
}
g_livef() { grp livef   # 局中不拉 frames.tsv（S2A_LIVE_FRAMES=0）：帧号走 host.log 心跳；frames.tsv 收局后照样拉回；停帧照样判
S2A_LIVE_FRAMES=0 run; ck "局中不拉 frames.tsv → done，局中没有 live/f.tsv，收局拉回的 frames.tsv 有内容" '[[ $RES == RESULT=done* ]] && [ ! -e $OUT/live/f.tsv ] && [ -s $OUT/frames.tsv ] && grep -q "窗口起点" $OUT/run.log'
sed_edit 's/^ACT_S=1$/ACT_S=8/' $J/job.env; S2A_LIVE_FRAMES=0 S2A_HB_STALL_MIN=2 S2A_STALL_S=2 FAKE_SCENE=stall MARK_AT=5 run; ck "局中不拉 frames.tsv 时心跳停了 → 照样红「出图停在」" '[[ $RES == RESULT=red*出图停在* ]]'
# GPU 慢段长短由假 App 按执行器的拉取事件定（tests/fake_app.py gpuslow：停到执行器必判过一轮停顿 + 看到 GPU 慢再恢复），不按固定秒数竞速
#   （10-23 负载 13–17 时固定 12 秒偶发红：一轮轮询拖长，慢段落在两轮之间，走不到「再等」）；窗口 20 秒
sed_edit 's/^ACT_S=8$/ACT_S=20/' $J/job.env; S2A_STALL_S=2 FAKE_SCENE=gpuslow MARK_AT=5 run; ck "加载帧 GPU 慢（宿主报「GPU 慢…继续等」）→ 不判停帧、照常收局" '[[ $RES == RESULT=done* ]] && grep -q "GPU 慢…继续等」：再等" $OUT/run.log'; }
g_lag() { grp lag   # 10-27 R13 坑 2：执行器拿到的 frames.tsv 滞后（拉回只到第 40 帧），设备心跳继续涨（每 10 帧一行）→ 不许拿滞后副本判「出图停在」
# 窗口 12 秒、停顿判据 8 秒：门禁并行时假 App 会被饿住 2–3 秒不出帧（见文件头 10-22 的记录），判据 2 秒会把「心跳在涨」那两条误判成停帧
# 期望红的 ①③ 窗口给 120 秒：红是「停顿满 8 秒」这个事件触发的，到了就收局，窗口只在生产判不出红时兜底（10-31 gate-host 全套并发：
#   窗口 12 秒时「第 40 帧被看到」晚过窗口起点 4 秒，8 秒停顿攒不满窗口就到点收成 done，假红）；
#   FAKE_HB_EVERY 极大 = 整局不写心跳行（原 999 在第 999 帧会冒一行），120 秒窗口里也不会冒出「心跳在涨」；红要停在副本末帧第 40 帧，不是别处饿住
lag_win() { sed_edit "s/^ACT_S=.*/ACT_S=$1/" $J/job.env; }
# ① 反证：帧号只认滞后的 frames 副本（心跳几乎不写 + 关掉判红前重读）→ 红，与 R13 现场同一形态
lag_win 120; FAKE_LAG_FRAMES=40 FAKE_HB_EVERY=999999 S2A_FRAME_RECHECK=0 S2A_STALL_S=8 run
ck "反证：只认滞后的 frames 副本 → 红「出图停在第 40 帧」（实得：${RES:0:90}）" '[[ $RES == RESULT=red*出图停在第\ 40\ 帧* ]]'
# ② 帧号取 frames 行数与 host.log 心跳里大的那个：副本停在 40 帧、心跳在涨 → 不判停、照常收局
lag_win 12; FAKE_LAG_FRAMES=40 FAKE_HB_EVERY=10 S2A_STALL_S=8 run
WN=$(grep -oE '收局：窗口 [0-9]+ 秒、[0-9]+ 帧' $OUT/run.log | tail -1 | grep -oE '、[0-9]+ 帧' | grep -oE '[0-9]+')
ck "帧号按心跳走：副本停 40 帧、心跳在涨 → 不判「出图停在」、照常 done（窗口读数 ${WN:-?} 帧）" '[[ $RES == RESULT=done* ]] && ! grep -q "出图停在" $OUT/run.log && [ "${WN:-0}" -gt 100 ]'
# ③ 判红前重读不许凭空造帧：副本与心跳都静止 → 仍判红（重读只是重读，不是免死金牌）
lag_win 120; FAKE_LAG_FRAMES=40 FAKE_HB_EVERY=999999 S2A_STALL_S=8 run
ck "重读不凭空造帧：副本与心跳都静止 → 仍判红「出图停在第 40 帧」" '[[ $RES == RESULT=red*出图停在第\ 40\ 帧* ]]'
# ④ 对照（维护方 A3 第 7 条）：只关重读、心跳照常 → 帧号取心跳照样 done，判据不是只靠重读撑着
lag_win 12; FAKE_LAG_FRAMES=40 FAKE_HB_EVERY=10 S2A_FRAME_RECHECK=0 S2A_STALL_S=8 run
ck "对照：只关 reframe（S2A_FRAME_RECHECK=0）、心跳在涨 → 照样 done、不判「出图停在」" '[[ $RES == RESULT=done* ]] && ! grep -q "出图停在" $OUT/run.log'
}
g_stale() { grp stale   # 拉不到 / 拉回不新鲜 ≠ 停帧（11-06 性能线 A：Duo 上 live 副本 30 秒没更新，把一直在出帧的局判「出图停」收掉）
# 停顿判据 4 秒；第 6 秒起 10 秒拉回的都是旧快照（> 判据、< 3 倍判据 12 秒），设备一直在出帧 → 不判停、照常 done，记「拉取不新鲜」
sed_edit 's/^ACT_S=.*/ACT_S=20/' $J/job.env
FAKE_STALE_FROM_S=6 FAKE_STALE_S=10 S2A_STALL_S=4 run
ck "拉回 10 秒不更新（旧快照、rc=0）、设备在出帧 → 不判「出图停在」、照常 done、记「拉取不新鲜」（实得：${RES:0:60}）" '[[ $RES == RESULT=done* ]] && ! grep -q "【红】出错即停：出图停在" $OUT/run.log && grep -q "拉取不新鲜" $OUT/run.log'
rm -f $DV/stale_*; FAKE_STALE_MODE=fail FAKE_STALE_FROM_S=6 FAKE_STALE_S=10 S2A_STALL_S=4 run
ck "拉文件连续失败 10 秒 → 不判停、照常 done、失败单独计数（实得：${RES:0:60}）" '[[ $RES == RESULT=done* ]] && ! grep -q "【红】出错即停：出图停在" $OUT/run.log && grep -q "拉文件失败.*本局第 [0-9]* 次" $OUT/run.log'
# 真停帧（心跳照打、帧不涨）照样判红：同 g_stall，确认新鲜读数门槛没把真停帧放过去
rm -f $DV/stale_*; sed_edit 's/^ACT_S=.*/ACT_S=6/' $J/job.env; S2A_STALL_S=2 FAKE_SCENE=stall run
ck "真停帧（心跳在长、帧停 40）→ 仍判红「出图停在第 40 帧」、不带「没有新心跳」" '[[ $RES == RESULT=red*出图停在第\ 40\ 帧* ]] && [[ $RES != *没有新心跳* ]]'
# 连心跳都停（进程整个卡死，或拉取一直不更新）：满 3 倍判据仍判红，原因写明 host.log 没增长
# 心跳每 7 帧一行（FAKE_HB_EVERY=7）：最后一行落在第 35 帧，不和停住的第 40 帧同时出——缺省每 10 帧一行时第 40 帧那行与停帧同一刻写出，
#   执行器先拉 host.log 后拉帧号，两次拉取一错开就把它当「停帧之后来过新心跳」立刻判真停帧（11-10 预跑与 main 同红），等不到 3 倍判据
sed_edit 's/^ACT_S=.*/ACT_S=20/' $J/job.env; FAKE_HB_EVERY=7 FAKE_STALL_NO_HB=1 S2A_STALL_S=2 FAKE_SCENE=stall run   # 窗口 20 秒长过 3 倍判据 6 秒
ck "心跳也停 → 满 3 倍判据判红、原因写明「host.log 也 N 秒没有新心跳」" '[[ $RES == RESULT=red*出图停在第\ 40\ 帧*host.log\ 也*没有新心跳* ]]'
# Duo 现场形态（11-06 两局）：心跳迟到但别的行照常到（host.log 在长）→ 不许按「host.log 变长」当新鲜读数判停
sed_edit 's/^ACT_S=.*/ACT_S=20/' $J/job.env; FAKE_HB_EVERY=7 FAKE_STALL_NO_HB=1 FAKE_STALL_CHATTER=1 S2A_STALL_S=2 FAKE_SCENE=stall FAKE_STALL_RESUME_S=4 run
ck "心跳迟到 4 秒（期间别的行照常写）、之后恢复出帧 → 不判停、照常 done（实得：${RES:0:60}）" '[[ $RES == RESULT=done* ]] && ! grep -q "【红】出错即停：出图停在" $OUT/run.log'; }
g_recheck() { grp recheck   # 判红前重读的截止保护（维护方 A3 第 6 条）：拉取超时按测试段剩余秒数截短，剩余 ≤ 0 不重读
eval "$(sed -n '/^frame_now() {/,/^}/p' $H/tools/dev_run.sh)"
eval "$(sed -n '/^reframe() {/,/^}/p' $H/tools/dev_run.sh)"
eval "$(sed -n '/^freshmark() {/p' $H/tools/dev_run.sh)"; HB0=0; HG_T=0   # reframe 拉到新副本后调它记心跳（原先没摘，报 freshmark: command not found）
O2=$T/recheck; mkdir -p $O2/live; printf 'frame\n1\n2\n3\n' > $O2/live/f.tsv; O=$O2
LIVEF=1; n=0; n0=0; stl=0; tch=$(date +%s); S2A_LIVE_PULL_T=30; lp=
livepull() { lp="$lp ${CPF_T:-?}"; }
SEC=100; T0=$(date +%s)
reframe; ck "剩余 100 秒、S2A_LIVE_PULL_T=30 → 重读按 30 秒（实得 ${lp# }），帧号 3" '[[ "${lp# }" = 30 ]] && [ "$n" = 3 ]'
lp=; T0=$(( $(date +%s) - 97 ))   # 剩余 ~3 秒
reframe; ck "剩余不足 S2A_LIVE_PULL_T → 超时截短到剩余秒数（实得 ${lp# }）" '[ -n "${lp# }" ] && [ "${lp# }" -ge 1 ] && [ "${lp# }" -le 4 ]'
lp=; rc=0; T0=$(( $(date +%s) - 101 )); reframe || rc=$?
ck "剩余 ≤ 0 → 不重读（返回 1）、一次都没拉（实得 rc=${rc}、拉取「${lp}」）" '[ "$rc" = 1 ] && [ -z "$lp" ]'
}
g_con() { grp con   # 局中日志走控制台流（S2A_CONSOLE=1 / 二进制带 S2A_LOG_CONSOLE 自动开）：局中不拉文件；文件服务起局后挂死照样判 MARK / 出帧 / 收局
S2A_CONSOLE=1 run; ck "控制台流局 → done；起局带 S2A_LOG_CONSOLE=1、流里有 MARK、收局拉回 host.log 与流对得上" '[[ $RES == RESULT=done* ]] && tail -1 $DV/launches.log | grep -q "\"S2A_LOG_CONSOLE\": \"1\"" && grep -q "S2A 标记 MARK" $OUT/live/host.log && grep -q "窗口起点" $OUT/run.log && [ "$(grep -c "心跳" $OUT/live/host.log)" -ge 1 ] && ! grep -q "$(printf "\r")" $OUT/live/host.log && ! grep -q "Launched application" $OUT/live/host.log'
ck "控制台流里没有 C# 侧 [gal] 行、收局拉回的 host.log 里有（以拉回为准）" '! grep -q "^\[gal\]" $OUT/live/host.log && grep -q "^\[gal\] 帧节拍" $OUT/host.log'
S2A_CONSOLE=1 ENVJSON='"S2A_LOG_CONSOLE":"0"' run; ck "局环境显式 S2A_LOG_CONSOLE=0 → 不开控制台流（不带 --console、起局环境保持 0）、照常 done" '[[ $RES == RESULT=done* ]] && tail -1 $DV/launches.log | grep -q "\"S2A_LOG_CONSOLE\": \"0\"" && ! tail -1 $DV/launches.log | grep -q -- "--console"' 
rm -f $DV/launched; FAKE_HANG_AFTER_LAUNCH=1 S2A_CONSOLE=1 S2A_END_PULL_T=2 run; ck "起局后文件服务挂死 → 照样 MARK、出帧、done；host.log 由控制台流顶上；第一次超时后余下跳过、记待拉" '[[ $RES == RESULT=done* ]] && grep -q "窗口起点" $OUT/run.log && grep -q "用控制台流顶上" $OUT/run.log && grep -q "Documents/host.log" $OUT/pending_pull.txt && grep -q "Documents/frames.tsv" $OUT/pending_pull.txt && [ "$(grep -c "超 2 秒（文件服务挂住）" $OUT/run.log)" = 1 ]'
ck "局中一次文件都不拉：起局后只有收局那 1 次拉（挂住后余下跳过）" '[ "$(sed -n "/device process launch/=" $DV/calls.log | tail -1 | xargs -I{} sed -n "{},\$p" $DV/calls.log | grep -c "device copy from")" = 1 ]'
rm -f $DV/launched; S2A_CONSOLE=1 FAKE_SCENE=crash MARK_AT=100 run; ck "控制台流局 App 崩溃退出 → 红「App 提前退出」带退出码 1" '[[ $RES == RESULT=red*App\ 提前退出*退出码\ 1* ]]'
w=$T/ipa_con; mkdir -p $w/Payload/X.app; plutil -create xml1 $w/Payload/X.app/Info.plist; plutil -insert CFBundleIdentifier -string com.s2arm.test $w/Payload/X.app/Info.plist; printf 'xx S2A_LOG_CONSOLE yy' > $w/Payload/X.app/S2Arm
rm -f $J/app.ipa; (cd $w && zip -qr $J/app.ipa Payload); run; ck "缺省 auto：App 二进制带 S2A_LOG_CONSOLE → 自动走控制台流" '[[ $RES == RESULT=done* ]] && [ -s $OUT/live/con.raw ] && grep -q "控制台流" $OUT/run.log'; }
g_batt() { grp batt   # 电量闸读 App 心跳区的「[s2a] [电] 电量 N%，状态」（日志行带 [s2a] 前缀；10-21 前按行首匹配、一次没读到，局中电量闸形同虚设）
FAKE_BATT='25%，未充电' run; ck "App 报电量 25% 未充电 → skip（未跑，不是游戏的红）「低于 30%」、落低电量标记" '[[ $RES == RESULT=skip*电量\ 25%* ]] && [ -f $G/lock/battery/device_low_battery ]'
rm -f $G/lock/battery/device_low_battery; FAKE_BATT='55%，未充电' run; ck "开局电量 55% 未充电 → skip（未跑），理由「开局电量 55% 低于 60%」（10-23 原先收 red、被记分板打成能玩红）" '[[ $RES == RESULT=skip*开局电量\ 55%\ 低于\ 60%* ]]'
rm -f $G/lock/battery/device_low_battery; FAKE_BATT='25%，充电中' run; ck "电量 25% 但在充电 → 照常 done" '[[ $RES == RESULT=done* ]]'; }
g_screen() { grp screen   # 整屏截图：正常局 MARK 时与收局前各一张 2868×1320；截屏失败不影响结论；开局挂死也截一张、结论写明
pngwh() { python3 -c "import struct,sys;b=open(sys.argv[1],'rb').read(24);print('%dx%d' % struct.unpack('>II', b[16:24]))" "$1" 2>/dev/null; }
S2A_SHOT_MIN_S=0 run; ck "正常局：screen/ 下 mark.png 与 end.png、都是 2868×1320" '[[ $RES == RESULT=done* ]] && [ "$(pngwh $OUT/screen/mark.png)" = 2868x1320 ] && [ "$(pngwh $OUT/screen/end.png)" = 2868x1320 ]'
FAKE_DUAL_DISPLAY=1 S2A_SHOT_MIN_S=0 run; ck "Duo 双屏（primary 背光关）→ 截亮着的那块：mark.png 与 end.png 都在（11-07 Duo 截屏全黑）" '[[ $RES == RESULT=done* ]] && [ -s $OUT/screen/mark.png ] && [ -s $OUT/screen/end.png ]'
FAKE_NO_SHOT=1 run; ck "截屏失败 → 局照样 done、记一行「截屏…失败」" '[[ $RES == RESULT=done* ]] && grep -q "截屏 mark 失败" $OUT/run.log'
rm -f $DV/hang_count; FAKE_HANG_FROM=2 S2A_PROBE_T=2 run; ck "开局挂死 → 截一张 hang.png、结论写「屏幕截图见 screen/hang.png」" '[[ $RES == RESULT=red*设备通道挂死*screen/hang.png* ]] && [ -s $OUT/screen/hang.png ]'; }
g_shc() { grp shc   # 着色器缓存拉回（画面·着色器线 C①）：收局把机上 Documents/S2A/ryujinx/games/<程序号>/cache/shader/ 四个文件拉回 shcache_rt/；拉不到不判红
run; ck "包里没有 shaders/shcache/<程序号>/ → 跳过一行、不建 shcache_rt/" '[[ $RES == RESULT=done* ]] && grep -q "着色器缓存拉回跳过：包里没有" $OUT/run.log && [ ! -e $OUT/shcache_rt ]'
mkdir -p $J/pack/shaders/shcache/0100abcd00000000; echo pk > $J/pack/shaders/shcache/0100abcd00000000/shared.toc; mkpack $J/pack
C=$DV/apps/com.s2arm.test/Documents/S2A/ryujinx/games/0100abcd00000000/cache/shader; mkdir -p $C; for f in guest.toc guest.data shared.toc shared.data; do echo "rt $f" > $C/$f; done
run; ck "四个文件都在 → 拉回 shcache_rt/ 4 个、内容与机上一致、包缓存克隆进 pkg_shcache/（收集器核包用，不依赖包快照存活）、局 done" '[[ $RES == RESULT=done* ]] && ( for f in guest.toc guest.data shared.toc shared.data; do [ "$(cat $OUT/shcache_rt/$f 2> /dev/null)" = "rt $f" ] || exit 1; done ) && grep -q "着色器缓存：拉回 shcache_rt/ 4 个文件" $OUT/run.log && [ "$(cat $OUT/pkg_shcache/shared.toc 2> /dev/null)" = pk ]'
rm -f $C/guest.data; run; ck "机上缺 guest.data → 拉回 3 个、记「不全 3/4」、不克隆包缓存、局照样 done（不判红）" '[[ $RES == RESULT=done* ]] && [ ! -e $OUT/shcache_rt/guest.data ] && grep -q "着色器缓存拉回不全：3/4" $OUT/run.log && [ ! -e $OUT/pkg_shcache ]'
FAKE_SCENE=gc run; ck "红局（分配闸门）也拉" '[[ $RES == RESULT=red* ]] && [ -s $OUT/shcache_rt/shared.toc ]'; }
g_apcrash() { grp apcrash   # 收推送的 App 起来即崩、没走到解包（10-22 链 SDK 27 没接 UIScene 的包，原先白等到部署预算用完报「收推送超时」）→ 当场红；解得慢但活着的不误判
run; P=$DV/apps/com.s2arm.test/Documents/资源包; ck "首局 → done" '[[ $RES == RESULT=done* ]]'
echo 旧包 > $P/.applied; rm -f $DV/launched; t0=$(date +%s); FAKE_APPLY=crash run; ck "收推送 App 起来即崩 → 红 push「App 没走到解包」带 pid、30 秒内收局、没起正式局" '[[ $RES == RESULT=push*App\ 没走到解包* ]] && grep -q "App 没走到解包：收推送的 App（pid [0-9]*）" $OUT/run.log && [ $(( $(date +%s) - t0 )) -lt 30 ] && [ ! -e $DV/launched ]'
echo 旧包 > $P/.applied; FAKE_APPLY=slow run; ck "收推送 App 解得慢（8 秒）但进程活着 → 照常解完、done" '[[ $RES == RESULT=done* ]] && grep -q "机上解开推送包" $OUT/run.log'
# 11-02 测试设备2 暗黑2：机上 .applied 推之前就等于本包清单（新旧包索引相同）、又推了包；App 先写进度行、3 秒后才解完写完成行 →
#   只认本次的完成行「解开 N 个包 … .applied = 本包」，期望照常解完 done、不报「收推送包数不符」（旧判法见到第一行进度就数批数判红）
W=$(python3 -c "import json;print(json.load(open('$J/pack/manifest.json'))['index_sha256'])"); echo "$W" > $P/.applied; S2A_PUSH_FULL=1 FAKE_APPLY=progress run
ck ".applied 推前已对上 + 解包先写进度行 → 等到本次完成行才算解完、done，不判包数不符" '[[ $RES == RESULT=done* ]] && grep -q "机上解开推送包" $OUT/run.log && ! grep -q "收推送包数不符" $OUT/run.log'
# 11-05 能耗线：收推送的 App 写完 .applied 还活着（收尾冲刷、优雅退出）→ 等它退出再盖空 / 起局，拉回的 host.log 是真局的（原先只剩收推送进程收尾那一行）
echo 旧包 > $P/.applied; FAKE_APPLY=linger run; ck "收推送 App 写完 .applied 又活 3 秒才重开 host.log → 等它退出（记一行）、拉回的 host.log 是真局的（不只剩收尾行）、done" '[[ $RES == RESULT=done* ]] && grep -q "收推送的 App（pid [0-9]*）已退出" $OUT/run.log && [ "$(grep -c . $OUT/host.log)" -gt 1 ] && ! grep -q 收推送进程收尾 $OUT/host.log'
echo 旧包 > $P/.applied; S2A_APPLY_EXIT_S=3 FAKE_APPLY=hang run; ck "收推送 App 一直不退 → 到点结束它（记一行）、照常起局 done" '[[ $RES == RESULT=done* ]] && grep -q "秒没自己退出，已结束" $OUT/run.log'; }
g_smoke() { grp smoke   # 登记冒烟（SMOKE=1）：部署后起 App 看 S2A_SMOKE_S 秒进程在不在；不带自动玩家环境、不跑局
echo SMOKE=1 >> $J/job.env; S2A_SMOKE_S=2 run; ck "App 一直活着 → RESULT=smoke-ok、起包不带环境（假 devicectl 照真机拒收空对象）、已结束 App" '[[ $RES == RESULT=smoke-ok* ]] && [ "$(tail -1 $DV/launches.log)" = "{}" ] && [ -s $DV/terminated.log ] && grep -q "冒烟：App 起来 2 秒进程一直在" $OUT/run.log'
ck "部署前机上没有这个身份 → 冒烟收尾卸掉（11-06 冒烟单把按 维护者 令卸掉的款装回测试设备）" '[ ! -d $DV/apps/com.s2arm.test ] && grep -q "冒烟：部署前机上没有 com.s2arm.test，已卸载" $OUT/run.log'
FAKE_SCENE=crash MARK_AT=999 S2A_SMOKE_S=5 run; ck "App 起来 2 秒内崩（第 50 帧退出）→ RESULT=smoke「App 起来 N 秒退出」，并已把这次的 host.log 拉回、结论带要点行（10-23 xc3 冒烟红没留证据）" '[[ $RES == RESULT=smoke\ *App\ 起来*秒退出*host.log\ 末要：* ]] && [ -s $OUT/host.log ]'
ck "崩了也卸（部署前没装）" '[ ! -d $DV/apps/com.s2arm.test ] && grep -q "已卸载" $OUT/run.log'
mkdir -p $DV/apps/com.s2arm.test; echo file:///pre/ > $DV/apps/com.s2arm.test/url; S2A_SMOKE_S=2 run
ck "部署前机上已装 → 冒烟后照样留着、不卸" '[[ $RES == RESULT=smoke-ok* ]] && [ -d $DV/apps/com.s2arm.test ] && ! grep -q "已卸载" $OUT/run.log'
rm -rf $DV/apps/com.s2arm.test; FAKE_APPS_GARBAGE=1 S2A_SMOKE_S=2 run
ck "读不清机上有没有（列表不是 JSON）→ 按装着处理、不卸（rc：${RES%% *}）" '[ -d $DV/apps/com.s2arm.test ] && ! grep -q "已卸载" $OUT/run.log'; }
g_ownplay() { export S2A_OWNPLAY_DIR=$T/ownplay_g; grp ownplay   # 两局测试之间有人自己玩过（机上 host.log 不是上一局测试收局时那份，按内容哈希认）→ 起局前存档 + 扫首遇现编，> 0 记记分板读数与着色器线收件箱；没人玩过不动
run; ck "首局（本机还没有首行记录）→ done、不存档" '[[ $RES == RESULT=done* ]] && [ ! -d $S2A_OWNPLAY_DIR ]'
run; ck "两局之间没人玩过（机上是上一局测试的 host.log）→ 不存档" '[[ $RES == RESULT=done* ]] && [ ! -d $S2A_OWNPLAY_DIR ]'
printf '[s2a] 维护者 自己起的局 09:58\n[gal] 管线现建：顶点 sA 片元 sB（3.8 ms，第 1 条，异步）\n[gal] 【首遇】程序 34 编译中，跳过本次绘制（第 1 次）\n' > $DV/apps/com.s2arm.test/Documents/host.log
mkdir -p $DV/apps/com.s2arm.test/Documents/logs; printf '[s2a] 维护者 更早一次 09:40\n' > $DV/apps/com.s2arm.test/Documents/logs/host-20261106-094000.log
rm -f $S2A_BOARD $S2A_SCHED_DIR/inbox/画面着色器.log; run; d=$(ls -d $S2A_OWNPLAY_DIR/*/ 2>/dev/null | head -1)
ck "中间有人玩过 → 局照常 done；日志存进 ownplay、扫出首遇现编 2；记分板加「维护者 实机局首遇现编 2 次」、着色器线收件箱一行" '[[ $RES == RESULT=done* ]] && [ -n "$d" ] && grep -q "维护者 自己起的局" $d/host.log && grep -q "现建管线 1、程序编译中跳过绘制 1" $d/firstuse.txt && grep -q "维护者 实机局首遇现编 2 次" $S2A_BOARD && grep -q "维护者 实机局首遇现编 2 次" $S2A_SCHED_DIR/inbox/画面着色器.log && grep -q "两局测试之间机上有人自己玩过" $OUT/run.log'
ck "机上往局日志 logs/host-<起局时刻>.log（不是测试局的）也存进同一目录" 'grep -q "维护者 更早一次" $d/logs/host-20261106-094000.log'
rm -rf $DV/apps/com.s2arm.test/Documents/logs
n=$(ls -d $S2A_OWNPLAY_DIR/*/ | wc -l); run; ck "存过之后下一局（机上又是测试的 host.log）→ 不再存" '[[ $RES == RESULT=done* ]] && [ "$(ls -d $S2A_OWNPLAY_DIR/*/ | wc -l)" = "$n" ]'
# 11-02：维护者 手玩那局第一行和测试局一模一样（每次开机首行都是同一行），只是后面多了内容；手玩时 env.txt 开了逐帧，frames.tsv 也是这局的
#   手算期望：按内容哈希认 → 存一个新目录，host.log 带手玩那行、frames.tsv 也存下
H0=$DV/apps/com.s2arm.test/Documents; { cat $H0/host.log; echo '[s2a] 维护者 手玩 v3 进局'; } > $T/hl && mv $T/hl $H0/host.log; printf 'frame\twall_us\n1\t16000\n2\t32700\n' > $H0/frames.tsv
n=$(ls -d $S2A_OWNPLAY_DIR/*/ | wc -l); run; d2=$(printf '%s\n' "$LOG" | sed -n 's/.*存 \([^；]*\)；首遇.*/\1/p' | tail -1)   # 存到哪个目录以本局日志为准（同一分钟建的目录按修改时间排不可靠）
ck "首行相同、内容不同（手玩局）→ 认出、存新目录，host.log 带手玩那行、frames.tsv 一并存" '[[ $RES == RESULT=done* ]] && [ "$(ls -d $S2A_OWNPLAY_DIR/*/ | wc -l)" -eq $((n + 1)) ] && grep -q "维护者 手玩 v3 进局" $d2/host.log && grep -q "32700" $d2/frames.tsv'
n=$(ls -d $S2A_OWNPLAY_DIR/*/ | wc -l); DEPLOY_ONLY=1 run; ck "只部署：机上还是测试局那份 → 不存（只部署也走抢救，不误报）" '[ "$(ls -d $S2A_OWNPLAY_DIR/*/ | wc -l)" -eq "$n" ]'; }
g_alt() { grp alt   # 同局交替 S2A_ALT（runtime/src/alt.c）：轮询 0.2 秒 → 护边前 1 秒、后 1+2 = 3 秒；段长 6 秒 → 第 k 段采样点 a = 6k+1、b = 6k+3（距 MARK 秒）
mkdir -p $G/bin; printf '#!/bin/sh\nd=$(date +%%s); echo $d >> %s/pwcalls.log\nprintf "<?xml version=\\"1.0\\"?><plist version=\\"1.0\\"><dict><key>IORegistry</key><dict><key>PowerTelemetryData</key><dict><key>AccumulatedSystemLoad</key><integer>%%s</integer><key>SystemLoadAccumulatorCount</key><integer>%%s</integer></dict></dict></dict></plist>\\n" $((d*4000)) $d\n' $G > $G/bin/idevicediagnostics; chmod +x $G/bin/idevicediagnostics
echo ACT_S=14 >> $J/job.env
PATH=$G/bin:$PATH S2A_ALT_MIN_S=6 ENVJSON='"S2A_ALT":"6"' run; A=$OUT/pwalt.tsv
# 只核顺序与「准点才采、迟到不采」，不核执行器实际迟了几秒：采样器按墙钟走，满负载机器上执行器晚一两个轮询是真的会发生的，
#   生产代码对迟到太多的点记「迟到」、不采（alt_report.py 算未测），这正是要证明的行为（10-31 合入线：gate-host 全套并发时按秒断言 5 次红）
ck "S2A_ALT=6、采 14 秒 → done；pwalt.tsv 依次记 0 a 1、0 b 3、1 a 7、1 b 9（目标秒）；准点的那几个采样文件都在、记「迟到」的没有采样文件" '[[ $RES == RESULT=done* ]] && [ "$(awk "NR<=4{printf \"%s %s %s,\", \$1, \$2, \$3}" $A)" = "0 a 1,0 b 3,1 a 7,1 b 9," ] && awk "NR<=4" $A | while read k ph tg act rest; do f=$OUT/pwalt_$k$ph.plist; if [ "$act" = 迟到 ]; then [ ! -e $f ] || exit 1; else [ -s $f ] || exit 1; fi; done'
ck "采了的点实际时刻都不早于目标（不把段边界前的功耗算进本段）" 'awk "\$4 != \"迟到\" && \$4 < \$3 {e=1} END{exit e}" $A'
ck "同段 a、b 都采了时 a 不晚于 b（先 a 后 b，段内两次采样之间有读数）" 'awk "\$4 != \"迟到\" {v[\$1\" \"\$2]=\$4} END{for(k=0;k<2;k++) if((k\" a\") in v && (k\" b\") in v && v[k\" a\"] > v[k\" b\"]) exit 1}" $A'
PATH=$G/bin:$PATH S2A_ALT_MIN_S=6 ENVJSON='"S2A_ALT":"5"' run; ck "S2A_ALT=5（不长于护边 1+3+1 秒）→ 判红「S2A_ALT 太短」" '[[ $RES == RESULT=red*S2A_ALT\ 太短* ]]'
ENVJSON='"S2A_ALT":"8"' run; ck "S2A_ALT=8 低于缺省下限 10 秒 → 判红" '[[ $RES == RESULT=red*S2A_ALT\ 太短* ]]'; }
g_dlib() { grp dlib   # 推前抽查带直连库顶层表（10-25 测试设备1 马车8：顶层表被清、b-<链>/ 还在，账上记着、机上没有）：包里 8 个 romfs 小文件 + shaders/direct 顶层两张表 + b-x/f
for i in 1 2 3 4 5 6 7 8; do echo "dir $i" > $J/pack/romfs/d$i.bin; done; mkdir -p $J/pack/shaders/direct/b-x; echo "map 3" > $J/pack/shaders/direct/shader_map.tsv; echo k > $J/pack/shaders/direct/keys.tsv; echo lib > $J/pack/shaders/direct/b-x/f; mkpack $J/pack
# 前提（不跑被测实现、按 sha 序直接算）：shader_map.tsv 不在「其余小文件按 sha 最小 5 个」里——旧规则（只核 keys.tsv + 5 个）抽不到它
pre=$(cd $J/pack && awk -F'\t' '$1 != "shaders/direct/keys.tsv" {print $3, $1}' index.tsv | LC_ALL=C sort | head -5 | grep -c shader_map)
run; rm -f $DV/apps/com.s2arm.test/Documents/资源包/shaders/direct/shader_map.tsv
run; ck "机上丢了 shader_map.tsv（前提：按 sha 抽 5 个抽不到它，实为 ${pre}）：顶层表全核 → 抽查不符、按机上清单只补推缺的、补回" '[ "$pre" = 0 ] && [[ $RES == RESULT=done* ]] && grep -q "机上实况与已应用索引不符：shaders/direct/shader_map.tsv" $OUT/run.log && grep -q "按机上实际清单核账" $OUT/run.log && [ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/shaders/direct/shader_map.tsv)" = "map 3" ]'
mkdir -p $J/pack/shaders/direct/b-x; printf 'IDX5' > $J/pack/shaders/direct/b-x/lib.idx; mkpack $J/pack
pre2=$(cd $J/pack && awk -F'\t' '$1 != "shaders/direct/keys.tsv" {print $3, $1}' index.tsv | LC_ALL=C sort | head -5 | grep -c lib.idx); run; rm -f $DV/apps/com.s2arm.test/Documents/资源包/shaders/direct/b-x/lib.idx
run; ck "机上丢了直连库容器索引 b-x/lib.idx（10-25 打包容器；前提：按 sha 抽 5 个抽不到它，实为 ${pre2}）：抽查不符、按机上清单只补推缺的、补回" '[ "$pre2" = 0 ] && [[ $RES == RESULT=done* ]] && grep -q "机上实况与已应用索引不符：shaders/direct/b-x/lib.idx" $OUT/run.log && [ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/shaders/direct/b-x/lib.idx)" = IDX5 ]'; }
g_vpush() { grp vpush   # 推完机上逐文件核（10-26 八方2：推存档 cpto 成功、机上少了 MJSaveData1）：第一次漏一个文件 → 核对不符、重推一次后照常跑完；连漏两次 → RESULT=push、点名缺的文件
rm -f $DV/drop_count; FAKE_DROP=s.dat run; ck "推存档第一次漏 s.dat → 核对不符、重推后 done" '[[ $RES == RESULT=done* ]] && grep -q "后机上核对不符（第 1 次）：device/s.dat：本地 2 字节，机上没有" $OUT/run.log'
rm -f $DV/drop_count; FAKE_DROP=s.dat FAKE_DROP_N=2 run; ck "连漏两次 → RESULT=push「推存档失败（机上核对：device/s.dat…没有）」" '[[ $RES == RESULT=push*推存档失败*device/s.dat*机上没有* ]]'; }
# 测试设备空间（10-29 原生层：测试设备1 满了，推 13 GB 资源包报 POSIX 28 / 连接被重置，看着像推送故障）：
#   ① 推之前读到可写 1000 字节 < 待推量 × 2 + 1 GB → RESULT=push「测试设备空间不足」，一批都不推；② 读不到剩余照推，推 tar 时报 POSIX 28 →「测试设备空间不足（推 … POSIX 28）」，
#   本机不留 push.tar；③ 读到可写 1 TB → 照常 done。改 1 个文件让本局有东西可推
g_prune() { grp prune   # 装包 / 推包前机上空间不够 → 卸 24 小时以上的我方 App 腾空间（11-06 维护者），本局的包不卸
  #   期望（按规则手写）：假设备上另有 io.s2arm.zz（没有装机记录、容器没文件）；ideviceinfo 报可写 1000 字节 → prune.log 有「卸 io.s2arm.zz」、run.log 有「腾空间」；
  #   本局包名照常装上、没被卸
  run; mkdir -p $DV/apps/io.s2arm.zz; echo file:///x/zz/ > $DV/apps/io.s2arm.zz/url
  echo "file 1 再改" > $J/pack/romfs/f1.bin; mkpack $J/pack
  printf '#!/bin/sh\necho 1000\n' > $G/ideviceinfo; chmod +x $G/ideviceinfo; PATH=$G:$PATH S2A_PRUNE_ROOTS=$G/rec S2A_SAVE_BACKUP_DIR=$G/bk run
  BB=$(sed -n 's/^BUNDLE=//p' $J/job.env)
  ck "空间不够 → 卸旧的我方 App 腾空间（prune.log：$(grep -o '^卸 [^（]*' $OUT/prune.log 2> /dev/null | tr '\n' ' ')）、本局包 ${BB} 还在" '[ ! -d $DV/apps/io.s2arm.zz ] && grep -q "^卸 io.s2arm.zz" $OUT/prune.log && grep -q "腾空间" $OUT/run.log && [ -d $DV/apps/$BB ]'; }
g_space() { grp space; run
echo "file 1 改" > $J/pack/romfs/f1.bin; mkpack $J/pack
printf '#!/bin/sh\necho 1000\n' > $G/ideviceinfo; chmod +x $G/ideviceinfo; PATH=$G:$PATH run
ck "① 读到可写 1000 字节 → RESULT=push「测试设备空间不足」、一批没推" '[[ $RES == RESULT=push*测试设备空间不足* ]] && grep -q "测试设备剩余空间不足：可写 0 GB < 要 1 GB" $OUT/run.log && ! ls $DV/apps/com.s2arm.test/Documents/资源包/.incoming/*.tar > /dev/null 2>&1'
FAKE_ENOSPC=.incoming run
ck "② 读不到剩余、推 tar 时 POSIX 28 → RESULT=push「测试设备空间不足（推 … POSIX 28）」、本机不留 push.tar" '[[ $RES == RESULT=push*测试设备空间不足*POSIX\ 28* ]] && [ ! -e $OUT/push.tar ]'
printf '#!/bin/sh\necho 1000000000000\n' > $G/ideviceinfo; PATH=$G:$PATH run
ck "③ 读到可写 1 TB → done、推了改过的 1 个文件" '[[ $RES == RESULT=done* ]] && grep -q "资源包：推 1 个文件" $OUT/run.log'; }
g_own() { grp own   # 贡献者自己的 iPhone（10-29 维护者 出差，开发者自查自修）：S2A_DEVICE_OWN=1 + DEV=<设备名 / UDID>，在 devicectl 列出的已配对实体 iOS 设备里找，恰好一台才用
#   期望（按规则手写）：不开开关 → skip 且不起局；开了按名 / 按 UDID → done、产物同测试设备局（host.log、frames.tsv 非空）；只有模拟器同名 / 同名两台 / 名字对上日常手机 → skip 且不起局
OU=00000000-00000000000000AA; OC=AAAAAAAA-0000-0000-0000-00000000000A; DV=$G/dev/$OC; mkdir -p $DV; : >> $DV/launches.log; n0=$(wc -l < $DV/launches.log)
export FAKE_DEVLIST='[{"name":"我的 iPhone","udid":"'$OU'","identifier":"'$OC'"},{"name":"模拟机","udid":"SIM-0001","identifier":"SIM-ID-1","reality":"simulated"},{"name":"双胞胎","udid":"T-1","identifier":"TW-1"},{"name":"双胞胎","udid":"T-2","identifier":"TW-2"},{"name":"日常","udid":"00000000-0000000000000000","identifier":"DAILY-1"}]'
sed_edit "s/^DEV=1$/DEV='我的 iPhone'/" $J/job.env   # job.env 按 shell 读：带空格的设备名加引号
FAKE_HW_UDID=$OU run; ck "不开 S2A_DEVICE_OWN：名单外设备 → skip（不在测试设备名单）且不起局" '[[ $RES == RESULT=skip* ]] && grep -q "不在测试设备名单" $OUT/run.log && [ "$(wc -l < $DV/launches.log)" -eq $n0 ]'
S2A_DEVICE_OWN=1 FAKE_HW_UDID=$OU run; ck "S2A_DEVICE_OWN=1 + 设备名 → done，拉回 host.log / frames.tsv 非空，起在这台设备上" '[[ $RES == RESULT=done* ]] && [ -s $OUT/host.log ] && [ -s $OUT/frames.tsv ] && [ "$(wc -l < $DV/launches.log)" -gt $n0 ]'
sed_edit "s/^DEV=.*/DEV=$OU/" $J/job.env
S2A_DEVICE_OWN=1 FAKE_HW_UDID=$OU run; ck "S2A_DEVICE_OWN=1 + 硬件 UDID → done" '[[ $RES == RESULT=done* ]]'
n1=$(wc -l < $DV/launches.log)
for w in 模拟机 双胞胎 日常; do sed_edit "s/^DEV=.*/DEV=$w/" $J/job.env; S2A_DEVICE_OWN=1 FAKE_HW_UDID=$OU run
  ck "S2A_DEVICE_OWN=1 + ${w}（只有模拟器 / 同名两台 / 日常手机）→ skip 且不起局" '[[ $RES == RESULT=skip* ]] && [ "$(wc -l < $DV/launches.log)" -eq $n1 ]'; done
ck "同名两台的报错写明改用 UDID" 'grep -lq "改用 UDID" $J/out_*/run.log'
}
g_vbr() { grp vbr   # 真机驾驶员桥（11-01）：S2A_VISION_BRIDGE=1 → 起局环境带 S2A_VISION_REMOTE=1、起桥、收局停桥；缺省（本单测设 0）不带、不起
  printf 'echo x\n' > $G/vis.sh
  S2A_VISION_BRIDGE=1 S2A_VISION_SH=$G/vis.sh BRIDGE_POLL=0.2 run
  ck "开桥：起局环境带 S2A_VISION_REMOTE=1，桥起 / 桥停各记一行，bridge.log 收到 stop 退出" '[[ $RES == RESULT=done* ]] && grep -q "S2A_VISION_REMOTE" $OUT/run.log && grep -q 驾驶员桥起 $OUT/run.log && grep -q 驾驶员桥停 $OUT/run.log && grep -q "桥停（收到 stop）" $OUT/vision/bridge.log'
  run; ck "缺省关：起局环境不带 S2A_VISION_REMOTE、不起桥" '[[ $RES == RESULT=done* ]] && ! grep -q S2A_VISION_REMOTE $OUT/run.log && ! grep -q 驾驶员桥 $OUT/run.log'
}
g_pso() { grp pso   # 管线种子来源（维护者 11-02）：卸载 / 覆盖装之前先把机上 wire/<程序号>/pso_user.tsv 拉回 pso_seed/<程序号>/（只部署也拉）；机上没有只记一行、不判红
mkdir -p $J/pack/shaders/shcache/0100abcd00000000; mkpack $J/pack; P=$S2A_PSO_SEED_DIR/0100ABCD00000000; W=$DV/apps/com.s2arm.test/Documents/wire/0100ABCD00000000
DEPLOY_ONLY=1 run; ck "机上还没装 → 装前记「机上没有」、不建种子目录、照常部署" '[[ $RES == RESULT=deployed* ]] && grep -q "管线种子来源（装前）：机上没有" $OUT/run.log && [ ! -d $P ]'
mkdir -p $W; printf 'R\tab\nC\tcd_b\n' > $W/pso_user.tsv
UNINSTALL=1 DEPLOY_ONLY=1 run; ck "人玩过留下 2 行、卸载重装 → 卸载前拉回 pso_seed/<程序号>/<作业>_<局>_装前.tsv、与机上原表一致（卸载后机上已没有）" '[[ $RES == RESULT=deployed* ]] && [ "$(ls $P/*_装前.tsv | wc -l)" -eq 1 ] && [ "$(cat $P/*_装前.tsv)" = "$(printf "R\tab\nC\tcd_b")" ] && [ ! -e $W/pso_user.tsv ]'
mkdir -p $W; printf 'R\tef\n' > $W/pso_user.tsv
run; ck "覆盖装的测试局：装前、局末各存一份，按局分文件、不盖掉上一份" '[[ $RES == RESULT=done* ]] && [ "$(ls $P/*_装前.tsv | wc -l)" -eq 2 ] && [ "$(ls $P/*_局末.tsv | wc -l)" -eq 1 ] && grep -q "管线种子来源（局末）：拉回 pso_user.tsv 1 行" $OUT/run.log'; }
g_psod() { grp psod   # 直连包（11-05 能耗线）：没有 shaders/shcache/<程序号>/、只有 shaders/direct/game.tsv → 程序号取 program_id，装前 / 局末照样拉 pso_user.tsv（原先只认 shcache，Windows 出包机直连包一行不回流）
rm -rf $J/pack/shaders/shcache; mkdir -p $J/pack/shaders/direct; printf 'title_id\t0100DCBA00000000\nprogram_id\t0100DCBA00000000\n' > $J/pack/shaders/direct/game.tsv; mkpack $J/pack
P=$S2A_PSO_SEED_DIR/0100DCBA00000000; W=$DV/apps/com.s2arm.test/Documents/wire/0100DCBA00000000; mkdir -p $W; printf 'R\tgh\n' > $W/pso_user.tsv
run; ck "直连包测试局：装前、局末各拉回一份进 pso_seed/0100DCBA00000000/（内容 = 机上 1 行），着色器缓存那段照旧跳过、不建 shcache_rt/" '[[ $RES == RESULT=done* ]] && [ "$(ls $P/*_装前.tsv 2> /dev/null | wc -l)" -eq 1 ] && [ "$(ls $P/*_局末.tsv 2> /dev/null | wc -l)" -eq 1 ] && [ "$(cat $P/*_局末.tsv)" = "$(printf "R\tgh")" ] && grep -q "着色器缓存拉回跳过：包里没有" $OUT/run.log && [ ! -e $OUT/shcache_rt ]'; }
g_vkspv() { grp vkspv   # Vulkan 直连包（11-06 银河 2）：没有 shaders/shcache/、没有 game.tsv，程序号只能从 exefs/main.npdm 取 → 收局照样拉 wire/<程序号>/shaders/、vk/<键>.spv 并进 pso_seed/<程序号>/vk_spv/
#   期望（手写）：机上 2 支 .spv → pso_seed/0100EEEE00000000/vk_spv/ 恰 2 个、内容同机上；日志「Vulkan 现编 SPIR-V：新收 2 支」。原先取不到程序号、整段跳过且一行不打
rm -rf $J/pack/shaders/shcache $J/pack/shaders/direct/game.tsv; mkdir -p $J/pack/exefs $J/pack/shaders/direct/vk
python3 -c "import struct,sys;b=bytearray(0x100);b[:4]=b'META';struct.pack_into('<I',b,0x70,0x80);b[0x80:0x84]=b'ACI0';struct.pack_into('<Q',b,0x90,0x0100EEEE00000000);open(sys.argv[1],'wb').write(b)" $J/pack/exefs/main.npdm
printf '# s2arm-vk-lib 3\n' > $J/pack/shaders/direct/vk/vk_shaders.tsv; mkpack $J/pack
P=$S2A_PSO_SEED_DIR/0100EEEE00000000/vk_spv; W=$DV/apps/com.s2arm.test/Documents/wire/0100EEEE00000000/shaders/vk
DEPLOY_ONLY=1 run   # 先装上（首次装包建容器），再在机上放现编产物
mkdir -p $W; printf 'spv-a' > $W/aaaaaaaaaaaaaaaa.spv; printf 'spv-b' > $W/bbbbbbbbbbbbbbbb.spv; printf 'x' > $W/aaaaaaaaaaaaaaaa.row
run; ck "Vulkan 包测试局：程序号取 npdm，vk_spv 2 支并进 pso_seed/0100EEEE00000000/vk_spv/、内容同机上" '[[ $RES == RESULT=done* ]] && [ "$(ls $P 2> /dev/null | wc -l | tr -d " ")" = 2 ] && [ "$(cat $P/aaaaaaaaaaaaaaaa.spv)" = spv-a ] && grep -q "Vulkan 现编 SPIR-V：新收 2 支" $OUT/run.log'; }
# 流式推包（11-06 Windows 端到端线：Mac 推包有 s2a-devpush + 手机 UDID 时每批 put-tar 直接写手机，不在本机落 1 GB 临时 tar）：
#   假 s2a-devpush 把清单打成 pax tar 写进假容器同名位置。① 有工具 + UDID → 汇总行「流式 s2a-devpush N 批 / devicectl 0 批」、本机不留 push.tar、App 照常解开、done
#   ② 工具这批写失败 → 这批退回 devicectl（「流式推 … 没成」+「devicectl 1 批」）、done
g_stream() { grp stream
  printf '#!/bin/bash\n[ "$3" = put-tar ] || exit 2\n[ -n "${FAKE_DEVPUSH_FAIL:-}" ] && { echo "AFC 连不上" >&2; exit 1; }\nr="$FAKE_DEVPUSH_DIR/apps/$2/$6"; mkdir -p "$(dirname "$r")"\nCOPYFILE_DISABLE=1 tar --format pax -cf "$r" -C "$5" -T "$4" && echo "put-tar $(wc -l < "$4" | tr -d " ") 个文件 $(stat -f %%z "$r") 字节 → $6"\n' > $G/s2a-devpush; chmod +x $G/s2a-devpush
  S2A_DEVPUSH=$G/s2a-devpush FAKE_DEVPUSH_DIR=$DV run
  ck "流式推包：有 s2a-devpush + UDID → 各批流式写、devicectl 0 批、本机不留 push.tar、done" '[[ $RES == RESULT=done* ]] && grep -q "流式 s2a-devpush 1 批 / devicectl 0 批" $OUT/run.log && [ ! -e $OUT/push.tar ] && [ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/romfs/f1.bin)" = "file 1" ]'
  echo changed2 > $J/pack/romfs/f3.bin; mkpack $J/pack
  S2A_DEVPUSH=$G/s2a-devpush FAKE_DEVPUSH_DIR=$DV FAKE_DEVPUSH_FAIL=1 run
  ck "流式推包这批失败 → 退回 devicectl、done" '[[ $RES == RESULT=done* ]] && grep -q "流式推 .* 没成" $OUT/run.log && grep -q "流式 s2a-devpush 0 批 / devicectl 1 批" $OUT/run.log && [ "$(cat $DV/apps/com.s2arm.test/Documents/资源包/romfs/f3.bin)" = changed2 ]'
}
GS=${DEV_RUN_GROUPS:-"stream ${S2A_PRIVATE_TOOLS:+prune }incr vpush shot hang gc stall stall2 gpuhang procs crash nomark restart guard apq trunc stalepush livef lag stale recheck screen batt con shc apcrash smoke ownplay alt wire dlib own space vbr pso psod vkspv"}   # 单跑某组：DEV_RUN_GROUPS=hang
# 组并行限流（11-02 体验线定因：每组执行器按 0.2 秒轮询、每轮起一个 python 假 devicectl，单份约 150 个滚动短命进程；
#   30 组全并行、门禁里两三份并发把 gate-host 负载顶到 200+）：同时最多 DEV_RUN_PAR 组，缺省核数 × 2/3（2–8；本机 18 核 / gate-host 12 核都是 8）。
#   同负载实测（本机）：全并行 84 秒、子孙进程峰值 244 → 限 8 组 82 秒、峰值 82；限 6 组 93 秒、峰值 60。轮询间隔不动（alt / lag 等组的时序断言按 0.2 秒算）
NP=${DEV_RUN_PAR:-$(( $(sysctl -n hw.ncpu 2> /dev/null || nproc 2> /dev/null || echo 12) * 2 / 3 ))}; [ "$NP" -lt 2 ] && NP=2; [ "$NP" -gt 8 ] && NP=8
# 组名 / 组函数不许重复（11-09：两个 g_stale 同名、清单里 stale 列了两次 → 同一组并行起两份、共用组目录与假设备互相清状态，门禁随机红；前一组用例被遮住从没跑过）
d=$( { tr ' ' '\n' <<< "$GS" | grep .; } | sort | uniq -d; grep -o '^g_[a-z0-9_]*()' "$0" | sort | uniq -d); [ -z "$d" ] || { echo "不通 组名或组函数重复：$(echo $d)"; bad=1; }
for g in $GS; do while [ "$(jobs -rp | wc -l)" -ge "$NP" ]; do sleep 0.2; done; ( g_$g ) > $T/out_$g 2>&1 & done; wait
for g in $GS; do cat $T/out_$g; done; grep -q '^不通' $T/out_* && bad=1
for g in $GS; do grep -q '^通' $T/out_$g || { echo "不通 组 $g 一条结果都没出（组脚本中途出错？）"; tail -3 $T/out_$g | sed 's/^/    | /'; bad=1; }; done   # 组脚本自己出错时没有「不通」行，不能算绿
for p in $T/g_*/dev/*/app.pid; do [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null; done; rm -rf $T   # 只收本单测起的假 App（门禁里几份单测并行）
[ $bad = 0 ] && echo "dev_run 单测全过" || { echo "dev_run 单测有不通"; exit 1; }
