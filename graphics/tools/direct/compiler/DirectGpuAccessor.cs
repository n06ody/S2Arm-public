// 直连静态 IGpuAccessor：每一个方法都有显式实现与唯一事实来源；没有事实就 MissingFact，
// 不回退到接口的“返回 0 / Texture2D / true”默认答案。
// 绑定分配器：VS/TCS/TES/GS/FS 顺序、CS 独立；set0/binding0 固定 SupportBuffer，其余按翻译调用顺序
// 在四类 reserved 之后连续分配。
using Ryujinx.Graphics.GAL;
using Ryujinx.Graphics.Gpu.Shader;
using Ryujinx.Graphics.Shader;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;

namespace S2A.DirectCompiler
{
    public sealed class DirectQuery
    {
        public readonly string Method, Arguments, Value, Source;
        public DirectQuery(string method, string arguments, string value, string source)
        { Method = method; Arguments = arguments; Value = value; Source = source; }
    }

    public sealed class DirectBinding
    {
        public readonly string Stage, Kind, Source;
        public readonly int Set, Binding, Count;
        // 池槽位/句柄索引：翻译期拿不到（CreateTextureBinding 早于 RegisterTexture），
        // 编译结束后按反射到的 TextureDescriptor.HandleIndex 回填；-1 表示还没定。
        public int Slot;
        // Storage address provenance, resolved from ShaderProgram.Info.SBuffers.
        // Translator guestIndex is deliberately NOT a runtime NVN slot.
        public int CbSlot = -1, CbOffset = -1;
        public int NvnSlot = -1, UboPtr = -1, UboSlot = -1, UboOffset = -1;
        public bool Writable;
        public DirectBinding(string stage, string kind, string source, int set, int binding, int count, int slot = -1)
        { Stage = stage; Kind = kind; Source = source; Set = set; Binding = binding; Count = count; Slot = slot; }
    }

    public sealed class DirectAllocator
    {
        private readonly DirectProfile _profile;
        private readonly Dictionary<string, int> _perStage = new(StringComparer.Ordinal);
        private int _uniforms, _storage, _textures, _images;
        public readonly int ReservedConstantBuffers;
        public readonly List<DirectBinding> Bindings = new();

        public DirectAllocator(DirectProfile profile)
        {
            _profile = profile;
            // 锁版本 translator 自己的保留槽；不能复制手写数字。
            ReservedConstantBuffers = new ResourceReservationCounts(false, false).ReservedConstantBuffers;
            if (ReservedConstantBuffers != 1)
                throw new DirectCompileException("IDENTITY", "translator reserved support-buffer count changed");
            Bindings.Add(new DirectBinding("program", "support", "SupportBuffer.Binding", 0, SupportBuffer.Binding, 1));
        }

        private void Count(string stage, string kind, uint limit)
        {
            string key = stage + "/" + kind;
            _perStage.TryGetValue(key, out int count);
            if (count + 1 > limit)
                throw new DirectCompileException("RESOURCE_LIMIT", stage + " exceeds " + kind + " descriptor limit " + limit);
            _perStage[key] = count + 1;
        }

        public SetBindingPair Uniform(string stage, int guestIndex)
        {
            Count(stage, "uniform", _profile.Caps.MaximumUniformBuffersPerStage);
            int binding = ReservedConstantBuffers + _uniforms++;
            // slot 记录 guest 常量缓冲号：运行时按它把 guest cbuf 绑到对应描述符。
            Bindings.Add(new DirectBinding(stage, "uniform", "guestUBO[" + guestIndex + "]", 0, binding, 1, guestIndex));
            return new SetBindingPair(0, binding);
        }

        public SetBindingPair Storage(string stage, int guestIndex)
        {
            Count(stage, "storage", _profile.Caps.MaximumStorageBuffersPerStage);
            int binding = _storage++;
            Bindings.Add(new DirectBinding(stage, "storage", "guestSSBO[" + guestIndex + "]", 1, binding, 1));
            return new SetBindingPair(1, binding);
        }

        public SetBindingPair Texture(string stage, int count, bool isBuffer, int slot = -1)
        {
            Count(stage, "texture", _profile.Caps.MaximumTexturesPerStage);
            int binding = _textures++;
            Bindings.Add(new DirectBinding(stage, "texture", isBuffer ? "buffer-texture" : "texture", 2, binding, count, slot));
            return new SetBindingPair(2, binding);
        }

        public SetBindingPair Image(string stage, int count, bool isBuffer)
        {
            Count(stage, "image", _profile.Caps.MaximumImagesPerStage);
            int binding = _images++;
            Bindings.Add(new DirectBinding(stage, "image", isBuffer ? "buffer-image" : "image", 3, binding, count));
            return new SetBindingPair(3, binding);
        }
    }

    public sealed class DirectGpuAccessor : IGpuAccessor
    {
        private readonly DirectProfile _profile;
        private readonly DirectProgram _program;
        private readonly DirectStage _stage;
        private readonly int _stageIndex;
        private readonly DirectAllocator _allocator;
        private int _textureHandle = -1;
        public readonly List<DirectQuery> Queries;
        public readonly List<string> Diagnostics = new();
        public uint StateReadFlags, AttributeReadMask;

        public DirectGpuAccessor(DirectProfile profile, DirectProgram program, DirectStage stage,
            int stageIndex, DirectAllocator allocator, List<DirectQuery> queries)
        {
            _profile = profile; _program = program; _stage = stage; _stageIndex = stageIndex;
            _allocator = allocator; Queries = queries;
        }

        private T Record<T>(string method, string arguments, T value, string source)
        {
            Queries.Add(new DirectQuery(method, arguments, Format(value), source));
            return value;
        }

        private static string Format(object value) => value switch
        {
            null => "null",
            bool b => b ? "true" : "false",
            float f => f.ToString("R", CultureInfo.InvariantCulture),
            double d => d.ToString("R", CultureInfo.InvariantCulture),
            IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
            _ => value.ToString(),
        };

        private static DirectCompileException Missing(string method, string args) =>
            new("MISSING_FACT", method + " has no source (" + args + ")");

        private string ProfileSource => "profile:" + _profile.ProfileHash;

        // ---------- code / literal ----------
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize)
        {
            if (address >= (ulong)_stage.Code.Length)
                throw Missing("GetCode", $"address={address} size={minimumSize} codeBytes={_stage.Code.Length}");
            int remaining = _stage.Code.Length - (int)address;
            Record("GetCode", $"address={address},minimumSize={minimumSize}", remaining, _stage.CodeSource);
            return MemoryMarshal.Cast<byte, ulong>(_stage.Code.AsSpan((int)address));
        }

        public uint ConstantBuffer1Read(int offset)
        {
            if (offset < 0 || offset + 4 > _stage.C1.Length)
                throw Missing("ConstantBuffer1Read", $"offset={offset} c1Bytes={_stage.C1.Length}");
            uint value = MemoryMarshal.Read<uint>(_stage.C1.AsSpan(offset));
            return Record("ConstantBuffer1Read", "offset=" + offset, value, _stage.C1Source);
        }

        // ---------- binding allocator ----------
        public SetBindingPair CreateConstantBufferBinding(int index) =>
            Record("CreateConstantBufferBinding", "index=" + index, _allocator.Uniform(_stage.Stage, index), "binding-allocator");
        public SetBindingPair CreateStorageBufferBinding(int index) =>
            Record("CreateStorageBufferBinding", "index=" + index, _allocator.Storage(_stage.Stage, index), "binding-allocator");
        // 翻译器先建 binding、后 RegisterTexture，所以这里不能按“当前句柄”猜槽位：
        // 猜出来的值在多纹理时会全落到第一张上。槽位由 DirectShaderCompiler 按反射
        // 结果（TextureDescriptor.HandleIndex）回填，拿不到就报错，不写占位 0。
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) =>
            Record("CreateTextureBinding", $"count={count},isBuffer={(isBuffer ? 1 : 0)}",
                _allocator.Texture(_stage.Stage, count, isBuffer, -1), "binding-allocator");
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) =>
            Record("CreateImageBinding", $"count={count},isBuffer={(isBuffer ? 1 : 0)}", _allocator.Image(_stage.Stage, count, isBuffer), "binding-allocator");
        public int CreateExtraSet() => Record("CreateExtraSet", "", -1, "first-version-four-sets");

        // ---------- compute ----------
        public int QueryComputeLocalSizeX() => Record("QueryComputeLocalSizeX", "", NeedCompute("QueryComputeLocalSizeX").LocalSizeX, "computeState");
        public int QueryComputeLocalSizeY() => Record("QueryComputeLocalSizeY", "", NeedCompute("QueryComputeLocalSizeY").LocalSizeY, "computeState");
        public int QueryComputeLocalSizeZ() => Record("QueryComputeLocalSizeZ", "", NeedCompute("QueryComputeLocalSizeZ").LocalSizeZ, "computeState");
        public int QueryComputeLocalMemorySize() => Record("QueryComputeLocalMemorySize", "", NeedCompute("QueryComputeLocalMemorySize").LocalMemoryBytes, "computeState");
        public int QueryComputeSharedMemorySize() => Record("QueryComputeSharedMemorySize", "", NeedCompute("QueryComputeSharedMemorySize").SharedMemoryBytes, "computeState");
        private DirectComputeFacts NeedCompute(string method) =>
            _program.Compute ?? throw Missing(method, "program kind=" + _program.Kind);

        // ---------- state ----------
        public uint QueryConstantBufferUse() =>
            Record("QueryConstantBufferUse", "stage=" + _stage.Stage, _stage.ConstantBufferUse, _stage.CodeSource + "#cbUse");

        public GpuGraphicsState QueryGraphicsState()
        {
            DirectGraphicsFacts facts = _program.Graphics ?? throw Missing("QueryGraphicsState", "program kind=" + _program.Kind);
            Record("QueryGraphicsState", "stage=" + _stage.Stage, "graphics-state", "graphicsState");
            return facts.State;
        }

        public bool QueryHasConstantBufferDrawParameters() =>
            Record("QueryHasConstantBufferDrawParameters", "stage=" + _stage.Stage,
                NeedGraphics("QueryHasConstantBufferDrawParameters").State.HasConstantBufferDrawParameters, "graphicsState");

        public bool QueryHasUnalignedStorageBuffer() =>
            Record("QueryHasUnalignedStorageBuffer", "stage=" + _stage.Stage,
                _program.Kind == "compute" ? NeedCompute("QueryHasUnalignedStorageBuffer").HasUnalignedStorageBuffer :
                NeedGraphics("QueryHasUnalignedStorageBuffer").HasUnalignedStorageBuffer,
                _program.Kind == "compute" ? "computeState" : "graphicsState");

        private DirectGraphicsFacts NeedGraphics(string method) =>
            _program.Graphics ?? throw Missing(method, "program kind=" + _program.Kind);

        // ---------- host / effective profile ----------
        // direct-v1 尚未声明 SSBO 窗口插桩/诊断协议，显式保持原有离线翻译策略。
        // 这是编译策略，不代表设备开启了 robustBufferAccess；不借用通用渲染器的环境开关。
        // S2A 直连（Metal 宿主）专属的四项翻译开关（compat/tree IGpuAccessor，10-30 两份 Ryujinx 收成一份 b69a78a71 起才进本接口）：
        //   安卓 Vulkan 直连宿主没有这些宿主合同，显式答假 = 合并前本编译器得到的上游行为，SPIR-V 输出不变。要开哪一项是单独的决定（先有安卓宿主合同）。
        //   S2ARuntimeState 不能走缺省：缺省跟进程开关 TranslatorContext.S2ARuntimeState，同一请求在不同进程会编出不同代码；安卓直连宿主不填 s2a_flags / s2a_point
        public bool QueryGeometryInputFixed() => Record("QueryGeometryInputFixed", "", false, "direct-v1:vs-fs-only (no fixed geometry capture contract)");
        public bool QueryTexelFetch1DWrap() => Record("QueryTexelFetch1DWrap", "", false, "direct-v1:no-tbuf-as-2d host contract");
        public bool QueryBindlessPhiSeeThrough() => Record("QueryBindlessPhiSeeThrough", "", false, "direct-v1:upstream bindless elimination");
        // 0 = 回退数组一律按池长（上游原样）：安卓宿主 textureArrayLength / samplerArrayLength 就是真实描述符池容量（vk_record.c OP_SET_TEX_POOL），
        //   贴图缓冲 / 图像也按池编号取；没有 nvn_mtl.m TEXD 那种按槽绑的约定，答 32 会把编号 ≥ 32 的贴图缓冲 / 图像夹到第 31 项读错图
        public int QueryBindlessSlotArrayLength() => Record("QueryBindlessSlotArrayLength", "", 0, "direct-v1:pool-length (no per-slot texture-buffer/image binding)");
        public bool QueryS2ARuntimeState() => Record("QueryS2ARuntimeState", "", false, "direct-v1:host does not fill s2a_flags / s2a_point");
        public void RecordTexelFetch1D(int handle) => Record("RecordTexelFetch1D", "handle=" + handle, "recorded", "direct-v1:no-tbuf-as-2d host contract");
        public bool QueryHostStorageWindowClamp() => Record("QueryHostStorageWindowClamp", "", false, "direct-v1:no-storage-window-instrumentation");
        public bool QueryHostStorageOobRecord() => Record("QueryHostStorageOobRecord", "", false, "direct-v1:no-storage-oob-recorder");
        // 11-01 #967 补丁（a703ee14a）新加：Mac 直连 ABI 2 由宿主给贴图尺寸表；安卓 Vulkan 直连宿主没有这张表，同上显式答假（= 接口缺省，SPIR-V 不变）
        public bool QueryHostSupportsTextureSizeTable() => Record("QueryHostSupportsTextureSizeTable", "", false, "direct-v1:no-texture-size-table host contract");
        public SetBindingPair CreateS2AStorageOobRecordBinding(ShaderStage stage) =>
            Record("CreateS2AStorageOobRecordBinding", "stage=" + stage, new SetBindingPair(-1, -1), "direct-v1:no-storage-oob-recorder");
        public bool QueryHostRequiresLinkedVaryingInterface() => Record("QueryHostRequiresLinkedVaryingInterface", "", _profile.Caps.RequiresLinkedVaryingInterface, ProfileSource);
        public bool QueryHostSupportsSpirv15() => Record("QueryHostSupportsSpirv15", "", _profile.Caps.S2ASpirv15, ProfileSource);
        public bool QueryHostSpirvLayerExt() => Record("QueryHostSpirvLayerExt", "", _profile.Caps.S2ASpirvLayerExt, ProfileSource);
        public bool QueryHostSpirvLayerNv() => Record("QueryHostSpirvLayerNv", "", _profile.Caps.S2ASpirvLayerNv, ProfileSource);
        public int QueryHostMaximumVertexOutputComponents() => Record("QueryHostMaximumVertexOutputComponents", "", _profile.Caps.MaximumVertexOutputComponents, ProfileSource);
        public int QueryHostMaximumFragmentInputComponents() => Record("QueryHostMaximumFragmentInputComponents", "", _profile.Caps.MaximumFragmentInputComponents, ProfileSource);
        public int QueryHostGatherBiasPrecision() => Record("QueryHostGatherBiasPrecision", "", _profile.Caps.GatherBiasPrecision, ProfileSource);
        public bool QueryHostReducedPrecision() => Record("QueryHostReducedPrecision", "", _profile.Caps.ReduceShaderPrecision, ProfileSource);
        public bool QueryHostHasFrontFacingBug() => Record("QueryHostHasFrontFacingBug", "", _profile.Caps.HasFrontFacingBug, ProfileSource);
        public bool QueryHostHasVectorIndexingBug() => Record("QueryHostHasVectorIndexingBug", "", _profile.Caps.HasVectorIndexingBug, ProfileSource);
        public int QueryHostStorageBufferOffsetAlignment() => Record("QueryHostStorageBufferOffsetAlignment", "", _profile.Caps.StorageBufferOffsetAlignment, ProfileSource);
        public int QueryHostSubgroupSize() => Record("QueryHostSubgroupSize", "", _profile.Caps.ShaderSubgroupSize, ProfileSource);
        public bool QueryHostSupportsBgraFormat() => Record("QueryHostSupportsBgraFormat", "", _profile.Caps.SupportsBgraFormat, ProfileSource);
        public bool QueryHostSupportsFragmentShaderInterlock() => Record("QueryHostSupportsFragmentShaderInterlock", "", _profile.Caps.SupportsFragmentShaderInterlock, ProfileSource);
        public bool QueryHostSupportsFragmentShaderOrderingIntel() => Record("QueryHostSupportsFragmentShaderOrderingIntel", "", _profile.Caps.SupportsFragmentShaderOrderingIntel, ProfileSource);
        public bool QueryHostSupportsGeometryShader() => Record("QueryHostSupportsGeometryShader", "", _profile.Caps.SupportsGeometryShader, ProfileSource);
        public bool QueryHostSupportsGeometryShaderPassthrough() => Record("QueryHostSupportsGeometryShaderPassthrough", "", _profile.Caps.SupportsGeometryShaderPassthrough, ProfileSource);
        public bool QueryHostSupportsImageLoadFormatted() => Record("QueryHostSupportsImageLoadFormatted", "", _profile.Caps.SupportsImageLoadFormatted, ProfileSource);
        // 11-05 #1485（45bed0f5e）给 IGpuAccessor 加了这一问、接口缺省 true，直连 profile 没有对应能力（DirectDeviceCaps 未采 shaderStorageImageWriteWithoutFormat、
        //   profile JSON 严格键里也没有），合入后安卓直连实际按 true 翻译。这里显式写出同一值、记明来源，行为与合入后一致；接设备能力由 #1485 作者另做
        public bool QueryHostSupportsImageStoreFormatted() => Record("QueryHostSupportsImageStoreFormatted", "", true, "direct-v1:image-store-formatted 未接设备能力（与 #1485 合入时接口缺省一致）");
        public bool QueryHostSupportsLayerVertexTessellation() => Record("QueryHostSupportsLayerVertexTessellation", "", _profile.Caps.SupportsLayerVertexTessellation, ProfileSource);
        public bool QueryHostSupportsNonConstantTextureOffset() => Record("QueryHostSupportsNonConstantTextureOffset", "", _profile.Caps.SupportsNonConstantTextureOffset, ProfileSource);
        public bool QueryHostSupportsScaledVertexFormats() => Record("QueryHostSupportsScaledVertexFormats", "", _profile.Caps.SupportsScaledVertexFormats, ProfileSource);
        public bool QueryHostS2AVertexPull() => Record("QueryHostS2AVertexPull", "", _profile.Caps.S2AVertexPull, ProfileSource);
        public bool QueryHostSupportsSeparateSampler() => Record("QueryHostSupportsSeparateSampler", "", _profile.Caps.SupportsSeparateSampler, ProfileSource);
        public bool QueryHostSupportsShaderBallot() => Record("QueryHostSupportsShaderBallot", "", _profile.Caps.SupportsShaderBallot, ProfileSource);
        public bool QueryHostSupportsShaderBarrierDivergence() => Record("QueryHostSupportsShaderBarrierDivergence", "", _profile.Caps.SupportsShaderBarrierDivergence, ProfileSource);
        public bool QueryHostSupportsShaderFloat64() => Record("QueryHostSupportsShaderFloat64", "", _profile.Caps.SupportsShaderFloat64, ProfileSource);
        public bool QueryHostSupportsShaderNonUniformIndexing() => Record("QueryHostSupportsShaderNonUniformIndexing", "", _profile.Caps.SupportsShaderNonUniformIndexing, ProfileSource);
        public bool QueryHostSupportsSnormBufferTextureFormat() => Record("QueryHostSupportsSnormBufferTextureFormat", "", _profile.Caps.SupportsSnormBufferTextureFormat, ProfileSource);
        public bool QueryHostSupportsTextureGatherOffsets() => Record("QueryHostSupportsTextureGatherOffsets", "", _profile.Caps.SupportsTextureGatherOffsets, ProfileSource);
        public bool QueryHostSupportsTextureShadowLod() => Record("QueryHostSupportsTextureShadowLod", "", _profile.Caps.SupportsTextureShadowLod, ProfileSource);
        public bool QueryHostSupportsTransformFeedback() => Record("QueryHostSupportsTransformFeedback", "", _profile.Caps.SupportsTransformFeedback, ProfileSource);
        public bool QueryHostSupportsViewportIndexVertexTessellation() => Record("QueryHostSupportsViewportIndexVertexTessellation", "", _profile.Caps.SupportsViewportIndexVertexTessellation, ProfileSource);
        public bool QueryHostSupportsViewportMask() => Record("QueryHostSupportsViewportMask", "", _profile.Caps.SupportsViewportMask, ProfileSource);
        public bool QueryHostSupportsDepthClipControl() => Record("QueryHostSupportsDepthClipControl", "", _profile.Caps.SupportsDepthClipControl, ProfileSource);

        // ---------- textures / samplers ----------
        // 直连宿主按 NVN 组合纹理槽给事实：handle=8+2*slot、cbufSlot=-1。着色器把同一个
        // 句柄经常量缓冲表读取时，查询会带 cbufSlot>=0；handle 仍落在同一个组合槽
        // （nvn_vk_first_draw 的 handleIndex→nvn_tex_slot 表）。所以 cbuf 查询没有精确
        // 事实时，回落到同 (stage, handle) 的直接事实；没有同句柄直接事实仍然是缺事实，
        // 不猜槽位、也不跨句柄借事实。
        private DirectTextureFact FindTexture(int handle, int cbufSlot) =>
            _program.Textures.FirstOrDefault(t =>
                t.Stage == _stage.Stage && t.Handle == handle && t.CbufSlot == cbufSlot);

        private DirectTextureFact NeedTexture(string method, int handle, int cbufSlot)
        {
            DirectTextureFact fact = FindTexture(handle, cbufSlot);
            if (fact == null && cbufSlot >= 0)
                fact = FindTexture(handle, -1);
            if (fact == null && cbufSlot >= 0)
            {
                // c2's separate texture and sampler words are ORed; either may be first.
                // Only accept one word from each exact NVN table, never a guessed slot.
                (int texBuffer, int sampBuffer) = TextureHandle.UnpackSlots(cbufSlot, -1);
                (int a, int b, TextureHandleType type) = TextureHandle.UnpackOffsets(handle);
                const int textureStart = 0x168 / 4, samplerStart = 0x568 / 4, samplerEnd = 0x968 / 4;
                bool IsTextureWord(int w) => w >= textureStart && w < samplerStart && (w & 1) == 0;
                bool IsSamplerWord(int w) => w >= samplerStart && w < samplerEnd && (w & 1) == 0;
                if (texBuffer == 2 && sampBuffer == 2 && type == TextureHandleType.SeparateSamplerHandle)
                {
                    if (IsTextureWord(a) && IsSamplerWord(b)) fact = FindTexture(a, 2);
                    else if (IsSamplerWord(a) && IsTextureWord(b)) fact = FindTexture(b, 2);
                    if (fact != null && !fact.Separate) fact = null;
                }
            }
            return fact ?? throw Missing(method, $"stage={_stage.Stage},handle={handle},cbufSlot={cbufSlot}");
        }

        public bool HasDirectTextureFact(int handle) =>
            _program.Textures.Any(t => t.Stage == _stage.Stage && t.Handle == handle && t.CbufSlot == -1);

        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1) =>
            Record("QuerySamplerType", $"handle={handle},cbufSlot={cbufSlot}",
                NeedTexture("QuerySamplerType", handle, cbufSlot).SamplerType,
                "texture-fact:" + _stage.Stage + ":" + handle + ":" + cbufSlot);
        public bool QueryTextureIsBuffer(int handle, int cbufSlot = -1) =>
            Record("QueryTextureIsBuffer", $"handle={handle},cbufSlot={cbufSlot}",
                NeedTexture("QueryTextureIsBuffer", handle, cbufSlot).IsBuffer,
                "texture-fact:" + _stage.Stage + ":" + handle + ":" + cbufSlot);
        public bool QueryTextureCoordNormalized(int handle, int cbufSlot = -1) =>
            Record("QueryTextureCoordNormalized", $"handle={handle},cbufSlot={cbufSlot}",
                NeedTexture("QueryTextureCoordNormalized", handle, cbufSlot).Normalized,
                "texture-fact:" + _stage.Stage + ":" + handle + ":" + cbufSlot);
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            TextureFormat f = NeedTexture("QueryTextureFormat", handle, cbufSlot).Format;
            // 图像读写才查格式：采样专用贴图（事实里格式 Unknown）被当图像用，格式确实无来源
            if (f == TextureFormat.Unknown) throw Missing("QueryTextureFormat", $"stage={_stage.Stage},handle={handle},cbufSlot={cbufSlot},format=Unknown");
            return Record("QueryTextureFormat", $"handle={handle},cbufSlot={cbufSlot}", f, "texture-fact:" + _stage.Stage + ":" + handle + ":" + cbufSlot);
        }
        public int QueryTextureArrayLengthFromPool() =>
            Record("QueryTextureArrayLengthFromPool", "stage=" + _stage.Stage, _program.TextureArrayLength, "texture-pool-capacity");
        public int QuerySamplerArrayLengthFromPool() =>
            Record("QuerySamplerArrayLengthFromPool", "stage=" + _stage.Stage, _program.SamplerArrayLength, "sampler-pool-capacity");
        public int QueryTextureArrayLengthFromBuffer(int slot)
        {
            if (!_program.TextureBufferLengths.TryGetValue(slot, out int length))
                throw Missing("QueryTextureArrayLengthFromBuffer", "slot=" + slot);
            return Record("QueryTextureArrayLengthFromBuffer", "slot=" + slot, length, "texture-buffer-lengths:" + slot);
        }

        // ---------- transform feedback ----------
        public bool QueryTransformFeedbackEnabled() =>
            Record("QueryTransformFeedbackEnabled", "stage=" + _stage.Stage, _program.TransformFeedbackEnabled, "transformFeedbackEnabled");
        public ReadOnlySpan<byte> QueryTransformFeedbackVaryingLocations(int bufferIndex)
        {
            Record("QueryTransformFeedbackVaryingLocations", "bufferIndex=" + bufferIndex, "empty",
                "first-version-no-transform-feedback");
            return ReadOnlySpan<byte>.Empty;
        }
        public int QueryTransformFeedbackStride(int bufferIndex) =>
            Record("QueryTransformFeedbackStride", "bufferIndex=" + bufferIndex, 0, "first-version-no-transform-feedback");

        // ---------- Switch 2（Ampere / NVN2）----------
        // 只在 TranslationFlags.Ampere 的程序里调（DecoderAmpere / InstEmitAmpere）；本编译器只收 Switch 1 NVN 程序、从不开 Ampere，
        // 走到这里就是收了不支持的程序：报红，不按接口缺省静默返回 0 / null。
        public int QueryConstantBuffer1Size() => throw Missing("QueryConstantBuffer1Size", "ampere-not-supported");
        public int[] QueryAmpereComputeC0Layout() => throw Missing("QueryAmpereComputeC0Layout", "ampere-not-supported");
        public int[] QueryAmpereStageMemory() => throw Missing("QueryAmpereStageMemory", "ampere-not-supported");

        // ---------- telemetry ----------
        public void RegisterTexture(int handle, int cbufSlot)
        {
            _textureHandle = handle;
            DirectTextureFact fact = FindTexture(handle, cbufSlot);
            if (fact == null && cbufSlot >= 0)
                fact = FindTexture(handle, -1);
            Queries.Add(new DirectQuery("RegisterTexture", $"handle={handle},cbufSlot={cbufSlot}",
                fact == null ? "unbound" : "bound", fact == null ? "unbound" : "texture-fact:" + _stage.Stage + ":" + handle + ":" + cbufSlot));
        }

        public void S2AReportStateUse(uint readFlags, uint attrReadMask)
        {
            StateReadFlags |= readFlags;
            AttributeReadMask |= attrReadMask;
            Queries.Add(new DirectQuery("S2AReportStateUse", $"readFlags={readFlags},attrReadMask={attrReadMask}",
                "recorded", "translator"));
        }

        public void Log(string message)
        {
            Diagnostics.Add(message ?? "");
            Queries.Add(new DirectQuery("Log", "", message ?? "", "translator"));
        }
    }
}
