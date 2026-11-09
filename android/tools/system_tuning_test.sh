#!/usr/bin/env bash
# 门禁路由：android/app/src/main/java/org/s2arm/android/SystemTuning.java android/tests/SystemTuningTest.java android/tools/system_tuning_test.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-system-tuning.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
javac -d "$out" "$root/android/tests/system_tuning_stubs/android/app/"*.java "$root/android/tests/system_tuning_stubs/android/content/"*.java \
    "$root/android/tests/system_tuning_stubs/android/os/"*.java "$root/android/tests/system_tuning_stubs/android/view/"*.java \
    "$root/android/tests/system_tuning_stubs/org/s2arm/android/"*.java "$root/android/tests/SystemTuningTest.java"
prod=("$root/android/app/src/main/java/org/s2arm/android/SystemTuning.java" "$root/android/app/src/main/java/org/s2arm/android/FrameRateVote.java")
present=(); for src in "${prod[@]}"; do [ -f "$src" ] && present+=("$src"); done
if [ "${#present[@]}" -gt 0 ]; then javac -cp "$out" -d "$out" "${present[@]}"; fi
java -cp "$out" org.s2arm.android.SystemTuningTest
