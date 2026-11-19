#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the active layer manifest before running shader-hazard controls."""
import json
import re
import sys
from pathlib import Path

KEY = "syncval_shader_accesses_heuristic"
MINIMUM = (1, 4, 341)


def declares(value):
    if isinstance(value, dict):
        return value.get("key") == KEY or any(declares(v) for v in value.values())
    if isinstance(value, list):
        return any(declares(v) for v in value)
    return False


def main():
    try:
        layer = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))["layer"]
        version = layer.get("api_version", "")
        match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\.\d+)?", version)
        if layer.get("name") != "VK_LAYER_KHRONOS_validation" or not match:
            raise ValueError("校验层身份或版本不可核实")
        if tuple(map(int, match.groups())) < MINIMUM or not declares(layer):
            raise ValueError(f"layer_api={version}，未满足版本与 {KEY} 能力要求")
    except (AttributeError, IndexError, OSError, ValueError, TypeError, KeyError) as exc:
        print(f"【红】同步校验能力不足（需 ≥1.4.341），未验证：{exc}；请用 CI 同版本容器（Mesa 26.0.8 / VVL 1.4.341）后重跑。", file=sys.stderr)
        return 75
    print(f"同步校验能力：layer_api={version}，已声明 {KEY}；仍须通过实际正负控。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
