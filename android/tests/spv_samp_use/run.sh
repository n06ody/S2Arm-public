#!/bin/bash
# 一次性诊断（不进 PR）：生成 SPIR-V 夹具（glslangValidator -V / spirv-as），编单测并跑。用法：run.sh <输出目录>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=${1:?输出目录}; mkdir -p "$out"
for f in "$here"/*.frag; do glslangValidator -V -S frag -o "$out/$(basename "$f").spv" "$f" > /dev/null; done
for f in "$here"/*.spvasm; do spirv-as --target-env spv1.0 -o "$out/$(basename "$f").spv" "$f"; done
for m in img_oob load_oob ptr_short ptr_tail dup_id dup_dec; do python3 "$here/mut.py" "$out/asmfetch.spvasm.spv" "$out/bad_$m.spv" $m; done
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$out/spv_samp_use_test" "$here/../spv_samp_use_test.c"
"$out/spv_samp_use_test" "$here/cases.tsv" "$out"
