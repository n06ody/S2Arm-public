// #783 出包 Vulkan 产物的独立入口（dsh_vklib）。直连出库（dsh_batch build）同一份输入，读包 / 读表与它共用同一份 PkgRead（dsh/batch/PkgRead.cs，经工程文件编进来）：
//   着色器包目录（每个 <阶段>*.bin 是一串以 "BTCDBTCD" 起头的记录；记录 +0x28 = 控制段字节数、+0x2c = 代码字节数，代码从 +0x38 起、控制段紧随其后），
//   配置档目录（shader_map.tsv 每行：包文件名 \t 记录序号 \t … \t 键 \t …；shader_facts.tsv / shader_pairs.tsv 等答案表，同 DshTables）。
// 逐键（按键名排序）走 DshVk.CompileWithSpv（DshCompiler.PackOut：产物留内存），只把 <key>_b.json 与 <key>_b.spv 写进输出目录（临时名再改名），
//   另写 vk_build.tsv（键 \t 阶段 \t 状态 \t 原因）。几何着色器走 DshVk.CompileGeometry（原生几何阶段，每种输入图元一份 <key>_vg<t>.json + .spv；
//   直通型几何头不出，记 skipped）；hs / ds 不出 .spv，记 skipped；其余报红记 failed，不中止整批。
// Vulkan 侧指纹（直连线 11-02 答 a、#1419 审 1）：VkLib.Fp = 源码部分 VkFp.V（VkFp.targets：DshVk.cs、本目录、libdsx 导出接口、SPIRV-Cross 链锁与源码）
//   与翻译器指纹 DshCompiler.Xfp（S2A.Dsh 的 DshXfpInput：翻译器本体）合成的 16 位十六进制，翻译器一变它就变；汇总行带上，dsh_vklib --fp 只打印它。
// 离线 Vulkan 库容器（#783 直连线答 a、合入线答 ①）：给了 --lib <目录> --chain <链号> 就把逐键产物打成 LibPack 容器（lib.idx + lib.NNN.pak，_b.json 种类 1 / _b.spv 种类 52，
//   几何的 _vg<t>.json 种类 112+t / _vg<t>.spv 种类 120+t，头部 36–43 记 VkLib.Fp），写进 <目录>（出包时是 shaders/direct/b-<链号>-vk/）；逐键产物与 vk_build.tsv 仍在 --out。
// 跨次复用（11-24）：给了 --cas <缓存仓根> 就把逐键成功产物存进与苹果整份出库同一个内容寻址仓（dsh/batch/Store.cs，仓根 <缓存仓根>/dshb，键前缀 vk/），
//   同一款重出包时键对上就直接取、不再翻译（原先每次出包整款逐键重翻，旷野之息约 17 分钟）。键 = 本工具身份 × 本键记录字节，本工具身份含：
//   完整指纹 VkLib.Fp（Vulkan 侧源码 + 翻译器 Xfp）、链号 DshChain、图像读档位、两个改产物的环境开关、配置档目录全部文件内容、libdsx 与 SPIRV-Cross 的功能身份（ToolLib.Fp）。
//   配置档整目录进键（不挑表）：DshVk 读哪张表都兜得住，代价只是改任一表全款重翻。只存 ok 的键，failed / skipped 每次照判。
// 用法：dsh_vklib --pkg <包目录> --profile <配置档目录> --out <输出目录> --spirv-cross <命令行> --dsx <libdsx> [--work <工作目录>] [--lib <库目录> --chain <链号>] [--cas <缓存仓根>] [--jobs <路数>]；dsh_vklib --fp
using S2A.Dsh;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace S2A.DshVkLib
{
    public static class VkLib
    {
        /// <summary>shader_map.tsv：键 → (包文件, 记录序号)，按键名排序；哪行算记录、怎么取同 dsh_batch LoadTables（PkgRead.IsRecord，序号 int.Parse）</summary>
        public static SortedDictionary<string, (string Pkg, int Idx)> Map(string profile)
        {
            var loc = new SortedDictionary<string, (string, int)>(StringComparer.Ordinal);
            foreach (var (_, c) in PkgRead.MapLines(Path.Combine(profile, "shader_map.tsv")))
                if (PkgRead.IsRecord(c)) loc.TryAdd(c[3], (c[0], int.Parse(c[1])));
            return loc;
        }

        /// <summary>完整的 Vulkan 侧指纹：SHA-256("vkfp 1\0" + VkFp.V + "\0" + DshCompiler.Xfp) 取前 16 位十六进制</summary>
        public static string Fp => Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes("vkfp 1\0" + VkFp.V + "\0" + DshCompiler.Xfp)))[..16];

        public sealed class Row { public string Key, Stage, Status, Why; }

        /// <summary>本次复用计数：命中 = 从缓存仓取出、没翻译；翻译 = 实际翻了的键（含翻译失败的）</summary>
        public static int CacheHits, CacheMisses;

        static string Sha(byte[] b) => Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(b));
        static string Sha(string s) => Sha(Encoding.UTF8.GetBytes(s));

        /// <summary>配置档目录全部文件（不含点文件）按相对路径排序，逐个「路径 \0 内容摘要」拼起来取摘要</summary>
        public static string ProfileDigest(string profile)
        {
            var sb = new StringBuilder("profile 1\0");
            foreach (var f in Directory.EnumerateFiles(profile, "*", SearchOption.AllDirectories)
                         .Select(f => (Rel: Path.GetRelativePath(profile, f).Replace('\\', '/'), Full: f))
                         .Where(x => !x.Rel.Split('/').Any(c => c.StartsWith('.')))
                         .OrderBy(x => x.Rel, StringComparer.Ordinal))
                sb.Append(f.Rel).Append('\0').Append(Sha(File.ReadAllBytes(f.Full))).Append('\n');
            return Sha(sb.ToString());
        }

        /// <summary>本工具身份（同一款重出包时键对上的前提，任一项变就全款重翻）；tools = 调用方给的原生工具身份（libdsx、SPIRV-Cross 的 ToolLib.Fp）</summary>
        public static string ToolId(string profile, bool imgLoadRaw, string tools) => Sha(string.Join("\0", "dshvk-cache 1", Fp, DshCompiler.Chain, imgLoadRaw ? "1" : "0",
            "strip_interlock=" + Environment.GetEnvironmentVariable("S2A_DIRECT_STRIP_INTERLOCK"), "bf=" + Environment.GetEnvironmentVariable("DMSH_BF"),
            ProfileDigest(profile), tools ?? ""));

        /// <summary>整批：返回逐键结果（ok / skipped / failed），产物写进 outDir。
        /// imgLoadRaw = 出库目标档案的 shaderStorageImageReadWithoutFormat：false = 最低档案（只读非缓冲图像声明具体格式，
        /// 任何设备都合法的 SPIR-V；不配设备的库取件在绑定核被拒、退回现编——#1850）。</summary>
        public static List<Row> Build(string pkg, string profile, string outDir, ISpvCross xc, string work, bool imgLoadRaw = false, string cas = null, string tools = null, int jobs = 1)
        {
            Directory.CreateDirectory(outDir); Directory.CreateDirectory(work);
            // 逐键互不依赖：键按名排好后并行翻（jobs 路，同 dsh_batch 的并行翻译）；结果按原顺序写回 rows，vk_build.tsv 与单路逐字节相同。
            //   每键的翻译状态（DshCompiler.PackFiles、DshVk.PackSpv、XIo.Mem）都是线程内的，SPIRV-Cross 命令行每次用独立临时名，缓存仓本来就支持并发写。
            //   着色器包记录先全部读进来（原先边翻边懒读，并行会抢同一张表）。
            var map = Map(profile).ToList();
            var packs = new Dictionary<string, List<byte[]>>(StringComparer.Ordinal);
            foreach (var (_, (pk0, _)) in map)
                if (!packs.ContainsKey(pk0)) packs[pk0] = File.Exists(Path.Combine(pkg, pk0)) ? PkgRead.Records(File.ReadAllBytes(Path.Combine(pkg, pk0))) : new List<byte[]>();
            var slots = new Row[map.Count];
            bool pack0 = DshCompiler.PackOut; DshCompiler.PackOut = true;
            CacheHits = CacheMisses = 0;
            string tid = null;
            if (cas != null) { Store.Open(Path.Combine(cas, "dshb")); tid = ToolId(profile, imgLoadRaw, tools); }
            try
            {
                System.Threading.Tasks.Parallel.For(0, map.Count, new System.Threading.Tasks.ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, jobs) }, n =>
                {
                    var (key, (pk, idx)) = map[n];
                    string stage = pk.Length >= 2 ? pk[..2] : pk;
                    var row = new Row { Key = key, Stage = stage, Status = "ok", Why = "" }; slots[n] = row;
                    var recs = packs[pk];
                    var cc = idx >= 0 && idx < recs.Count && PkgRead.Fits(recs[idx]) ? PkgRead.Split(recs[idx]) : ((byte[] Code, byte[] Ctl)?)null;   // 越界记 failed（dsh_batch 那边不核，坏记录整份出库停住）
                    if (cc == null) { row.Status = "failed"; row.Why = $"record {pk}#{idx} missing or truncated"; return; }
                    if (stage is "hs" or "ds") { row.Status = "skipped"; row.Why = stage + " 不出 .spv（DshVk）"; return; }
                    if (stage == "gs" && DshVk.GeometrySkip(cc.Value.Code, cc.Value.Ctl) is string gskip) { row.Status = "skipped"; row.Why = "gs: " + gskip; return; }
                    // 仓键：本工具身份 × 键名 × 包文件名 × 本键整条记录（代码 + 控制段）
                    string ck = tid == null ? null : "vk/" + Sha(tid + "\0" + key + "\0" + pk + "\0" + Sha(recs[idx]));
                    var hit = ck == null ? null : Store.Get(ck);
                    if (hit != null && hit.Count > 0)
                    {
                        foreach (var (name, bytes) in hit.OrderBy(x => x.Key, StringComparer.Ordinal)) Write(outDir, name, bytes);
                        System.Threading.Interlocked.Increment(ref CacheHits); return;
                    }
                    System.Threading.Interlocked.Increment(ref CacheMisses);
                    var made = new Dictionary<string, byte[]>(StringComparer.Ordinal);
                    try
                    {
                        if (stage == "gs")
                            foreach (var (name, bytes) in DshVk.CompileGeometry(key, cc.Value.Code, cc.Value.Ctl, xc, profile, imgLoadRaw, work)) made[name] = bytes;
                        else
                        {
                            DshVk.CompileWithSpv(key, stage, cc.Value.Code, cc.Value.Ctl, outDir, xc, profile, imgLoadRaw, work);
                            made[key + "_b.json"] = Encoding.UTF8.GetBytes(DshCompiler.PackFiles[key + "_b.json"]);
                            foreach (var (name, bytes) in DshVk.PackSpv) made[name] = bytes;
                        }
                        foreach (var (name, bytes) in made) Write(outDir, name, bytes);
                        if (ck != null && made.Count > 0) Store.Put(ck, made);
                    }
                    catch (DshError e) { row.Status = "failed"; row.Why = (e.Status + ": " + e.Detail).Replace('\t', ' ').Replace('\n', ' '); }
                    // 同 dsh_batch Translate：其它异常（如 libdsx 少导出、DllImport 找不到入口）也只记这一键失败，不中止整批
                    catch (Exception e) { row.Status = "failed"; row.Why = ("translation failed: " + e.GetType().Name + ": " + e.Message.Split('\n')[0]).Replace('\t', ' '); }
                });
            }
            finally { DshCompiler.PackOut = pack0; }
            var rows = slots.ToList();
            if (cas != null) Store.Compact();   // 给本次用到的条目刷新修改时间（容量预算 LRU 认它），不删条目
            File.WriteAllLines(Path.Combine(outDir, "vk_build.tsv"), rows.Select(r => $"{r.Key}\t{r.Stage}\t{r.Status}\t{r.Why}"));
            return rows;
        }

        /// <summary>把 keysDir 里逐键的反射、SPIR-V、片元输出变体打成离线 Vulkan 库，头部记完整指纹 Fp。libDir 里已有 lib.idx 报红。</summary>
        public static (List<string> Bad, string Line) Pack(string keysDir, string libDir, string chain)
        {
            Directory.CreateDirectory(libDir);
            var vg = new System.Text.RegularExpressions.Regex(@"_vg\d\.(json|spv)\z", System.Text.RegularExpressions.RegexOptions.CultureInvariant);
            var names = Directory.EnumerateFiles(keysDir).Select(Path.GetFileName)
                .Where(n => !n.StartsWith("._", StringComparison.Ordinal) &&
                    (n.EndsWith("_b.json", StringComparison.Ordinal) || n.EndsWith("_b.spv", StringComparison.Ordinal) ||
                     n.EndsWith("_vf.bin", StringComparison.Ordinal) || vg.IsMatch(n))).ToList();
            return LibPack.PackFiles(names, n => File.ReadAllBytes(Path.Combine(keysDir, n)), libDir, chain, Fp);
        }

        /// <summary>汇总行：指纹、键数、ok / skipped（按阶段分列，如「gs 3, hs 1」，看覆盖面用）/ failed</summary>
        public static string Summary(List<Row> rows)
        {
            var sk = rows.Where(r => r.Status == "skipped").GroupBy(r => r.Stage).OrderBy(g => g.Key, StringComparer.Ordinal).Select(g => $"{g.Key} {g.Count()}");
            return $"dsh_vklib (vkfp {Fp}): {rows.Count} keys, ok {rows.Count(r => r.Status == "ok")}, skipped {rows.Count(r => r.Status == "skipped")} ({string.Join(", ", sk)}), failed {rows.Count(r => r.Status == "failed")}";
        }

        static void Write(string dir, string name, byte[] data)
        {
            string p = Path.Combine(dir, name), tmp = p + ".tmp-" + Environment.ProcessId;
            File.WriteAllBytes(tmp, data); File.Move(tmp, p, true);
        }
    }

    static class Program
    {
        static int Main(string[] args)
        {
            if (args.Length == 1 && args[0] == "--fp") { Console.WriteLine(VkLib.Fp); return 0; }
            var a = new Dictionary<string, string>();
            for (int i = 0; i + 1 < args.Length; i += 2) a[args[i]] = args[i + 1];
            foreach (var k in new[] { "--pkg", "--profile", "--out", "--spirv-cross", "--dsx" })
                if (!a.ContainsKey(k)) { Console.Error.WriteLine("用法：dsh_vklib --pkg <包目录> --profile <配置档目录> --out <输出目录> --spirv-cross <命令行> --dsx <libdsx> [--work <工作目录>] [--lib <库目录> --chain <链号>] [--imgload-raw 0|1]；dsh_vklib --fp"); return 2; }
            if (a.ContainsKey("--lib") != a.ContainsKey("--chain")) { Console.Error.WriteLine("--lib 与 --chain 要一起给"); return 2; }
            // --imgload-raw 1 = 出库目标档案有 shaderStorageImageReadWithoutFormat（只读非缓冲图像声明 Unknown）；缺省 0 = 最低档案，声明具体格式
            bool imgLoadRaw = a.GetValueOrDefault("--imgload-raw", "0") == "1";
            string dsx = a["--dsx"];
            NativeLibrary.SetDllImportResolver(typeof(NxgalSpvCross).Assembly, (name, asm, path) => name == "dsx" ? NativeLibrary.Load(dsx) : IntPtr.Zero);
            string work = a.GetValueOrDefault("--work", Path.Combine(a["--out"], ".work"));
            string cas = a.GetValueOrDefault("--cas");
            string tools = cas == null ? null : "dsx " + ToolLib.Fp(dsx) + "\0spirv-cross " + ToolLib.Fp(a["--spirv-cross"]);
            int jobs = a.TryGetValue("--jobs", out var js) && int.TryParse(js, out var jn) && jn > 0 ? jn : 1;   // 缺省单路（出包由 direct_lib.py 按本单核数给）
            var rows = VkLib.Build(a["--pkg"], a["--profile"], a["--out"], new CliSpvCross(a["--spirv-cross"], work), work, imgLoadRaw, cas, tools, jobs);
            Console.WriteLine(VkLib.Summary(rows));
            if (cas != null) Console.WriteLine($"dsh_vklib cache: hit {VkLib.CacheHits}, translated {VkLib.CacheMisses} (store {Path.Combine(cas, "dshb")})");
            // 个别键翻不了不拒绝整库：失败的键记在 vk_build.tsv（出库脚本按它报 🟡 缺口、keys.tsv 不列它们），运行时对缺的键现编；成功的照常打库发布。
            // 原先只要有 1 键失败就整库不出，手机上上万支全部退回现编（010028600EBDA000 的 11194 键里 17 键失败就是这样）
            int failed = rows.Count(row => row.Status == "failed");
            if (failed > 0) Console.Error.WriteLine($"[RED] Vulkan 翻译失败 {failed} 键（其余照常出库，这几键运行时现编）；详见 vk_build.tsv");
            if (failed > 0 && !rows.Any(row => row.Status == "ok")) return 1;   // 一键都没成：没有可发布的库
            if (a.TryGetValue("--lib", out var lib))
            {
                var (bad, line) = VkLib.Pack(a["--out"], lib, a["--chain"]);
                if (bad.Count > 0) { foreach (var b in bad) Console.Error.WriteLine("[RED] " + b); return 1; }
                Console.WriteLine(line);
            }
            return 0;
        }
    }
}
