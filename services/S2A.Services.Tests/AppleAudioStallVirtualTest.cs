// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using Ryujinx.Audio.Backends.Apple;   // 平台例外：苹果音频后端单测（真 AudioQueue），只在 macOS 跑
using Ryujinx.Audio.Backends.Common;
using Ryujinx.Audio.Common;
using Ryujinx.Audio.Integration;
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace S2A.Services.Tests
{
    // D-GJCX / D-4T8M 宝可梦剑中止：设备不回调而不是系统中断（路由切换、失焦未冻结……）时，游戏约 0.8–1 秒收不到交还就关掉音频输出重开，
    //   重开路径不销毁上一个 SystemEvent，OpenDefaultAudioOut 中止。修法：交还永不停——任何原因设备超过 VirtualAfterMs（200 ms）不回调就改走
    //   无设备播放、按采样率交还，不在系统中断里 VirtualProbeMs（500 ms）后试回设备（AppleHardwareDeviceSession）。
    //   真声卡（本机 AudioQueue），钩子 S2A_AUDIO_TEST_STALL_AT=1：开播后第 1 秒外部暂停队列、不发任何通知（同系统悄悄停播）。期望按语义手算：
    // ① 钩子到点生效（一行「测试钩子…外部暂停输出队列」）
    // ② 看门狗（每 250 ms 一拍）1 秒内因「设备 … ms 没有回调」改走无设备播放（不是系统中断）
    // ③ 游戏侧交还不停：钩子后 2 秒里每 10 ms 取一次播完帧数，相邻两次增长之间最长间隔 ≤ 600 ms
    //   （上限 = 进无设备播放 200 ms + 看门狗一拍 250 ms + 余量；游戏约 800 ms 起重开）。
    //   负控：同一单测在改前（不在系统中断里只靠 1000 ms 兜底重启）的代码上跑，② 红、③ 最长间隔约 1 秒（11-23 实测 973 ms，已过游戏约 800 ms 的重开阈值）
    // ④ 2 秒内试回设备：一行「试回设备 … 结束无设备播放 … 重新开播 rc=0」
    // ⑤ 回到设备后照常播：10 秒内播完再涨 ≥ 24000 帧
    static class AppleAudioStallVirtualTest
    {
        private sealed class ToneReader : HardwareDeviceSessionOutputBase
        {
            public ToneReader() : base(null, SampleFormat.PcmInt16, 48000, 2) { }

            public override void ReadSamples(AudioBuffer buffer, ulong offset, Span<byte> destination) => destination.Fill(0x10);
            public override void Dispose() { }
            public override void PrepareToClose() { }
            public override void QueueBuffer(AudioBuffer buffer) => throw new NotSupportedException();
            public override void SetVolume(float volume) { }
            public override float GetVolume() => 1f;
            public override void Start() { }
            public override void Stop() { }
            public override ulong GetPlayedSampleCount() => 0;
            public override bool WasBufferFullyConsumed(AudioBuffer buffer) => false;
        }

        [SoloTest("--apple-audio-stall-virtual")]
        private static int Check()
        {
            if (!AppleHardwareDeviceDriver.IsSupported)   // 平台例外：苹果音频后端单测，只在 macOS 跑
            {
                Console.WriteLine("✗ 苹果音频停交还无设备播放：本机没有可用的苹果音频后端");
                return 1;
            }

            Environment.SetEnvironmentVariable("S2A_AUDIO_TEST_STALL_AT", "1");   // 会话类首次用到前设好（钩子是静态只读字段）
            var log = new StringWriter(); TextWriter err = Console.Error;
            Console.SetError(TextWriter.Synchronized(log));

            int bad = 0;
            void Row(string name, string expect, string actual, bool ok)
            {
                bad += ok ? 0 : 1;
                Console.WriteLine($"{(ok ? "✓" : "✗")} 苹果音频停交还无设备播放 {name}：{expect} → {actual}");
            }
            bool WaitLog(string s, int ms) { var sw = Stopwatch.StartNew(); while (sw.ElapsedMilliseconds < ms) { if (log.ToString().Contains(s)) return true; Thread.Sleep(5); } return log.ToString().Contains(s); }
            string Line(string s) { foreach (string l in log.ToString().Split('\n')) if (l.Contains(s)) return l.Trim(); return "—"; }

            var reader = new ToneReader();
            var driver = new AppleHardwareDeviceDriver();   // 平台例外：苹果音频后端单测（真 AudioQueue），只在 macOS 跑
            IHardwareDeviceSession session = driver.OpenDeviceSession(IHardwareDeviceDriver.Direction.Output, null, SampleFormat.PcmInt16, 48000, 2);
            session.Start();

            const ulong Bytes = 4800 * 4;   // 100 ms
            for (int i = 0; i < 80; i++)
            {
                var b = new AudioBuffer { DataPointer = 0x100000UL + (ulong)i * Bytes, DataSize = Bytes, BufferTag = (ulong)i, SampleReader = reader };
                session.RegisterBuffer(b);
                session.QueueBuffer(b);
            }

            bool hook = WaitLog("外部暂停输出队列", 3000);
            var sinceHook = Stopwatch.StartNew();
            Row("① 钩子到点生效", "3 秒内一行「外部暂停输出队列」", hook ? Line("外部暂停输出队列") : "没生效", hook);

            // ③ 先采样（② ④ 看日志，事后判）：钩子后 2 秒，每 10 ms 一次
            ulong last = session.GetPlayedSampleCount(); long lastInc = 0, maxGap = 0;
            while (sinceHook.ElapsedMilliseconds < 2000)
            {
                Thread.Sleep(10);
                ulong p = session.GetPlayedSampleCount(); long t = sinceHook.ElapsedMilliseconds;
                if (p != last) { maxGap = Math.Max(maxGap, t - lastInc); lastInc = t; last = p; }
            }
            maxGap = Math.Max(maxGap, sinceHook.ElapsedMilliseconds - lastInc);

            string vl = Line("改走无设备播放");
            Row("② 设备不回调就改走无设备播放", "钩子后 1 秒内、因「没有回调」（不是系统中断）", vl, vl.Contains("没有回调") && !vl.Contains("中断开始"));
            Row("③ 游戏侧交还不停", "钩子后 2 秒内相邻交还最长间隔 ≤ 600 ms", $"{maxGap} ms", maxGap <= 600);

            bool back = WaitLog("试回设备", 2000);
            string bl = Line("试回设备");
            Row("④ 试回设备", "一行「试回设备…结束无设备播放…重新开播 rc=0」", back ? bl : "没回", back && bl.Contains("结束无设备播放") && bl.Contains("重新开播 rc=0"));

            ulong p1 = session.GetPlayedSampleCount(), after = 0;
            var sw5 = Stopwatch.StartNew();
            while ((after = session.GetPlayedSampleCount() - p1) < 24000 && sw5.ElapsedMilliseconds < 10000) Thread.Sleep(20);
            Row("⑤ 回到设备后照常播", "10 秒内播完涨到 ≥ 24000 帧", $"{after} 帧（{sw5.ElapsedMilliseconds} ms）", after >= 24000);

            session.Dispose(); driver.Dispose();
            Console.SetError(err);
            return bad == 0 ? 0 : 1;
        }
    }
}
