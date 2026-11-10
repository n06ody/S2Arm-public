#!/bin/bash
# 门禁只看：tools/weak_link_decl.py runtime/src/* runtime/build.mk runtime/tests/mk/* runtime/tests/t_gfxmode.c graphics/nvn/Makefile graphics/nvn/gfx/* tests/vk_indirect_count/Makefile
# 弱链接登记单测（tools/weak_link_decl.py + runtime/build.mk S2A_HOST_WEAK + graphics/nvn NVN_TEST_WEAK，约 50 秒）：bash tests/weak_link_decl_test.sh
# 期望：本仓库 runtime/src 的 weak_import 全部登记 → check 退出 0、host 放行参数与 make 收到的一致；
#   临时仓库里给 gfxmode.c 加一个新弱符号并引用：只在源码登记一处 → check 绿、原生单测 gfxmode 照常编过跑过、Python 侧放行参数含它（7 处手抄归一后无需再改别处）；
#   不登记 → check 退出 1、点名这个符号与文件行（明确报错在一处），gfxmode 单测链接失败
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
WU=-Wl,; WU=${WU}-U   # 链接放行参数前缀拼出来写：全仓 check 不许字面出现（本单测也不例外）
trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
o=$(cd "$H" && python3 tools/weak_link_decl.py check 2>&1); r=$?
ck "本仓库：runtime/src 的 weak_import 全部登记（rc=${r}：$(echo "$o" | tail -1)）" '[ $r = 0 ]'
py=$(cd "$H" && python3 tools/weak_link_decl.py flags host | tr ' ' '\n' | LC_ALL=C sort)
mkv=$(cd "$H" && make -s --no-print-directory -f - BUILD=$T/b s2a_print_weak <<'EOF' 2>/dev/null | tr ' ' '\n' | LC_ALL=C sort
include runtime/build.mk
s2a_print_weak:; @echo $(call S2A_WEAK_U,host)
EOF
)
# make 侧取 $(call S2A_WEAK_U,host)，不取 S2A_HOST_WEAK：后者只在 Darwin 下定义，Linux 上为空、这条必红（11-01 #1242 贡献者自检报出）
ck "Python 侧与 make 侧 host 放行参数同一份（$(echo "$py" | grep -c .) 个）" '[ -n "$py" ] && [ "$py" = "$mkv" ]'

# 负对照：临时仓库里加一个新弱符号 s2a_zz_probe（gfxmode.c 声明并引用）
git clone -q --shared "$H" "$T/c" 2> /dev/null && git -C "$T/c" checkout -q "$(git -C "$H" rev-parse HEAD)" 2> /dev/null
for f in tools/weak_link_decl.py runtime/build.mk runtime/src/gfxmode.c runtime/src/host_main.c runtime/tests/t_gfxmode.c graphics/nvn/Makefile tests/vk_indirect_count/Makefile; do cp "$H/$f" "$T/c/$f"; done
cat >> "$T/c/runtime/src/gfxmode.c" <<'EOF'
void *s2a_zz_probe(void) __attribute__((weak_import));   // 单测探针：新弱符号
int s2a_zz_probe_linked(void) { return s2a_zz_probe != 0; }
EOF
o=$(cd "$T/c" && python3 tools/weak_link_decl.py check 2>&1); r=$?
ck "不登记：check 退出 1 并点名 s2a_zz_probe 与 gfxmode.c 行（rc=${r}）" '[ $r = 1 ] && [[ "$o" == *"弱符号 s2a_zz_probe（runtime/src/gfxmode.c:"* ]]'
if [ "$(uname -s)" = Darwin ]; then
  (cd "$T/c" && make -s native-test-gfxmode BUILD=$T/b1 > "$T/u.log" 2>&1); r=$?
  ck "不登记：gfxmode 单测链接失败（rc=${r}，$(grep -m1 -o '_s2a_zz_probe' "$T/u.log" || echo 未见符号名)）" '[ $r != 0 ] && grep -q _s2a_zz_probe "$T/u.log"'
fi
sed -i.bak '1a\
// 弱链接[host]：s2a_zz_probe（单测探针）' "$T/c/runtime/src/gfxmode.c"; rm -f "$T/c/runtime/src/gfxmode.c.bak"
o=$(cd "$T/c" && python3 tools/weak_link_decl.py check 2>&1); r=$?
ck "只登记一处：check 绿（rc=${r}）" '[ $r = 0 ]'
ck "只登记一处：Python 侧放行参数含它" '(cd "$T/c" && python3 tools/weak_link_decl.py flags host) | grep -q -- "${WU},_s2a_zz_probe"'
if [ "$(uname -s)" = Darwin ]; then
  (cd "$T/c" && make -s native-test-gfxmode BUILD=$T/b2 > "$T/r.log" 2>&1); r=$?
  ck "只登记一处：gfxmode 单测照常编过跑过（rc=${r}）" '[ $r = 0 ]'
fi

# graphics/nvn（11-01 t_nvn_m3 手抄放行参数漏帧录放三个符号）：直连层新引用宿主符号只登记一处，t_nvn_m3 就链得过；手写放行参数被 check 拦
sed -i.bak "s/-lc++ \$(NVN_TEST_WEAK)\$/-lc++ ${WU},_s2a_vk_xlate/" "$T/c/tests/vk_indirect_count/Makefile"; rm -f "$T/c/tests/vk_indirect_count/Makefile.bak"
o=$(cd "$T/c" && python3 tools/weak_link_decl.py check 2>&1); r=$?
ck "手写放行参数：check 退出 1 并点名文件行（rc=${r}）" '[ $r = 1 ] && [[ "$o" == *"tests/vk_indirect_count/Makefile:"*"手写了链接放行参数"* ]]'
cp "$H/tests/vk_indirect_count/Makefile" "$T/c/tests/vk_indirect_count/Makefile"
printf '\nint s2a_zz_nvnprobe(void);   /* 单测探针：直连层新引用的宿主符号 */\nint s2a_zz_nvnprobe_used(void) { return s2a_zz_nvnprobe(); }\n' >> "$T/c/graphics/nvn/gfx/nvn/hle_nvn.c"
if [ "$(uname -s)" = Darwin ]; then
  (cd "$T/c" && make -s -C graphics/nvn OUT="$T/nvn" "$T/nvn/tests/t_nvn_m3" > "$T/nu.log" 2>&1); r=$?
  ck "graphics/nvn 不登记：t_nvn_m3 链接失败（rc=${r}，$(grep -m1 -o '_s2a_zz_nvnprobe' "$T/nu.log" || echo 未见符号名)）" '[ $r != 0 ] && grep -q _s2a_zz_nvnprobe "$T/nu.log"'
fi
sed -i.bak '1a\
// 弱链接[nvntest]：s2a_zz_nvnprobe（单测探针）' "$T/c/graphics/nvn/gfx/nvn/hle_nvn.c"; rm -f "$T/c/graphics/nvn/gfx/nvn/hle_nvn.c.bak"
ck "graphics/nvn 只登记一处：nvntest 放行参数含它" '(cd "$T/c" && python3 tools/weak_link_decl.py flags nvntest) | grep -q -- "${WU},_s2a_zz_nvnprobe"'
if [ "$(uname -s)" = Darwin ]; then
  (cd "$T/c" && make -s -C graphics/nvn OUT="$T/nvn" "$T/nvn/tests/t_nvn_m3" > "$T/nr.log" 2>&1); r=$?
  ck "graphics/nvn 只登记一处：t_nvn_m3 链得过（rc=${r}）" '[ $r = 0 ]'
fi
# 场合按整词认（11-05 维护者：make 侧原先按子串收「[^]]*host[^]]*」、Python 侧按逗号拆开整词比；安卓登记 android_draw_host
#   被 make 误收进 host，c4d0a36e4 合入后 main 红）：登记一个名字含 host、但不是 host 的场合 → 两侧 host 放行参数都不含它；
#   它自己的场合两侧都含它；负对照：旧的 make sed 规则会把它收进 host
sed -i.bak '1a\
// 弱链接[myhost_probe]：s2a_zz_hostlike（单测探针：场合名含 host 但不是 host）' "$T/c/runtime/src/gfxmode.c"; rm -f "$T/c/runtime/src/gfxmode.c.bak"
mk() { (cd "$T/c" && make -s --no-print-directory -f - BUILD=$T/b s2a_print_weak <<EOF 2>/dev/null
include runtime/build.mk
s2a_print_weak:; @echo \$(call S2A_WEAK_U,$1)
EOF
); }
ck "场合整词：Python 侧 host 不含 s2a_zz_hostlike、myhost_probe 含它" '! (cd "$T/c" && python3 tools/weak_link_decl.py flags host) | grep -q _s2a_zz_hostlike && (cd "$T/c" && python3 tools/weak_link_decl.py flags myhost_probe) | grep -q _s2a_zz_hostlike'
ck "场合整词：make 侧 host 不含 s2a_zz_hostlike、myhost_probe 含它" '! mk host | grep -q _s2a_zz_hostlike && mk myhost_probe | grep -q _s2a_zz_hostlike'
old=$(cd "$T/c" && LC_ALL=C sed -n 's/.*弱链接\[[^]]*host[^]]*\]：\([A-Za-z0-9_]*\).*/\1/p' runtime/src/gfxmode.c)
ck "负对照：旧 make 规则（子串）会把 s2a_zz_hostlike 收进 host（收到：${old}）" 'grep -qx s2a_zz_hostlike <<< "$old"'
[ $bad = 0 ] && echo "弱链接登记单测全过" || { echo "弱链接登记单测有不通"; exit 1; }
