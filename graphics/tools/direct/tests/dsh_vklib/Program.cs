// #783 dsh_vklib 单测（t_dsh_vklib.sh 调）：把原创合成着色器（VS / PS / CS，同 dsh_vk/Program.cs 的夹具）打成假着色器包
//   （每个 <阶段>_0.bin 一串 "BTCDBTCD" 记录，+0x28 控制段字节数、+0x2c 代码字节数、代码从 +0x38 起）和配置档（shader_map.tsv + 空答案表），整批走 VkLib.Build。
//   期望按卡面写死，不跑被测实现推：
//   ① vk_build.tsv：vs / ps / cs 三键 ok，真几何键（同 dsh_vk ⑫ 的手写 GS）ok，几何包里放顶点代码的键 skipped（几何头阶段号不是 4），记录长度越界的键 failed，共 6 行；
//   ② 输出目录只有三键各自的 _b.json 与 _b.spv、几何键的 _vg0 / _vg1 / _vg3 各 json + spv（加 vk_build.tsv），没有 _b.metal；
//   ③g 几何键的 6 个文件与直接调 DshVk.CompileGeometry 的逐字节相同；
//   ③ 每个 _b.spv 与同一夹具直接调 DshVk.CompileWithSpv（落盘模式，#1254 已有单测）出的逐字节相同，_b.json 的 bindings 相同；
//   ④ 每个 _b.spv 过 spirv-val --target-env vulkan1.1；
//   ⑥ 编进工具的源码部分 VkFp.V 与完整指纹 VkLib.Fp 都是 16 位十六进制、两者不同，汇总行带完整指纹与按阶段分列的 skipped；
//      打 VKFP_SRC= / VKFP= / XFP= 三行，由 t_dsh_vklib.sh 与 VkFpPrint、按公式重算的值比（指纹随源码 / 翻译器变的核对在脚本里）；
//   ⑩ VkLib.Pack 把三键的产物打成离线 Vulkan 库（LibPack 容器）：按宿主格式（graphics/nvn/lib_pack.inc 文件头）自己解析 lib.idx——魔数、版本 1、链号，
//      12 条记录按 (键, 种类) 升序、_b.json 种类 1 / _b.spv 种类 52、_vg<t>.json 种类 112+t / _vg<t>.spv 种类 120+t，每段字节 = 逐键文件，
//      头部 36–43 = 完整指纹 VkLib.Fp（u64 小端）、44–47 为 0；同一目录再打一次报红。
// 用法：dsh_vklib_test <spirv-cross> <spirv-val> <工作目录> --dsx <libdsx>
using S2A.Dsh;
using S2A.DshVkLib;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

static class Program
{
    // ---- 夹具：与 dsh_vk/Program.cs Fixtures() 同一组原创指令 ----
    const ulong EXIT = 0xe30000000007000fUL;
    static ulong Mov32i(int rd, uint v) => 0x0100000000000000UL | ((ulong)v << 20) | (7UL << 16) | (15UL << 12) | (ulong)rd;
    static ulong Ldc(int rd, int slot, int off, int size) => 0xef90000000000000UL | ((ulong)size << 48) | ((ulong)slot << 36) | ((ulong)off << 20) | (7UL << 16) | (255UL << 8) | (ulong)rd;
    static ulong Ldg32(int rd, int ra, int off) => 0xeed4200000000000UL | ((ulong)off << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;
    static ulong Stg32(int rs, int ra, int off) => 0xeedc200000000000UL | ((ulong)off << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rs;
    static ulong Texs2D(int rd, int ra, int rb, int tid) => 0xd800000000000000UL | (1UL << 53) | ((ulong)tid << 36) | (255UL << 28) | ((ulong)rb << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;
    static byte[] Bundles(byte[] head, ulong[] ops)
    {
        int n = (ops.Length + 2) / 3 * 3; var b = new byte[head.Length + n / 3 * 32]; head.CopyTo(b, 0);
        for (int i = 0; i < n; i++) BitConverter.TryWriteBytes(b.AsSpan(head.Length + (i / 3 * 4 + i % 3 + 1) * 8), i < ops.Length ? ops[i] : EXIT);
        return b;
    }
    static byte[] Sph(uint word0, int omapByte)
    {
        var h = new byte[0x30 + 0x50]; BitConverter.TryWriteBytes(h.AsSpan(0x30), word0);
        if (omapByte >= 0) h[0x30 + 72] = (byte)omapByte;
        return h;
    }
    static (string Stage, byte[] Code, byte[] Ctl)[] Fixtures()
    {
        const uint store = (1u << 16) | (1u << 26);
        var vs = Bundles(Sph(1 | (3 << 5) | (1 << 10) | store, -1), new[] { Ldc(0, 0, 0x110, 5), Ldc(2, 3, 0, 4), Stg32(2, 0, 0), EXIT });
        var ps = Bundles(Sph(2 | (3 << 5) | (5 << 10), 15), new[] { Mov32i(0, 0x3f000000), Mov32i(1, 0x3f000000), Texs2D(0, 0, 1, 8), Ldc(1, 3, 0, 4), Mov32i(2, 0), Mov32i(3, 0x3f800000), EXIT });
        var csHead = new byte[0x100]; BitConverter.TryWriteBytes(csHead.AsSpan(0), 0x12345679u);
        var cs = Bundles(csHead, new[] { Ldc(0, 0, 0x310, 5), Ldg32(2, 0, 0), Ldc(3, 3, 0, 4), Stg32(3, 0, 4), Stg32(2, 0, 8), EXIT });
        (string, byte[], byte[]) F(string st, byte[] code, byte mark) { var ctl = new byte[0x880]; ctl[0x10] = mark; return (st, code, ctl); }
        return new[] { F("vs", vs, 1), F("ps", ps, 2), F("cs", cs, 3) };
    }
    // 几何夹具：同 dsh_vk/Program.cs GeometryFixture（ALD / AST / OUT 编码见那里）
    static ulong Ald(int rd, int attr, int alSize) => (0b1110111111011UL << 51) | ((ulong)alSize << 47) | (255UL << 39) | ((ulong)attr << 20) | (7UL << 16) | (255UL << 8) | (ulong)rd;
    static ulong Ast(int rs, int attr, int alSize) => (0b1110111111110UL << 51) | ((ulong)alSize << 47) | (255UL << 39) | ((ulong)attr << 20) | (7UL << 16) | (255UL << 8) | (ulong)rs;
    static ulong Out(int type) => (0b1111101111100UL << 51) | ((ulong)type << 39) | (255UL << 20) | (7UL << 16) | (255UL << 8) | 255UL;
    static (byte[] Code, byte[] Ctl) GeometryFixture()
    {
        var h = Sph(1u | (3u << 5) | (4u << 10), -1);
        BitConverter.TryWriteBytes(h.AsSpan(0x30 + 8), 1u << 24); BitConverter.TryWriteBytes(h.AsSpan(0x30 + 12), 7u << 24); BitConverter.TryWriteBytes(h.AsSpan(0x30 + 16), 3u);
        var code = Bundles(h, new[] { Ald(0, 0x70, 3), Mov32i(6, 0x3f000000), Mov32i(7, 0x3f000000), Texs2D(5, 6, 7, 8), Ldc(4, 3, 0, 4),
                                      Ast(0, 0x70, 3), Ast(4, 0x80, 0), Ast(5, 0x84, 0), Out(1), Out(2), EXIT });
        var ctl = new byte[0x880]; ctl[0x10] = 6;
        return (code, ctl);
    }
    static byte[] Record(byte[] code, byte[] ctl, uint? fakeCode = null)
    {
        var h = new byte[0x38]; Encoding.ASCII.GetBytes("BTCDBTCD").CopyTo(h, 0);
        BitConverter.TryWriteBytes(h.AsSpan(0x28), (uint)ctl.Length); BitConverter.TryWriteBytes(h.AsSpan(0x2c), fakeCode ?? (uint)code.Length);
        return h.Concat(code).Concat(ctl).ToArray();
    }
    static string Key(byte[] ctl) => Convert.ToHexStringLower(SHA256.HashData(ctl.AsSpan(0, 0x880)))[..16];

    static int bad;
    static void Check(string what, bool ok, string detail = "")
    {
        Console.WriteLine((ok ? "✓ " : "✗ ") + what + (ok || detail.Length == 0 ? "" : "：" + detail));
        if (!ok) bad = 1;
    }

    static int Main(string[] args)
    {
        if (!(args.Length == 5 && args[3] == "--dsx")) { Console.Error.WriteLine("用法：dsh_vklib_test <spirv-cross> <spirv-val> <工作目录> --dsx <libdsx>"); return 2; }
        string val = args[1], root = args[2], dsx = args[4];
        NativeLibrary.SetDllImportResolver(typeof(NxgalSpvCross).Assembly, (name, asm, path) => name == "dsx" ? NativeLibrary.Load(dsx) : IntPtr.Zero);
        string pkg = Path.Combine(root, "pkg"), prof = Path.Combine(root, "profile"), outDir = Path.Combine(root, "out"), refDir = Path.Combine(root, "ref"), work = Path.Combine(root, "work");
        foreach (var d in new[] { pkg, prof, refDir, work }) Directory.CreateDirectory(d);
        File.WriteAllText(Path.Combine(prof, "shader_facts.tsv"), ""); File.WriteAllText(Path.Combine(prof, "shader_pairs.tsv"), "");
        var fx = Fixtures(); var map = new List<string>(); var expect = new Dictionary<string, string>();
        foreach (var (stage, code, ctl) in fx)
        {
            File.WriteAllBytes(Path.Combine(pkg, stage + "_0.bin"), Record(code, ctl));
            map.Add($"{stage}_0.bin\t0\t-\t{Key(ctl)}\t-"); expect[Key(ctl)] = "ok";
        }
        var gsCtl = fx[0].Ctl.ToArray(); gsCtl[0x10] = 4;   // gs 包里放的是顶点代码（几何头阶段号不是 4）：不该翻
        File.WriteAllBytes(Path.Combine(pkg, "gs_0.bin"), Record(fx[0].Code, gsCtl)); map.Add($"gs_0.bin\t0\t-\t{Key(gsCtl)}\t-"); expect[Key(gsCtl)] = "skipped";
        var (gcode, gctl) = GeometryFixture(); string gk = Key(gctl);   // 真几何着色器：原生几何阶段三份
        File.WriteAllBytes(Path.Combine(pkg, "gs_1.bin"), Record(gcode, gctl)); map.Add($"gs_1.bin\t0\t-\t{gk}\t-"); expect[gk] = "ok";
        var gnames = new[] { 0, 1, 3 }.SelectMany(t => new[] { $"{gk}_vg{t}.json", $"{gk}_vg{t}.spv" }).ToList();
        var bkCtl = fx[0].Ctl.ToArray(); bkCtl[0x10] = 5;   // 代码字节数写大：记录越界
        File.WriteAllBytes(Path.Combine(pkg, "vs_1.bin"), Record(fx[0].Code, bkCtl, 0x100000)); map.Add($"vs_1.bin\t0\t-\t{Key(bkCtl)}\t-"); expect[Key(bkCtl)] = "failed";
        File.WriteAllLines(Path.Combine(prof, "shader_map.tsv"), map);

        var xc = new CliSpvCross(args[0], work);
        var rows = VkLib.Build(pkg, prof, outDir, xc, work, imgLoadRaw: true);   // 目标档案 true：与下面两处参照调用同档（false = 最低档案形态另由 dsh_vk ⑬ 覆盖）
        var tsv = File.ReadAllLines(Path.Combine(outDir, "vk_build.tsv")).Select(l => l.Split('\t')).ToDictionary(c => c[0], c => c[2]);
        Check($"① vk_build.tsv：{tsv.Count} 行，vs / ps / cs / 真几何 ok、顶点代码的 gs skipped、越界 failed", tsv.Count == 6 && expect.All(kv => tsv.GetValueOrDefault(kv.Key) == kv.Value),
            string.Join(" ", tsv.Select(kv => kv.Key + "=" + kv.Value)));
        var files = Directory.GetFiles(outDir).Select(Path.GetFileName).OrderBy(x => x, StringComparer.Ordinal).ToList();
        var want = fx.SelectMany(f => new[] { Key(f.Ctl) + "_b.json", Key(f.Ctl) + "_b.spv" }).Concat(gnames).Append("vk_build.tsv").OrderBy(x => x, StringComparer.Ordinal).ToList();
        Check($"② 输出目录只有三键的 _b.json / _b.spv、几何键的 6 个 _vg 文件和 vk_build.tsv（{files.Count} 个），没有 _b.metal", files.SequenceEqual(want), string.Join(",", files));
        var gref = DshVk.CompileGeometry(gk, gcode, gctl, xc, prof, true, work);
        Check("③g 几何键 6 个 _vg 文件与直接调 CompileGeometry 的逐字节相同", gref.Count == 6 && gnames.All(n => File.Exists(Path.Combine(outDir, n)) && File.ReadAllBytes(Path.Combine(outDir, n)).AsSpan().SequenceEqual(gref[n])));

        foreach (var (stage, code, ctl) in fx)
        {
            string k = Key(ctl);
            if (!File.Exists(Path.Combine(outDir, k + "_b.spv")) || !File.Exists(Path.Combine(outDir, k + "_b.json"))) { Check($"③ {stage} 输出里有 _b.spv / _b.json", false, "缺"); continue; }
            DshVk.CompileWithSpv(k, stage, code, ctl, refDir, xc, prof, true, work);   // 落盘模式（DshCompiler.PackOut 已由 Build 复原）
            byte[] a = File.ReadAllBytes(Path.Combine(outDir, k + "_b.spv")), b = File.ReadAllBytes(Path.Combine(refDir, k + "_b.spv"));
            var ja = JsonNode.Parse(File.ReadAllText(Path.Combine(outDir, k + "_b.json")))["bindings"]?.ToJsonString();
            var jb = JsonNode.Parse(File.ReadAllText(Path.Combine(refDir, k + "_b.json")))["bindings"]?.ToJsonString();
            Check($"③ {stage} _b.spv 与直接 CompileWithSpv 逐字节相同（{a.Length} 字节）、bindings 相同", a.AsSpan().SequenceEqual(b) && ja != null && ja == jb);
            var p = Process.Start(new ProcessStartInfo(val) { ArgumentList = { "--target-env", "vulkan1.1", Path.Combine(outDir, k + "_b.spv") }, RedirectStandardError = true, RedirectStandardOutput = true });
            string err = p.StandardError.ReadToEnd() + p.StandardOutput.ReadToEnd(); p.WaitForExit();
            Check($"④ {stage} spirv-val --target-env vulkan1.1", p.ExitCode == 0, err.Trim());
        }
        Check($"⑤ Build 返回 {rows.Count} 行，与 vk_build.tsv 一致", rows.Count == tsv.Count && rows.All(r => tsv.GetValueOrDefault(r.Key) == r.Status));
        static bool Hex16(string x) => x.Length == 16 && x.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
        Check($"⑥ 源码部分 VkFp.V（{VkFp.V}）与完整指纹 VkLib.Fp（{VkLib.Fp}）都是 16 位十六进制、两者不同", Hex16(VkFp.V) && Hex16(VkLib.Fp) && VkFp.V != VkLib.Fp);
        string sum = VkLib.Summary(rows);
        Check($"⑥ 汇总行带完整指纹与按阶段分列的 skipped（{sum}）", sum == $"dsh_vklib (vkfp {VkLib.Fp}): 6 keys, ok 4, skipped 1 (gs 1), failed 1");
        Console.WriteLine("VKFP_SRC=" + VkFp.V); Console.WriteLine("VKFP=" + VkLib.Fp); Console.WriteLine("XFP=" + DshCompiler.Xfp);
        string lib = Path.Combine(root, "b-0123456789ab-vk"); var (pb, pl) = VkLib.Pack(outDir, lib, "0123456789ab");
        Check($"⑩ 四键打成离线 Vulkan 库（{(pb.Count > 0 ? string.Join("；", pb) : pl)}）", pb.Count == 0 && Directory.GetFiles(lib).Select(Path.GetFileName).OrderBy(x => x, StringComparer.Ordinal).SequenceEqual(new[] { "lib.000.pak", "lib.idx" }));
        if (pb.Count == 0)
        {
            byte[] idx = File.ReadAllBytes(Path.Combine(lib, "lib.idx")), pak = File.ReadAllBytes(Path.Combine(lib, "lib.000.pak"));
            uint n = BitConverter.ToUInt32(idx, 12), ns = BitConverter.ToUInt32(idx, 16);
            Check($"⑩ 头：魔数 S2ALIB、版本 1、链号 0123456789ab、{n} 条记录、{ns} 个分片", idx.AsSpan(0, 8).SequenceEqual("S2ALIB\0\0"u8) && BitConverter.ToUInt32(idx, 8) == 1 && Encoding.ASCII.GetString(idx, 24, 12) == "0123456789ab" && n == 12 && ns == 1 && idx.Length == 48 + 16 * ns + 24 * n);
            Check($"⑩ 头部 36–43 = 完整指纹 {VkLib.Fp}（u64 小端）、44–47 为 0", BitConverter.ToUInt64(idx, 36) == Convert.ToUInt64(VkLib.Fp, 16) && BitConverter.ToUInt32(idx, 44) == 0);
            var wantRec = fx.Select(f => (Key(f.Ctl), new[] { (1, "_b.json"), (52, "_b.spv") }))
                .Append((gk, new[] { (112, "_vg0.json"), (113, "_vg1.json"), (115, "_vg3.json"), (120, "_vg0.spv"), (121, "_vg1.spv"), (123, "_vg3.spv") }))
                .OrderBy(e => Convert.ToUInt64(e.Item1, 16)).SelectMany(e => e.Item2.Select(r => (e.Item1, r.Item1, r.Item2))).ToList();
            var got = Enumerable.Range(0, (int)n).Select(i => 48 + 16 * (int)ns + 24 * i).Select(o => (Key: BitConverter.ToUInt64(idx, o), Kind: (int)idx[o + 8], Len: BitConverter.ToUInt32(idx, o + 12), Off: BitConverter.ToUInt64(idx, o + 16))).ToList();
            Check("⑩ 12 条记录按 (键, 种类) 升序、_b.json 种类 1 / _b.spv 种类 52、_vg<t>.json 112+t / _vg<t>.spv 120+t、每段字节 = 逐键文件",
                got.Count == wantRec.Count && got.Zip(wantRec).All(x => x.First.Key == Convert.ToUInt64(x.Second.Item1, 16) && x.First.Kind == x.Second.Item2
                    && pak.AsSpan((int)x.First.Off, (int)x.First.Len).SequenceEqual(File.ReadAllBytes(Path.Combine(outDir, x.Second.Item1 + x.Second.Item3)))));
            var (pb2, _) = VkLib.Pack(outDir, lib, "0123456789ab");
            Check($"⑩ 同一目录再打一次 → 报红（{string.Join("；", pb2)}）", pb2.Count > 0);
        }
        Console.WriteLine(bad == 0 ? "dsh_vklib 逐键产物：全过" : "dsh_vklib 逐键产物：有不通");
        return bad;
    }
}
