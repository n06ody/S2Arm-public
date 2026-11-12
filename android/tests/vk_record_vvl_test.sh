#!/usr/bin/env bash
# Run the production record decoder fixture on lavapipe with validation enabled.
# Enter via nvn_vk_real_teardown_test.sh so Mac uses the pinned Vulkan image.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
[ "$(uname -s)" = Linux ] || { echo 'use android/tests/nvn_vk_real_teardown_test.sh on macOS' >&2; exit 2; }
[ "${VK_INSTANCE_LAYERS:-}" = VK_LAYER_KHRONOS_validation ] || { echo 'Khronos validation required' >&2; exit 2; }
mkdir -p "$repo/build"
out=$(mktemp -d "$repo/build/vk-record-vvl.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
mkdir "$out/shaders"
for shader in tri.vert tri.frag fill.comp; do
    glslc --target-env=vulkan1.1 "$repo/android/tests/shaders/direct/$shader" -o "$out/shaders/$shader.spv"
done
inc=("$repo/android/tests/vkhost" "$repo/android/tests/hostvk" "$repo/android/native" \
     "$repo/android/native/direct" "$repo/android/native/nvn" "$repo/runtime/include" \
     "$repo/graphics/nvn/gfx/nvn" "$repo/graphics/nvn/gfx/nvn/include" \
     "$repo/graphics/nvn/gfx/tex" "$repo/graphics/nvn/third_party/vulkan-headers/include")
args=(); for dir in "${inc[@]}"; do args+=(-I "$dir"); done
sanitize=()
source="$repo/android/tests/vk_record_test.c"
clear_source="$repo/android/native/direct/vk_clear.c"
case "${1:-}" in
    '') ;;
    --release-negative)
        clear_source="$out/vk_clear_legacy.c"
        python3 - "$repo/android/native/direct/vk_clear.c" "$clear_source" <<'PY'
from pathlib import Path
import sys
src, dst = map(Path, sys.argv[1:])
text = src.read_text()
before = 'void s2a_vk_clear_fv_release_device(struct s2a_vk_device *d) {\n    if (!d || !d->device) return;'
after = 'void s2a_vk_clear_fv_release_device(struct s2a_vk_device *d) {\n    if (!clr_fv_q_on() || !d || !d->device) return;'
assert text.count(before) == 1
dst.write_text(text.replace(before, after))
PY
        ;;
    --legacy-negative)
        sanitize=(-DS2A_RECORD_DIAG_SIGNAL -fno-inline -rdynamic -no-pie)
        source="$out/legacy.c"
        python3 - "$repo" "$source" <<'PY'
from pathlib import Path
import sys
repo, dst = map(Path, sys.argv[1:])
text = (repo / 'android/tests/vk_record_test.c').read_text()
start = text.index('static void put_clear_tex(')
end = text.index('static void put_copy_t2t(', start)
part = text[start:end]
before = 'record.tex = (const tex_t *)(payload + off);'
assert part.count(before) == 1
part = part.replace(before, 'record.tex = (const tex_t *)(uintptr_t)0x1;')
text = text[:start] + part + text[end:]
text = text.replace('#include "../native/direct/vk_record.h"',
                    f'#include "{repo}/android/native/direct/vk_record.h"')
text = text.replace('#include "../native/direct/vk_clear.h"',
                    f'#include "{repo}/android/native/direct/vk_clear.h"')
dst.write_text(text)
PY
        ;;
    *) echo "unknown mode: $1" >&2; exit 2 ;;
esac
cc -std=gnu11 -O1 -g -ffunction-sections -fdata-sections "${sanitize[@]}" "${args[@]}" \
    "$source" "$repo/android/tests/vk_record_vvl_host.c" \
    "$repo"/android/native/direct/{vk_record,t2t_plan,buffer_texture_plan,bind_defer,vk_device,gs_path}.c "$clear_source" \
    "$repo"/android/native/nvn/{nvn_vk_format,nvn_vk_query}.c \
    -Wl,--gc-sections -l:libvulkan.so.1 -lpthread -lm -ldl -o "$out/vk_record"
set +e
if [ "${1:-}" = --release-negative ]; then
    stdbuf -oL -eL "$out/vk_record" "$out/shaders" >"$out/record.log" 2>&1
    rc=$?
else
    stdbuf -oL -eL "$out/vk_record" "$out/shaders" 2>&1 | tee "$out/record.log"
    rc=${PIPESTATUS[0]}
fi
set -e
if [ "${1:-}" = --release-negative ]; then
    count=$(grep -c 'Validation Error.*VUID-vkDestroyDevice-device-05137' "$out/record.log" || true)
    other=$(grep -iE 'Validation Error|VUID-|UNASSIGNED-|SYNC-HAZARD' "$out/record.log" | grep -vc 'VUID-vkDestroyDevice-device-05137' || true)
    if [ "$rc" -ne 0 ] || [ "$count" -ne 12 ] || [ "$other" -ne 0 ] || ! grep -q '^vk-record: ok$' "$out/record.log"; then
        cat "$out/record.log" >&2
        echo "release negative control failed: rc=$rc expected=12 actual=$count unrelated=$other" >&2
        exit 1
    fi
    echo 'PASS negative control: gate-dependent release leaves exactly 12 device children'
    exit 0
fi
if [ "${1:-}" = --legacy-negative ]; then
    trace=$(sed -n 's/.*\[\(0x[0-9a-f]*\)\].*/\1/p' "$out/record.log" | xargs -r addr2line -f -e "$out/vk_record")
    if [ "$rc" -ne 139 ] || ! grep -q 'record fixture SIGSEGV address=' "$out/record.log" ||
       ! grep -q '^find_texture$' <<< "$trace"; then
        printf '%s\n' "$trace" >&2
        echo 'legacy pointer control did not fail in find_texture' >&2
        exit 1
    fi
    echo 'PASS negative control: legacy pointer faults in find_texture'
    exit 0
fi
[ "$rc" -ne 0 ] || ! grep -qiE 'Validation Error|VUID-|UNASSIGNED-|SYNC-HAZARD' "$out/record.log" || {
    echo 'record fixture produced validation errors' >&2; exit 1;
}
exit "$rc"
