#!/usr/bin/env bash
# Run on Linux; exercises the production affinity implementation with synthetic
# capacity files, real pthread inheritance and the fake AAudio API.
set -euo pipefail
H=$(cd "$(dirname "$0")/../../.." && pwd)
O=${1:?pass a private output directory}
mkdir -p "$O"
CXX=${CXX:-c++}; CC=${CC:-cc}
"$CXX" -std=c++17 -Wall -Wextra -Werror -DS2A_CAPACITY_TEST=1 "$H/android/native/capacity_affinity.cpp" "$H/android/tests/capacity/affinity_test.cpp" -pthread -Wl,--wrap=sched_getaffinity,--wrap=sched_setaffinity -o "$O/affinity"
"$O/affinity"
"$CXX" -std=c++17 -Wall -Wextra -Werror -DS2A_CAPACITY_TEST=1 -DS2A_CAPACITY_WRAP=1 "$H/android/native/capacity_affinity.cpp" "$H/android/tests/capacity/inheritance_test.cpp" -pthread -Wl,--wrap=pthread_create -o "$O/inheritance"
"$O/inheritance"
# The host has no Android Vulkan device. Exercise the production completion
# thread's startup on a real pthread with the same Android entry statements.
awk '
    /^static void \*worker_main\(void \*arg\) \{/ { startup = 1; next }
    startup && /struct nvn_gpu_vk \*vk = arg;/ { found = 1; exit }
    startup { print }
    END { if (!found) exit 1 }
' "$H/android/native/nvn/backend_vulkan.c" > "$O/vk_done_startup.inc"
"$CXX" -std=c++17 -Wall -Wextra -Werror -DS2A_CAPACITY_TEST=1 -DS2A_CAPACITY_WRAP=1 -DS2A_ANDROID_VK_BACKEND_NVK=1 -I"$O" "$H/android/native/capacity_affinity.cpp" "$H/android/tests/capacity/tiered_test.cpp" -pthread -Wl,--wrap=pthread_create -o "$O/tiered"
"$O/tiered" "$O/tiered-topology"
(
    ulimit -c 0
    set +e
    "$O/tiered" "$O/tiered-topology" --omit-audio-role > "$O/tiered-negative.log" 2>&1
    rc=$?
    set -e
    test "$rc" -eq 134
    grep -F '!CPU_ISSET(cpus[4], &actual) && !CPU_ISSET(cpus[3], &actual)' "$O/tiered-negative.log"
    echo 'PASS tiered negative: omitting audio registration leaves both main cores in its mask'
    set +e
    "$O/tiered" "$O/tiered-topology" --bind-done-background > "$O/tiered-done-negative.log" 2>&1
    rc=$?
    set -e
    test "$rc" -eq 134
    grep -F 'CPU_EQUAL(&actual, &completion_allowed)' "$O/tiered-done-negative.log"
    echo 'PASS tiered negative: marking completion background loses its inherited mask'
)
"$CC" -std=c11 -Wall -Wextra -Werror -I"$H/android/tests/capacity/fake" -c "$H/android/native/aaudio_sink.c" -o "$O/aaudio.o"
"$CC" -std=c11 -Wall -Wextra -Werror -c "$H/android/native/pcm_ring.c" -o "$O/pcm.o"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$H/android/tests/capacity/fake" -c "$H/android/tests/capacity/aaudio_fake.c" -o "$O/aaudio-fake.o"
"$CXX" -std=c++17 -Wall -Wextra -Werror -DS2A_CAPACITY_TEST=1 -DS2A_CAPACITY_WRAP=1 -I"$H/android/tests/capacity/fake" "$H/android/native/capacity_affinity.cpp" "$H/android/tests/capacity/aaudio_route_test.cpp" "$O/aaudio.o" "$O/pcm.o" "$O/aaudio-fake.o" -pthread -Wl,--wrap=pthread_create -o "$O/aaudio-route"
"$O/aaudio-route"
echo 'capacity Linux host tests passed'
