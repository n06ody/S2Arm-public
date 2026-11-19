#!/usr/bin/env bash
# Exact CI Mesa/VVL container; Vulkan and property names come from NDK 29.
# The reference-only mode calibrates the oracle; it makes no spill claim.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mode=${1:---reference-only}
case "$mode" in --reference-only|--candidate|--prepare) ;; *) echo 'usage: run_shared_spill.sh [--reference-only|--candidate]' >&2; exit 2;; esac
if [ "$(uname -s)" = Darwin ]; then
    out=${S2A_HOST_OUT_ROOT:?set S2A_HOST_OUT_ROOT to an output directory outside the checkout}/shared-spill-vk
    mkdir -p "$out" "$out/ndk" "$out/tmp" "$out/cache"
    . "$repo/android/tools/ndk_toolchain.sh"
    ndk=$(s2a_ndk_resolve); tag=$(s2a_ndk_prebuilt_tag "$ndk")
    grep -q '^Pkg.Revision = 29\.0\.14206865$' "$ndk/source.properties"
    cp -R "$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vulkan" "$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vk_video" "$out/ndk/"
    run_container() { docker --context colima run --platform linux/amd64 --rm --pull=never --network=none \
        --user "$(id -u):$(id -g)" --mount "type=bind,source=$repo,target=/src,readonly" \
        --mount "type=bind,source=$out,target=/out" --workdir /src \
        --env S2A_SPILL_VK_OUT=/out --env TMPDIR=/out/tmp --env HOME=/out --env XDG_CACHE_HOME=/out/cache \
        --entrypoint bash s2a-vk-ci-parity:26.0.8-vvl-1.4.341 android/tests/vk/run_shared_spill.sh "$1"; }
    if [ "$mode" = --candidate ]; then
        run_container --prepare
        dotnet run -c Release --project "$repo/graphics/tools/direct/tests/compiler" -- \
            --spill-spirv "$out/reference.spv" "$out/spill.spv" 4096 | tee "$out/selection.log"
        # 6144 + 4096 + 4096 字节，必须至少选两个数组，剩余 4096 恰好等于模拟设备上限。
        grep -q 'spill bytes/group=10240 remaining=4096 measured=4096 set=0 binding=1' "$out/selection.log"
        spirv-val --target-env vulkan1.1 "$out/spill.spv"
        if dotnet run -c Release --project "$repo/graphics/tools/direct/tests/compiler" -- \
            --spill-spirv "$out/unsupported.spv" "$out/unsupported-output.spv" 1024 > "$out/unsupported.log" 2>&1; then
            echo '【红】unsupported shared float unexpectedly accepted' >&2; exit 1
        fi
        cat "$out/unsupported.log"
        grep -q 'UNSUPPORTED_PROGRAM' "$out/unsupported.log"
        echo 'unsupported shared array: named rejection verified'
    fi
    run_container "$mode"
    spirv-val --target-env vulkan1.1 "$out/reference.spv"
    echo 'reference SPIR-V: validated on host'
    exit 0
fi
out=${S2A_SPILL_VK_OUT:?container output missing}
mesa=$(dpkg-query -W -f='${Version}' mesa-vulkan-drivers)
vvl=$(dpkg-query -W -f='${Version}' vulkan-validationlayers)
printf 'CI parity: mesa=%s validation=%s\n' "$mesa" "$vvl"
[ "$mesa" = 26.0.8-1ubuntu0.3 ] && [ "$vvl" = 1.4.341.0-1 ]
icd=$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' -print -quit)
[ -n "$icd" ]
export VK_DRIVER_FILES="$icd" VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 VK_LAYER_SETTINGS_PATH="$out"
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
glslc --target-env=vulkan1.1 android/tests/vk/shaders/shared_spill.comp -o "$out/reference.spv"
glslc --target-env=vulkan1.1 android/tests/vk/shaders/shared_spill_unsupported.comp -o "$out/unsupported.spv"
[ "$mode" != --prepare ] || exit 0
cc -std=c11 -O2 -Wall -Wextra -Werror -I "$out/ndk" android/tests/vk/t_shared_spill.c -lvulkan -o "$out/test"
if [ "$mode" = --reference-only ]; then
    "$out/test" "$out/reference.spv" "$out/reference.spv"
    echo 'reference calibration only: transformed candidate not tested'
else
    [ -f "$out/spill.spv" ] || { echo '【红】production spill candidate missing' >&2; exit 1; }
    "$out/test" "$out/reference.spv" "$out/spill.spv"
    inc=(-I "$out/ndk" -I android/tests/vkhost -I graphics/nvn -I graphics/nvn/replay
         -I graphics/nvn/gfx/nvn -I graphics/nvn/gfx/nvn/include -I android/native
         -I android/native/nvn -I android/native/direct -I runtime/include)
    src=(android/tests/vk/t_replay_shared_spill.c)
    for f in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$f"); done
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -lvulkan -lpthread -lm -o "$out/replay-test"
    set +e
    "$out/replay-test" "$out" > "$out/replay.log" 2>&1
    replay_rc=$?
    set -e
    cat "$out/replay.log"
    if [ "$replay_rc" != 0 ] || grep -Eq 'Validation Error|VUID-|SYNC-HAZARD' "$out/replay.log"; then
        echo '【红】replay spill GPU or validation failed' >&2; exit 1
    fi
    echo 'replay validation errors=0'
fi
