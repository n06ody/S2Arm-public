#!/bin/bash
# 门禁覆盖：graphics/tools/direct/direct_lib.sh tests/direct_lib_android_test.sh
# 直连库出包脚本的安卓目标单测（graphics/tools/direct/direct_lib.sh，S2A_DIRECT_TARGETS=android，#783；假仓库 + 桩，约 3 秒）：bash tests/direct_lib_android_test.sh
#   脚本原样拷进假仓库；抽取合并、配对表、出库工具 dsh_vklib 换成桩（桩记下调用与参数）。Metal 那几步（fout_want.py / se_want.py / dsh_batch.py / spvc_inproc.py / pso_seed.py）
#   假仓库里不放：安卓分支调到它们就找不到、报红。期望按 #783 直连线 / 出包线的答复手写：
#   ① 出成：<输出>/b-<链>-vk/ 里是库容器（lib.idx + lib.000.pak，桩代写），没有逐键散文件、没有 Metal 的 b-<链>/；keys.tsv 只列 vk_build.tsv 里 ok 的键、排好序；
#      vk_build.tsv 副本、表与 key_map 收进输出目录；direct_lib.json 记 target=android；
#      dsh_vklib 收到合并后的包目录、配置档副本、库目录 b-<链>-vk 与链号、libnxgal，逐键产物放工作目录；末行是部分出库的 🟡，带 skipped 2 支
#   ② 输出目录已在时再出一次：互换成功（Linux renameat2 / macOS renamex_np），换下来的旧版本删掉、不留临时目录
#   ③ 产物根没有抽取阶段的 extract/ → 退出 3、🟡「这里不另抽」（抽取归 refresh.sh 在前面跑的 stage_extract.sh），不调抽取脚本、不调出库工具、输出目录不动
#   ④ Vulkan / OpenGL 款 → 退出 0、不出库、不调出库工具
#   ⑤ 出库工具失败 → 退出 1、🔴、输出目录不动
#   ⑥ 只出安卓却要发布 iOS（S2A_DIRECT_PUBLISH=ios）→ 退出 1、🔴、不调出库工具
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0; trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
P=$T/repo; mkdir -p $P/graphics/tools/direct/vklib $P/graphics/nvn/tools $P/graphics/nvn/games $P/translator $P/pack $P/tools
cp "$H/graphics/tools/direct/direct_lib.sh" $P/graphics/tools/direct/; mkdir -p $P/pack/jobs && cp "$H/pack/jobs/space_rule.py" "$H/pack/jobs/library.py" $P/pack/jobs/; cp "$H/pack/srcs.sh" $P/pack/; cp "$H/tools/progress.sh" "$H/tools/clonedir.py" "$H/tools/nostall.py" $P/tools/
export FAKE_CALLS=$T/calls
cat > $P/graphics/nvn/tools/gfx_api.py <<'EOF'
import os
print(os.environ.get('FAKE_API', 'nvn') + '\t桩')
EOF
cat > $P/graphics/nvn/games/game.py <<'EOF'
import os, sys
print(os.path.join(os.environ['FAKE_PROF'], sys.argv[2]))
EOF
: > $P/graphics/nvn/games/zero_skip.tsv
printf 'def version(): return "v1"\n' > $P/translator/dmsh_chain.py
cat > $P/graphics/tools/direct/extract.sh <<'EOF'
echo extract >> "$FAKE_CALLS.extract"; exit 1
EOF
cat > $P/graphics/tools/direct/merge_extract.py <<'EOF'
import os, shutil, sys
s2x, pkg, smap, out = sys.argv[1:5]
os.makedirs(os.path.join(out, 'pkg'), exist_ok=True); shutil.copy(smap, os.path.join(out, 'shader_map.tsv'))
open(os.path.join(out, 'shader_facts.tsv'), 'w').write('')
open(os.path.join(out, 'union_keys.tsv'), 'w').write(''.join(k * 16 + '\n' for k in 'abcd'))
open(os.path.join(out, 'key_map.tsv'), 'w').write('s2a0000000000001\taaaaaaaaaaaaaaaa\n')
open(os.path.join(out, 'merge.json'), 'w').write('{}')
EOF
printf 'print("配对表桩")\n' > $P/graphics/tools/direct/run_pairs.py
# 跑局知识快照桩（审计 #1534 P1-54：direct_lib.sh 单独调用时现出一份，四张表；真工具的规则在 graphics/tools/direct/tests/t_run_knowledge.py 核）
cat > $P/graphics/tools/direct/run_knowledge.py <<'EOF'
import os, sys
os.makedirs(sys.argv[1], exist_ok=True)
for t in ('pairs', 'fout_want', 'se_want', 'seen'): open(os.path.join(sys.argv[1], t + '.tsv'), 'w').write('# 桩\n')
print('跑局知识快照：桩')
EOF
# 出库工具桩：记参数；FAKE_VK_RC 非空就按它退出；否则 --out 里 a / c 两键 ok（出 _b.spv + _b.json）、b / d（gs）skipped；FAKE_VK_FAILED 时 b 为 failed 并非零退出，
#   --lib 里写 lib.idx / lib.000.pak（真工具是 LibPack 打的容器，那支在 t_dsh_vklib ⑩ 核）与 run<FAKE_RUN>（认出是第几次）
cat > $P/graphics/tools/direct/vklib/dsh_vklib.py <<'EOF'
import os, sys
a = sys.argv[1:]; open(os.environ['FAKE_CALLS'] + '.vk', 'a').write(' '.join(a) + '\n')
if os.environ.get('FAKE_VK_RC'): sys.exit(int(os.environ['FAKE_VK_RC']))
o = a[a.index('--out') + 1]; lib = a[a.index('--lib') + 1]; os.makedirs(o, exist_ok=True); os.makedirs(lib, exist_ok=True)
rows = [('cccccccccccccccc', 'vs', 'ok', ''), ('aaaaaaaaaaaaaaaa', 'ps', 'ok', ''), ('bbbbbbbbbbbbbbbb', 'gs', 'failed' if os.environ.get('FAKE_VK_FAILED') else 'skipped', 'translation failed: 桩' if os.environ.get('FAKE_VK_FAILED') else 'gs 不出 .spv'), ('dddddddddddddddd', 'gs', 'skipped', 'gs 不出 .spv')]
for k, st, s, why in rows:
    if s == 'ok':
        open(os.path.join(o, k + '_b.spv'), 'wb').write(b'\x03\x02\x23\x07'); open(os.path.join(o, k + '_b.json'), 'w').write('{}')
open(os.path.join(o, 'vk_build.tsv'), 'w').write(''.join('\t'.join(r) + '\n' for r in rows))
if os.environ.get('FAKE_VK_FAILED'):
    print('[RED] Vulkan 翻译失败 1 键', file=sys.stderr); sys.exit(1)
for n in ('lib.idx', 'lib.000.pak', 'run' + os.environ.get('FAKE_RUN', '1')): open(os.path.join(lib, n), 'w').write(n)
print('dsh_vklib (vkfp 00000000000000aa): 4 keys, ok 2, skipped 2, failed 0')
EOF
# 产物根（抽取阶段出好的 extract/）与配置档
G=$T/root; mkdir -p $G/romfs $G/exefs $G/extract/pkg $G/extract/split $G/extract/s2x $T/prof/0100ABCD00000000
echo main > $G/exefs/main; printf 'title_id\t0100abcd00000000\n' > $G/unpack.tsv
printf 'split\t1\ns2a_extract\t2\n' > $G/extract/ok; echo s > $G/extract/split.log; echo x > $G/extract/s2x.log
printf 'vs_0.bin\t0\t-\tcccccccccccccccc\t-\n' > $G/extract/split/shader_map.tsv; : > $G/extract/split/shader_facts.tsv; : > $G/extract/split/shader_pairs.tsv
echo 'title	桩' > $T/prof/0100ABCD00000000/game.tsv
export S2A_DISK_FREE_G=999   # 出库开工前的空间复核（pack/jobs/space_rule.py）按注入余量判，单测结果不随本机磁盘余量变（11-09 门禁机余量紧时假红）
export FAKE_PROF=$T/prof S2A_PYTHON=python3 S2A_DIRECT_CACHE=$T/cache S2A_CPU_LEDGER=0 S2A_PSO_SOURCES=$T/nopso J=2 DSX_LIB=$T/libdsx.so
O=$T/out
D() { S2A_DIRECT_TARGETS=android S2A_DIRECT_PUBLISH=${PUB:-android} bash $P/graphics/tools/direct/direct_lib.sh $G $O 2>&1; }
n() { wc -l < "$1" 2> /dev/null | tr -d ' ' || echo 0; }
snap() { (cd $O 2> /dev/null && find . -type f | LC_ALL=C sort | xargs -r cat | cksum); }
o=$(D); r=$?; V=$O/b-v1-vk
ck "① 出成（rc=${r}）：b-v1-vk/ 里是库容器、没有逐键散文件，没有 Metal 的 b-v1/，vk_build.tsv 副本在输出目录" '[ $r = 0 ] && [ -s $V/lib.idx ] && [ -s $V/lib.000.pak ] && [ -z "$(ls $V | grep _b\.)" ] && [ -f $O/vk_build.tsv ] && [ ! -e $O/b-v1 ]'
ck "① keys.tsv 只列 ok 的键、排好序（实 $(tr '\n' ' ' < $O/keys.tsv 2> /dev/null)）" '[ "$(tr "\n" " " < $O/keys.tsv)" = "aaaaaaaaaaaaaaaa cccccccccccccccc " ]'
ck "① 表与 key_map 收进输出目录、direct_lib.json 记 target=android" '[ -f $O/game.tsv ] && [ -f $O/shader_map.tsv ] && [ -f $O/key_map.tsv ] && python3 -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d[\"target\"] == \"android\" and d[\"built\"] == 2 and d[\"union\"] == 4 else 1)" $O/direct_lib.json'
a=$(tail -1 $T/calls.vk)
ck "① dsh_vklib 收到合并后的包目录、配置档副本、库目录与链号、libdsx（${a:0:60}…）" '[[ "$a" == "build --pkg "*"/merged/pkg --profile "*"/profile/0100ABCD00000000 --out "*"/vk/keys --lib "*"/b-v1-vk --chain v1 --dsx $T/libdsx.so --work "* ]]'
l=$(echo "$o" | tail -1)
ck "① 末行部分出库 🟡，带 skipped 2 支（${l:0:90}）" '[[ "$l" == *"🟡 直连缺口（部分出库）："* ]] && [[ "$l" == *"skipped 2 支"* ]] && [[ "$l" == *"安卓 Vulkan 库 b-v1-vk：并集 4，出库 2，没出成 2"* ]]'
o=$(FAKE_RUN=2 D); r=$?
ck "② 输出目录已在时再出一次（rc=${r}）：换成新的一份、旧的删掉、不留临时目录" '[ $r = 0 ] && [ -f $V/run2 ] && [ ! -e $V/run1 ] && [ -z "$(ls -d $O.stage.* 2> /dev/null)" ]'
s0=$(snap); nv=$(n $T/calls.vk); mv $G/extract/ok $T/ok; o=$(D); r=$?; mv $T/ok $G/extract/ok
ck "③ 没有抽取阶段的 extract/（rc=${r}）：退 3、🟡 这里不另抽、不调抽取 / 出库、输出目录不动" '[ $r = 3 ] && [[ "$o" == *"🟡 直连缺口：产物根里没有抽取阶段的 extract/"* ]] && [[ "$o" == *"这里不另抽着色器"* ]] && [ ! -e $T/calls.extract ] && [ "$(n $T/calls.vk)" = "$nv" ] && [ "$(snap)" = "$s0" ]'
for api in vulkan gl; do o=$(FAKE_API=$api D); r=$?
  ck "④ ${api} 款（rc=${r}）：退 0、不出库、不调出库工具" '[ $r = 0 ] && [[ "$o" == *"安卓运行时自己编，不出直连库"* ]] && [ "$(n $T/calls.vk)" = "$nv" ] && [ "$(snap)" = "$s0" ]'; done
o=$(FAKE_VK_FAILED=1 D); r=$?
ck "⑤ 逐键翻译失败（rc=${r}）：退 1、🔴、旧输出目录不动" '[ $r = 1 ] && [[ "$o" == *"🔴 安卓 Vulkan 库出库失败 rc=1"* ]] && [ "$(snap)" = "$s0" ] && [ -z "$(ls -d $O.stage.* 2> /dev/null)" ]'
o=$(FAKE_VK_RC=1 D); r=$?
ck "⑤ 出库工具失败（rc=${r}）：退 1、🔴、输出目录不动" '[ $r = 1 ] && [[ "$o" == *"🔴 安卓 Vulkan 库出库失败 rc=1"* ]] && [ "$(snap)" = "$s0" ] && [ -z "$(ls -d $O.stage.* 2> /dev/null)" ]'
nv=$(n $T/calls.vk); o=$(PUB=ios D); r=$?
ck "⑥ 只出安卓却要发布 iOS（rc=${r}）：退 1、🔴、不调出库工具" '[ $r = 1 ] && [[ "$o" == *🔴* ]] && [ "$(n $T/calls.vk)" = "$nv" ]'
[ $bad = 0 ] && echo "直连库安卓目标单测：全通" || echo "直连库安卓目标单测：有不通"
exit $bad
