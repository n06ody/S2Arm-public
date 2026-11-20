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
    // boundDynamic（只给图形阶段）：下标不是常量时按取值上下界算读集——证得下标落在 [lo, hi] 且 hi < N，
    //   就把 data[lo..hi] 整段（每个 vec4 整个）记为读；证不了照旧 null。计算阶段的消费端只收已知驱动字段的小区间，不开。
    public static class DirectComputeC0Reads
    {
        // Declarations carry literals (e.g. capability 64), not memory operands.
        // Do not mistake those literals for forward-declared pointer IDs.
        private static bool Declaration(int op) => op is 3 or 4 or 5 or 6 or 7 or 8 or 10 or 11 or 14 or 15 or 16 or 17 or 43 or 59 or 71 or 72
            || (op >= 19 && op <= 39);

        public static int[][] Analyze(ReadOnlySpan<byte> spv, int set, int binding, bool boundDynamic = false)
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
            if (!boundDynamic && coarse.Any(u => u.Set == set && u.Binding == binding && u.Dynamic)) return null;
            // 开 boundDynamic 时粗分析的 dynamic 不直接判未知：它的来源（非常量下标 / 整块加载 / 复制 / 当参数传）
            //   下面逐条重判——非常量下标走取值界，其余照旧 escape / chain:* 判 null。
            bool Type(uint id, int op, int length, out uint[] t) =>
                types.TryGetValue(id, out t) && (t[0] & 65535) == op && t.Length == length;
            if (!Type(variables[root], 32, 4, out var pointer) || pointer[2] != 2 ||
                !Type(pointer[3], 30, 3, out var structure) || offsets.GetValueOrDefault(pointer[3], uint.MaxValue) != 0 ||
                !Type(structure[2], 28, 4, out var array) || strides.GetValueOrDefault(structure[2]) != 16 ||
                !Type(array[2], 23, 4, out var vector) || vector[3] != 4 ||
                !types.TryGetValue(vector[2], out var scalar) ||
                !(((scalar[0] & 65535) == 21 || (scalar[0] & 65535) == 22) && scalar[2] == 32) ||
                !constants.TryGetValue(array[3], out uint count)) return null;
            var pointers = new Dictionary<uint, (int Offset, int Size, bool Scalar)>();
            var reads = new SortedSet<(int Offset, int Size)>();
            IndexRange bounds = boundDynamic ? new IndexRange(w, types) : null;
            for (int i = 5; i < w.Length;)
            {
                int n = (int)(w[i] >> 16), op = (int)(w[i] & 65535);
                if (op == 65 || op == 66)
                {
                    if (n >= 4 && w[i + 3] == root)
                    {
                        if ((n != 6 && n != 7) || !constants.TryGetValue(w[i + 4], out uint member) || member != 0) return null;
                        // 下标：常量照旧；非常量只在 boundDynamic 时按取值界（上界必须 < N：越界读的结果不可证）
                        (ulong Lo, ulong Hi)? index = constants.TryGetValue(w[i + 5], out uint ci) ? (ci, ci) : bounds?.Of(w[i + 5]);
                        if (index == null || index.Value.Hi >= count || index.Value.Hi > int.MaxValue / 16 - 1) return null;
                        (ulong Lo, ulong Hi)? component = n == 7 ? (constants.TryGetValue(w[i + 6], out uint cc) ? (cc, cc) : bounds?.Of(w[i + 6])) : (0UL, 0UL);
                        if (component == null || component.Value.Hi >= 4) return null;
                        if (!Type(w[i + 1], 32, 4, out var resultPointer) || resultPointer[2] != 2 ||
                            resultPointer[3] != (n == 7 ? vector[2] : array[2])) return null;
                        var (lo, hi) = index.Value;
                        bool exactComponent = n == 6 || component.Value.Lo == component.Value.Hi;
                        // 单个分量（下标、分量都确定）：4 字节；其余按覆盖的 vec4 记整段（分量不定时整个 vec4）
                        int offset = (int)(lo * 16 + (n == 7 && exactComponent ? component.Value.Lo * 4 : 0UL));
                        int size = (int)((hi - lo) * 16 + (n == 7 && exactComponent ? 4UL : 16UL));
                        pointers[w[i + 2]] = (offset, size, n == 7);
                    }
                    else if (n >= 4 && pointers.ContainsKey(w[i + 3])) return null;
                }
                else if (op == 61 && n >= 4 && pointers.TryGetValue(w[i + 3], out var range))
                {
                    if (w[i + 1] != (range.Scalar ? vector[2] : array[2])) return null;
                    reads.Add((range.Offset, range.Size));
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
                    if (w[i + 1] != (range.Scalar ? vector[2] : array[2])) return null;
                    reads.Add((range.Offset, range.Size));
                }
                else if (op == 59 && n >= 5 && (w[i + 4] == root || pointers.ContainsKey(w[i + 4]))) return null;
                else if (!Declaration(op))
                    for (int j = 1; j < n; j++)
                        if (w[i + j] == root || pointers.ContainsKey(w[i + j])) return null;
                i += n;
            }
            return reads.Select(r => new[] { r.Offset, r.Size }).ToArray();
        }

        // 32 位整数值的无符号取值界 [Lo, Hi]（证不了 = null）。只认翻译器算下标会用到的那几种指令：
        //   常量、加减乘除取模、移位、与或异或、无符号位段提取、选择 / phi（并集）、位转换 / 复制、
        //   GLSL.std.450 的 UMin / UMax / UClamp（及操作数都非负时的有符号版本、SClamp 两端非负常量界），
        //   函数内 / 私有变量：只被 OpLoad / OpStore 用过（指针没有别的用法）时取所有存入值与初值的并集。
        //   变量读在任何写之前是未定义值（Ryujinx 的局部变量都是 SSA 值落地，定义支配使用），这里按「读到的是某次写入的值」算。
        // 回边（循环里的 phi / 自增变量）求值时遇到正在求的值 → null，不做加宽。
        private sealed class IndexRange
        {
            const ulong Max = uint.MaxValue;
            readonly Dictionary<uint, int> _def = new();             // 结果 id → 指令起点
            readonly Dictionary<uint, List<uint>> _stores = new();   // 局部变量 → 存入的值（含初值）
            readonly HashSet<uint> _escaped = new();
            readonly Dictionary<uint, (ulong, ulong)?> _memo = new();
            readonly HashSet<uint> _busy = new();
            readonly uint[] _w;
            readonly Dictionary<uint, uint[]> _types;
            uint _glsl = uint.MaxValue;

            public IndexRange(ReadOnlySpan<uint> w, Dictionary<uint, uint[]> types)
            {
                _w = w.ToArray(); _types = types;
                for (int i = 5; i < _w.Length;)
                {
                    int n = (int)(_w[i] >> 16), op = (int)(_w[i] & 65535);
                    if (n == 0) break;
                    if (op == 11 && n >= 3 && Name(i + 2, n - 2) == "GLSL.std.450") _glsl = _w[i + 1];
                    if (op == 59 && n >= 4 && (_w[i + 3] == 7 || _w[i + 3] == 6))
                        _stores[_w[i + 2]] = n >= 5 ? new List<uint> { _w[i + 4] } : new List<uint>();
                    if (n >= 3 && HasResult(op)) _def[_w[i + 2]] = i;
                    i += n;
                }
                for (int i = 5; i < _w.Length;)
                {
                    int n = (int)(_w[i] >> 16), op = (int)(_w[i] & 65535);
                    if (n == 0) break;
                    for (int j = 1; j < n; j++)
                    {
                        uint id = _w[i + j];
                        if (!_stores.ContainsKey(id)) continue;
                        if (op == 61 && j == 3) continue;                                         // OpLoad 的指针
                        if (op == 62 && j == 1) { if (n >= 3) _stores[id].Add(_w[i + 2]); continue; }   // OpStore 的指针
                        if (op == 59 && j == 2) continue;                                         // 自己的声明
                        if (op is 5 or 71) continue;                                              // 名字 / 装饰
                        _escaped.Add(id);                                                         // 其它用法（含字面量巧合）：保守当逃逸
                    }
                    i += n;
                }
            }

            string Name(int at, int words)
            {
                var b = new List<byte>();
                for (int k = 0; k < words; k++) for (int s = 0; s < 32; s += 8) { byte c = (byte)(_w[at + k] >> s); if (c == 0) return System.Text.Encoding.ASCII.GetString(b.ToArray()); b.Add(c); }
                return null;
            }

            static bool HasResult(int op) => op is 1 or 12 or 43 or 61 or 83 or 113 or 124 or 128 or 130 or 132 or 134 or 137
                or 169 or 194 or 195 or 196 or 197 or 198 or 199 or 204 or 245;

            bool Int32(uint type) => _types.TryGetValue(type, out var t) && (t[0] & 65535) == 21 && t.Length >= 3 && t[2] == 32;

            static (ulong, ulong)? Union((ulong, ulong)? a, (ulong, ulong)? b) =>
                a == null || b == null ? null : (Math.Min(a.Value.Item1, b.Value.Item1), Math.Max(a.Value.Item2, b.Value.Item2));

            static ulong Mask(ulong v) { ulong m = 0; while (m < v) m = m << 1 | 1; return m; }

            public (ulong Lo, ulong Hi)? Of(uint id)
            {
                if (_memo.TryGetValue(id, out var known)) return known;
                if (!_busy.Add(id)) return null;   // 回边：证不了
                (ulong, ulong)? r = Eval(id);
                _busy.Remove(id);
                _memo[id] = r;
                return r;
            }

            (ulong, ulong)? Eval(uint id)
            {
                if (!_def.TryGetValue(id, out int i)) return null;
                int n = (int)(_w[i] >> 16), op = (int)(_w[i] & 65535);
                if (op != 12 && !Int32(_w[i + 1])) return null;   // 只算 32 位整数值
                (ulong Lo, ulong Hi)? A(int k) => n > k ? Of(_w[i + k]) : null;
                switch (op)
                {
                    case 43: return n == 4 ? (_w[i + 3], _w[i + 3]) : null;
                    case 83: case 113: case 124: return n == 4 && Int32(_w[i + 1]) ? A(3) : null;   // 复制 / 无符号扩宽（32→32）/ 位转换：位不变
                    case 61:
                    {
                        if (n < 4 || !_stores.TryGetValue(_w[i + 3], out var vals) || _escaped.Contains(_w[i + 3]) || vals.Count == 0) return null;
                        (ulong, ulong)? u = Of(vals[0]);
                        for (int k = 1; k < vals.Count && u != null; k++) u = Union(u, Of(vals[k]));
                        return u;
                    }
                    case 169: return n == 6 ? Union(A(4), A(5)) : null;
                    case 245:
                    {
                        if (n < 5) return null;
                        (ulong, ulong)? u = A(3);
                        for (int k = 5; k < n && u != null; k += 2) u = Union(u, A(k));
                        return u;
                    }
                }
                if (op == 12)
                {
                    if (n < 6 || _w[i + 3] != _glsl || !Int32(_w[i + 1])) return null;
                    uint inst = _w[i + 4];
                    var x = A(5); var y = A(6); var z = A(7);
                    const ulong SMax = int.MaxValue;
                    bool Pos((ulong Lo, ulong Hi)? v) => v != null && v.Value.Hi <= SMax;
                    switch (inst)
                    {
                        case 38:   // UMin
                            if (x != null && y != null) return (Math.Min(x.Value.Lo, y.Value.Lo), Math.Min(x.Value.Hi, y.Value.Hi));
                            return x != null ? (0, x.Value.Hi) : y != null ? (0, y.Value.Hi) : null;
                        case 39:   // SMin：两个都非负才等于 UMin
                            return Pos(x) && Pos(y) ? (Math.Min(x.Value.Lo, y.Value.Lo), Math.Min(x.Value.Hi, y.Value.Hi)) : null;
                        case 41:   // UMax
                        case 42:   // SMax（两个都非负）
                            if (x == null || y == null || (inst == 42 && (!Pos(x) || !Pos(y)))) return null;
                            return (Math.Max(x.Value.Lo, y.Value.Lo), Math.Max(x.Value.Hi, y.Value.Hi));
                        case 44:   // UClamp(x, mn, mx) = min(max(x, mn), mx)
                            if (z == null || y == null) return null;
                            return (Math.Min(y.Value.Lo, z.Value.Lo), z.Value.Hi);
                        case 45:   // SClamp：两端都是非负、mn ≤ mx 时结果落在 [mn, mx]，与 x 无关
                            if (!Pos(y) || !Pos(z) || y.Value.Hi > z.Value.Lo) return null;
                            return (y.Value.Lo, z.Value.Hi);
                    }
                    return null;
                }
                var a = A(3); var b = A(4);
                switch (op)
                {
                    case 128: return a != null && b != null && a.Value.Hi + b.Value.Hi <= Max ? (a.Value.Lo + b.Value.Lo, a.Value.Hi + b.Value.Hi) : null;
                    case 130: return a != null && b != null && a.Value.Lo >= b.Value.Hi ? (a.Value.Lo - b.Value.Hi, a.Value.Hi - b.Value.Lo) : null;
                    case 132: return a != null && b != null && a.Value.Hi * b.Value.Hi <= Max ? (a.Value.Lo * b.Value.Lo, a.Value.Hi * b.Value.Hi) : null;
                    case 134: return a != null && b != null && b.Value.Lo > 0 ? (a.Value.Lo / b.Value.Hi, a.Value.Hi / b.Value.Lo) : null;
                    case 137: return b != null && b.Value.Lo > 0 ? (0UL, a != null ? Math.Min(a.Value.Hi, b.Value.Hi - 1) : b.Value.Hi - 1) : null;
                    case 194: case 195:   // 逻辑右移；算术右移只在被移值非负时相同
                        if (a == null || b == null || b.Value.Hi >= 32 || (op == 195 && a.Value.Hi > int.MaxValue)) return null;
                        return (a.Value.Lo >> (int)b.Value.Hi, a.Value.Hi >> (int)b.Value.Lo);
                    case 196:
                        if (a == null || b == null || b.Value.Hi >= 32 || (a.Value.Hi << (int)b.Value.Hi) > Max) return null;
                        return (a.Value.Lo << (int)b.Value.Lo, a.Value.Hi << (int)b.Value.Hi);
                    case 199:   // 与：不大于任何一个已知操作数
                        return a != null && b != null ? (0UL, Math.Min(a.Value.Hi, b.Value.Hi)) : a != null ? (0UL, a.Value.Hi) : b != null ? (0UL, b.Value.Hi) : null;
                    case 197: case 198:
                        return a != null && b != null ? (op == 197 ? Math.Max(a.Value.Lo, b.Value.Lo) : 0UL, Mask(Math.Max(a.Value.Hi, b.Value.Hi))) : null;
                    case 204:   // OpBitFieldUExtract base offset count
                    {
                        var cnt = A(5);
                        if (cnt == null || cnt.Value.Hi > 32) return null;
                        ulong hi = cnt.Value.Hi == 32 ? Max : (1UL << (int)cnt.Value.Hi) - 1;
                        var off = A(4);
                        if (a != null && off != null && off.Value.Lo < 32) hi = Math.Min(hi, a.Value.Hi >> (int)off.Value.Lo);
                        return (0UL, hi);
                    }
                }
                return null;
            }
        }
    }
}
