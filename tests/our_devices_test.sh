#!/bin/bash
# 跑局机清单「在用」列单测（pack/our_devices.tsv + pack/jobs/archs.py run_machines，约 1 秒）：bash tests/our_devices_test.sh
# 期望（不跑被测实现，awk 直接读表算）：缺省 iOS 出包对 = 表里平台 ios、在用 = 是 的（架构, 大版本）；在用 = 否 的机器不出；在用列不是 是 / 否 → 报红
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
if [ -f "$H/pack/our_devices.tsv" ]; then   # 真清单是维护方私有文件，公开仓没有
want=$(LC_ALL=C awk -F'\t' '!/^#/ && NF >= 6 && $4 == "ios" && $6 == "是" {print $3 "\t" $5}' "$H/pack/our_devices.tsv" | sort -u)
got=$(env -u S2A_PSO_ARCHS -u S2A_PSO_ARCHS_ALL -u S2A_PACK_PURPOSE python3 "$H/pack/jobs/archs.py" pairs ios pso 2> /dev/null | cut -f1,2 | sort -u)
ck "真清单：iOS 对「$(echo $got)」= 表里在用的「$(echo $want)」" '[ -n "$want" ] && [ "$got" = "$want" ]'
fi
printf '# t\nA1\t甲\tapplegpu_g18p\tios\t27\t是\t-\nA2\t乙\tapplegpu_g19p\tios\t27\t否\t-\n' > $T/d.tsv
o=$(S2A_OUR_DEVICES=$T/d.tsv env -u S2A_PSO_ARCHS python3 "$H/pack/jobs/archs.py" pairs ios pso 2> /dev/null | cut -f1 | tr '\n' ' ')
ck "在用 = 否 的 g19p 不出（${o}）" '[ "$o" = "applegpu_g18p " ]'
printf 'A1\t甲\tapplegpu_g18p\tios\t27\t备注\n' > $T/e.tsv
e=$(S2A_OUR_DEVICES=$T/e.tsv env -u S2A_PSO_ARCHS python3 "$H/pack/jobs/archs.py" pairs ios pso 2>&1); r=$?
ck "在用列不是 是 / 否 → 报红（rc=${r}）" '[ $r != 0 ] && grep -q "在用" <<< "$e"'
[ $bad = 0 ] && echo "our_devices 单测全过" || { echo "our_devices 单测有不通"; exit 1; }
