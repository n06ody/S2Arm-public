// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using Ryujinx.Audio.Backends.Apple;   // 平台例外：苹果音频后端单测（真 AudioQueue），只在 macOS 跑
using Ryujinx.Audio.Backends.Common;
using Ryujinx.Audio.Common;
using Ryujinx.Audio.Integration;
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;

namespace S2A.Services.Tests
{
    // #1921：iOS 队列看门狗只凭交还账本判「时间轴跑前」，分不清零余量按时播，D-G8YT 40 秒重启 13 次（每次一次断音）。
    //   改为交还判据只起疑，队列自己的时钟越过交付量才算跑前（QueueTimelineWatch.PastSubmitted）。这条判法立在两个前提上，在本机真 AudioQueue 上核：
    //   一、正常播放时队列时钟（mSampleTime）不越过交给它的帧数；二、停队列（重启）后队列时间轴归零，交付计数也归零，两者重新对齐。
    //   期望手算：
    // ① 判定纯函数：时钟 1000 / 交付 1000 / 容差 480 → 不算跑前；1481 / 1000 → 跑前；500 / 1000 → 不算
    // ② 开播后（在途始终有数据）：时钟 − 交付量 = −（队列手上没播的帧）在 [−96000, 0]：不越过交付量；泵 32 槽最多交 1.6 秒 = 76800 帧，加设备延迟取 2 秒为界
    //   （11-21 首跑实测 −77549：在途满 32 槽）
    // ③ 钩子 S2A_AUDIO_TEST_STALL_AT=1（开播第 1 秒外部暂停 → 没回调先无设备播放、再试回设备重启），重启后再播 1.5 秒声音：仍在 [−96000, 0]。
    //   等法（11-22 门禁偶发红）：「再播 1.5 秒」按队列自己播完的帧数涨 72000 判，不按墙钟睡 1.5 秒——本机负载高时（门禁里各模式并行、同用一个声卡）
    //   重启后迟迟才重新开播，墙钟 1.5 秒里队列实际播得很少，读到的是调度。重启等 15 秒、再播等 15 秒为卡死上限
    //   若停队列后时间轴不归零，这时时钟 − 交付量 ≈ 重启前后已播的约 2.7 秒 − 满在途 1.6 秒 ≈ +52000 帧，判红
    //   负控：把 ② ③ 的判定换成「时钟 − 交付量 > 0」应全红（说明读数确实是负的在途，不是恒 0）——见行内 neg
    static class AppleAudioQueueClockTest
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

        [SoloTest("--apple-audio-queue-clock")]
        private static int Check()
        {
            int bad = 0;
            void Row(string name, string expect, string actual, bool ok)
            {
                bad += ok ? 0 : 1;
                Console.WriteLine($"{(ok ? "✓" : "✗")} 苹果音频队列时钟 {name}：{expect} → {actual}");
            }

            bool a = QueueTimelineWatch.PastSubmitted(1000, 1000, 480), b = QueueTimelineWatch.PastSubmitted(1481, 1000, 480), c = QueueTimelineWatch.PastSubmitted(500, 1000, 480);
            Row("① 判定纯函数", "否 / 是 / 否", $"{(a ? "是" : "否")} / {(b ? "是" : "否")} / {(c ? "是" : "否")}", !a && b && !c);

            if (!AppleHardwareDeviceDriver.IsSupported)   // 平台例外：苹果音频后端单测，只在 macOS 跑
            {
                Row("② ③ 真队列", "本机有苹果音频后端", "没有", false);
                return 1;
            }

            Environment.SetEnvironmentVariable("S2A_AUDIO_TEST_STALL_AT", "1");   // 会话类首次用到前设好（钩子是静态只读字段）
            var log = new StringWriter(); TextWriter err = Console.Error;
            Console.SetError(TextWriter.Synchronized(log));
            bool WaitLog(string s, int ms) { var sw = Stopwatch.StartNew(); while (sw.ElapsedMilliseconds < ms) { if (log.ToString().Contains(s)) return true; Thread.Sleep(20); } return log.ToString().Contains(s); }

            var reader = new ToneReader();
            var driver = new AppleHardwareDeviceDriver();   // 平台例外：苹果音频后端单测（真 AudioQueue），只在 macOS 跑
            IHardwareDeviceSession session = driver.OpenDeviceSession(IHardwareDeviceDriver.Direction.Output, null, SampleFormat.PcmInt16, 48000, 2);
            MethodInfo past = session.GetType().GetMethod("S2AQueuePastSubmitted", BindingFlags.Instance | BindingFlags.NonPublic);
            long Past() => past == null ? long.MinValue : (long)past.Invoke(session, null);
            session.Start();

            const ulong Bytes = 4800 * 4;   // 100 ms
            for (int i = 0; i < 200; i++)   // 20 秒声音：整个过程在途不缺数据
            {
                var buf = new AudioBuffer { DataPointer = 0x100000UL + (ulong)i * Bytes, DataSize = Bytes, BufferTag = (ulong)i, SampleReader = reader };
                session.RegisterBuffer(buf);
                session.QueueBuffer(buf);
            }

            // 开播后读一次：等队列真播起来（播完 ≥ 4800 帧 = 0.1 秒）、且读得到时钟（队列在跑；停着时 Past 回 long.MinValue）。
            //   原先墙钟睡 700 ms：调度慢时这一刻可能正落在钩子暂停 / 重启里，读到 long.MinValue 判红（11-22 后台 QoS 下复现）。15 秒为卡死上限
            long p2 = long.MinValue;
            var sw2 = Stopwatch.StartNew();
            while (sw2.ElapsedMilliseconds < 15000 && (session.GetPlayedSampleCount() < 4800 || (p2 = Past()) == long.MinValue)) Thread.Sleep(10);
            Row("② 开播后正常播", "时钟 − 交付量 在 [−96000, 0]", past == null ? "会话上没有 S2AQueuePastSubmitted" : $"{p2} 帧", p2 != long.MinValue && p2 <= 0 && p2 >= -96000);

            bool healed = WaitLog("没有回调", 15000) && WaitLog("试回设备", 5000);   // 11-23 起没回调先改走无设备播放（队列停着、读不到时钟），500 ms 后「试回设备」才是这次重启
            ulong q0 = session.GetPlayedSampleCount();
            var sw3 = Stopwatch.StartNew();
            while (healed && session.GetPlayedSampleCount() - q0 < 72000 && sw3.ElapsedMilliseconds < 15000) Thread.Sleep(20);
            long p3 = Past();
            Row("③ 重启（停队列）后再播 1.5 秒", "重启发生，时钟 − 交付量 在 [−96000, 0]（时间轴与交付计数一起归零）", healed ? $"重启了，{p3} 帧" : "没重启", healed && p3 != long.MinValue && p3 <= 0 && p3 >= -96000);
            bool neg = p2 > 0 || p3 > 0;
            Row("负控", "「时钟 − 交付量 > 0」不成立（读数是负的在途，不是恒 0）", neg ? "成立" : $"不成立（{p2} / {p3}）", !neg && p2 < 0 && p3 < 0);

            session.Dispose(); driver.Dispose();
            Console.SetError(err);
            return bad == 0 ? 0 : 1;
        }
    }
}
