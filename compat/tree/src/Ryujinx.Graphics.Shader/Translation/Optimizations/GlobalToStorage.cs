using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    /// <summary>
    /// S2A：单目标存储缓冲访存在翻译期能静态看出偏移为负（或带负常量）的位置，由 TranslatorContext 带程序哈希提示
    /// （整体负常量报红、寄存器 + 负常量报黄；只在钳位生效时收集）。
    /// </summary>
    /// <param name="CbSlot">存放存储缓冲基址的常量缓冲槽</param>
    /// <param name="CbOffset">基址在该常量缓冲内的字偏移</param>
    /// <param name="Offset">Definite 时为完整常量偏移，否则为寄存器之外的常量部分</param>
    /// <param name="Definite">偏移整体是常量（确定为负）；否则是寄存器 + 负常量（可能为负）</param>
    readonly record struct StorageOffsetFinding(int CbSlot, int CbOffset, long Offset, bool Definite);

    /// <summary>
    /// S2A（F1）：GlobalToStorage 的翻译期附加输入输出：静态负偏移收集（报红用）、越界记录里写的本阶段身份，
    /// 以及本阶段没有越界记录绑定时「只检查不记录」的访存处数（报黄用）。
    /// </summary>
    /// <param name="Stage">翻译中的阶段</param>
    /// <param name="CodeHashLow">本阶段客体代码 XXHash128 的 Low</param>
    /// <param name="CodeHashHigh">本阶段客体代码 XXHash128 的 High</param>
    sealed class StorageRangeContext(ShaderStage stage, ulong codeHashLow, ulong codeHashHigh)
    {
        public List<StorageOffsetFinding> Findings { get; } = [];
        public ShaderStage Stage { get; } = stage;
        public ulong CodeHashLow { get; } = codeHashLow;
        public ulong CodeHashHigh { get; } = codeHashHigh;

        /// <summary>单目标访存处数：仍按宿主窗口检查并钳位，但越界不进记录缓冲、不报红。</summary>
        public int UnrecordedSingleTarget { get; private set; }

        /// <summary>多目标访存处数：同上。</summary>
        public int UnrecordedMultiTarget { get; private set; }

        public void CountUnrecorded(bool isMultiTarget)
        {
            if (isMultiTarget)
            {
                UnrecordedMultiTarget++;
            }
            else
            {
                UnrecordedSingleTarget++;
            }
        }
    }

    static class GlobalToStorage
    {
        private static Operand StorageWindowMask(IGpuAccessor gpuAccessor) =>
            gpuAccessor.QueryS2ALateStorageBufferAlignment()
                ? new Operand(OperandType.StorageWindowMask, 0)
                : Const(-gpuAccessor.QueryHostStorageBufferOffsetAlignment());

        private const int DriverReservedCb = 0;

        enum LsMemoryType
        {
            Local,
            Shared,
        }

        private class GtsContext
        {
            private readonly struct Entry
            {
                public readonly int FunctionId;
                public readonly Instruction Inst;
                public readonly StorageKind StorageKind;
                public readonly bool IsMultiTarget;
                public readonly IReadOnlyList<uint> TargetCbs;

                public Entry(
                    int functionId,
                    Instruction inst,
                    StorageKind storageKind,
                    bool isMultiTarget,
                    IReadOnlyList<uint> targetCbs)
                {
                    FunctionId = functionId;
                    Inst = inst;
                    StorageKind = storageKind;
                    IsMultiTarget = isMultiTarget;
                    TargetCbs = targetCbs;
                }
            }

            private readonly struct LsKey : IEquatable<LsKey>
            {
                public readonly Operand BaseOffset;
                public readonly int ConstOffset;
                public readonly LsMemoryType Type;

                public LsKey(Operand baseOffset, int constOffset, LsMemoryType type)
                {
                    BaseOffset = baseOffset;
                    ConstOffset = constOffset;
                    Type = type;
                }

                public override int GetHashCode()
                {
                    return HashCode.Combine(BaseOffset, ConstOffset, Type);
                }

                public override bool Equals(object obj)
                {
                    return obj is LsKey other && Equals(other);
                }

                public bool Equals(LsKey other)
                {
                    return other.BaseOffset == BaseOffset && other.ConstOffset == ConstOffset && other.Type == Type;
                }
            }

            private readonly List<Entry> _entries;
            private readonly Dictionary<LsKey, Dictionary<uint, SearchResult>> _sharedEntries;
            private readonly HelperFunctionManager _hfm;

            // S2A：Ampere 代码（ShaderDefinitions.Ampere）：参考实现 的两处改动（统一寄存器相加的源序、追不到存储缓冲的原始指针访问）只对它生效
            public bool Ampere { get; init; }

            // S2Arm: the raw pool table search shared by the shader's raw access helpers (GetRawLookup), and the private words of their range cache
            public int RawLookupId { get; set; } = -1;
            public int[] RawCache { get; set; }

            // The last raw access GlobalToStorage made, and its block: a later word of a vector access right after it takes its entry (TryGenerateRawStorageOp)
            public Operation RawGroupHead { get; set; }
            public BasicBlock RawGroupBlock { get; set; }

            private readonly Dictionary<(Instruction, StorageKind), int> _rawPieces = [];

            public int AddRawLookup(Function function)
            {
                return _hfm.AddFunction(function);
            }

            public int AddRawPiece(Operation baseOp, Function function)
            {
                int functionId = _hfm.AddFunction(function);

                _rawPieces.Add((baseOp.Inst, baseOp.StorageKind), functionId);

                return functionId;
            }

            public bool TryGetRawPieceId(Operation baseOp, out int functionId)
            {
                return _rawPieces.TryGetValue((baseOp.Inst, baseOp.StorageKind), out functionId);
            }

            public GtsContext(HelperFunctionManager hfm)
            {
                _entries = [];
                _sharedEntries = new Dictionary<LsKey, Dictionary<uint, SearchResult>>();
                _hfm = hfm;
            }

            public int AddFunction(Operation baseOp, bool isMultiTarget, IReadOnlyList<uint> targetCbs, Function function)
            {
                int functionId = _hfm.AddFunction(function);

                _entries.Add(new Entry(functionId, baseOp.Inst, baseOp.StorageKind, isMultiTarget, targetCbs));

                return functionId;
            }

            public bool TryGetFunctionId(Operation baseOp, bool isMultiTarget, List<uint> targetCbs, out int functionId)
            {
                foreach (Entry entry in _entries)
                {
                    if (entry.Inst != baseOp.Inst ||
                        entry.StorageKind != baseOp.StorageKind ||
                        entry.IsMultiTarget != isMultiTarget ||
                        entry.TargetCbs.Count != targetCbs.Count)
                    {
                        continue;
                    }

                    bool allEqual = true;

                    for (int index = 0; index < targetCbs.Count; index++)
                    {
                        if (targetCbs[index] != entry.TargetCbs[index])
                        {
                            allEqual = false;
                            break;
                        }
                    }

                    if (allEqual)
                    {
                        functionId = entry.FunctionId;
                        return true;
                    }
                }

                functionId = -1;
                return false;
            }

            public void AddMemoryTargetCb(LsMemoryType type, Operand baseOffset, int constOffset, uint targetCb, SearchResult result)
            {
                LsKey key = new(baseOffset, constOffset, type);

                if (!_sharedEntries.TryGetValue(key, out Dictionary<uint, SearchResult> targetCbs))
                {
                    // No entry with this base offset, create a new one.

                    targetCbs = new Dictionary<uint, SearchResult>() { { targetCb, result } };

                    _sharedEntries.Add(key, targetCbs);
                }
                else if (targetCbs.TryGetValue(targetCb, out SearchResult existingResult))
                {
                    // If our entry already exists, but does not match the new result,
                    // we set the offset to null to indicate there are multiple possible offsets.
                    // This will be used on the multi-target access that does not need to know the offset.

                    if (existingResult.Offset != null &&
                        (existingResult.Offset != result.Offset ||
                        existingResult.ConstOffset != result.ConstOffset))
                    {
                        targetCbs[targetCb] = new SearchResult(result.SbCbSlot, result.SbCbOffset);
                    }
                }
                else
                {
                    // An entry for this base offset already exists, but not for the specified
                    // constant buffer region where the storage buffer base address and size
                    // comes from.

                    targetCbs.Add(targetCb, result);
                }
            }

            public bool TryGetMemoryTargetCb(LsMemoryType type, Operand baseOffset, int constOffset, out SearchResult result)
            {
                LsKey key = new(baseOffset, constOffset, type);

                if (_sharedEntries.TryGetValue(key, out Dictionary<uint, SearchResult> targetCbs) && targetCbs.Count == 1)
                {
                    SearchResult candidateResult = targetCbs.Values.First();

                    if (candidateResult.Found)
                    {
                        result = candidateResult;

                        return true;
                    }
                }

                result = default;

                return false;
            }
        }

        private readonly struct SearchResult
        {
            public static SearchResult NotFound => new(-1, 0);
            public bool Found => SbCbSlot != -1;
            public int SbCbSlot { get; }
            public int SbCbOffset { get; }
            public Operand Offset { get; }
            public int ConstOffset { get; }

            public SearchResult(int sbCbSlot, int sbCbOffset)
            {
                SbCbSlot = sbCbSlot;
                SbCbOffset = sbCbOffset;
            }

            public SearchResult(int sbCbSlot, int sbCbOffset, Operand offset, int constOffset = 0)
            {
                SbCbSlot = sbCbSlot;
                SbCbOffset = sbCbOffset;
                Offset = offset;
                ConstOffset = constOffset;
            }
        }

        public static void RunPass(
            HelperFunctionManager hfm,
            BasicBlock[] blocks,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            StorageRangeContext storageRange = null,
            bool ampere = false)
        {
            GtsContext gtsContext = new(hfm) { Ampere = ampere };

            foreach (BasicBlock block in blocks)
            {
                for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
                {
                    if (node.Value is not Operation operation)
                    {
                        continue;
                    }

                    if (IsGlobalMemory(operation.StorageKind))
                    {
                        LinkedListNode<INode> nextNode = ReplaceGlobalMemoryWithStorage(
                            gtsContext,
                            resourceManager,
                            gpuAccessor,
                            targetLanguage,
                            storageRange,
                            block,
                            node);

                        if (nextNode == null)
                        {
                            // The returned value being null means that the global memory replacement failed,
                            // so we just make loads read 0 and stores do nothing.

                            gpuAccessor.Log($"Failed to reserve storage buffer for global memory operation \"{operation.Inst}\".");

                            if (operation.Dest != null)
                            {
                                operation.TurnIntoCopy(Const(0));
                            }
                            else
                            {
                                Utils.DeleteNode(node, operation);
                            }
                        }
                        else
                        {
                            node = nextNode;
                        }
                    }
                    else if (operation.Inst == Instruction.Call)
                    {
                        // S2Arm: a called function may make raw accesses of its own: the next one tests its range again
                        gtsContext.RawGroupHead = null;
                    }
                    else if (operation.Inst == Instruction.Store &&
                        (operation.StorageKind == StorageKind.SharedMemory ||
                        operation.StorageKind == StorageKind.LocalMemory))
                    {
                        // The NVIDIA compiler can sometimes use shared or local memory as temporary
                        // storage to place the base address and size on, so we need
                        // to be able to find such information stored in memory too.

                        if (TryGetMemoryOffsets(operation, out LsMemoryType type, out Operand baseOffset, out int constOffset))
                        {
                            Operand value = operation.GetSource(operation.SourcesCount - 1);

                            SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, value, needsOffset: false);
                            if (result.Found)
                            {
                                uint targetCb = PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset);
                                gtsContext.AddMemoryTargetCb(type, baseOffset, constOffset, targetCb, result);
                            }
                        }
                    }
                }
            }
        }

        private static bool IsGlobalMemory(StorageKind storageKind)
        {
            return storageKind is StorageKind.GlobalMemory or
                   StorageKind.GlobalMemoryS8 or
                   StorageKind.GlobalMemoryS16 or
                   StorageKind.GlobalMemoryU8 or
                   StorageKind.GlobalMemoryU16;
        }

        private static bool IsSmallInt(StorageKind storageKind)
        {
            return storageKind is StorageKind.GlobalMemoryS8 or
                   StorageKind.GlobalMemoryS16 or
                   StorageKind.GlobalMemoryU8 or
                   StorageKind.GlobalMemoryU16;
        }

        private static LinkedListNode<INode> ReplaceGlobalMemoryWithStorage(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            StorageRangeContext storageRange,
            BasicBlock block,
            LinkedListNode<INode> node)
        {
            Operation operation = node.Value as Operation;
            Operand globalAddress = operation.GetSource(0);
            SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, globalAddress, needsOffset: true);

            // S2A（F1）：钳位只在宿主要求时生效（未启用 robustBufferAccess 的 Vulkan 渲染器）；为假（Metal、NVIDIA、离线能力表）时
            //   走改前的原样代码，输出逐字节不变
            bool clamp = gpuAccessor.QueryHostStorageWindowClamp();

            if (result.Found && !clamp)
            {
                return ReplaceSingleTargetUnclamped(gtsContext, resourceManager, gpuAccessor, targetLanguage, node, operation, globalAddress, result);
            }

            if (result.Found)
            {
                // We found the storage buffer that is being accessed.
                // S2A（C1/F1）：原先简单访存直接内联 `(偏移 >> 2)`，没有任何范围比较；32 位字节偏移超出宿主绑定时
                //   （负偏移回绕，或客体本来就远超 SbDescriptor.Size 的正偏移），宿主访问「绑定基址 + 最多 4 GiB」。没开
                //   robustBufferAccess 的驱动（Mali）直接按这个地址取数并缺页。现在单目标访存（含 phi / 内存回溯归属的）一律走辅助函数，
                //   由 TryGenerateStorageOp 按宿主实际绑定窗口（OpArrayLength）检查并钳位下标，见那里的说明。

                RecordStorageOffsetFinding(storageRange?.Findings, result);

                if (!TryGenerateSingleTargetStorageOp(
                    gtsContext,
                    resourceManager,
                    gpuAccessor,
                    targetLanguage,
                    storageRange,
                    operation,
                    result,
                    out int functionId))
                {
                    return null;
                }

                CountUnrecordedAccess(resourceManager, gpuAccessor, storageRange, isMultiTarget: false);

                // The helper takes the low 32 bits of the guest address and computes the binding-relative offset itself.
                return GenerateCallStorageOp(node, operation, globalAddress, functionId);
            }
            else
            {
                // Failed to find the storage buffer directly.
                // Try to walk through Phi chains and find all possible constant buffers where
                // the base address might be stored.
                // Generate a helper function that will check all possible storage buffers and use the right one.

                if (!TryGenerateMultiTargetStorageOp(
                    gtsContext,
                    resourceManager,
                    gpuAccessor,
                    targetLanguage,
                    clamp ? storageRange : null,
                    block,
                    operation,
                    clamp,
                    out int functionId))
                {
                    return null;
                }

                if (clamp)
                {
                    CountUnrecordedAccess(resourceManager, gpuAccessor, storageRange, isMultiTarget: true);
                }

                return GenerateCallStorageOp(node, operation, null, functionId);
            }
        }

        /// <summary>
        /// S2A（F1）：开了越界记录诊断、但本阶段拿不到记录绑定时（顶点 / 细分 / 几何阶段且宿主无 vertexPipelineStoresAndAtomics）
        /// 访存仍检查并钳位，但越界既不进记录、也不报红。按访存处计数，由 TranslatorContext 翻译期打限次【黄】；不加任何 GPU 工作。
        /// 记录诊断关（缺省）时不计：那时所有阶段都不记录，不逐阶段提示。
        /// </summary>
        private static void CountUnrecordedAccess(ResourceManager resourceManager, IGpuAccessor gpuAccessor, StorageRangeContext storageRange, bool isMultiTarget)
        {
            if (storageRange != null && gpuAccessor.QueryHostStorageOobRecord() && !resourceManager.S2AStorageOobRecordSupported)
            {
                storageRange.CountUnrecorded(isMultiTarget);
            }
        }

        private static void RecordStorageOffsetFinding(List<StorageOffsetFinding> findings, SearchResult result)
        {
            if (findings == null || result.Offset == null)
            {
                return;
            }

            bool definite = result.Offset.Type == OperandType.Constant;
            long offset = (definite ? result.Offset.Value : 0L) + result.ConstOffset;

            if (offset >= 0)
            {
                return;
            }

            StorageOffsetFinding finding = new(result.SbCbSlot, result.SbCbOffset, offset, definite);

            if (!findings.Contains(finding))
            {
                findings.Add(finding);
            }
        }

        private static LinkedListNode<INode> GenerateCallStorageOp(LinkedListNode<INode> node, Operation operation, Operand offset, int functionId)
        {
            // Generate call to a helper function that will perform the storage buffer operation.

            Operand[] sources = new Operand[operation.SourcesCount - 1 + (offset == null ? 2 : 1)];

            sources[0] = Const(functionId);

            if (offset != null)
            {
                // If the offset was supplised, we use that and skip the global address.

                sources[1] = offset;

                for (int srcIndex = 2; srcIndex < operation.SourcesCount; srcIndex++)
                {
                    sources[srcIndex] = operation.GetSource(srcIndex);
                }
            }
            else
            {
                // Use the 64-bit global address which is split in 2 32-bit arguments.

                for (int srcIndex = 0; srcIndex < operation.SourcesCount; srcIndex++)
                {
                    sources[srcIndex + 1] = operation.GetSource(srcIndex);
                }
            }

            bool returnsValue = operation.Dest != null;
            Operand returnValue = returnsValue ? Local() : null;

            Operation callOp = new(Instruction.Call, returnValue, sources);

            LinkedListNode<INode> newNode = node.List.AddBefore(node, callOp);

            if (returnsValue)
            {
                operation.TurnIntoCopy(returnValue);

                return node;
            }
            else
            {
                Utils.DeleteNode(node, operation);

                return newNode;
            }
        }

        private static bool TryGenerateSingleTargetStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            StorageRangeContext storageRange,
            Operation operation,
            SearchResult result,
            out int functionId)
        {
            List<uint> targetCbs = [PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset)];

            if (gtsContext.TryGetFunctionId(operation, isMultiTarget: false, targetCbs, out functionId))
            {
                return true;
            }

            int inArgumentsCount = 1;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 3;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 2;
            }

            EmitterContext context = new();

            // S2A（C1/F1）：宿主绑定窗口起点与 BufferManager.Set*StorageBuffer 一致：AlignDown(基址, 宿主对齐)。
            //   对齐特化下基址已对齐，窗口起点就是基址。偏移 = 客体地址低 32 位 − 窗口起点低 32 位（64 位差的低 32 位）。
            //   不用客体地址高 32 位：Get40BitsAddress 对负立即数不做高位符号扩展（高位会多 1），非 .E 访存高位恒为 0，
            //   拿它比较会把合法的「寄存器 − 立即数」访问误判为越界。窗口长度不再读 cbuf 里的大小（槽 0 以外不可信，
            //   宿主按映射长度、最多 1 MiB 绑定），统一用 OpArrayLength 取宿主描述符实际绑定的长度，见 TryGenerateStorageOp。
            Operand globalAddressLow = Argument(0);
            Operand baseAddressLow = Cbuf(result.SbCbSlot, result.SbCbOffset);
            Operand windowStart = baseAddressLow;

            if (gpuAccessor.QueryHasUnalignedStorageBuffer())
            {
                windowStart = context.BitwiseAnd(baseAddressLow, StorageWindowMask(gpuAccessor));
            }

            Operand offset = context.ISubtract(globalAddressLow, windowStart);

            Operand compare = null;
            Operand value = null;

            if (inArgumentsCount == 3)
            {
                compare = Argument(1);
                value = Argument(2);
            }
            else if (inArgumentsCount == 2)
            {
                value = Argument(1);
            }

            // 读与原子有返回值（未用的原子结果由调用处丢弃），写没有
            bool returnsValue = operation.Inst != Instruction.Store;

            if (!TryGenerateStorageOp(
                resourceManager,
                targetLanguage,
                context,
                operation.Inst,
                operation.StorageKind,
                offset,
                compare,
                value,
                result,
                new OobRecordSite(storageRange, isMultiTarget: false, globalAddressLow),
                returnsValue))
            {
                functionId = 0;
                return false;
            }

            string functionName = GetFunctionName(operation, isMultiTarget: false, targetCbs);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                functionName,
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: false, targetCbs, function);

            return true;
        }

        private static bool TryGenerateMultiTargetStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            StorageRangeContext storageRange,
            BasicBlock block,
            Operation operation,
            bool clamp,
            out int functionId)
        {
            Queue<PhiNode> phis = new();
            HashSet<PhiNode> visited = [];
            List<uint> targetCbs = [];

            Operand globalAddress = operation.GetSource(0);

            if (globalAddress.AsgOp is Operation addOp && addOp.Inst == Instruction.Add)
            {
                Operand src1 = addOp.GetSource(0);
                Operand src2 = addOp.GetSource(1);

                if (src1.Type == OperandType.Constant && src2.Type == OperandType.LocalVariable)
                {
                    globalAddress = src2;
                }
                else if (src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant)
                {
                    globalAddress = src1;
                }
            }

            if (globalAddress.AsgOp is PhiNode phi && visited.Add(phi))
            {
                phis.Enqueue(phi);
            }
            else
            {
                SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, operation.GetSource(0), needsOffset: false);

                if (result.Found)
                {
                    targetCbs.Add(PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset));
                }
            }

            while (phis.TryDequeue(out phi))
            {
                for (int srcIndex = 0; srcIndex < phi.SourcesCount; srcIndex++)
                {
                    BasicBlock phiBlock = phi.GetBlock(srcIndex);
                    Operand phiSource = phi.GetSource(srcIndex);

                    SearchResult result = FindUniqueBaseAddressCb(gtsContext, phiBlock, phiSource, needsOffset: false);

                    if (result.Found)
                    {
                        uint targetCb = PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset);

                        if (!targetCbs.Contains(targetCb))
                        {
                            targetCbs.Add(targetCb);
                        }
                    }
                    else if (phiSource.AsgOp is PhiNode phi2 && visited.Add(phi2))
                    {
                        phis.Enqueue(phi2);
                    }
                }
            }

            targetCbs.Sort();

            if (targetCbs.Count == 0)
            {
                gpuAccessor.Log(gtsContext.Ampere
                    ? $"Global memory operation \"{operation.Inst}\" traced to no storage buffer: it goes through the memory pool table."
                    : $"Failed to find storage buffer for global memory operation \"{operation.Inst}\".");
            }

            if (targetCbs.Count == 0 && gtsContext.Ampere)
            {
                return TryGenerateRawStorageOp(gtsContext, resourceManager, gpuAccessor, block, operation, out functionId);
            }

            if (gtsContext.TryGetFunctionId(operation, isMultiTarget: true, targetCbs, out functionId))
            {
                return true;
            }

            int inArgumentsCount = 2;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 4;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 3;
            }

            EmitterContext context = new();

            Operand globalAddressLow = Argument(0);
            Operand globalAddressHigh = Argument(1);

            // S2A（F1）：钳位时读与原子有返回值（与单目标一致，未用的原子结果由调用处丢弃），写没有；不钳位时同改前（按有无目的操作数）
            bool returnsValue = clamp ? operation.Inst != Instruction.Store : operation.Dest != null;

            foreach (uint targetCb in targetCbs)
            {
                (int sbCbSlot, int sbCbOffset) = UnpackCbSlotAndOffset(targetCb);

                Operand baseAddrLow = Cbuf(sbCbSlot, sbCbOffset);
                Operand baseAddrHigh = Cbuf(sbCbSlot, sbCbOffset + 1);
                Operand size = Cbuf(sbCbSlot, sbCbOffset + 2);

                Operand offset = context.ISubtract(globalAddressLow, baseAddrLow);
                Operand borrow = context.ICompareLessUnsigned(globalAddressLow, baseAddrLow);

                Operand inRangeLow = context.ICompareLessUnsigned(offset, size);

                Operand addrHighBorrowed = context.IAdd(globalAddressHigh, borrow);

                Operand inRangeHigh = context.ICompareEqual(addrHighBorrowed, baseAddrHigh);

                Operand inRange = context.BitwiseAnd(inRangeLow, inRangeHigh);

                Operand lblSkip = Label();
                context.BranchIfFalse(lblSkip, inRange);

                Operand compare = null;
                Operand value = null;

                if (inArgumentsCount == 4)
                {
                    compare = Argument(2);
                    value = Argument(3);
                }
                else if (inArgumentsCount == 3)
                {
                    value = Argument(2);
                }

                SearchResult result = new(sbCbSlot, sbCbOffset);

                Operand baseAddressMasked = context.BitwiseAnd(baseAddrLow, StorageWindowMask(gpuAccessor));
                Operand hostOffset = context.ISubtract(globalAddressLow, baseAddressMasked);

                if (!clamp)
                {
                    if (!TryGenerateStorageOpUnclamped(
                        resourceManager,
                        targetLanguage,
                        context,
                        operation.Inst,
                        operation.StorageKind,
                        hostOffset,
                        compare,
                        value,
                        result,
                        out Operand resultValue))
                    {
                        functionId = 0;
                        return false;
                    }

                    if (resultValue != null)
                    {
                        context.Return(resultValue);
                    }
                    else
                    {
                        context.Return();
                    }

                    context.MarkLabel(lblSkip);

                    continue;
                }

                // S2A（F1）：客体范围选中这个目标后，宿主访问仍按宿主绑定窗口检查并钳位（槽 0 以外的「大小」字不可信，
                //   宿主最多绑 1 MiB）；TryGenerateStorageOp 在两条路上都自己返回。
                if (!TryGenerateStorageOp(
                    resourceManager,
                    targetLanguage,
                    context,
                    operation.Inst,
                    operation.StorageKind,
                    hostOffset,
                    compare,
                    value,
                    result,
                    new OobRecordSite(storageRange, isMultiTarget: true, globalAddressLow),
                    returnsValue))
                {
                    functionId = 0;
                    return false;
                }

                context.MarkLabel(lblSkip);
            }

            if (returnsValue)
            {
                context.Return(Const(0));
            }
            else
            {
                context.Return();
            }

            string functionName = GetFunctionName(operation, isMultiTarget: true, targetCbs);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                functionName,
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: true, targetCbs, function);

            return true;
        }

        // Reference port: the memory pool table the host binds for raw global memory (dmsh_abi.h DMSH_GMEM_MAP_*): a storage buffer reserved under this sentinel constant buffer
        // slot, which no constant buffer of the game has (dmsh_build.py turns it into DMSH_KIND_GMEM_MAP). 1 << GmemMapLog2 entries of GmemMapEntryWords words each,
        // {start lo, start hi, end lo, end hi, delta lo, delta hi, hint, 0}, sorted by start; entry 0 is empty (start 0, end 0) and unused entries start at ~0
        // S2Arm（11-06 按需常驻，nvn_mtl.m gmem_map / gmem_scan）：字 7 = 这一项的池在宿主「访问位图」里的位号 + 1（0 = 不用记）；第 0 项的差值字（字 4、5）= 访问位图的 GPU 地址。
        //   宿主只让「访问到的池」进常驻集合：没常驻的池照样有一项，但终点 = 起点（旧翻译的着色器按「不在任何池里」跳过：读 0、写丢，GPU 碰不到不常驻的内存），
        //   真终点放在差值字里；查到这种项的访问记下位、照样跳过，宿主下一次提交前把池放进常驻集合、重建表。常驻但宿主要知道还用不用的池（字 7 非 0）选中时也记位
        public const int GmemMapCbSlot = 0xff;
        private const int GmemMapLog2 = 13;
        private const int GmemMapEntryWords = 8;
        private const int GmemHintShift = 21;

        // the write log (dmsh_abi.h DMSH_GMEM_LOG_*), reserved under the same sentinel slot at offset 1: a bitmap of the table entries the stores and atomics wrote through. The host
        // cannot know where a store through a loaded pointer lands before the GPU runs it, and it only wants the pools. Read first: a bit is set once, so nearly every store of a shader
        // that writes a whole frame costs a load, not an atomic (Ocarina of Time's movie compute ran at 41 ms a frame on a hash set of pages, which also ran out of slots)
        private const int GmemLogCbOffset = 1;

        // Reference port: a global memory access whose address the search traced to no storage buffer, an address the shader loaded from memory (Donkey Kong Bananza: gsys's vertex
        // shader that resets the terrain's indirect draw records reads each record's address from a storage buffer). Ryujinx drops such an access (a load reads 0). Here it
        // goes to the address itself: a tagged hint for its 2 MiB chunk, else a branchless binary search, finds the host's pool holding it, the address moves by that pool's delta (its Metal GPU address minus its
        // guest address) and is accessed through a physical storage buffer pointer. An address in no pool is still dropped
        // S2Arm（11-07 时之笛主遍顶点）：the helper of each access inlines and only the search (hint, binary search, access bitmap notes) is one function per shader that does not inline
        //   (GetRawLookup). An access first tests the range of the entry chosen last, kept with its delta in this invocation's private words (RawCache*: no table read when it holds),
        //   and only an access outside it calls the search. The second to fourth words of one LDG / STG .64 / .128 (ResourceManager.RawVectorHeads) take the first word's entry with no
        //   test at all. When every access called the whole search, the shader either inlined it at each one (3d0d6d61bb9076e7: 253 accesses, 65,000 IR instructions, 8.4 s on the
        //   device) or, made not to inline (11-05 Dsh.cs PoolNoInline), saved its live registers to the stack around each call (Ocarina of Time's main pass vertex shaders
        //   89d44129ff5e0e3e / e73b3c6708323a85: 253 calls a vertex, 60% of the pass)
        private static bool TryGenerateRawStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            BasicBlock block,
            Operation operation,
            out int functionId)
        {
            functionId = 0;

            // A later word of a vector access right after the access's earlier words (the same block, no other raw access in between): the cache holds the entry its first word chose,
            // or says that word was skipped. A vector access is naturally aligned, so its words are in one 16-byte block, inside one page and so one entry (the host's entries are made of
            // whole pages of pools, nvn_mtl.m gmem_map)
            bool piece = resourceManager.RawVectorHeads.TryGetValue(operation, out Operation head) && gtsContext.RawGroupBlock == block && gtsContext.RawGroupHead == head;

            gtsContext.RawGroupBlock = block;
            gtsContext.RawGroupHead = piece ? head : operation;

            if (piece ? gtsContext.TryGetRawPieceId(operation, out functionId) : gtsContext.TryGetFunctionId(operation, isMultiTarget: true, [], out functionId))
            {
                return true;
            }

            bool isSmall = IsSmallInt(operation.StorageKind);

            if (isSmall && operation.Inst.IsAtomic())
            {
                gpuAccessor.Log($"Global memory operation \"{operation.Inst}\" on a small integer traced to no storage buffer: not implemented.");
                gtsContext.RawGroupHead = null;
                return false;
            }

            if (!resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, 0, write: false, out int mapBinding))
            {
                gtsContext.RawGroupHead = null;
                return false;
            }

            bool writes = operation.Inst == Instruction.Store || operation.Inst.IsAtomic();
            int logBinding = 0;

            if (writes && !resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, GmemLogCbOffset, write: true, out logBinding))
            {
                gtsContext.RawGroupHead = null;
                return false;
            }

            resourceManager.Properties.UsesPhysicalPointers = true;

            int inArgumentsCount = 2;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 4;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 3;
            }

            int lookupId = GetRawLookup(gtsContext, resourceManager, mapBinding);
            int[] cache = gtsContext.RawCache;

            EmitterContext context = new();

            Operand addrLow = Argument(0);
            Operand addrHigh = Argument(1);

            Operand MapWord(Operand entry, int word)
            {
                return context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entry, Const(word)));
            }

            Operand Cached(int word)
            {
                return context.Load(StorageKind.LocalMemory, cache[word]);
            }

            void Cache(int word, Operand value)
            {
                context.Store(StorageKind.LocalMemory, cache[word], value);
            }

            Operand selected = Local();
            Operand deltaLow = Local();
            Operand deltaHigh = Local();
            Operand lblSkip = Label();

            context.Copy(deltaLow, Cached(RawCacheDeltaLow));
            context.Copy(deltaHigh, Cached(RawCacheDeltaHigh));

            if (piece)
            {
                context.BranchIfTrue(lblSkip, context.ICompareNotEqual(Cached(RawCacheSkipped), Const(0)));
            }
            else
            {
                Operand lblChosen = Label();

                // The cached range holds the address: the address minus its start, 64-bit, is below its size (the end minus the start, at most 2^32 - 1: an address 4 GiB or more past
                // the start of a larger entry calls the search each time, which chooses the same entry). Integer arithmetic, not short-circuit tests: one branch an access.
                // All zero at the start: the empty range
                Operand startLow = Cached(RawCacheStartLow);
                Operand offsetLow = context.ISubtract(addrLow, startLow);
                Operand borrow = context.ConditionalSelect(context.ICompareLessUnsigned(addrLow, startLow), Const(1), Const(0));
                Operand offsetHigh = context.ISubtract(context.ISubtract(addrHigh, Cached(RawCacheStartHigh)), borrow);
                Operand outside = context.BitwiseOr(offsetHigh, context.ConditionalSelect(context.ICompareGreaterOrEqualUnsigned(offsetLow, Cached(RawCacheSize)), Const(1), Const(0)));
                context.Copy(selected, Cached(RawCacheEntry));
                Cache(RawCacheSkipped, Const(0));
                context.BranchIfTrue(lblChosen, context.ICompareEqual(outside, Const(0)));

                // Outside it: the search returns the entry holding the address (noted in the access bitmap when the host tracks its pool), or -1 to skip the access
                // (no pool, or a pool that is not resident). Only an entry that holds the address is cached
                Operand found = context.Call(lookupId, true, addrLow, addrHigh);
                Operand lblFound = Label();
                context.BranchIfFalse(lblFound, context.ICompareLess(found, Const(0)));
                Cache(RawCacheSkipped, Const(1));
                context.Branch(lblSkip);
                context.MarkLabel(lblFound);

                Operand entry = context.IMultiply(found, Const(GmemMapEntryWords));
                Operand entryStartLow = MapWord(entry, 0);
                Operand entryStartHigh = MapWord(entry, 1);
                Operand entryEndLow = MapWord(entry, 2);
                Operand entryBorrow = context.ConditionalSelect(context.ICompareLessUnsigned(entryEndLow, entryStartLow), Const(1), Const(0));
                Operand sizeHigh = context.ISubtract(context.ISubtract(MapWord(entry, 3), entryStartHigh), entryBorrow);
                context.Copy(selected, found);
                context.Copy(deltaLow, MapWord(entry, 4));
                context.Copy(deltaHigh, MapWord(entry, 5));
                Cache(RawCacheEntry, found);
                Cache(RawCacheStartLow, entryStartLow);
                Cache(RawCacheStartHigh, entryStartHigh);
                Cache(RawCacheSize, context.ConditionalSelect(context.ICompareEqual(sizeHigh, Const(0)), context.ISubtract(entryEndLow, entryStartLow), Const(-1)));
                Cache(RawCacheDeltaLow, deltaLow);
                Cache(RawCacheDeltaHigh, deltaHigh);

                context.MarkLabel(lblChosen);

                if (writes)
                {
                    Operand word = context.ShiftRightU32(selected, Const(5));
                    Operand bit = context.ShiftLeft(Const(1), context.BitwiseAnd(selected, Const(31)));
                    Operand lblLogged = Label();

                    context.BranchIfTrue(lblLogged, context.ICompareNotEqual(context.BitwiseAnd(context.Load(StorageKind.StorageBuffer, logBinding, Const(0), word), bit), Const(0)));
                    context.AtomicOr(StorageKind.StorageBuffer, logBinding, Const(0), word, bit);
                    context.MarkLabel(lblLogged);
                }
            }

            // A small integer is read or written in its 32-bit word
            Operand wordLow = isSmall ? context.BitwiseAnd(addrLow, Const(~3)) : addrLow;
            Operand hostLow = context.IAdd(wordLow, deltaLow);
            Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(hostLow, wordLow), Const(1), Const(0));
            Operand hostHigh = context.IAdd(context.IAdd(addrHigh, deltaHigh), carry);

            Operand resultValue = null;

            if (operation.Inst == Instruction.Load)
            {
                resultValue = context.Load(StorageKind.GlobalMemory, hostLow, hostHigh);

                if (isSmall)
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, addrLow);

                    resultValue = operation.StorageKind switch
                    {
                        StorageKind.GlobalMemoryS8 => context.BitfieldExtractS32(context.ShiftRightS32(resultValue, bitOffset), Const(0), Const(8)),
                        StorageKind.GlobalMemoryS16 => context.BitfieldExtractS32(context.ShiftRightS32(resultValue, bitOffset), Const(0), Const(16)),
                        StorageKind.GlobalMemoryU8 => context.BitwiseAnd(context.ShiftRightU32(resultValue, bitOffset), Const(byte.MaxValue)),
                        _ => context.BitwiseAnd(context.ShiftRightU32(resultValue, bitOffset), Const(ushort.MaxValue)),
                    };
                }
            }
            else if (operation.Inst == Instruction.Store)
            {
                Operand value = Argument(2);

                if (isSmall)
                {
                    int bitSize = operation.StorageKind is StorageKind.GlobalMemoryS8 or StorageKind.GlobalMemoryU8 ? 8 : 16;
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, addrLow);

                    Operand lblLoopHead = Label();
                    context.MarkLabel(lblLoopHead);

                    Operand oldValue = context.Load(StorageKind.GlobalMemory, hostLow, hostHigh);
                    Operand newValue = context.BitfieldInsert(oldValue, value, bitOffset, Const(bitSize));
                    Operand casResult = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, hostLow, hostHigh, oldValue, newValue);

                    context.BranchIfTrue(lblLoopHead, context.ICompareNotEqual(casResult, oldValue));
                }
                else
                {
                    context.Store(StorageKind.GlobalMemory, hostLow, hostHigh, value);
                }
            }
            else if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                resultValue = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, hostLow, hostHigh, Argument(2), Argument(3));
            }
            else
            {
                resultValue = context.Add(operation.Inst, StorageKind.GlobalMemory, Local(), hostLow, hostHigh, Argument(2));
            }

            if (resultValue != null)
            {
                context.Return(resultValue);
            }
            else
            {
                context.Return();
            }

            context.MarkLabel(lblSkip);

            bool returnsValue = operation.Dest != null;

            if (returnsValue)
            {
                context.Return(Const(0));
            }
            else
            {
                context.Return();
            }

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                GetFunctionName(operation, isMultiTarget: true, []) + (piece ? "RawPiece" : "Raw"),
                returnsValue,
                inArgumentsCount,
                0);

            functionId = piece ? gtsContext.AddRawPiece(operation, function) : gtsContext.AddFunction(operation, isMultiTarget: true, [], function);

            return true;
        }

        // The private words of the range cache (TryGenerateRawStorageOp): the entry, its start, its size (at most 2^32 - 1), its delta, all zero at the start
        private const int RawCacheEntry = 0;
        private const int RawCacheStartLow = 1;
        private const int RawCacheStartHigh = 2;
        private const int RawCacheSize = 3;
        private const int RawCacheDeltaLow = 4;
        private const int RawCacheDeltaHigh = 5;
        private const int RawCacheSkipped = 6;   // the last full access was skipped (its later vector words are too)
        private const int RawCacheWords = 7;

        // S2Arm（11-07）：the search of the pool table, one function per shader shared by every raw access helper (Dsh.cs PoolNoInline makes it not inline: it touches no
        // private memory, so a call keeps nothing of the caller's on the stack but what is live across the call itself, and only an access outside the cached range calls it).
        // (address low, address high) → the entry holding the address, or -1: an address in no pool, or in a pool that is not resident (its entry ends where it starts and keeps
        // its true end in the delta words). The access bitmap (word 7 = bit + 1, 0 = none; entry 0's delta words = the bitmap's GPU address) gets the entry's bit when the search
        // chooses a pool the host tracks, and when the address is in a pool that is not resident (the host makes it resident before its next submission and rebuilds the table)
        private static int GetRawLookup(GtsContext gtsContext, ResourceManager resourceManager, int mapBinding)
        {
            if (gtsContext.RawLookupId >= 0)
            {
                return gtsContext.RawLookupId;
            }

            gtsContext.RawCache = new int[RawCacheWords];

            for (int word = 0; word < RawCacheWords; word++)
            {
                gtsContext.RawCache[word] = resourceManager.AddRawLastEntryMemory();
            }

            EmitterContext context = new();

            Operand addrLow = Argument(0);
            Operand addrHigh = Argument(1);

            Operand MapWord(Operand entry, int word)
            {
                return context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entry, Const(word)));
            }

            Operand AtOrBelow(Operand high, Operand low)   // the address is at or above {low, high}
            {
                return context.BitwiseOr(
                    context.ICompareLessUnsigned(high, addrHigh),
                    context.BitwiseAnd(context.ICompareEqual(high, addrHigh), context.ICompareLessOrEqualUnsigned(low, addrLow)));
            }

            Operand Below(Operand high, Operand low)   // the address is below {low, high}
            {
                return context.BitwiseOr(
                    context.ICompareLessUnsigned(addrHigh, high),
                    context.BitwiseAnd(context.ICompareEqual(addrHigh, high), context.ICompareLessUnsigned(addrLow, low)));
            }

            Operand selected = Local();
            Operand lblFound = Label();

            // The hint of the address's 2 MiB chunk names an entry holding the whole chunk (dmsh_abi.h DMSH_GMEM_HINT_SHIFT). An address too high for the tag searches
            Operand chunk = context.BitwiseOr(context.ShiftLeft(addrHigh, Const(32 - GmemHintShift)), context.ShiftRightU32(addrLow, Const(GmemHintShift)));
            Operand indexMask = Const((1 << GmemMapLog2) - 1);
            Operand hint = MapWord(context.IMultiply(context.BitwiseAnd(chunk, indexMask), Const(GmemMapEntryWords)), 6);
            Operand hintIndex = context.BitwiseAnd(hint, indexMask);
            Operand hintFits = context.BitwiseAnd(
                context.BitwiseAnd(context.ICompareNotEqual(hintIndex, Const(0)), context.ICompareEqual(context.ShiftRightU32(hint, Const(GmemMapLog2)), chunk)),
                context.ICompareLessUnsigned(addrHigh, Const(1 << (GmemHintShift - GmemMapLog2))));
            context.Copy(selected, hintIndex);
            context.BranchIfTrue(lblFound, hintFits);

            // The last entry whose start is at or below the address (entry 0 starts at 0, so there always is one)
            Operand index = Const(0);

            for (int step = 1 << (GmemMapLog2 - 1); step != 0; step >>= 1)
            {
                Operand candidate = context.IAdd(index, Const(step));
                Operand entry = context.IMultiply(candidate, Const(GmemMapEntryWords));
                index = context.ConditionalSelect(AtOrBelow(MapWord(entry, 1), MapWord(entry, 0)), candidate, index);
            }

            context.Copy(selected, index);
            context.MarkLabel(lblFound);
            Operand found = context.IMultiply(selected, Const(GmemMapEntryWords));
            Operand lblHeld = Label();

            // Read first: a bit is set once
            void Note(Operand state)
            {
                Operand lblNoted = Label();
                context.BranchIfTrue(lblNoted, context.ICompareEqual(state, Const(0)));
                Operand bitIndex = context.ISubtract(state, Const(1));
                Operand bitLow = context.IAdd(MapWord(Const(0), 4), context.ShiftLeft(context.ShiftRightU32(bitIndex, Const(5)), Const(2)));
                Operand bitHigh = context.IAdd(MapWord(Const(0), 5), context.ConditionalSelect(context.ICompareLessUnsigned(bitLow, MapWord(Const(0), 4)), Const(1), Const(0)));
                Operand bitMask = context.ShiftLeft(Const(1), context.BitwiseAnd(bitIndex, Const(31)));
                context.BranchIfTrue(lblNoted, context.ICompareNotEqual(context.BitwiseAnd(context.Load(StorageKind.GlobalMemory, bitLow, bitHigh), bitMask), Const(0)));
                context.AtomicOr(StorageKind.GlobalMemory, bitLow, bitHigh, bitMask);
                context.MarkLabel(lblNoted);
            }

            context.BranchIfTrue(lblHeld, Below(MapWord(found, 3), MapWord(found, 2)));

            // An address in no pool (the entry below it ends below it), or in a pool that is not resident: notes the pool and is skipped like one in no pool
            Operand lblSkip = Label();
            Operand cold = context.BitwiseAnd(
                context.BitwiseAnd(context.ICompareEqual(MapWord(found, 2), MapWord(found, 0)), context.ICompareEqual(MapWord(found, 3), MapWord(found, 1))),
                Below(MapWord(found, 5), MapWord(found, 4)));
            context.BranchIfFalse(lblSkip, cold);
            Note(MapWord(found, 7));
            context.MarkLabel(lblSkip);
            context.Return(Const(-1));

            // The hint's or the search's entry holds it: a pool the host tracks is noted when an invocation chooses it (later accesses in its range take the cached range)
            context.MarkLabel(lblHeld);
            Note(MapWord(found, 7));
            context.Return(selected);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                "GlobalMemoryRawLookup",
                true,
                2,
                0);

            gtsContext.RawLookupId = gtsContext.AddRawLookup(function);

            return gtsContext.RawLookupId;
        }

        private static uint PackCbSlotAndOffset(int cbSlot, int cbOffset)
        {
            return (uint)((ushort)cbSlot | ((ushort)cbOffset << 16));
        }

        private static (int, int) UnpackCbSlotAndOffset(uint packed)
        {
            return ((ushort)packed, (ushort)(packed >> 16));
        }

        private static string GetFunctionName(Operation baseOp, bool isMultiTarget, IReadOnlyList<uint> targetCbs)
        {
            StringBuilder nameBuilder = new();
            nameBuilder.Append(baseOp.Inst.ToString());

            nameBuilder.Append(baseOp.StorageKind switch
            {
                StorageKind.GlobalMemoryS8 => "S8",
                StorageKind.GlobalMemoryS16 => "S16",
                StorageKind.GlobalMemoryU8 => "U8",
                StorageKind.GlobalMemoryU16 => "U16",
                _ => string.Empty,
            });

            if (isMultiTarget)
            {
                nameBuilder.Append("Multi");
            }

            foreach (uint targetCb in targetCbs)
            {
                (int sbCbSlot, int sbCbOffset) = UnpackCbSlotAndOffset(targetCb);

                nameBuilder.Append($"_c{sbCbSlot}o{sbCbOffset}");
            }

            return nameBuilder.ToString();
        }

        /// <summary>
        /// S2A（F1）：越界记录的翻译期固定部分：本阶段身份与访存类别，越界分支里写进记录缓冲。
        /// </summary>
        private readonly struct OobRecordSite(StorageRangeContext storageRange, bool isMultiTarget, Operand globalAddressLow)
        {
            public StorageRangeContext StorageRange { get; } = storageRange;
            public bool IsMultiTarget { get; } = isMultiTarget;
            public Operand GlobalAddressLow { get; } = globalAddressLow;
        }

        /// <summary>
        /// 生成一次存储缓冲访存，并在两条路上都返回（读 / 原子返回值，写不返回）。
        /// S2A（F1）：按宿主实际绑定窗口检查，与客体描述符里的大小无关：
        /// - 窗口长度 = OpArrayLength（宿主描述符 range / 4），所有 cbuf 槽统一；
        /// - 字下标 &lt; 窗口长度才算界内；越界时下标钳成 0 再访存或跳过，读返回 0（不访存的语义与多目标找不到目标、
        ///   NVIDIA 上 robustBufferAccess 的越界读 0 相同），写 / 原子不执行（原子返回 0）。这与 Maxwell 真机不同：真机的
        ///   LDG/STG 是 64 位客体 VA 裸访存，只要那块客体内存有效就会读到真实数据，宿主只承载了 SbDescriptor 窗口；
        /// - 钳位而不只是分支：读不放在分支里，而是「下标先按界内选择、再无条件读、再按界内选择结果」，
        ///   所以即使驱动编译器把分支 if-conversion 或推测执行，发出的地址也在窗口内（0268 只有分支，Wonder 9000 上
        ///   同一条访存仍按原偏移缺页，见坑记录）；
        /// - 窗口长度为 0（宿主 range 不足 4 字节）时读仍在第 0 字发出，比窗口多读 4 字节，但地址是绑定起点，仍在该 VkBuffer
        ///   内、已映射；结果照样选 0；
        /// - 越界时若有记录缓冲（<see cref="S2AStorageOobRecord"/>，缺省关的诊断），原子加计数，拿到旧值 0 的调用写首个越界详情。
        /// 每次访存新增：一次 OpArrayLength、一次无符号比较、一次（写为分支，读为两次）选择；记录只在越界分支执行。
        /// </summary>
        private static bool TryGenerateStorageOp(
            ResourceManager resourceManager,
            TargetLanguage targetLanguage,
            EmitterContext context,
            Instruction inst,
            StorageKind storageKind,
            Operand offset,
            Operand compare,
            Operand value,
            SearchResult result,
            OobRecordSite recordSite,
            bool returnsValue)
        {
            bool isStore = inst.IsAtomic() || inst == Instruction.Store;

            if (!resourceManager.TryGetStorageBufferBinding(result.SbCbSlot, result.SbCbOffset, isStore, out int binding))
            {
                return false;
            }

            Operand rawWordOffset = context.ShiftRightU32(offset, Const(2));
            Operand windowLength = context.ArrayLength(binding, 0);
            Operand inRange = context.ICompareLessUnsigned(rawWordOffset, windowLength);
            Operand wordOffset = context.ConditionalSelect(inRange, rawWordOffset, Const(0));

            if (!isStore)
            {
                value = context.Load(StorageKind.StorageBuffer, binding, Const(0), wordOffset);

                if (IsSmallInt(storageKind))
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    switch (storageKind)
                    {
                        case StorageKind.GlobalMemoryS8:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(8));
                            break;
                        case StorageKind.GlobalMemoryS16:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(16));
                            break;
                        case StorageKind.GlobalMemoryU8:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(byte.MaxValue));
                            break;
                        case StorageKind.GlobalMemoryU16:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(ushort.MaxValue));
                            break;
                    }
                }

                Operand loaded = context.ConditionalSelect(inRange, value, Const(0));

                if (TryGetOobRecordBinding(resourceManager, recordSite, out int recordBinding))
                {
                    Operand lblInRange = Label();
                    context.BranchIfTrue(lblInRange, inRange);
                    EmitOobRecord(context, recordBinding, recordSite, inst, result, offset, windowLength);
                    context.MarkLabel(lblInRange);
                }

                context.Return(loaded);

                return true;
            }

            Operand lblOutOfRange = Label();
            context.BranchIfFalse(lblOutOfRange, inRange);

            Operand resultValue = null;

            if (inst.IsAtomic())
            {
                if (IsSmallInt(storageKind))
                {
                    throw new NotImplementedException();
                }

                switch (inst)
                {
                    case Instruction.AtomicAdd:
                        resultValue = context.AtomicAdd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicAnd:
                        resultValue = context.AtomicAnd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicCompareAndSwap:
                        resultValue = context.AtomicCompareAndSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, compare, value);
                        break;
                    case Instruction.AtomicMaxS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMaxS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMaximumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMaxU32:
                        resultValue = context.AtomicMaxU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicMinS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMinS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMinimumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMinU32:
                        resultValue = context.AtomicMinU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicOr:
                        resultValue = context.AtomicOr(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicSwap:
                        resultValue = context.AtomicSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicXor:
                        resultValue = context.AtomicXor(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                }
            }
            else
            {
                int bitSize = storageKind switch
                {
                    StorageKind.GlobalMemoryS8 or
                    StorageKind.GlobalMemoryU8 => 8,
                    StorageKind.GlobalMemoryS16 or
                    StorageKind.GlobalMemoryU16 => 16,
                    _ => 32,
                };

                if (bitSize < 32)
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                    {
                        return context.BitfieldInsert(memValue, value, bitOffset, Const(bitSize));
                    });
                }
                else
                {
                    context.Store(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                }
            }

            EmitReturn(context, returnsValue, resultValue);

            context.MarkLabel(lblOutOfRange);

            if (TryGetOobRecordBinding(resourceManager, recordSite, out int storeRecordBinding))
            {
                EmitOobRecord(context, storeRecordBinding, recordSite, inst, result, offset, windowLength);
            }

            EmitReturn(context, returnsValue, Const(0));

            return true;
        }

        private static void EmitReturn(EmitterContext context, bool returnsValue, Operand resultValue)
        {
            if (returnsValue)
            {
                context.Return(resultValue ?? Const(0));
            }
            else
            {
                context.Return();
            }
        }

        private static bool TryGetOobRecordBinding(ResourceManager resourceManager, OobRecordSite recordSite, out int binding)
        {
            binding = -1;

            return recordSite.StorageRange != null && resourceManager.TryGetS2AStorageOobRecordBinding(out binding);
        }

        /// <summary>
        /// S2A（F1）：越界分支里写记录缓冲。计数每次越界调用都加；只有拿到旧值 0 的调用写详情，最后写带有效位的信息字。
        /// </summary>
        private static void EmitOobRecord(
            EmitterContext context,
            int recordBinding,
            OobRecordSite recordSite,
            Instruction inst,
            SearchResult result,
            Operand offset,
            Operand windowLength)
        {
            uint kind = inst == Instruction.Store ? S2AStorageOobRecord.KindStore
                : inst.IsAtomic() ? S2AStorageOobRecord.KindAtomic
                : S2AStorageOobRecord.KindLoad;

            if (recordSite.IsMultiTarget)
            {
                kind |= S2AStorageOobRecord.KindMultiTarget;
            }

            StorageRangeContext range = recordSite.StorageRange;

            uint info = S2AStorageOobRecord.InfoValid |
                (kind << S2AStorageOobRecord.KindShift) |
                ((uint)range.Stage << S2AStorageOobRecord.StageShift) |
                ((uint)(byte)result.SbCbSlot << S2AStorageOobRecord.CbSlotShift) |
                (ushort)result.SbCbOffset;

            void StoreWord(int word, Operand wordValue) =>
                context.Store(StorageKind.StorageBuffer, recordBinding, Const(0), Const(word), wordValue);

            Operand previous = context.AtomicAdd(StorageKind.StorageBuffer, recordBinding, Const(0), Const(S2AStorageOobRecord.WordCount), Const(1));

            Operand lblRecorded = Label();
            context.BranchIfTrue(lblRecorded, context.ICompareNotEqual(previous, Const(0)));

            StoreWord(S2AStorageOobRecord.WordAddressLow, recordSite.GlobalAddressLow);
            StoreWord(S2AStorageOobRecord.WordOffset, offset);
            StoreWord(S2AStorageOobRecord.WordWindowLength, windowLength);
            StoreWord(S2AStorageOobRecord.WordBaseLow, Cbuf(result.SbCbSlot, result.SbCbOffset));
            StoreWord(S2AStorageOobRecord.WordBaseHigh, Cbuf(result.SbCbSlot, result.SbCbOffset + 1));
            StoreWord(S2AStorageOobRecord.WordSize, Cbuf(result.SbCbSlot, result.SbCbOffset + 2));
            StoreWord(S2AStorageOobRecord.WordHash, Const((int)(uint)range.CodeHashLow));
            StoreWord(S2AStorageOobRecord.WordHash + 1, Const((int)(uint)(range.CodeHashLow >> 32)));
            StoreWord(S2AStorageOobRecord.WordHash + 2, Const((int)(uint)range.CodeHashHigh));
            StoreWord(S2AStorageOobRecord.WordHash + 3, Const((int)(uint)(range.CodeHashHigh >> 32)));
            StoreWord(S2AStorageOobRecord.WordInfo, Const((int)info));

            context.MarkLabel(lblRecorded);
        }

        // ---------- S2A（F1）：以下是不钳位时用的改前原样代码（只改了函数名），见 ReplaceSingleTargetUnclamped ----------

        /// <summary>
        /// S2A（F1）：不钳位时的单目标访存，即改前的原样代码（简单访存内联 `偏移 >> 2`，其余走单目标辅助函数）。
        /// Metal（NxCaps 也报 Vulkan）、NVIDIA（开了 robustBufferAccess）与离线能力表走这里，输出与改前逐字节相同。
        /// </summary>
        private static LinkedListNode<INode> ReplaceSingleTargetUnclamped(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            LinkedListNode<INode> node,
            Operation operation,
            Operand globalAddress,
            SearchResult result)
        {
            // We found the storage buffer that is being accessed.
            // There are two possible paths here, if the operation is simple enough,
            // we just generate the storage access code inline.
            // Otherwise, we generate a function call (and the function if necessary).

            Operand offset = result.Offset;

            bool storageUnaligned = gpuAccessor.QueryHasUnalignedStorageBuffer();

            if (storageUnaligned)
            {
                Operand baseAddress = Cbuf(result.SbCbSlot, result.SbCbOffset);

                Operand baseAddressMasked = Local();
                Operand hostOffset = Local();

                Operation maskOp = new(Instruction.BitwiseAnd, baseAddressMasked, baseAddress, StorageWindowMask(gpuAccessor));
                Operation subOp = new(Instruction.Subtract, hostOffset, globalAddress, baseAddressMasked);

                node.List.AddBefore(node, maskOp);
                node.List.AddBefore(node, subOp);

                offset = hostOffset;
            }
            else if (result.ConstOffset != 0)
            {
                Operand newOffset = Local();

                Operation addOp = new(Instruction.Add, newOffset, offset, Const(result.ConstOffset));

                node.List.AddBefore(node, addOp);

                offset = newOffset;
            }

            if (CanUseInlineStorageOp(operation, targetLanguage))
            {
                return GenerateInlineStorageOp(resourceManager, node, operation, offset, result);
            }
            else
            {
                if (!TryGenerateSingleTargetStorageOpUnclamped(
                    gtsContext,
                    resourceManager,
                    targetLanguage,
                    operation,
                    result,
                    out int functionId))
                {
                    return null;
                }

                return GenerateCallStorageOp(node, operation, offset, functionId);
            }
        }

        private static bool CanUseInlineStorageOp(Operation operation, TargetLanguage targetLanguage)
        {
            if (operation.StorageKind != StorageKind.GlobalMemory)
            {
                return false;
            }

            return (operation.Inst != Instruction.AtomicMaxS32 &&
                    operation.Inst != Instruction.AtomicMinS32) || targetLanguage == TargetLanguage.Spirv;
        }

        private static LinkedListNode<INode> GenerateInlineStorageOp(
            ResourceManager resourceManager,
            LinkedListNode<INode> node,
            Operation operation,
            Operand offset,
            SearchResult result)
        {
            bool isStore = operation.Inst == Instruction.Store || operation.Inst.IsAtomic();
            if (!resourceManager.TryGetStorageBufferBinding(result.SbCbSlot, result.SbCbOffset, isStore, out int binding))
            {
                return null;
            }

            Operand wordOffset = Local();

            Operand[] sources;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                sources =
                [
                    Const(binding),
                    Const(0),
                    wordOffset,
                    operation.GetSource(operation.SourcesCount - 2),
                    operation.GetSource(operation.SourcesCount - 1)
                ];
            }
            else if (isStore)
            {
                sources = [Const(binding), Const(0), wordOffset, operation.GetSource(operation.SourcesCount - 1)];
            }
            else
            {
                sources = [Const(binding), Const(0), wordOffset];
            }

            Operation shiftOp = new(Instruction.ShiftRightU32, wordOffset, offset, Const(2));
            Operation storageOp = new(operation.Inst, StorageKind.StorageBuffer, operation.Dest, sources);

            node.List.AddBefore(node, shiftOp);
            LinkedListNode<INode> newNode = node.List.AddBefore(node, storageOp);

            Utils.DeleteNode(node, operation);

            return newNode;
        }

        private static bool TryGenerateSingleTargetStorageOpUnclamped(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            TargetLanguage targetLanguage,
            Operation operation,
            SearchResult result,
            out int functionId)
        {
            List<uint> targetCbs = [PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset)];

            if (gtsContext.TryGetFunctionId(operation, isMultiTarget: false, targetCbs, out functionId))
            {
                return true;
            }

            int inArgumentsCount = 1;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 3;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 2;
            }

            EmitterContext context = new();

            Operand offset = Argument(0);
            Operand compare = null;
            Operand value = null;

            if (inArgumentsCount == 3)
            {
                compare = Argument(1);
                value = Argument(2);
            }
            else if (inArgumentsCount == 2)
            {
                value = Argument(1);
            }

            if (!TryGenerateStorageOpUnclamped(
                resourceManager,
                targetLanguage,
                context,
                operation.Inst,
                operation.StorageKind,
                offset,
                compare,
                value,
                result,
                out Operand resultValue))
            {
                functionId = 0;
                return false;
            }

            bool returnsValue = resultValue != null;

            if (returnsValue)
            {
                context.Return(resultValue);
            }
            else
            {
                context.Return();
            }

            string functionName = GetFunctionName(operation, isMultiTarget: false, targetCbs);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                functionName,
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: false, targetCbs, function);

            return true;
        }

        private static bool TryGenerateStorageOpUnclamped(
            ResourceManager resourceManager,
            TargetLanguage targetLanguage,
            EmitterContext context,
            Instruction inst,
            StorageKind storageKind,
            Operand offset,
            Operand compare,
            Operand value,
            SearchResult result,
            out Operand resultValue)
        {
            resultValue = null;
            bool isStore = inst.IsAtomic() || inst == Instruction.Store;

            if (!resourceManager.TryGetStorageBufferBinding(result.SbCbSlot, result.SbCbOffset, isStore, out int binding))
            {
                return false;
            }

            Operand wordOffset = context.ShiftRightU32(offset, Const(2));

            if (inst.IsAtomic())
            {
                if (IsSmallInt(storageKind))
                {
                    throw new NotImplementedException();
                }

                switch (inst)
                {
                    case Instruction.AtomicAdd:
                        resultValue = context.AtomicAdd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicAnd:
                        resultValue = context.AtomicAnd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicCompareAndSwap:
                        resultValue = context.AtomicCompareAndSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, compare, value);
                        break;
                    case Instruction.AtomicMaxS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMaxS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMaximumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMaxU32:
                        resultValue = context.AtomicMaxU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicMinS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMinS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMinimumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMinU32:
                        resultValue = context.AtomicMinU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicOr:
                        resultValue = context.AtomicOr(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicSwap:
                        resultValue = context.AtomicSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicXor:
                        resultValue = context.AtomicXor(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                }
            }
            else if (inst == Instruction.Store)
            {
                int bitSize = storageKind switch
                {
                    StorageKind.GlobalMemoryS8 or
                    StorageKind.GlobalMemoryU8 => 8,
                    StorageKind.GlobalMemoryS16 or
                    StorageKind.GlobalMemoryU16 => 16,
                    _ => 32,
                };

                if (bitSize < 32)
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                    {
                        return context.BitfieldInsert(memValue, value, bitOffset, Const(bitSize));
                    });
                }
                else
                {
                    context.Store(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                }
            }
            else
            {
                value = context.Load(StorageKind.StorageBuffer, binding, Const(0), wordOffset);

                if (IsSmallInt(storageKind))
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    switch (storageKind)
                    {
                        case StorageKind.GlobalMemoryS8:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(8));
                            break;
                        case StorageKind.GlobalMemoryS16:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(16));
                            break;
                        case StorageKind.GlobalMemoryU8:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(byte.MaxValue));
                            break;
                        case StorageKind.GlobalMemoryU16:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(ushort.MaxValue));
                            break;
                    }
                }

                resultValue = value;
            }

            return true;
        }

        private static Operand GenerateAtomicCasLoop(EmitterContext context, Operand wordOffset, int binding, Func<Operand, Operand> opCallback)
        {
            Operand lblLoopHead = Label();

            context.MarkLabel(lblLoopHead);

            Operand oldValue = context.Load(StorageKind.StorageBuffer, binding, Const(0), wordOffset);
            Operand newValue = opCallback(oldValue);

            Operand casResult = context.AtomicCompareAndSwap(
                StorageKind.StorageBuffer,
                binding,
                Const(0),
                wordOffset,
                oldValue,
                newValue);

            Operand casFail = context.ICompareNotEqual(casResult, oldValue);

            context.BranchIfTrue(lblLoopHead, casFail);

            return oldValue;
        }

        private static SearchResult FindUniqueBaseAddressCb(GtsContext gtsContext, BasicBlock block, Operand globalAddress, bool needsOffset)
        {
            globalAddress = Utils.FindLastOperation(globalAddress, block);

            if (globalAddress.Type == OperandType.ConstantBuffer)
            {
                return GetBaseAddressCbWithOffset(globalAddress, Const(0), 0);
            }

            Operation operation = globalAddress.AsgOp as Operation;

            if (operation == null || operation.Inst != Instruction.Add)
            {
                return FindBaseAddressCbFromMemory(gtsContext, operation, 0, needsOffset);
            }

            Operand src1 = operation.GetSource(0);
            Operand src2 = operation.GetSource(1);

            int constOffset = 0;

            if ((src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant) ||
                (src2.Type == OperandType.LocalVariable && src1.Type == OperandType.Constant))
            {
                Operand baseAddr;
                Operand offset;

                if (src1.Type == OperandType.LocalVariable)
                {
                    baseAddr = Utils.FindLastOperation(src1, block);
                    offset = src2;
                }
                else
                {
                    baseAddr = Utils.FindLastOperation(src2, block);
                    offset = src1;
                }

                SearchResult result = GetBaseAddressCbWithOffset(baseAddr, offset, 0);
                if (result.Found)
                {
                    return result;
                }

                constOffset = offset.Value;
                operation = baseAddr.AsgOp as Operation;

                if (operation == null || operation.Inst != Instruction.Add)
                {
                    return FindBaseAddressCbFromMemory(gtsContext, operation, constOffset, needsOffset);
                }
            }

            src1 = operation.GetSource(0);
            src2 = operation.GetSource(1);

            // If we have two possible results, we give preference to the ones from
            // the driver reserved constant buffer, as those are the ones that
            // contains the base address.

            // If both are constant buffer, give preference to the second operand,
            // because constant buffer are always encoded as the second operand,
            // so the second operand will always be the one from the last instruction.
            // Unless only the first is from the driver reserved buffer: Ampere code adds uniform registers
            // (Donkey Kong Bananza's effect compute shaders: c0 storage entry + offset from c1, by UIMAD.WIDE),
            // and the IR keeps the order of their sources.

            if (gtsContext.Ampere && src1.Type == OperandType.ConstantBuffer && src2.Type == OperandType.ConstantBuffer &&
                src1.GetCbufSlot() == DriverReservedCb && src2.GetCbufSlot() != DriverReservedCb)
            {
                return GetBaseAddressCbWithOffset(src1, src2, constOffset);
            }

            if (src1.Type != OperandType.ConstantBuffer ||
                (src1.Type == OperandType.ConstantBuffer && src2.Type == OperandType.ConstantBuffer) ||
                (src2.Type == OperandType.ConstantBuffer && src2.GetCbufSlot() == DriverReservedCb))
            {
                return GetBaseAddressCbWithOffset(src2, src1, constOffset);
            }

            return GetBaseAddressCbWithOffset(src1, src2, constOffset);
        }

        private static SearchResult FindBaseAddressCbFromMemory(GtsContext gtsContext, Operation operation, int constOffset, bool needsOffset)
        {
            if (operation != null)
            {
                if (TryGetMemoryOffsets(operation, out LsMemoryType type, out Operand bo, out int co) &&
                    gtsContext.TryGetMemoryTargetCb(type, bo, co, out SearchResult result) &&
                    (result.Offset != null || !needsOffset))
                {
                    if (constOffset != 0)
                    {
                        return new SearchResult(
                            result.SbCbSlot,
                            result.SbCbOffset,
                            result.Offset,
                            result.ConstOffset + constOffset);
                    }

                    return result;
                }
            }

            return SearchResult.NotFound;
        }

        private static SearchResult GetBaseAddressCbWithOffset(Operand baseAddress, Operand offset, int constOffset)
        {
            if (baseAddress.Type == OperandType.ConstantBuffer)
            {
                int sbCbSlot = baseAddress.GetCbufSlot();
                int sbCbOffset = baseAddress.GetCbufOffset();

                // We require the offset to be aligned to 1 word (64 bits),
                // since the address size is 64-bit and the GPU only supports aligned memory access.
                if ((sbCbOffset & 1) == 0)
                {
                    return new SearchResult(sbCbSlot, sbCbOffset, offset, constOffset);
                }
            }

            return SearchResult.NotFound;
        }

        private static bool TryGetMemoryOffsets(Operation operation, out LsMemoryType type, out Operand baseOffset, out int constOffset)
        {
            baseOffset = null;

            if (operation.Inst is Instruction.Load or Instruction.Store)
            {
                if (operation.StorageKind == StorageKind.SharedMemory)
                {
                    type = LsMemoryType.Shared;
                    return TryGetSharedMemoryOffsets(operation, out baseOffset, out constOffset);
                }
                else if (operation.StorageKind == StorageKind.LocalMemory)
                {
                    type = LsMemoryType.Local;
                    return TryGetLocalMemoryOffset(operation, out constOffset);
                }
            }

            type = default;
            constOffset = 0;
            return false;
        }

        private static bool TryGetSharedMemoryOffsets(Operation operation, out Operand baseOffset, out int constOffset)
        {
            baseOffset = null;
            constOffset = 0;

            // The byte offset is right shifted by 2 to get the 32-bit word offset,
            // so we want to get the byte offset back, since each one of those word
            // offsets are a new "local variable" which will not match.

            if (operation.GetSource(1).AsgOp is Operation shiftRightOp &&
                shiftRightOp.Inst == Instruction.ShiftRightU32 &&
                shiftRightOp.GetSource(1).Type == OperandType.Constant &&
                shiftRightOp.GetSource(1).Value == 2)
            {
                baseOffset = shiftRightOp.GetSource(0);
            }

            // Check if we have a constant offset being added to the base offset.

            if (baseOffset?.AsgOp is Operation addOp && addOp.Inst == Instruction.Add)
            {
                Operand src1 = addOp.GetSource(0);
                Operand src2 = addOp.GetSource(1);

                if (src1.Type == OperandType.Constant && src2.Type == OperandType.LocalVariable)
                {
                    constOffset = src1.Value;
                    baseOffset = src2;
                }
                else if (src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant)
                {
                    baseOffset = src1;
                    constOffset = src2.Value;
                }
            }

            return baseOffset != null && baseOffset.Type == OperandType.LocalVariable;
        }

        private static bool TryGetLocalMemoryOffset(Operation operation, out int constOffset)
        {
            Operand offset = operation.GetSource(1);

            if (offset.Type == OperandType.Constant)
            {
                constOffset = offset.Value;
                return true;
            }

            constOffset = 0;
            return false;
        }
    }
}
