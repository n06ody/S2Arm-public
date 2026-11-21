// enabled profile 合成（唯一生产者）：入参是 native 实测的 s2a-device-caps-v1，
// 出参是完整 s2a-direct-profile-v1。compiler 程序集才能算出的三样（SupportLayout.Compile().Hash、
// CompilerAbi、profileHash）在这里产生；设备侧只提供真实 caps。不新增产物格式。
//
// profileHash 的定义：对"去掉 profileHash 字段的规范化 JSON 字节"做 SHA-256，写成 sha256-<64hex>。
// 规范化字节由本文件的 WriteBody 决定（字段顺序固定、无空白）；离线出包与设备现场用同一函数，
// 因此同 caps + 同 closure 得到同一 profileHash。设备不支持的能力一律写 false（enabled，不是 supported）。
using Ryujinx.Graphics.GAL;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;

namespace S2A.DirectCompiler
{
    public static class DirectProfileComposer
    {
        public const int MaxProfileBytes = 64 << 10;
        // 与 graphics.c 的 closure_ok 同字符集/长度，避免 profile 里的 closure 与描述符里的不是同一个身份。
        private const string ClosureCharset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-+:";

        // 首版执行层的固定 descriptor 上限（与后端的 pipeline layout 同源）。
        private const uint MaxUniformBuffersPerStage = 18;
        private const uint MaxStorageBuffersPerStage = 16;
        private const uint MaxTexturesPerStage = 64;
        private const uint MaxImagesPerStage = 16;

        private const uint QualcommVendorId = 0x5143;
        private const uint IntelVendorId = 0x8086;

        public static byte[] Compose(byte[] capsUtf8, string translatorClosure)
        {
            if (capsUtf8 == null || capsUtf8.Length == 0)
                throw new DirectCompileException("INVALID_JSON", "device caps is empty");
            if (!ClosureOk(translatorClosure))
                throw new DirectCompileException("UNSUPPORTED_PROFILE",
                    "translatorClosure is empty, too long or has characters the package descriptor would reject");

            DirectDeviceCaps caps;
            using (JsonDocument document = JsonStrict.Parse(capsUtf8, DirectDeviceCaps.MaxBytes))
                caps = DirectDeviceCaps.Parse(document.RootElement, "caps");

            Capabilities capabilities = Map(caps);
            SupportLayout layout = SupportLayout.Compile();

            byte[] body = WriteBody(translatorClosure, layout, capabilities);
            string hash = "sha256-" + Convert.ToHexStringLower(Sha256.HashData(body));
            byte[] profile = InsertHash(body, hash);

            // 自检：请求消费者用的就是 DirectProfile.Parse。合成出自己都不接受的东西等于没接通。
            using (JsonDocument check = JsonStrict.Parse(profile, MaxProfileBytes))
                DirectProfile.Parse(check.RootElement, "profile");
            return profile;
        }

        public static bool ClosureOk(string closure)
        {
            if (string.IsNullOrEmpty(closure) || closure.Length >= 128) return false;
            foreach (char c in closure)
                if (ClosureCharset.IndexOf(c) < 0) return false;
            return true;
        }

        // 设备事实 → 后端 enabled 语义。这里只放"这个后端真的会这样做"的开关：
        // 硬件支持、但首版执行层没实现的阶段/特性写 false，并由 DirectProfile.Parse 兜底拒绝。
        private static Capabilities Map(DirectDeviceCaps c)
        {
            bool qualcomm = c.VendorId == QualcommVendorId;
            uint maxTextures = Math.Min(c.MaxPerStageDescriptorSampledImages, MaxTexturesPerStage);
            uint maxImages = Math.Min(c.MaxPerStageDescriptorStorageImages, MaxImagesPerStage);
            uint maxUniforms = Math.Min(c.MaxPerStageDescriptorUniformBuffers, MaxUniformBuffersPerStage);
            uint maxStorage = Math.Min(c.MaxPerStageDescriptorStorageBuffers, MaxStorageBuffersPerStage);
            if (maxTextures == 0 || maxUniforms == 0)
                throw new DirectCompileException("UNSUPPORTED_PROFILE",
                    "device limits cannot hold the direct descriptor layout (sampled images/uniform buffers)");
            // 池数组的非一致索引：生成器一旦开就同时声明采样 + 存储两种能力（Instructions.MarkNonUniform），
            // 所以要求设备真的启用了描述符索引、且 runtime / partiallyBound / 采样非一致 / 存储非一致四位都为真；
            // 缺一位保持 false（现状），由编译器照旧具名拒绝。
            bool nonUniformIndexing = c.DescriptorIndexingEnabled && c.DescriptorIndexingRuntimeArray &&
                                      c.DescriptorIndexingPartiallyBound && c.DescriptorIndexingSampledNonUniform &&
                                      c.DescriptorIndexingStorageNonUniform;

            // Missing legacy facts map to unknown (0), never to invented hardware limits.
            uint vertexOutput = c.MaxVertexOutputComponents ?? 0;
            uint fragmentInput = c.MaxFragmentInputComponents ?? 0;
            if (vertexOutput > int.MaxValue || fragmentInput > int.MaxValue)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", "varying component limits exceed translator integer range");

            return new Capabilities(
                api: TargetApi.Vulkan,
                vendorName: c.DeviceName,
                memoryType: c.DeviceLocalHostVisible ? SystemMemoryType.UnifiedMemory : SystemMemoryType.DedicatedMemory,
                hasFrontFacingBug: false,
                hasVectorIndexingBug: qualcomm,
                needsFragmentOutputSpecialization: false,
                reduceShaderPrecision: false,
                supportsAstcCompression: c.TextureCompressionAstcLdr && c.FormatsAstc,
                supportsBc123Compression: c.TextureCompressionBc && c.FormatsBc123,
                supportsBc45Compression: c.TextureCompressionBc && c.FormatsBc45,
                supportsBc67Compression: c.TextureCompressionBc && c.FormatsBc67,
                supportsEtc2Compression: c.TextureCompressionEtc2 && c.FormatsEtc2,
                // 首版执行层只建 2D image；3D 压缩纹理的等价实现另做，这里不声明。
                supports3DTextureCompression: false,
                supportsBgraFormat: true,
                supportsR4G4Format: false,
                supportsR4G4B4A4Format: c.FormatsR4G4B4A4,
                supportsScaledVertexFormats: c.FormatsScaledVertex,
                // 首版不接 texel buffer，SNORM 缓冲纹理格式不声明。
                supportsSnormBufferTextureFormat: false,
                supports5BitComponentFormat: c.FormatsFiveBit,
                supportsSparseBuffer: false,
                // 需要 VK_EXT_blend_operation_advanced；本后端不启用。
                supportsBlendEquationAdvanced: false,
                // 只有 native 真的 enable 了该扩展才算 enabled。
                supportsFragmentShaderInterlock: c.HasExtension("VK_EXT_fragment_shader_interlock"),
                supportsFragmentShaderOrderingIntel: false,
                supportsGeometryShader: c.GeometryShader,
                supportsGeometryShaderPassthrough: false,
                // #1456：capprobe 能力位随 caps 透传——首版执行层 GS 仍关，位只记录
                // 设备事实；启用 GS 时由它决定是否安全。
                supportsGeometryShaderOcclusionQuery: c.GeometryShaderOcclusionQuery,
                supportsTransformFeedback: false,
                supportsImageLoadFormatted: c.ShaderStorageImageReadWithoutFormat,
                supportsLayerVertexTessellation: false,
                supportsMismatchingViewFormat: true,
                supportsCubemapView: true,
                supportsNonConstantTextureOffset: false,
                supportsQuads: false,
                supportsSeparateSampler: true,
                supportsShaderBallot: false,
                supportsShaderBarrierDivergence: c.VendorId != IntelVendorId,
                supportsShaderFloat64: false,
                supportsShaderNonUniformIndexing: nonUniformIndexing,
                supportsTextureGatherOffsets: c.ShaderImageGatherExtended,
                supportsTextureShadowLod: false,
                supportsVertexStoreAndAtomics: c.VertexPipelineStoresAndAtomics,
                supportsViewportIndexVertexTessellation: false,
                supportsViewportMask: false,
                supportsViewportSwizzle: false,
                supportsIndirectParameters: false,
                supportsDepthClipControl: false,
                uniformBufferSetIndex: 0,
                storageBufferSetIndex: 1,
                textureSetIndex: 2,
                imageSetIndex: 3,
                extraSetBaseIndex: 4,
                maximumExtraSets: 0,
                maximumUniformBuffersPerStage: maxUniforms,
                maximumStorageBuffersPerStage: maxStorage,
                maximumTexturesPerStage: maxTextures,
                maximumImagesPerStage: maxImages,
                maximumComputeSharedMemorySize: (int)Math.Min(c.MaxComputeSharedMemorySize, 1u << 20),
                // Vulkan 的 maxSamplerAnisotropy 是 float；写成整数让规范化字节跨端稳定。
                maximumSupportedAnisotropy: c.SamplerAnisotropy ? (float)Math.Round(c.MaxSamplerAnisotropy) : 0f,
                shaderSubgroupSize: c.SubgroupSize,
                storageBufferOffsetAlignment: (int)Math.Min(c.MinStorageBufferOffsetAlignment, 65536u),
                textureBufferOffsetAlignment: (int)Math.Min(c.MinTexelBufferOffsetAlignment, 65536u),
                gatherBiasPrecision: 0,
                maximumGpuMemory: c.DeviceLocalBytes,
                s2aVertexPull: false,
                // Device facts are global; linked policy belongs to an individual request.
                requiresLinkedVaryingInterface: false,
                maximumVertexOutputComponents: (int)vertexOutput,
                maximumFragmentInputComponents: (int)fragmentInput);
        }

        // 规范化 body：字段顺序固定、无空白，唯一不含 profileHash。profileHash = sha256(这份字节)。
        private static byte[] WriteBody(string closure, SupportLayout layout, Capabilities caps)
        {
            using var stream = new MemoryStream();
            using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = false, SkipValidation = false }))
            {
                w.WriteStartObject();
                w.WriteString("schema", DirectProfile.Schema);
                w.WriteNumber("directABI", 1);
                w.WriteString("translatorClosure", closure);
                w.WriteString("runtimeCompilerABI", DirectProfile.CompilerAbi);
                w.WriteString("apiVersion", "1.1");
                w.WriteString("spirvVersion", DirectProfile.SpirvVersion);
                w.WriteString("targetEnv", DirectProfile.TargetEnv);
                w.WriteStartObject("supportLayout");
                w.WriteNumber("requiredSize", layout.RequiredSize);
                w.WriteNumber("binding", layout.Binding);
                w.WriteString("hash", layout.Hash);
                w.WriteEndObject();
                w.WriteStartObject("capabilities");
                w.WriteString("vendorName", caps.VendorName);
                w.WriteString("memoryType", MemoryTypeName(caps.MemoryType));
                w.WriteBoolean("hasFrontFacingBug", caps.HasFrontFacingBug);
                w.WriteBoolean("hasVectorIndexingBug", caps.HasVectorIndexingBug);
                w.WriteBoolean("needsFragmentOutputSpecialization", caps.NeedsFragmentOutputSpecialization);
                w.WriteBoolean("reduceShaderPrecision", caps.ReduceShaderPrecision);
                w.WriteBoolean("supportsAstcCompression", caps.SupportsAstcCompression);
                w.WriteBoolean("supportsBc123Compression", caps.SupportsBc123Compression);
                w.WriteBoolean("supportsBc45Compression", caps.SupportsBc45Compression);
                w.WriteBoolean("supportsBc67Compression", caps.SupportsBc67Compression);
                w.WriteBoolean("supportsEtc2Compression", caps.SupportsEtc2Compression);
                w.WriteBoolean("supports3DTextureCompression", caps.Supports3DTextureCompression);
                w.WriteBoolean("supportsBgraFormat", caps.SupportsBgraFormat);
                w.WriteBoolean("supportsR4G4Format", caps.SupportsR4G4Format);
                w.WriteBoolean("supportsR4G4B4A4Format", caps.SupportsR4G4B4A4Format);
                w.WriteBoolean("supportsScaledVertexFormats", caps.SupportsScaledVertexFormats);
                w.WriteBoolean("supportsSnormBufferTextureFormat", caps.SupportsSnormBufferTextureFormat);
                w.WriteBoolean("supports5BitComponentFormat", caps.Supports5BitComponentFormat);
                w.WriteBoolean("supportsSparseBuffer", caps.SupportsSparseBuffer);
                w.WriteBoolean("supportsBlendEquationAdvanced", caps.SupportsBlendEquationAdvanced);
                w.WriteBoolean("supportsFragmentShaderInterlock", caps.SupportsFragmentShaderInterlock);
                w.WriteBoolean("supportsFragmentShaderOrderingIntel", caps.SupportsFragmentShaderOrderingIntel);
                w.WriteBoolean("supportsGeometryShader", caps.SupportsGeometryShader);
                w.WriteBoolean("supportsGeometryShaderPassthrough", caps.SupportsGeometryShaderPassthrough);
                w.WriteBoolean("supportsGeometryShaderOcclusionQuery", caps.SupportsGeometryShaderOcclusionQuery);
                w.WriteBoolean("supportsTransformFeedback", caps.SupportsTransformFeedback);
                w.WriteBoolean("supportsImageLoadFormatted", caps.SupportsImageLoadFormatted);
                w.WriteBoolean("supportsLayerVertexTessellation", caps.SupportsLayerVertexTessellation);
                w.WriteBoolean("supportsMismatchingViewFormat", caps.SupportsMismatchingViewFormat);
                w.WriteBoolean("supportsCubemapView", caps.SupportsCubemapView);
                w.WriteBoolean("supportsNonConstantTextureOffset", caps.SupportsNonConstantTextureOffset);
                w.WriteBoolean("supportsQuads", caps.SupportsQuads);
                w.WriteBoolean("supportsSeparateSampler", caps.SupportsSeparateSampler);
                w.WriteBoolean("supportsShaderBallot", caps.SupportsShaderBallot);
                w.WriteBoolean("supportsShaderBarrierDivergence", caps.SupportsShaderBarrierDivergence);
                w.WriteBoolean("supportsShaderFloat64", caps.SupportsShaderFloat64);
                w.WriteBoolean("supportsShaderNonUniformIndexing", caps.SupportsShaderNonUniformIndexing);
                w.WriteBoolean("supportsTextureGatherOffsets", caps.SupportsTextureGatherOffsets);
                w.WriteBoolean("supportsTextureShadowLod", caps.SupportsTextureShadowLod);
                w.WriteBoolean("supportsVertexStoreAndAtomics", caps.SupportsVertexStoreAndAtomics);
                w.WriteBoolean("supportsViewportIndexVertexTessellation", caps.SupportsViewportIndexVertexTessellation);
                w.WriteBoolean("supportsViewportMask", caps.SupportsViewportMask);
                w.WriteBoolean("supportsViewportSwizzle", caps.SupportsViewportSwizzle);
                w.WriteBoolean("supportsIndirectParameters", caps.SupportsIndirectParameters);
                w.WriteBoolean("supportsDepthClipControl", caps.SupportsDepthClipControl);
                w.WriteBoolean("s2aVertexPull", caps.S2AVertexPull);
                w.WriteNumber("uniformBufferSetIndex", caps.UniformBufferSetIndex);
                w.WriteNumber("storageBufferSetIndex", caps.StorageBufferSetIndex);
                w.WriteNumber("textureSetIndex", caps.TextureSetIndex);
                w.WriteNumber("imageSetIndex", caps.ImageSetIndex);
                w.WriteNumber("extraSetBaseIndex", caps.ExtraSetBaseIndex);
                w.WriteNumber("maximumExtraSets", caps.MaximumExtraSets);
                w.WriteNumber("maximumUniformBuffersPerStage", caps.MaximumUniformBuffersPerStage);
                w.WriteNumber("maximumStorageBuffersPerStage", caps.MaximumStorageBuffersPerStage);
                w.WriteNumber("maximumTexturesPerStage", caps.MaximumTexturesPerStage);
                w.WriteNumber("maximumImagesPerStage", caps.MaximumImagesPerStage);
                w.WriteNumber("maximumComputeSharedMemorySize", caps.MaximumComputeSharedMemorySize);
                w.WriteNumber("maximumSupportedAnisotropy", (int)MathF.Round(caps.MaximumSupportedAnisotropy));
                w.WriteNumber("shaderSubgroupSize", caps.ShaderSubgroupSize);
                w.WriteNumber("storageBufferOffsetAlignment", caps.StorageBufferOffsetAlignment);
                w.WriteNumber("textureBufferOffsetAlignment", caps.TextureBufferOffsetAlignment);
                w.WriteNumber("gatherBiasPrecision", caps.GatherBiasPrecision);
                w.WriteNumber("maximumGpuMemory", caps.MaximumGpuMemory);
                // Omit unknown legacy facts to preserve historical bytes/hash. Newly
                // measured facts intentionally change the global profile/cache identity.
                if (caps.MaximumVertexOutputComponents > 0)
                    w.WriteNumber("maximumVertexOutputComponents", caps.MaximumVertexOutputComponents);
                if (caps.MaximumFragmentInputComponents > 0)
                    w.WriteNumber("maximumFragmentInputComponents", caps.MaximumFragmentInputComponents);
                w.WriteEndObject();
                w.WriteEndObject();
            }
            return stream.ToArray();
        }

        private static string MemoryTypeName(SystemMemoryType type) => type switch
        {
            SystemMemoryType.UnifiedMemory => "unified",
            SystemMemoryType.DedicatedMemory => "dedicated",
            SystemMemoryType.DedicatedMemorySlowStorage => "dedicated-slow",
            _ => "backend",
        };

        // body 形如 {"schema":"s2a-direct-profile-v1","directABI":...}；
        // 把 profileHash 插在 schema 之后，其余字节原样保留。
        private static byte[] InsertHash(byte[] body, string hash)
        {
            int comma = Array.IndexOf(body, (byte)',');
            if (comma < 0)
                throw new DirectCompileException("INTERNAL", "canonical profile body has no field separator");
            byte[] prefix = Encoding.UTF8.GetBytes("{\"schema\":\"" + DirectProfile.Schema + "\",\"profileHash\":\"" + hash + "\",");
            var result = new byte[prefix.Length + body.Length - comma - 1];
            Buffer.BlockCopy(prefix, 0, result, 0, prefix.Length);
            Buffer.BlockCopy(body, comma + 1, result, prefix.Length, body.Length - comma - 1);
            return result;
        }
    }
}
