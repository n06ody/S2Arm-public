#!/usr/bin/env bash
# 门禁路由：android/app/src/main/java/org/s2arm/android/SystemPower.java android/app/src/main/java/org/s2arm/android/SystemPowerSampler.java android/app/src/main/java/org/s2arm/android/SystemPowerRails.java android/tests/SystemPowerTest.java android/tests/SystemPowerSamplerTest.java android/tests/SystemPowerRailsTest.java android/tests/system_power_stubs/ android/tools/system_power_test.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-system-power.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
javac -encoding UTF-8 -d "$out" "$root/android/tests/system_power_stubs/android/content/"*.java "$root/android/tests/system_power_stubs/android/os/"*.java \
    "$root/android/tests/system_power_stubs/android/os/health/"*.java "$root/android/tests/system_power_stubs/android/annotation/"*.java \
    "$root/android/tests/SystemPowerTest.java" "$root/android/tests/SystemPowerSamplerTest.java" "$root/android/tests/SystemPowerRailsTest.java"
prod=("$root/android/app/src/main/java/org/s2arm/android/SystemPower.java" "$root/android/app/src/main/java/org/s2arm/android/SystemPowerSampler.java" "$root/android/app/src/main/java/org/s2arm/android/SystemPowerRails.java")
present=(); for src in "${prod[@]}"; do [ -f "$src" ] && present+=("$src"); done
if [ "${#present[@]}" -gt 0 ]; then javac -encoding UTF-8 -cp "$out" -d "$out" "${present[@]}"; fi
java -cp "$out" org.s2arm.android.SystemPowerTest
java -cp "$out" org.s2arm.android.SystemPowerSamplerTest
java -cp "$out" org.s2arm.android.SystemPowerRailsTest
