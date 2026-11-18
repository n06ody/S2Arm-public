// EffectiveProfile：native Prepare 实际启用能力的唯一投影。编译器只接受这里显式列出的值，
// 缺失/未知字段一律拒绝；不从硬件名、平台或默认答案推断。
using Ryujinx.Graphics.GAL;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace S2A.DirectCompiler
{
    public readonly struct SupportLayoutField
    {
        public readonly string Name;
        public readonly int Offset, Size;
        public readonly string Owner;
        public SupportLayoutField(string name, int offset, int size, string owner)
        { Name = name; Offset = offset; Size = size; Owner = owner; }
    }

    public sealed class SupportLayout
    {
        public const string Schema = "s2a-support-layout-v1";
        public readonly int RequiredSize, Binding;
        public readonly SupportLayoutField[] Fields;
        public readonly string Hash;

        private SupportLayout()
        {
            RequiredSize = SupportBuffer.RequiredSize;
            Binding = SupportBuffer.Binding;
            Fields = SupportBuffer.S2ARegistry
                .Select(f => new SupportLayoutField(f.Name, f.Offset, f.Size, f.Owner))
                .OrderBy(f => f.Offset).ThenBy(f => f.Name, StringComparer.Ordinal).ToArray();
            var text = new StringBuilder();
            text.Append(Schema).Append('\n').Append(RequiredSize).Append('\n').Append(Binding).Append('\n');
            foreach (SupportLayoutField field in Fields)
                text.Append(field.Offset).Append('\t').Append(field.Size).Append('\t').Append(field.Name)
                    .Append('\t').Append(field.Owner).Append('\n');
            byte[] digest = Sha256.HashData(Encoding.UTF8.GetBytes(text.ToString()));
            Hash = "sha256-" + Convert.ToHexStringLower(digest);
        }

        public static SupportLayout Compile() => new();
    }

    public sealed class DirectProfile
    {
        public const string Schema = "s2a-direct-profile-v1";
        public const string CompilerAbi = "s2a-direct-compile-v1";
        public const string TargetEnv = "vulkan1.1";
        public const string SpirvVersion = "1.3";

        public readonly string ProfileHash;
        public readonly string TranslatorClosure;
        public readonly string RuntimeCompilerAbi;
        public readonly uint DirectAbi;
        public readonly Capabilities Caps;
        public readonly SupportLayout Support = SupportLayout.Compile();

        private DirectProfile(string hash, string closure, string compilerAbi, uint directAbi, Capabilities caps)
        {
            ProfileHash = hash; TranslatorClosure = closure; RuntimeCompilerAbi = compilerAbi;
            DirectAbi = directAbi; Caps = caps;
        }

        public static DirectProfile Parse(JsonElement e, string where)
        {
            JsonStrict.OnlyKeys(e, where, "schema", "profileHash", "directABI", "translatorClosure",
                "runtimeCompilerABI", "apiVersion", "spirvVersion", "targetEnv", "capabilities", "supportLayout");
            if (JsonStrict.String(e, "schema", where) != Schema)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".schema must be " + Schema);
            string hash = JsonStrict.String(e, "profileHash", where, 80);
            if (hash.Length != 71 || !hash.StartsWith("sha256-", StringComparison.Ordinal) ||
                !hash.AsSpan(7).ToString().All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f'))
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".profileHash is not sha256-<64 lowercase hex>");
            string closure = JsonStrict.String(e, "translatorClosure", where, 127);
            string compilerAbi = JsonStrict.String(e, "runtimeCompilerABI", where, 63);
            if (compilerAbi != CompilerAbi)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".runtimeCompilerABI is not " + CompilerAbi);
            uint directAbi = JsonStrict.UInt32(e, "directABI", where, 1, uint.MaxValue);
            if (directAbi != 1)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".directABI is not 1");
            if (JsonStrict.String(e, "apiVersion", where, 15) != "1.1")
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".apiVersion is not 1.1");
            if (JsonStrict.String(e, "spirvVersion", where, 15) != SpirvVersion)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".spirvVersion is not " + SpirvVersion);
            if (JsonStrict.String(e, "targetEnv", where, 31) != TargetEnv)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".targetEnv is not " + TargetEnv);

            JsonElement support = JsonStrict.Object(e, "supportLayout", where);
            JsonStrict.OnlyKeys(support, where + ".supportLayout", "requiredSize", "binding", "hash");
            SupportLayout compiled = SupportLayout.Compile();
            if (JsonStrict.Int(support, "requiredSize", where + ".supportLayout", 1, int.MaxValue) != compiled.RequiredSize ||
                JsonStrict.Int(support, "binding", where + ".supportLayout", 0, 0) != compiled.Binding ||
                JsonStrict.String(support, "hash", where + ".supportLayout", 80) != compiled.Hash)
                throw new DirectCompileException("IDENTITY",
                    where + ".supportLayout does not match this translator closure's SupportBuffer layout");

            JsonElement caps = JsonStrict.Object(e, "capabilities", where);
            string cw = where + ".capabilities";
            JsonStrict.OnlyKeys(caps, cw,
                "vendorName", "memoryType",
                "hasFrontFacingBug", "hasVectorIndexingBug", "needsFragmentOutputSpecialization", "reduceShaderPrecision",
                "supportsAstcCompression", "supportsBc123Compression", "supportsBc45Compression", "supportsBc67Compression",
                "supportsEtc2Compression", "supports3DTextureCompression", "supportsBgraFormat", "supportsR4G4Format",
                "supportsR4G4B4A4Format", "supportsScaledVertexFormats", "supportsSnormBufferTextureFormat",
                "supports5BitComponentFormat", "supportsSparseBuffer", "supportsBlendEquationAdvanced",
                "supportsFragmentShaderInterlock", "supportsFragmentShaderOrderingIntel", "supportsGeometryShader",
                "supportsGeometryShaderPassthrough", "supportsGeometryShaderOcclusionQuery", "supportsTransformFeedback", "supportsImageLoadFormatted",
                "supportsLayerVertexTessellation", "supportsMismatchingViewFormat", "supportsCubemapView",
                "supportsNonConstantTextureOffset", "supportsQuads", "supportsSeparateSampler", "supportsShaderBallot",
                "supportsShaderBarrierDivergence", "supportsShaderFloat64", "supportsShaderNonUniformIndexing",
                "supportsTextureGatherOffsets", "supportsTextureShadowLod", "supportsVertexStoreAndAtomics",
                "supportsViewportIndexVertexTessellation", "supportsViewportMask", "supportsViewportSwizzle",
                "supportsIndirectParameters", "supportsDepthClipControl", "s2aVertexPull",
                "uniformBufferSetIndex", "storageBufferSetIndex", "textureSetIndex", "imageSetIndex",
                "extraSetBaseIndex", "maximumExtraSets",
                "maximumUniformBuffersPerStage", "maximumStorageBuffersPerStage",
                "maximumTexturesPerStage", "maximumImagesPerStage",
                "maximumComputeSharedMemorySize", "maximumSupportedAnisotropy", "shaderSubgroupSize",
                "storageBufferOffsetAlignment", "textureBufferOffsetAlignment", "gatherBiasPrecision", "maximumGpuMemory",
                "maximumVertexOutputComponents", "maximumFragmentInputComponents");
            int vertexOutput = caps.TryGetProperty("maximumVertexOutputComponents", out _)
                ? JsonStrict.Int(caps, "maximumVertexOutputComponents", cw, 0, int.MaxValue) : 0;
            int fragmentInput = caps.TryGetProperty("maximumFragmentInputComponents", out _)
                ? JsonStrict.Int(caps, "maximumFragmentInputComponents", cw, 0, int.MaxValue) : 0;
            string memory = JsonStrict.String(caps, "memoryType", cw, 31);
            SystemMemoryType memoryType = memory switch
            {
                "unified" => SystemMemoryType.UnifiedMemory,
                "dedicated" => SystemMemoryType.DedicatedMemory,
                "dedicated-slow" => SystemMemoryType.DedicatedMemorySlowStorage,
                "backend" => SystemMemoryType.BackendManaged,
                _ => throw new DirectCompileException("UNSUPPORTED_PROFILE", cw + ".memoryType is unknown"),
            };
            var parsed = new Capabilities(
                api: TargetApi.Vulkan,
                vendorName: JsonStrict.String(caps, "vendorName", cw, 127),
                memoryType: memoryType,
                hasFrontFacingBug: JsonStrict.Bool(caps, "hasFrontFacingBug", cw),
                hasVectorIndexingBug: JsonStrict.Bool(caps, "hasVectorIndexingBug", cw),
                needsFragmentOutputSpecialization: JsonStrict.Bool(caps, "needsFragmentOutputSpecialization", cw),
                reduceShaderPrecision: JsonStrict.Bool(caps, "reduceShaderPrecision", cw),
                supportsAstcCompression: JsonStrict.Bool(caps, "supportsAstcCompression", cw),
                supportsBc123Compression: JsonStrict.Bool(caps, "supportsBc123Compression", cw),
                supportsBc45Compression: JsonStrict.Bool(caps, "supportsBc45Compression", cw),
                supportsBc67Compression: JsonStrict.Bool(caps, "supportsBc67Compression", cw),
                supportsEtc2Compression: JsonStrict.Bool(caps, "supportsEtc2Compression", cw),
                supports3DTextureCompression: JsonStrict.Bool(caps, "supports3DTextureCompression", cw),
                supportsBgraFormat: JsonStrict.Bool(caps, "supportsBgraFormat", cw),
                supportsR4G4Format: JsonStrict.Bool(caps, "supportsR4G4Format", cw),
                supportsR4G4B4A4Format: JsonStrict.Bool(caps, "supportsR4G4B4A4Format", cw),
                supportsScaledVertexFormats: JsonStrict.Bool(caps, "supportsScaledVertexFormats", cw),
                supportsSnormBufferTextureFormat: JsonStrict.Bool(caps, "supportsSnormBufferTextureFormat", cw),
                supports5BitComponentFormat: JsonStrict.Bool(caps, "supports5BitComponentFormat", cw),
                supportsSparseBuffer: JsonStrict.Bool(caps, "supportsSparseBuffer", cw),
                supportsBlendEquationAdvanced: JsonStrict.Bool(caps, "supportsBlendEquationAdvanced", cw),
                supportsFragmentShaderInterlock: JsonStrict.Bool(caps, "supportsFragmentShaderInterlock", cw),
                supportsFragmentShaderOrderingIntel: JsonStrict.Bool(caps, "supportsFragmentShaderOrderingIntel", cw),
                supportsGeometryShader: JsonStrict.Bool(caps, "supportsGeometryShader", cw),
                supportsGeometryShaderPassthrough: JsonStrict.Bool(caps, "supportsGeometryShaderPassthrough", cw),
                supportsGeometryShaderOcclusionQuery: JsonStrict.Bool(caps, "supportsGeometryShaderOcclusionQuery", cw),
                supportsTransformFeedback: JsonStrict.Bool(caps, "supportsTransformFeedback", cw),
                supportsImageLoadFormatted: JsonStrict.Bool(caps, "supportsImageLoadFormatted", cw),
                supportsLayerVertexTessellation: JsonStrict.Bool(caps, "supportsLayerVertexTessellation", cw),
                supportsMismatchingViewFormat: JsonStrict.Bool(caps, "supportsMismatchingViewFormat", cw),
                supportsCubemapView: JsonStrict.Bool(caps, "supportsCubemapView", cw),
                supportsNonConstantTextureOffset: JsonStrict.Bool(caps, "supportsNonConstantTextureOffset", cw),
                supportsQuads: JsonStrict.Bool(caps, "supportsQuads", cw),
                supportsSeparateSampler: JsonStrict.Bool(caps, "supportsSeparateSampler", cw),
                supportsShaderBallot: JsonStrict.Bool(caps, "supportsShaderBallot", cw),
                supportsShaderBarrierDivergence: JsonStrict.Bool(caps, "supportsShaderBarrierDivergence", cw),
                supportsShaderFloat64: JsonStrict.Bool(caps, "supportsShaderFloat64", cw),
                supportsShaderNonUniformIndexing: JsonStrict.Bool(caps, "supportsShaderNonUniformIndexing", cw),
                supportsTextureGatherOffsets: JsonStrict.Bool(caps, "supportsTextureGatherOffsets", cw),
                supportsTextureShadowLod: JsonStrict.Bool(caps, "supportsTextureShadowLod", cw),
                supportsVertexStoreAndAtomics: JsonStrict.Bool(caps, "supportsVertexStoreAndAtomics", cw),
                supportsViewportIndexVertexTessellation: JsonStrict.Bool(caps, "supportsViewportIndexVertexTessellation", cw),
                supportsViewportMask: JsonStrict.Bool(caps, "supportsViewportMask", cw),
                supportsViewportSwizzle: JsonStrict.Bool(caps, "supportsViewportSwizzle", cw),
                supportsIndirectParameters: JsonStrict.Bool(caps, "supportsIndirectParameters", cw),
                supportsDepthClipControl: JsonStrict.Bool(caps, "supportsDepthClipControl", cw),
                uniformBufferSetIndex: JsonStrict.Int(caps, "uniformBufferSetIndex", cw, 0, 3),
                storageBufferSetIndex: JsonStrict.Int(caps, "storageBufferSetIndex", cw, 0, 3),
                textureSetIndex: JsonStrict.Int(caps, "textureSetIndex", cw, 0, 3),
                imageSetIndex: JsonStrict.Int(caps, "imageSetIndex", cw, 0, 3),
                extraSetBaseIndex: JsonStrict.Int(caps, "extraSetBaseIndex", cw, 4, 4),
                maximumExtraSets: JsonStrict.Int(caps, "maximumExtraSets", cw, 0, 0),
                maximumUniformBuffersPerStage: JsonStrict.UInt32(caps, "maximumUniformBuffersPerStage", cw, 1, 4096),
                maximumStorageBuffersPerStage: JsonStrict.UInt32(caps, "maximumStorageBuffersPerStage", cw, 0, 4096),
                maximumTexturesPerStage: JsonStrict.UInt32(caps, "maximumTexturesPerStage", cw, 0, 65536),
                maximumImagesPerStage: JsonStrict.UInt32(caps, "maximumImagesPerStage", cw, 0, 65536),
                maximumComputeSharedMemorySize: JsonStrict.Int(caps, "maximumComputeSharedMemorySize", cw, 1, 1 << 20),
                maximumSupportedAnisotropy: JsonStrict.Float(caps, "maximumSupportedAnisotropy", cw),
                shaderSubgroupSize: JsonStrict.Int(caps, "shaderSubgroupSize", cw, 1, 128),
                storageBufferOffsetAlignment: JsonStrict.Int(caps, "storageBufferOffsetAlignment", cw, 1, 65536),
                textureBufferOffsetAlignment: JsonStrict.Int(caps, "textureBufferOffsetAlignment", cw, 1, 65536),
                gatherBiasPrecision: JsonStrict.Int(caps, "gatherBiasPrecision", cw, 0, 4096),
                maximumGpuMemory: JsonStrict.UInt64(caps, "maximumGpuMemory", cw, 0, ulong.MaxValue),
                s2aVertexPull: JsonStrict.Bool(caps, "s2aVertexPull", cw),
                requiresLinkedVaryingInterface: false,
                maximumVertexOutputComponents: vertexOutput,
                maximumFragmentInputComponents: fragmentInput);
            if (parsed.UniformBufferSetIndex != 0 || parsed.StorageBufferSetIndex != 1 ||
                parsed.TextureSetIndex != 2 || parsed.ImageSetIndex != 3)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", cw + " set indices must be 0/1/2/3");
            if (parsed.SupportsGeometryShaderPassthrough ||
                parsed.SupportsTransformFeedback || parsed.SupportsLayerVertexTessellation ||
                parsed.SupportsViewportIndexVertexTessellation || parsed.SupportsViewportMask ||
                parsed.S2AVertexPull || parsed.RequiresLinkedVaryingInterface)
                throw new DirectCompileException("UNSUPPORTED_PROFILE",
                    cw + " enables a stage/feature the first direct backend does not implement");
            return new DirectProfile(hash, closure, compilerAbi, directAbi, parsed);
        }
    }
}
