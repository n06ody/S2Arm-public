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
using System.Text;
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

        [ThreadStatic] private static string _requestSha;
        private static string RequestSha() => _requestSha ?? "unknown";

        // 翻译标志按阶段，与出库链同口径（graphics/tools/direct/dsh/Dsh.cs：debug = gs / cs，11-07 起）：顶点、片元关 DebugMode，
        //   AST 表达式传播打开，产物不再满是局部变量读写（#1774：同一延迟光照合成片元，现编 62,841 字 vs 出库 30,099 字）；
        //   几何保留（关掉后翻译慢 2.4 倍，首遇现编更卡）；计算走 ShaderCache.DecodeComputeShader，内部带 DebugMode，不经这里。
        //   原先三段都沿用 Ryujinx ShaderCache.DefaultFlags（= DebugMode），是出库链 11-07 改口径时没跟上的遗留。
        internal static TranslationFlags GraphicsFlags(string stage) => stage == "gs" ? ShaderCache.DefaultFlags : ShaderCache.DefaultFlags & ~TranslationFlags.DebugMode;

        public sealed class PhaseProbe
        {
            private readonly Func<long> _cpuNow;

            public PhaseProbe(Func<long> cpuNow) => _cpuNow = cpuNow;
            public string RequestSha { get; internal set; }
            public long SpirvWallTicks { get; private set; }
            public long SpirvCpuNs { get; private set; }
            public (long WallTicks, long CpuNs) Parse, Translate, Reflect, Serialize, InputCopy, Emit;

            public (long WallTicks, long CpuNs) Now() =>
                (System.Diagnostics.Stopwatch.GetTimestamp(), _cpuNow());

            public (long WallTicks, long CpuNs) Since((long WallTicks, long CpuNs) start)
            {
                var end = Now();
                return (end.WallTicks - start.WallTicks, end.CpuNs - start.CpuNs);
            }

            internal void BeginSpirv()
            {
                TranslatorContext.SetSpirvPhaseSample((wall, cpu) =>
                {
                    SpirvWallTicks += wall;
                    SpirvCpuNs += cpu;
                }, _cpuNow);
            }
        }

        public sealed class ResultTiming
        {
            public long Metadata, Encode;
        }

        [ThreadStatic] public static ResultTiming Timing;

        public static byte[] Compile(byte[] requestUtf8) => Compile(requestUtf8, null);

        public static byte[] Compile(byte[] requestUtf8, PhaseProbe probe) => CompileResult(requestUtf8, probe, false);

        public static byte[] CompileBinary(byte[] requestUtf8) => CompileBinary(requestUtf8, null);

        public static byte[] CompileBinary(byte[] requestUtf8, PhaseProbe probe) => CompileResult(requestUtf8, probe, true);

        public static byte[] EncodeLegacyResult(ReadOnlySpan<byte> result) => DirectResultWriter.LegacyJson(result);

        private static byte[] CompileResult(byte[] requestUtf8, PhaseProbe probe, bool binary)
        {
            if (probe == null) return CompileCore(requestUtf8, null, binary);
            probe.BeginSpirv();
            try { return CompileCore(requestUtf8, probe, binary); }
            finally { TranslatorContext.SetSpirvPhaseSample(null, null); }
        }

        private static byte[] CompileCore(byte[] requestUtf8, PhaseProbe probe, bool binary)
        {
            // 诊断：拒收行带上请求 sha，真机上就能把拒收与同一条请求对上（不用另开请求落盘开关）。
            var parseStart = probe?.Now() ?? default;
            _requestSha = Convert.ToHexStringLower(Sha256.HashData(requestUtf8));
            if (probe != null) probe.RequestSha = _requestSha;
            using JsonDocument document = JsonStrict.Parse(requestUtf8, MaxRequestBytes);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new DirectCompileException("INVALID_JSON", "request root is not an object");
            JsonStrict.OnlyKeys(root, "request", "schema", "session", "generation", "requestId",
                "directABI", "translatorClosure", "runtimeCompilerABI", "sourceParserVersion", "kindCheck", "profile", "program", "requiresLinkedVaryingInterface", "linkMapDiag");
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
            bool linked = JsonStrict.OptionalBool(root, "requiresLinkedVaryingInterface", "request", false);
            if (linked && (program.Kind != "graphics" || program.Stages.Length != 2 ||
                program.StageOf("vs") == null || program.StageOf("fs") == null))
                throw new DirectCompileException("UNSUPPORTED_PROGRAM", "linked varyings require exactly VS and FS");
            // 诊断（native 开关 nvk_link_map_diag 开时才带）：结果信封外层附两侧实际发射的「逻辑 location → 宿主 Location」，
            // 不进 content / contentHash，翻译与 SPIR-V 不受影响。只对配对 VS+FS 有意义，单独带视为请求错误。
            bool linkMapDiag = JsonStrict.OptionalBool(root, "linkMapDiag", "request", false);
            if (linkMapDiag && !linked)
                throw new DirectCompileException("UNSUPPORTED_PROGRAM", "linkMapDiag requires requiresLinkedVaryingInterface");
            if (program.StageOf("tcs") != null && !profile.TessellationShader)
                throw new DirectCompileException("UNSUPPORTED_PROGRAM", "tessellationShader is not enabled in the profile");
            if (program.StageOf("gs") is DirectStage gs)
            {
                if (!profile.Caps.SupportsGeometryShader)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", "geometryShader is not enabled in the profile");
                // 邻接图元：Vulkan 有原生邻接输入（翻译器按 InputTopology 出 InputLinesAdjacency / InputTrianglesAdjacency 执行模式），不拒。
                // 直通型几何头（NV passthrough）：宿主不支持该扩展时翻译器生成等价的普通 GS（Ryujinx EmitterContext 的 passthrough 模拟，
                //   以 profile 的 SupportsGeometryShaderPassthrough 为准），不拒。只校验这一段确实是几何着色器头。
                uint header = BinaryPrimitives.ReadUInt32LittleEndian(gs.Code);
                if (((header >> 10) & 15) != 4)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM", "GS slot does not hold a geometry shader header");
            }
            _ = sourceParserVersion;
            if (probe != null) probe.Parse = probe.Since(parseStart);
            var translateStart = probe?.Now() ?? default;

            var queries = new List<DirectQuery>();
            var allocator = new DirectAllocator(profile) { RequestKey = requestId };
            var accessors = new List<DirectGpuAccessor>();
            var translated = new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>();
            IReadOnlyDictionary<int, int> linkMapVs = null, linkMapFs = null;
            using var context = new GpuContext(new ProfileOnlyRenderer(profile.TranslationCaps), new DirtyHacks(Array.Empty<ulong>()))
            {
                Capabilities = profile.TranslationCaps,
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
                    int limit = profile.Caps.MaximumComputeSharedMemorySize;
                    if (shader.BinaryCode != null && program.Compute.SharedMemoryBytes > limit)
                    {
                        try
                        {
                            int shared = DirectSharedSpill.WorkgroupBytes(shader.BinaryCode);
                            if (shared < 0 || shared > limit)
                            {
                                // 先量出要挪的字节数再占绑定号：Rewrite 用的就是这一号
                                var pair = allocator.Spill("cs", 0);
                                DirectSharedSpill.Result spill = DirectSharedSpill.Rewrite(shader.BinaryCode, limit, pair.SetIndex, pair.Binding)
                                    ?? throw new DirectCompileException("UNSUPPORTED_PROGRAM", "workgroup memory could not be measured for spilling");
                                allocator.Bindings[^1].SpillBytesPerGroup = spill.BytesPerGroup;
                                shader = new ShaderProgram(shader.Info, shader.Language, spill.Spirv);
                            }
                        }
                        catch (DirectCompileException e)
                        {
                            throw new DirectCompileException(e.Code, "cs: sharedMemoryBytes " + program.Compute.SharedMemoryBytes +
                                " exceeds device compute shared memory limit " + limit + "; spill failed: " + e.Message);
                        }
                    }
                    translated.Add((cs, shader, accessor));
                }
                else
                {
                    // NVN 槽序与执行序不同；解码后按实际管线顺序串联。
                    var ordered = new[] { (name: "vs", index: 1), (name: "tcs", index: 2), (name: "tes", index: 3), (name: "gs", index: 4), (name: "fs", index: 5) };
                    var contexts = new List<(DirectStage stage, DirectGpuAccessor accessor, TranslatorContext context)>();
                    foreach (var item in ordered)
                    {
                        DirectStage stage = program.StageOf(item.name);
                        if (stage == null) continue;
                        var accessor = new DirectGpuAccessor(profile, program, stage, item.index, allocator, queries, linked);
                        accessors.Add(accessor);
                        contexts.Add((stage, accessor, ShaderCache.DecodeGraphicsShader(accessor, TargetApi.Vulkan, GraphicsFlags(item.name), 0)));
                    }
                    for (int i = contexts.Count - 2; i >= 0; i--)
                    {
                        // 直接接片元的细分求值阶段使用实际的片元输入位置。
                        // 其余细分接口沿用 Ryujinx 的阶段链接（含逐控制点与逐补丁属性）。
                        if (contexts[i].stage.Stage == "gs" ||
                            (contexts[i].stage.Stage == "tes" && contexts[i + 1].stage.Stage == "fs") ||
                            (linked && contexts[i].stage.Stage == "vs" && contexts[i + 1].stage.Stage == "fs"))
                            contexts[i].context.S2ASetNextStageActual(contexts[i + 1].context);
                        else contexts[i].context.SetNextStage(contexts[i + 1].context);
                    }
                    foreach (var item in contexts) translated.Add((item.stage, item.context.Translate(), item.accessor));
                    if (linkMapDiag)
                    {
                        linkMapVs = contexts.First(c => c.stage.Stage == "vs").context.S2AVaryingLocationMap(true);
                        linkMapFs = contexts.First(c => c.stage.Stage == "fs").context.S2AVaryingLocationMap(false);
                    }
                }
            }
            catch (DirectCompileException) { throw; }
            catch (Exception error)
            {
                throw new DirectCompileException("TRANSLATION", error.GetType().Name + ": " + error.Message);
            }

            if (probe != null) probe.Translate = probe.Since(translateStart);
            var reflectStart = probe?.Now() ?? default;
            foreach (DirectGpuAccessor accessor in accessors)
                if (accessor.Diagnostics.Count != 0)
                    throw new DirectCompileException("TRANSLATION_DIAGNOSTIC",
                        accessor.Diagnostics[0] + " (stage " + accessorStage(accessor, translated) + ")");

            foreach (var entry in translated)
                ValidateSpirv(entry.stage, entry.shader, entry.accessor, allocator, profile, kindCheck, program.StageOf("gs") != null);
            ResolvePoolSlots(translated, allocator);
            ResolveStorageSlots(translated, allocator);

            if (probe != null) probe.Reflect = probe.Since(reflectStart);
            var serialStart = probe?.Now() ?? default;
            bool v2 = allocator.Bindings.Any(b => b.SamplerSlot >= 0 || b.ConstSampler >= 0 || b.Pool != null);   // 新字段（samp_slot / constSampler / pool）都按 v2 写：v1 宿主不认就整份拒
            long start = Timing == null ? 0 : System.Diagnostics.Stopwatch.GetTimestamp();
            byte[] content = WriteContent(program, profile, translated, allocator, queries, v2, binary);
            string contentHash = binary ? null : "sha256-" + Convert.ToHexStringLower(Sha256.HashData(content));
            byte[] envelope = WriteEnvelope(session, generation, requestId, content, contentHash, v2, binary, linkMapVs, linkMapFs);
            if (Timing != null) Timing.Encode = System.Diagnostics.Stopwatch.GetTimestamp() - start - Timing.Metadata;
            if (probe != null) probe.Serialize = probe.Since(serialStart);
            return envelope;
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
            if (shared > 48 * 1024)
                throw new DirectCompileException("CONTROL_INVALID", stage.Stage + ": control shared memory exceeds 48 KiB");
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
            ValidateDescriptorIndexing(stage.Stage, words, profile);
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

        // profile 与 SPIR-V 描述符索引能力必须一致（C 片 C1）：
        // 生成器在 MarkNonUniform 里一次声明 SPV_EXT_descriptor_indexing + ShaderNonUniform +
        // SampledImageArrayNonUniformIndexing + StorageImageArrayNonUniformIndexing（Instructions.cs:611）。
        //   · profile 关（设备四位不齐 / 开发期隔离）：一个都不许出现；
        //   · profile 开：要么都没有（这支着色器没有非一致访问），要么四个齐全——缺一个就是生成器/profile 漂移。
        public static void ValidateDescriptorIndexing(string stage, uint[] words, DirectProfile profile)
        {
            bool ext = false, shaderNonUniform = false, sampled = false, storage = false;
            for (int i = 5; i < words.Length;)
            {
                int count = (int)(words[i] >> 16);
                int opcode = (int)(words[i] & 0xFFFF);
                if (count == 0 || i + count > words.Length)
                    throw new DirectCompileException("SPIRV_INVALID", stage + ": truncated instruction");
                if (opcode == (int)Spv.Specification.Op.OpCapability && count >= 2)
                {
                    switch ((Spv.Specification.Capability)words[i + 1])
                    {
                        case Spv.Specification.Capability.ShaderNonUniform: shaderNonUniform = true; break;
                        case Spv.Specification.Capability.SampledImageArrayNonUniformIndexing: sampled = true; break;
                        case Spv.Specification.Capability.StorageImageArrayNonUniformIndexing: storage = true; break;
                        default: break;
                    }
                }
                else if (opcode == (int)Spv.Specification.Op.OpExtension && count >= 2)
                {
                    if (SpirvLiteralIs(words, i + 1, count - 1, "SPV_EXT_descriptor_indexing")) ext = true;
                }
                else if (opcode != (int)Spv.Specification.Op.OpExtInstImport)
                {
                    break;   // 能力 / 扩展段结束
                }
                i += count;
            }
            bool any = ext || shaderNonUniform || sampled || storage;
            if (!profile.Caps.SupportsShaderNonUniformIndexing)
            {
                if (any)
                    throw new DirectCompileException("SPIRV_INVALID",
                        stage + ": descriptor indexing declared while the enabled profile disables it");
                return;
            }
            if (any && !(ext && shaderNonUniform && sampled && storage))
                throw new DirectCompileException("SPIRV_INVALID",
                    stage + ": descriptor indexing declaration is incomplete (ext=" + ext +
                    " shaderNonUniform=" + shaderNonUniform + " sampled=" + sampled + " storage=" + storage + ")");
        }

        private static bool SpirvLiteralIs(uint[] words, int start, int count, string want)
        {
            if (count <= 0 || (long)count * 4 < want.Length + 1) return false;
            for (int i = 0; i < want.Length; i++)
            {
                byte b = (byte)(words[start + i / 4] >> ((i % 4) * 8));
                if (b != (byte)want[i]) return false;
            }
            return (byte)(words[start + want.Length / 4] >> ((want.Length % 4) * 8)) == 0;
        }

        // 池槽位只能由反射结果决定：翻译器先建 texture binding、后 RegisterTexture，
        // 按调用顺序猜会把这几个槽位全指到第一张纹理上。cbufSlot >= 0 的句柄来自常量
        // 缓冲：c2 只有同句柄存在直接绑定事实时才按 NVN c2→组合槽放行；游戏 UBO（c3..c18）
        // 的单个组合句柄给出 uboSlot / uboOffset（见下）；其余 cbuf 路径和 separate 采样器
        // 仍未实现，明确报红，不写死 0 让运行时绑错。
        internal const int UserUboFirstCbuf = 3, UserUboLastCbuf = 18, UserUboMaxBytes = 0x10000;
        // 类型 3 句柄的采样器号是立即数 << 20，只占句柄高 12 位：采样器池下标 0..4095。
        private const int MaxConstSampler = 4095;
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
                // 整池数组：翻译器对追不到槽的句柄（算术 / 共享内存 / 全局内存 / phi 汇合拼出的）回退成按池编号读的两个数组
                //   （compat BindlessElimination：贴图数组长 = max(2, 贴图池长)、采样器数组（类型 None）长 = max(2, 采样器池长)，
                //   句柄 PackOffsets(0, 0, Direct) = 0x40000000、cbuf 0）。这不是从 c0 / c2 读句柄，不需要逐句柄事实；宿主按 NVN 池建快照绑整表。
                //   只认长度恰好等于请求里的池长的那一对（别的 cbuf 0 数组照旧走下面的拒绝），贴图缓冲不在池里（按槽绑）
                if (binding.Kind == "texture" && binding.Source == "texture" && resolved.CbufSlot == 0 &&
                    resolved.HandleIndex == TextureHandle.PackOffsets(0, 0, TextureHandleType.Direct) &&
                    resolved.ArrayLength > 1 && binding.Count == resolved.ArrayLength)
                {
                    bool sampPool = (resolved.Type & SamplerType.Mask) == SamplerType.None;
                    int poolLength = Math.Max(2, sampPool ? entry.accessor.SamplerPoolLength : entry.accessor.TexturePoolLength);
                    // 布局项的 count 是 16 位，且要多一项表尾 dummy（nvn_backend.h nvn_gpu_pool_*）：池长到 0xffff 及以上表达不了，落到下面照旧具名拒
                    if (resolved.ArrayLength == poolLength && poolLength < 0xffff)
                    {
                        binding.Pool = sampPool ? "sampler" : "texture";
                        binding.Slot = -1;
                        continue;
                    }
                }
                if (binding.Kind == "texture" && !resolved.Separate &&
                    entry.accessor.TrySeparateC2(resolved.HandleIndex, resolved.CbufSlot, out int texWord, out int samplerSlot))
                {
                    if (binding.Count != 1 || resolved.ArrayLength > 1)
                        throw new DirectCompileException("UNSUPPORTED_PROGRAM", binding.Stage + ": separate sampler arrays are not implemented");
                    binding.Slot = texWord;
                    binding.SamplerSlot = samplerSlot;
                    continue;
                }
                if (binding.Kind == "texture" && !resolved.Separate &&
                    entry.accessor.TrySeparateConstC2(resolved.HandleIndex, resolved.CbufSlot, out int constTexWord, out int constSampler))
                {
                    // #1594：贴图字在 c2 分离贴图表、采样器是着色器里的立即数采样器池下标（类型 3）。结果给 slot = 贴图字、constSampler = 池下标
                    //   （可选字段，按 v2 写）；宿主 / 回放按池下标直接取采样器（同 NVN_RB_SAMPI），不经 c2 采样器表。
                    //   绝不只填 slot：宿主会把「没有 samp_slot」读成采样器取同字高位（= 池里第 0 项），静默错绑。
                    //   所以表达不了的形态——数组、池下标超出句柄里 12 位的范围——具名报未接，什么都不填。
                    //   计算阶段（#1594 片 7）与图形阶段同一份合同：宿主计算侧解析与回放派发都按同一个 const_samp_id 取采样器。
                    //   计算阶段的游戏 UBO 来源是下一个分支（#1594 片 10）。
                    string shape = binding.Count != 1 || resolved.ArrayLength > 1 ? "constant sampler array" :
                                   constSampler > MaxConstSampler ? "constant sampler index above 4095" : null;
                    if (shape != null)
                        throw new DirectCompileException("UNSUPPORTED_PROGRAM",
                            WithHandleDetail(binding.Stage + ": texture handle from cbuf slot " + resolved.CbufSlot + " is not implemented (" + shape + ")",
                                HandleDetailFields(allocator, binding, resolved, messageHasShape: false)));
                    binding.Slot = constTexWord;
                    binding.ConstSampler = constSampler;
                    continue;
                }
                if (binding.Kind == "texture" && !resolved.Separate &&
                    entry.accessor.TrySeparateConstUbo(resolved.HandleIndex, resolved.CbufSlot, out int constUboWord, out int constUboCbuf, out int constUboSampler))
                {
                    // #1594 片 3：贴图字在游戏 UBO（c3..c18）、采样器是着色器里的立即数采样器池下标（类型 3）。结果给游戏 UBO 来源 uboSlot（NVN UBO 号 = cbuf − 3）/
                    //   uboOffset（贴图字的字节偏移 = 字 × 4）、没有 slot，再加 constSampler = 池下标（可选字段，按 v2 写）。
                    //   绝不只给 uboSlot / uboOffset：宿主对这种写法按「组合句柄」把采样器取成同一个字的高 12 位（那里不是采样器号），静默错绑。
                    //   形态判据同 c2 的类型 3：数组、池下标超出句柄里 12 位的范围，具名报未接，什么都不填。
                    //   计算阶段（#1594 片 10）与图形阶段同一份合同：宿主计算侧解析（compute_const_sampler）与回放派发（rp_dispatch 的 NVN_RB_TEXU）
                    //   都按同一个 const_samp_id 取采样器；游戏 UBO 来源的组合句柄（非类型 3）在计算阶段仍由宿主具名拒，这里不碰。
                    //   字偏移不用另查 64 KiB：类型 3 句柄的字只有 14 位（0..16383 = 字节 0..65532），装不下 64 KiB 以外的字。
                    string shape = binding.Count != 1 || resolved.ArrayLength > 1 ? "constant sampler array" :
                                   constUboSampler > MaxConstSampler ? "constant sampler index above 4095" : null;
                    if (shape != null)
                        throw new DirectCompileException("UNSUPPORTED_PROGRAM",
                            WithHandleDetail(binding.Stage + ": texture handle from cbuf slot " + resolved.CbufSlot + " is not implemented (" + shape + ")",
                                HandleDetailFields(allocator, binding, resolved, messageHasShape: false)));
                    binding.Slot = -1;
                    binding.UboSlot = constUboCbuf - UserUboFirstCbuf;
                    binding.UboOffset = constUboWord * 4;
                    binding.ConstSampler = constUboSampler;
                    continue;
                }
                if (resolved.CbufSlot >= UserUboFirstCbuf && resolved.CbufSlot <= UserUboLastCbuf)
                {
                    // 句柄在游戏自己的 UBO 里（#999 第 2 片）：照 Ryujinx（TextureBindingsManager 每笔读绑定 UBO 当时的句柄字）
                    //   与 iOS NVN_RB_TEXU，结果给 uboSlot（NVN UBO 号 = cbuf − 3，同存储绑定的 uboSlot）和 uboOffset（句柄字的字节偏移
                    //   = HandleIndex × 4），不给 slot（slot 只指 c2 句柄表 / 组合槽）。回放在绑定时读当时的 UBO 字解出贴图号 / 采样器号。
                    //   编译期事实是这个 (stage, handle, cbufSlot) 的精确事实（NeedTexture 不借组合槽事实，缺就 MISSING_FACT）。
                    //   只接单个、非分离的采样贴图；分离对（第 4 片）、句柄数组（第 5 片）、贴图缓冲、图像仍如实报未接。
                    string shape = binding.Kind != "texture" ? "image" : binding.Source == "buffer-texture" ? "buffer texture" :
                        resolved.Separate ? "separate" : binding.Count != 1 || resolved.ArrayLength > 1 ? "array" :
                        resolved.HandleIndex < 0 || (long)resolved.HandleIndex * 4 + 4 > UserUboMaxBytes ? "offset outside 64 KiB" : null;
                    if (shape != null)
                        throw new DirectCompileException("UNSUPPORTED_PROGRAM",
                            WithHandleDetail(binding.Stage + ": texture handle from cbuf slot " + resolved.CbufSlot + " is not implemented (" + shape + ")",
                                HandleDetailFields(allocator, binding, resolved, messageHasShape: false)));
                    binding.Slot = -1;
                    binding.UboSlot = resolved.CbufSlot - UserUboFirstCbuf;
                    binding.UboOffset = resolved.HandleIndex * 4;
                    continue;
                }
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
                    // 图像句柄从 c2 图像句柄表读（字节 0x120 + 8k = 字 72 + 2k，空轨 2nd 9000 计算「cbuf slot 2」）：这个字就是 BindImages 第 k 槽的句柄，
                    //   与立即句柄 72 + 2k 同一个图像槽（计算解析器按 72..86 换算，同 iOS C2_IMG）；要有同字直接事实（宿主按图像槽出），数组不接。
                    bool c2Image = binding.Kind == "image" && resolved.CbufSlot == 2 && resolved.HandleIndex >= 72 && resolved.HandleIndex < 88 &&
                                   (resolved.HandleIndex & 1) == 0 && binding.Count == 1 && resolved.ArrayLength <= 1 &&
                                   entry.accessor.HasDirectTextureFact(resolved.HandleIndex, resolved.CbufSlot);
                    // 单值贴图句柄直接从 c2 句柄表读（运行时句柄 / 游戏自己的句柄表）：翻译阶段已经靠同字
                    // 事实拿到了类型与格式；这里按句柄值当槽位交给回放（解析器换算 slot = (handle − 8) / 2）。
                    // 数组仍只认 HandleIndex == 0 的整表数组（非零起点的数组回放侧还没有消费口径）。
                    bool c2SingleHandle = binding.Kind == "texture" && resolved.CbufSlot == 2 &&
                                          resolved.HandleIndex >= 0 && binding.Count == 1 && resolved.ArrayLength <= 1 &&
                                          entry.accessor.HasTextureFact(resolved.HandleIndex, resolved.CbufSlot);
                    // 计算阶段的 c2 句柄表数组：编译先放行，回放侧现在按具名 skip "compute_texture_array" 计数
                    // （C3 接池表消费）；图形数组仍只认 HandleIndex == 0 的整表数组。
                    bool c2ComputeArray = binding.Kind == "texture" && resolved.CbufSlot == 2 && binding.Stage == "cs" &&
                                          resolved.HandleIndex >= 0 && binding.Count > 1 &&
                                          binding.Count == resolved.ArrayLength;
                    if (!handleTableArray && !c2Image && !c2SingleHandle && !c2ComputeArray &&
                        (binding.Kind != "texture" || !entry.accessor.HasDirectTextureFact(resolved.HandleIndex, resolved.CbufSlot)))
                        throw new DirectCompileException("UNSUPPORTED_PROGRAM",
                            WithHandleDetail(binding.Stage + ": texture handle from cbuf slot " + resolved.CbufSlot +
                                " is not implemented (handle=" + resolved.HandleIndex + ", arrayLength=" + resolved.ArrayLength +
                                ", count=" + binding.Count + ", request=" + RequestSha() + ")",
                                HandleDetailFields(allocator, binding, resolved, messageHasShape: true)));
                }
                if (resolved.Separate)
                    throw new DirectCompileException("UNSUPPORTED_PROGRAM",
                        binding.Stage + ": separate sampler slot is not implemented");
                binding.Slot = resolved.HandleIndex;
            }
        }

        // #999：被拒的贴图句柄的现场，接在「texture handle from cbuf slot N is not implemented …」后面，写成 " [字段 字段 …]"，只进消息文字，不参与任何判断。
        //   字段按重要性从前往后排，放不下的从最后一项起整项丢掉（不截半项）：
        //   kind   = 绑定种类（texture / image）；
        //   handle = TextureDescriptor.HandleIndex 原值（与 MISSING_FACT 消息里的 handle= 同一个数）；
        //   word   = 句柄字在常量缓冲里的字偏移（字节偏移 = word × 4）；分离的 / 带类型的句柄（高 4 位是类型）按 TextureHandle.UnpackOffsets 拆成
        //            word（贴图字）/ word2（采样器字）/ type（TextureHandleType 的整数值）；
        //   cbuf   = 槽号带高 16 位时（TextureHandle.PackSlots），拆成 贴图常量缓冲号 / 采样器常量缓冲号；
        //   count / array = 绑定的描述符个数 / 反射到的数组长度（两者决定「是不是数组」这条拒绝）；
        //   key    = 请求的 requestId（宿主 nvk_shader.c：vs / fs / gs 各阶段的 8 字节程序键，各 16 个十六进制字符，连写，这里只取前 48 个字符）；没有就写 "-"。
        // messageHasShape = 前面的消息已经带了 handle= / arrayLength= / count= / request=<sha>（#1488 补的 c2 / 打包槽那一处）：handle / count / array 三项不再重复写；
        //   用户 UBO 那一处只有括号里的形态词，这三项由这里补（程序标识用 key，不带请求 sha）。
        // 宿主按消息里的「texture handle from cbuf slot 」归类（nvk_shader.c compiler_rejected），前缀与括号里的形态词不动。
        // 宿主（android/native/direct/compile_request.c）把整段错误 JSON 抄进 reason[320]（最多 319 个字符），截断了的 JSON 在
        //   nvk_shader.c compiler_rejected 里解析不出来，整条归成 unclassified；JSON 的信封
        //   {"schema":"s2a-direct-error-v1","code":"UNSUPPORTED_PROGRAM","message":"…"} 占 74 个字符，所以消息整条 ≤ 245 个字符是硬限
        //   （第二道 message[256] 更宽）。#1488 的那条基础消息（含 64 个字符的请求 sha）典型 175 个字符（边界用例实测）、各数取 int 极值最坏 204，
        //   留给现场的只有 41～70 个字符；
        //   超了就从最低优先级的字段起丢（用例钉最坏情形与边界；宿主用例钉「245 个字符的消息仍归类」）。
        private const int HandleDetailKeyChars = 48;
        private const int HostMessageMaxChars = 245;

        private static List<string> HandleDetailFields(DirectAllocator allocator, DirectBinding binding, TextureDescriptor descriptor, bool messageHasShape)
        {
            var fields = new List<string> { "kind=" + binding.Kind };
            if (!messageHasShape) fields.Add("handle=" + descriptor.HandleIndex);
            (int word, int word2, TextureHandleType type) = TextureHandle.UnpackOffsets(descriptor.HandleIndex);
            fields.Add(type == TextureHandleType.CombinedSampler
                ? "word=" + descriptor.HandleIndex
                : "word=" + word + " word2=" + word2 + " type=" + (int)type);
            if (descriptor.CbufSlot >= 0x10000)
            {
                (int textureBuffer, int samplerBuffer) = TextureHandle.UnpackSlots(descriptor.CbufSlot, descriptor.CbufSlot);
                fields.Add("cbuf=" + textureBuffer + "/" + samplerBuffer);
            }
            if (!messageHasShape)
            {
                fields.Add("count=" + binding.Count);
                fields.Add("array=" + descriptor.ArrayLength);
            }
            string key = allocator.RequestKey;
            fields.Add("key=" + (string.IsNullOrEmpty(key) ? "-" : key.Length > HandleDetailKeyChars ? key.Substring(0, HandleDetailKeyChars) : key));
            return fields;
        }

        private static string WithHandleDetail(string message, List<string> fields)
        {
            var text = new StringBuilder(message);
            bool open = false;
            foreach (string field in fields)
            {
                // 前面的空格（第一项是 " ["）+ 本项 + 收尾的 "]"；整条不超过宿主缓冲，一项放不下就停（后面的更低优先级，不再往后凑）
                if (text.Length + (open ? 1 : 2) + field.Length + 1 > HostMessageMaxChars) break;
                text.Append(open ? " " : " [").Append(field);
                open = true;
            }
            if (open) text.Append(']');
            return text.ToString();
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

        private static void WriteRanges(DirectResultWriter writer, int[][] ranges)
        {
            if (ranges == null) { writer.WriteNullValue(); return; }
            writer.WriteStartArray();
            foreach (int[] range in ranges)
            {
                writer.WriteStartArray();
                writer.WriteNumberValue(range[0]); writer.WriteNumberValue(range[1]);
                writer.WriteEndArray();
            }
            writer.WriteEndArray();
        }

        private static byte[] WriteContent(DirectProgram program, DirectProfile profile,
            List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)> translated,
            DirectAllocator allocator, List<DirectQuery> queries, bool v2, bool binary)
        {
            using var writer = new DirectResultWriter(binary);
            {
                writer.WriteStartObject();
                writer.WriteString("contract", v2 ? "s2a-direct-program-content-v2" : ContentContract);
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
                    long readStart = Timing == null ? 0 : System.Diagnostics.Stopwatch.GetTimestamp();
                    var c0 = allocator.Bindings.Where(b => b.Stage == "cs" && b.Kind == "uniform" && b.Slot == 0).ToArray();
                    int[][] reads = c0.Length == 0 ? Array.Empty<int[]>() : c0.Length != 1 ? null :
                        DirectComputeC0Reads.Analyze(translated[0].shader.BinaryCode, c0[0].Set, c0[0].Binding);
                    if (Timing != null) Timing.Metadata += System.Diagnostics.Stopwatch.GetTimestamp() - readStart;
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
                    long readStart = Timing == null ? 0 : System.Diagnostics.Stopwatch.GetTimestamp();
                    int[][] supportReads = DirectComputeSupportReads.Analyze(translated[0].shader.BinaryCode,
                        0, profile.Support.Binding, profile.Support.RequiredSize, out var supportScaleRanges);
                    if (Timing != null) Timing.Metadata += System.Diagnostics.Stopwatch.GetTimestamp() - readStart;
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
                    if (!binary) writer.WriteString("spirvSha256", DirectStage.Sha256Hex(shader.BinaryCode));
                    WriteInfo(writer, shader.Info);
                    writer.WriteNumber("stateReadFlags", accessor.StateReadFlags);
                    writer.WriteNumber("attributeReadMask", accessor.AttributeReadMask);
                    if (program.Kind == "graphics")
                    {
                        // 图形阶段的 c0（驱动常量段）读集：回放据此判断间接绘制要不要把 GPU 侧的 base instance 拷进 c0 + 0x644。
                        // 分析不出（非标准布局、多个 c0、下标取值界证不了）= null = 未知，消费端照旧拷；没有 c0 绑定 = []。
                        // 非常量下标按取值界记整段（boundDynamic）：证得够不着 0x644 所在的 vec4 才算不读。
                        long readStart = Timing == null ? 0 : System.Diagnostics.Stopwatch.GetTimestamp();
                        var stageC0 = allocator.Bindings.Where(b => b.Stage == stage.Stage && b.Kind == "uniform" && b.Slot == 0).ToArray();
                        int[][] stageReads = stageC0.Length == 0 ? Array.Empty<int[]>() : stageC0.Length != 1 ? null :
                            DirectComputeC0Reads.Analyze(shader.BinaryCode, stageC0[0].Set, stageC0[0].Binding, boundDynamic: true);
                        if (Timing != null) Timing.Metadata += System.Diagnostics.Stopwatch.GetTimestamp() - readStart;
                        writer.WritePropertyName("c0Reads");

                        WriteRanges(writer, stageReads);
                        // 同一内容同时携带无动态定界的原始读集，运行时只选消费策略，不改变缓存身份。
                        readStart = Timing == null ? 0 : System.Diagnostics.Stopwatch.GetTimestamp();
                        int[][] staticReads = stageC0.Length == 0 ? Array.Empty<int[]>() : stageC0.Length != 1 ? null :
                            DirectComputeC0Reads.Analyze(shader.BinaryCode, stageC0[0].Set, stageC0[0].Binding);
                        if (Timing != null) Timing.Metadata += System.Diagnostics.Stopwatch.GetTimestamp() - readStart;
                        writer.WritePropertyName("c0ReadsStatic");
                        WriteRanges(writer, staticReads);
                    }
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
                    if (binding.SamplerSlot >= 0) writer.WriteNumber("samp_slot", binding.SamplerSlot);
                    if (binding.ConstSampler >= 0) writer.WriteNumber("constSampler", binding.ConstSampler);
                    if (binding.Pool != null) writer.WriteString("pool", binding.Pool);   // 整池数组（ResolvePoolSlots）：没有 slot，宿主按池建快照
                    if (binding.Kind == "spill") writer.WriteNumber("bytesPerGroup", binding.SpillBytesPerGroup);
                    if (binding.Kind == "texture" && binding.UboSlot >= 0)
                    {
                        // 句柄在游戏 UBO 里（ResolvePoolSlots）：没有 slot，只有这两个来源字段
                        writer.WriteNumber("uboSlot", binding.UboSlot);
                        writer.WriteNumber("uboOffset", binding.UboOffset);
                    }
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
            return writer.Finish();
        }

        private static void WriteInfo(DirectResultWriter writer, ShaderProgramInfo info)
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

        private static void WriteBuffer(DirectResultWriter writer, BufferDescriptor descriptor)
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

        private static void WriteTexture(DirectResultWriter writer, TextureDescriptor descriptor)
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
            byte[] content, string contentHash, bool v2, bool binary,
            IReadOnlyDictionary<int, int> linkMapVs = null, IReadOnlyDictionary<int, int> linkMapFs = null)
        {
            using var writer = new DirectResultWriter(binary, frame: binary);
            {
                writer.WriteStartObject();
                writer.WriteString("schema", v2 ? "s2a-direct-program-v2" : ResultSchema);
                writer.WriteStartObject("transport");
                writer.WriteNumber("session", session);
                writer.WriteNumber("generation", generation);
                writer.WriteString("requestId", requestId);
                writer.WriteEndObject();
                if (!binary) writer.WriteString("contentHash", contentHash);
                writer.WritePropertyName("content");
                writer.WriteRawValue(content);
                if (linkMapVs != null && linkMapFs != null)
                {
                    // 诊断附录（只在请求带 linkMapDiag 时出现）：[[逻辑, 宿主], …] 按逻辑号升序；结果解析器不读它。
                    writer.WriteStartObject("linkMap");
                    foreach (var (name, map) in new[] { ("vs", linkMapVs), ("fs", linkMapFs) })
                    {
                        writer.WriteStartArray(name);
                        foreach (var kv in map)
                        {
                            writer.WriteStartArray();
                            writer.WriteNumberValue(kv.Key);
                            writer.WriteNumberValue(kv.Value);
                            writer.WriteEndArray();
                        }
                        writer.WriteEndArray();
                    }
                    writer.WriteEndObject();
                }
                writer.WriteEndObject();
            }
            return writer.Finish();
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
