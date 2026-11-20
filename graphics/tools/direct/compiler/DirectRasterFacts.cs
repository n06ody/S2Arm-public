// 直连产物的光栅事实：对最终 SPIR-V 做 word 级 SSA 溯源，证得出才写，证不出回保守缺省。
// 字段与 C 侧解析器（android/native/nvn/nvn_vk_program_result.c 的 "raster" 段）一一对应：
//   pos_attr      — VS：out position 直通的那个顶点属性（Location）号：x,y 必须是
//                   该属性的 .x/.y，z 是该属性的 .z 或任意常量（回放侧覆盖证明只用
//                   xy），w 是常量 1.0 或该属性的 .w；z 过深度改写时指改写的 z 源
//                   （与 pos_z_half 组合使用）；证不出 = -1
//   pos_w1        — VS：out position.w 恒为常量 1.0f（该属性 .w 不被消费）
//   pos_z_half    — VS：写出位置的 z = z*0.5 + w*0.5（编译期 DepthMode 尾巴；运行时
//                   select 尾巴链条上有 OpSelect，一律证不出 = false）。成立时
//                   pos_attr 指改写源的属性——回放侧按 rp_draw.c 的顺序先读属性
//                   再对 P.z 重做同一变换。
//   pos_src       — VS：位置输出来源分类：0=complex（证不出/缺省，C 侧缺省 0）
//                   1=attr（pos_attr≥0 形态）2=const（四分量全常量，见 pos_const）
//                   3=vidgen（四分量表达式叶子只有 {常量, gl_VertexIndex}，编译期
//                   对 vid=0..63 逐点求值，见 pos_vtab）
//   pos_const     — VS：仅 pos_src==2：四分量 float 位模式 [x,y,z,w]
//   pos_vtab      — VS：仅 pos_src==3：vid=0..63 各点 vec4 位模式，vid*4+comp 排（256 字）
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
            public readonly int PosSrc;             // 0=complex 1=attr 2=const 3=vidgen
            public readonly uint[] PosConst;        // PosSrc==2：四分量位模式；否则 null
            public readonly uint[] PosVTab;         // PosSrc==3：vid*4+comp 表（256 字）；否则 null
            public readonly bool HasFragment;
            public readonly bool MayDiscard;        // 缺省 true = 可能丢
            public readonly bool WritesSampleMask;  // 缺省 true = 可能写
            public Result(bool hasVertex, int posAttr, bool posW1, bool posZHalf,
                          int posSrc, uint[] posConst, uint[] posVTab,
                          bool hasFragment, bool mayDiscard, bool writesSampleMask)
            {
                HasVertex = hasVertex; PosAttr = posAttr; PosW1 = posW1; PosZHalf = posZHalf;
                PosSrc = posSrc; PosConst = posConst; PosVTab = posVTab;
                HasFragment = hasFragment; MayDiscard = mayDiscard; WritesSampleMask = writesSampleMask;
            }
        }

        public static Result Analyze(ReadOnlySpan<byte> spirv)
        {
            if (spirv.Length < 24 || spirv.Length % 4 != 0)
                return new Result(false, -1, false, false, 0, null, null, false, true, true);
            return new Module(MemoryMarshal.Cast<byte, uint>(spirv).ToArray()).Evaluate();
        }

        // 指令集里只登记这些产线/指针 op；其余（phi、shuffle、算术、位转……）不登记 id，
        // trace 撞上就是 Fail——与「不能溯源 = 证不出」同义，不靠逐个枚举报乐观。
        private const int OpExtInstImport = 11;
        private const int OpExtInst = 12;
        private const int OpEntryPoint = 15;
        private const int OpTypeInt = 21;
        private const int OpTypeFloat = 22;
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
        private const int OpConvertSToF = 111;
        private const int OpConvertUToF = 112;
        private const int OpSNegate = 126;
        private const int OpFNegate = 127;
        private const int OpIAdd = 128;
        private const int OpFAdd = 129;
        private const int OpISub = 130;
        private const int OpFSub = 131;
        private const int OpIMul = 132;
        private const int OpFMul = 133;
        private const int OpShiftRightLogical = 194;
        private const int OpShiftRightArithmetic = 195;
        private const int OpShiftLeftLogical = 196;
        private const int OpBitwiseAnd = 199;
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
        // SPIR-V BuiltIn 编号：Position=0、SampleMask=20、VertexIndex=42（gl_VertexIndex）。
        private const int BuiltInPosition = 0, BuiltInSampleMask = 20, BuiltInVertexIndex = 42;
        private const int ExtInstFma = 50;                               // GLSL.std.450 Fma
        private const uint BitsHalf = 0x3F000000u, BitsOne = 0x3F800000u;
        private const int VidPoints = 64, VidEvalBudget = 512;           // vidgen 逐点求值：vid 数与节点上限
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

        // vidgen 逐点求值的值域：i32（环绕语义）或 f32；两域只经显式 Convert 互转，
        // 域不匹配的 op 一律证不出，不猜类型提升。
        private readonly struct VNum
        {
            public readonly bool F32;
            public readonly int I;
            public readonly float F;
            private VNum(bool f32, int i, float f) { F32 = f32; I = i; F = f; }
            public static VNum OfI(int i) => new VNum(false, i, 0);
            public static VNum OfF(float f) => new VNum(true, 0, f);
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
            // OpTypeInt / OpTypeFloat 的位宽登记：vidgen 常量叶子只认 32 位标量类型。
            private readonly Dictionary<uint, (bool isFloat, int width)> _primType =
                new Dictionary<uint, (bool, int)>();
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

            // vidgen 白名单算术 op 也登记（登记 ≠ 可证：Trace 仍只认常量/属性叶子，
            // EvalVid 另按白名单逐语义求值；登记只是让求值器能看见产线指令）。
            private static bool DefOp(int op) => op is OpExtInst or OpConstant or OpConstantComposite or OpConstantNull
                or OpVariable or OpLoad or OpAccessChain or OpInBoundsAccessChain or OpCompositeConstruct
                or OpCompositeExtract or OpCopyObject or OpFAdd or OpFMul
                or OpConvertSToF or OpConvertUToF or OpSNegate or OpFNegate or OpIAdd or OpISub or OpIMul or OpFSub
                or OpShiftRightLogical or OpShiftRightArithmetic or OpShiftLeftLogical or OpBitwiseAnd;

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
                        case OpTypeInt when wc >= 4:
                        case OpTypeFloat when wc >= 3:
                            _primType[_w[i + 1]] = (op == OpTypeFloat, (int)_w[i + 2]); break;
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
                int posSrc = 0; uint[] posConst = null, posVTab = null;
                if (posFound && !_posFail && !_poisoned.Contains(posVar))
                {
                    posAttr = VertexFacts(posVar, posMember, out posW1, out posZHalf,
                                          out uint[] cval, out int[][] csuf, out Prov[] cprov, out int[] ck);
                    // 位置来源分类按 attr → const → vidgen 顺序证：attr 证出即 1；
                    // 四分量全常量即 2；四分量各自能对 vid=0..63 逐点求值出 float 即 3；
                    // 都不中保持 0，绝不乐观。
                    if (posAttr >= 0)
                        posSrc = 1;
                    else if (cprov[0].Kind == 2 && cprov[1].Kind == 2 && cprov[2].Kind == 2 && cprov[3].Kind == 2)
                    {
                        posSrc = 2;
                        posConst = new[] { cprov[0].Bits, cprov[1].Bits, cprov[2].Bits, cprov[3].Bits };
                    }
                    else if (TryVidTab(posVar, posMember, cval, csuf, ck, out posVTab, out bool vidZHalf))
                    {
                        posSrc = 3;
                        // src3 的 pos_z_half 语义只看 vidgen 自己的判定：true = 表内是
                        // pre-tail 的 z（C 侧照 attr 路径同式重做 z=z*0.5+w*0.5）；
                        // z 直算成功时表内已是出口值，不许再带 attr 路径留下的旧标记。
                        posZHalf = vidZHalf;
                    }
                }
                // 「不写 sample mask」才输出 false：写不了根的 store、动态下标写脏 sink、
                // 可解析写入覆盖 SampleMask 成员——任一命中都按会写处理。
                bool writesMask = !_hasFragment || _unresolvedStore || _smaskWrite;
                if (!writesMask)
                    foreach (uint var in smaskVars.Keys)
                        if (_poisoned.Contains(var)) { writesMask = true; break; }
                return new Result(_hasVertex, posAttr, posW1, posZHalf, posSrc, posConst, posVTab,
                                  _hasFragment, _mayDiscard, writesMask);
            }

            private Result DefaultResult() =>
                new Result(_hasVertex, -1, false, false, 0, null, null, _hasFragment, true, true);

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

            // compVal/compSuf/comp/compK 输出给 Evaluate 的 pos_src 分类用：各分量末次覆盖
            // 写入的值 id、分量路径、透明链溯源结论与命中项在写入历史里的下标；
            // PositionLeaf 不中的分量 comp[c]=Fail、compSuf[c]=null。
            private int VertexFacts(uint posVar, int posMember, out bool posW1, out bool posZHalf,
                                    out uint[] compVal, out int[][] compSuf, out Prov[] comp, out int[] compK)
            {
                posW1 = false; posZHalf = false;
                WalkBlock(posVar);
                compVal = new uint[4]; compSuf = new int[4][]; compK = new int[4];
                comp = new Prov[4];
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
                // 放宽：z ∈ {同 attr 的 .z, 任意常量}——回放侧覆盖证明只用 xy；
                // z 其他形态仍判证不出（|z|>w 会被裁剪的改写不归本类）。
                Prov zProv = comp[2].Kind != 0 ? comp[2] : posZHalf ? zSrc : Prov.Fail;
                int attr = -1;
                if (SameAttr(comp[0], 0, out attr) && SameAttr(comp[1], 1, out int a1) && a1 == attr &&
                    (zProv.Kind == 2 || (SameAttr(zProv, 2, out int a2) && a2 == attr)) && attr <= 15)
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

            // 编译期深度尾巴三种形状的共用拆解：ExtInst GLSL.std.450 Fma(z,0.5, w*0.5)、
            // 两步 FAdd(FMul(z,0.5), FMul(w,0.5)) 或 FMul(FAdd(z,w), 0.5)。
            // wMatch 由调用方给「谁算 w」的判据：attr 路径是 SameProv(Trace(op), wProv)，
            // vidgen 路径是「逐点求值位模式 == 已填表 w 列」；两臂对称的 FAdd / FMul(FAdd)
            // 靠谁配 wMatch 定角色，都配/都不配 = 歧义证不出。命中返回 z 源操作数 id（zOp）。
            // 链上撞 OpSelect 就是证不出，运行时 select 尾巴天然证不出。
            private bool ZHalfTail(uint val, int[] suffix, Func<uint, bool> wMatch, out uint zOp)
            {
                zOp = 0;
                if (val == 0 || suffix == null || !Descend(val, suffix, out int at, out _)) return false;
                int wc = Wc(at), op = Op(at);
                if (op == OpExtInst && wc == 8 && _glslImports.Contains(_w[at + 3]) && _w[at + 4] == ExtInstFma)
                {
                    // fma(a,b,c)=a*b+c：{a,b} 恰一个是 0.5（另一个是 z），c 必须是 w*0.5。
                    bool ah = IsHalf(_w[at + 5]), bh = IsHalf(_w[at + 6]);
                    if (ah == bh) return false;
                    uint z = ah ? _w[at + 6] : _w[at + 5];
                    if (!IsMulHalf(_w[at + 7], out uint wOp) || !wMatch(wOp)) return false;
                    zOp = z;
                    return true;
                }
                if (op == OpFAdd && wc == 5)
                {
                    if (!IsMulHalf(_w[at + 3], out uint a) || !IsMulHalf(_w[at + 4], out uint b)) return false;
                    bool aw = wMatch(a), bw = wMatch(b);
                    if (aw == bw) return false;
                    zOp = aw ? b : a;
                    return true;
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
                    bool xw = wMatch(x), yw = wMatch(y);
                    if (xw == yw) return false;
                    zOp = xw ? y : x;
                    return true;
                }
                return false;
            }

            // attr 路径的 z-half：形状命中后对 zOp 做溯源，w 匹配谓词是
            // SameProv(Trace(op), wProv)（位置 w 的最终溯源），语义照旧。
            private bool TryZHalf(uint val, int[] suffix, Prov wProv, out Prov zSrc)
            {
                zSrc = Prov.Fail;
                if (!ZHalfTail(val, suffix, op => SameProv(Trace(op, EmptyPath), wProv), out uint zOp))
                    return false;
                zSrc = Trace(zOp, EmptyPath);
                return zSrc.Kind != 0;
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

            // vidgen 表：x,y,w 直接逐点求值填表；z 先同款求值，失败再认编译期深度尾巴
            // （与 attr 的 TryZHalf 同一三形——Fma/FAdd(FMul)/FMul(FAdd) 里 Fma 不在求值
            // 白名单，不看穿尾巴真机上 vidgen 全 0）。zHalf 输出表内 z 是不是 pre-tail。
            private bool TryVidTab(uint posVar, int posMember, uint[] compVal, int[][] compSuf,
                                   int[] compK, out uint[] tab, out bool zHalf)
            {
                tab = null; zHalf = false;
                var t = new uint[VidPoints * 4];
                foreach (int c in new[] { 0, 1, 3 })
                {
                    if (compSuf[c] == null || !EvalVidColumn(compVal[c], compSuf[c], out uint[] bits))
                        return false;
                    for (int vid = 0; vid < VidPoints; vid++) t[vid * 4 + c] = bits[vid];
                }
                // z 直算成功：表内已是出口值（白名单内的两步式尾巴会被原样求值掉，不另标）。
                if (compSuf[2] != null && EvalVidColumn(compVal[2], compSuf[2], out uint[] zDirect))
                {
                    for (int vid = 0; vid < VidPoints; vid++) t[vid * 4 + 2] = zDirect[vid];
                    tab = t;
                    return true;
                }
                // z 直算不中：编译期深度尾巴看穿，表内填 pre-tail 的 z 并标 zHalf。
                if (!TryVidZHalf(posVar, posMember, compVal[2], compSuf[2], compK[2], t, out uint[] zTail))
                    return false;
                for (int vid = 0; vid < VidPoints; vid++) t[vid * 4 + 2] = zTail[vid];
                zHalf = true; tab = t;
                return true;
            }

            // vidgen 的 z 深度尾巴：形状同 attr 的 TryZHalf 三形；wOp 逐点求值必须与
            // 已填表 w 列位模式逐 vid 相等；存在「改写前那次 z 写」时其表达式也要与
            // zOp 逐点相等；zOp 逐点求值出的就是写进表的 pre-tail z 列。
            private bool TryVidZHalf(uint posVar, int posMember, uint zVal, int[] zSuf, int lastK,
                                     uint[] tab, out uint[] zBits)
            {
                zBits = null;
                if (zVal == 0 || zSuf == null) return false;
                if (!ZHalfTail(zVal, zSuf, wOp => SameVidBits(wOp, tab, 3), out uint zOp)) return false;
                if (!PrevZVidMatch(posVar, posMember, lastK, zOp)) return false;
                return EvalVidColumn(zOp, EmptyPath, out zBits);
            }

            // w 匹配谓词的 vidgen 版：id 逐点求值的位模式与已填表 comp 列逐 vid 相等。
            // Prov 表示不了 vid 表达式，用「逐点求值位模式相等」替代 SameProv。
            private bool SameVidBits(uint id, uint[] tab, int comp)
            {
                for (int vid = 0; vid < VidPoints; vid++)
                    if (!EvalVid(id, EmptyPath, vid, out float f) ||
                        BitConverter.SingleToUInt32Bits(f) != tab[vid * 4 + comp])
                        return false;
                return true;
            }

            // PrevZMatches 的 vidgen 版：「改写前那次 z 写」存在时，其表达式逐点求值
            // 必须与 zOp 逐点相等（位模式）；不存在同前规则算过。
            private bool PrevZVidMatch(uint posVar, int posMember, int lastK, uint zOp)
            {
                if (lastK <= 0 || !_mem.TryGetValue(posVar, out var entries)) return true;
                var path = posMember >= 0 ? new[] { posMember, 2 } : new[] { 2 };
                for (int k = lastK - 1; k >= 0; k--)
                {
                    if (!Prefix(entries[k].idx, path)) continue;
                    int[] suf = Sub(path, entries[k].idx);
                    for (int vid = 0; vid < VidPoints; vid++)
                    {
                        if (!EvalVid(entries[k].val, suf, vid, out float a) ||
                            !EvalVid(zOp, EmptyPath, vid, out float b) ||
                            BitConverter.SingleToUInt32Bits(a) != BitConverter.SingleToUInt32Bits(b))
                            return false;
                    }
                    return true;
                }
                return true;
            }

            // 单分量整列逐点求值：vid=0..63 全过才给位模式列。
            private bool EvalVidColumn(uint id, int[] path, out uint[] bits)
            {
                bits = null;
                var t = new uint[VidPoints];
                for (int vid = 0; vid < VidPoints; vid++)
                {
                    if (!EvalVid(id, path, vid, out float f)) return false;
                    t[vid] = BitConverter.SingleToUInt32Bits(f);
                }
                bits = t;
                return true;
            }

            // 单分量单点求值：budget 防链式展开爆量；根必须求出 f32（位置分量是 f32，
            // 整型结果直通 f32 槽位不是合法形态，不猜位转语义）。
            private bool EvalVid(uint id, int[] path, int vid, out float f)
            {
                f = 0;
                int budget = VidEvalBudget;
                if (!EvalVidNode(id, path, vid, ref budget, out VNum v) || !v.F32) return false;
                f = v.F;
                return true;
            }

            // 沿 Descend 同款透明链下潜（Load 经 loadSrc / Construct / Extract / CopyObject），
            // 叶子只能是 VertexIndex 或常量；下潜到底后对产线指令做白名单求值。
            // 产线指令处仍有剩余分量路径 = 向量产物被取分量，不在证的范围，证不出。
            private bool EvalVidNode(uint id, int[] path, int vid, ref int budget, out VNum v)
            {
                v = default;
                for (int depth = 0; depth < 64; depth++)
                {
                    if (--budget < 0 || !_defs.TryGetValue(id, out int at)) return false;
                    int wc = Wc(at), op = Op(at);
                    if (op == OpLoad && wc >= 4)
                    {
                        // VertexIndex 叶子：Input 变量带 BuiltIn VertexIndex(42) 的标量装载。
                        // loadSrc 对它记 fail（Input 无 Location），先按内建身份判。
                        var p = Pointer(_w[at + 3]);
                        if (p.ok && !p.dyn && p.idx.Length == 0 &&
                            _varStorage.TryGetValue(p.root, out uint st) && st == StorageInput &&
                            _varBuiltin.TryGetValue(p.root, out uint b) && b == BuiltInVertexIndex)
                        {
                            v = VNum.OfI(vid);
                            return true;
                        }
                        if (!_loadSrc.TryGetValue(id, out var s) || s.fail || s.attrLoc >= 0) return false;
                        id = s.val; path = Concat(s.suffix, path); continue;
                    }
                    if (op == OpCopyObject && wc >= 4) { id = _w[at + 3]; continue; }
                    if (op == OpCompositeConstruct || op == OpConstantComposite)
                    {
                        if (path.Length == 0 || path[0] < 0 || path[0] >= wc - 3) return false;
                        id = _w[at + 3 + path[0]]; path = Tail(path); continue;
                    }
                    if (op == OpCompositeExtract && wc >= 5)
                    {
                        var next = new int[wc - 4 + path.Length];
                        for (int j = 0; j < wc - 4; j++) next[j] = unchecked((int)_w[at + 4 + j]);
                        Array.Copy(path, 0, next, wc - 4, path.Length);
                        id = _w[at + 3]; path = next; continue;
                    }
                    if (path.Length != 0) return false;
                    return EvalVidOp(at, vid, ref budget, out v);
                }
                return false;
            }

            // 白名单求值：常量叶子 + 一元/二元整型、位、移位、浮点算术、S/U→F 转换。
            // op 不在表内（Select、位转、向量混洗、除余……）一律证不出，宁可不证也不乐观；
            // 移位量越界在 SPIR-V 是未定义行为，同样证不出。
            private bool EvalVidOp(int at, int vid, ref int budget, out VNum v)
            {
                v = default;
                int wc = Wc(at), op = Op(at);
                switch (op)
                {
                    case OpConstant when wc >= 4:
                        return LeafConst(_w[at + 1], _w[at + 3], out v);
                    case OpConstantNull when wc >= 3:
                        return LeafConst(_w[at + 1], 0, out v);
                    case OpSNegate:
                    case OpFNegate:
                    case OpConvertSToF:
                    case OpConvertUToF:
                    {
                        if (wc < 4 || !EvalVidNode(_w[at + 3], EmptyPath, vid, ref budget, out VNum a))
                            return false;
                        switch (op)
                        {
                            case OpSNegate when !a.F32: v = VNum.OfI(unchecked(-a.I)); return true;
                            case OpFNegate when a.F32: v = VNum.OfF(-a.F); return true;
                            case OpConvertSToF when !a.F32: v = VNum.OfF(a.I); return true;
                            case OpConvertUToF when !a.F32: v = VNum.OfF(unchecked((uint)a.I)); return true;
                        }
                        return false;
                    }
                    case OpIAdd:
                    case OpISub:
                    case OpIMul:
                    case OpBitwiseAnd:
                    case OpShiftLeftLogical:
                    case OpShiftRightLogical:
                    case OpShiftRightArithmetic:
                    {
                        if (wc < 5 || !EvalVidNode(_w[at + 3], EmptyPath, vid, ref budget, out VNum a) ||
                            !EvalVidNode(_w[at + 4], EmptyPath, vid, ref budget, out VNum b))
                            return false;
                        if (a.F32 || b.F32) return false;
                        switch (op)
                        {
                            case OpIAdd: v = VNum.OfI(unchecked(a.I + b.I)); return true;
                            case OpISub: v = VNum.OfI(unchecked(a.I - b.I)); return true;
                            case OpIMul: v = VNum.OfI(unchecked(a.I * b.I)); return true;
                            case OpBitwiseAnd: v = VNum.OfI(a.I & b.I); return true;
                        }
                        if (b.I < 0 || b.I > 31) return false;
                        switch (op)
                        {
                            case OpShiftLeftLogical: v = VNum.OfI(a.I << b.I); return true;
                            case OpShiftRightLogical: v = VNum.OfI(unchecked((int)((uint)a.I >> b.I))); return true;
                            case OpShiftRightArithmetic: v = VNum.OfI(a.I >> b.I); return true;
                        }
                        return false;
                    }
                    case OpFAdd:
                    case OpFSub:
                    case OpFMul:
                    {
                        if (wc < 5 || !EvalVidNode(_w[at + 3], EmptyPath, vid, ref budget, out VNum a) ||
                            !EvalVidNode(_w[at + 4], EmptyPath, vid, ref budget, out VNum b))
                            return false;
                        if (!a.F32 || !b.F32) return false;
                        v = VNum.OfF(op == OpFAdd ? a.F + b.F : op == OpFSub ? a.F - b.F : a.F * b.F);
                        return true;
                    }
                }
                return false;
            }

            // 常量叶子：只认 32 位标量类型（i64/f64 立即数跨两字、布尔、向量都不猜）。
            private bool LeafConst(uint typeId, uint bits, out VNum v)
            {
                v = default;
                if (!_primType.TryGetValue(typeId, out var t) || t.width != 32) return false;
                v = t.isFloat ? VNum.OfF(BitConverter.UInt32BitsToSingle(bits)) : VNum.OfI(unchecked((int)bits));
                return true;
            }

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
