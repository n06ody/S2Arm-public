#!/bin/bash
# 门禁覆盖：pack/stage_vk_warm_seed.sh android/tools/vk_warm_seed.py
# 安卓首局预热种子阶段单测（pack/stage_vk_warm_seed.sh；真 vk_warm_seed.py、临时产物根，约 3 秒）：bash tests/stage_vk_warm_seed_test.sh
# 期望按「什么改产物」手写（种子目录名 = 闭包摘要前 8 字节 hex - profile 摘要前 8 字节 hex，测试自己算）：
#   ① 没有来源 → 退 0、现行没有 shaders/direct/warm-vk、记键
#   ② 来源里放一份真机清单（程序 1 条）→ 键变、现行 warm-vk/<闭包>-<profile>/programs-v1-<闭包>.bin 与来源逐字节相同
#   ③ 不变再跑 → 跳过、键不变；同内容换个来源子目录名 → 仍跳过（产出只由内容定）
#   ④ 来源只剩一份坏文件（校验和坏）→ 键变、现行 warm-vk 删掉（包里不带种子）
#   ⑤ 工具失败 → 退 0、打【黄】、现行不动、键不变
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0; trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
mkdir -p $T/repo/pack/orch $T/repo/tools $T/repo/android/tools $T/repo/graphics/tools/direct $T/roots $T/home
cp "$H/pack/roots.sh" "$H/pack/stage_vk_warm_seed.sh" "$H/pack/stage_keyfiles.sh" $T/repo/pack/; mkdir -p $T/repo/tools/stage_deps_hook && cp "$H/tools/stage_deps.py" "$H/tools/machine.sh" $T/repo/tools/ && cp "$H/tools/stage_deps_hook/sitecustomize.py" $T/repo/tools/stage_deps_hook/; cp "$H/pack/orch/current.py" $T/repo/pack/orch/
cp "$H/tools/clonedir.py" "$H/tools/plock.sh" "$H/tools/flock_hold.py" "$H/tools/direct_shards.py" $T/repo/tools/ 2> /dev/null
cp "$H/android/tools/vk_warm_seed.py" $T/repo/android/tools/
for f in run_knowledge.py fout_want.py se_want.py run_pairs.py pso_layout.py pso_layout.json; do cp "$H/graphics/tools/direct/$f" $T/repo/graphics/tools/direct/; done   # 程序号同 run_knowledge.program_id（vk_warm_seed.py 引它）
(cd $T/repo && git init -q && git add -A && git -c user.email=t@t -c user.name=t commit -qm x)
export S2A_ROOTS=$T/roots S2A_ROOTS_SHARED=$T/shared S2A_RUN_ROOT=$T/run HOME=$T/home; unset S2A_VK_WARM_SOURCES S2A_MACHINE_LOCAL S2A_PY
mkdir -p $T/roots/acnh@s0/romfs; echo r > $T/roots/acnh@s0/romfs/f; printf 'title_id\t0100aaaa00000000\n' > $T/roots/acnh@s0/unpack.tsv; printf '%s\n' acnh@s0 > $T/roots/acnh@现行
SD=$T/run/s2arm/vk_warm/0100AAAA00000000
cur() { cd -P $T/roots/$(cat $T/roots/acnh@现行) && pwd; }
S() { O=$(bash $T/repo/pack/stage_vk_warm_seed.sh acnh 2>&1); RC=$?; K=$(cat "$(cur)/.stage_vk_warm_seed" 2> /dev/null); }
# 一份真机程序清单（宿主格式手写：60 字节头 + 112 字节条目头 + 两段 0x100 的 c1 + FNV-1a；起始值照宿主 1469598103934665603）
prog() { python3 - "$1" "${2:-0}" <<'EOF'
import hashlib, os, struct, sys
d, corrupt = sys.argv[1], int(sys.argv[2])
cl = hashlib.sha256(b'closure-A').digest()[:8]
b = struct.pack('<IIII', 0x31575053, 3, 112, 1) + hashlib.sha256(b'profile-A').digest() + struct.pack('<i', 7) + cl
b += bytes(range(1, 33)) + struct.pack('<I', 3) + bytes(40) + b'\x03\x02\x00\x00' + struct.pack('<IIIIIIQ', 0x100, 0x100, 0, 0, 0, 1, 1) + bytes(0x200)
h = 1469598103934665603
for x in b: h = ((h ^ x) * 1099511628211) & (2**64 - 1)
os.makedirs(d, exist_ok=True)
open(os.path.join(d, 'programs-v1-' + cl.hex() + '.bin'), 'wb').write(b + struct.pack('<Q', h ^ corrupt))
EOF
}
DN=$(python3 -c 'import hashlib; print(hashlib.sha256(b"closure-A").hexdigest()[:16] + "-" + hashlib.sha256(b"profile-A").hexdigest()[:16])'); C16=${DN%%-*}
S; K1=$K
ck "① 没有来源：退 0（${RC}）、现行没有 warm-vk、记键（${K1}）" '[ $RC = 0 ] && [ ! -e "$(cur)/shaders/direct/warm-vk" ] && [ -n "$K1" ]'
prog $SD/dev1; S; K2=$K
ck "② 放一份真机清单：键变（${K1} → ${K2}）、现行 warm-vk/${DN}/programs-v1-${C16}.bin 与来源逐字节相同" '[ $RC = 0 ] && [ "$K2" != "$K1" ] && cmp -s "$(cur)/shaders/direct/warm-vk/$DN/programs-v1-$C16.bin" $SD/dev1/programs-v1-$C16.bin'
S; ck "③ 不变再跑：跳过、键不变" '[ $RC = 0 ] && [ "$K" = "$K2" ] && [[ "$O" == *跳过* ]]'
mv $SD/dev1 $SD/phone-b; S; ck "③ 同内容换子目录名：仍跳过" '[ $RC = 0 ] && [ "$K" = "$K2" ] && [[ "$O" == *跳过* ]]'
rm -rf $SD/phone-b; prog $SD/dev2 1; S
ck "④ 只剩一份坏文件：键变（${K2} → ${K}）、现行 warm-vk 删掉" '[ $RC = 0 ] && [ "$K" != "$K2" ] && [ ! -e "$(cur)/shaders/direct/warm-vk" ]'
K4=$K; prog $SD/dev3; printf 'import sys\nsys.exit("模拟工具坏了")\n' > $T/repo/android/tools/vk_warm_seed.py; S
ck "⑤ 工具失败：退 0、打【黄】、现行不动、键不变（${K}）" '[ $RC = 0 ] && [[ "$O" == *【黄】*首局预热种子这次没收*模拟工具坏了* ]] && [ "$K" = "$K4" ] && [ ! -e "$(cur)/shaders/direct/warm-vk" ]'
[ $bad = 0 ] && echo "首局预热种子阶段单测全过" || { echo "首局预热种子阶段单测有不通"; exit 1; }
