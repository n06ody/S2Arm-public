#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
mode=${1:-}
case "$mode" in ''|--negative-size|--negative-zero|--negative-transform|--negative-revoke) ;; *) exit 64 ;; esac
out="$repo/build/vk-wsi-extent"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_wsi_extent.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c \
         android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/replay/nvn_unimpl.c; do
    src+=("$repo/$f")
done
binary="$out/test"; log="$out/run.log"
if [ -n "$mode" ]; then
    python3 - "$repo/android/native/nvn" "$out" "$mode" <<'PY'
import pathlib, sys
root, out, mode = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
source = (root / 'backend_vulkan_wsi.inc').read_text()
if mode == '--negative-size':
    before = 'ext.width != s->extent.width || ext.height != s->extent.height;'
    after = '0;'
elif mode == '--negative-zero':
    before = 'if (!ext.width || !ext.height || !caps->maxImageExtent.width || !caps->maxImageExtent.height)'
    after = 'if (0 && (!ext.width || !ext.height || !caps->maxImageExtent.width || !caps->maxImageExtent.height))'
elif mode == '--negative-revoke':
    before = 'int unavailable = s->gen ? NVN_GPU_E_OUTDATED : NVN_GPU_E_REVOKED;'
    after = 'int unavailable = NVN_GPU_E_REVOKED;'
else:
    before = 'const uint32_t transform = (uint32_t)s->pretransform;'
    after = 'const uint32_t transform = s->pretransform == 2u ? 8u : s->pretransform == 8u ? 2u : 1u;'
assert source.count(before) == 1, 'WSI mutation anchor changed'
(out / 'wsi_mutant.inc').write_text(source.replace(before, after))
backend = (root / 'backend_vulkan.c').read_text()
anchor = '#include "backend_vulkan_wsi.inc"'
assert backend.count(anchor) == 1
(out / 'backend_mutant.c').write_text(backend.replace(anchor, '#include "wsi_mutant.inc"'))
PY
    src[1]="$out/backend_mutant.c"
    binary="$out/test-${mode#--}"; log="$out/${mode#--}.log"
fi
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--wrap=vkGetPhysicalDeviceSurfaceCapabilitiesKHR -Wl,--wrap=vkCreateSwapchainKHR \
    -Wl,--wrap=vkQueuePresentKHR -Wl,--wrap=vkGetSwapchainImagesKHR -Wl,--gc-sections -lvulkan -lpthread -lm -o "$binary"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
set +e
timeout 120 "$binary" > "$log" 2>&1
rc=$?
set -e
if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|fixture failure' "$log"; then cat "$log"; exit 1; fi
if [ "$mode" = --negative-transform ]; then
    [ "$rc" = 1 ] && grep -q 'wsi-extent cases=19 failures=0' "$log" || { cat "$log"; exit 1; }
    [ "$(grep -c '^wsi-pixels transform=0x1 .*bad=0$' "$log")" = 2 ] &&
    [ "$(grep -c '^wsi-pixels transform=0x8 crop=2 .*bad=0$' "$log")" = 2 ] &&
    [ "$(grep -cE '^wsi-pixels transform=0x[248] .*bad=[1-9][0-9]*$' "$log")" = 7 ] || { cat "$log"; exit 1; }
    echo 'wsi-extent --negative-transform: PASS (7 wrong rotated frames, 4 controls, validation clean)'
    exit 0
fi
if [ -n "$mode" ]; then
    expected=2; names=(zero-extent zero-restore)
    if [ "$mode" = --negative-size ]; then expected=3; names=(undefined-resize clamped-resize fixed-resize); fi
    if [ "$mode" = --negative-revoke ]; then names=(zero-restore zero-window-restore); fi
    if [ "$rc" != 1 ] || ! grep -q "wsi-extent cases=19 failures=$expected" "$log" ||
       ! grep -q 'wsi-pixels cases=11 bad=0' "$log"; then cat "$log"; exit 1; fi
    for name in "${names[@]}"; do grep -q "wsi-extent case=$name bad=1" "$log" || { cat "$log"; exit 1; }; done
    echo "wsi-extent $mode: PASS (expected $expected behavior failures; remaining controls pass; validation clean)"
    exit 0
fi
cat "$log"
[ "$rc" = 0 ] || exit "$rc"
grep -q 'wsi-extent cases=19 failures=0' "$log"
grep -q 'wsi-pixels cases=11 bad=0' "$log"
echo '【绿】WSI extent: 19 cases, 11 readbacks, validation 0 errors'
for mutant in size zero transform revoke; do bash "$0" "--negative-$mutant"; done
