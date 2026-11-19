// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using S2Arm.Gfx.Widen;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace S2A.DirectCompiler
{
    // Refine the existing vec4-use analysis only for Ryujinx's proven
    // struct { vec4 data[N]; } Uniform ABI. null means unknown, never no reads.
    // The runtime, not this analyzer, decides which c0 ABI ranges it can supply.
    public static class DirectComputeC0Reads
    {
        // Declarations carry literals (e.g. capability 64), not memory operands.
        // Do not mistake those literals for forward-declared pointer IDs.
        private static bool Declaration(int op) => op is 3 or 4 or 5 or 6 or 7 or 8 or 10 or 11 or 14 or 15 or 16 or 17 or 43 or 59 or 71 or 72
            || (op >= 19 && op <= 39);

        public static int[][] Analyze(ReadOnlySpan<byte> spv, int set, int binding)
        {
            var coarse = SpirvCbufUse.Analyze(spv);
            if (coarse == null) return null;
            ReadOnlySpan<uint> w = MemoryMarshal.Cast<byte, uint>(spv);
            var bindings = new Dictionary<uint, uint>();
            var sets = new Dictionary<uint, uint>();
            var constants = new Dictionary<uint, uint>();
            var types = new Dictionary<uint, uint[]>();
            var strides = new Dictionary<uint, uint>();
            var offsets = new Dictionary<uint, uint>();
            var variables = new Dictionary<uint, uint>();
            for (int i = 5; i < w.Length;)
            {
                int n = (int)(w[i] >> 16), op = (int)(w[i] & 65535);
                if (op == 71 && n == 4)
                {
                    if (w[i + 2] == 33) bindings[w[i + 1]] = w[i + 3];
                    if (w[i + 2] == 34) sets[w[i + 1]] = w[i + 3];
                    if (w[i + 2] == 6) strides[w[i + 1]] = w[i + 3];
                }
                if (op == 72 && n == 5 && w[i + 2] == 0 && w[i + 3] == 35)
                    offsets[w[i + 1]] = w[i + 4];
                if (op >= 19 && op <= 33 && n >= 2)
                    types[w[i + 1]] = w.Slice(i, n).ToArray();
                if (op == 43 && n == 4) constants[w[i + 2]] = w[i + 3];
                if (op == 59 && n >= 4 && w[i + 3] == 2) variables[w[i + 2]] = w[i + 1];
                i += n;
            }
            var roots = variables.Keys.Where(id => bindings.GetValueOrDefault(id, uint.MaxValue) == (uint)binding &&
                sets.GetValueOrDefault(id, uint.MaxValue) == (uint)set).ToArray();
            if (roots.Length != 1) return null;
            uint root = roots[0];
            // Dynamic/unmodelled uses reported by the shared analysis cannot become [].
            // 同一 (set, binding) 才算 c0 的读法：不同描述符集里同 binding 号的其它 UBO
            // （如 set1/binding1 的动态索引槽）与 c0 无关，不能借它的 dynamic 判 c0 不可证。
            if (coarse.Any(u => u.Set == set && u.Binding == binding && u.Dynamic)) return null;
            bool Type(uint id, int op, int length, out uint[] t) =>
                types.TryGetValue(id, out t) && (t[0] & 65535) == op && t.Length == length;
            if (!Type(variables[root], 32, 4, out var pointer) || pointer[2] != 2 ||
                !Type(pointer[3], 30, 3, out var structure) || offsets.GetValueOrDefault(pointer[3], uint.MaxValue) != 0 ||
                !Type(structure[2], 28, 4, out var array) || strides.GetValueOrDefault(structure[2]) != 16 ||
                !Type(array[2], 23, 4, out var vector) || vector[3] != 4 ||
                !types.TryGetValue(vector[2], out var scalar) ||
                !(((scalar[0] & 65535) == 21 || (scalar[0] & 65535) == 22) && scalar[2] == 32) ||
                !constants.TryGetValue(array[3], out uint count)) return null;
            var pointers = new Dictionary<uint, (int Offset, int Size)>();
            var reads = new SortedSet<(int Offset, int Size)>();
            for (int i = 5; i < w.Length;)
            {
                int n = (int)(w[i] >> 16), op = (int)(w[i] & 65535);
                if (op == 65 || op == 66)
                {
                    if (n >= 4 && w[i + 3] == root)
                    {
                        if ((n != 6 && n != 7) || !constants.TryGetValue(w[i + 4], out uint member) || member != 0 ||
                            !constants.TryGetValue(w[i + 5], out uint index) || index >= count || index > int.MaxValue / 16)
                            return null;
                        uint component = 0;
                        if (n == 7 && (!constants.TryGetValue(w[i + 6], out component) || component >= 4)) return null;
                        int size = n == 7 ? 4 : 16;
                        if (!Type(w[i + 1], 32, 4, out var resultPointer) || resultPointer[2] != 2 ||
                            resultPointer[3] != (n == 7 ? vector[2] : array[2])) return null;
                        pointers[w[i + 2]] = ((int)(index * 16 + component * 4), size);
                    }
                    else if (n >= 4 && pointers.ContainsKey(w[i + 3])) return null;
                }
                else if (op == 61 && n >= 4 && pointers.TryGetValue(w[i + 3], out var range))
                {
                    if (w[i + 1] != (range.Size == 4 ? vector[2] : array[2])) return null;
                    reads.Add(range);
                }
                else if (!Declaration(op))
                {
                    // Unknown pointer escapes (copy, phi, parameter, whole-block load) fail closed.
                    // Literal coincidences may conservatively reject, never under-report reads.
                    for (int j = 1; j < n; j++)
                        if (w[i + j] == root || pointers.ContainsKey(w[i + j])) return null;
                }
                i += n;
            }
            // Audit after collecting every pointer as well: a loop OpPhi may refer
            // forward to a pointer defined in a later block.
            for (int i = 5; i < w.Length;)
            {
                int n = (int)(w[i] >> 16), op = (int)(w[i] & 65535);
                if (op == 65 || op == 66)
                {
                    if (n >= 4 && pointers.ContainsKey(w[i + 3])) return null;
                }
                else if (op == 61 && n >= 4 && pointers.TryGetValue(w[i + 3], out var range))
                {
                    if (w[i + 1] != (range.Size == 4 ? vector[2] : array[2])) return null;
                    reads.Add(range);
                }
                else if (op == 59 && n >= 5 && (w[i + 4] == root || pointers.ContainsKey(w[i + 4]))) return null;
                else if (!Declaration(op))
                    for (int j = 1; j < n; j++)
                        if (w[i + j] == root || pointers.ContainsKey(w[i + j])) return null;
                i += n;
            }
            return reads.Select(r => new[] { r.Offset, r.Size }).ToArray();
        }
    }
}
