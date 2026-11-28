using Ryujinx.Common;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Threading;
using static Spv.Specification;

namespace Ryujinx.Graphics.Shader.CodeGen.Spirv
{
    using SpvInstruction = Spv.Generator.Instruction;
    using SpvInstructionPool = Spv.Generator.GeneratorPool<Spv.Generator.Instruction>;
    using SpvLiteralInteger = Spv.Generator.LiteralInteger;
    using SpvLiteralIntegerPool = Spv.Generator.GeneratorPool<Spv.Generator.LiteralInteger>;

    static class SpirvGenerator
    {
        // Resource pools for Spirv generation. Note: Increase count when more threads are being used.
        [ThreadStatic] internal static Action<long, long> PhaseSample;
        [ThreadStatic] internal static Func<long> PhaseCpuClock;

        private const int GeneratorPoolCount = 1;
        private static readonly ObjectPool<SpvInstructionPool> _instructionPool;
        private static readonly ObjectPool<SpvLiteralIntegerPool> _integerPool;
        private static readonly Lock _poolLock = new();

        static SpirvGenerator()
        {
            _instructionPool = new(() => new SpvInstructionPool(), GeneratorPoolCount);
            _integerPool = new(() => new SpvLiteralIntegerPool(), GeneratorPoolCount);
        }

        private const HelperFunctionsMask NeedsInvocationIdMask = HelperFunctionsMask.SwizzleAdd;

        public static byte[] Generate(StructuredProgramInfo info, CodeGenParameters parameters)
        {
            var phaseSample = PhaseSample;
            long wallStart = phaseSample == null ? 0 : System.Diagnostics.Stopwatch.GetTimestamp();
            long cpuStart = phaseSample == null ? 0 : PhaseCpuClock();
            SpvInstructionPool instPool;
            SpvLiteralIntegerPool integerPool;

            lock (_poolLock)
            {
                instPool = _instructionPool.Allocate();
                integerPool = _integerPool.Allocate();
            }

            CodeGenContext context = new(info, parameters, instPool, integerPool);

            context.AddCapability(Capability.Shader);

            // Reference port: raw global memory goes through physical storage buffer pointers (ShaderProperties.UsesPhysicalPointers), which take 64-bit physical addressing
            if (context.Properties.UsesPhysicalPointers)
            {
                context.AddCapability(Capability.PhysicalStorageBufferAddresses);
                context.AddCapability(Capability.Int64);
                context.AddExtension("SPV_KHR_physical_storage_buffer");
                context.SetMemoryModel(AddressingModel.PhysicalStorageBuffer64, MemoryModel.GLSL450);
            }
            else
            {
                context.SetMemoryModel(AddressingModel.Logical, MemoryModel.GLSL450);
            }

            context.AddCapability(Capability.GroupNonUniformBallot);
            context.AddCapability(Capability.GroupNonUniformShuffle);
            context.AddCapability(Capability.GroupNonUniformVote);
            context.AddCapability(Capability.GroupNonUniformArithmetic); // S2Arm: vertex / domain-stage lane id emulation (Instructions.LaneIdEmulated)
            context.AddCapability(Capability.ImageBuffer);
            context.AddCapability(Capability.ImageGatherExtended);
            context.AddCapability(Capability.ImageQuery);
            context.AddCapability(Capability.SampledBuffer);

            // S2Arm（#1774）：Float64 只在真建了 64 位浮点类型时声明（CodeGenContext.TypeFP64），不再按「宿主支持」声明——
            //   原先宿主答「支持」时每个模块都带 OpCapability Float64，没开 shaderFloat64 的设备建模块就违反 VUID-…-08740。

            if (parameters.Definitions.TransformFeedbackEnabled && parameters.Definitions.LastInVertexPipeline)
            {
                context.AddCapability(Capability.TransformFeedback);
            }

            if (parameters.Definitions.Stage == ShaderStage.Fragment)
            {
                if (context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Input, IoVariable.Layer)) ||
                    context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Input, IoVariable.PrimitiveId)))
                {
                    context.AddCapability(Capability.Geometry);
                }

                // S2Arm（#1774）：片元互锁的能力和扩展只在程序真有互锁临界区（HelperFunctionsMask.FSI）时声明，与下面的执行模式同条件；
                //   原先按「宿主支持」声明，Mali-G710 上 94.6% 的离线库模块声明了却没有一条互锁指令，设备没有这个扩展就建模块失败。
                if ((context.Info.HelperFunctionsMask & HelperFunctionsMask.FSI) != 0 && context.HostCapabilities.SupportsFragmentShaderInterlock)
                {
                    context.AddCapability(Capability.FragmentShaderPixelInterlockEXT);
                    context.AddExtension("SPV_EXT_fragment_shader_interlock");
                }
            }
            else if (parameters.Definitions.Stage == ShaderStage.Geometry)
            {
                context.AddCapability(Capability.Geometry);

                if (parameters.Definitions.GpPassthrough && context.HostCapabilities.SupportsGeometryShaderPassthrough)
                {
                    context.AddExtension("SPV_NV_geometry_shader_passthrough");
                    context.AddCapability(Capability.GeometryShaderPassthroughNV);
                }
            }
            else if (parameters.Definitions.Stage is ShaderStage.TessellationControl or
                     ShaderStage.TessellationEvaluation)
            {
                context.AddCapability(Capability.Tessellation);
            }
            else if (parameters.Definitions.Stage == ShaderStage.Vertex)
            {
                context.AddCapability(Capability.DrawParameters);
            }

            if (context.Definitions.Stage != ShaderStage.Fragment &&
                context.Definitions.Stage != ShaderStage.Geometry &&
                context.Definitions.Stage != ShaderStage.Compute &&
                (context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Output, IoVariable.Layer)) ||
                context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Output, IoVariable.ViewportIndex))))
            {
                if (context.IsSpirv15)
                {
                    if (context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Output, IoVariable.Layer)))
                    {
                        if (!context.HostCapabilities.LayerOutput)
                        {
                            throw new NotSupportedException("ShaderLayer output is not enabled on this host.");
                        }
                        context.AddCapability(Capability.ShaderLayer);
                    }
                    if (context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Output, IoVariable.ViewportIndex)))
                    {
                        if (!context.HostCapabilities.ViewportOutput)
                        {
                            throw new NotSupportedException("ShaderViewportIndex output is not enabled on this host.");
                        }
                        context.AddCapability(Capability.ShaderViewportIndex);
                    }
                }
                else
                {
                    if (context.HostCapabilities.SpirvLayerExt)
                    {
                        context.AddExtension("SPV_EXT_shader_viewport_index_layer");
                    }
                    else if (context.HostCapabilities.SpirvLayerNv)
                    {
                        context.AddExtension("SPV_NV_viewport_array2");
                    }
                    else
                    {
                        throw new NotSupportedException("No enabled SPIR-V Layer/ViewportIndex output route.");
                    }
                    context.AddCapability(Capability.ShaderViewportIndexLayerEXT);
                }
            }

            if (context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Output, IoVariable.ViewportMask)))
            {
                context.AddExtension("SPV_NV_viewport_array2");
                context.AddCapability(Capability.ShaderViewportMaskNV);
            }

            if ((info.HelperFunctionsMask & NeedsInvocationIdMask) != 0)
            {
                info.IoDefinitions.Add(new IoDefinition(StorageKind.Input, IoVariable.SubgroupLaneId));
            }

            Declarations.DeclareAll(context, info);

            for (int funcIndex = 0; funcIndex < info.Functions.Count; funcIndex++)
            {
                StructuredFunction function = info.Functions[funcIndex];
                SpvInstruction retType = context.GetType(function.ReturnType);

                SpvInstruction[] funcArgs = new SpvInstruction[function.InArguments.Length + function.OutArguments.Length];

                for (int argIndex = 0; argIndex < funcArgs.Length; argIndex++)
                {
                    SpvInstruction argType = context.GetType(function.GetArgumentType(argIndex));
                    SpvInstruction argPointerType = context.TypePointer(StorageClass.Function, argType);
                    funcArgs[argIndex] = argPointerType;
                }

                SpvInstruction funcType = context.TypeFunction(retType, false, funcArgs);
                SpvInstruction spvFunc = context.Function(retType, FunctionControlMask.MaskNone, funcType);

                // Reference port: helpers whose body the shader build replaces in the Metal source keep their name (dmsh_hmma16816_f16)
                if (function.Name.StartsWith("dmsh_"))
                {
                    context.Name(spvFunc, function.Name);
                }

                context.DeclareFunction(funcIndex, function, spvFunc);
            }

            for (int funcIndex = 0; funcIndex < info.Functions.Count; funcIndex++)
            {
                Generate(context, info, funcIndex);
            }

            byte[] result = context.Generate();

            lock (_poolLock)
            {
                _instructionPool.Release(instPool);
                _integerPool.Release(integerPool);
            }

            if (phaseSample != null)
                phaseSample(System.Diagnostics.Stopwatch.GetTimestamp() - wallStart, PhaseCpuClock() - cpuStart);
            return result;
        }

        private static void Generate(CodeGenContext context, StructuredProgramInfo info, int funcIndex)
        {
            (StructuredFunction function, SpvInstruction spvFunc) = context.GetFunction(funcIndex);

            context.CurrentFunction = function;
            context.AddFunction(spvFunc);
            context.StartFunction(isMainFunction: funcIndex == 0);

            Declarations.DeclareParameters(context, function);

            context.EnterBlock(function.MainBlock);

            Declarations.DeclareLocals(context, function);

            Generate(context, function.MainBlock);

            // Functions must always end with a return.
            if (function.MainBlock.Last is not AstOperation operation ||
                (operation.Inst != Instruction.Return && operation.Inst != Instruction.Discard))
            {
                context.Return();
            }

            context.FunctionEnd();

            if (funcIndex == 0)
            {
                context.AddEntryPoint(context.Definitions.Stage.Convert(), spvFunc, "main", context.GetMainInterface());

                if (context.Definitions.Stage == ShaderStage.TessellationControl)
                {
                    context.AddExecutionMode(spvFunc, ExecutionMode.OutputVertices, (SpvLiteralInteger)context.Definitions.ThreadsPerInputPrimitive);
                }

                // S2A：曲面控制（TCS）也声明域 / 分区 / 绕序（SPIR-V 允许 TCS、TES 任一或两者声明，两者须一致）。Metal 原生曲面细分里 TCS 翻成计算核写细分因子缓冲，
                // 因子布局（三角 / 四边形）取决于域：TCS 不带域就无法单独翻译、离线库也无法按单支着色器索引（graphics/metal/曲面几何设计.md）
                if (context.Definitions.Stage == ShaderStage.TessellationControl || context.Definitions.Stage == ShaderStage.TessellationEvaluation)
                {
                    switch (context.Definitions.TessPatchType)
                    {
                        case TessPatchType.Isolines:
                            context.AddExecutionMode(spvFunc, ExecutionMode.Isolines);
                            break;
                        case TessPatchType.Triangles:
                            context.AddExecutionMode(spvFunc, ExecutionMode.Triangles);
                            break;
                        case TessPatchType.Quads:
                            context.AddExecutionMode(spvFunc, ExecutionMode.Quads);
                            break;
                    }

                    switch (context.Definitions.TessSpacing)
                    {
                        case TessSpacing.EqualSpacing:
                            context.AddExecutionMode(spvFunc, ExecutionMode.SpacingEqual);
                            break;
                        case TessSpacing.FractionalEventSpacing:
                            context.AddExecutionMode(spvFunc, ExecutionMode.SpacingFractionalEven);
                            break;
                        case TessSpacing.FractionalOddSpacing:
                            context.AddExecutionMode(spvFunc, ExecutionMode.SpacingFractionalOdd);
                            break;
                    }

                    bool tessCw = context.Definitions.TessCw;

                    if (context.TargetApi == TargetApi.Vulkan)
                    {
                        // We invert the front face on Vulkan backend, so we need to do that here as well.
                        tessCw = !tessCw;
                    }

                    if (tessCw)
                    {
                        context.AddExecutionMode(spvFunc, ExecutionMode.VertexOrderCw);
                    }
                    else
                    {
                        context.AddExecutionMode(spvFunc, ExecutionMode.VertexOrderCcw);
                    }
                }
                else if (context.Definitions.Stage == ShaderStage.Geometry)
                {
                    context.AddExecutionMode(spvFunc, context.Definitions.InputTopology switch
                    {
                        InputTopology.Points => ExecutionMode.InputPoints,
                        InputTopology.Lines => ExecutionMode.InputLines,
                        InputTopology.LinesAdjacency => ExecutionMode.InputLinesAdjacency,
                        InputTopology.Triangles => ExecutionMode.Triangles,
                        InputTopology.TrianglesAdjacency => ExecutionMode.InputTrianglesAdjacency,
                        _ => throw new InvalidOperationException($"Invalid input topology \"{context.Definitions.InputTopology}\"."),
                    });

                    context.AddExecutionMode(spvFunc, ExecutionMode.Invocations, (SpvLiteralInteger)context.Definitions.ThreadsPerInputPrimitive);

                    context.AddExecutionMode(spvFunc, context.Definitions.OutputTopology switch
                    {
                        OutputTopology.PointList => ExecutionMode.OutputPoints,
                        OutputTopology.LineStrip => ExecutionMode.OutputLineStrip,
                        OutputTopology.TriangleStrip => ExecutionMode.OutputTriangleStrip,
                        _ => throw new InvalidOperationException($"Invalid output topology \"{context.Definitions.OutputTopology}\"."),
                    });

                    context.AddExecutionMode(spvFunc, ExecutionMode.OutputVertices, (SpvLiteralInteger)context.Definitions.MaxOutputVertices);
                }
                else if (context.Definitions.Stage == ShaderStage.Fragment)
                {
                    context.AddExecutionMode(spvFunc, context.Definitions.OriginUpperLeft
                        ? ExecutionMode.OriginUpperLeft
                        : ExecutionMode.OriginLowerLeft);

                    if (context.Info.IoDefinitions.Contains(new IoDefinition(StorageKind.Output, IoVariable.FragmentOutputDepth)))
                    {
                        context.AddExecutionMode(spvFunc, ExecutionMode.DepthReplacing);
                    }

                    if (context.Definitions.EarlyZForce)
                    {
                        context.AddExecutionMode(spvFunc, ExecutionMode.EarlyFragmentTests);
                    }

                    if ((info.HelperFunctionsMask & HelperFunctionsMask.FSI) != 0 &&
                        context.HostCapabilities.SupportsFragmentShaderInterlock)
                    {
                        context.AddExecutionMode(spvFunc, ExecutionMode.PixelInterlockOrderedEXT);
                    }
                }
                else if (context.Definitions.Stage == ShaderStage.Compute)
                {
                    SpvLiteralInteger localSizeX = (SpvLiteralInteger)context.Definitions.ComputeLocalSizeX;
                    SpvLiteralInteger localSizeY = (SpvLiteralInteger)context.Definitions.ComputeLocalSizeY;
                    SpvLiteralInteger localSizeZ = (SpvLiteralInteger)context.Definitions.ComputeLocalSizeZ;

                    context.AddExecutionMode(
                        spvFunc,
                        ExecutionMode.LocalSize,
                        localSizeX,
                        localSizeY,
                        localSizeZ);
                }

                if (context.Definitions.TransformFeedbackEnabled && context.Definitions.LastInVertexPipeline)
                {
                    context.AddExecutionMode(spvFunc, ExecutionMode.Xfb);
                }
            }
        }

        private static void Generate(CodeGenContext context, AstBlock block)
        {
            AstBlockVisitor visitor = new(block);

            Dictionary<AstBlock, (SpvInstruction, SpvInstruction)> loopTargets = new();

            context.LoopTargets = loopTargets;

            visitor.BlockEntered += (sender, e) =>
            {
                AstBlock mergeBlock = e.Block.Parent;

                if (e.Block.Type == AstBlockType.If)
                {
                    AstBlock ifTrueBlock = e.Block;
                    AstBlock ifFalseBlock;

                    if (AstHelper.Next(e.Block) is AstBlock nextBlock && nextBlock.Type == AstBlockType.Else)
                    {
                        ifFalseBlock = nextBlock;
                    }
                    else
                    {
                        ifFalseBlock = mergeBlock;
                    }

                    SpvInstruction condition = context.Get(AggregateType.Bool, e.Block.Condition);

                    context.SelectionMerge(context.GetNextLabel(mergeBlock), SelectionControlMask.MaskNone);
                    context.BranchConditional(condition, context.GetNextLabel(ifTrueBlock), context.GetNextLabel(ifFalseBlock));
                }
                else if (e.Block.Type == AstBlockType.DoWhile)
                {
                    SpvInstruction continueTarget = context.Label();

                    loopTargets.Add(e.Block, (context.NewBlock(), continueTarget));

                    context.LoopMerge(context.GetNextLabel(mergeBlock), continueTarget, LoopControlMask.MaskNone);
                    context.Branch(context.GetFirstLabel(e.Block));
                }

                context.EnterBlock(e.Block);
            };

            visitor.BlockLeft += (sender, e) =>
            {
                if (e.Block.Parent != null)
                {
                    if (e.Block.Type == AstBlockType.DoWhile)
                    {
                        // This is a loop, we need to jump back to the loop header
                        // if the condition is true.
                        AstBlock mergeBlock = e.Block.Parent;

                        (SpvInstruction loopTarget, SpvInstruction continueTarget) = loopTargets[e.Block];

                        context.Branch(continueTarget);
                        context.AddLabel(continueTarget);

                        SpvInstruction condition = context.Get(AggregateType.Bool, e.Block.Condition);

                        context.BranchConditional(condition, loopTarget, context.GetNextLabel(mergeBlock));
                    }
                    else
                    {
                        // We only need a branch if the last instruction didn't
                        // already cause the program to exit or jump elsewhere.
                        bool lastIsCf = e.Block.Last is AstOperation lastOp &&
                            (lastOp.Inst == Instruction.Discard ||
                             lastOp.Inst == Instruction.LoopBreak ||
                             lastOp.Inst == Instruction.LoopContinue ||
                             lastOp.Inst == Instruction.Return);

                        if (!lastIsCf)
                        {
                            context.Branch(context.GetNextLabel(e.Block.Parent));
                        }
                    }

                    bool hasElse = AstHelper.Next(e.Block) is AstBlock nextBlock &&
                        (nextBlock.Type == AstBlockType.Else ||
                         nextBlock.Type == AstBlockType.ElseIf);

                    // Re-enter the parent block.
                    if (e.Block.Parent != null && !hasElse)
                    {
                        context.EnterBlock(e.Block.Parent);
                    }
                }
            };

            foreach (IAstNode node in visitor.Visit())
            {
                if (node is AstAssignment assignment)
                {
                    AstOperand dest = (AstOperand)assignment.Destination;

                    if (dest.Type == OperandType.LocalVariable)
                    {
                        SpvInstruction source = context.Get(dest.VarType, assignment.Source);
                        context.Store(context.GetLocalPointer(dest), source);
                    }
                    else if (dest.Type == OperandType.Argument)
                    {
                        SpvInstruction source = context.Get(dest.VarType, assignment.Source);
                        context.Store(context.GetArgumentPointer(dest), source);
                    }
                    else
                    {
                        throw new NotImplementedException(dest.Type.ToString());
                    }
                }
                else if (node is AstOperation operation)
                {
                    Instructions.Generate(context, operation);
                }
            }
        }
    }
}
