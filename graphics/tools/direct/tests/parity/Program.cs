// 出库 × 现编双翻等价比较器（#1836 PR-1 门禁件）。
//
// 口径：同一合成请求（tests/parity_corpus/requests/*.json）过两条翻译路——
//   现编：DirectShaderCompiler.Compile（设备档案注入请求 profile 段）；
//   出库：DshVk.CompileWithSpv（请求 code 段补 0x30 包记录前缀、ctl 原样、空事实表——与真出库一致）。
// 比对 .spv 的接口事实面：能力 / 图像类型 / 执行模式 / IO 位置 / 描述符计数（按 storage class；
// 绑定编号不归一化——出库稀疏 arg*6+4 与现编紧凑属既定形态差）/ cs Workgroup 字节。
// vs 输出槽不按集合相等判（#488979 ①）：现编按快照全槽声明+常量填充，出库只声明写到的槽；
// 判等规则 = 出库 vs 输出 ⊇（现编非常量 store 的语义写入集 ∪ 同语料 fs 输入集），SelfTest 带负控。
// 性能线索（不在本件改）：现编 vs 满 32 槽声明在 Mali 类设备上可能多占 varying 带宽，待真机量化后单列。
// 差异行：<语料>\<设备>\<阶段>\<字段>\t<细节>；命中 whitelist.tsv 报过不判红；
// 未被消费的 whitelist 行本身报红（白名单只减不增）。
//
// 出库侧的 imgload_raw 与现编同一来源：组档后的 profile.capabilities.supportsImageLoadFormatted
// 逐轮传给 CompileWithSpv（#1774 ② / #490275）——8E/9000 档两轮两侧都声明 Unknown，最低档
// （s2a-device-caps-min.json 合入 devices 轮）两侧都声明具体格式，接口事实面按轮判等。
//
// 用法：parity <corpus_dir> <devices_dir|-> <spirv-cross> <work_dir> --dsx <libdsx>

using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using S2A.DirectCompiler;
using S2A.Dsh;

internal static class Program
{
    static int Main(string[] args)
    {
        if (!(args.Length == 4 || args.Length == 6 && args[4] == "--dsx"))
        { Console.Error.WriteLine("usage: parity <corpus> <devices|-> <spirv-cross> <work> [--dsx <libdsx>]"); return 2; }
        if (args.Length == 6)
            NativeLibrary.SetDllImportResolver(typeof(NxgalSpvCross).Assembly, (n, a, h) => n == "dsx" ? NativeLibrary.Load(args[5]) : IntPtr.Zero);
        SelfTest();
        string corpus = Path.GetFullPath(args[0]), work = Path.GetFullPath(args[3]);
        var devices = args[1] == "-" ? new List<(string, byte[])>()
            : Directory.GetFiles(Path.GetFullPath(args[1]), "*.json").OrderBy(x => x, StringComparer.Ordinal)
                .Select(p => (Path.GetFileNameWithoutExtension(p), File.ReadAllBytes(p))).ToList();
        if (Directory.Exists(work)) Directory.Delete(work, true);
        Directory.CreateDirectory(work);
        // 出库侧事实表：与真出库同口径——空表（出库链对缺失事实用无界缺省答）。
        string tables = Path.Combine(work, "tables");
        Directory.CreateDirectory(tables);
        foreach (var n in new[] { "shader_facts.tsv", "shader_pairs.tsv", "shader_map.tsv" }) File.WriteAllText(Path.Combine(tables, n), "");
        Directory.CreateDirectory(Path.Combine(work, "xc"));
        var xc = new CliSpvCross(Path.GetFullPath(args[2]), Path.Combine(work, "xc"));

        var whitelist = new HashSet<string>(StringComparer.Ordinal);
        var wlUsed = new HashSet<string>(StringComparer.Ordinal);
        string wlPath = Path.Combine(corpus, "whitelist.tsv");
        if (File.Exists(wlPath))
            foreach (var ln in File.ReadLines(wlPath))
            { var t = ln.TrimEnd(); if (t.Length == 0 || t.StartsWith('#')) continue; whitelist.Add(t.Split('\t')[0]); }

        int pass = 0, fail = 0;
        foreach (var reqFile in Directory.GetFiles(Path.Combine(corpus, "requests"), "*.json").OrderBy(x => x, StringComparer.Ordinal))
        {
            string name = Path.GetFileNameWithoutExtension(reqFile);
            if (name == "profile") continue;
            var req = JsonNode.Parse(File.ReadAllText(reqFile));
            string closure = req["translatorClosure"].GetValue<string>();
            var rounds = devices.Count > 0
                ? devices.Select(d => (d.Item1, (JsonNode)JsonNode.Parse(DirectProfileComposer.Compose(d.Item2, closure)))).ToList()
                : new List<(string, JsonNode)> { ("request", req["profile"]?.DeepClone()) };
            foreach (var (dev, prof) in rounds)
            {
                var r2 = (JsonObject)req.DeepClone();
                if (prof != null) r2["profile"] = prof.DeepClone();
                // 出库按所用档案答 Raw 读位：与现编同一份组档 profile 取 supportsImageLoadFormatted。
                bool imgLoadRaw = prof?["capabilities"]?["supportsImageLoadFormatted"]?.GetValue<bool>() ?? false;
                var diffs = Compare(name, dev, r2, xc, tables, work, imgLoadRaw);
                foreach (var d in diffs)
                {
                    if (whitelist.Contains(d.Key)) { wlUsed.Add(d.Key); pass++; Console.WriteLine("ok-wl " + d.Line); }
                    else { fail++; Console.WriteLine("DIFF " + d.Line); }
                }
                if (diffs.Count == 0) { pass++; Console.WriteLine($"ok {name}\\{dev}：接口事实全等"); }
            }
        }
        foreach (var w in whitelist) if (!wlUsed.Contains(w)) { fail++; Console.WriteLine("STALE-WL " + w); }
        Console.WriteLine($"结果：{pass} 绿 / {fail} 红");
        return fail == 0 ? 0 : 1;
    }

    static List<(string Key, string Line)> Compare(string name, string dev, JsonObject req, CliSpvCross xc, string tables, string work, bool imgLoadRaw)
    {
        var diffs = new List<(string, string)>();
        void Add(string stage, string field, string detail) =>
            diffs.Add(($"{name}\\{dev}\\{stage}\\{field}", $"{name}\\{dev}\\{stage}\\{field}\t{detail}"));

        var rtByStage = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        try
        {
            var rt = JsonDocument.Parse(DirectShaderCompiler.Compile(JsonSerializer.SerializeToUtf8Bytes(req)));
            string rd = Path.Combine(work, "rt-" + name + "-" + dev); Directory.CreateDirectory(rd);
            foreach (var s in rt.RootElement.GetProperty("content").GetProperty("stages").EnumerateArray())
            {
                var spv = s.GetProperty("spirv").GetBytesFromBase64();
                rtByStage[s.GetProperty("stage").GetString()] = spv;
                File.WriteAllBytes(Path.Combine(rd, s.GetProperty("stage").GetString() + ".spv"), spv);
            }
        }
        catch (Exception e) { Add("all", "compile-rt", Short(e)); return diffs; }

        var exByStage = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        int si = 0;
        try
        {
            string od = Path.Combine(work, "ex-" + name + "-" + dev);
            Directory.CreateDirectory(od);
            foreach (var s in req["program"]["stages"].AsArray())
            {
                string st = s["stage"].GetValue<string>(), ex = st == "fs" ? "ps" : st;
                byte[] code = Convert.FromBase64String(s["code"].GetValue<string>()), ctl = Convert.FromBase64String(s["control"].GetValue<string>());
                // NVN1 包记录码：图形 0x30 前缀（magic 0x12345678）+ 自带 SPH 的码；计算 0x100 头（magic 0x12345679）+ 码（同 dsh_vklib 夹具）
                int head = ex == "cs" ? 0x100 : 0x30;
                byte[] rec = new byte[head + code.Length];
                System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(rec, ex == "cs" ? 0x12345679u : 0x12345678u);
                code.CopyTo(rec, head);
                string key = Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(
                    rec.Concat(ctl).Concat(System.Text.Encoding.ASCII.GetBytes(ex)).ToArray()))[..16];
                si++;
                DshVk.CompileWithSpv(key, ex, rec, ctl, od, xc, tables, imgLoadRaw, work);
                exByStage[st] = File.ReadAllBytes(Path.Combine(od, key + "_b.spv"));
            }
        }
        catch (Exception e) { Add("all", "compile-ex", Short(e)); return diffs; }

        foreach (var (st, rtSpv) in rtByStage)
        {
            if (!exByStage.TryGetValue(st, out var exSpv)) { Add(st, "stage-missing-ex", "出库无该阶段产物"); continue; }
            var a = Surface.Of(rtSpv); var b = Surface.Of(exSpv);
            if (a.Caps != b.Caps) Add(st, "caps", $"rt=[{a.Caps}] ex=[{b.Caps}]");
            if (a.Images != b.Images) Add(st, "image-types", $"rt=[{a.Images}] ex=[{b.Images}]");
            if (a.ExecModes != b.ExecModes) Add(st, "exec-modes", $"rt=[{a.ExecModes}] ex=[{b.ExecModes}]");
            if (st == "vs")
            {
                // #488979 口径①：vs 输出声明面不强求集合相等（现编按快照全槽、出库只声明写到的槽）。
                // 判等规则：出库 vs 输出 ⊇ 实际写入集 ∪ 同语料 fs 输入集（两路 fs 输入在上面对过集合相等）。
                if (!a.Ins.SetEquals(b.Ins)) Add(st, "io-in", $"rt=[{Seq(a.Ins)}] ex=[{Seq(b.Ins)}]");
                var fsIns = rtByStage.ContainsKey("fs") ? Surface.Of(rtByStage["fs"]).Ins : new SortedSet<uint>();
                var missing = VsOutMissing(a, b, fsIns);
                if (missing.Count != 0) Add(st, "io-out", $"ex 缺 out[{Seq(missing)}]（写入集/同语料 fs 输入要求），rt-out=[{Seq(a.Outs)}] ex-out=[{Seq(b.Outs)}]");
            }
            else if (a.Io != b.Io) Add(st, "io", $"rt=[{a.Io}] ex=[{b.Io}]");
            if (a.Desc != b.Desc) Add(st, "descriptors", $"rt=[{a.Desc}] ex=[{b.Desc}]");
            if (st == "cs" && a.Wg != b.Wg) Add(st, "workgroup-bytes", $"rt={a.Wg} ex={b.Wg}");
        }
        foreach (var st in exByStage.Keys) if (!rtByStage.ContainsKey(st)) Add(st, "stage-missing-rt", "现编无该阶段产物");
        return diffs;
    }

    // #488979 口径①判等规则：出库 vs 输出集必须盖住 {实际写入集 ∪ 同语料 fs 输入集}；返回缺的槽（空 = 判等）
    static SortedSet<uint> VsOutMissing(Surface rtVs, Surface exVs, SortedSet<uint> fsIns)
    {
        var need = new SortedSet<uint>(rtVs.Writes); need.UnionWith(fsIns);
        need.ExceptWith(exVs.Outs);
        return need;
    }

    // 自检（每轮必跑，坏规则 = 门禁白跑）：出库漏一个 fs 读的槽必须判差；盖住写入集+fs 输入的少声明版必须判等。
    static void SelfTest()
    {
        var rt = new Surface(); rt.Writes.UnionWith(new uint[] { 0, 1 }); rt.Outs.UnionWith(new uint[] { 0, 1 });
        var ins = new SortedSet<uint> { 1 };
        var okEx = new Surface(); okEx.Outs.UnionWith(new uint[] { 0, 1, 2 });
        var badEx = new Surface(); badEx.Outs.UnionWith(new uint[] { 0 });   // fs 读 1，出库没声明
        if (VsOutMissing(rt, okEx, ins).Count != 0) throw new InvalidOperationException("vs io 规则自检①失败：盖住写入集+fs 输入的出库版被判差");
        if (VsOutMissing(rt, badEx, ins).Count != 1) throw new InvalidOperationException("vs io 规则自检②失败：出库漏 fs 读的槽没判差");
    }

    static string Seq(IEnumerable<uint> xs) => string.Join(',', xs.OrderBy(x => x));

    static string Short(Exception e)
    {
        var m = (e.GetType().Name + ":" + e.Message).Replace('\t', ' ').Replace('\n', ' ');
        return m.Length > 160 ? m[..160] : m;
    }

    // SPIR-V 接口事实面：绑定编号归一化为按 storage class 计数。
    sealed class Surface
    {
        public string Caps, Images, ExecModes, Io, Desc;
        public long Wg;
        public SortedSet<uint> Ins = new(), Outs = new(), Writes = new();
        public HashSet<uint> Consts = new();

        public static Surface Of(byte[] spv)
        {
            var s = new Surface();
            if (spv.Length < 20) return s;
            var u = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, uint>(spv);
            var types = new Dictionary<uint, (int op, uint[] p)>();   // OpType*/OpConstant 结果 id → 指令
            var vars = new Dictionary<uint, (uint type, uint cls)>();
            var dec = new Dictionary<uint, Dictionary<uint, uint>>(); // target → decoration → 参数
            var caps = new SortedSet<uint>(); var imgs = new SortedSet<string>(); var modes = new SortedSet<string>();
            int i = 5;
            while (i < u.Length)
            {
                uint w = u[i]; int op = (int)(w & 0xffff), n = (int)(w >> 16);
                if (n == 0 || i + n > u.Length) break;
                var p = new uint[n - 1]; for (int k = 1; k < n; k++) p[k - 1] = u[i + k];
                switch (op)
                {
                    case 17: caps.Add(p[0]); break;
                    case 16: modes.Add(p[1] + (p.Length > 2 ? "(" + string.Join(",", p[2..]) + ")" : "")); break;
                    case 59: vars[p[1]] = (p[0], p[2]); break;
                    case 71:
                        var d = dec.TryGetValue(p[0], out var m) ? m : dec[p[0]] = new();
                        if (p.Length >= 3) d[p[1]] = p[2];
                        break;
                    case >= 19 and <= 33: case 43:
                        types[op is >= 19 and <= 33 ? p[0] : p[1]] = (op, p);   // OpType* 无结果类型；OpConstant 有
                        break;
                }
                i += n;
            }
            var locs = new SortedSet<string>(); var desc = new SortedDictionary<uint, int>();
            foreach (var (v, (ty, cls)) in vars)
            {
                bool bound = dec.TryGetValue(v, out var dd) && dd.ContainsKey(34);
                if (bound && cls is 0 or 2 or 12) desc[cls] = desc.GetValueOrDefault(cls) + 1;
                if (cls is 1 or 3 && dec.TryGetValue(v, out var dl) && dl.TryGetValue(30, out var l)) locs.Add((cls == 1 ? "in" : "out") + l);
                if (cls == 4) s.Wg += SizeOf(ty, types);
            }
            foreach (var (id, (op, p)) in types) if (op == 25) imgs.Add($"dim{p[2]}/s{p[6]}/f{p[7]}");
            s.Caps = string.Join(',', caps); s.Images = string.Join(',', imgs); s.ExecModes = string.Join(',', modes);
            s.Io = string.Join(',', locs);
            s.Desc = string.Join(',', desc.Select(kv => kv.Key + "x" + kv.Value));
            // vs 判等规则用：分侧位置集 + 实际写入集（OpStore / OpAccessChain 基变量是 out 变量的 Location）
            var ins = new SortedSet<uint>(); var outs = new SortedSet<uint>(); var baseOf = new Dictionary<uint, uint>();
            i = 5;
            while (i < u.Length)
            {
                uint w = u[i]; int op = (int)(w & 0xffff), n = (int)(w >> 16);
                if (n == 0 || i + n > u.Length) break;
                if (op == 65 && n >= 4) baseOf[u[i + 2]] = u[i + 3];            // OpAccessChain 结果 → 基变量
                if (op is 43 or 44 or 46 && n >= 3) s.Consts.Add(u[i + 2]);      // OpConstant/Composite/Null 结果 id
                if (op == 1) s.Consts.Add(u[i + 2]);                             // OpUndef 也是非语义值
                if (op == 62 && n >= 3 && !s.Consts.Contains(u[i + 2]))          // OpStore：常量填充不算语义写
                {
                    uint ptr = u[i + 1];
                    while (baseOf.TryGetValue(ptr, out var b)) ptr = b;
                    if (vars.TryGetValue(ptr, out var vv) && vv.cls == 3 && dec.TryGetValue(ptr, out var vd) && vd.TryGetValue(30, out var wl))
                        s.Writes.Add(wl);
                }
                i += n;
            }
            foreach (var (v, (ty, cls)) in vars)
                if (cls is 1 or 3 && dec.TryGetValue(v, out var dl) && dl.TryGetValue(30, out var l)) (cls == 1 ? ins : outs).Add(l);
            s.Ins = ins; s.Outs = outs;
            return s;
        }

        static long SizeOf(uint typeId, Dictionary<uint, (int op, uint[] p)> types, int depth = 0)
        {
            if (depth > 8 || !types.TryGetValue(typeId, out var t)) return 0;
            return t.op switch
            {
                32 => SizeOf(t.p[2], types, depth + 1),                                  // pointer → pointee
                28 => SizeOf(t.p[1], types, depth + 1) * Const(t.p[2], types),           // array → elem × len
                29 => SizeOf(t.p[1], types, depth + 1),                                  // runtime array → 元素大小（长未知，按 1 计）
                30 => t.p[1..].Aggregate(0L, (a, m) => a + SizeOf(m, types, depth + 1)), // struct 成员和
                23 => SizeOf(t.p[1], types, depth + 1) * t.p[2],                         // vector → elem × count
                21 or 22 => t.p[1] / 8,                                                  // int/float width
                20 => 4,
                _ => 0,
            };
        }

        static long Const(uint id, Dictionary<uint, (int op, uint[] p)> types)
            => types.TryGetValue(id, out var c) && c.op == 43 && c.p.Length >= 3 ? c.p[2] : 0;
    }
}
