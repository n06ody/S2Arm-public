// 一个 graphics/compute program 的不可变输入快照：代码、控制段、C1、绑定事实与状态。
// 解析只接受显式字段；缺失/未知/超界一律拒绝，不补默认答案。
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.GAL;
using Ryujinx.Graphics.Gpu.Shader;
using Ryujinx.Graphics.Shader;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;

namespace S2A.DirectCompiler
{
    public sealed class DirectStage
    {
        public readonly string Stage;
        public readonly byte[] Code, Control, C1;
        public readonly uint ConstantBufferUse;
        public readonly string CodeSource, ControlSource, C1Source;

        public DirectStage(string stage, byte[] code, byte[] control, byte[] c1, uint cbUse)
        {
            Stage = stage; Code = code; Control = control; C1 = c1; ConstantBufferUse = cbUse;
            CodeSource = "code:sha256:" + Sha256Hex(code);
            ControlSource = "control:sha256:" + Sha256Hex(control);
            C1Source = "c1:sha256:" + Sha256Hex(c1);
        }

        public static string Sha256Hex(byte[] bytes) => Convert.ToHexStringLower(Sha256.HashData(bytes));

        public bool IsFragment => Stage == "fs";
        public bool IsVertex => Stage == "vs";
        public bool IsCompute => Stage == "cs";
    }

    public sealed class DirectTextureFact
    {
        public readonly string Stage;
        public readonly int Handle, CbufSlot;
        public readonly SamplerType SamplerType;
        public readonly TextureFormat Format;
        public readonly bool IsBuffer, Normalized, Separate;
        public readonly int ArrayLength;

        public DirectTextureFact(string stage, int handle, int cbufSlot, SamplerType samplerType,
            TextureFormat format, bool isBuffer, bool normalized, bool separate, int arrayLength)
        {
            Stage = stage; Handle = handle; CbufSlot = cbufSlot; SamplerType = samplerType;
            Format = format; IsBuffer = isBuffer; Normalized = normalized; Separate = separate;
            ArrayLength = arrayLength;
        }
    }

    public sealed class DirectGraphicsFacts
    {
        public readonly GpuGraphicsState State;
        public readonly bool HasUnalignedStorageBuffer;

        public DirectGraphicsFacts(GpuGraphicsState state, bool hasUnalignedStorageBuffer)
        { State = state; HasUnalignedStorageBuffer = hasUnalignedStorageBuffer; }

        public static readonly string[] Keys =
        [
            "earlyZForce", "topology", "tessCw", "tessPatchType", "tessSpacing",
            "alphaToCoverageEnable", "alphaToCoverageDitherEnable", "viewportTransformDisable", "depthMode",
            "programPointSizeEnable", "pointSize", "alphaTestCompare", "alphaTestReference",
            "attributeTypes", "hasConstantBufferDrawParameters", "fragmentOutputTypes",
            "dualSourceBlendEnable", "yNegateEnabled", "originUpperLeft", "halvePrimitiveId",
            "hasUnalignedStorageBuffer",
        ];

        public static DirectGraphicsFacts Parse(JsonElement e, string where)
        {
            JsonStrict.OnlyKeys(e, where, Keys);
            Array32<AttributeType> attrs = default;
            JsonElement attributeTypes = JsonStrict.Array(e, "attributeTypes", where);
            if (attributeTypes.GetArrayLength() != 32)
                throw new DirectCompileException("INVALID_JSON", where + ".attributeTypes must have 32 entries");
            for (int i = 0; i < 32; i++)
                attrs[i] = EnumValue<AttributeType>(attributeTypes[i], where + ".attributeTypes[" + i + "]");
            Array8<AttributeType> outputs = default;
            JsonElement fragmentOutputTypes = JsonStrict.Array(e, "fragmentOutputTypes", where);
            if (fragmentOutputTypes.GetArrayLength() != 8)
                throw new DirectCompileException("INVALID_JSON", where + ".fragmentOutputTypes must have 8 entries");
            for (int i = 0; i < 8; i++)
                outputs[i] = EnumValue<AttributeType>(fragmentOutputTypes[i], where + ".fragmentOutputTypes[" + i + "]");
            var state = new GpuGraphicsState(
                earlyZForce: JsonStrict.Bool(e, "earlyZForce", where),
                topology: EnumValue<InputTopology>(e, "topology", where),
                tessCw: JsonStrict.Bool(e, "tessCw", where),
                tessPatchType: EnumValue<TessPatchType>(e, "tessPatchType", where),
                tessSpacing: EnumValue<TessSpacing>(e, "tessSpacing", where),
                alphaToCoverageEnable: JsonStrict.Bool(e, "alphaToCoverageEnable", where),
                alphaToCoverageDitherEnable: JsonStrict.Bool(e, "alphaToCoverageDitherEnable", where),
                viewportTransformDisable: JsonStrict.Bool(e, "viewportTransformDisable", where),
                depthMode: JsonStrict.Bool(e, "depthMode", where),
                programPointSizeEnable: JsonStrict.Bool(e, "programPointSizeEnable", where),
                pointSize: JsonStrict.Float(e, "pointSize", where),
                alphaTestCompare: EnumValue<AlphaTestOp>(e, "alphaTestCompare", where),
                alphaTestReference: JsonStrict.Float(e, "alphaTestReference", where),
                attributeTypes: attrs,
                hasConstantBufferDrawParameters: JsonStrict.Bool(e, "hasConstantBufferDrawParameters", where),
                fragmentOutputTypes: outputs,
                dualSourceBlendEnable: JsonStrict.Bool(e, "dualSourceBlendEnable", where),
                yNegateEnabled: JsonStrict.Bool(e, "yNegateEnabled", where),
                originUpperLeft: JsonStrict.Bool(e, "originUpperLeft", where),
                halvePrimitiveId: JsonStrict.Bool(e, "halvePrimitiveId", where));
            return new DirectGraphicsFacts(state, JsonStrict.Bool(e, "hasUnalignedStorageBuffer", where));
        }

        private static T EnumValue<T>(JsonElement value, string where) where T : struct, Enum
        {
            if (value.ValueKind != JsonValueKind.String || !Enum.TryParse(value.GetString(), false, out T result))
                throw new DirectCompileException("INVALID_JSON", where + " is not a known " + typeof(T).Name);
            return result;
        }

        private static T EnumValue<T>(JsonElement parent, string name, string where) where T : struct, Enum
        {
            if (!parent.TryGetProperty(name, out JsonElement value))
                throw new DirectCompileException("MISSING_FIELD", where + "." + name + " is missing");
            return EnumValue<T>(value, where + "." + name);
        }
    }

    public sealed class DirectComputeFacts
    {
        public readonly int LocalSizeX, LocalSizeY, LocalSizeZ, LocalMemoryBytes, SharedMemoryBytes;
        public readonly bool HasUnalignedStorageBuffer;
        public DirectComputeFacts(int x, int y, int z, int local, int shared, bool hasUnalignedStorageBuffer)
        { LocalSizeX = x; LocalSizeY = y; LocalSizeZ = z; LocalMemoryBytes = local; SharedMemoryBytes = shared; HasUnalignedStorageBuffer = hasUnalignedStorageBuffer; }
        internal GpuChannelComputeState State => new(LocalSizeX, LocalSizeY, LocalSizeZ, LocalMemoryBytes, SharedMemoryBytes, HasUnalignedStorageBuffer);

        public static DirectComputeFacts Parse(JsonElement e, string where, DirectProfile profile)
        {
            JsonStrict.OnlyKeys(e, where, "localSize", "localMemoryBytes", "sharedMemoryBytes", "hasUnalignedStorageBuffer");
            JsonElement size = JsonStrict.Array(e, "localSize", where);
            if (size.GetArrayLength() != 3)
                throw new DirectCompileException("INVALID_JSON", where + ".localSize must have three entries");
            int x = JsonStrict.Int(size[0], where + ".localSize[0]"), y = JsonStrict.Int(size[1], where + ".localSize[1]"),
                z = JsonStrict.Int(size[2], where + ".localSize[2]");
            if (x < 1 || y < 1 || z < 1 || x > 1024 || y > 1024 || z > 1024 || (long)x * y * z > 1024)
                throw new DirectCompileException("UNSUPPORTED_PROGRAM", where + ".localSize is outside the 1024-invocation contract");
            int local = JsonStrict.Int(e, "localMemoryBytes", where, 0, 1 << 20);
            int shared = JsonStrict.Int(e, "sharedMemoryBytes", where, 0, profile.Caps.MaximumComputeSharedMemorySize);
            return new DirectComputeFacts(x, y, z, local, shared, JsonStrict.Bool(e, "hasUnalignedStorageBuffer", where));
        }
    }

    public sealed class DirectProgram
    {
        public readonly string Kind;
        public readonly DirectStage[] Stages;   // graphics: vs, fs in that order; compute: cs
        public readonly DirectGraphicsFacts Graphics;
        public readonly DirectComputeFacts Compute;
        public readonly DirectTextureFact[] Textures;
        public readonly Dictionary<int, int> TextureBufferLengths;
        public readonly int SamplerArrayLength, TextureArrayLength;
        public readonly bool TransformFeedbackEnabled;

        private DirectProgram(string kind, DirectStage[] stages, DirectGraphicsFacts graphics, DirectComputeFacts compute,
            DirectTextureFact[] textures, Dictionary<int, int> textureBufferLengths, int samplerArrayLength,
            int textureArrayLength, bool transformFeedbackEnabled)
        {
            Kind = kind; Stages = stages; Graphics = graphics; Compute = compute; Textures = textures;
            TextureBufferLengths = textureBufferLengths; SamplerArrayLength = samplerArrayLength;
            TextureArrayLength = textureArrayLength; TransformFeedbackEnabled = transformFeedbackEnabled;
        }

        public DirectStage StageOf(string stage) => Stages.FirstOrDefault(s => s.Stage == stage);

        public static DirectProgram Parse(JsonElement e, string where, DirectProfile profile)
        {
            JsonStrict.OnlyKeys(e, where, "kind", "stages", "graphicsState", "computeState", "textures",
                "samplerArrayLength", "textureArrayLength", "textureBufferLengths", "transformFeedbackEnabled");
            string kind = JsonStrict.String(e, "kind", where, 15);
            if (kind != "graphics" && kind != "compute")
                throw new DirectCompileException("UNSUPPORTED_PROGRAM", where + ".kind must be graphics or compute");
            JsonElement stagesJson = JsonStrict.Array(e, "stages", where);
            if (stagesJson.GetArrayLength() < 1 || stagesJson.GetArrayLength() > 6)
                throw new DirectCompileException("RESOURCE_LIMIT", where + ".stages must have 1..6 entries");
            var stages = new List<DirectStage>();
            long snapshotBytes = 0;
            foreach (JsonElement stageJson in stagesJson.EnumerateArray())
            {
                string sw = where + ".stages[" + stages.Count + "]";
                JsonStrict.OnlyKeys(stageJson, sw, "stage", "code", "control", "c1", "constantBufferUse");
                string stage = JsonStrict.String(stageJson, "stage", sw, 3);
                if (stage != "vs" && stage != "fs" && stage != "cs")
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", sw + ".stage " + stage + " is not implemented");
                if (stages.Any(s => s.Stage == stage))
                    throw new DirectCompileException("INVALID_JSON", sw + ".stage duplicates " + stage);
                byte[] code = JsonStrict.Base64(stageJson, "code", sw, 16 << 20);
                byte[] control = JsonStrict.Base64(stageJson, "control", sw, 64 << 10);
                byte[] c1 = JsonStrict.Base64(stageJson, "c1", sw, 64 << 10);
                if (code.Length == 0 || code.Length % 4 != 0 || code.Length > 16 << 20)
                    throw new DirectCompileException("RESOURCE_LIMIT", sw + ".code must be non-empty, 4-byte aligned and < 16 MiB");
                if (stage != "cs" && code.Length < 0x50)
                    throw new DirectCompileException("INVALID_JSON", sw + ".code has no 0x50-byte Maxwell header");
                if (control.Length == 0)
                    throw new DirectCompileException("MISSING_FIELD", sw + ".control snapshot is empty");
                snapshotBytes += code.Length + control.Length + c1.Length;
                if (snapshotBytes > 16 << 20)
                    throw new DirectCompileException("RESOURCE_LIMIT", where + " snapshot exceeds 16 MiB");
                // 常量缓冲掩码必须是完整 32 位：旧实现截在 0x1F（只表达 0..4 号），
                // 阶段绑定了 >=5 号 cbuf 时信息丢失，生成的着色器与运行时 layout 对不上，
                // 真机表现就是 Adreno “Bind group info mismatches the shader source for symbol fp_c1”
                // → vkCreateGraphicsPipelines failed → abort。
                uint cbUse = JsonStrict.UInt32(stageJson, "constantBufferUse", sw, 0, 0xFFFFFFFFu);
                stages.Add(new DirectStage(stage, code, control, c1, cbUse));
            }
            if (kind == "compute")
            {
                if (stages.Count != 1 || !stages[0].IsCompute)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", where + ": compute program must contain exactly cs");
            }
            else
            {
                // 图形程序认两种：vs+fs；只有 vs（只写深度的绘制：深度预通道、阴影图，游戏本来就没绑片元阶段）
                bool vsFs = stages.Count == 2 && stages.Any(s => s.IsVertex) && stages.Any(s => s.IsFragment);
                bool vsOnly = stages.Count == 1 && stages[0].IsVertex;
                if (!vsFs && !vsOnly)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", where + ": graphics contract is vs+fs or vs alone");
                stages = stages.OrderBy(s => s.IsVertex ? 0 : 1).ToList();
            }
            bool tf = JsonStrict.Bool(e, "transformFeedbackEnabled", where);
            if (tf)
                throw new DirectCompileException("UNSUPPORTED_PROGRAM", where + ".transformFeedbackEnabled is not implemented");

            DirectGraphicsFacts graphics = kind == "graphics"
                ? DirectGraphicsFacts.Parse(JsonStrict.Object(e, "graphicsState", where), where + ".graphicsState") : null;
            DirectComputeFacts compute = kind == "compute"
                ? DirectComputeFacts.Parse(JsonStrict.Object(e, "computeState", where), where + ".computeState", profile) : null;

            JsonElement texturesJson = JsonStrict.Array(e, "textures", where);
            var textures = new List<DirectTextureFact>();
            foreach (JsonElement t in texturesJson.EnumerateArray())
            {
                string tw = where + ".textures[" + textures.Count + "]";
                JsonStrict.OnlyKeys(t, tw, "stage", "handle", "cbufSlot", "samplerType", "format",
                    "isBuffer", "normalized", "separate", "arrayLength");
                string stageName = JsonStrict.String(t, "stage", tw, 3);
                if (!stages.Any(s => s.Stage == stageName))
                    throw new DirectCompileException("INVALID_JSON", tw + ".stage is not part of this program");
                string samplerText = JsonStrict.String(t, "samplerType", tw, 31);
                string formatText = JsonStrict.String(t, "format", tw, 63);
                // Unknown = 采样专用贴图（BC / sRGB / 深度 …，不在 TextureFormat 枚举里）：采样器类型、归一化照用；图像读写查格式时才缺事实（DirectGpuAccessor.QueryTextureFormat）
                if (!Enum.TryParse(formatText, false, out TextureFormat format))
                    throw new DirectCompileException("MISSING_FACT", tw + ".format is not a TextureFormat name");
                var fact = new DirectTextureFact(stageName, JsonStrict.Int(t, "handle", tw, 0, 1 << 20),
                    JsonStrict.Int(t, "cbufSlot", tw, -1, 1 << 20), ParseSamplerType(samplerText, tw),
                    format, JsonStrict.Bool(t, "isBuffer", tw), JsonStrict.Bool(t, "normalized", tw),
                    JsonStrict.Bool(t, "separate", tw), JsonStrict.Int(t, "arrayLength", tw, 1, 65536));
                textures.Add(fact);
            }
            var bufferLengths = new Dictionary<int, int>();
            if (e.TryGetProperty("textureBufferLengths", out JsonElement lengths))
            {
                if (lengths.ValueKind != JsonValueKind.Object)
                    throw new DirectCompileException("INVALID_JSON", where + ".textureBufferLengths is not an object");
                foreach (JsonProperty property in lengths.EnumerateObject())
                {
                    if (!int.TryParse(property.Name, out int slot) || slot < 0 || slot > 1 << 20 ||
                        property.Value.ValueKind != JsonValueKind.Number || !property.Value.TryGetInt32(out int length) ||
                        length < 1 || length > 1 << 20)
                        throw new DirectCompileException("INVALID_JSON", where + ".textureBufferLengths entry is invalid");
                    bufferLengths[slot] = length;
                }
            }
            int samplerLength = JsonStrict.Int(e, "samplerArrayLength", where, 1, 1048576);
            int textureLength = JsonStrict.Int(e, "textureArrayLength", where, 1, 1048576);
            return new DirectProgram(kind, stages.ToArray(), graphics, compute, textures.ToArray(),
                bufferLengths, samplerLength, textureLength, tf);
        }

        private static SamplerType ParseSamplerType(string text, string where)
        {
            SamplerType type = SamplerType.None;
            foreach (string part in text.Split('-', StringSplitOptions.RemoveEmptyEntries))
            {
                type |= part switch
                {
                    "1d" => SamplerType.Texture1D,
                    "2d" => SamplerType.Texture2D,
                    "3d" => SamplerType.Texture3D,
                    "buffer" => SamplerType.TextureBuffer,
                    "cube" => SamplerType.TextureCube,
                    "array" => SamplerType.Array,
                    "ms" => SamplerType.Multisample,
                    "shadow" => SamplerType.Shadow,
                    _ => throw new DirectCompileException("INVALID_JSON", where + ".samplerType has unknown part " + part),
                };
            }
            if ((type & SamplerType.Mask) == 0)
                throw new DirectCompileException("INVALID_JSON", where + ".samplerType has no base type");
            return type;
        }
    }
}
