// s2a-device-caps-v1：native 从"创建成功的 Vulkan device"记录的实测事实
// （properties/limits/enabled features/format support/memory/enabled extensions）。
// 这里只做严格解析与范围校验，不补默认值；enabled profile 合成只使用出现在这份 JSON 里的字段。
using System;
using System.Collections.Generic;
using System.Text.Json;

namespace S2A.DirectCompiler
{
    public sealed class DirectDeviceCaps
    {
        public const string Schema = "s2a-device-caps-v1";
        public const int MaxBytes = 64 << 10;
        public const int MaxExtensions = 64;

        public readonly int ApiMajor, ApiMinor, ApiPatch;
        public readonly uint DriverVersion, VendorId, DeviceId, DeviceType;
        public readonly string DeviceName, DriverName;

        public readonly uint MaxPerStageDescriptorSamplers, MaxPerStageDescriptorSampledImages;
        public readonly uint MaxPerStageDescriptorUniformBuffers, MaxPerStageDescriptorStorageBuffers;
        public readonly uint MaxPerStageDescriptorStorageImages;
        public readonly uint MaxUniformBufferRange, MaxStorageBufferRange;
        public readonly uint MaxComputeSharedMemorySize, MaxComputeWorkGroupInvocations;
        // Missing in older v1 snapshots means unknown, never a measured zero.
        public readonly uint? MaxVertexOutputComponents, MaxFragmentInputComponents;
        public readonly float MaxSamplerAnisotropy;
        public readonly uint MinUniformBufferOffsetAlignment, MinStorageBufferOffsetAlignment;
        public readonly uint MinTexelBufferOffsetAlignment;
        public readonly int SubgroupSize;

        public readonly bool GeometryShader, GeometryShaderOcclusionQuery, TessellationShader, ImageCubeArray;
        public readonly bool ShaderFloat64, ShaderInt64;
        public readonly bool FragmentStoresAndAtomics, VertexPipelineStoresAndAtomics;
        public readonly bool TextureCompressionAstcLdr, TextureCompressionBc, TextureCompressionEtc2;
        public readonly bool ShaderStorageImageExtendedFormats, IndependentBlend, DepthClamp, MultiViewport;
        public readonly bool SamplerAnisotropy, ShaderImageGatherExtended, ShaderStorageImageReadWithoutFormat;

        public readonly bool FormatsBc123, FormatsBc45, FormatsBc67, FormatsEtc2, FormatsAstc;
        public readonly bool FormatsFiveBit, FormatsR4G4B4A4, FormatsScaledVertex;

        public readonly ulong DeviceLocalBytes;
        public readonly uint HeapCount, MemoryTypeCount;
        public readonly bool DeviceLocalHostVisible;

        public readonly string[] EnabledExtensions;

        private DirectDeviceCaps(JsonElement e, string where)
        {
            string api = JsonStrict.String(e, "apiVersion", where, 31);
            string[] parts = api.Split('.');
            if (parts.Length != 3 ||
                !int.TryParse(parts[0], out ApiMajor) || !int.TryParse(parts[1], out ApiMinor) ||
                !int.TryParse(parts[2], out ApiPatch) || ApiMajor != 1 || ApiMinor < 1 || ApiMinor > 4)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".apiVersion is not a 1.x Vulkan version: " + api);
            DriverVersion = JsonStrict.UInt32(e, "driverVersion", where, 0, uint.MaxValue);
            VendorId = JsonStrict.UInt32(e, "vendorId", where, 0, uint.MaxValue);
            DeviceId = JsonStrict.UInt32(e, "deviceId", where, 0, uint.MaxValue);
            DeviceType = JsonStrict.UInt32(e, "deviceType", where, 0, 7);
            DeviceName = JsonStrict.String(e, "deviceName", where, 127);
            DriverName = JsonStrict.String(e, "driverName", where, 127);
            SubgroupSize = JsonStrict.Int(e, "subgroupSize", where, 1, 128);

            JsonElement limits = JsonStrict.Object(e, "limits", where);
            string lw = where + ".limits";
            JsonStrict.OnlyKeys(limits, lw,
                "maxImageDimension2D", "maxImageDimensionCube", "maxImageArrayLayers",
                "maxPerStageDescriptorSamplers", "maxPerStageDescriptorSampledImages",
                "maxPerStageDescriptorUniformBuffers", "maxPerStageDescriptorStorageBuffers",
                "maxPerStageDescriptorStorageImages",
                "maxUniformBufferRange", "maxStorageBufferRange",
                "maxComputeSharedMemorySize", "maxComputeWorkGroupInvocations",
                "maxVertexOutputComponents", "maxFragmentInputComponents",
                "maxSamplerAnisotropy",
                "minUniformBufferOffsetAlignment", "minStorageBufferOffsetAlignment",
                "minTexelBufferOffsetAlignment", "subgroupSize");
            MaxPerStageDescriptorSamplers = JsonStrict.UInt32(limits, "maxPerStageDescriptorSamplers", lw, 1, uint.MaxValue);
            MaxPerStageDescriptorSampledImages = JsonStrict.UInt32(limits, "maxPerStageDescriptorSampledImages", lw, 1, uint.MaxValue);
            MaxPerStageDescriptorUniformBuffers = JsonStrict.UInt32(limits, "maxPerStageDescriptorUniformBuffers", lw, 1, uint.MaxValue);
            MaxPerStageDescriptorStorageBuffers = JsonStrict.UInt32(limits, "maxPerStageDescriptorStorageBuffers", lw, 0, uint.MaxValue);
            MaxPerStageDescriptorStorageImages = JsonStrict.UInt32(limits, "maxPerStageDescriptorStorageImages", lw, 0, uint.MaxValue);
            MaxUniformBufferRange = JsonStrict.UInt32(limits, "maxUniformBufferRange", lw, 1, uint.MaxValue);
            MaxStorageBufferRange = JsonStrict.UInt32(limits, "maxStorageBufferRange", lw, 1, uint.MaxValue);
            MaxComputeSharedMemorySize = JsonStrict.UInt32(limits, "maxComputeSharedMemorySize", lw, 1, 1 << 20);
            MaxComputeWorkGroupInvocations = JsonStrict.UInt32(limits, "maxComputeWorkGroupInvocations", lw, 1, 1 << 20);
            MaxVertexOutputComponents = limits.TryGetProperty("maxVertexOutputComponents", out _)
                ? JsonStrict.UInt32(limits, "maxVertexOutputComponents", lw, 1, uint.MaxValue) : null;
            MaxFragmentInputComponents = limits.TryGetProperty("maxFragmentInputComponents", out _)
                ? JsonStrict.UInt32(limits, "maxFragmentInputComponents", lw, 1, uint.MaxValue) : null;
            MaxSamplerAnisotropy = JsonStrict.Float(limits, "maxSamplerAnisotropy", lw);
            if (!float.IsFinite(MaxSamplerAnisotropy) || MaxSamplerAnisotropy < 0 || MaxSamplerAnisotropy > 1024)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", lw + ".maxSamplerAnisotropy is outside 0..1024");
            MinUniformBufferOffsetAlignment = JsonStrict.UInt32(limits, "minUniformBufferOffsetAlignment", lw, 1, 65536);
            MinStorageBufferOffsetAlignment = JsonStrict.UInt32(limits, "minStorageBufferOffsetAlignment", lw, 1, 65536);
            MinTexelBufferOffsetAlignment = JsonStrict.UInt32(limits, "minTexelBufferOffsetAlignment", lw, 1, 65536);
            if (JsonStrict.Int(limits, "subgroupSize", lw, 1, 128) != SubgroupSize)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", lw + ".subgroupSize disagrees with the top-level value");

            JsonElement features = JsonStrict.Object(e, "features", where);
            string fw = where + ".features";
            JsonStrict.OnlyKeys(features, fw,
                "geometryShader", "geometryShaderOcclusionQuery", "tessellationShader", "imageCubeArray",
                "shaderFloat64", "shaderInt64",
                "fragmentStoresAndAtomics", "vertexPipelineStoresAndAtomics",
                "textureCompressionASTC_LDR", "textureCompressionBC", "textureCompressionETC2",
                "shaderStorageImageExtendedFormats", "independentBlend", "depthClamp", "multiViewport",
                "samplerAnisotropy", "shaderImageGatherExtended", "shaderStorageImageReadWithoutFormat");
            GeometryShader = JsonStrict.Bool(features, "geometryShader", fw);
            GeometryShaderOcclusionQuery = JsonStrict.Bool(features, "geometryShaderOcclusionQuery", fw);
            TessellationShader = JsonStrict.Bool(features, "tessellationShader", fw);
            ImageCubeArray = JsonStrict.Bool(features, "imageCubeArray", fw);
            ShaderFloat64 = JsonStrict.Bool(features, "shaderFloat64", fw);
            ShaderInt64 = JsonStrict.Bool(features, "shaderInt64", fw);
            FragmentStoresAndAtomics = JsonStrict.Bool(features, "fragmentStoresAndAtomics", fw);
            VertexPipelineStoresAndAtomics = JsonStrict.Bool(features, "vertexPipelineStoresAndAtomics", fw);
            TextureCompressionAstcLdr = JsonStrict.Bool(features, "textureCompressionASTC_LDR", fw);
            TextureCompressionBc = JsonStrict.Bool(features, "textureCompressionBC", fw);
            TextureCompressionEtc2 = JsonStrict.Bool(features, "textureCompressionETC2", fw);
            ShaderStorageImageExtendedFormats = JsonStrict.Bool(features, "shaderStorageImageExtendedFormats", fw);
            IndependentBlend = JsonStrict.Bool(features, "independentBlend", fw);
            DepthClamp = JsonStrict.Bool(features, "depthClamp", fw);
            MultiViewport = JsonStrict.Bool(features, "multiViewport", fw);
            SamplerAnisotropy = JsonStrict.Bool(features, "samplerAnisotropy", fw);
            ShaderImageGatherExtended = JsonStrict.Bool(features, "shaderImageGatherExtended", fw);
            ShaderStorageImageReadWithoutFormat = JsonStrict.Bool(features, "shaderStorageImageReadWithoutFormat", fw);

            JsonElement formats = JsonStrict.Object(e, "formats", where);
            string mw = where + ".formats";
            JsonStrict.OnlyKeys(formats, mw, "bc123", "bc45", "bc67", "etc2", "astc", "fiveBit", "r4g4b4a4", "scaledVertex");
            FormatsBc123 = JsonStrict.Bool(formats, "bc123", mw);
            FormatsBc45 = JsonStrict.Bool(formats, "bc45", mw);
            FormatsBc67 = JsonStrict.Bool(formats, "bc67", mw);
            FormatsEtc2 = JsonStrict.Bool(formats, "etc2", mw);
            FormatsAstc = JsonStrict.Bool(formats, "astc", mw);
            FormatsFiveBit = JsonStrict.Bool(formats, "fiveBit", mw);
            FormatsR4G4B4A4 = JsonStrict.Bool(formats, "r4g4b4a4", mw);
            FormatsScaledVertex = JsonStrict.Bool(formats, "scaledVertex", mw);

            JsonElement memory = JsonStrict.Object(e, "memory", where);
            string nw = where + ".memory";
            JsonStrict.OnlyKeys(memory, nw, "deviceLocalBytes", "heapCount", "memoryTypeCount", "deviceLocalHostVisible");
            DeviceLocalBytes = JsonStrict.UInt64(memory, "deviceLocalBytes", nw, 0, ulong.MaxValue);
            HeapCount = JsonStrict.UInt32(memory, "heapCount", nw, 1, 32);
            MemoryTypeCount = JsonStrict.UInt32(memory, "memoryTypeCount", nw, 1, 32);
            DeviceLocalHostVisible = JsonStrict.Bool(memory, "deviceLocalHostVisible", nw);

            JsonElement extensions = JsonStrict.Array(e, "enabledExtensions", where);
            if (extensions.GetArrayLength() > MaxExtensions)
                throw new DirectCompileException("RESOURCE_LIMIT", where + ".enabledExtensions has more than " + MaxExtensions + " entries");
            var list = new List<string>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (JsonElement item in extensions.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.String)
                    throw new DirectCompileException("INVALID_JSON", where + ".enabledExtensions has a non-string entry");
                string name = item.GetString();
                if (string.IsNullOrEmpty(name) || name.Length > 255)
                    throw new DirectCompileException("INVALID_JSON", where + ".enabledExtensions has an empty/oversized entry");
                if (!seen.Add(name))
                    throw new DirectCompileException("DUPLICATE_KEY", where + ".enabledExtensions repeats " + name);
                list.Add(name);
            }
            EnabledExtensions = list.ToArray();
        }

        public bool HasExtension(string name) => Array.IndexOf(EnabledExtensions, name) >= 0;

        public static DirectDeviceCaps Parse(JsonElement e, string where)
        {
            if (e.ValueKind != JsonValueKind.Object)
                throw new DirectCompileException("INVALID_JSON", where + " is not an object");
            JsonStrict.OnlyKeys(e, where, "schema", "apiVersion", "driverVersion", "vendorId", "deviceId",
                "deviceType", "deviceName", "driverName", "limits", "features", "formats", "memory",
                "enabledExtensions", "subgroupSize");
            if (JsonStrict.String(e, "schema", where) != Schema)
                throw new DirectCompileException("UNSUPPORTED_PROFILE", where + ".schema is not " + Schema);
            return new DirectDeviceCaps(e, where);
        }
    }
}
