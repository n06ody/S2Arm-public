// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Threading;

namespace S2A.Services.Tests
{
    // audsmx:u（Switch 2 系统混音器外部流，nn::audio::sysmix）+ auddmg:u 300 / 401 / 700 / 800：0400486024DEC000 OutputStreamImpl::Open 连不上 audsmx:u 即 Abort。
    //   报文按游戏 sdk 客户端组包函数反推的线上格式手写（0400486024DEC000 sdk 0x2b2cb0 / 0x2b30f0 / 0x2b34f0 / 0x2b3660 / 0xe5040 / 0x2adf70 / 0x2ae100 等）：
    //     audsmx:u 连上即转域；管理对象 0：pid + 复制句柄 1 个（本进程）、u32 1 @0、u64 0 @8 → 域对象 IUserSession
    //     会话 1 CreateStream：A 缓冲流名；pid；u32 设备 @0、u32 标志 @4（输出 0x24）、aruid @8、u64 工作缓冲 @0x10、u64 长 @0x18 → u32 流号 @0 + 域对象
    //     外部流 0 OpenStream：StreamFormat {格式, 声道, 采样率} @0、s32 帧数 @0xc、s32 0 @0x10、u32 0 @0x14
    //     外部流 7 AcquireStreamDataEvent：u32 方式 2 @0、s32 阈值 @4 → 复制句柄；8 Release / 1 Close：无入参
    //     auddmg 300：A 设备名、StreamFormat @0、u32 功能 @0xc；401：B 档案 0x228 + A 设备名、u32 功能 @0；700：A 设备名、u32 流号 @0、u32 功能 @4；800：u32 流号 @0
    //   工作缓冲布局（sdk 0x2b38d0）：B+0x44 = 服务端读指针，B+0x1004 = 客户端写指针，B+0x1040 起环，容量 = 帧数 + 1。
    //   期望不跑实现即可写出：环里写多少帧、宿主（哑设备立刻播完）就取走多少，读指针追上写指针（模容量），数据事件触发；
    //   401 回的延迟让 SDK 的「缓冲帧数 ≥ 延迟 × 采样率 ÷ 1e9」对 20 ms 缓冲成立；控制块 1 / 客户端控制块服务端不碰。
    public static unsafe partial class Program
    {
        private const int SmxFrames = 960;          // 20 ms @ 48 kHz（0400486024DEC000音频框架：4 × 240 帧）
        private const int SmxCapacity = SmxFrames + 1;
        private const int SmxWorkSize = 0x3000;     // SDK 最小长度：(align64(961 × 4) + 0x100 + 0x1FFF) & ~0xFFF

        private static byte* SmxWorkBuffer()
        {
            byte* raw = FakeNative.Alloc(SmxWorkSize + 0x1000);
            byte* b = (byte*)(((ulong)raw + 0xFFF) & ~0xFFFUL);
            new Span<byte>(b, SmxWorkSize).Clear();
            return b;
        }

        // 按客户端 WriteStreamImpl 的协议往环里写：写指针处起抄 n 帧（交错立体声 int16），写指针模容量推进
        private static void SmxClientWrite(byte* b, int startFrame, int n, Func<int, (short L, short R)> sample)
        {
            uint w = *(uint*)(b + 0x1004);
            short* ring = (short*)(b + 0x1040);

            for (int i = 0; i < n; i++)
            {
                (short l, short r) = sample(startFrame + i);
                int at = (int)((w + i) % SmxCapacity);
                ring[2 * at] = l;
                ring[2 * at + 1] = r;
            }

            *(uint*)(b + 0x1004) = (uint)((w + n) % SmxCapacity);
        }

        private static bool SmxWaitRead(byte* b, uint want, int ms)
        {
            Stopwatch sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < ms)
            {
                if (*(uint*)(b + 0x44) == want) return true;
                Thread.Sleep(2);
            }
            return *(uint*)(b + 0x44) == want;
        }

        [SvcCase]
        private static void AudSmxOutputStream()
        {
            Encoding.ASCII.GetBytes("sm:\0").CopyTo(new Span<byte>(_game, 4));
            uint sm = (uint)Svc(0x1F, 0, 0, (ulong)_game).Item2[1];
            byte* buf = FakeNative.Alloc(0x1000);
            bool strict = Red.Strict;
            Red.Strict = false;

            try
            {
                Reply gs = Call(sm, Msg.Cmif(1, Name8("audsmx:u")));
                Check("sm GetService(audsmx:u)", "结果 0、move 1 个会话", $"结果 {gs.Result}、move {gs.Move.Count} 个{(gs.Move.Count == 1 && FakeNative.KindOf(gs.Move[0]) == ObjKind.Session ? "会话" : "")}");

                if (gs.Move.Count != 1)
                {
                    return;
                }

                uint smx = gs.Move[0];
                // 域请求的应答：成功走域格式；直调宿主对报红的命令回的是不带域头的错误应答（HorizonHost.WriteCmifError），按非域格式解
                Reply D(Msg m, int obj)
                {
                    m.DomainObject = obj; m.Write(_tlr); Svc(0x21, FakeNative.CookieOf(smx));
                    if (m.DomainRequestType == 2) return new Reply();
                    try { return Reply.ParseDomain(_tlr); } catch (InvalidOperationException) { return Reply.Parse(_tlr); }
                }

                // 同 SDK：连上先转域（控制命令 0）
                Msg cv = Msg.Cmif(0); cv.Type = 5;
                Reply cvr = Call(smx, cv);
                int root = *(int*)cvr.Data;
                Check("audsmx:u 转域（控制 0）", "结果 0、根对象号 ≠ 0", $"结果 {cvr.Result}、根对象号 {(root != 0 ? "≠ 0" : "0")}");

                // 管理对象 0（同 sysmix::detail::InitializeImpl：pid、本进程句柄、u32 1、u64 0）
                Msg om = Msg.Cmif(0, Bytes(1u, 0u, 0UL)); om.SendPid = true; om.Copy.Add(0xFFFF8001);
                Reply omr = D(om, root);
                int userSession = *(int*)omr.Data;
                // 出对象号的位置按 SDK 客户端读法：数据区 +0（0x2b2e3c 读 SFCO 头后 +0x10）；客户端不读域出头的个数字段
                Check("audsmx IUserSessionManager 0", "结果 0、出对象号 ≠ 0", $"结果 {omr.Result}、出对象号 {(userSession != 0 ? "≠ 0" : "0")}");

                // 会话 1 CreateStream（同 OutputStreamImpl::Open：设备 = 401 档案 +0x220、标志 0x24、工作缓冲页对齐）
                byte* work = SmxWorkBuffer();
                new Span<byte>(work, 0x40).Fill(0x5A);                      // 控制块 1：客户端的，服务端不该碰
                int nameLen = Encoding.ASCII.GetBytes("OutputStream", new Span<byte>(buf, 0x100));
                byte[] csArgs = Bytes(1u, 0x24u, 0x1234UL, (ulong)work, (ulong)SmxWorkSize);
                Msg cs = Msg.Cmif(1, csArgs); cs.SendPid = true; cs.A.Add(((ulong)buf, (ulong)nameLen));
                Reply csr = D(cs, userSession);
                uint streamId = *(uint*)csr.Data;
                int streamObj = *(int*)(csr.Data + 4);
                // 客户端读法（0x2b32a0）：u32 流号在数据区 +0、出对象号在 +4
                Check("audsmx IUserSession 1 CreateStream", "结果 0、流号 ≥ 1、出对象号 ≠ 0 且 ≠ 会话号", $"结果 {csr.Result}、流号 {(streamId >= 1 ? "≥ 1" : streamId.ToString())}、出对象号 {(streamObj != 0 && streamObj != userSession ? "≠ 0 且 ≠ 会话号" : streamObj.ToString())}");

                // 外部流 0 OpenStream（PcmInt16 立体声 48 kHz、960 帧）
                Reply osr = D(Msg.Cmif(0, Bytes(2u, 2, 48000, SmxFrames, 0, 0u)), streamObj);
                Check("audsmx IExternalStreamSession 0 OpenStream（PcmInt16 / 2 声道 / 48 kHz / 960 帧）", "结果 0x0", $"结果 0x{osr.Result:X}");

                // 外部流 4 SetGain（同 OutputStreamImpl::SetGain：f32 增益 @0、s64 渐变 ns @8）：D-KY7F 生化危机 4 起局即调，回错就 Abort
                Reply sgr = D(Msg.Cmif(4, Bytes(BitConverter.SingleToUInt32Bits(0.5f), 0u, 0L)), streamObj);
                Check("audsmx IExternalStreamSession 4 SetGain（0.5、立即）", "结果 0x0", $"结果 0x{sgr.Result:X}");
                Reply sgf = D(Msg.Cmif(4, Bytes(BitConverter.SingleToUInt32Bits(1f), 0u, 100_000_000L)), streamObj);
                Check("audsmx IExternalStreamSession 4 SetGain（1.0、渐变 100 ms）", "结果 0x0", $"结果 0x{sgf.Result:X}");

                // 外部流 7 AcquireStreamDataEvent（方式 2、阈值 = 缓冲 − 1 ms）
                Reply aer = D(Msg.Cmif(7, Bytes(2u, SmxFrames - 48)), streamObj);
                uint dataEvent = aer.Copy.Count == 1 ? aer.Copy[0] : 0;
                Check("audsmx IExternalStreamSession 7 AcquireStreamDataEvent", "结果 0、copy 1 个事件读端",
                    $"结果 {aer.Result}、copy {aer.Copy.Count} 个{(dataEvent != 0 && FakeNative.KindOf(dataEvent) == ObjKind.EventR ? "事件读端" : "")}");

                // auddmg:u：管理对象 0 → 会话；300 / 401
                Reply gd = Call(sm, Msg.Cmif(1, Name8("auddmg:u")));
                Msg dm = Msg.Cmif(0, Bytes(2u, 0u, 0x1234UL, 0UL)); dm.SendPid = true;
                uint dev = Call(gd.Move[0], dm).Move[0];
                int devLen = Encoding.ASCII.GetBytes("DeviceOut", new Span<byte>(buf, 0x100));
                Msg fs = Msg.Cmif(300, Bytes(2u, 2, 48000, 1u)); fs.A.Add(((ulong)buf, (ulong)devLen));
                Check("auddmg 300 IsVirtualDeviceStreamFormatSupported（PcmInt16 / 2 / 48000）", "结果 0x0", $"结果 0x{Call(dev, fs).Result:X}");
                // #b36edb：游戏问格式 5（PcmFloat）立体声，宿主经兼容层转 PCM16 播 → 支持；Int8 / Int32 / 5.1 同理
                foreach ((uint f, int ch) in new[] { (5u, 2), (1u, 2), (4u, 2), (5u, 6) })
                {
                    Msg m = Msg.Cmif(300, Bytes(f, ch, 48000, 1u)); m.A.Add(((ulong)buf, (ulong)devLen));
                    Check($"auddmg 300 IsVirtualDeviceStreamFormatSupported（格式 {f} / {ch} / 48000）", "结果 0x0", $"结果 0x{Call(dev, m).Result:X}");
                }

                byte* profile = buf + 0x400;
                new Span<byte>(profile, 0x228).Fill(0xCC);
                Msg pf = Msg.Cmif(401, Bytes(1u)); pf.B.Add(((ulong)profile, 0x228)); pf.A.Add(((ulong)buf, (ulong)devLen));
                uint pfr = Call(dev, pf).Result;
                ulong latency = *(ulong*)(profile + 0x218);
                long minFrames = (long)((latency * 48000 + 999_999_999) / 1_000_000_000);
                Check("auddmg 401 GetVirtualDeviceProfile：延迟 / +0x220 / SDK 最少帧数 ≤ 960 / +0x108 不写",
                    "结果 0、10000000 ns、1、成立、0xCCCCCCCC",
                    $"结果 {pfr}、{latency} ns、{*(uint*)(profile + 0x220)}、{(minFrames <= SmxFrames ? "成立" : $"不成立（{minFrames}）")}、0x{*(uint*)(profile + 0x108):X}");

                // 客户端先写满 960 帧（同0400486024DEC000音频框架：Start 前写 4 × 240 帧），再 700 挂上
                (short, short) Pattern(int i) => ((short)(i * 7 - 3000), (short)(-i * 5));
                SmxClientWrite(work, 0, SmxFrames, Pattern);
                Msg at = Msg.Cmif(700, Bytes((uint)streamId, 1u)); at.A.Add(((ulong)buf, (ulong)devLen));
                Check("auddmg 700 AttachStream", "结果 0x0", $"结果 0x{Call(dev, at).Result:X}");

                bool r1 = SmxWaitRead(work, SmxFrames, 2000);
                bool fired;
                lock (FakeNative.Events) fired = dataEvent != 0 && FakeNative.Events.TryGetValue(FakeNative.CookieOf(dataEvent), out bool f) && f;
                Check("audsmx 宿主取走 960 帧：读指针（B+0x44）/ 数据事件", "960、已触发", $"{*(uint*)(work + 0x44)}{(r1 ? "" : "（2 秒未到）")}、{(fired ? "已触发" : "未触发")}");

                // 再写 480 帧（跨环尾）：读指针 = (960 + 480) mod 961 = 479
                SmxClientWrite(work, SmxFrames, 480, Pattern);
                bool r2 = SmxWaitRead(work, 479, 2000);
                Check("audsmx 跨环尾再取 480 帧：读指针", "479", $"{*(uint*)(work + 0x44)}{(r2 ? "" : "（2 秒未到）")}");

                Check("audsmx 服务端不碰客户端的块：控制块 1 首字 / 写指针 / 丢帧计数",
                    "0x5A5A5A5A、479、0", $"0x{*(uint*)work:X}、{*(uint*)(work + 0x1004)}、{*(uint*)(work + 0x1000)}");

                // 收尾（同 OutputStreamImpl::Close）：800 摘下 → 8 释放事件 → 1 关流 → 销毁域对象
                Check("auddmg 800 DetachStream", "结果 0x0", $"结果 0x{Call(dev, Msg.Cmif(800, Bytes((uint)streamId))).Result:X}");
                Check("audsmx IExternalStreamSession 8 ReleaseStreamDataEvent", "结果 0x0", $"结果 0x{D(Msg.Cmif(8), streamObj).Result:X}");
                Check("audsmx IExternalStreamSession 1 CloseStream", "结果 0x0", $"结果 0x{D(Msg.Cmif(1), streamObj).Result:X}");
                Msg del = Msg.Cmif(0); del.DomainRequestType = 2; D(del, streamObj);
                Msg at2 = Msg.Cmif(700, Bytes((uint)streamId, 1u)); at2.A.Add(((ulong)buf, (ulong)devLen));
                long redBefore = Red.Total;
                uint gone = Call(dev, at2).Result;
                Check("销毁外部流对象后 700 再挂同一流号", $"结果 0x{Red.ResultUnknownCommand:X}、报红 +1", $"结果 0x{gone:X}、报红 +{Red.Total - redBefore}");

                // #b36edb：PcmFloat 立体声开流成功（环帧长 8 字节：0x1040 + 961 × 8 = 0x2E48 ≤ 0x3000），关流也成功
                byte* work3 = SmxWorkBuffer();
                Msg cs3 = Msg.Cmif(1, Bytes(1u, 0x24u, 0x1234UL, (ulong)work3, (ulong)SmxWorkSize)); cs3.SendPid = true; cs3.A.Add(((ulong)buf, (ulong)nameLen));
                int stream3 = *(int*)(D(cs3, userSession).Data + 4);
                uint os3 = D(Msg.Cmif(0, Bytes(5u, 2, 48000, SmxFrames, 0, 0u)), stream3).Result;
                uint cl3 = D(Msg.Cmif(1), stream3).Result;
                Check("audsmx OpenStream PcmFloat / 2 声道 / 48 kHz / 960 帧；CloseStream", "结果 0x0；0x0", $"结果 0x{os3:X}；0x{cl3:X}");
                Msg del3 = Msg.Cmif(0); del3.DomainRequestType = 2; D(del3, stream3);

                // 负例：SDK 输出路径从不发的取值、开局路径不走的命令，一律报红、回未知命令
                byte* work2 = SmxWorkBuffer();
                Msg cs2 = Msg.Cmif(1, Bytes(1u, 0x24u, 0x1234UL, (ulong)work2, (ulong)SmxWorkSize)); cs2.SendPid = true; cs2.A.Add(((ulong)buf, (ulong)nameLen));
                Reply cs2r = D(cs2, userSession);
                int stream2 = *(int*)(cs2r.Data + 4);
                (string Name, Func<uint> Send)[] reds =
                [
                    ("管理对象 0 u32 = 2", () => { Msg m = Msg.Cmif(0, Bytes(2u, 0u, 0UL)); m.SendPid = true; m.Copy.Add(0xFFFF8001); return D(m, root).Result; }),
                    ("CreateStream 标志 0x64（0x40 语义不明）", () => { Msg m = Msg.Cmif(1, Bytes(1u, 0x64u, 0x1234UL, (ulong)work2, (ulong)SmxWorkSize)); m.SendPid = true; m.A.Add(((ulong)buf, (ulong)nameLen)); return D(m, userSession).Result; }),
                    ("IUserSession 3 EngageStreams（未实现）", () => { Msg m = Msg.Cmif(3); m.A.Add(((ulong)buf, 4)); return D(m, userSession).Result; }),
                    ("OpenStream 标志 1（带时间戳表）", () => D(Msg.Cmif(0, Bytes(2u, 2, 48000, SmxFrames, 0, 1u)), stream2).Result),
                    ("OpenStream PcmInt24（sysmix 按 4 字节存，24 位放哪不知道）", () => D(Msg.Cmif(0, Bytes(3u, 2, 48000, SmxFrames, 0, 0u)), stream2).Result),
                    ("AcquireStreamDataEvent 方式 1（流未开）", () => D(Msg.Cmif(7, Bytes(1u, 100)), stream2).Result),
                    ("外部流 11 PurgeStream（未实现）", () => D(Msg.Cmif(11, Bytes(0)), stream2).Result),
                    ("auddmg 300 PcmInt24", () => { Msg m = Msg.Cmif(300, Bytes(3u, 2, 48000, 1u)); m.A.Add(((ulong)buf, (ulong)devLen)); return Call(dev, m).Result; }),
                    ("auddmg 401 功能 2 配输出设备名（输入设备只有 BuiltInHeadset）", () => { Msg m = Msg.Cmif(401, Bytes(2u)); m.B.Add(((ulong)profile, 0x228)); m.A.Add(((ulong)buf, (ulong)devLen)); return Call(dev, m).Result; }),
                    ("auddmg 700 流未打开", () => { Msg m = Msg.Cmif(700, Bytes(*(uint*)cs2r.Data, 1u)); m.A.Add(((ulong)buf, (ulong)devLen)); return Call(dev, m).Result; }),
                ];

                foreach ((string name, Func<uint> send) in reds)
                {
                    long red0 = Red.Total;
                    uint r = send();
                    Check($"audsmx 负例 {name}", $"结果 0x{Red.ResultUnknownCommand:X}、报红 +1", $"结果 0x{r:X}、报红 +{Red.Total - red0}");
                }

                FakeNative.HandleClose(dev);
                FakeNative.HandleClose(gd.Move[0]);
                FakeNative.HandleClose(smx);
            }
            finally
            {
                Red.Strict = strict;
                FakeNative.HandleClose(sm);
            }
        }

        // 外部流的样本真的交到宿主声音输出：同一个 ExternalOutputStream，宿主换成记下全部样本的设备（经兼容层，立体声直通）。
        //   期望：设备按序收到客户端写进环的每一帧（跨环尾也连续），总数 = 写入数；读指针 = 写入数 mod 961
        [SvcCase]
        private static void AudSmxStreamSamples()
        {
            var cap = new SmxCaptureDriver();
            var mgr = new Ryujinx.Audio.Output.AudioOutputManager();
            var evs = new Ryujinx.Audio.Integration.IWritableEvent[Ryujinx.Audio.Constants.AudioOutSessionCountMax];
            for (int i = 0; i < evs.Length; i++) evs[i] = new NullEvent();
            mgr.Initialize(new Ryujinx.Audio.Backends.CompatLayer.CompatLayerHardwareDeviceDriver(cap), evs);

            byte* work = SmxWorkBuffer();
            var stream = new Ryujinx.Horizon.Sdk.Audio.Detail.ExternalOutputStream(mgr, Dispatcher.Instance.Memory, (ulong)work, SmxWorkSize, "samples");
            stream.Open(new Ryujinx.Horizon.Sdk.Audio.Detail.StreamFormat { SampleFormat = 2, ChannelCount = 2, SampleRate = 48000 }, SmxFrames);

            (short, short) Pattern(int i) => ((short)(i * 7 - 3000), (short)(-i * 5 + 11));
            const int Total = SmxFrames + 900;
            SmxClientWrite(work, 0, SmxFrames, Pattern);
            stream.Attach();

            // 宿主「播」= AudioOutputManager.Update（产品里由声音管理线程在设备取走后调）；写满一轮后客户端按可写帧数续写
            int written = SmxFrames;
            Stopwatch sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < 3000 && (cap.Frames < Total || *(uint*)(work + 0x44) != Total % SmxCapacity))
            {
                mgr.Update();
                stream.Pump();
                uint rd = *(uint*)(work + 0x44), wr = *(uint*)(work + 0x1004);
                int free = SmxCapacity - 1 - (int)((wr - rd + SmxCapacity) % SmxCapacity);
                int n = Math.Min(free, Total - written);
                if (n > 0) { SmxClientWrite(work, written, n, Pattern); written += n; }
                Thread.Sleep(1);
            }

            int same = 0;
            lock (cap.Samples)
            {
                for (int i = 0; i < Math.Min(Total, cap.Frames); i++)
                {
                    (short l, short r) = Pattern(i);
                    same += cap.Samples[2 * i] == l && cap.Samples[2 * i + 1] == r ? 1 : 0;
                }
            }

            Check("audsmx 外部流样本交到宿主输出：收到帧数 / 逐帧相同 / 读指针", $"{Total}、{Total}、{Total % SmxCapacity}",
                $"{cap.Frames}、{same}、{*(uint*)(work + 0x44)}");

            // SetGain 落到宿主输出音量：立即 0.5 → 0.5；100 ms 渐变到 1.0：约 50 ms 时在 (0.5, 1) 之间，150 ms 后 = 1.0
            stream.SetGain(0.5f, 0);
            float g0 = cap.Volume;
            stream.SetGain(1f, 100_000_000L);
            Thread.Sleep(50); stream.Pump();
            float gMid = cap.Volume;
            Thread.Sleep(100); stream.Pump();
            float gEnd = cap.Volume;
            Check("audsmx SetGain 落到宿主输出音量：立即 / 渐变一半 / 渐变结束", "0.5、0.5–1 之间、1",
                $"{g0}、{(gMid > 0.5f && gMid < 1f ? "0.5–1 之间" : gMid.ToString())}、{gEnd}");

            stream.Detach();
            stream.Close();
        }

        // 记下宿主声音输出收到的全部样本（立体声 int16）
        private sealed class SmxCaptureDriver : Ryujinx.Audio.Integration.IHardwareDeviceDriver
        {
            private readonly Ryujinx.Audio.Backends.Dummy.DummyHardwareDeviceDriver _d = new();
            public readonly List<short> Samples = new();
            public int Frames { get { lock (Samples) return Samples.Count / 2; } }

            public static bool IsSupported => true;
            public float Volume { get; set; } = 1f;
            public Ryujinx.Audio.Integration.IHardwareDeviceSession OpenDeviceSession(Ryujinx.Audio.Integration.IHardwareDeviceDriver.Direction direction, Ryujinx.Memory.IVirtualMemoryManager memoryManager, Ryujinx.Audio.Common.SampleFormat sampleFormat, uint sampleRate, uint channelCount)
                => new Session(this, memoryManager, sampleFormat, sampleRate, channelCount);
            public ManualResetEvent GetUpdateRequiredEvent() => _d.GetUpdateRequiredEvent();
            public ManualResetEvent GetPauseEvent() => _d.GetPauseEvent();
            public bool SupportsDirection(Ryujinx.Audio.Integration.IHardwareDeviceDriver.Direction direction) => direction == Ryujinx.Audio.Integration.IHardwareDeviceDriver.Direction.Output;
            public bool SupportsSampleRate(uint sampleRate) => true;
            public bool SupportsSampleFormat(Ryujinx.Audio.Common.SampleFormat sampleFormat) => true;
            public bool SupportsChannelCount(uint channelCount) => channelCount is 1 or 2;
            public void Dispose() => _d.Dispose();

            private sealed class Session : Ryujinx.Audio.Backends.Common.HardwareDeviceSessionOutputBase
            {
                private readonly SmxCaptureDriver _drv;
                private ulong _played;

                public Session(SmxCaptureDriver drv, Ryujinx.Memory.IVirtualMemoryManager mm, Ryujinx.Audio.Common.SampleFormat f, uint rate, uint ch) : base(mm, f, rate, ch) => _drv = drv;

                public override void QueueBuffer(Ryujinx.Audio.Common.AudioBuffer buffer)
                {
                    byte[] bytes = new byte[buffer.DataSize];
                    if (buffer.Data != null) buffer.Data.AsSpan(0, bytes.Length).CopyTo(bytes);
                    else buffer.SampleReader.ReadSamples(buffer, 0, bytes);
                    lock (_drv.Samples)
                    {
                        for (int i = 0; i + 1 < bytes.Length; i += 2) _drv.Samples.Add(BitConverter.ToInt16(bytes, i));
                    }
                    _played += GetSampleCount(buffer);
                }

                public override bool WasBufferFullyConsumed(Ryujinx.Audio.Common.AudioBuffer buffer) => true;
                public override void SetVolume(float volume) => _drv.Volume = volume;
                public override float GetVolume() => _drv.Volume;
                public override ulong GetPlayedSampleCount() => _played;
                public override void Start() { }
                public override void Stop() { }
                public override void PrepareToClose() { }
                public override void Dispose() { }
            }
        }
    }
}
