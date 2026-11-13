using Ryujinx.Common;
using Ryujinx.Graphics.Shader.CodeGen;
using Ryujinx.Graphics.Shader.CodeGen.Glsl;
using Ryujinx.Graphics.Shader.CodeGen.Spirv;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using Ryujinx.Graphics.Shader.Translation.Transforms;
using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Threading;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using static Ryujinx.Graphics.Shader.Translation.Translator;

namespace Ryujinx.Graphics.Shader.Translation
{
    public class TranslatorContext
    {
        private readonly DecodedProgram _program;
        private readonly int _localMemorySize;
        private IoUsage _vertexOutput;
        private AttributeUsage _vertexPassthroughAttributeUsage;

        public ulong Address { get; }
        public int Size { get; }
        public int Cb1DataSize => _program.Cb1DataSize;

        internal AttributeUsage AttributeUsage => _program.AttributeUsage;

        internal ShaderDefinitions Definitions { get; }

        public ShaderStage Stage => Definitions.Stage;

        internal IGpuAccessor GpuAccessor { get; }

        internal TranslationOptions Options { get; }

        private bool IsTransformFeedbackEmulated => !GpuAccessor.QueryHostSupportsTransformFeedback() && GpuAccessor.QueryTransformFeedbackEnabled();
        public bool HasStore => _program.UsedFeatures.HasFlag(FeatureFlags.Store) || (IsTransformFeedbackEmulated && Definitions.LastInVertexPipeline);

        public bool LayerOutputWritten { get; private set; }
        public int LayerOutputAttribute { get; private set; }

        internal TranslatorContext(
            ulong address,
            int size,
            int localMemorySize,
            ShaderDefinitions definitions,
            IGpuAccessor gpuAccessor,
            TranslationOptions options,
            DecodedProgram program)
        {
            Address = address;
            Size = size;
            _program = program;
            _localMemorySize = localMemorySize;
            _vertexOutput = new IoUsage(FeatureFlags.None, 0, -1);
            Definitions = definitions;
            GpuAccessor = gpuAccessor;
            Options = options;
            AttributeUsage.SetTargetLanguage(options.TargetLanguage);
        }

        private static bool IsLoadUserDefined(Operation operation)
        {
            // TODO: Check if sources count match and all sources are constant.
            return operation.Inst == Instruction.Load && (IoVariable)operation.GetSource(0).Value == IoVariable.UserDefined;
        }

        private static bool IsStoreUserDefined(Operation operation)
        {
            // TODO: Check if sources count match and all sources are constant.
            return operation.Inst == Instruction.Store && (IoVariable)operation.GetSource(0).Value == IoVariable.UserDefined;
        }

        private static FunctionCode[] Combine(FunctionCode[] a, FunctionCode[] b, int aStart)
        {
            // Here we combine two shaders.
            // For shader A:
            // - All user attribute stores on shader A are turned into copies to a
            // temporary variable. It's assumed that shader B will consume them.
            // - All return instructions are turned into branch instructions, the
            // branch target being the start of the shader B code.
            // For shader B:
            // - All user attribute loads on shader B are turned into copies from a
            // temporary variable, as long that attribute is written by shader A.
            FunctionCode[] output = new FunctionCode[a.Length + b.Length - 1];

            List<Operation> ops = new(a.Length + b.Length);

            Operand[] temps = new Operand[AttributeConsts.UserAttributesCount * 4];

            Operand lblB = Label();

            for (int index = aStart; index < a[0].Code.Length; index++)
            {
                Operation operation = a[0].Code[index];

                if (IsStoreUserDefined(operation))
                {
                    int tIndex = operation.GetSource(1).Value * 4 + operation.GetSource(2).Value;

                    Operand temp = temps[tIndex];

                    if (temp == null)
                    {
                        temp = Local();

                        temps[tIndex] = temp;
                    }

                    operation.Dest = temp;
                    operation.TurnIntoCopy(operation.GetSource(operation.SourcesCount - 1));
                }

                if (operation.Inst == Instruction.Return)
                {
                    ops.Add(new Operation(Instruction.Branch, lblB));
                }
                else
                {
                    ops.Add(operation);
                }
            }

            ops.Add(new Operation(Instruction.MarkLabel, lblB));

            for (int index = 0; index < b[0].Code.Length; index++)
            {
                Operation operation = b[0].Code[index];

                if (IsLoadUserDefined(operation))
                {
                    int tIndex = operation.GetSource(1).Value * 4 + operation.GetSource(2).Value;

                    Operand temp = temps[tIndex];

                    if (temp != null)
                    {
                        operation.TurnIntoCopy(temp);
                    }
                }

                ops.Add(operation);
            }

            output[0] = new FunctionCode(ops.ToArray());

            for (int i = 1; i < a.Length; i++)
            {
                output[i] = a[i];
            }

            for (int i = 1; i < b.Length; i++)
            {
                output[a.Length + i - 1] = b[i];
            }

            return output;
        }

        internal int GetDepthRegister()
        {
            // The depth register is always two registers after the last color output.
            return BitOperations.PopCount((uint)Definitions.OmapTargets) + 1;
        }

        public void SetLayerOutputAttribute(int attr)
        {
            LayerOutputWritten = true;
            LayerOutputAttribute = attr;
        }

        public void SetLastInVertexPipeline()
        {
            Definitions.LastInVertexPipeline = true;
        }

        // S2A：阶段独立翻译（缺省开；S2A_STAGE_INDEP=0 关，A/B 用——烘焙与运行时必须同一取值）
        public static bool S2AStageIndep = Environment.GetEnvironmentVariable("S2A_STAGE_INDEP") != "0";

        // S2A：特化状态运行时化（缺省开；S2A_RUNTIME_STATE=0 关，GPU 代价 A/B 用——烘焙与运行时必须同一取值）。开时 DepthMode 等改读支持缓冲 s2a_flags，翻译结果不随这些状态变
        /// <summary>S2A：顶点属性当整数用的位置掩码 / 其中带符号的（翻译后有效，离线烘焙器用）</summary>
        public uint S2AAttrIntMask => Definitions.S2AAttrIntUse;
        public uint S2AAttrSintMask => Definitions.S2AAttrSintUse;

        public static bool S2ARuntimeState = Environment.GetEnvironmentVariable("S2A_RUNTIME_STATE") != "0";

        // S2A：顶点输入按原始位读（缺省开；NXGAL_VPULL=0 关，与渲染器顶点自取同一开关——烘焙与运行时必须同一取值）。只在宿主报顶点自取时生效，顶点转计算不用
        public static bool S2AVertexRawEnv = Environment.GetEnvironmentVariable("NXGAL_VPULL") != "0";

        private bool S2AVertexRawFor(bool asCompute) => S2AVertexRawEnv && !asCompute && GpuAccessor.QueryHostS2AVertexPull();

        /// <summary>S2A：单独翻译顶点阶段（离线批量翻译里没配上片元的顶点）：按「下一阶段是片元、读全部通用输入」处理，结果与配任何片元时相同</summary>
        public void S2ASetStandaloneNextFragment()
        {
            if (AttributeUsage.RequiresLinkedVaryingInterface)
            {
                throw new NotSupportedException("Linked varying translation requires the actual next shader stage.");
            }

            if (S2AStageIndep && Definitions.Stage == ShaderStage.Vertex) AttributeUsage.MergeAllGenericInputsForFragment(Definitions.GpPassthrough);
        }

        /// <summary>S2A：单独翻几何着色器（几何转计算，离线库按几何着色器单独出、配哪个片元都用同一份）：按「下一阶段是片元、读全部 32 个通用输入」定输出接口，
        /// 与顶点阶段的 S2ASetStandaloneNextFragment 同一口径。上游靠 SetNextStage 拿片元的实际读取：直通型据此转发属性，非直通型给片元读、自己没写的输出补默认值。
        /// 单独翻时没有下一阶段，两样都是 0：异度神剑3 95bfab6122539993（非直通，4 顶点三角带，只写位置与 1 个分量）记录步长 5，配对的片元读 locn0–9，
        /// 读取变体（_bgv）的管线建不出、这些几何绘制整笔丢、开场全黑（10-31）。
        /// 非直通型（异度神剑3 131 支里 129 支）只记它自己写的输出，片元可能读、它没写的 32 个通用输出由读取变体按缺省值 (0, 0, 0, 1) 直接给（S2AReaderDefaultsUnwritten）：
        /// 补进记录的话每个发射顶点要多存 128 字常数（95bfab6122539993 记录步长 5 → 133），场景帧几千笔几何绘制、每帧 GPU 1.5–3.3 秒、帧临时缓冲攒爆（10-31）。
        /// 直通型照旧按「片元读全部 32 个」转发（转发的是输入的值，只能进记录）</summary>
        public void S2ASetStandaloneGeometryNextFragment()
        {
            if (Definitions.Stage != ShaderStage.Geometry) return;
            if (Definitions.GpPassthrough) AttributeUsage.MergeAllGenericInputsForFragment(true);
            else S2AReaderDefaultsUnwritten = true;
        }

        /// <summary>S2A：读取变体（GenerateVertexPassthroughForCompute）对记录里没有的通用输出 0–31 输出缺省值 (0, 0, 0, 1)（S2ASetStandaloneGeometryNextFragment）</summary>
        public bool S2AReaderDefaultsUnwritten { get; private set; }

        /// <summary>S2A：几何着色器头标了直通（着色器头第 0 字第 24 位，ShaderDefinitions.GpPassthrough）</summary>
        public bool S2AGpPassthrough => Definitions.GpPassthrough;

        /// <summary>S2A：直通几何快路的「只算副作用」变体（直连库 _bgs）：宿主不支持直通时 EmitterContext 在每个返回点模拟直通（逐顶点拷位置与直通属性、发射、结束图元），
        /// 这个开关下不模拟，转计算收尾也不补空下标：几何核不写记录、不写下标，只留几何着色器本体（存储写、原子等副作用）。
        /// 输出图元就是输入图元、顶点值就是上一阶段的输出，主遍改读抓顶点输出（S2AGeneratePassthroughCaptureReader），不读记录、不压缩下标</summary>
        public bool S2APassthroughSideEffectsOnly { get; set; }

        /// <summary>S2A：S2APassthroughSideEffectsOnly 翻译后：几何本体自己写了输出或自己发射 / 结束图元（直通模拟之外）。
        /// 这时记录里有本体算出的值或多出的图元，抓顶点读取器给不出，不能走快路（不出 _bgs / _bgc）</summary>
        public bool S2APassthroughBodyOutputs { get; private set; }

        public void SetNextStage(TranslatorContext nextStage) => SetNextStage(nextStage, false);

        // 配对编译已持有真实下游阶段时，可按其实际输入建立输出接口，
        // 避免独立缓存策略把原生 GS 输出扩到全部 32 个 location。
        public void S2ASetNextStageActual(TranslatorContext nextStage) => SetNextStage(nextStage, true);

        private void SetNextStage(TranslatorContext nextStage, bool actualNextStage)
        {
            bool linked = AttributeUsage.RequiresLinkedVaryingInterface;
            if (linked != nextStage.AttributeUsage.RequiresLinkedVaryingInterface)
            {
                throw new InvalidOperationException("Linked shader stages must use the same effective varying policy.");
            }

            if (!actualNextStage && !linked && S2AStageIndep && nextStage.Definitions.Stage == ShaderStage.Fragment && !nextStage._program.UsedFeatures.HasFlag(FeatureFlags.FixedFuncAttr))
            {
                AttributeUsage.MergeAllGenericInputsForFragment(Definitions.GpPassthrough);   // S2A：顶点输出接口不看片元实际读哪些（见 AttributeUsage）
            }
            else
            {
                AttributeUsage.MergeFromtNextStage(
                    Definitions.GpPassthrough,
                    nextStage._program.UsedFeatures.HasFlag(FeatureFlags.FixedFuncAttr),
                    nextStage.AttributeUsage);
            }

            if (linked && nextStage.Stage == ShaderStage.Fragment)
            {
                string identityOnlyReason = null;
                bool indexed = Definitions.OaIndexing || (Definitions.GpPassthrough && Definitions.IaIndexing) || nextStage.Definitions.IaIndexing;
                if (indexed)
                {
                    identityOnlyReason = "dynamically indexed attributes";
                }
                else if (AttributeUsage.UsedOutputAttributesPerPatch.Count != 0 || nextStage.AttributeUsage.UsedInputAttributesPerPatch.Count != 0)
                {
                    identityOnlyReason = "per-patch attributes";
                }
                else if (_program.UsedFeatures.HasFlag(FeatureFlags.FixedFuncAttr) || nextStage._program.UsedFeatures.HasFlag(FeatureFlags.FixedFuncAttr))
                {
                    identityOnlyReason = "fixed-function attributes";
                }
                else if (GpuAccessor.QueryTransformFeedbackEnabled())
                {
                    identityOnlyReason = "transform feedback";
                }

                AttributeUsage.LinkVaryingLocations(nextStage.AttributeUsage, identityOnlyReason, indexed ? Constants.MaxAttributes : 32);
            }

            if (linked && nextStage.Definitions.GpPassthrough)
            {
                string identityOnlyReason = Definitions.OaIndexing ? "dynamically indexed attributes"
                    : _program.UsedFeatures.HasFlag(FeatureFlags.FixedFuncAttr) ? "fixed-function attributes" : null;
                nextStage.AttributeUsage.LinkPassthroughInputs(AttributeUsage, identityOnlyReason, Definitions.OaIndexing ? Constants.MaxAttributes : 32);
            }

            // We don't consider geometry shaders using the geometry shader passthrough feature
            // as being the last because when this feature is used, it can't actually modify any of the outputs,
            // so the stage that comes before it is the last one that can do modifications.
            if (nextStage.Definitions.Stage != ShaderStage.Fragment &&
                (nextStage.Definitions.Stage != ShaderStage.Geometry || !nextStage.Definitions.GpPassthrough))
            {
                Definitions.LastInVertexPipeline = false;
            }
        }

        public ShaderProgram Translate(bool asCompute = false)
        {
            ResourceManager resourceManager = CreateResourceManager(asCompute);

            bool usesLocalMemory = _program.UsedFeatures.HasFlag(FeatureFlags.LocalMemory);

            resourceManager.SetCurrentLocalMemory(_localMemorySize, usesLocalMemory);

            if (Stage == ShaderStage.Compute)
            {
                bool usesSharedMemory = _program.UsedFeatures.HasFlag(FeatureFlags.SharedMemory);

                resourceManager.SetCurrentSharedMemory(GpuAccessor.QueryComputeSharedMemorySize(), usesSharedMemory);

                // S2A：计算着色器的局部 / 共享内存大小只在真用到时进数组声明，用到才记「读过」（图形阶段的局部内存大小来自着色器头 = 客体代码本身）
                _s2aMemRead = (usesLocalMemory ? S2AStateRead.LocalMem : 0) | (usesSharedMemory ? S2AStateRead.SharedMem : 0);
            }

            Definitions.S2AVertexRaw = S2AVertexRawFor(asCompute);

            FunctionCode[] code = EmitShader(this, resourceManager, _program, asCompute, initializeOutputs: true, out _);

            if (S2APassthroughSideEffectsOnly)
            {
                S2APassthroughBodyOutputs = S2AHasOutputOrEmit(code);
            }

            ShaderProgram s2aProgram = Translate(code, resourceManager, _program.UsedFeatures, _program.ClipDistancesWritten, asCompute);
            S2AReportStateUse();
            return s2aProgram;
        }

        // S2A：转计算变换（GeometryToCompute）之前的中间码里有没有写输出 / 发射 / 结束图元（含输出初始化：直通属性不初始化，非直通的初始化也算本体写）
        private static bool S2AHasOutputOrEmit(FunctionCode[] code)
        {
            foreach (FunctionCode function in code)
            {
                foreach (Operation operation in function.Code)
                {
                    if (operation.Inst is Instruction.EmitVertex or Instruction.EndPrimitive ||
                        (operation.Inst == Instruction.Store && operation.StorageKind is StorageKind.Output or StorageKind.OutputPerPatch))
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        // S2A：把这次翻译读过的特化状态报给访问器（写进特化状态，缓存匹配只比读过的）
        private void S2AReportStateUse() => GpuAccessor.S2AReportStateUse(
            (Definitions.S2AA2cRead ? S2AStateRead.A2c : 0) | (Definitions.S2ADepthRead ? S2AStateRead.Depth : 0) | (Definitions.S2AAlphaRead ? S2AStateRead.Alpha : 0) |
            (Definitions.S2AEarlyZRead ? S2AStateRead.EarlyZ : 0) | (Definitions.S2ATopologyRead ? S2AStateRead.Topology : 0) | (Definitions.S2AHalvePrimRead ? S2AStateRead.HalvePrim : 0) |
            (Definitions.S2ATessRead ? S2AStateRead.Tess : 0) | (Definitions.S2APointSizeRead ? S2AStateRead.PointSize : 0) | _s2aMemRead,
            Definitions.S2AAttrRead);

        private uint _s2aMemRead;

        public ShaderProgram Translate(TranslatorContext other, bool asCompute = false)
        {
            ResourceManager resourceManager = CreateResourceManager(asCompute);

            bool usesLocalMemory = _program.UsedFeatures.HasFlag(FeatureFlags.LocalMemory);
            resourceManager.SetCurrentLocalMemory(_localMemorySize, usesLocalMemory);

            Definitions.S2AVertexRaw = S2AVertexRawFor(asCompute);
            other.Definitions.S2AVertexRaw = Definitions.S2AVertexRaw;

            FunctionCode[] code = EmitShader(this, resourceManager, _program, asCompute, initializeOutputs: false, out _);

            bool otherUsesLocalMemory = other._program.UsedFeatures.HasFlag(FeatureFlags.LocalMemory);
            resourceManager.SetCurrentLocalMemory(other._localMemorySize, otherUsesLocalMemory);

            FunctionCode[] otherCode = EmitShader(other, resourceManager, other._program, asCompute, initializeOutputs: true, out int aStart);

            code = Combine(otherCode, code, aStart);

            ShaderProgram s2aProgram = Translate(
                code,
                resourceManager,
                _program.UsedFeatures | other._program.UsedFeatures,
                (byte)(_program.ClipDistancesWritten | other._program.ClipDistancesWritten),
                asCompute);
            S2AReportStateUse();
            other.S2AReportStateUse();
            return s2aProgram;
        }

        // S2A（C1/F1）：单目标存储缓冲访存偏移静态可见为负时翻译期提示（零成本）：整体是负常量报【红】（低于客体基址；
        //   未对齐特化下 AlignDown 留出的前缀可能仍把它含在宿主窗口内，宿主侧结果由运行时窗口检查决定），寄存器 + 负常量报【黄】
        //   （Maxwell 产物里 `[Rx − 立即数]` 很常见，绝大多数合法）。只在钳位生效时收集。
        //   程序哈希 = 本阶段客体代码（含头）的 XXHash128，与 ShaderCache 存的 CachedShaderStage.Code 同一段字节。
        //   限次：全进程前 S2AStorageOffsetReportLines 条逐条打，之后只在第 2^k 条打一行带累计数。
        private const int S2AStorageOffsetReportLines = 32;
        private static int _s2aStorageOffsetReports;

        // S2A（F1）：开了越界记录诊断、但本阶段拿不到记录绑定时，「只检查不记录」的访存翻译期按阶段打一行【黄】，限次同上（独立计数）。
        private static int _s2aUnrecordedStorageReports;

        // S2A（F1）：本阶段身份（阶段 + 客体代码哈希）。GlobalToStorage 把它编进越界记录，渲染器读回后按同一哈希报红。
        private StorageRangeContext S2ACreateStorageRangeContext()
        {
            ReadOnlySpan<byte> code = MemoryMarshal.AsBytes(GpuAccessor.GetCode(Address, Size));
            Hash128 hash = Hash128.ComputeHash(code[..Math.Min(code.Length, Size)]);

            return new StorageRangeContext(Stage, hash.Low, hash.High);
        }

        private static void S2AReportStorageOffsetFindings(StorageRangeContext range)
        {
            if (range == null || range.Findings.Count == 0)
            {
                return;
            }

            string hash = new Hash128(range.CodeHashLow, range.CodeHashHigh).ToString();

            foreach (StorageOffsetFinding finding in range.Findings)
            {
                int count = Interlocked.Increment(ref _s2aStorageOffsetReports);

                if (count > S2AStorageOffsetReportLines && (count & (count - 1)) != 0)
                {
                    continue;
                }

                string level = finding.Definite ? "【红】" : "【黄】";
                string kind = finding.Definite ? "常量偏移低于客体基址" : "寄存器偏移带负常量、运行时可能低于客体基址";
                string offset = finding.Offset < 0 ? $"-0x{-finding.Offset:X}" : $"0x{finding.Offset:X}";

                Console.Error.WriteLine(
                    $"[gpu] {level}存储缓冲访存{kind}：程序 {hash} 阶段 {range.Stage} 基址 c{finding.CbSlot}[0x{finding.CbOffset * 4:X}] 偏移 {offset}；" +
                    $"宿主侧按绑定窗口检查，窗口外读 0、写丢弃（全进程第 {count} 条）");
            }
        }

        // S2A（F1）：越界记录缓冲（诊断开时）要着色器写存储缓冲；宿主无 vertexPipelineStoresAndAtomics 时顶点 / 细分 / 几何阶段不能写，
        //   这些阶段的访存仍按宿主窗口检查并钳位（不发出窗口外地址），但越界读 0 / 写丢弃时没有运行时记录、不报红。
        //   这里只在翻译期说明「本阶段有 N 处这样的访存」，不判断是否真越界，也不加 GPU 工作；命中宿主缓存不翻译就不打。
        private static void S2AReportUnrecordedStorageAccesses(StorageRangeContext range)
        {
            if (range == null || range.UnrecordedSingleTarget == 0 && range.UnrecordedMultiTarget == 0)
            {
                return;
            }

            int count = Interlocked.Increment(ref _s2aUnrecordedStorageReports);

            if (count > S2AStorageOffsetReportLines && (count & (count - 1)) != 0)
            {
                return;
            }

            string hash = new Hash128(range.CodeHashLow, range.CodeHashHigh).ToString();

            Console.Error.WriteLine(
                $"[gpu] 【黄】存储缓冲访存越界不记录：程序 {hash} 阶段 {range.Stage} 单目标 {range.UnrecordedSingleTarget} 处、" +
                $"多目标 {range.UnrecordedMultiTarget} 处只做宿主绑定窗口检查与钳位（越界读 0、写 / 原子丢弃），已开越界记录诊断但本阶段没有记录绑定" +
                $"（顶点 / 细分 / 几何阶段且宿主无 vertexPipelineStoresAndAtomics），越界时不报红（全进程第 {count} 个阶段）");
        }

        private ShaderProgram Translate(FunctionCode[] functions, ResourceManager resourceManager, FeatureFlags usedFeatures, byte clipDistancesWritten, bool asCompute)
        {
            if (asCompute)
            {
                usedFeatures |= FeatureFlags.VtgAsCompute;
            }

            ControlFlowGraph[] cfgs = new ControlFlowGraph[functions.Length];
            RegisterUsage.FunctionRegisterUsage[] frus = new RegisterUsage.FunctionRegisterUsage[functions.Length];

            for (int i = 0; i < functions.Length; i++)
            {
                cfgs[i] = ControlFlowGraph.Create(functions[i].Code);

                if (i != 0)
                {
                    frus[i] = RegisterUsage.RunPass(cfgs[i]);
                }
            }

            List<Function> funcs = new(functions.Length);

            for (int i = 0; i < functions.Length; i++)
            {
                funcs.Add(null);
            }

            HelperFunctionManager hfm = new(funcs, Definitions.Stage);
            // S2A（F1）：钳位不生效（Metal、NVIDIA）时不算哈希、不收集，GlobalToStorage 走改前原样代码
            StorageRangeContext storageRange = GpuAccessor.QueryHostStorageWindowClamp() ? S2ACreateStorageRangeContext() : null;

            for (int i = 0; i < functions.Length; i++)
            {
                ControlFlowGraph cfg = cfgs[i];

                int inArgumentsCount = 0;
                int outArgumentsCount = 0;

                if (i != 0)
                {
                    RegisterUsage.FunctionRegisterUsage fru = frus[i];

                    inArgumentsCount = fru.InArguments.Length;
                    outArgumentsCount = fru.OutArguments.Length;
                }

                if (cfg.Blocks.Length != 0)
                {
                    RegisterUsage.FixupCalls(cfg.Blocks, frus);

                    Dominance.FindDominators(cfg);
                    Dominance.FindDominanceFrontiers(cfg.Blocks);

                    Ssa.Rename(cfg.Blocks);

                    TransformContext context = new(
                        hfm,
                        cfg.Blocks,
                        Definitions,
                        resourceManager,
                        GpuAccessor,
                        Options.TargetApi,
                        Options.TargetLanguage,
                        Definitions.Stage,
                        ref usedFeatures,
                        storageRange);

                    Optimizer.RunPass(context);
                    TransformPasses.RunPass(context);
                }

                funcs[i] = new Function(cfg.Blocks, $"fun{i}", false, inArgumentsCount, outArgumentsCount);
            }

            S2AReportStorageOffsetFindings(storageRange);
            S2AReportUnrecordedStorageAccesses(storageRange);

            return Generate(
                funcs,
                AttributeUsage,
                GetDefinitions(asCompute),
                Definitions,
                resourceManager,
                usedFeatures,
                clipDistancesWritten);
        }

        private ShaderProgram Generate(
            IReadOnlyList<Function> funcs,
            AttributeUsage attributeUsage,
            ShaderDefinitions definitions,
            ShaderDefinitions originalDefinitions,
            ResourceManager resourceManager,
            FeatureFlags usedFeatures,
            byte clipDistancesWritten)
        {
            StructuredProgramInfo sInfo = StructuredProgram.MakeStructuredProgram(
                funcs,
                attributeUsage,
                definitions,
                resourceManager,
                Options.TargetLanguage,
                Options.Flags.HasFlag(TranslationFlags.DebugMode));

            attributeUsage.ObserveFinalInterface(sInfo.IoDefinitions, definitions.Stage);

            int geometryVerticesPerPrimitive = Definitions.OutputTopology switch
            {
                OutputTopology.LineStrip => 2,
                OutputTopology.TriangleStrip => 3,
                _ => 1
            };

            ShaderProgramInfo info = new ShaderProgramInfo(
                resourceManager.GetConstantBufferDescriptors(),
                resourceManager.GetStorageBufferDescriptors(),
                resourceManager.GetTextureDescriptors(),
                resourceManager.GetImageDescriptors(),
                originalDefinitions.Stage,
                geometryVerticesPerPrimitive,
                originalDefinitions.MaxOutputVertices,
                originalDefinitions.ThreadsPerInputPrimitive,
                usedFeatures.HasFlag(FeatureFlags.FragCoordXY),
                usedFeatures.HasFlag(FeatureFlags.InstanceId),
                usedFeatures.HasFlag(FeatureFlags.DrawParameters),
                usedFeatures.HasFlag(FeatureFlags.RtLayer),
                clipDistancesWritten,
                originalDefinitions.OmapTargets);

            HostCapabilities hostCapabilities = new HostCapabilities(
                GpuAccessor.QueryHostReducedPrecision(),
                GpuAccessor.QueryHostSupportsFragmentShaderInterlock(),
                GpuAccessor.QueryHostSupportsFragmentShaderOrderingIntel(),
                GpuAccessor.QueryHostSupportsGeometryShaderPassthrough(),
                GpuAccessor.QueryHostSupportsShaderBallot(),
                GpuAccessor.QueryHostSupportsShaderBarrierDivergence(),
                GpuAccessor.QueryHostSupportsShaderFloat64(),
                GpuAccessor.QueryHostSupportsShaderNonUniformIndexing(),
                GpuAccessor.QueryHostSupportsTextureShadowLod(),
                GpuAccessor.QueryHostSupportsViewportMask(),
                GpuAccessor.QueryHostSupportsSpirv15(),
                GpuAccessor.QueryHostSpirvLayerExt(),
                GpuAccessor.QueryHostSpirvLayerNv(),
                GpuAccessor.QueryHostSupportsLayerVertexTessellation(),
                GpuAccessor.QueryHostSupportsViewportIndexVertexTessellation());

            CodeGenParameters parameters = new CodeGenParameters(attributeUsage, definitions, resourceManager.Properties, hostCapabilities, GpuAccessor, Options.TargetApi);

            return Options.TargetLanguage switch
            {
                TargetLanguage.Glsl => new ShaderProgram(info, TargetLanguage.Glsl, GlslGenerator.Generate(sInfo, parameters)),
                TargetLanguage.Spirv => new ShaderProgram(info, TargetLanguage.Spirv, SpirvGenerator.Generate(sInfo, parameters)),
                _ => throw new NotImplementedException(Options.TargetLanguage.ToString()),
            };
        }

        private ResourceManager CreateResourceManager(bool vertexAsCompute)
        {
            ResourceManager resourceManager = new(Definitions.Stage, GpuAccessor, GetResourceReservations());

            if (IsTransformFeedbackEmulated)
            {
                StructureType tfeDataStruct = new(new StructureField[]
                {
                    new(AggregateType.Array | AggregateType.U32, "data", 0)
                });

                for (int i = 0; i < ResourceReservations.TfeBuffersCount; i++)
                {
                    int binding = resourceManager.Reservations.GetTfeBufferStorageBufferBinding(i);
                    BufferDefinition tfeDataBuffer = new(BufferLayout.Std430, 1, binding, $"tfe_data{i}", tfeDataStruct);
                    resourceManager.Properties.AddOrUpdateStorageBuffer(tfeDataBuffer);
                }
            }

            if (vertexAsCompute)
            {
                int vertexInfoCbBinding = resourceManager.Reservations.VertexInfoConstantBufferBinding;
                BufferDefinition vertexInfoBuffer = new(BufferLayout.Std140, 0, vertexInfoCbBinding, "vb_info", VertexInfoBuffer.GetStructureType());
                resourceManager.Properties.AddOrUpdateConstantBuffer(vertexInfoBuffer);

                StructureType vertexOutputStruct = new(new StructureField[]
                {
                    new(AggregateType.Array | AggregateType.FP32, "data", 0)
                });

                int vertexOutputSbBinding = resourceManager.Reservations.VertexOutputStorageBufferBinding;
                BufferDefinition vertexOutputBuffer = new(BufferLayout.Std430, 1, vertexOutputSbBinding, "vertex_output", vertexOutputStruct);
                resourceManager.Properties.AddOrUpdateStorageBuffer(vertexOutputBuffer);

                if (Stage == ShaderStage.Vertex)
                {
                    SetBindingPair ibSetAndBinding = resourceManager.Reservations.GetIndexBufferTextureSetAndBinding();
                    TextureDefinition indexBuffer = new(ibSetAndBinding.SetIndex, ibSetAndBinding.Binding, "ib_data", SamplerType.TextureBuffer, AggregateType.U32);
                    resourceManager.Properties.AddOrUpdateTexture(indexBuffer);

                    int inputMap = _program.AttributeUsage.UsedInputAttributes;

                    while (inputMap != 0)
                    {
                        int location = BitOperations.TrailingZeroCount(inputMap);
                        SetBindingPair setAndBinding = resourceManager.Reservations.GetVertexBufferTextureSetAndBinding(location);
                        TextureDefinition vaBuffer = new(setAndBinding.SetIndex, setAndBinding.Binding, $"vb_data{location}", SamplerType.TextureBuffer, GetVertexTexelSampledType(location));
                        resourceManager.Properties.AddOrUpdateTexture(vaBuffer);

                        inputMap &= ~(1 << location);
                    }
                }
                else if (Stage == ShaderStage.Geometry)
                {
                    SetBindingPair trbSetAndBinding = resourceManager.Reservations.GetTopologyRemapBufferTextureSetAndBinding();
                    TextureDefinition remapBuffer = new(trbSetAndBinding.SetIndex, trbSetAndBinding.Binding, "trb_data", SamplerType.TextureBuffer, AggregateType.U32);
                    resourceManager.Properties.AddOrUpdateTexture(remapBuffer);

                    int geometryVbOutputSbBinding = resourceManager.Reservations.GeometryVertexOutputStorageBufferBinding;
                    BufferDefinition geometryVbOutputBuffer = new(BufferLayout.Std430, 1, geometryVbOutputSbBinding, "geometry_vb_output", vertexOutputStruct);
                    resourceManager.Properties.AddOrUpdateStorageBuffer(geometryVbOutputBuffer);

                    StructureType geometryIbOutputStruct = new(new StructureField[]
                    {
                        new(AggregateType.Array | AggregateType.U32, "data", 0)
                    });

                    int geometryIbOutputSbBinding = resourceManager.Reservations.GeometryIndexOutputStorageBufferBinding;
                    BufferDefinition geometryIbOutputBuffer = new(BufferLayout.Std430, 1, geometryIbOutputSbBinding, "geometry_ib_output", geometryIbOutputStruct);
                    resourceManager.Properties.AddOrUpdateStorageBuffer(geometryIbOutputBuffer);
                }

                resourceManager.SetVertexAsComputeLocalMemories(Definitions.Stage, Definitions.InputTopology);
            }

            return resourceManager;
        }

        // Match the actual single-component view chosen by FormatTable, including raw fallbacks.
        private AggregateType GetVertexTexelSampledType(int location)
        {
            AttributeType type = Definitions.GetAttributeType(location);
            if (type.HasFlag(AttributeType.TexelDecode)) return AggregateType.U32;
            AttributeType basic = type & AttributeType.BaseTypeMask;
            if (type.HasFlag(AttributeType.TexelInvalidFormat))
            {
                return basic switch
                {
                    AttributeType.Sint => AggregateType.S32,
                    AttributeType.Uint => AggregateType.U32,
                    _ => AggregateType.FP32,
                };
            }
            if (type.HasFlag(AttributeType.Packed))
            {
                if (basic == AttributeType.Sscaled) return AggregateType.S32;
                if (basic is AttributeType.Sint or AttributeType.Uint or AttributeType.Uscaled ||
                    type.HasFlag(AttributeType.PackedRgb10A2Signed)) return AggregateType.U32;
                return AggregateType.FP32;
            }
            return basic switch
            {
                AttributeType.Sint or AttributeType.Sscaled => AggregateType.S32,
                AttributeType.Uint or AttributeType.Uscaled => AggregateType.U32,
                _ => AggregateType.FP32,
            };
        }

        private ShaderDefinitions GetDefinitions(bool vertexAsCompute)
        {
            if (vertexAsCompute)
            {
                return new ShaderDefinitions(ShaderStage.Compute, 32, GpuAccessor.QueryGeometryInputFixed() ? 1 : 32, 1);
            }
            else
            {
                return Definitions;
            }
        }

        public ResourceReservations GetResourceReservations()
        {
            IoUsage ioUsage = _program.GetIoUsage();

            if (Definitions.GpPassthrough)
            {
                ioUsage = ioUsage.Combine(_vertexOutput);

                // S2A：直通几何转计算时直通的属性（PassthroughAttributes）也要在输出表里。上游拿顶点输出代替，前提是直通的 ⊆ 顶点写的；
                // 阶段独立翻译（MergeAllGenericInputsForFragment）下直通 = 全部 32 个位置，顶点没写的在输出表里查不到，几何转计算核留下 Output 变量编不过（异度神剑3 10-23）
                ioUsage = ioUsage.Combine(new IoUsage(FeatureFlags.None, 0, AttributeUsage.PassthroughAttributes));
            }

            return new ResourceReservations(GpuAccessor, IsTransformFeedbackEmulated, vertexAsCompute: true, _vertexOutput, ioUsage);
        }

        // S2A：抓顶点读取器里记录第 ioDefinition 项的值（见 S2AGeneratePassthroughCaptureReader）：直通模拟拷的（位置、直通属性）读抓顶点里同一项输入，其余给记录初值
        private Operand S2ACaptureValue(EmitterContext context, ResourceReservations reservations, IoDefinition output, Operand baseOffset, int binding)
        {
            bool copied = output.IoVariable == IoVariable.Position ||
                (output.IoVariable == IoVariable.UserDefined && (AttributeUsage.PassthroughAttributes & (1 << output.Location)) != 0);

            if (copied && reservations.TryGetOffset(StorageKind.Input, output.IoVariable, output.Location, output.Component, out int inputOffset))
            {
                return context.Load(StorageKind.StorageBuffer, binding, Const(0), inputOffset != 0 ? context.IAdd(baseOffset, Const(inputOffset)) : baseOffset);
            }

            bool one = (output.IoVariable == IoVariable.Position && output.Component == 3) || output.IoVariable == IoVariable.PointSize ||
                (output.IoVariable == IoVariable.UserDefined && output.Component == 3);

            return ConstF(one ? 1f : 0f);
        }

        public void SetVertexOutputMapForGeometryAsCompute(TranslatorContext vertexContext)
        {
            _vertexOutput = vertexContext._program.GetIoUsage();
        }

        public ShaderProgram GenerateVertexPassthroughForCompute() => GenerateVertexPassthroughForCompute(s2aCapture: false);

        /// <summary>S2A：直通几何快路的抓顶点读取器（直连库 _bgc）：与读记录变体（_bgv）同一份生成，按顶点号读顶点阶段抓下的输出（几何输入固定布局，
        /// ResourceReservations.FillGeometryInputMap，每顶点 144 字，绑定名 vertex_output = 抓顶点缓冲）而不是几何核写的记录。
        /// 记录里每个输出的值：位置与直通属性 = 直通模拟从输入拷来的（同一偏移的输入），其余 = 记录初值（转计算的本地顶点数据初始化：位置 w、点大小、通用属性 w 为 1，其余 0）——
        /// 前提是几何本体不写输出、不自己发射（S2APassthroughBodyOutputs 为假），由调用方保证</summary>
        public ShaderProgram S2AGeneratePassthroughCaptureReader()
        {
            if (!Definitions.GpPassthrough || Definitions.Stage != ShaderStage.Geometry || !GetResourceReservations().GeometryInputFixed)
            {
                throw new InvalidOperationException("Capture reader needs a passthrough geometry shader with the fixed geometry input layout.");
            }

            return GenerateVertexPassthroughForCompute(s2aCapture: true);
        }

        private ShaderProgram GenerateVertexPassthroughForCompute(bool s2aCapture)
        {
            AttributeUsage attributeUsage = new AttributeUsage(GpuAccessor);
            attributeUsage.InheritLinkedOutputs(AttributeUsage);
            ResourceManager resourceManager = new ResourceManager(ShaderStage.Vertex, GpuAccessor);

            ResourceReservations reservations = GetResourceReservations();

            int vertexInfoCbBinding = reservations.VertexInfoConstantBufferBinding;

            if (Stage == ShaderStage.Vertex)
            {
                BufferDefinition vertexInfoBuffer = new(BufferLayout.Std140, 0, vertexInfoCbBinding, "vb_info", VertexInfoBuffer.GetStructureType());
                resourceManager.Properties.AddOrUpdateConstantBuffer(vertexInfoBuffer);
            }

            StructureType vertexInputStruct = new(new StructureField[]
            {
                new(AggregateType.Array | AggregateType.FP32, "data", 0)
            });

            int vertexDataSbBinding = reservations.VertexOutputStorageBufferBinding;
            BufferDefinition vertexOutputBuffer = new(BufferLayout.Std430, 1, vertexDataSbBinding, s2aCapture ? "vertex_output" : "vb_input", vertexInputStruct, flags: BufferUsageFlags.None);
            resourceManager.Properties.AddOrUpdateStorageBuffer(vertexOutputBuffer);

            EmitterContext context = new EmitterContext();

            Operand vertexIndex = Options.TargetApi == TargetApi.OpenGL
                ? context.Load(StorageKind.Input, IoVariable.VertexId)
                : context.Load(StorageKind.Input, IoVariable.VertexIndex);

            if (Stage == ShaderStage.Vertex)
            {
                Operand vertexCount = context.Load(StorageKind.ConstantBuffer, vertexInfoCbBinding, Const((int)VertexInfoBufferField.VertexCounts), Const(0));

                // Base instance will be always zero when this shader is used, so which one we use here doesn't really matter.
                Operand instanceId = Options.TargetApi == TargetApi.OpenGL
                    ? context.Load(StorageKind.Input, IoVariable.InstanceId)
                    : context.Load(StorageKind.Input, IoVariable.InstanceIndex);

                vertexIndex = context.IAdd(context.IMultiply(instanceId, vertexCount), vertexIndex);
            }

            Operand baseOffset = context.IMultiply(vertexIndex, Const(s2aCapture ? reservations.InputSizePerInvocation : reservations.OutputSizePerInvocation));

            int userOutputs = 0;   // generic output locations taken from the record (S2AReaderDefaultsUnwritten)

            foreach ((IoDefinition ioDefinition, int inputOffset) in reservations.Offsets)
            {
                if (ioDefinition.StorageKind != StorageKind.Output)
                {
                    continue;
                }

                Operand value;

                if (s2aCapture)
                {
                    value = S2ACaptureValue(context, reservations, ioDefinition, baseOffset, vertexDataSbBinding);
                }
                else
                {
                    Operand vertexOffset = inputOffset != 0 ? context.IAdd(baseOffset, Const(inputOffset)) : baseOffset;
                    value = context.Load(StorageKind.StorageBuffer, vertexDataSbBinding, Const(0), vertexOffset);
                }

                if (ioDefinition.IoVariable == IoVariable.UserDefined)
                {
                    context.Store(StorageKind.Output, ioDefinition.IoVariable, null, Const(ioDefinition.Location), Const(ioDefinition.Component), value);
                    attributeUsage.SetOutputUserAttribute(ioDefinition.Location);
                    userOutputs |= 1 << ioDefinition.Location;
                }
                else if (ResourceReservations.IsVectorOrArrayVariable(ioDefinition.IoVariable))
                {
                    context.Store(StorageKind.Output, ioDefinition.IoVariable, null, Const(ioDefinition.Component), value);
                }
                else
                {
                    context.Store(StorageKind.Output, ioDefinition.IoVariable, null, value);
                }
            }

            if (S2AReaderDefaultsUnwritten)
            {
                for (int location = 0; location < 32; location++)
                {
                    if ((userOutputs & (1 << location)) != 0)
                    {
                        continue;
                    }

                    for (int c = 0; c < 4; c++)
                    {
                        context.Store(StorageKind.Output, IoVariable.UserDefined, null, Const(location), Const(c), c == 3 ? ConstF(1f) : ConstF(0f));
                    }

                    attributeUsage.SetOutputUserAttribute(location);
                }
            }

            Operation[] operations = context.GetOperations();
            ControlFlowGraph cfg = ControlFlowGraph.Create(operations);
            Function function = new Function(cfg.Blocks, "main", false, 0, 0);

            TransformFeedbackOutput[] transformFeedbackOutputs = GetTransformFeedbackOutputs(GpuAccessor, out ulong transformFeedbackVecMap);

            ShaderDefinitions definitions = new ShaderDefinitions(ShaderStage.Vertex, transformFeedbackVecMap, transformFeedbackOutputs)
            {
                LastInVertexPipeline = true
            };

            ShaderProgram program = Generate(
                new[] { function },
                attributeUsage,
                definitions,
                definitions,
                resourceManager,
                FeatureFlags.None,
                0);

            // 只有成功发射的宿主 VS 才是后补 GS 的真实生产者；Compute 本身没有图形输出接口。
            _vertexPassthroughAttributeUsage = attributeUsage;
            return program;
        }

        public ShaderProgram GenerateGeometryPassthrough()
        {
            AttributeUsage producerUsage = AttributeUsage.RequiresLinkedVaryingInterface
                ? _vertexPassthroughAttributeUsage ?? AttributeUsage : AttributeUsage;
            int outputAttributesMask = producerUsage.RequiresLinkedVaryingInterface
                ? producerUsage.FinalOutputAttributes : producerUsage.UsedOutputAttributes;
            int layerOutputAttr = LayerOutputAttribute;

            if (LayerOutputWritten)
            {
                outputAttributesMask |= 1 << ((layerOutputAttr - AttributeConsts.UserAttributeBase) / 16);
            }

            OutputTopology outputTopology;
            int maxOutputVertices;

            switch (Definitions.InputTopology)
            {
                case InputTopology.Points:
                    outputTopology = OutputTopology.PointList;
                    maxOutputVertices = 1;
                    break;
                case InputTopology.Lines:
                case InputTopology.LinesAdjacency:
                    outputTopology = OutputTopology.LineStrip;
                    maxOutputVertices = 2;
                    break;
                default:
                    outputTopology = OutputTopology.TriangleStrip;
                    maxOutputVertices = 3;
                    break;
            }

            AttributeUsage attributeUsage = new AttributeUsage(GpuAccessor);
            attributeUsage.InheritLinkedOutputs(producerUsage, passthroughInputs: true);
            ResourceManager resourceManager = new ResourceManager(ShaderStage.Geometry, GpuAccessor);

            EmitterContext context = new EmitterContext();

            for (int v = 0; v < maxOutputVertices; v++)
            {
                int outAttrsMask = outputAttributesMask;

                while (outAttrsMask != 0)
                {
                    int attrIndex = BitOperations.TrailingZeroCount(outAttrsMask);

                    outAttrsMask &= ~(1 << attrIndex);

                    for (int c = 0; c < 4; c++)
                    {
                        int attr = AttributeConsts.UserAttributeBase + attrIndex * 16 + c * 4;

                        Operand value = context.Load(StorageKind.Input, IoVariable.UserDefined, Const(attrIndex), Const(v), Const(c));

                        if (attr == layerOutputAttr)
                        {
                            context.Store(StorageKind.Output, IoVariable.Layer, null, value);
                        }
                        else
                        {
                            context.Store(StorageKind.Output, IoVariable.UserDefined, null, Const(attrIndex), Const(c), value);
                        }
                    }
                }

                for (int c = 0; c < 4; c++)
                {
                    Operand value = context.Load(StorageKind.Input, IoVariable.Position, Const(v), Const(c));

                    context.Store(StorageKind.Output, IoVariable.Position, null, Const(c), value);
                }

                context.EmitVertex();
            }

            context.EndPrimitive();

            Operation[] operations = context.GetOperations();
            ControlFlowGraph cfg = ControlFlowGraph.Create(operations);
            Function function = new Function(cfg.Blocks, "main", false, 0, 0);

            ShaderDefinitions definitions = new ShaderDefinitions(
                ShaderStage.Geometry,
                GpuAccessor.QueryGraphicsState(),
                false,
                1,
                outputTopology,
                maxOutputVertices);

            return Generate(
                new[] { function },
                attributeUsage,
                definitions,
                definitions,
                resourceManager,
                FeatureFlags.RtLayer,
                0);
        }
    }
}
