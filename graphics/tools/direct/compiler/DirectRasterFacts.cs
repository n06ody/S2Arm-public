// 直连产物的光栅事实：对最终 SPIR-V 做 word 级 SSA 溯源，证得出才写，证不出回保守缺省。
// 字段与 C 侧解析器（android/native/nvn/nvn_vk_program_result.c 的 "raster" 段）一一对应：
//   pos_attr      — VS：out position 直通的那个顶点属性（Location）号；z 过深度改写时
//                   指改写的 z 源属性（与 pos_z_half 组合使用）；证不出 = -1
//   pos_w1        — VS：out position.w 恒为常量 1.0f（该属性 .w 不被消费）
//   pos_z_half    — VS：写出位置的 z = z*0.5 + w*0.5（编译期 DepthMode 尾巴；运行时
//                   select 尾巴链条上有 OpSelect，一律证不出 = false）。成立时
//                   pos_attr 指改写源的属性——回放侧按 rp_draw.c 的顺序先读属性
//                   再对 P.z 重做同一变换。
//   may_discard   — FS：可能执行 discard（出现任何 kill 形态 = true）
//   writes_smask  — FS：写 sample mask（BuiltIn SampleMask 输出被 OpStore/OpCopyMemory 覆盖）
// 纪律：只证「纯直通、纯常量或 z-half 那一形」。FNegate / Select / Bitcast / phi 等任何
// 其他改写都判证不出；宁可全程保守，绝不输出乐观假值。
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace S2A.DirectCompiler
{
    public static class DirectRasterFacts
    {
        public readonly struct Result
        {
            // HasVertex / HasFragment 标记该模块有没有对应 ExecutionModel 的入口；
            // 没有入口的阶段一个字段都不写（C 侧缺省就是保守值，不损失信息）。
            public readonly bool HasVertex;
            public readonly int PosAttr;            // -1 或 0..15
            public readonly bool PosW1;
            public readonly bool PosZHalf;
            public readonly bool HasFragment;
            public readonly bool MayDiscard;        // 缺省 true = 可能丢
            public readonly bool WritesSampleMask;  // 缺省 true = 可能写
            public Result(bool hasVertex, int posAttr, bool posW1, bool posZHalf,
                          bool hasFragment, bool mayDiscard, bool writesSampleMask)
            {
                HasVertex = hasVertex; PosAttr = posAttr; PosW1 = posW1; PosZHalf = posZHalf;
                HasFragment = hasFragment; MayDiscard = mayDiscard; WritesSampleMask = writesSampleMask;
            }
        }

        public static Result Analyze(ReadOnlySpan<byte> spirv)
        {
            if (spirv.Length < 24 || spirv.Length % 4 != 0)
                return new Result(false, -1, false, false, false, true, true);
            return new Module(MemoryMarshal.Cast<byte, uint>(spirv).ToArray()).Evaluate();
        }

        // 指令集里只登记这些产线/指针 op；其余（phi、shuffle、算术、位转……）不登记 id，
        // trace 撞上就是 Fail——与「不能溯源 = 证不出」同义，不靠逐个枚举报乐观。
        private const int OpExtInstImport = 11;
        private const int OpExtInst = 12;
        private const int OpEntryPoint = 15;
        private const int OpTypePointer = 32;
        private const int OpConstant = 43;
        private const int OpConstantComposite = 44;
        private const int OpConstantNull = 46;
        private const int OpFunction = 54;
        private const int OpFunctionEnd = 56;
        private const int OpFunctionCall = 57;
        private const int OpVariable = 59;
        private const int OpLoad = 61;
        private const int OpStore = 62;
        private const int OpAccessChain = 65;
        private const int OpInBoundsAccessChain = 66;
        private const int OpDecorate = 71;
        private const int OpMemberDecorate = 72;
        private const int OpCompositeConstruct = 80;
        private const int OpCompositeExtract = 81;
        private const int OpCopyObject = 83;
        private const int OpCopyMemory = 84;
        private const int OpFAdd = 129;
        private const int OpFMul = 133;
        private const int OpPhi = 245;
        private const int OpSelectionMerge = 246;
        private const int OpLoopMerge = 247;
        private const int OpLabel = 248;
        private const int OpBranch = 249;
        private const int OpBranchConditional = 250;
        private const int OpSwitch = 251;
        private const int OpKill = 252;
        private const int OpReturn = 253;
        private const int OpUnreachable = 254;
        private const int OpReturnValue = 255;
        private const int OpTerminateInvocation = 4416;
        private const int OpDemoteToHelperInvocation = 4417;

        private const int StorageInput = 1, StorageOutput = 3, StoragePrivate = 6, StorageFunction = 7;
        private const int DecorationBuiltIn = 11, DecorationLocation = 30;
        private const int BuiltInPosition = 0, BuiltInSampleMask = 20;   // SPIR-V BuiltIn 编号：SampleMask=20
        private const int ExtInstFma = 50;                               // GLSL.std.450 Fma
        private const uint BitsHalf = 0x3F000000u, BitsOne = 0x3F800000u;
        private static readonly int[] EmptyPath = Array.Empty<int>();

        // 溯源结果：属性分量 / 常量位模式 / 证不出。
        private readonly struct Prov
        {
            public readonly int Kind;   // 0=Fail, 1=Attr, 2=Const
            public readonly int Loc, Comp;
            public readonly uint Bits;
            private Prov(int kind, int loc, int comp, uint bits) { Kind = kind; Loc = loc; Comp = comp; Bits = bits; }
            public static readonly Prov Fail = new Prov(0, 0, 0, 0);
            public static Prov Attr(int loc, int comp) => new Prov(1, loc, comp, 0);
            public static Prov Const(uint bits) => new Prov(2, 0, 0, bits);
        }

        private sealed class Module
        {
            private readonly uint[] _w;
            // _defs：已登记 id → 指令起始下标。只登记透明 op（Load/Construct/Extract/CopyObject/
            // AccessChain/Variable/常量）与 z-half 形状 op（ExtInst/FAdd/FMul）；没登记的 id = Fail。
            private readonly Dictionary<uint, int> _defs = new Dictionary<uint, int>();
            private readonly Dictionary<uint, uint> _varStorage = new Dictionary<uint, uint>();
            private readonly Dictionary<uint, uint> _varType = new Dictionary<uint, uint>();
            private readonly Dictionary<uint, uint> _locations = new Dictionary<uint, uint>();
            private readonly Dictionary<uint, uint> _varBuiltin = new Dictionary<uint, uint>();
            private readonly Dictionary<(uint, uint), uint> _memberBuiltin = new Dictionary<(uint, uint), uint>();
            private readonly Dictionary<uint, uint> _constants = new Dictionary<uint, uint>();
            private readonly Dictionary<uint, uint> _pointee = new Dictionary<uint, uint>();
            private readonly HashSet<uint> _glslImports = new HashSet<uint>();
            private readonly List<int> _storeInstrs = new List<int>();     // OpStore / OpCopyMemory 的下标
            private readonly List<int> _blockOps = new List<int>();        // 入口块内指令下标（有序）
            private uint _vertexEntry;
            private bool _hasVertex, _hasFragment, _mayDiscard;
            private int _blockStart, _blockEnd = -1;                       // 入口函数单块的指令区间
            // 每变量按程序序的写入历史（常量下标路径 + 值 id）；OpVariable 初始化器也算一次写入。
            private readonly Dictionary<uint, List<(int[] idx, uint val)>> _mem = new Dictionary<uint, List<(int[], uint)>>();
            private readonly HashSet<uint> _poisoned = new HashSet<uint>(); // 被动态下标 / CopyMemory / 块外写脏的变量
            private readonly Dictionary<int, (uint root, int[] idx)> _storePtr = new Dictionary<int, (uint, int[])>();
            private bool _unresolvedStore, _posFail, _smaskWrite, _inFunc;
            // 模块级变量的初始化器（函数外的 OpVariable … init）：走块前先当作一次全量写入。
            private readonly List<(uint var, uint val)> _globalInits = new List<(uint, uint)>();
            // OpLoad id → 叶子来源：attrLoc >= 0 为输入属性；否则沿内存最近一次写入继续溯源。
            private readonly Dictionary<uint, (int attrLoc, int attrComp, uint val, int[] suffix, bool fail)> _loadSrc =
                new Dictionary<uint, (int, int, uint, int[], bool)>();

            public Module(uint[] words) { _w = words; }

            private int Op(int at) => (int)(_w[at] & 0xffff);
            private int Wc(int at) => (int)(_w[at] >> 16);

            private static bool DefOp(int op) => op is OpExtInst or OpConstant or OpConstantComposite or OpConstantNull
                or OpVariable or OpLoad or OpAccessChain or OpInBoundsAccessChain or OpCompositeConstruct
                or OpCompositeExtract or OpCopyObject or OpFAdd or OpFMul;

            public Result Evaluate()
            {
                if (_w.Length < 6 || _w[0] != 0x07230203) return DefaultResult();
                for (int i = 5; i < _w.Length;)
                {
                    int wc = Wc(i), op = Op(i);
                    if (wc == 0 || i + wc > _w.Length) return DefaultResult();   // 截断：全保守
                    if (DefOp(op) && wc >= 3) _defs.TryAdd(_w[i + 2], i);
                    switch (op)
                    {
                        case OpExtInstImport when wc >= 3 && LiteralIs(i + 2, wc - 2, "GLSL.std.450"):
                            _glslImports.Add(_w[i + 1]); break;
                        case OpEntryPoint when wc >= 3:
                            if (_w[i + 1] == 0) _vertexEntry = _w[i + 2];
                            else if (_w[i + 1] == 4) _hasFragment = true;
                            break;
                        case OpDecorate when wc >= 4:
                            if (_w[i + 2] == DecorationLocation) _locations[_w[i + 1]] = _w[i + 3];
                            else if (_w[i + 2] == DecorationBuiltIn) _varBuiltin[_w[i + 1]] = _w[i + 3];
                            break;
                        case OpMemberDecorate when wc >= 5 && _w[i + 3] == DecorationBuiltIn:
                            _memberBuiltin[(_w[i + 1], _w[i + 2])] = _w[i + 4]; break;
                        case OpTypePointer when wc >= 4:
                            _pointee[_w[i + 1]] = _w[i + 3]; break;
                        case OpConstant when wc >= 4:
                            _constants[_w[i + 2]] = _w[i + 3]; break;
                        case OpVariable when wc >= 4:
                            _varStorage[_w[i + 2]] = _w[i + 3]; _varType[_w[i + 2]] = _w[i + 1];
                            if (wc >= 5 && !_inFunc) _globalInits.Add((_w[i + 2], _w[i + 4]));
                            break;
                        case OpFunction: _inFunc = true; break;
                        case OpFunctionEnd: _inFunc = false; break;
                        case OpStore:
                        case OpCopyMemory:
                            _storeInstrs.Add(i); break;
                        case OpKill:
                        case OpTerminateInvocation:
                        case OpDemoteToHelperInvocation:
                            _mayDiscard = true; break;
                    }
                    i += wc;
                }
                _hasVertex = _vertexEntry != 0;
                if (_hasVertex) FindEntryBlock();
                uint posVar = 0; int posMember = -1;
                bool posFound = _hasVertex && FindPositionVar(out posVar, out posMember);
                var smaskVars = _hasFragment ? SampleMaskVars() : new Dictionary<uint, int>();
                ScanStores(posFound ? posVar : 0, smaskVars);
                int posAttr = -1; bool posW1 = false, posZHalf = false;
                if (posFound && !_posFail && !_poisoned.Contains(posVar))
                    posAttr = VertexFacts(posVar, posMember, out posW1, out posZHalf);
                // 「不写 sample mask」才输出 false：写不了根的 store、动态下标写脏 sink、
                // 可解析写入覆盖 SampleMask 成员——任一命中都按会写处理。
                bool writesMask = !_hasFragment || _unresolvedStore || _smaskWrite;
                if (!writesMask)
                    foreach (uint var in smaskVars.Keys)
                        if (_poisoned.Contains(var)) { writesMask = true; break; }
                return new Result(_hasVertex, posAttr, posW1, posZHalf,
                                  _hasFragment, _mayDiscard, writesMask);
            }

            private Result DefaultResult() =>
                new Result(_hasVertex, -1, false, false, _hasFragment, true, true);

            private static bool DefBlockPoison(int op) => op is OpPhi or OpSelectionMerge or OpLoopMerge
                or OpBranch or OpBranchConditional or OpSwitch or OpKill or OpUnreachable or OpFunctionCall;

            // Vertex 入口函数体：单基本块才算「无条件路径」；分支/合并/phi/call 一律证不出。
            private void FindEntryBlock()
            {
                for (int i = 5; i < _w.Length;)
                {
                    int wc = Wc(i), op = Op(i);
                    if (wc == 0 || i + wc > _w.Length) return;
                    if (op == OpFunction && wc >= 3 && _w[i + 2] == _vertexEntry)
                    {
                        int labels = 0; bool terminated = false;
                        _blockStart = i + wc;
                        for (int j = _blockStart; j < _w.Length;)
                        {
                            int jc = Wc(j), jop = Op(j);
                            if (jc == 0 || j + jc > _w.Length) { _blockEnd = -1; return; }
                            if (jop == OpFunctionEnd) { _blockEnd = j; break; }
                            if (jop == OpLabel) { if (++labels > 1) { _blockEnd = -1; return; } }
                            else if (DefBlockPoison(jop)) { _blockEnd = -1; return; }
                            // 终结指令之后、FunctionEnd 之前的无标号尾巴不可达，不算执行路径。
                            if (jop == OpReturn || jop == OpReturnValue) terminated = true;
                            else if (!terminated) _blockOps.Add(j);
                            j += jc;
                        }
                        return;
                    }
                    i += wc;
                }
            }

            private bool IsBlockInstr(int at) => _blockEnd > _blockStart && at >= _blockStart && at < _blockEnd;

            // 指针解析：OpVariable / AccessChain / InBoundsAccessChain / CopyObject 链到根变量 +
            // 全常量下标路径。非常量下标 = dynamic；根证不出（phi / 参数 / 其他产线）= !ok。
            private (uint root, int[] idx, bool dyn, bool ok) Pointer(uint id, int depth = 0)
            {
                for (; depth < 8; depth++)
                {
                    if (!_defs.TryGetValue(id, out int at)) return (0, null, false, false);
                    int wc = Wc(at), op = Op(at);
                    if (op == OpVariable) return (id, EmptyPath, false, true);
                    if (op == OpCopyObject && wc >= 4) { id = _w[at + 3]; continue; }
                    if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 4)
                    {
                        var r = Pointer(_w[at + 3], depth + 1);
                        if (!r.ok) return r;
                        var idx = new int[r.idx.Length + wc - 4];
                        r.idx.CopyTo(idx, 0);
                        bool dyn = r.dyn;
                        for (int j = 0; j < wc - 4; j++)
                        {
                            if (_constants.TryGetValue(_w[at + 4 + j], out uint lit) && lit <= int.MaxValue)
                                idx[r.idx.Length + j] = (int)lit;
                            else { dyn = true; idx[r.idx.Length + j] = 0; }
                        }
                        return (r.root, idx, dyn, true);
                    }
                    return (0, null, false, false);
                }
                return (0, null, false, false);
            }

            // path 的最近覆盖写入：历史倒序找第一条下标是 path 前缀的项（单块内顺序执行，
            // 后写的覆盖先写的同一分量；分量级与整块级写入混用时各取最新的覆盖者）。
            private bool Covering(uint var, int[] path, out uint val, out int[] suffix)
            {
                val = 0; suffix = null;
                if (!_mem.TryGetValue(var, out var entries)) return false;
                for (int k = entries.Count - 1; k >= 0; k--)
                {
                    if (!Prefix(entries[k].idx, path)) continue;
                    val = entries[k].val; suffix = Sub(path, entries[k].idx);
                    return true;
                }
                return false;
            }

            private void Poison(uint root)
            {
                _poisoned.Add(root);
                _mem.Remove(root);
            }

            // 位置变量形态：Output 变量直接带 BuiltIn Position，或 Output 结构体变量的成员带
            // BuiltIn Position（gl_PerVertex 形态）；两种都认，找不到 / 多于一个都证不出。
            private bool FindPositionVar(out uint posVar, out int posMember)
            {
                posVar = 0; posMember = -1;
                foreach (var v in _varStorage)
                {
                    if (v.Value != StorageOutput) continue;
                    bool hit = _varBuiltin.TryGetValue(v.Key, out uint b) && b == BuiltInPosition;
                    int member = -1;
                    if (!hit && _pointee.TryGetValue(_varType[v.Key], out uint pointee))
                        foreach (var m in _memberBuiltin)
                            if (m.Key.Item1 == pointee && m.Value == BuiltInPosition) { hit = true; member = (int)m.Key.Item2; }
                    if (!hit) continue;
                    if (posVar != 0) return false;   // 两个候选：分不清哪个是「那个」位置
                    posVar = v.Key; posMember = member;
                }
                return posVar != 0;
            }

            // SampleMask sink：Output 变量直接带 BuiltIn SampleMask，或 Output 结构体成员带
            // BuiltIn SampleMask。值：var id → 成员号（-1 = 变量本身就是 mask）。
            private Dictionary<uint, int> SampleMaskVars()
            {
                var sinks = new Dictionary<uint, int>();
                foreach (var v in _varStorage)
                {
                    if (v.Value != StorageOutput) continue;
                    if (_varBuiltin.TryGetValue(v.Key, out uint b) && b == BuiltInSampleMask)
                    { sinks[v.Key] = -1; continue; }
                    if (!_pointee.TryGetValue(_varType[v.Key], out uint pointee)) continue;
                    foreach (var m in _memberBuiltin)
                        if (m.Key.Item1 == pointee && m.Value == BuiltInSampleMask) { sinks[v.Key] = (int)m.Key.Item2; break; }
                }
                return sinks;
            }

            // 全模块写扫描：位置变量的块外写入 / CopyMemory 写入 → 整个 VS 证明作废；
            // 写不了根的 store → 可能是位置写入也可能是 mask 写入，两边都按证不出处理；
            // 动态下标写 → 毒化那个变量；其余变量的块外写 → 只毒化那个变量。
            private void ScanStores(uint posVar, Dictionary<uint, int> smaskVars)
            {
                foreach (int at in _storeInstrs)
                {
                    uint ptr = _w[at + 1];
                    var p = Pointer(ptr);
                    if (!p.ok) { _unresolvedStore = true; _posFail = true; continue; }
                    if (p.dyn) { Poison(p.root); if (p.root == posVar) _posFail = true; continue; }
                    _storePtr[at] = (p.root, p.idx);
                    if (p.root == posVar)
                    {
                        if (!IsBlockInstr(at) || Op(at) != OpStore) _posFail = true;
                        continue;
                    }
                    if (smaskVars.TryGetValue(p.root, out int member))
                    {
                        if (member < 0 || p.idx.Length == 0 || p.idx[0] == member) _smaskWrite = true;
                        continue;
                    }
                    if (!IsBlockInstr(at)) _poisoned.Add(p.root);
                }
            }

            private List<(int[] idx, uint val)> Mem(uint var)
            {
                if (!_mem.TryGetValue(var, out var list)) _mem[var] = list = new List<(int[], uint)>();
                return list;
            }

            // 入口块顺序一遍：维护内存历史与每条 OpLoad 的来源。
            // 先放模块级变量初始化器——Private 全局的初值写在函数外、块内 store 之前。
            private void WalkBlock(uint posVar)
            {
                foreach (var g in _globalInits) Mem(g.var).Add((EmptyPath, g.val));
                foreach (int at in _blockOps)
                {
                    int wc = Wc(at), op = Op(at);
                    if (op == OpVariable)
                    {
                        if (wc >= 5) Mem(_w[at + 2]).Add((EmptyPath, _w[at + 4]));
                    }
                    else if (op == OpStore)
                    {
                        if (_storePtr.TryGetValue(at, out var s)) Mem(s.root).Add((s.idx, _w[at + 2]));
                    }
                    else if (op == OpCopyMemory)
                    {
                        if (_storePtr.TryGetValue(at, out var s)) Poison(s.root);
                    }
                    else if (op == OpLoad && wc >= 4)
                    {
                        uint id = _w[at + 2];
                        var p = Pointer(_w[at + 3]);
                        _loadSrc[id] = !p.ok || p.dyn
                            ? (-1, -1, 0, null, true)
                            : LoadSource(p.root, p.idx, posVar);
                    }
                }
            }

            // 单条 OpLoad 的来源：Input+Location = 属性叶子（attrComp -1 = 整向量装入）；
            // 位置变量 / 函数局部 / Private 变量 = 内存写入叶子；其余存储类证不出。
            private (int attrLoc, int attrComp, uint val, int[] suffix, bool fail) LoadSource(
                uint root, int[] idx, uint posVar)
            {
                uint storage = _varStorage.TryGetValue(root, out uint s) ? s : uint.MaxValue;
                if (storage == StorageInput)
                {
                    // 只认两级：裸装（整向量，attrComp=-1）与 var[c]（分量 c 直取）。
                    // 更长下标链（矩阵列、嵌套成员）分量归属证不出。
                    if (!_locations.TryGetValue(root, out uint loc) || loc > int.MaxValue)
                        return (-1, -1, 0, null, true);
                    if (idx.Length == 0) return ((int)loc, -1, 0, null, false);
                    if (idx.Length == 1 && idx[0] >= 0) return ((int)loc, idx[0], 0, null, false);
                    return (-1, -1, 0, null, true);
                }
                if (root == posVar)
                    return Covering(root, idx, out uint v, out int[] suffix)
                        ? (-1, -1, v, suffix, false) : (-1, -1, 0, null, true);
                if (storage == StorageFunction || storage == StoragePrivate)
                    return !_poisoned.Contains(root) && Covering(root, idx, out uint v, out int[] suffix)
                        ? (-1, -1, v, suffix, false) : (-1, -1, 0, null, true);
                return (-1, -1, 0, null, true);
            }

            // 沿透明链（Load 经 loadSrc / Construct / Extract 下降）到标量产线指令。
            // 命中属性 / 失败叶子 → leaf 给结论、返回 false；否则 at = 产线指令下标。
            private bool Descend(uint id, int[] path, out int at, out Prov leaf, int depth = 0)
            {
                for (; depth < 64; depth++)
                {
                    if (!_defs.TryGetValue(id, out at)) { leaf = Prov.Fail; return false; }
                    int wc = Wc(at), op = Op(at);
                    if (op == OpLoad)
                    {
                        if (!_loadSrc.TryGetValue(id, out var s) || s.fail) { leaf = Prov.Fail; return false; }
                        if (s.attrLoc >= 0)
                        {
                            // 链取到分量（attrComp>=0）时 path 必须已经走完；整向量装入
                            // （attrComp=-1）时 path 取的那个分量就是属性分量。
                            leaf = s.attrComp >= 0
                                ? (path.Length == 0 ? Prov.Attr(s.attrLoc, s.attrComp) : Prov.Fail)
                                : (path.Length == 1 && path[0] >= 0 ? Prov.Attr(s.attrLoc, path[0]) : Prov.Fail);
                            return false;
                        }
                        id = s.val; path = Concat(s.suffix, path); continue;
                    }
                    if (op == OpCompositeConstruct || op == OpConstantComposite)
                    {
                        // 成分操作数从 +3 起：type / id / 成分…
                        if (path.Length == 0 || path[0] < 0 || path[0] >= wc - 3) { leaf = Prov.Fail; return false; }
                        id = _w[at + 3 + path[0]]; path = Tail(path); continue;
                    }
                    if (op == OpCompositeExtract && wc >= 5)
                    {
                        // composite 在 +3，字面下标 +4 起（立即数，不是 id）。
                        var next = new int[wc - 4 + path.Length];
                        for (int j = 0; j < wc - 4; j++) next[j] = unchecked((int)_w[at + 4 + j]);
                        Array.Copy(path, 0, next, wc - 4, path.Length);
                        id = _w[at + 3]; path = next; continue;
                    }
                    leaf = default;
                    return true;
                }
                leaf = Prov.Fail; at = -1; return false;
            }

            private Prov Trace(uint id, int[] path)
            {
                if (!Descend(id, path, out int at, out Prov leaf)) return leaf;
                int op = Op(at);
                if (op == OpConstant && Wc(at) >= 4) return Prov.Const(_w[at + 3]);
                if (op == OpConstantNull) return Prov.Const(0);
                return Prov.Fail;
            }

            private static int[] Concat(int[] head, int[] tail)
            {
                if (head == null || head.Length == 0) return tail;
                if (tail == null || tail.Length == 0) return head;
                var r = new int[head.Length + tail.Length];
                head.CopyTo(r, 0); tail.CopyTo(r, head.Length);
                return r;
            }

            private static int[] Tail(int[] path)
            {
                var r = new int[path.Length - 1];
                Array.Copy(path, 1, r, 0, r.Length);
                return r;
            }

            // 位置变量第 c 分量的最终写入：找覆盖 memberPrefix+[c] 的最近一条历史项，
            // hitK 返回命中项在写入历史里的下标（z-half 判「改写前那次 z 写」要再往前找）。
            private bool PositionLeaf(uint posVar, int posMember, int comp,
                                      out uint val, out int[] suffix, out int hitK)
            {
                var path = posMember >= 0 ? new[] { posMember, comp } : new[] { comp };
                val = 0; suffix = null; hitK = -1;
                if (!_mem.TryGetValue(posVar, out var entries)) return false;
                for (int k = entries.Count - 1; k >= 0; k--)
                {
                    if (!Prefix(entries[k].idx, path)) continue;
                    val = entries[k].val; suffix = Sub(path, entries[k].idx); hitK = k;
                    return true;
                }
                return false;
            }

            private static bool Prefix(int[] e, int[] path)
            {
                if (e.Length > path.Length) return false;
                for (int j = 0; j < e.Length; j++) if (e[j] != path[j]) return false;
                return true;
            }

            private static int[] Sub(int[] path, int[] prefix)
            {
                var r = new int[path.Length - prefix.Length];
                Array.Copy(path, prefix.Length, r, 0, r.Length);
                return r;
            }

            private int VertexFacts(uint posVar, int posMember, out bool posW1, out bool posZHalf)
            {
                posW1 = false; posZHalf = false;
                WalkBlock(posVar);
                var compVal = new uint[4]; var compSuf = new int[4][]; var compK = new int[4];
                var comp = new Prov[4];
                for (int c = 0; c < 4; c++)
                {
                    comp[c] = !PositionLeaf(posVar, posMember, c, out compVal[c], out compSuf[c], out compK[c])
                        ? Prov.Fail : Trace(compVal[c], compSuf[c]);
                }
                // z-half：末次 z 写形如 zOp*0.5 + wOp*0.5，且 wOp 溯源 == 位置 w 的最终溯源
                // （回放侧 pos_w1 先改 P.w 再算 P.z*0.5+P.w*0.5，两边必须同源）；
                // 若存在「改写前那次 z 写」，zOp 的溯源还必须等于它的溯源。
                Prov zSrc = Prov.Fail;
                if (comp[3].Kind != 0 &&
                    TryZHalf(compVal[2], compSuf[2], comp[3], out zSrc) &&
                    PrevZMatches(posVar, posMember, compK[2], zSrc))
                    posZHalf = true;
                // pos_attr 的 z：直通就用直通溯源；过了 z-half 改写就用改写的 z 源。
                Prov zProv = comp[2].Kind != 0 ? comp[2] : posZHalf ? zSrc : Prov.Fail;
                int attr = -1;
                if (SameAttr(comp[0], 0, out attr) && SameAttr(comp[1], 1, out int a1) && a1 == attr &&
                    SameAttr(zProv, 2, out int a2) && a2 == attr && attr <= 15)
                {
                    // w：常量 1.0 → pos_w1；同 attr 的 w → 合法的 attr 直通形态；
                    // 其他一切（错分量、非常量、证不出）→ pos_attr 强制 -1。
                    if (comp[3].Kind == 2 && comp[3].Bits == BitsOne) posW1 = true;
                    else if (!(comp[3].Kind == 1 && comp[3].Loc == attr && comp[3].Comp == 3)) attr = -1;
                }
                else attr = -1;
                return attr;
            }

            private static bool SameAttr(Prov p, int wantComp, out int loc)
            {
                loc = p.Kind == 1 && p.Comp == wantComp ? p.Loc : -1;
                return loc >= 0;
            }

            private static bool SameProv(Prov a, Prov b) => a.Kind != 0 && a.Kind == b.Kind &&
                (a.Kind == 1 ? a.Loc == b.Loc && a.Comp == b.Comp : a.Bits == b.Bits);

            // 「改写前的 z」：末次 z 写（历史下标 lastK）之前覆盖同一分量的最近一次写入。
            // 存在则其溯源必须等于改写读到的 zSrc——链上读的正是这次旧 z；
            // 不存在（改写是分量唯一直接写）时无法对质，zSrc 已证非 Fail 即算过。
            private bool PrevZMatches(uint posVar, int posMember, int lastK, Prov zSrc)
            {
                if (lastK <= 0 || !_mem.TryGetValue(posVar, out var entries)) return true;
                var path = posMember >= 0 ? new[] { posMember, 2 } : new[] { 2 };
                for (int k = lastK - 1; k >= 0; k--)
                {
                    if (!Prefix(entries[k].idx, path)) continue;
                    return SameProv(zSrc, Trace(entries[k].val, Sub(path, entries[k].idx)));
                }
                return true;
            }

            // pos_z_half 形状：ExtInst GLSL.std.450 Fma(z,0.5, w*0.5)、两步 FAdd(FMul(z,0.5), FMul(w,0.5))
            // 或 FMul(FAdd(z,w), 0.5)。wOp 溯源必须等于 wProv（位置 w 最终溯源），
            // 两臂对称的 FAdd / FMul(FAdd) 靠「谁配 wProv」定角色，都配/都不配 = 歧义证不出；
            // 命中返回 z 操作数的溯源。链上撞 OpSelect 就是 Fail，运行时 select 尾巴天然证不出。
            private bool TryZHalf(uint val, int[] suffix, Prov wProv, out Prov zSrc)
            {
                zSrc = Prov.Fail;
                if (val == 0 || suffix == null || !Descend(val, suffix, out int at, out _)) return false;
                int wc = Wc(at), op = Op(at);
                if (op == OpExtInst && wc == 8 && _glslImports.Contains(_w[at + 3]) && _w[at + 4] == ExtInstFma)
                {
                    // fma(a,b,c)=a*b+c：{a,b} 恰一个是 0.5（另一个是 z），c 必须是 w*0.5。
                    uint zOp;
                    bool ah = IsHalf(_w[at + 5]), bh = IsHalf(_w[at + 6]);
                    if (ah == bh) return false;
                    zOp = ah ? _w[at + 6] : _w[at + 5];
                    if (!IsMulHalf(_w[at + 7], out uint wOp) || !SameProv(Trace(wOp, EmptyPath), wProv))
                        return false;
                    zSrc = Trace(zOp, EmptyPath);
                    return zSrc.Kind != 0;
                }
                if (op == OpFAdd && wc == 5)
                {
                    if (!IsMulHalf(_w[at + 3], out uint a) || !IsMulHalf(_w[at + 4], out uint b)) return false;
                    bool aw = SameProv(Trace(a, EmptyPath), wProv), bw = SameProv(Trace(b, EmptyPath), wProv);
                    if (aw == bw) return false;
                    zSrc = Trace(aw ? b : a, EmptyPath);
                    return zSrc.Kind != 0;
                }
                if (op == OpFMul && wc == 5)
                {
                    // (z+w)*0.5：一臂是 0.5，另一臂必须是 FAdd。
                    uint a = _w[at + 3], b = _w[at + 4];
                    bool ah = IsHalf(a), bh = IsHalf(b);
                    if (ah == bh) return false;
                    if (!Descend(ah ? b : a, EmptyPath, out int at2, out _) ||
                        Op(at2) != OpFAdd || Wc(at2) != 5) return false;
                    uint x = _w[at2 + 3], y = _w[at2 + 4];
                    bool xw = SameProv(Trace(x, EmptyPath), wProv), yw = SameProv(Trace(y, EmptyPath), wProv);
                    if (xw == yw) return false;
                    zSrc = Trace(xw ? y : x, EmptyPath);
                    return zSrc.Kind != 0;
                }
                return false;
            }

            // 形如 x*0.5 或 0.5*x 的 FMul：other = 非常量那个操作数。
            private bool IsMulHalf(uint id, out uint other)
            {
                other = 0;
                if (!Descend(id, EmptyPath, out int at, out _) || Op(at) != OpFMul || Wc(at) < 5) return false;
                uint a = _w[at + 3], b = _w[at + 4];
                bool ah = IsHalf(a), bh = IsHalf(b);
                if (ah == bh) return false;    // 两个都 0.5 或都不是：不是「src × 0.5」
                other = ah ? b : a;
                return true;
            }

            private bool IsHalf(uint id) { Prov p = Trace(id, EmptyPath); return p.Kind == 2 && p.Bits == BitsHalf; }

            private bool LiteralIs(int start, int count, string want)
            {
                if (count <= 0 || (long)count * 4 < want.Length + 1) return false;
                for (int i = 0; i < want.Length; i++)
                    if ((byte)(_w[start + i / 4] >> ((i % 4) * 8)) != (byte)want[i]) return false;
                return (byte)(_w[start + want.Length / 4] >> ((want.Length % 4) * 8)) == 0;
            }
        }
    }
}
