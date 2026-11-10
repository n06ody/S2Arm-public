#!/bin/bash
# #604 real production presenter, GPU-generated inputs, independent pixel checks.
# M3/M4 x four AA modes x three filters (MetalFX: 10-30, spatial upscale on the present). No game files or host/render substitutes.
# PRESENT_WIDEN_LIB may select a prepared production Widen library.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root"
source "$root/tools/toolchain.sh"
out=${PRESENT_SETTINGS_BUILD:-$root/build/tests/present-settings}
[[ "$out" = /* ]] || out="$root/$out"
mkdir -p "$out/host"
if [[ -n ${PRESENT_WIDEN_LIB:-} ]]; then
    widen=$PRESENT_WIDEN_LIB
    [[ "$widen" = /* ]] || widen="$root/$widen"
    [[ -r "$widen" ]] || { printf 'Missing production Widen: %s\n' "$widen" >&2; exit 2; }
else
    widen="$out/deps/tests/nvn/widen/S2Arm.Gfx.Widen.dylib"
    make -j1 S2A_CS=0 BUILD="$out/deps" "$widen"
fi
make -j1 -C "$root/graphics/nvn" mac
make -j1 -C "$root/graphics/metal" mac OUT="$out/gal"
make -j1 -C "$root/graphics/dsx" mac OUT="$out/dsx"   # 直连翻译库（翻译段 + SPIRV-Cross，11-08 从 libnxgal 拆出）
nvn="$root/build/nvn/mac/libnvn.a"
gal="$out/gal/mac/libnxgal.a"
dsx="$out/dsx/mac/libdsx.a"
common=(-O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -I"$root/runtime/include" -I"$root/runtime/ns2pro/include" -DNS2_SCAN_SECONDS=60)   # Mac 前端源文件含 Switch 2 Pro 连接（macos/ns2_controller.c、runtime/ns2pro，#277），同 launchers/macos/build.mk
objects=()
for src in "$root"/runtime/src/*.c "$root"/runtime/src/*.S "$root/platform/apple/plat_apple.c" "$root/graphics/track/track.c" "$root/graphics/track/glue.c"; do
    rel=${src#"$root"/}
    [[ "$rel" != runtime/src/host_main.c ]] || continue
    object="$out/host/${rel//\//_}.o"
    xcrun --sdk macosx clang "${common[@]}" -c "$src" -o "$object"
    objects+=("$object")
done
front_srcs=$(printf '%s\n' 'present-settings-front:;@echo $(HOST_SRCS)' | make -s -f "$root/launchers/macos/build.mk" -f - present-settings-front S2A_MAC_FRONTEND=1)
read -r -a front_srcs <<< "$front_srcs"
[[ ${#front_srcs[@]} -gt 0 ]] || { echo 'Production Mac source list is empty' >&2; exit 2; }
for rel in "${front_srcs[@]}"; do
    object="$out/host/front_${rel//\//_}.o"
    xcrun --sdk macosx clang "${common[@]}" -fobjc-arc -c "$root/$rel" -o "$object"
    objects+=("$object")
done
weak_output=$(printf '%s\n' 'present-settings-weak:;@echo $(NVN_TEST_WEAK)' | make -s -f "$root/runtime/build.mk" -f - present-settings-weak)
read -r -a weak_flags <<< "$weak_output"
xcrun --sdk macosx clang "${common[@]}" -std=gnu11 -fobjc-arc \
    -I"$root/graphics/nvn/gfx/nvn" -I"$root/graphics/nvn/gfx/nvn/include" -I"$root/graphics/nvn/host" \
    "$root/graphics/nvn/tests/t_present_settings.m" "${objects[@]}" "$nvn" "$gal" "$dsx" "$widen" -o "$out/t_present_settings" \
    -Wl,-dead_strip -Wl,-export_dynamic -Wl,-u,_s2a_mac_front_layer -Wl,-u,_nxgal_gpu_hold "${weak_flags[@]}" \
    -Wl,-rpath,"$(dirname "$widen")" -framework AppKit -framework GameController -framework CoreHaptics -framework CoreBluetooth -framework IOKit -framework Metal -framework Foundation \
    -framework QuartzCore -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -lc++
shasum -a 256 "$root/graphics/nvn/tests/t_present_settings.m" "$root/graphics/nvn/tests/t_present_settings.sh" \
    "${objects[@]}" "$nvn" "$gal" "$dsx" "$widen" "$out/t_present_settings" > "$out/link-inputs.sha256"

# Compile diagnostics while another real game owns the screen.
if [[ ${PRESENT_SETTINGS_BUILD_ONLY:-0} == 1 ]]; then
    printf 'present settings built without launching UI: %s\n' "$out/t_present_settings"
    exit 0
fi

# 各格 edge 图跨跑残留：格没跑成时旧图会让 distinctions 假绿——先清
rm -f "$out"/mtl?-*/edge-*.bgra
bad=0; unproven=0; unpr=' '
for backend in mtl3 mtl4; do
    for aa in None SmaaMedium SmaaHigh; do
        for filter in Nearest Bilinear MetalFX; do
            name="$backend-$aa-$filter"
            runtime="$out/$name"
            mkdir -p "$runtime"
            run_env=(env -u NXGAL_BE -u MTL_SHADER_VALIDATION -u MTL_SHADER_VALIDATION_DEFAULT_STATE \
                -u MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING -u S2A_INPUT_CONFIG -u DM_NVN_SHOT -u DM_NVN_REGLUM \
                S2A_GFX=direct S2A_GAME_DATA="$runtime" S2A_DOCS="$runtime" S2A_GAME_NAME="S2Arm #604 $name" \
                S2A_INPUT_GUIDE_PREFS="$runtime/input-guide.json" DM_PRESENT_LINK=0 S2A_WIDEN_NARROW=1 \
                S2A_ANTI_ALIASING="$aa" S2A_SCALING_FILTER="$filter" S2A_RES_SCALE=1 S2A_FPS=0 \
                MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=assert)
            [[ "$backend" != mtl3 ]] || run_env+=(NXGAL_BE=mtl3)
            # macOS 26 的 Metal 调试层与 Metal 4 + MetalFX 不兼容（11-07 本机 26.4.1 实测：MetalFX 在 MTL4DebugRenderCommandEncoder 上调
            #   globalTraceObjectID → unrecognized selector，进程 abort 134；Xcode 26.6 / 27.1 编都一样；不开调试层 3/3 全过 69/69；macOS 27 上开调试层全过）。
            #   这三格在 macOS 27 以下不开调试层跑，判据不变，并打一行说明
            if [[ "$backend" == mtl4 && "$filter" == MetalFX ]] && [ "$(sw_vers -productVersion | cut -d. -f1)" -lt 27 ]; then
                nodbg=(); for e in "${run_env[@]}"; do case $e in MTL_DEBUG_LAYER=*|MTL_DEBUG_LAYER_ERROR_MODE=*) ;; *) nodbg+=("$e");; esac; done; run_env=("${nodbg[@]}")
                echo "说明 ${name}：macOS $(sw_vers -productVersion) 的 Metal 调试层不支持 Metal 4 + MetalFX，本格不开调试层（macOS 27 起照开）"; fi
            status=0
            "${run_env[@]}" "$out/t_present_settings" "$runtime" > "$runtime/run.log" 2>&1 || status=$?
            # 77 = 宿主显示环境未证（屏睡/非活动 Space）：记未证不计红，见 t_present_settings.m 探针
            if [[ "$status" == 77 ]]; then unpr+="$name "; unproven=$((unproven+1)); printf 'UNPROVEN %s（宿主显示环境未证）\n' "$name"; continue; fi
            ok=1
            [[ "$status" == 0 ]] || ok=0
            if [[ "$backend" == mtl3 ]]; then expected='direct backend: Metal 3 (forced by NXGAL_BE=mtl3)'
            else expected='direct backend: Metal 4 (GPU in the Metal 4 family)'; fi
            /usr/bin/grep -Fq "$expected" "$runtime/run.log" || ok=0
            /usr/bin/grep -Eq '^present settings: [0-9]+/[0-9]+ passed;' "$runtime/run.log" || ok=0
            # MetalFX: the 67x53 edge scaled to 134x106 must have gone through the spatial scaler, on the backend under test
            if [[ "$filter" == MetalFX ]]; then
                if [[ "$backend" == mtl3 ]]; then fx='MetalFX 空间放大：67x53 → 134x106（Metal 3）'; else fx='MetalFX 空间放大：67x53 → 134x106（Metal 4）'; fi
                /usr/bin/grep -Fq "$fx" "$runtime/run.log" || ok=0
            fi
            # This graphics-only process has no game NPDM and requests no guest
            # address window. Keep that expected production diagnostic in the
            # raw log; every other red diagnostic still rejects this case.
            if /usr/bin/grep -Ein '^FAIL |command buffer.*error|GPU Address Fault|PageFault|Page Fault|FATAL|failed assertion|Metal Validation Error|\[红\]|【红】' "$runtime/run.log" | \
                /usr/bin/grep -Fv '【红】地址窗口：读不到本款 main.npdm（可执行文件同目录 / 模块目录），不知道要多大，不占窗口'; then ok=0; fi
            /usr/bin/grep -E '^PASS .*coverage|^PASS .*pixel-center|^present settings:' "$runtime/run.log" || true
            if [[ "$ok" == 1 ]]; then printf 'PASS %s\n' "$name"
            else printf 'FAIL %s (exit=%s, log=%s)\n' "$name" "$status" "$runtime/run.log" >&2; bad=$((bad+1)); fi
        done
    done
done
distinctions=0; dskip=0
for backend in mtl3 mtl4; do
    modes=(None SmaaMedium SmaaHigh)
    for ((i=0;i<3;i++)); do
        for ((j=i+1;j<3;j++)); do
            a="$out/$backend-${modes[i]}-Nearest/edge-native.bgra"
            b="$out/$backend-${modes[j]}-Nearest/edge-native.bgra"
            if [[ "$unpr" == *" $backend-${modes[i]}-Nearest "* || "$unpr" == *" $backend-${modes[j]}-Nearest "* ]]; then dskip=$((dskip+1))
            elif [[ -s "$a" && -s "$b" ]] && ! cmp -s "$a" "$b"; then distinctions=$((distinctions+1))
            else printf 'FAIL %s AA outputs not distinct: %s / %s\n' "$backend" "${modes[i]}" "${modes[j]}" >&2; fi
        done
        a="$out/$backend-${modes[i]}-Nearest/edge-scaled.bgra"
        b="$out/$backend-${modes[i]}-Bilinear/edge-scaled.bgra"
        if [[ "$unpr" == *" $backend-${modes[i]}-Nearest "* || "$unpr" == *" $backend-${modes[i]}-Bilinear "* ]]; then dskip=$((dskip+1))
        elif [[ -s "$a" && -s "$b" ]] && ! cmp -s "$a" "$b"; then distinctions=$((distinctions+1))
        else printf 'FAIL %s %s filters produce identical pixels\n' "$backend" "${modes[i]}" >&2; fi
        a="$out/$backend-${modes[i]}-MetalFX/edge-scaled.bgra"
        b="$out/$backend-${modes[i]}-Bilinear/edge-scaled.bgra"
        if [[ "$unpr" == *" $backend-${modes[i]}-MetalFX "* || "$unpr" == *" $backend-${modes[i]}-Bilinear "* ]]; then dskip=$((dskip+1))
        elif [[ -s "$a" && -s "$b" ]] && ! cmp -s "$a" "$b"; then distinctions=$((distinctions+1))
        else printf 'FAIL %s %s MetalFX and Bilinear produce identical pixels\n' "$backend" "${modes[i]}" >&2; fi
    done
done
printf 'present settings matrix: %d/18 passed, %d unproven (logs/images: %s)\n' "$((18-bad-unproven))" "$unproven" "$out"
printf 'present settings output distinctions: %d/18 passed, %d skipped\n' "$distinctions" "$dskip"
if ((unproven)); then printf 'present settings: %d cells 未证（宿主显示环境不给真上屏位）\n' "$unproven"; fi
[[ "$bad" == 0 && $((distinctions+dskip)) == 18 ]]   # 每个后端：3 种模式两两 3 对 + 每模式最近邻对双线性 3 次 + MetalFX 对双线性 3 次 = 9，两个后端 18；未证格的对子计 skip 不判（10-30 去掉 FXAA 前 4 种模式是 28）
