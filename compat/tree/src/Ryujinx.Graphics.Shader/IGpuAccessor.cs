using Ryujinx.Graphics.Shader.CodeGen;
using System;

namespace Ryujinx.Graphics.Shader
{
    /// <summary>
    /// GPU state access interface.
    /// </summary>
    /// <summary>S2A：翻译读过的特化状态位（S2AReportStateUse 的 readFlags；特化状态序列化按位存，只增不改位号）</summary>
    public static class S2AStateRead
    {
        public const uint A2c = 1, Depth = 2, Cbdp = 4, Alpha = 8, Tess = 16;   // Tess：曲面域 / 分区 / 绕序（TCS / TES 翻译读）
        // 10-21（画面·着色器线 spec）：翻译读了才比的其余状态——早期深度 / 输入拓扑 / 「四边形折半图元号」/ 点大小（ShaderDefinitions 取值即记），
        // 计算着色器的局部 / 共享内存大小（真用到才记，TranslatorContext.Translate）。包内条目首次命中时按这些位（连同 Tess）逐字段核对运行时真值（ShaderSpecializationState.S2AVerify）
        public const uint EarlyZ = 32, Topology = 64, HalvePrim = 128, PointSize = 256, LocalMem = 512, SharedMem = 1024;
    }

    public interface IGpuAccessor : ILogger
    {
        bool QueryHostSupportsSpirv15() => false;
        bool QueryHostSpirvLayerExt() => false;
        bool QueryHostSpirvLayerNv() => false;
        // Direct-render ABI opt-in. Generic producers retain their existing helpers.
        bool QueryHostSupportsTextureSizeTable() => false;

        /// <summary>S2A（直连离线几何）：顶点阶段输出按固定捕获布局给几何当计算跑（见 ResourceReservations.FillGeometryInputMap）。只有直连翻译器答真，通用模式缺省假。</summary>
        bool QueryGeometryInputFixed() => false;

        /// <summary>
        /// S2A（直连）：1D 整数坐标取样（TLD / TLDS 1D）在类型问不出是贴图缓冲时，按 (x &amp; 0x3FFF, x &gt;&gt; 14) 折成 2D 读，宿主把这一槽的贴图缓冲按 16384 一行的线性 2D 贴图绑（nvn_mtl.m tbuf_as_2d）。
        /// 依赖宿主这一绑定合同，所以只有直连翻译器答真；通用模式缺省假，输出与上游一致。
        /// </summary>
        bool QueryTexelFetch1DWrap() => false;

        /// <summary>
        /// S2A（直连）：BindlessElimination 认句柄时穿过嵌套 phi（循环变量里存的句柄），并把「或上同一常量采样器」当作同一句柄；
        /// 合成句柄的两个字按 c[2] 分离采样器表（0x568 起）认出哪个是采样器（编译器两种先后都出）；守卫下拼成的句柄（字 W 与 W | 采样器字经 phi 汇合）取拼好的那个。
        /// 只有直连翻译器答真（直连出库已长期用它）；通用模式缺省假，输出与上游一致。
        /// </summary>
        bool QueryBindlessPhiSeeThrough() => false;

        /// <summary>
        /// S2A：特化状态运行时化（点大小、DepthMode、alpha 测试、绘制参数来源、贴图坐标归一化改读支持缓冲 s2a_flags / s2a_point）。缺省 = 进程开关
        /// TranslatorContext.S2ARuntimeState（通用模式，S2A_RUNTIME_STATE=0 关）；直连翻译器答假：直连宿主不填这几个字段，编成运行时判断只会多读多分支。
        /// 按次翻译问，不用进程静态量：玩家包里直连兜底现编（S2A.Dsh）与通用模式在同一进程
        /// </summary>
        bool QueryS2ARuntimeState() => Translation.TranslatorContext.S2ARuntimeState;

        /// <summary>实际宿主要求按配对阶段建立紧凑接口；不是 TargetApi 的别名。</summary>
        bool QueryHostRequiresLinkedVaryingInterface() => false;

        /// <summary>linked 接口的真实顶点输出上限；未采用该合同的宿主返回 0。</summary>
        int QueryHostMaximumVertexOutputComponents() => 0;

        /// <summary>linked 接口的真实片元输入上限；未采用该合同的宿主返回 0。</summary>
        int QueryHostMaximumFragmentInputComponents() => 0;

        /// <summary>
        /// Reads data from the constant buffer 1.
        /// </summary>
        /// <param name="offset">Offset in bytes to read from</param>
        /// <returns>Value at the given offset</returns>
        uint ConstantBuffer1Read(int offset)
        {
            return 0;
        }

        /// <summary>
        /// Size in bytes of the constant buffer 1 data the host knows (an Ampere jump table ends there at the latest).
        /// </summary>
        int QueryConstantBuffer1Size()
        {
            return 0;
        }

        /// <summary>
        /// NVN2 (Switch 2) compute shaders: the counts of the shader's own driver constant layout in c[0] (uniform buffer entries, storage buffer entries, texture
        /// handles), its size and the bytes of its push constants, from the control section; null for other shaders.
        /// </summary>
        int[] QueryAmpereComputeC0Layout()
        {
            return null;
        }

        /// <summary>
        /// NVN2 (Switch 2) task and mesh shaders, translated as compute: the byte offsets in shared memory of the stage memory the build reads back (dmsh_meshstage.py): the
        /// workgroup's number, the stage input (a task's payload), the outputs (a payload, or a mesh shader's attributes) and a mesh shader's primitive bytes; null for other shaders.
        /// </summary>
        int[] QueryAmpereStageMemory()
        {
            return null;
        }

        /// <summary>
        /// Gets a span of the specified memory location, containing shader code.
        /// </summary>
        /// <param name="address">GPU virtual address of the data</param>
        /// <param name="minimumSize">Minimum size that the returned span may have</param>
        /// <returns>Span of the memory location</returns>
        ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize);

        /// <summary>
        /// Gets the binding number of a constant buffer.
        /// </summary>
        /// <param name="index">Constant buffer index</param>
        /// <returns>Binding number</returns>
        SetBindingPair CreateConstantBufferBinding(int index);

        /// <summary>
        /// Gets the binding number of an image.
        /// </summary>
        /// <param name="count">For array of images, the number of elements of the array, otherwise it should be 1</param>
        /// <param name="isBuffer">Indicates if the image is a buffer image</param>
        /// <returns>Binding number</returns>
        SetBindingPair CreateImageBinding(int count, bool isBuffer);

        /// <summary>
        /// Gets the binding number of a storage buffer.
        /// </summary>
        /// <param name="index">Storage buffer index</param>
        /// <returns>Binding number</returns>
        SetBindingPair CreateStorageBufferBinding(int index);

        /// <summary>
        /// S2A（F1）：存储缓冲访存是否按宿主绑定窗口（OpArrayLength）检查并钳位下标。只有未启用 robustBufferAccess 的 Vulkan
        /// 渲染器为真；为假时 GlobalToStorage 的输出与改前逐字节相同。
        /// </summary>
        bool QueryHostStorageWindowClamp()
        {
            return false;
        }

        /// <summary>
        /// S2A（F1）：越界记录诊断是否开启（缺省关，开启时必有钳位）。开启但某阶段拿不到记录绑定时，翻译期报黄。
        /// </summary>
        bool QueryHostStorageOobRecord()
        {
            return false;
        }

        /// <summary>
        /// S2A（F1）：越界记录缓冲（<see cref="S2AStorageOobRecord"/>）的绑定；记录诊断关、或该阶段不允许着色器写存储缓冲时为 (-1, -1)，
        /// 此时越界访存仍钳位，只是不记录。
        /// </summary>
        /// <param name="stage">翻译中的阶段</param>
        /// <returns>集合与绑定号</returns>
        SetBindingPair CreateS2AStorageOobRecordBinding(ShaderStage stage)
        {
            return new SetBindingPair(-1, -1);
        }

        /// <summary>
        /// Gets the binding number of a texture.
        /// </summary>
        /// <param name="count">For array of textures, the number of elements of the array, otherwise it should be 1</param>
        /// <param name="isBuffer">Indicates if the texture is a buffer texture</param>
        /// <returns>Binding number</returns>
        SetBindingPair CreateTextureBinding(int count, bool isBuffer);

        /// <summary>
        /// Gets the set index for an additional set, or -1 if there's no extra set available.
        /// </summary>
        /// <returns>Extra set index, or -1 if not available</returns>
        int CreateExtraSet()
        {
            return -1;
        }

        /// <summary>
        /// Queries Local Size X for compute shaders.
        /// </summary>
        /// <returns>Local Size X</returns>
        int QueryComputeLocalSizeX()
        {
            return 1;
        }

        /// <summary>
        /// Queries Local Size Y for compute shaders.
        /// </summary>
        /// <returns>Local Size Y</returns>
        int QueryComputeLocalSizeY()
        {
            return 1;
        }

        /// <summary>
        /// Queries Local Size Z for compute shaders.
        /// </summary>
        /// <returns>Local Size Z</returns>
        int QueryComputeLocalSizeZ()
        {
            return 1;
        }

        /// <summary>
        /// Queries Local Memory size in bytes for compute shaders.
        /// </summary>
        /// <returns>Local Memory size in bytes</returns>
        int QueryComputeLocalMemorySize()
        {
            return 0x1000;
        }

        /// <summary>
        /// Queries Shared Memory size in bytes for compute shaders.
        /// </summary>
        /// <returns>Shared Memory size in bytes</returns>
        int QueryComputeSharedMemorySize()
        {
            return 0xc000;
        }

        /// <summary>
        /// Queries Constant Buffer usage information.
        /// </summary>
        /// <returns>A mask where each bit set indicates a bound constant buffer</returns>
        uint QueryConstantBufferUse()
        {
            return 0;
        }

        /// <summary>
        /// Queries specialized GPU graphics state that the shader depends on.
        /// </summary>
        /// <returns>GPU graphics state</returns>
        GpuGraphicsState QueryGraphicsState()
        {
            return new GpuGraphicsState(
                false,
                InputTopology.Points,
                false,
                TessPatchType.Triangles,
                TessSpacing.EqualSpacing,
                false,
                false,
                false,
                false,
                false,
                1f,
                AlphaTestOp.Always,
                0f,
                default,
                true,
                default,
                false,
                false,
                false,
                false);
        }

        /// <summary>
        /// Queries whenever the current draw has written the base vertex and base instance into Constant Buffer 0.
        /// </summary>
        /// <returns>True if the shader translator can assume that the constant buffer contains the base IDs, false otherwise</returns>
        bool QueryHasConstantBufferDrawParameters()
        {
            return false;
        }

        /// <summary>
        /// Queries whenever the current draw uses unaligned storage buffer addresses.
        /// </summary>
        /// <returns>True if any storage buffer address is not aligned to 16 bytes, false otherwise</returns>
        bool QueryHasUnalignedStorageBuffer()
        {
            return false;
        }

        /// <summary>
        /// Queries host's gather operation precision bits for biasing their coordinates. Zero means no bias.
        /// </summary>
        /// <returns>Bits of gather operation precision to use for coordinate bias</returns>
        int QueryHostGatherBiasPrecision()
        {
            return 0;
        }

        /// <summary>
        /// Queries host about whether to reduce precision to improve performance.
        /// </summary>
        /// <returns>True if precision is limited to vertex position, false otherwise</returns>
        bool QueryHostReducedPrecision()
        {
            return false;
        }

        /// <summary>
        /// Queries host about the presence of the FrontFacing built-in variable bug.
        /// </summary>
        /// <returns>True if the bug is present on the host device used, false otherwise</returns>
        bool QueryHostHasFrontFacingBug()
        {
            return false;
        }

        /// <summary>
        /// Queries host about the presence of the vector indexing bug.
        /// </summary>
        /// <returns>True if the bug is present on the host device used, false otherwise</returns>
        bool QueryHostHasVectorIndexingBug()
        {
            return false;
        }

        /// <summary>
        /// Queries host storage buffer alignment required.
        /// </summary>
        /// <returns>Host storage buffer alignment in bytes</returns>
        int QueryHostStorageBufferOffsetAlignment()
        {
            return 16;
        }

        /// <summary>
        /// Queries host shader subgroup size.
        /// </summary>
        /// <returns>Host shader subgroup size in invocations</returns>
        int QueryHostSubgroupSize()
        {
            return 32;
        }

        /// <summary>
        /// Queries host support for texture formats with BGRA component order (such as BGRA8).
        /// </summary>
        /// <returns>True if BGRA formats are supported, false otherwise</returns>
        bool QueryHostSupportsBgraFormat()
        {
            return true;
        }

        /// <summary>
        /// Queries host support for fragment shader ordering critical sections on the shader code.
        /// </summary>
        /// <returns>True if fragment shader interlock is supported, false otherwise</returns>
        bool QueryHostSupportsFragmentShaderInterlock()
        {
            return true;
        }

        /// <summary>
        /// Queries host support for fragment shader ordering scoped critical sections on the shader code.
        /// </summary>
        /// <returns>True if fragment shader ordering is supported, false otherwise</returns>
        bool QueryHostSupportsFragmentShaderOrderingIntel()
        {
            return false;
        }

        /// <summary>
        /// Queries host GPU geometry shader support.
        /// </summary>
        /// <returns>True if the GPU and driver supports geometry shaders, false otherwise</returns>
        bool QueryHostSupportsGeometryShader()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU geometry shader passthrough support.
        /// </summary>
        /// <returns>True if the GPU and driver supports geometry shader passthrough, false otherwise</returns>
        bool QueryHostSupportsGeometryShaderPassthrough()
        {
            return true;
        }

        /// <summary>
        /// Queries host support for readable images without a explicit format declaration on the shader.
        /// </summary>
        /// <returns>True if formatted image load is supported, false otherwise</returns>
        bool QueryHostSupportsImageLoadFormatted()
        {
            return true;
        }

        /// <summary>
        /// S2A: Queries whether raw loads (SULD.D) from non-buffer storage images may be declared without a format,
        /// keeping the integer sampled type of the load size. The host then binds a view of the texture's byte size in
        /// that numeric class (an RG32F texture read as raw bits gets an RG32_UINT view), so the declaration matches the view.
        /// Off by default: a host that binds the texture's own format view keeps the explicit size format.
        /// </summary>
        /// <returns>True if raw image loads may be declared without a format, false otherwise</returns>
        bool QueryHostSupportsImageLoadFormattedRaw()
        {
            return false;
        }

        /// <summary>
        /// Queries host support for writable images without a explicit format declaration on the shader.
        /// </summary>
        /// <returns>True if formatless image store is supported, false otherwise</returns>
        bool QueryHostSupportsImageStoreFormatted()
        {
            return true;
        }

        /// <summary>
        /// Queries host support for writes to the layer from vertex or tessellation shader stages.
        /// </summary>
        /// <returns>True if writes to the layer from vertex or tessellation are supported, false otherwise</returns>
        bool QueryHostSupportsLayerVertexTessellation()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU non-constant texture offset support.
        /// </summary>
        /// <returns>True if the GPU and driver supports non-constant texture offsets, false otherwise</returns>
        bool QueryHostSupportsNonConstantTextureOffset()
        {
            return true;
        }

        /// <summary>
        /// Queries host support scaled vertex formats, where a integer value is converted to floating-point.
        /// </summary>
        /// <returns>True if the host support scaled vertex formats, false otherwise</returns>
        bool QueryHostSupportsScaledVertexFormats()
        {
            return true;
        }

        /// <summary>
        /// S2A：宿主渲染器顶点自取（顶点着色器自己按格式解码属性，产出 Maxwell 寄存器原始位）。为真时顶点输入可声明为原始位，与属性类型无关
        /// </summary>
        bool QueryHostS2AVertexPull()
        {
            return false;
        }

        /// <summary>
        /// Queries host API support for separate textures and samplers.
        /// </summary>
        /// <returns>True if the API supports samplers and textures to be combined on the shader, false otherwise</returns>
        bool QueryHostSupportsSeparateSampler()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU shader ballot support.
        /// </summary>
        /// <returns>True if the GPU and driver supports shader ballot, false otherwise</returns>
        bool QueryHostSupportsShaderBallot()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU shader support for barrier instructions on divergent control flow paths.
        /// </summary>
        /// <returns>True if the GPU supports barriers on divergent control flow paths, false otherwise</returns>
        bool QueryHostSupportsShaderBarrierDivergence()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU support for 64-bit floating point (double precision) operations on the shader.
        /// </summary>
        /// <returns>True if the GPU and driver supports double operations, false otherwise</returns>
        bool QueryHostSupportsShaderFloat64()
        {
            return true;
        }
        bool QueryHostSupportsShaderNonUniformIndexing()
        {
            return false;
        }

        /// <summary>
        /// Queries host GPU support for signed normalized buffer texture formats.
        /// </summary>
        /// <returns>True if the GPU and driver supports the formats, false otherwise</returns>
        bool QueryHostSupportsSnormBufferTextureFormat()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU texture gather with multiple offsets support.
        /// </summary>
        /// <returns>True if the GPU and driver supports texture gather offsets, false otherwise</returns>
        bool QueryHostSupportsTextureGatherOffsets()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU texture shadow LOD support.
        /// </summary>
        /// <returns>True if the GPU and driver supports texture shadow LOD, false otherwise</returns>
        bool QueryHostSupportsTextureShadowLod()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU transform feedback support.
        /// </summary>
        /// <returns>True if the GPU and driver supports transform feedback, false otherwise</returns>
        bool QueryHostSupportsTransformFeedback()
        {
            return true;
        }

        /// <summary>
        /// Queries host support for writes to the viewport index from vertex or tessellation shader stages.
        /// </summary>
        /// <returns>True if writes to the viewport index from vertex or tessellation are supported, false otherwise</returns>
        bool QueryHostSupportsViewportIndexVertexTessellation()
        {
            return true;
        }

        /// <summary>
        /// Queries host GPU shader viewport mask output support.
        /// </summary>
        /// <returns>True if the GPU and driver supports shader viewport mask output, false otherwise</returns>
        bool QueryHostSupportsViewportMask()
        {
            return true;
        }

        /// <summary>
        /// Queries whether the host supports depth clip control.
        /// </summary>
        /// <returns>True if the GPU and driver supports depth clip control, false otherwise</returns>
        bool QueryHostSupportsDepthClipControl()
        {
            return true;
        }

        /// <summary>
        /// Gets the maximum number of samplers that the bound texture pool may have.
        /// </summary>
        /// <returns>Maximum amount of samplers that the pool may have</returns>
        int QuerySamplerArrayLengthFromPool();

        /// <summary>
        /// Queries sampler type information.
        /// </summary>
        /// <param name="handle">Texture handle</param>
        /// <param name="cbufSlot">Constant buffer slot for the texture handle</param>
        /// <returns>The sampler type value for the given handle</returns>
        SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            return SamplerType.Texture2D;
        }

        /// <summary>
        /// Reference port: a 1D integer-coordinate fetch (TLD / TLDS 1D) of the bound texture handle was translated as a 2D read at (x &amp; 0x3FFF, x &gt;&gt; 14),
        /// because the texture type query did not say it is a texture buffer.
        /// </summary>
        /// <param name="handle">Texture handle</param>
        void RecordTexelFetch1D(int handle)
        {
        }

        /// <summary>
        /// S2A：只问贴图是不是贴图缓冲（TEX / TLDS 一维时区分「一维贴图」与「贴图缓冲」，翻出的代码只取决于这一位）。
        /// 与 <see cref="QuerySamplerType"/> 分开记，匹配时只比「是不是缓冲」——出包离线烘焙按缺省类型翻，运行时别的非缓冲类型不该被拒（10-22 暗黑2 27 个「贴图类型」误拒）
        /// </summary>
        bool QueryTextureIsBuffer(int handle, int cbufSlot = -1)
        {
            return QuerySamplerType(handle, cbufSlot) == SamplerType.TextureBuffer;
        }

        /// <summary>
        /// Gets the size in bytes of a bound constant buffer for the current shader stage.
        /// </summary>
        /// <param name="slot">The number of the constant buffer to get the size from</param>
        /// <returns>Size in bytes</returns>
        int QueryTextureArrayLengthFromBuffer(int slot);

        /// <summary>
        /// Gets the maximum number of textures that the bound texture pool may have.
        /// </summary>
        /// <returns>Maximum amount of textures that the pool may have</returns>
        int QueryTextureArrayLengthFromPool();

        /// <summary>
        /// Queries texture coordinate normalization information.
        /// </summary>
        /// <param name="handle">Texture handle</param>
        /// <param name="cbufSlot">Constant buffer slot for the texture handle</param>
        /// <returns>True if the coordinates are normalized, false otherwise</returns>
        bool QueryTextureCoordNormalized(int handle, int cbufSlot = -1)
        {
            return true;
        }

        /// <summary>
        /// Queries texture format information, for shaders using image load or store.
        /// </summary>
        /// <remarks>
        /// This only returns non-compressed color formats.
        /// If the format of the texture is a compressed, depth or unsupported format, then a default value is returned.
        /// </remarks>
        /// <param name="handle">Texture handle</param>
        /// <param name="cbufSlot">Constant buffer slot for the texture handle</param>
        /// <returns>Color format of the non-compressed texture</returns>
        TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            return TextureFormat.R8G8B8A8Unorm;
        }

        /// <summary>S2A: bound sampled-view format for numeric type selection; Unknown preserves the upstream float declaration.</summary>
        TextureFormat QueryS2ASampledTextureFormat(int handle, int cbufSlot = -1)
        {
            return TextureFormat.Unknown;
        }

        /// <summary>
        /// Queries transform feedback enable state.
        /// </summary>
        /// <returns>True if the shader uses transform feedback, false otherwise</returns>
        bool QueryTransformFeedbackEnabled()
        {
            return false;
        }

        /// <summary>
        /// Queries the varying locations that should be written to the transform feedback buffer.
        /// </summary>
        /// <param name="bufferIndex">Index of the transform feedback buffer</param>
        /// <returns>Varying locations for the specified buffer</returns>
        ReadOnlySpan<byte> QueryTransformFeedbackVaryingLocations(int bufferIndex)
        {
            return ReadOnlySpan<byte>.Empty;
        }

        /// <summary>
        /// Queries the stride (in bytes) of the per vertex data written into the transform feedback buffer.
        /// </summary>
        /// <param name="bufferIndex">Index of the transform feedback buffer</param>
        /// <returns>Stride for the specified buffer</returns>
        int QueryTransformFeedbackStride(int bufferIndex)
        {
            return 0;
        }

        /// <summary>
        /// Registers a texture used by the shader.
        /// </summary>
        /// <param name="handle">Texture handle word offset</param>
        /// <param name="cbufSlot">Constant buffer slot where the texture handle is located</param>
        void RegisterTexture(int handle, int cbufSlot)
        {
            // Only useful when recording information for a disk shader cache.
        }
    
        /// <summary>S2A：翻译结束时报这次实际读过的特化状态（a2c 抖动、读过类型的顶点属性位置掩码）；缺省不记（匹配退回原版全比）</summary>
        void S2AReportStateUse(uint readFlags, uint attrReadMask) { }   // readFlags：S2AStateRead 位
    }
}
