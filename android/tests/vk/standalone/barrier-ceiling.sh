#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
exec bash "$repo/android/tests/vk/standalone/external-dependency.sh" ceiling
