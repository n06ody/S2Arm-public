// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace S2A.DirectCompiler
{
    // 计算核共享内存超过设备上限时，按大小依次把线程组数组挪到一个存储缓冲里，
    // 每个线程组一片，宿主按派发组数分配（同 iOS SpillFix：
    // graphics/dsx/gal_spill.c、dmsh_build.py spill_fix，那边改 MSL 文本，这里改 Ryujinx 产出的 SPIR-V 1.3）。
    // 只认 Ryujinx 的形状：Workgroup 存储类的 uint[N] 变量、无初值，只经单下标 OpAccessChain 取元素，
    // 元素指针只给 OpLoad / OpStore / 原子操作用。认不出就抛 UNSUPPORTED_PROGRAM，不猜。
    // 组内可见性：原来对 Workgroup 内存的屏障 / 原子语义补 UniformMemory，存储缓冲上的读写也按同一屏障排序。
    public static class DirectSharedSpill
    {
        public sealed class Result
        {
            public byte[] Spirv;
            public int BytesPerGroup, Set, Binding, RemainingSharedBytes;
        }

        private const uint OpName = 5, OpEntryPoint = 15, OpTypeInt = 21, OpTypeVector = 23, OpTypeArray = 28,
            OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpFunction = 54,
            OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66,
            OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpIAdd = 128, OpIMul = 132,
            OpControlBarrier = 224, OpMemoryBarrier = 225, OpTypeBool = 20, OpULessThan = 176, OpSelect = 169,
            OpFunctionEnd = 56, OpLabel = 248, OpLine = 8, OpNoLine = 317;
        private const uint ScWorkgroup = 4, ScUniform = 2, ScInput = 1;
        private const uint DecBufferBlock = 3, DecArrayStride = 6, DecBuiltIn = 11, DecCoherent = 23, DecBinding = 33, DecSet = 34, DecOffset = 35;
        private const uint BuiltInNumWorkgroups = 24, BuiltInWorkgroupId = 26;
        private const uint SemWorkgroupMemory = 0x100, SemUniformMemory = 0x40;

        private static bool IsAtomic(uint op) => op >= 227 && op <= 242 && op != 231;   // 231 = OpAtomicCompareExchangeWeak 也是原子，但下面单列
        private static int AtomicPointerWord(uint op) => op == 228 ? 1 : 3;              // OpAtomicStore：指针在第 1 个操作数

        // 这条指令里哪些字是 id（不是字面量）。全局段（第一个 OpFunction 之前）只看调试名与修饰的目标；
        //   函数体里按操作码排除字面量位置（访存掩码、复合下标、合并 / 分支控制字、扩展指令号、switch 字面量）。
        //   排不全时宁可误报（如实拒），不会漏报。
        private static IEnumerable<int> IdWords(uint[] w, bool global)
        {
            uint op = w[0] & 0xFFFF; int n = w.Length;
            if (global)
            {
                if (op == OpName || op == 6 || op == OpDecorate || op == OpMemberDecorate) yield return 1;
                yield break;
            }
            int last = op switch
            {
                OpLoad => 3, OpStore => 2, 79 => 4, OpCompositeExtract => 3, 82 => 4, 246 => 2, 247 => 1, 250 => 3, 8 => 0,
                _ => n - 1,
            };
            for (int j = 1; j <= last && j < n; j++)
            {
                if (op == 12 && j == 4) continue;                       // OpExtInst：扩展指令号
                if (op == 251 && j >= 3 && (j & 1) == 1) continue;      // OpSwitch：case 字面量
                yield return j;
            }
        }

        private static Exception Unsupported(string why) =>
            new DirectCompileException("UNSUPPORTED_PROGRAM", "cs: shared memory spill: " + why);

        // 改写前先量：这个模块的 Workgroup 数组一共多少字节（只认 uint[N]）；认不出的形状返回 -1。
        public static int WorkgroupBytes(ReadOnlySpan<byte> spv)
        {
            var m = Parse(spv);
            long total = 0;
            foreach (var v in m.WorkgroupVars)
            {
                long bytes = m.UintArrayBytes(v.PointeeType);
                if (bytes < 0) return -1;
                total += bytes;
            }
            return total > int.MaxValue ? -1 : (int)total;
        }

        public static Result Rewrite(ReadOnlySpan<byte> spv, int limitBytes, int set, int binding)
        {
            var m = Parse(spv);
            if (m.Version != 0x00010300) throw Unsupported("only SPIR-V 1.3 is handled");
            var sized = new List<(Var v, long bytes)>();
            foreach (var v in m.WorkgroupVars)
            {
                long b = m.UintArrayBytes(v.PointeeType);
                if (b < 0) throw Unsupported("workgroup variable %" + v.Id + " is not a uint array");
                sized.Add((v, b));
            }
            long total = sized.Sum(x => x.bytes);
            if (total <= limitBytes) return null;
            var selected = new Dictionary<uint, (uint offset, uint length)>();
            long remaining = total, spillBytes = 0;
            foreach (var (v, bytes) in sized.OrderByDescending(x => x.bytes).ThenBy(x => x.v.Id))
            {
                if (remaining <= limitBytes) break;
                if (bytes <= 0 || bytes > int.MaxValue || spillBytes + bytes > int.MaxValue)
                    throw Unsupported("invalid or oversized workgroup array");
                if (v.HasInitializer) throw Unsupported("a selected workgroup array has an initializer");
                selected.Add(v.Id, ((uint)(spillBytes / 4), (uint)(bytes / 4)));
                spillBytes += bytes; remaining -= bytes;
            }
            if (remaining > limitBytes || selected.Count == 0) throw Unsupported("cannot reduce workgroup memory to the device limit");
            uint length = (uint)(spillBytes / 4);

            // 用处检查：变量只作单下标访问链的基址（调试名除外）；访问链结果只给读 / 写 / 原子用
            int fnStart = m.Insts.FindIndex(x => (x[0] & 0xFFFF) == OpFunction);
            if (fnStart < 0) throw Unsupported("module has no function");
            var chains = new Dictionary<uint, int>();   // 结果 id → 指令下标
            for (int k = 0; k < m.Insts.Count; k++)
            {
                uint[] w = m.Insts[k]; uint op = w[0] & 0xFFFF;
                if (op == OpVariable && selected.ContainsKey(w[2])) continue;
                if (op == OpName && selected.ContainsKey(w[1])) continue;
                bool refs = false; foreach (int j in IdWords(w, k < fnStart)) if (selected.ContainsKey(w[j])) refs = true;
                if (!refs) continue;
                if ((op == OpAccessChain || op == OpInBoundsAccessChain) && w.Length == 5 && selected.ContainsKey(w[3])) { chains[w[2]] = k; continue; }
                throw Unsupported("workgroup array used by opcode " + op + " (not a single-index access chain)");
            }
            if (chains.Count == 0) throw Unsupported("the largest workgroup array is never accessed");
            for (int k = 0; k < m.Insts.Count; k++)
            {
                uint[] w = m.Insts[k]; uint op = w[0] & 0xFFFF;
                if (chains.ContainsKey(w.Length > 2 ? w[2] : 0) && (op == OpAccessChain || op == OpInBoundsAccessChain)) continue;
                foreach (int j in IdWords(w, k < fnStart))
                {
                    if (!chains.ContainsKey(w[j])) continue;
                    bool ok = (op == OpLoad && j == 3) || (op == OpStore && j == 1) ||
                              ((IsAtomic(op) || op == 231) && j == AtomicPointerWord(op));
                    if (!ok) throw Unsupported("workgroup element pointer used by opcode " + op);
                }
            }

            uint bound = m.Bound;
            uint NewId() => bound++;
            uint tUint = m.FindType(w => (w[0] & 0xFFFF) == OpTypeInt && w.Length == 4 && w[2] == 32 && w[3] == 0);
            if (tUint == 0) throw Unsupported("no 32-bit unsigned integer type");
            var globals = new List<uint[]>(); var decorations = new List<uint[]>();
            uint tV3 = m.FindType(w => (w[0] & 0xFFFF) == OpTypeVector && w.Length == 4 && w[2] == tUint && w[3] == 3);
            if (tV3 == 0) { tV3 = NewId(); globals.Add(Inst(OpTypeVector, tV3, tUint, 3)); }
            uint tPIn = 0;
            var interfaceAdd = new List<uint>();
            uint Builtin(uint which)
            {
                uint found = m.BuiltinVar(which, tV3);
                if (found != 0) return found;
                if (tPIn == 0) { tPIn = NewId(); globals.Add(Inst(OpTypePointer, tPIn, ScInput, tV3)); }
                uint v = NewId(); globals.Add(Inst(OpVariable, tPIn, v, ScInput));
                decorations.Add(Inst(OpDecorate, v, DecBuiltIn, which)); interfaceAdd.Add(v);
                return v;
            }
            uint vGid = Builtin(BuiltInWorkgroupId), vNum = Builtin(BuiltInNumWorkgroups);
            uint tRa = NewId(), tSt = NewId(), tPSt = NewId(), tPU = NewId(), vSb = NewId(), cZero = NewId(), cLen = NewId();
            uint tBool = m.FindType(w => (w[0] & 0xFFFF) == OpTypeBool && w.Length == 2);
            if (tBool == 0) { tBool = NewId(); globals.Add(Inst(OpTypeBool, tBool)); }
            globals.Add(Inst(OpTypeRuntimeArray, tRa, tUint));
            globals.Add(Inst(OpTypeStruct, tSt, tRa));
            globals.Add(Inst(OpTypePointer, tPSt, ScUniform, tSt));
            globals.Add(Inst(OpTypePointer, tPU, ScUniform, tUint));
            globals.Add(Inst(OpVariable, tPSt, vSb, ScUniform));
            globals.Add(Inst(OpConstant, tUint, cZero, 0));
            globals.Add(Inst(OpConstant, tUint, cLen, length));
            var ranges = new Dictionary<uint, (uint len, uint last, uint offset)>();
            foreach (var (id, range) in selected)
            {
                uint len = NewId(), last = NewId(), offset = NewId();
                globals.Add(Inst(OpConstant, tUint, len, range.length));
                globals.Add(Inst(OpConstant, tUint, last, range.length - 1));
                globals.Add(Inst(OpConstant, tUint, offset, range.offset));
                ranges.Add(id, (len, last, offset));
            }
            decorations.Add(Inst(OpDecorate, tRa, DecArrayStride, 4));
            decorations.Add(Inst(OpDecorate, tSt, DecBufferBlock));
            decorations.Add(Inst(OpMemberDecorate, tSt, 0, DecOffset, 0));
            // Coherent：Workgroup 内存天然组内一致；挪到存储缓冲后，没有 Coherent 时按 GLSL / Vulkan 内存模型，
            //   一个调用写的值经 barrier 后对同组其他调用不保证可见（驱动可以走不一致的缓存 / 把读提到 barrier 前）。
            //   组内归约、前缀和一类（GPU 剔除核）靠的正是这种可见性，所以必须加（glslang 对 coherent 缓冲成员同样出 OpMemberDecorate Coherent）。
            decorations.Add(Inst(OpMemberDecorate, tSt, 0, DecCoherent));
            decorations.Add(Inst(OpDecorate, vSb, DecSet, (uint)set));
            decorations.Add(Inst(OpDecorate, vSb, DecBinding, (uint)binding));
            var semantics = new Dictionary<uint, uint>();   // 旧语义常量 id → 补了 UniformMemory 的新常量 id
            uint Widen(uint semId)
            {
                if (!m.Constants.TryGetValue(semId, out uint value))
                    throw Unsupported("memory semantics %" + semId + " is not a plain constant");
                if ((value & SemWorkgroupMemory) == 0 || (value & SemUniformMemory) != 0) return semId;
                if (!semantics.TryGetValue(semId, out uint n)) { n = NewId(); semantics[semId] = n; globals.Add(Inst(OpConstant, tUint, n, value | SemUniformMemory)); }
                return n;
            }

            var output = new List<uint[]>();
            bool globalsPlaced = false, decorationsPlaced = false;
            int lastAnnotation = -1, firstFunction = -1;
            for (int k = 0; k < m.Insts.Count; k++)
            {
                uint op = m.Insts[k][0] & 0xFFFF;
                if (op == OpDecorate || op == OpMemberDecorate || op == 73 || op == 74 || op == 75 || op == 332) lastAnnotation = k;
                if (op == OpFunction && firstFunction < 0) firstFunction = k;
            }
            if (firstFunction < 0) throw Unsupported("module has no function");
            // 本组片的基址（扁平组号 × 片长）在每个用到它的函数入口块算一次：函数入口块支配整个函数，
            //   每处访问只剩「夹界 + 加基址」，代码量不随访问次数成倍涨（每处重算会让访问上千处的核超出结果大小上限）。
            var baseAfter = new Dictionary<int, uint>();   // 指令下标 → 在它之后插入基址计算、基址 id
            var baseOf = new Dictionary<int, uint>();      // 访问链指令下标 → 所在函数的基址 id
            for (int k = 0, fn = -1, label = -1; k < m.Insts.Count; k++)
            {
                uint op = m.Insts[k][0] & 0xFFFF;
                if (op == OpFunction) { fn = k; label = -1; }
                else if (op == OpLabel && fn >= 0 && label < 0) label = k;
                else if (op == OpFunctionEnd) fn = -1;
                if (fn < 0 || !((op == OpAccessChain || op == OpInBoundsAccessChain) && m.Insts[k].Length == 5 && selected.ContainsKey(m.Insts[k][3]))) continue;
                if (label < 0) throw Unsupported("access chain outside a block");
                int at = label;
                while (at + 1 < m.Insts.Count && (m.Insts[at + 1][0] & 0xFFFF) is OpVariable or OpLine or OpNoLine) at++;
                if (!baseAfter.TryGetValue(at, out uint b)) { b = NewId(); baseAfter[at] = b; }
                baseOf[k] = b;
            }
            for (int k = 0; k < m.Insts.Count; k++)
            {
                uint[] w = m.Insts[k]; uint op = w[0] & 0xFFFF;
                if (k == firstFunction && !globalsPlaced)
                {
                    // 语义常量要在函数体扫描时才知道：先占位，扫完再插（见下）
                    output.Add(null); globalsPlaced = true;
                }
                if ((op == OpVariable && selected.ContainsKey(w[2])) || (op == OpName && selected.ContainsKey(w[1]))) { if (k == lastAnnotation && !decorationsPlaced) { output.AddRange(decorations); decorationsPlaced = true; } continue; }
                if (op == OpEntryPoint && interfaceAdd.Count != 0)
                {
                    var n = new List<uint>(w); n.AddRange(interfaceAdd); n[0] = (uint)n.Count << 16 | OpEntryPoint; w = n.ToArray();
                }
                if ((op == OpAccessChain || op == OpInBoundsAccessChain) && w.Length == 5 && selected.ContainsKey(w[3]))
                {
                    // 新下标 = 本组基址 + min(原下标, 片长 − 1)：越过声明长度的下标（在 Workgroup 里是未定义、通常无害）
                    //   不许写出本组的片——没开 robustBufferAccess 的设备上写出缓冲尾可能是 GPU 故障。OpIAdd 允许两边符号性不同。
                    var range = ranges[w[3]];
                    uint u = NewId(), lt = NewId(), clamped = NewId(), idx = NewId();
                    output.Add(Inst(OpIAdd, tUint, u, cZero, w[4]));
                    output.Add(Inst(OpULessThan, tBool, lt, u, range.len));
                    output.Add(Inst(OpSelect, tUint, clamped, lt, u, range.last));
                    uint local = clamped;
                    if (selected[w[3]].offset != 0) { local = NewId(); output.Add(Inst(OpIAdd, tUint, local, clamped, range.offset)); }
                    output.Add(Inst(OpIAdd, tUint, idx, baseOf[k], local));
                    output.Add(Inst(OpAccessChain, tPU, w[2], vSb, cZero, idx));
                    continue;
                }
                if (op == OpControlBarrier) { w = (uint[])w.Clone(); w[3] = Widen(w[3]); }
                else if (op == OpMemoryBarrier) { w = (uint[])w.Clone(); w[2] = Widen(w[2]); }
                else if ((IsAtomic(op) || op == 231) && w.Length > AtomicPointerWord(op) && chains.ContainsKey(w[AtomicPointerWord(op)]))
                {
                    w = (uint[])w.Clone(); int p = AtomicPointerWord(op);
                    w[p + 2] = Widen(w[p + 2]);                              // 指针、作用域、语义
                    if (op == 230 || op == 231) w[p + 3] = Widen(w[p + 3]);   // 比较交换：不等时的语义
                }
                output.Add(w);
                if (baseAfter.TryGetValue(k, out uint baseId))
                {
                    // 扁平组号 = x + nx·(y + ny·z)；基址 = 组号 × 片长
                    uint g = NewId(), n = NewId(), gx = NewId(), gy = NewId(), gz = NewId(), nx = NewId(), ny = NewId();
                    uint t1 = NewId(), t2 = NewId(), t3 = NewId(), flat = NewId();
                    output.Add(Inst(OpLoad, tV3, g, vGid)); output.Add(Inst(OpLoad, tV3, n, vNum));
                    output.Add(Inst(OpCompositeExtract, tUint, gx, g, 0)); output.Add(Inst(OpCompositeExtract, tUint, gy, g, 1));
                    output.Add(Inst(OpCompositeExtract, tUint, gz, g, 2)); output.Add(Inst(OpCompositeExtract, tUint, nx, n, 0));
                    output.Add(Inst(OpCompositeExtract, tUint, ny, n, 1));
                    output.Add(Inst(OpIMul, tUint, t1, ny, gz)); output.Add(Inst(OpIAdd, tUint, t2, gy, t1));
                    output.Add(Inst(OpIMul, tUint, t3, nx, t2)); output.Add(Inst(OpIAdd, tUint, flat, gx, t3));
                    output.Add(Inst(OpIMul, tUint, baseId, flat, cLen));
                }
                if (k == lastAnnotation && !decorationsPlaced) { output.AddRange(decorations); decorationsPlaced = true; }
            }
            if (!decorationsPlaced) throw Unsupported("module has no annotation section");
            int hole = output.IndexOf(null);
            output.RemoveAt(hole); output.InsertRange(hole, globals);

            var words = new List<uint> { m.Header[0], m.Header[1], m.Header[2], bound, m.Header[4] };
            foreach (uint[] w in output) words.AddRange(w);
            return new Result
            {
                Spirv = MemoryMarshal.AsBytes(words.ToArray().AsSpan()).ToArray(),
                BytesPerGroup = (int)spillBytes, Set = set, Binding = binding, RemainingSharedBytes = (int)remaining,
            };
        }

        private static uint[] Inst(uint op, params uint[] operands)
        {
            var w = new uint[operands.Length + 1];
            w[0] = (uint)w.Length << 16 | op; Array.Copy(operands, 0, w, 1, operands.Length);
            return w;
        }

        private sealed class Var { public uint Id, PointeeType; public bool HasInitializer; }

        private sealed class Module
        {
            public uint[] Header; public uint Version, Bound;
            public readonly List<uint[]> Insts = new();
            public readonly Dictionary<uint, uint[]> Types = new();
            public readonly Dictionary<uint, uint> Constants = new();
            public readonly List<Var> WorkgroupVars = new();
            public readonly Dictionary<uint, uint> BuiltinOf = new();   // 变量 id → BuiltIn
            public readonly Dictionary<uint, uint> VarPointer = new();  // 变量 id → 指针类型

            public uint FindType(Func<uint[], bool> pred) { foreach (var kv in Types) if (pred(kv.Value)) return kv.Key; return 0; }

            public long UintArrayBytes(uint type)
            {
                if (!Types.TryGetValue(type, out var a) || (a[0] & 0xFFFF) != OpTypeArray || a.Length != 4) return -1;
                if (!Types.TryGetValue(a[2], out var e) || (e[0] & 0xFFFF) != OpTypeInt || e[2] != 32) return -1;
                return Constants.TryGetValue(a[3], out uint n) ? 4L * n : -1;
            }

            public uint BuiltinVar(uint which, uint tV3)
            {
                foreach (var kv in BuiltinOf)
                {
                    if (kv.Value != which || !VarPointer.TryGetValue(kv.Key, out uint p)) continue;
                    if (Types.TryGetValue(p, out var pt) && pt[2] == ScInput && pt[3] == tV3) return kv.Key;
                    throw Unsupported("builtin " + which + " is declared with an unexpected type");
                }
                return 0;
            }
        }

        private static Module Parse(ReadOnlySpan<byte> spv)
        {
            if (spv.Length < 20 || spv.Length % 4 != 0) throw Unsupported("truncated SPIR-V");
            uint[] all = MemoryMarshal.Cast<byte, uint>(spv).ToArray();
            if (all[0] != 0x07230203) throw Unsupported("bad SPIR-V magic");
            var m = new Module { Header = all[..5], Version = all[1], Bound = all[3] };
            for (int i = 5; i < all.Length;)
            {
                int n = (int)(all[i] >> 16);
                if (n == 0 || i + n > all.Length) throw Unsupported("truncated instruction");
                uint[] w = all[i..(i + n)]; uint op = w[0] & 0xFFFF;
                m.Insts.Add(w);
                if (op >= 19 && op <= 39 && n >= 2) m.Types[w[1]] = w;
                if (op == OpConstant && n == 4) m.Constants[w[2]] = w[3];
                if (op == OpDecorate && n == 4 && w[2] == DecBuiltIn) m.BuiltinOf[w[1]] = w[3];
                if (op == OpVariable && n >= 4)
                {
                    m.VarPointer[w[2]] = w[1];
                    if (w[3] == ScWorkgroup)
                    {
                        if (!m.Types.TryGetValue(w[1], out var p) || (p[0] & 0xFFFF) != OpTypePointer) throw Unsupported("workgroup variable without pointer type");
                        m.WorkgroupVars.Add(new Var { Id = w[2], PointeeType = p[3], HasInitializer = n > 4 });
                    }
                }
                i += n;
            }
            return m;
        }
    }
}
