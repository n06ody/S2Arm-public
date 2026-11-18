using Ryujinx.Common;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;
using System.Globalization;

namespace Ryujinx.Graphics.Shader.Translation
{
    class ResourceManager
    {
        // Those values are used if the shader as local or shared memory access,
        // but for some reason the supplied size was 0.
        private const int DefaultLocalMemorySize = 128;
        private const int DefaultSharedMemorySize = 4096;

        private static readonly string[] _stagePrefixes = new string[] { "cp", "vp", "tcp", "tep", "gp", "fp" };

        private readonly IGpuAccessor _gpuAccessor;
        private readonly ShaderStage _stage;
        private readonly string _stagePrefix;

        private readonly SetBindingPair[] _cbSlotToBindingMap;
        private readonly SetBindingPair[] _sbSlotToBindingMap;

        private readonly Dictionary<int, int> _sbSlots;
        private readonly Dictionary<int, int> _sbSlotsReverse;

        private readonly HashSet<int> _usedConstantBufferBindings;

        private SetBindingPair _s2aOobRecord = new(-1, -1);
        private bool _s2aOobRecordQueried;

        private readonly record struct TextureInfo(int CbufSlot, int Handle, int ArrayLength, bool Separate, SamplerType Type, TextureFormat Format);

        private struct TextureMeta
        {
            public int Set;
            public int Binding;
            public bool AccurateType;
            public SamplerType Type;
            public TextureUsageFlags UsageFlags;
        }

        private readonly Dictionary<TextureInfo, TextureMeta> _usedTextures;
        private readonly Dictionary<TextureInfo, TextureMeta> _usedImages;

        public int LocalMemoryId { get; private set; }
        public int SharedMemoryId { get; private set; }

        public int LocalVertexDataMemoryId { get; private set; }
        public int LocalTopologyRemapMemoryId { get; private set; }
        public int LocalVertexIndexVertexRateMemoryId { get; private set; }
        public int LocalVertexIndexInstanceRateMemoryId { get; private set; }
        public int LocalGeometryOutputVertexCountMemoryId { get; private set; }
        public int LocalGeometryOutputIndexCountMemoryId { get; private set; }
        public int LocalGeometryPrimitiveOpenMemoryId { get; private set; }

        public ShaderProperties Properties { get; }

        public ResourceReservations Reservations { get; }

        public ResourceManager(ShaderStage stage, IGpuAccessor gpuAccessor, ResourceReservations reservations = null)
        {
            _gpuAccessor = gpuAccessor;
            Properties = new();
            Reservations = reservations;
            _stage = stage;
            _stagePrefix = GetShaderStagePrefix(stage);

            // Constant buffer banks 0-17 (Maxwell's), and 24: NVN2's push constants (c[24])
            _cbSlotToBindingMap = new SetBindingPair[25];
            // Storage buffers the global-to-storage pass found: the stage's storage buffer table and, for compute, its uniform buffer address table (NVN 16 + 16 slots)
            _sbSlotToBindingMap = new SetBindingPair[32];
            _cbSlotToBindingMap.AsSpan().Fill(new(-1, -1));
            _sbSlotToBindingMap.AsSpan().Fill(new(-1, -1));

            _sbSlots = new();
            _sbSlotsReverse = new();

            _usedConstantBufferBindings = new();

            _usedTextures = new();
            _usedImages = new();

            Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, 0, SupportBuffer.Binding, "support_buffer", SupportBuffer.GetStructureType()));

            LocalMemoryId = -1;
            SharedMemoryId = -1;
        }

        public void SetCurrentLocalMemory(int size, bool isUsed)
        {
            if (isUsed)
            {
                if (size <= 0)
                {
                    size = DefaultLocalMemorySize;
                }

                MemoryDefinition lmem = new MemoryDefinition("local_memory", AggregateType.Array | AggregateType.U32, BitUtils.DivRoundUp(size, sizeof(uint)));

                LocalMemoryId = Properties.AddLocalMemory(lmem);
            }
            else
            {
                LocalMemoryId = -1;
            }
        }

        public void SetCurrentSharedMemory(int size, bool isUsed)
        {
            if (isUsed)
            {
                if (size <= 0)
                {
                    size = DefaultSharedMemorySize;
                }

                MemoryDefinition smem = new MemoryDefinition("shared_memory", AggregateType.Array | AggregateType.U32, BitUtils.DivRoundUp(size, sizeof(uint)));

                SharedMemoryId = Properties.AddSharedMemory(smem);
            }
            else
            {
                SharedMemoryId = -1;
            }
        }

        // Reference port: the scratch of Ampere HMMA (InstEmitAmpereTensor.AHmma, helper dmsh_hmma16816_f16): a shared memory of its own, 256 words for each subgroup of the
        // workgroup, made the first time an HMMA needs it
        public int HmmaScratchMemoryId { get; private set; } = -1;

        public int GetHmmaScratchMemoryId(int subgroups)
        {
            if (HmmaScratchMemoryId < 0)
            {
                HmmaScratchMemoryId = Properties.AddSharedMemory(new MemoryDefinition("hmma_scratch", AggregateType.Array | AggregateType.U32, 256 * subgroups));
            }

            return HmmaScratchMemoryId;
        }

        public void SetVertexAsComputeLocalMemories(ShaderStage stage, InputTopology inputTopology)
        {
            LocalVertexDataMemoryId = AddMemoryDefinition("local_vertex_data", AggregateType.Array | AggregateType.FP32, Reservations.OutputSizePerInvocation);

            if (stage == ShaderStage.Vertex)
            {
                LocalVertexIndexVertexRateMemoryId = AddMemoryDefinition("local_vertex_index_vr", AggregateType.U32);
                LocalVertexIndexInstanceRateMemoryId = AddMemoryDefinition("local_vertex_index_ir", AggregateType.U32);
            }
            else if (stage == ShaderStage.Geometry)
            {
                LocalTopologyRemapMemoryId = AddMemoryDefinition("local_topology_remap", AggregateType.Array | AggregateType.U32, inputTopology.InputVertexCount);

                LocalGeometryOutputVertexCountMemoryId = AddMemoryDefinition("local_geometry_output_vertex", AggregateType.U32);
                LocalGeometryOutputIndexCountMemoryId = AddMemoryDefinition("local_geometry_output_index", AggregateType.U32);
                if (Reservations.GeometryInputFixed)
                    LocalGeometryPrimitiveOpenMemoryId = AddMemoryDefinition("local_geometry_primitive_open", AggregateType.U32);
            }
        }

        // S2Arm（11-07）：the second to fourth words of one Ampere LDG / STG .64 / .128 (InstEmitAmpereMem ALdg / AStg) → the operation of its first word, for the raw pool table
        // path (GlobalToStorage.TryGenerateRawStorageOp: a naturally aligned vector access is in one entry, so only its first word looks the entry up)
        public Dictionary<Operation, Operation> RawVectorHeads { get; } = [];

        // S2A (from the upstream reference implementation, MIT): a private word of the raw global memory range cache (GlobalToStorage.TryGenerateRawStorageOp: the entry chosen last, its range and delta; S2Arm 11-07: one cache for the whole shader), zero at the start
        public int AddRawLastEntryMemory()
        {
            return Properties.AddLocalMemory(new MemoryDefinition($"gmem_last_{Properties.LocalMemories.Count}", AggregateType.U32, 1, zeroInit: true));
        }

        private int AddMemoryDefinition(string name, AggregateType type, int arrayLength = 1)
        {
            return Properties.AddLocalMemory(new MemoryDefinition(name, type, arrayLength));
        }

        public int GetConstantBufferBinding(int slot)
        {
            SetBindingPair setAndBinding = _cbSlotToBindingMap[slot];
            if (setAndBinding.Binding < 0)
            {
                setAndBinding = _gpuAccessor.CreateConstantBufferBinding(slot);
                _cbSlotToBindingMap[slot] = setAndBinding;
                string slotNumber = slot.ToString(CultureInfo.InvariantCulture);
                AddNewConstantBuffer(setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_c{slotNumber}");
            }

            return setAndBinding.Binding;
        }

        public bool TryGetStorageBufferBinding(int sbCbSlot, int sbCbOffset, bool write, out int binding)
        {
            if (!TryGetSbSlot((byte)sbCbSlot, (ushort)sbCbOffset, out int slot))
            {
                binding = 0;
                return false;
            }

            SetBindingPair setAndBinding = _sbSlotToBindingMap[slot];

            if (setAndBinding.Binding < 0)
            {
                setAndBinding = _gpuAccessor.CreateStorageBufferBinding(slot);
                _sbSlotToBindingMap[slot] = setAndBinding;
                string slotNumber = slot.ToString(CultureInfo.InvariantCulture);
                AddNewStorageBuffer(setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_s{slotNumber}");
            }

            if (write)
            {
                Properties.SetStorageBufferWritten(setAndBinding.Binding);
            }

            binding = setAndBinding.Binding;
            return true;
        }

        /// <summary>
        /// S2A（F1）：本阶段能否有越界记录绑定。只问 GPU 访问器、不声明绑定（不给程序多加可写存储缓冲）。
        /// </summary>
        public bool S2AStorageOobRecordSupported => _gpuAccessor.CreateS2AStorageOobRecordBinding(_stage).Binding >= 0;

        /// <summary>
        /// S2A（F1）：越界记录缓冲（<see cref="S2AStorageOobRecord"/>）的绑定号；第一次调用时向 GPU 访问器申请并声明（可写、各阶段同一绑定号）。
        /// 渲染器或本阶段不支持时返回 false，调用方只做范围检查、不记录。
        /// </summary>
        public bool TryGetS2AStorageOobRecordBinding(out int binding)
        {
            if (!_s2aOobRecordQueried)
            {
                _s2aOobRecordQueried = true;
                _s2aOobRecord = _gpuAccessor.CreateS2AStorageOobRecordBinding(_stage);

                if (_s2aOobRecord.Binding >= 0)
                {
                    StructureType type = new(new[]
                    {
                        new StructureField(AggregateType.Array | AggregateType.U32, "data", 0),
                    });

                    Properties.AddOrUpdateStorageBuffer(new(
                        BufferLayout.Std430,
                        _s2aOobRecord.SetIndex,
                        _s2aOobRecord.Binding,
                        $"{_stagePrefix}_s2a_oob",
                        type,
                        BufferUsageFlags.Write | BufferUsageFlags.S2AOobRecord));
                }
            }

            binding = _s2aOobRecord.Binding;
            return binding >= 0;
        }

        private bool TryGetSbSlot(byte sbCbSlot, ushort sbCbOffset, out int slot)
        {
            int key = PackSbCbInfo(sbCbSlot, sbCbOffset);

            if (!_sbSlots.TryGetValue(key, out slot))
            {
                slot = _sbSlots.Count;

                if (slot >= _sbSlotToBindingMap.Length)
                {
                    return false;
                }

                _sbSlots.Add(key, slot);
                _sbSlotsReverse.Add(slot, key);
            }

            return true;
        }

        public bool TryGetConstantBufferSlot(int binding, out int slot)
        {
            for (slot = 0; slot < _cbSlotToBindingMap.Length; slot++)
            {
                if (_cbSlotToBindingMap[slot].Binding == binding)
                {
                    return true;
                }
            }

            slot = 0;
            return false;
        }

        public SetBindingPair GetTextureOrImageBinding(
            Instruction inst,
            SamplerType type,
            TextureFormat format,
            TextureFlags flags,
            int cbufSlot,
            int handle,
            int arrayLength = 1,
            bool separate = false)
        {
            inst &= Instruction.Mask;
            bool isImage = inst.IsImage();
            bool isWrite = inst.IsImageStore();
            bool accurateType = !inst.IsTextureQuery();
            bool intCoords = isImage || flags.HasFlag(TextureFlags.IntCoords) || inst == Instruction.TextureQuerySize;
            bool coherent = flags.HasFlag(TextureFlags.Coherent);

            if (!isImage)
            {
                format = TextureFormat.Unknown;
            }

            SetBindingPair setAndBinding = GetTextureOrImageBinding(
                cbufSlot,
                handle,
                arrayLength,
                type,
                format,
                isImage,
                intCoords,
                isWrite,
                accurateType,
                coherent,
                separate,
                inst == Instruction.ImageAtomic);

            _gpuAccessor.RegisterTexture(handle, cbufSlot);

            return setAndBinding;
        }

        private SetBindingPair GetTextureOrImageBinding(
            int cbufSlot,
            int handle,
            int arrayLength,
            SamplerType type,
            TextureFormat format,
            bool isImage,
            bool intCoords,
            bool write,
            bool accurateType,
            bool coherent,
            bool separate,
            bool isAtomic)
        {
            int dimensions = type == SamplerType.None ? 0 : type.Dimensions;
            Dictionary<TextureInfo, TextureMeta> dict = isImage ? _usedImages : _usedTextures;

            TextureUsageFlags usageFlags = TextureUsageFlags.None;

            if (intCoords)
            {
                usageFlags |= TextureUsageFlags.NeedsScaleValue;

                bool canScale = _stage.SupportsRenderScale && arrayLength == 1 && !write && dimensions == 2;

                if (!canScale)
                {
                    // Resolution scaling cannot be applied to this texture right now.
                    // Flag so that we know to blacklist scaling on related textures when binding them.
                    usageFlags |= TextureUsageFlags.ResScaleUnsupported;
                }
            }

            if (write)
            {
                usageFlags |= TextureUsageFlags.ImageStore;
            }

            if (isAtomic)
            {
                usageFlags |= TextureUsageFlags.ImageAtomic;
            }

            if (coherent)
            {
                usageFlags |= TextureUsageFlags.ImageCoherent;
            }

            // For array textures, we also want to use type as key,
            // since we may have texture handles stores in the same buffer, but for textures with different types.
            SamplerType keyType = arrayLength > 1 ? type : SamplerType.None;
            TextureInfo info = new TextureInfo(cbufSlot, handle, arrayLength, separate, keyType, format);
            TextureMeta meta = new TextureMeta()
            {
                AccurateType = accurateType,
                Type = type,
                UsageFlags = usageFlags,
            };

            int setIndex;
            int binding;

            if (dict.TryGetValue(info, out TextureMeta existingMeta))
            {
                dict[info] = MergeTextureMeta(meta, existingMeta);
                setIndex = existingMeta.Set;
                binding = existingMeta.Binding;
            }
            else
            {
                if (arrayLength > 1 && (setIndex = _gpuAccessor.CreateExtraSet()) >= 0)
                {
                    // We reserved an "extra set" for the array.
                    // In this case the binding is always the first one (0).
                    // Using separate sets for array is better as we need to do less descriptor set updates.

                    binding = 0;
                }
                else
                {
                    bool isBuffer = (type & SamplerType.Mask) == SamplerType.TextureBuffer;

                    SetBindingPair setAndBinding = isImage
                        ? _gpuAccessor.CreateImageBinding(arrayLength, isBuffer)
                        : _gpuAccessor.CreateTextureBinding(arrayLength, isBuffer);

                    setIndex = setAndBinding.SetIndex;
                    binding = setAndBinding.Binding;
                }

                meta.Set = setIndex;
                meta.Binding = binding;

                dict.Add(info, meta);
            }

            string nameSuffix;
            string prefix = isImage ? "i" : "t";

            if (arrayLength != 1 && type != SamplerType.None)
            {
                prefix += type.ShortTypeName;
            }

            if (isImage)
            {
                nameSuffix = cbufSlot < 0
                    ? $"{prefix}_tcb_{handle:X}_{format.ToGlslFormat()}"
                    : $"{prefix}_cb{cbufSlot}_{handle:X}_{format.ToGlslFormat()}";
            }
            else if (type == SamplerType.None)
            {
                nameSuffix = cbufSlot < 0 ? $"s_tcb_{handle:X}" : $"s_cb{cbufSlot}_{handle:X}";
            }
            else
            {
                nameSuffix = cbufSlot < 0 ? $"{prefix}_tcb_{handle:X}" : $"{prefix}_cb{cbufSlot}_{handle:X}";
            }

            // S2A：客体 texel buffer 的声明数值型必须与绑定视图一致（VUID-07753）。采样路径不记格式，
            // 按描述符视图查询；其余路径 format 即数值型来源（未知 / 非缓冲 = FP32，与旧行为一致）。
            AggregateType sampledType = info.Format.GetComponentType();

            if (!isImage && (type & SamplerType.Mask) == SamplerType.TextureBuffer)
            {
                sampledType = _gpuAccessor.QueryTextureFormat(handle, cbufSlot).GetComponentType();
            }
            else if (!isImage && type != SamplerType.None && arrayLength == 1)
            {
                // S2A: sampled images require the bound view's numeric type (VUID-07753).
                // An array shares one declaration across its descriptors and cannot use one element's format.
                sampledType = _gpuAccessor.QueryS2ASampledTextureFormat(handle, cbufSlot).GetComponentType();
            }

            // S2A：存储 texel buffer 的 Image Format 与数值型分开（VUID-08737）：宿主支持无格式读
            // （写时还要求无格式写）时保持 Unknown——R16ui 等扩展格式要求 StorageImageExtendedFormats
            // capability，宿主特性缺失时模块直接被拒；必须写具体格式时由 Declarations 补 capability、
            // 由设备特性门控。原子经 OpImageTexelPointer 访问、Vulkan 只收 R64/R32 具体格式（VUID-04658），
            // 累计用途里只要有原子就不得 Unknown。
            TextureFormat declaredFormat = info.Format;

            if (isImage && (type & SamplerType.Mask) == SamplerType.TextureBuffer)
            {
                TextureUsageFlags mergedFlags = dict[info].UsageFlags;
                bool formatless =
                    !mergedFlags.HasFlag(TextureUsageFlags.ImageAtomic) &&
                    _gpuAccessor.QueryHostSupportsImageLoadFormatted() &&
                    (!mergedFlags.HasFlag(TextureUsageFlags.ImageStore) ||
                        _gpuAccessor.QueryHostSupportsImageStoreFormatted());
                if (formatless)
                {
                    declaredFormat = TextureFormat.Unknown;
                }
            }
            else if (isImage && info.Format != TextureFormat.Unknown)
            {
                // S2A：非缓冲存储图像的按位读（SULD.D：格式只按读的大小定，如 B32 → R32Uint，见 InstEmitSurface.EmitSuld）。
                //   贴图本身常是别的格式（空轨 2nd 的曝光 1x1 RG32F 按 R32ui 读上一帧的位），声明 R32ui 而视图是 R32G32_SFLOAT 属未定义行为，
                //   Adreno 830 上读出的值与 Mali 不同（曝光钉在下限）。宿主支持无格式读、且这个绑定只读（无存储、无原子）时声明 Unknown，
                //   Sampled Type 仍按读的大小（uint / sint），宿主按贴图字节数绑该数值类的视图（安卓直连 rp_draw.c image_view_format）。
                //   写和原子照旧具体格式（无格式写的能力还没进 profile）。
                TextureUsageFlags mergedFlags = dict[info].UsageFlags;
                if (!mergedFlags.HasFlag(TextureUsageFlags.ImageAtomic) && !mergedFlags.HasFlag(TextureUsageFlags.ImageStore) &&
                    _gpuAccessor.QueryHostSupportsImageLoadFormattedRaw())
                {
                    declaredFormat = TextureFormat.Unknown;
                }
            }

            TextureDefinition definition = new TextureDefinition(
                setIndex,
                binding,
                arrayLength,
                separate,
                $"{_stagePrefix}_{nameSuffix}",
                meta.Type,
                declaredFormat,
                dict[info].UsageFlags,
                sampledType);

            if (isImage)
            {
                Properties.AddOrUpdateImage(definition);
            }
            else
            {
                Properties.AddOrUpdateTexture(definition);
            }

            return new SetBindingPair(setIndex, binding);
        }

        private static TextureMeta MergeTextureMeta(TextureMeta meta, TextureMeta existingMeta)
        {
            meta.Set = existingMeta.Set;
            meta.Binding = existingMeta.Binding;
            meta.UsageFlags |= existingMeta.UsageFlags;

            // If the texture we have has inaccurate type information, then
            // we prefer the most accurate one.
            if (existingMeta.AccurateType)
            {
                meta.AccurateType = true;
                meta.Type = existingMeta.Type;
            }

            return meta;
        }

        public void SetUsageFlagsForTextureQuery(SetBindingPair setAndBinding, SamplerType type)
        {
            TextureInfo selectedInfo = default;
            TextureMeta selectedMeta = default;
            bool found = false;

            foreach ((TextureInfo info, TextureMeta meta) in _usedTextures)
            {
                if (meta.Set == setAndBinding.SetIndex && meta.Binding == setAndBinding.Binding)
                {
                    selectedInfo = info;
                    selectedMeta = meta;
                    found = true;
                    break;
                }
            }

            if (found)
            {
                selectedMeta.UsageFlags |= TextureUsageFlags.NeedsScaleValue;
                
                int dimensions = type.Dimensions;
                bool canScale = _stage.SupportsRenderScale && selectedInfo.ArrayLength == 1 && dimensions == 2;

                if (!canScale)
                {
                    // Resolution scaling cannot be applied to this texture right now.
                    // Flag so that we know to blacklist scaling on related textures when binding them.
                    selectedMeta.UsageFlags |= TextureUsageFlags.ResScaleUnsupported;
                }

                _usedTextures[selectedInfo] = selectedMeta;
            }
        }

        public void SetUsedConstantBufferBinding(int binding)
        {
            _usedConstantBufferBindings.Add(binding);
        }

        public BufferDescriptor[] GetConstantBufferDescriptors()
        {
            BufferDescriptor[] descriptors = new BufferDescriptor[_usedConstantBufferBindings.Count];

            int descriptorIndex = 0;

            for (int slot = 0; slot < _cbSlotToBindingMap.Length; slot++)
            {
                SetBindingPair setAndBinding = _cbSlotToBindingMap[slot];

                if (setAndBinding.Binding >= 0 && _usedConstantBufferBindings.Contains(setAndBinding.Binding))
                {
                    descriptors[descriptorIndex++] = new BufferDescriptor(setAndBinding.SetIndex, setAndBinding.Binding, slot);
                }
            }

            if (descriptors.Length != descriptorIndex)
            {
                Array.Resize(ref descriptors, descriptorIndex);
            }

            return descriptors;
        }

        public BufferDescriptor[] GetStorageBufferDescriptors()
        {
            // S2A（F1）：越界记录缓冲排在客体存储缓冲之后，槽位记 byte.MaxValue（不是客体槽位，Gpu 层按 S2AOobRecord 标志跳过）
            bool hasOobRecord = _s2aOobRecord.Binding >= 0;
            BufferDescriptor[] descriptors = new BufferDescriptor[_sbSlots.Count + (hasOobRecord ? 1 : 0)];

            int descriptorIndex = 0;

            foreach ((int key, int slot) in _sbSlots)
            {
                SetBindingPair setAndBinding = _sbSlotToBindingMap[slot];

                if (setAndBinding.Binding >= 0)
                {
                    (int sbCbSlot, int sbCbOffset) = UnpackSbCbInfo(key);
                    BufferUsageFlags flags = Properties.StorageBuffers[setAndBinding.Binding].Flags;
                    descriptors[descriptorIndex++] = new BufferDescriptor(setAndBinding.SetIndex, setAndBinding.Binding, slot, sbCbSlot, sbCbOffset, flags);
                }
            }

            if (hasOobRecord)
            {
                descriptors[descriptorIndex++] = new BufferDescriptor(
                    _s2aOobRecord.SetIndex,
                    _s2aOobRecord.Binding,
                    byte.MaxValue,
                    0,
                    0,
                    BufferUsageFlags.Write | BufferUsageFlags.S2AOobRecord);
            }

            if (descriptors.Length != descriptorIndex)
            {
                Array.Resize(ref descriptors, descriptorIndex);
            }

            return descriptors;
        }

        public TextureDescriptor[] GetTextureDescriptors(bool includeArrays = true)
        {
            return GetDescriptors(_usedTextures, includeArrays);
        }

        public TextureDescriptor[] GetImageDescriptors(bool includeArrays = true)
        {
            return GetDescriptors(_usedImages, includeArrays);
        }

        private static TextureDescriptor[] GetDescriptors(IReadOnlyDictionary<TextureInfo, TextureMeta> usedResources, bool includeArrays)
        {
            List<TextureDescriptor> descriptors = new();

            bool hasAnyArray = false;

            foreach ((TextureInfo info, TextureMeta meta) in usedResources)
            {
                if (info.ArrayLength > 1)
                {
                    hasAnyArray = true;
                    continue;
                }

                descriptors.Add(new TextureDescriptor(
                    meta.Set,
                    meta.Binding,
                    meta.Type,
                    info.Format,
                    info.CbufSlot,
                    info.Handle,
                    info.ArrayLength,
                    info.Separate,
                    meta.UsageFlags));
            }

            if (hasAnyArray && includeArrays)
            {
                foreach ((TextureInfo info, TextureMeta meta) in usedResources)
                {
                    if (info.ArrayLength <= 1)
                    {
                        continue;
                    }

                    descriptors.Add(new TextureDescriptor(
                        meta.Set,
                        meta.Binding,
                        meta.Type,
                        info.Format,
                        info.CbufSlot,
                        info.Handle,
                        info.ArrayLength,
                        info.Separate,
                        meta.UsageFlags));
                }
            }

            return descriptors.ToArray();
        }

        public bool TryGetCbufSlotAndHandleForTexture(SetBindingPair setAndBinding, out int cbufSlot, out int handle)
        {
            foreach ((TextureInfo info, TextureMeta meta) in _usedTextures)
            {
                if (meta.Set == setAndBinding.SetIndex && meta.Binding == setAndBinding.Binding)
                {
                    cbufSlot = info.CbufSlot;
                    handle = info.Handle;

                    return true;
                }
            }

            cbufSlot = 0;
            handle = 0;
            return false;
        }

        private static int FindDescriptorIndex(TextureDescriptor[] array, SetBindingPair setAndBinding)
        {
            return Array.FindIndex(array, x => x.Set == setAndBinding.SetIndex && x.Binding == setAndBinding.Binding);
        }

        public int FindTextureDescriptorIndex(SetBindingPair setAndBinding)
        {
            return FindDescriptorIndex(GetTextureDescriptors(), setAndBinding);
        }

        public int FindImageDescriptorIndex(SetBindingPair setAndBinding)
        {
            return FindDescriptorIndex(GetImageDescriptors(), setAndBinding);
        }

        public bool IsArrayOfTexturesOrImages(SetBindingPair setAndBinding, bool isImage)
        {
            foreach ((TextureInfo info, TextureMeta meta) in isImage ? _usedImages : _usedTextures)
            {
                if (meta.Set == setAndBinding.SetIndex && meta.Binding == setAndBinding.Binding)
                {
                    return info.ArrayLength != 1;
                }
            }

            return false;
        }

        private void AddNewConstantBuffer(int setIndex, int binding, string name)
        {
            StructureType type = new(new[]
            {
                new StructureField(AggregateType.Array | AggregateType.Vector4 | AggregateType.FP32, "data", Constants.ConstantBufferSize / 16),
            });

            Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, setIndex, binding, name, type));
        }

        private void AddNewStorageBuffer(int setIndex, int binding, string name)
        {
            StructureType type = new(new[]
            {
                new StructureField(AggregateType.Array | AggregateType.U32, "data", 0),
            });

            Properties.AddOrUpdateStorageBuffer(new(BufferLayout.Std430, setIndex, binding, name, type, BufferUsageFlags.None));
        }

        public static string GetShaderStagePrefix(ShaderStage stage)
        {
            uint index = (uint)stage;

            return index >= _stagePrefixes.Length ? "invalid" : _stagePrefixes[index];
        }

        private static int PackSbCbInfo(int sbCbSlot, int sbCbOffset)
        {
            return sbCbOffset | (sbCbSlot << 16);
        }

        private static (int, int) UnpackSbCbInfo(int key)
        {
            return ((byte)(key >> 16), (ushort)key);
        }
    }
}
