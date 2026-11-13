// C2 主机验收：合成 VS/FS/CS 走真实 Ryujinx 翻译器；离线编译器与 NativeAOT 用的是同一
// DirectShaderCompiler。合成 shader 只用于测试，不是运行时占位 shader。
using Ryujinx.Graphics.Shader;
using S2A.Android.Aot;
using S2A.DirectCompiler;
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

internal static unsafe class Program
{
    private static int _checks, _failures;
    private static void Check(bool ok, string what)
    {
        _checks++;
        if (!ok) { Console.Error.WriteLine("FAIL " + what); _failures++; }
    }
    private static void Throws(string code, Action action, string what)
    {
        _checks++;
        try { action(); }
        catch (DirectCompileException error)
        {
            if (code != null && error.Code != code)
            { Console.Error.WriteLine($"FAIL {what}: code={error.Code} want={code} ({error.Message})"); _failures++; }
            return;
        }
        catch (Exception error) { Console.Error.WriteLine($"FAIL {what}: {error.GetType().Name} {error.Message}"); _failures++; return; }
        // 该抛没抛也算失败（原先这三条路只打印 FAIL、不计数，整份照样报 ok）
        Console.Error.WriteLine("FAIL " + what + ": no exception"); _failures++;
    }

    private const ulong Sched = 0x001F8000FC0007E0UL, Exit = 0xE30000000007000FUL;
    // 合成 Maxwell 指令；布局/编解码沿用 graphics/vulkan/runtime/varying-tests 的受控生成器。
    private static ulong Ast(int location, int component = 0, int reg = 0) =>
        0xEFF0000000000000UL | (0xFFUL << 39) | ((ulong)(0x80 + location * 16 + component * 4) << 20) | (7UL << 16) | (0xFFUL << 8) | (uint)reg;
    private static ulong Ald(int location, int component = 0, int reg = 0) =>
        0xEFD8000000000000UL | (0xFFUL << 39) | ((ulong)(0x80 + location * 16 + component * 4) << 20) | (7UL << 16) | (0xFFUL << 8) | (uint)reg;
    // FADD32I Rd, RZ, imm：用于合成“片元输出常量”的已实现 Maxwell 指令（不是占位 shader）。
    private static ulong Fadd32i(int dest, float value) =>
        0x0800000000000000UL | ((ulong)BitConverter.SingleToUInt32Bits(value) << 20) | (7UL << 16) | (0xFFUL << 8) | (uint)dest;
    // FADD Rd, Ra, Rb（Maxwell 顶层 opcode 0101110001011，SrcB 在 bit20、谓词在 bit16）。
    // 多纹理合成需要寄存器相加：两次采样写在不同寄存器，再逐通道相加。
    private static ulong FaddR(int dest, int a, int b) =>
        0x5C58000000000000UL | ((ulong)b << 20) | (7UL << 16) | ((ulong)a << 8) | (uint)dest;
    // LDC Rd, c[slot][offset]：B32 常量加载（LsSize=4），SrcA=RZ 表示只用立即偏移。
    private static ulong Ldc(int dest, int offset, int slot = 0, int srcA = 0xFF) =>
        0xEF90000000000000UL | (4UL << 48) | ((ulong)slot << 36) | ((ulong)(offset & 0xFFFF) << 20) |
        (7UL << 16) | ((ulong)srcA << 8) | (uint)dest;
    private static ulong Ald4(int location) =>
        0xEFD8000000000000UL | (0xFFUL << 39) | (3UL << 47) | ((ulong)(0x80 + location * 16) << 20) | (7UL << 16) | (0xFFUL << 8);
    // TEX R0, R2, texture0：2D、坐标 R2/R3、写 RGBA。
    private static ulong Tex2D(int dest, int coord, int tid = 0) =>
        0xC000000000000000UL | (7UL << 16) | (2UL << 28) | (0xFUL << 31) |
        ((ulong)tid << 36) | ((ulong)coord << 8) | (uint)dest;
    // TEX.B: SrcB supplies a runtime texture/sampler handle (InstTexB decoder).
    private static ulong TexBindless2D(int dest, int coord, int handle) =>
        0xDE80000000000000UL | (7UL << 16) | (2UL << 28) | (0xFUL << 31) |
        ((ulong)handle << 20) | ((ulong)coord << 8) | (uint)dest;
    private static ulong Ast4Pos(int reg) =>
        0xEFF0000000000000UL | (0xFFUL << 39) | (3UL << 47) | (0x70UL << 20) | (7UL << 16) | (0xFFUL << 8) | (uint)reg;
    private static byte[] Guest(bool fragment, ulong[] ops, int[] reads = null)
    {
        int blocks = (ops.Length + 1 + 2) / 3;
        byte[] b = new byte[0x50 + blocks * 32];
        BinaryPrimitives.WriteUInt32LittleEndian(b, fragment ? 2u | (3u << 5) | (5u << 10) : 1u | (3u << 5) | (1u << 10));
        if (fragment)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(72), 15);
            foreach (int location in reads ?? Array.Empty<int>()) b[24 + location] = 0xAA;
        }
        for (int i = 0; i < blocks * 4; i++)
        {
            int oi = i / 4 * 3 + i % 4 - 1;
            ulong op = i % 4 == 0 ? Sched : oi < ops.Length ? ops[oi] : Exit;
            BinaryPrimitives.WriteUInt64LittleEndian(b.AsSpan(0x50 + i * 8), op);
        }
        return b;
    }
    private static byte[] ComputeGuest() => MemoryMarshal.AsBytes(new ulong[] { Sched, Exit, 0x50B0000000070F00UL, 0xE2400FFFFF07000FUL }.AsSpan()).ToArray();

    // 原创合成 Maxwell GS：普通（非 passthrough）几何阶段，读写 loc0，发出顶点并结束图元。
    // OUT 的 opcode / 类型域分别来自 InstTable 和 InstOutR，不依赖游戏输入。
    private static byte[] GeometryGuest(bool viewportIndex = false, bool omitVaryingOutput = false, bool uniform = false)
    {
        ulong emit = 0xFBE0000000000000UL | (1UL << 39) | (0xFFUL << 20) | (7UL << 16) | (0xFFUL << 8) | 0xFFUL;
        ulong emitAndCut = 0xFBE0000000000000UL | (3UL << 39) | (0xFFUL << 20) | (7UL << 16) | (0xFFUL << 8) | 0xFFUL;
        var ops = new List<ulong> {
            Ald(0), Ast(0), Fadd32i(4, -1), Fadd32i(5, -1), Fadd32i(6, 0), Fadd32i(7, 1), Ast4Pos(4), emit,
            Fadd32i(4, 3), Ast4Pos(4), emit, Fadd32i(4, -1), Fadd32i(5, 3), Ast4Pos(4), emitAndCut,
        };
        // 仍把读到的 loc0 写到 loc1，防止编译器因读值未使用而合法消掉 GS 输入。
        if (omitVaryingOutput) ops[1] = Ast(1);
        if (uniform) ops.Insert(1, Ldc(0, 0));
        // Maxwell AttributeConsts.ViewportIndex = 0x068；写到 GS 输出，不能以默认为 viewport 0 冒充。
        if (viewportIndex) ops.Insert(2, (Ast(0) & ~(0x7ffUL << 20)) | (0x68UL << 20));
        byte[] code = Guest(false, ops.ToArray());
        BinaryPrimitives.WriteUInt32LittleEndian(code, 1u | (3u << 5) | (4u << 10));
        BinaryPrimitives.WriteUInt32LittleEndian(code.AsSpan(8), 1u << 24);
        BinaryPrimitives.WriteUInt32LittleEndian(code.AsSpan(12), 7u << 24);
        BinaryPrimitives.WriteUInt32LittleEndian(code.AsSpan(16), 3);
        return code;
    }

    // 从实际模块的 Location/OpVariable/类型声明读跨阶段接口；GS 输入按 SPIR-V 规则多一层顶点数组。
    private static Dictionary<uint, string> ShaderInterface(JsonElement stage, uint storage, bool stripVertexArray = false)
    {
        uint[] words = MemoryMarshal.Cast<byte, uint>(stage.GetProperty("spirv").GetBytesFromBase64()).ToArray();
        var instructions = new List<uint[]>();
        for (int i = 5; i < words.Length;)
        {
            int count = (int)(words[i] >> 16);
            if (count == 0 || i + count > words.Length) throw new InvalidDataException("invalid SPIR-V instruction");
            instructions.Add(words[i..(i + count)]); i += count;
        }
        var types = instructions.Where(x => (x[0] & 0xffff) is 21 or 22 or 23 or 28 or 32).ToDictionary(x => x[1]);
        var locations = instructions.Where(x => (x[0] & 0xffff) == 71 && x.Length >= 4 && x[2] == 30)
            .ToDictionary(x => x[1], x => x[3]);
        string Signature(uint id)
        {
            uint[] t = types[id];
            return (t[0] & 0xffff) switch
            {
                21 => $"int{t[2]}:{t[3]}", 22 => $"float{t[2]}",
                23 => $"vec{t[3]}<{Signature(t[2])}>",
                _ => throw new InvalidDataException("unexpected SPIR-V interface type " + (t[0] & 0xffff)),
            };
        }
        var result = new Dictionary<uint, string>();
        foreach (uint[] v in instructions.Where(x => (x[0] & 0xffff) == 59 && x.Length >= 4 && x[3] == storage))
        {
            if (!locations.TryGetValue(v[2], out uint loc)) continue;
            uint[] pointer = types[v[1]];
            if ((pointer[0] & 0xffff) != 32 || pointer[2] != storage) throw new InvalidDataException("invalid interface pointer");
            uint typeId = pointer[3];
            if (stripVertexArray)
            {
                uint[] array = types[typeId];
                if ((array[0] & 0xffff) != 28) throw new InvalidDataException("GS input is not a vertex array");
                typeId = array[2];
            }
            result.Add(loc, Signature(typeId));
        }
        return result;
    }

    private static HashSet<(uint set, uint binding)> ShaderBindings(JsonElement stage)
    {
        uint[] words = MemoryMarshal.Cast<byte, uint>(stage.GetProperty("spirv").GetBytesFromBase64()).ToArray();
        var sets = new Dictionary<uint, uint>();
        var bindings = new Dictionary<uint, uint>();
        for (int i = 5; i < words.Length;)
        {
            int count = (int)(words[i] >> 16);
            if (count == 0 || i + count > words.Length) throw new InvalidDataException("invalid SPIR-V instruction");
            if ((words[i] & 0xffff) == 71 && count >= 4)
            {
                if (words[i + 2] == 34) sets[words[i + 1]] = words[i + 3];
                if (words[i + 2] == 33) bindings[words[i + 1]] = words[i + 3];
            }
            i += count;
        }
        return sets.Where(p => bindings.ContainsKey(p.Key)).Select(p => (p.Value, bindings[p.Key])).ToHashSet();
    }

    private static uint GeometryInputArrayLength(JsonElement stage, uint location)
    {
        uint[] words = MemoryMarshal.Cast<byte, uint>(stage.GetProperty("spirv").GetBytesFromBase64()).ToArray();
        var instructions = new List<uint[]>();
        for (int i = 5; i < words.Length;)
        {
            int count = (int)(words[i] >> 16);
            if (count == 0 || i + count > words.Length) throw new InvalidDataException("invalid SPIR-V instruction");
            instructions.Add(words[i..(i + count)]); i += count;
        }
        var locations = instructions.Where(x => (x[0] & 0xffff) == 71 && x.Length >= 4 && x[2] == 30 && x[3] == location)
            .Select(x => x[1]).ToHashSet();
        uint[] variable = instructions.Single(x => (x[0] & 0xffff) == 59 && x.Length >= 4 && x[3] == 1 && locations.Contains(x[2]));
        uint[] pointer = instructions.Single(x => (x[0] & 0xffff) == 32 && x[1] == variable[1]);
        uint[] array = instructions.Single(x => (x[0] & 0xffff) == 28 && x[1] == pointer[3]);
        uint[] constant = instructions.Single(x => (x[0] & 0xffff) == 43 && x[2] == array[3]);
        return constant[3];
    }

    // 原创合成 CS，不来自游戏：编码依据本仓 Ryujinx.Graphics.Shader 的
    // Decoders/InstTable.cs、InstDecoders.cs 及 Instructions/InstEmitMemory.cs。
    // LDC.64 R0:R1,c[0][offset]; LDG.E.32 R2,[R0]; STG.E.32 [R0+4],R2;
    // MOV32I R3,42; STG.E.32 [R0+8],R3; EXIT。独立语义：复制 word0 到 word1，word2=42。
    // 每 32 B 的零 scheduler 仅供软件 Decoder 跳过，不宣称可直接在 Maxwell 硬件运行。
    private static byte[] ComputeStorageGuest(int cbOffset, bool prefix = false)
    {
        var ops = new List<ulong> {
            0xEF9500000007FF00UL | ((ulong)cbOffset << 20),
            0xEED4200000070002UL, 0xEEDC200000470002UL,
            0x0100000002A7F003UL, prefix ? 0xEEDC200000070003UL : 0xEEDC200000870003UL, Exit,
        };
        // Original IADD32I R0, R0, -4: force an actual prefix offset calculation.
        if (prefix) ops.Insert(1, 0x1C0FFFFFFFC70000UL);
        byte[] code = new byte[((ops.Count + 2) / 3) * 32];
        for (int i = 0; i < ops.Count; i++)
            BinaryPrimitives.WriteUInt64LittleEndian(code.AsSpan((i / 3 * 4 + i % 3 + 1) * 8), ops[i]);
        return code;
    }

    // Original SPIR-V analysis fixtures: exact Uniform struct/array/vector ABI,
    // not claimed to be executable modules. IDs/literals are deliberately disjoint.
    private static byte[] C0ReadFixture(int offset, bool vectorLoad = false, bool dynamic = false,
        bool load = true, bool escape = false)
    {
        var w = new List<uint> { 0x07230203, 0x10000, 0, 2000, 0 };
        void Emit(uint op, params uint[] args) { w.Add(((uint)args.Length + 1) << 16 | op); w.AddRange(args); }
        Emit(17, 120); // Literal deliberately equals the later access-chain pointer ID.
        Emit(71, 100, 33, 1); Emit(71, 100, 34, 0);
        Emit(71, 103, 6, 16); Emit(72, 104, 0, 35, 0);
        Emit(21, 101, 32, 0); Emit(23, 102, 101, 4);
        Emit(43, 101, 110, 4096); Emit(28, 103, 102, 110);
        Emit(30, 104, 103); Emit(32, 105, 2, 104);
        Emit(32, 106, 2, 101); Emit(32, 107, 2, 102);
        Emit(43, 101, 111, 0); Emit(43, 101, 112, (uint)offset / 16);
        Emit(43, 101, 113, (uint)offset % 16 / 4);
        Emit(59, 105, 100, 2);
        if (vectorLoad) Emit(65, 107, 120, 100, 111, dynamic ? 199u : 112u);
        else Emit(65, 106, 120, 100, 111, dynamic ? 199u : 112u, 113);
        if (load) Emit(61, vectorLoad ? 102u : 101u, 121, 120);
        if (escape) Emit(83, 106, 122, 120);
        return MemoryMarshal.AsBytes(w.ToArray().AsSpan()).ToArray();
    }

    // 实机 cs-1-1-7069 结构的合成等价物（无游戏数据）：c0 变量在 (set0,binding1)
    // 读 4 个 vec4 下标共 6 个分量区间；另一个 UBO（仿 cp_s1）在 (set1,binding1)
    // 按非常量下标动态索引——同 binding 号、不同描述符集。粗闸门缺 set 维度时
    // 会借后者的 dynamic 把 c0Reads 判成 null。
    private static byte[] C0ReadSetCollisionFixture()
    {
        var w = new List<uint> { 0x07230203, 0x10000, 0, 2000, 0 };
        void Emit(uint op, params uint[] args) { w.Add(((uint)args.Length + 1) << 16 | op); w.AddRange(args); }
        Emit(17, 120);
        Emit(71, 100, 33, 1); Emit(71, 100, 34, 0);   // c0：binding 1 / set 0
        Emit(71, 200, 33, 1); Emit(71, 200, 34, 1);   // 第二个 UBO：同 binding 1 / set 1
        Emit(71, 103, 6, 16); Emit(72, 104, 0, 35, 0);
        Emit(21, 101, 32, 0); Emit(23, 102, 101, 4);
        Emit(43, 101, 110, 4096); Emit(28, 103, 102, 110);
        Emit(30, 104, 103); Emit(32, 105, 2, 104);
        Emit(32, 106, 2, 101);
        Emit(43, 101, 111, 0);   // member 0
        Emit(43, 101, 112, 41); Emit(43, 101, 114, 46); Emit(43, 101, 115, 49); Emit(43, 101, 116, 53);
        Emit(43, 101, 113, 0); Emit(43, 101, 117, 1);   // component 0 / 1
        Emit(59, 105, 100, 2);   // c0 变量
        Emit(59, 105, 200, 2);   // 第二个 UBO 变量（同一指针类型）
        uint id = 120;
        foreach (var (index, component) in new[] { (112u, 113u), (114u, 113u), (114u, 117u),
                                                 (115u, 113u), (116u, 113u), (116u, 117u) })
        {
            Emit(65, 106, id, 100, 111, index, component);
            Emit(61, 101, id + 1, id);
            id += 2;
        }
        Emit(65, 106, 140, 200, 111, 199);   // data[动态]：下标 id 199 不是常量
        Emit(61, 101, 141, 140);
        return MemoryMarshal.AsBytes(w.ToArray().AsSpan()).ToArray();
    }

    // Original heterogeneous std140 SupportBuffer analysis fixture, not executable.
    // Independent layout: 12 members, scalar arrays padded to stride 16, vec4s 16 B.
    private static byte[] SupportReadFixture(int member, int index = 0, bool load = true,
        bool dynamic = false, bool escape = false, bool vectorLoad = false,
        uint stride = 16, bool forwardEscape = false, bool integerScale = false, uint scaleCount = 73)
    {
        var w = new List<uint> { 0x07230203, 0x10300, 0, 2000, 0 };
        void Emit(uint op, params uint[] args) { w.Add(((uint)args.Length + 1) << 16 | op); w.AddRange(args); }
        Emit(17, 120); // Capability literal is deliberately equal to the access-chain ID.
        Emit(71, 100, 33, 0); Emit(71, 100, 34, 0);
        Emit(21, 101, 32, 0); Emit(22, 102, 32); Emit(23, 103, 101, 4); Emit(23, 104, 102, 4);
        Emit(43, 101, 110, 8); Emit(43, 101, 111, scaleCount);
        Emit(28, 105, 101, 110); Emit(28, 106, integerScale ? 101u : 102u, 111);
        Emit(71, 105, 6, stride); Emit(71, 106, 6, stride);
        uint[] members = { 101, 105, 104, 104, 101, 106, 103, 101, 103, 103, 103, 106 };
        uint[] offsets = { 0, 16, 144, 160, 176, 192, 1360, 1376, 1392, 1408, 1424, 1440 };
        Emit(30, new uint[] { 107 }.Concat(members).ToArray());
        for (uint i = 0; i < offsets.Length; i++) Emit(72, 107, i, 35, offsets[i]);
        Emit(32, 108, 2, 107); Emit(59, 108, 100, 2);
        bool array = member == 1 || member == 5 || member == 11;
        bool vector = member == 2 || member == 3 || member == 6 || member >= 8 && member <= 10;
        uint leaf = array ? (member == 1 ? 101u : 102u) : members[member];
        if (vector && !vectorLoad) leaf = member == 2 || member == 3 ? 102u : 101u;
        Emit(32, 109, 2, leaf);
        Emit(43, 101, 112, (uint)member); Emit(43, 101, 113, (uint)index);
        if (forwardEscape) Emit(83, 109, 122, 120);
        var chain = new List<uint> { 109, 120, 100, 112 };
        if (array || vector && !vectorLoad) chain.Add(dynamic ? 199u : 113u);
        Emit(65, chain.ToArray());
        if (load) Emit(61, leaf, 121, 120);
        if (escape) Emit(83, 109, 123, 120);
        return MemoryMarshal.AsBytes(w.ToArray().AsSpan()).ToArray();
    }

    private static byte[] ComputeTextureGuest()
    {
        ulong[] ops = { 0xEF9500310007FF00UL, Fadd32i(4, 0.5f), Fadd32i(5, 0.5f),
            Tex2D(2, 4), 0xEEDC200000070002UL, Exit };
        byte[] code = new byte[((ops.Length + 2) / 3) * 32];
        for (int i = 0; i < ops.Length; i++)
            BinaryPrimitives.WriteUInt64LittleEndian(code.AsSpan((i / 3 * 4 + i % 3 + 1) * 8), ops[i]);
        return code;
    }

    // 两张纹理：TEX 2（红）写 R0-R3、TEX 3（绿）写 R4-R7；R/G 逐通道相加得黄，B/A 直接写常量。
    // 注意 TEX 会覆盖坐标寄存器 R2/R3，所以第二次采样前要重设 UV。
    // NVN 保留纹理池 0 号为 null（引擎也按 id==0 判空），1 号槽被单纹理例占用，双纹理用 2/3 号。
    private static byte[] FsTwoTextures() => Guest(true, new[]
    {
        Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(0, 2, 2),
        Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(4, 2, 3),
        FaddR(0, 0, 4), FaddR(1, 1, 5), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f),
    });
    private const string TwoTextureFacts =
        "[{\"stage\":\"fs\",\"handle\":2,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}," +
        "{\"stage\":\"fs\",\"handle\":3,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";

    // 从 guest 常量缓冲 c[0] 读两个 32 位字当 R/G：UBO 绑 1.0f/1.0f 时出黄，绑 0 时出黑。
    // 位型与数值两种译法在 1.0f 上都得到 1.0，所以颜色不依赖翻译器选哪种转换。
    private static byte[] FsUniform() => Guest(true, new[]
    {
        Ldc(0, 0x00), Ldc(1, 0x04), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f),
    });

    private static readonly string[] AttributeTypes = { "Float", "Uint", "Sint", "Sscaled", "Uscaled" };
    private static readonly string[] FragmentTypes = { "Float", "Uint", "Sint" };

    private static byte[] ProfileJson(string profileHash = null, uint maxTextures = 64,
        string closure = null)
    {
        SupportLayout support = SupportLayout.Compile();
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteString("schema", DirectProfile.Schema);
            w.WriteString("profileHash", profileHash ?? "sha256-" + new string('a', 64));
            w.WriteNumber("directABI", 1);
            w.WriteString("translatorClosure", closure ?? DirectBuildIdentity.TranslatorClosure);
            w.WriteString("runtimeCompilerABI", DirectProfile.CompilerAbi);
            w.WriteString("apiVersion", "1.1");
            w.WriteString("spirvVersion", DirectProfile.SpirvVersion);
            w.WriteString("targetEnv", DirectProfile.TargetEnv);
            w.WriteStartObject("supportLayout");
            w.WriteNumber("requiredSize", support.RequiredSize);
            w.WriteNumber("binding", support.Binding);
            w.WriteString("hash", support.Hash);
            w.WriteEndObject();
            w.WriteStartObject("capabilities");
            w.WriteString("vendorName", "synthetic-adreno730");
            w.WriteString("memoryType", "unified");
            bool[] flags =
            {
                false, false, false, false, // frontfacing, vector index, frag output spec, reduce precision
                true, true, true, true, true, true, true, false, false, // astc bc123 bc45 bc67 etc2 3d bgra r4g4 r4g4b4a4
                true, true, true, false, true, false, true, false, false, false, // scaled, snorm, 5bit, sparse, blend adv, interlock, ordering, gs, gs passthrough, gs occ query
                false, true, false, true, true, true, true, true, true, // tf, image load, layer tess, mismatching view, cubemap, nonconst offset, quads, separate sampler, ballot
                false, false, false, true, true, true, false, false, true, true, false, // barrier, f64, nonuniform, gather offsets, shadow lod, vertex stores, viewport index tess, viewport mask, swizzle, indirect, depth clip
                false, // s2aVertexPull
            };
            string[] keys =
            {
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
            };
            for (int i = 0; i < keys.Length; i++) w.WriteBoolean(keys[i], flags[i]);
            w.WriteNumber("uniformBufferSetIndex", 0);
            w.WriteNumber("storageBufferSetIndex", 1);
            w.WriteNumber("textureSetIndex", 2);
            w.WriteNumber("imageSetIndex", 3);
            w.WriteNumber("extraSetBaseIndex", 4);
            w.WriteNumber("maximumExtraSets", 0);
            w.WriteNumber("maximumUniformBuffersPerStage", 18);
            w.WriteNumber("maximumStorageBuffersPerStage", 16);
            w.WriteNumber("maximumTexturesPerStage", maxTextures);
            w.WriteNumber("maximumImagesPerStage", 16);
            w.WriteNumber("maximumComputeSharedMemorySize", 32768);
            w.WriteNumber("maximumSupportedAnisotropy", 16);
            w.WriteNumber("shaderSubgroupSize", 32);
            w.WriteNumber("storageBufferOffsetAlignment", 16);
            w.WriteNumber("textureBufferOffsetAlignment", 16);
            w.WriteNumber("gatherBiasPrecision", 0);
            w.WriteNumber("maximumGpuMemory", 0);
            w.WriteEndObject();
            w.WriteEndObject();
        }
        return stream.ToArray();
    }

    // 真机 Adreno 730 的实测形态用于合成测试（值是可解释的合成输入，不是从设备读的；真机同字节
    // 比对由 CLI profile 与设备日志的 caps 完成）。geometry/interlock 用来证明"硬件支持 ≠ enabled"。
    private static byte[] CapsJson(bool geometry = false, bool interlock = false, uint sampledImages = 64,
        string schema = null, string duplicateField = null, bool gsOccQuery = false)
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteString("schema", schema ?? DirectDeviceCaps.Schema);
            if (duplicateField != null) w.WriteString(duplicateField, "duplicate");
            w.WriteString("apiVersion", "1.1.128");
            w.WriteNumber("driverVersion", 2150002783);
            w.WriteNumber("vendorId", 20803);
            w.WriteNumber("deviceId", 117637122);
            w.WriteNumber("deviceType", 2);
            w.WriteString("deviceName", "Adreno (TM) 730");
            w.WriteString("driverName", "Adreno (TM) 730");
            w.WriteStartObject("limits");
            w.WriteNumber("maxPerStageDescriptorSamplers", 16);
            w.WriteNumber("maxPerStageDescriptorSampledImages", sampledImages);
            w.WriteNumber("maxPerStageDescriptorUniformBuffers", 12);
            w.WriteNumber("maxPerStageDescriptorStorageBuffers", 4);
            w.WriteNumber("maxPerStageDescriptorStorageImages", 4);
            w.WriteNumber("maxUniformBufferRange", 65536);
            w.WriteNumber("maxStorageBufferRange", 134217728);
            w.WriteNumber("maxComputeSharedMemorySize", 32768);
            w.WriteNumber("maxComputeWorkGroupInvocations", 1024);
            w.WriteNumber("maxSamplerAnisotropy", 16.0);
            w.WriteNumber("minUniformBufferOffsetAlignment", 16);
            w.WriteNumber("minStorageBufferOffsetAlignment", 16);
            w.WriteNumber("minTexelBufferOffsetAlignment", 16);
            w.WriteNumber("subgroupSize", 64);
            w.WriteEndObject();
            w.WriteStartObject("features");
            w.WriteBoolean("geometryShader", geometry);
            // #1456 capprobe 能力位是独立设备事实（9000：硬件 GS 有、判定不通过 → 位假）。
            w.WriteBoolean("geometryShaderOcclusionQuery", gsOccQuery);
            w.WriteBoolean("tessellationShader", false);
            w.WriteBoolean("imageCubeArray", true);
            w.WriteBoolean("shaderFloat64", false);
            w.WriteBoolean("shaderInt64", false);
            w.WriteBoolean("fragmentStoresAndAtomics", true);
            w.WriteBoolean("vertexPipelineStoresAndAtomics", true);
            w.WriteBoolean("textureCompressionASTC_LDR", true);
            w.WriteBoolean("textureCompressionBC", true);
            w.WriteBoolean("textureCompressionETC2", true);
            w.WriteBoolean("shaderStorageImageExtendedFormats", true);
            w.WriteBoolean("independentBlend", true);
            w.WriteBoolean("depthClamp", false);
            w.WriteBoolean("multiViewport", false);
            w.WriteBoolean("samplerAnisotropy", true);
            w.WriteBoolean("shaderImageGatherExtended", true);
            w.WriteBoolean("shaderStorageImageReadWithoutFormat", true);
            w.WriteEndObject();
            w.WriteStartObject("formats");
            w.WriteBoolean("bc123", true);
            w.WriteBoolean("bc45", true);
            w.WriteBoolean("bc67", true);
            w.WriteBoolean("etc2", true);
            w.WriteBoolean("astc", true);
            w.WriteBoolean("fiveBit", true);
            w.WriteBoolean("r4g4b4a4", true);
            w.WriteBoolean("scaledVertex", true);
            w.WriteEndObject();
            w.WriteStartObject("memory");
            w.WriteNumber("deviceLocalBytes", 12884901888UL);
            w.WriteNumber("heapCount", 1);
            w.WriteNumber("memoryTypeCount", 3);
            w.WriteBoolean("deviceLocalHostVisible", true);
            w.WriteEndObject();
            w.WriteStartArray("enabledExtensions");
            w.WriteStringValue("VK_KHR_swapchain");
            w.WriteStringValue("VK_EXT_swapchain_maintenance1");
            if (interlock) w.WriteStringValue("VK_EXT_fragment_shader_interlock");
            w.WriteEndArray();
            w.WriteNumber("subgroupSize", 64);
            w.WriteEndObject();
        }
        return stream.ToArray();
    }

    private static byte[] RequestJson(byte[] profile, string kind, ulong session, ulong generation, string requestId,
        byte[] vs = null, byte[] fs = null, byte[] cs = null, string fragmentType = "Float",
        string textureFacts = null, bool transformFeedback = false, uint localSize = 1,
        byte[] overrideCode = null, int controlX = -1, uint fsCbUse = 0, string closure = null,
        string textureBufferLengths = null, uint csCbUse = 0)
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteString("schema", DirectShaderCompiler.RequestSchema);
            w.WriteNumber("session", session);
            w.WriteNumber("generation", generation);
            w.WriteString("requestId", requestId);
            w.WriteNumber("directABI", 1);
            w.WriteString("translatorClosure", closure ?? DirectBuildIdentity.TranslatorClosure);
            w.WriteString("runtimeCompilerABI", DirectProfile.CompilerAbi);
            w.WriteNumber("sourceParserVersion", 1);
            w.WritePropertyName("profile"); w.WriteRawValue(profile);
            w.WriteStartObject("program");
            w.WriteString("kind", kind);
            w.WriteStartArray("stages");
            if (kind == "graphics")
            {
                WriteStage(w, "vs", overrideCode ?? vs, 0);
                if (fs != null || overrideCode != null) WriteStage(w, "fs", overrideCode ?? fs, fsCbUse);   // fs 不给 = 只有顶点阶段的程序
            }
            else
            {
                byte[] control = new byte[0x800];
                uint cx = controlX < 0 ? localSize : (uint)controlX;
                BinaryPrimitives.WriteUInt32LittleEndian(control.AsSpan(0x748), cx);
                BinaryPrimitives.WriteUInt32LittleEndian(control.AsSpan(0x74C), 1);
                BinaryPrimitives.WriteUInt32LittleEndian(control.AsSpan(0x750), 1);
                BinaryPrimitives.WriteUInt32LittleEndian(control.AsSpan(0x754), 0);
                // 让 controlX=0 明确产生非法控制段；其它情况如实填写。
                WriteStage(w, "cs", overrideCode ?? cs, csCbUse, control);
            }
            w.WriteEndArray();
            if (kind == "graphics")
            {
                w.WriteStartObject("graphicsState");
                w.WriteBoolean("earlyZForce", false);
                w.WriteString("topology", "Triangles");
                w.WriteBoolean("tessCw", false);
                w.WriteString("tessPatchType", "Triangles");
                w.WriteString("tessSpacing", "EqualSpacing");
                w.WriteBoolean("alphaToCoverageEnable", false);
                w.WriteBoolean("alphaToCoverageDitherEnable", false);
                w.WriteBoolean("viewportTransformDisable", false);
                w.WriteBoolean("depthMode", false);
                w.WriteBoolean("programPointSizeEnable", false);
                w.WriteNumber("pointSize", 1);
                w.WriteString("alphaTestCompare", "Always");
                w.WriteNumber("alphaTestReference", 0);
                w.WriteStartArray("attributeTypes");
                for (int i = 0; i < 32; i++) w.WriteStringValue(AttributeTypes[0]);
                w.WriteEndArray();
                w.WriteBoolean("hasConstantBufferDrawParameters", false);
                w.WriteStartArray("fragmentOutputTypes");
                for (int i = 0; i < 8; i++) w.WriteStringValue(i == 0 ? fragmentType : "Float");
                w.WriteEndArray();
                w.WriteBoolean("dualSourceBlendEnable", false);
                w.WriteBoolean("yNegateEnabled", false);
                w.WriteBoolean("originUpperLeft", true);
                w.WriteBoolean("halvePrimitiveId", false);
                w.WriteBoolean("hasUnalignedStorageBuffer", false);
                w.WriteEndObject();
            }
            else
            {
                w.WriteStartObject("computeState");
                w.WriteStartArray("localSize");
                w.WriteNumberValue(localSize); w.WriteNumberValue(1); w.WriteNumberValue(1);
                w.WriteEndArray();
                w.WriteNumber("localMemoryBytes", 0);
                w.WriteNumber("sharedMemoryBytes", 0);
                w.WriteBoolean("hasUnalignedStorageBuffer", false);
                w.WriteEndObject();
            }
            if (textureFacts == null) { w.WriteStartArray("textures"); w.WriteEndArray(); }
            else { w.WritePropertyName("textures"); w.WriteRawValue(textureFacts); }
            if (textureBufferLengths != null)
            {
                w.WritePropertyName("textureBufferLengths");
                w.WriteRawValue(textureBufferLengths);
            }
            w.WriteNumber("samplerArrayLength", 8);
            w.WriteNumber("textureArrayLength", 64);
            w.WriteBoolean("transformFeedbackEnabled", transformFeedback);
            w.WriteEndObject();
            w.WriteEndObject();
        }
        return stream.ToArray();
    }

    private static void WriteStage(Utf8JsonWriter w, string stage, byte[] code, uint cbUse, byte[] control = null)
    {
        w.WriteStartObject();
        w.WriteString("stage", stage);
        w.WriteBase64String("code", code);
        w.WriteBase64String("control", control ?? new byte[0x800]);
        w.WriteBase64String("c1", Array.Empty<byte>());
        w.WriteNumber("constantBufferUse", cbUse);
        w.WriteEndObject();
    }

    private static JsonDocument ParseResult(byte[] result) => JsonDocument.Parse(result);

    private static byte[] Content(JsonDocument document)
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream)) document.RootElement.GetProperty("content").WriteTo(w);
        return stream.ToArray();
    }

    private static byte[] _emitted;

    [UnmanagedCallersOnly]
    private static int Emit(nint sink, DirectBytes result)
    {
        if (sink != 0 || result.Data == 0 || result.Bytes == 0) return -1;
        _emitted = new ReadOnlySpan<byte>((void*)result.Data, checked((int)result.Bytes)).ToArray();
        return 0;
    }

    private static void Main(string[] args)
    {
        byte[] profile = ProfileJson();
        byte[] vs = Guest(false, new[] { Ast(0) });
        byte[] fs = Guest(true, new[] { Ald(0) }, new[] { 0 });
        byte[] cs = ComputeGuest();

        if (args.Length == 1 && args[0] == "--legacy-artifact")
        {
            byte[] request = RequestJson(profile, "graphics", 1, 1, "legacy-artifact", vs, FsTwoTextures(), textureFacts: TwoTextureFacts);
            Console.WriteLine(Encoding.UTF8.GetString(DirectShaderCompiler.Compile(request)));
            return;
        }

        if (args.Length == 2 && args[0] == "--emit-requests")
        {
            string directory = Path.GetFullPath(args[1]);
            if (Directory.Exists(directory) || File.Exists(directory))
                throw new InvalidOperationException("request output already exists: " + directory);
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "profile.json"), profile);
            File.WriteAllBytes(Path.Combine(directory, "graphics.json"),
                RequestJson(profile, "graphics", 1, 1, "cli-graphics", vs, fs));
            File.WriteAllBytes(Path.Combine(directory, "compute.json"),
                RequestJson(profile, "compute", 2, 1, "cli-compute", cs: cs));
            File.WriteAllBytes(Path.Combine(directory, "compute-storage-aligned.json"),
                RequestJson(profile, "compute", 2, 1, "c0-aligned", cs: ComputeStorageGuest(0x310), csCbUse: 1));
            File.WriteAllText(Path.Combine(directory, "compute-storage-prefix.json"),
                Encoding.UTF8.GetString(RequestJson(profile, "compute", 2, 1, "c0-prefix",
                    cs: ComputeStorageGuest(0x310, prefix: true), csCbUse: 1))
                    .Replace("\"hasUnalignedStorageBuffer\":false", "\"hasUnalignedStorageBuffer\":true"));
            const string csTexFact = "[{\"stage\":\"cs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            File.WriteAllBytes(Path.Combine(directory, "compute-support-texture.json"),
                RequestJson(profile, "compute", 2, 1, "support-texture", cs: ComputeTextureGuest(),
                    csCbUse: 1, textureFacts: csTexFact));
            byte[] fsRed = Guest(true, new[] { Fadd32i(0, 1.0f), Fadd32i(1, 0.0f), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f) });
            File.WriteAllBytes(Path.Combine(directory, "c2draw.json"),
                RequestJson(profile, "graphics", 1, 1, "c2draw", vs, fsRed));
            File.WriteAllBytes(Path.Combine(directory, "c2vs.json"),
                RequestJson(profile, "graphics", 1, 1, "c2vs", Guest(false, Array.Empty<ulong>()), fsRed));
            byte[] vsPos = Guest(false, new[] { Ald4(0), Ast4Pos(0) });
            File.WriteAllBytes(Path.Combine(directory, "c2vspos.json"),
                RequestJson(profile, "graphics", 1, 1, "c2vspos", vsPos, fsRed));
            byte[] fsGreen = Guest(true, new[] { Fadd32i(0, 0.0f), Fadd32i(1, 1.0f), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f) });
            File.WriteAllBytes(Path.Combine(directory, "c2green.json"),
                RequestJson(profile, "graphics", 1, 1, "c2green", vsPos, fsGreen));
            byte[] fsTex = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(0, 2) });
            const string texFact = "[{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            File.WriteAllBytes(Path.Combine(directory, "c2tex.json"),
                RequestJson(profile, "graphics", 1, 1, "c2tex", vsPos, fsTex, textureFacts: texFact));
            byte[] fsTex1 = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(0, 2, 1) });
            const string texFact1 = "[{\"stage\":\"fs\",\"handle\":1,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            File.WriteAllBytes(Path.Combine(directory, "c2tex1.json"),
                RequestJson(profile, "graphics", 1, 1, "c2tex1", vsPos, fsTex1, textureFacts: texFact1));
            // 两张纹理同时采样：R 取纹理0、G 取纹理1（红+绿=黄），证明多纹理槽位/绑定。
            File.WriteAllBytes(Path.Combine(directory, "c2tex2.json"),
                RequestJson(profile, "graphics", 1, 1, "c2tex2", vsPos, FsTwoTextures(), textureFacts: TwoTextureFacts));
            // guest UBO：fs 用 c[0]，constantBufferUse 置 bit0。
            File.WriteAllBytes(Path.Combine(directory, "c2ubo.json"),
                RequestJson(profile, "graphics", 1, 1, "c2ubo", vsPos, FsUniform(), fsCbUse: 1));
            Console.WriteLine("wrote synthetic requests to " + directory);
            return;
        }

        if (args.Length == 2 && args[0] == "--emit-caps")
        {
            string directory = Path.GetFullPath(args[1]);
            if (Directory.Exists(directory) || File.Exists(directory))
                throw new InvalidOperationException("caps output already exists: " + directory);
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "caps.json"), CapsJson());
            Console.WriteLine("wrote synthetic device caps to " + directory);
            return;
        }

        // A large pool must not prevent a program with constant texture handles from compiling.
        {
            byte[] fsConstant = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(0, 2) });
            const string texFact = "[{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            string raw = Encoding.UTF8.GetString(RequestJson(profile, "graphics", 1, 1, "large-pool", vs, fsConstant, textureFacts: texFact));
            foreach (int length in new[] { 524288, 1048576 })
            {
                string request = raw.Replace("\"textureArrayLength\":64", "\"textureArrayLength\":" + length)
                                    .Replace("\"samplerArrayLength\":8", "\"samplerArrayLength\":" + length);
                try
                {
                    using JsonDocument largeResult = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(request)));
                    var textures = largeResult.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                        .Where(b => b.GetProperty("kind").GetString() == "texture").ToArray();
                    Check(textures.Length == 1 && textures[0].GetProperty("count").GetInt32() == 1,
                        "large pool " + length + " compiles constant handle as one descriptor");
                }
                catch (Exception error) { Check(false, "large pool " + length + " constant handle: " + error.Message); }
            }
            string tooLarge = raw.Replace("\"textureArrayLength\":64", "\"textureArrayLength\":1048577");
            Throws("MISSING_FIELD", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(tooLarge)), "texture pool above cap rejected");
            tooLarge = raw.Replace("\"samplerArrayLength\":8", "\"samplerArrayLength\":1048577");
            Throws("MISSING_FIELD", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(tooLarge)), "sampler pool above cap rejected");

            // The real translator produces a runtime pool array, but this input currently hits
            // the independent cbuf-slot limitation before a program result is emitted.
            byte[] fsDynamic = Guest(true, new[] { Ald(1, 0, 4), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 4) }, new[] { 1 });
            string dynamicRequest = Encoding.UTF8.GetString(RequestJson(profile, "graphics", 1, 1, "runtime-pool", vs, fsDynamic, textureFacts: texFact))
                .Replace("\"textureArrayLength\":64", "\"textureArrayLength\":524288");
            try
            {
                DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(dynamicRequest));
                Check(false, "runtime handle must not claim an unsupported cbuf layout is usable");
            }
            catch (DirectCompileException error) {
                Check(error.Code == "UNSUPPORTED_PROGRAM" && error.Message.Contains("texture handle from cbuf slot 0"),
                    "runtime handle reports its actual upstream cbuf limitation: " + error.Message);
            }
        }

        // 1) 接口全覆盖（接口新增方法时这里先红）。
        var interfaceMethods = typeof(IGpuAccessor).GetMethods().Where(m => m.DeclaringType == typeof(IGpuAccessor)).ToList();
        var classMethods = typeof(DirectGpuAccessor).GetMethods().Where(m => m.DeclaringType == typeof(DirectGpuAccessor))
            .ToDictionary(m => m.Name + "/" + string.Join(",", m.GetParameters().Select(p => p.ParameterType.Name)), StringComparer.Ordinal);
        foreach (var method in interfaceMethods)
        {
            string key = method.Name + "/" + string.Join(",", method.GetParameters().Select(p => p.ParameterType.Name));
            Check(classMethods.ContainsKey(key), "DirectGpuAccessor overrides " + key);
        }
        Check(interfaceMethods.Count >= 50, "interface method inventory saw " + interfaceMethods.Count);

        // 1b) enabled profile 合成：设备 caps + 编译器常量/哈希。硬件支持不等于 enabled，
        //     同一 caps 必须给出同一 profileHash，且合成结果能被请求消费者 DirectProfile.Parse 接受。
        {
            const string closure = "synthetic-closure-v1";
            byte[] caps = CapsJson();
            byte[] composed = DirectProfileComposer.Compose(caps, closure);
            using (JsonDocument doc = JsonDocument.Parse(composed))
            {
                JsonElement root = doc.RootElement;
                Check(root.GetProperty("schema").GetString() == DirectProfile.Schema, "composed profile schema");
                string hash = root.GetProperty("profileHash").GetString();
                Check(hash.Length == 71 && hash.StartsWith("sha256-", StringComparison.Ordinal),
                    "composed profileHash is sha256-<64hex>");
                Check(root.GetProperty("supportLayout").GetProperty("hash").GetString() == SupportLayout.Compile().Hash,
                    "composed supportLayout.hash is this translator closure's layout");
                Check(root.GetProperty("runtimeCompilerABI").GetString() == DirectProfile.CompilerAbi, "composed compiler ABI");
                Check(root.GetProperty("translatorClosure").GetString() == closure, "composed translator closure");
                Check(root.GetProperty("targetEnv").GetString() == DirectProfile.TargetEnv, "composed target env");
                Check(root.GetProperty("apiVersion").GetString() == "1.1", "composed apiVersion");
                JsonElement profileCaps = root.GetProperty("capabilities");
                Check(profileCaps.GetProperty("hasVectorIndexingBug").GetBoolean(), "qualcomm vector indexing bug is enabled");
                Check(!profileCaps.GetProperty("supportsGeometryShader").GetBoolean(), "geometry is not enabled");
                Check(!profileCaps.GetProperty("supportsFragmentShaderInterlock").GetBoolean(),
                    "interlock is not enabled when the extension is absent");
                Check(profileCaps.GetProperty("maximumUniformBuffersPerStage").GetUInt32() == 12,
                    "descriptor limits follow the device");
                Check(profileCaps.GetProperty("maximumTexturesPerStage").GetUInt32() == 64, "texture limit clamped to the layout");
                DirectProfile.Parse(root, "composed");
            }
            Check(composed.SequenceEqual(DirectProfileComposer.Compose(CapsJson(), closure)),
                "same caps compose the same profile bytes");
            byte[] gsEnabled = DirectProfileComposer.Compose(CapsJson(geometry: true), closure);
            Check(!composed.SequenceEqual(gsEnabled), "enabled geometry changes the profile");
            using (JsonDocument enabled = JsonDocument.Parse(gsEnabled))
                Check(DirectProfile.Parse(enabled.RootElement, "gs profile").Caps.SupportsGeometryShader,
                    "enabled geometry reaches compiler profile");
            // #1456：capprobe 位是写进 profile 的设备事实——同 caps 不同判定必须产出不同 profileHash。
            byte[] gsOcc = DirectProfileComposer.Compose(CapsJson(geometry: true, gsOccQuery: true), closure);
            Check(!composed.SequenceEqual(gsOcc), "gs occlusion-query bit must change the profile");
            Check(JsonDocument.Parse(gsOcc).RootElement.GetProperty("capabilities")
                .GetProperty("supportsGeometryShaderOcclusionQuery").GetBoolean(),
                "gs occlusion-query bit recorded in profile capabilities");
            byte[] interlock = DirectProfileComposer.Compose(CapsJson(interlock: true), closure);
            Check(!composed.SequenceEqual(interlock), "enabling the interlock extension changes the profile");
            Check(JsonDocument.Parse(interlock).RootElement.GetProperty("capabilities")
                .GetProperty("supportsFragmentShaderInterlock").GetBoolean(), "interlock follows the enabled extension");
            Check(!composed.SequenceEqual(DirectProfileComposer.Compose(CapsJson(sampledImages: 32), closure)),
                "different device limits compose a different profile");
            Check(JsonDocument.Parse(DirectProfileComposer.Compose(CapsJson(sampledImages: 32), closure))
                .RootElement.GetProperty("capabilities").GetProperty("maximumTexturesPerStage").GetUInt32() == 32,
                "texture limit follows a smaller device limit");
            // 真机（Adreno 730）报 maxPerStageDescriptorSampledImages=524288；设备上限高于布局时按布局封顶。
            Check(JsonDocument.Parse(DirectProfileComposer.Compose(CapsJson(sampledImages: 524288), closure))
                .RootElement.GetProperty("capabilities").GetProperty("maximumTexturesPerStage").GetUInt32() == 64,
                "a device limit above the descriptor layout clamps to 64");
            Throws("UNSUPPORTED_PROFILE", () => DirectProfileComposer.Compose(CapsJson(schema: "s2a-device-caps-v2"), closure),
                "unknown caps schema rejected");
            Throws("DUPLICATE_KEY", () => DirectProfileComposer.Compose(
                Encoding.UTF8.GetBytes("{\"schema\":\"s2a-device-caps-v1\",\"schema\":\"s2a-device-caps-v1\"}"), closure),
                "duplicate caps key rejected");
            Throws("MISSING_FIELD", () => DirectProfileComposer.Compose(
                Encoding.UTF8.GetBytes("{\"schema\":\"s2a-device-caps-v1\"}"), closure), "caps without fields rejected");
            Throws("UNSUPPORTED_PROFILE", () => DirectProfileComposer.Compose(caps, "bad closure"), "invalid closure rejected");
        }

        // 1c) 翻译闭包身份（W2.1）：构建期嵌入的闭包是唯一来源，请求里的字符串必须等于它。
        {
            string closure = DirectBuildIdentity.TranslatorClosure;
            Check(DirectBuildIdentity.Rule == "s2a-direct-closure-v2", "closure rule version");
            // v2：构建选项必须进身份（工程文件已在 targets 的输入清单里）。
            Check(DirectBuildIdentity.CompilerOptions.Contains("DefineConstants="), "closure records build options");
            Check(closure.StartsWith("s2a-dc1-", StringComparison.Ordinal) && closure.Length == 8 + 64 &&
                closure.AsSpan(8).ToString().All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f'),
                "translator closure is s2a-dc1-<64 lowercase hex> (" + closure + ")");
            Check(DirectBuildIdentity.SourceCount > 0 && DirectBuildIdentity.SourceBytes > 0,
                "closure covers " + DirectBuildIdentity.SourceCount + " sources");
            Check(DirectIdentity.TranslatorClosure == closure, "identity accessor reads the embedded value");
            using (JsonDocument identity = JsonDocument.Parse(DirectIdentity.Json()))
            {
                JsonElement root = identity.RootElement;
                Check(root.GetProperty("schema").GetString() == DirectIdentity.Schema, "identity JSON schema");
                Check(root.GetProperty("translatorClosure").GetString() == closure,
                    "identity JSON carries the embedded closure");
                Check(root.GetProperty("sourcesDigest").GetString() == DirectBuildIdentity.SourcesDigest,
                    "identity JSON carries the sources digest");
                string mvid = root.GetProperty("moduleVersionId").GetString();
                Check(mvid != null && mvid.Length == 32 && mvid.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f'),
                    "identity JSON carries a lowercase 32-hex module version id (" + mvid + ")");
                Check(mvid == DirectIdentity.ModuleVersionId, "module version id is this build's");
                Check(!root.GetProperty("sourcesDigest").GetString().Contains(mvid),
                    "binary identity is reported separately from the source closure");
                Check(root.GetProperty("supportLayout").GetString() == SupportLayout.Compile().Hash,
                    "identity JSON carries this build's support layout");
            }
            // 旧的自洽假身份必须被拒绝：请求与 profile 都写同一个按游戏手写的值也不放行。
            byte[] staleProfile = ProfileJson(closure: "synthetic-closure-v1");
            Throws("IDENTITY", () => DirectShaderCompiler.Compile(
                RequestJson(staleProfile, "graphics", 1, 1, "x", vs, fs, closure: "synthetic-closure-v1")),
                "self-consistent but non-build closure rejected");
            Throws("IDENTITY", () => DirectShaderCompiler.Compile(
                RequestJson(profile, "graphics", 1, 1, "x", vs, fs, closure: "synthetic-closure-v1")),
                "request closure differing from the build rejected");
        }

        // 2) 合成 VS/FS 编译；SPIR-V/反射/依赖/支持缓冲。
        byte[] graphicsRequest = RequestJson(profile, "graphics", 1, 1, "req-graphics-1", vs, fs);
        // 先在真实请求解析器证明第三阶段不是被静默丢弃：没有替身解析器、没有游戏数据。
        // 红线在 program.stages 的 GS 拒绝；后续还须经真实翻译检验接口与拓扑。
        {
            byte[] gs = GeometryGuest();
            using var stageStream = new MemoryStream();
            using (var w = new Utf8JsonWriter(stageStream)) WriteStage(w, "gs", gs, 0);
            var requestNode = System.Text.Json.Nodes.JsonNode.Parse(graphicsRequest);
            var oldStages = requestNode["program"]["stages"].AsArray();
            requestNode["program"]["stages"] = new System.Text.Json.Nodes.JsonArray(
                oldStages[0].DeepClone(), oldStages[1].DeepClone(),
                System.Text.Json.Nodes.JsonNode.Parse(stageStream.ToArray()), null, null);
            requestNode["schema"] = DirectShaderCompiler.GraphicsRequestSchemaV2;
            string request = requestNode.ToJsonString();
            using JsonDocument requestDoc = JsonDocument.Parse(request);
            using JsonDocument profileDoc = JsonDocument.Parse(profile);
            try
            {
                DirectProfile parsedProfile = DirectProfile.Parse(profileDoc.RootElement, "profile");
                DirectProgram parsed = DirectProgram.Parse(requestDoc.RootElement.GetProperty("program"), "program", parsedProfile, 2);
                Check(parsed.Stages.Length == 3 && parsed.StageOf("gs") != null &&
                    parsed.StageOf("gs").Code.SequenceEqual(gs), "GS request preserves all three stages and exact GS code");
            }
            catch (DirectCompileException ex)
            {
                Console.Error.WriteLine("GS request rejected: " + ex.Code + " " + ex.Message);
                Check(false, "GS request preserves all three stages and exact GS code");
            }

            Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(request)),
                "GS request rejected when geometry is not enabled");
            byte[] enabledProfile = DirectProfileComposer.Compose(CapsJson(geometry: true), DirectBuildIdentity.TranslatorClosure);
            requestNode["profile"] = System.Text.Json.Nodes.JsonNode.Parse(enabledProfile);
            byte[] enabledRequest = Encoding.UTF8.GetBytes(requestNode.ToJsonString());
            {
                var badSlot = requestNode.DeepClone();
                var badStages = badSlot["program"]["stages"].AsArray();
                (badStages[1], badStages[2]) = (badStages[2].DeepClone(), badStages[1].DeepClone());
                Throws("INVALID_JSON", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(badSlot.ToJsonString())),
                    "v2 rejects a stage in the wrong NVN slot");
                var shortSlots = requestNode.DeepClone();
                shortSlots["program"]["stages"].AsArray().RemoveAt(4);
                Throws("INVALID_JSON", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(shortSlots.ToJsonString())),
                    "v2 requires all five slots including null slots");
                var wrongHeader = requestNode.DeepClone();
                wrongHeader["program"]["stages"][2]["code"] = Convert.ToBase64String(vs);
                Throws("INVALID_JSON", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(wrongHeader.ToJsonString())),
                    "v2 rejects a GS slot containing a VS header");
                var missingFs = requestNode.DeepClone();
                missingFs["program"]["stages"][1] = null;
                Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(missingFs.ToJsonString())),
                    "v2 rejects VS+GS without FS");
                var adjacent = requestNode.DeepClone();
                adjacent["program"]["graphicsState"]["topology"] = "TrianglesAdjacency";
                Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(adjacent.ToJsonString())),
                    "GS adjacency remains unsupported");
                var oldVersion = requestNode.DeepClone();
                oldVersion["schema"] = DirectShaderCompiler.RequestSchema;
                Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(oldVersion.ToJsonString())),
                    "v1 does not silently accept geometry");
                var passthrough = requestNode.DeepClone();
                byte[] pt = GeometryGuest(); pt[3] |= 1;
                passthrough["program"]["stages"][2]["code"] = Convert.ToBase64String(pt);
                Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(passthrough.ToJsonString())),
                    "GS passthrough remains unsupported");
                var viewport = requestNode.DeepClone();
                viewport["program"]["stages"][2]["code"] = Convert.ToBase64String(GeometryGuest(viewportIndex: true));
                Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(viewport.ToJsonString())),
                    "GS viewport index remains unsupported");
            }
            try
            {
                using JsonDocument translated = ParseResult(DirectShaderCompiler.Compile(enabledRequest));
                JsonElement stages = translated.RootElement.GetProperty("content").GetProperty("stages");
                Check(stages.GetArrayLength() == 3 && stages[0].GetProperty("stage").GetString() == "vs" &&
                    stages[1].GetProperty("stage").GetString() == "gs" && stages[2].GetProperty("stage").GetString() == "fs",
                    "GS compilation uses VS->GS->FS execution order");
                if (stages.GetArrayLength() == 3)
                {
                    uint[] words = MemoryMarshal.Cast<byte, uint>(stages[1].GetProperty("spirv").GetBytesFromBase64()).ToArray();
                    var instructions = new List<uint[]>();
                    for (int i = 5; i < words.Length;)
                    {
                        int count = (int)(words[i] >> 16);
                        if (count == 0 || i + count > words.Length) throw new InvalidDataException("invalid SPIR-V instruction");
                        instructions.Add(words[i..(i + count)]); i += count;
                    }
                    Check(instructions.Any(x => (x[0] & 0xffff) == 15 && x[1] == 3), "GS SPIR-V entry execution model Geometry");
                    bool Mode(uint mode, uint? value = null) => instructions.Any(x => (x[0] & 0xffff) == 16 && x.Length >= 3 &&
                        x[2] == mode && (!value.HasValue || x.Length >= 4 && x[3] == value.Value));
                    Check(Mode(22) && Mode(29) && Mode(26, 3) && Mode(0, 1),
                        "GS topology is triangles -> triangle strip, max 3 vertices, one invocation");
                    Check(instructions.Count(x => (x[0] & 0xffff) == 218) == 3 && instructions.Count(x => (x[0] & 0xffff) == 219) == 1,
                        "GS emits three vertices and ends one primitive");
                    var locations = instructions.Where(x => (x[0] & 0xffff) == 71 && x.Length >= 4 && x[2] == 30)
                        .ToDictionary(x => x[1], x => x[3]);
                    var variables = instructions.Where(x => (x[0] & 0xffff) == 59 && x.Length >= 4).ToArray();
                    Check(variables.Any(x => x[3] == 1 && locations.TryGetValue(x[2], out uint loc) && loc == 0) &&
                        variables.Any(x => x[3] == 3 && locations.TryGetValue(x[2], out uint loc) && loc == 0),
                        "GS consumes and produces location 0");
                    var vsOut = ShaderInterface(stages[0], 3);
                    var gsIn = ShaderInterface(stages[1], 1, stripVertexArray: true);
                    var gsOut = ShaderInterface(stages[1], 3);
                    var fsIn = ShaderInterface(stages[2], 1);
                    Check(vsOut.TryGetValue(0, out string vsType) && gsIn.TryGetValue(0, out string gsInType) && vsType == gsInType,
                        "VS output matches GS input location 0 and type");
                    Check(gsOut.TryGetValue(0, out string gsOutType) && fsIn.TryGetValue(0, out string fsType) && gsOutType == fsType,
                        "GS output matches FS input location 0 and type");
                    if (args.Length == 2 && args[0] == "--emit-gs")
                    {
                        Directory.CreateDirectory(args[1]);
                        foreach (JsonElement s in stages.EnumerateArray())
                            File.WriteAllBytes(Path.Combine(args[1], s.GetProperty("stage").GetString() + ".spv"),
                                s.GetProperty("spirv").GetBytesFromBase64());
                    }
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("GS compile failed: " + ex);
                Check(false, "GS real compiler translation succeeds");
            }
            // VS 与 GS 都没有显式写 location 0，而各自的下游阶段要读它。
            // 只有正确 SetNextStage，翻译器才会在两个生产者补出接口与缺省输出。
            try
            {
                var missingOutputs = requestNode.DeepClone();
                missingOutputs["program"]["stages"][0]["code"] = Convert.ToBase64String(Guest(false, Array.Empty<ulong>()));
                missingOutputs["program"]["stages"][2]["code"] = Convert.ToBase64String(GeometryGuest(omitVaryingOutput: true));
                using JsonDocument linked = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(missingOutputs.ToJsonString())));
                JsonElement linkedStages = linked.RootElement.GetProperty("content").GetProperty("stages");
                var vsDefaults = ShaderInterface(linkedStages[0], 3);
                var gsNeeds = ShaderInterface(linkedStages[1], 1, stripVertexArray: true);
                var gsDefaults = ShaderInterface(linkedStages[1], 3);
                var fsNeeds = ShaderInterface(linkedStages[2], 1);
                Check(vsDefaults.TryGetValue(0, out string v) && gsNeeds.TryGetValue(0, out string gi) && v == gi,
                    "GS input causes unwritten VS location 0 interface to be synthesized");
                Check(gsDefaults.TryGetValue(0, out string g) && fsNeeds.TryGetValue(0, out string f) && g == f,
                    "FS input causes unwritten GS location 0 interface to be synthesized");
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("GS linked defaults failed: " + ex);
                Check(false, "GS linked default interface synthesis succeeds");
            }
            // 每个阶段都独立读取客体 c[0]。共享分配器必须在同一个 set 内分配不同 binding，
            // 反射记录与各自实际模块的 DescriptorSet/Binding 必须一致。
            try
            {
                var resources = requestNode.DeepClone();
                var slots = resources["program"]["stages"];
                slots[0]["code"] = Convert.ToBase64String(Guest(false, new[] { Ldc(0, 0), Ast(0) }));
                slots[1]["code"] = Convert.ToBase64String(FsUniform());
                slots[2]["code"] = Convert.ToBase64String(GeometryGuest(uniform: true));
                for (int i = 0; i < 3; i++) slots[i]["constantBufferUse"] = 1;
                using JsonDocument compiled = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(resources.ToJsonString())));
                JsonElement content = compiled.RootElement.GetProperty("content");
                JsonElement[] stageResults = content.GetProperty("stages").EnumerateArray().ToArray();
                JsonElement[] uniformBindings = content.GetProperty("bindings").EnumerateArray()
                    .Where(b => b.GetProperty("kind").GetString() == "uniform").ToArray();
                Check(uniformBindings.Length == 3 && uniformBindings.Select(b => b.GetProperty("binding").GetUInt32()).Distinct().Count() == 3,
                    "VS, GS and FS uniforms have distinct bindings");
                foreach (string name in new[] { "vs", "gs", "fs" })
                {
                    JsonElement binding = uniformBindings.Single(b => b.GetProperty("stage").GetString() == name);
                    JsonElement stage = stageResults.Single(s => s.GetProperty("stage").GetString() == name);
                    uint set = binding.GetProperty("set").GetUInt32(), index = binding.GetProperty("binding").GetUInt32();
                    Check(set == 0 && binding.GetProperty("slot").GetInt32() == 0 && ShaderBindings(stage).Contains((set, index)),
                        name + " uniform reflection matches its actual SPIR-V set/binding and guest slot");
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("GS resource fixture failed: " + ex);
                Check(false, "GS per-stage uniform compilation succeeds");
            }

            // 输入图元由请求事实决定；几何头仍固定输出三角带。
            foreach (var (topology, mode, vertices) in new[] { ("Points", 19u, 1), ("Lines", 20u, 2), ("Triangles", 22u, 3) })
            {
                try
                {
                    var variant = requestNode.DeepClone();
                    variant["program"]["graphicsState"]["topology"] = topology;
                    using JsonDocument compiled = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(variant.ToJsonString())));
                    JsonElement stage = compiled.RootElement.GetProperty("content").GetProperty("stages")[1];
                    uint[] spv = MemoryMarshal.Cast<byte, uint>(stage.GetProperty("spirv").GetBytesFromBase64()).ToArray();
                    bool found = false;
                    for (int i = 5; i < spv.Length; i += (int)(spv[i] >> 16))
                        if ((spv[i] & 0xffff) == 16 && (spv[i] >> 16) >= 3 && spv[i + 2] == mode) found = true;
                    Check(found && GeometryInputArrayLength(stage, 0) == vertices &&
                        stage.GetProperty("info").GetProperty("geometryVerticesPerPrimitive").GetInt32() == 3,
                        "GS " + topology + " input matches SPIR-V mode and input array size; output primitive size is 3");
                    if (args.Length == 2 && args[0] == "--emit-gs")
                        File.WriteAllBytes(Path.Combine(args[1], "gs-" + topology + ".spv"), stage.GetProperty("spirv").GetBytesFromBase64());
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("GS " + topology + " fixture failed: " + ex);
                    Check(false, "GS " + topology + " topology compilation succeeds");
                }
            }
        }
        byte[] result = DirectShaderCompiler.Compile(graphicsRequest);
        using (JsonDocument doc = ParseResult(result))
        {
            JsonElement root = doc.RootElement;
            Check(root.GetProperty("schema").GetString() == DirectShaderCompiler.ResultSchema, "result schema");
            JsonElement content = root.GetProperty("content");
            Check(content.GetProperty("contract").GetString() == DirectShaderCompiler.ContentContract, "content contract");
            Check(!content.TryGetProperty("hasUnalignedStorageBuffer", out _), "graphics omits compute alignment reflection");
            Check(!content.TryGetProperty("c0Reads", out _), "graphics omits compute c0 reflection");
            Check(!content.TryGetProperty("supportReads", out _) && !content.TryGetProperty("supportScaleRanges", out _), "graphics omits compute support reflection");
            JsonElement stages = content.GetProperty("stages");
            Check(stages.GetArrayLength() == 2, "two stages");
            Check(stages[0].GetProperty("stage").GetString() == "vs" && stages[1].GetProperty("stage").GetString() == "fs", "stage order");
            foreach (JsonElement stage in stages.EnumerateArray())
            {
                byte[] spv = stage.GetProperty("spirv").GetBytesFromBase64();
                Check(spv.Length > 20 && BitConverter.ToUInt32(spv, 0) == 0x07230203, "SPIR-V magic " + stage.GetProperty("stage").GetString());
                Check(BitConverter.ToUInt32(spv, 4) == 0x00010300, "SPIR-V 1.3 " + stage.GetProperty("stage").GetString());
                Check(stage.GetProperty("spirvSha256").GetString() == Convert.ToHexStringLower(SHA256.HashData(spv)), "SPIR-V digest");
            }
            JsonElement support = content.GetProperty("supportLayout");
            Check(support.GetProperty("binding").GetInt32() == 0 && support.GetProperty("requiredSize").GetInt32() > 0, "support buffer layout");
            Check(support.GetProperty("hash").GetString().StartsWith("sha256-"), "support layout hash");
            JsonElement bindings = content.GetProperty("bindings");
            Check(bindings[0].GetProperty("kind").GetString() == "support" && bindings[0].GetProperty("set").GetInt32() == 0 &&
                  bindings[0].GetProperty("binding").GetInt32() == 0, "support binding first");
            Check(content.GetProperty("dependencies").GetArrayLength() > 10, "query dependencies recorded");
            Check(root.GetProperty("contentHash").GetString().StartsWith("sha256-"), "content hash");
        }

        // 3) 同快照逐字节一致；transport 不进入内容键。
        byte[] again = DirectShaderCompiler.Compile(graphicsRequest);
        using (JsonDocument a = ParseResult(result)) using (JsonDocument b = ParseResult(again))
            Check(Content(a).SequenceEqual(Content(b)), "same snapshot content bytes identical");
        byte[] otherTransport = RequestJson(profile, "graphics", 99, 7, "req-other", vs, fs);
        using (JsonDocument a = ParseResult(result)) using (JsonDocument b = ParseResult(DirectShaderCompiler.Compile(otherTransport)))
        {
            Check(Content(a).SequenceEqual(Content(b)), "transport excluded from content");
            Check(a.RootElement.GetProperty("contentHash").GetString() == b.RootElement.GetProperty("contentHash").GetString(),
                "content hash transport independent");
        }

        // 4) 附件类型变体：同一代码、不同片元输出类型 -> 新内容键。
        byte[] variantRequest = RequestJson(profile, "graphics", 1, 1, "req-graphics-1", vs, fs, fragmentType: "Sint");
        using (JsonDocument a = ParseResult(result)) using (JsonDocument b = ParseResult(DirectShaderCompiler.Compile(variantRequest)))
            Check(a.RootElement.GetProperty("contentHash").GetString() != b.RootElement.GetProperty("contentHash").GetString(),
                "attachment variant changes content hash");

        // 5) 合成 CS。
        // 只有顶点阶段的图形程序（只写深度的绘制）：结果里一个阶段，就是 vs，带 SPIR-V
        using (JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(
                   RequestJson(profile, "graphics", 1, 1, "req-vs-only", Guest(false, new[] { Ald4(0), Ast4Pos(0) })))))
        {
            JsonElement content = doc.RootElement.GetProperty("content");
            Check(content.GetProperty("kind").GetString() == "graphics", "vs-only kind is graphics");
            JsonElement stages = content.GetProperty("stages");
            Check(stages.GetArrayLength() == 1 && stages[0].GetProperty("stage").GetString() == "vs", "vs-only: exactly one stage, vs");
        }
        byte[] computeRequest = RequestJson(profile, "compute", 2, 1, "req-compute", cs: cs);
        using (JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(computeRequest)))
        {
            JsonElement content = doc.RootElement.GetProperty("content");
            Check(content.GetProperty("kind").GetString() == "compute", "compute kind");
            Check(content.GetProperty("c0Reads").GetArrayLength() == 0, "compute no c0 actual reads");
            Check(content.GetProperty("supportReads").GetArrayLength() == 0, "compute declared support but no actual loads");
            Check(content.GetProperty("stages").GetArrayLength() == 1 &&
                  content.GetProperty("stages")[0].GetProperty("stage").GetString() == "cs", "compute stage");
        }

        // 真实 decoder -> translator -> Info.SBuffers -> JSON 反射，补足下方 mapper 单测。
        // 预期来自维护合同 #1111/6531250423：c0 SSBO 表 0x310、UBO 指针表 0x210，步长 16 B。
        // 特意让唯一资源的 translator slot 都为 0，而 0x320 的 NVN slot 必须为 1。
        foreach (var v in new[] { (Offset: 0x310, Nvn: 0, Ptr: -1),
                                 (Offset: 0x320, Nvn: 1, Ptr: -1),
                                 (Offset: 0x220, Nvn: -1, Ptr: 1) })
        {
            using JsonDocument storageDoc = ParseResult(DirectShaderCompiler.Compile(
                RequestJson(profile, "compute", 2, 1, "real-storage-" + v.Offset,
                    cs: ComputeStorageGuest(v.Offset), csCbUse: 1)));
            JsonElement content = storageDoc.RootElement.GetProperty("content");
            JsonElement buffers = content.GetProperty("stages")[0].GetProperty("info").GetProperty("sbuffers");
            Check(buffers.GetArrayLength() == 1, "real CS one translated storage buffer " + v.Offset);
            JsonElement descriptor = buffers[0];
            Check(descriptor.GetProperty("slot").GetInt32() == 0 &&
                  descriptor.GetProperty("sbCbSlot").GetInt32() == 0 &&
                  descriptor.GetProperty("sbCbOffset").GetInt32() == v.Offset / 4,
                "real CS translator ordinal and CB word offset " + v.Offset);
            JsonElement binding = content.GetProperty("bindings").EnumerateArray()
                .Single(b => b.GetProperty("kind").GetString() == "storage");
            Check(binding.GetProperty("stage").GetString() == "cs" &&
                  binding.GetProperty("set").GetInt32() == descriptor.GetProperty("set").GetInt32() &&
                  binding.GetProperty("binding").GetInt32() == descriptor.GetProperty("binding").GetInt32(),
                "real CS descriptor joined by stage/set/binding " + v.Offset);
            Check(!binding.TryGetProperty("slot", out _) &&
                  binding.GetProperty("cbSlot").GetInt32() == 0 &&
                  binding.GetProperty("cbOffset").GetInt32() == v.Offset,
                "real CS runtime CB byte facts without ordinal slot " + v.Offset);
            Check(binding.GetProperty("rw").GetBoolean(), "real CS write flag " + v.Offset);
            int OptionalInt(string name) => binding.TryGetProperty(name, out JsonElement field) ? field.GetInt32() : -1;
            Check(OptionalInt("nvnSlot") == v.Nvn && OptionalInt("uboPtr") == v.Ptr &&
                  OptionalInt("uboSlot") == -1 && OptionalInt("uboOffset") == -1,
                "real CS runtime mapping " + v.Offset);
            // 原封保留系统 c0 声明；这些编译测试不冒充 runtime/parser/真机验证。
            Check(content.GetProperty("bindings").EnumerateArray().Any(b =>
                b.GetProperty("kind").GetString() == "uniform" && b.GetProperty("slot").GetInt32() == 0),
                "real CS preserves system c0 binding " + v.Offset);
        }

        // Precise byte ranges preserve ABI boundaries; runtime decides support/refusal.
        foreach (int offset in new[] { 0x200, 0x204, 0x208, 0x20c, 0x210, 0x310, 0x610, 0x644 })
        {
            int[][] reads = DirectComputeC0Reads.Analyze(C0ReadFixture(offset), 0, 1);
            Check(reads != null && reads.Length == 1 && reads[0].SequenceEqual(new[] { offset, 4 }),
                "exact c0 scalar range " + offset);
        }
        Check(DirectComputeC0Reads.Analyze(C0ReadFixture(0x200, vectorLoad: true), 0, 1)?[0]
            .SequenceEqual(new[] { 0x200, 16 }) == true, "cross-boundary vec4 not narrowed to 12");
        Check(DirectComputeC0Reads.Analyze(C0ReadFixture(0x200, dynamic: true), 0, 1) == null,
            "dynamic c0 read unknown not empty");
        Check(DirectComputeC0Reads.Analyze(C0ReadFixture(0x200, load: false), 0, 1)?.Length == 0,
            "unused access chain is not a load");
        Check(DirectComputeC0Reads.Analyze(C0ReadFixture(0x200, escape: true), 0, 1) == null,
            "escaped c0 pointer unknown not empty");
        Check(DirectComputeC0Reads.Analyze(C0ReadFixture(0x200), 1, 1) == null,
            "descriptor set must match");
        {
            // 实机 cs-1-1-7069 等价物：c0 (set0,binding1) 可证，cp_s1 仿品
            // (set1,binding1) 动态索引。粗闸门只比 binding 号会借后者把 c0Reads 判 null。
            int[][] collision = DirectComputeC0Reads.Analyze(C0ReadSetCollisionFixture(), 0, 1);
            Check(collision != null && collision.Select(r => (r[0], r[1])).SequenceEqual(
                new[] { (656, 4), (736, 4), (740, 4), (784, 4), (848, 4), (852, 4) }),
                "same-binding-number dynamic UBO in another set must not poison c0 reads");
            Check(DirectComputeC0Reads.Analyze(C0ReadSetCollisionFixture(), 1, 1) == null,
                "dynamic index on that same (set,binding) is still unknown");
        }
        using (JsonDocument alignedDoc = ParseResult(DirectShaderCompiler.Compile(
            RequestJson(profile, "compute", 2, 1, "aligned-c0", cs: ComputeStorageGuest(0x310), csCbUse: 1))))
            Check(alignedDoc.RootElement.GetProperty("content").GetProperty("c0Reads").GetArrayLength() == 0,
                "real aligned CS declared c0 but no loads");
        foreach (bool prefix in new[] { false, true })
        {
            string unalignedC0 = Encoding.UTF8.GetString(RequestJson(profile, "compute", 2, 1, "unaligned-c0",
                cs: ComputeStorageGuest(0x310, prefix: prefix), csCbUse: 1)).Replace("\"hasUnalignedStorageBuffer\":false", "\"hasUnalignedStorageBuffer\":true");
            using (JsonDocument unalignedDoc = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(unalignedC0))))
            {
                JsonElement reads = unalignedDoc.RootElement.GetProperty("content").GetProperty("c0Reads");
                Check(reads.ValueKind == JsonValueKind.Array && reads.GetArrayLength() == 1 &&
                    reads[0][0].GetInt32() == 0x310 && reads[0][1].GetInt32() == 4,
                    "real unaligned CS reads only c0 address low32 prefix=" + prefix);
            }
        }

        foreach (var r in new[] { (0, 0, 0), (5, 0, 192),
                                   (5, 1, 208), (5, 72, 1344), (11, 0, 1440),
                                   (11, 72, 2592), (8, 1, 1396), (2, 3, 156) })
        {
            int[][] reads = DirectComputeSupportReads.Analyze(SupportReadFixture(r.Item1, r.Item2), 0, 0, 2608);
            Check(reads != null && reads.Length == 1 && reads[0].SequenceEqual(new[] { r.Item3, 4 }),
                "heterogeneous support actual byte read " + r.Item3);
        }
        DirectComputeSupportReads.Analyze(SupportReadFixture(5), 0, 0, 2608, out var scaleRanges);
        Check(scaleRanges != null && scaleRanges.SequenceEqual(new[] {
            ("render_scale", 192, 16, 73, 4), ("render_scale_y", 1440, 16, 73, 4) }),
            "support scale semantics proven against actual FP32 array layout");
        int[][] absentReads = DirectComputeSupportReads.Analyze(SupportReadFixture(5), 0, 99, 2608, out var absentRanges);
        Check(absentReads?.Length == 0 && absentRanges == null, "absent support root proves no reads but not a layout");
        DirectComputeSupportReads.Analyze(SupportReadFixture(5, stride: 4), 0, 0, 2608, out var packedRanges);
        Check(packedRanges == null, "support packed scalar arrays cannot claim std140 scale mapping");
        DirectComputeSupportReads.Analyze(SupportReadFixture(5, integerScale: true), 0, 0, 2608, out var integerRanges);
        Check(integerRanges == null, "support integer array cannot claim floating scale semantics");
        DirectComputeSupportReads.Analyze(SupportReadFixture(5, scaleCount: 72), 0, 0, 2608, out var shortRanges);
        Check(shortRanges == null, "support shortened array cannot claim complete scale mapping");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(8, vectorLoad: true), 0, 0, 2608)?[0]
            .SequenceEqual(new[] { 1392, 16 }) == true, "support whole vector not narrowed");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, load: false), 0, 0, 2608)?.Length == 0,
            "unused support access is not load");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, dynamic: true), 0, 0, 2608) == null,
            "support dynamic array unknown");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, escape: true), 0, 0, 2608) == null,
            "support pointer escape unknown");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, forwardEscape: true), 0, 0, 2608) == null,
            "support forward pointer escape unknown");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, 73), 0, 0, 2608) == null,
            "support array bounds checked");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, 2, stride: uint.MaxValue), 0, 0, 2608) == null,
            "support stride overflow rejected");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, -2, stride: uint.MaxValue, scaleCount: uint.MaxValue), 0, 0, 2608) == null,
            "support unsigned index times stride cannot overflow signed long");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(5, stride: 0), 0, 0, 2608) == null,
            "support missing stride proof rejected");
        Check(DirectComputeSupportReads.Analyze(SupportReadFixture(11, 72), 0, 0, 2595) == null,
            "support required size checked");
        Check(DirectComputeSupportReads.Analyze(new byte[21], 0, 0, 2608) == null,
            "support malformed binary unknown not empty");

        // 6) 错误输入：坏 JSON/重复键/未知字段/版本/阶段/TF/支持布局。
        Throws("INVALID_JSON", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes("{not json")), "bad json");
        Throws("DUPLICATE_KEY", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes("{\"a\":1,\"a\":2}")), "duplicate key");
        Throws(null, () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes("{\"schema\":\"x\"}")), "missing fields");
        using (JsonDocument doc = ParseResult(graphicsRequest))
        {
            string json = doc.RootElement.GetRawText().Replace("\"sourceParserVersion\":1", "\"sourceParserVersion\":1,\"unknown\":0");
            Throws("INVALID_JSON", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(json)), "unknown field");
        }
        Throws(null, () => DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "x", vs, fs, transformFeedback: true)), "transform feedback");
        Throws(null, () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(
            Encoding.UTF8.GetString(graphicsRequest).Replace("\"vs\"", "\"gs\""))), "unsupported stage");
        Throws(null, () => DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "x", new byte[0x20], fs)), "short header");
        Throws(null, () => DirectShaderCompiler.Compile(RequestJson(profile, "compute", 1, 1, "x", cs: cs, localSize: 0)), "bad local size");
        Throws("CONTROL_MISMATCH", () => DirectShaderCompiler.Compile(
            RequestJson(profile, "compute", 1, 1, "x", cs: cs, localSize: 1, controlX: 2)), "control/computeState mismatch");
        Throws("CONTROL_INVALID", () => DirectShaderCompiler.Compile(
            RequestJson(profile, "compute", 1, 1, "x", cs: cs, localSize: 1, controlX: 0)), "zero control local size");
        byte[] badProfile = Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(profile).Replace(SupportLayout.Compile().Hash, "sha256-" + new string('b', 64)));
        Throws("IDENTITY", () => DirectShaderCompiler.Compile(RequestJson(badProfile, "graphics", 1, 1, "x", vs, fs)), "support layout mismatch");

        // 7) Accessor 事实来源：GetCode/CB1 越界、纹理变体、四 set 分配器。
        using (JsonDocument profileDoc = JsonDocument.Parse(profile))
        {
            DirectProfile parsedProfile = DirectProfile.Parse(profileDoc.RootElement, "profile");
            string textures = "[{\"stage\":\"fs\",\"handle\":3,\"cbufSlot\":2,\"samplerType\":\"2d-array\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":4}," +
                "{\"stage\":\"fs\",\"handle\":10,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"Unknown\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";   // 10-31 暗黑3 安卓：DXT1 / DXT5 采样贴图
            byte[] texturedRequest = RequestJson(profile, "graphics", 1, 1, "tex", vs, fs, textureFacts: textures);
            using JsonDocument doc = JsonDocument.Parse(texturedRequest);
            DirectProgram program = DirectProgram.Parse(doc.RootElement.GetProperty("program"), "program", parsedProfile);
            var queries = new List<DirectQuery>();
            var allocator = new DirectAllocator(parsedProfile);
            var accessor = new DirectGpuAccessor(parsedProfile, program, program.StageOf("fs"), 5, allocator, queries);
            Check(!accessor.QueryHostStorageWindowClamp() && !accessor.QueryHostStorageOobRecord(), "direct-v1 keeps uninstrumented storage policy");
            foreach (ShaderStage stage in Enum.GetValues<ShaderStage>())
                Check(accessor.CreateS2AStorageOobRecordBinding(stage) == new SetBindingPair(-1, -1), "no undeclared OOB binding for " + stage);
            Check(accessor.QuerySamplerType(3, 2) == (SamplerType.Texture2D | SamplerType.Array), "sampler type fact");
            Check(accessor.QueryTextureFormat(3, 2) == TextureFormat.R8G8B8A8Unorm, "texture format fact");
            Check(accessor.QueryTextureCoordNormalized(3, 2) && !accessor.QueryTextureIsBuffer(3, 2), "normalized/non-buffer fact");
            Throws("MISSING_FACT", () => accessor.QuerySamplerType(4, 2), "unbound texture is a missing fact");
            Check(accessor.QueryTextureCoordNormalized(10, -1) && accessor.QuerySamplerType(10, -1) == SamplerType.Texture2D, "sampled-only texture (format Unknown) answers sampler type and normalization");
            Throws("MISSING_FACT", () => accessor.QueryTextureFormat(10, -1), "format Unknown has no image format");
            Throws("MISSING_FACT", () => accessor.GetCode(1u << 20, 8), "GetCode out of range");
            Throws("MISSING_FACT", () => accessor.ConstantBuffer1Read(4), "empty C1 out of range");
            Check(allocator.Uniform("vs", 0) == new SetBindingPair(0, 1), "UBO set0 binding1 after support");
            Check(allocator.Storage("vs", 0) == new SetBindingPair(1, 0), "SSBO set1 from 0");
            Check(allocator.Bindings.Last().Slot == -1, "graphics storage slot remains absent");
            allocator.Storage("cs", 7);
            Check(allocator.Bindings.Last().Slot == -1, "compute allocator index is not a runtime slot");
            Check(allocator.Texture("fs", 4, false) == new SetBindingPair(2, 0), "texture set2 from 0");
            Check(allocator.Image("fs", 1, false) == new SetBindingPair(3, 0), "image set3 from 0");
            Check(accessor.CreateExtraSet() == -1, "no extra set");
        }

        // Exercise the production descriptor mapping, not an independently rewritten mapper.
        var mapStorage = typeof(DirectShaderCompiler).GetMethod("ApplyComputeStorageDescriptor",
            System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic);
        Check(mapStorage != null, "production compute storage mapper exists");
        foreach (var v in new[] {
            (Cb:0, Off:0x310, Nvn:0, Ptr:-1, Ubo:-1, Uoff:-1),
            (Cb:0, Off:0x320, Nvn:1, Ptr:-1, Ubo:-1, Uoff:-1),
            (Cb:0, Off:0x220, Nvn:-1, Ptr:1, Ubo:-1, Uoff:-1),
            (Cb:0, Off:0x314, Nvn:-1, Ptr:-1, Ubo:-1, Uoff:-1),
            (Cb:0, Off:0x410, Nvn:-1, Ptr:-1, Ubo:-1, Uoff:-1),
            (Cb:1, Off:0x310, Nvn:-1, Ptr:-1, Ubo:-1, Uoff:-1),
            (Cb:3, Off:0x38, Nvn:-1, Ptr:-1, Ubo:0, Uoff:0x38),
            (Cb:5, Off:0x40, Nvn:-1, Ptr:-1, Ubo:2, Uoff:0x40) })
        {
            var binding = new DirectBinding("cs", "storage", "guestSSBO[7]", 1, 2, 1, 7);
            mapStorage.Invoke(null, new object[] { binding, new BufferDescriptor(1, 2, 7, v.Cb, v.Off / 4, BufferUsageFlags.None) });
            Check(binding.CbSlot == v.Cb && binding.CbOffset == v.Off && binding.Slot == -1,
                "compute source byte facts " + v);
            Check(binding.NvnSlot == v.Nvn && binding.UboPtr == v.Ptr && binding.UboSlot == v.Ubo && binding.UboOffset == v.Uoff,
                "compute storage mapping " + v);
        }

        // #1403: graphics storage bindings use the same provenance mapper. The
        // runtime first-draw path consumes nvnSlot / uboSlot+uboOffset / rw.
        // c0 table bases are per NVN stage: vs 0x110, fs 0x510, gs 0x410,
        // tcs 0x210, tes 0x310 (compute 0x310 separately above).
        foreach (var v in new[] {
            (Stage:"vs", Cb:0, Off:0x110, Nvn:0, Ubo:-1, Uoff:-1, Rw:false),
            (Stage:"vs", Cb:0, Off:0x120, Nvn:1, Ubo:-1, Uoff:-1, Rw:true),
            (Stage:"fs", Cb:0, Off:0x510, Nvn:0, Ubo:-1, Uoff:-1, Rw:false),
            (Stage:"gs", Cb:0, Off:0x410, Nvn:0, Ubo:-1, Uoff:-1, Rw:false),
            (Stage:"tcs", Cb:0, Off:0x210, Nvn:0, Ubo:-1, Uoff:-1, Rw:false),
            (Stage:"tes", Cb:0, Off:0x310, Nvn:0, Ubo:-1, Uoff:-1, Rw:false),
            (Stage:"vs", Cb:0, Off:0x310, Nvn:-1, Ubo:-1, Uoff:-1, Rw:false),
            (Stage:"vs", Cb:3, Off:0x38, Nvn:-1, Ubo:0, Uoff:0x38, Rw:true) })
        {
            var binding = new DirectBinding(v.Stage, "storage", "guestSSBO[0]", 1, 0, 1);
            BufferUsageFlags flags = v.Rw ? BufferUsageFlags.Write : BufferUsageFlags.None;
            mapStorage.Invoke(null, new object[] { binding, new BufferDescriptor(1, 0, 0, v.Cb, v.Off / 4, flags) });
            Check(binding.CbSlot == v.Cb && binding.CbOffset == v.Off &&
                  binding.NvnSlot == v.Nvn && binding.UboSlot == v.Ubo && binding.UboOffset == v.Uoff &&
                  binding.Writable == v.Rw,
                "graphics storage provenance " + v);
        }

        // Join by actual (set,binding), never translator allocation order.
        using (JsonDocument mapProfileDoc = JsonDocument.Parse(profile))
        using (JsonDocument mapRequestDoc = JsonDocument.Parse(RequestJson(profile, "compute", 1, 1, "storage-map", cs: cs)))
        {
            DirectProfile mapProfile = DirectProfile.Parse(mapProfileDoc.RootElement, "profile");
            DirectProgram mapProgram = DirectProgram.Parse(mapRequestDoc.RootElement.GetProperty("program"), "program", mapProfile);
            var mapAllocator = new DirectAllocator(mapProfile);
            mapAllocator.Storage("cs", 9); mapAllocator.Storage("cs", 2);
            ShaderProgram MakeStorageInfo(BufferDescriptor[] descriptors) => new(new ShaderProgramInfo(
                Array.Empty<BufferDescriptor>(), descriptors, Array.Empty<TextureDescriptor>(), Array.Empty<TextureDescriptor>(),
                ShaderStage.Compute, 0, 0, 0, false, false, false, false, 0, 0),
                Ryujinx.Graphics.Shader.Translation.TargetLanguage.Spirv, Array.Empty<byte>());
            var descriptors = new[] {
                new BufferDescriptor(1, 1, 2, 0, 0x310 / 4, BufferUsageFlags.None),
                new BufferDescriptor(1, 0, 9, 0, 0x320 / 4, BufferUsageFlags.None) };
            var translatedMap = new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)> {
                (mapProgram.StageOf("cs"), MakeStorageInfo(descriptors), null) };
            var resolve = typeof(DirectShaderCompiler).GetMethod("ResolveComputeStorageSlots",
                System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic);
            resolve.Invoke(null, new object[] { translatedMap, mapAllocator });
            Check(mapAllocator.Bindings.First(b => b.Kind == "storage" && b.Binding == 0).NvnSlot == 1 &&
                mapAllocator.Bindings.First(b => b.Kind == "storage" && b.Binding == 1).NvnSlot == 0,
                "SBuffers joined by binding, not descriptor or allocator order");
            foreach (BufferDescriptor[] badDescriptors in new[] { Array.Empty<BufferDescriptor>(),
                new[] { descriptors[0], descriptors[0], descriptors[1] } })
            {
                translatedMap[0] = (mapProgram.StageOf("cs"), MakeStorageInfo(badDescriptors), null);
                bool rejected = false;
                try { resolve.Invoke(null, new object[] { translatedMap, mapAllocator }); }
                catch (System.Reflection.TargetInvocationException error) {
                    rejected = error.InnerException is DirectCompileException direct && direct.Code == "SPIRV_INVALID";
                }
                Check(rejected, "missing/duplicate storage reflection rejected");
            }
        }

        // 常量缓冲掩码必须是完整 32 位：旧实现截在 0x1F，>=5 号 cbuf 的信息丢失，
        // 生成的着色器与运行时 layout 对不上（真机 fp_c1 / vkCreateGraphicsPipelines failed）。
        {
            using JsonDocument cbProfileDoc = JsonDocument.Parse(profile);
            DirectProfile cbProfile = DirectProfile.Parse(cbProfileDoc.RootElement, "profile");
            string cbRaw = Encoding.UTF8.GetString(RequestJson(profile, "compute", 1, 1, "cbmask", cs: cs, csCbUse: 33));
            using JsonDocument cbParsed = JsonDocument.Parse(cbRaw);
            DirectProgram cbProgram = DirectProgram.Parse(cbParsed.RootElement.GetProperty("program"), "program", cbProfile);
            Check(cbProgram.StageOf("cs").ConstantBufferUse == 33u,
                "constantBufferUse 完整 32 位（>=5 号 cbuf 不丢信息），实得 " + cbProgram.StageOf("cs").ConstantBufferUse);
        }

        // 描述符种类负对照：SPIR-V 存储类与 allocator kind 不一致必须抛 SPIRV_INVALID，
        // 一致或“未知”（无法判定）必须放行。真机 fp_c1 那类错误应当在这里被拦住。
        // enabled=false 只来自请求 JSON 的 kindCheck=false（native 调试门 + debug.s2a.kind_check=0）。
        {
            bool KindRejected(int storageClass, string kind, bool enabled = true) {
                try {
                    SpirvReflection.CheckDescriptorKind("fragment",
                        new SharedDescriptor(0, 1, storageClass, "fp_c1"), kind, enabled);
                    return false;
                } catch (DirectCompileException e) { return e.Code == "SPIRV_INVALID"; }
            }
            Check(KindRejected(0, "uniform"), "UBO 位置声明成纹理 → 拒绝");
            Check(KindRejected(2, "texture"), "纹理位置声明成 UBO → 拒绝");
            Check(!KindRejected(2, "uniform"), "UBO 位置声明成 UBO → 放行");
            Check(!KindRejected(0, "texture"), "纹理位置声明成纹理 → 放行");
            Check(!KindRejected(-1, "uniform"), "未知存储类 → 不误报");
            Check(!KindRejected(0, "storage"), "无法判定的 allocator kind → 不误报");
            // 调试开关：门开 + debug.s2a.kind_check=0 时跳过校验；默认（缺省/门关）仍拦。
            Check(!KindRejected(0, "uniform", false), "kindCheck=false → 种类不符不抛（单变量对照）");
            Check(!KindRejected(2, "texture", false), "kindCheck=false → 反向种类不符不抛");
            Check(KindRejected(0, "uniform", true), "kindCheck=true → 种类不符仍拦（负对照）");
        }

        // 请求 JSON 的 kindCheck：合法布尔才收（缺省=true），非布尔必须 INVALID_JSON；不读进程环境变量。
        {
            string raw = Encoding.UTF8.GetString(RequestJson(profile, "compute", 1, 1, "kindcheck", cs: cs));
            string withFlag = raw.Replace(",\"profile\"", ",\"kindCheck\":false,\"profile\"");
            Check(withFlag != raw, "kindCheck 插入锚点存在");
            using JsonDocument flagged = JsonDocument.Parse(withFlag);
            Check(!JsonStrict.OptionalBool(flagged.RootElement, "kindCheck", "request", true),
                "kindCheck=false 被解析为跳过");
            using JsonDocument absent = JsonDocument.Parse(raw);
            Check(JsonStrict.OptionalBool(absent.RootElement, "kindCheck", "request", true),
                "缺省 kindCheck → 校验开着");
            string badFlag = raw.Replace(",\"profile\"", ",\"kindCheck\":\"no\",\"profile\"");
            bool badRejected = false;
            try { DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(badFlag)); }
            catch (DirectCompileException e) { badRejected = e.Code == "INVALID_JSON"; }
            Check(badRejected, "kindCheck 非布尔 → INVALID_JSON");
            // 端到端：带 kindCheck=false 的合法请求仍能编译（新键不被 OnlyKeys 拒绝）。
            using JsonDocument compiled = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(withFlag)));
            Check(compiled.RootElement.GetProperty("schema").GetString() == DirectShaderCompiler.ResultSchema,
                "带 kindCheck 的请求可编译");
        }

        // Compute alignment is an explicit required fact, not a graphics fallback.
        foreach (bool unaligned in new[] { false, true })
        {
            using JsonDocument computeProfileDoc = JsonDocument.Parse(profile);
            DirectProfile parsedProfile = DirectProfile.Parse(computeProfileDoc.RootElement, "profile");
            string raw = Encoding.UTF8.GetString(RequestJson(profile, "compute", 1, 1, "align", cs: cs));
            raw = raw.Replace("\"hasUnalignedStorageBuffer\":false", "\"hasUnalignedStorageBuffer\":" + (unaligned ? "true" : "false"));
            using JsonDocument doc = JsonDocument.Parse(raw);
            DirectProgram program = DirectProgram.Parse(doc.RootElement.GetProperty("program"), "program", parsedProfile);
            var accessor = new DirectGpuAccessor(parsedProfile, program, program.StageOf("cs"), 32,
                new DirectAllocator(parsedProfile), new List<DirectQuery>());
            Check(accessor.QueryHasUnalignedStorageBuffer() == unaligned, "compute alignment exact " + unaligned);
            using JsonDocument compiledAlignment = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(raw)));
            Check(compiledAlignment.RootElement.GetProperty("content").GetProperty("hasUnalignedStorageBuffer").GetBoolean() == unaligned,
                "compute alignment reflection exact " + unaligned);
            string missing = raw.Replace(",\"hasUnalignedStorageBuffer\":" + (unaligned ? "true" : "false"), "");
            using JsonDocument missingDoc = JsonDocument.Parse(missing);
            bool missingRejected = false;
            try { DirectProgram.Parse(missingDoc.RootElement.GetProperty("program"), "program", parsedProfile); }
            catch (DirectCompileException error) { missingRejected = error.Code == "MISSING_FIELD"; }
            Check(missingRejected, "compute alignment missing rejected");
        }

        // 7b) 多纹理：两张 texture fact 必须各拿一个 set2 binding，slot 分别是 0/1，
        //     这样 .xgs 才能把两张池纹理同时绑定到正确槽位。
        {
            using JsonDocument doc = JsonDocument.Parse(DirectShaderCompiler.Compile(
                RequestJson(profile, "graphics", 1, 1, "two-textures", vs, FsTwoTextures(), textureFacts: TwoTextureFacts)));
            JsonElement content = doc.RootElement.GetProperty("content");
            var texBindings = content.GetProperty("bindings").EnumerateArray()
                .Where(b => b.GetProperty("kind").GetString() == "texture").ToList();
            Check(texBindings.Count == 2, "two texture bindings");
            Check(texBindings[0].GetProperty("set").GetInt32() == 2 && texBindings[1].GetProperty("set").GetInt32() == 2,
                "texture bindings live in set2");
            Check(texBindings[0].GetProperty("slot").GetInt32() == 2 && texBindings[1].GetProperty("slot").GetInt32() == 3,
                "texture slots follow the two facts");
            Check(texBindings[0].GetProperty("binding").GetInt32() != texBindings[1].GetProperty("binding").GetInt32(),
                "texture bindings are distinct");
            JsonElement fsInfo = content.GetProperty("stages")[1].GetProperty("info");
            Check(fsInfo.GetProperty("textures").GetArrayLength() == 2, "fs reflects two sampled textures");
            Check(fsInfo.GetProperty("textures")[0].GetProperty("handleIndex").GetInt32() == 2 &&
                  fsInfo.GetProperty("textures")[1].GetProperty("handleIndex").GetInt32() == 3,
                "reflected texture handles 2/3");
        }

        // 7b2) 纹理事实矩阵（W2.2）：每个字段在 accessor 查询处逐项对上；同一 handle 不同
        //      cbufSlot、不同阶段不能互相复用；未知枚举/未知分量在解析期就拒绝。
        {
            DirectProfile matrixProfile;
            using (JsonDocument profileDoc = JsonDocument.Parse(profile))
                matrixProfile = DirectProfile.Parse(profileDoc.RootElement, "profile");
            const string facts = "["
                + "{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1},"
                + "{\"stage\":\"fs\",\"handle\":1,\"cbufSlot\":0,\"samplerType\":\"2d-array\",\"format\":\"R16G16B16A16Float\",\"isBuffer\":false,\"normalized\":false,\"separate\":false,\"arrayLength\":8},"
                + "{\"stage\":\"fs\",\"handle\":1,\"cbufSlot\":4,\"samplerType\":\"cube-shadow\",\"format\":\"R32Float\",\"isBuffer\":false,\"normalized\":false,\"separate\":false,\"arrayLength\":6},"
                + "{\"stage\":\"fs\",\"handle\":2,\"cbufSlot\":-1,\"samplerType\":\"buffer\",\"format\":\"R32Uint\",\"isBuffer\":true,\"normalized\":false,\"separate\":false,\"arrayLength\":1},"
                + "{\"stage\":\"fs\",\"handle\":3,\"cbufSlot\":-1,\"samplerType\":\"3d\",\"format\":\"R11G11B10Float\",\"isBuffer\":false,\"normalized\":false,\"separate\":true,\"arrayLength\":1}"
                + "]";
            byte[] matrixRequest = RequestJson(profile, "graphics", 1, 1, "tex-matrix", vs, fs, textureFacts: facts,
                textureBufferLengths: "{\"0\":8}");
            using JsonDocument doc = JsonDocument.Parse(matrixRequest);
            DirectProgram program = DirectProgram.Parse(doc.RootElement.GetProperty("program"), "program", matrixProfile);
            var matrixAccessor = new DirectGpuAccessor(matrixProfile, program, program.StageOf("fs"), 5,
                new DirectAllocator(matrixProfile), new List<DirectQuery>());

            Check(matrixAccessor.QuerySamplerType(0, -1) == SamplerType.Texture2D, "2d sampler type");
            Check(matrixAccessor.QueryTextureFormat(0, -1) == TextureFormat.R8G8B8A8Unorm, "2d format");
            Check(matrixAccessor.QueryTextureCoordNormalized(0, -1) && !matrixAccessor.QueryTextureIsBuffer(0, -1),
                "2d normalized non-buffer");

            Check(matrixAccessor.QuerySamplerType(1, 0) == (SamplerType.Texture2D | SamplerType.Array), "2d-array sampler type");
            Check(matrixAccessor.QueryTextureFormat(1, 0) == TextureFormat.R16G16B16A16Float, "2d-array format");
            Check(!matrixAccessor.QueryTextureCoordNormalized(1, 0), "float array is not normalized");

            Check(matrixAccessor.QuerySamplerType(1, 4) == (SamplerType.TextureCube | SamplerType.Shadow),
                "same handle keeps its own cbufSlot facts");
            Check(matrixAccessor.QueryTextureFormat(1, 4) == TextureFormat.R32Float, "per-slot format");

            Check(matrixAccessor.QuerySamplerType(2, -1) == SamplerType.TextureBuffer && matrixAccessor.QueryTextureIsBuffer(2, -1),
                "buffer texture");
            Check(matrixAccessor.QueryTextureFormat(2, -1) == TextureFormat.R32Uint, "integer buffer format");
            Check(matrixAccessor.QuerySamplerType(3, -1) == SamplerType.Texture3D, "3d sampler type");

            // cbuf 查询没有精确事实时，只能回落到同句柄的直接事实（NVN c2 句柄→组合槽）；
            // 没有同句柄直接事实、或未列出的 handle 仍是缺事实；事实不跨阶段。
            Check(matrixAccessor.QuerySamplerType(0, 0) == SamplerType.Texture2D &&
                  matrixAccessor.QueryTextureCoordNormalized(0, 0),
                "cbuf query falls back to the same handle's direct fact");
            Check(matrixAccessor.HasDirectTextureFact(0) && !matrixAccessor.HasDirectTextureFact(1),
                "only directly bound handles have a fallback");
            Throws("MISSING_FACT", () => matrixAccessor.QuerySamplerType(9, 0), "cbuf handle without a direct fact is a missing fact");
            Throws("MISSING_FACT", () => matrixAccessor.QueryTextureFormat(9, -1), "unlisted handle is a missing fact");
            var vsMatrix = new DirectGpuAccessor(matrixProfile, program, program.StageOf("vs"), 1,
                new DirectAllocator(matrixProfile), new List<DirectQuery>());
            Throws("MISSING_FACT", () => vsMatrix.QuerySamplerType(0, -1), "facts do not leak across stages");

            // 纹理缓冲长度是"句柄个数"（契约 §4.1）：64 B 的 cbuf = 8 个句柄。
            Check(matrixAccessor.QueryTextureArrayLengthFromBuffer(0) == 8, "texture buffer length is a handle count");
            Throws("MISSING_FACT", () => matrixAccessor.QueryTextureArrayLengthFromBuffer(1),
                "an unlisted texture buffer slot is a missing fact");

            // 反射到的 cbuf 句柄只有在同句柄直接事实存在时才放行：native 按 handleIndex
            // 找回同一个 NVN 组合槽；没有直接事实的 cbuf 句柄仍是未实现路径。
            var resolvePoolSlots = typeof(DirectShaderCompiler).GetMethod("ResolvePoolSlots",
                System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic);
            Check(resolvePoolSlots != null, "production pool-slot resolver exists");
            var cbufAllocator = new DirectAllocator(matrixProfile);
            cbufAllocator.Bindings.Add(new DirectBinding("fs", "texture", "test", 2, 0, 1));
            ShaderProgram CbufProgram(int handleIndex) => new(
                new ShaderProgramInfo(Array.Empty<BufferDescriptor>(), Array.Empty<BufferDescriptor>(),
                    new[] { new TextureDescriptor(2, 0, SamplerType.Texture2D, TextureFormat.R8G8B8A8Unorm,
                        cbufSlot: 4, handleIndex: handleIndex, arrayLength: 1, separate: false, TextureUsageFlags.None) },
                    Array.Empty<TextureDescriptor>(), ShaderStage.Fragment, 0, 0, 0,
                    usesFragCoord: false, usesInstanceId: false, usesDrawParameters: false, usesRtLayer: false,
                    clipDistancesWritten: 0, fragmentOutputMap: 0),
                Ryujinx.Graphics.Shader.Translation.TargetLanguage.Spirv, Array.Empty<byte>());
            var cbufTranslated = new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>
                { (program.StageOf("fs"), CbufProgram(0), matrixAccessor) };
            resolvePoolSlots.Invoke(null, new object[] { cbufTranslated, cbufAllocator });
            Check(cbufAllocator.Bindings.Last().Slot == 0, "cbuf handle with a direct fact keeps the reflected handle index");
            var missingCbufAllocator = new DirectAllocator(matrixProfile);
            missingCbufAllocator.Bindings.Add(new DirectBinding("fs", "texture", "test", 2, 0, 1));
            var missingCbuf = new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>
                { (program.StageOf("fs"), CbufProgram(9), matrixAccessor) };
            bool cbufRejected = false;
            try { resolvePoolSlots.Invoke(null, new object[] { missingCbuf, missingCbufAllocator }); }
            catch (System.Reflection.TargetInvocationException error)
            {
                cbufRejected = error.InnerException is DirectCompileException compile && compile.Code == "UNSUPPORTED_PROGRAM";
            }
            Check(cbufRejected, "cbuf handle without a direct fact is rejected");
            // Facts alone do not implement separate binding; the production resolver must still refuse it.
            var separateAllocator = new DirectAllocator(matrixProfile);
            separateAllocator.Bindings.Add(new DirectBinding("fs", "texture", "test", 2, 0, 1));
            ShaderProgram separateProgram = new(
                new ShaderProgramInfo(Array.Empty<BufferDescriptor>(), Array.Empty<BufferDescriptor>(),
                    new[] { new TextureDescriptor(2, 0, SamplerType.Texture2D, TextureFormat.R8G8B8A8Unorm,
                        cbufSlot: 4, handleIndex: 0, arrayLength: 1, separate: true, TextureUsageFlags.None) },
                    Array.Empty<TextureDescriptor>(), ShaderStage.Fragment, 0, 0, 0,
                    usesFragCoord: false, usesInstanceId: false, usesDrawParameters: false, usesRtLayer: false,
                    clipDistancesWritten: 0, fragmentOutputMap: 0),
                Ryujinx.Graphics.Shader.Translation.TargetLanguage.Spirv, Array.Empty<byte>());
            var separateTranslated = new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>
                { (program.StageOf("fs"), separateProgram, matrixAccessor) };
            bool separateRejected = false;
            try { resolvePoolSlots.Invoke(null, new object[] { separateTranslated, separateAllocator }); }
            catch (System.Reflection.TargetInvocationException error)
            {
                separateRejected = error.InnerException is DirectCompileException compile && compile.Code == "UNSUPPORTED_PROGRAM" && compile.Message.Contains("separate sampler");
            }
            Check(separateRejected, "separate binding stays unsupported when its texture fact exists");

            // 解析期拒绝：未知格式、未知 sampler 分量、越界 arrayLength。
            Throws("MISSING_FACT", () => DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "x", vs, fs,
                textureFacts: "[{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"NotAFormat\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]")),
                "unknown texture format rejected");
            Throws("INVALID_JSON", () => DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "x", vs, fs,
                textureFacts: "[{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d-warp\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]")),
                "unknown sampler part rejected");
            Throws(null, () => DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "x", vs, fs,
                textureFacts: "[{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":0}]")),
                "zero array length rejected");
        }

        // NVN c2 separate texture/sampler handle facts: the OR operands may arrive reversed.
        {
            using JsonDocument profileDoc = JsonDocument.Parse(profile);
            DirectProfile p = DirectProfile.Parse(profileDoc.RootElement, "profile");
            string facts = "[{\"stage\":\"vs\",\"handle\":90,\"cbufSlot\":2,\"samplerType\":\"cube\",\"format\":\"Unknown\",\"isBuffer\":false,\"normalized\":true,\"separate\":true,\"arrayLength\":1}," +
                "{\"stage\":\"fs\",\"handle\":90,\"cbufSlot\":2,\"samplerType\":\"2d\",\"format\":\"Unknown\",\"isBuffer\":false,\"normalized\":false,\"separate\":true,\"arrayLength\":1}]";
            using JsonDocument d = JsonDocument.Parse(RequestJson(profile, "graphics", 1, 1, "sep", vs, fs, textureFacts: facts));
            DirectProgram prog = DirectProgram.Parse(d.RootElement.GetProperty("program"), "program", p);
            var va = new DirectGpuAccessor(p, prog, prog.StageOf("vs"), 1, new DirectAllocator(p), new List<DirectQuery>());
            var fa = new DirectGpuAccessor(p, prog, prog.StageOf("fs"), 5, new DirectAllocator(p), new List<DirectQuery>());
            int slots = TextureHandle.PackSlots(2, 2);
            int forward = TextureHandle.PackOffsets(90, 346, TextureHandleType.SeparateSamplerHandle);
            int reversed = TextureHandle.PackOffsets(346, 90, TextureHandleType.SeparateSamplerHandle);
            try {
                Check(va.QuerySamplerType(forward, slots) == SamplerType.TextureCube && va.QueryTextureCoordNormalized(reversed, slots), "separate facts VS forward/reversed use its texture view");
                Check(fa.QuerySamplerType(reversed, slots) == SamplerType.Texture2D && !fa.QueryTextureCoordNormalized(forward, slots), "separate facts FS forward/reversed use its texture view");
            } catch (DirectCompileException e) { Check(false, "separate facts query should resolve: " + e.Code + " " + e.Message); }
            Throws("MISSING_FACT", () => fa.QuerySamplerType(forward, TextureHandle.PackSlots(3, 2)), "separate facts cannot use another cbuf");
            Throws("MISSING_FACT", () => fa.QuerySamplerType(TextureHandle.PackOffsets(90, 92, TextureHandleType.SeparateSamplerHandle), slots), "separate facts reject ambiguous texture operands");
            Throws("MISSING_FACT", () => fa.QuerySamplerType(TextureHandle.PackOffsets(92, 346, TextureHandleType.SeparateSamplerHandle), slots), "separate facts do not borrow another texture slot");
            Throws("MISSING_FACT", () => fa.QuerySamplerType(TextureHandle.PackOffsets(90, 602, TextureHandleType.SeparateSamplerHandle), slots), "separate facts reject sampler outside c2 table");
            Throws("MISSING_FACT", () => fa.QuerySamplerType(TextureHandle.PackOffsets(91, 346, TextureHandleType.SeparateSamplerHandle), slots), "separate facts reject unaligned texture word");
            Throws("MISSING_FACT", () => fa.QuerySamplerType(TextureHandle.PackOffsets(90, 346, TextureHandleType.SeparateSamplerId), slots), "separate facts reject unsupported handle kind");
            using JsonDocument combinedDoc = JsonDocument.Parse(RequestJson(profile, "graphics", 1, 1, "wrong-kind", vs, fs,
                textureFacts: facts.Replace("\"separate\":true", "\"separate\":false")));
            DirectProgram combinedProgram = DirectProgram.Parse(combinedDoc.RootElement.GetProperty("program"), "program", p);
            var combined = new DirectGpuAccessor(p, combinedProgram, combinedProgram.StageOf("fs"), 5, new DirectAllocator(p), new List<DirectQuery>());
            Throws("MISSING_FACT", () => combined.QuerySamplerType(forward, slots), "separate query cannot borrow a combined texture fact");
        }

        // 7c) guest UBO：c[0] 的 LDC 必须变成 set0 binding1 的 uniform binding、槽位 0，
        //     并且 constantBufferUse 记录了用到的槽位（前端据此绑定 guest 常量缓冲）。
        {
            using JsonDocument doc = JsonDocument.Parse(DirectShaderCompiler.Compile(
                RequestJson(profile, "graphics", 1, 1, "guest-ubo", vs, FsUniform(), fsCbUse: 1)));
            JsonElement content = doc.RootElement.GetProperty("content");
            var uniforms = content.GetProperty("bindings").EnumerateArray()
                .Where(b => b.GetProperty("kind").GetString() == "uniform").ToList();
            Check(uniforms.Count == 1, "one uniform binding");
            Check(uniforms[0].GetProperty("set").GetInt32() == 0 && uniforms[0].GetProperty("binding").GetInt32() == 1,
                "uniform binding sits after the support buffer");
            Check(uniforms[0].GetProperty("slot").GetInt32() == 0 &&
                  uniforms[0].GetProperty("source").GetString() == "guestUBO[0]", "uniform slot/source");
            JsonElement fsStage = content.GetProperty("stages")[1];
            Check(fsStage.GetProperty("constantBufferUse").GetInt32() == 1, "fs constantBufferUse records c[0]");
            JsonElement cbuffers = fsStage.GetProperty("info").GetProperty("cbuffers");
            Check(cbuffers.GetArrayLength() == 1 && cbuffers[0].GetProperty("slot").GetInt32() == 0,
                "fs reflects one constant buffer at slot 0");
        }

        // 8) 资源上限：上限 1 的 profile 第二个纹理必须拒绝。
        using (JsonDocument limitedDoc = JsonDocument.Parse(ProfileJson(maxTextures: 1)))
        {
            DirectProfile limited = DirectProfile.Parse(limitedDoc.RootElement, "profile");
            var allocator = new DirectAllocator(limited);
            Check(allocator.Texture("fs", 1, false) == new SetBindingPair(2, 0), "first texture fits");
            Throws("RESOURCE_LIMIT", () => allocator.Texture("fs", 1, false), "second texture exceeds limit");
        }

        // 9) NativeAOT C ABI shim：同一请求经 unmanaged 入口 + emit 回调得到完全相同的字节。
        {
            byte[] direct = DirectShaderCompiler.Compile(graphicsRequest);
            _emitted = null;
            var entry = (delegate* unmanaged<ulong, ulong, DirectBytes, nint, nint, int>)&DirectCompileNative.Compile;
            int rc;
            fixed (byte* requestData = graphicsRequest)
                rc = entry(1, 1, new DirectBytes { Data = (nint)requestData, Bytes = (ulong)graphicsRequest.Length },
                    (nint)(delegate* unmanaged<nint, DirectBytes, int>)&Emit, 0);
            Check(rc == 0 && _emitted != null && _emitted.SequenceEqual(direct), "unmanaged provider returns the same program bytes");
            Check(entry(0, 1, default, 0, 0) == DirectAbi.Argument, "unmanaged provider rejects empty input");
        }

        // 10) 支持缓冲布局身份稳定。
        SupportLayout a1 = SupportLayout.Compile(), a2 = SupportLayout.Compile();
        Check(a1.Hash == a2.Hash && a1.RequiredSize > 0 && a1.Binding == 0 && a1.Fields.Length >= 8, "support layout stable");

        Console.WriteLine(_failures == 0
            ? $"direct-compiler: ok ({_checks} checks)"
            : $"direct-compiler: {_failures} FAILURES / {_checks} checks");
        Environment.ExitCode = _failures == 0 ? 0 : 1;
    }
}
