#!/bin/bash
# 一键出包单测（pack/oneclick/oneclick.sh 认游戏一步 + pack/oneclick/adhoc_sign.sh，约 5 秒，不碰游戏 / 密钥 / 真机）：bash tests/oneclick_test.sh
# 假 nx_unpack（S2A_NX_UNPACK）按文件名吐 --inspect 清单、解包一律失败——脚本走完认游戏就停在解包。期望按规则手写：
#   ① 一个本体 + 更新 + DLC + 别的游戏的 DLC → 认出本体程序号、显示版本取最高更新、只取本游戏的 4 个包、点名不取的别家程序号；titles.tsv 里的款用已知款名
#   ② 两个本体 → 红「本体应恰好 1 款，实际 2 款」；③ 32 位程序 → 红「只跑 AArch64」；④ 没有本体 → 红「实际 0 款」；⑤ 新款没在表里 → 款名 t<程序号小写>
#   ⑭ 原件挪走、原位置留软链（10-26 维护者把 Downloads 原件挪外置盘）：本体是指向别处文件的软链、两份更新在软链目录里 → 认出结果同 ①（3 个包）；
#      pack/srcs.sh 的 src_ids 也列出这 3 个，大小取目标的（2048 字节），不是软链本身
#   ⑥ 自签：现编的小 App + 内嵌库 → 签名里的权限恰好只有提高内存上限、内嵌库也签上、codesign 严格校验过
#   ⑰ 目标设备架构（10-27 R13 坑 1）：一键出包与启动器打 iPhone 包（pack/jobs/ipa.py → target.py）同口径，按目标设备设 S2A_PSO_ARCHS。
#      离线夹具：假 CoreDevice（devicectl 列一台 iPhone16,1）+ 假 Xcode 设备特征库（iPhone16,1 → applegpu_g16p，与真库一致）。
#      期望：没给 --device 认连着那台、--device 指 UDID/设备名都认、显式 S2A_PSO_ARCHS 不覆盖、认不出按缺省并打一行提示
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0; trap 'rm -rf "$T"' EXIT
export S2A_COREDEVICE=$T/nocd   # 目标设备架构那一步不碰本机真连着的设备（结果与门禁机器无关、也不等 devicectl 的 60 秒）；⑰ 自己给夹具
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
cat > "$T/nxu" <<'X'
#!/bin/bash
[ "$2" = --inspect ] || { echo "假解包：不解" >&2; exit 1; }
case $(basename "$3") in
  base.nsp) echo '{"schema":1,"packages":[{"titleId":"0100A3501946E000","baseId":"0100A3501946E000","version":0,"type":"base","name":"Game A","display":"1.0.0","builds":{},"guestIsa":"aarch64","icon":""}]}';;
  upd1.nsp) echo '{"schema":1,"packages":[{"titleId":"0100A3501946E800","baseId":"0100A3501946E000","version":65536,"type":"update","name":"Game A","display":"1.0.1","builds":{},"guestIsa":"unknown","icon":""}]}';;
  upd2.nsp) echo '{"schema":1,"packages":[{"titleId":"0100A3501946E800","baseId":"0100A3501946E000","version":131072,"type":"update","name":"Game A","display":"1.1.1","builds":{},"guestIsa":"unknown","icon":""}]}';;
  dlc.nsp) echo '{"schema":1,"packages":[{"titleId":"0100A3501946F001","baseId":"0100A3501946E000","version":0,"type":"dlc","name":"","display":"","builds":{},"guestIsa":"unknown","icon":""}]}';;
  other_dlc.nsp) echo '{"schema":1,"packages":[{"titleId":"0100999999999001","baseId":"0100999999998000","version":0,"type":"dlc","name":"","display":"","builds":{},"guestIsa":"unknown","icon":""}]}';;
  b2.nsp) echo '{"schema":1,"packages":[{"titleId":"0100999999998000","baseId":"0100999999998000","version":0,"type":"base","name":"Game B","display":"1.0","builds":{},"guestIsa":"aarch64","icon":""}]}';;
  a32.nsp) echo '{"schema":1,"packages":[{"titleId":"0100111111110000","baseId":"0100111111110000","version":0,"type":"base","name":"Old","display":"1.0","builds":{},"guestIsa":"aarch32","icon":""}]}';;
  a32upd.nsp) echo '{"schema":1,"packages":[{"titleId":"0100111111110800","baseId":"0100111111110000","version":65536,"type":"update","name":"Old","display":"2.0","builds":{},"guestIsa":"aarch64","icon":""}]}';;
  a32unk.nsp) echo '{"schema":1,"packages":[{"titleId":"0100111111110800","baseId":"0100111111110000","version":65536,"type":"update","name":"Old","display":"2.0","builds":{},"guestIsa":"unknown","icon":""}]}';;
  acnh.nsp) echo '{"schema":1,"packages":[{"titleId":"01006F8002326000","baseId":"01006F8002326000","version":0,"type":"base","name":"Island","display":"1.0","builds":{},"guestIsa":"aarch64","icon":""}]}';;
  big.nsp) echo '{"schema":1,"packages":[{"titleId":"01006F8002326000","baseId":"01006F8002326000","version":0,"type":"base","name":"Island","display":"1.0","builds":{},"guestIsa":"aarch64","icon":"","contentBytes":8000000000}]}';;
  tool.nsp) echo '{"schema":1,"packages":[{"titleId":"0100F9C00EB8C000","baseId":"0100F9C00EB8C000","version":0,"type":"base","name":"Island Transfer Tool","display":"1.0","builds":{},"guestIsa":"aarch64","icon":"","contentBytes":2000000}]}';;
  new.nsp) echo '{"schema":1,"packages":[{"titleId":"0100ABCDEF012000","baseId":"0100ABCDEF012000","version":0,"type":"base","name":"New One","display":"2.0","builds":{},"guestIsa":"aarch64","icon":""}]}';;
  *) exit 3;;
esac
X
chmod +x "$T/nxu"; : > "$T/k.keys"
OC() { S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids.tsv bash "$H/pack/oneclick/oneclick.sh" "$1" --keys "$T/k.keys" --out "$T/out_$(basename "$1")" 2>&1; }
mk() { mkdir -p "$T/$1/sub"; shift; for f in "$@"; do : > "$T/$(dirname "$f")/$(basename "$f")"; done; }
mk g1 g1/base.nsp g1/sub/upd1.nsp g1/sub/upd2.nsp g1/dlc.nsp g1/other_dlc.nsp; o=$(OC "$T/g1")
ck "① 认出 Game A（1.1.1、程序号 0100A3501946E000、款 octopath2）、用 4 个包、不取别家 0100999999998000、停在解包（实得：$(grep -E '①|🔴' <<< "$o" | cut -c1-160 | tr '\n' ' ')）" \
  'grep -q "认出：Game A（1.1.1，程序号 0100A3501946E000，款 octopath2）" <<< "$o" && grep -q "用 4 个包" <<< "$o" && grep -q "不取：0100999999998000" <<< "$o" && grep -q "解包失败" <<< "$o"'
mk g2 g2/base.nsp g2/b2.nsp; o=$(OC "$T/g2"); ck "② 两个本体 → 红" 'grep -q "本体应恰好 1 款，实际 2 款" <<< "$o"'
# ②b 不止一个本体（10-26 #456 动森 + 小岛转移工具）：期望（按规则手写）主游戏 8 GB ≥ 小工具 2 MB × 4 → 自动认 Island、黄字点名跳过 Transfer Tool；
#    两款都没大小（g2）→ 仍红；--title-id 指定 Game B → 认出 Game B；指定不存在的程序号 → 红「指定的程序号」
mk gmb gmb/big.nsp gmb/tool.nsp; o=$(OC "$T/gmb")
ck "②b 游戏 + 小工具 → 认出 Island、黄字点名跳过 Transfer Tool" 'grep -q "认出：Island" <<< "$o" && grep -q "🟡 文件夹里有 2 个本体，按内容大小认 Island（01006F8002326000）是主游戏，跳过：Island Transfer Tool（0100F9C00EB8C000）" <<< "$o"'
o=$(S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids.tsv bash "$H/pack/oneclick/oneclick.sh" "$T/g2" --keys "$T/k.keys" --out "$T/out_g2t" --title-id 0100999999998000 2>&1)
ck "②b --title-id 指定 Game B → 认出 Game B" 'grep -q "认出：Game B" <<< "$o"'
o=$(S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids.tsv bash "$H/pack/oneclick/oneclick.sh" "$T/g2" --keys "$T/k.keys" --out "$T/out_g2x" --title-id 0100AAAAAAAAA000 2>&1)
ck "②b 指定不存在的程序号 → 红" 'grep -q "指定的程序号 0100AAAAAAAAA000 不在这些包里的本体中" <<< "$o"'
mk g3 g3/a32.nsp; o=$(OC "$T/g3"); ck "③ 32 位程序 → 红" 'grep -q "只跑 AArch64" <<< "$o"'
# 本体 32 位、最新更新 64 位（马车8 1.0 → 4.0.1）：按更新判、不拒；更新读不出指令集（unknown）才退回看本体、照样拒
mk g3u g3u/a32.nsp g3u/a32upd.nsp; o=$(OC "$T/g3u"); ck "③b 本体 32 位 + 更新 64 位 → 不拒（实得：$(grep -E '①|🔴' <<< "$o" | cut -c1-120 | tr '\n' ' ')）" '! grep -q "只跑 AArch64" <<< "$o" && grep -q "认出：Old" <<< "$o"'
mk g3k g3k/a32.nsp g3k/a32unk.nsp; o=$(OC "$T/g3k"); ck "③c 本体 32 位 + 更新指令集不明 → 按本体拒" 'grep -q "只跑 AArch64" <<< "$o"'
mk g4 g4/upd1.nsp g4/dlc.nsp; o=$(OC "$T/g4"); ck "④ 没有本体 → 红 0 款" 'grep -q "实际 0 款" <<< "$o"'
mk g5 g5/new.nsp; o=$(OC "$T/g5"); ck "⑤ 表外新款 → 款名 t0100abcdef012000" 'grep -q "款 t0100abcdef012000" <<< "$o"'
# 直连缺省（维护者 10-24 / 10-25 零配置）：有没有直连配置档（graphics/nvn/games/<程序号>/game.tsv，动森有、八方 2 / 表外新款没有）都出直连包；11-07 兼容模式 S2A_DUAL=0 已删
ck "⑤b 没有手写配置档（八方 2 / 表外新款）→ 照样直连（10-25 直连优先、零配置）" 'grep -q "① 画面：直连" <<< "$(OC "$T/g1")" && grep -q "① 画面：直连" <<< "$o"'
mk g6 g6/acnh.nsp; o=$(OC "$T/g6"); ck "⑤c 有手写配置档（动森 01006F8002326000）→ 同样直连，不另分档（实得：$(grep '① 画面' <<< "$o" | cut -c1-80)）" 'grep -q "① 画面：直连" <<< "$o"'
o=$(S2A_DUAL=0 OC "$T/g6"); ck "⑤d 环境里留着旧的 S2A_DUAL=0 → 不认、照样直连单轨" 'grep -q "① 画面：直连单轨" <<< "$o" && ! grep -q "兼容模式" <<< "$o"'
# Android 的打包阶段也只有直连单轨。
# 这里的选项是构建阶段选择，与 Android 运行时 AUTO 对设备能力的选择不同。
OCA() { S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids.tsv bash "$H/pack/oneclick/oneclick.sh" "$T/g6" --keys "$T/k.keys" --out "$T/out_android" --target android 2>&1; }
o=$(OCA); ck "⑤e Android 直连单轨" 'grep -q "① 画面：直连单轨" <<< "$o"'
# ⑭ 软链原件
mkdir -p "$T/real/subreal" "$T/g7"; head -c 2048 /dev/zero > "$T/real/base.nsp"; head -c 2048 /dev/zero > "$T/real/subreal/upd1.nsp"; head -c 2048 /dev/zero > "$T/real/subreal/upd2.nsp"
ln -s "$T/real/base.nsp" "$T/g7/base.nsp"; ln -s "$T/real/subreal" "$T/g7/sub"; o=$(OC "$T/g7")
ck "⑭ 软链原件：认出 Game A（1.1.1）、用 3 个包（实得：$(grep -E '①|🔴' <<< "$o" | cut -c1-160 | tr '\n' ' ')）" 'grep -q "认出：Game A（1.1.1，程序号 0100A3501946E000" <<< "$o" && grep -q "用 3 个包" <<< "$o"'
s7=$(S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids7.tsv S2A_GAME_SRC=$T/g7 bash -c "source '$H/pack/srcs.sh'; src_ids" | awk -F'\t' '{print $2}' | LC_ALL=C sort | uniq -c | awk '{print $1 "x" $2}')
ck "⑭ src_ids 列出软链原件：「${s7}」（3 个、每个 2048 字节）" '[ "$s7" = 3x2048 ]'
# ⑭b 系统 / 隐藏目录里的 .xci / .nsp 不当原件（11-02：回收站 $RECYCLE.BIN 的 $I*.xci 被探测）：源目录里另放回收站、System Volume Information、.Trashes 各一个，src_ids 仍只列那 3 个
mkdir -p "$T/g7/\$RECYCLE.BIN" "$T/g7/System Volume Information" "$T/g7/.Trashes"; for d in '$RECYCLE.BIN' 'System Volume Information' '.Trashes'; do head -c 1024 /dev/zero > "$T/g7/$d/\$IJUNK.xci"; done
s7b=$(S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids7b.tsv S2A_GAME_SRC=$T/g7 bash -c "source '$H/pack/srcs.sh'; src_ids" | awk -F'\t' '{print $2}' | LC_ALL=C sort | uniq -c | awk '{print $1 "x" $2}')
ck "⑭b src_ids 跳过回收站 / 系统 / 隐藏目录：「${s7b}」（仍是 3 个、每个 2048 字节）" '[ "$s7b" = 3x2048 ]'
# ⑥ 自签
mkdir -p "$T/A.app/Frameworks"; printf 'int main(void){return 0;}\n' > "$T/m.c"; printf 'int f(void){return 1;}\n' > "$T/l.c"
xcrun clang -o "$T/A.app/S2Arm" "$T/m.c" 2> /dev/null && xcrun clang -dynamiclib -o "$T/A.app/Frameworks/libx.dylib" "$T/l.c" 2> /dev/null
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>S2Arm</string><key>CFBundleIdentifier</key><string>t.x</string></dict></plist>\n' > "$T/A.app/Info.plist"
o=$(bash "$H/pack/oneclick/adhoc_sign.sh" "$T/A.app" "$H/pack/app.entitlements" 2>&1); r=$?
keys=$(codesign -d --entitlements - --xml "$T/A.app" 2> /dev/null | plutil -convert json -o - - 2> /dev/null | python3 -c 'import json,sys; print(" ".join(sorted(json.load(sys.stdin))))' 2> /dev/null)
ck "⑥ 自签 rc=${r}、权限恰好 1 项（${keys}）、内嵌库已签、严格校验过" '[ $r = 0 ] && [ "$keys" = com.apple.developer.kernel.increased-memory-limit ] && codesign -v "$T/A.app/Frameworks/libx.dylib" 2> /dev/null && codesign -v --strict "$T/A.app" 2> /dev/null'
# ⑦ pack/ipa.sh 共享根只出正式包（测试出包线 10-23）：共享根（S2A_ROOTS = S2A_ROOTS_SHARED）上给 S2A_SIGN=adhoc / S2A_BUNDLE_ID / S2A_DISPLAY_SUFFIX 各报红退出，在刷新产物根之前（产物根不动）；
#   已停用的 S2A_DISPLAY_NAME（11-04：显示名只取游戏自带 nacp 标题，pack/display_name.py）任何根上都报红
mkdir -p "$T/shr"; for kv in S2A_SIGN=adhoc S2A_BUNDLE_ID=x.y S2A_DISPLAY_SUFFIX=z; do
  o=$(env S2A_ROOTS=$T/shr S2A_ROOTS_SHARED=$T/shr S2A_PACK_LOCKS=$T/locks S2A_PACK_PAUSE=$T/pause S2A_DISK_FREE_G=999 S2A_TEAM=T0 "$kv" bash "$H/pack/ipa.sh" zz 2>&1); r=$?
  ck "⑦ 共享根给 ${kv%%=*} → 报红退出（rc ${r}）、没刷新产物根" '[ $r = 1 ] && [[ "$o" == *"共享根只出正式包"* ]] && [ -z "$(ls "$T/shr")" ]'
done
o=$(env S2A_ROOTS=$T/shr S2A_ROOTS_SHARED=$T/shr S2A_PACK_LOCKS=$T/locks S2A_PACK_PAUSE=$T/pause S2A_DISK_FREE_G=999 S2A_TEAM=T0 S2A_DISPLAY_NAME=z bash "$H/pack/ipa.sh" zz 2>&1); r=$?
ck "⑦b 给已停用的 S2A_DISPLAY_NAME → 报红退出（rc ${r}）、没刷新产物根" '[ $r = 1 ] && [[ "$o" == *"S2A_DISPLAY_NAME 已停用"* ]] && [ -z "$(ls "$T/shr")" ]'
# ⑧ 包键：显式 S2A_SIGN=dev 与不设相同（不追加 sign=），adhoc 才追加；直接取 ipa.sh 里那一行（经 pack/ipa_key.sh ipa_pkg_key）求值
mkdir -p "$T/pkroot/exefs" "$T/pkroot/romfs"; printf main > "$T/pkroot/exefs/main"   # 最小产物根：包键要算进包资源输入键（审计 P1-54），没有 exefs/main 会报红
PKL=$(grep '^PK=\$(ipa_pkg_key' "$H/pack/ipa.sh"); pk() { ( source "$H/pack/ipa_key.sh"; C=c1; CUR=$T/pkroot; G=pk; SIGN=$1; BID=b; DN="游戏 S2Arm"; VAMM_LAYOUT=alias; unset S2A_BUNDLE_ID; eval "$PKL"; echo "$PK" ); }
ck "⑧ 包键：dev 与缺省相同、adhoc 带 sign=adhoc（dev「$(pk dev)」adhoc「$(pk adhoc)」）" '[ "$(pk dev)" = "$(S2A_SIGN= pk dev)" ] && [[ "$(pk dev)" != *sign=* ]] && [[ "$(pk adhoc)" == *"sign=adhoc"* ]]'
ck "⑧b 包键：全能力包带 allcaps、与发布包键不同（发布「$(pk dev)」全能力「$(ALLCAPS=1 pk dev)」）" '[[ "$(ALLCAPS=1 pk dev)" == *" allcaps"* ]] && [[ "$(pk dev)" != *allcaps* ]]'
ck "⑧c 包键：原生档包带 native、与发布包 / 全能力包键都不同（原生「$(NATIVE=1 pk dev)」）" '[[ "$(NATIVE=1 pk dev)" == *" native"* ]] && [[ "$(pk dev)" != *native* ]] && [ "$(NATIVE=1 pk dev)" != "$(ALLCAPS=1 pk dev)" ]'
# ⑨–⑪ 冷 / 热标注按各部件自己报的命中算（pack/oneclick/cachehits.py）；夹具行抄自 10-23 丝之歌全冷局、马车8 热局的真日志，期望手算
CHD=$T/ch; mkdir -p "$CHD"
printf '%s\n' "🟢 sk 改写 3 个模块（12 秒）" "🟢 sk 系统数据：字体 7 个，时区生成，现行 → sk@sys-1" "🟢 sk 没有要加宽的布局（覆盖包 x），现行也没有旧覆盖，跳过" \
  "🟢 sk 追加内容：🟢 追加内容：程序号 0100；清单里 0 个；0 秒" "🟢 sk RomFS 索引 840K（27 秒），现行 → sk@rx-1" "🟢 sk 着色器全量预编（3529 秒；no_rule 0）" "🟢 sk 现行产物根各阶段与本树一致：sk@sh-1" > "$CHD/cold.log"
echo '{"rebuilt":{"msl":23588,"msl_total":23588,"pso_chunks":692,"pso_chunks_total":692},"link_dylib":{"total":23570,"hit":0}}' > "$CHD/cold.json"
echo '{"rebuilt":{"msl":0,"msl_total":23588,"pso_chunks":0,"pso_chunks_total":692},"link_dylib":{"total":23570,"hit":23570}}' > "$CHD/warm.json"
echo "nxaot：编 ios-arm64 NativeAOT 静态库（键 1）" > "$CHD/build_miss.log"; : > "$CHD/build_hit.log"; : > "$CHD/ipa.log"
printf '%s\n' "🟢 sk 现行已是改写 rw-1（改写器 / 扫描器 / 启用表都没变），跳过" "🟢 sk 系统数据已齐，跳过" "🟢 sk 现行已是布局覆盖 ui-1，跳过" \
  "🟢 sk 现行已是追加内容 ao-1，跳过" "🟢 sk 现行已是 RomFS 索引 rx-1，跳过" "🟢 sk 现行已是着色器 sh-1，跳过" "🟢 sk 现行产物根各阶段与本树一致：sk@sh-1" > "$CHD/allhit.log"
echo "🟢 sk 现行已是包 k" > "$CHD/ipa_hit.log"
CHP="$H/pack/oneclick/cachehits.py"
c1=$(python3 "$CHP" --unpack built --refresh "$CHD/cold.log" --shader "$CHD/cold.json" --ipa "$CHD/ipa.log" --build "$CHD/build_miss.log")
ck "⑨ 丝之歌式全冷（宽屏「没活…跳过」不算命中）→ 冷缓存（${c1%%	*}）" '[ "${c1%%	*}" = 冷缓存 ]'
c2=$(python3 "$CHP" --unpack built --refresh "$CHD/cold.log" --shader "$CHD/warm.json" --ipa "$CHD/ipa.log" --build "$CHD/build_hit.log")
ck "⑩ 马车8 式：新输出目录各阶段现做、着色器内部全命中、C# 库命中 → 部分命中（${c2%%	*}；${c2#*	}）" '[ "${c2%%	*}" = 部分命中 ] && [[ "$c2" == *"函数体命中 23570/23570"* ]] && [[ "$c2" == *"C# 库 命中"* ]]'
c3=$(python3 "$CHP" --unpack reused --refresh "$CHD/allhit.log" --ipa "$CHD/ipa_hit.log")
ck "⑪ 全命中 → 热缓存（${c3%%	*}）" '[ "${c3%%	*}" = 热缓存 ]'
# 反证：旧判法（refresh.log / unpack.log 里数「跳过 / 复用」）在丝之歌夹具上标成「有缓存命中」
old=$([ "$(grep -c '跳过\|复用' "$CHD/cold.log" 2> /dev/null)" -gt 0 ] && echo 有缓存命中 || echo 冷缓存)
ck "⑨ 反证：旧判法在同一夹具上误标「${old}」" '[ "$old" = 有缓存命中 ]'
# ⑫ 一键流程 refresh 只跑一次（10-25：外层 refresh 和 ipa.sh 里的 refresh 各算一遍，工具链不一致时直连库整份出两遍）：
#   oneclick 刷完把现行产物根名交给 ipa.sh（S2A_REFRESHED）。私有临时根故意缺 ctl/control.nacp（真刷新必红「刷不到本树的阶段键」）、
#   缺改写产物（跳过刷新后在「没有改写产物」停下，不往下编包）。期望：不给 → 照刷；给了且等于现行 → 不刷；给了但现行已变 → 照刷
mkdir -p $T/rr/acnh@v1/exefs; ln -s acnh@v1 $T/rr/acnh@现行
IP() { env S2A_ROOTS=$T/rr S2A_ROOTS_SHARED=$T/shr2 S2A_PACK_LOCKS=$T/locks2 S2A_PACK_PAUSE=$T/pause S2A_DISK_FREE_G=999 S2A_WIDEN_AUTO_RUN=0 S2A_SIGN=adhoc "$@" bash "$H/pack/ipa.sh" acnh 2>&1; }
o1=$(IP); o2=$(IP S2A_REFRESHED=acnh@v1); o3=$(IP S2A_REFRESHED=acnh@旧)
ck "⑫ 不给 S2A_REFRESHED → ipa.sh 照刷新（报刷不到阶段键）" '[[ "$o1" == *刷不到本树的阶段键* ]] && [[ "$o1" != *调用方刚刷过* ]]'
ck "⑫ 给了且等于现行 → 不再刷新、往下走到下一步" '[[ "$o2" == *调用方刚刷过* ]] && [[ "$o2" != *刷不到本树的阶段键* ]] && [[ "$o2" == *没有改写产物* ]]'
ck "⑫ 给了但现行已变 → 照刷新" '[[ "$o3" == *刷不到本树的阶段键* ]] && [[ "$o3" != *调用方刚刷过* ]]'
# ⑬ 缺省签名按本机（10-25 运维线：原先一律 adhoc，出给测试设备的包装不上）：没配签名团队 → adhoc；配了（S2A_TEAM_DEFAULT）→ dev；显式 --sign 照用。
#    keys 给不存在的路径：打出签名那行后就在 keys 检查处退出，不往下出包
mkdir -p "$T/sg"; SG() { (unset S2A_TEAM S2A_TEAM_DEFAULT; env S2A_MACHINE_NO_LOCAL=1 "$@" bash "$H/pack/oneclick/oneclick.sh" "$T/sg" --keys "$T/sg/没有" ${SGA:-} 2>&1 | grep -m1 '签名：' | sed 's/^\[[^]]*\] //'); }
s1=$(SG); s2=$(SG S2A_TEAM_DEFAULT=TEAMX); s3=$(SGA="--sign adhoc" SG S2A_TEAM_DEFAULT=TEAMX)
ck "⑬ 没配团队 →「${s1}」；配了 →「${s2}」；显式 --sign adhoc →「${s3}」" '[[ "$s1" == "签名：adhoc（本机没配签名团队；"* ]] && [ "$s2" = "签名：dev（本机配了签名团队）" ] && [[ "$s3" == "签名：adhoc（--sign 指定；"* ]]'
# ⑮ 只出通用时按目标系统拦（generic_os_gate）随兼容模式一起删（11-07）：一键出包里不再有这个函数
ck "⑮ 一键出包里不再有通用渲染器的系统版本闸门" '! grep -q "generic_os_gate" "$H/pack/oneclick/oneclick.sh"'
# ⑯ 机器读事件流（--events，10-26 照上游 --progress json）：每行是带 ev / t 的 JSON；g1 走完认游戏、停在解包（假解包不解）→
#   stage 认游戏（k 1 / n 6）→ stage_end 认游戏 ok → stage 解包（k 2）→ stage_end 解包 失败 → 最后一条 result ok=false，msg 是那句红；say 带全部输出（含 🔴 那行）
EVT=$T/ev.jsonl; S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids.tsv bash "$H/pack/oneclick/oneclick.sh" "$T/g1" --keys "$T/k.keys" --out "$T/out_ev" --events "$EVT" > "$T/ev.out" 2>&1
seq16=$(python3 - "$EVT" <<'PY'
import json, sys
ev = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
assert all(isinstance(e.get("t"), int) and e.get("ev") for e in ev)
st = [(e["ev"], e.get("k"), e.get("n"), e["name"], e.get("ok")) for e in ev if e["ev"] in ("stage", "stage_end")]
r = ev[-1]; says = [e["msg"] for e in ev if e["ev"] == "say"]
print(st == [("stage", 1, 6, "认游戏", None), ("stage_end", None, None, "认游戏", True), ("stage", 2, 6, "解包", None), ("stage_end", None, None, "解包", False)]
      and r["ev"] == "result" and r["ok"] is False and r["msg"] in "".join(says) and any(m.startswith("🔴") for m in says), len(ev))
PY
)
ck "⑯ 事件流：认游戏 → 解包失败 → result ok=false（${seq16}）" '[[ "$seq16" == True* ]]'
EVT2=$T/ev2.jsonl; bash "$H/pack/oneclick/oneclick.sh" "$T/g1" --events "$EVT2" --target ohos > /dev/null 2>&1
r16=$(tail -1 "$EVT2" 2> /dev/null)
ck "⑯ 不经 die 的退出（--target ohos 退 3）也有一条 result ok=false（${r16:0:80}）" 'python3 -c "import json,sys; r=json.loads(sys.argv[1]); sys.exit(0 if r[\"ev\"] == \"result\" and r[\"ok\"] is False and \"退出码 3\" in r[\"msg\"] else 1)" "$r16"'
# 计数事件：各阶段取 progress.tsv 里最新一行（总数 0 时 frac 为 null），阶段名里的引号转义
eval "$(sed -n '/^evprog() {/,/"\$EVF"; }$/p' "$H/pack/oneclick/oneclick.sh")"
printf '1\t抽取\t1\t3\n2\t直连库·翻译 "批"\t10\t100\n3\t抽取\t3\t3\n4\t空\t0\t0\n' > "$T/prog.tsv"; : > "$T/evp.jsonl"
S2A_PROGRESS=$T/prog.tsv EVF=$T/evp.jsonl evprog
p16=$(python3 -c "import json,sys; print([(e['name'], e['done'], e['total'], e['frac']) for e in map(json.loads, open(sys.argv[1], encoding='utf-8'))])" "$T/evp.jsonl" 2>&1)
ck "⑯ 计数事件按阶段取最新：${p16}" '[ "$p16" = "[('"'"'抽取'"'"', 3, 3, 1.0), ('"'"'直连库·翻译 \"批\"'"'"', 10, 100, 0.1), ('"'"'空'"'"', 0, 0, None)]" ]'

# 刷新结束的「⏱ 分阶段用时」一行（十来段 + 合计，实测 >300 字）原先被 cut -c1-200 截在 direct 段名上（11-02 c1102 普查：直连段秒数和合计都读不到，没法判缓存命中）：接线检查——复制进日志的那行不得再按 200 截，且上限放得下该行
ck "⑩ oneclick 日志里的 refresh.log 末行（含 ⏱ 分阶段用时）不按 200 字截断" '! grep -q "tail -1 \"\$OUT/logs/refresh.log\" | cut -c1-200" "$H/pack/oneclick/oneclick.sh" && grep -q "tail -1 \"\$OUT/logs/refresh.log\" | cut -c1-1200" "$H/pack/oneclick/oneclick.sh"'
# ⑰ 目标设备架构（10-27 R13 坑 1；10-28 按维护方 A3 对齐 ops/gpu-archs 三种模式）：命令行出包原先按缺省单架构出归档与函数体动态库，
#   缺省覆盖不到的机型装箱后才在 link_glue 报「no stub archive」。这里验它与 pack/jobs/ipa.py 同口径认设备，
#   且只在设备架构**不在**我方真机集合（pack/our_devices.tsv）内时才 export 单架构；S2A_PSO_ARCHS_ALL=1 不被降级。
#   夹具全离线：假 CoreDevice（devicectl 输出 JSON）+ 假 Xcode 设备特征库（真库同一套 JOIN：iPhone16,1 → applegpu_g16p）
printf 'A1\t甲\tapplegpu_g18p\tios\t27\t是\t-\nA2\t乙\tapplegpu_g19p\tios\t27\t是\t-\n' > "$T/our_devices.tsv"; export S2A_OUR_DEVICES=$T/our_devices.tsv   # 真清单是维护方私有文件
D=$T/devdb; mkdir -p "$D/cd/Resources/bin" "$D/xc/Platforms/iPhoneOS.platform/usr/standalone"
cat > "$D/cd/Resources/bin/devicectl" <<'X'
#!/bin/bash
o=; while [ $# -gt 0 ]; do [ "$1" = --json-output ] && { o=$2; shift 2; continue; }; shift; done
if [ -n "${FAKE_DEV_JSON:-}" ]; then printf '%s' "$FAKE_DEV_JSON" > "$o"; else printf '{"result":{"devices":[]}}' > "$o"; fi
X
chmod +x "$D/cd/Resources/bin/devicectl"
sqlite3 "$D/xc/Platforms/iPhoneOS.platform/usr/standalone/device_traits.db" <<'SQL'
create table Devices(Target text, ProductType text, ProductDescription text);
create table DeploymentVariant(DeviceTarget text, DeviceTraitSet text);
create table DeviceTraits(DeviceTraitSetID text, GPUArchName text);
insert into Devices values('t16','iPhone16,1','iPhone 15 Pro');
insert into DeploymentVariant values('t16','s16');
insert into DeviceTraits values('s16','applegpu_g16p');
insert into Devices values('t18','iPhone18,2','iPhone 17 Pro Max');
insert into DeploymentVariant values('t18','s18');
insert into DeviceTraits values('s18','applegpu_g18p');
SQL
A17='{"result":{"devices":[{"identifier":"D1","hardwareProperties":{"reality":"physical","platform":"iOS","productType":"iPhone16,1","udid":"00000000-0000000000000003","marketingName":"iPhone 15 Pro"},"deviceProperties":{"name":"测试设备","osVersionNumber":"27.0.1"},"connectionProperties":{"transportType":"wired","pairingState":"paired","tunnelState":"connected"}}]}}'
OCD() { local n=$1; shift; env -u DEVELOPER_DIR "FAKE_DEV_JSON=${FAKE_DEV_JSON-$A17}" S2A_COREDEVICE=$D/cd S2A_DEVELOPER_DIR=$D/xc S2A_NX_UNPACK=$T/nxu S2A_SRC_IDS=$T/ids.tsv \
  bash "$H/pack/oneclick/oneclick.sh" "$T/g1" --keys "$T/k.keys" --out "$T/ocd_$n" "$@" 2>&1; }
o=$(OCD auto); ck "⑰ 没给 --device：认连着那台 iPhone16,1（g16p 不在我方集合）→ export 单架构（实得：$(grep '目标架构' <<< "$o" | cut -c1-150)）" \
  'grep -q "目标架构：applegpu_g16p（目标 iPhone 15 Pro iPhone16,1 不在我方真机集合 applegpu_g18p applegpu_g19p 内：只覆盖该架构" <<< "$o"'
o=$(OCD udid --device 00000000-0000000000000003); ck "⑰ --device 指 UDID → applegpu_g16p" 'grep -q "applegpu_g16p（目标 iPhone 15 Pro iPhone16,1" <<< "$o"'
o=$(OCD name --device 测试设备); ck "⑰ --device 指设备名 → applegpu_g16p" 'grep -q "applegpu_g16p（目标 iPhone 15 Pro iPhone16,1" <<< "$o"'
# 该机型在我方真机集合内（iPhone18,2 → g18p，pack/our_devices.tsv）：不 export，下游按缺省集合出（10-28 维护方 A3 第 2 条）
A182='{"result":{"devices":[{"identifier":"D2","hardwareProperties":{"reality":"physical","platform":"iOS","productType":"iPhone18,2","udid":"00000000-0000000000000001","marketingName":"iPhone 17 Pro Max"},"deviceProperties":{"name":"测试设备1","osVersionNumber":"26.0.1"},"connectionProperties":{"transportType":"wired","pairingState":"paired","tunnelState":"connected"}}]}}'
o=$(FAKE_DEV_JSON=$A182 OCD ours); ck "⑰ 设备架构在集合内（iPhone18,2 → g18p）→ 不 export、按缺省集合出（实得：$(grep '目标架构' <<< "$o" | cut -c1-130)）" \
  'grep -q "目标架构：按缺省我方真机集合出（applegpu_g18p applegpu_g19p；目标 iPhone 17 Pro Max iPhone18,2 的 applegpu_g18p 已在集合内，不单编）" <<< "$o" && ! grep -q "只覆盖该架构" <<< "$o"'
# S2A_PSO_ARCHS_ALL=1（插着手机发全机型包）：不许被设备降成单架构（10-28 维护方 A3 第 1 条）
o=$(S2A_PSO_ARCHS_ALL=1 OCD all); ck "⑰ S2A_PSO_ARCHS_ALL=1 + 连着设备 → 不按设备改，按全机型出（实得：$(grep '目标架构' <<< "$o" | cut -c1-110)）" \
  'grep -q "目标架构：全机型（S2A_PSO_ARCHS_ALL=1，按 Xcode 设备库全集出，不按设备改）" <<< "$o" && ! grep -q "applegpu_g16p" <<< "$o"'
o=$(S2A_PSO_ARCHS=applegpu_g15p OCD keep); ck "⑰ 显式给 S2A_PSO_ARCHS=applegpu_g15p → 不按设备改" 'grep -q "applegpu_g15p" <<< "$o" && ! grep -q "applegpu_g16p" <<< "$o"'
o=$(FAKE_DEV_JSON='{"result":{"devices":[]}}' OCD none); ck "⑰ 认不出（没连设备）→ 按缺省出，并写明三种模式怎么指定" \
  'grep -q "没认出目标设备，按缺省出（我方真机集合；要全机型给 S2A_PSO_ARCHS_ALL=1，要单架构给 S2A_PSO_ARCHS）" <<< "$o"'
# 读不到系统版本（设备没报 osVersionNumber → target.py 输出 "-"）：不许 export S2A_PSO_OS=0（会出 ios0 归档、通用闸报「iOS 0」，维护方 A3 第 4 条）
A_NOOS='{"result":{"devices":[{"identifier":"D3","hardwareProperties":{"reality":"physical","platform":"iOS","productType":"iPhone16,1","udid":"00000000-0000000000000003","marketingName":"iPhone 15 Pro"},"deviceProperties":{"name":"测试设备"},"connectionProperties":{"transportType":"wired","pairingState":"paired","tunnelState":"connected"}}]}}'
o=$(FAKE_DEV_JSON=$A_NOOS OCD noos); ck "⑰ 读不到系统版本 → 只 export 架构、不设 S2A_PSO_OS=0" \
  'grep -q "只覆盖该架构" <<< "$o" && ! grep -q "iOS 0\|S2A_PSO_OS=0" <<< "$o"'
[ $bad = 0 ] && echo "oneclick 单测全过" || { echo "oneclick 单测有不通"; exit 1; }
