#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Build the Vulkan 1.1 hybrid BC7 encoder from Granite and Arm tables."""

import argparse
from pathlib import Path
import re
import struct
import subprocess

from generate import HERE, read_table, rows

GRANITE = HERE.parents[3] / "graphics/nvn/third_party/granite/decode"


def astc_mask(seed):
    # Arm astcenc select_partition, specialized to 4x4, Z=0, two partitions.
    seed += 1024
    word = seed ^ (seed >> 15)
    word = (word * 0xEEDE0891) & 0xFFFFFFFF
    word ^= word >> 5
    word = (word + (word << 16)) & 0xFFFFFFFF
    word ^= word >> 7
    word ^= word >> 3
    word = (word ^ (word << 6)) & 0xFFFFFFFF
    word ^= word >> 17
    shifts = (4 if seed & 2 else 5, 5)
    if not seed & 1:
        shifts = shifts[::-1]
    a, b, c, d = [((word >> (4 * i)) & 15) ** 2 >> shifts[i % 2] for i in range(4)]
    mask = 0
    for i in range(16):
        x, y = 2 * (i % 4), 2 * (i // 4)
        if ((a * x + b * y + (word >> 14)) & 63) < ((c * x + d * y + (word >> 10)) & 63):
            mask |= 1 << i
    return mask


def partition_tables(bc7):
    masks = {}
    for seed in range(1024):
        mask = astc_mask(seed)
        if mask not in (0, 0xFFFF):
            # Keep one seed for each shape regardless of partition labels.
            masks.setdefault(mask ^ (0xFFFF if mask & 1 else 0), seed | (mask << 10))

    remap = []
    exact_count = 0
    for mask in bc7:
        ordered = sorted(masks, key=lambda m: (min((m ^ mask).bit_count(), 16 - (m ^ mask).bit_count()), m))
        remap.extend(masks[m] for m in ordered[:2])
        exact_count += ordered[0] in (mask, mask ^ 0xFFFF)
    assert exact_count == 30
    return remap, [masks[0xFF00], masks[0xCCCC]]


def granite_tables(lib):
    result = {}
    for subsets in (2, 3):
        for prefix in ("partition", "anchor"):
            name = f"{prefix}_table{subsets}"
            kind = "ivec2" if name == "anchor_table3" else "int"
            declaration = re.search(rf"const {kind} {name}\[64\] = {kind}\[\]\((.*?)\);", lib, re.S)
            body = declaration[1]
            if prefix == "partition":
                width = subsets - 1
                values = [sum(int(bit) << (width * i) for i, bit in enumerate(row.split(",")))
                          for row in re.findall(rf"P{subsets}\(([0-2, ]+)\)", body)]
            elif subsets == 3:
                values = [int(a) | int(b) << 4 for a, b in re.findall(r"ivec2\((\d+), (\d+)\)", body)]
            else:
                values = list(map(int, re.findall(r"\d+", body)))
            assert len(values) == 64, name
            result[f"packed_{name}"] = values
            lanes = "ivec2(word & 15u, word >> 4u)" if kind == "ivec2" else "int(word)"
            lookup = f"{kind} {name}(int i) {{ uint word = packed_{name}(i); return {lanes}; }}"
            lib = lib.replace(declaration[0], lookup)
            lib = re.sub(rf"\b{name}\[([^\]]+)\]", rf"{name}(\1)", lib)
    return lib, result


def tables(out):
    source = (GRANITE / "bc7.comp").read_text()
    lib = source[source.index("const int weight_table2"):source.index("void main()")]
    lib, data = granite_tables(lib)
    cases = "\n".join(f"    case {mode}: return decode_bc7_mode{mode}(block, pixel);" for mode in range(8))
    lib += f"""DecodedInterpolation decode_bc7(uvec4 block, int mode, int pixel)
{{
    switch (mode) {{
{cases}
    default: return DecodedInterpolation(uvec4(0), uvec4(0), 0, 0, 0);
    }}
}}
"""
    license_text = source[source.index("/*"):source.index("*/") + 2]
    (out / "bc7_decode.glsl").write_text(license_text + '\n#include "bitextract.h"\n' + lib)

    for count in (16, 40, 48):
        table = read_table("astcenc_quantization.cpp", f"color_scrambled_pquant_to_uquant_q{count}", count)
        closest = [min(range(count), key=lambda i: (abs(table[i] - value), table[i])) for value in range(256)]
        symbols = [table[i] << 8 | i for i in closest]
        data[f"q{count}"] = [a | (b << 16) for a, b in zip(symbols[::2], symbols[1::2])]
    for name, count in (("trits", 243), ("quints", 125)):
        table = read_table("astcenc_integer_sequence.cpp", f"integer_of_{name}", count)
        data[name] = [int.from_bytes(bytes(table[i:i + 4]), "little") for i in range(0, count, 4)]
    data["bc7_remap"], data["axis_partitions"] = partition_tables(data["packed_partition_table2"])
    lines = ["layout(set = 0, binding = 2, std430) readonly buffer Tables { uint words[]; } tables;"]
    words = []
    for name, values in data.items():
        lines.append(f"#define {name}(i) tables.words[{len(words)}u + uint(i)]")
        words.extend(values)
    (out / "hybrid_tables.glsl").write_text("\n".join(lines) + "\n")
    (out / "hybrid_tables.bin").write_bytes(struct.pack(f"<{len(words)}I", *words))
    return words


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--glslc", default="glslc")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--tables-only", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(exist_ok=True, parents=True)
    table_words = tables(args.out)
    if args.tables_only:
        return
    spv = args.out / "hybrid.spv"
    subprocess.run([args.glslc, "--target-env=vulkan1.1", "-O", "-I", str(args.out), "-I", str(GRANITE),
                    str(HERE / "hybrid.comp"), "-o", str(spv)], check=True)
    words = [word for (word,) in struct.iter_unpack("<I", spv.read_bytes())]
    content = "\n".join(["// SPDX-License-Identifier: Apache-2.0",
                         "// Generated by bc7_astc_shaders/generate_hybrid.py.",
                         "#pragma once", "#include <stdint.h>", "static const uint32_t nvk_bc7_hybrid_spv[] = {",
                         *rows(words, 8, lambda n: f"0x{n:08x}u"), "};",
                         "static const uint32_t nvk_bc7_hybrid_tables[] = {",
                         *rows(table_words, 8, lambda n: f"0x{n:08x}u"), "};", ""])
    header = HERE.parent / "backend_vulkan_bc7_hybrid_spv.h"
    if args.check:
        if header.read_text() != content:
            raise SystemExit(f"{header}: stale generated file")
    else:
        header.write_text(content)


if __name__ == "__main__":
    main()
