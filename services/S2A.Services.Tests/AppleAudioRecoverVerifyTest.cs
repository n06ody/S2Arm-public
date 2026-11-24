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
    // #1632 / #1598：重启输出队列只看返回码、不确认设备真的重新播。玩家 M2 局重启后 10 秒回调 994 / 653 次（正常约 2000），回调没断，
    //   「设备 1 秒没回调」那条兜底接不住。真声卡（本机 AudioQueue），钩子 S2A_AUDIO_TEST_STALL_AT=1（开播后第 1 秒外部暂停 → 没回调兜底重启第 1 次）
    //   + S2A_AUDIO_TEST_SLOW_AFTER_HEAL=1（第 1 次重启后每交还 4 块只记 1 块播完：有回调、交还照常，只算播了约四分之一）。期望按语义手算，
    //   按事件判、不按墙钟（11-21：原先钩子靠看门狗拍点轮流暂停 / 开播，门禁机负载高时拍点漂移没压住，② 误判已恢复；等待上限只是兜底，不是判据）：
    // ① 出现第 1 次重启（没回调兜底：11-23 起先改走无设备播放，不在系统中断里 500 ms 后「试回设备」= 这次重启），重新开播 rc=0
    // ② 第 1 次重启后的核对：交还记成播完的不到应播的一半（约四分之一）→ 打「重启后设备没恢复播放…再重启」，重新开播 rc=0
    // ③ 钩子只管第 1 次之后，第 2 次重启后设备照常播；核对 → 打「重启后设备恢复播放…此前连续 1 次没恢复」
    // ④ 之后设备照常播：等到播完 24000 帧（0.5 秒声音）
    //   负控：同一单测在没有重启后核对的代码上跑（改前的 #1598 分支），② ③ 红
    static class AppleAudioRecoverVerifyTest
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

        [SoloTest("--apple-audio-recover-verify")]
        private static int Check()
        {
            if (!AppleHardwareDeviceDriver.IsSupported)   // 平台例外：苹果音频后端单测，只在 macOS 跑
            {
                Console.WriteLine("✗ 苹果音频重启后核对：本机没有可用的苹果音频后端");
                return 1;
            }

            Environment.SetEnvironmentVariable("S2A_AUDIO_TEST_STALL_AT", "1");   // 会话类首次用到前设好（钩子是静态只读字段）
            Environment.SetEnvironmentVariable("S2A_AUDIO_TEST_SLOW_AFTER_HEAL", "1");
            var log = new StringWriter(); TextWriter err = Console.Error;
            Console.SetError(TextWriter.Synchronized(log));

            int bad = 0;
            void Row(string name, string expect, string actual, bool ok)
            {
                bad += ok ? 0 : 1;
                Console.WriteLine($"{(ok ? "✓" : "✗")} 苹果音频重启后核对 {name}：{expect} → {actual}");
            }
            bool WaitLog(string s, int ms) { var sw = Stopwatch.StartNew(); while (sw.ElapsedMilliseconds < ms) { if (log.ToString().Contains(s)) return true; Thread.Sleep(20); } return log.ToString().Contains(s); }
            string Line(string s) { foreach (string l in log.ToString().Split('\n')) if (l.Contains(s)) return l.Trim(); return "—"; }

            var reader = new ToneReader();
            var driver = new AppleHardwareDeviceDriver();   // 平台例外：苹果音频后端单测（真 AudioQueue），只在 macOS 跑
            IHardwareDeviceSession session = driver.OpenDeviceSession(IHardwareDeviceDriver.Direction.Output, null, SampleFormat.PcmInt16, 48000, 2);
            session.Start();

            const ulong Bytes = 4800 * 4;   // 100 ms
            for (int i = 0; i < 200; i++)   // 20 秒声音：整个过程在途不缺数据
            {
                var b = new AudioBuffer { DataPointer = 0x100000UL + (ulong)i * Bytes, DataSize = Bytes, BufferTag = (ulong)i, SampleReader = reader };
                session.RegisterBuffer(b);
                session.QueueBuffer(b);
            }

            bool h1 = WaitLog("没有回调", 15000) && WaitLog("试回设备", 5000);
            Row("① 第 1 次重启（没回调兜底：无设备播放后试回设备）", "出现、重新开播 rc=0", h1 ? Line("试回设备") : "没重启", h1 && Line("试回设备").Contains("重新开播 rc=0"));

            bool h2 = WaitLog("重启后设备没恢复播放", 20000);
            string l2 = Line("重启后设备没恢复播放");
            Row("② 核出没恢复、再重启", "不到一半、重新开播 rc=0", h2 ? l2 : "没核出", h2 && l2.Contains("不到一半") && l2.Contains("重新开播 rc=0"));

            bool ok3 = WaitLog("重启后设备恢复播放", 30000);
            string l3 = Line("重启后设备恢复播放");
            Row("③ 核出已恢复", "此前连续 1 次没恢复", ok3 ? l3 : "没核出", ok3 && l3.Contains("此前连续 1 次没恢复"));

            ulong p0 = session.GetPlayedSampleCount(), played = 0;
            var sw4 = Stopwatch.StartNew();
            while ((played = session.GetPlayedSampleCount() - p0) < 24000 && sw4.ElapsedMilliseconds < 10000) Thread.Sleep(20);
            Row("④ 之后设备照常播", "播完 24000 帧（等事件，10 秒兜底）", $"{played} 帧，用时 {sw4.ElapsedMilliseconds} ms", played >= 24000);

            session.Dispose(); driver.Dispose();
            Console.SetError(err);
            return bad == 0 ? 0 : 1;
        }
    }
}
