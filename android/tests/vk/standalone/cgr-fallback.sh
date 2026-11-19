#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
export S2A_CGR_NARROW=0
exec bash "$(dirname "$0")/cgr-narrow.sh"
