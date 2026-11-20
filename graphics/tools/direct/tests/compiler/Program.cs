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
    // LOP.OR Rd, Ra, Rb: exact Maxwell opcode and fields from InstTable / InstLopR.
    private static ulong LopOr(int dest, int a, int b) =>
        0x5C40000000000000UL | (7UL << 48) | (1UL << 41) | ((ulong)b << 20) | (7UL << 16) | ((ulong)a << 8) | (uint)dest;
    // LOP32I.OR Rd, Ra, imm32：顶层 opcode 000001（bit 58）、逻辑运算在 bit 53..54（1 = OR）、32 位立即数在 bit 20..51（InstTable / InstLop32i）。
    // LOP32I.AND Rd, Ra, imm32：同上，逻辑运算字段 0 = AND。
    private static ulong Lop32iAnd(int dest, int a, uint imm) =>
        0x0400000000000000UL | ((ulong)imm << 20) | (7UL << 16) | ((ulong)a << 8) | (uint)dest;
    private static ulong Lop32iOr(int dest, int a, uint imm) =>
        0x0400000000000000UL | (1UL << 53) | ((ulong)imm << 20) | (7UL << 16) | ((ulong)a << 8) | (uint)dest;
    // FMNMX Rd, Ra, Rb（寄存器形式 0x5c60，谓词 bit 39 = PT 取 MIN、bit 42 取反 = MAX；编码同 tests/shader_cases/run.py）。
    private static ulong FmnmxR(int dest, int a, int b, bool max) =>
        0x5C60000000000000UL | ((ulong)b << 20) | ((max ? 1UL : 0UL) << 42) | (7UL << 39) | (7UL << 16) | ((ulong)a << 8) | (uint)dest;
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
    // STS / LDS [RZ + imm], B32（InstSts / InstLds：Rd 0..7、Ra 8..15、Imm24 20..43、LsSize 48..50 = B32 4）；BAR.SYNC 0（InstBar，BarOp Sync = 0）
    private static ulong Sts(int reg, int offset) => 0xEF58000000000000UL | (4UL << 48) | ((ulong)offset << 20) | (7UL << 16) | (0xFFUL << 8) | (uint)reg;
    private static ulong Lds(int reg, int offset) => 0xEF48000000000000UL | (4UL << 48) | ((ulong)offset << 20) | (7UL << 16) | (0xFFUL << 8) | (uint)reg;
    private const ulong BarSync = 0xF0A8000000000000UL | (7UL << 16) | (0xFFUL << 8);
    // 计算核（没有着色器头）：每 3 条指令前一个调度字，末尾 EXIT 加自跳转
    private static byte[] ComputeOps(params ulong[] ops)
    {
        var all = new List<ulong>(ops) { Exit, 0xE2400FFFFF07000FUL };
        var words = new List<ulong>();
        for (int i = 0; i < all.Count; i += 3) { words.Add(Sched); for (int j = 0; j < 3; j++) words.Add(i + j < all.Count ? all[i + j] : 0x50B0000000070F00UL); }
        return MemoryMarshal.AsBytes(words.ToArray().AsSpan()).ToArray();
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

    // 非常量下标读 c0（boundDynamic，图形阶段）：照 Ryujinx 翻 LDC c[0][Rx + imm] 的形状
    //   a = x & mask; addr = a + imm; wo = addr >> 2; vec = wo >> 2; elem = wo & 3; OpAccessChain %c0 0 vec elem
    // x 来自输入变量（取值不可证）。变体：
    //   and / reach / above：mask、imm 不同（证得的 vec 区间分别在 0x644 所在第 100 个之前 / 含它 / 之后）
    //   unmasked：没有 & mask；huge：mask 大到 vec 上界 ≥ 数组长度；local：a 先存进函数局部变量再读回；
    //   local-escape：局部变量的指针另有用法；local-two：局部变量还存过不可证的 x；
    //   loop：下标是循环 phi（i = phi(0, i + 1)）；sclamp：clamp(x, 0, 63)；smin：min(x, 63)（有符号，x 可能为负）
    private static byte[] C0DynFixture(string v, uint mask = 0x3F0, uint imm = 0)
    {
        var w = new List<uint> { 0x07230203, 0x10000, 0, 2000, 0 };
        void Emit(uint op, params uint[] args) { w.Add(((uint)args.Length + 1) << 16 | op); w.AddRange(args); }
        Emit(17, 1);
        Emit(11, 190, 0x4C534C47, 0x6474732E, 0x3035342E, 0);   // OpExtInstImport "GLSL.std.450"
        Emit(71, 100, 33, 1); Emit(71, 100, 34, 0);
        Emit(71, 103, 6, 16); Emit(72, 104, 0, 35, 0);
        Emit(21, 101, 32, 0); Emit(23, 102, 101, 4);
        Emit(43, 101, 110, 4096); Emit(28, 103, 102, 110);
        Emit(30, 104, 103); Emit(32, 105, 2, 104);
        Emit(32, 106, 2, 101); Emit(32, 108, 1, 101); Emit(32, 160, 7, 101);
        Emit(43, 101, 111, 0); Emit(43, 101, 150, mask); Emit(43, 101, 151, imm); Emit(43, 101, 152, 2); Emit(43, 101, 153, 3);
        Emit(43, 101, 154, 63); Emit(43, 101, 155, 1);
        Emit(59, 105, 100, 2);
        Emit(59, 108, 109, 1);                 // 输入变量：读出的 x 取值不可证
        Emit(61, 101, 140, 109);
        uint vec, elem;
        if (v is "loop" or "sclamp" or "smin")
        {
            if (v == "loop") { Emit(245, 101, 170, 111, 1, 171, 2); Emit(128, 101, 171, 170, 155); vec = 170; }
            else if (v == "sclamp") { Emit(12, 101, 146, 190, 45, 140, 111, 154); vec = 146; }
            else { Emit(12, 101, 146, 190, 39, 140, 154); vec = 146; }
            elem = 111;
        }
        else
        {
            uint a = 141;
            if (v == "unmasked") a = 140; else Emit(199, 101, 141, 140, 150);
            if (v is "local" or "local-escape" or "local-two")
            {
                Emit(59, 160, 161, 7); Emit(62, 161, 141);
                if (v == "local-two") Emit(62, 161, 140);
                if (v == "local-escape") Emit(83, 160, 163, 161);
                Emit(61, 101, 162, 161); a = 162;
            }
            Emit(128, 101, 142, a, 151); Emit(194, 101, 143, 142, 152); Emit(194, 101, 144, 143, 152); Emit(199, 101, 145, 143, 153);
            vec = 144; elem = 145;
        }
        Emit(65, 106, 120, 100, 111, vec, elem);
        Emit(61, 101, 121, 120);
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

    // 计算模块里唯一一个存储图像（OpTypeImage Sampled = 2）的声明：(Image Format 操作数, Sampled Type 是否 uint)
    private static (uint format, bool uintSampled, int images) StorageImageDecl(JsonElement stage)
    {
        uint[] words = MemoryMarshal.Cast<byte, uint>(stage.GetProperty("spirv").GetBytesFromBase64()).ToArray();
        var ints = new Dictionary<uint, uint>(); uint format = 99, sampledType = 0; int images = 0;
        for (int i = 5; i < words.Length;)
        {
            int count = (int)(words[i] >> 16); uint op = words[i] & 0xffff;
            if (count == 0 || i + count > words.Length) throw new InvalidDataException("invalid SPIR-V instruction");
            if (op == 21 && count >= 4) ints[words[i + 1]] = words[i + 3];
            if (op == 25 && count >= 9 && words[i + 7] == 2) { images++; format = words[i + 8]; sampledType = words[i + 2]; }
            i += count;
        }
        return (format, ints.TryGetValue(sampledType, out uint signed) && signed == 0, images);
    }

    private static bool SampledImageType(byte[] spv, int wantedOp, int wantedSigned)
    {
        var types = new Dictionary<uint, (int Op, int Signed)>(); int images = 0, match = 0;
        for (int i = 5; i * 4 < spv.Length;)
        {
            uint w0 = BitConverter.ToUInt32(spv, i * 4); int op = (int)(w0 & 0xffff), n = (int)(w0 >> 16);
            if (n == 0 || (i + n) * 4 > spv.Length) throw new InvalidDataException("invalid SPIR-V instruction");
            if (op == 21) types[BitConverter.ToUInt32(spv, (i + 1) * 4)] = (21, (int)BitConverter.ToUInt32(spv, (i + 3) * 4));
            if (op == 22) types[BitConverter.ToUInt32(spv, (i + 1) * 4)] = (22, -1);
            if (op == 25) { images++; if (types.TryGetValue(BitConverter.ToUInt32(spv, (i + 2) * 4), out var t) && t.Op == wantedOp && t.Signed == wantedSigned) match++; }
            i += n;
        }
        return images == 1 && match == 1;
    }

    // 原创合成 Maxwell CS：R0 = c0[0x310]（存储缓冲地址），R4 = R5 = 0，SULD.D.B32 R2, [R4]（二维、图像句柄 c2 字 74，按位读），STG [R0], R2。
    //   SULD.D 的字段来自 InstSuldD（目的 0..7、坐标 8..15、谓词 16..18、大小 20..22 = B32、目的谓词 30..32、维度 33..35 = 2D、句柄 36..48）。
    private static byte[] ComputeRawImageLoadGuest()
    {
        ulong[] ops = { 0xEF9500310007FF00UL, 0x1C0000000007FF04UL, 0x1C0000000007FF05UL, 0xEB1804A7C0470402UL, 0xEEDC200000070002UL, Exit };
        byte[] code = new byte[((ops.Length + 2) / 3) * 32];
        for (int i = 0; i < ops.Length; i++)
            BinaryPrimitives.WriteUInt64LittleEndian(code.AsSpan((i / 3 * 4 + i % 3 + 1) * 8), ops[i]);
        return code;
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
    // C1：在 CapsJson 的稳定输出里插 descriptorIndexing 对象（不改 CapsJson 签名，基线测试要能原样编译）。
    // enabled=false 时全部七位为 false；enabled=true 时按 storage 参数造「四位齐」或「缺存储位」。
    private static byte[] CapsJsonWithDescriptorIndexing(bool enabled, bool storage)
    {
        string text = Encoding.UTF8.GetString(CapsJson());
        string objectText = "\"descriptorIndexing\":{\"enabled\":" + (enabled ? "true" : "false") +
            ",\"runtimeArray\":" + (enabled ? "true" : "false") +
            ",\"partiallyBound\":" + (enabled ? "true" : "false") +
            ",\"sampledImageArrayNonUniformIndexing\":" + (enabled ? "true" : "false") +
            ",\"storageImageArrayNonUniformIndexing\":" + (enabled && storage ? "true" : "false") +
            ",\"variableDescriptorCount\":false,\"sampledImageUpdateAfterBind\":false},";
        text = text.Replace("\"formats\":", objectText + "\"formats\":", StringComparison.Ordinal);
        return Encoding.UTF8.GetBytes(text);
    }

    // C1：造一段最小 SPIR-V（头 + 能力 / 扩展前导段 + OpMemoryModel），只给能力核对用。
    private static uint[] SpvDescriptorIndexing(bool ext, bool shaderNonUniform, bool sampled, bool storage)
    {
        var words = new List<uint> { 0x07230203, 0x00010300, 0, 1, 0 };
        words.Add((2u << 16) | 17); words.Add(1);   // OpCapability Shader
        if (ext)
        {
            const string name = "SPV_EXT_descriptor_indexing";
            int count = (name.Length + 1 + 3) / 4;
            words.Add(((uint)count + 1) << 16 | 10);   // OpExtension
            uint word = 0;
            for (int i = 0; i <= count * 4; i++)
            {
                byte b = i < name.Length ? (byte)name[i] : (byte)0;
                word |= (uint)b << ((i % 4) * 8);
                if (i % 4 == 3) { words.Add(word); word = 0; }
            }
        }
        void Cap(uint cap) { words.Add((2u << 16) | 17); words.Add(cap); }
        if (shaderNonUniform) Cap(5301);
        if (sampled) Cap(5307);
        if (storage) Cap(5309);
        words.Add((3u << 16) | 14); words.Add(0); words.Add(1);   // OpMemoryModel Logical GLSL450
        return words.ToArray();
    }

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
        string textureBufferLengths = null, uint csCbUse = 0, uint sharedBytes = 0, bool depthMode = false)
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
                BinaryPrimitives.WriteUInt32LittleEndian(control.AsSpan(0x754), sharedBytes);
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
                w.WriteBoolean("depthMode", depthMode);
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
                w.WriteNumber("sharedMemoryBytes", (sharedBytes + 15) / 16 * 16);
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

    // 极简 SPIR-V 拼装器：每条指令 = opcode + 操作数字，只够喂 DirectRasterFacts.Analyze 的单元用例。
    private static uint[] A(params uint[] words) => words;
    private static uint[] I(uint op, params uint[][] parts)
    {
        var r = new List<uint> { op };
        foreach (uint[] p in parts) r.AddRange(p);
        return r.ToArray();
    }
    private static uint[] SpvStr(string s)
    {
        byte[] b = Encoding.ASCII.GetBytes(s + "\0");
        var r = new uint[(b.Length + 3) / 4];
        for (int i = 0; i < b.Length; i++) r[i / 4] |= (uint)b[i] << ((i % 4) * 8);
        return r;
    }
    private static byte[] Spv(params uint[][] insts)
    {
        var words = new List<uint> { 0x07230203, 0x00010000, 0, 128, 0 };
        foreach (uint[] inst in insts)
        {
            words.Add((uint)(inst.Length << 16) | inst[0]);
            words.AddRange(inst.Skip(1));
        }
        var bytes = new byte[words.Count * 4];
        for (int i = 0; i < words.Count; i++) BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(i * 4), words[i]);
        return bytes;
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
        string sampledSpvDir = args.Length == 2 && args[0] == "--emit-sampled" ? Path.GetFullPath(args[1]) : null;
        if (sampledSpvDir != null) Directory.CreateDirectory(sampledSpvDir);
        // GPU oracle: transform the exact reference shader with production code.
        if (args.Length == 4 && args[0] == "--spill-spirv")
        {
            try
            {
                int limit = int.Parse(args[3]);
                var spillResult = DirectSharedSpill.Rewrite(File.ReadAllBytes(args[1]), limit, 0, 1);
                if (spillResult == null) throw new DirectCompileException("UNSUPPORTED_PROGRAM", "fixture did not require a spill");
                // 重新解析产物，不能只信选择器自己的 remaining 账本：留在 Workgroup 的数组必须真的不超过设备上限。
                int measured = DirectSharedSpill.WorkgroupBytes(spillResult.Spirv);
                if (measured < 0 || measured != spillResult.RemainingSharedBytes || measured > limit)
                    throw new DirectCompileException("RESOURCE_LIMIT", $"spill retained workgroup bytes {measured}, reported {spillResult.RemainingSharedBytes}, limit {limit}");
                File.WriteAllBytes(args[2], spillResult.Spirv);
                Console.WriteLine($"spill bytes/group={spillResult.BytesPerGroup} remaining={spillResult.RemainingSharedBytes} measured={measured} set={spillResult.Set} binding={spillResult.Binding}");
                return;
            }
            catch (DirectCompileException e) { Console.Error.WriteLine($"{e.Code}: {e.Message}"); Environment.Exit(1); }
        }
        byte[] profile = ProfileJson();
        using (JsonDocument profileDoc = JsonDocument.Parse(profile))
        {
            var limits = DirectProfile.Parse(profileDoc.RootElement, "spill-profile");
            Check(limits.Caps.MaximumComputeSharedMemorySize == 32768 && limits.TranslationCaps.MaximumComputeSharedMemorySize == 49152,
                  "spill keeps physical 32 KiB separate from translation 48 KiB");
        }
        using (JsonDocument largeProfileDoc = JsonDocument.Parse(Encoding.UTF8.GetString(profile).Replace("\"maximumComputeSharedMemorySize\":32768", "\"maximumComputeSharedMemorySize\":65536")))
        {
            var limits = DirectProfile.Parse(largeProfileDoc.RootElement, "large-spill-profile");
            Check(limits.Caps.MaximumComputeSharedMemorySize == 65536 && limits.TranslationCaps.MaximumComputeSharedMemorySize == 65536,
                  "translation limit preserves devices above 48 KiB");
        }
        byte[] vs = Guest(false, new[] { Ast(0) });
        byte[] fs = Guest(true, new[] { Ald(0) }, new[] { 0 });
        byte[] cs = ComputeGuest();

        // 端到端：真编译器（DirectShaderCompiler.Compile）编一段读两个输入分量、做 FMNMX.MIN / MAX 的片元程序（读输入免得被常量折叠），
        // 出来的片元 SPIR-V 必须有 GLSL NMin + NMax、没有 FMin / FMax（共用翻译器的 Clamp / Maximum / Minimum 若回到 F 版，这一项必红）。
        {
            using JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "nan-minmax",
                Guest(false, new[] { Ald4(0), Ast4Pos(0) }),
                Guest(true, new[] { Ald(0, 0, 4), Ald(0, 1, 5), FmnmxR(0, 4, 5, false), FmnmxR(1, 4, 5, true), Fadd32i(2, 0f), Fadd32i(3, 1f) }, new[] { 0 }))));
            byte[] spv = doc.RootElement.GetProperty("content").GetProperty("stages")[1].GetProperty("spirv").GetBytesFromBase64();
            var ops = new List<uint>(); var glsl = new HashSet<uint>();
            for (int i = 5; i < spv.Length / 4;)
            {
                uint head = BinaryPrimitives.ReadUInt32LittleEndian(spv.AsSpan(i * 4)); int n = (int)(head >> 16); if (n == 0) break;
                if ((head & 0xffff) == 11 && Encoding.ASCII.GetString(spv, (i + 2) * 4, (n - 2) * 4).TrimEnd('\0') == "GLSL.std.450") glsl.Add(BinaryPrimitives.ReadUInt32LittleEndian(spv.AsSpan((i + 1) * 4)));
                if ((head & 0xffff) == 12 && glsl.Contains(BinaryPrimitives.ReadUInt32LittleEndian(spv.AsSpan((i + 3) * 4)))) ops.Add(BinaryPrimitives.ReadUInt32LittleEndian(spv.AsSpan((i + 4) * 4)));
                i += n;
            }
            Check(ops.Contains(79) && ops.Contains(80) && !ops.Contains(37) && !ops.Contains(40),
                  "real compile: FMNMX emits GLSL NMin / NMax, no FMin / FMax (GLSL ops " + string.Join(",", ops) + ")");
        }

        // 光栅事实（raster 段）：live-compile 的最终 SPIR-V 上 SSA 溯源证出的保守事实，
        // 字段名/摆放与 android 侧 nvn_vk_program_result.c 的 stage "raster" 解析器一一对应。
        {
            byte[] fsFlat = Guest(true, new[] { Fadd32i(0, 1.0f), Fadd32i(1, 0.0f), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f) });
            // 正例：attr0 四分量直通到 out position，无深度尾巴 → pos_attr=0、w 取属性 .w、无 z-half。
            using (JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "raster-pos",
                Guest(false, new[] { Ald4(0), Ast4Pos(0) }), fsFlat))))
            {
                var stages = doc.RootElement.GetProperty("content").GetProperty("stages");
                bool hasRv = stages[0].TryGetProperty("raster", out var rv);
                bool hasRf = stages[1].TryGetProperty("raster", out var rf);
                Check(hasRv && rv.GetProperty("pos_attr").GetInt32() == 0, "raster: position passthrough proves pos_attr 0");
                Check(hasRv && !rv.GetProperty("pos_w1").GetBoolean(), "raster: w comes from the attribute, not constant 1");
                Check(hasRv && !rv.GetProperty("pos_z_half").GetBoolean(), "raster: no depth tail, no z-half fact");
                Check(hasRf && !rf.GetProperty("may_discard").GetBoolean(), "raster: kill-free fs proves may_discard false");
                Check(hasRf && !rf.GetProperty("writes_smask").GetBoolean(), "raster: no SampleMask output proves writes_smask false");
            }
            // depthMode=true：翻译器追加的无条件 z=fma(z,0.5,w*0.5) 尾巴 → pos_z_half=1，
            // pos_attr 仍指改写源的属性 0（回放侧 rp_draw.c 先读属性再对 P.z 重做同一变换）。
            using (JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "raster-zhalf",
                Guest(false, new[] { Ald4(0), Ast4Pos(0) }), fsFlat, depthMode: true))))
            {
                var stages = doc.RootElement.GetProperty("content").GetProperty("stages");
                bool hasRv = stages[0].TryGetProperty("raster", out var rv);
                Check(hasRv && rv.GetProperty("pos_attr").GetInt32() == 0, "raster: depth tail keeps pos_attr 0 (rewritten source)");
                Check(hasRv && rv.GetProperty("pos_z_half").GetBoolean(), "raster: unconditional depth tail proves pos_z_half");
            }
            // 单元夹具：对拼出来的模块直接跑分析器（直挂 BuiltIn Position、运行时 OpSelect 尾巴、
            // OpKill、BuiltIn SampleMask——这些形状现有夹具产不出）。
            var posHead = new List<uint[]>
            {
                I(14, A(0, 1)),                                   // OpMemoryModel Logical GLSL450
                I(15, A(0, 8), SpvStr("main"), A(9, 10)),          // OpEntryPoint Vertex %main %pos %attr
                I(71, A(9, 11, 0)),                                // OpDecorate %pos BuiltIn Position
                I(71, A(10, 30, 0)),                               // OpDecorate %attr Location 0
                I(19, A(2)), I(33, A(3, 2)), I(22, A(4, 32)),      // void / fn / f32
                I(23, A(5, 4, 4)),                                 // vec4 f32
                I(32, A(6, 3, 5)), I(32, A(7, 1, 5)),              // ptr Output vec4 / ptr Input vec4
                I(43, A(4, 31, 0x3F800000)), I(43, A(4, 32, 0x3F000000)),  // f32 1.0 / 0.5
                I(59, A(6, 9, 3)), I(59, A(7, 10, 1)),             // %pos out / %attr in
                I(54, A(2, 8, 0, 3)), I(248, A(12)),               // function + label
            };
            uint[][] tail = { I(253), I(56) };                     // return / function end
            // 整向量 attr 直通
            var r = DirectRasterFacts.Analyze(Spv(posHead.Concat(new[]
            {
                I(61, A(5, 13, 10)), I(62, A(9, 13))
            }).Concat(tail).ToArray()));
            Check(r.HasVertex && r.PosAttr == 0 && !r.PosW1 && !r.PosZHalf,
                  "raster unit: whole-vector attr passthrough proves pos_attr 0");
            // construct(x, y, z, 1.0)：w 常量 1 → pos_w1=1
            r = DirectRasterFacts.Analyze(Spv(posHead.Concat(new[]
            {
                I(61, A(5, 13, 10)),
                I(81, A(4, 14, 13, 0)), I(81, A(4, 15, 13, 1)), I(81, A(4, 16, 13, 2)),
                I(80, A(5, 17, 14, 15, 16, 31)), I(62, A(9, 17))
            }).Concat(tail).ToArray()));
            Check(r.PosAttr == 0 && r.PosW1, "raster unit: constant w=1.0 proves pos_w1");
            // 编译期 z-half：z' = z*0.5 + w*0.5（两步 FMul+FAdd）→ z_half=1 且 pos_attr 指向改写源 0
            r = DirectRasterFacts.Analyze(Spv(posHead.Concat(new[]
            {
                I(61, A(5, 13, 10)),
                I(81, A(4, 14, 13, 0)), I(81, A(4, 15, 13, 1)), I(81, A(4, 16, 13, 2)), I(81, A(4, 18, 13, 3)),
                I(133, A(4, 19, 16, 32)), I(133, A(4, 20, 18, 32)), I(129, A(4, 21, 19, 20)),
                I(80, A(5, 17, 14, 15, 21, 18)), I(62, A(9, 17))
            }).Concat(tail).ToArray()));
            Check(r.PosAttr == 0 && r.PosZHalf, "raster unit: FMul/FAdd z-half shape proves pos_z_half");
            // FMA 形态：ext inst GLSL.std.450 #50 → 同样证出
            r = DirectRasterFacts.Analyze(Spv(new[]
            {
                I(14, A(0, 1)), I(11, A(1), SpvStr("GLSL.std.450")),
                I(15, A(0, 8), SpvStr("main"), A(9, 10)),
                I(71, A(9, 11, 0)), I(71, A(10, 30, 0)),
                I(19, A(2)), I(33, A(3, 2)), I(22, A(4, 32)), I(23, A(5, 4, 4)),
                I(32, A(6, 3, 5)), I(32, A(7, 1, 5)),
                I(43, A(4, 32, 0x3F000000)),
                I(59, A(6, 9, 3)), I(59, A(7, 10, 1)),
                I(54, A(2, 8, 0, 3)), I(248, A(12)),
                I(61, A(5, 13, 10)),
                I(81, A(4, 14, 13, 0)), I(81, A(4, 15, 13, 1)), I(81, A(4, 16, 13, 2)), I(81, A(4, 18, 13, 3)),
                I(133, A(4, 20, 18, 32)), I(12, A(4, 21, 1, 50, 16, 32, 20)),
                I(80, A(5, 17, 14, 15, 21, 18)), I(62, A(9, 17))
            }.Concat(tail).ToArray()));
            Check(r.PosAttr == 0 && r.PosZHalf, "raster unit: GLSL Fma z-half shape proves pos_z_half");
            // 负例：z 走 OpSelect（运行时深度尾巴的形态）→ z 证不出，pos_attr=-1、z_half=0
            r = DirectRasterFacts.Analyze(Spv(new[]
            {
                I(14, A(0, 1)), I(15, A(0, 8), SpvStr("main"), A(9, 10)),
                I(71, A(9, 11, 0)), I(71, A(10, 30, 0)),
                I(19, A(2)), I(33, A(3, 2)), I(22, A(4, 32)), I(23, A(5, 4, 4)),
                I(32, A(6, 3, 5)), I(32, A(7, 1, 5)),
                I(20, A(40)), I(41, A(40, 41)),                     // bool + const true
                I(59, A(6, 9, 3)), I(59, A(7, 10, 1)),
                I(54, A(2, 8, 0, 3)), I(248, A(12)),
                I(61, A(5, 13, 10)),
                I(81, A(4, 14, 13, 0)), I(81, A(4, 15, 13, 1)), I(81, A(4, 16, 13, 2)), I(81, A(4, 18, 13, 3)),
                I(169, A(4, 19, 41, 16, 18)),                       // z = select(cond, z, w)
                I(80, A(5, 17, 14, 15, 19, 18)), I(62, A(9, 17))
            }.Concat(tail).ToArray()));
            Check(r.HasVertex && r.PosAttr == -1 && !r.PosZHalf,
                  "raster unit: OpSelect z is unprovable (pos_attr -1, z_half 0)");
            // 负例：z = x + y（非 z-half 形状）→ -1 / 0
            r = DirectRasterFacts.Analyze(Spv(posHead.Concat(new[]
            {
                I(61, A(5, 13, 10)),
                I(81, A(4, 14, 13, 0)), I(81, A(4, 15, 13, 1)), I(81, A(4, 18, 13, 3)),
                I(129, A(4, 19, 14, 15)),
                I(80, A(5, 17, 14, 15, 19, 18)), I(62, A(9, 17))
            }).Concat(tail).ToArray()));
            Check(r.PosAttr == -1 && !r.PosZHalf, "raster unit: plain FAdd z is unprovable");
            // FS：OpKill → may_discard=1；写 BuiltIn SampleMask 输出 → writes_smask=1
            var fsHead = new List<uint[]>
            {
                I(14, A(0, 1)), I(15, A(4, 8), SpvStr("main"), A(45)),
                I(71, A(45, 11, 20)),                                // %sm BuiltIn SampleMask(20)
                I(19, A(2)), I(33, A(3, 2)), I(21, A(42, 32, 1)),    // void / fn / i32
                I(32, A(43, 3, 42)),                                 // ptr Output i32
                I(43, A(42, 44, 0)),                                 // i32 const 0
                I(59, A(43, 45, 3)),                                 // %sm out
                I(54, A(2, 8, 0, 3)), I(248, A(12)),
            };
            r = DirectRasterFacts.Analyze(Spv(fsHead.Concat(new[]
            {
                I(62, A(45, 44)), I(252)
            }).Concat(tail).ToArray()));
            Check(r.HasFragment && r.MayDiscard && r.WritesSampleMask,
                  "raster unit: OpKill and SampleMask store prove both flags");
            r = DirectRasterFacts.Analyze(Spv(fsHead.Concat(new[] { I(253) }).Concat(tail).ToArray()));
            Check(!r.MayDiscard && !r.WritesSampleMask,
                  "raster unit: kill-free mask-free fs proves both false");
            // 截断模块 → 全保守（may_discard / writes_smask 只能保守真）。
            r = DirectRasterFacts.Analyze(new byte[] { 0x03, 0x02, 0x23, 0x07 });
            Check(!r.HasVertex && r.PosAttr == -1 && r.MayDiscard && r.WritesSampleMask,
                  "raster unit: truncated module falls back to conservative defaults");
        }

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

        // 计算共享内存（#1592）：48 KiB 内请求交给翻译后 spill 判断；超过上限 / 负数仍是坏请求。
        {
            string csRaw = Encoding.UTF8.GetString(RequestJson(profile, "compute", 2, 1, "shared-limit", cs: cs));
            Func<int, byte[]> withShared = n => Encoding.UTF8.GetBytes(csRaw.Replace("\"sharedMemoryBytes\":0", "\"sharedMemoryBytes\":" + n));
            // 等于上限过得了上限检查（之后按控制块核对 1x1x1 shared 0 不符才拒，那是另一道检查）
            try { DirectShaderCompiler.Compile(withShared(32768)); Check(true, "shared 32768 passes the device limit check"); }
            catch (Exception error) { Check(!error.Message.Contains("exceeds"), "shared 32768 = device limit passes the limit check: " + error.Message); }
            using var csFacts = JsonDocument.Parse("{\"localSize\":[1,1,1],\"localMemoryBytes\":0,\"sharedMemoryBytes\":40960,\"hasUnalignedStorageBuffer\":false}");
            using var profileDoc = JsonDocument.Parse(profile);
            var facts = DirectComputeFacts.Parse(csFacts.RootElement, "compute", DirectProfile.Parse(profileDoc.RootElement, "profile"));
            Check(facts.SharedMemoryBytes == 40960, "shared 40960 passes parsing for spill on a 32768-byte device");
            Throws("MISSING_FIELD", () => DirectShaderCompiler.Compile(withShared(49153)), "shared above Maxwell 48 KiB is a bad request");
            Throws("MISSING_FIELD", () => DirectShaderCompiler.Compile(withShared(-1)), "negative shared is a bad request");
        }

        // 真实编译器输出：采样图像的 OpTypeImage Sampled Type 应与绑定视图数值类型一致。
        foreach (var test in new[] { (Format: "R16G16Uint", Op: 21, Signed: 0), (Format: "R16G16Sint", Op: 21, Signed: 1),
                                     (Format: "R16G16Float", Op: 22, Signed: -1), (Format: "R16G16Unorm", Op: 22, Signed: -1) })
        {
            string fact = "[{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"" + test.Format + "\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            using JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "sampled-" + test.Format,
                Guest(false, new[] { Ald4(0), Ast4Pos(0) }), Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(0, 2) }), textureFacts: fact)));
            byte[] spv = Convert.FromBase64String(doc.RootElement.GetProperty("content").GetProperty("stages")[1].GetProperty("spirv").GetString());
            if (sampledSpvDir != null) File.WriteAllBytes(Path.Combine(sampledSpvDir, test.Format + ".spv"), spv);
            Check(SampledImageType(spv, test.Op, test.Signed), "sampled texture " + test.Format + " declares its numeric type");
            var formatQueries = doc.RootElement.GetProperty("content").GetProperty("dependencies").EnumerateArray()
                .Where(d => d.GetProperty("method").GetString() == "QueryS2ASampledTextureFormat").ToArray();
            Check(formatQueries.Length == 1 && formatQueries[0].GetProperty("value").GetString() == test.Format,
                "sampled texture " + test.Format + " records its format dependency");
        }
        {
            // 分离贴图表的 c2 字 0x5c：与日志里的 fp_t_tcb_5C 相同的句柄坐标。
            const string fact = "[{\"stage\":\"fs\",\"handle\":92,\"cbufSlot\":2,\"samplerType\":\"2d\",\"format\":\"R16G16Uint\",\"isBuffer\":false,\"normalized\":true,\"separate\":true,\"arrayLength\":1}]";
            using JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "sampled-separate-c2-5c",
                Guest(false, new[] { Ald4(0), Ast4Pos(0) }), Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(0, 2, 92) }), textureFacts: fact)));
            var deps = doc.RootElement.GetProperty("content").GetProperty("dependencies").EnumerateArray()
                .Where(d => d.GetProperty("method").GetString() == "QueryS2ASampledTextureFormat").ToArray();
            Check(deps.Length == 1 && deps[0].GetProperty("value").GetString() == "R16G16Uint",
                "sampled c2 separate word 0x5c uses its uint format fact");
            byte[] spv = doc.RootElement.GetProperty("content").GetProperty("stages")[1].GetProperty("spirv").GetBytesFromBase64();
            Check(SampledImageType(spv, 21, 0), "sampled c2 separate word 0x5c declares uint");
            if (sampledSpvDir != null) File.WriteAllBytes(Path.Combine(sampledSpvDir, "c2-5c.spv"), spv);
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
                string reqSha = Convert.ToHexStringLower(Sha256.HashData(Encoding.UTF8.GetBytes(dynamicRequest)));
                Check(error.Code == "UNSUPPORTED_PROGRAM" && error.Message.Contains("texture handle from cbuf slot 0") &&
                      error.Message.Contains("handle=") && error.Message.Contains("arrayLength=") &&
                      error.Message.Contains("count=") && error.Message.Contains("request=" + reqSha),
                    "runtime handle reports its actual upstream cbuf limitation with shape and request sha: " + error.Message);
                // #999：请求的 requestId 经 Compile → 分配器 → 被拒消息，原样出现在末尾「[kind=… key=…]」里（真实翻译器、真实请求）；
                //   #1488 的基础消息已带 handle= / arrayLength= / count= / request=<sha>，现场不再重复写这三项
                Check(error.Message.Contains(" [kind=texture word=0 word2=0 type=4 key=runtime-pool]") && error.Message.EndsWith("]") &&
                      !error.Message.Contains("] [") && error.Message.IndexOf(" [kind=", StringComparison.Ordinal) > error.Message.IndexOf("request=" + reqSha + ")", StringComparison.Ordinal),
                    "the request's requestId reaches the reject message through the real compile path, after the shape facts: " + error.Message);
                Check(error.Message.Length <= 245, "the real-path reject message stays inside the host's 245-char limit (" + error.Message.Length + ")");
            }
        }

        // 整池数组（追不到槽的句柄回退，安卓整池 N=0）：同一支「句柄来自属性」的片元，池长在 16 位布局能表达的范围内时编得出，
        //   结果里是两条 pool 绑定（贴图池长 64、采样器池长 8），不带 slot / samp_slot；池长 524288 时（上面那例）照旧具名拒
        {
            byte[] fsPool = Guest(true, new[] { Ald(1, 0, 4), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 4) }, new[] { 1 });
            const string poolFact = "[{\"stage\":\"fs\",\"handle\":0,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            byte[] poolReq = RequestJson(profile, "graphics", 1, 1, "pool-array", vs, fsPool, textureFacts: poolFact);
            try
            {
                using JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(poolReq));
                JsonElement[] binds = doc.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray().ToArray();
                JsonElement[] texPool = binds.Where(b => b.TryGetProperty("pool", out JsonElement p) && p.GetString() == "texture").ToArray();
                JsonElement[] sampPool = binds.Where(b => b.TryGetProperty("pool", out JsonElement p) && p.GetString() == "sampler").ToArray();
                Check(texPool.Length == 1 && texPool[0].GetProperty("count").GetInt32() == 64 && texPool[0].GetProperty("stage").GetString() == "fs" &&
                      !texPool[0].TryGetProperty("slot", out _), "texture pool array: one fs binding, count = texture pool length 64, no slot");
                Check(sampPool.Length == 1 && sampPool[0].GetProperty("count").GetInt32() == 8 && !sampPool[0].TryGetProperty("slot", out _) &&
                      !sampPool[0].TryGetProperty("samp_slot", out _), "sampler pool array: one binding, count = sampler pool length 8, no slot");
            }
            catch (Exception error) { Check(false, "pool array compiles to two pool bindings: " + error.Message); }
            // 负对照：池长到 16 位布局表达不了（65535）时不收成池绑定，照旧具名拒
            string bigPool = Encoding.UTF8.GetString(poolReq).Replace("\"textureArrayLength\":64", "\"textureArrayLength\":65535");
            Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(bigPool)), "texture pool of 65535 entries is not taken as a pool binding");
        }

        // 句柄在游戏自己的 UBO 里（认领 #999）：LDC Rh, c[4][off]; TEX.B Rd, Rcoord, Rh。真实编译器、真实 BindlessElimination。
        //   A：字偏移 8（字节 0x20）正好等于组合槽 0 的句柄位置（8 + 2 × 槽）。请求只带运行时照常给的组合槽事实（handle 8 / cbufSlot −1）。
        //      这个句柄来自 c4、不是 c2 句柄表，不许当成组合槽 0 绑——修前「同 handle 事实回退」放行，绑到组合槽 0，读错图还不报。
        //   B：字节 0x400（字偏移 256，和组合区不重叠），请求带精确事实（handle 256 / cbufSlot 4）。第 2 片起编出贴图绑定，来源是
        //      uboSlot 1（NVN UBO 号 = cbuf − 3）/ uboOffset 1024（句柄字的字节偏移），不带 slot（slot 只指 c2 组合槽）。
        {
            byte[] fsUboA = Guest(true, new[] { Ldc(4, 0x20, 4), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 4) });
            const string comboFact = "[{\"stage\":\"fs\",\"handle\":8,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            string misbind = "";
            try
            {
                using JsonDocument r = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "ubo-handle-a", vs, fsUboA, textureFacts: comboFact, fsCbUse: 1u << 4)));
                foreach (var b in r.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray())
                    if (b.GetProperty("kind").GetString() == "texture" && !b.TryGetProperty("uboSlot", out _)) misbind += b.GetRawText();
            }
            catch (DirectCompileException) { }
            Check(misbind.Length == 0, "c4 handle at word 8 is not bound as combined texture slot 0: " + misbind);

            static string UboFact(int handle, int cbufSlot) =>
                "{\"stage\":\"fs\",\"handle\":" + handle + ",\"cbufSlot\":" + cbufSlot +
                ",\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}";
            // 编出的贴图绑定逐个列成「uboSlot/uboOffset/slot」，不带的字段写 -
            string UboTextures(string id, byte[] fs, string facts, uint cbUse)
            {
                try
                {
                    using JsonDocument r = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, id, vs, fs, textureFacts: facts, fsCbUse: cbUse)));
                    string Field(JsonElement b, string name) => b.TryGetProperty(name, out JsonElement f) ? f.GetInt32().ToString() : "-";
                    return string.Join(";", r.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                        .Where(b => b.GetProperty("kind").GetString() == "texture")
                        .Select(b => Field(b, "uboSlot") + "/" + Field(b, "uboOffset") + "/" + Field(b, "slot")).OrderBy(x => x, StringComparer.Ordinal));
                }
                catch (DirectCompileException error) { return error.Code + " " + error.Message; }
            }

            byte[] fsUboB = Guest(true, new[] { Ldc(4, 0x400, 4), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 4) });
            string got = UboTextures("ubo-handle-b", fsUboB, "[" + UboFact(256, 4) + "]", 1u << 4);
            Check(got == "1/1024/-", "c4 handle at byte 0x400 compiles to uboSlot 1 / uboOffset 1024 without a combined slot: " + got);

            // UBO 号的两端：c3 = NVN UBO 0、c18 = NVN UBO 15
            byte[] fsUboC3 = Guest(true, new[] { Ldc(4, 0, 3), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 4) });
            got = UboTextures("ubo-handle-c3", fsUboC3, "[" + UboFact(0, 3) + "]", 1u << 3);
            Check(got == "0/0/-", "c3 handle at byte 0 compiles to uboSlot 0 / uboOffset 0: " + got);
            byte[] fsUboC18 = Guest(true, new[] { Ldc(4, 0x7FFC, 18), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 4) });
            got = UboTextures("ubo-handle-c18", fsUboC18, "[" + UboFact(0x7FFC / 4, 18) + "]", 1u << 18);
            Check(got == "15/32764/-", "c18 handle at byte 0x7FFC compiles to uboSlot 15 / uboOffset 32764: " + got);

            // 同一个片元着色器两张 UBO 句柄贴图（c4 0x400、c6 0x10）：各自的来源，不串
            byte[] fsUboTwo = Guest(true, new[] {
                Ldc(4, 0x400, 4), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(8, 2, 4),
                Ldc(5, 0x10, 6), TexBindless2D(12, 2, 5),
                FaddR(0, 8, 12), FaddR(1, 9, 13), FaddR(2, 10, 14), FaddR(3, 11, 15) });
            got = UboTextures("ubo-handle-two", fsUboTwo, "[" + UboFact(256, 4) + "," + UboFact(4, 6) + "]", (1u << 4) | (1u << 6));
            Check(got == "1/1024/-;3/16/-", "two user-UBO handles keep their own uboSlot / uboOffset: " + got);

            // 缺精确事实：如实报 MISSING_FACT，坐标是「这个 UBO 句柄」（宿主按它读 UBO 补事实）；
            //   同 handle 的组合槽事实、别的 UBO 里同偏移的事实都不借
            got = UboTextures("ubo-handle-missing", fsUboB,
                "[{\"stage\":\"fs\",\"handle\":256,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}," +
                UboFact(256, 5) + "]", 1u << 4);
            Check(got.StartsWith("MISSING_FACT ", StringComparison.Ordinal) && got.EndsWith("has no source (stage=fs,handle=256,cbufSlot=4)", StringComparison.Ordinal),
                "a c4 handle without its exact fact is MISSING_FACT at (fs, 256, 4): " + got);
        }

        // C2 探针：c2 句柄表里的运行时句柄（先看现实现拒在哪一句）。
        {
            byte[] fsC2 = Guest(true, new[] { Ldc(4, 0x20, 2), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 4) });
            const string c2Fact = "[{\"stage\":\"fs\",\"handle\":8,\"cbufSlot\":2,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":true,\"separate\":false,\"arrayLength\":1}]";
            try
            {
                using JsonDocument r = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "c2-runtime-handle", vs, fsC2, textureFacts: c2Fact)));
                JsonElement c2Binding = r.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                    .First(b => b.GetProperty("kind").GetString() == "texture");
                Check(c2Binding.GetProperty("slot").GetInt32() == 8 && c2Binding.GetProperty("count").GetInt32() == 1,
                    "c2 handle-table single texture binds by handle value: " + c2Binding.GetRawText());
            }
            catch (DirectCompileException error)
            {
                Check(false, "c2 handle-table single texture binds by handle value: " + error.Code + " " + error.Message);
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

            // C1：descriptorIndexing 关闭时 profile 必须与「caps 里没有这个对象」逐字节相同；
            // 四位齐才开；缺存储位保持 false；SPIR-V 能力声明必须与 profile 一致。
            byte[] capsOff = CapsJsonWithDescriptorIndexing(enabled: false, storage: true);
            byte[] capsOn = CapsJsonWithDescriptorIndexing(enabled: true, storage: true);
            byte[] capsNoStorage = CapsJsonWithDescriptorIndexing(enabled: true, storage: false);
            byte[] profileOff = DirectProfileComposer.Compose(capsOff, closure);
            Check(composed.SequenceEqual(profileOff),
                "all-false descriptor-indexing bits compose the same profile as their absence");
            Check(profileOff.SequenceEqual(DirectProfileComposer.Compose(capsOff, closure)),
                "descriptor-indexing caps compose deterministically");
            byte[] profileOn = DirectProfileComposer.Compose(capsOn, closure);
            Check(!composed.SequenceEqual(profileOn), "enabling non-uniform indexing changes the profile");
            using (JsonDocument onDoc = JsonDocument.Parse(profileOn))
                Check(onDoc.RootElement.GetProperty("capabilities").GetProperty("supportsShaderNonUniformIndexing").GetBoolean(),
                    "four descriptor-indexing bits enable non-uniform indexing");
            using (JsonDocument noStoreDoc = JsonDocument.Parse(DirectProfileComposer.Compose(capsNoStorage, closure)))
                Check(!noStoreDoc.RootElement.GetProperty("capabilities").GetProperty("supportsShaderNonUniformIndexing").GetBoolean(),
                    "a missing storage non-uniform bit keeps non-uniform indexing disabled");
            string buildClosure = DirectBuildIdentity.TranslatorClosure;
            byte[] buildProfileAbsent = DirectProfileComposer.Compose(CapsJson(), buildClosure);
            byte[] buildProfileOff = DirectProfileComposer.Compose(CapsJsonWithDescriptorIndexing(false, true), buildClosure);
            Check(buildProfileAbsent.SequenceEqual(buildProfileOff),
                "descriptor-indexing bits off compose the same build profile");
            byte[] requestAbsent = RequestJson(buildProfileAbsent, "graphics", 1, 1, "dx-off", vs, fs);
            byte[] requestOff = RequestJson(buildProfileOff, "graphics", 1, 1, "dx-off", vs, fs);
            Check(DirectShaderCompiler.Compile(requestAbsent).SequenceEqual(DirectShaderCompiler.Compile(requestOff)),
                "profile without descriptor indexing compiles byte-identical to the pre-C1 caps");
            using (JsonDocument offDoc = JsonDocument.Parse(profileOff))
            using (JsonDocument onDoc2 = JsonDocument.Parse(profileOn))
            {
                DirectProfile profOff = DirectProfile.Parse(offDoc.RootElement, "caps-off");
                DirectProfile profOn = DirectProfile.Parse(onDoc2.RootElement, "caps-on");
                Throws("SPIRV_INVALID", () => DirectShaderCompiler.ValidateDescriptorIndexing("fs", SpvDescriptorIndexing(true, true, true, true), profOff),
                    "descriptor indexing with profile disabled is rejected");
                DirectShaderCompiler.ValidateDescriptorIndexing("fs", SpvDescriptorIndexing(false, false, false, false), profOff);
                DirectShaderCompiler.ValidateDescriptorIndexing("fs", SpvDescriptorIndexing(true, true, true, true), profOn);
                Throws("SPIRV_INVALID", () => DirectShaderCompiler.ValidateDescriptorIndexing("fs", SpvDescriptorIndexing(true, true, false, true), profOn),
                    "incomplete descriptor indexing declaration with profile enabled is rejected");
                Throws("SPIRV_INVALID", () => DirectShaderCompiler.ValidateDescriptorIndexing("fs", SpvDescriptorIndexing(true, true, true, false), profOn),
                    "missing storage capability with profile enabled is rejected");
            }
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
                // 几何阶段 SPIR-V 的执行模式与发顶点数（OpExecutionMode = 16、OpEmitVertex = 218）
                static (HashSet<uint> modes, int emits) GsSpirv(byte[] result)
                {
                    using JsonDocument doc = JsonDocument.Parse(result);
                    JsonElement st = doc.RootElement.GetProperty("content").GetProperty("stages");
                    var modes = new HashSet<uint>(); int emits = 0;
                    foreach (JsonElement s in st.EnumerateArray())
                    {
                        if (s.GetProperty("stage").GetString() != "gs") continue;
                        uint[] w = MemoryMarshal.Cast<byte, uint>(s.GetProperty("spirv").GetBytesFromBase64()).ToArray();
                        for (int i = 5; i < w.Length;)
                        {
                            int n = (int)(w[i] >> 16); if (n == 0 || i + n > w.Length) throw new InvalidDataException("invalid SPIR-V instruction");
                            if ((w[i] & 0xffff) == 16 && n >= 3) modes.Add(w[i + 2]);
                            if ((w[i] & 0xffff) == 218) emits++;
                            i += n;
                        }
                    }
                    return (modes, emits);
                }
                // 邻接三角形：编得出，GS 的输入执行模式是 InputTrianglesAdjacency（23），不是普通三角形（22）
                var adjacent = requestNode.DeepClone();
                adjacent["program"]["graphicsState"]["topology"] = "TrianglesAdjacency";
                try
                {
                    var (am, _) = GsSpirv(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(adjacent.ToJsonString())));
                    Check(am.Contains(23) && !am.Contains(22), "GS adjacency compiles with the InputTrianglesAdjacency execution mode");
                }
                catch (DirectCompileException ex) { Console.Error.WriteLine("GS adjacency rejected: " + ex.Code + " " + ex.Message); Check(false, "GS adjacency compiles with the InputTrianglesAdjacency execution mode"); }
                var oldVersion = requestNode.DeepClone();
                oldVersion["schema"] = DirectShaderCompiler.RequestSchema;
                Throws("UNSUPPORTED_PROGRAM", () => DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(oldVersion.ToJsonString())),
                    "v1 does not silently accept geometry");
                // 直通型几何头：profile 不支持 NV passthrough（同安卓设备），翻译器生成等价的普通 GS——编得出、三段齐、GS 里有发顶点
                var passthrough = requestNode.DeepClone();
                byte[] pt = GeometryGuest(); pt[3] |= 1;
                passthrough["program"]["stages"][2]["code"] = Convert.ToBase64String(pt);
                try
                {
                    byte[] pr = DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(passthrough.ToJsonString()));
                    var (_, pe) = GsSpirv(pr);
                    Check(pe >= 1, "GS passthrough header compiles to an emulated GS that emits vertices");
                }
                catch (DirectCompileException ex) { Console.Error.WriteLine("GS passthrough rejected: " + ex.Code + " " + ex.Message); Check(false, "GS passthrough header compiles to an emulated GS that emits vertices"); }
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
                    Check(gsOut.Count == 1 && gsOut.ContainsKey(0),
                        "native GS output follows actual FS inputs instead of all 32 independent-stage varyings");
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

        // 2b) 图形阶段的 c0Reads：回放按它决定间接绘制要不要把 GPU 侧的 base instance 拷进 c0 + 0x644。
        //     真实翻译器（DrawParametersReplace）：cbdp 真时 VS 对 c0 + 0x644 / 0x640 的立即读换成内建，读集里不再有；
        //     cbdp 假时照实列出；寄存器下标读 c0 分析不出 → null（未知，回放照旧拷）。
        {
            JsonElement[] C0Stages(ulong[] vsOps, bool cbdp, out string error)
            {
                error = null;
                var node = System.Text.Json.Nodes.JsonNode.Parse(graphicsRequest);
                node["program"]["stages"][0]["code"] = Convert.ToBase64String(Guest(false, vsOps));
                node["program"]["stages"][0]["constantBufferUse"] = 1;
                node["program"]["graphicsState"]["hasConstantBufferDrawParameters"] = cbdp;
                try
                {
                    using JsonDocument d = ParseResult(DirectShaderCompiler.Compile(Encoding.UTF8.GetBytes(node.ToJsonString())));
                    return d.RootElement.GetProperty("content").GetProperty("stages").EnumerateArray().Select(x => x.Clone()).ToArray();
                }
                catch (Exception ex) { error = ex.Message; return null; }
            }
            JsonValueKind Kind(JsonElement stage) => stage.TryGetProperty("c0Reads", out var r) ? r.ValueKind : JsonValueKind.Undefined;
            bool Covers(JsonElement stage, int offset) => stage.TryGetProperty("c0Reads", out var reads) && reads.ValueKind == JsonValueKind.Array &&
                reads.EnumerateArray().Any(r => r[0].GetInt32() < offset + 4 && r[0].GetInt32() + r[1].GetInt32() > offset);
            string Show(JsonElement[] st, string err) => err ?? string.Join(" / ", st.Select(x => x.GetProperty("stage").GetString() + "=" +
                (x.TryGetProperty("c0Reads", out var r) ? r.GetRawText() : "(missing)")));
            JsonElement[] binstOff = C0Stages(new[] { Ldc(0, 0x644), Ldc(1, 0x610), FaddR(0, 0, 1), Ast(0) }, false, out string e1);
            Check(binstOff != null && Covers(binstOff[0], 0x644) && Covers(binstOff[0], 0x610),
                "cbdp false: VS c0Reads lists the base-instance word 0x644 and 0x610: " + Show(binstOff, e1));
            JsonElement[] binstOn = C0Stages(new[] { Ldc(0, 0x644), Ldc(1, 0x610), FaddR(0, 0, 1), Ast(0) }, true, out string e2);
            Check(binstOn != null && Kind(binstOn[0]) == JsonValueKind.Array && !Covers(binstOn[0], 0x644) && Covers(binstOn[0], 0x610),
                "cbdp true: VS base-instance read becomes the builtin, c0Reads keeps 0x610 and drops 0x644: " + Show(binstOn, e2));
            JsonElement[] dynamic = C0Stages(new[] { Ald(0, 0, 1), Ldc(0, 0x644, 0, 1), Ast(0) }, true, out string e3);   // 下标来自顶点属性：翻译器折不成常量
            Check(dynamic != null && Kind(dynamic[0]) == JsonValueKind.Null,
                "register-indexed c0 read is unknown (null), never an empty read set: " + Show(dynamic, e3));
            // 下标先 & 0x3f0：真实翻译器的形状（(R1 + imm) >> 2 >> 2、& 3）按取值界证得读 data[lo..hi]
            JsonElement[] masked = C0Stages(new[] { Ald(0, 0, 1), Lop32iAnd(1, 1, 0x3F0), Ldc(0, 0, 0, 1), Ast(0) }, true, out string e4);
            Check(masked != null && Kind(masked[0]) == JsonValueKind.Array && Covers(masked[0], 0x3F0) && !Covers(masked[0], 0x644),
                "masked register index: c0Reads is data[0..63], proven not to read 0x644: " + Show(masked, e4));
            Check(masked != null && masked[0].TryGetProperty("c0ReadsStatic", out var maskedStatic) && maskedStatic.ValueKind == JsonValueKind.Null,
                "bounded dynamic has separate unknown static read set for switch off");
            Check(binstOn != null && binstOn[0].TryGetProperty("c0ReadsStatic", out var staticReads) && staticReads.ValueKind == JsonValueKind.Array &&
                  staticReads.EnumerateArray().Any(r => r[0].GetInt32() <= 0x610 && r[0].GetInt32() + r[1].GetInt32() > 0x610),
                "constant c0 reads retain their static proof when dynamic bounds are off");
            JsonElement[] maskedReach = C0Stages(new[] { Ald(0, 0, 1), Lop32iAnd(1, 1, 0x3F0), Ldc(0, 0x600, 0, 1), Ast(0) }, true, out string e5);
            Check(maskedReach != null && Covers(maskedReach[0], 0x644),
                "negative control: masked register index that can reach 0x644 is a read: " + Show(maskedReach, e5));
            Check(binstOn != null && binstOn.All(x => x.TryGetProperty("c0Reads", out _)), "every graphics stage carries c0Reads");
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
        // 按位读（SULD.D）浮点贴图的存储图像：设备支持无格式读时声明 Unknown + uint（回放按贴图字节数绑 uint 视图），
        //   不支持时保留按读大小的 R32ui（回放按声明格式能建就建、建不了报名跳过）。读的是 1x1 RG32F 曝光那一类（空轨 2nd update 核同形）。
        {
            const string rawFact = "[{\"stage\":\"cs\",\"handle\":74,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R32G32Float\",\"isBuffer\":false,\"normalized\":false,\"separate\":false,\"arrayLength\":1}]";
            string on = Encoding.UTF8.GetString(profile), off = on.Replace("\"supportsImageLoadFormatted\":true", "\"supportsImageLoadFormatted\":false");
            Check(on != off, "profile carries supportsImageLoadFormatted");
            foreach (bool formatless in new[] { true, false })
            {
                byte[] req = RequestJson(Encoding.UTF8.GetBytes(formatless ? on : off), "compute", 2, 1, "suld-raw", cs: ComputeRawImageLoadGuest(), csCbUse: 1, textureFacts: rawFact);
                using JsonDocument doc = ParseResult(DirectShaderCompiler.Compile(req));
                (uint format, bool uintSampled, int images) = StorageImageDecl(doc.RootElement.GetProperty("content").GetProperty("stages")[0]);
                Check(images == 1 && uintSampled && format == (formatless ? 0u : 33u),
                    $"raw SULD.D of an RG32F image: {(formatless ? "Unknown" : "R32ui")} with uint sampled type (got format {format}, uint {uintSampled}, images {images})");
            }
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

        // 5b) 共享内存超过设备上限（profile 32 KB）：最大的线程组数组挪到存储缓冲（DirectSharedSpill，同 iOS SpillFix）。
        //     真实 decoder / translator：STS 写到 0x9000（超出 32 KB）、BAR.SYNC、LDS 读回、再 STS 留住结果。
        {
            byte[] sharedCs = ComputeOps(Fadd32i(0, 1.0f), Sts(0, 0x9000), BarSync, Lds(1, 0x9000), Sts(1, 0x10));
            (JsonElement content, string error) SharedCompile(uint bytes)
            {
                try
                {
                    using JsonDocument d = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "compute", 2, 1, "shared-" + bytes, cs: sharedCs, sharedBytes: bytes)));
                    return (d.RootElement.GetProperty("content").Clone(), null);
                }
                catch (Exception ex) { return (default, ex.Message); }
            }
            uint[] Words(JsonElement content) => MemoryMarshal.Cast<byte, uint>(content.GetProperty("stages")[0].GetProperty("spirv").GetBytesFromBase64()).ToArray();
            IEnumerable<uint[]> Insts(uint[] w) { for (int i = 5; i < w.Length; i += (int)(w[i] >> 16)) yield return w[i..(i + (int)(w[i] >> 16))]; }
            bool HasWorkgroupVar(uint[] w) => Insts(w).Any(x => (x[0] & 0xFFFF) == 59 && x[3] == 4);
            var (big, bigErr) = SharedCompile(40960);
            if (bigErr != null) Check(false, "40 KB shared compute compiles with a spill: " + bigErr);
            else
            {
                uint[] w = Words(big);
                JsonElement[] spills = big.GetProperty("bindings").EnumerateArray().Where(b => b.GetProperty("kind").GetString() == "spill").ToArray();
                Check(spills.Length == 1 && spills[0].GetProperty("set").GetInt32() == 1 && spills[0].GetProperty("bytesPerGroup").GetInt32() == 40960 &&
                      spills[0].GetProperty("stage").GetString() == "cs", "40 KB shared: one spill binding in set 1, 40960 bytes per group");
                Check(!HasWorkgroupVar(w), "40 KB shared: no workgroup variable left in the SPIR-V");
                var consts = Insts(w).Where(x => (x[0] & 0xFFFF) == 43 && x.Length == 4).ToDictionary(x => x[2], x => x[3]);
                var builtins = Insts(w).Where(x => (x[0] & 0xFFFF) == 71 && x.Length == 4 && x[2] == 11).Select(x => x[3]).ToHashSet();
                Check(builtins.Contains(26) && builtins.Contains(24), "spill index uses WorkgroupId and NumWorkgroups");
                // 溢出缓冲的成员带 Coherent（组内调用经 barrier 互相看得见写，同 Workgroup 内存的语义）
                Check(Insts(w).Any(x => (x[0] & 0xFFFF) == 72 && x.Length == 4 && x[2] == 0 && x[3] == 23),
                      "spill buffer member is Coherent so writes are visible across the workgroup after barriers");
                // 每个指向溢出缓冲的访问链：最后一个下标 = OpIAdd(OpIMul(组号, 10240 字), 原下标)
                var defs = new Dictionary<uint, uint[]>();
                foreach (uint[] x in Insts(w)) if ((x[0] & 0xFFFF) is 128 or 132 && x.Length == 5 || (x[0] & 0xFFFF) == 169 && x.Length == 6) defs[x[2]] = x;
                bool Decorated(uint id, uint dec, int value) => Insts(w).Any(x => (x[0] & 0xFFFF) == 71 && x.Length == 4 && x[1] == id && x[2] == dec && x[3] == (uint)value);
                uint spillVar = Insts(w).Where(v => (v[0] & 0xFFFF) == 59 && v[3] == 2).Select(v => v[2]).FirstOrDefault(id =>
                    Decorated(id, 34, spills[0].GetProperty("set").GetInt32()) && Decorated(id, 33, spills[0].GetProperty("binding").GetInt32()));
                uint[][] spillChains = Insts(w).Where(x => (x[0] & 0xFFFF) is 65 or 66 && x.Length >= 5 && x[3] == spillVar).ToArray();
                bool Scaled(uint id) => defs.TryGetValue(id, out var add) && (add[0] & 0xFFFF) == 128 &&
                    new[] { add[3], add[4] }.Any(o => defs.TryGetValue(o, out var mul) && (mul[0] & 0xFFFF) == 132 &&
                        (consts.GetValueOrDefault(mul[3]) == 10240 || consts.GetValueOrDefault(mul[4]) == 10240));
                Check(spillVar != 0 && spillChains.Length >= 3 && spillChains.All(x => Scaled(x[^1])),
                      "every spill access indexes slice (flat group id x 10240 words) + original index: chains " + spillChains.Length);
                // 下标夹在本组片内：加到基址上的那一项 = OpSelect(idx < 10240, idx, 10239)
                bool Clamped(uint id) => defs.TryGetValue(id, out var add) && (add[0] & 0xFFFF) == 128 &&
                    new[] { add[3], add[4] }.Any(o => defs.TryGetValue(o, out var sel) && (sel[0] & 0xFFFF) == 169 && consts.GetValueOrDefault(sel[5]) == 10239);
                Check(spillChains.All(x => Clamped(x[^1])), "every spill index is clamped to the group's slice (select idx < 10240 ? idx : 10239)");
                // 组号基址每个函数算一次：读 WorkgroupId 的次数不随访问次数涨（3 处访问、1 个函数 → 1 次）
                uint gidVar = Insts(w).Where(x => (x[0] & 0xFFFF) == 71 && x.Length == 4 && x[2] == 11 && x[3] == 26).Select(x => x[1]).First();
                int gidLoads = Insts(w).Count(x => (x[0] & 0xFFFF) == 61 && x.Length >= 4 && x[3] == gidVar);
                Check(gidLoads == 1, "the group base is computed once per function, not per access: WorkgroupId loads " + gidLoads);
                var barriers = Insts(w).Where(x => (x[0] & 0xFFFF) == 224).ToArray();
                Check(barriers.Length > 0 && barriers.All(x => (consts.GetValueOrDefault(x[3]) & 0x140) == 0x140),
                      "workgroup barriers also order uniform (spill buffer) memory");
                string spvPath = Path.Combine(Path.GetTempPath(), "s2a-spill-" + Environment.ProcessId + ".spv");
                File.WriteAllBytes(spvPath, big.GetProperty("stages")[0].GetProperty("spirv").GetBytesFromBase64());
                string val = Environment.GetEnvironmentVariable("SPIRV_VAL") ?? "spirv-val";
                try
                {
                    using var proc = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(val, "--target-env vulkan1.1 \"" + spvPath + "\"")
                        { RedirectStandardError = true, RedirectStandardOutput = true });
                    string msg = proc.StandardError.ReadToEnd() + proc.StandardOutput.ReadToEnd(); proc.WaitForExit();
                    Check(proc.ExitCode == 0, "spilled SPIR-V passes spirv-val (vulkan1.1): " + msg.Trim());
                }
                catch (System.ComponentModel.Win32Exception) { Console.WriteLine("NOTE spirv-val not found: spilled SPIR-V not validated by SPIRV-Tools (set SPIRV_VAL)"); }
                finally { File.Delete(spvPath); }
            }
            var (small, smallErr) = SharedCompile(16384);
            Check(smallErr == null && !small.GetProperty("bindings").EnumerateArray().Any(b => b.GetProperty("kind").GetString() == "spill") && HasWorkgroupVar(Words(small)),
                  "16 KB shared stays in workgroup memory without a spill binding: " + smallErr);
            byte[] noStorageProfile = Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(profile).Replace("\"maximumStorageBuffersPerStage\":16", "\"maximumStorageBuffersPerStage\":0"));
            try {
                DirectShaderCompiler.Compile(RequestJson(noStorageProfile, "compute", 2, 1, "spill-no-storage", cs: sharedCs, sharedBytes: 40960));
                Check(false, "spill without storage descriptors must fail");
            } catch (DirectCompileException error) {
                Check(error.Code == "RESOURCE_LIMIT" && error.Message.Contains("sharedMemoryBytes 40960") && error.Message.Contains("limit 32768"),
                      "failed spill retains reason and reports shared request / device limit: " + error.Code + " " + error.Message);
            }
            var (maxGuest, maxGuestErr) = SharedCompile(48 * 1024);
            Check(maxGuestErr == null && maxGuest.GetProperty("bindings").EnumerateArray().Any(b => b.GetProperty("kind").GetString() == "spill"),
                  "48 KiB guest shared memory is accepted and spilled: " + maxGuestErr);
            var (_, overGuestErr) = SharedCompile(48 * 1024 + 1);
            Check(overGuestErr != null, "guest shared memory over 48 KiB is rejected");
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
        // 非常量下标按取值界（boundDynamic，只给图形阶段）：证得的 vec4 整段记为读；证不了照旧 null。
        {
            string Dyn(int[][] r) => r == null ? "null" : "[" + string.Join(",", r.Select(x => "[0x" + x[0].ToString("x") + "," + x[1] + "]")) + "]";
            bool Hits(int[][] r, int off) => r != null && r.Any(x => x[0] < off + 4 && x[0] + x[1] > off);
            int[][] D(string v, uint mask = 0x3F0, uint imm = 0, bool bound = true) => DirectComputeC0Reads.Analyze(C0DynFixture(v, mask, imm), 0, 1, bound);
            int[][] and = D("and");
            Check(and != null && and.Length == 1 && and[0].SequenceEqual(new[] { 0, 0x400 }) && !Hits(and, 0x644),
                "masked dynamic index (x & 0x3f0) >> 4 is data[0..63]: proven not to read 0x644: " + Dyn(and));
            int[][] reach = D("and", imm: 0x600);
            Check(reach != null && reach[0].SequenceEqual(new[] { 0x600, 0x400 }) && Hits(reach, 0x644),
                "negative control: dynamic index that can reach data[100] reads 0x644: " + Dyn(reach));
            int[][] above = D("and", imm: 0x650);
            Check(above != null && above[0].SequenceEqual(new[] { 0x650, 0x400 }) && !Hits(above, 0x644),
                "lower bound counts too: data[101..164] does not read 0x644: " + Dyn(above));
            Check(D("unmasked") == null, "unmasked register index stays unknown: " + Dyn(D("unmasked")));
            Check(D("and", mask: 0xFFFF0) == null, "upper bound past the array length stays unknown: " + Dyn(D("and", mask: 0xFFFF0)));
            int[][] local = D("local");
            Check(local != null && local[0].SequenceEqual(new[] { 0, 0x400 }), "bounded value through a function local: " + Dyn(local));
            Check(D("local-escape") == null, "local whose pointer escapes stays unknown: " + Dyn(D("local-escape")));
            Check(D("local-two") == null, "local that also stores an unbounded value stays unknown: " + Dyn(D("local-two")));
            Check(D("loop") == null, "loop phi index stays unknown: " + Dyn(D("loop")));
            int[][] clamp = D("sclamp");
            Check(clamp != null && clamp[0].SequenceEqual(new[] { 0, 63 * 16 + 4 }), "sclamp(x, 0, 63) bounds the index (component 0 of data[0..63]): " + Dyn(clamp));
            Check(D("smin") == null, "signed min(x, 63) with possibly negative x stays unknown: " + Dyn(D("smin")));
            Check(D("and", bound: false) == null, "compute (no boundDynamic) keeps dynamic index unknown");
            Check(DirectComputeC0Reads.Analyze(C0ReadFixture(0x200, dynamic: true), 0, 1, true) == null,
                "undefined index id stays unknown with boundDynamic");
            Check(DirectComputeC0Reads.Analyze(C0ReadFixture(0x644), 0, 1, true)?[0].SequenceEqual(new[] { 0x644, 4 }) == true,
                "constant index unchanged with boundDynamic (real 0x644 read is a read)");
        }
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
                + "{\"stage\":\"fs\",\"handle\":3,\"cbufSlot\":-1,\"samplerType\":\"3d\",\"format\":\"R11G11B10Float\",\"isBuffer\":false,\"normalized\":false,\"separate\":true,\"arrayLength\":1},"
                + "{\"stage\":\"fs\",\"handle\":72,\"cbufSlot\":-1,\"samplerType\":\"2d\",\"format\":\"R8G8B8A8Unorm\",\"isBuffer\":false,\"normalized\":false,\"separate\":false,\"arrayLength\":1}"
                + "]";
            byte[] matrixRequest = RequestJson(profile, "graphics", 1, 1, "tex-matrix", vs, fs, textureFacts: facts,
                textureBufferLengths: "{\"0\":8}");
            using JsonDocument doc = JsonDocument.Parse(matrixRequest);
            DirectProgram program = DirectProgram.Parse(doc.RootElement.GetProperty("program"), "program", matrixProfile);
            var matrixAccessor = new DirectGpuAccessor(matrixProfile, program, program.StageOf("fs"), 5,
                new DirectAllocator(matrixProfile), new List<DirectQuery>());

            Check(matrixAccessor.QuerySamplerType(0, -1) == SamplerType.Texture2D, "2d sampler type");
            Check(matrixAccessor.QueryTextureFormat(0, -1) == TextureFormat.R8G8B8A8Unorm, "2d format");
            Check(matrixAccessor.QueryS2ASampledTextureFormat(0, -1) == TextureFormat.R8G8B8A8Unorm, "sampled format uses exact fact");
            Check(matrixAccessor.QueryTextureCoordNormalized(0, -1) && !matrixAccessor.QueryTextureIsBuffer(0, -1),
                "2d normalized non-buffer");

            Check(matrixAccessor.QuerySamplerType(1, 0) == (SamplerType.Texture2D | SamplerType.Array), "2d-array sampler type");
            Check(matrixAccessor.QueryTextureFormat(1, 0) == TextureFormat.R16G16B16A16Float, "2d-array format");
            Check(!matrixAccessor.QueryTextureCoordNormalized(1, 0), "float array is not normalized");

            Check(matrixAccessor.QuerySamplerType(1, 4) == (SamplerType.TextureCube | SamplerType.Shadow),
                "same handle keeps its own cbufSlot facts");
            Check(matrixAccessor.QueryTextureFormat(1, 4) == TextureFormat.R32Float, "per-slot format");
            Check(matrixAccessor.QueryS2ASampledTextureFormat(1, 4) == TextureFormat.R32Float, "sampled format preserves cbuf slot");

            Check(matrixAccessor.QuerySamplerType(2, -1) == SamplerType.TextureBuffer && matrixAccessor.QueryTextureIsBuffer(2, -1),
                "buffer texture");
            Check(matrixAccessor.QueryTextureFormat(2, -1) == TextureFormat.R32Uint, "integer buffer format");
            Check(matrixAccessor.QuerySamplerType(3, -1) == SamplerType.Texture3D, "3d sampler type");

            // 经 c2 句柄表的查询没有精确事实时，只能回落到同句柄的直接事实（NVN c2 句柄→组合槽）；
            // 别的常量缓冲（游戏 UBO、c0）同一字偏移不借（#999）；没有同句柄直接事实、或未列出的 handle 仍是缺事实；事实不跨阶段。
            Check(matrixAccessor.QuerySamplerType(0, 2) == SamplerType.Texture2D &&
                  matrixAccessor.QueryTextureCoordNormalized(0, 2),
                "c2 query falls back to the same handle's direct fact");
            Throws("MISSING_FACT", () => matrixAccessor.QuerySamplerType(0, 4), "a user-UBO handle does not borrow the combined slot's fact");
            Throws("MISSING_FACT", () => matrixAccessor.QuerySamplerType(0, 3), "c3 (NVN UBO 0) does not borrow either");
            Check(matrixAccessor.HasDirectTextureFact(0, 2) && !matrixAccessor.HasDirectTextureFact(1, 2),
                "only directly bound handles have a fallback");
            Check(!matrixAccessor.HasDirectTextureFact(0, 4) && !matrixAccessor.HasDirectTextureFact(0, 0),
                "the fallback is only for the c2 handle table");
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
            ShaderProgram CbufProgram(int handleIndex, int cbufSlot = 2) => new(
                new ShaderProgramInfo(Array.Empty<BufferDescriptor>(), Array.Empty<BufferDescriptor>(),
                    new[] { new TextureDescriptor(2, 0, SamplerType.Texture2D, TextureFormat.R8G8B8A8Unorm,
                        cbufSlot: cbufSlot, handleIndex: handleIndex, arrayLength: 1, separate: false, TextureUsageFlags.None) },
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
            // 游戏 UBO（c4）里的同一字偏移：不是组合槽，有同句柄直接事实也拒（#999 错绑）
            var uboAllocator = new DirectAllocator(matrixProfile);
            uboAllocator.Bindings.Add(new DirectBinding("fs", "texture", "test", 2, 0, 1));
            bool uboRejected = false;
            try { resolvePoolSlots.Invoke(null, new object[] { new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>
                { (program.StageOf("fs"), CbufProgram(0, 4), matrixAccessor) }, uboAllocator }); }
            catch (System.Reflection.TargetInvocationException error)
            {
                uboRejected = error.InnerException is DirectCompileException compile && compile.Code == "UNSUPPORTED_PROGRAM" && compile.Message.Contains("cbuf slot 4");
            }
            Check(!uboRejected && uboAllocator.Bindings.Last().Slot == -1 && uboAllocator.Bindings.Last().UboSlot == 1 &&
                  uboAllocator.Bindings.Last().UboOffset == 0,
                "a c4 handle is not bound as the combined slot with the same handle index (it is uboSlot 1 / uboOffset 0)");
            // 游戏 UBO 句柄只接单个、非分离的采样贴图；其余形态如实报未接，不给 uboSlot
            string UboShape(string kind, string source, int count, int cbufSlot, int handleIndex, int arrayLength, bool separate, string requestKey = null)
            {
                var allocator = new DirectAllocator(matrixProfile) { RequestKey = requestKey };
                var b = new DirectBinding("fs", kind, source, kind == "image" ? 3 : 2, 0, count);
                allocator.Bindings.Add(b);
                var descriptor = new TextureDescriptor(kind == "image" ? 3 : 2, 0, source == "buffer-texture" ? SamplerType.TextureBuffer : SamplerType.Texture2D,
                    TextureFormat.R8G8B8A8Unorm, cbufSlot: cbufSlot, handleIndex: handleIndex, arrayLength: arrayLength, separate: separate, TextureUsageFlags.None);
                ShaderProgram shaderProgram = new(
                    new ShaderProgramInfo(Array.Empty<BufferDescriptor>(), Array.Empty<BufferDescriptor>(),
                        kind == "image" ? Array.Empty<TextureDescriptor>() : new[] { descriptor },
                        kind == "image" ? new[] { descriptor } : Array.Empty<TextureDescriptor>(), ShaderStage.Fragment, 0, 0, 0,
                        usesFragCoord: false, usesInstanceId: false, usesDrawParameters: false, usesRtLayer: false,
                        clipDistancesWritten: 0, fragmentOutputMap: 0),
                    Ryujinx.Graphics.Shader.Translation.TargetLanguage.Spirv, Array.Empty<byte>());
                try
                {
                    resolvePoolSlots.Invoke(null, new object[] { new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>
                        { (program.StageOf("fs"), shaderProgram, matrixAccessor) }, allocator });
                    return "bound " + b.UboSlot + "/" + b.UboOffset + "/" + b.Slot;
                }
                catch (System.Reflection.TargetInvocationException error) when (error.InnerException is DirectCompileException compile)
                {
                    return compile.Code + " " + compile.Message;
                }
            }
            Check(UboShape("texture", "texture", 1, 18, 0x3FFF, 1, false) == "bound 15/65532/-1", "c18 last word of a 64 KiB UBO is bound");
            // 被拒消息尾巴（#999）：「 [kind=<绑定种类> handle=<HandleIndex 原值> word=<字偏移> count=<绑定个数> array=<数组长度> key=<程序键，没有写 ->]」；
            //   用例不给程序键，所以是 key=-；期望值手算：普通句柄 word = handle；count / array 就是 UboShape 传进去的两个数
            foreach (var (what, shape, tail, outcome) in new[] {
                ("image", "image", "[kind=image handle=256 word=256 count=1 array=1 key=-]", UboShape("image", "image", 1, 4, 256, 1, false)),
                ("buffer texture", "buffer texture", "[kind=texture handle=256 word=256 count=1 array=1 key=-]", UboShape("texture", "buffer-texture", 1, 4, 256, 1, false)),
                ("separate", "separate", "[kind=texture handle=256 word=256 count=1 array=1 key=-]", UboShape("texture", "texture", 1, 4, 256, 1, true)),
                ("array", "array", "[kind=texture handle=0 word=0 count=4 array=4 key=-]", UboShape("texture", "texture", 4, 4, 0, 4, false)),
                ("offset past 64 KiB", "offset outside 64 KiB", "[kind=texture handle=16384 word=16384 count=1 array=1 key=-]", UboShape("texture", "texture", 1, 4, 0x4000, 1, false)) })
                Check(outcome == "UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 4 is not implemented (" + shape + ") " + tail,
                    "user-UBO " + what + " handle stays unsupported: " + outcome);
            // 图像句柄从 c2 图像句柄表读（字 72 + 2k，空轨 2nd 9000 计算「cbuf slot 2」）：有同字直接事实（宿主按图像槽出）→ 绑同号图像槽；
            //   没有事实的槽、表外的字、数组照旧报未接
            string c2Img = UboShape("image", "image", 1, 2, 72, 1, false);
            Check(c2Img == "bound -1/-1/72", "c2 image-table word 72 with its direct fact binds image handle 72: " + c2Img);
            foreach (var (what, handle, count, len) in new[] { ("without a fact", 74, 1, 1), ("outside the image table", 88, 1, 1), ("as an array", 72, 4, 4) })
            {
                string r = UboShape("image", "image", count, 2, handle, len, false);
                // c2 / 打包槽那一处：#1488 的基础消息已带 handle= / arrayLength= / count= / request=<sha>（sha 是本进程里最后一次 Compile 的，这里不比），
                //   现场只补 kind / word（没给程序键，所以 key 是 -）
                Check(r.StartsWith("UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 2 is not implemented (handle=" + handle +
                                   ", arrayLength=" + len + ", count=" + count + ", request=") &&
                      r.EndsWith(") [kind=image word=" + handle + " key=-]"),
                    "c2 image word " + what + " stays unsupported: " + r);
            }
            string c19 = UboShape("texture", "texture", 1, 19, 256, 1, false);
            Check(c19.StartsWith("UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 19 is not implemented (handle=256, arrayLength=1, count=1, request=") &&
                  c19.EndsWith(") [kind=texture word=256 key=-]"),
                "cbuf 19 is outside the user UBO range: " + c19);
            // #999 被拒现场：字段按重要性排序（kind、handle、word…、cbuf、count、array、key），放不下从最后一项起整项丢；
            //   程序键原样带出（≤ 48 个字符，超出的截掉）；分离 / 带类型的句柄按 UnpackOffsets 拆字偏移与类型，槽号带高 16 位时拆两个常量缓冲号；
            //   宿主把整段错误 JSON 抄进 reason[320]（≤ 319 个字符）、截断了归成 unclassified；JSON 信封占 74 个字符，所以消息整条 ≤ 245。期望值手算：
            //   809517146 = 0x3040405A → 低 14 位 0x5A = 90（贴图字）、14..27 位 0x101 = 257（采样器字）、高 4 位 3（SeparateConstantSamplerHandle）；
            //   65538 = 0x10002 → 贴图在 c2、采样器在 c(高 16 位 1 − 1) = c0。
            const string Hex48 = "0123456789abcdef0123456789abcdef0123456789abcdef";
            //   用户 UBO 那一处（#1488 没有补过）：基础消息短，什么都放得下，键放最后
            string withKey = UboShape("texture", "texture", 1, 4, 0x4000, 1, false, Hex48);
            Check(withKey == "UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 4 is not implemented (offset outside 64 KiB) " +
                             "[kind=texture handle=16384 word=16384 count=1 array=1 key=" + Hex48 + "]",
                "user-UBO reject message carries the 48-char program key verbatim, last: " + withKey);
            string longKey = UboShape("texture", "texture", 1, 4, 0x4000, 1, false, Hex48 + "ZZZZZZZZ" + new string('Y', 71));
            Check(longKey == withKey && !longKey.Contains("Z"), "program key longer than 48 chars is cut to its first 48: " + longKey);
            Check(UboShape("texture", "texture", 1, 4, 0x4000, 1, false, "") == UboShape("texture", "texture", 1, 4, 0x4000, 1, false),
                "an empty program key is written as '-', same as no key");
            //   c2 / 打包槽那一处：基础消息（#1488，含 64 个字符的请求 sha）典型已有 ~176 个字符，现场尽量放，放不下的丢
            string packed = UboShape("texture", "texture", 1, 65538, 809517146, 1, false, Hex48);
            int shaAt = packed.IndexOf("request=", StringComparison.Ordinal);
            Check(shaAt > 0 && packed.IndexOf(')', shaAt) - (shaAt + "request=".Length) == 64,
                "the base message carries a 64-char request sha (the budget below assumes it): " + packed);
            Check(packed.StartsWith("UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65538 is not implemented (handle=809517146, arrayLength=1, count=1, request=") &&
                  packed.EndsWith(") [kind=texture word=90 word2=257 type=3 cbuf=2/0]"),
                "packed separate-constant-sampler handle is decoded into words, type and both cbufs; the 48-char key no longer fits and is dropped whole: " + packed);
            string packedShortKey = UboShape("texture", "texture", 1, 65538, 809517146, 1, false, "abc");
            Check(packedShortKey.EndsWith(") [kind=texture word=90 word2=257 type=3 cbuf=2/0 key=abc]"),
                "a key that fits is kept, after the decoded fields: " + packedShortKey);
            //   边界：这条的基础消息 175 个字符 + 现场 " [kind=texture word=90 word2=257 type=3 cbuf=2/0" 48 个 + " key=" 5 个 + 键 + "]"，键 16 个字符时正好 245；17 个字符就是 246，整项丢
            string packedEdge = UboShape("texture", "texture", 1, 65538, 809517146, 1, false, new string('e', 16));
            Check(packedEdge.Length - "UNSUPPORTED_PROGRAM ".Length == 245 && packedEdge.EndsWith(" cbuf=2/0 key=" + new string('e', 16) + "]"),
                "a tail that ends exactly at 245 characters is kept: " + packedEdge.Length + " " + packedEdge);
            string packedOver = UboShape("texture", "texture", 1, 65538, 809517146, 1, false, new string('e', 17));
            Check(packedOver.EndsWith(" cbuf=2/0]") && !packedOver.Contains("key="),
                "one character more and the key is dropped whole, not cut: " + packedOver);
            // 最坏情形：槽号 / 句柄取 int 极值、个数 / 数组长度取合同上限 1048576、键 127 个字符（两处消息出口都有）。
            //   整条 ≤ 245、前缀不动；有 key 就是完整的 48 个字符收尾（不截半项），一项都放不下就没有尾巴
            string[] worst = {
                UboShape("texture", "texture", 1048576, int.MaxValue, int.MinValue, 1048576, false, new string('K', 127)),
                UboShape("texture", "texture", 1048576, 18, int.MinValue, 1048576, false, new string('K', 127)),
                UboShape("texture", "texture", 1, 18, int.MinValue, 1, false, new string('K', 127)),
                UboShape("texture", "texture", 1, 4, int.MinValue, 1, true, new string('K', 127)),
                UboShape("image", "image", 1048576, 4, int.MaxValue, 1048576, false, new string('K', 127)),
                UboShape("texture", "texture", 1048576, 0x7FFF0002, 0x3040405A, 1048576, false, new string('K', 127)) };
            foreach (string w in worst)
                Check(w.StartsWith("UNSUPPORTED_PROGRAM ") && w.Length - "UNSUPPORTED_PROGRAM ".Length <= 245 && w.Contains(" is not implemented (") &&
                      (w.Contains(" key=") ? w.EndsWith(" key=" + new string('K', 48) + "]") : (w.EndsWith(")") || w.EndsWith("]"))),
                    "worst-case reject message stays inside the host's 245-char limit (" + (w.Length - "UNSUPPORTED_PROGRAM ".Length) + "): " + w);
            // Facts alone do not implement separate binding; the production resolver must still refuse it.
            var separateAllocator = new DirectAllocator(matrixProfile);
            separateAllocator.Bindings.Add(new DirectBinding("fs", "texture", "test", 2, 0, 1));
            ShaderProgram separateProgram = new(
                new ShaderProgramInfo(Array.Empty<BufferDescriptor>(), Array.Empty<BufferDescriptor>(),
                    new[] { new TextureDescriptor(2, 0, SamplerType.Texture2D, TextureFormat.R8G8B8A8Unorm,
                        cbufSlot: 2, handleIndex: 0, arrayLength: 1, separate: true, TextureUsageFlags.None) },
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

        // Separate NVN c2 texture/sampler words, translated by the real Maxwell compiler.
        // Both operand orders produce the same pair of reflected sources.
        {
            const string separateFact = "[{\"stage\":\"fs\",\"handle\":90,\"cbufSlot\":2,\"samplerType\":\"2d\",\"format\":\"Unknown\",\"isBuffer\":false,\"normalized\":true,\"separate\":true,\"arrayLength\":1}]";
            foreach (bool reverse in new[] { false, true })
            {
                byte[] shader = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f),
                    Ldc(6, 0x168, 2), Ldc(7, 0x568, 2), LopOr(4, reverse ? 7 : 6, reverse ? 6 : 7), TexBindless2D(0, 2, 4) });
                try {
                    using JsonDocument separateResult = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "separate-c2", vs, shader, textureFacts: separateFact, fsCbUse: 4)));
                    var root = separateResult.RootElement;
                    var binding = root.GetProperty("content").GetProperty("bindings").EnumerateArray()
                        .Single(b => b.GetProperty("kind").GetString() == "texture");
                    Check(root.GetProperty("schema").GetString() == "s2a-direct-program-v2" &&
                          binding.GetProperty("slot").GetInt32() == 90 && binding.GetProperty("samp_slot").GetInt32() == 0,
                          "real separate c2 shader emits v2 texture word 90 and sampler slot 0");
                } catch (DirectCompileException error) {
                    Check(false, "real separate c2 shader " + (reverse ? "reversed" : "forward") + " must compile: " + error.Code + " " + error.Message);
                }
            }
        }

        // 立即句柄直接读 c2 分离贴图表（空轨 2nd 9000：fs「QuerySamplerType has no source (handle=90,cbufSlot=-1)」）：
        //   TEX 的立即句柄 90 = c2 字节 0x168 = 分离贴图槽 0。宿主照真机只按 (90, cbufSlot 2, separate) 给事实；
        //   期望编出贴图绑定 slot 90、无 samp_slot（采样器取同字）。负控：只有非分离事实、相邻字、采样器表的字都不借。
        {
            string Fact(int handle, int cbufSlot, bool separate) => "{\"stage\":\"fs\",\"handle\":" + handle + ",\"cbufSlot\":" + cbufSlot +
                ",\"samplerType\":\"2d\",\"format\":\"Unknown\",\"isBuffer\":false,\"normalized\":true,\"separate\":" + (separate ? "true" : "false") + ",\"arrayLength\":1}";
            string Direct(int tid, string facts)
            {
                byte[] shader = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Tex2D(0, 2, tid) });
                try
                {
                    using JsonDocument r = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "direct-sep-" + tid, vs, shader, textureFacts: "[" + facts + "]")));
                    string Field(JsonElement b, string name) => b.TryGetProperty(name, out JsonElement f) ? f.GetInt32().ToString() : "-";
                    return string.Join(";", r.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                        .Where(b => b.GetProperty("kind").GetString() == "texture").Select(b => Field(b, "slot") + "/" + Field(b, "samp_slot")));
                }
                catch (DirectCompileException error) { return error.Code + " " + error.Message; }
            }
            string got = Direct(90, Fact(90, 2, true));
            Check(got == "90/-", "direct handle 90 (c2 separate texture slot 0) uses the (90, cbuf 2, separate) fact and binds slot 90: " + got);
            got = Direct(124, Fact(124, 2, true));
            Check(got == "124/-", "direct handle 124 (separate slot 17) binds slot 124: " + got);
            got = Direct(90, Fact(90, 2, false));
            Check(got.StartsWith("MISSING_FACT", StringComparison.Ordinal) && got.Contains("handle=90,cbufSlot=-1"), "a non-separate (90, cbuf 2) fact is not borrowed: " + got);
            got = Direct(92, Fact(90, 2, true));
            Check(got.StartsWith("MISSING_FACT", StringComparison.Ordinal) && got.Contains("handle=92,cbufSlot=-1"), "another separate slot's fact is not borrowed: " + got);
            got = Direct(346, Fact(346, 2, true));
            Check(got.StartsWith("MISSING_FACT", StringComparison.Ordinal) && got.Contains("handle=346,cbufSlot=-1"), "a sampler-table word is not a separate texture: " + got);
            got = Direct(8, Fact(8, -1, false));
            Check(got == "8/-", "combined direct handle 8 is unchanged: " + got);
        }

        // 7b2) 同一对 c2 分离字在两条路径上各 OR 一次、汇合（phi）后才进 TEX.B（空轨 2nd compute_atmosphere_scatter 的形状）。
        //      真实 decoder / translator：句柄要穿过 phi 按值比叶子才认得出（QueryBindlessPhiSeeThrough，同 iOS dmsh_xlate）。
        //      循环里套分支，TEX 处的句柄 = phi(循环前算的 OR, phi(分支 A 的 OR, 分支 B 的 OR))：上游只比 phi 的直接输入（一个是 phi、一个是 OR）认不出。
        //      R4 = R6|R7；L: TEX.B R0, R2, R4；@P0 BRA A；R4 = R6|R7；BRA J；A: R4 = R6|R7；J: @P0 BRA L。
        {
            const string separateFact = "[{\"stage\":\"fs\",\"handle\":90,\"cbufSlot\":2,\"samplerType\":\"2d\",\"format\":\"Unknown\",\"isBuffer\":false,\"normalized\":true,\"separate\":true,\"arrayLength\":1}]";
            // 槽位：Guest 每 3 条指令前插一个调度字，第 oi 条在第 (oi/3)*4 + oi%3 + 1 个 8 字节；BRA 目标 = 自身地址 + 8 + Imm24
            static int Slot(int oi) => oi / 3 * 4 + oi % 3 + 1;
            static ulong Bra(int from, int to, int pred) => 0xE240000000000000UL | ((ulong)((Slot(to) - Slot(from) - 1) * 8 & 0xFFFFFF) << 20) | ((ulong)pred << 16) | 0xFUL;
            const ulong IsetpNeP0R1 = 0x3660000000000000UL | (5UL << 49) | (7UL << 39) | (7UL << 16) | (1UL << 8) | 7UL;   // ISETP.NE.AND P0, PT, R1, 0, PT
            byte[] shader = Guest(true, new[] {
                Ald(1, 0, 1), IsetpNeP0R1, Ldc(6, 0x168, 2), Ldc(7, 0x568, 2), Fadd32i(2, 0.5f), Fadd32i(3, 0.5f),
                LopOr(4, 6, 7), TexBindless2D(0, 2, 4), Bra(8, 11, 0), LopOr(4, 6, 7), Bra(10, 12, 7), LopOr(4, 6, 7), Bra(12, 7, 0) }, new[] { 1 });
            try {
                using JsonDocument phiResult = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "separate-c2-phi", vs, shader, textureFacts: separateFact, fsCbUse: 4)));
                var binding = phiResult.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                    .Single(b => b.GetProperty("kind").GetString() == "texture");
                Check(binding.GetProperty("slot").GetInt32() == 90 && binding.GetProperty("samp_slot").GetInt32() == 0,
                      "phi-merged c2 separate handle resolves to texture word 90 and sampler slot 0");
            } catch (DirectCompileException error) {
                Check(false, "phi-merged c2 separate handle must compile: " + error.Code + " " + error.Message);
            }
        }

        // 7b3) 类型 3 句柄（#1594）：TEX.B 的句柄 = c2 贴图字 | 采样器号 << 20，OR 的另一边是低 20 位为 0 的常量（Ryujinx TextureHandleType.SeparateConstantSamplerHandle）。
        //      翻译器查事实时给 PackOffsets(贴图字, 采样器号, 3) 与 PackSlots(贴图所在 cbuf, 0)：第二个偏移是立即数采样器池下标，不是常量缓冲字，
        //      所以不能按「两个字都在 c2 表里」配对（那是类型 1，TrySeparateC2）。期望值手算：PackOffsets(90, 257, 3) = 90 | 257 << 14 | 3 << 28 = 809517146；
        //      PackSlots(2, 0) = 2 | 1 << 16 = 65538（与真机读到的那批 `MISSING_FACT … cbufSlot=65538` 同一对数）。
        //      事实只取同一个贴图字的 c2 分离事实 (字, cbufSlot 2, separate=true)；同字事实是纹素缓冲（isBuffer）的不取（立即数采样器对纹素缓冲没意义，仍按缺事实拒）。
        //      片 2：取到事实后 ResolvePoolSlots 编成 slot = 贴图字、constSampler = 采样器池下标（结果多一个可选字段，按 v2 写；没有 samp_slot / uboSlot），
        //      宿主 / 回放按池下标直接取采样器（同 SAMPI）。数组、计算阶段仍具名报未接；下标 0 也是真下标（不能当「没有」省掉）。
        {
            DirectProfile p3;
            using (JsonDocument profileDoc3 = JsonDocument.Parse(profile))
                p3 = DirectProfile.Parse(profileDoc3.RootElement, "profile");
            string Fact3(string stage, int handle, string samplerType, bool isBuffer, bool normalized, bool separate) =>
                "{\"stage\":\"" + stage + "\",\"handle\":" + handle + ",\"cbufSlot\":2,\"samplerType\":\"" + samplerType + "\",\"format\":\"Unknown\",\"isBuffer\":" +
                (isBuffer ? "true" : "false") + ",\"normalized\":" + (normalized ? "true" : "false") + ",\"separate\":" + (separate ? "true" : "false") + ",\"arrayLength\":1}";
            string facts3 = "[" + string.Join(",", new[] {
                Fact3("vs", 90, "cube", false, true, true),     // 字 90 在 vs 的事实（立方体、归一化）：不借给 fs
                Fact3("fs", 90, "2d", false, false, true),      // 字 90 在 fs 的事实（2d、非归一化，用来和 vs 的区分）
                Fact3("fs", 91, "2d", false, true, true),       // 奇数字：有事实也不是分离贴图表的项
                Fact3("fs", 92, "buffer", true, false, true),   // 纹素缓冲：类型 3 不取
                Fact3("fs", 94, "2d", false, true, false),      // 非分离事实：不取
                Fact3("fs", 346, "2d", false, true, true),      // 字 346 是采样器表的起点，不是贴图字
                Fact3("vs", 98, "2d", false, true, true) }) + "]"; // 只有 vs 有的字：fs 不借
            using JsonDocument doc3 = JsonDocument.Parse(RequestJson(profile, "graphics", 1, 1, "const-sampler", vs, fs, textureFacts: facts3));
            DirectProgram prog3 = DirectProgram.Parse(doc3.RootElement.GetProperty("program"), "program", p3);
            var va3 = new DirectGpuAccessor(p3, prog3, prog3.StageOf("vs"), 1, new DirectAllocator(p3), new List<DirectQuery>());
            var fa3 = new DirectGpuAccessor(p3, prog3, prog3.StageOf("fs"), 5, new DirectAllocator(p3), new List<DirectQuery>());
            int slots3 = TextureHandle.PackSlots(2, 0);
            int Const3(int word, int sampler = 257) => TextureHandle.PackOffsets(word, sampler, TextureHandleType.SeparateConstantSamplerHandle);
            Check(Const3(90) == 809517146 && slots3 == 65538, "hand-computed type-3 handle and slots equal the numbers read on the device: " + Const3(90) + " / " + slots3);
            try
            {
                Check(fa3.QuerySamplerType(Const3(90), slots3) == SamplerType.Texture2D && !fa3.QueryTextureCoordNormalized(Const3(90), slots3) &&
                      !fa3.QueryTextureIsBuffer(Const3(90), slots3),
                    "a type-3 handle takes its own stage's same-word c2 separate fact as is (type / buffer flag / normalized)");
                Check(fa3.QuerySamplerType(Const3(90, 1), slots3) == SamplerType.Texture2D && fa3.QuerySamplerType(Const3(90, 4095), slots3) == SamplerType.Texture2D,
                    "the immediate sampler index does not take part in the fact lookup");
                Check(va3.QuerySamplerType(Const3(90), slots3) == SamplerType.TextureCube && va3.QueryTextureCoordNormalized(Const3(90), slots3),
                    "the same handle in vs takes vs's own fact");
            }
            catch (DirectCompileException e) { Check(false, "type-3 fact queries should resolve: " + e.Code + " " + e.Message); }
            // 负控：事实在却不能取的、和别的句柄类型串味的，一律仍是缺事实
            Throws("MISSING_FACT", () => fa3.QueryTextureIsBuffer(Const3(92), slots3), "a type-3 handle on a texel-buffer fact stays a missing fact");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(Const3(92), slots3), "a texel-buffer fact is not borrowed for the sampler type either");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(Const3(94), slots3), "a non-separate fact is not borrowed");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(Const3(96), slots3), "a word without any fact is a missing fact");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(Const3(98), slots3), "another stage's fact is not borrowed");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(Const3(91), slots3), "an odd word is not a separate texture slot");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(Const3(346), slots3), "a sampler-table word is not a separate texture slot");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(Const3(90), TextureHandle.PackSlots(3, 0)), "the texture word must be in c2, not another cbuf");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(TextureHandle.PackOffsets(90, 257, TextureHandleType.SeparateSamplerHandle), slots3),
                "the same numbers as a type-1 handle with a c0 sampler word do not take the type-3 rule");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(TextureHandle.PackOffsets(90, 257, TextureHandleType.SeparateSamplerId), slots3),
                "the same numbers as a type-2 handle do not take the type-3 rule");
            Throws("MISSING_FACT", () => fa3.QuerySamplerType(TextureHandle.PackOffsets(90, 257, TextureHandleType.CombinedSampler), slots3),
                "the same numbers as a combined handle do not take the type-3 rule");
            // 解析口（片 2）：取到事实后，类型 3 句柄在 ResolvePoolSlots 里编成 slot = 贴图字、constSampler = 采样器池下标；不填 samp_slot / uboSlot
            var resolver3 = typeof(DirectShaderCompiler).GetMethod("ResolvePoolSlots", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic);
            // arrayLength 缺省（0）= 与绑定个数相同；单独给是为了让「绑定个数」与「描述符数组长度」两个条件各自单独成立一次
            string Resolve3(DirectGpuAccessor accessor, DirectStage stage3, string stageName, ShaderStage shaderStage, int handle, int cbufSlot, int count, out DirectBinding bound,
                            int arrayLength = 0)
            {
                var allocator3 = new DirectAllocator(p3) { RequestKey = "const-sampler" };
                bound = new DirectBinding(stageName, "texture", "texture", 2, 0, count);
                allocator3.Bindings.Add(bound);
                ShaderProgram shader3 = new(
                    new ShaderProgramInfo(Array.Empty<BufferDescriptor>(), Array.Empty<BufferDescriptor>(),
                        new[] { new TextureDescriptor(2, 0, SamplerType.Texture2D, TextureFormat.R8G8B8A8Unorm,
                            cbufSlot: cbufSlot, handleIndex: handle, arrayLength: arrayLength > 0 ? arrayLength : count, separate: false, TextureUsageFlags.None) },
                        Array.Empty<TextureDescriptor>(), shaderStage, 0, 0, 0,
                        usesFragCoord: false, usesInstanceId: false, usesDrawParameters: false, usesRtLayer: false,
                        clipDistancesWritten: 0, fragmentOutputMap: 0),
                    Ryujinx.Graphics.Shader.Translation.TargetLanguage.Spirv, Array.Empty<byte>());
                try
                {
                    resolver3.Invoke(null, new object[] { new List<(DirectStage stage, ShaderProgram shader, DirectGpuAccessor accessor)>
                        { (stage3, shader3, accessor) }, allocator3 });
                    return "bound";
                }
                catch (System.Reflection.TargetInvocationException error) when (error.InnerException is DirectCompileException compile)
                {
                    return compile.Code + " " + compile.Message;
                }
            }
            DirectStage fsStage3 = prog3.StageOf("fs");
            string Tail3(int word, int sampler, int count = 1, int array = 0) => " [kind=texture handle=" + Const3(word, sampler) + " word=" + word +
                " word2=" + sampler + " type=3 cbuf=2/0 count=" + count + " array=" + (array > 0 ? array : count) + " key=const-sampler]";
            string Bound3(int word, int sampler, out DirectBinding b) =>
                Resolve3(fa3, fsStage3, "fs", ShaderStage.Fragment, Const3(word, sampler), slots3, 1, out b);
            string resolved3 = Bound3(90, 257, out DirectBinding bound3);
            Check(resolved3 == "bound", "a type-3 handle with its same-word fact resolves: " + resolved3);
            Check(bound3.Slot == 90 && bound3.ConstSampler == 257 && bound3.SamplerSlot == -1 && bound3.UboSlot == -1,
                "a type-3 handle binds texture word 90 with constant sampler 257, no samp_slot / uboSlot: slot=" + bound3.Slot + " const=" + bound3.ConstSampler +
                " samp=" + bound3.SamplerSlot + " ubo=" + bound3.UboSlot);
            foreach (var (what, word, sampler) in new[] { ("sampler index 0 is a real index, not 'none'", 90, 0), ("sampler index above 255", 90, 261), ("the largest sampler index", 90, 4095) })
            {
                string r3 = Bound3(word, sampler, out DirectBinding b3);
                Check(r3 == "bound" && b3.Slot == word && b3.ConstSampler == sampler && b3.SamplerSlot == -1, "type-3 " + what + ": " + r3 + " slot=" + b3.Slot + " const=" + b3.ConstSampler);
            }
            // 负控：表达不了的、没有同字事实的、计算阶段 / 数组，都不填槽，具名报未接（前缀不变，宿主按前缀归类）
            string over3 = Bound3(90, 4096, out DirectBinding boundOver3);
            Check(over3 == "UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65538 is not implemented (constant sampler index above 4095)" + Tail3(90, 4096) &&
                  boundOver3.Slot == -1 && boundOver3.ConstSampler == -1,
                "a sampler index the 12-bit handle field cannot hold is rejected by name and binds nothing: " + over3);
            string array3 = Resolve3(fa3, fsStage3, "fs", ShaderStage.Fragment, Const3(90), slots3, 2, out DirectBinding boundArray3);
            Check(array3 == "UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65538 is not implemented (constant sampler array)" + Tail3(90, 257, 2) &&
                  boundArray3.Slot == -1 && boundArray3.ConstSampler == -1,
                "a constant-sampler handle array is rejected by name and binds nothing: " + array3);
            // 数组的两个判据各自单独成立也要拒：只有绑定个数 > 1，或只有描述符数组长度 > 1
            foreach (var (what, count, length) in new[] { ("binding count above 1 alone", 2, 1), ("descriptor array length above 1 alone", 1, 2) })
            {
                string a3 = Resolve3(fa3, fsStage3, "fs", ShaderStage.Fragment, Const3(90), slots3, count, out DirectBinding bArr3, length);
                Check(a3 == "UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65538 is not implemented (constant sampler array)" + Tail3(90, 257, count, length) &&
                      bArr3.Slot == -1 && bArr3.ConstSampler == -1,
                    "a constant-sampler handle with " + what + " is rejected by name and binds nothing: " + a3);
            }
            using JsonDocument docCs3 = JsonDocument.Parse(RequestJson(profile, "compute", 2, 1, "const-sampler-cs", cs: ComputeGuest(),
                textureFacts: "[" + Fact3("cs", 90, "2d", false, true, true) + "]"));
            DirectProgram progCs3 = DirectProgram.Parse(docCs3.RootElement.GetProperty("program"), "program", p3);
            DirectStage csStage3 = progCs3.StageOf("cs");
            var fcs3 = new DirectGpuAccessor(p3, progCs3, csStage3, 0, new DirectAllocator(p3), new List<DirectQuery>());
            string compute3 = Resolve3(fcs3, csStage3, "cs", ShaderStage.Compute, Const3(90), slots3, 1, out DirectBinding boundCs3);
            Check(compute3 == "UNSUPPORTED_PROGRAM cs: texture handle from cbuf slot 65538 is not implemented (constant sampler, compute)" + Tail3(90, 257) &&
                  boundCs3.Slot == -1 && boundCs3.ConstSampler == -1,
                "a compute-stage constant-sampler handle is rejected by name and binds nothing: " + compute3);
            string noFact3 = Bound3(96, 257, out DirectBinding boundNo3);
            //   没有同字事实：走 c2 通用拒绝（#1488 的基础消息：handle= / arrayLength= / count= / request=<sha>，#1601 的尾巴补 kind / word / word2 / type / cbuf / key）；
            //   sha 随请求内容变，所以前后两段分开比，不写死 sha
            Check(noFact3.StartsWith("UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65538 is not implemented (handle=" + Const3(96, 257) +
                                     ", arrayLength=1, count=1, request=", StringComparison.Ordinal) &&
                  noFact3.EndsWith(") [kind=texture word=96 word2=257 type=3 cbuf=2/0 key=const-sampler]", StringComparison.Ordinal) &&
                  !noFact3.Contains("constant sampler", StringComparison.Ordinal) &&
                  boundNo3.Slot == -1 && boundNo3.ConstSampler == -1,
                "a type-3 handle without a same-word fact keeps the generic rejection: " + noFact3);
            // 真实翻译器：LDC c2[0x168] 取贴图字、LOP32I.OR 拼上立即数采样器号、TEX.B 取样；翻译器自己拼出 (809517146, 65538)
            //   结果：贴图绑定 slot = 贴图字、constSampler = 采样器号、没有 samp_slot，schema 按 v2
            string Real3(string facts, uint sampler = 257)
            {
                byte[] shader = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Ldc(6, 0x168, 2), Lop32iOr(4, 6, sampler << 20), TexBindless2D(0, 2, 4) });
                try
                {
                    using JsonDocument r = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "const-sampler", vs, shader, textureFacts: facts, fsCbUse: 4)));
                    string Field(JsonElement b, string name) => b.TryGetProperty(name, out JsonElement f) ? f.GetInt32().ToString() : "-";
                    return r.RootElement.GetProperty("schema").GetString() + " " + string.Join(";", r.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                        .Where(b => b.GetProperty("kind").GetString() == "texture").Select(b => Field(b, "slot") + "/" + Field(b, "constSampler") + "/" + Field(b, "samp_slot")));
                }
                catch (DirectCompileException error) { return error.Code + " " + error.Message; }
            }
            string real3 = Real3(facts3);
            Check(real3 == "s2a-direct-program-v2 90/257/-", "real translator: a constant-sampler handle compiles to texture word 90 + constSampler 257 (v2, no samp_slot): " + real3);
            string real3b = Real3(facts3, 4095);
            Check(real3b == "s2a-direct-program-v2 90/4095/-", "real translator: another immediate sampler index only changes constSampler: " + real3b);
            string real3c = Real3(facts3, 1);
            Check(real3c == "s2a-direct-program-v2 90/1/-", "real translator: a small immediate sampler index is kept as is: " + real3c);
            // 立即数 0：翻译器把 `c2 字 | 0` 折成 c2 字本身（组合句柄，不是类型 3），所以下标 0 走不到 constSampler 的写出；
            //   上面逐项的 allocator 用例钉的是「0 是真下标」，这条钉的是这个折叠：不能被当成常量采样器收下。
            //   折叠之后是一个普通的 c2 单值句柄：#1488 之前走「cbuf slot 2」未接拒绝，之后按 c2 单值句柄放行（v1，slot = 字号，没有 constSampler / samp_slot）
            string real3z = Real3(facts3, 0);
            Check(real3z == "s2a-direct-program-v1 90/-/-",
                "real translator: an immediate sampler index 0 is folded away into a plain c2 word and never becomes a constSampler: " + real3z);
            // 同一个着色器里两个类型 3 句柄（两个贴图字、两个采样器号）：两个贴图绑定各带各的 constSampler
            {
                string facts3Two = "[" + Fact3("fs", 90, "2d", false, true, true) + "," + Fact3("fs", 100, "2d", false, true, true) + "]";
                byte[] shader = Guest(true, new[] {
                    Ldc(6, 0x168, 2), Lop32iOr(8, 6, 257u << 20), Ldc(7, 0x190, 2), Lop32iOr(9, 7, 259u << 20),
                    Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 8),
                    Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(4, 2, 9),
                    FaddR(0, 0, 4), FaddR(1, 1, 5), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f) });
                try
                {
                    using JsonDocument two = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "const-sampler-two", vs, shader, textureFacts: facts3Two, fsCbUse: 4)));
                    string Field(JsonElement b, string name) => b.TryGetProperty(name, out JsonElement f) ? f.GetInt32().ToString() : "-";
                    string pairs = string.Join(";", two.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                        .Where(b => b.GetProperty("kind").GetString() == "texture").Select(b => Field(b, "slot") + "/" + Field(b, "constSampler") + "/" + Field(b, "samp_slot"))
                        .OrderBy(s => s, StringComparer.Ordinal));
                    Check(pairs == "100/259/-;90/257/-", "real translator: two constant-sampler handles keep their own word and sampler: " + pairs);
                }
                catch (DirectCompileException error) { Check(false, "real translator: two constant-sampler handles must compile: " + error.Code + " " + error.Message); }
            }
            foreach (var (what, only) in new[] {
                ("only another stage has the fact", "[" + Fact3("vs", 90, "cube", false, true, true) + "]"),
                ("the same-word fact is a texel buffer", "[" + Fact3("fs", 90, "buffer", true, false, true) + "]"),
                ("the same-word fact is not separate", "[" + Fact3("fs", 90, "2d", false, true, false) + "]") })
            {
                string r = Real3(only);
                Check(r.StartsWith("MISSING_FACT", StringComparison.Ordinal), "real translator: type-3 handle stays MISSING_FACT when " + what + ": " + r);
            }
            // 别的绑定不受影响：类型 1（两个字都在 c2 表里）照旧 samp_slot、没有 constSampler
            {
                const string separateFact = "[{\"stage\":\"fs\",\"handle\":90,\"cbufSlot\":2,\"samplerType\":\"2d\",\"format\":\"Unknown\",\"isBuffer\":false,\"normalized\":true,\"separate\":true,\"arrayLength\":1}]";
                byte[] shader = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Ldc(6, 0x168, 2), Ldc(7, 0x568, 2), LopOr(4, 6, 7), TexBindless2D(0, 2, 4) });
                try
                {
                    using JsonDocument one = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "const-sampler-type1", vs, shader, textureFacts: separateFact, fsCbUse: 4)));
                    JsonElement binding = one.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray().Single(b => b.GetProperty("kind").GetString() == "texture");
                    Check(binding.GetProperty("slot").GetInt32() == 90 && binding.GetProperty("samp_slot").GetInt32() == 0 && !binding.TryGetProperty("constSampler", out _),
                        "a type-1 handle keeps samp_slot and gets no constSampler: " + binding.GetRawText());
                }
                catch (DirectCompileException error) { Check(false, "type-1 handle must still compile: " + error.Code + " " + error.Message); }
            }

            // ---- 片 3：类型 3 句柄的贴图字在游戏自己的 UBO（c3..c18）里，采样器仍是立即数采样器池下标 ----
            //   真机读到的一对数（fs 的 MISSING_FACT）：handle = PackOffsets(40, 260, 3) = 809566248，cbufSlot = PackSlots(5, 0) = 65541（c5 字节偏移 160）。
            //   取事实用拆开的 (字, 贴图所在 cbuf)，就是普通 UBO 句柄那一份精确事实；缺事实时 MISSING_FACT 也写拆开的坐标：
            //   宿主（nvk_shader.c ubo_need_learn）只学「cbuf 3..18、字 < 0x4000」的坐标，打包坐标它一个也不认，写了就永远学不到。
            //   取到事实后 ResolvePoolSlots 编成 uboSlot（cbuf − 3）/ uboOffset（字 × 4）+ constSampler，没有 slot / samp_slot（按 v2 写）。
            {
                string Fact4(string stage, int word, int cbuf, string samplerType, bool isBuffer, bool normalized, bool separate) =>
                    "{\"stage\":\"" + stage + "\",\"handle\":" + word + ",\"cbufSlot\":" + cbuf + ",\"samplerType\":\"" + samplerType + "\",\"format\":\"Unknown\",\"isBuffer\":" +
                    (isBuffer ? "true" : "false") + ",\"normalized\":" + (normalized ? "true" : "false") + ",\"separate\":" + (separate ? "true" : "false") + ",\"arrayLength\":1}";
                string facts4 = "[" + string.Join(",", new[] {
                    Fact4("fs", 40, 5, "2d", false, false, false),      // fs 的 c5 字 40（真机那一对）：2d、非归一化
                    Fact4("fs", 40, 6, "cube", false, true, false),     // 同一个字在 c6：另一个缓冲的另一份事实，不互借
                    Fact4("vs", 40, 5, "cube", false, true, false),     // vs 的 c5 字 40：和 fs 的区分
                    Fact4("vs", 52, 5, "2d", false, true, false),       // 只有 vs 有的字：fs 不借
                    Fact4("fs", 44, 5, "buffer", true, false, false),   // 纹素缓冲：类型 3 不取
                    Fact4("fs", 48, 5, "2d", false, true, true),        // 分离事实：不取（宿主对 UBO 坐标从不给）
                    Fact4("fs", 0, 3, "2d", false, true, false),        // c3 字 0（UBO 号 0）
                    Fact4("fs", 16383, 18, "2d", false, true, false),   // c18 最后一个字（UBO 号 15、字节偏移 65532；类型 3 句柄的字只有 14 位）
                    Fact4("fs", 8191, 18, "2d", false, true, false),    // c18 里 LDC 立即数能到的最后一个字（字节偏移 32764 = 0x7FFC，立即数是有符号 16 位）
                    Fact3("fs", 90, "2d", false, true, true) }) + "]";  // c2 的分离事实（片 1 / 片 2 的来源）：不借给 UBO 句柄
                using JsonDocument doc4 = JsonDocument.Parse(RequestJson(profile, "graphics", 1, 1, "const-sampler-ubo", vs, fs, textureFacts: facts4));
                DirectProgram prog4 = DirectProgram.Parse(doc4.RootElement.GetProperty("program"), "program", p3);
                var va4 = new DirectGpuAccessor(p3, prog4, prog4.StageOf("vs"), 1, new DirectAllocator(p3), new List<DirectQuery>());
                var fa4 = new DirectGpuAccessor(p3, prog4, prog4.StageOf("fs"), 5, new DirectAllocator(p3), new List<DirectQuery>());
                int Slots4(int cbuf) => TextureHandle.PackSlots(cbuf, 0);
                Check(Const3(40, 260) == 809566248 && Slots4(5) == 65541, "hand-computed type-3 UBO handle and slots equal the numbers read on the device: " + Const3(40, 260) + " / " + Slots4(5));
                try
                {
                    Check(fa4.QuerySamplerType(Const3(40, 260), Slots4(5)) == SamplerType.Texture2D && !fa4.QueryTextureCoordNormalized(Const3(40, 260), Slots4(5)) &&
                          !fa4.QueryTextureIsBuffer(Const3(40, 260), Slots4(5)),
                        "a type-3 handle whose texture word is in c5 takes its own stage's exact (word, cbuf) fact as is (type / buffer flag / normalized)");
                    Check(fa4.QuerySamplerType(Const3(40, 0), Slots4(5)) == SamplerType.Texture2D && fa4.QuerySamplerType(Const3(40, 4095), Slots4(5)) == SamplerType.Texture2D,
                        "the immediate sampler index does not take part in the fact lookup (UBO)");
                    Check(va4.QuerySamplerType(Const3(40, 260), Slots4(5)) == SamplerType.TextureCube && va4.QueryTextureCoordNormalized(Const3(40, 260), Slots4(5)),
                        "the same handle in vs takes vs's own fact (UBO)");
                    Check(fa4.QuerySamplerType(Const3(40, 260), Slots4(6)) == SamplerType.TextureCube,
                        "the same word in another cbuf takes that cbuf's own fact");
                    Check(fa4.QuerySamplerType(40, 5) == fa4.QuerySamplerType(Const3(40, 260), Slots4(5)) &&
                          fa4.QueryTextureCoordNormalized(40, 5) == fa4.QueryTextureCoordNormalized(Const3(40, 260), Slots4(5)),
                        "a plain user-UBO handle and a type-3 handle at the same (word, cbuf) take the same fact");
                    Check(fa4.QuerySamplerType(Const3(0, 7), Slots4(3)) == SamplerType.Texture2D && fa4.QuerySamplerType(Const3(16383, 7), Slots4(18)) == SamplerType.Texture2D,
                        "both ends of the user cbuf range (c3 word 0, c18 word 16383) resolve");
                }
                catch (DirectCompileException e) { Check(false, "type-3 UBO fact queries should resolve: " + e.Code + " " + e.Message); }
                string Miss4(Action query)
                {
                    try { query(); return "no exception"; }
                    catch (DirectCompileException e) { return e.Code + " " + e.Message; }
                }
                // 缺精确事实：坐标是拆开后的 (字, 贴图所在 cbuf)，宿主按它读 UBO 补事实
                foreach (var (what, query, want) in new (string, Action, string)[] {
                    ("no fact for that cbuf", () => fa4.QuerySamplerType(Const3(40, 260), Slots4(7)), "stage=fs,handle=40,cbufSlot=7"),
                    ("only another stage has the fact", () => fa4.QuerySamplerType(Const3(52, 260), Slots4(5)), "stage=fs,handle=52,cbufSlot=5"),
                    ("a c2 separate fact with the same word number is not borrowed", () => fa4.QuerySamplerType(Const3(90, 260), Slots4(5)), "stage=fs,handle=90,cbufSlot=5"),
                    ("a word without any fact", () => fa4.QueryTextureCoordNormalized(Const3(41, 260), Slots4(5)), "stage=fs,handle=41,cbufSlot=5") })
                {
                    string r = Miss4(query);
                    Check(r.StartsWith("MISSING_FACT ", StringComparison.Ordinal) && r.EndsWith(" has no source (" + want + ")", StringComparison.Ordinal),
                        "type-3 UBO handle, " + what + ": MISSING_FACT with the unpacked coordinates the host can learn: " + r);
                }
                // 事实在却不能取的、形态不是这一种的：坐标仍写打包的原样（宿主学不了，如实报缺，不会反复重编）
                int Raw(int word, TextureHandleType type) => TextureHandle.PackOffsets(word, 260, type);
                foreach (var (what, query, want) in new (string, Action, string)[] {
                    ("a texel-buffer fact is not taken", () => fa4.QueryTextureIsBuffer(Const3(44, 260), Slots4(5)), "stage=fs,handle=" + Const3(44, 260) + ",cbufSlot=" + Slots4(5)),
                    ("a separate fact is not taken", () => fa4.QuerySamplerType(Const3(48, 260), Slots4(5)), "stage=fs,handle=" + Const3(48, 260) + ",cbufSlot=" + Slots4(5)),
                    ("cbuf 19 is not a user cbuf", () => fa4.QuerySamplerType(Const3(40, 260), Slots4(19)), "stage=fs,handle=" + Const3(40, 260) + ",cbufSlot=" + Slots4(19)),
                    ("cbuf 1 is not a user cbuf", () => fa4.QuerySamplerType(Const3(40, 260), Slots4(1)), "stage=fs,handle=" + Const3(40, 260) + ",cbufSlot=" + Slots4(1)),
                    ("cbuf 0 is not a user cbuf", () => fa4.QuerySamplerType(Const3(40, 260), Slots4(0)), "stage=fs,handle=" + Const3(40, 260) + ",cbufSlot=" + Slots4(0)),
                    ("no cbuf at all", () => fa4.QuerySamplerType(Const3(40, 260), -1), "stage=fs,handle=" + Const3(40, 260) + ",cbufSlot=-1"),
                    ("the same numbers as a type-1 handle", () => fa4.QuerySamplerType(Raw(40, TextureHandleType.SeparateSamplerHandle), Slots4(5)),
                        "stage=fs,handle=" + Raw(40, TextureHandleType.SeparateSamplerHandle) + ",cbufSlot=" + Slots4(5)),
                    ("the same numbers as a type-2 handle", () => fa4.QuerySamplerType(Raw(40, TextureHandleType.SeparateSamplerId), Slots4(5)),
                        "stage=fs,handle=" + Raw(40, TextureHandleType.SeparateSamplerId) + ",cbufSlot=" + Slots4(5)),
                    ("the same numbers as a combined handle", () => fa4.QuerySamplerType(Raw(40, TextureHandleType.CombinedSampler), Slots4(5)),
                        "stage=fs,handle=" + Raw(40, TextureHandleType.CombinedSampler) + ",cbufSlot=" + Slots4(5)) })
                {
                    string r = Miss4(query);
                    Check(r.StartsWith("MISSING_FACT ", StringComparison.Ordinal) && r.EndsWith(" has no source (" + want + ")", StringComparison.Ordinal),
                        "type-3 UBO handle, " + what + ": MISSING_FACT with the packed coordinates as they came: " + r);
                }
                // 解析口：取到事实后编成 uboSlot / uboOffset + constSampler；不填 slot / samp_slot
                DirectStage fsStage4 = prog4.StageOf("fs");
                string Tail4(int word, int sampler, int cbuf, int count = 1, int array = 0) => " [kind=texture handle=" + Const3(word, sampler) + " word=" + word + " word2=" + sampler +
                    " type=3 cbuf=" + cbuf + "/0 count=" + count + " array=" + (array > 0 ? array : count) + " key=const-sampler]";
                string Bound4(int word, int sampler, int cbuf, out DirectBinding b) =>
                    Resolve3(fa4, fsStage4, "fs", ShaderStage.Fragment, Const3(word, sampler), Slots4(cbuf), 1, out b);
                foreach (var (what, word, sampler, cbuf, ubo, off) in new[] {
                    ("the handle read on the device", 40, 260, 5, 2, 160),
                    ("sampler index 0 is a real index, not 'none'", 0, 0, 3, 0, 0),
                    ("the largest sampler index and the last word of c18", 16383, 4095, 18, 15, 65532) })
                {
                    string r4 = Bound4(word, sampler, cbuf, out DirectBinding b4);
                    Check(r4 == "bound" && b4.Slot == -1 && b4.UboSlot == ubo && b4.UboOffset == off && b4.ConstSampler == sampler && b4.SamplerSlot == -1,
                        "type-3 UBO " + what + ": " + r4 + " slot=" + b4.Slot + " ubo=" + b4.UboSlot + "/" + b4.UboOffset + " const=" + b4.ConstSampler + " samp=" + b4.SamplerSlot);
                }
                // 负控：表达不了的、没有同字事实的、计算阶段 / 数组，都不填任何来源，具名报未接（前缀不变，宿主按前缀归类）
                string over4 = Bound4(40, 4096, 5, out DirectBinding boundOver4);
                Check(over4 == "UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65541 is not implemented (constant sampler index above 4095)" + Tail4(40, 4096, 5) &&
                      boundOver4.Slot == -1 && boundOver4.UboSlot == -1 && boundOver4.ConstSampler == -1,
                    "a sampler index the 12-bit handle field cannot hold is rejected by name and binds nothing (UBO): " + over4);
                foreach (var (what, count, length) in new[] { ("binding count above 1", 2, 2), ("binding count above 1 alone", 2, 1), ("descriptor array length above 1 alone", 1, 2) })
                {
                    string a4 = Resolve3(fa4, fsStage4, "fs", ShaderStage.Fragment, Const3(40, 260), Slots4(5), count, out DirectBinding bArr4, length);
                    Check(a4 == "UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65541 is not implemented (constant sampler array)" + Tail4(40, 260, 5, count, length) &&
                          bArr4.Slot == -1 && bArr4.UboSlot == -1 && bArr4.ConstSampler == -1,
                        "a type-3 UBO handle with " + what + " is rejected by name and binds nothing: " + a4);
                }
                using JsonDocument docCs4 = JsonDocument.Parse(RequestJson(profile, "compute", 2, 1, "const-sampler-ubo-cs", cs: ComputeGuest(),
                    textureFacts: "[" + Fact4("cs", 40, 5, "2d", false, true, false) + "]"));
                DirectProgram progCs4 = DirectProgram.Parse(docCs4.RootElement.GetProperty("program"), "program", p3);
                DirectStage csStage4 = progCs4.StageOf("cs");
                var fcs4 = new DirectGpuAccessor(p3, progCs4, csStage4, 0, new DirectAllocator(p3), new List<DirectQuery>());
                string compute4 = Resolve3(fcs4, csStage4, "cs", ShaderStage.Compute, Const3(40, 260), Slots4(5), 1, out DirectBinding boundCs4);
                Check(compute4 == "UNSUPPORTED_PROGRAM cs: texture handle from cbuf slot 65541 is not implemented (constant sampler, compute)" + Tail4(40, 260, 5) &&
                      boundCs4.Slot == -1 && boundCs4.UboSlot == -1 && boundCs4.ConstSampler == -1,
                    "a compute-stage type-3 UBO handle is rejected by name and binds nothing: " + compute4);
                foreach (var (what, word) in new[] { ("no same-word fact", 41), ("the same-word fact is a texel buffer", 44), ("the same-word fact is separate", 48) })
                {
                    string n4 = Bound4(word, 260, 5, out DirectBinding bNo4);
                    Check(n4.StartsWith("UNSUPPORTED_PROGRAM fs: texture handle from cbuf slot 65541 is not implemented (handle=" + Const3(word, 260) +
                                        ", arrayLength=1, count=1, request=", StringComparison.Ordinal) &&
                          n4.EndsWith(") [kind=texture word=" + word + " word2=260 type=3 cbuf=5/0 key=const-sampler]", StringComparison.Ordinal) &&
                          !n4.Contains("constant sampler", StringComparison.Ordinal) && bNo4.Slot == -1 && bNo4.UboSlot == -1 && bNo4.ConstSampler == -1,
                        "a type-3 UBO handle with " + what + " keeps the generic rejection: " + n4);
                }
                // 真实翻译器：LDC c5[0xA0] 取贴图字、LOP32I.OR 拼上立即数采样器号、TEX.B 取样；翻译器自己拼出 (809566248, 65541)
                string Real4(string facts, uint sampler = 260, int cbuf = 5, int byteOffset = 0xA0)
                {
                    byte[] shader = Guest(true, new[] { Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), Ldc(6, byteOffset, cbuf), Lop32iOr(4, 6, sampler << 20), TexBindless2D(0, 2, 4) });
                    try
                    {
                        using JsonDocument r = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "const-sampler-ubo", vs, shader, textureFacts: facts, fsCbUse: 1u << cbuf)));
                        string Field(JsonElement b, string name) => b.TryGetProperty(name, out JsonElement f) ? f.GetInt32().ToString() : "-";
                        return r.RootElement.GetProperty("schema").GetString() + " " + string.Join(";", r.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                            .Where(b => b.GetProperty("kind").GetString() == "texture")
                            .Select(b => Field(b, "slot") + "/" + Field(b, "uboSlot") + "/" + Field(b, "uboOffset") + "/" + Field(b, "constSampler") + "/" + Field(b, "samp_slot")));
                    }
                    catch (DirectCompileException error) { return error.Code + " " + error.Message; }
                }
                string real4 = Real4(facts4);
                Check(real4 == "s2a-direct-program-v2 -/2/160/260/-", "real translator: a type-3 handle in c5 compiles to uboSlot 2 / uboOffset 160 + constSampler 260 (v2, no slot / samp_slot): " + real4);
                string real4b = Real4(facts4, 4095, 3, 0);
                Check(real4b == "s2a-direct-program-v2 -/0/0/4095/-", "real translator: c3 word 0 with the largest sampler index: " + real4b);
                string real4c = Real4(facts4, 260, 18, 0x7FFC);
                Check(real4c == "s2a-direct-program-v2 -/15/32764/260/-", "real translator: c18 at the last offset an LDC immediate reaches (byte 0x7FFC): " + real4c);
                // 立即数 0：翻译器把 `UBO 字 | 0` 折成 UBO 字本身（组合句柄，不是类型 3），所以下标 0 走不到 constSampler 的写出；
                //   折叠之后是一个普通的游戏 UBO 句柄（v1，没有 constSampler / samp_slot）：采样器取同字高位，是现行口径
                string real4z = Real4(facts4, 0);
                Check(real4z == "s2a-direct-program-v1 -/2/160/-/-",
                    "real translator: an immediate sampler index 0 is folded away into a plain user-UBO handle and never becomes a constSampler: " + real4z);
                string real4Miss = Real4("[" + Fact3("fs", 90, "2d", false, true, true) + "]");
                Check(real4Miss.StartsWith("MISSING_FACT ", StringComparison.Ordinal) && real4Miss.EndsWith(" has no source (stage=fs,handle=40,cbufSlot=5)", StringComparison.Ordinal),
                    "real translator: no (word, cbuf) fact reports the unpacked coordinates the host learns from: " + real4Miss);
                foreach (var (what, only) in new[] {
                    ("only another stage has the fact", "[" + Fact4("vs", 40, 5, "cube", false, true, false) + "]"),
                    ("the same-word fact is a texel buffer", "[" + Fact4("fs", 40, 5, "buffer", true, false, false) + "]"),
                    ("the same-word fact is separate", "[" + Fact4("fs", 40, 5, "2d", false, true, true) + "]") })
                {
                    string r = Real4(only);
                    Check(r.StartsWith("MISSING_FACT", StringComparison.Ordinal), "real translator: type-3 UBO handle stays MISSING_FACT when " + what + ": " + r);
                }
                // 同一个着色器里两个 UBO 里的类型 3 句柄：各带各的来源，不串
                {
                    string facts4Two = "[" + Fact4("fs", 40, 5, "2d", false, true, false) + "," + Fact4("fs", 40, 6, "2d", false, true, false) + "]";
                    byte[] shader = Guest(true, new[] {
                        Ldc(6, 0xA0, 5), Lop32iOr(8, 6, 260u << 20), Ldc(7, 0xA0, 6), Lop32iOr(9, 7, 262u << 20),
                        Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 8),
                        Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(4, 2, 9),
                        FaddR(0, 0, 4), FaddR(1, 1, 5), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f) });
                    try
                    {
                        using JsonDocument two = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "const-sampler-ubo-two", vs, shader, textureFacts: facts4Two, fsCbUse: (1u << 5) | (1u << 6))));
                        string Field(JsonElement b, string name) => b.TryGetProperty(name, out JsonElement f) ? f.GetInt32().ToString() : "-";
                        string pairs = string.Join(";", two.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                            .Where(b => b.GetProperty("kind").GetString() == "texture").Select(b => Field(b, "uboSlot") + "/" + Field(b, "uboOffset") + "/" + Field(b, "constSampler"))
                            .OrderBy(s => s, StringComparer.Ordinal));
                        Check(pairs == "2/160/260;3/160/262", "real translator: two type-3 UBO handles keep their own cbuf and sampler: " + pairs);
                    }
                    catch (DirectCompileException error) { Check(false, "real translator: two type-3 UBO handles must compile: " + error.Code + " " + error.Message); }
                }
                // 同一个着色器里 c2 的类型 3（片 2）与 UBO 的类型 3（片 3）各走各的口：c2 给 slot + constSampler，UBO 给 uboSlot / uboOffset + constSampler
                {
                    string factsMixed = "[" + Fact3("fs", 90, "2d", false, true, true) + "," + Fact4("fs", 40, 5, "2d", false, true, false) + "]";
                    byte[] shader = Guest(true, new[] {
                        Ldc(6, 0x168, 2), Lop32iOr(8, 6, 257u << 20), Ldc(7, 0xA0, 5), Lop32iOr(9, 7, 260u << 20),
                        Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(0, 2, 8),
                        Fadd32i(2, 0.5f), Fadd32i(3, 0.5f), TexBindless2D(4, 2, 9),
                        FaddR(0, 0, 4), FaddR(1, 1, 5), Fadd32i(2, 0.0f), Fadd32i(3, 1.0f) });
                    try
                    {
                        using JsonDocument mixed = ParseResult(DirectShaderCompiler.Compile(RequestJson(profile, "graphics", 1, 1, "const-sampler-mixed", vs, shader, textureFacts: factsMixed, fsCbUse: (1u << 2) | (1u << 5))));
                        string Field(JsonElement b, string name) => b.TryGetProperty(name, out JsonElement f) ? f.GetInt32().ToString() : "-";
                        string pairs = string.Join(";", mixed.RootElement.GetProperty("content").GetProperty("bindings").EnumerateArray()
                            .Where(b => b.GetProperty("kind").GetString() == "texture")
                            .Select(b => Field(b, "slot") + "/" + Field(b, "uboSlot") + "/" + Field(b, "uboOffset") + "/" + Field(b, "constSampler"))
                            .OrderBy(s => s, StringComparer.Ordinal));
                        Check(pairs == "-/2/160/260;90/-/-/257", "real translator: a c2 type-3 handle and a UBO type-3 handle in one shader keep their own sources: " + pairs);
                    }
                    catch (DirectCompileException error) { Check(false, "real translator: mixed type-3 handles must compile: " + error.Code + " " + error.Message); }
                }
            }
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
