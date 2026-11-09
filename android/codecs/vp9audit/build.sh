#!/usr/bin/env bash
# vp9audit：宿主侧 VP9 可表示性审计（测量工具，不进产品）。解一段 IVF（编码器默认参数生成、
# 无游戏资源），按 McVp9Backend 同款判门把每帧归桶（decoded/hidden/header/prevMv/probs/size），
# 报覆盖率分布——「占位压缩头」形态下 AMediaCodec 路能原生解多少。
# 用法：build.sh [out]   → 产出 <out>/vp9audit（缺省 build/vp9audit）；需要 sources 缓存里有
# 钉住版 libvpx 源码包（android/codecs/build.sh --prepare-sources 或正常构建会自动取）。
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/../../.." && pwd -P)
out=${1:-$repo_dir/build/vp9audit}
cache_root=${S2A_BUILD_CACHE:-${HOME:?}/.cache/s2arm-build}
line=$(grep -m1 -E '^VER=[0-9.]+; SHA=[0-9a-f]{64}$' "$repo_dir/services/vpx/build.sh") || { echo 'services/vpx/build.sh no VER/SHA'; exit 1; }
eval "$line"
src_tar=$cache_root/sources/$SHA/libvpx-$VER.tar.gz
[ -f "$src_tar" ] || { echo "missing $src_tar — 先跑 android/codecs/build.sh --prepare-sources"; exit 1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src" "$work/obj" "$out"
tar xzf "$src_tar" -C "$work/src"
mv "$work/src"/libvpx-* "$work/src/libvpx"
host_target=$(cc -dumpmachine | sed -E 's/^(aarch64|arm64)-apple-darwin.*/arm64-darwin23-gcc/;s/^x86_64-apple-darwin.*/x86_64-darwin23-gcc/')
host_target=${host_target:-generic-gnu}
(cd "$work/obj" && CC=cc "$work/src/libvpx/configure" --target="$host_target" \
  --disable-vp8-encoder --disable-vp9-encoder --enable-vp8-decoder --enable-vp9-decoder \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-webm-io \
  --disable-libyuv --enable-static --disable-shared > configure.log 2>&1 && make -j"$(sysctl -n hw.ncpu)" > make.log 2>&1) \
  || { echo "libvpx host build failed — 看 $work/obj/*.log"; exit 1; }
cc -std=c11 -O2 -I"$work/src/libvpx" -I"$work/obj" -o "$out/vp9audit" \
  "$repo_dir/android/codecs/vp9audit/vp9audit.c" "$work/obj/libvpx.a" -lm
echo "$out/vp9audit"
