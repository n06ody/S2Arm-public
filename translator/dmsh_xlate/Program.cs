// Original Switch shaders (Maxwell machine code) → SPIR-V (the Ryujinx.Graphics.Shader translator), for route B's offline metallib build.
// Difference from Ryujinx.ShaderTools: the translator's "queries" are answered truthfully rather than always with defaults -
//   texture type / image format / vertex attribute type / fragment output type / compute thread group size come from the job file (the game profile's answer table shader_facts.tsv, by the slot numbers of the reflection);
//   window origin upper-left; depth is not rewritten (z goes out unchanged, NVN's near=0 convention). A game's depth convention is runtime state (nvnDeviceSetDepthMode: Mario / Metroid etc. keep the default near=-w, the GL convention;
//   Link's Awakening / 13 Sentinels / Engage set near=0), so dmsh_build.py adds the GL rewrite z' = z/2 + w/2 in the MSL as two entry points, main0 (rewritten, GL) and main0_z (unchanged, near=0), and the host picks by the game's convention.
//   This used to rewrite for GL always: a near=0 game's depth was squeezed into 0.5..1, and effects that turn depth into distance (Link's Awakening's outdoor depth of field blurring the whole screen, occlusion) were all wrong.
// Usage: dmsh_xlate <job.json> <output dir>
//   Job: [{ "key", "stage": "vs|ps|cs", "code": machine code file (graphics = 0x50-byte SPH + instructions, compute = instructions),
//           "arch": "sm86" for Ampere code (NVN2, Switch 2: a 0x80-byte SPH and 128-bit instructions; absent = Maxwell), "nvn2_cs": NVN2 compute's c[0] layout (see Dsh.cs JobFor),
//           "tex": { "slot": "2d|2darray|cube|cubearray|3d|buffer|1d|1darray|2dms|2dmsarray", ... }, "img": { "slot": { "type", "fmt" } },
//           "attr": { "location": "float|sint|uint" }, "fout": { "target": "float|sint|uint" }, "local": [x, y, z],
//           "gs_input_topology": a native (not compute-emulated) geometry shader's input primitive, InputTopology number (absent = triangles) }]
//   Output: <key>.spv and <key>.res.json (offsets of storage buffer slots in c0, etc.; decoding texture handle → slot number is done in the script that builds the metallib;
//           "fetch1d": the texture slots read by 1D integer fetches translated as wrapped 2D reads, see RecordTexelFetch1D; "fetch1d_exact": Ampere's form of them, rows sized by the host to the element count)
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

// 文件读写经这里：本线程设了 Mem（出包 C# 批量进程 S2A.Dsh，11-01）时读写都在这张表里，不落临时文件；单独跑的 dmsh_xlate 照常读写文件
static class XIo
{
    [ThreadStatic] public static Dictionary<string, byte[]> Mem;
    public static byte[] ReadAllBytes(string p) => Mem != null && Mem.TryGetValue(p, out var b) ? b : File.ReadAllBytes(p);
    public static string ReadAllText(string p) => Mem != null && Mem.TryGetValue(p, out var b) ? System.Text.Encoding.UTF8.GetString(b) : File.ReadAllText(p);
    public static void WriteAllBytes(string p, byte[] b) { if (Mem != null) Mem[p] = b; else File.WriteAllBytes(p, b); }
    public static void WriteAllText(string p, string t) { if (Mem != null) Mem[p] = new System.Text.UTF8Encoding(false).GetBytes(t); else File.WriteAllText(p, t); }
    public static bool Exists(string p) => Mem != null ? Mem.ContainsKey(p) : File.Exists(p);
}
static class SwshXlate
{
    // NVN's handle tables in c[2], byte offsets, 8 bytes per slot (one layout in every game, see dm/gfx/tools/dmsh_build.py): combined textures 0x20 + 8 r, images 0x120 + 8 r (8 slots),
    // separate textures 0x168 + 8 r (slot 128 + r), separate samplers 0x568 + 8 r
    const int TexBase = 0x20, ImgBase = 0x120, ImgSlots = 8, SepTexBase = 0x168, SepTexSlot0 = 128, SampBase = 0x568;

    sealed class Accessor : IGpuAccessor
    {
        public bool QueryHostSupportsTextureSizeTable() => true;
        public bool QueryS2ALateStorageBufferAlignment() => _job["storage_window_mask"] is JsonValue value && (bool)value;
        public bool QueryHasUnalignedStorageBuffer() => QueryS2ALateStorageBufferAlignment();
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new();
        // S2Arm 11-05: the translator's own warnings (Ryujinx ILogger.Log: "Invalid reduction type", forms it drops) go into the shader's notes, so a build shows what a
        //   translation lost (they were silent: 398 float reductions in 33 of Zelda: Ocarina of Time's shaders disappeared without a trace)
        public void Log(string message) => Notes.Add(message);
        // Only queries actually requested by this translation contribute facts.
        public readonly SortedSet<string> MissingResolutionFacts = new(StringComparer.Ordinal);
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job, Dictionary<int, SamplerType> imageTypes = null) { if (job["geometry_topology"] != null) _binding = 40; _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); _imageTypes = imageTypes; }
        // A texture query on an image handle (TXQ on c2 0x120 + 8 r: Unreal's GetDimensions on the RWTexture3D a compute shader writes) asks the type of that handle, which a table built
        // without the game's PC shaders does not answer, and the 2D default took a 3D image's depth from its width. The shader's own surface instructions give the image's type: the
        // first translation records that a query went unanswered (ImageQueryGuessed), and Main translates again with the types of the images it found, by c2 handle (ImageTypesOf)
        readonly Dictionary<int, SamplerType> _imageTypes; public bool ImageQueryGuessed;
        public static Dictionary<int, SamplerType> ImageTypesOf(ShaderProgram p)
        {
            var types = new Dictionary<int, SamplerType>();
            foreach (var im in p.Info.Images) if (im.CbufSlot == 2 || im.CbufSlot < 0) types[im.HandleIndex] = im.Type & (SamplerType.Mask | SamplerType.Array);
            return types;
        }
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        // Ampere (job arch sm86; the reference port d3995ca4 / a7bd05f4, MIT): a jump table ends at the end of c1 at the latest; NVN2 compute's own c[0] layout from its control section (job nvn2_cs)
        public int QueryConstantBuffer1Size() => _c1.Length;
        public int[] QueryAmpereComputeC0Layout() => _job["nvn2_cs"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;
        // NVN2 task / mesh shaders translated as compute (job stage_memory, Dsh.cs MeshStage.Memory): the shared memory offsets of their stage memory, which ISBERD / 0x927 address
        public int[] QueryAmpereStageMemory() => _job["stage_memory"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 read out of bounds +{offset:X} (c1 length {_c1.Length})"); return 0; }
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        // An address past the code is no code (upstream (MIT) 9507c221c): FindBrxTargetsAmpere ends a jump table on it when the words after the table are not code offsets; the slice threw
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => address >= (ulong)_code.Length ? default : MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        // 追不到槽的句柄（Ryujinx 回退 <阶段>_<t|s>_cb0_40000000）：采样贴图按整段编号读池（纹理号 20 位、采样器号 12 位，同上游参考实现（MIT）Program.cs），
        //   S2A.Dsh PoolArrays 把这两个数组改成指向宿主池表的指针（nvn_mtl.m pool_gpu）。原先写死 32：下标夹到 31、宿主第 k 项绑第 k 个贴图槽，
        //   句柄指的贴图编号 ≥ 32 时读到别的贴图（时之笛 4,454 支、阴影深度比较）。贴图缓冲与图像同样按整段编号（宿主按元素类型分表）
        public int QuerySamplerArrayLengthFromPool() => 1 << 12;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 1 << 20;
        public bool QueryHostSupportsBindlessUniformHandles() => _job["bindless_uniform_handles"] is JsonNode enabled && (bool)enabled;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        // A handle's texture or image slot, numbered as dmsh_build.py decode_tex numbers the reflection's (so an answer table row names the slot the reflection shows): the handle ORs two
        // c[2] words in no fixed order, so both are tried (only the first for a combined handle or a constant sampler); -1 when neither is in the table
        static int Slot(int handle, int cbufSlot, bool image)
        {
            if (cbufSlot >= 0 && (cbufSlot & 0xffff) != 2) return -1;
            (int w0, int w1, TextureHandleType ty) = TextureHandle.UnpackOffsets(handle);
            foreach (int w in ty == TextureHandleType.CombinedSampler || ty == TextureHandleType.SeparateConstantSamplerHandle ? new[] { w0 } : new[] { w0, w1 })
            {
                int b = w * 4;
                if (b % 8 != 0) continue;
                if (image && b >= ImgBase && b < ImgBase + 8 * ImgSlots) return (b - ImgBase) / 8;
                if (!image && b >= TexBase && b < SepTexBase) return (b - TexBase) / 8;
                if (!image && b >= SepTexBase && b < SampBase) return SepTexSlot0 + (b - SepTexBase) / 8;
            }
            return -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, false), islot = Slot(handle, cbufSlot, true);
            string t = (slot >= 0 ? Get("tex", slot) : null) ?? (islot >= 0 ? Get("img", islot) : null);
            if (t == null && islot >= 0 && cbufSlot < 0)
            {
                if (_imageTypes != null && _imageTypes.TryGetValue(handle, out SamplerType it)) { Notes.Add($"texture handle {handle:X} is image slot {islot}: type {it} from the shader's image instructions"); return it; }
                ImageQueryGuessed = true;
            }
            if (t == null)
            {
                MissingResolutionFacts.Add($"unknown_missing_fact:sampler_type:handle={handle:X}:cbuf={cbufSlot}:slot={slot}");
                Notes.Add($"texture handle {handle:X} (constant buffer {cbufSlot}, slot {slot}) has no type in the answer table, using 2D");
                return SamplerType.Texture2D;
            }
            SamplerType? type = t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => null,
            };
            if (type == null) MissingResolutionFacts.Add($"unknown_missing_fact:sampler_type:handle={handle:X}:cbuf={cbufSlot}:slot={slot}:value={t}");
            return type ?? SamplerType.Texture2D;
        }
        // Texture slots read by a 1D integer fetch that the translator made a wrapped 2D read (x & 0x3FFF, x >> 14) for want of a type answer: they may hold a 1D texture or a texture buffer,
        // which only the bound descriptor tells (res "fetch1d"; dmsh_build.py flags their bindings, and the host binds a texture buffer there as 16384-texel rows, nvn_mtl.m tbuf_as_2d)
        public readonly SortedSet<int> Fetch1D = new();
        public void RecordTexelFetch1D(int handle) { int slot = Slot(handle, -1, false); if (slot >= 0) Fetch1D.Add(slot); }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, true);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null)
            {
                MissingResolutionFacts.Add($"unknown_missing_fact:image_format:handle={handle:X}:cbuf={cbufSlot}:slot={slot}");
                Notes.Add($"image handle {handle:X} slot {slot} has no format in the answer table, using R8G8B8A8Unorm");
                return TextureFormat.R8G8B8A8Unorm;
            }
            bool parsed = Enum.TryParse(f, true, out TextureFormat tf);
            if (!parsed || !Enum.IsDefined(tf) || tf == TextureFormat.Unknown)
                MissingResolutionFacts.Add($"unknown_missing_fact:image_format:handle={handle:X}:cbuf={cbufSlot}:slot={slot}:value={f}");
            return parsed ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // the shared memory size in the compute shader's control section (shader_split.py)
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public bool QueryGeometryInputFixed() => _job["geometry_topology"] != null;
        // 直连宿主的两条合同（compat/tree IGpuAccessor 上缺省关，只有直连答真）：1D 整数取样折行读 2D（宿主 tbuf_as_2d 配合）、bindless 句柄穿过嵌套 phi
        public bool QueryTexelFetch1DWrap() => true;
        // 按出库目标档案答（job imgload_raw，Dsh Ctx.ImageLoadRaw 写）：档档案有 shaderStorageImageReadWithoutFormat 才 true——
        //   直连 Vulkan 宿主按「贴图字节数绑同数值类视图」消费无格式声明（rp_draw.c image_view_format），Unknown 与现编
        //   （DirectGpuAccessor 按设备真能力答）同口径。缺省 false = 最低档案：只读非缓冲图像声明具体格式，任何设备都合法；
        //   声明与设备不配时出库件在绑定核时被拒、退回现编（#1850 的 rp_shader_lib_reject），不出库成死件（#1774 ②）。
        public bool QueryHostSupportsImageLoadFormattedRaw() => _job["imgload_raw"] is JsonNode n && (bool)n;
        public bool QueryBindlessPhiSeeThrough() => true;
        public bool QueryS2ARuntimeState() => false;   // 直连宿主不填 s2a_flags / s2a_point（compat/tree IGpuAccessor.QueryS2ARuntimeState）
        public bool QueryHostSpirvLayerExt() => true; // Metal conversion consumes the legacy EXT form.
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            // tess_mode：曲面模式（3D 寄存器 TessMode 的打包值，NVN 控制段 +0x744，同 graphics/tools/shbake TessModeOf）：位 0–1 域、4–5 分区、8 顺时针；hull / domain 阶段由 dmsh_build.py 给，其余缺省三角 / 等距
            uint tm = _job["tess_mode"] is JsonNode tmn ? (uint)tmn : 1u;   // 缺省 1 = 三角、等距、逆时针（与原先写死的相同）
            // gs_input_topology：原生几何阶段（不转计算，安卓离线 Vulkan 库导出）的输入图元，按 InputTopology 编号；几何着色器的 SPIR-V 随它变（输入图元执行模式、输入数组长）
            return new GpuGraphicsState(_job["early_z"] is JsonNode ez && (bool)ez /* 片元早期深度：控制段 +0x718 位 0（Dsh JobFor） */,
                _job["geometry_topology"] is JsonNode gt ? (InputTopology)(int)gt : _job["gs_input_topology"] is JsonNode it ? (InputTopology)(int)it : InputTopology.Triangles, (tm & 0x100) != 0, (TessPatchType)(tm & 3), (TessSpacing)((tm >> 4) & 3),
                false, false, false, false /* depth not rewritten: dmsh_build.py adds the GL entry point (see the file header) */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* no Y flip */, true /* origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        // Metal has no viewport mask output (NV_viewport_array2). The interface default is true, so the translator wrote the mask to a builtin SPIRV-Cross cannot name for Metal
        //   (gl_BuiltIn_5253): Splatoon 3's 5 geometry passthrough readers (_bgv) never compiled. Same answer as Ryujinx on MoltenVK (Capabilities.SupportsViewportMask) and our generic renderer (NxCaps.cs)
        public bool QueryHostSupportsViewportMask() => false;
        // No geometry stage either: geometry shaders run as compute kernels (dmsh_build.py geometry_finish), so nothing passes vertices through. With the default true a passthrough
        //   geometry shader was translated for NV hardware passthrough and copied no input to its output (its kernel had no geometry_vb_output at all); false makes the translator
        //   write the copies (EmitterContext GpPassthrough). Same answer as Ryujinx on MoltenVK and our generic renderer
        public bool QueryHostSupportsGeometryShaderPassthrough() => false;
        public bool QueryHostSupportsDepthClipControl() => false;   // Metal has no -1..1 clip control; the depth convention is not left to the translator, the rewrite is added per entry point in the MSL (see the file header)
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // Job softfloat64 = true: the host has no FP64, so Ryujinx's DoubleToFloat pass lowers double operations to float (Metal has no double type). Set only on the retry of a
        // shader whose first translation declared a 64-bit float (see Main): answering "no" for every shader would also drop the Float64 capability from all the others
        public bool QueryHostSupportsShaderFloat64() => !(_job["softfloat64"] is JsonNode n && (bool)n);
    }

    // True when the SPIR-V declares a 64-bit float type (OpTypeFloat 64): SPIRV-Cross then writes double, which the Metal compiler rejects
    static bool UsesFloat64(byte[] spirv)
    {
        for (int i = 20; i + 12 <= spirv.Length; )
        {
            uint word = BitConverter.ToUInt32(spirv, i);
            if ((word & 0xffff) == 22 && (word >> 16) == 3 && BitConverter.ToUInt32(spirv, i + 8) == 64) return true;
            i += Math.Max(1, (int)(word >> 16)) * 4;
        }
        return false;
    }

    // S2Arm geometry transform feedback (10-26): word offsets in a geometry shader's output record (GeometryToCompute: one record of OutputSizePerInvocation words per emitted
    //   vertex). The record is packed by what the shader writes (ResourceReservations.FillIoOffsetMap: position 0-3, point size, written clip distances, layer, viewport, then each
    //   written generic output n as 4 words), so the offsets come from the translator itself, not recomputed. TryGetOffset and both enums are internal: called by reflection.
    //   → [position, generic 0 … generic 31] word offset of component x, −1 where the record has none (nvn_geometry.inc geo_tf)
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.NonPublicMethods, typeof(ResourceReservations))]   // the trimmed on-device translator (S2A.Dsh) keeps TryGetOffset for the reflection below
    static JsonArray GeometryOutOffsets(ResourceReservations rr)
    {
        // The two internal enum types are read off TryGetOffset's own parameters (not Assembly.GetType by name: that is not trim-analyzable, IL2026 in the
        //   NativeAOT dsh_batch / on-device builds); the method itself is kept by the DynamicDependency above
        var mi = typeof(ResourceReservations).GetMethods(System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)
            .SingleOrDefault(m => m.Name == "TryGetOffset" && m.GetParameters() is var ps && ps.Length == 5 && ps[0].ParameterType.Name == "StorageKind" && ps[1].ParameterType.Name == "IoVariable"
                && ps[2].ParameterType == typeof(int) && ps[3].ParameterType == typeof(int) && ps[4].ParameterType == typeof(int).MakeByRefType())
            ?? throw new MissingMethodException("ResourceReservations.TryGetOffset(StorageKind, IoVariable, int, int, out int)");
        Type kind = mi.GetParameters()[0].ParameterType, io = mi.GetParameters()[1].ParameterType;
        object output = Enum.Parse(kind, "Output");
        int Off(string v, int loc) { var a = new object[] { output, Enum.Parse(io, v), loc, 0, 0 }; return (bool)mi.Invoke(rr, a) ? (int)a[4] : -1; }
        var r = new JsonArray { (JsonNode)JsonValue.Create(Off("Position", 0)) };   // JsonValue.Create(int), not JsonArray.Add<T>: the trimmed on-device translator has no type info for Add<int>
        for (int n = 0; n < 32; n++) r.Add((JsonNode)JsonValue.Create(Off("UserDefined", n)));
        return r;
    }

    // S2Arm widen: which vec4 of each uniform slot the shader reads (graphics/widen/shader/SpirvCbufUse.cs, the generic mode's analysis, compiled in), as the text the
    //   runtime hands to s2a_widen_shader unchanged (graphics/widen/SpirvCbufUseText.cs), keyed by the replay's uniform register: NVN slot - 3 as in dmsh_build.py reflect;
    //   the driver slots below 3 are left out. The widen core then knows per draw whether the shader reads the rewritten matrix rather than taking every bound slot.
    static string CbufUseText(ShaderProgram p)
    {
        int n = 64; foreach (var b in p.Info.CBuffers) n = Math.Max(n, b.Binding + 1);   // never empty: an empty map means "binding = slot" to Format, which would take the support buffer
        var map = new int[n]; Array.Fill(map, -1);
        foreach (var b in p.Info.CBuffers) map[b.Binding] = b.Slot >= 3 && b.Slot <= 16 ? b.Slot - 3 : -1;
        var uses = S2Arm.Gfx.Widen.SpirvCbufUse.Analyze(p.BinaryCode);   // null: not SPIR-V it can walk (left unknown: the core takes every bound slot)
        return uses == null ? null : S2Arm.Gfx.Widen.SpirvCbufUseText.Format(uses, map);
    }

    // All stages: tex_scales = [support index, SPIR-V binding, is image]. Legacy
    // scale_exact keeps the generic widening contract. The ABI2 proof is separate:
    // unproven programs retain their legal legacy 1x translation and zero-size table.
    static void FetchScales(ShaderProgram p, JsonObject res)
    {
        S2Arm.Gfx.Widen.SpirvFetchScale.PatchY(p.BinaryCode, S2Arm.Gfx.Widen.WidenShaders.RenderScaleMember, out var legacyScales, out _, out bool exact);
        bool resolutionExact = S2Arm.Gfx.Widen.SpirvFetchScale.AnalyzeSizeTable(p.BinaryCode, S2Arm.Gfx.Widen.WidenShaders.RenderScaleMember, out var scales, out var analysisReasons);
        var a = new JsonArray(); var seen = new HashSet<(int, int, bool)>();
        foreach (var scale in scales) if (seen.Add(scale)) a.Add((JsonNode)new JsonArray(JsonValue.Create(scale.Index), JsonValue.Create(scale.Binding), JsonValue.Create(scale.Image)));
        foreach (var scale in legacyScales) if (seen.Add(scale)) a.Add((JsonNode)new JsonArray(JsonValue.Create(scale.Index), JsonValue.Create(scale.Binding), JsonValue.Create(scale.Image)));
        res["tex_scales"] = a; res["scale_exact"] = exact;
        var reasons = new SortedSet<string>(StringComparer.Ordinal);
        foreach (string reason in analysisReasons) reasons.Add("unsupported_scaled_access:" + reason);
        if (!resolutionExact && reasons.Count == 0) reasons.Add("unsupported_scaled_access:unproven_shader_access");
        foreach (var scale in seen)
            if (scale.Item1 < 1 || scale.Item1 >= SupportBuffer.RenderScaleMaxCount)
                reasons.Add($"unsupported_scaled_access:scale_index_out_of_range:index={scale.Item1}");
        foreach (var list in new[] { p.Info.Textures, p.Info.Images })
            foreach (var t in list)
            {
                if (t.ArrayLength != 1) reasons.Add($"unsupported_scaled_access:descriptor_array:binding={t.Binding}:length={t.ArrayLength}");
                if (t.Set != 0) reasons.Add($"unsupported_scaled_access:descriptor_set:binding={t.Binding}:set={t.Set}");
                // 1D / 3D / 立方体 / 缓冲：宿主从不缩放这类存储（nvn_rscale.inc rs_eligible），单独记成 never_scaled_resource（内部分辨率按它放行，见 SpirvFetchScale）；类型认不出（None）照旧
                if ((t.Type & SamplerType.Mask) != SamplerType.Texture2D)
                    reasons.Add((t.Type & SamplerType.Mask) == SamplerType.None ? $"unsupported_scaled_access:non_2d_resource:binding={t.Binding}:type={t.Type}" : $"unsupported_scaled_access:never_scaled_resource:binding={t.Binding}:type={t.Type}");
                // 写图像不再记原因（11-22）：宿主对存储图像槽一律保持原尺寸（nvn_rscale_ops.inc rs_pre_images / rs_pre_draw），写的坐标按游戏纹素就对，见 SpirvFetchScale OpImageWrite
                if ((t.Flags & TextureUsageFlags.ResScaleUnsupported) != 0) reasons.Add($"unsupported_scaled_access:resource_scale_unsupported:binding={t.Binding}");
            }
        SetResolutionMetadata(res, reasons);
    }

    static void SetResolutionMetadata(JsonObject res, IEnumerable<string> reasons)
    {
        var unique = new SortedSet<string>(reasons, StringComparer.Ordinal);
        var a = new JsonArray(); foreach (string reason in unique) a.Add((JsonNode)JsonValue.Create(reason));
        res["resolution_scale_abi"] = SupportBuffer.TextureSizeTableAbi;
        res["resolution_scale_exact"] = unique.Count == 0;
        res["resolution_scale_status"] = unique.Count == 0 ? "exact" : "legacy_1x";
        res["resolution_scale_reasons"] = a;
    }

    // A geometry shader is translated on its own (as a compute kernel, one library entry whatever fragment shader it pairs with): its output interface is every generic
    //   attribute a fragment shader may read (TranslatorContext.S2ASetStandaloneGeometryNextFragment: passthrough ones forwarded, the rest the shader does not write
    //   given their defaults), in both the kernel's output record and the reader (_bgv)
    static TranslatorContext Ctx(Accessor acc, TranslationFlags flags, bool geometry)
    {
        var c = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
        if (geometry) c.S2ASetStandaloneGeometryNextFragment();
        return c;
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--source-sha")   // first 16 digits of the sha256 of the embedded sources: Program.cs, csproj, the widen read text format (same convention as dmsh_chain.py XLATE_SOURCES),
        {                                                     //   then the library source manifest (src3, see the csproj S2ALibSourceManifest): paths already ryu/... and widen/..., lines sorted, joined by \n
            var asm = typeof(SwshXlate).Assembly; var ms = new MemoryStream();
            foreach (var n in new[] { "src0", "src1", "src2" }) asm.GetManifestResourceStream(n).CopyTo(ms);
            var rows = new List<string>(new StreamReader(asm.GetManifestResourceStream("src3")).ReadToEnd().Split('\n', StringSplitOptions.RemoveEmptyEntries));
            rows.Sort(StringComparer.Ordinal);
            ms.Write(System.Text.Encoding.UTF8.GetBytes(string.Join("\n", rows)));
            Console.WriteLine(Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(ms.ToArray()))[..16].ToLowerInvariant());
            return 0;
        }
        var jobs = JsonNode.Parse(XIo.ReadAllText(args[0])).AsArray(); string outDir = args[1]; if (XIo.Mem == null) Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string key = (string)job["key"];
            try
            {
                byte[] code = XIo.ReadAllBytes((string)job["code"]);
                var acc = new Accessor(code, job);
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0)
                    | ((string)job["arch"] == "sm86" ? TranslationFlags.Ampere : 0);   // Switch 2 (NVN2): Ampere machine code, the translator's Ampere frontend
                bool geometry = job["geometry_topology"] != null, passthroughFast = false;
                var ctx = Ctx(acc, flags, geometry);
                ShaderProgram p = ctx.Translate(geometry);
                Dictionary<int, SamplerType> imageTypes = null;
                if (acc.ImageQueryGuessed && (imageTypes = Accessor.ImageTypesOf(p)).Count > 0)   // a query on an image handle went unanswered: again with the images' types (see Accessor)
                {
                    acc = new Accessor(code, job, imageTypes);
                    ctx = Ctx(acc, flags, geometry);
                    p = ctx.Translate(geometry);
                }
                if (UsesFloat64(p.BinaryCode))   // Tears of the Kingdom 1.4.3: 30 shaders used double and failed in the Metal compiler
                {
                    job["softfloat64"] = true; acc = new Accessor(code, job, imageTypes);
                    ctx = Ctx(acc, flags, geometry);
                    p = ctx.Translate(geometry); acc.Notes.Add("FP64 lowered to FP32 (DoubleToFloat)");
                }
                if (geometry)
                {
                    // A fresh context: translation mutates the decoded program.
                    var reader = Ctx(new Accessor(code, job, imageTypes), flags, geometry);
                    XIo.WriteAllBytes(Path.Combine(outDir, key + ".reader.spv"), reader.GenerateVertexPassthroughForCompute().BinaryCode);
                    // Passthrough geometry fast path (S2Arm 11-06 energy line, Donkey Kong: the kernel's record writes and the index compaction were the largest GPU item of a frame):
                    //   a passthrough shader's output primitives are its input primitives and its vertex values are the previous stage's, so the main pass can read the vertex capture
                    //   directly (.capture.spv, the _bgc library) and the kernel only keeps the shader body's side effects (.side.spv, _bgs<topology>: no records, no indices).
                    //   Only when the body itself writes no output and emits nothing (S2APassthroughBodyOutputs): those values exist only in the records
                    if (ctx.S2AGpPassthrough)
                    {
                        var side = Ctx(new Accessor(code, job, imageTypes), flags, geometry);
                        side.S2APassthroughSideEffectsOnly = true;
                        byte[] sideCode = side.Translate(geometry).BinaryCode;
                        if (!side.S2APassthroughBodyOutputs)
                        {
                            XIo.WriteAllBytes(Path.Combine(outDir, key + ".side.spv"), sideCode);
                            var capture = Ctx(new Accessor(code, job, imageTypes), flags, geometry);
                            XIo.WriteAllBytes(Path.Combine(outDir, key + ".capture.spv"), capture.S2AGeneratePassthroughCaptureReader().BinaryCode);
                            passthroughFast = true;
                        }
                    }
                }
                XIo.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["geometry_stride"] = geometry ? ctx.GetResourceReservations().OutputSizePerInvocation : 0,
                    ["geometry_out_offsets"] = geometry ? GeometryOutOffsets(ctx.GetResourceReservations()) : null,
                    ["gs_passthrough"] = geometry && ctx.S2AGpPassthrough, ["gs_passthrough_fast"] = passthroughFast,   // header passthrough bit; .side.spv / .capture.spv written (see above)
                    ["geometry_max_vertices"] = geometry ? p.Info.GeometryMaxOutputVertices : 0,   // the kernel's records per invocation: a passthrough geometry shader without host passthrough emits the input primitive's vertices (ShaderDefinitions.MaxOutputVertices), not the header's count
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                };
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                res["cbuf_use"] = CbufUseText(p);
                FetchScales(p, res);
                var resolutionReasons = new List<string>(acc.MissingResolutionFacts);
                foreach (JsonNode reason in res["resolution_scale_reasons"].AsArray()) resolutionReasons.Add((string)reason);
                if (acc.Fetch1D.Count != 0) resolutionReasons.Add("unsupported_scaled_access:wrapped_1d_fetch");
                if ((string)job["stage"] == "gs" || geometry) resolutionReasons.Add("unsupported_scaled_access:geometry_stage");
                if ((string)job["stage"] is "hs" or "ds") resolutionReasons.Add("unsupported_scaled_access:tessellation_stage");
                SetResolutionMetadata(res, resolutionReasons);
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add((JsonNode)new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                // 不用 Add<int>：玩家包宿主（NativeAOT）关了反射式 JSON，没有 int 的类型信息（同 GeometryOutOffsets；11-02 疯狂兔子兜底现编整批 NotSupportedException）
                if (acc.Fetch1D.Count > 0) { var f1 = new JsonArray(); foreach (int h in acc.Fetch1D) f1.Add((JsonNode)JsonValue.Create(h)); res["fetch1d"] = f1; }
                // Ampere's wrapped fetch takes its row width and count from the bound 2D form and reads 0 past W * H (InstEmitAmpereMem.cs, 11-04): the host binds a texture buffer there as exact rows
                //   (W * H = element count, W a power of two when H > 1; nvn_mtl.m tbuf_as_2d). Maxwell's wrapped fetch keeps the fixed 16384-texel rows
                if (acc.Fetch1D.Count > 0 && (string)job["arch"] == "sm86") res["fetch1d_exact"] = true;
                XIo.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                ok++;
            }
            catch (Exception e) { bad++; Console.Error.WriteLine($"[RED] {key}: {e.GetType().Name}: {e.Message.Split('\n')[0]}"); if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); }
        }
        Console.WriteLine($"translated: {ok} ok, {bad} failed");
        return bad == 0 ? 0 : 1;
    }
}
