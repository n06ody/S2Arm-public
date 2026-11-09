#!/usr/bin/env bash
# 门禁路由：android/app/src/main/java/org/s2arm/android/FrameRateVote.java android/tests/FrameRateVoteTest.java android/tools/frame_rate_vote_test.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-frame-vote.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
javac -d "$out" "$root/android/tests/frame_vote_stubs/android/os/"*.java "$root/android/tests/frame_vote_stubs/android/view/"*.java "$root/android/tests/FrameRateVoteTest.java"
src="$root/android/app/src/main/java/org/s2arm/android/FrameRateVote.java"
if [ -f "$src" ]; then javac -cp "$out" -d "$out" "$src"; fi
java -cp "$out" org.s2arm.android.FrameRateVoteTest
