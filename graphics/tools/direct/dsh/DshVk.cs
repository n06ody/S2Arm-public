// 通用 XGS1 打包（NVN 运行时补编 / 离线出库共用）：dmsh_xlate 的 .spv + .res.json → 引擎 .xgs。
// 逻辑逐条对照 d2r-mobile 的 dm/gfx/tools/dmsh_vk.py（export/rewrite）与 pack_game_shaders.py（pack）；
// 翻译段复用 DshCompiler.TranslateForVulkan（同一 JobFor / 翻译器，iOS 现编用的那条）。
// 不改第三方的引擎，也不写逐款规则：任何 NVN 程序的 code/control 都走这里。
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace S2A.Dsh
{
    public static class DshVk
    {
        public const uint Magic = 0x31534758;
        const uint TexBase = 0x20, ImgBase = 0x120, SampBase = 0x568;
        static readonly Dictionary<string, uint> StageNo = new() { ["vs"] = 0, ["hs"] = 1, ["ds"] = 2, ["gs"] = 3, ["ps"] = 4, ["cs"] = 5 };
        [ThreadStatic] public static Dictionary<string, byte[]> PackSpv;   // 出包（DshCompiler.PackOut）：本线程这次 CompileWithSpv 的 _b.spv（名 → 字节），与 DshCompiler.PackFiles 并列，调用方取走
        static readonly Dictionary<string, uint> Types = new()
        {
            ["sampler"] = 0, ["combined_image_sampler"] = 1, ["sampled_image"] = 2, ["storage_image"] = 3,
            ["uniform_texel_buffer"] = 4, ["storage_texel_buffer"] = 5, ["uniform_buffer"] = 6, ["storage_buffer"] = 7,
        };
        const uint SupportBytes = 2608;   // 本仓引擎的 support layout；d2r-mobile 的 1392 是另一份引擎的布局


        /// <summary>code/control → .xgs。stage = vs/ps/cs/gs。失败抛 DshError。</summary>
        // ---------------- SPIR-V 读取 ----------------
        static IEnumerable<(int At, int Op, uint[] A)> Insts(uint[] w)
        {
            int i = 5;
            while (i < w.Length)
            {
                int n = (int)(w[i] >> 16);
                if (n == 0 || i + n > w.Length) yield break;
                uint[] a = new uint[n - 1];
                Array.Copy(w, i + 1, a, 0, n - 1);
                yield return (i, (int)(w[i] & 0xffff), a);
                i += n;
            }
        }

        static string Str(uint[] a, int skip = 0)
        {
            var bytes = new List<byte>();
            for (int i = skip; i < a.Length; i++) bytes.AddRange(BitConverter.GetBytes(a[i]));
            int z = bytes.IndexOf(0);
            return System.Text.Encoding.UTF8.GetString(bytes.Take(z < 0 ? bytes.Count : z).ToArray());
        }

        sealed class Info
        {
            public List<uint> Caps = new(); public List<string> Exts = new();
            public Dictionary<uint, Dictionary<uint, uint>> Deco = new();
            public Dictionary<uint, (int Op, uint[] A)> Types = new();
            public Dictionary<uint, (uint Storage, uint Type)> Ptr = new(); public Dictionary<uint, (uint Type, uint Class)> Var = new();
            public Dictionary<uint, string> Names = new();
            public Dictionary<uint, uint> Consts = new();
        }

        static Info ReadInfo(uint[] w)
        {
            var x = new Info();
            foreach (var (at, op, a) in Insts(w))
            {
                switch (op)
                {
                    case 17: if (a.Length > 0) x.Caps.Add(a[0]); break;
                    case 10: x.Exts.Add(Str(a, 0)); break;
                    case 5: if (a.Length > 0) x.Names[a[0]] = Str(a, 1); break;
                    case 43: if (a.Length >= 3) x.Consts[a[1]] = a[2]; break;
                    case 71:
                        if (a.Length >= 2)
                        {
                            if (!x.Deco.TryGetValue(a[0], out var d)) x.Deco[a[0]] = d = new Dictionary<uint, uint>();
                            d[a[1]] = a.Length > 2 ? a[2] : 1;
                        }
                        break;
                    case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30:
                        if (a.Length >= 1) x.Types[a[0]] = (op, a.Skip(1).ToArray());
                        break;
                    case 32: if (a.Length >= 3) x.Ptr[a[0]] = (a[1], a[2]); break;
                    case 59: if (a.Length >= 3) x.Var[a[1]] = (a[0], a[2]); break;
                }
            }
            return x;
        }

        // ---------------- 描述符导出（dmsh_vk.export） ----------------
        sealed class Img
        {
            public uint Format, Dim; public bool Arrayed, Ms, Depth; public bool Float; public bool Signed;
        }

        sealed class Desc
        {
            public string Name = "", Type = ""; public uint Binding, Count = 1;
            public string Cat = "", Kind = "";
            public int Slot, Bank = -1, Sampler = -1, HandleType;
            public uint Bytes;
            public Img Image;
        }

        static Img ImageDesc(Info x, uint tid)
        {
            var (op, a) = x.Types[tid];
            while (op is 27 or 28 or 29) { tid = a[0]; (op, a) = x.Types[tid]; }
            // OpTypeImage: a[0]=sampled type, a[1]=dim, a[2]=depth, a[3]=arrayed, a[4]=ms, a[5]=sampled, a[6]=format
            var st = a[0];
            var (sop, sa) = x.Types[st];
            return new Img { Format = a[6], Dim = a[1], Depth = a[2] == 1, Arrayed = a[3] == 1, Ms = a[4] == 1, Float = sop == 22, Signed = sop == 21 && sa.Length > 1 && sa[1] == 1 };
        }

        static (int? Slot, int? Sampler, uint Type) DecodeTex(int slots, uint h, bool image)
        {
            int tb = slots >= 0 ? slots & 0xffff : 2;
            int sb = slots >= 0 && (slots >> 16) != 0 ? (slots >> 16) - 1 : tb;
            uint o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
            int? SlotOf(uint word, uint b, int cb) => cb == 2 && word * 4 >= b && (word * 4 - b) % 8 == 0 ? (int)((word * 4 - b) / 8) : null;
            var words = ty != 0 ? new[] { o0, o1 } : new[] { o0 };
            if (image) return (words.Select(w => ImgBase <= w * 4 && w * 4 < TexBase ? SlotOf(w, ImgBase, tb) : null).FirstOrDefault(s => s != null), null, ty);
            int? t = null;
            foreach (uint w in words)
            {
                int? s = SlotOf(w, TexBase, tb);
                if (s != null && w * 4 < SampBase) { t = s; break; }
            }
            int? sm = null;
            foreach (uint w in words)
            {
                int? s = SlotOf(w, SampBase, sb);
                if (w * 4 >= SampBase && s != null) { sm = s; break; }
            }
            return (t, sm, ty);
        }

        static uint BlockBytes(Info x, uint tid)
        {
            var (op, a) = x.Types[tid];
            if (op == 30 && a.Length > 0)
            {
                var (mop, ma) = x.Types[a[0]];
                if (mop == 28) return 16 * x.Consts.GetValueOrDefault(ma[1]);
            }
            return 0;
        }

        static List<Desc> Export(string key, string stage, byte[] spv, JsonObject res)
        {
            uint[] w = Words(spv);
            Info x = ReadInfo(w);
            uint baseNo = StageNo[stage];
            uint VBind(uint b) => b * 6 + baseNo;
            var byBind = new Dictionary<uint, (uint Vid, uint Type, uint Class)>();
            foreach (var (vid, vt) in x.Var)
                if (x.Deco.TryGetValue(vid, out var d) && d.TryGetValue(33, out uint bind) && vt.Class is 0 or 2 or 12)
                    byBind[VBind(bind)] = (vid, x.Ptr[vt.Type].Type, vt.Class);

            var descs = new List<Desc>();
            if (byBind.TryGetValue(VBind(0), out var sup) && x.Names.GetValueOrDefault(sup.Vid, "") == "support_buffer")
                descs.Add(new Desc { Name = "support_buffer", Binding = VBind(0), Type = "uniform_buffer", Cat = "support_buffer" });

            foreach (JsonNode n in Arr(res, "cbufs"))
            {
                uint b = U(n, "binding");
                var (vid, tid, _) = byBind[VBind(b)];
                int slot = (int)U(n, "slot");
                var d = new Desc { Name = x.Names.GetValueOrDefault(vid, ""), Binding = VBind(b), Type = "uniform_buffer", Cat = "nvn", Bytes = BlockBytes(x, tid) };
                if (slot is >= 3 and <= 16) { d.Kind = "uniform"; d.Slot = slot - 3; }
                else if (slot == 1) d.Kind = "c1";
                else { d.Kind = "driver"; d.Bank = slot; d.Slot = 0; }
                descs.Add(d);
            }

            int? sbase = stage switch { "vs" => 0x110, "cs" => 0x310, "ps" => 0x510, _ => null };
            foreach (JsonNode n in Arr(res, "sbufs").OrderBy(b => U(b, "binding")))
            {
                uint b = U(n, "binding");
                var (vid, tid, _) = byBind[VBind(b)];
                if (U(n, "cb_slot") != 0) throw new DshError("export failed", "存储缓冲不在 c0：" + n!.ToJsonString());
                long off = (long)U(n, "cb_offset") * 4;
                var d = new Desc { Name = x.Names.GetValueOrDefault(vid, ""), Binding = VBind(b), Type = "storage_buffer", Cat = "nvn", Bytes = BlockBytes(x, tid) };
                if (stage == "cs" && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) { d.Kind = "uniform_pointer"; d.Slot = (int)((off - 0x210) / 16); }
                else if (sbase is not null && off - sbase >= 0 && off - sbase < 0x100 && (off - sbase) % 16 == 0) { d.Kind = "storage_pointer"; d.Slot = (int)((off - sbase!.Value) / 16); }
                else throw new DshError("export failed", $"存储缓冲槽解不出：{stage} off={off:#x}");
                descs.Add(d);
            }

            foreach (var (kind, list) in new[] { ("texture", Arr(res, "textures")), ("image", Arr(res, "images")) })
                foreach (JsonNode n in list)
                {
                    uint b = U(n, "binding");
                    if (!byBind.TryGetValue(VBind(b), out var tvar)) {
                        // 反射里的 binding 与 SPIR-V 变量对不上时，跳过该纹理并在导出里留痕，避免整支程序编不出来
                        Console.Error.WriteLine($"【黄】导出：纹理/图像 binding {b} 不在 SPIR-V 变量里，跳过（{key} {stage}）");
                        continue;
                    }
                    var (vid, tid, _) = tvar;
                    Img img = ImageDesc(x, tid);
                    var (slot, samp, ty) = DecodeTex((int)U(n, "cbuf_slot"), U(n, "handle"), kind == "image");
                    if (slot is null) throw new DshError("export failed", $"{kind} 槽解不出：{n!.ToJsonString()}");
                    var d = new Desc { Name = x.Names.GetValueOrDefault(vid, ""), Binding = VBind(b), Cat = "nvn", Image = img, HandleType = (int)ty };
                    if (kind == "texture") { d.Type = img.Dim == 5 ? "uniform_texel_buffer" : "combined_image_sampler"; d.Kind = "texture"; d.Slot = slot.Value; d.Sampler = ty != 0 ? samp ?? -1 : -1; }
                    else { d.Type = img.Dim == 5 ? "storage_texel_buffer" : "storage_image"; d.Kind = "image"; d.Slot = slot.Value; }
                    descs.Add(d);
                }

            descs.Sort((a, b) => a.Binding.CompareTo(b.Binding));
            return descs;
        }

        static JsonArray Arr(JsonObject o, string name) => o[name] as JsonArray ?? new JsonArray();
        static uint U(JsonNode n, string key) => n is JsonObject o && o[key] != null ? (uint)o[key]!.GetValue<long>() : 0;


        /// <summary>设备不支持 Float64（本机 Adreno 730 float64=0）：翻译器会声明/带出 f64 类型。
        /// 若 f64 只出现在类型/常量声明、没有任何指令使用，就把能力与这些声明一起删掉；
        /// 真有指令用 f64 则原样保留并打日志（引擎会明确报能力不支持）。</summary>
        static byte[] TrimFloat64(byte[] spv, string key, string stage)
        {
            uint[] w = Words(spv);
            var ins = new List<(int At, int N, int Op, uint[] A)>();
            foreach (var x in Insts(w)) ins.Add((x.At, x.A.Length + 1, x.Op, x.A));
            var types = new Dictionary<uint, (int Op, uint[] A)>();
            foreach (var i in ins) if (i.Op is >= 19 and <= 30) types[i.A[0]] = (i.Op, i.A.Skip(1).ToArray());
            var f64 = new HashSet<uint>(types.Where(kv => kv.Value.Op == 22 && kv.Value.A.Length >= 1 && kv.Value.A[0] == 64).Select(kv => kv.Key));
            bool hasCap = ins.Any(i => i.Op == 17 && i.A.Length > 0 && i.A[0] == 10);
            if (!hasCap) return spv;
            if (f64.Count == 0)   // 只有能力声明、没有 f64 类型：直接删声明
            {
                var only = new List<uint>(w.Take(5));
                foreach (var i in ins) if (!(i.Op == 17 && i.A.Length > 0 && i.A[0] == 10)) only.AddRange(w.Skip(i.At).Take(i.N));
                Console.Error.WriteLine($"【绿】{key}（{stage}）: 删掉无类型的 Float64 能力声明");
                return WordsBytes(only.ToArray());
            }
            bool grew = true;
            while (grew) { grew = false; foreach (var kv in types) if (!f64.Contains(kv.Key) && kv.Value.A.Any(a => f64.Contains(a))) { f64.Add(kv.Key); grew = true; } }
            var dead = new HashSet<uint>(f64);
            foreach (var i in ins) if ((i.Op == 43 || i.Op == 44) && i.A.Length >= 2 && f64.Contains(i.A[0])) dead.Add(i.A[1]);
            bool used = false;
            foreach (var i in ins)
            {
                if (i.Op is >= 19 and <= 32 or 43 or 44 or 5 or 7 or 8 or 10 or 11 or 14 or 15 or 16 or 17 or 71 or 72 or 331) continue;
                if (i.A.Length > 0 && dead.Contains(i.A[0])) { used = true; break; }
                if (i.A.Any(a => dead.Contains(a))) { used = true; break; }
            }
            if (used)
            {
                Console.Error.WriteLine($"【黄】{key}（{stage}）: SPIR-V 真正使用 Float64，设备不支持；原样保留");
                return spv;
            }
            var outW = new List<uint>(w.Take(5));
            foreach (var i in ins)
            {
                bool drop = i.Op == 17 && i.A.Length > 0 && i.A[0] == 10;
                if (!drop && i.A.Length > 0 && dead.Contains(i.A[0]) && (i.Op is (>= 19 and <= 32) or (>= 43 and <= 44) or 71 or 72 or 5)) drop = true;
                if (!drop) outW.AddRange(w.Skip(i.At).Take(i.N));
            }
            Console.Error.WriteLine($"【绿】{key}（{stage}）: 删掉未使用的 Float64（能力 + {dead.Count} 个声明）");
            return WordsBytes(outW.ToArray());
        }

        // ---------------- 改写 ----------------
        static byte[] WordsBytes(uint[] w)
        {
            byte[] b = new byte[w.Length * 4];
            Buffer.BlockCopy(w, 0, b, 0, b.Length);
            return b;
        }

        static uint[] Words(byte[] b)
        {
            uint[] w = new uint[b.Length / 4];
            Buffer.BlockCopy(b, 0, w, 0, b.Length & ~3);
            return w;
        }

        [System.Runtime.InteropServices.DllImport("libc", EntryPoint = "__system_property_get")]
        static extern int SystemPropertyGet(string name, byte[] value);

        static bool StripInterlockEnabled()
        {
            try
            {
                var buf = new byte[92];
                int n = SystemPropertyGet("debug.s2a.strip_interlock", buf);
                if (n > 0 && buf[0] == (byte)'1') return true;
            }
            catch { }
            return Environment.GetEnvironmentVariable("S2A_DIRECT_STRIP_INTERLOCK") == "1";
        }

        static byte[] StripInterlock(byte[] spv)
        {
            uint[] w = Words(spv); var o = new List<uint>(w.Length);
            for (int h = 0; h < 5 && h < w.Length; h++) o.Add(w[h]);   // SPIR-V 头 5 words 不是指令
            for (int i = 5; i < w.Length;)
            {
                int wc = (int)(w[i] >> 16), op = (int)(w[i] & 0xffff);
                if (wc <= 0 || i + wc > w.Length) break;
                if (op == 17 && wc >= 2 && w[i + 1] == 5378u) { i += wc; continue; }   // OpCapability FragmentShaderPixelInterlockEXT
                if (op == 5364 || op == 5365) { i += wc; continue; }                   // OpBegin/EndInvocationInterlockEXT
                if (op == 10) {                                                        // OpExtension "SPV_EXT_fragment_shader_interlock"
                    var sb = new System.Text.StringBuilder();
                    for (int k = 1; k < wc; k++) {
                        uint v = w[i + k];
                        for (int b = 0; b < 4; b++) { byte c = (byte)(v >> (8 * b)); if (c == 0) break; sb.Append((char)c); }
                    }
                    if (sb.ToString() == "SPV_EXT_fragment_shader_interlock") { i += wc; continue; }
                }
                for (int j = 0; j < wc; j++) o.Add(w[i + j]);
                i += wc;
            }
            return WordsBytes(o.ToArray());
        }

        static byte[] Rewrite(byte[] spv, string stage)
        {
            uint[] w = Words(spv);
            uint baseNo = StageNo[stage];
            foreach (var (at, op, a) in Insts(w))
                if (op == 71 && a.Length >= 3 && a[1] == 33) w[at + 3] = a[2] * 6 + baseNo;
            return WordsBytes(w);
        }

        static byte[] AddFormatCaps(byte[] spv)
        {
            uint[] w = Words(spv);
            var fmt = new Dictionary<uint, uint>();
            var ty = new Dictionary<uint, uint>();
            var have = new HashSet<uint>();
            foreach (var (at, op, a) in Insts(w))
            {
                if (op == 17 && a.Length > 0) have.Add(a[0]);
                else if (op == 25 && a.Length >= 8) fmt[a[0]] = a[7];
                else if (op == 61 && a.Length >= 3) ty[a[1]] = a[0];
            }
            var need = new List<uint>();
            foreach (var (at, op, a) in Insts(w))
            {
                if (op is 98 or 99)
                {
                    uint img = op == 98 ? a[2] : a[0];
                    uint cap = op == 98 ? 55u : 56u;
                    if (fmt.GetValueOrDefault(ty.GetValueOrDefault(img)) == 0 && !have.Contains(cap) && !need.Contains(cap)) need.Add(cap);
                }
            }
            if (need.Count == 0) return spv;
            var list = w.ToList();
            for (int i = 0; i < need.Count; i++) { list.Insert(5 + 2 * i, (2u << 16) | 17); list.Insert(6 + 2 * i, need[i]); }
            return WordsBytes(list.ToArray());
        }

        static uint[] Inputs(byte[] spv, string stage)
        {
            if (stage != "vs") return Array.Empty<uint>();
            Info x = ReadInfo(Words(spv));
            return x.Var.Where(kv => kv.Value.Class == 1 && x.Deco.TryGetValue(kv.Key, out var d) && d.ContainsKey(30) && !d.ContainsKey(11))
                .Select(kv => x.Deco[kv.Key][30]).Distinct().OrderBy(v => v).ToArray();
        }

        // ---------------- 出包的 Vulkan 产物（#783：契约 §4.1 / §11.4 方案 A，直连线 11-01 口径 (a)） ----------------
        /// <summary>同一个键出 Metal 与 Vulkan 两份，共用 &lt;key&gt;_b.json：先照常 DshCompiler.Compile（写 _b.metal / _b.json），
        /// 再用同一张答案表、同一 JobFor / 翻译器重出单线程改写之前的 .spv（VulkanSpv），按 json 改写绑定号（BindToArgIds），写 &lt;key&gt;_b.spv。
        /// 出包（DshCompiler.PackOut）时 Compile 的产物留在 DshCompiler.PackFiles、不落盘：json 从那里取，_b.spv 进 PackSpv，同样不落盘。
        /// 几何着色器走 CompileGeometry（按输入图元出几份，见那里）；曲面外壳 / 曲面域着色器不出 .spv（报红，不静默）。返回产物名。</summary>
        /// <param name="imgLoadRaw">出库目标档案的 shaderStorageImageReadWithoutFormat：false = 最低档案（只读非缓冲图像声明具体格式，
        /// 任何设备都合法）；按真档案 true 才声明 Unknown。不配设备的库取件在绑定核被拒、退回现编（#1850）。</param>
        public static List<string> CompileWithSpv(string key, string stage, byte[] code, byte[] ctl, string outDir, ISpvCross xc, string tablesDir, bool imgLoadRaw, string workRoot = null)
        {
            if (stage is "gs" or "hs" or "ds") throw new DshError("export failed", $"{stage} 着色器不走 CompileWithSpv（vs / ps / cs 在这里，几何走 CompileGeometry，曲面不出）");
            PackSpv = DshCompiler.PackOut ? new Dictionary<string, byte[]>() : null;
            var written = DshCompiler.Compile(key, stage, code, ctl, outDir, xc, tablesDir, workRoot);
            string jsonName = key + "_b.json", spvName = key + "_b.spv";
            string jsonText = !DshCompiler.PackOut ? File.ReadAllText(Path.Combine(outDir, jsonName))
                : DshCompiler.PackFiles != null && DshCompiler.PackFiles.TryGetValue(jsonName, out var t) ? t
                : throw new DshError("export failed", "出包模式下 PackFiles 里没有 " + jsonName);
            var json = JsonNode.Parse(jsonText).AsObject();
            var bindings = json["bindings"] as JsonArray ?? throw new DshError("export failed", jsonName + " 没有 bindings");
            byte[] spv = VulkanSpv(key, stage, code, ctl, xc, tablesDir, imgLoadRaw, workRoot);
            if (stage is "vs" or "ps" or "cs")
            {
                StorageWindowContract(json, bindings, spv);
                if (DshCompiler.PackOut) DshCompiler.PackFiles[jsonName] = json.ToJsonString();
                else File.WriteAllText(Path.Combine(outDir, jsonName), json.ToJsonString());
            }
            spv = BindToArgIds(spv, bindings, stage);
            if (DshCompiler.PackOut) PackSpv[spvName] = spv;
            else
            {
                string p = Path.Combine(outDir, spvName), tmp = p + ".tmp-" + Environment.ProcessId + "-" + Environment.CurrentManagedThreadId;
                File.WriteAllBytes(tmp, spv); File.Move(tmp, p, true);
            }
            written.Add(spvName);
            return written;
        }

        /// <summary>json 对应的那一遍翻译的 .spv，取在单线程改写（SpvSingleLane，Metal 专用）之前。顶点着色器照 Finish 的判据：
        /// 单线程改写后的 MSL 里有答案表没给类型的整数输入，就补进 attr 再译一遍（json 记的是这一遍）；Dsh.cs 在 dmsh_chain 的 FILES 里，这里只调不改。
        /// 工作文件同 Compile 留在本线程的内存表（XIo），不落临时目录（出包时整条路都不碰盘）。</summary>
        static byte[] VulkanSpv(string key, string stage, byte[] code, byte[] ctl, ISpvCross xc, string tablesDir, bool imgLoadRaw, string workRoot)
        {
            string w = Path.Combine(workRoot ?? Path.GetTempPath(), "dshvkb_" + key + "_" + Guid.NewGuid().ToString("N")[..8]);
            var c = new DshCompiler.Ctx { Key = key, Out = w, W = w, Xc = xc, T = DshTables.Get(tablesDir), ImageLoadRaw = imgLoadRaw };
            var mem0 = XIo.Mem; XIo.Mem = new Dictionary<string, byte[]>();
            try
            {
                var (j, _) = c.JobFor(stage, code, ctl);
                if (stage is "vs" or "ps" or "cs") j["storage_window_mask"] = true;
                c.TranslateFirst(j);
                string spv = Path.Combine(w, key + ".spv");
                if (stage == "vs")
                {
                    string probe = Path.Combine(w, key + "_lane.spv");
                    XIo.WriteAllBytes(probe, XIo.ReadAllBytes(spv));
                    DshCompiler.SpvSingleLane(probe);
                    string src = xc.Msl(XIo.ReadAllBytes(probe), out var e) ?? throw new DshError("MSL failed", e ?? "");
                    var attr = (JsonObject)j["attr"]; bool again = false;
                    foreach (var kv in DshCompiler.IntInputs(src))
                        if (!attr.ContainsKey(kv.Key)) { attr[kv.Key] = (string)kv.Value; again = true; }
                    if (again)
                    {
                        string jp = Path.Combine(w, "job1_" + key + ".json");
                        XIo.WriteAllText(jp, "[" + j.ToJsonString() + "]");
                        var (rc, err) = DshCompiler.RunXlate(jp, w);
                        if (rc != 0 || !XIo.Exists(spv)) throw new DshError("translation failed", "integer-input retranslation: " + err.Trim());
                    }
                }
                return XIo.ReadAllBytes(spv);
            }
            finally { XIo.Mem = mem0; }
        }

        /// <summary>把 .spv 里每个资源变量（UniformConstant / Uniform / StorageBuffer，带 Binding 装饰）的 DescriptorSet / Binding
        /// 改成 (0, json 里同名成员的 arg_id × 6 + 阶段号)。阶段号同 StageNo（vs 0 / hs 1 / ds 2 / gs 3 / ps 4 / cs 5），与 XGS 路径的 VBind 同一口径
        /// （#1254 直连缺陷线定）：Metal 的 arg_id 每阶段各自从 0 编，安卓 backend_vulkan.c 按 (set, binding) 合并各阶段，直接当绑定号必然跨阶段撞号；
        /// 按阶段分 set 不行（图形最多 5 个阶段，后端只到 4 个 set）。绑定号可逆：binding / 6 = arg_id，binding % 6 = 阶段。SPIRV-Cross 出 MSL 参数缓冲时成员名就是变量的 OpName，组合纹理的采样器另起「名字 + Smplr」，
        /// 按变量名对上的正是 texture 那个；MSL 独有的成员（uvx / lb / 几何辅助等）在 .spv 里没有变量，自然跳过。
        /// 翻译器总会声明 support_buffer 等资源，入口没用到时 SPIRV-Cross 不放进参数缓冲、json 里就没有它：这类「声明了没用」的变量
        /// 依次放到本阶段所有 arg_id 之后的空号（(最大 arg_id + 1 + k) × 6 + 阶段号；Vulkan 仍要求资源变量带装饰，管线布局不必含它）；「没用到」按保守判定：函数体里任何指令引用了它都算用到。
        /// .spv 里用到的资源变量找不到同名成员、或两个变量落到同一个绑定号 = 红。</summary>
        /// <summary>指令 op 的第 k 个操作字（不含指令头）是不是字面量（不是 id）：判「函数体里用到了哪个资源变量」时跳过这些位置。
        /// 原先每个操作字都当 id 比，字面量恰好等于某个资源变量的 id 就误判「用到」→ 报红整支丢：某款 11194 键里 17 键是这样，
        /// 例如 OpExtInst 的指令号 32（GLSL.std.450 InverseSqrt）撞上 %32「fp_t_tcb_8」，反汇编里这个变量其实一次都没被引用。
        /// 只列翻译器会出、带字面量的指令；没列的指令照旧每个操作字都当 id（保守：宁可误报红，不把用到的挪去空号）。</summary>
        static bool LiteralOperand(int op, int k) => op switch
        {
            12 => k == 3,                         // OpExtInst：结果类型、结果、扩展集、指令号（字面量）、操作数…
            79 => k >= 4,                         // OpVectorShuffle：分量下标
            81 => k >= 3,                         // OpCompositeExtract：下标
            82 => k >= 4,                         // OpCompositeInsert：下标
            61 => k >= 3,                         // OpLoad：内存访问掩码与对齐
            62 => k >= 2,                         // OpStore：内存访问掩码与对齐
            63 => k >= 2,                         // OpCopyMemory
            64 => k >= 3,                         // OpCopyMemorySized
            246 => k >= 2,                        // OpLoopMerge：循环控制
            247 => k >= 1,                        // OpSelectionMerge：选择控制
            250 => k >= 3,                        // OpBranchConditional：分支权重
            251 => k >= 2 && (k - 2) % 2 == 0,    // OpSwitch：选择子、缺省、（字面量值、目标）对
            87 or 88 or 91 or 92 or 95 or 98 => k == 4,   // 图像采样 / 取 / 读：图像操作数掩码（之后的是 id）
            89 or 90 or 93 or 94 or 96 or 97 => k == 5,   // 带比较值 / 收集分量的图像指令：图像操作数掩码
            99 => k == 3,                         // OpImageWrite：图像操作数掩码
            8 => true,                            // OpLine：文件、行、列
            _ => false,
        };

        /// <summary>入口用到的变量，保守判定：函数体（OpFunction … OpFunctionEnd）里任何指令的任何操作字等于该变量 id 都算用到，
        /// 指针经 OpCopyObject / OpPhi / OpSelect / 原子等传递也算；宁可把真没用的当成用到而报红，不把用到的静默挪到空号</summary>
        static HashSet<uint> UsedVars(uint[] w, Info x)
        {
            var used = new HashSet<uint>();
            bool inFunction = false;
            foreach (var (_, op, a) in Insts(w))
            {
                if (op == 54) inFunction = true;
                else if (op == 56) inFunction = false;
                else if (inFunction) for (int k = 0; k < a.Length; k++) if (!LiteralOperand(op, k) && x.Var.ContainsKey(a[k])) used.Add(a[k]);
            }
            return used;
        }

        // The Vulkan-only retranslation can introduce a c0 read for a base
        // address that the aligned Metal translation did not need to read.
        static void StorageWindowContract(JsonObject json, JsonArray bindings, byte[] spv)
        {
            const string maskName = "s2a_storage_window_mask";
            uint[] words = Words(spv);
            Info info = ReadInfo(words);
            json["ssbo_window_abi"] = 1;
            var masks = info.Names.Where(pair => pair.Value == maskName).Select(pair => pair.Key).ToArray();
            if (masks.Length == 0) return;
            if (masks.Length != 1 || !Insts(words).Any(inst => inst.Op == 50 && inst.A.Length == 3 &&
                    inst.A[1] == masks[0] && inst.A[2] == uint.MaxValue))
                throw new DshError("export failed", "invalid Vulkan storage window mask");
            json["ssbo_window_mask"] = masks[0];
            var used = UsedVars(words, info);
            foreach (var (id, v) in info.Var)
            {
                if (v.Class != 2 || !used.Contains(id) || !info.Deco.TryGetValue(id, out var deco) ||
                    !deco.ContainsKey(33) || !deco.TryGetValue(34, out uint set) || set != 0 ||
                    !info.Names.TryGetValue(id, out string name) || !name.EndsWith("_c0", StringComparison.Ordinal))
                    continue;
                if (bindings.Any(binding => (string)binding["msl_name"] == name)) continue;
                long argId = bindings.Select(binding => (long)binding["arg_id"]).DefaultIfEmpty(-1).Max() + 1;
                bindings.Add(new JsonObject { ["msl_name"] = name, ["arg_id"] = argId, ["kind"] = "drvcb", ["bank"] = 0 });
            }
        }

        public static byte[] BindToArgIds(byte[] spv, JsonArray bindings, string stage)
        {
            if (!StageNo.TryGetValue(stage, out uint stageNo)) throw new DshError("export failed", "未知阶段 " + stage);
            var argId = new Dictionary<string, uint>(StringComparer.Ordinal);
            foreach (var b in bindings)
                if (b is JsonObject o && o["msl_name"] is JsonNode n && o["arg_id"] is JsonNode a) argId[(string)n] = (uint)(long)a;
            uint[] w = Words(spv);
            Info x = ReadInfo(w);
            var used = UsedVars(w, x);
            var target = new Dictionary<uint, uint>(); var idle = new List<uint>();
            foreach (var (vid, vt) in x.Var.OrderBy(v => v.Key))
            {
                if (vt.Class is not (0 or 2 or 12) || !x.Deco.TryGetValue(vid, out var d) || !d.ContainsKey(33)) continue;
                string name = x.Names.GetValueOrDefault(vid, "");
                if (argId.TryGetValue(name, out uint id)) target[vid] = id;
                else if (!used.Contains(vid)) idle.Add(vid);
                else throw new DshError("export failed", $".spv 资源变量 %{vid}「{name}」入口用到了，但 _b.json 的 bindings 里没有同名成员");
            }
            uint spare = argId.Count == 0 ? 0 : argId.Values.Max() + 1;
            foreach (uint vid in idle) target[vid] = spare++;
            if (target.Values.Distinct().Count() != target.Count) throw new DshError("export failed", "两个 .spv 资源变量对到同一个绑定号");
            foreach (var (at, op, a) in Insts(w))
                if (op == 71 && a.Length >= 3 && target.TryGetValue(a[0], out uint id))
                {
                    if (a[1] == 33) w[at + 3] = id * 6 + stageNo;   // Binding：arg_id × 6 + 阶段号，各阶段落在各自的模 6 槽里
                    else if (a[1] == 34) w[at + 3] = 0;    // DescriptorSet：参数缓冲只有一个
                }
            return WordsBytes(w);
        }

        // ---------------- 原生几何阶段（安卓离线 Vulkan 库） ----------------
        /// <summary>离线出的几何着色器输入图元（翻译器 InputTopology 编号）：点 0 / 线 1 / 三角 3。邻接（2 / 4）设备现编也还没接（DirectShaderCompiler 具名拒），这里同样不出；
        /// 消费端（android/native/direct/libpack_vk.c）按绘制图元挑同号那份，没有就整支 miss</summary>
        public static readonly int[] GeometryTopologies = { 0, 1, 3 };

        /// <summary>离线不出这支几何着色器的原因（同设备现编 DirectShaderCompiler 的具名拒：几何头阶段号不是 4、直通型几何头），null = 出。
        /// code / ctl 是着色器包里的原样两段（NVN1 代码区 0x30 前缀之后才是 SPH，NVN2 从头就是）</summary>
        public static string GeometrySkip(byte[] code, byte[] ctl)
        {
            int at = Nvn2.Is(ctl) ? 0 : 0x30;
            if (code == null || code.Length < at + 4) return "geometry code is shorter than its header";
            uint h = BitConverter.ToUInt32(code, at);
            if (((h >> 10) & 15) != 4) return "header stage is not geometry";
            if ((h & (1u << 24)) != 0) return "passthrough geometry header (the device compiles it)";
            return null;
        }

        /// <summary>几何着色器 → 原生 Vulkan 几何阶段的 .spv 与反射，每种输入图元一份：&lt;key&gt;_vg&lt;t&gt;.json + &lt;key&gt;_vg&lt;t&gt;.spv（t 见 GeometryTopologies），返回 名 → 字节（不落盘，调用方写）。
        /// 同设备现编（DirectShaderCompiler：VS → GS → FS 原生几何）一样不转计算；Metal 那条（GeometryFinish）是几何转计算，产物与这里无关。
        /// 几何阶段没有 Metal 对应物、SPIRV-Cross 也出不了几何的 MSL，反射就照 .spv 本身写：按入口用到的资源变量（名字 = 翻译器的 OpName，与 Metal 参数缓冲的成员名同源）
        /// 拼出参数缓冲成员表交 DshCompiler.Reflect（与 vs / ps / cs 的 _b.json 同一套按名字分类：cbuf / c1 / sbuf / tex / samp…），参数号按成员先后编；
        /// 再按它改写绑定号（BindToArgIds：参数号 × 6 + 3）。输出接口不配片元（翻译器不调 SetNextStage）：只写着色器自己写的输出，与离线顶点阶段同一口径。
        /// 运行时描述符数组（池）、认不出的资源类型、直通型几何头报 DshError，不出这支（设备现编）。</summary>
        /// <param name="imgLoadRaw">同 CompileWithSpv：出库目标档案的 shaderStorageImageReadWithoutFormat。</param>
        public static Dictionary<string, byte[]> CompileGeometry(string key, byte[] code, byte[] ctl, ISpvCross xc, string tablesDir, bool imgLoadRaw, string workRoot = null)
        {
            if (GeometrySkip(code, ctl) is string why) throw new DshError("export failed", why);
            var outFiles = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            foreach (int t in GeometryTopologies)
            {
                var (spv, res) = GeometryXlate(key, code, ctl, t, xc, tablesDir, imgLoadRaw, workRoot);
                var bindings = (JsonArray)JsonNode.Parse(Py.Dumps(DshCompiler.Reflect("gs", GeometryArgs(spv), res)));
                var json = new JsonObject
                {
                    ["key"] = key + "_vg" + t, ["stage"] = "gs", ["dm_ctl"] = key, ["gs_input_topology"] = t,
                    ["resolution_scale_abi"] = res["resolution_scale_abi"]?.DeepClone(), ["notes"] = res["notes"]?.DeepClone(), ["bindings"] = bindings,
                };
                StorageWindowContract(json, bindings, spv);
                outFiles[key + "_vg" + t + ".json"] = System.Text.Encoding.UTF8.GetBytes(json.ToJsonString());
                outFiles[key + "_vg" + t + ".spv"] = BindToArgIds(spv, bindings, "gs");
            }
            return outFiles;
        }

        /// <summary>一种输入图元的翻译：同 JobFor / 翻译器，任务带 gs_input_topology（不带 geometry_topology：那个是几何转计算）。工作文件留在本线程的内存表（XIo）</summary>
        static (byte[] Spv, JsonObject Res) GeometryXlate(string key, byte[] code, byte[] ctl, int topology, ISpvCross xc, string tablesDir, bool imgLoadRaw, string workRoot)
        {
            string w = Path.Combine(workRoot ?? Path.GetTempPath(), "dshvkg_" + key + "_" + Guid.NewGuid().ToString("N")[..8]);
            var c = new DshCompiler.Ctx { Key = key, Out = w, W = w, Xc = xc, T = DshTables.Get(tablesDir), ImageLoadRaw = imgLoadRaw };
            var mem0 = XIo.Mem; XIo.Mem = new Dictionary<string, byte[]>();
            try
            {
                var (j, _) = c.JobFor("gs", code, ctl);
                j["gs_input_topology"] = topology;
                j["storage_window_mask"] = true;
                c.TranslateFirst(j);
                return (XIo.ReadAllBytes(Path.Combine(w, key + ".spv")), JsonNode.Parse(XIo.ReadAllText(Path.Combine(w, key + ".res.json"))).AsObject());
            }
            finally { XIo.Mem = mem0; }
        }

        /// <summary>.spv 入口用到的资源变量 → Metal 参数缓冲成员表的写法（struct spvDescriptorSetBuffer0 { 类型 名 [[id(N)]]; … }，Reflect 只读这一段）。
        /// 组合纹理在 Metal 里是「名」+「名Smplr」两个成员（SPIRV-Cross 的拆法），这里同样拆；N 按 (原绑定号, 变量 id) 先后连续编</summary>
        static string GeometryArgs(byte[] spv)
        {
            uint[] w = Words(spv);
            Info x = ReadInfo(w);
            var used = UsedVars(w, x);
            var sb = new System.Text.StringBuilder("struct spvDescriptorSetBuffer0\n{\n");
            int id = 0;
            foreach (var (vid, vt) in x.Var.Where(v => v.Value.Class is 0 or 2 or 12 && used.Contains(v.Key) && x.Deco.TryGetValue(v.Key, out var d) && d.ContainsKey(33))
                                             .OrderBy(v => x.Deco[v.Key][33]).ThenBy(v => v.Key))
            {
                string name = x.Names.GetValueOrDefault(vid, "");
                if (name.Length == 0) throw new DshError("export failed", $"geometry .spv resource %{vid} has no name");
                string ty = MslType(x, x.Ptr[vt.Type].Type, vt.Class, name, out bool combined, out uint count);
                sb.Append($"    {ty} {name} [[id({id++})]];\n");
                if (combined) sb.Append($"    {(count > 0 ? $"array<sampler, {count}>" : "sampler")} {name}Smplr [[id({id++})]];\n");
            }
            return sb.Append("};\n").ToString();
        }

        /// <summary>资源变量指向的类型 → Reflect 认的 MSL 成员类型（它只看 texture_buffer / texture 前缀 / array&lt; 与名字）。combined = 组合纹理（另有采样器成员），count = 数组长（0 = 不是数组）</summary>
        static string MslType(Info x, uint tid, uint cls, string name, out bool combined, out uint count)
        {
            combined = false; count = 0;
            if (!x.Types.TryGetValue(tid, out var t)) throw new DshError("export failed", $"geometry .spv resource {name}: type %{tid} not declared");
            if (t.Op == 29) throw new DshError("export failed", $"geometry .spv resource {name} is a runtime descriptor array (pool): not emitted");
            if (t.Op == 28)
            {
                if (!x.Consts.TryGetValue(t.A[1], out count) || count == 0) throw new DshError("export failed", $"geometry .spv resource {name}: array length is not a constant");
                return $"array<{MslType(x, t.A[0], cls, name, out combined, out _)}, {count}>";
            }
            string Img(uint img, bool storage)
            {
                var (op, a) = x.Types[img];
                if (op != 25) throw new DshError("export failed", $"geometry .spv resource {name}: sampled image of a non-image type");
                return (a[1] == 5 ? "texture_buffer<float" : "texture2d<float") + (storage ? ", access::read_write>" : ">");   // Dim 5 = Buffer
            }
            switch (t.Op)
            {
                case 27: combined = true; return Img(t.A[0], false);                 // OpTypeSampledImage
                case 25: return Img(tid, t.A[5] == 2);                                // OpTypeImage：Sampled 2 = 存储图像
                case 26: return "sampler";
                case 30: return (cls == 12 ? "device " : "constant ") + "S2AGsBlock_" + name + "&";   // 常量 / 存储块（Reflect 按名字分类）
                default: throw new DshError("export failed", $"geometry .spv resource {name}: type op {t.Op} not mappable");
            }
        }

        // ---------------- 打包（pack_game_shaders.pack） ----------------
    }
}
