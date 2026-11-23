// #783 出包 Vulkan 产物单测（t_dsh_vk.sh 调）：原创合成控制段 + 手写 Maxwell 指令（VS / PS / CS 各一：存储缓冲、组合纹理、常量缓冲），
//   走 DshVk.CompileWithSpv 同键出 _b.json 与 _b.spv。期望按卡面写死，不跑被测实现推：
//   ① 每个键 _b.json 与 _b.spv 都在（两边键集合相等）；
//   ② _b.spv 里每个资源变量 DescriptorSet = 0、Binding = json 里同名成员的 arg_id × 6 + 阶段号（vs 0 / ps 4 / cs 5，#1254 直连缺陷线定；
//      本文件自己解析 SPIR-V，不用 BindToArgIds），//      组合纹理变量对的是 texture 成员，不是 …Smplr；
//   ③ 每个 _b.spv 过 spirv-val --target-env vulkan1.1。
//   反证：④ 翻译器原样的 .spv（未改写）在本夹具上至少一个资源对不上 arg_id，判据不会绿着失效；
//   ⑤ json 里删掉一个 .spv 有的成员 → BindToArgIds 报红，不静默（⑪ 字面量撞上变量 id 不算引用）；⑥ json 里改一个 arg_id → 改写结果跟着变，对原 json 判不一致；
//   ⑦ 把 json 里没有的 support_buffer 经 OpCopyObject 在函数体里取用一次 → 判为「用到了却没有成员」报红，不挪到空号。
//   ⑧ vs 与 ps 合在一个管线布局里不撞号：两阶段 arg_id 0 / 1 的绑定号手算为 vs 0 / 6、ps 4 / 10，两阶段全部绑定号互不相同（安卓 backend_vulkan.c 按 (set, binding) 合并）；
//   ⑨ 出包模式（DshCompiler.PackOut）：输出目录不建、json 取自 PackFiles、_b.spv 进 DshVk.PackSpv，且与落盘那份逐字节相同；
//   ⑩ gs / hs / ds 调 CompileWithSpv 报 DshError（不出 .spv）；
//   ⑫ 几何着色器走 DshVk.CompileGeometry（原生几何阶段，安卓离线库收几何）：手写 GS（读 c3、采样纹理槽 0、读输入位置、写位置与通用输出 0、发射一个顶点），
//      出点 / 线 / 三角三份 _vg<t>.json + _vg<t>.spv 恰好 6 个文件；每份 json stage = gs、dm_ctl = 键、gs_input_topology = t，
//      反射里有 cbuf reg 0 与 tex reg 0（手算：LDC c3 → 用户常量槽 0；TEXS 句柄字 8 → 纹理槽 0）；.spv 入口是 Geometry、输入图元执行模式随 t
//      （InputPoints 19 / InputLines 20 / Triangles 22），每个资源变量 (set, binding) = (0, arg_id × 6 + 3)、spirv-val 过；
//      反证：同一份按阶段号 4 判必不一致（判据不会绿着失效）；直通型几何头（字 0 位 24）GeometrySkip 给原因、CompileGeometry 报 DshError。
// 夹具：控制段 0x880 字节全零（c1 / VertexA/B / 变换反馈都空），+0x10 放阶段号只为键不同；代码区 = 0x30 前缀 + SPH（计算着色器 = 0x12345679 头 + 0x100 起指令），
//   编码按 compat/tree/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs / InstDecoders.cs；答案表 / 配对表为空表（全部按缺省答）。没有游戏内容。
// libdsx（--dsx，必给）：Finish 对片元调 NxgalSpvCross.ColorRound，Compile 对所有阶段交 SPIRV-Cross 前先调 NxgalSpvCross.Harden（main de14970a6，#1204 T1），
//   两者都 DllImport("dsx")。t_dsh_vk.sh 在 Mac 给 build/dsx/libdsx.dylib，在 Linux 给 build/dsx/linux/libdsx.so（#783），
//   三个键都走 CompileWithSpv。SPIRV-Cross 两边都用命令行版（CliSpvCross），--dsx 只改上面两个 DllImport 的去处。
//   ⑭ 存储缓冲窗口读回夹具料（可选 --emit <目录>）：同一份 VS（读 [基址+0x20]、写 [基址+0x40] 与 [基址+0]，基址取自 c0+0x110，写入值取自 c3[0]）
//      走 CompileWithSpv 出 _b.json / _b.spv 放进给定目录，android/tests/vk/standalone/libpack-ssbo-window.sh 在 lavapipe 上用它读回字节。
// 用法：dsh_vk_test <spirv-cross> <spirv-val> <工作目录> --dsx <libdsx> [--emit <目录>]
using S2A.Dsh;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

static class Program
{
    const ulong EXIT = 0xe30000000007000fUL;
    static ulong Mov32i(int rd, uint v) => 0x0100000000000000UL | ((ulong)v << 20) | (7UL << 16) | (15UL << 12) | (ulong)rd;
    static ulong Ldc(int rd, int slot, int off, int size) => 0xef90000000000000UL | ((ulong)size << 48) | ((ulong)slot << 36) | ((ulong)off << 20) | (7UL << 16) | (255UL << 8) | (ulong)rd;   // size 4 = B32、5 = B64
    static ulong Ldg32(int rd, int ra, int off) => 0xeed4200000000000UL | ((ulong)off << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;
    static ulong Ldg128(int rd, int ra, int off) => 0xeed6200000000000UL | ((ulong)off << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;   // LDG.E.128：Rd..Rd+3
    static ulong Stg128(int rs, int ra, int off) => 0xeede200000000000UL | ((ulong)off << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rs;   // STG.E.128：Rs..Rs+3
    static ulong Stg32(int rs, int ra, int off) => 0xeedc200000000000UL | ((ulong)off << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rs;
    // TEXS.2D Rd(仅 R 分量), Ra = x, Rb = y；TidB = 驱动常量段 c2 里的句柄字偏移（8 → 0x20 = 纹理槽 0）
    static ulong Texs2D(int rd, int ra, int rb, int tid) => 0xd800000000000000UL | (1UL << 53) | ((ulong)tid << 36) | (255UL << 28) | ((ulong)rb << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;

    // SULD.D Rd, [Ra…]：非 bindless 的表面读——TidB = c2 图像表槽位号；格式按读的大小定（Size 4 = B32 → R32Uint、5 = B64 → R32G32Uint），
    //   空轨 2nd 的 post_tonemap_update（读上一帧曝光的 1x1）与 set_texture_alpha 同型；Dim 3 = _2d。
    static ulong SuldD(int rd, int ra, int tid, int dim, int size) =>
        0xeb18000000000000UL | ((ulong)tid << 36) | ((ulong)dim << 33) | (7UL << 30) | ((ulong)size << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;
    // SUST.D [Ra…], Rd 写出值——同槽位有写就并入 ImageStore 用途，声明照旧必须具体格式（无格式写能力还没进 profile）
    static ulong SustD(int rd, int ra, int tid, int dim, int size) =>
        0xeb38000000000000UL | ((ulong)tid << 36) | ((ulong)dim << 33) | (7UL << 30) | ((ulong)size << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;

    static byte[] Bundles(byte[] head, ulong[] ops)
    {
        int n = (ops.Length + 2) / 3 * 3;
        var b = new byte[head.Length + n / 3 * 32];
        head.CopyTo(b, 0);
        for (int i = 0; i < n; i++) BitConverter.TryWriteBytes(b.AsSpan(head.Length + (i / 3 * 4 + i % 3 + 1) * 8), i < ops.Length ? ops[i] : EXIT);
        return b;
    }

    static byte[] Sph(uint word0, int omapByte)
    {
        var h = new byte[0x30 + 0x50];   // 0x30 前缀（JobFor 切掉）+ SPH
        BitConverter.TryWriteBytes(h.AsSpan(0x30), word0);
        if (omapByte >= 0) h[0x30 + 72] = (byte)omapByte;
        return h;
    }

    // ⑫ 几何：ALD / AST / OUT 编码按 InstTable.cs（ALD 1110111111011…、AST 1110111111110…、OUT(R) 1111101111100…）与 InstDecoders.cs 字段
    //   （ALD/AST：Rd/数据 0–7、Ra 8–15、谓词 16–18、Imm11 属性字节地址 20–30、顶点 Rb/Rc 39–46、AlSize 47–48；OUT：OutType 39–40，1 = EMIT、2 = CUT）
    static ulong Ald(int rd, int attr, int alSize) => (0b1110111111011UL << 51) | ((ulong)alSize << 47) | (255UL << 39) | ((ulong)attr << 20) | (7UL << 16) | (255UL << 8) | (ulong)rd;
    static ulong Ast(int rs, int attr, int alSize) => (0b1110111111110UL << 51) | ((ulong)alSize << 47) | (255UL << 39) | ((ulong)attr << 20) | (7UL << 16) | (255UL << 8) | (ulong)rs;
    static ulong Out(int type) => (0b1111101111100UL << 51) | ((ulong)type << 39) | (255UL << 20) | (7UL << 16) | (255UL << 8) | 255UL;
    static (byte[] Code, byte[] Ctl) GeometryFixture(bool passthrough)
    {
        var h = Sph(1u | (3u << 5) | (4u << 10) | (passthrough ? 1u << 24 : 0u), -1);
        BitConverter.TryWriteBytes(h.AsSpan(0x30 + 8), 1u << 24);   // 字 2：每输入图元 1 个调用
        BitConverter.TryWriteBytes(h.AsSpan(0x30 + 12), 7u << 24);  // 字 3：输出拓扑 7 = 三角带
        BitConverter.TryWriteBytes(h.AsSpan(0x30 + 16), 3u);        // 字 4：最多输出 3 个顶点
        var code = Bundles(h, new[] {
            Ald(0, 0x70, 3),                                                // R0–R3 = 输入顶点 0 的位置
            Mov32i(6, 0x3f000000), Mov32i(7, 0x3f000000), Texs2D(5, 6, 7, 8), // R5 = 纹理槽 0 采样
            Ldc(4, 3, 0, 4),                                                // R4 = c3[0]
            Ast(0, 0x70, 3), Ast(4, 0x80, 0), Ast(5, 0x84, 0),              // 位置 + 通用输出 0 的 x / y
            Out(1), Out(2), EXIT });
        var ctl = new byte[0x880]; ctl[0x10] = 4;
        return (code, ctl);
    }

    // ⑭ 窗口读回夹具的 VS：R0:R1 = c0+0x110 的存储缓冲 0 基址（游戏原始地址），R2 = c3[0]；R4 = [基址+0x20]；[基址+0x40] = R4；[基址+0] = R2
    static (byte[] Code, byte[] Ctl) WindowFixture()
    {
        const uint store = (1u << 16) | (1u << 26);   // SPH DoesGlobalStore / DoesLoadOrStore
        var code = Bundles(Sph(1 | (3 << 5) | (1 << 10) | store, -1), new[] {
            Ldc(0, 0, 0x110, 5), Ldc(2, 3, 0, 4), Ldg32(4, 0, 0x20), Stg32(4, 0, 0x40), Stg32(2, 0, 0), EXIT });
        var ctl = new byte[0x880]; ctl[0x10] = 21;
        return (code, ctl);
    }

    // ⑭ 片元阶段（8E 新库里 8240 个 PS 带窗口掩码、只有 440 个 VS）：存储缓冲 0 = c0+0x510、1 = c0+0x520，用户常量缓冲 0 首字 = c3[0]
    //   [B0+0x40] = [B0+0x20]（32 位）；[B0+0x80..0x8f] = [B0+0x30..0x3f]（128 位）；[B0+0] = c3[0]；[B1+0x20] = [B1+0x10]（第二个存储缓冲）；最后写出常量颜色
    static (byte[] Code, byte[] Ctl) WindowFixturePs()
    {
        const uint store = (1u << 16) | (1u << 26);
        var code = Bundles(Sph(2 | (3 << 5) | (5 << 10) | store, 15), new[] {
            Ldc(0, 0, 0x510, 5), Ldc(14, 0, 0x520, 5), Ldc(2, 3, 0, 4),
            Ldg32(4, 0, 0x20), Stg32(4, 0, 0x40),
            Ldg128(8, 0, 0x30), Stg128(8, 0, 0x80),
            Stg32(2, 0, 0),
            Ldg32(5, 14, 0x10), Stg32(5, 14, 0x20),
            Mov32i(0, 0x3f800000), Mov32i(1, 0x3f000000), Mov32i(2, 0), Mov32i(3, 0x3f800000), EXIT });
        var ctl = new byte[0x880]; ctl[0x10] = 22;
        return (code, ctl);
    }

    // ⑭ 计算阶段：存储缓冲 0 = c0+0x310。[B+0x40] = [B+0x20]；[B+0x80..0x8f] = [B+0x30..0x3f]（128 位）；[B+0] = c3[0]
    static (byte[] Code, byte[] Ctl) WindowFixtureCs()
    {
        var head = new byte[0x100]; BitConverter.TryWriteBytes(head.AsSpan(0), 0x12345679u);
        var code = Bundles(head, new[] {
            Ldc(0, 0, 0x310, 5), Ldc(3, 3, 0, 4),
            Ldg32(2, 0, 0x20), Stg32(2, 0, 0x40),
            Ldg128(8, 0, 0x30), Stg128(8, 0, 0x80),
            Stg32(3, 0, 0), EXIT });
        var ctl = new byte[0x880]; ctl[0x10] = 31;
        return (code, ctl);
    }

    static (string Stage, byte[] Code, byte[] Ctl)[] Fixtures()
    {
        const uint store = (1u << 16) | (1u << 26);   // SPH DoesGlobalStore / DoesLoadOrStore
        var vs = Bundles(Sph(1 | (3 << 5) | (1 << 10) | store, -1), new[] {
            Ldc(0, 0, 0x110, 5), Ldc(2, 3, 0, 4), Stg32(2, 0, 0), EXIT });                       // c0+0x110 = 顶点阶段存储缓冲 0；c3 = 常量缓冲 0
        var ps = Bundles(Sph(2 | (3 << 5) | (5 << 10), 15), new[] {
            Mov32i(0, 0x3f000000), Mov32i(1, 0x3f000000), Texs2D(0, 0, 1, 8), Ldc(1, 3, 0, 4), Mov32i(2, 0), Mov32i(3, 0x3f800000), EXIT });
        var csHead = new byte[0x100]; BitConverter.TryWriteBytes(csHead.AsSpan(0), 0x12345679u);
        var cs = Bundles(csHead, new[] {
            Ldc(0, 0, 0x310, 5), Ldg32(2, 0, 0), Ldc(3, 3, 0, 4), Stg32(3, 0, 4), Stg32(2, 0, 8), EXIT });   // c0+0x310 = 计算阶段存储缓冲 0
        (string, byte[], byte[]) F(string st, byte[] code, byte mark) { var ctl = new byte[0x880]; ctl[0x10] = mark; return (st, code, ctl); }
        return new[] { F("vs", vs, 1), F("ps", ps, 2), F("cs", cs, 3) };
    }

    // ---- 独立的 SPIR-V 读取：资源变量名 → (DescriptorSet, Binding, 入口是否取用) ----
    static Dictionary<string, (uint Set, uint Binding, bool Used)> Resources(byte[] spv)
    {
        var w = new uint[spv.Length / 4]; Buffer.BlockCopy(spv, 0, w, 0, w.Length * 4);
        var names = new Dictionary<uint, string>(); var set = new Dictionary<uint, uint>(); var bind = new Dictionary<uint, uint>(); var vars = new Dictionary<uint, uint>();
        var touched = new HashSet<uint>();
        for (int i = 5; i < w.Length;)
        {
            int n = (int)(w[i] >> 16), op = (int)(w[i] & 0xffff);
            if (n == 0) throw new InvalidDataException("bad SPIR-V word count");
            if (op == 5) { var bytes = new List<byte>(); for (int k = i + 2; k < i + n; k++) bytes.AddRange(BitConverter.GetBytes(w[k])); int z = bytes.IndexOf(0); names[w[i + 1]] = System.Text.Encoding.UTF8.GetString(bytes.Take(z < 0 ? bytes.Count : z).ToArray()); }
            if (op == 71 && n >= 4 && w[i + 2] == 34) set[w[i + 1]] = w[i + 3];
            if (op == 71 && n >= 4 && w[i + 2] == 33) bind[w[i + 1]] = w[i + 3];
            if (op == 59 && n >= 4) vars[w[i + 2]] = w[i + 3];
            // 取用：OpLoad 指针、OpStore 指针、Op*AccessChain / OpArrayLength / OpImageTexelPointer 的基址
            if (op == 61 && n >= 4) touched.Add(w[i + 3]);
            if (op == 62 && n >= 3) touched.Add(w[i + 1]);
            if (op is 60 or 65 or 66 or 67 or 68 && n >= 4) touched.Add(w[i + 3]);
            i += n;
        }
        return vars.Where(v => v.Value is 0 or 2 or 12 && bind.ContainsKey(v.Key))
                   .ToDictionary(v => names.GetValueOrDefault(v.Key, "%" + v.Key), v => (set.GetValueOrDefault(v.Key, uint.MaxValue), bind[v.Key], touched.Contains(v.Key)));
    }

    static bool UsesStorageWindowMask(byte[] spv, uint id)
    {
        var words = MemoryMarshal.Cast<byte, uint>(spv.AsSpan());
        bool defaultMask = false;
        bool maskedAddress = false;
        var maskIds = new HashSet<uint> { id };
        for (int i = 5; i < words.Length;)
        {
            int count = (int)(words[i] >> 16);
            if (count == 0 || i + count > words.Length) throw new InvalidDataException("bad SPIR-V word count");
            int op = (int)(words[i] & 0xffff);
            if (op == 50 && count == 4 && words[i + 2] == id) // OpSpecConstant
                defaultMask = words[i + 3] == uint.MaxValue;
            if (op == 124 && count == 4 && maskIds.Contains(words[i + 3])) // OpBitcast
                maskIds.Add(words[i + 2]);
            if (op == 199 && count == 5 && (maskIds.Contains(words[i + 3]) || maskIds.Contains(words[i + 4]))) // OpBitwiseAnd
                maskedAddress = true;
            i += count;
        }
        return defaultMask && maskedAddress;
    }

    // 全部 OpTypeImage 的（dim, Sampled, Image Format 操作数）：OpTypeImage %id %采样类型 %dim %depth %arrayed %ms %sampled %format
    static List<(uint Dim, uint Sampled, uint Fmt)> ImageFormats(byte[] spv)
    {
        var w = new uint[spv.Length / 4]; Buffer.BlockCopy(spv, 0, w, 0, w.Length * 4);
        var list = new List<(uint, uint, uint)>();
        for (int i = 5; i < w.Length;)
        {
            int n = (int)(w[i] >> 16), op = (int)(w[i] & 0xffff);
            if (n == 0) throw new InvalidDataException("bad SPIR-V word count");
            if (op == 25 && n >= 9) list.Add((w[i + 3], w[i + 7], w[i + 8]));
            i += n;
        }
        return list;
    }

    // 阶段号（期望表，照卡面口径手写）：绑定号 = arg_id × 6 + 阶段号
    static readonly Dictionary<string, long> StageNoOf = new() { ["vs"] = 0, ["ps"] = 4, ["cs"] = 5 };

    // 有同名成员的：(set, binding) = (0, arg_id × 6 + 阶段号)；没有的：必须入口没取用、在 set 0、落在本阶段的模 6 槽且 binding / 6 大于全部 arg_id；全部绑定号互不相同
    static List<string> Mismatch(Dictionary<string, (uint Set, uint Binding, bool Used)> res, JsonArray bindings, long stageNo)
    {
        var aid = bindings.OfType<JsonObject>().ToDictionary(o => (string)o["msl_name"], o => (long)o["arg_id"]);
        long top = aid.Count == 0 ? -1 : aid.Values.Max();
        var bad = new List<string>();
        foreach (var (name, (s, b, used)) in res)
            if (aid.TryGetValue(name, out long id)) { if (s != 0 || b != id * 6 + stageNo) bad.Add($"{name}: (set {s}, binding {b}) ≠ (0, {id} × 6 + {stageNo})"); }
            else if (used) bad.Add($"{name}: 入口取用了，但 json 里没有同名成员");
            else if (s != 0 || b % 6 != stageNo || b / 6 <= top) bad.Add($"{name}: 没取用的变量应在 set 0、binding % 6 = {stageNo}、binding / 6 > {top}，实际 (set {s}, binding {b})");
        if (res.Values.Select(r => r.Binding).Distinct().Count() != res.Count) bad.Add("绑定号有重复");
        return bad;
    }

    // 第一个函数体的第一个 OpLabel 之后插一条 OpCopyObject %指针类型 %新 id %变量（新 id 取头里的 bound）
    static byte[] InjectCopy(byte[] spv, string varName)
    {
        var w = new uint[spv.Length / 4]; Buffer.BlockCopy(spv, 0, w, 0, w.Length * 4);
        var names = new Dictionary<uint, string>(); var vars = new Dictionary<uint, uint>(); int label = -1; bool inFn = false;
        for (int i = 5; i < w.Length;)
        {
            int n = (int)(w[i] >> 16), op = (int)(w[i] & 0xffff);
            if (op == 5) { var bytes = new List<byte>(); for (int k = i + 2; k < i + n; k++) bytes.AddRange(BitConverter.GetBytes(w[k])); int z = bytes.IndexOf(0); names[w[i + 1]] = System.Text.Encoding.UTF8.GetString(bytes.Take(z < 0 ? bytes.Count : z).ToArray()); }
            if (op == 59 && n >= 4) vars[w[i + 2]] = w[i + 1];
            if (op == 54) inFn = true;
            if (op == 248 && inFn && label < 0) label = i + n;
            i += n;
        }
        uint vid = names.First(kv => kv.Value == varName && vars.ContainsKey(kv.Key)).Key, id = w[3];
        var list = w.ToList(); list[3] = id + 1;
        list.InsertRange(label, new[] { (4u << 16) | 83, vars[vid], id, vid });
        var b = new byte[list.Count * 4]; Buffer.BlockCopy(list.ToArray(), 0, b, 0, b.Length);
        return b;
    }

    // ⑪ 用：往第一个函数体插一条 OpExtInst，它的指令号（字面量）恰好等于 varName 变量的 id——变量并没被引用，只是数值撞上
    static byte[] InjectExtInstLiteral(byte[] spv, string varName)
    {
        var w = new uint[spv.Length / 4]; Buffer.BlockCopy(spv, 0, w, 0, w.Length * 4);
        var names = new Dictionary<uint, string>(); var vars = new HashSet<uint>(); int label = -1; bool inFn = false; uint ext = 0;
        for (int i = 5; i < w.Length;)
        {
            int n = (int)(w[i] >> 16), op = (int)(w[i] & 0xffff);
            if (op == 5) { var bytes = new List<byte>(); for (int k = i + 2; k < i + n; k++) bytes.AddRange(BitConverter.GetBytes(w[k])); int z = bytes.IndexOf(0); names[w[i + 1]] = System.Text.Encoding.UTF8.GetString(bytes.Take(z < 0 ? bytes.Count : z).ToArray()); }
            if (op == 11 && ext == 0) ext = w[i + 1];   // OpExtInstImport
            if (op == 59 && n >= 4) vars.Add(w[i + 2]);
            if (op == 54) inFn = true;
            if (op == 248 && inFn && label < 0) label = i + n;
            i += n;
        }
        uint vid = names.First(kv => kv.Value == varName && vars.Contains(kv.Key)).Key, id = w[3];
        var list = w.ToList(); list[3] = id + 1;
        list.InsertRange(label, new[] { (6u << 16) | 12, 1u, id, ext == 0 ? 1u : ext, vid, 1u });   // 类型 / 操作数随便给：这里只看 BindToArgIds 怎么解析
        var b = new byte[list.Count * 4]; Buffer.BlockCopy(list.ToArray(), 0, b, 0, b.Length);
        return b;
    }

    static int bad;
    static void Check(string what, bool ok, string detail = "")
    {
        Console.WriteLine((ok ? "✓ " : "✗ ") + what + (ok || detail.Length == 0 ? "" : "：" + detail));
        if (!ok) bad = 1;
    }

    static int Main(string[] args)
    {
        if (!((args.Length == 5 || (args.Length == 7 && args[5] == "--emit")) && args[3] == "--dsx")) { Console.Error.WriteLine("用法：dsh_vk_test <spirv-cross> <spirv-val> <工作目录> --dsx <libdsx> [--emit <目录>]"); return 2; }
        string val = args[1], root = args[2], tables = Path.Combine(root, "tables"), outDir = Path.Combine(root, "out"), dsx = args[4];
        NativeLibrary.SetDllImportResolver(typeof(NxgalSpvCross).Assembly, (name, asm, path) => name == "dsx" ? NativeLibrary.Load(dsx) : IntPtr.Zero);
        Directory.CreateDirectory(tables); Directory.CreateDirectory(outDir);
        File.WriteAllText(Path.Combine(tables, "shader_facts.tsv"), ""); File.WriteAllText(Path.Combine(tables, "shader_pairs.tsv"), "");
        var xc = new CliSpvCross(args[0], root);
        var keys = new List<(string Key, string Stage, byte[] Code, byte[] Ctl)>();
        foreach (var (stage, code, ctl) in Fixtures())
        {
            string key = Convert.ToHexStringLower(SHA256.HashData(ctl.AsSpan(0, 0x880)))[..16];
            Console.WriteLine($"{stage} {key}: {string.Join(" ", DshVk.CompileWithSpv(key, stage, code, ctl, outDir, xc, tables, true, root))}");
            keys.Add((key, stage, code, ctl));
        }

        // ① 键集合
        var jsonKeys = Directory.GetFiles(outDir, "*_b.json").Select(p => Path.GetFileName(p)[..^7]).OrderBy(k => k).ToList();
        var spvKeys = Directory.GetFiles(outDir, "*_b.spv").Select(p => Path.GetFileName(p)[..^6]).OrderBy(k => k).ToList();
        Check($"① _b.spv 键集合 = _b.json 键集合（{spvKeys.Count} 个）", jsonKeys.SequenceEqual(spvKeys) && spvKeys.Count == keys.Count, string.Join(",", jsonKeys) + " / " + string.Join(",", spvKeys));

        int rawDiff = 0;
        var perStage = new Dictionary<string, (Dictionary<string, (uint Set, uint Binding, bool Used)> Res, JsonArray Bindings)>();
        foreach (var (key, stage, code, ctl) in keys)
        {
            var metadata = JsonNode.Parse(File.ReadAllText(Path.Combine(outDir, key + "_b.json")));
            var bindings = metadata["bindings"].AsArray();
            string spvPath = Path.Combine(outDir, key + "_b.spv");
            byte[] spv = File.ReadAllBytes(spvPath);
            var res = Resources(spv);
            var mm = Mismatch(res, bindings, StageNoOf[stage]);
            perStage[stage] = (res, bindings);
            Check($"② {stage} 每个资源变量 (set, binding) = (0, arg_id × 6 + {StageNoOf[stage]})：{string.Join(" ", res.Select(r => r.Key + "=" + r.Value.Binding))}", res.Count > 0 && mm.Count == 0, string.Join("；", mm));
            if (stage == "ps")
            {
                var tex = bindings.OfType<JsonObject>().FirstOrDefault(o => (string)o["kind"] == "tex");
                var smp = tex == null ? null : bindings.OfType<JsonObject>().FirstOrDefault(o => (string)o["msl_name"] == (string)tex["msl_name"] + "Smplr");
                Check("② ps 组合纹理变量的 binding = texture 成员的 arg_id × 6 + 4（≠ …Smplr 的）",
                    tex != null && smp != null && (long)tex["arg_id"] != (long)smp["arg_id"] && res.TryGetValue((string)tex["msl_name"], out var t) && t.Binding == (long)tex["arg_id"] * 6 + 4,
                    tex == null ? "json 里没有 tex 成员" : smp == null ? "json 里没有 …Smplr 成员" : "");
            }
            var p = Process.Start(new ProcessStartInfo(val) { ArgumentList = { "--target-env", "vulkan1.1", spvPath }, RedirectStandardError = true, RedirectStandardOutput = true });
            string err = p.StandardError.ReadToEnd() + p.StandardOutput.ReadToEnd(); p.WaitForExit();
            Check($"③ {stage} spirv-val --target-env vulkan1.1", p.ExitCode == 0, err.Trim());

            var raw = DshCompiler.TranslateForVulkan(stage, code, ctl, root).Spv;   // 空表，现编路径，未改写绑定
            rawDiff += Mismatch(Resources(raw), bindings, StageNoOf[stage]).Count;

            if (stage == "vs")
            {
                uint mask = (uint?)metadata["ssbo_window_mask"] ?? 0;
                Check("顶点出包将驱动地址与可替换 SSBO 窗口掩码相与",
                    (int?)metadata["ssbo_window_abi"] == 1 && mask != 0 && UsesStorageWindowMask(spv, mask));
                Check("顶点的驱动常量段 c0 在绑定表中且入口取用",
                    bindings.OfType<JsonObject>().Any(o => (string)o["kind"] == "drvcb" && (int?)o["bank"] == 0 &&
                        res.TryGetValue((string)o["msl_name"], out var driver) && driver.Used));
            }
            if (stage == "ps")
                Check("不访问 SSBO 的片元仍声明窗口协议，无掩码", (int?)metadata["ssbo_window_abi"] == 1 &&
                    metadata["ssbo_window_mask"] == null);
            if (stage == "cs")
                Check("计算出包携带窗口合同：协议 1、掩码参与按位与、c0 绑定在表里且入口取用",
                    (int?)metadata["ssbo_window_abi"] == 1 && (uint?)metadata["ssbo_window_mask"] is uint cmask && cmask != 0 &&
                    UsesStorageWindowMask(spv, cmask) &&
                    bindings.OfType<JsonObject>().Any(o => (string)o["kind"] == "drvcb" && (int?)o["bank"] == 0 &&
                        res.TryGetValue((string)o["msl_name"], out var driver) && driver.Used));

            if (stage == "ps")
            {
                var name = res.Keys.First(n => bindings.OfType<JsonObject>().Any(o => (string)o["msl_name"] == n && (string)o["kind"] == "tex"));
                var drop = new JsonArray(bindings.OfType<JsonObject>().Where(o => (string)o["msl_name"] != name).Select(o => o.DeepClone()).ToArray());
                string why = null;
                try { DshVk.BindToArgIds(raw, drop, "ps"); } catch (DshError e) { why = e.Detail; }
                Check("⑤ json 删掉纹理成员 → BindToArgIds 报红", why != null && why.Contains(name), why ?? "没有报错");
                var moved = bindings.DeepClone().AsArray();
                var o0 = moved.OfType<JsonObject>().First(o => (string)o["msl_name"] == name);
                o0["arg_id"] = (long)o0["arg_id"] + 40;
                var shifted = Resources(DshVk.BindToArgIds(raw, moved, "ps"));
                Check("⑥ json 改纹理成员的 arg_id → 改写结果跟着变、对原 json 判不一致",
                    shifted[name].Binding == (long)o0["arg_id"] * 6 + 4 && Mismatch(shifted, bindings, 4).Count == 1);
                string why7 = null;
                try { DshVk.BindToArgIds(InjectCopy(raw, "support_buffer"), bindings, "ps"); } catch (DshError e) { why7 = e.Detail; }
                Check("⑦ json 里没有的 support_buffer 经 OpCopyObject 取用 → 报「用到了却没有成员」", why7 != null && why7.Contains("support_buffer") && why7.Contains("用到"), why7 ?? "没有报错（被挪到空号）");
                string why11 = null;
                try { DshVk.BindToArgIds(InjectExtInstLiteral(raw, "support_buffer"), bindings, "ps"); } catch (DshError e) { why11 = e.Detail; }
                Check("⑪ 函数体里 OpExtInst 的指令号字面量恰好等于 support_buffer 的 id（没被引用）→ 不报红、照旧挪到空号", why11 == null, why11 ?? "");
            }
        }
        Check($"④ 翻译器原样的 .spv 对不上 arg_id 的资源数 > 0（{rawDiff} 个）", rawDiff > 0);

        // ⑧ vs + ps 合进一个管线布局（安卓按 (set, binding) 合并）：期望值手算，arg_id 0 / 1 → vs 0 / 6、ps 4 / 10
        long BindOf(string st, long arg)
        {
            var (res, bindings) = perStage[st];
            var name = bindings.OfType<JsonObject>().Where(o => (long)o["arg_id"] == arg).Select(o => (string)o["msl_name"]).FirstOrDefault(res.ContainsKey);
            return name == null ? -1 : res[name].Binding;
        }
        var merged = perStage["vs"].Res.Values.Concat(perStage["ps"].Res.Values).ToList();
        Check($"⑧ vs + ps 合并布局不撞号：vs arg_id 0 / 1 → {BindOf("vs", 0)} / {BindOf("vs", 1)}（期望 0 / 6），ps → {BindOf("ps", 0)} / {BindOf("ps", 1)}（期望 4 / 10），{merged.Count} 个绑定号互不相同",
            BindOf("vs", 0) == 0 && BindOf("vs", 1) == 6 && BindOf("ps", 0) == 4 && BindOf("ps", 1) == 10
            && merged.All(r => r.Set == 0) && merged.Select(r => r.Binding).Distinct().Count() == merged.Count
            && perStage["vs"].Res.Values.All(r => r.Binding % 6 == 0) && perStage["ps"].Res.Values.All(r => r.Binding % 6 == 4));

        // ⑨ 出包模式：Compile 的产物在 PackFiles、不落盘；CompileWithSpv 从那里取 json，_b.spv 进 PackSpv
        var (pk, pst, pcode, pctl) = keys.First(k => k.Stage == "ps");
        string packDir = Path.Combine(root, "packout"), why9 = "";
        List<string> packed = null;
        DshCompiler.PackOut = true;
        try { packed = DshVk.CompileWithSpv(pk, pst, pcode, pctl, packDir, xc, tables, true, root); }
        catch (Exception e) { why9 = e.GetType().Name + ": " + e.Message; }
        finally { DshCompiler.PackOut = false; }
        byte[] inMem = DshVk.PackSpv?.GetValueOrDefault(pk + "_b.spv");
        Check("⑨ 出包模式（PackOut）：输出目录不建、json 取自 PackFiles、_b.spv 进 PackSpv 且与落盘那份逐字节相同",
            packed != null && packed.Contains(pk + "_b.spv") && !Directory.Exists(packDir) && DshCompiler.PackFiles?.ContainsKey(pk + "_b.json") == true
            && inMem != null && inMem.AsSpan().SequenceEqual(File.ReadAllBytes(Path.Combine(outDir, pk + "_b.spv"))), why9);

        // ⑬ 非缓冲存储图像的只读按位读：OpTypeImage 的 Image Format 按出库目标档案答（#1774 ② / #490275）。
        //   SULD.D.2D.B32 / B64 只读不写——目标档案有 shaderStorageImageReadWithoutFormat（imgload_raw=true）时声明 Unknown，
        //   与现编（DirectGpuAccessor 按设备真能力答）同口径；最低档案（缺省 false）声明具体格式，任何设备都合法。
        //   修前声明 R32Uint / R32G32Uint，安卓回放 image_view_format 按声明字节数与绑定视图比对 → 尺寸不合整笔 dispatch 被 skip
        //   （8E post_tonemap_update b56206eabdd3e4df / set_texture_alpha 6ee1fa73e2b1b220 同型实机证：compute_image_decl_format_size_mismatch 每帧一条，tonemap 不落 → 世界黑）。
        var imgHead = new byte[0x100]; BitConverter.TryWriteBytes(imgHead.AsSpan(0), 0x12345679u);
        foreach (var (shape, size, mark) in new[] { ("SULD.D.2D.B32（post_tonemap_update 形）", 4, (byte)9), ("SULD.D.2D.B64（set_texture_alpha 形）", 5, (byte)10) })
        {
            var ictl = new byte[0x880]; ictl[0x10] = mark;
            var icode = Bundles(imgHead, new[] { SuldD(4, 0, 1, 3, size), EXIT });
            var ifmts = ImageFormats(DshCompiler.TranslateForVulkan("cs", icode, ictl, root, imgLoadRaw: true).Spv);
            Check($"⑬ cs {shape}（目标档案 imgload_raw=true）：存储图像 OpTypeImage Format = Unknown（{ifmts.Count} 个声明）",
                ifmts.Count > 0 && ifmts.All(f => f.Fmt == 0),
                string.Join(",", ifmts.Select(f => $"dim{f.Dim} sampled{f.Sampled} fmt{f.Fmt}")));
            var ifmtsFloor = ImageFormats(DshCompiler.TranslateForVulkan("cs", icode, ictl, root).Spv);
            Check($"⑬ cs {shape}（最低档案缺省 imgload_raw=false）：OpTypeImage Format = 具体格式（{ifmtsFloor.Count} 个声明）",
                ifmtsFloor.Count > 0 && ifmtsFloor.All(f => f.Fmt != 0),
                string.Join(",", ifmtsFloor.Select(f => $"dim{f.Dim} sampled{f.Sampled} fmt{f.Fmt}")));
        }
        {
            // ⑬ 守卫：同槽位有写（ImageStore 用途并集）时照旧必须具体格式——Raw 放行只放松只读绑定，不能顺手把写绑定也放成 Unknown
            var ictl = new byte[0x880]; ictl[0x10] = 11;
            var icode = Bundles(imgHead, new[] { SuldD(4, 0, 1, 3, 4), SustD(4, 0, 1, 3, 4), EXIT });
            var ifmts = ImageFormats(DshCompiler.TranslateForVulkan("cs", icode, ictl, root, imgLoadRaw: true).Spv);
            Check($"⑬ cs 读写同槽（SULD.D + SUST.D.2D.B32，imgload_raw=true）：OpTypeImage Format 仍 = 具体格式（{ifmts.Count} 个声明）",
                ifmts.Count > 0 && ifmts.All(f => f.Fmt != 0),
                string.Join(",", ifmts.Select(f => $"dim{f.Dim} sampled{f.Sampled} fmt{f.Fmt}")));
        }

        // ⑩ 几何 / 曲面外壳 / 曲面域：不出 .spv，明确报红（在 Compile 之前）
        var (_, _, vcode, vctl) = keys.First(k => k.Stage == "vs");
        var refused = new List<string>();
        foreach (var st in new[] { "gs", "hs", "ds" })
        {
            try { DshVk.CompileWithSpv("refuse_" + st, st, vcode, vctl, Path.Combine(root, "refuse"), xc, tables, true, root); }
            catch (DshError) { refused.Add(st); }
        }
        Check($"⑩ gs / hs / ds 调 CompileWithSpv 报 DshError（{string.Join(" ", refused)}）", refused.Count == 3);

        // ⑫ 原生几何阶段：CompileGeometry 出点 / 线 / 三角三份
        var (gcode, gctl) = GeometryFixture(false);
        string gkey = Convert.ToHexStringLower(SHA256.HashData(gctl.AsSpan(0, 0x880)))[..16];
        Dictionary<string, byte[]> gfiles = null; string gwhy = "";
        try { gfiles = DshVk.CompileGeometry(gkey, gcode, gctl, xc, tables, true, root); }
        catch (Exception e) { gwhy = e is DshError de ? de.Status + ": " + de.Detail : e.GetType().Name + ": " + e.Message; }
        var gwant = new[] { 0, 1, 3 }.SelectMany(t => new[] { $"{gkey}_vg{t}.json", $"{gkey}_vg{t}.spv" }).OrderBy(n => n, StringComparer.Ordinal).ToList();
        Check($"⑫ CompileGeometry 出 6 个文件（_vg0 / _vg1 / _vg3 各 json + spv）", gfiles != null && gfiles.Keys.OrderBy(n => n, StringComparer.Ordinal).SequenceEqual(gwant),
            gfiles == null ? gwhy : string.Join(",", gfiles.Keys));
        var modeOf = new Dictionary<int, uint> { [0] = 19, [1] = 20, [3] = 22 };   // InputPoints / InputLines / Triangles
        foreach (int t in new[] { 0, 1, 3 })
        {
            if (gfiles == null || !gfiles.ContainsKey($"{gkey}_vg{t}.json")) break;
            var gj = JsonNode.Parse(gfiles[$"{gkey}_vg{t}.json"]).AsObject();
            var gb = gj["bindings"].AsArray();
            byte[] gspv = gfiles[$"{gkey}_vg{t}.spv"];
            var w = new uint[gspv.Length / 4]; Buffer.BlockCopy(gspv, 0, w, 0, w.Length * 4);
            uint model = uint.MaxValue; var modes = new List<uint>();
            for (int i = 5; i < w.Length;)
            {
                int n = (int)(w[i] >> 16), op = (int)(w[i] & 0xffff);
                if (n == 0) break;
                if (op == 15 && n >= 2) model = w[i + 1];             // OpEntryPoint：执行模型
                if (op == 16 && n >= 3) modes.Add(w[i + 2]);          // OpExecutionMode：模式
                i += n;
            }
            bool cb = gb.OfType<JsonObject>().Any(o => (string)o["kind"] == "cbuf" && (long?)o["reg"] == 0);
            bool tx = gb.OfType<JsonObject>().Any(o => (string)o["kind"] == "tex" && (long?)o["reg"] == 0);
            Check($"⑫ _vg{t}.json：stage gs、dm_ctl = 键、gs_input_topology = {t}、反射有 cbuf reg 0 与 tex reg 0",
                (string)gj["stage"] == "gs" && (string)gj["dm_ctl"] == gkey && (long?)gj["gs_input_topology"] == t && cb && tx, gj.ToJsonString());
            Check($"⑫ _vg{t}.spv：入口执行模型 Geometry（3）、输入图元模式 {modeOf[t]}", model == 3 && modes.Contains(modeOf[t]) && modes.Count(m => m is 19 or 20 or 21 or 22 or 23) == 1,
                $"model {model} modes {string.Join(",", modes)}");
            var gres = Resources(gspv);
            var gmm = Mismatch(gres, gb, 3);
            Check($"⑫ _vg{t}.spv 每个资源变量 (set, binding) = (0, arg_id × 6 + 3)：{string.Join(" ", gres.Select(r => r.Key + "=" + r.Value.Binding))}", gres.Count >= 2 && gmm.Count == 0, string.Join("；", gmm));
            Check($"⑫ 反证：_vg{t}.spv 按阶段号 4 判必不一致", Mismatch(gres, gb, 4).Count > 0);
            string gp = Path.Combine(root, $"gs_vg{t}.spv"); File.WriteAllBytes(gp, gspv);
            var vp = Process.Start(new ProcessStartInfo(val) { ArgumentList = { "--target-env", "vulkan1.1", gp }, RedirectStandardError = true, RedirectStandardOutput = true });
            string verr = vp.StandardError.ReadToEnd() + vp.StandardOutput.ReadToEnd(); vp.WaitForExit();
            Check($"⑫ _vg{t}.spv spirv-val --target-env vulkan1.1", vp.ExitCode == 0, verr.Trim());
        }
        var (pcode2, pctl2) = GeometryFixture(true);
        string pwhy = null;
        try { DshVk.CompileGeometry("pt" + gkey[2..], pcode2, pctl2, xc, tables, true, root); } catch (DshError e) { pwhy = e.Detail; }
        Check("⑫ 直通型几何头：GeometrySkip 给原因、CompileGeometry 报 DshError（离线不出，设备现编）",
            DshVk.GeometrySkip(pcode2, pctl2) is string sk && sk.Contains("passthrough") && pwhy != null && pwhy.Contains("passthrough") && DshVk.GeometrySkip(gcode, gctl) == null, pwhy ?? "没有报错");
        // ⑭ 窗口读回夹具料：给了 --emit 才出（android/tests/vk/standalone/libpack-ssbo-window.sh 在 lavapipe 上读回字节）
        if (args.Length == 7)
        {
            var (wcode, wctl) = WindowFixture();
            string wkey = Convert.ToHexStringLower(SHA256.HashData(wctl.AsSpan(0, 0x880)))[..16], emit = args[6];
            Directory.CreateDirectory(emit);
            // 普通编译请求从 0x50 字节的 Maxwell 阶段头开始，库工具 JobFor 另消费前面的 0x30 字节。
            File.WriteAllBytes(Path.Combine(emit, "window_vs.bin"), wcode.AsSpan(0x30).ToArray());
            DshVk.CompileWithSpv(wkey, "vs", wcode, wctl, emit, xc, tables, true, root);
            var wj = JsonNode.Parse(File.ReadAllText(Path.Combine(emit, wkey + "_b.json")));
            byte[] wspv = File.ReadAllBytes(Path.Combine(emit, wkey + "_b.spv"));
            uint wmask = (uint?)wj["ssbo_window_mask"] ?? 0;
            Check("⑭ 读回夹具 VS：窗口协议 1、有掩码且参与按位与",
                (int?)wj["ssbo_window_abi"] == 1 && wmask != 0 && UsesStorageWindowMask(wspv, wmask));
            File.WriteAllText(Path.Combine(emit, "window_vs.key"), wkey);
            var (wpcode, wpctl) = WindowFixturePs();
            string wpkey = Convert.ToHexStringLower(SHA256.HashData(wpctl.AsSpan(0, 0x880)))[..16];
            DshVk.CompileWithSpv(wpkey, "ps", wpcode, wpctl, emit, xc, tables, true, root);
            var wpj = JsonNode.Parse(File.ReadAllText(Path.Combine(emit, wpkey + "_b.json")));
            uint wpmask = (uint?)wpj["ssbo_window_mask"] ?? 0;
            Check("⑭ 读回夹具 PS：窗口协议 1、有掩码且参与按位与、两个存储缓冲槽",
                (int?)wpj["ssbo_window_abi"] == 1 && wpmask != 0 && UsesStorageWindowMask(File.ReadAllBytes(Path.Combine(emit, wpkey + "_b.spv")), wpmask) &&
                wpj["bindings"].AsArray().OfType<JsonObject>().Count(o => (string)o["kind"] == "sbuf") == 2);
            File.WriteAllText(Path.Combine(emit, "window_ps.key"), wpkey);
            var (ccode, cctl) = WindowFixtureCs();
            string ckey = Convert.ToHexStringLower(SHA256.HashData(cctl.AsSpan(0, 0x880)))[..16];
            DshVk.CompileWithSpv(ckey, "cs", ccode, cctl, emit, xc, tables, true, root);
            File.WriteAllText(Path.Combine(emit, "window_cs.key"), ckey);
        }
        Console.WriteLine(bad == 0 ? "dsh_vk: 全过" : "dsh_vk: 有不通");
        return bad;
    }
}
