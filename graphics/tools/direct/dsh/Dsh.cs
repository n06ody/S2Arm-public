// 直连兜底现编的主干：直连离线链 translator/dmsh_build.py 「单个键出库」那条路（job_for → dmsh_xlate → finish / geometry_finish）逐条移植。
// 每个函数名与离线链同名（Python 原文在 dmsh_build.py / dmsh_geometry.py），改动只在三处，都是运行时没有的东西：
//   · 不出 .metallib（iOS 上运行时编出的库存不成文件）：MTL.compile 那步不做，<键>_b.metal 由载入端 newLibraryWithSource 编；
//     随之「带层 / 整数输出入口编不过就去掉重编」的退路（finish 的 MTL.compile 失败分支）也不做——它要编过才知道；
//   · _bv（影子输入库，DshVin）/ _bp（拉取式顶点输入，DshPull）/ _bf（读帧缓冲版，DshFetch）/ _ft（输出类型变体，DshFt）只写源码 + 元数据（<键>_bv/_bf/_ft.metal + .json，Finish 末尾），不编、_b.json 不带 vin / fetch：
//     出包时由 Python 编排层编库并写回索引（同离线 vin_finish / fetch_finish / ft_finish）；载入端缺省时各有退路，见 DshCApi.cs 文件头；
//   · json 的 chain_metal 写本库的编法（载入端不读它），source / pc_paired 取 shader_map.tsv，表里没有这个键时写 "s2a_dsh" / null。
// 翻译器 dmsh_xlate/Program.cs 原样编进本程序集，进程内用临时目录写 job.json、反射调它的 Main。
using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using static S2A.Dsh.Py;

[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("dsh_cmp")]   // 对拍工具的逐函数对照（t_dsh_fn.py）
[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("dsh_batch")]   // 出包批量进程（batch/Program.cs）：设本款曲面模式表

namespace S2A.Dsh
{
    /// <summary>失败：Status 同离线链的分类（translation failed / MSL failed / geometry failed …），Detail 是原因</summary>
    public sealed class DshError : Exception
    {
        public readonly string Status, Detail;
        /// <summary>同一原因已报过红（缺表：每个键都会撞上同一份缺的表，只报第一次），导出入口照常返回失败、不再打红</summary>
        public bool Reported;
        public DshError(string status, string detail) : base(status + ": " + detail) { Status = status; Detail = detail; }
    }

    public static class DshCompiler
    {
        /// <summary>载入端编这份 MSL 的约定（离线链 MAC_RECORD 的位置）：newLibraryWithSource，preserveInvariance = YES（同离线的 -fpreserve-invariance）</summary>
        public const string ChainMetal = "runtime newLibraryWithSource preserveInvariance=YES (S2A.Dsh)";
        public static string Chain => DshChain.V;
        public static bool PackOut;
        [ThreadStatic] public static Dictionary<string, string> PackFiles;   // 出包：本线程这次 Compile 的产物（名 → 文本），调用方取走
        public static readonly System.Collections.Concurrent.ConcurrentDictionary<string, string> PackMsl = new();   // 出包：本进程出过的、有配对顶点要读的片元 _b.metal（vs_pad）
        public static Func<string, string> PackMslRead;   // 出包：配对顶点读片元 _b.metal 的来处（批量进程给「从产物表读」，就不在 PackMsl 里另留一份全文；11-02 马力欧派对 5.2 万支片元全文常驻约 7 GB）；null = 照旧查 PackMsl
        public static HashSet<string> PackMslKeep;   // 出包：要留的那些名字（<片元键>_b.metal，配对表第二列）；null = 全留   // 出包批量进程（batch/Program.cs）置真：另写 <键>_b.plain.metal 与变体源码 _bv / _bf / _ft（编译归出包编排层；设备现编不出变体）
        /// <summary>翻译器指纹（S2A.Dsh.csproj DshXfpHash：本库与所引翻译器工程源码的哈希）：现编结果 json 带它，宿主比 s2a_dsh_xfp() 不等就不复用（链号不随翻译器变，兜底目录跨 App 更新保留）</summary>
        public static string Xfp => DshChain.Xfp;
        static readonly string[] Stages = { "vs", "ps", "cs", "gs", "ts", "ms" };   // C 导出的阶段号：4 / 5 = NVN2 任务 / 网格（dsh_miss.inc dsh_stage）
        public static string StageName(int stage) => stage >= 0 && stage < Stages.Length ? Stages[stage] : null;

        // NVN 驱动常量段布局（dmsh_build.py 同名常量）
        const int TEX_BASE = 0x20, IMG_BASE = 0x120, IMG_SLOTS = 8, SEPTEX_BASE = 0x168, SEPTEX_SLOT0 = 128, SAMP_BASE = 0x568;
        // Same logical graphics stages as the offline chain; compute has a separate table.
        const uint SPILL_SLOT = 27;   // 计算核共享内存溢出缓冲的 Metal 槽（同 dmsh_build.py SPILL_SLOT / 渲染器 GAL_SPILL_SLOT）
        // NVN2 任务 / 网格的表在 0x670 / 0x770（上游参考实现（MIT） 5ae0d9aea / #1186：MK World 任务读 c0 0x670 / 0x680、网格 0x770..0x7b0，游戏从槽 0 绑；不是 0x100 步长的 0x610 / 0x710）
        static int? SSBO(string stage) => stage switch { "vs" => 0x110, "hs" => 0x210, "ds" => 0x310, "gs" => 0x410, "ps" => 0x510, "cs" => 0x310, "ts" => 0x670, "ms" => 0x770, _ => null };
        const int TF_SLOT = 8;

        /// <summary>Android Vulkan 运行时补编的翻译段：与 iOS 现编同一 JobFor / 翻译器，只取 SPIR-V + 反射，不出 MSL。
        /// 没有离线答案表时用空表（查询按缺省）；返回 control 键（也是 .xgs 文件名）与 c1。
        /// imgLoadRaw = 目标档案的 shaderStorageImageReadWithoutFormat（job imgload_raw）：缺省 false = 最低档案，只读非缓冲图像声明具体格式。</summary>
        public static (string Key, byte[] Spv, JsonObject Res, byte[] C1) TranslateForVulkan(
            string stage, byte[] code, byte[] ctl, string workRoot = null, bool imgLoadRaw = false)
        {
            if (Array.IndexOf(Stages, stage) < 0) throw new DshError("bad stage", "阶段 " + stage + " 不支持（只认 vs / ps / cs / gs）");
            if (ctl == null || ctl.Length < 0x880) throw new DshError("bad record", $"控制段只有 0x{ctl?.Length ?? 0:x} 字节（要 0x880）");
            string key = Convert.ToHexStringLower(Sha256.HashData(ctl.AsSpan(0, 0x880)))[..16];
            string w = Path.Combine(workRoot ?? Path.GetTempPath(), "dshvk_" + key + "_" + Guid.NewGuid().ToString("N")[..8]);
            Directory.CreateDirectory(w);
            var c = new Ctx { Key = key, Out = w, W = w, Xc = null, T = DshTables.Empty(), ImageLoadRaw = imgLoadRaw };
            try
            {
                (JsonObject j, byte[] c1) = c.JobFor(stage, code, ctl);
                c.TranslateFirst(j);
                byte[] spv = File.ReadAllBytes(Path.Combine(w, key + ".spv"));
                var res = JsonNode.Parse(File.ReadAllText(Path.Combine(w, key + ".res.json"))).AsObject();
                return (key, spv, res, c1);
            }
            finally { try { Directory.Delete(w, true); } catch { } }
        }

        public const string AstMemKept = "_astmem_kept";   // #1429 验收计数在出包产物（PackFiles）里的后缀
        /// <summary>出一个键：成功返回写出的文件名；失败抛 DshError。tablesDir = 答案表 / 配对表所在目录（运行时 $S2A_GAME_DATA/shaders/direct）</summary>
        public static List<string> Compile(string key, string stage, byte[] code, byte[] ctl, string outDir, ISpvCross xc, string tablesDir, string workRoot = null)
        {
            if (!Regex.IsMatch(key ?? "", @"\A[0-9a-f]{16}\z")) throw new DshError("bad key", "键不是 16 位小写十六进制：" + key);
            if (Array.IndexOf(Stages, stage) < 0 && !(PackOut && (stage == "hs" || stage == "ds")))   // 曲面外壳 / 域只在出包（dsh_batch）翻：设备首遇现编不支持曲面阶段（宿主没有曲面现编路径）
                throw new DshError("bad stage", "阶段 " + stage + " 不支持（只认 vs / ps / cs / gs / ts / ms）");
            var t = DshTables.Get(tablesDir);
            // 工作文件（翻译器的作业、机器码、SPIR-V、反射）在本线程的内存表里（XIo，dmsh_xlate/Program.cs），不落临时目录；出包（PackOut）连产物也留内存（PackFiles）
            string w = Path.Combine(workRoot ?? Path.GetTempPath(), "dsh_" + key + "_" + Guid.NewGuid().ToString("N")[..8]);
            if (!PackOut) Directory.CreateDirectory(outDir);
            var c = new Ctx { Key = key, Out = outDir, W = w, Xc = xc, T = t };
            var mem0 = XIo.Mem; XIo.Mem = new Dictionary<string, byte[]>(); PackFiles = PackOut ? new Dictionary<string, string>() : null;
            Ryujinx.Graphics.Shader.StructuredIr.AstMemoryOrderStats.Take();
            try
            {
                if (!PackOut) foreach (var n in OutNames(key)) { try { File.Delete(Path.Combine(outDir, n)); } catch { } }   // 这次出的就是留下的（离线链部分重建先删这些键的产物）
                var (j, c1) = c.JobFor(stage, code, ctl);
                c.TranslateFirst(j);
                c.Finish(j, c1);
                // #1429 验收：本键各次翻译里 #1414 规则保留下来的定义数（每线程计数，本线程刚翻完本键），随产物进出库缓存，dsh_batch 汇总行「ast mem order kept」读它
                if (PackFiles != null) PackFiles[key + AstMemKept] = Ryujinx.Graphics.Shader.StructuredIr.AstMemoryOrderStats.Take().ToString(System.Globalization.CultureInfo.InvariantCulture);
                return c.Written;
            }
            finally { XIo.Mem = mem0; }
        }

        public static IEnumerable<string> OutNames(string key)
        {
            foreach (var s in new[] { "_b", "_bg0", "_bg1", "_bg2", "_bg3", "_bg4", "_bgv", "_bgs0", "_bgs1", "_bgs2", "_bgs3", "_bgs4", "_bgc" })
                foreach (var e in new[] { ".metal", ".json" }) yield return key + s + e;
        }

        // ---------- 翻译器（dmsh_xlate/Program.cs 原样）：反射调它的私有 Main ----------
        static Func<string[], int> s_main;
        [DynamicDependency("Main", typeof(SwshXlate))]
        static Func<string[], int> XlateMain()
        {
            if (s_main != null) return s_main;
            var m = typeof(SwshXlate).GetMethod("Main", BindingFlags.NonPublic | BindingFlags.Static) ?? throw new DshError("translation failed", "翻译器 Main 被裁掉了");
            return s_main = m.CreateDelegate<Func<string[], int>>();
        }
        /// <summary>进程内跑一次翻译器；它的 stdout / stderr 只在本线程截下（ThreadCapture），不进宿主日志</summary>
        internal static (int Rc, string Err) RunXlate(string jobPath, string outDir)
        {
            var main = XlateMain();
            ThreadCapture.Begin();
            int rc; string err;
            try { rc = main(new[] { jobPath, outDir }); }
            finally { ThreadCapture.End(out _, out err); }
            return (rc, err);
        }

        internal sealed class Ctx
        {
            public string Key, Out, W; public ISpvCross Xc; public DshTables T;
            // 出库目标档案的 shaderStorageImageReadWithoutFormat（job imgload_raw）：false = 最低档案，只读非缓冲图像声明具体格式，
            //   在任何设备上都是合法 SPIR-V；真档案有这能力才写 true 声明 Unknown（dmsh_xlate Accessor 答问，#1774 ②）
            public bool ImageLoadRaw;
            public readonly List<string> Written = new();
            string Spv => Path.Combine(W, Key + ".spv");

            // ---- job_for ----
            public (JsonObject, byte[]) JobFor(string stage, byte[] code, byte[] ctl)
            {
                if (Nvn2.Is(ctl)) return Nvn2Job(stage, code, ctl);
                if (stage is "ts" or "ms") throw new DshError("bad stage", "任务 / 网格阶段只有 NVN2（Switch 2）控制段有");
                // rec[0x38:] = 机器码区 + 控制段（运行时拿到的就是这两段）；c1 照离线链按 rec 下标切
                var tail = new byte[code.Length + ctl.Length]; code.CopyTo(tail, 0); ctl.CopyTo(tail, code.Length);
                if (ctl.Length < 0x708) throw new DshError("bad record", $"控制段只有 0x{ctl.Length:x} 字节（要读 +0x700）");
                uint c1o = BitConverter.ToUInt32(ctl, 0x700), c1t = BitConverter.ToUInt32(ctl, 0x704);
                byte[] c1 = Sl(tail, (int)Math.Min(c1o, int.MaxValue), (int)Math.Min(c1t, int.MaxValue));
                if (stage == "vs") { var ab = VertexAb(Key, code, ctl); if (ab != null) (code, c1) = ab.Value; }
                if (stage != "cs") code = Sl(code, 0x30, code.Length);
                else if (code.Length >= 4 && code[0] == 0x79 && code[1] == 0x56 && code[2] == 0x34 && code[3] == 0x12)
                {
                    if (Sl(code, 0x30, 0x100).Any(b => b != 0)) throw new DshError("bad record", "compute code area +0x30..+0x100 is not all zero, so the instructions do not start at +0x100");
                    code = Sl(code, 0x100, code.Length);
                }
                string cp = Path.Combine(W, Key + ".bin"); XIo.WriteAllBytes(cp, code);
                var tf = stage == "vs" ? TfLayout(Key, ctl) : null;
                var j = new JsonObject { ["key"] = Key, ["stage"] = stage, ["code"] = cp, ["tex"] = new JsonObject(), ["img"] = new JsonObject(), ["attr"] = new JsonObject(), ["fout"] = new JsonObject(), ["c1"] = Convert.ToHexStringLower(c1), ["imgload_raw"] = ImageLoadRaw };
                if (tf != null) j["tf"] = TfJson(tf);
                if (stage == "gs" && TfLayout(Key, ctl) is { } gtf) j["gs_tf"] = TfJson(gtf);   // 几何着色器的变换反馈由宿主按输出记录写（同 dmsh_build.py tf_entries → geometry_finish geo_tf，审计 P2-40）
                // S2Arm 曲面细分：域着色器的模式在控制段 +0x738 / +0x73c / +0x744（GL 枚举，TessCtl.Nvn1）；外壳没有自己的模式，取本款第一种域的，其余域在 TessFinish 里重翻
                if (stage == "ds") j["tess_mode"] = (long)TessCtl.Nvn1(ctl);
                if (stage == "hs") j["tess_mode"] = T.TessModes.Count > 0 ? (long)T.TessModes.First().Value : 1L;
                // 早期深度（片元 layout(early_fragment_tests)）：控制段 +0x718 位 0，驱动按它写 3D 寄存器 EarlyZForce（同通用档 shbake EarlyZOf）；原先翻译器写死 false
                if (stage == "ps" && ctl.Length > 0x718) j["early_z"] = (ctl[0x718] & 1) != 0;
                j["debug"] = stage is "gs" or "cs"; j["reduced"] = true;   // 11-07 计算着色器同几何保留 DebugMode：first_store 等只对计算着色器的 MSL 文本改写按开着时的写法认（ampere_isa_test first_store 例），计算着色器每款一两百支、降分配收益在顶点 / 片元。XLATE_DEBUG / XLATE_REDUCED 的缺省（非缺省出的是另一个链版本，载入端不认）。11-07 起 DebugMode 只留给几何阶段（dmsh_chain.py options 同改）：
                //   开着时 AST 表达式传播整段跳过、输出满是临时量；关掉后异度3 真机转存 553 支翻译分配 5527 → 2941 MB、顶点耗时合计 -38%，像素中位 +5 ms。
                //   几何关掉后翻译慢 2.4 倍（9 支 2.9 → 7.6 s：CanPropagate 遍历约 3.1 s，其余是传播后表达式树变大），首遇现编会更卡，先保留；传播的正确性问题 #1166 / #1414 已修
                if (!T.Facts.TryGetValue(Key, out var f))   // 不在答案表：查询按缺省答
                {
                    j["no_facts"] = true;
                    // 计算着色器的线程组尺寸与共享内存在自己的控制段里（同 merge_extract.py cs_facts / shader_split.py）：不在表里也照读，否则反射没有 threadgroup_size、宿主派发不了
                    //   （11-02 疯狂兔子奇遇派对：包里直连库只有 2 支，局内兜底现编的计算着色器全报「compute shader lacks threadgroup size reflection」）
                    if (stage == "cs" && ctl.Length >= 0x758)
                    {
                        uint x = U(ctl, 0x748), y = U(ctl, 0x74c), z = U(ctl, 0x750), sh = U(ctl, 0x754);
                        if (x != 0 && y != 0 && z != 0 && (ulong)x * y * z <= 1024 && sh <= 0x10000)
                        {
                            j["local"] = new JsonArray(JsonValue.Create((long)x), JsonValue.Create((long)y), JsonValue.Create((long)z));
                            j["shared"] = JsonValue.Create((long)((sh + 15) / 16 * 16));
                        }
                    }
                    return (j, c1);
                }
                foreach (var kv in JsonNode.Parse(f).AsObject().ToList()) j[kv.Key] = kv.Value?.DeepClone();
                return (j, c1);
            }

            // ---- Switch 2（NVN2，控制段版本 0xc9）：Ampere 机器码交翻译器的 Ampere 前端（任务 arch = sm86），同 the reference port dmsh_build.py job_for（d3995ca4 / a7bd05f4，MIT） ----
            // 代码区 = 0x80 字节 SPH v4（图形阶段）+ 指令（控制段 +0x38 长）+ c1 常量段（+0x40 到 +0x44）；Maxwell 那几步（0x30 头、VertexA / B、变换反馈、+0x7xx 字段）NVN2 控制段里都没有
            (JsonObject, byte[]) Nvn2Job(string stage, byte[] code, byte[] ctl)
            {
                if (ctl.Length < 0xbc) throw new DshError("bad record", $"NVN2 控制段只有 0x{ctl.Length:x} 字节（要读 +0xba）");
                uint ilen = U(ctl, 0x38), c1a = U(ctl, 0x40), c1e = U(ctl, 0x44);
                if (ilen > code.Length || c1a > c1e || c1e > code.Length) throw new DshError("bad record", $"NVN2 控制段长度不合（+0x38 0x{ilen:x} +0x40 0x{c1a:x} +0x44 0x{c1e:x}，代码区 0x{code.Length:x}）");
                byte[] c1 = Sl(code, (int)c1a, (int)c1e);
                if (stage is "ts" or "ms") return MeshJob(stage, code, ctl, ilen, c1);
                code = Sl(code, 0, (int)ilen);
                string cp = Path.Combine(W, Key + ".bin"); XIo.WriteAllBytes(cp, code);
                var j = new JsonObject { ["key"] = Key, ["stage"] = stage, ["code"] = cp, ["arch"] = "sm86", ["tex"] = new JsonObject(), ["img"] = new JsonObject(), ["attr"] = new JsonObject(), ["fout"] = new JsonObject(), ["c1"] = Convert.ToHexStringLower(c1), ["imgload_raw"] = ImageLoadRaw };
                if (stage == "cs") j["tensor_mma_count"] = DshTensor.Count(code);
                // 计算阶段的驱动常量在 c[0] 里按程序排（推送常量字数 +0xa9，uniform / storage / 纹理项数 +0xb1..+0xb3，分离纹理 / 分离采样器项数 +0xb4 / +0xb5（图像在它们之后，上游参考实现（MIT）67f1d0055），大小 +0xba），翻译器按它映射（InstEmitAmpere.cs AMapC0）
                //   推常量块 1（+0xaa 字数，排在推常量块 0 之后、uniform 项之前）：游戏推进来的 {地址, 大小} 存储缓冲项或 8 字节句柄，翻译器映射到推常量槽 1 = c[25]（上游参考实现（MIT）nvn2_cs[7]）。
                //   不给它时，一款 Switch 2 游戏的计算着色器（+0xb2 = 0、+0xaa = 4 × 1..7）把这些项读进了 uniform / 句柄表的位置，存储缓冲解不出槽、整支不载
                if (stage == "cs") j["nvn2_cs"] = new JsonArray(ctl[0xb1], ctl[0xb2], ctl[0xb3], BitConverter.ToUInt16(ctl, 0xba), ctl[0xa9] * 4, ctl[0xb4], ctl[0xb5], ctl[0xaa] * 4);
                // 早期深度（NVN2）：像素着色器控制段 +0x5b（NVN1 在 +0x718，见上）。原先 NVN2 不读，Switch 2 款里该先测深度再着色的像素着色器（先写内存、后测深度）全部晚测，
                //   被遮挡的像素也整片着色。搬 nxnative a888e668（MIT）：NVN2 驱动按单字节读 +0x58..+0x62 的片元标志，+0x5b 即早测；对方读数（一款第三方 Switch 2 游戏，M4 Pro）GPU 19.0 → 16.6 ms、
                //   p95 64 → 26 ms；040074A01BF12000 的 13546 支像素着色器里 2855 支、0400486024DEC000 的 22352 支里 746 支带它。自己写深度的（SPH 字 19 位 1，OmapDepth）不提前：先测的深度不是它算出的那个
                if (stage == "ps" && ctl.Length > 0x5b && ctl[0x5b] != 0 && !(code.Length >= 0x50 && (BitConverter.ToUInt32(code, 19 * 4) & 2) != 0)) j["early_z"] = true;
                if (stage == "ds") j["tess_mode"] = (long)Nvn2.TessMode(ctl);
                if (stage == "hs") j["tess_mode"] = T.TessModes.Count > 0 ? (long)T.TessModes.First().Value : 1L;
                j["debug"] = stage is "gs" or "cs"; j["reduced"] = true;
                if (!T.Facts.TryGetValue(Key, out var f))
                {
                    j["no_facts"] = true;
                    if (stage == "cs")
                    {
                        uint x = U(ctl, 0x58), y = U(ctl, 0x5c), z = U(ctl, 0x60), sh = U(ctl, 0x64);   // 线程组 +0x58、共享内存 +0x64（ns2_shaders.py compute_facts）
                        if (x != 0 && y != 0 && z != 0 && (ulong)x * y * z <= 1024 && sh <= 0x10000)
                        {
                            j["local"] = new JsonArray(JsonValue.Create((long)x), JsonValue.Create((long)y), JsonValue.Create((long)z));
                            j["shared"] = JsonValue.Create((long)((sh + 15) / 16 * 16));
                        }
                    }
                    return (j, c1);
                }
                foreach (var kv in JsonNode.Parse(f).AsObject().ToList()) j[kv.Key] = kv.Value?.DeepClone();
                return (j, c1);
            }

            // ---- NVN2 任务 / 网格（上游参考实现（MIT） dmsh_build.py job_for 1d4b8e82b，MIT）：去掉 0x80 字节 SPH 当计算着色器翻，阶段内存在共享内存（MeshStage.Memory，作业 stage_memory），
            //   线程组 = 控制段 +0x58、共享内存 = 阶段内存大小；c[0] / c[2] 照图形阶段的表（不给 nvn2_cs），翻译器从代码里读地址。线程与内存都来自控制段，不查答案表 ----
            (JsonObject, byte[]) MeshJob(string stage, byte[] area, byte[] ctl, uint ilen, byte[] c1)
            {
                if (ilen < 0x80) throw new DshError("bad record", $"任务 / 网格指令区 0x{ilen:x} 不足 0x80 字节的 SPH");
                if (MeshStage.Kind(ctl) != stage) throw new DshError("bad record", $"控制段阶段字节 {ctl[0x56]} 不是 {stage}");
                var mi = MeshStage.Info(ctl, area);
                var (lay, size) = MeshStage.Memory(mi);
                var code = Sl(area, 0x80, (int)ilen);
                // 不支持的明确报红（接口约定 9）：自有共享内存会压在阶段内存上、控制段不给它的大小；阶段内存超 Metal 线程组内存；线程数为 0
                if (MeshStage.OwnShared(code)) throw new DshError("mesh stage failed", stage + ": 指令里有自己的共享内存访问（LDS / STS / ATOMS），会压在阶段内存上");
                // 载荷超 16 KB 的网格（MeshStage.Relay）：阶段内存整段在设备中转缓冲里，不受线程组内存限
                if (size > MeshStage.METAL_MAX_SHARED && !(stage == "ms" && MeshStage.Relay(mi))) throw new DshError("mesh stage failed", $"{stage}: 阶段内存 {size} 字节超 Metal 线程组内存 {MeshStage.METAL_MAX_SHARED}");
                if (mi.Threads == 0 || mi.Threads > 1024) throw new DshError("mesh stage failed", $"{stage}: 控制段 +0x58 每组线程数 {mi.Threads}");
                string cp = Path.Combine(W, Key + ".bin"); XIo.WriteAllBytes(cp, code);
                var j = new JsonObject { ["key"] = Key, ["stage"] = "cs", ["code"] = cp, ["arch"] = "sm86", ["tex"] = new JsonObject(), ["img"] = new JsonObject(), ["attr"] = new JsonObject(), ["fout"] = new JsonObject(), ["c1"] = Convert.ToHexStringLower(c1), ["imgload_raw"] = ImageLoadRaw };
                j["stage_memory"] = new JsonArray(lay.Select(v => (JsonNode)JsonValue.Create((long)v)).ToArray());
                j["local"] = new JsonArray(JsonValue.Create((long)mi.Threads), JsonValue.Create(1L), JsonValue.Create(1L));
                j["shared"] = JsonValue.Create((long)size);
                j["mesh_stage"] = mi.ToJson();
                j["debug"] = false; j["reduced"] = true;
                return (j, c1);
            }

            // ---- vertex_ab ----
            internal static (byte[], byte[])? VertexAb(string key, byte[] code, byte[] ctl)
            {
                if (code.Length < 0x80 || ctl.Length < 0x760 || !(code[0] == 0x78 && code[1] == 0x56 && code[2] == 0x34 && code[3] == 0x12)) return null;
                uint w0 = BitConverter.ToUInt32(code, 0x30);
                if ((w0 & 0x1f) != 1 || ((w0 >> 10) & 0xf) != 0) return null;
                uint a_sph = U(ctl, 0x748), a_len = U(ctl, 0x74c), b_sph = U(ctl, 0x754), b_len = U(ctl, 0x758);
                if (a_sph == 0 && a_len == 0 && b_sph == 0 && b_len == 0) return null;
                uint bw0 = 0x80 <= b_sph && b_sph <= code.Length - 0x50 ? U(code, (int)b_sph) : 0;
                if (a_sph != 0x30 || b_sph != U(ctl, 0x700) + 0x30 || (bw0 & 0x1f) != 1 || ((bw0 >> 10) & 0xf) != 1 || (a_len - 0x50) % 32 != 0 || (b_len - 0x50) % 32 != 0 || (long)b_sph + b_len > code.Length)
                    throw new DshError("bad record", $"vertex shader of shader type 0 without a VertexB part in the expected place (control +0x748 0x{a_sph:x} 0x{a_len:x} 0x{b_sph:x} 0x{b_len:x})");
                var sa = new uint[20]; var sb = new uint[20];
                for (int i = 0; i < 20; i++) { sa[i] = U(code, (int)a_sph + 4 * i); sb[i] = U(code, (int)b_sph + 4 * i); }
                var sph = new uint[20];
                sph[0] = sb[0] | (sa[0] & ~(0xfu << 10));
                for (int i = 1; i <= 3; i++) sph[i] = Math.Max(sa[i] & 0xffffff, sb[i] & 0xffffff) | (sb[i] & ~0xffffffu);
                sph[4] = sb[4];
                for (int i = 5; i < 20; i++) sph[i] = sa[i] | sb[i];
                var a = Sl(code, (int)a_sph + 0x50, (int)(a_sph + a_len)); var b = Sl(code, (int)b_sph + 0x50, (int)(b_sph + b_len));
                for (int o = 0; o < a.Length; o += 8)
                {
                    if (o % 32 == 0) continue;
                    ulong ins = BitConverter.ToUInt64(a, o);
                    if (ins >> 52 == 0xe30 && (ins & 0x1f) == 0xf)
                        BitConverter.TryWriteBytes(a.AsSpan(o), 0xe24UL << 52 | ((ulong)(a.Length - (o + 8)) & 0xffffff) << 20 | ins & 0xfffff);
                }
                var ms = new MemoryStream();
                ms.Write(code, 0, 0x30);
                foreach (var x in sph) ms.Write(BitConverter.GetBytes(x));
                ms.Write(a); ms.Write(b);
                return (ms.ToArray(), Sl(code, (int)(b_sph + b_len), code.Length));
            }
            static uint U(byte[] d, int o) => BitConverter.ToUInt32(d, o);

            // ---- tf_layout ----
            static JsonNode TfJson(object v) => v switch
            {
                null => null,
                long l => JsonValue.Create(l),
                List<object> xs => new JsonArray(xs.Select(TfJson).ToArray()),
                _ => throw new DshError("bad record", "transform feedback layout value of unknown type"),
            };
            // 同 dmsh_build.py tf_entries / tf_layout：每个缓冲 null 或 [步长, [[通用输出号, 分量数], …]]（一缓冲多条依次存，10-26 异度3）；没有变换反馈 = null
            internal static List<object> TfLayout(string Key, byte[] ctl)
            {
                var strides = new int[4]; for (int b = 0; b < 4; b++) strides[b] = BitConverter.ToUInt16(ctl, 0x6dc + 2 * b);
                if (strides.All(s => s == 0)) return null;
                var o = new List<object>();
                for (int b = 0; b < 4; b++)
                {
                    if (strides[b] == 0) { o.Add(null); continue; }
                    int n = ctl[0x6e8 + b];
                    var locs = Sl(ctl, 0xdc + 0x80 * b, 0xdc + 0x80 * b + n).Select(x => (int)x).ToList();
                    var comps = Sl(ctl, 0x4dc + 0x80 * b, 0x4dc + 0x80 * b + n).Select(x => (int)x).ToList();
                    var first = Sl(ctl, 0x2dc + 0x80 * b, 0x2dc + 0x80 * b + n);
                    if (!(1 <= n && n <= 32) || locs.Any(x => !(0x20 <= x && x < 0x40)) || comps.Any(c => !(1 <= c && c <= 4)) || first.Any(x => x != 0) || strides[b] != 4 * comps.Sum())
                    {
                        Console.Error.WriteLine($"[RED] {Key}: transform feedback buffer {b} has a layout of unknown form (stride {strides[b]}, entries {n}, outputs [{string.Join(", ", locs)}], components [{string.Join(", ", comps)}], start [{string.Join(", ", first)}]): no capture; see tf_entries");
                        return null;
                    }
                    o.Add(new List<object> { (long)strides[b], locs.Zip(comps, (x, c) => (object)new List<object> { (long)(x - 0x20), (long)c }).ToList() });
                }
                return o;
            }

            // ---- 翻译（dmsh_xlate）：三处调法照离线链各自的判据 ----
            string WriteJobs(JsonObject[] jobs, string name)
            {
                string jp = Path.Combine(W, name);
                XIo.WriteAllText(jp, "[" + string.Join(",", jobs.Select(x => x.ToJsonString())) + "]");
                return jp;
            }
            /// <summary>第一遍（dmsh_build.py 的 xl / fin）：stderr 里本键的 [RED] 行或没有 .spv = translation failed（退出码不看）</summary>
            public void TranslateFirst(JsonObject j)
            {
                var (_, err) = RunXlate(WriteJobs(new[] { j }, "job_" + Key + ".json"), W);
                string why = null;
                foreach (var ln in err.Split('\n'))
                {
                    var m = Regex.Match(ln.TrimEnd('\r'), @"\A\[RED\] (\w+): (.*)");
                    if (m.Success && m.Groups[1].Value == Key) why = Sl(m.Groups[2].Value, 0, 160);
                }
                if (why != null || !XIo.Exists(Spv)) throw new DshError("translation failed", why ?? "no spv");
            }
            /// <summary>整数输入二次翻译（finish）：退出码非 0 或没有 .spv</summary>
            void TranslateAgain(JsonObject j)
            {
                var (rc, err) = RunXlate(WriteJobs(new[] { j }, "job1_" + Key + ".json"), W);
                if (rc != 0 || !XIo.Exists(Spv)) throw new DshError("translation failed", Sl("integer-input retranslation: " + err.Trim(), 0, 160));
            }
            /// <summary>几何五种输入拓扑（geometry_finish）：退出码非 0</summary>
            void TranslateGeometry(JsonObject[] jobs)
            {
                var (rc, err) = RunXlate(WriteJobs(jobs, Key + "_geometry.json"), W);
                if (rc != 0) throw new DshError("geometry failed", Sl(err.Trim(), 0, 160));
            }

            // 直连三条链交 SPIRV-Cross 都过这里（顶点 / 片元 / 计算 / 几何）与下面的 TessMsl（曲面）：先加固（NxgalSpvCross.Harden，#1204 T1），同通用路径 xc_shader1
            string Msl(string spvPath, string status = "MSL failed", string resKey = null)   // resKey：资源表取别的键的（直通几何只算副作用的核 .side.spv 与本键同一份存储缓冲表）
            {
                var s = Xc.Msl(NxgalSpvCross.Harden(XIo.ReadAllBytes(spvPath)), out var e);
                if (s == null) throw new DshError(status, Sl(e ?? "", 0, 160));
                s = GatherOffsets(s);   // 四偏移 gather 拆成四次常量偏移（见 GatherOffsets）
                s = PoolArrays(s);   // 追不到槽的句柄读宿主的池表（见 PoolArrays）
                s = MulExtended(s);   // 标量宽乘法离开 optnone 模板（见 MulExtended）
                s = WideLoads(s);   // 同一只读存储缓冲的相邻字一次读（见 WideLoads）
                string resPath = Path.Combine(Path.GetDirectoryName(spvPath) ?? "", (resKey ?? Path.GetFileNameWithoutExtension(spvPath)) + ".res.json");
                return XIo.Exists(resPath) ? PoolNoInline(s, JsonNode.Parse(XIo.ReadAllText(resPath)) as JsonObject) : s;
            }

            void Save(string name, string text)
            {
                Written.Add(name);
                if (PackFiles != null) { PackFiles[name] = text; if (name.EndsWith("_b.metal", StringComparison.Ordinal) && (PackMslKeep == null || PackMslKeep.Contains(name))) PackMsl[name] = text; return; }
                string p = Path.Combine(Out, name), tmp = p + ".tmp-" + Environment.ProcessId + "-" + Environment.CurrentManagedThreadId;
                File.WriteAllText(tmp, text, new UTF8Encoding(false)); File.Move(tmp, p, true);
            }
            void SaveJson(string name, PyDict refl) { var v = new PyDict(refl); v["chain_metal"] = ChainMetal; Save(name, Dumps(v)); }

            string SourceOf() => T.Loc.TryGetValue(Key, out var l) ? $"{l.Pkg}#{l.Index}" : "s2a_dsh";
            object PcPaired() => T.PcOf.TryGetValue(Key, out var p) ? p : null;

            // ---- finish ----
            public void Finish(JsonObject j, byte[] c1)
            {
                string stage = (string)j["stage"];
                if (j["mesh_stage"] is JsonObject mo) { MeshFinish(j, c1, MeshInfo.FromJson(mo)); return; }
                if (stage == "hs" || stage == "ds") { TessFinish(j, c1); return; }
                if (stage == "gs")
                {
                    var s = GsSlice(Spv);
                    if (s != null)
                    {
                        var r0 = new PyDict { ["key"] = Key + "_b", ["route"] = "B", ["chain"] = Chain, ["xfp"] = Xfp, ["dm_ctl"] = Key, ["source"] = SourceOf(), ["pc_paired"] = PcPaired(), ["stage"] = "gs", ["gs_slice"] = s,
                            ["notes"] = new List<object>(), ["drv_reads"] = "", ["drv_unfillable"] = new List<object>(), ["bindings"] = new List<object>() };
                        SaveJson(Key + "_b.json", r0);
                        return;
                    }
                    try { GeometryFinish(j, c1); return; }
                    catch (DshError) { throw; }
                    catch (Exception e) when (e is ArgumentException || e is FormatException || e is InvalidOperationException) { throw new DshError("geometry failed", Sl(e.Message, 0, 160)); }
                }
                if (stage == "vs") SpvSingleLane(Spv);
                string src = Msl(Spv);
                if (stage == "vs")
                {
                    var extra = new PyDict();
                    var attr = (JsonObject)j["attr"];
                    foreach (var kv in IntInputs(src)) if (!attr.ContainsKey(kv.Key)) extra[kv.Key] = kv.Value;
                    if (extra.Count > 0)
                    {
                        foreach (var kv in extra) attr[kv.Key] = (string)kv.Value;
                        TranslateAgain(j);
                        SpvSingleLane(Spv);
                        src = Msl(Spv);
                    }
                }
                src = Precise(src);
                int tensorMma = stage == "cs" ? j["tensor_mma_count"]?.GetValue<int>() ?? 0 : 0;
                if (tensorMma > 0)
                {
                    long threads = j["local"] is JsonArray tl && tl.Count == 3 ? tl.Aggregate(1L, (n, v) => n * v.GetValue<long>()) : 0;
                    if (threads <= 0 || threads > 1024 || threads % 32 != 0) tensorMma = -1;
                }
                if (tensorMma > 0) src = DshTensor.Lower(src);
                uint spill = 0;   // 共享内存超过 32 KB 的计算核：最大的数组挪到设备缓冲（同 dmsh_build.py spill_fix，在 fs_lb 之前）
                if (stage == "cs") (src, spill) = NxgalSpvCross.SpillFix(src, SPILL_SLOT);
                if (stage == "ps") src = FsUvx(src);   // 宽屏 16:9 采样换算，在整数输出入口复制 main0 之前（同 dmsh_build.py）
                if (stage == "ps" || stage == "cs") src = FsLb(src);   // #800 采样器偏置进主体，在整数输出入口复制前
                var first = new List<(string Name, long Word, long Value)>();
                if (stage == "cs") (src, first) = FirstStore(src, j["local"] as JsonArray);   // 只有第一个调用做的存储挪给宿主（见 FirstStore）
                string colorAnalysis = src;
                if (stage == "ps") src = NxgalSpvCross.ColorRound(src);
                var pad = new List<object>();
                if (stage == "vs") src = VsLayer(VsInit(src));
                if (stage == "vs" && T.Pairs.TryGetValue(Key, out var ps) && ps.Count > 0) { var (s2, miss) = VsPadCore(Out, ps, src, T.PsInputs); src = s2; foreach (var x in miss) pad.Add(x); }
                string full = src, plain = null; PyDict lay = null;   // plain：不带层入口 / 整数输出入口的那份（出包成批合编时带不上就退回它，dmsh_build.py unity_flush）
                if (stage == "vs")
                {
                    string capture = GeoCapture(src);
                    (src, full) = VsDepth(src);
                    full += capture;
                    if (j["tf"] is JsonArray tf) full += VsTf(src, tf);
                    plain = VsPush(full);
                    (full, lay) = VsLayerEntry(full);
                    full = VsPush(full);
                }
                // 片元：Alpha 测试与多重采样关的可选函数常量（FsAlphaMs，族 34 / 37）进库里的主入口；src 不带它（反射、可读瓦片 _bf、输出类型 _ft 照旧读 src）
                string srcx = stage == "ps" ? FsFout(FsAlphaMs(src)) : src;
                if (stage == "ps") full = srcx;
                bool fout = srcx.Contains(FOUT_MARK, StringComparison.Ordinal);   // 输出类型由常量选（FsFout）：不再另出整数输出入口 main0_u / 输出类型变体 _ft
                var (fu, uMask) = stage == "ps" && !fout ? FsUint(srcx, colorAnalysis) : (null, 0u);
                if (fu != null) { plain = srcx; full = fu; }
                var res = JsonNode.Parse(XIo.ReadAllText(Path.Combine(W, Key + ".res.json"))).AsObject();
                full = DshFlat.GiantFix(full).Item1;   // 巨型着色器的尾部阶梯压平（同 dmsh_build.py giant_fix）
                Save(Key + "_b.metal", full);
                if (PackOut && (lay != null || fu != null)) Save(Key + "_b.plain.metal", DshFlat.GiantFix(plain).Item1);   // 出包（dsh_batch）才写
                var entry = Search(@"(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(", src);
                var dr = DrvReads(src);
                var notes = new List<object>(); foreach (var n in res["notes"].AsArray()) notes.Add((string)n);
                if (j["no_facts"] != null) notes.Add("not in the answer table, queries use defaults");
                if ((stage == "ps" || stage == "cs") && HelperSamples(src)) notes.Add("sampler LOD bias / reduction not applied: a sample outside main0 (helper function)");
                var refl = new PyDict { ["key"] = Key + "_b", ["route"] = "B", ["chain"] = Chain, ["xfp"] = Xfp, ["dm_ctl"] = Key, ["source"] = SourceOf(), ["pc_paired"] = PcPaired(), ["stage"] = stage,
                    ["entry"] = entry.Success ? entry.Groups[2].Value : "main0", ["msl_entry_kind"] = entry.Success ? entry.Groups[1].Value : null };
                if (stage == "vs") { refl["entry_z"] = "main0_z"; refl["entry_geometry_capture"] = "main0_gs"; }
                if (j["tf"] is JsonArray tf2) { refl["entry_tf"] = "main0_tf"; refl["tf_strides"] = tf2.Select(e => (object)(e is JsonArray ea && ea.Count > 0 ? (long)ea[0] : 0L)).ToList(); }
                if (lay != null) { refl["entry_layer"] = "main0_l"; refl["entry_layer_z"] = "main0_lz"; refl["layer_from"] = lay; }
                if (fu != null) { refl["entry_u"] = "main0_u"; refl["u_mask"] = (long)uMask; }
                refl["threadgroup_size"] = j["local"]?.DeepClone();
                if (stage == "cs") { refl["tensor_mma_abi"] = tensorMma >= 0 ? 1L : 0L; refl["tensor_mma_count"] = (long)tensorMma; }
                if (spill != 0) { refl["spill"] = (long)spill; refl["spill_slot"] = (long)SPILL_SLOT; }
                refl["c1"] = Convert.ToHexStringLower(c1); refl["notes"] = notes; refl["drv_reads"] = dr; refl["drv_unfillable"] = Unfillable(dr);
                var binds = Reflect(stage, src, res);
                refl["bindings"] = binds;
                if (first.Count > 0)   // 宿主在派发前按序写这些字（上游参考实现（MIT）反射 first_store：reg = 存储缓冲槽，即该成员 sbuf 绑定的 index）
                    refl["first_store"] = first.Select(f => (object)new PyDict { ["reg"] = binds.OfType<PyDict>().FirstOrDefault(b => (string)b["msl_name"] == f.Name && (string)b["kind"] == "sbuf")?["index"]
                        ?? throw new DshError("reflection failed", "first-invocation store to " + f.Name + ", which is no storage buffer binding"), ["word"] = f.Word, ["value"] = f.Value }).ToList();
                refl["vertex_inputs"] = All(@"(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]", src).Select(m => (object)new PyDict { ["location"] = (long)Int(m.Groups[2].Value), ["msl_type"] = m.Groups[1].Value }).ToList();
                refl["raster"] = RasterFacts(stage, src); refl["vs_pad"] = pad;
                if (UserLocs(src, stage) is { } ul) refl[stage == "vs" ? "user_out" : "user_in"] = ul;   // 同 dmsh_build.py user_locs：宿主建管线前核对配对
                if (stage == "vs" && full.Contains(PushParam, StringComparison.Ordinal)) refl["s2a_push"] = true;   // 宽屏：上屏入口读 s2a_push（同 dmsh_build.py）
                // 宽屏几何判据（11-06，移植通用模式 SpirvPosSlice / GeomCovers）：能外推的顶点着色器带位置切片（deflate 压缩的原 SPIR-V），宿主在「原尺寸居中层收缩 + 推满」
                //   候选笔上用 CPU 求各顶点 gl_Position.x / w，两侧都到画布边（NDC ±1）才收缩——Unreal 界面小部件按整画布视口画、程序画的细环等内容判据管不到（八方2 小地图）
                if (stage == "vs" && refl.ContainsKey("s2a_push") && S2Arm.Gfx.Widen.SpirvPosSlice.Extract(XIo.ReadAllBytes(Path.Combine(W, Key + ".spv")), out _) is { } posSlice)
                {
                    refl["pos_slice"] = Convert.ToBase64String(posSlice.Packed);
                    // 切片求值读常量按 SPIR-V 绑定号：宿主按「绑定号 → 常量寄存器（NVN 槽 − 3，同 cbuf_use）」取 S.ubo[阶段][寄存器]；槽 1（着色器自带常量段 c1）记 −1、宿主读 rf.c1；其余驱动槽不给，求值读到即判不出
                    var pu = new List<object>();
                    if (res["cbufs"] is JsonArray cbs) foreach (var c in cbs) { int b = (int)c["binding"], sl = (int)c["slot"]; if (sl >= 3 && sl <= 16) pu.Add(new List<object> { (long)b, (long)(sl - 3) }); else if (sl == 1) pu.Add(new List<object> { (long)b, -1L }); }   // −1 = 这支着色器自带的常量段 c1（json c1，宿主 rf.c1）
                    refl["pos_ubo"] = pu;
                }
                if (stage == "ps" && res["uses_frag_coord"] is JsonNode ufc && (bool)ufc) refl["frag_coord"] = true;   // 宽屏：读片元坐标（同 dmsh_build.py，宿主按它写 render_scale[0]）
                if (res["cbuf_use"] is JsonNode cu) refl["cbuf_use"] = (string)cu;   // 宽屏：每个常量寄存器读哪些 vec4（同 dmsh_build.py，宿主交给 s2a_widen_shader）
                foreach (var field in ScaleMetadata(res)) refl[field.Key] = field.Value;
                SaveJson(Key + "_b.json", refl);
                if (PackOut && stage == "vs") VinFinish(full);   // 同 dmsh_build.py finish：vin_finish(key, open(msl).read(), refl)，msl 此时 = 写出的 _b.metal（全部入口、压平后）
                if (PackOut && stage == "vs") PullFinish(full);   // 拉取式顶点输入变体 _bp（DshPull，照上游参考实现（MIT）dmsh_pull：pull_finish 同样从全部入口的源码出）
                if (PackOut && stage == "ps") { if (!fout) FtFinish(src, refl); BfFinish(src, refl); }   // 同 fetch_finish(key, src, refl)：src = color_round 之后、未接 main0_u（fs_uint 只出 fu，不改 src）
            }

            // ---- 任务 / 网格（上游参考实现（MIT） dmsh_build.py finish 1d4b8e82b，MIT）：计算核 MSL 包成对象 / 网格函数（MeshStage.ToStage）；反射字段按接口约定 6 ----
            void MeshFinish(JsonObject j, byte[] c1, MeshInfo mi)
            {
                string kind = mi.Kind;
                string src = Precise(Msl(Spv));
                string full = MeshStage.ToStage(src, mi);
                var res = JsonNode.Parse(XIo.ReadAllText(Path.Combine(W, Key + ".res.json"))).AsObject();
                full = DshFlat.GiantFix(full).Item1;
                Save(Key + "_b.metal", full);
                var dr = DrvReads(src);
                var notes = new List<object>(); foreach (var n in res["notes"].AsArray()) notes.Add((string)n);
                var refl = new PyDict { ["key"] = Key + "_b", ["route"] = "B", ["chain"] = Chain, ["xfp"] = Xfp, ["dm_ctl"] = Key, ["source"] = SourceOf(), ["pc_paired"] = PcPaired(), ["stage"] = kind,
                    ["entry"] = "main0", ["msl_entry_kind"] = kind == "ts" ? "object" : "mesh" };
                if (kind == "ms") refl["entry_z"] = "main0_z";
                refl["mesh_threads"] = (long)mi.Threads; refl["mesh_payload"] = (long)mi.Payload;
                if (MeshStage.Relay(mi))   // 载荷走设备中转（MeshStage.Relay）：宿主按这几个数分配、绑中转缓冲（nvn_geometry.inc mtk_encode）
                {
                    var (pw, sw) = MeshStage.RelayWords(mi);
                    refl["mesh_relay"] = (long)MeshStage.RELAY_SLOT; refl["mesh_relay_payload_words"] = (long)pw;
                    if (kind == "ms") refl["mesh_relay_slice_words"] = (long)sw;   // 每个任务组的片数 = 任务线程数（mesh_threads），宿主写进中转头
                }
                if (kind == "ms") { refl["mesh_task"] = mi.Task; refl["max_vertices"] = (long)mi.MaxV; refl["max_primitives"] = (long)mi.MaxP; }
                refl["threadgroup_size"] = j["local"]?.DeepClone();
                refl["c1"] = Convert.ToHexStringLower(c1); refl["notes"] = notes; refl["drv_reads"] = dr; refl["drv_unfillable"] = Unfillable(dr, kind);
                refl["bindings"] = Reflect(kind, src, res);
                refl["vertex_inputs"] = new List<object>(); refl["raster"] = new PyDict(); refl["vs_pad"] = new List<object>();
                if (res["cbuf_use"] is JsonNode cu) refl["cbuf_use"] = (string)cu;
                foreach (var field in ScaleMetadata(res)) refl[field.Key] = field.Value;
                SaveJson(Key + "_b.json", refl);
            }

            // ---- vin_finish（变体源码部分）：整数输入的顶点着色器出影子输入变体 <键>_bv；编库与写 refl['vin'] 归编排层，json 给它要的 vin / 失败原因 ----
            void VinFinish(string full)
            {
                var (v, shadows) = DshVin.Variant(full);
                if (v == null)
                {
                    if ((string)shadows != "no integer input") Save(Key + "_bv.json", Dumps(new PyDict { ["vin_fail"] = shadows }));   // 编排层照离线打 [RED] 行
                    return;
                }
                Save(Key + "_bv.metal", v);
                Save(Key + "_bv.json", Dumps(new PyDict { ["vin"] = new PyDict { ["entry"] = "main0", ["entry_z"] = "main0_z", ["shadow"] = shadows } }));
            }

            // ---- pull_finish（变体源码部分，DshPull）：有顶点属性输入的顶点着色器出拉取版 <键>_bp；编库与写 refl['pull'] 归编排层（Build.cs PullBuild），json 给它要的 pull / 认不出的原因 ----
            void PullFinish(string full)
            {
                var (v, info) = DshPull.Variant(full);
                if (v == null)
                {
                    if ((string)info != "no stage input") Save(Key + "_bp.json", Dumps(new PyDict { ["pull_fail"] = info }));   // 编排层打 [RED] 行
                    return;
                }
                Save(Key + "_bp.metal", v);
                Save(Key + "_bp.json", Dumps(new PyDict { ["pull"] = info }));
            }

            // ---- ft_finish（拼 MSL 部分，DshFt）：<键>_ft.json = {"want": [[u, s], …]}；拼不出 {"ft_fail": true}；表里有本键但全被过滤 {"ft_covered": true}（编排层计数）----
            void FtFinish(string src, PyDict refl)
            {
                T.FoutWant.TryGetValue(Key, out var req);
                var want = DshFt.Masks(refl, req);
                if (req != null && req.Count > 0 && want.Count == 0) { Save(Key + "_ft.json", Dumps(new PyDict { ["ft_covered"] = true })); return; }
                if (want.Count == 0) return;
                string msl = DshFt.Msl(src, want);
                if (msl == null) { Save(Key + "_ft.json", Dumps(new PyDict { ["ft_fail"] = true })); return; }
                Save(Key + "_ft.metal", msl);
                Save(Key + "_ft.json", Dumps(new PyDict { ["want"] = want.Select(m => (object)new List<object> { m.U, m.S }).ToList() }));
            }

            // ---- fetch_finish（变体源码部分，DshFetch）：om = DMSH_BF=1 且键在 bf_want.tsv；DMSH_BF 不开时只出含投影屏幕坐标采样的；<键>_bf.json = fmeta ----
            void BfFinish(string src, PyDict refl)
            {
                bool bfOn = Environment.GetEnvironmentVariable("DMSH_BF") == "1", om = bfOn && T.BfWant.Contains(Key);
                var (fv, fmeta) = DshFetch.Variant(src, JsonNode.Parse(Dumps(refl["bindings"])).AsArray(), om);
                if (fv == null) return;
                var meta = (PyDict)fmeta;
                if (!bfOn && !((List<object>)meta["calls"]).Any(c => c is PyDict d && d.TryGetValue("proj", out var p) && p is long pl && pl != 0)) return;
                Save(Key + "_bf.metal", fv);
                Save(Key + "_bf.json", Dumps(meta));
            }

            // ---- tess_finish（同 dmsh_build.py）：外壳 → 计算核、域 → 顶点函数（libnxgal nxgal_dmsh_xc_tess）。外壳按本款每种域各出一份：第一种 <键>_b，其余 <键>_b_t<域>；
            //   json 的 tess 写各域的库名、记录标志（gal_internal.h GAL_REC_*）与输出控制点数
            static readonly Dictionary<int, string> TESS_DOM = new() { [0] = "isolines", [1] = "triangles", [2] = "quads" };
            const uint TESS_BASE = 20;   // 曲面接口缓冲的第一个 Metal 槽（同 dmsh_build.py TESS_BASE）
            // 外壳要出的各域 →（域, 模式字, 是否本款扫到）。本款域着色器扫到的域按扫到的模式字在前（第一种进 <键>_b，同原先）；没扫到的域也补上：运行时才配上的域着色器
            //   （romfs 晚装、设备上现编）用到它时包里就有核（诊断 18 款「hull shader … has no isolines-domain kernel (variants: triangles)」，那组曲面绘制整笔跳过）。
            //   补的域取外壳自己的模式字、换掉低 2 位的域（外壳翻译里只有域改变曲面因子的布局，分区 / 绕序 / 点模式运行时由域着色器交给固定核）。域着色器只出它自己那一个域
            internal static List<(int Dom, uint Mode, bool Seen)> TessDomains(string stage, long mode, IReadOnlyDictionary<int, uint> seen)
            {
                var r = new List<(int, uint, bool)>();
                if (stage != "hs") { r.Add(((int)(mode & 3), (uint)mode, true)); return r; }
                if (seen.Count > 0) foreach (var kv in seen) r.Add((kv.Key, kv.Value, true));
                else r.Add(((int)(mode & 3), (uint)mode, true));
                for (int d = 0; d < 3; d++) if (!r.Any(x => x.Item1 == d)) r.Add((d, (uint)((mode & ~3L) | (long)d), false));
                return r;
            }
            void TessFinish(JsonObject j, byte[] c1)
            {
                string stage = (string)j["stage"]; long mode = (long)j["tess_mode"];
                var variants = new PyDict(); string first = null;
                foreach (var (d, dmode, seen) in TessDomains(stage, mode, T.TessModes))
                {
                    string lk = Key + (first == null ? "_b" : "_b_t" + d), spv = Spv;
                    if (stage == "hs" && first != null)   // 同一外壳按这一域的模式再翻一次
                    {
                        var jj = (JsonObject)j.DeepClone(); jj["key"] = Key + "_t" + d; jj["tess_mode"] = (long)dmode;
                        var (rc, err) = RunXlate(WriteJobs(new[] { jj }, "job_t_" + Key + "_" + d + ".json"), W); spv = Path.Combine(W, Key + "_t" + d + ".spv");
                        if (rc != 0 || !XIo.Exists(spv)) {
                            if (!seen) continue;   // 补出的域翻不出来：只是少这一域，本款扫到的域照旧（原先没有它）
                            throw new DshError("translation failed", Sl($"hull shader for {TESS_DOM.GetValueOrDefault(d, d.ToString())}: " + err.Trim(), 0, 160));
                        }
                    }
                    var (src, flags, ocp) = NxgalSpvCross.TessMsl(NxgalSpvCross.Harden(XIo.ReadAllBytes(spv)), 0, TESS_BASE);
                    src = Precise(WideLoads(MulExtended(PoolArrays(GatherOffsets(src))))); Save(lk + ".metal", src);
                    variants[TESS_DOM.GetValueOrDefault(d, d.ToString())] = new PyDict { ["library"] = lk, ["flags"] = (long)flags, ["output_control_points"] = (long)ocp };
                    first ??= src;
                }
                var res = JsonNode.Parse(XIo.ReadAllText(Path.Combine(W, Key + ".res.json"))).AsObject(); var dr = DrvReads(first);
                var notes = new List<object>(); foreach (var n in res["notes"].AsArray()) notes.Add((string)n);
                if (j["no_facts"] != null) notes.Add("not in the answer table, queries use defaults");
                var refl = new PyDict { ["key"] = Key + "_b", ["route"] = "B", ["chain"] = Chain, ["xfp"] = Xfp, ["dm_ctl"] = Key, ["source"] = SourceOf(), ["pc_paired"] = PcPaired(), ["stage"] = stage,
                    ["entry"] = "main0", ["msl_entry_kind"] = stage == "hs" ? "kernel" : "vertex", ["c1"] = Convert.ToHexStringLower(c1), ["notes"] = notes, ["drv_reads"] = dr,
                    ["drv_unfillable"] = Unfillable(dr), ["bindings"] = Reflect(stage, first, res), ["vertex_inputs"] = new List<object>(), ["raster"] = new PyDict(), ["vs_pad"] = new List<object>(),
                    ["tess"] = new PyDict { ["base_slot"] = (long)TESS_BASE, ["mode"] = mode, ["variants"] = variants } };
                SaveJson(Key + "_b.json", refl);
            }

            // ---- geometry_finish ----
            void GeometryFinish(JsonObject j, byte[] c1)
            {
                var code = XIo.ReadAllBytes((string)j["code"]);
                uint w2 = U(code, 8), w3 = U(code, 12), w4 = U(code, 16);
                uint nv = w4 & 4095, inv = w2 >> 24, topology = w3 >> 24 & 15;
                if (!(topology == 1 || topology == 6 || topology == 7) || nv == 0 || inv == 0) throw new DshError("geometry failed", "invalid output declaration");
                var jobs = new JsonObject[5];
                for (int t = 0; t < 5; t++) { var x = (JsonObject)j.DeepClone(); x["key"] = Key + "_g" + t; x["geometry_topology"] = t; jobs[t] = x; }
                TranslateGeometry(jobs);
                for (int t = 0; t < 5; t++)
                {
                    string k = Key + "_g" + t, spv = Path.Combine(W, k + ".spv");
                    if (!XIo.Exists(spv)) throw new DshError("geometry failed", "no spv " + k);
                    FlattenSets(spv);
                    string src = RollVbCopy(TopologyPointer(Precise(Msl(spv, "geometry MSL failed"))));
                    var res = JsonNode.Parse(XIo.ReadAllText(Path.Combine(W, k + ".res.json"))).AsObject();
                    var notes = new List<object>(); foreach (var n in res["notes"].AsArray()) notes.Add((string)n);
                    // 每次调用实际写的记录数（dmsh_xlate geometry_max_vertices，同 dmsh_build.py geometry_finish）：头里的数，但没有宿主直通时模拟的直通几何按输入图元发射
                    //   （线 2、三角 3，头里写 1）；只按头里的数分配暂存会越界约 3 倍、GPU 挂起（异度3 10-28）
                    var (nvt, nidx) = GeoRecords(nv, res);
                    // 直通几何快路（11-06 能耗线，金刚：几何核写记录 + 压缩下标是一帧 GPU 最大一项）：头第 0 字第 24 位标直通、翻译器出了只算副作用的核与抓顶点读取器
                    //   （dmsh_xlate gs_passthrough_fast：几何本体不写输出、不自己发射）时，反射标 "passthrough"，另出 _bgs<拓扑>（核：只留本体副作用）与 _bgc（主遍读抓顶点输出）；
                    //   宿主（nvn_geometry.inc geo_draw）见标记且两库都在才走快路，老库没有标记照旧
                    bool hdrPass = (U(code, 0) >> 24 & 1) != 0;
                    if (hdrPass != (res["gs_passthrough"] is JsonNode gp && (bool)gp)) throw new DshError("geometry failed", "passthrough header bit disagrees with the translator");
                    bool fast = hdrPass && res["gs_passthrough_fast"] is JsonNode pf && (bool)pf;
                    var refl = new PyDict { ["key"] = Key + "_bg" + t, ["route"] = "B", ["chain"] = Chain, ["xfp"] = Xfp, ["dm_ctl"] = Key, ["source"] = SourceOf(), ["pc_paired"] = PcPaired(), ["stage"] = "gs",
                        ["entry"] = "main0", ["msl_entry_kind"] = "kernel", ["threadgroup_size"] = new List<object> { 32L, 1L, 1L },
                        ["geometry"] = new PyDict { ["vertices"] = (long)nvt, ["invocations"] = (long)inv, ["output"] = (long)topology, ["stride"] = Py.FromJson(res["geometry_stride"]), ["index_stride"] = (long)nidx },
                        ["c1"] = Convert.ToHexStringLower(c1), ["notes"] = notes, ["drv_reads"] = DrvReads(src), ["drv_unfillable"] = new List<object>(),
                        ["bindings"] = Reflect("gs", src, res), ["vertex_inputs"] = new List<object>(), ["raster"] = new PyDict() };
                    if (j["gs_tf"] is JsonArray gtf) ((PyDict)refl["geometry"])["tf"] = GeoTf(Key, gtf, res);   // 同 dmsh_build.py geometry_finish：有 gs_tf 就写，换算不了是 null
                    foreach (var field in ScaleMetadata(res)) refl[field.Key] = field.Value;
                    if (fast)
                    {
                        ((PyDict)refl["geometry"])["passthrough"] = true;
                        string sspv = Path.Combine(W, k + ".side.spv"); FlattenSets(sspv);
                        string ssrc = RollVbCopy(TopologyPointer(Precise(Msl(sspv, "geometry side MSL failed", k))));
                        var side = new PyDict(refl) { ["key"] = Key + "_bgs" + t, ["drv_reads"] = DrvReads(ssrc), ["bindings"] = Reflect("gs", ssrc, res) };
                        side["geometry"] = new PyDict((PyDict)refl["geometry"]) { ["side_effects_only"] = true };
                        Save(Key + "_bgs" + t + ".metal", ssrc); SaveJson(Key + "_bgs" + t + ".json", side);
                    }
                    Save(Key + "_bg" + t + ".metal", src); SaveJson(Key + "_bg" + t + ".json", refl);
                    if (t == 0)
                    {
                        var b = new PyDict(refl) { ["key"] = Key + "_b" };
                        Save(Key + "_b.metal", src); SaveJson(Key + "_b.json", b);
                        GeometryReader(k + ".reader.spv", "_bgv", "geometry reader failed", topology);
                        if (fast) GeometryReader(k + ".capture.spv", "_bgc", "geometry capture reader failed", topology);   // 同 _bgv 一套改写，读的是抓顶点缓冲（vertex_output）
                    }
                }
            }

            // 主遍读几何结果的顶点变体：_bgv 读记录，_bgc 读抓顶点输出（直通几何快路）
            void GeometryReader(string spvName, string suffix, string status, uint topology)
            {
                string rspv = Path.Combine(W, spvName); FlattenSets(rspv);
                string reader = VsLayer(VsInit(Precise(Msl(rspv, status))));
                var (analysis, full) = VsDepth(reader);
                full = VsPush(full);
                var meta = new PyDict { ["key"] = Key + suffix, ["route"] = "B", ["chain"] = Chain, ["xfp"] = Xfp, ["dm_ctl"] = Key, ["stage"] = "vs", ["entry"] = "main0", ["entry_z"] = "main0_z", ["msl_entry_kind"] = "vertex",
                    ["geometry_reader"] = true, ["geometry_output"] = (long)topology, ["c1"] = "", ["notes"] = new List<object>(), ["drv_reads"] = new PyDict(), ["drv_unfillable"] = new List<object>(),
                    ["bindings"] = Reflect("vs", analysis, new JsonObject { ["sbufs"] = new JsonArray() }), ["vertex_inputs"] = new List<object>(), ["raster"] = new PyDict() };
                if (full.Contains(PushParam, StringComparison.Ordinal)) meta["s2a_push"] = true;
                meta["resolution_scale_abi"] = 2L;
                meta["resolution_scale_exact"] = false;
                meta["resolution_scale_status"] = "legacy_1x";
                meta["resolution_scale_reasons"] = new List<object> { "unsupported_scaled_access:geometry_reader" };
                Save(Key + suffix + ".metal", full); SaveJson(Key + suffix + ".json", meta);
            }

            // ---- gs_slice ----
            PyDict GsSlice(string spv) => GsSliceGlsl(Xc.Glsl450(XIo.ReadAllBytes(spv), out _));
            /// <summary>gs_slice 认形状那段（g = spirv-cross 出的 GLSL 450，null = 出不来）</summary>
            internal static PyDict GsSliceGlsl(string g)
            {
                if (g == null || !Has(g, "layout(triangles) in;") || !Has(g, "layout(max_vertices = 3, triangle_strip) out;") || Count(g, "EmitVertex();") != 3 || Has(g, "EndPrimitive")) return null;
                var ins = new Dictionary<string, int>(); foreach (Match m in All(@"layout\(location = (\d+)\) in vec4 (\w+)\[3\];", g)) ins[m.Groups[2].Value] = Int(m.Groups[1].Value);
                var outs = new Dictionary<string, int>(); foreach (Match m in All(@"layout\(location = (\d+)\) out vec4 (\w+);", g)) outs[m.Groups[2].Value] = Int(m.Groups[1].Value);
                string body = Sl(g, Find(g, "void main()"));
                var vr = new Dictionary<string, (string, string)>(); var layer = new HashSet<(string, string)>();
                foreach (var s0 in Regex.Split(Sl(body, Find(body, "{") + 1), "[;{}]"))
                {
                    string st = s0.Trim();
                    if (st.Length == 0 || st == "if (false)" || G(Full(@"int \w+ = -?\d+|\w+ = int\(0x80000000\)|int \w+ = \w+|EmitVertex\(\)", st))) continue;
                    var m = Full(@"float (\w+) = (\w+)\[\w+\]\.([xyzw])", st);
                    if (G(m) && ins.ContainsKey(m.Groups[2].Value)) { vr[m.Groups[1].Value] = (ins[m.Groups[2].Value].ToString(), m.Groups[3].Value); continue; }
                    m = Full(@"float (\w+) = gl_in\[\w+\]\.gl_Position\.([xyzw])", st);
                    if (G(m)) { vr[m.Groups[1].Value] = ("pos", m.Groups[2].Value); continue; }
                    m = Full(@"gl_Position\.([xyzw]) = (\w+)", st);
                    if (G(m) && vr.TryGetValue(m.Groups[2].Value, out var p0) && p0 == ("pos", m.Groups[1].Value)) continue;
                    m = Full(@"(\w+)\.([xyzw]) = (\w+)", st);
                    if (G(m) && outs.TryGetValue(m.Groups[1].Value, out int ol) && vr.TryGetValue(m.Groups[3].Value, out var p1) && p1 == (ol.ToString(), m.Groups[2].Value)) continue;
                    m = Full(@"gl_Layer = floatBitsToInt\((\w+)\)", st);
                    if (G(m) && vr.TryGetValue(m.Groups[1].Value, out var p2) && p2.Item1 != "pos") { layer.Add(p2); continue; }
                    return null;
                }
                if (layer.Count != 1 || Count(g, "gl_Layer =") != 3) return null;
                var (loc, comp) = layer.First();
                return new PyDict { ["loc"] = (long)Int(loc), ["comp"] = comp };
            }
        }

        // ---------- MSL 改写与分析（dmsh_build.py / dmsh_geometry.py 同名函数）----------
        static readonly Regex s_fast = new(@"\bfast::(min|max|clamp)\(", RegexOptions.CultureInvariant);
        /// <summary>查内存池表的辅助函数不内联（11-05 时之笛慢编）：追不到具体存储缓冲的全局访问，翻译器（GlobalToStorage TryGenerateRawStorageOp）生成一个按池表二分查找的函数，
        /// 各处调用它；SPIRV-Cross 给每个辅助函数加 static inline always_inline，Metal 就把它在每个调用处展开一份。3d0d6d61bb9076e7 有 253 处，IR 约 6.5 万条，
        /// iOS G18P 后端 2.5 秒、真机 8.4 秒。认法：参数里带池表 / 写记录缓冲（翻译器哨兵常量缓冲槽 0xff 下的存储缓冲，res.json sbufs 按 binding 排序的下标 = MSL 名
        /// &lt;阶段&gt;_s&lt;下标&gt;，同 Reflect）的函数，改成 static __attribute__((noinline))：static 保留内部链接（合批进同一库不撞名），函数体与调用不变。
        /// 11-07 只认参数里没有可写线程私有引用（thread 后不跟 const）的：翻译器把查表拆成每处内联的快路（区间缓存在线程私有字里）+ 全着色器一份的查找（GetRawLookup，不碰私有字）；
        /// 带私有字引用的函数不内联，那些字就得放进栈里、调用点存栈取栈（时之笛主遍顶点着色器每顶点 253 次调用，主遍六成时间）</summary>
        internal static string PoolNoInline(string msl, JsonObject res)
        {
            if (res?["sbufs"] is not JsonArray arr) return msl;
            var sbs = arr.Select(x => x.AsObject()).OrderBy(x => (long)x["binding"]).ToList();
            var idx = Enumerable.Range(0, sbs.Count).Where(i => sbs[i]["cb_slot"] is JsonNode c && (long)c == 0xff).ToList();
            if (idx.Count == 0) return msl;
            return Regex.Replace(msl, @"static inline __attribute__\(\(always_inline\)\)\n(?=[^\n{]*&\s*\w+_s(?:" + string.Join("|", idx) + @")\b)(?![^\n{]*\bthread (?!const\b))",
                "static __attribute__((noinline))\n", RegexOptions.CultureInvariant);
        }

        // ---- first_store（上游参考实现（MIT）dmsh_build.py c8521eeb + e07cba1c）：只有派发里第一个调用做的存储（计算着色器的开场：if (-local.x == workgroup.x << log2(local size)) buf[k] = v）。
        //   斯普拉遁 突击队的 Hoian_Proc 这样清零追加计数器，其余调用几百条指令后往上加；Switch 上第一个工作组先跑，Apple GPU 工作组不分先后：先追加的工作组被后来的清零抹掉
        //   （植物逐帧闪、马车世界树木成块消失）。所以存储挪出着色器，宿主在派发前按序做（反射 first_store，宿主 nvn_mtl.m 派发前写这些字）。只认这几种确切的条件：一维线程组、
        //   或带行的（y、x 同样的测试且 workgroup.z == 0）；存储可一块一个守卫块，值可以是同一守卫下设成常量的局部量。认不出的守卫存储一律整支原样不动。
        //   → (源码, [(存储缓冲的 MSL 名, 字号, 值)])
        static readonly Regex s_firstStore = R(@"float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*int (\w+) = as_type<int>\(\1\) << (\d+);\s*"
            + @"int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \5 \+ 1;\s*bool (\w+) = \6 != \3;\s*");
        static readonly Regex s_firstStore2D = R(@"float (\w+) = as_type<float>\(gl_WorkGroupID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*"
            + @"float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_WorkGroupID\.z\);\s*"
            + @"int (\w+) = as_type<int>\(\1\) << (\d+);\s*int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \8 \+ 1;\s*bool (\w+) = \9 != \6;\s*"
            + @"int (\w+) = as_type<int>\(\4\) << (\d+);\s*int (\w+) = ~as_type<int>\(\3\);\s*int (\w+) = \13 \+ 1;\s*bool (\w+) = \14 == \11;\s*"
            + @"bool (\w+) = !\10;\s*bool (\w+) = \15 && \16;\s*bool (\w+) = (?:as_type<int>\(\5\) == 0|0 == as_type<int>\(\5\));\s*bool (\w+) = \18 && \17;\s*(?:bool (\w+) = !\19;\s*)?");
        static readonly Regex s_firstPlain = R(@"\Gint \w+ = [^;{}]*;\s*");   // 编译器夹在中间的算术
        // 一条存储：上游的裸形 (*set.name)._m0[k] = v;，或我方加固后的形状（存储缓冲下标先过界检查：uint _i = uint(k); if (_i < 长度) { (*set.name)._m0[min(_i, 末项)] = v; }）。
        //   值是常量 Nu 或同一守卫下设成常量的局部量 uint(_n)
        const string FirstStoreOne = @"(?:\(\*\w+\.(?<n>\w+)\)\._m0\[(?<w>\d+)\] = (?<v>\d+u|uint\(\w+\));"
            + @"|uint (?<i>\w+) = uint\((?<w>\d+)\);\s*if \(\k<i> < \w+\)\s*\{\s*\(\*\w+\.(?<n>\w+)\)\._m0\[min\(\k<i>, \w+\)\] = (?<v>\d+u|uint\(\w+\));\s*\})";
        static readonly Regex s_firstOne = R(FirstStoreOne);
        internal static (string Src, List<(string Name, long Word, long Value)> Stores) FirstStore(string msl, JsonArray local)
        {
            var none = (msl, new List<(string, long, long)>());
            if (local == null || local.Count != 3) return none;
            long L(int i) => local[i] is JsonValue v && v.TryGetValue(out long x) ? x : -1;
            if (L(2) != 1) return none;
            static long Pow(string sh) => int.TryParse(sh, out int k) && k < 31 ? 1L << k : -1;
            string guard; var m = s_firstStore.Match(msl);
            if (m.Success)
            {
                if (L(1) != 1 || Pow(m.Groups[4].Value) != L(0)) return none;
                guard = "!" + Esc(m.Groups[7].Value);
            }
            else
            {
                m = s_firstStore2D.Match(msl);
                if (!m.Success || Pow(m.Groups[7].Value) != L(1) || Pow(m.Groups[12].Value) != L(0)) return none;
                guard = m.Groups[20].Success ? "!" + Esc(m.Groups[20].Value) : Esc(m.Groups[19].Value);
            }
            var held = R(@"\Gint (\w+) = \w+;\s*if \(" + guard + @"\)\s*\{\s*\1 = (\d+);\s*\}\s*int (\w+) = \1;\s*");   // 第一个调用里设成常量的局部量
            var block = R(@"\Gif \(" + guard + @"\)\s*\{\s*((?:" + FirstStoreOne + @"\s*)+)\}\s*");
            int at = m.Index + m.Length; var outp = new StringBuilder(msl, 0, at, msl.Length);
            var stores = new List<(string, long, long)>(); var values = new Dictionary<string, long>(StringComparer.Ordinal);
            while (true)
            {
                var h = held.Match(msl, at);
                if (h.Success && long.TryParse(h.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out long hv)) { values[h.Groups[3].Value] = hv; outp.Append(msl, at, h.Index + h.Length - at); at = h.Index + h.Length; continue; }
                var b = block.Match(msl, at);
                if (b.Success)
                {
                    foreach (Match st in s_firstOne.Matches(b.Groups[1].Value))
                    {
                        string v = st.Groups["v"].Value, nm = st.Groups["n"].Value; long w = long.Parse(st.Groups["w"].Value, CultureInfo.InvariantCulture);
                        if (v.EndsWith('u') && long.TryParse(v[..^1], NumberStyles.None, CultureInfo.InvariantCulture, out long c) && c <= uint.MaxValue) stores.Add((nm, w, c));
                        else if (!v.EndsWith('u') && values.TryGetValue(v[5..^1], out long hv2)) stores.Add((nm, w, hv2));
                        else return none;
                    }
                    at = b.Index + b.Length; continue;
                }
                var p = s_firstPlain.Match(msl, at);
                if (!p.Success) break;
                outp.Append(msl, at, p.Index + p.Length - at); at = p.Index + p.Length;
            }
            string rest = msl[at..];
            if (stores.Count == 0 || stores.Count > 4 || Search(@"if \(" + guard + @"\)", rest).Success) return none;
            return (outp.Append(rest).ToString(), stores);
        }

        // ---------- 追不到槽的句柄读整个池（上游参考实现（MIT）dmsh_build.py pool_arrays / POOL_MARK，b1d7c707 / cf3ec563 / 6121fd75）----------
        internal const int POOL_MIN = 33;    // 至少这么长的数组是池：翻译器（Program.cs）把回退数组按池长出（纹理 2^20、采样器 2^12），贴图缓冲 / 图像与统一缓冲句柄数组仍是 32 项
        internal const int POOL_HEAD = 16;   // = nvn_refl.h DMSH_POOL_HEAD：池表头 = 上界 n（uint）、4 字节空、已读位图的 GPU 地址（ulong）；元素 0 从这里起，每项 8 字节资源号
        internal const string POOL_MARK = "template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)\n{\n"
            + "    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);\n    uint b = 1u << (i & 31u);\n"
            + "    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);\n    return i;\n}\n\n";
        static readonly Regex s_poolArr = new(@"^(\s*)array<(.+), (\d+)> (\w+) \[\[\s*id\(", RegexOptions.Multiline | RegexOptions.CultureInvariant);
        static readonly Regex s_poolMember = new(@"^(\s*)(.+?)\s+(\w+)\s*\[\[\s*id\((\d+)\)\s*((?:,[^\]]*)?)\]\];", RegexOptions.Multiline | RegexOptions.CultureInvariant);
        static readonly Regex s_poolOwnSide = new(@"\bdiscard_fragment\b|\[\[\s*depth\(|\[\[\s*sample_mask\s*\]\]|\[\[\s*stencil\s*\]\]|\.write\(|\batomic_\w+_explicit\b|^\s*device\s", RegexOptions.Multiline | RegexOptions.CultureInvariant);
        /// <summary>存储缓冲逐字读合并（照 nxnative dm/gfx/tools/dmsh_wide.py，MIT；改成适配本链加固后的写法）。
        /// Ampere 一次读 64 / 128 位存储缓冲（LDG.E.64 / .128），翻译器拆成 32 位逐字读，SPIRV-Cross 再给每个字各写一个下标，加固（gal_spvx.c spv_bounds，#1204 T1）
        /// 又给每个字各钳一次末项：
        ///     uint _1613 = uint(int(uint(_77 + 220) >> uint(2)));
        ///     float _103 = as_type&lt;float&gt;((*spvDescriptorSet0.vp_s2)._m0[min(_1613, _1580)]);
        /// Metal 的前端认不出这些字是相邻的，每个字一次读、各带一套地址运算。Switch 2 的着色器大量这样读（随机抽 120 个批库：设备读 75% 是同一基址上的连续字）。
        /// 这里把同一只读（const device）缓冲、同一根值、字节偏移 k, k+4, …（一段直线代码里、末项表达式相同）的相邻字改成一次 packed_uint2/3/4 读：
        ///     uint wb = min(i0, max(L, N-1) - (N-1)); packed_uintN w = *(const device packed_uintN*)&amp;buf._m0[wb];   第 k 个字 = w[min(i_k, L) - wb]
        /// 不越界时一次读 N 个字，各字的值与改前逐位相同（(x + 4k) >> 2 = (x >> 2) + k）。越界时整块往回挪到缓冲末尾，各字仍按原逐字钳位 min(i_k, L) 取同一个字
        /// （11-14 改：原先越界时直接取第 k 个分量，和逐字钳位不是同一个字，游戏读到的值变了）。
        /// 缓冲不足 N 个字（L &lt; N-1）时从 0 起读 N 个字，会读出绑定范围最多 N-1 个字（仍在游戏的内存池里）；N 个字以内的存储缓冲没见过。
        /// 不用「越界就逐字照原样」的三目写法：M5 Max 上微基准（2^20 线程 × 64 次循环 × 20 个字）改前 0.71 ms，三目写法 1.50 ms（反而慢一倍），本写法 0.34 ms（2.1 倍）、
        /// 结果与改前逐位相同（不越界时）。
        /// 一串从根值的 16 字节边界开始、最多 4 个字（同原来的宽读），最低的字要最先读；碰到可能写内存或改值的行（下标 / 指针存储、原子、屏障、图像写、给已有变量赋值）、
        /// 花括号就断开，所以别的绑定对同一内存的写、或根值的改变，不会落在一串字中间：把后面的字提前读，读到的值相同。函数体按第 0 列的 '{' / '}' 分段，各段各自建表</summary>
        static readonly Regex s_wAdd = R(@"^\s*int (_\w+) = (_\w+) \+ \(?(-?\d+)\)?;\s*$");
        static readonly Regex s_wIdx = R(@"^\s*uint (_\w+) = uint\(int\(uint\((_\w+)(?: \+ \(?(-?\d+)\)?)?\) >> uint\(2\)\)\);\s*$");
        static readonly Regex s_wLoad = R(@"\(\*spvDescriptorSet0\.([a-z]p_s\d+)\)\._m0\[min\((_\w+), ((?:[^\[\]()]|\([^()]*\))+)\)\]");
        static readonly Regex s_wRo = new(@"^\s*const device _\w+\* ([a-z]p_s\d+) \[\[id\(\d+\)\]\];", RegexOptions.Multiline | RegexOptions.CultureInvariant);
        const string WOP = @"(?:[-+*/%|&^]|<<|>>)?=(?!=)";
        static readonly Regex s_wWrite = R(@"atomic_|_barrier\(|\.write\(|\]\s*" + WOP + @"|^\s*\*|^\s*_\w+\s*" + WOP);
        internal static string WideLoads(string msl)
        {
            if (!msl.Contains(" >> uint(2)", StringComparison.Ordinal) || !msl.Contains("._m0[min(", StringComparison.Ordinal)) return msl;
            var ro = new HashSet<string>(); foreach (Match m in s_wRo.Matches(msl)) ro.Add(m.Groups[1].Value);
            if (ro.Count == 0) return msl;
            string[] lines = msl.Split('\n');
            var edits = new SortedDictionary<int, List<(int S, int E, string Rep, string Decl)>>(); int n = 0;
            var segs = new List<(int A, int B)>(); int open = -1;
            for (int i = 0; i < lines.Length; i++) { if (lines[i] == "{") open = i; else if (lines[i] == "}" && open >= 0) { segs.Add((open, i)); open = -1; } }
            string[] lane = { "x", "y", "z", "w" };
            foreach (var (a, b) in segs)
            {
                var adds = new Dictionary<string, (string V, long K)>(); var idx = new Dictionary<string, (string V, long K)>();
                for (int i = a; i < b; i++)
                {
                    var ma = s_wAdd.Match(lines[i]); if (ma.Success) { adds[ma.Groups[1].Value] = (ma.Groups[2].Value, long.Parse(ma.Groups[3].Value)); continue; }
                    var mi = s_wIdx.Match(lines[i]); if (mi.Success) idx[mi.Groups[1].Value] = (mi.Groups[2].Value, mi.Groups[3].Success ? long.Parse(mi.Groups[3].Value) : 0);
                }
                var words = new Dictionary<(int Block, string Buf, string Root, string Last), SortedDictionary<long, (int Line, Match M)>>(); int block = 0;
                var order = new List<(int, string, string, string)>();
                for (int i = a + 1; i < b; i++)
                {
                    string ln = lines[i];
                    if (ln.Contains('{') || ln.Contains('}') || s_wWrite.IsMatch(ln)) { block++; continue; }
                    var ms = s_wLoad.Matches(ln); if (ms.Count != 1) continue;
                    var m = ms[0]; string buf = m.Groups[1].Value, iv = m.Groups[2].Value;
                    if (!ro.Contains(buf) || !idx.TryGetValue(iv, out var ix)) continue;
                    string v = ix.V; long off = ix.K;
                    for (int g = 0; g < 64 && adds.TryGetValue(v, out var ad); g++) { v = ad.V; off += ad.K; }
                    if (off % 4 != 0) continue;
                    var key = (block, buf, v, m.Groups[3].Value);
                    if (!words.TryGetValue(key, out var by)) { words[key] = by = new SortedDictionary<long, (int, Match)>(); order.Add(key); }
                    by.TryAdd(off, (i, m));
                }
                foreach (var key in order)
                {
                    var by = words[key];
                    var bases = new SortedSet<long>(); foreach (long o in by.Keys) bases.Add(o - ((o % 16) + 16) % 16);
                    foreach (long bs in bases)
                    {
                        var run = new List<(int Line, Match M)>();
                        while (run.Count < 4 && by.TryGetValue(bs + 4 * run.Count, out var w)) run.Add(w);
                        if (run.Count < 2 || run.Exists(w => w.Line < run[0].Line)) continue;
                        int N = run.Count; string name = "_dmw" + n++, vec = "packed_uint" + N, i0 = run[0].M.Groups[2].Value, last = key.Item4;
                        string bb = $"(*spvDescriptorSet0.{key.Item2})._m0";
                        // 基址 b = min(i0, max(L, N-1) - (N-1))：不越界时 b = i0，一次读 i0..i0+N-1；越界时整块挪到缓冲末尾。
                        //   每个字仍按原逐字钳位取值：第 w 个字 = 块里第 min(i_w, L) - b 个分量（不越界时恰是 w；越界时与原 min(i_w, L) 读到同一个字）。
                        //   11-14 改：原先越界时直接取第 w 个分量，读到的是末尾往回第 N-1-w 个字，和逐字钳位不是同一个字——某款游戏卡在第 1390 帧（游戏线程等一个值、GPU 空闲）的头号嫌疑
                        string decl = $"uint {name}b = min({i0}, max({last}, {N - 1}u) - {N - 1}u); {vec} {name} = *(const device {vec}*)&{bb}[{name}b];";
                        for (int w = 0; w < N; w++)
                        {
                            var (li, mm) = run[w];
                            if (!edits.TryGetValue(li, out var el)) edits[li] = el = new List<(int, int, string, string)>();
                            el.Add((mm.Index, mm.Index + mm.Length, $"{name}[min({mm.Groups[2].Value}, {last}) - {name}b]", w == 0 ? decl : null));
                        }
                    }
                }
            }
            if (edits.Count == 0) return msl;
            foreach (var (li, el) in edits)
            {
                string ln = lines[li]; var pre = new List<string>();
                el.Sort((x, y) => y.S.CompareTo(x.S));
                foreach (var (s0, e0, rep, decl) in el) { ln = ln[..s0] + rep + ln[e0..]; if (decl != null) pre.Add(decl); }
                string ind = lines[li][..(lines[li].Length - lines[li].TrimStart().Length)];
                lines[li] = string.Concat(pre.ConvertAll(d => ind + d + "\n")) + ln;
            }
            return string.Join('\n', lines);
        }
        /// <summary>Ryujinx 给追不到槽的句柄的回退数组（&lt;阶段&gt;_&lt;t|s&gt;_cb0_40000000，按句柄的纹理 / 采样器号下标，长度 = 整段编号，Program.cs）→ 指向宿主按元素类型建的池表
        /// （nvn_mtl.m pool_gpu）：struct spvPool_&lt;名&gt; { uint n; ulong seen; array&lt;元素, 1&gt; e; }，读成 (&amp;e[0])[min(下标, n)]，n = 宿主填到的最大编号。
        /// 贴图池的读还在 seen 处的位图里置这个编号的位（dm_pool_mark）：着色器读哪些编号只有 GPU 知道，宿主据此只建被读过的贴图，而不是池里全部（11-06 时之笛登记约 3.3 万、读约 300）。
        /// 先用普通读测位、没置才原子或：一个位只置一次，几乎每次读只多一次读。测位不用原子读（照 nxnative 04811a63，MIT）：原子读即使 relaxed 也走 GPU 共享缓存，一遍里的像素只读少数几个编号、挤在同一个位图字上排队（M6 上 1536x864 一遍每像素 16 次置位 2.60 → 0.34 ms，iPad Pro M5 上 0400486024DEC000 的颜色遍约 13.0 → 9.2 ms）；读到旧的 0 只多做一次不改值的或，位在一次提交内只会 0 → 1。结构体只声明 1 个元素：Metal 的参数编码器会逐个走声明的数组元素（2^20 个时每个编码器 57 ms、66 MB，上游实测），
        /// 元素是 8 字节资源号、经指针读到其余。参数号随后按成员次序重排（指针占 1 个、其余数组占其长度），同宿主排参数块的方式。
        /// 贴图池的置位是写内存，Metal 会把这支片元改成着色后再测深度（失去隐藏面剔除，且与预深度遍差一个 ulp 时 EQUAL 沿轮廓失败，上游实测）：置位是宿主自己加的，
        /// 不该改测试时机，所以改成早测——除非着色器自己丢弃 / 写深度 / 写样本掩码或模板 / 写内存（那时早测会错，保持晚测，只多付着色）。
        /// 改写覆盖不到的用法（整个数组当参数传）抛 DshError</summary>
        internal static string PoolArrays(string src)
        {
            var body = Search(@"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", src, RegexOptions.Singleline);
            if (!body.Success) return src;
            var pools = new List<(string Name, string Elem)>();
            string members = s_poolArr.Replace(body.Groups[1].Value, m =>
            {
                if (Int(m.Groups[3].Value) < POOL_MIN || !m.Groups[4].Value.EndsWith("_cb0_40000000", StringComparison.Ordinal)) return m.Value;
                pools.Add((m.Groups[4].Value, m.Groups[2].Value));
                return m.Groups[1].Value + "constant spvPool_" + m.Groups[4].Value + "* " + m.Groups[4].Value + " [[id(";
            });
            if (pools.Count == 0) return src;
            long aid = 0;
            members = s_poolMember.Replace(members, m =>
            {
                string o = m.Groups[1].Value + m.Groups[2].Value + " " + m.Groups[3].Value + " [[id(" + aid + ")" + m.Groups[5].Value + "]];";
                var a = Full(@"array<.+, (\d+)>", m.Groups[2].Value.Trim());
                aid += a.Success ? Int(a.Groups[1].Value) : 1;
                return o;
            });
            var structs = new StringBuilder();
            if (pools.Any(p => p.Elem != "sampler")) structs.Append(POOL_MARK);
            foreach (var (nm, el) in pools) structs.Append("struct spvPool_").Append(nm).Append("\n{\n    uint n;\n    ulong seen;\n    array<").Append(el).Append(", 1> e;\n};\n\n");
            int at = body.Index;
            src = src[..at] + structs + src[at..body.Groups[1].Index] + members + src[(body.Groups[1].Index + body.Groups[1].Length)..];
            foreach (var (nm, el) in pools)   // base.name[下标] → (&base.name->e[0])[min(uint(下标), base.name->n)]，下标按方括号配平取
            {
                var sb = new StringBuilder(); int pos = 0;
                foreach (Match m in Regex.Matches(src, @"([\w.]+)\." + Esc(nm) + @"\[", RegexOptions.CultureInvariant))
                {
                    if (m.Index < pos) continue;
                    int depth = 1, j = m.Index + m.Length;
                    while (depth > 0)
                    {
                        if (j >= src.Length) throw new DshError("pool rewrite failed", nm + ": unbalanced index");
                        depth += src[j] == '[' ? 1 : src[j] == ']' ? -1 : 0; j++;
                    }
                    string bs = m.Groups[1].Value + "." + nm, index = "min(uint(" + src[(m.Index + m.Length)..(j - 1)] + "), " + bs + "->n)";
                    sb.Append(src, pos, m.Index - pos).Append("(&").Append(bs).Append("->e[0])[").Append(el == "sampler" ? index : "dm_pool_mark(" + bs + ", " + index + ")").Append(']');
                    pos = j;
                }
                src = sb.Append(src, pos, src.Length - pos).ToString();
                int names = el == "sampler" ? 2 : 3;   // 每处读写名字两次（贴图的置位里再一次）；成员声明一次（spvPool_<名> 是一个词，不算）
                int left = Regex.Matches(src, @"\b" + Esc(nm) + @"\b", RegexOptions.CultureInvariant).Count - names * Regex.Matches(src, @"\." + Esc(nm) + @"->e\[0\]\)\[", RegexOptions.CultureInvariant).Count - 1;
                if (left != 0) throw new DshError("pool rewrite failed", $"{nm}: {left} uses of the pool array the rewrite does not cover");
            }
            string own = src.Replace(POOL_MARK, "");   // 着色器自己的写：有就保持游戏原来的测试时机
            if (pools.Any(p => p.Elem != "sampler") && !s_poolOwnSide.IsMatch(own))
                src = Regex.Replace(src, @"^(?<!\]\] )fragment\s", "[[early_fragment_tests]] fragment ", RegexOptions.Multiline | RegexOptions.CultureInvariant);
            return src;
        }

        internal static string Precise(string msl) => s_fast.Replace(msl, m => "precise::" + m.Groups[1].Value + "(");

        // 宽乘法（OpUMulExtended / OpSMulExtended）：SPIRV-Cross 写成一个 [[clang::optnone]] 模板 spvMulExtended（它的注释：Metal 编译器内联 mulhi 可能出内部错误），
        // 每个乘积都是一次不优化的函数调用，常数除法（Switch 2 着色器用乘积高字做 / 16 之类）折不掉。在模板后面补标量 uint / int 两个重载：总内联、取 64 位乘积的高低半，
        // 不用 mulhi；重载决议优先选它们，标量调用全部离开 optnone、常数折掉（2 的幂变移位），向量调用仍走原模板。模板原文不是 SPIRV-Cross 那样就原样返回。
        // 照 nxnative dmsh_rewrite.py mul_extended（MIT）；它的读数：时之笛 M4 Pro 森林路线 GPU 每帧 15.55 → 14.76 ms、54.0 → 59.1 帧
        const string MULEXT = "template<typename T, typename U> static inline __attribute__((always_inline)) T spvMulExtended(uint l, uint r) { ulong p = ulong(l) * ulong(r); return T{U(p), U(p >> 32)}; }\n"
                            + "template<typename T, typename U> static inline __attribute__((always_inline)) T spvMulExtended(int l, int r) { long p = long(l) * long(r); return T{U(p), U(p >> 32)}; }\n";
        static readonly Regex s_mulExt = new(@"template<typename T, typename U, typename V>\s*\[\[clang::optnone\]\] T spvMulExtended\(V l, V r\)\s*\{\s*return T\{U\(l \* r\), U\(mulhi\(l, r\)\)\};\s*\}\n", RegexOptions.CultureInvariant);
        internal static string MulExtended(string msl)
        {
            if (!msl.Contains("spvMulExtended", StringComparison.Ordinal) || msl.Contains("spvMulExtended(uint l, uint r)", StringComparison.Ordinal)) return msl;
            var found = s_mulExt.Matches(msl);
            if (found.Count != 1) return msl;
            int at = found[0].Index + found[0].Length;
            return msl[..at] + MULEXT + msl[at..];
        }

        // 四偏移 gather（SPIR-V ConstOffsets，GL textureGatherOffsets）：SPIRV-Cross 的 spvGatherConstOffsets / spvGatherCompareConstOffsets 在循环里把 coffsets[i] 传给 gather()，
        // Metal 的 gather 偏移只认常量表达式，编译不报错，偏移成了运行时值（AIR 里 load <2 x i32> 再交 air.gather_texture_2d）。时之笛 NDSR 放大器 b106afba1a18faa9 的 9 处 gather 同形，
        // 上游参考实现（MIT）记的症状是同一输入每次跑出的画面不同。每次调用改成四次单偏移 gather、偏移写成常量，各取足迹左上纹素（.w，gather 次序 i0j1, i1j1, i1j0, i0j0），
        // 正是包装函数返回的那四个分量。偏移数组不是常量 int2 × 4 就拒翻（报红，不留可能读错的形式）
        const string GATHER4 = "template<typename T> static inline __attribute__((always_inline)) vec<T, 4> s2a_gather4(T a, T b, T c, T d) { return vec<T, 4>(a, b, c, d); }\n";
        static readonly Regex s_gatherCall = new(@"\bspvGather(Compare)?ConstOffsets\(", RegexOptions.CultureInvariant);
        static readonly Regex s_gatherArr = new(@"\bconstant spvUnsafeArray<int2, 4> (\w+) = spvUnsafeArray<int2, 4>\(\{ (.*?) \}\);", RegexOptions.CultureInvariant);
        static readonly Regex s_int2 = new(@"int2\([^()]*\)", RegexOptions.CultureInvariant);

        // 四个偏移恰好是 2x2 方块（b、b+(1,0)、b+(0,1)、b+(1,1)，次序不限）：改成在 b 处一次 gather 再换序（照 nxnative 0c8a3ac0，MIT）。每个偏移取的是自己足迹的
        // (i0, j0) 角，整数偏移下这四个角正好是 b 处足迹的四个纹素，各自按包裹方式取，两种写法同值。Metal 的次序：x (i0, j1)、y (i1, j1)、z (i1, j0)、w (i0, j0)。
        // agl 的 NDSR 放大网络用这种方块拼输入块（0400486024DEC000 的 32 支带偏移 gather 的着色器里 27 支，每支 3–15 处）；nxnative 读数：M4 Pro 最大一次分派 845 → 779 us
        static readonly Regex s_int2Lit = new(@"\Aint2\(\s*(-?\d+)\s*(?:,\s*(-?\d+)\s*)?\)\z", RegexOptions.CultureInvariant);
        internal static (string Corner, string Swz)? GatherQuad(List<string> off)
        {
            var xy = new List<(int X, int Y)>();
            foreach (var o in off)
            {
                var m = s_int2Lit.Match(o); if (!m.Success) return null;
                int x = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
                xy.Add((x, m.Groups[2].Success ? int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture) : x));
            }
            int bx = xy.Min(p => p.X), by = xy.Min(p => p.Y);
            var swz = new StringBuilder(); var seen = new HashSet<(int, int)>();
            foreach (var (x, y) in xy)
            {
                (int, int) r = (x - bx, y - by);
                char c = r switch { (0, 0) => 'w', (1, 0) => 'z', (0, 1) => 'x', (1, 1) => 'y', _ => '?' };
                if (c == '?' || !seen.Add(r)) return null;
                swz.Append(c);
            }
            return ($"int2({bx}, {by})", swz.ToString());
        }

        internal static string GatherOffsets(string msl)
        {
            var calls = s_gatherCall.Matches(msl).Where(m => string.CompareOrdinal(msl, m.Index + m.Length, "const ", 0, 6) != 0).ToList();   // 去掉包装自己的定义（参数以 const 开头）
            if (calls.Count == 0) return msl;
            var arrays = new Dictionary<string, List<string>>();
            foreach (Match m in s_gatherArr.Matches(msl))
            {
                var off = s_int2.Matches(m.Groups[2].Value).Select(x => x.Value).ToList();
                if (off.Count != 4) throw new DshError("gather offsets rewrite failed", Sl("unexpected offset array " + m.Value, 0, 160));
                arrays[m.Groups[1].Value] = off;
            }
            var sb = new StringBuilder(); int pos = 0;
            foreach (Match m in calls)
            {
                if (m.Index < pos) continue;
                var args = new List<string>(); var cur = new StringBuilder(); int depth = 1, j = m.Index + m.Length;
                while (depth > 0)   // 实参按括号配平切开（圆 / 方 / 花括号都算层）
                {
                    if (j >= msl.Length) throw new DshError("gather offsets rewrite failed", Sl("unbalanced call " + msl[m.Index..], 0, 160));
                    char ch = msl[j++];
                    if (ch is '(' or '[' or '{') depth++;
                    else if (ch is ')' or ']' or '}') depth--;
                    if (depth == 0 || (ch == ',' && depth == 1)) { args.Add(cur.ToString().Trim()); cur.Clear(); }
                    else cur.Append(ch);
                }
                bool cmp = m.Groups[1].Success; int rest0 = cmp ? 3 : 4;   // 普通：纹理、采样器、偏移、分量、其余；比较：纹理、采样器、偏移、其余（坐标 [、层]、参考值）
                if (args.Count <= rest0 || !arrays.TryGetValue(args[2], out var offs))
                    throw new DshError("gather offsets rewrite failed", Sl("offsets not a constant int2 array: " + msl[m.Index..j], 0, 160));
                string rest = string.Join(", ", args.Skip(rest0)), tail = cmp ? "" : ", " + args[3], fn = cmp ? "gather_compare" : "gather";
                var quad = GatherQuad(offs);
                if (quad != null) sb.Append(msl, pos, m.Index - pos).Append($"({args[0]}.{fn}({args[1]}, {rest}, {quad.Value.Corner}{tail}).{quad.Value.Swz})");
                else sb.Append(msl, pos, m.Index - pos).Append("s2a_gather4(").Append(string.Join(", ", offs.Select(o => $"{args[0]}.{fn}({args[1]}, {rest}, {o}{tail}).w"))).Append(')');
                pos = j;
            }
            sb.Append(msl, pos, msl.Length - pos);
            string s = sb.ToString();
            if (!s.Contains("s2a_gather4(", StringComparison.Ordinal)) return s;
            int at = s.IndexOf("using namespace metal;", StringComparison.Ordinal);
            if (at < 0) throw new DshError("gather offsets rewrite failed", "no using namespace metal;");
            at = s.IndexOf('\n', at) + 1;
            return s[..at] + GATHER4 + s[at..];
        }

        internal static (string, string) VsDepth(string src)
        {
            var m = Search(@"\bvertex\s+main0_out\s+main0\s*\(", src);
            if (!m.Success) throw new DshError("MSL failed", "vertex shader without a main0 main function, cannot add the depth entry points");
            string body = src[m.Index..];
            string gl = Replace(body, "return out;", "out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);\n    return out;");
            string single = src[..m.Index] + gl;
            return (single, single + "\n" + Sub(@"\bmain0\s*\(", "main0_z(", body, 1));
        }

        // ---- vs_push（S2Arm 宽屏外推尾段，dmsh_build.py 同名函数，逐字同输出）----
        const int PUSH_SLOT = 13;   // = dmsh_build.py PUSH_SLOT（graphics/widen/export/s2a_widen.h S2A_WIDEN_PUSH_SLOT）
        static readonly string PushParam = $"constant float4& s2a_push [[buffer({PUSH_SLOT})]]";   // 接在入口参数后（原本没有参数时不带前导逗号）
        static readonly string[] s_pushEntries = { "main0", "main0_z", "main0_l", "main0_lz" };
        // 竖向外推段（11-05 族 6，窄屏加高 Duo）：接在横向段后面、横向段原文不动。s2a_push.w = 竖向外推量 1 − 1/Ky（> 0 才做），门限² 与横向共用 .y = ((1 − 3‰)/Ky)²，
        //   推画布上下边上及外的顶点（|y/w|² > 门限²）：画布边上的顶点落到屏幕上下边、画布内的顶点不动——同横向实际生效的规则（横向外推恒带收缩量 NoBandZ，撤掉了窄带，10-24 动森转场）。
        //   加高时核心给横向外推量 .x = 0、收缩 .z = 0，横向段只加 0。原先加高拿「整笔竖向拉满」顶替，把文字 / 页签底板一起拉走（哆啦A梦牧场条款页，录帧回放定因）
        // 横 / 竖两段都只推屏幕内的顶点（t² < (1 − 3‰)² = 0.994009，同门限的 3‰ 容差：推满恒带收缩量 NoBandZ，正好在屏幕边的 ±1 收缩后落在边内一丝，11-05）：已在屏幕边上及外的顶点不缺覆盖，再挪只把插值出的采样坐标整体错开——裁剪空间写死的全屏三角形 (−1, 3)
        //   各挪 1 − 1/k 后中心采样落到左边 pad/2 列（旷野之息宽屏整套放宽：光照合成整幅左移 70 列，一深一浅两个林克，录帧回放关掉推满即归位）
        internal static string VsPushY(string pos) => "{ float4 s2a_q = s2a_push; if (s2a_q.w > 0.0f) { float4 s2a_p = " + pos + "; float s2a_t = s2a_p.y / s2a_p.w, s2a_t2 = s2a_t * s2a_t; "
            + pos + ".y = s2a_p.y + (s2a_t2 > s2a_q.y && s2a_t2 < 0.994009f ? (s2a_t < 0.0f ? -s2a_q.w : s2a_q.w) * s2a_p.w : 0.0f); } }";
        internal static string VsPush(string full)
        {
            foreach (string ep in s_pushEntries)
            {
                var a = Search(@"\bvertex\s+(\w+)\s+" + ep + @"\s*\(", full);
                if (!a.Success) continue;
                var st = Search(@"struct " + a.Groups[1].Value + @"\s*\{(.*?)\};", full, RegexOptions.Singleline);
                var pm = Search(@"float4 (\w+) \[\[position(?:, invariant)?\]\];", st.Success ? st.Groups[1].Value : "");
                if (!pm.Success) continue;
                int b = a.Index + a.Length, depth = 1;
                for (; b < full.Length; b++) { depth += full[b] == '(' ? 1 : 0; depth -= full[b] == ')' ? 1 : 0; if (depth == 0) break; }   // 参数表的右括号（属性里自带括号：[[buffer(0)]]）
                int e = full.IndexOf('{', b); depth = 0;
                for (; e < full.Length; e++) { depth += full[e] == '{' ? 1 : 0; depth -= full[e] == '}' ? 1 : 0; if (depth == 0) break; }
                string pos = "out." + pm.Groups[1].Value;
                string tail = "{ float4 s2a_q = s2a_push; float4 s2a_p = " + pos + "; float s2a_x = s2a_p.x * (1.0f - s2a_q.z), s2a_t = s2a_x / s2a_p.w, s2a_t2 = s2a_t * s2a_t; "
                    + "bool s2a_big = s2a_t2 > s2a_q.y && s2a_t2 < 0.994009f && (s2a_q.z > 0.0f || s2a_t2 < s2a_q.y * (((1.0f + 3e-3f) / (1.0f - 3e-3f)) * ((1.0f + 3e-3f) / (1.0f - 3e-3f)))); "
                    + pos + ".x = s2a_x + (s2a_big ? (s2a_t < 0.0f ? -s2a_q.x : s2a_q.x) * s2a_p.w : 0.0f); }\n    " + VsPushY(pos) + "\n    return out;";
                string body = Replace(full[b..(e + 1)], "return out;", tail);
                full = full[..b] + (full[(a.Index + a.Length)..b].Trim().Length > 0 ? ", " : "") + PushParam + body + full[(e + 1)..];
            }
            return full;
        }

        // ---- fs_lb / fs_rd（S2Arm 采样器 LOD 偏置进主体 / 归约变体，#800；translator/dmsh_red.py 同名函数，逐字同输出）----
        //   FsLb：每支片元 / 计算的 main0 自己给采样加采样器偏置（参数块成员 s2a_lb，宿主 nvn_mtl.m lb_fill 写），Compile 里接在 FsUvx 之后（同 dmsh_build.py finish）；
        //   FsRd：采样模拟变体 <键>_se.metal（DshCApi s2a_dsh_samp_emu）。离线库不带，绘制真绑了本机不原生归约的 min / max 采样器、或（任何 GPU）Metal 没有的边界色时
        //   宿主在后台要它（nvn_mtl.m se_need / se_kick）
        const string RED = "s2a_red";   // 模拟表：变体 main0 的缓冲参数，每个采样器成员一项 S2ASe（64 字节），下标 = 该成员 [[id]]（graphics/nvn/gfx/nvn/nvn_red.h nvn_se_ent）
        const int RED_SLOT = 26;   // 它的 Metal 缓冲槽（片元与计算同号，nvn_mtl.m RED_SLOT）
        const string LB = "s2a_lb";   // 偏置表：参数块成员，每个采样器成员一个 float，按该成员的 [[id]] 取（= dmsh_red.py LB）
        const long LB_MAXID = 191;   // = dmsh_red.py LB_MAXID（NVN_REFL_MAXARG - 1）
        // #529 定点深度比较：同一张 s2a_lb 表的后半段 [LB_DC + 成员 id] = 1 → 该采样器成员这次配的贴图客体是定点深度（D16 / D24 / D24S8，宿主 nvn_mtl.m lb_fill 按客体格式写），
        //   sample_compare 的参考值先 saturate 到 [0,1] 再比（NVN 定点深度格式的比较语义；Metal 用 Depth32Float 承载它们、不会自己限制）；浮点深度（D32F 系）写 0、参考值原样
        internal const long LB_DC = LB_MAXID + 1;
        internal const string RED_BEGIN = "// s2a:samp-emu 4 (S2A.Dsh FsRd)\n";   // 变体格式版本 4（11-03 加折行贴图 s2a_fx：超 Metal 2D 上限的高线性贴图）；3（11-01 表项改 S2ASe、加任意边界色 / 全过滤组合归约），宿主 nvn_mtl.m SE_VERSION 认它：旧版 2 的兜底文件重出
        static readonly Regex s_redCall = R(@"spvDescriptorSet0\.(\w+)\.sample\(");
        // FsLb：比较采样同样加偏置（缺口族 31）。#529 营地阴影：深度贴图按 texture2d 声明时 SPIRV-Cross 写成 spvDepthCast(spvDescriptorSet0.X).sample_compare(...)，
        //   原先只认直接调用，这一形态的定点深度比较标志、偏置都没加上（玩家同存档实测：左下地面平均亮度 26.22 → 44.71）。贴图成员名在第 1 或第 2 组（Tx）
        static readonly Regex s_lbCall = R(@"(?:spvDepthCast\(spvDescriptorSet0\.(\w+)\)|spvDescriptorSet0\.(\w+))\.(?:sample|sample_compare)\(");
        static readonly Regex s_redEntry = R(@"^((?:\[\[[^\]]*\]\]\s*)*)(fragment|kernel) (\S+) main0\((.*)\)\n\{\n", RegexOptions.Multiline);
        // 成员的 [[id(N)]] 后面可以还有别的属性（[[id(4), raster_order_group(0)]]，SPIRV-Cross 给片元同步的资源这样写；马力欧派对 超级巨星 10-30）：
        //   三处成员正则与反射（Reflect）同一种写法，只认 [[id(N)]] 会漏掉它们——下一个 id 撞车、带第二属性的贴图不加偏置（审计 1101 18:00 P1-18）
        const string IdAttr = @"\[\[\s*id\((\d+)\)\s*(?:,[^\]]*)?\]\];";
        static readonly Regex s_redTex = R(@"^\s*((?:texture|depth)(?:2d|3d|cube)(?:_array)?)<(\w+)>\s+(\w+)\s*" + IdAttr, RegexOptions.Multiline);   // = RED_TEX
        static readonly Regex s_member = R(@"^\s*(.+?)\s+(\w+)\s*" + IdAttr, RegexOptions.Multiline);
        const string RedLod = @"(level|bias|gradient2d|gradient3d|gradientcube|min_lod_clamp)\((.*)\)";   // = RED_LOD（整串匹配）
        const string LbLevel = @"level\(\((.*)\) \+ spvDescriptorSet0\.s2a_lb\[(\d+)\]\)";   // = LB_LEVEL（整串匹配）：FsLb 加过偏置的显式 level
        static bool Balanced(string s) { int d = 0; foreach (char c in s) { d += c == '(' || c == '[' ? 1 : 0; d -= c == ')' || c == ']' ? 1 : 0; if (d < 0) return false; } return d == 0; }
        static (string, string)? Split2(string s)
        {
            int d = 0;
            for (int i = 0; i < s.Length; i++) { char c = s[i]; d += c == '(' || c == '[' ? 1 : 0; d -= c == ')' || c == ']' ? 1 : 0; if (c == ',' && d == 0) return (s[..i].Trim(), s[(i + 1)..].Trim()); }
            return null;
        }
        /// <summary>= next_id：参数块结构体里所有成员之后的第一个 id（array&lt;T, n&gt; 成员从自己的 id 起占 n 个）</summary>
        internal static long NextId(string body)
        {
            long n = 0;
            foreach (Match m in s_member.Matches(body)) { var a = Full(@"array<.*,\s*(\d+)>", m.Groups[1].Value.Trim()); n = Math.Max(n, Int(m.Groups[3].Value) + (a.Success ? Int(a.Groups[1].Value) : 1)); }
            return n;
        }
        /// <summary>= members：参数块结构体、贴图成员（种类、元素类型）、采样器成员（id）；没有结构体返回 null</summary>
        static (Match Body, Dictionary<string, (string Kind, string Elem)> Tex, Dictionary<string, long> Smp)? Members(string src)
        {
            var body = Search(@"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", src, RegexOptions.Singleline);
            if (!body.Success) return null;
            var tex = new Dictionary<string, (string Kind, string Elem)>(); var smp = new Dictionary<string, long>();
            foreach (Match m in s_redTex.Matches(body.Groups[1].Value)) tex[m.Groups[3].Value] = (m.Groups[1].Value, m.Groups[2].Value);
            foreach (Match m in All(@"^\s*sampler\s+(\w+)\s*" + IdAttr, body.Groups[1].Value, RegexOptions.Multiline)) smp[m.Groups[1].Value] = Int(m.Groups[2].Value);
            return (body, tex, smp);
        }
        /// <summary>= starts：main0 函数体里经贴图成员的采样从后往前的起点（内层先改，外层按改过的串重新解析）</summary>
        static List<int> Starts(string body, Dictionary<string, (string Kind, string Elem)> tex, Regex call = null)
        {
            var l = new List<int>();
            foreach (Match m in (call ?? s_redCall).Matches(body)) if (tex.ContainsKey(Tx(m))) l.Add(m.Index);
            l.Reverse(); return l;
        }
        /// <summary>= call_at：st 起的采样调用 → (结尾, 参数, 贴图成员)；括号不闭合返回 null</summary>
        static (int End, List<string> Args, string T)? CallAt(string body, int st, Regex call = null)
        {
            var m = (call ?? s_redCall).Match(body, st);
            if (!m.Success || m.Index != st) return null;
            int i = m.Index + m.Length, a0 = i, depth = 1; var args = new List<string>();
            while (depth > 0 && i < body.Length)
            {
                char c = body[i]; depth += c == '(' || c == '[' ? 1 : 0; depth -= c == ')' || c == ']' ? 1 : 0;
                if ((c == ',' && depth == 1) || depth == 0) { args.Add(body[a0..i].Trim()); a0 = i + 1; }
                i++;
            }
            return depth != 0 ? null : (i, args, Tx(m));
        }
        /// <summary>采样调用匹配里的贴图成员名：第一个匹配上的捕获组（s_lbCall 的两种形态各一组；别的调用正则只有第 1 组）</summary>
        static string Tx(Match m) { for (int g = 1; g < m.Groups.Count; g++) if (m.Groups[g].Success) return m.Groups[g].Value; return ""; }
        /// <summary>= fs_lb：片元 / 计算着色器 main0 里经单个采样器成员的每次采样加该采样器的偏置 b = spvDescriptorSet0.s2a_lb[成员 id]：level(l) → level((l) + b)、bias(x) → bias((x) + b)、
        /// gradient → 两个导数 × precise::exp2(b)、没写 LOD → 片元插 bias(b) / 计算插 level(b)（计算只带 min_lod_clamp(m) 的 → level(max(b, m))）；参数块末尾加 constant float* s2a_lb。没有可改的采样原样返回。
        /// 深度比较采样 sample_compare 同样改（缺口族 31：本机不吃采样器偏置时比较采样的偏置原来丢掉），LOD 参数排在参考值之后；
        /// 它的参考值另按表后半段 s2a_lb[LB_DC + 成员 id] 决定要不要 saturate 到 [0,1]（#529 定点深度比较语义）</summary>
        internal static string FsLb(string src)
        {
            var ms = Members(src); var ent = s_redEntry.Match(src);
            if (ms == null || !ent.Success) return src;
            var (body, tex, smp) = ms.Value;
            if (tex.Count == 0 || smp.Count == 0 || Search(@"\b" + LB + @"\s*\[\[id\(", body.Groups[1].Value).Success) return src;
            long nid = NextId(body.Groups[1].Value);
            if (nid > LB_MAXID) return src;
            bool frag = ent.Groups[2].Value == "fragment";
            int b0 = ent.Index + ent.Length, b1 = src.IndexOf("\n}\n", b0 - 1, StringComparison.Ordinal);
            if (b1 < 0) return src;
            string main = src[b0..(b1 + 1)]; int n = 0;
            foreach (int st in Starts(main, tex, s_lbCall))
            {
                var ca = CallAt(main, st, s_lbCall);
                if (ca == null) continue;
                var (i, args, t) = ca.Value;
                string head = s_lbCall.Match(main, st).Value;   // 调用头原样（含 spvDepthCast( … ) 包法与方法名、左括号）：改写时照抄
                bool cmp = head.EndsWith(".sample_compare(", StringComparison.Ordinal);
                string kind = tex[t].Kind; int k = (kind.EndsWith("_array", StringComparison.Ordinal) ? 3 : 2) + (cmp ? 1 : 0);   // 比较采样：参考值在 LOD 之前
                var sm = args.Count > 0 ? Full(@"spvDescriptorSet0\.(\w+)", args[0]) : null;
                if (sm == null || !sm.Success || !smp.ContainsKey(sm.Groups[1].Value) || args.Count < k) continue;
                string b = "spvDescriptorSet0." + LB + "[" + smp[sm.Groups[1].Value] + "]";
                var lods = new List<(int J, Match M)>();
                for (int j = k; j < args.Count; j++) { var lm = Full(RedLod, args[j], RegexOptions.Singleline); if (lm.Success) lods.Add((j, lm)); }
                if (lods.Any(x => !Balanced(x.M.Groups[2].Value))) continue;
                var lod = lods.Where(x => x.M.Groups[1].Value != "min_lod_clamp").ToList();
                var na = new List<string>(args);
                if (cmp) na[k - 1] = "(spvDescriptorSet0." + LB + "[" + (LB_DC + smp[sm.Groups[1].Value]) + "] != 0.0 ? saturate(" + na[k - 1] + ") : (" + na[k - 1] + "))";   // #529
                if (lod.Count > 0)
                {
                    var (j, lm) = lod[0]; string nm = lm.Groups[1].Value, inner = lm.Groups[2].Value;
                    if (nm == "level" || nm == "bias") na[j] = nm + "((" + inner + ") + " + b + ")";
                    else
                    {
                        var g = Split2(inner);
                        if (g == null) continue;
                        na[j] = nm + "((" + g.Value.Item1 + ") * precise::exp2(" + b + "), (" + g.Value.Item2 + ") * precise::exp2(" + b + "))";
                    }
                }
                else if (!frag && lods.Count > 0) { var (j, lm) = lods[0]; na[j] = "level(max(" + b + ", " + lm.Groups[2].Value + "))"; }   // 计算、只带 min_lod_clamp(m)：level(max(b, m))
                else na.Insert(k, frag ? "bias(" + b + ")" : "level(" + b + ")");
                main = main[..st] + head + string.Join(", ", na) + ")" + main[i..]; n++;
            }
            if (n == 0) return src;
            src = src[..b0] + main + src[(b1 + 1)..];
            var nb = Search(@"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", src, RegexOptions.Singleline);   // 结构体在 main0 之前：位置没动
            int e = nb.Groups[1].Index + nb.Groups[1].Length;
            return src[..e] + "    constant float* " + LB + " [[id(" + nid + ")]];\n" + src[e..];
        }
        /// <summary>main0 入口之前（辅助函数里）有经参数块贴图或采样器参数的采样：FsLb / FsRd 只改 main0，这类采样不加偏置、不进归约（审计 1101 18:00 P2-2）。
        /// SPIRV-Cross 把采样内联进 main0，11-01 动森真库 6031 支片元 / 计算一处都没有；没有读数前不改改写范围，出了就记一条反射笔记作读数</summary>
        internal static bool HelperSamples(string src)
        {
            var ent = s_redEntry.Match(src);
            if (!ent.Success) return false;
            string head = src[..ent.Index];
            return s_lbCall.IsMatch(head) || Search(@"\.\s*(?:sample|sample_compare)\(\s*\w+Smplr\b", head).Success;
        }
        /// <summary>= fs_rd：采样模拟变体（缺口族 31）。main0 里经单个采样器成员采浮点贴图的每次 sample 改成 s2a_bs(贴图, 采样器, s2a_red[id], 其余参数…)：表项 m.w ≠ 0 时用表里两个采样器
        /// （透明黑 / 不透明白边界）各采一次按边界色混合（任意边界色），否则原样采。其中显式 LOD 采 2D / 2D 数组的再包成「(s2a_red[id].m.x != 0.0 ? (s2a_rs(...)) : (s2a_bs(...)))」（min / max 归约），
        /// LOD 取 FsLb 留下的 level((l) + b) 里的 l（或原样 level(l)）；FsLb 给隐式采样插的 level(b) / level(max(b, m)) 不归约。版本行 + RED_HELP + main0 末尾加表参数；没有可改的原样返回</summary>
        internal static string FsRd(string src)
        {
            var ms = Members(src); var ent = s_redEntry.Match(src);
            if (ms == null || !ent.Success || Has(src, RED_BEGIN)) return src;
            var (_, tex, smp) = ms.Value;
            if (tex.Count == 0 || smp.Count == 0 || Has(ent.Groups[4].Value, $"[[buffer({RED_SLOT})]]")) return src;
            int b0 = ent.Index + ent.Length, b1 = src.IndexOf("\n}\n", b0 - 1, StringComparison.Ordinal);
            if (b1 < 0) return src;
            string main = src[b0..(b1 + 1)]; int n = 0;
            foreach (int st in Starts(main, tex))
            {
                var ca = CallAt(main, st);
                if (ca == null) continue;
                var (i, args, t) = ca.Value;
                var (kind, elem) = tex[t]; bool arr = kind.EndsWith("_array", StringComparison.Ordinal); int k = arr ? 3 : 2;
                var sm = args.Count > 0 ? Full(@"spvDescriptorSet0\.(\w+)", args[0]) : null;
                if (sm == null || !sm.Success || !smp.ContainsKey(sm.Groups[1].Value) || args.Count < 2 || elem != "float") continue;
                long sid = smp[sm.Groups[1].Value]; string q = RED + "[" + sid + "]";
                string bs = "s2a_bs(spvDescriptorSet0." + t + ", " + string.Join(", ", args.Take(1).Append(q).Concat(args.Skip(1))) + ")";
                string lv = null;
                if ((args.Count == k + 1 || args.Count == k + 2) && (kind == "texture2d" || kind == "texture2d_array"))
                {
                    var lb = Full(LbLevel, args[k], RegexOptions.Singleline);
                    if (lb.Success && Int(lb.Groups[2].Value) == sid && Balanced(lb.Groups[1].Value)) lv = lb.Groups[1].Value;
                    else
                    {
                        var pl = Full(@"level\((.*)\)", args[k], RegexOptions.Singleline);
                        if (pl.Success && Balanced(pl.Groups[1].Value) && !Has(pl.Groups[1].Value, LB + "[")) lv = pl.Groups[1].Value;
                    }
                }
                string rw = bs;
                if (lv != null)
                {
                    string rs = "s2a_rs(spvDescriptorSet0." + t + ", " + args[1] + ", " + (arr ? args[2] : "0u") + ", " + lv + ", " + (args.Count > k + 1 ? args[k + 1] : "int2(0)") + ", " + q + ")";
                    rw = "(" + q + ".m.x != 0.0 ? (" + rs + ") : (" + bs + "))";
                }
                main = main[..st] + rw + main[i..]; n++;
            }
            if (n == 0) return src;
            int p1 = b0 - ")\n{\n".Length;
            return RED_BEGIN + src[..ent.Index] + RedHelp + "\n" + src[ent.Index..p1] + ", constant S2ASe* " + RED + " [[buffer(" + RED_SLOT + ")]]" + src[p1..b0] + main + src[(b1 + 1)..];
        }
        // 变体的辅助：表项 S2ASe（= nvn_red.h nvn_se_ent）、寻址折回 s2a_rw、取纹素 s2a_rt、边界色混合 s2a_bmix / s2a_bs、足迹归约 s2a_rs
        //   s2a_rs：采样器 LOD = clamp(lod + 偏置, 夹取)，> 0 用缩小过滤、否则放大过滤（l.z：缩小线性 1 + 放大线性 2 + mip 方式 × 4）；mip 无 → 第 0 级、最近 → ceil(l − 0.5) 一级、线性 → 小数时两级；
        //   每级最近点取 floor(c × 尺寸) 一个纹素、线性取双线性足迹里权重非零的纹素（坐标小数为 0 的那侧不参与），加偏移、按寻址折回，越界取边界色，逐纹素 min / max
        //   （11-01 M5 Max 上 Metal 26 原生 reductionMode 十二种过滤组合都等于这个定义）
        const string RedHelp = "struct S2ASe { float4 m; float4 l; float4 c; sampler s0; sampler s1; };\n"
            + "static inline int s2a_rw(int i, int n, int m) { if (m == 2) { int r = i % n; return r < 0 ? r + n : r; } "
            + "if (m == 3) { int p = 2 * n, r = i % p; r = r < 0 ? r + p : r; return r < n ? r : p - 1 - r; } "
            + "if (m == 1) return min(i < 0 ? -1 - i : i, n - 1); if (m == 5) return i < 0 || i >= n ? -1 : i; return clamp(i, 0, n - 1); }\n"
            + "static inline float4 s2a_rt(texture2d<float> t, uint2 p, uint a, uint l) { return t.read(p, l); }\n"
            + "static inline float4 s2a_rt(texture2d_array<float> t, uint2 p, uint a, uint l) { return t.read(p, a, l); }\n"
            + "static inline float4 s2a_bmix(float4 a, float4 b, float4 c) { return a + (b - a) * c; }\n"
            + "static inline float s2a_bmix(float a, float b, float4 c) { return a + (b - a) * c.x; }\n"
            // 折行贴图（l.z ≥ 16，宿主 nvn_red.h nvn_se_fold）：超 Metal 2D 上限的高线性贴图，宿主按 k 行折成一行建在池缓冲上（行距 k 倍、高 ⌈H/k⌉，逻辑纹素 (x, y) 在 (x + (y % k)·P, y / k)，
            //   P = 原行距的纹素数），c = (W, H, k, 边界色档)、m.w = P；这里按采样器的过滤 / 寻址 / LOD 规则逐纹素取（s2a_ff），LOD 来自 level / gradient2d / 隐式时 2×2 四元组的邻格坐标差（quad_shuffle_xor，片元与计算都能编；dfdx 计算核编不过），偏置 bias 与表里 l.w 相加。只 2D / 2D 数组浮点贴图的 sample
            + "struct S2AFo { float l; float b; float mn; float2 gx; float2 gy; int2 o; int ex; };\n"
            + "static inline void s2a_fa(thread S2AFo& f) {}\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, bias v, A... r);\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, level v, A... r);\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, gradient2d v, A... r);\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, int2 v, A... r);\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, bias v, A... r) { f.b += v.value; s2a_fa(f, r...); }\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, level v, A... r) { f.l = v.lod; f.ex = 1; s2a_fa(f, r...); }\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, gradient2d v, A... r) { f.gx = v.dPdx; f.gy = v.dPdy; f.ex = 2; s2a_fa(f, r...); }\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, int2 v, A... r) { f.o = v; s2a_fa(f, r...); }\n"
            + "#if defined(__HAVE_MIN_LOD_CLAMP__)\n"
            + "template<typename... A> static inline void s2a_fa(thread S2AFo& f, min_lod_clamp v, A... r) { f.mn = v.value; s2a_fa(f, r...); }\n"
            + "#endif\n"
            + "static inline float4 s2a_fr(texture2d<float> t, int x, int y, int p, int k, uint a) { return t.read(uint2(uint(x + (y % k) * p), uint(y / k))); }\n"
            + "static inline float4 s2a_fr(texture2d_array<float> t, int x, int y, int p, int k, uint a) { return t.read(uint2(uint(x + (y % k) * p), uint(y / k)), min(a, t.get_array_size() - 1u)); }\n"
            + "template<typename T> static inline float4 s2a_ff(T t, constant S2ASe& q, float2 c, uint a, thread const S2AFo& f)\n"
            + "{ int w = int(q.c.x), h = int(q.c.y), k = int(q.c.z), p = int(q.m.w), fm = int(q.l.z) - 16; float2 sz = float2(float(w), float(h));\n"
            + "  float lod = f.ex == 1 ? f.l : f.ex == 2 ? log2(max(length(f.gx * sz), length(f.gy * sz))) : log2(max(length((quad_shuffle_xor(c, ushort(1)) - c) * sz), length((quad_shuffle_xor(c, ushort(2)) - c) * sz)));\n"
            + "  float ls = clamp(max(lod + f.b + q.l.w, f.mn), q.l.x, q.l.y); bool li = (ls > 0.0f ? (fm & 1) : (fm & 2)) != 0;\n"
            + "  float fx = c.x * sz.x - (li ? 0.5f : 0.0f), fy = c.y * sz.y - (li ? 0.5f : 0.0f), x0 = floor(fx), y0 = floor(fy), ax = li ? fx - x0 : 0.0f, ay = li ? fy - y0 : 0.0f;\n"
            + "  int2 b = int2(int(x0), int(y0)) + f.o; float4 bc = q.c.w > 1.5f ? float4(1.0f) : float4(0.0f, 0.0f, 0.0f, q.c.w > 0.5f ? 1.0f : 0.0f), r = float4(0.0f);\n"
            + "  for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) { float wt = (i ? ax : 1.0f - ax) * (j ? ay : 1.0f - ay); if (wt == 0.0f) continue;\n"
            + "    int xi = s2a_rw(b.x + i, w, int(q.m.y)), yj = s2a_rw(b.y + j, h, int(q.m.z)); r += wt * (xi < 0 || yj < 0 ? bc : s2a_fr(t, xi, yj, p, k, a)); }\n"
            + "  return r; }\n"
            + "template<typename T, typename... A> static inline auto s2a_fx(T t, sampler s, constant S2ASe& q, A... a) -> decltype(t.sample(s, a...)) { return t.sample(s, a...); }\n"
            + "template<typename... A> static inline float4 s2a_fx(texture2d<float> t, sampler s, constant S2ASe& q, float2 c, A... r) { S2AFo f = {}; s2a_fa(f, r...); return s2a_ff(t, q, c, 0u, f); }\n"
            + "template<typename... A> static inline float4 s2a_fx(texture2d_array<float> t, sampler s, constant S2ASe& q, float2 c, uint a, A... r) { S2AFo f = {}; s2a_fa(f, r...); return s2a_ff(t, q, c, a, f); }\n"
            + "template<typename T, typename... A> static inline auto s2a_bs(T t, sampler s, constant S2ASe& q, A... a) -> decltype(t.sample(s, a...))\n"
            + "{ return q.l.z > 15.5f ? s2a_fx(t, s, q, a...) : q.m.w != 0.0f ? s2a_bmix(t.sample(q.s0, a...), t.sample(q.s1, a...), q.c) : t.sample(s, a...); }\n"
            + "kernel void s2a_se_fold() {}\n"   // 能读折行贴图的标记：库的函数表里有它（宿主 nvn_mtl.m se_lib_fold），包里预编的 / 设备上现编的都认；旧版变体没有，宿主不给它绑折行贴图
            + "template<typename T> static inline float4 s2a_rs(T t, float2 c, uint a, float lod, int2 o, constant S2ASe& q)\n"
            + "{ bool mx = q.m.x > 1.5f, f0 = true; int fm = int(q.l.z), mp = fm >> 2; float4 r = float4(0.0f); float ls = clamp(lod + q.l.w, q.l.x, q.l.y);\n"
            + "  bool li = (ls > 0.0f ? (fm & 1) : (fm & 2)) != 0; float l = mp == 0 ? 0.0f : clamp(ls, 0.0f, float(t.get_num_mip_levels()) - 1.0f); if (mp == 1) l = ceil(l - 0.5f); float l0 = floor(l);\n"
            + "  for (uint k = 0; k < (l > l0 ? 2u : 1u); k++) { uint lv = uint(l0) + k; int w = int(t.get_width(lv)), h = int(t.get_height(lv));\n"
            + "    float fx = c.x * float(w) - (li ? 0.5f : 0.0f), fy = c.y * float(h) - (li ? 0.5f : 0.0f), x0 = floor(fx), y0 = floor(fy); int2 b = int2(int(x0), int(y0)) + o;\n"
            + "    for (int j = 0; j < (li && fy > y0 ? 2 : 1); j++) for (int i = 0; i < (li && fx > x0 ? 2 : 1); i++) { int xi = s2a_rw(b.x + i, w, int(q.m.y)), yj = s2a_rw(b.y + j, h, int(q.m.z));\n"
            + "      float4 v = xi < 0 || yj < 0 ? float4(q.c) : s2a_rt(t, uint2(xi, yj), a, lv); r = f0 ? v : mx ? max(r, v) : min(r, v); f0 = false; } }\n"
            + "  return r; }\n";

        // ---- fs_uvx（S2Arm 宽屏 16:9 采样换算，dmsh_build.py 同名函数，逐字同输出）----
        const string UVX = "s2a_uvx";   // 参数块成员：每个贴图成员一个 float4，按该成员的 [[id]] 取
        const string UvxHelp = "static inline float2 s2a_uv(float2 c, float4 q) { return float2(fma(c.x, q.x, c.x) + q.y, fma(c.y, q.z, c.y) + q.w); }\n";   // 与 dmsh_build.py UVX_HELP 逐字一致（10-28 B06 纵向）
        static readonly Regex s_uvxCall = R(@"spvDescriptorSet0\.(\w+)\.(sample|sample_compare|gather|gather_compare)\(");
        // ---- 片元 Alpha 测试 / 多重采样关（缺口族 34 / 37，11-01）：主入口 main0 带两个可选函数常量，宿主不给就未定义、整段被编译掉 ----
        //   S2A_AT（int，100）= NVNalphaFunc（1 NEVER … 8 ALWAYS），给了才比较颜色输出 0 的 a 与参考值；参考值不是常量（SetAlphaRef 是命令缓冲动态状态，做常量每个值一条管线），
        //   从片元缓冲槽 30 读（s2a_aref，无条件参数：成批合编共用函数体 dmsh_ushare 原样传参，带常量条件的参数在未定义时不能被引用；只在常量开着时解引用，宿主不开就不用绑）。
        //   S2A_MSOFF（bool，101）= true：写满 [[sample_mask]]（NVN 关多重采样、附件却是多样本；Metal 光栅样本数必须等于附件样本数）。不写颜色 0 时按 Ryujinx 语义只有 NEVER 丢弃。
        //   声明压成一行（FC_DECL），成批合编把它提到批源码顶部只声明一次（dmsh_build.py unity_src _UG）。main0_u 由 FsUint 从这份复制，同样带上
        internal const string FC_DECL = "constant int S2A_AT [[function_constant(100)]]; constant bool S2A_AT_ON = is_function_constant_defined(S2A_AT); "
            + "constant bool S2A_MSOFF [[function_constant(101)]]; constant bool S2A_MSOFF_ON = is_function_constant_defined(S2A_MSOFF) ? S2A_MSOFF : false; "
            + "static inline bool s2a_alpha_pass(int f, float a, float r) { return f == 8 || (f == 2 && a < r) || (f == 3 && a == r) || (f == 4 && a <= r) || (f == 5 && a > r) || (f == 6 && a != r) || (f == 7 && a >= r); }";
        internal const int AREF_SLOT = 30;   // 片元缓冲槽：0 参数块、26 采样模拟表（RED_SLOT）、27 半精度颜色（NVN_HALF_SLOT / GAL_COLOR_ROUND_SLOT）；通用模式片元 28 边框色、29 线宽 / 逻辑运算（gal_internal.h），30 全仓没人用（t_alpha_ms ④ 枚举核）
        internal static string FsAlphaMs(string src)
        {
            var sig = Search(@"^((?:\[\[[^\]]*\]\]\s*)*)fragment main0_out main0\((.*)\)\n\{\n", src, RegexOptions.Multiline);
            var st = Search(@"\nstruct main0_out\n\{\n(.*?)\n\};", src, RegexOptions.Singleline);
            int ui = src.IndexOf("using namespace metal;\n", StringComparison.Ordinal);
            if (!sig.Success || !st.Success || ui < 0 || src.Contains("S2A_AT_ON", StringComparison.Ordinal)) return src;
            // 池读改成早测的片元（PoolArrays）：深度在着色前就写了，函数常量里的 alpha 丢弃 / 样本掩码改不回来，不加——宿主要 alpha 测试时报红 134（「drawn without them」），不悄悄写错深度
            if (sig.Groups[1].Value.Contains("early_fragment_tests", StringComparison.Ordinal) && src.Contains("dm_pool_mark(", StringComparison.Ordinal)) return src;
            int open = sig.Index + sig.Length, close = -1, depth = 1;   // main0 的函数体：按花括号配对（SPIRV-Cross 每个花括号各占一行）
            for (int i = open; i < src.Length; i++) { if (src[i] == '{') depth++; else if (src[i] == '}' && --depth == 0) { close = i; break; } }
            if (close < 0) return src;
            var c0 = Search(@"^\s*(\w+)\s+(\w+)\s*\[\[color\(0\)\]\];", st.Groups[1].Value, RegexOptions.Multiline);
            string pass = c0.Success ? $"s2a_alpha_pass(S2A_AT, float(out.{c0.Groups[2].Value}.w), s2a_aref[0])" : "S2A_AT != 1";
            string body = src[open..close].Replace("return out;",
                "{ if (S2A_MSOFF_ON) out.s2a_smask = 0xffffffffu; if (S2A_AT_ON && !(" + pass + ")) discard_fragment(); return out; }");
            string prm = sig.Groups[2].Value, ext = (prm.Trim().Length > 0 ? ", " : "") + $"constant float* s2a_aref [[buffer({AREF_SLOT})]]";
            string head = src[..sig.Index] + sig.Groups[1].Value + "fragment main0_out main0(" + prm + ext + ")\n{\n";
            string outp = head + body + src[close..];
            int se = outp.IndexOf("\nstruct main0_out\n{\n", StringComparison.Ordinal), sx = outp.IndexOf("\n};", se + 1, StringComparison.Ordinal);
            outp = outp[..sx] + "\n    uint s2a_smask [[sample_mask, function_constant(S2A_MSOFF_ON)]];" + outp[sx..];
            int u2 = outp.IndexOf("using namespace metal;\n", StringComparison.Ordinal) + "using namespace metal;\n".Length;
            return outp[..u2] + "\n" + FC_DECL + "\n" + outp[u2..];
        }

        // ---- 片元输出类型由函数常量选（#427，11-19）：主入口 main0 再带两个可选常量，宿主建管线时按画布格式特化（nvn_mtl.m fs_atms_fn） ----
        //   S2A_FU（uint，102）位 n = color(n) 的画布是无符号整数、S2A_FS（uint，103）位 n = 有符号整数；S2A_FS 位 8 = 画布 0 用第二源混合（宿主 PSO_FOUT_DUAL >> 8，同 FsTyped）。
        //   原先这些靠另外的入口：整数位刚好等于 u_mask 的用 main0_u（FsUint，整份函数体复制一遍），别的掩码靠出包按种子表预编的 _ft，种子表里没有的在设备提交线程上
        //   从源码现编（#427 空之轨迹 the 1st：G-buffer 三张整数目标、掩码 0xe，一局 7 次各 90–157 ms）。常量版：每个浮点颜色输出旁加同位置的 uint / int 成员（各由一个
        //   派生常量开关，三者互斥），返回前按位原样取（as_type，同 FsRetyped：硬件往整数画布写的就是输出寄存器的位）；函数体不动、不复制。宿主不给常量 = 全浮点 = 原样。
        //   声明接在 FC_DECL 那一行末尾（成批合编按整行提到批顶只出一次，Unity.cs UG）；FsAlphaMs 没加声明的（不是片元主入口、早测池读）原样返回，宿主照旧走旧路
        internal const string FOUT_MARK = "constant uint S2A_FU [[function_constant(102)]];";
        internal static readonly string FOUT_DECL = " " + FOUT_MARK + " constant uint S2A_FS [[function_constant(103)]]; "
            + "constant uint S2A_FU_V = is_function_constant_defined(S2A_FU) ? S2A_FU : 0u; constant uint S2A_FS_V = is_function_constant_defined(S2A_FS) ? S2A_FS : 0u; "
            + "constant bool S2A_DUAL = (S2A_FS_V & 0x100u) != 0u;"
            + string.Concat(Enumerable.Range(0, 8).Select(n => { string d = n == 1 ? "!S2A_DUAL && " : ""; uint b = 1u << n;   // 双源时输出 1 改去 [[color(0), index(1)]]
                return $" constant bool S2A_U{n} = {d}(S2A_FU_V & {b}u) != 0u; constant bool S2A_I{n} = {d}!S2A_U{n} && (S2A_FS_V & {b}u) != 0u; constant bool S2A_F{n} = {d}!S2A_U{n} && !S2A_I{n};"; }));
        internal static string FsFout(string src)
        {
            int dl = src.IndexOf("constant int S2A_AT [[function_constant(100)]];", StringComparison.Ordinal);
            if (dl < 0 || src.Contains(FOUT_MARK, StringComparison.Ordinal)) return src;
            var sig = Search(@"^(?:\[\[[^\]]*\]\]\s*)*fragment main0_out main0\(.*\)\n\{\n", src, RegexOptions.Multiline);
            var st = Search(@"\nstruct main0_out\n\{\n(.*?)\n\};", src, RegexOptions.Singleline);
            if (!sig.Success || !st.Success) return src;
            int open = sig.Index + sig.Length, close = -1, depth = 1;   // main0 的函数体（同 FsAlphaMs 按花括号配对）
            for (int i = open; i < src.Length; i++) { if (src[i] == '{') depth++; else if (src[i] == '}' && --depth == 0) { close = i; break; } }
            if (close < 0) return src;
            var cols = All(@"^    (float\d?) (\w+) \[\[color\(([0-7])\)\]\];$", st.Groups[1].Value, RegexOptions.Multiline)
                .Select(m => (Ty: m.Groups[1].Value, Nm: m.Groups[2].Value, N: int.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture))).ToList();
            var mem = new StringBuilder(); var conv = new StringBuilder();
            foreach (var (ty, nm, n) in cols)
            {
                string uty = ty.Replace("float", "uint"), ity = ty.Replace("float", "int");
                mem.Append($"\n    {uty} {nm}_s2u [[color({n}), function_constant(S2A_U{n})]];\n    {ity} {nm}_s2i [[color({n}), function_constant(S2A_I{n})]];");
                conv.Append($"if (S2A_U{n}) out.{nm}_s2u = as_type<{uty}>(out.{nm}); if (S2A_I{n}) out.{nm}_s2i = as_type<{ity}>(out.{nm}); ");
                if (n == 1) { mem.Append($"\n    {ty} {nm}_s2d [[color(0), index(1), function_constant(S2A_DUAL)]];"); conv.Append($"if (S2A_DUAL) out.{nm}_s2d = out.{nm}; "); }
            }
            var c0 = cols.FirstOrDefault(c => c.N == 0);
            if (c0.Nm != null && !cols.Any(c => c.N == 1))   // 不写输出 1 的：第二源恒 0（同 FsTyped / Ryujinx 开双源时先把输出 1 置 0）
            { mem.Append($"\n    {c0.Ty} s2a_src1 [[color(0), index(1), function_constant(S2A_DUAL)]];"); conv.Append($"if (S2A_DUAL) out.s2a_src1 = {c0.Ty}(0); "); }
            var g = st.Groups[1]; int eol = src.IndexOf('\n', dl);
            if (!(eol < g.Index && g.Index + g.Length < open)) return src;   // 声明行、结构体、main0 依次在前（SPIRV-Cross 的次序）；不是就不动
            string members = R(@"^    (float\d?) (\w+) \[\[color\(([0-7])\)\]\];$", RegexOptions.Multiline)
                .Replace(g.Value, m => $"    {m.Groups[1].Value} {m.Groups[2].Value} [[color({m.Groups[3].Value}), function_constant(S2A_F{m.Groups[3].Value})]];") + mem;
            string body = src[open..close].Replace("return out;", conv + "return out;");
            return src[..eol] + FOUT_DECL + src[eol..g.Index] + members + src[(g.Index + g.Length)..open] + body + src[close..];
        }

        internal static string FsUvx(string src)
        {
            var body = Search(@"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", src, RegexOptions.Singleline);
            if (!body.Success) return src;
            var tex = new Dictionary<string, long>();
            foreach (Match m in All(@"^\s*(?:texture2d|depth2d)(?:_array)?<\w+>\s+(\w+)\s*" + IdAttr, body.Groups[1].Value, RegexOptions.Multiline)) tex[m.Groups[1].Value] = Int(m.Groups[2].Value);
            if (tex.Count == 0) return src;
            var starts = new List<int>();
            foreach (Match m in s_uvxCall.Matches(src)) if (tex.ContainsKey(m.Groups[1].Value)) starts.Add(m.Index);
            starts.Reverse();   // 从后往前：外层坐标里套着的内层访问先改
            int n = 0;
            foreach (int st in starts)
            {
                var m = s_uvxCall.Match(src, st);
                if (!m.Success || m.Index != st) continue;
                int i = m.Index + m.Length, a0 = i, depth = 1; var args = new List<(int A, int B)>();
                while (depth > 0 && i < src.Length)
                {
                    char c = src[i]; depth += c == '(' || c == '[' ? 1 : 0; depth -= c == ')' || c == ']' ? 1 : 0;
                    if ((c == ',' && depth == 1) || depth == 0) { args.Add((a0, i)); a0 = i + 1; }
                    i++;
                }
                if (args.Count < 2) continue;
                var (a, b) = args[1]; string arg = src[a..b]; int lead = arg.Length - arg.TrimStart().Length;
                src = src[..(a + lead)] + "s2a_uv(" + src[(a + lead)..b].Trim() + ", spvDescriptorSet0." + UVX + "[" + tex[m.Groups[1].Value] + "])" + src[b..]; n++;
            }
            if (n == 0) return src;
            body = Search(@"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", src, RegexOptions.Singleline);
            long nid = NextId(body.Groups[1].Value) - 1;   // 同 FsLb：数组成员占 n 个 id、带第二属性的成员也算（原先取最大 id：数组排在最后时新成员撞它的第 2 个元素；审计 1101 18:00 P1-18 同族）
            int e1 = body.Groups[1].Index + body.Groups[1].Length, e2 = body.Index + body.Length;
            return src[..e1] + "    constant float4* " + UVX + " [[id(" + (nid + 1) + ")]];\n" + src[e1..e2] + "\n\n" + UvxHelp + src[e2..];
        }

        // 同 dmsh_build.py vs_tf：缓冲 b 的各条依次按分量存，每个顶点 步长 / 4 个字
        internal static string VsTf(string src, JsonArray tf)
        {
            var m = Search(@"\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{", src, RegexOptions.Singleline);
            if (!m.Success) throw new DshError("MSL failed", "vertex shader without a main0 main function, cannot add the transform feedback entry point");
            string pars = m.Groups[1].Value, body = src[(m.Index + m.Length)..];
            var vm = Search(@"uint\s+(\w+)\s*\[\[vertex_id\]\]", pars); string vid = vm.Success ? vm.Groups[1].Value : "dm_vid";
            var st = Search(@"struct main0_out\s*\{(.*?)\};", src, RegexOptions.Singleline);
            var names = new Dictionary<int, string>();
            foreach (Match x in All(@"float4 (\w+) \[\[user\(locn(\d+)\)\]\]", st.Success ? st.Groups[1].Value : "")) names[Int(x.Groups[2].Value)] = x.Groups[1].Value;
            var stores = new StringBuilder(); var extra = new StringBuilder(vm.Success ? "" : ", uint dm_vid [[vertex_id]]");
            for (int b = 0; b < tf.Count; b++)
            {
                if (tf[b] is not JsonArray e || e.Count == 0) continue;
                int stride = (int)(long)e[0], w = 0;   // 布局值是 long（TfJson）：JsonNode 直接转 int 会抛（异度3 8 支 vs，11-01）
                foreach (var en in e[1].AsArray())
                {
                    int o = (int)(long)en[0], n = (int)(long)en[1];
                    for (int i = 0; i < n; i++, w++)
                        stores.Append($" dm_tf{b}[{vid} * {stride / 4} + {w}] = {(names.TryGetValue(o, out var nm) ? $"as_type<uint>(out.{nm}.{"xyzw"[i]})" : "0u")};");
                }
            }
            for (int b = 0; b < tf.Count; b++) if (tf[b] is JsonArray e && e.Count > 0) extra.Append($", device uint* dm_tf{b} [[buffer({TF_SLOT + b})]]");
            return $"\nvertex void main0_tf({pars}{extra})\n{{" + Replace(body, "return out;", "{" + stores + " return; }");
        }

        // main0 的级间接口 user(locn) 位置：顶点写的（main0_out，vs_pad 之后）/ 片元读的（main0_in）；别的阶段 null（同 dmsh_build.py user_locs）
        internal static List<object> UserLocs(string src, string stage)
        {
            string name = stage == "vs" ? "main0_out" : stage == "ps" ? "main0_in" : null;
            if (name == null) return null;
            int i = src.IndexOf("struct " + name + "\n{", StringComparison.Ordinal);
            if (i < 0) return new List<object>();
            int e = src.IndexOf("\n};", i, StringComparison.Ordinal); string blk = e < 0 ? src[i..] : src[i..e];
            return All(@"\[\[user\(locn(\d+)\)", blk).Select(m => Int(m.Groups[1].Value)).Distinct().OrderBy(x => x).Select(x => (object)x).ToList();
        }
        // 同 dmsh_build.py _fn_body：vertex 入口 entry 的函数体（花括号配对；找不到返回 null）
        static string FnBody(string src, string entry)
        {
            var m = Search(@"\bvertex\s+main0_out\s+" + Esc(entry) + @"\s*\(", src);
            if (!m.Success) return null;
            int i = Find(src, ")\n{", m.Index + m.Length);
            if (i < 0) return null;
            i += 2; int d = 0;
            for (int j = i; j < src.Length; j++)
            {
                if (src[j] == '{') d++;
                else if (src[j] == '}') { d--; if (d == 0) return src.Substring(i + 1, j - (i + 1)); }
            }
            return null;
        }
        // 屏幕坐标插值量（10-30 帧率能耗线，原同 dmsh_build.py scr_uv_locns）：插值量 locn L 是不是「这个顶点裁剪坐标对应的屏幕坐标」
        //   （.x = (P.x + P.w) * 0.5、.y = (-P.y + P.w) * 0.5、.w = P.w）；只认函数顶层的赋值，main0 与 main0_z 都要成立（缺 main0 = 不成立）。返回升序的 locn 列表。
        // 11-07 卡顿线：原先按文本模板认（P.* 必须是单个临时量、_a = _b * 0.5、_b = _c + _d 逐层），关 DebugMode 后表达式内联成 (_237 + _239) * 0.5，
        //   旷野之息 819 支顶点着色器识别全落空。改成形态无关：各分量右边解析成线性式（原子 → 系数 + 常数），只代回「顶层单次定义、块里没再赋值」的临时量，
        //   非线性片段（函数调用、两个非常量相乘）按文本当原子；判定 = 线性式相等。两种写法、任意内联程度结论相同
        internal static List<object> ScrUvLocns(string src) => ScrLocns(src).Uv;
        // #1152：插值量 locn L 是不是「本顶点裁剪坐标本身」（.x = P.x、.y = P.y、.w = P.w，main0 与 main0_z 都成立）：片元自己做 (x / w) * 0.5 + 0.5、
        //   (y / w) * -0.5 + 0.5（异度之刃 3 / DE 的 G-buffer 贴花、灯光），算出来就是本像素的屏幕坐标，与 scr_uv 同一件事换个地方算。返回升序 locn 列表
        internal static List<object> ScrPosLocns(string src) => ScrLocns(src).Pos;
        internal static (List<object> Uv, List<object> Pos) ScrLocns(string src)
        {
            var none = (new List<object>(), new List<object>());
            static (string Top, List<string> Blocks) SplitDepth(string body)
            {
                var top = new StringBuilder(); var blocks = new List<string>(); var cur = new StringBuilder(); int d = 0;
                foreach (char ch in body)
                {
                    if (ch == '{') { d++; if (d == 1) cur.Clear(); }
                    if (d == 0) top.Append(ch); else cur.Append(ch);
                    if (ch == '}') { d--; if (d == 0) { blocks.Add(cur.ToString()); top.Append(' '); } }
                }
                return (top.ToString(), blocks);
            }
            var ost = Search(@"struct main0_out\s*\{(.*?)\};", src, RegexOptions.Singleline);
            var locn = new List<(string Mem, int K)>(); var seen = new Dictionary<string, int>();
            foreach (Match m in All(@"(m_\d+) \[\[user\(locn(\d+)\)\]\]", ost.Success ? ost.Groups[1].Value : ""))
            {
                if (seen.ContainsKey(m.Groups[1].Value)) locn[seen[m.Groups[1].Value]] = (m.Groups[1].Value, Int(m.Groups[2].Value));   // dict 同键后写覆盖、位置不变
                else { seen[m.Groups[1].Value] = locn.Count; locn.Add((m.Groups[1].Value, Int(m.Groups[2].Value))); }
            }
            SortedSet<int> res = null, resP = null;
            foreach (string entry in new[] { "main0", "main0_z" })
            {
                string body = FnBody(src, entry);
                if (body == null) { if (entry == "main0") return none; continue; }
                var (top, blocks) = SplitDepth(body);
                var notDef = new HashSet<string>();   // 块里又赋过值的临时量：值随路径变，不能代回
                foreach (string blk in blocks)
                {
                    if (Has(blk, "s2a_push")) continue;
                    if (Search(@"out\.(gl_Position|m_\d+)\.?[xyzw]?\s*=[^=]", blk).Success) return none;
                    foreach (Match m in All(@"(?<![\w.])(_\d+)\s*=(?!=)", blk)) notDef.Add(m.Groups[1].Value);
                }
                var defs = new Dictionary<string, string>();
                foreach (Match m in All(@"(?<![\w.])(?:(?:float|int|uint|bool)\s+)?(_\d+)\s*=(?!=)\s*([^;]+);", top))
                {
                    string v = m.Groups[1].Value;
                    if (defs.ContainsKey(v)) notDef.Add(v); else defs[v] = m.Groups[2].Value.Trim();
                }
                foreach (string v in notDef) defs.Remove(v);
                string Last(string lhs) { string v = null; foreach (Match m in All(Esc(lhs) + @"\s*=(?!=)\s*([^;]+);", top)) v = m.Groups[1].Value.Trim(); return v; }
                var lin = new LinForm(defs);
                string px = Last("out.gl_Position.x"), py = Last("out.gl_Position.y"), pw = Last("out.gl_Position.w");
                if (px == null || py == null || pw == null) return none;
                var lx = lin.Of(px); var ly = lin.Of(py); var lw = lin.Of(pw);
                if (lx == null || ly == null || lw == null || lx.IsConst || ly.IsConst || lw.IsConst) return none;
                var wantX = lx.Scale(0.5).Plus(lw.Scale(0.5)); var wantY = ly.Scale(-0.5).Plus(lw.Scale(0.5));
                var ok = new SortedSet<int>(); var okP = new SortedSet<int>();
                foreach (var (mem, k) in locn)
                {
                    string vx = Last("out." + mem + ".x"), vy = Last("out." + mem + ".y"), vw = Last("out." + mem + ".w");
                    if (vx == null || vy == null || vw == null) continue;
                    if (lw.Same(lin.Of(vw)) && wantX.Same(lin.Of(vx)) && wantY.Same(lin.Of(vy))) ok.Add(k);
                    if (lw.Same(lin.Of(vw)) && lx.Same(lin.Of(vx)) && ly.Same(lin.Of(vy))) okP.Add(k);
                }
                if (res == null) res = ok; else res.IntersectWith(ok);
                if (resP == null) resP = okP; else resP.IntersectWith(okP);
            }
            return ((res ?? new SortedSet<int>()).Select(x => (object)(long)x).ToList(), (resP ?? new SortedSet<int>()).Select(x => (object)(long)x).ToList());
        }

        /// <summary>MSL 标量表达式的线性式（ScrUvLocns 用）：Σ 系数·原子 + 常数。+、−（二元 / 一元）、乘除常数、括号逐层解析；临时量 _N 在 defs 里就代回其定义
        /// （调用方保证 defs 只含「顶层单次定义、别处没再赋值」的），否则是原子；函数调用、成员 / 下标访问、两个非常量相乘等非线性片段按去空白后的文本当原子。
        /// 解析不了（括号不配等）返回 null</summary>
        internal sealed class LinForm
        {
            public sealed class L
            {
                public readonly SortedDictionary<string, double> A = new(StringComparer.Ordinal); public double C;
                public bool IsConst => A.Count == 0;
                public L Scale(double k) { var r = new L { C = C * k }; foreach (var kv in A) if (kv.Value * k != 0) r.A[kv.Key] = kv.Value * k; return r; }
                public L Plus(L o) { var r = Scale(1); r.C += o.C; foreach (var kv in o.A) { double v = r.A.GetValueOrDefault(kv.Key) + kv.Value; if (v == 0) r.A.Remove(kv.Key); else r.A[kv.Key] = v; } return r; }
                public bool Same(L o) => o != null && Math.Abs(C - o.C) < 1e-9 && A.Count == o.A.Count && A.All(kv => o.A.TryGetValue(kv.Key, out var v) && Math.Abs(v - kv.Value) < 1e-9);
                public string Text() => string.Join("+", A.Select(kv => kv.Value.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + "*" + kv.Key)) + "+" + C.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
            }
            readonly Dictionary<string, string> _defs; readonly Dictionary<string, L> _memo = new(); readonly HashSet<string> _busy = new();
            public LinForm(Dictionary<string, string> defs) { _defs = defs; }
            public L Of(string e) { try { return Expr(e.Trim()); } catch (FormatException) { return null; } }
            static bool IsNum(string t, out double v) => double.TryParse(t.TrimEnd('f', 'F'), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v) && Full(@"[0-9.]+(?:[eE][-+]?[0-9]+)?[fF]?", t).Success;
            // 按顶层（括号 / 方括号外）运算符切分；ops 是要切的字符，返回 (运算符, 片段)，首段运算符为 '\0'。一元负号不切（前面是开头或运算符）
            static List<(char Op, string Part)> Split(string e, string ops)
            {
                var r = new List<(char, string)>(); int d = 0, st = 0; char op = '\0';
                for (int i = 0; i < e.Length; i++)
                {
                    char c = e[i];
                    if (c == '(' || c == '[') d++;
                    else if (c == ')' || c == ']') { d--; if (d < 0) throw new FormatException(); }
                    else if (d == 0 && ops.IndexOf(c) >= 0)
                    {
                        if (c == '-' && i + 1 < e.Length && e[i + 1] == '>') { i++; continue; }   // 指针成员 ->
                        if (c == '-' || c == '+')
                        {
                            int p = i - 1; while (p >= 0 && e[p] == ' ') p--;
                            if (p < 0 || "+-*/(,?:".IndexOf(e[p]) >= 0) continue;   // 一元
                            if ((e[p] == 'e' || e[p] == 'E') && p > 0 && char.IsDigit(e[p - 1]) && Full(@".*?(?<![\w.])[0-9.]+[eE]", e[..(p + 1)]).Success) continue;   // 1.0e-5
                        }
                        r.Add((op, e[st..i].Trim())); op = c; st = i + 1;
                    }
                }
                if (d != 0) throw new FormatException();
                r.Add((op, e[st..].Trim())); return r;
            }
            static bool Wrapped(string e)
            {
                if (e.Length < 2 || e[0] != '(' || e[^1] != ')') return false;
                int d = 0; for (int i = 0; i < e.Length; i++) { if (e[i] == '(') d++; else if (e[i] == ')') { d--; if (d == 0 && i != e.Length - 1) return false; } }
                return true;
            }
            L Atom(string t) { var l = new L(); l.A[Regex.Replace(t, @"\s+", "")] = 1; return l; }
            L Expr(string e)
            {
                if (e.Length == 0) throw new FormatException();
                L acc = null;
                foreach (var (op, part) in Split(e, "+-"))
                {
                    var t = Term(part); acc = acc == null ? (op == '-' ? t.Scale(-1) : t) : acc.Plus(op == '-' ? t.Scale(-1) : t);
                }
                return acc;
            }
            L Term(string e)
            {
                var parts = Split(e, "*/");
                if (parts.Count == 1) return Factor(parts[0].Part);
                L acc = Factor(parts[0].Part);
                foreach (var (op, part) in parts.Skip(1))
                {
                    var f = Factor(part);
                    if (op == '*' && f.IsConst) acc = acc.Scale(f.C);
                    else if (op == '*' && acc.IsConst) acc = f.Scale(acc.C);
                    else if (op == '/' && f.IsConst && f.C != 0) acc = acc.Scale(1 / f.C);
                    else return Atom(e);   // 非线性：整段当原子
                }
                return acc;
            }
            L Factor(string e)
            {
                e = e.Trim();
                if (e.Length == 0) throw new FormatException();
                if (e[0] == '-') return Factor(e[1..]).Scale(-1);
                if (e[0] == '+') return Factor(e[1..]);
                if (Wrapped(e)) return Expr(e[1..^1].Trim());
                if (IsNum(e, out double v)) return new L { C = v };
                if (Full(@"_\d+", e).Success && _defs.TryGetValue(e, out var def))
                {
                    if (_memo.TryGetValue(e, out var m)) return m;
                    if (!_busy.Add(e) || _busy.Count > 256) return Atom(e);   // 自引用 / 太深：当原子
                    var r = Expr(def); _busy.Remove(e); _memo[e] = r; return r;
                }
                return Atom(e);
            }
        }

        internal static PyDict RasterFacts(string stage, string src)
        {
            if (stage == "ps")
            {
                var o = Search(@"struct main0_out\s*\{(.*?)\};", src, RegexOptions.Singleline); string og = o.Success ? o.Groups[1].Value : "";
                var cols = new SortedSet<int>(); foreach (Match m in All(@"\[\[color\((\d+)\)\]\]", og)) cols.Add(Int(m.Groups[1].Value));
                return new PyDict { ["discard"] = (long)(Has(src, "discard_fragment") ? 1 : 0), ["sample_mask"] = (long)(Has(og, "sample_mask") ? 1 : 0), ["colors"] = cols.Select(x => (object)(long)x).ToList() };
            }
            if (stage != "vs") return new PyDict();
            var (scr, scrPos) = ScrLocns(src);   // 10-30：屏幕坐标插值量（与下面的整片覆盖证明无关，带分支的顶点着色器也照判；同 dmsh_build.py）
            var body = Search(@"\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$", src, RegexOptions.Singleline);
            var neg = new PyDict { ["pos_attr"] = -1L, ["scr_uv"] = scr, ["scr_pos"] = scrPos };
            if (!body.Success || Search(@"\b(if|for|while|switch|do)\b|clip_distance|ClipDistance", body.Groups[1].Value).Success) return neg;
            string b = body.Groups[1].Value;
            var attr = new Dictionary<string, int>(); foreach (Match m in All(@"(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]", src)) attr[m.Groups[2].Value] = Int(m.Groups[3].Value);
            var vr = new Dictionary<string, (string, string)>(); foreach (Match m in All(@"float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);", b)) vr[m.Groups[1].Value] = (m.Groups[2].Value, m.Groups[3].Value);
            var last = new Dictionary<string, string>(); foreach (Match m in All(@"out\.gl_Position\.([xyzw])\s*=\s*([^;]+);", b)) last[m.Groups[1].Value] = m.Groups[2].Value.Trim();
            (string, string)? SrcOf(string c) => last.TryGetValue(c, out var v) && vr.TryGetValue(v, out var r) ? r : null;
            var m0 = SrcOf("x");
            if (m0 == null || !attr.ContainsKey(m0.Value.Item1) || "xyz".Any(c => SrcOf(c.ToString()) != (m0.Value.Item1, c.ToString()))) return neg;
            string wv;
            if (last.TryGetValue("w", out var lw) && lw == "1.0") wv = "one";
            else if (SrcOf("w") == (m0.Value.Item1, "w")) wv = "attr";
            else return neg;
            var ost = Search(@"struct main0_out\s*\{(.*?)\};", src, RegexOptions.Singleline);
            var locn = new PyDict(); foreach (Match m in All(@"(m_\d+) \[\[user\(locn(\d+)\)\]\]", ost.Success ? ost.Groups[1].Value : "")) locn[m.Groups[1].Value] = Int(m.Groups[2].Value);
            var olast = new Dictionary<(string, string), string>(); foreach (Match m in All(@"out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);", b)) olast[(m.Groups[1].Value, m.Groups[2].Value)] = m.Groups[3].Value.Trim();
            var uv = new PyDict(); var aff = new PyDict();
            string OL(string mem, string c) => olast.TryGetValue((mem, c), out var v) ? v : "";
            (string, string)? Var(string v) => vr.TryGetValue(v, out var r) ? r : null;
            ((string, string) V, List<object> A, List<object> B)? Fm(string v_)
            {
                var m = Search(@"float " + Esc(v_) + @" = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);", b);
                if (!m.Success || !vr.ContainsKey(m.Groups[1].Value)) return null;
                return (vr[m.Groups[1].Value], new List<object> { (long)(Int(m.Groups[2].Value) - 3), (long)Int(m.Groups[3].Value), (long)"xyzw".IndexOf(m.Groups[4].Value) },
                        new List<object> { (long)(Int(m.Groups[5].Value) - 3), (long)Int(m.Groups[6].Value), (long)"xyzw".IndexOf(m.Groups[7].Value) });
            }
            foreach (var kv in locn)
            {
                string mem = kv.Key; int k = (int)kv.Value;
                var sx = Var(OL(mem, "x")); var sy = Var(OL(mem, "y"));
                if (sx != null && sy != null && sx.Value.Item1 == sy.Value.Item1 && attr.ContainsKey(sx.Value.Item1) && sx.Value.Item2 == "x" && sy.Value.Item2 == "y") { uv[k.ToString()] = (long)attr[sx.Value.Item1]; continue; }
                var fx = Fm(OL(mem, "x")); var fy = Fm(OL(mem, "y"));
                if (fx != null && fy != null && fx.Value.V.Item1 == fy.Value.V.Item1 && attr.ContainsKey(fx.Value.V.Item1) && fx.Value.V.Item2 == "x" && fy.Value.V.Item2 == "y"
                    && new[] { (long)fx.Value.A[0], (long)fx.Value.B[0], (long)fy.Value.A[0], (long)fy.Value.B[0] }.Min() >= 0)
                    aff[k.ToString()] = new PyDict { ["attr"] = (long)attr[fx.Value.V.Item1], ["sx"] = fx.Value.A, ["ox"] = fx.Value.B, ["sy"] = fy.Value.A, ["oy"] = fy.Value.B };
            }
            return new PyDict { ["pos_attr"] = (long)attr[m0.Value.Item1], ["pos_w"] = wv, ["uv_from_attr"] = uv, ["uv_affine"] = aff, ["scr_uv"] = scr, ["scr_pos"] = scrPos };
        }

        // 同 dmsh_build.py resolution_scale_contract；现编直接编入正式 Program，
        // 此处只核对正式生产者给的能力与回放槽映射，不另建一套翻译规则。
        internal static PyDict ScaleMetadata(JsonObject res)
        {
            var result = new PyDict();
            var reasons = new List<string>();
            if (res["resolution_scale_reasons"] is JsonArray sourceReasons)
                foreach (var reason in sourceReasons) reasons.Add((string)reason);
            int abi = res["resolution_scale_abi"] is JsonNode version ? (int)version : 0;
            if (abi != 2) reasons.Add("unknown_missing_fact:resolution_scale_abi");
            JsonArray entries = res["tex_scales"] as JsonArray;
            if (entries == null) reasons.Add("unknown_missing_fact:tex_scales");
            var mapped = TexScales(res, entries ?? new JsonArray(), reasons);
            bool producerExact = res["resolution_scale_exact"] is JsonNode exact && (bool)exact;
            if (!producerExact && reasons.Count == 0) reasons.Add("unsupported_scaled_access:producer_not_exact");
            var unique = new SortedSet<string>(reasons, StringComparer.Ordinal);
            bool resolutionExact = producerExact && unique.Count == 0;
            if (entries != null)
            {
                result["tex_scales"] = mapped;
                result["scale_exact"] = res["scale_exact"] is JsonNode oldExact && (bool)oldExact;
            }
            result["resolution_scale_abi"] = (long)abi;
            result["resolution_scale_exact"] = resolutionExact;
            result["resolution_scale_status"] = resolutionExact ? "exact" : "legacy_1x";
            result["resolution_scale_reasons"] = unique.Cast<object>().ToList();
            return result;
        }

        // 同 dmsh_build.py tex_scales：[render_scale 下标, SPIR-V 绑定号, 是图像] → [下标, "tex" | "img", 回放槽]；找不到槽的不出
        static List<object> TexScales(JsonObject res, JsonArray tsc, List<string> reasons = null)
        {
            var o = new List<object>();
            var seen = new Dictionary<int, (string Kind, long Slot)>();
            var conflicted = new HashSet<int>();
            void Unproven(string detail) { reasons?.Add("unsupported_scaled_access:" + detail); }
            foreach (var entry in tsc)
            {
                if (entry is not JsonArray e || e.Count != 3 || e[0] is not JsonValue iv || !iv.TryGetValue<int>(out int idx)
                    || e[1] is not JsonValue bv || !bv.TryGetValue<int>(out int binding) || e[2] is not JsonValue mv || !mv.TryGetValue<bool>(out bool image))
                { Unproven("invalid_scale_entry"); continue; }
                if (idx < 1 || idx >= 73) { Unproven($"scale_index_out_of_range:index={idx}"); continue; }
                JsonNode t = null; int matches = 0;
                if (res[image ? "images" : "textures"] is JsonArray resources)
                    foreach (var x in resources) if ((int)x["binding"] == binding) { t ??= x; matches++; }
                if (matches != 1) Unproven($"binding_mapping:binding={binding}:matches={matches}");
                if (t == null) { Unproven($"slot_mapping:binding={binding}:index={idx}:kind={(image ? "img" : "tex")}"); continue; }
                if (t["array"] is JsonNode length && (int)length != 1) Unproven($"descriptor_array:binding={binding}:length={(int)length}");
                int cs = (int)t["cbuf_slot"];
                var d = DecodeTex((image ? "fp_i" : "fp_t") + (cs < 0 ? "_tcb_" : $"_cb{cs}_") + ((int)t["handle"]).ToString("X"));
                string kind = image ? "img" : "tex";
                if (d is { T: long reg })
                {
                    if (reg < 0 || reg >= (image ? IMG_SLOTS : 256)) { Unproven($"slot_out_of_range:binding={binding}:kind={kind}:slot={reg}"); continue; }
                    if (conflicted.Contains(idx)) continue;
                    if (seen.TryGetValue(idx, out var old))
                    {
                        if (old != (kind, reg))
                        {
                            Unproven($"conflicting_scale_index:index={idx}");
                            conflicted.Add(idx); seen.Remove(idx);
                            o.RemoveAll(value => value is List<object> row && (long)row[0] == idx);
                        }
                        continue;
                    }
                    seen[idx] = (kind, reg);
                    o.Add(new List<object> { (long)idx, kind, reg });
                }
                else Unproven($"slot_mapping:binding={binding}:index={idx}:kind={kind}");
            }
            return o;
        }

        internal static (long? T, long? S, long Ty)? DecodeTex(string name)
        {
            var m = MatchStart(@"[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)", name);
            if (!m.Success) return null;
            long slots = m.Groups[1].Success && m.Groups[1].Value.Length > 0 ? long.Parse(m.Groups[1].Value) : -1, h = Hex(m.Groups[2].Value);
            long tb = slots >= 0 ? (slots & 0xffff) : 2, sb = slots >= 0 && (slots >> 16) != 0 ? (slots >> 16) - 1 : tb;
            long o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
            static long? Slot(long word, long bs, long cb) => cb == 2 && word * 4 >= bs && (word * 4 - bs) % 8 == 0 ? (word * 4 - bs) / 8 : null;
            var words = ty != 0 ? new[] { o0, o1 } : new[] { o0 };
            if (MatchStart(@"[a-z]+_i", name).Success)
            {
                long? r = null;
                foreach (var x in words) if (IMG_BASE <= x * 4 && x * 4 < IMG_BASE + 8 * IMG_SLOTS && Slot(x, IMG_BASE, tb) != null) { r = Slot(x, IMG_BASE, tb); break; }
                return (r, null, ty);
            }
            var tw = ty == 3 ? new[] { o0 } : words;
            long? t = null;
            foreach (var x in tw) if (Slot(x, TEX_BASE, tb) != null && x * 4 < SEPTEX_BASE) { t = Slot(x, TEX_BASE, tb); break; }
            if (t == null) foreach (var x in tw) if (Slot(x, SEPTEX_BASE, tb) != null && x * 4 < SAMP_BASE) { t = SEPTEX_SLOT0 + Slot(x, SEPTEX_BASE, tb); break; }
            long? s = null;
            if (ty != 3) foreach (var x in words) if (x * 4 >= SAMP_BASE && Slot(x, SAMP_BASE, sb) != null) { s = Slot(x, SAMP_BASE, sb); break; }
            return (t, s, ty);
        }
        internal static long? ConstSampler(string name)
        {
            var m = MatchStart(@"[a-z]+_t\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)", name);
            if (!m.Success) return null;
            long h = Hex(m.Groups[2].Value);
            return h >> 28 == 3 ? (h >> 14) & 0x3fff : null;
        }
        // → (统一缓冲槽, 字节偏移, 这个字是采样器号而不是句柄)：独立采样器的字在游戏自己的统一缓冲、纹理不在（句柄类型 1 / 2，巫师3）
        internal static (long Reg, long Off, bool Sid)? UboSampler(string name)
        {
            var m = MatchStart(@"[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)", name);
            if (!m.Success) return null;
            long slots = long.Parse(m.Groups[1].Value), h = Hex(m.Groups[2].Value);
            long sb = (slots >> 16) != 0 ? (slots >> 16) - 1 : slots & 0xffff, ty = h >> 28;
            if (!(ty == 1 || ty == 2) || !(3 <= sb && sb <= 16)) return null;
            return (sb - 3, ((h >> 14) & 0x3fff) * 4, ty == 2);
        }
        // → (纹理 (槽, 偏移), 采样器：Id != null 时是常量采样器号，否则 (槽, 偏移)；Sid = 那个字是采样器号而不是句柄（句柄类型 2）)
        //   句柄类型同 Ryujinx TextureHandle：0 合用、1 独立采样器句柄、2 独立采样器号（缓冲里读到的是号，Ryujinx ReadPackedId 左移 20 拼进句柄）、3 常量采样器。
        //   11-05：原先只认 0 / 1 / 3，类型 2 的纹理落空 → 反射「B binding … has no resolved slot number」，DOOM Eternal 631 支着色器用不了、起局 90 秒没首帧
        //   采样器缓冲是驱动表 cb2、句柄类型 1（独立采样器句柄）：采样器字是驱动按绑定槽写进采样器表的句柄（Ryujinx 照样从 cb2 读），与纹理在 cb2 时同一规则（DecodeTex Slot(·, SAMP_BASE, 2)）
        //   取绑定槽 → Bound。11-05 霍格沃茨之遗 fp_t2d_cb196615_10570000（纹理在 c7、采样器字 cb2+0x570 = 绑定槽 1）原先整支返回 null、反射「no resolved slot number」
        internal static ((long, long) T, (long? Id, long Reg, long Off, bool Sid, long? Bound) S)? DecodeUboTex(string name)
        {
            var m = MatchStart(@"[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)", name);
            if (!m.Success) return null;
            long slots = long.Parse(m.Groups[1].Value), h = Hex(m.Groups[2].Value);
            long tb = slots & 0xffff, sb = (slots >> 16) != 0 ? (slots >> 16) - 1 : tb;
            long o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
            if (!(3 <= tb && tb <= 16) || !(ty <= 3)) return null;
            if (ty == 1 && sb == 2) return o1 * 4 >= SAMP_BASE && (o1 * 4 - SAMP_BASE) % 8 == 0 ? ((tb - 3, o0 * 4), (null, 0, 0, false, (o1 * 4 - SAMP_BASE) / 8)) : null;
            if ((ty == 1 || ty == 2) && !(3 <= sb && sb <= 16)) return null;
            return ((tb - 3, o0 * 4), ty == 3 ? (o1, 0, 0, false, null) : ty != 0 ? (null, sb - 3, o1 * 4, ty == 2, null) : (null, tb - 3, o0 * 4, false, null));
        }
        // → (统一缓冲槽, 字节偏移)：图像句柄在游戏自己的统一缓冲 c3..c16（句柄类型 0，只有纹理半边；Ryujinx 照 ReadPackedId 取低 20 位当纹理号、按图像绑那张纹理）。
        //   11-05 无人深空 124 支计算 / 片元着色器（cp_i_cb3_260_：写 2D 图像，句柄在 c3 字 0x260）原先落到「图像只认 cb2」的 DecodeTex → 没有槽号整支拒载。
        //   <名>_atomic（MSL 2.3 时 SPIRV-Cross 另造的原子模拟指针）不走这里；MSL 3.1 原生贴图原子的接收者就是本图像，Reflect 照 img 标 atomic（11-07 无人深空）
        internal static (long Reg, long Off)? DecodeUboImg(string name)
        {
            if (name.EndsWith("_atomic", StringComparison.Ordinal)) return null;
            var m = MatchStart(@"[a-z]+_i\w*?_cb(\d+)_([0-9A-F]+)", name);
            if (!m.Success) return null;
            long slots = long.Parse(m.Groups[1].Value), h = Hex(m.Groups[2].Value);
            long tb = slots & 0xffff;
            if ((slots >> 16) != 0 || !(3 <= tb && tb <= 16) || (h >> 28) != 0) return null;
            return (tb - 3, (h & 0x3fff) * 4);
        }
        // 着色器按运行时下标取 c2 句柄表的数组（BindlessToArray）：返回贴图半边第 0 项的字节偏移（宿主 TEXA / SAMPA：第 k 项 = c2 字节 off + 8k 那一槽）。
        //   组合句柄（类型 0）：贴图、采样器两个成员都按表取。分离句柄对（类型 ≠ 0，库号 PackSlots(2, 2) = cb196610 或 cb2）：一个字是运行时下标起点（贴图），
        //   另一个是 c2 分离采样器表里的常量字（0x568 + 8r）：只贴图成员给 tex_c2a，采样器成员回落 DecodeTex 的单个 samp（宿主把数组每一项都填它）。
        //   原先分离句柄对整组解不出槽（「fp_t2d_cb196610_… has no resolved slot number」，几款游戏各一支片元着色器整支不载）
        internal static long? C2Array(string name, string ty)
        {
            bool smp = name.EndsWith("Smplr", StringComparison.Ordinal);
            var m = Full(@"[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)", smp ? name[..^5] : name);
            if (!m.Success || !ty.StartsWith("array<", StringComparison.Ordinal)) return null;
            long slots = long.Parse(m.Groups[1].Value), h = Hex(m.Groups[2].Value);
            long tb = slots & 0xffff, sb = (slots >> 16) != 0 ? (slots >> 16) - 1 : tb;
            if (tb != 2 || sb != 2) return null;
            if (h >> 28 == 0) return (h & 0x3fff) * 4;
            if (smp || (h >> 28) is not (1 or 2)) return null;   // 类型 3（常量采样器）的采样器半边是 samp_id，宿主不给它填数组：照旧不接
            long o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff;
            static bool SampWord(long w) => w * 4 >= 0x568 && w * 4 < 0x568 + 8 * 128 && (w * 4 - 0x568) % 8 == 0;
            return SampWord(o1) && !SampWord(o0) ? o0 * 4 : SampWord(o0) && !SampWord(o1) ? o1 * 4 : null;
        }
        static readonly Dictionary<string, long> HELPERS = new() { ["vertex_output"] = 0, ["geometry_vb_output"] = 1, ["geometry_ib_output"] = 2, ["vb_info"] = 3, ["trb_data"] = 4, ["vb_input"] = 1 };

        internal static List<object> Reflect(string stage, string msl, JsonObject res)
        {
            var body = Search(@"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", msl, RegexOptions.Singleline);
            var binds = new List<PyDict>();
            var fetch1d = new HashSet<long>(); if (res["fetch1d"] is JsonArray fa) foreach (var x in fa) fetch1d.Add((long)x);
            bool fetch1dExact = res["fetch1d_exact"] is JsonValue fe && (bool)fe;   // Ampere: rows sized to the element count, reads past them 0 (dmsh_xlate Program.cs)
            // 成员在 id 之后可以还有别的属性（[[id(4), raster_order_group(0)]]：SPIRV-Cross 给片元着色器同步的资源这样写）。只认 [[id(N)]] 会把它们悄悄漏掉
            //   （同 dmsh_build.py c52c5727d，Mario Party Superstars 10-30：参数块按 id ≤ 6 排 56 字节、Metal 按 9 个成员排 72 字节，着色器往空槽写）。每个 [[id( 成员都必须解出
            string members = body.Success ? body.Groups[1].Value : "";
            foreach (Match m in All(@"^\s*(.+?)\s+(\w+)\s*\[\[\s*id\((\d+)\)\s*(?:,[^\]]*)?\]\];", members, RegexOptions.Multiline))
            {
                string ty = m.Groups[1].Value.Trim(), nm = m.Groups[2].Value; long aid = Int(m.Groups[3].Value);
                var e = new PyDict { ["msl_name"] = nm, ["msl_type"] = ty, ["arg_id"] = aid };
                string bare = nm.EndsWith("Smplr", StringComparison.Ordinal) ? nm[..^5] : nm;
                Match mm;
                Match pool = ty == "constant spvPool_" + nm + "*" ? Search(@"struct spvPool_" + Esc(nm) + @"\s*\{\s*uint n;\s*ulong seen;\s*array<(.+), \d+> e;", msl) : Match.Empty;
                bool sampPool = pool.Success && pool.Groups[1].Value == "sampler";
                if (pool.Success) e.Set("kind", sampPool ? "samp_pool" : "tex_pool").Set("elem", pool.Groups[1].Value);   // 追不到槽的句柄读的池（PoolArrays；宿主 nvn_refl.h TEXP / SAMPP）
                else if (HELPERS.TryGetValue(nm, out long hr)) e.Set("kind", "geometry").Set("reg", hr);
                else if (nm == "support_buffer") e.Set("kind", "support");
                else if (nm == UVX) e.Set("kind", "uvx");   // 宽屏 16:9 采样换算（FsUvx）
                else if (nm == LB) e.Set("kind", "lb");   // #800 采样器偏置表（FsLb）
                else if (nm == "gal_ubo_len") e.Set("kind", "ubolen");   // #1204 T1 常量缓冲界长表（gal_spvx.m 直连口径：uint[NVN 统一缓冲槽] = 绑定的 vec4 个数，宿主按当前绑定填）
                else if (nm.StartsWith("gal_tbl_", StringComparison.Ordinal)) e.Set("kind", "tblead").Set("for", nm["gal_tbl_".Length..]);   // #798 纹素缓冲前导元素数（gal_spvx.c spv_tbias：for = 那个纹素缓冲的成员名；宿主指向常量表里值为前导数的一格）
                else if (nm == "spvBufferSizeConstants") e.Set("kind", "sbsize");   // SPIRV-Cross 存储缓冲字节数表（uint[存储缓冲的 [[id]]]，加固取运行时数组长度用；宿主按当前绑定填）
                else if ((mm = Full(@"[a-z]+_c(\d+)", nm)).Success)
                {
                    long n = Int(mm.Groups[1].Value);
                    if (3 <= n && n <= 16) e.Set("kind", "cbuf").Set("reg", n - 3); else e.Set("kind", n == 1 ? "c1" : "drvcb").Set("bank", n);
                }
                else if ((mm = Full(@"[a-z]+_s(\d+)", nm)).Success) e.Set("kind", "sbuf").Set("index", (long)Int(mm.Groups[1].Value));
                else if (DecodeUboTex(bare) is { } u)
                {
                    if (nm.EndsWith("Smplr", StringComparison.Ordinal)) { if (u.S.Id != null) e.Set("kind", "samp_id").Set("reg", u.S.Id.Value); else if (u.S.Bound is long bslot) e.Set("kind", "samp").Set("reg", bslot); else { e.Set("kind", "samp_ubo").Set("reg", u.S.Reg).Set("off", u.S.Off); if (u.S.Sid) e["sid"] = true; } }
                    // 缓冲纹理（texture_buffer，TLD 读、不用采样器）的句柄同样在统一缓冲里：Ryujinx 照 ReadPackedId 取纹理字 o0 的低 20 位当纹理号、绑那张缓冲纹理（TextureBindingsManager
                    //   CommitTextureBindings 的 Target.TextureBuffer 分支），采样器半边不用。11-05 文明6 两支（fp_t_cb1114127_10010000：纹理在 c15、采样器字在 c16）原先出「?」整支拒载。
                    //   一组这样的句柄（array<texture_buffer…>）宿主还没接，照旧「?」
                    else if (Has(ty, "texture_buffer")) { if (ty.StartsWith("texture_buffer", StringComparison.Ordinal)) e.Set("kind", "tbuf_ubo").Set("reg", u.T.Item1).Set("off", u.T.Item2); else e.Set("kind", "?"); }
                    else e.Set("kind", "tex_ubo").Set("reg", u.T.Item1).Set("off", u.T.Item2);
                }
                else if (C2Array(nm, ty) is long c2) e.Set("kind", nm.EndsWith("Smplr", StringComparison.Ordinal) ? "samp_c2a" : "tex_c2a").Set("off", c2);
                else if (nm.EndsWith("Smplr", StringComparison.Ordinal) && UboSampler(nm[..^5]) is { } us)
                {
                    e.Set("kind", "samp_ubo").Set("reg", us.Reg).Set("off", us.Off);
                    if (us.Sid) e["sid"] = true;
                }
                else if (nm.EndsWith("Smplr", StringComparison.Ordinal))
                {
                    var d = DecodeTex(nm[..^5]) ?? throw new DshError("reflect failed", "cannot unpack the handle of " + nm);   // 离线链此处 TypeError
                    if (d.Ty == 3) e.Set("kind", "samp_id").Set("reg", (object)ConstSampler(nm[..^5]));
                    else if (d.Ty != 0) e.Set("kind", "samp").Set("reg", (object)d.S);
                    else e.Set("kind", "samp_tex").Set("reg", (object)d.T);
                }
                else if (DecodeUboImg(nm) is { } ui) e.Set("kind", "img_ubo").Set("reg", ui.Reg).Set("off", ui.Off);
                else
                {
                    var d = DecodeTex(nm);
                    if (d == null) e.Set("kind", "?");
                    else e.Set("kind", MatchStart(@"[a-z]+_i", nm).Success ? "img" : Has(ty, "texture_buffer") ? "tbuf" : "tex").Set("reg", (object)d.Value.T);
                    if (e.TryGetValue("kind", out var kk) && (string)kk == "tex" && e.TryGetValue("reg", out var rg) && rg is long rl && fetch1d.Contains(rl))
                    {
                        e["fetch1d"] = true;
                        if (fetch1dExact) e["fetch1d_exact"] = true;   // the host binds a texture buffer here as exact rows (nvn_mtl.m tbuf_as_2d, nvn_refl f1d = 2)
                        if (Search(@"\b" + Esc(nm) + @"\.get_width\(", msl).Success) e["fetch1d_size"] = true;
                    }
                }
                binds.Add(e);
            }
            int nid = Regex.Matches(members, @"\[\[\s*id\(").Count;
            if (binds.Count != nid) throw new DshError("reflect failed", $"{nid} argument buffer members, {binds.Count} parsed");
            // Native Metal texture atomics are methods on the exact argument-buffer image member. Do not infer them from integer format or access::read_write.
            string code = Regex.Replace(msl, @"/\*.*?\*/|//[^\r\n]*", "", RegexOptions.Singleline);
            var byName = new Dictionary<string, PyDict>(StringComparer.Ordinal);
            foreach (var e in binds)
            {
                string nm = (string)e["msl_name"];
                if (!byName.TryAdd(nm, e)) throw new DshError("reflect failed", "duplicate MSL binding name " + nm);
            }
            var atomicCalls = All(@"\.\s*atomic_[A-Za-z_]\w*\s*\(", code);
            // 带接收者的那条式子有嵌套重复、在每个标识符处回溯，原先每支着色器都在整份 MSL 上跑（11-06 出包速度线剖析：出库进程正则耗时 86% 在本函数）。
            //   它的每处匹配都以「. atomic_xxx (」结尾，即都含 atomicCalls 的一处匹配：atomicCalls 为 0（绝大多数着色器）时它必为空，不跑
            IList<Match> atomics = atomicCalls.Count == 0 ? Array.Empty<Match>()
                : All(@"(?<receiver>[A-Za-z_]\w*(?:(?:\s*\.\s*[A-Za-z_]\w*)|(?:\s*\[[^\]\r\n]*\]))*)\s*\.\s*atomic_[A-Za-z_]\w*\s*\(", code);
            if (atomics.Count != atomicCalls.Count)
                throw new DshError("reflect failed", $"could not parse every native texture atomic receiver ({atomics.Count} of {atomicCalls.Count})");
            foreach (Match call in atomics)
            {
                string receiver = call.Groups["receiver"].Value;
                Match direct = Full(@"spvDescriptorSet0\s*\.\s*([A-Za-z_]\w*)", receiver);
                if (!direct.Success) throw new DshError("reflect failed", "native texture atomic receiver is not a direct image member: " + receiver);
                string nm = direct.Groups[1].Value;
                // 图像两种来路都可以做原子：img = NVN 图像槽 reg（0..IMG_SLOTS-1）；img_ubo = 句柄在游戏统一缓冲 c3..c16（槽 reg 0..13、字节 off，DecodeUboImg）。
                //   11-07 无人深空：聚光灯光源表（light_frag_spot_inner / outer_list，fp_i_cb3_28_r32i.atomic_fetch_add）与深度重投影（postprocess_frag_depth_reprj_frw，
                //   fp_i_cb3_26_r32ui.atomic_fetch_min）三支的句柄在 c3，原先这里只认 img → 反射失败、出库与设备兜底现编都没有这三支，用到它们的绘制整笔丢掉
                if (!byName.TryGetValue(nm, out var e) || !e.TryGetValue("reg", out var ro) || ro is not long reg || reg < 0
                    || !((string)e["kind"] == "img" ? reg < IMG_SLOTS : (string)e["kind"] == "img_ubo" && reg < 14 && e.TryGetValue("off", out var oo) && oo is long))
                    throw new DshError("reflect failed", "native texture atomic receiver is not a resolved image binding: " + nm);
                if (e["msl_type"] is not string ty)
                    throw new DshError("reflect failed", "native texture atomic receiver is not a resolved image binding: " + nm);
                if (ty.StartsWith("array<", StringComparison.Ordinal))
                    throw new DshError("reflect failed", "native texture atomic array binding is unsupported: " + nm);
                if (!ty.StartsWith("texture", StringComparison.Ordinal))
                    throw new DshError("reflect failed", "native texture atomic receiver is not a resolved image binding: " + nm);
                e["atomic"] = true;
            }
            int? bs = SSBO(stage);
            var sbs = res["sbufs"].AsArray().Select(x => x.AsObject()).OrderBy(x => (long)x["binding"]).ToList();
            foreach (var e in binds)
            {
                if ((string)e["kind"] != "sbuf") continue;
                long idx = (long)e["index"];
                var b = idx < sbs.Count ? sbs[(int)idx] : null;
                long? off = b != null && b.Count > 0 && (long)b["cb_slot"] == 0 ? (long)b["cb_offset"] * 4 : null;
                // 不是游戏的缓冲：翻译器哨兵槽 0xff（GlobalToStorage.GmemMapCbSlot）下，偏移 0 = 宿主的原始内存池表、偏移 1 = 写记录位图（GmemLogCbOffset），同上游参考实现（MIT）dmsh_build.py。
                //   原先落进下面 cb_slot ≥ 3 那支成 sbuf_ubo reg 252，宿主读不到槽 252 → 绑哑缓冲：地址从内存读来的访问一律读 0、写丢掉（时之笛 2,206 支，11-06）
                if (b != null && b.Count > 0 && (long)b["cb_slot"] == 0xff)
                {
                    if ((long)b["cb_offset"] == 1) { e.Set("kind", "gmem_log").Set("rw", true); continue; }
                    // rw：其中有写（翻译器给每处记一条 notes，带操作名 Load / Store / 原子）
                    e.Set("kind", "gmem_map").Set("rw", res["notes"] is JsonArray nn && nn.Any(n => Regex.IsMatch((string)n ?? "", @"operation ""(Store|Atomic\w*)"" traced to no storage buffer", RegexOptions.CultureInvariant)));
                    continue;
                }
                if (stage == "cs" && off != null && 0x210 <= off && off < 0x310 && (off - 0x210) % 16 == 0) e.Set("kind", "ubo_ptr").Set("reg", (off - 0x210) / 16);
                // 地址在 NVN2 推常量槽里（c[24 + k]：计算阶段推常量块 1 映射到 c[25]，图形阶段 i 自己的缓冲库 c[25 + i]）：宿主从推常量槽读地址（nvn_refl.h SBUFQ）。
                //   原先落进下一支成 sbuf_ubo reg 21..28，宿主按 uniform 槽读不到 → 哑缓冲
                else if (b != null && b.Count > 0 && (long)b["cb_slot"] >= 24 && (long)b["cb_slot"] < 32) e.Set("kind", "sbuf_push").Set("reg", (long)b["cb_slot"] - 24).Set("off", (long)b["cb_offset"] * 4);
                else if (b != null && b.Count > 0 && (long)b["cb_slot"] >= 3) e.Set("kind", "sbuf_ubo").Set("reg", (long)b["cb_slot"] - 3).Set("off", (long)b["cb_offset"] * 4);
                else e["reg"] = off != null && bs != null && 0 <= off - bs && off - bs < 0x100 && (off - bs) % 16 == 0 ? (off - bs) / 16 : null;
                e["rw"] = b != null && b.Count > 0 && ((long)b["flags"] & 1) != 0;   // BufferUsageFlags.Write = bit 0（同 dmsh_build.py）
            }
            return binds.Cast<object>().ToList();
        }

        internal static PyDict DrvReads(string src)
        {
            var o = new PyDict();
            foreach (int bank in new[] { 0, 2 })
            {
                var offs = new SortedSet<long>(); bool dyn = false;
                foreach (Match m in All(@"fp_c" + bank + @"\)\._m0\[([^\]]+)\]", src))
                {
                    string idx = m.Groups[1].Value.TrimEnd('u');
                    if (idx.Length > 0 && idx.All(char.IsDigit)) offs.Add(long.Parse(idx) * 16); else dyn = true;
                }
                if (offs.Count > 0 || dyn) o[bank.ToString()] = new PyDict { ["vec4_offsets"] = offs.Select(x => (object)x).ToList(), ["dynamic"] = dyn };
            }
            return o;
        }
        static readonly Dictionary<int, (long, long)[]> DRV_FILL = new()
        {
            [0] = new[] { (0x110L, 0x210L), (0x210L, 0x310L), (0x310L, 0x410L), (0x410L, 0x510L), (0x510L, 0x610L) },
            [2] = new[] { ((long)TEX_BASE, (long)SEPTEX_BASE), ((long)IMG_BASE, (long)IMG_BASE + 8 * IMG_SLOTS), ((long)SEPTEX_BASE, (long)SAMP_BASE), ((long)SAMP_BASE, (long)SAMP_BASE + 8 * 128) },
        };
        internal static List<object> Unfillable(PyDict dr, string stage = null)
        {
            var bad = new List<object>();
            // 任务 / 网格自己的存储缓冲表（0x670 / 0x770，回放层按阶段填）只对本阶段算可填，不进各阶段共用的 DRV_FILL
            var own = stage is "ts" or "ms" ? new[] { ((long)SSBO(stage), (long)SSBO(stage) + 0x100) } : Array.Empty<(long, long)>();
            foreach (var kv in dr)
            {
                var d = (PyDict)kv.Value;
                // c2 按运行时下标读：宿主 DRV2 每次把整段的四张句柄表（贴图 / 图像 / 分离贴图 / 分离采样器）全部填好、其余字节 0（nvn_mtl.m encode_args），
                //   与静态读哪几格无关，下标落在表里就读到绑定的句柄。原先一律判填不了、整支拒载（几款游戏的片元着色器按同一下标既取 c2 贴图数组、又把句柄字当数据读）
                if ((bool)d["dynamic"] && kv.Key != "2") bad.Add($"c{kv.Key} dynamic index");
                foreach (long o in ((List<object>)d["vec4_offsets"]).Cast<long>())
                    if (!DRV_FILL[int.Parse(kv.Key)].Concat(kv.Key == "0" ? own : Array.Empty<(long, long)>()).Any(r => o < r.Item2 && r.Item1 < o + 16)) bad.Add($"c{kv.Key}+0x{o:x}");
            }
            return bad;
        }

        static readonly string[] CAST = { "as_type<int>(", "as_type<uint>(" };
        static bool EndsCast(string body, int at) { var s = body.AsSpan(0, at); return s.EndsWith(CAST[0]) || s.EndsWith(CAST[1]); }
        // 一处读出值（直接读的 in.m_N.c 或承接它的浮点临时量，正则 expr）怎么用：1 = 整数用法（as_type<int / uint>(…)），0 = 原样拷贝（整条语句是「左值 = 它;」，位型不变），-1 = 别的
        static int IntUse(string body, int at, string expr)
        {
            if (EndsCast(body, at)) return 1;
            int ls = RFind(body, '\n', at) + 1, le = Find(body, '\n', at);
            return MatchStart(@"\s*[\w.\[\]]+\s*=\s*" + expr + ";$", Sl(body, ls, le)).Success ? 0 : -1;
        }
        // 整数输入推断：某个属性的读出值只经位型转换当整数用、或原样拷贝（至少有一处整数用法），就按 uint 声明（重翻后 Metal 按整数格式取数，宿主不在 CPU 上转顶点）。
        //   读出值两种写法同一判法：DebugMode 开时先落浮点临时量「float _N = in.m_X.c;」再用 _N；关了（11-07 起顶点阶段缺省）表达式传播把临时量内联，
        //   直接写「out.m_Y.x = in.m_X.c;」「_M = in.m_X.c;」。原先直接读只认 as_type 包着的，原样拷贝判成浮点用法：金刚 154 支顶点着色器 178 个整数输入退成 float、每次提交 CPU 转 3.4–6 万个顶点（坑记录 11-07）
        internal static PyDict IntInputs(string src)
        {
            var ins = new PyDict(); foreach (Match m in All(@"(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]", src)) ins[m.Groups[2].Value] = Int(m.Groups[3].Value);
            int i = Find(src, "vertex main0_out main0(");
            var o = new PyDict();
            if (i < 0) return o;
            string body = src[i..];
            foreach (var kv in ins)
            {
                string name = kv.Key; int loc = (int)kv.Value;
                int nint = 0; bool bad = false;
                string rd = @"in\." + Esc(name) + @"\.[xyzw]";
                foreach (Match u in All(@"in\." + Esc(name) + @"\b", body))
                {
                    int ls = RFind(body, '\n', u.Index) + 1, le = Find(body, '\n', u.Index + u.Length);
                    var m = MatchStart(@"\s*float\s+(_\w+)\s*=\s*" + rd + ";$", Sl(body, ls, le));
                    if (!m.Success)
                    {
                        int k = IntUse(body, u.Index, rd);
                        if (k < 0) { bad = true; break; }
                        nint += k; continue;
                    }
                    string v = m.Groups[1].Value;
                    bool first = true;
                    foreach (Match oc in All(@"\b" + Esc(v) + @"\b", body))
                    {
                        if (first) { first = false; continue; }
                        int k = IntUse(body, oc.Index, Esc(v));
                        if (k < 0) { bad = true; break; }
                        nint += k;
                    }
                    if (bad) break;
                }
                if (!bad && nint > 0) o[loc.ToString()] = "uint";
            }
            return o;
        }

        // vs_pad：配对的片元着色器（同一输出目录里的 <片元键>_b.metal）读而本顶点着色器不写的输出补 (0,0,0,1)。
        //   设备上配对片元多半是包里预编的：只有库、没有 MSL（10-24 直连线 Mac 验：顶点 88fa953e 现编、片元 a2813ed5 在包里，建管线报 user(locn2/3) 不匹配）→
        //   同目录没有片元 MSL 时查 ps_inputs.tsv（psInputs，出包从离线库 _b.metal 用同一正则抽），与离线链读 MSL 得到的位置相同
        internal static (string, List<long>) VsPadCore(string outDir, IEnumerable<string> pairs, string src, IReadOnlyDictionary<string, long[]> psInputs = null)
        {
            var want = new HashSet<long>();
            foreach (var ps in pairs)
            {
                string fn = Path.Combine(outDir, ps + "_b.metal"), txt = PackMslRead?.Invoke(ps + "_b.metal") ?? (PackMsl.TryGetValue(ps + "_b.metal", out var pm) ? pm : File.Exists(fn) ? File.ReadAllText(fn) : null);
                if (txt != null) foreach (Match m in All(@"\[\[user\(locn(\d+)\)", txt)) want.Add(Int(m.Groups[1].Value));
                else if (psInputs != null && psInputs.TryGetValue(ps, out var locs)) foreach (long l in locs) want.Add(l);
            }
            return PadLocs(src, want);
        }
        // 设备上首遇的配对（nvn_mtl.m vp_kick）：片元要读、本顶点着色器不写的位置由宿主按掩码给（位 n = user(locn)），补法同上
        internal static (string, List<long>) VsPadMask(string src, uint mask) => PadLocs(src, Enumerable.Range(0, 32).Where(l => ((mask >> l) & 1) != 0).Select(l => (long)l));
        static (string, List<long>) PadLocs(string src, IEnumerable<long> want)
        {
            var have = new HashSet<long>(); foreach (Match m in All(@"\[\[user\(locn(\d+)\)", src)) have.Add(Int(m.Groups[1].Value));
            var miss = want.Where(x => !have.Contains(x)).Distinct().OrderBy(x => x).ToList();
            int i = Find(src, "struct main0_out\n{\n"), j = Find(src, "\n};", i), k = Find(src, "main0_out out = {};");
            if (miss.Count == 0 || i < 0 || j < 0 || k < 0) return (src, new List<long>());
            src = src[..j] + string.Concat(miss.Select(l => $"\n    float4 dm_pad{l} [[user(locn{l})]];")) + src[j..];
            k = Find(src, "main0_out out = {};") + "main0_out out = {};".Length;
            return (src[..k] + string.Concat(miss.Select(l => $"\n    out.dm_pad{l} = float4(0.0, 0.0, 0.0, 1.0);")) + src[k..], miss);
        }
        internal static string VsInit(string src)
        {
            int i = Find(src, "struct main0_out\n{\n"), j = Find(src, "\n};", i), k = Find(src, "main0_out out = {};");
            if (i < 0 || j < 0 || k < 0) return src;
            var outs = All(@"\n    float4 (\w+) \[\[user\(locn\d+\)\]\];", Sl(src, i, j)).Select(m => m.Groups[1].Value);
            k += "main0_out out = {};".Length;
            return src[..k] + string.Concat(outs.Select(n => $"\n    out.{n} = float4(0.0, 0.0, 0.0, 1.0);")) + src[k..];
        }

        internal static string VsLayer(string src)
        {
            int i = Find(src, "struct main0_out\n{\n"), j = Find(src, "\n};", i);
            if (i < 0 || j < 0 || !Has(Sl(src, i, j), "[[render_target_array_index]]") || !Has(Sl(src, i, j), "[[point_size]]")) return src;
            src = src[..i] + Sub(@"\n    float gl_PointSize \[\[point_size\]\];", "", Sl(src, i, j), 1) + src[j..];
            return DropPointSize(src);
        }

        /// <summary>输出结构已去掉点大小的顶点函数里拿掉对它的引用（上游参考实现（MIT）f9b438fba 的 drop_point_size）：赋值直接删；翻译出的函数体按引用写输出的
        /// （虚幻 5.4：_88(out.gl_Position, out.gl_PointSize)），被调函数还要有处可写，就给所在顶点函数开一个局部 float 顶上。原先只删赋值，这种着色器的层入口编不过</summary>
        internal static string DropPointSize(string src)
        {
            src = Sub(@"\n    out\.gl_PointSize = [^;\n]*;", "", src);
            for (int use; (use = Find(src, "out.gl_PointSize")) >= 0;)
            {
                Match a = null;
                foreach (Match x in All(@"\bvertex\s+\w+\s+\w+\s*\(", src)) { if (x.Index >= use) break; a = x; }
                if (a == null) throw new DshError("translation failed", "点大小输出用在顶点函数之外");
                int b = Find(src, '{', a.Index + a.Length), e = b, depth = 0;
                for (; e < src.Length; e++)
                {
                    depth += src[e] == '{' ? 1 : 0; depth -= src[e] == '}' ? 1 : 0;
                    if (depth == 0) break;
                }
                if (b < 0 || e >= src.Length || use > e) throw new DshError("translation failed", "点大小输出所在的顶点函数没有配对的花括号");
                src = src[..(b + 1)] + "\n    float dmsh_point_size = 1.0;" + Replace(src[(b + 1)..e], "out.gl_PointSize", "dmsh_point_size") + src[e..];
            }
            return src;
        }

        internal static (string, PyDict) VsLayerEntry(string full)
        {
            int i = Find(full, "struct main0_out\n{\n"), j = Find(full, "\n};", i);
            if (i < 0 || j < 0 || !Has(full, "gl_InstanceIndex") || Has(Sl(full, i, j), "[[render_target_array_index]]")) return (full, null);
            var locs = new Dictionary<string, int>(); foreach (Match x in All(@"\n    float4 (\w+) \[\[user\(locn(\d+)\)\]\];", Sl(full, i, j))) locs[x.Groups[1].Value] = Int(x.Groups[2].Value);
            Match m = null;
            foreach (Match x in All(@"\n    out\.(\w+)\.([xyzw]) = as_type<float>\(", full)) if (locs.ContainsKey(x.Groups[1].Value)) { m = x; break; }
            if (m == null) return (full, null);
            string name = m.Groups[1].Value, comp = m.Groups[2].Value;
            string st = Replace1(Sub(@"\n    float gl_PointSize \[\[point_size\]\];", "", Sl(full, i, j + 3)), "struct main0_out", "struct main0l_out");
            st = Sl(st, 0, -3) + "\n    uint gl_Layer [[render_target_array_index]];\n};";
            var add = new StringBuilder("\n" + st + "\n");
            foreach (var (ep, nw) in new[] { ("main0", "main0_l"), ("main0_z", "main0_lz") })
            {
                var a = Search(@"\bvertex\s+main0_out\s+" + ep + @"\s*\(", full);
                if (!a.Success) return (full, null);
                int b = Find(full, '{', a.Index + a.Length), depth = 0, e = b;
                for (e = b; e < full.Length; e++)
                {
                    char ch = full[e < 0 ? e + full.Length : e];
                    depth += ch == '{' ? 1 : 0; depth -= ch == '}' ? 1 : 0;
                    if (depth == 0) break;
                }
                if (e >= full.Length) e = full.Length - 1;
                string fn = Replace1(Replace(Sl(full, a.Index, e + 1), "main0_out", "main0l_out"), " " + ep + "(", " " + nw + "(");
                fn = DropPointSize(fn);
                add.Append('\n').Append(Replace(fn, "return out;", $"out.gl_Layer = as_type<uint>(out.{name}.{comp});\n    return out;")).Append('\n');
            }
            return (full + add, new PyDict { ["loc"] = (long)locs[name], ["comp"] = comp });
        }

        /// <summary>整数输出入口 main0_u（同 dmsh_build.fs_uint，缺口 2 A 段）：只把按整数位写的颜色输出改 uint，其余照旧浮点；只写常量的（清屏）全部改 uint。
        /// → (接上 main0_u 的源码, u_mask：位 n = 改成 uint 的 color(n))；不像整数写的返回 (null, 0)</summary>
        internal static (string Src, uint UMask) FsUint(string src, string analysis = null)
        {
            var m = Search(@"^.*\bfragment\s+main0_out\s+main0\s*\(", src, RegexOptions.Multiline); var st = Search(@"struct main0_out\s*\{(.*?)\};", src, RegexOptions.Singleline);
            if (!m.Success || !st.Success) return (null, 0);
            var mem = FsMembers(st);
            var cols = new PyDict(); foreach (var x in mem) if (Has(x.At, "color(") && x.Ty.StartsWith("float", StringComparison.Ordinal)) cols[x.Nm] = x.Ty;
            // 量化新增的赋值不参与猜测整数写入；复制的函数体仍含量化，实际整数槽的 mask 由管线清零。
            string probe = analysis ?? src;
            var fromInt = cols.Keys.Where(nm => Search(@"\bout\." + nm + @"(?:\.\w+)? = as_type<float\d?>\(", probe).Success).ToHashSet();
            var writes = cols.Keys.SelectMany(nm => All(@"\bout\." + nm + @"(?:\.\w+)? = ([^;]+);", probe).Select(x => x.Groups[1].Value)).ToList();
            // 哪些输出真装整数（上游参考实现（MIT）a4d7bb7b int_outputs）：翻译器把寄存器放在整数局部量里，浮点颜色也经 as_type<float>(_n) 写出，只看 as_type 会把它们全算成整数
            var pm = Search(@"^.*\bfragment\s+main0_out\s+main0\s*\(", probe, RegexOptions.Multiline);
            string body = pm.Success ? probe[pm.Index..] : ""; int ret = body.IndexOf("return out;", StringComparison.Ordinal); if (ret >= 0) body = body[..(ret + 1)];
            var ints = IntOutputs(body, cols.Keys);
            if (fromInt.Count == 0 && ints.Count == 0 && !(writes.Count > 0 && writes.All(w => Full(@"-?[0-9.]+f?|float\d\([-0-9., ]*\)", w.Trim()).Success))) return (null, 0);
            if (ints.Count == 0) ints = fromInt;   // 一个都说不清：照旧，按整数位写出的都算
            bool U((string Ty, string Nm, string At) x) => cols.ContainsKey(x.Nm) && (ints.Count == 0 || ints.Contains(x.Nm));
            uint mask = 0; foreach (var x in mem) if (U(x)) mask |= 1u << Int(Search(@"color\((\d+)\)", x.At).Groups[1].Value);
            return (src + "\n" + FsRetyped(src, m, mem, x => U(x) ? "u" : null, "_u"), mask);
        }

        /// <summary>装整数、不是浮点位型的颜色输出（同上游参考实现（MIT）a4d7bb7b dmsh_build.py int_outputs）。逐个输出写顺着局部量往回追：整数运算算出的、或小整数常量
        /// 算整数，算出来的浮点的位型算浮点；0、大常量（浮点常量的位型）、从 uniform / 输入抄来的字说明不了什么。三目选择看两个值；非规格化浮点字面量是 SPIRV-Cross 按输出类型印出的小整数。
        /// 星之卡比 新星同盟的 G-buffer（RGBA16F、RGBA8 sRGB、RGBA8Uint、RG11B10F）颜色 0..2 都经 as_type&lt;float&gt; 写，只有颜色 2 进整数画布（材质位）。
        /// → 只有整数、没有浮点位型的输出；一个都不干净（同一寄存器被两条分支复用）时，含整数的输出</summary>
        internal static HashSet<string> IntOutputs(string body, IEnumerable<string> cols)
        {
            var types = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (Match m in All(@"^\s*(u?int\d?|float\d?|bool)\s+(_\d+)\b", body, RegexOptions.Multiline)) types[m.Groups[2].Value] = m.Groups[1].Value;
            var defs = new Dictionary<string, List<string>>(StringComparer.Ordinal);
            foreach (Match m in All(@"^\s*(?:(?:u?int\d?|float\d?|bool)\s+)?(_\d+)\s*=\s*([^;]+);", body, RegexOptions.Multiline))
            {
                if (!defs.TryGetValue(m.Groups[1].Value, out var l)) defs[m.Groups[1].Value] = l = new List<string>();
                l.Add(m.Groups[2].Value.Trim());
            }
            HashSet<string> Kinds(string e, HashSet<string> seen)
            {
                if (Full(@"_\d+", e).Success)
                {
                    if (types.TryGetValue(e, out var t) && t.StartsWith("float", StringComparison.Ordinal)) return new() { "float" };
                    if (!seen.Add(e)) return new();
                    var r = new HashSet<string>(); if (defs.TryGetValue(e, out var dl)) foreach (var x in dl) r.UnionWith(Kinds(x, seen));
                    return r;
                }
                if (Full(@"-?\d+u?", e).Success)
                {
                    string d = e.TrimEnd('u').TrimStart('-');
                    return d.Length <= 7 && long.TryParse(d, NumberStyles.None, CultureInfo.InvariantCulture, out long v) && v > 0 && v < 0x800000 ? new() { "int" } : new();
                }
                var t3 = Full(@"[^?]+\?\s*([^:]+?)\s*:\s*(.+)", e);   // 两个值之间的选择（Ghost Trick 颜色 0 的比较结果 1.0 / 0.0）看两个值
                if (t3.Success) { var r = Kinds(t3.Groups[1].Value, seen); r.UnionWith(Kinds(t3.Groups[2].Value, seen)); return r; }
                var a = Full(@"as_type<u?int\d?>\((.*)\)", e);
                if (a.Success)
                {
                    // 11-07 卡顿线：位型原样往返 as_type<int>(as_type<float>(Z))（中间可经浮点临时量）= Z 本身，按 Z 判。DebugMode 开时翻译器留着这对往返
                    //   （_329 = as_type<int>(_339)、_339 = as_type<float>(_26[…])），原规则「取浮点的整数位型 = 浮点」判错，关时往返被消成 int(_26[…]) 判对
                    string arg = a.Groups[1].Value.Trim(); var z = new List<string>();
                    if (Full(@"as_type<float\d?>\((.*)\)", arg) is { Success: true } f0) z.Add(f0.Groups[1].Value.Trim());
                    else if (Full(@"_\d+", arg).Success && defs.TryGetValue(arg, out var al) && al.Count > 0 && al.All(x => Full(@"as_type<float\d?>\((.*)\)", x).Success))
                        z.AddRange(al.Select(x => Full(@"as_type<float\d?>\((.*)\)", x).Groups[1].Value.Trim()));
                    if (z.Count > 0) { var r = new HashSet<string>(); foreach (var x in z) r.UnionWith(Kinds(x, seen)); return r; }
                    return Search(@"fp_c\d|\bin\.|^-?[0-9.]", arg).Success ? new() : new() { "float" };
                }
                return e.StartsWith("(*spvDescriptorSet0", StringComparison.Ordinal) || e.StartsWith("spvDescriptorSet0", StringComparison.Ordinal) ? new() : new() { "int" };
            }
            var found = new List<(string Nm, HashSet<string> K)>();
            foreach (var nm in cols)
            {
                var k = new HashSet<string>();
                foreach (Match wm in All(@"\bout\." + nm + @"(?:\.\w+)? = ([^;]+);", body))
                {
                    string w = wm.Groups[1].Value.Trim(); var a = Full(@"as_type<float\d?>\((.*)\)", w);
                    if (a.Success) k.UnionWith(Kinds(a.Groups[1].Value.Trim(), new HashSet<string>(StringComparer.Ordinal)));
                    else if (Full(@"-?[0-9.]+(?:e[-+]?\d+)?f?", w).Success) { if (double.TryParse(w.TrimEnd('f'), NumberStyles.Float, CultureInfo.InvariantCulture, out double fv) && Math.Abs(fv) > 0 && Math.Abs(fv) < 1.17549e-38) k.Add("int"); }
                    else k.Add("float");
                }
                found.Add((nm, k));
            }
            var clean = found.Where(f => f.K.Count == 1 && f.K.Contains("int")).Select(f => f.Nm).ToHashSet(StringComparer.Ordinal);
            return clean.Count > 0 ? clean : found.Where(f => f.K.Contains("int")).Select(f => f.Nm).ToHashSet(StringComparer.Ordinal);
        }

        static List<(string Ty, string Nm, string At)> FsMembers(Match st) =>
            All(@"^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;", st.Groups[1].Value, RegexOptions.Multiline).Select(x => (Ty: x.Groups[1].Value, Nm: x.Groups[2].Value, At: x.Groups[3].Value)).ToList();

        /// <summary>main0 的另一个入口 main0<sfx>：同一函数体，按 pre 给的前缀（"u" / "" / null 不改）把浮点颜色输出改成同位宽的整数向量（as_type 原样取位，不换算）。
        /// 硬件往整数画布写的就是输出寄存器的位，与着色器怎么算无关，所以按画布格式改类型总是对的（FsUint：全部改 uint；FsTyped：逐个位置）。→ 新结构体 + 新入口（接在 src 后）</summary>
        static string FsRetyped(string src, Match m, List<(string Ty, string Nm, string At)> mem, Func<(string Ty, string Nm, string At), string> pre, string sfx, ISet<string> zero = null)
        {
            string Nty((string Ty, string Nm, string At) x) => pre(x) is string p && x.Ty.StartsWith("float", StringComparison.Ordinal) ? p + x.Ty.Replace("float", "int") : x.Ty;   // float4 → uint4 / int4
            string strct = $"struct main0_out{sfx}\n{{\n" + string.Concat(mem.Select(x => $"    {Nty(x)} {x.Nm} {x.At};\n")) + "};\n\n";
            string ret = $"return main0_out{sfx}{{" + string.Join(", ", mem.Select(x => zero != null && zero.Contains(x.Nm) ? $"{Nty(x)}(0)" : Nty(x) != x.Ty ? $"as_type<{Nty(x)}>(out.{x.Nm})" : $"out.{x.Nm}")) + "};";   // zero：main0 没有的成员（双源补的第二源）
            string body = Replace(Sub(@"\bfragment\s+main0_out\s+main0\s*\(", $"fragment main0_out{sfx} main0{sfx}(", src[m.Index..], 1), "return out;", ret);
            return strct + body;
        }

        /// <summary>smask 位 8（宿主 PSO_FOUT_DUAL &gt;&gt; 8，graphics/nvn/gfx/nvn/nvn_pso_skey.h）：画布 0 用第二源混合因子（SRC1_*）。Maxwell 双源混合把第二个颜色输出当第二源
        /// （Ryujinx 开 DualSourceBlendEnable 时把输出 0 / 1 声明成 location 0 index 0 / 1），Metal 只认 [[color(0), index(1)]]；main0 不知道混合状态、输出 1 是 [[color(1)]]，管线建不出。</summary>
        internal const uint FoutDual = 0x100;

        /// <summary>逐个输出位置定类型的入口 main0_t（建管线时按画布格式要，nvn_mtl.m pso_get）：位 n = color(n)，umask 里的改 uint、smask 里的改 int，其余照旧；
        /// smask 带 FoutDual 时 color(0) / color(1) 改 [[color(0), index(0)]] / [[color(0), index(1)]]（双源混合），着色器不写输出 1 的补一个恒 0 的第二源
        /// （同 Ryujinx Translator 开双源时把输出 1 先初始化成 0）；没有 color(0) 的认不出、返回 null。
        /// src 是现编出的整份 .metal：先截掉已接的 main0_u（main0 是它前面最后一个函数）。→ 只含 main0 与 main0_t 的源码；认不出 main0 返回 null</summary>
        internal static string FsTyped(string src, uint umask, uint smask)
        {
            int cut = src.IndexOf("\nstruct main0_out_u\n", StringComparison.Ordinal); if (cut >= 0) src = src[..cut];
            var m = Search(@"^.*\bfragment\s+main0_out\s+main0\s*\(", src, RegexOptions.Multiline); var st = Search(@"struct main0_out\s*\{(.*?)\};", src, RegexOptions.Singleline);
            if (!m.Success || !st.Success) return null;
            int Loc(string at) { var c = Search(@"color\((\d+)\)", at); return c.Success ? int.Parse(c.Groups[1].Value) : -1; }
            var mem = FsMembers(st);
            var pre = new Dictionary<string, string>();   // 按原位置定类型（双源改写后输出 1 也写 color(0)）
            foreach (var x in mem) { int l = Loc(x.At); if (l >= 0) pre[x.Nm] = (umask >> l & 1) != 0 ? "u" : (smask >> l & 1) != 0 ? "" : null; }
            HashSet<string> zero = null;
            if ((smask & FoutDual) != 0)
            {
                int c0 = mem.FindIndex(x => Loc(x.At) == 0), c1 = mem.FindIndex(x => Loc(x.At) == 1);
                if (c0 < 0) return null;
                mem[c0] = (mem[c0].Ty, mem[c0].Nm, "[[color(0), index(0)]]");
                if (c1 >= 0) mem[c1] = (mem[c1].Ty, mem[c1].Nm, "[[color(0), index(1)]]");
                else { mem.Insert(c0 + 1, (mem[c0].Ty, "s2a_src1", "[[color(0), index(1)]]")); zero = new HashSet<string> { "s2a_src1" }; }
            }
            return src + "\n" + FsRetyped(src, m, mem, x => pre.GetValueOrDefault(x.Nm), "_t", zero);
        }

        // ---- dmsh_geometry.capture ----
        const int WORDS = 144, CAPTURE_SLOT = 13;
        internal static string GeoCapture(string src)
        {
            var m = Search(@"\bvertex main0_out main0\((.*?)\)\s*\{", src, RegexOptions.Singleline);
            if (!m.Success) throw new DshError("geometry capture", "geometry capture: missing main0 vertex entry");
            string pars = m.Groups[1].Value, body = src[(m.Index + m.Length)..];
            if (!body.TrimEnd().EndsWith('}')) throw new DshError("geometry capture", "geometry capture: unexpected function ending");
            string pro = "";
            foreach (var (sem, name) in new[] { ("vertex_id", "dm_vid"), ("instance_id", "dm_iid") })
            {
                var f = Search(@"uint (\w+) \[\[" + sem + @"\]\]", pars);
                if (f.Success) pro += $"    uint {name} = {f.Groups[1].Value};\n";
                else pars += (pars.Length > 0 ? ", " : "") + $"uint {name} [[{sem}]]";
            }
            var st = Search(@"struct main0_out\s*\{(.*?)\};", src, RegexOptions.Singleline);
            if (!st.Success) throw new DshError("geometry capture", "geometry capture: missing output struct");
            var stores = new List<string>();
            foreach (var l0 in st.Groups[1].Value.Split('\n'))
            {
                string line = l0.Trim();
                if (line.Length == 0) continue;
                var v = Full(@"(float4|uint4|int4) (\w+) \[\[user\(locn(\d+)\)\]\];", line);
                if (v.Success)
                {
                    int l = Int(v.Groups[3].Value);
                    if (l >= 32) throw new DshError("geometry capture", "geometry capture: attribute outside Maxwell map");
                    for (int c = 0; c < 4; c++) stores.Add($"dm_p[{16 + l * 4 + c}] = as_type<uint>(out.{v.Groups[2].Value}.{"xyzw"[c]});");
                    continue;
                }
                v = Full(@"float4 (\w+) \[\[position(?:, invariant)?\]\];", line);
                if (v.Success) { for (int c = 0; c < 4; c++) stores.Add($"dm_p[{c}] = as_type<uint>(out.{v.Groups[1].Value}.{"xyzw"[c]});"); continue; }
                v = Full(@"(float|uint|int) (\w+) \[\[(point_size|render_target_array_index|viewport_array_index)\]\];", line);
                if (v.Success) { int off = v.Groups[3].Value switch { "point_size" => 4, "render_target_array_index" => 13, _ => 14 }; stores.Add($"dm_p[{off}] = as_type<uint>(out.{v.Groups[2].Value});"); continue; }
                v = Full(@"float (\w+)(?:\[(\d+)\])? \[\[clip_distance\]\](?: \[(\d+)\])?;", line);
                if (v.Success)
                {
                    string ns = v.Groups[2].Value.Length > 0 ? v.Groups[2].Value : v.Groups[3].Value;
                    if (ns.Length > 0 && Int(ns) <= 8) { for (int c = 0; c < Int(ns); c++) stores.Add($"dm_p[{5 + c}] = as_type<uint>(out.{v.Groups[1].Value}[{c}]);"); continue; }
                }
                v = Full(@"float (\w+) \[\[user\(clip(\d)\)\]\];", line);
                if (v.Success) { stores.Add($"dm_p[{5 + Int(v.Groups[2].Value)}] = as_type<uint>(out.{v.Groups[1].Value});"); continue; }
                throw new DshError("geometry capture", "geometry capture: unknown output " + line);
            }
            pars += $", constant dm_geometry_capture& dm_gs [[buffer({CAPTURE_SLOT})]]";
            pro += "    uint4 dm_ci = dm_gs.info; device uint* dm_capture = dm_gs.dst;\n"
                 + "    if (dm_vid - dm_ci.x >= dm_ci.y || dm_iid - dm_ci.z >= dm_ci.w) return;\n"
                 + $"    device uint* dm_p = dm_capture + ((ulong(dm_iid - dm_ci.z) * dm_ci.y + dm_vid - dm_ci.x) * {WORDS});\n";
            string epilogue = "{ for (uint dm_k = 0; dm_k < 144; ++dm_k) dm_p[dm_k] = 0; "
                            + "dm_p[3] = dm_p[4] = 0x3f800000u; "
                            + "for (uint dm_k = 19; dm_k < 144; dm_k += 4) dm_p[dm_k] = 0x3f800000u; "
                            + string.Join(" ", stores) + " return; }";
            if (!Has(body, "return out;")) throw new DshError("geometry capture", "geometry capture: missing return");
            return "\nstruct dm_geometry_capture { device uint* dst; uint4 info; };\nvertex void main0_gs(" + pars + ")\n{\n" + pro + Replace(body, "return out;", epilogue);
        }

        // ---- dmsh_geometry.flatten_sets / topology_pointer ----
        internal static void FlattenSets(string path)
        {
            var d = XIo.ReadAllBytes(path); var w = new uint[d.Length / 4]; Buffer.BlockCopy(d, 0, w, 0, w.Length * 4);
            var sets = new List<(uint Id, uint S, int At)>(); var bindings = new Dictionary<uint, int>();
            int i = 5;
            while (i < w.Length)
            {
                int n = (int)(w[i] >> 16); uint op = w[i] & 65535;
                if (n == 0 || i + n > w.Length) throw new ArgumentException("invalid SPIR-V instruction");
                if (op == 71 && n == 4)
                {
                    if (w[i + 2] == 34) { sets.RemoveAll(x => x.Id == w[i + 1]); sets.Add((w[i + 1], w[i + 3], i + 3)); }
                    if (w[i + 2] == 33) bindings[w[i + 1]] = i + 3;
                }
                i += n;
            }
            foreach (var (id, s, at) in sets)
            {
                if (s == 0) continue;
                if ((s != 1 && s != 2) || !bindings.ContainsKey(id)) throw new ArgumentException("unknown geometry auxiliary descriptor set");
                w[bindings[id]] += s == 1 ? 10000u : 11000u;
                w[at] = 0;
            }
            var o = new byte[w.Length * 4]; Buffer.BlockCopy(w, 0, o, 0, o.Length); XIo.WriteAllBytes(path, o);
        }
        // 同 dmsh_geometry.py roll_vb_copy（11-01 异度3 M3 / macOS 27：发射顶点逐分量写 geometry_vb_output 的展开代码三段叠起来，设备编译器建管线内部错误；卷回循环两代机器都过）
        internal static string RollVbCopy(string src)
        {
            var L = src.Split('\n'); var o = new List<string>(L.Length); int i = 0;
            // 标识符 _<数字> 的出现次数（同正则 \b_\d+\b）：按字符扫、用 span 查表，不为每次出现生成 Match / 字符串（11-07 卡顿线：几何每支 38–45 MB 的大头）
            var count = UnderscoreIdCounts(src); var head = s_rollHead;
            while (i < L.Length)
            {
                var m = head.Match(L[i]);
                if (m.Success)
                {
                    string ind = m.Groups[1].Value, b = m.Groups[2].Value; int n = int.Parse(m.Groups[4].Value);
                    string vb = ind + "(*spvDescriptorSet0.geometry_vb_output)._m0[";
                    var t0 = i + 2 < L.Length ? Regex.Match(L[i + 1], "^" + Regex.Escape(ind) + @"float (_\d+) = (_\d+)\[0\];$") : Match.Empty;
                    if (n > 1 && t0.Success && L[i + 2] == vb + b + "] = " + t0.Groups[1].Value + ";")
                    {
                        string arr = t0.Groups[2].Value; int j = i + 3, k = 1; var dropped = new List<string> { t0.Groups[1].Value };
                        while (k < n && j + 2 < L.Length)
                        {
                            var oo = Regex.Match(L[j], "^" + Regex.Escape(ind) + @"int (_\d+) = " + b + @" \+ " + k + ";$");
                            var t = Regex.Match(L[j + 1], "^" + Regex.Escape(ind) + @"float (_\d+) = " + arr + @"\[" + k + @"\];$");
                            if (!(oo.Success && t.Success && L[j + 2] == vb + oo.Groups[1].Value + "] = " + t.Groups[1].Value + ";")) break;
                            dropped.Add(oo.Groups[1].Value); dropped.Add(t.Groups[1].Value); j += 3; k++;
                        }
                        var run = new Dictionary<string, int>(); foreach (Match r in Regex.Matches(string.Join("\n", L[(i + 1)..j]), @"\b_\d+\b")) run[r.Value] = run.GetValueOrDefault(r.Value) + 1;
                        if (k == n && dropped.All(d => count.GetValueOrDefault(d) == run.GetValueOrDefault(d)))
                        {
                            o.Add(L[i]); o.Add(ind + "for (int s2a_c = 0; s2a_c < " + n + "; s2a_c++) (*spvDescriptorSet0.geometry_vb_output)._m0[" + b + " + s2a_c] = " + arr + "[s2a_c];"); i = j; continue;
                        }
                    }
                }
                o.Add(L[i]); i++;
            }
            return string.Join("\n", o);
        }
        static readonly Regex s_rollHead = new(@"^(\s+)int (_\d+) = (_\d+) \* (\d+);$", RegexOptions.Compiled);
        // 同 .NET 正则 \w：字母、十进制数字、连接标点、非间距标记
        static bool IsWord(char c) => char.IsLetterOrDigit(c) || char.GetUnicodeCategory(c) is UnicodeCategory.ConnectorPunctuation or UnicodeCategory.NonSpacingMark;
        // 同 Regex.Matches(src, @"\b_\d+\b") 计数：'_' 前一字符不是单词字符（或开头），后接一个以上数字（\d 按 Unicode），数字后不是单词字符（或结尾）
        internal static Dictionary<string, int> UnderscoreIdCounts(string src)
        {
            var count = new Dictionary<string, int>(); var look = count.GetAlternateLookup<ReadOnlySpan<char>>();
            for (int i = 0; i < src.Length; i++)
            {
                if (src[i] != '_' || (i > 0 && IsWord(src[i - 1]))) continue;
                int j = i + 1; while (j < src.Length && char.IsDigit(src[j])) j++;
                if (j == i + 1) continue;
                if (j < src.Length && IsWord(src[j])) { i = j - 1; continue; }
                var span = src.AsSpan(i, j - i);
                if (look.TryGetValue(span, out int c)) look[span] = c + 1; else count[span.ToString()] = 1;
                i = j - 1;
            }
            return count;
        }
        internal static string TopologyPointer(string src)
        {
            // trb_data 的元素类型随翻译器声明走（#1469 起 VTG texel 按声明采样类型出：float / uint / int 都可能），
            // 宿主填的始终是 uint 位数据；声明归一成 device const uint*，读点按原元素类型回写同型表达式
            int n = 0; string elem = "";
            src = Regex.Replace(src, @"texture_buffer<(float|u?int)> trb_data", m => { n++; elem = m.Groups[1].Value; return "device const uint* trb_data"; });
            if (n != 1) throw new ArgumentException("missing geometry topology buffer");
            const string prefix = "spvDescriptorSet0.trb_data.read(";
            // 单遍拼（11-07 卡顿线：原先每改一处就把整段源码重拼一次，平方级复制，几何每支 22–27 MB）；读点从左到右逐个改、不回看已改部分，结果与原写法逐字节同
            var sb = new StringBuilder(src.Length); int pos = 0;
            for (int start = src.IndexOf(prefix, StringComparison.Ordinal); start >= 0; start = src.IndexOf(prefix, pos, StringComparison.Ordinal))
            {
                int at = start + prefix.Length, depth = 1, end = at;
                while (depth != 0)
                {
                    if (src[end] == '(') depth++;
                    if (src[end] == ')') depth--;
                    end++;
                }
                if (Sl(src, end, end + 2) != ".x") throw new ArgumentException("unexpected topology read");
                sb.Append(src, pos, start - pos);
                if (elem != "uint") sb.Append("as_type<").Append(elem).Append(">(");
                sb.Append("spvDescriptorSet0.trb_data[").Append(src, at, end - 1 - at).Append(']');
                if (elem != "uint") sb.Append(')');
                pos = end + 2;
            }
            sb.Append(src, pos, src.Length - pos); src = sb.ToString();
            if (Has(src, "trb_data.read")) throw new ArgumentException("unconverted geometry topology read");
            return src;
        }

        // ---- spv_single_lane / _ep_drop ----
        static readonly Dictionary<uint, string> SG_OPS = new() { [333] = "true", [334] = "pred", [335] = "pred", [336] = "true", [337] = "value", [338] = "value", [339] = "ballot", [345] = "value", [346] = "value", [347] = "value", [348] = "value" };
        // OpGroupNonUniform{I,F}Add / Mul / Min / Max / Bitwise / Logical → the identity of an exclusive scan (min / max: the type's extreme)
        static readonly Dictionary<uint, string> SG_ARITH = new() { [349] = "0", [350] = "0", [351] = "1", [352] = "1", [353] = "inf", [354] = "ones", [355] = "inf", [356] = "-inf", [357] = "0", [358] = "-inf", [359] = "ones", [360] = "0", [361] = "0", [362] = "1", [363] = "0", [364] = "0" };
        static readonly Dictionary<uint, string> SG_BUILTINS = new() { [36] = "one", [41] = "zero", [4416] = "one4", [4417] = "one4", [4418] = "zero4", [4419] = "one4", [4420] = "zero4" };
        // [每次调用写的记录数, 索引步长]（同 dmsh_build.py geo_records）：翻译器的 geometry_max_vertices（只在一个都不发射时为 0，才用头里的 nv），每个发射顶点后一个重启
        // 几何着色器的变换反馈（同 dmsh_build.py geo_tf）：每个缓冲 null 或 [步长, [[输出记录里的字偏移, 分量数], …]]；偏移取翻译器的 geometry_out_offsets（[位置, 通用 0 … 31]）；
        //   捕获了记录里没有的输出就报红、整份不捕获（null）
        internal static List<object> GeoTf(string key, JsonArray gtf, JsonObject res)
        {
            var off = res["geometry_out_offsets"] is JsonArray oa ? oa.Select(x => (long)x).ToList() : new List<long>();
            var o = new List<object>();
            foreach (var b in gtf)
            {
                if (b is not JsonArray bb || bb.Count == 0) { o.Add(null); continue; }
                var ent = new List<object>();
                foreach (var e in bb[1].AsArray())
                {
                    long n = (long)e[0], c = (long)e[1], at = 1 + n < off.Count ? off[(int)(1 + n)] : -1;
                    if (at < 0) { Console.Error.WriteLine($"[RED] {key}: transform feedback captures generic output {n}, which the geometry output record does not hold: no capture"); return null; }
                    ent.Add(new List<object> { at, c });
                }
                o.Add(new List<object> { (long)bb[0], ent });
            }
            return o;
        }
        internal static (uint, uint) GeoRecords(uint nv, JsonObject res)
        {
            uint nvt = res["geometry_max_vertices"] is JsonNode g && (uint)g != 0 ? (uint)g : nv;
            return (nvt, nvt * 2);
        }
        internal static bool SpvSingleLane(string path)
        {
            var d = XIo.ReadAllBytes(path); var words = new uint[d.Length / 4]; Buffer.BlockCopy(d, 0, words, 0, words.Length * 4);
            var hdr = words[..5]; var ins = new List<uint[]>();
            for (int i = 5; i < words.Length;) { int n = (int)(words[i] >> 16); ins.Add(words.AsSpan(i, Math.Min(n, words.Length - i)).ToArray()); i += n; if (n == 0) return false; }
            static uint Op(uint[] x) => x[0] & 0xffff;
            static bool IsBuiltinDecor(uint[] x) => Op(x) == 71 && x.Length > 3 && x[2] == 11 && SG_BUILTINS.ContainsKey(x[3]);
            if (!ins.Any(x => SG_OPS.ContainsKey(Op(x)) || SG_ARITH.ContainsKey(Op(x)) || IsBuiltinDecor(x))) return false;
            if (ins.Any(x => 340 <= Op(x) && Op(x) <= 366 && !SG_OPS.ContainsKey(Op(x)) && !SG_ARITH.ContainsKey(Op(x)))) return false;   // other subgroup operations (bit counts, quad): not covered
            var bvar = new Dictionary<uint, string>(); foreach (var x in ins) if (IsBuiltinDecor(x)) bvar[x[1]] = SG_BUILTINS[x[3]];
            // arithmetic group operations alone translate to Metal SIMD functions: left as they are; only beside a subgroup builtin the whole shader goes one-lane (dmsh_build.py spv_single_lane)
            if (bvar.Count == 0 && ins.Any(x => SG_ARITH.ContainsKey(Op(x)))) return false;
            uint? uint_ = ins.FirstOrDefault(x => Op(x) == 21 && x.Length >= 4 && x[2] == 32 && x[3] == 0)?[1];
            uint? boolt = ins.FirstOrDefault(x => Op(x) == 20)?[1];
            uint? uvec4 = uint_ == null ? null : ins.FirstOrDefault(x => Op(x) == 23 && x[2] == uint_ && x[3] == 4)?[1];
            if (uint_ == null || boolt == null || uvec4 == null) return false;
            uint bound = hdr[3]; var c = new Dictionary<string, uint>();
            foreach (var nm in new[] { "zero", "one", "zero4", "one4", "true" }) { c[nm] = bound; bound++; }
            var consts = new List<uint[]> {
                new uint[] { 43 | 4 << 16, uint_.Value, c["zero"], 0 }, new uint[] { 43 | 4 << 16, uint_.Value, c["one"], 1 },
                new uint[] { 44 | 7 << 16, uvec4.Value, c["zero4"], c["zero"], c["zero"], c["zero"], c["zero"] },
                new uint[] { 44 | 7 << 16, uvec4.Value, c["one4"], c["one"], c["zero"], c["zero"], c["zero"] }, new uint[] { 41 | 3 << 16, boolt.Value, c["true"] } };
            // Reduce, InclusiveScan and ClusteredReduce over one lane are the lane's value; an ExclusiveScan is the operation's identity, a constant of the (scalar, 32-bit or bool) result type
            var types = new Dictionary<uint, uint[]>(); foreach (var x in ins) if (Op(x) is 20 or 21 or 22) types[x[1]] = x;
            var ident = new Dictionary<(uint, uint), uint>();
            foreach (var x in ins)
            {
                if (!SG_ARITH.TryGetValue(Op(x), out var v) || x[4] != 2) continue;
                if (!types.TryGetValue(x[1], out var t) || (Op(t) != 20 && t[2] != 32)) return false;   // a vector or a 16 / 64-bit scalar: not covered
                if (ident.ContainsKey((x[1], Op(x)))) continue;
                if (Op(t) == 20) consts.Add(new uint[] { (v != "0" ? 41u : 42u) | 3 << 16, x[1], bound });
                else if (Op(t) == 22) consts.Add(new uint[] { 43 | 4 << 16, x[1], bound, v switch { "0" => 0u, "1" => 0x3f800000u, "inf" => 0x7f800000u, "-inf" => 0xff800000u, _ => throw new ArgumentException("float identity " + v) } });
                else consts.Add(new uint[] { 43 | 4 << 16, x[1], bound, v switch { "0" => 0u, "1" => 1u, "ones" => 0xffffffffu, "inf" => t[3] != 0 ? 0x7fffffffu : 0xffffffffu, _ => t[3] != 0 ? 0x80000000u : 0u } });
                ident[(x[1], Op(x))] = bound; bound++;
            }
            var kconst = new Dictionary<uint, uint>(); foreach (var x in ins) if (Op(x) == 43 && x.Length == 4) kconst[x[2]] = x[3];
            var chain = new Dictionary<uint, uint>();   // a mask's component at a constant index → that component's constant
            foreach (var x in ins)
                if (Op(x) == 65 && x.Length == 5 && bvar.TryGetValue(x[3], out var bk) && (bk == "zero4" || bk == "one4") && kconst.TryGetValue(x[4], out var ki))
                    chain[x[2]] = bk == "one4" && ki == 0 ? c["one"] : c["zero"];
            var o = new List<uint[]>(); bool placed = false;
            foreach (var x0 in ins)
            {
                var x = x0; uint op = Op(x);
                if ((op == 65 && chain.ContainsKey(x[2])) || (op == 5 && chain.ContainsKey(x[1]))) continue;
                if (op == 61 && chain.TryGetValue(x[3], out var cv)) { o.Add(new uint[] { 83 | 4 << 16, x[1], x[2], cv }); continue; }   // a load through it → the constant
                if (op == 17 && x.Length > 1 && new uint[] { 61, 62, 63, 64, 65, 66, 67, 68, 4423, 4431 }.Contains(x[1])) continue;
                if (op == 10)
                {
                    var bytes = new byte[(x.Length - 1) * 4]; Buffer.BlockCopy(x, 4, bytes, 0, bytes.Length);
                    if (bytes.AsSpan().IndexOf("ballot"u8) >= 0 || bytes.AsSpan().IndexOf("subgroup"u8) >= 0) continue;
                }
                if ((op == 71 || op == 5) && x.Length > 1 && bvar.ContainsKey(x[1])) continue;
                if (op == 59 && x.Length > 2 && bvar.ContainsKey(x[2])) continue;
                if (op == 15) x = EpDrop(x, bvar);
                if (op == 54 && !placed) { o.AddRange(consts); placed = true; }
                if (op == 61 && x.Length > 3 && bvar.ContainsKey(x[3])) { o.Add(new uint[] { 83 | 4 << 16, x[1], x[2], c[bvar[x[3]]] }); continue; }
                if (SG_OPS.TryGetValue(op, out var kind))
                {
                    if (kind == "ballot")   // the lane's own bit: OpSelect(pred, 1, 0) in component 0, then the uvec4 (a scalar condition selecting a vector is not valid before SPIR-V 1.4)
                    {
                        o.Add(new uint[] { 169 | 6 << 16, uint_.Value, bound, x[4], c["one"], c["zero"] }); o.Add(new uint[] { 80 | 7 << 16, x[1], x[2], bound, c["zero"], c["zero"], c["zero"] }); bound++; continue;
                    }
                    o.Add(new uint[] { 83 | 4 << 16, x[1], x[2], kind == "true" ? c["true"] : x[4] }); continue;
                }
                if (SG_ARITH.ContainsKey(op)) { o.Add(new uint[] { 83 | 4 << 16, x[1], x[2], x[4] == 2 ? ident[(x[1], op)] : x[5] }); continue; }   // OpCopyObject of the value, or of the identity
                o.Add(x);
            }
            if (!placed) return false;
            if (o.Any(x => Op(x) == 65 && x.Length > 3 && bvar.ContainsKey(x[3]))) return false;
            if (o.Any(x => x.Skip(1).Any(chain.ContainsKey))) return false;   // the component pointer used other than by a load
            hdr[3] = bound;
            var flat = hdr.Concat(o.SelectMany(x => x)).ToArray(); var ob = new byte[flat.Length * 4]; Buffer.BlockCopy(flat, 0, ob, 0, ob.Length);
            XIo.WriteAllBytes(path, ob);
            return true;
        }
        static uint[] EpDrop(uint[] x, Dictionary<uint, string> ids)
        {
            int k = 3;
            while (k < x.Length)
            {
                k++;
                uint wd = x[k - 1];
                if (((wd >> 0) & 0xff) == 0 || ((wd >> 8) & 0xff) == 0 || ((wd >> 16) & 0xff) == 0 || ((wd >> 24) & 0xff) == 0) break;
            }
            var y = x.Take(k).Concat(x.Skip(k).Where(v => !ids.ContainsKey(v))).ToArray();
            y[0] = (uint)y.Length << 16 | (x[0] & 0xffff);
            return y;
        }
    }
}
