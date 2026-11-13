// 直连统一着色器编译器：离线 CLI/出包与 Android NativeAOT 需求编译调用同一实现。
// 输入只有带版本的严格 JSON（代码/控制段/C1/绑定事实/EffectiveProfile），输出程序 JSON。
// 不做任何 GPU 调用、不读磁盘缓存、不使用 DispatchProxy/反射造类型。
using Ryujinx.Common.Configuration;
using Ryujinx.Graphics.GAL;
using Ryujinx.Graphics.Gpu;
using Ryujinx.Graphics.Gpu.Shader;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;

namespace S2A.DirectCompiler
{
    public static class DirectShaderCompiler
    {
        public const string RequestSchema = "s2a-direct-compile-v1";
        // v2 图形 stages 是五个 NVN 槽 [vs,fs,gs,tcs,tes]，未绑定的写 null。
        // 只接 VS / VS+FS / VS+GS+FS；v1 的图形和计算合同保持原样。
        public const string GraphicsRequestSchemaV2 = "s2a-direct-compile-v2";
        public const string ResultSchema = "s2a-direct-program-v1";
        public const string ContentContract = "s2a-direct-program-content-v1";
        public const int MaxRequestBytes = 256 << 20;

        public static byte[] Compile(byte[] requestUtf8)
        {
            using JsonDocument document = JsonStrict.Parse(requestUtf8, MaxRequestBytes);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new DirectCompileException("INVALID_JSON", "request root is not an object");
            JsonStrict.OnlyKeys(root, "request", "schema", "session", "generation", "requestId",
                "directABI", "translatorClosure", "runtimeCompilerABI", "sourceParserVersion", "kindCheck", "profile", "program");
            string requestSchema = JsonStrict.String(root, "schema", "request");
            int requestVersion = requestSchema == RequestSchema ? 1 : requestSchema == GraphicsRequestSchemaV2 ? 2 : 0;
            if (requestVersion == 0)
                throw new DirectCompileException("UNSUPPORTED_VERSION", "unknown request.schema " + requestSchema);
            // 描述符种类校验开关：native 过了调试门、debug.s2a.kind_check=0 才在请求里带 kindCheck=false；
            // 缺省（离线库/CLI）为开。这里只从请求读，不读进程环境变量。
            bool kindCheck = JsonStrict.OptionalBool(root, "kindCheck", "request", true);
            ulong session = JsonStrict.UInt64(root, "session", "request", 1, ulong.MaxValue);
            ulong generation = JsonStrict.UInt64(root, "generation", "request", 1, ulong.MaxValue);
            string requestId = JsonStrict.String(root, "requestId", "request", 127);
            uint directAbi = JsonStrict.UInt32(root, "directABI", "request", 1, uint.MaxValue);
            string closure = JsonStrict.String(root, "translatorClosure", "request", 127);
            // 身份只认本构建：描述文件/生产者传来的字符串必须等于编译进去的闭包，
            // 只核对"请求 == profile"会放过任意自洽的假身份（如按游戏手写的 pass1）。
            if (!string.Equals(closure, DirectBuildIdentity.TranslatorClosure, StringComparison.Ordinal))
                throw new DirectCompileException("IDENTITY",
                    "request.translatorClosure is not this compiler build's closure (" +
                    DirectBuildIdentity.TranslatorClosure + ")");
            string compilerAbi = JsonStrict.String(root, "runtimeCompilerABI", "request", 63);
            int sourceParserVersion = JsonStrict.Int(root, "sourceParserVersion", "request", 1, 1);
            if (compilerAbi != DirectProfile.CompilerAbi)
                throw new DirectCompileException("UNSUPPORTED_VERSION", "request.runtimeCompilerABI is not " + DirectProfile.CompilerAbi);
            DirectProfile profile = DirectProfile.Parse(JsonStrict.Object(root, "profile", "request"), "request.profile");
            if (profile.DirectAbi != directAbi || profile.TranslatorClosure != closure || profile.RuntimeCompilerAbi != compilerAbi)
                throw new DirectCompileException("IDENTITY", "request identity does not match the effective profile");
            DirectProgram program = DirectProgram.Parse(JsonStrict.Object(root, "program", "request"), "request.program", profile, requestVersion);
            if (program.StageOf("gs") is DirectStage gs)
            {
                if (!profile.Caps.SupportsGeometryShader)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", "geometryShader is not enabled in the profile");
                if (program.Graphics.State.Topology is InputTopology.LinesAdjacency or InputTopology.TrianglesAdjacency)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", "geometry adjacency topology is not implemented");
                uint header = BinaryPrimitives.ReadUInt32LittleEndian(gs.Code);
                if (((header >> 10) & 15) != 4 || (header & (1u << 24)) != 0)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", "GS must have a non-passthrough geometry header");
            }
            _ = sourceParserVersion;

            var queries = new List<DirectQuery>();
            var allocator = new DirectAllocator(profile);
            var accessors = new List<DirectGpuAccessor>();
            var translated = new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>();
            using var context = new GpuContext(new ProfileOnlyRenderer(profile.Caps), new DirtyHacks(Array.Empty<ulong>()))
            {
                Capabilities = profile.Caps,
            };
            try
            {
                if (program.Kind == "compute")
                {
                    DirectStage cs = program.StageOf("cs");
                    ValidateComputeControl(cs, program.Compute);
                    var accessor = new DirectGpuAccessor(profile, program, cs, 0, allocator, queries);
                    accessors.Add(accessor);
                    ShaderProgram shader = ShaderCache.DecodeComputeShader(accessor, TargetApi.Vulkan, 0).Translate();
                    translated.Add((cs, shader, accessor));
                }
                else
                {
                    // NVN 槽序 VS/FS/GS 与执行序不同；解码后按实际管线顺序显式串联。
                    var ordered = new[] { (name: "vs", index: 1), (name: "gs", index: 4), (name: "fs", index: 5) };
                    var contexts = new List<(DirectStage stage, DirectGpuAccessor accessor, TranslatorContext context)>();
                    foreach (var item in ordered)
                    {
                        DirectStage stage = program.StageOf(item.name);
                        if (stage == null) continue;
                        var accessor = new DirectGpuAccessor(profile, program, stage, item.index, allocator, queries);
                        accessors.Add(accessor);
                        contexts.Add((stage, accessor, ShaderCache.DecodeGraphicsShader(accessor, TargetApi.Vulkan, ShaderCache.DefaultFlags, 0)));
                    }
                    for (int i = contexts.Count - 2; i >= 0; i--)
                    {
                        if (contexts[i].stage.Stage == "gs") contexts[i].context.S2ASetNextStageActual(contexts[i + 1].context);
                        else contexts[i].context.SetNextStage(contexts[i + 1].context);
                    }
                    foreach (var item in contexts) translated.Add((item.stage, item.context.Translate(), item.accessor));
                }
            }
            catch (DirectCompileException) { throw; }
            catch (Exception error)
            {
                throw new DirectCompileException("TRANSLATION", error.GetType().Name + ": " + error.Message);
            }

            foreach (DirectGpuAccessor accessor in accessors)
                if (accessor.Diagnostics.Count != 0)
                    throw new DirectCompileException("TRANSLATION_DIAGNOSTIC",
                        accessor.Diagnostics[0] + " (stage " + accessorStage(accessor, translated) + ")");

            foreach (var entry in translated)
                ValidateSpirv(entry.stage, entry.shader, entry.accessor, allocator, profile, kindCheck, program.StageOf("gs") != null);
            ResolvePoolSlots(translated, allocator);
            ResolveStorageSlots(translated, allocator);

            byte[] content = WriteContent(program, profile, translated, allocator, queries);
            string contentHash = "sha256-" + Convert.ToHexStringLower(Sha256.HashData(content));
            return WriteEnvelope(session, generation, requestId, content, contentHash);
        }

        // 与 runtime/direct/control.c、merge_extract.py:cs_facts 同口径：+0x748 三组、+0x754 shared，
        // shared 按 16 对齐；computeState 必须与控制段一致，缺字段/越界/不一致都拒绝，不补 1x1x1。
        private static void ValidateComputeControl(DirectStage stage, DirectComputeFacts facts)
        {
            if (stage.Control.Length < 0x758)
                throw new DirectCompileException("MISSING_FACT", stage.Stage + ": control segment is shorter than 0x758");
            uint x = BitConverter.ToUInt32(stage.Control, 0x748);
            uint y = BitConverter.ToUInt32(stage.Control, 0x74C);
            uint z = BitConverter.ToUInt32(stage.Control, 0x750);
            uint shared = BitConverter.ToUInt32(stage.Control, 0x754);
            if (x == 0 || y == 0 || z == 0)
                throw new DirectCompileException("CONTROL_INVALID", stage.Stage + ": control local size contains zero");
            if ((ulong)x * y * z > 1024)
                throw new DirectCompileException("CONTROL_INVALID", stage.Stage + ": control local size exceeds 1024 invocations");
            if (shared > 0x10000)
                throw new DirectCompileException("CONTROL_INVALID", stage.Stage + ": control shared memory exceeds 64 KiB");
            uint aligned = (shared + 15u) / 16u * 16u;
            if (x != (uint)facts.LocalSizeX || y != (uint)facts.LocalSizeY || z != (uint)facts.LocalSizeZ ||
                aligned != (uint)facts.SharedMemoryBytes)
                throw new DirectCompileException("CONTROL_MISMATCH",
                    $"{stage.Stage}: computeState {facts.LocalSizeX}x{facts.LocalSizeY}x{facts.LocalSizeZ} shared {facts.SharedMemoryBytes} " +
                    $"does not match control {x}x{y}x{z} shared {aligned}");
        }

        private static string accessorStage(DirectGpuAccessor accessor, List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)> translated) =>
            translated.FirstOrDefault(t => ReferenceEquals(t.accessor, accessor)).stage?.Stage ?? "?";

        private static void ValidateSpirv(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor,
            DirectAllocator allocator, DirectProfile profile, bool kindCheck, bool rejectViewportArray)
        {
            byte[] spv = shader.BinaryCode;
            if (spv == null || spv.Length < 20 || spv.Length % 4 != 0)
                throw new DirectCompileException("SPIRV_INVALID", stage.Stage + ": truncated SPIR-V");
            uint[] words = new uint[spv.Length / 4];
            Buffer.BlockCopy(spv, 0, words, 0, spv.Length);
            if (words[0] != 0x07230203)
                throw new DirectCompileException("SPIRV_INVALID", stage.Stage + ": bad magic");
            if (words[1] != 0x00010300)
                throw new DirectCompileException("SPIRV_INVALID", stage.Stage + ": target is not SPIR-V 1.3");
            bool entry = false;
            int instructionEnd = 5;
            for (int i = 5; i < words.Length;)
            {
                int count = (int)(words[i] >> 16);
                int opcode = (int)(words[i] & 0xFFFF);
                if (count == 0 || i + count > words.Length)
                    throw new DirectCompileException("SPIRV_INVALID", stage.Stage + ": truncated instruction");
                // The direct replay path does not implement viewport arrays. Check actual SPIR-V
                // decorations for all stages in a GS program, including interface-block members.
                int builtinWord = opcode == (int)Spv.Specification.Op.OpDecorate && count >= 4 &&
                    words[i + 2] == (uint)Spv.Specification.Decoration.BuiltIn ? i + 3 :
                    opcode == (int)Spv.Specification.Op.OpMemberDecorate && count >= 5 &&
                    words[i + 3] == (uint)Spv.Specification.Decoration.BuiltIn ? i + 4 : -1;
                if (rejectViewportArray && builtinWord >= 0 && words[builtinWord] is
                    (uint)Spv.Specification.BuiltIn.ViewportIndex or (uint)Spv.Specification.BuiltIn.ViewportMaskNV or
                    (uint)Spv.Specification.BuiltIn.SecondaryViewportMaskNV or (uint)Spv.Specification.BuiltIn.ViewportMaskPerViewNV)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", stage.Stage + ": viewport arrays are not implemented");
                if (opcode == 15 && count >= 4)
                {
                    int nameStart = i + 3;
                    int chars = (count - 3) * 4;
                    byte[] raw = new byte[chars];
                    Buffer.BlockCopy(words, nameStart * 4, raw, 0, chars);
                    int end = Array.IndexOf(raw, (byte)0);
                    string name = System.Text.Encoding.UTF8.GetString(raw, 0, end < 0 ? chars : end);
                    if (name == "main") entry = true;
                }
                instructionEnd = i + count;
                i += count;
            }
            // 指令流必须刚好覆盖全部字；header 的第 3 个字不是模块长度。
            if (instructionEnd != words.Length)
                throw new DirectCompileException("SPIRV_INVALID", stage.Stage + ": trailing words after the instruction stream");
            if (!entry)
                throw new DirectCompileException("SPIRV_INVALID", stage.Stage + ": no main entry point");
            foreach (SharedDescriptor pair in SpirvReflection.Descriptors(words))
            {
                if (pair.Set < 0 || pair.Set > 3)
                    throw new DirectCompileException("SPIRV_INVALID", stage.Stage + ": descriptor set " + pair.Set + " outside 0..3");
                if (pair.Set == 0 && pair.Binding == 0) continue;   // support buffer
                DirectBinding src = allocator.Bindings.FirstOrDefault(b => b.Set == pair.Set && b.Binding == pair.Binding &&
                                                                          (b.Stage == stage.Stage || b.Stage == "program"));
                if (src == null)
                    throw new DirectCompileException("SPIRV_INVALID",
                        stage.Stage + ": SPIR-V descriptor " + pair.Set + ":" + pair.Binding + " has no allocator source");
                // 只在“两边都明确”时比对描述符种类：UBO 是 Uniform(2)，纹理/图像是 UniformConstant(0)。
                // 之前只比对 set/binding 是否存在，类型错要到设备上建管线才暴露（驱动只报一个符号名）。
                SpirvReflection.CheckDescriptorKind(stage.Stage, pair, src.Kind, kindCheck);
            }
            foreach (DirectBinding binding in allocator.Bindings.Where(b => b.Kind != "support" &&
                                                                             (b.Stage == stage.Stage || b.Stage == "program")))
            {
                bool present = SpirvReflection.Descriptors(words).Any(d => d.Set == binding.Set && d.Binding == binding.Binding);
                if (!present && shader.Info != null)
                    throw new DirectCompileException("SPIRV_INVALID",
                        stage.Stage + ": reflected binding " + binding.Set + ":" + binding.Binding + " is absent from SPIR-V");
            }
        }

        // 池槽位只能由反射结果决定：翻译器先建 texture binding、后 RegisterTexture，
        // 按调用顺序猜会把这几个槽位全指到第一张纹理上。cbufSlot >= 0 的句柄来自常量
        // 缓冲：只有同句柄存在直接绑定事实时才按 NVN c2→组合槽放行；其余 cbuf 路径和
        // separate 采样器仍未实现，明确报红，不写死 0 让运行时绑错。
        private static void ResolvePoolSlots(List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)> translated,
            DirectAllocator allocator)
        {
            foreach (DirectBinding binding in allocator.Bindings)
            {
                if (binding.Kind != "texture" && binding.Kind != "image")
                    continue;
                var entry = translated.FirstOrDefault(t => t.stage.Stage == binding.Stage);
                if (entry.shader == null || entry.shader.Info == null)
                    throw new DirectCompileException("IDENTITY", "binding stage " + binding.Stage + " has no translated shader info");
                TextureDescriptor? found = null;
                foreach (TextureDescriptor descriptor in binding.Kind == "image" ? entry.shader.Info.Images : entry.shader.Info.Textures)
                {
                    if (descriptor.Set == binding.Set && descriptor.Binding == binding.Binding) { found = descriptor; break; }
                }
                if (found == null)
                    throw new DirectCompileException("SPIRV_INVALID",
                        binding.Stage + ": " + binding.Kind + " binding " + binding.Set + ":" + binding.Binding + " has no reflected descriptor");
                TextureDescriptor resolved = found.Value;
                if (resolved.CbufSlot >= 0)
                {
                    // NVN 的 c2 句柄与直连组合纹理槽一一对应：native 侧按 handleIndex
                    // 找回同一个 nvn_tex_slot（见 nvn_vk_first_draw 的 handleIndex→slot 表）。
                    // 只有该句柄有直接绑定事实时才放行；没有就直接事实的 cbuf 句柄仍是
                    // 未实现路径，不能把槽位绑到别的纹理。
                    // 例外：着色器按运行时下标取句柄，翻译器把它变成从驱动句柄表（c2）第 0 项起的贴图数组（BindlessToArray）。
                    // 数组的第 k 项就是句柄表的第 k 个 8 字节项，宿主按项逐个绑（slot = 0、count = 数组长度），不需要逐句柄的事实。
                    bool handleTableArray = binding.Kind == "texture" && resolved.CbufSlot == 2 && resolved.ArrayLength > 1 &&
                                            resolved.HandleIndex == 0 && binding.Count == resolved.ArrayLength;
                    if (!handleTableArray && (binding.Kind != "texture" || !entry.accessor.HasDirectTextureFact(resolved.HandleIndex)))
                        throw new DirectCompileException("UNSUPPORTED_PROGRAM",
                            binding.Stage + ": texture handle from cbuf slot " + resolved.CbufSlot + " is not implemented");
                }
                if (resolved.Separate)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM",
                        binding.Stage + ": separate sampler slot is not implemented");
                binding.Slot = resolved.HandleIndex;
            }
        }

        // Same address provenance rules as dmsh_build.py, without guessing from allocation order.
        private static void ApplyStorageDescriptor(DirectBinding binding, BufferDescriptor descriptor)
        {
            binding.CbSlot = descriptor.SbCbSlot;
            binding.CbOffset = checked(descriptor.SbCbOffset * 4);
            binding.Slot = -1;
            binding.Writable = (descriptor.Flags & BufferUsageFlags.Write) != 0;
            binding.NvnSlot = binding.UboPtr = binding.UboSlot = binding.UboOffset = -1;
            int off = binding.CbOffset;
            // NVN driver c0 storage table base by stage (nvn_mtl.m c0_ssbo_base):
            // vertex/pixel/geometry/hull/domain/compute = 0x110/0x510/0x410/0x210/0x310/0x310.
            int ssbo_base = binding.Stage switch
            {
                "vs" => 0x110,
                "fs" => 0x510,
                "gs" => 0x410,
                "tcs" => 0x210,
                "tes" => 0x310,
                "cs" => 0x310,
                _ => -1
            };
            if (binding.CbSlot == 0 && binding.Stage == "cs" && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0)
                binding.UboPtr = (off - 0x210) / 16;
            else if (binding.CbSlot >= 3)
            {
                binding.UboSlot = binding.CbSlot - 3;
                binding.UboOffset = off;
            }
            else if (binding.CbSlot == 0 && ssbo_base >= 0 && off >= ssbo_base && off < ssbo_base + 0x100 &&
                     (off - ssbo_base) % 16 == 0)
                binding.NvnSlot = (off - ssbo_base) / 16;
            // Unknown remains explicit source facts with no mapping; runtime must miss.
        }

        // Kept for the existing compute storage mapper regression (reflection by name);
        // graphics and compute share the exact same provenance rules.
        private static void ApplyComputeStorageDescriptor(DirectBinding binding, BufferDescriptor descriptor)
            => ApplyStorageDescriptor(binding, descriptor);

        // Existing regression reflects on the compute-stage entry point by name.
        private static void ResolveComputeStorageSlots(
            List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)> translated,
            DirectAllocator allocator) => ResolveStorageSlots(translated, allocator);

        // Graphics storage c0 tables are per NVN stage:
        // vs/fs/gs/tcs/tes = 0x110/0x510/0x410/0x210/0x310; compute = 0x310.
        // cbuf >= 3 = user UBO holding an SbDescriptor; compute 0x210..0x310 is the
        // indirect UBO pointer table. The runtime maps these facts to the guest
        // binding instead of re-deriving NVN slots.
        private static void ResolveStorageSlots(
            List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)> translated,
            DirectAllocator allocator)
        {
            foreach (DirectBinding binding in allocator.Bindings.Where(b => b.Kind == "storage"))
            {
                var entry = translated.FirstOrDefault(t => t.stage.Stage == binding.Stage);
                if (entry.shader?.Info == null)
                    throw new DirectCompileException("IDENTITY", binding.Stage + " storage has no translated shader info");
                BufferDescriptor? found = null;
                foreach (BufferDescriptor descriptor in entry.shader.Info.SBuffers)
                {
                    if (descriptor.Set != binding.Set || descriptor.Binding != binding.Binding) continue;
                    if (found != null) throw new DirectCompileException("SPIRV_INVALID", "duplicate storage descriptor");
                    found = descriptor;
                }
                if (found == null)
                    throw new DirectCompileException("SPIRV_INVALID", binding.Stage + " storage has no reflected descriptor");
                ApplyStorageDescriptor(binding, found.Value);
            }
        }

        private static byte[] WriteContent(DirectProgram program, DirectProfile profile,
            List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)> translated,
            DirectAllocator allocator, List<DirectQuery> queries)
        {
            using var stream = new MemoryStream();
            using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = false }))
            {
                writer.WriteStartObject();
                writer.WriteString("contract", ContentContract);
                writer.WriteStartObject("identity");
                writer.WriteNumber("directABI", profile.DirectAbi);
                writer.WriteString("translatorClosure", profile.TranslatorClosure);
                writer.WriteString("runtimeCompilerABI", profile.RuntimeCompilerAbi);
                writer.WriteString("profileHash", profile.ProfileHash);
                writer.WriteEndObject();
                writer.WriteString("kind", program.Kind);
                if (program.Kind == "compute")
                    {
                    writer.WriteBoolean("hasUnalignedStorageBuffer", program.Compute.HasUnalignedStorageBuffer);
                    var c0 = allocator.Bindings.Where(b => b.Stage == "cs" && b.Kind == "uniform" && b.Slot == 0).ToArray();
                    int[][] reads = c0.Length == 0 ? Array.Empty<int[]>() : c0.Length != 1 ? null :
                        DirectComputeC0Reads.Analyze(translated[0].shader.BinaryCode, c0[0].Set, c0[0].Binding);
                    writer.WritePropertyName("c0Reads");
                    if (reads == null) writer.WriteNullValue();
                    else
                    {
                        writer.WriteStartArray();
                        foreach (int[] range in reads)
                        {
                            writer.WriteStartArray();
                            writer.WriteNumberValue(range[0]); writer.WriteNumberValue(range[1]);
                            writer.WriteEndArray();
                        }
                        writer.WriteEndArray();
                    }
                }
                if (program.Kind == "compute")
                {
                    int[][] supportReads = DirectComputeSupportReads.Analyze(translated[0].shader.BinaryCode,
                        0, profile.Support.Binding, profile.Support.RequiredSize, out var supportScaleRanges);
                    writer.WritePropertyName("supportReads");
                    if (supportReads == null) writer.WriteNullValue();
                    else
                    {
                        writer.WriteStartArray();
                        foreach (int[] range in supportReads)
                        {
                            writer.WriteStartArray();
                            writer.WriteNumberValue(range[0]); writer.WriteNumberValue(range[1]);
                            writer.WriteEndArray();
                        }
                        writer.WriteEndArray();
                    }
                    writer.WritePropertyName("supportScaleRanges");
                    if (supportScaleRanges == null) writer.WriteNullValue();
                    else
                    {
                        writer.WriteStartArray();
                        foreach (var range in supportScaleRanges)
                        {
                            writer.WriteStartObject();
                            writer.WriteString("name", range.Name);
                            writer.WriteNumber("offset", range.Offset);
                            writer.WriteNumber("stride", range.Stride);
                            writer.WriteNumber("count", range.Count);
                            writer.WriteNumber("elementBytes", range.ElementBytes);
                            writer.WriteEndObject();
                        }
                        writer.WriteEndArray();
                    }
                }
                writer.WriteStartArray("stages");
                foreach (var (stage, shader, accessor) in translated)
                {
                    writer.WriteStartObject();
                    writer.WriteString("stage", stage.Stage);
                    writer.WriteString("codeSource", stage.CodeSource);
                    writer.WriteString("controlSource", stage.ControlSource);
                    writer.WriteString("c1Source", stage.C1Source);
                    writer.WriteNumber("constantBufferUse", stage.ConstantBufferUse);
                    writer.WriteBase64String("spirv", shader.BinaryCode);
                    writer.WriteString("spirvSha256", DirectStage.Sha256Hex(shader.BinaryCode));
                    WriteInfo(writer, shader.Info);
                    writer.WriteNumber("stateReadFlags", accessor.StateReadFlags);
                    writer.WriteNumber("attributeReadMask", accessor.AttributeReadMask);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteStartArray("bindings");
                foreach (DirectBinding binding in allocator.Bindings)
                {
                    writer.WriteStartObject();
                    writer.WriteString("stage", binding.Stage);
                    writer.WriteString("kind", binding.Kind);
                    writer.WriteString("source", binding.Source);
                    writer.WriteNumber("set", binding.Set);
                    writer.WriteNumber("binding", binding.Binding);
                    writer.WriteNumber("count", binding.Count);
                    if (binding.Slot >= 0) writer.WriteNumber("slot", binding.Slot);
                    if (binding.Kind == "storage")
                    {
                        // Resolved and uniqueness-checked by ResolveStorageSlots above.
                        writer.WriteBoolean("rw", binding.Writable);
                        writer.WriteNumber("cbSlot", binding.CbSlot);
                        writer.WriteNumber("cbOffset", binding.CbOffset);
                        if (binding.NvnSlot >= 0) writer.WriteNumber("nvnSlot", binding.NvnSlot);
                        else if (binding.UboPtr >= 0) writer.WriteNumber("uboPtr", binding.UboPtr);
                        else if (binding.UboSlot >= 0)
                        {
                            writer.WriteNumber("uboSlot", binding.UboSlot);
                            writer.WriteNumber("uboOffset", binding.UboOffset);
                        }
                    }
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteStartObject("supportLayout");
                writer.WriteNumber("requiredSize", profile.Support.RequiredSize);
                writer.WriteNumber("binding", profile.Support.Binding);
                writer.WriteString("hash", profile.Support.Hash);
                writer.WriteStartArray("fields");
                foreach (SupportLayoutField field in profile.Support.Fields)
                {
                    writer.WriteStartObject();
                    writer.WriteString("name", field.Name);
                    writer.WriteNumber("offset", field.Offset);
                    writer.WriteNumber("size", field.Size);
                    writer.WriteString("owner", field.Owner);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteEndObject();
                writer.WriteStartArray("dependencies");
                foreach (DirectQuery query in queries)
                {
                    writer.WriteStartObject();
                    writer.WriteString("method", query.Method);
                    writer.WriteString("arguments", query.Arguments);
                    writer.WriteString("value", query.Value);
                    writer.WriteString("source", query.Source);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteEndObject();
            }
            return stream.ToArray();
        }

        private static void WriteInfo(Utf8JsonWriter writer, ShaderProgramInfo info)
        {
            writer.WriteStartObject("info");
            writer.WriteString("stage", StageName(info.Stage));
            writer.WriteNumber("geometryVerticesPerPrimitive", info.GeometryVerticesPerPrimitive);
            writer.WriteNumber("geometryMaxOutputVertices", info.GeometryMaxOutputVertices);
            writer.WriteNumber("threadsPerInputPrimitive", info.ThreadsPerInputPrimitive);
            writer.WriteBoolean("usesFragCoord", info.UsesFragCoord);
            writer.WriteBoolean("usesInstanceId", info.UsesInstanceId);
            writer.WriteBoolean("usesDrawParameters", info.UsesDrawParameters);
            writer.WriteBoolean("usesRtLayer", info.UsesRtLayer);
            writer.WriteNumber("clipDistancesWritten", info.ClipDistancesWritten);
            writer.WriteNumber("fragmentOutputMap", info.FragmentOutputMap);
            writer.WriteStartArray("cbuffers");
            foreach (BufferDescriptor b in info.CBuffers) WriteBuffer(writer, b);
            writer.WriteEndArray();
            writer.WriteStartArray("sbuffers");
            foreach (BufferDescriptor b in info.SBuffers) WriteBuffer(writer, b);
            writer.WriteEndArray();
            writer.WriteStartArray("textures");
            foreach (TextureDescriptor t in info.Textures) WriteTexture(writer, t);
            writer.WriteEndArray();
            writer.WriteStartArray("images");
            foreach (TextureDescriptor t in info.Images) WriteTexture(writer, t);
            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        private static void WriteBuffer(Utf8JsonWriter writer, BufferDescriptor descriptor)
        {
            writer.WriteStartObject();
            writer.WriteNumber("set", descriptor.Set);
            writer.WriteNumber("binding", descriptor.Binding);
            writer.WriteNumber("slot", descriptor.Slot);
            writer.WriteNumber("sbCbSlot", descriptor.SbCbSlot);
            writer.WriteNumber("sbCbOffset", descriptor.SbCbOffset);
            writer.WriteNumber("flags", (int)descriptor.Flags);
            writer.WriteEndObject();
        }

        private static void WriteTexture(Utf8JsonWriter writer, TextureDescriptor descriptor)
        {
            writer.WriteStartObject();
            writer.WriteNumber("set", descriptor.Set);
            writer.WriteNumber("binding", descriptor.Binding);
            writer.WriteString("type", descriptor.Type.ToString());
            writer.WriteString("format", descriptor.Format.ToString());
            writer.WriteNumber("cbufSlot", descriptor.CbufSlot);
            writer.WriteNumber("handleIndex", descriptor.HandleIndex);
            writer.WriteNumber("arrayLength", descriptor.ArrayLength);
            writer.WriteBoolean("separate", descriptor.Separate);
            writer.WriteNumber("flags", (int)descriptor.Flags);
            writer.WriteEndObject();
        }

        private static string StageName(ShaderStage stage) => stage switch
        {
            ShaderStage.Vertex => "vs",
            ShaderStage.TessellationControl => "tcs",
            ShaderStage.TessellationEvaluation => "tes",
            ShaderStage.Geometry => "gs",
            ShaderStage.Fragment => "fs",
            ShaderStage.Compute => "cs",
            _ => throw new DirectCompileException("UNSUPPORTED_PROGRAM", "unknown shader stage " + stage),
        };

        private static byte[] WriteEnvelope(ulong session, ulong generation, string requestId,
            byte[] content, string contentHash)
        {
            using var stream = new MemoryStream();
            using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = false }))
            {
                writer.WriteStartObject();
                writer.WriteString("schema", ResultSchema);
                writer.WriteStartObject("transport");
                writer.WriteNumber("session", session);
                writer.WriteNumber("generation", generation);
                writer.WriteString("requestId", requestId);
                writer.WriteEndObject();
                writer.WriteString("contentHash", contentHash);
                writer.WritePropertyName("content");
                writer.WriteRawValue(content);
                writer.WriteEndObject();
            }
            return stream.ToArray();
        }
    }

    public readonly struct SharedDescriptor
    {
        public readonly int Set, Binding;
        // SPIR-V 存储类：0=UniformConstant（纹理/采样器）、2=Uniform（UBO）、12=StorageBuffer。
        // 只读这一个字就能区分「UBO 还是纹理」，不必建类型图。
        public readonly int StorageClass;
        public readonly string Name;
        public SharedDescriptor(int set, int binding, int storageClass = -1, string name = null)
        { Set = set; Binding = binding; StorageClass = storageClass; Name = name ?? ""; }
    }

    // 只读 SPIR-V 的 descriptor decoration；不替代完整验证器，只核对集合/绑定来源。
    public static class SpirvReflection
    {
        // 存储类名（仅用于错误信息）：0=纹理/采样器，2=UBO，12=SSBO。
        // 描述符种类校验的唯一实现：SPIR-V 存储类与分配器 kind 必须一致（两边都明确才判）。
        // 真机 fp_c1 那类“Bind group info mismatches the shader source”应当在这里就被拦住。
        // enabled=false 跳过（请求 JSON kindCheck=false；native 由调试门 + debug.s2a.kind_check 决定），
        // 供单变量对照，不读进程环境变量。
        public static void CheckDescriptorKind(string stage, SharedDescriptor pair, string allocatorKind, bool enabled = true)
        {
            if (!enabled) return;
            int expected = allocatorKind == "uniform" || allocatorKind == "c1" || allocatorKind == "support" ? 2
                         : allocatorKind == "texture" || allocatorKind == "image" ? 0
                         : -1;
            if (expected < 0 || pair.StorageClass < 0 || pair.StorageClass == expected) return;
            string sym = string.IsNullOrEmpty(pair.Name) ? ("#" + pair.Set + ":" + pair.Binding) : pair.Name;
            throw new DirectCompileException("SPIRV_INVALID",
                stage + ": descriptor " + pair.Set + ":" + pair.Binding + " (" + sym + ") 种类不匹配：" +
                "SPIR-V 存储类=" + pair.StorageClass + "（" + StorageClassName(pair.StorageClass) + "）" +
                "，分配器 kind=" + allocatorKind + "（期望 " + expected + "=" + StorageClassName(expected) + "）");
        }

        public static string StorageClassName(int storageClass)
        {
            switch (storageClass)
            {
                case 0: return "UniformConstant(纹理/采样器)";
                case 2: return "Uniform(UBO)";
                case 12: return "StorageBuffer(SSBO)";
                default: return "未知";
            }
        }

        public static IEnumerable<SharedDescriptor> Descriptors(uint[] words)
        {
            var decorations = new Dictionary<uint, (int set, int binding)>();
            var variables = new List<(uint id, int storage)>();
            var names = new Dictionary<uint, string>();
            for (int i = 5; i < words.Length;)
            {
                int count = (int)(words[i] >> 16), opcode = (int)(words[i] & 0xFFFF);
                if (count == 0 || i + count > words.Length) yield break;
                if (opcode == 71 && count >= 4)
                {
                    uint target = words[i + 1];
                    int kind = (int)words[i + 2];
                    uint value = words[i + 3];
                    if (kind == 34 || kind == 33)
                    {
                        decorations.TryGetValue(target, out var entry);
                        if (kind == 34) entry.set = (int)value; else entry.binding = (int)value;
                        decorations[target] = entry;
                    }
                }
                else if (opcode == 59 && count >= 4)
                {
                    // OpVariable: result type, result id, storage class
                    variables.Add((words[i + 2], (int)words[i + 3]));
                }
                else if (opcode == 5 && count >= 3)
                {
                    // OpName: target id + 字符串（驱动报错里的符号名就是它，例如 fp_c1）
                    uint target = words[i + 1];
                    int chars = (count - 2) * 4;
                    var raw = new byte[chars];
                    Buffer.BlockCopy(words, (i + 2) * 4, raw, 0, chars);
                    int end = Array.IndexOf(raw, (byte)0);
                    names[target] = System.Text.Encoding.UTF8.GetString(raw, 0, end < 0 ? chars : end);
                }
                i += count;
            }
            var seen = new HashSet<(int, int)>();
            foreach (var v in variables)
                if (decorations.TryGetValue(v.id, out var entry) && seen.Add((entry.set, entry.binding)))
                    yield return new SharedDescriptor(entry.set, entry.binding, v.storage,
                                                      names.TryGetValue(v.id, out var nm) ? nm : null);
        }
    }

    // 翻译只用到 GetCapabilities；其余 IRenderer 成员一律显式 NotSupported，不能靠反射代理。
    internal sealed class ProfileOnlyRenderer : IRenderer
    {
        private readonly Capabilities _caps;
        public ProfileOnlyRenderer(Capabilities caps) { _caps = caps; }
        public event EventHandler<ScreenCaptureImageInfo> ScreenCaptured { add { } remove { } }
        public bool PreferThreading => false;
        public IPipeline Pipeline => throw new NotSupportedException(nameof(Pipeline));
        public IWindow Window => throw new NotSupportedException(nameof(Window));
        public uint ProgramCount => 0;
        public Capabilities GetCapabilities() => _caps;
        public void Dispose() { }
        public void BackgroundContextAction(Action action, bool alwaysBackground = false) => throw new NotSupportedException(nameof(BackgroundContextAction));
        public BufferHandle CreateBuffer(int size, BufferAccess access = BufferAccess.Default) => throw new NotSupportedException(nameof(CreateBuffer));
        public BufferHandle CreateBuffer(nint pointer, int size) => throw new NotSupportedException(nameof(CreateBuffer));
        public BufferHandle CreateBufferSparse(ReadOnlySpan<BufferRange> storageBuffers) => throw new NotSupportedException(nameof(CreateBufferSparse));
        public IImageArray CreateImageArray(int size, bool isBuffer) => throw new NotSupportedException(nameof(CreateImageArray));
        public IProgram CreateProgram(ShaderSource[] shaders, ShaderInfo info) => throw new NotSupportedException(nameof(CreateProgram));
        public ISampler CreateSampler(SamplerCreateInfo info) => throw new NotSupportedException(nameof(CreateSampler));
        public ITexture CreateTexture(TextureCreateInfo info) => throw new NotSupportedException(nameof(CreateTexture));
        public ITextureArray CreateTextureArray(int size, bool isBuffer) => throw new NotSupportedException(nameof(CreateTextureArray));
        public bool PrepareHostMapping(nint address, ulong size) => throw new NotSupportedException(nameof(PrepareHostMapping));
        public void CreateSync(ulong id, bool strict) => throw new NotSupportedException(nameof(CreateSync));
        public void DeleteBuffer(BufferHandle buffer) => throw new NotSupportedException(nameof(DeleteBuffer));
        public PinnedSpan<byte> GetBufferData(BufferHandle buffer, int offset, int size) => throw new NotSupportedException(nameof(GetBufferData));
        public ulong GetCurrentSync() => throw new NotSupportedException(nameof(GetCurrentSync));
        public HardwareInfo GetHardwareInfo() => throw new NotSupportedException(nameof(GetHardwareInfo));
        public IProgram LoadProgramBinary(byte[] programBinary, bool hasFragmentShader, ShaderInfo info) => throw new NotSupportedException(nameof(LoadProgramBinary));
        public void SetBufferData(BufferHandle buffer, int offset, ReadOnlySpan<byte> data) => throw new NotSupportedException(nameof(SetBufferData));
        public void UpdateCounters() => throw new NotSupportedException(nameof(UpdateCounters));
        public void PreFrame() => throw new NotSupportedException(nameof(PreFrame));
        public ICounterEvent ReportCounter(CounterType type, EventHandler<ulong> resultHandler, float divisor, bool hostReserved) => throw new NotSupportedException(nameof(ReportCounter));
        public void ResetCounter(CounterType type) => throw new NotSupportedException(nameof(ResetCounter));
        public void WaitSync(ulong id) => throw new NotSupportedException(nameof(WaitSync));
        public void Initialize(GraphicsDebugLevel logLevel) => throw new NotSupportedException(nameof(Initialize));
        public void SetInterruptAction(Action<Action> interruptAction) => throw new NotSupportedException(nameof(SetInterruptAction));
        public void Screenshot() => throw new NotSupportedException(nameof(Screenshot));
    }
}
