#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
if [ "$(uname -s)" = Darwin ]; then
    . android/tools/ndk_toolchain.sh
    ndk=$(s2a_ndk_resolve)
    tag=$(s2a_ndk_prebuilt_tag "$ndk")
    mkdir -p build/vuid-context-ndk
    cp -R "$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vulkan" "$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vk_video" build/vuid-context-ndk/
    mkdir -p build/vuid-context-ndk/sys
    cp "$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/sys/system_properties.h" build/vuid-context-ndk/sys/
    exec docker --context colima run --platform linux/amd64 --rm --pull=never --network=none \
        --user "$(id -u):$(id -g)" --mount "type=bind,source=$repo,target=/src" --workdir /src \
        --entrypoint bash s2a-vk-ci-parity:26.0.8-vvl-1.4.341 android/tests/run_vuid_context_queue.sh
fi
mkdir -p build/vuid-context-test
cflags=(-std=gnu11 -D_GNU_SOURCE -DS2A_ANDROID_DEBUG_ENV=1 -DS2A_VUID_CONTEXT_TEST=1 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter)
includes=(-I android/tests/vkhost -I build/vuid-context-ndk -I graphics/nvn/replay)
gate_flags=(-std=gnu11 -D__ANDROID__ -D_Nonnull= -D_Nullable= -D__unused= '-D__BIONIC_AVAILABILITY_GUARD(x)=0' -Wall -Wextra -Werror -I build/vuid-context-ndk -I android/native)
cc "${gate_flags[@]}" -DS2A_ANDROID_DEBUG_ENV=1 android/tests/vuid_context_gate_test.c -o build/vuid-context-test/gate-debug
cc "${gate_flags[@]}" android/tests/vuid_context_gate_test.c -o build/vuid-context-test/gate-release
build/vuid-context-test/gate-debug
build/vuid-context-test/gate-release
cc "${cflags[@]}" "${includes[@]}" android/tests/vuid_context_test.c android/native/direct/vuid_context.c -o build/vuid-context-test/context -lpthread
build/vuid-context-test/context
cc "${cflags[@]}" "${includes[@]}" -I android/native/nvn android/tests/vuid_context_queue_test.c android/native/direct/vuid_context.c -o build/vuid-context-test/queue -lpthread
build/vuid-context-test/queue
mkdir -p build/vuid-context-test/mutant
python3 - <<'PY'
from pathlib import Path
src = Path('android/native/nvn/nvn_vk_dq.inc').read_text()
old = 'h->op |= DQ_CONTEXT_FLAG;'
assert src.count(old) == 1
Path('build/vuid-context-test/mutant/nvn_vk_dq.inc').write_text(src.replace(old, '/* negative control: discard the association */'))
PY
cc "${cflags[@]}" "${includes[@]}" -I build/vuid-context-test/mutant -I android/native/nvn android/tests/vuid_context_queue_test.c android/native/direct/vuid_context.c -o build/vuid-context-test/mutant-queue -lpthread
set +e
build/vuid-context-test/mutant-queue > build/vuid-context-test/negative.log 2>&1
rc=$?
set -e
cat build/vuid-context-test/negative.log
if [ "$rc" -eq 0 ] || ! grep -q 'expected key=.*got kind=0' build/vuid-context-test/negative.log || ! grep -q 'observed=403 expected=403' build/vuid-context-test/negative.log; then
    echo '【红】负控未在队列归属断言处变红' >&2; exit 1
fi
echo 'vuid_context_queue: 负控如期变红'
