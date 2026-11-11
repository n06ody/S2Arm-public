// Original synthetic Maxwell programs for the DshVk/DirectCompiler comparison.
using S2A.Dsh;
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

static class Program
{
    const ulong Exit = 0xe30000000007000fUL;
    static ulong Ldc(int rd, int slot, int off, int size) => 0xef90000000000000UL | ((ulong)size << 48) | ((ulong)slot << 36) | ((ulong)off << 20) | (7UL << 16) | (255UL << 8) | (ulong)rd;
    static ulong Stg(int rs, int ra, int off) => 0xeedc200000000000UL | ((ulong)off << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rs;
    static ulong Mov(int rd, uint value) => 0x0100000000000000UL | ((ulong)value << 20) | (7UL << 16) | (15UL << 12) | (ulong)rd;
    static ulong Tex(int rd, int ra, int rb, int tid) => 0xd800000000000000UL | (1UL << 53) | ((ulong)tid << 36) | (255UL << 28) | ((ulong)rb << 20) | (7UL << 16) | ((ulong)ra << 8) | (ulong)rd;
    static byte[] Code(uint header, bool fragment, ulong[] ops)
    {
        int n = (ops.Length + 2) / 3 * 3;
        var code = new byte[0x80 + n / 3 * 32];
        BitConverter.TryWriteBytes(code.AsSpan(0x30), header);
        if (fragment) code[0x30 + 72] = 15;
        for (int i = 0; i < n; ++i)
            BitConverter.TryWriteBytes(code.AsSpan(0x80 + (i / 3 * 4 + i % 3 + 1) * 8), i < ops.Length ? ops[i] : Exit);
        return code;
    }
    static void Main(string[] args)
    {
        if (args.Length != 3) throw new ArgumentException("<spirv-cross> <libdsx> <work-dir>");
        string root = args[2], tables = Path.Combine(root, "tables"), output = Path.Combine(root, "out");
        Directory.CreateDirectory(tables); Directory.CreateDirectory(output);
        File.WriteAllText(Path.Combine(tables, "shader_facts.tsv"), "");
        File.WriteAllText(Path.Combine(tables, "shader_pairs.tsv"), "");
        NativeLibrary.SetDllImportResolver(typeof(NxgalSpvCross).Assembly, (name, asm, path) => name == "dsx" ? NativeLibrary.Load(args[1]) : IntPtr.Zero);
        var cross = new CliSpvCross(args[0], root);
        const uint store = (1u << 16) | (1u << 26);
        var vs = Code(1 | (3 << 5) | (1 << 10) | store, false, new[] {
            Ldc(0, 0, 0x110, 5), Ldc(2, 3, 0, 4), Stg(2, 0, 0),
            Ldc(4, 1, 0, 4), Stg(4, 0, 4), Ldc(6, 0, 0, 4), Stg(6, 0, 8), Exit });
        var ps = Code(2 | (3 << 5) | (5 << 10), true, new[] {
            Mov(0, 0x3f000000), Mov(1, 0x3f000000), Tex(0, 0, 1, 8),
            Ldc(1, 3, 0, 4), Mov(2, 0), Mov(3, 0x3f800000), Exit });
        foreach (var (stage, code, mark) in new[] { ("vs", vs, 1), ("ps", ps, 2) })
        {
            var ctl = new byte[0x880]; ctl[0x10] = (byte)mark;
            byte[] c1 = Array.Empty<byte>();
            if (stage == "vs") {
                c1 = new byte[0x100];
                for (int i = 0; i < c1.Length; ++i) c1[i] = (byte)(i * 17 + 3);
                c1.CopyTo(ctl, 0x760);
                BitConverter.TryWriteBytes(ctl.AsSpan(0x700), (uint)(code.Length + 0x760));
                BitConverter.TryWriteBytes(ctl.AsSpan(0x704), (uint)(code.Length + 0x860));
            }
            string key = Convert.ToHexStringLower(SHA256.HashData(ctl))[..16];
            DshVk.CompileWithSpv(key, stage, code, ctl, output, cross, tables, root);
            File.WriteAllBytes(Path.Combine(root, stage + ".code"), code.AsSpan(0x30).ToArray());
            File.WriteAllBytes(Path.Combine(root, stage + ".ctl"), ctl);
            File.WriteAllBytes(Path.Combine(root, stage + ".c1"), c1);
        }
    }
}
